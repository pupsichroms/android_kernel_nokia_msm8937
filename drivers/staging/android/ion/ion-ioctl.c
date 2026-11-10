// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2011 Google, Inc.
 */

#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include "ion.h"
#include "ion_system_secure_heap.h"

#ifdef CONFIG_ION_LEGACY
#include <linux/dma-buf.h>
#include "ion_legacy.h"
#include "ion_legacy_cache.h"
#endif

union ion_ioctl_arg {
	struct ion_allocation_data allocation;
	struct ion_heap_query query;
	struct ion_prefetch_data prefetch_data;
#ifdef CONFIG_ION_LEGACY
	struct ion_fd_data fd;
	struct ion_old_allocation_data old_allocation;
	struct ion_handle_data handle;
#endif
};

static int validate_ioctl_arg(unsigned int cmd, union ion_ioctl_arg *arg)
{
	switch (cmd) {
	case ION_IOC_HEAP_QUERY:
		if (arg->query.reserved0 ||
		    arg->query.reserved1 ||
		    arg->query.reserved2)
			return -EINVAL;
		break;
	default:
		break;
	}

	return 0;
}

/* fix up the cases where the ioctl direction bits are incorrect */
static unsigned int ion_ioctl_dir(unsigned int cmd)
{
	switch (cmd) {
#ifdef CONFIG_ION_LEGACY
	case ION_IOC_FREE:
		return _IOC_WRITE;
#endif
	default:
		return _IOC_DIR(cmd);
	}
}

#ifdef CONFIG_ION_LEGACY
static bool ion_legacy_cache_cmd(unsigned int cmd)
{
	return cmd == ION_LEGACY_CACHE_CUSTOM64 ||
		cmd == ION_LEGACY_CACHE_CUSTOM32;
}

static long ion_legacy_cache_ioctl(unsigned int cmd, unsigned long arg)
{
	struct dma_buf *dmabuf;
	u64 payload;
	u32 cache_cmd, first_cmd, offset, length;
	int fd, ret;

	if (cmd == ION_LEGACY_CACHE_CUSTOM64) {
		struct ion_legacy_custom64 custom;
		struct ion_legacy_flush64 flush;

		if (copy_from_user(&custom, (void __user *)arg, sizeof(custom)))
			return -EFAULT;
		cache_cmd = custom.cmd;
		first_cmd = ION_LEGACY_CACHE_CLEAN64;
		payload = custom.arg;
		if (cache_cmd < first_cmd || cache_cmd > first_cmd + 2)
			return -ENOTTY;
		if (copy_from_user(&flush, u64_to_user_ptr(payload), sizeof(flush)))
			return -EFAULT;
		fd = flush.handle > 0 ? flush.handle : flush.fd;
		offset = flush.offset;
		length = flush.length;
	} else if (cmd == ION_LEGACY_CACHE_CUSTOM32) {
		struct ion_legacy_custom32 custom;
		struct ion_legacy_flush32 flush;

		if (copy_from_user(&custom, (void __user *)arg, sizeof(custom)))
			return -EFAULT;
		cache_cmd = custom.cmd;
		first_cmd = ION_LEGACY_CACHE_CLEAN32;
		payload = custom.arg;
		if (cache_cmd < first_cmd || cache_cmd > first_cmd + 2)
			return -ENOTTY;
		if (copy_from_user(&flush, u64_to_user_ptr(payload), sizeof(flush)))
			return -EFAULT;
		fd = flush.handle > 0 ? flush.handle : flush.fd;
		offset = flush.offset;
		length = flush.length;
	} else {
		return -ENOTTY;
	}

	if (fd < 0)
		return -EBADF;

	/* CONFIG_ION_LEGACY already represents old handles as owned DMA-buf FDs. */
	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	if (!length || (u64)offset + length > dmabuf->size) {
		ret = -EINVAL;
		goto put_buffer;
	}

	/* Old cache requests are not DMA-buf ownership transitions. */
	ret = ion_legacy_cache_sync(dmabuf, offset, length, cache_cmd - first_cmd);

put_buffer:
	dma_buf_put(dmabuf);
	return ret;
}
#endif

long ion_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	unsigned int dir;
	union ion_ioctl_arg data;

#ifdef CONFIG_ION_LEGACY
	if (ion_legacy_cache_cmd(cmd))
		return ion_legacy_cache_ioctl(cmd, arg);
#endif

	dir = ion_ioctl_dir(cmd);

	if (_IOC_SIZE(cmd) > sizeof(data))
		return -EINVAL;

	/*
	 * The copy_from_user is unconditional here for both read and write
	 * to do the validate. If there is no write for the ioctl, the
	 * buffer is cleared
	 */
	if (copy_from_user(&data, (void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	ret = validate_ioctl_arg(cmd, &data);
	if (ret) {
		pr_warn_once("%s: ioctl validate failed\n", __func__);
		return ret;
	}

	if (!(dir & _IOC_WRITE))
		memset(&data, 0, sizeof(data));

	switch (cmd) {
	case ION_IOC_ALLOC:
	{
		int fd;

		fd = ion_alloc_fd(data.allocation.len,
				  data.allocation.heap_id_mask,
				  data.allocation.flags);
		if (fd < 0)
			return fd;

		data.allocation.fd = fd;

		break;
	}
	case ION_IOC_HEAP_QUERY:
		ret = ion_query_heaps(&data.query);
		break;
	case ION_IOC_PREFETCH:
	{
		int ret;

		ret = ion_walk_heaps(data.prefetch_data.heap_id,
				     (enum ion_heap_type)
				     ION_HEAP_TYPE_SYSTEM_SECURE,
				     (void *)&data.prefetch_data,
				     ion_system_secure_heap_prefetch);
		if (ret)
			return ret;
		break;
	}
	case ION_IOC_DRAIN:
	{
		int ret;

		ret = ion_walk_heaps(data.prefetch_data.heap_id,
				     (enum ion_heap_type)
				     ION_HEAP_TYPE_SYSTEM_SECURE,
				     (void *)&data.prefetch_data,
				     ion_system_secure_heap_drain);

		if (ret)
			return ret;
		break;
	}
#ifdef CONFIG_ION_LEGACY
	case ION_OLD_IOC_ALLOC:
	{
		int fd;

		fd = ion_alloc_fd(data.old_allocation.len,
				  data.old_allocation.heap_id_mask,
				  data.old_allocation.flags);
		if (fd < 0)
			return fd;

		data.old_allocation.handle = fd;

		break;
	}
	case ION_IOC_FREE:
		/*
		 * libion passes 0 as the handle to check for this ioctl's
		 * existence and expects -ENOTTY on kernel 4.12+ as an indicator
		 * of having a new ION ABI. We want to use new ION as much as
		 * possible, so pretend that this ioctl doesn't exist when
		 * libion checks for it.
		 */
		if (!data.handle.handle)
			ret = -ENOTTY;

		break;
	case ION_IOC_SHARE:
	case ION_IOC_MAP:
		data.fd.fd = data.fd.handle;
		break;
	case ION_IOC_IMPORT:
		data.fd.handle = data.fd.fd;
		break;
#endif
	default:
		return -ENOTTY;
	}

	if (dir & _IOC_READ) {
		if (copy_to_user((void __user *)arg, &data, _IOC_SIZE(cmd)))
			return -EFAULT;
	}
	return ret;
}
