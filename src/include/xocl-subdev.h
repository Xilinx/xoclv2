/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_SUBDEV_H_
#define	_XOCL_SUBDEV_H_

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>

/*
 * Every subdev driver should have an ID for others to refer to it.
 * There can be unlimited number of instances of a subdev driver. A
 * <subdev_id, subdev_instance> tuple should be a unique identification of
 * a specific instance of a subdev driver.
 */
enum xocl_subdev_id {
	XOCL_SUBDEV_PART = 0,
	XOCL_SUBDEV_TEST,
};

/*
 * If populated by subdev driver, parent will handle the mechanics of
 * char device (un)registration.
 */
struct xocl_subdev_file_ops {
	const struct file_operations xsf_ops;
	dev_t xsf_dev_t;
};

/*
 * Subdev driver callbacks populated by subdev driver.
 */
struct xocl_subdev_drv_ops {
	/*
	 * Per driver module callback. Don't take any arguments.
	 * If defined these are called as part of driver (un)registration.
	 */
	int (*xsd_post_init)(void);
	void (*xsd_pre_exit)(void);

	/*
	 * Per driver instance callback. The pdev points to the instance.
	 * If defined these are called by other leaf drivers.
	 * Note that root driver may call into xsd_ioctl of a partition driver.
	 */
	long (*xsd_ioctl)(struct platform_device *pdev, u32 cmd, u64 arg);

	/*
	 * Per driver instance callback. The pdev points to the instance.
	 * If defined these are called by partition or root drivers.
	 */
	int (*xsd_online)(struct platform_device *pdev);
	int (*xsd_offline)(struct platform_device *pdev);
};

/*
 * Defined and populated by subdev driver, exported as driver_data in
 * struct platform_device_id.
 */
struct xocl_subdev_drvdata {
	struct xocl_subdev_file_ops xsd_file_ops;
	struct xocl_subdev_drv_ops xsd_dev_ops;
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
typedef long (*xocl_subdev_parent_cb_t)(struct device *, u32, u64);
struct xocl_subdev_platdata {
	/*
	 * Per driver instance callback. The pdev points to the instance.
	 * Should always be defined for subdev driver to call into its parent.
	 */
	xocl_subdev_parent_cb_t xsp_parent_cb;

	/* Something to associate w/ root for msg printing. */
	const char *xsp_root_name;

	/*
	 * Char dev support for this subdev instance.
	 * Initialized by subdev driver.
	 */
	struct cdev xsp_cdev;
	struct device *xsp_sysdev;
	struct mutex xsp_devnode_lock;
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
	char xsp_dtb[1];
};

/*
 * It represents a specific instance of platform driver for a subdev, which
 * provides services to its clients (another subdev driver or root driver).
 */
struct xocl_subdev {
	struct list_head xs_dev_list;
	enum xocl_subdev_id xs_id;		/* type of subdev */
	struct platform_device *xs_pdev;	/* a particular subdev inst */
};

typedef bool (*xocl_leaf_match_t)(struct xocl_subdev *, u64);

/* All subdev drivers should use below common routines to print out msg. */
#define	DEV(pdev)	(&(pdev)->dev)
#define	DEV_PDATA(pdev)					\
	((struct xocl_subdev_platdata*)dev_get_platdata(DEV(pdev)))
#define	DEV_DRVDATA(pdev)				\
	((struct xocl_subdev_drvdata*)platform_get_device_id(pdev)->driver_data)
#define	FMT_PRT(prt_fn, pdev, fmt, args...)		\
	prt_fn(DEV(pdev), "%s %s: "fmt,			\
	DEV_PDATA(pdev)->xsp_root_name, __func__, ##args)
#define xocl_err(pdev, fmt, args...) FMT_PRT(dev_err, pdev, fmt, ##args)
#define xocl_warn(pdev, fmt, args...) FMT_PRT(dev_warn, pdev, fmt, ##args)
#define xocl_info(pdev, fmt, args...) FMT_PRT(dev_info, pdev, fmt, ##args)
#define xocl_dbg(pdev, fmt, args...) FMT_PRT(dev_dbg, pdev, fmt, ##args)

/* For root and partition drivers. */
extern struct xocl_subdev *
xocl_subdev_create(struct device *parent, enum xocl_subdev_id id,
	int instance, xocl_subdev_parent_cb_t pcb, void *dtb);
extern void xocl_subdev_destroy(struct xocl_subdev *sdev);
extern int xocl_subdev_online(struct platform_device *pdev);
extern int xocl_subdev_offline(struct platform_device *pdev);

/* For leaf drivers. */
extern long xocl_subdev_parent_ioctl(struct platform_device *self,
	u32 cmd, u64 arg);
extern long xocl_subdev_ioctl(struct platform_device *tgt, u32 cmd, u64 arg);
extern struct platform_device *
xocl_subdev_get_leaf(struct platform_device *pdev, enum xocl_subdev_id id,
	xocl_leaf_match_t match_cb, u64 match_arg);

extern int xocl_devnode_create(struct platform_device *pdev, const char *name);
extern int xocl_devnode_destroy(struct platform_device *pdev);
extern struct platform_device *xocl_devnode_open_excl(struct inode *inode);
extern struct platform_device *xocl_devnode_open(struct inode *inode);
extern void xocl_devnode_close(struct inode *inode);

#endif	/* _XOCL_SUBDEV_H_ */
