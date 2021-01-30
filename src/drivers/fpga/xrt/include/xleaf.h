/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *    Cheng Zhen <maxz@xilinx.com>
 *    Sonal Santan <sonal.santan@xilinx.com>
 */

#ifndef	_XRT_XLEAF_H_
#define	_XRT_XLEAF_H_

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/libfdt_env.h>
#include "libfdt.h"
#include "subdev_id.h"
#include "xroot.h"
#include "events.h"

/* All subdev drivers should use below common routines to print out msg. */
#define	DEV(pdev)	(&(pdev)->dev)
#define	DEV_PDATA(pdev)					\
	((struct xrt_subdev_platdata *)dev_get_platdata(DEV(pdev)))
#define	DEV_DRVDATA(pdev)				\
	((struct xrt_subdev_drvdata *)			\
	platform_get_device_id(pdev)->driver_data)
#define	FMT_PRT(prt_fn, pdev, fmt, args...)		\
	({typeof(pdev) (_pdev) = (pdev);		\
	prt_fn(DEV(_pdev), "%s %s: " fmt,		\
	DEV_PDATA(_pdev)->xsp_root_name, __func__, ##args); })
#define xrt_err(pdev, fmt, args...) FMT_PRT(dev_err, pdev, fmt, ##args)
#define xrt_warn(pdev, fmt, args...) FMT_PRT(dev_warn, pdev, fmt, ##args)
#define xrt_info(pdev, fmt, args...) FMT_PRT(dev_info, pdev, fmt, ##args)
#define xrt_dbg(pdev, fmt, args...) FMT_PRT(dev_dbg, pdev, fmt, ##args)

/*
 * Common IOCTLs implemented by all leafs.
 */
enum xrt_xleaf_ioctl_cmd {
	XRT_XLEAF_BASE = 0,
	XRT_XLEAF_EVENT = XRT_XLEAF_BASE,

	/* Below cmds are leaf specific ones. */
	XRT_XLEAF_CUSTOM_BASE = 64,
};

/*
 * If populated by subdev driver, infra will handle the mechanics of
 * char device (un)registration.
 */
enum xrt_subdev_file_mode {
	// Infra create cdev, default file name
	XRT_SUBDEV_FILE_DEFAULT = 0,
	// Infra create cdev, need to encode inst num in file name
	XRT_SUBDEV_FILE_MULTI_INST,
	// No auto creation of cdev by infra, leaf handles it by itself
	XRT_SUBDEV_FILE_NO_AUTO,
};

struct xrt_subdev_file_ops {
	const struct file_operations xsf_ops;
	dev_t xsf_dev_t;
	const char *xsf_dev_name;
	enum xrt_subdev_file_mode xsf_mode;
};

/*
 * Subdev driver callbacks populated by subdev driver.
 */
struct xrt_subdev_drv_ops {
	/*
	 * Per driver module callback. Don't take any arguments.
	 * If defined these are called as part of driver (un)registration.
	 */
	int (*xsd_post_init)(void);
	void (*xsd_pre_exit)(void);

	/*
	 * Per driver instance callback. The pdev points to the instance.
	 * If defined these are called by other leaf drivers.
	 * Note that root driver may call into xsd_ioctl of a group driver.
	 */
	int (*xsd_ioctl)(struct platform_device *pdev, u32 cmd, void *arg);
};

/*
 * Defined and populated by subdev driver, exported as driver_data in
 * struct platform_device_id.
 */
struct xrt_subdev_drvdata {
	struct xrt_subdev_file_ops xsd_file_ops;
	struct xrt_subdev_drv_ops xsd_dev_ops;
};

/*
 * Partially initialized by parent driver, then, passed in as subdev driver's
 * platform data when creating subdev driver instance by calling platform
 * device register API (platform_device_register_data() or the likes).
 *
 * Once device register API returns, platform driver framework makes a copy of
 * this buffer and maintains its life cycle. The content of the buffer is
 * completely owned by subdev driver.
 *
 * Thus, parent driver should be very careful when it touches this buffer
 * again once it's handed over to subdev driver. And the data structure
 * should not contain pointers pointing to buffers that is managed by
 * other or parent drivers since it could have been freed before platform
 * data buffer is freed by platform driver framework.
 */
struct xrt_subdev_platdata {
	/*
	 * Per driver instance callback. The pdev points to the instance.
	 * Should always be defined for subdev driver to get service from root.
	 */
	xrt_subdev_root_cb_t xsp_root_cb;
	void *xsp_root_cb_arg;

	/* Something to associate w/ root for msg printing. */
	const char *xsp_root_name;

	/*
	 * Char dev support for this subdev instance.
	 * Initialized by subdev driver.
	 */
	struct cdev xsp_cdev;
	struct device *xsp_sysdev;
	struct mutex xsp_devnode_lock; /* devnode lock */
	struct completion xsp_devnode_comp;
	int xsp_devnode_ref;
	bool xsp_devnode_online;
	bool xsp_devnode_excl;

	/*
	 * Subdev driver specific init data. The buffer should be embedded
	 * in this data structure buffer after dtb, so that it can be freed
	 * together with platform data.
	 */
	loff_t xsp_priv_off; /* Offset into this platform data buffer. */
	size_t xsp_priv_len;

	/*
	 * Populated by parent driver to describe the device tree for
	 * the subdev driver to handle. Should always be last one since it's
	 * of variable length.
	 */
	char xsp_dtb[sizeof(struct fdt_header)];
};

/*
 * this struct define the endpoints belong to the same subdevice
 */
struct xrt_subdev_ep_names {
	const char *ep_name;
	const char *regmap_name;
};

struct xrt_subdev_endpoints {
	struct xrt_subdev_ep_names *xse_names;
	/* minimum number of endpoints to support the subdevice */
	u32 xse_min_ep;
};

struct subdev_match_arg {
	enum xrt_subdev_id id;
	int instance;
};

bool xleaf_has_epname(struct platform_device *pdev, const char *nm);
struct platform_device *xleaf_get_leaf(struct platform_device *pdev,
				       xrt_subdev_match_t cb, void *arg);

static inline bool subdev_match(enum xrt_subdev_id id,
				struct platform_device *pdev, void *arg)
{
	const struct subdev_match_arg *a = (struct subdev_match_arg *)arg;
	return id == a->id &&
		(pdev->id == a->instance || PLATFORM_DEVID_NONE == a->instance);
}

static inline bool xrt_subdev_match_epname(enum xrt_subdev_id id,
					   struct platform_device *pdev, void *arg)
{
	return xleaf_has_epname(pdev, arg);
}

static inline struct platform_device *
xleaf_get_leaf_by_id(struct platform_device *pdev,
		     enum xrt_subdev_id id, int instance)
{
	struct subdev_match_arg arg = { id, instance };

	return xleaf_get_leaf(pdev, subdev_match, &arg);
}

static inline struct platform_device *
xleaf_get_leaf_by_epname(struct platform_device *pdev, const char *name)
{
	return xleaf_get_leaf(pdev, xrt_subdev_match_epname, (void *)name);
}

static inline int xleaf_ioctl(struct platform_device *tgt, u32 cmd, void *arg)
{
	struct xrt_subdev_drvdata *drvdata = DEV_DRVDATA(tgt);

	return (*drvdata->xsd_dev_ops.xsd_ioctl)(tgt, cmd, arg);
}

int xleaf_put_leaf(struct platform_device *pdev,
		   struct platform_device *leaf);
int xleaf_create_group(struct platform_device *pdev, char *dtb);
int xleaf_destroy_group(struct platform_device *pdev, int instance);
int xleaf_wait_for_group_bringup(struct platform_device *pdev);
void xleaf_hot_reset(struct platform_device *pdev);
int xleaf_broadcast_event(struct platform_device *pdev,
			  enum xrt_events evt, bool async);
void xleaf_get_barres(struct platform_device *pdev,
		      struct resource **res, uint bar_idx);
void xleaf_get_root_id(struct platform_device *pdev,
		       unsigned short *vendor, unsigned short *device,
		       unsigned short *subvendor, unsigned short *subdevice);
struct device *xleaf_register_hwmon(struct platform_device *pdev,
				    const char *name, void *drvdata,
				    const struct attribute_group **grps);
void xleaf_unregister_hwmon(struct platform_device *pdev, struct device *hwmon);

/*
 * Character device helper APIs for use by leaf drivers
 */
static inline bool xleaf_devnode_enabled(struct xrt_subdev_drvdata *drvdata)
{
	return drvdata && drvdata->xsd_file_ops.xsf_ops.open;
}

int xleaf_devnode_create(struct platform_device *pdev,
			 const char *file_name, const char *inst_name);
int xleaf_devnode_destroy(struct platform_device *pdev);

struct platform_device *xleaf_devnode_open_excl(struct inode *inode);
struct platform_device *xleaf_devnode_open(struct inode *inode);
void xleaf_devnode_close(struct inode *inode);

/* Helpers. */
static inline void xrt_memcpy_fromio(void *buf, void __iomem *iomem, u32 size)
{
	int i;

	WARN_ON(size & 0x3);
	for (i = 0; i < size / 4; i++)
		((u32 *)buf)[i] = ioread32((char *)(iomem) + sizeof(u32) * i);
}

static inline void xrt_memcpy_toio(void __iomem *iomem, void *buf, u32 size)
{
	int i;

	WARN_ON(size & 0x3);
	for (i = 0; i < size / 4; i++)
		iowrite32(((u32 *)buf)[i], ((char *)(iomem) + sizeof(u32) * i));
}

int xleaf_register_external_driver(enum xrt_subdev_id id,
				   struct platform_driver *drv,
				   struct xrt_subdev_endpoints *eps);
void xleaf_unregister_external_driver(enum xrt_subdev_id id);

#endif	/* _XRT_LEAF_H_ */
