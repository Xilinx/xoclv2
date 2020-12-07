#include <linux/libfdt_env.h>
#include <linux/export.h>
#include "../scripts/dtc/libfdt/fdt_rw.c"

#ifdef CONFIG_FPGA_ALVEO_LIB

EXPORT_SYMBOL_GPL(fdt_del_node);
EXPORT_SYMBOL_GPL(fdt_add_subnode);
EXPORT_SYMBOL_GPL(fdt_pack);
EXPORT_SYMBOL_GPL(fdt_setprop);

#endif
