/*
 * Project: rtai_cpp - RTAI C++ Framework 
 *
 * File: $Id: rtai_wrapper.h,v 1.3 2005/03/18 09:29:59 rpm Exp $
 *
 * Copyright: (C) 2001,2002 Erwin Rol <erwin@muffin.org>
 *
 * Licence:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 */

#ifndef __RTAI_WRAPPER_H__
#define __RTAI_WRAPPER_H__

/**
 * The aim of this file is to contain all RTAI functions that can by no
 * means be included with the C++ compiler.
 */

#ifdef __cplusplus
extern "C"  {
#endif

#include <rtai_types.h>
	
void __rt_get_global_lock(void);

void __rt_release_global_lock(void);

int __hard_cpu_id(void);

#ifdef __cplusplus
}
#endif

#endif /* __RTAI_WRAPPER_H__ */
