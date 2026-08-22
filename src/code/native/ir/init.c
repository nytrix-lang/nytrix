/*
 * NYIR init: bootstraps the NYIR subsystem by amalgamating core,
 * binary, eval, verify, ssa, regalloc, machine, and isle sources.
 */
#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "code/native/object/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nyir_call_args(const nyir_inst_t *in, int value_count, int *args,
                      size_t args_cap, int *argc_out, char *err,
                      size_t err_len) {
  if (!in || in->op != NYIR_CALL || !args || !argc_out) {
    if (err && err_len)
      snprintf(err, err_len, "native NYIR call: invalid input");
    return false;
  }
  int argc = (int)in->imm;
  if (argc < 0 || argc > NYIR_CALL_MAX_ARGS || (size_t)argc > args_cap) {
    if (err && err_len)
      snprintf(err, err_len,
               "native NYIR call: argument count %d exceeds capacity %zu",
               argc, args_cap);
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
                 "native NYIR call: invalid argument %d value v%d", i,
                 value);
      return false;
    }
    args[i] = value;
  }
  *argc_out = argc;
  return true;
}

static bool nyir_op_f64(nyir_op_t op) {
  return op == NYIR_CONST_F64 || op == NYIR_ADD_F64 ||
         op == NYIR_SUB_F64 || op == NYIR_MUL_F64 || op == NYIR_DIV_F64 ||
         op == NYIR_I64_TO_F64 || op == NYIR_F32_TO_F64 ||
         op == NYIR_SQRT_F64 || op == NYIR_SIN_F64 || op == NYIR_COS_F64 ||
         op == NYIR_VEC4_REDUCE_ADD_F64;
}
static bool nyir_op_f32(nyir_op_t op) {
  return op == NYIR_CONST_F32 || op == NYIR_ADD_F32 ||
         op == NYIR_SUB_F32 || op == NYIR_MUL_F32 || op == NYIR_DIV_F32 ||
         op == NYIR_I64_TO_F32 || op == NYIR_F64_TO_F32;
}

static bool nyir_op_v128_i64(nyir_op_t op) {
  return op == NYIR_VEC4_LOAD_I64 || op == NYIR_VEC4_SET1_I64 ||
         op == NYIR_VEC4_ADD_I64 ||
         op == NYIR_VEC4_SUB_I64 || op == NYIR_VEC4_AND_I64 ||
         op == NYIR_VEC4_OR_I64 || op == NYIR_VEC4_XOR_I64 ||
         op == NYIR_VEC4_SHL_I64 || op == NYIR_VEC4_SAR_I64;
}

/*
 * VEC8_I64 is classified as v128 in the type map until backends emit
 * distinct 256-bit register classes.
 */
static bool nyir_op_v256_i64(nyir_op_t op) {
  return op == NYIR_VEC8_LOAD_I64 || op == NYIR_VEC8_ADD_I64 ||
         op == NYIR_VEC8_SUB_I64 || op == NYIR_VEC8_AND_I64 ||
         op == NYIR_VEC8_OR_I64 || op == NYIR_VEC8_XOR_I64;
}

static bool nyir_op_v128_f64(nyir_op_t op) {
  return op == NYIR_VEC4_LOAD_F64 || op == NYIR_VEC4_ADD_F64 ||
         op == NYIR_VEC4_SUB_F64 || op == NYIR_VEC4_MUL_F64 ||
         op == NYIR_VEC4_DIV_F64 || op == NYIR_VEC4_FMA_F64 ||
         op == NYIR_VEC4_SET1_F64 || op == NYIR_VEC4_SHUFFLE_F64;
}

static bool nyir_op_v128_f32(nyir_op_t op) {
  return op == NYIR_VEC8_LOAD_F32 || op == NYIR_VEC8_ADD_F32 ||
         op == NYIR_VEC8_SUB_F32 || op == NYIR_VEC8_MUL_F32 ||
         op == NYIR_VEC8_DIV_F32 || op == NYIR_VEC8_FMA_F32 ||
         op == NYIR_VEC8_SET1_F32 || op == NYIR_VEC8_SHUFFLE_F32;
}

static size_t nyir_type_root(size_t *parents, size_t node) {
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

static void nyir_type_union(size_t *parents, size_t left, size_t right) {
  left = nyir_type_root(parents, left);
  right = nyir_type_root(parents, right);
  if (left != right)
    parents[right] = left;
}

void nyir_type_map_free(nyir_type_map_t *map) {
  if (!map) return;
  free(map->value_f64);
  free(map->value_f32);
  free(map->value_v128_i64);
  free(map->value_v256_i64);
  free(map->value_v128_f64);
  free(map->value_v128_f32);
  free(map->local_f64);
  free(map->local_f32);
  free(map->local_v128_i64);
  free(map->local_v256_i64);
  free(map->local_v128_f64);
  free(map->local_v128_f32);
  *map = (nyir_type_map_t){0};
}

bool nyir_type_map_init(nyir_type_map_t *map, const nyir_func_t *nyir,
                          size_t local_count) {
  if (!map || !nyir) return false;
  if (nyir->param_count && !nyir->param_types)
    return false;
  *map = (nyir_type_map_t){.value_count = (size_t)nyir->next_value,
                             .local_count = local_count};
  if (map->value_count) {
    map->value_f64 = calloc(map->value_count, sizeof(bool));
    map->value_f32 = calloc(map->value_count, sizeof(bool));
    map->value_v128_i64 = calloc(map->value_count, sizeof(bool));
    map->value_v256_i64 = calloc(map->value_count, sizeof(bool));
    map->value_v128_f64 = calloc(map->value_count, sizeof(bool));
    map->value_v128_f32 = calloc(map->value_count, sizeof(bool));
  }
  if (local_count) {
    map->local_f64 = calloc(local_count, sizeof(bool));
    map->local_f32 = calloc(local_count, sizeof(bool));
    map->local_v128_i64 = calloc(local_count, sizeof(bool));
    map->local_v256_i64 = calloc(local_count, sizeof(bool));
    map->local_v128_f64 = calloc(local_count, sizeof(bool));
    map->local_v128_f32 = calloc(local_count, sizeof(bool));
  }
  if ((map->value_count &&
       (!map->value_f64 || !map->value_f32 || !map->value_v128_i64 ||
        !map->value_v256_i64 || !map->value_v128_f64 || !map->value_v128_f32)) ||
      (local_count &&
       (!map->local_f64 || !map->local_f32 || !map->local_v128_i64 ||
        !map->local_v256_i64 || !map->local_v128_f64 || !map->local_v128_f32))) {
    nyir_type_map_free(map);
    return false;
  }

  size_t node_count = map->value_count + local_count;
  size_t *parents = node_count ? malloc(node_count * sizeof(*parents)) : NULL;
  unsigned char *root_types = node_count ? calloc(node_count, 1) : NULL;
  if (node_count && (!parents || !root_types)) {
    free(parents);
    free(root_types);
    nyir_type_map_free(map);
    return false;
  }
  for (size_t i = 0; i < node_count; ++i)
    parents[i] = i;

  /*
   * Copies and local loads/stores preserve type. Collapse those constraints
   * once so long chains remain near-linear rather than requiring fixed-point
   * rescans of the complete function.
   */
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyir_inst_t *in = &nyir->data[i];
    if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
        (size_t)in->dst < map->value_count &&
        (size_t)in->a < map->value_count)
      nyir_type_union(parents, (size_t)in->dst, (size_t)in->a);
    if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
        (size_t)in->dst < map->value_count && (size_t)in->imm < local_count)
      nyir_type_union(parents, (size_t)in->dst,
                        map->value_count + (size_t)in->imm);
    if (in->op == NYIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
        (size_t)in->a < map->value_count && (size_t)in->imm < local_count)
      nyir_type_union(parents, (size_t)in->a,
                        map->value_count + (size_t)in->imm);
  }

  size_t param_count = nyir->param_count < local_count
                           ? nyir->param_count
                           : local_count;
  for (size_t i = 0; i < param_count; ++i) {
    size_t root = nyir_type_root(parents, map->value_count + i);
    if (nyir->param_types[i] == NYIR_PARAM_F64)
      root_types[root] |= 1u;
    else if (nyir->param_types[i] == NYIR_PARAM_F32)
      root_types[root] |= 2u;
  }

  for (size_t i = 0; i < nyir->len; ++i) {
    const nyir_inst_t *in = &nyir->data[i];
    bool f64_result = nyir_op_f64(in->op) ||
        (in->op == NYIR_LOAD_I64 && (in->flags & NYIR_INST_F_MEM_F64)) ||
        ((in->op == NYIR_CALL || in->op == NYIR_RET) &&
         (in->flags & NYIR_INST_F_RET_F64));
    bool f32_result = nyir_op_f32(in->op) ||
        ((in->op == NYIR_CALL || in->op == NYIR_RET) &&
         (in->flags & NYIR_INST_F_RET_F32));
    bool v128_i64_result = nyir_op_v128_i64(in->op) ||
        (in->op == NYIR_CAPTURE_RET && in->imm >= 10 && in->imm <= 13);
    bool v256_i64_result = nyir_op_v256_i64(in->op);
    bool v128_f64_result = nyir_op_v128_f64(in->op);
    bool v128_f32_result = nyir_op_v128_f32(in->op);
    if (in->dst >= 0 && (size_t)in->dst < map->value_count) {
      size_t root = nyir_type_root(parents, (size_t)in->dst);
      if (f64_result)
        root_types[root] |= 1u;
      if (f32_result)
        root_types[root] |= 2u;
      if (v128_i64_result)
        root_types[root] |= 4u;
      if (v256_i64_result)
        root_types[root] |= 4u | 32u;
      if (v128_f64_result)
        root_types[root] |= 8u;
      if (v128_f32_result)
        root_types[root] |= 16u;
    }
    if (in->op == NYIR_RET && in->a >= 0 &&
        (size_t)in->a < map->value_count) {
      size_t root = nyir_type_root(parents, (size_t)in->a);
      if (f64_result)
        root_types[root] |= 1u;
      if (f32_result)
        root_types[root] |= 2u;
    }
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
    bool v128_f64_ops =
        in->op == NYIR_VEC4_ADD_F64 || in->op == NYIR_VEC4_SUB_F64 ||
        in->op == NYIR_VEC4_MUL_F64 || in->op == NYIR_VEC4_DIV_F64 ||
        in->op == NYIR_VEC4_FMA_F64 || in->op == NYIR_VEC4_SHUFFLE_F64;
    bool v128_f32_ops =
        in->op == NYIR_VEC8_ADD_F32 || in->op == NYIR_VEC8_SUB_F32 ||
        in->op == NYIR_VEC8_MUL_F32 || in->op == NYIR_VEC8_DIV_F32 ||
        in->op == NYIR_VEC8_FMA_F32 || in->op == NYIR_VEC8_SHUFFLE_F32;
    if (in->a >= 0 && (size_t)in->a < map->value_count) {
      size_t root = nyir_type_root(parents, (size_t)in->a);
      if (f64_operands)
        root_types[root] |= 1u;
      if (f32_operands)
        root_types[root] |= 2u;
      if (v128_i64_ops)
        root_types[root] |= 4u;
      if (v256_i64_ops)
        root_types[root] |= 4u | 32u;
      if (v128_f64_ops)
        root_types[root] |= 8u;
      if (v128_f32_ops)
        root_types[root] |= 16u;
    }
    if (in->b >= 0 && (size_t)in->b < map->value_count) {
      size_t root = nyir_type_root(parents, (size_t)in->b);
      if (f64_operands)
        root_types[root] |= 1u;
      if (f32_operands)
        root_types[root] |= 2u;
      if (v128_i64_ops)
        root_types[root] |= 4u;
      if (v256_i64_ops)
        root_types[root] |= 4u | 32u;
      if (v128_f64_ops)
        root_types[root] |= 8u;
      if (v128_f32_ops)
        root_types[root] |= 16u;
    }
    if (in->c >= 0 && (size_t)in->c < map->value_count &&
        (in->op == NYIR_VEC4_FMA_F64 || in->op == NYIR_VEC8_FMA_F32 ||
         (in->op == NYIR_STORE_I64 && (in->flags & NYIR_INST_F_MEM_F64)))) {
      size_t root = nyir_type_root(parents, (size_t)in->c);
      if (in->op == NYIR_VEC4_FMA_F64)
        root_types[root] |= 8u;
      else if (in->op == NYIR_STORE_I64 &&
               (in->flags & NYIR_INST_F_MEM_F64))
        root_types[root] |= 1u;
      else
        root_types[root] |= 16u;
    }
    if (in->op == NYIR_VEC4_REDUCE_ADD_F64) {
      if (in->a >= 0 && (size_t)in->a < map->value_count) {
        size_t root = nyir_type_root(parents, (size_t)in->a);
        root_types[root] |= 1u;
      }
      if (in->b >= 0 && (size_t)in->b < map->value_count) {
        size_t root = nyir_type_root(parents, (size_t)in->b);
        root_types[root] |= 8u;
      }
    }
    if (in->op == NYIR_VEC4_REDUCE_ADD_I64 ||
        in->op == NYIR_VEC8_REDUCE_ADD_I64) {
      if (in->b >= 0 && (size_t)in->b < map->value_count) {
        size_t root = nyir_type_root(parents, (size_t)in->b);
        root_types[root] |= in->op == NYIR_VEC8_REDUCE_ADD_I64 ? (4u | 32u) : 4u;
      }
    }
    if (in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_STORE_I64 ||
        in->op == NYIR_VEC4_STORE_F64 || in->op == NYIR_VEC8_STORE_F32) {
      int value = in->b >= 0 ? in->b : in->c;
      if (value >= 0 && (size_t)value < map->value_count) {
        size_t root = nyir_type_root(parents, (size_t)value);
        if (in->op == NYIR_VEC4_STORE_I64)
          root_types[root] |= 4u;
        else if (in->op == NYIR_VEC8_STORE_I64)
          root_types[root] |= 4u | 32u;
        else if (in->op == NYIR_VEC4_STORE_F64)
          root_types[root] |= 8u;
        else
          root_types[root] |= 16u;
      }
    }
  }

  for (size_t i = 0; i < map->value_count; ++i) {
    unsigned char type = root_types[nyir_type_root(parents, i)];
    map->value_f64[i] = (type & 1u) != 0;
    map->value_f32[i] = (type & 2u) != 0;
    map->value_v128_i64[i] = (type & 4u) != 0;
    map->value_v256_i64[i] = (type & 32u) != 0;
    map->value_v128_f64[i] = (type & 8u) != 0;
    map->value_v128_f32[i] = (type & 16u) != 0;
  }
  for (size_t i = 0; i < local_count; ++i) {
    unsigned char type =
        root_types[nyir_type_root(parents, map->value_count + i)];
    map->local_f64[i] = (type & 1u) != 0;
    map->local_f32[i] = (type & 2u) != 0;
    map->local_v128_i64[i] = (type & 4u) != 0;
    map->local_v256_i64[i] = (type & 32u) != 0;
    map->local_v128_f64[i] = (type & 8u) != 0;
    map->local_v128_f32[i] = (type & 16u) != 0;
  }
  free(parents);
  free(root_types);
  return true;
}

int64_t nyir_f64_to_bits(double v) {
  int64_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

double nyir_bits_to_f64(int64_t bits) {
  double v = 0;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

int64_t nyir_f32_to_bits(float v) {
  int32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return (int64_t)(uint32_t)bits;
}

float nyir_bits_to_f32(int64_t bits) {
  int32_t b32 = (int32_t)(uint32_t)bits;
  float v = 0;
  memcpy(&v, &b32, sizeof(v));
  return v;
}

static const char *nyir_own_symbol_copy(nyir_func_t *f,
                                          const char *symbol) {
  if (!symbol)
    return NULL;
  char *copy = ny_strndup(symbol, strlen(symbol));
  if (!copy)
    return NULL;
  if (f->owned_symbols_len >= f->owned_symbols_cap) {
    size_t cap = f->owned_symbols_cap ? f->owned_symbols_cap * 2 : 16;
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

void nyir_func_free(nyir_func_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->owned_symbols_len; ++i)
    free(f->owned_symbols[i]);
  free(f->owned_symbols);
  free(f->param_types);
  for (size_t i = 0; i < f->len; ++i) {
    free(f->data[i].extra_args);
    free(f->data[i].arg_sizes);
    free(f->data[i].phi_incoming);
  }
  free(f->data);
  memset(f, 0, sizeof(*f));
}

bool nyir_func_clone(const nyir_func_t *src, nyir_func_t *dst) {
  if (!src || !dst)
    return false;
  memset(dst, 0, sizeof(*dst));
  dst->next_value = src->next_value;
  dst->vectorize_attempted_loops = src->vectorize_attempted_loops;
  dst->vectorize_rejected_loops = src->vectorize_rejected_loops;
  dst->vectorized_loops = src->vectorized_loops;
  if (src->param_count) {
    if (!src->param_types)
      return false;
    dst->param_types = malloc(src->param_count * sizeof(*dst->param_types));
    if (!dst->param_types)
      return false;
    memcpy(dst->param_types, src->param_types,
           src->param_count * sizeof(*dst->param_types));
    dst->param_count = src->param_count;
  }
  if (src->len == 0)
    return true;
  dst->data = (nyir_inst_t *)calloc(src->len, sizeof(nyir_inst_t));
  if (!dst->data) {
    nyir_func_free(dst);
    return false;
  }
  dst->cap = src->len;
  for (size_t i = 0; i < src->len; ++i) {
    const nyir_inst_t *s = &src->data[i];
    nyir_inst_t d = *s;
    d.extra_args = NULL;
    d.extra_args_len = 0;
    d.arg_sizes = NULL;
    d.phi_incoming = NULL;
    d.phi_incoming_len = 0;
    d.symbol = NULL;
    d.debug.file = NULL;
    if (s->symbol) {
      d.symbol = nyir_own_symbol_copy(dst, s->symbol);
      if (!d.symbol) {
        nyir_func_free(dst);
        return false;
      }
    }
    if (s->debug.file && s->debug.file[0]) {
      d.debug.file = nyir_own_symbol_copy(dst, s->debug.file);
      if (!d.debug.file) {
        nyir_func_free(dst);
        return false;
      }
    }
    if (s->extra_args && s->extra_args_len > 0) {
      d.extra_args =
          (int *)malloc(s->extra_args_len * sizeof(*d.extra_args));
      if (!d.extra_args) {
        nyir_func_free(dst);
        return false;
      }
      memcpy(d.extra_args, s->extra_args,
             s->extra_args_len * sizeof(*d.extra_args));
      d.extra_args_len = s->extra_args_len;
    }
    if (s->arg_sizes && s->imm > 0) {
      d.arg_sizes = (uint32_t *)malloc((size_t)s->imm * sizeof(*d.arg_sizes));
      if (!d.arg_sizes) {
        free(d.extra_args);
        nyir_func_free(dst);
        return false;
      }
      memcpy(d.arg_sizes, s->arg_sizes, (size_t)s->imm * sizeof(*d.arg_sizes));
    }
    if (s->phi_incoming && s->phi_incoming_len > 0) {
      d.phi_incoming = (nyir_phi_incoming_t *)malloc(
          s->phi_incoming_len * sizeof(*d.phi_incoming));
      if (!d.phi_incoming) {
        free(d.extra_args);
        free(d.arg_sizes);
        nyir_func_free(dst);
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

void nyir_inst_discard(nyir_inst_t *in) {
  if (!in)
    return;
  free(in->extra_args);
  free(in->arg_sizes);
  free(in->phi_incoming);
  *in = (nyir_inst_t){.op = NYIR_NOP,
                        .dst = -1,
                        .a = -1,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1};
}

bool nyir_replace_all_uses(nyir_func_t *f, int old_value, int new_value) {
  if (!f || old_value < 0 || new_value < 0 || old_value >= f->next_value ||
      new_value >= f->next_value)
    return false;
  if (old_value == new_value)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
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

bool nyir_erase_instruction(nyir_func_t *f, size_t index) {
  if (!f || index >= f->len)
    return false;
  nyir_inst_discard(&f->data[index]);
  return true;
}

void nyir_use_def_free(nyir_use_def_t *uses) {
  if (!uses)
    return;
  free(uses->offsets);
  free(uses->users);
  *uses = (nyir_use_def_t){0};
}

bool nyir_build_use_def(const nyir_func_t *f, nyir_use_def_t *out) {
  if (!f || !out || f->next_value < 0)
    return false;
  *out = (nyir_use_def_t){0};
  size_t values = (size_t)f->next_value;
  size_t *offsets = calloc(values + 1, sizeof(*offsets));
  if (!offsets)
    return false;
#define NIR_COUNT_USE(v) do {                                             \
  if ((v) >= 0) {                                                         \
    if ((size_t)(v) >= values || offsets[(size_t)(v) + 1] == SIZE_MAX)    \
      goto fail;                                                          \
    ++offsets[(size_t)(v) + 1];                                           \
  }                                                                       \
} while (0)
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    NIR_COUNT_USE(in->a); NIR_COUNT_USE(in->b); NIR_COUNT_USE(in->c);
    NIR_COUNT_USE(in->d); NIR_COUNT_USE(in->e); NIR_COUNT_USE(in->f);
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
#define NIR_ADD_USE(v) do {                                               \
  if ((v) >= 0)                                                           \
    users[cursor[(size_t)(v)]++] = i;                                     \
} while (0)
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    NIR_ADD_USE(in->a); NIR_ADD_USE(in->b); NIR_ADD_USE(in->c);
    NIR_ADD_USE(in->d); NIR_ADD_USE(in->e); NIR_ADD_USE(in->f);
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

const char *nyir_op_name(nyir_op_t op) {
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
  case NYIR_OP_COUNT:
    break;
  }
  return "unknown";
}

unsigned nyir_inst_effects(const nyir_inst_t *inst) {
  if (!inst)
    return NYIR_EFFECT_NONE;
  switch (inst->op) {
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
  default:
    return NYIR_EFFECT_NONE;
  }
}

static void nyir_init_inst_metadata(nyir_inst_t *inst) {
  if (!inst)
    return;
  inst->effects = nyir_inst_effects(inst);
  if (inst->op == NYIR_CONST_I64 && !inst->range.has_min &&
      !inst->range.has_max) {
    inst->range.has_min = true;
    inst->range.has_max = true;
    inst->range.min = inst->imm;
    inst->range.max = inst->imm;
  } else if ((inst->op == NYIR_CMP_I64 || inst->op == NYIR_CMP_F64 ||
              inst->op == NYIR_CMP_F32) &&
             !inst->range.has_min &&
             !inst->range.has_max) {
    inst->range.has_min = true;
    inst->range.has_max = true;
    inst->range.min = 0;
    inst->range.max = 1;
  }
}

static void nyir_normalize_operands(nyir_inst_t *inst) {
  if (!inst)
    return;
  switch (inst->op) {
  case NYIR_BOUNDS_CHECK:
  case NYIR_VEC4_REDUCE_ADD_F64:
  case NYIR_VEC4_REDUCE_ADD_I64:
  case NYIR_VEC8_REDUCE_ADD_I64:
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
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_SQRT_F64:
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
  case NYIR_LOAD_I64:
  case NYIR_RET:
  case NYIR_BR_IF:
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
  case NYIR_CMP_I64:
  case NYIR_CMP_F64:
  case NYIR_CMP_F32:
    inst->c = -1;
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

void nyir_refresh_metadata(nyir_func_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    nyir_normalize_operands(in);
    /*
     * Calls tagged with NYIR_INST_F_EFFECTS_KNOWN carry a builder-inferred
     * interprocedural summary in in->effects; keep it instead of restoring
     * the static CALL|UNKNOWN_SIDE_EFFECT default.
     */
    if (!(in->op == NYIR_CALL &&
          (in->flags & NYIR_INST_F_EFFECTS_KNOWN)))
      in->effects = nyir_inst_effects(in);
    if (in->op == NYIR_CONST_I64) {
      in->range = (nyir_range_t){.has_min = true,
                                   .has_max = true,
                                   .min = in->imm,
                                   .max = in->imm};
    } else if (in->op == NYIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
               in->op == NYIR_CMP_F32) {
      in->range = (nyir_range_t){.has_min = true,
                                   .has_max = true,
                                   .min = 0,
                                   .max = 1};
    }
  }
}

int nyir_emit(nyir_func_t *f, nyir_inst_t inst) {
  if (!f)
    return -1;
  nyir_normalize_operands(&inst);
  if (f->len >= f->cap) {
    size_t cap = f->cap ? f->cap * 2 : 64;
    nyir_inst_t *data = (nyir_inst_t *)realloc(f->data, cap * sizeof(*data));
    if (!data)
      return -1;
    f->data = data;
    f->cap = cap;
  }
  if (inst.symbol) {
    inst.symbol = nyir_own_symbol_copy(f, inst.symbol);
    if (!inst.symbol)
      return -1;
  }
  if (inst.dst < 0 && inst.op != NYIR_STORE_LOCAL && inst.op != NYIR_RET &&
      inst.op != NYIR_BR && inst.op != NYIR_BR_IF &&
      inst.op != NYIR_LABEL && inst.op != NYIR_NOP)
    inst.dst = f->next_value++;
  nyir_init_inst_metadata(&inst);
  f->data[f->len++] = inst;
  return inst.dst;
}

bool nyir_err(char *err, size_t err_len, const char *fmt, ...) {
  if (err && err_len > 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
  }
  return false;
}

bool nyir_inst_err(char *err, size_t err_len, const nyir_inst_t *in,
                     size_t index, const char *reason) {
  if (in && in->debug.line) {
    if (in->debug.file && in->debug.file[0]) {
      return nyir_err(err, err_len,
                      "native NYIR verify: inst %zu opcode=%s dst=v%d a=v%d b=v%d "
                      "imm=%" PRId64 " at %s:%u:%u: %s",
                      index, nyir_op_name(in->op), in->dst, in->a, in->b, in->imm,
                      in->debug.file, in->debug.line, in->debug.column,
                      reason ? reason : "invalid instruction");
    }
    return nyir_err(err, err_len,
                    "native NYIR verify: inst %zu opcode=%s dst=v%d a=v%d b=v%d "
                    "imm=%" PRId64 " at line %u:%u: %s",
                    index, nyir_op_name(in->op), in->dst, in->a, in->b, in->imm,
                    in->debug.line, in->debug.column,
                    reason ? reason : "invalid instruction");
  }
  return nyir_err(err, err_len,
                  "native NYIR verify: inst %zu opcode=%s dst=v%d a=v%d b=v%d "
                  "imm=%" PRId64 ": %s",
                  index, in ? nyir_op_name(in->op) : "<null>",
                  in ? in->dst : -1, in ? in->a : -1, in ? in->b : -1,
                  in ? in->imm : 0, reason ? reason : "invalid instruction");
}

static const char *nyir_dump_color(FILE *out, const char *code) {
  return color_enabled() && (out == stderr || out == stdout) ? code : "";
}

static const char *nyir_cmp_i64_name(nyir_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ: return "eq";
  case NYIR_CMP_NE: return "ne";
  case NYIR_CMP_LT: return "slt";
  case NYIR_CMP_LE: return "sle";
  case NYIR_CMP_GT: return "sgt";
  case NYIR_CMP_GE: return "sge";
  }
  return "invalid";
}

/*
 * Compact TUI-like dump helpers (not a real TUI): short ops, pack many
 * insts per line, elide nops, skip verbose effects/range spam.
 */

static const char *nyir_op_short(const nyir_inst_t *in) {
  if (!in)
    return "?";
  switch (in->op) {
  case NYIR_NOP: return "nop";
  case NYIR_CONST_I64: return "i64";
  case NYIR_CONST_F64: return "f64";
  case NYIR_CONST_F32: return "f32";
  case NYIR_COPY: return "mov";
  case NYIR_PHI: return "phi";
  case NYIR_ADD_I64: return "add";
  case NYIR_SUB_I64: return "sub";
  case NYIR_MUL_I64: return "mul";
  case NYIR_DIV_I64: return "div";
  case NYIR_MOD_I64: return "mod";
  case NYIR_AND_I64: return "and";
  case NYIR_OR_I64: return "or";
  case NYIR_XOR_I64: return "xor";
  case NYIR_SHL_I64: return "shl";
  case NYIR_SAR_I64: return "sar";
  case NYIR_CMP_I64: return nyir_cmp_i64_name(in->cmp);
  case NYIR_LABEL: return "L";
  case NYIR_LOAD_LOCAL: return "ld";
  case NYIR_STORE_LOCAL: return "st";
  case NYIR_CALL: return "call";
  case NYIR_RET: return "ret";
  case NYIR_BR: return "br";
  case NYIR_BR_IF: return "br.if";
  case NYIR_ADD_F64: return "fadd";
  case NYIR_SUB_F64: return "fsub";
  case NYIR_MUL_F64: return "fmul";
  case NYIR_DIV_F64: return "fdiv";
  case NYIR_CMP_F64: return "fcmp";
  case NYIR_SQRT_F64: return "sqrt.f64";
  case NYIR_SIN_F64: return "sin.f64";
  case NYIR_COS_F64: return "cos.f64";
  case NYIR_ADDR_LOCAL: return "lea";
  case NYIR_LOAD_I64: return "ld.i64";
  case NYIR_STORE_I64: return "st.i64";
  case NYIR_ADDR_SYMBOL: return "sym";
  case NYIR_ALLOCA: return "alloca";
  case NYIR_COPY_STRUCT: return "memcpy";
  case NYIR_CAPTURE_RET: return "cap.ret";
  default: return nyir_op_name(in->op);
  }
}

/*
 * Format one inst into buf; returns false for pure nops (callers elide).
 */
static bool nyir_fmt_inst(const nyir_inst_t *in, char *buf, size_t n) {
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
    APP("%.6g", nyir_bits_to_f64(in->imm));
    return true;
  }
  if (in->op == NYIR_CONST_F32) {
    APP("%.6g", (double)nyir_bits_to_f32(in->imm));
    return true;
  }
  APP("%s", nyir_op_short(in));
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
    if (in->imm && in->op != NYIR_RET)
      APP(" #%" PRId64, in->imm);
  }
#undef APP
  return true;
}

static bool nyir_inst_same(const nyir_inst_t *a, const nyir_inst_t *b) {
  if (!a || !b)
    return a == b;
  if (a->op != b->op || a->dst != b->dst || a->a != b->a || a->b != b->b ||
      a->c != b->c || a->d != b->d || a->e != b->e || a->f != b->f ||
      a->imm != b->imm || a->cmp != b->cmp ||
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

void nyir_dump(FILE *out, const nyir_func_t *f, const char *name) {
  if (!out)
    out = stderr;
  const char *reset = nyir_dump_color(out, NY_CLR_RESET);
  const char *heading = nyir_dump_color(out, NY_CLR_BRIGHT_CYAN NY_CLR_BOLD);
  const char *muted = nyir_dump_color(out, NY_CLR_BRIGHT_BLACK);
  const char *value = nyir_dump_color(out, NY_CLR_BRIGHT_MAGENTA);
  const char *opcode = nyir_dump_color(out, NY_CLR_BRIGHT_GREEN);

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
  char ibuf[160];
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
    if (!nyir_fmt_inst(&f->data[i], ibuf, sizeof(ibuf)))
      continue;
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

void nyir_dump_diff(FILE *out, const nyir_func_t *before,
                      const nyir_func_t *after, const char *pass_tag) {
  if (!out)
    out = stderr;
  if (!after)
    return;
  const char *reset = nyir_dump_color(out, NY_CLR_RESET);
  const char *muted = nyir_dump_color(out, NY_CLR_BRIGHT_BLACK);
  const char *addc = nyir_dump_color(out, NY_CLR_BRIGHT_GREEN);
  const char *delc = nyir_dump_color(out, NY_CLR_BRIGHT_RED);
  const char *chgc = nyir_dump_color(out, NY_CLR_BRIGHT_YELLOW);

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
    const nyir_inst_t *bi =
        before && i < blen ? &before->data[i] : NULL;
    const nyir_inst_t *ai = i < alen ? &after->data[i] : NULL;
    bool bnop = !bi || bi->op == NYIR_NOP;
    bool anop = !ai || ai->op == NYIR_NOP;
    if (bnop && anop)
      continue;
    if (bi && ai && nyir_inst_same(bi, ai))
      continue;

    if (on_line >= 5) {
      fputc('\n', out);
      fprintf(out, "    ");
      on_line = 0;
    } else if (on_line)
      fputs("  ", out);

    if (bnop && !anop) {
      nyir_fmt_inst(ai, abuf, sizeof(abuf));
      fprintf(out, "%s+%zu:%s%s", addc, i, abuf, reset);
    } else if (!bnop && anop) {
      nyir_fmt_inst(bi, bbuf, sizeof(bbuf));
      fprintf(out, "%s-%zu:%s%s", delc, i, bbuf, reset);
    } else {
      nyir_fmt_inst(ai, abuf, sizeof(abuf));
      fprintf(out, "%s~%zu:%s%s", chgc, i, abuf, reset);
    }
    ++on_line;
    ++shown;
  }
  if (shown == 0)
    fprintf(out, "%s(no live --)%s", muted, reset);
  else if (shown >= MAX_DIFF_LINES)
    fprintf(out, "  %s---%s", muted, reset);
  fputc('\n', out);
}

void nyir_dump_cfg(FILE *out, const nyir_func_t *f, const char *name) {
  if (!out)
    out = stderr;
  if (!f) {
    fprintf(out, "nyir cfg function %s unavailable\n",
            name && name[0] ? name : "<anon>");
    return;
  }
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg)) {
    fprintf(out, "nyir cfg function %s unavailable (build failed)\n",
            name && name[0] ? name : "<anon>");
    return;
  }
  fprintf(out, "nyir cfg function %s blocks=%zu\n",
          name && name[0] ? name : "<anon>", cfg.block_count);
  size_t *rpo = NULL;
  size_t rpo_len = 0;
  if (nyir_cfg_reverse_postorder(&cfg, &rpo, &rpo_len)) {
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
  nyir_cfg_free(&cfg);
}

void nyir_eval_result_dump(FILE *out, const char *name,
                             const nyir_eval_result_t *result) {
  if (!out)
    out = stderr;
  const char *reset = nyir_dump_color(out, NY_CLR_RESET);
  const char *heading = nyir_dump_color(out, NY_CLR_BRIGHT_CYAN NY_CLR_BOLD);
  const char *value = nyir_dump_color(out, NY_CLR_BRIGHT_MAGENTA);
  fprintf(out,
          "%snyir vm profile%s function=%s%s%s returned=%s result=%" PRId64 " steps=%zu branches_taken=%zu branches_not_taken=%zu calls=%zu max_pc=%zu max_value=%zu max_local=%zu\n",
          heading, reset, value, name && name[0] ? name : "rt_main", reset,
          result && result->returned ? "yes" : "no",
          result ? result->result : 0, result ? result->steps : 0,
          result ? result->branch_taken : 0,
          result ? result->branch_not_taken : 0,
          result ? result->call_count : 0,
          result ? result->max_pc : 0,
          result ? result->max_value_index : 0,
          result ? result->max_local_index : 0);
  if (!result)
    return;
  fprintf(out, "NYP2 function=%s steps=%zu\n",
          name && name[0] ? name : "rt_main", result->steps);
  for (size_t i = 0; i < result->pc_count_len; ++i)
    if (result->pc_counts && result->pc_counts[i])
      fprintf(out, "pc %zu count=%" PRIu64 "\n", i, result->pc_counts[i]);
  for (size_t i = 0; i < result->edge_count; ++i)
    fprintf(out, "edge %zu %zu count=%" PRIu64 "\n",
            result->edges[i].from_pc, result->edges[i].to_pc,
            result->edges[i].count);
  for (size_t i = 0; i < (size_t)NYIR_OP_COUNT; ++i) {
    if (result->op_counts[i] == 0)
      continue;
    fprintf(out, "  op %s%-14s%s %zu\n",
            nyir_dump_color(out, NY_CLR_BRIGHT_GREEN),
            nyir_op_name((nyir_op_t)i), reset, result->op_counts[i]);
  }
}

void nyir_collect_stats(const nyir_func_t *f, size_t *insts,
                          int *values, size_t *ops, size_t op_count) {
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

void nyir_dump_stats(FILE *out, const nyir_opt_stats_t *stats) {
  if (!out)
    out = stderr;
  if (!stats)
    return;
  size_t removed = stats->before_insts > stats->after_insts
                       ? stats->before_insts - stats->after_insts
                       : 0;
  fprintf(out,
          "%snyir optimize%s before_insts=%zu after_insts=%zu removed=%zu "
          "before_values=%d after_values=%d\n",
          nyir_dump_color(out, NY_CLR_BRIGHT_CYAN NY_CLR_BOLD),
          nyir_dump_color(out, NY_CLR_RESET), stats->before_insts,
          stats->after_insts, removed,
          stats->before_values, stats->after_values);
  fprintf(out, "%snyir optimize ops%s",
          nyir_dump_color(out, NY_CLR_BRIGHT_CYAN NY_CLR_BOLD),
          nyir_dump_color(out, NY_CLR_RESET));
  for (size_t op = 0; op < (size_t)NYIR_OP_COUNT; ++op) {
    size_t before = stats->before_ops[op];
    size_t after = stats->after_ops[op];
    if (!before && !after)
      continue;
    fprintf(out, " %s%s%s:%zu->%zu",
            nyir_dump_color(out, NY_CLR_BRIGHT_GREEN),
            nyir_op_name((nyir_op_t)op),
            nyir_dump_color(out, NY_CLR_RESET), before, after);
  }
  fputc('\n', out);
  {
    unsigned long long mach_ok = 0, nir_fb = 0;
    ny_native_mach_encode_stats(&mach_ok, &nir_fb);
    fprintf(out,
            "%smach encode%s mach_ok=%llu nir_fallback=%llu\n",
            nyir_dump_color(out, NY_CLR_BRIGHT_CYAN NY_CLR_BOLD),
            nyir_dump_color(out, NY_CLR_RESET), mach_ok, nir_fb);
  }
}

size_t nyir_max_local(const nyir_func_t *f) {
  if (!f)
    return 0;
  int64_t mx = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm > mx)
      mx = in->imm;
  }
  return mx >= 0 ? (size_t)mx + 1 : 0;
}
