/* C evaluation fixture for the bounded polyhedral optimizer. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef N
#define N 65
#endif
#define M (N*N)
#define CAP (6*M+64)
extern void dot(double*,double*,double*);
static void ref(double*a,double*b,double*c){for(int i=0;i<N;i++)for(int j=0;j<N;j++)for(int k=0;k<N;k++)c[i*N+j]+=a[i*N+k]*b[j*N+k];}
int main(void){double *x=malloc(CAP*sizeof(double)),*y=malloc(CAP*sizeof(double));if(!x||!y)return 2;
 const int offsets[][3]={{0,M,2*M},{0,0,2*M},{0,M,0},{0,M,M},{0,0,0},{0,M,M-1},{0,M,M+1},{1,2*M,0},{0,1,2*M},{0,2*M,1},{M,0,2*M},{M,0,M-1},{0,M,3*M},{0,2*M,4*M}};
 unsigned tests=0;for(unsigned seed=0;seed<4;seed++)for(unsigned z=0;z<sizeof(offsets)/sizeof(*offsets);z++){
 uint32_t state=seed+1;for(int i=0;i<CAP;i++){state=state*1664525u+1013904223u; x[i]=((double)(state%21)-10)/(seed&1?12347.0:4096.0);}
 memcpy(y,x,CAP*sizeof(double));int a=offsets[z][0],b=offsets[z][1],c=offsets[z][2];ref(y+a,y+b,y+c);dot(x+a,x+b,x+c);
 for(int i=0;i<CAP;i++)if(!isfinite(x[i])||fabs(x[i]-y[i])>1e-12*(1+fabs(y[i]))){fprintf(stderr,"FP mismatch size=%d seed=%u alias=%u index=%d %.17g %.17g\n",N,seed,z,i,x[i],y[i]);return 1;}tests++;
 }free(x);free(y);printf("N=%d: %u full-buffer comparisons incl. all-root aliasing, nonzero initial accumulators, nonexact floats and odd tails\n",N,tests);}
