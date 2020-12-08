// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Mailbox IP Leaf Driver
 *
 * Copyright (C) 2016-2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

/**
 * DOC: Statement of Theory
 *
 * This is the mailbox sub-device driver added to xrt drivers so that user pf
 * and mgmt pf can send and receive messages of arbitrary length to / from its
 * peer. The driver is written based on the spec of PG114 document
 * (https://www.xilinx.com/support/documentation/ip_documentation/mailbox/v2_1/
 * pg114-mailbox.pdf). The HW provides one TX channel and one RX channel, which
 * operate completely independent of each other. Data can be pushed into or read
 * from a channel in DWORD unit as a FIFO.
 *
 *
 * Packet layer
 *
 * The driver implemented two transport layers - packet and message layer (see
 * below). A packet is a fixed size chunk of data that can be sent through TX
 * channel or retrieved from RX channel. The driver will not attempt to send
 * next packet until the previous one is read by peer. Similarly, the driver
 * will not attempt to read the data from HW until a full packet has been
 * written to HW by peer.
 *
 * Interrupt is not enabled and driver will poll the HW periodically to see if
 * FIFO is ready for reading or writing. When there is outstanding msg to be
 * sent or received, driver will poll at high frequency. Otherwise, driver polls
 * HW at very low frequency so that it will not consume much CPU cycles.
 *
 * A packet is defined as struct mailbox_pkt. There are mainly two types of
 * packets: start-of-msg and msg-body packets. Both can carry end-of-msg flag to
 * indicate that the packet is the last one in the current msg.
 *
 * The start-of-msg packet contains some meta data related to the entire msg,
 * such as msg ID, msg flags and msg size. Strictly speaking, these info belongs
 * to the msg layer, but it helps the receiving end to prepare buffer for the
 * incoming msg payload after seeing the 1st packet instead of the whole msg.
 * It is an optimization for msg receiving.
 *
 * The body-of-msg packet contains only msg payload.
 *
 *
 * Message layer
 *
 * A message is a data buffer of arbitrary length. The driver will break a
 * message into multiple packets and transmit them to the peer, which, in turn,
 * will assemble them into a full message before it's delivered to upper layer
 * for further processing. One message requires at least one packet to be
 * transferred to the peer (a start-of-msg packet with end-of-msg flag).
 *
 * Each message has a unique temporary u64 ID (see communication model below
 * for more detail). The ID shows up in start-of-msg packet only. So, at packet
 * layer, there is an assumption that adjacent packets belong to the same
 * message unless the next one is another start-of-msg packet. So, at message
 * layer, the driver will not attempt to send the next message until the
 * transmitting of current one is done. I.E., we implement a FIFO for message
 * TX channel. All messages are sent by driver in the order of received from
 * upper layer. We can implement msgs of different priority later, if needed.
 *
 * On the RX side, there is no certain order for receiving messages. It's up to
 * the peer to decide which message gets enqueued into its own TX queue first,
 * which will be received first on the other side.
 *
 * A TX message is considered as time'd out when it's transmit is not done
 * within 1 seconds. An RX msg is considered as time'd out 20 seconds after the
 * corresponding TX one has been sent out. There is no retry after msg time'd
 * out. The error will be simply propagated back to the upper layer.
 *
 * A msg is defined as struct mailbox_msg. It carrys a flag indicating that if
 * it's a msg of request or response msg. A response msg must have a big enough
 * msg buffer sitting in the receiver's RX queue waiting for it. A request msg
 * does not have a waiting msg buffer.
 *
 *
 * Communication layer
 *
 * At the highest layer, the driver implements a request-response communication
 * model. Three types of msgs can be sent/received in this model:
 *
 * - A request msg which requires a response.
 * - A notification msg which does not require a response.
 * - A response msg which is used to respond a request.
 *
 * The OP code of the request determines whether it's a request or notification.
 *
 * A request buffer or response buffer will be wrapped with a single msg. This
 * means that a session contains at most 2 msgs and the msg ID serves as the
 * session ID.
 *
 * A request or notification msg will automatically be assigned a msg ID when
 * it's enqueued into TX channel for transmitting. A response msg must match a
 * request msg by msg ID, or it'll be silently dropped. A communication session
 * starts with a request and finishes with 0 or 1 reponse, always.
 *
 * Currently, the driver implements one kernel thread for RX channel (RX thread)
 * , one for TX channel (TX thread) and one thread for processing incoming
 * request (REQ thread).
 *
 * The RX thread is responsible for receiving incoming msgs. If it's a request
 * or notification msg, it'll punt it to REQ thread for processing, which, in
 * turn, will call the callback provided by mgmt pf driver or user pf driver to
 * further process it. If it's a response, it'll simply wake up the waiting
 * thread.
 *
 * The TX thread is responsible for sending out msgs. When it's done, the TX
 * thread will simply wake up the waiting thread.
 *
 *
 * Software communication channel
 *
 * A msg can be sent or received through HW mailbox channel or through a daemon
 * implemented in user land (software communication daemon). The daemon waiting
 * for sending msg from user pf to mgmt pf is called MPD. The other one is MSD,
 * which is responsible for sending msg from mgmt pf to user pf.
 *
 * Each mailbox subdevice driver creates a device node under /dev. A daemon
 * (MPD or MSD) can block and wait in the read() interface waiting for fetching
 * out-going msg sent to peer. Or it can block and wait in the poll()/select()
 * interface and will be woken up when there is an out-going msg ready to be
 * sent. Then it can fetch the msg via read() interface. It's entirely up to the
 * daemon to process the msg. It may pass it through to the peer or handle it
 * completely in its own way.
 *
 * If the daemon wants to pass a msg (request or response) to a mailbox driver,
 * it can do so by calling write() driver interface. It may block and wait until
 * the previous msg is consumed by the RX thread before it can finish
 * transmiting its own msg and return back to user land.
 *
 *
 * Communication protocols
 *
 * As indicated above, the packet layer and msg layer communication protocol is
 * defined as struct mailbox_pkt and struct mailbox_msg respectively in this
 * file. The protocol for communicating at communication layer is defined in
 * mailbox_proto.h.
 *
 * The software communication channel communicates at communication layer only,
 * which sees only request and response buffers. It should only implement the
 * protocol defined in mailbox_proto.h.
 *
 * The current protocol defined at communication layer followed a rule as below:
 * All requests initiated from user pf requires a response and all requests from
 * mgmt pf does not require a response. This should avoid any possible deadlock
 * derived from each side blocking and waiting for response from the peer.
 *
 * The overall architecture can be shown as below::
 *
 *             +----------+      +----------+            +----------+
 *             [ Req/Resp ]  <---[SW Channel]---->       [ Req/Resp ]
 *       +-----+----------+      +----------+      +-----+----------+
 *       [ Msg | Req/Resp ]                        [ Msg | Req/Resp ]
 *       +---+-+------+---+      +----------+      +---+-+-----+----+
 *       [Pkt]...[]...[Pkt]  <---[HW Channel]----> [Pkt]...[]...[Pkt]
 *       +---+        +---+      +----------+      +---+        +---+
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/poll.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/crc32c.h>
#include <linux/xrt/mailbox_transport.h>
#include "xrt-metadata.h"
#include "xrt-subdev.h"
#include "xrt-mailbox.h"
#include "xmgmt-main.h"

#define	FLAG_STI	(1 << 0)
#define	FLAG_RTI	(1 << 1)

#define	STATUS_EMPTY	(1 << 0)
#define	STATUS_FULL	(1 << 1)
#define	STATUS_STA	(1 << 2)
#define	STATUS_RTA	(1 << 3)
#define	STATUS_VALID	(STATUS_EMPTY|STATUS_FULL|STATUS_STA|STATUS_RTA)

#define	MBX_ERR(mbx, fmt, arg...) xrt_err(mbx->mbx_pdev, fmt "\n", ##arg)
#define	MBX_WARN(mbx, fmt, arg...) xrt_warn(mbx->mbx_pdev, fmt "\n", ##arg)
#define	MBX_INFO(mbx, fmt, arg...) xrt_info(mbx->mbx_pdev, fmt "\n", ##arg)
#define	MBX_DBG(mbx, fmt, arg...) xrt_dbg(mbx->mbx_pdev, fmt "\n", ##arg)

#define	MAILBOX_TTL_TIMER	(HZ / 10) /* in jiffies */
#define	MAILBOX_SEC2TTL(s)	((s) * HZ / MAILBOX_TTL_TIMER)
#define	MSG_MAX_TTL		INT_MAX /* used to disable TTL checking */

#define	INVALID_MSG_ID		((u64)-1)

#define	MAX_MSG_QUEUE_LEN	5
#define	MAX_REQ_MSG_SZ		(1024 * 1024)

#define MBX_SW_ONLY(mbx) ((mbx)->mbx_regs == NULL)
/*
 * Mailbox IP register layout
 */
struct mailbox_reg {
	u32			mbr_wrdata;
	u32			mbr_resv1;
	u32			mbr_rddata;
	u32			mbr_resv2;
	u32			mbr_status;
	u32			mbr_error;
	u32			mbr_sit;
	u32			mbr_rit;
	u32			mbr_is;
	u32			mbr_ie;
	u32			mbr_ip;
	u32			mbr_ctrl;
} __packed;

/*
 * A message transport by mailbox.
 */
#define MSG_FLAG_RESPONSE	(1 << 0)
#define MSG_FLAG_REQUEST	(1 << 1)
struct mailbox_msg {
	struct list_head	mbm_list;
	struct mailbox_channel	*mbm_ch;
	u64			mbm_req_id;
	char			*mbm_data;
	size_t			mbm_len;
	int			mbm_error;
	struct completion	mbm_complete;
	mailbox_msg_cb_t	mbm_cb;
	void			*mbm_cb_arg;
	u32			mbm_flags;
	atomic_t		mbm_ttl;
	bool			mbm_chan_sw;

	/* Statistics for debugging. */
	u64			mbm_num_pkts;
	u64			mbm_start_ts;
	u64			mbm_end_ts;
};

/* Mailbox communication channel state. */
#define MBXCS_BIT_READY		0
#define MBXCS_BIT_STOP		1
#define MBXCS_BIT_TICK		2

enum mailbox_chan_type {
	MBXCT_RX,
	MBXCT_TX
};

struct mailbox_channel;
typedef	bool (*chan_func_t)(struct mailbox_channel *ch);
struct mailbox_channel {
	struct mailbox		*mbc_parent;
	enum mailbox_chan_type	mbc_type;

	struct workqueue_struct	*mbc_wq;
	struct work_struct	mbc_work;
	struct completion	mbc_worker;
	chan_func_t		mbc_tran;
	unsigned long		mbc_state;

	struct mutex		mbc_mutex;
	struct list_head	mbc_msgs;

	struct mailbox_msg	*mbc_cur_msg;
	int			mbc_bytes_done;
	struct mailbox_pkt	mbc_packet;

	/*
	 * Software channel settings
	 */
	wait_queue_head_t	sw_chan_wq;
	struct mutex		sw_chan_mutex;
	void			*sw_chan_buf;
	size_t			sw_chan_buf_sz;
	uint64_t		sw_chan_msg_id;
	uint64_t		sw_chan_msg_flags;

	atomic_t		sw_num_pending_msg;
};

/*
 * The mailbox softstate.
 */
struct mailbox {
	struct platform_device	*mbx_pdev;
	struct timer_list	mbx_poll_timer;
	struct mailbox_reg	*mbx_regs;

	struct mailbox_channel	mbx_rx;
	struct mailbox_channel	mbx_tx;

	/* For listening to peer's request. */
	mailbox_msg_cb_t	mbx_listen_cb;
	void			*mbx_listen_cb_arg;
	struct mutex		mbx_listen_cb_lock;
	struct workqueue_struct	*mbx_listen_wq;
	struct work_struct	mbx_listen_worker;

	/*
	 * For testing basic intr and mailbox comm functionality via sysfs.
	 * No locking protection, use with care.
	 */
	struct mailbox_pkt	mbx_tst_pkt;

	/* Req list for all incoming request message */
	struct completion	mbx_comp;
	struct mutex		mbx_lock;
	struct list_head	mbx_req_list;
	uint32_t		mbx_req_cnt;
	bool			mbx_listen_stop;

	bool			mbx_peer_dead;
	uint64_t		mbx_opened;
};

static inline const char *reg2name(struct mailbox *mbx, u32 *reg)
{
	static const char * const reg_names[] = {
		"wrdata",
		"reserved1",
		"rddata",
		"reserved2",
		"status",
		"error",
		"sit",
		"rit",
		"is",
		"ie",
		"ip",
		"ctrl"
	};

	return reg_names[((uintptr_t)reg -
		(uintptr_t)mbx->mbx_regs) / sizeof(u32)];
}

static inline u32 mailbox_reg_rd(struct mailbox *mbx, u32 *reg)
{
	u32 val = ioread32(reg);

#ifdef	MAILBOX_REG_DEBUG
	MBX_DBG(mbx, "REG_RD(%s)=0x%x", reg2name(mbx, reg), val);
#endif
	return val;
}

static inline void mailbox_reg_wr(struct mailbox *mbx, u32 *reg, u32 val)
{
#ifdef	MAILBOX_REG_DEBUG
	MBX_DBG(mbx, "REG_WR(%s, 0x%x)", reg2name(mbx, reg), val);
#endif
	iowrite32(val, reg);
}

static inline void reset_pkt(struct mailbox_pkt *pkt)
{
	pkt->hdr.type = PKT_INVALID;
}

static inline bool valid_pkt(struct mailbox_pkt *pkt)
{
	return (pkt->hdr.type != PKT_INVALID);
}

static inline bool is_rx_chan(struct mailbox_channel *ch)
{
	return ch->mbc_type == MBXCT_RX;
}

static inline char *ch_name(struct mailbox_channel *ch)
{
	return is_rx_chan(ch) ? "RX" : "TX";
}

static bool is_rx_msg(struct mailbox_msg *msg)
{
	return is_rx_chan(msg->mbm_ch);
}

static void chan_tick(struct mailbox_channel *ch)
{
	mutex_lock(&ch->mbc_mutex);

	set_bit(MBXCS_BIT_TICK, &ch->mbc_state);
	complete(&ch->mbc_worker);

	mutex_unlock(&ch->mbc_mutex);
}

static void mailbox_poll_timer(struct timer_list *t)
{
	struct mailbox *mbx = from_timer(mbx, t, mbx_poll_timer);

	chan_tick(&mbx->mbx_tx);
	chan_tick(&mbx->mbx_rx);

	/* We're a periodic timer. */
	mutex_lock(&mbx->mbx_lock);
	mod_timer(&mbx->mbx_poll_timer, jiffies + MAILBOX_TTL_TIMER);
	mutex_unlock(&mbx->mbx_lock);
}

static void free_msg(struct mailbox_msg *msg)
{
	vfree(msg);
}

static void msg_done(struct mailbox_msg *msg, int err)
{
	struct mailbox_channel *ch = msg->mbm_ch;
	struct mailbox *mbx = ch->mbc_parent;
	u64 elapsed = (msg->mbm_end_ts - msg->mbm_start_ts) / 1000; /* in us. */

	MBX_INFO(ch->mbc_parent,
		"msg(id=0x%llx sz=%ldB crc=0x%x): %s %lldpkts in %lldus: %d",
		msg->mbm_req_id, msg->mbm_len,
		crc32c_le(~0, msg->mbm_data, msg->mbm_len),
		ch_name(ch), msg->mbm_num_pkts, elapsed, err);

	msg->mbm_error = err;

	if (msg->mbm_cb) {
		msg->mbm_cb(msg->mbm_cb_arg, msg->mbm_data, msg->mbm_len,
			msg->mbm_req_id, msg->mbm_error, msg->mbm_chan_sw);
		free_msg(msg);
		return;
	}

	if (is_rx_msg(msg) && (msg->mbm_flags & MSG_FLAG_REQUEST)) {
		if (err) {
			MBX_WARN(mbx, "Time'd out receiving full req message");
			free_msg(msg);
		} else if (mbx->mbx_req_cnt >= MAX_MSG_QUEUE_LEN) {
			MBX_WARN(mbx, "Too many pending req messages, dropped");
			free_msg(msg);
		} else {
			mutex_lock(&ch->mbc_parent->mbx_lock);
			list_add_tail(&msg->mbm_list,
				&ch->mbc_parent->mbx_req_list);
			mbx->mbx_req_cnt++;
			mutex_unlock(&ch->mbc_parent->mbx_lock);
			complete(&ch->mbc_parent->mbx_comp);
		}
	} else {
		complete(&msg->mbm_complete);
	}
}

static void reset_sw_ch(struct mailbox_channel *ch)
{
	BUG_ON(!mutex_is_locked(&ch->sw_chan_mutex));

	vfree(ch->sw_chan_buf);
	ch->sw_chan_buf = NULL;
	ch->sw_chan_buf_sz = 0;
	ch->sw_chan_msg_flags = 0;
	ch->sw_chan_msg_id = 0;
	atomic_dec_if_positive(&ch->sw_num_pending_msg);
}

static void reset_hw_ch(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;

	if (!mbx->mbx_regs)
		return;

	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_ctrl,
		is_rx_chan(ch) ? 0x2 : 0x1);
}

static void chan_msg_done(struct mailbox_channel *ch, int err)
{
	if (!ch->mbc_cur_msg)
		return;

	ch->mbc_cur_msg->mbm_end_ts = ktime_get_ns();
	if (err) {
		if (ch->mbc_cur_msg->mbm_chan_sw) {
			mutex_lock(&ch->sw_chan_mutex);
			reset_sw_ch(ch);
			mutex_unlock(&ch->sw_chan_mutex);
		} else {
			reset_hw_ch(ch);
		}
	}

	msg_done(ch->mbc_cur_msg, err);
	ch->mbc_cur_msg = NULL;
	ch->mbc_bytes_done = 0;
}

void timeout_msg(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;
	struct mailbox_msg *msg = NULL;
	struct list_head *pos, *n;
	struct list_head l = LIST_HEAD_INIT(l);

	/* Check outstanding msg first. */
	msg = ch->mbc_cur_msg;
	if (msg) {
		if (atomic_dec_if_positive(&msg->mbm_ttl) < 0) {
			MBX_WARN(mbx, "found outstanding msg time'd out");
			if (!mbx->mbx_peer_dead) {
				MBX_WARN(mbx, "peer becomes dead");
				/* Peer is not active any more. */
				mbx->mbx_peer_dead = true;
			}
			chan_msg_done(ch, -ETIMEDOUT);
		}
	}

	mutex_lock(&ch->mbc_mutex);

	list_for_each_safe(pos, n, &ch->mbc_msgs) {
		msg = list_entry(pos, struct mailbox_msg, mbm_list);
		if (atomic_dec_if_positive(&msg->mbm_ttl) < 0) {
			list_del(&msg->mbm_list);
			list_add_tail(&msg->mbm_list, &l);
		}
	}

	mutex_unlock(&ch->mbc_mutex);

	if (!list_empty(&l))
		MBX_ERR(mbx, "found awaiting msg time'd out");

	list_for_each_safe(pos, n, &l) {
		msg = list_entry(pos, struct mailbox_msg, mbm_list);
		list_del(&msg->mbm_list);
		msg_done(msg, -ETIMEDOUT);
	}
}

static void msg_timer_on(struct mailbox_msg *msg, u32 ttl)
{
	atomic_set(&msg->mbm_ttl, MAILBOX_SEC2TTL(ttl));
}

/*
 * Reset TTL for outstanding msg. Next portion of the msg is expected to
 * arrive or go out before it times out.
 */
static void outstanding_msg_ttl_reset(struct mailbox_channel *ch)
{
	struct mailbox_msg *msg = ch->mbc_cur_msg;

	if (!msg)
		return;

	// outstanding msg will time out if no progress is made within 1 second.
	msg_timer_on(msg, 1);
}

static void handle_timer_event(struct mailbox_channel *ch)
{
	if (!test_bit(MBXCS_BIT_TICK, &ch->mbc_state))
		return;
	timeout_msg(ch);
	clear_bit(MBXCS_BIT_TICK, &ch->mbc_state);
}

static void chan_worker(struct work_struct *work)
{
	struct mailbox_channel *ch =
		container_of(work, struct mailbox_channel, mbc_work);
	struct mailbox *mbx = ch->mbc_parent;
	bool progress;

	while (!test_bit(MBXCS_BIT_STOP, &ch->mbc_state)) {
		if (ch->mbc_cur_msg) {
			// fast poll (1000/s) to finish outstanding msg
			usleep_range(1000, 2000);
		} else {
			// Wait for next poll timer trigger
			wait_for_completion_interruptible(&ch->mbc_worker);
		}

		progress = ch->mbc_tran(ch);
		if (progress) {
			outstanding_msg_ttl_reset(ch);
			if (mbx->mbx_peer_dead) {
				MBX_INFO(mbx, "peer becomes active");
				mbx->mbx_peer_dead = false;
			}
		}

		handle_timer_event(ch);
	}
}

static inline u32 mailbox_chk_err(struct mailbox *mbx)
{
	u32 val = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_error);

	/* Ignore bad register value after firewall is tripped. */
	if (val == 0xffffffff)
		val = 0;

	/* Error should not be seen, shout when found. */
	if (val)
		MBX_ERR(mbx, "mailbox error detected, error=0x%x", val);
	return val;
}

static int chan_msg_enqueue(struct mailbox_channel *ch, struct mailbox_msg *msg)
{
	int rv = 0;

	MBX_DBG(ch->mbc_parent, "%s enqueuing msg, id=0x%llx",
		ch_name(ch), msg->mbm_req_id);

	BUG_ON(msg->mbm_req_id == INVALID_MSG_ID);

	mutex_lock(&ch->mbc_mutex);
	if (test_bit(MBXCS_BIT_STOP, &ch->mbc_state)) {
		rv = -ESHUTDOWN;
	} else {
		list_add_tail(&msg->mbm_list, &ch->mbc_msgs);
		msg->mbm_ch = ch;
	}
	mutex_unlock(&ch->mbc_mutex);

	return rv;
}

static struct mailbox_msg *chan_msg_dequeue(struct mailbox_channel *ch,
	u64 req_id)
{
	struct mailbox_msg *msg = NULL;
	struct list_head *pos;

	mutex_lock(&ch->mbc_mutex);

	/* Take the first msg. */
	if (req_id == INVALID_MSG_ID) {
		msg = list_first_entry_or_null(&ch->mbc_msgs,
		struct mailbox_msg, mbm_list);
	/* Take the msg w/ specified ID. */
	} else {
		list_for_each(pos, &ch->mbc_msgs) {
			struct mailbox_msg *temp;

			temp = list_entry(pos, struct mailbox_msg, mbm_list);
			if (temp->mbm_req_id == req_id) {
				msg = temp;
				break;
			}
		}
	}

	if (msg) {
		MBX_DBG(ch->mbc_parent, "%s dequeued msg, id=0x%llx",
			ch_name(ch), msg->mbm_req_id);
		list_del(&msg->mbm_list);
	}

	mutex_unlock(&ch->mbc_mutex);
	return msg;
}

static struct mailbox_msg *alloc_msg(void *buf, size_t len)
{
	char *newbuf = NULL;
	struct mailbox_msg *msg = NULL;
	/* Give MB*2 secs as time to live */

	if (!buf) {
		msg = vzalloc(sizeof(struct mailbox_msg) + len);
		if (!msg)
			return NULL;
		newbuf = ((char *)msg) + sizeof(struct mailbox_msg);
	} else {
		msg = vzalloc(sizeof(struct mailbox_msg));
		if (!msg)
			return NULL;
		newbuf = buf;
	}

	INIT_LIST_HEAD(&msg->mbm_list);
	msg->mbm_data = newbuf;
	msg->mbm_len = len;
	atomic_set(&msg->mbm_ttl, MSG_MAX_TTL);
	msg->mbm_chan_sw = false;
	init_completion(&msg->mbm_complete);

	return msg;
}

static void chan_fini(struct mailbox_channel *ch)
{
	struct mailbox_msg *msg;

	if (!ch->mbc_parent)
		return;

	/*
	 * Holding mutex to ensure no new msg is enqueued after
	 * flag is set.
	 */
	mutex_lock(&ch->mbc_mutex);
	set_bit(MBXCS_BIT_STOP, &ch->mbc_state);
	mutex_unlock(&ch->mbc_mutex);

	if (ch->mbc_wq) {
		complete(&ch->mbc_worker);
		cancel_work_sync(&ch->mbc_work);
		destroy_workqueue(ch->mbc_wq);
	}

	mutex_lock(&ch->sw_chan_mutex);
	if (ch->sw_chan_buf != NULL)
		vfree(ch->sw_chan_buf);
	mutex_unlock(&ch->sw_chan_mutex);

	msg = ch->mbc_cur_msg;
	if (msg)
		chan_msg_done(ch, -ESHUTDOWN);

	while ((msg = chan_msg_dequeue(ch, INVALID_MSG_ID)) != NULL)
		msg_done(msg, -ESHUTDOWN);

	mutex_destroy(&ch->mbc_mutex);
	mutex_destroy(&ch->sw_chan_mutex);
	ch->mbc_parent = NULL;
}

static int chan_init(struct mailbox *mbx, enum mailbox_chan_type type,
	struct mailbox_channel *ch, chan_func_t fn)
{
	ch->mbc_parent = mbx;
	ch->mbc_type = type;
	ch->mbc_tran = fn;
	INIT_LIST_HEAD(&ch->mbc_msgs);
	init_completion(&ch->mbc_worker);
	mutex_init(&ch->mbc_mutex);
	mutex_init(&ch->sw_chan_mutex);

	init_waitqueue_head(&ch->sw_chan_wq);
	atomic_set(&ch->sw_num_pending_msg, 0);
	ch->mbc_cur_msg = NULL;
	ch->mbc_bytes_done = 0;

	/* Reset pkt buffer. */
	reset_pkt(&ch->mbc_packet);
	/* Reset HW channel. */
	reset_hw_ch(ch);
	/* Reset SW channel. */
	mutex_lock(&ch->sw_chan_mutex);
	reset_sw_ch(ch);
	mutex_unlock(&ch->sw_chan_mutex);

	/* One thread for one channel. */
	ch->mbc_wq =
		create_singlethread_workqueue(dev_name(&mbx->mbx_pdev->dev));
	if (!ch->mbc_wq) {
		chan_fini(ch);
		return -ENOMEM;
	}
	INIT_WORK(&ch->mbc_work, chan_worker);

	/* Kick off channel thread, all initialization should be done by now. */
	clear_bit(MBXCS_BIT_STOP, &ch->mbc_state);
	set_bit(MBXCS_BIT_READY, &ch->mbc_state);
	queue_work(ch->mbc_wq, &ch->mbc_work);
	return 0;
}

static void listen_wq_fini(struct mailbox *mbx)
{
	BUG_ON(mbx == NULL);

	if (mbx->mbx_listen_wq != NULL) {
		mbx->mbx_listen_stop = true;
		complete(&mbx->mbx_comp);
		cancel_work_sync(&mbx->mbx_listen_worker);
		destroy_workqueue(mbx->mbx_listen_wq);
		mbx->mbx_listen_wq = NULL;
	}
}

static void chan_recv_pkt(struct mailbox_channel *ch)
{
	int i, retry = 10;
	struct mailbox *mbx = ch->mbc_parent;
	struct mailbox_pkt *pkt = &ch->mbc_packet;

	BUG_ON(valid_pkt(pkt));

	/* Picking up a packet from HW. */
	for (i = 0; i < PACKET_SIZE; i++) {
		while ((mailbox_reg_rd(mbx,
			&mbx->mbx_regs->mbr_status) & STATUS_EMPTY) &&
			(retry-- > 0))
			msleep(100);

		*(((u32 *)pkt) + i) =
			mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_rddata);
	}
	if ((mailbox_chk_err(mbx) & STATUS_EMPTY) != 0)
		reset_pkt(pkt);
	else
		MBX_DBG(mbx, "received pkt: type=0x%x", pkt->hdr.type);
}

static void chan_send_pkt(struct mailbox_channel *ch)
{
	int i;
	struct mailbox *mbx = ch->mbc_parent;
	struct mailbox_pkt *pkt = &ch->mbc_packet;

	BUG_ON(!valid_pkt(pkt));

	MBX_DBG(mbx, "sending pkt: type=0x%x", pkt->hdr.type);

	/* Pushing a packet into HW. */
	for (i = 0; i < PACKET_SIZE; i++) {
		mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_wrdata,
			*(((u32 *)pkt) + i));
	}

	reset_pkt(pkt);
	if (ch->mbc_cur_msg)
		ch->mbc_bytes_done += ch->mbc_packet.hdr.payload_size;

	BUG_ON((mailbox_chk_err(mbx) & STATUS_FULL) != 0);
}

static int chan_pkt2msg(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;
	void *msg_data, *pkt_data;
	struct mailbox_msg *msg = ch->mbc_cur_msg;
	struct mailbox_pkt *pkt = &ch->mbc_packet;
	size_t cnt = pkt->hdr.payload_size;
	u32 type = (pkt->hdr.type & PKT_TYPE_MASK);

	BUG_ON(((type != PKT_MSG_START) && (type != PKT_MSG_BODY)) || !msg);

	if (type == PKT_MSG_START) {
		msg->mbm_req_id = pkt->body.msg_start.msg_req_id;
		BUG_ON(msg->mbm_len < pkt->body.msg_start.msg_size);
		msg->mbm_len = pkt->body.msg_start.msg_size;
		pkt_data = pkt->body.msg_start.payload;
	} else {
		pkt_data = pkt->body.msg_body.payload;
	}

	if (cnt > msg->mbm_len - ch->mbc_bytes_done) {
		MBX_ERR(mbx, "invalid mailbox packet size");
		return -EBADMSG;
	}

	msg_data = msg->mbm_data + ch->mbc_bytes_done;
	(void) memcpy(msg_data, pkt_data, cnt);
	ch->mbc_bytes_done += cnt;
	msg->mbm_num_pkts++;

	reset_pkt(pkt);
	return 0;
}

/* Prepare outstanding msg for receiving incoming msg. */
static void dequeue_rx_msg(struct mailbox_channel *ch,
	u32 flags, u64 id, size_t sz)
{
	struct mailbox *mbx = ch->mbc_parent;
	struct mailbox_msg *msg = NULL;
	int err = 0;

	if (ch->mbc_cur_msg)
		return;

	if (flags & MSG_FLAG_RESPONSE) {
		msg = chan_msg_dequeue(ch, id);
		if (!msg) {
			MBX_ERR(mbx, "Failed to find msg (id 0x%llx)", id);
		} else if (msg->mbm_len < sz) {
			MBX_ERR(mbx, "Response (id 0x%llx) is too big: %lu",
				id, sz);
			err = -EMSGSIZE;
		}
	} else if (flags & MSG_FLAG_REQUEST) {
		if (sz < MAX_REQ_MSG_SZ)
			msg = alloc_msg(NULL, sz);
		if (msg) {
			msg->mbm_req_id = id;
			msg->mbm_ch = ch;
			msg->mbm_flags = flags;
		} else {
			MBX_ERR(mbx, "req msg len %luB is too big", sz);
		}
	} else {
		/* Not a request or response? */
		MBX_ERR(mbx, "Invalid incoming msg flags: 0x%x", flags);
	}

	if (msg) {
		msg->mbm_start_ts = ktime_get_ns();
		msg->mbm_num_pkts = 0;
		ch->mbc_cur_msg = msg;
	}

	/* Fail received msg now on error. */
	if (err)
		chan_msg_done(ch, err);
}

static bool do_sw_rx(struct mailbox_channel *ch)
{
	u32 flags = 0;
	u64 id = 0;
	size_t len = 0;

	/*
	 * Don't receive new msg when a msg is being received from HW
	 * for simplicity.
	 */
	if (ch->mbc_cur_msg)
		return false;

	mutex_lock(&ch->sw_chan_mutex);

	flags = ch->sw_chan_msg_flags;
	id = ch->sw_chan_msg_id;
	len = ch->sw_chan_buf_sz;

	mutex_unlock(&ch->sw_chan_mutex);

	/* Nothing to receive. */
	if (id == 0)
		return false;

	/* Prepare outstanding msg. */
	dequeue_rx_msg(ch, flags, id, len);

	mutex_lock(&ch->sw_chan_mutex);

	BUG_ON(id != ch->sw_chan_msg_id);

	if (ch->mbc_cur_msg) {
		ch->mbc_cur_msg->mbm_chan_sw = true;
		memcpy(ch->mbc_cur_msg->mbm_data,
			ch->sw_chan_buf, ch->sw_chan_buf_sz);
	}

	/* Done with sw msg. */
	reset_sw_ch(ch);

	mutex_unlock(&ch->sw_chan_mutex);

	wake_up_interruptible(&ch->sw_chan_wq);

	chan_msg_done(ch, 0);

	return true;
}

static bool do_hw_rx(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;
	struct mailbox_pkt *pkt = &ch->mbc_packet;
	u32 type;
	bool eom = false, read_hw = false;
	u32 st = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_status);
	bool progress = false;

	/* Check if a packet is ready for reading. */
	if (st & ~STATUS_VALID) {
		/* Device is still being reset or firewall tripped. */
		read_hw = false;
	} else {
		read_hw = ((st & STATUS_RTA) != 0);
	}

	if (!read_hw)
		return progress;

	chan_recv_pkt(ch);
	type = pkt->hdr.type & PKT_TYPE_MASK;
	eom = ((pkt->hdr.type & PKT_TYPE_MSG_END) != 0);

	switch (type) {
	case PKT_TEST:
		(void) memcpy(&mbx->mbx_tst_pkt, &ch->mbc_packet,
			sizeof(struct mailbox_pkt));
		reset_pkt(pkt);
		break;
	case PKT_MSG_START:
		if (ch->mbc_cur_msg) {
			MBX_ERR(mbx, "Received partial msg (id 0x%llx)",
				ch->mbc_cur_msg->mbm_req_id);
			chan_msg_done(ch, -EBADMSG);
		}
		/* Prepare outstanding msg. */
		dequeue_rx_msg(ch, pkt->body.msg_start.msg_flags,
			pkt->body.msg_start.msg_req_id,
			pkt->body.msg_start.msg_size);
		if (!ch->mbc_cur_msg) {
			MBX_ERR(mbx, "got unexpected msg start pkt");
			reset_pkt(pkt);
		}
		break;
	case PKT_MSG_BODY:
		if (!ch->mbc_cur_msg) {
			MBX_ERR(mbx, "got unexpected msg body pkt");
			reset_pkt(pkt);
		}
		break;
	default:
		MBX_ERR(mbx, "invalid mailbox pkt type");
		reset_pkt(pkt);
		break;
	}

	if (valid_pkt(pkt)) {
		int err = chan_pkt2msg(ch);

		if (err || eom)
			chan_msg_done(ch, err);
		progress = true;
	}

	return progress;
}

/*
 * Worker for RX channel.
 */
static bool chan_do_rx(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;
	bool progress = false;

	progress = do_sw_rx(ch);
	if (!MBX_SW_ONLY(mbx))
		progress |= do_hw_rx(ch);

	return progress;
}

static void chan_msg2pkt(struct mailbox_channel *ch)
{
	size_t cnt = 0;
	size_t payload_off = 0;
	void *msg_data, *pkt_data;
	struct mailbox_msg *msg = ch->mbc_cur_msg;
	struct mailbox_pkt *pkt = &ch->mbc_packet;
	bool is_start = (ch->mbc_bytes_done == 0);
	bool is_eom = false;

	if (is_start) {
		payload_off = offsetof(struct mailbox_pkt,
			body.msg_start.payload);
	} else {
		payload_off = offsetof(struct mailbox_pkt,
			body.msg_body.payload);
	}
	cnt = PACKET_SIZE * sizeof(u32) - payload_off;
	if (cnt >= msg->mbm_len - ch->mbc_bytes_done) {
		cnt = msg->mbm_len - ch->mbc_bytes_done;
		is_eom = true;
	}

	pkt->hdr.type = is_start ? PKT_MSG_START : PKT_MSG_BODY;
	pkt->hdr.type |= is_eom ? PKT_TYPE_MSG_END : 0;
	pkt->hdr.payload_size = cnt;

	if (is_start) {
		pkt->body.msg_start.msg_req_id = msg->mbm_req_id;
		pkt->body.msg_start.msg_size = msg->mbm_len;
		pkt->body.msg_start.msg_flags = msg->mbm_flags;
		pkt_data = pkt->body.msg_start.payload;
	} else {
		pkt_data = pkt->body.msg_body.payload;
	}
	msg_data = msg->mbm_data + ch->mbc_bytes_done;
	(void) memcpy(pkt_data, msg_data, cnt);
}

static void do_sw_tx(struct mailbox_channel *ch)
{
	mutex_lock(&ch->sw_chan_mutex);

	BUG_ON(ch->mbc_cur_msg == NULL || !ch->mbc_cur_msg->mbm_chan_sw);
	BUG_ON(ch->sw_chan_msg_id != 0);

	ch->sw_chan_buf = vmalloc(ch->mbc_cur_msg->mbm_len);
	if (!ch->sw_chan_buf) {
		mutex_unlock(&ch->sw_chan_mutex);
		return;
	}

	ch->sw_chan_buf_sz = ch->mbc_cur_msg->mbm_len;
	ch->sw_chan_msg_id = ch->mbc_cur_msg->mbm_req_id;
	ch->sw_chan_msg_flags = ch->mbc_cur_msg->mbm_flags;
	(void) memcpy(ch->sw_chan_buf, ch->mbc_cur_msg->mbm_data,
		ch->sw_chan_buf_sz);
	ch->mbc_bytes_done = ch->mbc_cur_msg->mbm_len;

	/* Notify sw tx channel handler. */
	atomic_inc(&ch->sw_num_pending_msg);

	mutex_unlock(&ch->sw_chan_mutex);
	wake_up_interruptible(&ch->sw_chan_wq);
}

static void do_hw_tx(struct mailbox_channel *ch)
{
	BUG_ON(ch->mbc_cur_msg == NULL || ch->mbc_cur_msg->mbm_chan_sw);
	chan_msg2pkt(ch);
	chan_send_pkt(ch);
}

/* Prepare outstanding msg for sending outgoing msg. */
static void dequeue_tx_msg(struct mailbox_channel *ch)
{
	if (ch->mbc_cur_msg)
		return;

	ch->mbc_cur_msg = chan_msg_dequeue(ch, INVALID_MSG_ID);
	if (ch->mbc_cur_msg) {
		ch->mbc_cur_msg->mbm_start_ts = ktime_get_ns();
		ch->mbc_cur_msg->mbm_num_pkts = 0;
	}
}

/* Check if HW TX channel is ready for next msg. */
static bool tx_hw_chan_ready(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;
	u32 st;

	st = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_status);
	return ((st != 0xffffffff) && ((st & STATUS_STA) != 0));
}

/* Check if SW TX channel is ready for next msg. */
static bool tx_sw_chan_ready(struct mailbox_channel *ch)
{
	bool ready;

	mutex_lock(&ch->sw_chan_mutex);
	ready = (ch->sw_chan_msg_id == 0);
	mutex_unlock(&ch->sw_chan_mutex);
	return ready;
}

/*
 * Worker for TX channel.
 */
static bool chan_do_tx(struct mailbox_channel *ch)
{
	struct mailbox_msg *curmsg = ch->mbc_cur_msg;
	bool progress = false;

	/* Check if current outstanding msg is fully sent. */
	if (curmsg) {
		bool done = curmsg->mbm_chan_sw ? tx_sw_chan_ready(ch) :
			tx_hw_chan_ready(ch);
		if (done) {
			curmsg->mbm_num_pkts++;
			if (curmsg->mbm_len == ch->mbc_bytes_done)
				chan_msg_done(ch, 0);
			progress = true;
		}
	}

	dequeue_tx_msg(ch);
	curmsg = ch->mbc_cur_msg;

	/* Send the next msg out. */
	if (curmsg) {
		if (curmsg->mbm_chan_sw) {
			if (tx_sw_chan_ready(ch)) {
				do_sw_tx(ch);
				progress = true;
			}
		} else {
			if (tx_hw_chan_ready(ch)) {
				do_hw_tx(ch);
				progress = true;
			}
		}
	}

	return progress;
}

static ssize_t mailbox_ctl_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	u32 *reg = (u32 *)mbx->mbx_regs;
	int r, n;
	int nreg = sizeof(struct mailbox_reg) / sizeof(u32);

	if (MBX_SW_ONLY(mbx))
		return 0;

	for (r = 0, n = 0; r < nreg; r++, reg++) {
		/* Non-status registers. */
		if ((reg == &mbx->mbx_regs->mbr_resv1)		||
			(reg == &mbx->mbx_regs->mbr_wrdata)	||
			(reg == &mbx->mbx_regs->mbr_rddata)	||
			(reg == &mbx->mbx_regs->mbr_resv2))
			continue;
		/* Write-only status register. */
		if (reg == &mbx->mbx_regs->mbr_ctrl) {
			n += sprintf(buf + n, "%02ld %10s = --",
				r * sizeof(u32), reg2name(mbx, reg));
		/* Read-able status register. */
		} else {
			n += sprintf(buf + n, "%02ld %10s = 0x%08x",
				r * sizeof(u32), reg2name(mbx, reg),
				mailbox_reg_rd(mbx, reg));
		}
	}

	return n;
}
static ssize_t mailbox_ctl_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	u32 off, val;
	int nreg = sizeof(struct mailbox_reg) / sizeof(u32);
	u32 *reg = (u32 *)mbx->mbx_regs;

	if (MBX_SW_ONLY(mbx))
		return count;

	if (sscanf(buf, "%d:%d", &off, &val) != 2 || (off % sizeof(u32)) ||
		off >= nreg * sizeof(u32)) {
		MBX_ERR(mbx, "input should be < reg_offset:reg_val>");
		return -EINVAL;
	}
	reg += off / sizeof(u32);

	mailbox_reg_wr(mbx, reg, val);
	return count;
}
/* HW register level debugging i/f. */
static DEVICE_ATTR_RW(mailbox_ctl);

static ssize_t mailbox_pkt_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	struct mailbox_pkt *pkt = &mbx->mbx_tst_pkt;
	u32 sz = pkt->hdr.payload_size;

	if (MBX_SW_ONLY(mbx))
		return -ENODEV;

	if (!valid_pkt(pkt))
		return -ENOENT;

	(void) memcpy(buf, pkt->body.data, sz);
	reset_pkt(pkt);

	return sz;
}
static ssize_t mailbox_pkt_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	struct mailbox_pkt *pkt = &mbx->mbx_tst_pkt;
	size_t maxlen = sizeof(mbx->mbx_tst_pkt.body.data);

	if (MBX_SW_ONLY(mbx))
		return -ENODEV;

	if (count > maxlen) {
		MBX_ERR(mbx, "max input length is %ld", maxlen);
		return 0;
	}

	(void) memcpy(pkt->body.data, buf, count);
	pkt->hdr.payload_size = count;
	pkt->hdr.type = PKT_TEST;

	/* Sending test pkt. */
	(void) memcpy(&mbx->mbx_tx.mbc_packet, &mbx->mbx_tst_pkt,
		sizeof(struct mailbox_pkt));
	reset_pkt(&mbx->mbx_tst_pkt);
	chan_send_pkt(&mbx->mbx_tx);
	return count;
}
/* Packet test i/f. */
static DEVICE_ATTR_RW(mailbox_pkt);

static struct attribute *mailbox_attrs[] = {
	&dev_attr_mailbox_ctl.attr,
	&dev_attr_mailbox_pkt.attr,
	NULL,
};

static const struct attribute_group mailbox_attrgroup = {
	.attrs = mailbox_attrs,
};

/*
 * Msg will be sent to peer and reply will be received.
 */
static int mailbox_request(struct platform_device *pdev, void *req,
	size_t reqlen, void *resp, size_t *resplen, bool sw_ch, u32 resp_ttl)
{
	int rv = -ENOMEM;
	struct mailbox *mbx = platform_get_drvdata(pdev);
	struct mailbox_msg *reqmsg = NULL, *respmsg = NULL;

	/* If peer is not alive, no point sending req and waiting for resp. */
	if (mbx->mbx_peer_dead)
		return -ENOTCONN;

	reqmsg = alloc_msg(req, reqlen);
	if (!reqmsg)
		goto fail;
	reqmsg->mbm_chan_sw = sw_ch;
	reqmsg->mbm_req_id = (uintptr_t)reqmsg->mbm_data;
	reqmsg->mbm_flags |= MSG_FLAG_REQUEST;

	respmsg = alloc_msg(resp, *resplen);
	if (!respmsg)
		goto fail;
	/* Only interested in response w/ same ID. */
	respmsg->mbm_req_id = reqmsg->mbm_req_id;
	respmsg->mbm_chan_sw = sw_ch;

	/* Always enqueue RX msg before TX one to avoid race. */
	rv = chan_msg_enqueue(&mbx->mbx_rx, respmsg);
	if (rv)
		goto fail;
	rv = chan_msg_enqueue(&mbx->mbx_tx, reqmsg);
	if (rv) {
		respmsg = chan_msg_dequeue(&mbx->mbx_rx, reqmsg->mbm_req_id);
		goto fail;
	}

	/* Wait for req to be sent. */
	wait_for_completion(&reqmsg->mbm_complete);
	rv = reqmsg->mbm_error;
	if (rv) {
		(void) chan_msg_dequeue(&mbx->mbx_rx, reqmsg->mbm_req_id);
		goto fail;
	}
	free_msg(reqmsg);

	/* Start timer and wait for resp to be received. */
	msg_timer_on(respmsg, resp_ttl);
	wait_for_completion(&respmsg->mbm_complete);
	rv = respmsg->mbm_error;
	if (rv == 0)
		*resplen = respmsg->mbm_len;

	free_msg(respmsg);
	return rv;

fail:
	if (reqmsg)
		free_msg(reqmsg);
	if (respmsg)
		free_msg(respmsg);
	return rv;
}

/*
 * Posting notification or response to peer.
 */
static int mailbox_post(struct platform_device *pdev,
	u64 reqid, void *buf, size_t len, bool sw_ch)
{
	int rv = 0;
	struct mailbox *mbx = platform_get_drvdata(pdev);
	struct mailbox_msg *msg = NULL;

	/* If peer is not alive, no point posting a msg. */
	if (mbx->mbx_peer_dead)
		return -ENOTCONN;

	msg = alloc_msg(NULL, len);
	if (!msg)
		return -ENOMEM;

	(void) memcpy(msg->mbm_data, buf, len);
	msg->mbm_chan_sw = sw_ch;
	msg->mbm_req_id = reqid ? reqid : (uintptr_t)msg->mbm_data;
	msg->mbm_flags |= reqid ? MSG_FLAG_RESPONSE : MSG_FLAG_REQUEST;

	rv = chan_msg_enqueue(&mbx->mbx_tx, msg);
	if (rv == 0) {
		wait_for_completion(&msg->mbm_complete);
		rv = msg->mbm_error;
	}

	if (rv)
		MBX_ERR(mbx, "failed to post msg, err=%d", rv);
	free_msg(msg);
	return rv;
}

static void process_request(struct mailbox *mbx, struct mailbox_msg *msg)
{
	/* Call client's registered callback to process request. */
	mutex_lock(&mbx->mbx_listen_cb_lock);

	if (mbx->mbx_listen_cb) {
		mbx->mbx_listen_cb(mbx->mbx_listen_cb_arg, msg->mbm_data,
			msg->mbm_len, msg->mbm_req_id, msg->mbm_error,
			msg->mbm_chan_sw);
	} else {
		MBX_INFO(mbx, "msg dropped, no listener");
	}

	mutex_unlock(&mbx->mbx_listen_cb_lock);
}

/*
 * Wait for request from peer.
 */
static void mailbox_recv_request(struct work_struct *work)
{
	struct mailbox_msg *msg = NULL;
	struct mailbox *mbx =
		container_of(work, struct mailbox, mbx_listen_worker);

	while (!mbx->mbx_listen_stop) {
		/* Only interested in request msg. */
		(void) wait_for_completion_interruptible(&mbx->mbx_comp);

		mutex_lock(&mbx->mbx_lock);

		while ((msg = list_first_entry_or_null(&mbx->mbx_req_list,
			struct mailbox_msg, mbm_list)) != NULL) {
			list_del(&msg->mbm_list);
			mbx->mbx_req_cnt--;
			mutex_unlock(&mbx->mbx_lock);

			/* Process msg without holding mutex. */
			process_request(mbx, msg);
			free_msg(msg);

			mutex_lock(&mbx->mbx_lock);
		}

		mutex_unlock(&mbx->mbx_lock);
	}

	/* Drain all msg before quit. */
	mutex_lock(&mbx->mbx_lock);
	while ((msg = list_first_entry_or_null(&mbx->mbx_req_list,
		struct mailbox_msg, mbm_list)) != NULL) {
		list_del(&msg->mbm_list);
		free_msg(msg);
	}
	mutex_unlock(&mbx->mbx_lock);
}

static int mailbox_listen(struct platform_device *pdev,
	mailbox_msg_cb_t cb, void *cbarg)
{
	struct mailbox *mbx = platform_get_drvdata(pdev);

	mutex_lock(&mbx->mbx_listen_cb_lock);

	mbx->mbx_listen_cb_arg = cbarg;
	mbx->mbx_listen_cb = cb;

	mutex_unlock(&mbx->mbx_listen_cb_lock);

	return 0;
}

static int mailbox_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct mailbox *mbx = platform_get_drvdata(pdev);
	int ret = 0;

	MBX_INFO(mbx, "handling IOCTL cmd: %d", cmd);

	switch (cmd) {
	case XRT_MAILBOX_POST: {
		struct xrt_mailbox_ioctl_post *post =
			(struct xrt_mailbox_ioctl_post *)arg;

		ret = mailbox_post(pdev, post->xmip_req_id, post->xmip_data,
			post->xmip_data_size, post->xmip_sw_ch);
		break;
	}
	case XRT_MAILBOX_REQUEST: {
		struct xrt_mailbox_ioctl_request *req =
			(struct xrt_mailbox_ioctl_request *)arg;

		ret = mailbox_request(pdev, req->xmir_req, req->xmir_req_size,
			req->xmir_resp, &req->xmir_resp_size, req->xmir_sw_ch,
			req->xmir_resp_ttl);
		break;
	}
	case XRT_MAILBOX_LISTEN: {
		struct xrt_mailbox_ioctl_listen *listen =
			(struct xrt_mailbox_ioctl_listen *)arg;

		ret = mailbox_listen(pdev,
			listen->xmil_cb, listen->xmil_cb_arg);
		break;
	}
	default:
		MBX_ERR(mbx, "unknown cmd: %d", cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static void mailbox_stop(struct mailbox *mbx)
{
	/* Tear down all threads. */
	del_timer_sync(&mbx->mbx_poll_timer);
	chan_fini(&mbx->mbx_tx);
	chan_fini(&mbx->mbx_rx);
	listen_wq_fini(mbx);
	BUG_ON(!(list_empty(&mbx->mbx_req_list)));
}

static int mailbox_start(struct mailbox *mbx)
{
	int ret;

	mbx->mbx_req_cnt = 0;
	mbx->mbx_peer_dead = false;
	mbx->mbx_opened = 0;
	mbx->mbx_listen_stop = false;

	/* Dedicated thread for listening to peer request. */
	mbx->mbx_listen_wq =
		create_singlethread_workqueue(dev_name(&mbx->mbx_pdev->dev));
	if (!mbx->mbx_listen_wq) {
		MBX_ERR(mbx, "failed to create request-listen work queue");
		ret = -ENOMEM;
		goto out;
	}
	INIT_WORK(&mbx->mbx_listen_worker, mailbox_recv_request);
	queue_work(mbx->mbx_listen_wq, &mbx->mbx_listen_worker);

	/* Set up communication channels. */
	ret = chan_init(mbx, MBXCT_RX, &mbx->mbx_rx, chan_do_rx);
	if (ret != 0) {
		MBX_ERR(mbx, "failed to init rx channel");
		goto out;
	}
	ret = chan_init(mbx, MBXCT_TX, &mbx->mbx_tx, chan_do_tx);
	if (ret != 0) {
		MBX_ERR(mbx, "failed to init tx channel");
		goto out;
	}

	/* Only see status change when we have full packet sent or received. */
	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_rit, PACKET_SIZE - 1);
	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_sit, 0);

	/* Disable both TX / RX intrs. We only do polling. */
	if (!MBX_SW_ONLY(mbx))
		mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_ie, 0x0);
	timer_setup(&mbx->mbx_poll_timer, mailbox_poll_timer, 0);
	mod_timer(&mbx->mbx_poll_timer, jiffies + MAILBOX_TTL_TIMER);

out:
	return ret;
}

static int mailbox_open(struct inode *inode, struct file *file)
{
	/*
	 * Only allow one open from daemon. Mailbox msg can only be polled
	 * by one daemon.
	 */
	struct platform_device *pdev = xrt_devnode_open_excl(inode);
	struct mailbox *mbx = NULL;

	if (!pdev)
		return -ENXIO;

	mbx = platform_get_drvdata(pdev);
	if (!mbx)
		return -ENXIO;

	/*
	 * Indicates that mpd/msd is up and running, assuming msd/mpd
	 * is the only user of the software mailbox
	 */
	mutex_lock(&mbx->mbx_lock);
	mbx->mbx_opened++;
	mutex_unlock(&mbx->mbx_lock);

	file->private_data = mbx;
	return 0;
}

/*
 * Called when the device goes from used to unused.
 */
static int mailbox_close(struct inode *inode, struct file *file)
{
	struct mailbox *mbx = file->private_data;

	mutex_lock(&mbx->mbx_lock);
	mbx->mbx_opened--;
	mutex_unlock(&mbx->mbx_lock);
	xrt_devnode_close(inode);
	return 0;
}

/*
 * Software channel TX handler. Msg goes out to peer.
 *
 * We either read the entire msg out or nothing and return error. Partial read
 * is not supported.
 */
static ssize_t
mailbox_read(struct file *file, char __user *buf, size_t n, loff_t *ignd)
{
	struct mailbox *mbx = file->private_data;
	struct mailbox_channel *ch = &mbx->mbx_tx;
	struct xcl_sw_chan args = { 0 };

	if (n < sizeof(struct xcl_sw_chan)) {
		MBX_ERR(mbx, "Software TX buf has no room for header");
		return -EINVAL;
	}

	/* Wait until tx worker has something to transmit to peer. */
	if (wait_event_interruptible(ch->sw_chan_wq,
		atomic_read(&ch->sw_num_pending_msg) > 0) == -ERESTARTSYS) {
		MBX_ERR(mbx, "Software TX channel handler is interrupted");
		return -ERESTARTSYS;
	}

	/* We have something to send, do it now. */

	mutex_lock(&ch->sw_chan_mutex);

	/* Nothing to do. Someone is ahead of us and did the job? */
	if (ch->sw_chan_msg_id == 0) {
		mutex_unlock(&ch->sw_chan_mutex);
		MBX_ERR(mbx, "Software TX channel is empty");
		return 0;
	}

	/* Copy header to user. */
	args.id = ch->sw_chan_msg_id;
	args.sz = ch->sw_chan_buf_sz;
	args.flags = ch->sw_chan_msg_flags;
	if (copy_to_user(buf, &args, sizeof(struct xcl_sw_chan)) != 0) {
		mutex_unlock(&ch->sw_chan_mutex);
		return -EFAULT;
	}

	/*
	 * Buffer passed in is too small for payload, return EMSGSIZE to ask
	 * for a bigger one.
	 */
	if (ch->sw_chan_buf_sz > (n - sizeof(struct xcl_sw_chan))) {
		mutex_unlock(&ch->sw_chan_mutex);
		/*
		 * This error occurs when daemons try to query the size
		 * of the msg. Show it as info to avoid flushing sytem console.
		 */
		MBX_INFO(mbx, "Software TX msg is too big");
		return -EMSGSIZE;
	}

	/* Copy payload to user. */
	if (copy_to_user(((struct xcl_sw_chan *)buf)->data,
		ch->sw_chan_buf, ch->sw_chan_buf_sz) != 0) {
		mutex_unlock(&ch->sw_chan_mutex);
		return -EFAULT;
	}

	/* Mark that job is done and we're ready for next TX msg. */
	reset_sw_ch(ch);

	mutex_unlock(&ch->sw_chan_mutex);
	return args.sz + sizeof(struct xcl_sw_chan);
}

/*
 * Software channel RX handler. Msg comes in from peer.
 *
 * We either receive the entire msg or nothing and return error. Partial write
 * is not supported.
 */
static ssize_t
mailbox_write(struct file *file, const char __user *buf, size_t n, loff_t *ignd)
{
	struct mailbox *mbx = file->private_data;
	struct mailbox_channel *ch = &mbx->mbx_rx;
	struct xcl_sw_chan args = { 0 };
	void *payload = NULL;

	if (n < sizeof(struct xcl_sw_chan)) {
		MBX_ERR(mbx, "Software RX msg has invalid header");
		return -EINVAL;
	}

	/* Wait until rx worker is ready for receiving next msg from peer. */
	if (wait_event_interruptible(ch->sw_chan_wq,
		atomic_read(&ch->sw_num_pending_msg) == 0) == -ERESTARTSYS) {
		MBX_ERR(mbx, "Software RX channel handler is interrupted");
		return -ERESTARTSYS;
	}

	/* Rx worker is ready to receive msg, do it now. */

	mutex_lock(&ch->sw_chan_mutex);

	/* No room for us. Someone is ahead of us and is using the channel? */
	if (ch->sw_chan_msg_id != 0) {
		mutex_unlock(&ch->sw_chan_mutex);
		MBX_ERR(mbx, "Software RX channel is busy");
		return -EBUSY;
	}

	/* Copy header from user. */
	if (copy_from_user(&args, buf, sizeof(struct xcl_sw_chan)) != 0) {
		mutex_unlock(&ch->sw_chan_mutex);
		return -EFAULT;
	}
	if (args.id == 0 || args.sz == 0) {
		mutex_unlock(&ch->sw_chan_mutex);
		MBX_ERR(mbx, "Software RX msg has malformed header");
		return -EINVAL;
	}

	/* Copy payload from user. */
	if (n < args.sz + sizeof(struct xcl_sw_chan)) {
		mutex_unlock(&ch->sw_chan_mutex);
		MBX_ERR(mbx, "Software RX msg has invalid payload");
		return -EINVAL;
	}
	payload = vmalloc(args.sz);
	if (payload == NULL) {
		mutex_unlock(&ch->sw_chan_mutex);
		return -ENOMEM;
	}
	if (copy_from_user(payload, ((struct xcl_sw_chan *)buf)->data,
		args.sz) != 0) {
		mutex_unlock(&ch->sw_chan_mutex);
		vfree(payload);
		return -EFAULT;
	}

	/* Set up received msg and notify rx worker. */
	ch->sw_chan_buf_sz = args.sz;
	ch->sw_chan_msg_id = args.id;
	ch->sw_chan_msg_flags = args.flags;
	ch->sw_chan_buf = payload;

	atomic_inc(&ch->sw_num_pending_msg);

	mutex_unlock(&ch->sw_chan_mutex);

	return args.sz + sizeof(struct xcl_sw_chan);
}

static uint mailbox_poll(struct file *file, poll_table *wait)
{
	struct mailbox *mbx = file->private_data;
	struct mailbox_channel *ch = &mbx->mbx_tx;
	int counter;

	poll_wait(file, &ch->sw_chan_wq, wait);
	counter = atomic_read(&ch->sw_num_pending_msg);

	MBX_DBG(mbx, "%s: %d", __func__, counter);
	if (counter == 0)
		return 0;
	return POLLIN;
}

static int mailbox_remove(struct platform_device *pdev)
{
	struct mailbox *mbx = platform_get_drvdata(pdev);

	BUG_ON(mbx == NULL);

	/* Stop accessing from sysfs node. */
	sysfs_remove_group(&pdev->dev.kobj, &mailbox_attrgroup);

	mailbox_stop(mbx);

	if (mbx->mbx_regs)
		iounmap(mbx->mbx_regs);

	MBX_INFO(mbx, "mailbox cleaned up successfully");

	platform_set_drvdata(pdev, NULL);
	return 0;
}

static int mailbox_probe(struct platform_device *pdev)
{
	struct mailbox *mbx = NULL;
	struct resource *res;
	int ret;

	mbx = devm_kzalloc(DEV(pdev), sizeof(struct mailbox), GFP_KERNEL);
	if (!mbx)
		return -ENOMEM;

	mbx->mbx_pdev = pdev;
	platform_set_drvdata(pdev, mbx);

	init_completion(&mbx->mbx_comp);
	mutex_init(&mbx->mbx_lock);
	mutex_init(&mbx->mbx_listen_cb_lock);
	INIT_LIST_HEAD(&mbx->mbx_req_list);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res != NULL) {
		mbx->mbx_regs = ioremap(res->start, res->end - res->start + 1);
		if (!mbx->mbx_regs) {
			MBX_ERR(mbx, "failed to map in registers");
			ret = -EIO;
			goto failed;
		}
	}

	ret = mailbox_start(mbx);
	if (ret)
		goto failed;

	/* Enable access thru sysfs node. */
	ret = sysfs_create_group(&pdev->dev.kobj, &mailbox_attrgroup);
	if (ret != 0) {
		MBX_ERR(mbx, "failed to init sysfs");
		goto failed;
	}

	MBX_INFO(mbx, "successfully initialized");
	return 0;

failed:
	mailbox_remove(pdev);
	return ret;
}

struct xrt_subdev_endpoints xrt_mailbox_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []){
			{ .ep_name = NODE_MAILBOX_VSEC},
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xrt_subdev_drvdata mailbox_drvdata = {
	.xsd_dev_ops = {
		.xsd_ioctl = mailbox_leaf_ioctl,
	},
	.xsd_file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = mailbox_open,
			.release = mailbox_close,
			.read = mailbox_read,
			.write = mailbox_write,
			.poll = mailbox_poll,
		},
		.xsf_dev_name = "mailbox",
	},
};

#define	XRT_MAILBOX	"xrt_mailbox"

struct platform_device_id mailbox_id_table[] = {
	{ XRT_MAILBOX, (kernel_ulong_t)&mailbox_drvdata },
	{ },
};

struct platform_driver xrt_mailbox_driver = {
	.probe		= mailbox_probe,
	.remove		= mailbox_remove,
	.driver		= {
		.name	= XRT_MAILBOX,
	},
	.id_table = mailbox_id_table,
};
