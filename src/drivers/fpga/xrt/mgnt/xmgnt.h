/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XMGNT_XMGNT_H_
#define _XMGNT_XMGNT_H_

#include "xmgnt-main.h"

struct fpga_manager;
int xmgnt_process_xclbin(struct xrt_device *xdev,
			 struct fpga_manager *fmgr,
			 const struct axlf *xclbin,
			 enum provider_kind kind);
void xmgnt_region_cleanup_all(struct xrt_device *xdev);

int bitstream_axlf_mailbox(struct xrt_device *xdev, const void *xclbin);
int xmgnt_hot_reset(struct xrt_device *xdev);

/* Getting dtb for specified group. Caller should vfree returned dtb. */
char *xmgnt_get_dtb(struct xrt_device *xdev, enum provider_kind kind);
char *xmgnt_get_vbnv(struct xrt_device *xdev);
int xmgnt_get_provider_uuid(struct xrt_device *xdev,
			    enum provider_kind kind, uuid_t *uuid);

void *xmgnt_xdev2mailbox(struct xrt_device *xdev);
void *xmgnt_mailbox_probe(struct xrt_device *xdev);
void xmgnt_mailbox_remove(void *handle);
void xmgnt_peer_notify_state(void *handle, bool online);
void xmgnt_mailbox_event_cb(struct xrt_device *xdev, void *arg);

int xmgnt_register_leaf(void);
void xmgnt_unregister_leaf(void);

#endif	/* _XMGNT_XMGNT_H_ */
