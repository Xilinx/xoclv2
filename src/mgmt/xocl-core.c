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

#include "xocl-lib.h"

#define	XOCL_IPLIB_MODULE_NAME	        "xocl-lib"
#define	XOCL_IPLIB_MODULE_VERSION	"4.0.0"

extern struct platform_driver xocl_rom_driver;
extern struct platform_driver xocl_xmc_driver;
extern struct platform_driver xocl_icap_driver;
extern struct platform_driver xocl_region_driver;

struct class *xocl_class;

static struct platform_driver *xocl_subdev_drivers[] = {
	&xocl_region_driver,
	&xocl_rom_driver,
	&xocl_icap_driver,
	&xocl_xmc_driver,
};

long xocl_subdev_ioctl(struct xocl_subdev_base *subdev, unsigned int cmd, unsigned long arg)
{
	const struct xocl_subdev_ops *ops;
	struct platform_device *pdev = subdev->pdev;
	const struct platform_device_id	*id = platform_get_device_id(pdev);
	if (!id || !id->driver_data)
		return -EOPNOTSUPP;
	ops = (const struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->ioctl)
		return -EOPNOTSUPP;

	return ops->ioctl(pdev, cmd, arg);
}

int xocl_subdev_offline(struct xocl_subdev_base *subdev)
{
	const struct xocl_subdev_ops *ops;
	struct platform_device *pdev = subdev->pdev;
	const struct platform_device_id	*id = platform_get_device_id(pdev);
	if (!id || !id->driver_data)
		return -EOPNOTSUPP;
	ops = (const struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->offline)
		return -EOPNOTSUPP;

	return ops->offline(pdev);
}

int xocl_subdev_online(struct xocl_subdev_base *subdev)
{
	const struct xocl_subdev_ops *ops;
	struct platform_device *pdev = subdev->pdev;
	const struct platform_device_id	*id = platform_get_device_id(pdev);
	if (!id || !id->driver_data)
		return -EOPNOTSUPP;
	ops = (const struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->online)
		return -EOPNOTSUPP;

	return ops->online(pdev);
}

int xocl_subdev_cdev_create(struct xocl_subdev_base *subdev)
{
	int ret;
	struct xocl_subdev_ops *ops;
	dev_t mydevt;
	const struct platform_device_id	*id = platform_get_device_id(subdev->pdev);

	if (!id || !id->driver_data)
		return -EOPNOTSUPP;
	ops = (struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->fops)
		return -EOPNOTSUPP;

	cdev_init(&subdev->chr_dev, ops->fops);
	subdev->chr_dev.owner = THIS_MODULE;
	cdev_set_parent(&subdev->chr_dev, &subdev->pdev->dev.kobj);
	ret = ida_simple_get(&ops->minor, 0, XOCL_MAX_DEVICES, GFP_KERNEL);
	if (ret < 0)
		goto out_get;
	mydevt = MKDEV(MAJOR(ops->dnum), ret);
	subdev->sys_device = device_create(xocl_class,
					   &subdev->pdev->dev,
					   mydevt, NULL,
					   "%s%d", id->name, ret);
	if (IS_ERR(subdev->sys_device)) {
		ret = PTR_ERR(subdev->sys_device);
		goto out_device;
	}

	ret = cdev_add(&subdev->chr_dev, mydevt, 1);
	if (ret)
		goto out_add;
	xocl_info(&subdev->pdev->dev, "Created device node %s (%d %d)\n", dev_name(subdev->sys_device),
		  MAJOR(subdev->sys_device->devt), MINOR(subdev->sys_device->devt));
	return 0;

out_add:
	device_destroy(xocl_class, mydevt);
out_device:
	ida_simple_remove(&ops->minor, MINOR(subdev->chr_dev.dev));
out_get:
	return ret;
}

int xocl_subdev_cdev_destroy(struct xocl_subdev_base *subdev)
{
	struct xocl_subdev_ops *ops;
	const struct platform_device_id	*id = platform_get_device_id(subdev->pdev);

	if (!id || !id->driver_data)
		return 0;
	ops = (struct xocl_subdev_ops *)id->driver_data;
	if (!ops || !ops->fops)
		return 0;

	device_destroy(xocl_class, subdev->chr_dev.dev);
	cdev_del(&subdev->chr_dev);
	ida_simple_remove(&ops->minor, MINOR(subdev->chr_dev.dev));
	return 0;
}

static int __init xocl_iplib_init(void)
{
	int i, j, rc;
	struct xocl_subdev_ops *ops;
	xocl_class = class_create(THIS_MODULE, "xocl-lib");
	if (IS_ERR(xocl_class))
		return PTR_ERR(xocl_class);

	rc = platform_register_drivers(xocl_subdev_drivers, ARRAY_SIZE(xocl_subdev_drivers));
	if (rc)
		goto out_register;
	;
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
out_register:
	class_destroy(xocl_class);
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
	class_destroy(xocl_class);
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
