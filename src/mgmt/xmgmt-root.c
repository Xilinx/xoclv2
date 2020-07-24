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
	{ PCI_DEVICE(0x10EE, 0x5020), },
	{ 0, }
};

static int xmgmt_parent_cb(struct device *, u32, void *);

struct xmgmt_event_cb {
	struct list_head list;
	bool initialized;
	struct xocl_parent_ioctl_add_evt_cb cb;
};

struct xmgmt_event_cbs {
	struct list_head cb_list;
	struct mutex cb_lock;
};

struct xmgmt {
	struct pci_dev *pdev;
	struct xocl_subdev_pool parts;
	struct xmgmt_event_cbs evt_cbs;
	struct work_struct evt_work;
};

struct xmgmt_subdev_match_arg {
	enum xocl_subdev_id id;
	int instance;
};

static bool xmgmt_subdev_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	struct xmgmt_subdev_match_arg *a = (struct xmgmt_subdev_match_arg *)arg;
	return id == a->id && pdev->id == a->instance;
}

static int xmgmt_get_partition(struct xmgmt *xm, enum xocl_partition_id id,
	struct platform_device **partp)
{
	int rc = 0;
	struct xocl_subdev_pool *parts = &xm->parts;
	struct device *dev = DEV(xm->pdev);
	struct xmgmt_subdev_match_arg arg = { XOCL_SUBDEV_PART, id };

	if (id == (enum xocl_partition_id)-1) {
		rc = xocl_subdev_pool_get(parts, XOCL_SUBDEV_MATCH_NEXT,
			*partp, dev, partp);
	} else {
		rc = xocl_subdev_pool_get(parts, xmgmt_subdev_match,
			&arg, dev, partp);
	}

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

static int xmgmt_event_partition(struct xmgmt *xm, struct xmgmt_event_cb *cb,
	struct platform_device *part, enum xocl_events evt)
{
	xocl_subdev_match_t match = cb->cb.xevt_match_cb;
	xocl_event_cb_t evtcb = cb->cb.xevt_cb;
	void *arg = cb->cb.xevt_match_arg;
	struct xocl_partition_ioctl_event e = { evt, &cb->cb };

	if (match(XOCL_SUBDEV_PART, part, arg)) {
		int rc = evtcb(cb->cb.xevt_pdev,
			XOCL_SUBDEV_PART, part->id, evt);
		if (rc)
			return rc;
	}

	return xocl_subdev_ioctl(part, XOCL_PARTITION_EVENT, &e);
}

static void
xmgmt_event(struct xmgmt *xm, enum xocl_partition_id id, enum xocl_events evt)
{
	int ret;
	struct platform_device *pdev = NULL;
	const struct list_head *ptr, *next;
	struct xmgmt_event_cb *tmp;

	ret = xmgmt_get_partition(xm, id, &pdev);
	if (ret)
		return;

	mutex_lock(&xm->evt_cbs.cb_lock);
	list_for_each_safe(ptr, next, &xm->evt_cbs.cb_list) {
		int rc;

		tmp = list_entry(ptr, struct xmgmt_event_cb, list);
		if (!tmp->initialized)
			continue;

		rc = xmgmt_event_partition(xm, tmp, pdev, evt);
		if (rc) {
			list_del(&tmp->list);
			vfree(tmp);
		}
	}
	mutex_unlock(&xm->evt_cbs.cb_lock);

	(void) xmgmt_put_partition(xm, pdev);
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
	if (ret)
		return ret;

	/* Now bring up all children in this partition. */
	ret = xocl_subdev_ioctl(pdev, XOCL_PARTITION_INIT_CHILDREN, 0);
	(void) xmgmt_put_partition(xm, pdev);
	xmgmt_event(xm, id, XOCL_EVENT_POST_CREATION);
	return ret;
}

static int xmgmt_destroy_partition(struct xmgmt *xm, enum xocl_partition_id id)
{
	struct platform_device *pdev = NULL;
	int ret;

	ret = xmgmt_get_partition(xm, id, &pdev);
	if (ret)
		return ret;

	xmgmt_event(xm, id, XOCL_EVENT_PRE_REMOVAL);

	/* Now tear down all children in this partition. */
	ret = xocl_subdev_ioctl(pdev, XOCL_PARTITION_FINI_CHILDREN, 0);
	(void) xmgmt_put_partition(xm, pdev);
	return xocl_subdev_pool_del(&xm->parts, XOCL_SUBDEV_PART, id);
}

static void xmgmt_evt_work(struct work_struct *work)
{
	const struct list_head *ptr, *next;
	struct xmgmt_event_cb *tmp;
	struct platform_device *part = NULL;
	struct xmgmt *xm = container_of(work, struct xmgmt, evt_work);

	mutex_lock(&xm->evt_cbs.cb_lock);

	list_for_each_safe(ptr, next, &xm->evt_cbs.cb_list) {
		tmp = list_entry(ptr, struct xmgmt_event_cb, list);
		if (tmp->initialized)
			continue;

		while (xmgmt_get_partition(xm, -1, &part) != -ENOENT) {
			if (xmgmt_event_partition(xm, tmp, part,
				XOCL_EVENT_POST_CREATION)) {
				list_del(&tmp->list);
				vfree(tmp);
				tmp = NULL;
			}
			xmgmt_put_partition(xm, part);
		}

		if (tmp)
			tmp->initialized = true;
	}

	mutex_unlock(&xm->evt_cbs.cb_lock);
}

static void xmgmt_evt_init(struct xmgmt *xm)
{
	INIT_LIST_HEAD(&xm->evt_cbs.cb_list);
	mutex_init(&xm->evt_cbs.cb_lock);
	INIT_WORK(&xm->evt_work, xmgmt_evt_work);
}

static void xmgmt_evt_fini(struct xmgmt *xm)
{
	const struct list_head *ptr, *next;
	struct xmgmt_event_cb *tmp;

	flush_scheduled_work();

	mutex_lock(&xm->evt_cbs.cb_lock);
	list_for_each_safe(ptr, next, &xm->evt_cbs.cb_list) {
		tmp = list_entry(ptr, struct xmgmt_event_cb, list);
		list_del(&tmp->list);
		vfree(tmp);
	}
	mutex_unlock(&xm->evt_cbs.cb_lock);
}

static int xmgmt_evt_cb_add(struct xmgmt *xm,
	struct xocl_parent_ioctl_add_evt_cb *cb)
{
	struct xmgmt_event_cb *new = vzalloc(sizeof(*new));

	if (!new)
		return -ENOMEM;

	cb->xevt_hdl = new;
	new->cb = *cb;
	new->initialized = false;

	mutex_lock(&xm->evt_cbs.cb_lock);
	list_add(&new->list, &xm->evt_cbs.cb_list);
	mutex_unlock(&xm->evt_cbs.cb_lock);

	schedule_work(&xm->evt_work);
	return 0;
}

static void xmgmt_evt_cb_del(struct xmgmt *xm, void *hdl)
{
	struct xmgmt_event_cb *cb = (struct xmgmt_event_cb *)hdl;
	const struct list_head *ptr;
	struct xmgmt_event_cb *tmp;

	mutex_lock(&xm->evt_cbs.cb_lock);
	list_for_each(ptr, &xm->evt_cbs.cb_list) {
		tmp = list_entry(ptr, struct xmgmt_event_cb, list);
		if (tmp == cb)
			break;
	}
	list_del(&cb->list);
	mutex_unlock(&xm->evt_cbs.cb_lock);
	vfree(cb);
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

static int xmgmt_get_leaf(struct xmgmt *xm,
	struct xocl_parent_ioctl_get_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *part = NULL;

	while (rc && xmgmt_get_partition(xm, -1, &part) != -ENOENT) {
		rc = xocl_subdev_ioctl(part, XOCL_PARTITION_GET_LEAF, arg);
		xmgmt_put_partition(xm, part);
	}
	return rc;
}

static int xmgmt_get_leaf_by_id(struct xmgmt *xm,
	struct xocl_parent_ioctl_get_leaf_by_id *arg)
{
	int rc = -ENOENT;
	struct xmgmt_subdev_match_arg marg = {
		arg->xpiglbi_id, arg->xpiglbi_instance };
	struct xocl_parent_ioctl_get_leaf glarg;

	glarg.xpigl_pdev = arg->xpiglbi_pdev;
	glarg.xpigl_match_cb = xmgmt_subdev_match;
	glarg.xpigl_match_arg = &marg;
	rc = xmgmt_get_leaf(xm, &glarg);
	if (!rc)
		arg->xpiglbi_leaf = glarg.xpigl_leaf;
	return rc;
}

static int xmgmt_put_leaf(struct xmgmt *xm,
	struct xocl_parent_ioctl_put_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *part = NULL;

	while (rc && xmgmt_get_partition(xm, -1, &part) != -ENOENT) {
		rc = xocl_subdev_ioctl(part, XOCL_PARTITION_PUT_LEAF, arg);
		xmgmt_put_partition(xm, part);
	}
	return rc;
}

static int xmgmt_parent_cb(struct device *dev, u32 cmd, void *arg)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct xmgmt *xm = pci_get_drvdata(pdev);
	int rc = 0;

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
		rc = xmgmt_destroy_partition(xm,
			(enum xocl_partition_id)(uintptr_t)arg);
		break;
	case XOCL_PARENT_ADD_EVENT_CB: {
		struct xocl_parent_ioctl_add_evt_cb *cb =
			(struct xocl_parent_ioctl_add_evt_cb *)arg;
		rc = xmgmt_evt_cb_add(xm, cb);
		break;
	}
	case XOCL_PARENT_REMOVE_EVENT_CB:
		xmgmt_evt_cb_del(xm, arg);
		rc = 0;
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
	if (!xm)
		return -ENOMEM;
	xm->pdev = pdev;
	xocl_subdev_pool_init(DEV(xm->pdev), &xm->parts);
	xmgmt_evt_init(xm);

	xmgmt_config_pci(xm);
	pci_set_drvdata(pdev, xm);

	(void) xmgmt_create_partition(xm, XOCL_PART_TEST, NULL);
	return 0;
}

static void xmgmt_remove(struct pci_dev *pdev)
{
	struct xmgmt *xm = pci_get_drvdata(pdev);
	struct platform_device *part = NULL;

	xmgmt_info(xm, "leaving...");

	/*
	 * Assuming subdevs in higher partition ID can depend on ones in
	 * lower ID partitions, we remove them in the reservse order.
	 */
	while (xmgmt_get_partition(xm, -1, &part) != -ENOENT) {
		enum xocl_partition_id partid = part->id;

		xmgmt_put_partition(xm, part);
		(void) xmgmt_destroy_partition(xm, partid);
		part = NULL;
	}

	xmgmt_evt_fini(xm);

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
