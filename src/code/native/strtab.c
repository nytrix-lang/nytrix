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
 * during NYIR lower as .Lnystr.N symbols and emitted into the code blob after
 * functions so LEA/PC32 can resolve them without a separate .rodata section.
 *
 * Lookup is indexed by an open-addressing hash table keyed on (len, hash) so a
 * dedup hit is O(1) average instead of a linear scan over all prior entries.
 */

enum { NY_STRTAB_MAX = NY_NATIVE_MAX_STRINGS, NY_STRTAB_NAME = 32,
       NY_STRTAB_HASH_CAP = NY_NATIVE_MAX_STRINGS * 2 };

typedef struct {
  char name[NY_STRTAB_NAME];
  char *bytes;
  size_t len;
} ny_strtab_ent_t;

static ny_strtab_ent_t ny_strtab[NY_STRTAB_MAX];
static size_t ny_strtab_len = 0;
/*
 * Maps each hash slot to a 1-based index into ny_strtab (0 = empty).  Sized to
 * a power of two above NY_STRTAB_MAX so the table never needs to grow and stays
 * below a 0.5 load factor at full capacity.
 */
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
  /*
   * ny_strtab_hash was advanced to the first empty slot during the dedup
   * probe, so install the new entry (1-based) there.
   */
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
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      ny_native_set_err(err, err_len, "native strtab: too many symbols");
      return false;
    }
    /*
     * 4-byte align for clean LEA targets.
     */
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

/*
 * Session-local pool of foldable top-level `def` constants referenced as
 * object symbols.  The native lowerer registers a name/value pair for every
 * top-level def whose initializer folds to a raw i64; the object emitters
 * then write each registered constant as an 8-byte .data definition so a
 * user function's LEA/ADDR_SYMBOL reference resolves at link time instead
 * of leaving an undefined symbol (e.g. conway's `HALF_W`, `CELL_ALIVE`).
 *
 * Lookup is a simple linear scan: the pool is bounded (NY_CONSTTAB_MAX) and
 * recreated per program, so a scan is fine at these sizes.
 */

enum { NY_CONSTTAB_MAX = NY_NATIVE_MAX_CONSTANTS, NY_CONSTTAB_NAME = 128 };

typedef struct {
  char name[NY_CONSTTAB_NAME];
  int64_t value;
} ny_consttab_ent_t;

static ny_consttab_ent_t ny_consttab[NY_CONSTTAB_MAX];
static size_t ny_consttab_len = 0;

void ny_native_consttab_clear(void) {
  ny_consttab_len = 0;
  memset(ny_consttab, 0, sizeof(ny_consttab));
}

bool ny_native_consttab_add(const char *name, int64_t value) {
  if (!name || !name[0])
    return false;
  for (size_t i = 0; i < ny_consttab_len; ++i) {
    if (strcmp(ny_consttab[i].name, name) == 0)
      return true; /* already registered — keep first value */
  }
  if (ny_consttab_len >= NY_CONSTTAB_MAX)
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

bool ny_native_consttab_append_defs(ny_obj_buf_t *code,
                                    ny_x64_obj_symbol_def_t *defs,
                                    size_t *def_count, char *err,
                                    size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < ny_consttab_len; ++i) {
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
    (*def_count)++;
  }
  return true;
}

/*
 * Session-local pool of constant arrays (list literals with all-constant
 * elements).  NY_E_LIST lowering interns the element values here and emits a
 * NYIR_ADDR_SYMBOL to the generated .Lnyarr.N symbol; the object emitters
 * write a 24-byte tbuf header (count, stride, capacity) followed by count *
 * stride bytes of .data so indexed loads and `.len` resolve at link time
 * (e.g. pong's `def offs = [-1.0, ...]` followed by
 * `offs[(rally * 7 + score) % 7]`).  The header mirrors rt_native_tbuf_new's
 * layout so the array data pointer satisfies rt_native_tbuf_len/data[-24].
 *
 * Interned by content so repeated literals share one symbol; the pool is
 * bounded and recreated per program.
 */

enum { NY_ARRAYTAB_MAX = NY_NATIVE_MAX_ARRAYS, NY_ARRAYTAB_NAME = 32,
       NY_ARRAYTAB_ELEMS = NY_NATIVE_MAX_ARRAY_ELEMS };
typedef struct {
  char name[NY_ARRAYTAB_NAME];
  ny_native_array_elem_t values[NY_ARRAYTAB_ELEMS];
  char *strings[NY_ARRAYTAB_ELEMS];
  size_t count;
  size_t stride;
} ny_arraytab_ent_t;

static ny_arraytab_ent_t ny_arraytab[NY_ARRAYTAB_MAX];
static size_t ny_arraytab_len;

void ny_native_arraytab_clear(void) {
  for (size_t i = 0; i < ny_arraytab_len; ++i)
    for (size_t k = 0; k < ny_arraytab[i].count; ++k)
      free(ny_arraytab[i].strings[k]);
  ny_arraytab_len = 0;
  memset(ny_arraytab, 0, sizeof(ny_arraytab));
}

const char *ny_native_arraytab_intern(const ny_native_array_elem_t *v,
                                      size_t n, size_t stride, char *out,
                                      size_t cap) {
  if (!v || !n || n > NY_ARRAYTAB_ELEMS || (stride != 8 && stride != 24))
    return NULL;
  for (size_t i = 0; i < ny_arraytab_len; ++i) {
    bool equal = ny_arraytab[i].count == n && ny_arraytab[i].stride == stride;
    for (size_t k = 0; equal && k < n; ++k) {
      equal = ny_arraytab[i].values[k].value == v[k].value &&
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
  if (ny_arraytab_len >= NY_ARRAYTAB_MAX)
    return NULL;
  ny_arraytab_ent_t *e = &ny_arraytab[ny_arraytab_len];
  snprintf(e->name, sizeof(e->name), ".Lnyarr.%zu", ny_arraytab_len);
  e->count = n;
  e->stride = stride;
  for (size_t k = 0; k < n; ++k) {
    e->values[k] = v[k];
    if (!v[k].str)
      continue;
    e->strings[k] = malloc(v[k].str_len + 1);
    if (!e->strings[k]) {
      for (size_t j = 0; j < k; ++j)
        free(e->strings[j]);
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
      int64_t words[3] = {e->values[k].value, 0, 0};
      if (e->stride == 24) {
        if (e->values[k].str) {
          words[0] = (int64_t)(string_base + string_cursor - k * 24);
          words[1] = (int64_t)e->values[k].str_len;
          words[2] = 121;
        } else {
          words[2] = 3;
        }
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
    if (!ny_native_printf(w, "%s:\n", ny_strtab[i].name))
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
  if (!ny_native_put(w, "\t.section\t.rodata\n\t.p2align\t3\n"))
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
