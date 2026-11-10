// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PLE619F Goodix platform ABI port from HMD d548dae4af7fa5a75be88ce2e93f95ad4c867777
 * drivers/input/fingerprint/goodix_gfx18m/{gf_spi.c,gf_spi.h,platform.c}.
 * Original interface: Copyright (C) 2006 SWAPP, Andrea Paterniani;
 * Copyright (C) 2007 David Brownell. Goodix author: Jiangtao Yi.
 * Preserve G-ioctl/SIGIO/customer1 semantics, replace old wakelock and unsafe
 * global lifetime/cleanup with a per-device refcount and serialized controls.
 * AP_CONTROL_CLK was disabled in stock: SPI/secure clocks belong to the TEE.
 */
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "gf_abi.h"

struct ple_gf {
	struct miscdevice misc;
	struct kref refs;
	struct mutex lock;
	struct input_dev *input;
	struct regulator *supply;
	struct wakeup_source *wake;
	struct fasync_struct *async;
	struct notifier_block fb_notifier;
	int reset_gpio, irq;
	unsigned int users;
	bool dead, available, irq_enabled;
	u8 fb_black;
};

static const unsigned int ple_gf_keys[] = { KEY_WAKEUP, KEY_HOMEPAGE,
	KEY_MENU, KEY_BACK, KEY_UP, KEY_DOWN, KEY_F9, KEY_F19, KEY_F1,
	KEY_F2, KEY_F3, KEY_APPSELECT };

static void ple_gf_release_keys(struct ple_gf *gf)
{
	unsigned int i;

	/* A decoder restart must not leave a physically released key stuck down. */
	for (i = 0; i < ARRAY_SIZE(ple_gf_keys); i++)
		input_report_key(gf->input, ple_gf_keys[i], 0);
	input_sync(gf->input);
}

static void ple_gf_free(struct kref *refs)
{
	kfree(container_of(refs, struct ple_gf, refs));
}

/* Caller holds lock; IRQ handler never takes this lock. */
static void ple_gf_irq_set(struct ple_gf *gf, bool enabled)
{
	if (gf->irq_enabled == enabled)
		return;
	if (enabled)
		enable_irq(gf->irq);
	else
		disable_irq(gf->irq);
	gf->irq_enabled = enabled;
}

static void ple_gf_reset(struct ple_gf *gf, unsigned int delay_ms)
{
	gpio_set_value_cansleep(gf->reset_gpio, 0);
	msleep(3);
	gpio_set_value_cansleep(gf->reset_gpio, 1);
	msleep(delay_ms);
}

static irqreturn_t ple_gf_irq(int irq, void *data)
{
	struct ple_gf *gf = data;

	if (!READ_ONCE(gf->dead) && READ_ONCE(gf->available)) {
		__pm_wakeup_event(gf->wake, 2000);
		kill_fasync(&gf->async, SIGIO, POLL_IN);
	}
	return IRQ_HANDLED;
}

static int ple_gf_open(struct inode *inode, struct file *file)
{
	struct ple_gf *gf = container_of(file->private_data, struct ple_gf, misc);
	int ret = 0;

	mutex_lock(&gf->lock);
	if (gf->dead) {
		ret = -ENODEV;
		goto out;
	}
	if (!gf->users) {
		/* Stock regulator stays enabled; power ioctls only change availability. */
		ple_gf_reset(gf, 360);
		gf->available = true;
		ple_gf_irq_set(gf, true);
	}
	kref_get(&gf->refs);
	gf->users++;
	file->private_data = gf;
	ret = nonseekable_open(inode, file);
out:
	mutex_unlock(&gf->lock);
	return ret;
}

static int ple_gf_fasync(int fd, struct file *file, int mode)
{
	struct ple_gf *gf = file->private_data;
	int ret;

	mutex_lock(&gf->lock);
	ret = gf->dead && mode ? -ENODEV : fasync_helper(fd, file, mode, &gf->async);
	mutex_unlock(&gf->lock);
	return ret;
}

static int ple_gf_release(struct inode *inode, struct file *file)
{
	struct ple_gf *gf = file->private_data;

	mutex_lock(&gf->lock);
	fasync_helper(-1, file, 0, &gf->async);
	if (!--gf->users && !gf->dead) {
		ple_gf_irq_set(gf, false);
		gf->available = false;
		ple_gf_release_keys(gf);
	}
	mutex_unlock(&gf->lock);
	kref_put(&gf->refs, ple_gf_free);
	return 0;
}

static long ple_gf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ple_gf *gf = file->private_data;
	struct gf_key key;
	void __user *ptr = (void __user *)arg;
	int ret = 0, code;

	if (!ple_gf_valid_cmd(cmd))
		return -ENOTTY;
	mutex_lock(&gf->lock);
	if (gf->dead || (!gf->available && cmd != GF_IOC_POWER_ON &&
			cmd != GF_IOC_POWER_OFF)) {
		ret = -ENODEV;
		goto out;
	}
	switch (cmd) {
	case GF_IOC_DISABLE_IRQ:
		ple_gf_irq_set(gf, false);
		break;
	case GF_IOC_ENABLE_IRQ:
		ple_gf_irq_set(gf, true);
		break;
	case GF_IOC_RESET:
		ple_gf_reset(gf, 70);
		break;
	case GF_IOC_COOLBOOT:
		/* Exact stock power_off/on are no-ops, with a5ms interval. */
		msleep(5);
		break;
	case GF_IOC_SENDKEY:
		if (copy_from_user(&key, ptr, sizeof(key))) {
			ret = -EFAULT;
			break;
		}
		code = ple_gf_keycode(key.key, key.value);
		if (code < 0) {
			ret = code;
			break;
		}
		input_report_key(gf->input, code, key.value);
		input_sync(gf->input);
		break;
	case GF_IOC_PM_FBCABCK:
		ret = put_user(gf->fb_black, (u8 __user *)ptr);
		break;
	case GF_IOC_POWER_ON:
		gf->available = true;
		break;
	case GF_IOC_POWER_OFF:
		gf->available = false;
		ple_gf_release_keys(gf);
		break;
	case GF_IOC_SETSPEED: case GF_IOC_CLK_READY: case GF_IOC_CLK_UNREADY:
		/* Exact stock platform configuration: AP_CONTROL_CLK is not defined. */
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&gf->lock);
	return ret;
}

#ifdef CONFIG_COMPAT
static long ple_gf_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return ple_gf_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations ple_gf_fops = {
	.owner = THIS_MODULE, .open = ple_gf_open, .release = ple_gf_release,
	.fasync = ple_gf_fasync, .unlocked_ioctl = ple_gf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ple_gf_compat_ioctl,
#endif
	.llseek = no_llseek,
};

static int ple_gf_fb(struct notifier_block *notifier, unsigned long event, void *data)
{
	struct ple_gf *gf = container_of(notifier, struct ple_gf, fb_notifier);
	struct fb_event *fb = data;
	int blank;

	if (event != FB_EARLY_EVENT_BLANK || !fb || !fb->data)
		return NOTIFY_DONE;
	blank = *(int *)fb->data;
	if (blank != FB_BLANK_POWERDOWN && blank != FB_BLANK_UNBLANK)
		return NOTIFY_DONE;
	mutex_lock(&gf->lock);
	if (!gf->dead && gf->available) {
		gf->fb_black = blank == FB_BLANK_POWERDOWN;
		kill_fasync(&gf->async, SIGIO, POLL_IN);
	}
	mutex_unlock(&gf->lock);
	return NOTIFY_OK;
}

static int ple_gf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ple_gf *gf;
	struct pinctrl *pins;
	int irq_gpio, ret;
	unsigned int i;

	/* This board port is intentionally not a generic Goodix replacement. */
	if (!strstr(saved_command_line, "androidboot.fp=goodix") ||
	    strstr(saved_command_line, "androidboot.mode=charger"))
		return -ENODEV;
	gf = kzalloc(sizeof(*gf), GFP_KERNEL);
	if (!gf)
		return -ENOMEM;
	kref_init(&gf->refs);
	mutex_init(&gf->lock);
	pins = devm_pinctrl_get_select_default(dev);
	if (IS_ERR(pins)) { ret = PTR_ERR(pins); goto fail; }
	gf->reset_gpio = of_get_named_gpio(dev->of_node, "goodix,gpio_reset", 0);
	irq_gpio = of_get_named_gpio(dev->of_node, "goodix,gpio_irq", 0);
	if (gf->reset_gpio < 0) { ret = gf->reset_gpio; goto fail; }
	if (irq_gpio < 0) { ret = irq_gpio; goto fail; }
	ret = devm_gpio_request_one(dev, gf->reset_gpio, GPIOF_OUT_INIT_HIGH, "goodix_reset");
	if (ret) goto fail;
	ret = devm_gpio_request_one(dev, irq_gpio, GPIOF_IN, "goodix_irq");
	if (ret) goto fail;
	gf->supply = devm_regulator_get(dev, "vdd_io");
	if (IS_ERR(gf->supply)) { ret = PTR_ERR(gf->supply); goto fail; }
	ret = regulator_enable(gf->supply);
	if (ret) goto fail;
	gf->input = devm_input_allocate_device(dev);
	if (!gf->input) { ret = -ENOMEM; goto supply; }
	gf->input->name = "goodix_fp";
	gf->input->id.bustype = BUS_HOST;
	for (i = 0; i < ARRAY_SIZE(ple_gf_keys); i++)
		input_set_capability(gf->input, EV_KEY, ple_gf_keys[i]);
	ret = input_register_device(gf->input);
	if (ret) goto supply;
	gf->wake = wakeup_source_register(dev, "gf_ttw_wl");
	if (!gf->wake) { ret = -ENOMEM; goto supply; }
	gf->irq = gpio_to_irq(irq_gpio);
	if (gf->irq < 0) { ret = gf->irq; goto wake; }
	ret = request_threaded_irq(gf->irq, NULL, ple_gf_irq,
		IRQF_TRIGGER_RISING | IRQF_ONESHOT, "goodix_fp", gf);
	if (ret) goto wake;
	gf->irq_enabled = true;
	ple_gf_irq_set(gf, false);
	ret = enable_irq_wake(gf->irq);
	if (ret) goto irq;
	gf->fb_notifier.notifier_call = ple_gf_fb;
	ret = fb_register_client(&gf->fb_notifier);
	if (ret) goto irq_wake;
	gf->misc.minor = MISC_DYNAMIC_MINOR;
	gf->misc.name = "goodix_fp";
	gf->misc.fops = &ple_gf_fops;
	gf->misc.parent = dev;
	ret = misc_register(&gf->misc);
	if (ret) goto fb;
	platform_set_drvdata(pdev, gf);
	dev_info(dev, "PLE619F Goodix G-ioctl/SIGIO navigation ready\n");
	return 0;
fb:
	fb_unregister_client(&gf->fb_notifier);
irq_wake:
	disable_irq_wake(gf->irq);
irq:
	free_irq(gf->irq, gf);
wake:
	wakeup_source_unregister(gf->wake);
supply:
	regulator_disable(gf->supply);
fail:
	kref_put(&gf->refs, ple_gf_free);
	return ret;
}

static int ple_gf_remove(struct platform_device *pdev)
{
	struct ple_gf *gf = platform_get_drvdata(pdev);

	mutex_lock(&gf->lock);
	gf->dead = true;
	gf->available = false;
	ple_gf_irq_set(gf, false);
	ple_gf_release_keys(gf);
	mutex_unlock(&gf->lock);
	/* misc_deregister synchronizes in-flight open before dropping driver ref. */
	misc_deregister(&gf->misc);
	fb_unregister_client(&gf->fb_notifier);
	disable_irq_wake(gf->irq);
	free_irq(gf->irq, gf);
	wakeup_source_unregister(gf->wake);
	gpio_set_value_cansleep(gf->reset_gpio, 0);
	regulator_disable(gf->supply);
	kref_put(&gf->refs, ple_gf_free);
	return 0;
}

static const struct of_device_id ple_gf_match[] = {
	{ .compatible = "nokia,ple-goodix" }, { }
};
MODULE_DEVICE_TABLE(of, ple_gf_match);
static struct platform_driver ple_gf_driver = {
	.probe = ple_gf_probe, .remove = ple_gf_remove,
	.driver = { .name = "ple-goodix", .of_match_table = ple_gf_match },
};
module_platform_driver(ple_gf_driver);
MODULE_DESCRIPTION("Nokia PLE619F stock Goodix TEE navigation interface");
MODULE_LICENSE("GPL");
