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
#include "metadata.h"
#include "xleaf.h"
#include "xleaf/test.h"

#define	XRT_TEST "xrt_test"

struct xrt_test {
	struct platform_device *pdev;
	struct platform_device *leaf;
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

	leaf = xleaf_get_leaf(pdev, xrt_test_leaf_match,
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
		(void) xleaf_put_leaf(pdev, xt->leaf);
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

static void xrt_test_event_cb(struct platform_device *pdev, void *arg)
{
	struct platform_device *leaf;
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
		xrt_dbg(pdev, "ignored event %d", e);
		return;
	}

	leaf = xleaf_get_leaf_by_id(pdev, id, instance);
	if (leaf) {
		(void) xleaf_ioctl(leaf, 1, NULL);
		(void) xleaf_put_leaf(pdev, leaf);
	}

	/* Broadcast event. */
	if (pdev->id == 1)
		xleaf_broadcast_event(pdev, XRT_EVENT_TEST, true);
	xrt_dbg(pdev, "processed XRT_EVENT_POST_CREATION for (%d, %d)",
		id, instance);
}

static int xrt_test_ioctl_cb_a(struct platform_device *pdev, void *arg)
{
	union xrt_xleaf_test_payload *payload = (union xrt_xleaf_test_payload *)arg;
	const struct xrt_test *xt = platform_get_drvdata(pdev);

	payload->out.dummy3 = 0xdeadface;
	xrt_dbg(pdev, "processed ioctl cmd XRT_XLEAF_TEST_A on leaf %p", xt->pdev);
	return 0;
}

static int xrt_test_ioctl_cb_b(struct platform_device *pdev, void *arg)
{
	union xrt_xleaf_test_payload *payload = (union xrt_xleaf_test_payload *)arg;
	const struct xrt_test *xt = platform_get_drvdata(pdev);

	payload->out.dummy3 = 0xfaceb00c;
	xrt_dbg(pdev, "processed ioctl cmd XRT_XLEAF_TEST_B on leaf %p", xt->pdev);
	return 0;
}

#if 0
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
#endif
static int xrt_test_probe(struct platform_device *pdev)
{
	struct xrt_test *xt;
//	char *dtb = NULL;

	xrt_info(pdev, "probing...");

	xt = devm_kzalloc(DEV(pdev), sizeof(*xt), GFP_KERNEL);
	if (!xt)
		return -ENOMEM;

	xt->pdev = pdev;
	platform_set_drvdata(pdev, xt);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(pdev)->kobj, &xrt_test_attrgroup))
		xrt_err(pdev, "failed to create sysfs group");

	/* Trigger group creation, only when this is the first instance. */
#if 0
	if (pdev->id == 0) {
		(void) xrt_test_create_metadata(xt, &dtb);
		if (dtb)
			(void) xleaf_create_group(pdev, dtb);
		vfree(dtb);
	} else {
		xleaf_broadcast_event(pdev, XRT_EVENT_TEST, false);
	}
#endif
	/* After we return here, we'll get inter-leaf calls. */
	return 0;
}

static int xrt_test_remove(struct platform_device *pdev)
{
	/* By now, group driver should prevent any inter-leaf call. */
	xrt_info(pdev, "leaving...");

	(void) sysfs_remove_group(&DEV(pdev)->kobj, &xrt_test_attrgroup);
	/* By now, no more access thru sysfs nodes. */

	/* Clean up can safely be done now. */
	return 0;
}

static int
xrt_test_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	int ret = 0;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		xrt_test_event_cb(pdev, arg);
		break;
	case XRT_XLEAF_TEST_A:
		ret = xrt_test_ioctl_cb_a(pdev, arg);
		break;
	case XRT_XLEAF_TEST_B:
		ret = xrt_test_ioctl_cb_b(pdev, arg);
	default:
		break;
	}
	return ret;
}

static int xrt_test_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xleaf_devnode_open(inode);

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

	for (i = 0; i < 4; i++) {
		xrt_info(xt->pdev, "reading...");
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
		xrt_info(xt->pdev, "writing %d...", i);
		ssleep(1);
	}
	return n;
}

static int xrt_test_close(struct inode *inode, struct file *file)
{
	struct xrt_test *xt = file->private_data;

	xleaf_devnode_close(inode);

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
 * Callbacks registered with root.
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
			.write = xrt_test_write,
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
