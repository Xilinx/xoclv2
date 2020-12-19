rmmod xclmgmt
modprobe fpga_mgr
modprobe fpga_region
modprobe fpga_bridge
insmod ./lib/xrt-lib.ko
insmod ./mgmt/xmgmt.ko
