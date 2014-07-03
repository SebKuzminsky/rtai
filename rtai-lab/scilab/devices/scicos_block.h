#ifndef __SCICOS_BLOCK_H__
#define __SCICOS_BLOCK_H__

#ifdef __STDC__
#include <stdlib.h>
#else 
#include <malloc.h>
#endif 

typedef void (*voidg)(void);

typedef struct {
  int nevprt;
  voidg funpt ;
  int type;
  int scsptr;
  int nz;
  double *z;
  int nx;
  double *x;
  double *xd;
  double *res;
  int nin;
  int *insz;
  double **inptr;
  int nout;
  int *outsz;
  double **outptr;
  int nevout;
  double *evout;
  int nrpar;
  double *rpar;
  int nipar;
  int *ipar;
  int ng;
  double *g;
  int ztyp;
  int *jroot;
  char *label;
  void **work;
  int nmode;
  int *mode;
} scicos_block;


void do_cold_restart(void);
int get_phase_simulation(void);
double get_scicos_time(void);
int get_block_number(void);
void par_getstr(char * str, int par[], int init, int len);
void set_block_error(int);
void set_pointer_xproperty(int* pointer);

void * scicos_malloc(size_t );
void scicos_free(void *p);

#define max(a,b) ((a) >= (b) ? (a) : (b))
#define min(a,b) ((a) <= (b) ? (a) : (b))

extern int s_copy(void);
extern int s_cmp(void);


#endif /* __SCICOS_BLOCK_H__ */
