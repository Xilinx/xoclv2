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

static int myinit(struct platform_device *pdev, const struct xocl_subdev_info *detail)
{
	xmgmt_info(&pdev->dev, "%s init 0x%p\n", pdev->name, detail);
	return 0;
}

static void myuinit(struct platform_device *pdev)
{
	xmgmt_info(&pdev->dev, "%s uinit\n", pdev->name);
}

static long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
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

	xmgmt_info(dev, "Probed %s/%s: Info 0x%px Subdev 0x%px\n", info->name, pdev->name,
		   info, pdev);

	platform_set_drvdata(pdev, NULL);
	return 0;

eprobe_mgr_put:
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int xmgmt_rom_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	xmgmt_info(dev, "Removed %s/%s\n", info->name, pdev->name);
	return 0;
}

static const struct platform_device_id rom_id_table[] = {
	{ "alveo-rom", 0 },
	{ },
};

static const struct platform_device_id icap_id_table[] = {
	{ "alveo-icap", 0 },
	{ },
};

static struct platform_driver xmgmt_rom_driver = {
	.driver	= {
		.name    = "alveo-rom",
	},
	.probe    = xmgmt_rom_probe,
	.remove   = xmgmt_rom_remove,
	.id_table = rom_id_table,
};

static struct platform_driver xmgmt_icap_driver = {
	.driver	= {
		.name    = "alveo-icap",
	},
	.probe    = xmgmt_rom_probe,
	.remove   = xmgmt_rom_remove,
	.id_table = icap_id_table,
};

static int __init xmgmt_iplib_init(void)
{
	int rc = platform_driver_register(&xmgmt_rom_driver);
	if (rc)
		return rc;
	rc = platform_driver_register(&xmgmt_icap_driver);
	if (rc)
		platform_driver_unregister(&xmgmt_rom_driver);
	return rc;
}

static void __exit xmgmt_iplib_exit(void)
{
	platform_driver_unregister(&xmgmt_icap_driver);
	platform_driver_unregister(&xmgmt_rom_driver);
}

module_init(xmgmt_iplib_init);
module_exit(xmgmt_iplib_exit);

MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
