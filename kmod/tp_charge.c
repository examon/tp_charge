#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/dmi.h>
#include <linux/rtc.h>
#include <asm/rtc.h>

/* Magic number is CMOS to detect SMAPI */
#define SMAPI_MAGIC 0x5349

static struct platform_device *tp_charge_pdev;
static short smapi_port;
static bool has_smapi;
static DEFINE_MUTEX(smapi_lock);
static DEFINE_MUTEX(state_lock);

struct state_value
{
	bool cached;
	int err;
	u8 value;
};

struct battery_state
{
	struct state_value start_threshold, stop_threshold;
};

struct battery_state batteries[2];

static int detect_smapi(void)
{
	unsigned long flags;
	unsigned short smapi_id;

	spin_lock_irqsave(&rtc_lock, flags);
	smapi_id = CMOS_READ(0x7C) | (CMOS_READ(0x7D) << 8);
	if (smapi_id == SMAPI_MAGIC) {
		has_smapi = true;
		smapi_port = CMOS_READ(0x7E) | (CMOS_READ(0x7F) << 8);
	}
	spin_unlock_irqrestore(&rtc_lock, flags);

	if (!has_smapi)
		return -ENODEV;

	printk(KERN_DEBUG "tp_charge: Found SMAPI at address 0x%04X\n",
	       smapi_port);

	return 0;
}

int smapi_get_charge_threshold(int bat, int start, u8 *val)
{
	int eax, ecx, errcode;

	mutex_lock(&smapi_lock);
	asm volatile ("out %%al, %[smapi_port]\n\t"
		      "out %%al, $0x4F"
		      : "=c" (ecx), "=a" (eax)
		      : [smapi_port] "d" (smapi_port),
		        "a" (0x5380), "b" (start ? 0x2116 : 0x211a),
		        "c" ((bat+1) << 8), "S" (0), "D" (0)
		      : "cc");
	mutex_unlock(&smapi_lock);

	errcode = (eax >> 8) & 0xFF;
	if (errcode == 0xA6)
		return -EAGAIN;
	else if (errcode == 0x53)
		return -ENODEV;
	else if (errcode != 0)
		return -EIO;

	if ((ecx & 0x0100) != 0x0100)
		return -EIO;

	*val = ecx & 0xFF;
	return 0;
}

int smapi_set_charge_threshold(int bat, int start, u8 val)
{
	int eax, ecx, errcode;
	int esi, edi;
	int ret = 0;

	if (val > 100) {
		ret = -EINVAL;
		return ret;
	}

	mutex_lock(&smapi_lock);

	/* First query, but keep ESI and EDI. */
	asm volatile ("out %%al, %[smapi_port]\n\t"
		      "out %%al, $0x4F"
		      : "=c" (ecx), "=a" (eax), "=S" (esi), "=D" (edi)
		      : [smapi_port] "d" (smapi_port),
			"a" (0x5380), "b" (start ? 0x2116 : 0x211a),
			"c" ((bat+1) << 8), "S" (0), "D" (0)
		      : "cc");

	errcode = (eax >> 8) & 0xFF;
	if (errcode == 0xA6)
		ret = -EAGAIN;
	else if (errcode == 0x53)
		ret = -ENODEV;
	else if (errcode != 0)
		ret = -EIO;
	else if ((ecx & 0x0100) != 0x0100)
		ret = -EIO;

	if (ret)
		goto out_unlock;

	/* Now set. */
	asm volatile ("out %%al, %[smapi_port]\n\t"
		      "out %%al, $0x4F"
		      : "=a" (eax)
		      : [smapi_port] "d" (smapi_port),
			"a" (0x5380), "b" (start ? 0x2117 : 0x211b),
			"c" (((bat+1) << 8) | val), "S" (esi), "D" (edi)
		      : "cc");

	/*
	 * Issuing another smapi call within 50 ms can cause this one to
	 * have no effect.
	 */
	msleep(50);

	errcode = (eax >> 8) & 0xFF;
	if (errcode == 0xA6)
		ret = -EAGAIN;
	else if (errcode != 0)
		ret = -EIO;

out_unlock:
	mutex_unlock(&smapi_lock);

	return ret;
}

static void load_state(int bat, int start, int force)
{
	struct state_value *state = start ?
		&batteries[bat].start_threshold :
		&batteries[bat].stop_threshold;

	if (state->cached && !force)
		return;

	state->err = smapi_get_charge_threshold(bat, start, &state->value);
}

static ssize_t tp_charge_show(struct device *dev, struct device_attribute *attr,
			      char *buf);
static ssize_t tp_charge_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count);

DEVICE_ATTR(bat0_start, 0644, tp_charge_show, tp_charge_store);
DEVICE_ATTR(bat0_stop, 0644, tp_charge_show, tp_charge_store);
DEVICE_ATTR(bat1_start, 0644, tp_charge_show, tp_charge_store);
DEVICE_ATTR(bat1_stop, 0644, tp_charge_show, tp_charge_store);

static ssize_t tp_charge_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	int ret;

	/* Which attribute are we? */
	int bat = (attr == &dev_attr_bat1_start || attr == &dev_attr_bat1_stop);
	int start = (attr == &dev_attr_bat0_start ||
		     attr == &dev_attr_bat1_start);
	struct state_value *val = (start ? &batteries[bat].start_threshold :
				   &batteries[bat].stop_threshold);

	mutex_lock(&state_lock);
	load_state(bat, start, 0);
	if (val->err)
		ret = val->err;
	else
		ret = snprintf(buf, PAGE_SIZE, "%d\n", (int)val->value);
	mutex_unlock(&state_lock);

	return ret;
}

static ssize_t tp_charge_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int ret;
	u8 new_val;

	/* Which attribute are we? */
	int bat = (attr == &dev_attr_bat1_start || attr == &dev_attr_bat1_stop);
	int start = (attr == &dev_attr_bat0_start ||
		     attr == &dev_attr_bat1_start);
	struct state_value *val = (start ? &batteries[bat].start_threshold :
				   &batteries[bat].stop_threshold);

	ret = kstrtou8(buf, 0, &new_val);
	if (ret)
		return ret;

	mutex_lock(&state_lock);
	val->cached = false;
	ret = smapi_set_charge_threshold(bat, start, new_val);
	mutex_unlock(&state_lock);

	return (ret < 0 ? ret : count);
}

static struct attribute const * tp_charge_attrs[5];
static int tp_charge_nattrs = 0;

static int tp_charge_probe(struct platform_device *dev)
{
	int err = 0;

	/* Load initial state so we can tell which attributes to create. */
	mutex_lock(&state_lock);
	load_state(0, 0, 1);
	load_state(0, 1, 1);
	load_state(1, 0, 1);
	load_state(1, 1, 1);

	if (batteries[0].start_threshold.err != -ENODEV)
		tp_charge_attrs[tp_charge_nattrs++] = &dev_attr_bat0_start.attr;
	if (batteries[0].stop_threshold.err != -ENODEV)
		tp_charge_attrs[tp_charge_nattrs++] = &dev_attr_bat0_stop.attr;
	if (batteries[1].start_threshold.err != -ENODEV)
		tp_charge_attrs[tp_charge_nattrs++] = &dev_attr_bat1_start.attr;
	if (batteries[1].stop_threshold.err != -ENODEV)
		tp_charge_attrs[tp_charge_nattrs++] = &dev_attr_bat1_stop.attr;
	tp_charge_attrs[tp_charge_nattrs] = 0;
	mutex_unlock(&state_lock);

	err = sysfs_create_files(&dev->dev.kobj, tp_charge_attrs);
	return err;
}

static int tp_charge_remove(struct platform_device *dev)
{
	sysfs_remove_files(&dev->dev.kobj, tp_charge_attrs);
	return 0;
}

static struct resource smapi_resources[] = {
	[0] = {
		.name	= "smapi",
		.flags	= IORESOURCE_IO,
	},
	[1] = {
		.name	= "smapi",
		.start	= 0x4F,
		.end	= 0x4F,
		.flags	= IORESOURCE_IO,
	},
};

static struct platform_driver tp_charge_driver = {
	.driver = {
		.name = "tp_charge",
		.owner = THIS_MODULE,
	},
	.probe = tp_charge_probe,
	.remove = tp_charge_remove,
};

static bool is_real_resource(struct resource *r)
{
	/* XXX: This is gross.  Is there a better way? */
	return r && (r->start > 0 || r->end < 0xff);
}

static int __init tp_charge_init(void)
{
	int err;

	/* Don't even try to load except on ThinkPads. */
	if (!strstr(dmi_get_system_info(DMI_PRODUCT_VERSION), "ThinkPad"))
		return -ENODEV;

	err = detect_smapi();
	if (err)
		goto fail;

	smapi_resources[0].start = smapi_port;
	smapi_resources[0].end = smapi_port;

	tp_charge_pdev = platform_device_register_simple(
		"tp_charge", -1, smapi_resources, ARRAY_SIZE(smapi_resources));
	if (IS_ERR(tp_charge_pdev)) {
		err = PTR_ERR(tp_charge_pdev);
		goto fail;
	}

	/* Paranoia: make sure that we didn't conflict with another driver. */
	if (is_real_resource(tp_charge_pdev->resource[0].parent) ||
	    is_real_resource(tp_charge_pdev->resource[1].parent))
	{
		printk(KERN_ERR "tp_charge: Resource conflict\n");
		err = -EBUSY;
		goto fail;
	}

	err = platform_driver_register(&tp_charge_driver);
	if (err)
		goto fail;

	return 0;

fail:
	if (tp_charge_pdev && !IS_ERR(tp_charge_pdev))
		platform_device_unregister(tp_charge_pdev);
	return err;
}

static void tp_charge_exit(void)
{
	platform_device_unregister(tp_charge_pdev);
	platform_driver_unregister(&tp_charge_driver);
}

module_init(tp_charge_init);
module_exit(tp_charge_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrew Lutomirski");
MODULE_DESCRIPTION("ThinkPad battery charge control");
