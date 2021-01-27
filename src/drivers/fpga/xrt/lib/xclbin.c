// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Kernel Driver XCLBIN parser
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors: David Zhang <davidzha@xilinx.com>
 */

#include <asm/errno.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include "xclbin-helper.h"
#include "metadata.h"

/* Used for parsing bitstream header */
#define XHI_EVEN_MAGIC_BYTE     0x0f
#define XHI_ODD_MAGIC_BYTE      0xf0

/* Extra mode for IDLE */
#define XHI_OP_IDLE  -1
#define XHI_BIT_HEADER_FAILURE -1

/* The imaginary module length register */
#define XHI_MLR                  15

const char *xrt_xclbin_kind_to_string(enum axlf_section_kind kind)
{
	switch (kind) {
	case BITSTREAM:			return "BITSTREAM";
	case CLEARING_BITSTREAM:	return "CLEARING_BITSTREAM";
	case EMBEDDED_METADATA:		return "EMBEDDED_METADATA";
	case FIRMWARE:			return "FIRMWARE";
	case DEBUG_DATA:		return "DEBUG_DATA";
	case SCHED_FIRMWARE:		return "SCHED_FIRMWARE";
	case MEM_TOPOLOGY:		return "MEM_TOPOLOGY";
	case CONNECTIVITY:		return "CONNECTIVITY";
	case IP_LAYOUT:			return "IP_LAYOUT";
	case DEBUG_IP_LAYOUT:		return "DEBUG_IP_LAYOUT";
	case DESIGN_CHECK_POINT:	return "DESIGN_CHECK_POINT";
	case CLOCK_FREQ_TOPOLOGY:	return "CLOCK_FREQ_TOPOLOGY";
	case MCS:			return "MCS";
	case BMC:			return "BMC";
	case BUILD_METADATA:		return "BUILD_METADATA";
	case KEYVALUE_METADATA:		return "KEYVALUE_METADATA";
	case USER_METADATA:		return "USER_METADATA";
	case DNA_CERTIFICATE:		return "DNA_CERTIFICATE";
	case PDI:			return "PDI";
	case BITSTREAM_PARTIAL_PDI:	return "BITSTREAM_PARTIAL_PDI";
	case PARTITION_METADATA:	return "PARTITION_METADATA";
	case EMULATION_DATA:		return "EMULATION_DATA";
	case SYSTEM_METADATA:		return "SYSTEM_METADATA";
	case SOFT_KERNEL:		return "SOFT_KERNEL";
	case ASK_FLASH:			return "ASK_FLASH";
	case AIE_METADATA:		return "AIE_METADATA";
	case ASK_GROUP_TOPOLOGY:	return "ASK_GROUP_TOPOLOGY";
	case ASK_GROUP_CONNECTIVITY:	return "ASK_GROUP_CONNECTIVITY";
	default:			return "UNKNOWN";
	}
}

static const struct axlf_section_header *
xrt_xclbin_get_section_hdr(const struct axlf *xclbin,
			   enum axlf_section_kind kind)
{
	int i = 0;

	for (i = 0; i < xclbin->m_header.m_numSections; i++) {
		if (xclbin->m_sections[i].m_sectionKind == kind)
			return &xclbin->m_sections[i];
	}

	return NULL;
}

static int
xrt_xclbin_check_section_hdr(const struct axlf_section_header *header,
			     u64 xclbin_len)
{
	return (header->m_sectionOffset + header->m_sectionSize) > xclbin_len ?
		-EINVAL : 0;
}

static int xrt_xclbin_section_info(const struct axlf *xclbin,
				   enum axlf_section_kind kind,
				   u64 *offset, u64 *size)
{
	const struct axlf_section_header *mem_header = NULL;
	u64 xclbin_len;
	int err = 0;

	mem_header = xrt_xclbin_get_section_hdr(xclbin, kind);
	if (!mem_header)
		return -EINVAL;

	xclbin_len = xclbin->m_header.m_length;
	err = xrt_xclbin_check_section_hdr(mem_header, xclbin_len);
	if (err)
		return err;

	*offset = mem_header->m_sectionOffset;
	*size = mem_header->m_sectionSize;

	return 0;
}

/* caller should free the allocated memory for **data */
int xrt_xclbin_get_section(const struct axlf *buf,
			   enum axlf_section_kind kind,
			   void **data, u64 *len)
{
	const struct axlf *xclbin = (const struct axlf *)buf;
	void *section = NULL;
	int err = 0;
	u64 offset = 0;
	u64 size = 0;

	err = xrt_xclbin_section_info(xclbin, kind, &offset, &size);
	if (err)
		return err;

	section = vmalloc(size);
	if (!section)
		return -ENOMEM;

	memcpy(section, ((const char *)xclbin) + offset, size);

	*data = section;
	if (len)
		*len = size;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_xclbin_get_section);

/* parse bitstream header */
int xrt_xclbin_parse_bitstream_header(const unsigned char *data,
				      unsigned int size,
				      struct hw_icap_bit_header *header)
{
	unsigned int i;
	unsigned int len;
	unsigned int tmp;
	unsigned int index;

	/* Start Index at start of bitstream */
	index = 0;

	/* Initialize HeaderLength.  If header returned early inidicates
	 * failure.
	 */
	header->header_length = XHI_BIT_HEADER_FAILURE;

	/* Get "Magic" length */
	header->magic_length = data[index++];
	header->magic_length = (header->magic_length << 8) | data[index++];

	/* Read in "magic" */
	for (i = 0; i < header->magic_length - 1; i++) {
		tmp = data[index++];
		if (i % 2 == 0 && tmp != XHI_EVEN_MAGIC_BYTE)
			return -1;	/* INVALID_FILE_HEADER_ERROR */

		if (i % 2 == 1 && tmp != XHI_ODD_MAGIC_BYTE)
			return -1;	/* INVALID_FILE_HEADER_ERROR */
	}

	/* Read null end of magic data. */
	tmp = data[index++];

	/* Read 0x01 (short) */
	tmp = data[index++];
	tmp = (tmp << 8) | data[index++];

	/* Check the "0x01" half word */
	if (tmp != 0x01)
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Read 'a' */
	tmp = data[index++];
	if (tmp != 'a')
		return -1;	/* INVALID_FILE_HEADER_ERROR	*/

	/* Get Design Name length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for design name and final null character. */
	header->design_name = vmalloc(len);

	/* Read in Design Name */
	for (i = 0; i < len; i++)
		header->design_name[i] = data[index++];

	if (header->design_name[len - 1] != '\0')
		return -1;

	/* Read 'b' */
	tmp = data[index++];
	if (tmp != 'b')
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Get Part Name length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for part name and final null character. */
	header->part_name = vmalloc(len);

	/* Read in part name */
	for (i = 0; i < len; i++)
		header->part_name[i] = data[index++];

	if (header->part_name[len - 1] != '\0')
		return -1;

	/* Read 'c' */
	tmp = data[index++];
	if (tmp != 'c')
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Get date length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for date and final null character. */
	header->date = vmalloc(len);

	/* Read in date name */
	for (i = 0; i < len; i++)
		header->date[i] = data[index++];

	if (header->date[len - 1] != '\0')
		return -1;

	/* Read 'd' */
	tmp = data[index++];
	if (tmp != 'd')
		return -1;	/* INVALID_FILE_HEADER_ERROR  */

	/* Get time length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for time and final null character. */
	header->time = vmalloc(len);

	/* Read in time name */
	for (i = 0; i < len; i++)
		header->time[i] = data[index++];

	if (header->time[len - 1] != '\0')
		return -1;

	/* Read 'e' */
	tmp = data[index++];
	if (tmp != 'e')
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Get byte length of bitstream */
	header->bitstream_length = data[index++];
	header->bitstream_length = (header->bitstream_length << 8) | data[index++];
	header->bitstream_length = (header->bitstream_length << 8) | data[index++];
	header->bitstream_length = (header->bitstream_length << 8) | data[index++];
	header->header_length = index;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_xclbin_parse_bitstream_header);

void xrt_xclbin_free_header(struct hw_icap_bit_header *header)
{
	vfree(header->design_name);
	vfree(header->part_name);
	vfree(header->date);
	vfree(header->time);
}

struct xrt_clock_desc {
	char	*clock_ep_name;
	u32	clock_xclbin_type;
	char	*clkfreq_ep_name;
} clock_desc[] = {
	{
		.clock_ep_name = NODE_CLK_KERNEL1,
		.clock_xclbin_type = CT_DATA,
		.clkfreq_ep_name = NODE_CLKFREQ_K1,
	},
	{
		.clock_ep_name = NODE_CLK_KERNEL2,
		.clock_xclbin_type = CT_KERNEL,
		.clkfreq_ep_name = NODE_CLKFREQ_K2,
	},
	{
		.clock_ep_name = NODE_CLK_KERNEL3,
		.clock_xclbin_type = CT_SYSTEM,
		.clkfreq_ep_name = NODE_CLKFREQ_HBM,
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
	int rc = xrt_xclbin_get_section(xclbin, CLOCK_FREQ_TOPOLOGY,
					(void **)&clock_topo, NULL);

	if (rc)
		return 0;

	for (i = 0; i < clock_topo->m_count; i++) {
		u8 type = clock_topo->m_clock_freq[i].m_type;
		const char *ep_name = xrt_clock_type2epname(type);
		const char *counter_name = clock_type2clkfreq_name(type);

		if (!ep_name || !counter_name)
			continue;

		freq = cpu_to_be16(clock_topo->m_clock_freq[i].m_freq_Mhz);
		rc = xrt_md_set_prop(dev, dtb, ep_name, NULL, PROP_CLK_FREQ,
				     &freq, sizeof(freq));
		if (rc)
			break;

		rc = xrt_md_set_prop(dev, dtb, ep_name, NULL, PROP_CLK_CNT,
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
	u64 len;
	int rc = xrt_xclbin_get_section(xclbin, PARTITION_METADATA,
					(void **)&md, &len);

	if (rc)
		goto done;

	/* Sanity check the dtb section. */
	if (xrt_md_size(dev, md) > len) {
		rc = -EINVAL;
		goto done;
	}

	newmd = xrt_md_dup(dev, md);
	if (!newmd) {
		rc = -EFAULT;
		goto done;
	}
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
