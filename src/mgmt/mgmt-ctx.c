// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2019 Xilinx, Inc. All rights reserved.
 *
 * Authors: Sonal.Santan@xilinx.com
 */

#include <linux/mutex.h>

#include "mgmt-drv.h"

/*
 * helper functions to protect driver private data
 */
DEFINE_MUTEX(xrt_drvinst_lock);
struct xrt_drvinst *xrt_drvinst_array[XMGMT_MAX_DEVICES];

void *xrt_drvinst_alloc(struct device *dev, u32 size)
{
	struct xrt_drvinst	*drvinstp = NULL;
	int	        	inst;

	mutex_lock(&xrt_drvinst_lock);
	for (inst = 0; inst < ARRAY_SIZE(xrt_drvinst_array); inst++)
		if (!xrt_drvinst_array[inst])
			break;

	if (inst == ARRAY_SIZE(xrt_drvinst_array))
		goto failed;

	drvinstp = devm_kzalloc(dev, size + sizeof(struct xrt_drvinst), GFP_KERNEL);
	if (!drvinstp)
		goto failed;

	drvinstp->dev = dev;
	drvinstp->size = size;
	atomic_set(&drvinstp->ref, 1);
	xrt_drvinst_array[inst] = drvinstp;

	mutex_unlock(&xrt_drvinst_lock);
	return drvinstp->data;

failed:
	mutex_unlock(&xrt_drvinst_lock);
	return NULL;
}

void xrt_drvinst_free(void *data)
{
	struct xrt_drvinst	*drvinstp;
	struct xrt_drvinst_proc *proc, *temp;
	struct pid		*p;
	int      		inst;

	mutex_lock(&xrt_drvinst_lock);
	drvinstp = container_of(data, struct xrt_drvinst, data);
	for (inst = 0; inst < ARRAY_SIZE(xrt_drvinst_array); inst++) {
		if (drvinstp == xrt_drvinst_array[inst])
			break;
	}

	/* it must be created before */
	BUG_ON(inst == ARRAY_SIZE(xrt_drvinst_array));

	xrt_drvinst_array[inst] = NULL;
	mutex_unlock(&xrt_drvinst_lock);

#if 0
	/* wait all opened instances to close */
	if (atomic_read(&drvinstp->ref) > 1) {
		xrt_info(drvinstp->dev, "Wait for close %p\n",
				&drvinstp->comp);
	}
#endif
}
