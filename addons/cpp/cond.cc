/*
 * Project: rtai_cpp - RTAI C++ Framework
 *
 * File: $Id: cond.cc,v 1.3 2005/03/18 09:29:59 rpm Exp $
 *
 * Copyright: (C) 2001,2002 Erwin Rol <erwin@muffin.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include "cond.h"
#include "mutex.h"

namespace RTAI {

Condition::Condition()
:	Semaphore(0, BIN_SEM)
{
}

Condition::~Condition(){
}

int Condition::wait( Mutex& mtx )
{
	return rt_cond_wait( m_Sem, mtx.m_Sem );
}

int Condition::wait_until( Mutex& mtx,const Count& time)
{
	return rt_cond_wait_until( m_Sem, mtx.m_Sem, time );	
}

int Condition::wait_timed( Mutex& mtx, const Count& delay)
{
	return rt_cond_wait_timed( m_Sem, mtx.m_Sem, delay );	
}

}; // namespace RTAI
