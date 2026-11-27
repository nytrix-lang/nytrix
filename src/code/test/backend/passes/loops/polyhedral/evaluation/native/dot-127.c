/* C evaluation fixture for the bounded polyhedral optimizer. */
void dot(double *a,double *b,double *c) { for(long i=0;i<127;i++)for(long j=0;j<127;j++)for(long k=0;k<127;k++)c[i*127+j]+=a[i*127+k]*b[j*127+k]; }
