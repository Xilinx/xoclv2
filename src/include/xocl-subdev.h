// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_SUBDEV_H_
#define	_XOCL_SUBDEV_H_

#include <linux/platform_device.h>
#include <linux/pci.h>
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
 * Defines all flavors of partitions. This also serves as instance ID for
 * partition subdev. An instance of partition subdev can be identified by
 * <XOCL_SUBDEV_PART, xocl_partition_id>.
 */
enum xocl_partition_id {
	XOCL_PART_TEST = 0,
};

/*
 * If populated by subdev driver, parent will handle the mechanics of
 * char device (un)registration.
 */
struct xocl_subdev_file_ops {
	const struct file_operations xsf_ops;
	dev_t xsf_dev_t;
	const char *xsf_dev_name;
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
	 */
	long (*xsd_ioctl)(struct platform_device *pdev, u32 cmd, u64 arg);

	/*
	 * Per driver instance callback. The pdev points to the instance.
	 * If defined these are called by partition or root drivers.
	 */
	int (*xsd_offline)(struct platform_device *pdev);
	int (*xsd_online)(struct platform_device *pdev);
};

#define	XOCL_MAX_DEVICE_NODES	128

/*
 * Defined and populated by subdev driver, exported as driver_data in
 * struct platform_device_id.
 */
struct xocl_subdev_data {
	struct xocl_subdev_file_ops xsd_file_ops;
	struct xocl_subdev_drv_ops xsd_dev_ops;
};

/*
 * Defined and populated by parent driver, passed in as subdev driver's
 * platform data when creating subdev driver instance.
 */
typedef long (*xocl_subdev_parent_cb_t)(struct device *, u32, u64);
struct xocl_subdev_platdata {
	/*
	 * Refer back to the platform device who is holding it.
	 */
	struct platform_device *xsp_pdev;
	/*
	 * Per driver instance callback. The pdev points to the instance.
	 * Should always be defined for subdev driver to call into its parent.
	 */
	xocl_subdev_parent_cb_t xsp_parent_cb;

	/*
	 * Populated by parent driver to pass in the subdev driver
	 * private data. This is optional.
	 */
	void *xsp_drv_priv;
	size_t xsp_drv_priv_len;

	/* Something to associate w/ root for msg printing. */
	int xsp_domain;
	unsigned int xsp_bus;
	unsigned int xsp_dev;
	unsigned int xsp_func;

	/* Char dev of this subdev instance */
	struct cdev xsp_cdev;
	struct mutex xsp_devnode_lock;
	struct completion xsp_devnode_comp;
	int xsp_devnode_ref;
	bool xsp_devnode_online;
	bool xsp_devnode_excl;

	/*
	 * Populated by parent driver to describe the device tree for
	 * the subdev driver to handle. Variable len, should always be last one.
	 */
	size_t xsp_dtb_len; // Redundant??
	char xsp_dtb[1];
};

/*
 * It represents a specific instance of platform driver for a subdev, which
 * provides services to its clients (another subdev driver or root driver).
 */
struct xocl_subdev {
	struct list_head xs_dev_list;
	enum xocl_subdev_id xs_id;		/* type of subdev */
	int xs_instance;			/* drv instance & minor */
	struct platform_driver *xs_drv;		/* all drv ops found by xs_id */
	struct platform_device *xs_pdev;	/* a particular subdev inst */
};

/*
 * Parent IOCTL calls.
 */
enum xocl_parent_ioctl_cmd {
	XOCL_PARENT_GET_LEAF = 0,
	XOCL_PARENT_PUT_LEAF,
};

typedef bool (*xocl_leaf_match_t)(struct xocl_subdev *, u64);
typedef void * xocl_subdev_leaf_handle_t;
struct xocl_parent_ioctl_get_leaf {
	struct platform_device *xpigl_pdev; /* caller's pdev */
	enum xocl_subdev_id xpigl_id;
	xocl_leaf_match_t xpigl_match_cb;
	u64 xpigl_match_arg;
	xocl_subdev_leaf_handle_t xpigl_leaf; /* target leaf handle */
};

/* All subdev drivers should use below common routines to print out msg. */
#define	DEV(pdev)	(&(pdev)->dev)
#define	DEV_PDATA(pdev)					\
	((struct xocl_subdev_platdata *)dev_get_platdata(DEV(pdev)))
#define	FMT_PRT(prt_fn, pdev, fmt, args...)		\
	prt_fn(DEV(pdev), "%x:%x:%x.%x %s: "fmt,	\
	DEV_PDATA(pdev)->xsp_domain,			\
	DEV_PDATA(pdev)->xsp_bus,			\
	DEV_PDATA(pdev)->xsp_dev,			\
	DEV_PDATA(pdev)->xsp_func,			\
	__func__, ##args)
#define xocl_err(pdev, fmt, args...) FMT_PRT(dev_err, pdev, fmt, ##args)
#define xocl_warn(pdev, fmt, args...) FMT_PRT(dev_warn, pdev, fmt, ##args)
#define xocl_info(pdev, fmt, args...) FMT_PRT(dev_info, pdev, fmt, ##args)
#define xocl_dbg(pdev, fmt, args...) FMT_PRT(dev_dbg, pdev, fmt, ##args)

/* For root and partition drivers. */
extern struct xocl_subdev *
xocl_subdev_create_partition(struct pci_dev *root, enum xocl_partition_id id,
	xocl_subdev_parent_cb_t pcb, void *dtb, size_t dtb_len);
extern struct xocl_subdev *
xocl_subdev_create_leaf(struct platform_device *part, enum xocl_subdev_id id,
	xocl_subdev_parent_cb_t pcb, void *dtb, size_t dtb_len);
extern void xocl_subdev_destroy(struct xocl_subdev *sdev);

/* For leaf drivers. */
extern long xocl_subdev_parent_ioctl(struct platform_device *pdev,
	u32 cmd, u64 arg);
extern long xocl_subdev_ioctl(xocl_subdev_leaf_handle_t handle,
	u32 cmd, u64 arg);
extern xocl_subdev_leaf_handle_t
xocl_subdev_get_leaf(struct platform_device *pdev, enum xocl_subdev_id id,
	xocl_leaf_match_t match_cb, u64 match_arg);
extern void xocl_devnode_allowed(struct platform_device *pdev);
extern int xocl_devnode_disallowed(struct platform_device *pdev);
extern struct platform_device *xocl_devnode_open_excl(struct inode *inode);
extern struct platform_device *xocl_devnode_open(struct inode *inode);
extern void xocl_devnode_close(struct inode *inode);

#endif	/* _XOCL_SUBDEV_H_ */
