// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */


#include <linux/sched.h>
#include <linux/version.h>
#include <../kernel/sched/sched.h>

#define WALT_RT_PULL_THRESHOLD_NS 250000
#define CPUSET_AUDIO_APP (7)

static inline int has_pushable_tasks(struct rq *rq)
{
	return !plist_head_empty(&rq->rt.pushable_tasks);
}

static int pick_rt_task(struct rq *rq, struct task_struct *p, int cpu)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0))
	if (!task_running(rq, p) && cpumask_test_cpu(cpu, p->cpus_ptr))
		return 1;
#else
	if (!task_running(rq, p) && cpumask_test_cpu(cpu, &p->cpus_allowed))
		return 1;
#endif

	return 0;
}

static struct task_struct *pick_highest_pushable_task(struct rq *rq, int cpu)
{
	struct plist_head *head = &rq->rt.pushable_tasks;
	struct task_struct *p;

	if (!has_pushable_tasks(rq))
		return NULL;

	plist_for_each_entry(p, head, pushable_tasks) {
		if (pick_rt_task(rq, p, cpu))
			return p;
	}

	return NULL;
}


static inline bool is_audio_app_group(struct task_struct *p)
{
#ifdef CONFIG_CGROUP_SCHED
	return task_css(p, cpuset_cgrp_id)->id == CPUSET_AUDIO_APP;
#else
	return false;
#endif
}

#ifdef CONFIG_SCHED_WALT
bool oplus_rt_new_idle_balance(struct rq *this_rq, u64 wallclock)
{
	int i, this_cpu = this_rq->cpu, src_cpu = this_cpu;
	struct rq *src_rq;
	struct task_struct *p;
	bool pulled = false;

	/* can't help if this has a runnable RT */
	if (this_rq->rt.rt_queued > 0)
		return false;

	/* check if any CPU has a pushable RT task */
	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);

		if (!has_pushable_tasks(rq))
			continue;
		src_cpu = i;
		break;
	}

	if (src_cpu == this_cpu)
		return false;

	src_rq = cpu_rq(src_cpu);
	double_lock_balance(this_rq, src_rq);

	/* lock is dropped, so check again */
	if (this_rq->rt.rt_queued > 0)
		goto unlock;

	p = pick_highest_pushable_task(src_rq, this_cpu);

	if (!p)
		goto unlock;

	/* we only allow audio-app group task doing this work */
	if (!is_audio_app_group(p))
		goto unlock;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0))
	if (wallclock - p->wts.last_wake_ts < WALT_RT_PULL_THRESHOLD_NS)
		goto unlock;
#else
	if (wallclock - p->last_wake_ts < WALT_RT_PULL_THRESHOLD_NS)
		goto unlock;
#endif

	pulled = true;
	deactivate_task(src_rq, p, 0);
	set_task_cpu(p, this_cpu);
	activate_task(this_rq, p , 0);
unlock:
	double_unlock_balance(this_rq, src_rq);

	return pulled;
}
#endif
