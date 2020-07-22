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
	struct xocl_subdev_pool parts;
};

struct xmgmt_subdev_match_arg {
	enum xocl_subdev_id id;
	int instance;
};

static bool xmgmt_subdev_match(enum xocl_subdev_id id,
	struct platform_device *pdev, u64 arg)
{
	struct xmgmt_subdev_match_arg *a = (struct xmgmt_subdev_match_arg *)arg;
	return id == a->id && pdev->id == a->instance;
}

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

static int xmgmt_get_partition(struct xmgmt *xm, enum xocl_partition_id id,
	struct platform_device **partp)
{
	struct xmgmt_subdev_match_arg arg = { XOCL_SUBDEV_PART, id };
	int rc = xocl_subdev_pool_get(&xm->parts, xmgmt_subdev_match, (u64)&arg,
		DEV(xm->pdev), partp);

	if (rc && rc != -ENOENT)
		xmgmt_err(xm, "failed to hold partition %d: %d", id, rc);
	return rc;
}

static void xmgmt_put_partition(struct xmgmt *xm, struct platform_device *part)
{
	int inst = part->id;
	int rc = xocl_subdev_pool_put(&xm->parts, part, DEV(xm->pdev));
	if (rc)
		xmgmt_err(xm, "failed to release partition %d: %d", inst, rc);
}

static int xmgmt_create_partition(struct xmgmt *xm,
	enum xocl_partition_id id, void *dtb)
{
	struct platform_device *pdev = NULL;
	int ret = xocl_subdev_pool_add(&xm->parts,
		XOCL_SUBDEV_PART, id, xmgmt_parent_cb, dtb);

	if (ret)
		return ret;

	ret = xmgmt_get_partition(xm, id, &pdev);
	if (ret) {
		xocl_subdev_pool_del(&xm->parts, XOCL_SUBDEV_PART, id);
	} else {
		/* Now bring up all children in this partition. */
		(void) xocl_subdev_online(pdev);
		xmgmt_put_partition(xm, pdev);
	}
	return ret;
}

static int xmgmt_destroy_partition(struct xmgmt *xm, enum xocl_partition_id id)
{
	return xocl_subdev_pool_del(&xm->parts, XOCL_SUBDEV_PART, id);
}

static long xmgmt_get_leaf(struct xmgmt *xm,
	struct xocl_parent_ioctl_get_leaf *arg)
{
	int rc = -ENOENT;
	enum xocl_partition_id partid;
	struct platform_device *part;

	for (partid = XOCL_PART_BEGIN; rc == -ENOENT && partid < XOCL_PART_END;
		partid++) {
		rc = xmgmt_get_partition(xm, partid, &part);
		if (!rc) {
			rc = xocl_subdev_ioctl(part, XOCL_PARTITION_GET_LEAF,
				(u64)arg);
			xmgmt_put_partition(xm, part);
		}
	}
	return rc;
}

static long xmgmt_get_leaf_by_id(struct xmgmt *xm,
	struct xocl_parent_ioctl_get_leaf_by_id *arg)
{
	int rc = -ENOENT;
	struct xmgmt_subdev_match_arg marg = {
		arg->xpiglbi_id, arg->xpiglbi_instance };
	struct xocl_parent_ioctl_get_leaf glarg;

	glarg.xpigl_pdev = arg->xpiglbi_pdev;
	glarg.xpigl_match_cb = xmgmt_subdev_match;
	glarg.xpigl_match_arg = (u64)&marg;
	rc = xmgmt_get_leaf(xm, &glarg);
	if (!rc)
		arg->xpiglbi_leaf = glarg.xpigl_leaf;
	return rc;
}

static long xmgmt_put_leaf(struct xmgmt *xm,
	struct xocl_parent_ioctl_put_leaf *arg)
{
	int rc = -ENOENT;
	enum xocl_partition_id partid;
	struct platform_device *part;

	for (partid = XOCL_PART_BEGIN; rc == -ENOENT && partid < XOCL_PART_END;
		partid++) {
		rc = xmgmt_get_partition(xm, partid, &part);
		if (!rc) {
			rc = xocl_subdev_ioctl(part,
				XOCL_PARTITION_PUT_LEAF, (u64)arg);
			xmgmt_put_partition(xm, part);
		}
	}
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
	case XOCL_PARENT_GET_LEAF_BY_ID: {
		struct xocl_parent_ioctl_get_leaf_by_id *getleaf =
			(struct xocl_parent_ioctl_get_leaf_by_id *)arg;
		rc = xmgmt_get_leaf_by_id(xm, getleaf);
		break;
	}
	case XOCL_PARENT_PUT_LEAF: {
		struct xocl_parent_ioctl_put_leaf *putleaf =
			(struct xocl_parent_ioctl_put_leaf *)arg;
		rc = xmgmt_put_leaf(xm, putleaf);
		break;
	}
	case XOCL_PARENT_CREATE_PARTITION: {
		struct xocl_parent_ioctl_create_partition *part =
			(struct xocl_parent_ioctl_create_partition *)arg;
		rc = xmgmt_create_partition(xm,
			part->xpicp_id, part->xpicp_dtb);
		break;
	}
	case XOCL_PARENT_REMOVE_PARTITION:
		rc = xmgmt_destroy_partition(xm, arg);
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
	struct device *dev = DEV(pdev);

	dev_info(dev, "%s: probing...", __func__);

	xm = devm_kzalloc(dev, sizeof(*xm), GFP_KERNEL);
	if (!xm) {
		dev_err(dev, "failed to alloc xmgmt");
		return -ENOMEM;
	}
	xm->pdev = pdev;
	xocl_subdev_pool_init(DEV(xm->pdev), &xm->parts);

	xmgmt_config_pci(xm);
	pci_set_drvdata(pdev, xm);

	(void) xmgmt_create_partition(xm, XOCL_PART_TEST, NULL);
	return 0;
}

static void xmgmt_remove(struct pci_dev *pdev)
{
	struct xmgmt *xm = pci_get_drvdata(pdev);

	xmgmt_info(xm, "leaving...");
	(void) xocl_subdev_pool_fini(&xm->parts);
	pci_disable_pcie_error_reporting(pdev);
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
