/*
 * Native string table: interned-symbol storage for ELF/Mach-O object
 * files, mapping symbol names to section-relative offsets.
 */
#include "code/native/internal.h"
#include "code/native/object/internal.h"
#include "base/util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Session-local C-string pool for the pure native path. Strings are interned
 * during NYIR lower as ny_str_N symbols and emitted into the code blob after
 * functions so LEA/PC32 can resolve them without a separate .rodata section.
 * Each symbol points at the bytes, preceded by the standard 16-byte Nytrix
 * string header.  This keeps the same address usable by C-string helpers and
 * by runtime diagnostics/type inspection.
 *
 * Lookup is indexed by an open-addressing hash table keyed on (len, hash) so a
 * dedup hit is O(1) average instead of a linear scan over all prior entries.
 */

enum { NY_STRTAB_NAME = 32 };

typedef struct {
  char name[NY_STRTAB_NAME];
  char *bytes;
  size_t len;
} ny_strtab_ent_t;

static ny_strtab_ent_t *ny_strtab;
static size_t ny_strtab_len;
static size_t ny_strtab_cap;
static uint32_t *ny_strtab_hash;
static size_t ny_strtab_hash_cap;

static inline size_t ny_strtab_slot(size_t len, uint64_t hash) {
  return (size_t)((hash ^ ((uint64_t)len * 0x9E3779B97F4A7C15ULL)) &
                  (ny_strtab_hash_cap - 1u));
}

static bool ny_strtab_reserve(size_t want) {
  if (want <= ny_strtab_cap)
    return true;
  size_t cap = ny_strtab_cap ? ny_strtab_cap : 64;
  while (cap < want) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  ny_strtab_ent_t *entries =
      ny_realloc_array(ny_strtab, cap, sizeof(*entries));
  if (!entries)
    return false;
  ny_strtab = entries;
  ny_strtab_cap = cap;
  return true;
}

static bool ny_strtab_rehash(size_t want_cap) {
  if (ny_strtab_hash && want_cap <= SIZE_MAX / 2u &&
      ny_strtab_hash_cap >= want_cap * 2u)
    return true;
  size_t cap = ny_strtab_hash_cap ? ny_strtab_hash_cap : 128;
  while (cap < want_cap * 2u) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  uint32_t *hash = ny_calloc_array(cap, sizeof(*hash));
  if (!hash)
    return false;
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    uint64_t h = ny_hash64(ny_strtab[i].bytes, ny_strtab[i].len);
    size_t slot = (size_t)((h ^ ((uint64_t)ny_strtab[i].len *
                                 0x9E3779B97F4A7C15ULL)) &
                           (cap - 1u));
    while (hash[slot])
      slot = (slot + 1u) & (cap - 1u);
    hash[slot] = (uint32_t)(i + 1u);
  }
  free(ny_strtab_hash);
  ny_strtab_hash = hash;
  ny_strtab_hash_cap = cap;
  return true;
}

void ny_native_strtab_clear(void) {
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    free(ny_strtab[i].bytes);
    ny_strtab[i].bytes = NULL;
    ny_strtab[i].len = 0;
    ny_strtab[i].name[0] = '\0';
  }
  free(ny_strtab);
  free(ny_strtab_hash);
  ny_strtab = NULL;
  ny_strtab_hash = NULL;
  ny_strtab_len = 0;
  ny_strtab_cap = 0;
  ny_strtab_hash_cap = 0;
}

const char *ny_native_strtab_intern(const char *s, size_t len, char *name_out,
                                    size_t name_cap) {
  if (!s)
    s = "";
  if (len == (size_t)-1)
    len = strlen(s);
  if (len == SIZE_MAX || !ny_strtab_rehash(ny_strtab_len + 1u) ||
      !ny_strtab_reserve(ny_strtab_len + 1u))
    return NULL;
  uint64_t hash = ny_hash64(s, len);
  size_t mask = ny_strtab_hash_cap - 1u;
  size_t idx = ny_strtab_slot(len, hash);
  for (;;) {
    uint32_t existing = ny_strtab_hash[idx];
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
  char *copy = (char *)malloc(len + 1u);
  if (!copy)
    return NULL;
  memcpy(copy, s, len);
  copy[len] = '\0';
  ny_strtab_ent_t *e = &ny_strtab[ny_strtab_len];
  snprintf(e->name, sizeof(e->name), "ny_str_%zu", ny_strtab_len);
  e->bytes = copy;
  e->len = len;
  ny_strtab_hash[idx] = (uint32_t)(ny_strtab_len + 1u);
  ny_strtab_len++;
  if (name_out && name_cap)
    snprintf(name_out, name_cap, "%s", e->name);
  return e->name;
}

const char *ny_native_strtab_get(const char *name, size_t *len_out) {
  if (!name || !name[0])
    return NULL;
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    if (strcmp(ny_strtab[i].name, name) == 0) {
      if (len_out)
        *len_out = ny_strtab[i].len;
      return ny_strtab[i].bytes;
    }
  }
  return NULL;
}

bool ny_native_strtab_append_defs(ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
                                  size_t *def_count, char *err, size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    if (ny_x64_obj_def_index(defs, *def_count, ny_strtab[i].name) >= 0)
      continue;
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      ny_native_set_err(err, err_len, "native strtab: too many symbols");
      return false;
    }
    /*
     * 8-byte align for clean LEA targets and 8-byte aligned pointer tagging.
     */
    while (code->len & 7u) {
      if (!ny_obj_u8(code, 0)) {
        ny_native_set_err(err, err_len, "native strtab: out of memory");
        return false;
      }
    }
    if (!ny_obj_u64(code, (uint64_t)(ny_strtab[i].len * 2u + 1u)) ||
        !ny_obj_u64(code, 121u)) {
      ny_native_set_err(err, err_len, "native strtab: out of memory");
      return false;
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
    defs[*def_count].is_data = true;
    (*def_count)++;
  }
  return true;
}

/*
 * Session-local pool of foldable top-level `def` constants referenced as
 * object symbols. The table grows geometrically; the name width remains a
 * symbol-format sanity bound.
 */
enum { NY_CONSTTAB_NAME = 256 };

typedef struct {
  char name[NY_CONSTTAB_NAME];
  int64_t value;
} ny_consttab_ent_t;

static ny_consttab_ent_t *ny_consttab;
static size_t ny_consttab_len;
static size_t ny_consttab_cap;

static bool ny_consttab_reserve(size_t want) {
  if (want <= ny_consttab_cap)
    return true;
  size_t cap = ny_consttab_cap ? ny_consttab_cap : 64;
  while (cap < want) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  ny_consttab_ent_t *entries =
      ny_realloc_array(ny_consttab, cap, sizeof(*entries));
  if (!entries)
    return false;
  ny_consttab = entries;
  ny_consttab_cap = cap;
  return true;
}

void ny_native_consttab_clear(void) {
  free(ny_consttab);
  ny_consttab = NULL;
  ny_consttab_len = 0;
  ny_consttab_cap = 0;
}

bool ny_native_consttab_add(const char *name, int64_t value) {
  if (!name || !name[0])
    return false;
  if (strlen(name) >= NY_CONSTTAB_NAME) {
    fprintf(stderr, "native consttab: symbol name exceeds %u bytes: %s\n",
            NY_CONSTTAB_NAME - 1u, name);
    return false;
  }
  for (size_t i = 0; i < ny_consttab_len; ++i) {
    if (strcmp(ny_consttab[i].name, name) == 0)
      return true; /* already registered — keep first value */
  }
  if (!ny_consttab_reserve(ny_consttab_len + 1u))
    return false;
  snprintf(ny_consttab[ny_consttab_len].name,
           sizeof(ny_consttab[ny_consttab_len].name), "%s", name);
  ny_consttab[ny_consttab_len].value = value;
  ny_consttab_len++;
  return true;
}

bool ny_native_consttab_has(const char *name) {
  if (!name)
    return false;
  for (size_t i = 0; i < ny_consttab_len; ++i)
    if (strcmp(ny_consttab[i].name, name) == 0)
      return true;
  return false;
}
bool ny_native_consttab_get(const char *name, int64_t *value) {
  if (!name || !value)
    return false;
  for (size_t i = 0; i < ny_consttab_len; ++i) {
    if (strcmp(ny_consttab[i].name, name) == 0) {
      *value = ny_consttab[i].value;
      return true;
    }
  }
  return false;
}

bool ny_native_consttab_get_tail(const char *name, int64_t *value) {
  if (!name || !*name || !value)
    return false;
  const ny_consttab_ent_t *match = NULL;
  size_t name_len = strlen(name);
  for (size_t i = 0; i < ny_consttab_len; ++i) {
    const char *candidate = ny_consttab[i].name;
    size_t candidate_len = strlen(candidate);
    if (candidate_len < name_len ||
        strcmp(candidate + candidate_len - name_len, name) != 0 ||
        (candidate_len > name_len &&
         candidate[candidate_len - name_len - 1] != '.'))
      continue;
    if (match && match->value != ny_consttab[i].value)
      return false;
    match = &ny_consttab[i];
  }
  if (!match)
    return false;
  *value = match->value;
  return true;
}

bool ny_native_consttab_append_defs(ny_obj_buf_t *code,
                                    ny_x64_obj_symbol_def_t *defs,
                                    size_t *def_count, char *err,
                                    size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < ny_consttab_len; ++i) {
    if (ny_x64_obj_def_index(defs, *def_count, ny_consttab[i].name) >= 0)
      continue;
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      ny_native_set_err(err, err_len, "native consttab: too many symbols");
      return false;
    }
    /*
     * 8-byte align for clean .data values.
     */
    while (code->len & 7u) {
      if (!ny_obj_u8(code, 0)) {
        ny_native_set_err(err, err_len, "native consttab: out of memory");
        return false;
      }
    }
    size_t off = code->len;
    uint64_t raw = (uint64_t)ny_consttab[i].value;
    for (int b = 0; b < 8; ++b) {
      unsigned char byte = (unsigned char)((raw >> (b * 8)) & 0xff);
      if (!ny_obj_u8(code, byte)) {
        ny_native_set_err(err, err_len, "native consttab: out of memory");
        return false;
      }
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             ny_consttab[i].name);
    defs[*def_count].off = off;
    defs[*def_count].size = 8;
    defs[*def_count].is_data = true;
    (*def_count)++;
  }
  return true;
}



typedef struct {
  char name[NY_CONSTTAB_NAME];
} ny_globaltab_ent_t;

static ny_globaltab_ent_t *ny_globaltab;
static size_t ny_globaltab_len;
static size_t ny_globaltab_cap;

void ny_native_globaltab_clear(void) {
  free(ny_globaltab);
  ny_globaltab = NULL;
  ny_globaltab_len = 0;
  ny_globaltab_cap = 0;
}

bool ny_native_globaltab_add(const char *name) {
  if (!name || !name[0] || strlen(name) >= NY_CONSTTAB_NAME)
    return false;
  for (size_t i = 0; i < ny_globaltab_len; ++i)
    if (strcmp(ny_globaltab[i].name, name) == 0)
      return true;
  if (ny_globaltab_len == ny_globaltab_cap) {
    size_t cap = ny_globaltab_cap ? ny_globaltab_cap * 2u : 64u;
    if (cap < ny_globaltab_cap)
      return false;
    ny_globaltab_ent_t *entries =
        ny_realloc_array(ny_globaltab, cap, sizeof(*entries));
    if (!entries)
      return false;
    ny_globaltab = entries;
    ny_globaltab_cap = cap;
  }
  snprintf(ny_globaltab[ny_globaltab_len].name,
           sizeof(ny_globaltab[ny_globaltab_len].name), "%s", name);
  ++ny_globaltab_len;
  return true;
}

const char *ny_native_globaltab_name(const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < ny_globaltab_len; ++i)
    if (strcmp(ny_globaltab[i].name, name) == 0)
      return ny_globaltab[i].name;
  return NULL;
}
const char *ny_native_globaltab_name_tail(const char *name) {
  if (!name || !*name)
    return NULL;
  const char *match = NULL;
  for (size_t i = 0; i < ny_globaltab_len; ++i) {
    const char *dot = strrchr(ny_globaltab[i].name, '.');
    const char *tail = dot ? dot + 1 : ny_globaltab[i].name;
    if (strcmp(tail, name) != 0)
      continue;
    if (!match)
      match = ny_globaltab[i].name;
  }
  return match;
}


bool ny_native_globaltab_has(const char *name) {
  return ny_native_globaltab_name(name) != NULL;
}
bool ny_native_globaltab_append_defs(ny_obj_buf_t *code,
                                     ny_x64_obj_symbol_def_t *defs,
                                     size_t *def_count, char *err,
                                     size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < ny_globaltab_len; ++i) {
    /*
     * A symbol may also have been discovered as a foldable top-level value.
     * Emit exactly one definition: strict system linkers reject duplicate
     * globals even when both definitions come from the same object.
     */
    if (ny_x64_obj_def_index(defs, *def_count, ny_globaltab[i].name) >= 0)
      continue;
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      ny_native_set_err(err, err_len, "native globaltab: too many symbols");
      return false;
    }
    while (code->len & 7u)
      if (!ny_obj_u8(code, 0))
        return ny_native_set_err(err, err_len,
                                 "native globaltab: out of memory"), false;
    size_t off = code->len;
    for (int b = 0; b < 8; ++b)
      if (!ny_obj_u8(code, 0))
        return ny_native_set_err(err, err_len,
                                 "native globaltab: out of memory"), false;
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             ny_globaltab[i].name);
    defs[*def_count].off = off;
    defs[*def_count].size = 8;
    defs[*def_count].is_data = true;
    ++*def_count;
  }
  return true;
}


/*
 * Session-local pool of constant arrays (list literals with all-constant
 * elements). NY_E_LIST lowering interns the element values here and emits a
 * NYIR_ADDR_SYMBOL to the generated .Lnyarr.N symbol; the object emitters
 * write a 24-byte tbuf header followed by count * stride bytes of .data.
 */
enum { NY_ARRAYTAB_NAME = 32, NY_ARRAYTAB_MAX_ELEMS = 65536 };
typedef struct {
  char name[NY_ARRAYTAB_NAME];
  ny_native_array_elem_t *values;
  char **strings;
  size_t count;
  size_t stride;
} ny_arraytab_ent_t;

static ny_arraytab_ent_t *ny_arraytab;
static size_t ny_arraytab_len;
static size_t ny_arraytab_cap;

static bool ny_arraytab_reserve(size_t want) {
  if (want <= ny_arraytab_cap)
    return true;
  size_t cap = ny_arraytab_cap ? ny_arraytab_cap : 32;
  while (cap < want) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  ny_arraytab_ent_t *entries =
      ny_realloc_array(ny_arraytab, cap, sizeof(*entries));
  if (!entries)
    return false;
  memset(entries + ny_arraytab_cap, 0,
         (cap - ny_arraytab_cap) * sizeof(*entries));
  ny_arraytab = entries;
  ny_arraytab_cap = cap;
  return true;
}

void ny_native_arraytab_clear(void) {
  for (size_t i = 0; i < ny_arraytab_len; ++i) {
    for (size_t k = 0; k < ny_arraytab[i].count; ++k)
      free(ny_arraytab[i].strings[k]);
    free(ny_arraytab[i].strings);
    free(ny_arraytab[i].values);
  }
  free(ny_arraytab);
  ny_arraytab = NULL;
  ny_arraytab_len = 0;
  ny_arraytab_cap = 0;
}

const char *ny_native_arraytab_intern(const ny_native_array_elem_t *v,
                                      size_t n, size_t stride, char *out,
                                      size_t cap) {
  if (!v || !n || n > NY_ARRAYTAB_MAX_ELEMS ||
      (stride != 8 && stride != 24))
    return NULL;
  for (size_t i = 0; i < ny_arraytab_len; ++i) {
    bool equal = ny_arraytab[i].count == n && ny_arraytab[i].stride == stride;
    for (size_t k = 0; equal && k < n; ++k) {
      equal = ny_arraytab[i].values[k].value == v[k].value &&
              ny_arraytab[i].values[k].tag == v[k].tag &&
              ny_arraytab[i].values[k].str_len == v[k].str_len;
      if (equal && v[k].str_len)
        equal = memcmp(ny_arraytab[i].strings[k], v[k].str, v[k].str_len) == 0;
    }
    if (equal) {
      if (out && cap)
        snprintf(out, cap, "%s", ny_arraytab[i].name);
      return ny_arraytab[i].name;
    }
  }
  if (!ny_arraytab_reserve(ny_arraytab_len + 1u))
    return NULL;
  ny_arraytab_ent_t *e = &ny_arraytab[ny_arraytab_len];
  e->values = calloc(n, sizeof(*e->values));
  e->strings = calloc(n, sizeof(*e->strings));
  if (!e->values || !e->strings) {
    free(e->strings);
    free(e->values);
    memset(e, 0, sizeof(*e));
    return NULL;
  }
  memcpy(e->values, v, n * sizeof(*e->values));
  snprintf(e->name, sizeof(e->name), ".Lnyarr.%zu", ny_arraytab_len);
  e->count = n;
  e->stride = stride;
  for (size_t k = 0; k < n; ++k) {
    if (!v[k].str)
      continue;
    if (v[k].str_len == SIZE_MAX) {
      for (size_t j = 0; j < k; ++j)
        free(e->strings[j]);
      free(e->strings);
      free(e->values);
      memset(e, 0, sizeof(*e));
      return NULL;
    }
    e->strings[k] = malloc(v[k].str_len + 1);
    if (!e->strings[k]) {
      for (size_t j = 0; j < k; ++j)
        free(e->strings[j]);
      free(e->strings);
      free(e->values);
      memset(e, 0, sizeof(*e));
      return NULL;
    }
    memcpy(e->strings[k], v[k].str, v[k].str_len);
    e->strings[k][v[k].str_len] = '\0';
    e->values[k].str = e->strings[k];
  }
  ++ny_arraytab_len;
  if (out && cap)
    snprintf(out, cap, "%s", e->name);
  return e->name;
}

bool ny_native_arraytab_get(const char *name,
                            const ny_native_array_elem_t **elems_out,
                            size_t *count_out, size_t *stride_out) {
  if (!name)
    return false;
  for (size_t i = 0; i < ny_arraytab_len; ++i) {
    if (strcmp(ny_arraytab[i].name, name) == 0) {
      if (elems_out)
        *elems_out = ny_arraytab[i].values;
      if (count_out)
        *count_out = ny_arraytab[i].count;
      if (stride_out)
        *stride_out = ny_arraytab[i].stride;
      return true;
    }
  }
  return false;
}

bool ny_native_arraytab_append_defs(ny_obj_buf_t *code,
                                    ny_x64_obj_symbol_def_t *defs,
                                    size_t *def_count, char *err,
                                    size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < ny_arraytab_len; ++i) {
    ny_arraytab_ent_t *e = &ny_arraytab[i];
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      ny_native_set_err(err, err_len, "native arraytab: too many symbols");
      return false;
    }
    while (code->len & 7u)
      if (!ny_obj_u8(code, 0))
        goto oom;
    for (int b = 0; b < 8; ++b)
      if (!ny_obj_u8(code, (unsigned char)(((uint64_t)e->count >> (b * 8)) & 255)))
        goto oom;
    for (int b = 0; b < 8; ++b)
      if (!ny_obj_u8(code, (unsigned char)(((uint64_t)e->stride >> (b * 8)) & 255)))
        goto oom;
    for (int b = 0; b < 8; ++b)
      if (!ny_obj_u8(code, (unsigned char)(((uint64_t)e->count >> (b * 8)) & 255)))
        goto oom;

    size_t off = code->len;
    size_t string_base = e->count * e->stride;
    size_t string_cursor = 0;
    for (size_t k = 0; k < e->count; ++k) {
      int64_t words[3] = {e->values[k].value, 0, e->values[k].tag};
      if (e->stride == 24 && e->values[k].str) {
        words[0] = (int64_t)(string_base + string_cursor - k * 24);
        words[1] = (int64_t)e->values[k].str_len;
        words[2] = 121;
      }
      for (size_t word = 0; word < e->stride / 8; ++word) {
        uint64_t raw = (uint64_t)words[word];
        for (int b = 0; b < 8; ++b)
          if (!ny_obj_u8(code, (unsigned char)((raw >> (b * 8)) & 255)))
            goto oom;
      }
      if (e->stride == 24 && e->values[k].str)
        string_cursor += e->values[k].str_len + 1;
    }
    if (e->stride == 24) {
      for (size_t k = 0; k < e->count; ++k) {
        if (e->values[k].str &&
            !ny_obj_emit(code, e->strings[k], e->values[k].str_len + 1))
          goto oom;
      }
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             e->name);
    defs[*def_count].off = off;
    defs[*def_count].size = code->len - off;
    defs[*def_count].is_data = true;
    ++(*def_count);
  }
  return true;

oom:
  ny_native_set_err(err, err_len, "native arraytab: out of memory");
  return false;
}

bool ny_native_strtab_append_asm(ny_native_writer_t *w, char *err,
                                 size_t err_len) {
  if (!w)
    return false;
  if (!ny_native_put(w, "\t.section\t.rodata\n\t.p2align\t2\n"))
    return false;
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    if (!ny_native_printf(w, "\t.globl\t%s\n%s:\n", ny_strtab[i].name,
                          ny_strtab[i].name))
      return false;
    for (size_t j = 0; j < ny_strtab[i].len + 1; ++j)
      if (!ny_native_printf(w, "\t.byte\t%u\n",
                            (unsigned char)ny_strtab[i].bytes[j]))
        return false;
  }
  if (!ny_native_put(w, "\t.text\n"))
    return false;
  (void)err;
  (void)err_len;
  return true;
}

bool ny_native_arraytab_append_asm(ny_native_writer_t *w, char *err,
                                   size_t err_len) {
  if (!w)
    return false;
  /*
   * List literals live in this pool, and Ny lists are mutable
   * (`msg[1] = 7` mutates in place).  The pool must therefore land in a
   * writable section; .rodata made any element store fault at runtime.
   */
  if (!ny_native_put(w, "\t.section\t.data\n\t.p2align\t3\n"))
    return false;
  for (size_t i = 0; i < ny_arraytab_len; ++i) {
    ny_arraytab_ent_t *e = &ny_arraytab[i];
    if (!ny_native_printf(w, "\t.quad\t%zu\n\t.quad\t%zu\n\t.quad\t%zu\n%s:\n",
                          e->count, e->stride, e->count, e->name))
      return false;
    size_t so = e->count * e->stride;
    size_t sc = 0;
    for (size_t k = 0; k < e->count; ++k) {
      int64_t words[3] = {e->values[k].value, 0, 0};
      if (e->stride == 24) {
        if (e->values[k].str) {
          words[0] = (int64_t)(so + sc - k * 24);
          words[1] = (int64_t)e->values[k].str_len;
          words[2] = 121;
        } else {
          words[2] = 3;
        }
      }
      for (size_t word = 0; word < e->stride / 8; ++word)
        if (!ny_native_printf(w, "\t.quad\t%" PRId64 "\n", words[word]))
          return false;
      if (e->stride == 24 && e->values[k].str)
        sc += e->values[k].str_len + 1;
    }
    if (e->stride == 24) {
      for (size_t k = 0; k < e->count; ++k) {
        if (!e->values[k].str)
          continue;
        for (size_t j = 0; j < e->values[k].str_len + 1; ++j)
          if (!ny_native_printf(w, "\t.byte\t%u\n",
                                (unsigned char)e->strings[k][j]))
            return false;
      }
    }
  }
  if (!ny_native_put(w, "\t.text\n"))
    return false;
  (void)err;
  (void)err_len;
  return true;
}
