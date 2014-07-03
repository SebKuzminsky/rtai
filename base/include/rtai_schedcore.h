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


#ifndef _RTAI_SCHEDCORE_H
#define _RTAI_SCHEDCORE_H

#include <rtai_version.h>
#include <rtai_lxrt.h>
#include <rtai_sched.h>
#include <rtai_malloc.h>
#include <rtai_trace.h>
#include <rtai_leds.h>
#include <rtai_sem.h>
#include <rtai_rwl.h>
#include <rtai_spl.h>
#include <rtai_scb.h>
#include <rtai_mbx.h>
#include <rtai_msg.h>
#include <rtai_tbx.h>
#include <rtai_mq.h>
#include <rtai_bits.h>
#include <rtai_wd.h>
#include <rtai_tasklets.h>
#include <rtai_fifos.h>
#include <rtai_netrpc.h>
#include <rtai_shm.h>
#include <rtai_usi.h>

#ifdef __KERNEL__

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/timex.h>
#include <linux/sched.h>
#include <asm/param.h>
#include <asm/system.h>
#include <asm/io.h>


#ifndef _RTAI_SCHED_XN_H
#define _RTAI_SCHED_XN_H

#if defined(CONFIG_RTAI_IMMEDIATE_LINUX_SYSCALL) && CONFIG_RTAI_IMMEDIATE_LINUX_SYSCALL
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
#define SKIP_IMMEDIATE_LINUX_SYSCALL() \
	if (regs->LINUX_SYSCALL_NR == __NR_kill || regs->LINUX_SYSCALL_NR == __NR_rt_sigsuspend) { return 0; }
#else
#define SKIP_IMMEDIATE_LINUX_SYSCALL()
#endif
#else
#define SKIP_IMMEDIATE_LINUX_SYSCALL()  do { return 0; } while (0)
#endif

#ifdef RTAI_TRIOSS

extern int nkgkptd;
#define FUSIONEXT  (nkgkptd)

// provisional, to be replaced by appropriate headers declarations
#define XNSUSP   (0x00000001)
#define XNRELAX  (0x00000100)

typedef struct xnarchtcb {
    union i387_union fpuenv __attribute__ ((aligned (16)));
    unsigned stacksize;
    unsigned long *stackbase;
    unsigned long esp; 
    unsigned long eip;
    struct task_struct *user_task;
    struct task_struct *active_task;
    unsigned long *espp; 
    unsigned long *eipp;
    union i387_union *fpup; 
} xnarchtcb_t;

typedef struct xnthread { xnarchtcb_t tcb; unsigned long status; } xnthread_t;

extern void xnpod_resume_thread(void *, unsigned long);
extern void xnpod_schedule(void);
// end of provisional

extern struct hal_domain_struct *fusion_domain;

extern struct klist_t fusion_wake_up_srq[];

#define NON_RTAI_TASK_SUSPEND(task) \
do { \
	xnthread_t *thread; \
	if ((thread = (task->lnxtsk)->rtai_tskext(FUSIONEXT)) && !(thread->status & XNRELAX)) { \
		atomic_set_mask(XNSUSP, (atomic_t *)&thread->status); \
	} else { \
		(task->lnxtsk)->state = TASK_SOFTREALTIME; \
	} \
} while (0)

#define pend_fusion_wake_up_srq(lnxtsk, cpuid) \
do { \
	fusion_wake_up_srq[cpuid].task[fusion_wake_up_srq[cpuid].in++ & (MAX_WAKEUP_SRQ - 1)] = lnxtsk; \
	hal_pend_domain_uncond(fusion_wake_up_srq[cpuid].srq, fusion_domain, cpuid); \
} while (0)

#define NON_RTAI_TASK_RESUME(ready_task) \
do { \
	xnthread_t *thread; \
	if ((thread = (ready_task->lnxtsk)->rtai_tskext(FUSIONEXT)) && !(thread->status & XNRELAX)) { \
		pend_fusion_wake_up_srq(ready_task->lnxtsk, rtai_cpuid()); \
	} else { \
               	pend_wake_up_srq(ready_task->lnxtsk, rtai_cpuid()); \
	} \
} while (0)

#define DECLARE_FUSION_WAKE_UP_STUFF \
struct klist_t fusion_wake_up_srq[MAX_WAKEUP_SRQ]; \
static void fusion_wake_up_srq_handler(unsigned srq) \
{ \
	int cpuid = srq - fusion_wake_up_srq[0].srq; \
	while (fusion_wake_up_srq[cpuid].out != fusion_wake_up_srq[cpuid].in) { \
		xnpod_resume_thread(((struct task_struct *)fusion_wake_up_srq[cpuid].task[fusion_wake_up_srq[cpuid].out++ & (MAX_WAKEUP_SRQ - 1)])->rtai_tskext(FUSIONEXT), XNSUSP); \
	} \
	xnpod_schedule(); \
} \
EXPORT_SYMBOL(fusion_wake_up_srq);

#define REQUEST_RESUME_SRQs_STUFF() \
do { \
	int cpuid; \
	for (cpuid = 0; cpuid < num_online_cpus(); cpuid++) { \
        	hal_virtualize_irq(hal_root_domain, wake_up_srq[cpuid].srq = hal_alloc_irq(), wake_up_srq_handler, NULL, IPIPE_HANDLE_FLAG); \
        	hal_virtualize_irq(fusion_domain, fusion_wake_up_srq[cpuid].srq = hal_alloc_irq(), fusion_wake_up_srq_handler, NULL, IPIPE_HANDLE_FLAG); \
	} \
} while (0)

#define RELEASE_RESUME_SRQs_STUFF() \
do { \
	int cpuid; \
	for (cpuid = 0; cpuid < num_online_cpus(); cpuid++) { \
		hal_virtualize_irq(hal_root_domain, wake_up_srq[cpuid].srq, NULL, NULL, 0); \
		hal_free_irq(wake_up_srq[cpuid].srq); \
		hal_virtualize_irq(fusion_domain, fusion_wake_up_srq[cpuid].srq, NULL, NULL, 0); \
		hal_free_irq(fusion_wake_up_srq[cpuid].srq); \
	} \
} while (0)

#else /* !RTAI_TRIOSS */

#define FUSIONEXT  (0)

#define DECLARE_FUSION_WAKE_UP_STUFF

#define NON_RTAI_TASK_SUSPEND(task) \
	do { (task->lnxtsk)->state = TASK_SOFTREALTIME; } while (0)

#define NON_RTAI_TASK_RESUME(ready_task) \
	do { pend_wake_up_srq(ready_task->lnxtsk, rtai_cpuid()); } while (0)

#define REQUEST_RESUME_SRQs_STUFF() \
do { \
	int cpuid; \
	for (cpuid = 0; cpuid < num_online_cpus(); cpuid++) { \
        	hal_virtualize_irq(hal_root_domain, wake_up_srq[cpuid].srq = hal_alloc_irq(), wake_up_srq_handler, NULL, IPIPE_HANDLE_FLAG); \
		if ( wake_up_srq[cpuid].srq != (wake_up_srq[0].srq + cpuid)) { \
			int i; \
			for (i = 0; i <= cpuid; i++) { \
				hal_virtualize_irq(hal_root_domain, wake_up_srq[i].srq, NULL, NULL, 0); \
				hal_free_irq(wake_up_srq[i].srq); \
			} \
			printk("*** NON CONSECUTIVE WAKE UP SRQs, ABORTING ***\n"); \
			return -1; \
		} \
	} \
} while (0)

#define RELEASE_RESUME_SRQs_STUFF() \
do { \
	int cpuid; \
	for (cpuid = 0; cpuid < num_online_cpus(); cpuid++) { \
		hal_virtualize_irq(hal_root_domain, wake_up_srq[cpuid].srq, NULL, NULL, 0); \
		hal_free_irq(wake_up_srq[cpuid].srq); \
	} \
} while (0)

#endif /* END RTAI_TRIOSS */

#endif /* !_RTAI_SCHED_XN_H */


extern RT_TASK rt_smp_linux_task[];

extern RT_TASK *rt_smp_current[];

extern RTIME rt_smp_time_h[];

extern int rt_smp_oneshot_timer[];

extern volatile int rt_sched_timed;

#ifdef CONFIG_RTAI_MALLOC
#ifdef CONFIG_RTAI_MALLOC_BUILTIN
#define sched_mem_init() \
	{ if(__rtai_heap_init() != 0) { \
                return(-ENOMEM); \
        } }
#define sched_mem_end()	 __rtai_heap_exit()
#else  /* CONFIG_RTAI_MALLOC_BUILTIN */
#define sched_mem_init()
#define sched_mem_end()
#endif /* !CONFIG_RTAI_MALLOC_BUILTIN */
#define call_exit_handlers(task)	        __call_exit_handlers(task)
#define set_exit_handler(task, fun, arg1, arg2)	__set_exit_handler(task, fun, arg1, arg2)
#else  /* !CONFIG_RTAI_MALLOC */
#define sched_mem_init()
#define sched_mem_end()
#define call_exit_handlers(task)
#define set_exit_handler(task, fun, arg1, arg2)
#endif /* CONFIG_RTAI_MALLOC */

#define SEMHLF 0x0000FFFF
#define RPCHLF 0xFFFF0000
#define RPCINC 0x00010000

#define DECLARE_RT_CURRENT int cpuid; RT_TASK *rt_current
#define ASSIGN_RT_CURRENT rt_current = rt_smp_current[cpuid = rtai_cpuid()]
#define RT_CURRENT rt_smp_current[rtai_cpuid()]

#define MAX_LINUX_RTPRIO  99
#define MIN_LINUX_RTPRIO   1

#ifdef CONFIG_RTAI_SCHED_ISR_LOCK
void rtai_handle_isched_lock(int nesting);
#endif /* CONFIG_RTAI_SCHED_ISR_LOCK */

#ifdef CONFIG_SMP
#define rt_time_h (rt_smp_time_h[cpuid])
#define oneshot_timer (rt_smp_oneshot_timer[cpuid])
#define rt_linux_task (rt_smp_linux_task[cpuid])
#else
#define rt_time_h (rt_smp_time_h[0])
#define oneshot_timer (rt_smp_oneshot_timer[0])
#define rt_linux_task (rt_smp_linux_task[0])
#endif

/*
 * WATCH OUT for the max expected number of arguments of rtai funs and 
 * their scattered around different calling ways.
 */

#define RTAI_MAX_FUN_ARGS  9
struct fun_args { unsigned long a[RTAI_MAX_FUN_ARGS]; RTAI_SYSCALL_MODE long long (*fun)(unsigned long, ...); };
//used in sys.c
#define RTAI_FUN_ARGS  arg[0],arg[1],arg[2],arg[3],arg[4],arg[5],arg[6],arg[7],arg[RTAI_MAX_FUN_ARGS - 1]
//used in sched.c and netrpc.c (generalised calls from soft threads)
#define RTAI_FUNARGS   funarg->a[0],funarg->a[1],funarg->a[2],funarg->a[3],funarg->a[4],funarg->a[5],funarg->a[6],funarg->a[7],funarg->a[RTAI_MAX_FUN_ARGS - 1]
//used in netrpc.c
#define RTAI_FUN_A     a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[RTAI_MAX_FUN_ARGS - 1]

#ifdef CONFIG_SMP

static inline void send_sched_ipi(unsigned long dest)
{
	_send_sched_ipi(dest);
}

#define RT_SCHEDULE_MAP(schedmap) \
	do { if (schedmap) send_sched_ipi(schedmap); } while (0)

#define RT_SCHEDULE_MAP_BOTH(schedmap) \
	do { if (schedmap) send_sched_ipi(schedmap); rt_schedule(); } while (0)

#define RT_SCHEDULE(task, cpuid) \
	do { \
		if ((task)->runnable_on_cpus != (cpuid)) { \
			send_sched_ipi(1 << (task)->runnable_on_cpus); \
		} else { \
			rt_schedule(); \
		} \
	} while (0)

#define RT_SCHEDULE_BOTH(task, cpuid) \
	{ \
		if ((task)->runnable_on_cpus != (cpuid)) { \
			send_sched_ipi(1 << (task)->runnable_on_cpus); \
		} \
		rt_schedule(); \
	}

#else /* !CONFIG_SMP */

#define send_sched_ipi(dest)

#define RT_SCHEDULE_MAP_BOTH(schedmap)  rt_schedule()

#define RT_SCHEDULE_MAP(schedmap)       rt_schedule()

#define RT_SCHEDULE(task, cpuid)        rt_schedule()

#define RT_SCHEDULE_BOTH(task, cpuid)   rt_schedule()

#endif /* CONFIG_SMP */

#define BASE_SOFT_PRIORITY 1000000000

#define TASK_HARDREALTIME  TASK_UNINTERRUPTIBLE
#define TASK_SOFTREALTIME  TASK_INTERRUPTIBLE

static inline void enq_ready_edf_task(RT_TASK *ready_task)
{
	RT_TASK *task;
#ifdef CONFIG_SMP
	task = rt_smp_linux_task[ready_task->runnable_on_cpus].rnext;
#else
	task = rt_smp_linux_task[0].rnext;
#endif
	while (task->policy < 0 && ready_task->period >= task->period) {
		task = task->rnext;
	}
	task->rprev = (ready_task->rprev = task->rprev)->rnext = ready_task;
	ready_task->rnext = task;
}

struct epoch_struct { spinlock_t lock; volatile int touse; volatile RTIME time[2][2]; };

#ifdef CONFIG_RTAI_CLOCK_REALTIME
#define REALTIME2COUNT(rtime) \
	if (rtime > boot_epoch.time[boot_epoch.touse][0]) { \
		rtime -= boot_epoch.time[boot_epoch.touse][0]; \
	}
#else 
#define REALTIME2COUNT(rtime)
#endif

#define MAX_WAKEUP_SRQ (2 << 6)

struct klist_t { int srq; volatile unsigned long in, out; void *task[MAX_WAKEUP_SRQ]; };
extern struct klist_t wake_up_srq[];

#define pend_wake_up_srq(lnxtsk, cpuid) \
do { \
	wake_up_srq[cpuid].task[wake_up_srq[cpuid].in++ & (MAX_WAKEUP_SRQ - 1)] = lnxtsk; \
	hal_pend_uncond(wake_up_srq[cpuid].srq, cpuid); \
} while (0)

static inline void enq_ready_task(RT_TASK *ready_task)
{
	RT_TASK *task;
	if (ready_task->is_hard) {
#ifdef CONFIG_SMP
		task = rt_smp_linux_task[ready_task->runnable_on_cpus].rnext;
#else
		task = rt_smp_linux_task[0].rnext;
#endif
		while (ready_task->priority >= task->priority) {
			if ((task = task->rnext)->priority < 0) break;
		}
		task->rprev = (ready_task->rprev = task->rprev)->rnext = ready_task;
		ready_task->rnext = task;
	} else {
		ready_task->state |= RT_SCHED_SFTRDY;
		NON_RTAI_TASK_RESUME(ready_task);
	}
}

static inline int renq_ready_task(RT_TASK *ready_task, int priority)
{
	int retval;
	if ((retval = ready_task->priority != priority)) {
		ready_task->priority = priority;
		if (ready_task->state == RT_SCHED_READY) {
			(ready_task->rprev)->rnext = ready_task->rnext;
			(ready_task->rnext)->rprev = ready_task->rprev;
			enq_ready_task(ready_task);
		}
	}
	return retval;
}

static inline int renq_current(RT_TASK *rt_current, int priority)
{
	int retval;
	if ((retval = rt_current->priority != priority)) {
		rt_current->priority = priority;
		(rt_current->rprev)->rnext = rt_current->rnext;
		(rt_current->rnext)->rprev = rt_current->rprev;
		enq_ready_task(rt_current);
	}
	return retval;
}

static inline void rem_ready_task(RT_TASK *task)
{
	if (task->state == RT_SCHED_READY) {
		if (!task->is_hard) {
			NON_RTAI_TASK_SUSPEND(task);
		}
//		task->unblocked = 0;
		(task->rprev)->rnext = task->rnext;
		(task->rnext)->rprev = task->rprev;
	}
}

static inline void rem_ready_current(RT_TASK *rt_current)
{
	if (!rt_current->is_hard) {
		NON_RTAI_TASK_SUSPEND(rt_current);
	}
//	rt_current->unblocked = 0;
	(rt_current->rprev)->rnext = rt_current->rnext;
	(rt_current->rnext)->rprev = rt_current->rprev;
}

#ifdef CONFIG_RTAI_LONG_TIMED_LIST

/* BINARY TREE */
static inline void enq_timed_task(RT_TASK *timed_task)
{
	RT_TASK *taskh, *tsknxt, *task;
	rb_node_t **rbtn, *rbtpn = NULL;
#ifdef CONFIG_SMP
	task = taskh = &rt_smp_linux_task[timed_task->runnable_on_cpus];
#else
	task = taskh = &rt_smp_linux_task[0];
#endif
	rbtn = &taskh->rbr.rb_node;

	while (*rbtn) {
		rbtpn = *rbtn;
		tsknxt = rb_entry(rbtpn, RT_TASK, rbn);
		if (timed_task->resume_time > tsknxt->resume_time) {
			rbtn = &(rbtpn)->rb_right;
		} else {
			rbtn = &(rbtpn)->rb_left;
			task = tsknxt;
		}
	}
	rb_link_node(&timed_task->rbn, rbtpn, rbtn);
	rb_insert_color(&timed_task->rbn, &taskh->rbr);
	task->tprev = (timed_task->tprev = task->tprev)->tnext = timed_task;
	timed_task->tnext = task;
}

#define	rb_erase_task(task, cpuid) \
	rb_erase(&(task)->rbn, &rt_smp_linux_task[cpuid].rbr);

#else /* !CONFIG_RTAI_LONG_TIMED_LIST */

/* LINEAR */
static inline void enq_timed_task(RT_TASK *timed_task)
{
	RT_TASK *task;
#ifdef CONFIG_SMP
	task = rt_smp_linux_task[timed_task->runnable_on_cpus].tnext;
#else
	task = rt_smp_linux_task[0].tnext;
#endif
	while (timed_task->resume_time > task->resume_time) {
		task = task->tnext;
	}
	task->tprev = (timed_task->tprev = task->tprev)->tnext = timed_task;
	timed_task->tnext = task;
}

#define	rb_erase_task(task, cpuid)

#endif /* !CONFIG_RTAI_LONG_TIMED_LIST */

static inline void rem_timed_task(RT_TASK *task)
{
	if ((task->state & RT_SCHED_DELAYED)) {
                (task->tprev)->tnext = task->tnext;
                (task->tnext)->tprev = task->tprev;
#ifdef CONFIG_SMP
		rb_erase_task(task, task->runnable_on_cpus);
#else
		rb_erase_task(task, 0);
#endif
	}
}

static inline void wake_up_timed_tasks(int cpuid)
{
	RT_TASK *taskh, *task;
#ifdef CONFIG_SMP
	task = (taskh = &rt_smp_linux_task[cpuid])->tnext;
#else
	task = (taskh = &rt_smp_linux_task[0])->tnext;
#endif
	if (task->resume_time <= rt_time_h) {
		do {
        	        if ((task->state &= ~(RT_SCHED_DELAYED | RT_SCHED_SUSPENDED | RT_SCHED_SEMAPHORE | RT_SCHED_RECEIVE | RT_SCHED_SEND | RT_SCHED_RPC | RT_SCHED_RETURN | RT_SCHED_MBXSUSP)) == RT_SCHED_READY) {
                	        if (task->policy < 0) {
                        	        enq_ready_edf_task(task);
	                        } else {
        	                        enq_ready_task(task);
                	        }
#if defined(CONFIG_RTAI_BUSY_TIME_ALIGN) && CONFIG_RTAI_BUSY_TIME_ALIGN
	                        task->trap_handler_data = (void *)oneshot_timer;
#endif
        	        }
			rb_erase_task(task, cpuid);
			task = task->tnext;
		} while (task->resume_time <= rt_time_h);
#ifdef CONFIG_SMP
		rt_smp_linux_task[cpuid].tnext = task;
		task->tprev = &rt_smp_linux_task[cpuid];
#else
		rt_smp_linux_task[0].tnext = task;
		task->tprev = &rt_smp_linux_task[0];
#endif
	}
}

#define get_time() rt_get_time()
#if 0
static inline RTIME get_time(void)
{
#ifdef CONFIG_SMP
	int cpuid;
	return rt_smp_oneshot_timer[cpuid = rtai_cpuid()] ? rdtsc() : rt_smp_times[cpuid].tick_time;
#else
	return rt_smp_oneshot_timer[0] ? rdtsc() : rt_smp_times[0].tick_time;
#endif
}
#endif

static inline void enqueue_blocked(RT_TASK *task, QUEUE *queue, int qtype)
{
        QUEUE *q;
        task->blocked_on = (q = queue);
        if (!qtype) {
                while ((q = q->next) != queue && (q->task)->priority <= task->priority);
        }
        q->prev = (task->queue.prev = q->prev)->next  = &(task->queue);
        task->queue.next = q;
}


static inline void dequeue_blocked(RT_TASK *task)
{
        task->prio_passed_to     = NULL;
        (task->queue.prev)->next = task->queue.next;
        (task->queue.next)->prev = task->queue.prev;
        task->blocked_on         = NULL;
}

static inline unsigned long pass_prio(RT_TASK *to, RT_TASK *from)
{
        QUEUE *q, *blocked_on;
#ifdef CONFIG_SMP
        unsigned long schedmap;
        schedmap = 0;
#endif
//	from->prio_passed_to = to;
        while (to && to->priority > from->priority) {
                to->priority = from->priority;
		if (to->state == RT_SCHED_READY) {
			if ((to->rprev)->priority > to->priority || (to->rnext)->priority < to->priority) {
				(to->rprev)->rnext = to->rnext;
				(to->rnext)->rprev = to->rprev;
				enq_ready_task(to);
#ifdef CONFIG_SMP
				if (to == rt_smp_linux_task[to->runnable_on_cpus].rnext) {
					__set_bit(to->runnable_on_cpus & 0x1F, &schedmap);
				}
#endif
			}
			break;
//		} else if ((void *)(q = to->blocked_on) > RTE_HIGERR && !((to->state & RT_SCHED_SEMAPHORE) && ((SEM *)q)->qtype)) {
		} else if ((unsigned long)(blocked_on = to->blocked_on) > RTE_HIGERR && (((to->state & RT_SCHED_SEMAPHORE) && ((SEM *)blocked_on)->type > 0) || (to->state & (RT_SCHED_SEND | RT_SCHED_RPC | RT_SCHED_RETURN)))) {
			if (to->queue.prev != blocked_on) {
				q = blocked_on;
				(to->queue.prev)->next = to->queue.next;
				(to->queue.next)->prev = to->queue.prev;
				while ((q = q->next) != blocked_on && (q->task)->priority <= to->priority);
				q->prev = (to->queue.prev = q->prev)->next  = &(to->queue);
				to->queue.next = q;
				if (to->queue.prev != blocked_on) {
					break;
				}
			}
			to = (to->state & RT_SCHED_SEMAPHORE) ? ((SEM *)blocked_on)->owndby : blocked_on->task;
                }
//		to = to->prio_passed_to;
	}
#ifdef CONFIG_SMP
	return schedmap;
#else
	return 0;
#endif
}

static inline RT_TASK *_rt_whoami(void)
{
#ifdef CONFIG_SMP
        RT_TASK *rt_current;
        unsigned long flags;
        flags = rt_global_save_flags_and_cli();
        rt_current = RT_CURRENT;
        rt_global_restore_flags(flags);
        return rt_current;
#else
        return rt_smp_current[0];
#endif
}

static inline void __call_exit_handlers(RT_TASK *task)
{
	XHDL *pt, *tmp;

	pt = task->ExitHook; // Initialise ExitHook in rt_task_init()
	while ( pt ) {
		(*pt->fun) (pt->arg1, pt->arg2);
		tmp = pt;
		pt  = pt->nxt;
		rt_free(tmp);
	}
	task->ExitHook = 0;
}

static inline XHDL *__set_exit_handler(RT_TASK *task, void (*fun) (void *, int), void *arg1, int arg2)
{
	XHDL *p;

	// exit handler functions are automatically executed at terminattion time by rt_task_delete()
	// in the reverse order they were created (like C++ destructors behave).
	if (task->magic != RT_TASK_MAGIC) return 0;
	if (!(p = (XHDL *) rt_malloc (sizeof(XHDL)))) return 0;
	p->fun  = fun;
	p->arg1 = arg1;
	p->arg2 = arg2;
	p->nxt  = task->ExitHook;
	return (task->ExitHook = p);
}

static inline int rtai_init_features (void)

{
#ifdef CONFIG_RTAI_LEDS_BUILTIN
    __rtai_leds_init();
#endif /* CONFIG_RTAI_LEDS_BUILTIN */
#ifdef CONFIG_RTAI_SEM_BUILTIN
    __rtai_sem_init();
#endif /* CONFIG_RTAI_SEM_BUILTIN */
#ifdef CONFIG_RTAI_MSG_BUILTIN
    __rtai_msg_init();
#endif /* CONFIG_RTAI_MSG_BUILTIN */
#ifdef CONFIG_RTAI_MBX_BUILTIN
    __rtai_mbx_init();
#endif /* CONFIG_RTAI_MBX_BUILTIN */
#ifdef CONFIG_RTAI_TBX_BUILTIN
    __rtai_msg_queue_init();
#endif /* CONFIG_RTAI_TBX_BUILTIN */
#ifdef CONFIG_RTAI_MQ_BUILTIN
    __rtai_mq_init();
#endif /* CONFIG_RTAI_MQ_BUILTIN */
#ifdef CONFIG_RTAI_BITS_BUILTIN
    __rtai_bits_init();
#endif /* CONFIG_RTAI_BITS_BUILTIN */
#ifdef CONFIG_RTAI_TASKLETS_BUILTIN
    __rtai_tasklets_init();
#endif /* CONFIG_RTAI_TASKLETS_BUILTIN */
#ifdef CONFIG_RTAI_FIFOS_BUILTIN
    __rtai_fifos_init();
#endif /* CONFIG_RTAI_FIFOS_BUILTIN */
#ifdef CONFIG_RTAI_NETRPC_BUILTIN
    __rtai_netrpc_init();
#endif /* CONFIG_RTAI_NETRPC_BUILTIN */
#ifdef CONFIG_RTAI_SHM_BUILTIN
    __rtai_shm_init();
#endif /* CONFIG_RTAI_SHM_BUILTIN */
#ifdef CONFIG_RTAI_USI_BUILTIN
    __rtai_usi_init();
#endif /* CONFIG_RTAI_USI_BUILTIN */
#ifdef CONFIG_RTAI_MATH_BUILTIN
    __rtai_math_init();
#endif /* CONFIG_RTAI_MATH_BUILTIN */

	return 0;
}

static inline void rtai_cleanup_features (void) {

#ifdef CONFIG_RTAI_MATH_BUILTIN
    __rtai_math_exit();
#endif /* CONFIG_RTAI_MATH_BUILTIN */
#ifdef CONFIG_RTAI_USI_BUILTIN
    __rtai_usi_exit();
#endif /* CONFIG_RTAI_USI_BUILTIN */
#ifdef CONFIG_RTAI_SHM_BUILTIN
    __rtai_shm_exit();
#endif /* CONFIG_RTAI_SHM_BUILTIN */
#ifdef CONFIG_RTAI_NETRPC_BUILTIN
    __rtai_netrpc_exit();
#endif /* CONFIG_RTAI_NETRPC_BUILTIN */
#ifdef CONFIG_RTAI_FIFOS_BUILTIN
    __rtai_fifos_exit();
#endif /* CONFIG_RTAI_FIFOS_BUILTIN */
#ifdef CONFIG_RTAI_TASKLETS_BUILTIN
    __rtai_tasklets_exit();
#endif /* CONFIG_RTAI_TASKLETS_BUILTIN */
#ifdef CONFIG_RTAI_BITS_BUILTIN
    __rtai_bits_exit();
#endif /* CONFIG_RTAI_BITS_BUILTIN */
#ifdef CONFIG_RTAI_MQ_BUILTIN
    __rtai_mq_exit();
#endif /* CONFIG_RTAI_MQ_BUILTIN */
#ifdef CONFIG_RTAI_TBX_BUILTIN
    __rtai_msg_queue_exit();
#endif /* CONFIG_RTAI_TBX_BUILTIN */
#ifdef CONFIG_RTAI_MBX_BUILTIN
    __rtai_mbx_exit();
#endif /* CONFIG_RTAI_MBX_BUILTIN */
#ifdef CONFIG_RTAI_MSG_BUILTIN
    __rtai_msg_exit();
#endif /* CONFIG_RTAI_MSG_BUILTIN */
#ifdef CONFIG_RTAI_SEM_BUILTIN
    __rtai_sem_exit();
#endif /* CONFIG_RTAI_SEM_BUILTIN */
#ifdef CONFIG_RTAI_LEDS_BUILTIN
    __rtai_leds_exit();
#endif /* CONFIG_RTAI_LEDS_BUILTIN */
}

int rt_check_current_stack(void);

int rt_kthread_init(RT_TASK *task,
		    void (*rt_thread)(long),
		    long data,
		    int stack_size,
		    int priority,
		    int uses_fpu,
		    void(*signal)(void));

int rt_kthread_init_cpuid(RT_TASK *task,
			  void (*rt_thread)(long),
			  long data,
			  int stack_size,
			  int priority,
			  int uses_fpu,
			  void(*signal)(void),
			  unsigned int cpuid);

#endif /* __KERNEL__ */

#endif /* !_RTAI_SCHEDCORE_H */
