// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Driver XCLBIN parser
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors: David Zhang <davidzha@xilinx.com>
 */

#include <asm/errno.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include "xclbin-helper.h"
#include "metadata.h"

/* Used for parsing bitstream header */
#define BITSTREAM_EVEN_MAGIC_BYTE	0x0f
#define BITSTREAM_ODD_MAGIC_BYTE	0xf0

#define XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size)		\
	({							\
		typeof(offset) _offset = offset++;		\
		if (_offset >= size)				\
			return -EINVAL;				\
		tmp = data[_offset];				\
	})

static const struct axlf_section_header *
xrt_xclbin_get_section_hdr(const struct axlf *xclbin,
			   enum axlf_section_kind kind)
{
	const struct axlf_section_header *header = 0;
	u64 xclbin_len;
	int i = 0;

	for (i = 0; i < xclbin->m_header.m_numSections; i++) {
		if (xclbin->m_sections[i].m_sectionKind == kind)
			header = &xclbin->m_sections[i];
	}

	if (!header)
		return NULL;

	xclbin_len = xclbin->m_header.m_length;
	if (xclbin_len > XCLBIN_MAX_SIZE)
		return NULL;

	if (header->section_offset + header->section_size > xclbin_len)
		return NULL;

	return header;
}

static int xrt_xclbin_section_info(const struct axlf *xclbin,
				   enum axlf_section_kind kind,
				   u64 *offset, u64 *size)
{
	const struct axlf_section_header *mem_header = NULL;

	mem_header = xrt_xclbin_get_section_hdr(xclbin, kind);
	if (!mem_header)
		return -EINVAL;

	*offset = mem_header->section_offset;
	*size = mem_header->section_size;

	return 0;
}

/* caller must free the allocated memory for **data */
int xrt_xclbin_get_section(struct device *dev,
			   const struct axlf *buf,
			   enum axlf_section_kind kind,
			   void **data, u64 *len)
{
	const struct axlf *xclbin = (const struct axlf *)buf;
	void *section = NULL;
	u64 offset = 0;
	u64 size = 0;
	int err = 0;

	if (!data || !len) {
		dev_err(dev, "invalid data or len pointer");
		return -EINVAL;
	}

	err = xrt_xclbin_section_info(xclbin, kind, &offset, &size);
	if (err) {
		dev_err(dev, "parsing section failed. err = %d", err);
		return err;
	}

	section = vmalloc(size);
	if (!section)
		return -ENOMEM;

	memcpy(section, ((const char *)xclbin) + offset, size);

	*data = section;
	*len = size;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_xclbin_get_section);

static inline int xclbin_bit_get_string(const unchar *data, u32 size, u32 offset, unchar prefix, const unchar **str)
{
	int len;
	u32 tmp;

	/* Read prefix */
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	if (tmp != prefix)
		return -EINVAL;

	/* Get string length */
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	len = tmp;
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	len = (len << 8) | tmp;

	if (offset + len > size)
		return -EINVAL;

	if (data[offset + len - 1] != '\0')
		return -EINVAL;

	*str = data + offset;

	return len;
}

/* parse bitstream header */
int xrt_xclbin_parse_bitstream_header(struct device *dev, const unchar *data,
				      u32 size, struct xclbin_bit_head_info *head_info)
{
	u32 offset = 0;
	int len, i;
	u16 magic;
	unchar tmp;

	memset(head_info, 0, sizeof(*head_info));

	/* Get "Magic" length, XCLBIN_BIT_NEXT_BYTE increase offset by 1 */
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	head_info->magic_length = tmp;
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	head_info->magic_length = (head_info->magic_length << 8) | tmp;

	for (i = 0; i < head_info->magic_length - 1; i++) {
		XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
		if (!(i % 2) && tmp != BITSTREAM_EVEN_MAGIC_BYTE) {
			dev_err(dev, "invalid magic even byte at %d", offset);
			return -EINVAL;
		}

		XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
		if ((i % 2) && tmp != BITSTREAM_ODD_MAGIC_BYTE) {
			dev_err(dev, "invalid magic odd byte at %d", offset);
			return -EINVAL;
		}
	}

	/* Read null end of magic data. */
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	if (tmp) {
		dev_err(dev, "invalid magic end");
		return -EINVAL;
	}

	/* Read 0x01 (short) */
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	magic = tmp;
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	magic = (magic << 8) | tmp;

	/* Check the "0x01" half word */
	if (magic != 0x01) {
		dev_err(dev, "invalid magic");
		return -EINVAL;
	}

	len = xclbin_bit_get_string(data, size, offset, 'a', &head_info->design_name);
	if (len < 0) {
		dev_err(dev, "get design name failed");
		return -EINVAL;
	}

	head_info->version = strstr(head_info->design_name, "Version=") + strlen("Version=");
	offset += len;

	len = xclbin_bit_get_string(data, size, offset, 'b', &head_info->part_name);
	if (len < 0) {
		dev_err(dev, "get part name failed");
		return -EINVAL;
	}
	offset += len;

	len = xclbin_bit_get_string(data, size, offset, 'c', &head_info->date);
	if (len < 0) {
		dev_err(dev, "get part name failed");
		return -EINVAL;
	}
	offset += len;

	len = xclbin_bit_get_string(data, size, offset, 'd', &head_info->time);
	if (len < 0) {
		dev_err(dev, "get part name failed");
		return -EINVAL;
	}
	offset += len;

	/* Read 'e' */
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	if (tmp != 'e') {
		dev_err(dev, "invalid prefix of bitstream length");
		return -EINVAL;
	}

	/* Get byte length of bitstream */
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	head_info->bitstream_length = tmp;
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	head_info->bitstream_length = (head_info->bitstream_length << 8) | tmp;
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	head_info->bitstream_length = (head_info->bitstream_length << 8) | tmp;
	XCLBIN_BIT_NEXT_BYTE(tmp, offset, data, size);
	head_info->bitstream_length = (head_info->bitstream_length << 8) | tmp;

	head_info->header_length = offset;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_xclbin_parse_bitstream_header);

struct xrt_clock_desc {
	char	*clock_ep_name;
	u32	clock_xclbin_type;
	char	*clkfreq_ep_name;
} clock_desc[] = {
	{
		.clock_ep_name = XRT_MD_NODE_CLK_KERNEL1,
		.clock_xclbin_type = CT_DATA,
		.clkfreq_ep_name = XRT_MD_NODE_CLKFREQ_K1,
	},
	{
		.clock_ep_name = XRT_MD_NODE_CLK_KERNEL2,
		.clock_xclbin_type = CT_KERNEL,
		.clkfreq_ep_name = XRT_MD_NODE_CLKFREQ_K2,
	},
	{
		.clock_ep_name = XRT_MD_NODE_CLK_KERNEL3,
		.clock_xclbin_type = CT_SYSTEM,
		.clkfreq_ep_name = XRT_MD_NODE_CLKFREQ_HBM,
	},
};

const char *xrt_clock_type2epname(enum CLOCK_TYPE type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clock_desc); i++) {
		if (clock_desc[i].clock_xclbin_type == type)
			return clock_desc[i].clock_ep_name;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(xrt_clock_type2epname);

static const char *clock_type2clkfreq_name(u32 type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clock_desc); i++) {
		if (clock_desc[i].clock_xclbin_type == type)
			return clock_desc[i].clkfreq_ep_name;
	}
	return NULL;
}

static int xrt_xclbin_add_clock_metadata(struct device *dev,
					 const struct axlf *xclbin,
					 char *dtb)
{
	int i;
	u16 freq;
	struct clock_freq_topology *clock_topo;
	int rc = xrt_xclbin_get_section(dev, xclbin, CLOCK_FREQ_TOPOLOGY,
					(void **)&clock_topo, NULL);

	if (rc)
		return 0;

	for (i = 0; i < clock_topo->m_count; i++) {
		u8 type = clock_topo->m_clock_freq[i].m_type;
		const char *ep_name = xrt_clock_type2epname(type);
		const char *counter_name = clock_type2clkfreq_name(type);

		if (!ep_name || !counter_name)
			continue;

		freq = cpu_to_be16(clock_topo->m_clock_freq[i].m_freq_MHZ);
		rc = xrt_md_set_prop(dev, dtb, ep_name, NULL, XRT_MD_PROP_CLK_FREQ,
				     &freq, sizeof(freq));
		if (rc)
			break;

		rc = xrt_md_set_prop(dev, dtb, ep_name, NULL, XRT_MD_PROP_CLK_CNT,
				     counter_name, strlen(counter_name) + 1);
		if (rc)
			break;
	}

	vfree(clock_topo);

	return rc;
}

int xrt_xclbin_get_metadata(struct device *dev, const struct axlf *xclbin, char **dtb)
{
	char *md = NULL, *newmd = NULL;
	u64 len, md_len;
	int rc = xrt_xclbin_get_section(dev, xclbin, PARTITION_METADATA,
					(void **)&md, &len);

	if (rc)
		goto done;

	md_len = xrt_md_size(dev, md);

	/* Sanity check the dtb section. */
	if (md_len > len) {
		rc = -EINVAL;
		goto done;
	}

	newmd = vzalloc(md_len);
	if (!newmd) {
		rc = -ENOMEM;
		goto done;
	}
	memcpy(newmd, md, md_len);

	/* Convert various needed xclbin sections into dtb. */
	rc = xrt_xclbin_add_clock_metadata(dev, xclbin, newmd);

done:
	if (rc == 0)
		*dtb = newmd;
	else
		vfree(newmd);
	vfree(md);
	return rc;
}
EXPORT_SYMBOL_GPL(xrt_xclbin_get_metadata);
