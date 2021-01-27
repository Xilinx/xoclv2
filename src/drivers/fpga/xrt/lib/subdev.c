// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/vmalloc.h>
#include "xleaf.h"
#include "subdev_pool.h"
#include "main.h"
#include "metadata.h"

#define DEV_IS_PCI(dev) ((dev)->bus == &pci_bus_type)
static inline struct device *find_root(struct platform_device *pdev)
{
	struct device *d = DEV(pdev);

	while (!DEV_IS_PCI(d))
		d = d->parent;
	return d;
}

/*
 * It represents a holder of a subdev. One holder can repeatedly hold a subdev
 * as long as there is a unhold corresponding to a hold.
 */
struct xrt_subdev_holder {
	struct list_head xsh_holder_list;
	struct device *xsh_holder;
	int xsh_count;
};

/*
 * It represents a specific instance of platform driver for a subdev, which
 * provides services to its clients (another subdev driver or root driver).
 */
struct xrt_subdev {
	struct list_head xs_dev_list;
	struct list_head xs_holder_list;
	enum xrt_subdev_id xs_id;		/* type of subdev */
	struct platform_device *xs_pdev;	/* a particular subdev inst */
	struct completion xs_holder_comp;
};

static struct xrt_subdev *xrt_subdev_alloc(void)
{
	struct xrt_subdev *sdev = vzalloc(sizeof(*sdev));

	if (!sdev)
		return NULL;

	INIT_LIST_HEAD(&sdev->xs_dev_list);
	INIT_LIST_HEAD(&sdev->xs_holder_list);
	init_completion(&sdev->xs_holder_comp);
	return sdev;
}

static void xrt_subdev_free(struct xrt_subdev *sdev)
{
	vfree(sdev);
}

int xrt_subdev_root_ioctl(struct platform_device *self, u32 cmd, void *arg)
{
	struct device *dev = DEV(self);
	struct xrt_subdev_platdata *pdata = DEV_PDATA(self);

	return (*pdata->xsp_root_cb)(dev->parent, pdata->xsp_root_cb_arg,
				     cmd, arg);
}

/*
 * Subdev common sysfs nodes.
 */
static ssize_t holders_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	ssize_t len;
	struct platform_device *pdev = to_platform_device(dev);
	struct xrt_root_ioctl_get_holders holders = { pdev, buf, 1024 };

	len = xrt_subdev_root_ioctl(pdev, XRT_ROOT_GET_LEAF_HOLDERS, &holders);
	if (len >= holders.xpigh_holder_buf_len)
		return len;
	buf[len] = '\n';
	return len + 1;
}
static DEVICE_ATTR_RO(holders);

static struct attribute *xrt_subdev_attrs[] = {
	&dev_attr_holders.attr,
	NULL,
};

static ssize_t metadata_output(struct file *filp, struct kobject *kobj,
			       struct bin_attribute *attr, char *buf,
			       loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct platform_device *pdev = to_platform_device(dev);
	struct xrt_subdev_platdata *pdata = DEV_PDATA(pdev);
	unsigned char *blob;
	long  size;
	ssize_t ret = 0;

	blob = pdata->xsp_dtb;
	size = xrt_md_size(dev, blob);
	if (size <= 0) {
		ret = -EINVAL;
		goto failed;
	}

	if (off >= size)
		goto failed;

	if (off + count > size)
		count = size - off;
	memcpy(buf, blob + off, count);

	ret = count;
failed:
	return ret;
}

static struct bin_attribute meta_data_attr = {
	.attr = {
		.name = "metadata",
		.mode = 0400
	},
	.read = metadata_output,
	.size = 0
};

static struct bin_attribute  *xrt_subdev_bin_attrs[] = {
	&meta_data_attr,
	NULL,
};

static const struct attribute_group xrt_subdev_attrgroup = {
	.attrs = xrt_subdev_attrs,
	.bin_attrs = xrt_subdev_bin_attrs,
};

static int
xrt_subdev_getres(struct device *parent, enum xrt_subdev_id id,
		  char *dtb, struct resource **res, int *res_num)
{
	struct xrt_subdev_platdata *pdata;
	struct resource *pci_res = NULL;
	const u64 *bar_range;
	const u32 *bar_idx;
	char *ep_name = NULL, *regmap = NULL;
	uint bar;
	int count1 = 0, count2 = 0, ret;

	if (!dtb)
		return -EINVAL;

	pdata = DEV_PDATA(to_platform_device(parent));

	for (xrt_md_get_next_endpoint(parent, dtb, NULL, NULL,
				      &ep_name, &regmap);
	     ep_name;
	     xrt_md_get_next_endpoint(parent, dtb, ep_name, regmap,
				      &ep_name, &regmap)) {
		ret = xrt_md_get_prop(parent, dtb, ep_name, regmap,
				      PROP_IO_OFFSET, (const void **)&bar_range, NULL);
		if (!ret)
			count1++;
	}
	if (!count1)
		return 0;

	*res = vzalloc(sizeof(*res) * count1);

	for (xrt_md_get_next_endpoint(parent, dtb, NULL, NULL, &ep_name, &regmap);
	     ep_name;
	     xrt_md_get_next_endpoint(parent, dtb, ep_name, regmap, &ep_name, &regmap)) {
		ret = xrt_md_get_prop(parent, dtb, ep_name, regmap,
				      PROP_IO_OFFSET, (const void **)&bar_range, NULL);
		if (ret)
			continue;
		xrt_md_get_prop(parent, dtb, ep_name, regmap,
				PROP_BAR_IDX, (const void **)&bar_idx, NULL);
		bar = bar_idx ? be32_to_cpu(*bar_idx) : 0;
		xleaf_get_barres(to_platform_device(parent), &pci_res, bar);
		(*res)[count2].start = pci_res->start +
			be64_to_cpu(bar_range[0]);
		(*res)[count2].end = pci_res->start +
			be64_to_cpu(bar_range[0]) +
			be64_to_cpu(bar_range[1]) - 1;
		(*res)[count2].flags = IORESOURCE_MEM;
		/* check if there is conflicted resource */
		ret = request_resource(pci_res, *res + count2);
		if (ret) {
			dev_err(parent, "Conflict resource %pR\n",
				*res + count2);
			vfree(*res);
			*res_num = 0;
			*res = NULL;
			return ret;
		}
		release_resource(*res + count2);

		(*res)[count2].parent = pci_res;

		xrt_md_get_epname_pointer(parent, pdata->xsp_dtb, ep_name,
					  regmap, &(*res)[count2].name);

		count2++;
	}

	WARN_ON(count1 != count2);
	*res_num = count2;

	return 0;
}

static inline enum xrt_subdev_file_mode
xleaf_devnode_mode(struct xrt_subdev_drvdata *drvdata)
{
	return drvdata->xsd_file_ops.xsf_mode;
}

static bool xrt_subdev_cdev_auto_creation(struct platform_device *pdev)
{
	struct xrt_subdev_drvdata *drvdata = DEV_DRVDATA(pdev);

	if (!drvdata)
		return false;

	return xleaf_devnode_enabled(drvdata) &&
		(xleaf_devnode_mode(drvdata) == XRT_SUBDEV_FILE_DEFAULT ||
		(xleaf_devnode_mode(drvdata) == XRT_SUBDEV_FILE_MULTI_INST));
}

static struct xrt_subdev *
xrt_subdev_create(struct device *parent, enum xrt_subdev_id id,
		  xrt_subdev_root_cb_t pcb, void *pcb_arg, char *dtb)
{
	struct xrt_subdev *sdev = NULL;
	struct platform_device *pdev = NULL;
	struct xrt_subdev_platdata *pdata = NULL;
	long dtb_len = 0;
	size_t pdata_sz;
	int inst = PLATFORM_DEVID_NONE;
	struct resource *res = NULL;
	int res_num = 0;

	sdev = xrt_subdev_alloc();
	if (!sdev) {
		dev_err(parent, "failed to alloc subdev for ID %d", id);
		goto fail;
	}
	sdev->xs_id = id;

	if (dtb) {
		xrt_md_pack(parent, dtb);
		dtb_len = xrt_md_size(parent, dtb);
		if (dtb_len <= 0) {
			dev_err(parent, "invalid metadata len %ld", dtb_len);
			goto fail;
		}
	}
	pdata_sz = sizeof(struct xrt_subdev_platdata) + dtb_len - 1;

	/* Prepare platform data passed to subdev. */
	pdata = vzalloc(pdata_sz);
	if (!pdata)
		goto fail;

	pdata->xsp_root_cb = pcb;
	pdata->xsp_root_cb_arg = pcb_arg;
	(void)memcpy(pdata->xsp_dtb, dtb, dtb_len);
	if (id == XRT_SUBDEV_GRP) {
		/* Group can only be created by root driver. */
		pdata->xsp_root_name = dev_name(parent);
	} else {
		struct platform_device *grp = to_platform_device(parent);
		/* Leaf can only be created by group driver. */
		WARN_ON(strcmp(xrt_drv_name(XRT_SUBDEV_GRP),
			       platform_get_device_id(grp)->name));
		pdata->xsp_root_name = DEV_PDATA(grp)->xsp_root_name;
	}

	/* Obtain dev instance number. */
	inst = xrt_drv_get_instance(id);
	if (inst < 0) {
		dev_err(parent, "failed to obtain instance: %d", inst);
		goto fail;
	}

	/* Create subdev. */
	if (id == XRT_SUBDEV_GRP) {
		pdev = platform_device_register_data(parent, xrt_drv_name(XRT_SUBDEV_GRP),
						     inst, pdata, pdata_sz);
	} else {
		int rc = xrt_subdev_getres(parent, id, dtb, &res, &res_num);

		if (rc) {
			dev_err(parent, "failed to get resource for %s.%d: %d",
				xrt_drv_name(id), inst, rc);
			goto fail;
		}
		pdev = platform_device_register_resndata(parent, xrt_drv_name(id),
							 inst, res, res_num, pdata, pdata_sz);
		vfree(res);
	}
	if (IS_ERR(pdev)) {
		dev_err(parent, "failed to create subdev for %s inst %d: %ld",
			xrt_drv_name(id), inst, PTR_ERR(pdev));
		goto fail;
	}
	sdev->xs_pdev = pdev;

	if (device_attach(DEV(pdev)) != 1) {
		xrt_err(pdev, "failed to attach");
		goto fail;
	}

	if (sysfs_create_group(&DEV(pdev)->kobj, &xrt_subdev_attrgroup))
		xrt_err(pdev, "failed to create sysfs group");

	/*
	 * Create sysfs sym link under root for leaves
	 * under random groups for easy access to them.
	 */
	if (id != XRT_SUBDEV_GRP) {
		if (sysfs_create_link(&find_root(pdev)->kobj,
				      &DEV(pdev)->kobj, dev_name(DEV(pdev)))) {
			xrt_err(pdev, "failed to create sysfs link");
		}
	}

	/* All done, ready to handle req thru cdev. */
	if (xrt_subdev_cdev_auto_creation(pdev)) {
		(void)xleaf_devnode_create(pdev,
			DEV_DRVDATA(pdev)->xsd_file_ops.xsf_dev_name, NULL);
	}

	vfree(pdata);
	return sdev;

fail:
	vfree(pdata);
	if (sdev && !IS_ERR_OR_NULL(sdev->xs_pdev))
		platform_device_unregister(sdev->xs_pdev);
	if (inst >= 0)
		xrt_drv_put_instance(id, inst);
	xrt_subdev_free(sdev);
	return NULL;
}

static void xrt_subdev_destroy(struct xrt_subdev *sdev)
{
	struct platform_device *pdev = sdev->xs_pdev;
	int inst = pdev->id;
	struct device *dev = DEV(pdev);

	/* Take down the device node */
	if (xrt_subdev_cdev_auto_creation(pdev))
		(void)xleaf_devnode_destroy(pdev);
	if (sdev->xs_id != XRT_SUBDEV_GRP)
		(void)sysfs_remove_link(&find_root(pdev)->kobj, dev_name(dev));
	(void)sysfs_remove_group(&dev->kobj, &xrt_subdev_attrgroup);
	platform_device_unregister(pdev);
	xrt_drv_put_instance(sdev->xs_id, inst);
	xrt_subdev_free(sdev);
}

struct platform_device *
xleaf_get_leaf(struct platform_device *pdev,
	       xrt_subdev_match_t match_cb, void *match_arg)
{
	int rc;
	struct xrt_root_ioctl_get_leaf get_leaf = {
		pdev, match_cb, match_arg, };

	rc = xrt_subdev_root_ioctl(pdev, XRT_ROOT_GET_LEAF, &get_leaf);
	if (rc)
		return NULL;
	return get_leaf.xpigl_leaf;
}
EXPORT_SYMBOL_GPL(xleaf_get_leaf);

bool xleaf_has_epname(struct platform_device *pdev, const char *ep_name)
{
	struct resource	*res;
	int		i;

	for (i = 0, res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	    res;
	    res = platform_get_resource(pdev, IORESOURCE_MEM, ++i)) {
		if (!strncmp(res->name, ep_name, strlen(res->name) + 1))
			return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(xleaf_has_epname);

int xleaf_put_leaf(struct platform_device *pdev,
		   struct platform_device *leaf)
{
	struct xrt_root_ioctl_put_leaf put_leaf = { pdev, leaf };

	return xrt_subdev_root_ioctl(pdev, XRT_ROOT_PUT_LEAF, &put_leaf);
}
EXPORT_SYMBOL_GPL(xleaf_put_leaf);

int xleaf_create_group(struct platform_device *pdev, char *dtb)
{
	return xrt_subdev_root_ioctl(pdev, XRT_ROOT_CREATE_GROUP, dtb);
}
EXPORT_SYMBOL_GPL(xleaf_create_group);

int xleaf_destroy_group(struct platform_device *pdev, int instance)
{
	return xrt_subdev_root_ioctl(pdev,
		XRT_ROOT_REMOVE_GROUP, (void *)(uintptr_t)instance);
}
EXPORT_SYMBOL_GPL(xleaf_destroy_group);

int xleaf_wait_for_group_bringup(struct platform_device *pdev)
{
	return xrt_subdev_root_ioctl(pdev, XRT_ROOT_WAIT_GROUP_BRINGUP, NULL);
}
EXPORT_SYMBOL_GPL(xleaf_wait_for_group_bringup);

static ssize_t
xrt_subdev_get_holders(struct xrt_subdev *sdev, char *buf, size_t len)
{
	const struct list_head *ptr;
	struct xrt_subdev_holder *h;
	ssize_t n = 0;

	list_for_each(ptr, &sdev->xs_holder_list) {
		h = list_entry(ptr, struct xrt_subdev_holder, xsh_holder_list);
		n += snprintf(buf + n, len - n, "%s:%d ",
			dev_name(h->xsh_holder), h->xsh_count);
		if (n >= len)
			break;
	}
	return n;
}

void xrt_subdev_pool_init(struct device *dev, struct xrt_subdev_pool *spool)
{
	INIT_LIST_HEAD(&spool->xpool_dev_list);
	spool->xpool_owner = dev;
	mutex_init(&spool->xpool_lock);
	spool->xpool_closing = false;
}

static void xrt_subdev_pool_wait_for_holders(struct xrt_subdev_pool *spool,
					     struct xrt_subdev *sdev)
{
	const struct list_head *ptr, *next;
	char holders[128];
	struct xrt_subdev_holder *holder;
	struct mutex *lk = &spool->xpool_lock;

	WARN_ON(!mutex_is_locked(lk));

	while (!list_empty(&sdev->xs_holder_list)) {
		int rc;

		/* It's most likely a bug if we ever enters this loop. */
		(void)xrt_subdev_get_holders(sdev, holders, sizeof(holders));
		xrt_err(sdev->xs_pdev, "awaits holders: %s", holders);
		mutex_unlock(lk);
		rc = wait_for_completion_killable(&sdev->xs_holder_comp);
		mutex_lock(lk);
		if (rc == -ERESTARTSYS) {
			xrt_err(sdev->xs_pdev,
				"give up on waiting for holders, clean up now");
			list_for_each_safe(ptr, next, &sdev->xs_holder_list) {
				holder = list_entry(ptr, struct xrt_subdev_holder,
						    xsh_holder_list);
				list_del(&holder->xsh_holder_list);
				vfree(holder);
			}
		}
	}
}

int xrt_subdev_pool_fini(struct xrt_subdev_pool *spool)
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
		struct xrt_subdev *sdev = list_first_entry(dl,
			struct xrt_subdev, xs_dev_list);
		xrt_subdev_pool_wait_for_holders(spool, sdev);
		list_del(&sdev->xs_dev_list);
		mutex_unlock(lk);
		xrt_subdev_destroy(sdev);
		mutex_lock(lk);
	}

	mutex_unlock(lk);

	return ret;
}

static int xrt_subdev_hold(struct xrt_subdev *sdev, struct device *holder_dev)
{
	const struct list_head *ptr;
	struct list_head *hl = &sdev->xs_holder_list;
	struct xrt_subdev_holder *holder;
	bool found = false;

	list_for_each(ptr, hl) {
		holder = list_entry(ptr, struct xrt_subdev_holder,
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
xrt_subdev_release(struct xrt_subdev *sdev, struct device *holder_dev)
{
	const struct list_head *ptr, *next;
	struct list_head *hl = &sdev->xs_holder_list;
	struct xrt_subdev_holder *holder;
	int count;
	bool found = false;

	list_for_each_safe(ptr, next, hl) {
		holder = list_entry(ptr, struct xrt_subdev_holder,
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

int xrt_subdev_pool_add(struct xrt_subdev_pool *spool, enum xrt_subdev_id id,
			xrt_subdev_root_cb_t pcb, void *pcb_arg, char *dtb)
{
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xrt_subdev *sdev;
	int ret = 0;

	sdev = xrt_subdev_create(spool->xpool_owner, id, pcb, pcb_arg, dtb);
	if (sdev) {
		mutex_lock(lk);
		if (spool->xpool_closing) {
			/* No new subdev when pool is going away. */
			xrt_err(sdev->xs_pdev, "pool is closing");
			ret = -ENODEV;
		} else {
			list_add(&sdev->xs_dev_list, dl);
		}
		mutex_unlock(lk);
		if (ret)
			xrt_subdev_destroy(sdev);
	} else {
		ret = -EINVAL;
	}

	return ret ? ret : sdev->xs_pdev->id;
}

int xrt_subdev_pool_del(struct xrt_subdev_pool *spool, enum xrt_subdev_id id,
			int instance)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xrt_subdev *sdev;
	int ret = -ENOENT;

	mutex_lock(lk);
	list_for_each(ptr, dl) {
		sdev = list_entry(ptr, struct xrt_subdev, xs_dev_list);
		if (sdev->xs_id != id || sdev->xs_pdev->id != instance)
			continue;
		xrt_subdev_pool_wait_for_holders(spool, sdev);
		list_del(&sdev->xs_dev_list);
		ret = 0;
		break;
	}
	mutex_unlock(lk);
	if (ret)
		return ret;

	xrt_subdev_destroy(sdev);
	return 0;
}

static int xrt_subdev_pool_get_impl(struct xrt_subdev_pool *spool,
				    xrt_subdev_match_t match, void *arg,
				    struct device *holder_dev,
				    struct xrt_subdev **sdevp)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xrt_subdev *sdev = NULL;
	int ret = -ENOENT;

	mutex_lock(lk);

	if (match == XRT_SUBDEV_MATCH_PREV) {
		struct platform_device *pdev = (struct platform_device *)arg;
		struct xrt_subdev *d = NULL;

		if (!pdev) {
			sdev = list_empty(dl) ? NULL :
				list_last_entry(dl, struct xrt_subdev, xs_dev_list);
		} else {
			list_for_each(ptr, dl) {
				d = list_entry(ptr, struct xrt_subdev, xs_dev_list);
				if (d->xs_pdev != pdev)
					continue;
				if (!list_is_first(ptr, dl))
					sdev = list_prev_entry(d, xs_dev_list);
				break;
			}
		}
	} else if (match == XRT_SUBDEV_MATCH_NEXT) {
		struct platform_device *pdev = (struct platform_device *)arg;
		struct xrt_subdev *d = NULL;

		if (!pdev) {
			sdev = list_first_entry_or_null(dl, struct xrt_subdev, xs_dev_list);
		} else {
			list_for_each(ptr, dl) {
				d = list_entry(ptr, struct xrt_subdev, xs_dev_list);
				if (d->xs_pdev != pdev)
					continue;
				if (!list_is_last(ptr, dl))
					sdev = list_next_entry(d, xs_dev_list);
				break;
			}
		}
	} else {
		list_for_each(ptr, dl) {
			struct xrt_subdev *d = NULL;

			d = list_entry(ptr, struct xrt_subdev, xs_dev_list);
			if (d && !match(d->xs_id, d->xs_pdev, arg))
				continue;
			sdev = d;
			break;
		}
	}

	if (sdev)
		ret = xrt_subdev_hold(sdev, holder_dev);

	mutex_unlock(lk);

	if (ret >= 0)
		*sdevp = sdev;
	return ret;
}

int xrt_subdev_pool_get(struct xrt_subdev_pool *spool,
			xrt_subdev_match_t match, void *arg,
			struct device *holder_dev,
			struct platform_device **pdevp)
{
	int rc;
	struct xrt_subdev *sdev;

	rc = xrt_subdev_pool_get_impl(spool, match, arg, holder_dev, &sdev);
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
		xrt_dbg(to_platform_device(holder_dev), "%s <<==== %s",
			dev_name(holder_dev), dev_name(DEV(sdev->xs_pdev)));
	}

	*pdevp = sdev->xs_pdev;
	return 0;
}

static int xrt_subdev_pool_put_impl(struct xrt_subdev_pool *spool,
				    struct platform_device *pdev,
				    struct device *holder_dev)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xrt_subdev *sdev;
	int ret = -ENOENT;

	mutex_lock(lk);
	list_for_each(ptr, dl) {
		sdev = list_entry(ptr, struct xrt_subdev, xs_dev_list);
		if (sdev->xs_pdev != pdev)
			continue;
		ret = xrt_subdev_release(sdev, holder_dev);
		break;
	}
	mutex_unlock(lk);

	if (ret < 0 && ret != -ENOENT)
		dev_err(holder_dev, "failed to release device: %d", ret);
	return ret;
}

int xrt_subdev_pool_put(struct xrt_subdev_pool *spool,
			struct platform_device *pdev,
			struct device *holder_dev)
{
	int ret = xrt_subdev_pool_put_impl(spool, pdev, holder_dev);

	if (ret < 0)
		return ret;

	if (DEV_IS_PCI(holder_dev)) {
#ifdef	SUBDEV_DEBUG
		dev_info(holder_dev, "%s: %s <<==X== %s, ref=%d", __func__,
			 dev_name(holder_dev), dev_name(DEV(spdev)), ret);
#endif
	} else {
		struct platform_device *d = to_platform_device(holder_dev);

		xrt_dbg(d, "%s <<==X== %s",
			dev_name(holder_dev), dev_name(DEV(pdev)));
	}
	return 0;
}

void xrt_subdev_pool_trigger_event(struct xrt_subdev_pool *spool,
				   enum xrt_events e)
{
	struct platform_device *tgt = NULL;
	struct xrt_subdev *sdev = NULL;
	struct xrt_event evt;

	while (xrt_subdev_pool_get_impl(spool, XRT_SUBDEV_MATCH_NEXT,
					tgt, spool->xpool_owner, &sdev) != -ENOENT) {
		tgt = sdev->xs_pdev;
		evt.xe_evt = e;
		evt.xe_subdev.xevt_subdev_id = sdev->xs_id;
		evt.xe_subdev.xevt_subdev_instance = tgt->id;
		(void)xrt_subdev_root_ioctl(tgt, XRT_ROOT_EVENT, &evt);
		(void)xrt_subdev_pool_put_impl(spool, tgt, spool->xpool_owner);
	}
}

void xrt_subdev_pool_handle_event(struct xrt_subdev_pool *spool,
				  struct xrt_event *evt)
{
	struct platform_device *tgt = NULL;
	struct xrt_subdev *sdev = NULL;

	while (xrt_subdev_pool_get_impl(spool, XRT_SUBDEV_MATCH_NEXT,
					tgt, spool->xpool_owner, &sdev) != -ENOENT) {
		tgt = sdev->xs_pdev;
		(void)xleaf_ioctl(tgt, XRT_XLEAF_EVENT, evt);
		(void)xrt_subdev_pool_put_impl(spool, tgt, spool->xpool_owner);
	}
}

ssize_t xrt_subdev_pool_get_holders(struct xrt_subdev_pool *spool,
				    struct platform_device *pdev,
				    char *buf, size_t len)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xpool_lock;
	struct list_head *dl = &spool->xpool_dev_list;
	struct xrt_subdev *sdev;
	ssize_t ret = 0;

	mutex_lock(lk);
	list_for_each(ptr, dl) {
		sdev = list_entry(ptr, struct xrt_subdev, xs_dev_list);
		if (sdev->xs_pdev != pdev)
			continue;
		ret = xrt_subdev_get_holders(sdev, buf, len);
		break;
	}
	mutex_unlock(lk);

	return ret;
}
EXPORT_SYMBOL_GPL(xrt_subdev_pool_get_holders);

int xleaf_broadcast_event(struct platform_device *pdev,
			  enum xrt_events evt, bool async)
{
	struct xrt_event e = { evt, };
	u32 cmd = async ? XRT_ROOT_EVENT_ASYNC : XRT_ROOT_EVENT;

	WARN_ON(evt == XRT_EVENT_POST_CREATION || evt == XRT_EVENT_PRE_REMOVAL);
	return xrt_subdev_root_ioctl(pdev, cmd, &e);
}
EXPORT_SYMBOL_GPL(xleaf_broadcast_event);

void xleaf_hot_reset(struct platform_device *pdev)
{
	(void)xrt_subdev_root_ioctl(pdev, XRT_ROOT_HOT_RESET, NULL);
}
EXPORT_SYMBOL_GPL(xleaf_hot_reset);

void xleaf_get_barres(struct platform_device *pdev,
		      struct resource **res, uint bar_idx)
{
	struct xrt_root_ioctl_get_res arg = { 0 };

	if (bar_idx > PCI_STD_RESOURCE_END) {
		xrt_err(pdev, "Invalid bar idx %d", bar_idx);
		*res = NULL;
		return;
	}

	(void)xrt_subdev_root_ioctl(pdev, XRT_ROOT_GET_RESOURCE, &arg);

	*res = &arg.xpigr_res[bar_idx];
}

void xleaf_get_root_id(struct platform_device *pdev,
		       unsigned short *vendor, unsigned short *device,
		       unsigned short *subvendor, unsigned short *subdevice)
{
	struct xrt_root_ioctl_get_id id = { 0 };

	(void)xrt_subdev_root_ioctl(pdev, XRT_ROOT_GET_ID, (void *)&id);
	if (vendor)
		*vendor = id.xpigi_vendor_id;
	if (device)
		*device = id.xpigi_device_id;
	if (subvendor)
		*subvendor = id.xpigi_sub_vendor_id;
	if (subdevice)
		*subdevice = id.xpigi_sub_device_id;
}

struct device *xleaf_register_hwmon(struct platform_device *pdev,
				    const char *name, void *drvdata,
				    const struct attribute_group **grps)
{
	struct xrt_root_ioctl_hwmon hm = { true, name, drvdata, grps, };

	(void)xrt_subdev_root_ioctl(pdev, XRT_ROOT_HWMON, (void *)&hm);
	return hm.xpih_hwmon_dev;
}

void xleaf_unregister_hwmon(struct platform_device *pdev,
			    struct device *hwmon)
{
	struct xrt_root_ioctl_hwmon hm = { false, };

	hm.xpih_hwmon_dev = hwmon;
	(void)xrt_subdev_root_ioctl(pdev, XRT_ROOT_HWMON, (void *)&hm);
}
