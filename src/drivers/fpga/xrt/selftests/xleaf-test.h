/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_TEST_H_
#define _XRT_TEST_H_

#include "linux/xrt/ring.h"

enum xrt_test_ioc_cmds {
	XRT_TEST_IOC_REGISTER_RING,
	XRT_TEST_IOC_UNREGISTER_RING,
	XRT_TEST_IOC_SQ_WAKEUP,
};

#define XRT_TEST_IOC_MAGIC	'T'
#define XRT_TEST_REGISTER_RING		_IOW(XRT_TEST_IOC_MAGIC,	\
	XRT_TEST_IOC_REGISTER_RING, struct xrt_ioc_ring_register)
#define XRT_TEST_UNREGISTER_RING	_IOW(XRT_TEST_IOC_MAGIC,	\
	XRT_TEST_IOC_UNREGISTER_RING, struct xrt_ioc_ring_unregister)
#define XRT_TEST_SQ_WAKEUP		_IOW(XRT_TEST_IOC_MAGIC,	\
	XRT_TEST_IOC_SQ_WAKEUP, struct xrt_ioc_ring_sq_wakeup)

#endif /* _XRT_TEST_H_ */
