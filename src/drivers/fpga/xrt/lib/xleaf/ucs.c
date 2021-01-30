// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA UCS Driver
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include "metadata.h"
#include "xleaf.h"
#include "xleaf/ucs.h"
#include "xleaf/clock.h"

#define UCS_ERR(ucs, fmt, arg...)   \
	xrt_err((ucs)->pdev, fmt "\n", ##arg)
#define UCS_WARN(ucs, fmt, arg...)  \
	xrt_warn((ucs)->pdev, fmt "\n", ##arg)
#define UCS_INFO(ucs, fmt, arg...)  \
	xrt_info((ucs)->pdev, fmt "\n", ##arg)
#define UCS_DBG(ucs, fmt, arg...)   \
	xrt_dbg((ucs)->pdev, fmt "\n", ##arg)

#define XRT_UCS		"xrt_ucs"

#define CHANNEL1_OFFSET			0
#define CHANNEL2_OFFSET			8

#define CLK_MAX_VALUE			6400

struct ucs_control_status_ch1 {
	unsigned int shutdown_clocks_latched:1;
	unsigned int reserved1:15;
	unsigned int clock_throttling_average:14;
	unsigned int reserved2:2;
};

struct xrt_ucs {
	struct platform_device	*pdev;
	void __iomem		*ucs_base;
	struct mutex		ucs_lock; /* ucs dev lock */
};

static inline u32 reg_rd(struct xrt_ucs *ucs, u32 offset)
{
	return ioread32(ucs->ucs_base + offset);
}

static inline void reg_wr(struct xrt_ucs *ucs, u32 val, u32 offset)
{
	iowrite32(val, ucs->ucs_base + offset);
}

static void xrt_ucs_event_cb(struct platform_device *pdev, void *arg)
{
	struct platform_device	*leaf;
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id = evt->xe_subdev.xevt_subdev_id;
	int instance = evt->xe_subdev.xevt_subdev_instance;

	switch (e) {
	case XRT_EVENT_POST_CREATION:
		break;
	default:
		xrt_dbg(pdev, "ignored event %d", e);
		return;
	}

	if (id != XRT_SUBDEV_CLOCK)
		return;

	leaf = xleaf_get_leaf_by_id(pdev, XRT_SUBDEV_CLOCK, instance);
	if (!leaf) {
		xrt_err(pdev, "does not get clock subdev");
		return;
	}

	xleaf_ioctl(leaf, XRT_CLOCK_VERIFY, NULL);
	xleaf_put_leaf(pdev, leaf);
}

static void ucs_check(struct xrt_ucs *ucs, bool *latched)
{
	struct ucs_control_status_ch1 *ucs_status_ch1;
	u32 status;

	mutex_lock(&ucs->ucs_lock);
	status = reg_rd(ucs, CHANNEL1_OFFSET);
	ucs_status_ch1 = (struct ucs_control_status_ch1 *)&status;
	if (ucs_status_ch1->shutdown_clocks_latched) {
		UCS_ERR(ucs,
			"Critical temperature or power event, kernel clocks have been stopped.");
		UCS_ERR(ucs,
			"run 'xbutil valiate -q' to continue. See AR 73398 for more details.");
		/* explicitly indicate reset should be latched */
		*latched = true;
	} else if (ucs_status_ch1->clock_throttling_average >
	    CLK_MAX_VALUE) {
		UCS_ERR(ucs, "kernel clocks %d exceeds expected maximum value %d.",
			ucs_status_ch1->clock_throttling_average,
			CLK_MAX_VALUE);
	} else if (ucs_status_ch1->clock_throttling_average) {
		UCS_ERR(ucs, "kernel clocks throttled at %d%%.",
			(ucs_status_ch1->clock_throttling_average /
			 (CLK_MAX_VALUE / 100)));
	}
	mutex_unlock(&ucs->ucs_lock);
}

static void ucs_enable(struct xrt_ucs *ucs)
{
	reg_wr(ucs, 1, CHANNEL2_OFFSET);
}

static int
xrt_ucs_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xrt_ucs		*ucs;
	int			ret = 0;

	ucs = platform_get_drvdata(pdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		xrt_ucs_event_cb(pdev, arg);
		break;
	case XRT_UCS_CHECK: {
		ucs_check(ucs, (bool *)arg);
		break;
	}
	case XRT_UCS_ENABLE:
		ucs_enable(ucs);
		break;
	default:
		xrt_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int ucs_remove(struct platform_device *pdev)
{
	struct xrt_ucs *ucs;

	ucs = platform_get_drvdata(pdev);
	if (!ucs) {
		xrt_err(pdev, "driver data is NULL");
		return -EINVAL;
	}

	if (ucs->ucs_base)
		iounmap(ucs->ucs_base);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, ucs);

	return 0;
}

static int ucs_probe(struct platform_device *pdev)
{
	struct xrt_ucs *ucs = NULL;
	struct resource *res;
	int ret;

	ucs = devm_kzalloc(&pdev->dev, sizeof(*ucs), GFP_KERNEL);
	if (!ucs)
		return -ENOMEM;

	platform_set_drvdata(pdev, ucs);
	ucs->pdev = pdev;
	mutex_init(&ucs->ucs_lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ucs->ucs_base = ioremap(res->start, res->end - res->start + 1);
	if (!ucs->ucs_base) {
		UCS_ERR(ucs, "map base %pR failed", res);
		ret = -EFAULT;
		goto failed;
	}
	ucs_enable(ucs);

	return 0;

failed:
	ucs_remove(pdev);
	return ret;
}

struct xrt_subdev_endpoints xrt_ucs_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .ep_name = NODE_UCS_CONTROL_STATUS },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_ucs_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_ucs_leaf_ioctl,
	},
};

static const struct platform_device_id xrt_ucs_table[] = {
	{ XRT_UCS, (kernel_ulong_t)&xrt_ucs_data },
	{ },
};

struct platform_driver xrt_ucs_driver = {
	.driver = {
		.name = XRT_UCS,
	},
	.probe = ucs_probe,
	.remove = ucs_remove,
	.id_table = xrt_ucs_table,
};
