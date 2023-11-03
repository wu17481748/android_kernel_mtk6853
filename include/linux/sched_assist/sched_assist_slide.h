/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */


#ifndef _OPLUS_SCHED_SLIDE_H_
#define _OPLUS_SCHED_SLIDE_H_

#include <linux/version.h>
#include "sched_assist_common.h"

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)) && defined(CONFIG_OPLUS_SYSTEM_KERNEL_QCOM)
extern unsigned int sched_ravg_window;
extern unsigned int walt_scale_demand_divisor;
#else
#ifdef CONFIG_HZ_300
#define DEFAULT_SCHED_RAVG_WINDOW (3333333 * 6)
#else
#define DEFAULT_SCHED_RAVG_WINDOW 20000000
#endif
#define sched_ravg_window DEFAULT_SCHED_RAVG_WINDOW
extern unsigned int walt_ravg_window;
#define walt_scale_demand_divisor (walt_ravg_window >> SCHED_CAPACITY_SHIFT)
#endif
#define scale_demand(d) ((d)/walt_scale_demand_divisor)

extern int sysctl_slide_boost_enabled;
extern int sysctl_boost_task_threshold;
extern int sysctl_frame_rate;

extern bool oplus_task_misfit(struct task_struct *p, int cpu);
extern bool _slide_task_misfit(struct task_struct *p, int cpu);
extern u64 _slide_get_boost_load(int cpu);
void _slide_find_start_cpu(struct root_domain *rd, struct task_struct *p, int *start_cpu);

extern void slide_calc_boost_load(struct rq *rq, unsigned int *flag, int cpu);
extern bool should_adjust_slide_task_placement(struct task_struct *p, int cpu);
extern int sched_frame_rate_handler(struct ctl_table *table, int write, void __user *buffer, size_t *lenp, loff_t *ppos);
extern int sysctl_sched_slide_boost_handler(struct ctl_table *table, int write, void __user *buffer, size_t *lenp, loff_t *ppos);
extern int sysctl_sched_animation_type_handler(struct ctl_table *table, int write, void __user *buffer, size_t *lenp, loff_t *ppos);
extern int sysctl_sched_boost_task_threshold_handler(struct ctl_table *table, int write, void __user *buffer, size_t *lenp, loff_t *ppos);
extern int sysctl_sched_assist_input_boost_ctrl_handler(struct ctl_table * table, int write, void __user * buffer, size_t * lenp, loff_t * ppos);
extern bool keep_slide_nolimit(struct task_struct *p);

static bool inline check_slide_scene(void)
{
	return sched_assist_scene(SA_SLIDE)
		|| sched_assist_scene(SA_LAUNCHER_SI)
		|| sched_assist_scene(SA_INPUT)
		|| sched_assist_scene(SA_ANIM);
}

static bool inline slide_task_misfit(struct task_struct *p, int cpu)
{
	return check_slide_scene() && (is_heavy_ux_task(p) || is_sf(p)) &&
		oplus_task_misfit(p, cpu);
}

static void inline slide_set_start_cpu(struct root_domain *rd, struct task_struct *p, int *start_cpu)
{
	if (check_slide_scene() && (is_heavy_ux_task(p) || is_sf(p)))
		_slide_find_start_cpu(rd, p, start_cpu);
}

static void inline slide_set_task_cpu(struct task_struct *p, int *best_energy_cpu)
{
	if (should_adjust_slide_task_placement(p, *best_energy_cpu))
		find_ux_task_cpu(p, best_energy_cpu);
}

static void inline slide_set_boost_load(u64 *load, int cpu)
{
	u64 tmpload = *load;

	if (check_slide_scene()) {
		tmpload = max_t(u64, tmpload, _slide_get_boost_load(cpu));
		*load = tmpload;
	}
}
#endif /* _OPLUS_SCHED_SLIDE_H_ */
