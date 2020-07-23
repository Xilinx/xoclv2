// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Test Leaf Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/delay.h>
#include "xocl-subdev.h"

#define	XOCL_TEST "xocl_test"

struct xocl_test {
	struct platform_device *pdev;
	struct platform_device *leaf;
	xocl_event_cb_handle_t evt_hdl;
};

static bool xocl_test_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	int myid = (int)(uintptr_t)arg;
	return id == XOCL_SUBDEV_TEST && pdev->id != myid;
}

static ssize_t hold_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct xocl_test *xt = platform_get_drvdata(pdev);
	struct platform_device *leaf;

	leaf = xocl_subdev_get_leaf(pdev, xocl_test_leaf_match,
		(void *)(uintptr_t)pdev->id);
	if (leaf)
		xt->leaf = leaf;
	return count;
}
static DEVICE_ATTR_WO(hold);

static ssize_t release_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct xocl_test *xt = platform_get_drvdata(pdev);

	if (xt->leaf)
		(void) xocl_subdev_put_leaf(pdev, xt->leaf);
	return count;
}
static DEVICE_ATTR_WO(release);

static struct attribute *xocl_test_attrs[] = {
	&dev_attr_hold.attr,
	&dev_attr_release.attr,
	NULL,
};

static const struct attribute_group xocl_test_attrgroup = {
	.attrs = xocl_test_attrs,
};

static int xocl_test_event_cb(struct platform_device *pdev,
	enum xocl_subdev_id id, int instance, enum xocl_events evt)
{
	struct platform_device *leaf;

	xocl_info(pdev, "event %d for (%d, %d)", evt, id, instance);

	switch (evt) {
	case XOCL_EVENT_POST_CREATION:
		break;
	default:
		return 0;
	}
	
	leaf = xocl_subdev_get_leaf_by_id(pdev, id, instance);
	if (leaf) {
		(void) xocl_subdev_ioctl(leaf, 1, NULL);
		(void) xocl_subdev_put_leaf(pdev, leaf);
	}
	return 0;
}

static int xocl_test_probe(struct platform_device *pdev)
{
	struct xocl_test *xt;

	xocl_info(pdev, "probing...");

	xt = devm_kzalloc(DEV(pdev), sizeof(*xt), GFP_KERNEL);
	if (!xt) {
		xocl_err(pdev, "failed to alloc xocl_test");
		return -ENOMEM;
	}
	xt->pdev = pdev;
	platform_set_drvdata(pdev, xt);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(pdev)->kobj, &xocl_test_attrgroup))
		xocl_err(pdev, "failed to create sysfs group");

	/* Ready to handle req thru cdev. */
	(void) xocl_devnode_create(pdev, "test");

	/* Add event callback to wait for the peer instance. */
	xt->evt_hdl = xocl_subdev_add_event_cb(pdev, xocl_test_leaf_match,
		(void *)(uintptr_t)pdev->id, xocl_test_event_cb);

	/* Trigger partition creation. */
	(void) xocl_subdev_create_partition(pdev, XOCL_PART_TEST_1, NULL);

	/* After we return here, we'll get inter-leaf calls. */
	return 0;
}

static int xocl_test_remove(struct platform_device *pdev)
{
	int ret;
	struct xocl_test *xt = platform_get_drvdata(pdev);

	/* By now, partition driver should prevent any inter-leaf call. */

	xocl_info(pdev, "leaving...");

	(void) xocl_subdev_remove_event_cb(pdev, xt->evt_hdl);

	ret = xocl_devnode_destroy(pdev);
	if (ret)
		return ret;
	/* By now, no more access thru cdev. */

	(void) sysfs_remove_group(&DEV(pdev)->kobj, &xocl_test_attrgroup);
	/* By now, no more access thru sysfs nodes. */

	/* Clean up can safely be done now. */
	return 0;
}

static int
xocl_test_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
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

struct xocl_subdev_drvdata xocl_test_data = {
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
