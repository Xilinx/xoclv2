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
	XOCL_GPIO_UUID,
	XOCL_GPIO_MAX
};

struct xocl_gpio_ioctl_rw {
	u32	xgir_id;
	void	*xgir_buf;
	u32	xgir_len;
	u32	xgir_offset;
};

bool xocl_gpio_match_epname(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg);
#endif	/* _XOCL_GPIO_H_ */
