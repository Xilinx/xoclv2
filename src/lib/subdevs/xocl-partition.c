// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Partition Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include "xocl-subdev.h"
#include "xocl-partition.h"

#define	XOCL_PART "xocl_partition"

struct xocl_partition {
	struct platform_device *pdev;
	struct list_head leaves;
};

static long xocl_part_parent_cb(struct device *dev, u32 cmd, u64 arg)
{
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);

	xocl_info(pdev, "forwarding parent call, cmd %d", cmd);
	return xocl_subdev_parent_ioctl(pdev, cmd, arg);
}

static int xocl_part_create_subdevs(struct xocl_partition *xp)
{
	struct xocl_subdev *sdev;
	struct platform_device *pdev = xp->pdev;

	if (!list_empty(&xp->leaves))
		return 0;

	/* Create 1st leaf. */
	sdev = xocl_subdev_create(&pdev->dev, XOCL_SUBDEV_TEST,
		PLATFORM_DEVID_AUTO, xocl_part_parent_cb, NULL);
	if (sdev)
		list_add(&sdev->xs_dev_list, &xp->leaves);
#if 0
	/* Create 2nd leaf. */
	sdev = xocl_subdev_create(&pdev->dev, XOCL_SUBDEV_TEST,
		PLATFORM_DEVID_AUTO, xocl_part_parent_cb, NULL);
	if (sdev)
		list_add(&sdev->xs_dev_list, &xp->leaves);
#endif
	return 0;
}

static int xocl_part_probe(struct platform_device *pdev)
{
	struct xocl_partition *xp;

	xocl_info(pdev, "probing...");

	xp = devm_kzalloc(&pdev->dev, sizeof(*xp), GFP_KERNEL);
	if (!xp) {
		xocl_info(pdev, "failed to alloc xocl_partition");
		return -ENOMEM;
	}
	xp->pdev = pdev;
	platform_set_drvdata(pdev, xp);

	INIT_LIST_HEAD(&xp->leaves);

	return 0;
}

static int xocl_part_remove(struct platform_device *pdev)
{
	struct xocl_partition *xp = platform_get_drvdata(pdev);

	xocl_info(pdev, "leaving...");

	while (!list_empty(&xp->leaves)) {
		struct xocl_subdev *sdev = list_first_entry(&xp->leaves,
			struct xocl_subdev, xs_dev_list);
		list_del(&sdev->xs_dev_list);
		xocl_subdev_destroy(sdev);
	}

	return 0;
}

static int xocl_part_get_leaf(struct xocl_partition *xp,
	struct xocl_partition_ioctl_get_leaf *get_leaf)
{
	struct list_head *ptr;
	struct xocl_subdev *sdev;
	bool found = false;
	xocl_leaf_match_t match_cb = get_leaf->xpart_match_cb;

	list_for_each(ptr, &xp->leaves) {
		sdev = list_entry(ptr, struct xocl_subdev, xs_dev_list);

		if (sdev->xs_id != get_leaf->xpart_id)
			continue;

		if (match_cb)
			found = match_cb(sdev, get_leaf->xpart_match_arg);
		else
			found = true;

		if (found)
			break;
	}

	if (found)
		get_leaf->xpart_leaf = sdev->xs_pdev;

	return found ? 0 : -ENOENT;
}

static long xocl_part_ioctl(struct platform_device *pdev, u32 cmd, u64 arg)
{
	long rc = 0;
	struct xocl_partition *xp = platform_get_drvdata(pdev);

	xocl_info(pdev, "handling IOCTL cmd %d", cmd);

	switch (cmd) {
	case XOCL_PARTITION_GET_LEAF:
		rc = xocl_part_get_leaf(xp,
			(struct xocl_partition_ioctl_get_leaf *)arg);
		break;
	default:
		xocl_err(pdev, "unknown IOCTL cmd %d", cmd);
		rc = -EINVAL;
		break;
	}
	return rc;
}

static int xocl_part_online(struct platform_device *pdev)
{
	int rc;
	struct xocl_partition *xp = platform_get_drvdata(pdev);

	rc = xocl_part_create_subdevs(xp);
	if (rc)
		return rc;
	return 0;
}

struct xocl_subdev_drvdata xocl_part_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_part_ioctl,
		.xsd_online = xocl_part_online,
	},
};

static const struct platform_device_id xocl_part_id_table[] = {
	{ XOCL_PART, (kernel_ulong_t)&xocl_part_data },
	{ },
};

struct platform_driver xocl_partition_driver = {
	.driver	= {
		.name    = XOCL_PART,
	},
	.probe   = xocl_part_probe,
	.remove  = xocl_part_remove,
	.id_table = xocl_part_id_table,
};
