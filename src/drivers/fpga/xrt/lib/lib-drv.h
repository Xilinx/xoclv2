/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _LIB_DRV_H_
#define _LIB_DRV_H_

const char *xrt_drv_name(enum xrt_subdev_id id);
int xrt_drv_get_instance(enum xrt_subdev_id id);
void xrt_drv_put_instance(enum xrt_subdev_id id, int instance);
struct xrt_subdev_endpoints *xrt_drv_get_endpoints(enum xrt_subdev_id id);

#endif	/* _LIB_DRV_H_ */
