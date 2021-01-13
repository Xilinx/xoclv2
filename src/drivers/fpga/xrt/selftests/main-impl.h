/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XMGMT_MAIN_IMPL_H_
#define	_XMGMT_MAIN_IMPL_H_

#include <linux/platform_device.h>
#include "xmgmt-main.h"


extern int xmgmt_main_register_leaf(void);
extern void xmgmt_main_unregister_leaf(void);

#endif	/* _XMGMT_MAIN_IMPL_H_ */
