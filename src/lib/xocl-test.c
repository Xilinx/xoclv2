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
#include <linux/delay.h>
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

	xocl_info(pdev, "probing...");

	xt = devm_kzalloc(&pdev->dev, sizeof(*xt), GFP_KERNEL);
	if (!xt) {
		xocl_err(pdev, "failed to alloc xocl_test");
		return -ENOMEM;
	}
	xt->pdev = pdev;
	platform_set_drvdata(pdev, xt);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&pdev->dev.kobj, &xocl_test_attrgroup))
		xocl_err(pdev, "failed to create sysfs group");

	/* Ready to handle req thru cdev. */
	xocl_devnode_allowed(pdev);

	/* After we return here, we'll get inter-leaf calls. */
	return 0;
}

static int xocl_test_remove(struct platform_device *pdev)
{
	int ret;

	/* By now, partition driver should prevent any inter-leaf call. */

	xocl_info(pdev, "leaving...");

	ret = xocl_devnode_disallowed(pdev);
	if (ret)
		return ret;
	/* By now, no more access thru cdev. */

	(void) sysfs_remove_group(&pdev->dev.kobj, &xocl_test_attrgroup);
	/* By now, no more access thru sysfs nodes. */

	/* Clean up can safely be done now. */
	return 0;
}

static long xocl_test_leaf_ioctl(struct platform_device *pdev, u32 cmd, u64 arg)
{
	xocl_info(pdev, "handling IOCTL cmd: %d", cmd);
	return 0;
}

static int xocl_test_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xocl_devnode_open(inode);

	/* Device may have gone already when we get here. */
	if (!pdev)
		return -ENODEV;

	xocl_info(pdev, "opened");
	file->private_data = platform_get_drvdata(pdev);
	return 0;
}

static ssize_t
xocl_test_read(struct file *file, char __user *ubuf, size_t n, loff_t *off)
{
	int i;
	struct xocl_test *xt = file->private_data;

	for (i = 0; i < 10; i++) {
		xocl_info(xt->pdev, "reading...");
		ssleep(1);
	}
	return 0;
}

static int xocl_test_close(struct inode *inode, struct file *file)
{
	struct xocl_test *xt = file->private_data;

	xocl_devnode_close(inode);

	xocl_info(xt->pdev, "closed");
	return 0;
}

struct xocl_subdev_data xocl_test_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_test_leaf_ioctl,
	},
	.xsd_file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = xocl_test_open,
			.release = xocl_test_close,
			.read = xocl_test_read,
		},
		.xsf_dev_name = "test",
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
