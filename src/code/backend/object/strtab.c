/*
 * Native string table: interned-symbol storage for ELF/Mach-O object
 * files, mapping symbol names to section-relative offsets.
 */
#include "base/util.h"
#include "code/backend/internal.h"
#include "code/backend/object/internal.h"
#include <nyr/layout.h>

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
  ny_strtab_ent_t *entries = ny_realloc_array(ny_strtab, cap, sizeof(*entries));
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
    size_t slot =
        (size_t)((h ^ ((uint64_t)ny_strtab[i].len * 0x9E3779B97F4A7C15ULL)) &
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

void nyr_strtab_clear(void) {
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

const char *nyr_strtab_intern(const char *s, size_t len, char *name_out,
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

const char *nyr_strtab_get(const char *name, size_t *len_out) {
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

bool nyr_strtab_append_defs(nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs,
                            size_t *def_count, char *err, size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    if (nyr_x64_obj_def_index(defs, *def_count, ny_strtab[i].name) >= 0)
      continue;
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      nyr_set_error(err, err_len, "native strtab: too many symbols");
      return false;
    }
    /*
     * 8-byte align for clean LEA targets and 8-byte aligned pointer tagging.
     */
    while (code->len & 7u) {
      if (!nyr_obj_u8(code, 0)) {
        nyr_set_error(err, err_len, "native strtab: out of memory");
        return false;
      }
    }
    if (!nyr_obj_u64(code, (uint64_t)(ny_strtab[i].len * 2u + 1u)) ||
        !nyr_obj_u64(code, 121u)) {
      nyr_set_error(err, err_len, "native strtab: out of memory");
      return false;
    }
    size_t off = code->len;
    if (!nyr_obj_emit(code, ny_strtab[i].bytes, ny_strtab[i].len + 1)) {
      nyr_set_error(err, err_len, "native strtab: out of memory");
      return false;
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             ny_strtab[i].name);
    defs[*def_count].off = off;
    defs[*def_count].size = ny_strtab[i].len + 1;
    defs[*def_count].is_data = true;
    defs[*def_count].section = NULL;
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

void nyr_consttab_clear(void) {
  free(ny_consttab);
  ny_consttab = NULL;
  ny_consttab_len = 0;
  ny_consttab_cap = 0;
}

bool nyr_consttab_add(const char *name, int64_t value) {
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

bool nyr_consttab_has(const char *name) {
  if (!name)
    return false;
  for (size_t i = 0; i < ny_consttab_len; ++i)
    if (strcmp(ny_consttab[i].name, name) == 0)
      return true;
  return false;
}
bool nyr_consttab_get(const char *name, int64_t *value) {
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

bool nyr_consttab_get_tail(const char *name, int64_t *value) {
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

bool nyr_consttab_append_defs(nyr_obj_buf_t *code,
                              nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
                              char *err, size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < ny_consttab_len; ++i) {
    if (nyr_x64_obj_def_index(defs, *def_count, ny_consttab[i].name) >= 0)
      continue;
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      nyr_set_error(err, err_len, "native consttab: too many symbols");
      return false;
    }
    /*
     * 8-byte align for clean .data values.
     */
    while (code->len & 7u) {
      if (!nyr_obj_u8(code, 0)) {
        nyr_set_error(err, err_len, "native consttab: out of memory");
        return false;
      }
    }
    size_t off = code->len;
    uint64_t raw = (uint64_t)ny_consttab[i].value;
    for (int b = 0; b < 8; ++b) {
      unsigned char byte = (unsigned char)((raw >> (b * 8)) & 0xff);
      if (!nyr_obj_u8(code, byte)) {
        nyr_set_error(err, err_len, "native consttab: out of memory");
        return false;
      }
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             ny_consttab[i].name);
    defs[*def_count].off = off;
    defs[*def_count].size = 8;
    defs[*def_count].is_data = true;
    defs[*def_count].section = NULL;
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

void nyr_globaltab_clear(void) {
  free(ny_globaltab);
  ny_globaltab = NULL;
  ny_globaltab_len = 0;
  ny_globaltab_cap = 0;
}

bool nyr_globaltab_add(const char *name) {
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

const char *nyr_globaltab_name(const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < ny_globaltab_len; ++i)
    if (strcmp(ny_globaltab[i].name, name) == 0)
      return ny_globaltab[i].name;
  return NULL;
}
const char *nyr_globaltab_name_tail(const char *name) {
  if (!name || !*name)
    return NULL;
  const char *match = NULL;
  for (size_t i = 0; i < ny_globaltab_len; ++i) {
    const char *dot = strrchr(ny_globaltab[i].name, '.');
    const char *tail = dot ? dot + 1 : ny_globaltab[i].name;
    if (strcmp(tail, name) != 0)
      continue;
    if (match)
      return NULL;
    match = ny_globaltab[i].name;
  }
  return match;
}

bool nyr_globaltab_has(const char *name) {
  return nyr_globaltab_name(name) != NULL;
}
bool nyr_globaltab_append_defs(nyr_obj_buf_t *code,
                               nyr_x64_obj_symbol_def_t *defs,
                               size_t *def_count, char *err, size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < ny_globaltab_len; ++i) {
    /*
     * A symbol may also have been discovered as a foldable top-level value.
     * Emit exactly one definition: strict system linkers reject duplicate
     * globals even when both definitions come from the same object.
     */
    if (nyr_x64_obj_def_index(defs, *def_count, ny_globaltab[i].name) >= 0)
      continue;
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      nyr_set_error(err, err_len, "native globaltab: too many symbols");
      return false;
    }
    while (code->len & 7u)
      if (!nyr_obj_u8(code, 0))
        return nyr_set_error(err, err_len, "native globaltab: out of memory"),
               false;
    size_t off = code->len;
    for (int b = 0; b < 8; ++b)
      if (!nyr_obj_u8(code, 0))
        return nyr_set_error(err, err_len, "native globaltab: out of memory"),
               false;
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             ny_globaltab[i].name);
    defs[*def_count].off = off;
    defs[*def_count].size = 8;
    defs[*def_count].is_data = true;
    defs[*def_count].section = NULL;
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
ny_arraytab_ent_t *nyr_arraytab;
size_t nyr_arraytab_len;
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
      ny_realloc_array(nyr_arraytab, cap, sizeof(*entries));
  if (!entries)
    return false;
  memset(entries + ny_arraytab_cap, 0,
         (cap - ny_arraytab_cap) * sizeof(*entries));
  nyr_arraytab = entries;
  ny_arraytab_cap = cap;
  return true;
}

void nyr_arraytab_clear(void) {
  for (size_t i = 0; i < nyr_arraytab_len; ++i) {
    for (size_t k = 0; k < nyr_arraytab[i].count; ++k)
      free(nyr_arraytab[i].strings[k]);
    free(nyr_arraytab[i].strings);
    free(nyr_arraytab[i].values);
    free(nyr_arraytab[i].data_ptr);
  }
  free(nyr_arraytab);
  nyr_arraytab = NULL;
  nyr_arraytab_len = 0;
  ny_arraytab_cap = 0;
}

const char *nyr_arraytab_intern(const nyr_array_elem_t *v, size_t n,
                                size_t stride, char *out, size_t cap) {
  if (!v || !n || n > NY_ARRAYTAB_MAX_ELEMS || (stride != 8 && stride != 24))
    return NULL;
  for (size_t i = 0; i < nyr_arraytab_len; ++i) {
    bool equal = nyr_arraytab[i].count == n && nyr_arraytab[i].stride == stride;
    for (size_t k = 0; equal && k < n; ++k) {
      equal = nyr_arraytab[i].values[k].value == v[k].value &&
              nyr_arraytab[i].values[k].tag == v[k].tag &&
              nyr_arraytab[i].values[k].str_len == v[k].str_len;
      if (equal && v[k].str_len)
        equal = memcmp(nyr_arraytab[i].strings[k], v[k].str, v[k].str_len) == 0;
    }
    if (equal) {
      if (out && cap)
        snprintf(out, cap, "%s", nyr_arraytab[i].name);
      return nyr_arraytab[i].name;
    }
  }
  if (!ny_arraytab_reserve(nyr_arraytab_len + 1u))
    return NULL;
  ny_arraytab_ent_t *e = &nyr_arraytab[nyr_arraytab_len];
  e->values = calloc(n, sizeof(*e->values));
  e->strings = calloc(n, sizeof(*e->strings));
  if (!e->values || !e->strings) {
    free(e->strings);
    free(e->values);
    memset(e, 0, sizeof(*e));
    return NULL;
  }
  memcpy(e->values, v, n * sizeof(*e->values));
  snprintf(e->name, sizeof(e->name), ".Lnyarr.%zu", nyr_arraytab_len);
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
  ++nyr_arraytab_len;
  if (out && cap)
    snprintf(out, cap, "%s", e->name);
  return e->name;
}

bool nyr_arraytab_get(const char *name, const nyr_array_elem_t **elems_out,
                      size_t *count_out, size_t *stride_out) {
  if (!name)
    return false;
  for (size_t i = 0; i < nyr_arraytab_len; ++i) {
    if (strcmp(nyr_arraytab[i].name, name) == 0) {
      if (elems_out)
        *elems_out = nyr_arraytab[i].values;
      if (count_out)
        *count_out = nyr_arraytab[i].count;
      if (stride_out)
        *stride_out = nyr_arraytab[i].stride;
      return true;
    }
  }
  return false;
}

bool nyr_arraytab_append_defs(nyr_obj_buf_t *code,
                              nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
                              char *err, size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < nyr_arraytab_len; ++i) {
    ny_arraytab_ent_t *e = &nyr_arraytab[i];
    if (*def_count > NY_NATIVE_MAX_DEFS - 2) {
      nyr_set_error(err, err_len, "native arraytab: too many symbols");
      return false;
    }
    while (code->len & 7u)
      if (!nyr_obj_u8(code, 0))
        goto oom;
    size_t header_off = code->len;
    uint64_t magic = NY_NATIVE_TBUF_MAGIC;
    for (int b = 0; b < 8; ++b)
      if (!nyr_obj_u8(code, (unsigned char)((magic >> (b * 8)) & 255)))
        goto oom;
    for (int b = 0; b < 8; ++b)
      if (!nyr_obj_u8(code,
                      (unsigned char)(((uint64_t)e->count >> (b * 8)) & 255)))
        goto oom;
    for (int b = 0; b < 8; ++b)
      if (!nyr_obj_u8(code,
                      (unsigned char)(((uint64_t)e->stride >> (b * 8)) & 255)))
        goto oom;
    for (int b = 0; b < 8; ++b)
      if (!nyr_obj_u8(code,
                      (unsigned char)(((uint64_t)e->count >> (b * 8)) & 255)))
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
          if (!nyr_obj_u8(code, (unsigned char)((raw >> (b * 8)) & 255)))
            goto oom;
      }
      if (e->stride == 24 && e->values[k].str)
        string_cursor += e->values[k].str_len + 1;
    }
    if (e->stride == 24) {
      for (size_t k = 0; k < e->count; ++k) {
        if (e->values[k].str &&
            !nyr_obj_emit(code, e->strings[k], e->values[k].str_len + 1))
          goto oom;
      }
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             e->name);
    defs[*def_count].off = off;
    defs[*def_count].size = code->len - off;
    defs[*def_count].is_data = true;
    defs[*def_count].section = NULL;
    ++(*def_count);
    /*
     * The named array symbol points at its first element, while its 32-byte
     * typed-buffer header immediately precedes that address. Some executable
     * writers materialize only ranges covered by symbol definitions, so give
     * the otherwise anonymous header its own range as well.
     */
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s.hdr",
             e->name);
    defs[*def_count].off = header_off;
    defs[*def_count].size = 32;
    defs[*def_count].is_data = true;
    defs[*def_count].section = NULL;
    ++(*def_count);
  }
  return true;

oom:
  nyr_set_error(err, err_len, "native arraytab: out of memory");
  return false;
}

static bool nyr_asm_emit_ascii(nyr_writer_t *w, const char *bytes, size_t len) {
  if (!nyr_put(w, "\t.ascii\t\""))
    return false;
  for (size_t i = 0; i < len; ++i) {
    unsigned char byte = (unsigned char)bytes[i];
    if (byte == '"' || byte == '\\') {
      if (!nyr_printf(w, "\\%c", byte))
        return false;
    } else if (byte >= 0x20 && byte <= 0x7e) {
      if (!nyr_printf(w, "%c", (int)byte))
        return false;
    } else if (!nyr_printf(w, "\\%03o", (unsigned)byte)) {
      return false;
    }
  }
  return nyr_put(w, "\"\n");
}

bool nyr_strtab_append_asm(nyr_writer_t *w, char *err, size_t err_len) {
  if (!w)
    return false;
  if (ny_strtab_len == 0)
    return true;
  if (!nyr_put(w, "\t.section\t.rodata\n"))
    return false;
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    if (!nyr_printf(w, "\t.globl\t%s\n%s:\n", ny_strtab[i].name,
                    ny_strtab[i].name))
      return false;
    if (!nyr_asm_emit_ascii(w, ny_strtab[i].bytes, ny_strtab[i].len) ||
        !nyr_put(w, "\t.byte\t0\n"))
      return false;
  }
  if (!nyr_put(w, "\t.text\n"))
    return false;
  (void)err;
  (void)err_len;
  return true;
}

bool nyr_arraytab_append_asm(nyr_writer_t *w, char *err, size_t err_len) {
  if (!w)
    return false;
  if (nyr_arraytab_len == 0)
    return true;
  /*
   * List literals live in this pool, and Ny lists are mutable
   * (`msg[1] = 7` mutates in place).  The pool must therefore land in a
   * writable section; .rodata made any element store fault at runtime.
   */
  if (!nyr_put(w, "\t.section\t.data\n\t.p2align\t3\n"))
    return false;
  for (size_t i = 0; i < nyr_arraytab_len; ++i) {
    ny_arraytab_ent_t *e = &nyr_arraytab[i];
    if (!nyr_printf(w,
                    "\t.quad\t%" PRIu64
                    "\n\t.quad\t%zu\n\t.quad\t%zu\n\t.quad\t%zu\n%s:\n",
                    (uint64_t)NY_NATIVE_TBUF_MAGIC, e->count, e->stride,
                    e->count, e->name))
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
          words[2] = e->values[k].tag;
        }
      }
      for (size_t word = 0; word < e->stride / 8; ++word)
        if (!nyr_printf(w, "\t.quad\t%" PRId64 "\n", words[word]))
          return false;
      if (e->stride == 24 && e->values[k].str)
        sc += e->values[k].str_len + 1;
    }
    if (e->stride == 24) {
      for (size_t k = 0; k < e->count; ++k) {
        if (!e->values[k].str)
          continue;
        if (!nyr_asm_emit_ascii(w, e->strings[k], e->values[k].str_len) ||
            !nyr_put(w, "\t.byte\t0\n"))
          return false;
      }
    }
  }
  if (!nyr_put(w, "\t.text\n"))
    return false;
  (void)err;
  (void)err_len;
  return true;
}

static bool nyr_data_write_u32(FILE *out, uint32_t value) {
  unsigned char b[4] = {(unsigned char)value, (unsigned char)(value >> 8),
                        (unsigned char)(value >> 16),
                        (unsigned char)(value >> 24)};
  return fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool nyr_data_write_u64(FILE *out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    if (fputc((int)((value >> (i * 8)) & 0xffu), out) == EOF)
      return false;
  return true;
}

static bool nyr_data_write_name(FILE *out, const char *name) {
  size_t len = name ? strlen(name) : 0;
  return len > 0 && len <= UINT16_MAX &&
         fputc((int)(len & 0xffu), out) != EOF &&
         fputc((int)((len >> 8) & 0xffu), out) != EOF &&
         fwrite(name, 1, len, out) == len;
}

bool nyr_native_data_write(FILE *out, char *err, size_t err_len) {
  if (!out || ny_strtab_len > UINT32_MAX || nyr_arraytab_len > UINT32_MAX ||
      ny_consttab_len > UINT32_MAX || ny_globaltab_len > UINT32_MAX)
    return nyr_set_error(err, err_len, "native NYIR data: invalid table size"),
           false;
  if (!nyr_data_write_u32(out, (uint32_t)ny_strtab_len) ||
      !nyr_data_write_u32(out, (uint32_t)nyr_arraytab_len) ||
      !nyr_data_write_u32(out, (uint32_t)ny_consttab_len) ||
      !nyr_data_write_u32(out, (uint32_t)ny_globaltab_len))
    goto io_error;
  for (size_t i = 0; i < ny_strtab_len; ++i) {
    if (ny_strtab[i].len > UINT32_MAX ||
        !nyr_data_write_name(out, ny_strtab[i].name) ||
        !nyr_data_write_u32(out, (uint32_t)ny_strtab[i].len) ||
        fwrite(ny_strtab[i].bytes, 1, ny_strtab[i].len, out) !=
            ny_strtab[i].len)
      goto io_error;
  }
  for (size_t i = 0; i < nyr_arraytab_len; ++i) {
    const ny_arraytab_ent_t *e = &nyr_arraytab[i];
    if (e->count > UINT32_MAX || e->stride > UINT32_MAX ||
        !nyr_data_write_name(out, e->name) ||
        !nyr_data_write_u32(out, (uint32_t)e->count) ||
        !nyr_data_write_u32(out, (uint32_t)e->stride))
      goto io_error;
    for (size_t j = 0; j < e->count; ++j) {
      bool has_string = e->values[j].str != NULL;
      size_t len = has_string ? e->values[j].str_len : 0;
      if (len > UINT32_MAX ||
          !nyr_data_write_u64(out, (uint64_t)e->values[j].value) ||
          !nyr_data_write_u64(out, (uint64_t)e->values[j].tag) ||
          fputc(has_string ? 1 : 0, out) == EOF ||
          !nyr_data_write_u32(out, (uint32_t)len) ||
          (len && fwrite(e->strings[j], 1, len, out) != len))
        goto io_error;
    }
  }
  for (size_t i = 0; i < ny_consttab_len; ++i)
    if (!nyr_data_write_name(out, ny_consttab[i].name) ||
        !nyr_data_write_u64(out, (uint64_t)ny_consttab[i].value))
      goto io_error;
  for (size_t i = 0; i < ny_globaltab_len; ++i)
    if (!nyr_data_write_name(out, ny_globaltab[i].name))
      goto io_error;
  return true;

io_error:
  nyr_set_error(err, err_len, "native NYIR data: failed to write tables");
  return false;
}

static bool nyr_data_read_u32(FILE *in, uint32_t *out) {
  unsigned char b[4];
  if (!in || !out || fread(b, 1, sizeof(b), in) != sizeof(b))
    return false;
  *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
  return true;
}

static bool nyr_data_read_u64(FILE *in, uint64_t *out) {
  uint64_t value = 0;
  if (!in || !out)
    return false;
  for (unsigned i = 0; i < 8; ++i) {
    int byte = fgetc(in);
    if (byte == EOF)
      return false;
    value |= (uint64_t)(unsigned char)byte << (i * 8);
  }
  *out = value;
  return true;
}

static bool nyr_data_read_name(FILE *in, char *name, size_t cap) {
  int lo = fgetc(in), hi = fgetc(in);
  if (lo == EOF || hi == EOF)
    return false;
  size_t len = (size_t)(unsigned char)lo | ((size_t)(unsigned char)hi << 8);
  if (!len || len >= cap || fread(name, 1, len, in) != len)
    return false;
  name[len] = '\0';
  return true;
}

bool nyr_native_data_read(FILE *in, char *err, size_t err_len) {
  uint32_t strings = 0, arrays = 0, constants = 0, globals = 0;
  size_t payload_bytes = 0;
  nyr_strtab_clear();
  nyr_arraytab_clear();
  nyr_consttab_clear();
  nyr_globaltab_clear();
  if (!in || !nyr_data_read_u32(in, &strings) ||
      !nyr_data_read_u32(in, &arrays) || !nyr_data_read_u32(in, &constants) ||
      !nyr_data_read_u32(in, &globals) || strings > 65536u || arrays > 65536u ||
      constants > 65536u || globals > 65536u)
    goto malformed;
  for (uint32_t i = 0; i < strings; ++i) {
    char name[32] = {0};
    uint32_t len = 0;
    if (!nyr_data_read_name(in, name, sizeof(name)) ||
        !nyr_data_read_u32(in, &len) || len > 64u * 1024u * 1024u)
      goto malformed;
    if (payload_bytes > 64u * 1024u * 1024u - len || nyr_strtab_get(name, NULL))
      goto malformed;
    payload_bytes += len;
    size_t old_len = ny_strtab_len;
    char *bytes = malloc((size_t)len + 1);
    if (!bytes)
      goto oom;
    if (fread(bytes, 1, len, in) != len) {
      free(bytes);
      goto malformed;
    }
    bytes[len] = '\0';
    const char *symbol = nyr_strtab_intern(bytes, len, NULL, 0);
    free(bytes);
    if (!symbol)
      goto oom;
    if (ny_strtab_len == old_len)
      goto malformed;
    snprintf((char *)symbol, sizeof(ny_strtab[0].name), "%s", name);
  }
  for (uint32_t i = 0; i < arrays; ++i) {
    char name[32] = {0};
    uint32_t count = 0, stride = 0;
    if (!nyr_data_read_name(in, name, sizeof(name)) ||
        !nyr_data_read_u32(in, &count) || !nyr_data_read_u32(in, &stride) ||
        !count || count > 65536u || (stride != 8 && stride != 24) ||
        nyr_arraytab_get(name, NULL, NULL, NULL))
      goto malformed;
    nyr_array_elem_t *values = calloc(count, sizeof(*values));
    char **strings_data = calloc(count, sizeof(*strings_data));
    if (!values || !strings_data) {
      free(values);
      free(strings_data);
      goto oom;
    }
    bool read_ok = true;
    for (uint32_t j = 0; j < count && read_ok; ++j) {
      uint64_t value = 0, tag = 0;
      uint32_t len = 0;
      read_ok = nyr_data_read_u64(in, &value) && nyr_data_read_u64(in, &tag);
      int has_string = read_ok ? fgetc(in) : EOF;
      read_ok = read_ok && has_string != EOF &&
                (has_string == 0 || has_string == 1) &&
                nyr_data_read_u32(in, &len) && len <= 64u * 1024u * 1024u &&
                (stride == 24 || !has_string) && (has_string || len == 0);
      if (!read_ok)
        break;
      if (payload_bytes > 64u * 1024u * 1024u - len) {
        read_ok = false;
        break;
      }
      payload_bytes += len;
      values[j].value = (int64_t)value;
      values[j].tag = (int64_t)tag;
      values[j].str_len = len;
      if (has_string) {
        strings_data[j] = malloc((size_t)len + 1);
        if (!strings_data[j]) {
          read_ok = false;
          break;
        }
        read_ok = fread(strings_data[j], 1, len, in) == len;
        strings_data[j][len] = '\0';
        values[j].str = strings_data[j];
      }
    }
    char symbol_name[32] = {0};
    size_t old_len = nyr_arraytab_len;
    const char *symbol =
        read_ok ? nyr_arraytab_intern(values, count, stride, symbol_name,
                                      sizeof(symbol_name))
                : NULL;
    for (uint32_t j = 0; j < count; ++j)
      free(strings_data[j]);
    free(strings_data);
    free(values);
    if (!read_ok)
      goto malformed;
    if (!symbol)
      goto oom;
    if (nyr_arraytab_len == old_len)
      goto malformed;
    snprintf((char *)symbol, sizeof(nyr_arraytab[0].name), "%s", name);
  }
  for (uint32_t i = 0; i < constants; ++i) {
    char name[256] = {0};
    uint64_t value = 0;
    if (!nyr_data_read_name(in, name, sizeof(name)) ||
        !nyr_data_read_u64(in, &value) || nyr_consttab_has(name) ||
        !nyr_consttab_add(name, (int64_t)value))
      goto malformed;
  }
  for (uint32_t i = 0; i < globals; ++i) {
    char name[256] = {0};
    if (!nyr_data_read_name(in, name, sizeof(name)) ||
        nyr_globaltab_has(name) || !nyr_globaltab_add(name))
      goto malformed;
  }
  return true;

oom:
  nyr_set_error(err, err_len, "native NYIR data: allocation failed");
  return false;
malformed:
  nyr_set_error(err, err_len, "native NYIR data: malformed table section");
  return false;
}
