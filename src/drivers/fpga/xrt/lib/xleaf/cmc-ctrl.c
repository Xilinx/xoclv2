// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/delay.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include "xleaf.h"
#include "xmgmt-main.h"
#include "xrt-cmc-impl.h"

struct xrt_cmc_ctrl {
	struct xrt_device *xdev;
	struct cmc_reg_map reg_mutex;
	struct cmc_reg_map reg_reset;
	struct cmc_reg_map reg_io;
	struct cmc_reg_map reg_image;
	char *firmware;
	size_t firmware_size;
};

static inline void xrt_memcpy_toio(void __iomem *iomem, void *buf, u32 size)
{
	int i;

	WARN_ON(size & 0x3);
	for (i = 0; i < size / 4; i++)
		iowrite32(((u32 *)buf)[i], ((char *)(iomem) + sizeof(u32) * i));
}

static inline void
cmc_mutex_config(struct xrt_cmc_ctrl *cmc_ctrl, u32 val)
{
	iowrite32(val, cmc_ctrl->reg_mutex.crm_addr + CMC_REG_MUTEX_CONFIG);
}

static inline u32
cmc_mutex_status(struct xrt_cmc_ctrl *cmc_ctrl)
{
	return ioread32(cmc_ctrl->reg_mutex.crm_addr + CMC_REG_MUTEX_STATUS);
}

static inline void
cmc_reset_wr(struct xrt_cmc_ctrl *cmc_ctrl, u32 val)
{
	iowrite32(val, cmc_ctrl->reg_reset.crm_addr);
}

static inline u32
cmc_reset_rd(struct xrt_cmc_ctrl *cmc_ctrl)
{
	return ioread32(cmc_ctrl->reg_reset.crm_addr);
}

static inline void
cmc_io_wr(struct xrt_cmc_ctrl *cmc_ctrl, u32 off, u32 val)
{
	iowrite32(val, cmc_ctrl->reg_io.crm_addr + off);
}

static inline u32
cmc_io_rd(struct xrt_cmc_ctrl *cmc_ctrl, u32 off)
{
	return ioread32(cmc_ctrl->reg_io.crm_addr + off);
}

static inline bool cmc_reset_held(struct xrt_cmc_ctrl *cmc_ctrl)
{
	return cmc_reset_rd(cmc_ctrl) == CMC_RESET_MASK_ON;
}

static inline bool cmc_ulp_access_allowed(struct xrt_cmc_ctrl *cmc_ctrl)
{
	return (cmc_mutex_status(cmc_ctrl) & CMC_MUTEX_MASK_GRANT) != 0;
}

static inline bool cmc_stopped(struct xrt_cmc_ctrl *cmc_ctrl)
{
	union cmc_status st;

	st.status_val = cmc_io_rd(cmc_ctrl, CMC_REG_IO_STATUS);
	return st.status.mb_stopped;
}

static inline bool cmc_ready(struct xrt_cmc_ctrl *cmc_ctrl)
{
	return (cmc_mutex_status(cmc_ctrl) & CMC_MUTEX_MASK_READY) != 0;
}

static int cmc_ulp_access(struct xrt_cmc_ctrl *cmc_ctrl, bool granted)
{
	const char *opname = granted ? "grant access" : "revoke access";

	cmc_mutex_config(cmc_ctrl, granted ? 1 : 0);
	CMC_WAIT(cmc_ulp_access_allowed(cmc_ctrl) == granted);
	if (cmc_ulp_access_allowed(cmc_ctrl) != granted) {
		xrt_err(cmc_ctrl->xdev, "mutex status is 0x%x after %s",
			cmc_mutex_status(cmc_ctrl), opname);
		return -EBUSY;
	}
	xrt_info(cmc_ctrl->xdev, "%s operation succeeded", opname);
	return 0;
}

static int cmc_stop(struct xrt_cmc_ctrl *cmc_ctrl)
{
	struct xrt_device *xdev = cmc_ctrl->xdev;

	if (cmc_reset_held(cmc_ctrl)) {
		xrt_info(xdev, "CMC is already in reset state");
		return 0;
	}

	if (!cmc_stopped(cmc_ctrl)) {
		cmc_io_wr(cmc_ctrl, CMC_REG_IO_CONTROL, CMC_CTRL_MASK_STOP);
		cmc_io_wr(cmc_ctrl, CMC_REG_IO_STOP_CONFIRM, 1);
		CMC_WAIT(cmc_stopped(cmc_ctrl));
		if (!cmc_stopped(cmc_ctrl)) {
			xrt_err(xdev, "failed to stop CMC");
			return -ETIMEDOUT;
		}
	}

	cmc_reset_wr(cmc_ctrl, CMC_RESET_MASK_ON);
	if (!cmc_reset_held(cmc_ctrl)) {
		xrt_err(xdev, "failed to hold CMC in reset state");
		return -EINVAL;
	}

	xrt_info(xdev, "CMC is successfully stopped");
	return 0;
}

static int cmc_load_image(struct xrt_cmc_ctrl *cmc_ctrl)
{
	struct xrt_device *xdev = cmc_ctrl->xdev;

	/* Sanity check the size of the firmware. */
	if (cmc_ctrl->firmware_size > cmc_ctrl->reg_image.crm_size) {
		xrt_err(xdev, "CMC firmware image is too big: %zu",
			cmc_ctrl->firmware_size);
		return -EINVAL;
	}

	xrt_memcpy_toio(cmc_ctrl->reg_image.crm_addr, cmc_ctrl->firmware, cmc_ctrl->firmware_size);
	return 0;
}

static int cmc_start(struct xrt_cmc_ctrl *cmc_ctrl)
{
	struct xrt_device *xdev = cmc_ctrl->xdev;

	cmc_reset_wr(cmc_ctrl, CMC_RESET_MASK_OFF);
	if (cmc_reset_held(cmc_ctrl)) {
		xrt_err(xdev, "failed to release CMC from reset state");
		return -EINVAL;
	}

	CMC_WAIT(cmc_ready(cmc_ctrl));
	if (!cmc_ready(cmc_ctrl)) {
		xrt_err(xdev, "failed to wait for CMC to be ready");
		return -ETIMEDOUT;
	}

	xrt_info(xdev, "Wait for 5 seconds for CMC to connect to SC");
	ssleep(5);

	xrt_info(xdev, "CMC is ready: version 0x%x, status 0x%x, id 0x%x",
		 cmc_io_rd(cmc_ctrl, CMC_REG_IO_VERSION),
		 cmc_io_rd(cmc_ctrl, CMC_REG_IO_STATUS),
		 cmc_io_rd(cmc_ctrl, CMC_REG_IO_MAGIC));

	return 0;
}

static int cmc_fetch_firmware(struct xrt_cmc_ctrl *cmc_ctrl)
{
	int ret = 0;
	struct xrt_device *xdev = cmc_ctrl->xdev;
	struct xrt_device *mgmt_leaf = xleaf_get_leaf_by_id(xdev,
		XRT_SUBDEV_MGMT_MAIN, XRT_INVALID_DEVICE_INST);
	struct xrt_mgmt_main_get_axlf_section gs = {
		XMGMT_BLP, FIRMWARE,
	};

	if (!mgmt_leaf)
		return -ENOENT;

	ret = xleaf_call(mgmt_leaf, XRT_MGMT_MAIN_GET_AXLF_SECTION, &gs);
	if (ret == 0) {
		cmc_ctrl->firmware = vmalloc(gs.xmmigas_section_size);
		if (!cmc_ctrl->firmware) {
			ret = -ENOMEM;
		} else {
			memcpy(cmc_ctrl->firmware, gs.xmmigas_section, gs.xmmigas_section_size);
			cmc_ctrl->firmware_size = gs.xmmigas_section_size;
		}
	} else {
		xrt_err(xdev, "failed to fetch firmware: %d", ret);
	}
	xleaf_put_leaf(xdev, mgmt_leaf);

	return ret;
}

static ssize_t status_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct xrt_cmc_ctrl *cmc_ctrl = dev_get_drvdata(dev);
	u32 val = cmc_io_rd(cmc_ctrl, CMC_REG_IO_STATUS);

	return sprintf(buf, "0x%x\n", val);
}
static DEVICE_ATTR_RO(status);

static struct attribute *cmc_ctrl_attrs[] = {
	&dev_attr_status.attr,
	NULL,
};

static struct attribute_group cmc_ctrl_attr_group = {
	.attrs = cmc_ctrl_attrs,
};

void cmc_ctrl_remove(struct xrt_device *xdev)
{
	struct xrt_cmc_ctrl *cmc_ctrl =
		(struct xrt_cmc_ctrl *)cmc_xdev2ctrl(xdev);

	if (!cmc_ctrl)
		return;

	sysfs_remove_group(&DEV(cmc_ctrl->xdev)->kobj, &cmc_ctrl_attr_group);
	cmc_ulp_access(cmc_ctrl, false);
	vfree(cmc_ctrl->firmware);
	/* We intentionally leave CMC in running state. */
}

void cmc_ctrl_event_cb(struct xrt_device *xdev, void *arg)
{
	struct xrt_cmc_ctrl *cmc_ctrl =
		(struct xrt_cmc_ctrl *)cmc_xdev2ctrl(xdev);
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;

	switch (e) {
	case XRT_EVENT_PRE_GATE_CLOSE:
		cmc_ulp_access(cmc_ctrl, false);
		break;
	case XRT_EVENT_POST_GATE_OPEN:
		cmc_ulp_access(cmc_ctrl, true);
		break;
	default:
		xrt_dbg(xdev, "ignored event %d", e);
		break;
	}
}

int cmc_ctrl_probe(struct xrt_device *xdev, struct cmc_reg_map *regmaps, void **hdl)
{
	struct xrt_cmc_ctrl *cmc_ctrl;
	int ret = 0;

	cmc_ctrl = devm_kzalloc(DEV(xdev), sizeof(*cmc_ctrl), GFP_KERNEL);
	if (!cmc_ctrl)
		return -ENOMEM;

	cmc_ctrl->xdev = xdev;

	/* Obtain register maps we need to start/stop CMC. */
	cmc_ctrl->reg_mutex = regmaps[IO_MUTEX];
	cmc_ctrl->reg_reset = regmaps[IO_GPIO];
	cmc_ctrl->reg_io = regmaps[IO_REG];
	cmc_ctrl->reg_image = regmaps[IO_IMAGE_MGMT];

	/* Get firmware image from xmgmt-main leaf. */
	ret = cmc_fetch_firmware(cmc_ctrl);
	if (ret)
		goto done;

	/* Load firmware. */

	ret = cmc_ulp_access(cmc_ctrl, false);
	if (ret)
		goto done;

	ret = cmc_stop(cmc_ctrl);
	if (ret)
		goto done;

	ret = cmc_load_image(cmc_ctrl);
	if (ret)
		goto done;

	ret = cmc_start(cmc_ctrl);
	if (ret)
		goto done;

	ret  = sysfs_create_group(&DEV(xdev)->kobj, &cmc_ctrl_attr_group);
	if (ret)
		xrt_err(xdev, "failed to create sysfs nodes: %d", ret);

	*hdl = cmc_ctrl;
	return 0;

done:
	cmc_ctrl_remove(xdev);
	devm_kfree(DEV(xdev), cmc_ctrl);
	return ret;
}
