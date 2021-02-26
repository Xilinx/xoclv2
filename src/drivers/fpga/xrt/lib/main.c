// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/module.h>
#include <linux/vmalloc.h>
#include "xleaf.h"
#include "xroot.h"
#include "main.h"

#define XRT_IPLIB_MODULE_NAME		"xrt-lib"
#define XRT_IPLIB_MODULE_VERSION	"4.0.0"
#define XRT_MAX_DEVICE_NODES		128
#define XRT_DRVNAME(drv)		((drv)->driver.name)

/*
 * Subdev driver is known by ID to others. We map the ID to it's
 * struct platform_driver, which contains it's binding name and driver/file ops.
 * We also map it to the endpoint name in DTB as well, if it's different
 * than the driver's binding name.
 */
struct xrt_drv_map {
	struct list_head list;
	enum xrt_subdev_id id;
	struct platform_driver *drv;
	struct xrt_subdev_endpoints *eps;
	struct ida ida; /* manage driver instance and char dev minor */
};

static DEFINE_MUTEX(xrt_lib_lock); /* global lock protecting xrt_drv_maps list */
static LIST_HEAD(xrt_drv_maps);
struct class *xrt_class;

static inline struct xrt_subdev_drvdata *
xrt_drv_map2drvdata(struct xrt_drv_map *map)
{
	return (struct xrt_subdev_drvdata *)map->drv->id_table[0].driver_data;
}

static struct xrt_drv_map *
xrt_drv_find_map_by_id_nolock(enum xrt_subdev_id id)
{
	const struct list_head *ptr;

	list_for_each(ptr, &xrt_drv_maps) {
		struct xrt_drv_map *tmap = list_entry(ptr, struct xrt_drv_map, list);

		if (tmap->id == id)
			return tmap;
	}
	return NULL;
}

static struct xrt_drv_map *
xrt_drv_find_map_by_id(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map;

	mutex_lock(&xrt_lib_lock);
	map = xrt_drv_find_map_by_id_nolock(id);
	mutex_unlock(&xrt_lib_lock);
	/*
	 * map should remain valid even after lock is dropped since a registered
	 * driver should only be unregistered when driver module is being unloaded,
	 * which means that the driver should not be used by then.
	 */
	return map;
}

static int xrt_drv_register_driver(struct xrt_drv_map *map)
{
	struct xrt_subdev_drvdata *drvdata;
	int rc = 0;
	const char *drvname = XRT_DRVNAME(map->drv);

	rc = platform_driver_register(map->drv);
	if (rc) {
		pr_err("register %s platform driver failed\n", drvname);
		return rc;
	}

	drvdata = xrt_drv_map2drvdata(map);
	if (drvdata) {
		/* Initialize dev_t for char dev node. */
		if (xleaf_devnode_enabled(drvdata)) {
			rc = alloc_chrdev_region(&drvdata->xsd_file_ops.xsf_dev_t, 0,
						 XRT_MAX_DEVICE_NODES, drvname);
			if (rc) {
				platform_driver_unregister(map->drv);
				pr_err("failed to alloc dev minor for %s: %d\n", drvname, rc);
				return rc;
			}
		} else {
			drvdata->xsd_file_ops.xsf_dev_t = (dev_t)-1;
		}
	}

	ida_init(&map->ida);

	pr_info("%s registered successfully\n", drvname);

	return 0;
}

static void xrt_drv_unregister_driver(struct xrt_drv_map *map)
{
	const char *drvname = XRT_DRVNAME(map->drv);
	struct xrt_subdev_drvdata *drvdata;

	ida_destroy(&map->ida);

	drvdata = xrt_drv_map2drvdata(map);
	if (drvdata && drvdata->xsd_file_ops.xsf_dev_t != (dev_t)-1) {
		unregister_chrdev_region(drvdata->xsd_file_ops.xsf_dev_t,
					 XRT_MAX_DEVICE_NODES);
	}

	platform_driver_unregister(map->drv);

	pr_info("%s unregistered successfully\n", drvname);
}

int xleaf_register_driver(enum xrt_subdev_id id,
			  struct platform_driver *drv,
			  struct xrt_subdev_endpoints *eps)
{
	struct xrt_drv_map *map;

	mutex_lock(&xrt_lib_lock);

	map = xrt_drv_find_map_by_id_nolock(id);
	if (map) {
		mutex_unlock(&xrt_lib_lock);
		pr_err("Id %d already has a registered driver, 0x%p\n",
		       id, map->drv);
		return -EEXIST;
	}

	map = vzalloc(sizeof(*map));
	if (!map) {
		mutex_unlock(&xrt_lib_lock);
		return -ENOMEM;
	}
	map->id = id;
	map->drv = drv;
	map->eps = eps;

	xrt_drv_register_driver(map);

	list_add(&map->list, &xrt_drv_maps);

	mutex_unlock(&xrt_lib_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(xleaf_register_driver);

void xleaf_unregister_driver(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map;

	mutex_lock(&xrt_lib_lock);

	map = xrt_drv_find_map_by_id_nolock(id);
	if (!map) {
		mutex_unlock(&xrt_lib_lock);
		pr_err("Id %d has no registered driver\n", id);
		return;
	}

	list_del(&map->list);

	mutex_unlock(&xrt_lib_lock);

	xrt_drv_unregister_driver(map);
	vfree(map);
}
EXPORT_SYMBOL_GPL(xleaf_unregister_driver);

const char *xrt_drv_name(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);

	if (map)
		return XRT_DRVNAME(map->drv);
	return NULL;
}

int xrt_drv_get_instance(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);

	return ida_alloc_range(&map->ida, 0, XRT_MAX_DEVICE_NODES, GFP_KERNEL);
}

void xrt_drv_put_instance(enum xrt_subdev_id id, int instance)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);

	ida_free(&map->ida, instance);
}

struct xrt_subdev_endpoints *xrt_drv_get_endpoints(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);
	struct xrt_subdev_endpoints *eps;

	eps = map ? map->eps : NULL;
	return eps;
}

/* Leaf driver's module init/fini callbacks. */
static void (*leaf_init_fini_cbs[])(bool) = {
	group_leaf_init_fini,
	vsec_leaf_init_fini,
	vsec_golden_leaf_init_fini,
	devctl_leaf_init_fini,
	axigate_leaf_init_fini,
	icap_leaf_init_fini,
	calib_leaf_init_fini,
	qspi_leaf_init_fini,
	mailbox_leaf_init_fini,
	cmc_leaf_init_fini,
	clkfreq_leaf_init_fini,
	clock_leaf_init_fini,
	ucs_leaf_init_fini,
};

static __init int xrt_lib_init(void)
{
	int i;

	xrt_class = class_create(THIS_MODULE, XRT_IPLIB_MODULE_NAME);
	if (IS_ERR(xrt_class))
		return PTR_ERR(xrt_class);

	for (i = 0; i < ARRAY_SIZE(leaf_init_fini_cbs); i++)
		leaf_init_fini_cbs[i](true);
	return 0;
}

static __exit void xrt_lib_fini(void)
{
	struct xrt_drv_map *map;
	int i;

	for (i = 0; i < ARRAY_SIZE(leaf_init_fini_cbs); i++)
		leaf_init_fini_cbs[i](false);

	mutex_lock(&xrt_lib_lock);

	while (!list_empty(&xrt_drv_maps)) {
		map = list_first_entry_or_null(&xrt_drv_maps, struct xrt_drv_map, list);
		pr_err("Unloading module with %s still registered\n", XRT_DRVNAME(map->drv));
		list_del(&map->list);
		mutex_unlock(&xrt_lib_lock);
		xrt_drv_unregister_driver(map);
		vfree(map);
		mutex_lock(&xrt_lib_lock);
	}

	mutex_unlock(&xrt_lib_lock);

	class_destroy(xrt_class);
}

module_init(xrt_lib_init);
module_exit(xrt_lib_fini);

MODULE_VERSION(XRT_IPLIB_MODULE_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
