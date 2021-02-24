/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header file for Xilinx Runtime (XRT) driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XMGMT_MAIN_H_
#define _XMGMT_MAIN_H_

#include <linux/xrt/xclbin.h>
#include "xleaf.h"

enum xrt_mgmt_main_ioctl_cmd {
	/* section needs to be vfree'd by caller */
	XRT_MGMT_MAIN_GET_AXLF_SECTION = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
	/* vbnv needs to be kfree'd by caller */
	XRT_MGMT_MAIN_GET_VBNV,
};

enum provider_kind {
	XMGMT_BLP,
	XMGMT_PLP,
	XMGMT_ULP,
};

struct xrt_mgmt_main_ioctl_get_axlf_section {
	enum provider_kind xmmigas_axlf_kind;
	enum axlf_section_kind xmmigas_section_kind;
	void *xmmigas_section;
	u64 xmmigas_section_size;
};

#endif	/* _XMGMT_MAIN_H_ */
