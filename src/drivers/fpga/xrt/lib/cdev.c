// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA device node helper functions.
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include "xleaf.h"

extern struct class *xrt_class;

#define	XRT_CDEV_DIR		"xfpga"
#define	INODE2PDATA(inode)	\
	container_of((inode)->i_cdev, struct xrt_subdev_platdata, xsp_cdev)
#define	INODE2PDEV(inode)	\
	to_platform_device(kobj_to_dev((inode)->i_cdev->kobj.parent))
#define	CDEV_NAME(sysdev)	(strchr((sysdev)->kobj.name, '!') + 1)

/* Allow it to be accessed from cdev. */
static void xleaf_devnode_allowed(struct platform_device *pdev)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(pdev);

	/* Allow new opens. */
	mutex_lock(&pdata->xsp_devnode_lock);
	pdata->xsp_devnode_online = true;
	mutex_unlock(&pdata->xsp_devnode_lock);
}

/* Turn off access from cdev and wait for all existing user to go away. */
static int xleaf_devnode_disallowed(struct platform_device *pdev)
{
	int ret = 0;
	struct xrt_subdev_platdata *pdata = DEV_PDATA(pdev);

	mutex_lock(&pdata->xsp_devnode_lock);

	/* Prevent new opens. */
	pdata->xsp_devnode_online = false;
	/* Wait for existing user to close. */
	while (!ret && pdata->xsp_devnode_ref) {
		int rc;

		mutex_unlock(&pdata->xsp_devnode_lock);
		rc = wait_for_completion_killable(&pdata->xsp_devnode_comp);
		mutex_lock(&pdata->xsp_devnode_lock);

		if (rc == -ERESTARTSYS) {
			/* Restore online state. */
			pdata->xsp_devnode_online = true;
			xrt_err(pdev, "%s is in use, ref=%d",
				CDEV_NAME(pdata->xsp_sysdev),
				pdata->xsp_devnode_ref);
			ret = -EBUSY;
		}
	}

	mutex_unlock(&pdata->xsp_devnode_lock);

	return ret;
}

static struct platform_device *
__xleaf_devnode_open(struct inode *inode, bool excl)
{
	struct xrt_subdev_platdata *pdata = INODE2PDATA(inode);
	struct platform_device *pdev = INODE2PDEV(inode);
	bool opened = false;

	mutex_lock(&pdata->xsp_devnode_lock);

	if (pdata->xsp_devnode_online) {
		if (excl && pdata->xsp_devnode_ref) {
			xrt_err(pdev, "%s has already been opened exclusively",
				CDEV_NAME(pdata->xsp_sysdev));
		} else if (!excl && pdata->xsp_devnode_excl) {
			xrt_err(pdev, "%s has been opened exclusively",
				CDEV_NAME(pdata->xsp_sysdev));
		} else {
			pdata->xsp_devnode_ref++;
			pdata->xsp_devnode_excl = excl;
			opened = true;
			xrt_info(pdev, "opened %s, ref=%d",
				 CDEV_NAME(pdata->xsp_sysdev),
				 pdata->xsp_devnode_ref);
		}
	} else {
		xrt_err(pdev, "%s is offline", CDEV_NAME(pdata->xsp_sysdev));
	}

	mutex_unlock(&pdata->xsp_devnode_lock);

	return opened ? pdev : NULL;
}

struct platform_device *
xleaf_devnode_open_excl(struct inode *inode)
{
	return __xleaf_devnode_open(inode, true);
}

struct platform_device *
xleaf_devnode_open(struct inode *inode)
{
	return __xleaf_devnode_open(inode, false);
}
EXPORT_SYMBOL_GPL(xleaf_devnode_open);

void xleaf_devnode_close(struct inode *inode)
{
	struct xrt_subdev_platdata *pdata = INODE2PDATA(inode);
	struct platform_device *pdev = INODE2PDEV(inode);
	bool notify = false;

	mutex_lock(&pdata->xsp_devnode_lock);

	pdata->xsp_devnode_ref--;
	if (pdata->xsp_devnode_ref == 0) {
		pdata->xsp_devnode_excl = false;
		notify = true;
	}
	if (notify) {
		xrt_info(pdev, "closed %s, ref=%d",
			 CDEV_NAME(pdata->xsp_sysdev), pdata->xsp_devnode_ref);
	} else {
		xrt_info(pdev, "closed %s, notifying waiter",
			 CDEV_NAME(pdata->xsp_sysdev));
	}

	mutex_unlock(&pdata->xsp_devnode_lock);

	if (notify)
		complete(&pdata->xsp_devnode_comp);
}
EXPORT_SYMBOL_GPL(xleaf_devnode_close);

static inline enum xrt_subdev_file_mode
devnode_mode(struct xrt_subdev_drvdata *drvdata)
{
	return drvdata->xsd_file_ops.xsf_mode;
}

int xleaf_devnode_create(struct platform_device *pdev, const char *file_name,
			 const char *inst_name)
{
	struct xrt_subdev_drvdata *drvdata = DEV_DRVDATA(pdev);
	struct xrt_subdev_file_ops *fops = &drvdata->xsd_file_ops;
	struct xrt_subdev_platdata *pdata = DEV_PDATA(pdev);
	struct cdev *cdevp;
	struct device *sysdev;
	int ret = 0;
	char fname[256];

	mutex_init(&pdata->xsp_devnode_lock);
	init_completion(&pdata->xsp_devnode_comp);

	cdevp = &DEV_PDATA(pdev)->xsp_cdev;
	cdev_init(cdevp, &fops->xsf_ops);
	cdevp->owner = fops->xsf_ops.owner;
	cdevp->dev = MKDEV(MAJOR(fops->xsf_dev_t), pdev->id);

	/*
	 * Set pdev as parent of cdev so that when pdev (and its platform
	 * data) will not be freed when cdev is not freed.
	 */
	cdev_set_parent(cdevp, &DEV(pdev)->kobj);

	ret = cdev_add(cdevp, cdevp->dev, 1);
	if (ret) {
		xrt_err(pdev, "failed to add cdev: %d", ret);
		goto failed;
	}
	if (!file_name)
		file_name = pdev->name;
	if (!inst_name) {
		if (devnode_mode(drvdata) == XRT_SUBDEV_FILE_MULTI_INST) {
			snprintf(fname, sizeof(fname), "%s/%s/%s.%u",
				 XRT_CDEV_DIR, DEV_PDATA(pdev)->xsp_root_name,
				 file_name, pdev->id);
		} else {
			snprintf(fname, sizeof(fname), "%s/%s/%s",
				 XRT_CDEV_DIR, DEV_PDATA(pdev)->xsp_root_name,
				 file_name);
		}
	} else {
		snprintf(fname, sizeof(fname), "%s/%s/%s.%s", XRT_CDEV_DIR,
			 DEV_PDATA(pdev)->xsp_root_name, file_name, inst_name);
	}
	sysdev = device_create(xrt_class, NULL, cdevp->dev, NULL, "%s", fname);
	if (IS_ERR(sysdev)) {
		ret = PTR_ERR(sysdev);
		xrt_err(pdev, "failed to create device node: %d", ret);
		goto failed;
	}
	pdata->xsp_sysdev = sysdev;

	xleaf_devnode_allowed(pdev);

	xrt_info(pdev, "created (%d, %d): /dev/%s",
		 MAJOR(cdevp->dev), pdev->id, fname);
	return 0;

failed:
	device_destroy(xrt_class, cdevp->dev);
	cdev_del(cdevp);
	cdevp->owner = NULL;
	return ret;
}

int xleaf_devnode_destroy(struct platform_device *pdev)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(pdev);
	struct cdev *cdevp = &pdata->xsp_cdev;
	dev_t dev = cdevp->dev;
	int rc;

	rc = xleaf_devnode_disallowed(pdev);
	if (rc)
		return rc;

	xrt_info(pdev, "removed (%d, %d): /dev/%s/%s", MAJOR(dev), MINOR(dev),
		 XRT_CDEV_DIR, CDEV_NAME(pdata->xsp_sysdev));
	device_destroy(xrt_class, cdevp->dev);
	pdata->xsp_sysdev = NULL;
	cdev_del(cdevp);
	return 0;
}
