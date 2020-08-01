// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/vmalloc.h>

#include "xocl-root.h"
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

extern struct platform_driver xmgmt_main_driver;

static struct class *xmgmt_class;
static const struct pci_device_id xmgmt_pci_ids[] = {
	{ PCI_DEVICE(0x10EE, 0x5020), },
	{ 0, }
};

struct xmgmt {
	struct pci_dev *pdev;
	void *root;
};

static int xmgmt_config_pci(struct xmgmt *xm)
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

static int xmgmt_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;
	struct device *dev = DEV(pdev);
	struct xmgmt *xm = devm_kzalloc(dev, sizeof(*xm), GFP_KERNEL);

	if (!xm)
		return -ENOMEM;
	xm->pdev = pdev;
	pci_set_drvdata(pdev, xm);

	ret = xmgmt_config_pci(xm);
	if (ret)
		goto failed;

	ret = xroot_probe(pdev, &xm->root);
	if (ret)
		goto failed;

	return 0;

failed:
	pci_set_drvdata(pdev, NULL);
	return ret;
}

static void xmgmt_remove(struct pci_dev *pdev)
{
	struct xmgmt *xm = pci_get_drvdata(pdev);

	(void) xroot_remove(xm->root);
	pci_disable_pcie_error_reporting(xm->pdev);
}

static struct pci_driver xmgmt_driver = {
	.name = XMGMT_MODULE_NAME,
	.id_table = xmgmt_pci_ids,
	.probe = xmgmt_probe,
	.remove = xmgmt_remove,
};

static int __init xmgmt_init(void)
{
	int res = xocl_subdev_register_external_driver(XOCL_SUBDEV_MGMT_MAIN, &xmgmt_main_driver);

	if (res)
		return res;

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
	xocl_subdev_unregister_external_driver(XOCL_SUBDEV_MGMT_MAIN);
}

module_init(xmgmt_init);
module_exit(xmgmt_exit);

MODULE_DEVICE_TABLE(pci, xmgmt_pci_ids);
MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo management function driver");
MODULE_LICENSE("GPL v2");
