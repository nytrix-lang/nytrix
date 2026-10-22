/*
 * Multi-object static ELF64 x86-64 linker for relocatable code and archives.
 */
#include <nyr/nyr.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__linux__) && defined(__x86_64__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void l_error(char *err, size_t len, const char *message) {
  if (err && len)
    snprintf(err, len, "%s", message);
}

#if defined(__linux__) && defined(__x86_64__)
enum {
  L_ET_REL = 1,
  L_ET_EXEC = 2,
  L_EM_X86_64 = 62,
  L_PT_LOAD = 1,
  L_SHT_PROGBITS = 1,
  L_SHT_SYMTAB = 2,
  L_SHT_STRTAB = 3,
  L_SHT_RELA = 4,
  L_SHT_NOBITS = 8,
  L_SHF_WRITE = 1,
  L_SHF_ALLOC = 2,
  L_SHF_EXECINSTR = 4,
  L_SHN_UNDEF = 0,
  L_SHN_ABS = 0xfff1,
  L_STB_LOCAL = 0,
  L_STB_WEAK = 2,
  L_STT_FUNC = 2,
  L_R_X86_64_64 = 1,
  L_R_X86_64_PC32 = 2,
  L_R_X86_64_PLT32 = 4,
  L_R_X86_64_32 = 10,
  L_R_X86_64_32S = 11,
};

typedef struct {
  unsigned char ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} l_ehdr_t;
typedef struct {
  uint32_t name, type;
  uint64_t flags, addr, offset, size;
  uint32_t link, info;
  uint64_t align, entsize;
} l_shdr_t;
typedef struct {
  uint32_t name;
  unsigned char info, other;
  uint16_t shndx;
  uint64_t value, size;
} l_sym_t;
typedef struct {
  uint64_t offset, info;
  int64_t addend;
} l_rela_t;
typedef struct {
  uint32_t type, flags;
  uint64_t offset, vaddr, paddr, filesz, memsz, align;
} l_phdr_t;

typedef struct {
  unsigned char *bytes;
  size_t len;
  bool owns_bytes;
  l_ehdr_t eh;
  l_shdr_t *sections;
  size_t *offsets;
  unsigned char *segments;
  size_t symtab_index, sym_count;
  const unsigned char *symbols, *strings;
  size_t strings_len;
} l_input_t;
typedef struct {
  const char *name;
  size_t input, symbol;
  bool weak;
} l_definition_t;
typedef struct {
  const char *name;
  bool defined, unresolved;
} l_symbol_state_t;
typedef struct {
  l_symbol_state_t *entries;
  size_t len, cap;
} l_symbol_map_t;
typedef struct {
  const unsigned char *bytes;
  size_t len;
  l_ehdr_t eh;
  l_shdr_t *sections;
  const unsigned char *symbols, *strings;
  size_t symbol_count, strings_len;
} l_scan_t;
typedef struct {
  size_t offset, len;
  bool selected;
  l_scan_t scan;
} l_archive_member_t;
typedef struct {
  unsigned char *bytes;
  size_t len;
  l_archive_member_t *members;
  size_t member_count, member_cap;
} l_archive_t;

static const unsigned char l_start[] = {
    0xf3, 0x0f, 0x1e, 0xfa, 0x31, 0xed, 0x48, 0x83, 0xe4, 0xf0, 0xe8, 0,
    0,    0,    0,    0x89, 0xc7, 0xb8, 60,   0,    0,    0,    0x0f, 0x05};

static bool l_range(size_t total, uint64_t off, uint64_t size) {
  return off <= total && size <= (uint64_t)total - off;
}
static bool l_align(size_t value, size_t alignment, size_t *out) {
  if (!out || !alignment || (alignment & (alignment - 1)) ||
      alignment > (1u << 20) || value > SIZE_MAX - alignment + 1)
    return false;
  *out = (value + alignment - 1) & ~(alignment - 1);
  return true;
}
static bool l_string(const unsigned char *table, size_t len, uint32_t off,
                     const char **out) {
  if (!table || !out || off >= len || !memchr(table + off, 0, len - off))
    return false;
  *out = (const char *)(table + off);
  return true;
}
static uint64_t l_hash(const char *s) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (; *s; ++s)
    h = (h ^ (unsigned char)*s) * UINT64_C(1099511628211);
  return h;
}
static bool l_map_rehash(l_symbol_map_t *map, size_t cap) {
  l_symbol_state_t *entries = calloc(cap, sizeof(*entries));
  if (!entries)
    return false;
  for (size_t i = 0; i < map->cap; ++i) {
    l_symbol_state_t old = map->entries[i];
    if (!old.name)
      continue;
    size_t slot = (size_t)l_hash(old.name) & (cap - 1);
    while (entries[slot].name)
      slot = (slot + 1) & (cap - 1);
    entries[slot] = old;
  }
  free(map->entries);
  map->entries = entries;
  map->cap = cap;
  return true;
}
static l_symbol_state_t *l_map_get(l_symbol_map_t *map, const char *name,
                                   bool create) {
  if (!map || !name || !*name)
    return NULL;
  if (!map->cap || (create && (map->len + 1) * 10 >= map->cap * 7)) {
    size_t cap = map->cap ? map->cap * 2 : 64;
    if (cap < map->cap || !l_map_rehash(map, cap))
      return NULL;
  }
  size_t slot = (size_t)l_hash(name) & (map->cap - 1);
  while (map->entries[slot].name && strcmp(map->entries[slot].name, name))
    slot = (slot + 1) & (map->cap - 1);
  if (!map->entries[slot].name && create) {
    size_t len = strlen(name);
    char *copy = malloc(len + 1);
    if (!copy)
      return NULL;
    memcpy(copy, name, len + 1);
    map->entries[slot].name = copy;
    map->len++;
  }
  return map->entries[slot].name ? &map->entries[slot] : NULL;
}
static void l_map_destroy(l_symbol_map_t *map) {
  if (map) {
    for (size_t i = 0; i < map->cap; ++i)
      free((void *)map->entries[i].name);
    free(map->entries);
    memset(map, 0, sizeof(*map));
  }
}

static bool l_scan_open(l_scan_t *scan, const unsigned char *bytes,
                        size_t len) {
  memset(scan, 0, sizeof(*scan));
  if (!bytes || len < sizeof(scan->eh))
    return false;
  scan->bytes = bytes;
  scan->len = len;
  memcpy(&scan->eh, bytes, sizeof(scan->eh));
  if (scan->eh.ident[0] != 0x7f || scan->eh.ident[1] != 'E' ||
      scan->eh.ident[2] != 'L' || scan->eh.ident[3] != 'F' ||
      scan->eh.ident[4] != 2 || scan->eh.ident[5] != 1 ||
      scan->eh.ident[6] != 1 || scan->eh.type != L_ET_REL ||
      scan->eh.machine != L_EM_X86_64 ||
      scan->eh.shentsize != sizeof(l_shdr_t) || !scan->eh.shnum ||
      scan->eh.shstrndx >= scan->eh.shnum ||
      !l_range(len, scan->eh.shoff,
               (uint64_t)scan->eh.shnum * sizeof(l_shdr_t)))
    return false;
  scan->sections = malloc((size_t)scan->eh.shnum * sizeof(*scan->sections));
  if (!scan->sections)
    return false;
  memcpy(scan->sections, bytes + scan->eh.shoff,
         (size_t)scan->eh.shnum * sizeof(*scan->sections));
  size_t symtab_index = SIZE_MAX;
  for (size_t i = 1; i < scan->eh.shnum; ++i) {
    if (scan->sections[i].type != L_SHT_SYMTAB)
      continue;
    if (symtab_index != SIZE_MAX)
      goto fail;
    symtab_index = i;
  }
  if (symtab_index == SIZE_MAX)
    goto fail;
  l_shdr_t *symtab = &scan->sections[symtab_index];
  if (symtab->entsize != sizeof(l_sym_t) || symtab->size % sizeof(l_sym_t) ||
      symtab->link >= scan->eh.shnum ||
      scan->sections[symtab->link].type != L_SHT_STRTAB ||
      !l_range(len, symtab->offset, symtab->size) ||
      !l_range(len, scan->sections[symtab->link].offset,
               scan->sections[symtab->link].size))
    goto fail;
  scan->symbols = bytes + symtab->offset;
  scan->symbol_count = (size_t)(symtab->size / sizeof(l_sym_t));
  scan->strings = bytes + scan->sections[symtab->link].offset;
  scan->strings_len = (size_t)scan->sections[symtab->link].size;
  return true;
fail:
  free(scan->sections);
  scan->sections = NULL;
  return false;
}
static void l_scan_close(l_scan_t *scan) {
  free(scan->sections);
  memset(scan, 0, sizeof(*scan));
}
static bool l_scan_symbols(l_scan_t *scan, l_symbol_map_t *map) {
  for (size_t i = 0; i < scan->symbol_count; ++i) {
    l_sym_t sym;
    memcpy(&sym, scan->symbols + i * sizeof(sym), sizeof(sym));
    const char *name;
    if (!l_string(scan->strings, scan->strings_len, sym.name, &name))
      return false;
    unsigned binding = sym.info >> 4;
    if (binding == L_STB_LOCAL || !*name)
      continue;
    bool defined = sym.shndx == L_SHN_ABS ||
                   (sym.shndx < scan->eh.shnum &&
                    (scan->sections[sym.shndx].flags & L_SHF_ALLOC));
    bool unresolved = sym.shndx == L_SHN_UNDEF && binding != L_STB_WEAK;
    if (!defined && !unresolved)
      continue;
    l_symbol_state_t *state = l_map_get(map, name, true);
    if (!state)
      return false;
    if (defined) {
      state->defined = true;
      state->unresolved = false;
    } else if (!state->defined) {
      state->unresolved = true;
    }
  }
  return true;
}
static bool l_scan_satisfies_unresolved(l_scan_t *scan, l_symbol_map_t *map) {
  for (size_t i = 0; i < scan->symbol_count; ++i) {
    l_sym_t sym;
    memcpy(&sym, scan->symbols + i * sizeof(sym), sizeof(sym));
    if ((sym.info >> 4) == L_STB_LOCAL || sym.shndx == L_SHN_UNDEF)
      continue;
    bool defined = sym.shndx == L_SHN_ABS ||
                   (sym.shndx < scan->eh.shnum &&
                    (scan->sections[sym.shndx].flags & L_SHF_ALLOC));
    if (!defined)
      continue;
    const char *name;
    if (!l_string(scan->strings, scan->strings_len, sym.name, &name))
      return false;
    l_symbol_state_t *state = l_map_get(map, name, false);
    if (state && state->unresolved && !state->defined)
      return true;
  }
  return false;
}
static bool l_parse_decimal(const unsigned char *data, size_t len,
                            size_t *value) {
  size_t n = 0;
  bool any = false;
  for (size_t i = 0; i < len && data[i] != ' '; ++i) {
    if (!isdigit(data[i]) || n > (SIZE_MAX - (size_t)(data[i] - '0')) / 10)
      return false;
    any = true;
    n = n * 10 + (size_t)(data[i] - '0');
  }
  if (!any)
    return false;
  *value = n;
  return true;
}
static bool l_archive_add(l_archive_t *archive, size_t offset, size_t len,
                          l_scan_t *scan) {
  if (archive->member_count == archive->member_cap) {
    size_t cap = archive->member_cap ? archive->member_cap * 2 : 16;
    if (cap < archive->member_cap || cap > 100000 ||
        cap > SIZE_MAX / sizeof(*archive->members))
      return false;
    l_archive_member_t *members =
        realloc(archive->members, cap * sizeof(*members));
    if (!members)
      return false;
    archive->members = members;
    archive->member_cap = cap;
  }
  archive->members[archive->member_count++] =
      (l_archive_member_t){.offset = offset, .len = len, .scan = *scan};
  memset(scan, 0, sizeof(*scan));
  return true;
}
static bool l_archive_open(l_archive_t *archive, unsigned char *bytes,
                           size_t len) {
  static const unsigned char magic[] = "!<arch>\n";
  if (len < sizeof(magic) - 1 || memcmp(bytes, magic, sizeof(magic) - 1))
    return false;
  archive->bytes = bytes;
  archive->len = len;
  size_t long_names_off = 0, long_names_len = 0;
  for (size_t pos = sizeof(magic) - 1; pos < len;) {
    if (!l_range(len, pos, 60) || bytes[pos + 58] != '`' ||
        bytes[pos + 59] != '\n')
      return false;
    size_t member_len;
    if (!l_parse_decimal(bytes + pos + 48, 10, &member_len) ||
        !l_range(len, pos + 60, member_len))
      return false;
    if (bytes[pos] == '/' && bytes[pos + 1] == '/' &&
        (bytes[pos + 2] == ' ' || bytes[pos + 2] == '/')) {
      long_names_off = pos + 60;
      long_names_len = member_len;
      break;
    }
    size_t step = 60 + member_len + (member_len & 1);
    if (step > len - pos)
      return false;
    pos += step;
  }
  for (size_t pos = sizeof(magic) - 1; pos < len;) {
    if (!l_range(len, pos, 60) || bytes[pos + 58] != '`' ||
        bytes[pos + 59] != '\n')
      return false;
    size_t member_len;
    if (!l_parse_decimal(bytes + pos + 48, 10, &member_len) ||
        !l_range(len, pos + 60, member_len))
      return false;
    const unsigned char *name = bytes + pos;
    size_t name_len = 16;
    while (name_len && name[name_len - 1] == ' ')
      --name_len;
    size_t data_off = pos + 60, data_len = member_len;
    bool special = (name_len == 1 && name[0] == '/') ||
                   (name_len == 7 && !memcmp(name, "/SYM64/", 7)) ||
                   (name_len == 2 && name[0] == '/' && name[1] == '/');
    if (name_len >= 3 && name[0] == '#' && name[1] == '1' && name[2] == '/') {
      size_t prefix_len;
      if (!l_parse_decimal(name + 3, name_len - 3, &prefix_len) ||
          prefix_len > data_len)
        return false;
      data_off += prefix_len;
      data_len -= prefix_len;
    } else if (name_len > 1 && name[0] == '/' && isdigit(name[1])) {
      size_t name_off;
      if (!long_names_len ||
          !l_parse_decimal(name + 1, name_len - 1, &name_off) ||
          name_off >= long_names_len ||
          !memchr(bytes + long_names_off + name_off, '\n',
                  long_names_len - name_off))
        return false;
    }
    if (!special && data_len >= 4 && bytes[data_off] == 0x7f &&
        bytes[data_off + 1] == 'E' && bytes[data_off + 2] == 'L' &&
        bytes[data_off + 3] == 'F') {
      l_scan_t scan;
      if (!l_scan_open(&scan, bytes + data_off, data_len))
        return false;
      if (!l_archive_add(archive, data_off, data_len, &scan)) {
        l_scan_close(&scan);
        return false;
      }
    }
    size_t step = 60 + member_len + (member_len & 1);
    if (step > len - pos)
      return false;
    pos += step;
  }
  return true;
}
static bool l_read(const char *path, unsigned char **bytes, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  bool ok = fseek(f, 0, SEEK_END) == 0;
  long n = ok ? ftell(f) : -1;
  ok = ok && n >= (long)sizeof(l_ehdr_t) && n <= 256l * 1024l * 1024l &&
       fseek(f, 0, SEEK_SET) == 0;
  unsigned char *p = ok ? malloc((size_t)n) : NULL;
  ok = ok && p && fread(p, 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  if (!ok) {
    free(p);
    return false;
  }
  *bytes = p;
  *len = (size_t)n;
  return true;
}
static bool l_write(const char *path, const unsigned char *data, size_t len) {
  size_t n = strlen(path);
  static const char suffix[] = ".nyr-tmp-XXXXXX";
  if (n > SIZE_MAX - sizeof(suffix))
    return false;
  char *tmp = malloc(n + sizeof(suffix));
  if (!tmp)
    return false;
  memcpy(tmp, path, n);
  memcpy(tmp + n, suffix, sizeof(suffix));
  int fd = mkstemp(tmp);
  bool ok = fd >= 0;
  size_t at = 0;
  while (ok && at < len) {
    ssize_t wrote = write(fd, data + at, len - at);
    if (wrote < 0 && errno == EINTR)
      continue;
    if (wrote <= 0) {
      ok = false;
      break;
    }
    at += (size_t)wrote;
  }
  if (ok && fchmod(fd, 0755) != 0)
    ok = false;
  if (fd >= 0 && close(fd) != 0)
    ok = false;
  if (ok && rename(tmp, path) != 0)
    ok = false;
  if (!ok)
    remove(tmp);
  free(tmp);
  return ok;
}
static bool l_apply(unsigned char *segment, size_t seg_len, size_t off,
                    uint32_t type, uint64_t s, int64_t addend, uint64_t p) {
  size_t width = type == L_R_X86_64_64 ? 8 : 4;
  if (!l_range(seg_len, off, width))
    return false;
  __int128 value;
  switch (type) {
  case L_R_X86_64_64:
  case L_R_X86_64_32:
  case L_R_X86_64_32S:
    value = (__int128)s + addend;
    break;
  case L_R_X86_64_PC32:
  case L_R_X86_64_PLT32:
    value = (__int128)s + addend - p;
    break;
  default:
    return false;
  }
  if (type == L_R_X86_64_64) {
    /*
     * ELF's 64-bit field records the low 64 bits of S + A.
     */
    uint64_t v = (uint64_t)value;
    memcpy(segment + off, &v, sizeof(v));
    return true;
  }
  if ((type == L_R_X86_64_32 && (value < 0 || value > UINT32_MAX)) ||
      (type != L_R_X86_64_32 && (value < INT32_MIN || value > INT32_MAX)))
    return false;
  int32_t v = (int32_t)value;
  memcpy(segment + off, &v, sizeof(v));
  return true;
}

static void l_destroy(l_input_t *inputs, size_t count) {
  if (!inputs)
    return;
  for (size_t i = 0; i < count; ++i) {
    if (inputs[i].owns_bytes)
      free(inputs[i].bytes);
    free(inputs[i].sections);
    free(inputs[i].offsets);
    free(inputs[i].segments);
  }
  free(inputs);
}

static bool l_link_objects(const char *const object_paths[],
                           const unsigned char *const object_data[],
                           const size_t object_lens[], size_t object_count,
                           const char *entry_symbol, const char *output_path,
                           char *err, size_t err_len) {
  if (!object_paths || !object_count || object_count > 4096 || !entry_symbol ||
      !*entry_symbol || !output_path || !*output_path) {
    l_error(err, err_len, "internal linker: invalid object list or output");
    return false;
  }
  l_input_t *inputs = calloc(object_count, sizeof(*inputs));
  l_definition_t *defs = NULL;
  size_t *slots = NULL;
  unsigned char *rx = NULL, *rw = NULL, *image = NULL;
  size_t rx_len = sizeof(l_start), rw_file_len = 0, rw_mem_len = 0;
  size_t image_len = 0, def_cap = 0, def_count = 0, slot_cap = 1;
  bool found_exec = false, ok = false;
  if (!inputs) {
    l_error(err, err_len, "internal linker: input allocation failed");
    return false;
  }
  for (size_t oi = 0; oi < object_count; ++oi) {
    l_input_t *in = &inputs[oi];
    if (object_data && object_data[oi]) {
      if (!object_lens || object_lens[oi] < sizeof(in->eh)) {
        l_error(err, err_len, "internal linker: invalid in-memory object");
        goto done;
      }
      in->bytes = (unsigned char *)object_data[oi];
      in->len = object_lens[oi];
    } else if (!object_paths[oi] || !*object_paths[oi] ||
               !l_read(object_paths[oi], &in->bytes, &in->len)) {
      l_error(err, err_len, "internal linker: cannot read input object");
      goto done;
    } else {
      in->owns_bytes = true;
    }
    memcpy(&in->eh, in->bytes, sizeof(in->eh));
    l_ehdr_t *eh = &in->eh;
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 || eh->ident[5] != 1 ||
        eh->ident[6] != 1 || eh->type != L_ET_REL ||
        eh->machine != L_EM_X86_64 || eh->ehsize != sizeof(*eh) ||
        eh->shentsize != sizeof(l_shdr_t) || !eh->shnum ||
        eh->shstrndx >= eh->shnum ||
        !l_range(in->len, eh->shoff, (uint64_t)eh->shnum * sizeof(l_shdr_t))) {
      l_error(err, err_len, "internal linker: expected ELF64 x86-64 objects");
      goto done;
    }
    in->sections = malloc((size_t)eh->shnum * sizeof(*in->sections));
    in->offsets = calloc(eh->shnum, sizeof(*in->offsets));
    in->segments = calloc(eh->shnum, sizeof(*in->segments));
    if (!in->sections || !in->offsets || !in->segments) {
      l_error(err, err_len, "internal linker: section allocation failed");
      goto done;
    }
    memcpy(in->sections, in->bytes + eh->shoff,
           (size_t)eh->shnum * sizeof(*in->sections));
    l_shdr_t *shstr_sec = &in->sections[eh->shstrndx];
    if (shstr_sec->type != L_SHT_STRTAB ||
        !l_range(in->len, shstr_sec->offset, shstr_sec->size)) {
      l_error(err, err_len, "internal linker: invalid section-name table");
      goto done;
    }
    const unsigned char *shstr = in->bytes + shstr_sec->offset;
    in->symtab_index = SIZE_MAX;
    for (size_t si = 1; si < eh->shnum; ++si) {
      l_shdr_t *sec = &in->sections[si];
      const char *name;
      if (!l_string(shstr, (size_t)shstr_sec->size, sec->name, &name)) {
        l_error(err, err_len, "internal linker: invalid section name");
        goto done;
      }
      (void)name;
      if (sec->type == L_SHT_SYMTAB) {
        if (in->symtab_index != SIZE_MAX) {
          l_error(err, err_len, "internal linker: multiple symbol tables");
          goto done;
        }
        in->symtab_index = si;
      }
      if (!(sec->flags & L_SHF_ALLOC))
        continue;
      if ((sec->flags & (L_SHF_WRITE | L_SHF_EXECINSTR)) ==
          (L_SHF_WRITE | L_SHF_EXECINSTR)) {
        l_error(err, err_len, "internal linker: writable executable section");
        goto done;
      }
      if (sec->type != L_SHT_PROGBITS && sec->type != L_SHT_NOBITS) {
        l_error(err, err_len, "internal linker: unsupported allocated section");
        goto done;
      }
      if (sec->type == L_SHT_PROGBITS &&
          !l_range(in->len, sec->offset, sec->size)) {
        l_error(err, err_len, "internal linker: truncated allocated section");
        goto done;
      }
      size_t *cursor = (sec->flags & L_SHF_WRITE) ? &rw_mem_len : &rx_len;
      size_t alignment = sec->align ? (size_t)sec->align : 1;
      if (!l_align(*cursor, alignment, cursor) ||
          sec->size > SIZE_MAX - *cursor) {
        l_error(err, err_len, "internal linker: invalid section layout");
        goto done;
      }
      in->offsets[si] = *cursor;
      in->segments[si] = (sec->flags & L_SHF_WRITE) ? 2 : 1;
      if (in->segments[si] == 1 && (sec->flags & L_SHF_EXECINSTR))
        found_exec = true;
      *cursor += (size_t)sec->size;
      if (*cursor > ((size_t)1 << 30)) {
        l_error(err, err_len, "internal linker: segment exceeds 1 GiB");
        goto done;
      }
      if (in->segments[si] == 2 && sec->type == L_SHT_PROGBITS &&
          rw_file_len < *cursor)
        rw_file_len = *cursor;
    }
    if (in->symtab_index == SIZE_MAX) {
      l_error(err, err_len, "internal linker: input has no symbol table");
      goto done;
    }
    l_shdr_t *symtab = &in->sections[in->symtab_index];
    if (symtab->entsize != sizeof(l_sym_t) || symtab->size % sizeof(l_sym_t) ||
        symtab->link >= eh->shnum ||
        in->sections[symtab->link].type != L_SHT_STRTAB ||
        !l_range(in->len, symtab->offset, symtab->size) ||
        !l_range(in->len, in->sections[symtab->link].offset,
                 in->sections[symtab->link].size)) {
      l_error(err, err_len, "internal linker: invalid symbol table");
      goto done;
    }
    in->symbols = in->bytes + symtab->offset;
    in->sym_count = (size_t)(symtab->size / sizeof(l_sym_t));
    in->strings = in->bytes + in->sections[symtab->link].offset;
    in->strings_len = (size_t)in->sections[symtab->link].size;
    if (in->sym_count > 1000000 - def_cap) {
      l_error(err, err_len, "internal linker: excessive symbol count");
      goto done;
    }
    def_cap += in->sym_count;
  }
  if (!found_exec || (!rw_mem_len && rw_file_len)) {
    l_error(err, err_len, "internal linker: no executable code section");
    goto done;
  }
  defs = calloc(def_cap ? def_cap : 1, sizeof(*defs));
  while (slot_cap < def_cap * 2 + 1) {
    if (slot_cap > SIZE_MAX / 2) {
      l_error(err, err_len, "internal linker: symbol table overflow");
      goto done;
    }
    slot_cap *= 2;
  }
  slots = calloc(slot_cap, sizeof(*slots));
  if (!defs || !slots) {
    l_error(err, err_len, "internal linker: symbol allocation failed");
    goto done;
  }
  for (size_t oi = 0; oi < object_count; ++oi) {
    l_input_t *in = &inputs[oi];
    for (size_t si = 0; si < in->sym_count; ++si) {
      l_sym_t sym;
      memcpy(&sym, in->symbols + si * sizeof(sym), sizeof(sym));
      const char *name;
      if (!l_string(in->strings, in->strings_len, sym.name, &name)) {
        l_error(err, err_len, "internal linker: invalid symbol name");
        goto done;
      }
      unsigned binding = sym.info >> 4;
      bool defined = sym.shndx == L_SHN_ABS ||
                     (sym.shndx < in->eh.shnum && in->segments[sym.shndx]);
      if (binding == L_STB_LOCAL || !defined || !*name)
        continue;
      size_t slot = (size_t)l_hash(name) & (slot_cap - 1);
      while (slots[slot] && strcmp(defs[slots[slot] - 1].name, name))
        slot = (slot + 1) & (slot_cap - 1);
      bool weak = binding == L_STB_WEAK;
      if (!slots[slot]) {
        defs[def_count] = (l_definition_t){name, oi, si, weak};
        slots[slot] = ++def_count;
      } else {
        l_definition_t *old = &defs[slots[slot] - 1];
        if (!old->weak && !weak) {
          if (err && err_len)
            snprintf(err, err_len,
                     "internal linker: duplicate strong definition of '%s'",
                     name);
          goto done;
        }
        if (old->weak && !weak)
          *old = (l_definition_t){name, oi, si, false};
      }
    }
  }
  size_t entry_slot = (size_t)l_hash(entry_symbol) & (slot_cap - 1);
  while (slots[entry_slot] &&
         strcmp(defs[slots[entry_slot] - 1].name, entry_symbol))
    entry_slot = (entry_slot + 1) & (slot_cap - 1);
  if (!slots[entry_slot]) {
    l_error(err, err_len, "internal linker: entry symbol is not defined");
    goto done;
  }
  l_definition_t *entry_def = &defs[slots[entry_slot] - 1];
  l_input_t *entry_in = &inputs[entry_def->input];
  l_sym_t entry_sym;
  memcpy(&entry_sym, entry_in->symbols + entry_def->symbol * sizeof(entry_sym),
         sizeof(entry_sym));
  if ((entry_sym.info & 0x0f) != L_STT_FUNC ||
      entry_sym.shndx >= entry_in->eh.shnum ||
      entry_in->segments[entry_sym.shndx] != 1 ||
      !(entry_in->sections[entry_sym.shndx].flags & L_SHF_EXECINSTR) ||
      entry_sym.value >= entry_in->sections[entry_sym.shndx].size) {
    l_error(err, err_len, "internal linker: entry is not executable code");
    goto done;
  }
  size_t entry_offset =
      entry_in->offsets[entry_sym.shndx] + (size_t)entry_sym.value;

  const size_t rx_file_off = 0x1000;
  if (rx_len > SIZE_MAX - rx_file_off ||
      !l_align(rx_file_off + rx_len, 0x1000, &image_len)) {
    l_error(err, err_len, "internal linker: executable image is too large");
    goto done;
  }
  size_t rw_file_off = image_len;
  if (rw_file_len > SIZE_MAX - rw_file_off) {
    l_error(err, err_len, "internal linker: output size overflow");
    goto done;
  }
  image_len = rw_file_len ? rw_file_off + rw_file_len : rx_file_off + rx_len;
  image = calloc(1, image_len);
  rx = calloc(1, rx_len);
  rw = rw_file_len ? calloc(1, rw_file_len) : NULL;
  if (!image || !rx || (rw_file_len && !rw)) {
    l_error(err, err_len, "internal linker: output allocation failed");
    goto done;
  }
  memcpy(rx, l_start, sizeof(l_start));
  for (size_t oi = 0; oi < object_count; ++oi) {
    l_input_t *in = &inputs[oi];
    for (size_t si = 1; si < in->eh.shnum; ++si) {
      l_shdr_t *sec = &in->sections[si];
      if (!in->segments[si] || sec->type == L_SHT_NOBITS)
        continue;
      unsigned char *dst = in->segments[si] == 1 ? rx : rw;
      size_t dst_len = in->segments[si] == 1 ? rx_len : rw_file_len;
      if (in->offsets[si] > dst_len || sec->size > dst_len - in->offsets[si]) {
        l_error(err, err_len, "internal linker: section copy out of range");
        goto done;
      }
      memcpy(dst + in->offsets[si], in->bytes + sec->offset, (size_t)sec->size);
    }
  }

  const uint64_t rx_base = 0x400000, rw_base = rx_base + rw_file_off;
  for (size_t oi = 0; oi < object_count; ++oi) {
    l_input_t *in = &inputs[oi];
    for (size_t ri = 1; ri < in->eh.shnum; ++ri) {
      l_shdr_t *relsec = &in->sections[ri];
      if (relsec->type != L_SHT_RELA)
        continue;
      if (relsec->info >= in->eh.shnum) {
        l_error(err, err_len, "internal linker: invalid relocation target");
        goto done;
      }
      if (relsec->link != in->symtab_index ||
          relsec->entsize != sizeof(l_rela_t) ||
          relsec->size % sizeof(l_rela_t) ||
          !l_range(in->len, relsec->offset, relsec->size)) {
        l_error(err, err_len, "internal linker: invalid relocation table");
        goto done;
      }
      size_t ti = relsec->info;
      if (!in->segments[ti])
        continue;
      unsigned char *target = in->segments[ti] == 1 ? rx : rw;
      size_t target_len = in->segments[ti] == 1 ? rx_len : rw_file_len;
      uint64_t target_vaddr = in->segments[ti] == 1
                                  ? rx_base + rx_file_off + in->offsets[ti]
                                  : rw_base + in->offsets[ti];
      size_t rel_count = (size_t)(relsec->size / sizeof(l_rela_t));
      for (size_t r = 0; r < rel_count; ++r) {
        l_rela_t rela;
        memcpy(&rela, in->bytes + relsec->offset + r * sizeof(rela),
               sizeof(rela));
        size_t sym_index = (size_t)(rela.info >> 32);
        uint32_t type = (uint32_t)rela.info;
        if (sym_index >= in->sym_count || rela.offset > in->sections[ti].size) {
          l_error(err, err_len, "internal linker: invalid relocation entry");
          goto done;
        }
        if (!type)
          continue;
        l_sym_t sym;
        memcpy(&sym, in->symbols + sym_index * sizeof(sym), sizeof(sym));
        const char *name;
        if (!l_string(in->strings, in->strings_len, sym.name, &name)) {
          l_error(err, err_len, "internal linker: invalid relocation symbol");
          goto done;
        }
        uint64_t symbol_value = 0;
        unsigned binding = sym.info >> 4;
        if (sym.shndx == L_SHN_ABS) {
          symbol_value = sym.value;
        } else {
          l_input_t *def_in = in;
          size_t def_sym_index = sym_index;
          if (binding != L_STB_LOCAL) {
            size_t slot = (size_t)l_hash(name) & (slot_cap - 1);
            while (slots[slot] && strcmp(defs[slots[slot] - 1].name, name))
              slot = (slot + 1) & (slot_cap - 1);
            if (slots[slot]) {
              l_definition_t *def = &defs[slots[slot] - 1];
              def_in = &inputs[def->input];
              def_sym_index = def->symbol;
              memcpy(&sym, def_in->symbols + def_sym_index * sizeof(sym),
                     sizeof(sym));
            } else if (sym.shndx == L_SHN_UNDEF && binding == L_STB_WEAK) {
              sym.shndx = L_SHN_UNDEF;
            } else {
              if (err && err_len)
                snprintf(err, err_len,
                         "internal linker: unresolved external symbol '%s'",
                         name);
              goto done;
            }
          }
          if (sym.shndx == L_SHN_ABS) {
            symbol_value = sym.value;
          } else if (sym.shndx != L_SHN_UNDEF) {
            if (sym.shndx >= def_in->eh.shnum || !def_in->segments[sym.shndx] ||
                sym.value > def_in->sections[sym.shndx].size ||
                sym.size > def_in->sections[sym.shndx].size - sym.value) {
              l_error(err, err_len, "internal linker: symbol outside section");
              goto done;
            }
            symbol_value =
                (def_in->segments[sym.shndx] == 1 ? rx_base + rx_file_off
                                                  : rw_base) +
                def_in->offsets[sym.shndx] + sym.value;
          }
        }
        size_t patch = in->offsets[ti] + (size_t)rela.offset;
        if (!l_apply(target, target_len, patch, type, symbol_value, rela.addend,
                     target_vaddr + rela.offset)) {
          l_error(err, err_len,
                  "internal linker: unsupported or overflowing relocation");
          goto done;
        }
      }
    }
  }
  if (!l_apply(rx, rx_len, 11, L_R_X86_64_PC32,
               rx_base + rx_file_off + entry_offset, -4,
               rx_base + rx_file_off + 11)) {
    l_error(err, err_len, "internal linker: entry relocation overflow");
    goto done;
  }
  memcpy(image + rx_file_off, rx, rx_len);
  if (rw_file_len)
    memcpy(image + rw_file_off, rw, rw_file_len);
  l_ehdr_t out = {0};
  out.ident[0] = 0x7f;
  out.ident[1] = 'E';
  out.ident[2] = 'L';
  out.ident[3] = 'F';
  out.ident[4] = 2;
  out.ident[5] = 1;
  out.ident[6] = 1;
  out.type = L_ET_EXEC;
  out.machine = L_EM_X86_64;
  out.version = 1;
  out.entry = rx_base + rx_file_off;
  out.phoff = sizeof(out);
  out.ehsize = sizeof(out);
  out.phentsize = sizeof(l_phdr_t);
  out.phnum = rw_mem_len ? 2 : 1;
  l_phdr_t phdrs[2] = {{0}};
  phdrs[0] = (l_phdr_t){.type = L_PT_LOAD,
                        .flags = 5,
                        .vaddr = rx_base,
                        .paddr = rx_base,
                        .filesz = rx_file_off + rx_len,
                        .memsz = rx_file_off + rx_len,
                        .align = 0x1000};
  if (rw_mem_len)
    phdrs[1] = (l_phdr_t){.type = L_PT_LOAD,
                          .flags = 6,
                          .offset = rw_file_off,
                          .vaddr = rw_base,
                          .paddr = rw_base,
                          .filesz = rw_file_len,
                          .memsz = rw_mem_len,
                          .align = 0x1000};
  memcpy(image, &out, sizeof(out));
  memcpy(image + out.phoff, phdrs, (size_t)out.phnum * sizeof(*phdrs));
  ok = l_write(output_path, image, image_len);
  if (!ok)
    l_error(err, err_len, "internal linker: could not write executable");
done:
  free(image);
  free(rx);
  free(rw);
  free(defs);
  free(slots);
  l_destroy(inputs, object_count);
  if (ok && err && err_len)
    err[0] = '\0';
  return ok;
}

bool nyr_link_elf64_executable_objects(const char *const object_paths[],
                                       size_t object_count,
                                       const char *entry_symbol,
                                       const char *output_path, char *err,
                                       size_t err_len) {
  if (!object_paths || !object_count || object_count > 256) {
    l_error(err, err_len, "internal linker: invalid input list");
    return false;
  }
  const size_t max_link_inputs = 4096;
  const char **link_paths = calloc(max_link_inputs, sizeof(*link_paths));
  const unsigned char **link_data = calloc(max_link_inputs, sizeof(*link_data));
  size_t *link_lens = calloc(max_link_inputs, sizeof(*link_lens));
  l_archive_t *archives = calloc(object_count, sizeof(*archives));
  l_symbol_map_t symbols = {0};
  size_t link_count = 0, archive_count = 0;
  bool ok = false;
  if (!link_paths || !link_data || !link_lens || !archives) {
    l_error(err, err_len, "internal linker: input expansion allocation failed");
    goto cleanup;
  }
  for (size_t i = 0; i < object_count; ++i) {
    unsigned char *bytes = NULL;
    size_t len = 0;
    if (!object_paths[i] || !*object_paths[i] ||
        !l_read(object_paths[i], &bytes, &len)) {
      l_error(err, err_len, "internal linker: cannot read input");
      goto cleanup;
    }
    static const unsigned char archive_magic[] = "!<arch>\n";
    if (len >= sizeof(archive_magic) - 1 &&
        !memcmp(bytes, archive_magic, sizeof(archive_magic) - 1)) {
      if (!l_archive_open(&archives[archive_count], bytes, len)) {
        for (size_t mi = 0; mi < archives[archive_count].member_count; ++mi)
          l_scan_close(&archives[archive_count].members[mi].scan);
        free(archives[archive_count].members);
        free(bytes);
        l_error(err, err_len, "internal linker: malformed static archive");
        goto cleanup;
      }
      archive_count++;
      continue;
    }
    if (link_count == max_link_inputs) {
      free(bytes);
      l_error(err, err_len, "internal linker: too many link inputs");
      goto cleanup;
    }
    link_paths[link_count] = object_paths[i];
    link_count++;
    l_scan_t scan;
    if (!l_scan_open(&scan, bytes, len) || !l_scan_symbols(&scan, &symbols)) {
      l_scan_close(&scan);
      free(bytes);
      l_error(err, err_len, "internal linker: invalid ELF input symbol table");
      goto cleanup;
    }
    l_scan_close(&scan);
    free(bytes);
  }
  if (!archive_count) {
    ok = l_link_objects(link_paths, link_data, link_lens, link_count,
                        entry_symbol, output_path, err, err_len);
    goto cleanup;
  }
  l_symbol_state_t *entry_state = l_map_get(&symbols, entry_symbol, true);
  if (!entry_state) {
    l_error(err, err_len, "internal linker: entry symbol allocation failed");
    goto cleanup;
  }
  if (!entry_state->defined)
    entry_state->unresolved = true;
  bool changed;
  do {
    changed = false;
    for (size_t ai = 0; ai < archive_count; ++ai) {
      l_archive_t *archive = &archives[ai];
      for (size_t mi = 0; mi < archive->member_count; ++mi) {
        l_archive_member_t *member = &archive->members[mi];
        if (member->selected)
          continue;
        bool needed = l_scan_satisfies_unresolved(&member->scan, &symbols);
        if (!needed)
          continue;
        if (link_count == max_link_inputs) {
          l_error(err, err_len,
                  "internal linker: too many extracted archive members");
          goto cleanup;
        }
        if (!l_scan_symbols(&member->scan, &symbols)) {
          l_error(err, err_len,
                  "internal linker: failed to extract archive member");
          goto cleanup;
        }
        member->selected = true;
        link_paths[link_count] = NULL;
        link_data[link_count] = archive->bytes + member->offset;
        link_lens[link_count] = member->len;
        link_count++;
        changed = true;
      }
    }
  } while (changed);
  ok = l_link_objects(link_paths, link_data, link_lens, link_count,
                      entry_symbol, output_path, err, err_len);
cleanup:
  for (size_t i = 0; i < archive_count; ++i) {
    for (size_t mi = 0; mi < archives[i].member_count; ++mi)
      l_scan_close(&archives[i].members[mi].scan);
    free(archives[i].members);
    free(archives[i].bytes);
  }
  l_map_destroy(&symbols);
  free(archives);
  free(link_data);
  free(link_lens);
  free(link_paths);
  return ok;
}
#else
bool nyr_link_elf64_executable_objects(const char *const object_paths[],
                                       size_t object_count,
                                       const char *entry_symbol,
                                       const char *output_path, char *err,
                                       size_t err_len) {
  (void)object_paths;
  (void)object_count;
  (void)entry_symbol;
  (void)output_path;
  l_error(err, err_len,
          "internal linker: ELF64 executable output requires Linux x86-64");
  return false;
}
#endif

/*
 * Backward-compatible single-object ELF entry point.
 */
bool nyr_link_elf64_executable(const char *object_path,
                               const char *entry_symbol,
                               const char *output_path, char *err,
                               size_t err_len) {
  const char *objects[] = {object_path};
  return nyr_link_elf64_executable_objects(objects, 1, entry_symbol,
                                           output_path, err, err_len);
}

static bool nyr_link_has(const char *text, const char *needle) {
  return text && needle && strstr(text, needle) != NULL;
}

static int nyr_link_run(const char *tool, const char *const argv[]) {
#if defined(_WIN32)
  intptr_t status = _spawnvp(_P_WAIT, tool, argv);
  return status == 0 ? 0 : -1;
#else
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (child == 0) {
    execvp(tool, (char *const *)argv);
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
#endif
}

static const char *nyr_link_tool(const char *env_name,
                                const char *fallback) {
  const char *value = getenv(env_name);
  return value && *value ? value : fallback;
}

static bool nyr_link_external(const nyr_link_request_t *request, char *err,
                              size_t err_len) {
  bool wasm = nyr_link_has(request->target_triple, "wasm32") ||
              nyr_link_has(request->target_triple, "wasm64");
  if (wasm && request->kind == NYR_LINK_SHARED_LIBRARY) {
    l_error(err, err_len, "linker: shared libraries are not supported for Wasm");
    return false;
  }
  const char *tool = NULL;
  char *owned_entry = NULL;
  const char **argv = calloc(request->input_count + 12, sizeof(*argv));
  if (!argv) {
    l_error(err, err_len, "linker: argument allocation failed");
    return false;
  }
  size_t n = 0;
  if (request->kind == NYR_LINK_STATIC_ARCHIVE) {
    tool = nyr_link_tool("NYR_LINK_AR", "llvm-ar");
    argv[n++] = tool;
    argv[n++] = "rcs";
    argv[n++] = (char *)request->output_path;
    for (size_t i = 0; i < request->input_count; ++i)
      argv[n++] = request->input_paths[i];
  } else if (wasm) {
    tool = nyr_link_tool("NYR_LINK_WASM_LD", "wasm-ld");
    argv[n++] = tool;
    if (request->kind == NYR_LINK_EXECUTABLE) {
      if (request->entry_symbol) {
        size_t len = strlen(request->entry_symbol) + sizeof("--entry=");
        owned_entry = malloc(len);
        if (!owned_entry) {
          free(argv);
          l_error(err, err_len, "linker: entry argument allocation failed");
          return false;
        }
        snprintf(owned_entry, len, "--entry=%s", request->entry_symbol);
        argv[n++] = owned_entry;
      } else {
        argv[n++] = "--no-entry";
      }
    }
    argv[n++] = "-o";
    argv[n++] = request->output_path;
    for (size_t i = 0; i < request->input_count; ++i)
      argv[n++] = request->input_paths[i];
  } else {
    tool = nyr_link_tool("NYR_LINK_CLANG", "clang");
    argv[n++] = tool;
    if (request->target_triple) {
      argv[n++] = "-target";
      argv[n++] = request->target_triple;
    }
    argv[n++] = "-fuse-ld=lld";
    bool freestanding = request->entry_symbol != NULL ||
                       request->kind == NYR_LINK_SHARED_LIBRARY;
    if (freestanding)
      argv[n++] = "-nostdlib";
    if (request->kind == NYR_LINK_SHARED_LIBRARY) {
      if (nyr_link_has(request->target_triple, "apple") ||
          nyr_link_has(request->target_triple, "darwin"))
        argv[n++] = "-dynamiclib";
      else if (nyr_link_has(request->target_triple, "windows") ||
               nyr_link_has(request->target_triple, "mingw") ||
               nyr_link_has(request->target_triple, "msvc")) {
        argv[n++] = "-Wl,/dll";
        argv[n++] = "-Wl,/noentry";
      }
      else
        argv[n++] = "-shared";
    } else if (nyr_link_has(request->target_triple, "windows") ||
               nyr_link_has(request->target_triple, "mingw") ||
               nyr_link_has(request->target_triple, "msvc")) {
      argv[n++] = "-Wl,/subsystem:console";
      if (request->entry_symbol) {
        size_t len = strlen(request->entry_symbol) + sizeof("-Wl,/entry:");
        owned_entry = malloc(len);
        if (!owned_entry) {
          free(argv);
          l_error(err, err_len, "linker: entry argument allocation failed");
          return false;
        }
        snprintf(owned_entry, len, "-Wl,/entry:%s", request->entry_symbol);
        argv[n++] = owned_entry;
      }
    } else {
      if (freestanding && !nyr_link_has(request->target_triple, "apple") &&
          !nyr_link_has(request->target_triple, "darwin"))
        argv[n++] = "-static";
      if (request->entry_symbol) {
        const char *prefix =
            nyr_link_has(request->target_triple, "apple") ||
                    nyr_link_has(request->target_triple, "darwin")
                ? "-Wl,-e,_"
                : "-Wl,-e,";
        size_t len = strlen(request->entry_symbol) + strlen(prefix) + 1;
        owned_entry = malloc(len);
        if (!owned_entry) {
          free(argv);
          l_error(err, err_len, "linker: entry argument allocation failed");
          return false;
        }
        snprintf(owned_entry, len, "%s%s", prefix, request->entry_symbol);
        argv[n++] = owned_entry;
      }
    }
    argv[n++] = "-o";
    argv[n++] = request->output_path;
    for (size_t i = 0; i < request->input_count; ++i)
      argv[n++] = request->input_paths[i];
  }
  argv[n] = NULL;
  int rc = nyr_link_run(tool, argv);
  free(owned_entry);
  free(argv);
  if (rc != 0) {
    if (err && err_len)
      snprintf(err, err_len,
               "linker: %s failed; verify the target toolchain and input objects",
               tool);
    return false;
  }
  if (err && err_len)
    err[0] = '\0';
  return true;
}

bool nyr_link(const nyr_link_request_t *request, char *err, size_t err_len) {
  if (!request || !request->input_paths || !request->input_count ||
      request->input_count > SIZE_MAX / sizeof(char *) - 12 ||
      !request->output_path || !*request->output_path ||
      request->kind < NYR_LINK_EXECUTABLE ||
      request->kind > NYR_LINK_STATIC_ARCHIVE) {
    l_error(err, err_len, "linker: invalid link request");
    return false;
  }
  for (size_t i = 0; i < request->input_count; ++i) {
    if (!request->input_paths[i] || !*request->input_paths[i]) {
      l_error(err, err_len, "linker: invalid input path");
      return false;
    }
  }
  if (request->kind == NYR_LINK_EXECUTABLE && request->entry_symbol &&
      !request->target_triple) {
#if defined(__linux__) && defined(__x86_64__)
    return nyr_link_elf64_executable_objects(
        request->input_paths, request->input_count, request->entry_symbol,
        request->output_path, err, err_len);
#endif
  }
  return nyr_link_external(request, err, err_len);
}
