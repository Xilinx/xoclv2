// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2019-2020 Xilinx, Inc.
 * Bulk of the code borrowed from XRT mgmt driver file, fmgr.c
 *
 * Authors: Sonal.Santan@xilinx.com
 */

#include <linux/cred.h>
#include <linux/efi.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/platform_device.h>
#include <linux/module.h>

#include "xmgmt-drv.h"
#include "xocl-lib.h"
#include "xmgmt-fmgr.h"

#include "mgmt-ioctl.h"

struct key *xfpga_keys = NULL;

static int xmgmt_pr_write_init(struct fpga_manager *mgr,
			       struct fpga_image_info *info, const char *buf, size_t count)
{
	struct xfpga_klass *obj = mgr->priv;
	const struct axlf *bin = (const struct axlf *)buf;

	if (!obj->fixed) {
		obj->state = FPGA_MGR_STATE_WRITE_INIT_ERR;
		return -EBUSY;
	}

	if (count < sizeof(struct axlf)) {
	 	obj->state = FPGA_MGR_STATE_WRITE_INIT_ERR;
		return -EINVAL;
	}

	if (count > bin->m_header.m_length) {
	 	obj->state = FPGA_MGR_STATE_WRITE_INIT_ERR;
		return -EINVAL;
	}

	/* Free up the previous blob */
	vfree(obj->blob);
	obj->blob = vmalloc(bin->m_header.m_length);
	if (!obj->blob) {
		obj->state = FPGA_MGR_STATE_WRITE_INIT_ERR;
		return -ENOMEM;
	}

	memcpy(obj->blob, buf, count);
	xmgmt_info(&mgr->dev, "Begin download of xclbin %pUb of length %lld B", &obj->blob->m_header.uuid,
		  obj->blob->m_header.m_length);
	obj->count = count;
	obj->state = FPGA_MGR_STATE_WRITE_INIT;
	return 0;
}

static int xmgmt_pr_write(struct fpga_manager *mgr,
			 const char *buf, size_t count)
{
	struct xfpga_klass *obj = mgr->priv;
	char *curr = (char *)obj->blob;

	if ((obj->state != FPGA_MGR_STATE_WRITE_INIT) && (obj->state != FPGA_MGR_STATE_WRITE)) {
		obj->state = FPGA_MGR_STATE_WRITE_ERR;
		return -EINVAL;
	}

	curr += obj->count;
	obj->count += count;
	/* Check if the xclbin buffer is not longer than advertised in the header */
	if (obj->blob->m_header.m_length < obj->count) {
		obj->state = FPGA_MGR_STATE_WRITE_ERR;
		return -EINVAL;
	}
	memcpy(curr, buf, count);
	xmgmt_info(&mgr->dev, "Next block of %zu B of xclbin %pUb", count, &obj->blob->m_header.uuid);
	obj->state = FPGA_MGR_STATE_WRITE;
	return 0;
}


static int xmgmt_pr_write_complete(struct fpga_manager *mgr,
				   struct fpga_image_info *info)
{
	struct xocl_subdev_base *icap;
	int result = 0;
	struct xfpga_klass *obj = mgr->priv;
	if (obj->state != FPGA_MGR_STATE_WRITE) {
		obj->state = FPGA_MGR_STATE_WRITE_COMPLETE_ERR;
		return -EINVAL;
	}

	/* Check if we got the complete xclbin */
	if (obj->blob->m_header.m_length != obj->count) {
		obj->state = FPGA_MGR_STATE_WRITE_COMPLETE_ERR;
		return -EINVAL;
	}

	icap = xocl_lookup_subdev(obj->fixed, XOCL_SUBDEV_ICAP);
	if (!icap)
		return -ENODEV;

	/* Send the xclbin blob to actual download framework in icap */
	result = xocl_subdev_ioctl(icap, XCLMGMT_IOCICAPDOWNLOAD_AXLF, (long)obj->blob);

	obj->state = result ? FPGA_MGR_STATE_WRITE_COMPLETE_ERR : FPGA_MGR_STATE_WRITE_COMPLETE;
	xmgmt_info(&mgr->dev, "Finish download of xclbin %pUb of size %zu B", &obj->blob->m_header.uuid, obj->count);
	vfree(obj->blob);
	obj->blob = NULL;
	obj->count = 0;
	return result;
}

static enum fpga_mgr_states xmgmt_pr_state(struct fpga_manager *mgr)
{
	struct xfpga_klass *obj = mgr->priv;

	return obj->state;
}

static const struct fpga_manager_ops xmgmt_pr_ops = {
	.initial_header_size = sizeof(struct axlf),
	.write_init = xmgmt_pr_write_init,
	.write = xmgmt_pr_write,
	.write_complete = xmgmt_pr_write_complete,
	.state = xmgmt_pr_state,
};

static int fmgr_probe(struct platform_device *pdev)
{
	struct fpga_manager *mgr;
	int ret = 0;

	struct xfpga_klass *obj = vzalloc(sizeof(struct xfpga_klass));
	if (!obj)
		return -ENOMEM;

	obj->fixed = dev_get_platdata(&pdev->dev);
	snprintf(obj->name, sizeof(obj->name), "Xilinx PCIe FPGA Manager");
	obj->state = FPGA_MGR_STATE_UNKNOWN;
	mgr = fpga_mgr_create(&pdev->dev,
			      obj->name,
			      &xmgmt_pr_ops,
			      obj);
	xmgmt_info(&pdev->dev, "fmgr_probe 0x%px 0x%px\n", mgr, &pdev->dev);
	if (!mgr)
		return -ENOMEM;

	/* Historically this was internally called by fpga_mgr_register (in the form
	 * of drv_set_drvdata) but is expected to be called here since Linux 4.18.
	 */
	platform_set_drvdata(pdev, mgr);
	obj->sec_level = XFPGA_SEC_NONE;
#ifdef CONFIG_EFI
	if (is_module_sig_enforced())
		obj->sec_level = XFPGA_SEC_SYSTEM;
	xmgmt_info(&pdev->dev, "Secure boot mode detected");
	// TODO: This does not work with the latest kernels
	/*
	if (efi_get_secureboot() == efi_secureboot_mode_enabled) {
		if (efi_enabled(EFI_SECURE_BOOT)) {

			icap->sec_level = ICAP_SEC_SYSTEM;
		} else {
			icap->sec_level = ICAP_SEC_NONE;
		}
	*/
#else
	xmgmt_info(&pdev->dev, "no support for detection of secure boot mode");
#endif
	ret = fpga_mgr_register(mgr);
	if (ret)
		fpga_mgr_free(mgr);
	mutex_init(&obj->axlf_lock);
	return ret;
}

static int fmgr_remove(struct platform_device *pdev)
{
	struct fpga_manager *mgr = platform_get_drvdata(pdev);
	struct xfpga_klass *obj = mgr->priv;

	xmgmt_info(&pdev->dev, "fmgr_remove 0x%px 0x%px\n", mgr, &pdev->dev);
	mutex_destroy(&obj->axlf_lock);

	obj->state = FPGA_MGR_STATE_UNKNOWN;
	/* TODO: Remove old fpga_mgr_unregister as soon as Linux < 4.18 is no
	 * longer supported.
	 */
	fpga_mgr_unregister(mgr);
	platform_set_drvdata(pdev, NULL);
	vfree(obj->blob);
	vfree(obj);
	return 0;
}

static struct platform_driver fmgr_driver = {
	.probe		= fmgr_probe,
	.remove		= fmgr_remove,
	.driver		= {
		.name = "xocl-fmgr",
	},
};

static int __init xocl_fmgr_init(void)
{
	int err = 0;
	xfpga_keys = keyring_alloc(XOCL_AXLF_SIGNING_KEYS, KUIDT_INIT(0),
				  KGIDT_INIT(0), current_cred(),
				  ((KEY_POS_ALL & ~KEY_POS_SETATTR) |
				   KEY_USR_VIEW | KEY_USR_WRITE | KEY_USR_SEARCH),
				  KEY_ALLOC_NOT_IN_QUOTA, NULL, NULL);

	if (IS_ERR(xfpga_keys)) {
		err = PTR_ERR(xfpga_keys);
		xfpga_keys = NULL;
		pr_err("Failed to allocate keyring \"%s\": %d\n",
		       XOCL_AXLF_SIGNING_KEYS, err);
		return err;
	}
	pr_info("Allocated keyring \"%s\" for xclbin signature validation\n",
		XOCL_AXLF_SIGNING_KEYS);
	err = platform_driver_register(&fmgr_driver);
	if (err)
		key_put(xfpga_keys);

	return err;
}

static void __exit xocl_fmgr_exit(void)
{
	platform_driver_unregister(&fmgr_driver);
	if (!xfpga_keys)
		return;

	key_put(xfpga_keys);
	pr_info("Released keyring \"%s\"\n", XOCL_AXLF_SIGNING_KEYS);
}

module_init(xocl_fmgr_init);
module_exit(xocl_fmgr_exit);

MODULE_DESCRIPTION("FPGA Manager for Xilinx Alveo");
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:xocl-fmgr");
