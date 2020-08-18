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
#define	CMC_WAIT(cond)						\
	do {							\
		int retry = 0;					\
		while (retry++ < CMC_MAX_RETRY && !(cond))	\
			msleep(CMC_RETRY_INTERVAL);		\
	} while (0)

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

#endif	/* _XOCL_CMC_IMPL_H_ */
