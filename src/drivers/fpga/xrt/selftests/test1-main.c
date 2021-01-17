// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA MGMT PF entry point driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
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

#define	TEST1_MAIN "xrt-test1-main"

struct test1_main {
	struct platform_device *pdev;
	struct mutex busy_mutex;
};

struct test1_main_client_data {
	struct platform_device *pdev;
	struct platform_device *leaf0;
	struct platform_device *leaf1;
};

static void test1_main_event_cb(struct platform_device *pdev, void *arg)
{
	struct test1_main *xmm = platform_get_drvdata(pdev);
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id = evt->xe_subdev.xevt_subdev_id;

	xrt_info(pdev, "%p.event(%d, %p) %d", xmm, e, evt, id);
	switch (e) {
	case XRT_EVENT_POST_CREATION:
		/* mgmt driver finishes attaching, notify user pf. */
		break;
	case XRT_EVENT_PRE_REMOVAL:
		/* mgmt driver is about to detach, notify user pf. */
		break;
	default:
		xrt_dbg(pdev, "ignored event %d", e);
		break;
	}
}

static int test1_main_probe(struct platform_device *pdev)
{
	struct test1_main *xmm;

	xrt_info(pdev, "probing...");

	xmm = devm_kzalloc(DEV(pdev), sizeof(*xmm), GFP_KERNEL);
	if (!xmm)
		return -ENOMEM;

	xmm->pdev = pdev;
	platform_set_drvdata(pdev, xmm);
	mutex_init(&xmm->busy_mutex);

	return 0;
}

static int test1_main_remove(struct platform_device *pdev)
{
	xrt_info(pdev, "leaving...");
	return 0;
}

static struct test1_main_client_data *test1_validate_ini(struct platform_device *pdev)
{
	struct test1_main_client_data *xdd = vzalloc(sizeof(struct test1_main_client_data));
	xdd->pdev = pdev;
	xdd->leaf0 = xleaf_get_leaf_by_id(pdev, XRT_SUBDEV_TEST, 0);
	if (!xdd->leaf0) {
		xrt_err(pdev, "Cannot find xleaf test instance[0]");
		return xdd;
	}
	xdd->leaf1 = xleaf_get_leaf_by_id(pdev, XRT_SUBDEV_TEST, 1);
	if (!xdd->leaf1) {
		xrt_err(pdev, "Cannot find xleaf test instance[1]");
		xleaf_put_leaf(xdd->pdev, xdd->leaf0);
		return xdd;
	}

	xrt_info(pdev, "xleaf test instance[0] %p", xdd->leaf0);
	xrt_info(pdev, "xleaf test instance[1] %p", xdd->leaf1);
	return xdd;
}

static int test1_validate_fini(struct test1_main_client_data *xdd)
{
	int ret;
	struct xrt_xleaf_test_payload arg_a = {uuid_null, "FPGA"};
	struct xrt_xleaf_test_payload arg_b = {uuid_null, "FPGA"};

	if (!xdd->leaf1)
		goto finally;

	generate_random_uuid(arg_a.dummy1.b);
	generate_random_uuid(arg_b.dummy1.b);
	ret = xleaf_ioctl(xdd->leaf0, XRT_XLEAF_TEST_A, &arg_a);
	if (ret || !uuid_is_null(&arg_a.dummy1) || strcmp(arg_a.dummy2, "alveo")) {
		xrt_err(xdd->pdev, "xleaf test instance[0] %p ioctl %d failed", xdd->leaf1, XRT_XLEAF_TEST_A);
		ret = -EDOM;
		goto error;
	}
	ret = xleaf_ioctl(xdd->leaf1, XRT_XLEAF_TEST_B, &arg_b);
	if (ret || !uuid_is_null(&arg_b.dummy1) || strcmp(arg_b.dummy2, "alveo")) {
		xrt_err(xdd->pdev, "xleaf test instance[1] %p ioctl %d failed", xdd->leaf1, XRT_XLEAF_TEST_B);
		ret = -EDOM;
		goto error;
	}
error:
	xleaf_put_leaf(xdd->pdev, xdd->leaf1);
	xleaf_put_leaf(xdd->pdev, xdd->leaf0);
finally:
	vfree(xdd);
	return ret;
}

static int test1_main_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct test1_main *xmm = platform_get_drvdata(pdev);
	int ret = 0;

	xrt_info(pdev, "%p.ioctl(%d, %p)", xmm, cmd, arg);
	switch (cmd) {
	case XRT_XLEAF_EVENT:
		test1_main_event_cb(pdev, arg);
		break;
	case XRT_MGMT_MAIN_GET_AXLF_SECTION:
		break;
	case XRT_MGMT_MAIN_GET_VBNV:
		break;
	default:
		xrt_err(pdev, "unknown cmd: %d", cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static ssize_t
test1_main_leaf_read(struct file *file, char __user *ubuf, size_t n, loff_t *off)
{
	int i;
	struct test1_main_client_data *xdd = file->private_data;

	for (i = 0; i < 4; i++) {
		xrt_info(xdd->pdev, "reading...");
		ssleep(1);
	}
	return n;
}

static ssize_t
test1_main_leaf_write(struct file *file, const char __user *ubuf, size_t n, loff_t *off)
{
	int i;
	struct test1_main_client_data *xdd = file->private_data;

	for (i = 0; i < 4; i++) {
		xrt_info(xdd->pdev, "writing %d...", i);
		ssleep(1);
	}
	return n;
}

static int test1_main_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xleaf_devnode_open(inode);
	struct test1_main_client_data *xdd;

	/* Device may have gone already when we get here. */
	if (!pdev)
		return -ENODEV;

	xrt_info(pdev, "opened");
	xdd = test1_validate_ini(pdev);
	file->private_data = xdd;
	return xdd->leaf1 ? 0 : -EDOM;
}

static int test1_main_close(struct inode *inode, struct file *file)
{
	struct test1_main_client_data *xdd = file->private_data;
	struct platform_device *pdev = xdd->pdev;

	test1_validate_fini(xdd);
	file->private_data = NULL;
	xleaf_devnode_close(inode);

	xrt_info(pdev, "closed");
	return 0;
}

static long test1_main_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg)
{
	long result = 0;
	struct test1_main *xmm = filp->private_data;

	BUG_ON(!xmm);

	if (_IOC_TYPE(cmd) != XMGMT_IOC_MAGIC)
		return -ENOTTY;

	mutex_lock(&xmm->busy_mutex);

	xrt_info(xmm->pdev, "ioctl cmd %d, arg %ld", cmd, arg);
	switch (cmd) {
	case XMGMT_IOCICAPDOWNLOAD_AXLF:
		break;
	default:
		result = -ENOTTY;
		break;
	}

	mutex_unlock(&xmm->busy_mutex);
	return result;
}

struct xrt_subdev_endpoints xrt_mgmt_main_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []){
			{ .ep_name = NODE_MGMT_MAIN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xrt_subdev_drvdata test1_main_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = test1_main_leaf_ioctl,
	},
	.xsd_file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = test1_main_open,
			.release = test1_main_close,
			.unlocked_ioctl = test1_main_ioctl,
			.read = test1_main_leaf_read,
			.write = test1_main_leaf_write,
		},
		.xsf_dev_name = "test1",
	},
};

static const struct platform_device_id test1_main_id_table[] = {
	{ TEST1_MAIN, (kernel_ulong_t)&test1_main_data },
	{ },
};

struct platform_driver test1_main_driver = {
	.driver	= {
		.name    = TEST1_MAIN,
	},
	.probe   = test1_main_probe,
	.remove  = test1_main_remove,
	.id_table = test1_main_id_table,
};

int test1_main_register_leaf(void)
{
	return xleaf_register_external_driver(XRT_SUBDEV_MGMT_MAIN,
		&test1_main_driver, xrt_mgmt_main_endpoints);
}

void test1_main_unregister_leaf(void)
{
	xleaf_unregister_external_driver(XRT_SUBDEV_MGMT_MAIN);
}
