/*
COPYRIGHT (C) 2006  Roberto Bucher (roberto.bucher@supsi.ch)

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
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
*/

#include <machine.h>
#include <scicos_block4.h>
#include <rtai_fifos.h>
#include <stdio.h>

static void init(scicos_block *block)
{
  double * y;
  int i;
  int ntraces=block->nout;
  rtf_create(block->ipar[0],block->ipar[1]);
  rtf_reset(block->ipar[0]);
  for(i=0;i<ntraces;i++) {
    y = block->outptr[i];
    y[0]=0.0;
  }
}

static void inout(scicos_block *block)
{
  double *y;
  int ntraces=block->nout;
  int count;
  struct {
    double u[ntraces]; 
  } data;
  int i;

  count=rtf_get(block->ipar[0],&data,sizeof(data));
  if(count!=0) {
    for(i=0;i<ntraces;i++){
      y = block->outptr[i];
      y[0]=data.u[i];
    }
  }
}

static void end(scicos_block *block)
{
  rtf_destroy(block->ipar[0]);
  printf("FIFO %d closed\n",block->ipar[0]);
}



void rt_fifoin(scicos_block *block,int flag)
{
  if (flag==1){          /* set output */
    inout(block);
  }
  else if (flag==5){     /* termination */ 
    end(block);
  }
  else if (flag ==4){    /* initialisation */
    init(block);
  }
}


