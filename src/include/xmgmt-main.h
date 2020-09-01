/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XMGMT_MAIN_H_
#define	_XMGMT_MAIN_H_

#include "uapi/xclbin.h"

enum xocl_mgmt_main_ioctl_cmd {
	XOCL_MGMT_MAIN_GET_XSABIN_SECTION = 0,
	XOCL_MGMT_MAIN_GET_VBNV, // vbnv needs to be kfree'd by caller
};

struct xocl_mgmt_main_ioctl_get_xsabin_section {
	enum axlf_section_kind xmmigxs_section_kind;
	void *xmmigxs_section;
	u64 xmmigxs_section_size;
};

#endif	/* _XMGMT_MAIN_H_ */
