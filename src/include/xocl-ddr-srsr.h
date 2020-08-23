/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XOCL_DDR_SRSR_H_
#define _XOCL_DDR_SRSR_H_

#include "xocl-subdev.h"

/*
 * ddr-srsr driver IOCTL calls.
 */
enum xocl_ddr_srsr_ioctl_cmd {
	XOCL_DDR_SRSR_SAVE,
	XOCL_DDR_SRSR_CALIB,
	XOCL_DDR_SRSR_WRITE,
	XOCL_DDR_SRSR_READ,
	XOCL_DDR_SRSR_SIZE,
};

struct xocl_srsr_ioctl_rw {
	void	*xdirw_buf;
	u32	xdirw_size;
};

static inline bool xocl_srsr_match_idx(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	u32 idx = (u32)(uintptr_t)arg;
	char ep_name[64];
	struct resource *res;
	int i;

	if (id != XOCL_SUBDEV_SRSR)
		return false;

	snprintf(ep_name, sizeof(ep_name), "%s_%d", NODE_DDR_SRSR, idx);
	for (i = 0, res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	    res;
	    res = platform_get_resource(pdev, IORESOURCE_MEM, ++i)) {
		if (!strncmp(res->name, ep_name, strlen(res->name) + 1))
			return true;
	}

	return false;
}

#endif
