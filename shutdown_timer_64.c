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

#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>



#ifndef cache_flush
#define cache_flush
#endif


#ifdef cache_flush

#ifdef CONFIG_X86
#include <asm/processor.h>
#include <asm/special_insns.h>
#endif

#ifdef CONFIG_ARM64
#include <asm/cacheflush.h>
#endif

#endif


/*------------------------------------------------------------------------------------*/
/* shutdown */
static DEFINE_MUTEX(control);

static bool shutdown_timer_active = false;
static s64 shutdown_time_sec = -1;
static u64 shutdown_time_sec_temp = -1;

/* Shutdown */
static struct task_struct *kt_shutdown_timer;



/*------------------------------------------------------------------------------------*/
/*
 * Kernel thread
 */
static int shutdown_timer_64(void *data)
{

	s64 diff_time;
	s64 old_unix_epoch_time_sec;
	/*
	Time in Sec.
	Seconds are adjusted to the entered system time,
	regardless of whether the time is set forward or
	backward. Not monotone.
	*/

	/*
	unix epoche time
	1970.01.01
	*/

	/*
	32Bit
	2028.01.19 03:14:07 overflow
	*/

	/*
	Max set: date 041901472232	2062.04.2262 01:47
	*/

	old_unix_epoch_time_sec = ktime_get_real_seconds();

	/* wait */
	for (;;) {

		/* wait 5 sec */
		ssleep(5);

		mutex_lock(&control);

		diff_time = ktime_get_real_seconds() - old_unix_epoch_time_sec;

		/*
		test if system time is entered backward. this is not allowed!
		time set forward is allowed
		*/
		if (diff_time < 5) 
			shutdown_time_sec -= 5;
		else shutdown_time_sec -= diff_time;


		if (shutdown_time_sec <= 0) {
			mutex_unlock(&control);
			break;
		}

		old_unix_epoch_time_sec = ktime_get_real_seconds();

		mutex_unlock(&control);
	}

	/* Shutdown/Halt */
	/* if shutdown etc fails. never work with this computer */
	for (;;) {


#ifdef cache_flush

/* 1. X86 (Intel/AMD): Der globale Hammer */
#ifdef CONFIG_X86
/*
 * Schreibt alle dirty Lines zurueck und macht ALLE Caches (L1-L3) sofort ungueltig.
 * Signalisiert auch externen Caches (Mainboard), dasselbe zu tun. 
*/
		wbinvd(); 
#endif

/* 2. ARM64 (z.B. Apple M-Serie, Snapdragon): Monotone Bereinigung */
#ifdef CONFIG_ARM64
/*
 * ARM erlaubt oft kein globales 'Invalidate' aus dem Kernel ohne Memory-Ranges.
 * Wir nutzen die Kernel-Funktion, um den Kernel-Adressraum zu flashen. 
*/
		flush_cache_all(); 
/* 
 * Zusaetzlicher Barrier-Befehl, um sicherzustellen, dass die CPU wartet,
 * bis alle Cache-Operationen physikalisch abgeschlossen sind. 
*/
		dsb(ish);
		isb();
#endif

#endif

		kernel_power_off();
		ssleep(1);

		kernel_halt();			/* if error */
		ssleep(1);

		kernel_restart(" ");		/* if error */
		ssleep(1);
	}

	return 0;
}




static int proc_set_shutdown_timer_64(const struct ctl_table *table,
					int write,
					void *buffer,
					size_t *lenp,
					loff_t *ppos)
{

	int retval;

	mutex_lock(&control);

	retval = proc_doulongvec_minmax(table, write, buffer, lenp, ppos);

	if (write && retval != 0) {
		mutex_unlock(&control);
		return retval;
	}

	/* Only 30er steps */
	shutdown_time_sec_temp /= 30;
	shutdown_time_sec_temp *= 30;

	/* overflow or begin epochtime + 30? */
	if (shutdown_time_sec_temp < 30) {
		printk("SHUTDOWN TIMER64: Time < 30s not allowed.\n");
		mutex_unlock(&control);
		return -EINVAL;
	}

	/* Timer inaktiv → Thread start */
	if (shutdown_timer_active == false) {
		kt_shutdown_timer = kthread_create(shutdown_timer_64, NULL, "pari64");
		if (IS_ERR(kt_shutdown_timer)) {
			printk("SHUTDOWN TIMER64: ERROR create Thread.\n");
			mutex_unlock(&control);
			return -ECHILD;
		}

		printk("SHUTDOWN TIMER64: Thread start ok.\n");
		printk("Countdown64     : %llds\n", shutdown_time_sec_temp);

		shutdown_time_sec = (s64) shutdown_time_sec_temp;

		shutdown_timer_active = true;

		mutex_unlock(&control);
		wake_up_process(kt_shutdown_timer);
		return 0;
	}

	/* Timer active */
	/* if new time => old time -> error */
	if (shutdown_time_sec_temp >= shutdown_time_sec) {
		printk("SHUTDOWN TIMER64: NEW Time is not allowed.\n");
		mutex_unlock(&control);
		return -EINVAL;
	}

	printk("SHUTDOWN TIMER64: New Countdown.\n");
	printk("COUNTDOWN64     : %llds \n", shutdown_time_sec_temp);

	shutdown_time_sec = (s64) shutdown_time_sec_temp;

	mutex_unlock(&control);

	return 0;
}



static unsigned long min = 30;
static unsigned long max = 0x7fffffffffffffff;
static const struct ctl_table shutdown_timer_table[] = {
	{
		.procname       = "shutdown_timer64",
		.data           = &shutdown_time_sec_temp,
		.maxlen         = sizeof(unsigned long),
		.mode           = 0200,
		.proc_handler   = proc_set_shutdown_timer_64,
		.extra1		= &min,
		.extra2		= &max,
	},
};


static int __init shutdown_timer_init_64(void)
{
	register_sysctl_init("kernel/timer", shutdown_timer_table);
	return 0;
}
postcore_initcall(shutdown_timer_init_64);



/*------------------------------------------------------------------------------------*/
static int shutdown_info_proc_show_64(struct seq_file *proc_show, void *v)
{

	if (shutdown_timer_active == true) {
		seq_printf(proc_show,
			"SHUTDOWN TIMER64: ACTIVE. TIME BASE 5 SEC.\n");

		seq_printf(proc_show,
			"SHUTDOWN TIMER64: %lld:%lld:%lld:%lld YEAR:DAY:HOUR:MIN\n",
				(shutdown_time_sec / 365 / 24 / 3600),	/* years */
				((shutdown_time_sec  / 3600 / 24) % 365),	/* days */
				((shutdown_time_sec  / 3600) % 24),		/* hours */
				((shutdown_time_sec  / 60) % 60));		/* mIN */

		seq_printf(proc_show,
			"SHUTDOWN TIMER64: %lld SEC.\n", shutdown_time_sec);

	} else {
		seq_printf(proc_show,
			"SHUTDOWN TIMER64: NOT ACTIVE\n\n");
	}

	return(0);
}


static int __init init_shutdown_timer_info_proc_show(void)
{
	proc_create_single("stat.shutdown.timer.64", 0, NULL, shutdown_info_proc_show_64);
	return(0);
}
fs_initcall(init_shutdown_timer_info_proc_show);


