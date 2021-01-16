// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	 Sonal Santan <sonal.santan@xilinx.com>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>

#include "xroot.h"
#include "main-impl.h"
#include "metadata.h"

#define	TEST1_MODULE_NAME	"xrt-test1"
#define	TEST1_DRIVER_VERSION	"4.0.0"

#define	TEST1_PDEV(xm)		((xm)->pdev)
#define	TEST1_DEV(xm)		(&(TEST1_PDEV(xm)->dev))
#define test1_err(xm, fmt, args...)	\
	dev_err(TEST1_DEV(xm), "%s: " fmt, __func__, ##args)
#define test1_warn(xm, fmt, args...)	\
	dev_warn(TEST1_DEV(xm), "%s: " fmt, __func__, ##args)
#define test1_info(xm, fmt, args...)	\
	dev_info(TEST1_DEV(xm), "%s: " fmt, __func__, ##args)
#define test1_dbg(xm, fmt, args...)	\
	dev_dbg(TEST1_DEV(xm), "%s: " fmt, __func__, ##args)
#define	TEST1_DEV_ID(pdev)			\
	((pci_domain_nr(pdev->bus) << 16) |	\
	PCI_DEVID(pdev->bus->number, 0))

static struct class *test1_class;
static const struct pci_device_id test1_pci_ids[] = {
	{ PCI_DEVICE(0x10EE, 0xd020), },
	{ PCI_DEVICE(0x10EE, 0x5020), },
	{ 0, }
};

struct test1 {
	struct pci_dev *pdev;
	struct xroot *root;

	/* save config for pci reset */
	u32 saved_config[8][16];
	bool ready;
};

static int test1_config_pci(struct test1 *xm)
{
	struct pci_dev *pdev = TEST1_PDEV(xm);
	int rc;

	rc = pcim_enable_device(pdev);
	if (rc < 0) {
		test1_err(xm, "failed to enable device: %d", rc);
		return rc;
	}

	rc = pci_enable_pcie_error_reporting(pdev);
	if (rc)
		test1_warn(xm, "failed to enable AER: %d", rc);

	pci_set_master(pdev);

	rc = pcie_get_readrq(pdev);
	if (rc < 0) {
		test1_err(xm, "failed to read mrrs %d", rc);
		return rc;
	}
	if (rc > 512) {
		rc = pcie_set_readrq(pdev, 512);
		if (rc) {
			test1_err(xm, "failed to force mrrs %d", rc);
			return rc;
		}
	}

	return 0;
}

static void test1_root_hot_reset(struct pci_dev *pdev)
{
	struct test1 *xm = pci_get_drvdata(pdev);

	test1_info(xm, "hot reset ignored");
}

static int test1_create_root_metadata(struct test1 *xm, char **root_dtb)
{
	char *dtb = NULL;
	int ret;

	ret = xrt_md_create(TEST1_DEV(xm), &dtb);
	if (ret) {
		test1_err(xm, "create metadata failed, ret %d", ret);
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
	struct test1 *xm = pci_get_drvdata(pdev);

	return sprintf(buf, "%d\n", xm->ready);
}
static DEVICE_ATTR_RO(ready);

static struct attribute *test1_root_attrs[] = {
	&dev_attr_ready.attr,
	NULL
};

static struct attribute_group test1_root_attr_group = {
	.attrs = test1_root_attrs,
};

static struct xroot_pf_cb test1_xroot_pf_cb = {
	.xpc_hot_reset = test1_root_hot_reset,
};


static int test1_create_group(struct test1 *xm)
{
	char *dtb = NULL;
	int ret = test1_create_root_metadata(xm, &dtb);

	if (ret)
		return ret;

	ret = xroot_create_group(xm->root, dtb);
	vfree(dtb);
	if (ret < 0)
		test1_err(xm, "failed to create root group: %d", ret);
	return 0;
}

static int test1_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;
	struct device *dev = &(pdev->dev);
	struct test1 *xm = devm_kzalloc(dev, sizeof(*xm), GFP_KERNEL);

	if (!xm)
		return -ENOMEM;
	xm->pdev = pdev;
	pci_set_drvdata(pdev, xm);

	ret = test1_config_pci(xm);
	if (ret)
		goto failed;

	ret = xroot_probe(pdev, &test1_xroot_pf_cb, &xm->root);
	if (ret)
		goto failed;

	ret = test1_create_group(xm);

	if (ret)
		goto failed_metadata;

	ret = test1_create_group(xm);

	if (ret)
		goto failed_metadata;

	if (!xroot_wait_for_bringup(xm->root))
		test1_err(xm, "failed to bringup all groups");
	else
		xm->ready = true;

	ret = sysfs_create_group(&pdev->dev.kobj, &test1_root_attr_group);
	if (ret) {
		/* Warning instead of failing the probe. */
		test1_warn(xm, "create test1 root attrs failed: %d", ret);
	}

	xroot_broadcast(xm->root, XRT_EVENT_POST_CREATION);
	test1_info(xm, "%s started successfully", TEST1_MODULE_NAME);
	return 0;

failed_metadata:
	(void) xroot_remove(xm->root);
failed:
	pci_set_drvdata(pdev, NULL);
	return ret;
}

static void test1_remove(struct pci_dev *pdev)
{
	struct test1 *xm = pci_get_drvdata(pdev);

	xroot_broadcast(xm->root, XRT_EVENT_PRE_REMOVAL);
	sysfs_remove_group(&pdev->dev.kobj, &test1_root_attr_group);
	(void) xroot_remove(xm->root);
	pci_disable_pcie_error_reporting(xm->pdev);
	test1_info(xm, "%s cleaned up successfully", TEST1_MODULE_NAME);
}

static struct pci_driver test1_driver = {
	.name = TEST1_MODULE_NAME,
	.id_table = test1_pci_ids,
	.probe = test1_probe,
	.remove = test1_remove,
};

static int __init test1_init(void)
{
	int res = 0;

	res = test1_main_register_leaf();
	if (res)
		return res;

	test1_class = class_create(THIS_MODULE, TEST1_MODULE_NAME);
	if (IS_ERR(test1_class))
		return PTR_ERR(test1_class);

	res = pci_register_driver(&test1_driver);
	if (res) {
		class_destroy(test1_class);
		return res;
	}

	return 0;
}

static __exit void test1_exit(void)
{
	pci_unregister_driver(&test1_driver);
	class_destroy(test1_class);
	test1_main_unregister_leaf();
}

module_init(test1_init);
module_exit(test1_exit);

MODULE_DEVICE_TABLE(pci, test1_pci_ids);
MODULE_VERSION(TEST1_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx XRT selftest driver");
MODULE_LICENSE("GPL v2");
