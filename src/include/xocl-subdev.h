/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_SUBDEV_H_
#define	_XOCL_SUBDEV_H_

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>

/*
 * Every subdev driver should have an ID for others to refer to it.
 * There can be unlimited number of instances of a subdev driver. A
 * <subdev_id, subdev_instance> tuple should be a unique identification of
 * a specific instance of a subdev driver.
 * NOTE: PLEASE do not change the order of IDs. Sub devices in the same
 * partition are initialized by this order.
 */
enum xocl_subdev_id {
	XOCL_SUBDEV_PART = 0,
	XOCL_SUBDEV_VSEC,
	XOCL_SUBDEV_VSEC_GOLDEN,
	XOCL_SUBDEV_GPIO,
	XOCL_SUBDEV_AXIGATE,
	XOCL_SUBDEV_ICAP,
	XOCL_SUBDEV_TEST,
	XOCL_SUBDEV_MGMT_MAIN,
	XOCL_SUBDEV_QSPI,
	XOCL_SUBDEV_MAILBOX,
	XOCL_SUBDEV_CMC,
	XOCL_SUBDEV_SRSR,
	XOCL_SUBDEV_NUM,
};

/*
 * If populated by subdev driver, parent will handle the mechanics of
 * char device (un)registration.
 */
enum xocl_subdev_file_mode {
	// Infra create cdev, default file name
	XOCL_SUBDEV_FILE_DEFAULT = 0,
	// Infra create cdev, need to encode inst num in file name
	XOCL_SUBDEV_FILE_MULTI_INST,
	// No auto creation of cdev by infra, leaf handles it by itself
	XOCL_SUBDEV_FILE_NO_AUTO,
};
struct xocl_subdev_file_ops {
	const struct file_operations xsf_ops;
	dev_t xsf_dev_t;
	const char *xsf_dev_name;
	enum xocl_subdev_file_mode xsf_mode;
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
	int (*xsd_ioctl)(struct platform_device *pdev, u32 cmd, void *arg);
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
typedef int (*xocl_subdev_parent_cb_t)(struct device *, void *, u32, void *);
struct xocl_subdev_platdata {
	/*
	 * Per driver instance callback. The pdev points to the instance.
	 * Should always be defined for subdev driver to call into its parent.
	 */
	xocl_subdev_parent_cb_t xsp_parent_cb;
	void *xsp_parent_cb_arg;

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
 * this struct define the endpoints belong to the same subdevice
 */
struct xocl_subdev_ep_names {
	const char *ep_name;
	const char *regmap_name;
};

struct xocl_subdev_endpoints {
	struct xocl_subdev_ep_names *xse_names;
	/* minimum number of endpoints to support the subdevice */
	u32 xse_min_ep;
};

/*
 * It manages a list of xocl_subdevs for root and partition drivers.
 */
struct xocl_subdev_pool {
	struct list_head xpool_dev_list;
	struct device *xpool_owner;
	struct mutex xpool_lock;
	bool xpool_closing;
};

typedef bool (*xocl_subdev_match_t)(enum xocl_subdev_id,
	struct platform_device *, void *);
#define	XOCL_SUBDEV_MATCH_PREV	((xocl_subdev_match_t)-1)
#define	XOCL_SUBDEV_MATCH_NEXT	((xocl_subdev_match_t)-2)

/* All subdev drivers should use below common routines to print out msg. */
#define	DEV(pdev)	(&(pdev)->dev)
#define	DEV_PDATA(pdev)					\
	((struct xocl_subdev_platdata *)dev_get_platdata(DEV(pdev)))
#define	DEV_DRVDATA(pdev)				\
	((struct xocl_subdev_drvdata *)			\
	platform_get_device_id(pdev)->driver_data)
#define	FMT_PRT(prt_fn, pdev, fmt, args...)		\
	prt_fn(DEV(pdev), "%s %s: "fmt,			\
	DEV_PDATA(pdev)->xsp_root_name, __func__, ##args)
#define xocl_err(pdev, fmt, args...) FMT_PRT(dev_err, pdev, fmt, ##args)
#define xocl_warn(pdev, fmt, args...) FMT_PRT(dev_warn, pdev, fmt, ##args)
#define xocl_info(pdev, fmt, args...) FMT_PRT(dev_info, pdev, fmt, ##args)
#define xocl_dbg(pdev, fmt, args...) FMT_PRT(dev_dbg, pdev, fmt, ##args)
#define xocl_cont(pdev, fmt, args...) FMT_PRT(dev_cont, pdev, fmt, ##args)

/*
 * Event notification.
 */
enum xocl_events {
	XOCL_EVENT_TEST = 0, // for testing
	/*
	 * Events related to specific subdev
	 * Callback arg: struct xocl_event_arg_subdev
	 */
	XOCL_EVENT_POST_CREATION,
	XOCL_EVENT_PRE_REMOVAL,
	/*
	 * Events related to change of the whole board
	 * Callback arg: <none>
	 */
	XOCL_EVENT_PRE_HOT_RESET,
	XOCL_EVENT_POST_HOT_RESET,
	XOCL_EVENT_PRE_GATE_CLOSE,
	XOCL_EVENT_POST_GATE_OPEN,
};

typedef int (*xocl_event_cb_t)(struct platform_device *pdev,
	enum xocl_events evt, void *arg);

struct xocl_event_arg_subdev {
	enum xocl_subdev_id xevt_subdev_id;
	int xevt_subdev_instance;
};

/*
 * Flags in return value from event callback.
 */
/* Done with event handling, stop waiting for the next one */
#define	XOCL_EVENT_CB_STOP	0x1
/* Error processing event */
#define	XOCL_EVENT_CB_ERR	0x2

/*
 * Subdev pool API for root and partition drivers only.
 */
extern void xocl_subdev_pool_init(struct device *dev,
	struct xocl_subdev_pool *spool);
extern int xocl_subdev_pool_fini(struct xocl_subdev_pool *spool);
extern int xocl_subdev_pool_get(struct xocl_subdev_pool *spool,
	xocl_subdev_match_t match, void *arg, struct device *holder_dev,
	struct platform_device **pdevp);
extern int xocl_subdev_pool_put(struct xocl_subdev_pool *spool,
	struct platform_device *pdev, struct device *holder_dev);
extern int xocl_subdev_pool_add(struct xocl_subdev_pool *spool,
	enum xocl_subdev_id id, xocl_subdev_parent_cb_t pcb,
	void *pcb_arg, char *dtb);
extern int xocl_subdev_pool_del(struct xocl_subdev_pool *spool,
	enum xocl_subdev_id id, int instance);
extern int xocl_subdev_pool_event(struct xocl_subdev_pool *spool,
	struct platform_device *pdev, xocl_subdev_match_t match, void *arg,
	xocl_event_cb_t xevt_cb, enum xocl_events evt);
extern ssize_t xocl_subdev_pool_get_holders(struct xocl_subdev_pool *spool,
	struct platform_device *pdev, char *buf, size_t len);
/*
 * For leaf drivers.
 */
extern struct platform_device *xocl_subdev_get_leaf(
	struct platform_device *pdev, xocl_subdev_match_t cb, void *arg);
extern struct platform_device *xocl_subdev_get_leaf_by_id(
	struct platform_device *pdev, enum xocl_subdev_id id, int instance);
extern int xocl_subdev_put_leaf(struct platform_device *pdev,
	struct platform_device *leaf);
extern int xocl_subdev_create_partition(struct platform_device *pdev,
	char *dtb);
extern int xocl_subdev_destroy_partition(struct platform_device *pdev,
	int instance);
extern void *xocl_subdev_add_event_cb(struct platform_device *pdev,
	xocl_subdev_match_t match, void *match_arg, xocl_event_cb_t cb);
extern void xocl_subdev_remove_event_cb(
	struct platform_device *pdev, void *hdl);
extern int xocl_subdev_ioctl(struct platform_device *tgt, u32 cmd, void *arg);
extern int xocl_subdev_broadcast_event(struct platform_device *pdev,
	enum xocl_events evt);
extern int xocl_subdev_broadcast_event_async(struct platform_device *pdev,
	enum xocl_events evt);
extern void xocl_subdev_hot_reset(struct platform_device *pdev);
extern void xocl_subdev_get_barres(struct platform_device *pdev,
	struct resource **res, uint bar_idx);
extern void xocl_subdev_get_parent_id(struct platform_device *pdev,
	unsigned short *vendor, unsigned short *device,
	unsigned short *subvendor, unsigned short *subdevice);
extern struct device *xocl_subdev_register_hwmon(struct platform_device *pdev,
	const char *name, void *drvdata, const struct attribute_group **grps);
extern void xocl_subdev_unregister_hwmon(struct platform_device *pdev,
	struct device *hwmon);

extern int xocl_subdev_register_external_driver(enum xocl_subdev_id id,
	struct platform_driver *drv, struct xocl_subdev_endpoints *eps);
extern void xocl_subdev_unregister_external_driver(enum xocl_subdev_id id);

/*
 * Char dev APIs.
 */
static inline bool xocl_devnode_enabled(struct xocl_subdev_drvdata *drvdata)
{
	return drvdata && drvdata->xsd_file_ops.xsf_ops.open != NULL;
}
extern int xocl_devnode_create(struct platform_device *pdev,
	const char *file_name, const char *inst_name);
extern int xocl_devnode_destroy(struct platform_device *pdev);
extern struct platform_device *xocl_devnode_open_excl(struct inode *inode);
extern struct platform_device *xocl_devnode_open(struct inode *inode);
extern void xocl_devnode_close(struct inode *inode);
static inline void xocl_memcpy_fromio(void *buf, void __iomem *iomem, u32 size)
{
	int i;

	BUG_ON(size & 0x3);
	for (i = 0; i < size / 4; i++)
		((u32 *)buf)[i] = ioread32((char *)(iomem) + sizeof(u32) * i);
}
static inline void xocl_memcpy_toio(void __iomem *iomem, void *buf, u32 size)
{
	int i;

	BUG_ON(size & 0x3);
	for (i = 0; i < size / 4; i++)
		iowrite32(((u32 *)buf)[i], ((char *)(iomem) + sizeof(u32) * i));
}

#endif	/* _XOCL_SUBDEV_H_ */
