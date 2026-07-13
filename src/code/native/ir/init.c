#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "base/compat.h"
#include "base/common.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ny_nir_call_args(const ny_nir_inst_t *in, int value_count, int *args,
                      size_t args_cap, int *argc_out, char *err,
                      size_t err_len) {
  if (!in || in->op != NY_NIR_CALL || !args || !argc_out) {
    if (err && err_len)
      snprintf(err, err_len, "native NYIR call: invalid input");
    return false;
  }
  int argc = (int)in->imm;
  if (argc < 0 || argc > NY_NIR_CALL_MAX_ARGS || (size_t)argc > args_cap) {
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

static bool ny_nir_op_f64(ny_nir_op_t op) {
  return op == NYIR_CONST_F64 || op == NYIR_ADD_F64 ||
         op == NYIR_SUB_F64 || op == NYIR_MUL_F64 || op == NYIR_DIV_F64 ||
         op == NYIR_I64_TO_F64 || op == NYIR_F32_TO_F64;
}

static bool ny_nir_op_f32(ny_nir_op_t op) {
  return op == NYIR_CONST_F32 || op == NYIR_ADD_F32 ||
         op == NYIR_SUB_F32 || op == NYIR_MUL_F32 || op == NYIR_DIV_F32 ||
         op == NYIR_I64_TO_F32 || op == NYIR_F64_TO_F32;
}

static size_t ny_nir_type_root(size_t *parents, size_t node) {
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

static void ny_nir_type_union(size_t *parents, size_t left, size_t right) {
  left = ny_nir_type_root(parents, left);
  right = ny_nir_type_root(parents, right);
  if (left != right)
    parents[right] = left;
}

void ny_nir_type_map_free(ny_nir_type_map_t *map) {
  if (!map) return;
  free(map->value_f64); free(map->value_f32);
  free(map->local_f64); free(map->local_f32);
  *map = (ny_nir_type_map_t){0};
}

bool ny_nir_type_map_init(ny_nir_type_map_t *map, const ny_nir_func_t *nir,
                          size_t local_count) {
  if (!map || !nir) return false;
  *map = (ny_nir_type_map_t){.value_count = (size_t)nir->next_value,
                             .local_count = local_count};
  if (map->value_count) {
    map->value_f64 = calloc(map->value_count, sizeof(bool));
    map->value_f32 = calloc(map->value_count, sizeof(bool));
  }
  if (local_count) {
    map->local_f64 = calloc(local_count, sizeof(bool));
    map->local_f32 = calloc(local_count, sizeof(bool));
  }
  if ((map->value_count && (!map->value_f64 || !map->value_f32)) ||
      (local_count && (!map->local_f64 || !map->local_f32))) {
    ny_nir_type_map_free(map);
    return false;
  }

  size_t node_count = map->value_count + local_count;
  size_t *parents = node_count ? malloc(node_count * sizeof(*parents)) : NULL;
  unsigned char *root_types = node_count ? calloc(node_count, 1) : NULL;
  if (node_count && (!parents || !root_types)) {
    free(parents);
    free(root_types);
    ny_nir_type_map_free(map);
    return false;
  }
  for (size_t i = 0; i < node_count; ++i)
    parents[i] = i;

  /* Copies and local loads/stores preserve type. Collapse those constraints
   * once so long chains remain near-linear rather than requiring fixed-point
   * rescans of the complete function. */
  for (size_t i = 0; i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    if (in->op == NY_NIR_COPY && in->dst >= 0 && in->a >= 0 &&
        (size_t)in->dst < map->value_count &&
        (size_t)in->a < map->value_count)
      ny_nir_type_union(parents, (size_t)in->dst, (size_t)in->a);
    if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
        (size_t)in->dst < map->value_count && (size_t)in->imm < local_count)
      ny_nir_type_union(parents, (size_t)in->dst,
                        map->value_count + (size_t)in->imm);
    if (in->op == NY_NIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
        (size_t)in->a < map->value_count && (size_t)in->imm < local_count)
      ny_nir_type_union(parents, (size_t)in->a,
                        map->value_count + (size_t)in->imm);
  }

  for (size_t i = 0; i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    bool f64_result = ny_nir_op_f64(in->op) ||
        (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F64));
    bool f32_result = ny_nir_op_f32(in->op) ||
        (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F32));
    if (in->dst >= 0 && (size_t)in->dst < map->value_count) {
      size_t root = ny_nir_type_root(parents, (size_t)in->dst);
      if (f64_result)
        root_types[root] |= 1u;
      if (f32_result)
        root_types[root] |= 2u;
    }
    bool f64_operands = in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
                        in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64 ||
                        in->op == NYIR_CMP_F64;
    bool f32_operands = in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
                        in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32 ||
                        in->op == NYIR_CMP_F32;
    if (in->a >= 0 && (size_t)in->a < map->value_count) {
      size_t root = ny_nir_type_root(parents, (size_t)in->a);
      if (f64_operands)
        root_types[root] |= 1u;
      if (f32_operands)
        root_types[root] |= 2u;
    }
    if (in->b >= 0 && (size_t)in->b < map->value_count) {
      size_t root = ny_nir_type_root(parents, (size_t)in->b);
      if (f64_operands)
        root_types[root] |= 1u;
      if (f32_operands)
        root_types[root] |= 2u;
    }
  }

  for (size_t i = 0; i < map->value_count; ++i) {
    unsigned char type = root_types[ny_nir_type_root(parents, i)];
    map->value_f64[i] = (type & 1u) != 0;
    map->value_f32[i] = (type & 2u) != 0;
  }
  for (size_t i = 0; i < local_count; ++i) {
    unsigned char type =
        root_types[ny_nir_type_root(parents, map->value_count + i)];
    map->local_f64[i] = (type & 1u) != 0;
    map->local_f32[i] = (type & 2u) != 0;
  }
  free(parents);
  free(root_types);
  return true;
}

int64_t ny_nir_f64_to_bits(double v) {
  int64_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

double ny_nir_bits_to_f64(int64_t bits) {
  double v = 0;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

int64_t ny_nir_f32_to_bits(float v) {
  int32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return (int64_t)(uint32_t)bits;
}

float ny_nir_bits_to_f32(int64_t bits) {
  int32_t b32 = (int32_t)(uint32_t)bits;
  float v = 0;
  memcpy(&v, &b32, sizeof(v));
  return v;
}

static const char *ny_nir_own_symbol_copy(ny_nir_func_t *f,
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

void ny_nir_func_free(ny_nir_func_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->owned_symbols_len; ++i)
    free(f->owned_symbols[i]);
  free(f->owned_symbols);
  for (size_t i = 0; i < f->len; ++i) {
    free(f->data[i].extra_args);
    free(f->data[i].arg_sizes);
  }
  free(f->data);
  memset(f, 0, sizeof(*f));
}

void ny_nir_inst_discard(ny_nir_inst_t *in) {
  if (!in)
    return;
  free(in->extra_args);
  free(in->arg_sizes);
  *in = (ny_nir_inst_t){.op = NY_NIR_NOP,
                        .dst = -1,
                        .a = -1,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1};
}

const char *ny_nir_op_name(ny_nir_op_t op) {
  switch (op) {
  case NY_NIR_NOP:
    return "nop";
  case NY_NIR_CONST_I64:
    return "const.i64";
  case NY_NIR_COPY:
    return "copy";
  case NY_NIR_ADD_I64:
    return "add.i64";
  case NY_NIR_SUB_I64:
    return "sub.i64";
  case NY_NIR_MUL_I64:
    return "mul.i64";
  case NY_NIR_DIV_I64:
    return "div.i64";
  case NY_NIR_MOD_I64:
    return "mod.i64";
  case NY_NIR_AND_I64:
    return "and.i64";
  case NY_NIR_OR_I64:
    return "or.i64";
  case NY_NIR_XOR_I64:
    return "xor.i64";
  case NY_NIR_SHL_I64:
    return "shl.i64";
  case NY_NIR_SAR_I64:
    return "sar.i64";
  case NY_NIR_CMP_I64:
    return "cmp.i64";
  case NY_NIR_LABEL:
    return "label";
  case NY_NIR_LOAD_LOCAL:
    return "load.local";
  case NY_NIR_STORE_LOCAL:
    return "store.local";
  case NY_NIR_CALL:
    return "call";
  case NY_NIR_RET:
    return "ret";
  case NY_NIR_BR:
    return "br";
  case NY_NIR_BR_IF:
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
  case NYIR_OP_COUNT:
    break;
  }
  return "unknown";
}

unsigned ny_nir_inst_effects(const ny_nir_inst_t *inst) {
  if (!inst)
    return NY_NIR_EFFECT_NONE;
  switch (inst->op) {
  case NY_NIR_LOAD_LOCAL:
    return NY_NIR_EFFECT_READ_LOCAL;
  case NY_NIR_STORE_LOCAL:
    return NY_NIR_EFFECT_WRITE_LOCAL;
  case NYIR_LOAD_I64:
    return NY_NIR_EFFECT_CALL;
  case NYIR_STORE_I64:
    return NY_NIR_EFFECT_CALL | NY_NIR_EFFECT_WRITE_LOCAL;
  case NY_NIR_CALL:
    return NY_NIR_EFFECT_CALL;
  case NY_NIR_RET:
  case NY_NIR_BR:
  case NY_NIR_BR_IF:
    return NY_NIR_EFFECT_CONTROL;
  default:
    return NY_NIR_EFFECT_NONE;
  }
}

static void ny_nir_init_inst_metadata(ny_nir_inst_t *inst) {
  if (!inst)
    return;
  inst->effects = ny_nir_inst_effects(inst);
  if (inst->op == NY_NIR_CONST_I64 && !inst->range.has_min &&
      !inst->range.has_max) {
    inst->range.has_min = true;
    inst->range.has_max = true;
    inst->range.min = inst->imm;
    inst->range.max = inst->imm;
  } else if ((inst->op == NY_NIR_CMP_I64 || inst->op == NYIR_CMP_F64 ||
              inst->op == NYIR_CMP_F32) &&
             !inst->range.has_min &&
             !inst->range.has_max) {
    inst->range.has_min = true;
    inst->range.has_max = true;
    inst->range.min = 0;
    inst->range.max = 1;
  }
}

static void ny_nir_normalize_operands(ny_nir_inst_t *inst) {
  if (!inst)
    return;
  switch (inst->op) {
  case NY_NIR_COPY:
  case NYIR_I64_TO_F64:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_LOAD_I64:
  case NY_NIR_RET:
  case NY_NIR_BR_IF:
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
  case NY_NIR_STORE_LOCAL:
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
  case NY_NIR_CALL:
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
  case NY_NIR_ADD_I64:
  case NY_NIR_SUB_I64:
  case NY_NIR_MUL_I64:
  case NY_NIR_DIV_I64:
  case NY_NIR_MOD_I64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NY_NIR_AND_I64:
  case NY_NIR_OR_I64:
  case NY_NIR_XOR_I64:
  case NY_NIR_SHL_I64:
  case NY_NIR_SAR_I64:
  case NY_NIR_CMP_I64:
  case NYIR_CMP_F64:
  case NYIR_CMP_F32:
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NY_NIR_NOP:
  case NY_NIR_CONST_I64:
  case NYIR_CONST_F64:
  case NYIR_CONST_F32:
  case NY_NIR_LABEL:
  case NY_NIR_LOAD_LOCAL:
  case NY_NIR_BR:
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

void ny_nir_refresh_metadata(ny_nir_func_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    ny_nir_normalize_operands(in);
    in->effects = ny_nir_inst_effects(in);
    if (in->op == NY_NIR_CONST_I64) {
      in->range = (ny_nir_range_t){.has_min = true,
                                   .has_max = true,
                                   .min = in->imm,
                                   .max = in->imm};
    } else if (in->op == NY_NIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
               in->op == NYIR_CMP_F32) {
      in->range = (ny_nir_range_t){.has_min = true,
                                   .has_max = true,
                                   .min = 0,
                                   .max = 1};
    }
  }
}

int ny_nir_emit(ny_nir_func_t *f, ny_nir_inst_t inst) {
  if (!f)
    return -1;
  ny_nir_normalize_operands(&inst);
  if (f->len >= f->cap) {
    size_t cap = f->cap ? f->cap * 2 : 64;
    ny_nir_inst_t *data = (ny_nir_inst_t *)realloc(f->data, cap * sizeof(*data));
    if (!data)
      return -1;
    f->data = data;
    f->cap = cap;
  }
  if (inst.symbol) {
    inst.symbol = ny_nir_own_symbol_copy(f, inst.symbol);
    if (!inst.symbol)
      return -1;
  }
  if (inst.dst < 0 && inst.op != NY_NIR_STORE_LOCAL && inst.op != NY_NIR_RET &&
      inst.op != NY_NIR_BR && inst.op != NY_NIR_BR_IF &&
      inst.op != NY_NIR_LABEL && inst.op != NY_NIR_NOP)
    inst.dst = f->next_value++;
  ny_nir_init_inst_metadata(&inst);
  f->data[f->len++] = inst;
  return inst.dst;
}

bool ny_nir_err(char *err, size_t err_len, const char *fmt, ...) {
  if (err && err_len > 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
  }
  return false;
}

bool ny_nir_inst_err(char *err, size_t err_len, const ny_nir_inst_t *in,
                     size_t index, const char *reason) {
  return ny_nir_err(err, err_len,
                 "native NYIR verify: inst %zu opcode=%s dst=v%d a=v%d b=v%d "
                 "imm=%" PRId64 ": %s",
                 index, in ? ny_nir_op_name(in->op) : "<null>",
                 in ? in->dst : -1, in ? in->a : -1, in ? in->b : -1,
                 in ? in->imm : 0, reason ? reason : "invalid instruction");
}

void ny_nir_dump(FILE *out, const ny_nir_func_t *f, const char *name) {
  if (!out)
    out = stderr;
  fprintf(out, "nyir function %s values=%d insts=%zu\n",
          name && name[0] ? name : "<anon>", f ? f->next_value : 0,
          f ? f->len : 0);
  if (!f)
    return;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    fprintf(out, "  %04zu: ", i);
    if (in->dst >= 0)
      fprintf(out, "v%d = ", in->dst);
    fprintf(out, "%s", ny_nir_op_name(in->op));
    if (in->op == NY_NIR_CONST_I64)
      fprintf(out, " %" PRId64, in->imm);
    else if (in->op == NYIR_CONST_F64)
      fprintf(out, " %.17g", ny_nir_bits_to_f64(in->imm));
    else if (in->op == NYIR_CONST_F32)
      fprintf(out, " %.9g", (double)ny_nir_bits_to_f32(in->imm));
    else if (in->op == NY_NIR_LABEL)
      fprintf(out, " L%" PRId64, in->imm);
    else if (in->op == NY_NIR_COPY || in->op == NYIR_I64_TO_F64 ||
             in->op == NYIR_I64_TO_F32 || in->op == NYIR_F64_TO_F32 ||
             in->op == NYIR_F32_TO_F64) {
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
    } else if (in->op == NYIR_ADDR_SYMBOL) {
      fprintf(out, " %s", in->symbol ? in->symbol : "<null>");
    } else if (in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL ||
               in->op == NYIR_ADDR_LOCAL) {
      fprintf(out, " local#%" PRId64, in->imm);
      if (in->symbol)
        fprintf(out, "(%s)", in->symbol);
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
    } else if (in->op == NY_NIR_CALL) {
      fprintf(out, " %s argc=%" PRId64, in->symbol ? in->symbol : "<null>",
              in->imm);
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
      if (in->b >= 0)
        fprintf(out, " v%d", in->b);
      if (in->c >= 0)
        fprintf(out, " v%d", in->c);
      if (in->d >= 0)
        fprintf(out, " v%d", in->d);
      if (in->e >= 0)
        fprintf(out, " v%d", in->e);
      if (in->f >= 0)
        fprintf(out, " v%d", in->f);
      for (size_t k = 0; k < in->extra_args_len; ++k)
        fprintf(out, " v%d", in->extra_args[k]);
    } else if (in->op == NY_NIR_BR || in->op == NY_NIR_BR_IF) {
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
      fprintf(out, " L%" PRId64, in->imm);
    } else {
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
      if (in->b >= 0)
        fprintf(out, " v%d", in->b);
      if (in->imm)
        fprintf(out, " imm=%" PRId64, in->imm);
    }
    if (in->effects)
      fprintf(out, " effects=0x%x", in->effects);
    if (in->range.has_min || in->range.has_max) {
      fprintf(out, " range=");
      if (in->range.has_min)
        fprintf(out, "%" PRId64, in->range.min);
      else
        fputc('*', out);
      fputs("..", out);
      if (in->range.has_max)
        fprintf(out, "%" PRId64, in->range.max);
      else
        fputc('*', out);
    }
    if (in->debug.line) {
      fprintf(out, " loc=");
      if (in->debug.file && in->debug.file[0])
        fprintf(out, "%s:", in->debug.file);
      fprintf(out, "%u:%u", in->debug.line, in->debug.column);
    }
    fprintf(out, "\n");
  }
}

void ny_nir_eval_result_dump(FILE *out, const char *name,
                             const ny_nir_eval_result_t *result) {
  if (!out)
    out = stderr;
  fprintf(out,
          "nyir vm profile function=%s returned=%s result=%" PRId64 " steps=%zu branches_taken=%zu branches_not_taken=%zu calls=%zu max_pc=%zu max_value=%zu max_local=%zu\n",
          name && name[0] ? name : "rt_main",
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
  for (size_t i = 0; i < (size_t)NYIR_OP_COUNT; ++i) {
    if (result->op_counts[i] == 0)
      continue;
    fprintf(out, "  op %-14s %zu\n", ny_nir_op_name((ny_nir_op_t)i),
            result->op_counts[i]);
  }
}

void ny_nir_collect_stats(const ny_nir_func_t *f, size_t *insts,
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

void ny_nir_dump_stats(FILE *out, const ny_nir_opt_stats_t *stats) {
  if (!out)
    out = stderr;
  if (!stats)
    return;
  size_t removed = stats->before_insts > stats->after_insts
                       ? stats->before_insts - stats->after_insts
                       : 0;
  fprintf(out,
          "nyir optimize before_insts=%zu after_insts=%zu removed=%zu "
          "before_values=%d after_values=%d\n",
          stats->before_insts, stats->after_insts, removed,
          stats->before_values, stats->after_values);
  fputs("nyir optimize ops", out);
  for (size_t op = 0; op < (size_t)NYIR_OP_COUNT; ++op) {
    size_t before = stats->before_ops[op];
    size_t after = stats->after_ops[op];
    if (!before && !after)
      continue;
    fprintf(out, " %s:%zu->%zu", ny_nir_op_name((ny_nir_op_t)op), before,
            after);
  }
  fputc('\n', out);
}
