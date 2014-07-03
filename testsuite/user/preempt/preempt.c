/*
COPYRIGHT (C) 2003  Paolo Mantegazza (mantegazza@aero.polimi.it)

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
*/


#include <pthread.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>

#include <rtai_sem.h>
#include <rtai_mbx.h>

#define NAVRG 4000

#define USE_FPU 0

#define FASTMUL  4

#define SLOWMUL  24

#if defined(CONFIG_UCLINUX) || defined(CONFIG_ARM)
#define TICK_TIME 1000000
#else
#define TICK_TIME 100000
#endif

static RT_TASK *Latency_Task;
static RT_TASK *Slow_Task;
static RT_TASK *Fast_Task;

static volatile int period, slowjit, fastjit;
static volatile RTIME expected;

static MBX *mbx;

static SEM *barrier;

static volatile int end;
static void endme (int dummy) { end = 1; }

static void *slow_fun(void *arg)
{
        int jit;
        RTIME svt, t;

        if (!(Slow_Task = rt_thread_init(nam2num("SLWTSK"), 3, 0, SCHED_FIFO, 1))) {
                printf("CANNOT INIT SLOW TASK\n");
                exit(1);
        }

	mlockall(MCL_CURRENT | MCL_FUTURE);
	rt_make_hard_real_time();
	rt_sem_wait_barrier(barrier);
        svt = rt_get_time() - SLOWMUL*period;
        while (!end) {  
                jit = (int) count2nano((t = rt_get_time()) - svt - SLOWMUL*period);
                svt = t;
                if (jit) { jit = - jit; }
                if (jit > slowjit) { slowjit = jit; }
                rt_busy_sleep(SLOWMUL/2*TICK_TIME);
                rt_task_wait_period();                                        
        }
	rt_sem_wait_barrier(barrier);
	rt_make_soft_real_time();
	rt_thread_delete(Slow_Task);
	return 0;
}                                        

static void *fast_fun(void *arg) 
{                             
        int jit;
        RTIME svt, t;

        if (!(Fast_Task = rt_thread_init(nam2num("FSTSK"), 2, 0, SCHED_FIFO, 1))) {
                printf("CANNOT INIT FAST TASK\n");
                exit(1);
        }

	mlockall(MCL_CURRENT | MCL_FUTURE);
	rt_make_hard_real_time();
	rt_sem_wait_barrier(barrier);
        svt = rt_get_time() - SLOWMUL*period;
        while (!end) {  
                jit = (int) count2nano((t = rt_get_time()) - svt - FASTMUL*period);
                svt = t;
                if (jit) { jit = - jit; }
                if (jit > fastjit) { fastjit = jit; }
                rt_busy_sleep(FASTMUL/2*TICK_TIME);
                rt_task_wait_period();                                        
        }                      
	rt_sem_wait_barrier(barrier);
	rt_make_soft_real_time();
	rt_thread_delete(Fast_Task);
	return 0;
}

static void *latency_fun(void *arg)
{
	struct sample { long min, max, avrg, jitters[2]; } samp;
	int diff;
	int skip;
	int average;
	int min_diff;
	int max_diff;

	min_diff = 1000000000;
	max_diff = -1000000000;
        if (!(Latency_Task = rt_thread_init(nam2num("LTCTSK"), 1, 0, SCHED_FIFO, 1))) {
                printf("CANNOT INIT LATENCY TASK\n");
                exit(1);
        }

	mlockall(MCL_CURRENT | MCL_FUTURE);
	rt_make_hard_real_time();
	rt_sem_wait_barrier(barrier);
        while (!end) {  
		average = 0;
		for (skip = 0; skip < NAVRG && !end; skip++) {
			expected += period;
			rt_task_wait_period();
			diff = (int)count2nano(rt_get_time() - expected);
			if (diff < min_diff) {
				min_diff = diff;
			}
			if (diff > max_diff) {
				max_diff = diff;
			}
			average += diff;
		}
		samp.min = min_diff;
		samp.max = max_diff;
		samp.avrg = average/NAVRG;
		samp.jitters[0] = fastjit;
		samp.jitters[1] = slowjit;
		rt_mbx_send_if(mbx, &samp, sizeof(samp));
	}
	rt_sem_wait_barrier(barrier);
	rt_make_soft_real_time();
	rt_thread_delete(Latency_Task);
	return 0;
}

static pthread_t latency_thread, fast_thread, slow_thread;

int main(void)
{
	RT_TASK *Main_Task;

	signal(SIGHUP,  endme);
	signal(SIGINT,  endme);
	signal(SIGKILL, endme);
	signal(SIGTERM, endme);
	signal(SIGALRM, endme);

        if (!(Main_Task = rt_thread_init(nam2num("MNTSK"), 0, 0, SCHED_FIFO, 1))) {
                printf("CANNOT INIT MAIN TASK\n");
                exit(1);
        }

        if (!(mbx = rt_mbx_init(nam2num("MBX"), 1000))) {
                printf("ERROR OPENING MBX\n");
                exit(1);
        }

	barrier = rt_sem_init(nam2num("PREMSM"), 4);
	latency_thread = rt_thread_create(latency_fun, NULL, 0);
	fast_thread    = rt_thread_create(fast_fun, NULL, 0);
	slow_thread    = rt_thread_create(slow_fun, NULL, 0);
	start_rt_timer(0);
	period = nano2count(TICK_TIME);
	rt_sem_wait_barrier(barrier);
	expected = rt_get_time() + 100*period;
	rt_task_make_periodic(Latency_Task, expected, period);
	rt_task_make_periodic(Fast_Task, expected, FASTMUL*period);
	rt_task_make_periodic(Slow_Task, expected, SLOWMUL*period);
	pause();
	end = 1;
	rt_sem_wait_barrier(barrier);
	rt_thread_join(latency_thread);
	rt_thread_join(fast_thread);
	rt_thread_join(slow_thread);
	stop_rt_timer();	
	rt_mbx_delete(mbx);
	rt_sem_delete(barrier);
	rt_thread_delete(Main_Task);
	return 0;
}
