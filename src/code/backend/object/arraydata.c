#include "code/backend/internal.h"
#include <nyr/layout.h>

#include <stdlib.h>
#include <string.h>

extern int64_t rt_tbuf_register_static_handle(int64_t value)
    __attribute__((weak));

void *nyr_arraytab_data(const char *name) {
  if (!name || !rt_tbuf_register_static_handle)
    return NULL;
  for (size_t i = 0; i < nyr_arraytab_len; ++i) {
    if (strcmp(nyr_arraytab[i].name, name) != 0)
      continue;
    ny_arraytab_ent_t *entry = &nyr_arraytab[i];
    if (!entry->data_ptr) {
      size_t total = 32 + entry->count * entry->stride;
      uint8_t *raw = calloc(1, total);
      if (!raw)
        return NULL;
      int64_t *header = (int64_t *)raw;
      header[0] = (int64_t)NY_NATIVE_TBUF_MAGIC;
      header[1] = (int64_t)entry->count;
      header[2] = (int64_t)entry->stride;
      header[3] = (int64_t)entry->count;
      uint8_t *data = raw + 32;
      for (size_t k = 0; k < entry->count; ++k) {
        if (entry->stride == 8) {
          ((int64_t *)data)[k] = entry->values[k].value;
        } else if (entry->stride == 24) {
          int64_t *slot = (int64_t *)(data + k * 24);
          slot[0] = entry->values[k].value;
          slot[1] = 0;
          slot[2] = entry->values[k].tag;
        }
      }
      entry->data_ptr = raw;
      (void)rt_tbuf_register_static_handle((int64_t)(uintptr_t)(raw + 32));
    }
    return (void *)((uintptr_t)entry->data_ptr + 32);
  }
  return NULL;
}
