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
#include "xocl-parent.h"
#include "xocl-partition.h"

#define	XOCL_PART "xocl_partition"

struct xocl_partition {
	struct platform_device *pdev;
	struct xocl_subdev_pool leaves;
	bool dev_created;
	struct mutex lock;
};

static long xocl_part_parent_cb(struct device *dev, u32 cmd, u64 arg)
{
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	/* Forward parent call to root. */
	return xocl_subdev_parent_ioctl(pdev, cmd, arg);
}

static int xocl_part_create_subdevs(struct xocl_partition *xp)
{
	bool created;

	mutex_lock(&xp->lock);
	created = xp->dev_created;
	xp->dev_created = true;
	mutex_unlock(&xp->lock);
	if (created)
		return 0;

	/* Create 1st leaf. */
	xocl_info(xp->pdev, "bringing up subdevs ...");
	(void) xocl_subdev_pool_add(&xp->leaves, XOCL_SUBDEV_TEST,
		PLATFORM_DEVID_AUTO, xocl_part_parent_cb, NULL);
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
	xocl_subdev_pool_init(DEV(xp->pdev), &xp->leaves);
	mutex_init(&xp->lock);
	platform_set_drvdata(pdev, xp);

	return 0;
}

static int xocl_part_remove(struct platform_device *pdev)
{
	struct xocl_partition *xp = platform_get_drvdata(pdev);

	xocl_info(pdev, "leaving...");
	(void) xocl_subdev_pool_fini(&xp->leaves);
	return 0;
}

static long xocl_part_ioctl(struct platform_device *pdev, u32 cmd, u64 arg)
{
	long rc = 0;
	struct xocl_partition *xp = platform_get_drvdata(pdev);

	xocl_info(pdev, "handling IOCTL cmd %d", cmd);

	switch (cmd) {
	case XOCL_PARTITION_GET_LEAF: {
		struct xocl_parent_ioctl_get_leaf *get_leaf =
			(struct xocl_parent_ioctl_get_leaf *)arg;
		rc = xocl_subdev_pool_get(&xp->leaves, get_leaf->xpigl_match_cb, 
			get_leaf->xpigl_match_arg, DEV(get_leaf->xpigl_pdev),
			&get_leaf->xpigl_leaf);
		break;
	}
	case XOCL_PARTITION_PUT_LEAF: {
		struct xocl_parent_ioctl_put_leaf *put_leaf =
			(struct xocl_parent_ioctl_put_leaf *)arg;
		rc = xocl_subdev_pool_put(&xp->leaves, put_leaf->xpipl_leaf,
			DEV(put_leaf->xpipl_pdev));
		break;
	}
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
