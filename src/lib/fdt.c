#include <linux/libfdt_env.h>
#include <linux/export.h>
#include "../scripts/dtc/libfdt/fdt.c"

#ifdef CONFIG_FPGA_ALVEO_LIB

EXPORT_SYMBOL_GPL(fdt_next_node);
EXPORT_SYMBOL_GPL(fdt_first_subnode);
EXPORT_SYMBOL_GPL(fdt_next_subnode);
EXPORT_SYMBOL_GPL(fdt_subnode_offset);

#endif
