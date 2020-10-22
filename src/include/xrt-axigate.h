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

#endif	/* _XOCL_AXIGATE_H_ */
