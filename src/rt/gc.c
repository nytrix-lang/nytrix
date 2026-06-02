#include "rt/gc.h"
#include "base/common.h"
#include "rt/shared.h"

#include <ctype.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global GC state */
nyGcState_t gNyGc = {0};

typedef struct nyGcForward {
  int64_t from;
  int64_t to;
} nyGcForward_t;

typedef struct nyGcForwardMap {
  nyGcForward_t *data;
  size_t count;
  size_t cap;
} nyGcForwardMap_t;

static atomic_flag gNyGcLock = ATOMIC_FLAG_INIT;

/* Forward declarations */
static void nyGcMinorCollectUnlocked(void);
static void nyGcMajorCollectUnlocked(void);
static void nyGcMark_from_roots(void);
static void nyGcPromoteSurvivors(void);
static void nyGcSweepTenured(void);
static void nyGcSweepLargeUnlocked(void);
static void nyGcClearLargeMarksUnlocked(void);
static void nyGcAddRememberedUnlocked(int64_t *slot);
static void nyGcMarkUnlocked(int64_t obj);
static void nyGcValidateUnlocked(const char *phase);

static void nyGcLock(void) {
  while (atomic_flag_test_and_set_explicit(&gNyGcLock, memory_order_acquire)) {
  }
}

static void nyGcUnlock(void) {
  atomic_flag_clear_explicit(&gNyGcLock, memory_order_release);
}

static uint8_t *nyGcObjPtr(nyGcHeader_t *header) {
  return (uint8_t *)header + NYGC_OBJECT_DATA_OFFSET;
}

static bool nyGcHeaderCandidateValid(nyGcHeader_t *header, uint8_t *used_end) {
  if (!header || !used_end)
    return false;
  if ((uint8_t *)header + NYGC_OBJECT_DATA_OFFSET > used_end)
    return false;
  uint8_t *obj = nyGcObjPtr(header);
  uint8_t *prefix = obj - NYGC_RUNTIME_PREFIX_SIZE;
  if (*(uint64_t *)prefix != NY_MAGIC1)
    return false;
  uint64_t size_slot = *(uint64_t *)(prefix + 8);
  if (header->size > (size_t)-1 - 15u)
    return false;
  size_t body = nyGcBodySize(header->size);
  size_t total = NYGC_OBJECT_DATA_OFFSET + body;
  if (total < NYGC_OBJECT_DATA_OFFSET || (uint8_t *)header + total > used_end)
    return false;
  return (size_slot >> 1) >= header->size;
}

static bool nyGcHeaderInSpace(int64_t obj, uint8_t *start, uint8_t *used_end,
                              nyGcHeader_t **out) {
  if (!start || !used_end)
    return false;
  uint8_t *p = (uint8_t *)(uintptr_t)obj;
  if (p < start + NYGC_OBJECT_DATA_OFFSET || p >= used_end + NYGC_OBJECT_DATA_OFFSET)
    return false;
  nyGcHeader_t *header = nyGcHeaderFromObject(obj);
  if ((uint8_t *)header < start || (uint8_t *)header >= used_end ||
      !nyGcHeaderCandidateValid(header, used_end))
    return false;
  if (out)
    *out = header;
  return true;
}

static bool nyGcHeaderForObject(int64_t obj, nyGcHeader_t **out) {
  if (!obj || !is_ptr(obj) || !gNyGc.initialized)
    return false;
  if (nyGcHeaderInSpace(obj, gNyGc.nursery_start, gNyGc.nursery_ptr, out))
    return true;
  if (nyGcHeaderInSpace(obj, gNyGc.tenured_start, gNyGc.tenured_free, out))
    return true;
  uint8_t *p = (uint8_t *)(uintptr_t)obj;
  for (nyGcLargeObject_t *large = gNyGc.large_objects; large; large = large->next) {
    if (!large->header)
      continue;
    uint8_t *obj_ptr = nyGcObjPtr(large->header);
    if (p == obj_ptr && nyGcHeaderCandidateValid(large->header,
                                                 (uint8_t *)large->header + large->total_size)) {
      if (out)
        *out = large->header;
      return true;
    }
  }
  return false;
}

static void nyGcInitObject(nyGcHeader_t *header, size_t size, uint32_t flags, uint32_t age) {
  size_t total = nyGcAllocSize(size);
  memset(header, 0, total);
  header->flags = flags;
  header->age = age;
  header->size = size;

  uint8_t *obj = nyGcObjPtr(header);
  uint8_t *prefix = obj - NYGC_RUNTIME_PREFIX_SIZE;
  *(uint64_t *)prefix = NY_MAGIC1;
  *(uint64_t *)(prefix + 8) = ((uint64_t)nyGcBodySize(size) << 1) | 1u;
  rt_heap_ptr_cache_store((uintptr_t)obj);
}

static int64_t nyGcAllocFastUnlocked(size_t size) {
  size_t total = nyGcAllocSize(size);
  if (gNyGc.nursery_ptr + total > gNyGc.nursery_limit)
    return 0;
  nyGcHeader_t *header = (nyGcHeader_t *)gNyGc.nursery_ptr;
  nyGcInitObject(header, size, 0, 0);
  gNyGc.nursery_ptr += total;
  gNyGc.stats.nursery_allocated += total;
  return (int64_t)(uintptr_t)nyGcObjPtr(header);
}

static int64_t nyGcAllocTenuredUnlocked(size_t size) {
  size_t total = nyGcAllocSize(size);
  if (gNyGc.tenured_free + total > gNyGc.tenured_limit)
    return 0;
  nyGcHeader_t *header = (nyGcHeader_t *)gNyGc.tenured_free;
  nyGcInitObject(header, size, NYGC_TENURED, NYGC_PROMOTION_AGE);
  gNyGc.tenured_free += total;
  gNyGc.stats.tenured_allocated += total;
  return (int64_t)(uintptr_t)nyGcObjPtr(header);
}

static int64_t nyGcAllocLargeUnlocked(size_t size) {
  size_t total = nyGcAllocSize(size);
  nyGcLargeObject_t *node = (nyGcLargeObject_t *)malloc(sizeof(*node));
  nyGcHeader_t *header = (nyGcHeader_t *)malloc(total);
  if (!node || !header) {
    free(node);
    free(header);
    return 0;
  }
  nyGcInitObject(header, size, NYGC_TENURED | NYGC_LARGE, NYGC_PROMOTION_AGE);
  node->header = header;
  node->total_size = total;
  node->next = gNyGc.large_objects;
  gNyGc.large_objects = node;
  gNyGc.large_count++;
  gNyGc.stats.large_allocated += total;
  return (int64_t)(uintptr_t)nyGcObjPtr(header);
}

static bool nyGcForwardPush(nyGcForwardMap_t *map, int64_t from, int64_t to) {
  if (!from || from == to)
    return true;
  if (map->count >= map->cap) {
    size_t next = map->cap ? map->cap * 2 : 256;
    nyGcForward_t *data = (nyGcForward_t *)realloc(map->data, next * sizeof(*data));
    if (!data)
      return false;
    map->data = data;
    map->cap = next;
  }
  map->data[map->count++] = (nyGcForward_t){from, to};
  return true;
}

static int64_t nyGcForwardFind(const nyGcForwardMap_t *map, int64_t value) {
  if (!value)
    return 0;
  for (size_t i = 0; i < map->count; i++) {
    if (map->data[i].from == value)
      return map->data[i].to;
  }
  return 0;
}

static void nyGcForwardSlot(const nyGcForwardMap_t *map, int64_t *slot) {
  if (!slot || !*slot || !is_ptr(*slot))
    return;
  int64_t next = nyGcForwardFind(map, *slot);
  if (next)
    *slot = next;
}

static int64_t nyGcTaggedToInt(int64_t v) {
  return is_int(v) ? (v >> 1) : v;
}

static void nyGcMarkSlot(int64_t value) {
  if (value && is_ptr(value))
    nyGcMarkUnlocked(value);
}

static void nyGcUpdateObjectRefs(nyGcHeader_t *header, const nyGcForwardMap_t *map) {
  int64_t obj = (int64_t)(uintptr_t)nyGcObjPtr(header);
  int64_t tag = *(int64_t *)((uint8_t *)(uintptr_t)obj - 8);

  if (tag == TAG_LIST || tag == TAG_TUPLE) {
    int64_t len = nyGcTaggedToInt(*(int64_t *)((uint8_t *)(uintptr_t)obj + 0));
    size_t max_items = header->size > 16 ? (header->size - 16) / sizeof(int64_t) : 0;
    if (len < 0)
      len = 0;
    if ((uint64_t)len > max_items)
      len = (int64_t)max_items;
    int64_t *items = (int64_t *)((uint8_t *)(uintptr_t)obj + 16);
    for (int64_t i = 0; i < len; i++)
      nyGcForwardSlot(map, &items[i]);
  } else if (tag == TAG_DICT) {
    int64_t cap = nyGcTaggedToInt(*(int64_t *)((uint8_t *)(uintptr_t)obj + 8));
    size_t max_slots = header->size > 16 ? (header->size - 16) / 24 : 0;
    if (cap < 0)
      cap = 0;
    if ((uint64_t)cap > max_slots)
      cap = (int64_t)max_slots;
    for (int64_t i = 0; i < cap; i++) {
      uint8_t *slot = (uint8_t *)(uintptr_t)obj + 16 + (size_t)i * 24;
      int64_t state = *(int64_t *)(slot + 16);
      if (state != 1 && state != rt_tag_v(1))
        continue;
      nyGcForwardSlot(map, (int64_t *)slot);
      nyGcForwardSlot(map, (int64_t *)(slot + 8));
    }
  } else if (tag == TAG_OK || tag == TAG_ERR) {
    if (header->size >= 8)
      nyGcForwardSlot(map, (int64_t *)((uint8_t *)(uintptr_t)obj + 0));
    if (header->size >= 16)
      nyGcForwardSlot(map, (int64_t *)((uint8_t *)(uintptr_t)obj + 8));
  } else if (tag == TAG_CLOSURE) {
    if (header->size >= 16)
      nyGcForwardSlot(map, (int64_t *)((uint8_t *)(uintptr_t)obj + 8));
    if (header->size >= 24)
      nyGcForwardSlot(map, (int64_t *)((uint8_t *)(uintptr_t)obj + 16));
  }
}

static void nyGcApplyForwards(const nyGcForwardMap_t *map) {
  for (size_t i = 0; i < gNyGc.root_count; i++)
    nyGcForwardSlot(map, gNyGc.roots[i]);
  for (size_t i = 0; i < gNyGc.remembered_count; i++)
    nyGcForwardSlot(map, gNyGc.remembered_set[i]);

  uint8_t *ptr = gNyGc.tenured_start;
  while (ptr < gNyGc.tenured_free) {
    nyGcHeader_t *header = (nyGcHeader_t *)ptr;
    nyGcUpdateObjectRefs(header, map);
    ptr += nyGcAllocSize(header->size);
  }
  for (nyGcLargeObject_t *large = gNyGc.large_objects; large; large = large->next) {
    if (large->header)
      nyGcUpdateObjectRefs(large->header, map);
  }
}

static void nyGcValidateSpace(const char *phase, const char *name,
                              uint8_t *start, uint8_t *ptr, uint8_t *limit) {
  if (!start || !ptr || !limit)
    return;
  if (ptr < start || ptr > limit) {
    fprintf(stderr, "GC validate failed (%s): %s pointer outside space\n", phase, name);
    abort();
  }
  uint8_t *p = start;
  while (p < ptr) {
    if (p + NYGC_OBJECT_DATA_OFFSET > ptr) {
      fprintf(stderr, "GC validate failed (%s): truncated %s object header\n", phase, name);
      abort();
    }
    nyGcHeader_t *header = (nyGcHeader_t *)p;
    size_t total = nyGcAllocSize(header->size);
    if (total < NYGC_OBJECT_DATA_OFFSET || p + total > ptr) {
      fprintf(stderr, "GC validate failed (%s): invalid %s object size %llu\n",
              phase, name, (unsigned long long)header->size);
      abort();
    }
    uint8_t *obj = nyGcObjPtr(header);
    uint64_t magic = *(uint64_t *)(obj - NYGC_RUNTIME_PREFIX_SIZE);
    uint64_t size_slot = *(uint64_t *)(obj - NYGC_RUNTIME_PREFIX_SIZE + 8);
    if (magic != NY_MAGIC1 || ((size_slot >> 1) < header->size)) {
      fprintf(stderr,
              "GC validate failed (%s): invalid %s runtime prefix header=%p obj=%p "
              "magic=0x%llx size_slot=0x%llx header_size=%llu\n",
              phase, name, (void *)header, (void *)obj,
              (unsigned long long)magic, (unsigned long long)size_slot,
              (unsigned long long)header->size);
      abort();
    }
    p += total;
  }
  if (p != ptr) {
    fprintf(stderr, "GC validate failed (%s): %s object walk mismatch\n", phase, name);
    abort();
  }
}

static void nyGcValidateUnlocked(const char *phase) {
  if (!gNyGc.initialized || !rt_env_enabled("NYTRIX_GC_VALIDATE"))
    return;
  nyGcValidateSpace(phase, "nursery", gNyGc.nursery_start, gNyGc.nursery_ptr, gNyGc.nursery_limit);
  nyGcValidateSpace(phase, "tenured", gNyGc.tenured_start, gNyGc.tenured_free, gNyGc.tenured_limit);
  for (size_t i = 0; i < gNyGc.root_count; i++) {
    if (!gNyGc.roots[i]) {
      fprintf(stderr, "GC validate failed (%s): null root slot\n", phase);
      abort();
    }
  }
  for (size_t i = 0; i < gNyGc.remembered_count; i++) {
    int64_t *slot = gNyGc.remembered_set[i];
    bool slot_in_large = false;
    for (nyGcLargeObject_t *large = gNyGc.large_objects; large; large = large->next) {
      uint8_t *start = (uint8_t *)large->header;
      uint8_t *end = start + large->total_size;
      if ((uint8_t *)slot >= start && (uint8_t *)slot < end) {
        slot_in_large = true;
        break;
      }
    }
    if (!slot || (!slot_in_large &&
                  ((uint8_t *)slot < gNyGc.tenured_start || (uint8_t *)slot >= gNyGc.tenured_free))) {
      fprintf(stderr, "GC validate failed (%s): invalid remembered slot\n", phase);
      abort();
    }
  }
}

static size_t nyGcLargeThresholdFromEnv(void) {
  const char *raw = getenv("NYTRIX_GC_LOS_THRESHOLD");
  if (!raw || !*raw)
    return NYGC_DEFAULT_LOS_THRESHOLD;
  char *end = NULL;
  unsigned long long value = strtoull(raw, &end, 0);
  if (!end || *end != '\0' || value < NYGC_OBJECT_DATA_OFFSET)
    return NYGC_DEFAULT_LOS_THRESHOLD;
  return (size_t)value;
}

static size_t nyGcByteSizeFromEnv(const char *name, size_t fallback, size_t minimum) {
  const char *raw = getenv(name);
  if (!raw || !*raw)
    return fallback;

  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(raw, &end, 0);
  if (end == raw || errno != 0)
    return fallback;

  while (*end && isspace((unsigned char)*end))
    end++;

  unsigned long long scale = 1;
  if (*end) {
    switch (tolower((unsigned char)*end)) {
    case 'k':
      scale = 1024ULL;
      end++;
      break;
    case 'm':
      scale = 1024ULL * 1024ULL;
      end++;
      break;
    case 'g':
      scale = 1024ULL * 1024ULL * 1024ULL;
      end++;
      break;
    default:
      return fallback;
    }
    if (*end == 'b' || *end == 'B')
      end++;
    while (*end && isspace((unsigned char)*end))
      end++;
    if (*end)
      return fallback;
  }

  if (value > (unsigned long long)(SIZE_MAX / scale))
    return fallback;
  size_t out = (size_t)(value * scale);
  return out < minimum ? minimum : out;
}

/* Initialize GC */
void nyGcInit(void) {
  nyGcLock();
  if (gNyGc.initialized) {
    nyGcUnlock();
    return;
  }

  memset(&gNyGc, 0, sizeof(gNyGc));
  gNyGc.enable_nursery = rt_env_enabled("NYTRIX_GC");
  gNyGc.large_threshold = nyGcLargeThresholdFromEnv();
  gNyGc.initialized = true;

  if (!gNyGc.enable_nursery) {
    nyGcUnlock();
    return;
  }

  gNyGc.nursery_capacity =
      nyGcByteSizeFromEnv("NYTRIX_GC_NURSERY_SIZE", NYGC_NURSERY_SIZE,
                          64u * 1024u);
  gNyGc.tenured_capacity =
      nyGcByteSizeFromEnv("NYTRIX_GC_TENURED_SIZE", NYGC_TENURED_SIZE,
                          256u * 1024u);

  gNyGc.nursery_start = (uint8_t *)malloc(gNyGc.nursery_capacity);
  if (!gNyGc.nursery_start) {
    memset(&gNyGc, 0, sizeof(gNyGc));
    nyGcUnlock();
    fprintf(stderr, "Failed to allocate GC nursery\n");
    exit(1);
  }
  gNyGc.nursery_ptr = gNyGc.nursery_start;
  gNyGc.nursery_limit = gNyGc.nursery_start + gNyGc.nursery_capacity;

  gNyGc.tenured_start = (uint8_t *)malloc(gNyGc.tenured_capacity);
  if (!gNyGc.tenured_start) {
    free(gNyGc.nursery_start);
    memset(&gNyGc, 0, sizeof(gNyGc));
    nyGcUnlock();
    fprintf(stderr, "Failed to allocate GC tenured space\n");
    exit(1);
  }
  gNyGc.tenured_free = gNyGc.tenured_start;
  gNyGc.tenured_limit = gNyGc.tenured_start + gNyGc.tenured_capacity;
  gNyGc.tenured_scan = gNyGc.tenured_start;

  gNyGc.remembered_capacity = 1024;
  gNyGc.remembered_set = (int64_t **)malloc(gNyGc.remembered_capacity * sizeof(int64_t *));
  gNyGc.root_capacity = 256;
  gNyGc.roots = (int64_t **)malloc(gNyGc.root_capacity * sizeof(int64_t *));
  if (!gNyGc.remembered_set || !gNyGc.roots) {
    free(gNyGc.nursery_start);
    free(gNyGc.tenured_start);
    free(gNyGc.remembered_set);
    free(gNyGc.roots);
    memset(&gNyGc, 0, sizeof(gNyGc));
    nyGcUnlock();
    fprintf(stderr, "Failed to allocate GC metadata\n");
    exit(1);
  }

  nyGcUnlock();
}

/* Shutdown GC */
void nyGcDispose(void) {
  nyGcLock();
  if (!gNyGc.initialized) {
    nyGcUnlock();
    return;
  }

  free(gNyGc.nursery_start);
  free(gNyGc.tenured_start);
  nyGcLargeObject_t *large = gNyGc.large_objects;
  while (large) {
    nyGcLargeObject_t *next = large->next;
    free(large->header);
    free(large);
    large = next;
  }
  free(gNyGc.remembered_set);
  free(gNyGc.roots);

  memset(&gNyGc, 0, sizeof(gNyGc));
  nyGcUnlock();
}

/* Add root */
void nyGcAddRoot(int64_t *slot) {
  if (!slot)
    return;
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery)
    return;
  nyGcLock();
  if (gNyGc.root_count >= gNyGc.root_capacity) {
    gNyGc.root_capacity *= 2;
    gNyGc.roots = (int64_t **)realloc(gNyGc.roots, gNyGc.root_capacity * sizeof(int64_t *));
    if (!gNyGc.roots) {
      nyGcUnlock();
      fprintf(stderr, "GC: root set allocation failed\n");
      exit(1);
    }
  }
  gNyGc.roots[gNyGc.root_count++] = slot;
  nyGcUnlock();
}

/* Remove root */
void nyGcRemoveRoot(int64_t *slot) {
  if (!gNyGc.initialized || !gNyGc.enable_nursery)
    return;
  nyGcLock();
  for (size_t i = 0; i < gNyGc.root_count; i++) {
    if (gNyGc.roots[i] == slot) {
      gNyGc.roots[i] = gNyGc.roots[--gNyGc.root_count];
      break;
    }
  }
  nyGcUnlock();
}

/* Write barrier for tenured -> nursery references */
void nyGcWriteBarrier(int64_t *slot, int64_t value) {
  if (!slot)
    return;
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery) {
    *slot = value;
    return;
  }
  nyGcLock();
  if (value) {
    uint8_t *slot_addr = (uint8_t *)slot;
    uint8_t *val_addr = (uint8_t *)(uintptr_t)value;
    bool slot_in_large = false;
    for (nyGcLargeObject_t *large = gNyGc.large_objects; large; large = large->next) {
      uint8_t *start = (uint8_t *)large->header;
      uint8_t *end = start + large->total_size;
      if (slot_addr >= start && slot_addr < end) {
        slot_in_large = true;
        break;
      }
    }
    if (((slot_addr >= gNyGc.tenured_start && slot_addr < gNyGc.tenured_free) || slot_in_large) &&
        val_addr >= gNyGc.nursery_start && val_addr < gNyGc.nursery_ptr) {
      nyGcAddRememberedUnlocked(slot);
    }
  }
  *slot = value;
  nyGcUnlock();
}

/* Add to remembered set */
static void nyGcAddRememberedUnlocked(int64_t *slot) {
  for (size_t i = 0; i < gNyGc.remembered_count; i++) {
    if (gNyGc.remembered_set[i] == slot)
      return;
  }

  if (gNyGc.remembered_count >= gNyGc.remembered_capacity) {
    gNyGc.remembered_capacity *= 2;
    gNyGc.remembered_set =
        (int64_t **)realloc(gNyGc.remembered_set, gNyGc.remembered_capacity * sizeof(int64_t *));
    if (!gNyGc.remembered_set) {
      fprintf(stderr, "GC: remembered set allocation failed\n");
      exit(1);
    }
  }
  gNyGc.remembered_set[gNyGc.remembered_count++] = slot;
}

/* Allocate from nursery (fast path) */
int64_t nyGcAllocFast(size_t size) {
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery) {
    return rt_malloc((int64_t)size);
  }
  nyGcLock();
  int64_t obj = (nyGcAllocSize(size) >= gNyGc.large_threshold)
                    ? nyGcAllocLargeUnlocked(size)
                    : nyGcAllocFastUnlocked(size);
  nyGcUnlock();
  return obj;
}

/* Allocate (with fallback) */
int64_t nyGcAlloc(size_t size) {
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery) {
    return rt_malloc((int64_t)size);
  }
  nyGcLock();
  if (nyGcAllocSize(size) >= gNyGc.large_threshold) {
    int64_t obj = nyGcAllocLargeUnlocked(size);
    nyGcUnlock();
    if (!obj)
      fprintf(stderr, "GC: Out of memory (requested %zu bytes)\n", size);
    return obj;
  }
  int64_t obj = nyGcAllocFastUnlocked(size);
  if (!obj) {
    nyGcMinorCollectUnlocked();
    obj = nyGcAllocFastUnlocked(size);
  }
  if (!obj) {
    obj = nyGcAllocTenuredUnlocked(size);
    if (!obj) {
      nyGcMajorCollectUnlocked();
      obj = nyGcAllocTenuredUnlocked(size);
    }
  }
  nyGcUnlock();
  if (!obj)
    fprintf(stderr, "GC: Out of memory (requested %zu bytes)\n", size);
  return obj;
}

/* Slow path allocation */
int64_t nyGcAllocSlow(size_t size) {
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery) {
    return rt_malloc((int64_t)size);
  }
  nyGcLock();
  if (nyGcAllocSize(size) >= gNyGc.large_threshold) {
    int64_t obj = nyGcAllocLargeUnlocked(size);
    nyGcUnlock();
    if (!obj)
      fprintf(stderr, "GC: Out of memory (requested %zu bytes)\n", size);
    return obj;
  }
  nyGcMinorCollectUnlocked();
  int64_t obj = nyGcAllocFastUnlocked(size);
  if (!obj)
    obj = nyGcAllocTenuredUnlocked(size);
  if (!obj) {
    nyGcMajorCollectUnlocked();
    obj = nyGcAllocTenuredUnlocked(size);
  }
  nyGcUnlock();
  if (!obj)
    fprintf(stderr, "GC: Out of memory (requested %zu bytes)\n", size);
  return obj;
}

/* Trigger minor GC */
void nyGcTriggerMinor(void) {
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery)
    return;
  nyGcLock();
  nyGcMinorCollectUnlocked();
  nyGcUnlock();
}

/* Trigger major GC */
void nyGcTriggerMajor(void) {
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery)
    return;
  nyGcLock();
  nyGcMajorCollectUnlocked();
  nyGcUnlock();
}

/* Full collection */
void nyGcCollect(void) {
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery)
    return;
  nyGcLock();
  nyGcMinorCollectUnlocked();
  nyGcMajorCollectUnlocked();
  nyGcUnlock();
}

/* Minor collection (nursery only) */
static void nyGcMinorCollectUnlocked(void) {
  nyGcValidateUnlocked("minor-before");
  gNyGc.stats.nursery_collections++;
  nyGcClearLargeMarksUnlocked();
  nyGcMark_from_roots();
  nyGcPromoteSurvivors();
  gNyGc.nursery_ptr = gNyGc.nursery_start;
  gNyGc.remembered_count = 0;
  nyGcValidateUnlocked("minor-after");
}

/* Major collection (full) */
static void nyGcMajorCollectUnlocked(void) {
  nyGcValidateUnlocked("major-before");
  gNyGc.stats.tenured_collections++;
  nyGcMinorCollectUnlocked();
  nyGcMark_from_roots();
  nyGcSweepTenured();
  nyGcSweepLargeUnlocked();
  nyGcValidateUnlocked("major-after");
}

/* Mark from all roots */
static void nyGcMark_from_roots(void) {
  for (size_t i = 0; i < gNyGc.root_count; i++) {
    int64_t root = *gNyGc.roots[i];
    if (root)
      nyGcMarkUnlocked(root);
  }

  for (size_t i = 0; i < gNyGc.remembered_count; i++) {
    int64_t ref = *gNyGc.remembered_set[i];
    if (ref)
      nyGcMarkUnlocked(ref);
  }
}

/* Mark object and its children */
static void nyGcMarkUnlocked(int64_t obj) {
  nyGcHeader_t *header = NULL;
  if (!nyGcHeaderForObject(obj, &header))
    return;
  if (header->flags & NYGC_MARKED)
    return;

  header->flags |= NYGC_MARKED;
  int64_t tag = *(int64_t *)((uint8_t *)(uintptr_t)obj - 8);

  if (tag == TAG_LIST || tag == TAG_TUPLE) {
    int64_t len = nyGcTaggedToInt(*(int64_t *)((uint8_t *)(uintptr_t)obj + 0));
    size_t max_items = header->size > 16 ? (header->size - 16) / sizeof(int64_t) : 0;
    if (len < 0)
      len = 0;
    if ((uint64_t)len > max_items)
      len = (int64_t)max_items;
    int64_t *items = (int64_t *)((uint8_t *)(uintptr_t)obj + 16);
    for (int64_t i = 0; i < len; i++)
      nyGcMarkSlot(items[i]);
  } else if (tag == TAG_DICT) {
    int64_t cap = nyGcTaggedToInt(*(int64_t *)((uint8_t *)(uintptr_t)obj + 8));
    size_t max_slots = header->size > 16 ? (header->size - 16) / 24 : 0;
    if (cap < 0)
      cap = 0;
    if ((uint64_t)cap > max_slots)
      cap = (int64_t)max_slots;
    for (int64_t i = 0; i < cap; i++) {
      uint8_t *slot = (uint8_t *)(uintptr_t)obj + 16 + (size_t)i * 24;
      int64_t state = *(int64_t *)(slot + 16);
      if (state != 1 && state != rt_tag_v(1))
        continue;
      nyGcMarkSlot(*(int64_t *)slot);
      nyGcMarkSlot(*(int64_t *)(slot + 8));
    }
  } else if (tag == TAG_OK || tag == TAG_ERR) {
    if (header->size >= 8)
      nyGcMarkSlot(*(int64_t *)((uint8_t *)(uintptr_t)obj + 0));
    if (header->size >= 16)
      nyGcMarkSlot(*(int64_t *)((uint8_t *)(uintptr_t)obj + 8));
  } else if (tag == TAG_CLOSURE) {
    if (header->size >= 16)
      nyGcMarkSlot(*(int64_t *)((uint8_t *)(uintptr_t)obj + 8));
    if (header->size >= 24)
      nyGcMarkSlot(*(int64_t *)((uint8_t *)(uintptr_t)obj + 16));
  }
}

void nyGcMark(int64_t obj) {
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery)
    return;
  nyGcLock();
  nyGcMarkUnlocked(obj);
  nyGcUnlock();
}

/* Check if object is marked */
bool nyGcIsMarked(int64_t obj) {
  bool marked = false;
  if (!gNyGc.initialized || !gNyGc.enable_nursery)
    return false;
  nyGcLock();
  nyGcHeader_t *header = NULL;
  if (nyGcHeaderForObject(obj, &header))
    marked = (header->flags & NYGC_MARKED) != 0;
  nyGcUnlock();
  return marked;
}

/* Promote nursery survivors to tenured */
static void nyGcPromoteSurvivors(void) {
  nyGcForwardMap_t forwards = {0};
  uint8_t *ptr = gNyGc.nursery_start;

  while (ptr < gNyGc.nursery_ptr) {
    nyGcHeader_t *header = (nyGcHeader_t *)ptr;
    size_t total = nyGcAllocSize(header->size);
    if (header->flags & NYGC_MARKED) {
      if (gNyGc.tenured_free + total <= gNyGc.tenured_limit) {
        nyGcHeader_t *dst = (nyGcHeader_t *)gNyGc.tenured_free;
        memcpy(dst, header, total);
        dst->flags = (dst->flags | NYGC_TENURED) & ~NYGC_MARKED;
        dst->age = header->age + 1;
        gNyGc.tenured_free += total;
        gNyGc.stats.tenured_allocated += total;
        gNyGc.stats.objects_promoted++;
        rt_heap_ptr_cache_store((uintptr_t)nyGcObjPtr(dst));
        if (!nyGcForwardPush(&forwards, (int64_t)(uintptr_t)nyGcObjPtr(header),
                             (int64_t)(uintptr_t)nyGcObjPtr(dst))) {
          free(forwards.data);
          fprintf(stderr, "GC: promotion map allocation failed\n");
          exit(1);
        }
      } else {
        fprintf(stderr, "GC: Out of tenured space while promoting survivor\n");
      }
    } else {
      gNyGc.stats.objects_swept++;
      gNyGc.stats.bytes_freed += header->size;
    }
    ptr += total;
  }

  if (forwards.count)
    nyGcApplyForwards(&forwards);
  free(forwards.data);
}

/* Sweep tenured */
static void nyGcSweepTenured(void) {
  nyGcForwardMap_t forwards = {0};
  uint8_t *ptr = gNyGc.tenured_start;
  uint8_t *new_free = gNyGc.tenured_start;
  uint8_t *old_free = gNyGc.tenured_free;

  while (ptr < old_free) {
    nyGcHeader_t *header = (nyGcHeader_t *)ptr;
    size_t total = nyGcAllocSize(header->size);
    if (header->flags & NYGC_MARKED) {
      int64_t from = (int64_t)(uintptr_t)nyGcObjPtr(header);
      int64_t to = (int64_t)(uintptr_t)(new_free + NYGC_OBJECT_DATA_OFFSET);
      if (!nyGcForwardPush(&forwards, from, to)) {
        free(forwards.data);
        fprintf(stderr, "GC: tenured compaction map allocation failed\n");
        exit(1);
      }
      new_free += total;
    } else {
      gNyGc.stats.objects_swept++;
      gNyGc.stats.bytes_freed += header->size;
    }
    ptr += total;
  }

  for (size_t i = 0; i < forwards.count; i++) {
    nyGcHeader_t *src = nyGcHeaderFromObject(forwards.data[i].from);
    nyGcHeader_t *dst = nyGcHeaderFromObject(forwards.data[i].to);
    size_t total = nyGcAllocSize(src->size);
    memmove(dst, src, total);
    dst->flags = (dst->flags | NYGC_TENURED) & ~NYGC_MARKED;
    rt_heap_ptr_cache_store((uintptr_t)nyGcObjPtr(dst));
  }

  gNyGc.tenured_free = new_free;
  gNyGc.tenured_scan = gNyGc.tenured_start;
  if (old_free > new_free)
    memset(new_free, 0, (size_t)(old_free - new_free));
  if (forwards.count)
    nyGcApplyForwards(&forwards);
  free(forwards.data);
}

static void nyGcClearLargeMarksUnlocked(void) {
  for (nyGcLargeObject_t *large = gNyGc.large_objects; large; large = large->next) {
    if (large->header)
      large->header->flags &= ~NYGC_MARKED;
  }
}

static void nyGcSweepLargeUnlocked(void) {
  nyGcLargeObject_t **link = &gNyGc.large_objects;
  while (*link) {
    nyGcLargeObject_t *large = *link;
    if (!large->header || !(large->header->flags & NYGC_MARKED)) {
      *link = large->next;
      if (large->header) {
        gNyGc.stats.objects_swept++;
        gNyGc.stats.bytes_freed += large->header->size;
        gNyGc.stats.large_freed += large->total_size;
      }
      if (gNyGc.large_count > 0)
        gNyGc.large_count--;
      free(large->header);
      free(large);
      continue;
    }
    large->header->flags &= ~NYGC_MARKED;
    link = &large->next;
  }
}

/* Set finalizer */
void nyGcSetFinalizer(int64_t obj, void (*finalizer)(int64_t)) {
  if (!gNyGc.initialized)
    nyGcInit();
  if (!gNyGc.enable_nursery)
    return;
  nyGcLock();
  nyGcHeader_t *header = NULL;
  if (nyGcHeaderForObject(obj, &header)) {
    if (finalizer)
      header->flags |= NYGC_FINALIZER;
    else
      header->flags &= ~NYGC_FINALIZER;
  }
  nyGcUnlock();
}

/* Get heap usage */
size_t nyGcGetHeapUsage(void) {
  size_t usage = 0;
  nyGcLock();
  if (gNyGc.initialized && gNyGc.enable_nursery)
    usage = (size_t)(gNyGc.nursery_ptr - gNyGc.nursery_start) +
            (size_t)(gNyGc.tenured_free - gNyGc.tenured_start);
  for (nyGcLargeObject_t *large = gNyGc.large_objects; large; large = large->next)
    usage += large->total_size;
  nyGcUnlock();
  return usage;
}

/* Dump statistics */
void nyGcDumpStats(FILE *out) {
  if (!out)
    out = stderr;

  nyGcLock();
  fprintf(out, "\n=== GC Statistics ===\n");
  fprintf(out, "Enabled:               %s\n", gNyGc.enable_nursery ? "yes" : "no");
  fprintf(out, "Nursery capacity:      %zu bytes\n", gNyGc.nursery_capacity);
  fprintf(out, "Tenured capacity:      %zu bytes\n", gNyGc.tenured_capacity);
  fprintf(out, "Nursery allocated:     %zu bytes\n", gNyGc.stats.nursery_allocated);
  fprintf(out, "Tenured allocated:     %zu bytes\n", gNyGc.stats.tenured_allocated);
  fprintf(out, "Large allocated:       %zu bytes\n", gNyGc.stats.large_allocated);
  fprintf(out, "Large freed:           %zu bytes\n", gNyGc.stats.large_freed);
  fprintf(out, "Large objects:         %zu\n", gNyGc.large_count);
  fprintf(out, "Large threshold:       %zu bytes\n", gNyGc.large_threshold);
  fprintf(out, "Minor collections:     %zu\n", gNyGc.stats.nursery_collections);
  fprintf(out, "Major collections:     %zu\n", gNyGc.stats.tenured_collections);
  fprintf(out, "Objects promoted:      %zu\n", gNyGc.stats.objects_promoted);
  fprintf(out, "Objects swept:         %zu\n", gNyGc.stats.objects_swept);
  fprintf(out, "Bytes freed:           %zu\n", gNyGc.stats.bytes_freed);
  size_t heap_usage =
      (gNyGc.initialized && gNyGc.enable_nursery)
          ? (size_t)(gNyGc.nursery_ptr - gNyGc.nursery_start) +
                (size_t)(gNyGc.tenured_free - gNyGc.tenured_start)
          : 0u;
  for (nyGcLargeObject_t *large = gNyGc.large_objects; large; large = large->next)
    heap_usage += large->total_size;
  fprintf(out, "Heap usage:            %zu bytes\n", heap_usage);
  fprintf(out, "======================\n\n");
  nyGcUnlock();
}
