/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors: Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_RING_H_
#define _XRT_RING_H_

#ifdef	__KERNEL__

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/barrier.h>
#define	read_barrier()		rmb() /**/
#define	write_barrier()		wmb() /**/
#define	memory_barrier()	mb() /**/

#else /* __KERNEL__ */

#include "ring-user.h"

#endif /* __KERNEL__ */

struct xrt_ring_entry {
	uint64_t xre_id;
	uint32_t xre_flags;
	union {
		int32_t xre_op;		// for SQ
		int32_t xre_op_result;	// for CQ
	};
	/* 64-bit aligned, variable length, can be safely cast to other type */
	char *xre_args[1];
};
#define	XRT_RING_ENTRY_HEADER_SIZE	(sizeof(struct xrt_ring_entry) - 1)

#define	XRT_RING_FLAGS_NEEDS_WAKEUP	(1 << 0)
struct xrt_ring_buffer {
	size_t xrb_entries;
	size_t xrb_entry_size;

	/*
	 * Caching to reduce access to data in shared ring buffer to prevent
	 * CPU cache line from bouncing among CPUs uncessarily.
	 */
	uint32_t xrb_head_cached;
	uint32_t xrb_tail_cached;

	/* Data in shared ring buffer. */
	void *xrb_buf;
	uint32_t *xrb_head;
	uint32_t *xrb_tail;
	uint64_t *xrb_flags;
};

struct xrt_ring {
	void *xr_buf;
	uint64_t *xr_flags;
	struct xrt_ring_buffer xr_sq;
	struct xrt_ring_buffer xr_cq;
};

#define	INVALID_RING_HANDLE	((uint64_t)-1)
struct xrt_ioc_ring_register {
	/* Input parameters. */
	unsigned long xirr_ring_buf;
	size_t xirr_ring_buf_size;
	size_t xirr_sqe_arg_size;
	size_t xirr_cqe_arg_size;

	/* Output parameters. */
	uint64_t xirr_ring_handle;
	off_t xirr_flags_offset;
	off_t xirr_sq_head_offset;
	off_t xirr_sq_tail_offset;
	off_t xirr_sq_ring_offset;
	off_t xirr_cq_head_offset;
	off_t xirr_cq_tail_offset;
	off_t xirr_cq_ring_offset;
	size_t xirr_entries;
};

struct xrt_ioc_ring_unregister {
	size_t xiru_ring_handle;
};

struct xrt_ioc_ring_sq_wakeup {
	size_t xirs_ring_handle;
};

static inline void xrt_ring_struct_init(struct xrt_ring *ring, void *buf,
	struct xrt_ioc_ring_register *reg)
{
	ring->xr_buf = buf;
	ring->xr_flags = buf + reg->xirr_flags_offset;

	// Init sq
	ring->xr_sq.xrb_buf = buf + reg->xirr_sq_ring_offset;
	ring->xr_sq.xrb_entries = reg->xirr_entries;
	ring->xr_sq.xrb_entry_size =
		XRT_RING_ENTRY_HEADER_SIZE + reg->xirr_sqe_arg_size;
	ring->xr_sq.xrb_head = buf + reg->xirr_sq_head_offset;
	ring->xr_sq.xrb_tail = buf + reg->xirr_sq_tail_offset;

	// Init cq
	ring->xr_cq.xrb_buf = buf + reg->xirr_cq_ring_offset;
	ring->xr_cq.xrb_entries = reg->xirr_entries;
	ring->xr_cq.xrb_entry_size =
		XRT_RING_ENTRY_HEADER_SIZE + reg->xirr_cqe_arg_size;
	ring->xr_cq.xrb_head = buf + reg->xirr_cq_head_offset;
	ring->xr_cq.xrb_tail = buf + reg->xirr_cq_tail_offset;
}

static inline void *xrt_ring_entry_ptr(struct xrt_ring_buffer *r, uint32_t idx)
{
	uint32_t pos = idx & (r->xrb_entries - 1);

	return r->xrb_buf + pos * r->xrb_entry_size;
}

static inline size_t xrt_ring_used(struct xrt_ring_buffer *r)
{
	return r->xrb_head_cached - r->xrb_tail_cached;
}

static inline void *xrt_ring_produce_begin(struct xrt_ring_buffer *r)
{
	void *ptr;

	if (xrt_ring_used(r) >= r->xrb_entries) {
		r->xrb_tail_cached = READ_ONCE(*r->xrb_tail);
		/* Make sure all entry writes after this read. */
		memory_barrier();
	}
	/* Check again after updating cache. */
	if (xrt_ring_used(r) >= r->xrb_entries)
		return NULL;

	ptr = xrt_ring_entry_ptr(r, r->xrb_head_cached);
	r->xrb_head_cached++;
	return ptr;
}

static inline void xrt_ring_produce_end(struct xrt_ring_buffer *r)
{
	/* Make sure all prior entry writes happens before this write. */
	write_barrier();
	WRITE_ONCE(*r->xrb_head, r->xrb_head_cached);
}

static inline void *xrt_ring_consume_begin(struct xrt_ring_buffer *r)
{
	void *ptr;

	if (xrt_ring_used(r) == 0) {
		r->xrb_head_cached = READ_ONCE(*r->xrb_head);
		/* Make sure all entry reads happens after this read. */
		read_barrier();
	}
	/* Check again after updating cache. */
	if (xrt_ring_used(r) == 0)
		return NULL;

	ptr = xrt_ring_entry_ptr(r, r->xrb_tail_cached);
	r->xrb_tail_cached++;
	return ptr;
}

static inline void xrt_ring_consume_end(struct xrt_ring_buffer *r)
{
	/* Make sure all prior entry reads happens before this write. */
	memory_barrier();
	WRITE_ONCE(*r->xrb_tail, r->xrb_tail_cached);
}

static inline bool xrt_ring_flag_is_set(struct xrt_ring *r, uint64_t flags)
{
	/* Make sure all prior entry writes happens before this read. */
	memory_barrier();
	return (READ_ONCE(*r->xr_flags) & flags) == flags;
}

static inline void xrt_ring_flag_set(struct xrt_ring *r, uint64_t flags)
{
	WRITE_ONCE(*r->xr_flags, READ_ONCE(*r->xr_flags) | flags);
	/* Make sure all entry reads happens after this read. */
	memory_barrier();
}

static inline void xrt_ring_flag_clear(struct xrt_ring *r, uint64_t flags)
{
	WRITE_ONCE(*r->xr_flags, READ_ONCE(*r->xr_flags) & ~flags);
	/* Make sure all entry reads happens after this read. */
	memory_barrier();
}

#endif /* _XRT_RING_H_ */
