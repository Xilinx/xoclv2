// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2019 Xilinx, Inc. All rights reserved.
 * Bulk of the code borrowed from XRT mgmt driver fmgr.c
 *
 * Authors: Sonal.Santan@xilinx.com
 */

/*
 * FPGA Mgr integration is support limited to Ubuntu for now. RHEL/CentOS 7.X
 * kernels do not support FPGA Mgr yet.
 */

#include <linux/fpga/fpga-mgr.h>
#include <linux/platform_device.h>
#include <linux/module.h>

#include "alveo-drv.h"
#include "alveo-devices.h"
#include "xclbin.h"

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
//	struct xmgmt_dev *xdev;
	struct axlf *blob;
	char name[64];
	size_t count;
	enum fpga_mgr_states state;
};

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
	xmgmt_info(&mgr->dev, "Begin download of xclbin %pUb of length %lld B", &obj->blob->m_header.uuid,
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
	xmgmt_info(&mgr->dev, "Next block of %zu B of xclbin %pUb", count, &obj->blob->m_header.uuid);
	obj->state = FPGA_MGR_STATE_WRITE;
	return 0;
}


static int xmgmt_pr_write_complete(struct fpga_manager *mgr,
				  struct fpga_image_info *info)
{
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
	/* Send the xclbin blob to actual download framework in icap */
//	result = xmgmt_icap_download_axlf(obj->xdev, obj->blob);
	obj->state = result ? FPGA_MGR_STATE_WRITE_COMPLETE_ERR : FPGA_MGR_STATE_WRITE_COMPLETE;
	xmgmt_info(&mgr->dev, "Finish download of xclbin %pUb of size %zu B", &obj->blob->m_header.uuid, obj->count);
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

struct platform_device_id fmgr_id_table[] = {
	{ XOCL_DEVNAME(XOCL_FMGR), 0 },
	{ },
};

static int fmgr_probe(struct platform_device *pdev)
{
	struct fpga_manager *mgr;
	int ret = 0;
	void *part = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;

	struct xfpga_klass *obj = kzalloc(sizeof(struct xfpga_klass), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

//	obj->xdev = dev_get_platdata(&pdev->dev);
	snprintf(obj->name, sizeof(obj->name), "Xilinx PCIe FPGA Manager");
	obj->state = FPGA_MGR_STATE_UNKNOWN;
	mgr = fpga_mgr_create(&pdev->dev,
			      obj->name,
			      &xmgmt_pr_ops,
			      obj);
	xmgmt_info(&pdev->dev, "fmgr_probe 0x%p 0x%p\n", mgr, dev);
	if (!mgr)
		return -ENOMEM;

	/* Historically this was internally called by fpga_mgr_register (in the form
	 * of drv_set_drvdata) but is expected to be called here since Linux 4.18.
	 */
	platform_set_drvdata(pdev, mgr);

	ret = fpga_mgr_register(mgr);
	if (ret)
		fpga_mgr_free(mgr);

	return ret;
}

static int fmgr_remove(struct platform_device *pdev)
{
	struct fpga_manager *mgr = platform_get_drvdata(pdev);
	struct xfpga_klass *obj = mgr->priv;

	xmgmt_info(&pdev->dev, "fmgr_remove 0x%p 0x%p\n", mgr, &pdev->dev);
	obj->state = FPGA_MGR_STATE_UNKNOWN;
	/* TODO: Remove old fpga_mgr_unregister as soon as Linux < 4.18 is no
	 * longer supported.
	 */
	fpga_mgr_unregister(mgr);
	platform_set_drvdata(pdev, NULL);
	vfree(obj->blob);
	kfree(obj);
	return 0;
}

static struct platform_driver fmgr_driver = {
	.probe		= fmgr_probe,
	.remove		= fmgr_remove,
	.driver		= {
		.name = "alveo-fmgr",
	},
};

module_platform_driver(fmgr_driver);

MODULE_DESCRIPTION("FPGA Manager for Xilinx Alveo");
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:alveo-fmgr");
