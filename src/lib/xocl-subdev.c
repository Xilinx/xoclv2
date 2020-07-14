// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include "xocl-subdev.h"

#define	XOCL_IPLIB_MODULE_NAME		"xocl-lib"
#define	XOCL_IPLIB_MODULE_VERSION	"4.0.0"

#define	XOCL_CDEV_DIR			"xfpga"
#define	XOCL_DRVNAME(drv)		((drv)->driver.name)

extern struct platform_driver xocl_partition_driver;
extern struct platform_driver xocl_test_driver;

static struct class *xocl_class;

/*
 * Subdev driver is known by ID to others. We map the ID to it's
 * struct platform_driver, which contains it's binding name and driver/file ops.
 * We also map it to the endpoint name in DTB as well, if it's different
 * than the driver's binding name.
 */
static struct xocl_subdev_map {
	enum xocl_subdev_id id;
	struct platform_driver *drv;
	char *dtb_name;
	struct ida ida; /* manage driver instance and char dev minor */
} xocl_subdev_maps[] = {
	{ XOCL_SUBDEV_PART, &xocl_partition_driver, },
	{ XOCL_SUBDEV_TEST, &xocl_test_driver, },
};

static inline struct xocl_subdev_data *
xocl_subdev_map2drvdata(struct xocl_subdev_map *map)
{
	return (struct xocl_subdev_data *)map->drv->id_table[0].driver_data;
}

static inline struct xocl_subdev_data *
xocl_subdev_drvdata(struct platform_device *pdev)
{
	return (struct xocl_subdev_data *)
		platform_get_device_id(pdev)->driver_data;
}

static struct xocl_subdev_map *
xocl_subdev_find_map_by_id(enum xocl_subdev_id id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(xocl_subdev_maps); i++) {
		struct xocl_subdev_map *map = &xocl_subdev_maps[i];
		if (map->id == id)
			return map;
	}
	return NULL;
}

static int xocl_subdev_register_driver(enum xocl_subdev_id id)
{
	struct xocl_subdev_map *map = xocl_subdev_find_map_by_id(id);
	struct xocl_subdev_data *data;
	int rc;
	const char *drvname;

	BUG_ON(!map);
	drvname = XOCL_DRVNAME(map->drv);

	rc = platform_driver_register(map->drv);
	if (rc) {
		pr_err("register %s subdev driver failed\n", drvname);
		return rc;
	}

	data = xocl_subdev_map2drvdata(map);
	if (data && data->xsd_dev_ops.xsd_post_init) {
		rc = data->xsd_dev_ops.xsd_post_init();
		if (rc) {
			platform_driver_unregister(map->drv);
			pr_err("%s's post-init, ret %d\n", drvname, rc);
			return rc;
		}
	}

	if (data && data->xsd_file_ops.xsf_ops.owner) {
		rc = alloc_chrdev_region(&data->xsd_file_ops.xsf_dev_t, 0,
			XOCL_MAX_DEVICE_NODES, drvname);
		if (rc) {
			if (data->xsd_dev_ops.xsd_pre_exit)
				data->xsd_dev_ops.xsd_pre_exit();
			platform_driver_unregister(map->drv);
			pr_err("failed to alloc dev minors for %s, ret %d\n",
				drvname, rc);
			return rc;
		}
	} else {
		data->xsd_file_ops.xsf_dev_t = (dev_t)-1;
	}

	ida_init(&map->ida);

	pr_info("registered %s subdev driver\n", drvname);
	return 0;
}

static void xocl_subdev_unregister_driver(enum xocl_subdev_id id)
{
	struct xocl_subdev_map *map = xocl_subdev_find_map_by_id(id);
	struct xocl_subdev_data *data;
	const char *drvname;

	BUG_ON(!map);
	drvname = XOCL_DRVNAME(map->drv);

	ida_destroy(&map->ida);

	data = xocl_subdev_map2drvdata(map);
	if (data && data->xsd_file_ops.xsf_dev_t != (dev_t)-1) {
		unregister_chrdev_region(data->xsd_file_ops.xsf_dev_t,
			XOCL_MAX_DEVICE_NODES);
	}

	if (data && data->xsd_dev_ops.xsd_pre_exit)
		data->xsd_dev_ops.xsd_pre_exit();

	platform_driver_unregister(map->drv);

	pr_info("unregistered %s subdev driver\n", drvname);
}

static __init int xocl_subdev_register_drivers(void)
{
	int i;
	int rc;

	xocl_class = class_create(THIS_MODULE, XOCL_IPLIB_MODULE_NAME);
	if (IS_ERR(xocl_class))
		return PTR_ERR(xocl_class);

	for (i = 0; i < ARRAY_SIZE(xocl_subdev_maps); i++) {
		rc = xocl_subdev_register_driver(xocl_subdev_maps[i].id);
		if (rc)
			break;
	}
	if (!rc)
		return 0;

	while (i-- > 0)
		xocl_subdev_unregister_driver(xocl_subdev_maps[i].id);
	class_destroy(xocl_class);
	return rc;
}

static __exit void xocl_subdev_unregister_drivers(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(xocl_subdev_maps); i++)
		xocl_subdev_unregister_driver(xocl_subdev_maps[i].id);
	class_destroy(xocl_class);
}

static struct xocl_subdev *
xocl_subdev_alloc(enum xocl_subdev_id devid, int instance)
{
	struct xocl_subdev_map *map = xocl_subdev_find_map_by_id(devid);
	struct xocl_subdev *sdev = vzalloc(sizeof(struct xocl_subdev));
	int inst;

	if (!sdev)
		return NULL;

	if (instance == PLATFORM_DEVID_AUTO) {
		inst = ida_alloc_range(&map->ida, 0, XOCL_MAX_DEVICE_NODES,
			GFP_KERNEL);
	} else {
		inst = ida_alloc_range(&map->ida, instance, instance,
			GFP_KERNEL);
		BUG_ON(inst == -ENOSPC);
	}
	if (inst < 0) {
		vfree(sdev);
		return NULL;
	}

	INIT_LIST_HEAD(&sdev->xs_dev_list);
	sdev->xs_id = devid;
	sdev->xs_instance = inst;
	sdev->xs_drv = map->drv;
	return sdev;
}

static void xocl_subdev_free(struct xocl_subdev *sdev)
{
	struct xocl_subdev_map *map = xocl_subdev_find_map_by_id(sdev->xs_id);

	ida_free(&map->ida, sdev->xs_instance);
	vfree(sdev);
}

struct xocl_subdev *
xocl_subdev_create_partition(struct pci_dev *root, enum xocl_partition_id id,
	xocl_subdev_parent_cb_t pcb, void *dtb, size_t dtb_len)
{
	struct xocl_subdev *sdev;
	struct device *parent = &root->dev;
	struct xocl_subdev_platdata *pdata = NULL;
	size_t pdata_sz = sizeof(struct xocl_subdev_platdata) + dtb_len - 1;

	sdev = xocl_subdev_alloc(XOCL_SUBDEV_PART, id);
	if (!sdev) {
		dev_err(parent, "failed to alloc subdev");
		return NULL;
	}

	/* Prepare platform data passed to subdev. */
	pdata = vzalloc(pdata_sz);
	if (!pdata) {
		dev_err(parent, "failed to alloc platform data");
		goto fail;
	}
	pdata->xsp_parent_cb = pcb;
	(void) memcpy(pdata->xsp_dtb, dtb, dtb_len);
	pdata->xsp_domain = pci_domain_nr(root->bus);
	pdata->xsp_bus = root->bus->number;
	pdata->xsp_dev = PCI_SLOT(root->devfn);
	pdata->xsp_func = PCI_FUNC(root->devfn);

	sdev->xs_pdev = platform_device_register_data(parent,
		sdev->xs_drv->driver.name, sdev->xs_instance, pdata, pdata_sz);
	if (IS_ERR(sdev->xs_pdev)) {
		dev_err(parent, "failed to create subdev: %ld",
			PTR_ERR(sdev->xs_pdev));
		goto fail;
	}

	if (device_attach(&sdev->xs_pdev->dev) != 1) {
		xocl_err(sdev->xs_pdev, "failed to attach");
		goto fail;
	}

	vfree(pdata);
	return sdev;

fail:
	vfree(pdata);
	if (!IS_ERR_OR_NULL(sdev->xs_pdev))
		platform_device_unregister(sdev->xs_pdev);
	xocl_subdev_free(sdev);
	return NULL;
}

static int xocl_subdev_create_cdev(struct xocl_subdev *sdev)
{
	struct platform_device *pdev = sdev->xs_pdev;
	struct xocl_subdev_data *drvdata = xocl_subdev_drvdata(pdev);
	struct xocl_subdev_file_ops *fops = &drvdata->xsd_file_ops;
	struct cdev *cdevp;
	struct device *sysdev;
	int ret = 0;
	const char *cdevname;
	char filename[256];

	if (fops->xsf_dev_t == (dev_t)-1)
		return 0; /* subdev does not support char dev */

	cdevp = &DEV_PDATA(pdev)->xsp_cdev;
	cdev_init(cdevp, &fops->xsf_ops);
	cdevp->owner = fops->xsf_ops.owner;
	cdevp->dev = MKDEV(MAJOR(fops->xsf_dev_t),
		(sdev->xs_instance & MINORMASK));
	/*
	 * Set pdev as parent of cdev so that when pdev (and its platform
	 * data) will not be freed when cdev is not freed.
	 */
	cdev_set_parent(cdevp, &pdev->dev.kobj);

	ret = cdev_add(cdevp, cdevp->dev, 1);
	if (ret) {
		xocl_err(pdev, "failed to add cdev: %d", ret);
		goto failed;
	}

	cdevname = fops->xsf_dev_name;
	if (!cdevname)
		cdevname = sdev->xs_drv->driver.name;
	snprintf(filename, sizeof(filename) - 1, "%s/%s.%x:%x:%x.%x-%u",
		XOCL_CDEV_DIR, cdevname, DEV_PDATA(pdev)->xsp_domain,
		DEV_PDATA(pdev)->xsp_bus, DEV_PDATA(pdev)->xsp_dev,
		DEV_PDATA(pdev)->xsp_func, sdev->xs_instance);
	sysdev = device_create(xocl_class, &pdev->dev, cdevp->dev,
		NULL, "%s", filename);
	if (IS_ERR(sysdev)) {
		ret = PTR_ERR(sysdev);
		xocl_err(pdev, "failed to create device node: %d", ret);
		goto failed;
	}

	xocl_info(pdev, "created device node: %s", filename);
	return 0;

failed:
	device_destroy(xocl_class, cdevp->dev);
	cdev_del(cdevp);
	cdevp->owner = NULL;
	return ret;
}

static void xocl_subdev_destroy_cdev(struct xocl_subdev *sdev)
{
	struct cdev *cdevp = &DEV_PDATA(sdev->xs_pdev)->xsp_cdev;

	if (!cdevp->owner)
		return;

	device_destroy(xocl_class, cdevp->dev);
	cdev_del(cdevp);
	cdevp->owner = NULL;
	xocl_info(sdev->xs_pdev, "removed device node");
}

struct xocl_subdev *
xocl_subdev_create_leaf(struct platform_device *part, enum xocl_subdev_id id,
	xocl_subdev_parent_cb_t pcb, void *dtb, size_t dtb_len)
{
	struct xocl_subdev *sdev;
	struct device *parent = &part->dev;
	struct xocl_subdev_platdata *pdata = NULL;
	size_t pdata_sz = sizeof(struct xocl_subdev_platdata) + dtb_len - 1;

	sdev = xocl_subdev_alloc(id, PLATFORM_DEVID_AUTO);
	if (!sdev) {
		dev_err(parent, "failed to alloc subdev for ID %d", id);
		return NULL;
	}

	/* Prepare platform data passed to subdev. */
	pdata = vzalloc(pdata_sz);
	if (!pdata) {
		dev_err(parent, "failed to alloc platform data");
		goto fail;
	}
	pdata->xsp_parent_cb = pcb;
	(void) memcpy(pdata->xsp_dtb, dtb, dtb_len);
	pdata->xsp_domain = DEV_PDATA(part)->xsp_domain;
	pdata->xsp_bus = DEV_PDATA(part)->xsp_bus;
	pdata->xsp_dev = DEV_PDATA(part)->xsp_dev;
	pdata->xsp_func = DEV_PDATA(part)->xsp_func;

	sdev->xs_pdev = platform_device_register_resndata(parent,
		sdev->xs_drv->driver.name, sdev->xs_instance,
		NULL, 0, /* TODO: find out IO and IRQ resources from dtb */
		pdata, pdata_sz);
	if (IS_ERR(sdev->xs_pdev)) {
		dev_err(parent, "failed to create subdev for %s inst %d: %ld",
			XOCL_DRVNAME(sdev->xs_drv), sdev->xs_instance,
			PTR_ERR(sdev->xs_pdev));
		goto fail;
	}

	if (device_attach(&sdev->xs_pdev->dev) != 1) {
		xocl_err(sdev->xs_pdev, "failed to attach");
		goto fail;
	}

	(void) xocl_subdev_create_cdev(sdev);
	vfree(pdata);
	return sdev;

fail:
	vfree(pdata);
	if (!IS_ERR_OR_NULL(sdev->xs_pdev))
		platform_device_unregister(sdev->xs_pdev);
	xocl_subdev_free(sdev);
	return NULL;

}

void xocl_subdev_destroy(struct xocl_subdev *sdev)
{
	xocl_subdev_destroy_cdev(sdev);
	platform_device_unregister(sdev->xs_pdev);
	xocl_subdev_free(sdev);
}

long xocl_subdev_parent_ioctl(struct platform_device *pdev, u32 cmd, u64 arg)
{
	struct device *dev = DEV(pdev);
	struct xocl_subdev_platdata *pdata = DEV_PDATA(pdev);

	return (*pdata->xsp_parent_cb)(dev->parent, cmd, arg);
}

long xocl_subdev_ioctl(xocl_subdev_leaf_handle_t handle, u32 cmd, u64 arg)
{
	struct platform_device *pdev = (struct platform_device *)handle;
	struct xocl_subdev_data *drvdata = xocl_subdev_drvdata(pdev);

	return (*drvdata->xsd_dev_ops.xsd_ioctl)(pdev, cmd, arg);
}

xocl_subdev_leaf_handle_t
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

module_init(xocl_subdev_register_drivers);
module_exit(xocl_subdev_unregister_drivers);

EXPORT_SYMBOL_GPL(xocl_subdev_create_partition);
EXPORT_SYMBOL_GPL(xocl_subdev_create_leaf);
EXPORT_SYMBOL_GPL(xocl_subdev_destroy);
EXPORT_SYMBOL_GPL(xocl_subdev_parent_ioctl);
EXPORT_SYMBOL_GPL(xocl_subdev_ioctl);
EXPORT_SYMBOL_GPL(xocl_subdev_get_leaf);

MODULE_VERSION(XOCL_IPLIB_MODULE_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
