/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XRT_UCS_H_
#define _XRT_UCS_H_

#include "xleaf.h"

/*
 * UCS driver leaf calls.
 */
enum xrt_ucs_leaf_cmd {
	XRT_UCS_CHECK = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
	XRT_UCS_ENABLE,
};

#endif	/* _XRT_UCS_H_ */
