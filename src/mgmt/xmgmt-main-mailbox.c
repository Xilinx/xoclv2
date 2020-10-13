// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA MGMT PF entry point driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Peer communication via mailbox
 *
 * Authors:
 *      Cheng Zhen <maxz@xilinx.com>
 */

#include "uapi/mailbox_proto.h"
#include "xmgmt-main-impl.h"
#include "xocl-mailbox.h"

struct xmgmt_mailbox {
	struct platform_device *pdev;
	struct platform_device *mailbox;
	struct mutex lock;
	void *evt_hdl;
	char *test_msg;
};

#define	XMGMT_MAILBOX_PRT_REQ(xmbx, send, request, sw_ch)	do {	\
	const char *dir = (send) ? ">>>>>" : "<<<<<";			\
									\
	if ((request)->req == XCL_MAILBOX_REQ_PEER_DATA) {		\
		struct xcl_mailbox_subdev_peer *p =			\
			(struct xcl_mailbox_subdev_peer *)(request)->data; \
									\
		xocl_info((xmbx)->pdev, "%s(%s) %s%s",			\
			mailbox_req2name((request)->req),		\
			mailbox_group_kind2name(p->kind),		\
			dir, mailbox_chan2name(sw_ch));			\
	} else {							\
		xocl_info((xmbx)->pdev, "%s %s%s",			\
			mailbox_req2name((request)->req),		\
			dir, mailbox_chan2name(sw_ch));			\
	}								\
} while (0)
#define	XMGMT_MAILBOX_PRT_REQ_SEND(xmbx, req, sw_ch)			\
	XMGMT_MAILBOX_PRT_REQ(xmbx, true, req, sw_ch)
#define	XMGMT_MAILBOX_PRT_REQ_RECV(xmbx, req, sw_ch)			\
	XMGMT_MAILBOX_PRT_REQ(xmbx, false, req, sw_ch)
#define	XMGMT_MAILBOX_PRT_RESP(xmbx, resp)				\
	xocl_info((xmbx)->pdev, "respond %ld bytes >>>>>%s",		\
	(resp)->xmip_data_size, mailbox_chan2name((resp)->xmip_sw_ch))

static inline struct xmgmt_mailbox *pdev2mbx(struct platform_device *pdev)
{
	return (struct xmgmt_mailbox *)xmgmt_pdev2mailbox(pdev);
}

static void xmgmt_mailbox_post(struct xmgmt_mailbox *xmbx,
	u64 msgid, bool sw_ch, void *buf, size_t len)
{
	int rc;
	struct xocl_mailbox_ioctl_post post = {
		.xmip_req_id = msgid,
		.xmip_sw_ch = sw_ch,
		.xmip_data = buf,
		.xmip_data_size = len
	};

	BUG_ON(!mutex_is_locked(&xmbx->lock));

	if (!xmbx->mailbox) {
		xocl_err(xmbx->pdev, "mailbox not available");
		return;
	}

	if (msgid == 0) {
		XMGMT_MAILBOX_PRT_REQ_SEND(xmbx,
			(struct xcl_mailbox_req *)buf, sw_ch);
	} else {
		XMGMT_MAILBOX_PRT_RESP(xmbx, &post);
	}

	rc = xocl_subdev_ioctl(xmbx->mailbox, XOCL_MAILBOX_POST, &post);
	if (rc)
		xocl_err(xmbx->pdev, "failed to post msg: %d", rc);
}

static void xmgmt_mailbox_notify(struct xmgmt_mailbox *xmbx, bool sw_ch,
	struct xcl_mailbox_req *req, size_t len)
{
	xmgmt_mailbox_post(xmbx, 0, sw_ch, req, len);
}

static void xmgmt_mailbox_respond(struct xmgmt_mailbox *xmbx,
	u64 msgid, bool sw_ch, void *buf, size_t len)
{
	xmgmt_mailbox_post(xmbx, msgid, sw_ch, buf, len);
}

static void xmgmt_mailbox_resp_test_msg(struct xmgmt_mailbox *xmbx,
	u64 msgid, bool sw_ch)
{
	struct platform_device *pdev = xmbx->pdev;

	mutex_lock(&xmbx->lock);

	if (xmbx->test_msg == NULL) {
		mutex_unlock(&xmbx->lock);
		xocl_err(pdev, "test msg is not set, drop request");
		return;
	}

	xmgmt_mailbox_respond(xmbx, msgid, sw_ch,
		xmbx->test_msg, strlen(xmbx->test_msg) + 1);
	vfree(xmbx->test_msg);
	xmbx->test_msg = NULL;
	mutex_unlock(&xmbx->lock);
}

static void xmgmt_mailbox_listener(void *arg, void *data, size_t len,
	u64 msgid, int err, bool sw_ch)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)arg;
	struct platform_device *pdev = xmbx->pdev;
	struct xcl_mailbox_req *req = (struct xcl_mailbox_req *)data;

	if (err) {
		xocl_err(pdev, "failed to receive request: %d", err);
		return;
	}
	if (len < sizeof(*req)) {
		xocl_err(pdev, "received corrupted request");
		return;
	}

	XMGMT_MAILBOX_PRT_REQ_RECV(xmbx, req, sw_ch);
	switch (req->req) {
	case XCL_MAILBOX_REQ_TEST_READ:
		xmgmt_mailbox_resp_test_msg(xmbx, msgid, sw_ch);
		break;
	default:
		xocl_warn(pdev, "request(%d) not handled", req->req);
		break;
	}
}

static void xmgmt_mailbox_reg_listener(struct xmgmt_mailbox *xmbx)
{
	struct xocl_mailbox_ioctl_listen listen = {
		xmgmt_mailbox_listener, xmbx };

	BUG_ON(!mutex_is_locked(&xmbx->lock));
	if (!xmbx->mailbox)
		return;
	(void) xocl_subdev_ioctl(xmbx->mailbox, XOCL_MAILBOX_LISTEN, &listen);
}

static void xmgmt_mailbox_unreg_listener(struct xmgmt_mailbox *xmbx)
{
	struct xocl_mailbox_ioctl_listen listen = { 0 };

	BUG_ON(!mutex_is_locked(&xmbx->lock));
	BUG_ON(!xmbx->mailbox);
	(void) xocl_subdev_ioctl(xmbx->mailbox, XOCL_MAILBOX_LISTEN, &listen);
}

static void xmgmt_mailbox_notify_state(struct xmgmt_mailbox *xmbx, bool online)
{
	struct xcl_mailbox_peer_state *st;
	struct xcl_mailbox_req *req;
	size_t reqlen = sizeof(*req) + sizeof(*st) - 1;

	req = vzalloc(reqlen);
	if (req == NULL)
		return;

	req->req = XCL_MAILBOX_REQ_MGMT_STATE;
	st = (struct xcl_mailbox_peer_state *)req->data;
	st->state_flags = online ? XCL_MB_STATE_ONLINE : XCL_MB_STATE_OFFLINE;
	xmgmt_mailbox_notify(xmbx, false, req, reqlen);
}

static bool xmgmt_mailbox_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	return (id == XOCL_SUBDEV_MAILBOX);
}

static int xmgmt_mailbox_event_cb(struct platform_device *pdev,
	enum xocl_events evt, void *arg)
{
	struct xmgmt_mailbox *xmbx = pdev2mbx(pdev);
	struct xocl_event_arg_subdev *esd = (struct xocl_event_arg_subdev *)arg;

	switch (evt) {
	case XOCL_EVENT_POST_CREATION:
		BUG_ON(esd->xevt_subdev_id != XOCL_SUBDEV_MAILBOX);
		BUG_ON(xmbx->mailbox);
		mutex_lock(&xmbx->lock);
		xmbx->mailbox = xocl_subdev_get_leaf_by_id(pdev,
			XOCL_SUBDEV_MAILBOX, PLATFORM_DEVID_NONE);
		xmgmt_mailbox_reg_listener(xmbx);
		xmgmt_mailbox_notify_state(xmbx, true);
		mutex_unlock(&xmbx->lock);
		break;
	case XOCL_EVENT_PRE_REMOVAL:
		BUG_ON(esd->xevt_subdev_id != XOCL_SUBDEV_MAILBOX);
		BUG_ON(!xmbx->mailbox);
		mutex_lock(&xmbx->lock);
		xmgmt_mailbox_notify_state(xmbx, false);
		xmgmt_mailbox_unreg_listener(xmbx);
		(void) xocl_subdev_put_leaf(pdev, xmbx->mailbox);
		xmbx->mailbox = NULL;
		mutex_unlock(&xmbx->lock);
		break;
	default:
		break;
	}

	return XOCL_EVENT_CB_CONTINUE;
}

void *xmgmt_mailbox_probe(struct platform_device *pdev)
{
	struct xmgmt_mailbox *xmbx =
		devm_kzalloc(DEV(pdev), sizeof(*xmbx), GFP_KERNEL);

	if (!xmbx)
		return NULL;
	xmbx->pdev = pdev;
	mutex_init(&xmbx->lock);

	xmbx->evt_hdl = xocl_subdev_add_event_cb(pdev,
		xmgmt_mailbox_leaf_match, NULL, xmgmt_mailbox_event_cb);
	return xmbx;
}

void xmgmt_mailbox_remove(void *handle)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)handle;
	struct platform_device *pdev = xmbx->pdev;

	if (xmbx->evt_hdl)
		(void) xocl_subdev_remove_event_cb(pdev, xmbx->evt_hdl);
	if (xmbx->mailbox)
		(void) xocl_subdev_put_leaf(pdev, xmbx->mailbox);
	if (xmbx->test_msg)
		vfree(xmbx->test_msg);
}

int xmgmt_mailbox_set_test_msg(struct xmgmt_mailbox *xmbx,
	struct xocl_mgmt_main_peer_test_msg *tm)
{
	mutex_lock(&xmbx->lock);

	if (xmbx->test_msg)
		vfree(xmbx->test_msg);
	xmbx->test_msg = vmalloc(tm->xmmpgtm_len);
	if (xmbx->test_msg == NULL) {
		mutex_unlock(&xmbx->lock);
		return -ENOMEM;
	}
	(void) memcpy(xmbx->test_msg, tm->xmmpgtm_buf, tm->xmmpgtm_len);

	mutex_unlock(&xmbx->lock);
	return 0;
}

int xmgmt_mailbox_get_test_msg(struct xmgmt_mailbox *xmbx,
	struct xocl_mgmt_main_peer_test_msg *tm)
{
	int rc;
	struct platform_device *pdev = xmbx->pdev;
	struct xcl_mailbox_req req = { 0, XCL_MAILBOX_REQ_TEST_READ, };
	struct xocl_mailbox_ioctl_request leaf_req = {
		.xmir_sw_ch = false,
		.xmir_resp_ttl = 1,
		.xmir_req = &req,
		.xmir_req_size = sizeof(req),
		.xmir_resp = tm->xmmpgtm_buf,
		.xmir_resp_size = tm->xmmpgtm_len
	};

	mutex_lock(&xmbx->lock);
	if (xmbx->mailbox) {
		XMGMT_MAILBOX_PRT_REQ_SEND(xmbx, &req, leaf_req.xmir_sw_ch);
		/*
		 * mgmt should never send request to peer. it should send
		 * either notification or response. here is the only exception
		 * for debugging purpose.
		 */
		rc = xocl_subdev_ioctl(xmbx->mailbox,
			XOCL_MAILBOX_REQUEST, &leaf_req);
	} else {
		rc = -ENODEV;
		xocl_err(pdev, "mailbox not available");
	}
	mutex_unlock(&xmbx->lock);

	tm->xmmpgtm_len = leaf_req.xmir_resp_size;
	return rc;
}

int xmgmt_peer_test_msg(void *handle, struct xocl_mgmt_main_peer_test_msg *tm)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)handle;

	if (tm->xmmpgtm_set)
		return xmgmt_mailbox_set_test_msg(xmbx, tm);
	return xmgmt_mailbox_get_test_msg(xmbx, tm);
}
