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
#include "xocl-metadata.h"

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
	struct xocl_parent_ioctl_evt_cb cb;
};

struct xmgmt_events {
	struct list_head cb_list;
	struct mutex lock;
	struct work_struct cb_init_work;
};

struct xmgmt_parts {
	struct xocl_subdev_pool pool;
	struct work_struct bringup_work;
};

struct xmgmt {
	struct pci_dev *pdev;
	struct xmgmt_events events;
	struct xmgmt_parts parts;

	char *root_dtb;
};

struct xmgmt_part_match_arg {
	enum xocl_subdev_id id;
	int instance;
};

static bool xmgmt_part_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	struct xmgmt_part_match_arg *a = (struct xmgmt_part_match_arg *)arg;
	return id == a->id && pdev->id == a->instance;
}

static int xmgmt_get_partition(struct xmgmt *xm, int instance,
	struct platform_device **partp)
{
	int rc = 0;
	struct xocl_subdev_pool *parts = &xm->parts.pool;
	struct device *dev = DEV(xm->pdev);
	struct xmgmt_part_match_arg arg = { XOCL_SUBDEV_PART, instance };

	if (instance == PLATFORM_DEVID_NONE) {
		rc = xocl_subdev_pool_get(parts, XOCL_SUBDEV_MATCH_NEXT,
			*partp, dev, partp);
	} else {
		rc = xocl_subdev_pool_get(parts, xmgmt_part_match,
			&arg, dev, partp);
	}

	if (rc && rc != -ENOENT)
		xmgmt_err(xm, "failed to hold partition %d: %d", instance, rc);
	return rc;
}

static void xmgmt_put_partition(struct xmgmt *xm, struct platform_device *part)
{
	int inst = part->id;
	int rc = xocl_subdev_pool_put(&xm->parts.pool, part, DEV(xm->pdev));

	if (rc)
		xmgmt_err(xm, "failed to release partition %d: %d", inst, rc);
}

static int
xmgmt_partition_trigger_evt(struct xmgmt *xm, struct xmgmt_event_cb *cb,
	struct platform_device *part, enum xocl_events evt)
{
	xocl_subdev_match_t match = cb->cb.xevt_match_cb;
	xocl_event_cb_t evtcb = cb->cb.xevt_cb;
	void *arg = cb->cb.xevt_match_arg;
	struct xocl_partition_ioctl_event e = { evt, &cb->cb };
	int rc;

	if (match(XOCL_SUBDEV_PART, part, arg)) {
		rc = evtcb(cb->cb.xevt_pdev, evt, XOCL_SUBDEV_PART, part->id);
		if (rc)
			return rc;
	}

	return xocl_subdev_ioctl(part, XOCL_PARTITION_EVENT, &e);
}

static void
xmgmt_event_partition(struct xmgmt *xm, int instance, enum xocl_events evt)
{
	int ret;
	struct platform_device *pdev = NULL;
	const struct list_head *ptr, *next;
	struct xmgmt_event_cb *tmp;

	ret = xmgmt_get_partition(xm, instance, &pdev);
	if (ret)
		return;

	mutex_lock(&xm->events.lock);
	list_for_each_safe(ptr, next, &xm->events.cb_list) {
		int rc;

		tmp = list_entry(ptr, struct xmgmt_event_cb, list);
		if (!tmp->initialized)
			continue;

		rc = xmgmt_partition_trigger_evt(xm, tmp, pdev, evt);
		if (rc) {
			list_del(&tmp->list);
			vfree(tmp);
		}
	}
	mutex_unlock(&xm->events.lock);

	(void) xmgmt_put_partition(xm, pdev);
}

static int xmgmt_create_partition(struct xmgmt *xm, char *dtb)
{
	int ret = xocl_subdev_pool_add(&xm->parts.pool,
		XOCL_SUBDEV_PART, xmgmt_parent_cb, dtb);

	if (ret >= 0)
		schedule_work(&xm->parts.bringup_work);
	return ret;
}

static int xmgmt_destroy_partition(struct xmgmt *xm, int instance)
{
	struct platform_device *pdev = NULL;
	int ret;

	ret = xmgmt_get_partition(xm, instance, &pdev);
	if (ret)
		return ret;

	xmgmt_event_partition(xm, instance, XOCL_EVENT_PRE_REMOVAL);

	/* Now tear down all children in this partition. */
	ret = xocl_subdev_ioctl(pdev, XOCL_PARTITION_FINI_CHILDREN, NULL);
	(void) xmgmt_put_partition(xm, pdev);
	if (!ret) {
		ret = xocl_subdev_pool_del(&xm->parts.pool,
			XOCL_SUBDEV_PART, instance);
	}

	return ret;
}

static void xmgmt_evt_cb_init_work(struct work_struct *work)
{
	const struct list_head *ptr, *next;
	struct xmgmt_event_cb *tmp;
	struct platform_device *part = NULL;
	struct xmgmt *xm =
		container_of(work, struct xmgmt, events.cb_init_work);

	mutex_lock(&xm->events.lock);

	list_for_each_safe(ptr, next, &xm->events.cb_list) {
		tmp = list_entry(ptr, struct xmgmt_event_cb, list);
		if (tmp->initialized)
			continue;

		while (xmgmt_get_partition(xm, PLATFORM_DEVID_NONE,
			&part) != -ENOENT) {
			if (xmgmt_partition_trigger_evt(xm, tmp, part,
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

	mutex_unlock(&xm->events.lock);
}

static void xmgmt_evt_broadcast(struct xmgmt *xm, enum xocl_events evt)
{
	const struct list_head *ptr, *next;
	struct xmgmt_event_cb *tmp;
	int rc;

	mutex_lock(&xm->events.lock);

	list_for_each_safe(ptr, next, &xm->events.cb_list) {
		tmp = list_entry(ptr, struct xmgmt_event_cb, list);
		rc = tmp->cb.xevt_cb(tmp->cb.xevt_pdev, evt, -1, -1);
		if (rc) {
			list_del(&tmp->list);
			vfree(tmp);
		}
	}

	mutex_unlock(&xm->events.lock);
}

static void xmgmt_evt_init(struct xmgmt *xm)
{
	INIT_LIST_HEAD(&xm->events.cb_list);
	mutex_init(&xm->events.lock);
	INIT_WORK(&xm->events.cb_init_work, xmgmt_evt_cb_init_work);
}

static void xmgmt_evt_fini(struct xmgmt *xm)
{
	const struct list_head *ptr, *next;
	struct xmgmt_event_cb *tmp;

	flush_scheduled_work();

	mutex_lock(&xm->events.lock);
	list_for_each_safe(ptr, next, &xm->events.cb_list) {
		tmp = list_entry(ptr, struct xmgmt_event_cb, list);
		list_del(&tmp->list);
		vfree(tmp);
	}
	mutex_unlock(&xm->events.lock);
}

static int xmgmt_evt_cb_add(struct xmgmt *xm,
	struct xocl_parent_ioctl_evt_cb *cb)
{
	struct xmgmt_event_cb *new = vzalloc(sizeof(*new));

	if (!new)
		return -ENOMEM;

	cb->xevt_hdl = new;
	new->cb = *cb;
	new->initialized = false;

	mutex_lock(&xm->events.lock);
	list_add(&new->list, &xm->events.cb_list);
	mutex_unlock(&xm->events.lock);

	schedule_work(&xm->events.cb_init_work);
	return 0;
}

static void xmgmt_evt_cb_del(struct xmgmt *xm, void *hdl)
{
	struct xmgmt_event_cb *cb = (struct xmgmt_event_cb *)hdl;
	const struct list_head *ptr;
	struct xmgmt_event_cb *tmp;

	mutex_lock(&xm->events.lock);
	list_for_each(ptr, &xm->events.cb_list) {
		tmp = list_entry(ptr, struct xmgmt_event_cb, list);
		if (tmp == cb)
			break;
	}
	list_del(&cb->list);
	mutex_unlock(&xm->events.lock);
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

	while (rc && xmgmt_get_partition(xm, PLATFORM_DEVID_NONE,
		&part) != -ENOENT) {
		rc = xocl_subdev_ioctl(part, XOCL_PARTITION_GET_LEAF, arg);
		xmgmt_put_partition(xm, part);
	}
	return rc;
}

static int xmgmt_put_leaf(struct xmgmt *xm,
	struct xocl_parent_ioctl_put_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *part = NULL;

	while (rc && xmgmt_get_partition(xm, PLATFORM_DEVID_NONE,
		&part) != -ENOENT) {
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

	switch (cmd) {
	case XOCL_PARENT_GET_LEAF: {
		struct xocl_parent_ioctl_get_leaf *getleaf =
			(struct xocl_parent_ioctl_get_leaf *)arg;
		rc = xmgmt_get_leaf(xm, getleaf);
		break;
	}
	case XOCL_PARENT_PUT_LEAF: {
		struct xocl_parent_ioctl_put_leaf *putleaf =
			(struct xocl_parent_ioctl_put_leaf *)arg;
		rc = xmgmt_put_leaf(xm, putleaf);
		break;
	}
	case XOCL_PARENT_CREATE_PARTITION:
		rc = xmgmt_create_partition(xm, (char *)arg);
		break;
	case XOCL_PARENT_REMOVE_PARTITION:
		rc = xmgmt_destroy_partition(xm, (int)(uintptr_t)arg);
		break;
	case XOCL_PARENT_ADD_EVENT_CB: {
		struct xocl_parent_ioctl_evt_cb *cb =
			(struct xocl_parent_ioctl_evt_cb *)arg;
		rc = xmgmt_evt_cb_add(xm, cb);
		break;
	}
	case XOCL_PARENT_REMOVE_EVENT_CB:
		xmgmt_evt_cb_del(xm, arg);
		rc = 0;
		break;
	case XOCL_PARENT_BOARDCAST_EVENT:
		xmgmt_evt_broadcast(xm, (enum xocl_events)(uintptr_t)arg);
		break;
	case XOCL_PARENT_GET_HOLDERS: {
		struct xocl_parent_ioctl_get_holders *holders =
			(struct xocl_parent_ioctl_get_holders *)arg;
		rc = xocl_subdev_pool_get_holders(&xm->parts.pool,
			holders->xpigh_pdev, holders->xpigh_holder_buf,
			holders->xpigh_holder_buf_len);
		break;
	}
	default:
		xmgmt_err(xm, "unknown IOCTL cmd %d", cmd);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static void xmgmt_bringup_partition_work(struct work_struct *work)
{
	struct platform_device *pdev = NULL;
	struct xmgmt *xm = container_of(work, struct xmgmt, parts.bringup_work);

	while (xmgmt_get_partition(xm, PLATFORM_DEVID_NONE, &pdev) != -ENOENT) {
		int r, i;

		i = pdev->id;
		r = xocl_subdev_ioctl(pdev, XOCL_PARTITION_INIT_CHILDREN, NULL);
		(void) xmgmt_put_partition(xm, pdev);
		if (!r)
			xmgmt_event_partition(xm, i, XOCL_EVENT_POST_CREATION);
	}
}

static void xmgmt_parts_init(struct xmgmt *xm)
{
	xocl_subdev_pool_init(DEV(xm->pdev), &xm->parts.pool);
	INIT_WORK(&xm->parts.bringup_work, xmgmt_bringup_partition_work);
}

static void xmgmt_parts_fini(struct xmgmt *xm)
{
	flush_scheduled_work();
	(void) xocl_subdev_pool_fini(&xm->parts.pool);
}

static int xmgmt_add_vsec_node(struct xmgmt *xm)
{
	struct device *dev = DEV(xm->pdev);
	struct xocl_md_endpoint ep;
	int cap, ret = 0;
	u32 off_low, off_high, vsec_bar;
	u64 vsec_off;

	cap = pci_find_ext_capability(xm->pdev, PCI_EXT_CAP_ID_VNDR);
	if (!cap) {
		xmgmt_info(xm, "No Vendor Specific Capability.");
		return 0;
	}

	if (pci_read_config_dword(xm->pdev, cap+8, &off_low) ||
	    pci_read_config_dword(xm->pdev, cap+12, &off_high)) {
		xmgmt_err(xm, "pci_read vendor specific failed.");
		return -EINVAL;
	}

	memset(&ep, 0, sizeof(ep));
	ep.ep_name = NODE_VSEC;
	ret = xocl_md_add_endpoint(dev, &xm->root_dtb, &ep);
	if (ret) {
		xmgmt_err(xm, "add vsec metadata failed, ret %d", ret);
		goto failed;
	}

	vsec_bar = cpu_to_be32(off_low & 0xf);
	vsec_off = cpu_to_be64(((u64)off_high << 32) | (off_low & ~0xfU));
	ret = xocl_md_setprop_by_nodename(dev, &xm->root_dtb, NODE_VSEC,
		PROP_OFFSET, &vsec_off, sizeof(vsec_off));
	if (ret) {
		xmgmt_err(xm, "add vsec offset failed, ret %d", ret);
		goto failed;
	}

failed:
	return ret;
}

static int xmgmt_create_root_metadata(struct xmgmt *xm)
{
	struct device *dev = DEV(xm->pdev);
	int ret;

	ret = xocl_md_create(dev, &xm->root_dtb);
	if (ret) {
		xmgmt_err(xm, "create metadata failed, ret %d", ret);
		goto failed;
	}

	ret = xmgmt_add_vsec_node(xm);
	if (ret)
		goto failed;

	return 0;

failed:
	if (xm->root_dtb) {
		vfree(xm->root_dtb);
		xm->root_dtb = NULL;
	}
	return ret;
}

static int xmgmt_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct xmgmt *xm;
	struct device *dev = DEV(pdev);
	int ret = 0;

	dev_info(dev, "%s: probing...", __func__);

	xm = devm_kzalloc(dev, sizeof(*xm), GFP_KERNEL);
	if (!xm)
		return -ENOMEM;
	xm->pdev = pdev;
	xmgmt_parts_init(xm);
	xmgmt_evt_init(xm);

	xmgmt_config_pci(xm);
	pci_set_drvdata(pdev, xm);

	ret = xmgmt_create_root_metadata(xm);
	if (ret)
		return ret;

	(void) xmgmt_create_partition(xm, xm->root_dtb);
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
	while (xmgmt_get_partition(xm, PLATFORM_DEVID_NONE, &part) != -ENOENT) {
		int instance = part->id;

		xmgmt_put_partition(xm, part);
		(void) xmgmt_destroy_partition(xm, instance);
		part = NULL;
	}

	xmgmt_evt_fini(xm);
	xmgmt_parts_fini(xm);

	pci_disable_pcie_error_reporting(pdev);

	if (xm->root_dtb) {
		vfree(xm->root_dtb);
		xm->root_dtb = NULL;
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
