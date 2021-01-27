// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Group Driver
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include "xleaf.h"
#include "subdev_pool.h"
#include "group.h"
#include "metadata.h"
#include "main.h"

#define	XRT_GRP "xrt_group"

struct xrt_group {
	struct platform_device *pdev;
	struct xrt_subdev_pool leaves;
	bool leaves_created;
	struct mutex lock; /* lock for group */
};

static int xrt_grp_root_cb(struct device *dev, void *parg,
			   u32 cmd, void *arg)
{
	int rc;
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	struct xrt_group *xg = (struct xrt_group *)parg;

	switch (cmd) {
	case XRT_ROOT_GET_LEAF_HOLDERS: {
		struct xrt_root_ioctl_get_holders *holders =
			(struct xrt_root_ioctl_get_holders *)arg;
		rc = xrt_subdev_pool_get_holders(&xg->leaves,
						 holders->xpigh_pdev,
						 holders->xpigh_holder_buf,
						 holders->xpigh_holder_buf_len);
		break;
	}
	default:
		/* Forward parent call to root. */
		rc = xrt_subdev_root_ioctl(pdev, cmd, arg);
		break;
	}

	return rc;
}

static int xrt_grp_create_leaves(struct xrt_group *xg)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(xg->pdev);
	enum xrt_subdev_id did;
	struct xrt_subdev_endpoints *eps = NULL;
	int ep_count = 0, i, ret = 0, failed = 0;
	long mlen;
	char *dtb, *grp_dtb = NULL;
	const char *ep_name;

	mutex_lock(&xg->lock);

	if (xg->leaves_created) {
		mutex_unlock(&xg->lock);
		return -EEXIST;
	}

	xrt_info(xg->pdev, "bringing up leaves...");

	/* Create all leaves based on dtb. */
	if (!pdata)
		goto bail;

	mlen = xrt_md_size(DEV(xg->pdev), pdata->xsp_dtb);
	if (mlen <= 0) {
		xrt_err(xg->pdev, "invalid dtb, len %ld", mlen);
		goto bail;
	}

	grp_dtb = vmalloc(mlen);
	if (!grp_dtb)
		goto bail;

	memcpy(grp_dtb, pdata->xsp_dtb, mlen);
	for (did = 0; did < XRT_SUBDEV_NUM;) {
		eps = eps ? eps + 1 : xrt_drv_get_endpoints(did);
		if (!eps || !eps->xse_names) {
			did++;
			eps = NULL;
			continue;
		}
		ret = xrt_md_create(DEV(xg->pdev), &dtb);
		if (ret) {
			xrt_err(xg->pdev, "create md failed, drv %s",
				xrt_drv_name(did));
			failed++;
			continue;
		}
		for (i = 0; eps->xse_names[i].ep_name ||
		     eps->xse_names[i].regmap_name; i++) {
			ep_name = (char *)eps->xse_names[i].ep_name;
			if (!ep_name) {
				(void)xrt_md_get_compatible_epname(DEV(xg->pdev),
								    grp_dtb,
								    eps->xse_names[i].regmap_name,
								    &ep_name);
			}
			if (!ep_name)
				continue;

			ret = xrt_md_copy_endpoint(DEV(xg->pdev),
						   dtb, grp_dtb, ep_name,
						   (char *)eps->xse_names[i].regmap_name,
						   NULL);
			if (ret)
				continue;
			xrt_md_del_endpoint(DEV(xg->pdev), grp_dtb, ep_name,
					    (char *)eps->xse_names[i].regmap_name);
			ep_count++;
		}
		if (ep_count >= eps->xse_min_ep) {
			ret = xrt_subdev_pool_add(&xg->leaves, did,
						  xrt_grp_root_cb, xg, dtb);
			eps = NULL;
			if (ret < 0) {
				failed++;
				xrt_err(xg->pdev, "failed to create %s: %d",
					xrt_drv_name(did), ret);
			}
		} else if (ep_count > 0) {
			xrt_md_copy_all_eps(DEV(xg->pdev), grp_dtb, dtb);
		}
		vfree(dtb);
		ep_count = 0;
	}

	xg->leaves_created = true;

bail:
	mutex_unlock(&xg->lock);

	if (grp_dtb)
		vfree(grp_dtb);

	return failed == 0 ? 0 : -ECHILD;
}

static int xrt_grp_remove_leaves(struct xrt_group *xg)
{
	int rc;

	mutex_lock(&xg->lock);

	if (!xg->leaves_created) {
		mutex_unlock(&xg->lock);
		return 0;
	}

	xrt_info(xg->pdev, "tearing down leaves...");
	rc = xrt_subdev_pool_fini(&xg->leaves);
	xg->leaves_created = false;

	mutex_unlock(&xg->lock);

	return rc;
}

static int xrt_grp_probe(struct platform_device *pdev)
{
	struct xrt_group *xg;

	xrt_info(pdev, "probing...");

	xg = devm_kzalloc(&pdev->dev, sizeof(*xg), GFP_KERNEL);
	if (!xg)
		return -ENOMEM;

	xg->pdev = pdev;
	mutex_init(&xg->lock);
	xrt_subdev_pool_init(DEV(pdev), &xg->leaves);
	platform_set_drvdata(pdev, xg);

	return 0;
}

static int xrt_grp_remove(struct platform_device *pdev)
{
	struct xrt_group *xg = platform_get_drvdata(pdev);

	xrt_info(pdev, "leaving...");
	return xrt_grp_remove_leaves(xg);
}

static int xrt_grp_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	int rc = 0;
	struct xrt_group *xg = platform_get_drvdata(pdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Simply forward to every child. */
		xrt_subdev_pool_handle_event(&xg->leaves,
					     (struct xrt_event *)arg);
		break;
	case XRT_GROUP_GET_LEAF: {
		struct xrt_root_ioctl_get_leaf *get_leaf =
			(struct xrt_root_ioctl_get_leaf *)arg;

		rc = xrt_subdev_pool_get(&xg->leaves, get_leaf->xpigl_match_cb,
					 get_leaf->xpigl_match_arg,
					 DEV(get_leaf->xpigl_pdev),
					 &get_leaf->xpigl_leaf);
		break;
	}
	case XRT_GROUP_PUT_LEAF: {
		struct xrt_root_ioctl_put_leaf *put_leaf =
			(struct xrt_root_ioctl_put_leaf *)arg;

		rc = xrt_subdev_pool_put(&xg->leaves, put_leaf->xpipl_leaf,
					 DEV(put_leaf->xpipl_pdev));
		break;
	}
	case XRT_GROUP_INIT_CHILDREN:
		rc = xrt_grp_create_leaves(xg);
		break;
	case XRT_GROUP_FINI_CHILDREN:
		rc = xrt_grp_remove_leaves(xg);
		break;
	case XRT_GROUP_TRIGGER_EVENT:
		xrt_subdev_pool_trigger_event(&xg->leaves, (enum xrt_events)(uintptr_t)arg);
		break;
	default:
		xrt_err(pdev, "unknown IOCTL cmd %d", cmd);
		rc = -EINVAL;
		break;
	}
	return rc;
}

static struct xrt_subdev_drvdata xrt_grp_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_grp_ioctl,
	},
};

static const struct platform_device_id xrt_grp_id_table[] = {
	{ XRT_GRP, (kernel_ulong_t)&xrt_grp_data },
	{ },
};

struct platform_driver xrt_group_driver = {
	.driver	= {
		.name    = XRT_GRP,
	},
	.probe   = xrt_grp_probe,
	.remove  = xrt_grp_remove,
	.id_table = xrt_grp_id_table,
};
