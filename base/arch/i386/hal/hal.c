/**
 *   @ingroup hal
 *   @file
 *
 *   ARTI -- RTAI-compatible Adeos-based Real-Time Interface. Based on
 *   the original RTAI layer for x86.
 *
 *   Original RTAI/x86 layer implementation: \n
 *   Copyright &copy; 2000 Paolo Mantegazza, \n
 *   Copyright &copy; 2000 Steve Papacharalambous, \n
 *   Copyright &copy; 2000 Stuart Hughes, \n
 *   and others.
 *
 *   RTAI/x86 rewrite over Adeos: \n
 *   Copyright &copy 2002 Philippe Gerum.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
 *   USA; either version 2 of the License, or (at your option) any later
 *   version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/**
 * @defgroup hal RTAI services functions.
 *
 * This module defines some functions that can be used by RTAI tasks, for
 * managing interrupts and communication services with Linux processes.
 *
 *@{*/


#include <linux/module.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");

#include <asm/rtai_hal.h>

#if defined(CONFIG_SMP) && defined(CONFIG_RTAI_DIAG_TSC_SYNC)

/*
	Hacked from arch/ia64/kernel/smpboot.c.
*/

volatile long rtai_tsc_ofst[RTAI_NR_CPUS];

static inline long long readtsc(void)
{
	long long t;
	__asm__ __volatile__("rdtsc" : "=A" (t));
	return t;
}

#define MASTER	(0)
#define SLAVE	(SMP_CACHE_BYTES/8)

#define NUM_ITERS  10

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
static spinlock_t tsc_sync_lock = SPIN_LOCK_UNLOCKED;
static spinlock_t tsclock = SPIN_LOCK_UNLOCKED;
#else
static DEFINE_SPINLOCK(tsc_sync_lock);
static DEFINE_SPINLOCK(tsclock);
#endif

static volatile long long go[SLAVE + 1];

static void sync_master(void *arg)
{
	unsigned long flags, lflags, i;

	if ((unsigned long)arg != hard_smp_processor_id()) {
		return;
	}

	go[MASTER] = 0;
	local_irq_save(flags);
	for (i = 0; i < NUM_ITERS; ++i) {
		while (!go[MASTER]) {
			cpu_relax();
		}
		go[MASTER] = 0;
		spin_lock_irqsave(&tsclock, lflags);
		go[SLAVE] = readtsc();
		spin_unlock_irqrestore(&tsclock, lflags);
	}
	local_irq_restore(flags);
}

static int first_sync_loop_done;
static unsigned long worst_tsc_round_trip[RTAI_NR_CPUS];

static inline long long get_delta(long long *rt, long long *master, unsigned int slave)
{
	unsigned long long best_t0 = 0, best_t1 = ~0ULL, best_tm = 0;
	unsigned long long tcenter = 0, t0, t1, tm, dt;
	long i, lflags, done;

	for (done = i = 0; i < NUM_ITERS; ++i) {
		t0 = readtsc();
		go[MASTER] = 1;
		spin_lock_irqsave(&tsclock, lflags);
		while (!(tm = go[SLAVE])) {
			spin_unlock_irqrestore(&tsclock, lflags);
			cpu_relax();
			spin_lock_irqsave(&tsclock, lflags);
		}
		spin_unlock_irqrestore(&tsclock, lflags);
		go[SLAVE] = 0;
		t1 = readtsc();
		dt = t1 - t0;
		if (!first_sync_loop_done && dt > worst_tsc_round_trip[slave]) {
			worst_tsc_round_trip[slave] = dt;
		}
		if (dt < (best_t1 - best_t0) && (dt <= worst_tsc_round_trip[slave] || !first_sync_loop_done)) {
			done = 1;
			best_t0 = t0, best_t1 = t1, best_tm = tm;
		}
	}

	if (done) {
		*rt = best_t1 - best_t0;
		*master = best_tm - best_t0;
		tcenter = best_t0/2 + best_t1/2;
		if (best_t0 % 2 + best_t1 % 2 == 2) {
			++tcenter;
		}
	}
	if (!first_sync_loop_done) {
		worst_tsc_round_trip[slave] = (worst_tsc_round_trip[slave]*120)/100;
		first_sync_loop_done = 1;
		return done ? rtai_tsc_ofst[slave] = tcenter - best_tm : 0;
	}
	return done ? rtai_tsc_ofst[slave] = (8*rtai_tsc_ofst[slave] + 2*((long)(tcenter - best_tm)))/10 : 0;
}

static void sync_tsc(unsigned int master, unsigned int slave)
{
	unsigned long flags;
	long long delta, rt, master_time_stamp;

	go[MASTER] = 1;
	if (smp_call_function(sync_master, (void *)master, 1, 0) < 0) {
//		printk(KERN_ERR "sync_tsc: failed to get attention of CPU %u!\n", master);
		return;
	}
	while (go[MASTER]) {
		cpu_relax();	/* wait for master to be ready */
	}
	spin_lock_irqsave(&tsc_sync_lock, flags);
	delta = get_delta(&rt, &master_time_stamp, slave);
	spin_unlock_irqrestore(&tsc_sync_lock, flags);

//	printk(KERN_INFO "CPU %u: synced its TSC with CPU %u (master time stamp %llu cycles, < - OFFSET %lld cycles - > , max double tsc read span %llu cycles)\n", slave, master, master_time_stamp, delta, rt);
}

//#define CONFIG_RTAI_MASTER_TSC_CPU  0
#define SLEEP0  500 // ms
#define DSLEEP  500 // ms
static volatile int end;

static inline int irandu(void)
{
	static int i = 783637;
	i = 125*i;
	return i = i - (i/2796203)*2796203;
}

static void kthread_fun(void *null)
{
	int i;
	while (!end) {
		for (i = 0; i < num_online_cpus(); i++) {
			if (i != CONFIG_RTAI_MASTER_TSC_CPU) {
				set_cpus_allowed(current, cpumask_of_cpu(i));
				sync_tsc(CONFIG_RTAI_MASTER_TSC_CPU, i);
			}
		}
		msleep(SLEEP0 + irandu()%DSLEEP);
	}
	end = 0;
}

void init_tsc_sync(void)
{
	kernel_thread((void *)kthread_fun, NULL, 0);
	while(!first_sync_loop_done) {
		msleep(100);
	}
}

void cleanup_tsc_sync(void)
{
	end = 1;
	while(end) {
		msleep(100);
	}
}

EXPORT_SYMBOL(rtai_tsc_ofst);

#endif /* defined(CONFIG_SMP) && defined(CONFIG_RTAI_DIAG_TSC_SYNC) */

#undef INCLUDED_BY_HAL_C
#define INCLUDED_BY_HAL_C
#ifdef RTAI_DUOSS
#include "hal.immed"
#else
#include "hal.piped"
#endif
#include "rtc.c"
