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
#include <linux/cdev.h>
#include <linux/idr.h>
#include <linux/platform_device.h>

#include "xocl-devices.h"

#define	XOCL_IPLIB_MODULE_NAME	        "xocl-iplib"
#define	XOCL_IPLIB_MODULE_VERSION	"4.0.0"

extern struct platform_driver xocl_rom_driver;
extern struct platform_driver xocl_xmc_driver;
extern struct platform_driver xocl_icap_driver;

static long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xocl_info(&pdev->dev, "Subdev %s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

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


static const struct platform_device_id sysmon_id_table[] = {
	{ "xocl-sysmon", (kernel_ulong_t)&srom_ops },
	{ },
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
	&xocl_xmc_driver,
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

int xocl_subdev_cdev_create(struct platform_device *pdev, struct cdev *chr_dev)
{
	int ret;
	struct xocl_subdev_ops *ops;
	const struct platform_device_id	*id = platform_get_device_id(pdev);

	if (!id || !id->driver_data)
		return -EOPNOTSUPP;
	ops = (struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->fops)
		return -EOPNOTSUPP;
	cdev_init(chr_dev, ops->fops);
	chr_dev->owner = ops->fops->owner;
	cdev_set_parent(chr_dev, &pdev->dev.kobj);
	ret = ida_simple_get(&ops->minor, 0, XOCL_MAX_DEVICES, GFP_KERNEL);
	if (ret < 0)
		goto out_get;
	ret = cdev_add(chr_dev, MKDEV(ops->dnum, ret), 1);
	if (ret)
		goto out_add;
	return 0;
out_add:
	ida_simple_remove(&ops->minor, MINOR(chr_dev->dev));
out_get:
	cdev_del(chr_dev);
	return ret;
}

int xocl_subdev_cdev_destroy(const struct platform_device *pdev, struct cdev *chr_dev)
{
	struct xocl_subdev_ops *ops;
	const struct platform_device_id	*id = platform_get_device_id(pdev);

	if (!id || !id->driver_data)
		return 0;
	ops = (struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->fops)
		return 0;

	ida_simple_remove(&ops->minor, MINOR(chr_dev->dev));
	cdev_del(chr_dev);
	return 0;
}

static int __init xocl_iplib_init(void)
{
	int i, j;
	struct xocl_subdev_ops *ops;
	int rc = platform_register_drivers(xocl_subdev_drivers, ARRAY_SIZE(xocl_subdev_drivers));
	if (rc)
		return rc;
	for (i = 0; i < ARRAY_SIZE(xocl_subdev_drivers); i++) {
		pr_info("Registering subdev driver[%d] %s...", i, xocl_subdev_drivers[i]->driver.name);
		ops = (struct xocl_subdev_ops *)xocl_subdev_drivers[i]->id_table[0].driver_data;
		if (!ops || !ops->fops)
			continue;
		rc = alloc_chrdev_region(&ops->dnum, 0, XOCL_MAX_DEVICES, xocl_subdev_drivers[i]->driver.name);
		if (rc)
			goto out_error;
		ida_init(&ops->minor);
	}
	return 0;
out_error:
	pr_info("Error registering subdev driver[%d] %s\n", i, xocl_subdev_drivers[i]->driver.name);
	for (j = i; j >= 0; j--) {
		ops = (struct xocl_subdev_ops *)xocl_subdev_drivers[j]->id_table[0].driver_data;
		if (!ops || !ops->fops)
			continue;
		ida_destroy(&ops->minor);
		unregister_chrdev_region(ops->dnum, XOCL_MAX_DEVICES);
	}
	platform_unregister_drivers(xocl_subdev_drivers, ARRAY_SIZE(xocl_subdev_drivers));
	return rc;
}

static void __exit xocl_iplib_exit(void)
{
	int i;
	struct xocl_subdev_ops *ops;

	for (i = 0; i < ARRAY_SIZE(xocl_subdev_drivers); i++) {
		ops = (struct xocl_subdev_ops *)xocl_subdev_drivers[i]->id_table[0].driver_data;
		pr_info("Unregistering subdev driver[%d] %s\n", i, xocl_subdev_drivers[i]->driver.name);
		if (!ops || !ops->fops)
			continue;
		ida_destroy(&ops->minor);
		unregister_chrdev_region(ops->dnum, XOCL_MAX_DEVICES);
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
