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
#include "xrt-metadata.h"
#include "xrt-subdev.h"

#define	XRT_TEST "xrt_test"

struct xrt_test {
	struct platform_device *pdev;
	struct platform_device *leaf;
	void *evt_hdl;
};

static bool xrt_test_leaf_match(enum xrt_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	int myid = (int)(uintptr_t)arg;
	return id == XRT_SUBDEV_TEST && pdev->id != myid;
}

static ssize_t hold_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct xrt_test *xt = platform_get_drvdata(pdev);
	struct platform_device *leaf;

	leaf = xrt_subdev_get_leaf(pdev, xrt_test_leaf_match,
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
	struct xrt_test *xt = platform_get_drvdata(pdev);

	if (xt->leaf)
		(void) xrt_subdev_put_leaf(pdev, xt->leaf);
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

static void xrt_test_async_evt_cb(struct platform_device *pdev,
	enum xrt_events evt, void *arg, bool success)
{
	xrt_info(pdev, "async broadcast event (%d) is %s", evt,
		success ? "successful" : "failed");
}

static int xrt_test_event_cb(struct platform_device *pdev,
	enum xrt_events evt, void *arg)
{
	struct platform_device *leaf;
	struct xrt_event_arg_subdev *esd = (struct xrt_event_arg_subdev *)arg;


	switch (evt) {
	case XRT_EVENT_POST_CREATION:
		break;
	default:
		xrt_info(pdev, "ignored event %d", evt);
		return XRT_EVENT_CB_CONTINUE;
	}

	leaf = xrt_subdev_get_leaf_by_id(pdev, esd->xevt_subdev_id,
		esd->xevt_subdev_instance);
	if (leaf) {
		(void) xrt_subdev_ioctl(leaf, 1, NULL);
		(void) xrt_subdev_put_leaf(pdev, leaf);
	}

	/* Broadcast event. */
	if (pdev->id == 1) {
		xrt_subdev_broadcast_event_async(pdev, XRT_EVENT_TEST,
			xrt_test_async_evt_cb, NULL);
	}

	xrt_info(pdev, "processed event %d for (%d, %d)",
		evt, esd->xevt_subdev_id, esd->xevt_subdev_instance);
	return XRT_EVENT_CB_CONTINUE;
}

static int xrt_test_create_metadata(struct xrt_test *xt, char **root_dtb)
{
	char *dtb = NULL;
	struct xrt_md_endpoint ep = { .ep_name = NODE_TEST };
	int ret;

	ret = xrt_md_create(DEV(xt->pdev), &dtb);
	if (ret) {
		xrt_err(xt->pdev, "create metadata failed, ret %d", ret);
		goto failed;
	}

	ret = xrt_md_add_endpoint(DEV(xt->pdev), dtb, &ep);
	if (ret) {
		xrt_err(xt->pdev, "add test node failed, ret %d", ret);
		goto failed;
	}

	*root_dtb = dtb;
	return 0;

failed:
	vfree(dtb);
	return ret;
}

static int xrt_test_probe(struct platform_device *pdev)
{
	struct xrt_test *xt;
	char *dtb = NULL;

	xrt_info(pdev, "probing...");

	xt = devm_kzalloc(DEV(pdev), sizeof(*xt), GFP_KERNEL);
	if (!xt)
		return -ENOMEM;

	xt->pdev = pdev;
	platform_set_drvdata(pdev, xt);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(pdev)->kobj, &xrt_test_attrgroup))
		xrt_err(pdev, "failed to create sysfs group");

	/* Add event callback to wait for the peer instance. */
	xt->evt_hdl = xrt_subdev_add_event_cb(pdev, xrt_test_leaf_match,
		(void *)(uintptr_t)pdev->id, xrt_test_event_cb);

	/* Trigger partition creation, only when this is the first instance. */
	if (pdev->id == 0) {
		(void) xrt_test_create_metadata(xt, &dtb);
		if (dtb)
			(void) xrt_subdev_create_partition(pdev, dtb);
		vfree(dtb);
	} else {
		xrt_subdev_broadcast_event(pdev, XRT_EVENT_TEST);
	}

	/* After we return here, we'll get inter-leaf calls. */
	return 0;
}

static int xrt_test_remove(struct platform_device *pdev)
{
	struct xrt_test *xt = platform_get_drvdata(pdev);

	/* By now, partition driver should prevent any inter-leaf call. */

	xrt_info(pdev, "leaving...");

	(void) xrt_subdev_remove_event_cb(pdev, xt->evt_hdl);

	(void) sysfs_remove_group(&DEV(pdev)->kobj, &xrt_test_attrgroup);
	/* By now, no more access thru sysfs nodes. */

	/* Clean up can safely be done now. */
	return 0;
}

static int
xrt_test_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	xrt_info(pdev, "handling IOCTL cmd: %d", cmd);
	return 0;
}

static int xrt_test_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xrt_devnode_open(inode);

	/* Device may have gone already when we get here. */
	if (!pdev)
		return -ENODEV;

	xrt_info(pdev, "opened");
	file->private_data = platform_get_drvdata(pdev);
	return 0;
}

static ssize_t
xrt_test_read(struct file *file, char __user *ubuf, size_t n, loff_t *off)
{
	int i;
	struct xrt_test *xt = file->private_data;

	for (i = 0; i < 10; i++) {
		xrt_info(xt->pdev, "reading...");
		ssleep(1);
	}
	return 0;
}

static int xrt_test_close(struct inode *inode, struct file *file)
{
	struct xrt_test *xt = file->private_data;

	xrt_devnode_close(inode);

	xrt_info(xt->pdev, "closed");
	return 0;
}

/* Link to device tree nodes. */
struct xrt_subdev_endpoints xrt_test_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []){
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
struct xrt_subdev_drvdata xrt_test_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_test_leaf_ioctl,
	},
	.xsd_file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = xrt_test_open,
			.release = xrt_test_close,
			.read = xrt_test_read,
		},
		.xsf_mode = XRT_SUBDEV_FILE_MULTI_INST,
	},
};

static const struct platform_device_id xrt_test_id_table[] = {
	{ XRT_TEST, (kernel_ulong_t)&xrt_test_data },
	{ },
};

/*
 * Callbacks registered with Linux's platform driver infrastructure.
 */
struct platform_driver xrt_test_driver = {
	.driver	= {
		.name    = XRT_TEST,
	},
	.probe   = xrt_test_probe,
	.remove  = xrt_test_remove,
	.id_table = xrt_test_id_table,
};
