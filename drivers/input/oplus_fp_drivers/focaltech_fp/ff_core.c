// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

/*****************************************************************************
* Included header files
*****************************************************************************/
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/printk.h>
#include <linux/signal.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/bug.h>
#include <linux/types.h>
#include <linux/param.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/pm_wakeup.h>

#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif

#include "ff_log.h"
#include "ff_core.h"
#include "../include/oplus_fp_common.h"

/*****************************************************************************
* Private constant and macro definitions using #define
*****************************************************************************/
/* Define the driver version string. */
#define FF_DRV_VERSION            "V3.0.0-20220517"

/* Define the driver name. */
#define FF_DRV_NAME            "focaltech_fp"
#define FF_WAKELOCK_TIMEOUT            2000

/*power voltage range*/
#define FF_VTG_MIN_UV               3300000
#define FF_VTG_MAX_UV               3300000

/* Logging driver to logcat through uevent mechanism.    */
#undef LOG_TAG
#define LOG_TAG                "focaltech"


/*****************************************************************************
* Static variable or functions
*****************************************************************************/


/*****************************************************************************
* Global variable or extern global variabls/functions
*****************************************************************************/
ff_context_t *g_ff_ctx;
/* Log level can be runtime configurable by 'FF_IOC_SYNC_CONFIG'. */
ff_log_level_t g_ff_log_level = (ff_log_level_t)(__FF_EARLY_LOG_LEVEL);


/*#ifdef CONFIG_FINGERPRINT_FOCALTECH_SPI_SUPPORT    */
extern void mt_spi_enable_master_clk(struct spi_device *spidev);
extern void mt_spi_disable_master_clk(struct spi_device *spidev);
/*#endif    */

/*****************************************************************************
* Static function prototypes
*****************************************************************************/
static int ff_init_pins(ff_context_t *ff_ctx);
static int ff_init_power(ff_context_t *ff_ctx);
static int ff_register_irq(ff_context_t *ff_ctx);
static void ff_unprobe(ff_context_t *ff_ctx);

/*#ifdef CONFIG_FINGERPRINT_FOCALTECH_SPI_SUPPORT    */
__attribute__((weak)) void mt_spi_enable_master_clk(struct spi_device *spidev)
{
    FF_LOGD("enable spi clock-weak");
}
__attribute__((weak)) void mt_spi_disable_master_clk(struct spi_device *spidev)
{
    FF_LOGD("disable spi clock-weak");
}
/*#endif    */


int ff_log_printf(ff_log_level_t level, const char *tag, const char *fmt, ...)
{
    /* Prepare the storage. */
    va_list ap;
    static char uevent_env_buf[128];
    char *ptr = uevent_env_buf;
    int n, available = sizeof(uevent_env_buf);
    ff_context_t *ff_ctx = g_ff_ctx;

    /* Fill logging level. */
    available -= sprintf(uevent_env_buf, "FF_LOG=%1d", level);
    ptr += strlen(uevent_env_buf);

    /* Fill logging message. */
    va_start(ap, fmt);
    vsnprintf(ptr, available, fmt, ap);
    va_end(ap);

    /* Send to ff_device. */
    if (likely(ff_ctx) && unlikely(ff_ctx->ff_config.logcat_driver)
        && (ff_ctx->event_type == FF_EVENT_NETLINK)) {
        char *uevent_env[2] = {uevent_env_buf, NULL};
        kobject_uevent_env(&ff_ctx->fp_dev->kobj, KOBJ_CHANGE, uevent_env);
    }

    /* Native output. */
    switch (level) {
    case FF_LOG_LEVEL_ERR:
        n = printk(KERN_ERR "[E]focaltech: %s\n", ptr);
        break;
    case FF_LOG_LEVEL_WRN:
        n = printk(KERN_WARNING "[W]focaltech: %s\n", ptr);
        break;
    case FF_LOG_LEVEL_INF:
        n = printk(KERN_INFO "[I]focaltech: %s\n", ptr);
        break;
    case FF_LOG_LEVEL_DBG:
    case FF_LOG_LEVEL_VBS:
    default:
        n = printk(KERN_DEBUG "focaltech: %s\n", ptr);
        break;
    }
    return n;
}

/*disable/enable irq*/
static int ff_set_irq(ff_context_t *ff_ctx, bool enable)
{
    FF_LOGV("'%s' enter.", __func__);
    if (!ff_ctx || (ff_ctx->irq_num <= 0) || (!ff_ctx->b_driver_inited)) {
        FF_LOGE("ff_ctx/irq_num(%d)/b_driver_inited is invalid", ff_ctx->irq_num);
        return -EINVAL;
    }

    FF_LOGI("set irq:%d->%d", ff_ctx->b_irq_enabled, enable);
    if (!ff_ctx->b_irq_enabled && enable) {
        enable_irq(ff_ctx->irq_num);
        ff_ctx->b_irq_enabled = true;
    } else if (ff_ctx->b_irq_enabled && !enable) {
        disable_irq(ff_ctx->irq_num);
        ff_ctx->b_irq_enabled = false;
    }

    FF_LOGV("'%s' leave.", __func__);
    return 0;
}

/*reset device*/
static int ff_set_reset(ff_context_t *ff_ctx, bool high)
{
    int ret = 0;

    FF_LOGD("'%s' enter.", __func__);
    if (!ff_ctx || (!ff_ctx->b_driver_inited)) {
        FF_LOGE("ff_ctx/b_driver_inited is invalid");
        return -EINVAL;
    }

    FF_LOGI("set reset pin to %s,use_pinctrl:%d", high ? "high" : "low", ff_ctx->b_use_pinctrl);
    if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
        FF_LOGI("use pinctrl to control reset pin");
        if (high && ff_ctx->pins_reset_high) {
            ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_reset_high); }
        else if (!high && ff_ctx->pins_reset_low) {
            ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_reset_low); }
    }

    if (!ff_ctx->b_use_pinctrl && gpio_is_valid(ff_ctx->reset_gpio)) {
        FF_LOGI("use gpio(%d) to control reset pin", ff_ctx->reset_gpio);
        ret = gpio_direction_output(ff_ctx->reset_gpio, high);
    }

    if (ret < 0) {
        FF_LOGE("reset device fails");
        }
    FF_LOGD("'%s' leave.", __func__);
    return ret;
}

static int ff_power_on(ff_context_t *ff_ctx, bool on)
{
    int ret = 0;

    FF_LOGD("'%s' enter.", __func__);
    if (!ff_ctx || (!ff_ctx->b_driver_inited)) {
        FF_LOGE("ff_ctx/b_driver_inited is invalid");
        return -EINVAL;
    }

    if (ff_ctx->b_power_always_on) {
        FF_LOGI("power is always on, no control power");
        return 0;
    }

    FF_LOGI("set power:%d->%d,use_regulator:%d", ff_ctx->b_power_on, on, ff_ctx->b_use_regulator);
    if (ff_ctx->b_power_on ^ on) {
        if (ff_ctx->b_use_regulator && ff_ctx->ff_vdd) {
            FF_LOGI("use regulator to control power");
            if (on) {
                ret = regulator_enable(ff_ctx->ff_vdd); }
            else {
                ret = regulator_disable(ff_ctx->ff_vdd); }
        }

        if (!ff_ctx->b_use_regulator) {
            if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
                FF_LOGI("use pinctrl to control power pin");
                if (on && ff_ctx->pins_power_high) {
                    ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_power_high); }
                else if (!on && ff_ctx->pins_power_low) {
                    ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_power_low); }
            }

            if (!ff_ctx->b_use_pinctrl && gpio_is_valid(ff_ctx->vdd_gpio)) {
                FF_LOGI("use gpio(%d) to control power", ff_ctx->vdd_gpio);
                ret = gpio_direction_output(ff_ctx->vdd_gpio, on);
            }
        }

        if (ret < 0) {
            FF_LOGE("set power to %d failed,ret=%d", on, ret); }
        else {
            ff_ctx->b_power_on = on; }
    }

    FF_LOGD("'%s' leave.", __func__);
    return ret;
}

static void ff_spi_mode_enable(ff_context_t *ff_ctx, u8 bonoff)
{
    if (!ff_ctx->pinctrl) {
        FF_LOGE("ff_spi_mode_enable Cannot find pinctrl");
        return;
    }
    if (bonoff == 0) {
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_cs_low);
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_clk_low);
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_mosi_low);
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_miso_low);
    } else if (bonoff == 1) {
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_cs_mode);
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_clk);
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_mosi);
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_miso);
    }
}

static int ff_set_spiclk(ff_context_t *ff_ctx, bool enable)
{
    FF_LOGD("'%s' enter.", __func__);
/*#ifdef CONFIG_FINGERPRINT_FOCALTECH_SPI_SUPPORT    */
    if (ff_ctx && ff_ctx->b_spiclk && ff_ctx->spi && ff_ctx->b_power_on) {
        FF_LOGD("set spi clock:%d->%d", ff_ctx->b_spiclk_enabled, enable);
        if (!ff_ctx->b_spiclk_enabled && enable) {
            ff_spi_mode_enable(ff_ctx, 1);
            mt_spi_enable_master_clk(ff_ctx->spi);
            ff_ctx->b_spiclk_enabled = true;
        } else if (ff_ctx->b_spiclk_enabled && !enable) {
            mt_spi_disable_master_clk(ff_ctx->spi);
            ff_spi_mode_enable(ff_ctx, 0);
            ff_ctx->b_spiclk_enabled = false;
        } else {
            FF_LOGD("can't set spi clock , becasue : ff_ctx->b_spiclk is %d , enable is %d ", ff_ctx->b_spiclk, enable);
        }
    }
    else {
        FF_LOGD("set spi clock fail ...");
    }
/*#endif    */
    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

static int ff_register_input(ff_context_t *ff_ctx)
{
    int ret , i_try = 0;
    ff_driver_config_t *config = &ff_ctx->ff_config;
    struct input_dev *input;

    FF_LOGD("'%s' enter.", __func__);
    /* Allocate the input device. */
    input = input_allocate_device();
    if (!input) {
        FF_LOGE("input_allocate_device() failed.");
        return (-ENOMEM);
    }

    /* Register the key event capabilities. */
    for (i_try = 0; i_try < FF_GK_MAX; i_try++) {
        if (config->gesture_keycode[i_try] > 0) {
            FF_LOGD("register gesture keycode:%d", config->gesture_keycode[i_try]);
            input_set_capability(input, EV_KEY, config->gesture_keycode[i_try]);
        }
    }

    /* Register the allocated input device. */
    input->name = "ff_key";
    ret = input_register_device(input);
    if (ret < 0) {
        FF_LOGE("input_register_device(..) = %d.", ret);
        input_free_device(input);
        input = NULL;
        return (-ENODEV);
    }

    ff_ctx->input = input;
    FF_LOGD("'%s' leave.", __func__);
    return ret;
}

static int ff_ctl_init_driver(ff_context_t *ff_ctx)
{
    int ret = 0;
    FF_LOGD("'%s' enter.", __func__);

    if ((!ff_ctx) || ff_ctx->b_driver_inited) {
        FF_LOGE("ff_ctx is null, or driver has already initialized");
        return -EINVAL;
    }

    ret = ff_init_pins(ff_ctx);
    if (ret < 0) {
        FF_LOGE("init gpio pins fails,ret=%d", ret);
        return ret;
    }

    ret = ff_init_power(ff_ctx);
    if (ret < 0) {
        FF_LOGE("init power fails,ret=%d", ret);
        goto err_init_power;
    }

    ret = ff_register_irq(ff_ctx);
    if (ret < 0) {
        FF_LOGE("register ff irq fails,ret=%d", ret);
        goto err_request_irq;
    }

    if (ff_ctx->b_gesture_support) {
        /* Initialize the input subsystem. */
        ret = ff_register_input(ff_ctx);
        if (ret < 0) {
            FF_LOGE("ff_register_input() = %d.", ret);
            goto err_register_input;
        }
    }

    ff_ctx->b_driver_inited = true;
    FF_LOGD("'%s' leave.", __func__);
    return 0;

err_enable_spiclk:
    if (ff_ctx->b_gesture_support) {
        input_unregister_device(ff_ctx->input);
        ff_ctx->input = NULL;
    }
err_register_input:
    if (ff_ctx->irq_num > 0) {
        disable_irq_wake(ff_ctx->irq_num);
        free_irq(ff_ctx->irq_num, ff_ctx);
        ff_ctx->irq_num = -1;
    }
err_request_irq:
    if (!ff_ctx->b_power_always_on) {
        if (ff_ctx->b_use_regulator && ff_ctx->ff_vdd) {
            regulator_put(ff_ctx->ff_vdd); }
        if (!ff_ctx->b_use_regulator && !ff_ctx->b_use_pinctrl) {
            if (gpio_is_valid(ff_ctx->vdd_gpio)) {
                gpio_free(ff_ctx->vdd_gpio); }
        }
    }
err_init_power:
    if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
        devm_pinctrl_put(ff_ctx->pinctrl);
        ff_ctx->pins_irq_as_int = NULL;
        ff_ctx->pins_reset_low = NULL;
        ff_ctx->pins_reset_high = NULL;
        ff_ctx->pins_power_low = NULL;
        ff_ctx->pins_power_high = NULL;
    } else if (!ff_ctx->b_use_pinctrl) {
        if (gpio_is_valid(ff_ctx->irq_gpio)) {
            gpio_free(ff_ctx->irq_gpio); }
        if (gpio_is_valid(ff_ctx->reset_gpio)) {
            gpio_free(ff_ctx->reset_gpio); }
    }

    FF_LOGD("'%s' leave.", __func__);
    return ret;
}

static int ff_ctl_free_driver(ff_context_t *ff_ctx)
{
    int ret = 0;
    FF_LOGD("'%s' enter.", __func__);

    if ((!ff_ctx) || !ff_ctx->b_driver_inited) {
        FF_LOGE("ff_ctx is null, or driver has not initialized");
        return -EINVAL;
    }

    /* Disable SPI clock. */
    ret = ff_set_spiclk(ff_ctx, 0);
    if (ret < 0) {
        FF_LOGE("disable spi clock fails");
        }
    /* De-initialize the input subsystem. */
    if (ff_ctx->input) {
        input_unregister_device(ff_ctx->input);
        ff_ctx->input = NULL;
    }

    /*free irq*/
    if (ff_ctx->irq_num > 0) {
        disable_irq_wake(ff_ctx->irq_num);
        free_irq(ff_ctx->irq_num, ff_ctx);
        ff_ctx->irq_num = -1;
    }

    /*free power pin*/
    if (!ff_ctx->b_power_always_on) {
        if (ff_ctx->b_power_on) {
            ff_set_reset(ff_ctx, 0);
            msleep(10);
            ff_power_on(ff_ctx, 0);
            msleep(10);
        }

        if (ff_ctx->b_use_regulator && ff_ctx->ff_vdd) {
            regulator_put(ff_ctx->ff_vdd); }
        if (!ff_ctx->b_use_regulator && !ff_ctx->b_use_pinctrl) {
            if (gpio_is_valid(ff_ctx->vdd_gpio)) {
                gpio_free(ff_ctx->vdd_gpio); }
        }
    }

    /*free rst/int pins*/
    if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
        devm_pinctrl_put(ff_ctx->pinctrl);
        ff_ctx->pins_irq_as_int = NULL;
        ff_ctx->pins_reset_low = NULL;
        ff_ctx->pins_reset_high = NULL;
        ff_ctx->pins_power_low = NULL;
        ff_ctx->pins_power_high = NULL;
    } else if (!ff_ctx->b_use_pinctrl) {
        if (gpio_is_valid(ff_ctx->irq_gpio)) {
            gpio_free(ff_ctx->irq_gpio); }
        if (gpio_is_valid(ff_ctx->reset_gpio)) {
            gpio_free(ff_ctx->reset_gpio); }
    }

    ff_ctx->b_driver_inited = false;
    FF_LOGD("'%s' leave.", __func__);
    return ret;
}

static int ff_ctl_report_key_event(ff_context_t *ff_ctx, unsigned long arg)
{
    ff_key_event_t kevent;

    FF_LOGV("'%s' enter.", __func__);
    if (copy_from_user(&kevent, (ff_key_event_t *)arg, sizeof(ff_key_event_t))) {
        FF_LOGE("copy_from_user(ff_key_event_t) failed.");
        return (-EFAULT);
    }

    FF_LOGD("key code=%d,value=%d", kevent.code, kevent.value);
    if (ff_ctx->input && kevent.code) {
        input_report_key(ff_ctx->input, kevent.code, kevent.value);
        input_sync(ff_ctx->input);
    }

    FF_LOGV("'%s' leave.", __func__);
    return 0;
}

static int ff_ctl_get_version(ff_context_t *ff_ctx, unsigned long arg)
{
    char version[FF_DRV_VERSION_LEN] = { 0 };

    FF_LOGD("'%s' enter.", __func__);
    strncpy(version, FF_DRV_VERSION, FF_DRV_VERSION_LEN);
    if (copy_to_user((void *)arg, version, FF_DRV_VERSION_LEN)) {
        FF_LOGE("copy_to_user(version) failed.");
        return (-EFAULT);
    }

    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

static int ff_ctl_acquire_wakelock(ff_context_t *ff_ctx, unsigned long arg)
{
    u32 wake_lock_ms = 0;
    FF_LOGD("'%s' enter.", __func__);

    if (copy_from_user(&wake_lock_ms, (u32 *)arg, sizeof(u32))) {
        FF_LOGE("copy_from_user(wake_lock_ms) failed.");
        return (-EFAULT);
    }

    FF_LOGD("wake_lock_ms = %u", wake_lock_ms);
    if (wake_lock_ms == 0) {
        __pm_stay_awake(&ff_ctx->wake_lock_ctl); }
    else {
        __pm_wakeup_event(&ff_ctx->wake_lock_ctl, wake_lock_ms); }

    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

static int ff_ctl_release_wakelock(ff_context_t *ff_ctx)
{
    FF_LOGD("'%s' enter.", __func__);
    __pm_relax(&ff_ctx->wake_lock_ctl);
    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

static int ff_ctl_sync_config(ff_context_t *ff_ctx, unsigned long arg)
{
    int  i_try = 0;
    ff_driver_config_t driver_config;

    FF_LOGD("'%s' enter.", __func__);
    if (copy_from_user(&driver_config, (ff_driver_config_t *)arg, sizeof(ff_driver_config_t))) {
        FF_LOGE("copy_from_user(ff_driver_config_t) failed.");
        return (-EFAULT);
    }

    /*check gesture is supported or not*/
    ff_ctx->b_gesture_support = false;
    for (i_try = 0; i_try < FF_GK_MAX; i_try++) {
        if (driver_config.gesture_keycode[i_try] > 0) {
            FF_LOGI("gesture is supported.");
            ff_ctx->b_gesture_support = true;
            break;
        }
    }

    /* Take logging level effect. */
    FF_LOGI("set log:%d->%d", g_ff_log_level, driver_config.log_level);
    g_ff_log_level = driver_config.log_level;
    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

static int ff_ctl_fasync(int fd, struct file *filp, int mode)
{
    int err = 0;
    ff_context_t *ff_ctx = filp->private_data;

    FF_LOGV("'%s' enter.", __func__);
    if (!ff_ctx) {
        FF_LOGE("ff_ctx is null");
        return -ENODATA;
    }

    FF_LOGD("%s: mode = 0x%08x.", __func__, mode);
    err = fasync_helper(fd, filp, mode, &ff_ctx->async_queue);

    FF_LOGV("'%s' leave.", __func__);
    return err;
}

static long ff_ctl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    ff_context_t *ff_ctx = filp->private_data;

    FF_LOGV("'%s' enter.", __func__);
    if (!ff_ctx) {
        FF_LOGE("ff_ctx is null");
        return -ENODATA;
    }

    FF_LOGV("ioctl cmd:%x,arg:%lx", (int)cmd, arg);
    switch (cmd) {
    case FF_IOC_GET_EVENT_INFO:
        FF_LOGV("get event info:%d", ff_ctx->poll_event);
        ret = __put_user(ff_ctx->poll_event, (int __user *)arg);
        ff_ctx->poll_event = FF_POLLEVT_NONE;
        break;
    case FF_IOC_INIT_DRIVER:
        FF_LOGI("ioctrl:FF_IOC_INIT_DRIVER");
        if (ff_ctx->b_driver_inited) {
            FF_LOGI("ff driver inited, need free first");
            ret = ff_ctl_free_driver(ff_ctx);
            if (ret < 0) {
                FF_LOGE("free driver fails");
                return ret;
            }
        }
        ret = ff_ctl_init_driver(ff_ctx);
        break;
    case FF_IOC_FREE_DRIVER:
        FF_LOGI("ioctrl:FF_IOC_FREE_DRIVER");
        if (ff_ctx->b_driver_inited) {
            ret = ff_ctl_free_driver(ff_ctx); }
        break;
    case FF_IOC_RESET_DEVICE:
        FF_LOGD("need low->high");
        ret = ff_set_reset(ff_ctx, 0);
        msleep(10);
        ret = ff_set_reset(ff_ctx, 1);
        break;
    case FF_IOC_RESET_DEVICE_HL:
        ret = ff_set_reset(ff_ctx, !!arg);
        break;
    case FF_IOC_ENABLE_IRQ:
        ret = ff_set_irq(ff_ctx, 1);
        break;
    case FF_IOC_DISABLE_IRQ:
        ret = ff_set_irq(ff_ctx, 0);
        break;
    case FF_IOC_ENABLE_SPI_CLK:
        ret = ff_set_spiclk(ff_ctx, 1);
        break;
    case FF_IOC_DISABLE_SPI_CLK:
        ret = ff_set_spiclk(ff_ctx, 0);
        break;
    case FF_IOC_ENABLE_POWER:
        ret = ff_power_on(ff_ctx, 1);
        break;
    case FF_IOC_DISABLE_POWER:
        ret = ff_power_on(ff_ctx, 0);
        break;
    case FF_IOC_REPORT_KEY_EVENT:
        ret = ff_ctl_report_key_event(ff_ctx, arg);
        break;
    case FF_IOC_SYNC_CONFIG:
        ret = ff_ctl_sync_config(ff_ctx, arg);
        break;
    case FF_IOC_GET_VERSION:
        ret = ff_ctl_get_version(ff_ctx, arg);
        break;
    case FF_IOC_SET_VENDOR_INFO:
        break;
    case FF_IOC_WAKELOCK_ACQUIRE:
        ret = ff_ctl_acquire_wakelock(ff_ctx, arg);
        break;
    case FF_IOC_WAKELOCK_RELEASE:
        ret = ff_ctl_release_wakelock(ff_ctx);
        break;
    case FF_IOC_GET_LOGLEVEL:
        FF_LOGI("get log:%d", g_ff_log_level);
        ret = __put_user(g_ff_log_level, (int __user *)arg);
        break;
    case FF_IOC_SET_LOGLEVEL:
        ret = __get_user(g_ff_log_level, (int __user *)arg);
        FF_LOGI("set log level:%d", g_ff_log_level);
        break;
    case FF_IOC_GET_FEATURE:
        FF_LOGI("feature ver:%04xh", ff_ctx->feature.ver);
        ret = copy_to_user((ff_feature_t __user *)arg, &ff_ctx->feature, sizeof(ff_feature_t));
        break;
    case FF_IOC_GET_EVENT_TYPE:
        FF_LOGI("get event type:%d", ff_ctx->event_type);
        ret = __put_user(ff_ctx->event_type, (int __user *)arg);
        break;
    case FF_IOC_SET_EVENT_TYPE:
        ret = __get_user(ff_ctx->event_type, (int __user *)arg);
        FF_LOGI("set event type:%d", ff_ctx->event_type);
        break;
    case FF_IOC_UNPROBE:
        FF_LOGI("device unprobe");
        ff_unprobe(ff_ctx);
        break;
    default:
        ret = (-EINVAL);
        break;
    }

    FF_LOGV("'%s' leave.", __func__);
    return ret;
}

static unsigned int ff_ctl_poll(struct file *filp, struct poll_table_struct *wait)
{
    unsigned int mask = 0;
    ff_context_t *ff_ctx = filp->private_data;

    FF_LOGV("'%s' enter.", __func__);
    if (!ff_ctx) {
        FF_LOGE("ff_ctx is null");
        return -ENODATA;
    }

    poll_wait(filp, &ff_ctx->wait_queue_head, wait);
    if (ff_ctx->poll_event != FF_POLLEVT_NONE) {
        mask |= POLLIN | POLLRDNORM; }
    FF_LOGV("'%s' leave.", __func__);
    return mask;
}

static int ff_ctl_open(struct inode *inode, struct file *filp)
{
    ff_context_t *ff_ctx = container_of(inode->i_cdev, ff_context_t, ff_cdev);
    FF_LOGV("'%s' enter.", __func__);
    /*Don't allow open device while b_ff_probe is 0*/
    if (!ff_ctx || !ff_ctx->b_ff_probe) {
        FF_LOGE("ff_ctx is null/probe(%d) fails", ff_ctx->b_ff_probe);
        return -EINVAL;
    }
    filp->private_data = ff_ctx;
    FF_LOGV("'%s' leave.", __func__);
    return 0;
}

static int ff_ctl_release(struct inode *inode, struct file *filp)
{
    int err = 0;
    FF_LOGV("'%s' enter.", __func__);

    /* Remove this filp from the asynchronously notified filp's. */
    err = ff_ctl_fasync(-1, filp, 0);

    FF_LOGV("'%s' leave.", __func__);
    return err;
}

#ifdef CONFIG_COMPAT
static long ff_ctl_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int err = 0;
    FF_LOGV("focal '%s' enter.\n", __func__);

    err = ff_ctl_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));

    FF_LOGV("'%s' leave.", __func__);
    return err;
}
#endif


static struct file_operations ff_fops = {
    .owner          = THIS_MODULE,
    .fasync         = ff_ctl_fasync,
    .unlocked_ioctl = ff_ctl_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = ff_ctl_compat_ioctl,
#endif
    .poll           = ff_ctl_poll,
    .open           = ff_ctl_open,
    .release        = ff_ctl_release,
};


/* ff_log node */
static ssize_t ff_log_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    FF_LOGV("'%s' enter.", __func__);
    count += snprintf(buf + count, PAGE_SIZE, "log level:%d\n", g_ff_log_level);
    FF_LOGV("'%s' leave.", __func__);
    return count;
}

static ssize_t ff_log_store(
    struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int value = 0;
    FF_LOGV("'%s' enter.", __func__);
    sscanf(buf, "%d", &value);
    FF_LOGI("log level:%d->%d", g_ff_log_level, value);
    g_ff_log_level = value;

    FF_LOGV("'%s' leave.", __func__);
    return count;
}

/* ff_reset interface */
static ssize_t ff_reset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGV("'%s' enter.", __func__);
    if (ff_ctx && ff_ctx->b_driver_inited) {
        ff_set_reset(ff_ctx, 0);
        msleep(10);
        ff_set_reset(ff_ctx, 1);
        count = snprintf(buf, PAGE_SIZE, "fingerprint device has been reset\n");
    }
    FF_LOGV("'%s' leave.", __func__);
    return count;
}

static ssize_t ff_reset_store(
    struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGV("'%s' enter.", __func__);
    if (buf[0] == '1') {
        ff_set_reset(ff_ctx, 1);
    } else if (buf[0] == '0') {
        ff_set_reset(ff_ctx, 0);
    }
    FF_LOGV("'%s' leave.", __func__);
    return count;
}

/* ff_irq interface */
static ssize_t ff_irq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGV("'%s' enter.", __func__);
    if (ff_ctx && (ff_ctx->irq_num > 0) && ff_ctx->b_driver_inited) {
        struct irq_desc *desc = irq_to_desc(ff_ctx->irq_num);
        count = snprintf(buf, PAGE_SIZE, "irq_depth:%d\n", desc->depth);
    }
    FF_LOGV("'%s' leave.", __func__);
    return count;
}

static ssize_t ff_irq_store(
    struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGV("'%s' enter.", __func__);
    if (buf[0] == '1') {
        ff_set_irq(ff_ctx, 1);
    } else if (buf[0] == '0') {
        ff_set_irq(ff_ctx, 0);
    }
    FF_LOGV("'%s' leave.", __func__);
    return count;
}

/* ff_info interface */
static ssize_t ff_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGV("'%s' enter.", __func__);
    if (ff_ctx) {
        count += snprintf(buf + count, PAGE_SIZE, "gpio,reset:%d,int:%d-%d,power:%d\n",
                     ff_ctx->reset_gpio, ff_ctx->irq_gpio, ff_ctx->irq_num, ff_ctx->vdd_gpio);
        count += snprintf(buf + count, PAGE_SIZE, "power_always_on:%d,use_regulator:%d,use_pinctrl:%d\n",
                     ff_ctx->b_power_always_on, ff_ctx->b_use_regulator, ff_ctx->b_use_pinctrl);
        count += snprintf(buf + count, PAGE_SIZE, "read_chip:%d,ree:%d,screen on/off:%d\n",
                     ff_ctx->b_read_chipid, ff_ctx->b_ree, ff_ctx->b_screen_onoff_event);

        count += snprintf(buf + count, PAGE_SIZE, "driver_init:%d,power_on:%d,irq_en:%d,spiclk_en:%d\n",
                     ff_ctx->b_driver_inited, ff_ctx->b_power_on, ff_ctx->b_irq_enabled, ff_ctx->b_spiclk_enabled);

        count += snprintf(buf + count, PAGE_SIZE, "gesture_en:%d,%d-%d-%d-%d\n", ff_ctx->b_gesture_support,
                     ff_ctx->ff_config.gesture_keycode[0], ff_ctx->ff_config.gesture_keycode[1],
                     ff_ctx->ff_config.gesture_keycode[2], ff_ctx->ff_config.gesture_keycode[3]);
    }
    FF_LOGV("'%s' leave.", __func__);
    return count;
}

static ssize_t ff_info_store(
    struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGV("'%s' enter.", __func__);
    if (buf[0] == '1') {
        ff_ctl_init_driver(ff_ctx);
    } else if (buf[0] == '0') {
        ff_ctl_free_driver(ff_ctx);
    }
    FF_LOGV("'%s' leave.", __func__);
    return count;
}

static DEVICE_ATTR(ff_log, S_IRUGO | S_IWUSR, ff_log_show, ff_log_store);
static DEVICE_ATTR(ff_reset, S_IRUGO | S_IWUSR, ff_reset_show, ff_reset_store);
static DEVICE_ATTR(ff_irq, S_IRUGO | S_IWUSR, ff_irq_show, ff_irq_store);
static DEVICE_ATTR(ff_info, S_IRUGO | S_IWUSR, ff_info_show, ff_info_store);
static struct attribute *ff_attrs[] = {
    &dev_attr_ff_log.attr,
    &dev_attr_ff_reset.attr,
    &dev_attr_ff_irq.attr,
    &dev_attr_ff_info.attr,
    NULL
};
static struct attribute_group ff_attrs_group = {
    .attrs = ff_attrs
};

static void ff_work_device_event(struct work_struct *ws)
{
    ff_context_t *ff_ctx = container_of(ws, ff_context_t, event_work);
    char *uevent_env[2] = {"FF_INTERRUPT", NULL};
    FF_LOGV("'%s' enter.", __func__);

    /* System should keep wakeup for 2 seconds at least. */
    __pm_wakeup_event(&ff_ctx->wake_lock, jiffies_to_msecs(FF_WAKELOCK_TIMEOUT));

    FF_LOGD("%s(irq = %d, ..) toggled.", __func__, ff_ctx->irq_num);
    if (ff_ctx->event_type == FF_EVENT_POLL) {
        ff_ctx->poll_event |= FF_POLLEVT_INTERRUPT;
        wake_up_interruptible(&ff_ctx->wait_queue_head);
    } else if (ff_ctx->event_type == FF_EVENT_NETLINK) {
        kobject_uevent_env(&ff_ctx->fp_dev->kobj, KOBJ_CHANGE, uevent_env);
    } else {
        FF_LOGE("unknowed event type:%d", ff_ctx->event_type); }
        FF_LOGV("'%s' leave.", __func__);
}


static irqreturn_t ff_irq_handler(int irq, void *dev_id)
{
    ff_context_t *ff_ctx = (ff_context_t *)dev_id;

    if (ff_ctx && (likely(irq == ff_ctx->irq_num))) {
        if (ff_ctx->ff_config.enable_fasync && ff_ctx->async_queue) {
            FF_LOGV("'%s' start kill_fasync ...");
            kill_fasync(&ff_ctx->async_queue, SIGIO, POLL_IN);
            FF_LOGV("'%s' end kill_fasync ...");
        } else {
            FF_LOGV("'%s' start schedule_work ...");
            schedule_work(&ff_ctx->event_work);
            FF_LOGV("'%s' end schedule_work ...");
        }
    }

    return IRQ_HANDLED;
}


static void ff_parse_dt(struct device *dev, ff_context_t *ff_ctx)
{
    struct device_node *np = dev->of_node;

    FF_LOGV("'%s' enter.", __func__);
    if (!np) {
        FF_LOGE("device node is null");
    }

    /*check if need report screen on/off event*/
    ff_ctx->b_screen_onoff_event = !of_property_read_bool(np, "focaltech_fp,no-screen-on-off");

    /*check if power is always on*/
    ff_ctx->b_power_always_on = of_property_read_bool(np, "focaltech_fp,power-always-on");

    /*check if power regulator is used*/
    ff_ctx->b_use_regulator = of_property_read_bool(np, "focaltech_fp,use-regulator");

    /*check if pinctrl(irq and reset) is used*/
    ff_ctx->b_use_pinctrl = of_property_read_bool(np, "focaltech_fp,use-pinctrl");

    /*check if need read chip id*/
    ff_ctx->b_read_chipid = of_property_read_bool(np, "focaltech_fp,read-chip");

    /*check if supports REE*/
    ff_ctx->b_ree = of_property_read_bool(np, "focaltech_fp,ree");

    /*check if supports spi clock enable/disable*/
    ff_ctx->b_spiclk = of_property_read_bool(np, "focaltech_fp,spiclk");

    /*check gpios*/
    ff_ctx->reset_gpio = of_get_named_gpio(np, "focaltech_fp,reset-gpio", 0);
    if (!gpio_is_valid(ff_ctx->reset_gpio)) {
        FF_LOGD("unable to get ff reset gpio");
        }
    ff_ctx->irq_gpio = of_get_named_gpio(np, "focaltech_fp,irq-gpio", 0);
    if (!gpio_is_valid(ff_ctx->irq_gpio)) {
        FF_LOGD("unable to get ff irq gpio");
        }
    ff_ctx->vdd_gpio = of_get_named_gpio(np, "focaltech_fp,vdd-gpio", 0);
    if (!gpio_is_valid(ff_ctx->vdd_gpio)) {
        FF_LOGD("unable to get ff vdd gpio");
        }
    FF_LOGI("irq gpio:%d, reset gpio:%d, vdd gpio:%d", ff_ctx->irq_gpio,
            ff_ctx->reset_gpio, ff_ctx->vdd_gpio);
    FF_LOGD("power_always_on:%d, use_regulator:%d, use_pinctrl:%d",
            ff_ctx->b_power_always_on, ff_ctx->b_use_regulator, ff_ctx->b_use_pinctrl);
    FF_LOGD("read_chip:%d, ree:%d, spiclk:%d, screen on/off:%d", ff_ctx->b_read_chipid,
            ff_ctx->b_ree, ff_ctx->b_spiclk, ff_ctx->b_screen_onoff_event);
    FF_LOGV("'%s' leave.", __func__);
}

static int ff_init_pins(ff_context_t *ff_ctx)
{
    int ret = 0;

    FF_LOGD("'%s' enter.", __func__);
    if (ff_ctx->b_use_pinctrl) {
        /*get pinctrl from DTS*/
        ff_ctx->pinctrl = devm_pinctrl_get(&ff_ctx->pdev->dev);
        if (IS_ERR_OR_NULL(ff_ctx->pinctrl)) {
            ret = PTR_ERR(ff_ctx->pinctrl);
            FF_LOGE("Failed to get pinctrl, please check dts,ret=%d", ret);
            return ret;
        }

        ff_ctx->pins_reset_low = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_reset_low");
        if (IS_ERR_OR_NULL(ff_ctx->pins_reset_low)) {
            ret = PTR_ERR(ff_ctx->pins_reset_low);
            FF_LOGE("ff_pins_reset_low not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            return ret;
        }

        ff_ctx->pins_reset_high = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_reset_high");
        if (IS_ERR_OR_NULL(ff_ctx->pins_reset_high)) {
            ret = PTR_ERR(ff_ctx->pins_reset_high);
            FF_LOGE("ff_pins_reset_high not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            return ret;
        }

        ff_ctx->pins_irq_as_int = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_irq_as_int");
        if (IS_ERR_OR_NULL(ff_ctx->pins_irq_as_int)) {
            ret = PTR_ERR(ff_ctx->pins_irq_as_int);
            FF_LOGE("ff_pins_irq_as_int not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            return ret;
        }

        FF_LOGI("get irq-reset-power pinctrl success");
    } else {
        /* request irq gpio */
        if (gpio_is_valid(ff_ctx->irq_gpio)) {
            FF_LOGI("init irq gpio:%d", ff_ctx->irq_gpio);
            ret = gpio_request(ff_ctx->irq_gpio, "ff_irq_gpio");
            if (ret < 0) {
                FF_LOGE("irq gpio request failed");
                return ret;
            }

            ret = gpio_direction_input(ff_ctx->irq_gpio);
            if (ret < 0) {
                FF_LOGE("set_direction for irq gpio failed");
                gpio_free(ff_ctx->irq_gpio);
                return ret;
            }
        }

        /* request reset gpio */
        if (gpio_is_valid(ff_ctx->reset_gpio)) {
            FF_LOGI("init reset gpio:%d", ff_ctx->reset_gpio);
            ret = gpio_request(ff_ctx->reset_gpio, "ff_reset_gpio");
            if (ret) {
                FF_LOGE("reset gpio request failed");
                if (gpio_is_valid(ff_ctx->irq_gpio)) {
                    gpio_free(ff_ctx->irq_gpio); }
                return ret;
            }

            ret = gpio_direction_output(ff_ctx->reset_gpio, 0);
            if (ret < 0) {
                FF_LOGE("set_direction for reset gpio failed");
                if (gpio_is_valid(ff_ctx->irq_gpio)) {
                    gpio_free(ff_ctx->irq_gpio); }
                gpio_free(ff_ctx->reset_gpio);
                return ret;
            }
        }
        FF_LOGI("get irq and reset gpio success");

        /*get pinctrl from DTS*/
        ff_ctx->pinctrl = devm_pinctrl_get(&ff_ctx->pdev->dev);
        if (IS_ERR_OR_NULL(ff_ctx->pinctrl)) {
            ret = PTR_ERR(ff_ctx->pinctrl);
            FF_LOGE("Failed to get pinctrl, please check dts,ret=%d", ret);
            return ret;
        }

        ff_ctx->pins_spi_cs_mode = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_cs_mode");
        if (IS_ERR_OR_NULL(ff_ctx->pins_spi_cs_mode)) {
            ret = PTR_ERR(ff_ctx->pins_spi_cs_mode);
            FF_LOGE("ff_pins_cs_mode not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            return ret;
        }
        ff_ctx->pins_spi_clk = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_spi_clk");
        if (IS_ERR_OR_NULL(ff_ctx->pins_spi_clk)) {
            ret = PTR_ERR(ff_ctx->pins_spi_clk);
            FF_LOGE("ff_pins_spi_clk not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            return ret;
        }
        ff_ctx->pins_spi_mosi = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_spi_mosi");
        if (IS_ERR_OR_NULL(ff_ctx->pins_spi_mosi)) {
            ret = PTR_ERR(ff_ctx->pins_spi_mosi);
            FF_LOGE("ff_pins_spi_mosi not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            return ret;
        }
        ff_ctx->pins_spi_miso = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_spi_miso");
        if (IS_ERR_OR_NULL(ff_ctx->pins_spi_miso)) {
            ret = PTR_ERR(ff_ctx->pins_spi_miso);
            FF_LOGE("ff_pins_spi_miso not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            return ret;
        }
        ff_ctx->pins_spi_clk_low = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_spi_clk_low");
        if (IS_ERR_OR_NULL(ff_ctx->pins_spi_clk_low)) {
            ret = PTR_ERR(ff_ctx->pins_spi_clk_low);
            FF_LOGE("ff_pins_spi_clk_low not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            return ret;
        }
        ff_ctx->pins_spi_mosi_low = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_spi_mosi_low");
        if (IS_ERR_OR_NULL(ff_ctx->pins_spi_mosi_low)) {
            ret = PTR_ERR(ff_ctx->pins_spi_mosi_low);
            FF_LOGE("ff_pins_spi_mosi_low not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            return ret;
        }
        ff_ctx->pins_spi_miso_low = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_spi_miso_low");
        if (IS_ERR_OR_NULL(ff_ctx->pins_spi_miso_low)) {
            ret = PTR_ERR(ff_ctx->pins_spi_miso_low);
            FF_LOGE("ff_pins_spi_miso_low not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            return ret;
        }
        ff_ctx->pins_spi_cs_low = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_spi_cs_low");
        if (IS_ERR_OR_NULL(ff_ctx->pins_spi_cs_low)) {
            ret = PTR_ERR(ff_ctx->pins_spi_cs_low);
            FF_LOGE("ff_pins_spi_cs_low not found,ret=%d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            return ret;
        }

        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_cs_low);
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_clk_low);
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_miso_low);
        pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_spi_mosi_low);

        FF_LOGI("pinctrl_select_state SPI ok");
    }

    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

static int ff_init_power(ff_context_t *ff_ctx)
{
    int ret = 0;

    FF_LOGD("'%s' enter.", __func__);

    if (ff_ctx->b_power_always_on) {
        FF_LOGI("power is always on, no init power");
        return 0;
    }

    if (ff_ctx->b_use_regulator) {
        /*get regulator*/
        ff_ctx->ff_vdd = regulator_get(&ff_ctx->pdev->dev, "ff_vdd");
        if (IS_ERR_OR_NULL(ff_ctx->ff_vdd)) {
            ret = PTR_ERR(ff_ctx->ff_vdd);
            FF_LOGE("get ff vdd regulator failed,ret=%d", ret);
            return ret;
        }

        if (regulator_count_voltages(ff_ctx->ff_vdd) > 0) {
            ret = regulator_set_voltage(ff_ctx->ff_vdd, FF_VTG_MIN_UV, FF_VTG_MAX_UV);
            if (ret) {
                FF_LOGD("vdd regulator set_voltage failed ret=%d", ret);
                regulator_put(ff_ctx->ff_vdd);
                return ret;
            }
        }
        FF_LOGI("get power regulator success");
    } else if (ff_ctx->b_use_pinctrl) {
        FF_LOGI("get power pinctrl success");
    } else {
        if (gpio_is_valid(ff_ctx->vdd_gpio)) {
            ret = gpio_request(ff_ctx->vdd_gpio, "ff_vdd_gpio");
            if (ret < 0) {
                FF_LOGE("vdd gpio request failed");
                return ret;
            }

            ret = gpio_direction_output(ff_ctx->vdd_gpio, 0);
            if (ret < 0) {
                FF_LOGE("set_direction for vdd gpio failed");
                gpio_free(ff_ctx->vdd_gpio);
                return ret;
            }
            FF_LOGI("get power gpio(%d) success", ff_ctx->vdd_gpio);
        }
    }

    ff_ctx->b_power_on = false;
    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

static int ff_register_irq(ff_context_t *ff_ctx)
{
    int ret = 0;
    unsigned long irqflags = 0;
    struct device_node *np = ff_ctx->pdev->dev.of_node;

    FF_LOGD("'%s' enter.", __func__);
    /*get irq num*/
    ff_ctx->irq_num = gpio_to_irq(ff_ctx->irq_gpio);
    FF_LOGI("irq_gpio:%d,irq_num:%d", ff_ctx->irq_gpio, ff_ctx->irq_num);
    if (np && ff_ctx->b_use_pinctrl && (ff_ctx->irq_num <= 0)) {
        ff_ctx->irq_num = irq_of_parse_and_map(np, 0);
    }

    if (ff_ctx->irq_num > 0) {
        FF_LOGI("irq_num:%d", ff_ctx->irq_num);
        irqflags = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
        ret = request_irq(ff_ctx->irq_num, ff_irq_handler, irqflags, FF_DRV_NAME, ff_ctx);
        ff_ctx->b_irq_enabled = true;
        enable_irq_wake(ff_ctx->irq_num);
    } else {
        FF_LOGE("irq_num:%d is invalid", ff_ctx->irq_num);
        ret = ff_ctx->irq_num;
    }

    FF_LOGD("'%s' leave.", __func__);
    return ret;
}

/*create /dev/focaltech_fp*/
static int ff_register_device(ff_context_t *ff_ctx)
{
    int ret = 0;
    dev_t ff_devno;

    FF_LOGD("'%s' enter.", __func__);
    ret = alloc_chrdev_region(&ff_devno, 0, 1, FF_DRV_NAME);
    if (ret < 0) {
        FF_LOGE("can't  get ff major number,ret=%d", ret);
        return ret;
    }

    cdev_init(&ff_ctx->ff_cdev, &ff_fops);
    ff_ctx->ff_cdev.owner = THIS_MODULE;
    ff_ctx->ff_cdev.ops = &ff_fops;
    ret = cdev_add(&ff_ctx->ff_cdev, ff_devno, 1);
    if (ret < 0) {
        FF_LOGE("ff cdev add fails,ret=%d", ret);
        unregister_chrdev_region(ff_devno, 1);
        return ret;
    }
    FF_LOGD("ff devno:%x, %x", (int)ff_devno, (int)ff_ctx->ff_cdev.dev);

    ff_ctx->ff_class = class_create(THIS_MODULE, FF_DRV_NAME);
    if (IS_ERR(ff_ctx->ff_class)) {
        ret = PTR_ERR(ff_ctx->ff_class);
        FF_LOGE("create ff class fails,ret=%d", ret);
        cdev_del(&ff_ctx->ff_cdev);
        unregister_chrdev_region(ff_devno, 1);
        return ret;
    }

    ff_ctx->fp_dev = device_create(ff_ctx->ff_class, NULL, ff_devno, ff_ctx, FF_DRV_NAME);
    if (IS_ERR(ff_ctx->fp_dev)) {
        ret = PTR_ERR(ff_ctx->fp_dev);
        FF_LOGE("ff device_create failed,ret=%d\n", ret);
        class_destroy(ff_ctx->ff_class);
        cdev_del(&ff_ctx->ff_cdev);
        unregister_chrdev_region(ff_devno, 1);
        return ret;
    }

    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

/*screen on/off callback*/
#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
    struct fb_event *evdata = data;
    int *blank = NULL;
    ff_context_t *ff_ctx = container_of(self, ff_context_t, fb_notif);
    char *uevent_env[2];
    bool this_screen_on;

    FF_LOGV("'%s' enter.", __func__);
    if (!ff_ctx || !evdata) {
        FF_LOGE("ff_ctx/evdata is null");
        return NOTIFY_DONE;
    }

    if (!(event == FB_EARLY_EVENT_BLANK || event == FB_EVENT_BLANK)) {
        FF_LOGI("event(%lu) do not need process\n", event);
        return NOTIFY_DONE;
    }

    blank = evdata->data;
    FF_LOGD("FB event:%lu,blank:%d", event, *blank);
    switch (*blank) {
    case FB_BLANK_UNBLANK:
        uevent_env[0] = "FF_SCREEN_ON";
        this_screen_on = true;
        break;
    case FB_BLANK_POWERDOWN:
        uevent_env[0] = "FF_SCREEN_OFF";
        this_screen_on = false;
        break;
    default:
        FF_LOGI("FB BLANK(%d) do not need process\n", *blank);
        uevent_env[0] = "FF_SCREEN_??";
        break;
    }

    if (ff_ctx->b_screen_onoff_event && (ff_ctx->b_screen_on ^ this_screen_on)) {
        FF_LOGD("screen:%d->%d", ff_ctx->b_screen_on, this_screen_on);
        ff_ctx->b_screen_on = this_screen_on;
        if (ff_ctx->event_type == FF_EVENT_POLL) {
            ff_ctx->poll_event |= this_screen_on ? FF_POLLEVT_SCREEN_ON : FF_POLLEVT_SCREEN_OFF;
            wake_up_interruptible(&ff_ctx->wait_queue_head);
        } else if (ff_ctx->event_type == FF_EVENT_NETLINK) {
            uevent_env[1] = NULL;
            kobject_uevent_env(&ff_ctx->fp_dev->kobj, KOBJ_CHANGE, uevent_env);
        }  else
            FF_LOGE("unknowed event type:%d", ff_ctx->event_type);
    }

    FF_LOGV("'%s' leave.", __func__);
    return NOTIFY_OK;
}
#endif

static int ff_probe(struct platform_device *pdev)
{
    int ret = 0;
    ff_context_t *ff_ctx = NULL;

    FF_LOGI("'%s' enter.", __func__);
    ff_ctx = kzalloc(sizeof(ff_context_t), GFP_KERNEL);
    if (!ff_ctx) {
        FF_LOGE("kzalloc for ff_context_t fails");
        return -ENOMEM;
    }

    /*parse dts*/
    ff_parse_dt(&pdev->dev, ff_ctx);

    /*variables*/
    ff_ctx->pdev = pdev;
    ff_ctx->feature.ver = 0x0100;
    ff_ctx->feature.fbs_wakelock = true;
    ff_ctx->feature.fbs_power_const = (ff_ctx->b_power_always_on) ? true : false;
    ff_ctx->feature.fbs_screen_onoff = (ff_ctx->b_screen_onoff_event) ? true : false;
    ff_ctx->feature.fbs_pollevt = true;
    ff_ctx->feature.fbs_reset_hl = true;
    ff_ctx->feature.fbs_spiclk = (ff_ctx->b_spiclk) ? true : false;
    ff_ctx->poll_event = FF_POLLEVT_NONE;
    ff_ctx->event_type = FF_EVENT_NETLINK;

    /* Init the interrupt workqueue. */
    INIT_WORK(&ff_ctx->event_work, ff_work_device_event);
    init_waitqueue_head(&ff_ctx->wait_queue_head);

    /* Init the wake lock. */
    wakeup_source_init(&ff_ctx->wake_lock, "ff_wake_lock");
    wakeup_source_init(&ff_ctx->wake_lock_ctl, "ff_wake_lock_ctl");

    ret = ff_register_device(ff_ctx);
    if (ret < 0) {
        FF_LOGE("register devce(focaltech_fp) fails,ret=%d", ret);
        goto err_register_device;
    }

    ret = sysfs_create_group(&pdev->dev.kobj, &ff_attrs_group);
    if (ret < 0) {
        FF_LOGE("sysfs_create_group fails");
        }
    if (ff_ctx->b_screen_onoff_event) {
        ff_ctx->b_screen_on = true;
#if defined(CONFIG_FB)
        ff_ctx->fb_notif.notifier_call = fb_notifier_callback;
        ret = fb_register_client(&ff_ctx->fb_notif);
        if (ret) {
            FF_LOGE("Unable to register fb_notifier,ret=%d", ret); }
#endif
    }

    platform_set_drvdata(pdev, ff_ctx);
    ff_ctx->b_ff_probe = true;
    g_ff_ctx = ff_ctx;

    FF_LOGI("'%s' ff driver probe succeeds.", __func__);
    return 0;

err_register_device:
    wakeup_source_trash(&ff_ctx->wake_lock);
    wakeup_source_trash(&ff_ctx->wake_lock_ctl);

    cancel_work_sync(&ff_ctx->event_work);

    kfree(ff_ctx);
    ff_ctx = NULL;

    FF_LOGI("'%s' leave.", __func__);
    return ret;
}

static void ff_unprobe(ff_context_t *ff_ctx)
{
    FF_LOGI("'%s' enter.", __func__);
    if (ff_ctx && ff_ctx->b_ff_probe) {
        ff_ctx->b_ff_probe = false;
        if (ff_ctx->b_driver_inited) {
            ff_ctl_free_driver(ff_ctx);
            }
        sysfs_remove_group(&ff_ctx->pdev->dev.kobj, &ff_attrs_group);

        /* De-init the wake lock. */
        wakeup_source_trash(&ff_ctx->wake_lock);
        wakeup_source_trash(&ff_ctx->wake_lock_ctl);
        cancel_work_sync(&ff_ctx->event_work);

        if (ff_ctx->b_screen_onoff_event) {
#if defined(CONFIG_FB)
            fb_unregister_client(&ff_ctx->fb_notif);
#endif
        }
    }
    FF_LOGI("'%s' leave.", __func__);
}

static int ff_remove(struct platform_device *pdev)
{
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGI("'%s' enter.", __func__);
    if (ff_ctx && ff_ctx->b_ff_probe) {
        device_destroy(ff_ctx->ff_class, ff_ctx->ff_cdev.dev);
        class_destroy(ff_ctx->ff_class);
        cdev_del(&ff_ctx->ff_cdev);
        unregister_chrdev_region(ff_ctx->ff_cdev.dev, 1);

        sysfs_remove_group(&pdev->dev.kobj, &ff_attrs_group);

        /* De-init the wake lock. */
        wakeup_source_trash(&ff_ctx->wake_lock);
        wakeup_source_trash(&ff_ctx->wake_lock_ctl);
        cancel_work_sync(&ff_ctx->event_work);

        if (ff_ctx->b_screen_onoff_event) {
#if defined(CONFIG_FB)
            fb_unregister_client(&ff_ctx->fb_notif);
#endif
        }

        platform_set_drvdata(pdev, NULL);
        kfree(ff_ctx);
        ff_ctx = NULL;
    }
    FF_LOGI("'%s' leave.", __func__);
    return 0;
}

static const struct of_device_id ff_dt_match[] = {
    {.compatible = "focaltech_fp,fingerprint", },
    {},
};
MODULE_DEVICE_TABLE(of, ff_dt_match);

static struct platform_driver ff_driver = {
    .driver = {
        .name = FF_DRV_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(ff_dt_match),
    },
    .probe = ff_probe,
    .remove = ff_remove,
};


static int __init ff_driver_init(void)
{
    int ret = 0;
    FF_LOGI("'%s' enter.", __func__);
    if (FP_FT_9396 != get_fpsensor_type()) {
        FF_LOGE("Focaltech IC not matched! exit ff_driver_init\n!");
        ret = -EINVAL;
        return ret;
    }
    FF_LOGI("Focaltech IC matched! start register111 ...\n");
    ret = platform_driver_register(&ff_driver);
    if (ret < 0) {
        FF_LOGE("ff platform driver init failed!");
        return ret;
    }
/*#ifdef CONFIG_FINGERPRINT_FOCALTECH_REE    */
    ret = ff_spi_init();
    if (ret < 0) {
        FF_LOGE("ff spi driver init failed!");
        platform_driver_unregister(&ff_driver);
        return ret;
    }
/*#endif    */
    FF_LOGI("'%s' leave.", __func__);
    return 0;
}

static void __exit ff_driver_exit(void)
{
    FF_LOGI("'%s' enter.", __func__);
    platform_driver_unregister(&ff_driver);
/*#ifdef CONFIG_FINGERPRINT_FOCALTECH_SPI_SUPPORT    */
    ff_spi_exit();
/*#endif    */
    FF_LOGI("'%s' leave.", __func__);
}

late_initcall(ff_driver_init);
module_exit(ff_driver_exit);

MODULE_AUTHOR("FocalTech Fingerprint Department");
MODULE_DESCRIPTION("FocalTech Fingerprint Driver");
MODULE_LICENSE("GPL v2");
