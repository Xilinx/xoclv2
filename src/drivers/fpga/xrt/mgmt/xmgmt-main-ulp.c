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
#include "xrt-xclbin.h"
#include "xrt-metadata.h"
#include "xrt-subdev.h"
#include "xrt-gpio.h"
#include "xmgmt-main.h"
#include "xrt-icap.h"
#include "xrt-axigate.h"

static int xmgmt_download_bitstream(struct platform_device  *pdev,
	const void *xclbin)
{
	struct platform_device *icap_leaf = NULL;
	struct XHwIcap_Bit_Header bit_header = { 0 };
	struct xrt_icap_ioctl_wr arg;
	char *bitstream = NULL;
	int ret;

	ret = xrt_xclbin_get_section(xclbin, BITSTREAM, (void **)&bitstream,
		NULL);
	if (ret || !bitstream) {
		xrt_err(pdev, "bitstream not found");
		return -ENOENT;
	}
	ret = xrt_xclbin_parse_header(bitstream,
		DMA_HWICAP_BITFILE_BUFFER_SIZE, &bit_header);
	if (ret) {
		ret = -EINVAL;
		xrt_err(pdev, "invalid bitstream header");
		goto done;
	}
	icap_leaf = xrt_subdev_get_leaf_by_id(pdev, XRT_SUBDEV_ICAP,
		PLATFORM_DEVID_NONE);
	if (!icap_leaf) {
		ret = -ENODEV;
		xrt_err(pdev, "icap does not exist");
		goto done;
	}
	arg.xiiw_bit_data = bitstream + bit_header.HeaderLength;
	arg.xiiw_data_len = bit_header.BitstreamLength;
	ret = xrt_subdev_ioctl(icap_leaf, XRT_ICAP_WRITE, &arg);
	if (ret)
		xrt_err(pdev, "write bitstream failed, ret = %d", ret);

done:
	if (icap_leaf)
		xrt_subdev_put_leaf(pdev, icap_leaf);
	vfree(bitstream);

	return ret;
}

static bool match_shell(enum xrt_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(pdev);
	const char *ulp_gate;
	int ret;

	if (!pdata || xrt_md_size(&pdev->dev, pdata->xsp_dtb) <= 0)
		return false;

	ret = xrt_md_get_epname_pointer(&pdev->dev, pdata->xsp_dtb,
		NODE_GATE_ULP, NULL, &ulp_gate);
	if (ret)
		return false;

	ret = xrt_md_check_uuids(&pdev->dev, pdata->xsp_dtb, arg);
	if (ret)
		return false;

	return true;
}

static bool match_ulp(enum xrt_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(pdev);
	const char *ulp_gate;
	int ret;

	if (!pdata || xrt_md_size(&pdev->dev, pdata->xsp_dtb) <= 0)
		return false;

	ret = xrt_md_check_uuids(&pdev->dev, pdata->xsp_dtb, arg);
	if (ret)
		return false;

	ret = xrt_md_get_epname_pointer(&pdev->dev, pdata->xsp_dtb,
		NODE_GATE_ULP, NULL, &ulp_gate);
	if (!ret)
		return false;

	return true;
}

int xmgmt_ulp_download(struct platform_device  *pdev, const void *xclbin)
{
	struct platform_device *axigate_leaf;
	char *dtb = NULL;
	int ret = 0, part_inst;

	ret = xrt_xclbin_get_metadata(DEV(pdev), xclbin, &dtb);
	if (ret) {
		xrt_err(pdev, "can not get partition metadata, ret %d", ret);
		goto failed;
	}

	part_inst = xrt_subdev_lookup_partition(pdev, match_shell, dtb);
	if (part_inst < 0) {
		xrt_err(pdev, "not found matching plp.");
		ret = -ENODEV;
		goto failed;
	}

	/*
	 * Find ulp partition with interface uuid from incoming xclbin, which
	 * is verified before with matching plp partition.
	 */
	part_inst = xrt_subdev_lookup_partition(pdev, match_ulp, dtb);
	if (part_inst >= 0) {
		ret = xrt_subdev_destroy_partition(pdev, part_inst);
		if (ret) {
			xrt_err(pdev, "failed to destroy existing ulp, %d",
				ret);
			goto failed;
		}
	}

	axigate_leaf = xrt_subdev_get_leaf_by_epname(pdev, NODE_GATE_ULP);

	/* gate may not be exist for 0rp */
	if (axigate_leaf) {
		ret = xrt_subdev_ioctl(axigate_leaf, XRT_AXIGATE_FREEZE,
			NULL);
		if (ret) {
			xrt_err(pdev, "can not freeze gate %s, %d",
				NODE_GATE_ULP, ret);
			xrt_subdev_put_leaf(pdev, axigate_leaf);
			goto failed;
		}
	}
	ret = xmgmt_download_bitstream(pdev, xclbin);
	if (axigate_leaf) {
		xrt_subdev_ioctl(axigate_leaf, XRT_AXIGATE_FREE, NULL);

		/* Do we really need this extra toggling gate before setting
		 * clocks?
		 * xrt_subdev_ioctl(axigate_leaf, XRT_AXIGATE_FREEZE, NULL);
		 * xrt_subdev_ioctl(axigate_leaf, XRT_AXIGATE_FREE, NULL);
		 */

		xrt_subdev_put_leaf(pdev, axigate_leaf);
	}
	if (ret) {
		xrt_err(pdev, "bitstream download failed, ret %d", ret);
		goto failed;
	}
	ret = xrt_subdev_create_partition(pdev, dtb);
	if (ret < 0) {
		xrt_err(pdev, "failed creating partition, ret %d", ret);
		goto failed;
	}

	ret = xrt_subdev_wait_for_partition_bringup(pdev);
	if (ret)
		xrt_err(pdev, "partiton bringup failed, ret %d", ret);

	/*
	 * TODO: needs to check individual subdevs to see if there
	 * is any error, such as clock setting, memory bank calibration.
	 */

failed:
	vfree(dtb);
	return ret;
}
