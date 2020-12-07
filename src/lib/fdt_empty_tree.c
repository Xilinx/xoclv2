#include <linux/libfdt_env.h>
#include <linux/export.h>
#include "../scripts/dtc/libfdt/fdt_empty_tree.c"

#ifdef CONFIG_FPGA_ALVEO_LIB

EXPORT_SYMBOL_GPL(fdt_create_empty_tree);

#endif
