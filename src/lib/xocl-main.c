// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/module.h>
#include "xocl-subdev.h"

#define	XOCL_IPLIB_MODULE_NAME		"xocl-lib"
#define	XOCL_IPLIB_MODULE_VERSION	"4.0.0"
#define	XOCL_DRVNAME(drv)		((drv)->driver.name)
#define	XOCL_MAX_DEVICE_NODES		128

struct class *xocl_class;

/*
 * Subdev driver is known by ID to others. We map the ID to it's
 * struct platform_driver, which contains it's binding name and driver/file ops.
 * We also map it to the endpoint name in DTB as well, if it's different
 * than the driver's binding name.
 */
extern struct platform_driver xocl_partition_driver;
extern struct platform_driver xocl_test_driver;
static struct xocl_drv_map {
	enum xocl_subdev_id id;
	struct platform_driver *drv;
	char *dtb_name;
	struct ida ida; /* manage driver instance and char dev minor */
} xocl_drv_maps[] = {
	{ XOCL_SUBDEV_PART, &xocl_partition_driver, },
	{ XOCL_SUBDEV_TEST, &xocl_test_driver, },
};

static inline struct xocl_subdev_drvdata *
xocl_drv_map2drvdata(struct xocl_drv_map *map)
{
	return (struct xocl_subdev_drvdata *)map->drv->id_table[0].driver_data;
}

static struct xocl_drv_map *
xocl_drv_find_map_by_id(enum xocl_subdev_id id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(xocl_drv_maps); i++) {
		struct xocl_drv_map *map = &xocl_drv_maps[i];
		if (map->id == id)
			return map;
	}
	return NULL;
}

static int xocl_drv_register_driver(enum xocl_subdev_id id)
{
	struct xocl_drv_map *map = xocl_drv_find_map_by_id(id);
	struct xocl_subdev_drvdata *drvdata;
	int rc;
	const char *drvname;

	BUG_ON(!map);
	drvname = XOCL_DRVNAME(map->drv);

	rc = platform_driver_register(map->drv);
	if (rc) {
		pr_err("register %s subdev driver failed\n", drvname);
		return rc;
	}

	drvdata = xocl_drv_map2drvdata(map);
	if (drvdata && drvdata->xsd_dev_ops.xsd_post_init) {
		rc = drvdata->xsd_dev_ops.xsd_post_init();
		if (rc) {
			platform_driver_unregister(map->drv);
			pr_err("%s's post-init, ret %d\n", drvname, rc);
			return rc;
		}
	}

	if (drvdata && drvdata->xsd_file_ops.xsf_ops.owner) {
		rc = alloc_chrdev_region(&drvdata->xsd_file_ops.xsf_dev_t, 0,
			XOCL_MAX_DEVICE_NODES, drvname);
		if (rc) {
			if (drvdata->xsd_dev_ops.xsd_pre_exit)
				drvdata->xsd_dev_ops.xsd_pre_exit();
			platform_driver_unregister(map->drv);
			pr_err("failed to alloc dev minors for %s, ret %d\n",
				drvname, rc);
			return rc;
		}
	} else {
		drvdata->xsd_file_ops.xsf_dev_t = (dev_t)-1;
	}

	ida_init(&map->ida);

	pr_info("registered %s subdev driver\n", drvname);
	return 0;
}

static void xocl_drv_unregister_driver(enum xocl_subdev_id id)
{
	struct xocl_drv_map *map = xocl_drv_find_map_by_id(id);
	struct xocl_subdev_drvdata *drvdata;
	const char *drvname;

	BUG_ON(!map);
	drvname = XOCL_DRVNAME(map->drv);

	ida_destroy(&map->ida);

	drvdata = xocl_drv_map2drvdata(map);
	if (drvdata && drvdata->xsd_file_ops.xsf_dev_t != (dev_t)-1) {
		unregister_chrdev_region(drvdata->xsd_file_ops.xsf_dev_t,
			XOCL_MAX_DEVICE_NODES);
	}

	if (drvdata && drvdata->xsd_dev_ops.xsd_pre_exit)
		drvdata->xsd_dev_ops.xsd_pre_exit();

	platform_driver_unregister(map->drv);

	pr_info("unregistered %s subdev driver\n", drvname);
}

static __init int xocl_drv_register_drivers(void)
{
	int i;
	int rc;

	xocl_class = class_create(THIS_MODULE, XOCL_IPLIB_MODULE_NAME);
	if (IS_ERR(xocl_class))
		return PTR_ERR(xocl_class);

	for (i = 0; i < ARRAY_SIZE(xocl_drv_maps); i++) {
		rc = xocl_drv_register_driver(xocl_drv_maps[i].id);
		if (rc)
			break;
	}
	if (!rc)
		return 0;

	while (i-- > 0)
		xocl_drv_unregister_driver(xocl_drv_maps[i].id);
	class_destroy(xocl_class);
	return rc;
}

static __exit void xocl_drv_unregister_drivers(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(xocl_drv_maps); i++)
		xocl_drv_unregister_driver(xocl_drv_maps[i].id);
	class_destroy(xocl_class);
}

const char *xocl_drv_name(enum xocl_subdev_id id)
{
	struct xocl_drv_map *map = xocl_drv_find_map_by_id(id);

	if (map)
		return XOCL_DRVNAME(map->drv);
	return NULL;
}

int xocl_drv_get_instance(enum xocl_subdev_id id, int instance)
{
	int inst;
	struct xocl_drv_map *map = xocl_drv_find_map_by_id(id);

	if (instance >= 0) {
		inst = ida_alloc_range(&map->ida, instance, instance,
			GFP_KERNEL);
	} else {
		inst = ida_alloc_range(&map->ida, 0, XOCL_MAX_DEVICE_NODES,
			GFP_KERNEL);
	}
	return inst;
}

void xocl_drv_put_instance(enum xocl_subdev_id id, int instance)
{
	struct xocl_drv_map *map = xocl_drv_find_map_by_id(id);

	ida_free(&map->ida, instance);
}

module_init(xocl_drv_register_drivers);
module_exit(xocl_drv_unregister_drivers);

MODULE_VERSION(XOCL_IPLIB_MODULE_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
