// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2021 Xilinx, Inc.
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

#include "xclbin-helper.h"
#include "xleaf.h"
#include "fmgr.h"
#include "xleaf/axigate.h"
#include "xleaf/icap.h"
#include "main-impl.h"

struct xfpga_klass {
	const struct platform_device *pdev;
	char                          name[64];
};

/*
 * xclbin download plumbing -- find the download subsystem, ICAP and
 * pass the xclbin for heavy lifting
 */
static int xmgmt_download_bitstream(struct platform_device *pdev,
				    const struct axlf *xclbin)

{
	struct hw_icap_bit_header bit_header = { 0 };
	struct platform_device *icap_leaf = NULL;
	struct xrt_icap_ioctl_wr arg;
	char *bitstream = NULL;
	int ret;

	ret = xrt_xclbin_get_section(xclbin, BITSTREAM, (void **)&bitstream, NULL);
	if (ret || !bitstream) {
		xrt_err(pdev, "bitstream not found");
		return -ENOENT;
	}
	ret = xrt_xclbin_parse_bitstream_header(bitstream,
						DMA_HWICAP_BITFILE_BUFFER_SIZE,
						&bit_header);
	if (ret) {
		ret = -EINVAL;
		xrt_err(pdev, "invalid bitstream header");
		goto done;
	}
	icap_leaf = xleaf_get_leaf_by_id(pdev, XRT_SUBDEV_ICAP, PLATFORM_DEVID_NONE);
	if (!icap_leaf) {
		ret = -ENODEV;
		xrt_err(pdev, "icap does not exist");
		goto done;
	}
	arg.xiiw_bit_data = bitstream + bit_header.header_length;
	arg.xiiw_data_len = bit_header.bitstream_length;
	ret = xleaf_ioctl(icap_leaf, XRT_ICAP_WRITE, &arg);
	if (ret)
		xrt_err(pdev, "write bitstream failed, ret = %d", ret);

done:
	if (icap_leaf)
		xleaf_put_leaf(pdev, icap_leaf);
	vfree(bitstream);

	return ret;
}

/*
 * There is no HW prep work we do here since we need the full
 * xclbin for its sanity check.
 */
static int xmgmt_pr_write_init(struct fpga_manager *mgr,
			       struct fpga_image_info *info,
			       const char *buf, size_t count)
{
	const struct axlf *bin = (const struct axlf *)buf;
	struct xfpga_klass *obj = mgr->priv;

	if (!(info->flags & FPGA_MGR_PARTIAL_RECONFIG)) {
		xrt_info(obj->pdev, "%s only supports partial reconfiguration\n", obj->name);
		return -EINVAL;
	}

	if (count < sizeof(struct axlf))
		return -EINVAL;

	if (count > bin->m_header.m_length)
		return -EINVAL;

	xrt_info(obj->pdev, "Prepare download of xclbin %pUb of length %lld B",
		 &bin->m_header.uuid, bin->m_header.m_length);

	return 0;
}

/*
 * The implementation requries full xclbin image before we can start
 * programming the hardware via ICAP subsystem. Full image is required
 * for checking the validity of xclbin and walking the sections to
 * discover the bitstream.
 */
static int xmgmt_pr_write(struct fpga_manager *mgr,
			  const char *buf, size_t count)
{
	const struct axlf *bin = (const struct axlf *)buf;
	struct xfpga_klass *obj = mgr->priv;

	if (bin->m_header.m_length != count)
		return -EINVAL;

	return xmgmt_download_bitstream((void *)obj->pdev, bin);
}

static int xmgmt_pr_write_complete(struct fpga_manager *mgr,
				   struct fpga_image_info *info)
{
	const struct axlf *bin = (const struct axlf *)info->buf;
	struct xfpga_klass *obj = mgr->priv;

	xrt_info(obj->pdev, "Finished download of xclbin %pUb",
		 &bin->m_header.uuid);
	return 0;
}

static enum fpga_mgr_states xmgmt_pr_state(struct fpga_manager *mgr)
{
	return FPGA_MGR_STATE_UNKNOWN;
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
	struct xfpga_klass *obj = devm_kzalloc(DEV(pdev), sizeof(struct xfpga_klass),
					       GFP_KERNEL);
	struct fpga_manager *fmgr = NULL;
	int ret = 0;

	if (!obj)
		return ERR_PTR(-ENOMEM);

	snprintf(obj->name, sizeof(obj->name), "Xilinx Alveo FPGA Manager");
	obj->pdev = pdev;
	fmgr = fpga_mgr_create(&pdev->dev,
			       obj->name,
			       &xmgmt_pr_ops,
			       obj);
	if (!fmgr)
		return ERR_PTR(-ENOMEM);

	ret = fpga_mgr_register(fmgr);
	if (ret) {
		fpga_mgr_free(fmgr);
		return ERR_PTR(ret);
	}
	return fmgr;
}

int xmgmt_fmgr_remove(struct fpga_manager *fmgr)
{
	fpga_mgr_unregister(fmgr);
	return 0;
}
