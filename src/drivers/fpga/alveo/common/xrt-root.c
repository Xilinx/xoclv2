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
#include "xrt-subdev.h"
#include "xrt-parent.h"
#include "xrt-partition.h"
#include "xrt-root.h"
#include "xrt-metadata.h"
#include "xrt-root.h"

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

#define XRT_VSEC_ID	0x20
#define	XROOT_PART_FIRST	(-1)
#define	XROOT_PART_LAST		(-2)

static int xroot_parent_cb(struct device *, void *, u32, void *);

struct xroot_async_evt {
	struct list_head list;
	struct xrt_parent_ioctl_async_broadcast_evt evt;
};

struct xroot_event_cb {
	struct list_head list;
	bool initialized;
	struct xrt_parent_ioctl_evt_cb cb;
};

struct xroot_events {
	struct list_head cb_list;
	struct mutex cb_lock;
	struct work_struct cb_init_work;
	struct mutex async_evt_lock;
	struct list_head async_evt_list;
	struct work_struct async_evt_work;
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

static int
xroot_partition_trigger_evt(struct xroot *xr, struct xroot_event_cb *cb,
	struct platform_device *part, enum xrt_events evt)
{
	xrt_subdev_match_t match = cb->cb.xevt_match_cb;
	xrt_event_cb_t evtcb = cb->cb.xevt_cb;
	void *arg = cb->cb.xevt_match_arg;
	struct xrt_partition_ioctl_event e = { evt, &cb->cb };
	struct xrt_event_arg_subdev esd = { XRT_SUBDEV_PART, part->id };
	int rc;

	if (match(XRT_SUBDEV_PART, part, arg)) {
		rc = evtcb(cb->cb.xevt_pdev, evt, &esd);
		if (rc)
			return rc;
	}

	return xrt_subdev_ioctl(part, XRT_PARTITION_EVENT, &e);
}

static void
xroot_event_partition(struct xroot *xr, int instance, enum xrt_events evt)
{
	int ret;
	struct platform_device *pdev = NULL;
	const struct list_head *ptr, *next;
	struct xroot_event_cb *tmp;

	BUG_ON(instance < 0);
	ret = xroot_get_partition(xr, instance, &pdev);
	if (ret)
		return;

	mutex_lock(&xr->events.cb_lock);
	list_for_each_safe(ptr, next, &xr->events.cb_list) {
		int rc;

		tmp = list_entry(ptr, struct xroot_event_cb, list);
		if (!tmp->initialized)
			continue;

		rc = xroot_partition_trigger_evt(xr, tmp, pdev, evt);
		if (rc) {
			list_del(&tmp->list);
			vfree(tmp);
		}
	}
	mutex_unlock(&xr->events.cb_lock);

	(void) xroot_put_partition(xr, pdev);
}

int xroot_create_partition(void *root, char *dtb)
{
	struct xroot *xr = (struct xroot *)root;
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

static int xroot_destroy_single_partition(struct xroot *xr, int instance)
{
	struct platform_device *pdev = NULL;
	int ret;

	BUG_ON(instance < 0);
	ret = xroot_get_partition(xr, instance, &pdev);
	if (ret)
		return ret;

	xroot_event_partition(xr, instance, XRT_EVENT_PRE_REMOVAL);

	/* Now tear down all children in this partition. */
	ret = xrt_subdev_ioctl(pdev, XRT_PARTITION_FINI_CHILDREN, NULL);
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

static void xroot_evt_cb_init_work(struct work_struct *work)
{
	const struct list_head *ptr, *next;
	struct xroot_event_cb *tmp;
	struct platform_device *part = NULL;
	struct xroot *xr =
		container_of(work, struct xroot, events.cb_init_work);

	mutex_lock(&xr->events.cb_lock);

	list_for_each_safe(ptr, next, &xr->events.cb_list) {
		tmp = list_entry(ptr, struct xroot_event_cb, list);
		if (tmp->initialized)
			continue;

		while (xroot_get_partition(xr, XROOT_PART_LAST,
			&part) != -ENOENT) {
			int rc = xroot_partition_trigger_evt(xr, tmp, part,
				XRT_EVENT_POST_CREATION);

			(void) xroot_put_partition(xr, part);
			if (rc & XRT_EVENT_CB_STOP) {
				list_del(&tmp->list);
				vfree(tmp);
				tmp = NULL;
				break;
			}
		}

		if (tmp)
			tmp->initialized = true;
	}

	mutex_unlock(&xr->events.cb_lock);
}

static bool xroot_evt(struct xroot *xr, enum xrt_events evt)
{
	const struct list_head *ptr, *next;
	struct xroot_event_cb *tmp;
	int rc;
	bool success = true;

	mutex_lock(&xr->events.cb_lock);
	list_for_each_safe(ptr, next, &xr->events.cb_list) {
		tmp = list_entry(ptr, struct xroot_event_cb, list);
		rc = tmp->cb.xevt_cb(tmp->cb.xevt_pdev, evt, NULL);
		if (rc & XRT_EVENT_CB_ERR)
			success = false;
		if (rc & XRT_EVENT_CB_STOP) {
			list_del(&tmp->list);
			vfree(tmp);
		}
	}
	mutex_unlock(&xr->events.cb_lock);

	return success;
}

static void xroot_evt_async_evt_work(struct work_struct *work)
{
	struct xroot_async_evt *tmp;
	struct xroot *xr =
		container_of(work, struct xroot, events.async_evt_work);
	bool success;

	mutex_lock(&xr->events.async_evt_lock);
	while (!list_empty(&xr->events.async_evt_list)) {
		tmp = list_first_entry(&xr->events.async_evt_list,
			struct xroot_async_evt, list);
		list_del(&tmp->list);
		mutex_unlock(&xr->events.async_evt_lock);

		success = xroot_evt(xr, tmp->evt.xaevt_event);
		if (tmp->evt.xaevt_cb) {
			tmp->evt.xaevt_cb(tmp->evt.xaevt_pdev,
				tmp->evt.xaevt_event, tmp->evt.xaevt_arg,
				success);
		}
		vfree(tmp);

		mutex_lock(&xr->events.async_evt_lock);
	}
	mutex_unlock(&xr->events.async_evt_lock);
}

static void xroot_evt_init(struct xroot *xr)
{
	INIT_LIST_HEAD(&xr->events.cb_list);
	INIT_LIST_HEAD(&xr->events.async_evt_list);
	mutex_init(&xr->events.async_evt_lock);
	mutex_init(&xr->events.cb_lock);
	INIT_WORK(&xr->events.cb_init_work, xroot_evt_cb_init_work);
	INIT_WORK(&xr->events.async_evt_work, xroot_evt_async_evt_work);
}

static void xroot_evt_fini(struct xroot *xr)
{
	const struct list_head *ptr, *next;
	struct xroot_event_cb *tmp;

	flush_scheduled_work();

	BUG_ON(!list_empty(&xr->events.async_evt_list));

	mutex_lock(&xr->events.cb_lock);
	list_for_each_safe(ptr, next, &xr->events.cb_list) {
		tmp = list_entry(ptr, struct xroot_event_cb, list);
		list_del(&tmp->list);
		vfree(tmp);
	}
	mutex_unlock(&xr->events.cb_lock);
}

static int xroot_evt_cb_add(struct xroot *xr,
	struct xrt_parent_ioctl_evt_cb *cb)
{
	struct xroot_event_cb *new = vzalloc(sizeof(*new));

	if (!new)
		return -ENOMEM;

	cb->xevt_hdl = new;
	new->cb = *cb;
	new->initialized = false;

	mutex_lock(&xr->events.cb_lock);
	list_add(&new->list, &xr->events.cb_list);
	mutex_unlock(&xr->events.cb_lock);

	schedule_work(&xr->events.cb_init_work);
	return 0;
}

static int xroot_async_evt_add(struct xroot *xr,
	struct xrt_parent_ioctl_async_broadcast_evt *arg)
{
	struct xroot_async_evt *new = vzalloc(sizeof(*new));

	if (!new)
		return -ENOMEM;

	new->evt = *arg;

	mutex_lock(&xr->events.async_evt_lock);
	list_add(&new->list, &xr->events.async_evt_list);
	mutex_unlock(&xr->events.async_evt_lock);

	schedule_work(&xr->events.async_evt_work);
	return 0;
}

static void xroot_evt_cb_del(struct xroot *xr, void *hdl)
{
	struct xroot_event_cb *cb = (struct xroot_event_cb *)hdl;
	const struct list_head *ptr;
	struct xroot_event_cb *tmp;

	mutex_lock(&xr->events.cb_lock);
	list_for_each(ptr, &xr->events.cb_list) {
		tmp = list_entry(ptr, struct xroot_event_cb, list);
		if (tmp == cb)
			break;
	}
	list_del(&cb->list);
	mutex_unlock(&xr->events.cb_lock);
	vfree(cb);
}

static int xroot_get_leaf(struct xroot *xr,
	struct xrt_parent_ioctl_get_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *part = NULL;

	while (rc && xroot_get_partition(xr, XROOT_PART_LAST,
		&part) != -ENOENT) {
		rc = xrt_subdev_ioctl(part, XRT_PARTITION_GET_LEAF, arg);
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
		rc = xrt_subdev_ioctl(part, XRT_PARTITION_PUT_LEAF, arg);
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
	case XRT_PARENT_ADD_EVENT_CB: {
		struct xrt_parent_ioctl_evt_cb *cb =
			(struct xrt_parent_ioctl_evt_cb *)arg;
		rc = xroot_evt_cb_add(xr, cb);
		break;
	}
	case XRT_PARENT_REMOVE_EVENT_CB:
		xroot_evt_cb_del(xr, arg);
		rc = 0;
		break;
	case XRT_PARENT_ASYNC_BOARDCAST_EVENT:
		rc = xroot_async_evt_add(xr,
			(struct xrt_parent_ioctl_async_broadcast_evt *)arg);
		break;


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


	case XRT_PARENT_HOT_RESET: {
		xroot_hot_reset(xr->pdev);
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

	while (xroot_get_partition(xr, XROOT_PART_LAST, &pdev) != -ENOENT) {
		int r, i;

		i = pdev->id;
		r = xrt_subdev_ioctl(pdev, XRT_PARTITION_INIT_CHILDREN, NULL);
		(void) xroot_put_partition(xr, pdev);
		if (r == -EEXIST)
			continue; /* Already brough up, nothing to do. */
		if (r)
			atomic_inc(&xr->parts.bringup_failed);

		xroot_event_partition(xr, i, XRT_EVENT_POST_CREATION);

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

int xroot_add_vsec_node(void *root, char *dtb)
{
	struct xroot *xr = (struct xroot *)root;
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

int xroot_add_simple_node(void *root, char *dtb, const char *endpoint)
{
	struct xroot *xr = (struct xroot *)root;
	struct device *dev = DEV(xr->pdev);
	struct xrt_md_endpoint ep = { 0 };
	int ret = 0;

	ep.ep_name = endpoint;
	ret = xrt_md_add_endpoint(dev, dtb, &ep);
	if (ret)
		xroot_err(xr, "add %s failed, ret %d", endpoint, ret);

	return ret;
}

bool xroot_wait_for_bringup(void *root)
{
	struct xroot *xr = (struct xroot *)root;

	wait_for_completion(&xr->parts.bringup_comp);
	return atomic_xchg(&xr->parts.bringup_failed, 0) == 0;
}

int xroot_probe(struct pci_dev *pdev, void **root)
{
	struct device *dev = DEV(pdev);
	struct xroot *xr = NULL;

	dev_info(dev, "%s: probing...", __func__);

	xr = devm_kzalloc(dev, sizeof(*xr), GFP_KERNEL);
	if (!xr)
		return -ENOMEM;

	xr->pdev = pdev;
	xroot_parts_init(xr);
	xroot_evt_init(xr);

	*root = xr;
	return 0;
}

void xroot_remove(void *root)
{
	struct xroot *xr = (struct xroot *)root;
	struct platform_device *part = NULL;

	xroot_info(xr, "leaving...");

	if (xroot_get_partition(xr, XROOT_PART_FIRST, &part) == 0) {
		int instance = part->id;

		xroot_put_partition(xr, part);
		(void) xroot_destroy_partition(xr, instance);
	}

	xroot_evt_fini(xr);
	xroot_parts_fini(xr);
}

static void xroot_broadcast_event_cb(struct platform_device *pdev,
	enum xrt_events evt, void *arg, bool success)
{
	struct completion *comp = (struct completion *)arg;

	complete(comp);
}

void xroot_broadcast(void *root, enum xrt_events evt)
{
	int rc;
	struct completion comp;
	struct xroot *xr = (struct xroot *)root;
	struct xrt_parent_ioctl_async_broadcast_evt e = {
		NULL, evt, xroot_broadcast_event_cb, &comp
	};

	init_completion(&comp);
	rc = xroot_async_evt_add(xr, &e);
	if (rc == 0)
		wait_for_completion(&comp);
	else
		xroot_err(xr, "can't broadcast event (%d): %d", evt, rc);
}
