// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/platform_device.h>
#include <linux/pci.h>
#include "xocl-subdev.h"
#include "xocl-parent.h"

extern const char *xocl_drv_name(enum xocl_subdev_id id);
extern int xocl_drv_get_instance_fixed(enum xocl_subdev_id id, int instance);
extern int xocl_drv_get_instance(enum xocl_subdev_id id);
extern void xocl_drv_put_instance(enum xocl_subdev_id id, int instance);

static struct xocl_subdev *xocl_subdev_alloc(void)
{
	struct xocl_subdev *sdev = vzalloc(sizeof(struct xocl_subdev));

	if (!sdev)
		return NULL;

	INIT_LIST_HEAD(&sdev->xs_dev_list);
	return sdev;
}

static void xocl_subdev_free(struct xocl_subdev *sdev)
{
	vfree(sdev);
}

struct xocl_subdev *
xocl_subdev_create(struct device *parent, enum xocl_subdev_id id,
	int instance, xocl_subdev_parent_cb_t pcb, void *dtb)
{
	struct xocl_subdev *sdev = NULL;
	struct platform_device *pdev = NULL;
	struct xocl_subdev_platdata *pdata = NULL;
	size_t dtb_len = 0; /* TODO: comes from dtb. */
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
	if (!pdata) {
		dev_err(parent, "failed to alloc platform data");
		goto fail;
	}
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
	if (instance == PLATFORM_DEVID_AUTO)
		inst = xocl_drv_get_instance(id);
	else
		inst = instance;
	if (inst < 0) {
		dev_err(parent, "failed to obtain instance %d: %d",
			instance, inst);
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

	vfree(pdata);
	return sdev;

fail:
	vfree(pdata);
	if (inst != PLATFORM_DEVID_NONE)
		xocl_drv_put_instance(id, inst);
	if (sdev && !IS_ERR_OR_NULL(sdev->xs_pdev))
		platform_device_unregister(sdev->xs_pdev);
	xocl_subdev_free(sdev);
	return NULL;
}

void xocl_subdev_destroy(struct xocl_subdev *sdev)
{
	int inst = sdev->xs_pdev->id;

	platform_device_unregister(sdev->xs_pdev);
	if (sdev->xs_id != XOCL_SUBDEV_PART)
		xocl_drv_put_instance(sdev->xs_id, inst);
	xocl_subdev_free(sdev);
}

long xocl_subdev_parent_ioctl(struct platform_device *self, u32 cmd, u64 arg)
{
	struct device *dev = DEV(self);
	struct xocl_subdev_platdata *pdata = DEV_PDATA(self);

	return (*pdata->xsp_parent_cb)(dev->parent, cmd, arg);
}

long xocl_subdev_ioctl(struct platform_device *tgt, u32 cmd, u64 arg)
{
	struct xocl_subdev_drvdata *drvdata = DEV_DRVDATA(tgt);

	return (*drvdata->xsd_dev_ops.xsd_ioctl)(tgt, cmd, arg);
}

int xocl_subdev_online(struct platform_device *pdev)
{
	struct xocl_subdev_drvdata *drvdata = DEV_DRVDATA(pdev);

	return (*drvdata->xsd_dev_ops.xsd_online)(pdev);
}

int xocl_subdev_offline(struct platform_device *pdev)
{
	struct xocl_subdev_drvdata *drvdata = DEV_DRVDATA(pdev);

	return (*drvdata->xsd_dev_ops.xsd_offline)(pdev);
}

struct platform_device *
xocl_subdev_get_leaf(struct platform_device *pdev, enum xocl_subdev_id id,
	xocl_leaf_match_t match_cb, u64 match_arg)
{
	long rc;
	struct xocl_parent_ioctl_get_leaf get_leaf =
		{ pdev, id, match_cb, match_arg, };

	rc = xocl_subdev_parent_ioctl(
		pdev, XOCL_PARENT_GET_LEAF, (u64)&get_leaf);
	if (rc) {
		xocl_err(pdev, "failed to find leaf subdev id %d: %ld", id, rc);
		return NULL;
	}
	return get_leaf.xpigl_leaf;
}

EXPORT_SYMBOL_GPL(xocl_subdev_create);
EXPORT_SYMBOL_GPL(xocl_subdev_destroy);
EXPORT_SYMBOL_GPL(xocl_subdev_ioctl);
EXPORT_SYMBOL_GPL(xocl_subdev_online);
EXPORT_SYMBOL_GPL(xocl_subdev_offline);
