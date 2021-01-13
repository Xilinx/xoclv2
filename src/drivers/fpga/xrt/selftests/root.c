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
#include <linux/delay.h>

#include "xroot.h"
#include "main-impl.h"
#include "metadata.h"

#define	XMGMT_MODULE_NAME	"xrt-test1"
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
#define	XMGMT_DEV_ID(pdev)			\
	((pci_domain_nr(pdev->bus) << 16) |	\
	PCI_DEVID(pdev->bus->number, 0))

static struct class *xmgmt_class;
static const struct pci_device_id xmgmt_pci_ids[] = {
	{ PCI_DEVICE(0x10EE, 0xd020), },
	{ PCI_DEVICE(0x10EE, 0x5020), },
	{ 0, }
};

struct xmgmt {
	struct pci_dev *pdev;
	struct xroot *root;

	/* save config for pci reset */
	u32 saved_config[8][16];
	bool ready;
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

static void xmgmt_root_hot_reset(struct pci_dev *pdev)
{
	struct xmgmt *xm = pci_get_drvdata(pdev);

	xmgmt_info(xm, "hot reset ignored");
}

static int xmgmt_create_root_metadata(struct xmgmt *xm, char **root_dtb)
{
	char *dtb = NULL;
	int ret;

	ret = xrt_md_create(XMGMT_DEV(xm), &dtb);
	if (ret) {
		xmgmt_err(xm, "create metadata failed, ret %d", ret);
		goto failed;
	}

	ret = xroot_add_simple_node(xm->root, dtb, NODE_TEST);
	if (ret)
		goto failed;

	*root_dtb = dtb;
	return 0;

failed:
	vfree(dtb);
	return ret;
}

static ssize_t ready_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xmgmt *xm = pci_get_drvdata(pdev);

	return sprintf(buf, "%d\n", xm->ready);
}
static DEVICE_ATTR_RO(ready);

static struct attribute *xmgmt_root_attrs[] = {
	&dev_attr_ready.attr,
	NULL
};

static struct attribute_group xmgmt_root_attr_group = {
	.attrs = xmgmt_root_attrs,
};

static struct xroot_pf_cb xmgmt_xroot_pf_cb = {
	.xpc_hot_reset = xmgmt_root_hot_reset,
};

static int xmgmt_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;
	struct device *dev = &(pdev->dev);
	struct xmgmt *xm = devm_kzalloc(dev, sizeof(*xm), GFP_KERNEL);
	char *dtb = NULL;

	if (!xm)
		return -ENOMEM;
	xm->pdev = pdev;
	pci_set_drvdata(pdev, xm);

	ret = xmgmt_config_pci(xm);
	if (ret)
		goto failed;

	ret = xroot_probe(pdev, &xmgmt_xroot_pf_cb, &xm->root);
	if (ret)
		goto failed;

	ret = xmgmt_create_root_metadata(xm, &dtb);
	if (ret)
		goto failed_metadata;

	ret = xroot_create_partition(xm->root, dtb);
	vfree(dtb);
	if (ret)
		xmgmt_err(xm, "failed to create root partition: %d", ret);

	if (!xroot_wait_for_bringup(xm->root))
		xmgmt_err(xm, "failed to bringup all partitions");
	else
		xm->ready = true;

	ret = sysfs_create_group(&pdev->dev.kobj, &xmgmt_root_attr_group);
	if (ret) {
		/* Warning instead of failing the probe. */
		xmgmt_warn(xm, "create xmgmt root attrs failed: %d", ret);
	}

	xroot_broadcast(xm->root, XRT_EVENT_POST_CREATION);
	xmgmt_info(xm, "%s started successfully", XMGMT_MODULE_NAME);
	return 0;

failed_metadata:
	(void) xroot_remove(xm->root);
failed:
	pci_set_drvdata(pdev, NULL);
	return ret;
}

static void xmgmt_remove(struct pci_dev *pdev)
{
	struct xmgmt *xm = pci_get_drvdata(pdev);

	xroot_broadcast(xm->root, XRT_EVENT_PRE_REMOVAL);
	sysfs_remove_group(&pdev->dev.kobj, &xmgmt_root_attr_group);
	(void) xroot_remove(xm->root);
	pci_disable_pcie_error_reporting(xm->pdev);
	xmgmt_info(xm, "%s cleaned up successfully", XMGMT_MODULE_NAME);
}

static struct pci_driver xmgmt_driver = {
	.name = XMGMT_MODULE_NAME,
	.id_table = xmgmt_pci_ids,
	.probe = xmgmt_probe,
	.remove = xmgmt_remove,
};

static int __init xmgmt_init(void)
{
	int res = 0;

	res = xmgmt_main_register_leaf();
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
	xmgmt_main_unregister_leaf();
}

module_init(xmgmt_init);
module_exit(xmgmt_exit);

MODULE_DEVICE_TABLE(pci, xmgmt_pci_ids);
MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo management function driver");
MODULE_LICENSE("GPL v2");
