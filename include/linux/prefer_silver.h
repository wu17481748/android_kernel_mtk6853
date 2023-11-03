/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _OPLUS_PREFER_SILVER_H_
#define _OPLUS_PREFER_SILVER_H_

#ifdef CONFIG_SCHED_WALT
extern unsigned int walt_ravg_window;
#define walt_scale_demand_divisor (walt_ravg_window >> SCHED_CAPACITY_SHIFT)
#define scale_demand(d) ((d)/walt_scale_demand_divisor)
#endif

extern int sysctl_prefer_silver;
extern int sysctl_heavy_task_thresh;
extern int sysctl_cpu_util_thresh;
extern int sysctl_silver_trigger_freq;

extern bool prefer_silver_check_ux(struct task_struct *task);

extern bool prefer_silver_check_freq(int cpu);
extern bool prefer_silver_check_task_util(struct task_struct *p);
extern bool prefer_silver_check_cpu_util(int cpu);

#endif /*_OPLUS_PREFER_SILVER_H_*/
