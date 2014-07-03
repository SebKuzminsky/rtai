/*
 *   ARTI -- RTAI-compatible Adeos-based Real-Time Interface. Based on
 *   the original RTAI layer for x86.
 *
 *   Copyright (C) 1994 Linus Torvalds
 *   Copyright (C) 2000 Gareth Hughes <gareth@valinux.com>,
 *   Copyright (C) 2000 Pierre Cloutier <pcloutier@PoseidonControls.com>
 *   and others.
 *
 *   RTAI/x86 rewrite over Adeos:
 *   Copyright (C) 2002 Philippe Gerum.
 * 
 *   Porting to x86_64 architecture:
 *   Copyright &copy; 2005 Paolo Mantegazza, \n
 *   Copyright &copy; 2005 Daniele Gasperini \n
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

#ifndef _RTAI_ASM_X8664_FPU_H
#define _RTAI_ASM_X8664_FPU_H

#ifndef __cplusplus
#include <asm/processor.h>
#endif /* !__cplusplus */

typedef union i387_union FPU_ENV;
   
#ifdef CONFIG_RTAI_FPU_SUPPORT

#define enable_fpu() clts()

#define save_fpcr_and_enable_fpu(fpcr)  do { \
	fpcr = read_cr0(); \
	enable_fpu(); \
} while (0)

#define restore_fpcr(fpcr)  do { \
	if (fpcr & 8) { \
		unsigned long flags; \
		rtai_hw_save_flags_and_cli(flags); \
		fpcr = read_cr0(); \
		write_cr0(8 | fpcr); \
		rtai_hw_restore_flags(flags); \
	} \
} while (0)

// initialise the hard fpu unit directly
#define init_hard_fpenv() do { \
	unsigned long __mxcsr; \
	__asm__ __volatile__ ("clts; fninit"); \
	__mxcsr = 0xffbfUL & 0x1f80UL; \
	__asm__ __volatile__ ("ldmxcsr %0": : "m" (__mxcsr)); \
} while (0)

// initialise the given fpenv union, without touching the related hard fpu unit
#define init_fpenv(fpenv)  do { \
        memset(&(fpenv).fxsave, 0, sizeof(struct i387_fxsave_struct)); \
        (fpenv).fxsave.cwd = 0x37f; \
	(fpenv).fxsave.mxcsr = 0x1f80; \
} while (0)

#if 0

#define save_fpenv(fpenv)  do { \
	__asm__ __volatile__ ("rex64; fxsave %0; fnclex": "=m" ((fpenv).fxsave)); \
} while (0)

#define restore_fpenv(fpenv)  do { \
        __asm__ __volatile__ ("fxrstor %0": : "m" ((fpenv).fxsave)); \
} while (0)

#else

/* taken from Linux i387.h */

static inline int __save_fpenv(struct i387_fxsave_struct __user *fx) 
{ 
	int err;

	asm volatile("1:  rex64/fxsave (%[fx])\n\t"
		     "2:\n"
		     ".section .fixup,\"ax\"\n"
		     "3:  movl $-1,%[err]\n"
		     "    jmp  2b\n"
		     ".previous\n"
		     ".section __ex_table,\"a\"\n"
		     "   .align 8\n"
		     "   .quad  1b,3b\n"
		     ".previous"
		     : [err] "=r" (err), "=m" (*fx)
		     : [fx] "cdaSDb" (fx), "0" (0));

	return err;
} 

static inline int __restore_fpenv(struct i387_fxsave_struct *fx) 
{ 
	int err;

	asm volatile("1:  rex64/fxrstor (%[fx])\n\t"
		     "2:\n"
		     ".section .fixup,\"ax\"\n"
		     "3:  movl $-1,%[err]\n"
		     "    jmp  2b\n"
		     ".previous\n"
		     ".section __ex_table,\"a\"\n"
		     "   .align 8\n"
		     "   .quad  1b,3b\n"
		     ".previous"
		     : [err] "=r" (err)
		     : [fx] "cdaSDb" (fx), "m" (*fx), "0" (0));

	return err;
} 

#define save_fpenv(fpenv)  do { \
	__save_fpenv(&fpenv.fxsave); \
} while (0)

#define restore_fpenv(fpenv)  do { \
	__restore_fpenv(&fpenv.fxsave); \
} while (0)
#endif

// FPU MANAGEMENT DRESSED FOR IN KTHREAD/THREAD/PROCESS FPU USAGE FROM RTAI

#define init_hard_fpu(lnxtsk)  do { \
        init_hard_fpenv(); \
        set_lnxtsk_uses_fpu(lnxtsk); \
        set_lnxtsk_using_fpu(lnxtsk); \
} while (0)

#define init_fpu(lnxtsk)  do { \
        init_fpenv((lnxtsk)->thread.i387); \
        set_lnxtsk_uses_fpu(lnxtsk); \
} while (0)

#define restore_fpu(lnxtsk)  do { \
        enable_fpu(); \
        restore_fpenv((lnxtsk)->thread.i387); \
        set_lnxtsk_using_fpu(lnxtsk); \
} while (0)

#else /* !CONFIG_RTAI_FPU_SUPPORT */

#define enable_fpu()
#define save_fpcr_and_enable_fpu(fpcr)
#define restore_fpcr(fpcr)
#define init_hard_fpenv()
#define init_fpenv(fpenv)
#define save_fpenv(fpenv)
#define restore_fpenv(fpenv)
#define init_hard_fpu(lnxtsk)
#define init_fpu(lnxtsk)
#define restore_fpu(lnxtsk)

#endif /* CONFIG_RTAI_FPU_SUPPORT */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0) && LINUX_VERSION_CODE < KERNEL_VERSION(2,6,11)

#define set_lnxtsk_uses_fpu(lnxtsk) \
        do { (lnxtsk)->used_math = 1; } while(0)
#define clear_lnxtsk_uses_fpu(lnxtsk) \
        do { (lnxtsk)->used_math = 0; } while(0)
#define lnxtsk_uses_fpu(lnxtsk)  ((lnxtsk)->used_math)

#define set_lnxtsk_using_fpu(lnxtsk) \
	do { task_thread_info(lnxtsk)->status |= TS_USEDFPU; } while(0)
//	do { (lnxtsk)->thread_info->status |= TS_USEDFPU; } while(0)

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0) && LINUX_VERSION_CODE < KERNEL_VERSION(2,6,11) */


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11)

#define set_lnxtsk_uses_fpu(lnxtsk) \
        do { set_stopped_child_used_math(lnxtsk); } while(0)
#define clear_lnxtsk_uses_fpu(lnxtsk) \
        do { clear_stopped_child_used_math(lnxtsk); } while(0)
#define lnxtsk_uses_fpu(lnxtsk)  (tsk_used_math(lnxtsk))

#define set_lnxtsk_using_fpu(lnxtsk) \
	do { task_thread_info(lnxtsk)->status |= TS_USEDFPU; } while(0)
//	do { (lnxtsk)->thread_info->status |= TS_USEDFPU; } while(0)

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11) */

#endif /* !_RTAI_ASM_X8664_FPU_H */
