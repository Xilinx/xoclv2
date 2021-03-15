// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA Region Support for Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
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
#include "xmgnt.h"

struct xmgmt_bridge {
	struct platform_device *pdev;
	const char *bridge_name;
};

struct xmgmt_region {
	struct platform_device *pdev;
	struct fpga_region *region;
	struct fpga_compat_id compat_id;
	uuid_t intf_uuid;
	struct fpga_bridge *bridge;
	int group_instance;
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

	axigate_leaf = xleaf_get_leaf_by_epname(br_data->pdev, br_data->bridge_name);
	if (!axigate_leaf) {
		xrt_err(br_data->pdev, "failed to get leaf %s",
			br_data->bridge_name);
		return -ENOENT;
	}

	if (enable)
		rc = xleaf_call(axigate_leaf, XRT_AXIGATE_OPEN, NULL);
	else
		rc = xleaf_call(axigate_leaf, XRT_AXIGATE_CLOSE, NULL);

	if (rc) {
		xrt_err(br_data->pdev, "failed to %s gate %s, rc %d",
			(enable ? "free" : "freeze"), br_data->bridge_name,
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

	xrt_info(br_data->pdev, "destroy fpga bridge %s", br_data->bridge_name);
	fpga_bridge_unregister(br);

	devm_kfree(DEV(br_data->pdev), br_data);

	fpga_bridge_free(br);
}

static struct fpga_bridge *xmgmt_create_bridge(struct platform_device *pdev,
					       char *dtb)
{
	struct fpga_bridge *br = NULL;
	struct xmgmt_bridge *br_data;
	const char *gate;
	int rc;

	br_data = devm_kzalloc(DEV(pdev), sizeof(*br_data), GFP_KERNEL);
	if (!br_data)
		return NULL;
	br_data->pdev = pdev;

	br_data->bridge_name = XRT_MD_NODE_GATE_ULP;
	rc = xrt_md_find_endpoint(&pdev->dev, dtb, XRT_MD_NODE_GATE_ULP,
				  NULL, &gate);
	if (rc) {
		br_data->bridge_name = XRT_MD_NODE_GATE_PLP;
		rc = xrt_md_find_endpoint(&pdev->dev, dtb, XRT_MD_NODE_GATE_PLP,
					  NULL, &gate);
	}
	if (rc) {
		xrt_err(pdev, "failed to get axigate, rc %d", rc);
		goto failed;
	}

	br = fpga_bridge_create(DEV(pdev), br_data->bridge_name,
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

	xrt_info(pdev, "created fpga bridge %s", br_data->bridge_name);

	return br;

failed:
	if (br)
		fpga_bridge_free(br);
	if (br_data)
		devm_kfree(DEV(pdev), br_data);

	return NULL;
}

static void xmgmt_destroy_region(struct fpga_region *region)
{
	struct xmgmt_region *r_data = region->priv;

	xrt_info(r_data->pdev, "destroy fpga region %llx.%llx",
		 region->compat_id->id_l, region->compat_id->id_h);

	fpga_region_unregister(region);

	if (r_data->group_instance > 0)
		xleaf_destroy_group(r_data->pdev, r_data->group_instance);

	if (r_data->bridge)
		xmgmt_destroy_bridge(r_data->bridge);

	if (r_data->region->info) {
		fpga_image_info_free(r_data->region->info);
		r_data->region->info = NULL;
	}

	fpga_region_free(region);

	devm_kfree(DEV(r_data->pdev), r_data);
}

static int xmgmt_region_match(struct device *dev, const void *data)
{
	const struct xmgmt_region_match_arg *arg = data;
	const struct fpga_region *match_region;
	uuid_t compat_uuid;
	int i;

	if (dev->parent != &arg->pdev->dev)
		return false;

	match_region = to_fpga_region(dev);
	/*
	 * The device tree provides both parent and child uuids for an
	 * xclbin in one array. Here we try both uuids to see if it matches
	 * with target region's compat_id. Strictly speaking we should
	 * only match xclbin's parent uuid with target region's compat_id
	 * but given the uuids by design are unique comparing with both
	 * does not hurt.
	 */
	import_uuid(&compat_uuid, (const char *)match_region->compat_id);
	for (i = 0; i < arg->uuid_num; i++) {
		if (uuid_equal(&compat_uuid, &arg->uuids[i]))
			return true;
	}

	return false;
}

static int xmgmt_region_match_base(struct device *dev, const void *data)
{
	const struct xmgmt_region_match_arg *arg = data;
	const struct fpga_region *match_region;
	const struct xmgmt_region *r_data;

	if (dev->parent != &arg->pdev->dev)
		return false;

	match_region = to_fpga_region(dev);
	r_data = match_region->priv;
	if (uuid_is_null(&r_data->dep_uuid))
		return true;

	return false;
}

static int xmgmt_region_match_by_uuid(struct device *dev, const void *data)
{
	const struct xmgmt_region_match_arg *arg = data;
	const struct fpga_region *match_region;
	const struct xmgmt_region *r_data;

	if (dev->parent != &arg->pdev->dev)
		return false;

	if (arg->uuid_num != 1)
		return false;

	match_region = to_fpga_region(dev);
	r_data = match_region->priv;
	if (uuid_equal(&r_data->dep_uuid, arg->uuids))
		return true;

	return false;
}

static void xmgmt_region_cleanup(struct fpga_region *region)
{
	struct xmgmt_region *r_data = region->priv, *pdata, *temp;
	struct platform_device *pdev = r_data->pdev;
	struct xmgmt_region_match_arg arg = { 0 };
	struct fpga_region *match_region = NULL;
	struct device *start_dev = NULL;
	LIST_HEAD(free_list);
	uuid_t compat_uuid;

	list_add_tail(&r_data->list, &free_list);
	arg.pdev = pdev;
	arg.uuid_num = 1;
	arg.uuids = &compat_uuid;

	/* find all regions depending on this region */
	list_for_each_entry_safe(pdata, temp, &free_list, list) {
		import_uuid(arg.uuids, (const char *)pdata->region->compat_id);
		start_dev = NULL;
		while ((match_region = fpga_region_class_find(start_dev, &arg,
							      xmgmt_region_match_by_uuid))) {
			pdata = match_region->priv;
			list_add_tail(&pdata->list, &free_list);
			start_dev = &match_region->dev;
			put_device(&match_region->dev);
		}
	}

	list_del(&r_data->list);

	list_for_each_entry_safe_reverse(pdata, temp, &free_list, list)
		xmgmt_destroy_region(pdata->region);

	if (r_data->group_instance > 0) {
		xleaf_destroy_group(pdev, r_data->group_instance);
		r_data->group_instance = -1;
	}
	if (r_data->region->info) {
		fpga_image_info_free(r_data->region->info);
		r_data->region->info = NULL;
	}
}

void xmgmt_region_cleanup_all(struct platform_device *pdev)
{
	struct xmgmt_region_match_arg arg = { 0 };
	struct fpga_region *base_region;

	arg.pdev = pdev;

	while ((base_region = fpga_region_class_find(NULL, &arg, xmgmt_region_match_base))) {
		put_device(&base_region->dev);

		xmgmt_region_cleanup(base_region);
		xmgmt_destroy_region(base_region);
	}
}

/*
 * Program a region with a xclbin image. Bring up the subdevs and the
 * group object to contain the subdevs.
 */
static int xmgmt_region_program(struct fpga_region *region, const void *xclbin, char *dtb)
{
	const struct axlf *xclbin_obj = xclbin;
	struct fpga_image_info *info;
	struct platform_device *pdev;
	struct xmgmt_region *r_data;
	int rc;

	r_data = region->priv;
	pdev = r_data->pdev;

	info = fpga_image_info_alloc(&pdev->dev);
	if (!info)
		return -ENOMEM;

	info->buf = xclbin;
	info->count = xclbin_obj->header.length;
	info->flags |= FPGA_MGR_PARTIAL_RECONFIG;
	region->info = info;
	rc = fpga_region_program_fpga(region);
	if (rc) {
		xrt_err(pdev, "programming xclbin failed, rc %d", rc);
		return rc;
	}

	/* free bridges to allow reprogram */
	if (region->get_bridges)
		fpga_bridges_put(&region->bridge_list);

	/*
	 * Next bringup the subdevs for this region which will be managed by
	 * its own group object.
	 */
	r_data->group_instance = xleaf_create_group(pdev, dtb);
	if (r_data->group_instance < 0) {
		xrt_err(pdev, "failed to create group, rc %d",
			r_data->group_instance);
		rc = r_data->group_instance;
		return rc;
	}

	rc = xleaf_wait_for_group_bringup(pdev);
	if (rc)
		xrt_err(pdev, "group bringup failed, rc %d", rc);
	return rc;
}

static int xmgmt_get_bridges(struct fpga_region *region)
{
	struct xmgmt_region *r_data = region->priv;
	struct device *dev = &r_data->pdev->dev;

	return fpga_bridge_get_to_list(dev, region->info, &region->bridge_list);
}

/*
 * Program/create FPGA regions based on input xclbin file.
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
	struct fpga_region *region, *compat_region = NULL;
	struct xmgmt_region_match_arg arg = { 0 };
	struct xmgmt_region *r_data;
	uuid_t compat_uuid;
	char *dtb = NULL;
	int rc, i;

	rc = xrt_xclbin_get_metadata(DEV(pdev), xclbin, &dtb);
	if (rc) {
		xrt_err(pdev, "failed to get dtb: %d", rc);
		goto failed;
	}

	rc = xrt_md_get_interface_uuids(DEV(pdev), dtb, 0, NULL);
	if (rc < 0) {
		xrt_err(pdev, "failed to get intf uuid");
		rc = -EINVAL;
		goto failed;
	}
	arg.uuid_num = rc;
	arg.uuids = vzalloc(sizeof(uuid_t) * arg.uuid_num);
	if (!arg.uuids) {
		rc = -ENOMEM;
		goto failed;
	}
	arg.pdev = pdev;

	rc = xrt_md_get_interface_uuids(DEV(pdev), dtb, arg.uuid_num, arg.uuids);
	if (rc != arg.uuid_num) {
		xrt_err(pdev, "only get %d uuids, expect %d", rc, arg.uuid_num);
		rc = -EINVAL;
		goto failed;
	}

	/* if this is not base firmware, search for a compatible region */
	if (kind != XMGMT_BLP) {
		compat_region = fpga_region_class_find(NULL, &arg, xmgmt_region_match);
		if (!compat_region) {
			xrt_err(pdev, "failed to get compatible region");
			rc = -ENOENT;
			goto failed;
		}

		xmgmt_region_cleanup(compat_region);

		rc = xmgmt_region_program(compat_region, xclbin, dtb);
		if (rc) {
			xrt_err(pdev, "failed to program region");
			goto failed;
		}
	}

	if (compat_region)
		import_uuid(&compat_uuid, (const char *)compat_region->compat_id);

	/* create all the new regions contained in this xclbin */
	for (i = 0; i < arg.uuid_num; i++) {
		if (compat_region && uuid_equal(&compat_uuid, &arg.uuids[i])) {
			/* region for this interface already exists */
			continue;
		}

		region = fpga_region_create(DEV(pdev), fmgr, xmgmt_get_bridges);
		if (!region) {
			xrt_err(pdev, "failed to create fpga region");
			rc = -EFAULT;
			goto failed;
		}
		r_data = devm_kzalloc(DEV(pdev), sizeof(*r_data), GFP_KERNEL);
		if (!r_data) {
			rc = -ENOMEM;
			fpga_region_free(region);
			goto failed;
		}
		r_data->pdev = pdev;
		r_data->region = region;
		r_data->group_instance = -1;
		uuid_copy(&r_data->intf_uuid, &arg.uuids[i]);
		if (compat_region)
			import_uuid(&r_data->dep_uuid, (const char *)compat_region->compat_id);
		r_data->bridge = xmgmt_create_bridge(pdev, dtb);
		if (!r_data->bridge) {
			xrt_err(pdev, "failed to create fpga bridge");
			rc = -EFAULT;
			devm_kfree(DEV(pdev), r_data);
			fpga_region_free(region);
			goto failed;
		}

		region->compat_id = &r_data->compat_id;
		export_uuid((char *)region->compat_id, &r_data->intf_uuid);
		region->priv = r_data;

		rc = fpga_region_register(region);
		if (rc) {
			xrt_err(pdev, "failed to register fpga region");
			xmgmt_destroy_bridge(r_data->bridge);
			fpga_region_free(region);
			devm_kfree(DEV(pdev), r_data);
			goto failed;
		}

		xrt_info(pdev, "created fpga region %llx%llx",
			 region->compat_id->id_l, region->compat_id->id_h);
	}

	if (compat_region)
		put_device(&compat_region->dev);
	vfree(dtb);
	return 0;

failed:
	if (compat_region) {
		put_device(&compat_region->dev);
		xmgmt_region_cleanup(compat_region);
	} else {
		xmgmt_region_cleanup_all(pdev);
	}

	vfree(dtb);
	return rc;
}
