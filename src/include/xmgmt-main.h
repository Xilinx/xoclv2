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

enum xrt_mgmt_main_ioctl_cmd {
	XOCL_MGMT_MAIN_GET_XSABIN_SECTION = 0,
	XOCL_MGMT_MAIN_GET_VBNV, // vbnv needs to be kfree'd by caller
	XOCL_MGMT_MAIN_GET_ULP_SECTION,
	XOCL_MGMT_MAIN_PEER_TEST_MSG,
};

struct xrt_mgmt_main_ioctl_get_axlf_section {
	enum axlf_section_kind xmmigas_section_kind;
	void *xmmigas_section;
	u64 xmmigas_section_size;
};

struct xrt_mgmt_main_peer_test_msg {
	bool xmmpgtm_set;
	char *xmmpgtm_buf;
	size_t xmmpgtm_len;
};

#endif	/* _XMGMT_MAIN_H_ */
