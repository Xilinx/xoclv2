/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_CMC_IMPL_H_
#define	_XOCL_CMC_IMPL_H_

#include "xocl-subdev.h"

#define	CMC_MAX_RETRY		150 /* Retry is set to 15s */
#define	CMC_RETRY_INTERVAL	100 /* 100ms */

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

enum cmc_packet_op {
	XPO_UNKNOWN = 0,
	XPO_MSP432_SEC_START,
	XPO_MSP432_SEC_DATA,
	XPO_MSP432_IMAGE_END,
	XPO_BOARD_INFO,
	XPO_MSP432_ERASE_FW,
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

#endif	/* _XOCL_CMC_IMPL_H_ */
