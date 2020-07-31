// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/vmalloc.h>
#include "xocl-subdev.h"
#include "xocl-parent.h"
#include "xocl-main.h"
#include "xocl-metadata.h"

#define DEV_IS_PCI(dev) ((dev)->bus == &pci_bus_type)

/*
 * It represents a holder of a subdev. One holder can repeatedly hold a subdev
 * as long as there is a unhold corresponding to a hold.
 */
struct xocl_subdev_holder {
	struct list_head xsh_holder_list;
	struct device *xsh_holder;
	int xsh_count;
};

/*
 * It represents a specific instance of platform driver for a subdev, which
 * provides services to its clients (another subdev driver or root driver).
 */
struct xocl_subdev {
	struct list_head xs_dev_list;
	struct list_head xs_holder_list;
	enum xocl_subdev_id xs_id;		/* type of subdev */
	struct platform_device *xs_pdev;	/* a particular subdev inst */
	struct completion xs_holder_comp;
};

static struct xocl_subdev *xocl_subdev_alloc(void)
{
	struct xocl_subdev *sdev = vzalloc(sizeof(struct xocl_subdev));

	if (!sdev)
		return NULL;

	INIT_LIST_HEAD(&sdev->xs_dev_list);
	INIT_LIST_HEAD(&sdev->xs_holder_list);
	init_completion(&sdev->xs_holder_comp);
	return sdev;
}

static void xocl_subdev_free(struct xocl_subdev *sdev)
{
	vfree(sdev);
}

/*
 * Subdev common sysfs nodes.
 */
static ssize_t holders_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t len;
	struct platform_device *pdev = to_platform_device(dev);
	struct xocl_parent_ioctl_get_holders holders = { pdev, buf, 1024 };

	len = xocl_subdev_parent_ioctl(pdev, XOCL_PARENT_GET_HOLDERS, &holders);
	if (len >= holders.xpigh_holder_buf_len)
		return len;
	buf[len] = '\n';
	return len + 1;
}
static DEVICE_ATTR_RO(holders);

static struct attribute *xocl_subdev_attrs[] = {
	&dev_attr_holders.attr,
	NULL,
};

static const struct attribute_group xocl_subdev_attrgroup = {
	.attrs = xocl_subdev_attrs,
};

struct xocl_subdev *
xocl_subdev_create(struct device *parent, enum xocl_subdev_id id,
	xocl_subdev_parent_cb_t pcb, char *dtb)
{
	struct xocl_subdev *sdev = NULL;
	struct platform_device *pdev = NULL;
	struct xocl_subdev_platdata *pdata = NULL;
	size_t dtb_len = dtb ? xocl_md_size(parent, dtb) : 0;
	size_t pdata_sz = sizeof(struct xocl_subdev_platdata) + dtb_len - 1;
	int inst = PLATFORM_DEVID_NONE;

	sdev = xocl_subdev_alloc();
	if (!sdev) {
		dev_err(parent, "failed to alloc subdev for ID %d", id);
		goto fail;
	}
	sdev->xs_id = id;

	/* Prepare platform data passed to subdev. */
	pdata = vzalloc(pdata_sz);
	if (!pdata)
		goto fail;

	pdata->xsp_parent_cb = pcb;
	(void) memcpy(pdata->xsp_dtb, dtb, dtb_len);
	if (id == XOCL_SUBDEV_PART) {
		/* Partition can only be created by root driver. */
		BUG_ON(parent->bus != &pci_bus_type);
		pdata->xsp_root_name = dev_name(parent);
	} else {
		struct platform_device *part = to_platform_device(parent);
		/* Leaf can only be created by partition driver. */
		BUG_ON(parent->bus != &platform_bus_type);
		BUG_ON(strcmp(xocl_drv_name(XOCL_SUBDEV_PART),
			platform_get_device_id(part)->name));
		pdata->xsp_root_name = DEV_PDATA(part)->xsp_root_name;
	}

	/* Obtain dev instance number. */
	inst = xocl_drv_get_instance(id);
	if (inst < 0) {
		dev_err(parent, "failed to obtain instance: %d", inst);
		goto fail;
	}

	/* Create subdev. */
	if (id == XOCL_SUBDEV_PART) {
		pdev = platform_device_register_data(parent,
			xocl_drv_name(XOCL_SUBDEV_PART), inst, pdata, pdata_sz);
	} else {
		pdev = platform_device_register_resndata(parent,
			xocl_drv_name(id), inst,
			NULL, 0, /* TODO: find out IO and IRQ res from dtb */
			pdata, pdata_sz);
	}
	if (IS_ERR(pdev)) {
		dev_err(parent, "failed to create subdev for %s inst %d: %ld",
			xocl_drv_name(id), inst, PTR_ERR(pdev));
		goto fail;
	}
	sdev->xs_pdev = pdev;

	if (device_attach(DEV(pdev)) != 1) {
		xocl_err(pdev, "failed to attach");
		goto fail;
	}

	if (sysfs_create_group(&DEV(pdev)->kobj, &xocl_subdev_attrgroup))
		xocl_err(pdev, "failed to create sysfs group");

	vfree(pdata);
	return sdev;

fail:
	vfree(pdata);
	if (sdev && !IS_ERR_OR_NULL(sdev->xs_pdev))
		platform_device_unregister(sdev->xs_pdev);
	if (inst >= 0)
		xocl_drv_put_instance(id, inst);
	xocl_subdev_free(sdev);
	return NULL;
}

void xocl_subdev_destroy(struct xocl_subdev *sdev)
{
	struct platform_device *pdev = sdev->xs_pdev;
	int inst = pdev->id;

	(void) sysfs_remove_group(&DEV(pdev)->kobj, &xocl_subdev_attrgroup);
	platform_device_unregister(pdev);
	xocl_drv_put_instance(sdev->xs_id, inst);
	xocl_subdev_free(sdev);
}

int xocl_subdev_parent_ioctl(struct platform_device *self, u32 cmd, void *arg)
{
	struct device *dev = DEV(self);
	struct xocl_subdev_platdata *pdata = DEV_PDATA(self);

	return (*pdata->xsp_parent_cb)(dev->parent, cmd, arg);
}

int xocl_subdev_ioctl(struct platform_device *tgt, u32 cmd, void *arg)
{
	struct xocl_subdev_drvdata *drvdata = DEV_DRVDATA(tgt);

	return (*drvdata->xsd_dev_ops.xsd_ioctl)(tgt, cmd, arg);
}
EXPORT_SYMBOL_GPL(xocl_subdev_ioctl);

struct platform_device *
xocl_subdev_get_leaf(struct platform_device *pdev,
	xocl_subdev_match_t match_cb, void *match_arg)
{
	int rc;
	struct xocl_parent_ioctl_get_leaf get_leaf = {
		pdev, match_cb, match_arg, };

	rc = xocl_subdev_parent_ioctl(pdev, XOCL_PARENT_GET_LEAF, &get_leaf);
	if (rc)
		return NULL;
	return get_leaf.xpigl_leaf;
}

struct subdev_match_arg {
	enum xocl_subdev_id id;
	int instance;
};

static bool subdev_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	struct subdev_match_arg *a = (struct subdev_match_arg *)arg;
	return id == a->id &&
		(pdev->id == a->instance || PLATFORM_DEVID_NONE == a->instance);
}

struct platform_device *
xocl_subdev_get_leaf_by_id(struct platform_device *pdev,
	enum xocl_subdev_id id, int instance)
{
	struct subdev_match_arg arg = { id, instance };

	return xocl_subdev_get_leaf(pdev, subdev_match, &arg);
}

int xocl_subdev_put_leaf(struct platform_device *pdev,
	struct platform_device *leaf)
{
	struct xocl_parent_ioctl_put_leaf put_leaf = { pdev, leaf };

	return xocl_subdev_parent_ioctl(pdev, XOCL_PARENT_PUT_LEAF, &put_leaf);
}

int xocl_subdev_create_partition(struct platform_device *pdev, char *dtb)
{
	return xocl_subdev_parent_ioctl(pdev,
		XOCL_PARENT_CREATE_PARTITION, dtb);
}

int xocl_subdev_destroy_partition(struct platform_device *pdev, int instance)
{
	return xocl_subdev_parent_ioctl(pdev,
		XOCL_PARENT_REMOVE_PARTITION, (void *)(uintptr_t)instance);
}

void *xocl_subdev_add_event_cb(struct platform_device *pdev,
	xocl_subdev_match_t match, void *match_arg, xocl_event_cb_t cb)
{
	struct xocl_parent_ioctl_evt_cb c = { pdev, match, match_arg, cb };

	(void) xocl_subdev_parent_ioctl(pdev, XOCL_PARENT_ADD_EVENT_CB, &c);
	return c.xevt_hdl;
}

void xocl_subdev_remove_event_cb(struct platform_device *pdev, void *hdl)
{
	(void) xocl_subdev_parent_ioctl(pdev, XOCL_PARENT_REMOVE_EVENT_CB, hdl);
}

static ssize_t
xocl_subdev_get_holders(struct xocl_subdev *sdev, char *buf, size_t len)
{
	const struct list_head *ptr;
	struct xocl_subdev_holder *h;
	ssize_t n = 0;

	list_for_each(ptr, &sdev->xs_holder_list) {
		h = list_entry(ptr, struct xocl_subdev_holder, xsh_holder_list);
		n += snprintf(buf + n, len - n, "%s:%d ",
			dev_name(h->xsh_holder), h->xsh_count);
		if (n >= len)
			break;
	}
	return n;
}

void xocl_subdev_pool_init(struct device *dev, struct xocl_subdev_pool *spool)
{
	INIT_LIST_HEAD(&spool->xpool_dev_list);
	spool->xpool_owner = dev;
	mutex_init(&spool->xpool_lock);
	spool->xpool_closing = false;
}
EXPORT_SYMBOL_GPL(xocl_subdev_pool_init);

static void xocl_subdev_pool_wait_for_holders(struct xocl_subdev_pool *spool,
	struct xocl_subdev *sdev)
{
	const struct list_head *ptr, *next;
	char holders[128];
	struct xocl_subdev_holder *holder;
	struct mutex *lk = &spool->xpool_lock;

	BUG_ON(!mutex_is_locked(lk));

	while (!list_empty(&sdev->xs_holder_list)) {
		int rc;

		/* It's most likely a bug if we ever enters this loop. */
		(void) xocl_subdev_get_holders(sdev, holders, sizeof(holders));
		xocl_err(sdev->xs_pdev, "awaits holders: %s", holders);
		mutex_unlock(lk);
		rc = wait_for_completion_killable(&sdev->xs_holder_comp);
		mutex_lock(lk);
		if (rc == -ERESTARTSYS) {
			xocl_err(sdev->xs_pdev,
				"give up on waiting for holders, clean up now");
			list_for_each_safe(ptr, next, &sdev->xs_holder_list) {
				holder = list_entry(ptr,
					struct xocl_subdev_holder,
					xsh_holder_list);
				list_del(&holder->xsh_holder_list);
				vfree(holder);
			}
		}
	}
}

int xocl_subdev_pool_fini(struct xocl_subdev_pool *spool)
{
	int ret = 0;
	struct list_head *dl = &spool->xpool_dev_list;
	struct mutex *lk = &spool->xpool_lock;

	mutex_lock(lk);

	if (spool->xpool_closing) {
		mutex_unlock(lk);
		return 0;
	}

	spool->xpool_closing = true;
	/* Remove subdev in the reverse order of added. */
	while (!ret && !list_empty(dl)) {
		struct xocl_subdev *sdev = list_first_entry(dl,
			struct xocl_subdev, xs_dev_list);
		xocl_subdev_pool_wait_for_holders(spool, sdev);
		list_del(&sdev->xs_dev_list);
		mutex_unlock(lk);
		xocl_subdev_destroy(sdev);
		mutex_lock(lk);
	}

	mutex_unlock(lk);

	return ret;
}
EXPORT_SYMBOL_GPL(xocl_subdev_pool_fini);

static int xocl_subdev_hold(struct xocl_subdev *sdev, struct device *holder_dev)
{
	const struct list_head *ptr;
	struct list_head *hl = &sdev->xs_holder_list;
	struct xocl_subdev_holder *holder;
	bool found = false;

	list_for_each(ptr, hl) {
		holder = list_entry(ptr, struct xocl_subdev_holder,
			xsh_holder_list);
		if (holder->xsh_holder == holder_dev) {
			holder->xsh_count++;
			found = true;
			break;
		}
	}

	if (!found) {
		holder = vzalloc(sizeof(*holder));
		if (!holder)
			return -ENOMEM;
		holder->xsh_holder = holder_dev;
		holder->xsh_count = 1;
		list_add_tail(&holder->xsh_holder_list, hl);
	}

	return holder->xsh_count;
}

static int
xocl_subdev_release(struct xocl_subdev *sdev, struct device *holder_dev)
{
	const struct list_head *ptr, *next;
	struct list_head *hl = &sdev->xs_holder_list;
	struct xocl_subdev_holder *holder;
	int count;
	bool found = false;

	list_for_each_safe(ptr, next, hl) {
		holder = list_entry(ptr, struct xocl_subdev_holder,
			xsh_holder_list);
		if (holder->xsh_holder == holder_dev) {
			found = true;
			holder->xsh_count--;

			count = holder->xsh_count;
			if (count == 0) {
				list_del(&holder->xsh_holder_list);
				vfree(holder);
				if (list_empty(hl))
					complete(&sdev->xs_holder_comp);
			}
			break;
		}
	}
	if (!found) {
		dev_err(holder_dev, "can't release, %s did not hold %s",
			dev_name(holder_dev),
			dev_name(DEV(sdev->xs_pdev)));
	}
	return found ? count : -EINVAL;
}

int xocl_subdev_pool_add(struct xocl_subdev_pool *spool, enum xocl_subdev_id id,
	xocl_subdev_parent_cb_t pcb, char *dtb)
{
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xocl_subdev *sdev;
	int ret = 0;

	sdev = xocl_subdev_create(spool->xpool_owner, id, pcb, dtb);
	if (sdev) {
		mutex_lock(lk);
		if (spool->xpool_closing) {
			/* No new subdev when pool is going away. */
			xocl_err(sdev->xs_pdev, "pool is closing");
			ret = -ENODEV;
		} else {
			list_add(&sdev->xs_dev_list, dl);
		}
		mutex_unlock(lk);
		if (ret)
			xocl_subdev_destroy(sdev);
	} else {
		ret = -EINVAL;
	}

	return ret ? ret : sdev->xs_pdev->id;
}
EXPORT_SYMBOL_GPL(xocl_subdev_pool_add);

int xocl_subdev_pool_del(struct xocl_subdev_pool *spool, enum xocl_subdev_id id,
	int instance)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xocl_subdev *sdev;
	int ret = -ENOENT;

	mutex_lock(lk);
	list_for_each(ptr, dl) {
		sdev = list_entry(ptr, struct xocl_subdev, xs_dev_list);
		if (sdev->xs_id != id || sdev->xs_pdev->id != instance)
			continue;
		xocl_subdev_pool_wait_for_holders(spool, sdev);
		list_del(&sdev->xs_dev_list);
		ret = 0;
		break;
	}
	mutex_unlock(lk);
	if (ret)
		return ret;

	xocl_subdev_destroy(sdev);
	return 0;
}
EXPORT_SYMBOL_GPL(xocl_subdev_pool_del);

static int xocl_subdev_pool_get_impl(struct xocl_subdev_pool *spool,
	xocl_subdev_match_t match, void *arg, struct device *holder_dev,
	struct xocl_subdev **sdevp)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xocl_subdev *sdev = NULL;
	int ret = -ENOENT;

	mutex_lock(lk);

	if (match == XOCL_SUBDEV_MATCH_PREV) {
		struct platform_device *pdev = (struct platform_device *)arg;
		struct xocl_subdev *d = NULL;

		if (!pdev) {
			sdev = list_empty(dl) ? NULL : list_last_entry(dl,
				struct xocl_subdev, xs_dev_list);
		} else {
			list_for_each(ptr, dl) {
				d = list_entry(ptr, struct xocl_subdev,
					xs_dev_list);
				if (d->xs_pdev != pdev)
					continue;
				if (!list_is_first(ptr, dl))
					sdev = list_prev_entry(d, xs_dev_list);
				break;
			}
		}
	} else if (match == XOCL_SUBDEV_MATCH_NEXT) {
		struct platform_device *pdev = (struct platform_device *)arg;
		struct xocl_subdev *d = NULL;

		if (!pdev) {
			sdev = list_first_entry_or_null(dl,
				struct xocl_subdev, xs_dev_list);
		} else {
			list_for_each(ptr, dl) {
				d = list_entry(ptr, struct xocl_subdev,
					xs_dev_list);
				if (d->xs_pdev != pdev)
					continue;
				if (!list_is_last(ptr, dl))
					sdev = list_next_entry(d, xs_dev_list);
				break;
			}
		}
	} else {
		list_for_each(ptr, dl) {
			struct xocl_subdev *d = NULL;

			d = list_entry(ptr, struct xocl_subdev, xs_dev_list);
			if (d && !match(d->xs_id, d->xs_pdev, arg))
				continue;
			sdev = d;
			break;
		}
	}

	if (sdev)
		ret = xocl_subdev_hold(sdev, holder_dev);

	mutex_unlock(lk);

	if (ret >= 0)
		*sdevp = sdev;
	return ret;
}
EXPORT_SYMBOL_GPL(xocl_subdev_pool_get);

int xocl_subdev_pool_get(struct xocl_subdev_pool *spool,
	xocl_subdev_match_t match, void *arg, struct device *holder_dev,
	struct platform_device **pdevp)
{
	int rc;
	struct xocl_subdev *sdev;

	rc = xocl_subdev_pool_get_impl(spool, match, arg, holder_dev, &sdev);
	if (rc < 0) {
		if (rc != -ENOENT)
			dev_err(holder_dev, "failed to hold device: %d", rc);
		return rc;
	}

	if (DEV_IS_PCI(holder_dev)) {
#ifdef	SUBDEV_DEBUG
		dev_info(holder_dev, "%s: %s <<==== %s, ref=%d", __func__,
			dev_name(holder_dev), dev_name(DEV(sdev->xs_pdev)), rc);
#endif
	} else {
		xocl_info(to_platform_device(holder_dev), "%s <<==== %s",
			dev_name(holder_dev), dev_name(DEV(sdev->xs_pdev)));
	}

	*pdevp = sdev->xs_pdev;
	return 0;
}

static int xocl_subdev_pool_put_impl(struct xocl_subdev_pool *spool,
	struct platform_device *pdev, struct device *holder_dev)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xocl_subdev *sdev;
	int ret = -ENOENT;

	mutex_lock(lk);
	list_for_each(ptr, dl) {
		sdev = list_entry(ptr, struct xocl_subdev, xs_dev_list);
		if (sdev->xs_pdev != pdev)
			continue;
		ret = xocl_subdev_release(sdev, holder_dev);
		break;
	}
	mutex_unlock(lk);

	if (ret < 0 && ret != -ENOENT)
		dev_err(holder_dev, "failed to release device: %d", ret);
	return ret;
}

int xocl_subdev_pool_put(struct xocl_subdev_pool *spool,
	struct platform_device *pdev, struct device *holder_dev)
{
	int ret = xocl_subdev_pool_put_impl(spool, pdev, holder_dev);

	if (ret < 0)
		return ret;

	if (DEV_IS_PCI(holder_dev)) {
#ifdef	SUBDEV_DEBUG
		dev_info(holder_dev, "%s: %s <<==X== %s, ref=%d", __func__,
			dev_name(holder_dev), dev_name(DEV(spdev)), ret);
#endif
	} else {
		struct platform_device *d = to_platform_device(holder_dev);

		xocl_info(d, "%s <<==X== %s",
			dev_name(holder_dev), dev_name(DEV(pdev)));
	}
	return 0;
}
EXPORT_SYMBOL_GPL(xocl_subdev_pool_put);

int xocl_subdev_pool_event(struct xocl_subdev_pool *spool,
	struct platform_device *pdev, xocl_subdev_match_t match, void *arg,
	xocl_event_cb_t xevt_cb, enum xocl_events evt)
{
	int rc = 0;
	struct platform_device *tgt = NULL;
	struct xocl_subdev *sdev = NULL;

	while (!rc && xocl_subdev_pool_get_impl(spool, XOCL_SUBDEV_MATCH_NEXT,
		tgt, DEV(pdev), &sdev) != -ENOENT) {
		tgt = sdev->xs_pdev;
		if (match(sdev->xs_id, sdev->xs_pdev, arg))
			rc = xevt_cb(pdev, evt, sdev->xs_id, tgt->id);
		(void) xocl_subdev_pool_put_impl(spool, tgt, DEV(pdev));
	}
	return rc;
}

ssize_t xocl_subdev_pool_get_holders(struct xocl_subdev_pool *spool,
	struct platform_device *pdev, char *buf, size_t len)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xocl_subdev *sdev;
	ssize_t ret = 0;

	mutex_lock(lk);
	list_for_each(ptr, dl) {
		sdev = list_entry(ptr, struct xocl_subdev, xs_dev_list);
		if (sdev->xs_pdev != pdev)
			continue;
		ret = xocl_subdev_get_holders(sdev, buf, len);
		break;
	}
	mutex_unlock(lk);

	return ret;
}
EXPORT_SYMBOL_GPL(xocl_subdev_pool_get_holders);

void xocl_subdev_broadcast_event(struct platform_device *pdev,
	enum xocl_events evt)
{
	(void) xocl_subdev_parent_ioctl(pdev,
		XOCL_PARENT_BOARDCAST_EVENT, (void *)evt);
}

int xocl_subdev_add_by_metadata(struct platform_device *pdev,
	struct xocl_subdev_pool *spool, xocl_subdev_parent_cb_t pcb)
{
	struct xocl_subdev_platdata *pdata = DEV_PDATA(pdev);
	enum xocl_subdev_id did;
	struct xocl_subdev_endpoints *eps;
	int ep_count = 0, i, ret;
	ulong mlen;
	char *dtb, *part_dtb;

	if (!pdata || !pdata->xsp_dtb)
		return 0;

	mlen = xocl_md_size(&pdev->dev, pdata->xsp_dtb);
	if (!mlen)
		return -EINVAL;

	part_dtb = vmalloc(mlen);
	if (!part_dtb)
		return -ENOMEM;
	memcpy(part_dtb, pdata->xsp_dtb, mlen);

	for (did = 0; did < XOCL_SUBDEV_NUM; did++) {
		eps = xocl_drv_get_endpoints(did);
		if (!eps || !eps->xse_names)
			continue;
		ret = xocl_md_create(&pdev->dev, &dtb);
		if (ret) {
			xocl_err(pdev, "create md failed, did %s",
				xocl_drv_name(did));
			continue;
		}
		for (i = 0; eps->xse_names[i].ep_name; i++) {
			ret = xocl_md_copy_endpoint(&pdev->dev,
				&dtb, part_dtb,
				(char *)eps->xse_names[i].ep_name,
				(char *)eps->xse_names[i].regmap_name);
			if (ret)
				continue;
			xocl_md_del_endpoint(&pdev->dev, &part_dtb,
				(char *)eps->xse_names[i].ep_name,
				(char *)eps->xse_names[i].regmap_name);
			ep_count++;
		}
		if (ep_count >= eps->xse_min_ep)
			xocl_subdev_pool_add(spool, did, pcb, dtb);
		else if (ep_count > 0) {
			xocl_md_overlay(&pdev->dev, &part_dtb, -1,
				dtb, -1);
		}
	}

	return 0;
}
