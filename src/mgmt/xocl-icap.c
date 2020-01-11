// SPDX-License-Identifier: GPL-2.0
/*
 * Platform driver for Xilinx acclerator feature ROM IP.
 * Bulk of the code borrowed from XRT driver file feature_rom.c
 *
 * Copyright (C) 2016-2019 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 *          chien-wei.lan@xilinx.com
 */

#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include "xocl-lib.h"
#include "xocl-features.h"

struct xocl_icap {
	struct xocl_subdev_base  core;
};

static long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xocl_info(&pdev->dev, "Subdev %s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

const static struct xocl_subdev_ops irom_ops = {
	.ioctl = myioctl,
	.id = XOCL_SUBDEV_ICAP,
};

static int xocl_icap_probe(struct platform_device *pdev)
{
	const struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct xocl_icap *rom = devm_kzalloc(&pdev->dev, sizeof(struct xocl_icap), GFP_KERNEL);
	if (!rom)
		return -ENOMEM;
	rom->core.pdev =  pdev;
	platform_set_drvdata(pdev, rom);

	xocl_info(&pdev->dev, "Probed subdev %s: resource %pr", pdev->name, res);
	return 0;
}

static int xocl_icap_remove(struct platform_device *pdev)
{
	struct xocl_xmc *icap = platform_get_drvdata(pdev);
	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, icap);
	xocl_info(&pdev->dev, "Removed subdev %s\n", pdev->name);
	return 0;
}


static const struct platform_device_id icap_id_table[] = {
	{ XOCL_ICAP, (kernel_ulong_t)&irom_ops },
	{ },
};


struct platform_driver xocl_icap_driver = {
	.driver	= {
		.name    = XOCL_ICAP,
	},
	.probe    = xocl_icap_probe,
	.remove   = xocl_icap_remove,
	.id_table = icap_id_table,
};
