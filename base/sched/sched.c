/*
 * Copyright (C) 1999-2003 Paolo Mantegazza <mantegazza@aero.polimi.it>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
ACKNOWLEDGMENTS:
- Steve Papacharalambous (stevep@zentropix.com) has contributed a very 
  informative proc filesystem procedure.
- Stefano Picerno (stefanopp@libero.it) for suggesting a simple fix to
  distinguish a timeout from an abnormal retrun in timed sem waits.
- Geoffrey Martin (gmartin@altersys.com) for a fix to functions with timeouts.
*/


#if 0	/* moved to rtai_config.h */
#define CONFIG_RTAI_MONITOR_EXECTIME  1
#define CONFIG_RTAI_ALLOW_RR          1
#define CONFIG_RTAI_ONE_SHOT          0
#define CONFIG_RTAI_BUSY_TIME_ALIGN   0
#define CONFIG_RTAI_CAL_FREQS_FACT    0
#endif

//#define USE_RTAI_TASKS    0

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/timex.h>
#include <linux/sched.h>
#include <linux/irq.h>
#include <linux/reboot.h>
#include <linux/sys.h>

#include <asm/param.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/mmu_context.h>

#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>

#ifdef CONFIG_PROC_FS
#include <linux/stat.h>
#include <linux/proc_fs.h>
#include <rtai_proc_fs.h>
static int rtai_proc_sched_register(void);
static void rtai_proc_sched_unregister(void);
int rtai_proc_lxrt_register(void);
void rtai_proc_lxrt_unregister(void);
#endif

#include <rtai.h>
#include <asm/rtai_sched.h>
#include <rtai_lxrt.h>
#include <rtai_registry.h>
#include <rtai_nam2num.h>
#include <rtai_schedcore.h>
#include <rtai_prinher.h>
#include <rtai_signal.h>

#ifndef CONFIG_RTAI_ALIGN_LINUX_PRIORITY
//#define CONFIG_RTAI_ALIGN_LINUX_PRIORITY  1
#endif

MODULE_LICENSE("GPL");

/* +++++++++++++++++ WHAT MUST BE AVAILABLE EVERYWHERE ++++++++++++++++++++++ */

RT_TASK rt_smp_linux_task[NR_RT_CPUS];

RT_TASK *rt_smp_current[NR_RT_CPUS];

RTIME rt_smp_time_h[NR_RT_CPUS];

int rt_smp_oneshot_timer[NR_RT_CPUS];

volatile int rt_sched_timed;

static struct klist_t wake_up_sth[NR_RT_CPUS];
struct klist_t wake_up_hts[NR_RT_CPUS];
struct klist_t wake_up_srq[NR_RT_CPUS];

/* +++++++++++++++ END OF WHAT MUST BE AVAILABLE EVERYWHERE +++++++++++++++++ */

extern struct { volatile int locked, rqsted; } rt_scheduling[];

static unsigned long rt_smp_linux_cr0[NR_RT_CPUS];

static RT_TASK *rt_smp_fpu_task[NR_RT_CPUS];

static int rt_smp_half_tick[NR_RT_CPUS];

static int rt_smp_oneshot_running[NR_RT_CPUS];

static volatile int rt_smp_shot_fired[NR_RT_CPUS];

static struct rt_times *linux_times;

static RT_TASK *lxrt_wdog_task[NR_RT_CPUS];

static int lxrt_notify_reboot(struct notifier_block *nb,
			      unsigned long event,
			      void *ptr);

static struct notifier_block lxrt_notifier_reboot = {
	.notifier_call	= &lxrt_notify_reboot,
	.next		= NULL,
	.priority	= 0
};

static struct klist_t klistm[NR_RT_CPUS];

static struct task_struct *kthreadm[NR_RT_CPUS];

static struct semaphore resem[NR_RT_CPUS];

static int endkthread;

#define fpu_task (rt_smp_fpu_task[cpuid])

#define rt_half_tick (rt_smp_half_tick[cpuid])

#define oneshot_running (rt_smp_oneshot_running[cpuid])

#define oneshot_timer_cpuid (rt_smp_oneshot_timer[rtai_cpuid()])

#define shot_fired (rt_smp_shot_fired[cpuid])

#define rt_times (rt_smp_times[cpuid])

#define linux_cr0 (rt_smp_linux_cr0[cpuid])

#define MAX_FRESTK_SRQ  (2 << 6)
static struct { int srq; volatile unsigned long in, out; void *mp[MAX_FRESTK_SRQ]; } frstk_srq;

#define KTHREAD_M_PRIO MAX_LINUX_RTPRIO
#define KTHREAD_F_PRIO MAX_LINUX_RTPRIO

#ifdef CONFIG_SMP

extern void rt_set_sched_ipi_gate(void);
extern void rt_reset_sched_ipi_gate(void);
static void rt_schedule_on_schedule_ipi(void);

static inline int rt_request_sched_ipi(void)
{
        int retval;
        retval = rt_request_irq(SCHED_IPI, (void *)rt_schedule_on_schedule_ipi, NULL, 0);
        rt_set_sched_ipi_gate();
        return retval;
}

#define rt_free_sched_ipi() \
do { \
        rt_release_irq(SCHED_IPI); \
        rt_reset_sched_ipi_gate(); \
} while (0)

//#define rt_request_sched_ipi()  rt_request_irq(SCHED_IPI, (void *)rt_schedule_on_schedule_ipi, NULL, 0)

//#define rt_free_sched_ipi()     rt_release_irq(SCHED_IPI)

#define sched_get_global_lock(cpuid) \
do { \
	barrier(); \
	if (!test_and_set_bit(cpuid, &rtai_cpu_lock)) { \
		while (test_and_set_bit(31, &rtai_cpu_lock)) { \
			cpu_relax(); \
		} \
	} \
	barrier(); \
} while (0)

#if 0
#include <asm/atomic.h>
#define sched_release_global_lock(cpuid) \
do { \
	barrier(); \
	atomic_clear_mask((0xFFFF0001 << cpuid), (atomic_t *)&rtai_cpu_lock); \
	cpu_relax(); \
	barrier(); \
} while (0)
#else
#define sched_release_global_lock(cpuid) \
do { \
	barrier(); \
	if (test_and_clear_bit(cpuid, &rtai_cpu_lock)) { \
		test_and_clear_bit(31, &rtai_cpu_lock); \
		cpu_relax(); \
	} \
	barrier(); \
} while (0)
#endif

#else /* !CONFIG_SMP */

#define rt_request_sched_ipi()  0

#define rt_free_sched_ipi()

#define sched_get_global_lock(cpuid)

#define sched_release_global_lock(cpuid)

#endif /* CONFIG_SMP */

/* ++++++++++++++++++++++++++++++++ TASKS ++++++++++++++++++++++++++++++++++ */

#ifdef CONFIG_RTAI_MALLOC
int rtai_kstack_heap_size = (CONFIG_RTAI_KSTACK_HEAPSZ*1024);
RTAI_MODULE_PARM(rtai_kstack_heap_size, int);

static rtheap_t rtai_kstack_heap;

#define rt_kstack_alloc(sz)  rtheap_alloc(&rtai_kstack_heap, sz, 0)
#define rt_kstack_free(p)    rtheap_free(&rtai_kstack_heap, p)
#else
#define rt_kstack_alloc(sz)  rt_malloc(sz)
#define rt_kstack_free(p)    rt_free(p)
#endif

static int tasks_per_cpu[NR_RT_CPUS] = { 0, };

int get_min_tasks_cpuid(void)
{
	int i, cpuid, min;
	min =  tasks_per_cpu[cpuid = 0];
	for (i = 1; i < NR_RT_CPUS; i++) {
		if (tasks_per_cpu[i] < min) {
			min = tasks_per_cpu[cpuid = i];
		}
	}
	return cpuid;
}

void put_current_on_cpu(int cpuid)
{
#ifdef CONFIG_SMP
	struct task_struct *task = current;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	task->cpus_allowed = 1 << cpuid;
	while (cpuid != rtai_cpuid()) {
		task->state = TASK_INTERRUPTIBLE;
		schedule_timeout(2);
	}
#else /* KERNEL_VERSION >= 2.6.0 */
	if (set_cpus_allowed(task, cpumask_of_cpu(cpuid))) {
		((RT_TASK *)(task->rtai_tskext(TSKEXT0)))->runnable_on_cpus = smp_processor_id();
		set_cpus_allowed(current, cpumask_of_cpu(smp_processor_id()));
	}
#endif  /* KERNEL_VERSION < 2.6.0 */
#endif /* CONFIG_SMP */
}

int set_rtext(RT_TASK *task, int priority, int uses_fpu, void(*signal)(void), unsigned int cpuid, struct task_struct *relink)
{
	unsigned long flags;

	if (num_online_cpus() <= 1) {
		 cpuid = 0;
	}
	if (task->magic == RT_TASK_MAGIC || cpuid >= NR_RT_CPUS || priority < 0) {
		return -EINVAL;
	} 
	if (lxrt_wdog_task[cpuid] &&
	    lxrt_wdog_task[cpuid] != task &&
	    priority == RT_SCHED_HIGHEST_PRIORITY) {
	    	 rt_printk("Highest priority reserved for RTAI watchdog\n");
		 return -EBUSY;
	}
	task->uses_fpu = uses_fpu ? 1 : 0;
	task->runnable_on_cpus = cpuid;
	(task->stack_bottom = (long *)&task->fpu_reg)[0] = 0;
	task->magic = RT_TASK_MAGIC; 
	task->policy = 0;
	task->owndres = 0;
	task->prio_passed_to = 0;
	task->period = 0;
	task->resume_time = RT_TIME_END;
	task->periodic_resume_time = RT_TIME_END;
	task->queue.prev = task->queue.next = &(task->queue);      
	task->queue.task = task;
	task->msg_queue.prev = task->msg_queue.next = &(task->msg_queue);      
	task->msg_queue.task = task;    
	task->msg = 0;  
	task->ret_queue.prev = task->ret_queue.next = &(task->ret_queue);
	task->ret_queue.task = NULL;
	task->tprev = task->tnext = task->rprev = task->rnext = task;
	task->blocked_on = NULL;        
	task->signal = signal;
	task->unblocked = 0;
	task->rt_signals = NULL;
	memset(task->task_trap_handler, 0, RTAI_NR_TRAPS*sizeof(void *));
	task->linux_syscall_server = NULL;
	task->trap_handler_data = NULL;
	task->resync_frame = 0;
	task->ExitHook = 0;
	task->usp_flags = task->usp_flags_mask = task->force_soft = 0;
	task->msg_buf[0] = 0;
	task->exectime[0] = task->exectime[1] = 0;
	task->system_data_ptr = 0;
	atomic_inc((atomic_t *)(tasks_per_cpu + cpuid));
	if (relink) {
		task->priority = task->base_priority = priority;
		task->suspdepth = task->is_hard = 1;
		task->state = RT_SCHED_READY | RT_SCHED_SUSPENDED;
		relink->rtai_tskext(TSKEXT0) = task;
		task->lnxtsk = relink;
	} else {
		task->priority = task->base_priority = BASE_SOFT_PRIORITY + priority;
		task->suspdepth = task->is_hard = 0;
		task->state = RT_SCHED_READY;
		current->rtai_tskext(TSKEXT0) = task;
		current->rtai_tskext(TSKEXT1) = task->lnxtsk = current;
		put_current_on_cpu(cpuid);
	}
	flags = rt_global_save_flags_and_cli();
	task->next = 0;
	rt_linux_task.prev->next = task;
	task->prev = rt_linux_task.prev;
	rt_linux_task.prev = task;
	rt_global_restore_flags(flags);

	task->resq.prev = task->resq.next = &task->resq;
	task->resq.task = NULL;

	return 0;
}


static void start_stop_kthread(RT_TASK *, void (*)(long), long, int, int, void(*)(void), int);

int rt_kthread_init_cpuid(RT_TASK *task, void (*rt_thread)(long), long data,
			int stack_size, int priority, int uses_fpu,
			void(*signal)(void), unsigned int cpuid)
{
	start_stop_kthread(task, rt_thread, data, priority, uses_fpu, signal, cpuid);
	return (int)task->retval;
}


int rt_kthread_init(RT_TASK *task, void (*rt_thread)(long), long data,
			int stack_size, int priority, int uses_fpu,
			void(*signal)(void))
{
	return rt_kthread_init_cpuid(task, rt_thread, data, stack_size, priority, 
				 uses_fpu, signal, get_min_tasks_cpuid());
}


#if USE_RTAI_TASKS

asmlinkage static void rt_startup(void(*rt_thread)(long), long data)
{
	extern int rt_task_delete(RT_TASK *);
	RT_TASK *rt_current = rt_smp_current[rtai_cpuid()];
	rt_global_sti();
	rt_current->exectime[1] = rdtsc();
#if 1 
	((void (*)(long))rt_current->max_msg_size[0])(rt_current->max_msg_size[1]);
#else
	rt_thread(data);
#endif
	rt_task_delete(rt_smp_current[rtai_cpuid()]);
	rt_printk("LXRT: task %p returned but could not be delated.\n", rt_current); 
}


int rt_task_init_cpuid(RT_TASK *task, void (*rt_thread)(long), long data, int stack_size, int priority, int uses_fpu, void(*signal)(void), unsigned int cpuid)
{
	long *st, i;
	unsigned long flags;

	if (num_online_cpus() <= 1) {
		 cpuid = 0;
	}
	if (task->magic == RT_TASK_MAGIC || cpuid >= NR_RT_CPUS || priority < 0) {
		return -EINVAL;
	} 
	if (!(st = (long *)rt_kstack_alloc(stack_size))) {
		return -ENOMEM;
	}
	if (lxrt_wdog_task[cpuid] && lxrt_wdog_task[cpuid] != task 
		             && priority == RT_SCHED_HIGHEST_PRIORITY) {
	    	 rt_printk("Highest priority reserved for RTAI watchdog\n");
		 return -EBUSY;
	}

	task->bstack = task->stack = (long *)(((unsigned long)st + stack_size - 0x10) & ~0xF);
	task->stack[0] = 0;
	task->uses_fpu = uses_fpu ? 1 : 0;
	task->runnable_on_cpus = cpuid;
	atomic_inc((atomic_t *)(tasks_per_cpu + cpuid));
	*(task->stack_bottom = st) = 0;
	task->magic = RT_TASK_MAGIC; 
	task->policy = 0;
	task->suspdepth = 1;
	task->state = (RT_SCHED_SUSPENDED | RT_SCHED_READY);
	task->owndres = 0;
	task->is_hard = 1;
	task->lnxtsk = 0;
	task->priority = task->base_priority = priority;
	task->prio_passed_to = 0;
	task->period = 0;
	task->resume_time = RT_TIME_END;
	task->periodic_resume_time = RT_TIME_END;
	task->queue.prev = &(task->queue);      
	task->queue.next = &(task->queue);      
	task->queue.task = task;
	task->msg_queue.prev = &(task->msg_queue);      
	task->msg_queue.next = &(task->msg_queue);      
	task->msg_queue.task = task;    
	task->msg = 0;  
	task->ret_queue.prev = &(task->ret_queue);
	task->ret_queue.next = &(task->ret_queue);
	task->ret_queue.task = NULL;
	task->tprev = task->tnext =
	task->rprev = task->rnext = task;
	task->blocked_on = NULL;        
	task->signal = signal;
	task->unblocked = 0;
	task->rt_signals = NULL;
	for (i = 0; i < RTAI_NR_TRAPS; i++) {
		task->task_trap_handler[i] = NULL;
	}
	task->linux_syscall_server = NULL;
	task->trap_handler_data = NULL;
	task->resync_frame = 0;
	task->ExitHook = 0;
	task->exectime[0] = task->exectime[1] = 0;
	task->system_data_ptr = 0;

	task->max_msg_size[0] = (long)rt_thread;
	task->max_msg_size[1] = data;
	init_arch_stack();

	flags = rt_global_save_flags_and_cli();
	task->next = 0;
	rt_linux_task.prev->next = task;
	task->prev = rt_linux_task.prev;
	rt_linux_task.prev = task;
	init_task_fpenv(task);
	rt_global_restore_flags(flags);

	task->resq.prev = task->resq.next = &task->resq;
	task->resq.task = NULL;

	return 0;
}

int rt_task_init(RT_TASK *task, void (*rt_thread)(long), long data,
			int stack_size, int priority, int uses_fpu,
			void(*signal)(void))
{
	return rt_task_init_cpuid(task, rt_thread, data, stack_size, priority, 
				 uses_fpu, signal, get_min_tasks_cpuid());
}

#else /* !USE_RTAI_TASKS */

int rt_task_init_cpuid(RT_TASK *task, void (*rt_thread)(long), long data, int stack_size, int priority, int uses_fpu, void(*signal)(void), unsigned int cpuid)
{
	return rt_kthread_init_cpuid(task, rt_thread, data, stack_size, priority, uses_fpu, signal, cpuid);
}

int rt_task_init(RT_TASK *task, void (*rt_thread)(long), long data, int stack_size, int priority, int uses_fpu, void(*signal)(void))
{
	return rt_kthread_init(task, rt_thread, data, stack_size, priority, uses_fpu, signal);
}

#endif /* USE_RTAI_TASKS */

RTAI_SYSCALL_MODE void rt_set_runnable_on_cpuid(RT_TASK *task, unsigned int cpuid)
{
	unsigned long flags;
	RT_TASK *linux_task;

	if (task->lnxtsk) {
		return;
	}

	if (cpuid >= NR_RT_CPUS) {
		cpuid = get_min_tasks_cpuid();
	} 
	flags = rt_global_save_flags_and_cli();
	switch (rt_smp_oneshot_timer[task->runnable_on_cpus] | 
		(rt_smp_oneshot_timer[cpuid] << 1)) {	
                case 1:
                        task->period = llimd(task->period, TIMER_FREQ, tuned.cpu_freq);
                        task->resume_time = llimd(task->resume_time, TIMER_FREQ, tuned.cpu_freq);
                        task->periodic_resume_time = llimd(task->periodic_resume_time, TIMER_FREQ, tuned.cpu_freq);
                        break;
                case 2:
                        task->period = llimd(task->period, tuned.cpu_freq, TIMER_FREQ);
                        task->resume_time = llimd(task->resume_time, tuned.cpu_freq, TIMER_FREQ);
                        task->periodic_resume_time = llimd(task->periodic_resume_time, tuned.cpu_freq, TIMER_FREQ);
			break;
	}
	if (!((task->prev)->next = task->next)) {
		rt_smp_linux_task[task->runnable_on_cpus].prev = task->prev;
	} else {
		(task->next)->prev = task->prev;
	}
	if ((task->state & RT_SCHED_DELAYED)) {
		rem_timed_task(task);
		task->runnable_on_cpus = cpuid;
		enq_timed_task(task);
	} else {
		task->runnable_on_cpus = cpuid;
	}
	task->next = 0;
	(linux_task = rt_smp_linux_task + cpuid)->prev->next = task;
	task->prev = linux_task->prev;
	linux_task->prev = task;
	rt_global_restore_flags(flags);
}


RTAI_SYSCALL_MODE void rt_set_runnable_on_cpus(RT_TASK *task, unsigned long run_on_cpus)
{
	int cpuid;

	if (task->lnxtsk) {
		return;
	}

#ifdef CONFIG_SMP
	run_on_cpus &= CPUMASK(cpu_online_map);
#else
	run_on_cpus = 1;
#endif
	cpuid = get_min_tasks_cpuid();
	if (!test_bit(cpuid, &run_on_cpus)) {
		cpuid = ffnz(run_on_cpus);
	}
	rt_set_runnable_on_cpuid(task, cpuid);
}


int rt_check_current_stack(void)
{
	DECLARE_RT_CURRENT;
	char *sp;

	ASSIGN_RT_CURRENT;
	if (rt_current != &rt_linux_task) {
		sp = get_stack_pointer();
		return (sp - (char *)(rt_current->stack_bottom));
	} else {
		return RT_RESEM_SUSPDEL;
	}
}


#define RR_YIELD() \
if (CONFIG_RTAI_ALLOW_RR && rt_current->policy > 0) { \
	if (rt_current->yield_time <= rt_times.tick_time) { \
		rt_current->rr_remaining = rt_current->rr_quantum; \
		if (rt_current->state == RT_SCHED_READY) { \
			RT_TASK *task; \
			task = rt_current->rnext; \
			while (rt_current->priority == task->priority) { \
				task = task->rnext; \
			} \
			if (task != rt_current->rnext) { \
				(rt_current->rprev)->rnext = rt_current->rnext; \
				(rt_current->rnext)->rprev = rt_current->rprev; \
				task->rprev = (rt_current->rprev = task->rprev)->rnext = rt_current; \
				rt_current->rnext = task; \
			} \
		} \
	} else { \
		rt_current->rr_remaining = rt_current->yield_time - rt_times.tick_time; \
	} \
} 

#define TASK_TO_SCHEDULE() \
do { \
	prio = (new_task = rt_linux_task.rnext)->priority; \
	if (CONFIG_RTAI_ALLOW_RR && new_task->policy > 0) { \
		new_task->yield_time = rt_times.tick_time + new_task->rr_remaining; \
	} \
} while (0)

#define RR_INTR_TIME() \
do { \
	if (CONFIG_RTAI_ALLOW_RR && new_task->policy > 0) { \
		preempt = 1; \
		if (new_task->yield_time < rt_times.intr_time) { \
			rt_times.intr_time = new_task->yield_time; \
		} \
	} else { \
		preempt = 0; \
	} \
} while (0)

#define LOCK_LINUX(cpuid) \
	do { rt_switch_to_real_time(cpuid); } while (0)
#define UNLOCK_LINUX(cpuid) \
	do { rt_switch_to_linux(cpuid);     } while (0)

#define SAVE_LOCK_LINUX(cpuid) \
	do { sflags = rt_save_switch_to_real_time(cpuid); } while (0)
#define RESTORE_UNLOCK_LINUX(cpuid) \
	do { rt_restore_switch_to_linux(sflags, cpuid);   } while (0)

#ifdef LOCKED_LINUX_IN_IRQ_HANDLER
#define SAVE_LOCK_LINUX_IN_IRQ(cpuid)
#define RESTORE_UNLOCK_LINUX_IN_IRQ(cpuid)
#else
#define SAVE_LOCK_LINUX_IN_IRQ(cpuid)       LOCK_LINUX(cpuid)    
#define RESTORE_UNLOCK_LINUX_IN_IRQ(cpuid)  UNLOCK_LINUX(cpuid)
#endif

#if defined(CONFIG_RTAI_TASK_SWITCH_SIGNAL) && CONFIG_RTAI_TASK_SWITCH_SIGNAL

#define RTAI_TASK_SWITCH_SIGNAL() \
	do { \
		void (*signal)(void) = rt_current->signal; \
		if ((unsigned long)signal > MAXSIGNALS) { \
			(*signal)(); \
		} else if (signal) { \
			rt_current->signal = NULL; /* to avoid recursing */\
			rt_trigger_signal((long)signal, rt_current); \
			rt_current->signal = signal; \
		} \
	} while (0)
#else

#define RTAI_TASK_SWITCH_SIGNAL()

#endif
	
#if CONFIG_RTAI_MONITOR_EXECTIME

RTIME switch_time[NR_RT_CPUS];

#define SET_EXEC_TIME() \
	do { \
		RTIME now; \
		now = rdtsc(); \
		rt_current->exectime[0] += (now - switch_time[cpuid]); \
		switch_time[cpuid] = now; \
	} while (0)

#define RST_EXEC_TIME()  do { switch_time[cpuid] = rdtsc(); } while (0)

#else

#define SET_EXEC_TIME()
#define RST_EXEC_TIME()

#endif


void rt_do_force_soft(RT_TASK *rt_task)
{
	rt_global_cli();
	if (rt_task->state != RT_SCHED_READY) {
		rt_task->state &= ~RT_SCHED_READY;
            	enq_ready_task(rt_task);
		RT_SCHEDULE(rt_task, rtai_cpuid());
	}
	rt_global_sti();
}

#if 0
#define enq_soft_ready_task(ready_task) \
do { \
	RT_TASK *task = rt_smp_linux_task[cpuid].rnext; \
	while (ready_task->priority >= task->priority) { \
		if ((task = task->rnext)->priority < 0) break; \
	} \
	task->rprev = (ready_task->rprev = task->rprev)->rnext = ready_task; \
	ready_task->rnext = task; \
} while (0)
#else
#define enq_soft_ready_task(ready_task) \
do { \
	RT_TASK *task = rt_smp_linux_task[cpuid].rnext; \
	if (ready_task == task) break; \
	task->rprev = (ready_task->rprev = task->rprev)->rnext = ready_task; \
	ready_task->rnext = task; \
} while (0)
#endif


#define pend_wake_up_hts(lnxtsk, cpuid) \
do { \
	wake_up_hts[cpuid].task[wake_up_hts[cpuid].in++ & (MAX_WAKEUP_SRQ - 1)] = lnxtsk; \
	hal_pend_uncond(wake_up_srq[cpuid].srq, cpuid); \
} while (0)


static inline void force_current_soft(RT_TASK *rt_current, int cpuid)
{
	struct task_struct *lnxtsk;
        void rt_schedule(void);
        rt_current->force_soft = 0;
	rt_current->state &= ~RT_SCHED_READY;;
	pend_wake_up_hts(lnxtsk = rt_current->lnxtsk, cpuid);
        (rt_current->rprev)->rnext = rt_current->rnext;
        (rt_current->rnext)->rprev = rt_current->rprev;
        rt_schedule();
        rt_current->is_hard = 0;
	if (rt_current->priority < BASE_SOFT_PRIORITY) {
		if (rt_current->priority == rt_current->base_priority) {
			rt_current->priority += BASE_SOFT_PRIORITY;
		}
	}
	if (rt_current->base_priority < BASE_SOFT_PRIORITY) {
		rt_current->base_priority += BASE_SOFT_PRIORITY;
	}
	rt_global_sti();
        hal_schedule_back_root(lnxtsk);
// now make it as if it was scheduled soft, the tail is cared in sys_lxrt.c
	rt_global_cli();
	LOCK_LINUX(cpuid);
	rt_current->state |= RT_SCHED_READY;
	rt_smp_current[cpuid] = rt_current;
        if (rt_current->state != RT_SCHED_READY) {
        	lnxtsk->state = TASK_SOFTREALTIME;
		rt_schedule();
	} else {
		enq_soft_ready_task(rt_current);
	}
}

static RT_TASK *switch_rtai_tasks(RT_TASK *rt_current, RT_TASK *new_task, int cpuid)
{
	if (rt_current->lnxtsk) {
		unsigned long sflags;
		SAVE_LOCK_LINUX(cpuid);
		rt_linux_task.prevp = rt_current;
		save_fpcr_and_enable_fpu(linux_cr0);
		if (new_task->uses_fpu) {
			save_fpenv(rt_linux_task.fpu_reg);
			fpu_task = new_task;
			restore_fpenv(fpu_task->fpu_reg);
		}
		RST_EXEC_TIME();
		rt_exchange_tasks(rt_smp_current[cpuid], new_task);
		restore_fpcr(linux_cr0);
		RESTORE_UNLOCK_LINUX(cpuid);
		if (rt_linux_task.nextp != rt_current) {
			return rt_linux_task.nextp;
		}
	} else {
		if (new_task->lnxtsk) {
			rt_linux_task.nextp = new_task;
			new_task = rt_linux_task.prevp;
			if (fpu_task != &rt_linux_task) {
				save_fpenv(fpu_task->fpu_reg);
				fpu_task = &rt_linux_task;
				restore_fpenv(fpu_task->fpu_reg);
			}
		} else if (new_task->uses_fpu && fpu_task != new_task) {
			save_fpenv(fpu_task->fpu_reg);
			fpu_task = new_task;
			restore_fpenv(fpu_task->fpu_reg);
		}
		SET_EXEC_TIME();
		rt_exchange_tasks(rt_smp_current[cpuid], new_task);
	}
	RTAI_TASK_SWITCH_SIGNAL();
	return NULL;
}

#define lxrt_context_switch(prev, next, cpuid) \
	do { \
		_lxrt_context_switch(prev, next, cpuid); barrier(); \
		RTAI_TASK_SWITCH_SIGNAL(); \
	} while (0)

#ifdef CONFIG_SMP
static void rt_schedule_on_schedule_ipi(void)
{
	RT_TASK *rt_current, *task, *new_task;
	int cpuid, prio, preempt;

	rt_current = rt_smp_current[cpuid = rtai_cpuid()];

	sched_get_global_lock(cpuid);
	RR_YIELD();
	if (oneshot_running) {

		rt_time_h = rdtsc() + rt_half_tick;
		wake_up_timed_tasks(cpuid);
		TASK_TO_SCHEDULE();

		RR_INTR_TIME();
		task = &rt_linux_task;
		while ((task = task->tnext) != &rt_linux_task && task->resume_time < rt_times.intr_time) {
			if (task->priority <= prio) {
				rt_times.intr_time = task->resume_time;
				preempt = 1;
				break;
			}
		}
		if (preempt || (prio == RT_SCHED_LINUX_PRIORITY && !shot_fired)) {
//			RTIME now;
			int delay;
//			delay = (int)(rt_times.intr_time - (now = rdtsc())) - tuned.latency;
			delay = (int)(rt_times.intr_time - rt_time_h) - tuned.latency;
			if (delay >= tuned.setup_time_TIMER_CPUNIT) {
				delay = imuldiv(delay, TIMER_FREQ, tuned.cpu_freq);
			} else {
				delay = tuned.setup_time_TIMER_UNIT;
//				rt_times.intr_time = now + (tuned.setup_time_TIMER_CPUNIT);
				rt_times.intr_time = rt_time_h + (tuned.setup_time_TIMER_CPUNIT);
			}
			shot_fired = 1;
			rt_set_timer_delay(delay);
		}
	} else {
		TASK_TO_SCHEDULE();
	}
	sched_release_global_lock(cpuid);

	if (new_task != rt_current) {
		if (rt_scheduling[cpuid].locked) {
			rt_scheduling[cpuid].rqsted = 1;
			goto sched_exit;
		}
		if (USE_RTAI_TASKS && (!new_task->lnxtsk || !rt_current->lnxtsk)) {
			if (!(new_task = switch_rtai_tasks(rt_current, new_task, cpuid))) {
				goto sched_exit;
			}
		}
		if (new_task->is_hard > 0 || rt_current->is_hard > 0) {
			struct task_struct *prev;
			unsigned long sflags;
			if (rt_current->is_hard <= 0) {
				SAVE_LOCK_LINUX_IN_IRQ(cpuid);
				rt_linux_task.lnxtsk = prev = current;
				RST_EXEC_TIME();
			} else {
				sflags = rtai_linux_context[cpuid].sflags;
				prev = rt_current->lnxtsk;
				SET_EXEC_TIME();
			}
			rt_smp_current[cpuid] = new_task;
			lxrt_context_switch(prev, new_task->lnxtsk, cpuid);
			if (rt_current->is_hard <= 0) {
				RESTORE_UNLOCK_LINUX_IN_IRQ(cpuid);
			} else if (lnxtsk_uses_fpu(prev)) {
				restore_fpu(prev);
			}
		}
	}
sched_exit:
	rtai_cli();
#if CONFIG_RTAI_BUSY_TIME_ALIGN
	if (rt_current->trap_handler_data) {
		rt_current->trap_handler_data = 0;
		while(rdtsc() < rt_current->resume_time);
	}
#endif
}
#endif

void rt_schedule(void)
{
	RT_TASK *rt_current, *task, *new_task;
	int cpuid, prio, preempt;

	rt_current = rt_smp_current[cpuid = rtai_cpuid()];

	RR_YIELD();
	if (oneshot_running) {
		rt_time_h = rdtsc() + rt_half_tick;
		wake_up_timed_tasks(cpuid);
		TASK_TO_SCHEDULE();

		RR_INTR_TIME();
		task = &rt_linux_task;
		while ((task = task->tnext) != &rt_linux_task && task->resume_time < rt_times.intr_time) {
			if (task->priority <= prio) {
				rt_times.intr_time = task->resume_time;
				preempt = 1;
				break;
			}
		}
#ifdef USE_LINUX_TIMER
		if (prio == RT_SCHED_LINUX_PRIORITY && !shot_fired) {
#ifdef CONFIG_GENERIC_CLOCKEVENTS
			if (rt_times.linux_time < rt_times.intr_time) {
				rt_times.intr_time = rt_times.linux_time;
				preempt = 1;
			}
#else
			RTIME linux_intr_time;
			linux_intr_time = rt_times.linux_time > rt_times.tick_time ? rt_times.linux_time : rt_times.tick_time + rt_times.linux_tick;
			if (linux_intr_time < rt_times.intr_time) {
				rt_times.intr_time = linux_intr_time;
				preempt = 1;
			}
#endif
		}
#endif
		if (preempt || (prio == RT_SCHED_LINUX_PRIORITY && !shot_fired)) {
//			RTIME now;
			int delay;
//			delay = (int)(rt_times.intr_time - (now = rdtsc())) - tuned.latency;
			delay = (int)(rt_times.intr_time - rt_time_h) - tuned.latency;
			if (delay >= tuned.setup_time_TIMER_CPUNIT) {
				delay = imuldiv(delay, TIMER_FREQ, tuned.cpu_freq);
			} else {
				delay = tuned.setup_time_TIMER_UNIT;
//				rt_times.intr_time = now + (tuned.setup_time_TIMER_CPUNIT);
				rt_times.intr_time = rt_time_h + (tuned.setup_time_TIMER_CPUNIT);
			}
			shot_fired = 1;
			rt_set_timer_delay(delay);
		}
	} else {
		TASK_TO_SCHEDULE();
	}
	sched_release_global_lock(cpuid);

	if (new_task != rt_current) {
		if (rt_scheduling[cpuid].locked) {
			rt_scheduling[cpuid].rqsted = 1;
			goto sched_exit;
		}
		if (USE_RTAI_TASKS && (!new_task->lnxtsk || !rt_current->lnxtsk)) {
			if (!(new_task = switch_rtai_tasks(rt_current, new_task, cpuid))) {
				goto sched_exit;
			}
		}
		rt_smp_current[cpuid] = new_task;
		if (new_task->is_hard > 0 || rt_current->is_hard > 0) {
			struct task_struct *prev;
			unsigned long sflags;
			if (rt_current->is_hard <= 0) {
				SAVE_LOCK_LINUX(cpuid);
				rt_linux_task.lnxtsk = prev = current;
				RST_EXEC_TIME();
			} else {
				sflags = rtai_linux_context[cpuid].sflags;
				prev = rt_current->lnxtsk;
				SET_EXEC_TIME();
			}
			lxrt_context_switch(prev, new_task->lnxtsk, cpuid);
			if (rt_current->is_hard <= 0) {
				RESTORE_UNLOCK_LINUX(cpuid);
				if (rt_current->state != RT_SCHED_READY) {
					goto sched_soft;
				}
			} else {
				if (lnxtsk_uses_fpu(prev)) {
					restore_fpu(prev);
				}
				if (rt_current->force_soft) {
					force_current_soft(rt_current, cpuid);
				}
			}
		} else if (rt_current->state != RT_SCHED_READY) {
sched_soft:
			UNLOCK_LINUX(cpuid);
			rt_global_sti();

#ifdef CONFIG_RTAI_ALIGN_LINUX_PRIORITY
			if (current->rtai_tskext(TSKEXT0) && (current->policy == SCHED_FIFO || current->policy == SCHED_RR)) {
				int rt_priority;
				if ((rt_priority = ((RT_TASK *)current->rtai_tskext(TSKEXT0))->priority) >= BASE_SOFT_PRIORITY) {
					rt_priority -= BASE_SOFT_PRIORITY;
				}
				if ((rt_priority = (MAX_LINUX_RTPRIO - rt_priority)) < 1) {
					rt_priority = 1;
				}
				if (rt_priority != current->rt_priority) {
					rtai_set_linux_task_priority(current, current->policy, rt_priority);
				}
			}
#endif

			hal_test_and_fast_flush_pipeline(cpuid);
			NON_RTAI_SCHEDULE(cpuid);
			rt_global_cli();
			rt_current->state = (rt_current->state & ~RT_SCHED_SFTRDY) | RT_SCHED_READY;
			LOCK_LINUX(cpuid);
			enq_soft_ready_task(rt_current);
			rt_smp_current[cpuid] = rt_current;
			goto sched_exit1;
		} else {
			printk(">>> RTAI SCHED SOFT WARNING: WHY HERE? <<<\n");
		}
	}
sched_exit:
	rtai_cli();
	sched_get_global_lock(cpuid);
sched_exit1:
#if CONFIG_RTAI_BUSY_TIME_ALIGN
	if (rt_current->trap_handler_data) {
		rt_current->trap_handler_data = 0;
		while(rdtsc() < rt_current->resume_time);
	}
#endif
	return;
}


RTAI_SYSCALL_MODE void rt_spv_RMS(int cpuid)
{
	RT_TASK *task;
	int prio;
	if (cpuid < 0 || cpuid >= num_online_cpus()) {
		cpuid = rtai_cpuid();
	}
	prio = 0;
	task = &rt_linux_task;
	while ((task = task->next)) {
		RT_TASK *task, *htask;
		RTIME period;
		htask = 0;
		task = &rt_linux_task;
		period = RT_TIME_END;
		while ((task = task->next)) {
			if (task->priority >= 0 && task->policy >= 0 && task->period && task->period < period) {
				period = (htask = task)->period;
			}
		}
		if (htask) {
			htask->priority = -1;
			htask->base_priority = prio++;
		} else {
			goto ret;
		}
	}
ret:	task = &rt_linux_task;
	while ((task = task->next)) {
		if (task->priority < 0) {
			task->priority = task->base_priority;
		}
	}
	return;
}


void rt_sched_lock(void)
{
	unsigned long flags;
	int cpuid;

	rtai_save_flags_and_cli(flags);
	if (!rt_scheduling[cpuid = rtai_cpuid()].locked++) {
		rt_scheduling[cpuid].rqsted = 0;
	}
	rtai_restore_flags(flags);
}

#define SCHED_UNLOCK_SCHEDULE(cpuid) \
	do { \
		rt_scheduling[cpuid].rqsted = 0; \
		sched_get_global_lock(cpuid); \
		rt_schedule(); \
		sched_release_global_lock(cpuid); \
	} while (0)


void rt_sched_unlock(void)
{
	unsigned long flags;
	int cpuid;

	rtai_save_flags_and_cli(flags);
	if (rt_scheduling[cpuid = rtai_cpuid()].locked && !(--rt_scheduling[cpuid].locked)) {
		if (rt_scheduling[cpuid].rqsted > 0) {
			SCHED_UNLOCK_SCHEDULE(cpuid);
		}
	} else {
//		rt_printk("*** TOO MANY SCHED_UNLOCK ***\n");
	}
	rtai_restore_flags(flags);
}


//#ifdef CONFIG_RTAI_SCHED_ISR_LOCK
void rtai_handle_isched_lock (int cpuid) /* Called with interrupts off */
{
	SCHED_UNLOCK_SCHEDULE(cpuid);
}
//#endif /* CONFIG_RTAI_SCHED_ISR_LOCK */


void *rt_get_lxrt_fun_entry(int index);
static inline void sched_sem_signal(SEM *sem)
{
	((RTAI_SYSCALL_MODE void (*)(SEM *, ...))rt_get_lxrt_fun_entry(SEM_SIGNAL))(sem);
}

int clr_rtext(RT_TASK *task)
{
	DECLARE_RT_CURRENT;
	unsigned long flags;
	QUEUE *q;

	if (task->magic != RT_TASK_MAGIC || task->priority == RT_SCHED_LINUX_PRIORITY) {
		return -EINVAL;
	}

	flags = rt_global_save_flags_and_cli();
	ASSIGN_RT_CURRENT;
	if (!task_owns_sems(task) || task == rt_current || rt_current->priority == RT_SCHED_LINUX_PRIORITY) {
		call_exit_handlers(task);
		rem_timed_task(task);
		if (task->blocked_on) {
			if (task->state & (RT_SCHED_SEMAPHORE | RT_SCHED_SEND | RT_SCHED_RPC | RT_SCHED_RETURN)) {
				(task->queue.prev)->next = task->queue.next;
				(task->queue.next)->prev = task->queue.prev;
				if (task->state & RT_SCHED_SEMAPHORE) {
					SEM *sem = (SEM *)(task->blocked_on);
					if (++sem->count > 1 && sem->type) {
						sem->count = 1;
					}
				}
			} else if (task->state & RT_SCHED_MBXSUSP) {
				MBX *mbx = (MBX *)task->blocked_on;
				mbx->waiting_task = NULL;
				sched_sem_signal(!mbx->frbs ? &mbx->sndsem : &mbx->rcvsem);
			}
		}
		q = &(task->msg_queue);
		while ((q = q->next) != &(task->msg_queue)) {
			rem_timed_task(q->task);
			if ((q->task)->state != RT_SCHED_READY && ((q->task)->state &= ~(RT_SCHED_SEND | RT_SCHED_RPC | RT_SCHED_DELAYED)) == RT_SCHED_READY) {
				enq_ready_task(q->task);
			}       
			(q->task)->blocked_on = RTP_OBJREM;
		}       
                q = &(task->ret_queue);
                while ((q = q->next) != &(task->ret_queue)) {
			rem_timed_task(q->task);
                       	if ((q->task)->state != RT_SCHED_READY && ((q->task)->state &= ~(RT_SCHED_RETURN | RT_SCHED_DELAYED)) == RT_SCHED_READY) {
				enq_ready_task(q->task);
			}       
			(q->task)->blocked_on = RTP_OBJREM;
               	}
		if (!((task->prev)->next = task->next)) {
			rt_smp_linux_task[task->runnable_on_cpus].prev = task->prev;
		} else {
			(task->next)->prev = task->prev;
		}
		if (rt_smp_fpu_task[task->runnable_on_cpus] == task) {
			rt_smp_fpu_task[task->runnable_on_cpus] = rt_smp_linux_task + task->runnable_on_cpus;;
		}
		if (!task->lnxtsk) {
			frstk_srq.mp[frstk_srq.in++ & (MAX_FRESTK_SRQ - 1)] = task->stack_bottom;
			rt_pend_linux_srq(frstk_srq.srq);
		}
		task->magic = 0;
		rem_ready_task(task);
		task->state = 0;
		atomic_dec((void *)(tasks_per_cpu + task->runnable_on_cpus));
		if (task == rt_current) {
			rt_schedule();
		}
	} else {
		task->suspdepth = -0x7FFFFFFF;
	}
	rt_global_restore_flags(flags);
	return 0;
}


int rt_task_delete(RT_TASK *task)
{
	if (!clr_rtext(task)) {
		if (task->lnxtsk) {
			start_stop_kthread(task, 0, 0, 0, 0, 0, 0);
		}
	}
	return 0;
}


int rt_get_timer_cpu(void)
{
	return 1;
}


static void rt_timer_handler(void)
{
	RT_TASK *rt_current, *task, *new_task;
	int cpuid, prio, preempt;

	DO_TIMER_PROPER_OP();
	rt_current = rt_smp_current[cpuid = rtai_cpuid()];

	rt_times.tick_time = oneshot_timer ? rdtsc() : rt_times.intr_time;
	rt_time_h = rt_times.tick_time + rt_half_tick;
#ifdef USE_LINUX_TIMER
	if (rt_times.tick_time >= rt_times.linux_time) {
#ifdef CONFIG_GENERIC_CLOCKEVENTS
		if (rt_times.linux_tick > 0) {
			rt_times.linux_time += rt_times.linux_tick;
		} else {
			rt_times.linux_time = RT_TIME_END;
		}
#else
		rt_times.linux_time += rt_times.linux_tick;
#endif
		update_linux_timer(cpuid);
	}
#endif

	sched_get_global_lock(cpuid);
	wake_up_timed_tasks(cpuid);
	RR_YIELD();
	TASK_TO_SCHEDULE();

	if (oneshot_timer) {
#ifdef USE_LINUX_TIMER
		int islnx;
#endif
		shot_fired = 0;
		rt_times.intr_time = rt_times.tick_time + ONESHOT_SPAN;
		RR_INTR_TIME();
		task = &rt_linux_task;
		while ((task = task->tnext) != &rt_linux_task && task->resume_time < rt_times.intr_time) {
			if (task->priority <= prio) {
				rt_times.intr_time = task->resume_time;
				preempt = 1;
				break;
			}
		}
#ifndef USE_LINUX_TIMER
		if (preempt || prio == RT_SCHED_LINUX_PRIORITY) {
//			RTIME now;
			int delay;
#else
		if ((islnx = (prio == RT_SCHED_LINUX_PRIORITY)) || preempt) {
//			RTIME now;
			int delay;
			if (islnx) {
#ifdef CONFIG_GENERIC_CLOCKEVENTS
				if (rt_times.linux_time < rt_times.intr_time) {
					rt_times.intr_time = rt_times.linux_time;
				}
#else
				RTIME linux_intr_time;
				linux_intr_time = rt_times.linux_time > rt_times.tick_time ? rt_times.linux_time : rt_times.tick_time + rt_times.linux_tick;
				if (linux_intr_time < rt_times.intr_time) {
					rt_times.intr_time = linux_intr_time;
				}
#endif
			}
#endif
//			delay = (int)(rt_times.intr_time - (now = rdtsc())) - tuned.latency;
			delay = (int)(rt_times.intr_time - rt_time_h) - tuned.latency;
			if (delay >= tuned.setup_time_TIMER_CPUNIT) {
				delay = imuldiv(delay, TIMER_FREQ, tuned.cpu_freq);
			} else {
				delay = tuned.setup_time_TIMER_UNIT;
//				rt_times.intr_time = now + (tuned.setup_time_TIMER_CPUNIT);
				rt_times.intr_time = rt_time_h + (tuned.setup_time_TIMER_CPUNIT);
			}
			shot_fired = 1;
			rt_set_timer_delay(delay);
		}
	} else {
		rt_times.intr_time += rt_times.periodic_tick;
                rt_set_timer_delay(0);
	}
	sched_release_global_lock(cpuid);

	if (new_task != rt_current) {
		if (rt_scheduling[cpuid].locked) {
			rt_scheduling[cpuid].rqsted = 1;
			goto sched_exit;
		}
		if (USE_RTAI_TASKS && (!new_task->lnxtsk || !rt_current->lnxtsk)) {
			if (!(new_task = switch_rtai_tasks(rt_current, new_task, cpuid))) {
				goto sched_exit;
			}
		}
		if (new_task->is_hard > 0 || rt_current->is_hard > 0) {
			struct task_struct *prev;
			unsigned long sflags;
			if (rt_current->is_hard <= 0) {
				SAVE_LOCK_LINUX_IN_IRQ(cpuid);
				rt_linux_task.lnxtsk = prev = current;
				RST_EXEC_TIME();
			} else {
				sflags = rtai_linux_context[cpuid].sflags;
				prev = rt_current->lnxtsk;
				SET_EXEC_TIME();
			}
			rt_smp_current[cpuid] = new_task;
			lxrt_context_switch(prev, new_task->lnxtsk, cpuid);
			if (rt_current->is_hard <= 0) {
				RESTORE_UNLOCK_LINUX_IN_IRQ(cpuid);
			} else if (lnxtsk_uses_fpu(prev)) {
				restore_fpu(prev);
			}
		}
        }
sched_exit:
	rtai_cli();
}


#if defined(USE_LINUX_TIMER) && !defined(CONFIG_GENERIC_CLOCKEVENTS)

static irqreturn_t recover_jiffies(int irq, void *dev_id, struct pt_regs *regs) 
{
	rt_global_cli();
	if (linux_times->tick_time >= linux_times->linux_time) {
		linux_times->linux_time += linux_times->linux_tick;
		update_linux_timer(rtai_cpuid());
	}
	rt_global_sti();
	return RTAI_LINUX_IRQ_HANDLED;
} 

#define REQUEST_RECOVER_JIFFIES()  rt_request_linux_irq(TIMER_8254_IRQ, recover_jiffies, "rtai_jif_chk", recover_jiffies)

#define RELEASE_RECOVER_JIFFIES(timer)  rt_free_linux_irq(TIMER_8254_IRQ, recover_jiffies)

#else 

#define REQUEST_RECOVER_JIFFIES()

#define RELEASE_RECOVER_JIFFIES()

#endif


int rt_is_hard_timer_running(void) 
{ 
	return rt_sched_timed;
}


void rt_set_periodic_mode(void) 
{ 
	int cpuid;
	stop_rt_timer();
	for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
		oneshot_timer = oneshot_running = 0;
	}
}


void rt_set_oneshot_mode(void)
{ 
	int cpuid;
	stop_rt_timer();
	for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
		oneshot_timer = 1;
	}
}


#if defined(CONFIG_RTAI_RTC_FREQ) && CONFIG_RTAI_RTC_FREQ >= 2

#ifdef CONFIG_SMP

RTAI_SYSCALL_MODE RTIME start_rt_timer(int period)
{
	int cpuid;
	for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
		oneshot_timer = oneshot_running = 0;
		rt_smp_times[cpuid].linux_tick    = 0;
		rt_smp_times[cpuid].tick_time     = 0;
		rt_smp_times[cpuid].intr_time     = 0;
		rt_smp_times[cpuid].linux_time    = 0;
		rt_smp_times[cpuid].periodic_tick = 1;
		tuned.timers_tol[cpuid] = rt_half_tick = 0;
		rt_time_h = 0;
	}
	linux_times = rt_smp_times;
	rt_request_irq(RTAI_APIC_TIMER_IPI, (void *)rt_timer_handler, NULL, 0);
	rt_request_rtc(CONFIG_RTAI_RTC_FREQ, NULL);
	rt_sched_timed = 1;
	rt_gettimeorig(NULL);
	return 1LL;
}


void stop_rt_timer(void)
{
	if (rt_sched_timed) {
		int cpuid;
		rt_sched_timed = 0;
		rt_release_rtc();
		rt_release_irq(RTAI_APIC_TIMER_IPI);
		for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
			rt_time_h = RT_TIME_END;
			oneshot_running = 0;
		}
	}
}

#else /* !CONFIG_SMP */

RTAI_SYSCALL_MODE RTIME start_rt_timer(int period)
{
	int const cpuid = 0;
	oneshot_timer = oneshot_running = 0;
	rt_smp_times[cpuid].linux_tick    = 0;
	rt_smp_times[cpuid].tick_time     = 0;
	rt_smp_times[cpuid].intr_time     = 0;
	rt_smp_times[cpuid].linux_time    = 0;
	rt_smp_times[cpuid].periodic_tick = 1;
	tuned.timers_tol[0] = rt_half_tick = 0;
	rt_time_h = 0;
	linux_times = rt_smp_times;
	rt_request_rtc(CONFIG_RTAI_RTC_FREQ, (void *)rt_timer_handler);
	rt_sched_timed = 1;
	rt_gettimeorig(NULL);
	return 1LL;
}

void stop_rt_timer(void)
{
	if (rt_sched_timed) {
		rt_sched_timed = 0;
		rt_release_rtc();
		rt_time_h = RT_TIME_END;
		rt_smp_oneshot_timer[0] = 0;
	}
}

#endif /* CONFIG_SMP */

RTAI_SYSCALL_MODE void start_rt_apic_timers(struct apic_timer_setup_data *setup_data, unsigned int rcvr_jiffies_cpuid)
{
	start_rt_timer(0);
}

#else /* !CONFIG_RTAI_RTC_FREQ */

#ifdef CONFIG_GENERIC_CLOCKEVENTS

#include <linux/clockchips.h>
#include <linux/ipipe_tickdev.h>

extern void *rt_linux_hrt_set_mode;
extern void *rt_linux_hrt_next_shot;

static void _rt_linux_hrt_set_mode(enum clock_event_mode mode, struct ipipe_tick_device *hrt_dev)
{
	int cpuid = rtai_cpuid();

	if (mode == CLOCK_EVT_MODE_ONESHOT || mode == CLOCK_EVT_MODE_SHUTDOWN) {
		rt_times.linux_tick = 0;
	} else if (mode == CLOCK_EVT_MODE_PERIODIC) {
		rt_times.linux_tick = nano2count_cpuid((1000000000 + HZ/2)/HZ, cpuid);
	}
}

static int _rt_linux_hrt_next_shot(unsigned long deltat, struct ipipe_tick_device *hrt_dev)
{
	int cpuid = rtai_cpuid();

	deltat = nano2count_cpuid(deltat, cpuid);
	rtai_cli();
	rt_times.linux_time = rt_get_time_cpuid(cpuid) + deltat;
	if (oneshot_running) {
		if (rt_times.linux_time < rt_times.intr_time) {
			int delay;
			rt_times.intr_time = rt_times.linux_time;
			delay = deltat - tuned.latency;
			if (delay >= tuned.setup_time_TIMER_CPUNIT) {
				delay = imuldiv(delay, TIMER_FREQ, tuned.cpu_freq);
			} else {
				delay = tuned.setup_time_TIMER_UNIT;
				rt_times.intr_time = rt_time_h + (tuned.setup_time_TIMER_CPUNIT);
			}
			shot_fired = 1;
			rt_set_timer_delay(delay);
		}
	}
	rtai_sti();
	return 0;
}

#endif /* CONFIG_GENERIC_CLOCKEVENTS */

#ifdef CONFIG_SMP

RTAI_SYSCALL_MODE void start_rt_apic_timers(struct apic_timer_setup_data *setup_data, unsigned int rcvr_jiffies_cpuid)
{
	unsigned long flags, cpuid;

	rt_request_apic_timers(rt_timer_handler, setup_data);
	flags = rt_global_save_flags_and_cli();
	for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
		if (setup_data[cpuid].mode > 0) {
			oneshot_timer = oneshot_running = 0;
			tuned.timers_tol[cpuid] = rt_half_tick = (rt_times.periodic_tick + 1)>>1;
		} else {
			oneshot_timer = oneshot_running = 1;
			tuned.timers_tol[cpuid] = rt_half_tick = (tuned.latency + 1)>>1;
		}
		rt_time_h = rt_times.tick_time + rt_half_tick;
		shot_fired = 1;
	}
	rt_sched_timed = 1;
	linux_times = rt_smp_times + (rcvr_jiffies_cpuid < NR_RT_CPUS ? rcvr_jiffies_cpuid : 0);
	rt_global_restore_flags(flags);
}


RTAI_SYSCALL_MODE RTIME start_rt_timer(int period)
{
	int cpuid;
	struct apic_timer_setup_data setup_data[NR_RT_CPUS];
	if (period <= 0) {
		period = 0;
		rt_set_oneshot_mode();
	}
	for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
		setup_data[cpuid].mode = oneshot_timer ? 0 : 1;
		setup_data[cpuid].count = count2nano(period);
	}
	start_rt_apic_timers(setup_data, rtai_cpuid());
	rt_gettimeorig(NULL);
	return setup_data[0].mode ? setup_data[0].count : period;
}


void stop_rt_timer(void)
{
	if (rt_sched_timed) {
		int cpuid; 
		rt_sched_timed = 0;
		rt_free_apic_timers();
		for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
			rt_time_h = RT_TIME_END;
			oneshot_running = 0;
		}
	}
}

#else /* !CONFIG_SMP */

#ifndef TIMER_TYPE
#define TIMER_TYPE  1
#endif

RTAI_SYSCALL_MODE RTIME start_rt_timer(int period)
{
#define cpuid 0
#undef rt_times

        unsigned long flags;
	if (period <= 0) {
		period = 0;
		rt_set_oneshot_mode();
	}
        flags = rt_global_save_flags_and_cli();
        if (oneshot_timer) {
		rt_request_timer(rt_timer_handler, 0, TIMER_TYPE);
                tuned.timers_tol[0] = rt_half_tick = (tuned.latency + 1)>>1;
                oneshot_running = shot_fired = 1;
        } else {
		rt_request_timer(rt_timer_handler, !TIMER_TYPE && period > LATCH ? LATCH: period, TIMER_TYPE);
                tuned.timers_tol[0] = rt_half_tick = (rt_times.periodic_tick + 1)>>1;
        }
	rt_sched_timed = 1;
	rt_smp_times[cpuid].linux_tick    = rt_times.linux_tick;
	rt_smp_times[cpuid].tick_time     = rt_times.tick_time;
	rt_smp_times[cpuid].intr_time     = rt_times.intr_time;
	rt_smp_times[cpuid].linux_time    = rt_times.linux_time;
	rt_smp_times[cpuid].periodic_tick = rt_times.periodic_tick;
        rt_time_h = rt_times.tick_time + rt_half_tick;
	linux_times = rt_smp_times;
        rt_global_restore_flags(flags);
	REQUEST_RECOVER_JIFFIES();
	rt_gettimeorig(NULL);
        return period;

#undef cpuid
#define rt_times (rt_smp_times[cpuid])
}


RTAI_SYSCALL_MODE void start_rt_apic_timers(struct apic_timer_setup_data *setup_mode, unsigned int rcvr_jiffies_cpuid)
{
	int cpuid, period;

	period = 0;
	for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
		period += setup_mode[cpuid].mode;
	}
	if (period == NR_RT_CPUS) {
		period = 2000000000;
		for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
			if (setup_mode[cpuid].count < period) {
				period = setup_mode[cpuid].count;
			}
		}
		start_rt_timer(nano2count(period));
	} else {
		rt_set_oneshot_mode();
		start_rt_timer(0);
	}
}


void stop_rt_timer(void)
{
	if (rt_sched_timed) {
		rt_sched_timed = 0;
		RELEASE_RECOVER_JIFFIES();
		rt_free_timer();
		rt_time_h = RT_TIME_END;
		rt_smp_oneshot_timer[0] = 0;
	}
}

#endif /* CONFIG_SMP */

#endif /* CONFIG_RTAI_RTC_FREQ */

int rt_sched_type(void)
{
	return RT_SCHED_MUP;
}


RTAI_SYSCALL_MODE int rt_hard_timer_tick_count(void)
{
	int cpuid = rtai_cpuid();
	if (rt_sched_timed) {
		return oneshot_timer ? 0 : rt_smp_times[cpuid].periodic_tick;
	}
	return -1;
}


RTAI_SYSCALL_MODE int rt_hard_timer_tick_count_cpuid(int cpuid)
{
	if (rt_sched_timed) {
		return oneshot_timer ? 0 : rt_smp_times[cpuid].periodic_tick;
	}
	return -1;
}


RT_TRAP_HANDLER rt_set_task_trap_handler( RT_TASK *task, unsigned int vec, RT_TRAP_HANDLER handler)
{
	RT_TRAP_HANDLER old_handler;

	if (!task || (vec >= RTAI_NR_TRAPS)) {
		return (RT_TRAP_HANDLER) -EINVAL;
	}
	old_handler = task->task_trap_handler[vec];
	task->task_trap_handler[vec] = handler;
	return old_handler;
}

static int OneShot = CONFIG_RTAI_ONE_SHOT;
RTAI_MODULE_PARM(OneShot, int);

static int Latency = TIMER_LATENCY;
RTAI_MODULE_PARM(Latency, int);

static int SetupTimeTIMER = TIMER_SETUP_TIME;
RTAI_MODULE_PARM(SetupTimeTIMER, int);

extern void krtai_objects_release(void);

static void frstk_srq_handler(void)
{
        while (frstk_srq.out != frstk_srq.in) {
		rt_kstack_free(frstk_srq.mp[frstk_srq.out++ & (MAX_FRESTK_SRQ - 1)]);
	}
}

static void nihil(void) { };
struct rt_fun_entry rt_fun_lxrt[MAX_LXRT_FUN];

void reset_rt_fun_entries(struct rt_native_fun_entry *entry)
{
	while (entry->fun.fun) {
		if (entry->index >= MAX_LXRT_FUN) {
			rt_printk("*** RESET ENTRY %d FOR USER SPACE CALLS EXCEEDS ALLOWD TABLE SIZE %d, NOT USED ***\n", entry->index, MAX_LXRT_FUN);
		} else {
			rt_fun_lxrt[entry->index] = (struct rt_fun_entry){ 1, nihil };
		}
		entry++;
        }
}

int set_rt_fun_entries(struct rt_native_fun_entry *entry)
{
	int error;
	error = 0;
	while (entry->fun.fun) {
		if (rt_fun_lxrt[entry->index].fun != nihil) {
			rt_printk("*** SUSPICIOUS ENTRY ASSIGNEMENT FOR USER SPACE CALL AT %d, DUPLICATED INDEX OR REPEATED INITIALIZATION ***\n", entry->index);
			error = -1;
		} else if (entry->index >= MAX_LXRT_FUN) {
			rt_printk("*** ASSIGNEMENT ENTRY %d FOR USER SPACE CALLS EXCEEDS ALLOWED TABLE SIZE %d, NOT USED ***\n", entry->index, MAX_LXRT_FUN);
			error = -1;
		} else {
			rt_fun_lxrt[entry->index] = entry->fun;
		}
		entry++;
        }
	if (error) {
		reset_rt_fun_entries(entry);
	}
	return 0;
}

void *rt_get_lxrt_fun_entry(int index) {
	return rt_fun_lxrt[index].fun;
}

static void lxrt_killall (void)
{
	int cpuid;
	
	stop_rt_timer();
	for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
		while (rt_linux_task.next) {
			rt_task_delete(rt_linux_task.next);
		}
	}
}

static int lxrt_notify_reboot (struct notifier_block *nb, unsigned long event, void *p)

{
    switch (event)
	{
	case SYS_DOWN:
	case SYS_HALT:
	case SYS_POWER_OFF:

	    /* FIXME: this is far too late. */
	    printk("LXRT: REBOOT NOTIFIED -- KILLING TASKS\n");
	    lxrt_killall();
	}

    return NOTIFY_DONE;
}

/* ++++++++++++++++++++++++++ TIME CONVERSIONS +++++++++++++++++++++++++++++ */

RTAI_SYSCALL_MODE RTIME count2nano(RTIME counts)
{
	int sign;

	if (counts >= 0) {
		sign = 1;
	} else {
		sign = 0;
		counts = - counts;
	}
	counts = oneshot_timer_cpuid ?
		 llimd(counts, 1000000000, tuned.cpu_freq):
		 llimd(counts, 1000000000, TIMER_FREQ);
	return sign ? counts : - counts;
}


RTAI_SYSCALL_MODE RTIME nano2count(RTIME ns)
{
	int sign;

	if (ns >= 0) {
		sign = 1;
	} else {
		sign = 0;
		ns = - ns;
	}
	ns =  oneshot_timer_cpuid ?
	      llimd(ns, tuned.cpu_freq, 1000000000) :
	      llimd(ns, TIMER_FREQ, 1000000000);
	return sign ? ns : - ns;
}

RTAI_SYSCALL_MODE RTIME count2nano_cpuid(RTIME counts, unsigned int cpuid)
{
	int sign;

	if (counts >= 0) {
		sign = 1;
	} else {
		sign = 0;
		counts = - counts;
	}
	counts = oneshot_timer ?
		 llimd(counts, 1000000000, tuned.cpu_freq):
		 llimd(counts, 1000000000, TIMER_FREQ);
	return sign ? counts : - counts;
}


RTAI_SYSCALL_MODE RTIME nano2count_cpuid(RTIME ns, unsigned int cpuid)
{
	int sign;

	if (ns >= 0) {
		sign = 1;
	} else {
		sign = 0;
		ns = - ns;
	}
	ns =  oneshot_timer ?
	      llimd(ns, tuned.cpu_freq, 1000000000) :
	      llimd(ns, TIMER_FREQ, 1000000000);
	return sign ? ns : - ns;
}

/* +++++++++++++++++++++++++++++++ TIMINGS ++++++++++++++++++++++++++++++++++ */

RTIME rt_get_time(void)
{
	int cpuid;
	return rt_smp_oneshot_timer[cpuid = rtai_cpuid()] ? rdtsc() : rt_smp_times[cpuid].tick_time;
}

RTAI_SYSCALL_MODE RTIME rt_get_time_cpuid(unsigned int cpuid)
{
	return oneshot_timer ? rdtsc(): rt_times.tick_time;
}

RTIME rt_get_time_ns(void)
{
	int cpuid = rtai_cpuid();
	return oneshot_timer ? llimd(rdtsc(), 1000000000, tuned.cpu_freq) :
	    		       llimd(rt_times.tick_time, 1000000000, TIMER_FREQ);
}

RTAI_SYSCALL_MODE RTIME rt_get_time_ns_cpuid(unsigned int cpuid)
{
	return oneshot_timer ? llimd(rdtsc(), 1000000000, tuned.cpu_freq) :
			       llimd(rt_times.tick_time, 1000000000, TIMER_FREQ);
}

RTIME rt_get_cpu_time_ns(void)
{
	return llimd(rdtsc(), 1000000000, tuned.cpu_freq);
}

extern struct epoch_struct boot_epoch;

RTIME rt_get_real_time(void)
{
	return boot_epoch.time[boot_epoch.touse][0] + rtai_rdtsc();
}

RTIME rt_get_real_time_ns(void)
{
	return boot_epoch.time[boot_epoch.touse][1] + llimd(rtai_rdtsc(), 1000000000, tuned.cpu_freq);
}

/* +++++++++++++++++++++++++++ SECRET BACK DOORS ++++++++++++++++++++++++++++ */

RT_TASK *rt_get_base_linux_task(RT_TASK **base_linux_tasks)
{
        int cpuid;
        for (cpuid = 0; cpuid < num_online_cpus(); cpuid++) {
                base_linux_tasks[cpuid] = rt_smp_linux_task + cpuid;
        }
        return rt_smp_linux_task;
}

RT_TASK *rt_alloc_dynamic_task(void)
{
#ifdef CONFIG_RTAI_MALLOC
        return rt_malloc(sizeof(RT_TASK)); // For VC's, proxies and C++ support.
#else
	return NULL;
#endif
}

/* +++++++++++++++++++++++++++ WATCHDOG SUPPORT ++++++++++++++++++++++++++++ */

RT_TASK **rt_register_watchdog(RT_TASK *wd, int cpuid)
{
    	RT_TASK *task;

	if (lxrt_wdog_task[cpuid]) return (RT_TASK**) -EBUSY;
	task = &rt_linux_task;
	while ((task = task->next)) {
		if (task != wd && task->priority == RT_SCHED_HIGHEST_PRIORITY) {
			return (RT_TASK**) -EBUSY;
		}
	}
	lxrt_wdog_task[cpuid] = wd;
	return (RT_TASK**) 0;
}

void rt_deregister_watchdog(RT_TASK *wd, int cpuid)
{
    	if (lxrt_wdog_task[cpuid] != wd) return;
	lxrt_wdog_task[cpuid] = NULL;
}

/* +++++++++++++++ SUPPORT FOR LINUX TASKS AND KERNEL THREADS +++++++++++++++ */

//#define ECHO_SYSW
#ifdef ECHO_SYSW
#define SYSW_DIAG_MSG(x) x
#else
#define SYSW_DIAG_MSG(x)
#endif

static RT_TRAP_HANDLER lxrt_old_trap_handler;

static inline void _rt_schedule_soft_tail(RT_TASK *rt_task, int cpuid)
{
	rt_global_cli();
	rt_task->state &= ~(RT_SCHED_READY | RT_SCHED_SFTRDY);
	(rt_task->rprev)->rnext = rt_task->rnext;
	(rt_task->rnext)->rprev = rt_task->rprev;
	rt_smp_current[cpuid] = &rt_linux_task;
	rt_schedule();
	UNLOCK_LINUX(cpuid);
	rt_global_sti();

#ifdef CONFIG_RTAI_ALIGN_LINUX_PRIORITY
do {
	int rt_priority;
	struct task_struct *lnxtsk;

	if ((lnxtsk = rt_task->lnxtsk)->policy == SCHED_FIFO || lnxtsk->policy == SCHED_RR) {
		if ((rt_priority = rt_task->priority) >= BASE_SOFT_PRIORITY) {
			rt_priority -= BASE_SOFT_PRIORITY;
		}
		if ((rt_priority = (MAX_LINUX_RTPRIO - rt_priority)) < 1) {
			rt_priority = 1;
		}
		if (rt_priority != lnxtsk->rt_priority) {
			rtai_set_linux_task_priority(lnxtsk, lnxtsk->policy, rt_priority);
		}
	}
} while (0);
#endif
}

void rt_schedule_soft(RT_TASK *rt_task)
{
	struct fun_args *funarg;
	int cpuid;

	rt_global_cli();
	rt_task->state |= RT_SCHED_READY;
	while (rt_task->state != RT_SCHED_READY) {
		current->state = TASK_SOFTREALTIME;
		rt_global_sti();
		schedule();
		rt_global_cli();
	}
	cpuid = rt_task->runnable_on_cpus;
	LOCK_LINUX(cpuid);
	enq_soft_ready_task(rt_task);
	rt_smp_current[cpuid] = rt_task;
	rt_global_sti();
	funarg = (void *)rt_task->fun_args;
	rt_task->retval = funarg->fun(RTAI_FUNARGS);
	_rt_schedule_soft_tail(rt_task, cpuid);
}

void rt_schedule_soft_tail(RT_TASK *rt_task, int cpuid)
{
	_rt_schedule_soft_tail(rt_task, cpuid);
}

static inline void fast_schedule(RT_TASK *new_task, struct task_struct *lnxtsk, int cpuid)
{
	RT_TASK *rt_current;
	rt_global_cli();
	new_task->state |= RT_SCHED_READY;
	enq_soft_ready_task(new_task);
	sched_release_global_lock(cpuid);
	LOCK_LINUX(cpuid);
	(rt_current = &rt_linux_task)->lnxtsk = lnxtsk;
	SET_EXEC_TIME();
	rt_smp_current[cpuid] = new_task;
	lxrt_context_switch(lnxtsk, new_task->lnxtsk, cpuid);
	UNLOCK_LINUX(cpuid);
	rtai_sti();
}


static RT_TASK thread_task[NR_RT_CPUS];
static int rsvr_cnt[NR_RT_CPUS];

#if USE_RTAI_TASKS
#define RESERVOIR 0
#else
#define RESERVOIR 11
#endif
static int Reservoir = RESERVOIR;
RTAI_MODULE_PARM(Reservoir, int);
static int SpareKthreads = 100;
RTAI_MODULE_PARM(SpareKthreads, int);

static int taskidx[NR_RT_CPUS];
static struct task_struct **taskav[NR_RT_CPUS];

static struct task_struct *__get_kthread(int cpuid)
{
	unsigned long flags;
	struct task_struct *p;

	flags = rt_global_save_flags_and_cli();
	if (taskidx[cpuid] > 0) {
		p = taskav[cpuid][--taskidx[cpuid]];
		rt_global_restore_flags(flags);
		return p;
	}
	rt_global_restore_flags(flags);
	return 0;
}


/* detach the kernel thread from user space; not fully, only:
   session, process-group, tty. */ 

static inline void detach_kthread(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	current->session = 1;
	current->pgrp    = 1;
	current->tty     = NULL;
#else
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,19)
	(current->signal)->__session = 1;
#else
	(current->signal)->session   = 1;
#endif
	(current->signal)->pgrp    = 1;
	(current->signal)->tty     = NULL;
#endif
}

static inline void lxrt_sigfillset(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	spin_lock_irq(&current->sigmask_lock);
	sigfillset(&current->blocked);
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);
#else
	spin_lock_irq(&(current->sighand)->siglock);
	sigfillset(&current->blocked);
	recalc_sigpending();
	spin_unlock_irq(&(current->sighand)->siglock);
#endif
}

static void kthread_fun(int cpuid) 
{
	void steal_from_linux(RT_TASK *);
	void give_back_to_linux(RT_TASK *, int);
	RT_TASK *task;

	detach_kthread();
	rtai_set_linux_task_priority(current, SCHED_FIFO, KTHREAD_F_PRIO);
	sprintf(current->comm, "F:HARD:%d:%d", cpuid, ++rsvr_cnt[cpuid]);
	current->rtai_tskext(TSKEXT0) = task = &thread_task[cpuid];
	current->rtai_tskext(TSKEXT1) = task->lnxtsk = current;
	lxrt_sigfillset();
	put_current_on_cpu(cpuid);
	init_hard_fpu(current);
	task->msg_queue.next = &task->msg_queue;
	task->resq.next = &task->resq;
	steal_from_linux(task);
	while(1) {
		rt_task_suspend(task);
		current->comm[0] = 'U';
		if (!(task = current->rtai_tskext(TSKEXT0))->max_msg_size[0]) {
			break;
		}
		task->exectime[1] = rdtsc();
		((void (*)(long))task->max_msg_size[0])(task->max_msg_size[1]);
		task->owndres = 0;
		current->comm[0] = 'F';
		current->rtai_tskext(TSKEXT1) = 0;
		rtai_cli();
		if (taskidx[cpuid] < SpareKthreads) {
			taskav[cpuid][taskidx[cpuid]++] = task->lnxtsk;
		}
		rtai_sti();
	}
	give_back_to_linux(task, 0);
	clr_rtext(task);
}

#define WAKE_UP_TASKs(klist) \
do { \
	struct klist_t *p = &klist[cpuid]; \
	while (p->out != p->in) { \
		wake_up_process(p->task[p->out++ & (MAX_WAKEUP_SRQ - 1)]); \
	} \
} while (0)

static void kthread_m(int cpuid)
{
	struct task_struct *lnxtsk;
	struct klist_t *klistp;
	RT_TASK *task;
	
	detach_kthread();
	(task = &thread_task[cpuid])->magic = RT_TASK_MAGIC;
	task->runnable_on_cpus = cpuid;
	sprintf(current->comm, "RTAI_KTHRD_M:%d", cpuid);
	put_current_on_cpu(cpuid);
	kthreadm[cpuid] = current;
	klistp = &klistm[cpuid];
	rtai_set_linux_task_priority(current, SCHED_FIFO, KTHREAD_M_PRIO);
	lxrt_sigfillset();
	up(&resem[cpuid]);
	while (!endkthread) {
		current->state = TASK_UNINTERRUPTIBLE;
		schedule();
#if defined(CONFIG_SMP) && LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
		WAKE_UP_TASKs(wake_up_hts);
#endif
		while (klistp->out != klistp->in) {
			unsigned long hard, flags;
			flags = rt_global_save_flags_and_cli();
			hard = (unsigned long)(lnxtsk = klistp->task[klistp->out++ & (MAX_WAKEUP_SRQ - 1)]);
			if (hard > 1) {
				if (lnxtsk->rtai_tskext(TSKEXT2)) {
					if (lnxtsk->rtai_tskext(TSKEXT1) && taskidx[cpuid] < SpareKthreads) {;
						taskav[cpuid][taskidx[cpuid]++] = lnxtsk;
						lnxtsk->comm[0] = 'F';
					}
					kthread_fun_long_jump(lnxtsk);
				}
			} else {
				if (taskidx[cpuid] < Reservoir) {
					task->suspdepth = task->state = 0;
					rt_global_sti();
					kernel_thread((void *)kthread_fun, (void *)(long)cpuid, 0);
					while (task->state != (RT_SCHED_READY | RT_SCHED_SUSPENDED)) {
						current->state = TASK_INTERRUPTIBLE;
						schedule_timeout(2);
					}
					kthread_fun_set_jump(task->lnxtsk);
					rt_global_cli();
					taskav[cpuid][taskidx[cpuid]++] = (void *)task->lnxtsk;
				}
				if (hard) {
					rt_task_resume((void *)klistp->task[klistp->out++ & (MAX_WAKEUP_SRQ - 1)]);
				} else {
					rt_global_sti();
					up(&resem[cpuid]);
					rt_global_cli();
				}
			}
			rt_global_restore_flags(flags);
		}
	}
	kthreadm[cpuid] = 0;
}

void steal_from_linux(RT_TASK *rt_task)
{
	struct klist_t *klistp;
	struct task_struct *lnxtsk;

	if (signal_pending(rt_task->lnxtsk)) {
		rt_task->is_hard = -1;
		return;
	}
	klistp = &wake_up_sth[rt_task->runnable_on_cpus];
	rtai_cli();
	klistp->task[klistp->in++ & (MAX_WAKEUP_SRQ - 1)] = rt_task;
	if (rt_task->base_priority >= BASE_SOFT_PRIORITY) {
		rt_task->base_priority -= BASE_SOFT_PRIORITY;
	}
	if (rt_task->priority >= BASE_SOFT_PRIORITY) {
		rt_task->priority -= BASE_SOFT_PRIORITY;
	}
	rt_task->is_hard = 1;
#if defined(TASK_ATOMICSWITCH) && TASK_ATOMICSWITCH && defined(CONFIG_PREEMPT)
	preempt_disable();
	(lnxtsk = rt_task->lnxtsk)->state = (TASK_HARDREALTIME | TASK_ATOMICSWITCH);
	rtai_sti();
#else
	(lnxtsk = rt_task->lnxtsk)->state = TASK_HARDREALTIME;
#endif
	do {
		schedule();
	} while (rt_task->state != RT_SCHED_READY);
	if (!rt_task->exectime[1]) {
		rt_task->exectime[1] = rdtsc();
	}
	if (lnxtsk_uses_fpu(lnxtsk)) {
		rtai_cli();
		restore_fpu(lnxtsk);
		rtai_sti();
	}
}

void give_back_to_linux(RT_TASK *rt_task, int keeprio)
{
	struct task_struct *lnxtsk;
	int rt_priority;

	rt_global_cli();
	(rt_task->rprev)->rnext = rt_task->rnext;
	(rt_task->rnext)->rprev = rt_task->rprev;
	rt_task->state = 0;
	pend_wake_up_hts(lnxtsk = rt_task->lnxtsk, rt_task->runnable_on_cpus);
	rt_schedule();
	if (!(rt_task->is_hard = keeprio)) {
		if (rt_task->priority < BASE_SOFT_PRIORITY) {
			rt_priority = rt_task->priority;
			if (rt_task->priority == rt_task->base_priority) {
				rt_task->priority += BASE_SOFT_PRIORITY;
			}
		} else {
			rt_priority = rt_task->priority - BASE_SOFT_PRIORITY;
		}
		if (rt_task->base_priority < BASE_SOFT_PRIORITY) {
			rt_task->base_priority += BASE_SOFT_PRIORITY;
		}
	} else {
		if (rt_task->priority < BASE_SOFT_PRIORITY) {
			rt_priority = rt_task->priority;
		} else {
			rt_priority = rt_task->priority - BASE_SOFT_PRIORITY;
		}
	}
	rt_global_sti();
	/* Perform Linux's scheduling tail now since we woke up
	   outside the regular schedule() point. */
	hal_schedule_back_root(lnxtsk);

#ifdef CONFIG_RTAI_ALIGN_LINUX_PRIORITY
	if (lnxtsk->policy == SCHED_FIFO || lnxtsk->policy == SCHED_RR) {
		if ((rt_priority = (MAX_LINUX_RTPRIO - rt_priority)) < 1) {
			rt_priority = 1;
		}
		if (rt_priority != lnxtsk->rt_priority) {
			rtai_set_linux_task_priority(lnxtsk, lnxtsk->policy, rt_priority);
		}
	}
#endif

	return;
}

static struct task_struct *get_kthread(int get, int cpuid, void *lnxtsk)
{
	struct task_struct *kthread;
	struct klist_t *klistp;
	RT_TASK *this_task;
	int hard;

	klistp = &klistm[cpuid];
	if (get) {
		while (!(kthread = __get_kthread(cpuid))) {
			this_task = rt_smp_current[cpuid];
			rt_global_cli();
			klistp->task[klistp->in++ & (MAX_WAKEUP_SRQ - 1)] = (void *)(long)(hard = this_task->is_hard > 0 ? 1 : 0);
			klistp->task[klistp->in++ & (MAX_WAKEUP_SRQ - 1)] = (void *)this_task;
			pend_wake_up_srq(kthreadm[cpuid], cpuid);
			rt_global_sti();
        		if (hard) {
				rt_task_suspend(this_task);
			} else {
				down(&resem[cpuid]);
			}
		}
		rt_global_cli();
		klistp->task[klistp->in++ & (MAX_WAKEUP_SRQ - 1)] = 0;
		klistp->task[klistp->in++ & (MAX_WAKEUP_SRQ - 1)] = 0;
	} else {
		kthread = 0;
		rt_global_cli();
		klistp->task[klistp->in++ & (MAX_WAKEUP_SRQ - 1)] = lnxtsk;
	}
	pend_wake_up_srq(kthreadm[cpuid], cpuid);
	rt_global_sti();
	return kthread;
}

static void start_stop_kthread(RT_TASK *task, void (*rt_thread)(long), long data, int priority, int uses_fpu, void(*signal)(void), int runnable_on_cpus)
{
	if (num_online_cpus() == 1) {
		runnable_on_cpus = 0;
	}
	if (rt_thread) {
		task->retval = set_rtext(task, priority, uses_fpu, signal, runnable_on_cpus, get_kthread(1, runnable_on_cpus, 0));
		task->max_msg_size[0] = (long)rt_thread;
		task->max_msg_size[1] = data;
	} else {
		get_kthread(0, task->runnable_on_cpus, task->lnxtsk);
	}
}

static void wake_up_srq_handler(unsigned srq)
{
#ifdef CONFIG_PREEMPT
//	preempt_disable(); {
#endif
#ifdef CONFIG_SMP
	int cpuid = srq - wake_up_srq[0].srq;
#else
	int cpuid = 0; // optimises, ok for x86_64-2.6.10, which can be UP only.
#endif
#if !defined(CONFIG_SMP) || LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	WAKE_UP_TASKs(wake_up_hts);
#else
	wake_up_process(kthreadm[cpuid]);
#endif
	WAKE_UP_TASKs(wake_up_srq);
	set_need_resched();
#ifdef CONFIG_PREEMPT
//	} preempt_enable();
#endif
}

static unsigned long traptrans, systrans;

static int lxrt_handle_trap(int vec, int signo, struct pt_regs *regs, void *dummy_data)
{
	RT_TASK *rt_task;

	rt_task = rt_smp_current[smp_processor_id()];
	if (USE_RTAI_TASKS && !rt_task->lnxtsk) {
		if (rt_task->task_trap_handler[vec]) {
			return rt_task->task_trap_handler[vec](vec, signo, regs, rt_task);
		}
		rt_printk("Default Trap Handler: vector %d: Suspend RT task %p\n", vec, rt_task);
		rt_task_suspend(rt_task);
		return 1;
	}

	if (rt_task->is_hard > 0) {
		if (!traptrans++) {
			rt_printk("\nLXRT CHANGED MODE (TRAP), PID = %d, VEC = %d, SIGNO = %d.\n", (rt_task->lnxtsk)->pid, vec, signo);
		}
		SYSW_DIAG_MSG(rt_printk("\nFORCING IT SOFT (TRAP), PID = %d, VEC = %d, SIGNO = %d.\n", (rt_task->lnxtsk)->pid, vec, signo););
		give_back_to_linux(rt_task, -1);
		SYSW_DIAG_MSG(rt_printk("FORCED IT SOFT (TRAP), PID = %d, VEC = %d, SIGNO = %d.\n", (rt_task->lnxtsk)->pid, vec, signo););
	}

	return 0;
}

static inline void rt_signal_wake_up(RT_TASK *task)
{
	if (task->state && task->state != RT_SCHED_READY) {
		task->unblocked = 1;
		rt_task_masked_unblock(task, ~RT_SCHED_READY);
	} else {
		task->unblocked = -1;
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,32)
static struct mmreq {
    int in, out, count;
#define MAX_MM 32  /* Should be more than enough (must be a power of 2). */
#define bump_mmreq(x) do { x = (x + 1) & (MAX_MM - 1); } while(0)
    struct mm_struct *mm[MAX_MM];
} lxrt_mmrqtab[NR_CPUS];

struct prev_next_t { struct task_struct *prev, *next; };
static int lxrt_intercept_schedule_head (unsigned long event, struct prev_next_t *evdata)
{
    IN_INTERCEPT_IRQ_ENABLE(); {

    struct task_struct *prev = evdata->prev;

    /* The SCHEDULE_HEAD event is sent by the (Adeosized) Linux kernel
       each time it's about to switch a process out. This hook is
       aimed at preventing the last active MM from being dropped
       during the LXRT real-time operations since it's a lengthy
       atomic operation. See kernel/sched.c (schedule()) for more. The
       MM dropping is simply postponed until the SCHEDULE_TAIL event
       is received, right after the incoming task has been switched
       in. */

    if (!prev->mm)
	{
	struct mmreq *p = lxrt_mmrqtab + task_cpu(prev);
	struct mm_struct *oldmm = prev->active_mm;
	BUG_ON(p->count >= MAX_MM);
	/* Prevent the MM from being dropped in schedule(), then pend
	   a request to drop it later in lxrt_intercept_schedule_tail(). */
	atomic_inc(&oldmm->mm_count);
	p->mm[p->in] = oldmm;
	bump_mmreq(p->in);
	p->count++;
	}

	return 0;
} }

#endif  /* KERNEL_VERSION < 2.4.32 */

static int lxrt_intercept_schedule_tail (unsigned event, void *nothing)

{
	IN_INTERCEPT_IRQ_ENABLE(); {

	int cpuid = smp_processor_id();
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,11)
	if (in_hrt_mode(cpuid)) {
		return 1;
	} else 
#endif
	{
		struct klist_t *klistp = &wake_up_sth[cpuid];
		while (klistp->out != klistp->in) {
			fast_schedule(klistp->task[klistp->out++ & (MAX_WAKEUP_SRQ - 1)], current, cpuid);
		}
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,32)
    {
    struct mmreq *p;

#ifdef CONFIG_PREEMPT
    preempt_disable();
#endif /* CONFIG_PREEMPT */

    p = lxrt_mmrqtab + smp_processor_id();

    while (p->out != p->in)
	{
	struct mm_struct *oldmm = p->mm[p->out];
	mmdrop(oldmm);
	bump_mmreq(p->out);
	p->count--;
	}

#ifdef CONFIG_PREEMPT
    preempt_enable();
#endif /* CONFIG_PREEMPT */
    }
#endif  /* KERNEL_VERSION < 2.4.32 */

    return 0;
} }

struct sig_wakeup_t { struct task_struct *task; };
static int lxrt_intercept_sig_wakeup (long event, void *data)
{
	IN_INTERCEPT_IRQ_ENABLE(); {
	RT_TASK *task;
	if ((task = INTERCEPT_WAKE_UP_TASK(data)->rtai_tskext(TSKEXT0))) {
		rt_signal_wake_up(task);
		return 1;
	}
	return 0;
} }

static int lxrt_intercept_exit (unsigned long event, struct task_struct *lnx_task)
{
	IN_INTERCEPT_IRQ_ENABLE(); {

	extern void linux_process_termination(void);
	RT_TASK *task;
	if ((task = lnx_task->rtai_tskext(TSKEXT0))) {
		if (task->is_hard > 0) {
			give_back_to_linux(task, 0);
		}
		linux_process_termination();
	}
	return 0;
} }

extern long long rtai_lxrt_invoke (unsigned long, void *, void *);
extern int (*sys_call_table[])(struct pt_regs);

#if 0
static RT_TASK *server_task_init(int prio, int cpus_allowed)
{
	RT_TASK *tsk;
	if ((tsk = rt_malloc(sizeof(RT_TASK) + 2*sizeof(struct fun_args)))) { 
		tsk->magic = 0;
		if (!set_rtext(tsk, prio, 0, 0, cpus_allowed, 0)) {
			tsk->fun_args = (long *)((struct fun_args *)(tsk + 1));
			if (rt_register((unsigned long)tsk, tsk, IS_TASK, 0)) {
				return tsk;
			} else {
				clr_rtext(tsk);
			}
		}
		rt_free(tsk);
	}
	return 0;
}

static inline RT_TASK *soft_rt_linux_server_call(RT_TASK *task, void *fun, void *arg1, void *arg2)
{
	task->fun_args[0] = (long)arg1;
	task->fun_args[1] = (long)arg2;
	((struct fun_args *)task->fun_args)->fun = fun;
	rt_schedule_soft(task);
	return (RT_TASK *)(unsigned long)task->retval;
}

static void linux_syscall_server_fun(RT_TASK *master_task)
{
	RT_TASK *server_task;
        struct pt_regs regs;

	master_task->linux_syscall_server = server_task = server_task_init(master_task->base_priority >= BASE_SOFT_PRIORITY ? master_task->base_priority - BASE_SOFT_PRIORITY : master_task->base_priority, master_task->runnable_on_cpus);
	rt_task_resume(master_task);
	while (soft_rt_linux_server_call(server_task, rt_receive_linux_syscall, master_task, &regs) == master_task) {
		rt_return_linux_syscall(master_task, sys_call_table[regs.LINUX_SYSCALL_NR](regs));
	}
}

RT_TASK *lxrt_init_linux_server(RT_TASK *master_task)
{
	int is_hard;
	if (!master_task) {
		if (!current->rtai_tskext(TSKEXT0)) {
			return NULL;
		}
		master_task = current->rtai_tskext(TSKEXT0);
	}
	if (!master_task->lnxtsk) {
		return NULL;
	}
	if ((is_hard = master_task->is_hard) > 0) {
		give_back_to_linux(master_task, 0);
	}
	master_task->linux_syscall_server = NULL;
	kernel_thread((void *)linux_syscall_server_fun, master_task, CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	soft_rt_linux_server_call(master_task, rt_task_suspend, master_task, NULL);
	if (is_hard > 0) {
		steal_from_linux(master_task);
	}
	return master_task->linux_syscall_server;
}

#endif

static int lxrt_intercept_syscall_prologue(struct pt_regs *regs)
{
	IN_INTERCEPT_IRQ_ENABLE(); {
	RT_TASK *task;

	if (regs->LINUX_SYSCALL_NR < NR_syscalls && (task = current->rtai_tskext(TSKEXT0))) {
		if (task->is_hard > 0) {
			if (task->linux_syscall_server) {
				rt_exec_linux_syscall(task, task->linux_syscall_server, regs);
				return 1;
			}
			if (!systrans++) {
				rt_printk("\nLXRT CHANGED MODE (SYSCALL), PID = %d, SYSCALL = %lu.\n", (task->lnxtsk)->pid, regs->LINUX_SYSCALL_NR);
			}
			SYSW_DIAG_MSG(rt_printk("\nFORCING IT SOFT (SYSCALL), PID = %d, SYSCALL = %d.\n", (task->lnxtsk)->pid, regs->LINUX_SYSCALL_NR););
			give_back_to_linux(task, -1);
			SKIP_IMMEDIATE_LINUX_SYSCALL();
			if (signal_pending(task->lnxtsk)) return 0;
			SYSW_DIAG_MSG(rt_printk("FORCED IT SOFT, CALLING LINUX (SYSCALL), PID = %d, SYSCALL = %d.\n", (task->lnxtsk)->pid, regs->LINUX_SYSCALL_NR););
			LXRT_DO_IMMEDIATE_LINUX_SYSCALL(regs);
			SYSW_DIAG_MSG(rt_printk("LINUX RETURNED, GOING BACK TO HARD (SYSLXRT), PID = %d.\n", current->pid););
			steal_from_linux(task);
			SYSW_DIAG_MSG(rt_printk("GONE BACK TO HARD (SYSLXRT),  PID = %d.\n", current->pid););
			return 1;
		}
	}
	return 0;
} }

static int lxrt_intercept_syscall_epilogue(unsigned long event, void *nothing)
{
	IN_INTERCEPT_IRQ_ENABLE(); {

	RT_TASK *task;
	if ((task = (RT_TASK *)current->rtai_tskext(TSKEXT0))) {
		if (task->system_data_ptr) {
			struct pt_regs *r = task->system_data_ptr;
			r->LINUX_SYSCALL_RETREG = -ERESTARTSYS;
			r->LINUX_SYSCALL_NR = RTAI_SYSCALL_NR;
			task->system_data_ptr = NULL;
		} else if (task->is_hard < 0) {
			SYSW_DIAG_MSG(rt_printk("GOING BACK TO HARD (SYSLXRT), PID = %d.\n", current->pid););
			steal_from_linux(task);
			SYSW_DIAG_MSG(rt_printk("GONE BACK TO HARD (SYSLXRT),  PID = %d.\n", current->pid););
			return 1;
		}
	}
	return 0;
} }

/* ++++++++++++++++++++++++++ SCHEDULER PROC FILE +++++++++++++++++++++++++++ */

#ifdef CONFIG_PROC_FS
/* -----------------------< proc filesystem section >-------------------------*/

static int rtai_read_sched(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
	PROC_PRINT_VARS;
        int cpuid, i = 1;
	unsigned long t;
	RT_TASK *task;

	PROC_PRINT("\nRTAI LXRT Real Time Task Scheduler.\n\n");
	PROC_PRINT("    Calibrated CPU Frequency: %lu Hz\n", tuned.cpu_freq);
	PROC_PRINT("    Calibrated interrupt to scheduler latency: %d ns\n", (int)imuldiv(tuned.latency - tuned.setup_time_TIMER_CPUNIT, 1000000000, tuned.cpu_freq));
	PROC_PRINT("    Calibrated oneshot timer setup_to_firing time: %d ns\n\n",
                  (int)imuldiv(tuned.setup_time_TIMER_CPUNIT, 1000000000, tuned.cpu_freq));
	PROC_PRINT("Number of RT CPUs in system: %d\n\n", NR_RT_CPUS);

	PROC_PRINT("Real time kthreads in resorvoir (cpu/#)");
        for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
                PROC_PRINT(": (%d/%d)", cpuid, taskidx[cpuid]);
        }
	PROC_PRINT("\n\n");

	PROC_PRINT("Number of forced hard/soft/hard transitions: traps %lu, syscalls %lu\n\n", traptrans, systrans);

	PROC_PRINT("Priority  Period(ns)  FPU  Sig  State  CPU  Task  HD/SF  PID  RT_TASK *  TIME\n" );
	PROC_PRINT("------------------------------------------------------------------------------\n" );
        for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
                task = &rt_linux_task;
/*
* Display all the active RT tasks and their state.
*
* Note: As a temporary hack the tasks are given an id which is
*       the order they appear in the task list, needs fixing!
*/
		while ((task = task->next)) {
/*
* The display for the task period is set to an integer (%d) as 64 bit
* numbers are not currently handled correctly by the kernel routines.
* Hence the period display will be wrong for time periods > ~4 secs.
*/
			t = 0;
			if ((!task->lnxtsk || task->is_hard) && task->exectime[1]) {
				unsigned long den = (unsigned long)llimd(rdtsc() - task->exectime[1], 10, tuned.cpu_freq);
				if (den) {
					t = 1000UL*(unsigned long)llimd(task->exectime[0], 10, tuned.cpu_freq)/den;
				}				
			}
			PROC_PRINT("%-10d %-11lu %-4s %-3s 0x%-3x  %1lu:%1lu   %-4d   %-4d %-4d  %p   %-lu\n",
                               task->priority,
                               (unsigned long)count2nano_cpuid(task->period, task->runnable_on_cpus),
                               task->uses_fpu || task->lnxtsk ? "Yes" : "No",
                               task->signal ? "Yes" : "No",
                               task->state,
			       task->runnable_on_cpus, // cpuid,
			       task->lnxtsk ? CPUMASK((task->lnxtsk)->cpus_allowed) : (1 << task->runnable_on_cpus),
                               i,
			       task->is_hard,
			       task->lnxtsk ? task->lnxtsk->pid : 0,
			       task, t);
			i++;
                } /* End while loop - display all RT tasks on a CPU. */

		PROC_PRINT("TIMED\n");
		task = &rt_linux_task;
		while ((task = task->tnext) != &rt_linux_task) {
			PROC_PRINT("> %p ", task);
		}
		PROC_PRINT("\nREADY\n");
		task = &rt_linux_task;
		while ((task = task->rnext) != &rt_linux_task) {
			PROC_PRINT("> %p ", task);
		}

        }  /* End for loop - display RT tasks on all CPUs. */

	PROC_PRINT_DONE;

}  /* End function - rtai_read_sched */


static int rtai_proc_sched_register(void) 
{
        struct proc_dir_entry *proc_sched_ent;


        proc_sched_ent = create_proc_entry("scheduler", S_IFREG|S_IRUGO|S_IWUSR, rtai_proc_root);
        if (!proc_sched_ent) {
                printk("Unable to initialize /proc/rtai/scheduler\n");
                return(-1);
        }
        proc_sched_ent->read_proc = rtai_read_sched;
        return(0);
}  /* End function - rtai_proc_sched_register */


static void rtai_proc_sched_unregister(void) 
{
        remove_proc_entry("scheduler", rtai_proc_root);
}  /* End function - rtai_proc_sched_unregister */

/* --------------------< end of proc filesystem section >---------------------*/
#endif /* CONFIG_PROC_FS */

/* ++++++++++++++ SCHEDULER ENTRIES AND RELATED INITIALISATION ++++++++++++++ */

extern void usp_request_rtc(int, void *);
extern void rt_release_rtc(void);

static int rt_gettid(void)
{
	return current->pid;
}

static struct rt_native_fun_entry rt_sched_entries[] = {
	{ { 0, rt_set_runnable_on_cpus },	    SET_RUNNABLE_ON_CPUS },
	{ { 0, rt_set_runnable_on_cpuid },	    SET_RUNNABLE_ON_CPUID },
	{ { 0, rt_set_sched_policy },		    SET_SCHED_POLICY },
	{ { 0, rt_get_timer_cpu },		    GET_TIMER_CPU },
	{ { 0, rt_is_hard_timer_running },	    HARD_TIMER_RUNNING },
	{ { 0, rt_set_periodic_mode },		    SET_PERIODIC_MODE },
	{ { 0, rt_set_oneshot_mode },		    SET_ONESHOT_MODE },
	{ { 0, start_rt_timer },		    START_TIMER },
	{ { 0, start_rt_apic_timers },		    START_RT_APIC_TIMERS },
	{ { 0, stop_rt_timer },			    STOP_TIMER },
	{ { 0, rt_task_signal_handler },	    SIGNAL_HANDLER  },
	{ { 0, rt_task_use_fpu },		    TASK_USE_FPU },
	{ { 0, rt_hard_timer_tick_count },	    HARD_TIMER_COUNT },
	{ { 0, rt_hard_timer_tick_count_cpuid },    HARD_TIMER_COUNT_CPUID },
	{ { 0, count2nano },			    COUNT2NANO },
	{ { 0, nano2count },			    NANO2COUNT },
	{ { 0, count2nano_cpuid },		    COUNT2NANO_CPUID },
	{ { 0, nano2count_cpuid },		    NANO2COUNT_CPUID },
	{ { 0, rt_get_time },			    GET_TIME },
	{ { 0, rt_get_time_cpuid },		    GET_TIME_CPUID },
	{ { 0, rt_get_time_ns },		    GET_TIME_NS },
	{ { 0, rt_get_time_ns_cpuid },		    GET_TIME_NS_CPUID },
	{ { 0, rt_get_cpu_time_ns },		    GET_CPU_TIME_NS },
	{ { 0, rt_get_priorities },		    GET_PRIORITIES },
	{ { 0, rt_spv_RMS },			    SPV_RMS },
	{ { 1, rt_change_prio },		    CHANGE_TASK_PRIO },
	{ { 0, rt_sched_lock },			    SCHED_LOCK },
	{ { 0, rt_sched_unlock },		    SCHED_UNLOCK },
	{ { 1, rt_task_yield },			    YIELD },  
	{ { 1, rt_task_suspend },		    SUSPEND },
	{ { 1, rt_task_suspend_if },		    SUSPEND_IF },
	{ { 1, rt_task_suspend_until },		    SUSPEND_UNTIL },
	{ { 1, rt_task_suspend_timed },		    SUSPEND_TIMED },
	{ { 1, rt_task_resume },		    RESUME },
	{ { 1, rt_set_linux_syscall_mode },	    SET_LINUX_SYSCALL_MODE },
	{ { 1, rt_irq_wait },			    IRQ_WAIT },
	{ { 1, rt_irq_wait_if },		    IRQ_WAIT_IF },
	{ { 1, rt_irq_wait_until },		    IRQ_WAIT_UNTIL },
	{ { 1, rt_irq_wait_timed },		    IRQ_WAIT_TIMED },
	{ { 0, rt_irq_signal },			    IRQ_SIGNAL },
	{ { 0, rt_request_irq_task },		    REQUEST_IRQ_TASK },
	{ { 0, rt_release_irq_task },		    RELEASE_IRQ_TASK },
	{ { 1, rt_task_make_periodic_relative_ns }, MAKE_PERIODIC_NS },
	{ { 1, rt_task_make_periodic },		    MAKE_PERIODIC },
	{ { 1, rt_task_set_resume_end_times },	    SET_RESUME_END },
	{ { 0, rt_set_resume_time },  		    SET_RESUME_TIME },
	{ { 0, rt_set_period },			    SET_PERIOD },
	{ { 1, rt_task_wait_period },		    WAIT_PERIOD },
	{ { 0, rt_busy_sleep },			    BUSY_SLEEP },
	{ { 1, rt_sleep },			    SLEEP },
	{ { 1, rt_sleep_until },		    SLEEP_UNTIL },
	{ { 0, rt_task_masked_unblock },	    WAKEUP_SLEEPING },
	{ { 0, rt_named_task_init },		    NAMED_TASK_INIT },  
	{ { 0, rt_named_task_init_cpuid },	    NAMED_TASK_INIT_CPUID },  
	{ { 0, rt_named_task_delete },	 	    NAMED_TASK_DELETE },  
	{ { 0, rt_get_name },			    GET_NAME },
	{ { 0, rt_get_adr },			    GET_ADR },
	{ { 0, usr_rt_pend_linux_irq },		    PEND_LINUX_IRQ },
	{ { 0, usp_request_rtc },                   REQUEST_RTC },
	{ { 0, rt_release_rtc },                    RELEASE_RTC },
	{ { 0, rt_gettid },                         RT_GETTID },
	{ { 0, rt_get_real_time },		    GET_REAL_TIME },
	{ { 0, rt_get_real_time_ns },		    GET_REAL_TIME_NS },
	{ { 1, rt_signal_helper }, 		    RT_SIGNAL_HELPER },
	{ { 1, rt_wait_signal }, 		    RT_SIGNAL_WAITSIG },
	{ { 1, rt_request_signal_ },		    RT_SIGNAL_REQUEST },
	{ { 1, rt_release_signal },		    RT_SIGNAL_RELEASE },
	{ { 1, rt_enable_signal },		    RT_SIGNAL_ENABLE },
	{ { 1, rt_disable_signal },		    RT_SIGNAL_DISABLE },
	{ { 1, rt_trigger_signal }, 		    RT_SIGNAL_TRIGGER },
	{ { 0, 0 },			            000 }
};

extern void *rtai_lxrt_dispatcher;

DECLARE_FUSION_WAKE_UP_STUFF;

static int lxrt_init(void)

{
    void init_fun_ext(void);
    int cpuid;

    init_fun_ext();

    REQUEST_RESUME_SRQs_STUFF();

    /* We will start stealing Linux tasks as soon as the reservoir is
       instantiated, so create the migration service now. */

    if (Reservoir <= 0)
	Reservoir = 1;

    Reservoir = (Reservoir + NR_RT_CPUS - 1)/NR_RT_CPUS;

    for (cpuid = 0; cpuid < num_online_cpus(); cpuid++)
	{
	taskav[cpuid] = (void *)kmalloc(SpareKthreads*sizeof(void *), GFP_KERNEL);
	init_MUTEX_LOCKED(&resem[cpuid]);
	kernel_thread((void *)kthread_m, (void *)(long)cpuid, 0);
	down(&resem[cpuid]);
	klistm[cpuid].in = (2*Reservoir) & (MAX_WAKEUP_SRQ - 1);
	wake_up_process(kthreadm[cpuid]);
	}

    for (cpuid = 0; cpuid < MAX_LXRT_FUN; cpuid++)
	{
	rt_fun_lxrt[cpuid].type = 1;
	rt_fun_lxrt[cpuid].fun  = nihil;
	}
	
    set_rt_fun_entries(rt_sched_entries);

    lxrt_old_trap_handler = rt_set_rtai_trap_handler(lxrt_handle_trap);

#ifdef CONFIG_PROC_FS
    rtai_proc_lxrt_register();
#endif

    /* Must be called on behalf of the Linux domain. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,32)
    rtai_catch_event(hal_root_domain, HAL_SCHEDULE_HEAD, (void *)lxrt_intercept_schedule_head);
#endif  /* KERNEL_VERSION < 2.4.32 */
    rtai_catch_event(hal_root_domain, HAL_SCHEDULE_TAIL, (void *)lxrt_intercept_schedule_tail);
    rtai_catch_event(hal_root_domain, HAL_SYSCALL_PROLOGUE, (void *)lxrt_intercept_syscall_prologue);
    rtai_catch_event(hal_root_domain, HAL_SYSCALL_EPILOGUE, (void *)lxrt_intercept_syscall_epilogue);
    rtai_catch_event(hal_root_domain, HAL_EXIT_PROCESS, (void *)lxrt_intercept_exit);
    rtai_catch_event(hal_root_domain, HAL_KICK_PROCESS, (void *)lxrt_intercept_sig_wakeup);
	rtai_lxrt_dispatcher = rtai_lxrt_invoke;

    return 0;
}

static void lxrt_exit(void)
{
	RT_TASK *rt_task;
	struct task_struct *kthread;
	unsigned long flags;
	int cpuid;

#ifdef CONFIG_PROC_FS
	rtai_proc_lxrt_unregister();
#endif

	rt_task = kmalloc(sizeof(RT_TASK), GFP_KERNEL);
	for (cpuid = 0; cpuid < num_online_cpus(); cpuid++) {
		while ((kthread = __get_kthread(cpuid))) {
			if (kthread->rtai_tskext(TSKEXT2)) {
				kfree(kthread->rtai_tskext(TSKEXT2));
			}
			rt_task->magic = 0;
			set_rtext(rt_task, 0, 0, 0, cpuid, kthread);
			rt_task->max_msg_size[0] = 0;
			rt_task_resume(rt_task);
			while (rt_task->magic || rt_task->state) {
				current->state = TASK_INTERRUPTIBLE;
				schedule_timeout(2);
			}
		}
	}
	kfree(rt_task);

	endkthread = 1;
	for (cpuid = 0; cpuid < num_online_cpus(); cpuid++) {
		wake_up_process(kthreadm[cpuid]);
		while (kthreadm[cpuid]) {
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(2);
		}
		kfree(taskav[cpuid]);
	}

	rt_set_rtai_trap_handler(lxrt_old_trap_handler);

	RELEASE_RESUME_SRQs_STUFF();

	rtai_catch_event(hal_root_domain, HAL_SCHEDULE_HEAD, NULL);
	rtai_catch_event(hal_root_domain, HAL_SCHEDULE_TAIL, NULL);
	rtai_catch_event(hal_root_domain, HAL_SYSCALL_PROLOGUE, NULL);
	rtai_catch_event(hal_root_domain, HAL_SYSCALL_EPILOGUE, NULL);
	rtai_catch_event(hal_root_domain, HAL_EXIT_PROCESS, NULL);
	rtai_catch_event(hal_root_domain, HAL_KICK_PROCESS, NULL);
	rtai_lxrt_dispatcher = NULL;
    
	flags = rtai_critical_enter(NULL);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,32)
	do {
		struct mmreq *p;
/* Flush the MM log for all processors */
		for (p = lxrt_mmrqtab; p < lxrt_mmrqtab + NR_CPUS; p++) {
			while (p->out != p->in) {
				struct mm_struct *oldmm = p->mm[p->out];
				mmdrop(oldmm);
				bump_mmreq(p->out);
				p->count--;
			}
		}
	} while (0);
#endif  /* KERNEL_VERSION < 2.4.32 */
	rtai_critical_exit(flags);

	reset_rt_fun_entries(rt_sched_entries);
}

#ifdef DECLR_8254_TSC_EMULATION
DECLR_8254_TSC_EMULATION;

static void timer_fun(unsigned long none)
{
	TICK_8254_TSC_EMULATION();
	timer.expires = jiffies + (HZ + TSC_EMULATION_GUARD_FREQ/2 - 1)/TSC_EMULATION_GUARD_FREQ;
	add_timer(&timer);
}
#endif

extern int rt_registry_alloc(void);
extern void rt_registry_free(void);

static int __rtai_lxrt_init(void)
{
	int cpuid, retval;
	
#ifdef CONFIG_RTAI_MALLOC
	rtai_kstack_heap_size = (rtai_kstack_heap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (rtheap_init(&rtai_kstack_heap, NULL, rtai_kstack_heap_size, PAGE_SIZE, GFP_KERNEL)) {
		printk(KERN_INFO "RTAI[malloc]: failed to initialize the kernel stacks heap (size=%d bytes).\n", rtai_kstack_heap_size);
		return 1;
	}
#endif
	sched_mem_init();

	rt_registry_alloc();

	for (cpuid = 0; cpuid < NR_RT_CPUS; cpuid++) {
		rt_linux_task.uses_fpu = 1;
		rt_linux_task.magic = 0;
		rt_linux_task.policy = rt_linux_task.is_hard = 0;
		rt_linux_task.runnable_on_cpus = cpuid;
		rt_linux_task.state = RT_SCHED_READY;
		rt_linux_task.msg_queue.prev = &(rt_linux_task.msg_queue);      
		rt_linux_task.msg_queue.next = &(rt_linux_task.msg_queue);      
		rt_linux_task.msg_queue.task = &rt_linux_task;    
		rt_linux_task.msg = 0;  
		rt_linux_task.ret_queue.prev = &(rt_linux_task.ret_queue);
		rt_linux_task.ret_queue.next = &(rt_linux_task.ret_queue);
		rt_linux_task.ret_queue.task = NULL;
		rt_linux_task.priority = RT_SCHED_LINUX_PRIORITY;
		rt_linux_task.base_priority = RT_SCHED_LINUX_PRIORITY;
		rt_linux_task.signal = 0;
		rt_linux_task.prev = &rt_linux_task;
                rt_linux_task.resume_time = RT_TIME_END;
                rt_linux_task.periodic_resume_time = RT_TIME_END;
                rt_linux_task.tprev = rt_linux_task.tnext =
                rt_linux_task.rprev = rt_linux_task.rnext = &rt_linux_task;
#ifdef CONFIG_RTAI_LONG_TIMED_LIST
		rt_linux_task.rbr.rb_node = NULL;
#endif
		rt_linux_task.next = 0;
		rt_linux_task.lnxtsk = current;
		rt_smp_current[cpuid] = &rt_linux_task;
		rt_smp_fpu_task[cpuid] = &rt_linux_task;
		oneshot_timer = OneShot ? 1 : 0;
		oneshot_running = 0;
		linux_cr0 = 0;
		rt_linux_task.resq.prev = rt_linux_task.resq.next = &rt_linux_task.resq;
		rt_linux_task.resq.task = NULL;
	}
	tuned.latency = imuldiv(Latency, tuned.cpu_freq, 1000000000);
	tuned.setup_time_TIMER_CPUNIT = imuldiv( SetupTimeTIMER, 
						 tuned.cpu_freq, 
						 1000000000);
	tuned.setup_time_TIMER_UNIT   = imuldiv( SetupTimeTIMER, 
						 TIMER_FREQ, 
						 1000000000);
	tuned.timers_tol[0] = 0;
#ifdef CONFIG_PROC_FS
	if (rtai_proc_sched_register()) {
		retval = 1;
		goto mem_end;
	}
#endif

// 0x7dd763ad == nam2num("MEMSRQ").
	if ((frstk_srq.srq = rt_request_srq(0x7dd763ad, frstk_srq_handler, 0)) < 0) {
		printk("MEM SRQ: no sysrq available.\n");
		retval = frstk_srq.srq;
		goto proc_unregister;
	}

	frstk_srq.in = frstk_srq.out = 0;
	if ((retval = rt_request_sched_ipi()) != 0)
		goto free_srq;

	if ((retval = lxrt_init()) != 0)
		goto free_sched_ipi;

#ifdef CONFIG_RTAI_SCHED_ISR_LOCK
	rt_set_ihook(&rtai_handle_isched_lock);
#endif /* CONFIG_RTAI_SCHED_ISR_LOCK */

	register_reboot_notifier(&lxrt_notifier_reboot);
#ifdef CONFIG_SMP
	printk(KERN_INFO "RTAI[sched]: IMMEDIATE, MP, USER/KERNEL SPACE: <%s RTAI OWN KTASKs>", USE_RTAI_TASKS ? "with" : "without");
#else
	printk(KERN_INFO "RTAI[sched]: loaded (IMMEDIATE, UP, USER/KERNEL SPACE: <%s RTAI OWN KTASKs>", USE_RTAI_TASKS ? "with" : "without");
#endif
#ifdef CONFIG_RTAI_LXRT_USE_LINUX_SYSCALL
	printk(", <uses LINUX SYSCALLs>");
#endif
	printk(", kstacks pool size = %d bytes", rtai_kstack_heap_size);
	printk(".\n");
	printk(KERN_INFO "RTAI[sched]: hard timer type/freq = %s/%d(Hz); default timing: %s; ", TIMER_NAME, (int)TIMER_FREQ, OneShot ? "oneshot" : "periodic");
#ifdef CONFIG_RTAI_LONG_TIMED_LIST
	printk("black/red timed lists.\n");
#else
	printk("linear timed lists.\n");
#endif
	printk(KERN_INFO "RTAI[sched]: Linux timer freq = %d (Hz), CPU freq = %lu hz.\n", HZ, (unsigned long)tuned.cpu_freq);
	printk(KERN_INFO "RTAI[sched]: timer setup = %d ns, resched latency = %d ns.\n", (int)imuldiv(tuned.setup_time_TIMER_CPUNIT, 1000000000, tuned.cpu_freq), (int)imuldiv(tuned.latency - tuned.setup_time_TIMER_CPUNIT, 1000000000, tuned.cpu_freq));

#ifdef DECLR_8254_TSC_EMULATION
	SETUP_8254_TSC_EMULATION;
#endif

	retval = rtai_init_features(); /* see rtai_schedcore.h */

exit:
#ifdef CONFIG_GENERIC_CLOCKEVENTS
	rt_linux_hrt_set_mode  = _rt_linux_hrt_set_mode;
	rt_linux_hrt_next_shot = _rt_linux_hrt_next_shot;
#endif
	return retval;
free_sched_ipi:
	rt_free_sched_ipi();
free_srq:
	rt_free_srq(frstk_srq.srq);
proc_unregister:
#ifdef CONFIG_PROC_FS
	rtai_proc_sched_unregister();
#endif
mem_end:
	sched_mem_end();
#ifdef CONFIG_RTAI_MALLOC
	rtheap_destroy(&rtai_kstack_heap, GFP_KERNEL);
#endif
	rt_registry_free();
	goto exit;
}

static void __rtai_lxrt_exit(void)
{
	unregister_reboot_notifier(&lxrt_notifier_reboot);

#ifdef CONFIG_GENERIC_CLOCKEVENTS
	rt_linux_hrt_set_mode  = NULL;
	rt_linux_hrt_next_shot = NULL;
#endif

	lxrt_killall();

	krtai_objects_release();

	lxrt_exit();

	rtai_cleanup_features();

#ifdef CONFIG_PROC_FS
        rtai_proc_sched_unregister();
#endif
	while (frstk_srq.out != frstk_srq.in);
	if (rt_free_srq(frstk_srq.srq) < 0) {
		printk("MEM SRQ: frstk_srq %d illegal or already free.\n", frstk_srq.srq);
	}
	rt_free_sched_ipi();
	sched_mem_end();
#ifdef CONFIG_RTAI_MALLOC
	rtheap_destroy(&rtai_kstack_heap, GFP_KERNEL);
#endif
	rt_registry_free();
	current->state = TASK_INTERRUPTIBLE;
	schedule_timeout(HZ/10);
#ifdef CONFIG_RTAI_SCHED_ISR_LOCK
	rt_set_ihook(NULL);
#endif /* CONFIG_RTAI_SCHED_ISR_LOCK */

#ifdef DECLR_8254_TSC_EMULATION
	CLEAR_8254_TSC_EMULATION;
#endif

	printk(KERN_INFO "RTAI[sched]: unloaded (forced hard/soft/hard transitions: traps %lu, syscalls %lu).\n", traptrans, systrans);
}

module_init(__rtai_lxrt_init);
module_exit(__rtai_lxrt_exit);

#ifndef CONFIG_KBUILD
#define CONFIG_KBUILD
#endif

#ifdef CONFIG_KBUILD

EXPORT_SYMBOL(rt_fun_lxrt);
EXPORT_SYMBOL(clr_rtext);
EXPORT_SYMBOL(set_rtext);
EXPORT_SYMBOL(get_min_tasks_cpuid);
EXPORT_SYMBOL(put_current_on_cpu);
EXPORT_SYMBOL(rt_schedule_soft);
EXPORT_SYMBOL(rt_do_force_soft);
EXPORT_SYMBOL(rt_schedule_soft_tail);
EXPORT_SYMBOL(rt_sched_timed);
EXPORT_SYMBOL(rtai_handle_isched_lock);
#if CONFIG_RTAI_MONITOR_EXECTIME
EXPORT_SYMBOL(switch_time);
#endif

#endif /* CONFIG_KBUILD */
