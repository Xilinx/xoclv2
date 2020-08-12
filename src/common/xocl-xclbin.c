// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Kernel Driver XCLBIN parser
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors: David Zhang <davidzha@xilinx.com>
 */

#include <asm/errno.h>
#include <linux/vmalloc.h>
#include "xocl-xclbin.h"

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
	uint64_t xclbin_len)
{
	return (header->m_sectionOffset + header->m_sectionSize) > xclbin_len ?
		-EINVAL : 0;
}

static int xrt_xclbin_section_info(const struct axlf *xclbin,
	enum axlf_section_kind kind,
	uint64_t *offset, uint64_t *size)
{
	const struct axlf_section_header *memHeader = NULL;
	uint64_t xclbin_len;
	int err = 0;

	memHeader = xrt_xclbin_get_section_hdr(xclbin, kind);
	if (!memHeader)
		return -EINVAL;

	xclbin_len = xclbin->m_header.m_length;
	err = xrt_xclbin_check_section_hdr(memHeader, xclbin_len);
	if (err)
		return err;

	*offset = memHeader->m_sectionOffset;
	*size = memHeader->m_sectionSize;

	return 0;
}

/* caller should free the allocated memory for **data */
int xrt_xclbin_get_section(const char *buf,
	enum axlf_section_kind kind, void **data, uint64_t *len)
{
	const struct axlf *xclbin = (const struct axlf *)buf;
	void *section = NULL;
	int err = 0;
	uint64_t offset = 0;
	uint64_t size = 0;

	err = xrt_xclbin_section_info(xclbin, kind, &offset, &size);
	if (err)
		return err;

	section = vmalloc(size);
	if (section == NULL)
		return -ENOMEM;

	memcpy(section, ((const char *)xclbin) + offset, size);

	*data = section;
	if (len)
		*len = size;

	return 0;
}
