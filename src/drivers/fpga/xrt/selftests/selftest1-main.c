// SPDX-License-Identifier: GPL-2.0
/*
 * XRT driver infrastructure selftest 1 main leaf
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Sonal Santan <sonals@xilinx.com>
 */

#include <linux/firmware.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include "xclbin-helper.h"
#include "metadata.h"
#include "xleaf/flash.h"
#include "xleaf/devctl.h"
#include "xleaf/test.h"
#include "xmgnt-main.h"
#include "main-impl.h"
#include "xleaf.h"
#include <linux/xrt/flash_xrt_data.h>
#include <linux/xrt/xmgnt-ioctl.h>

#define SELFTEST1_MAIN "xrt-selftest1-main"

struct selftest1_main {
	struct xrt_device *xdev;
	struct mutex busy_mutex; /* device busy lock */
};

struct selftest1_main_client_data {
	struct xrt_device *xdev;  /* This subdev */
	struct xrt_device *leaf0; /* test[0] handle obtained after lookup */
	struct xrt_device *leaf1; /* test[1] handle obtained after lookup */
};

static void selftest1_main_event_cb(struct xrt_device *xdev, void *arg)
{
	struct selftest1_main *xmm = xrt_get_drvdata(xdev);
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id = evt->xe_subdev.xevt_subdev_id;

	xrt_info(xdev, "%p.event(%d, %p) %d", xmm, e, evt, id);
	switch (e) {
	case XRT_EVENT_POST_CREATION:
	case XRT_EVENT_PRE_REMOVAL:
	default:
		xrt_dbg(xdev, "ignored event %d", e);
		break;
	}
}

static int selftest1_main_probe(struct xrt_device *xdev)
{
	struct selftest1_main *xmm;

	xrt_info(xdev, "probing...");

	xmm = devm_kzalloc(DEV(xdev), sizeof(*xmm), GFP_KERNEL);
	if (!xmm)
		return -ENOMEM;

	xmm->xdev = xdev;
	xrt_set_drvdata(xdev, xmm);
	mutex_init(&xmm->busy_mutex);

	return 0;
}

static void selftest1_main_remove(struct xrt_device *xdev)
{
	xrt_info(xdev, "leaving...");
}

/* Basic test for XRT core which validates xleaf lookup with EP name together with
 * instance number as key. Perform the following operation:
 *
 * group2.xmgnt_main() {
 *     lookup(group0.test);
 *     lookup(group1.test);
 * }
 */

static struct selftest1_main_client_data *
selftest1_validate_ini(struct xrt_device *xdev)
{
	struct selftest1_main_client_data *xdd =
		vzalloc(sizeof(struct selftest1_main_client_data));

	xdd->xdev = xdev;
	xdd->leaf0 = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_TEST, 0);
	if (!xdd->leaf0) {
		xrt_err(xdev, "Cannot find xleaf test instance[0]");
		goto finally;
	}
	xdd->leaf1 = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_TEST, 1);
	if (!xdd->leaf1) {
		xrt_err(xdev, "Cannot find xleaf test instance[1]");
		xleaf_put_leaf(xdd->xdev, xdd->leaf0);
		goto finally;
	}

	xrt_info(xdev, "xleaf test instance[0] %p", xdd->leaf0);
	xrt_info(xdev, "xleaf test instance[1] %p", xdd->leaf1);
	return xdd;
finally:
	vfree(xdd);
	return NULL;
}

/* Basic test for XRT core which validates inter xleaf calls. Perform the
 * following operations:
 *
 * group2.xmgnt_main() {
 *     xleaf_call(group0.test, XRT_XLEAF_TEST_A, arg);
 *     xleaf_call(group1.test, XRT_XLEAF_TEST_B, arg) {
 *         lookup(group0.test);
 *         xleaf_call(group0.test, XRT_XLEAF_TEST_A, arg);
 *     }
 * }
 */
static int selftest1_validate_fini(struct selftest1_main_client_data *xdd)
{
	int ret = -EDOM;
	struct xrt_xleaf_test_payload arg_a = {uuid_null, "FPGA"};
	struct xrt_xleaf_test_payload arg_b = {uuid_null, "FPGA"};

	if (!xdd)
		return ret;

	generate_random_uuid(arg_a.dummy1.b);
	generate_random_uuid(arg_b.dummy1.b);

	ret = xleaf_call(xdd->leaf0, XRT_XLEAF_TEST_A, &arg_a);
	if (ret || !uuid_is_null(&arg_a.dummy1) || strcmp(arg_a.dummy2, "alveo")) {
		xrt_err(xdd->xdev, "xleaf test instance[0] %p cmd %d failed",
			xdd->leaf1, XRT_XLEAF_TEST_A);
		ret = -EDOM;
		goto finally;
	}
	ret = xleaf_call(xdd->leaf1, XRT_XLEAF_TEST_B, &arg_b);
	if (ret || !uuid_is_null(&arg_b.dummy1) || strcmp(arg_b.dummy2, "alveo")) {
		xrt_err(xdd->xdev, "xleaf test instance[1] %p cmd %d failed",
			xdd->leaf1, XRT_XLEAF_TEST_B);
		ret = -EDOM;
		goto finally;
	}

finally:
	xleaf_put_leaf(xdd->xdev, xdd->leaf1);
	xleaf_put_leaf(xdd->xdev, xdd->leaf0);
	vfree(xdd);
	return ret;
}

static int selftest1_mainleaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	struct selftest1_main *xmm = xrt_get_drvdata(xdev);
	int ret = 0;

	xrt_info(xdev, "%p.leaf_call(%d, %p)", xmm, cmd, arg);
	switch (cmd) {
	case XRT_XLEAF_EVENT:
		selftest1_main_event_cb(xdev, arg);
		break;
	default:
		xrt_err(xdev, "unknown cmd: %d", cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static ssize_t selftest1_main_leaf_read(struct file *file, char __user *ubuf, size_t n, loff_t *off)
{
	int i;
	struct selftest1_main_client_data *xdd = file->private_data;

	for (i = 0; i < 4; i++) {
		xrt_info(xdd->xdev, "reading...");
		ssleep(1);
	}
	return n;
}

static ssize_t selftest1_main_leaf_write(struct file *file, const char __user *ubuf,
					 size_t n, loff_t *off)
{
	int i;
	struct selftest1_main_client_data *xdd = file->private_data;

	for (i = 0; i < 4; i++) {
		xrt_info(xdd->xdev, "writing %d...", i);
		ssleep(1);
	}
	return n;
}

static int selftest1_main_open(struct inode *inode, struct file *file)
{
	struct xrt_device *xdev = xleaf_devnode_open(inode);
	struct selftest1_main_client_data *xdd;

	/* Device may have gone already when we get here. */
	if (!xdev)
		return -ENODEV;

	xrt_info(xdev, "opened");
	/* Obtain the reference to test xleaf nodes */
	xdd = selftest1_validate_ini(xdev);
	file->private_data = xdd;
	if (!xdd) {
		xrt_err(xdev, "FAILED test %s", SELFTEST1_MAIN);
		return -EDOM;
	}
	return 0;
}

static int selftest1_main_close(struct inode *inode, struct file *file)
{
	struct selftest1_main_client_data *xdd = file->private_data;
	struct xrt_device *xdev = xdd->xdev;
	/* Perform inter xleaf calls and then release test node handles */
	int ret = selftest1_validate_fini(xdd);

	file->private_data = NULL;
	xleaf_devnode_close(inode);

	if (ret)
		xrt_err(xdev, "FAILED test %s", SELFTEST1_MAIN);
	else
		xrt_info(xdev, "PASSED test %s", SELFTEST1_MAIN);

	xrt_info(xdev, "closed");
	return 0;
}

static struct xrt_dev_endpoints xrt_mgnt_main_endpoints[] = {
	{
		.xse_names = (struct xrt_dev_ep_names []){
			{ .ep_name = XRT_MD_NODE_MGNT_MAIN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_driver selftest1_main_driver = {
	.driver	= {
		.name    = SELFTEST1_MAIN,
	},
	.file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = selftest1_main_open,
			.release = selftest1_main_close,
			.read = selftest1_main_leaf_read,
			.write = selftest1_main_leaf_write,
		},
		.xsf_dev_name = "selftest1",
	},
	.subdev_id = XRT_SUBDEV_MGNT_MAIN,
	.endpoints = xrt_mgnt_main_endpoints,
	.probe = selftest1_main_probe,
	.remove = selftest1_main_remove,
	.leaf_call = selftest1_mainleaf_call,
};

int selftest1_main_register_leaf(void)
{
	return xrt_register_driver(&selftest1_main_driver);
}

void selftest1_main_unregister_leaf(void)
{
	xrt_unregister_driver(&selftest1_main_driver);
}
