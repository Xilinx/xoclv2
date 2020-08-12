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

static inline bool xocl_gpio_match_epname(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	char			*ep_name = arg;
	struct resource		*res;
	int			i;

	if (id != XOCL_SUBDEV_GPIO)
		return false;

	for (i = 0, res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	    res;
	    res = platform_get_resource(pdev, IORESOURCE_MEM, ++i)) {
		if (!strncmp(res->name, ep_name, strlen(res->name) + 1))
			return true;
	}

	return false;
}

#endif	/* _XOCL_GPIO_H_ */
