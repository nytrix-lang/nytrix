#ifndef NYTRIX_RT_GC_H
#define NYTRIX_RT_GC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

// Local Gc for the compiler internal list like usage
// Not for the user program

/* GC Configuration. The collector is opt-in; these are enabled-GC defaults. */
#define NYGC_NURSERY_SIZE (256 * 1024 * 1024)    /* 256 MB nursery */
#define NYGC_TENURED_SIZE (1024 * 1024 * 1024)   /* 1 GB tenured */
#define NYGC_PROMOTION_AGE 3                   /* Promote after 3 collections */
#define NYGC_DEFAULT_LOS_THRESHOLD (1024 * 1024) /* 1 MB large-object cutoff */

/* Object header flags */
#define NYGC_MARKED (1 << 0)
#define NYGC_SCANNED (1 << 1)
#define NYGC_TENURED (1 << 2)
#define NYGC_FINALIZER (1 << 3)
#define NYGC_WEAK (1 << 4)
#define NYGC_LARGE (1 << 5)

/* Object header (16 bytes) */
typedef struct nyGcHeader {
  uint32_t flags;
  uint32_t age;
  uint64_t size;
} nyGcHeader_t;

#define NYGC_HEADER_SIZE sizeof(nyGcHeader_t)
#define NYGC_RUNTIME_PREFIX_SIZE 32
#define NYGC_OBJECT_DATA_OFFSET (NYGC_HEADER_SIZE + NYGC_RUNTIME_PREFIX_SIZE)

static inline size_t nyGcBodySize(size_t size) { return (size + 15u) & ~15u; }
static inline size_t nyGcAllocSize(size_t size) {
  return NYGC_OBJECT_DATA_OFFSET + nyGcBodySize(size);
}
static inline nyGcHeader_t *nyGcHeaderFromObject(int64_t obj) {
  uintptr_t raw = (uintptr_t)obj;
  assert(raw == 0 || (raw % _Alignof(nyGcHeader_t)) == 0);
  return (nyGcHeader_t *)((uint8_t *)raw - NYGC_OBJECT_DATA_OFFSET);
}

/* GC Statistics */
typedef struct nyGcStats {
  size_t nursery_allocated;
  size_t tenured_allocated;
  size_t large_allocated;
  size_t large_freed;
  size_t nursery_collections;
  size_t tenured_collections;
  size_t objects_promoted;
  size_t objects_swept;
  size_t bytes_freed;
  double last_pause_ms;
} nyGcStats_t;

typedef struct nyGcLargeObject {
  nyGcHeader_t *header;
  size_t total_size;
  struct nyGcLargeObject *next;
} nyGcLargeObject_t;

/* GC State */
typedef struct nyGcState {
  /* Nursery space */
  uint8_t *nursery_start;
  uint8_t *nursery_limit;
  uint8_t *nursery_ptr;
  size_t nursery_capacity;

  /* Tenured space */
  uint8_t *tenured_start;
  uint8_t *tenured_limit;
  uint8_t *tenured_free;
  size_t tenured_capacity;

  /* Large object space */
  nyGcLargeObject_t *large_objects;
  size_t large_count;
  size_t large_threshold;

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
  bool enable_nursery;
} nyGcState_t;

/* Initialize GC */
void nyGcInit(void);

/* Shutdown GC */
void nyGcDispose(void);

/* Allocate from nursery (fast path) */
int64_t nyGcAllocFast(size_t size);

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
