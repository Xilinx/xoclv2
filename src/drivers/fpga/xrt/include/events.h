/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_EVENTS_H_
#define	_XRT_EVENTS_H_

#include <linux/platform_device.h>
#include "subdev_id.h"

/*
 * Event notification.
 */
enum xrt_events {
	XRT_EVENT_TEST = 0, // for testing
	/*
	 * Events related to specific subdev
	 * Callback arg: struct xrt_event_arg_subdev
	 */
	XRT_EVENT_POST_CREATION,
	XRT_EVENT_PRE_REMOVAL,
	/*
	 * Events related to change of the whole board
	 * Callback arg: <none>
	 */
	XRT_EVENT_PRE_HOT_RESET,
	XRT_EVENT_POST_HOT_RESET,
	XRT_EVENT_PRE_GATE_CLOSE,
	XRT_EVENT_POST_GATE_OPEN,
	XRT_EVENT_POST_ATTACH,
	XRT_EVENT_PRE_DETACH,
};

typedef int (*xrt_event_cb_t)(struct platform_device *pdev,
	enum xrt_events evt, void *arg);
typedef void (*xrt_async_broadcast_event_cb_t)(struct platform_device *pdev,
	enum xrt_events evt, void *arg, bool success);

struct xrt_event_arg_subdev {
	enum xrt_subdev_id xevt_subdev_id;
	int xevt_subdev_instance;
};

/*
 * Flags in return value from event callback.
 */
/* Done with event handling, continue waiting for the next one */
#define	XRT_EVENT_CB_CONTINUE	0x0
/* Done with event handling, stop waiting for the next one */
#define	XRT_EVENT_CB_STOP	0x1
/* Error processing event */
#define	XRT_EVENT_CB_ERR	0x2

#endif	/* _XRT_EVENTS_H_ */
