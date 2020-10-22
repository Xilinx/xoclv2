/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XOCL_UCS_H_
#define	_XOCL_UCS_H_

#include "xocl-subdev.h"

/*
 * UCS driver IOCTL calls.
 */
enum xocl_ucs_ioctl_cmd {
	XOCL_UCS_CHECK = 0,
	XOCL_UCS_ENABLE,
};

#endif	/* _XOCL_UCS_H_ */
