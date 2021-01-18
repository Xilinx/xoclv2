/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Sonal Santan <sonal.santan@xilinx.com>
 */

#ifndef	_SELFTEST1_MAIN_IMPL_H_
#define	_SELFTEST1_MAIN_IMPL_H_

#include <linux/platform_device.h>
#include "xmgmt-main.h"


extern int selftest1_main_register_leaf(void);
extern void selftest1_main_unregister_leaf(void);

#endif	/* _SELFTEST1_MAIN_IMPL_H_ */
