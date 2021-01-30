/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Xilinx Alveo FPGA Test Leaf Driver
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XRT_METADATA_H
#define _XRT_METADATA_H

#include <linux/device.h>
#include <linux/vmalloc.h>
#include <linux/uuid.h>

#define PROP_COMPATIBLE "compatible"
#define PROP_PF_NUM "pcie_physical_function"
#define PROP_BAR_IDX "pcie_bar_mapping"
#define PROP_IO_OFFSET "reg"
#define PROP_INTERRUPTS "interrupts"
#define PROP_INTERFACE_UUID "interface_uuid"
#define PROP_LOGIC_UUID "logic_uuid"
#define PROP_VERSION_MAJOR "firmware_version_major"

#define PROP_HWICAP "axi_hwicap"
#define PROP_PDI_CONFIG "pdi_config_mem"

#define NODE_ENDPOINTS "addressable_endpoints"
#define INTERFACES_PATH "/interfaces"

#define NODE_FIRMWARE "firmware"
#define NODE_INTERFACES "interfaces"
#define NODE_PARTITION_INFO "partition_info"

#define NODE_FLASH "ep_card_flash_program_00"
#define NODE_XVC_PUB "ep_debug_bscan_user_00"
#define NODE_XVC_PRI "ep_debug_bscan_mgmt_00"
#define NODE_SYSMON "ep_cmp_sysmon_00"
#define NODE_AF_BLP_CTRL_MGMT "ep_firewall_blp_ctrl_mgmt_00"
#define NODE_AF_BLP_CTRL_USER "ep_firewall_blp_ctrl_user_00"
#define NODE_AF_CTRL_MGMT "ep_firewall_ctrl_mgmt_00"
#define NODE_AF_CTRL_USER "ep_firewall_ctrl_user_00"
#define NODE_AF_CTRL_DEBUG "ep_firewall_ctrl_debug_00"
#define NODE_AF_DATA_H2C "ep_firewall_data_h2c_00"
#define NODE_AF_DATA_C2H "ep_firewall_data_c2h_00"
#define NODE_AF_DATA_P2P "ep_firewall_data_p2p_00"
#define NODE_AF_DATA_M2M "ep_firewall_data_m2m_00"
#define NODE_CMC_REG "ep_cmc_regmap_00"
#define NODE_CMC_RESET "ep_cmc_reset_00"
#define NODE_CMC_MUTEX "ep_cmc_mutex_00"
#define NODE_CMC_FW_MEM "ep_cmc_firmware_mem_00"
#define NODE_ERT_FW_MEM "ep_ert_firmware_mem_00"
#define NODE_ERT_CQ_MGMT "ep_ert_command_queue_mgmt_00"
#define NODE_ERT_CQ_USER "ep_ert_command_queue_user_00"
#define NODE_MAILBOX_MGMT "ep_mailbox_mgmt_00"
#define NODE_MAILBOX_USER "ep_mailbox_user_00"
#define NODE_GATE_PLP "ep_pr_isolate_plp_00"
#define NODE_GATE_ULP "ep_pr_isolate_ulp_00"
#define NODE_PCIE_MON "ep_pcie_link_mon_00"
#define NODE_DDR_CALIB "ep_ddr_mem_calib_00"
#define NODE_CLK_KERNEL1 "ep_aclk_kernel_00"
#define NODE_CLK_KERNEL2 "ep_aclk_kernel_01"
#define NODE_CLK_KERNEL3 "ep_aclk_hbm_00"
#define NODE_KDMA_CTRL "ep_kdma_ctrl_00"
#define NODE_FPGA_CONFIG "ep_fpga_configuration_00"
#define NODE_ERT_SCHED "ep_ert_sched_00"
#define NODE_XDMA "ep_xdma_00"
#define NODE_MSIX "ep_msix_00"
#define NODE_QDMA "ep_qdma_00"
#define NODE_QDMA4 "ep_qdma4_00"
#define NODE_STM "ep_stream_traffic_manager_00"
#define NODE_STM4 "ep_stream_traffic_manager4_00"
#define NODE_CLK_SHUTDOWN "ep_aclk_shutdown_00"
#define NODE_ERT_BASE "ep_ert_base_address_00"
#define NODE_ERT_RESET "ep_ert_reset_00"
#define NODE_CLKFREQ_K1 "ep_freq_cnt_aclk_kernel_00"
#define NODE_CLKFREQ_K2 "ep_freq_cnt_aclk_kernel_01"
#define NODE_CLKFREQ_HBM "ep_freq_cnt_aclk_hbm_00"
#define NODE_GAPPING "ep_gapping_demand_00"
#define NODE_UCS_CONTROL_STATUS "ep_ucs_control_status_00"
#define NODE_P2P "ep_p2p_00"
#define NODE_REMAP_P2P "ep_remap_p2p_00"
#define NODE_DDR4_RESET_GATE "ep_ddr_mem_srsr_gate_00"
#define NODE_ADDR_TRANSLATOR "ep_remap_data_c2h_00"
#define NODE_MAILBOX_XRT "ep_mailbox_user_to_ert_00"
#define NODE_PMC_INTR   "ep_pmc_intr_00"
#define NODE_PMC_MUX    "ep_pmc_mux_00"

/* driver defined endpoints */
#define NODE_VSEC "drv_ep_vsec_00"
#define NODE_VSEC_GOLDEN "drv_ep_vsec_golden_00"
#define NODE_BLP_ROM "drv_ep_blp_rom_00"
#define NODE_MAILBOX_VSEC "ep_mailbox_vsec_00"
#define NODE_PLAT_INFO "drv_ep_platform_info_mgmt_00"
#define NODE_TEST "drv_ep_test_00"
#define NODE_MGMT_MAIN "drv_ep_mgmt_main_00"
#define NODE_FLASH_VSEC "drv_ep_card_flash_program_00"
#define NODE_GOLDEN_VER "drv_ep_golden_ver_00"
#define NODE_PARTITION_INFO_BLP "partition_info_0"
#define NODE_PARTITION_INFO_PLP "partition_info_1"

#define NODE_DDR_SRSR "drv_ep_ddr_srsr"
#define REGMAP_DDR_SRSR "drv_ddr_srsr"

#define PROP_OFFSET "drv_offset"
#define PROP_CLK_FREQ "drv_clock_frequency"
#define PROP_CLK_CNT "drv_clock_frequency_counter"
#define	PROP_VBNV "vbnv"
#define	PROP_VROM "vrom"
#define PROP_PARTITION_LEVEL "partition_level"

struct xrt_md_endpoint {
	const char	*ep_name;
	u32		bar;
	long		bar_off;
	ulong		size;
	char		*regmap;
	char		*regmap_ver;
};

/* Note: res_id is defined by leaf driver and must start with 0. */
struct xrt_iores_map {
	char		*res_name;
	int		res_id;
};

static inline int xrt_md_res_name2id(const struct xrt_iores_map *res_map,
				     int entry_num, const char *res_name)
{
	int i;

	for (i = 0; i < entry_num; i++) {
		if (!strcmp(res_name, res_map->res_name))
			return res_map->res_id;
		res_map++;
	}
	return -1;
}

static inline const char *
xrt_md_res_id2name(const struct xrt_iores_map *res_map, int entry_num, int id)
{
	int i;

	for (i = 0; i < entry_num; i++) {
		if (res_map->res_id == id)
			return res_map->res_name;
		res_map++;
	}
	return NULL;
}

long xrt_md_size(struct device *dev, const char *blob);
int xrt_md_create(struct device *dev, char **blob);
int xrt_md_add_endpoint(struct device *dev, char *blob,
			struct xrt_md_endpoint *ep);
int xrt_md_del_endpoint(struct device *dev, char *blob, const char *ep_name,
			char *regmap_name);
int xrt_md_get_prop(struct device *dev, const char *blob, const char *ep_name,
		    const char *regmap_name, const char *prop,
		    const void **val, int *size);
int xrt_md_set_prop(struct device *dev, char *blob, const char *ep_name,
		    const char *regmap_name, const char *prop,
		    const void *val, int size);
int xrt_md_copy_endpoint(struct device *dev, char *blob, const char *src_blob,
			 const char *ep_name, const char *regmap_name,
			 const char *new_ep_name);
int xrt_md_copy_all_eps(struct device *dev, char  *blob, const char *src_blob);
int xrt_md_get_next_endpoint(struct device *dev, const char *blob,
			     const char *ep_name,  const char *regmap_name,
			     char **next_ep, char **next_regmap);
int xrt_md_get_compatible_epname(struct device *dev, const char *blob,
				 const char *regmap_name, const char **ep_name);
int xrt_md_get_epname_pointer(struct device *dev, const char *blob,
			      const char *ep_name, const char *regmap_name,
			      const char **epname);
void xrt_md_pack(struct device *dev, char *blob);
char *xrt_md_dup(struct device *dev, const char *blob);
int xrt_md_get_intf_uuids(struct device *dev, const char *blob,
			  u32 *num_uuids, uuid_t *intf_uuids);
int xrt_md_check_uuids(struct device *dev, const char *blob, char *subset_blob);
int xrt_md_uuid_strtoid(struct device *dev, const char *uuidstr, uuid_t *uuid);

#endif
