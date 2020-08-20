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

/* Mutex register defines. */
#define	CMC_REG_MUTEX_CONFIG			0x0
#define	CMC_REG_MUTEX_STATUS			0x8
#define	CMC_MUTEX_GRANT_MASK			0x1
#define	CMC_MUTEX_READY_MASK			0x2

/* Reset register defines. */
#define	CMC_RESET_ON				0x0
#define	CMC_RESET_OFF				0x1

/* IO register defines. */
#define	CMC_REG_IO_MAGIC			0x0
#define	CMC_REG_IO_VERSION			0x4
#define	CMC_REG_IO_STATUS			0x8
#define	CMC_REG_IO_CONTROL			0x18
#define	CMC_REG_IO_STOP_CONFIRM			0x1C
#define	CMC_CTRL_MASK_STOP			0x8
#define	CMC_STATUS_MASK_STOPPED			0x2
#define	CMC_WAIT(cond)						\
	do {							\
		int retry = 0;					\
		while (retry++ < CMC_MAX_RETRY && !(cond))	\
			msleep(CMC_RETRY_INTERVAL);		\
	} while (0)

struct xocl_cmc_ctrl {
	struct platform_device *pdev;
	struct cmc_reg_map reg_mutex;
	struct cmc_reg_map reg_reset;
	struct cmc_reg_map reg_io;
	struct cmc_reg_map reg_image;
	char *firmware;
	size_t firmware_size;
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
	return cmc_reset_rd(cmc_ctrl) == CMC_RESET_ON;
}

static inline bool cmc_ulp_access_allowed(struct xocl_cmc_ctrl *cmc_ctrl)
{
	return (cmc_mutex_status(cmc_ctrl) & CMC_MUTEX_GRANT_MASK) != 0;
}

static inline bool cmc_stopped(struct xocl_cmc_ctrl *cmc_ctrl)
{
	return (cmc_io_rd(cmc_ctrl, CMC_REG_IO_STATUS) &
		CMC_STATUS_MASK_STOPPED) != 0;
}

static inline bool cmc_ready(struct xocl_cmc_ctrl *cmc_ctrl)
{
	return (cmc_mutex_status(cmc_ctrl) & CMC_MUTEX_READY_MASK) != 0;
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

	cmc_reset_wr(cmc_ctrl, CMC_RESET_ON);
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

	cmc_reset_wr(cmc_ctrl, CMC_RESET_OFF);
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
	struct xocl_mgmt_main_ioctl_get_xsabin_section gs = { FIRMWARE, };

	if (mgmt_leaf == NULL)
		return -ENOENT;

	ret = xocl_subdev_ioctl(mgmt_leaf,
		XOCL_MGMT_MAIN_GET_XSABIN_SECTION, &gs);
	if (ret == 0) {
		cmc_ctrl->firmware = vmalloc(gs.xmmigxs_section_size);
		if (cmc_ctrl->firmware == NULL) {
			ret = -ENOMEM;
		} else {
			memcpy(cmc_ctrl->firmware, gs.xmmigxs_section,
				gs.xmmigxs_section_size);
			cmc_ctrl->firmware_size = gs.xmmigxs_section_size;
		}
	} else {
		xocl_err(pdev, "failed to fetch firmware: %d", ret);
	}
	(void) xocl_subdev_put_leaf(pdev, mgmt_leaf);

	return ret;
}

void cmc_ctrl_remove(struct platform_device *pdev)
{
	struct xocl_cmc_ctrl *cmc_ctrl =
		(struct xocl_cmc_ctrl *)cmc_pdev2ctrl(pdev);

	if (!cmc_ctrl)
		return;

	(void) cmc_ulp_access(cmc_ctrl, false);
	vfree(cmc_ctrl->firmware);
	/* We intentionally leave CMC in running state. */
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

	*hdl = cmc_ctrl;
	return 0;

done:
	(void) cmc_ctrl_remove(pdev);
	devm_kfree(DEV(pdev), cmc_ctrl);
	return ret;
}
