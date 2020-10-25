// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/module.h>
#include "xrt-subdev.h"
#include "xrt-main.h"

#define	XRT_IPLIB_MODULE_NAME		"xrt-lib"
#define	XRT_IPLIB_MODULE_VERSION	"4.0.0"
#define	XRT_DRVNAME(drv)		((drv)->driver.name)
#define	XRT_MAX_DEVICE_NODES		128

struct mutex xrt_class_lock;
struct class *xrt_class;

/*
 * Subdev driver is known by ID to others. We map the ID to it's
 * struct platform_driver, which contains it's binding name and driver/file ops.
 * We also map it to the endpoint name in DTB as well, if it's different
 * than the driver's binding name.
 */
static struct xrt_drv_map {
	enum xrt_subdev_id id;
	struct platform_driver *drv;
	struct xrt_subdev_endpoints *eps;
	struct ida ida; /* manage driver instance and char dev minor */
} xrt_drv_maps[] = {
	{ XRT_SUBDEV_PART, &xrt_partition_driver, },
	{ XRT_SUBDEV_VSEC, &xrt_vsec_driver, xrt_vsec_endpoints, },
	{ XRT_SUBDEV_VSEC_GOLDEN, &xrt_vsec_golden_driver, xrt_vsec_golden_endpoints, },
	{ XRT_SUBDEV_GPIO, &xrt_gpio_driver, xrt_gpio_endpoints,},
	{ XRT_SUBDEV_AXIGATE, &xrt_axigate_driver, xrt_axigate_endpoints, },
	{ XRT_SUBDEV_ICAP, &xrt_icap_driver, xrt_icap_endpoints, },
	{ XRT_SUBDEV_CALIB, &xrt_calib_driver, xrt_calib_endpoints, },
	{ XRT_SUBDEV_TEST, &xrt_test_driver, xrt_test_endpoints, },
	{ XRT_SUBDEV_MGMT_MAIN, NULL, },
	{ XRT_SUBDEV_QSPI, &xrt_qspi_driver, xrt_qspi_endpoints, },
	{ XRT_SUBDEV_MAILBOX, &xrt_mailbox_driver, xrt_mailbox_endpoints, },
	{ XRT_SUBDEV_CMC, &xrt_cmc_driver, xrt_cmc_endpoints, },
	{ XRT_SUBDEV_CLKFREQ, &xrt_clkfreq_driver, xrt_clkfreq_endpoints, },
	{ XRT_SUBDEV_CLOCK, &xrt_clock_driver, xrt_clock_endpoints, },
	{ XRT_SUBDEV_UCS, &xrt_ucs_driver, xrt_ucs_endpoints, },
};

static inline struct xrt_subdev_drvdata *
xrt_drv_map2drvdata(struct xrt_drv_map *map)
{
	return (struct xrt_subdev_drvdata *)map->drv->id_table[0].driver_data;
}

static struct xrt_drv_map *
xrt_drv_find_map_by_id(enum xrt_subdev_id id)
{
	int i;
	struct xrt_drv_map *map = NULL;

	for (i = 0; i < ARRAY_SIZE(xrt_drv_maps); i++) {
		struct xrt_drv_map *tmap = &xrt_drv_maps[i];

		if (tmap->id != id)
			continue;
		map = tmap;
		break;
	}
	return map;
}

static int xrt_drv_register_driver(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);
	struct xrt_subdev_drvdata *drvdata;
	int rc = 0;
	const char *drvname;

	BUG_ON(!map);

	if (!map->drv) {
		pr_info("skip registration of subdev driver for id %d\n", id);
		return rc;
	}
	drvname = XRT_DRVNAME(map->drv);

	rc = platform_driver_register(map->drv);
	if (rc) {
		pr_err("register %s subdev driver failed\n", drvname);
		return rc;
	}

	drvdata = xrt_drv_map2drvdata(map);
	if (drvdata && drvdata->xsd_dev_ops.xsd_post_init) {
		rc = drvdata->xsd_dev_ops.xsd_post_init();
		if (rc) {
			platform_driver_unregister(map->drv);
			pr_err("%s's post-init, ret %d\n", drvname, rc);
			return rc;
		}
	}

	if (drvdata) {
		/* Initialize dev_t for char dev node. */
		if (xrt_devnode_enabled(drvdata)) {
			rc = alloc_chrdev_region(
				&drvdata->xsd_file_ops.xsf_dev_t, 0,
				XRT_MAX_DEVICE_NODES, drvname);
			if (rc) {
				if (drvdata->xsd_dev_ops.xsd_pre_exit)
					drvdata->xsd_dev_ops.xsd_pre_exit();
				platform_driver_unregister(map->drv);
				pr_err("failed to alloc dev minor for %s: %d\n",
					drvname, rc);
				return rc;
			}
		} else {
			drvdata->xsd_file_ops.xsf_dev_t = (dev_t)-1;
		}
	}

	ida_init(&map->ida);

	pr_info("registered %s subdev driver\n", drvname);
	return 0;
}

static void xrt_drv_unregister_driver(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);
	struct xrt_subdev_drvdata *drvdata;
	const char *drvname;

	BUG_ON(!map);
	if (!map->drv) {
		pr_info("skip unregistration of subdev driver for id %d\n", id);
		return;
	}

	drvname = XRT_DRVNAME(map->drv);

	ida_destroy(&map->ida);

	drvdata = xrt_drv_map2drvdata(map);
	if (drvdata && drvdata->xsd_file_ops.xsf_dev_t != (dev_t)-1) {
		unregister_chrdev_region(drvdata->xsd_file_ops.xsf_dev_t,
			XRT_MAX_DEVICE_NODES);
	}

	if (drvdata && drvdata->xsd_dev_ops.xsd_pre_exit)
		drvdata->xsd_dev_ops.xsd_pre_exit();

	platform_driver_unregister(map->drv);

	pr_info("unregistered %s subdev driver\n", drvname);
}

int xrt_subdev_register_external_driver(enum xrt_subdev_id id,
	struct platform_driver *drv, struct xrt_subdev_endpoints *eps)
{
	int i;
	int result = 0;

	mutex_lock(&xrt_class_lock);
	for (i = 0; i < ARRAY_SIZE(xrt_drv_maps); i++) {
		struct xrt_drv_map *map = &xrt_drv_maps[i];

		if (map->id != id)
			continue;
		if (map->drv) {
			result = -EEXIST;
			pr_err("Id %d already has a registered driver, 0x%p\n",
				id, map->drv);
			break;
		}
		map->drv = drv;
		BUG_ON(map->eps);
		map->eps = eps;
		xrt_drv_register_driver(id);
	}
	mutex_unlock(&xrt_class_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(xrt_subdev_register_external_driver);

void xrt_subdev_unregister_external_driver(enum xrt_subdev_id id)
{
	int i;

	mutex_lock(&xrt_class_lock);
	for (i = 0; i < ARRAY_SIZE(xrt_drv_maps); i++) {
		struct xrt_drv_map *map = &xrt_drv_maps[i];

		if (map->id != id)
			continue;
		xrt_drv_unregister_driver(id);
		map->drv = NULL;
		map->eps = NULL;
		break;
	}
	mutex_unlock(&xrt_class_lock);
}
EXPORT_SYMBOL_GPL(xrt_subdev_unregister_external_driver);

static __init int xrt_drv_register_drivers(void)
{
	int i;
	int rc = 0;

	mutex_init(&xrt_class_lock);
	xrt_class = class_create(THIS_MODULE, XRT_IPLIB_MODULE_NAME);
	if (IS_ERR(xrt_class))
		return PTR_ERR(xrt_class);

	for (i = 0; i < ARRAY_SIZE(xrt_drv_maps); i++) {
		rc = xrt_drv_register_driver(xrt_drv_maps[i].id);
		if (rc)
			break;
	}
	if (!rc)
		return 0;

	while (i-- > 0)
		xrt_drv_unregister_driver(xrt_drv_maps[i].id);
	class_destroy(xrt_class);
	return rc;
}

static __exit void xrt_drv_unregister_drivers(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(xrt_drv_maps); i++)
		xrt_drv_unregister_driver(xrt_drv_maps[i].id);
	class_destroy(xrt_class);
}

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

	return map ? map->eps : NULL;
}

module_init(xrt_drv_register_drivers);
module_exit(xrt_drv_unregister_drivers);

MODULE_VERSION(XRT_IPLIB_MODULE_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
