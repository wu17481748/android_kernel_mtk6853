// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */
#include <linux/sched.h>
#include <linux/sysctl.h>
#include <linux/reciprocal_div.h>
#include <linux/topology.h>
#include <../kernel/sched/sched.h>
#include <linux/cpufreq.h>

#include "prefer_silver.h"

int sysctl_prefer_silver = 0;
int sysctl_heavy_task_thresh = 50;
int sysctl_cpu_util_thresh = 85;
int sysctl_silver_trigger_freq = 1503000;

#ifdef OPLUS_FEATURE_SCHED_ASSIST
extern bool test_task_ux(struct task_struct *task);
#endif

bool prefer_silver_check_ux(struct task_struct *task) {
	bool is_ux = false;
#ifdef OPLUS_FEATURE_SCHED_ASSIST
	is_ux = test_task_ux(current);
#endif
	return is_ux;
}

bool prefer_silver_check_freq(int cpu)
{
	unsigned int freq = 0;
	freq = cpufreq_quick_get(cpu);

	return freq < sysctl_silver_trigger_freq;
}

static inline unsigned long task_util(struct task_struct *p)
{
#ifdef CONFIG_SCHED_WALT
	static int sched_use_walt_nice = 101;
	if (!walt_disabled && (sysctl_sched_use_walt_task_util ||
								p->prio < sched_use_walt_nice)) {
		unsigned long demand = p->ravg.demand;
		return (demand << SCHED_CAPACITY_SHIFT) / walt_ravg_window;
	}
#endif
	return p->se.avg.util_avg;
}

static inline unsigned long cpu_util(int cpu)
{
	struct cfs_rq *cfs_rq;
	unsigned int util;

#ifdef CONFIG_SCHED_WALT
	if (likely(!walt_disabled && sysctl_sched_use_walt_cpu_util)) {
		u64 walt_cpu_util = cpu_rq(cpu)->cumulative_runnable_avg;

		walt_cpu_util <<= SCHED_CAPACITY_SHIFT;
		do_div(walt_cpu_util, walt_ravg_window);

		return min_t(unsigned long, walt_cpu_util,
			     capacity_orig_of(cpu));
	}
#endif

	cfs_rq = &cpu_rq(cpu)->cfs;
	util = READ_ONCE(cfs_rq->avg.util_avg);

	if (sched_feat(UTIL_EST))
		util = max(util, READ_ONCE(cfs_rq->avg.util_est.enqueued));

	return min_t(unsigned long, util, capacity_orig_of(cpu));
}


bool prefer_silver_check_task_util(struct task_struct *p)
{
	int cpu;
	unsigned long thresh_load;
	struct reciprocal_value spc_rdiv = reciprocal_value(100);

	if (!p)
		return false;

	cpu = task_cpu(p);
	thresh_load = capacity_orig_of(cpu) * sysctl_heavy_task_thresh;
	if(task_util(p) <  reciprocal_divide(thresh_load,spc_rdiv) ||
			scale_demand(p->ravg.sum) < reciprocal_divide(thresh_load,spc_rdiv))
		return true;

	return false;
}

bool prefer_silver_check_cpu_util(int cpu)
{
	return  (capacity_orig_of(cpu) * sysctl_cpu_util_thresh) >
		(cpu_util(cpu) * 100);
}
