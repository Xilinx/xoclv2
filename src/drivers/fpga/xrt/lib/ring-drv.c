// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Kernel Driver Ring Buffer
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors: Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/types.h>
#include <linux/cache.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include "ring-drv.h"

/*
 * Global link list to maintain dev-to-ring-handle mapping, which is needed
 * in sysfs handlers
 */
static DEFINE_MUTEX(ring_global_lock);
static LIST_HEAD(ring_dev_list);
struct ring_dev {
	struct list_head node;
	struct device *dev;
	void *ring_hdl;
};

static inline int xrt_ring_register_dev(struct device *dev, void *ring_hdl)
{
	struct ring_dev *rd = vzalloc(sizeof(struct ring_dev));

	if (!rd)
		return -ENOMEM;

	rd->dev = dev;
	rd->ring_hdl = ring_hdl;
	mutex_lock(&ring_global_lock);
	list_add_tail(&rd->node, &ring_dev_list);
	mutex_unlock(&ring_global_lock);
	return 0;
}

static void *xrt_ring_dev2handle(struct device *dev)
{
	struct ring_dev *rd;
	void *r = NULL;

	mutex_lock(&ring_global_lock);
	list_for_each_entry(rd, &ring_dev_list, node) {
		if (rd->dev == dev) {
			r = rd->ring_hdl;
			break;
		}
	}
	mutex_unlock(&ring_global_lock);
	return r;
}

static inline void xrt_ring_unregister_dev(struct device *dev)
{
	struct ring_dev *rd;

	mutex_lock(&ring_global_lock);
	list_for_each_entry(rd, &ring_dev_list, node) {
		if (rd->dev == dev)
			break;
	}
	list_del(&rd->node);
	vfree(rd);
	mutex_unlock(&ring_global_lock);
}

#define	MAX_RING_BUF_SIZE	(32 * 1024 * 1024)
#define	MAX_RING_BUF_NUM	(2 * 1024)
#define INVALID_RING_PTR	((struct xrt_ring_drv *)(uintptr_t)-1)

#define xring_err(rings, fmt, args...)                       \
	dev_err(rings->dev, "%s: "fmt, __func__, ##args)
#define xring_warn(rings, fmt, args...)                      \
	dev_warn(rings->dev, "%s: "fmt, __func__, ##args)
#define xring_info(rings, fmt, args...)                      \
	dev_info(rings->dev, "%s: "fmt, __func__, ##args)
#define xring_dbg(rings, fmt, args...)                       \
	dev_dbg(rings->dev, "%s: "fmt, __func__, ##args)

#define	RING_DRV2SQ(ring)	(&ring->shared_ring.xr_sq)
#define	RING_DRV2CQ(ring)	(&ring->shared_ring.xr_cq)

struct xrt_ring_header {
	uint64_t flags ____cacheline_aligned_in_smp;
	uint32_t sq_head ____cacheline_aligned_in_smp;
	uint32_t cq_head ____cacheline_aligned_in_smp;
	uint32_t sq_tail ____cacheline_aligned_in_smp;
	uint32_t cq_tail ____cacheline_aligned_in_smp;
};

enum sq_worker_stages {
	SQ_WORKER_BUSY_POLL = 0,
	SQ_WORKER_SLOW_POLL,
	SQ_WORKER_POLL_WITH_WAKEUP_FLAG,
	SQ_WORKER_SLEEP,
	// always the last one
	SQ_WORKER_MAX_STAGE
};

static int sq_worker_stage_poll_miss[SQ_WORKER_MAX_STAGE] = {
	50, 500, 1, 1
};

struct xrt_ring_drv {
	struct xrt_rings *parent;
	int index;
	struct mutex mutex;
	bool closing;
	struct xrt_ring shared_ring;
	enum sq_worker_stages sq_stage;
	int sq_stage_poll_miss;
	xrt_ring_req_handler req_handler;
	void *req_handler_arg;

	struct completion comp_sq;
	struct workqueue_struct	*sq_workq;
	struct work_struct sq_worker;
	u64 num_stages[SQ_WORKER_MAX_STAGE];
};

struct xrt_rings {
	struct device *dev;
	struct mutex lock;
	size_t max_num_rings;
	struct xrt_ring_drv *rings[1];
	u64 sysfs_cur_ring_id;
};

static ssize_t num_stage_transit_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	size_t id, i;
	struct xrt_rings *rings = xrt_ring_dev2handle(dev);
	struct xrt_ring_drv *r;
	int ret = kstrtol(buf, 0, &id);

	if (ret || id >= rings->max_num_rings) {
		xring_err(rings, "input should be int and < %ld",
			rings->max_num_rings);
		return -EINVAL;
	}

	rings->sysfs_cur_ring_id = id;

	mutex_lock(&rings->lock);
	r = rings->rings[rings->sysfs_cur_ring_id];
	if (r) {
		for (i = 0; i < SQ_WORKER_MAX_STAGE; i++)
			r->num_stages[i] = 0;
	}
	mutex_unlock(&rings->lock);
	return count;
}
static ssize_t num_stage_transit_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	int i;
	ssize_t sz = 0;
	struct xrt_ring_drv *r;
	struct xrt_rings *rings = xrt_ring_dev2handle(dev);

	mutex_lock(&rings->lock);
	r = rings->rings[rings->sysfs_cur_ring_id];
	if (r) {
		for (i = 0; i < SQ_WORKER_MAX_STAGE; i++) {
			sz += sprintf(buf + sz, "stage %d: %lld\n", i,
				r->num_stages[i]);
		}
	} else {
		sz += sprintf(buf, "ring is not available\n");
	}
	mutex_unlock(&rings->lock);
	return sz;
}
static DEVICE_ATTR_RW(num_stage_transit);

static struct attribute *xrt_ring_attrs[] = {
	&dev_attr_num_stage_transit.attr,
	NULL,
};

static struct attribute_group xrt_ring_attr_group = {
	.attrs = xrt_ring_attrs,
};

void *xrt_ring_probe(struct device *dev, size_t max_num_rings)
{
	struct xrt_rings *rings;
	int ret;

	if (max_num_rings > MAX_RING_BUF_NUM)
		max_num_rings = MAX_RING_BUF_NUM;

	rings = vzalloc(sizeof(struct xrt_rings) +
		(max_num_rings - 1) * sizeof(struct xrt_ring *));
	if (rings) {
		mutex_init(&rings->lock);
		rings->max_num_rings = max_num_rings;
		rings->dev = dev;
	}

	ret = xrt_ring_register_dev(dev, rings);
	if (ret) {
		vfree(rings);
		rings = NULL;
	}

	ret  = sysfs_create_group(&dev->kobj, &xrt_ring_attr_group);
	if (ret)
		xring_err(rings, "failed to create sysfs nodes: %d", ret);

	return rings;
}
EXPORT_SYMBOL_GPL(xrt_ring_probe);

void xrt_ring_remove(void *handle)
{
	int i;
	struct xrt_rings *rings = (struct xrt_rings *)handle;

	(void) sysfs_remove_group(&rings->dev->kobj, &xrt_ring_attr_group);
	xrt_ring_unregister_dev(rings->dev);
	for (i = 0; i < rings->max_num_rings; i++)
		BUG_ON(rings->rings[i]);
	vfree(rings);
}
EXPORT_SYMBOL_GPL(xrt_ring_remove);

static void *map_ring(struct xrt_rings *rings, unsigned long addr, size_t sz)
{
	struct page **pages = NULL;
	size_t page_cnt = 0;
	size_t page_pinned = 0;
	void *ret = NULL;

	if ((u64)(uintptr_t)addr % PAGE_SIZE) {
		xring_err(rings, "ring buffer addr %ld not page aligned", addr);
		goto out;
	}

	if (sz > MAX_RING_BUF_SIZE) {
		xring_err(rings, "ring buffer size %ld too big", sz);
		goto out;
	}

	if (!access_ok((const void __user *)addr, sz)) {
		xring_err(rings, "can't access user ring buffer");
		goto out;
	}

	page_cnt = (sz >> PAGE_SHIFT) + ((sz % PAGE_SIZE) ? 1 : 0);
	if (!page_cnt) {
		xring_err(rings, "ring buffer size %ld too small", sz);
		goto out;
	}

	pages = kvmalloc_array(page_cnt, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		goto out;

	page_pinned = get_user_pages_fast(addr, page_cnt, FOLL_WRITE, pages);
	if (page_pinned != page_cnt) {
		xring_err(rings, "can't pin down all pages");
		goto out;
	}

	ret = vmap(pages, page_cnt, VM_MAP, PAGE_KERNEL);
	if (!ret)
		xring_err(rings, "can't map in %ld pages", page_cnt);

out:
	if (page_pinned)
		release_pages(pages, page_pinned);
	if (pages)
		kvfree(pages);
	if (ret)
		xring_info(rings, "successfully mapped in user ring buf");
	return ret;
}

static void unmap_ring(struct xrt_rings *rings, void *kva)
{
	vunmap(kva);
	xring_info(rings, "successfully unmapped user ring buf");
}

static size_t ring_entries(size_t total_sz,
	size_t sqe_arg_sz, size_t cqe_arg_sz)
{
	size_t array_sz = total_sz - sizeof(struct xrt_ring_header);
	size_t entry_size =
		XRT_RING_ENTRY_HEADER_SIZE * 2 + cqe_arg_sz + sqe_arg_sz;

	if (total_sz < sizeof(struct xrt_ring_header))
		return 0;

	return rounddown_pow_of_two(array_sz / entry_size);
}

static inline void stage_transit_poll_hit(struct xrt_ring_drv *ring)
{
	enum sq_worker_stages prev = ring->sq_stage;

	if (prev == SQ_WORKER_BUSY_POLL)
		return;

	ring->sq_stage = SQ_WORKER_BUSY_POLL;
	ring->sq_stage_poll_miss = 0;
	ring->num_stages[ring->sq_stage]++;

	if (prev == SQ_WORKER_POLL_WITH_WAKEUP_FLAG) {
		xrt_ring_flag_clear(&ring->shared_ring,
			XRT_RING_FLAGS_NEEDS_WAKEUP);
	}
}

static inline void stage_transit_poll_miss(struct xrt_ring_drv *ring)
{
	enum sq_worker_stages st = ring->sq_stage;
	int max_miss = sq_worker_stage_poll_miss[st];

	ring->sq_stage_poll_miss++;

	/*
	 * Do nothing if not ready to move to next stage
	 * or already at last one.
	 */
	if (ring->sq_stage_poll_miss < max_miss ||
		(st + 1) == SQ_WORKER_MAX_STAGE)
		return;

	st++;
	if (st == SQ_WORKER_POLL_WITH_WAKEUP_FLAG) {
		xrt_ring_flag_set(&ring->shared_ring,
			XRT_RING_FLAGS_NEEDS_WAKEUP);
	}
	ring->sq_stage = st;
	ring->sq_stage_poll_miss = 0;
	ring->num_stages[ring->sq_stage]++;
}

static inline void wait_before_next_poll(struct xrt_ring_drv *ring)
{
	struct xrt_rings *rings = ring->parent;

	switch (ring->sq_stage) {
	case SQ_WORKER_BUSY_POLL:
		/* Immediately make the next poll from SQ ring. */
		break;
	case SQ_WORKER_SLOW_POLL:
	case SQ_WORKER_POLL_WITH_WAKEUP_FLAG:
		/* Make the next poll from SQ ring after waiting for 1us. */
		usleep_range(1, 2);
		break;
	case SQ_WORKER_SLEEP:
		/* Wait for wakeup call before making next poll from SQ. */
		(void) wait_for_completion_interruptible(&ring->comp_sq);
		break;
	default:
		xring_err(rings, "SQ worker unknown stage: %d", ring->sq_stage);
		break;
	}
}

static void sq_worker_thread(struct work_struct *work)
{
	struct xrt_ring_drv *ring =
		container_of(work, struct xrt_ring_drv, sq_worker);
	struct xrt_rings *rings = ring->parent;

	xring_info(rings, "SQ worker started: ring %d of %s",
		ring->index, dev_name(rings->dev));
	while (!ring->closing) {
		void *sqe = xrt_ring_consume_begin(RING_DRV2SQ(ring));

		if (sqe) {
			ring->req_handler(ring->req_handler_arg, sqe,
				RING_DRV2SQ(ring)->xrb_entry_size);
			xrt_ring_consume_end(RING_DRV2SQ(ring));

			stage_transit_poll_hit(ring);
		} else {
			stage_transit_poll_miss(ring);
		}

		wait_before_next_poll(ring);
	}
	xring_info(rings, "SQ worker ended: ring %d of %s",
		ring->index, dev_name(rings->dev));
}

static int xrt_ring_init(struct xrt_rings *rings, int index,
	struct xrt_ring_drv *ring, void *buf, struct xrt_ioc_ring_register *reg,
	xrt_ring_req_handler handler, void *handler_arg)
{
	char wqname[128];

	snprintf(wqname, sizeof(wqname), "%s:%d", dev_name(rings->dev), index);
	ring->sq_workq = create_singlethread_workqueue(wqname);
	if (ring->sq_workq == NULL) {
		xring_err(rings, "failed to create work q for SQ ring");
		return -ENOMEM;
	}
	INIT_WORK(&ring->sq_worker, sq_worker_thread);

	ring->parent = rings;
	ring->index = index;
	ring->req_handler = handler;
	ring->req_handler_arg = handler_arg;
	mutex_init(&ring->mutex);
	ring->closing = false;
	init_completion(&ring->comp_sq);
	xrt_ring_struct_init(&ring->shared_ring, buf, reg);

	mutex_lock(&ring->mutex);
	ring->closing = false;
	mutex_unlock(&ring->mutex);
	queue_work(ring->sq_workq, &ring->sq_worker);
	return 0;
}

int xrt_ring_register(void *handle, struct xrt_ioc_ring_register *reg,
	xrt_ring_req_handler handler, void *arg)
{
	size_t i, entries;
	struct xrt_ring_drv *ring;
	struct xrt_rings *rings = (struct xrt_rings *)handle;
	void *ring_buf;
	int ret;

	entries = ring_entries(reg->xirr_ring_buf_size,
		reg->xirr_sqe_arg_size, reg->xirr_cqe_arg_size);
	if (!entries) {
		xring_err(rings, "total ring size (%ld) is too small",
			reg->xirr_ring_buf_size);
		xring_err(rings, "or arg size is too big: sqe (%ld), cqe (%ld)",
			reg->xirr_sqe_arg_size, reg->xirr_cqe_arg_size);
		return -EINVAL;
	}

	// reserve the ring slot
	mutex_lock(&rings->lock);
	for (i = 0; i < rings->max_num_rings && rings->rings[i]; i++)
		;
	if (i == rings->max_num_rings) {
		xring_err(rings, "can't register more than %ld rings",
			rings->max_num_rings);
		return -ENOSPC;
	}
	rings->rings[i] = INVALID_RING_PTR;
	mutex_unlock(&rings->lock);

	ring = vzalloc(sizeof(struct xrt_ring_drv));
	if (!ring) {
		rings->rings[i] = NULL;
		return -ENOMEM;
	}

	ring_buf = map_ring(rings, reg->xirr_ring_buf, reg->xirr_ring_buf_size);
	if (!ring_buf) {
		vfree(ring);
		rings->rings[i] = NULL;
		return -EINVAL;
	}
	/* All data in shared ring buffer should start with zero. */
	memset(ring_buf, 0, reg->xirr_ring_buf_size);

	reg->xirr_ring_handle = i;
	reg->xirr_flags_offset = offsetof(struct xrt_ring_header, flags);
	reg->xirr_sq_head_offset = offsetof(struct xrt_ring_header, sq_head);
	reg->xirr_sq_tail_offset = offsetof(struct xrt_ring_header, sq_tail);
	reg->xirr_sq_ring_offset = sizeof(struct xrt_ring_header);
	reg->xirr_cq_head_offset = offsetof(struct xrt_ring_header, cq_head);
	reg->xirr_cq_tail_offset = offsetof(struct xrt_ring_header, cq_tail);
	reg->xirr_cq_ring_offset = reg->xirr_sq_ring_offset +
		(XRT_RING_ENTRY_HEADER_SIZE + reg->xirr_sqe_arg_size) * entries;
	reg->xirr_entries = entries;

	ret = xrt_ring_init(rings, i, ring, ring_buf, reg, handler, arg);
	if (ret) {
		unmap_ring(rings, ring_buf);
		vfree(ring);
		rings->rings[i] = NULL;
	} else {
		rings->rings[i] = ring;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(xrt_ring_register);

static void xrt_ring_fini(struct xrt_ring_drv *ring)
{
	mutex_lock(&ring->mutex);
	ring->closing = true;
	mutex_unlock(&ring->mutex);

	complete(&ring->comp_sq);
	cancel_work_sync(&ring->sq_worker);
	destroy_workqueue(ring->sq_workq);
}

int xrt_ring_unregister(void *handle, struct xrt_ioc_ring_unregister *unreg)
{
	int ret = 0;
	struct xrt_ring_drv *ring = NULL;
	size_t i = unreg->xiru_ring_handle;
	struct xrt_rings *rings = (struct xrt_rings *)handle;

	if (i >= rings->max_num_rings) {
		xring_err(rings, "ring %ld not valid", i);
		return -EINVAL;
	}

	mutex_lock(&rings->lock);
	ring = rings->rings[i];
	rings->rings[i] = NULL;
	mutex_unlock(&rings->lock);

	if (ring) {
		xrt_ring_fini(ring);
		unmap_ring(rings, ring->shared_ring.xr_buf);
		vfree(ring);
	} else {
		xring_err(rings, "ring %ld not found", i);
		ret = -ENOENT;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(xrt_ring_unregister);

static inline struct xrt_ring_drv *handle2ring(void *handle, u64 ring_hdl)
{
	struct xrt_rings *rings = (struct xrt_rings *)handle;

	BUG_ON(ring_hdl == INVALID_RING_HANDLE);
	return rings->rings[ring_hdl];
}

struct xrt_ring_entry *xrt_ring_cq_produce_begin(void *handle,
	u64 ring_hdl, size_t *sz)
{
	struct xrt_ring_buffer *r = RING_DRV2CQ(handle2ring(handle, ring_hdl));

	if (sz)
		*sz = r->xrb_entry_size;
	return xrt_ring_produce_begin(r);
}
EXPORT_SYMBOL_GPL(xrt_ring_cq_produce_begin);

void xrt_ring_cq_produce_end(void *handle, u64 ring_hdl)
{
	struct xrt_ring_buffer *r = RING_DRV2CQ(handle2ring(handle, ring_hdl));

	xrt_ring_produce_end(r);
}
EXPORT_SYMBOL_GPL(xrt_ring_cq_produce_end);

int xrt_ring_sq_wakeup(void *handle, struct xrt_ioc_ring_sq_wakeup *wakeup)
{
	int ret = 0;
	struct xrt_ring_drv *ring = NULL;
	size_t i = wakeup->xirs_ring_handle;
	struct xrt_rings *rings = (struct xrt_rings *)handle;

	if (i >= rings->max_num_rings) {
		xring_err(rings, "ring %ld not valid", i);
		return -EINVAL;
	}

	ring = rings->rings[i];

	if (ring && ring != INVALID_RING_PTR) {
		complete(&ring->comp_sq);
	} else {
		xring_err(rings, "ring %ld not found", i);
		ret = -ENOENT;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(xrt_ring_sq_wakeup);
