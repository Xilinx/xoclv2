/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_MAILBOX_H_
#define	_XRT_MAILBOX_H_

/*
 * Mailbox IP driver IOCTL calls.
 */
enum xrt_mailbox_ioctl_cmd {
	XRT_MAILBOX_POST = 0,
	XRT_MAILBOX_REQUEST,
	XRT_MAILBOX_LISTEN,
};

struct xrt_mailbox_ioctl_post {
	u64 xmip_req_id; /* 0 means response */
	bool xmip_sw_ch;
	void *xmip_data;
	size_t xmip_data_size;
};

struct xrt_mailbox_ioctl_request {
	bool xmir_sw_ch;
	u32 xmir_resp_ttl;
	void *xmir_req;
	size_t xmir_req_size;
	void *xmir_resp;
	size_t xmir_resp_size;
};

typedef	void (*mailbox_msg_cb_t)(void *arg, void *data, size_t len,
	u64 msgid, int err, bool sw_ch);
struct xrt_mailbox_ioctl_listen {
	mailbox_msg_cb_t xmil_cb;
	void *xmil_cb_arg;
};

#endif	/* _XRT_MAILBOX_H_ */
