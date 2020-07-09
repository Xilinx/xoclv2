// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2019-2020 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/ioctl.h>
#include <linux/aer.h>
#include <linux/platform_device.h>
#include <linux/fpga/fpga-mgr.h>

#include "xmgmt-drv.h"
#include "xocl-lib.h"
#include "xocl-devices.h"
#include "mgmt-ioctl.h"
#include "xclbin.h"

static int bitstream_ioctl_axlf(struct xmgmt_dev *lro, const void __user *arg)
{
	struct fpga_image_info info;
	void *copy_buffer = NULL;
	size_t copy_buffer_size = 0;
	struct xclmgmt_ioc_bitstream_axlf ioc_obj = { 0 };
	struct axlf xclbin_obj = { {0} };
	struct fpga_manager *fmgr = platform_get_drvdata(lro->fmgr);
	int ret = 0;

	if (copy_from_user((void *)&ioc_obj, arg, sizeof(ioc_obj)))
		return -EFAULT;
	if (copy_from_user((void *)&xclbin_obj, ioc_obj.xclbin,
		sizeof(xclbin_obj)))
		return -EFAULT;
	if (memcmp(xclbin_obj.m_magic, ICAP_XCLBIN_V2, sizeof(ICAP_XCLBIN_V2)))
		return -EINVAL;

	copy_buffer_size = xclbin_obj.m_header.m_length;
	/* Assuming xclbin is not over 1G */
	if (copy_buffer_size > 1024 * 1024 * 1024)
		return -EINVAL;
	copy_buffer = vmalloc(copy_buffer_size);
	if (copy_buffer == NULL)
		return -ENOMEM;

	if (copy_from_user(copy_buffer, ioc_obj.xclbin, copy_buffer_size))
		ret = -EFAULT;
	else {
		info.buf = (char *)copy_buffer;
		info.count = copy_buffer_size;
		ret = fpga_mgr_load(fmgr, &info);
	}
	vfree(copy_buffer);
	return ret;
}

long xmgmt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct xmgmt_dev *lro = (struct xmgmt_dev *)file->private_data;
	int result = 0;

	printk(KERN_INFO "mgmgt ioctl called. \n");

	BUG_ON(!lro);

	if (!lro->ready || _IOC_TYPE(cmd) != XCLMGMT_IOC_MAGIC)
		return -ENOTTY;

	if (_IOC_DIR(cmd) & _IOC_READ)
		result = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		result = !access_ok((void __user *)arg, _IOC_SIZE(cmd));

	if (result)
		return -EFAULT;

	switch (cmd) {
	case XCLMGMT_IOCINFO:
		printk(KERN_INFO "mgmgt INFO ioctl called. \n");
		//result = version_ioctl(lro, (void __user *)arg);
		break;
	case XCLMGMT_IOCICAPDOWNLOAD:
		printk(KERN_INFO "mgmgt ICAP ioctl called. \n");
		//result = bitstream_ioctl(lro, (void __user *)arg);
		break;
	case XCLMGMT_IOCICAPDOWNLOAD_AXLF:
		printk(KERN_INFO "mgmgt axlf ioctl called. \n");
		result = bitstream_ioctl_axlf(lro, (void __user *)arg);
		break;
	case XCLMGMT_IOCFREQSCALE:
		//result = ocl_freqscaling_ioctl(lro, (void __user *)arg);
		break;
	case XCLMGMT_IOCREBOOT:
		result = -EINVAL;
		break;
	case XCLMGMT_IOCERRINFO:
		result = -EINVAL;
		break;
	default:
		result = -ENOTTY;
	}
	return result;
}
