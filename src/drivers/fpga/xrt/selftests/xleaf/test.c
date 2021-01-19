// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Test Leaf Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/uuid.h>
#include <linux/string.h>
#include "metadata.h"
#include "xleaf.h"
#include "test.h"
#include "ring-drv.h"
#include "../xleaf-test.h"

#define	XRT_TEST		"xrt_test"
#define	XRT_TEST_MAX_RINGS	64

struct xrt_test;

struct xrt_test_client {
	struct xrt_test *xt;
};

struct xrt_test_ring {
	struct xrt_test_client *client;
	uint64_t ring;
};

struct xrt_test {
	struct platform_device *pdev;
	struct platform_device *leaf;
	struct mutex lock;
	// ring buffer module handle
	void *ring_hdl;
	// records individual ring buffer handle
	struct xrt_test_ring rings[XRT_TEST_MAX_RINGS];
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
	struct xrt_xleaf_test_payload *payload = (struct xrt_xleaf_test_payload *)arg;
	const struct xrt_test *xt = platform_get_drvdata(pdev);

	uuid_copy(&payload->dummy1, &uuid_null);
	strcpy(payload->dummy2, "alveo");
	xrt_dbg(pdev, "processed ioctl cmd XRT_XLEAF_TEST_A on leaf %p", xt->pdev);
	return 0;
}

/*
 * Forward the ioctl call to peer after flipping the cmd from _B to _A.
 */
static int xrt_test_ioctl_cb_b(struct platform_device *pdev, void *arg)
{
	int ret;
	int peer_instance = (pdev->id == 0) ? 1 : 0;
	struct platform_device *peer = xleaf_get_leaf_by_id(pdev, XRT_SUBDEV_TEST, peer_instance);
	const struct xrt_test *xt = platform_get_drvdata(pdev);

	if (!peer)
		return -ENODEV;
	ret = xleaf_ioctl(peer, XRT_XLEAF_TEST_A, arg);
	xleaf_put_leaf(pdev, peer);
	xrt_dbg(pdev, "processed ioctl cmd XRT_XLEAF_TEST_B on leaf %p", xt->pdev);
	return ret;
}


static int xrt_test_probe(struct platform_device *pdev)
{
	struct xrt_test *xt;

	xrt_info(pdev, "probing...");

	xt = devm_kzalloc(DEV(pdev), sizeof(*xt), GFP_KERNEL);
	if (!xt)
		return -ENOMEM;

	xt->pdev = pdev;
	mutex_init(&xt->lock);
	platform_set_drvdata(pdev, xt);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(pdev)->kobj, &xrt_test_attrgroup))
		xrt_err(pdev, "failed to create sysfs group");

	xt->ring_hdl = xrt_ring_probe(DEV(pdev), XRT_TEST_MAX_RINGS);

	return 0;
}

static int xrt_test_remove(struct platform_device *pdev)
{
	const struct xrt_test *xt = platform_get_drvdata(pdev);

	/* By now, group driver should prevent any inter-leaf call. */
	xrt_info(pdev, "leaving...");

	if (xt->ring_hdl)
		xrt_ring_remove(xt->ring_hdl);

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
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	return ret;
}

static void
xrt_test_ring_req_handler(void *arg, struct xrt_ring_entry *req, size_t reqsz)
{
	struct xrt_test_ring *xr = arg;
	struct xrt_ring_entry *resp = xrt_ring_cq_produce_begin(
		xr->client->xt->ring_hdl, xr->ring, NULL);

	if (resp) {
		resp->xre_op_result = 0;
		resp->xre_id = req->xre_id;
		xrt_ring_cq_produce_end(xr->client->xt->ring_hdl, xr->ring);
	} else {
		xrt_err(xr->client->xt->pdev, "CQ ring overflow!");
	}
}

static int xrt_test_add_ring(struct xrt_test_client *xc,
	struct xrt_ioc_ring_register *reg)
{
	int i, ret;
	struct xrt_test *xt = xc->xt;

	mutex_lock(&xt->lock);
	for (i = 0; i < XRT_TEST_MAX_RINGS && xt->rings[i].client; i++)
		;
	BUG_ON(i == XRT_TEST_MAX_RINGS);
	xt->rings[i].client = xc;
	xt->rings[i].ring = INVALID_RING_HANDLE;
	mutex_unlock(&xt->lock);

	ret = xrt_ring_register(xt->ring_hdl, reg,
		xrt_test_ring_req_handler, &xt->rings[i]);
	if (ret) {
		xt->rings[i].client = NULL;
		return ret;
	}

	xt->rings[i].ring = reg->xirr_ring_handle;

	return 0;
}

static int xrt_test_del_ring(struct xrt_test_client *xc,
	struct xrt_ioc_ring_unregister *unreg)
{
	int i, ret = 0;
	struct xrt_test_ring *r;
	struct xrt_test *xt = xc->xt;
	struct xrt_ioc_ring_unregister ur = { 0 };
	struct xrt_ioc_ring_unregister *urp = unreg ? unreg : &ur;

	mutex_lock(&xt->lock);

	for (i = 0; i < XRT_TEST_MAX_RINGS; i++) {
		r = &xt->rings[i];
		if (r->client != xc)
			continue;
		if (unreg && unreg->xiru_ring_handle != r->ring)
			continue;

		if (!unreg)
			urp->xiru_ring_handle = r->ring;
		ret = xrt_ring_unregister(xt->ring_hdl, urp);
		if (ret)
			break;
		r->client = NULL;
	}

	mutex_unlock(&xt->lock);

	return ret;
}

static int xrt_test_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xleaf_devnode_open(inode);
	struct xrt_test_client *xc;

	/* Device may have gone already when we get here. */
	if (!pdev)
		return -ENODEV;

	xc = vzalloc(sizeof(struct xrt_test_client));
	if (!xc)
		return -ENOMEM;

	xc->xt = platform_get_drvdata(pdev);
	file->private_data = xc;
	xrt_info(pdev, "opened");
	return 0;
}

static ssize_t
xrt_test_read(struct file *file, char __user *ubuf, size_t n, loff_t *off)
{
	int i;
	struct xrt_test_client *xc = file->private_data;
	struct xrt_test *xt = xc->xt;

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
	struct xrt_test_client *xc = file->private_data;
	struct xrt_test *xt = xc->xt;

	for (i = 0; i < 4; i++) {
		xrt_info(xt->pdev, "writing %d...", i);
		ssleep(1);
	}
	return n;
}

static long xrt_test_ioctl(struct file *file, unsigned int cmd,
	unsigned long uarg)
{
	long ret = 0;
	struct xrt_test_client *xc = file->private_data;
	struct xrt_test *xt = xc->xt;
	void __user *arg = (void __user *)uarg;

	switch (cmd) {
	case XRT_TEST_REGISTER_RING: {
		struct xrt_ioc_ring_register reg;

		if (copy_from_user((void *)&reg, arg, sizeof(reg))) {
			ret = -EFAULT;
			break;
		}

		ret = xrt_test_add_ring(xc, &reg);
		if (ret) {
			xrt_err(xt->pdev, "can't add ring buffer: %ld", ret);
			break;
		}

		if (copy_to_user(arg, (void *)&reg, sizeof(reg))) {
			struct xrt_ioc_ring_unregister unreg = {
				reg.xirr_ring_handle };

			xrt_test_del_ring(xc, &unreg);
			ret = -EFAULT;
			break;
		}
		xrt_info(xt->pdev, "successfully added ring buffer");
		break;
	}
	case XRT_TEST_UNREGISTER_RING: {
		struct xrt_ioc_ring_unregister unreg;

		if (copy_from_user((void *)&unreg, arg, sizeof(unreg))) {
			ret = -EFAULT;
			break;
		}

		ret = xrt_test_del_ring(xc, &unreg);
		if (ret) {
			xrt_err(xt->pdev, "can't delete ring buffer: %ld", ret);
			break;
		}

		xrt_info(xt->pdev, "successfully deleted ring buffer");
		break;
	}
	case XRT_TEST_SQ_WAKEUP: {
		struct xrt_ioc_ring_sq_wakeup wakeup;

		if (copy_from_user((void *)&wakeup, arg, sizeof(wakeup))) {
			ret = -EFAULT;
			break;
		}

		ret = xrt_ring_sq_wakeup(xt->ring_hdl, &wakeup);
		if (ret)
			xrt_err(xt->pdev, "can't wakeup ring buffer: %ld", ret);
		break;
	}
	default:
		xrt_err(xt->pdev, "unknown IOCTL cmd: %d", cmd);
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static int xrt_test_close(struct inode *inode, struct file *file)
{
	struct xrt_test_client *xc = file->private_data;
	struct xrt_test *xt = xc->xt;

	xleaf_devnode_close(inode);
	// remove all registered ring for this client.
	(void) xrt_test_del_ring(xc, NULL);
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
			.unlocked_ioctl = xrt_test_ioctl,
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

int selftest_test_register_leaf(void)
{
	return xleaf_register_external_driver(XRT_SUBDEV_TEST,
		&xrt_test_driver, xrt_test_endpoints);
}

void selftest_test_unregister_leaf(void)
{
	xleaf_unregister_external_driver(XRT_SUBDEV_TEST);
}
