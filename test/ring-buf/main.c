#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/ioctl.h>
#include "../../src/drivers/fpga/xrt/selftests/xleaf-test.h"

#define PRINT_ERROR \
    do { \
        fprintf(stderr, "Error at line %d, file %s (%d) [%s]\n", \
        __LINE__, __FILE__, errno, strerror(errno)); exit(1); \
    } while(0)
#define	RING_BUF_SIZE	4096
#define MAX_CMD_ID      100
#define MAX_CMD_ISSUED  10000000

static inline uint64_t now()
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return (uint64_t)tp.tv_sec * 1000000000UL + tp.tv_nsec; 
}

static void print_ring_buf_ioc_info(struct xrt_ioc_ring_register *reg)
{
    printf("ring handle: %ld\n", reg->xirr_ring_handle);
    printf("ring flags offset: %ld\n", reg->xirr_flags_offset);
    printf("ring sq head offset: %ld\n", reg->xirr_sq_head_offset);
    printf("ring cq head offset: %ld\n", reg->xirr_cq_head_offset);
    printf("ring sq tail offset: %ld\n", reg->xirr_sq_tail_offset);
    printf("ring cq tail offset: %ld\n", reg->xirr_cq_tail_offset);
    printf("ring sq ring offset: %ld\n", reg->xirr_sq_ring_offset);
    printf("ring cq ring offset: %ld\n", reg->xirr_cq_ring_offset);
    printf("ring entries: %ld\n", reg->xirr_entries);
}

static void issue_cmd(int fd, uint64_t ring_hdl, struct xrt_ring *ring,
    uint64_t cmdid)
{
    struct xrt_ring_entry *entry = NULL;
    struct xrt_ioc_ring_sq_wakeup wakeup = { 0 };

    entry = xrt_ring_produce_begin(&ring->xr_sq);
    if (entry) {
        entry->xre_id = cmdid;
        xrt_ring_produce_end(&ring->xr_sq);
    } else {
            PRINT_ERROR;
    }

    if (xrt_ring_flag_is_set(ring, XRT_RING_FLAGS_NEEDS_WAKEUP)) {
        wakeup.xirs_ring_handle = ring_hdl;
        if (ioctl(fd, XRT_TEST_SQ_WAKEUP, &wakeup) < 0) {
            PRINT_ERROR;
        }
    }
}

static void check_cmd(struct xrt_ring_entry *entry)
{
    if (entry->xre_op_result != 0) {
        PRINT_ERROR;
    }
    if (entry->xre_id >= MAX_CMD_ID) {
        PRINT_ERROR;
    }
}

static uint64_t complete_cmd(int fd, uint64_t ring_hdl, struct xrt_ring *ring)
{
    struct xrt_ring_entry *entry = NULL;

    while (!entry)
        entry = xrt_ring_consume_begin(&ring->xr_cq);
    check_cmd(entry);
    xrt_ring_consume_end(&ring->xr_cq);
    return entry->xre_id;
}

int main(int argc, char **argv)
{
    int fd; 
    void *buf = NULL;
    struct xrt_ioc_ring_register reg = { 0 };
    struct xrt_ioc_ring_unregister unreg = { 0 };
    struct xrt_ring ring = { 0 };
    uint64_t i, ncmds, ringhdl, issued = 0;

    if (argc != 2) {
        printf("USAGE: %s <path-to-test-leaf-dev-node>\n", argv[0]);
        return -1;
    }

    const char *filename = argv[1];

    if((fd = open(filename, O_RDONLY)) == -1) {
        PRINT_ERROR;
    }

    buf = aligned_alloc(getpagesize(), RING_BUF_SIZE);
    if (!buf) {
        PRINT_ERROR;
    }

    reg.xirr_ring_buf = (uintptr_t)buf;
    reg.xirr_ring_buf_size = RING_BUF_SIZE;
    reg.xirr_sqe_arg_size = 0;
    reg.xirr_cqe_arg_size = 0;
    if (ioctl(fd, XRT_TEST_REGISTER_RING, &reg) < 0) {
        PRINT_ERROR;
    }
    xrt_ring_struct_init(&ring, buf, &reg);
    print_ring_buf_ioc_info(&reg);
    ringhdl = reg.xirr_ring_handle;
    ncmds = MAX_CMD_ID > reg.xirr_entries ? reg.xirr_entries - 1 : MAX_CMD_ID;

    uint64_t start = now();

    for (i = 0, issued = 0; i < ncmds && issued < MAX_CMD_ISSUED; i++, issued++)
        issue_cmd(fd, ringhdl, &ring, i);

    while (issued < MAX_CMD_ISSUED) {
        i = complete_cmd(fd, ringhdl, &ring);
        issue_cmd(fd, ringhdl, &ring, i);
        issued++;
    }

    uint64_t end = now();

    printf("successfully completed %ld commands, IOPS: %ld\n",
        issued, (issued * 1000000000) / (end - start));
    printf("Press return to quit...");
    getchar();

    unreg.xiru_ring_handle = ringhdl;
    if (ioctl(fd, XRT_TEST_UNREGISTER_RING, &unreg) < 0) {
        PRINT_ERROR;
    }

    free(buf);
    close(fd);
    return 0;
}
