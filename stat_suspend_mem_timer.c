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

/*Important: INTEL: 32Bit Linux Long=32Bit. INTEL: 64Bit Linux Long=64Bit */



#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>



/* proto. */
struct suspend_mem_timer_info_struct {
	s64 suspend_to_mem_time_sec;
	bool suspend_mem_active;
};
static struct suspend_mem_timer_info_struct info;


extern void info_suspend_mem_timer(struct suspend_mem_timer_info_struct *info);



static int suspend_mem_timer_info_proc_show(struct seq_file *proc_show, void *v)
{

	info_suspend_mem_timer(&info);

	if (info.suspend_mem_active == true) {
		seq_printf(proc_show,
			"SUSPEND TO MEM: ACTIVE. TIME BASE 5 SEC.\n");

		seq_printf(proc_show,
			"SUSPEND TO MEM: %lld:%lld:%lld:%lld YEAR:DAY:HOUR:MIN\n", (info.suspend_to_mem_time_sec / 365 / 24 / 3600),	/* years */
										((info.suspend_to_mem_time_sec  / 3600 / 24) % 365),	/* days */
										((info.suspend_to_mem_time_sec  / 3600) % 24),		/* hours */
										((info.suspend_to_mem_time_sec  / 60) % 60));		/* mIN */

		seq_printf(proc_show,
			"SUSPEND TO MEM: %lld SEC.\n", info.suspend_to_mem_time_sec);

	} else {
		seq_printf(proc_show,
			"SUSPEND TO MEM: NOT ACTIVE\n\n");
	}

	return(0);
}


static int __init init_suspend_timer_mem_info_proc_show(void)
{
	proc_create_single("stat.suspend.mem.timer", 0, NULL, suspend_mem_timer_info_proc_show);
	return(0);
}
fs_initcall(init_suspend_timer_mem_info_proc_show);

