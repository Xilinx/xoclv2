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
#include <linux/export.h>
#include <linux/platform_device.h>

#include "alveo-drv.h"
#include "alveo-devices.h"

#define	XOCL_IPLIB_MODULE_NAME	"xocl-iplib"

static long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xmgmt_info(&pdev->dev, "%s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

const static struct xocl_subdev_ops rom_ops = {
	.ioctl = myioctl,
};

static int xocl_rom_probe(struct platform_device *pdev)
{
	struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	int ret;

	xmgmt_info(dev, "Probed %s/%s: Info 0x%px Subdev 0x%px\n", info->name, pdev->name,
		   info, pdev);

	platform_set_drvdata(pdev, (void *)&rom_ops);
	return 0;

eprobe_mgr_put:
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int xocl_rom_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	platform_set_drvdata(pdev, NULL);
	xmgmt_info(dev, "Removed %s/%s\n", info->name, pdev->name);
	return 0;
}

static const struct platform_device_id rom_id_table[] = {
	{ "xocl-rom", 0 },
	{ },
};

static const struct platform_device_id icap_id_table[] = {
	{ "xocl-icap", 0 },
	{ },
};

static const struct platform_device_id sysmon_id_table[] = {
	{ "xocl-sysmon", 0 },
	{ },
};

static struct platform_driver xocl_rom_driver = {
	.driver	= {
		.name    = "xocl-rom",
	},
	.probe    = xocl_rom_probe,
	.remove   = xocl_rom_remove,
	.id_table = rom_id_table,
};

static struct platform_driver xocl_icap_driver = {
	.driver	= {
		.name    = "xocl-icap",
	},
	.probe    = xocl_rom_probe,
	.remove   = xocl_rom_remove,
	.id_table = icap_id_table,
};

static struct platform_driver xocl_sysmon_driver = {
	.driver	= {
		.name    = "xocl-sysmon",
	},
	.probe    = xocl_rom_probe,
	.remove   = xocl_rom_remove,
	.id_table = sysmon_id_table,
};

static struct platform_driver xocl_subdev_drivers[] = {
	{
		.driver	= {
			.name    = "xocl-rom",
		},
		.probe    = xocl_rom_probe,
		.remove   = xocl_rom_remove,
		.id_table = rom_id_table,
	},
	{
		.driver	= {
			.name    = "xocl-icap",
		},
		.probe    = xocl_rom_probe,
		.remove   = xocl_rom_remove,
		.id_table = icap_id_table,
	},
	{
		.driver	= {
			.name    = "xocl-sysmon",
		},
		.probe    = xocl_rom_probe,
		.remove   = xocl_rom_remove,
		.id_table = sysmon_id_table,
	},

};

long xocl_subdev_ioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	const struct xocl_subdev_ops *ops = platform_get_drvdata(pdev);
	if (!ops || !ops->ioctl)
		return -EOPNOTSUPP;

	return ops->ioctl(pdev, cmd, arg);
}

static int __init xocl_iplib_init(void)
{
	int i = 0;
	int j = 0;
	int rc = 0;
	for (i = 0; i < ARRAY_SIZE(xocl_subdev_drivers); i++) {
		pr_info(XOCL_IPLIB_MODULE_NAME " Registering subdev driver[%d] %s\n", i,
			xocl_subdev_drivers[i].driver.name);
		rc = platform_driver_register(&xocl_subdev_drivers[i]);
		if (!rc)
			continue;
		/* Registration of driver index 'i' failed; unregister all and return error code */
		for (j = 0; j < i; j++)
			platform_driver_unregister(&xocl_subdev_drivers[j]);
		break;
	}
	return rc;
}

static void __exit xocl_iplib_exit(void)
{
	int i = 0;
	for (i = 0; i < ARRAY_SIZE(xocl_subdev_drivers); i++)
		platform_driver_unregister(&xocl_subdev_drivers[i]);
}

module_init(xocl_iplib_init);
module_exit(xocl_iplib_exit);

EXPORT_SYMBOL_GPL(xocl_subdev_ioctl);

MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
