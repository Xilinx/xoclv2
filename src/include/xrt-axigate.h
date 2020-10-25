/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XRT_AXIGATE_H_
#define	_XRT_AXIGATE_H_


#include "xrt-subdev.h"
#include "xrt-metadata.h"

/*
 * AXIGATE driver IOCTL calls.
 */
enum xrt_axigate_ioctl_cmd {
	XRT_AXIGATE_FREEZE = 0,
	XRT_AXIGATE_FREE,
};

/* the ep names are in the order of hardware layers */
static const char * const xrt_axigate_epnames[] = {
	NODE_GATE_PLP,
	NODE_GATE_ULP,
	NULL
};

#endif	/* _XRT_AXIGATE_H_ */
