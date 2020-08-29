// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2019-2020 Xilinx, Inc.
 * Bulk of the code borrowed from XRT mgmt driver file, fmgr.c
 *
 * Authors: Sonal.Santan@xilinx.com
 */

#include <linux/cred.h>
#include <linux/efi.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

#include "xocl-subdev.h"
#include "xmgmt-fmgr.h"
#include "xocl-axigate.h"
/*
 * Container to capture and cache full xclbin as it is passed in blocks by FPGA
 * Manager. xocl needs access to full xclbin to walk through xclbin sections. FPGA
 * Manager's .write() backend sends incremental blocks without any knowledge of
 * xclbin format forcing us to collect the blocks and stitch them together here.
 * TODO:
 * 1. Add a variant of API, icap_download_bitstream_axlf() which works off kernel buffer
 * 2. Call this new API from FPGA Manager's write complete hook, xmgmt_pr_write_complete()
 */

struct xfpga_klass {
	const struct platform_device *pdev;
	struct axlf         *blob;
	char                 name[64];
	size_t               count;
	struct mutex         axlf_lock;
	int                  reader_ref;
	enum fpga_mgr_states state;
	enum xfpga_sec_level sec_level;
};

struct key *xfpga_keys;

static int xmgmt_pr_write_init(struct fpga_manager *mgr,
			       struct fpga_image_info *info, const char *buf, size_t count)
{
	struct xfpga_klass *obj = mgr->priv;
	const struct axlf *bin = (const struct axlf *)buf;

	if (count < sizeof(struct axlf)) {
		obj->state = FPGA_MGR_STATE_WRITE_INIT_ERR;
		return -EINVAL;
	}

	if (count > bin->m_header.m_length) {
		obj->state = FPGA_MGR_STATE_WRITE_INIT_ERR;
		return -EINVAL;
	}

	/* Free up the previous blob */
	vfree(obj->blob);
	obj->blob = vmalloc(bin->m_header.m_length);
	if (!obj->blob) {
		obj->state = FPGA_MGR_STATE_WRITE_INIT_ERR;
		return -ENOMEM;
	}

	memcpy(obj->blob, buf, count);
	xocl_info(obj->pdev, "Begin download of xclbin %pUb of length %lld B", &obj->blob->m_header.uuid,
		  obj->blob->m_header.m_length);
	obj->count = count;
	obj->state = FPGA_MGR_STATE_WRITE_INIT;
	return 0;
}

static int xmgmt_pr_write(struct fpga_manager *mgr,
			 const char *buf, size_t count)
{
	struct xfpga_klass *obj = mgr->priv;
	char *curr = (char *)obj->blob;

	if ((obj->state != FPGA_MGR_STATE_WRITE_INIT) && (obj->state != FPGA_MGR_STATE_WRITE)) {
		obj->state = FPGA_MGR_STATE_WRITE_ERR;
		return -EINVAL;
	}

	curr += obj->count;
	obj->count += count;
	/* Check if the xclbin buffer is not longer than advertised in the header */
	if (obj->blob->m_header.m_length < obj->count) {
		obj->state = FPGA_MGR_STATE_WRITE_ERR;
		return -EINVAL;
	}
	memcpy(curr, buf, count);
	xocl_info(obj->pdev, "Next block of %zu B of xclbin %pUb", count, &obj->blob->m_header.uuid);
	obj->state = FPGA_MGR_STATE_WRITE;
	return 0;
}


static int xmgmt_pr_write_complete(struct fpga_manager *mgr,
				   struct fpga_image_info *info)
{
	struct platform_device *axigate_leaf;
	int result = 0;
	struct xfpga_klass *obj = mgr->priv;

	if (obj->state != FPGA_MGR_STATE_WRITE) {
		obj->state = FPGA_MGR_STATE_WRITE_COMPLETE_ERR;
		return -EINVAL;
	}

	/* Check if we got the complete xclbin */
	if (obj->blob->m_header.m_length != obj->count) {
		obj->state = FPGA_MGR_STATE_WRITE_COMPLETE_ERR;
		return -EINVAL;
	}

	axigate_leaf = xocl_subdev_get_leaf((struct platform_device *)obj->pdev, xocl_subdev_match_epname, NODE_GATE_ULP);
	if (axigate_leaf == NULL) {
		xocl_err(obj->pdev, "failed to hold axi gate leaf");
		return -ENODEV;
	}

	(void) xocl_subdev_ioctl(axigate_leaf, XOCL_AXIGATE_FREEZE, NULL);
	/* TODO: Plumb calls into icap, cw, mig */
	(void) xocl_subdev_ioctl(axigate_leaf, XOCL_AXIGATE_FREE, NULL);

	obj->state = result ? FPGA_MGR_STATE_WRITE_COMPLETE_ERR : FPGA_MGR_STATE_WRITE_COMPLETE;
	xocl_info(obj->pdev, "Finish download of xclbin %pUb of size %zu B", &obj->blob->m_header.uuid, obj->count);
	(void) xocl_subdev_put_leaf((struct platform_device *)obj->pdev, axigate_leaf);
	vfree(obj->blob);
	obj->blob = NULL;
	obj->count = 0;
	return result;
}

static enum fpga_mgr_states xmgmt_pr_state(struct fpga_manager *mgr)
{
	struct xfpga_klass *obj = mgr->priv;

	return obj->state;
}

static const struct fpga_manager_ops xmgmt_pr_ops = {
	.initial_header_size = sizeof(struct axlf),
	.write_init = xmgmt_pr_write_init,
	.write = xmgmt_pr_write,
	.write_complete = xmgmt_pr_write_complete,
	.state = xmgmt_pr_state,
};


struct fpga_manager *xmgmt_fmgr_probe(struct platform_device *pdev)
{
	struct fpga_manager *fmgr;
	int ret = 0;
	struct xfpga_klass *obj = vzalloc(sizeof(struct xfpga_klass));

	xocl_info(pdev, "probing...");
	if (!obj)
		return ERR_PTR(-ENOMEM);

	snprintf(obj->name, sizeof(obj->name), "Xilinx Alveo FPGA Manager");
	obj->state = FPGA_MGR_STATE_UNKNOWN;
	obj->pdev = pdev;
	fmgr = fpga_mgr_create(&pdev->dev,
			       obj->name,
			       &xmgmt_pr_ops,
			       obj);
	if (!fmgr)
		return ERR_PTR(-ENOMEM);

	obj->sec_level = XFPGA_SEC_NONE;
	ret = fpga_mgr_register(fmgr);
	if (ret) {
		fpga_mgr_free(fmgr);
		kfree(obj);
		return ERR_PTR(ret);
	}
	mutex_init(&obj->axlf_lock);
	return fmgr;
}

int xmgmt_fmgr_remove(struct fpga_manager *fmgr)
{
	struct xfpga_klass *obj = fmgr->priv;

	mutex_destroy(&obj->axlf_lock);
	obj->state = FPGA_MGR_STATE_UNKNOWN;
	fpga_mgr_unregister(fmgr);
	vfree(obj->blob);
	vfree(obj);
	return 0;
}
