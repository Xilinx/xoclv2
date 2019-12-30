// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2019 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#include "alveo-drv.h"
#include "alveo-devices.h"

int myinit(struct platform_device *pdev, const struct xocl_subdev_info *detail)
{
	xmgmt_info(&pdev->dev, "%s init 0x%p\n", pdev->name, detail);
	return 0;
}

void myuinit(struct platform_device *pdev)
{
	xmgmt_info(&pdev->dev, "%s uinit\n", pdev->name);
}

long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xmgmt_info(&pdev->dev, "%s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

const static struct xmgmt_subdev_ops ops = {
	.init = myinit,
	.uinit = myuinit,
	.ioctl = myioctl,
};

static int xmgmt_rom_probe(struct platform_device *pdev)
{
	struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	int ret;

	xmgmt_info(dev, "Part 0x%p Dev 0x%p\n", info, dev);

	platform_set_drvdata(pdev, NULL);
	xmgmt_info(dev, "Alveo ROM probed\n");

	return 0;

eprobe_mgr_put:
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int xmgmt_rom_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver xmgmt_rom_driver = {
	.driver	= {
		.name    = "alveo-rom",
	},
	.probe   = xmgmt_rom_probe,
	.remove  = xmgmt_rom_remove,
};

static int __init xmgmt_iplib_init(void)
{
	return platform_driver_register(&xmgmt_rom_driver);
}

static void __exit xmgmt_iplib_exit(void)
{
	platform_driver_unregister(&xmgmt_rom_driver);
}

module_init(xmgmt_iplib_init);
module_exit(xmgmt_iplib_exit);

MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
