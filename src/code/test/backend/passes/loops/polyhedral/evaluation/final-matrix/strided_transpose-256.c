/* C evaluation fixture for the bounded polyhedral optimizer. */
long kernel(long seed){long a[65536],b[65536];for(long i=0;i<256;i++)for(long j=0;j<256;j++){a[i*256+j]=i*256+j+seed;b[i*256+j]=0;}for(long i=1;i<256;i+=2)for(long j=1;j<256;j+=2)b[j*256+i]=a[i*256+j]+1;long sum=0;for(long i=0;i<256;i++)for(long j=0;j<256;j++)sum+=(a[i*256+j]+b[i*256+j])*(i+j+1);return sum;}
