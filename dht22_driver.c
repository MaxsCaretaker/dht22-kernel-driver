// SPDX-License-Identifier: GPL-2.0
/*
 * dht22_driver.c  –  DHT22 temperature/humidity sensor driver
 *
 * Target : Raspberry Pi 4, kernel 6.12.x (64-bit, aarch64)
 * GPIO   : GPIO4 (physical pin 7)  ← data line
 * Wiring : DHT22-VCC → 3.3 V (pin 1)
 *          DHT22-DATA → GPIO4 (pin 7) + 4.7 kΩ pull-up to 3.3 V
 *          DHT22-GND  → GND  (pin 9)
 *
 * GPIO lookup strategy (no Device Tree overlay needed)
 * ─────────────────────────────────────────────────────
 * Kernel 6.12 deprecates the global GPIO number space. Instead we register
 * a gpiod_lookup_table that maps our platform device's "data" consumer
 * to line 4 on the BCM2711 GPIO controller ("pinctrl-bcm2835").
 * devm_gpiod_get() then resolves it cleanly through the descriptor API.
 *
 * DHT22 protocol summary
 * ───────────────────────
 *  1. Host pulls data line LOW ≥ 1 ms  (start signal)
 *  2. Host releases; pull-up takes it HIGH
 *  3. Sensor ACK: 80 µs LOW then 80 µs HIGH
 *  4. 40 bits:  50 µs LOW + 26 µs HIGH = 0
 *               50 µs LOW + 70 µs HIGH = 1
 *  5. 40 bits = 16-bit RH + 16-bit Temp + 8-bit checksum
 *
 * /dev/dht22 read() returns:
 *   "<temp_×10> <rh_×10> <ktime_ns>\n"
 *   e.g.  "234 612 1716400000123456789\n"  → 23.4 °C, 61.2 %RH
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>

/* ── Module metadata ─────────────────────────────────────────────────── */
#define DRIVER_NAME   "dht22"
#define DEVICE_NAME   "dht22"
#define CLASS_NAME    "dht22_class"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pi Project");
MODULE_DESCRIPTION("DHT22 temperature/humidity sensor driver for Raspberry Pi 4");
MODULE_VERSION("1.1");

/* ── Protocol constants (µs) ─────────────────────────────────────────── */
#define DHT_START_LOW_US     1200
#define DHT_BIT_THRESHOLD_US   50   /* HIGH pulse > this → bit 1           */
#define DHT_TIMEOUT_US        200   /* max wait for any single edge         */
#define DHT_BITS               40
#define DHT_MIN_INTERVAL_MS  2000   /* sensor needs 2 s between readings   */

/* ── GPIO lookup table (kernel 6.12 descriptor API, no DT needed) ────── */
static struct gpiod_lookup_table dht22_gpio_lookup = {
    .dev_id = DRIVER_NAME,           /* matches our platform_device name   */
    .table  = {
        /*            chip label          line  con_id  idx  flags         */
        GPIO_LOOKUP("pinctrl-bcm2711",    4,   "data",       GPIO_ACTIVE_HIGH),
        { }
    },
};

/* ── Reading cache ───────────────────────────────────────────────────── */
struct dht22_reading {
    s32     temp_x10;
    u32     rh_x10;
    ktime_t timestamp;
    bool    valid;
};

/* ── Per-device state ────────────────────────────────────────────────── */
struct dht22_dev {
    struct gpio_desc    *gpiod;
    struct cdev          cdev;
    dev_t                devno;
    struct device       *device;
    struct mutex         lock;
    struct dht22_reading cache;
};

static struct dht22_dev *g_dev;
static struct class     *g_class;

/* ── Low-level timing helper ─────────────────────────────────────────── */
static int wait_for_level(struct gpio_desc *gpiod, int level, int timeout_us)
{
    ktime_t deadline = ktime_add_us(ktime_get(), timeout_us);

    while (gpiod_get_value(gpiod) != level) {
        if (ktime_after(ktime_get(), deadline))
            return -ETIMEDOUT;
    }
    return (int)ktime_us_delta(ktime_get(), deadline) + timeout_us;
}

/* ── Full DHT22 transaction ──────────────────────────────────────────── */
static int dht22_read_raw(struct dht22_dev *d, struct dht22_reading *out)
{
    struct gpio_desc *g = d->gpiod;
    unsigned long flags;
    u8  data[5] = {0};
    int i, ret, pulse_us;

    /* 1. Start signal: drive LOW for 1.2 ms */
    gpiod_direction_output(g, 0);
    usleep_range(DHT_START_LOW_US, DHT_START_LOW_US + 200);

    local_irq_save(flags);

    /* Release → input; pull-up takes line HIGH */
    gpiod_direction_input(g);

    /* 2. Wait for sensor ACK */
    ret = wait_for_level(g, 0, DHT_TIMEOUT_US);   /* falling edge */
    if (ret < 0) goto timeout;
    ret = wait_for_level(g, 1, DHT_TIMEOUT_US);   /* rising  edge */
    if (ret < 0) goto timeout;
    ret = wait_for_level(g, 0, DHT_TIMEOUT_US);   /* falling edge */
    if (ret < 0) goto timeout;

    /* 3. Read 40 bits */
    for (i = 0; i < DHT_BITS; i++) {
        ret = wait_for_level(g, 1, DHT_TIMEOUT_US);   /* end of 50µs low */
        if (ret < 0) goto timeout;

        pulse_us = wait_for_level(g, 0, DHT_TIMEOUT_US);  /* measure high */
        if (pulse_us < 0) goto timeout;

        data[i / 8] <<= 1;
        if (pulse_us > DHT_BIT_THRESHOLD_US)
            data[i / 8] |= 1;
    }

    local_irq_restore(flags);

    /* 4. Checksum */
    if (((data[0] + data[1] + data[2] + data[3]) & 0xFF) != data[4]) {
        pr_debug(DRIVER_NAME ": checksum fail\n");
        return -EBADMSG;
    }

    /* 5. Parse */
    out->rh_x10   = ((u32)data[0] << 8) | data[1];
    out->temp_x10 = ((s32)(data[2] & 0x7F) << 8) | data[3];
    if (data[2] & 0x80)
        out->temp_x10 = -out->temp_x10;
    out->timestamp = ktime_get();
    out->valid     = true;

    pr_debug(DRIVER_NAME ": T=%d.%d°C RH=%d.%d%%\n",
             out->temp_x10 / 10, abs(out->temp_x10 % 10),
             out->rh_x10   / 10,     out->rh_x10   % 10);
    return 0;

timeout:
    local_irq_restore(flags);
    pr_debug(DRIVER_NAME ": timeout at bit %d\n", i);
    return -ETIMEDOUT;
}

/* ── File operations ─────────────────────────────────────────────────── */
static int dht22_open(struct inode *inode, struct file *filp)
{
    filp->private_data = g_dev;
    return 0;
}

static int dht22_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t dht22_read(struct file *filp, char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    struct dht22_dev    *d = filp->private_data;
    struct dht22_reading reading;
    char   kbuf[64];
    ssize_t len;
    s64 age_ms;
    int ret, attempts;

    if (*ppos > 0)
        return 0;

    if (mutex_lock_interruptible(&d->lock))
        return -ERESTARTSYS;

    age_ms = d->cache.valid
           ? ktime_ms_delta(ktime_get(), d->cache.timestamp)
           : LLONG_MAX;

    if (age_ms < DHT_MIN_INTERVAL_MS && d->cache.valid) {
        reading = d->cache;
        goto format;
    }

    ret = -ETIMEDOUT;
    for (attempts = 0; attempts < 3 && ret != 0; attempts++) {
        if (attempts > 0)
            msleep(100);
        ret = dht22_read_raw(d, &reading);
    }

    if (ret == 0) {
        d->cache = reading;
    } else if (d->cache.valid) {
        pr_warn_ratelimited(DRIVER_NAME ": read failed, returning stale cache\n");
        reading = d->cache;
    } else {
        mutex_unlock(&d->lock);
        return ret;
    }

format:
    mutex_unlock(&d->lock);

    len = scnprintf(kbuf, sizeof(kbuf), "%d %u %lld\n",
                    reading.temp_x10,
                    reading.rh_x10,
                    (long long)ktime_to_ns(reading.timestamp));

    if (len > count)
        return -EINVAL;
    if (copy_to_user(ubuf, kbuf, len))
        return -EFAULT;

    *ppos += len;
    return len;
}

static const struct file_operations dht22_fops = {
    .owner   = THIS_MODULE,
    .open    = dht22_open,
    .release = dht22_release,
    .read    = dht22_read,
};

/* ── Platform driver probe / remove ─────────────────────────────────── */
static int dht22_probe(struct platform_device *pdev)
{
    struct dht22_dev *d;
    int ret;

    d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
    if (!d)
        return -ENOMEM;

    mutex_init(&d->lock);

    /*
     * Try DT first (overlay installed), then fall through to the lookup
     * table registered at module init time.
     */
    d->gpiod = devm_gpiod_get(&pdev->dev, "data", GPIOD_IN);
    if (IS_ERR(d->gpiod)) {
        dev_err(&pdev->dev, "cannot get GPIO4: %ld\n", PTR_ERR(d->gpiod));
        return PTR_ERR(d->gpiod);
    }

    /* ── Character device ── */
    ret = alloc_chrdev_region(&d->devno, 0, 1, DEVICE_NAME);
    if (ret)
        return ret;

    cdev_init(&d->cdev, &dht22_fops);
    d->cdev.owner = THIS_MODULE;
    ret = cdev_add(&d->cdev, d->devno, 1);
    if (ret)
        goto err_unreg;

    g_class = class_create(CLASS_NAME);
    if (IS_ERR(g_class)) {
        ret = PTR_ERR(g_class);
        goto err_cdev;
    }

    d->device = device_create(g_class, &pdev->dev, d->devno, NULL, DEVICE_NAME);
    if (IS_ERR(d->device)) {
        ret = PTR_ERR(d->device);
        goto err_class;
    }

    platform_set_drvdata(pdev, d);
    g_dev = d;

    dev_info(&pdev->dev,
             "DHT22 ready → /dev/%s  (major=%d, GPIO4/pin7)\n",
             DEVICE_NAME, MAJOR(d->devno));
    return 0;

err_class: class_destroy(g_class);
err_cdev:  cdev_del(&d->cdev);
err_unreg: unregister_chrdev_region(d->devno, 1);
    return ret;
}

static void dht22_remove(struct platform_device *pdev)
{
    struct dht22_dev *d = platform_get_drvdata(pdev);
    device_destroy(g_class, d->devno);
    class_destroy(g_class);
    cdev_del(&d->cdev);
    unregister_chrdev_region(d->devno, 1);
    pr_info(DRIVER_NAME ": removed\n");
}

/* ── Device Tree match ───────────────────────────────────────────────── */
static const struct of_device_id dht22_of_match[] = {
    { .compatible = "custom,dht22" },
    { }
};
MODULE_DEVICE_TABLE(of, dht22_of_match);

static struct platform_driver dht22_platform_driver = {
    .probe  = dht22_probe,
    .remove = dht22_remove,
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = dht22_of_match,
        .owner          = THIS_MODULE,
    },
};

/* ── Module init/exit ────────────────────────────────────────────────── */
static struct platform_device *dht22_pdev;

static int __init dht22_init(void)
{
    int ret;

    /* Register lookup table BEFORE the platform device probes */
    gpiod_add_lookup_table(&dht22_gpio_lookup);

    ret = platform_driver_register(&dht22_platform_driver);
    if (ret)
        goto err_lookup;

    dht22_pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
    if (IS_ERR(dht22_pdev)) {
        ret = PTR_ERR(dht22_pdev);
        dht22_pdev = NULL;
        goto err_driver;
    }
    return 0;

err_driver:
    platform_driver_unregister(&dht22_platform_driver);
err_lookup:
    gpiod_remove_lookup_table(&dht22_gpio_lookup);
    return ret;
}

static void __exit dht22_exit(void)
{
    if (dht22_pdev)
        platform_device_unregister(dht22_pdev);
    platform_driver_unregister(&dht22_platform_driver);
    gpiod_remove_lookup_table(&dht22_gpio_lookup);
}

module_init(dht22_init);
module_exit(dht22_exit);
