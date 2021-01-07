/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_SUBDEV_POOL_H_
#define	_XRT_SUBDEV_POOL_H_

#include "xroot.h"
#include "events.h"

/*
 * It manages a list of xrt_subdevs for root and partition drivers.
 */
struct xrt_subdev_pool {
	struct list_head xpool_dev_list;
	struct device *xpool_owner;
	struct mutex xpool_lock;
	bool xpool_closing;
};

/*
 * Subdev pool API for root and partition drivers only.
 */
extern void xrt_subdev_pool_init(struct device *dev,
	struct xrt_subdev_pool *spool);
extern int xrt_subdev_pool_fini(struct xrt_subdev_pool *spool);
extern int xrt_subdev_pool_get(struct xrt_subdev_pool *spool,
	xrt_subdev_match_t match, void *arg, struct device *holder_dev,
	struct platform_device **pdevp);
extern int xrt_subdev_pool_put(struct xrt_subdev_pool *spool,
	struct platform_device *pdev, struct device *holder_dev);
extern int xrt_subdev_pool_add(struct xrt_subdev_pool *spool,
	enum xrt_subdev_id id, xrt_subdev_root_cb_t pcb,
	void *pcb_arg, char *dtb);
extern int xrt_subdev_pool_del(struct xrt_subdev_pool *spool,
	enum xrt_subdev_id id, int instance);
extern ssize_t xrt_subdev_pool_get_holders(struct xrt_subdev_pool *spool,
	struct platform_device *pdev, char *buf, size_t len);

extern int xrt_subdev_pool_trigger_event(struct xrt_subdev_pool *spool,
	enum xrt_events evt);
extern int xrt_subdev_pool_handle_event(struct xrt_subdev_pool *spool,
	struct xrt_event *evt);

#endif	/* _XRT_SUBDEV_POOL_H_ */
