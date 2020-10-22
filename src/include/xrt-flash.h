/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_FLASH_H_
#define	_XOCL_FLASH_H_

#include "xrt-subdev.h"

/*
 * Flash controller driver IOCTL calls.
 */
enum xrt_flash_ioctl_cmd {
	XOCL_FLASH_GET_SIZE = 0,
	XOCL_FLASH_READ,
};

struct xrt_flash_ioctl_read {
	char *xfir_buf;
	size_t xfir_size;
	loff_t xfir_offset;
};

#endif	/* _XOCL_FLASH_H_ */
