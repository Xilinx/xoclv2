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
#include "xrt-subdev.h"
#include "xrt-parent.h"
#include "xrt-partition.h"
#include "xrt-metadata.h"
#include "../xrt-main.h"

#define	XRT_PART "xrt_partition"

struct xrt_partition {
	struct platform_device *pdev;
	struct xrt_subdev_pool leaves;
	bool leaves_created;
	struct mutex lock;
};

static int xrt_part_parent_cb(struct device *dev, void *parg,
	u32 cmd, void *arg)
{
	int rc;
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	struct xrt_partition *xp = (struct xrt_partition *)parg;

	switch (cmd) {
	case XRT_PARENT_GET_LEAF_HOLDERS: {
		struct xrt_parent_ioctl_get_holders *holders =
			(struct xrt_parent_ioctl_get_holders *)arg;
		rc = xrt_subdev_pool_get_holders(&xp->leaves,
			holders->xpigh_pdev, holders->xpigh_holder_buf,
			holders->xpigh_holder_buf_len);
		break;
	}
	default:
		/* Forward parent call to root. */
		rc = xrt_subdev_parent_ioctl(pdev, cmd, arg);
		break;
	}

	return rc;
}

static int xrt_part_create_leaves(struct xrt_partition *xp)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(xp->pdev);
	enum xrt_subdev_id did;
	struct xrt_subdev_endpoints *eps = NULL;
	int ep_count = 0, i, ret = 0, failed = 0;
	long mlen;
	char *dtb, *part_dtb = NULL;
	const char *ep_name;


	mutex_lock(&xp->lock);

	if (xp->leaves_created) {
		mutex_unlock(&xp->lock);
		return -EEXIST;
	}

	xrt_info(xp->pdev, "bringing up leaves...");

	/* Create all leaves based on dtb. */
	if (!pdata)
		goto bail;

	mlen = xrt_md_size(DEV(xp->pdev), pdata->xsp_dtb);
	if (mlen <= 0) {
		xrt_err(xp->pdev, "invalid dtb, len %ld", mlen);
		goto bail;
	}

	part_dtb = vmalloc(mlen);
	if (!part_dtb)
		goto bail;

	memcpy(part_dtb, pdata->xsp_dtb, mlen);
	for (did = 0; did < XRT_SUBDEV_NUM;) {
		eps = eps ? eps + 1 : xrt_drv_get_endpoints(did);
		if (!eps || !eps->xse_names) {
			did++;
			eps = NULL;
			continue;
		}
		ret = xrt_md_create(DEV(xp->pdev), &dtb);
		if (ret) {
			xrt_err(xp->pdev, "create md failed, drv %s",
				xrt_drv_name(did));
			failed++;
			continue;
		}
		for (i = 0; eps->xse_names[i].ep_name ||
		    eps->xse_names[i].regmap_name; i++) {
			if (!eps->xse_names[i].ep_name) {
				ret = xrt_md_get_compatible_epname(
					DEV(xp->pdev), part_dtb,
					eps->xse_names[i].regmap_name,
					&ep_name);
				if (ret)
					continue;
			} else
				ep_name = (char *)eps->xse_names[i].ep_name;
			ret = xrt_md_copy_endpoint(DEV(xp->pdev),
				dtb, part_dtb, ep_name,
				(char *)eps->xse_names[i].regmap_name, NULL);
			if (ret)
				continue;
			xrt_md_del_endpoint(DEV(xp->pdev), part_dtb, ep_name,
				(char *)eps->xse_names[i].regmap_name);
			ep_count++;
		}
		if (ep_count >= eps->xse_min_ep) {
			ret = xrt_subdev_pool_add(&xp->leaves, did,
				xrt_part_parent_cb, xp, dtb);
			eps = NULL;
			if (ret < 0) {
				failed++;
				xrt_err(xp->pdev, "failed to create %s: %d",
					xrt_drv_name(did), ret);
			}
		} else if (ep_count > 0) {
			xrt_md_copy_all_eps(DEV(xp->pdev), part_dtb, dtb);
		}
		vfree(dtb);
		ep_count = 0;
	}

	xp->leaves_created = true;

bail:
	mutex_unlock(&xp->lock);

	if (part_dtb)
		vfree(part_dtb);

	return failed == 0 ? 0 : -ECHILD;
}

static int xrt_part_remove_leaves(struct xrt_partition *xp)
{
	int rc;

	mutex_lock(&xp->lock);

	if (!xp->leaves_created) {
		mutex_unlock(&xp->lock);
		return 0;
	}

	xrt_info(xp->pdev, "tearing down leaves...");
	rc = xrt_subdev_pool_fini(&xp->leaves);
	xp->leaves_created = false;

	mutex_unlock(&xp->lock);

	return rc;
}

static int xrt_part_probe(struct platform_device *pdev)
{
	struct xrt_partition *xp;

	xrt_info(pdev, "probing...");

	xp = devm_kzalloc(&pdev->dev, sizeof(*xp), GFP_KERNEL);
	if (!xp)
		return -ENOMEM;

	xp->pdev = pdev;
	mutex_init(&xp->lock);
	xrt_subdev_pool_init(DEV(pdev), &xp->leaves);
	platform_set_drvdata(pdev, xp);

	return 0;
}

static int xrt_part_remove(struct platform_device *pdev)
{
	struct xrt_partition *xp = platform_get_drvdata(pdev);

	xrt_info(pdev, "leaving...");
	return xrt_part_remove_leaves(xp);
}

static int xrt_part_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	int rc = 0;
	struct xrt_partition *xp = platform_get_drvdata(pdev);

	switch (cmd) {
	case XRT_PARTITION_GET_LEAF: {
		struct xrt_parent_ioctl_get_leaf *get_leaf =
			(struct xrt_parent_ioctl_get_leaf *)arg;

		rc = xrt_subdev_pool_get(&xp->leaves, get_leaf->xpigl_match_cb,
			get_leaf->xpigl_match_arg, DEV(get_leaf->xpigl_pdev),
			&get_leaf->xpigl_leaf);
		break;
	}
	case XRT_PARTITION_PUT_LEAF: {
		struct xrt_parent_ioctl_put_leaf *put_leaf =
			(struct xrt_parent_ioctl_put_leaf *)arg;

		rc = xrt_subdev_pool_put(&xp->leaves, put_leaf->xpipl_leaf,
			DEV(put_leaf->xpipl_pdev));
		break;
	}
	case XRT_PARTITION_INIT_CHILDREN:
		rc = xrt_part_create_leaves(xp);
		break;
	case XRT_PARTITION_FINI_CHILDREN:
		rc = xrt_part_remove_leaves(xp);
		break;
	case XRT_PARTITION_EVENT: {
		struct xrt_partition_ioctl_event *evt =
			(struct xrt_partition_ioctl_event *)arg;
		struct xrt_parent_ioctl_evt_cb *cb = evt->xpie_cb;

		rc = xrt_subdev_pool_event(&xp->leaves, cb->xevt_pdev,
			cb->xevt_match_cb, cb->xevt_match_arg, cb->xevt_cb,
			evt->xpie_evt);
		break;
	}
	default:
		xrt_err(pdev, "unknown IOCTL cmd %d", cmd);
		rc = -EINVAL;
		break;
	}
	return rc;
}

struct xrt_subdev_drvdata xrt_part_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_part_ioctl,
	},
};

static const struct platform_device_id xrt_part_id_table[] = {
	{ XRT_PART, (kernel_ulong_t)&xrt_part_data },
	{ },
};

struct platform_driver xrt_partition_driver = {
	.driver	= {
		.name    = XRT_PART,
	},
	.probe   = xrt_part_probe,
	.remove  = xrt_part_remove,
	.id_table = xrt_part_id_table,
};
