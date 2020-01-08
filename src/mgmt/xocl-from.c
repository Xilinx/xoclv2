// SPDX-License-Identifier: GPL-2.0
/*
 * Platform driver for Xilinx acclerator feature ROM IP.
 * Bulk of the code borrowed from XRT driver file feature_rom.c
 *
 * Copyright (C) 2016-2019 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 *          chien-wei.lan@xilinx.com
 */

#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include "xocl-devices.h"
#include "xocl-features.h"

#define	MAGIC_NUM	0x786e6c78
struct feature_rom {
	void __iomem		*base;
	struct platform_device	*pdev;
	struct xocl_dev_core    *xdev;
/*
	struct FeatureRomHeader	header;
	bool			unified;
	bool			mb_mgmt_enabled;
	bool			mb_sche_enabled;
	bool			are_dev;
	bool			aws_dev;
	bool			runtime_clk_scale_en;
	char			uuid[65];
	bool			passthrough_virt_en;
*/
};

struct xocl_rom_funcs {
	//struct xocl_subdev_funcs common_funcs;
	bool (*is_unified)(const struct platform_device *pdev);
	bool (*mb_mgmt_on)(const struct platform_device *pdev);
	bool (*mb_sched_on)(const struct platform_device *pdev);
	uint32_t* (*cdma_addr)(const struct platform_device *pdev);
	u16 (*get_ddr_channel_count)(const struct platform_device *pdev);
	u64 (*get_ddr_channel_size)(const struct platform_device *pdev);
	bool (*is_are)(const struct platform_device *pdev);
	bool (*is_aws)(const struct platform_device *pdev);
	bool (*verify_timestamp)(struct platform_device *pdev, u64 timestamp);
	u64 (*get_timestamp)(const struct platform_device *pdev);
	int (*get_raw_header)(struct platform_device *pdev, void *header);
	bool (*runtime_clk_scale_on)(const struct platform_device *pdev);
	int (*find_firmware)(const struct platform_device *pdev, char *fw_name,
		size_t len, u16 deviceid, const struct firmware **fw);
	bool (*passthrough_virtualization_on)(const struct platform_device *pdev);
	const char *(*get_uuid)(const struct platform_device *pdev);
};

static ssize_t VBNV_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
	struct feature_rom *rom = platform_get_drvdata(to_platform_device(dev));

	return sprintf(buf, "%s\n", rom->xdev->from.header.VBNVName);
}
static DEVICE_ATTR_RO(VBNV);

static ssize_t dr_base_addr_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
	struct feature_rom *rom = platform_get_drvdata(to_platform_device(dev));

	//TODO: Fix: DRBaseAddress no longer required in feature rom
	if(rom->xdev->from.header.MajorVersion >= 10)
		return sprintf(buf, "%llu\n", rom->xdev->from.header.DRBaseAddress);
	else
		return sprintf(buf, "%u\n", 0);
}
static DEVICE_ATTR_RO(dr_base_addr);

static ssize_t ddr_bank_count_max_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
	struct feature_rom *rom = platform_get_drvdata(to_platform_device(dev));

	return sprintf(buf, "%d\n", rom->xdev->from.header.DDRChannelCount);
}
static DEVICE_ATTR_RO(ddr_bank_count_max);

static ssize_t ddr_bank_size_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
	struct feature_rom *rom = platform_get_drvdata(to_platform_device(dev));

	return sprintf(buf, "%d\n", rom->xdev->from.header.DDRChannelSize);
}
static DEVICE_ATTR_RO(ddr_bank_size);

static ssize_t timestamp_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
	struct feature_rom *rom = platform_get_drvdata(to_platform_device(dev));

	return sprintf(buf, "%llu\n", rom->xdev->from.header.TimeSinceEpoch);
}
static DEVICE_ATTR_RO(timestamp);

static ssize_t uuid_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
	struct feature_rom *rom = platform_get_drvdata(to_platform_device(dev));

	return sprintf(buf, "%s\n", rom->xdev->from.uuid);
}
static DEVICE_ATTR_RO(uuid);

static ssize_t FPGA_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
	const struct feature_rom *rom = platform_get_drvdata(to_platform_device(dev));

	return sprintf(buf, "%s\n", rom->xdev->from.header.FPGAPartName);
}
static DEVICE_ATTR_RO(FPGA);

static struct attribute *rom_attrs[] = {
	&dev_attr_VBNV.attr,
	&dev_attr_dr_base_addr.attr,
	&dev_attr_ddr_bank_count_max.attr,
	&dev_attr_ddr_bank_size.attr,
	&dev_attr_timestamp.attr,
	&dev_attr_FPGA.attr,
	&dev_attr_uuid.attr,
	NULL,
};

static ssize_t raw_show(struct file *filp, struct kobject *kobj,
	struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	const struct feature_rom *rom = platform_get_drvdata(to_platform_device(dev));

	if (off >= sizeof(rom->xdev->from.header))
		return 0;

	if (off + count >= sizeof(rom->xdev->from.header))
		count = sizeof(rom->xdev->from.header) - off;

	memcpy(buf, &rom->xdev->from.header, count);

	return count;
};

static struct bin_attribute raw_attr = {
	.attr = {
		.name = "raw",
		.mode = 0400
	},
	.read = raw_show,
	.write = NULL,
	.size = 0
};

static struct bin_attribute  *rom_bin_attrs[] = {
	&raw_attr,
	NULL,
};

static const struct attribute_group rom_attr_group = {
	.attrs = rom_attrs,
	.bin_attrs = rom_bin_attrs,
};

static bool is_unified(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.unified;
}

static bool mb_mgmt_on(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.mb_mgmt_enabled;
}

static bool mb_sched_on(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);
	// TODO: SS
	// return rom->mb_sche_enabled && !XOCL_DSA_MB_SCHE_OFF(xocl_get_xdev(pdev));
	return true;
}

static bool runtime_clk_scale_on(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.runtime_clk_scale_en;
}

static bool passthrough_virtualization_on(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.passthrough_virt_en;
}

static uint32_t* get_cdma_base_addresses(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	// TODO: SS
	//return (!XOCL_DSA_NO_KDMA(xocl_get_xdev(pdev)) &&
	//	(rom->xdev->from.header.FeatureBitMap & CDMA)) ?
	//	rom->xdev->from.header.CDMABaseAddress : 0;
	return NULL;
}

static u16 get_ddr_channel_count(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.header.DDRChannelCount;
}

static u64 get_ddr_channel_size(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.header.DDRChannelSize;
}

static u64 get_timestamp(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.header.TimeSinceEpoch;
}

static const char *get_uuid(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.uuid;
}

static bool is_are(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.are_dev;
}

static bool is_aws(const struct platform_device *pdev)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	return rom->xdev->from.aws_dev;
}

static bool verify_timestamp(struct platform_device *pdev, u64 timestamp)
{
	const struct feature_rom *rom;

	/* Ignore timestamp matching for AWS platform */
	if (is_aws(pdev))
		return true;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	xocl_info(&pdev->dev, "Shell timestamp: 0x%llx",
		rom->xdev->from.header.TimeSinceEpoch);
	xocl_info(&pdev->dev, "Verify timestamp: 0x%llx", timestamp);

	if (strlen(rom->xdev->from.uuid) > 0) {
		xocl_info(&pdev->dev, "2RP platform, skip timestamp check");
		return true;
	}

	return (rom->xdev->from.header.TimeSinceEpoch == timestamp);
}

static int get_raw_header(struct platform_device *pdev, void *header)
{
	const struct feature_rom *rom;

	rom = platform_get_drvdata(pdev);
	BUG_ON(!rom);

	memcpy(header, &rom->xdev->from.header, sizeof (rom->xdev->from.header));

	return 0;
}

static int __find_firmware(const struct platform_device *pdev, char *fw_name,
	size_t len, u16 deviceid, const struct firmware **fw, const char *suffix)
{
	const struct feature_rom *rom = platform_get_drvdata(pdev);
	struct pci_dev *pcidev = XOCL_PL_TO_PCI_DEV(pdev);
	u16 vendor = le16_to_cpu(pcidev->vendor);
	u16 subdevice = le16_to_cpu(pcidev->subsystem_device);
	u64 timestamp = rom->xdev->from.header.TimeSinceEpoch;
	bool is_multi_rp = (strlen(rom->xdev->from.uuid) > 0) ? true : false;
	int err = 0;

	/* For 2RP, only uuid is provided */
	if (is_multi_rp) {
		snprintf(fw_name, len, "xilinx/%s/partition.%s", rom->xdev->from.uuid,
			suffix);
	} else {
		snprintf(fw_name, len, "xilinx/%04x-%04x-%04x-%016llx.%s",
			vendor, deviceid, subdevice, timestamp, suffix);
	}

	/* deviceid is arg, the others are from pdev) */
	xocl_info(&pdev->dev, "try load %s", fw_name);
	err = request_firmware(fw, fw_name, &pcidev->dev);
	if (err && !is_multi_rp) {
		snprintf(fw_name, len, "xilinx/%04x-%04x-%04x-%016llx.%s",
			vendor, (deviceid + 1), subdevice, timestamp, suffix);
		xocl_info(&pdev->dev, "try load %s", fw_name);
		err = request_firmware(fw, fw_name, &pcidev->dev);
	}

	if (err && is_multi_rp) {
		snprintf(fw_name, len, "xilinx/%s/%s.%s", rom->xdev->from.uuid, rom->xdev->from.uuid,
			suffix);
		err = request_firmware(fw, fw_name, &pcidev->dev);
	}

	/* Retry with the legacy dsabin */
	if (err && !is_multi_rp) {
		snprintf(fw_name, len, "xilinx/%04x-%04x-%04x-%016llx.%s",
			vendor, le16_to_cpu(pcidev->device + 1), subdevice,
			le64_to_cpu(0x0000000000000000), suffix);
		xocl_info(&pdev->dev, "try load %s", fw_name);
		err = request_firmware(fw, fw_name, &pcidev->dev);
	}

	return err;
}

static int find_firmware(const struct platform_device *pdev, char *fw_name,
	size_t len, u16 deviceid, const struct firmware **fw)
{
	// try xsabin first, then dsabin
	if (__find_firmware(pdev, fw_name, len, deviceid, fw, "xsabin")) {
		return __find_firmware(pdev, fw_name, len, deviceid, fw,
			"dsabin");
	}

	return 0;
}

static struct xocl_rom_funcs rom_ops = {
	.is_unified = is_unified,
	.mb_mgmt_on = mb_mgmt_on,
	.mb_sched_on = mb_sched_on,
	.cdma_addr = get_cdma_base_addresses,
	.get_ddr_channel_count = get_ddr_channel_count,
	.get_ddr_channel_size = get_ddr_channel_size,
	.is_are = is_are,
	.is_aws = is_aws,
	.verify_timestamp = verify_timestamp,
	.get_timestamp = get_timestamp,
	.get_raw_header = get_raw_header,
	.runtime_clk_scale_on = runtime_clk_scale_on,
	.find_firmware = find_firmware,
	.passthrough_virtualization_on = passthrough_virtualization_on,
	.get_uuid = get_uuid,
};

static int get_header_from_peer(struct feature_rom *rom)
{
	//TODO: SS
/*

	struct FeatureRomHeader *header;
	struct resource *res;
	xdev_handle_t xdev = xocl_get_xdev(rom->pdev);

	header = XOCL_GET_SUBDEV_PRIV(&rom->pdev->dev);
	if (!header)
		return -ENODEV;

	memcpy(&rom->xdev->from.header, header, sizeof(*header));

	xocl_xdev_info(xdev, "Searching CMC in dtb.");
	res = xocl_subdev_get_ioresource(xdev, RESNAME_KDMA);
	if (res) {
                rom->xdev->from.header.FeatureBitMap |= CDMA;
		memset(rom->xdev->from.header.CDMABaseAddress, 0,
			sizeof(rom->xdev->from.header.CDMABaseAddress));
		rom->xdev->from.header.CDMABaseAddress[0] = (uint32_t)res->start;

		xocl_xdev_info(xdev, "CDMA is on, CU offset: 0x%x",
				rom->xdev->from.header.CDMABaseAddress[0]);
	}
*/
	return 0;
}

static void platform_type_append(char *prefix, u32 platform_type)
{
	char *type;

	if (!prefix)
		return;

	switch (platform_type) {
	case XOCL_VSEC_PLAT_RECOVERY:
		type = "_Recovery";
		break;
	case XOCL_VSEC_PLAT_1RP:
		type = "_xdma_gen3x4_201920_3";
		break;
	case XOCL_VSEC_PLAT_2RP:
		type = "_xdma_gen3x4_201920_3";
		break;
	default:
		type = "_Unknown";
	}

	strncat(prefix, type, XOCL_MAXNAMELEN - strlen(prefix) - 1);
}

static int init_rom_by_dtb(struct feature_rom *rom)
{
	// TODO: SS
/*
	xdev_handle_t xdev = xocl_get_xdev(rom->pdev);
	struct FeatureRomHeader *header = &rom->xdev->from.header;
	struct resource *res;
	const char *vbnv;
	int i;
	int bar;
	u64 offset;
	u32 platform_type;

	if (XDEV(xdev)->fdt_blob) {
		vbnv = fdt_getprop(XDEV(xdev)->fdt_blob, 0, "vbnv", NULL);
		if (vbnv)
			strcpy(header->VBNVName, vbnv);
		for (i = 0; i < strlen(header->VBNVName); i++)
			if (header->VBNVName[i] == ':' ||
					header->VBNVName[i] == '.')
				header->VBNVName[i] = '_';
	}
	header->FeatureBitMap = UNIFIED_PLATFORM;
	*(u32 *)header->EntryPointString = MAGIC_NUM;
	if (XDEV(xdev)->priv.vbnv)
		strncpy(header->VBNVName, XDEV(xdev)->priv.vbnv,
				sizeof (header->VBNVName) - 1);

	if (!xocl_subdev_vsec(xdev, XOCL_VSEC_PLATFORM_INFO, &bar, &offset)) {
		platform_type = xocl_subdev_vsec_read32(xdev, bar, offset);
		platform_type_append(header->VBNVName, platform_type);
	}

	xocl_xdev_info(xdev, "Searching ERT and CMC in dtb.");
	res = xocl_subdev_get_ioresource(xdev, NODE_CMC_FW_MEM);
	if (res) {
		xocl_xdev_info(xdev, "CMC is on");
		header->FeatureBitMap |= BOARD_MGMT_ENBLD;
	}

	res = xocl_subdev_get_ioresource(xdev, NODE_ERT_FW_MEM);
	if (res) {
		xocl_xdev_info(xdev, "ERT is on");
		header->FeatureBitMap |= MB_SCHEDULER;
	}
*/
	return 0;
}

static int get_header_from_dtb(struct feature_rom *rom)
{
	int i, j = 0;

	/* uuid string should be 64 + '\0' */
	BUG_ON(sizeof(rom->xdev->from.uuid) <= 64);

	for (i = 28; i >= 0 && j < 64; i -= 4, j += 8) {
		sprintf(&rom->xdev->from.uuid[j], "%08x", ioread32(rom->base + i));
	}
	xocl_info(&rom->pdev->dev, "UUID %s", rom->xdev->from.uuid);

	return init_rom_by_dtb(rom);
}

static int get_header_from_vsec(struct feature_rom *rom)
{
/*
	xdev_handle_t xdev = xocl_get_xdev(rom->pdev);
	int bar;
	u64 offset;
	int ret;

	ret = xocl_subdev_vsec(xdev, XOCL_VSEC_UUID_ROM, &bar, &offset);
	if (ret)
		return -ENODEV;

	offset += pci_resource_start(XDEV(xdev)->pdev, bar);
	xocl_xdev_info(xdev, "Mapping uuid at offset 0x%llx", offset);
	rom->base = ioremap_nocache(offset, PAGE_SIZE);
*/
	return get_header_from_dtb(rom);
}

static int get_header_from_iomem(struct feature_rom *rom)
{
	struct platform_device *pdev = rom->pdev;
	u32	val;
	u16	vendor, did;
	int	ret = 0;

	val = ioread32(rom->base);
	if (val != MAGIC_NUM) {
		vendor = XOCL_PL_TO_PCI_DEV(pdev)->vendor;
		did = XOCL_PL_TO_PCI_DEV(pdev)->device;
		if (vendor == 0x1d0f && (did == 0x1042 || did == 0xf010)) { // MAGIC, we should define elsewhere
			xocl_info(&pdev->dev,
				"Found AWS VU9P Device without featureROM");
			/*
 			 * This is AWS device. Fill the FeatureROM struct.
 			 * Right now it doesn't have FeatureROM
 			 */
			memset(rom->xdev->from.header.EntryPointString, 0,
				sizeof(rom->xdev->from.header.EntryPointString));
			strncpy(rom->xdev->from.header.EntryPointString, "xlnx", 4);
			memset(rom->xdev->from.header.FPGAPartName, 0,
				sizeof(rom->xdev->from.header.FPGAPartName));
			strncpy(rom->xdev->from.header.FPGAPartName, "AWS VU9P", 8);
			memset(rom->xdev->from.header.VBNVName, 0,
				sizeof(rom->xdev->from.header.VBNVName));
			strncpy(rom->xdev->from.header.VBNVName,
				"xilinx_aws-vu9p-f1_dynamic_5_0", 35);
			rom->xdev->from.header.MajorVersion = 4;
			rom->xdev->from.header.MinorVersion = 0;
			rom->xdev->from.header.VivadoBuildID = 0xabcd;
			rom->xdev->from.header.IPBuildID = 0xabcd;
			rom->xdev->from.header.TimeSinceEpoch = 0xabcd;
			rom->xdev->from.header.DDRChannelCount = 4;
			rom->xdev->from.header.DDRChannelSize = 16;
			rom->xdev->from.header.FeatureBitMap = 0x0;
			rom->xdev->from.header.FeatureBitMap = UNIFIED_PLATFORM;
			rom->xdev->from.unified = true;
			rom->xdev->from.aws_dev = true;

			xocl_info(&pdev->dev, "Enabling AWS dynamic 5.0 Shell");
		} else {
			xocl_err(&pdev->dev, "Magic number does not match, "
			"actual 0x%x, expected 0x%x", val, MAGIC_NUM);
			ret = -ENODEV;
			goto failed;
		}
	} else
		xocl_memcpy_fromio(&rom->xdev->from.header, rom->base,
				sizeof(rom->xdev->from.header));

failed:
	return ret;
}

static long myrom_ioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xocl_info(&pdev->dev, "%s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

const static struct xocl_subdev_ops myrom_ops = {
	.ioctl = myrom_ioctl,
	.id = XOCL_SUBDEV_FEATURE_ROM,
};

static int feature_rom_probe_helper(struct platform_device *pdev, const struct resource *res,
				    struct feature_rom *rom)
{
	int ret;
	char *tmp;

	if (res == NULL) {
		ret = get_header_from_vsec(rom);
		if (ret)
			(void)get_header_from_peer(rom);
	} else {
		rom->base = ioremap_nocache(res->start, res->end - res->start + 1);
		if (!rom->base) {
			ret = -EIO;
			xocl_err(&pdev->dev, "Map iomem failed");
			goto failed;
		}

		if (!strcmp(res->name, "uuid"))
			(void)get_header_from_dtb(rom);
		else
			(void)get_header_from_iomem(rom);
	}

	if (strstr(rom->xdev->from.header.VBNVName, "-xare")) {
		/*
		 * ARE device, ARE is mapped like another DDR inside FPGA;
		 * map_connects as M04_AXI
		 */
		rom->xdev->from.header.DDRChannelCount = rom->xdev->from.header.DDRChannelCount - 1;
		rom->xdev->from.are_dev = true;
	}

	if(rom->xdev->from.header.FeatureBitMap & UNIFIED_PLATFORM)
		rom->xdev->from.unified = true;

	if(rom->xdev->from.header.FeatureBitMap & BOARD_MGMT_ENBLD)
		rom->xdev->from.mb_mgmt_enabled = true;

	if(rom->xdev->from.header.FeatureBitMap & MB_SCHEDULER)
		rom->xdev->from.mb_sche_enabled = true;

	if(rom->xdev->from.header.FeatureBitMap & RUNTIME_CLK_SCALE)
		rom->xdev->from.runtime_clk_scale_en = true;

	if(rom->xdev->from.header.FeatureBitMap & PASSTHROUGH_VIRTUALIZATION)
		rom->xdev->from.passthrough_virt_en = true;

	ret = sysfs_create_group(&pdev->dev.kobj, &rom_attr_group);
	if (ret) {
		xocl_err(&pdev->dev, "create sysfs failed");
		goto failed;
	}

	tmp = rom->xdev->from.header.EntryPointString;
	xocl_info(&pdev->dev, "ROM magic : %c%c%c%c",
		tmp[0], tmp[1], tmp[2], tmp[3]);
	xocl_info(&pdev->dev, "VBNV: %s", rom->xdev->from.header.VBNVName);
	xocl_info(&pdev->dev, "DDR channel count : %d",
		rom->xdev->from.header.DDRChannelCount);
	xocl_info(&pdev->dev, "DDR channel size: %d GB",
		rom->xdev->from.header.DDRChannelSize);
	xocl_info(&pdev->dev, "Major Version: %d", rom->xdev->from.header.MajorVersion);
	xocl_info(&pdev->dev, "Minor Version: %d", rom->xdev->from.header.MinorVersion);
	xocl_info(&pdev->dev, "IPBuildID: %u", rom->xdev->from.header.IPBuildID);
	xocl_info(&pdev->dev, "TimeSinceEpoch: %llx",
		rom->xdev->from.header.TimeSinceEpoch);
	xocl_info(&pdev->dev, "FeatureBitMap: %llx", rom->xdev->from.header.FeatureBitMap);

	return 0;

failed:
	if (rom->base)
		iounmap(rom->base);
	return ret;
}

static int xocl_rom_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	const struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct feature_rom *rom = devm_kzalloc(&pdev->dev, sizeof(*rom), GFP_KERNEL);
	if (!rom)
		return -ENOMEM;
	rom->pdev =  pdev;
	rom->xdev = xocl_get_xdev(pdev);
	ret = feature_rom_probe_helper(pdev, res, rom);
	if (ret)
		goto out;
	platform_set_drvdata(pdev, rom);
	xocl_info(dev, "Probed subdev %s: resource %pr mapped @%px\n", pdev->name, res, rom->base);
	return 0;

out:
	devm_kfree(&pdev->dev, rom);
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int xocl_rom_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct feature_rom *rom = platform_get_drvdata(pdev);

	if (rom->base)
		iounmap(rom->base);
	sysfs_remove_group(&pdev->dev.kobj, &rom_attr_group);
	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, rom);
	xocl_info(dev, "Removed subdev %s\n", pdev->name);
	return 0;
}

static const struct platform_device_id rom_id_table[] = {
	{ "xocl-rom", (kernel_ulong_t)&myrom_ops },
	{ },
};

struct platform_driver xocl_rom_driver = {
	.driver	= {
		.name    = "xocl-rom",
	},
	.probe    = xocl_rom_probe,
	.remove   = xocl_rom_remove,
	.id_table = rom_id_table,
};
