/* SPDX-License-Identifier: GPL-2.0-only */
/* PLE619F HMD d548dae4af7fa5a75be88ce2e93f95ad4c867777,
 * msm8952.c enable_ear_switch and msm8x16-wcd.c DAPM event ordering.
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */
#ifndef _PLE_EAR_SWITCH_H
#define _PLE_EAR_SWITCH_H

/* Call with the devm-owned exact PLE GPIO descriptors. Unexpected DAPM
 * phases must not toggle hardware; missing either descriptor fails closed.
 */
static inline int ple_ear_switch_set(struct gpio_desc *en,
				   struct gpio_desc *in, int event)
{
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		if (!en || !in)
			return -ENODEV;
		gpiod_set_value_cansleep(en, 1);
		gpiod_set_value_cansleep(in, 1);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		if (!en || !in)
			return -ENODEV;
		gpiod_set_value_cansleep(in, 0);
		gpiod_set_value_cansleep(en, 0);
		break;
	}
	return 0;
}
#endif
