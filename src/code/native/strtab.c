#include "code/native/internal.h"
#include "code/native/object/internal.h"
#include "base/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Session-local C-string pool for the pure native path. Strings are interned
 * during NYIR lower as .Lnystr.N symbols and emitted into the code blob after
 * functions so LEA/PC32 can resolve them without a separate .rodata section.
 *
 * Lookup is indexed by an open-addressing hash table keyed on (len, hash) so a
 * dedup hit is O(1) average instead of a linear scan over all prior entries. */

enum { NY_STRTAB_MAX = 256, NY_STRTAB_NAME = 32, NY_STRTAB_HASH_CAP = 512 };

typedef struct {
  char name[NY_STRTAB_NAME];
  char *bytes;
  size_t len;
} ny_strtab_ent_t;

static ny_strtab_ent_t ny_strtab[NY_STRTAB_MAX];
static size_t ny_strtab_len = 0;
/* Maps each hash slot to a 1-based index into ny_strtab (0 = empty).  Sized to
 * a power of two above NY_STRTAB_MAX so the table never needs to grow and stays
 * below a 0.5 load factor at full capacity. */
static uint16_t ny_strtab_hash[NY_STRTAB_HASH_CAP];

static inline size_t ny_strtab_slot(size_t len, uint64_t hash) {
  return (size_t)((hash ^ ((uint64_t)len * 0x9E3779B97F4A7C15ULL)) &
                  (NY_STRTAB_HASH_CAP - 1u));
}

void ny_native_strtab_clear(void) {
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    free(ny_strtab[i].bytes);
    ny_strtab[i].bytes = NULL;
    ny_strtab[i].len = 0;
    ny_strtab[i].name[0] = '\0';
  }
  ny_strtab_len = 0;
  memset(ny_strtab_hash, 0, sizeof(ny_strtab_hash));
}

const char *ny_native_strtab_intern(const char *s, size_t len, char *name_out,
                                    size_t name_cap) {
  if (!s)
    s = "";
  if (len == (size_t)-1)
    len = strlen(s);
  uint64_t hash = ny_hash64(s, len);
  size_t mask = NY_STRTAB_HASH_CAP - 1u;
  size_t idx = ny_strtab_slot(len, hash);
  for (;;) {
    uint16_t existing = ny_strtab_hash[idx];
    if (existing == 0)
      break;
    const ny_strtab_ent_t *e = &ny_strtab[existing - 1u];
    if (e->len == len && memcmp(e->bytes, s, len) == 0) {
      if (name_out && name_cap)
        snprintf(name_out, name_cap, "%s", e->name);
      return e->name;
    }
    idx = (idx + 1u) & mask;
  }
  if (ny_strtab_len >= NY_STRTAB_MAX)
    return NULL;
  char *copy = (char *)malloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, s, len);
  copy[len] = '\0';
  ny_strtab_ent_t *e = &ny_strtab[ny_strtab_len];
  snprintf(e->name, sizeof(e->name), ".Lnystr.%zu", ny_strtab_len);
  e->bytes = copy;
  e->len = len;
  /* ny_strtab_hash was advanced to the first empty slot during the dedup
   * probe, so install the new entry (1-based) there. */
  ny_strtab_hash[idx] = (uint16_t)(ny_strtab_len + 1u);
  ny_strtab_len++;
  if (name_out && name_cap)
    snprintf(name_out, name_cap, "%s", e->name);
  return e->name;
}

bool ny_native_strtab_append_defs(ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
                                  size_t *def_count, char *err, size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    if (*def_count >= 256) {
      ny_native_set_err(err, err_len, "native strtab: too many symbols");
      return false;
    }
    /* 4-byte align for clean LEA targets. */
    while (code->len & 3u) {
      if (!ny_obj_u8(code, 0)) {
        ny_native_set_err(err, err_len, "native strtab: out of memory");
        return false;
      }
    }
    size_t off = code->len;
    if (!ny_obj_emit(code, ny_strtab[i].bytes, ny_strtab[i].len + 1)) {
      ny_native_set_err(err, err_len, "native strtab: out of memory");
      return false;
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             ny_strtab[i].name);
    defs[*def_count].off = off;
    defs[*def_count].size = ny_strtab[i].len + 1;
    (*def_count)++;
  }
  return true;
}
