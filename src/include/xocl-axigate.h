/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XOCL_AXIGATE_H_
#define	_XOCL_AXIGATE_H_


#include "xocl-subdev.h"
#include "xocl-metadata.h"

/*
 * AXIGATE driver IOCTL calls.
 */
enum xocl_axigate_ioctl_cmd {
	XOCL_AXIGATE_FREEZE = 0,
	XOCL_AXIGATE_FREE,
};

/* the ep names are in the order of hardware layers */
static const char * const xocl_axigate_epnames[] = {
	NODE_GATE_PLP,
	NODE_GATE_ULP,
	NULL
};

static inline bool xocl_axigate_match_epname(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	char			*ep_name = arg;
	struct resource		*res;
	int			i;

	if (id != XOCL_SUBDEV_AXIGATE)
		return false;

	for (i = 0, res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	    res;
	    res = platform_get_resource(pdev, IORESOURCE_MEM, ++i)) {
		if (!strncmp(res->name, ep_name, strlen(res->name) + 1))
			return true;
	}

	return false;
}

#endif	/* _XOCL_AXIGATE_H_ */
