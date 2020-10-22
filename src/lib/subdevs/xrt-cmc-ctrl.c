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
#include "xocl-subdev.h"
#include "xmgmt-main.h"
#include "xocl-cmc-impl.h"

struct xocl_cmc_ctrl {
	struct platform_device *pdev;
	struct cmc_reg_map reg_mutex;
	struct cmc_reg_map reg_reset;
	struct cmc_reg_map reg_io;
	struct cmc_reg_map reg_image;
	char *firmware;
	size_t firmware_size;
	void *evt_hdl;
};

static inline void
cmc_mutex_config(struct xocl_cmc_ctrl *cmc_ctrl, u32 val)
{
	iowrite32(val, cmc_ctrl->reg_mutex.crm_addr + CMC_REG_MUTEX_CONFIG);
}

static inline u32
cmc_mutex_status(struct xocl_cmc_ctrl *cmc_ctrl)
{
	return ioread32(cmc_ctrl->reg_mutex.crm_addr + CMC_REG_MUTEX_STATUS);
}

static inline void
cmc_reset_wr(struct xocl_cmc_ctrl *cmc_ctrl, u32 val)
{
	iowrite32(val, cmc_ctrl->reg_reset.crm_addr);
}

static inline u32
cmc_reset_rd(struct xocl_cmc_ctrl *cmc_ctrl)
{
	return ioread32(cmc_ctrl->reg_reset.crm_addr);
}

static inline void
cmc_io_wr(struct xocl_cmc_ctrl *cmc_ctrl, u32 off, u32 val)
{
	iowrite32(val, cmc_ctrl->reg_io.crm_addr + off);
}

static inline u32
cmc_io_rd(struct xocl_cmc_ctrl *cmc_ctrl, u32 off)
{
	return ioread32(cmc_ctrl->reg_io.crm_addr + off);
}

static inline bool cmc_reset_held(struct xocl_cmc_ctrl *cmc_ctrl)
{
	return cmc_reset_rd(cmc_ctrl) == CMC_RESET_MASK_ON;
}

static inline bool cmc_ulp_access_allowed(struct xocl_cmc_ctrl *cmc_ctrl)
{
	return (cmc_mutex_status(cmc_ctrl) & CMC_MUTEX_MASK_GRANT) != 0;
}

static inline bool cmc_stopped(struct xocl_cmc_ctrl *cmc_ctrl)
{
	union cmc_status st;

	st.status_val = cmc_io_rd(cmc_ctrl, CMC_REG_IO_STATUS);
	return st.status.mb_stopped;
}

static inline bool cmc_ready(struct xocl_cmc_ctrl *cmc_ctrl)
{
	return (cmc_mutex_status(cmc_ctrl) & CMC_MUTEX_MASK_READY) != 0;
}

static int cmc_ulp_access(struct xocl_cmc_ctrl *cmc_ctrl, bool granted)
{
	const char *opname = granted ? "grant access" : "revoke access";

	cmc_mutex_config(cmc_ctrl, granted ? 1 : 0);
	CMC_WAIT(cmc_ulp_access_allowed(cmc_ctrl) == granted);
	if (cmc_ulp_access_allowed(cmc_ctrl) != granted) {
		xocl_err(cmc_ctrl->pdev, "mutex status is 0x%x after %s",
			cmc_mutex_status(cmc_ctrl), opname);
		return -EBUSY;
	}
	xocl_info(cmc_ctrl->pdev, "%s operation succeeded", opname);
	return 0;
}

static int cmc_stop(struct xocl_cmc_ctrl *cmc_ctrl)
{
	struct platform_device *pdev = cmc_ctrl->pdev;

	if (cmc_reset_held(cmc_ctrl)) {
		xocl_info(pdev, "CMC is already in reset state");
		return 0;
	}

	if (!cmc_stopped(cmc_ctrl)) {
		cmc_io_wr(cmc_ctrl, CMC_REG_IO_CONTROL, CMC_CTRL_MASK_STOP);
		cmc_io_wr(cmc_ctrl, CMC_REG_IO_STOP_CONFIRM, 1);
		CMC_WAIT(cmc_stopped(cmc_ctrl));
		if (!cmc_stopped(cmc_ctrl)) {
			xocl_err(pdev, "failed to stop CMC");
			return -ETIMEDOUT;
		}
	}

	cmc_reset_wr(cmc_ctrl, CMC_RESET_MASK_ON);
	if (!cmc_reset_held(cmc_ctrl)) {
		xocl_err(pdev, "failed to hold CMC in reset state");
		return -EINVAL;
	}

	xocl_info(pdev, "CMC is successfully stopped");
	return 0;
}

static int cmc_load_image(struct xocl_cmc_ctrl *cmc_ctrl)
{
	struct platform_device *pdev = cmc_ctrl->pdev;

	/* Sanity check the size of the firmware. */
	if (cmc_ctrl->firmware_size > cmc_ctrl->reg_image.crm_size) {
		xocl_err(pdev, "CMC firmware image is too big: %ld",
			cmc_ctrl->firmware_size);
		return -EINVAL;
	}

	xocl_memcpy_toio(cmc_ctrl->reg_image.crm_addr,
		cmc_ctrl->firmware, cmc_ctrl->firmware_size);
	return 0;
}

static int cmc_start(struct xocl_cmc_ctrl *cmc_ctrl)
{
	struct platform_device *pdev = cmc_ctrl->pdev;

	cmc_reset_wr(cmc_ctrl, CMC_RESET_MASK_OFF);
	if (cmc_reset_held(cmc_ctrl)) {
		xocl_err(pdev, "failed to release CMC from reset state");
		return -EINVAL;
	}

	CMC_WAIT(cmc_ready(cmc_ctrl));
	if (!cmc_ready(cmc_ctrl)) {
		xocl_err(pdev, "failed to wait for CMC to be ready");
		return -ETIMEDOUT;
	}

	xocl_info(pdev, "Wait for 5 seconds for CMC to connect to SC");
	ssleep(5);

	xocl_info(pdev, "CMC is ready: version 0x%x, status 0x%x, id 0x%x",
		cmc_io_rd(cmc_ctrl, CMC_REG_IO_VERSION),
		cmc_io_rd(cmc_ctrl, CMC_REG_IO_STATUS),
		cmc_io_rd(cmc_ctrl, CMC_REG_IO_MAGIC));

	return 0;
}

static int cmc_fetch_firmware(struct xocl_cmc_ctrl *cmc_ctrl)
{
	int ret = 0;
	struct platform_device *pdev = cmc_ctrl->pdev;
	struct platform_device *mgmt_leaf = xocl_subdev_get_leaf_by_id(pdev,
		XOCL_SUBDEV_MGMT_MAIN, PLATFORM_DEVID_NONE);
	struct xocl_mgmt_main_ioctl_get_axlf_section gs = { FIRMWARE, };

	if (mgmt_leaf == NULL)
		return -ENOENT;

	ret = xocl_subdev_ioctl(mgmt_leaf,
		XOCL_MGMT_MAIN_GET_XSABIN_SECTION, &gs);
	if (ret == 0) {
		cmc_ctrl->firmware = vmalloc(gs.xmmigas_section_size);
		if (cmc_ctrl->firmware == NULL) {
			ret = -ENOMEM;
		} else {
			memcpy(cmc_ctrl->firmware, gs.xmmigas_section,
				gs.xmmigas_section_size);
			cmc_ctrl->firmware_size = gs.xmmigas_section_size;
		}
	} else {
		xocl_err(pdev, "failed to fetch firmware: %d", ret);
	}
	(void) xocl_subdev_put_leaf(pdev, mgmt_leaf);

	return ret;
}

static ssize_t status_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	struct xocl_cmc_ctrl *cmc_ctrl = dev_get_drvdata(dev);
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

void cmc_ctrl_remove(struct platform_device *pdev)
{
	struct xocl_cmc_ctrl *cmc_ctrl =
		(struct xocl_cmc_ctrl *)cmc_pdev2ctrl(pdev);

	if (!cmc_ctrl)
		return;

	if (cmc_ctrl->evt_hdl)
		(void) xocl_subdev_remove_event_cb(pdev, cmc_ctrl->evt_hdl);
	(void) sysfs_remove_group(&DEV(cmc_ctrl->pdev)->kobj,
		&cmc_ctrl_attr_group);
	(void) cmc_ulp_access(cmc_ctrl, false);
	vfree(cmc_ctrl->firmware);
	/* We intentionally leave CMC in running state. */
}

static bool cmc_ctrl_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	/* Only interested in broadcast events. */
	return false;
}

static int cmc_ctrl_event_cb(struct platform_device *pdev,
	enum xocl_events evt, void *arg)
{
	struct xocl_cmc_ctrl *cmc_ctrl =
		(struct xocl_cmc_ctrl *)cmc_pdev2ctrl(pdev);

	switch (evt) {
	case XOCL_EVENT_PRE_GATE_CLOSE:
		(void) cmc_ulp_access(cmc_ctrl, false);
		break;
	case XOCL_EVENT_POST_GATE_OPEN:
		(void) cmc_ulp_access(cmc_ctrl, true);
		break;
	default:
		xocl_info(pdev, "ignored event %d", evt);
		break;
	}
	return XOCL_EVENT_CB_CONTINUE;
}

int cmc_ctrl_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl)
{
	struct xocl_cmc_ctrl *cmc_ctrl;
	int ret = 0;

	cmc_ctrl = devm_kzalloc(DEV(pdev), sizeof(*cmc_ctrl), GFP_KERNEL);
	if (!cmc_ctrl)
		return -ENOMEM;

	cmc_ctrl->pdev = pdev;

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

	ret  = sysfs_create_group(&DEV(pdev)->kobj, &cmc_ctrl_attr_group);
	if (ret)
		xocl_err(pdev, "failed to create sysfs nodes: %d", ret);

	cmc_ctrl->evt_hdl = xocl_subdev_add_event_cb(pdev,
		cmc_ctrl_leaf_match, NULL, cmc_ctrl_event_cb);

	*hdl = cmc_ctrl;
	return 0;

done:
	(void) cmc_ctrl_remove(pdev);
	devm_kfree(DEV(pdev), cmc_ctrl);
	return ret;
}
