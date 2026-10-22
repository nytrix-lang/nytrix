/*
 * NYIR binary format: reads and writes the versioned, pointer-free
 * NYIR program bundle for cross-session caching and diagnostics.
 */
#include "code/backend/internal.h"
#include "code/backend/ir/internal.h"
#include <nyr/ir.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Versioned NYIR binary serialization and loading. Keep byte-order codecs and
 * load-owned strings together so the core IR/optimizer does not own file I/O.
 */

enum {
  NYIR_BINARY_VERSION = 19,
  NYIR_BINARY_MIN_VERSION = 1,
  NYIR_PHI_VERSION = 8,
  NYIR_PARAM_TYPES_VERSION = 11,
  NYIR_PARAM_AGG_VERSION = 19,
  NYIR_SOURCE_PARAM_WIDTHS_VERSION = 13,
  NYIR_CALL_SCALAR_CLASSES_VERSION = 16,
  NYIR_CALL_OWNERSHIP_VERSION = 17,
  NYIR_CALL_CONTRACTS_VERSION = 18,
  /*
   * Retained only to import v1-v15 artifacts. These bits are not part of the
   * current in-memory NYIR flag vocabulary.
   */
  NYIR_LEGACY_INST_F_RET_F64 = 2u,
  NYIR_LEGACY_INST_F_RET_F32 = 4u,
  /*
   * Versions 1--7 predate NYIR_PHI, which was inserted after COPY. Their
   * wire opcode table therefore has 44 entries and must be normalized before
   * the current verifier or a backend observes it.
   */
  NYIR_LEGACY_OP_COUNT = 44,
  /*
   * Untrusted/stale cache artifacts are parsed before verification.  Bound the
   * aggregate work done by the decoder so a structurally valid header cannot
   * request unbounded allocations.  These ceilings are deliberately much
   * larger than normal compilation units; they are corruption/DoS guards, not
   * optimizer limits.
   */
  NYIR_BINARY_MAX_INSTRUCTIONS = 4 * 1024 * 1024,
  NYIR_BINARY_MAX_VALUES = 16 * 1024 * 1024,
  NYIR_BINARY_MAX_PARAMS = 64 * 1024,
  NYIR_BINARY_MAX_AUX_ENTRIES = 16 * 1024 * 1024,
  NYIR_BINARY_MAX_STRING_BYTES = 64 * 1024 * 1024,
};

static const char *nyr_func_own_symbol(nyr_func_t *f, char *s) {
  if (!f || !s)
    return NULL;
  if (f->owned_symbols_len >= f->owned_symbols_cap) {
    if (f->owned_symbols_cap > SIZE_MAX / 2 / sizeof(*f->owned_symbols)) {
      free(s);
      return NULL;
    }
    size_t cap = f->owned_symbols_cap ? f->owned_symbols_cap * 2 : 16;
    if (cap > SIZE_MAX / sizeof(*f->owned_symbols)) {
      free(s);
      return NULL;
    }
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

static bool nyr_write_u8(FILE *out, uint8_t v) {
  return out && fwrite(&v, 1, 1, out) == 1;
}

static bool nyr_write_u16le(FILE *out, uint16_t v) {
  uint8_t b[2] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff)};
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool nyr_write_u32le(FILE *out, uint32_t v) {
  uint8_t b[4] = {
      (uint8_t)(v & 0xff),
      (uint8_t)((v >> 8) & 0xff),
      (uint8_t)((v >> 16) & 0xff),
      (uint8_t)((v >> 24) & 0xff),
  };
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool nyr_write_i32le(FILE *out, int32_t v) {
  return nyr_write_u32le(out, (uint32_t)v);
}

static bool nyr_write_i64le(FILE *out, int64_t v) {
  uint64_t u = (uint64_t)v;
  uint8_t b[8] = {
      (uint8_t)(u & 0xff),         (uint8_t)((u >> 8) & 0xff),
      (uint8_t)((u >> 16) & 0xff), (uint8_t)((u >> 24) & 0xff),
      (uint8_t)((u >> 32) & 0xff), (uint8_t)((u >> 40) & 0xff),
      (uint8_t)((u >> 48) & 0xff), (uint8_t)((u >> 56) & 0xff),
  };
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool nyr_write_str(FILE *out, const char *s) {
  uint32_t n = s ? (uint32_t)strlen(s) : 0;
  return nyr_write_u32le(out, n) && (n == 0 || fwrite(s, 1, n, out) == n);
}

static bool nyr_read_exact(FILE *in, void *p, size_t n) {
  return in && (n == 0 || fread(p, 1, n, in) == n);
}

static bool nyr_read_u16le(FILE *in, uint16_t *out) {
  uint8_t b[2];
  if (!out || !nyr_read_exact(in, b, sizeof(b)))
    return false;
  *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  return true;
}

static bool nyr_read_u32le(FILE *in, uint32_t *out) {
  uint8_t b[4];
  if (!out || !nyr_read_exact(in, b, sizeof(b)))
    return false;
  *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
  return true;
}

static bool nyr_read_i32le(FILE *in, int32_t *out) {
  uint32_t u = 0;
  if (!out || !nyr_read_u32le(in, &u))
    return false;
  *out = (int32_t)u;
  return true;
}

static bool nyr_read_i64le(FILE *in, int64_t *out) {
  uint8_t b[8];
  if (!out || !nyr_read_exact(in, b, sizeof(b)))
    return false;
  uint64_t u = (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) |
               ((uint64_t)b[3] << 24) | ((uint64_t)b[4] << 32) |
               ((uint64_t)b[5] << 40) | ((uint64_t)b[6] << 48) |
               ((uint64_t)b[7] << 56);
  *out = (int64_t)u;
  return true;
}

static bool nyr_read_str(FILE *in, char **out, uint32_t max_len) {
  uint32_t n = 0;
  if (!out || !nyr_read_u32le(in, &n) || n > max_len)
    return false;
  char *s = (char *)malloc((size_t)n + 1);
  if (!s)
    return false;
  if (!nyr_read_exact(in, s, n)) {
    free(s);
    return false;
  }
  s[n] = '\0';
  *out = s;
  return true;
}

static bool nyr_decode_opcode(uint16_t encoded, uint16_t version,
                               uint16_t *decoded) {
  if (!decoded)
    return false;
  if (version < NYIR_PHI_VERSION) {
    if (encoded >= NYIR_LEGACY_OP_COUNT)
      return false;
    /*
     * PHI occupies current opcode 3; legacy opcode 3 was ADD_I64.
     */
    if (encoded >= 3)
      ++encoded;
  }
  if (encoded >= NYIR_OP_COUNT)
    return false;
  *decoded = encoded;
  return true;
}

bool nyr_dump_binary(FILE *out, const nyr_func_t *f, const char *name) {
  if (!out || !f)
    return false;
  if (f->len > UINT32_MAX || f->len > NYIR_BINARY_MAX_INSTRUCTIONS ||
      f->next_value < 0 || f->next_value > NYIR_BINARY_MAX_VALUES ||
      f->param_count > UINT32_MAX || f->param_count > NYIR_BINARY_MAX_PARAMS ||
      (f->param_count && !f->param_types))
    return false;
  if (fwrite("NYIR", 1, 4, out) != 4)
    return false;
  if (!nyr_write_u16le(out, NYIR_BINARY_VERSION) || /* format version */
      !nyr_write_u16le(out, 0) ||                   /* flags */
      !nyr_write_str(out, name && name[0] ? name : "<anon>") ||
      !nyr_write_i32le(out, f->next_value) ||
      !nyr_write_u32le(out, (uint32_t)f->len) ||
      !nyr_write_u32le(out, (uint32_t)f->param_count))
    return false;
  for (size_t i = 0; i < f->param_count; ++i)
    if (f->param_types[i] > NYIR_PARAM_AGGREGATE ||
        !nyr_write_u8(out, (uint8_t)f->param_types[i]))
      return false;
  if (f->param_count && f->param_types) {
    bool has_aggregate = false;
    for (size_t i = 0; i < f->param_count && !has_aggregate; ++i)
      has_aggregate = f->param_types[i] == NYIR_PARAM_AGGREGATE;
    if (has_aggregate) {
      if (!f->param_abi)
        return false;
      for (size_t i = 0; i < f->param_count; ++i) {
        if (f->param_types[i] != NYIR_PARAM_AGGREGATE)
          continue;
        uint32_t packed = f->param_abi[i];
        if (NYIR_ARG_AGG_SIZE(packed) == 0 ||
            NYIR_ARG_AGG_CLASS(packed, 0) > NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
            NYIR_ARG_AGG_CLASS(packed, 1) > NYIR_ARG_CLASS_AAPCS_INTEGER_A16)
          return false;
      }
    }
  }
  if (f->return_aggregate_size > NYIR_ARG_AGG_SIZE_MASK ||
      f->return_aggregate_classes[0] > NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
      f->return_aggregate_classes[1] > NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
      (f->return_aggregate_size == 0 &&
       (f->return_aggregate_classes[0] || f->return_aggregate_classes[1])))
    return false;
  if (f->source_param_count > NYIR_BINARY_MAX_PARAMS ||
      (f->source_param_count && !f->source_param_widths) ||
      !nyr_write_u32le(out, (uint32_t)f->source_param_count))
    return false;
  for (size_t i = 0; i < f->source_param_count; ++i)
    if (f->source_param_widths[i] < 1 || f->source_param_widths[i] > 3 ||
        !nyr_write_u8(out, f->source_param_widths[i]))
      return false;
  /*
   * Function-entry aggregate ABI + function return ABI (v19+).
   */
  uint8_t has_param_abi = 0;
  for (size_t i = 0; i < f->param_count; ++i)
    if (f->param_types && f->param_types[i] == NYIR_PARAM_AGGREGATE)
      has_param_abi = 1;
  if (has_param_abi && !f->param_abi)
    return false;
  if (!nyr_write_u8(out, has_param_abi))
    return false;
  if (has_param_abi) {
    for (size_t i = 0; i < f->param_count; ++i)
      if (!nyr_write_u32le(out, f->param_abi[i]))
        return false;
  }
  if (!nyr_write_u32le(out, f->return_aggregate_size) ||
      !nyr_write_u8(out, f->return_aggregate_classes[0]) ||
      !nyr_write_u8(out, f->return_aggregate_classes[1]))
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op >= NYIR_OP_COUNT || in->extra_args_len > UINT32_MAX ||
        in->phi_incoming_len > UINT32_MAX)
      return false;
    if (in->arg_sizes &&
        (in->op != NYIR_CALL || in->imm <= 0 || in->imm > NYIR_CALL_MAX_ARGS))
      return false;
    if ((in->op != NYIR_PHI && in->phi_incoming_len != 0) ||
        (in->op == NYIR_PHI && !in->phi_incoming))
      return false;
    if (!nyr_write_u16le(out, (uint16_t)in->op) ||
        !nyr_write_u16le(out, (uint16_t)in->cmp) ||
        !nyr_write_i32le(out, in->dst) || !nyr_write_i32le(out, in->a) ||
        !nyr_write_i32le(out, in->b) || !nyr_write_i32le(out, in->c) ||
        !nyr_write_i32le(out, in->d) || !nyr_write_i32le(out, in->e) ||
        !nyr_write_i32le(out, in->f) || !nyr_write_i64le(out, in->imm) ||
        !nyr_write_u32le(out, in->flags) ||
        !nyr_write_u32le(out, in->effects) ||
        !nyr_write_u32le(out, in->debug.line) ||
        !nyr_write_u32le(out, in->debug.column) ||
        !nyr_write_u8(out, in->range.has_min ? 1 : 0) ||
        !nyr_write_u8(out, in->range.has_max ? 1 : 0) ||
        !nyr_write_i64le(out, in->range.min) ||
        !nyr_write_i64le(out, in->range.max) ||
        !nyr_write_str(out, in->debug.file) ||
        !nyr_write_str(out, in->symbol) ||
        !nyr_write_u32le(out, (uint32_t)in->extra_args_len))
      return false;
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (!nyr_write_i32le(out, in->extra_args[k]))
        return false;
    }
    uint32_t arg_sizes_len = in->arg_sizes ? (uint32_t)in->imm : 0;
    if (!nyr_write_u32le(out, arg_sizes_len))
      return false;
    for (uint32_t k = 0; k < arg_sizes_len; ++k) {
      if (!nyr_write_u32le(out, in->arg_sizes[k]))
        return false;
    }
    if (!nyr_write_u32le(out, in->ret_aggregate_size) ||
        !nyr_write_u8(out, in->ret_aggregate_classes[0]) ||
        !nyr_write_u8(out, in->ret_aggregate_classes[1]) ||
        !nyr_write_u8(out, (uint8_t)in->scalar_class))
      return false;
    if (in->op == NYIR_CALL) {
      uint8_t class_words = 0;
      for (uint8_t k = 0; k < NYIR_CALL_MAX_ARGS / 16; ++k)
        if (in->arg_scalar_classes[k])
          class_words = (uint8_t)(k + 1);
      if (!nyr_write_u8(out, class_words))
        return false;
      for (uint8_t k = 0; k < class_words; ++k)
        if (!nyr_write_i64le(out, (int64_t)in->arg_scalar_classes[k]))
          return false;
      uint8_t ownership_words = 0;
      for (uint8_t k = 0; k < NYIR_CALL_MAX_ARGS / 32; ++k)
        if (in->arg_ownership[k])
          ownership_words = (uint8_t)(k + 1);
      if (!nyr_write_u8(out, ownership_words))
        return false;
      for (uint8_t k = 0; k < ownership_words; ++k)
        if (!nyr_write_i64le(out, (int64_t)in->arg_ownership[k]))
          return false;
      uint8_t contract_words = 0;
      for (uint8_t k = 0; k < NYIR_CALL_MAX_ARGS / 32; ++k)
        if (in->arg_contracts[k])
          contract_words = (uint8_t)(k + 1);
      if (!nyr_write_u8(out, contract_words))
        return false;
      for (uint8_t k = 0; k < contract_words; ++k)
        if (!nyr_write_i64le(out, (int64_t)in->arg_contracts[k]))
          return false;
    }
    if (!nyr_write_u32le(out, (uint32_t)in->phi_incoming_len))
      return false;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (!nyr_write_i64le(out, in->phi_incoming[k].predecessor_label) ||
          !nyr_write_i32le(out, in->phi_incoming[k].value))
        return false;
  }
  return true;
}

bool nyr_load_binary(FILE *in, nyr_func_t *out, char *name, size_t name_len,
                      char *err, size_t err_len) {
  if (!in || !out)
    return nyr_err(err, err_len, "native NYIR load: missing input");
  nyr_func_t loaded = {0};
  char magic[4];
  uint16_t version = 0;
  uint16_t flags = 0;
  char *loaded_name = NULL;
  int32_t next_value = 0;
  uint32_t inst_count = 0;

  if (!nyr_read_exact(in, magic, sizeof(magic)) ||
      memcmp(magic, "NYIR", 4) != 0)
    goto malformed;
  if (!nyr_read_u16le(in, &version) || !nyr_read_u16le(in, &flags))
    goto malformed;
  if (version < NYIR_BINARY_MIN_VERSION || version > NYIR_BINARY_VERSION)
    return nyr_err(err, err_len, "native NYIR load: unsupported version %u",
                    (unsigned)version);
  if (flags != 0)
    return nyr_err(err, err_len, "native NYIR load: unsupported flags 0x%x",
                    (unsigned)flags);
  if (!nyr_read_str(in, &loaded_name, 1024) ||
      !nyr_read_i32le(in, &next_value) || !nyr_read_u32le(in, &inst_count))
    goto malformed;
  if (next_value < 0 || next_value > NYIR_BINARY_MAX_VALUES ||
      inst_count > NYIR_BINARY_MAX_INSTRUCTIONS)
    return nyr_err(err, err_len,
                    "native NYIR load: artifact exceeds decode budget");
  if (version >= NYIR_PARAM_TYPES_VERSION) {
    uint32_t param_count = 0;
    if (!nyr_read_u32le(in, &param_count) || param_count > INT32_MAX ||
        param_count > NYIR_BINARY_MAX_PARAMS)
      goto malformed;
    if (param_count) {
      loaded.param_types =
          malloc((size_t)param_count * sizeof(*loaded.param_types));
      if (!loaded.param_types) {
        free(loaded_name);
        return nyr_err(err, err_len, NY_NATIVE_LOAD_OOM);
      }
      loaded.param_count = param_count;
      for (uint32_t i = 0; i < param_count; ++i) {
        uint8_t type = 0;
        if (!nyr_read_exact(in, &type, 1) || type > NYIR_PARAM_AGGREGATE)
          goto malformed;
        loaded.param_types[i] = (nyr_param_type_t)type;
      }
    }
  }
  if (version >= NYIR_SOURCE_PARAM_WIDTHS_VERSION) {
    uint32_t source_count = 0;
    if (!nyr_read_u32le(in, &source_count) ||
        source_count > NYIR_BINARY_MAX_PARAMS)
      goto malformed;
    if (source_count) {
      loaded.source_param_widths = malloc(source_count);
      if (!loaded.source_param_widths) {
        free(loaded_name);
        nyr_ir_func_free(&loaded);
        return nyr_err(err, err_len, NY_NATIVE_LOAD_OOM);
      }
      loaded.source_param_count = source_count;
      for (uint32_t i = 0; i < source_count; ++i)
        if (!nyr_read_exact(in, &loaded.source_param_widths[i], 1) ||
            loaded.source_param_widths[i] < 1 ||
            loaded.source_param_widths[i] > 3)
          goto malformed;
    }
  }
  if (version >= NYIR_PARAM_AGG_VERSION) {
    uint8_t has_param_abi = 0;
    if (!nyr_read_exact(in, &has_param_abi, 1))
      goto malformed;
    if (has_param_abi) {
      if (!loaded.param_count || loaded.param_count > NYIR_BINARY_MAX_PARAMS)
        goto malformed;
      loaded.param_abi = malloc((size_t)loaded.param_count *
                                sizeof(*loaded.param_abi));
      if (!loaded.param_abi) {
        free(loaded_name);
        nyr_ir_func_free(&loaded);
        return nyr_err(err, err_len, NY_NATIVE_LOAD_OOM);
      }
      for (uint32_t i = 0; i < loaded.param_count; ++i) {
        if (!nyr_read_u32le(in, &loaded.param_abi[i]))
          goto malformed;
        uint32_t packed = loaded.param_abi[i];
        if (NYIR_ARG_AGG_CLASS(packed, 0) > NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
            NYIR_ARG_AGG_CLASS(packed, 1) > NYIR_ARG_CLASS_AAPCS_INTEGER_A16)
          goto malformed;
      }
    }
    if (!nyr_read_u32le(in, &loaded.return_aggregate_size) ||
        !nyr_read_exact(in, &loaded.return_aggregate_classes[0], 1) ||
        !nyr_read_exact(in, &loaded.return_aggregate_classes[1], 1))
      goto malformed;
    if (loaded.return_aggregate_size > NYIR_ARG_AGG_SIZE_MASK ||
        loaded.return_aggregate_classes[0] > NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
        loaded.return_aggregate_classes[1] > NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
        (loaded.return_aggregate_size == 0 &&
         (loaded.return_aggregate_classes[0] ||
          loaded.return_aggregate_classes[1])))
      goto malformed;
  }
  if (inst_count > (uint32_t)(SIZE_MAX / sizeof(*loaded.data)))
    return nyr_err(err, err_len,
                    "native NYIR load: instruction count too large");
  if (inst_count > 0) {
    loaded.data = (nyr_inst_t *)calloc(inst_count, sizeof(*loaded.data));
    if (!loaded.data) {
      free(loaded_name);
      return nyr_err(err, err_len, NY_NATIVE_LOAD_OOM);
    }
    loaded.cap = inst_count;
  }
  loaded.next_value = next_value;
  size_t aux_entries = 0;
  size_t owned_string_bytes = loaded_name ? strlen(loaded_name) + 1 : 0;
  if (owned_string_bytes > NYIR_BINARY_MAX_STRING_BYTES)
    goto malformed;

  for (uint32_t i = 0; i < inst_count; ++i) {
    nyr_inst_t inst = {
        .dst = -1, .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1};
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
    if (!nyr_read_u16le(in, &op) || !nyr_read_u16le(in, &cmp) ||
        !nyr_read_i32le(in, &dst) || !nyr_read_i32le(in, &a) ||
        !nyr_read_i32le(in, &b))
      goto malformed;
    if (version >= 3) {
      if (!nyr_read_i32le(in, &inst.c) || !nyr_read_i32le(in, &inst.d))
        goto malformed;
    }
    if (version >= 4) {
      if (!nyr_read_i32le(in, &inst.e) || !nyr_read_i32le(in, &inst.f))
        goto malformed;
    }
    if (!nyr_read_i64le(in, &inst.imm) || !nyr_read_u32le(in, &flags32))
      goto malformed;
    if (!nyr_decode_opcode(op, version, &op))
      goto malformed;
    if (version >= 2) {
      if (!nyr_read_u32le(in, &effects32) ||
          !nyr_read_u32le(in, &debug_line) ||
          !nyr_read_u32le(in, &debug_column) ||
          !nyr_read_exact(in, &has_min, 1) ||
          !nyr_read_exact(in, &has_max, 1) ||
          !nyr_read_i64le(in, &inst.range.min) ||
          !nyr_read_i64le(in, &inst.range.max) ||
          !nyr_read_str(in, &debug_file, 4096))
        goto malformed;
    }
    if (!nyr_read_str(in, &symbol, 4096)) {
      free(debug_file);
      goto malformed;
    }
    uint32_t extra_len = 0;
    int *extra = NULL;
    uint32_t *arg_sizes = NULL;
    if (version >= 5) {
      if (!nyr_read_u32le(in, &extra_len) || extra_len > NYIR_CALL_MAX_ARGS ||
          aux_entries > NYIR_BINARY_MAX_AUX_ENTRIES - extra_len) {
        free(debug_file);
        free(symbol);
        goto malformed;
      }
      aux_entries += extra_len;
      if (extra_len > 0) {
        extra = (int *)malloc((size_t)extra_len * sizeof(*extra));
        if (!extra) {
          free(symbol);
          free(loaded_name);
          nyr_ir_func_free(&loaded);
          return nyr_err(err, err_len, NY_NATIVE_LOAD_OOM);
        }
        for (uint32_t k = 0; k < extra_len; ++k) {
          int32_t v = 0;
          if (!nyr_read_i32le(in, &v)) {
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
      if (!nyr_read_u32le(in, &arg_sizes_len) ||
          (arg_sizes_len != 0 && (op != NYIR_CALL || inst.imm <= 0 ||
                                  arg_sizes_len != (uint32_t)inst.imm ||
                                  arg_sizes_len > NYIR_CALL_MAX_ARGS)) ||
          aux_entries > NYIR_BINARY_MAX_AUX_ENTRIES - arg_sizes_len) {
        free(debug_file);
        free(extra);
        free(symbol);
        goto malformed;
      }
      aux_entries += arg_sizes_len;
      if (arg_sizes_len > 0) {
        arg_sizes =
            (uint32_t *)malloc((size_t)arg_sizes_len * sizeof(*arg_sizes));
        if (!arg_sizes) {
          free(extra);
          free(symbol);
          free(loaded_name);
          nyr_ir_func_free(&loaded);
          return nyr_err(err, err_len, NY_NATIVE_LOAD_OOM);
        }
        for (uint32_t k = 0; k < arg_sizes_len; ++k) {
          if (!nyr_read_u32le(in, &arg_sizes[k])) {
            free(arg_sizes);
            free(extra);
            free(symbol);
            goto malformed;
          }
          if (version < 7 && arg_sizes[k] > 0)
            arg_sizes[k] = (arg_sizes[k] & NYIR_ARG_AGG_SIZE_MASK) |
                           (NYIR_ARG_CLASS_MEMORY << NYIR_ARG_AGG_CLASS0_SHIFT);
        }
      }
    }
    if (version >= 14 &&
        (!nyr_read_u32le(in, &inst.ret_aggregate_size) ||
         !nyr_read_exact(in, &inst.ret_aggregate_classes[0], 1) ||
         !nyr_read_exact(in, &inst.ret_aggregate_classes[1], 1))) {
      free(arg_sizes);
      free(extra);
      free(symbol);
      goto malformed;
    }
    if (version >= 15) {
      uint8_t scalar_class = 0;
      if (!nyr_read_exact(in, &scalar_class, 1) ||
          scalar_class > NYIR_SCALAR_AGGREGATE) {
        free(arg_sizes);
        free(extra);
        free(symbol);
        goto malformed;
      }
      inst.scalar_class = (nyr_scalar_class_t)scalar_class;
    }
    /*
     * Older artifacts overloaded instruction flags for floating call/return
     * classes. Normalize that side data at the file boundary so an active
     * NYIR instruction carries its ABI class only in scalar_class.
     */
    if (version < NYIR_BINARY_VERSION && (op == NYIR_CALL || op == NYIR_RET)) {
      const uint32_t legacy_class_bits =
          NYIR_LEGACY_INST_F_RET_F64 | NYIR_LEGACY_INST_F_RET_F32;
      uint32_t legacy_class = flags32 & legacy_class_bits;
      if (legacy_class == legacy_class_bits) {
        inst.scalar_class = (nyr_scalar_class_t)(NYIR_SCALAR_AGGREGATE + 1);
      } else if (inst.scalar_class == NYIR_SCALAR_UNKNOWN &&
                 legacy_class == NYIR_LEGACY_INST_F_RET_F64) {
        inst.scalar_class = NYIR_SCALAR_F64;
      } else if (inst.scalar_class == NYIR_SCALAR_UNKNOWN &&
                 legacy_class == NYIR_LEGACY_INST_F_RET_F32) {
        inst.scalar_class = NYIR_SCALAR_F32;
      }
      flags32 &= ~legacy_class_bits;
    }
    if (version >= NYIR_CALL_SCALAR_CLASSES_VERSION && op == NYIR_CALL) {
      uint8_t class_words = 0;
      if (!nyr_read_exact(in, &class_words, 1) ||
          class_words > NYIR_CALL_MAX_ARGS / 16) {
        free(arg_sizes);
        free(extra);
        free(symbol);
        goto malformed;
      }
      for (uint8_t k = 0; k < class_words; ++k) {
        int64_t packed = 0;
        if (!nyr_read_i64le(in, &packed)) {
          free(arg_sizes);
          free(extra);
          free(symbol);
          goto malformed;
        }
        inst.arg_scalar_classes[k] = (uint64_t)packed;
      }
    }
    if (version >= NYIR_CALL_OWNERSHIP_VERSION && op == NYIR_CALL) {
      uint8_t ownership_words = 0;
      if (!nyr_read_exact(in, &ownership_words, 1) ||
          ownership_words > NYIR_CALL_MAX_ARGS / 32) {
        free(arg_sizes);
        free(extra);
        free(symbol);
        goto malformed;
      }
      for (uint8_t k = 0; k < ownership_words; ++k) {
        int64_t packed = 0;
        if (!nyr_read_i64le(in, &packed)) {
          free(arg_sizes);
          free(extra);
          free(symbol);
          goto malformed;
        }
        inst.arg_ownership[k] = (uint64_t)packed;
      }
    }
    if (version >= NYIR_CALL_CONTRACTS_VERSION && op == NYIR_CALL) {
      uint8_t contract_words = 0;
      if (!nyr_read_exact(in, &contract_words, 1) ||
          contract_words > NYIR_CALL_MAX_ARGS / 32) {
        free(arg_sizes);
        free(extra);
        free(symbol);
        goto malformed;
      }
      for (uint8_t k = 0; k < contract_words; ++k) {
        int64_t packed = 0;
        if (!nyr_read_i64le(in, &packed)) {
          free(arg_sizes);
          free(extra);
          free(symbol);
          goto malformed;
        }
        inst.arg_contracts[k] = (uint64_t)packed;
      }
    }
    /*
     * v1-v15 encoded the first six floating call arguments in flag bits.
     * Upgrade them to the typed argument-class record at the load boundary
     * so verifiers and backends only need to understand the canonical form.
     */
    if (version < NYIR_CALL_SCALAR_CLASSES_VERSION && op == NYIR_CALL) {
      const uint32_t legacy_float_arg_bits = 0xfc00u;
      for (unsigned arg = 0; arg < 6; ++arg)
        if (flags32 & (1024u << arg))
          nyr_call_arg_class_set(inst.arg_scalar_classes, arg,
                                  NYIR_SCALAR_F64);
      inst.flags &= ~legacy_float_arg_bits;
    }
    uint32_t phi_len = 0;
    nyr_phi_incoming_t *phi = NULL;
    if (version >= 8 &&
        (!nyr_read_u32le(in, &phi_len) || phi_len > inst_count ||
         aux_entries > NYIR_BINARY_MAX_AUX_ENTRIES - phi_len)) {
      free(debug_file);
      free(arg_sizes);
      free(extra);
      free(symbol);
      goto malformed;
    }
    aux_entries += phi_len;
    if (phi_len) {
      if (op != NYIR_PHI) {
        free(arg_sizes);
        free(extra);
        free(symbol);
        goto malformed;
      }
      phi = malloc((size_t)phi_len * sizeof(*phi));
      if (!phi) {
        free(arg_sizes);
        free(extra);
        free(symbol);
        goto malformed;
      }
      for (uint32_t k = 0; k < phi_len; ++k) {
        int32_t value = 0;
        if (!nyr_read_i64le(in, &phi[k].predecessor_label) ||
            !nyr_read_i32le(in, &value)) {
          free(phi);
          free(arg_sizes);
          free(extra);
          free(symbol);
          goto malformed;
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
    inst.op = (nyr_op_t)op;
    inst.cmp = (nyr_cmp_t)cmp;
    inst.dst = dst;
    inst.a = a;
    inst.b = b;
    inst.flags = flags32;
    inst.effects = effects32;
    inst.debug.line = debug_line;
    inst.debug.column = debug_column;
    inst.range.has_min = has_min != 0;
    inst.range.has_max = has_max != 0;
    size_t debug_bytes = debug_file ? strlen(debug_file) + 1 : 0;
    size_t symbol_bytes = symbol ? strlen(symbol) + 1 : 0;
    if (debug_bytes > NYIR_BINARY_MAX_STRING_BYTES - owned_string_bytes ||
        symbol_bytes >
            NYIR_BINARY_MAX_STRING_BYTES - owned_string_bytes - debug_bytes) {
      free(debug_file);
      free(symbol);
      free(inst.phi_incoming);
      free(inst.arg_sizes);
      free(inst.extra_args);
      goto malformed;
    }
    owned_string_bytes += debug_bytes + symbol_bytes;
    /*
     * Version 8 serialized the earlier, coarser pointer-memory effect
     * classification.  Recompute it before verification so valid v8 IR
     * remains loadable while v9 writes the precise canonical mask.
     */
    if (version == 8)
      inst.effects = nyr_inst_effects(&inst);
    if (debug_file && debug_file[0]) {
      inst.debug.file = nyr_func_own_symbol(&loaded, debug_file);
      if (!inst.debug.file) {
        free(inst.phi_incoming);
        free(inst.arg_sizes);
        free(inst.extra_args);
        free(symbol);
        free(loaded_name);
        nyr_ir_func_free(&loaded);
        return nyr_err(err, err_len, NY_NATIVE_LOAD_OOM);
      }
    } else {
      free(debug_file);
    }
    if (symbol[0]) {
      inst.symbol = nyr_func_own_symbol(&loaded, symbol);
      if (!inst.symbol) {
        free(inst.phi_incoming);
        free(inst.arg_sizes);
        free(inst.extra_args);
        free(loaded_name);
        nyr_ir_func_free(&loaded);
        return nyr_err(err, err_len, NY_NATIVE_LOAD_OOM);
      }
    } else {
      free(symbol);
    }
    loaded.data[loaded.len++] = inst;
    if (inst.op == NYIR_CALL && inst.symbol)
      loaded.has_symbol_calls = true;
  }

  if (!nyr_ir_verify(&loaded, err, err_len)) {
    free(loaded_name);
    nyr_ir_func_free(&loaded);
    return false;
  }
  nyr_refresh_metadata(&loaded);
  if (name && name_len > 0) {
    snprintf(name, name_len, "%s", loaded_name ? loaded_name : "");
  }
  free(loaded_name);
  nyr_ir_func_free(out);
  *out = loaded;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;

malformed:
  free(loaded_name);
  nyr_ir_func_free(&loaded);
  return nyr_err(err, err_len, "native NYIR load: malformed binary dump");
}
