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

#define XOCL_AXIGATE "xocl_axigate"

struct xocl_axigate {
	struct platform_device	*pdev;
	void			*base;
};

struct xocl_subdev_endpoints xocl_axigate_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = NODE_GATE_PLP },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = NODE_GATE_ULP },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static int xocl_axigate_remove(struct platform_device *pdev)
{
	return 0;
}

static int xocl_axigate_probe(struct platform_device *pdev)
{
	return 0;
}

static const struct platform_device_id xocl_axigate_table[] = {
	{ XOCL_AXIGATE, 0 },
	{ },
};

struct platform_driver xocl_axigate_driver = {
	.driver = {
		.name = XOCL_AXIGATE,
	},
	.probe = xocl_axigate_probe,
	.remove = xocl_axigate_remove,
	.id_table = xocl_axigate_table,
};
