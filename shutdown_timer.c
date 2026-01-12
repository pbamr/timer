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



/*------------------------------------------------------------------------------------*/
/* shutdown */
static DEFINE_MUTEX(control);
static bool shutdown_active = false;
static s64 shutdown_time_sec = 0;



/* proto. */
struct shutdown_timer_info_struct_sysctl {
	s64 shutdown_time_sec;
	bool shutdown_active;
};

/* Makes Compiler happy */
void info_shutdown_timer_sysctl(struct shutdown_timer_info_struct_sysctl *info);


/* DATA: Only over function */
void info_shutdown_timer_sysctl(struct shutdown_timer_info_struct_sysctl *info)
{
	info->shutdown_time_sec = shutdown_time_sec;
	info->shutdown_active = shutdown_active;
}



/* Suspend to MEM */
static struct task_struct *kt_shutdown_timer;


/*------------------------------------------------------------------------------------*/
/*
 * Kernel thread
 * verzoegerter suspend to ram timer
 */
static int shutdown_timer(void *data)
{

	/* monoton vorwaertslaufend. Auch bei vorherigem sleep wird die Zeit angepasst */
	/* erkennt vorheriges schlafen */
	s64 start_time_sec = ktime_get_boottime_seconds();

	/* wait */
	for (;;) {

		/* wait 5 sec */
		ssleep(5);

		mutex_lock(&control);

		shutdown_time_sec -= ktime_get_boottime_seconds() - start_time_sec;

		if (shutdown_time_sec <= 0) {
			mutex_unlock(&control);
			break;
		}

		start_time_sec = ktime_get_boottime_seconds();

		mutex_unlock(&control);

	}

	/* Shutdown/Halt */
	while (1) {
		kernel_power_off();
		kernel_halt();			/* if error */
		kernel_restart(" ");		/* if error */
		ssleep(3);
	}

	return 0;

}


static unsigned long init_shutdown_time_sec = 0;
static int proc_init_shutdown_timer(const struct ctl_table *table,
				int write,
				void *buffer,
				size_t *lenp,
				loff_t *ppos)
{

	if (shutdown_active == true) return -1;

	mutex_lock(&control);

	int retval = proc_doulongvec_minmax(table, write, buffer, lenp, ppos);

	if (write && retval != 0) {
		mutex_unlock(&control);
		return retval;
	}

	// Nur 30er Schritte/
	init_shutdown_time_sec /= 30;
	init_shutdown_time_sec *= 30;

	if ((s64) (ktime_get_boottime_seconds() + init_shutdown_time_sec) < 30) {
					printk("SHUTDOWN: TIME NOT allowed\n");
					mutex_unlock(&control);
					return -1;
				}


	kt_shutdown_timer = kthread_create (shutdown_timer, NULL, "pari");
	if (kt_shutdown_timer == NULL) {
		printk("SHUTDOWN:TIMER INIT ERROR\n");
		mutex_unlock(&control);
		return -1;
	}

	pr_info("SHUTDOWN:TIMER INIT OK\n");
	// ACTIVE 
	shutdown_time_sec = init_shutdown_time_sec;
	shutdown_active = true;
	mutex_unlock(&control);
	wake_up_process(kt_shutdown_timer);

	return 0;
}



static unsigned long new_shutdown_time_sec = 0;
static int proc_new_shutdown_timer(const struct ctl_table *table,
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
	new_shutdown_time_sec /= 30;
	new_shutdown_time_sec *= 30;

	if ((s64) (ktime_get_boottime_seconds() + init_shutdown_time_sec) < 30) {
					printk("SHUTDOWN: TIME NOT allowed\n");
					mutex_unlock(&control);
					return -1;
				}


	if (new_shutdown_time_sec > shutdown_time_sec) {
		printk("SHUTDOWN: TIME NOT allowed.\n");
		mutex_unlock(&control);
		return -1;
	}


	shutdown_time_sec = new_shutdown_time_sec;
	printk("SHUTDOWN: TIME allowed.\n");

	mutex_unlock(&control);

	return 0;
}






static unsigned long min = 30;
static unsigned long max = 0x7fffffffffffffff;
static const struct ctl_table shutdown_timer_table[] = {
	{
		.procname       = "init_shutdown_timer",
		.data           = &init_shutdown_time_sec,
		.maxlen         = sizeof(unsigned long),
		.mode           = 0600,
		.proc_handler   = proc_init_shutdown_timer,
		.extra1		= &min,
		.extra2		= &max,
	},
	{
		.procname       = "set_new_shutdown_timer",
		.data           = &new_shutdown_time_sec,
		.maxlen         = sizeof(unsigned long),
		.mode           = 0600,
		.proc_handler   = proc_new_shutdown_timer,
		.extra1		= &min,
		.extra2		= &max,
	},



};

static int __init shutdown_timer_init(void)
{
	register_sysctl_init("kernel/timer", shutdown_timer_table);
	return 0;
}
postcore_initcall(shutdown_timer_init);

