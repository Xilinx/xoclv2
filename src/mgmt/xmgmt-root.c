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
#include "xocl-parent.h"
#include "xocl-partition.h"

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

static long xmgmt_parent_cb(struct device *, u32, u64);

struct xmgmt {
	struct pci_dev *pdev;

	struct list_head parts;
	struct mutex parts_lock;
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

static void xmgmt_add_partition(struct xmgmt *xm, struct xocl_subdev *sdev)
{
	mutex_lock(&xm->parts_lock);
	list_add(&sdev->xs_dev_list, &xm->parts);
	mutex_unlock(&xm->parts_lock);
}

static int xmgmt_create_partition(struct xmgmt *xm,
	enum xocl_partition_id id, void *dtb)
{
	const struct list_head *ptr;
	struct xocl_subdev *sdev = NULL;
	int ret = 0;

	list_for_each(ptr, &xm->parts) {
		sdev = list_entry(ptr, struct xocl_subdev, xs_dev_list);
		if (sdev->xs_pdev->id == id) {
			xmgmt_err(xm, "partition %d already exists", id);
			ret = -EEXIST;
			break;
		}
	}
	if (ret)
		return ret;

	sdev = xocl_subdev_create(&xm->pdev->dev, XOCL_SUBDEV_PART, id,
		xmgmt_parent_cb, dtb);
	if (sdev)
		xmgmt_add_partition(xm, sdev);
	else
		ret = -EINVAL;
	if (sdev) {
		/* Now bring up all children in this partition. */
		(void) xocl_subdev_online(sdev->xs_pdev);
	}

	return ret;
}

static long
xmgmt_get_leaf(struct xmgmt *xm, struct xocl_parent_ioctl_get_leaf *arg)
{
	struct xocl_subdev *sdev;
	struct list_head *ptr;
	long rc = -ENOENT;
	struct xocl_partition_ioctl_get_leaf getleaf = { 0 };

	getleaf.xpart_pdev = arg->xpigl_pdev;
	getleaf.xpart_id = arg->xpigl_id;
	getleaf.xpart_match_cb = arg->xpigl_match_cb;
	getleaf.xpart_match_arg = arg->xpigl_match_arg;

	mutex_lock(&xm->parts_lock);
	list_for_each(ptr, &xm->parts) {
		sdev = list_entry(ptr, struct xocl_subdev, xs_dev_list);
		rc = xocl_subdev_ioctl(sdev->xs_pdev,
			XOCL_PARTITION_GET_LEAF, (u64)&getleaf);
		if (rc != -ENOENT)
			break;
	}
	mutex_unlock(&xm->parts_lock);

	arg->xpigl_leaf = getleaf.xpart_leaf;

	return rc;
}

static long xmgmt_parent_cb(struct device *dev, u32 cmd, u64 arg)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct xmgmt *xm = pci_get_drvdata(pdev);
	long rc = 0;

	xmgmt_info(xm, "handling parent call, cmd %d", cmd);

	switch (cmd) {
	case XOCL_PARENT_GET_LEAF: {
		struct xocl_parent_ioctl_get_leaf *getleaf =
			(struct xocl_parent_ioctl_get_leaf *)arg;
		rc = xmgmt_get_leaf(xm, getleaf);
		break;
	}
	case XOCL_PARENT_PUT_LEAF:
		break;
	case XOCL_PARENT_CREATE_PARTITION: {
		struct xocl_parent_ioctl_create_partition *part =
			(struct xocl_parent_ioctl_create_partition *)arg;
		rc = xmgmt_create_partition(xm,
			part->xpicp_id, part->xpicp_dtb);
		break;
	}
	case XOCL_PARENT_REMOVE_PARTITION:
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
	struct xmgmt *xm;
	struct device *dev = &pdev->dev;

	dev_info(dev, "%s: probing...", __func__);

	xm = devm_kzalloc(&pdev->dev, sizeof(*xm), GFP_KERNEL);
	if (!xm) {
		dev_err(dev, "failed to alloc xmgmt");
		return -ENOMEM;
	}
	xm->pdev = pdev;
	INIT_LIST_HEAD(&xm->parts);
	mutex_init(&xm->parts_lock);

	xmgmt_config_pci(xm);
	pci_set_drvdata(pdev, xm);

	(void) xmgmt_create_partition(xm, XOCL_PART_TEST, NULL);
	return 0;
}

static void xmgmt_remove(struct pci_dev *pdev)
{
	struct xmgmt *xm = pci_get_drvdata(pdev);

	xmgmt_info(xm, "leaving...");
	pci_disable_pcie_error_reporting(pdev);
	mutex_lock(&xm->parts_lock);
	while (!list_empty(&xm->parts)) {
		struct xocl_subdev *sdev = list_first_entry(&xm->parts,
			struct xocl_subdev, xs_dev_list);
		list_del(&sdev->xs_dev_list);
		mutex_unlock(&xm->parts_lock);
		xocl_subdev_destroy(sdev);
		mutex_lock(&xm->parts_lock);
	}
	mutex_unlock(&xm->parts_lock);
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
