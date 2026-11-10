/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ION_LEGACY_CACHE_H
#define _ION_LEGACY_CACHE_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Exact pre-4.12 Qualcomm cache ABI; never dereference the legacy vaddr. */
struct ion_legacy_custom64 {
	__u32 cmd;
	__u32 padding;
	__u64 arg;
};

struct ion_legacy_custom32 {
	__u32 cmd;
	__u32 arg;
};

struct ion_legacy_flush64 {
	__s32 handle;
	__s32 fd;
	__u64 vaddr;
	__u32 offset;
	__u32 length;
};

struct ion_legacy_flush32 {
	__s32 handle;
	__s32 fd;
	__u32 vaddr;
	__u32 offset;
	__u32 length;
};

#define ION_LEGACY_CACHE_CUSTOM64 _IOWR('I', 6, struct ion_legacy_custom64)
#define ION_LEGACY_CACHE_CUSTOM32 _IOWR('I', 6, struct ion_legacy_custom32)
#define ION_LEGACY_CACHE_CLEAN64 _IOWR('M', 0, struct ion_legacy_flush64)
#define ION_LEGACY_CACHE_CLEAN32 _IOWR('M', 0, struct ion_legacy_flush32)

#endif
