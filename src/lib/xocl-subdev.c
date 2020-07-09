// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include "xocl-subdev.h"

#define	XOCL_IPLIB_MODULE_NAME	        "xocl-lib"
#define	XOCL_IPLIB_MODULE_VERSION	"4.0.0"

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
} xocl_subdev_maps[] = {
	{ XOCL_SUBDEV_PART, &xocl_partition_driver, },
	{ XOCL_SUBDEV_TEST, &xocl_test_driver, },
};

static inline const char *
xocl_subdev_map2name(struct xocl_subdev_map *map)
{
	return map->drv->driver.name;
}

static inline struct xocl_subdev_data *
xocl_subdev_map2drvdata(struct xocl_subdev_map *map)
{
	return (struct xocl_subdev_data *)map->drv->id_table[0].driver_data;
}

static int xocl_subdev_register_driver(struct xocl_subdev_map *map)
{
	struct xocl_subdev_data *data = xocl_subdev_map2drvdata(map);
	int rc = platform_driver_register(map->drv);

	if (rc) {
		pr_err("can't register subdev driver: %s",
			xocl_subdev_map2name(map));
		return rc;
	}

	if (data && data->xsd_dev_ops.xsd_post_init) {
		rc = data->xsd_dev_ops.xsd_post_init();
		if (rc) {
			platform_driver_unregister(map->drv);
			pr_err("failed to post-init subdev driver: %s: %d",
				xocl_subdev_map2name(map), rc);
			return rc;
		}
	}

	/* TODO: Alloc device node regsion, if needed. */

	pr_info("registered subdev driver: %s\n", xocl_subdev_map2name(map));
	return 0;
}

static void xocl_subdev_unregister_driver(struct xocl_subdev_map *map)
{
	struct xocl_subdev_data *data = xocl_subdev_map2drvdata(map);

	/* TODO: Free device node regsion, if needed. */

	if (data && data->xsd_dev_ops.xsd_pre_exit)
		data->xsd_dev_ops.xsd_pre_exit();

	platform_driver_unregister(map->drv);

	pr_info("unregistered subdev driver: %s\n", xocl_subdev_map2name(map));
}

static __init int xocl_subdev_register_drivers(void)
{
	int i;
	int rc;

	xocl_class = class_create(THIS_MODULE, XOCL_IPLIB_MODULE_NAME);
	if (IS_ERR(xocl_class))
		return PTR_ERR(xocl_class);

	for (i = 0; i < ARRAY_SIZE(xocl_subdev_maps); i++) {
		rc = xocl_subdev_register_driver(&xocl_subdev_maps[i]);
		if (rc)
			break;

	}
	if (!rc)
		return 0;

	while (i-- > 0)
		xocl_subdev_unregister_driver(&xocl_subdev_maps[i]);
	class_destroy(xocl_class);
	return rc;
}

static __exit void xocl_subdev_unregister_drivers(void)
{
	int i;

	/* TODO: Free device node region, if needed. */

	for (i = 0; i < ARRAY_SIZE(xocl_subdev_maps); i++)
		xocl_subdev_unregister_driver(&xocl_subdev_maps[i]);
	class_destroy(xocl_class);
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

struct platform_driver *
xocl_subdev_id2drv(enum xocl_subdev_id id)
{
	struct xocl_subdev_map *map = xocl_subdev_find_map_by_id(id);

	if (!map)
		return NULL;
	return map->drv;
}

static struct xocl_subdev *
xocl_subdev_alloc(xocl_subdev_parent_cb_t pcb, void *dtb, size_t dtb_len)
{
	size_t sz = sizeof(struct xocl_subdev) + dtb_len;
	struct xocl_subdev *sdev = vzalloc(sz);

	if (!sdev)
		return NULL;

	INIT_LIST_HEAD(&sdev->xs_dev_list);
	sdev->xs_priv.xsp_parent_cb = pcb;
	sdev->xs_priv.xsp_dtb = (char *)sdev + sizeof(struct xocl_subdev);
	sdev->xs_priv.xsp_dtb_len = dtb_len;
	(void) memcpy(sdev->xs_priv.xsp_dtb, dtb, dtb_len);
	return sdev;
}

static void xocl_subdev_free(struct xocl_subdev *sdev)
{
	vfree(sdev);
}

struct xocl_subdev *
xocl_subdev_create_partition(struct pci_dev *root, enum xocl_partition_id id,
	xocl_subdev_parent_cb_t pcb, void *dtb, size_t dtb_len)
{
	struct xocl_subdev_map *map =
		xocl_subdev_find_map_by_id(XOCL_SUBDEV_PART);
	struct xocl_subdev *sdev = xocl_subdev_alloc(pcb, dtb, dtb_len);
	struct device *parent = &root->dev;

	BUG_ON(map == NULL);

	if (!sdev) {
		xocl_err(parent, "failed to alloc subdev.");
		return NULL;
	}

	sdev->xs_drv = map->drv;
	sdev->xs_pdev = platform_device_register_data(parent,
		map->drv->driver.name, id,
		&sdev->xs_priv, sizeof(struct xocl_subdev_priv));
	if (IS_ERR(sdev->xs_pdev)) {
		xocl_err(parent, "failed to create subdev: %ld.",
			PTR_ERR(sdev->xs_pdev));
		xocl_subdev_free(sdev);
		return NULL;
	}

	if (device_attach(&sdev->xs_pdev->dev) != 1) {
		xocl_err(parent, "failed to attach subdev.");
		platform_device_unregister(sdev->xs_pdev);
		xocl_subdev_free(sdev);
		return NULL;
	}

	return sdev;
}

struct xocl_subdev *
xocl_subdev_create_leaf(struct platform_device *part, enum xocl_subdev_id id,
	xocl_subdev_parent_cb_t pcb, void *dtb, size_t dtb_len)
{
	struct xocl_subdev_map *map;
	struct xocl_subdev *sdev;
	struct device *parent = &part->dev;

	map = xocl_subdev_find_map_by_id(id);
	if (!map) {
		xocl_err(parent, "failed to find subdev.");
		return NULL;
	}

	sdev = xocl_subdev_alloc(pcb, dtb, dtb_len);
	if (!sdev) {
		xocl_err(parent, "failed to alloc subdev.");
		return NULL;
	}

	sdev->xs_drv = map->drv;
	sdev->xs_pdev = platform_device_register_resndata(parent,
		map->drv->driver.name, PLATFORM_DEVID_AUTO,
		NULL, 0, /* TODO: find out IO and IRQ resources from dtb */
		&sdev->xs_priv, sizeof(struct xocl_subdev_priv));
	if (IS_ERR(sdev->xs_pdev)) {
		xocl_err(parent, "failed to create subdev: %ld.",
			PTR_ERR(sdev->xs_pdev));
		xocl_subdev_free(sdev);
		return NULL;
	}

	if (device_attach(&sdev->xs_pdev->dev) != 1) {
		xocl_err(parent, "failed to attach subdev.");
		platform_device_unregister(sdev->xs_pdev);
		xocl_subdev_free(sdev);
		return NULL;
	}

	/*
	 * TODO: create device node if needed
	 */

	return sdev;
}

void xocl_subdev_destroy(struct xocl_subdev *sdev)
{
	/*
	 * TODO: remove device node if needed
	 */

	platform_device_unregister(sdev->xs_pdev);
	xocl_subdev_free(sdev);
}

long xocl_subdev_parent_ioctl(struct platform_device *pdev, u32 cmd, u64 arg)
{
	struct device *dev = &pdev->dev;
	struct xocl_subdev_priv *priv = dev_get_platdata(dev);

	return (*priv->xsp_parent_cb)(dev->parent, cmd, arg);
}

long xocl_subdev_ioctl(xocl_subdev_leaf_handle_t handle, u32 cmd, u64 arg)
{
	struct platform_device *pdev = (struct platform_device *)handle;
	struct xocl_subdev_data *drvdata = (struct xocl_subdev_data *)
		platform_get_device_id(pdev)->driver_data;

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
		xocl_err(&pdev->dev, "failed to find leaf subdev: %ld.", rc);
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
EXPORT_SYMBOL_GPL(xocl_subdev_id2drv);
EXPORT_SYMBOL_GPL(xocl_subdev_get_leaf);

MODULE_VERSION(XOCL_IPLIB_MODULE_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
