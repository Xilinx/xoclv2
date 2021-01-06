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

#include <linux/platform_device.h>
#include "xmgmt-main.h"

struct fpga_manager;
extern int xmgmt_process_xclbin(struct platform_device *pdev,
	struct fpga_manager *fmgr, const struct axlf *xclbin, enum provider_kind kind);
extern void xmgmt_region_cleanup_all(struct platform_device *pdev);

extern int bitstream_axlf_mailbox(struct platform_device *pdev,
	const void *xclbin);
extern int xmgmt_hot_reset(struct platform_device *pdev);

/* Getting dtb for specified partition. Caller should vfree returned dtb .*/
extern char *xmgmt_get_dtb(struct platform_device *pdev,
	enum provider_kind kind);
extern char *xmgmt_get_vbnv(struct platform_device *pdev);
extern int xmgmt_get_provider_uuid(struct platform_device *pdev,
	enum provider_kind kind, uuid_t *uuid);

extern void *xmgmt_pdev2mailbox(struct platform_device *pdev);
extern void *xmgmt_mailbox_probe(struct platform_device *pdev);
extern void xmgmt_mailbox_remove(void *handle);
extern void xmgmt_peer_notify_state(void *handle, bool online);
extern void xmgmt_mailbox_event_cb(struct platform_device *pdev, void *arg);

extern int xmgmt_main_register_leaf(void);
extern void xmgmt_main_unregister_leaf(void);

#endif	/* _XMGMT_MAIN_IMPL_H_ */
