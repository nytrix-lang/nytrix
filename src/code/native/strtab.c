#include "code/native/internal.h"
#include "code/native/object/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Session-local C-string pool for the pure native path. Strings are interned
 * during NYIR lower as .Lnystr.N symbols and emitted into the code blob after
 * functions so LEA/PC32 can resolve them without a separate .rodata section. */

enum { NY_STRTAB_MAX = 256, NY_STRTAB_NAME = 32 };

typedef struct {
  char name[NY_STRTAB_NAME];
  char *bytes;
  size_t len;
} ny_strtab_ent_t;

static ny_strtab_ent_t ny_strtab[NY_STRTAB_MAX];
static size_t ny_strtab_len = 0;

void ny_native_strtab_clear(void) {
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    free(ny_strtab[i].bytes);
    ny_strtab[i].bytes = NULL;
    ny_strtab[i].len = 0;
    ny_strtab[i].name[0] = '\0';
  }
  ny_strtab_len = 0;
}

const char *ny_native_strtab_intern(const char *s, size_t len, char *name_out,
                                    size_t name_cap) {
  if (!s)
    s = "";
  if (len == (size_t)-1)
    len = strlen(s);
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    if (ny_strtab[i].len == len &&
        memcmp(ny_strtab[i].bytes, s, len) == 0) {
      if (name_out && name_cap)
        snprintf(name_out, name_cap, "%s", ny_strtab[i].name);
      return ny_strtab[i].name;
    }
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
