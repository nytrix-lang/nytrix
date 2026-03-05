#ifndef NYTRIX_RT_GC_H
#define NYTRIX_RT_GC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* GC Configuration */
#define NYGC_NURSERY_SIZE (4 * 1024 * 1024)  /* 4 MB nursery */
#define NYGC_TENURED_SIZE (64 * 1024 * 1024) /* 64 MB tenured */
#define NYGC_PROMOTION_AGE 3                 /* Promote after 3 collections */

/* Object header flags */
#define NYGC_MARKED (1 << 0)
#define NYGC_SCANNED (1 << 1)
#define NYGC_TENURED (1 << 2)
#define NYGC_FINALIZER (1 << 3)
#define NYGC_WEAK (1 << 4)

/* Object header (16 bytes) */
typedef struct nyGcHeader {
  uint32_t flags;
  uint32_t age;
  uint64_t size;
} nyGcHeader_t;

#define NYGC_HEADER_SIZE sizeof(nyGcHeader_t)
#define NYGC_OBJECT_DATA_OFFSET NYGC_HEADER_SIZE

/* GC Statistics */
typedef struct nyGcStats {
  size_t nursery_allocated;
  size_t tenured_allocated;
  size_t nursery_collections;
  size_t tenured_collections;
  size_t objects_promoted;
  size_t objects_swept;
  size_t bytes_freed;
  double last_pause_ms;
} nyGcStats_t;

/* GC State */
typedef struct nyGcState {
  /* Nursery space */
  uint8_t *nursery_start;
  uint8_t *nursery_limit;
  uint8_t *nursery_ptr;

  /* Tenured space */
  uint8_t *tenured_start;
  uint8_t *tenured_limit;
  uint8_t *tenured_scan;
  uint8_t *tenured_free;

  /* Remembered set (tenured -> nursery refs) */
  int64_t **remembered_set;
  size_t remembered_count;
  size_t remembered_capacity;

  /* Roots */
  int64_t **roots;
  size_t root_count;
  size_t root_capacity;

  /* Statistics */
  nyGcStats_t stats;

  /* State flags */
  bool initialized;
  bool collecting;
  bool parallel_enabled;
} nyGcState_t;

/* Initialize GC */
void nyGcInit(void);

/* Shutdown GC */
void nyGcDispose(void);

/* Allocate from nursery (fast path) */
static inline int64_t nyGcAllocFast(size_t size) {
  extern nyGcState_t gNyGc;

  size_t aligned_size = (size + 7) & ~7; /* 8-byte align */
  uint8_t *new_ptr = gNyGc.nursery_ptr + aligned_size;

  if (new_ptr > gNyGc.nursery_limit) {
    return 0; /* Nursery overflow, use slow path */
  }

  nyGcHeader_t *header = (nyGcHeader_t *)gNyGc.nursery_ptr;
  header->flags = 0;
  header->age = 0;
  header->size = size;

  gNyGc.nursery_ptr = new_ptr;
  gNyGc.stats.nursery_allocated += aligned_size;

  return (int64_t)((uint8_t *)header + NYGC_HEADER_SIZE);
}

/* Allocate (with fallback to tenured) */
int64_t nyGcAlloc(size_t size);

/* Slow path allocation */
int64_t nyGcAllocSlow(size_t size);

/* Trigger collections */
void nyGcCollect(void);
void nyGcTriggerMinor(void);
void nyGcTriggerMajor(void);

/* Root management */
void nyGcAddRoot(int64_t *slot);
void nyGcRemoveRoot(int64_t *slot);
void nyGcWriteBarrier(int64_t *slot, int64_t value);

/* Object management */
void nyGcSetFinalizer(int64_t obj, void (*finalizer)(int64_t));
bool nyGcIsMarked(int64_t obj);
void nyGcMark(int64_t obj);

/* Debug */
void nyGcDumpStats(FILE *out);
size_t nyGcGetHeapUsage(void);

#endif /* NYTRIX_RT_GC_H */
