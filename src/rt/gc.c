#include "rt/gc.h"
#include "base/common.h"
#include "base/util.h"
#include "rt/shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global GC state */
nyGcState_t gNyGc = {0};

/* Forward declarations */
static void nyGcMinorCollect(void);
static void nyGcMajorCollect(void);
static void nyGcMark_from_roots(void);
static void nyGcSweepNursery(void);
static void nyGcSweepTenured(void);
static void nyGcPromoteSurvivors(void);
static void nyGcAddRemembered(int64_t *slot);

/* Initialize GC */
void nyGcInit(void) {
  if (gNyGc.initialized)
    return;

  memset(&gNyGc, 0, sizeof(gNyGc));

  /* Allocate nursery */
  gNyGc.nursery_start = (uint8_t *)malloc(NYGC_NURSERY_SIZE);
  if (!gNyGc.nursery_start) {
    fprintf(stderr, "Failed to allocate GC nursery\n");
    exit(1);
  }
  gNyGc.nursery_ptr = gNyGc.nursery_start;
  gNyGc.nursery_limit = gNyGc.nursery_start + NYGC_NURSERY_SIZE;

  /* Allocate tenured space */
  gNyGc.tenured_start = (uint8_t *)malloc(NYGC_TENURED_SIZE);
  if (!gNyGc.tenured_start) {
    fprintf(stderr, "Failed to allocate GC tenured space\n");
    exit(1);
  }
  gNyGc.tenured_free = gNyGc.tenured_start;
  gNyGc.tenured_limit = gNyGc.tenured_start + NYGC_TENURED_SIZE;
  gNyGc.tenured_scan = gNyGc.tenured_start;

  /* Initialize remembered set */
  gNyGc.remembered_capacity = 1024;
  gNyGc.remembered_set =
      (int64_t **)malloc(gNyGc.remembered_capacity * sizeof(int64_t *));
  gNyGc.remembered_count = 0;

  /* Initialize root set */
  gNyGc.root_capacity = 256;
  gNyGc.roots = (int64_t **)malloc(gNyGc.root_capacity * sizeof(int64_t *));
  gNyGc.root_count = 0;

  gNyGc.initialized = true;
  gNyGc.parallel_enabled = ny_env_enabled("NYTRIX_GC_PARALLEL");
}

/* Shutdown GC */
void nyGcDispose(void) {
  if (!gNyGc.initialized)
    return;

  free(gNyGc.nursery_start);
  free(gNyGc.tenured_start);
  free(gNyGc.remembered_set);
  free(gNyGc.roots);

  memset(&gNyGc, 0, sizeof(gNyGc));
}

/* Add root */
void nyGcAddRoot(int64_t *slot) {
  if (!slot)
    return;

  if (gNyGc.root_count >= gNyGc.root_capacity) {
    gNyGc.root_capacity *= 2;
    gNyGc.roots = (int64_t **)realloc(gNyGc.roots,
                                      gNyGc.root_capacity * sizeof(int64_t *));
  }
  gNyGc.roots[gNyGc.root_count++] = slot;
}

/* Remove root */
void nyGcRemoveRoot(int64_t *slot) {
  for (size_t i = 0; i < gNyGc.root_count; i++) {
    if (gNyGc.roots[i] == slot) {
      gNyGc.roots[i] = gNyGc.roots[--gNyGc.root_count];
      return;
    }
  }
}

/* Write barrier for tenured -> nursery references */
void nyGcWriteBarrier(int64_t *slot, int64_t value) {
  if (!slot || !value)
    return;

  /* Check if slot is in tenured space */
  uint8_t *slot_addr = (uint8_t *)slot;
  if (slot_addr >= gNyGc.tenured_start && slot_addr < gNyGc.tenured_limit) {
    /* Check if value points to nursery */
    uint8_t *val_addr = (uint8_t *)(uintptr_t)value;
    if (val_addr >= gNyGc.nursery_start && val_addr < gNyGc.nursery_limit) {
      nyGcAddRemembered(slot);
    }
  }

  *slot = value;
}

/* Add to remembered set */
static void nyGcAddRemembered(int64_t *slot) {
  /* Check if already in set */
  for (size_t i = 0; i < gNyGc.remembered_count; i++) {
    if (gNyGc.remembered_set[i] == slot)
      return;
  }

  if (gNyGc.remembered_count >= gNyGc.remembered_capacity) {
    gNyGc.remembered_capacity *= 2;
    gNyGc.remembered_set = (int64_t **)realloc(
        gNyGc.remembered_set, gNyGc.remembered_capacity * sizeof(int64_t *));
  }
  gNyGc.remembered_set[gNyGc.remembered_count++] = slot;
}

/* Allocate (with fallback) */
int64_t nyGcAlloc(size_t size) {
  if (!gNyGc.initialized)
    nyGcInit();

  /* Try fast path first */
  int64_t obj = nyGcAllocFast(size);
  if (obj)
    return obj;

  /* Slow path: trigger collection and retry */
  return nyGcAllocSlow(size);
}

/* Slow path allocation */
int64_t nyGcAllocSlow(size_t size) {
  /* Trigger minor GC */
  nyGcTriggerMinor();

  /* Try nursery again */
  int64_t obj = nyGcAllocFast(size);
  if (obj)
    return obj;

  /* Allocate directly in tenured space */
  size_t aligned_size = ((size + NYGC_HEADER_SIZE + 7) & ~7);

  if (gNyGc.tenured_free + aligned_size > gNyGc.tenured_limit) {
    /* Need major GC */
    nyGcTriggerMajor();

    /* Check again after major GC */
    if (gNyGc.tenured_free + aligned_size <= gNyGc.tenured_limit) {
      nyGcHeader_t *header = (nyGcHeader_t *)gNyGc.tenured_free;
      header->flags = NYGC_TENURED;
      header->age = NYGC_PROMOTION_AGE;
      header->size = size;

      gNyGc.tenured_free += aligned_size;
      gNyGc.stats.tenured_allocated += aligned_size;

      return (int64_t)((uint8_t *)header + NYGC_HEADER_SIZE);
    }

    /* Out of memory */
    fprintf(stderr, "GC: Out of memory (requested %zu bytes)\n", size);
    return 0;
  }

  nyGcHeader_t *header = (nyGcHeader_t *)gNyGc.tenured_free;
  header->flags = NYGC_TENURED;
  header->age = NYGC_PROMOTION_AGE;
  header->size = size;

  gNyGc.tenured_free += aligned_size;
  gNyGc.stats.tenured_allocated += aligned_size;

  return (int64_t)((uint8_t *)header + NYGC_HEADER_SIZE);
}

/* Trigger minor GC */
void nyGcTriggerMinor(void) {
  if (gNyGc.collecting)
    return;

  gNyGc.collecting = true;
  nyGcMinorCollect();
  gNyGc.collecting = false;
}

/* Trigger major GC */
void nyGcTriggerMajor(void) {
  if (gNyGc.collecting)
    return;

  gNyGc.collecting = true;
  nyGcMajorCollect();
  gNyGc.collecting = false;
}

/* Full collection */
void nyGcCollect(void) {
  nyGcTriggerMinor();
  nyGcTriggerMajor();
}

/* Minor collection (nursery only) */
static void nyGcMinorCollect(void) {
  gNyGc.stats.nursery_collections++;

  /* Mark from roots */
  nyGcMark_from_roots();

  /* Sweep nursery */
  nyGcSweepNursery();

  /* Promote survivors */
  nyGcPromoteSurvivors();

  /* Reset nursery */
  gNyGc.nursery_ptr = gNyGc.nursery_start;

  /* Clear remembered set */
  gNyGc.remembered_count = 0;
}

/* Major collection (full) */
static void nyGcMajorCollect(void) {
  gNyGc.stats.tenured_collections++;

  /* Minor GC first */
  nyGcMinorCollect();

  /* Mark from roots (including tenured) */
  nyGcMark_from_roots();

  /* Sweep tenured */
  nyGcSweepTenured();
}

/* Mark from all roots */
static void nyGcMark_from_roots(void) {
  /* Mark root objects */
  for (size_t i = 0; i < gNyGc.root_count; i++) {
    int64_t root = *gNyGc.roots[i];
    if (root)
      nyGcMark(root);
  }

  /* Process remembered set */
  for (size_t i = 0; i < gNyGc.remembered_count; i++) {
    int64_t ref = *gNyGc.remembered_set[i];
    if (ref)
      nyGcMark(ref);
  }
}

/* Mark object and its children */
void nyGcMark(int64_t obj) {
  if (!obj)
    return;

  /* Get header */
  nyGcHeader_t *header =
      (nyGcHeader_t *)((uint8_t *)(uintptr_t)obj - NYGC_HEADER_SIZE);

  /* Check if already marked */
  if (header->flags & NYGC_MARKED)
    return;

  /* Mark it */
  header->flags |= NYGC_MARKED;

  /* TODO: Scan object fields and mark children */
  /* This requires type information which would come from the runtime */
}

/* Check if object is marked */
bool nyGcIsMarked(int64_t obj) {
  if (!obj)
    return false;

  nyGcHeader_t *header =
      (nyGcHeader_t *)((uint8_t *)(uintptr_t)obj - NYGC_HEADER_SIZE);

  return (header->flags & NYGC_MARKED) != 0;
}

/* Sweep nursery */
static void nyGcSweepNursery(void) {
  uint8_t *ptr = gNyGc.nursery_start;

  while (ptr < gNyGc.nursery_ptr) {
    nyGcHeader_t *header = (nyGcHeader_t *)ptr;
    size_t aligned_size = ((header->size + NYGC_HEADER_SIZE + 7) & ~7);

    if (!(header->flags & NYGC_MARKED)) {
      /* Object is dead */
      gNyGc.stats.objects_swept++;
      gNyGc.stats.bytes_freed += header->size;
    } else {
      /* Clear mark for next cycle */
      header->flags &= ~NYGC_MARKED;
    }

    ptr += aligned_size;
  }
}

/* Sweep tenured */
static void nyGcSweepTenured(void) {
  uint8_t *ptr = gNyGc.tenured_start;
  uint8_t *new_free = gNyGc.tenured_start;

  while (ptr < gNyGc.tenured_free) {
    nyGcHeader_t *header = (nyGcHeader_t *)ptr;
    size_t aligned_size = ((header->size + NYGC_HEADER_SIZE + 7) & ~7);

    if (header->flags & NYGC_MARKED) {
      /* Object is live */
      header->flags &= ~NYGC_MARKED;

      if (ptr != new_free) {
        memmove(new_free, ptr, aligned_size);
      }
      new_free += aligned_size;
    } else {
      /* Object is dead */
      gNyGc.stats.objects_swept++;
      gNyGc.stats.bytes_freed += header->size;
    }

    ptr += aligned_size;
  }

  gNyGc.tenured_free = new_free;
  gNyGc.tenured_scan = gNyGc.tenured_start;
}

/* Promote nursery survivors to tenured */
static void nyGcPromoteSurvivors(void) {
  uint8_t *ptr = gNyGc.nursery_start;

  while (ptr < gNyGc.nursery_ptr) {
    nyGcHeader_t *header = (nyGcHeader_t *)ptr;
    size_t aligned_size = ((header->size + NYGC_HEADER_SIZE + 7) & ~7);

    if (header->flags & NYGC_MARKED) {
      header->age++;

      /* Promote if old enough */
      if (header->age >= NYGC_PROMOTION_AGE) {
        /* Will be copied during tenured sweep */
        gNyGc.stats.objects_promoted++;
      }
    }

    ptr += aligned_size;
  }
}

/* Set finalizer */
void nyGcSetFinalizer(int64_t obj, void (*finalizer)(int64_t)) {
  if (!obj)
    return;

  nyGcHeader_t *header =
      (nyGcHeader_t *)((uint8_t *)(uintptr_t)obj - NYGC_HEADER_SIZE);

  if (finalizer) {
    header->flags |= NYGC_FINALIZER;
    /* TODO: Store finalizer pointer */
  } else {
    header->flags &= ~NYGC_FINALIZER;
  }
}

/* Get heap usage */
size_t nyGcGetHeapUsage(void) {
  return (gNyGc.nursery_ptr - gNyGc.nursery_start) +
         (gNyGc.tenured_free - gNyGc.tenured_start);
}

/* Dump statistics */
void nyGcDumpStats(FILE *out) {
  if (!out)
    out = stderr;

  fprintf(out, "\n=== GC Statistics ===\n");
  fprintf(out, "Nursery allocated:     %zu bytes\n",
          gNyGc.stats.nursery_allocated);
  fprintf(out, "Tenured allocated:     %zu bytes\n",
          gNyGc.stats.tenured_allocated);
  fprintf(out, "Minor collections:     %zu\n", gNyGc.stats.nursery_collections);
  fprintf(out, "Major collections:     %zu\n", gNyGc.stats.tenured_collections);
  fprintf(out, "Objects promoted:      %zu\n", gNyGc.stats.objects_promoted);
  fprintf(out, "Objects swept:         %zu\n", gNyGc.stats.objects_swept);
  fprintf(out, "Bytes freed:           %zu\n", gNyGc.stats.bytes_freed);
  fprintf(out, "Heap usage:            %zu bytes\n", nyGcGetHeapUsage());
  fprintf(out, "Parallel GC:           %s\n",
          gNyGc.parallel_enabled ? "enabled" : "disabled");
  fprintf(out, "======================\n\n");
}
