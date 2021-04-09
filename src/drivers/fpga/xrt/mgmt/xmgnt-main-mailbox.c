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
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/xrt/mailbox_proto.h>
#include "xmgnt.h"
#include "xleaf/mailbox.h"
#include "xleaf/cmc.h"
#include "metadata.h"
#include "xclbin-helper.h"
#include "xleaf/clock.h"
#include "xleaf/ddr_calibration.h"
#include "xleaf/icap.h"

struct xmgmt_mailbox {
	struct xrt_device *xdev;
	struct xrt_device *mailbox;
	struct mutex lock; /* lock for xmgmt_mailbox */
	char *test_msg;
	bool peer_in_same_domain;
};

static inline const char *mailbox_chan2name(bool sw_ch)
{
	return sw_ch ? "SW" : "HW";
}

static inline void xmgmt_mailbox_prt_req(struct xmgmt_mailbox *xmbx, bool send,
					 struct xcl_mailbox_req *request, bool sw_ch)
{
	const char *dir = send ? ">>>" : "<<<";

	if (request->req == XCL_MAILBOX_REQ_PEER_DATA) {
		struct xcl_mailbox_peer_data *p = (struct xcl_mailbox_peer_data *)request->data;

		xrt_info(xmbx->xdev, "%s(%s) %s%s%s", mailbox_req2name(request->req),
			 mailbox_group_kind2name(p->kind), dir, mailbox_chan2name(sw_ch), dir);
	} else {
		xrt_info(xmbx->xdev, "%s %s%s%s", mailbox_req2name(request->req),
			 dir, mailbox_chan2name(sw_ch), dir);
	}
}

#define XMGMT_MAILBOX_PRT_REQ_SEND(xmbx, req, sw_ch)			\
	xmgmt_mailbox_prt_req(xmbx, true, req, sw_ch)
#define XMGMT_MAILBOX_PRT_REQ_RECV(xmbx, req, sw_ch)			\
	xmgmt_mailbox_prt_req(xmbx, false, req, sw_ch)

static inline void xmgmt_mailbox_prt_resp(struct xmgmt_mailbox *xmbx,
					  struct xrt_mailbox_post *resp)
{
	xrt_info(xmbx->xdev, "respond %zu bytes >>>%s>>>", resp->xmip_data_size,
		 mailbox_chan2name((resp)->xmip_sw_ch));
}

static inline struct xmgmt_mailbox *xdev2mbx(struct xrt_device *xdev)
{
	return (struct xmgmt_mailbox *)xmgmt_xdev2mailbox(xdev);
}

static void xmgmt_mailbox_post(struct xmgmt_mailbox *xmbx,
			       u64 msgid, bool sw_ch, void *buf, size_t len)
{
	struct xrt_mailbox_post post = {
		.xmip_req_id = msgid,
		.xmip_sw_ch = sw_ch,
		.xmip_data = buf,
		.xmip_data_size = len
	};
	int rc;

	WARN_ON(!mutex_is_locked(&xmbx->lock));

	if (!xmbx->mailbox) {
		xrt_err(xmbx->xdev, "mailbox not available");
		return;
	}

	if (msgid == 0)
		XMGMT_MAILBOX_PRT_REQ_SEND(xmbx, (struct xcl_mailbox_req *)buf, sw_ch);
	else
		xmgmt_mailbox_prt_resp(xmbx, &post);

	rc = xleaf_call(xmbx->mailbox, XRT_MAILBOX_POST, &post);
	if (rc && rc != -ESHUTDOWN)
		xrt_err(xmbx->xdev, "failed to post msg: %d", rc);
}

static void xmgmt_mailbox_notify(struct xmgmt_mailbox *xmbx, bool sw_ch,
				 struct xcl_mailbox_req *req, size_t len)
{
	xmgmt_mailbox_post(xmbx, 0, sw_ch, req, len);
}

static void xmgmt_mailbox_respond(struct xmgmt_mailbox *xmbx,
				  u64 msgid, bool sw_ch, void *buf, size_t len)
{
	mutex_lock(&xmbx->lock);
	xmgmt_mailbox_post(xmbx, msgid, sw_ch, buf, len);
	mutex_unlock(&xmbx->lock);
}

static void xmgmt_mailbox_resp_test_msg(struct xmgmt_mailbox *xmbx, u64 msgid, bool sw_ch)
{
	struct xrt_device *xdev = xmbx->xdev;
	char *msg;

	mutex_lock(&xmbx->lock);

	if (!xmbx->test_msg) {
		mutex_unlock(&xmbx->lock);
		xrt_err(xdev, "test msg is not set, drop request");
		return;
	}
	msg = xmbx->test_msg;
	xmbx->test_msg = NULL;

	mutex_unlock(&xmbx->lock);

	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, msg, strlen(msg) + 1);
	vfree(msg);
}

static int xmgmt_mailbox_dtb_add_prop(struct xrt_device *xdev,
				      char *dst_dtb, const char *ep_name, const char *regmap_name,
				      const char *prop, const void *val, int size)
{
	int rc = xrt_md_set_prop(DEV(xdev), dst_dtb, ep_name, regmap_name, prop, val, size);

	if (rc)
		xrt_err(xdev, "failed to set %s@(%s, %s): %d", ep_name, regmap_name, prop, rc);
	return rc;
}

static int xmgmt_mailbox_dtb_add_vbnv(struct xrt_device *xdev, char *dtb)
{
	int rc = 0;
	char *vbnv = xmgmt_get_vbnv(xdev);

	if (!vbnv) {
		xrt_err(xdev, "failed to get VBNV");
		return -ENOENT;
	}
	rc = xmgmt_mailbox_dtb_add_prop(xdev, dtb, NULL, NULL, XRT_MD_PROP_VBNV,
					vbnv, strlen(vbnv) + 1);
	kfree(vbnv);
	return rc;
}

static int xmgmt_mailbox_dtb_copy_logic_uuid(struct xrt_device *xdev,
					     const char *src_dtb, char *dst_dtb)
{
	const void *val;
	int sz;
	int rc = xrt_md_get_prop(DEV(xdev), src_dtb, NULL, NULL, XRT_MD_PROP_LOGIC_UUID, &val, &sz);

	if (rc) {
		xrt_err(xdev, "failed to get %s: %d", XRT_MD_PROP_LOGIC_UUID, rc);
		return rc;
	}
	return xmgmt_mailbox_dtb_add_prop(xdev, dst_dtb, NULL, NULL,
					  XRT_MD_PROP_LOGIC_UUID, val, sz);
}

static int xmgmt_mailbox_dtb_add_vrom(struct xrt_device *xdev,
				      const char *src_dtb, char *dst_dtb)
{
	/* For compatibility for legacy xrt driver. */
	enum feature_bit_mask {
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
	struct feature_rom_header {
		unsigned char entry_point_string[4];
		u8 major_version;
		u8 minor_version;
		u32 vivado_build_id;
		u32 ip_build_id;
		u64 time_since_ephoc;
		unsigned char fpga_part_number[64];
		unsigned char vbnv_name[64];
		u8 ddr_channel_count;
		u8 ddr_channel_size;
		u64 dr_base_address;
		u64 feature_bitmap;
		unsigned char uuid[16];
		u8 hbm_count;
		u8 hbm_size;
		u32 cdma_base_address[4];
	} header = { 0 };
	char *vbnv = xmgmt_get_vbnv(xdev);
	const u64 *rng;
	int rc;

	*(u32 *)header.entry_point_string = 0x786e6c78;

	if (vbnv)
		strncpy(header.vbnv_name, vbnv, sizeof(header.vbnv_name) - 1);
	kfree(vbnv);

	header.feature_bitmap = UNIFIED_PLATFORM;
	rc = xrt_md_get_prop(DEV(xdev), src_dtb, XRT_MD_NODE_CMC_FW_MEM, NULL,
			     XRT_MD_PROP_IO_OFFSET, (const void **)&rng, NULL);
	if (rc == 0)
		header.feature_bitmap |= BOARD_MGMT_ENBLD;
	rc = xrt_md_get_prop(DEV(xdev), src_dtb, XRT_MD_NODE_ERT_FW_MEM, NULL,
			     XRT_MD_PROP_IO_OFFSET, (const void **)&rng, NULL);
	if (rc == 0)
		header.feature_bitmap |= MB_SCHEDULER;

	return xmgmt_mailbox_dtb_add_prop(xdev, dst_dtb, NULL, NULL,
					  XRT_MD_PROP_VROM, &header, sizeof(header));
}

static u32 xmgmt_mailbox_dtb_user_pf(struct xrt_device *xdev,
				     const char *dtb, const char *epname, const char *regmap)
{
	const u32 *pfnump;
	int rc = xrt_md_get_prop(DEV(xdev), dtb, epname, regmap,
				 XRT_MD_PROP_PF_NUM, (const void **)&pfnump, NULL);

	if (rc)
		return -1;
	return be32_to_cpu(*pfnump);
}

static int xmgmt_mailbox_dtb_copy_user_endpoints(struct xrt_device *xdev,
						 const char *src, char *dst)
{
	int rc = 0;
	char *epname = NULL, *regmap = NULL;
	u32 pfnum = xmgmt_mailbox_dtb_user_pf(xdev, src, XRT_MD_NODE_MAILBOX_USER, NULL);
	const u32 level = cpu_to_be32(1);
	struct device *dev = DEV(xdev);

	if (pfnum == (u32)-1) {
		xrt_err(xdev, "failed to get user pf num");
		rc = -EINVAL;
	}

	for (xrt_md_get_next_endpoint(dev, src, NULL, NULL, &epname, &regmap);
		rc == 0 && epname;
		xrt_md_get_next_endpoint(dev, src, epname, regmap, &epname, &regmap)) {
		if (pfnum !=
			xmgmt_mailbox_dtb_user_pf(xdev, src, epname, regmap))
			continue;
		rc = xrt_md_copy_endpoint(dev, dst, src, epname, regmap, NULL);
		if (rc) {
			xrt_err(xdev, "failed to copy (%s, %s): %d", epname, regmap, rc);
		} else {
			rc = xrt_md_set_prop(dev, dst, epname, regmap,
					     XRT_MD_PROP_PARTITION_LEVEL, &level, sizeof(level));
			if (rc) {
				xrt_err(xdev, "can't set level for (%s, %s): %d",
					epname, regmap, rc);
			}
		}
	}
	return rc;
}

static char *xmgmt_mailbox_user_dtb(struct xrt_device *xdev)
{
	const char *src = NULL;
	char *dst = NULL;
	struct device *dev = DEV(xdev);
	int rc = xrt_md_create(dev, &dst);

	if (rc || !dst)
		return NULL;

	rc = xmgmt_mailbox_dtb_add_vbnv(xdev, dst);
	if (rc)
		goto fail;

	src = xmgmt_get_dtb(xdev, XMGMT_BLP);
	if (!src) {
		xrt_err(xdev, "failed to get BLP dtb");
		goto fail;
	}

	rc = xmgmt_mailbox_dtb_copy_logic_uuid(xdev, src, dst);
	if (rc)
		goto fail;

	rc = xmgmt_mailbox_dtb_add_vrom(xdev, src, dst);
	if (rc)
		goto fail;

	rc = xrt_md_copy_endpoint(dev, dst, src, XRT_MD_NODE_PARTITION_INFO,
				  NULL, XRT_MD_NODE_PARTITION_INFO_BLP);
	if (rc)
		goto fail;

	rc = xrt_md_copy_endpoint(dev, dst, src, XRT_MD_NODE_INTERFACES, NULL, NULL);
	if (rc)
		goto fail;

	rc = xmgmt_mailbox_dtb_copy_user_endpoints(xdev, src, dst);
	if (rc)
		goto fail;

	xrt_md_pack(dev, dst);
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
	struct xrt_device *xdev = xmbx->xdev;
	char *dtb = xmgmt_mailbox_user_dtb(xdev);
	long dtbsz;
	struct xcl_subdev *hdr;
	u64 totalsz;

	if (!dtb)
		return;

	dtbsz = xrt_md_size(DEV(xdev), dtb);
	totalsz = dtbsz + sizeof(*hdr) - sizeof(hdr->data);
	if (offset != 0 || totalsz > size) {
		/* Only support fetching dtb in one shot. */
		vfree(dtb);
		xrt_err(xdev, "need %lldB, user buffer size is %lldB, dropped", totalsz, size);
		return;
	}

	hdr = vzalloc(totalsz);
	if (!hdr) {
		vfree(dtb);
		return;
	}

	hdr->ver = 1;
	hdr->size = dtbsz;
	hdr->rtncode = XRT_MSG_SUBDEV_RTN_COMPLETE;
	memcpy(hdr->data, dtb, dtbsz);

	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, hdr, totalsz);

	vfree(dtb);
	vfree(hdr);
}

static void xmgmt_mailbox_resp_sensor(struct xmgmt_mailbox *xmbx,
				      u64 msgid, bool sw_ch, u64 offset, u64 size)
{
	struct xrt_device *xdev = xmbx->xdev;
	struct xcl_sensor sensors = { 0 };
	struct xrt_device *cmcxdev =
		xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_CMC, XRT_INVALID_DEVICE_INST);
	int rc;

	if (cmcxdev) {
		rc = xleaf_call(cmcxdev, XRT_CMC_READ_SENSORS, &sensors);
		xleaf_put_leaf(xdev, cmcxdev);
		if (rc)
			xrt_err(xdev, "can't read sensors: %d", rc);
	}

	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, &sensors, min((u64)sizeof(sensors), size));
}

static int xmgmt_mailbox_get_freq(struct xmgmt_mailbox *xmbx,
				  enum XCLBIN_CLOCK_TYPE type, u64 *freq, u64 *freq_cnter)
{
	struct xrt_device *xdev = xmbx->xdev;
	const char *clkname =
		xrt_clock_type2epname(type) ?
		xrt_clock_type2epname(type) : "UNKNOWN";
	struct xrt_device *clkxdev =
		xleaf_get_leaf_by_epname(xdev, clkname);
	int rc;
	struct xrt_clock_get getfreq = { 0 };

	if (!clkxdev) {
		xrt_info(xdev, "%s clock is not available", clkname);
		return -ENOENT;
	}

	rc = xleaf_call(clkxdev, XRT_CLOCK_GET, &getfreq);
	xleaf_put_leaf(xdev, clkxdev);
	if (rc) {
		xrt_err(xdev, "can't get %s clock frequency: %d", clkname, rc);
		return rc;
	}

	if (freq)
		*freq = getfreq.freq;
	if (freq_cnter)
		*freq_cnter = getfreq.freq_cnter;
	return 0;
}

static int xmgmt_mailbox_get_icap_idcode(struct xmgmt_mailbox *xmbx, u64 *id)
{
	struct xrt_device *xdev = xmbx->xdev;
	struct xrt_device *icapxdev =
		xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_ICAP, XRT_INVALID_DEVICE_INST);
	int rc;

	if (!icapxdev) {
		xrt_err(xdev, "can't find icap");
		return -ENOENT;
	}

	rc = xleaf_call(icapxdev, XRT_ICAP_GET_IDCODE, id);
	xleaf_put_leaf(xdev, icapxdev);
	if (rc)
		xrt_err(xdev, "can't get icap idcode: %d", rc);
	return rc;
}

static int xmgmt_mailbox_get_mig_calib(struct xmgmt_mailbox *xmbx, u64 *calib)
{
	struct xrt_device *xdev = xmbx->xdev;
	struct xrt_device *calibxdev =
		xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_CALIB, XRT_INVALID_DEVICE_INST);
	int rc;
	enum xrt_calib_results res;

	if (!calibxdev) {
		xrt_err(xdev, "can't find mig calibration subdev");
		return -ENOENT;
	}

	rc = xleaf_call(calibxdev, XRT_CALIB_RESULT, &res);
	xleaf_put_leaf(xdev, calibxdev);
	if (rc) {
		xrt_err(xdev, "can't get mig calibration result: %d", rc);
	} else {
		if (res == XRT_CALIB_SUCCEEDED)
			*calib = 1;
		else
			*calib = 0;
	}
	return rc;
}

static void xmgmt_mailbox_resp_icap(struct xmgmt_mailbox *xmbx,
				    u64 msgid, bool sw_ch, u64 offset, u64 size)
{
	struct xrt_device *xdev = xmbx->xdev;
	struct xcl_pr_region icap = { 0 };

	xmgmt_mailbox_get_freq(xmbx, CT_DATA, &icap.freq_data, &icap.freq_cntr_data);
	xmgmt_mailbox_get_freq(xmbx, CT_KERNEL, &icap.freq_kernel, &icap.freq_cntr_kernel);
	xmgmt_mailbox_get_freq(xmbx, CT_SYSTEM, &icap.freq_system, &icap.freq_cntr_system);
	xmgmt_mailbox_get_icap_idcode(xmbx, &icap.idcode);
	xmgmt_mailbox_get_mig_calib(xmbx, &icap.mig_calib);
	WARN_ON(sizeof(icap.uuid) != sizeof(uuid_t));
	xmgmt_get_provider_uuid(xdev, XMGMT_ULP, (uuid_t *)&icap.uuid);

	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, &icap, min_t(u64, sizeof(icap), size));
}

static void xmgmt_mailbox_resp_bdinfo(struct xmgmt_mailbox *xmbx,
				      u64 msgid, bool sw_ch, u64 offset, u64 size)
{
	struct xrt_device *xdev = xmbx->xdev;
	struct xcl_board_info *info = vzalloc(sizeof(*info));
	struct xrt_device *cmcxdev;
	int rc;

	if (!info)
		return;

	cmcxdev = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_CMC, XRT_INVALID_DEVICE_INST);
	if (cmcxdev) {
		rc = xleaf_call(cmcxdev, XRT_CMC_READ_BOARD_INFO, info);
		xleaf_put_leaf(xdev, cmcxdev);
		if (rc)
			xrt_err(xdev, "can't read board info: %d", rc);
	}

	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, info, min((u64)sizeof(*info), size));

	vfree(info);
}

static void xmgmt_mailbox_simple_respond(struct xmgmt_mailbox *xmbx, u64 msgid, bool sw_ch, int rc)
{
	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, &rc, sizeof(rc));
}

static void xmgmt_mailbox_resp_peer_data(struct xmgmt_mailbox *xmbx, struct xcl_mailbox_req *req,
					 size_t len, u64 msgid, bool sw_ch)
{
	struct xcl_mailbox_peer_data *pdata = (struct xcl_mailbox_peer_data *)req->data;

	if (len < (sizeof(*req) + sizeof(*pdata) - 1)) {
		xrt_err(xmbx->xdev, "received corrupted %s, dropped", mailbox_req2name(req->req));
		return;
	}

	switch (pdata->kind) {
	case XCL_SENSOR:
		xmgmt_mailbox_resp_sensor(xmbx, msgid, sw_ch, pdata->offset, pdata->size);
		break;
	case XCL_ICAP:
		xmgmt_mailbox_resp_icap(xmbx, msgid, sw_ch, pdata->offset, pdata->size);
		break;
	case XCL_BDINFO:
		xmgmt_mailbox_resp_bdinfo(xmbx, msgid, sw_ch, pdata->offset, pdata->size);
		break;
	case XCL_SUBDEV:
		xmgmt_mailbox_resp_subdev(xmbx, msgid, sw_ch, pdata->offset, pdata->size);
		break;
	case XCL_MIG_ECC:
	case XCL_FIREWALL:
	case XCL_DNA:
		xmgmt_mailbox_simple_respond(xmbx, msgid, sw_ch, 0);
		break;
	default:
		xrt_err(xmbx->xdev, "%s(%s) request not handled", mailbox_req2name(req->req),
			mailbox_group_kind2name(pdata->kind));
		break;
	}
}

static bool xmgmt_mailbox_is_same_domain(struct xmgmt_mailbox *xmbx,
					 struct xcl_mailbox_conn *mb_conn)
{
	u32 crc_chk;
	phys_addr_t paddr;
	struct xrt_device *xdev = xmbx->xdev;

	paddr = virt_to_phys((void *)mb_conn->kaddr);
	if (paddr != (phys_addr_t)mb_conn->paddr) {
		xrt_info(xdev, "paddrs differ, user 0x%llx, mgmt 0x%llx", mb_conn->paddr, paddr);
		return false;
	}

	crc_chk = crc32c_le(~0, (void *)mb_conn->kaddr, PAGE_SIZE);
	if (crc_chk != mb_conn->crc32) {
		xrt_info(xdev, "CRCs differ, user 0x%x, mgmt 0x%x", mb_conn->crc32, crc_chk);
		return false;
	}

	return true;
}

static void xmgmt_mailbox_resp_user_probe(struct xmgmt_mailbox *xmbx, struct xcl_mailbox_req *req,
					  size_t len, u64 msgid, bool sw_ch)
{
	struct xcl_mailbox_conn_resp *resp = vzalloc(sizeof(*resp));
	struct xcl_mailbox_conn *conn = (struct xcl_mailbox_conn *)req->data;

	if (!resp)
		return;

	if (len < (sizeof(*req) + sizeof(*conn) - 1)) {
		xrt_err(xmbx->xdev, "received corrupted %s, dropped", mailbox_req2name(req->req));
		vfree(resp);
		return;
	}

	resp->conn_flags |= XCL_MB_PEER_READY;
	if (xmgmt_mailbox_is_same_domain(xmbx, conn)) {
		xmbx->peer_in_same_domain = true;
		resp->conn_flags |= XCL_MB_PEER_SAME_DOMAIN;
	}

	xmgmt_mailbox_respond(xmbx, msgid, sw_ch, resp, sizeof(*resp));
	vfree(resp);
}

static void xmgmt_mailbox_resp_hot_reset(struct xmgmt_mailbox *xmbx, struct xcl_mailbox_req *req,
					 size_t len, u64 msgid, bool sw_ch)
{
	int ret;
	struct xrt_device *xdev = xmbx->xdev;

	xmgmt_mailbox_simple_respond(xmbx, msgid, sw_ch, 0);

	ret = xmgmt_hot_reset(xdev);
	if (ret)
		xrt_err(xdev, "failed to hot reset: %d", ret);
	else
		xmgmt_peer_notify_state(xmbx, true);
}

static void xmgmt_mailbox_resp_load_xclbin(struct xmgmt_mailbox *xmbx, struct xcl_mailbox_req *req,
					   size_t len, u64 msgid, bool sw_ch)
{
	struct xcl_mailbox_bitstream_kaddr *kaddr =
		(struct xcl_mailbox_bitstream_kaddr *)req->data;
	void *xclbin = (void *)(uintptr_t)kaddr->addr;
	int ret = bitstream_axlf_mailbox(xmbx->xdev, xclbin);

	xmgmt_mailbox_simple_respond(xmbx, msgid, sw_ch, ret);
}

static void xmgmt_mailbox_listener(void *arg, void *data, size_t len,
				   u64 msgid, int err, bool sw_ch)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)arg;
	struct xrt_device *xdev = xmbx->xdev;
	struct xcl_mailbox_req *req = (struct xcl_mailbox_req *)data;

	if (err) {
		xrt_err(xdev, "failed to receive request: %d", err);
		return;
	}
	if (len < sizeof(*req)) {
		xrt_err(xdev, "received corrupted request");
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
		xmgmt_mailbox_simple_respond(xmbx, msgid, sw_ch, -ENOTSUPP);
		break;
	case XCL_MAILBOX_REQ_USER_PROBE:
		xmgmt_mailbox_resp_user_probe(xmbx, req, len, msgid, sw_ch);
		break;
	case XCL_MAILBOX_REQ_HOT_RESET:
		xmgmt_mailbox_resp_hot_reset(xmbx, req, len, msgid, sw_ch);
		break;
	case XCL_MAILBOX_REQ_LOAD_XCLBIN_KADDR:
		if (xmbx->peer_in_same_domain) {
			xmgmt_mailbox_resp_load_xclbin(xmbx, req, len, msgid, sw_ch);
		} else {
			xrt_err(xdev, "%s not handled, not in same domain",
				mailbox_req2name(req->req));
		}
		break;
	default:
		xrt_err(xdev, "%s(%d) request not handled", mailbox_req2name(req->req), req->req);
		break;
	}
}

static void xmgmt_mailbox_reg_listener(struct xmgmt_mailbox *xmbx)
{
	struct xrt_mailbox_listen listen = { xmgmt_mailbox_listener, xmbx };

	WARN_ON(!mutex_is_locked(&xmbx->lock));
	if (!xmbx->mailbox)
		return;
	xleaf_call(xmbx->mailbox, XRT_MAILBOX_LISTEN, &listen);
}

static void xmgmt_mailbox_unreg_listener(struct xmgmt_mailbox *xmbx)
{
	struct xrt_mailbox_listen listen = { 0 };

	WARN_ON(!mutex_is_locked(&xmbx->lock));
	WARN_ON(!xmbx->mailbox);
	xleaf_call(xmbx->mailbox, XRT_MAILBOX_LISTEN, &listen);
}

void xmgmt_mailbox_event_cb(struct xrt_device *xdev, void *arg)
{
	struct xmgmt_mailbox *xmbx = xdev2mbx(xdev);
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id = evt->xe_subdev.xevt_subdev_id;

	if (id != XRT_SUBDEV_MAILBOX)
		return;

	switch (e) {
	case XRT_EVENT_POST_CREATION:
		WARN_ON(xmbx->mailbox);
		mutex_lock(&xmbx->lock);
		xmbx->mailbox = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_MAILBOX,
						     XRT_INVALID_DEVICE_INST);
		xmgmt_mailbox_reg_listener(xmbx);
		mutex_unlock(&xmbx->lock);
		break;
	case XRT_EVENT_PRE_REMOVAL:
		WARN_ON(!xmbx->mailbox);
		mutex_lock(&xmbx->lock);
		xmgmt_mailbox_unreg_listener(xmbx);
		xleaf_put_leaf(xdev, xmbx->mailbox);
		xmbx->mailbox = NULL;
		mutex_unlock(&xmbx->lock);
		break;
	default:
		break;
	}
}

static ssize_t xmgmt_mailbox_user_dtb_show(struct file *filp, struct kobject *kobj,
					   struct bin_attribute *attr, char *buf,
					   loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct xrt_device *xdev = to_xrt_dev(dev);
	char *blob = NULL;
	long  size;
	ssize_t ret = 0;

	blob = xmgmt_mailbox_user_dtb(xdev);
	if (!blob) {
		ret = -ENOENT;
		goto failed;
	}

	size = xrt_md_size(dev, blob);
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

static int xmgmt_mailbox_get_test_msg(struct xmgmt_mailbox *xmbx, bool sw_ch,
				      char *buf, size_t *len)
{
	int rc;
	struct xrt_device *xdev = xmbx->xdev;
	struct xcl_mailbox_req req = { 0, XCL_MAILBOX_REQ_TEST_READ, };
	struct xrt_mailbox_request leaf_req = {
		.xmir_sw_ch = sw_ch,
		.xmir_resp_ttl = 1,
		.xmir_req = &req,
		.xmir_req_size = sizeof(req),
		.xmir_resp = buf,
		.xmir_resp_size = *len
	};

	mutex_lock(&xmbx->lock);
	if (xmbx->mailbox) {
		XMGMT_MAILBOX_PRT_REQ_SEND(xmbx, &req, leaf_req.xmir_sw_ch);
		/*
		 * mgmt should never send request to peer. it should send
		 * either notification or response. here is the only exception
		 * for debugging purpose.
		 */
		rc = xleaf_call(xmbx->mailbox, XRT_MAILBOX_REQUEST, &leaf_req);
	} else {
		rc = -ENODEV;
		xrt_err(xdev, "mailbox not available");
	}
	mutex_unlock(&xmbx->lock);

	if (rc == 0)
		*len = leaf_req.xmir_resp_size;
	return rc;
}

static int xmgmt_mailbox_set_test_msg(struct xmgmt_mailbox *xmbx, char *buf, size_t len)
{
	mutex_lock(&xmbx->lock);

	if (xmbx->test_msg)
		vfree(xmbx->test_msg);
	xmbx->test_msg = vmalloc(len);
	if (!xmbx->test_msg) {
		mutex_unlock(&xmbx->lock);
		return -ENOMEM;
	}
	memcpy(xmbx->test_msg, buf, len);

	mutex_unlock(&xmbx->lock);
	return 0;
}

static ssize_t peer_msg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	size_t len = 4096;
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xmgmt_mailbox *xmbx = xdev2mbx(xdev);
	int ret = xmgmt_mailbox_get_test_msg(xmbx, false, buf, &len);

	return ret == 0 ? len : ret;
}

static ssize_t peer_msg_store(struct device *dev,
			      struct device_attribute *da, const char *buf, size_t count)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xmgmt_mailbox *xmbx = xdev2mbx(xdev);
	int ret = xmgmt_mailbox_set_test_msg(xmbx, (char *)buf, count);

	return ret == 0 ? count : ret;
}

/* Message test i/f. */
static DEVICE_ATTR_RW(peer_msg);

static struct attribute *xmgmt_mailbox_attrs[] = {
	&dev_attr_peer_msg.attr,
	NULL,
};

static const struct attribute_group xmgmt_mailbox_attrgroup = {
	.bin_attrs = xmgmt_mailbox_bin_attrs,
	.attrs = xmgmt_mailbox_attrs,
};

void *xmgmt_mailbox_probe(struct xrt_device *xdev)
{
	struct xmgmt_mailbox *xmbx = devm_kzalloc(DEV(xdev), sizeof(*xmbx), GFP_KERNEL);
	int ret;

	if (!xmbx)
		return NULL;
	xmbx->xdev = xdev;
	mutex_init(&xmbx->lock);

	ret = sysfs_create_group(&DEV(xdev)->kobj, &xmgmt_mailbox_attrgroup);
	if (ret) {
		xrt_err(xdev, "create sysfs group failed, ret %d", ret);
		return NULL;
	}

	return xmbx;
}

void xmgmt_mailbox_remove(void *handle)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)handle;
	struct xrt_device *xdev = xmbx->xdev;

	sysfs_remove_group(&DEV(xdev)->kobj, &xmgmt_mailbox_attrgroup);
	if (xmbx->mailbox)
		xleaf_put_leaf(xdev, xmbx->mailbox);
	if (xmbx->test_msg)
		vfree(xmbx->test_msg);
}

void xmgmt_peer_notify_state(void *handle, bool online)
{
	struct xmgmt_mailbox *xmbx = (struct xmgmt_mailbox *)handle;
	struct xcl_mailbox_peer_state *st;
	struct xcl_mailbox_req *req;
	size_t reqlen = sizeof(*req) + sizeof(*st) - 1;

	req = vzalloc(reqlen);
	if (!req)
		return;

	req->req = XCL_MAILBOX_REQ_MGMT_STATE;
	st = (struct xcl_mailbox_peer_state *)req->data;
	st->state_flags = online ? XCL_MB_STATE_ONLINE : XCL_MB_STATE_OFFLINE;
	mutex_lock(&xmbx->lock);
	xmgmt_mailbox_notify(xmbx, false, req, reqlen);
	mutex_unlock(&xmbx->lock);
}
