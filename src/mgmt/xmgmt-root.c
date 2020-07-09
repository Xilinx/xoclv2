// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include "xocl-subdev.h"

#define	XMGMT_MODULE_NAME	"xmgmt"
#define	XMGMT_DRIVER_VERSION	"4.0.0"
#define	XMGMT_PDEV(xm)		((xm)->pdev)
#define	XMGMT_DEV(xm)		(&(XMGMT_PDEV(xm)->dev))

#define xmgmt_err(xm, fmt, args...)	\
	dev_err(XMGMT_DEV(xm), "%s: " fmt, __func__, ##args)
#define xmgmt_warn(xm, fmt, args...)	\
	dev_warn(XMGMT_DEV(xm), "%s: " fmt, __func__, ##args)
#define xmgmt_info(xm, fmt, args...)	\
	dev_info(XMGMT_DEV(xm), "%s: " fmt, __func__, ##args)
#define xmgmt_dbg(xm, fmt, args...)	\
	dev_dbg(XMGMT_DEV(xm), "%s: " fmt, __func__, ##args)

static struct class *xmgmt_class;
static const struct pci_device_id xmgmt_pci_ids[] = {
	{ PCI_DEVICE(0x10EE, 0x5000), },
	{ PCI_DEVICE(0x10EE, 0x5020), },
	{ 0, }
};

struct xmgmt_dev {
	struct pci_dev *pdev;
	struct list_head parts;
};

static int xmgmt_config_pci(struct xmgmt_dev *xm)
{
	struct pci_dev *pdev = XMGMT_PDEV(xm);
	int rc;

	rc = pcim_enable_device(pdev);
	if (rc < 0) {
		xmgmt_err(xm, "failed to enable device: %d", rc);
		return rc;
	}

	rc = pci_enable_pcie_error_reporting(pdev);
	if (rc)
		xmgmt_warn(xm, "failed to enable AER: %d", rc);

	pci_set_master(pdev);

	rc = pcie_get_readrq(pdev);
	if (rc < 0) {
		xmgmt_err(xm, "failed to read mrrs %d", rc);
		return rc;
	}
	if (rc > 512) {
		rc = pcie_set_readrq(pdev, 512);
		if (rc) {
			xmgmt_err(xm, "failed to force mrrs %d", rc);
			return rc;
		}
	}

	return 0;
}

static long xmgmt_parent_cb(struct device *dev, u32 cmd, u64 arg)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct xmgmt_dev *xm = pci_get_drvdata(pdev);
	struct list_head *ptr;
	struct xocl_subdev *sdev;
	long rc = 0;

	xmgmt_info(xm, "handling parent call, cmd %d", cmd);

	switch (cmd) {
	case XOCL_PARENT_GET_LEAF:
		rc = -ENOENT;
		list_for_each(ptr, &xm->parts) {
			sdev = list_entry(ptr, struct xocl_subdev, xs_dev_list);
			rc = xocl_subdev_ioctl(sdev->xs_pdev, cmd, arg);
			if (rc != -ENOENT)
				break;
		}
		break;
	default:
		xmgmt_err(xm, "unknown IOCTL cmd %d", cmd);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int xmgmt_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct xmgmt_dev *xm;
	struct device *dev = &pdev->dev;
	struct xocl_subdev *sdev;

	dev_info(dev, "%s: probing...", __func__);

	xm = devm_kzalloc(&pdev->dev, sizeof(*xm), GFP_KERNEL);
	if (!xm) {
		dev_err(dev, "failed to alloc xmgmt_dev");
		return -ENOMEM;
	}
	xm->pdev = pdev;
	INIT_LIST_HEAD(&xm->parts);

	xmgmt_config_pci(xm);
	pci_set_drvdata(pdev, xm);

	sdev = xocl_subdev_create_partition(pdev, XOCL_PART_TEST,
		xmgmt_parent_cb, NULL, 0);
	if (sdev)
		list_add(&sdev->xs_dev_list, &xm->parts);

	return 0;
}

static void xmgmt_remove(struct pci_dev *pdev)
{
	struct xmgmt_dev *xm = pci_get_drvdata(pdev);

	xmgmt_info(xm, "leaving...");
	pci_disable_pcie_error_reporting(pdev);
	while (!list_empty(&xm->parts)) {
		struct xocl_subdev *sdev = list_first_entry(&xm->parts,
			struct xocl_subdev, xs_dev_list);
		list_del(&sdev->xs_dev_list);
		xocl_subdev_destroy(sdev);
	}
}

static struct pci_driver xmgmt_driver = {
	.name = XMGMT_MODULE_NAME,
	.id_table = xmgmt_pci_ids,
	.probe = xmgmt_probe,
	.remove = xmgmt_remove,
};

static int __init xmgmt_init(void)
{
	int res;

	xmgmt_class = class_create(THIS_MODULE, XMGMT_MODULE_NAME);
	if (IS_ERR(xmgmt_class))
		return PTR_ERR(xmgmt_class);

	res = pci_register_driver(&xmgmt_driver);
	if (res) {
		class_destroy(xmgmt_class);
		return res;
	}

	return 0;
}

static __exit void xmgmt_exit(void)
{
	pci_unregister_driver(&xmgmt_driver);
	class_destroy(xmgmt_class);
}

module_init(xmgmt_init);
module_exit(xmgmt_exit);

MODULE_DEVICE_TABLE(pci, xmgmt_pci_ids);
MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo management function driver");
MODULE_LICENSE("GPL v2");
