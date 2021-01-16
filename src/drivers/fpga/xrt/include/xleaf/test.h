/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
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

struct xrt_xleaf_test_payload_in {
	uuid_t dummy1;
	char dummy2[16];
};

struct xrt_xleaf_test_payload_out {
	int dummy3;
	char dummy4[16];
};

union xrt_xleaf_test_payload {
	struct xrt_xleaf_test_payload_in in;
	struct xrt_xleaf_test_payload_out out;
};

#endif	/* _XRT_XLEAF_TEST_H_ */
