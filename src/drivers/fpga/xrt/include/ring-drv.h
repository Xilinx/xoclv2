/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors: Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_RING_DRV_H
#define _XRT_RING_DRV_H

#include <linux/device.h>
#include "linux/xrt/ring.h"

typedef void (*xrt_ring_req_handler)(void *arg, struct xrt_ring_entry *req,
	size_t reqsz);
extern void *xrt_ring_probe(struct device *dev, size_t max_num_rings);
extern void xrt_ring_remove(void *handle);
extern int xrt_ring_register(void *handle, struct xrt_ioc_ring_register *reg,
	xrt_ring_req_handler handler, void *handler_arg);
extern int xrt_ring_unregister(void *handle,
	struct xrt_ioc_ring_unregister *unreg);
extern struct xrt_ring_entry *xrt_ring_cq_produce_begin(void *handle,
	u64 ring_hdl, size_t *sz);
extern void xrt_ring_cq_produce_end(void *handle, u64 ring_hdl);
extern int xrt_ring_sq_wakeup(void *handle,
	struct xrt_ioc_ring_sq_wakeup *wakeup);

#endif /* _XRT_RING_DRV_H */
