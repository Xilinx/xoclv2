// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Partition Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
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
	bool leaves_created;
	struct mutex lock;
};

static int xocl_part_parent_cb(struct device *dev, u32 cmd, void *arg)
{
	int rc;
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	struct xocl_partition *xp = platform_get_drvdata(pdev);

	switch (cmd) {
	case XOCL_PARENT_GET_HOLDERS: {
		struct xocl_parent_ioctl_get_holders *holders =
			(struct xocl_parent_ioctl_get_holders *)arg;
		rc = xocl_subdev_pool_get_holders(&xp->leaves,
			holders->xpigh_pdev, holders->xpigh_holder_buf,
			holders->xpigh_holder_buf_len);
		break;
	}
	default:
		/* Forward parent call to root. */
		rc = xocl_subdev_parent_ioctl(pdev, cmd, arg);
		break;
	}

	return rc;
}

static int xocl_part_create_leaves(struct xocl_partition *xp)
{
	mutex_lock(&xp->lock);

	if (xp->leaves_created) {
		mutex_unlock(&xp->lock);
		return -EEXIST;
	}

	xocl_info(xp->pdev, "bringing up leaves...");

	/* TODO: Create all leaves based on dtb. */

	(void) xocl_subdev_pool_add(&xp->leaves, XOCL_SUBDEV_TEST,
		xocl_part_parent_cb, NULL);
	xp->leaves_created = true;

	mutex_unlock(&xp->lock);

	return 0;
}

static int xocl_part_remove_leaves(struct xocl_partition *xp)
{
	int rc;

	mutex_lock(&xp->lock);

	if (!xp->leaves_created) {
		mutex_unlock(&xp->lock);
		return 0;
	}

	xocl_info(xp->pdev, "tearing down leaves...");
	rc = xocl_subdev_pool_fini(&xp->leaves);
	xp->leaves_created = false;

	mutex_unlock(&xp->lock);

	return rc;
}

static int xocl_part_probe(struct platform_device *pdev)
{
	struct xocl_partition *xp;

	xocl_info(pdev, "probing...");

	xp = devm_kzalloc(&pdev->dev, sizeof(*xp), GFP_KERNEL);
	if (!xp)
		return -ENOMEM;

	xp->pdev = pdev;
	mutex_init(&xp->lock);
	xocl_subdev_pool_init(DEV(pdev), &xp->leaves);
	platform_set_drvdata(pdev, xp);

	return 0;
}

static int xocl_part_remove(struct platform_device *pdev)
{
	struct xocl_partition *xp = platform_get_drvdata(pdev);

	xocl_info(pdev, "leaving...");
	return xocl_part_remove_leaves(xp);
}

static int xocl_part_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	int rc = 0;
	struct xocl_partition *xp = platform_get_drvdata(pdev);

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
	case XOCL_PARTITION_INIT_CHILDREN:
		rc = xocl_part_create_leaves(xp);
		break;
	case XOCL_PARTITION_FINI_CHILDREN:
		rc = xocl_part_remove_leaves(xp);
		break;
	case XOCL_PARTITION_EVENT: {
		struct xocl_partition_ioctl_event *evt =
			(struct xocl_partition_ioctl_event *)arg;
		struct xocl_parent_ioctl_evt_cb *cb = evt->xpie_cb;

		rc = xocl_subdev_pool_event(&xp->leaves, cb->xevt_pdev,
			cb->xevt_match_cb, cb->xevt_match_arg, cb->xevt_cb,
			evt->xpie_evt);
		break;
	}
	default:
		xocl_err(pdev, "unknown IOCTL cmd %d", cmd);
		rc = -EINVAL;
		break;
	}
	return rc;
}

struct xocl_subdev_drvdata xocl_part_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_part_ioctl,
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
