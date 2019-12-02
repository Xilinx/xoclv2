// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Xilinx, Inc. All rights reserved.
 *
 * Authors: Sonal.Santan@Xilinx.com
 */

#ifndef	_XMGMT_ALVEO_DRV_H_
#define	_XMGMT_ALVEO_DRV_H_

#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/pci.h>

#define	XMGMT_MODULE_NAME	"xmgmt"
#define	ICAP_XCLBIN_V2		"xclbin2"
#define XMGMT_MAX_DEVICES	24
#define MGMT_DEFAULT            0x000e
#define XRT_DRIVER_VERSION      "4.0.0"

#define xrt_err(dev, fmt, args...)			\
	dev_err(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)
#define xrt_warn(dev, fmt, args...)			\
	dev_warn(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)
#define xrt_info(dev, fmt, args...)			\
	dev_info(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)
#define xrt_dbg(dev, fmt, args...)			\
	dev_dbg(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)

#define	XRT_DEV_ID(pdev)			\
	((pci_domain_nr(pdev->bus) << 16) |	\
	PCI_DEVID(pdev->bus->number, pdev->devfn))

struct xrt_drvinst {
	struct device		*dev;
	u32			size;
	atomic_t		ref;
	bool			offline;
        /*
	 * The derived object placed inline in field "data"
	 * should be aligned at 8 byte boundary
	 */
        u64			data[1];
};

struct xmgmt_dev;

struct xmgmt_char {
	struct xmgmt_dev *lro;
	struct cdev *cdev;
	struct device *sys_device;
};

struct xmgmt_dev {
	/* the kernel pci device data structure provided by probe() */
	struct pci_dev *pdev;
        int			dev_minor;
	int instance;
	struct xmgmt_char user_char_dev;
        bool ready;
};

void *xrt_drvinst_alloc(struct device *dev, u32 size);
void xrt_drvinst_free(void *data);

#endif
