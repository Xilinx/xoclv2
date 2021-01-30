/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XRT_GPIO_H_
#define	_XRT_GPIO_H_

#include "xleaf.h"

/*
 * GPIO driver IOCTL calls.
 */
enum xrt_gpio_ioctl_cmd {
	XRT_GPIO_READ = XRT_XLEAF_CUSTOM_BASE,
	XRT_GPIO_WRITE,
};

enum xrt_gpio_id {
	XRT_GPIO_ROM_UUID,
	XRT_GPIO_DDR_CALIB,
	XRT_GPIO_GOLDEN_VER,
	XRT_GPIO_MAX
};

struct xrt_gpio_ioctl_rw {
	u32	xgir_id;
	void	*xgir_buf;
	u32	xgir_len;
	u32	xgir_offset;
};

struct xrt_gpio_ioctl_intf_uuid {
	u32	xgir_uuid_num;
	uuid_t	*xgir_uuids;
};

#endif	/* _XRT_GPIO_H_ */
