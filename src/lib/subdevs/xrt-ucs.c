// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA UCS Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include "xocl-metadata.h"
#include "xocl-subdev.h"
#include "xocl-parent.h"
#include "xocl-ucs.h"
#include "xocl-clock.h"

#define UCS_ERR(ucs, fmt, arg...)   \
	xocl_err((ucs)->pdev, fmt "\n", ##arg)
#define UCS_WARN(ucs, fmt, arg...)  \
	xocl_warn((ucs)->pdev, fmt "\n", ##arg)
#define UCS_INFO(ucs, fmt, arg...)  \
	xocl_info((ucs)->pdev, fmt "\n", ##arg)
#define UCS_DBG(ucs, fmt, arg...)   \
	xocl_dbg((ucs)->pdev, fmt "\n", ##arg)


#define XOCL_UCS		"xocl_ucs"

#define CHANNEL1_OFFSET			0
#define CHANNEL2_OFFSET			8

#define CLK_MAX_VALUE			6400

struct ucs_control_status_ch1 {
	unsigned int shutdown_clocks_latched:1;
	unsigned int reserved1:15;
	unsigned int clock_throttling_average:14;
	unsigned int reserved2:2;
};


struct xocl_ucs {
	struct platform_device	*pdev;
	void __iomem		*ucs_base;
	struct mutex		ucs_lock;
	void			*evt_hdl;
};

static inline u32 reg_rd(struct xocl_ucs *ucs, u32 offset)
{
	return ioread32(ucs->ucs_base + offset);
}

static inline void reg_wr(struct xocl_ucs *ucs, u32 val, u32 offset)
{
	iowrite32(val, ucs->ucs_base + offset);
}

static bool xocl_ucs_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	if (id == XOCL_SUBDEV_CLOCK)
		return true;

	return false;
}

static int xocl_ucs_event_cb(struct platform_device *pdev,
	enum xocl_events evt, void *arg)
{

	struct xocl_ucs		*ucs;
	struct platform_device	*leaf;
	struct xocl_event_arg_subdev *esd = (struct xocl_event_arg_subdev *)arg;

	ucs = platform_get_drvdata(pdev);

	switch (evt) {
	case XOCL_EVENT_POST_CREATION:
		break;
	default:
		xocl_info(pdev, "ignored event %d", evt);
		return XOCL_EVENT_CB_CONTINUE;
	}

	leaf = xocl_subdev_get_leaf_by_id(pdev,
		XOCL_SUBDEV_CLOCK, esd->xevt_subdev_instance);
	BUG_ON(!leaf);
	xocl_subdev_ioctl(leaf, XOCL_CLOCK_VERIFY, NULL);
	xocl_subdev_put_leaf(pdev, leaf);

	return XOCL_EVENT_CB_CONTINUE;
}

static void ucs_check(struct xocl_ucs *ucs, bool *latched)
{
	struct ucs_control_status_ch1 *ucs_status_ch1;
	u32 status;

	mutex_lock(&ucs->ucs_lock);
	status = reg_rd(ucs, CHANNEL1_OFFSET);
	ucs_status_ch1 = (struct ucs_control_status_ch1 *)&status;
	if (ucs_status_ch1->shutdown_clocks_latched) {
		UCS_ERR(ucs, "Critical temperature or power event, kernel clocks have been stopped, run 'xbutil valiate -q' to continue. See AR 73398 for more details.");
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

static void ucs_enable(struct xocl_ucs *ucs)
{
	reg_wr(ucs, 1, CHANNEL2_OFFSET);
}

static int
xocl_ucs_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xocl_ucs		*ucs;
	int			ret = 0;

	ucs = platform_get_drvdata(pdev);

	switch (cmd) {
	case XOCL_UCS_CHECK: {
		ucs_check(ucs, (bool *)arg);
		break;
	}
	case XOCL_UCS_ENABLE:
		ucs_enable(ucs);
		break;
	default:
		xocl_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int ucs_remove(struct platform_device *pdev)
{
	struct xocl_ucs *ucs;

	ucs = platform_get_drvdata(pdev);
	if (!ucs) {
		xocl_err(pdev, "driver data is NULL");
		return -EINVAL;
	}

	xocl_subdev_remove_event_cb(pdev, ucs->evt_hdl);
	if (ucs->ucs_base)
		iounmap(ucs->ucs_base);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, ucs);

	return 0;
}



static int ucs_probe(struct platform_device *pdev)
{
	struct xocl_ucs *ucs = NULL;
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
	ucs->evt_hdl = xocl_subdev_add_event_cb(pdev, xocl_ucs_leaf_match,
		NULL, xocl_ucs_event_cb);

	return 0;

failed:
	ucs_remove(pdev);
	return ret;
}


struct xocl_subdev_endpoints xocl_ucs_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = NODE_UCS_CONTROL_STATUS },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xocl_subdev_drvdata xocl_ucs_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_ucs_leaf_ioctl,
	},
};

static const struct platform_device_id xocl_ucs_table[] = {
	{ XOCL_UCS, (kernel_ulong_t)&xocl_ucs_data },
	{ },
};

struct platform_driver xocl_ucs_driver = {
	.driver = {
		.name = XOCL_UCS,
	},
	.probe = ucs_probe,
	.remove = ucs_remove,
	.id_table = xocl_ucs_table,
};
