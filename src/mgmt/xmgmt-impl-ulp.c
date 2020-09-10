// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA MGMT PF entry point driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * xclbin download
 *
 * Authors:
 *      Lizhi Hou <lizhi.hou@xilinx.com>
 */

#include <linux/firmware.h>
#include <linux/uaccess.h>
#include "xocl-xclbin.h"
#include "xocl-metadata.h"
#include "xocl-subdev.h"
#include "xocl-gpio.h"
#include "xmgmt-main.h"
#include "xocl-icap.h"
#include "xocl-axigate.h"

static int xmgmt_impl_download_bitstream(struct platform_device  *pdev,
	void *xclbin)
{
	struct platform_device *icap_leaf;
	struct XHwIcap_Bit_Header bit_header = { 0 };
	struct xocl_icap_ioctl_wr arg;
	char *bitstream;
	int ret;

	ret = xrt_xclbin_get_section(xclbin, BITSTREAM, (void **)&bitstream,
		NULL);
	if (ret || !bitstream) {
		xocl_err(pdev, "bitstream not found");
		return -ENOENT;
	}
	ret = xrt_xclbin_parse_header(bitstream,
		DMA_HWICAP_BITFILE_BUFFER_SIZE, &bit_header);
	if (ret) {
		xocl_err(pdev, "invalid bitstream header");
		return -EINVAL;
	}
	icap_leaf = xocl_subdev_get_leaf_by_id(pdev, XOCL_SUBDEV_ICAP,
		PLATFORM_DEVID_NONE);
	if (!icap_leaf) {
		xocl_err(pdev, "icap does not exist");
		return -ENODEV;
	}
	arg.xiiw_bit_data = bitstream + bit_header.HeaderLength;
	arg.xiiw_data_len = bit_header.BitstreamLength;
	ret = xocl_subdev_ioctl(icap_leaf, XOCL_ICAP_WRITE, &arg);
	if (ret)
		xocl_err(pdev, "write bitstream failed, ret = %d", ret);

	xocl_subdev_put_leaf(pdev, icap_leaf);

	return ret;
}

int xmgmt_impl_ulp_download(struct platform_device  *pdev, void *xclbin)
{
	struct platform_device *axigate_leaf;
	char *dtb = NULL;
	int ret = 0;

	ret = xrt_xclbin_get_metadata(DEV(pdev), xclbin, &dtb);
	if (ret) {
		xocl_err(pdev, "can not get partition metadata, ret %d", ret);
		goto failed;
	}

	axigate_leaf = xocl_subdev_get_leaf(pdev, xocl_subdev_match_epname,
		NODE_GATE_ULP);

	/* gate may not be exist for 0rp */
	if (axigate_leaf) {
		ret = xocl_subdev_ioctl(axigate_leaf, XOCL_AXIGATE_FREEZE,
			NULL);
	}
	ret = xmgmt_impl_download_bitstream(pdev, xclbin);
	if (axigate_leaf) {
		xocl_subdev_ioctl(axigate_leaf, XOCL_AXIGATE_FREE, NULL);

		/* Do we really need this extra toggling gate before setting
		 * clocks?
		 * xocl_subdev_ioctl(axigate_leaf, XOCL_AXIGATE_FREEZE, NULL);
		 * xocl_subdev_ioctl(axigate_leaf, XOCL_AXIGATE_FREE, NULL);
		 */

		xocl_subdev_put_leaf(pdev, axigate_leaf);
	}
	if (ret) {
		xocl_err(pdev, "bitstream download failed, ret %d", ret);
		goto failed;
	}
	ret = xocl_subdev_create_partition(pdev, dtb);
	if (ret < 0)
		xocl_err(pdev, "failed creating partition, ret %d", ret);

failed:
	return ret;
}
