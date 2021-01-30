/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_SUBDEV_ID_H_
#define	_XRT_SUBDEV_ID_H_

/*
 * Every subdev driver should have an ID for others to refer to it.
 * There can be unlimited number of instances of a subdev driver. A
 * <subdev_id, subdev_instance> tuple should be a unique identification of
 * a specific instance of a subdev driver.
 * NOTE: PLEASE do not change the order of IDs. Sub devices in the same
 * group are initialized by this order.
 */
enum xrt_subdev_id {
	XRT_SUBDEV_GRP = 0,
	XRT_SUBDEV_VSEC,
	XRT_SUBDEV_VSEC_GOLDEN,
	XRT_SUBDEV_GPIO,
	XRT_SUBDEV_AXIGATE,
	XRT_SUBDEV_ICAP,
	XRT_SUBDEV_TEST,
	XRT_SUBDEV_MGMT_MAIN,
	XRT_SUBDEV_QSPI,
	XRT_SUBDEV_MAILBOX,
	XRT_SUBDEV_CMC,
	XRT_SUBDEV_CALIB,
	XRT_SUBDEV_CLKFREQ,
	XRT_SUBDEV_CLOCK,
	XRT_SUBDEV_SRSR,
	XRT_SUBDEV_UCS,
	XRT_SUBDEV_NUM,
	XRT_ROOT = -1, // Special ID for root driver
};

#endif	/* _XRT_SUBDEV_ID_H_ */
