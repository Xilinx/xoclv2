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
#include "xocl-subdev.h"
#include "xocl-parent.h"
#include "xocl-partition.h"
#include "xocl-root.h"
#include "xocl-metadata.h"
#include "xocl-root.h"

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

static int xroot_parent_cb(struct device *, void *, u32, void *);

struct xroot_event_cb {
	struct list_head list;
	bool initialized;
	struct xocl_parent_ioctl_evt_cb cb;
};

struct xroot_events {
	struct list_head cb_list;
	struct mutex lock;
	struct work_struct cb_init_work;
};

struct xroot_parts {
	struct xocl_subdev_pool pool;
	struct work_struct bringup_work;
};

struct xroot {
	struct pci_dev *pdev;
	struct xroot_events events;
	struct xroot_parts parts;
};

struct xroot_part_match_arg {
	enum xocl_subdev_id id;
	int instance;
};

static bool xroot_part_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	struct xroot_part_match_arg *a = (struct xroot_part_match_arg *)arg;
	return id == a->id && pdev->id == a->instance;
}

static int xroot_get_partition(struct xroot *xr, int instance,
	struct platform_device **partp)
{
	int rc = 0;
	struct xocl_subdev_pool *parts = &xr->parts.pool;
	struct device *dev = DEV(xr->pdev);
	struct xroot_part_match_arg arg = { XOCL_SUBDEV_PART, instance };

	if (instance == PLATFORM_DEVID_NONE) {
		rc = xocl_subdev_pool_get(parts, XOCL_SUBDEV_MATCH_PREV,
			*partp, dev, partp);
	} else {
		rc = xocl_subdev_pool_get(parts, xroot_part_match,
			&arg, dev, partp);
	}

	if (rc && rc != -ENOENT)
		xroot_err(xr, "failed to hold partition %d: %d", instance, rc);
	return rc;
}

static void xroot_put_partition(struct xroot *xr, struct platform_device *part)
{
	int inst = part->id;
	int rc = xocl_subdev_pool_put(&xr->parts.pool, part, DEV(xr->pdev));

	if (rc)
		xroot_err(xr, "failed to release partition %d: %d", inst, rc);
}

static int
xroot_partition_trigger_evt(struct xroot *xr, struct xroot_event_cb *cb,
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
xroot_event_partition(struct xroot *xr, int instance, enum xocl_events evt)
{
	int ret;
	struct platform_device *pdev = NULL;
	const struct list_head *ptr, *next;
	struct xroot_event_cb *tmp;

	ret = xroot_get_partition(xr, instance, &pdev);
	if (ret)
		return;

	mutex_lock(&xr->events.lock);
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
	mutex_unlock(&xr->events.lock);

	(void) xroot_put_partition(xr, pdev);
}

int xroot_create_partition(void *root, char *dtb)
{
	struct xroot *xr = (struct xroot *)root;
	int ret = xocl_subdev_pool_add(&xr->parts.pool,
		XOCL_SUBDEV_PART, xroot_parent_cb, xr, dtb);

	if (ret >= 0)
		schedule_work(&xr->parts.bringup_work);
	return ret;
}

static int xroot_destroy_partition(struct xroot *xr, int instance)
{
	struct platform_device *pdev = NULL;
	int ret;

	ret = xroot_get_partition(xr, instance, &pdev);
	if (ret)
		return ret;

	xroot_event_partition(xr, instance, XOCL_EVENT_PRE_REMOVAL);

	/* Now tear down all children in this partition. */
	ret = xocl_subdev_ioctl(pdev, XOCL_PARTITION_FINI_CHILDREN, NULL);
	(void) xroot_put_partition(xr, pdev);
	if (!ret) {
		ret = xocl_subdev_pool_del(&xr->parts.pool,
			XOCL_SUBDEV_PART, instance);
	}

	return ret;
}

static void xroot_evt_cb_init_work(struct work_struct *work)
{
	const struct list_head *ptr, *next;
	struct xroot_event_cb *tmp;
	struct platform_device *part = NULL;
	struct xroot *xr =
		container_of(work, struct xroot, events.cb_init_work);

	mutex_lock(&xr->events.lock);

	list_for_each_safe(ptr, next, &xr->events.cb_list) {
		tmp = list_entry(ptr, struct xroot_event_cb, list);
		if (tmp->initialized)
			continue;

		while (xroot_get_partition(xr, PLATFORM_DEVID_NONE,
			&part) != -ENOENT) {
			if (xroot_partition_trigger_evt(xr, tmp, part,
				XOCL_EVENT_POST_CREATION)) {
				list_del(&tmp->list);
				vfree(tmp);
				tmp = NULL;
				break;
			}
			xroot_put_partition(xr, part);
		}

		if (tmp)
			tmp->initialized = true;
	}

	mutex_unlock(&xr->events.lock);
}

static void xroot_evt_broadcast(struct xroot *xr, enum xocl_events evt)
{
	const struct list_head *ptr, *next;
	struct xroot_event_cb *tmp;
	int rc;

	mutex_lock(&xr->events.lock);

	list_for_each_safe(ptr, next, &xr->events.cb_list) {
		tmp = list_entry(ptr, struct xroot_event_cb, list);
		rc = tmp->cb.xevt_cb(tmp->cb.xevt_pdev, evt, -1, -1);
		if (rc) {
			list_del(&tmp->list);
			vfree(tmp);
		}
	}

	mutex_unlock(&xr->events.lock);
}

static void xroot_evt_init(struct xroot *xr)
{
	INIT_LIST_HEAD(&xr->events.cb_list);
	mutex_init(&xr->events.lock);
	INIT_WORK(&xr->events.cb_init_work, xroot_evt_cb_init_work);
}

static void xroot_evt_fini(struct xroot *xr)
{
	const struct list_head *ptr, *next;
	struct xroot_event_cb *tmp;

	flush_scheduled_work();

	mutex_lock(&xr->events.lock);
	list_for_each_safe(ptr, next, &xr->events.cb_list) {
		tmp = list_entry(ptr, struct xroot_event_cb, list);
		list_del(&tmp->list);
		vfree(tmp);
	}
	mutex_unlock(&xr->events.lock);
}

static int xroot_evt_cb_add(struct xroot *xr,
	struct xocl_parent_ioctl_evt_cb *cb)
{
	struct xroot_event_cb *new = vzalloc(sizeof(*new));

	if (!new)
		return -ENOMEM;

	cb->xevt_hdl = new;
	new->cb = *cb;
	new->initialized = false;

	mutex_lock(&xr->events.lock);
	list_add(&new->list, &xr->events.cb_list);
	mutex_unlock(&xr->events.lock);

	schedule_work(&xr->events.cb_init_work);
	return 0;
}

static void xroot_evt_cb_del(struct xroot *xr, void *hdl)
{
	struct xroot_event_cb *cb = (struct xroot_event_cb *)hdl;
	const struct list_head *ptr;
	struct xroot_event_cb *tmp;

	mutex_lock(&xr->events.lock);
	list_for_each(ptr, &xr->events.cb_list) {
		tmp = list_entry(ptr, struct xroot_event_cb, list);
		if (tmp == cb)
			break;
	}
	list_del(&cb->list);
	mutex_unlock(&xr->events.lock);
	vfree(cb);
}

static int xroot_get_leaf(struct xroot *xr,
	struct xocl_parent_ioctl_get_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *part = NULL;

	while (rc && xroot_get_partition(xr, PLATFORM_DEVID_NONE,
		&part) != -ENOENT) {
		rc = xocl_subdev_ioctl(part, XOCL_PARTITION_GET_LEAF, arg);
		xroot_put_partition(xr, part);
	}
	return rc;
}

static int xroot_put_leaf(struct xroot *xr,
	struct xocl_parent_ioctl_put_leaf *arg)
{
	int rc = -ENOENT;
	struct platform_device *part = NULL;

	while (rc && xroot_get_partition(xr, PLATFORM_DEVID_NONE,
		&part) != -ENOENT) {
		rc = xocl_subdev_ioctl(part, XOCL_PARTITION_PUT_LEAF, arg);
		xroot_put_partition(xr, part);
	}
	return rc;
}

static int xroot_parent_cb(struct device *dev, void *parg, u32 cmd, void *arg)
{
	struct xroot *xr = (struct xroot *)parg;
	int rc = 0;

	switch (cmd) {
	case XOCL_PARENT_GET_LEAF: {
		struct xocl_parent_ioctl_get_leaf *getleaf =
			(struct xocl_parent_ioctl_get_leaf *)arg;
		rc = xroot_get_leaf(xr, getleaf);
		break;
	}
	case XOCL_PARENT_PUT_LEAF: {
		struct xocl_parent_ioctl_put_leaf *putleaf =
			(struct xocl_parent_ioctl_put_leaf *)arg;
		rc = xroot_put_leaf(xr, putleaf);
		break;
	}
	case XOCL_PARENT_CREATE_PARTITION:
		rc = xroot_create_partition(xr, (char *)arg);
		break;
	case XOCL_PARENT_REMOVE_PARTITION:
		rc = xroot_destroy_partition(xr, (int)(uintptr_t)arg);
		break;
	case XOCL_PARENT_ADD_EVENT_CB: {
		struct xocl_parent_ioctl_evt_cb *cb =
			(struct xocl_parent_ioctl_evt_cb *)arg;
		rc = xroot_evt_cb_add(xr, cb);
		break;
	}
	case XOCL_PARENT_REMOVE_EVENT_CB:
		xroot_evt_cb_del(xr, arg);
		rc = 0;
		break;
	case XOCL_PARENT_BOARDCAST_EVENT:
		xroot_evt_broadcast(xr, (enum xocl_events)(uintptr_t)arg);
		break;
	case XOCL_PARENT_GET_HOLDERS: {
		struct xocl_parent_ioctl_get_holders *holders =
			(struct xocl_parent_ioctl_get_holders *)arg;
		rc = xocl_subdev_pool_get_holders(&xr->parts.pool,
			holders->xpigh_pdev, holders->xpigh_holder_buf,
			holders->xpigh_holder_buf_len);
		break;
	}
	case XOCL_PARENT_GET_BAR: {
		struct xocl_parent_ioctl_get_bar *bars =
			(struct xocl_parent_ioctl_get_bar *)arg;
		int i;

		for (i = 0; i <= PCI_STD_RESOURCE_END; i++) {
			((ulong *)bars->xpigb_bar_addrs)[i] =
				(ulong)pci_resource_start(xr->pdev, i);
			bars->xpigb_bar_len[i] =
				(ulong)pci_resource_len(xr->pdev, i);
		}
		break;
	}
	case XOCL_PARENT_HOT_RESET: {
		xroot_evt_broadcast(xr, XOCL_EVENT_PRE_HOT_RESET);
		xroot_hot_reset(xr->pdev);
		xroot_evt_broadcast(xr, XOCL_EVENT_POST_HOT_RESET);
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

	while (xroot_get_partition(xr, PLATFORM_DEVID_NONE, &pdev) != -ENOENT) {
		int r, i;

		i = pdev->id;
		r = xocl_subdev_ioctl(pdev, XOCL_PARTITION_INIT_CHILDREN, NULL);
		(void) xroot_put_partition(xr, pdev);
		if (!r)
			xroot_event_partition(xr, i, XOCL_EVENT_POST_CREATION);
	}
}

static void xroot_parts_init(struct xroot *xr)
{
	xocl_subdev_pool_init(DEV(xr->pdev), &xr->parts.pool);
	INIT_WORK(&xr->parts.bringup_work, xroot_bringup_partition_work);
}

static void xroot_parts_fini(struct xroot *xr)
{
	flush_scheduled_work();
	(void) xocl_subdev_pool_fini(&xr->parts.pool);
}

int xroot_add_vsec_node(void *root, char **dtb)
{
	struct xroot *xr = (struct xroot *)root;
	struct device *dev = DEV(xr->pdev);
	struct xocl_md_endpoint ep = { 0 };
	int cap, ret = 0;
	u32 off_low, off_high, vsec_bar;
	u64 vsec_off;

	cap = pci_find_ext_capability(xr->pdev, PCI_EXT_CAP_ID_VNDR);
	if (!cap) {
		xroot_info(xr, "No Vendor Specific Capability.");
		return 0;
	}

	if (pci_read_config_dword(xr->pdev, cap+8, &off_low) ||
	    pci_read_config_dword(xr->pdev, cap+12, &off_high)) {
		xroot_err(xr, "pci_read vendor specific failed.");
		return -EINVAL;
	}

	ep.ep_name = NODE_VSEC;
	ret = xocl_md_add_endpoint(dev, dtb, &ep);
	if (ret) {
		xroot_err(xr, "add vsec metadata failed, ret %d", ret);
		goto failed;
	}

	vsec_bar = cpu_to_be32(off_low & 0xf);
	ret = xocl_md_set_prop(dev, dtb, NODE_VSEC,
		NULL, PROP_BAR_IDX, &vsec_bar, sizeof(vsec_bar));
	if (ret) {
		xroot_err(xr, "add vsec bar idx failed, ret %d", ret);
		goto failed;
	}

	vsec_off = cpu_to_be64(((u64)off_high << 32) | (off_low & ~0xfU));
	ret = xocl_md_set_prop(dev, dtb, NODE_VSEC,
		NULL, PROP_OFFSET, &vsec_off, sizeof(vsec_off));
	if (ret) {
		xroot_err(xr, "add vsec offset failed, ret %d", ret);
		goto failed;
	}

failed:
	return ret;
}

int xroot_add_simple_node(void *root, char **dtb, const char *endpoint)
{
	struct xroot *xr = (struct xroot *)root;
	struct device *dev = DEV(xr->pdev);
	struct xocl_md_endpoint ep = { 0 };
	int ret = 0;

	ep.ep_name = endpoint;
	ret = xocl_md_add_endpoint(dev, dtb, &ep);
	if (ret)
		xroot_err(xr, "add %s failed, ret %d", endpoint, ret);

	return ret;
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

	/*
	 * Assuming subdevs in higher partition ID can depend on ones in
	 * lower ID partitions, we remove them in the reservse order.
	 */
	while (xroot_get_partition(xr, PLATFORM_DEVID_NONE, &part) != -ENOENT) {
		int instance = part->id;

		xroot_put_partition(xr, part);
		(void) xroot_destroy_partition(xr, instance);
		part = NULL;
	}

	xroot_evt_fini(xr);
	xroot_parts_fini(xr);
}
