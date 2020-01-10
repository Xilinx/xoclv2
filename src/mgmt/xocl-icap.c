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
	//struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	const struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	const struct xocl_dev_core *core = xocl_get_xdev(pdev);

	xocl_info(dev, "Probed subdev %s: resource %pr", pdev->name, res);
	xocl_info(dev, "xocl_core 0x%px\n", core);
	xocl_info(dev, "VBNV %s: ", core->from.header.VBNVName);
	return 0;
}

static int xocl_icap_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	//struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	platform_set_drvdata(pdev, NULL);
	xocl_info(dev, "Removed subdev %s\n", pdev->name);
	return 0;
}


static const struct platform_device_id icap_id_table[] = {
	{ "xocl-icap", (kernel_ulong_t)&irom_ops },
	{ },
};


struct platform_driver xocl_icap_driver = {
	.driver	= {
		.name    = "xocl-icap",
	},
	.probe    = xocl_icap_probe,
	.remove   = xocl_icap_remove,
	.id_table = icap_id_table,
};
