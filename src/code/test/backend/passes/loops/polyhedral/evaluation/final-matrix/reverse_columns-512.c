/* C evaluation fixture for the bounded polyhedral optimizer. */
long kernel(long seed){long a[262144],b[262144];for(long i=0;i<512;i++)for(long j=0;j<512;j++){a[i*512+j]=i*512+j+seed;b[i*512+j]=0;}for(long i=0;i<512;i++)for(long j=511;j>=0;j--)b[j*512+i]=a[i*512+j]+1;long sum=0;for(long i=0;i<512;i++)for(long j=0;j<512;j++)sum+=(a[i*512+j]+b[i*512+j])*(i+j+1);return sum;}
