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
#include "xleaf/gpio.h"
#include "xleaf/test.h"
#include "xmgmt-main.h"
#include "main-impl.h"
#include "xleaf.h"
#include <linux/xrt/flash_xrt_data.h>
#include <linux/xrt/xmgmt-ioctl.h>

#define	SELFTEST1_MAIN "xrt-selftest1-main"

struct selftest1_main {
	struct platform_device *pdev;
	struct mutex busy_mutex;
};

struct selftest1_main_client_data {
	struct platform_device *pdev;  /* This subdev */
	struct platform_device *leaf0; /* test[0] handle obtained after lookup */
	struct platform_device *leaf1; /* test[1] handle obtained after lookup */
};

static void selftest1_main_event_cb(struct platform_device *pdev, void *arg)
{
	struct selftest1_main *xmm = platform_get_drvdata(pdev);
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id = evt->xe_subdev.xevt_subdev_id;

	xrt_info(pdev, "%p.event(%d, %p) %d", xmm, e, evt, id);
	switch (e) {
	case XRT_EVENT_POST_CREATION:
	case XRT_EVENT_PRE_REMOVAL:
	default:
		xrt_dbg(pdev, "ignored event %d", e);
		break;
	}
}

static int selftest1_main_probe(struct platform_device *pdev)
{
	struct selftest1_main *xmm;

	xrt_info(pdev, "probing...");

	xmm = devm_kzalloc(DEV(pdev), sizeof(*xmm), GFP_KERNEL);
	if (!xmm)
		return -ENOMEM;

	xmm->pdev = pdev;
	platform_set_drvdata(pdev, xmm);
	mutex_init(&xmm->busy_mutex);

	return 0;
}

static int selftest1_main_remove(struct platform_device *pdev)
{
	xrt_info(pdev, "leaving...");
	return 0;
}

/* Basic test for XRT core which validates xleaf lookup with EP name together with
 * instance number as key. Perform the following operation:
 *
 * group2.xmgmt_main() {
 *     lookup(group0.test);
 *     lookup(group1.test);
 * }
 */

static struct selftest1_main_client_data *
selftest1_validate_ini(struct platform_device *pdev)
{
	struct selftest1_main_client_data *xdd =
		vzalloc(sizeof(struct selftest1_main_client_data));

	xdd->pdev = pdev;
	xdd->leaf0 = xleaf_get_leaf_by_id(pdev, XRT_SUBDEV_TEST, 0);
	if (!xdd->leaf0) {
		xrt_err(pdev, "Cannot find xleaf test instance[0]");
		goto finally;
	}
	xdd->leaf1 = xleaf_get_leaf_by_id(pdev, XRT_SUBDEV_TEST, 1);
	if (!xdd->leaf1) {
		xrt_err(pdev, "Cannot find xleaf test instance[1]");
		xleaf_put_leaf(xdd->pdev, xdd->leaf0);
		goto finally;
	}

	xrt_info(pdev, "xleaf test instance[0] %p", xdd->leaf0);
	xrt_info(pdev, "xleaf test instance[1] %p", xdd->leaf1);
	return xdd;
finally:
	vfree(xdd);
	return NULL;
}

/* Basic test for XRT core which validates inter xleaf ioctl calls. Perform the
 * following operations:
 *
 * group2.xmgmt_main() {
 *     ioctl(group0.test, XRT_XLEAF_TEST_A, arg);
 *     ioctl(group1.test, XRT_XLEAF_TEST_B, arg) {
 *         lookup(group0.test);
 *         ioctl(group0.test, XRT_XLEAF_TEST_A, arg);
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

	ret = xleaf_ioctl(xdd->leaf0, XRT_XLEAF_TEST_A, &arg_a);
	if (ret || !uuid_is_null(&arg_a.dummy1) || strcmp(arg_a.dummy2, "alveo")) {
		xrt_err(xdd->pdev, "xleaf test instance[0] %p ioctl %d failed",
			xdd->leaf1, XRT_XLEAF_TEST_A);
		ret = -EDOM;
		goto finally;
	}
	ret = xleaf_ioctl(xdd->leaf1, XRT_XLEAF_TEST_B, &arg_b);
	if (ret || !uuid_is_null(&arg_b.dummy1) || strcmp(arg_b.dummy2, "alveo")) {
		xrt_err(xdd->pdev, "xleaf test instance[1] %p ioctl %d failed",
			xdd->leaf1, XRT_XLEAF_TEST_B);
		ret = -EDOM;
		goto finally;
	}

finally:
	xleaf_put_leaf(xdd->pdev, xdd->leaf1);
	xleaf_put_leaf(xdd->pdev, xdd->leaf0);
	vfree(xdd);
	return ret;
}

static int selftest1_main_leaf_ioctl(struct platform_device *pdev, u32 cmd,
				     void *arg)
{
	struct selftest1_main *xmm = platform_get_drvdata(pdev);
	int ret = 0;

	xrt_info(pdev, "%p.ioctl(%d, %p)", xmm, cmd, arg);
	switch (cmd) {
	case XRT_XLEAF_EVENT:
		selftest1_main_event_cb(pdev, arg);
		break;
	default:
		xrt_err(pdev, "unknown cmd: %d", cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static ssize_t selftest1_main_leaf_read(struct file *file, char __user *ubuf,
				    size_t n, loff_t *off)
{
	int i;
	struct selftest1_main_client_data *xdd = file->private_data;

	for (i = 0; i < 4; i++) {
		xrt_info(xdd->pdev, "reading...");
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
		xrt_info(xdd->pdev, "writing %d...", i);
		ssleep(1);
	}
	return n;
}

static int selftest1_main_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xleaf_devnode_open(inode);
	struct selftest1_main_client_data *xdd;

	/* Device may have gone already when we get here. */
	if (!pdev)
		return -ENODEV;

	xrt_info(pdev, "opened");
	/* Obtain the reference to test xleaf nodes */
	xdd = selftest1_validate_ini(pdev);
	file->private_data = xdd;
	if (!xdd) {
		xrt_err(pdev, "FAILED test %s", SELFTEST1_MAIN);
		return -EDOM;
	}
	return 0;
}

static int selftest1_main_close(struct inode *inode, struct file *file)
{
	struct selftest1_main_client_data *xdd = file->private_data;
	struct platform_device *pdev = xdd->pdev;
	/* Perform inter xleaf ioctls and then release test node handles */
	int ret = selftest1_validate_fini(xdd);

	file->private_data = NULL;
	xleaf_devnode_close(inode);

	if (ret)
		xrt_err(pdev, "FAILED test %s", SELFTEST1_MAIN);
	else
		xrt_info(pdev, "PASSED test %s", SELFTEST1_MAIN);

	xrt_info(pdev, "closed");
	return 0;
}


static struct xrt_subdev_endpoints xrt_mgmt_main_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []){
			{ .ep_name = NODE_MGMT_MAIN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xrt_subdev_drvdata selftest1_main_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = selftest1_main_leaf_ioctl,
	},
	.xsd_file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = selftest1_main_open,
			.release = selftest1_main_close,
			.read = selftest1_main_leaf_read,
			.write = selftest1_main_leaf_write,
		},
		.xsf_dev_name = "selftest1",
	},
};

static const struct platform_device_id selftest1_main_id_table[] = {
	{ SELFTEST1_MAIN, (kernel_ulong_t)&selftest1_main_data },
	{ },
};

struct platform_driver selftest1_main_driver = {
	.driver	= {
		.name    = SELFTEST1_MAIN,
	},
	.probe   = selftest1_main_probe,
	.remove  = selftest1_main_remove,
	.id_table = selftest1_main_id_table,
};

int selftest1_main_register_leaf(void)
{
	return xleaf_register_external_driver(XRT_SUBDEV_MGMT_MAIN,
		&selftest1_main_driver, xrt_mgmt_main_endpoints);
}

void selftest1_main_unregister_leaf(void)
{
	xleaf_unregister_external_driver(XRT_SUBDEV_MGMT_MAIN);
}
