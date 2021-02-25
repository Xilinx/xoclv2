/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header file for Xilinx Runtime (XRT) driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_SUBDEV_ID_H_
#define _XRT_SUBDEV_ID_H_

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
	XRT_SUBDEV_VSEC = 1,
	XRT_SUBDEV_VSEC_GOLDEN = 2,
	XRT_SUBDEV_DEVCTL = 3,
	XRT_SUBDEV_AXIGATE = 4,
	XRT_SUBDEV_ICAP = 5,
	XRT_SUBDEV_TEST = 6,
	XRT_SUBDEV_MGMT_MAIN = 7,
	XRT_SUBDEV_QSPI = 8,
	XRT_SUBDEV_MAILBOX = 9,
	XRT_SUBDEV_CMC = 10,
	XRT_SUBDEV_CALIB = 11,
	XRT_SUBDEV_CLKFREQ = 12,
	XRT_SUBDEV_CLOCK = 13,
	XRT_SUBDEV_SRSR = 14,
	XRT_SUBDEV_UCS = 15,
	XRT_SUBDEV_NUM = 16, /* Total number of subdevs. */
	XRT_ROOT = -1, /* Special ID for root driver. */
};

#endif	/* _XRT_SUBDEV_ID_H_ */
