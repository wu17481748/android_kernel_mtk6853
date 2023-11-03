// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */
#include <linux/version.h>
#include "powerkey_monitor.h"
#include "theia_kevent_kernel.h"

#define POWER_MONITOR_DEBUG_PRINTK(a, arg...)\
	do {\
		printk("[powerkey_monitor]: " a, ##arg);\
	}while(0)

static char *flow_buf = NULL;
static char *flow_buf_curr = NULL;
static int flow_index = 0;
static int stage_start = 0;
#define FLOW_SIZE 16
#define STAGE_BRIEF_SIZE 64
#define STAGE_TOTAL_SIZE ((STAGE_BRIEF_SIZE)*(FLOW_SIZE))

static struct task_struct *block_thread = NULL;
static bool timer_started = false;
static int systemserver_pid = -1;

int get_systemserver_pid(void)
{
	return systemserver_pid;
}

/*bool is_valid_systemserver_pid(int pid)
{
    struct task_struct *task;

    if (pid < 0) {
        return false;
    }

    rcu_read_lock();
    task = find_task_by_vpid(pid);
    rcu_read_unlock();

    if (task != NULL) {
        const struct cred *tcred = __task_cred(task);
        if ((task->group_leader->pid == task->pid)
        && (tcred->uid.val == 1000)
        && (task->parent != 0 && !strcmp(task->parent->comm, "main"))) {
            return true;
        }
    }

    return false;
}

bool is_powerkey_check_valid(void)
{
    return is_valid_systemserver_pid(systemserver_pid);
}*/

static ssize_t powerkey_monitor_param_proc_read(struct file *file, char __user *buf,
        size_t count, loff_t *off)
{
	char page[512] = {0};
	int len = 0;

	len = sprintf(&page[len], "status=%d timeout_ms=%u is_panic=%d get_log=%d systemserver_pid=%d\n",
        g_black_data.status, g_black_data.timeout_ms, g_black_data.is_panic, g_black_data.get_log, systemserver_pid);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buf, page, (len < count ? len : count))) {
		return -EFAULT;
	}
	*off += len < count ? len : count;
	return (len < count ? len : count);
}

static bool handle_param_setup(char *key, char *value)
{
	bool ret = true;

	POWER_MONITOR_DEBUG_PRINTK("%s: setup param key:%s, value:%s\n", __func__, key, value);
	if (!strncmp(key, "state", 5)) {
		int state;
		if (sscanf(value, "%d", &state) == 1) {
			g_black_data.status = g_bright_data.status = state;
		}
	} else if (!strncmp(key, "timeout", 7)) {
		int timeout;
		if (sscanf(value, "%d", &timeout) == 1) {
			g_black_data.timeout_ms = g_bright_data.timeout_ms = timeout;
		}
	} else if (!strncmp(key, "log", 3)) {
		int get_log;
		if (sscanf(value, "%d", &get_log) == 1) {
			g_black_data.get_log = g_bright_data.get_log = get_log;
		}
	} else if (!strncmp(key, "panic", 5)) {
		int is_panic;
		if (sscanf(value, "%d", &is_panic) == 1) {
			g_black_data.is_panic = g_bright_data.is_panic = is_panic;
		}
	} else if (!strncmp(key, "systemserver_pid", 16)) {
		int s_pid;
		if (sscanf(value, "%d", &s_pid) == 1) {
			systemserver_pid = s_pid;
		}
	} else {
		ret = false;
	}

	return ret;
}

/*
param format:
state 4;timeout 20000;panic 0;log 1
systemserver_pid 32639
*/
static ssize_t powerkey_monitor_param_proc_write(struct file *file, const char __user *buf,
        size_t count, loff_t *off)
{
	char buffer[256] = {0};
	char *pBuffer = NULL;
	char *param;
	int ret = 0;

	if (count > 255) {
	    count = 255;
	}

	if (copy_from_user(buffer, buf, count)) {
		POWER_MONITOR_DEBUG_PRINTK("%s: read proc input error.\n", __func__);
		return count;
	}
	buffer[count] = '\0';
	pBuffer = buffer;

	POWER_MONITOR_DEBUG_PRINTK("%s: buffer:%s\n", __func__, buffer);

	while ((param = strsep(&pBuffer, ";"))) {
		char key[64] = {0}, value[64] = {0};
		ret = sscanf(param, "%s %s", key, value);
		POWER_MONITOR_DEBUG_PRINTK("%s: param:%s ret:%d key:%s value:%s\n", __func__, param, ret, key, value);
		if (ret == 2) {
			if (!handle_param_setup(key, value)) {
				POWER_MONITOR_DEBUG_PRINTK("%s: setup param fail! key:%s, value:%s\n", __func__, key, value);
			}
		}
	}

	return count;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
static int powerkey_monitor_param_proc_show(struct seq_file *seq_file, void *data) {
	seq_printf(seq_file, "%s called\n", __func__);
	return 0;
}

static int powerkey_monitor_param_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, powerkey_monitor_param_proc_show, NULL);
}
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
        static const struct proc_ops powerkey_monitor_param_proc_fops = {
		.proc_open = powerkey_monitor_param_proc_open,
		.proc_read = powerkey_monitor_param_proc_read,
		.proc_write = powerkey_monitor_param_proc_write,
		.proc_lseek = seq_lseek,
		.proc_release = single_release,
         };
#else
        struct file_operations powerkey_monitor_param_proc_fops = {
		.read = powerkey_monitor_param_proc_read,
		.write = powerkey_monitor_param_proc_write,
         };
#endif



ssize_t get_last_pwkey_stage(char *buf)
{
	if (stage_start != flow_index) {
		int last_index = (flow_index == 0) ? (FLOW_SIZE - 1) : (flow_index - 1);
		snprintf(buf, 64, (last_index*STAGE_BRIEF_SIZE + flow_buf));
	} else {
		sprintf(buf, "");
	}

	return strlen(buf);
}

ssize_t get_pwkey_stages(char *buf)
{
	char *buf_curr = NULL;
	int start_index = stage_start;
	int end_index = flow_index;

	if (start_index == end_index) {
		return 0;
	}

	buf_curr = start_index*STAGE_BRIEF_SIZE + flow_buf;
	POWER_MONITOR_DEBUG_PRINTK("get_pwkey_stages start_index:%d end_index:%d\n", start_index, end_index);

	while (start_index!= end_index) {
		strcat(buf, buf_curr);
		strcat(buf, ",");
		POWER_MONITOR_DEBUG_PRINTK("get_pwkey_stages buf:%s\n", buf);

		buf_curr += STAGE_BRIEF_SIZE;

		/*w lock index*/
		start_index++;
		if (start_index == FLOW_SIZE) {
			start_index = 0;
			buf_curr = flow_buf;
		}
	}

	return strlen(buf);
}

static ssize_t theia_powerkey_report_proc_read(struct file *file, char __user *buf,
        size_t count, loff_t *off)
{
	char stages[STAGE_TOTAL_SIZE] = {0};
	int stages_len;

	POWER_MONITOR_DEBUG_PRINTK("enter theia_powerkey_report_proc_read %d  %d", count, *off);

	stages_len = get_pwkey_stages(stages);

	return simple_read_from_buffer(buf, count, off, stages, stages_len);
}

void record_stage(const char *buf)
{
	if (!timer_started) {
		return;
	}

	POWER_MONITOR_DEBUG_PRINTK("%s: buf:%s\n", __func__, buf);

	memset(flow_buf_curr, 0, STAGE_BRIEF_SIZE);
	snprintf(flow_buf_curr, STAGE_BRIEF_SIZE, buf);
	flow_buf_curr+=STAGE_BRIEF_SIZE;

	/*w lock index*/
	flow_index++;
	if (flow_index == FLOW_SIZE) {
		flow_index = 0;
		flow_buf_curr = flow_buf;
	}
	/*w lock index*/

	/*flow_buf_curr+=STAGE_BRIEF_SIZE;*/
}

static ssize_t theia_powerkey_report_proc_write(struct file *file, const char __user *buf,
        size_t count, loff_t *off)
{
	char buffer[STAGE_BRIEF_SIZE] = {0};

	POWER_MONITOR_DEBUG_PRINTK("%s: read proc input called.\n", __func__);

	if(g_black_data.status == BLACK_STATUS_INIT || g_black_data.status == BLACK_STATUS_INIT_FAIL) {
		POWER_MONITOR_DEBUG_PRINTK("%s init not finish: status = %d\n", __func__, g_black_data.status);
		return count;
	}

	if (count >= STAGE_BRIEF_SIZE) {
		count = STAGE_BRIEF_SIZE - 1;
	}

	if (copy_from_user(buffer, buf, count)) {
		POWER_MONITOR_DEBUG_PRINTK("%s: read proc input error.\n", __func__);
		return count;
	}

	record_stage(buffer);
	return count;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
	static int theia_powerkey_report_proc_show(struct seq_file *seq_file, void *data) {
        	seq_printf(seq_file, "%s called\n", __func__);
        	return 0;
	}

	static int theia_powerkey_report_proc_open(struct inode *inode, struct file *file) {
		return single_open(file, theia_powerkey_report_proc_show, NULL);
	}
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
        static const struct proc_ops theia_powerkey_report_proc_fops = {
                .proc_open = theia_powerkey_report_proc_open,
                .proc_read = theia_powerkey_report_proc_read,
                .proc_write = theia_powerkey_report_proc_write,
		.proc_lseek = seq_lseek,
		.proc_release = single_release,
        };
#else
        struct file_operations theia_powerkey_report_proc_fops = {
                .read = theia_powerkey_report_proc_read,
                .write = theia_powerkey_report_proc_write,
        };
#endif



static ssize_t theia_powerkey_test_node_proc_read(struct file *file, char __user *buf,
        size_t count, loff_t *off)
{
	return 0;
}

static ssize_t theia_powerkey_test_node_proc_write(struct file *file, const char __user *buf,
        size_t count, loff_t *off)
{
	char buffer[128] = {0};
	if (count > 127 || copy_from_user(buffer, buf, count)) {
        POWER_MONITOR_DEBUG_PRINTK("%s: read proc input error.\n", __func__);
        return count;
	}

	POWER_MONITOR_DEBUG_PRINTK("theia_powerkey_test_node_proc_write buffer:%s\n", buffer);
	if (!strncmp(buffer, "test_d_block\n", 13)) {
        block_thread = get_current();
	    POWER_MONITOR_DEBUG_PRINTK("theia_powerkey_test_node_proc_write set TASK_UNINTERRUPTIBLE block_thread pid:%d\n", block_thread->pid);
        set_current_state(TASK_UNINTERRUPTIBLE);
        schedule();
        POWER_MONITOR_DEBUG_PRINTK("theia_powerkey_test_node_proc_write set TASK_UNINTERRUPTIBLE, after schedule\n");
        block_thread = NULL;
	} else if (!strncmp(buffer, "test_d_unblock\n", 15)) {
        if (block_thread != NULL) {
	        POWER_MONITOR_DEBUG_PRINTK("theia_powerkey_test_node_proc_write call wake_up_process pid:%d\n", block_thread->pid);
	        wake_up_process(block_thread);
        }
	} else if (!strncmp(buffer, "test_d_unblock_with_kill\n", 25)) {
/*
        if (block_thread != NULL) {
            POWER_MONITOR_DEBUG_PRINTK("theia_powerkey_test_node_proc_write call wake_up_process with kill pid:%d\n", block_thread->pid);
            block_thread->flags |= PF_KILLING;
            do_send_sig_info(SIGKILL, SEND_SIG_FORCED, block_thread, true);
            wake_up_process(block_thread);
        }
*/
	} else if (!strncmp(buffer, "test_blackscreen_dcs\n", 21)) {
        send_black_screen_dcs_msg();
	} else if (!strncmp(buffer, "test_kevent\n", 12)) {
        SendTheiaKevent(THEIA_KEVENT_TYPE_COMMON_STRING, "logTagTest", "eventIDTest", "Powerkey test kevent!");
	}

	return count;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
	static int theia_powerkey_test_node_proc_open(struct inode *inode, struct file *file) {
		int ret;

		ret = nonseekable_open(inode, file);
		if (unlikely(ret))
			return ret;
		file->private_data = NULL;

		return 0;
	}
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
	static const struct proc_ops theia_powerkey_test_node_proc_fops = {
                .proc_open = theia_powerkey_test_node_proc_open,
                .proc_read = theia_powerkey_test_node_proc_read,
                .proc_write = theia_powerkey_test_node_proc_write,
        };

#else
	struct file_operations theia_powerkey_test_node_proc_fops = {
                .read = theia_powerkey_test_node_proc_read,
                .write = theia_powerkey_test_node_proc_write,
        };
#endif

void theia_pwk_stage_start(char *reason)
{
	POWER_MONITOR_DEBUG_PRINTK("theia_pwk_stage_start start %x:  %x   %x   flow_buf\n", flow_buf, flow_buf_curr, flow_index);
	stage_start = flow_index;
	timer_started = true;
	record_stage(reason);
}

void theia_pwk_stage_end(char *reason)
{
	if (timer_started) {
		POWER_MONITOR_DEBUG_PRINTK("theia_pwk_stage_end, reason:%s\n", reason);
		record_stage(reason);
		timer_started = false;
	}
}


static bool is_zygote_process(struct task_struct *t)
{
	const struct cred *tcred = __task_cred(t);
	if(!strncmp(t->comm, "main", 4) && (tcred->uid.val == 0) &&
	    (t->parent != 0 && !strncmp(t->parent->comm, "init", 4))) {
        return true;
	} else {
		return false;
	}
	return false;
}
extern void touch_all_softlockup_watchdogs(void);

static void show_coretask_state(void)
{
	struct task_struct *g, *p;

	rcu_read_lock();
	for_each_process_thread(g, p) {
		if (is_zygote_process(p) || !strncmp(p->comm, "system_server", TASK_COMM_LEN)
			|| !strncmp(p->comm, "surfaceflinger", TASK_COMM_LEN)) {
#if IS_MODULE(CONFIG_OPLUS_FEATURE_THEIA)
			touch_nmi_watchdog();
#endif
			sched_show_task(p);
		}
	}

#if IS_BUILTIN(CONFIG_OPLUS_FEATURE_THEIA)
	touch_all_softlockup_watchdogs();
#endif
	rcu_read_unlock();
}

void doPanic(void)
{
	/*1.meminfo*/
	/*2. show all D task*/
#if IS_MODULE(CONFIG_OPLUS_FEATURE_THEIA)
	handle_sysrq('w');
#else
	show_state_filter(TASK_UNINTERRUPTIBLE);
#endif
	/*3. show system_server zoygot surfacefliger state*/
	show_coretask_state();
	/*4.current cpu registers :skip for minidump*/
	panic("bright screen detected, force panic");
}

int __init powerkey_monitor_init(void)
{
	POWER_MONITOR_DEBUG_PRINTK("powerkey_monitor_init\n");

	theia_kevent_module_init();
	bright_screen_check_init();
	black_screen_check_init();

	/*a node for param setup*/
	proc_create("pwkMonitorParam", S_IRWXUGO, NULL, &powerkey_monitor_param_proc_fops);

	/*a node for normal stage record*/
	proc_create("theiaPwkReport", S_IRWXUGO, NULL, &theia_powerkey_report_proc_fops);

	/*a node fo test*/
	/*proc_create("theiaPwkTestNode", S_IRWXUGO, NULL, &theia_powerkey_test_node_proc_fops);*/

	flow_buf = vmalloc(STAGE_BRIEF_SIZE*FLOW_SIZE);
	if (!flow_buf) {
		return -ENOMEM;
	}
	memset(flow_buf, 0, STAGE_BRIEF_SIZE*FLOW_SIZE);
	flow_buf_curr = flow_buf;

	return 0;
}

void __exit powerkey_monitor_exit(void)
{
	POWER_MONITOR_DEBUG_PRINTK("powerkey_monitor_exit\n");

	black_screen_exit();
	bright_screen_exit();
	theia_kevent_module_exit();

	if (!flow_buf) {
		vfree(flow_buf);
		flow_buf = NULL;
	}
}

late_initcall(powerkey_monitor_init);
module_exit(powerkey_monitor_exit);

MODULE_AUTHOR("jianping.zheng <jianping.zheng@oplus.com>");
MODULE_DESCRIPTION("powerkey_monitor@1.0");
MODULE_VERSION("1.0");
#if IS_MODULE(CONFIG_OPLUS_FEATURE_THEIA)
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif
MODULE_LICENSE("GPL v2");
