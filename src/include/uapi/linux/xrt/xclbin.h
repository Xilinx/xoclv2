/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 WITH Linux-syscall-note */
/*
 *  Xilinx FPGA compiled binary container format
 *
 *  Copyright (C) 2015-2020, Xilinx Inc
 */

#ifndef _XCLBIN_H_
#define _XCLBIN_H_

#ifdef _WIN32
  #include <cstdint>
  #include <algorithm>
  #include "windows/uuid.h"
#else
  #if defined(__KERNEL__)
    #include <linux/types.h>
    #include <linux/uuid.h>
    #include <linux/version.h>
  #elif defined(__cplusplus)
    #include <cstdlib>
    #include <cstdint>
    #include <algorithm>
    #include <uuid/uuid.h>
  #else
    #include <stdlib.h>
    #include <stdint.h>
    #include <uuid/uuid.h>
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DOC: Container format for Xilinx FPGA images
 * The container stores bitstreams, metadata and firmware images.
 * xclbin/xsabin is ELF-like binary container format. It is structured
 * series of sections. There is a file header followed by several section
 * headers which is followed by sections. A section header points to an
 * actual section. There is an optional signature at the end. The
 * following figure illustrates a typical xclbin:
 *
 *     +---------------------+
 *     |		     |
 *     |       HEADER	     |
 *     +---------------------+
 *     |   SECTION  HEADER   |
 *     |		     |
 *     +---------------------+
 *     |	 ...	     |
 *     |		     |
 *     +---------------------+
 *     |   SECTION  HEADER   |
 *     |		     |
 *     +---------------------+
 *     |       SECTION	     |
 *     |		     |
 *     +---------------------+
 *     |	 ...	     |
 *     |		     |
 *     +---------------------+
 *     |       SECTION	     |
 *     |		     |
 *     +---------------------+
 *     |      SIGNATURE	     |
 *     |      (OPTIONAL)     |
 *     +---------------------+
 */

enum XCLBIN_MODE {
	XCLBIN_FLAT,
	XCLBIN_PR,
	XCLBIN_TANDEM_STAGE2,
	XCLBIN_TANDEM_STAGE2_WITH_PR,
	XCLBIN_HW_EMU,
	XCLBIN_SW_EMU,
	XCLBIN_MODE_MAX
};

enum axlf_section_kind {
	BITSTREAM = 0,
	CLEARING_BITSTREAM,
	EMBEDDED_METADATA,
	FIRMWARE,
	DEBUG_DATA,
	SCHED_FIRMWARE,
	MEM_TOPOLOGY,
	CONNECTIVITY,
	IP_LAYOUT,
	DEBUG_IP_LAYOUT,
	DESIGN_CHECK_POINT,
	CLOCK_FREQ_TOPOLOGY,
	MCS,
	BMC,
	BUILD_METADATA,
	KEYVALUE_METADATA,
	USER_METADATA,
	DNA_CERTIFICATE,
	PDI,
	BITSTREAM_PARTIAL_PDI,
	PARTITION_METADATA,
	EMULATION_DATA,
	SYSTEM_METADATA,
	SOFT_KERNEL,
	ASK_FLASH,
	AIE_METADATA,
	ASK_GROUP_TOPOLOGY,
	ASK_GROUP_CONNECTIVITY
};

enum MEM_TYPE {
	MEM_DDR3,
	MEM_DDR4,
	MEM_DRAM,
	MEM_STREAMING,
	MEM_PREALLOCATED_GLOB,
	MEM_ARE,
	MEM_HBM,
	MEM_BRAM,
	MEM_URAM,
	MEM_STREAMING_CONNECTION
};

enum IP_TYPE {
	IP_MB = 0,
	IP_KERNEL,
	IP_DNASC,
	IP_DDR4_CONTROLLER,
	IP_MEM_DDR4,
	IP_MEM_HBM
};

struct axlf_section_header {
	uint32_t m_sectionKind;		    /* Section type */
	char m_sectionName[16];		    /* Examples: "stage2", "clear1", */
					    /* "clear2", "ocl1", "ocl2, */
					    /* "ublaze", "sched" */
	uint64_t m_sectionOffset;	    /* File offset of section data */
	uint64_t m_sectionSize;		    /* Size of section data */
};

struct axlf_header {
	uint64_t m_length;		    /* Total size of the xclbin file */
	uint64_t m_timeStamp;		    /* Number of seconds since epoch */
					    /* when xclbin was created */
	uint64_t m_featureRomTimeStamp;	    /* TimeSinceEpoch of the featureRom */
	uint16_t m_versionPatch;	    /* Patch Version */
	uint8_t m_versionMajor;		    /* Major Version - Version: 2.1.0*/
	uint8_t m_versionMinor;		    /* Minor Version */
	uint32_t m_mode;		    /* XCLBIN_MODE */
	union {
		struct {
			uint64_t m_platformId;	/* 64 bit platform ID: */
					/* vendor-device-subvendor-subdev */
			uint64_t m_featureId;	/* 64 bit feature id */
		} rom;
		unsigned char rom_uuid[16];	/* feature ROM UUID for which */
						/* this xclbin was generated */
	};
	unsigned char m_platformVBNV[64];	/* e.g. */
		/* xilinx:xil-accel-rd-ku115:4ddr-xpr:3.4: null terminated */
	union {
		char m_next_axlf[16];		/* Name of next xclbin file */
						/* in the daisy chain */
		uuid_t uuid;			/* uuid of this xclbin*/
	};
	char m_debug_bin[16];			/* Name of binary with debug */
						/* information */
	uint32_t m_numSections;			/* Number of section headers */
};

struct axlf {
	char m_magic[8];			/* Should be "xclbin2\0"  */
	int32_t m_signature_length;		/* Length of the signature. */
						/* -1 indicates no signature */
	unsigned char reserved[28];		/* Note: Initialized to 0xFFs */

	unsigned char m_keyBlock[256];		/* Signature for validation */
						/* of binary */
	uint64_t m_uniqueId;			/* axlf's uniqueId, use it to */
						/* skip redownload etc */
	struct axlf_header m_header;		/* Inline header */
	struct axlf_section_header m_sections[1];   /* One or more section */
						    /* headers follow */
};

/* bitstream information */
struct xlnx_bitstream {
	uint8_t m_freq[8];
	char bits[1];
};

/****	MEMORY TOPOLOGY SECTION ****/
struct mem_data {
	uint8_t m_type; /* enum corresponding to mem_type. */
	uint8_t m_used; /* if 0 this bank is not present */
	union {
		uint64_t m_size; /* if mem_type DDR, then size in KB; */
		uint64_t route_id; /* if streaming then "route_id" */
	};
	union {
		uint64_t m_base_address;/* if DDR then the base address; */
		uint64_t flow_id; /* if streaming then "flow id" */
	};
	unsigned char m_tag[16]; /* DDR: BANK0,1,2,3, has to be null */
			/* terminated; if streaming then stream0, 1 etc */
};

struct mem_topology {
	int32_t m_count; /* Number of mem_data */
	struct mem_data m_mem_data[1]; /* Should be sorted on mem_type */
};

/****	CONNECTIVITY SECTION ****/
/* Connectivity of each argument of Kernel. It will be in terms of argument
 * index associated. For associating kernel instances with arguments and
 * banks, start at the connectivity section. Using the m_ip_layout_index
 * access the ip_data.m_name. Now we can associate this kernel instance
 * with its original kernel name and get the connectivity as well. This
 * enables us to form related groups of kernel instances.
 */

struct connection {
	int32_t arg_index; /* From 0 to n, may not be contiguous as scalars */
			   /* skipped */
	int32_t m_ip_layout_index; /* index into the ip_layout section. */
			   /* ip_layout.m_ip_data[index].m_type == IP_KERNEL */
	int32_t mem_data_index; /* index of the m_mem_data . Flag error is */
				/* m_used false. */
};

struct connectivity {
	int32_t m_count;
	struct connection m_connection[1];
};

/****	IP_LAYOUT SECTION ****/

/* IP Kernel */
#define IP_INT_ENABLE_MASK	  0x0001
#define IP_INTERRUPT_ID_MASK  0x00FE
#define IP_INTERRUPT_ID_SHIFT 0x1

enum IP_CONTROL {
	AP_CTRL_HS = 0,
	AP_CTRL_CHAIN = 1,
	AP_CTRL_NONE = 2,
	AP_CTRL_ME = 3,
	ACCEL_ADAPTER = 4
};

#define IP_CONTROL_MASK	 0xFF00
#define IP_CONTROL_SHIFT 0x8

/* IPs on AXI lite - their types, names, and base addresses.*/
struct ip_data {
	uint32_t m_type; /* map to IP_TYPE enum */
	union {
		uint32_t properties; /* Default: 32-bits to indicate ip */
				     /* specific property. */
		/* m_type: IP_KERNEL
		 *	    m_int_enable   : Bit  - 0x0000_0001;
		 *	    m_interrupt_id : Bits - 0x0000_00FE;
		 *	    m_ip_control   : Bits = 0x0000_FF00;
		 */
		struct {		 /* m_type: IP_MEM_* */
			uint16_t m_index;
			uint8_t m_pc_index;
			uint8_t unused;
		} indices;
	};
	uint64_t m_base_address;
	uint8_t m_name[64]; /* eg Kernel name corresponding to KERNEL */
			    /* instance, can embed CU name in future. */
};

struct ip_layout {
	int32_t m_count;
	struct ip_data m_ip_data[1]; /* All the ip_data needs to be sorted */
				     /* by m_base_address. */
};

/*** Debug IP section layout ****/
enum DEBUG_IP_TYPE {
	UNDEFINED = 0,
	LAPC,
	ILA,
	AXI_MM_MONITOR,
	AXI_TRACE_FUNNEL,
	AXI_MONITOR_FIFO_LITE,
	AXI_MONITOR_FIFO_FULL,
	ACCEL_MONITOR,
	AXI_STREAM_MONITOR,
	AXI_STREAM_PROTOCOL_CHECKER,
	TRACE_S2MM,
	AXI_DMA,
	TRACE_S2MM_FULL
};

struct debug_ip_data {
	uint8_t m_type; /* type of enum DEBUG_IP_TYPE */
	uint8_t m_index_lowbyte;
	uint8_t m_properties;
	uint8_t m_major;
	uint8_t m_minor;
	uint8_t m_index_highbyte;
	uint8_t m_reserved[2];
	uint64_t m_base_address;
	char	m_name[128];
};

struct debug_ip_layout {
	uint16_t m_count;
	struct debug_ip_data m_debug_ip_data[1];
};

/* Supported clock frequency types */
enum CLOCK_TYPE {
	CT_UNUSED = 0,			   /* Initialized value */
	CT_DATA	  = 1,			   /* Data clock */
	CT_KERNEL = 2,			   /* Kernel clock */
	CT_SYSTEM = 3			   /* System Clock */
};

/* Clock Frequency Entry */
struct clock_freq {
	uint16_t m_freq_Mhz;		   /* Frequency in MHz */
	uint8_t m_type;			   /* Clock type (enum CLOCK_TYPE) */
	uint8_t m_unused[5];		   /* Not used - padding */
	char m_name[128];		   /* Clock Name */
};

/* Clock frequency section */
struct clock_freq_topology {
	int16_t m_count;		   /* Number of entries */
	struct clock_freq m_clock_freq[1]; /* Clock array */
};

/* Supported MCS file types */
enum MCS_TYPE {
	MCS_UNKNOWN = 0,		   /* Initialized value */
	MCS_PRIMARY = 1,		   /* The primary mcs file data */
	MCS_SECONDARY = 2,		   /* The secondary mcs file data */
};

/* One chunk of MCS data */
struct mcs_chunk {
	uint8_t m_type;			   /* MCS data type */
	uint8_t m_unused[7];		   /* padding */
	uint64_t m_offset;		   /* data offset from the start of */
					   /* the section */
	uint64_t m_size;		   /* data size */
};

/* MCS data section */
struct mcs {
	int8_t m_count;			   /* Number of chunks */
	int8_t m_unused[7];		   /* padding */
	struct mcs_chunk m_chunk[1];	   /* MCS chunks followed by data */
};

/* bmc data section */
struct bmc {
	uint64_t m_offset;		   /* data offset from the start of */
					   /* the section */
	uint64_t m_size;		   /* data size (bytes) */
	char m_image_name[64];		   /* Name of the image */
					   /* (e.g., MSP432P401R) */
	char m_device_name[64];		   /* Device ID	(e.g., VCU1525)	 */
	char m_version[64];
	char m_md5value[33];		   /* MD5 Expected Value */
				/* (e.g., 56027182079c0bd621761b7dab5a27ca)*/
	char m_padding[7];		   /* Padding */
};

/* soft kernel data section, used by classic driver */
struct soft_kernel {
	/** Prefix Syntax:
	 *  mpo - member, pointer, offset
	 *  This variable represents a zero terminated string
	 *  that is offseted from the beginning of the section.
	 *  The pointer to access the string is initialized as follows:
	 *  char * pCharString = (address_of_section) + (mpo value)
	 */
	uint32_t mpo_name;	   /* Name of the soft kernel */
	uint32_t m_image_offset;   /* Image offset */
	uint32_t m_image_size;	   /* Image size */
	uint32_t mpo_version;	   /* Version */
	uint32_t mpo_md5_value;	   /* MD5 checksum */
	uint32_t mpo_symbol_name;  /* Symbol name */
	uint32_t m_num_instances;  /* Number of instances */
	uint8_t padding[36];	   /* Reserved for future use */
	uint8_t reservedExt[16];   /* Reserved for future extended data */
};

enum CHECKSUM_TYPE {
	CST_UNKNOWN = 0,
	CST_SDBM = 1,
	CST_LAST
};

#ifdef __cplusplus
}
#endif

#endif
