/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XMGMT_MAIN_IMPL_H_
#define	_XMGMT_MAIN_IMPL_H_

#include "xocl-subdev.h"
#include "xmgmt-main.h"

extern struct platform_driver xmgmt_main_driver;
extern struct xocl_subdev_endpoints xocl_mgmt_main_endpoints[];

extern int xmgmt_ulp_download(struct platform_device *pdev, void *xclbin);

/* Getting dtb for specified partition. Caller should vfree returned dtb .*/
enum provider_kind {
	XMGMT_BLP,
	XMGMT_PLP,
	XMGMT_ULP,
};
extern char *xmgmt_get_dtb(struct platform_device *pdev, enum provider_kind);
extern char *xmgmt_get_vbnv(struct platform_device *pdev);

extern void *xmgmt_pdev2mailbox(struct platform_device *pdev);
extern void *xmgmt_mailbox_probe(struct platform_device *pdev);
extern void xmgmt_mailbox_remove(void *handle);
extern int xmgmt_peer_test_msg(void *handle,
	struct xocl_mgmt_main_peer_test_msg *tm);
extern void xmgmt_peer_notify_state(void *handle, bool online);

#endif	/* _XMGMT_MAIN_IMPL_H_ */
