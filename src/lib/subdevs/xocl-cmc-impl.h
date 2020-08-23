/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_CMC_IMPL_H_
#define	_XOCL_CMC_IMPL_H_

#include "linux/delay.h"
#include "xocl-subdev.h"

#define	CMC_MAX_RETRY		150 /* Retry is set to 15s */
#define	CMC_MAX_RETRY_LONG	(CMC_MAX_RETRY * 4) /* mailbox retry is 1min */
#define	CMC_RETRY_INTERVAL	100 /* 100ms */

/* Mutex register defines. */
#define	CMC_REG_MUTEX_CONFIG			0x0
#define	CMC_REG_MUTEX_STATUS			0x8
#define	CMC_MUTEX_MASK_GRANT			(0x1 << 0)
#define	CMC_MUTEX_MASK_READY			(0x1 << 1)

/* Reset register defines. */
#define	CMC_RESET_MASK_ON			0x0
#define	CMC_RESET_MASK_OFF			0x1

/* IO register defines. */
#define	CMC_REG_IO_MAGIC			0x0
#define	CMC_REG_IO_VERSION			0x4
#define	CMC_REG_IO_STATUS			0x8
#define	CMC_REG_IO_ERROR			0xc
#define	CMC_REG_IO_CONTROL			0x18
#define	CMC_REG_IO_STOP_CONFIRM			0x1C
#define	CMC_REG_IO_MBX_OFFSET			0x300
#define	CMC_REG_IO_MBX_ERROR			0x304
#define	CMC_REG_IO_CORE_VERSION			0xC4C

#define	CMC_CTRL_MASK_CLR_ERR			(1 << 1)
#define	CMC_CTRL_MASK_STOP			(1 << 3)
#define	CMC_CTRL_MASK_MBX_PKT_OWNER		(1 << 5)
#define	CMC_ERROR_MASK_MBX_ERR			(1 << 26)
#define	CMC_STATUS_MASK_STOPPED			(1 << 1)

#define	__CMC_WAIT(cond, retries)				\
	do {							\
		int retry = 0;					\
		while (retry++ < retries && !(cond))		\
			msleep(CMC_RETRY_INTERVAL);		\
	} while (0)
#define CMC_WAIT(cond)	__CMC_WAIT(cond, CMC_MAX_RETRY)
#define CMC_LONG_WAIT(cond)	__CMC_WAIT(cond, CMC_MAX_RETRY_LONG)

union cmc_status {
	u32 status_val;
	struct {
		u32 init_done		: 1;
		u32 mb_stopped		: 1;
		u32 reserved0		: 1;
		u32 watchdog_reset	: 1;
		u32 reserved1		: 6;
		u32 power_mode		: 2;
		u32 reserved2		: 12;
		u32 sc_comm_ver		: 4;
		u32 sc_mode		: 3;
		u32 invalid_sc		: 1;
	} status;
};

enum {
	CMC_MBX_PKT_OP_UNKNOWN = 0,
	CMC_MBX_PKT_OP_MSP432_SEC_START,
	CMC_MBX_PKT_OP_MSP432_SEC_DATA,
	CMC_MBX_PKT_OP_MSP432_IMAGE_END,
	CMC_MBX_PKT_OP_BOARD_INFO,
	CMC_MBX_PKT_OP_MSP432_ERASE_FW,
};

enum {
	IO_REG = 0,
	IO_GPIO,
	IO_IMAGE_MGMT,
	IO_MUTEX,
	NUM_IOADDR
};

struct cmc_reg_map {
	void __iomem *crm_addr;
	size_t crm_size;
};

extern int cmc_ctrl_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl);
extern void cmc_ctrl_remove(struct platform_device *pdev);
extern void *cmc_pdev2ctrl(struct platform_device *pdev);

extern int cmc_sensor_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl);
extern void cmc_sensor_remove(struct platform_device *pdev);
extern void *cmc_pdev2sensor(struct platform_device *pdev);

extern int cmc_mailbox_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl);
extern void cmc_mailbox_remove(struct platform_device *pdev);
extern void *cmc_pdev2mbx(struct platform_device *pdev);
extern int cmc_mailbox_acquire(struct platform_device *pdev);
extern void cmc_mailbox_release(struct platform_device *pdev, int generation);
extern size_t cmc_mailbox_max_payload(struct platform_device *pdev);
extern int cmc_mailbox_send_packet(struct platform_device *pdev, int generation,
	u8 op, const char *buf, size_t len);
extern int cmc_mailbox_recv_packet(struct platform_device *pdev, int generation,
	char *buf, size_t *len);

extern int cmc_bdinfo_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl);
extern void cmc_bdinfo_remove(struct platform_device *pdev);
extern void *cmc_pdev2bdinfo(struct platform_device *pdev);
extern int cmc_refresh_board_info(struct platform_device *pdev);

extern int cmc_sc_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl);
extern void cmc_sc_remove(struct platform_device *pdev);
extern void *cmc_pdev2sc(struct platform_device *pdev);
extern int cmc_sc_open(struct inode *inode, struct file *file);
extern int cmc_sc_close(struct inode *inode, struct file *file);
extern ssize_t cmc_update_sc_firmware(struct file *file,
	const char __user *ubuf, size_t n, loff_t *off);
extern loff_t cmc_sc_llseek(struct file *filp, loff_t off, int whence);

#endif	/* _XOCL_CMC_IMPL_H_ */
