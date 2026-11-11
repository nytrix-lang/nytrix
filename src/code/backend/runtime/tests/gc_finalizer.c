/*
 * GC regression tests for finalizer ordering, repetition, and thread
 * collection.
 */
#define _POSIX_C_SOURCE 200809L
#include "code/backend/runtime/gc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <pthread.h>
#endif

static int finalized;
static int repeated;
static void finalize_again(int64_t object) {
  if (++repeated == 1)
    nyGcSetFinalizer(object, finalize_again);
}
static int64_t survivor;

#ifndef _WIN32
static void *collect_on_another_thread(void *unused) {
  (void)unused;
  nyGcTriggerMajor();
  return NULL;
}
#endif

static void finalize(int64_t object) {
  (void)object;
  ++finalized;
  /*
   * Reentry must not compact an object while its callback is running.
   */
  nyGcTriggerMajor();
#ifndef _WIN32
  pthread_t worker;
  if (pthread_create(&worker, NULL, collect_on_another_thread, NULL) != 0 ||
      pthread_join(worker, NULL) != 0)
    abort();
#endif
  survivor = nyGcAlloc(32);
  if (!survivor)
    abort();
  *(uint64_t *)(uintptr_t)survivor = 42;
  nyGcAddRoot(&survivor);
}

int main(void) {
#ifdef _WIN32
  _putenv_s("NYTRIX_GC", "1");
  _putenv_s("NYTRIX_GC_LOS_THRESHOLD", "1m");
  _putenv_s("NYTRIX_GC_NURSERY_SIZE", "64k");
  _putenv_s("NYTRIX_GC_TENURED_SIZE", "256k");
#else
  setenv("NYTRIX_GC", "1", 1);
  setenv("NYTRIX_GC_LOS_THRESHOLD", "1m", 1);
  setenv("NYTRIX_GC_NURSERY_SIZE", "64k", 1);
  setenv("NYTRIX_GC_TENURED_SIZE", "256k", 1);
#endif
  nyGcInit();
  size_t sizes[] = {32, 64 * 1024 + 16, NYGC_DEFAULT_LOS_THRESHOLD};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    int64_t object = nyGcAlloc(sizes[i]);
    if (!object)
      return 1;
    nyGcAddRoot(&object);
    nyGcSetFinalizer(object, finalize);
    /*
     * Exercise registration relocation through nursery and tenured moves.
     */
    for (int n = 0; n < 4; ++n)
      nyGcTriggerMajor();
    if (finalized != (int)i)
      return 2;
    nyGcRemoveRoot(&object);
    nyGcTriggerMajor();
    if (finalized != (int)i + 1 || !survivor)
      return 3;
    nyGcAlloc(32);
    nyGcAlloc(32);
    nyGcTriggerMajor();
    if (*(uint64_t *)(uintptr_t)survivor != 42) {
      fputs("finalizer allocation was overwritten\n", stderr);
      return 4;
    }
    nyGcRemoveRoot(&survivor);
    survivor = 0;
  }
  int64_t again = nyGcAlloc(32);
  if (!again)
    return 7;
  nyGcSetFinalizer(again, finalize_again);
  nyGcTriggerMajor();
  if (repeated != 1)
    return 8;
  nyGcTriggerMajor();
  if (repeated != 2)
    return 9;
  int64_t plain = nyGcAlloc(32);
  if (!plain)
    return 5;
  *(uint64_t *)(uintptr_t)plain = 99;
  nyGcAddRoot(&plain);
  for (int n = 0; n < 5; ++n)
    nyGcTriggerMajor();
  if (*(uint64_t *)(uintptr_t)plain != 99)
    return 6;
  nyGcRemoveRoot(&plain);
  nyGcDispose();
  puts("GC-FINALIZER-OK");
  return 0;
}
