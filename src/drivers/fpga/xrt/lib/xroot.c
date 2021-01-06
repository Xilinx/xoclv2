// SPDX-License-Identifier: GPL-2.0
/*
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/hwmon.h>
#include "xleaf.h"
#include "subdev_pool.h"
#include "parent.h"
#include "partition.h"
#include "xroot.h"
#include "metadata.h"

#define	XROOT_PDEV(xr)		((xr)->pdev)
#define	XROOT_DEV(xr)		(&(XROOT_PDEV(xr)->dev))
#define xroot_err(xr, fmt, args...)	\
	dev_err(XROOT_DEV(xr), "%s: " fmt, __func__, ##args)
#define xroot_warn(xr, fmt, args...)	\
	dev_warn(XROOT_DEV(xr), "%s: " fmt, __func__, ##args)
#define xroot_info(xr, fmt, args...)	\
	dev_info(XROOT_DEV(xr), "%s: " fmt, __func__, ##args)
#define xroot_dbg(xr, fmt, args...)	\
	dev_dbg(XROOT_DEV(xr), "%s: " fmt, __func__, ##args)

#define XRT_VSEC_ID		0x20

#define	XROOT_PART_FIRST	(-1)
#define	XROOT_PART_LAST		(-2)

static int xroot_parent_cb(struct device *, void *, u32, void *);

struct xroot_evt {
	struct list_head list;
	struct xrt_event evt;
	struct completion comp;
	bool async;
};

struct xroot_events {
	struct mutex evt_lock;
	struct list_head evt_list;
	struct work_struct evt_work;
};

struct xroot_parts {
	struct xrt_subdev_pool pool;
	struct work_struct bringup_work;
	atomic_t bringup_pending;
	atomic_t bringup_failed;
	struct completion bringup_comp;
};

struct xroot {
	struct pci_dev *pdev;
	struct xroot_events events;
	struct xroot_parts parts;
	struct xroot_pf_cb pf_cb;
};

struct xroot_part_match_arg {
	enum xrt_subdev_id id;
	int instance;
};

static bool xroot_part_match(enum xrt_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	struct xroot_part_match_arg *a = (struct xroot_part_match_arg *)arg;
	return id == a->id && pdev->id == a->instance;
}

static int xroot_get_partition(struct xroot *xr, int instance,
	struct platform_device **partp)
{
	int rc = 0;
	struct xrt_subdev_pool *parts = &xr->parts.pool;
	struct device *dev = DEV(xr->pdev);
	struct xroot_part_match_arg arg = { XRT_SUBDEV_PART, instance };

	if (instance == XROOT_PART_LAST) {
		rc = xrt_subdev_pool_get(parts, XRT_SUBDEV_MATCH_NEXT,
			*partp, dev, partp);
	} else if (instance == XROOT_PART_FIRST) {
		rc = xrt_subdev_pool_get(parts, XRT_SUBDEV_MATCH_PREV,
			*partp, dev, partp);
	} else {
		rc = xrt_subdev_pool_get(parts, xroot_part_match,
			&arg, dev, partp);
	}

	if (rc && rc != -ENOENT)
		xroot_err(xr, "failed to hold partition %d: %d", instance, rc);
	return rc;
}

static void xroot_put_partition(struct xroot *xr, struct platform_device *part)
{
	int inst = part->id;
	int rc = xrt_subdev_pool_put(&xr->parts.pool, part, DEV(xr->pdev));

	if (rc)
		xroot_err(xr, "failed to release partition %d: %d", inst, rc);
}

static int xroot_trigger_event(struct xroot *xr,
	struct xrt_event *e, bool async)
{
	struct xroot_evt *enew = vzalloc(sizeof(*enew));

	if (!enew)
		return -ENOMEM;

	enew->evt = *e;
	enew->async = async;
	init_completion(&enew->comp);

	mutex_lock(&xr->events.evt_lock);
	list_add(&enew->list, &xr->events.evt_list);
	mutex_unlock(&xr->events.evt_lock);

	schedule_work(&xr->events.evt_work);

	if (async)
		return 0;

	wait_for_completion(&enew->comp);
	vfree(enew);
	return 0;
}

static void
xroot_partition_trigger_event(struct xroot *xr, int inst, enum xrt_events e)
{
	int ret;
	struct platform_device *pdev = NULL;
	struct xrt_event evt = { 0 };

	BUG_ON(inst < 0);
	/* Only triggers subdev specific events. */
	BUG_ON(e != XRT_EVENT_POST_CREATION && e != XRT_EVENT_PRE_REMOVAL);

	ret = xroot_get_partition(xr, inst, &pdev);
	if (ret)
		return;

	/* Triggers event for children, first. */
	(void) xleaf_ioctl(pdev, XRT_PARTITION_TRIGGER_EVENT,
		(void *)(uintptr_t)e);

	/* Triggers event for itself. */
	evt.xe_evt = e;
	evt.xe_subdev.xevt_subdev_id = XRT_SUBDEV_PART;
	evt.xe_subdev.xevt_subdev_instance = inst;
	(void) xroot_trigger_event(xr, &evt, false);

	(void) xroot_put_partition(xr, pdev);
}

int xroot_create_partition(struct xroot *xr, char *dtb)
{
	int ret;

	atomic_inc(&xr->parts.bringup_pending);
	ret = xrt_subdev_pool_add(&xr->parts.pool,
		XRT_SUBDEV_PART, xroot_parent_cb, xr, dtb);
	if (ret >= 0) {
		schedule_work(&xr->parts.bringup_work);
	} else {
		atomic_dec(&xr->parts.bringup_pending);
		atomic_inc(&xr->parts.bringup_failed);
		xroot_err(xr, "failed to create partition: %d", ret);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(xroot_create_partition);

static int xroot_destroy_single_partition(struct xroot *xr, int instance)
{
	struct platform_device *pdev = NULL;
	int ret;

	BUG_ON(instance < 0);
	ret = xroot_get_partition(xr, instance, &pdev);
	if (ret)
		return ret;

	xroot_partition_trigger_event(xr, instance, XRT_EVENT_PRE_REMOVAL);

	/* Now tear down all children in this partition. */
	ret = xleaf_ioctl(pdev, XRT_PARTITION_FINI_CHILDREN, NULL);
	(void) xroot_put_partition(xr, pdev);
	if (!ret) {
		ret = xrt_subdev_pool_del(&xr->parts.pool,
			XRT_SUBDEV_PART, instance);
	}

	return ret;
}

static int xroot_destroy_partition(struct xroot *xr, int instance)
{
	struct platform_device *target = NULL;
	struct platform_device *deps = NULL;
	int ret;

	BUG_ON(instance < 0);
	/*
	 * Make sure target partition exists and can't go away before
	 * we remove it's dependents
	 */
	ret = xroot_get_partition(xr, instance, &target);
	if (ret)
		return ret;

	/*
	 * Remove all partitions depend on target one.
	 * Assuming subdevs in higher partition ID can depend on ones in
	 * lower ID partitions, we remove them in the reservse order.
	 */
	while (xroot_get_partition(xr, XROOT_PART_LAST, &deps) != -ENOENT) {
		int inst = deps->id;

		xroot_put_partition(xr, deps);
		if (instance == inst)
			break;
		(void) xroot_destroy_single_partition(xr, inst);
		deps = NULL;
	}

	/* Now we can remove the target partition. */
	xroot_put_partition(xr, target);
	return xroot_destroy_single_partition(xr, instance);
}

static int xroot_lookup_partition(struct xroot *xr,
	struct xrt_parent_ioctl_lookup_partition *arg)
{
	int rc = -ENOENT;
	struct platform_device *part = NULL;

	while (rc < 0 && xroot_get_partition(xr, XROOT_PART_LAST,
		&part) != -ENOENT) {
		if (arg->xpilp_match_cb(XRT_SUBDEV_PART, part,
			arg->xpilp_match_arg)) {
			rc = part->id;
		}
		xroot_put_partition(xr, part);
	}
	return rc;
}

static void xroot_process_event(struct xroot *xr, struct xrt_event *evt)
{
	struct platform_device *part = NULL;

	while (xroot_get_partition(xr, XROOT_PART_LAST, &part) != -ENOENT) {
		(void) xleaf_ioctl(part, XRT_XLEAF_EVENT, evt);
		xroot_put_partition(xr, part);
	}
}

static void xroot_event_work(struct work_struct *work)
{
	struct xroot_evt *tmp;
	struct xroot *xr = container_of(work, struct xroot, events.evt_work);

	mutex_lock(&xr->events.evt_lock);
	while (!list_empty(&xr->events.evt_list)) {
		tmp = list_first_entry(&xr->events.evt_list,
			struct xroot_evt, list);
		list_del(&tmp->list);
		mutex_unlock(&xr->events.evt_lock);

		xroot_process_event(xr, &tmp->evt);

		if (tmp->async)
			vfree(tmp);
		else
			complete(&tmp->comp);

		mutex_lock(&xr->events.evt_lock);
	}
	mutex_unlock(&xr->events.evt_lock);
}

static void xroot_event_init(struct xroot *xr)
{
	INIT_LIST_HEAD(&xr->events.evt_list);
	mutex_init(&xr->events.evt_lock);
	INIT_WORK(&xr->events.evt_work, xroot_event_work);
}

static void xroot_event_fini(struct xroot *xr)
{
	flush_scheduled_work();
	BUG_ON(!list_empty(&xr->events.evt_list));
}

static int xroot_get_leaf(struct xroot *xr,
	struct xrt_parent_ioctl_get_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *part = NULL;

	while (rc && xroot_get_partition(xr, XROOT_PART_LAST,
		&part) != -ENOENT) {
		rc = xleaf_ioctl(part, XRT_PARTITION_GET_LEAF, arg);
		xroot_put_partition(xr, part);
	}
	return rc;
}

static int xroot_put_leaf(struct xroot *xr,
	struct xrt_parent_ioctl_put_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *part = NULL;

	while (rc && xroot_get_partition(xr, XROOT_PART_LAST,
		&part) != -ENOENT) {
		rc = xleaf_ioctl(part, XRT_PARTITION_PUT_LEAF, arg);
		xroot_put_partition(xr, part);
	}
	return rc;
}

static int xroot_parent_cb(struct device *dev, void *parg, u32 cmd, void *arg)
{
	struct xroot *xr = (struct xroot *)parg;
	int rc = 0;

	switch (cmd) {
	/* Leaf actions. */
	case XRT_PARENT_GET_LEAF: {
		struct xrt_parent_ioctl_get_leaf *getleaf =
			(struct xrt_parent_ioctl_get_leaf *)arg;
		rc = xroot_get_leaf(xr, getleaf);
		break;
	}
	case XRT_PARENT_PUT_LEAF: {
		struct xrt_parent_ioctl_put_leaf *putleaf =
			(struct xrt_parent_ioctl_put_leaf *)arg;
		rc = xroot_put_leaf(xr, putleaf);
		break;
	}
	case XRT_PARENT_GET_LEAF_HOLDERS: {
		struct xrt_parent_ioctl_get_holders *holders =
			(struct xrt_parent_ioctl_get_holders *)arg;
		rc = xrt_subdev_pool_get_holders(&xr->parts.pool,
			holders->xpigh_pdev, holders->xpigh_holder_buf,
			holders->xpigh_holder_buf_len);
		break;
	}


	/* Partition actions. */
	case XRT_PARENT_CREATE_PARTITION:
		rc = xroot_create_partition(xr, (char *)arg);
		break;
	case XRT_PARENT_REMOVE_PARTITION:
		rc = xroot_destroy_partition(xr, (int)(uintptr_t)arg);
		break;
	case XRT_PARENT_LOOKUP_PARTITION: {
		struct xrt_parent_ioctl_lookup_partition *getpart =
			(struct xrt_parent_ioctl_lookup_partition *)arg;
		rc = xroot_lookup_partition(xr, getpart);
		break;
	}
	case XRT_PARENT_WAIT_PARTITION_BRINGUP:
		rc = xroot_wait_for_bringup(xr) ? 0 : -EINVAL;
		break;


	/* Event actions. */
	case XRT_PARENT_EVENT:
	case XRT_PARENT_EVENT_ASYNC: {
		bool async = (cmd == XRT_PARENT_EVENT_ASYNC);
		struct xrt_event *evt = (struct xrt_event *)arg;

		rc = xroot_trigger_event(xr, evt, async);
		break;
	}


	/* Device info. */
	case XRT_PARENT_GET_RESOURCE: {
		struct xrt_parent_ioctl_get_res *res =
			(struct xrt_parent_ioctl_get_res *)arg;
		res->xpigr_res = xr->pdev->resource;
		break;
	}
	case XRT_PARENT_GET_ID: {
		struct xrt_parent_ioctl_get_id *id =
			(struct xrt_parent_ioctl_get_id *)arg;

		id->xpigi_vendor_id = xr->pdev->vendor;
		id->xpigi_device_id = xr->pdev->device;
		id->xpigi_sub_vendor_id = xr->pdev->subsystem_vendor;
		id->xpigi_sub_device_id = xr->pdev->subsystem_device;
		break;
	}

	/* MISC generic PCIE driver functions. */
	case XRT_PARENT_HOT_RESET: {
		xr->pf_cb.xpc_hot_reset(xr->pdev);
		break;
	}
	case XRT_PARENT_HWMON: {
		struct xrt_parent_ioctl_hwmon *hwmon =
			(struct xrt_parent_ioctl_hwmon *)arg;

		if (hwmon->xpih_register) {
			hwmon->xpih_hwmon_dev =
				hwmon_device_register_with_info(DEV(xr->pdev),
				hwmon->xpih_name, hwmon->xpih_drvdata, NULL,
				hwmon->xpih_groups);
		} else {
			(void) hwmon_device_unregister(hwmon->xpih_hwmon_dev);
		}
		break;
	}

	default:
		xroot_err(xr, "unknown IOCTL cmd %d", cmd);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static void xroot_bringup_partition_work(struct work_struct *work)
{
	struct platform_device *pdev = NULL;
	struct xroot *xr = container_of(work, struct xroot, parts.bringup_work);

	while (xroot_get_partition(xr, XROOT_PART_FIRST, &pdev) != -ENOENT) {
		int r, i;

		i = pdev->id;
		r = xleaf_ioctl(pdev, XRT_PARTITION_INIT_CHILDREN, NULL);
		(void) xroot_put_partition(xr, pdev);
		if (r == -EEXIST)
			continue; /* Already brough up, nothing to do. */
		if (r)
			atomic_inc(&xr->parts.bringup_failed);

		xroot_partition_trigger_event(xr, i, XRT_EVENT_POST_CREATION);

		if (atomic_dec_and_test(&xr->parts.bringup_pending))
			complete(&xr->parts.bringup_comp);
	}
}

static void xroot_parts_init(struct xroot *xr)
{
	xrt_subdev_pool_init(DEV(xr->pdev), &xr->parts.pool);
	INIT_WORK(&xr->parts.bringup_work, xroot_bringup_partition_work);
	atomic_set(&xr->parts.bringup_pending, 0);
	atomic_set(&xr->parts.bringup_failed, 0);
	init_completion(&xr->parts.bringup_comp);
}

static void xroot_parts_fini(struct xroot *xr)
{
	flush_scheduled_work();
	(void) xrt_subdev_pool_fini(&xr->parts.pool);
}

int xroot_add_vsec_node(struct xroot *xr, char *dtb)
{
	struct device *dev = DEV(xr->pdev);
	struct xrt_md_endpoint ep = { 0 };
	int cap = 0, ret = 0;
	u32 off_low, off_high, vsec_bar, header;
	u64 vsec_off;

	while ((cap = pci_find_next_ext_capability(xr->pdev, cap,
	    PCI_EXT_CAP_ID_VNDR))) {
		pci_read_config_dword(xr->pdev, cap + PCI_VNDR_HEADER, &header);
		if (PCI_VNDR_HEADER_ID(header) == XRT_VSEC_ID)
			break;
	}
	if (!cap) {
		xroot_info(xr, "No Vendor Specific Capability.");
		return -ENOENT;
	}

	if (pci_read_config_dword(xr->pdev, cap+8, &off_low) ||
	    pci_read_config_dword(xr->pdev, cap+12, &off_high)) {
		xroot_err(xr, "pci_read vendor specific failed.");
		return -EINVAL;
	}

	ep.ep_name = NODE_VSEC;
	ret = xrt_md_add_endpoint(dev, dtb, &ep);
	if (ret) {
		xroot_err(xr, "add vsec metadata failed, ret %d", ret);
		goto failed;
	}

	vsec_bar = cpu_to_be32(off_low & 0xf);
	ret = xrt_md_set_prop(dev, dtb, NODE_VSEC,
		NULL, PROP_BAR_IDX, &vsec_bar, sizeof(vsec_bar));
	if (ret) {
		xroot_err(xr, "add vsec bar idx failed, ret %d", ret);
		goto failed;
	}

	vsec_off = cpu_to_be64(((u64)off_high << 32) | (off_low & ~0xfU));
	ret = xrt_md_set_prop(dev, dtb, NODE_VSEC,
		NULL, PROP_OFFSET, &vsec_off, sizeof(vsec_off));
	if (ret) {
		xroot_err(xr, "add vsec offset failed, ret %d", ret);
		goto failed;
	}

failed:
	return ret;
}
EXPORT_SYMBOL_GPL(xroot_add_vsec_node);

int xroot_add_simple_node(struct xroot *xr, char *dtb, const char *endpoint)
{
	struct device *dev = DEV(xr->pdev);
	struct xrt_md_endpoint ep = { 0 };
	int ret = 0;

	ep.ep_name = endpoint;
	ret = xrt_md_add_endpoint(dev, dtb, &ep);
	if (ret)
		xroot_err(xr, "add %s failed, ret %d", endpoint, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(xroot_add_simple_node);

bool xroot_wait_for_bringup(struct xroot *xr)
{
	wait_for_completion(&xr->parts.bringup_comp);
	return atomic_xchg(&xr->parts.bringup_failed, 0) == 0;
}
EXPORT_SYMBOL_GPL(xroot_wait_for_bringup);

int xroot_probe(struct pci_dev *pdev, struct xroot_pf_cb *cb,
	struct xroot **root)
{
	struct device *dev = DEV(pdev);
	struct xroot *xr = NULL;

	dev_info(dev, "%s: probing...", __func__);

	xr = devm_kzalloc(dev, sizeof(*xr), GFP_KERNEL);
	if (!xr)
		return -ENOMEM;

	xr->pdev = pdev;
	xr->pf_cb = *cb;
	xroot_parts_init(xr);
	xroot_event_init(xr);

	*root = xr;
	return 0;
}
EXPORT_SYMBOL_GPL(xroot_probe);

void xroot_remove(struct xroot *xr)
{
	struct platform_device *part = NULL;

	xroot_info(xr, "leaving...");

	if (xroot_get_partition(xr, XROOT_PART_FIRST, &part) == 0) {
		int instance = part->id;

		xroot_put_partition(xr, part);
		(void) xroot_destroy_partition(xr, instance);
	}

	xroot_event_fini(xr);
	xroot_parts_fini(xr);
}
EXPORT_SYMBOL_GPL(xroot_remove);

void xroot_broadcast(struct xroot *xr, enum xrt_events evt)
{
	struct xrt_event e = { 0 };

	/* Root pf driver only broadcasts below two events. */
	BUG_ON(evt != XRT_EVENT_POST_CREATION && evt != XRT_EVENT_PRE_REMOVAL);

	e.xe_evt = evt;
	e.xe_subdev.xevt_subdev_id = XRT_ROOT;
	e.xe_subdev.xevt_subdev_instance = 0;
	(void) xroot_trigger_event(xr, &e, false);
}
EXPORT_SYMBOL_GPL(xroot_broadcast);
