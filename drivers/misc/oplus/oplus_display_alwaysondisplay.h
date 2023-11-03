/***************************************************************
** Copyright (C),  2020,  OPPO Mobile Comm Corp.,  Ltd
** File : oppo_display_alwaysondisplay.h
** Description : oppo_display_alwaysondisplay. implement
** Version : 1.0
** Date : 2020/05/13
**
** ------------------------------- Revision History: -----------
**  <author>        <data>        <version >        <desc>
**   Zhang.JianBin2020/05/13        1.0          Modify for MT6779_R
******************************************************************/
#ifndef _OPPO_DISPLAY_ALWAYSONDISPLAY_H_
#define _OPPO_DISPLAY_ALWAYSONDISPLAY_H_

#include <linux/err.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/err.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/leds.h>
#include "primary_display.h"
#include "ddp_hal.h"
#include "ddp_manager.h"
#include <linux/types.h>
#include "disp_session.h"
#include "disp_lcm.h"
#include "disp_helper.h"
#include "oplus_display_private_api.h"


int oppo_panel_set_aod_light_mode(void *buf);
int oppo_panel_get_aod_light_mode(void *buf);
extern int _is_lcm_inited(struct disp_lcm_handle *plcm);

/* #ifdef OPLUS_FEATURE_RAMLESS_AOD */
extern bool oppo_display_aod_ramless_support;
extern unsigned int rlta_enable;
extern int disp_lcm_set_aod_area(struct disp_lcm_handle *plcm, void *handle, unsigned char *area);
extern int disp_lcm_doze_enable(struct disp_lcm_handle *plcm, void *handle);
extern int disp_lcm_doze_disable(struct disp_lcm_handle *plcm, void *handle);
extern int primary_display_switch_aod_mode(int mode);
extern int primary_display_set_aod_area(unsigned char *area, int use_cmdq);
extern int disp_lcm_set_aod_cv_mode(struct disp_lcm_handle *plcm, void *handle, unsigned int mode);
/* #endif */ /* OPLUS_FEATURE_RAMLESS_AOD */
#endif /*_OPPO_DISPLAY_ALWAYSONDISPLAY_H_*/

