/*
 * Project: rtai_cpp - RTAI C++ Framework 
 *
 * File: $Id: trace.h,v 1.3 2005/03/18 09:29:59 rpm Exp $
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


#ifndef __TRACE_H__
#define __TRACE_H__

#include <rtai_config.h>

namespace RTAI {

/**
 * Trace toolkit wrapper class
 */
class TraceEvent 
{
public:
	TraceEvent();
	TraceEvent( const char* name, void*, void* );
	~TraceEvent();
	
	void init( const char* name, void*, void* );
	void trigger( int size, void* data );	
protected:
	int m_ID;	
};

}; // namespace RTAI

#ifndef  CONFIG_RTAI_TRACE
#warning included trace.h but no support in kernel for tracing
#else

#ifdef __cplusplus
extern "C"  {
#endif
void __trace_destroy_event(int id);

int __trace_create_event(const char *name,
			 void *p1,
			 void *p2);

int __trace_raw_event(int id,
		      int size,
		      void *p);
#ifdef __cplusplus
}
#endif
#endif // CONFIG_RTAI_TRACE

#endif // __TRACE_H__

