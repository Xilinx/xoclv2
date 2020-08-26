/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XOCL_CLKFREQ_H_
#define	_XOCL_CLKFREQ_H_

#include "xocl-subdev.h"

/*
 * CLKFREQ driver IOCTL calls.
 */
enum xocl_clkfreq_ioctl_cmd {
	XOCL_CLKFREQ_READ = 0,
};

static inline bool xocl_clkfreq_match_epname(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	char *ep_name = arg;
	struct resource *res;

	if (id != XOCL_SUBDEV_CLKFREQ)
		return false;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!strncmp(res->name, ep_name, strlen(res->name) + 1))
		return true;

	return false;
}

#endif	/* _XOCL_CLKFREQ_H_ */
