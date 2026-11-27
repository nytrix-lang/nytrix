/* C evaluation fixture for the bounded polyhedral optimizer. */
long kernel(long seed){long a[16384],b[16384];for(long i=0;i<128;i++)for(long j=0;j<128;j++){a[i*128+j]=i*128+j+seed;b[i*128+j]=0;}for(long i=0;i<128;i++)for(long j=0;j<128;j++)b[i*128+j]=a[i*128+j]+1;long sum=0;for(long i=0;i<128;i++)for(long j=0;j<128;j++)sum+=(a[i*128+j]+b[i*128+j])*(i+j+1);return sum;}
