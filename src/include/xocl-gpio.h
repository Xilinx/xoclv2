/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XOCL_GPIO_H_
#define	_XOCL_GPIO_H_

#include "xocl-subdev.h"

/*
 * GPIO driver IOCTL calls.
 */
enum xocl_gpio_ioctl_cmd {
	XOCL_GPIO_READ = 0,
	XOCL_GPIO_WRITE,
};

enum xocl_gpio_id {
	XOCL_GPIO_ROM_UUID,
	XOCL_GPIO_DDR_CALIB,
	XOCL_GPIO_GOLDEN_VER,
	XOCL_GPIO_MAX
};

struct xocl_gpio_ioctl_rw {
	u32	xgir_id;
	void	*xgir_buf;
	u32	xgir_len;
	u32	xgir_offset;
};

struct xocl_gpio_ioctl_intf_uuid {
	u32	xgir_uuid_num;
	uuid_t	*xgir_uuids;
};

#endif	/* _XOCL_GPIO_H_ */
