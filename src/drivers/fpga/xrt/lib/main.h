/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_MAIN_H_
#define	_XRT_MAIN_H_

extern struct platform_driver xrt_group_driver;
extern struct platform_driver xrt_test_driver;
extern struct platform_driver xrt_vsec_driver;
extern struct platform_driver xrt_vsec_golden_driver;
extern struct platform_driver xrt_axigate_driver;
extern struct platform_driver xrt_qspi_driver;
extern struct platform_driver xrt_gpio_driver;
extern struct platform_driver xrt_mailbox_driver;
extern struct platform_driver xrt_icap_driver;
extern struct platform_driver xrt_cmc_driver;
extern struct platform_driver xrt_clkfreq_driver;
extern struct platform_driver xrt_clock_driver;
extern struct platform_driver xrt_ucs_driver;
extern struct platform_driver xrt_calib_driver;

extern struct xrt_subdev_endpoints xrt_vsec_endpoints[];
extern struct xrt_subdev_endpoints xrt_vsec_golden_endpoints[];
extern struct xrt_subdev_endpoints xrt_axigate_endpoints[];
extern struct xrt_subdev_endpoints xrt_test_endpoints[];
extern struct xrt_subdev_endpoints xrt_qspi_endpoints[];
extern struct xrt_subdev_endpoints xrt_gpio_endpoints[];
extern struct xrt_subdev_endpoints xrt_mailbox_endpoints[];
extern struct xrt_subdev_endpoints xrt_icap_endpoints[];
extern struct xrt_subdev_endpoints xrt_cmc_endpoints[];
extern struct xrt_subdev_endpoints xrt_clkfreq_endpoints[];
extern struct xrt_subdev_endpoints xrt_clock_endpoints[];
extern struct xrt_subdev_endpoints xrt_ucs_endpoints[];
extern struct xrt_subdev_endpoints xrt_calib_endpoints[];

const char *xrt_drv_name(enum xrt_subdev_id id);
int xrt_drv_get_instance(enum xrt_subdev_id id);
void xrt_drv_put_instance(enum xrt_subdev_id id, int instance);
struct xrt_subdev_endpoints *xrt_drv_get_endpoints(enum xrt_subdev_id id);

#endif	/* _XRT_MAIN_H_ */
