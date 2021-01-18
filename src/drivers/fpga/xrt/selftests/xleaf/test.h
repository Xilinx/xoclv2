/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Sonal Santan <sonal.santan@xilinx.com>
 */

#ifndef	_XRT_XLEAF_TEST_H_
#define	_XRT_XLEAF_TEST_H_

#include "xleaf.h"

/*
 * XLEAF TEST driver IOCTL calls.
 */
enum xrt_xleaf_test_ioctl_cmd {
	XRT_XLEAF_TEST_A = XRT_XLEAF_CUSTOM_BASE,
	XRT_XLEAF_TEST_B,
};

struct xrt_xleaf_test_payload {
	uuid_t dummy1;
	char dummy2[16];
};


int selftest_test_register_leaf(void);
void selftest_test_unregister_leaf(void);

#endif	/* _XRT_XLEAF_TEST_H_ */
