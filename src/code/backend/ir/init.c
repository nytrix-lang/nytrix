/*
 * NYIR init: bootstraps the NYIR subsystem by amalgamating core,
 * binary, eval, verify, ssa, regalloc, machine, and isle sources.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "base/util.h"
#include "code/backend/internal.h"
#include "code/backend/ir/internal.h"
#include "code/backend/object/internal.h"
#include <inttypes.h>
#include <nyr/ir.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *nyr_scalar_class_name(nyr_scalar_class_t scalar_class) {
  switch (scalar_class) {
  case NYIR_SCALAR_I64_RAW:
    return "i64-raw";
  case NYIR_SCALAR_F32:
    return "f32";
  case NYIR_SCALAR_F64:
    return "f64";
  case NYIR_SCALAR_I64_TAGGED:
    return "i64-tagged";
  case NYIR_SCALAR_STRING:
    return "string";
  case NYIR_SCALAR_TYPED_BUFFER:
    return "typed-buffer";
  case NYIR_SCALAR_OBJECT:
    return "object";
  case NYIR_SCALAR_POINTER:
    return "pointer";
  case NYIR_SCALAR_CLOSURE:
    return "closure";
  case NYIR_SCALAR_AGGREGATE:
    return "aggregate";
  case NYIR_SCALAR_UNKNOWN:
  default:
    return "unknown";
  }
}

bool nyr_call_args(const nyr_inst_t *in, int value_count, int *args,
                    size_t args_cap, int *argc_out, char *err, size_t err_len) {
  if (!in || in->op != NYIR_CALL || !args || !argc_out) {
    if (err && err_len)
      snprintf(err, err_len, "native NYIR call: invalid input");
    return false;
  }
  int argc = (int)in->imm;
  if (argc < 0 || argc > NYIR_CALL_MAX_ARGS || (size_t)argc > args_cap) {
    if (err && err_len)
      snprintf(err, err_len,
               "native NYIR call: argument count %d exceeds capacity %zu", argc,
               args_cap);
    return false;
  }
  const int inline_args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (int i = 0; i < argc; ++i) {
    int value = i < 6 ? inline_args[i]
                : in->extra_args && (size_t)(i - 6) < in->extra_args_len
                    ? in->extra_args[i - 6]
                    : -1;
    if (value < 0 || value >= value_count) {
      if (err && err_len)
        snprintf(err, err_len,
                 "native NYIR call: invalid argument %d value v%d", i, value);
      return false;
    }
    args[i] = value;
  }
  *argc_out = argc;
  return true;
}

static bool nyr_op_v128_i64(nyr_op_t op) {
  return op == NYIR_VEC4_LOAD_I64 || op == NYIR_VEC4_SET1_I64 ||
         op == NYIR_VEC4_ADD_I64 || op == NYIR_VEC4_SUB_I64 ||
         op == NYIR_VEC4_AND_I64 || op == NYIR_VEC4_OR_I64 ||
         op == NYIR_VEC4_XOR_I64 || op == NYIR_VEC4_SHL_I64 ||
         op == NYIR_VEC4_SAR_I64;
}

/*
 * VEC8_I64 is classified as v128 in the type map until backends emit
 * distinct 256-bit register classes.
 */
static bool nyr_op_v256_i64(nyr_op_t op) {
  return op == NYIR_VEC8_LOAD_I64 || op == NYIR_VEC8_ADD_I64 ||
         op == NYIR_VEC8_SUB_I64 || op == NYIR_VEC8_AND_I64 ||
         op == NYIR_VEC8_OR_I64 || op == NYIR_VEC8_XOR_I64;
}

/*
 * VEC4_F64 carries four f64 lanes (256 bits), matching the TV SMT model, the
 * NYIR evaluator, and the x86-64 NYIR encoder. The historical v128_f64
 * classification truncated the upper half.
 */
static bool nyr_op_v256_f64(nyr_op_t op) {
  return op == NYIR_VEC4_LOAD_F64 || op == NYIR_VEC4_ADD_F64 ||
         op == NYIR_VEC4_SUB_F64 || op == NYIR_VEC4_MUL_F64 ||
         op == NYIR_VEC4_DIV_F64 || op == NYIR_VEC4_FMA_F64 ||
         op == NYIR_VEC4_SET1_F64 || op == NYIR_VEC4_SHUFFLE_F64;
}

static bool nyr_op_v128_f32(nyr_op_t op) {
  return op == NYIR_VEC8_LOAD_F32 || op == NYIR_VEC8_ADD_F32 ||
         op == NYIR_VEC8_SUB_F32 || op == NYIR_VEC8_MUL_F32 ||
         op == NYIR_VEC8_DIV_F32 || op == NYIR_VEC8_FMA_F32 ||
         op == NYIR_VEC8_SET1_F32 || op == NYIR_VEC8_SHUFFLE_F32;
}

static size_t nyr_type_root(size_t *parents, size_t node) {
  size_t root = node;
  while (parents[root] != root)
    root = parents[root];
  while (parents[node] != node) {
    size_t next = parents[node];
    parents[node] = root;
    node = next;
  }
  return root;
}

static void nyr_type_union(size_t *parents, size_t left, size_t right) {
  left = nyr_type_root(parents, left);
  right = nyr_type_root(parents, right);
  if (left != right)
    parents[right] = left;
}

void nyr_type_map_free(nyr_type_map_t *map) {
  if (!map)
    return;
  free(map->storage);
  *map = (nyr_type_map_t){0};
}

bool nyr_type_map_init(nyr_type_map_t *map, const nyr_func_t *nyir,
                        size_t local_count) {
  if (!map || !nyir)
    return false;
  if (nyir->param_count && !nyir->param_types)
    return false;
  *map = (nyr_type_map_t){.value_count = (size_t)nyir->next_value,
                           .local_count = local_count};
  size_t flag_count = map->value_count + local_count;
  if (flag_count < map->value_count ||
      flag_count > SIZE_MAX / (14 * sizeof(bool))) {
    nyr_type_map_free(map);
    return false;
  }
  if (flag_count) {
    map->storage = calloc(flag_count * 14, sizeof(bool));
    if (!map->storage) {
      nyr_type_map_free(map);
      return false;
    }
    bool **value_flags[] = {&map->value_f64,      &map->value_f32,
                            &map->value_v128_i64, &map->value_v256_i64,
                            &map->value_v128_f64, &map->value_v128_f32,
                            &map->value_v256_f64};
    bool **local_flags[] = {&map->local_f64,      &map->local_f32,
                            &map->local_v128_i64, &map->local_v256_i64,
                            &map->local_v128_f64, &map->local_v128_f32,
                            &map->local_v256_f64};
    for (size_t i = 0; i < 7; ++i) {
      *value_flags[i] = map->storage + i * flag_count;
      *local_flags[i] = map->storage + i * flag_count + map->value_count;
    }
  }
  if ((map->value_count &&
       (!map->value_f64 || !map->value_f32 || !map->value_v128_i64 ||
        !map->value_v256_i64 || !map->value_v128_f64 || !map->value_v128_f32 ||
        !map->value_v256_f64)) ||
      (local_count &&
       (!map->local_f64 || !map->local_f32 || !map->local_v128_i64 ||
        !map->local_v256_i64 || !map->local_v128_f64 || !map->local_v128_f32 ||
        !map->local_v256_f64))) {
    nyr_type_map_free(map);
    return false;
  }

  size_t param_count =
      nyir->param_count < local_count ? nyir->param_count : local_count;
  bool has_typed_values = false;
  for (size_t i = 0; i < param_count; ++i)
    has_typed_values |= nyir->param_types[i] == NYIR_PARAM_F64 ||
                        nyir->param_types[i] == NYIR_PARAM_F32;
  for (size_t i = 0; i < nyir->len && !has_typed_values; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    has_typed_values =
        in->scalar_class == NYIR_SCALAR_F64 ||
        in->scalar_class == NYIR_SCALAR_F32 || nyr_op_is_f64(in->op) ||
        nyr_op_is_f32(in->op) || nyr_op_v128_i64(in->op) ||
        nyr_op_v256_i64(in->op) || nyr_op_v256_f64(in->op) ||
        nyr_op_v128_f32(in->op) ||
        (in->op == NYIR_CAPTURE_RET && in->imm >= 10 && in->imm <= 13);
  }
  if (!has_typed_values)
    return true;

  size_t node_count = flag_count;
  if (node_count > SIZE_MAX / (sizeof(size_t) + sizeof(unsigned char))) {
    nyr_type_map_free(map);
    return false;
  }
  void *workspace =
      node_count ? malloc(node_count * (sizeof(size_t) + sizeof(unsigned char)))
                 : NULL;
  if (node_count && !workspace) {
    nyr_type_map_free(map);
    return false;
  }
  size_t *parents = workspace;
  unsigned char *root_types =
      node_count ? (unsigned char *)(parents + node_count) : NULL;
  if (node_count)
    memset(root_types, 0, node_count * sizeof(*root_types));
  for (size_t i = 0; i < node_count; ++i)
    parents[i] = i;

  /*
   * Copies and unclassified local loads/stores preserve type. An explicit
   * scalar_class on a local access is the representation contract for that
   * value; do not let a different store on another control-flow path widen
   * that value through the shared slot. Collapse compatible constraints once
   * so long chains remain near-linear instead of rescanning the function.
   */
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
        (size_t)in->dst < map->value_count && (size_t)in->a < map->value_count)
      nyr_type_union(parents, (size_t)in->dst, (size_t)in->a);
    if (in->op == NYIR_PHI && in->dst >= 0 &&
        (size_t)in->dst < map->value_count) {
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int incoming = in->phi_incoming[k].value;
        if (incoming >= 0 && (size_t)incoming < map->value_count)
          nyr_type_union(parents, (size_t)in->dst, (size_t)incoming);
      }
    }
    if (in->op == NYIR_LOAD_LOCAL && in->scalar_class == NYIR_SCALAR_UNKNOWN &&
        in->dst >= 0 && in->imm >= 0 && (size_t)in->dst < map->value_count &&
        (size_t)in->imm < local_count)
      nyr_type_union(parents, (size_t)in->dst,
                      map->value_count + (size_t)in->imm);
    if (in->op == NYIR_STORE_LOCAL && in->scalar_class == NYIR_SCALAR_UNKNOWN &&
        in->a >= 0 && in->imm >= 0 && (size_t)in->a < map->value_count &&
        (size_t)in->imm < local_count)
      nyr_type_union(parents, (size_t)in->a,
                      map->value_count + (size_t)in->imm);
  }

  for (size_t i = 0; i < param_count; ++i) {
    size_t root = nyr_type_root(parents, map->value_count + i);
    if (nyir->param_types[i] == NYIR_PARAM_F64)
      root_types[root] |= 1u;
    else if (nyir->param_types[i] == NYIR_PARAM_F32)
      root_types[root] |= 2u;
  }

  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    bool f64_result =
        in->scalar_class == NYIR_SCALAR_F64 || nyr_op_is_f64(in->op);
    bool f32_result =
        in->scalar_class == NYIR_SCALAR_F32 || nyr_op_is_f32(in->op);
    /*
     * A classified store declares the slot's carrier on that path. Seed the
     * local root with it so an unclassified load in a merge block (where the
     * reaching store lives in a predecessor) still observes the float fact.
     * Mixed-class slots surface through the store/load ABI checks instead.
     */
    if (in->op == NYIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
        (size_t)in->imm < local_count) {
      size_t local_root =
          nyr_type_root(parents, map->value_count + (size_t)in->imm);
      if (in->scalar_class == NYIR_SCALAR_F64)
        root_types[local_root] |= 1u;
      else if (in->scalar_class == NYIR_SCALAR_F32)
        root_types[local_root] |= 2u;
    }
    bool v128_i64_result =
        nyr_op_v128_i64(in->op) ||
        (in->op == NYIR_CAPTURE_RET && in->imm >= 10 && in->imm <= 13);
    bool v256_i64_result = nyr_op_v256_i64(in->op);
    bool v256_f64_result = nyr_op_v256_f64(in->op);
    bool v128_f32_result = nyr_op_v128_f32(in->op);
    if (in->dst >= 0 && (size_t)in->dst < map->value_count) {
      size_t root = nyr_type_root(parents, (size_t)in->dst);
      if (f64_result)
        root_types[root] |= 1u;
      if (f32_result)
        root_types[root] |= 2u;
      if (v128_i64_result)
        root_types[root] |= 4u;
      if (v256_i64_result)
        root_types[root] |= 4u | 32u;
      if (v256_f64_result)
        root_types[root] |= 64u;
      if (v128_f32_result)
        root_types[root] |= 16u;
    }
    /*
     * RET's scalar_class describes the ABI contract. It must not be fed back
     * into the producer's type facts, or a mismatched annotation would make
     * the producer appear to have both float widths.
     */
    bool f64_operands = in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
                        in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64 ||
                        in->op == NYIR_CMP_F64 || in->op == NYIR_F64_TO_F32 ||
                        in->op == NYIR_SQRT_F64 || in->op == NYIR_SIN_F64 ||
                        in->op == NYIR_COS_F64;
    bool f32_operands = in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
                        in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32 ||
                        in->op == NYIR_CMP_F32 || in->op == NYIR_F32_TO_F64;
    bool v128_i64_ops =
        in->op == NYIR_VEC4_ADD_I64 || in->op == NYIR_VEC4_SUB_I64 ||
        in->op == NYIR_VEC4_AND_I64 || in->op == NYIR_VEC4_OR_I64 ||
        in->op == NYIR_VEC4_XOR_I64 || in->op == NYIR_VEC4_SHL_I64 ||
        in->op == NYIR_VEC4_SAR_I64;
    bool v256_i64_ops =
        in->op == NYIR_VEC8_ADD_I64 || in->op == NYIR_VEC8_SUB_I64 ||
        in->op == NYIR_VEC8_AND_I64 || in->op == NYIR_VEC8_OR_I64 ||
        in->op == NYIR_VEC8_XOR_I64;
    bool v256_f64_ops =
        in->op == NYIR_VEC4_ADD_F64 || in->op == NYIR_VEC4_SUB_F64 ||
        in->op == NYIR_VEC4_MUL_F64 || in->op == NYIR_VEC4_DIV_F64 ||
        in->op == NYIR_VEC4_FMA_F64 || in->op == NYIR_VEC4_SHUFFLE_F64;
    bool v128_f32_ops =
        in->op == NYIR_VEC8_ADD_F32 || in->op == NYIR_VEC8_SUB_F32 ||
        in->op == NYIR_VEC8_MUL_F32 || in->op == NYIR_VEC8_DIV_F32 ||
        in->op == NYIR_VEC8_FMA_F32 || in->op == NYIR_VEC8_SHUFFLE_F32;
    if (in->a >= 0 && (size_t)in->a < map->value_count) {
      size_t root = nyr_type_root(parents, (size_t)in->a);
      if (f64_operands)
        root_types[root] |= 1u;
      if (f32_operands)
        root_types[root] |= 2u;
      if (v128_i64_ops)
        root_types[root] |= 4u;
      if (v256_i64_ops)
        root_types[root] |= 4u | 32u;
      if (v256_f64_ops)
        root_types[root] |= 64u;
      if (v128_f32_ops)
        root_types[root] |= 16u;
    }
    if (in->b >= 0 && (size_t)in->b < map->value_count) {
      size_t root = nyr_type_root(parents, (size_t)in->b);
      if (f64_operands)
        root_types[root] |= 1u;
      if (f32_operands)
        root_types[root] |= 2u;
      if (v128_i64_ops)
        root_types[root] |= 4u;
      if (v256_i64_ops)
        root_types[root] |= 4u | 32u;
      if (v256_f64_ops)
        root_types[root] |= 64u;
      if (v128_f32_ops)
        root_types[root] |= 16u;
    }
    /*
     * FMA's c operand is a vector input; a typed memory store's class is an
     * assertion about the destination layout and must not rewrite the
     * producer's independent scalar fact.
     */
    if (in->c >= 0 && (size_t)in->c < map->value_count &&
        (in->op == NYIR_VEC4_FMA_F64 || in->op == NYIR_VEC8_FMA_F32)) {
      size_t root = nyr_type_root(parents, (size_t)in->c);
      if (in->op == NYIR_VEC4_FMA_F64)
        root_types[root] |= 64u;
      else
        root_types[root] |= 16u;
    }
    if (in->op == NYIR_VEC4_REDUCE_ADD_F64) {
      if (in->a >= 0 && (size_t)in->a < map->value_count) {
        size_t root = nyr_type_root(parents, (size_t)in->a);
        root_types[root] |= 1u;
      }
      if (in->b >= 0 && (size_t)in->b < map->value_count) {
        size_t root = nyr_type_root(parents, (size_t)in->b);
        root_types[root] |= 64u;
      }
    }
    if (in->op == NYIR_VEC4_REDUCE_ADD_I64 ||
        in->op == NYIR_VEC8_REDUCE_ADD_I64) {
      if (in->b >= 0 && (size_t)in->b < map->value_count) {
        size_t root = nyr_type_root(parents, (size_t)in->b);
        root_types[root] |=
            in->op == NYIR_VEC8_REDUCE_ADD_I64 ? (4u | 32u) : 4u;
      }
    }
    if (in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_STORE_I64 ||
        in->op == NYIR_VEC4_STORE_F64 || in->op == NYIR_VEC8_STORE_F32) {
      int value = in->b >= 0 ? in->b : in->c;
      if (value >= 0 && (size_t)value < map->value_count) {
        size_t root = nyr_type_root(parents, (size_t)value);
        if (in->op == NYIR_VEC4_STORE_I64)
          root_types[root] |= 4u;
        else if (in->op == NYIR_VEC8_STORE_I64)
          root_types[root] |= 4u | 32u;
        else if (in->op == NYIR_VEC4_STORE_F64)
          root_types[root] |= 64u;
        else
          root_types[root] |= 16u;
      }
    }
  }

  for (size_t i = 0; i < map->value_count; ++i) {
    unsigned char type = root_types[nyr_type_root(parents, i)];
    map->value_f64[i] = (type & 1u) != 0;
    map->value_f32[i] = (type & 2u) != 0;
    map->value_v128_i64[i] = (type & 4u) != 0;
    map->value_v256_i64[i] = (type & 32u) != 0;
    map->value_v128_f64[i] = (type & 8u) != 0;
    map->value_v128_f32[i] = (type & 16u) != 0;
    map->value_v256_f64[i] = (type & 64u) != 0;
  }
  for (size_t i = 0; i < local_count; ++i) {
    unsigned char type =
        root_types[nyr_type_root(parents, map->value_count + i)];
    map->local_f64[i] = (type & 1u) != 0;
    map->local_f32[i] = (type & 2u) != 0;
    map->local_v128_i64[i] = (type & 4u) != 0;
    map->local_v256_i64[i] = (type & 32u) != 0;
    map->local_v128_f64[i] = (type & 8u) != 0;
    map->local_v128_f32[i] = (type & 16u) != 0;
    map->local_v256_f64[i] = (type & 64u) != 0;
  }
  free(workspace);
  return true;
}

int64_t nyr_f64_to_bits(double v) {
  int64_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

double nyr_bits_to_f64(int64_t bits) {
  double v = 0;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

int64_t nyr_f32_to_bits(float v) {
  int32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return (int64_t)(uint32_t)bits;
}

float nyr_bits_to_f32(int64_t bits) {
  int32_t b32 = (int32_t)(uint32_t)bits;
  float v = 0;
  memcpy(&v, &b32, sizeof(v));
  return v;
}

static bool nyr_next_capacity(size_t current, size_t initial,
                               size_t element_size, size_t *next) {
  if (!next || !element_size)
    return false;
  size_t max_capacity = SIZE_MAX / element_size;
  if (current >= max_capacity)
    return false;
  if (!current) {
    if (!initial || initial > max_capacity)
      return false;
    *next = initial;
    return true;
  }
  *next = current > max_capacity / 2 ? max_capacity : current * 2;
  return *next > current;
}

const char *nyr_func_own_symbol_copy(nyr_func_t *f, const char *symbol) {
  if (!f)
    return NULL;
  if (!symbol)
    return NULL;
  if (f->owned_symbols_len > f->owned_symbols_cap ||
      f->owned_symbols_cap > SIZE_MAX / sizeof(*f->owned_symbols) ||
      (f->owned_symbols_cap && !f->owned_symbols))
    return NULL;
  char *copy = ny_strndup(symbol, strlen(symbol));
  if (!copy)
    return NULL;
  if (f->owned_symbols_len >= f->owned_symbols_cap) {
    size_t cap = 0;
    if (!nyr_next_capacity(f->owned_symbols_cap, 16, sizeof(*f->owned_symbols),
                            &cap)) {
      free(copy);
      return NULL;
    }
    char **data =
        (char **)realloc(f->owned_symbols, cap * sizeof(*f->owned_symbols));
    if (!data) {
      free(copy);
      return NULL;
    }
    f->owned_symbols = data;
    f->owned_symbols_cap = cap;
  }
  f->owned_symbols[f->owned_symbols_len++] = copy;
  return copy;
}

void nyr_ir_func_free(nyr_func_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->owned_symbols_len; ++i)
    free(f->owned_symbols[i]);
  free(f->owned_symbols);
  free(f->param_types);
  free(f->param_abi);
  free(f->source_param_widths);
  for (size_t i = 0; i < f->len; ++i) {
    free(f->data[i].extra_args);
    free(f->data[i].arg_sizes);
    free(f->data[i].phi_incoming);
  }
  free(f->data);
  memset(f, 0, sizeof(*f));
}

bool nyr_ir_func_clone(const nyr_func_t *src, nyr_func_t *dst) {
  if (!src || !dst || src == dst)
    return false;
  if (dst->next_value || dst->data || dst->len || dst->cap ||
      dst->owned_symbols || dst->owned_symbols_len || dst->owned_symbols_cap ||
      dst->param_types || dst->param_abi || dst->param_count ||
      dst->source_param_widths || dst->source_param_count ||
      dst->has_symbol_calls || dst->return_aggregate_size ||
      dst->return_aggregate_classes[0] || dst->return_aggregate_classes[1] ||
      dst->vectorize_attempted_loops || dst->vectorize_rejected_loops ||
      dst->vectorized_loops)
    return false;
  if (src->len > src->cap || src->cap > SIZE_MAX / sizeof(*src->data) ||
      (src->cap && !src->data) ||
      src->param_count > SIZE_MAX / sizeof(*src->param_types) ||
      (src->param_count && !src->param_types) ||
      (src->param_abi && !src->param_count) ||
      (src->source_param_count && !src->source_param_widths) ||
      src->owned_symbols_len > src->owned_symbols_cap ||
      src->owned_symbols_cap > SIZE_MAX / sizeof(*src->owned_symbols) ||
      (src->owned_symbols_cap && !src->owned_symbols))
    return false;
  for (size_t i = 0; i < src->len; ++i) {
    const nyr_inst_t *inst = &src->data[i];
    if ((inst->extra_args_len && !inst->extra_args) ||
        (inst->phi_incoming_len && !inst->phi_incoming) ||
        inst->extra_args_len > SIZE_MAX / sizeof(*inst->extra_args) ||
        inst->phi_incoming_len > SIZE_MAX / sizeof(*inst->phi_incoming) ||
        (inst->arg_sizes &&
         (inst->imm <= 0 || inst->imm > NYIR_CALL_MAX_ARGS ||
          (uint64_t)inst->imm > SIZE_MAX / sizeof(*inst->arg_sizes))))
      return false;
  }
  memset(dst, 0, sizeof(*dst));
  dst->next_value = src->next_value;
  dst->has_symbol_calls = src->has_symbol_calls;
  dst->vectorize_attempted_loops = src->vectorize_attempted_loops;
  dst->vectorize_rejected_loops = src->vectorize_rejected_loops;
  dst->vectorized_loops = src->vectorized_loops;
  if (src->param_count) {
    dst->param_types = malloc(src->param_count * sizeof(*dst->param_types));
    if (!dst->param_types) {
      nyr_ir_func_free(dst);
      return false;
    }
    memcpy(dst->param_types, src->param_types,
           src->param_count * sizeof(*dst->param_types));
    dst->param_count = src->param_count;
  }
  if (src->param_abi) {
    if (src->param_count > SIZE_MAX / sizeof(*dst->param_abi)) {
      nyr_ir_func_free(dst);
      return false;
    }
    dst->param_abi = malloc(src->param_count * sizeof(*dst->param_abi));
    if (!dst->param_abi) {
      nyr_ir_func_free(dst);
      return false;
    }
    memcpy(dst->param_abi, src->param_abi,
           src->param_count * sizeof(*dst->param_abi));
  }
  dst->return_aggregate_size = src->return_aggregate_size;
  dst->return_aggregate_classes[0] = src->return_aggregate_classes[0];
  dst->return_aggregate_classes[1] = src->return_aggregate_classes[1];
  if (src->source_param_count) {
    if (!src->source_param_widths) {
      nyr_ir_func_free(dst);
      return false;
    }
    dst->source_param_widths = malloc(src->source_param_count);
    if (!dst->source_param_widths) {
      nyr_ir_func_free(dst);
      return false;
    }
    memcpy(dst->source_param_widths, src->source_param_widths,
           src->source_param_count);
    dst->source_param_count = src->source_param_count;
  }
  if (src->len == 0)
    return true;
  if (src->len > SIZE_MAX / sizeof(*dst->data)) {
    nyr_ir_func_free(dst);
    return false;
  }
  dst->data = (nyr_inst_t *)calloc(src->len, sizeof(nyr_inst_t));
  if (!dst->data) {
    nyr_ir_func_free(dst);
    return false;
  }
  dst->cap = src->len;
  for (size_t i = 0; i < src->len; ++i) {
    const nyr_inst_t *s = &src->data[i];
    nyr_inst_t d = *s;
    d.extra_args = NULL;
    d.extra_args_len = 0;
    d.arg_sizes = NULL;
    d.phi_incoming = NULL;
    d.phi_incoming_len = 0;
    d.symbol = NULL;
    d.debug.file = NULL;
    if (s->symbol) {
      d.symbol = nyr_func_own_symbol_copy(dst, s->symbol);
      if (!d.symbol) {
        nyr_ir_func_free(dst);
        return false;
      }
    }
    if (s->debug.file && s->debug.file[0]) {
      d.debug.file = nyr_func_own_symbol_copy(dst, s->debug.file);
      if (!d.debug.file) {
        nyr_ir_func_free(dst);
        return false;
      }
    }
    if (s->extra_args && s->extra_args_len > 0) {
      if (s->extra_args_len > SIZE_MAX / sizeof(*d.extra_args)) {
        nyr_ir_func_free(dst);
        return false;
      }
      d.extra_args = (int *)malloc(s->extra_args_len * sizeof(*d.extra_args));
      if (!d.extra_args) {
        nyr_ir_func_free(dst);
        return false;
      }
      memcpy(d.extra_args, s->extra_args,
             s->extra_args_len * sizeof(*d.extra_args));
      d.extra_args_len = s->extra_args_len;
    }
    if (s->arg_sizes && s->imm > 0) {
      size_t arg_count = (size_t)s->imm;
      if (arg_count > SIZE_MAX / sizeof(*d.arg_sizes)) {
        free(d.extra_args);
        nyr_ir_func_free(dst);
        return false;
      }
      d.arg_sizes = (uint32_t *)malloc(arg_count * sizeof(*d.arg_sizes));
      if (!d.arg_sizes) {
        free(d.extra_args);
        nyr_ir_func_free(dst);
        return false;
      }
      memcpy(d.arg_sizes, s->arg_sizes, arg_count * sizeof(*d.arg_sizes));
    }
    if (s->phi_incoming && s->phi_incoming_len > 0) {
      if (s->phi_incoming_len > SIZE_MAX / sizeof(*d.phi_incoming)) {
        free(d.extra_args);
        free(d.arg_sizes);
        nyr_ir_func_free(dst);
        return false;
      }
      d.phi_incoming = (nyr_phi_incoming_t *)malloc(s->phi_incoming_len *
                                                     sizeof(*d.phi_incoming));
      if (!d.phi_incoming) {
        free(d.extra_args);
        free(d.arg_sizes);
        nyr_ir_func_free(dst);
        return false;
      }
      memcpy(d.phi_incoming, s->phi_incoming,
             s->phi_incoming_len * sizeof(*d.phi_incoming));
      d.phi_incoming_len = s->phi_incoming_len;
    }
    dst->data[dst->len++] = d;
  }
  return true;
}

void nyr_func_replace(nyr_func_t *dst, nyr_func_t *src) {
  if (!dst || !src || dst == src)
    return;
  nyr_param_type_t *param_types = dst->param_types;
  uint32_t *param_abi = dst->param_abi;
  uint32_t return_size = dst->return_aggregate_size;
  uint8_t return_classes[2] = {dst->return_aggregate_classes[0],
                               dst->return_aggregate_classes[1]};
  size_t param_count = dst->param_count;
  uint8_t *source_widths = dst->source_param_widths;
  size_t source_count = dst->source_param_count;
  bool keep_param_types = !src->param_types || src->param_types == param_types;
  bool keep_param_abi = !src->param_abi || src->param_abi == param_abi;
  bool keep_return_abi = src->return_aggregate_size == 0 && return_size != 0;
  bool keep_source_widths =
      !src->source_param_widths || src->source_param_widths == source_widths;
  /*
   * Detach only dst-owned buffers so nyr_ir_func_free below cannot release
   * metadata that must survive the replacement.  src is memset() whole at
   * the end, so src-side field zeroing here is both unnecessary and
   * harmful: *dst = *src would copy a zeroed param_count over the live
   * function's signature and every later pass would treat parameter slots
   * as uninitialized locals.
   */
  if (keep_param_types) {
    dst->param_types = NULL;
    if (src->param_types == param_types)
      src->param_types = NULL;
  }
  if (keep_param_abi) {
    dst->param_abi = NULL;
    if (src->param_abi == param_abi)
      src->param_abi = NULL;
  }
  if (keep_source_widths) {
    dst->source_param_widths = NULL;
    if (src->source_param_widths == source_widths)
      src->source_param_widths = NULL;
  }
  nyr_ir_func_free(dst);
  *dst = *src;
  if (keep_param_types) {
    dst->param_types = param_types;
    dst->param_count = param_count;
  }
  if (keep_param_abi)
    dst->param_abi = param_abi;
  if (keep_return_abi) {
    dst->return_aggregate_size = return_size;
    dst->return_aggregate_classes[0] = return_classes[0];
    dst->return_aggregate_classes[1] = return_classes[1];
  }
  if (keep_source_widths) {
    dst->source_param_widths = source_widths;
    dst->source_param_count = source_count;
  }
  memset(src, 0, sizeof(*src));
}

void nyr_inst_discard(nyr_inst_t *in) {
  if (!in)
    return;
  free(in->extra_args);
  free(in->arg_sizes);
  free(in->phi_incoming);
  *in = (nyr_inst_t){.op = NYIR_NOP,
                      .dst = -1,
                      .a = -1,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1};
}

bool nyr_replace_all_uses(nyr_func_t *f, int old_value, int new_value) {
  if (!f || old_value < 0 || new_value < 0 || old_value >= f->next_value ||
      new_value >= f->next_value)
    return false;
  if (old_value == new_value)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    int *operands[] = {&in->a, &in->b, &in->c, &in->d, &in->e, &in->f};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k)
      if (*operands[k] == old_value)
        *operands[k] = new_value;
    for (size_t k = 0; k < in->extra_args_len; ++k)
      if (in->extra_args[k] == old_value)
        in->extra_args[k] = new_value;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value == old_value)
        in->phi_incoming[k].value = new_value;
  }
  return true;
}

bool nyr_erase_instruction(nyr_func_t *f, size_t index) {
  if (!f || index >= f->len)
    return false;
  nyr_inst_discard(&f->data[index]);
  return true;
}

void nyr_use_def_free(nyr_use_def_t *uses) {
  if (!uses)
    return;
  free(uses->offsets);
  free(uses->users);
  *uses = (nyr_use_def_t){0};
}

bool nyr_build_use_def(const nyr_func_t *f, nyr_use_def_t *out) {
  if (!f || !out || f->next_value < 0)
    return false;
  *out = (nyr_use_def_t){0};
  size_t values = (size_t)f->next_value;
  size_t *offsets = calloc(values + 1, sizeof(*offsets));
  if (!offsets)
    return false;
#define NIR_COUNT_USE(v)                                                       \
  do {                                                                         \
    if ((v) >= 0) {                                                            \
      if ((size_t)(v) >= values || offsets[(size_t)(v) + 1] == SIZE_MAX)       \
        goto fail;                                                             \
      ++offsets[(size_t)(v) + 1];                                              \
    }                                                                          \
  } while (0)
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    NIR_COUNT_USE(in->a);
    NIR_COUNT_USE(in->b);
    NIR_COUNT_USE(in->c);
    NIR_COUNT_USE(in->d);
    NIR_COUNT_USE(in->e);
    NIR_COUNT_USE(in->f);
    for (size_t k = 0; k < in->extra_args_len; ++k)
      NIR_COUNT_USE(in->extra_args[k]);
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      NIR_COUNT_USE(in->phi_incoming[k].value);
  }
  for (size_t v = 1; v <= values; ++v) {
    if (offsets[v] > SIZE_MAX - offsets[v - 1])
      goto fail;
    offsets[v] += offsets[v - 1];
  }
  size_t total = offsets[values];
  size_t *users = total ? malloc(total * sizeof(*users)) : NULL;
  size_t *cursor = values ? malloc(values * sizeof(*cursor)) : NULL;
  if ((total && !users) || (values && !cursor)) {
    free(users);
    free(cursor);
    goto fail;
  }
  if (values)
    memcpy(cursor, offsets, values * sizeof(*cursor));
#define NIR_ADD_USE(v)                                                         \
  do {                                                                         \
    if ((v) >= 0)                                                              \
      users[cursor[(size_t)(v)]++] = i;                                        \
  } while (0)
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    NIR_ADD_USE(in->a);
    NIR_ADD_USE(in->b);
    NIR_ADD_USE(in->c);
    NIR_ADD_USE(in->d);
    NIR_ADD_USE(in->e);
    NIR_ADD_USE(in->f);
    for (size_t k = 0; k < in->extra_args_len; ++k)
      NIR_ADD_USE(in->extra_args[k]);
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      NIR_ADD_USE(in->phi_incoming[k].value);
  }
#undef NIR_ADD_USE
#undef NIR_COUNT_USE
  free(cursor);
  out->value_count = values;
  out->use_count = total;
  out->offsets = offsets;
  out->users = users;
  return true;
fail:
#undef NIR_COUNT_USE
  free(offsets);
  return false;
}

const char *nyr_op_name(nyr_op_t op) {
  switch (op) {
  case NYIR_NOP:
    return "nop";
  case NYIR_BOUNDS_CHECK:
    return "bounds.check";
  case NYIR_CONST_I64:
    return "const.i64";
  case NYIR_COPY:
    return "copy";
  case NYIR_PHI:
    return "phi";
  case NYIR_ADD_I64:
    return "add.i64";
  case NYIR_SUB_I64:
    return "sub.i64";
  case NYIR_MUL_I64:
    return "mul.i64";
  case NYIR_DIV_I64:
    return "div.i64";
  case NYIR_MOD_I64:
    return "mod.i64";
  case NYIR_AND_I64:
    return "and.i64";
  case NYIR_OR_I64:
    return "or.i64";
  case NYIR_XOR_I64:
    return "xor.i64";
  case NYIR_SHL_I64:
    return "shl.i64";
  case NYIR_SAR_I64:
    return "sar.i64";
  case NYIR_ROR_I64:
    return "ror.i64";
  case NYIR_ROR32_I64:
    return "ror32.i64";
  case NYIR_BSWAP_I64:
    return "bswap.i64";
  case NYIR_BSWAP32_I64:
    return "bswap32.i64";
  case NYIR_CMP_I64:
    return "cmp.i64";
  case NYIR_LABEL:
    return "label";
  case NYIR_LOAD_LOCAL:
    return "load.local";
  case NYIR_STORE_LOCAL:
    return "store.local";
  case NYIR_CALL:
    return "call";
  case NYIR_RET:
    return "ret";
  case NYIR_BR:
    return "br";
  case NYIR_BR_IF:
    return "br.if";
  case NYIR_CONST_F64:
    return "const.f64";
  case NYIR_ADD_F64:
    return "add.f64";
  case NYIR_SUB_F64:
    return "sub.f64";
  case NYIR_MUL_F64:
    return "mul.f64";
  case NYIR_DIV_F64:
    return "div.f64";
  case NYIR_I64_TO_F64:
    return "i64.to.f64";
  case NYIR_F64_TO_I64:
    return "f64.to.i64";
  case NYIR_F32_TO_I64:
    return "f32.to.i64";
  case NYIR_CMP_F64:
    return "cmp.f64";
  case NYIR_SQRT_F64:
    return "sqrt.f64";
  case NYIR_SIN_F64:
    return "sin.f64";
  case NYIR_COS_F64:
    return "cos.f64";
  case NYIR_CONST_F32:
    return "const.f32";
  case NYIR_ADD_F32:
    return "add.f32";
  case NYIR_SUB_F32:
    return "sub.f32";
  case NYIR_MUL_F32:
    return "mul.f32";
  case NYIR_DIV_F32:
    return "div.f32";
  case NYIR_I64_TO_F32:
    return "i64.to.f32";
  case NYIR_F64_TO_F32:
    return "f64.to.f32";
  case NYIR_F32_TO_F64:
    return "f32.to.f64";
  case NYIR_CMP_F32:
    return "cmp.f32";
  case NYIR_ADDR_LOCAL:
    return "addr.local";
  case NYIR_LOAD_I64:
    return "load.i64";
  case NYIR_STORE_I64:
    return "store.i64";
  case NYIR_ADDR_SYMBOL:
    return "addr.symbol";
  case NYIR_ALLOCA:
    return "alloca";
  case NYIR_COPY_STRUCT:
    return "copy.struct";
  case NYIR_CAPTURE_RET:
    return "capture.ret";
  case NYIR_VEC4_LOAD_F64:
    return "vec4.load.f64";
  case NYIR_VEC4_STORE_F64:
    return "vec4.store.f64";
  case NYIR_VEC4_ADD_F64:
    return "vec4.add.f64";
  case NYIR_VEC4_SUB_F64:
    return "vec4.sub.f64";
  case NYIR_VEC4_MUL_F64:
    return "vec4.mul.f64";
  case NYIR_VEC4_DIV_F64:
    return "vec4.div.f64";
  case NYIR_VEC4_FMA_F64:
    return "vec4.fma.f64";
  case NYIR_VEC4_SET1_F64:
    return "vec4.set1.f64";
  case NYIR_VEC4_SHUFFLE_F64:
    return "vec4.shuffle.f64";
  case NYIR_VEC4_REDUCE_ADD_F64:
    return "vec4.reduce_add.f64";
  case NYIR_VEC8_LOAD_F32:
    return "vec8.load.f32";
  case NYIR_VEC8_STORE_F32:
    return "vec8.store.f32";
  case NYIR_VEC8_ADD_F32:
    return "vec8.add.f32";
  case NYIR_VEC8_SUB_F32:
    return "vec8.sub.f32";
  case NYIR_VEC8_MUL_F32:
    return "vec8.mul.f32";
  case NYIR_VEC8_DIV_F32:
    return "vec8.div.f32";
  case NYIR_VEC8_FMA_F32:
    return "vec8.fma.f32";
  case NYIR_VEC8_SET1_F32:
    return "vec8.set1.f32";
  case NYIR_VEC8_SHUFFLE_F32:
    return "vec8.shuffle.f32";
  case NYIR_VEC4_LOAD_I64:
    return "vec4.load.i64";
  case NYIR_VEC4_SET1_I64:
    return "vec4.set1.i64";
  case NYIR_VEC4_STORE_I64:
    return "vec4.store.i64";
  case NYIR_VEC4_ADD_I64:
    return "vec4.add.i64";
  case NYIR_VEC4_SUB_I64:
    return "vec4.sub.i64";
  case NYIR_VEC4_AND_I64:
    return "vec4.and.i64";
  case NYIR_VEC4_OR_I64:
    return "vec4.or.i64";
  case NYIR_VEC4_XOR_I64:
    return "vec4.xor.i64";
  case NYIR_VEC4_SHL_I64:
    return "vec4.shl.i64";
  case NYIR_VEC4_SAR_I64:
    return "vec4.sar.i64";
  case NYIR_VEC4_REDUCE_ADD_I64:
    return "vec4.reduce_add.i64";
  case NYIR_VEC8_LOAD_I64:
    return "vec8.load.i64";
  case NYIR_VEC8_STORE_I64:
    return "vec8.store.i64";
  case NYIR_VEC8_ADD_I64:
    return "vec8.add.i64";
  case NYIR_VEC8_SUB_I64:
    return "vec8.sub.i64";
  case NYIR_VEC8_AND_I64:
    return "vec8.and.i64";
  case NYIR_VEC8_OR_I64:
    return "vec8.or.i64";
  case NYIR_VEC8_XOR_I64:
    return "vec8.xor.i64";
  case NYIR_VEC8_REDUCE_ADD_I64:
    return "vec8.reduce_add.i64";
  case NYIR_SELECT_I64:
    return "select.i64";
  case NYIR_OP_COUNT:
    break;
  }
  return "unknown";
}

unsigned nyr_inst_effects(const nyr_inst_t *inst) {
  if (!inst)
    return NYIR_EFFECT_NONE;
  switch (inst->op) {
  case NYIR_NOP:
  case NYIR_CONST_I64:
  case NYIR_COPY:
  case NYIR_PHI:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR_I64:
  case NYIR_CMP_I64:
  case NYIR_LABEL:
  case NYIR_CONST_F64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_I64_TO_F64:
  case NYIR_F64_TO_I64:
  case NYIR_F32_TO_I64:
  case NYIR_CMP_F64:
  case NYIR_SQRT_F64:
  case NYIR_CONST_F32:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_CMP_F32:
  case NYIR_ADDR_LOCAL:
  case NYIR_ADDR_SYMBOL:
  case NYIR_CAPTURE_RET:
  case NYIR_VEC4_ADD_F64:
  case NYIR_VEC4_SUB_F64:
  case NYIR_VEC4_MUL_F64:
  case NYIR_VEC4_DIV_F64:
  case NYIR_VEC4_FMA_F64:
  case NYIR_VEC4_SET1_F64:
  case NYIR_VEC4_SHUFFLE_F64:
  case NYIR_VEC8_ADD_F32:
  case NYIR_VEC8_SUB_F32:
  case NYIR_VEC8_MUL_F32:
  case NYIR_VEC8_DIV_F32:
  case NYIR_VEC8_FMA_F32:
  case NYIR_VEC8_SET1_F32:
  case NYIR_VEC8_SHUFFLE_F32:
  case NYIR_VEC4_ADD_I64:
  case NYIR_VEC4_SUB_I64:
  case NYIR_VEC4_AND_I64:
  case NYIR_VEC4_OR_I64:
  case NYIR_VEC4_XOR_I64:
  case NYIR_VEC4_SHL_I64:
  case NYIR_VEC4_SAR_I64:
  case NYIR_VEC4_REDUCE_ADD_I64:
  case NYIR_VEC8_ADD_I64:
  case NYIR_VEC8_SUB_I64:
  case NYIR_VEC8_AND_I64:
  case NYIR_VEC8_OR_I64:
  case NYIR_VEC8_XOR_I64:
  case NYIR_VEC8_REDUCE_ADD_I64:
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
  case NYIR_VEC4_REDUCE_ADD_F64:
  case NYIR_VEC4_SET1_I64:
  case NYIR_SELECT_I64:
  case NYIR_ROR32_I64:
  case NYIR_BSWAP_I64:
  case NYIR_BSWAP32_I64:
    return NYIR_EFFECT_NONE;
  case NYIR_LOAD_LOCAL:
    return NYIR_EFFECT_READ_LOCAL;
  case NYIR_STORE_LOCAL:
    return NYIR_EFFECT_WRITE_LOCAL;
  case NYIR_LOAD_I64:
  case NYIR_VEC4_LOAD_I64:
  case NYIR_VEC8_LOAD_I64:
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC8_LOAD_F32:
    return NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP;
  case NYIR_STORE_I64:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC8_STORE_I64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC8_STORE_F32:
    return NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP;
  case NYIR_BOUNDS_CHECK:
    return NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP;
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
    return NYIR_EFFECT_MAY_TRAP;
  case NYIR_COPY_STRUCT:
    return NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
           NYIR_EFFECT_MAY_TRAP;
  case NYIR_ALLOCA:
    return NYIR_EFFECT_ALLOCATION;
  case NYIR_CALL:
    return NYIR_EFFECT_CALL | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  case NYIR_RET:
  case NYIR_BR:
  case NYIR_BR_IF:
    return NYIR_EFFECT_CONTROL;
  case NYIR_OP_COUNT:
    return NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  }
  /*
   * Keep newly added and invalid opcodes conservative until explicitly
   * classified above. With no default switch arm, compilers also diagnose a
   * new enum member omitted from the explicit classification.
   */
  return NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
}

static void nyr_init_inst_metadata(nyr_inst_t *inst) {
  if (!inst)
    return;
  /*
   * A caller that supplies an explicit, verified interprocedural call
   * summary must not lose it while the instruction is appended. All other
   * instructions start with the opcode's conservative effect classification.
   */
  if (!(inst->op == NYIR_CALL && (inst->flags & NYIR_INST_F_EFFECTS_KNOWN)))
    inst->effects = nyr_inst_effects(inst);
  if (inst->op == NYIR_CONST_I64 && !inst->range.has_min &&
      !inst->range.has_max) {
    inst->range.has_min = true;
    inst->range.has_max = true;
    inst->range.min = inst->imm;
    inst->range.max = inst->imm;
  } else if ((inst->op == NYIR_CMP_I64 || inst->op == NYIR_CMP_F64 ||
              inst->op == NYIR_CMP_F32) &&
             !inst->range.has_min && !inst->range.has_max) {
    inst->range.has_min = true;
    inst->range.has_max = true;
    inst->range.min = 0;
    inst->range.max = 1;
  }
}

static void nyr_normalize_operands(nyr_inst_t *inst) {
  if (!inst)
    return;
  switch (inst->op) {
  case NYIR_BOUNDS_CHECK:
  case NYIR_VEC4_REDUCE_ADD_F64:
  case NYIR_VEC4_REDUCE_ADD_I64:
  case NYIR_VEC8_REDUCE_ADD_I64:
  case NYIR_RET:
    break;
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC8_LOAD_F32:
  case NYIR_VEC4_LOAD_I64:
  case NYIR_VEC8_LOAD_I64:
  case NYIR_VEC4_SET1_F64:
  case NYIR_VEC8_SET1_F32:
  case NYIR_VEC4_SET1_I64:
  case NYIR_VEC4_SHUFFLE_F64:
  case NYIR_VEC8_SHUFFLE_F32:
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC8_STORE_F32:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC8_STORE_I64:
    inst->dst = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_VEC4_FMA_F64:
  case NYIR_VEC8_FMA_F32:
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_VEC4_ADD_F64:
  case NYIR_VEC4_SUB_F64:
  case NYIR_VEC4_MUL_F64:
  case NYIR_VEC4_DIV_F64:
  case NYIR_VEC8_ADD_F32:
  case NYIR_VEC8_SUB_F32:
  case NYIR_VEC8_MUL_F32:
  case NYIR_VEC8_DIV_F32:
  case NYIR_VEC4_ADD_I64:
  case NYIR_VEC4_SUB_I64:
  case NYIR_VEC4_AND_I64:
  case NYIR_VEC4_OR_I64:
  case NYIR_VEC4_XOR_I64:
  case NYIR_VEC4_SHL_I64:
  case NYIR_VEC4_SAR_I64:
  case NYIR_VEC8_ADD_I64:
  case NYIR_VEC8_SUB_I64:
  case NYIR_VEC8_AND_I64:
  case NYIR_VEC8_OR_I64:
  case NYIR_VEC8_XOR_I64:
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_COPY:
  case NYIR_I64_TO_F64:
  case NYIR_F64_TO_I64:
  case NYIR_F32_TO_I64:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_SQRT_F64:
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
  case NYIR_BSWAP_I64:
  case NYIR_BSWAP32_I64:
  case NYIR_LOAD_I64:
  case NYIR_BR_IF:
    /*
     * NYIR_RET keeps .b: a register-class aggregate return carries its
     * second eightbyte piece there (see nyr_func_t.
     * return_aggregate_classes).  Every scalar-return constructor sets
     * .b = -1 explicitly.
     */
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_ADDR_LOCAL:
  case NYIR_ADDR_SYMBOL:
  case NYIR_ALLOCA:
  case NYIR_CAPTURE_RET:
    inst->a = -1;
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_PHI:
    inst->a = -1;
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_STORE_LOCAL:
    inst->dst = -1;
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_STORE_I64:
  case NYIR_COPY_STRUCT:
    inst->dst = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    if (inst->op == NYIR_STORE_I64)
      inst->b = -1;
    else
      inst->c = -1;
    break;
  case NYIR_CALL:
    if (inst->imm <= 0)
      inst->a = -1;
    if (inst->imm <= 1)
      inst->b = -1;
    if (inst->imm <= 2)
      inst->c = -1;
    if (inst->imm <= 3)
      inst->d = -1;
    if (inst->imm <= 4)
      inst->e = -1;
    if (inst->imm <= 5)
      inst->f = -1;
    if (inst->imm <= 6) {
      free(inst->extra_args);
      inst->extra_args = NULL;
      inst->extra_args_len = 0;
    } else {
      inst->extra_args_len = (size_t)(inst->imm - 6);
    }
    break;
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR_I64:
  case NYIR_ROR32_I64:
  case NYIR_CMP_I64:
  case NYIR_CMP_F64:
  case NYIR_CMP_F32:
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_SELECT_I64:
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_NOP:
  case NYIR_CONST_I64:
  case NYIR_CONST_F64:
  case NYIR_CONST_F32:
  case NYIR_LABEL:
  case NYIR_LOAD_LOCAL:
  case NYIR_BR:
    inst->a = -1;
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_OP_COUNT:
    break;
  }
}

void nyr_refresh_metadata(nyr_func_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    nyr_normalize_operands(in);
    /*
     * Calls tagged with NYIR_INST_F_EFFECTS_KNOWN carry a builder-inferred
     * interprocedural summary in in->effects; keep it instead of restoring
     * the static CALL|UNKNOWN_SIDE_EFFECT default.
     */
    if (!(in->op == NYIR_CALL && (in->flags & NYIR_INST_F_EFFECTS_KNOWN)))
      in->effects = nyr_inst_effects(in);
    if (in->op == NYIR_CONST_I64) {
      in->range = (nyr_range_t){
          .has_min = true, .has_max = true, .min = in->imm, .max = in->imm};
    } else if (in->op == NYIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
               in->op == NYIR_CMP_F32) {
      in->range =
          (nyr_range_t){.has_min = true, .has_max = true, .min = 0, .max = 1};
    }
  }
}

int nyr_ir_emit(nyr_func_t *f, nyr_inst_t inst) {
  if (!f)
    return -1;
  if (f->len > f->cap || f->cap > SIZE_MAX / sizeof(*f->data) ||
      (f->cap && !f->data))
    return -1;
  NY_INVARIANT_ASSERT(inst.op >= 0 && inst.op < NYIR_OP_COUNT,
                      "valid nyir opcode", inst.debug.file, inst.debug.line,
                      inst.debug.column, "nyr_ir_emit",
                      "instruction construction",
                      "invalid opcode %d emitted in nyir", (int)inst.op);
  if (inst.op < 0 || inst.op >= NYIR_OP_COUNT)
    return -1;
  nyr_normalize_operands(&inst);
  if (f->len >= f->cap) {
    size_t cap = 0;
    if (!nyr_next_capacity(f->cap, 64, sizeof(*f->data), &cap))
      return -1;
    nyr_inst_t *data = (nyr_inst_t *)realloc(f->data, cap * sizeof(*data));
    if (!data)
      return -1;
    f->data = data;
    f->cap = cap;
  }
  if (inst.symbol) {
    inst.symbol = nyr_func_own_symbol_copy(f, inst.symbol);
    if (!inst.symbol)
      return -1;
  }
  if (inst.dst < 0 && inst.op != NYIR_STORE_LOCAL &&
      inst.op != NYIR_STORE_I64 && inst.op != NYIR_RET && inst.op != NYIR_BR &&
      inst.op != NYIR_BR_IF && inst.op != NYIR_LABEL && inst.op != NYIR_NOP)
    inst.dst = f->next_value++;
  nyr_init_inst_metadata(&inst);
  f->data[f->len++] = inst;
  if (inst.op == NYIR_CALL && inst.symbol)
    f->has_symbol_calls = true;
  return inst.dst;
}

bool nyr_err(char *err, size_t err_len, const char *fmt, ...) {
  if (err && err_len > 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
  }
  return false;
}

bool nyr_inst_err(char *err, size_t err_len, const nyr_inst_t *in,
                   size_t index, const char *reason) {
  if (in && in->debug.line) {
    if (in->debug.file && in->debug.file[0]) {
      return nyr_err(
          err, err_len,
          "native NYIR verify: inst %zu opcode=%s dst=v%d a=v%d b=v%d "
          "imm=%" PRId64 " at %s:%u:%u: %s",
          index, nyr_op_name(in->op), in->dst, in->a, in->b, in->imm,
          in->debug.file, in->debug.line, in->debug.column,
          reason ? reason : "invalid instruction");
    }
    return nyr_err(
        err, err_len,
        "native NYIR verify: inst %zu opcode=%s dst=v%d a=v%d b=v%d "
        "imm=%" PRId64 " at line %u:%u: %s",
        index, nyr_op_name(in->op), in->dst, in->a, in->b, in->imm,
        in->debug.line, in->debug.column,
        reason ? reason : "invalid instruction");
  }
  return nyr_err(err, err_len,
                  "native NYIR verify: inst %zu opcode=%s dst=v%d a=v%d b=v%d "
                  "imm=%" PRId64 ": %s",
                  index, in ? nyr_op_name(in->op) : "<null>",
                  in ? in->dst : -1, in ? in->a : -1, in ? in->b : -1,
                  in ? in->imm : 0, reason ? reason : "invalid instruction");
}

static const char *nyr_dump_color(FILE *out, const char *code) {
  return color_enabled() && (out == stderr || out == stdout) ? code : "";
}

static const char *nyr_cmp_i64_name(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return "eq";
  case NYIR_CMP_NE:
    return "ne";
  case NYIR_CMP_LT:
    return "slt";
  case NYIR_CMP_LE:
    return "sle";
  case NYIR_CMP_GT:
    return "sgt";
  case NYIR_CMP_GE:
    return "sge";
  }
  return "invalid";
}

/*
 * Compact TUI-like dump helpers (not a real TUI): short ops, pack many
 * insts per line, elide nops, skip verbose effects/range spam.
 */

static const char *nyr_op_short(const nyr_inst_t *in) {
  if (!in)
    return "?";
  switch (in->op) {
  case NYIR_NOP:
    return "nop";
  case NYIR_CONST_I64:
    return "i64";
  case NYIR_CONST_F64:
    return "f64";
  case NYIR_CONST_F32:
    return "f32";
  case NYIR_COPY:
    return "mov";
  case NYIR_PHI:
    return "phi";
  case NYIR_ADD_I64:
    return "add";
  case NYIR_SUB_I64:
    return "sub";
  case NYIR_MUL_I64:
    return "mul";
  case NYIR_DIV_I64:
    return "div";
  case NYIR_MOD_I64:
    return "mod";
  case NYIR_AND_I64:
    return "and";
  case NYIR_OR_I64:
    return "or";
  case NYIR_XOR_I64:
    return "xor";
  case NYIR_SHL_I64:
    return "shl";
  case NYIR_SAR_I64:
    return "sar";
  case NYIR_ROR_I64:
    return "ror";
  case NYIR_ROR32_I64:
    return "ror32";
  case NYIR_BSWAP_I64:
    return "bswap";
  case NYIR_BSWAP32_I64:
    return "bswap32";
  case NYIR_SELECT_I64:
    return "select";
  case NYIR_CMP_I64:
    return nyr_cmp_i64_name(in->cmp);
  case NYIR_LABEL:
    return "L";
  case NYIR_LOAD_LOCAL:
    return "ld";
  case NYIR_STORE_LOCAL:
    return "st";
  case NYIR_CALL:
    return "call";
  case NYIR_RET:
    return "ret";
  case NYIR_BR:
    return "br";
  case NYIR_BR_IF:
    return "br.if";
  case NYIR_ADD_F64:
    return "fadd";
  case NYIR_SUB_F64:
    return "fsub";
  case NYIR_MUL_F64:
    return "fmul";
  case NYIR_DIV_F64:
    return "fdiv";
  case NYIR_CMP_F64:
    return "fcmp";
  case NYIR_SQRT_F64:
    return "sqrt.f64";
  case NYIR_SIN_F64:
    return "sin.f64";
  case NYIR_COS_F64:
    return "cos.f64";
  case NYIR_ADDR_LOCAL:
    return "lea";
  case NYIR_LOAD_I64:
    return "ld.i64";
  case NYIR_STORE_I64:
    return "st.i64";
  case NYIR_ADDR_SYMBOL:
    return "sym";
  case NYIR_ALLOCA:
    return "alloca";
  case NYIR_COPY_STRUCT:
    return "memcpy";
  case NYIR_CAPTURE_RET:
    return "cap.ret";
  default:
    return nyr_op_name(in->op);
  }
}

/*
 * Format one inst into buf; returns false for pure nops (callers elide).
 */
static bool nyr_fmt_inst(const nyr_inst_t *in, char *buf, size_t n) {
  if (!in || !buf || n == 0)
    return false;
  if (in->op == NYIR_NOP) {
    buf[0] = '\0';
    return false;
  }
  size_t used = 0;
#define APP(...)                                                               \
  do {                                                                         \
    int _w = snprintf(buf + used, n > used ? n - used : 0, __VA_ARGS__);       \
    if (_w > 0)                                                                \
      used += (size_t)_w < (n > used ? n - used : 0)                           \
                  ? (size_t)_w                                                 \
                  : (n > used ? n - used : 0);                                 \
  } while (0)

  if (in->op == NYIR_LABEL) {
    APP("L%" PRId64 ":", in->imm);
    return true;
  }
  if (in->dst >= 0)
    APP("v%d=", in->dst);
  if (in->op == NYIR_CONST_I64) {
    APP("%" PRId64, in->imm);
    return true;
  }
  if (in->op == NYIR_CONST_F64) {
    APP("%.6g", nyr_bits_to_f64(in->imm));
    return true;
  }
  if (in->op == NYIR_CONST_F32) {
    APP("%.6g", (double)nyr_bits_to_f32(in->imm));
    return true;
  }
  APP("%s", nyr_op_short(in));
  if ((in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) &&
      (in->flags & NYIR_INST_F_MEM_BYTE))
    APP(".byte");
  if (in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
      in->op == NYIR_ADDR_LOCAL) {
    APP(" #%" PRId64, in->imm);
    if (in->symbol && in->symbol[0])
      APP("(%s)", in->symbol);
    if (in->a >= 0)
      APP(" v%d", in->a);
  } else if (in->op == NYIR_BR) {
    APP("→L%" PRId64, in->imm);
  } else if (in->op == NYIR_BR_IF) {
    if (in->a >= 0)
      APP(" v%d", in->a);
    APP("→L%" PRId64, in->imm);
  } else if (in->op == NYIR_CALL) {
    APP(" %s", in->symbol ? in->symbol : "?");
    if (in->a >= 0)
      APP(" v%d", in->a);
    if (in->b >= 0)
      APP(" v%d", in->b);
    if (in->c >= 0)
      APP(" v%d", in->c);
    if (in->d >= 0)
      APP(" v%d", in->d);
    if (in->e >= 0)
      APP(" v%d", in->e);
    if (in->f >= 0)
      APP(" v%d", in->f);
    for (size_t k = 0; k < in->extra_args_len; ++k)
      APP(" v%d", in->extra_args[k]);
  } else if (in->op == NYIR_PHI) {
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      APP("%sL%" PRId64 ":v%d", k ? "," : " ",
          in->phi_incoming[k].predecessor_label, in->phi_incoming[k].value);
  } else if (in->op == NYIR_ADDR_SYMBOL) {
    APP(" %s", in->symbol ? in->symbol : "?");
  } else {
    if (in->a >= 0)
      APP(" v%d", in->a);
    if (in->b >= 0)
      APP(" v%d", in->b);
    if (in->op == NYIR_SELECT_I64 && in->c >= 0)
      APP(" v%d", in->c);
    if (in->imm && in->op != NYIR_RET)
      APP(" #%" PRId64, in->imm);
  }
#undef APP
  return true;
}

static bool nyr_inst_same(const nyr_inst_t *a, const nyr_inst_t *b) {
  if (!a || !b)
    return a == b;
  if (a->op != b->op || a->dst != b->dst || a->a != b->a || a->b != b->b ||
      a->c != b->c || a->d != b->d || a->e != b->e || a->f != b->f ||
      a->imm != b->imm || a->cmp != b->cmp ||
      a->ret_aggregate_size != b->ret_aggregate_size ||
      a->ret_aggregate_classes[0] != b->ret_aggregate_classes[0] ||
      a->ret_aggregate_classes[1] != b->ret_aggregate_classes[1] ||
      a->extra_args_len != b->extra_args_len ||
      a->phi_incoming_len != b->phi_incoming_len)
    return false;
  if (a->symbol != b->symbol) {
    if (!a->symbol || !b->symbol || strcmp(a->symbol, b->symbol) != 0)
      return false;
  }
  for (size_t i = 0; i < a->extra_args_len; ++i)
    if (a->extra_args[i] != b->extra_args[i])
      return false;
  for (size_t i = 0; i < a->phi_incoming_len; ++i)
    if (a->phi_incoming[i].predecessor_label !=
            b->phi_incoming[i].predecessor_label ||
        a->phi_incoming[i].value != b->phi_incoming[i].value)
      return false;
  return true;
}

static const char *nyr_dump_scalar_class(nyr_scalar_class_t cls) {
  switch (cls) {
  case NYIR_SCALAR_I64_RAW:
    return "i64-raw";
  case NYIR_SCALAR_F32:
    return "f32";
  case NYIR_SCALAR_F64:
    return "f64";
  case NYIR_SCALAR_I64_TAGGED:
    return "i64-tagged";
  case NYIR_SCALAR_STRING:
    return "str";
  case NYIR_SCALAR_TYPED_BUFFER:
    return "tbuf";
  case NYIR_SCALAR_OBJECT:
    return "object";
  case NYIR_SCALAR_POINTER:
    return "ptr";
  case NYIR_SCALAR_CLOSURE:
    return "closure";
  case NYIR_SCALAR_AGGREGATE:
    return "aggregate";
  case NYIR_SCALAR_UNKNOWN:
  default:
    return "?";
  }
}

static const char *nyr_dump_arg_contract(uint8_t contract) {
  switch (contract) {
  case NYIR_ARG_CONTRACT_BORROWS:
    return "borrow";
  case NYIR_ARG_CONTRACT_CONSUMES:
    return "consume";
  case NYIR_ARG_CONTRACT_RETURNS_BORROW:
    return "returns-borrow";
  case NYIR_ARG_CONTRACT_UNKNOWN:
  default:
    return NULL;
  }
}

void nyr_dump(FILE *out, const nyr_func_t *f, const char *name) {
  if (!out)
    out = stderr;
  const char *reset = nyr_dump_color(out, NY_CLR_RESET);
  const char *heading = nyr_dump_color(out, NY_CLR_BRIGHT_CYAN NY_CLR_BOLD);
  const char *muted = nyr_dump_color(out, NY_CLR_BRIGHT_BLACK);
  const char *value = nyr_dump_color(out, NY_CLR_BRIGHT_MAGENTA);
  const char *opcode = nyr_dump_color(out, NY_CLR_BRIGHT_GREEN);

  size_t nops = 0;
  if (f) {
    for (size_t i = 0; i < f->len; ++i) {
      if (f->data[i].op == NYIR_NOP)
        ++nops;
    }
  }
  fprintf(out, "%snyir%s %s%s%s  %s%dv %zui%s", heading, reset, value,
          name && name[0] ? name : "<anon>", reset, muted,
          f ? f->next_value : 0, f ? f->len : 0, reset);
  if (nops)
    fprintf(out, " %s(%zu nop)%s", muted, nops, reset);
  fputc('\n', out);
  if (!f || !f->len)
    return;

  /*
   * Pack several live insts per line; elide nops with a run count.
   */
  enum { PER_LINE = 5 };
  int on_line = 0;
  size_t nop_run = 0;
  char ibuf[1024];
  for (size_t i = 0; i <= f->len; ++i) {
    bool end = i == f->len;
    bool is_nop = !end && f->data[i].op == NYIR_NOP;
    if (is_nop) {
      ++nop_run;
      continue;
    }
    if (nop_run) {
      if (on_line == 0)
        fprintf(out, "  %s%3zu%s ", muted, i - nop_run, reset);
      else
        fputs("  ", out);
      fprintf(out, "%s---%zunop%s", muted, nop_run, reset);
      ++on_line;
      nop_run = 0;
      if (on_line >= PER_LINE) {
        fputc('\n', out);
        on_line = 0;
      }
    }
    if (end)
      break;
    if (!nyr_fmt_inst(&f->data[i], ibuf, sizeof(ibuf)))
      continue;
    if (f->data[i].scalar_class != NYIR_SCALAR_UNKNOWN) {
      size_t used = strlen(ibuf);
      const char *cls = nyr_dump_scalar_class(f->data[i].scalar_class);
      if (used + strlen(cls) + 8 < sizeof(ibuf))
        snprintf(ibuf + used, sizeof(ibuf) - used, " [abi=%s]", cls);
    }
    if (f->data[i].flags & NYIR_INST_F_NOALIAS) {
      size_t used = strlen(ibuf);
      if (used + sizeof(" [noalias]") < sizeof(ibuf))
        memcpy(ibuf + used, " [noalias]", sizeof(" [noalias]"));
      used = strlen(ibuf);
      if (used + sizeof(" [ownership=255]") < sizeof(ibuf))
        snprintf(ibuf + used, sizeof(ibuf) - used, " [ownership=%u]",
                 (unsigned)f->data[i].semantic_ownership);
    }
    if (f->data[i].op == NYIR_CALL && f->data[i].imm > 0) {
      size_t argc = (size_t)f->data[i].imm;
      if (argc > NYIR_CALL_MAX_ARGS)
        argc = NYIR_CALL_MAX_ARGS;
      size_t used = strlen(ibuf);
      bool opened = false;
      for (size_t arg = 0; arg < argc; ++arg) {
        nyr_scalar_class_t cls = nyr_call_arg_class_get(&f->data[i], arg);
        const char *contract = nyr_dump_arg_contract(
            nyr_call_arg_contract_get(&f->data[i], arg));
        const char *cls_name =
            cls == NYIR_SCALAR_UNKNOWN ? NULL : nyr_dump_scalar_class(cls);
        if (cls == NYIR_SCALAR_UNKNOWN && !contract)
          continue;
        int wrote =
            snprintf(ibuf + used, sizeof(ibuf) - used, "%s%zu:%s%s%s",
                     opened ? " " : " [arg-abi ", arg, cls_name ? cls_name : "",
                     cls_name && contract ? "/" : "", contract ? contract : "");
        if (wrote < 0 || (size_t)wrote >= sizeof(ibuf) - used) {
          used = sizeof(ibuf) - 1;
          break;
        }
        used += (size_t)wrote;
        opened = true;
      }
      if (opened && used + 2 < sizeof(ibuf)) {
        ibuf[used++] = ']';
        ibuf[used] = '\0';
      }
    }
    if (f->data[i].debug.line) {
      size_t used = strlen(ibuf);
      if (used + 16 < sizeof(ibuf))
        snprintf(ibuf + used, sizeof(ibuf) - used, " @%s:%u:%u",
                 f->data[i].debug.file && f->data[i].debug.file[0]
                     ? f->data[i].debug.file
                     : "<source>",
                 f->data[i].debug.line,
                 f->data[i].debug.column ? f->data[i].debug.column : 1);
    }
    if (on_line == 0)
      fprintf(out, "  %s%3zu%s ", muted, i, reset);
    else
      fputs("  ", out);
    fprintf(out, "%s%s%s", opcode, ibuf, reset);
    ++on_line;
    if (on_line >= PER_LINE || f->data[i].op == NYIR_LABEL ||
        f->data[i].op == NYIR_BR || f->data[i].op == NYIR_BR_IF ||
        f->data[i].op == NYIR_RET) {
      fputc('\n', out);
      on_line = 0;
    }
  }
  if (on_line)
    fputc('\n', out);
}

void nyr_dump_compact(FILE *out, const nyr_func_t *f, const char *name) {
  if (!out)
    out = stderr;
  if (!f) {
    fprintf(out, "nyir %s unavailable\n", name && name[0] ? name : "<anon>");
    return;
  }
  fprintf(out, "nyir %s %dv/%zu ", name && name[0] ? name : "<anon>",
          f->next_value, f->len);
  char ibuf[256];
  size_t shown = 0;
  size_t shown_cap = 12;
  {
    const char *env = getenv("NY_DUMP_COMPACT_LIMIT");
    if (env && *env) {
      long v = strtol(env, NULL, 10);
      if (v >= 0)
        shown_cap = (size_t)v;
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_NOP)
      continue;
    if (shown == shown_cap) {
      fputs(" ...", out);
      break;
    }
    if (!nyr_fmt_inst(&f->data[i], ibuf, sizeof(ibuf)))
      continue;
    if (f->data[i].flags & NYIR_INST_F_NOALIAS) {
      size_t used = strlen(ibuf);
      if (used + sizeof(" [noalias own=255]") < sizeof(ibuf))
        snprintf(ibuf + used, sizeof(ibuf) - used, " [noalias own=%u]",
                 (unsigned)f->data[i].semantic_ownership);
    }
    fprintf(out, "%s%zu:%s", shown ? " | " : "", i, ibuf);
    ++shown;
  }
  if (shown == 0)
    fputs("<empty>", out);
  fputc('\n', out);
}

void nyr_dump_diff(FILE *out, const nyr_func_t *before,
                    const nyr_func_t *after, const char *pass_tag) {
  if (!out)
    out = stderr;
  if (!after)
    return;
  const char *reset = nyr_dump_color(out, NY_CLR_RESET);
  const char *muted = nyr_dump_color(out, NY_CLR_BRIGHT_BLACK);
  const char *addc = nyr_dump_color(out, NY_CLR_BRIGHT_GREEN);
  const char *delc = nyr_dump_color(out, NY_CLR_BRIGHT_RED);
  const char *chgc = nyr_dump_color(out, NY_CLR_BRIGHT_YELLOW);

  size_t blen = before ? before->len : 0;
  size_t alen = after->len;
  size_t n = blen > alen ? blen : alen;
  char abuf[160], bbuf[160];
  int shown = 0;
  enum { MAX_DIFF_LINES = 400 };

  /*
   * Packed changes under the -- <pass> status line: ~i:new  -i  +i:new
   */
  (void)pass_tag;
  fprintf(out, "    ");
  int on_line = 0;
  for (size_t i = 0; i < n && shown < MAX_DIFF_LINES; ++i) {
    const nyr_inst_t *bi = before && i < blen ? &before->data[i] : NULL;
    const nyr_inst_t *ai = i < alen ? &after->data[i] : NULL;
    bool bnop = !bi || bi->op == NYIR_NOP;
    bool anop = !ai || ai->op == NYIR_NOP;
    if (bnop && anop)
      continue;
    if (bi && ai && nyr_inst_same(bi, ai))
      continue;

    if (on_line >= 5) {
      fputc('\n', out);
      fprintf(out, "    ");
      on_line = 0;
    } else if (on_line)
      fputs("  ", out);

    if (bnop && !anop) {
      nyr_fmt_inst(ai, abuf, sizeof(abuf));
      fprintf(out, "%s+%zu:%s%s", addc, i, abuf, reset);
    } else if (!bnop && anop) {
      nyr_fmt_inst(bi, bbuf, sizeof(bbuf));
      fprintf(out, "%s-%zu:%s%s", delc, i, bbuf, reset);
    } else {
      nyr_fmt_inst(ai, abuf, sizeof(abuf));
      fprintf(out, "%s~%zu:%s%s", chgc, i, abuf, reset);
    }
    ++on_line;
    ++shown;
  }
  if (shown == 0)
    fprintf(out, "%s(unchanged)%s", muted, reset);
  else if (shown >= MAX_DIFF_LINES)
    fprintf(out, "  %s---%s", muted, reset);
  fputc('\n', out);
}

bool nyr_diff_is_empty(const nyr_func_t *before, const nyr_func_t *after) {
  size_t blen = before ? before->len : 0;
  size_t alen = after ? after->len : 0;
  size_t n = blen > alen ? blen : alen;
  for (size_t i = 0; i < n; ++i) {
    const nyr_inst_t *bi = before && i < blen ? &before->data[i] : NULL;
    const nyr_inst_t *ai = after && i < alen ? &after->data[i] : NULL;
    bool bnop = !bi || bi->op == NYIR_NOP;
    bool anop = !ai || ai->op == NYIR_NOP;
    if (bnop && anop)
      continue;
    if (bi && ai && nyr_inst_same(bi, ai))
      continue;
    return false;
  }
  return true;
}

void nyr_dump_cfg(FILE *out, const nyr_func_t *f, const char *name) {
  if (!out)
    out = stderr;
  if (!f) {
    fprintf(out, "nyir cfg function %s unavailable\n",
            name && name[0] ? name : "<anon>");
    return;
  }
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg)) {
    fprintf(out, "nyir cfg function %s unavailable (build failed)\n",
            name && name[0] ? name : "<anon>");
    return;
  }
  fprintf(out, "nyir cfg function %s blocks=%zu\n",
          name && name[0] ? name : "<anon>", cfg.block_count);
  size_t *rpo = NULL;
  size_t rpo_len = 0;
  if (nyr_cfg_reverse_postorder(&cfg, &rpo, &rpo_len)) {
    fputs("  rpo=[", out);
    for (size_t i = 0; i < rpo_len; ++i)
      fprintf(out, "%sB%zu", i ? ", " : "", rpo[i]);
    fputs("]\n", out);
  }
  free(rpo);
  for (size_t block = 0; block < cfg.block_count; ++block) {
    fprintf(out, "  block B%zu%s insts=%zu..%zu preds=[", block,
            block == 0 ? " entry" : "", cfg.block_start[block],
            cfg.block_end[block]);
    bool first = true;
    for (size_t edge = cfg.pred_offsets[block];
         edge < cfg.pred_offsets[block + 1]; ++edge) {
      size_t pred = cfg.pred_blocks[edge];
      fprintf(out, "%sB%zu", first ? "" : ", ", pred);
      first = false;
    }
    fputs("] succs=[", out);
    first = true;
    for (size_t edge = cfg.succ_offsets[block];
         edge < cfg.succ_offsets[block + 1]; ++edge) {
      size_t succ = cfg.succ_blocks[edge];
      fprintf(out, "%sB%zu", first ? "" : ", ", succ);
      first = false;
    }
    fprintf(out, "]%s%s\n", cfg.block_label[block] >= 0 ? " label" : "",
            cfg.reachable && !cfg.reachable[block] ? " unreachable" : "");
  }
  nyr_cfg_free(&cfg);
}

void nyr_eval_result_dump(FILE *out, const char *name,
                           const nyr_eval_result_t *result) {
  if (!out)
    out = stderr;
  const char *reset = nyr_dump_color(out, NY_CLR_RESET);
  const char *heading = nyr_dump_color(out, NY_CLR_BRIGHT_CYAN NY_CLR_BOLD);
  const char *value = nyr_dump_color(out, NY_CLR_BRIGHT_MAGENTA);
  fprintf(
      out,
      "%snyir vm profile%s function=%s%s%s returned=%s result=%" PRId64
      " steps=%zu branches_taken=%zu branches_not_taken=%zu calls=%zu "
      "max_pc=%zu max_value=%zu max_local=%zu\n",
      heading, reset, value, name && name[0] ? name : "rt_main", reset,
      result && result->returned ? "yes" : "no", result ? result->result : 0,
      result ? result->steps : 0, result ? result->branch_taken : 0,
      result ? result->branch_not_taken : 0, result ? result->call_count : 0,
      result ? result->max_pc : 0, result ? result->max_value_index : 0,
      result ? result->max_local_index : 0);
  if (!result)
    return;
  fprintf(out, "NYP2 function=%s steps=%zu\n",
          name && name[0] ? name : "rt_main", result->steps);
  for (size_t i = 0; i < result->pc_count_len; ++i)
    if (result->pc_counts && result->pc_counts[i])
      fprintf(out, "pc %zu count=%" PRIu64 "\n", i, result->pc_counts[i]);
  for (size_t i = 0; i < result->edge_count; ++i)
    fprintf(out, "edge %zu %zu count=%" PRIu64 "\n", result->edges[i].from_pc,
            result->edges[i].to_pc, result->edges[i].count);
  for (size_t i = 0; i < (size_t)NYIR_OP_COUNT; ++i) {
    if (result->op_counts[i] == 0)
      continue;
    fprintf(out, "  op %s%-14s%s %zu\n",
            nyr_dump_color(out, NY_CLR_BRIGHT_GREEN),
            nyr_op_name((nyr_op_t)i), reset, result->op_counts[i]);
  }
}

void nyr_collect_stats(const nyr_func_t *f, size_t *insts, int *values,
                        size_t *ops, size_t op_count) {
  if (insts)
    *insts = f ? f->len : 0;
  if (values)
    *values = f ? f->next_value : 0;
  if (ops && op_count > 0) {
    memset(ops, 0, op_count * sizeof(*ops));
    for (size_t i = 0; f && i < f->len; ++i) {
      if ((size_t)f->data[i].op < op_count)
        ops[f->data[i].op]++;
    }
  }
}

void nyr_dump_stats(FILE *out, const nyr_opt_stats_t *stats) {
  if (!out)
    out = stderr;
  if (!stats)
    return;
  if (ny_dbg_json_enabled()) {
    for (size_t i = 0; i < stats->pass_count; ++i) {
      const nyr_pass_stat_t *r = &stats->passes[i];
      ny_dbg_heuristic_event(
          "NYIR_PASS_STATS",
          "pass=%s time_ms=%.3f instructions=%zu->%zu values=%d->%d "
          "blocks=%zu->%zu",
          r->name ? r->name : "?", r->time_ms, r->before_insts, r->after_insts,
          r->before_values, r->after_values, r->before_blocks, r->after_blocks);
    }
    ny_dbg_heuristic_event("NYIR_PASS_STATS", "total_time_ms=%.3f",
                           stats->total_time_ms);
    return;
  }
  size_t removed = stats->before_insts > stats->after_insts
                       ? stats->before_insts - stats->after_insts
                       : 0;
  /*
   * One compact summary line: instruction and value deltas plus the
   * safety-sensitive key metrics, instead of two or three separate lines.
   */
  fprintf(out, "%snyir optimize%s %zu→%zu inst (-%zu) v%d→%d",
          nyr_dump_color(out, NY_CLR_BRIGHT_CYAN NY_CLR_BOLD),
          nyr_dump_color(out, NY_CLR_RESET), stats->before_insts,
          stats->after_insts, removed, stats->before_values,
          stats->after_values);
  /*
   * Highlight key optimization metrics so reviewers can quickly gauge
   * the safety and quality of an optimization change.
   *
   * Bounds-check eliminations, allocation count changes, and indirect-call
   * eliminations are the most safety-sensitive transformations in the
   * pipeline and must always show clear before→after deltas.
   */
  size_t bce_before = stats->before_ops[NYIR_BOUNDS_CHECK];
  size_t bce_after = stats->after_ops[NYIR_BOUNDS_CHECK];
  size_t alloc_before = stats->before_ops[NYIR_ALLOCA];
  size_t alloc_after = stats->after_ops[NYIR_ALLOCA];
  size_t call_before = stats->before_ops[NYIR_CALL];
  size_t call_after = stats->after_ops[NYIR_CALL];
  if (bce_before != bce_after || alloc_before != alloc_after ||
      call_before != call_after) {
    fprintf(out, "%s | bce %zu→%zu alloc %zu→%zu calls %zu→%zu%s",
            nyr_dump_color(out, NY_CLR_BRIGHT_YELLOW NY_CLR_BOLD), bce_before,
            bce_after, alloc_before, alloc_after, call_before, call_after,
            nyr_dump_color(out, NY_CLR_RESET));
  }
  fprintf(out, "\n");
  bool op_counts_changed = false;
  for (size_t op = 0; op < (size_t)NYIR_OP_COUNT; ++op) {
    if (stats->before_ops[op] != stats->after_ops[op]) {
      op_counts_changed = true;
      break;
    }
  }
  if (op_counts_changed)
    fprintf(out, "%snyir optimize ops%s",
            nyr_dump_color(out, NY_CLR_BRIGHT_CYAN NY_CLR_BOLD),
            nyr_dump_color(out, NY_CLR_RESET));
  for (size_t op = 0; op < (size_t)NYIR_OP_COUNT; ++op) {
    size_t before = stats->before_ops[op];
    size_t after = stats->after_ops[op];
    if (!op_counts_changed || before == after)
      continue;
    fprintf(out, " %s%s%s:%zu->%zu", nyr_dump_color(out, NY_CLR_BRIGHT_GREEN),
            nyr_op_name((nyr_op_t)op), nyr_dump_color(out, NY_CLR_RESET),
            before, after);
  }
  if (op_counts_changed)
    fputc('\n', out);
  if (stats->pass_count) {
    /*
     * Aggregate repeated passes by name (the pipeline runs e.g. cfg-simplify
     * many times), so the 40-row `· <pass> · 0.00ms` flood collapses.  Rows
     * that moved instructions or took measurable time print once with a `*`
     * repeat marker; everything else folds into one brace-grouped quiet line
     * (e.g. `tbuf-{private-object,promote-alloca}` for consecutive pairs).
     */
    typedef struct {
      const char *name;
      size_t count;
      double time_ms;
      size_t before_insts;
      size_t after_insts;
      size_t max_delta;
    } dump_agg_t;
    dump_agg_t agg[NYIR_MAX_PASS_STATS] = {{0}};
    size_t agg_count = 0;
    const char *quiet_seq[NYIR_MAX_PASS_STATS] = {0};
    size_t quiet_len = 0;
    size_t quiet_rows = 0;
    double quiet_ms = 0.0;
    for (size_t i = 0; i < stats->pass_count; ++i) {
      const nyr_pass_stat_t *r = &stats->passes[i];
      const char *name = r->name ? r->name : "unknown";
      size_t delta = r->before_insts > r->after_insts
                         ? r->before_insts - r->after_insts
                         : r->after_insts - r->before_insts;
      bool informative = delta != 0 || r->time_ms >= 0.005;
      if (!informative) {
        quiet_rows++;
        quiet_ms += r->time_ms;
        if (quiet_len < NYIR_MAX_PASS_STATS)
          quiet_seq[quiet_len++] = name;
        continue;
      }
      size_t slot = agg_count;
      for (size_t j = 0; j < agg_count; ++j) {
        if (strcmp(agg[j].name, name) == 0) {
          slot = j;
          break;
        }
      }
      if (slot == agg_count && agg_count < NYIR_MAX_PASS_STATS) {
        agg[agg_count++] = (dump_agg_t){.name = name,
                                        .count = 0,
                                        .time_ms = 0.0,
                                        .before_insts = 0,
                                        .after_insts = 0,
                                        .max_delta = 0};
      }
      if (slot >= NYIR_MAX_PASS_STATS)
        continue;
      if (agg[slot].count == 0)
        agg[slot].before_insts = r->before_insts;
      agg[slot].count++;
      agg[slot].time_ms += r->time_ms;
      agg[slot].after_insts = r->after_insts;
      size_t d = agg[slot].before_insts > agg[slot].after_insts
                     ? agg[slot].before_insts - agg[slot].after_insts
                     : agg[slot].after_insts - agg[slot].before_insts;
      if (d > agg[slot].max_delta)
        agg[slot].max_delta = d;
    }
    size_t max_delta = 0;
    for (size_t i = 0; i < agg_count; ++i)
      if (agg[i].max_delta > max_delta)
        max_delta = agg[i].max_delta;
    fprintf(out, "nyir passes  time_ms  inst Δ  change\n");
    for (size_t i = 0; i < agg_count; ++i) {
      size_t delta = agg[i].max_delta;
      char spark[9] = "--------";
      if (max_delta && delta) {
        size_t bars = (delta * 8 + max_delta - 1) / max_delta;
        if (bars > 8)
          bars = 8;
        memset(spark, '#', bars);
        memset(spark + bars, '-', 8 - bars);
      }
      char label[32];
      snprintf(label, sizeof(label), "%s%s", agg[i].name,
               agg[i].count > 1 ? "*" : "");
      /*
       * Show per-name info even when aggregated: total time + net change.
       */
      fprintf(out, "  %-20s %7.2f  %c%-5zu %s  (×%zu)\n", label, agg[i].time_ms,
              agg[i].after_insts < agg[i].before_insts   ? '-'
              : agg[i].after_insts > agg[i].before_insts ? '+'
                                                         : '=',
              delta, spark, agg[i].count);
    }
    if (quiet_rows) {
      /*
       * Brace-group consecutive same-family quiet passes for one line.
       */
      char grouped[1024] = {0};
      size_t glen = 0;
      size_t shown = 0;
      /*
       * First compress to ordered distinct runs with counts.
       */
      const char *runs[NYIR_MAX_PASS_STATS] = {0};
      size_t rcounts[NYIR_MAX_PASS_STATS] = {0};
      size_t rlen = 0;
      for (size_t i = 0; i < quiet_len; ++i) {
        if (rlen && strcmp(runs[rlen - 1], quiet_seq[i]) == 0) {
          rcounts[rlen - 1]++;
        } else if (rlen < NYIR_MAX_PASS_STATS) {
          runs[rlen] = quiet_seq[i];
          rcounts[rlen] = 1;
          rlen++;
        }
      }
      /*
       * Group consecutive tbuf_/loop_/inline_/etc. pairs with braces.
       */
      for (size_t i = 0; i < rlen && shown < 10;) {
        char token[256] = {0};
        size_t next = i + 1;
        /*
         * Common-prefix check (mirrors pipeline.c grouping).
         */
        size_t prefix = 0;
        if (i + 1 < rlen) {
          const char *a = runs[i];
          const char *b = runs[i + 1];
          size_t k = 0;
          size_t last_sep = 0;
          while (a[k] && b[k] && a[k] == b[k]) {
            if (a[k] == '_' || a[k] == '-')
              last_sep = k + 1;
            k++;
          }
          if (last_sep >= 2 && a[last_sep] && b[last_sep])
            prefix = last_sep;
        }
        if (prefix) {
          size_t ge = i + 1;
          while (ge + 1 < rlen) {
            const char *a = runs[ge];
            const char *b = runs[ge + 1];
            size_t k = 0;
            size_t ls = 0;
            while (a[k] && b[k] && a[k] == b[k]) {
              if (a[k] == '_' || a[k] == '-')
                ls = k + 1;
              k++;
            }
            if (ls != prefix || strncmp(runs[i], runs[ge + 1], prefix) != 0)
              break;
            ge++;
          }
          /*
           * Render prefix-{suf1,suf2}.
           */
          char pfx[64] = {0};
          size_t pl = prefix < sizeof(pfx) - 1 ? prefix : sizeof(pfx) - 1;
          for (size_t k = 0; k < pl; ++k)
            pfx[k] = runs[i][k] == '_' ? '-' : runs[i][k];
          size_t pos = snprintf(token, sizeof(token), "%s{", pfx);
          for (size_t k = i; k <= ge && pos < sizeof(token) - 1; ++k) {
            char suf[64] = {0};
            size_t sl = 0;
            for (const char *p = runs[k] + prefix; *p && sl + 1 < sizeof(suf);
                 ++p, ++sl)
              suf[sl] = *p == '_' ? '-' : *p;
            suf[sl] = '\0';
            pos += snprintf(token + pos, sizeof(token) - pos, "%s%s",
                            k > i ? "," : "", suf);
            if (rcounts[k] > 1)
              pos += snprintf(token + pos, sizeof(token) - pos, "×%zu",
                              rcounts[k]);
          }
          snprintf(token + strlen(token), sizeof(token) - strlen(token), "}");
          next = ge + 1;
        } else {
          /*
           * Suffix check for *-sroa / *-elim families.
           */
          size_t slen = 0;
          if (i + 1 < rlen) {
            const char *a = runs[i];
            const char *b = runs[i + 1];
            size_t al = strlen(a);
            size_t bl = strlen(b);
            size_t k = 0;
            while (k < al && k < bl && a[al - 1 - k] == b[bl - 1 - k])
              k++;
            if (k >= 4) {
              size_t as = al - k;
              size_t bs = bl - k;
              if (as > 1 && bs > 1 && (a[as] == '_' || a[as] == '-') &&
                  (b[bs] == '_' || b[bs] == '-'))
                slen = k;
            }
          }
          if (slen) {
            size_t al = strlen(runs[i]);
            const char *suf = runs[i] + al - slen;
            char dsuf[64] = {0};
            size_t dl = 0;
            for (const char *p = suf; *p && dl + 1 < sizeof(dsuf); ++p, ++dl)
              dsuf[dl] = *p == '_' ? '-' : *p;
            dsuf[dl] = '\0';
            size_t ge = i + 1;
            while (ge + 1 < rlen) {
              const char *nxt = runs[ge + 1];
              size_t nl = strlen(nxt);
              if (nl < slen || strcmp(nxt + nl - slen, suf) != 0)
                break;
              /*
               * Require same suffix length via shared separator.
               */
              size_t a2l = strlen(runs[ge]);
              size_t b2l = nl;
              size_t kk = 0;
              while (kk < a2l && kk < b2l &&
                     runs[ge][a2l - 1 - kk] == nxt[b2l - 1 - kk])
                kk++;
              if (kk != slen)
                break;
              ge++;
            }
            size_t pos = snprintf(token, sizeof(token), "{");
            for (size_t k = i; k <= ge && pos < sizeof(token) - 1; ++k) {
              size_t kl = strlen(runs[k]);
              char stem[64] = {0};
              size_t stl = kl > slen ? kl - slen : 0;
              if (stl >= sizeof(stem))
                stl = sizeof(stem) - 1;
              for (size_t s = 0; s < stl; ++s)
                stem[s] = runs[k][s] == '_' ? '-' : runs[k][s];
              stem[stl] = '\0';
              pos += snprintf(token + pos, sizeof(token) - pos, "%s%s",
                              k > i ? "," : "", stem);
              if (rcounts[k] > 1)
                pos += snprintf(token + pos, sizeof(token) - pos, "×%zu",
                                rcounts[k]);
            }
            snprintf(token + strlen(token), sizeof(token) - strlen(token),
                     "}%s", dsuf);
            next = ge + 1;
          } else {
            char disp[64] = {0};
            size_t dl = 0;
            for (const char *p = runs[i]; *p && dl + 1 < sizeof(disp);
                 ++p, ++dl)
              disp[dl] = *p == '_' ? '-' : *p;
            disp[dl] = '\0';
            if (rcounts[i] > 1)
              snprintf(token, sizeof(token), "%s×%zu", disp, rcounts[i]);
            else
              snprintf(token, sizeof(token), "%s", disp);
          }
        }
        size_t need = strlen(token) + (shown ? 2 : 1) + 1;
        if (glen + need >= sizeof(grouped))
          break;
        glen += (size_t)snprintf(grouped + glen, sizeof(grouped) - glen, "%s%s",
                                 shown ? ", " : " ", token);
        shown++;
        i = next;
      }
      size_t hidden = rlen > shown ? rlen - shown : 0;
      /*
       * Truncate long quiet lists to one line.
       */
      char short_grouped[72] = {0};
      snprintf(short_grouped, sizeof(short_grouped), "%.60s%s", grouped,
               strlen(grouped) > 60 ? "…" : "");
      if (hidden)
        fprintf(out, "  (quiet: %zu row%s, %.2fms:%s, +%zu more)\n", quiet_rows,
                quiet_rows == 1 ? "" : "s", quiet_ms, short_grouped, hidden);
      else
        fprintf(out, "  (quiet: %zu row%s, %.2fms:%s)\n", quiet_rows,
                quiet_rows == 1 ? "" : "s", quiet_ms, short_grouped);
    }
    fprintf(out, "  total                %7.2f ms\n", stats->total_time_ms);
  }
}

size_t nyr_max_local(const nyr_func_t *f) {
  if (!f)
    return 0;
  int64_t mx = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm > mx)
      mx = in->imm;
  }
  return mx >= 0 ? (size_t)mx + 1 : 0;
}
