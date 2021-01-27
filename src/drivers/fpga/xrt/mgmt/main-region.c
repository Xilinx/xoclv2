// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2021 Xilinx, Inc.
 * Bulk of the code borrowed from XRT mgmt driver file, fmgr.c
 *
 * Authors: Lizhi.Hou@xilinx.com
 */

#include <linux/uuid.h>
#include <linux/fpga/fpga-bridge.h>
#include <linux/fpga/fpga-region.h>
#include "metadata.h"
#include "xleaf.h"
#include "xleaf/axigate.h"
#include "xclbin-helper.h"
#include "main-impl.h"

struct xmgmt_bridge {
	struct platform_device *pdev;
	const char *axigate_name;
};

struct xmgmt_region {
	struct platform_device *pdev;
	struct fpga_region *fregion;
	uuid_t intf_uuid;
	struct fpga_bridge *fbridge;
	int grp_inst;
	uuid_t dep_uuid;
	struct list_head list;
};

struct xmgmt_region_match_arg {
	struct platform_device *pdev;
	uuid_t *uuids;
	u32 uuid_num;
};

static int xmgmt_br_enable_set(struct fpga_bridge *bridge, bool enable)
{
	struct xmgmt_bridge *br_data = (struct xmgmt_bridge *)bridge->priv;
	struct platform_device *axigate_leaf;
	int rc;

	axigate_leaf = xleaf_get_leaf_by_epname(br_data->pdev, br_data->axigate_name);
	if (!axigate_leaf) {
		xrt_err(br_data->pdev, "failed to get leaf %s",
			br_data->axigate_name);
		return -ENOENT;
	}

	if (enable)
		rc = xleaf_ioctl(axigate_leaf, XRT_AXIGATE_FREE, NULL);
	else
		rc = xleaf_ioctl(axigate_leaf, XRT_AXIGATE_FREEZE, NULL);

	if (rc) {
		xrt_err(br_data->pdev, "failed to %s gate %s, rc %d",
			(enable ? "free" : "freeze"), br_data->axigate_name,
			rc);
	}

	xleaf_put_leaf(br_data->pdev, axigate_leaf);

	return rc;
}

const struct fpga_bridge_ops xmgmt_bridge_ops = {
	.enable_set = xmgmt_br_enable_set
};

static void xmgmt_destroy_bridge(struct fpga_bridge *br)
{
	struct xmgmt_bridge *br_data = br->priv;

	if (!br_data)
		return;

	xrt_info(br_data->pdev, "destroy fpga bridge %s", br_data->axigate_name);
	fpga_bridge_unregister(br);

	devm_kfree(DEV(br_data->pdev), br_data);

	fpga_bridge_free(br);
}

static struct fpga_bridge *xmgmt_create_bridge(struct platform_device *pdev,
					       char *dtb)
{
	struct xmgmt_bridge *br_data;
	struct fpga_bridge *br = NULL;
	const char *gate;
	int rc;

	br_data = devm_kzalloc(DEV(pdev), sizeof(*br_data), GFP_KERNEL);
	if (!br_data)
		return NULL;
	br_data->pdev = pdev;

	br_data->axigate_name = NODE_GATE_ULP;
	rc = xrt_md_get_epname_pointer(&pdev->dev, dtb, NODE_GATE_ULP,
				       NULL, &gate);
	if (rc) {
		br_data->axigate_name = NODE_GATE_PLP;
		rc = xrt_md_get_epname_pointer(&pdev->dev, dtb, NODE_GATE_PLP,
					       NULL, &gate);
	}
	if (rc) {
		xrt_err(pdev, "failed to get axigate, rc %d", rc);
		goto failed;
	}

	br = fpga_bridge_create(DEV(pdev), br_data->axigate_name,
				&xmgmt_bridge_ops, br_data);
	if (!br) {
		xrt_err(pdev, "failed to create bridge");
		goto failed;
	}

	rc = fpga_bridge_register(br);
	if (rc) {
		xrt_err(pdev, "failed to register bridge, rc %d", rc);
		goto failed;
	}

	xrt_info(pdev, "created fpga bridge %s", br_data->axigate_name);

	return br;

failed:
	if (br)
		fpga_bridge_free(br);
	if (br_data)
		devm_kfree(DEV(pdev), br_data);

	return NULL;
}

static void xmgmt_destroy_region(struct fpga_region *re)
{
	struct xmgmt_region *r_data = re->priv;

	xrt_info(r_data->pdev, "destroy fpga region %llx%llx",
		 re->compat_id->id_l, re->compat_id->id_h);

	fpga_region_unregister(re);

	if (r_data->grp_inst > 0)
		xleaf_destroy_group(r_data->pdev, r_data->grp_inst);

	if (r_data->fbridge)
		xmgmt_destroy_bridge(r_data->fbridge);

	if (r_data->fregion->info) {
		fpga_image_info_free(r_data->fregion->info);
		r_data->fregion->info = NULL;
	}

	fpga_region_free(re);

	devm_kfree(DEV(r_data->pdev), r_data);
}

static int xmgmt_region_match(struct device *dev, const void *data)
{
	const struct xmgmt_region_match_arg *arg = data;
	const struct fpga_region *match_re;
	int i;

	if (dev->parent != &arg->pdev->dev)
		return false;

	match_re = to_fpga_region(dev);
	/*
	 * The device tree provides both parent and child uuids for an
	 * xclbin in one array. Here we try both uuids to see if it matches
	 * with target region's compat_id. Strictly speaking we should
	 * only match xclbin's parent uuid with target region's compat_id
	 * but given the uuids by design are unique comparing with both
	 * does not hurt.
	 */
	for (i = 0; i < arg->uuid_num; i++) {
		if (!memcmp(match_re->compat_id, &arg->uuids[i],
			    sizeof(*match_re->compat_id)))
			return true;
	}

	return false;
}

static int xmgmt_region_match_base(struct device *dev, const void *data)
{
	const struct xmgmt_region_match_arg *arg = data;
	const struct fpga_region *match_re;
	const struct xmgmt_region *r_data;

	if (dev->parent != &arg->pdev->dev)
		return false;

	match_re = to_fpga_region(dev);
	r_data = match_re->priv;
	if (uuid_is_null(&r_data->dep_uuid))
		return true;

	return false;
}

static int xmgmt_region_match_by_depuuid(struct device *dev, const void *data)
{
	const struct xmgmt_region_match_arg *arg = data;
	const struct fpga_region *match_re;
	const struct xmgmt_region *r_data;

	if (dev->parent != &arg->pdev->dev)
		return false;

	match_re = to_fpga_region(dev);
	r_data = match_re->priv;
	if (!memcmp(&r_data->dep_uuid, arg->uuids, sizeof(uuid_t)))
		return true;

	return false;
}

static void xmgmt_region_cleanup(struct fpga_region *re)
{
	struct xmgmt_region *r_data = re->priv, *temp;
	struct platform_device *pdev = r_data->pdev;
	struct fpga_region *match_re = NULL;
	struct device *start_dev = NULL;
	struct xmgmt_region_match_arg arg;
	LIST_HEAD(free_list);

	list_add_tail(&r_data->list, &free_list);
	arg.pdev = pdev;
	arg.uuid_num = 1;

	while (!r_data) {
		arg.uuids = (uuid_t *)r_data->fregion->compat_id;
		match_re = fpga_region_class_find(start_dev, &arg,
						  xmgmt_region_match_by_depuuid);
		if (match_re) {
			r_data = match_re->priv;
			list_add_tail(&r_data->list, &free_list);
			start_dev = &match_re->dev;
			put_device(&match_re->dev);
			continue;
		}

		r_data = list_is_last(&r_data->list, &free_list) ? NULL :
			list_next_entry(r_data, list);
		start_dev = NULL;
	}

	list_for_each_entry_safe_reverse(r_data, temp, &free_list, list) {
		if (list_is_first(&r_data->list, &free_list)) {
			if (r_data->grp_inst > 0) {
				xleaf_destroy_group(pdev, r_data->grp_inst);
				r_data->grp_inst = -1;
			}
			if (r_data->fregion->info) {
				fpga_image_info_free(r_data->fregion->info);
				r_data->fregion->info = NULL;
			}
			continue;
		}
		xmgmt_destroy_region(r_data->fregion);
	}
}

void xmgmt_region_cleanup_all(struct platform_device *pdev)
{
	struct fpga_region *base_re;
	struct xmgmt_region_match_arg arg;

	arg.pdev = pdev;

	for (base_re = fpga_region_class_find(NULL, &arg, xmgmt_region_match_base);
	    base_re;
	    base_re = fpga_region_class_find(NULL, &arg, xmgmt_region_match_base)) {
		put_device(&base_re->dev);

		xmgmt_region_cleanup(base_re);
		xmgmt_destroy_region(base_re);
	}
}

/*
 * Program a given region with given xclbin image. Bring up the subdevs and the
 * group object to contain the subdevs.
 */
static int xmgmt_region_program(struct fpga_region *re, const void *xclbin, char *dtb)
{
	struct xmgmt_region *r_data = re->priv;
	struct platform_device *pdev = r_data->pdev;
	struct fpga_image_info *info;
	const struct axlf *xclbin_obj = xclbin;
	int rc;

	info = fpga_image_info_alloc(&pdev->dev);
	if (!info)
		return -ENOMEM;

	info->buf = xclbin;
	info->count = xclbin_obj->m_header.m_length;
	info->flags |= FPGA_MGR_PARTIAL_RECONFIG;
	re->info = info;
	rc = fpga_region_program_fpga(re);
	if (rc) {
		xrt_err(pdev, "programming xclbin failed, rc %d", rc);
		return rc;
	}

	/* free bridges to allow reprogram */
	if (re->get_bridges)
		fpga_bridges_put(&re->bridge_list);

	/*
	 * Next bringup the subdevs for this region which will be managed by
	 * its own group object.
	 */
	r_data->grp_inst = xleaf_create_group(pdev, dtb);
	if (r_data->grp_inst < 0) {
		xrt_err(pdev, "failed to create group, rc %d",
			r_data->grp_inst);
		rc = r_data->grp_inst;
		return rc;
	}

	rc = xleaf_wait_for_group_bringup(pdev);
	if (rc)
		xrt_err(pdev, "group bringup failed, rc %d", rc);
	return rc;
}

static int xmgmt_get_bridges(struct fpga_region *re)
{
	struct xmgmt_region *r_data = re->priv;
	struct device *dev = &r_data->pdev->dev;

	return fpga_bridge_get_to_list(dev, re->info, &re->bridge_list);
}

/*
 * Program/create FPGA regions based on input xclbin file. This is key function
 * stitching the flow together:
 * 1. Identify a matching existing region for this xclbin
 * 2. Tear down any previous objects for the found region
 * 3. Program this region with input xclbin
 * 4. Iterate over this region's interface uuids to determine if it defines any
 *    child region. Create fpga_region for the child region.
 */
int xmgmt_process_xclbin(struct platform_device *pdev,
			 struct fpga_manager *fmgr,
			 const struct axlf *xclbin,
			 enum provider_kind kind)
{
	struct fpga_region *re, *compat_re = NULL;
	struct xmgmt_region_match_arg arg;
	struct xmgmt_region *r_data;
	char *dtb = NULL;
	int rc, i;

	rc = xrt_xclbin_get_metadata(DEV(pdev), xclbin, &dtb);
	if (rc) {
		xrt_err(pdev, "failed to get dtb: %d", rc);
		goto failed;
	}

	xrt_md_get_intf_uuids(DEV(pdev), dtb, &arg.uuid_num, NULL);
	if (arg.uuid_num == 0) {
		xrt_err(pdev, "failed to get intf uuid");
		rc = -EINVAL;
		goto failed;
	}
	arg.uuids = vzalloc(sizeof(uuid_t) * arg.uuid_num);
	if (!arg.uuids) {
		rc = -ENOMEM;
		goto failed;
	}
	arg.pdev = pdev;

	xrt_md_get_intf_uuids(DEV(pdev), dtb, &arg.uuid_num, arg.uuids);

	/* if this is not base firmware, search for a compatible region */
	if (kind != XMGMT_BLP) {
		compat_re = fpga_region_class_find(NULL, &arg,
						   xmgmt_region_match);
		if (!compat_re) {
			xrt_err(pdev, "failed to get compatible region");
			rc = -ENOENT;
			goto failed;
		}

		xmgmt_region_cleanup(compat_re);

		rc = xmgmt_region_program(compat_re, xclbin, dtb);
		if (rc) {
			xrt_err(pdev, "failed to program region");
			goto failed;
		}
	}

	/* create all the new regions contained in this xclbin */
	for (i = 0; i < arg.uuid_num; i++) {
		if (compat_re && !memcmp(compat_re->compat_id, &arg.uuids[i],
					 sizeof(*compat_re->compat_id)))
			/* region for this interface already exists */
			continue;
		re = fpga_region_create(DEV(pdev), fmgr, xmgmt_get_bridges);
		if (!re) {
			xrt_err(pdev, "failed to create fpga region");
			rc = -EFAULT;
			goto failed;
		}
		r_data = devm_kzalloc(DEV(pdev), sizeof(*r_data), GFP_KERNEL);
		if (!r_data) {
			rc = -ENOMEM;
			fpga_region_free(re);
			goto failed;
		}
		r_data->pdev = pdev;
		r_data->fregion = re;
		r_data->grp_inst = -1;
		memcpy(&r_data->intf_uuid, &arg.uuids[i],
		       sizeof(r_data->intf_uuid));
		if (compat_re) {
			memcpy(&r_data->dep_uuid, compat_re->compat_id,
			       sizeof(r_data->intf_uuid));
		}
		r_data->fbridge = xmgmt_create_bridge(pdev, dtb);
		if (!r_data->fbridge) {
			xrt_err(pdev, "failed to create fpga bridge");
			rc = -EFAULT;
			devm_kfree(DEV(pdev), r_data);
			fpga_region_free(re);
			goto failed;
		}

		re->compat_id = (struct fpga_compat_id *)&r_data->intf_uuid;
		re->priv = r_data;

		rc = fpga_region_register(re);
		if (rc) {
			xrt_err(pdev, "failed to register fpga region");
			xmgmt_destroy_bridge(r_data->fbridge);
			fpga_region_free(re);
			devm_kfree(DEV(pdev), r_data);
			goto failed;
		}

		xrt_info(pdev, "created fpga region %llx%llx",
			 re->compat_id->id_l, re->compat_id->id_h);
	}

failed:
	if (compat_re)
		put_device(&compat_re->dev);

	if (rc) {
		if (compat_re)
			xmgmt_region_cleanup(compat_re);
	}

	if (dtb)
		vfree(dtb);

	return rc;
}
