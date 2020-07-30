/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_MAIN_H_
#define	_XOCL_MAIN_H_

extern struct platform_driver xocl_partition_driver;
extern struct platform_driver xocl_test_driver;
extern struct platform_driver xocl_vsec_driver;

extern struct xocl_subdev_endpoints xocl_vsec_endpoints;

extern const char *xocl_drv_name(enum xocl_subdev_id id);
extern int xocl_drv_get_instance(enum xocl_subdev_id id);
extern void xocl_drv_put_instance(enum xocl_subdev_id id, int instance);
extern struct xocl_subdev_endpoints *xocl_drv_get_endpoints(enum xocl_subdev_id id);

#endif	/* _XOCL_PARTITION_H_ */
