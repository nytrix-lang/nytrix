/* C evaluation fixture for the bounded polyhedral optimizer. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
extern long kernel(long);
int main(int argc, char **argv) {
  int reps = argc > 1 ? atoi(argv[1]) : 200;
  int warmups = argc > 2 ? atoi(argv[2]) : 4;
  volatile uint64_t sum=0;
  for (int i=0;i<warmups;i++) sum+=(uint64_t)kernel(i);
  sum=0;
  struct timespec a,b;
  clock_gettime(CLOCK_MONOTONIC,&a);
  for(int i=0;i<reps;i++) sum+=(uint64_t)kernel(i&15);
  clock_gettime(CLOCK_MONOTONIC,&b);
  printf("%llu %.9f\n", (unsigned long long)sum,(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)*1e-9);
}
