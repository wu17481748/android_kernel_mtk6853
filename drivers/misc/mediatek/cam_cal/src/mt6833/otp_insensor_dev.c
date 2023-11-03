/***********************************************************
 * ** Copyright (C), 2008-2016, OPPO Mobile Comm Corp., Ltd.
 * ** ODM_HQ_EDIT
 * ** File: - otp_insensor_dev.c
 * ** Description: Source file for CBufferList.
 * **           To allocate and free memory block safely.
 * ** Version: 1.0
 * ** Date : 2018/12/07
 * ** Author: YanKun.Zhai@Mutimedia.camera.driver.otp
 * **
 * ** ------------------------------- Revision History: -------------------------------
 * **   <author>History<data>      <version >version       <desc>
 * **  YanKun.Zhai 2018/12/07     1.0     build this module
 * **
 * ****************************************************************/

#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <asm/atomic.h>
#include <linux/slab.h>

#include "cam_cal.h"
#include "cam_cal_define.h"
#include "cam_cal_list.h"

#include "kd_camera_typedef.h"
#include "kd_imgsensor.h"
#include "kd_imgsensor_define.h"
#include "kd_imgsensor_errcode.h"

#ifdef ODM_HQ_EDIT
/*Houbing.Peng@ODM_HQ Cam.Drv 20200915 add for otp*/
#include <soc/oppo/oppo_project.h>
#endif

#define LOG_TAG "OTP_InSensor"
#define LOG_ERR(format, ...) pr_err(LOG_TAG "Line:%d  FUNC: %s:  "format, __LINE__, __func__, ## __VA_ARGS__)
#define LOG_INFO(format, ...) pr_info(LOG_TAG "  %s:  "format, __func__, ## __VA_ARGS__)
#define LOG_DEBUG(format, ...) pr_debug(LOG_TAG "  %s:  "format, __func__, ## __VA_ARGS__)

#define ERROR_I2C       1
#define ERROR_CHECKSUM  2
#define ERROR_READ_FLAG 3




static DEFINE_SPINLOCK(g_spinLock);

static struct i2c_client *g_pstI2CclientG;
/* add for linux-4.4 */
#ifndef I2C_WR_FLAG
#define I2C_WR_FLAG		(0x1000)
#define I2C_MASK_FLAG	(0x00ff)
#endif

struct i2c_client *g_pstI2Cclients[3]; /* I2C_DEV_IDX_MAX */


#define MAX_EEPROM_BYTE 0x1FFF
#define CHECKSUM_FLAG_ADDR MAX_EEPROM_BYTE-1
#define READ_FLAG_ADDR MAX_EEPROM_BYTE-2

#define OTP_DATA_GOOD_FLAG 0x88
char g_otp_buf[3][MAX_EEPROM_BYTE] = {{0}, {0}, {0}};

#define MAX_NAME_LENGTH 20
#define MAX_GROUP_NUM 8
#define MAX_GROUP_ADDR_NUM 3


typedef struct
{
	int group_start_addr;
	int group_end_addr;
	int group_flag_addr;
	int group_checksum_addr;
}GROUP_ADDR_INFO;

typedef struct
{
	char group_name[MAX_NAME_LENGTH];
	GROUP_ADDR_INFO group_addr_info[MAX_GROUP_ADDR_NUM];
}GROUP_INFO;


typedef struct
{
	char module_name[MAX_NAME_LENGTH];
	int group_num;
	int group_addr_info_num;
	GROUP_INFO group_info[MAX_GROUP_NUM];
	int  (* readFunc) (u16 addr, u8 *data);
}OTP_MAP;


int hi556_read_data(u16 addr, u8 *data);

OTP_MAP hi556_otp_map_blade = {
		.module_name = "HI556_Blade",
		.group_num = 3,
		.group_addr_info_num = 3,
		.readFunc = hi556_read_data,
		.group_info = {
						{"info",
							{
								{0x0401, 0x040F, 0x0410, 0x0411},
								{0x0C49, 0x0C57, 0x0C58, 0x0C59},
								{0x1491, 0x149F, 0x14A0, 0x14A1},
							},
						},
						{"awb",
							{
								{0x0421, 0x0430, 0x0431, 0x0432},
								{0x0C69, 0x0C78, 0x0C79, 0x0C7A},
								{0x14B1, 0x14C0, 0x14C1, 0x14C2},
							 },
						},
						{"lsc",
							{
								{0x0433, 0x0B7E, 0x0B7F, 0x0B80},
								{0X0C7B, 0x13C6, 0x13C7, 0x13C8},
								{0x14C3, 0x1C0E, 0x1C0F, 0x1C10},
							},
						},
			},
};

static int read_reg16_data8(u16 addr, u8 *data)
{
	int ret = 0;
	char puSendCmd[2] = {(char)(addr >> 8), (char)(addr & 0xff)};

	spin_lock(&g_spinLock);
		g_pstI2CclientG->addr =
			g_pstI2CclientG->addr & (I2C_MASK_FLAG | I2C_WR_FLAG);
	spin_unlock(&g_spinLock);

	ret = i2c_master_send(g_pstI2CclientG, puSendCmd, 2);
	if (ret != 2) {
		pr_err("I2C send failed!!, Addr = 0x%x\n", addr);
		return -1;
	}
	ret = i2c_master_recv(g_pstI2CclientG, (char *)data, 1);
	if (ret != 1) {
		pr_err("I2C read failed!!\n");
		return -1;
	}
	spin_lock(&g_spinLock);
	g_pstI2CclientG->addr = g_pstI2CclientG->addr & I2C_MASK_FLAG;
	spin_unlock(&g_spinLock);

	return 0;
}
static int write_reg16_data8(u16 addr, u8 data)
{
	int ret = 0;
	char puSendCmd[3] = {(char)(addr >> 8), (char)(addr & 0xff),
						(char)(data & 0xff)};
	spin_lock(&g_spinLock);
	g_pstI2CclientG->addr =
			g_pstI2CclientG->addr & (I2C_MASK_FLAG | I2C_WR_FLAG);
	spin_unlock(&g_spinLock);

	ret = i2c_master_send(g_pstI2CclientG, puSendCmd, 3);

	if (ret != 3) {
		pr_err("I2C send failed!!, Addr = 0x%x\n", addr);
		return -1;
	}
	spin_lock(&g_spinLock);
	g_pstI2CclientG->addr = g_pstI2CclientG->addr & I2C_MASK_FLAG;
	spin_unlock(&g_spinLock);

	return 0;
}

static int parse_otp_map_data(OTP_MAP * map, char * data)
{
	int i = 0, j = 0;
	int addr = 0, size = 0, curr_addr = 0;
	int  ret = 0;
	char readbyte = 0;
	char group_flag = 0;
	int checksum = -1;

	LOG_INFO("module: %s ......", map->module_name);

	for (i = 0; i < map->group_num; i++) {
		checksum = 0;
		size = 0;

		LOG_INFO("groupinfo: %s, start_addr 0x%04x(%04d)", map->group_info[i].group_name, curr_addr, curr_addr);

		 for (j = 0; j < map->group_addr_info_num; j++) {
			ret = map->readFunc(map->group_info[i].group_addr_info[j].group_flag_addr, &group_flag);
			if (ret < 0) {
				LOG_ERR("   read group flag error addr 0x%04x", map->group_info[i].group_addr_info[j].group_flag_addr);
				return -ERROR_I2C;
			}
			LOG_INFO("readFunc: group %d flag_addr = 0x%04x, value = 0x%02x\n", j, map->group_info[i].group_addr_info[j].group_flag_addr, group_flag);
			if (group_flag == 0x01) {
				LOG_INFO(" group %d selected\n", j);
				break;
			}
		}

		if (j == map->group_addr_info_num) {
			LOG_ERR("Can't found group flag 0x%x", readbyte);
			return -ERROR_READ_FLAG;
		}

		for (addr = map->group_info[i].group_addr_info[j].group_start_addr;
				addr <= map->group_info[i].group_addr_info[j].group_end_addr;
				addr++) {
			ret = map->readFunc(addr, data);
			if (ret < 0) {
				LOG_ERR(" read data error");
			}
			LOG_DEBUG(" group: %s, addr: 0x%04x, viraddr: 0x%04x(%04d), data: 0x%04x(%04d) ",
						map->group_info[i].group_name, addr, curr_addr, curr_addr, *data, *data);

			checksum += *data;
			curr_addr++;
			size++;
			data++;
		}

		checksum = checksum % 0xFF;
		ret = map->readFunc(map->group_info[i].group_addr_info[j].group_checksum_addr, &readbyte);
		if (checksum == readbyte) {
			LOG_INFO("groupinfo: %s, checksum OK c(%04d) r(%04d)", map->group_info[i].group_name, checksum, readbyte);
		} else {
			LOG_ERR("groupinfo: %s, checksum ERROR ret=%d, checksum=%04d readbyte=%04d", map->group_info[i].group_name, ret, checksum, readbyte);
			ret = -ERROR_CHECKSUM;
		}
		LOG_INFO("groupinfo: %s, end_addr 0x%04x(%04d) size 0x%04x(%04d)",
						map->group_info[i].group_name, curr_addr-1, curr_addr-1, size, size);
	}
	return ret;
}

int hi556_read_data(u16 addr, u8 *data)
{
	static u16 last_addr = 0;
	if (addr != last_addr+ 1) {
		write_reg16_data8(0x010a, (addr >> 8) & 0xff);
		write_reg16_data8(0x010b, addr & 0xff);
		write_reg16_data8(0x0102, 0x01);
	}

	last_addr = addr;
	return read_reg16_data8(0x0108, data);
}
void hi556_otp_read_enable()
{
	write_reg16_data8(0x0a02, 0x01);
	write_reg16_data8(0x0a00, 0x00);
	mdelay(10);
	write_reg16_data8(0x0f02, 0x00);
	write_reg16_data8(0x011a, 0x01);
	write_reg16_data8(0x011b, 0x09);
	write_reg16_data8(0x0d04, 0x01);
	write_reg16_data8(0x0d02, 0x07);
	write_reg16_data8(0x003e, 0x10);
	write_reg16_data8(0x0a00, 0x01);
}
void hi556_otp_read_disable()
{
	write_reg16_data8(0x0a00, 0x00);
	mdelay(10);
	write_reg16_data8(0x004a, 0x00);
	write_reg16_data8(0x0d04, 0x00);
	write_reg16_data8(0x003e, 0x00);
	write_reg16_data8(0x004a, 0x01);
	write_reg16_data8(0x0a00, 0x01);
}

unsigned int Hi556_read_region(struct i2c_client *client, unsigned int addr, unsigned char *data, unsigned int size)
{
	int ret = 0;

	if (g_otp_buf[IMGSENSOR_SENSOR_IDX_SUB][READ_FLAG_ADDR]) {
		LOG_INFO("read otp data from g_otp_buf: addr 0x%x, size %d", addr, size);
		memcpy((void *)data, &g_otp_buf[IMGSENSOR_SENSOR_IDX_SUB][addr], size);
		return size;
	}
	if (client != NULL) {
		g_pstI2CclientG = client;
	} else if (g_pstI2Cclients[IMGSENSOR_SENSOR_IDX_SUB] != NULL) {
		g_pstI2CclientG = g_pstI2Cclients[IMGSENSOR_SENSOR_IDX_SUB];
		g_pstI2CclientG->addr = 0x40 >> 1;
	}
	hi556_otp_read_enable();
	ret = parse_otp_map_data(&hi556_otp_map_blade, &g_otp_buf[IMGSENSOR_SENSOR_IDX_SUB][0]);
	if (!ret) {
		g_otp_buf[IMGSENSOR_SENSOR_IDX_SUB][CHECKSUM_FLAG_ADDR] = OTP_DATA_GOOD_FLAG;
		g_otp_buf[IMGSENSOR_SENSOR_IDX_SUB][READ_FLAG_ADDR] = 1;
	}
	hi556_otp_read_disable();

	if (NULL != data) {
		memcpy((void *)data, &g_otp_buf[IMGSENSOR_SENSOR_IDX_SUB][addr], size);
	}
	return ret;
}
