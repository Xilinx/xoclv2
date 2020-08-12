/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_MAILBOX_H_
#define	_XOCL_MAILBOX_H_

typedef	void (*mailbox_msg_cb_t)(void *arg, void *data, size_t len,
	u64 msgid, int err, bool sw_ch);

enum mb_kind {
	DAEMON_STATE,
	CHAN_STATE,
	CHAN_SWITCH,
	COMM_ID,
	VERSION,
};

/*
 * Mailbox IP driver IOCTL calls.
 */
enum xocl_mailbox_ioctl_cmd {
	XOCL_MAILBOX_SET = 0,
};

#endif	/* _XOCL_MAILBOX_H_ */
