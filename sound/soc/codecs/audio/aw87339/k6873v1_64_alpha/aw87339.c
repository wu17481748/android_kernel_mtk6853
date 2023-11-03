/*
 * aw87319_audio.c   aw87319 pa module
 *
 * Version: v1.2.2
 *
 * Copyright (c) 2017 AWINIC Technology CO., LTD
 *
 *  Author: Nick Li <liweilei@awinic.com.cn>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/gameport.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/module.h>

/*******************************************************************************
 * aw87339 marco
 ******************************************************************************/
#define OPLUS_BUG_COMPATIBILITY
#define AW87339_I2C_NAME_SPK    "aw87339_pa_spk"
#define AW87339_I2C_NAME_REC    "aw87339_pa_rec"

#define AW87339_DRIVER_VERSION  "v1.2.2"

#define AWINIC_CFG_UPDATE_DELAY

#define AW_I2C_RETRIES 5
#define AW_I2C_RETRY_DELAY 2
#define AW_READ_CHIPID_RETRIES 5
#define AW_READ_CHIPID_RETRY_DELAY 2

#define aw87339_REG_CHIPID      0x00
#define aw87339_REG_SYSCTRL     0x01
#define aw87339_REG_MODECTRL    0x02
#define aw87339_REG_CPOVP       0x03
#define aw87339_REG_CPP         0x04
#define aw87339_REG_GAIN        0x05
#define aw87339_REG_AGC3_PO     0x06
#define aw87339_REG_AGC3        0x07
#define aw87339_REG_AGC2_PO     0x08
#define aw87339_REG_AGC2        0x09
#define aw87339_REG_AGC1        0x0A

#define aw87339_CHIP_DISABLE    0x0c

#define REG_NONE_ACCESS         0
#define REG_RD_ACCESS           1 << 0
#define REG_WR_ACCESS           1 << 1
#define aw87339_REG_MAX         0x0F

const unsigned char aw87339_reg_access[aw87339_REG_MAX] = {
    [aw87339_REG_CHIPID  ] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_SYSCTRL ] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_MODECTRL] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_CPOVP   ] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_CPP     ] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_GAIN    ] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_AGC3_PO ] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_AGC3    ] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_AGC2_PO ] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_AGC2    ] = REG_RD_ACCESS|REG_WR_ACCESS,
    [aw87339_REG_AGC1    ] = REG_RD_ACCESS|REG_WR_ACCESS,
};


/*******************************************************************************
 * aw87339 variable
 ******************************************************************************/
 struct aw87339_container{
    int len;
    unsigned char data[];
};
struct aw87339_t{
    struct i2c_client *i2c_client;
    int reset_gpio;
    unsigned char init_flag;
    unsigned char hwen_flag;
    unsigned char kspk_cfg_update_flag;
    unsigned char drcv_cfg_update_flag;
    unsigned char abrcv_cfg_update_flag;
    unsigned char rcvspk_cfg_update_flag;
    struct hrtimer cfg_timer;
    struct work_struct cfg_work;
    struct aw87339_container *aw87339_kspk_cnt;
    struct aw87339_container *aw87339_drcv_cnt;
    struct aw87339_container *aw87339_abrcv_cnt;
    struct aw87339_container *aw87339_rcvspk_cnt;
};
struct aw87339_t *aw87339_spk = NULL;
struct aw87339_t *aw87339_rec = NULL;

#ifdef OPLUS_BUG_COMPATIBILITY
int aw87339_voice_status = 0;
int aw87339_spk_low_voltage_status = 0;
static unsigned char aw87339_kspk_cfg_voice[] = {
0x39, 0x0E, 0xA3, 0x06, 0x05, 0x0C, 0x0E, 0xCA, 0x08, 0x04, 0x9E
};
#endif /* OPLUS_BUG_COMPATIBILITY */

static char *aw87339_kspk_name_spk = "aw87339_kspk_spk.bin";
static char *aw87339_drcv_name_spk = "aw87339_drcv_spk.bin";
static char *aw87339_abrcv_name_spk = "aw87339_abrcv_spk.bin";
static char *aw87339_rcvspk_name_spk = "aw87339_rcvspk_spk.bin";
static char *aw87339_kspk_name_rec = "aw87339_kspk_rcv.bin";
static char *aw87339_drcv_name_rec = "aw87339_drcv_rcv.bin";
static char *aw87339_abrcv_name_rec = "aw87339_abrcv_rcv.bin";
static char *aw87339_rcvspk_name_rec = "aw87339_rcvspk_rcv.bin";

static unsigned char aw87339_kspk_cfg_default[] = {
0x39, 0x0E, 0xA3, 0x06, 0x05, 0x10, 0x07, 0x52, 0x06, 0x08, 0x96
};
static unsigned char aw87339_drcv_cfg_default[] = {
0x39, 0x0A, 0xAB, 0x06, 0x05, 0x00, 0x0f, 0x52, 0x09, 0x08, 0x97
};
static unsigned char aw87339_abrcv_cfg_default[] = {
0x39, 0x0A, 0xAF, 0x06, 0x05, 0x00, 0x0f, 0x52, 0x09, 0x08, 0x97
};
static unsigned char aw87339_rcvspk_cfg_default[] = {
0x39, 0x0E, 0xB3, 0x06, 0x05, 0x00, 0x07, 0x52, 0x06, 0x08, 0x96
};

static int aw87339_spk_ctrl;

/*******************************************************************************
 * aw87339 functions
 ******************************************************************************/
unsigned char aw87339_audio_off(struct aw87339_t *aw87339);
unsigned char aw87339_audio_kspk(struct aw87339_t *aw87339);
unsigned char aw87339_audio_drcv(struct aw87339_t *aw87339);
unsigned char aw87339_audio_abrcv(struct aw87339_t *aw87339);
unsigned char aw87339_audio_rcvspk(struct aw87339_t *aw87339);
/*******************************************************************************
 * i2c write and read
 ******************************************************************************/
static int i2c_write_reg(unsigned char reg_addr, unsigned char reg_data, struct aw87339_t *aw87339)
{
    int ret = -1;
    unsigned char cnt = 0;

    while(cnt < AW_I2C_RETRIES) {
        ret = i2c_smbus_write_byte_data(aw87339->i2c_client, reg_addr, reg_data);
        if(ret < 0) {
            pr_err("%s: i2c_write cnt=%d error=%d\n", __func__, cnt, ret);
        } else {
            break;
        }
        cnt ++;
        msleep(AW_I2C_RETRY_DELAY);
    }

    return ret;
}

static unsigned char i2c_read_reg(unsigned char reg_addr, struct aw87339_t *aw87339)
{
    int ret = 1;
    unsigned char cnt = 0;

    while(cnt < AW_I2C_RETRIES) {
        ret = i2c_smbus_read_byte_data(aw87339->i2c_client, reg_addr);
        if(ret < 0) {
            pr_err("%s: i2c_read cnt=%d error=%d\n", __func__, cnt, ret);
        } else {
            break;
        }
        cnt ++;
        msleep(AW_I2C_RETRY_DELAY);
    }

    return ret;
}

/*******************************************************************************
 * aw87339 hardware control
 ******************************************************************************/
unsigned char aw87339_hw_on(struct aw87339_t *aw87339)
{
    pr_info("%s enter\n", __func__);

    if (aw87339 && gpio_is_valid(aw87339->reset_gpio)) {
        gpio_set_value_cansleep(aw87339->reset_gpio, 0);
        msleep(2);
        gpio_set_value_cansleep(aw87339->reset_gpio, 1);
        msleep(2);
        aw87339->hwen_flag = 1;
    } else {
        dev_err(&aw87339->i2c_client->dev, "%s:  failed\n", __func__);
    }

    return 0;
}

unsigned char aw87339_hw_off(struct aw87339_t *aw87339)
{
    pr_info("%s enter\n", __func__);

    if (aw87339 && gpio_is_valid(aw87339->reset_gpio)) {
        gpio_set_value_cansleep(aw87339->reset_gpio, 0);
        msleep(2);
        aw87339->hwen_flag = 0;
    } else {
        dev_err(&aw87339->i2c_client->dev, "%s:  failed\n", __func__);
    }
    return 0;
}


/*******************************************************************************
 * aw87339 control interface
 ******************************************************************************/
unsigned char aw87339_kspk_reg_val(unsigned char reg, struct aw87339_t *aw87339)
{
    if(aw87339->kspk_cfg_update_flag) {
#ifdef OPLUS_BUG_COMPATIBILITY
        if (aw87339_voice_status > 0) {
            pr_info("%s: aw87339 return voice reg 0x%x 0x%x\n", __func__, reg, aw87339_kspk_cfg_voice[reg]);
            return aw87339_kspk_cfg_voice[reg];
        } else {
            return *(aw87339->aw87339_kspk_cnt->data+reg);
        }
#else /* OPLUS_BUG_COMPATIBILITY */
        return *(aw87339->aw87339_kspk_cnt->data+reg);
#endif /* OPLUS_BUG_COMPATIBILITY */
    } else {
        return aw87339_kspk_cfg_default[reg];
    }
}

unsigned char aw87339_drcv_reg_val(unsigned char reg, struct aw87339_t *aw87339)
{
    if(aw87339->drcv_cfg_update_flag) {
        return *(aw87339->aw87339_drcv_cnt->data+reg);
    } else {
        return aw87339_drcv_cfg_default[reg];
    }
}

unsigned char aw87339_abrcv_reg_val(unsigned char reg, struct aw87339_t *aw87339)
{
    if(aw87339->abrcv_cfg_update_flag) {
        return *(aw87339->aw87339_abrcv_cnt->data+reg);
    } else {
        return aw87339_abrcv_cfg_default[reg];
    }
}

unsigned char aw87339_rcvspk_reg_val(unsigned char reg, struct aw87339_t *aw87339)
{
    if(aw87339->rcvspk_cfg_update_flag) {
        return *(aw87339->aw87339_rcvspk_cnt->data+reg);
    } else {
        return aw87339_rcvspk_cfg_default[reg];
    }
}

#ifdef OPLUS_BUG_COMPATIBILITY

void aw87339_audio_spk_low_voltage_status(int bStatus)
{
    aw87339_spk_low_voltage_status = bStatus;

    if(aw87339_spk == NULL) {
        pr_err("%s: aw87339 is NULL\n", __func__);
        return;
    }

    if (aw87339_spk->hwen_flag == 1) {
        if (aw87339_spk_low_voltage_status > 0) {
            pr_info("%s: aw87339 write vol reg 0 when playing\n", __func__);
            i2c_write_reg(aw87339_REG_CPOVP, 0x00, aw87339_spk);
        } else {
            pr_info("%s: aw87339 write vol reg normal when playing\n", __func__);
            i2c_write_reg(aw87339_REG_CPOVP, aw87339_kspk_reg_val(aw87339_REG_CPOVP,aw87339_spk), aw87339_spk);
        }
    }
    return;
}

void aw87339_voice_setting(int bStatus)
{
    aw87339_voice_status = bStatus;

    pr_info("%s: aw87339 voice status %d\n", __func__, aw87339_voice_status);
    return;
}

#endif /* OPLUS_BUG_COMPATIBILITY */

unsigned char aw87339_audio_kspk(struct aw87339_t *aw87339)
{
    if(aw87339 == NULL) {
        pr_err("%s: aw87339 is NULL\n", __func__);
        return 1;
    }

    if(!aw87339->init_flag) {
        pr_err("%s: aw87339 init failed\n", __func__);
        return 1;
    }

    if(!aw87339->hwen_flag) {
        aw87339_hw_on(aw87339);
    }

    i2c_write_reg(aw87339_REG_SYSCTRL , aw87339_kspk_reg_val(aw87339_REG_SYSCTRL,aw87339 )&0xF7, aw87339);
    i2c_write_reg(aw87339_REG_MODECTRL, aw87339_kspk_reg_val(aw87339_REG_MODECTRL,aw87339), aw87339);
#ifdef OPLUS_BUG_COMPATIBILITY
    if (aw87339_spk_low_voltage_status > 0) {
        i2c_write_reg(aw87339_REG_CPOVP, 0x00, aw87339);
    } else {
        i2c_write_reg(aw87339_REG_CPOVP, aw87339_kspk_reg_val(aw87339_REG_CPOVP,aw87339), aw87339);
    }
#else /* OPLUS_BUG_COMPATIBILITY */
    i2c_write_reg(aw87339_REG_CPOVP   , aw87339_kspk_reg_val(aw87339_REG_CPOVP,aw87339   ), aw87339);
#endif /* OPLUS_BUG_COMPATIBILITY */
    i2c_write_reg(aw87339_REG_CPP     , aw87339_kspk_reg_val(aw87339_REG_CPP,aw87339     ), aw87339);
    i2c_write_reg(aw87339_REG_GAIN    , aw87339_kspk_reg_val(aw87339_REG_GAIN,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC3_PO , aw87339_kspk_reg_val(aw87339_REG_AGC3_PO,aw87339 ), aw87339);
    i2c_write_reg(aw87339_REG_AGC3    , aw87339_kspk_reg_val(aw87339_REG_AGC3,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC2_PO , aw87339_kspk_reg_val(aw87339_REG_AGC2_PO,aw87339 ), aw87339);
    i2c_write_reg(aw87339_REG_AGC2    , aw87339_kspk_reg_val(aw87339_REG_AGC2,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC1    , aw87339_kspk_reg_val(aw87339_REG_AGC1,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_SYSCTRL , aw87339_kspk_reg_val(aw87339_REG_SYSCTRL,aw87339 ), aw87339);

    return 0;
}
unsigned char aw87339_audio_spk_if_kspk(void)
{
    return aw87339_audio_kspk(aw87339_spk);
}
unsigned char aw87339_audio_rcv_if_kspk(void)
{
    return aw87339_audio_kspk(aw87339_rec);
}
unsigned char aw87339_audio_drcv(struct aw87339_t *aw87339)
{
    if(aw87339 == NULL) {
        pr_err("%s: aw87339 is NULL\n", __func__);
        return 1;
    }

    if(!aw87339->init_flag) {
        pr_err("%s: aw87339 init failed\n", __func__);
        return 1;
    }

    if(!aw87339->hwen_flag) {
        aw87339_hw_on(aw87339);
    }

    i2c_write_reg(aw87339_REG_SYSCTRL , aw87339_drcv_reg_val(aw87339_REG_SYSCTRL,aw87339 )&0xF7, aw87339);
    i2c_write_reg(aw87339_REG_MODECTRL, aw87339_drcv_reg_val(aw87339_REG_MODECTRL,aw87339), aw87339);
    i2c_write_reg(aw87339_REG_CPOVP   , aw87339_drcv_reg_val(aw87339_REG_CPOVP,aw87339   ), aw87339);
    i2c_write_reg(aw87339_REG_CPP     , aw87339_drcv_reg_val(aw87339_REG_CPP,aw87339     ), aw87339);
    i2c_write_reg(aw87339_REG_GAIN    , aw87339_drcv_reg_val(aw87339_REG_GAIN,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC3_PO , aw87339_drcv_reg_val(aw87339_REG_AGC3_PO,aw87339 ), aw87339);
    i2c_write_reg(aw87339_REG_AGC3    , aw87339_drcv_reg_val(aw87339_REG_AGC3,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC2_PO , aw87339_drcv_reg_val(aw87339_REG_AGC2_PO,aw87339 ), aw87339);
    i2c_write_reg(aw87339_REG_AGC2    , aw87339_drcv_reg_val(aw87339_REG_AGC2,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC1    , aw87339_drcv_reg_val(aw87339_REG_AGC1,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_SYSCTRL , aw87339_drcv_reg_val(aw87339_REG_SYSCTRL,aw87339 ), aw87339);

    return 0;
}
unsigned char aw87339_audio_rcv_if_drcv(void)
{
    return aw87339_audio_drcv(aw87339_rec);
}
unsigned char aw87339_audio_abrcv(struct aw87339_t *aw87339)
{
    if(aw87339 == NULL) {
        pr_err("%s: aw87339 is NULL\n", __func__);
        return 1;
    }

    if(!aw87339->init_flag) {
        pr_err("%s: aw87339 init failed\n", __func__);
        return 1;
    }

    if(!aw87339->hwen_flag) {
        aw87339_hw_on(aw87339);
    }

    i2c_write_reg(aw87339_REG_SYSCTRL , aw87339_abrcv_reg_val(aw87339_REG_SYSCTRL,aw87339 )&0xF7, aw87339);
    i2c_write_reg(aw87339_REG_MODECTRL, aw87339_abrcv_reg_val(aw87339_REG_MODECTRL,aw87339), aw87339);
    i2c_write_reg(aw87339_REG_CPOVP   , aw87339_abrcv_reg_val(aw87339_REG_CPOVP,aw87339   ), aw87339);
    i2c_write_reg(aw87339_REG_CPP     , aw87339_abrcv_reg_val(aw87339_REG_CPP,aw87339     ), aw87339);
    i2c_write_reg(aw87339_REG_GAIN    , aw87339_abrcv_reg_val(aw87339_REG_GAIN,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC3_PO , aw87339_abrcv_reg_val(aw87339_REG_AGC3_PO,aw87339 ), aw87339);
    i2c_write_reg(aw87339_REG_AGC3    , aw87339_abrcv_reg_val(aw87339_REG_AGC3,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC2_PO , aw87339_abrcv_reg_val(aw87339_REG_AGC2_PO,aw87339 ), aw87339);
    i2c_write_reg(aw87339_REG_AGC2    , aw87339_abrcv_reg_val(aw87339_REG_AGC2,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC1    , aw87339_abrcv_reg_val(aw87339_REG_AGC1,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_SYSCTRL , aw87339_abrcv_reg_val(aw87339_REG_SYSCTRL,aw87339 ), aw87339);

    return 0;
}

unsigned char aw87339_audio_rcvspk(struct aw87339_t *aw87339)
{
    if(aw87339 == NULL) {
        pr_err("%s: aw87339 is NULL\n", __func__);
        return 1;
    }

    if(!aw87339->init_flag) {
        pr_err("%s: aw87339 init failed\n", __func__);
        return 1;
    }

    if(!aw87339->hwen_flag) {
        aw87339_hw_on(aw87339);
    }

    i2c_write_reg(aw87339_REG_SYSCTRL , aw87339_rcvspk_reg_val(aw87339_REG_SYSCTRL,aw87339 )&0xF7, aw87339);
    i2c_write_reg(aw87339_REG_MODECTRL, aw87339_rcvspk_reg_val(aw87339_REG_MODECTRL,aw87339), aw87339);
    i2c_write_reg(aw87339_REG_CPOVP   , aw87339_rcvspk_reg_val(aw87339_REG_CPOVP,aw87339   ), aw87339);
    i2c_write_reg(aw87339_REG_CPP     , aw87339_rcvspk_reg_val(aw87339_REG_CPP,aw87339     ), aw87339);
    i2c_write_reg(aw87339_REG_GAIN    , aw87339_rcvspk_reg_val(aw87339_REG_GAIN,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC3_PO , aw87339_rcvspk_reg_val(aw87339_REG_AGC3_PO,aw87339 ), aw87339);
    i2c_write_reg(aw87339_REG_AGC3    , aw87339_rcvspk_reg_val(aw87339_REG_AGC3,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC2_PO , aw87339_rcvspk_reg_val(aw87339_REG_AGC2_PO,aw87339 ), aw87339);
    i2c_write_reg(aw87339_REG_AGC2    , aw87339_rcvspk_reg_val(aw87339_REG_AGC2,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_AGC1    , aw87339_rcvspk_reg_val(aw87339_REG_AGC1,aw87339    ), aw87339);
    i2c_write_reg(aw87339_REG_SYSCTRL , aw87339_rcvspk_reg_val(aw87339_REG_SYSCTRL,aw87339 ), aw87339);

    return 0;
}

unsigned char aw87339_audio_off(struct aw87339_t *aw87339)
{
    if(aw87339 == NULL) {
        pr_err("%s: aw87339 is NULL\n", __func__);
        return 1;
    }

    if(!aw87339->init_flag) {
        pr_err("%s: aw87339 init failed\n", __func__);
        return 1;
    }

    if(aw87339->hwen_flag) {
        i2c_write_reg(aw87339_REG_SYSCTRL, aw87339_CHIP_DISABLE, aw87339);
    }
    aw87339_hw_off(aw87339);

    return 0;
}

unsigned char aw87339_audio_spk_if_off(void)
{
    return aw87339_audio_off(aw87339_spk);
}
unsigned char aw87339_audio_rcv_if_off(void)
{
    return aw87339_audio_off(aw87339_rec);
}
/*******************************************************************************
 * aw87339 firmware cfg update
 ******************************************************************************/
static void aw87339_rcvspk_cfg_loaded_spk(const struct firmware *cont, void *context)
{
    unsigned int i;

    if (!cont) {
        pr_err("%s: failed to read %s\n", __func__, aw87339_rcvspk_name_spk);
        release_firmware(cont);
        return;
    }

    pr_info("%s: loaded %s - size: %zu\n", __func__, aw87339_rcvspk_name_spk,
                    cont ? cont->size : 0);

    for(i=0; i<cont->size; i++) {
        pr_info("%s: cont: addr:0x%02x, data:0x%02x\n", __func__, i, *(cont->data+i));
    }

    aw87339_spk->aw87339_rcvspk_cnt = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
    if (!aw87339_spk->aw87339_rcvspk_cnt) {
        release_firmware(cont);
        pr_err("%s: error allocating memory\n", __func__);
        return;
    }
    aw87339_spk->aw87339_rcvspk_cnt->len = cont->size;
    memcpy(aw87339_spk->aw87339_rcvspk_cnt->data, cont->data, cont->size);
    release_firmware(cont);

    for(i=0; i<aw87339_spk->aw87339_rcvspk_cnt->len; i++) {
        pr_info("%s: rcv_cnt: addr:0x%02x, data:0x%02x\n",
                __func__, i, aw87339_rcvspk_reg_val(i,aw87339_spk));
    }

    aw87339_spk->rcvspk_cfg_update_flag = 1;    
}

static void aw87339_abrcv_cfg_loaded_spk(const struct firmware *cont, void *context)
{
    unsigned int i;
    int ret;

    if (!cont) {
        pr_err("%s: failed to read %s\n", __func__, aw87339_abrcv_name_spk);
        release_firmware(cont);
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_rcvspk_name_spk, 
                &aw87339_spk->i2c_client->dev, GFP_KERNEL, NULL, aw87339_rcvspk_cfg_loaded_spk);
        if(ret) {
            aw87339_spk->rcvspk_cfg_update_flag = 0;    
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_rcvspk_name_spk);
        }
        return;
    }

    pr_info("%s: loaded %s - size: %zu\n", __func__, aw87339_abrcv_name_spk,
                    cont ? cont->size : 0);

    for(i=0; i<cont->size; i++) {
        pr_info("%s: cont: addr:0x%02x, data:0x%02x\n", __func__, i, *(cont->data+i));
    }

    aw87339_spk->aw87339_abrcv_cnt = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
    if (!aw87339_spk->aw87339_abrcv_cnt) {
        release_firmware(cont);
        pr_err("%s: error allocating memory\n", __func__);
        return;
    }
    aw87339_spk->aw87339_abrcv_cnt->len = cont->size;
    memcpy(aw87339_spk->aw87339_abrcv_cnt->data, cont->data, cont->size);
    release_firmware(cont);

    for(i=0; i<aw87339_spk->aw87339_abrcv_cnt->len; i++) {
        pr_info("%s: rcv_cnt: addr:0x%02x, data:0x%02x\n",
                __func__, i, aw87339_abrcv_reg_val(i,aw87339_spk));
    }

    aw87339_spk->abrcv_cfg_update_flag = 1;

    ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_rcvspk_name_spk,
            &aw87339_spk->i2c_client->dev, GFP_KERNEL, NULL, aw87339_rcvspk_cfg_loaded_spk);
    if(ret) {
        aw87339_spk->rcvspk_cfg_update_flag = 0;
        pr_err("%s: request_firmware_nowait failed with read %s",
                __func__, aw87339_rcvspk_name_spk);
    }
}


static void aw87339_drcv_cfg_loaded_spk(const struct firmware *cont, void *context)
{
    unsigned int i;
    int ret;

    if (!cont) {
        pr_err("%s: failed to read %s\n", __func__, aw87339_drcv_name_spk);
        release_firmware(cont);
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_abrcv_name_spk, 
                &aw87339_spk->i2c_client->dev, GFP_KERNEL, NULL, aw87339_abrcv_cfg_loaded_spk);
        if(ret) {
            aw87339_spk->abrcv_cfg_update_flag = 0;    
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_abrcv_name_spk);
        }
        return;
    }

    pr_info("%s: loaded %s - size: %zu\n", __func__, aw87339_drcv_name_spk,
                    cont ? cont->size : 0);

    for(i=0; i<cont->size; i++) {
        pr_info("%s: cont: addr:0x%02x, data:0x%02x\n", __func__, i, *(cont->data+i));
    }

    aw87339_spk->aw87339_drcv_cnt = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
    if (!aw87339_spk->aw87339_drcv_cnt) {
        release_firmware(cont);
        pr_err("%s: error allocating memory\n", __func__);
        return;
    }
    aw87339_spk->aw87339_drcv_cnt->len = cont->size;
    memcpy(aw87339_spk->aw87339_drcv_cnt->data, cont->data, cont->size);
    release_firmware(cont);

    for(i=0; i<aw87339_spk->aw87339_drcv_cnt->len; i++) {
        pr_info("%s: rcv_cnt: addr:0x%02x, data:0x%02x\n",
                __func__, i, aw87339_drcv_reg_val(i,aw87339_spk));
    }

    aw87339_spk->drcv_cfg_update_flag = 1;

    ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_abrcv_name_spk,
            &aw87339_spk->i2c_client->dev, GFP_KERNEL, NULL, aw87339_abrcv_cfg_loaded_spk);
    if(ret) {
        aw87339_spk->abrcv_cfg_update_flag = 0;
        pr_err("%s: request_firmware_nowait failed with read %s",
                __func__, aw87339_abrcv_name_spk);
    }
}

static void aw87339_kspk_cfg_loaded_spk(const struct firmware *cont, void *context)
{
    unsigned int i;
    int ret;

    if (!cont) {
        pr_err("%s: failed to read %s\n", __func__, aw87339_kspk_name_spk);
        release_firmware(cont);
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_drcv_name_spk, 
                &aw87339_spk->i2c_client->dev, GFP_KERNEL, NULL, aw87339_drcv_cfg_loaded_spk);
        if(ret) {
            aw87339_spk->drcv_cfg_update_flag = 0;
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_drcv_name_spk);
        }
        return;
    }

    pr_info("%s: loaded %s - size: %zu\n", __func__, aw87339_kspk_name_spk,
                    cont ? cont->size : 0);

    for(i=0; i<cont->size; i++) {
        pr_info("%s: cont: addr:0x%02x, data:0x%02x\n",
                __func__, i, *(cont->data+i));
    }

    aw87339_spk->aw87339_kspk_cnt = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
    if (!aw87339_spk->aw87339_kspk_cnt) {
        release_firmware(cont);
        pr_err("%s: error allocating memory\n", __func__);
        return;
    }
    aw87339_spk->aw87339_kspk_cnt->len = cont->size;
    memcpy(aw87339_spk->aw87339_kspk_cnt->data, cont->data, cont->size);
    release_firmware(cont);

    for(i=0; i<aw87339_spk->aw87339_kspk_cnt->len; i++) {
        pr_info("%s: spk_cnt: addr:0x%02x, data:0x%02x\n",
                __func__, i, aw87339_kspk_reg_val(i,aw87339_spk));
    }

    aw87339_spk->kspk_cfg_update_flag = 1;

#ifndef OPLUS_BUG_COMPATIBILITY
    ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_drcv_name_spk, 
            &aw87339_spk->i2c_client->dev, GFP_KERNEL, NULL, aw87339_drcv_cfg_loaded_spk);
    if(ret) {
        aw87339_spk->drcv_cfg_update_flag = 0;
        pr_err("%s: request_firmware_nowait failed with read %s",
                __func__, aw87339_drcv_name_spk);
    }
#endif /* OPLUS_BUG_COMPATIBILITY */
}

static void aw87339_rcvspk_cfg_loaded_rec(const struct firmware *cont, void *context)
{
    unsigned int i;

    if (!cont) {
        pr_err("%s: failed to read %s\n", __func__, aw87339_rcvspk_name_rec);
        release_firmware(cont);
        return;
    }

    pr_info("%s: loaded %s - size: %zu\n", __func__, aw87339_rcvspk_name_rec,
                    cont ? cont->size : 0);

    for(i=0; i<cont->size; i++) {
        pr_info("%s: cont: addr:0x%02x, data:0x%02x\n", __func__, i, *(cont->data+i));
    }

    aw87339_rec->aw87339_rcvspk_cnt = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
    if (!aw87339_rec->aw87339_rcvspk_cnt) {
        release_firmware(cont);
        pr_err("%s: error allocating memory\n", __func__);
        return;
    }
    aw87339_rec->aw87339_rcvspk_cnt->len = cont->size;
    memcpy(aw87339_rec->aw87339_rcvspk_cnt->data, cont->data, cont->size);
    release_firmware(cont);

    for(i=0; i<aw87339_rec->aw87339_rcvspk_cnt->len; i++) {
        pr_info("%s: rcv_cnt: addr:0x%02x, data:0x%02x\n",
                __func__, i, aw87339_rcvspk_reg_val(i,aw87339_rec));
    }

    aw87339_rec->rcvspk_cfg_update_flag = 1;    
}

static void aw87339_abrcv_cfg_loaded_rec(const struct firmware *cont, void *context)
{
    unsigned int i;
    int ret;

    if (!cont) {
        pr_err("%s: failed to read %s\n", __func__, aw87339_abrcv_name_rec);
        release_firmware(cont);
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_rcvspk_name_rec, 
                &aw87339_rec->i2c_client->dev, GFP_KERNEL, NULL, aw87339_rcvspk_cfg_loaded_rec);
        if(ret) {
            aw87339_rec->rcvspk_cfg_update_flag = 0;    
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_rcvspk_name_rec);
        }
        return;
    }

    pr_info("%s: loaded %s - size: %zu\n", __func__, aw87339_abrcv_name_rec,
                    cont ? cont->size : 0);

    for(i=0; i<cont->size; i++) {
        pr_info("%s: cont: addr:0x%02x, data:0x%02x\n", __func__, i, *(cont->data+i));
    }

    aw87339_rec->aw87339_abrcv_cnt = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
    if (!aw87339_rec->aw87339_abrcv_cnt) {
        release_firmware(cont);
        pr_err("%s: error allocating memory\n", __func__);
        return;
    }
    aw87339_rec->aw87339_abrcv_cnt->len = cont->size;
    memcpy(aw87339_rec->aw87339_abrcv_cnt->data, cont->data, cont->size);
    release_firmware(cont);

    for(i=0; i<aw87339_rec->aw87339_abrcv_cnt->len; i++) {
        pr_info("%s: rcv_cnt: addr:0x%02x, data:0x%02x\n",
                __func__, i, aw87339_abrcv_reg_val(i,aw87339_rec));
    }

    aw87339_rec->abrcv_cfg_update_flag = 1;

    ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_rcvspk_name_rec,
            &aw87339_rec->i2c_client->dev, GFP_KERNEL, NULL, aw87339_rcvspk_cfg_loaded_rec);
    if(ret) {
        aw87339_rec->rcvspk_cfg_update_flag = 0;
        pr_err("%s: request_firmware_nowait failed with read %s",
                __func__, aw87339_rcvspk_name_rec);
    }
}


static void aw87339_drcv_cfg_loaded_rec(const struct firmware *cont, void *context)
{
    unsigned int i;
    int ret;

    if (!cont) {
        pr_err("%s: failed to read %s\n", __func__, aw87339_drcv_name_rec);
        release_firmware(cont);
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_abrcv_name_rec, 
                &aw87339_rec->i2c_client->dev, GFP_KERNEL, NULL, aw87339_abrcv_cfg_loaded_rec);
        if(ret) {
            aw87339_rec->abrcv_cfg_update_flag = 0;    
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_abrcv_name_rec);
        }
        return;
    }

    pr_info("%s: loaded %s - size: %zu\n", __func__, aw87339_drcv_name_rec,
                    cont ? cont->size : 0);

    for(i=0; i<cont->size; i++) {
        pr_info("%s: cont: addr:0x%02x, data:0x%02x\n", __func__, i, *(cont->data+i));
    }

    aw87339_rec->aw87339_drcv_cnt = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
    if (!aw87339_rec->aw87339_drcv_cnt) {
        release_firmware(cont);
        pr_err("%s: error allocating memory\n", __func__);
        return;
    }
    aw87339_rec->aw87339_drcv_cnt->len = cont->size;
    memcpy(aw87339_rec->aw87339_drcv_cnt->data, cont->data, cont->size);
    release_firmware(cont);

    for(i=0; i<aw87339_rec->aw87339_drcv_cnt->len; i++) {
        pr_info("%s: rcv_cnt: addr:0x%02x, data:0x%02x\n",
                __func__, i, aw87339_drcv_reg_val(i,aw87339_rec));
    }

    aw87339_rec->drcv_cfg_update_flag = 1;

    ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_abrcv_name_rec,
            &aw87339_rec->i2c_client->dev, GFP_KERNEL, NULL, aw87339_abrcv_cfg_loaded_rec);
    if(ret) {
        aw87339_rec->abrcv_cfg_update_flag = 0;
        pr_err("%s: request_firmware_nowait failed with read %s",
                __func__, aw87339_abrcv_name_rec);
    }
}

static void aw87339_kspk_cfg_loaded_rec(const struct firmware *cont, void *context)
{
    unsigned int i;
    int ret;

    if (!cont) {
        pr_err("%s: failed to read %s\n", __func__, aw87339_kspk_name_rec);
        release_firmware(cont);
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_drcv_name_rec, 
                &aw87339_rec->i2c_client->dev, GFP_KERNEL, NULL, aw87339_drcv_cfg_loaded_rec);
        if(ret) {
            aw87339_rec->drcv_cfg_update_flag = 0;
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_drcv_name_rec);
        }
        return;
    }

    pr_info("%s: loaded %s - size: %zu\n", __func__, aw87339_kspk_name_rec,
                    cont ? cont->size : 0);

    for(i=0; i<cont->size; i++) {
        pr_info("%s: cont: addr:0x%02x, data:0x%02x\n",
                __func__, i, *(cont->data+i));
    }

    aw87339_rec->aw87339_kspk_cnt = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
    if (!aw87339_rec->aw87339_kspk_cnt) {
        release_firmware(cont);
        pr_err("%s: error allocating memory\n", __func__);
        return;
    }
    aw87339_rec->aw87339_kspk_cnt->len = cont->size;
    memcpy(aw87339_rec->aw87339_kspk_cnt->data, cont->data, cont->size);
    release_firmware(cont);

    for(i=0; i<aw87339_rec->aw87339_kspk_cnt->len; i++) {
        pr_info("%s: spk_cnt: addr:0x%02x, data:0x%02x\n",
                __func__, i, aw87339_kspk_reg_val(i,aw87339_rec));
    }

    aw87339_rec->kspk_cfg_update_flag = 1;

    ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_drcv_name_rec, 
            &aw87339_rec->i2c_client->dev, GFP_KERNEL, NULL, aw87339_drcv_cfg_loaded_rec);
    if(ret) {
        aw87339_rec->drcv_cfg_update_flag = 0;
        pr_err("%s: request_firmware_nowait failed with read %s",
                __func__, aw87339_drcv_name_rec);
    }
}


#ifdef AWINIC_CFG_UPDATE_DELAY
static enum hrtimer_restart cfg_timer_func_spk(struct hrtimer *timer)
{
    pr_info("%s enter\n", __func__);

    schedule_work(&aw87339_spk->cfg_work);

    return HRTIMER_NORESTART;
}

static void cfg_work_routine_spk(struct work_struct *work)
{
    int ret = -1;

    pr_info("%s enter\n", __func__);

    ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_kspk_name_spk,
            &aw87339_spk->i2c_client->dev, GFP_KERNEL, NULL, aw87339_kspk_cfg_loaded_spk);
    if(ret) {
        pr_err("%s: request_firmware_nowait failed with read %s",
                __func__, aw87339_kspk_name_spk);
    }

}
static enum hrtimer_restart cfg_timer_func_rec(struct hrtimer *timer)
{
    pr_info("%s enter\n", __func__);

    schedule_work(&aw87339_rec->cfg_work);

    return HRTIMER_NORESTART;
}

static void cfg_work_routine_rec(struct work_struct *work)
{
    int ret = -1;

    pr_info("%s enter\n", __func__);

    ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_kspk_name_rec,
            &aw87339_rec->i2c_client->dev, GFP_KERNEL, NULL, aw87339_kspk_cfg_loaded_rec);
    if(ret) {
        pr_err("%s: request_firmware_nowait failed with read %s",
                __func__, aw87339_kspk_name_rec);
    }

}
#endif

static int aw87339_cfg_init(struct aw87339_t *aw87339)
{
    int ret = -1;
#ifdef AWINIC_CFG_UPDATE_DELAY
    #ifndef OPLUS_BUG_COMPATIBILITY
    int cfg_timer_val = 5000;
    #else
    int cfg_timer_val = 25000;
    #endif //OPLUS_BUG_COMPATIBILITY

    hrtimer_init(&aw87339->cfg_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    if (aw87339 == aw87339_spk) {
        aw87339->cfg_timer.function = cfg_timer_func_spk;
        INIT_WORK(&aw87339->cfg_work, cfg_work_routine_spk);
    }
    if (aw87339 == aw87339_rec) {
        aw87339->cfg_timer.function = cfg_timer_func_rec;
        INIT_WORK(&aw87339->cfg_work, cfg_work_routine_rec);
    }
    hrtimer_start(&aw87339->cfg_timer,
            ktime_set(cfg_timer_val/1000, (cfg_timer_val%1000)*1000000),
            HRTIMER_MODE_REL);
    ret = 0;
#else
    if (aw87339 == aw87339_spk) {
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_kspk_name_spk,
                &aw87339->i2c_client->dev, GFP_KERNEL, NULL, aw87339_kspk_cfg_loaded_spk);
        if(ret) {
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_kspk_name_spk);
        }
    }
    if (aw87339 == aw87339_rec) {
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_kspk_name_rec,
                &aw87339->i2c_client->dev, GFP_KERNEL, NULL, aw87339_kspk_cfg_loaded_rec);
        if(ret) {
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_kspk_name_rec);
        }
    }
#endif
    return ret;
}
/*******************************************************************************
 * aw87339 attribute
 ******************************************************************************/
static ssize_t aw87339_get_reg_spk(struct device* cd,struct device_attribute *attr, char* buf)
{
    unsigned char reg_val;
    ssize_t len = 0;
    unsigned char i;
    for(i=0; i<aw87339_REG_MAX ;i++) {
        if(aw87339_reg_access[i] & REG_RD_ACCESS) {
            reg_val = i2c_read_reg(i, aw87339_spk);
            len += snprintf(buf+len, PAGE_SIZE-len, "reg:0x%02x=0x%02x\n", i,reg_val);
        }
    }

    return len;
}

static ssize_t aw87339_set_reg_spk(struct device* cd, struct device_attribute *attr, const char* buf, size_t len)
{
    unsigned int databuf[2];
    if(2 == sscanf(buf,"%x %x",&databuf[0], &databuf[1])) {
        i2c_write_reg(databuf[0],databuf[1],aw87339_spk);
    }
    return len;
}


static ssize_t aw87339_get_hwen_spk(struct device* cd,struct device_attribute *attr, char* buf)
{
    ssize_t len = 0;

    len += snprintf(buf+len, PAGE_SIZE-len, "hwen: %d\n", aw87339_spk->hwen_flag);

    return len;
}

static ssize_t aw87339_set_hwen_spk(struct device* cd, struct device_attribute *attr, const char* buf, size_t len)
{
    unsigned int databuf[16];
    
    sscanf(buf,"%d",&databuf[0]);
    if(databuf[0] == 0) {        // OFF
        aw87339_hw_off(aw87339_spk);
    } else {                    // ON
        aw87339_hw_on(aw87339_spk);
    }

    return len;
}

static ssize_t aw87339_get_update_spk(struct device* cd,struct device_attribute *attr, char* buf)
{
    ssize_t len = 0;

    return len;
}

static ssize_t aw87339_set_update_spk(struct device* cd, struct device_attribute *attr, const char* buf, size_t len)
{
    unsigned int databuf[16];
    int ret;

    sscanf(buf,"%d",&databuf[0]);
    if(databuf[0] == 0) {        
    } else {                    
        aw87339_spk->kspk_cfg_update_flag = 0;
        aw87339_spk->drcv_cfg_update_flag = 0;
        aw87339_spk->abrcv_cfg_update_flag = 0;
        aw87339_spk->rcvspk_cfg_update_flag = 0;
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_kspk_name_spk, 
                &aw87339_spk->i2c_client->dev, GFP_KERNEL, NULL, aw87339_kspk_cfg_loaded_spk);
        if(ret) {
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_kspk_name_spk);
        }
    }

    return len;
}

static ssize_t aw87339_get_mode_spk(struct device* cd,struct device_attribute *attr, char* buf)
{
    ssize_t len = 0;

    len += snprintf(buf+len, PAGE_SIZE-len, "0: off mode\n");
    len += snprintf(buf+len, PAGE_SIZE-len, "1: kspk mode\n");
    len += snprintf(buf+len, PAGE_SIZE-len, "2: drcv mode\n");
    len += snprintf(buf+len, PAGE_SIZE-len, "3: abrcv mode\n");
    len += snprintf(buf+len, PAGE_SIZE-len, "4: rcvspk mode\n");

    return len;
}

static ssize_t aw87339_set_mode_spk(struct device* cd, struct device_attribute *attr, const char* buf, size_t len)
{
    unsigned int databuf[16];

    sscanf(buf,"%d",&databuf[0]);
    if(databuf[0] == 0) {
        aw87339_audio_off(aw87339_spk);
    } else if(databuf[0] == 1) {
        aw87339_audio_kspk(aw87339_spk);
    } else if(databuf[0] == 2) {
        aw87339_audio_drcv(aw87339_spk);
    } else if(databuf[0] == 3) {
        aw87339_audio_abrcv(aw87339_spk);
    } else if(databuf[0] == 4) {
        aw87339_audio_rcvspk(aw87339_spk);
    } else {
        aw87339_audio_off(aw87339_spk);
    }

    return len;
}

static ssize_t aw87339_get_reg_rec(struct device* cd,struct device_attribute *attr, char* buf)
{
    unsigned char reg_val;
    ssize_t len = 0;
    unsigned char i;
    for(i=0; i<aw87339_REG_MAX ;i++) {
        if(aw87339_reg_access[i] & REG_RD_ACCESS) {
            reg_val = i2c_read_reg(i,aw87339_rec);
            len += snprintf(buf+len, PAGE_SIZE-len, "reg:0x%02x=0x%02x\n", i,reg_val);
        }
    }

    return len;
}

static ssize_t aw87339_set_reg_rec(struct device* cd, struct device_attribute *attr, const char* buf, size_t len)
{
    unsigned int databuf[2];
    if(2 == sscanf(buf,"%x %x",&databuf[0], &databuf[1])) {
        i2c_write_reg(databuf[0],databuf[1],aw87339_rec);
    }
    return len;
}


static ssize_t aw87339_get_hwen_rec(struct device* cd,struct device_attribute *attr, char* buf)
{
    ssize_t len = 0;

    len += snprintf(buf+len, PAGE_SIZE-len, "hwen: %d\n", aw87339_rec->hwen_flag);

    return len;
}

static ssize_t aw87339_set_hwen_rec(struct device* cd, struct device_attribute *attr, const char* buf, size_t len)
{
    unsigned int databuf[16];
    
    sscanf(buf,"%d",&databuf[0]);
    if(databuf[0] == 0) {        // OFF
        aw87339_hw_off(aw87339_rec);
    } else {                    // ON
        aw87339_hw_on(aw87339_rec);
    }

    return len;
}

static ssize_t aw87339_get_update_rec(struct device* cd,struct device_attribute *attr, char* buf)
{
    ssize_t len = 0;

    return len;
}

static ssize_t aw87339_set_update_rec(struct device* cd, struct device_attribute *attr, const char* buf, size_t len)
{
    unsigned int databuf[16];
    int ret;

    sscanf(buf,"%d",&databuf[0]);
    if(databuf[0] == 0) {        
    } else {                    
        aw87339_rec->kspk_cfg_update_flag = 0;
        aw87339_rec->drcv_cfg_update_flag = 0;
        aw87339_rec->abrcv_cfg_update_flag = 0;
        aw87339_rec->rcvspk_cfg_update_flag = 0;
        ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, aw87339_kspk_name_rec, 
                &aw87339_rec->i2c_client->dev, GFP_KERNEL, NULL, aw87339_kspk_cfg_loaded_rec);
        if(ret) {
            pr_err("%s: request_firmware_nowait failed with read %s",
                    __func__, aw87339_kspk_name_rec);
        }
    }

    return len;
}

static ssize_t aw87339_get_mode_rec(struct device* cd,struct device_attribute *attr, char* buf)
{
    ssize_t len = 0;

    len += snprintf(buf+len, PAGE_SIZE-len, "0: off mode\n");
    len += snprintf(buf+len, PAGE_SIZE-len, "1: kspk mode\n");
    len += snprintf(buf+len, PAGE_SIZE-len, "2: drcv mode\n");
    len += snprintf(buf+len, PAGE_SIZE-len, "3: abrcv mode\n");
    len += snprintf(buf+len, PAGE_SIZE-len, "4: rcvspk mode\n");

    return len;
}

static ssize_t aw87339_set_mode_rec(struct device* cd, struct device_attribute *attr, const char* buf, size_t len)
{
    unsigned int databuf[16];

    sscanf(buf,"%d",&databuf[0]);
    if(databuf[0] == 0) {
        aw87339_audio_off(aw87339_rec);
    } else if(databuf[0] == 1) {
        aw87339_audio_kspk(aw87339_rec);
    } else if(databuf[0] == 2) {
        aw87339_audio_drcv(aw87339_rec);
    } else if(databuf[0] == 3) {
        aw87339_audio_abrcv(aw87339_rec);
    } else if(databuf[0] == 4) {
        aw87339_audio_rcvspk(aw87339_rec);
    } else {
        aw87339_audio_off(aw87339_rec);
    }

    return len;
}

static DEVICE_ATTR(spkreg, 0660, aw87339_get_reg_spk,  aw87339_set_reg_spk);
static DEVICE_ATTR(spkhwen, 0660, aw87339_get_hwen_spk,  aw87339_set_hwen_spk);
static DEVICE_ATTR(spkupdate, 0660, aw87339_get_update_spk,  aw87339_set_update_spk);
static DEVICE_ATTR(spkmode, 0660, aw87339_get_mode_spk,  aw87339_set_mode_spk);

static struct attribute *aw87339_attributes_spk[] = {
    &dev_attr_spkreg.attr,
    &dev_attr_spkhwen.attr,
    &dev_attr_spkupdate.attr,
    &dev_attr_spkmode.attr,
    NULL
};
static DEVICE_ATTR(rcvreg, 0660, aw87339_get_reg_rec,  aw87339_set_reg_rec);
static DEVICE_ATTR(rcvhwen, 0660, aw87339_get_hwen_rec,  aw87339_set_hwen_rec);
static DEVICE_ATTR(rcvupdate, 0660, aw87339_get_update_rec,  aw87339_set_update_rec);
static DEVICE_ATTR(rcvmode, 0660, aw87339_get_mode_rec,  aw87339_set_mode_rec);

static struct attribute *aw87339_attributes_rec[] = {
    &dev_attr_rcvreg.attr,
    &dev_attr_rcvhwen.attr,
    &dev_attr_rcvupdate.attr,
    &dev_attr_rcvmode.attr,
    NULL
};
static struct attribute_group aw87339_attribute_group_spk = {
    .attrs = aw87339_attributes_spk
};
static struct attribute_group aw87339_attribute_group_rec = {
    .attrs = aw87339_attributes_rec
};

/*****************************************************
 * device tree
 *****************************************************/
static int aw87339_parse_dt(struct device *dev, struct device_node *np, struct aw87339_t *aw87339) {
    aw87339->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);
    if (aw87339->reset_gpio < 0) {
        dev_err(dev, "%s: no reset gpio provided\n", __func__);
        return -1;
    } else {
        dev_info(dev, "%s: reset gpio provided ok\n", __func__);
    }
    return 0;
}

int aw87339_hw_reset(struct aw87339_t *aw87339)
{
    pr_info("%s enter\n", __func__);

    if (aw87339 && gpio_is_valid(aw87339->reset_gpio)) {
        gpio_set_value_cansleep(aw87339->reset_gpio, 0);
        msleep(2);
        gpio_set_value_cansleep(aw87339->reset_gpio, 1);
        msleep(2);
        aw87339->hwen_flag = 1;
    } else {
        aw87339->hwen_flag = 0;
        dev_err(&aw87339->i2c_client->dev, "%s:  failed\n", __func__);
    }
    return 0;
}

/*****************************************************
 * check chip id
 *****************************************************/
int aw87339_read_chipid(struct aw87339_t *aw87339)
{
    unsigned int cnt = 0;
    unsigned int reg = 0;
  
    while(cnt < AW_READ_CHIPID_RETRIES) {
        reg = i2c_read_reg(0x00, aw87339);
        if(reg == 0x39) {
            pr_info("%s: aw87339 chipid=0x%x\n", __func__, reg);
            return 0;
        } else {
            pr_info("%s: aw87339 chipid=0x%x error\n", __func__, reg);
        }
        cnt ++;

        msleep(AW_READ_CHIPID_RETRY_DELAY);
    }

    return -EINVAL;
}

int aw87339_spk_status_get(void)
{
	return aw87339_spk_ctrl;
}

void aw87339_spk_enable_set(int enable)
{
	if (enable == aw87339_spk_ctrl)
		return;

	if (enable) {
		aw87339_audio_spk_if_kspk();
		aw87339_spk_ctrl = 1;
	} else {
		aw87339_audio_spk_if_off();
		aw87339_spk_ctrl = 0;
	}
}

/*******************************************************************************
 * aw87339 i2c driver
 ******************************************************************************/
static int aw87339_i2c_probe_spk(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct device_node *np = client->dev.of_node;
    int ret = -1;
    pr_info("%s Enter\n", __func__);

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        dev_err(&client->dev, "%s: check_functionality failed\n", __func__);
        ret = -ENODEV;
        goto exit_check_functionality_failed;
    }

    aw87339_spk = devm_kzalloc(&client->dev, sizeof(struct aw87339_t), GFP_KERNEL);
    if (aw87339_spk == NULL) {
        ret = -ENOMEM;
        goto exit_devm_kzalloc_failed;
    }

    aw87339_spk->i2c_client = client;
    i2c_set_clientdata(client, aw87339_spk);

    /* aw87339 rst */
    if (np) {
        ret = aw87339_parse_dt(&client->dev, np, aw87339_spk);
        if (ret) {
            dev_err(&client->dev, "%s: failed to parse device tree node\n", __func__);
            goto exit_gpio_get_failed;
        }
    } else {
        aw87339_spk->reset_gpio = -1;
    }

    if (gpio_is_valid(aw87339_spk->reset_gpio)) {
        ret = devm_gpio_request_one(&client->dev, aw87339_spk->reset_gpio,
            GPIOF_OUT_INIT_LOW, "aw87339_spk_rst");
        if (ret){
            dev_err(&client->dev, "%s: rst request failed\n", __func__);
            goto exit_gpio_request_failed;
        }
    }

    /* hardware reset */
    aw87339_hw_reset(aw87339_spk);

    /* aw87339 chip id */
    ret = aw87339_read_chipid(aw87339_spk);
    if (ret < 0) {
        dev_err(&client->dev, "%s: aw87339_spk_read_chipid failed ret=%d\n", __func__, ret);
        goto exit_i2c_check_id_failed;
    }

    ret = sysfs_create_group(&client->dev.kobj, &aw87339_attribute_group_spk);
    if (ret < 0) {
        dev_info(&client->dev, "%s error creating sysfs attr files\n", __func__);
    }

    /* aw87339 cfg update */
    aw87339_spk->kspk_cfg_update_flag = 0;
    aw87339_spk->drcv_cfg_update_flag = 0;
    aw87339_spk->abrcv_cfg_update_flag = 0;
    aw87339_spk->rcvspk_cfg_update_flag = 0;
    aw87339_cfg_init(aw87339_spk);


    /* aw87339 hardware off */
    aw87339_hw_off(aw87339_spk);

    aw87339_spk->init_flag = 1;

    return 0;

exit_i2c_check_id_failed:
    devm_gpio_free(&client->dev, aw87339_spk->reset_gpio);
exit_gpio_request_failed:
exit_gpio_get_failed:
    devm_kfree(&client->dev, aw87339_spk);
    aw87339_spk = NULL;
exit_devm_kzalloc_failed:
exit_check_functionality_failed:
    return ret;
}

static int aw87339_i2c_remove_spk(struct i2c_client *client)
{
    if(gpio_is_valid(aw87339_spk->reset_gpio)) {
        devm_gpio_free(&client->dev, aw87339_spk->reset_gpio);
    }

    return 0;
}
static int aw87339_i2c_probe_rec(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct device_node *np = client->dev.of_node;
    int ret = -1;
    pr_info("%s Enter\n", __func__);

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        dev_err(&client->dev, "%s: check_functionality failed\n", __func__);
        ret = -ENODEV;
        goto exit_check_functionality_failed;
    }

    aw87339_rec = devm_kzalloc(&client->dev, sizeof(struct aw87339_t), GFP_KERNEL);
    if (aw87339_rec == NULL) {
        ret = -ENOMEM;
        goto exit_devm_kzalloc_failed;
    }

    aw87339_rec->i2c_client = client;
    i2c_set_clientdata(client, aw87339_rec);

    /* aw87339 rst */
    if (np) {
        ret = aw87339_parse_dt(&client->dev, np, aw87339_rec);
        if (ret) {
            dev_err(&client->dev, "%s: failed to parse device tree node\n", __func__);
            goto exit_gpio_get_failed;
        }
    } else {
        aw87339_rec->reset_gpio = -1;
    }

    if (gpio_is_valid(aw87339_rec->reset_gpio)) {
        ret = devm_gpio_request_one(&client->dev, aw87339_rec->reset_gpio,
            GPIOF_OUT_INIT_LOW, "aw87339_rec_rst");
        if (ret){
            dev_err(&client->dev, "%s: rst request failed\n", __func__);
            goto exit_gpio_request_failed;
        }
    }

    /* hardware reset */
    aw87339_hw_reset(aw87339_rec);

    /* aw87339 chip id */
    ret = aw87339_read_chipid(aw87339_rec);
    if (ret < 0) {
        dev_err(&client->dev, "%s: aw87339_rec_read_chipid failed ret=%d\n", __func__, ret);
        goto exit_i2c_check_id_failed;
    }

    ret = sysfs_create_group(&client->dev.kobj, &aw87339_attribute_group_rec);
    if (ret < 0) {
        dev_info(&client->dev, "%s error creating sysfs attr files\n", __func__);
    }

    /* aw87339 cfg update */
    aw87339_rec->kspk_cfg_update_flag = 0;
    aw87339_rec->drcv_cfg_update_flag = 0;
    aw87339_rec->abrcv_cfg_update_flag = 0;
    aw87339_rec->rcvspk_cfg_update_flag = 0;
    aw87339_cfg_init(aw87339_rec);


    /* aw87339 hardware off */
    aw87339_hw_off(aw87339_rec);

    aw87339_rec->init_flag = 1;

    return 0;

exit_i2c_check_id_failed:
    devm_gpio_free(&client->dev, aw87339_rec->reset_gpio);
exit_gpio_request_failed:
exit_gpio_get_failed:
    devm_kfree(&client->dev, aw87339_rec);
    aw87339_rec = NULL;
exit_devm_kzalloc_failed:
exit_check_functionality_failed:
    return ret;
}

static int aw87339_i2c_remove_rec(struct i2c_client *client)
{
    if(gpio_is_valid(aw87339_rec->reset_gpio)) {
        devm_gpio_free(&client->dev, aw87339_rec->reset_gpio);
    }

    return 0;
}
static const struct i2c_device_id aw87339_i2c_id_spk[] = {
    { AW87339_I2C_NAME_SPK, 0 },
    { }
};
static const struct i2c_device_id aw87339_i2c_id_rec[] = {
    { AW87339_I2C_NAME_REC, 0 },
    { }
};

static const struct of_device_id extpa_of_match_spk[] = {
    {.compatible = "awinic,aw87339_pa_spk"},
    {},
};
static const struct of_device_id extpa_of_match_rec[] = {
    {.compatible = "awinic,aw87339_pa_rec"},
    {},
};

static struct i2c_driver aw87339_i2c_driver_spk = {
    .driver = {
        .owner = THIS_MODULE,
        .name = AW87339_I2C_NAME_SPK,
        .of_match_table = extpa_of_match_spk,
    },
    .probe = aw87339_i2c_probe_spk,
    .remove = aw87339_i2c_remove_spk,
    .id_table    = aw87339_i2c_id_spk,
};
static struct i2c_driver aw87339_i2c_driver_rec = {
    .driver = {
        .owner = THIS_MODULE,
        .name = AW87339_I2C_NAME_REC,
        .of_match_table = extpa_of_match_rec,
    },
    .probe = aw87339_i2c_probe_rec,
    .remove = aw87339_i2c_remove_rec,
    .id_table    = aw87339_i2c_id_rec,
};
static int __init aw87339_pa_init(void) {
    int ret;

    pr_info("%s enter\n", __func__);
    pr_info("%s: driver version: %s\n", __func__, AW87339_DRIVER_VERSION);

    ret = i2c_add_driver(&aw87339_i2c_driver_spk);
    if (ret) {
        pr_info("****[%s] Unable to register speaker pa driver (%d)\n",
                __func__, ret);
        return ret;
    }
    ret = i2c_add_driver(&aw87339_i2c_driver_rec);
    if (ret) {
        pr_info("****[%s] Unable to register receiver pa driver (%d)\n",
                __func__, ret);
        return ret;
    }
    return 0;
}

static void __exit aw87339_pa_exit(void) {
    pr_info("%s enter\n", __func__);
    i2c_del_driver(&aw87339_i2c_driver_spk);
    i2c_del_driver(&aw87339_i2c_driver_rec);
}

module_init(aw87339_pa_init);
module_exit(aw87339_pa_exit);
EXPORT_SYMBOL(aw87339_audio_spk_if_kspk);
EXPORT_SYMBOL(aw87339_audio_rcv_if_kspk);
EXPORT_SYMBOL(aw87339_audio_rcv_if_drcv);
EXPORT_SYMBOL(aw87339_audio_spk_if_off);
EXPORT_SYMBOL(aw87339_audio_rcv_if_off);
EXPORT_SYMBOL(aw87339_spk_status_get);
EXPORT_SYMBOL(aw87339_spk_enable_set);
MODULE_AUTHOR("<liweilei@awinic.com.cn>");
MODULE_DESCRIPTION("awinic aw87339 pa driver");
MODULE_LICENSE("GPL v2");
