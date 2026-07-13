#include "code/native/ir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Versioned NYIR binary serialization and loading. Keep byte-order codecs and
 * load-owned strings together so the core IR/optimizer does not own file I/O. */

static bool ny_nir_binary_err(char *err, size_t err_len, const char *fmt, ...) {
  if (err && err_len) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
  }
  return false;
}

static const char *ny_nir_func_own_symbol(ny_nir_func_t *f, char *s) {
  if (!f || !s)
    return NULL;
  if (f->owned_symbols_len >= f->owned_symbols_cap) {
    size_t cap = f->owned_symbols_cap ? f->owned_symbols_cap * 2 : 16;
    char **data = realloc(f->owned_symbols, cap * sizeof(*data));
    if (!data) {
      free(s);
      return NULL;
    }
    f->owned_symbols = data;
    f->owned_symbols_cap = cap;
  }
  f->owned_symbols[f->owned_symbols_len++] = s;
  return s;
}

static bool ny_nir_write_u8(FILE *out, uint8_t v) {
  return out && fwrite(&v, 1, 1, out) == 1;
}

static bool ny_nir_write_u16le(FILE *out, uint16_t v) {
  uint8_t b[2] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff)};
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool ny_nir_write_u32le(FILE *out, uint32_t v) {
  uint8_t b[4] = {
      (uint8_t)(v & 0xff),
      (uint8_t)((v >> 8) & 0xff),
      (uint8_t)((v >> 16) & 0xff),
      (uint8_t)((v >> 24) & 0xff),
  };
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool ny_nir_write_i32le(FILE *out, int32_t v) {
  return ny_nir_write_u32le(out, (uint32_t)v);
}

static bool ny_nir_write_i64le(FILE *out, int64_t v) {
  uint64_t u = (uint64_t)v;
  uint8_t b[8] = {
      (uint8_t)(u & 0xff),
      (uint8_t)((u >> 8) & 0xff),
      (uint8_t)((u >> 16) & 0xff),
      (uint8_t)((u >> 24) & 0xff),
      (uint8_t)((u >> 32) & 0xff),
      (uint8_t)((u >> 40) & 0xff),
      (uint8_t)((u >> 48) & 0xff),
      (uint8_t)((u >> 56) & 0xff),
  };
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool ny_nir_write_str(FILE *out, const char *s) {
  uint32_t n = s ? (uint32_t)strlen(s) : 0;
  return ny_nir_write_u32le(out, n) &&
         (n == 0 || fwrite(s, 1, n, out) == n);
}

static bool ny_nir_read_exact(FILE *in, void *p, size_t n) {
  return in && (n == 0 || fread(p, 1, n, in) == n);
}

static bool ny_nir_read_u16le(FILE *in, uint16_t *out) {
  uint8_t b[2];
  if (!out || !ny_nir_read_exact(in, b, sizeof(b)))
    return false;
  *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  return true;
}

static bool ny_nir_read_u32le(FILE *in, uint32_t *out) {
  uint8_t b[4];
  if (!out || !ny_nir_read_exact(in, b, sizeof(b)))
    return false;
  *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return true;
}

static bool ny_nir_read_i32le(FILE *in, int32_t *out) {
  uint32_t u = 0;
  if (!out || !ny_nir_read_u32le(in, &u))
    return false;
  *out = (int32_t)u;
  return true;
}

static bool ny_nir_read_i64le(FILE *in, int64_t *out) {
  uint8_t b[8];
  if (!out || !ny_nir_read_exact(in, b, sizeof(b)))
    return false;
  uint64_t u = (uint64_t)b[0] | ((uint64_t)b[1] << 8) |
               ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24) |
               ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) |
               ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
  *out = (int64_t)u;
  return true;
}

static bool ny_nir_read_str(FILE *in, char **out, uint32_t max_len) {
  uint32_t n = 0;
  if (!out || !ny_nir_read_u32le(in, &n) || n > max_len)
    return false;
  char *s = (char *)malloc((size_t)n + 1);
  if (!s)
    return false;
  if (!ny_nir_read_exact(in, s, n)) {
    free(s);
    return false;
  }
  s[n] = '\0';
  *out = s;
  return true;
}

bool ny_nir_dump_binary(FILE *out, const ny_nir_func_t *f, const char *name) {
  if (!out || !f)
    return false;
  if (fwrite("NYIR", 1, 4, out) != 4)
    return false;
  if (!ny_nir_write_u16le(out, 7) ||              /* format version */
      !ny_nir_write_u16le(out, 0) ||              /* flags */
      !ny_nir_write_str(out, name && name[0] ? name : "<anon>") ||
      !ny_nir_write_i32le(out, f->next_value) ||
      !ny_nir_write_u32le(out, (uint32_t)f->len))
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->arg_sizes &&
        (in->op != NY_NIR_CALL || in->imm <= 0 ||
         in->imm > NY_NIR_CALL_MAX_ARGS))
      return false;
    if (!ny_nir_write_u16le(out, (uint16_t)in->op) ||
        !ny_nir_write_u16le(out, (uint16_t)in->cmp) ||
        !ny_nir_write_i32le(out, in->dst) ||
        !ny_nir_write_i32le(out, in->a) ||
        !ny_nir_write_i32le(out, in->b) ||
        !ny_nir_write_i32le(out, in->c) ||
        !ny_nir_write_i32le(out, in->d) ||
        !ny_nir_write_i32le(out, in->e) ||
        !ny_nir_write_i32le(out, in->f) ||
        !ny_nir_write_i64le(out, in->imm) ||
        !ny_nir_write_u32le(out, in->flags) ||
        !ny_nir_write_u32le(out, in->effects) ||
        !ny_nir_write_u32le(out, in->debug.line) ||
        !ny_nir_write_u32le(out, in->debug.column) ||
        !ny_nir_write_u8(out, in->range.has_min ? 1 : 0) ||
        !ny_nir_write_u8(out, in->range.has_max ? 1 : 0) ||
        !ny_nir_write_i64le(out, in->range.min) ||
        !ny_nir_write_i64le(out, in->range.max) ||
        !ny_nir_write_str(out, in->debug.file) ||
        !ny_nir_write_str(out, in->symbol) ||
        !ny_nir_write_u32le(out, (uint32_t)in->extra_args_len))
      return false;
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (!ny_nir_write_i32le(out, in->extra_args[k]))
        return false;
    }
    uint32_t arg_sizes_len = in->arg_sizes ? (uint32_t)in->imm : 0;
    if (!ny_nir_write_u32le(out, arg_sizes_len))
      return false;
    for (uint32_t k = 0; k < arg_sizes_len; ++k) {
      if (!ny_nir_write_u32le(out, in->arg_sizes[k]))
        return false;
    }
  }
  return true;
}

bool ny_nir_load_binary(FILE *in, ny_nir_func_t *out, char *name,
                        size_t name_len, char *err, size_t err_len) {
  if (!in || !out)
    return ny_nir_binary_err(err, err_len, "native NYIR load: missing input");
  ny_nir_func_t loaded = {0};
  char magic[4];
  uint16_t version = 0;
  uint16_t flags = 0;
  char *loaded_name = NULL;
  int32_t next_value = 0;
  uint32_t inst_count = 0;

  if (!ny_nir_read_exact(in, magic, sizeof(magic)) ||
      memcmp(magic, "NYIR", 4) != 0)
    goto malformed;
  if (!ny_nir_read_u16le(in, &version) || !ny_nir_read_u16le(in, &flags))
    goto malformed;
  if (version != 1 && version != 2 && version != 3 && version != 4 &&
      version != 5 && version != 6 && version != 7)
    return ny_nir_binary_err(err, err_len, "native NYIR load: unsupported version %u",
                   (unsigned)version);
  if (flags != 0)
    return ny_nir_binary_err(err, err_len, "native NYIR load: unsupported flags 0x%x",
                   (unsigned)flags);
  if (!ny_nir_read_str(in, &loaded_name, 1024) ||
      !ny_nir_read_i32le(in, &next_value) ||
      !ny_nir_read_u32le(in, &inst_count))
    goto malformed;
  if (next_value < 0)
    goto malformed;
  if (inst_count > (uint32_t)(SIZE_MAX / sizeof(*loaded.data)))
    return ny_nir_binary_err(err, err_len, "native NYIR load: instruction count too large");
  if (inst_count > 0) {
    loaded.data = (ny_nir_inst_t *)calloc(inst_count, sizeof(*loaded.data));
    if (!loaded.data) {
      free(loaded_name);
      return ny_nir_binary_err(err, err_len, "native NYIR load: out of memory");
    }
    loaded.cap = inst_count;
  }
  loaded.next_value = next_value;

  for (uint32_t i = 0; i < inst_count; ++i) {
    ny_nir_inst_t inst = {.dst = -1,
                          .a = -1,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1};
    uint16_t op = 0;
    uint16_t cmp = 0;
    int32_t dst = 0;
    int32_t a = 0;
    int32_t b = 0;
    uint32_t flags32 = 0;
    uint32_t effects32 = 0;
    uint32_t debug_line = 0;
    uint32_t debug_column = 0;
    uint8_t has_min = 0;
    uint8_t has_max = 0;
    char *debug_file = NULL;
    char *symbol = NULL;
    if (!ny_nir_read_u16le(in, &op) || !ny_nir_read_u16le(in, &cmp) ||
        !ny_nir_read_i32le(in, &dst) || !ny_nir_read_i32le(in, &a) ||
        !ny_nir_read_i32le(in, &b))
      goto malformed;
    if (version >= 3) {
      if (!ny_nir_read_i32le(in, &inst.c) || !ny_nir_read_i32le(in, &inst.d))
        goto malformed;
    }
    if (version >= 4) {
      if (!ny_nir_read_i32le(in, &inst.e) || !ny_nir_read_i32le(in, &inst.f))
        goto malformed;
    }
    if (!ny_nir_read_i64le(in, &inst.imm) || !ny_nir_read_u32le(in, &flags32))
      goto malformed;
    if (version >= 2) {
      if (!ny_nir_read_u32le(in, &effects32) ||
          !ny_nir_read_u32le(in, &debug_line) ||
          !ny_nir_read_u32le(in, &debug_column) ||
          !ny_nir_read_exact(in, &has_min, 1) ||
          !ny_nir_read_exact(in, &has_max, 1) ||
          !ny_nir_read_i64le(in, &inst.range.min) ||
          !ny_nir_read_i64le(in, &inst.range.max) ||
          !ny_nir_read_str(in, &debug_file, 4096))
        goto malformed;
    }
    if (!ny_nir_read_str(in, &symbol, 4096)) {
      free(debug_file);
      goto malformed;
    }
    uint32_t extra_len = 0;
    int *extra = NULL;
    uint32_t *arg_sizes = NULL;
    if (version >= 5) {
      if (!ny_nir_read_u32le(in, &extra_len) ||
          extra_len > NY_NIR_CALL_MAX_ARGS) {
        free(symbol);
        goto malformed;
      }
      if (extra_len > 0) {
        extra = (int *)malloc((size_t)extra_len * sizeof(*extra));
        if (!extra) {
          free(symbol);
          free(loaded_name);
          ny_nir_func_free(&loaded);
          return ny_nir_binary_err(err, err_len, "native NYIR load: out of memory");
        }
        for (uint32_t k = 0; k < extra_len; ++k) {
          int32_t v = 0;
          if (!ny_nir_read_i32le(in, &v)) {
            free(extra);
            free(symbol);
            goto malformed;
          }
          extra[k] = (int)v;
        }
      }
    }
    if (version >= 6) {
      uint32_t arg_sizes_len = 0;
      if (!ny_nir_read_u32le(in, &arg_sizes_len) ||
          (arg_sizes_len != 0 &&
           (op != NY_NIR_CALL || inst.imm <= 0 ||
            arg_sizes_len != (uint32_t)inst.imm ||
            arg_sizes_len > NY_NIR_CALL_MAX_ARGS))) {
        free(extra);
        free(symbol);
        goto malformed;
      }
      if (arg_sizes_len > 0) {
        arg_sizes = (uint32_t *)malloc((size_t)arg_sizes_len * sizeof(*arg_sizes));
        if (!arg_sizes) {
          free(extra);
          free(symbol);
          free(loaded_name);
          ny_nir_func_free(&loaded);
          return ny_nir_binary_err(err, err_len, "native NYIR load: out of memory");
        }
        for (uint32_t k = 0; k < arg_sizes_len; ++k) {
          if (!ny_nir_read_u32le(in, &arg_sizes[k])) {
            free(arg_sizes);
            free(extra);
            free(symbol);
            goto malformed;
          }
          if (version < 7 && arg_sizes[k] > 0)
            arg_sizes[k] =
                (arg_sizes[k] & NY_NIR_ARG_AGG_SIZE_MASK) |
                (NY_NIR_ARG_CLASS_MEMORY << NY_NIR_ARG_AGG_CLASS0_SHIFT);
        }
      }
    }
    if (op >= NYIR_OP_COUNT || cmp > NY_NIR_CMP_GE) {
      free(arg_sizes);
      free(extra);
      free(symbol);
      goto malformed;
    }
    inst.arg_sizes = arg_sizes;
    inst.extra_args = extra;
    inst.extra_args_len = extra_len;
    inst.op = (ny_nir_op_t)op;
    inst.cmp = (ny_nir_cmp_t)cmp;
    inst.dst = dst;
    inst.a = a;
    inst.b = b;
    inst.flags = flags32;
    inst.effects = effects32;
    inst.debug.line = debug_line;
    inst.debug.column = debug_column;
    inst.range.has_min = has_min != 0;
    inst.range.has_max = has_max != 0;
    if (debug_file && debug_file[0]) {
      inst.debug.file = ny_nir_func_own_symbol(&loaded, debug_file);
      if (!inst.debug.file) {
        free(inst.arg_sizes);
        free(inst.extra_args);
        free(symbol);
        free(loaded_name);
        ny_nir_func_free(&loaded);
        return ny_nir_binary_err(err, err_len, "native NYIR load: out of memory");
      }
    } else {
      free(debug_file);
    }
    if (symbol[0]) {
      inst.symbol = ny_nir_func_own_symbol(&loaded, symbol);
      if (!inst.symbol) {
        free(inst.arg_sizes);
        free(inst.extra_args);
        free(loaded_name);
        ny_nir_func_free(&loaded);
        return ny_nir_binary_err(err, err_len, "native NYIR load: out of memory");
      }
    } else {
      free(symbol);
    }
    loaded.data[loaded.len++] = inst;
  }

  if (!ny_nir_verify(&loaded, err, err_len)) {
    free(loaded_name);
    ny_nir_func_free(&loaded);
    return false;
  }
  ny_nir_refresh_metadata(&loaded);
  if (name && name_len > 0) {
    snprintf(name, name_len, "%s", loaded_name ? loaded_name : "");
  }
  free(loaded_name);
  ny_nir_func_free(out);
  *out = loaded;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;

malformed:
  free(loaded_name);
  ny_nir_func_free(&loaded);
  return ny_nir_binary_err(err, err_len, "native NYIR load: malformed binary dump");
}
