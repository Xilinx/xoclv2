// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA VSEC Driver
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

#define XOCL_ICAP "xocl_icap"

struct xocl_icap {
	struct platform_device	*pdev;
	void			*base;
};

struct xocl_subdev_endpoints xocl_icap_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = NODE_FPGA_CONFIG },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static int
xocl_icap_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	return 0;
}

static int xocl_icap_remove(struct platform_device *pdev)
{
	return 0;
}

static int xocl_icap_probe(struct platform_device *pdev)
{
	return 0;
}

struct xocl_subdev_drvdata xocl_icap_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_icap_leaf_ioctl,
	},
};

static const struct platform_device_id xocl_icap_table[] = {
	{ XOCL_ICAP, (kernel_ulong_t)&xocl_icap_data },
	{ },
};

struct platform_driver xocl_icap_driver = {
	.driver = {
		.name = XOCL_ICAP,
	},
	.probe = xocl_icap_probe,
	.remove = xocl_icap_remove,
	.id_table = xocl_icap_table,
};
