/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_TEST1_MAIN_IMPL_H_
#define	_TEST1_MAIN_IMPL_H_

#include <linux/platform_device.h>
#include "xmgmt-main.h"


extern int test1_main_register_leaf(void);
extern void test1_main_unregister_leaf(void);

#endif	/* _TEST1_MAIN_IMPL_H_ */
