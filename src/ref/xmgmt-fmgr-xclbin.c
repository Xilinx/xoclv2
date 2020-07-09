// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 * Bulk of the code borrowed from XRT mgmt driver file, icap.c
 *
 * Authors: Sonal.Santan@xilinx.com
 */

#include <linux/key.h>
#include <linux/verification.h>

#include "xmgmt-drv.h"
#include "xmgmt-fmgr.h"

#define	SYS_KEYS	((void *)1UL)

extern struct key *xfpga_keys;

int xfpga_verify_signature(struct fpga_manager *mgr, const void *data, size_t data_len,
			   const void *sig, size_t sig_len)
{
	struct xfpga_klass *xfpga = mgr->priv;
	int ret = verify_pkcs7_signature(data, data_len, sig, sig_len,
					 (xfpga->sec_level == XFPGA_SEC_SYSTEM) ? SYS_KEYS : xfpga_keys,
					 VERIFYING_UNSPECIFIED_SIGNATURE, NULL, NULL);
	if (!ret) {
		xmgmt_info(&mgr->dev, "signature verification is done successfully");
		return 0;
	}

	xmgmt_err(&mgr->dev, "signature verification failed: %d", ret);
	ret = (xfpga->sec_level == XFPGA_SEC_NONE) ? 0 : -EKEYREJECTED;
	return ret;
}


int xfpga_xclbin_download(struct fpga_manager *mgr)
{
	int err = 0;
	struct xfpga_klass *xfpga = mgr->priv;
	struct axlf *xclbin = xfpga->blob;
	if (xclbin->m_signature_length != -1) {
		int siglen = xclbin->m_signature_length;
		u64 origlen = xclbin->m_header.m_length - siglen;

		xmgmt_info(&mgr->dev, "signed xclbin detected");
		xmgmt_info(&mgr->dev, "original size: %llu, signature size: %d",
			   origlen, siglen);

		/* restore original xclbin for verification */
		xclbin->m_signature_length = -1;
		xclbin->m_header.m_length = origlen;

		err = xfpga_verify_signature(mgr, xclbin, origlen,
					     ((char *)xclbin) + origlen, siglen);
		if (err)
			return err;
	} else if (xfpga->sec_level > XFPGA_SEC_NONE) {
		xmgmt_info(&mgr->dev, "xclbin is not signed, rejected");
		return -EKEYREJECTED;
	}
/*
	err = icap_setup_clock_freq_topology(xfpga);
	if (err)
		return err;
	err = axlf_set_freqscaling(xfpga);
	if (err)
		return err;

	err = icap_download_bitstream(xfpga);
	if (err)
		return err;

	err = calibrate_mig(xfpga);
*/
	return err;
}
