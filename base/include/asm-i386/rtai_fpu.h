/*
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

/* 
 * Part of this code acked from Linux i387.h and i387.c:
 * Copyright (C) 1994 Linus Torvalds,
 * Copyright (C) 2000 Gareth Hughes <gareth@valinux.com>,
 * original idea of an RTAI own header file for the FPU stuff:
 * Copyright (C) 2000 Pierre Cloutier <pcloutier@PoseidonControls.com>,
 * this final rewrite and cleanup:
 * Copyright (C) 2005 Paolo Mantegazza <mantegazza@aero.polimi.it>.
 */

#ifndef _RTAI_ASM_I386_FPU_H
#define _RTAI_ASM_I386_FPU_H

#ifndef __cplusplus
#include <asm/processor.h>
#endif /* !__cplusplus */

typedef union i387_union FPU_ENV;
   
#ifdef CONFIG_RTAI_FPU_SUPPORT

// RAW FPU MANAGEMENT FOR USAGE FROM WHAT/WHEREVER RTAI DOES IN KERNEL

#define enable_fpu()  do { \
	__asm__ __volatile__ ("clts"); \
} while(0)

#define save_fpcr_and_enable_fpu(fpcr)  do { \
	__asm__ __volatile__ ("movl %%cr0, %0; clts": "=r" (fpcr)); \
} while (0)

#define restore_fpcr(fpcr)  do { \
	if (fpcr & 8) { \
		__asm__ __volatile__ ("movl %%cr0, %0": "=r" (fpcr)); \
		__asm__ __volatile__ ("movl %0, %%cr0": :"r" (fpcr | 8)); \
	} \
} while (0)

// initialise the hard fpu unit directly
#define init_hard_fpenv() do { \
	__asm__ __volatile__ ("clts; fninit"); \
	if (cpu_has_xmm) { \
		unsigned long __mxcsr = (0xffbfu & 0x1f80u); \
		__asm__ __volatile__ ("ldmxcsr %0": : "m" (__mxcsr)); \
	} \
} while (0)

// initialise the given fpenv union, without touching the related hard fpu unit
#define init_fpenv(fpenv)  do { \
	if (cpu_has_fxsr) { \
		memset(&(fpenv).fxsave, 0, sizeof(struct i387_fxsave_struct)); \
		(fpenv).fxsave.cwd = 0x37f; \
		if (cpu_has_xmm) { \
			(fpenv).fxsave.mxcsr = 0x1f80; \
		} \
	} else { \
		memset(&(fpenv).fsave, 0, sizeof(struct i387_fsave_struct)); \
		(fpenv).fsave.cwd = 0xffff037fu; \
		(fpenv).fsave.swd = 0xffff0000u; \
		(fpenv).fsave.twd = 0xffffffffu; \
		(fpenv).fsave.fos = 0xffff0000u; \
	} \
} while (0)

#define save_fpenv(fpenv)  do { \
	if (cpu_has_fxsr) { \
		__asm__ __volatile__ ("fxsave %0; fnclex": "=m" ((fpenv).fxsave)); \
	} else { \
		__asm__ __volatile__ ("fnsave %0; fwait": "=m" ((fpenv).fsave)); \
	} \
} while (0)

#define restore_fpenv(fpenv)  do { \
	if (cpu_has_fxsr) { \
		__asm__ __volatile__ ("fxrstor %0": : "m" ((fpenv).fxsave)); \
	} else { \
		__asm__ __volatile__ ("frstor %0": : "m" ((fpenv).fsave)); \
	} \
} while (0)

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


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)

#define set_lnxtsk_uses_fpu(lnxtsk) \
	do { (lnxtsk)->used_math = 1; } while(0)
#define clear_lnxtsk_uses_fpu(lnxtsk) \
	do { (lnxtsk)->used_math = 0; } while(0)
#define lnxtsk_uses_fpu(lnxtsk)  ((lnxtsk)->used_math)

#define set_lnxtsk_using_fpu(lnxtsk) \
	do { (lnxtsk)->flags |= PF_USEDFPU; } while(0)

#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0) */


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


#endif /* !_RTAI_ASM_I386_FPU_H */
