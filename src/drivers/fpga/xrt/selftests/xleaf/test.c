// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Test Leaf Driver
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 *	Lizhi Hou <lizhih@xilinx.com>
 *	Sonal Santan <sonals@xilinx.com>
 */

#include <linux/delay.h>
#include <linux/uuid.h>
#include <linux/string.h>
#include "metadata.h"
#include "xleaf.h"
#include "test.h"

#define XRT_TEST "xrt_test"

struct xrt_test {
	struct xrt_device *xdev;
	struct xrt_device *leaf;
};

static bool xrt_test_leaf_match(enum xrt_subdev_id id,
				struct xrt_device *xdev,
				void *arg)
{
	int myid = (int)(uintptr_t)arg;
	return id == XRT_SUBDEV_TEST && xdev->instance != myid;
}

static ssize_t hold_store(struct device *dev,
			  struct device_attribute *da,
			  const char *buf, size_t count)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xrt_test *xt = xrt_get_drvdata(xdev);
	struct xrt_device *leaf;

	leaf = xleaf_get_leaf(xdev, xrt_test_leaf_match,
			      (void *)(uintptr_t)xdev->instance);
	if (leaf)
		xt->leaf = leaf;
	return count;
}
static DEVICE_ATTR_WO(hold);

static ssize_t release_store(struct device *dev,
			     struct device_attribute *da,
			     const char *buf, size_t count)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xrt_test *xt = xrt_get_drvdata(xdev);

	if (xt->leaf)
		(void)xleaf_put_leaf(xdev, xt->leaf);
	return count;
}
static DEVICE_ATTR_WO(release);

static struct attribute *xrt_test_attrs[] = {
	&dev_attr_hold.attr,
	&dev_attr_release.attr,
	NULL,
};

static const struct attribute_group xrt_test_attrgroup = {
	.attrs = xrt_test_attrs,
};

static void xrt_test_event_cb(struct xrt_device *xdev, void *arg)
{
	struct xrt_device *leaf;
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id = evt->xe_subdev.xevt_subdev_id;
	int instance = evt->xe_subdev.xevt_subdev_instance;

	switch (e) {
	case XRT_EVENT_POST_CREATION:
		if (id != XRT_SUBDEV_TEST)
			return;
		break;
	default:
		xrt_dbg(xdev, "ignored event %d", e);
		return;
	}

	leaf = xleaf_get_leaf_by_id(xdev, id, instance);
	if (leaf) {
		(void)xleaf_call(leaf, 1, NULL);
		(void)xleaf_put_leaf(xdev, leaf);
	}

	/* Broadcast event. */
	if (xdev->instance == 1)
		xleaf_broadcast_event(xdev, XRT_EVENT_TEST, true);
	xrt_info(xdev, "processed XRT_EVENT_POST_CREATION for (%d, %d)",
		id, instance);
}

static int xrt_test_cb_a(struct xrt_device *xdev, void *arg)
{
	struct xrt_xleaf_test_payload *payload = (struct xrt_xleaf_test_payload *)arg;
	const struct xrt_test *xt = xrt_get_drvdata(xdev);

	uuid_copy(&payload->dummy1, &uuid_null);
	strcpy(payload->dummy2, "alveo");
	xrt_info(xdev, "processed xleaf cmd XRT_XLEAF_TEST_A on leaf %p", xt->xdev);
	return 0;
}

/*
 * Forward the xleaf call to peer after flipping the cmd from _B to _A.
 */
static int xrt_test_cb_b(struct xrt_device *xdev, void *arg)
{
	int ret;
	int peer_instance = (xdev->instance == 0) ? 1 : 0;
	struct xrt_device *peer = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_TEST, peer_instance);
	const struct xrt_test *xt = xrt_get_drvdata(xdev);

	if (!peer)
		return -ENODEV;
	ret = xleaf_call(peer, XRT_XLEAF_TEST_A, arg);
	xleaf_put_leaf(xdev, peer);
	xrt_info(xdev, "processed xleaf cmd XRT_XLEAF_TEST_B on leaf %p", xt->xdev);
	return ret;
}

static int xrt_test_probe(struct xrt_device *xdev)
{
	struct xrt_test *xt;

	xrt_info(xdev, "probing...");

	xt = devm_kzalloc(DEV(xdev), sizeof(*xt), GFP_KERNEL);
	if (!xt)
		return -ENOMEM;

	xt->xdev = xdev;
	xrt_set_drvdata(xdev, xt);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(xdev)->kobj, &xrt_test_attrgroup))
		xrt_err(xdev, "failed to create sysfs group");

	return 0;
}

static void xrt_test_remove(struct xrt_device *xdev)
{
	/* By now, group driver should prevent any inter-leaf call. */
	xrt_info(xdev, "leaving...");

	(void)sysfs_remove_group(&DEV(xdev)->kobj, &xrt_test_attrgroup);
	/* By now, no more access thru sysfs nodes. */

	/* Clean up can safely be done now. */
}

static int
xrt_test_leaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	int ret = 0;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		xrt_test_event_cb(xdev, arg);
		break;
	case XRT_XLEAF_TEST_A:
		ret = xrt_test_cb_a(xdev, arg);
		break;
	case XRT_XLEAF_TEST_B:
		ret = xrt_test_cb_b(xdev, arg);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	return ret;
}

static int xrt_test_open(struct inode *inode, struct file *file)
{
	struct xrt_device *xdev = xleaf_devnode_open(inode);

	/* Device may have gone already when we get here. */
	if (!xdev)
		return -ENODEV;

	xrt_info(xdev, "opened");
	file->private_data = xrt_get_drvdata(xdev);
	return 0;
}

static ssize_t
xrt_test_read(struct file *file, char __user *ubuf, size_t n, loff_t *off)
{
	int i;
	struct xrt_test *xt = file->private_data;

	for (i = 0; i < 4; i++) {
		xrt_info(xt->xdev, "reading...");
		ssleep(1);
	}
	return n;
}

static ssize_t
xrt_test_write(struct file *file, const char __user *ubuf, size_t n, loff_t *off)
{
	int i;
	struct xrt_test *xt = file->private_data;

	for (i = 0; i < 4; i++) {
		xrt_info(xt->xdev, "writing %d...", i);
		ssleep(1);
	}
	return n;
}

static int xrt_test_close(struct inode *inode, struct file *file)
{
	struct xrt_test *xt = file->private_data;

	xleaf_devnode_close(inode);

	xrt_info(xt->xdev, "closed");
	return 0;
}

/* Link to device tree nodes. */
static struct xrt_dev_endpoints xrt_test_endpoints[] = {
	{
		.xse_names = (struct xrt_dev_ep_names []){
			{ .ep_name = XRT_MD_NODE_TEST },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

/*
 * Callbacks registered with Linux's platform driver infrastructure.
 */
static struct xrt_driver xrt_test_driver = {
	.driver	= {
		.name = XRT_TEST,
	},
	.file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = xrt_test_open,
			.release = xrt_test_close,
			.read = xrt_test_read,
			.write = xrt_test_write,
		},
		.xsf_mode = XRT_DEV_FILE_MULTI_INST,
	},
	.subdev_id = XRT_SUBDEV_TEST,
	.endpoints = xrt_test_endpoints,
	.probe = xrt_test_probe,
	.remove = xrt_test_remove,
	.leaf_call = xrt_test_leaf_call,
};

int selftest_test_register_leaf(void)
{
	return xrt_register_driver(&xrt_test_driver);
}

void selftest_test_unregister_leaf(void)
{
	xrt_unregister_driver(&xrt_test_driver);
}
