// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

#include <linux/module.h>
/*#include <linux/init.h>*/
#include <linux/errno.h>
#include <linux/spi/spi.h>
#include <linux/miscdevice.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include "ff_log.h"
#include "ff_core.h"

/*****************************************************************************
* Private constant and macro definitions using #define
*****************************************************************************/
#undef LOG_TAG
#define LOG_TAG "focaltech_spi"

#define FF_SPI_DRIVER_NAME            "focaltech_fpspi"


/*****************************************************************************
* static variable or structure
*****************************************************************************/
struct ff_spi_context {
    struct spi_device *spi;
    struct miscdevice mdev;
    struct mutex bus_lock;
    u8 *bus_tx_buf;
    u8 *bus_rx_buf;
    bool b_misc;
};

struct ff_spi_sync_data {
    char *tx;
    char *rx;
    unsigned int size;
};

struct ff_spi_sync_data_2 {
    char *tx;
    char *rx;
    unsigned int tx_len;
    unsigned int rx_len;
};

#define FF_IOCTL_SPI_DEVICE         0xC6
#define FF_IOCTL_SPI_SYNC           _IOWR(FF_IOCTL_SPI_DEVICE, 0x01, struct ff_spi_sync_data)
#define FF_IOCTL_GET_SPI_MODE       _IOR(FF_IOCTL_SPI_DEVICE, 0x02, u32)
#define FF_IOCTL_SET_SPI_MODE       _IOW(FF_IOCTL_SPI_DEVICE, 0x02, u32)
#define FF_IOCTL_GET_SPI_SPEED      _IOR(FF_IOCTL_SPI_DEVICE, 0x03, u32)
#define FF_IOCTL_SET_SPI_SPEED      _IOW(FF_IOCTL_SPI_DEVICE, 0x03, u32)
#define FF_IOCTL_SPI_SYNC_2         _IOWR(FF_IOCTL_SPI_DEVICE, 0x10, struct ff_spi_sync_data_2)


/*****************************************************************************
* Global variable or extern global variabls/functions
*****************************************************************************/

/* spi interface */
static int ff_spi_sync(struct ff_spi_context *spi_ctx, u8 *tx_buf, u8 *rx_buf, u32 len)
{
    int ret = 0;
    struct spi_message msg;
    struct spi_transfer xfer = {
        .tx_buf = tx_buf,
        .rx_buf = rx_buf,
        .len    = len,
    };

    if (!spi_ctx) {
        FF_LOGE("spi_ctx is null");
        return -ENODATA;
    }

    mutex_lock(&spi_ctx->bus_lock);
    spi_message_init(&msg);
    spi_message_add_tail(&xfer, &msg);

    ret = spi_sync(spi_ctx->spi, &msg);
    if (ret) {
        FF_LOGE("spi_sync fail,ret:%d", ret);
    }

    mutex_unlock(&spi_ctx->bus_lock);
    return ret;
}

static int ff_ioctl_spi_sync(struct ff_spi_context *spi_ctx, unsigned long arg)
{
    int ret = 0;
    struct ff_spi_sync_data spi_data;
    u8 *txbuf = spi_ctx->bus_tx_buf;
    u8 *rxbuf = spi_ctx->bus_rx_buf;

    FF_LOGV("'%s' enter.", __func__);
    if (copy_from_user(&spi_data, (void *)arg, sizeof(struct ff_spi_sync_data))) {
        FF_LOGE("copy spi_sync data from userspace fail");
        return -EFAULT;
    }
    if (!spi_data.tx || !spi_data.size) {
        FF_LOGE("tx/size(%d) from userspace are invalid", spi_data.size);
        return -EINVAL;
    }

    if (!txbuf || !rxbuf) {
        FF_LOGE("spi tx/rx buf is null");
        return -EINVAL;
    }

    if (spi_data.size > PAGE_SIZE) {
        txbuf = kzalloc(spi_data.size, GFP_KERNEL);
        rxbuf = kzalloc(spi_data.size, GFP_KERNEL);
        if (!txbuf || !rxbuf) {
            FF_LOGE("kzalloc memory(size:%d) for spi tx/rx buffer fail", spi_data.size);
            ret = -ENOMEM;
            goto spi_sync_err;
        }
    } else {
        memset(txbuf, 0, spi_data.size);
        memset(rxbuf, 0, spi_data.size);
    }

    if (copy_from_user(txbuf, spi_data.tx, spi_data.size)) {
        FF_LOGE("copy spi tx data from userspace fail");
        ret = -EFAULT;
        goto spi_sync_err;
    }

    ret = ff_spi_sync(spi_ctx, txbuf, rxbuf, spi_data.size);
    if (ret) {
        FF_LOGE("spi sync fail");
        ret = -EIO;
        goto spi_sync_err;
    }

    if (spi_data.rx && copy_to_user(spi_data.rx, rxbuf, spi_data.size)) {
        FF_LOGE("copy spi rx data to userspace fail");
        ret = -EFAULT;
        goto spi_sync_err;
    }

    ret = 0;
spi_sync_err:
    if (spi_data.size > PAGE_SIZE) {
        kfree(txbuf);
        kfree(rxbuf);
    }
    FF_LOGV("'%s' leave.", __func__);
    return ret;
}

static int ff_ioctl_spi_sync_2(struct ff_spi_context *spi_ctx, unsigned long arg)
{
    int ret = 0;
    struct ff_spi_sync_data_2 spi_data;
    u8 *txbuf = spi_ctx->bus_tx_buf;
    u8 *rxbuf = spi_ctx->bus_rx_buf;
    u32 size = 0;

    FF_LOGV("'%s' enter.", __func__);
    if (copy_from_user(&spi_data, (void *)arg, sizeof(struct ff_spi_sync_data_2))) {
        FF_LOGE("copy spi_sync data from userspace fail");
        return -EFAULT;
    }

    size = spi_data.tx_len + spi_data.rx_len;
    if ((!spi_data.tx && !spi_data.rx) || !size) {
        FF_LOGE("tx_rx/size(%d) from userspace are invalid", size);
        return -EINVAL;
    }

    if (!txbuf || !rxbuf) {
        FF_LOGE("spi tx/rx buf is null");
        return -EINVAL;
    }

    if (size > PAGE_SIZE) {
        txbuf = kzalloc(size, GFP_KERNEL);
        rxbuf = kzalloc(size, GFP_KERNEL);
        if (!txbuf || !rxbuf) {
            FF_LOGE("kzalloc memory(size:%d) for spi tx/rx buffer fail", size);
            ret = -ENOMEM;
            goto spi_sync2_err;
        }
    } else {
        memset(txbuf, 0, size);
        memset(rxbuf, 0, size);
    }

    if (spi_data.tx && spi_data.tx_len && \
        copy_from_user(txbuf, spi_data.tx, spi_data.tx_len)) {
        FF_LOGE("copy spi tx data from userspace fail");
        ret = -EFAULT;
        goto spi_sync2_err;
    }

    ret = ff_spi_sync(spi_ctx, txbuf, rxbuf, size);
    if (ret) {
        FF_LOGE("spi sync fail");
        ret = -EIO;
        goto spi_sync2_err;
    }

    if (spi_data.rx && spi_data.rx_len && \
        copy_to_user(spi_data.rx, rxbuf + spi_data.tx_len, spi_data.rx_len)) {
        FF_LOGE("copy spi rx data to userspace fail");
        ret = -EFAULT;
        goto spi_sync2_err;
    }

    ret = 0;
spi_sync2_err:
    if (size > PAGE_SIZE) {
        kfree(txbuf);
        kfree(rxbuf);
    }
    FF_LOGV("'%s' leave.", __func__);
    return ret;
}

static int ff_ioctl_set_spi_mode(struct ff_spi_context *spi_ctx, unsigned long arg)
{
    int ret = 0;
    u32 mode = 0;

    FF_LOGD("'%s' enter.", __func__);
    ret = __get_user(mode, (__u32 __user *)arg);
    FF_LOGD("set spi mode:%d", mode);
    if ((ret == 0) && (spi_ctx->spi->mode != mode)) {
        spi_ctx->spi->mode = mode;
        ret = spi_setup(spi_ctx->spi);
        if (ret < 0) {
            FF_LOGE("spi setup fail, ret:%d", ret); }
        else {
            FF_LOGI("set spi mode:0x%x", (int)spi_ctx->spi->mode); }
    }
    FF_LOGD("'%s' leave.", __func__);
    return ret;
}

static int ff_ioctl_set_spi_speed(struct ff_spi_context *spi_ctx, unsigned long arg)
{
    int ret = 0;
    u32 speed = 0;

    FF_LOGD("'%s' enter.", __func__);
    ret = __get_user(speed, (__u32 __user *)arg);
    FF_LOGD("set spi speed:%d", speed);
    if ((ret == 0) && (spi_ctx->spi->max_speed_hz != speed)) {
        spi_ctx->spi->max_speed_hz = speed;
        ret = spi_setup(spi_ctx->spi);
        if (ret < 0) {
            FF_LOGE("spi setup fail, ret:%d", ret); }
        else {
            FF_LOGI("set spi speed:%d", spi_ctx->spi->max_speed_hz); }
    }
    FF_LOGD("'%s' leave.", __func__);
    return ret;
}

static int ff_spi_open(struct inode *inode, struct file *filp)
{
    struct ff_spi_context *spi_ctx = container_of(filp->private_data, struct ff_spi_context, mdev);

    FF_LOGD("'%s' enter.", __func__);
    if (!spi_ctx) {
        FF_LOGE("spi_ctx is null");
        return -ENODATA;
    }

    if (!spi_ctx->bus_tx_buf) {
        spi_ctx->bus_tx_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
        if (NULL == spi_ctx->bus_tx_buf) {
            FF_LOGE("failed to allocate memory for bus_tx_buf");
            return -ENOMEM;
        }
    }

    if (!spi_ctx->bus_rx_buf) {
        spi_ctx->bus_rx_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
        if (NULL == spi_ctx->bus_rx_buf) {
            FF_LOGE("failed to allocate memory for bus_rx_buf");
            kfree(spi_ctx->bus_tx_buf);
            spi_ctx->bus_tx_buf = NULL;
            return -ENOMEM;
        }
    }

    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

static int ff_spi_close(struct inode *inode, struct file *filp)
{
    struct ff_spi_context *spi_ctx = container_of(filp->private_data, struct ff_spi_context, mdev);

    FF_LOGD("'%s' enter.", __func__);
    if (!spi_ctx) {
        FF_LOGE("spi_ctx is null");
        return -ENODATA;
    }
    if (spi_ctx->bus_tx_buf) {
        kfree(spi_ctx->bus_tx_buf);
        spi_ctx->bus_tx_buf = NULL;
    }

    if (spi_ctx->bus_rx_buf) {
        kfree(spi_ctx->bus_rx_buf);
        spi_ctx->bus_rx_buf = NULL;
    }

    FF_LOGD("'%s' leave.", __func__);
    return 0;
}

static long ff_spi_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    struct ff_spi_context *spi_ctx = container_of(filp->private_data, struct ff_spi_context, mdev);

    FF_LOGV("'%s' enter.", __func__);
    if (!spi_ctx) {
        FF_LOGE("spi_ctx is null");
        return -ENODATA;
    }

    FF_LOGV("ioctl cmd:%x,arg:%lx,%x", (int)cmd, arg, FF_IOCTL_SPI_SYNC);
    switch (cmd) {
    case FF_IOCTL_SPI_SYNC:
        ret = ff_ioctl_spi_sync(spi_ctx, arg);
        break;
    case FF_IOCTL_GET_SPI_MODE:
        ret = __put_user(spi_ctx->spi->mode, (__u32 __user *)arg);
        break;
    case FF_IOCTL_GET_SPI_SPEED:
        ret = __put_user(spi_ctx->spi->max_speed_hz, (__u32 __user *)arg);
        break;
    case FF_IOCTL_SET_SPI_MODE:
        ret = ff_ioctl_set_spi_mode(spi_ctx, arg);
        break;
    case FF_IOCTL_SET_SPI_SPEED:
        ret = ff_ioctl_set_spi_speed(spi_ctx, arg);
        break;
    case FF_IOCTL_SPI_SYNC_2:
        ret = ff_ioctl_spi_sync_2(spi_ctx, arg);
        break;
    default:
        FF_LOGI("unkown ioctl cmd(0x%x)", (int)cmd);
        break;
    }

    FF_LOGV("'%s' leave.", __func__);
    return ret;
}

static struct file_operations ff_spi_fops = {
    .open = ff_spi_open,
    .release = ff_spi_close,
    .unlocked_ioctl = ff_spi_ioctl,
};

#ifdef FF_SPI_PINCTRL
static void ff_spi_set_active(struct device *dev)
{
    int ret = 0;
    struct pinctrl *pinctrl;
    struct pinctrl_state *pins_spi_active;
    FF_LOGV("'%s' enter.", __func__);
    pinctrl = devm_pinctrl_get(dev);
    if (IS_ERR(pinctrl)) {
        ret = PTR_ERR(pinctrl);
        FF_LOGD("no SPI pinctrl,ret=%d", ret);
    }

    pins_spi_active = pinctrl_lookup_state(pinctrl, "ffspi_pins_active");
    if (IS_ERR(pins_spi_active)) {
        ret = PTR_ERR(pins_spi_active);
        FF_LOGE("Cannot find pinctrl ffspi_active,ret=%d", ret);
    }

    pinctrl_select_state(pinctrl, pins_spi_active);
    FF_LOGV("'%s' leave.", __func__);
}
#endif

static int ff_spi_probe(struct spi_device *spi)
{
    int ret = 0;
    struct ff_spi_context *spi_ctx = NULL;
    ff_context_t *ff_ctx = g_ff_ctx;

    FF_LOGI("'%s' enter.", __func__);
    ff_ctx->spi = spi;
    ff_ctx->b_spiclk_enabled = 0;

#ifdef FF_SPI_PINCTRL
    /*set SPI pinctrl*/
    ff_spi_set_active(&spi->dev);
#endif

    if (ff_ctx->b_read_chipid || ff_ctx->b_ree) {
        FF_LOGI("need configure spi for communication");
        /*spi_setup*/
        spi->mode = SPI_MODE_0;
        spi->bits_per_word = 8;
        spi->max_speed_hz = 5000000;
        ret = spi_setup(spi);
        if (ret < 0) {
            FF_LOGE("spi setup fail");
            return ret;
        }

        /* malloc memory for global struct variable */
        spi_ctx = (struct ff_spi_context *)kzalloc(sizeof(*spi_ctx), GFP_KERNEL);
        if (!spi_ctx) {
            FF_LOGE("allocate memory for ff_spi_context fail");
            return -ENOMEM;
        }

        /*init*/
        spi_ctx->spi = spi;
        spi_set_drvdata(spi, spi_ctx);
        mutex_init(&spi_ctx->bus_lock);

        if (ff_ctx->b_ree) {
            FF_LOGI("need add spi ioctrl for ree");
            /*register misc_device*/
            spi_ctx->mdev.minor = MISC_DYNAMIC_MINOR;
            spi_ctx->mdev.name = FF_SPI_DRIVER_NAME;
            spi_ctx->mdev.fops = &ff_spi_fops;
            ret = misc_register(&spi_ctx->mdev);
            if (ret < 0) {
                FF_LOGE("misc_register(%s) fail", FF_SPI_DRIVER_NAME);
                kfree(spi_ctx);
                spi_ctx = NULL;
                spi_set_drvdata(spi, NULL);
                return ret;
            }
            spi_ctx->b_misc = true;
        }
    }

    FF_LOGI("'%s' leave.", __func__);
    return 0;
}

static int ff_spi_remove(struct spi_device *spi)
{
    struct ff_spi_context *spi_ctx = spi_get_drvdata(spi);

    FF_LOGI("'%s' enter.", __func__);
    if (spi_ctx) {
        if (spi_ctx->b_misc) {
            misc_deregister(&spi_ctx->mdev); }
        mutex_destroy(&spi_ctx->bus_lock);
        if (spi_ctx->bus_tx_buf) {
            kfree(spi_ctx->bus_tx_buf); }
        if (spi_ctx->bus_rx_buf) {
            kfree(spi_ctx->bus_rx_buf); }
        kfree(spi_ctx);
        spi_ctx = NULL;
    }

    spi_set_drvdata(spi, NULL);
    FF_LOGI("'%s' leave.", __func__);
}

static const struct of_device_id ff_spi_dt_match[] = {
    {.compatible = "mediatek,fingerspi-fp", },
    {.compatible = "oplus,oplus_fp", },
    {.compatible = "focaltech,fpspi", },
    {},
};
MODULE_DEVICE_TABLE(of, ff_spi_dt_match);

static struct spi_driver ff_spi_driver = {
    .driver = {
        .name = FF_SPI_DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(ff_spi_dt_match),
    },
    .probe = ff_spi_probe,
    .remove = ff_spi_remove,
};

int ff_spi_init(void)
{
    int ret = 0;
    FF_LOGI("'%s' enter.", __func__);

    ret = spi_register_driver(&ff_spi_driver);
    if (ret != 0) {
        FF_LOGE("ff spi driver init failed!");
    }
    FF_LOGI("'%s' leave.", __func__);
    return ret;
}

void ff_spi_exit(void)
{
    FF_LOGI("'%s' enter.", __func__);
    spi_unregister_driver(&ff_spi_driver);
    FF_LOGI("'%s' leave.", __func__);
}
