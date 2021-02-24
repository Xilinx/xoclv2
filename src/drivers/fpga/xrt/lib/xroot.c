// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Root Functions
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/hwmon.h>
#include "xroot.h"
#include "subdev_pool.h"
#include "group.h"
#include "metadata.h"

#define XROOT_PDEV(xr)		((xr)->pdev)
#define XROOT_DEV(xr)		(&(XROOT_PDEV(xr)->dev))
#define xroot_err(xr, fmt, args...)	\
	dev_err(XROOT_DEV(xr), "%s: " fmt, __func__, ##args)
#define xroot_warn(xr, fmt, args...)	\
	dev_warn(XROOT_DEV(xr), "%s: " fmt, __func__, ##args)
#define xroot_info(xr, fmt, args...)	\
	dev_info(XROOT_DEV(xr), "%s: " fmt, __func__, ##args)
#define xroot_dbg(xr, fmt, args...)	\
	dev_dbg(XROOT_DEV(xr), "%s: " fmt, __func__, ##args)

#define XRT_VSEC_ID		0x20

#define XROOT_GRP_FIRST		(-1)
#define XROOT_GRP_LAST		(-2)

static int xroot_root_cb(struct device *, void *, u32, void *);

struct xroot_evt {
	struct list_head list;
	struct xrt_event evt;
	struct completion comp;
	bool async;
};

struct xroot_events {
	struct mutex evt_lock; /* event lock */
	struct list_head evt_list;
	struct work_struct evt_work;
};

struct xroot_grps {
	struct xrt_subdev_pool pool;
	struct work_struct bringup_work;
	atomic_t bringup_pending;
	atomic_t bringup_failed;
	struct completion bringup_comp;
};

struct xroot {
	struct pci_dev *pdev;
	struct xroot_events events;
	struct xroot_grps grps;
	struct xroot_pf_cb pf_cb;
};

struct xroot_grp_match_arg {
	enum xrt_subdev_id id;
	int instance;
};

static bool xroot_grp_match(enum xrt_subdev_id id,
			    struct platform_device *pdev, void *arg)
{
	struct xroot_grp_match_arg *a = (struct xroot_grp_match_arg *)arg;
	return id == a->id && pdev->id == a->instance;
}

static int xroot_get_group(struct xroot *xr, int instance,
			   struct platform_device **grpp)
{
	int rc = 0;
	struct xrt_subdev_pool *grps = &xr->grps.pool;
	struct device *dev = DEV(xr->pdev);
	struct xroot_grp_match_arg arg = { XRT_SUBDEV_GRP, instance };

	if (instance == XROOT_GRP_LAST) {
		rc = xrt_subdev_pool_get(grps, XRT_SUBDEV_MATCH_NEXT,
					 *grpp, dev, grpp);
	} else if (instance == XROOT_GRP_FIRST) {
		rc = xrt_subdev_pool_get(grps, XRT_SUBDEV_MATCH_PREV,
					 *grpp, dev, grpp);
	} else {
		rc = xrt_subdev_pool_get(grps, xroot_grp_match,
					 &arg, dev, grpp);
	}

	if (rc && rc != -ENOENT)
		xroot_err(xr, "failed to hold group %d: %d", instance, rc);
	return rc;
}

static void xroot_put_group(struct xroot *xr, struct platform_device *grp)
{
	int inst = grp->id;
	int rc = xrt_subdev_pool_put(&xr->grps.pool, grp, DEV(xr->pdev));

	if (rc)
		xroot_err(xr, "failed to release group %d: %d", inst, rc);
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
xroot_group_trigger_event(struct xroot *xr, int inst, enum xrt_events e)
{
	int ret;
	struct platform_device *pdev = NULL;
	struct xrt_event evt = { 0 };

	WARN_ON(inst < 0);
	/* Only triggers subdev specific events. */
	if (e != XRT_EVENT_POST_CREATION && e != XRT_EVENT_PRE_REMOVAL) {
		xroot_err(xr, "invalid event %d", e);
		return;
	}

	ret = xroot_get_group(xr, inst, &pdev);
	if (ret)
		return;

	/* Triggers event for children, first. */
	(void)xleaf_ioctl(pdev, XRT_GROUP_TRIGGER_EVENT, (void *)(uintptr_t)e);

	/* Triggers event for itself. */
	evt.xe_evt = e;
	evt.xe_subdev.xevt_subdev_id = XRT_SUBDEV_GRP;
	evt.xe_subdev.xevt_subdev_instance = inst;
	(void)xroot_trigger_event(xr, &evt, false);

	(void)xroot_put_group(xr, pdev);
}

int xroot_create_group(void *root, char *dtb)
{
	struct xroot *xr = (struct xroot *)root;
	int ret;

	atomic_inc(&xr->grps.bringup_pending);
	ret = xrt_subdev_pool_add(&xr->grps.pool, XRT_SUBDEV_GRP,
				  xroot_root_cb, xr, dtb);
	if (ret >= 0) {
		schedule_work(&xr->grps.bringup_work);
	} else {
		atomic_dec(&xr->grps.bringup_pending);
		atomic_inc(&xr->grps.bringup_failed);
		xroot_err(xr, "failed to create group: %d", ret);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(xroot_create_group);

static int xroot_destroy_single_group(struct xroot *xr, int instance)
{
	struct platform_device *pdev = NULL;
	int ret;

	WARN_ON(instance < 0);
	ret = xroot_get_group(xr, instance, &pdev);
	if (ret)
		return ret;

	xroot_group_trigger_event(xr, instance, XRT_EVENT_PRE_REMOVAL);

	/* Now tear down all children in this group. */
	ret = xleaf_ioctl(pdev, XRT_GROUP_FINI_CHILDREN, NULL);
	(void)xroot_put_group(xr, pdev);
	if (!ret) {
		ret = xrt_subdev_pool_del(&xr->grps.pool, XRT_SUBDEV_GRP,
					  instance);
	}

	return ret;
}

static int xroot_destroy_group(struct xroot *xr, int instance)
{
	struct platform_device *target = NULL;
	struct platform_device *deps = NULL;
	int ret;

	WARN_ON(instance < 0);
	/*
	 * Make sure target group exists and can't go away before
	 * we remove it's dependents
	 */
	ret = xroot_get_group(xr, instance, &target);
	if (ret)
		return ret;

	/*
	 * Remove all groups depend on target one.
	 * Assuming subdevs in higher group ID can depend on ones in
	 * lower ID groups, we remove them in the reservse order.
	 */
	while (xroot_get_group(xr, XROOT_GRP_LAST, &deps) != -ENOENT) {
		int inst = deps->id;

		xroot_put_group(xr, deps);
		if (instance == inst)
			break;
		(void)xroot_destroy_single_group(xr, inst);
		deps = NULL;
	}

	/* Now we can remove the target group. */
	xroot_put_group(xr, target);
	return xroot_destroy_single_group(xr, instance);
}

static int xroot_lookup_group(struct xroot *xr,
			      struct xrt_root_ioctl_lookup_group *arg)
{
	int rc = -ENOENT;
	struct platform_device *grp = NULL;

	while (rc < 0 && xroot_get_group(xr, XROOT_GRP_LAST, &grp) != -ENOENT) {
		if (arg->xpilp_match_cb(XRT_SUBDEV_GRP, grp,
					arg->xpilp_match_arg)) {
			rc = grp->id;
		}
		xroot_put_group(xr, grp);
	}
	return rc;
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

		(void)xrt_subdev_pool_handle_event(&xr->grps.pool, &tmp->evt);

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
	WARN_ON(!list_empty(&xr->events.evt_list));
}

static int xroot_get_leaf(struct xroot *xr, struct xrt_root_ioctl_get_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *grp = NULL;

	while (rc && xroot_get_group(xr, XROOT_GRP_LAST, &grp) != -ENOENT) {
		rc = xleaf_ioctl(grp, XRT_GROUP_GET_LEAF, arg);
		xroot_put_group(xr, grp);
	}
	return rc;
}

static int xroot_put_leaf(struct xroot *xr, struct xrt_root_ioctl_put_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *grp = NULL;

	while (rc && xroot_get_group(xr, XROOT_GRP_LAST, &grp) != -ENOENT) {
		rc = xleaf_ioctl(grp, XRT_GROUP_PUT_LEAF, arg);
		xroot_put_group(xr, grp);
	}
	return rc;
}

static int xroot_root_cb(struct device *dev, void *parg, u32 cmd, void *arg)
{
	struct xroot *xr = (struct xroot *)parg;
	int rc = 0;

	switch (cmd) {
	/* Leaf actions. */
	case XRT_ROOT_GET_LEAF: {
		struct xrt_root_ioctl_get_leaf *getleaf =
			(struct xrt_root_ioctl_get_leaf *)arg;
		rc = xroot_get_leaf(xr, getleaf);
		break;
	}
	case XRT_ROOT_PUT_LEAF: {
		struct xrt_root_ioctl_put_leaf *putleaf =
			(struct xrt_root_ioctl_put_leaf *)arg;
		rc = xroot_put_leaf(xr, putleaf);
		break;
	}
	case XRT_ROOT_GET_LEAF_HOLDERS: {
		struct xrt_root_ioctl_get_holders *holders =
			(struct xrt_root_ioctl_get_holders *)arg;
		rc = xrt_subdev_pool_get_holders(&xr->grps.pool,
						 holders->xpigh_pdev,
						 holders->xpigh_holder_buf,
						 holders->xpigh_holder_buf_len);
		break;
	}

	/* Group actions. */
	case XRT_ROOT_CREATE_GROUP:
		rc = xroot_create_group(xr, (char *)arg);
		break;
	case XRT_ROOT_REMOVE_GROUP:
		rc = xroot_destroy_group(xr, (int)(uintptr_t)arg);
		break;
	case XRT_ROOT_LOOKUP_GROUP: {
		struct xrt_root_ioctl_lookup_group *getgrp =
			(struct xrt_root_ioctl_lookup_group *)arg;
		rc = xroot_lookup_group(xr, getgrp);
		break;
	}
	case XRT_ROOT_WAIT_GROUP_BRINGUP:
		rc = xroot_wait_for_bringup(xr) ? 0 : -EINVAL;
		break;

	/* Event actions. */
	case XRT_ROOT_EVENT:
	case XRT_ROOT_EVENT_ASYNC: {
		bool async = (cmd == XRT_ROOT_EVENT_ASYNC);
		struct xrt_event *evt = (struct xrt_event *)arg;

		rc = xroot_trigger_event(xr, evt, async);
		break;
	}

	/* Device info. */
	case XRT_ROOT_GET_RESOURCE: {
		struct xrt_root_ioctl_get_res *res =
			(struct xrt_root_ioctl_get_res *)arg;
		res->xpigr_res = xr->pdev->resource;
		break;
	}
	case XRT_ROOT_GET_ID: {
		struct xrt_root_ioctl_get_id *id =
			(struct xrt_root_ioctl_get_id *)arg;

		id->xpigi_vendor_id = xr->pdev->vendor;
		id->xpigi_device_id = xr->pdev->device;
		id->xpigi_sub_vendor_id = xr->pdev->subsystem_vendor;
		id->xpigi_sub_device_id = xr->pdev->subsystem_device;
		break;
	}

	/* MISC generic PCIE driver functions. */
	case XRT_ROOT_HOT_RESET: {
		xr->pf_cb.xpc_hot_reset(xr->pdev);
		break;
	}
	case XRT_ROOT_HWMON: {
		struct xrt_root_ioctl_hwmon *hwmon =
			(struct xrt_root_ioctl_hwmon *)arg;

		if (hwmon->xpih_register) {
			hwmon->xpih_hwmon_dev =
				hwmon_device_register_with_info(DEV(xr->pdev),
								hwmon->xpih_name,
								hwmon->xpih_drvdata,
								NULL,
								hwmon->xpih_groups);
		} else {
			(void)hwmon_device_unregister(hwmon->xpih_hwmon_dev);
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

static void xroot_bringup_group_work(struct work_struct *work)
{
	struct platform_device *pdev = NULL;
	struct xroot *xr = container_of(work, struct xroot, grps.bringup_work);

	while (xroot_get_group(xr, XROOT_GRP_FIRST, &pdev) != -ENOENT) {
		int r, i;

		i = pdev->id;
		r = xleaf_ioctl(pdev, XRT_GROUP_INIT_CHILDREN, NULL);
		(void)xroot_put_group(xr, pdev);
		if (r == -EEXIST)
			continue; /* Already brough up, nothing to do. */
		if (r)
			atomic_inc(&xr->grps.bringup_failed);

		xroot_group_trigger_event(xr, i, XRT_EVENT_POST_CREATION);

		if (atomic_dec_and_test(&xr->grps.bringup_pending))
			complete(&xr->grps.bringup_comp);
	}
}

static void xroot_grps_init(struct xroot *xr)
{
	xrt_subdev_pool_init(DEV(xr->pdev), &xr->grps.pool);
	INIT_WORK(&xr->grps.bringup_work, xroot_bringup_group_work);
	atomic_set(&xr->grps.bringup_pending, 0);
	atomic_set(&xr->grps.bringup_failed, 0);
	init_completion(&xr->grps.bringup_comp);
}

static void xroot_grps_fini(struct xroot *xr)
{
	flush_scheduled_work();
	xrt_subdev_pool_fini(&xr->grps.pool);
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

	if (pci_read_config_dword(xr->pdev, cap + 8, &off_low) ||
	    pci_read_config_dword(xr->pdev, cap + 12, &off_high)) {
		xroot_err(xr, "pci_read vendor specific failed.");
		return -EINVAL;
	}

	ep.ep_name = XRT_MD_NODE_VSEC;
	ret = xrt_md_add_endpoint(dev, dtb, &ep);
	if (ret) {
		xroot_err(xr, "add vsec metadata failed, ret %d", ret);
		goto failed;
	}

	vsec_bar = cpu_to_be32(off_low & 0xf);
	ret = xrt_md_set_prop(dev, dtb, XRT_MD_NODE_VSEC, NULL,
			      XRT_MD_PROP_BAR_IDX, &vsec_bar, sizeof(vsec_bar));
	if (ret) {
		xroot_err(xr, "add vsec bar idx failed, ret %d", ret);
		goto failed;
	}

	vsec_off = cpu_to_be64(((u64)off_high << 32) | (off_low & ~0xfU));
	ret = xrt_md_set_prop(dev, dtb, XRT_MD_NODE_VSEC, NULL,
			      XRT_MD_PROP_OFFSET, &vsec_off, sizeof(vsec_off));
	if (ret) {
		xroot_err(xr, "add vsec offset failed, ret %d", ret);
		goto failed;
	}

failed:
	return ret;
}
EXPORT_SYMBOL_GPL(xroot_add_vsec_node);

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
EXPORT_SYMBOL_GPL(xroot_add_simple_node);

bool xroot_wait_for_bringup(void *root)
{
	struct xroot *xr = (struct xroot *)root;

	wait_for_completion(&xr->grps.bringup_comp);
	return atomic_xchg(&xr->grps.bringup_failed, 0) == 0;
}
EXPORT_SYMBOL_GPL(xroot_wait_for_bringup);

int xroot_probe(struct pci_dev *pdev, struct xroot_pf_cb *cb, void **root)
{
	struct device *dev = DEV(pdev);
	struct xroot *xr = NULL;

	dev_info(dev, "%s: probing...", __func__);

	xr = devm_kzalloc(dev, sizeof(*xr), GFP_KERNEL);
	if (!xr)
		return -ENOMEM;

	xr->pdev = pdev;
	xr->pf_cb = *cb;
	xroot_grps_init(xr);
	xroot_event_init(xr);

	*root = xr;
	return 0;
}
EXPORT_SYMBOL_GPL(xroot_probe);

void xroot_remove(void *root)
{
	struct xroot *xr = (struct xroot *)root;
	struct platform_device *grp = NULL;

	xroot_info(xr, "leaving...");

	if (xroot_get_group(xr, XROOT_GRP_FIRST, &grp) == 0) {
		int instance = grp->id;

		xroot_put_group(xr, grp);
		(void)xroot_destroy_group(xr, instance);
	}

	xroot_event_fini(xr);
	xroot_grps_fini(xr);
}
EXPORT_SYMBOL_GPL(xroot_remove);

void xroot_broadcast(void *root, enum xrt_events evt)
{
	struct xroot *xr = (struct xroot *)root;
	struct xrt_event e = { 0 };

	/* Root pf driver only broadcasts below two events. */
	if (evt != XRT_EVENT_POST_CREATION && evt != XRT_EVENT_PRE_REMOVAL) {
		xroot_info(xr, "invalid event %d", evt);
		return;
	}

	e.xe_evt = evt;
	e.xe_subdev.xevt_subdev_id = XRT_ROOT;
	e.xe_subdev.xevt_subdev_instance = 0;
	(void)xroot_trigger_event(xr, &e, false);
}
EXPORT_SYMBOL_GPL(xroot_broadcast);
