/*
 * Fixture: verify -pthread exposes the platform reentrant header contract.
 */
#include <pthread.h>

#ifndef _REENTRANT
#error -pthread must expose the reentrant header contract
#endif
#if _REENTRANT != 1
#error -pthread must define _REENTRANT as 1
#endif

static void *worker(void *context) {
  int *value = context;
  *value += 37;
  return context;
}

int main(void) {
  int value = 5;
  pthread_t thread;
  void *result = 0;
  if (pthread_create(&thread, 0, worker, &value) != 0)
    return 1;
  if (pthread_join(thread, &result) != 0)
    return 2;
  return result != &value || value != 42;
}
