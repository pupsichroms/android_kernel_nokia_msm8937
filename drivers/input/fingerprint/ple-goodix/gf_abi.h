/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PLE619F HMD d548dae4 goodix_gfx18m ABI, customer1 (not Santoni netlink). */
#ifndef PLE_GOODIX_ABI_H
#define PLE_GOODIX_ABI_H
#include <linux/errno.h>
#include <linux/input-event-codes.h>
#include <linux/ioctl.h>
#include <linux/types.h>

struct gf_key { __u32 key; __s32 value; };
#define GF_IOC_DISABLE_IRQ _IO('G', 0)
#define GF_IOC_ENABLE_IRQ _IO('G', 1)
#define GF_IOC_SETSPEED _IOW('G', 2, __u32)
#define GF_IOC_RESET _IO('G', 3)
#define GF_IOC_COOLBOOT _IO('G', 4)
#define GF_IOC_SENDKEY _IOW('G', 5, struct gf_key)
#define GF_IOC_CLK_READY _IO('G', 6)
#define GF_IOC_CLK_UNREADY _IO('G', 7)
/* Stock encodes no payload size but writes one byte to arg. */
#define GF_IOC_PM_FBCABCK _IO('G', 8)
#define GF_IOC_POWER_ON _IO('G', 9)
#define GF_IOC_POWER_OFF _IO('G', 10)

static inline int ple_gf_valid_cmd(unsigned int cmd)
{
	switch (cmd) {
	case GF_IOC_DISABLE_IRQ: case GF_IOC_ENABLE_IRQ: case GF_IOC_SETSPEED:
	case GF_IOC_RESET: case GF_IOC_COOLBOOT: case GF_IOC_SENDKEY:
	case GF_IOC_CLK_READY: case GF_IOC_CLK_UNREADY: case GF_IOC_PM_FBCABCK:
	case GF_IOC_POWER_ON: case GF_IOC_POWER_OFF:
		return 1;
	default:
		return 0;
	}
}

static inline int ple_gf_keycode(__u32 key, __s32 value)
{
	/* EV_KEY: release0, press1, repeat2; reject values outside that ABI. */
	if (value < 0 || value > 2)
		return -EINVAL;
	switch (key) {
	case KEY_POWER:
		return KEY_WAKEUP;
	case KEY_WAKEUP: case KEY_HOMEPAGE: case KEY_MENU: case KEY_BACK:
	case KEY_UP: case KEY_DOWN: case KEY_F9: case KEY_F19:
	case KEY_F1: case KEY_F2: case KEY_F3: case KEY_APPSELECT:
		return key;
	default:
		return -EINVAL;
	}
}
#endif
