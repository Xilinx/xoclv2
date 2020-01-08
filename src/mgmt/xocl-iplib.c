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

#include "xocl-devices.h"

#define	XOCL_IPLIB_MODULE_NAME	        "xocl-iplib"
#define	XOCL_IPLIB_MODULE_VERSION	"4.0.0"

extern struct platform_driver xocl_rom_driver;

static long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xocl_info(&pdev->dev, "Subdev %s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

const static struct xocl_subdev_ops irom_ops = {
	.ioctl = myioctl,
	.id = XOCL_SUBDEV_ICAP,
};

const static struct xocl_subdev_ops srom_ops = {
	.ioctl = myioctl,
	.id = XOCL_SUBDEV_SYSMON,
};

static int xocl_rom_probe(struct platform_device *pdev)
{
	//struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	const struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	xocl_info(dev, "Probed subdev %s: resource %pr", pdev->name, res);
	return 0;
}

static int xocl_rom_remove(struct platform_device *pdev)
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

static const struct platform_device_id sysmon_id_table[] = {
	{ "xocl-sysmon", (kernel_ulong_t)&srom_ops },
	{ },
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

static struct platform_driver *xocl_subdev_drivers[] = {
	&xocl_rom_driver,
	&xocl_icap_driver,
	&xocl_sysmon_driver,
};

long xocl_subdev_ioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	const struct xocl_subdev_ops *ops;
	const struct platform_device_id	*id = platform_get_device_id(pdev);
	if (!id || !id->driver_data)
		return -EOPNOTSUPP;
	ops = (const struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->ioctl)
		return -EOPNOTSUPP;

	return ops->ioctl(pdev, cmd, arg);
}

int xocl_subdev_offline(struct platform_device *pdev)
{
	const struct xocl_subdev_ops *ops;
	const struct platform_device_id	*id = platform_get_device_id(pdev);
	if (!id || !id->driver_data)
		return -EOPNOTSUPP;
	ops = (const struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->offline)
		return -EOPNOTSUPP;

	return ops->offline(pdev);
}

int xocl_subdev_online(struct platform_device *pdev)
{
	const struct xocl_subdev_ops *ops;
	const struct platform_device_id	*id = platform_get_device_id(pdev);
	if (!id || !id->driver_data)
		return -EOPNOTSUPP;
	ops = (const struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->online)
		return -EOPNOTSUPP;

	return ops->online(pdev);
}

int xocl_subdev_cdev_create(const struct platform_device *pdev, struct cdev *chr_dev)
{
	const struct xocl_subdev_ops *ops;
	const struct platform_device_id	*id = platform_get_device_id(pdev);
	if (!id || !id->driver_data)
		return -EOPNOTSUPP;
	ops = (const struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->fops)
		return -EOPNOTSUPP;
	cdev_init(chr_dev, ops->fops);
	chr_dev->owner = owner;
	chr_dev->kobj.parent = pdev->dev.kobj;
	return cdev_add(chr_dev, pdev->dev.devt, 1);
}

int xocl_subdev_cdev_destroy(const struct platform_device *pdev, struct cdev *chr_dev)
{
	cdev_del(chr_dev);
}

static int __init xocl_iplib_init(void)
{
	int i, j;
	struct xocl_subdev_ops *ops;
	int rc = platform_register_drivers(xocl_subdev_drivers, ARRAY_SIZE(xocl_subdev_drivers));
	if (rc)
		return rc;
	for (i = 0; i < ARRAY_SIZE(xocl_subdev_drivers); i++) {
		ops = (struct xocl_subdev_ops *)xocl_subdev_drivers[i]->id_table[0].driver_data;
		if (!ops || ops->fops == 0)
			continue;
		rc = alloc_chrdev_region(&ops->dev, 0, XOCL_MAX_DEVICES, xocl_subdev_drivers[i]->driver.name);
		if (rc == 0)
			continue;
		goto out_error;
	}
	return 0;
out_error:
	for (j = 0; j < i; j++) {
		ops = (struct xocl_subdev_ops *)xocl_subdev_drivers[j]->id_table[0].driver_data;
		if (!ops || ops->fops == 0)
			continue;
		unregister_chrdev_region(ops->dev, XOCL_MAX_DEVICES);
	}
	platform_unregister_drivers(xocl_subdev_drivers, ARRAY_SIZE(xocl_subdev_drivers));
	return rc;
}

static void __exit xocl_iplib_exit(void)
{
	int i;
	const struct xocl_subdev_ops *ops;

	for (i = 0; i < ARRAY_SIZE(xocl_subdev_drivers); i++) {
		ops = (const struct xocl_subdev_ops *)xocl_subdev_drivers[i]->id_table[0].driver_data;
		if (!ops || ops->dev == 0)
			continue;
		unregister_chrdev_region(ops->dev, XOCL_MAX_DEVICES);
	}
	platform_unregister_drivers(xocl_subdev_drivers, ARRAY_SIZE(xocl_subdev_drivers));
}

module_init(xocl_iplib_init);
module_exit(xocl_iplib_exit);

EXPORT_SYMBOL_GPL(xocl_subdev_ioctl);
EXPORT_SYMBOL_GPL(xocl_subdev_offline);
EXPORT_SYMBOL_GPL(xocl_subdev_online);

MODULE_VERSION(XOCL_IPLIB_MODULE_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
