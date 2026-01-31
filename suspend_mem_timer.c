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

	/* monoton vorwaertslaufend. Auch bei vorherigem sleep wird die Zeit angepasst */
	/* erkennt vorheriges schlafen */
	s64 start_time_sec = ktime_get_boottime_seconds();

	/* wait */
	for (;;) {

		/* wait 5 sec */
		ssleep(5);

		mutex_lock(&control);

		suspend_to_mem_time_sec -= ktime_get_boottime_seconds() - start_time_sec;

		if (suspend_to_mem_time_sec <= 0) {
			mutex_unlock(&control);
			break;
		}

		start_time_sec = ktime_get_boottime_seconds();

		mutex_unlock(&control);
	}


	for (;;) {
		error = pm_suspend(PM_SUSPEND_MEM);
		if (!error) break;

		error = pm_suspend(PM_SUSPEND_TO_IDLE);
		if (!error) break;

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

	/* overflow or begin epochtime + 30? */
	if ((s64) (ktime_get_boottime_seconds() + suspend_to_mem_time_sec_temp) < 30) {
					printk("SUSPEND TO MEM: TIME NOT allowed\n");
					mutex_unlock(&control);
					return -1;
				}


	if (suspend_mem_active == false) {
		kt_suspend_to_mem_timer = kthread_create (suspend_to_mem_timer, NULL, "suspend_to_mem");
		if (IS_ERR(kt_suspend_to_mem_timer)) {
			printk("SUSPEND TO MEM:TIMER INIT ERROR\n");
			mutex_unlock(&control);
			return -1;
		}

		printk("SUSPEND TO MEM:TIMER INIT OK\n");
		// ACTIVE 
		suspend_to_mem_time_sec = suspend_to_mem_time_sec_temp;
		suspend_mem_active = true;
		mutex_unlock(&control);
		wake_up_process(kt_suspend_to_mem_timer);
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








/*
Wir haben einen Linux-Kernel-Syscall-Code für einen Shutdown-Timer und Suspend-to-MEM-Timer. Lassen Sie uns den Code analysieren.

Ziele des Codes:

Einen ununterbrechbaren Shutdown-Timer im Kernel erstellen.

Einen Suspend-to-MEM-Timer im Kernel erstellen.

Wichtige Punkte:

Der Code ist für den Kernel und verwendet Kernel-APIs.

Es gibt zwei Kernel-Threads: einen für den Shutdown-Timer und einen für den Suspend-to-MEM-Timer.

Der Shutdown-Timer kann nur von root (User ID 0) gesetzt werden (allerdings auskommentiert).

Der Timer ist "uninterruptible" - der Entwickler behauptet, dass er nicht durch kill oder gdb gestoppt werden kann.

Analyse:

Sicherheit:

Die Überprüfung der Benutzer-ID ist auskommentiert. Das bedeutet, dass jeder Benutzer den Syscall aufrufen kann. Das ist ein Sicherheitsproblem, da der Timer nur von root gesetzt werden sollte.

Der Code verwendet Mutexe, um Race Conditions zu vermeiden.

Potenzielle Probleme:

Der Shutdown-Timer-Thread führt eine Endlosschleife aus, die den Kernel hart herunterfährt (kernel_power_off, kernel_halt, kernel_restart). Wenn der Timer abläuft, wird der Rechner heruntergefahren und kann nicht mehr aufgehalten werden.

Der Suspend-to-MEM-Timer versucht, den Rechner in den Suspend-to-MEM-Zustand zu versetzen. Wenn das fehlschlägt, versucht er andere Suspend-Zustände.

Zeitberechnung:

Die Zeit wird in Sekunden angegeben.

Der Code berücksichtigt 32-Bit- und 64-Bit-Systeme unterschiedlich bei der Berechnung von Jahren, Tagen, Stunden und Minuten.

Fehlerbehandlung:

Bei Fehlern gibt es printk-Ausgaben (wenn PRINTK definiert ist).

Der Shutdown-Timer überprüft, ob die angegebene Zeit gültig ist (nicht negativ, mindestens 60 Sekunden, und nicht zu weit in der Zukunft).

Kernel-Threads:

Die Threads werden mit kthread_create erstellt und mit wake_up_process gestartet.

Sie laufen im Hintergrund und überwachen die Zeit.

Syscall-Definition:

Der Syscall hat zwei Parameter: function_number und time_sec.

function_number:
0: Shutdown-Timer initialisieren
1: Shutdown-Timer auf eine neue, frühere Zeit setzen
2: Suspend-to-MEM-Timer setzen
3: Shutdown blockieren (wenn der Timer aktiv ist, wird er blockiert)

Blockieren des Shutdown-Timers:

Es gibt eine Möglichkeit, den Shutdown-Timer zu blockieren (shutdown_block_flag). Wenn dies gesetzt ist, können die Funktionen 0, 1 und 3 nicht ausgeführt werden.

Suspend-to-MEM:

Der Suspend-to-MEM-Timer kann nur alle 30 Sekunden gestellt werden (rundet auf 30-Sekunden-Schritte).

Wenn der Timer abläuft, wird versucht, den Rechner in den Suspend-to-MEM-Zustand zu versetzen. Falls das fehlschlägt, werden andere Suspend-Zustände versucht.

Zeitaktualisierung:

In den Threads wird die vergangene Zeit gemessen und der Timer entsprechend dekrementiert.

Wenn die Systemzeit sich unerwartet ändert (z.B. durch Suspend), wird die Differenz berücksichtigt.

Code-Qualität:

Der Code enthält einige Makros für 32-Bit/64-Bit-Unterschiede.

Die printk-Ausgaben sind mit #ifdef PRINTK geschützt.

Verbesserungsvorschläge:

Sicherheit: Die Benutzerprüfung sollte aktiviert sein, um zu gewährleisten, dass nur root den Timer setzen kann.

Code-Struktur: Der Code ist etwas unübersichtlich. Man könnte die Funktionen in mehrere Dateien aufteilen oder zumindest besser strukturieren.

Magic Numbers: Es gibt einige Magic Numbers (z.B. 30, 60, 5). Diese sollten durch #define ersetzt werden, um die Wartbarkeit zu verbessern.

Fehlerbehandlung: Bei der Erstellung der Kernel-Threads gibt es keine Überprüfung auf Fehler (z.B. ob kthread_create erfolgreich war). Das sollte hinzugefügt werden.

Shutdown-Thread: Der Shutdown-Thread hat eine Endlosschleife, die den Rechner herunterfährt. Wenn kernel_power_off fehlschlägt, wird kernel_halt aufgerufen, 
dann kernel_restart. Das ist vielleicht nicht das gewünschte Verhalten. Möglicherweise sollte man nur eine der Funktionen aufrufen.

Suspend-to-MEM-Thread: Der Suspend-to-MEM-Thread versucht mehrere Suspend-Zustände, was gut ist. Allerdings könnte man hier auch eine bessere Fehlerbehandlung einbauen.

Zeitberechnung: Die Zeitberechnungsfunktionen (shutdown_time_years, etc.) sind für 32-Bit und 64-Bit unterschiedlich. Das ist notwendig, aber der Code könnte lesbarer sein.

Mutex-Handling: Es werden zwei Mutexe verwendet (shutdown_lock und suspend_to_mem_lock). Sie werden in einer festen Reihenfolge gelockt (zuerst suspend_to_mem_lock, dann shutdown_lock), was Deadlocks vermeidet.

Blockieren des Timers: Die Blockier-Funktion (Funktion 3) blockiert den Timer, wenn er aktiv ist. Das ist eine nützliche Sicherheitsfunktion.

Kernel-Thread-Namen: Der Shutdown-Thread heißt "kworker/pari". Das ist nicht standardmäßig. Vielleicht sollte man einen aussagekräftigeren Namen wählen.

Zusammenfassung:

Der Code ist ein Kernel-Modul, das einen ununterbrechbaren Shutdown-Timer und einen Suspend-to-MEM-Timer bereitstellt. Es gibt einige Sicherheits- und Robustheitsprobleme, 
die behoben werden sollten. Der Code ist jedoch im Großen und Ganzen funktional und verwendet Kernel-APIs korrekt.

Wichtig: Dieser Code ist für den Kernel und sollte mit äußerster Vorsicht verwendet werden, da er das System herunterfahren oder in den Suspend-Zustand versetzen kann.

Ich analysiere Ihren Linux-Kernel-Syscall-Code für einen Shutdown/Suspend-Timer. Hier ist meine detaillierte Analyse:

*/