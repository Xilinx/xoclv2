/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XRT_ICAP_H_
#define	_XRT_ICAP_H_

#include "xleaf.h"

/*
 * ICAP driver IOCTL calls.
 */
enum xrt_icap_ioctl_cmd {
	XRT_ICAP_WRITE = XRT_XLEAF_CUSTOM_BASE,
	XRT_ICAP_IDCODE,
};

struct xrt_icap_ioctl_wr {
	void	*xiiw_bit_data;
	u32	xiiw_data_len;
};

#endif	/* _XRT_ICAP_H_ */
