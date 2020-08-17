/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XOCL_ICAP_H_
#define	_XOCL_ICAP_H_

#include "xocl-subdev.h"

/*
 * ICAP driver IOCTL calls.
 */
enum xocl_icap_ioctl_cmd {
	XOCL_ICAP_WRITE = 0,
};

struct xocl_icap_ioctl_wr {
	void	*xiiw_bit_data;
	u32	xiiw_data_len;
};

#endif	/* _XOCL_ICAP_H_ */
