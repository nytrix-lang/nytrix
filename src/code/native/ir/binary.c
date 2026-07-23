#include "code/native/ir.h"
#include "code/native/ir/internal.h"
#include "code/native/internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Versioned NYIR binary serialization and loading. Keep byte-order codecs and
 * load-owned strings together so the core IR/optimizer does not own file I/O. */

enum {
  NYIR_BINARY_VERSION = 10,
  NYIR_BINARY_MIN_VERSION = 1,
  NYIR_PHI_VERSION = 8,
  NYIR_PARAM_TYPES_VERSION = 10,
  /* Versions 1--7 predate NYIR_PHI, which was inserted after COPY. Their
   * wire opcode table therefore has 44 entries and must be normalized before
   * the current verifier or a backend observes it. */
  NYIR_LEGACY_OP_COUNT = 44,
};

static const char *nyir_func_own_symbol(nyir_func_t *f, char *s) {
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

static bool nyir_write_u8(FILE *out, uint8_t v) {
  return out && fwrite(&v, 1, 1, out) == 1;
}

static bool nyir_write_u16le(FILE *out, uint16_t v) {
  uint8_t b[2] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff)};
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool nyir_write_u32le(FILE *out, uint32_t v) {
  uint8_t b[4] = {
      (uint8_t)(v & 0xff),
      (uint8_t)((v >> 8) & 0xff),
      (uint8_t)((v >> 16) & 0xff),
      (uint8_t)((v >> 24) & 0xff),
  };
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool nyir_write_i32le(FILE *out, int32_t v) {
  return nyir_write_u32le(out, (uint32_t)v);
}

static bool nyir_write_i64le(FILE *out, int64_t v) {
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

static bool nyir_write_str(FILE *out, const char *s) {
  uint32_t n = s ? (uint32_t)strlen(s) : 0;
  return nyir_write_u32le(out, n) &&
         (n == 0 || fwrite(s, 1, n, out) == n);
}

static bool nyir_read_exact(FILE *in, void *p, size_t n) {
  return in && (n == 0 || fread(p, 1, n, in) == n);
}

static bool nyir_read_u16le(FILE *in, uint16_t *out) {
  uint8_t b[2];
  if (!out || !nyir_read_exact(in, b, sizeof(b)))
    return false;
  *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  return true;
}

static bool nyir_read_u32le(FILE *in, uint32_t *out) {
  uint8_t b[4];
  if (!out || !nyir_read_exact(in, b, sizeof(b)))
    return false;
  *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return true;
}

static bool nyir_read_i32le(FILE *in, int32_t *out) {
  uint32_t u = 0;
  if (!out || !nyir_read_u32le(in, &u))
    return false;
  *out = (int32_t)u;
  return true;
}

static bool nyir_read_i64le(FILE *in, int64_t *out) {
  uint8_t b[8];
  if (!out || !nyir_read_exact(in, b, sizeof(b)))
    return false;
  uint64_t u = (uint64_t)b[0] | ((uint64_t)b[1] << 8) |
               ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24) |
               ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) |
               ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
  *out = (int64_t)u;
  return true;
}

static bool nyir_read_str(FILE *in, char **out, uint32_t max_len) {
  uint32_t n = 0;
  if (!out || !nyir_read_u32le(in, &n) || n > max_len)
    return false;
  char *s = (char *)malloc((size_t)n + 1);
  if (!s)
    return false;
  if (!nyir_read_exact(in, s, n)) {
    free(s);
    return false;
  }
  s[n] = '\0';
  *out = s;
  return true;
}

static bool nyir_decode_opcode(uint16_t encoded, uint16_t version,
                                 uint16_t *decoded) {
  if (!decoded)
    return false;
  if (version < NYIR_PHI_VERSION) {
    if (encoded >= NYIR_LEGACY_OP_COUNT)
      return false;
    /* PHI occupies current opcode 3; legacy opcode 3 was ADD_I64. */
    if (encoded >= 3)
      ++encoded;
  }
  if (encoded >= NYIR_OP_COUNT)
    return false;
  *decoded = encoded;
  return true;
}

bool nyir_dump_binary(FILE *out, const nyir_func_t *f, const char *name) {
  if (!out || !f)
    return false;
  if (f->len > UINT32_MAX || f->param_count > UINT32_MAX ||
      (f->param_count && !f->param_types))
    return false;
  if (fwrite("NYIR", 1, 4, out) != 4)
    return false;
  if (!nyir_write_u16le(out, NYIR_BINARY_VERSION) || /* format version */
      !nyir_write_u16le(out, 0) ||              /* flags */
      !nyir_write_str(out, name && name[0] ? name : "<anon>") ||
      !nyir_write_i32le(out, f->next_value) ||
      !nyir_write_u32le(out, (uint32_t)f->len) ||
      !nyir_write_u32le(out, (uint32_t)f->param_count))
    return false;
  for (size_t i = 0; i < f->param_count; ++i)
    if (f->param_types[i] > NYIR_PARAM_F64 ||
        !nyir_write_u8(out, (uint8_t)f->param_types[i]))
      return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op >= NYIR_OP_COUNT || in->extra_args_len > UINT32_MAX ||
        in->phi_incoming_len > UINT32_MAX)
      return false;
    if (in->arg_sizes &&
        (in->op != NYIR_CALL || in->imm <= 0 ||
         in->imm > NYIR_CALL_MAX_ARGS))
      return false;
    if ((in->op != NYIR_PHI && in->phi_incoming_len != 0) ||
        (in->op == NYIR_PHI && !in->phi_incoming))
      return false;
    if (!nyir_write_u16le(out, (uint16_t)in->op) ||
        !nyir_write_u16le(out, (uint16_t)in->cmp) ||
        !nyir_write_i32le(out, in->dst) ||
        !nyir_write_i32le(out, in->a) ||
        !nyir_write_i32le(out, in->b) ||
        !nyir_write_i32le(out, in->c) ||
        !nyir_write_i32le(out, in->d) ||
        !nyir_write_i32le(out, in->e) ||
        !nyir_write_i32le(out, in->f) ||
        !nyir_write_i64le(out, in->imm) ||
        !nyir_write_u32le(out, in->flags) ||
        !nyir_write_u32le(out, in->effects) ||
        !nyir_write_u32le(out, in->debug.line) ||
        !nyir_write_u32le(out, in->debug.column) ||
        !nyir_write_u8(out, in->range.has_min ? 1 : 0) ||
        !nyir_write_u8(out, in->range.has_max ? 1 : 0) ||
        !nyir_write_i64le(out, in->range.min) ||
        !nyir_write_i64le(out, in->range.max) ||
        !nyir_write_str(out, in->debug.file) ||
        !nyir_write_str(out, in->symbol) ||
        !nyir_write_u32le(out, (uint32_t)in->extra_args_len))
      return false;
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (!nyir_write_i32le(out, in->extra_args[k]))
        return false;
    }
    uint32_t arg_sizes_len = in->arg_sizes ? (uint32_t)in->imm : 0;
    if (!nyir_write_u32le(out, arg_sizes_len))
      return false;
    for (uint32_t k = 0; k < arg_sizes_len; ++k) {
      if (!nyir_write_u32le(out, in->arg_sizes[k]))
        return false;
    }
    if (!nyir_write_u32le(out, (uint32_t)in->phi_incoming_len))
      return false;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (!nyir_write_i64le(out, in->phi_incoming[k].predecessor_label) ||
          !nyir_write_i32le(out, in->phi_incoming[k].value))
        return false;
  }
  return true;
}

bool nyir_load_binary(FILE *in, nyir_func_t *out, char *name,
                        size_t name_len, char *err, size_t err_len) {
  if (!in || !out)
    return nyir_err(err, err_len, "native NYIR load: missing input");
  nyir_func_t loaded = {0};
  char magic[4];
  uint16_t version = 0;
  uint16_t flags = 0;
  char *loaded_name = NULL;
  int32_t next_value = 0;
  uint32_t inst_count = 0;

  if (!nyir_read_exact(in, magic, sizeof(magic)) ||
      memcmp(magic, "NYIR", 4) != 0)
    goto malformed;
  if (!nyir_read_u16le(in, &version) || !nyir_read_u16le(in, &flags))
    goto malformed;
  if (version < NYIR_BINARY_MIN_VERSION || version > NYIR_BINARY_VERSION)
    return nyir_err(err, err_len, "native NYIR load: unsupported version %u",
                   (unsigned)version);
  if (flags != 0)
    return nyir_err(err, err_len, "native NYIR load: unsupported flags 0x%x",
                   (unsigned)flags);
  if (!nyir_read_str(in, &loaded_name, 1024) ||
      !nyir_read_i32le(in, &next_value) ||
      !nyir_read_u32le(in, &inst_count))
    goto malformed;
  if (next_value < 0)
    goto malformed;
  if (version >= NYIR_PARAM_TYPES_VERSION) {
    uint32_t param_count = 0;
    if (!nyir_read_u32le(in, &param_count) || param_count > INT32_MAX)
      goto malformed;
    if (param_count) {
      loaded.param_types = malloc((size_t)param_count * sizeof(*loaded.param_types));
      if (!loaded.param_types) {
        free(loaded_name);
        return nyir_err(err, err_len, NY_NATIVE_LOAD_OOM);
      }
      loaded.param_count = param_count;
      for (uint32_t i = 0; i < param_count; ++i) {
        uint8_t type = 0;
        if (!nyir_read_exact(in, &type, 1) || type > NYIR_PARAM_F64)
          goto malformed;
        loaded.param_types[i] = (nyir_param_type_t)type;
      }
    }
  }
  if (inst_count > (uint32_t)(SIZE_MAX / sizeof(*loaded.data)))
    return nyir_err(err, err_len, "native NYIR load: instruction count too large");
  if (inst_count > 0) {
    loaded.data = (nyir_inst_t *)calloc(inst_count, sizeof(*loaded.data));
    if (!loaded.data) {
      free(loaded_name);
      return nyir_err(err, err_len, NY_NATIVE_LOAD_OOM);
    }
    loaded.cap = inst_count;
  }
  loaded.next_value = next_value;

  for (uint32_t i = 0; i < inst_count; ++i) {
    nyir_inst_t inst = {.dst = -1,
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
    if (!nyir_read_u16le(in, &op) || !nyir_read_u16le(in, &cmp) ||
        !nyir_read_i32le(in, &dst) || !nyir_read_i32le(in, &a) ||
        !nyir_read_i32le(in, &b))
      goto malformed;
    if (version >= 3) {
      if (!nyir_read_i32le(in, &inst.c) || !nyir_read_i32le(in, &inst.d))
        goto malformed;
    }
    if (version >= 4) {
      if (!nyir_read_i32le(in, &inst.e) || !nyir_read_i32le(in, &inst.f))
        goto malformed;
    }
    if (!nyir_read_i64le(in, &inst.imm) || !nyir_read_u32le(in, &flags32))
      goto malformed;
    if (!nyir_decode_opcode(op, version, &op))
      goto malformed;
    if (version >= 2) {
      if (!nyir_read_u32le(in, &effects32) ||
          !nyir_read_u32le(in, &debug_line) ||
          !nyir_read_u32le(in, &debug_column) ||
          !nyir_read_exact(in, &has_min, 1) ||
          !nyir_read_exact(in, &has_max, 1) ||
          !nyir_read_i64le(in, &inst.range.min) ||
          !nyir_read_i64le(in, &inst.range.max) ||
          !nyir_read_str(in, &debug_file, 4096))
        goto malformed;
    }
    if (!nyir_read_str(in, &symbol, 4096)) {
      free(debug_file);
      goto malformed;
    }
    uint32_t extra_len = 0;
    int *extra = NULL;
    uint32_t *arg_sizes = NULL;
    if (version >= 5) {
      if (!nyir_read_u32le(in, &extra_len) ||
          extra_len > NYIR_CALL_MAX_ARGS) {
        free(symbol);
        goto malformed;
      }
      if (extra_len > 0) {
        extra = (int *)malloc((size_t)extra_len * sizeof(*extra));
        if (!extra) {
          free(symbol);
          free(loaded_name);
          nyir_func_free(&loaded);
          return nyir_err(err, err_len, NY_NATIVE_LOAD_OOM);
        }
        for (uint32_t k = 0; k < extra_len; ++k) {
          int32_t v = 0;
          if (!nyir_read_i32le(in, &v)) {
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
      if (!nyir_read_u32le(in, &arg_sizes_len) ||
          (arg_sizes_len != 0 &&
           (op != NYIR_CALL || inst.imm <= 0 ||
            arg_sizes_len != (uint32_t)inst.imm ||
            arg_sizes_len > NYIR_CALL_MAX_ARGS))) {
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
          nyir_func_free(&loaded);
          return nyir_err(err, err_len, NY_NATIVE_LOAD_OOM);
        }
        for (uint32_t k = 0; k < arg_sizes_len; ++k) {
          if (!nyir_read_u32le(in, &arg_sizes[k])) {
            free(arg_sizes);
            free(extra);
            free(symbol);
            goto malformed;
          }
          if (version < 7 && arg_sizes[k] > 0)
            arg_sizes[k] =
                (arg_sizes[k] & NYIR_ARG_AGG_SIZE_MASK) |
                (NYIR_ARG_CLASS_MEMORY << NYIR_ARG_AGG_CLASS0_SHIFT);
        }
      }
    }
    uint32_t phi_len = 0;
    nyir_phi_incoming_t *phi = NULL;
    if (version >= 8 &&
        (!nyir_read_u32le(in, &phi_len) || phi_len > inst_count)) {
      free(arg_sizes); free(extra); free(symbol); goto malformed;
    }
    if (phi_len) {
      if (op != NYIR_PHI) { free(arg_sizes); free(extra); free(symbol); goto malformed; }
      phi = malloc((size_t)phi_len * sizeof(*phi));
      if (!phi) { free(arg_sizes); free(extra); free(symbol); goto malformed; }
      for (uint32_t k = 0; k < phi_len; ++k) {
        int32_t value = 0;
        if (!nyir_read_i64le(in, &phi[k].predecessor_label) ||
            !nyir_read_i32le(in, &value)) {
          free(phi); free(arg_sizes); free(extra); free(symbol); goto malformed;
        }
        phi[k].value = (int)value;
      }
    }
    if (op >= NYIR_OP_COUNT || cmp > NYIR_CMP_GE) {
      free(phi);
      free(arg_sizes);
      free(extra);
      free(symbol);
      goto malformed;
    }
    inst.arg_sizes = arg_sizes;
    inst.phi_incoming = phi;
    inst.phi_incoming_len = phi_len;
    inst.extra_args = extra;
    inst.extra_args_len = extra_len;
    inst.op = (nyir_op_t)op;
    inst.cmp = (nyir_cmp_t)cmp;
    inst.dst = dst;
    inst.a = a;
    inst.b = b;
    inst.flags = flags32;
    inst.effects = effects32;
    inst.debug.line = debug_line;
    inst.debug.column = debug_column;
    inst.range.has_min = has_min != 0;
    inst.range.has_max = has_max != 0;
    /* Version 8 serialized the earlier, coarser pointer-memory effect
     * classification.  Recompute it before verification so valid v8 IR
     * remains loadable while v9 writes the precise canonical mask. */
    if (version == 8)
      inst.effects = nyir_inst_effects(&inst);
    if (debug_file && debug_file[0]) {
      inst.debug.file = nyir_func_own_symbol(&loaded, debug_file);
      if (!inst.debug.file) {
        free(inst.phi_incoming);
        free(inst.arg_sizes);
        free(inst.extra_args);
        free(symbol);
        free(loaded_name);
        nyir_func_free(&loaded);
        return nyir_err(err, err_len, NY_NATIVE_LOAD_OOM);
      }
    } else {
      free(debug_file);
    }
    if (symbol[0]) {
      inst.symbol = nyir_func_own_symbol(&loaded, symbol);
      if (!inst.symbol) {
        free(inst.phi_incoming);
        free(inst.arg_sizes);
        free(inst.extra_args);
        free(loaded_name);
        nyir_func_free(&loaded);
        return nyir_err(err, err_len, NY_NATIVE_LOAD_OOM);
      }
    } else {
      free(symbol);
    }
    loaded.data[loaded.len++] = inst;
  }

  if (!nyir_verify(&loaded, err, err_len)) {
    free(loaded_name);
    nyir_func_free(&loaded);
    return false;
  }
  nyir_refresh_metadata(&loaded);
  if (name && name_len > 0) {
    snprintf(name, name_len, "%s", loaded_name ? loaded_name : "");
  }
  free(loaded_name);
  nyir_func_free(out);
  *out = loaded;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;

malformed:
  free(loaded_name);
  nyir_func_free(&loaded);
  return nyir_err(err, err_len, "native NYIR load: malformed binary dump");
}
