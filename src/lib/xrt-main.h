/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_MAIN_H_
#define	_XOCL_MAIN_H_

extern struct platform_driver xocl_partition_driver;
extern struct platform_driver xocl_test_driver;
extern struct platform_driver xocl_vsec_driver;
extern struct platform_driver xocl_vsec_golden_driver;
extern struct platform_driver xocl_axigate_driver;
extern struct platform_driver xocl_qspi_driver;
extern struct platform_driver xocl_gpio_driver;
extern struct platform_driver xocl_mailbox_driver;
extern struct platform_driver xocl_icap_driver;
extern struct platform_driver xocl_cmc_driver;
extern struct platform_driver xocl_clkfreq_driver;
extern struct platform_driver xocl_clock_driver;
extern struct platform_driver xocl_ucs_driver;
extern struct platform_driver xocl_calib_driver;

extern struct xocl_subdev_endpoints xocl_vsec_endpoints[];
extern struct xocl_subdev_endpoints xocl_vsec_golden_endpoints[];
extern struct xocl_subdev_endpoints xocl_axigate_endpoints[];
extern struct xocl_subdev_endpoints xocl_test_endpoints[];
extern struct xocl_subdev_endpoints xocl_qspi_endpoints[];
extern struct xocl_subdev_endpoints xocl_gpio_endpoints[];
extern struct xocl_subdev_endpoints xocl_mailbox_endpoints[];
extern struct xocl_subdev_endpoints xocl_icap_endpoints[];
extern struct xocl_subdev_endpoints xocl_cmc_endpoints[];
extern struct xocl_subdev_endpoints xocl_clkfreq_endpoints[];
extern struct xocl_subdev_endpoints xocl_clock_endpoints[];
extern struct xocl_subdev_endpoints xocl_ucs_endpoints[];
extern struct xocl_subdev_endpoints xocl_calib_endpoints[];

extern const char *xocl_drv_name(enum xocl_subdev_id id);
extern int xocl_drv_get_instance(enum xocl_subdev_id id);
extern void xocl_drv_put_instance(enum xocl_subdev_id id, int instance);
extern struct xocl_subdev_endpoints *xocl_drv_get_endpoints(enum xocl_subdev_id id);

#endif	/* _XOCL_MAIN_H_ */
