#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/workqueue.h>
#include <linux/fb.h>
#include <linux/mutex.h>
#include <linux/input/ili210x.h>

#define MAX_TOUCHES		2
#define ILI2121X_MAX_TOUCHES	10
#define ILI2121X_DATA_SIZE	53
#define ILI2121X_X_MAX		(1080 + 2)
#define ILI2121X_Y_MAX		(1920 + 2)
#define DEFAULT_POLL_PERIOD	20

/* Touchscreen commands */
#define REG_TOUCHDATA		0x10
#define REG_PANEL_INFO		0x20
#define REG_FIRMWARE_VERSION	0x40
#define REG_CALIBRATE		0xcc

struct finger {
	u8 x_low;
	u8 x_high;
	u8 y_low;
	u8 y_high;
} __packed;

struct touchdata {
	u8 status;
	struct finger finger[MAX_TOUCHES];
} __packed;

struct panel_info {
	struct finger finger_max;
	u8 xchannel_num;
	u8 ychannel_num;
} __packed;

struct firmware_version {
	u8 id;
	u8 major;
	u8 minor;
} __packed;

struct ili210x {
	struct i2c_client *client;
	struct input_dev *input;
	bool (*get_pendown_state)(void);
	bool is_ili2121x;
	unsigned int poll_period;
	struct delayed_work dwork;
	struct notifier_block fb_notifier;
	struct mutex pm_lock;
	bool display_suspended;
	bool system_suspended;
	bool suspended;
	bool fb_early_changed;
};

static int ili210x_read_reg(struct i2c_client *client, u8 reg, void *buf,
			    size_t len)
{
	struct i2c_msg msg[2] = {
		{
			.addr	= client->addr,
			.flags	= 0,
			.len	= 1,
			.buf	= &reg,
		},
		{
			.addr	= client->addr,
			.flags	= I2C_M_RD,
			.len	= len,
			.buf	= buf,
		}
	};

	if (i2c_transfer(client->adapter, msg, 2) != 2) {
		dev_err(&client->dev, "i2c transfer failed\n");
		return -EIO;
	}

	return 0;
}

static void ili210x_report_events(struct input_dev *input,
				  const struct touchdata *touchdata)
{
	int i;
	bool touch;
	unsigned int x, y;
	const struct finger *finger;

	for (i = 0; i < MAX_TOUCHES; i++) {
		input_mt_slot(input, i);

		finger = &touchdata->finger[i];

		touch = touchdata->status & (1 << i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, touch);
		if (touch) {
			x = finger->x_low | (finger->x_high << 8);
			y = finger->y_low | (finger->y_high << 8);

			input_report_abs(input, ABS_MT_POSITION_X, x);
			input_report_abs(input, ABS_MT_POSITION_Y, y);
		}
	}

	input_mt_report_pointer_emulation(input, false);
	input_sync(input);
}

static void ili2121x_report_events(struct input_dev *input, const u8 *data)
{
	int i;
	bool release_all = data[0] == 0;

	/* HMD reserves this packet marker for gesture/diagnostic reports. */
	if (data[1] == 0x5f)
		return;

	/*
	 * D1C uses Ilitek protocol 3.x: byte 0 is the active-contact count and
	 * ten fixed five-byte records begin at byte 3. This is the exact input
	 * subset used by HMD Android 9; updater, gestures and diagnostics are
	 * deliberately outside the recovery-critical port.
	 */
	for (i = 0; i < ILI2121X_MAX_TOUCHES; i++) {
		unsigned int offset = i * 5 + 3;
		bool active = !release_all && (data[offset] & BIT(7));

		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, active);
		if (active) {
			unsigned int x = ((data[offset] & 0x3f) << 8) |
					 data[offset + 1];
			unsigned int y = (data[offset + 2] << 8) |
					 data[offset + 3];

			input_report_abs(input, ABS_MT_POSITION_X, x);
			input_report_abs(input, ABS_MT_POSITION_Y, y);
			input_report_abs(input, ABS_MT_TOUCH_MAJOR, 1);
		}
	}

	input_mt_report_pointer_emulation(input, true);
	input_sync(input);
}

static bool get_pendown_state(const struct ili210x *priv)
{
	bool state = false;

	if (priv->get_pendown_state)
		state = priv->get_pendown_state();

	return state;
}

/* pm_lock serializes FB and system sleep; the worker never takes it. */
static void ili2121x_update_power_state(struct ili210x *priv)
{
	bool suspended = priv->display_suspended || priv->system_suspended;
	const u8 release[ILI2121X_DATA_SIZE] = { 0 };

	if (priv->suspended == suspended)
		return;

	WRITE_ONCE(priv->suspended, suspended);
	if (suspended) {
		/* HMD powers/resets touch with the panel: drain BEFORE blank. */
		disable_irq(priv->client->irq);
		cancel_delayed_work_sync(&priv->dwork);
		ili2121x_report_events(priv->input, release);
	} else {
		/* Successful FB unblank has already completed panel power/reset. */
		enable_irq(priv->client->irq);
	}
}

static int ili2121x_fb_notifier(struct notifier_block *notifier,
			       unsigned long event, void *data)
{
	struct ili210x *priv = container_of(notifier, struct ili210x, fb_notifier);
	struct fb_event *fb_event = data;
	int blank;

	if (!fb_event || !fb_event->info || fb_event->info->node != 0 ||
	    !fb_event->data)
		return NOTIFY_DONE;

	blank = *(int *)fb_event->data;
	mutex_lock(&priv->pm_lock);
	if (event == FB_EARLY_EVENT_BLANK && blank != FB_BLANK_UNBLANK) {
		priv->fb_early_changed = !priv->display_suspended;
		priv->display_suspended = true;
	} else if (event == FB_R_EARLY_EVENT_BLANK &&
		   blank != FB_BLANK_UNBLANK) {
		/* Undo only OUR change if physical blank failed. */
		if (priv->fb_early_changed)
			priv->display_suspended = false;
		priv->fb_early_changed = false;
	} else if (event == FB_EVENT_BLANK) {
		if (blank == FB_BLANK_UNBLANK)
			priv->display_suspended = false;
		priv->fb_early_changed = false;
	} else {
		mutex_unlock(&priv->pm_lock);
		return NOTIFY_DONE;
	}
	ili2121x_update_power_state(priv);
	mutex_unlock(&priv->pm_lock);
	return NOTIFY_OK;
}

static void ili210x_work(struct work_struct *work)
{
	struct ili210x *priv = container_of(work, struct ili210x,
					    dwork.work);
	struct i2c_client *client = priv->client;
	union {
		struct touchdata legacy;
		u8 ili2121x[ILI2121X_DATA_SIZE];
	} touchdata;
	int error;

	if (priv->is_ili2121x && READ_ONCE(priv->suspended))
		return;

	if (priv->is_ili2121x) {
		error = ili210x_read_reg(client, REG_TOUCHDATA,
					 touchdata.ili2121x,
					 sizeof(touchdata.ili2121x));
		if (error) {
			dev_err(&client->dev,
				"Unable to get ILI2121X touchdata, err = %d\n",
				error);
			return;
		}

		ili2121x_report_events(priv->input, touchdata.ili2121x);
		return;
	}

	error = ili210x_read_reg(client, REG_TOUCHDATA,
				 &touchdata.legacy, sizeof(touchdata.legacy));
	if (error) {
		dev_err(&client->dev,
			"Unable to get touchdata, err = %d\n", error);
		return;
	}

	ili210x_report_events(priv->input, &touchdata.legacy);

	if ((touchdata.legacy.status & 0xf3) || get_pendown_state(priv))
		schedule_delayed_work(&priv->dwork,
				      msecs_to_jiffies(priv->poll_period));
}

static irqreturn_t ili210x_irq(int irq, void *irq_data)
{
	struct ili210x *priv = irq_data;

	if (priv->is_ili2121x && READ_ONCE(priv->suspended))
		return IRQ_HANDLED;

	schedule_delayed_work(&priv->dwork, 0);

	return IRQ_HANDLED;
}

static ssize_t ili210x_calibrate(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ili210x *priv = i2c_get_clientdata(client);
	unsigned long calibrate;
	int rc;
	u8 cmd = REG_CALIBRATE;

	if (kstrtoul(buf, 10, &calibrate))
		return -EINVAL;

	if (calibrate > 1)
		return -EINVAL;

	if (calibrate) {
		rc = i2c_master_send(priv->client, &cmd, sizeof(cmd));
		if (rc != sizeof(cmd))
			return -EIO;
	}

	return count;
}
static DEVICE_ATTR(calibrate, S_IWUSR, NULL, ili210x_calibrate);

static struct attribute *ili210x_attributes[] = {
	&dev_attr_calibrate.attr,
	NULL,
};

static const struct attribute_group ili210x_attr_group = {
	.attrs = ili210x_attributes,
};

static int ili210x_i2c_probe(struct i2c_client *client,
				       const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	const struct ili210x_platform_data *pdata = dev_get_platdata(dev);
	struct ili210x *priv;
	struct input_dev *input;
	struct panel_info panel;
	struct firmware_version firmware;
	bool is_ili2121x;
	int xmax, ymax;
	int error;

	dev_dbg(dev, "Probing for ILI210X I2C Touschreen driver");

	is_ili2121x = client->dev.of_node &&
		of_device_is_compatible(client->dev.of_node,
					"ilitech,ili2121x");

	if (!pdata && !is_ili2121x) {
		dev_err(dev, "No platform data!\n");
		return -EINVAL;
	}

	if (client->irq <= 0) {
		dev_err(dev, "No IRQ!\n");
		return -EINVAL;
	}

	if (is_ili2121x) {
		xmax = ILI2121X_X_MAX;
		ymax = ILI2121X_Y_MAX;
	} else {
		/* Get firmware version. */
		error = ili210x_read_reg(client, REG_FIRMWARE_VERSION,
					 &firmware, sizeof(firmware));
		if (error) {
			dev_err(dev,
				"Failed to get firmware version, err: %d\n",
				error);
			return error;
		}

		/* Get legacy panel geometry. */
		error = ili210x_read_reg(client, REG_PANEL_INFO, &panel,
					 sizeof(panel));
		if (error) {
			dev_err(dev,
				"Failed to get panel information, err: %d\n",
				error);
			return error;
		}

		xmax = panel.finger_max.x_low |
			(panel.finger_max.x_high << 8);
		ymax = panel.finger_max.y_low |
			(panel.finger_max.y_high << 8);
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	input = input_allocate_device();
	if (!priv || !input) {
		error = -ENOMEM;
		goto err_free_mem;
	}

	priv->client = client;
	priv->input = input;
	priv->get_pendown_state = pdata ? pdata->get_pendown_state : NULL;
	priv->is_ili2121x = is_ili2121x;
	mutex_init(&priv->pm_lock);
	priv->poll_period = pdata && pdata->poll_period ?
		pdata->poll_period : DEFAULT_POLL_PERIOD;
	INIT_DELAYED_WORK(&priv->dwork, ili210x_work);

	/* Setup input device */
	input->name = is_ili2121x ?
		"ILI2121X Touchscreen" : "ILI210x Touchscreen";
	input->id.bustype = BUS_I2C;
	input->dev.parent = dev;

	__set_bit(EV_SYN, input->evbit);
	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_ABS, input->evbit);
	__set_bit(BTN_TOUCH, input->keybit);

	/* Single touch */
	input_set_abs_params(input, ABS_X, 0, xmax, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, ymax, 0, 0);

	/* Multi touch */
	input_mt_init_slots(input,
			    is_ili2121x ? ILI2121X_MAX_TOUCHES : MAX_TOUCHES,
			    is_ili2121x ? INPUT_MT_DIRECT : 0);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, xmax, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ymax, 0, 0);
	if (is_ili2121x)
		input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	i2c_set_clientdata(client, priv);

	error = request_irq(client->irq, ili210x_irq,
			    is_ili2121x ? IRQF_TRIGGER_FALLING : pdata->irq_flags,
			    client->name, priv);
	if (error) {
		dev_err(dev, "Unable to request touchscreen IRQ, err: %d\n",
			error);
		goto err_free_mem;
	}

	error = sysfs_create_group(&dev->kobj, &ili210x_attr_group);
	if (error) {
		dev_err(dev, "Unable to create sysfs attributes, err: %d\n",
			error);
		goto err_free_irq;
	}

	error = input_register_device(priv->input);
	if (error) {
		dev_err(dev, "Cannot register input device, err: %d\n", error);
		goto err_remove_sysfs;
	}

	if (is_ili2121x) {
		priv->fb_notifier.notifier_call = ili2121x_fb_notifier;
		error = fb_register_client(&priv->fb_notifier);
		if (error) {
			dev_err(dev, "Unable to register display notifier: %d\n", error);
			goto err_unregister_input;
		}
	}

	/* PLE touch is powered off with the panel; no gesture wake is ported. */
	device_init_wakeup(dev, !is_ili2121x);

	if (is_ili2121x)
		dev_info(dev, "ILI2121X initialized (IRQ: %d)\n", client->irq);
	else
		dev_dbg(dev,
			"ILI210x initialized (IRQ: %d), firmware version %d.%d.%d",
			client->irq, firmware.id, firmware.major,
			firmware.minor);

	return 0;

err_unregister_input:
	sysfs_remove_group(&dev->kobj, &ili210x_attr_group);
	free_irq(client->irq, priv);
	cancel_delayed_work_sync(&priv->dwork);
	input_unregister_device(input);
	kfree(priv);
	return error;
err_remove_sysfs:
	sysfs_remove_group(&dev->kobj, &ili210x_attr_group);
err_free_irq:
	free_irq(client->irq, priv);
	cancel_delayed_work_sync(&priv->dwork);
err_free_mem:
	input_free_device(input);
	kfree(priv);
	return error;
}

static int ili210x_i2c_remove(struct i2c_client *client)
{
	struct ili210x *priv = i2c_get_clientdata(client);

	if (priv->is_ili2121x)
		fb_unregister_client(&priv->fb_notifier);

	sysfs_remove_group(&client->dev.kobj, &ili210x_attr_group);
	free_irq(priv->client->irq, priv);
	cancel_delayed_work_sync(&priv->dwork);
	input_unregister_device(priv->input);
	kfree(priv);

	return 0;
}

static int __maybe_unused ili210x_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ili210x *priv = i2c_get_clientdata(client);

	if (priv->is_ili2121x) {
		mutex_lock(&priv->pm_lock);
		priv->system_suspended = true;
		ili2121x_update_power_state(priv);
		mutex_unlock(&priv->pm_lock);
		return 0;
	}

	if (device_may_wakeup(&client->dev))
		enable_irq_wake(client->irq);

	return 0;
}

static int __maybe_unused ili210x_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ili210x *priv = i2c_get_clientdata(client);

	if (priv->is_ili2121x) {
		mutex_lock(&priv->pm_lock);
		priv->system_suspended = false;
		ili2121x_update_power_state(priv);
		mutex_unlock(&priv->pm_lock);
		return 0;
	}

	if (device_may_wakeup(&client->dev))
		disable_irq_wake(client->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(ili210x_i2c_pm,
			 ili210x_i2c_suspend, ili210x_i2c_resume);

static const struct i2c_device_id ili210x_i2c_id[] = {
	{ "ili210x", 0 },
	{ "ili2121x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ili210x_i2c_id);

static const struct of_device_id ili210x_of_match[] = {
	{ .compatible = "ilitek,ili2121x" },
	{ }
};
MODULE_DEVICE_TABLE(of, ili210x_of_match);

static struct i2c_driver ili210x_ts_driver = {
	.driver = {
		.name = "ili210x_i2c",
		.pm = &ili210x_i2c_pm,
		.of_match_table = of_match_ptr(ili210x_of_match),
	},
	.id_table = ili210x_i2c_id,
	.probe = ili210x_i2c_probe,
	.remove = ili210x_i2c_remove,
};

module_i2c_driver(ili210x_ts_driver);

MODULE_AUTHOR("Olivier Sobrie <olivier@sobrie.be>");
MODULE_DESCRIPTION("ILI210X I2C Touchscreen Driver");
MODULE_LICENSE("GPL");
