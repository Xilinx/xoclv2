// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Test Leaf Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/delay.h>
#include "xocl-metadata.h"
#include "xocl-subdev.h"

#define	XOCL_TEST "xocl_test"

struct xocl_test {
	struct platform_device *pdev;
	struct platform_device *leaf;
	void *evt_hdl;
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

static void xocl_test_async_evt_cb(struct platform_device *pdev,
	enum xocl_events evt, void *arg, bool success)
{
	xocl_info(pdev, "async broadcast event (%d) is %s", evt,
		success ? "successful" : "failed");
}

static int xocl_test_event_cb(struct platform_device *pdev,
	enum xocl_events evt, void *arg)
{
	struct platform_device *leaf;
	struct xocl_event_arg_subdev *esd = (struct xocl_event_arg_subdev *)arg;


	switch (evt) {
	case XOCL_EVENT_POST_CREATION:
		break;
	default:
		xocl_info(pdev, "ignored event %d", evt);
		return XOCL_EVENT_CB_CONTINUE;
	}

	leaf = xocl_subdev_get_leaf_by_id(pdev, esd->xevt_subdev_id,
		esd->xevt_subdev_instance);
	if (leaf) {
		(void) xocl_subdev_ioctl(leaf, 1, NULL);
		(void) xocl_subdev_put_leaf(pdev, leaf);
	}

	/* Broadcast event. */
	if (pdev->id == 1) {
		xocl_subdev_broadcast_event_async(pdev, XOCL_EVENT_TEST,
			xocl_test_async_evt_cb, NULL);
	}

	xocl_info(pdev, "processed event %d for (%d, %d)",
		evt, esd->xevt_subdev_id, esd->xevt_subdev_instance);
	return XOCL_EVENT_CB_CONTINUE;
}

static int xocl_test_create_metadata(struct xocl_test *xt, char **root_dtb)
{
	char *dtb = NULL;
	struct xocl_md_endpoint ep = { .ep_name = NODE_TEST };
	int ret;

	ret = xocl_md_create(DEV(xt->pdev), &dtb);
	if (ret) {
		xocl_err(xt->pdev, "create metadata failed, ret %d", ret);
		goto failed;
	}

	ret = xocl_md_add_endpoint(DEV(xt->pdev), dtb, &ep);
	if (ret) {
		xocl_err(xt->pdev, "add test node failed, ret %d", ret);
		goto failed;
	}

	*root_dtb = dtb;
	return 0;

failed:
	vfree(dtb);
	return ret;
}

static int xocl_test_probe(struct platform_device *pdev)
{
	struct xocl_test *xt;
	char *dtb = NULL;

	xocl_info(pdev, "probing...");

	xt = devm_kzalloc(DEV(pdev), sizeof(*xt), GFP_KERNEL);
	if (!xt)
		return -ENOMEM;

	xt->pdev = pdev;
	platform_set_drvdata(pdev, xt);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(pdev)->kobj, &xocl_test_attrgroup))
		xocl_err(pdev, "failed to create sysfs group");

	/* Add event callback to wait for the peer instance. */
	xt->evt_hdl = xocl_subdev_add_event_cb(pdev, xocl_test_leaf_match,
		(void *)(uintptr_t)pdev->id, xocl_test_event_cb);

	/* Trigger partition creation, only when this is the first instance. */
	if (pdev->id == 0) {
		(void) xocl_test_create_metadata(xt, &dtb);
		if (dtb)
			(void) xocl_subdev_create_partition(pdev, dtb);
		vfree(dtb);
	} else {
		xocl_subdev_broadcast_event(pdev, XOCL_EVENT_TEST);
	}

	/* After we return here, we'll get inter-leaf calls. */
	return 0;
}

static int xocl_test_remove(struct platform_device *pdev)
{
	struct xocl_test *xt = platform_get_drvdata(pdev);

	/* By now, partition driver should prevent any inter-leaf call. */

	xocl_info(pdev, "leaving...");

	(void) xocl_subdev_remove_event_cb(pdev, xt->evt_hdl);

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

/* Link to device tree nodes. */
struct xocl_subdev_endpoints xocl_test_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names []){
			{ .ep_name = NODE_TEST },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

/*
 * Callbacks registered with parent driver infrastructure.
 */
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
		.xsf_mode = XOCL_SUBDEV_FILE_MULTI_INST,
	},
};

static const struct platform_device_id xocl_test_id_table[] = {
	{ XOCL_TEST, (kernel_ulong_t)&xocl_test_data },
	{ },
};

/*
 * Callbacks registered with Linux's platform driver infrastructure.
 */
struct platform_driver xocl_test_driver = {
	.driver	= {
		.name    = XOCL_TEST,
	},
	.probe   = xocl_test_probe,
	.remove  = xocl_test_remove,
	.id_table = xocl_test_id_table,
};
