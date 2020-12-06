#include <linux/libfdt_env.h>
#include <linux/export.h>
#include "../scripts/dtc/libfdt/fdt_ro.c"

EXPORT_SYMBOL_GPL(fdt_getprop_by_offset);
EXPORT_SYMBOL_GPL(fdt_node_check_compatible);
EXPORT_SYMBOL_GPL(fdt_get_name);
EXPORT_SYMBOL_GPL(fdt_next_property_offset);
EXPORT_SYMBOL_GPL(fdt_getprop);
EXPORT_SYMBOL_GPL(fdt_node_offset_by_compatible);
EXPORT_SYMBOL_GPL(fdt_parent_offset);
EXPORT_SYMBOL_GPL(fdt_stringlist_get);
EXPORT_SYMBOL_GPL(fdt_first_property_offset);
