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

extern struct platform_driver xmgmt_main_driver;
extern struct xocl_subdev_endpoints xocl_mgmt_main_endpoints[];

extern int xmgmt_impl_ulp_download(struct platform_device *pdev, void *xclbin);
#endif	/* _XMGMT_MAIN_IMPL_H_ */
