/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XMGMT_XMGNT_H_
#define _XMGMT_XMGNT_H_

#include "xmgmt-main.h"

struct fpga_manager;
int xmgmt_process_xclbin(struct xrt_device *xdev,
			 struct fpga_manager *fmgr,
			 const struct axlf *xclbin,
			 enum provider_kind kind);
void xmgmt_region_cleanup_all(struct xrt_device *xdev);

int bitstream_axlf_mailbox(struct xrt_device *xdev, const void *xclbin);
int xmgmt_hot_reset(struct xrt_device *xdev);

/* Getting dtb for specified group. Caller should vfree returned dtb. */
char *xmgmt_get_dtb(struct xrt_device *xdev, enum provider_kind kind);
char *xmgmt_get_vbnv(struct xrt_device *xdev);
int xmgmt_get_provider_uuid(struct xrt_device *xdev,
			    enum provider_kind kind, uuid_t *uuid);

void *xmgmt_xdev2mailbox(struct xrt_device *xdev);
void *xmgmt_mailbox_probe(struct xrt_device *xdev);
void xmgmt_mailbox_remove(void *handle);
void xmgmt_peer_notify_state(void *handle, bool online);
void xmgmt_mailbox_event_cb(struct xrt_device *xdev, void *arg);

int xmgmt_register_leaf(void);
void xmgmt_unregister_leaf(void);

#endif	/* _XMGMT_XMGNT_H_ */
