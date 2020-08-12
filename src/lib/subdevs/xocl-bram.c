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

#define XOCL_BRAM "xocl_bram"
#define XOCL_BRAM_MAX	64

struct xocl_bram {
	struct platform_device	*pdev;
	void		__iomem *base[XOCL_BRAM_MAX];
	resource_size_t		bar_off[XOCL_BRAM_MAX];
	int			bar_idx[XOCL_BRAM_MAX];
};

static bool xocl_bram_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	char			*ep_name = arg;
	struct xocl_bram	*bram;

	if (id != XOCL_SUBDEV_BRAM)
		return false;

	bram = platform_get_drvdata(pdev);

	return true;
}

static int xocl_bram_remove(struct platform_device *pdev)
{
	return 0;
}

static int xocl_bram_probe(struct platform_device *pdev)
{
	struct xocl_bram	*bram;
	int			i, ret = 0;
	struct resource		*res;

	bram = devm_kzalloc(&pdev->dev, sizeof(*bram), GFP_KERNEL);
	if (!bram)
		return -ENOMEM;

	bram->pdev = pdev;
	platform_set_drvdata(pdev, bram);

	for (i = 0, res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	    res;
	    res = platform_get_resource(pdev, IORESOURCE_MEM, ++i)) {


	return 0;
}

struct xocl_subdev_endpoints xocl_bram_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = NODE_BLP_ROM },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xocl_subdev_drvdata xocl_bram_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_bram_ioctl,
	},
};

static const struct platform_device_id xocl_bram_table[] = {
	{ XOCL_BRAM, (kernel_ulong_t)&xocl_bram_data },
	{ },
};

struct platform_driver xocl_axigate_driver = {
	.driver = {
		.name = XOCL_BRAM,
	},
	.probe = xocl_bram_probe,
	.remove = xocl_bram_remove,
	.id_table = xocl_bram_table,
};
