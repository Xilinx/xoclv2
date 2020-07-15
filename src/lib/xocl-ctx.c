// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA device node helper functions.
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#include "xocl-subdev.h"

static void xocl_devnode_init(struct platform_device *pdev)
{
	struct xocl_subdev_platdata *pdata = DEV_PDATA(pdev);

	if (pdata->xsp_pdev)
		return;

	pdata->xsp_pdev = pdev;
	mutex_init(&pdata->xsp_devnode_lock);
	init_completion(&pdata->xsp_devnode_comp);
}

/* Allow it to be accessed from cdev. */
void xocl_devnode_allowed(struct platform_device *pdev)
{
	struct xocl_subdev_platdata *pdata = DEV_PDATA(pdev);

	xocl_devnode_init(pdev);

	/* Allow new opens. */
	mutex_lock(&pdata->xsp_devnode_lock);
	pdata->xsp_devnode_online = true;
	mutex_unlock(&pdata->xsp_devnode_lock);
}

/* Turn off access from cdev and wait for all existing user to go away. */
int xocl_devnode_disallowed(struct platform_device *pdev)
{
	struct xocl_subdev_platdata *pdata = DEV_PDATA(pdev);

	/* Prevent new opens. */
	mutex_lock(&pdata->xsp_devnode_lock);
	pdata->xsp_devnode_online = false;
	mutex_unlock(&pdata->xsp_devnode_lock);

	/* Wait for existing user to close. */
	while (pdata->xsp_devnode_ref) {
		int ret = wait_for_completion_killable(
			&pdata->xsp_devnode_comp);
		if (ret == -ERESTARTSYS) {
			/* Restore online state. */
			mutex_lock(&pdata->xsp_devnode_lock);
			pdata->xsp_devnode_online = true;
			mutex_unlock(&pdata->xsp_devnode_lock);

			xocl_err(pdev, "driver is in use, ref=%d",
				pdata->xsp_devnode_ref);
			return -EBUSY;
		}
	}
	return 0;
}

static struct platform_device *
__xocl_devnode_open(struct inode *inode, bool excl)
{
	struct xocl_subdev_platdata *pdata = container_of(inode->i_cdev,
		struct xocl_subdev_platdata, xsp_cdev);
	struct platform_device *pdev = pdata->xsp_pdev;
	bool opened = false;

	mutex_lock(&pdata->xsp_devnode_lock);

	if (pdata->xsp_devnode_online) {
		if (excl && pdata->xsp_devnode_ref) {
			xocl_err(pdev, "dev is already opened exclusively");
		} else if (!excl && pdata->xsp_devnode_excl) {
			xocl_err(pdev, "dev is opened exclusively");
		} else {
			pdata->xsp_devnode_ref++;
			pdata->xsp_devnode_excl = excl;
			opened = true;
			xocl_info(pdev, "dev is successfully opened, ref=%d",
				pdata->xsp_devnode_ref);
		}
	} else {
		xocl_err(pdev, "dev is offline");
	}

	mutex_unlock(&pdata->xsp_devnode_lock);

	return opened ? pdev : NULL;
}

struct platform_device *
xocl_devnode_open_excl(struct inode *inode)
{
	return __xocl_devnode_open(inode, true);
}

struct platform_device *
xocl_devnode_open(struct inode *inode)
{
	return __xocl_devnode_open(inode, false);
}

void xocl_devnode_close(struct inode *inode)
{
	struct xocl_subdev_platdata *pdata = container_of(inode->i_cdev,
		struct xocl_subdev_platdata, xsp_cdev);
	struct platform_device *pdev = pdata->xsp_pdev;
	bool notify = false;

	mutex_lock(&pdata->xsp_devnode_lock);

	pdata->xsp_devnode_ref--;
	if (pdata->xsp_devnode_ref == 0) {
		pdata->xsp_devnode_excl = false;
		notify = true;
	}
	xocl_info(pdev, "dev is successfully closed%s, ref=%d",
		notify ? ", notifying waiter" : "", pdata->xsp_devnode_ref);

	mutex_unlock(&pdata->xsp_devnode_lock);

	if (notify)
		complete(&pdata->xsp_devnode_comp);
}
