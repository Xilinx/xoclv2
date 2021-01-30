// SPDX-License-Identifier: GPL-2.0
/*
 * XRT driver infrastructure selftest 1
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *     Sonal Santan <sonal.santan@xilinx.com>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>

#include "xroot.h"
#include "main-impl.h"
#include "metadata.h"
#include "xleaf/test.h"

#define	SELFTEST1_MODULE_NAME	"xrt-selftest1"
#define	SELFTEST1_DRIVER_VERSION	"4.0.0"

#define	SELFTEST1_PDEV(xm)		((xm)->pdev)
#define	SELFTEST1_DEV(xm)		(&(SELFTEST1_PDEV(xm)->dev))
#define selftest1_err(xm, fmt, args...)	\
	dev_err(SELFTEST1_DEV(xm), "%s: " fmt, __func__, ##args)
#define selftest1_warn(xm, fmt, args...)	\
	dev_warn(SELFTEST1_DEV(xm), "%s: " fmt, __func__, ##args)
#define selftest1_info(xm, fmt, args...)	\
	dev_info(SELFTEST1_DEV(xm), "%s: " fmt, __func__, ##args)
#define selftest1_dbg(xm, fmt, args...)	\
	dev_dbg(SELFTEST1_DEV(xm), "%s: " fmt, __func__, ##args)
#define	SELFTEST1_DEV_ID(_pdev)			\
	({ typeof(_pdev) pdev = (_pdev);	\
	((pci_domain_nr(pdev->bus) << 16) |	\
	PCI_DEVID(pdev->bus->number, 0)); })

static struct class *selftest1_class;
static const struct pci_device_id selftest1_pci_ids[] = {
	{ PCI_DEVICE(0x10EE, 0xd020), },
	{ PCI_DEVICE(0x10EE, 0x5020), },
	{ 0, }
};

struct selftest1 {
	struct pci_dev *pdev;
	struct xroot *root;
	bool ready;
};

static void selftest1_root_hot_reset(struct pci_dev *pdev)
{
	struct selftest1 *xm = pci_get_drvdata(pdev);

	selftest1_info(xm, "hot reset ignored");
}

static int selftest1_create_root_metadata(struct selftest1 *xm, char **root_dtb, const char *ep)
{
	char *dtb = NULL;
	int ret;

	ret = xrt_md_create(SELFTEST1_DEV(xm), &dtb);
	if (ret) {
		selftest1_err(xm, "create metadata failed, ret %d", ret);
		goto failed;
	}

	ret = xroot_add_simple_node(xm->root, dtb, ep);
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
	struct selftest1 *xm = pci_get_drvdata(pdev);

	return sprintf(buf, "%d\n", xm->ready);
}
static DEVICE_ATTR_RO(ready);

static struct attribute *selftest1_root_attrs[] = {
	&dev_attr_ready.attr,
	NULL
};

static struct attribute_group selftest1_root_attr_group = {
	.attrs = selftest1_root_attrs,
};

static struct xroot_pf_cb selftest1_xroot_pf_cb = {
	.xpc_hot_reset = selftest1_root_hot_reset,
};

static int selftest1_create_group(struct selftest1 *xm, const char *ep)
{
	char *dtb = NULL;
	int ret = selftest1_create_root_metadata(xm, &dtb, ep);

	if (ret)
		return ret;

	ret = xroot_create_group(xm->root, dtb);
	vfree(dtb);
	if (ret < 0)
		selftest1_err(xm, "failed to create root group: %d", ret);
	return 0;
}

/*
 * As part of the probe the following hierarchy is built from synthetic
 * device tree fragments:
 *                          +-----------+
 *                          |   selftest1   |
 *                          +-----+-----+
 *                                |
 *           +--------------------+--------------------+
 *           |                    |                    |
 *           v                    v                    v
 *      +--------+           +--------+            +--------+
 *      | group0 |           | group1 |            | group2 |
 *      +----+---+           +----+---+            +---+----+
 *           |                    |                    |
 *           |                    |                    |
 *           v                    v                    v
 *      +---------+          +---------+          +-----------+
 *      | test[0] |          | test[1] |          | mgmt_main |
 *      +---------+          +---------+          +-----------+
 */
static int selftest1_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct selftest1 *xm = devm_kzalloc(dev, sizeof(*xm), GFP_KERNEL);

	if (!xm)
		return -ENOMEM;
	xm->pdev = pdev;
	pci_set_drvdata(pdev, xm);

	ret = xroot_probe(pdev, &selftest1_xroot_pf_cb, &xm->root);
	if (ret)
		goto failed;

	ret = selftest1_create_group(xm, NODE_TEST);

	if (ret)
		goto failed_metadata;

	ret = selftest1_create_group(xm, NODE_TEST);

	if (ret)
		goto failed_metadata;

	ret = selftest1_create_group(xm, NODE_MGMT_MAIN);

	if (ret)
		goto failed_metadata;

	if (!xroot_wait_for_bringup(xm->root))
		selftest1_err(xm, "failed to bringup all groups");
	else
		xm->ready = true;

	ret = sysfs_create_group(&pdev->dev.kobj, &selftest1_root_attr_group);
	if (ret) {
		/* Warning instead of failing the probe. */
		selftest1_warn(xm, "create selftest1 root attrs failed: %d", ret);
	}

	xroot_broadcast(xm->root, XRT_EVENT_POST_CREATION);
	selftest1_info(xm, "%s started successfully", SELFTEST1_MODULE_NAME);
	return 0;

failed_metadata:
	(void)xroot_remove(xm->root);
failed:
	pci_set_drvdata(pdev, NULL);
	return ret;
}

static void selftest1_remove(struct pci_dev *pdev)
{
	struct selftest1 *xm = pci_get_drvdata(pdev);

	xroot_broadcast(xm->root, XRT_EVENT_PRE_REMOVAL);
	sysfs_remove_group(&pdev->dev.kobj, &selftest1_root_attr_group);
	(void)xroot_remove(xm->root);
	selftest1_info(xm, "%s cleaned up successfully", SELFTEST1_MODULE_NAME);
}

static struct pci_driver selftest1_driver = {
	.name = SELFTEST1_MODULE_NAME,
	.id_table = selftest1_pci_ids,
	.probe = selftest1_probe,
	.remove = selftest1_remove,
};

static int __init selftest1_init(void)
{
	int res = 0;

	res = selftest1_main_register_leaf();
	if (res)
		return res;

	res = selftest_test_register_leaf();
	if (res)
		return res;

	selftest1_class = class_create(THIS_MODULE, SELFTEST1_MODULE_NAME);
	if (IS_ERR(selftest1_class))
		return PTR_ERR(selftest1_class);

	res = pci_register_driver(&selftest1_driver);
	if (res) {
		class_destroy(selftest1_class);
		return res;
	}

	return 0;
}

static __exit void selftest1_exit(void)
{
	pci_unregister_driver(&selftest1_driver);
	class_destroy(selftest1_class);
	selftest_test_unregister_leaf();
	selftest1_main_unregister_leaf();
}

module_init(selftest1_init);
module_exit(selftest1_exit);

MODULE_DEVICE_TABLE(pci, selftest1_pci_ids);
MODULE_VERSION(SELFTEST1_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx XRT selftest driver");
MODULE_LICENSE("GPL v2");
