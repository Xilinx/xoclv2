rmmod xclmgmt
modprobe fpga_mgr
insmod ./lib/xrt-lib.ko
insmod ./mgmt/xmgmt.ko
