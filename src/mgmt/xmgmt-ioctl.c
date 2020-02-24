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

#include "xmgmt-drv.h"
#include "xocl-lib.h"
#include "xocl-devices.h"
#include "mgmt-ioctl.h"

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
		//result = bitstream_ioctl_axlf(lro, (void __user *)arg);
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
