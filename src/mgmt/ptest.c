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
#include <linux/cdev.h>
#include <linux/platform_device.h>

struct ptest_subdev_ops {
	int (*init)(struct platform_device *pdev, void *detail);
	void (*uinit)(struct platform_device *pdev);
	long (*ioctl)(struct platform_device *pdev, unsigned int cmd, unsigned long arg);
};

static int myinit(struct platform_device *pdev, void *detail)
{
	dev_info(&pdev->dev, "%s init 0x%p\n", pdev->name, detail);
	return 0;
}

static void myuinit(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "%s uinit\n", pdev->name);
}

static long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	dev_info(&pdev->dev, "%s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

const static struct ptest_subdev_ops ops = {
	.init = myinit,
	.uinit = myuinit,
	.ioctl = myioctl,
};

static int xmgmt_rom_probe(struct platform_device *pdev)
{
	void *info = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	int ret;

	dev_info(dev, "Part 0x%p Dev 0x%p\n", info, dev);

	platform_set_drvdata(pdev, NULL);
	dev_info(dev, "Probed %s\n", pdev->name);

	return 0;

eprobe_mgr_put:
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int xmgmt_rom_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	dev_info(dev, "Removed %s\n", pdev->name);
	return 0;
}

static dev_t			devp;
static dev_t			devq;
static const struct platform_device_id rom_id_table[] = {
	{ "my-ptest", 0 },
	{ },
};

static const struct platform_device_id icap_id_table[] = {
	{ "my-qtest", 0 },
	{ },
};

static struct platform_driver xmgmt_rom_driver = {
	.driver	= {
		.name    = "my-ptest",
	},
	.probe    = xmgmt_rom_probe,
	.remove   = xmgmt_rom_remove,
	.id_table = rom_id_table,
};


static struct platform_driver xmgmt_icap_driver = {
	.driver	= {
		.name    = "my-qtest",
	},
	.probe    = xmgmt_rom_probe,
	.remove   = xmgmt_rom_remove,
	.id_table = icap_id_table,
};

static struct platform_driver *xocl_subdev_drivers[] = {
	&xmgmt_rom_driver,
	&xmgmt_icap_driver,
};

static int __init xmgmt_iplib_init(void)
{
	int rc = platform_register_drivers(xocl_subdev_drivers, ARRAY_SIZE(xocl_subdev_drivers));
	printk("Registered p/q drivers %d\n", rc);
	rc = alloc_chrdev_region(&devp, 0, 16, xmgmt_rom_driver.driver.name);
	rc = alloc_chrdev_region(&devq, 0, 16, xmgmt_icap_driver.driver.name);
	return rc;
}

static void __exit xmgmt_iplib_exit(void)
{
	unregister_chrdev_region(devp, 16);
	unregister_chrdev_region(devq, 16);
	platform_unregister_drivers(xocl_subdev_drivers, ARRAY_SIZE(xocl_subdev_drivers));
	printk("Unregistered p/q drivers\n");
}

module_init(xmgmt_iplib_init);
module_exit(xmgmt_iplib_exit);

MODULE_VERSION("11.1");
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Plat Test driver");
MODULE_LICENSE("GPL v2");
