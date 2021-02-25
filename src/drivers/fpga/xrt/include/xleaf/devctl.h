/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header file for XRT DEVCTL Leaf Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XRT_DEVCTL_H_
#define _XRT_DEVCTL_H_

#include "xleaf.h"

/*
 * DEVCTL driver IOCTL calls.
 */
enum xrt_devctl_ioctl_cmd {
	XRT_DEVCTL_READ = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
	XRT_DEVCTL_WRITE,
};

enum xrt_devctl_id {
	XRT_DEVCTL_ROM_UUID,
	XRT_DEVCTL_DDR_CALIB,
	XRT_DEVCTL_GOLDEN_VER,
	XRT_DEVCTL_MAX
};

struct xrt_devctl_ioctl_rw {
	u32	xgir_id;
	void	*xgir_buf;
	u32	xgir_len;
	u32	xgir_offset;
};

struct xrt_devctl_ioctl_intf_uuid {
	u32	xgir_uuid_num;
	uuid_t	*xgir_uuids;
};

#endif	/* _XRT_DEVCTL_H_ */
