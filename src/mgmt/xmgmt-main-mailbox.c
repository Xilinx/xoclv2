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

#include <linux/crc32c.h>
#include "uapi/mailbox_proto.h"
#include "xmgmt-main-impl.h"
#include "xocl-mailbox.h"
#include "xocl-metadata.h"

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
		struct xcl_mailbox_peer_data *p =			\
			(struct xcl_mailbox_peer_data *)(request)->data;\
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

static int xmgmt_mailbox_dtb_add_prop(struct platform_device *pdev,
	char *dst_dtb, const char *ep_name, const char *regmap_name,
	const char *prop, const void *val, int size)
{
	int rc = xocl_md_set_prop(DEV(pdev), dst_dtb, ep_name, regmap_name,
		prop, val, size);

	if (rc) {
		xocl_err(pdev, "failed to set %s@(%s, %s): %d",
			ep_name, regmap_name, prop, rc);
	}
	return rc;
}

static int xmgmt_mailbox_dtb_add_vbnv(struct platform_device *pdev, char *dtb)
{
	int rc = 0;
	char *vbnv = xmgmt_get_vbnv(pdev);

	if (vbnv == NULL) {
		xocl_err(pdev, "failed to get VBNV");
		return -ENOENT;
	}
	rc = xmgmt_mailbox_dtb_add_prop(pdev, dtb, NULL, NULL,
		PROP_VBNV, vbnv, strlen(vbnv) + 1);
	kfree(vbnv);
	return rc;
}

static int xmgmt_mailbox_dtb_copy_logic_uuid(struct platform_device *pdev,
	const char *src_dtb, char *dst_dtb)
{
	const void *val;
	int sz;
	int rc = xocl_md_get_prop(DEV(pdev), src_dtb, NULL, NULL,
		PROP_LOGIC_UUID, &val, &sz);

	if (rc) {
		xocl_err(pdev, "failed to get %s: %d", PROP_LOGIC_UUID, rc);
		return rc;
	}
	return xmgmt_mailbox_dtb_add_prop(pdev, dst_dtb, NULL, NULL,
		PROP_LOGIC_UUID, val, sz);
}

static int xmgmt_mailbox_dtb_add_vrom(struct platform_device *pdev,
	const char *src_dtb, char *dst_dtb)
{
	/* For compatibility for legacy xocl driver. */
	enum FeatureBitMask {
		UNIFIED_PLATFORM		= 0x0000000000000001
		, XARE_ENBLD			= 0x0000000000000002
		, BOARD_MGMT_ENBLD		= 0x0000000000000004
		, MB_SCHEDULER			= 0x0000000000000008
		, PROM_MASK			= 0x0000000000000070
		, DEBUG_MASK			= 0x000000000000FF00
		, PEER_TO_PEER			= 0x0000000000010000
		, FBM_UUID			= 0x0000000000020000
		, HBM				= 0x0000000000040000
		, CDMA				= 0x0000000000080000
		, QDMA				= 0x0000000000100000
		, RUNTIME_CLK_SCALE		= 0x0000000000200000
		, PASSTHROUGH_VIRTUALIZATION	= 0x0000000000400000
	};
	struct FeatureRomHeader {
		unsigned char EntryPointString[4];
		uint8_t MajorVersion;
		uint8_t MinorVersion;
		uint32_t VivadoBuildID;
		uint32_t IPBuildID;
		uint64_t TimeSinceEpoch;
		unsigned char FPGAPartName[64];
		unsigned char VBNVName[64];
		uint8_t DDRChannelCount;
		uint8_t DDRChannelSize;
		uint64_t DRBaseAddress;
		uint64_t FeatureBitMap;
		unsigned char uuid[16];
		uint8_t HBMCount;
		uint8_t HBMSize;
		uint32_t CDMABaseAddress[4];
	} header = { 0 };
	char *vbnv = xmgmt_get_vbnv(pdev);
	int rc;

	*(u32 *)header.EntryPointString = 0x786e6c78;

	if (vbnv)
		strncpy(header.VBNVName, vbnv, sizeof(header.VBNVName) - 1);
	kfree(vbnv);

	header.FeatureBitMap = UNIFIED_PLATFORM;
	rc = xocl_md_get_prop(DEV(pdev), src_dtb,
		NODE_CMC_FW_MEM, NULL, PROP_IO_OFFSET, NULL, NULL);
	if (rc == 0)
		header.FeatureBitMap |= BOARD_MGMT_ENBLD;
	rc = xocl_md_get_prop(DEV(pdev), src_dtb,
		NODE_ERT_FW_MEM, NULL, PROP_IO_OFFSET, NULL, NULL);
	if (rc == 0)
		header.FeatureBitMap |= MB_SCHEDULER;

	return xmgmt_mailbox_dtb_add_prop(pdev, dst_dtb, NULL, NULL,
		PROP_VROM, &header, sizeof(header));
}

static u32 xmgmt_mailbox_dtb_user_pf(struct platform_device *pdev,
	const char *dtb, const char *epname, const char *regmap)
{
	const u32 *pfnump;
	int rc = xocl_md_get_prop(DEV(pdev), dtb, epname, regmap,
		PROP_PF_NUM, (const void **)&pfnump, NULL);

	if (rc)
		return -1;
	return be32_to_cpu(*pfnump);
}

static int xmgmt_mailbox_dtb_copy_user_endpoints(struct platform_device *pdev,
	const char *src, char *dst)
{
	int rc = 0;
	char *epname = NULL, *regmap = NULL;
	u32 pfnum = xmgmt_mailbox_dtb_user_pf(pdev, src,
		NODE_MAILBOX_USER, NULL);
	const u32 level = cpu_to_be32(1);
	struct device *dev = DEV(pdev);

	if (pfnum == (u32)-1) {
		xocl_err(pdev, "failed to get user pf num");
		rc = -EINVAL;
	}

	for (xocl_md_get_next_endpoint(dev, src, NULL, NULL, &epname, &regmap);
		rc == 0 && epname != NULL;
		xocl_md_get_next_endpoint(dev, src, epname, regmap,
		&epname, &regmap)) {
		if (pfnum !=
			xmgmt_mailbox_dtb_user_pf(pdev, src, epname, regmap))
			continue;
		rc = xocl_md_copy_endpoint(dev, dst, src, epname, regmap, NULL);
		if (rc) {
			xocl_err(pdev, "failed to copy (%s, %s): %d",
				epname, regmap, rc);
		} else {
			rc = xocl_md_set_prop(dev, dst, epname, regmap,
				PROP_PARTITION_LEVEL, &level, sizeof(level));
			if (rc) {
				xocl_err(pdev,
					"can't set level for (%s, %s): %d",
					epname, regmap, rc);
			}
		}
	}
	return rc;
}

static char *xmgmt_mailbox_user_dtb(struct platform_device *pdev)
{
	/* TODO: add support for PLP. */
	const char *src = NULL;
	char *dst = NULL;
	struct device *dev = DEV(pdev);
	int rc = xocl_md_create(dev, &dst);

	if (rc || dst == NULL)
		return NULL;

	rc = xmgmt_mailbox_dtb_add_vbnv(pdev, dst);
	if (rc)
		goto fail;

	src = xmgmt_get_dtb(pdev, XMGMT_BLP);
	if (src == NULL) {
		xocl_err(pdev, "failed to get BLP dtb");
		goto fail;
	}

	rc = xmgmt_mailbox_dtb_copy_logic_uuid(pdev, src, dst);
	if (rc)
		goto fail;

	rc = xmgmt_mailbox_dtb_add_vrom(pdev, src, dst);
	if (rc)
		goto fail;

	rc = xocl_md_copy_endpoint(dev, dst, src, NODE_PARTITION_INFO,
		NULL, NODE_PARTITION_INFO_BLP);
	if (rc)
		goto fail;

	rc = xocl_md_copy_endpoint(dev, dst, src, NODE_INTERFACES, NULL, NULL);
	if (rc)
		goto fail;

	rc = xmgmt_mailbox_dtb_copy_user_endpoints(pdev, src, dst);
	if (rc)
		goto fail;

	xocl_md_pack(dev, dst);
	vfree(src);
	return dst;

fail:
	vfree(src);
	vfree(dst);
	return NULL;
}

static void xmgmt_mailbox_resp_subdev(struct xmgmt_mailbox *xmbx,
	u64 msgid, bool sw_ch, u64 offset, u64 size)
{
	struct platform_device *pdev = xmbx->pdev;
	char *dtb = xmgmt_mailbox_user_dtb(pdev);
	long dtbsz;
	struct xcl_subdev *hdr;
	u64 totalsz;

	if (dtb == NULL)
		return;

	dtbsz = xocl_md_size(DEV(pdev), dtb);
	totalsz = dtbsz + sizeof(*hdr) - sizeof(hdr->data);
	if (offset != 0 || totalsz > size) {
		/* Only support fetching dtb in one shot. */
		vfree(dtb);
		xocl_err(pdev, "need %lldB, user buffer size is %lldB, dropped",
			totalsz, size);
		return;
	}

	hdr = vzalloc(totalsz);
	if (hdr == NULL) {
		vfree(dtb);
		return;
	}

	hdr->ver = 1;
	hdr->size = dtbsz;
	hdr->rtncode = XOCL_MSG_SUBDEV_RTN_COMPLETE;
	(void) memcpy(hdr->data, dtb, dtbsz);

	mutex_lock(&xmbx->lock);
	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, hdr, totalsz);
	mutex_unlock(&xmbx->lock);

	vfree(dtb);
	vfree(hdr);
}

static void xmgmt_mailbox_resp_peer_data(struct xmgmt_mailbox *xmbx,
	struct xcl_mailbox_req *req, size_t len, u64 msgid, bool sw_ch)
{
	struct xcl_mailbox_peer_data *pdata =
		(struct xcl_mailbox_peer_data *)req->data;

	if (len < (sizeof(*req) + sizeof(*pdata) - 1)) {
		xocl_err(xmbx->pdev, "received corrupted %s, dropped",
			mailbox_req2name(req->req));
		return;
	}

	switch (pdata->kind) {
	case XCL_SUBDEV:
		xmgmt_mailbox_resp_subdev(xmbx, msgid, sw_ch,
			pdata->offset, pdata->size);
		break;
	default:
		xocl_err(xmbx->pdev, "%s(%s) request not handled",
			mailbox_req2name(req->req),
			mailbox_group_kind2name(pdata->kind));
		break;
	}
}

static void xmgmt_mailbox_simple_resp(struct xmgmt_mailbox *xmbx,
	u64 msgid, bool sw_ch, int rc)
{
	mutex_lock(&xmbx->lock);
	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, &rc, sizeof(rc));
	mutex_unlock(&xmbx->lock);
}

static bool xmgmt_mailbox_is_same_domain(struct xmgmt_mailbox *xmbx,
	struct xcl_mailbox_conn *mb_conn)
{
	uint32_t crc_chk;
	phys_addr_t paddr;
	struct platform_device *pdev = xmbx->pdev;

	paddr = virt_to_phys((void *)mb_conn->kaddr);
	if (paddr != (phys_addr_t)mb_conn->paddr) {
		xocl_info(pdev, "paddrs differ, user 0x%llx, mgmt 0x%llx",
			mb_conn->paddr, paddr);
		return false;
	}

	crc_chk = crc32c_le(~0, (void *)mb_conn->kaddr, PAGE_SIZE);
	if (crc_chk != mb_conn->crc32) {
		xocl_info(pdev, "CRCs differ, user 0x%x, mgmt 0x%x",
			mb_conn->crc32, crc_chk);
		return false;
	}

	return true;
}

static void xmgmt_mailbox_resp_user_probe(struct xmgmt_mailbox *xmbx,
	struct xcl_mailbox_req *req, size_t len, u64 msgid, bool sw_ch)
{
	struct xcl_mailbox_conn_resp *resp = vzalloc(sizeof(*resp));
	struct xcl_mailbox_conn *conn = (struct xcl_mailbox_conn *)req->data;

	if (resp == NULL)
		return;

	if (len < (sizeof(*req) + sizeof(*conn) - 1)) {
		xocl_err(xmbx->pdev, "received corrupted %s, dropped",
			mailbox_req2name(req->req));
		vfree(resp);
		return;
	}

	resp->conn_flags |= XCL_MB_PEER_READY;
	if (xmgmt_mailbox_is_same_domain(xmbx, conn))
		resp->conn_flags |= XCL_MB_PEER_SAME_DOMAIN;

	mutex_lock(&xmbx->lock);
	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, resp, sizeof(*resp));
	mutex_unlock(&xmbx->lock);
	vfree(resp);
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
	case XCL_MAILBOX_REQ_PEER_DATA:
		xmgmt_mailbox_resp_peer_data(xmbx, req, len, msgid, sw_ch);
		break;
	case XCL_MAILBOX_REQ_READ_P2P_BAR_ADDR:
		xmgmt_mailbox_simple_resp(xmbx, msgid, sw_ch, -ENOTSUPP);
		break;
	case XCL_MAILBOX_REQ_USER_PROBE:
		xmgmt_mailbox_resp_user_probe(xmbx, req, len, msgid, sw_ch);
		break;
	default:
		xocl_err(pdev, "%s(%d) request not handled",
			mailbox_req2name(req->req), req->req);
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
		mutex_unlock(&xmbx->lock);
		break;
	case XOCL_EVENT_PRE_REMOVAL:
		BUG_ON(esd->xevt_subdev_id != XOCL_SUBDEV_MAILBOX);
		BUG_ON(!xmbx->mailbox);
		mutex_lock(&xmbx->lock);
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

static ssize_t xmgmt_mailbox_user_dtb_show(struct file *filp,
	struct kobject *kobj, struct bin_attribute *attr,
	char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct platform_device *pdev = to_platform_device(dev);
	char *blob = NULL;
	long  size;
	ssize_t ret = 0;

	blob = xmgmt_mailbox_user_dtb(pdev);
	if (!blob) {
		ret = -ENOENT;
		goto failed;
	}

	size = xocl_md_size(dev, blob);
	if (size <= 0) {
		ret = -EINVAL;
		goto failed;
	}

	if (off >= size)
		goto failed;
	if (off + count > size)
		count = size - off;
	memcpy(buf, blob + off, count);

	ret = count;
failed:
	vfree(blob);
	return ret;
}

static struct bin_attribute meta_data_attr = {
	.attr = {
		.name = "metadata_for_user",
		.mode = 0400
	},
	.read = xmgmt_mailbox_user_dtb_show,
	.size = 0
};

static struct bin_attribute  *xmgmt_mailbox_bin_attrs[] = {
	&meta_data_attr,
	NULL,
};

static const struct attribute_group xmgmt_mailbox_attrgroup = {
	.bin_attrs = xmgmt_mailbox_bin_attrs,
};

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
	(void) sysfs_create_group(&DEV(pdev)->kobj, &xmgmt_mailbox_attrgroup);
	return xmbx;
}

void xmgmt_mailbox_remove(void *handle)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)handle;
	struct platform_device *pdev = xmbx->pdev;

	(void) sysfs_remove_group(&DEV(pdev)->kobj, &xmgmt_mailbox_attrgroup);
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

void xmgmt_peer_notify_state(void *handle, bool online)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)handle;
	struct xcl_mailbox_peer_state *st;
	struct xcl_mailbox_req *req;
	size_t reqlen = sizeof(*req) + sizeof(*st) - 1;

	req = vzalloc(reqlen);
	if (req == NULL)
		return;

	req->req = XCL_MAILBOX_REQ_MGMT_STATE;
	st = (struct xcl_mailbox_peer_state *)req->data;
	st->state_flags = online ? XCL_MB_STATE_ONLINE : XCL_MB_STATE_OFFLINE;
	mutex_lock(&xmbx->lock);
	xmgmt_mailbox_notify(xmbx, false, req, reqlen);
	mutex_unlock(&xmbx->lock);
}
