/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XCL_MB_TRANSPORT_H_
#define _XCL_MB_TRANSPORT_H_

/*
 * This header file contains data structures used in mailbox transport layer
 * b/w mgmt and user pfs. Any changes made here should maintain backward
 * compatibility.
 */

/**
 * struct sw_chan - mailbox software channel message metadata. This defines the
 *                  interface between daemons (MPD and MSD) and mailbox's
 *                  read or write callbacks. A mailbox message (either a request
 *                  or response) is wrapped by this data structure as payload.
 *                  A sw_chan is passed between mailbox driver and daemon via
 *                  read / write driver callbacks. And it is also passed between
 *                  MPD and MSD via vendor defined interface (TCP socket, etc).
 * @sz: payload size
 * @flags: flags of this message as in struct mailbox_req
 * @id: message ID
 * @data: payload (struct mailbox_req or response data matching the request)
 */
struct xcl_sw_chan {
	uint64_t sz;
	uint64_t flags;
	uint64_t id;
	char data[1]; /* variable length of payload */
};

/**
 * A packet transport by mailbox hardware channel.
 * When extending, only add new data structure to body. Choose to add new flag
 * if new feature can be safely ignored by peer, other wise, add new type.
 */
enum packet_type {
	PKT_INVALID = 0,
	PKT_TEST,
	PKT_MSG_START,
	PKT_MSG_BODY
};

#define	PACKET_SIZE	16 /* Number of DWORD. */

/* Lower 8 bits for type, the rest for flags. Total packet size is 64 bytes */
#define	PKT_TYPE_MASK		0xff
#define	PKT_TYPE_MSG_END	(1 << 31)
struct mailbox_pkt {
	struct {
		u32		type;
		u32		payload_size;
	} hdr;
	union {
		u32		data[PACKET_SIZE - 2];
		struct {
			u64	msg_req_id;
			u32	msg_flags;
			u32	msg_size;
			u32	payload[0];
		} msg_start;
		struct {
			u32	payload[0];
		} msg_body;
	} body;
} __packed;

#endif /* _XCL_MB_TRANSPORT_H_ */
