/*
 * Project: rtai_cpp - RTAI C++ Framework 
 *
 * File: $Id: trace.cc,v 1.3 2005/03/18 09:29:59 rpm Exp $
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

#include "trace.h"

namespace RTAI {

TraceEvent::TraceEvent()
{
	m_ID = -1;
}

TraceEvent::TraceEvent( const char* name, void* p1, void* p2)
{
	init( name, p1, p2 );
}

TraceEvent::~TraceEvent()
{
#ifdef CONFIG_RTAI_TRACE
	if( m_ID != -1 )
		__trace_destroy_event(m_ID);
#endif
}

void TraceEvent::init( const char* name, void* p1, void* p2)
{
#ifdef CONFIG_RTAI_TRACE
	m_ID = __trace_create_event(name, p1, p2);
#endif
}

void TraceEvent::trigger( int size, void* data )
{
#ifdef CONFIG_RTAI_TRACE
	__trace_raw_event(m_ID, size, data );	
#endif
}

}; // namespace RTAI
