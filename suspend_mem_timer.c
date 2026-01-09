/* Copyright (c) 2021/08/28, Peter Boettcher, Germany/NRW, Muelheim Ruhr, mail:peter.boettcher@gmx.net
 * Urheber: 2021.08.28, Peter Boettcher, Germany/NRW, Muelheim Ruhr, mail:peter.boettcher@gmx.net

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



#include <linux/kernel.h>
#include <linux/sysctl.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/uidgid.h>
#include <linux/suspend.h>




#define PRINTK

/*------------------------------------------------------------------------------------*/
/* suspend to MEM */
static DEFINE_MUTEX(control);
static bool suspend_mem_active = false;
static s64 suspend_to_mem_time_sec = 0;



/* proto. */
struct suspend_mem_timer_info_struct {
	u64 suspend_to_mem_time_sec;
	bool suspend_mem_active;
};

/* Makes Compiler happy */
void info_suspend_mem_timer(struct suspend_mem_timer_info_struct *info);


/* DATA: Only over function */
void info_suspend_mem_timer(struct suspend_mem_timer_info_struct *info)
{
	info->suspend_to_mem_time_sec = suspend_to_mem_time_sec;
	info->suspend_mem_active = suspend_mem_active;
}



/* Suspend to MEM */
static struct task_struct *kt_suspend_to_mem_timer;


/*------------------------------------------------------------------------------------*/
/*
 * Kernel thread
 * verzoegerter suspend to ram timer
 */
static int suspend_to_mem_timer(void *data)
{

	int error;
	s64 diff_time;
	s64 old_unix_epoch_time_sec = ktime_get_real_seconds();

	/* wait */
	for (;;) {

		/* wait 5 sec */
		ssleep(5);

		mutex_lock(&control);

		diff_time = ktime_get_real_seconds() - old_unix_epoch_time_sec;

		if (diff_time <= 0) 
			suspend_to_mem_time_sec -= 5;
		else suspend_to_mem_time_sec -= diff_time;


		if (suspend_to_mem_time_sec <= 0) {
			mutex_unlock(&control);
			break;
		}

		old_unix_epoch_time_sec = ktime_get_real_seconds();

		mutex_unlock(&control);
	}


	for (;;) {
		error = pm_suspend(PM_SUSPEND_MEM);
		if (!error) break;

		ssleep(5);
		error = pm_suspend(PM_SUSPEND_TO_IDLE);
		if (!error) break;

		ssleep(5);
		error = pm_suspend(PM_SUSPEND_STANDBY);

		break;
	}


	if (error) {
		printk("SUSPEND TO MEM: ERROR!\n");
		suspend_mem_active = false;

		return -1;
	}

	printk("SUSPEND TO MEM: END! NOT ACTIVE!\n");

	suspend_mem_active = false;

	return 0;

}



static unsigned long suspend_to_mem_time_sec_temp = 0;
static int proc_set_suspend_mem_timer(const struct ctl_table *table,
				int write,
				void *buffer,
				size_t *lenp,
				loff_t *ppos)
{

	mutex_lock(&control);

	int retval = proc_doulongvec_minmax(table, write, buffer, lenp, ppos);

	if (write && retval != 0) {
		mutex_unlock(&control);
		return retval;
	}

	// Nur 30er Schritte/
	suspend_to_mem_time_sec_temp /= 30;
	suspend_to_mem_time_sec_temp *= 30;

	if ((s64) (ktime_get_real_seconds() + suspend_to_mem_time_sec_temp) <= 0) {
					printk("SUSPEND TO MEM: TIME NOT allowed\n");
					mutex_unlock(&control);
					return -1;
				}


	if (suspend_mem_active == false) {
		kt_suspend_to_mem_timer = kthread_create (suspend_to_mem_timer, NULL, "suspend_to_mem");
		if (kt_suspend_to_mem_timer == NULL) {
			printk("SUSPEND TO MEM:TIMER INIT ERROR\n");
			mutex_unlock(&control);
			return -1;
		}

		printk("SUSPEND TO MEM:TIMER INIT OK\n");
		// ACTIVE 
		suspend_to_mem_time_sec = suspend_to_mem_time_sec_temp;
		suspend_mem_active = true;
		wake_up_process(kt_suspend_to_mem_timer);
		mutex_unlock(&control);
		return 0;
	}

	suspend_to_mem_time_sec = suspend_to_mem_time_sec_temp;

	mutex_unlock(&control);

	return 0;
}




static unsigned long min = 30;
static unsigned long max = 0x7fffffffffffffff;
static const struct ctl_table suspend_mem_timer_table[] = {
	{
		.procname       = "suspend_mem_timer",
		.data           = &suspend_to_mem_time_sec_temp,
		.maxlen         = sizeof(unsigned long),
		.mode           = 0666,
		.proc_handler   = proc_set_suspend_mem_timer,
		.extra1		= &min,
		.extra2		= &max,

	},
};

static int __init suspend_mem_timer_init(void)
{
	register_sysctl_init("kernel/timer", suspend_mem_timer_table);
	return 0;
}
postcore_initcall(suspend_mem_timer_init);


