/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_CMC_H_
#define _XRT_CMC_H_

#include "xleaf.h"
#include <linux/xrt/xclbin.h>

/*
 * CMC driver leaf calls.
 */
enum xrt_cmc_leaf_cmd {
	XRT_CMC_READ_BOARD_INFO = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
	XRT_CMC_READ_SENSORS,
};

#endif	/* _XRT_CMC_H_ */
