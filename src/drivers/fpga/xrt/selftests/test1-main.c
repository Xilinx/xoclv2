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
#include "xclbin-helper.h"
#include "metadata.h"
#include "xleaf/flash.h"
#include "xleaf/gpio.h"
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

static int
test1_main_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
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

static int test1_main_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xleaf_devnode_open(inode);

	/* Device may have gone already when we get here. */
	if (!pdev)
		return -ENODEV;

	xrt_info(pdev, "opened");
	file->private_data = platform_get_drvdata(pdev);
	return 0;
}

static int test1_main_close(struct inode *inode, struct file *file)
{
	struct test1_main *xmm = file->private_data;

	xleaf_devnode_close(inode);

	xrt_info(xmm->pdev, "closed");
	return 0;
}

/*
 * Called for xclbin download by either: xclbin load ioctl or
 * peer request from the userpf driver over mailbox.
 */
static int test1_bitstream_axlf_fpga_mgr(struct test1_main *xmm,
	void *axlf, size_t size)
{
	int ret;

	BUG_ON(!mutex_is_locked(&xmm->busy_mutex));

	/*
	 * Should any error happens during download, we can't trust
	 * the cached xclbin any more.
	 */
	return ret;
}


static int bitstream_axlf_ioctl(struct test1_main *xmm, const void __user *arg)
{
	void *copy_buffer = NULL;
	size_t copy_buffer_size = 0;
	struct xmgmt_ioc_bitstream_axlf ioc_obj = { 0 };
	struct axlf xclbin_obj = { {0} };
	int ret = 0;

	if (copy_from_user((void *)&ioc_obj, arg, sizeof(ioc_obj)))
		return -EFAULT;
	if (copy_from_user((void *)&xclbin_obj, ioc_obj.xclbin,
		sizeof(xclbin_obj)))
		return -EFAULT;
	if (memcmp(xclbin_obj.m_magic, ICAP_XCLBIN_V2, sizeof(ICAP_XCLBIN_V2)))
		return -EINVAL;

	copy_buffer_size = xclbin_obj.m_header.m_length;
	if (copy_buffer_size > MAX_XCLBIN_SIZE)
		return -EINVAL;
	copy_buffer = vmalloc(copy_buffer_size);
	if (copy_buffer == NULL)
		return -ENOMEM;

	if (copy_from_user(copy_buffer, ioc_obj.xclbin, copy_buffer_size)) {
		vfree(copy_buffer);
		return -EFAULT;
	}

	ret = test1_bitstream_axlf_fpga_mgr(xmm, copy_buffer, copy_buffer_size);
	if (ret)
		vfree(copy_buffer);

	return ret;
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
		result = bitstream_axlf_ioctl(xmm, (const void __user *)arg);
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
