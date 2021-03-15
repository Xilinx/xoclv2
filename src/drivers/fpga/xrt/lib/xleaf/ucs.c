// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA UCS Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include "metadata.h"
#include "xleaf.h"
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

#define XRT_UCS_CHANNEL1_REG			0
#define XRT_UCS_CHANNEL2_REG			8

#define CLK_MAX_VALUE			6400

static const struct regmap_config ucs_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x1000,
};

struct xrt_ucs {
	struct platform_device	*pdev;
	struct regmap		*regmap;
	struct mutex		ucs_lock; /* ucs dev lock */
};

static void xrt_ucs_event_cb(struct platform_device *pdev, void *arg)
{
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	struct platform_device *leaf;
	enum xrt_subdev_id id;
	int instance;

	id = evt->xe_subdev.xevt_subdev_id;
	instance = evt->xe_subdev.xevt_subdev_instance;

	if (e != XRT_EVENT_POST_CREATION) {
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

	xleaf_call(leaf, XRT_CLOCK_VERIFY, NULL);
	xleaf_put_leaf(pdev, leaf);
}

static int ucs_enable(struct xrt_ucs *ucs)
{
	int ret;

	mutex_lock(&ucs->ucs_lock);
	ret = regmap_write(ucs->regmap, XRT_UCS_CHANNEL2_REG, 1);
	mutex_unlock(&ucs->ucs_lock);

	return ret;
}

static int
xrt_ucs_leaf_call(struct platform_device *pdev, u32 cmd, void *arg)
{
	switch (cmd) {
	case XRT_XLEAF_EVENT:
		xrt_ucs_event_cb(pdev, arg);
		break;
	default:
		xrt_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return 0;
}

static int ucs_probe(struct platform_device *pdev)
{
	struct xrt_ucs *ucs = NULL;
	void __iomem *base = NULL;
	struct resource *res;

	ucs = devm_kzalloc(&pdev->dev, sizeof(*ucs), GFP_KERNEL);
	if (!ucs)
		return -ENOMEM;

	platform_set_drvdata(pdev, ucs);
	ucs->pdev = pdev;
	mutex_init(&ucs->ucs_lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ucs->regmap = devm_regmap_init_mmio(&pdev->dev, base, &ucs_regmap_config);
	if (IS_ERR(ucs->regmap)) {
		UCS_ERR(ucs, "map base %pR failed", res);
		return PTR_ERR(ucs->regmap);
	}
	ucs_enable(ucs);

	return 0;
}

static struct xrt_subdev_endpoints xrt_ucs_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .ep_name = XRT_MD_NODE_UCS_CONTROL_STATUS },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_ucs_data = {
	.xsd_dev_ops = {
		.xsd_leaf_call = xrt_ucs_leaf_call,
	},
};

static const struct platform_device_id xrt_ucs_table[] = {
	{ XRT_UCS, (kernel_ulong_t)&xrt_ucs_data },
	{ },
};

static struct platform_driver xrt_ucs_driver = {
	.driver = {
		.name = XRT_UCS,
	},
	.probe = ucs_probe,
	.id_table = xrt_ucs_table,
};

void ucs_leaf_init_fini(bool init)
{
	if (init)
		xleaf_register_driver(XRT_SUBDEV_UCS, &xrt_ucs_driver, xrt_ucs_endpoints);
	else
		xleaf_unregister_driver(XRT_SUBDEV_UCS);
}
