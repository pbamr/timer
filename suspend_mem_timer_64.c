

/*
 * Suspend-to-MEM Timer
 *
 * Autor: Peter Boettcher, 2021-08-28, Muelheim an der Ruhr, Deutschland
 * Mail:  peter.boettcher@gmx.net
 *
 * Beschreibung:
 * ------------
 * Dieses Feature erlaubt es, das System nach einer konfigurierbaren Zeit
 * automatisch in den Suspend-to-Memory-Zustand (S3/S2Idle/Standby) zu versetzen.
 *
 * Implementierungshinweise:
 * -------------------------
 * - Die Zeit wird in Sekunden ueber /proc/sys/kernel/timer/suspend_mem_timer gesetzt.
 * - Die Zeit wird in 30-Sekunden-Schritten gerundet.
 * - Es wird die *reale Systemzeit* (Systemclock, `ktime_get_real_seconds()`) verwendet,
 *   um auch Uhrzeit-Aenderungen (z. B. NTP oder manuelle Zeitkorrekturen) zu erkennen:
 *     → Wird die Uhr vorgestellt, verkuerezt sich der Timer entsprechend.
 *     → Wird die Uhr zurueckgestellt, wird dies ignoriert (Zeit laeuft normal weiter).
 *
 *   Damit kann der Timer korrekt auf Systemzeitaenderungen reagieren,
 *   bleibt aber robust gegenueber rueckwaertsgestellten Uhren.
 *
 * - Der Thread beendet sich immer sauber selbst:
 *     → nach erfolgreichem Suspend und Resume
 *     → oder wenn alle Suspend-Versuche fehlschlagen
 *
 * Kein expliziter kthread_stop() noetig, da kein Modul-Unload.
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
#include <linux/atomic.h>

#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>



/* ------------------------------------------------------------------------------------ */
/* Global Sync and State */

static DEFINE_MUTEX(control);

static bool suspend_mem_active = false;

/* time in sec. */
static s64 suspend_to_mem_time_sec = 0;

/* temp time sec. /proc/sys/kernel.. */
static unsigned long suspend_to_mem_time_sec_temp = 0;

/* Threadhandle */
static struct task_struct *kt_suspend_to_mem_timer;


/* ------------------------------------------------------------------------------------ */
/*
 *	Time in Sec.
 *	Seconds are adjusted to the entered system time,
 *	regardless of whether the time is set forward or
 *	backward. Not monotone.
 *	*/

/*
 *	unix epoche time
 *	1970.01.01

 *	KERNEL 5.6 sicher
 *	glib 2.34
 *
*/

/*
 * *	Max set: date 041901472232 -> 2232.04.19:01:47
*/

static int suspend_to_mem_timer(void *data)
{
	int error;
	s64 diff_time;
	s64 old_unix_epoch_time_sec = ktime_get_real_seconds();

	/*
	Time in Sec.
	Seconds are adjusted to the entered system time,
	regardless of whether the time is set forward or
	backward. Not monotone.
	*/

	for (;;) {
		/* alle 5 Sekunden pruefen */
		ssleep(5);

		mutex_lock(&control);

		diff_time = ktime_get_real_seconds() - old_unix_epoch_time_sec;

		/*
		test if system time is entered backward. this is not allowed!
		time set forward is allowed
		*/

		/*
		 * Zeit wurde zurueckgestellt → Differenz < 5
		 * → ignoriere Rueckstellung, ziehe nur 5s ab
		 */
		if (diff_time < 5)
			suspend_to_mem_time_sec -= 5;
		else
			suspend_to_mem_time_sec -= diff_time;

		if (suspend_to_mem_time_sec <= 0) {
			mutex_unlock(&control);
			break;
		}

		old_unix_epoch_time_sec = ktime_get_real_seconds();
		mutex_unlock(&control);
	}

	/* Versuche Suspend in verschiedenen Modi */
	for (;;) {
		error = pm_suspend(PM_SUSPEND_MEM);
		if (!error)
			break;

		ssleep(5);
		error = pm_suspend(PM_SUSPEND_TO_IDLE);
		if (!error)
			break;

		ssleep(5);
		error = pm_suspend(PM_SUSPEND_STANDBY);
		break;
	}

	if (error) {
		printk("suspend_mem_timer: ERROR!\n");
		suspend_mem_active = false;
		return -1;
	}

	printk("suspend_mem_timer: Suspend end, Timer stop!\n");
	suspend_mem_active = false;

	return 0;
}

/* ------------------------------------------------------------------------------------ */
/*
 * proc_set_suspend_mem_timer()
 *
 * Wird aufgerufen, wenn /proc/sys/kernel/timer/suspend_mem_timer beschrieben wird.
 * Erwartet Sekundenwert (unsigned long).
 *
 * Verhalten:
 *  - Rundung auf 30s-Schritte
 *  - Minimalwert 30s
 *  - Wenn Timer inaktiv → Thread starten
 *  - Wenn aktiv → Zeit aktualisieren
 */

static int proc_set_suspend_mem_timer(const struct ctl_table *table,
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

	/* Rundung auf 30s-Schritte */
	suspend_to_mem_time_sec_temp -= suspend_to_mem_time_sec_temp % 30;

	/* Minimalwert pruefen */
	if (suspend_to_mem_time_sec_temp < 30) {
		printk("SUPEND MEM TIMER64: Time < 30s not allowed.\n");
		mutex_unlock(&control);
		return -EINVAL;
	}

	/* Wenn Timer inaktiv → Thread starten */
	if (suspend_mem_active == false) {
		kt_suspend_to_mem_timer = kthread_create(suspend_to_mem_timer, NULL, "suspend_to_mem_64");
		if (IS_ERR(kt_suspend_to_mem_timer)) {
			printk("SUPEND MEM TIMER64: ERROR create Thread.\n");
			mutex_unlock(&control);
			return -ECHILD;
		}

		printk("SUPEND MEM TIMER64: Thread start ok.\n");
		printk("Countdown64       : %lus.\n", suspend_to_mem_time_sec_temp);

		suspend_to_mem_time_sec = suspend_to_mem_time_sec_temp;

		suspend_mem_active = true;

		mutex_unlock(&control);
		wake_up_process(kt_suspend_to_mem_timer);
		return 0;
	}

	printk("SUPEND MEM TIMER64: New Countdown.\n");
	printk("Countdown64       : %lus.\n", suspend_to_mem_time_sec_temp);

	/* Timer laeuft bereits → Zeit aktualisieren */
	suspend_to_mem_time_sec = suspend_to_mem_time_sec_temp;

	mutex_unlock(&control);

	return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Sysctl-Tabelle fuer /proc/sys/kernel/timer/suspend_mem_timer */

static unsigned long min = 30;
static unsigned long max = 0x7fffffffffffffff;
static const struct ctl_table suspend_mem_timer_table[] = {
	{
		.procname       = "suspend_mem_timer64",
		.data           = &suspend_to_mem_time_sec_temp,
		.maxlen         = sizeof(unsigned long),
		.mode           = 0666,
		.proc_handler   = proc_set_suspend_mem_timer,
		.extra1		= &min,
		.extra2		= &max,
	},
};



/* ------------------------------------------------------------------------------------ */
/* Initialisierung – wird beim Kernelstart automatisch aufgerufen */

static int __init suspend_mem_timer_init_64(void)
{
	register_sysctl_init("kernel/timer", suspend_mem_timer_table);
	pr_info("suspend_mem_timer: Sysctl init /proc/sys/kernel/timer/\n");
	return 0;
}
postcore_initcall(suspend_mem_timer_init_64);



/* ------------------------------------------------------------------------------------ */
static int suspend_mem_timer_info_proc_show(struct seq_file *proc_show, void *v)
{

	if (suspend_mem_active == true) {
		seq_printf(proc_show,
			"SUPEND MEM TIMER64: ACTIVE. TIME BASE 5 SEC.\n");

		seq_printf(proc_show,
			"SUPEND MEM TIMER64: %lld:%lld:%lld:%lld YEAR:DAY:HOUR:MIN\n",
				 (suspend_to_mem_time_sec / 365 / 24 / 3600),	/* years */
				((suspend_to_mem_time_sec  / 3600 / 24) % 365),	/* days */
				((suspend_to_mem_time_sec  / 3600) % 24),	/* hours */
				((suspend_to_mem_time_sec  / 60) % 60));	/* mIN */

		seq_printf(proc_show,
			"SUPEND MEM TIMER64: %lld SEC.\n", suspend_to_mem_time_sec);

	} else {
		seq_printf(proc_show,
			"SUPEND MEM TIMER64: NOT ACTIVE\n\n");
	}

	return(0);
}


static int __init init_suspend_timer_mem_info_proc_show(void)
{
	proc_create_single("stat.suspend.mem.timer.64", 0, NULL, suspend_mem_timer_info_proc_show);
	return(0);
}
fs_initcall(init_suspend_timer_mem_info_proc_show);

