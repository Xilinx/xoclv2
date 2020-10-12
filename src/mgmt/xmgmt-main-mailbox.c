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

#define	XMGMT_MAILBOX_PRT_REQ(pdev, send, request, sw_ch)	do {	\
	const char *dir = (send) ? ">>>>>" : "<<<<<";			\
									\
	if ((request)->req == XCL_MAILBOX_REQ_PEER_DATA) {		\
		struct xcl_mailbox_subdev_peer *p =			\
			(struct xcl_mailbox_subdev_peer *)(request)->data; \
									\
		xocl_info((pdev), "%s(%s) %s%s",			\
			mailbox_req2name((request)->req),		\
			mailbox_group_kind2name(p->kind),		\
			dir, mailbox_chan2name(sw_ch));			\
	} else {							\
		xocl_info((pdev), "%s %s%s",				\
			mailbox_req2name((request)->req),		\
			dir, mailbox_chan2name(sw_ch));			\
	}								\
} while (0)
#define	XMGMT_MAILBOX_PRT_REQ_SEND(pdev, req, sw_ch)			\
	XMGMT_MAILBOX_PRT_REQ(pdev, true, req, sw_ch)
#define	XMGMT_MAILBOX_PRT_REQ_RECV(pdev, req, sw_ch)			\
	XMGMT_MAILBOX_PRT_REQ(pdev, false, req, sw_ch)
#define	XMGMT_MAILBOX_PRT_RESP(pdev, resp)				\
	xocl_info((pdev), "respond %ld bytes >>>>>%s",			\
	(resp)->xmip_data_size, mailbox_chan2name((resp)->xmip_sw_ch))

static inline struct xmgmt_mailbox *pdev2mbx(struct platform_device *pdev)
{
	return (struct xmgmt_mailbox *)xmgmt_pdev2mailbox(pdev);
}

static bool xmgmt_mailbox_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	return (id == XOCL_SUBDEV_MAILBOX);
}

static void xmgmt_mailbox_req_test_msg(struct xmgmt_mailbox *xmbx,
	struct xocl_mailbox_ioctl_post *post)
{
	struct platform_device *pdev = xmbx->pdev;

	mutex_lock(&xmbx->lock);

	if (xmbx->test_msg == NULL) {
		mutex_unlock(&xmbx->lock);
		xocl_err(pdev, "test msg is not set, drop request");
		return;
	}

	post->xmip_data = xmbx->test_msg;
	post->xmip_data_size = strlen(xmbx->test_msg) + 1;

	if (xmbx->mailbox) {
		XMGMT_MAILBOX_PRT_RESP(pdev, post);
		(void) xocl_subdev_ioctl(xmbx->mailbox, XOCL_MAILBOX_POST, post);
		vfree(xmbx->test_msg);
		xmbx->test_msg = NULL;
	} else {
		xocl_err(pdev, "mailbox not available");
	}
	mutex_unlock(&xmbx->lock);
}

static void xmgmt_mailbox_listener(void *arg, void *data, size_t len,
	u64 msgid, int err, bool sw_ch)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)arg;
	struct platform_device *pdev = xmbx->pdev;
	struct xcl_mailbox_req *req = (struct xcl_mailbox_req *)data;
	struct xocl_mailbox_ioctl_post post = {
		.xmip_req_id = msgid,
		.xmip_sw_ch = sw_ch,
	};

	if (err) {
		xocl_err(pdev, "failed to receive request: %d", err);
		return;
	}
	if (len < sizeof(*req)) {
		xocl_err(pdev, "received corrupted request");
		return;
	}

	XMGMT_MAILBOX_PRT_REQ_RECV(pdev, req, sw_ch);
	switch (req->req) {
	case XCL_MAILBOX_REQ_TEST_READ:
		xmgmt_mailbox_req_test_msg(xmbx, &post);
		break;
	default:
		xocl_err(pdev, "request not handled");
		break;
	}
}

static int xmgmt_mailbox_event_cb(struct platform_device *pdev,
	enum xocl_events evt, void *arg)
{
	struct xmgmt_mailbox *xmbx = pdev2mbx(pdev);
	struct xocl_event_arg_subdev *esd = (struct xocl_event_arg_subdev *)arg;
	struct xocl_mailbox_ioctl_listen listen = { 0 };

	switch (evt) {
	case XOCL_EVENT_POST_CREATION:
		BUG_ON(esd->xevt_subdev_id != XOCL_SUBDEV_MAILBOX);
		BUG_ON(xmbx->mailbox);
		mutex_lock(&xmbx->lock);
		xmbx->mailbox = xocl_subdev_get_leaf_by_id(pdev,
			XOCL_SUBDEV_MAILBOX, PLATFORM_DEVID_NONE);
		listen.xmil_cb = xmgmt_mailbox_listener;
		listen.xmil_cb_arg = xmbx;
		(void) xocl_subdev_ioctl(xmbx->mailbox,
			XOCL_MAILBOX_LISTEN, &listen);
		mutex_unlock(&xmbx->lock);
		break;
	case XOCL_EVENT_PRE_REMOVAL:
		BUG_ON(esd->xevt_subdev_id != XOCL_SUBDEV_MAILBOX);
		BUG_ON(!xmbx->mailbox);
		mutex_lock(&xmbx->lock);
		(void) xocl_subdev_ioctl(xmbx->mailbox,
			XOCL_MAILBOX_LISTEN, &listen);
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

int xmgmt_peer_set_test_msg(struct xmgmt_mailbox *xmbx,
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

int xmgmt_peer_get_test_msg(struct xmgmt_mailbox *xmbx,
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
		XMGMT_MAILBOX_PRT_REQ_SEND(pdev, &req, leaf_req.xmir_sw_ch);
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

int xmgmt_peer_test_msg(void *handle,
	struct xocl_mgmt_main_peer_test_msg *tm)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)handle;

	if (tm->xmmpgtm_set)
		return xmgmt_peer_set_test_msg(xmbx, tm);
	return xmgmt_peer_get_test_msg(xmbx, tm);
}
