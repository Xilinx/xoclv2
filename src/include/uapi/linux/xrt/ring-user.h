/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors: Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_RING_USER_H_
#define _XRT_RING_USER_H_

#include <stdint.h>
#include <stdbool.h>

#if defined(__x86_64) || defined(__i386__)
#define	read_barrier()		__asm__ __volatile__("lfence":::"memory")
#define	write_barrier()		__asm__ __volatile__("sfence":::"memory")
#define	memory_barrier()	__asm__ __volatile__("mfence":::"memory")
#else
/*
 * TODO: need to find out fencing instructions for other CPU archs,
 * use full fencing for now, which is less efficient, but safe.
 */
#define read_barrier()		__sync_synchronize()
#define write_barrier()		__sync_synchronize()
#define memory_barrier()	__sync_synchronize()
#endif

#define WRITE_ONCE(x, val) (*((volatile typeof(val) *)(&(x))) = (val))
#define READ_ONCE(x) (*(volatile typeof(x) *)&(x))

#endif /* _XRT_RING_USER_H_ */
