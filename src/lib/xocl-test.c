// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Test Leaf Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include "xocl-subdev.h"

#define	XOCL_TEST "xocl_test"

struct xocl_test {
	struct platform_device *pdev;
};

static bool xocl_test_leaf_match(struct xocl_subdev *sdev, u64 arg)
{
	int myid = arg;

	return sdev->xs_pdev->id != myid;
}

static ssize_t test_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	xocl_subdev_leaf_handle_t leaf = xocl_subdev_get_leaf(pdev,
		XOCL_SUBDEV_TEST, xocl_test_leaf_match, pdev->id);
	if (leaf)
		(void) xocl_subdev_ioctl(leaf, 1, 0);
	return 0;
}

static ssize_t test_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	/* Place holder for now. */
	return count;
}

static DEVICE_ATTR_RW(test);

static struct attribute *xocl_test_attrs[] = {
	&dev_attr_test.attr,
	NULL,
};

static const struct attribute_group xocl_test_attrgroup = {
	.attrs = xocl_test_attrs,
};

static int xocl_test_probe(struct platform_device *pdev)
{
	struct xocl_test *xt;

	xocl_info(&pdev->dev, "probing...");

	xt = devm_kzalloc(&pdev->dev, sizeof(*xt), GFP_KERNEL);
	if (!xt) {
		xocl_err(&pdev->dev, "failed to alloc xocl_test");
		return -ENOMEM;
	}
	xt->pdev = pdev;
	platform_set_drvdata(pdev, xt);
	if (sysfs_create_group(&pdev->dev.kobj, &xocl_test_attrgroup))
		xocl_err(&pdev->dev, "failed to create sysfs group");
	return 0;
}

static int xocl_test_remove(struct platform_device *pdev)
{

	xocl_info(&pdev->dev, "leaving...");
	(void) sysfs_remove_group(&pdev->dev.kobj, &xocl_test_attrgroup);
	return 0;
}

static long xocl_test_ioctl(struct platform_device *pdev, u32 cmd, u64 arg)
{
	xocl_info(&pdev->dev, "handling IOCTL cmd: %d", cmd);
	return 0;
}

struct xocl_subdev_data xocl_test_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_test_ioctl,
	},
};

static const struct platform_device_id xocl_test_id_table[] = {
	{ XOCL_TEST, (kernel_ulong_t)&xocl_test_data },
	{ },
};

struct platform_driver xocl_test_driver = {
	.driver	= {
		.name    = XOCL_TEST,
	},
	.probe   = xocl_test_probe,
	.remove  = xocl_test_remove,
	.id_table = xocl_test_id_table,
};
