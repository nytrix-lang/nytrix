/*
 * Loop vectorization: widens scalar loop operations into SIMD
 * vector operations when memory-access patterns are stride-1 aligned.
 *
 * VF selection considers VF=4 (VEC8, 4×i64=256-bit) and VF=2
 * (VEC4, 2×i64=128-bit) when both are legal, using setup/tail/pressure
 * estimates rather than unconditionally preferring the widest form.
 * Variable-trip loops use a scalar tail, so bounds
 * smaller than VF remain correct without speculative memory accesses.
 * VEC8_I64 opcodes are fully wired in the IR/verifier/type-map,
 * machine form, and x86-64 object encoder (native ymm AVX2 path).
 *
 * Candidate memory roots must be private stack objects or compile-time-known
 * allocator results. Alias checks reject shifted accesses to the same root,
 * side effects, and escaping scalar values. Constant-trip reductions are
 * widened through reduction PHIs; variable-trip reductions remain scalar
 * until a guarded reduction lowering exists.
 *
 * See also: slp_vectorize.c (bottom-up SLP), ivelim.c (IV elimination).
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nyr_vector_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
         ny_trace_enabled("NY_TRACE_VECTORIZE");
}

static void nyr_vector_trace(const nyr_func_t *f, size_t pc,
                              const char *status, const char *reason, int vf) {
  const nyr_inst_t *in = f && pc < f->len ? &f->data[pc] : NULL;
  const nyr_inst_t *site = in;
  if (f && in && (!in->debug.file || in->debug.line == 0)) {
    for (size_t i = pc + 1; i < f->len && f->data[i].op != NYIR_LABEL; ++i) {
      if (f->data[i].debug.file && f->data[i].debug.line != 0) {
        site = &f->data[i];
        break;
      }
    }
  }
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event(
        "LOOP_VECTORIZE", "%s pc=%zu op=%s vf=%d at %s:%u:%u reason=%s",
        status ? status : "remark", pc, in ? nyr_op_name(in->op) : "<none>",
        vf, site && site->debug.file ? site->debug.file : "<unknown>",
        site ? site->debug.line : 0, site ? site->debug.column : 0,
        reason ? reason : "none");
  if (!nyr_vector_trace_enabled())
    return;
  const char *file = site && site->debug.file ? site->debug.file : "<unknown>";
  unsigned line = site ? site->debug.line : 0;
  unsigned column = site ? site->debug.column : 0;
  ny_trace_line("VECTORIZE",
                "nyir vectorize: %s pc=%zu op=%s vf=%d at %s:%u:%u reason=%s",
                status ? status : "remark", pc,
                in ? nyr_op_name(in->op) : "<none>", vf, file, line, column,
                reason ? reason : "none");
}

/*
 * Resolve a value through MOV/COPY chains to find its root definition.
 * Returns the root value, or -1 if not a simple MOV/COPY chain.
 */
static int nyr_vectorize_resolve_value(const nyr_func_t *f, const int *defs,
                                        int value) {
  int current = value;
  for (int depth = 0; depth < 64; ++depth) {
    if (current < 0 || current >= f->next_value)
      break;
    int def_idx = defs[current];
    if (def_idx < 0 || def_idx >= (int)f->len)
      break;
    const nyr_inst_t *def = &f->data[def_idx];
    if (def->op != NYIR_COPY)
      break;
    current = def->a;
  }
  return current;
}

static bool nyr_vectorize_is_typed_buffer(const nyr_func_t *f,
                                           const int *defs, int value) {
  value = nyr_vectorize_resolve_value(f, defs, value);
  if (!f || !defs || value < 0 || value >= f->next_value || defs[value] < 0 ||
      (size_t)defs[value] >= f->len)
    return false;
  return f->data[defs[value]].scalar_class == NYIR_SCALAR_TYPED_BUFFER;
}

/*
 * V128/V256 i64 loop vectorization.
 *
 * VEC4_I64 opcodes represent 128-bit vectors (2×i64 lanes).
 * VEC8_I64 opcodes represent 256-bit vectors (4×i64 lanes).
 * Candidate loops have stride-1 induction, 8-byte affine accesses, and no
 * side effects.  Constant-trip loops use a vector body for the largest
 * aligned prefix and a scalar epilogue for the remaining iterations.
 * Variable-trip loops use a runtime peel:
 * the vector loop processes `init` to `vec_end = init + ((limit-init) &
 * ~(VF-1))`, then a scalar tail covers `vec_end` to `limit` at stride 1.
 * The root must be a private stack object or a compile-time-known allocator
 * call; unknown heap aliases remain scalar.
 */

#define NYIR_I64_VF_MIN 2
#define NYIR_I64_VF_MAX 4

/*
 * Most control-flow-heavy functions contain only forward branches. Prove
 * that case with a linear scan so we can avoid constructing the full CFG and
 * def map just to discover there are no loops. Label IDs must increase in
 * lexical order; unusual IR falls back to the full analysis.
 */
static bool nyr_vectorizer_no_lexical_backedge(const nyr_func_t *f) {
  int64_t first = -1, previous = -1;
  bool saw_branch = false, forward_only = true;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL) {
      if (in->imm < 0)
        return false;
      if (first < 0)
        first = in->imm;
      else if (in->imm <= previous)
        return false;
      previous = in->imm;
    } else if (in->op == NYIR_BR) {
      saw_branch = true;
      if (in->imm < 0 || (previous >= 0 && in->imm <= previous))
        forward_only = false;
    }
  }
  if (!saw_branch || first < 0)
    return !saw_branch;
  return forward_only;
}

typedef struct {
  bool ok;
  int root;
  int64_t offset;
  int64_t stride;
} affine_t;

static bool op_is_vec_i64_bin(nyr_op_t op) {
  return op == NYIR_ADD_I64 || op == NYIR_SUB_I64 || op == NYIR_AND_I64 ||
         op == NYIR_OR_I64 || op == NYIR_XOR_I64;
}

static bool op_is_vec_f64_bin(nyr_op_t op) {
  return op == NYIR_ADD_F64 || op == NYIR_SUB_F64 || op == NYIR_MUL_F64 ||
         op == NYIR_DIV_F64;
}

static nyr_op_t vec_i64_bin(nyr_op_t op, int vf) {
  if (vf == 4) {
    switch (op) {
    case NYIR_ADD_I64:
      return NYIR_VEC8_ADD_I64;
    case NYIR_SUB_I64:
      return NYIR_VEC8_SUB_I64;
    case NYIR_AND_I64:
      return NYIR_VEC8_AND_I64;
    case NYIR_OR_I64:
      return NYIR_VEC8_OR_I64;
    case NYIR_XOR_I64:
      return NYIR_VEC8_XOR_I64;
    default:
      return NYIR_NOP;
    }
  }
  switch (op) {
  case NYIR_ADD_I64:
    return NYIR_VEC4_ADD_I64;
  case NYIR_SUB_I64:
    return NYIR_VEC4_SUB_I64;
  case NYIR_AND_I64:
    return NYIR_VEC4_AND_I64;
  case NYIR_OR_I64:
    return NYIR_VEC4_OR_I64;
  case NYIR_XOR_I64:
    return NYIR_VEC4_XOR_I64;
  default:
    return NYIR_NOP;
  }
}

static nyr_op_t vec_f64_bin(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_F64:
    return NYIR_VEC4_ADD_F64;
  case NYIR_SUB_F64:
    return NYIR_VEC4_SUB_F64;
  case NYIR_MUL_F64:
    return NYIR_VEC4_MUL_F64;
  case NYIR_DIV_F64:
    return NYIR_VEC4_DIV_F64;
  default:
    return NYIR_NOP;
  }
}

static bool tbuf_alloc_bytes(const nyr_func_t *f, const int *defs, int value,
                             int64_t *bytes) {
  if (!f || !defs || value < 0 || value >= f->next_value || !bytes ||
      defs[value] < 0)
    return false;
  const nyr_inst_t *def = &f->data[(size_t)defs[value]];
  if (def->op != NYIR_CALL || !def->symbol ||
      (strcmp(def->symbol, "rt_tbuf_new_raw") != 0 &&
       strcmp(def->symbol, "rt_tbuf_new_typed_raw") != 0))
    return false;
  int args[2] = {-1, -1};
  int argc = 0;
  int64_t count = 0, elem = 0;
  if (!nyr_call_args(def, f->next_value, args, 2, &argc, NULL, 0) ||
      argc != 2 || !nyr_const_i64(f, defs, args[0], &count) ||
      !nyr_const_i64(f, defs, args[1], &elem) || count <= 0 || elem <= 0 ||
      count > INT64_MAX / elem)
    return false;
  *bytes = count * elem;
  return true;
}

/*
 * A loop may exchange two private buffers at the latch:
 *
 *   a = phi(initial_a, b)
 *   b = phi(initial_b, a)
 *
 * If the initial values are distinct allocator calls on the same incoming
 * edge, the two PHIs remain disjoint for every iteration.  This relational
 * proof is deliberately exact; independent may-alias PHIs are not accepted.
 */
static bool tbuf_swap_phi_pair(const nyr_func_t *f, const int *defs,
                               int a_value, int b_value, int64_t *bytes_out) {
  if (!f || !defs || a_value < 0 || b_value < 0 || a_value >= f->next_value ||
      b_value >= f->next_value || a_value == b_value || defs[a_value] < 0 ||
      defs[b_value] < 0)
    return false;
  const nyr_inst_t *a = &f->data[(size_t)defs[a_value]];
  const nyr_inst_t *b = &f->data[(size_t)defs[b_value]];
  if (a->op != NYIR_PHI || b->op != NYIR_PHI || a->phi_incoming_len != 2 ||
      b->phi_incoming_len != 2)
    return false;
  int64_t a_init_label = -1, b_init_label = -1;
  int64_t a_latch_label = -1, b_latch_label = -1;
  int a_init = -1, b_init = -1;
  for (size_t i = 0; i < 2; ++i) {
    int v = nyr_vectorize_resolve_value(f, defs, a->phi_incoming[i].value);
    if (v == b_value)
      a_latch_label = a->phi_incoming[i].predecessor_label;
    else {
      a_init = v;
      a_init_label = a->phi_incoming[i].predecessor_label;
    }
  }
  for (size_t i = 0; i < 2; ++i) {
    int v = nyr_vectorize_resolve_value(f, defs, b->phi_incoming[i].value);
    if (v == a_value)
      b_latch_label = b->phi_incoming[i].predecessor_label;
    else {
      b_init = v;
      b_init_label = b->phi_incoming[i].predecessor_label;
    }
  }
  int64_t a_bytes = 0, b_bytes = 0;
  if (a_init < 0 || b_init < 0 || a_init == b_init || a_latch_label < 0 ||
      a_latch_label != b_latch_label || a_init_label < 0 ||
      a_init_label != b_init_label || a_init_label == a_latch_label ||
      !tbuf_alloc_bytes(f, defs, a_init, &a_bytes) ||
      !tbuf_alloc_bytes(f, defs, b_init, &b_bytes) || a_bytes != b_bytes)
    return false;
  if (bytes_out)
    *bytes_out = a_bytes;
  return true;
}

static bool tbuf_phi_bytes(const nyr_func_t *f, const int *defs, int value,
                           int64_t *bytes) {
  if (!f || !defs || value < 0 || value >= f->next_value || defs[value] < 0 ||
      !bytes)
    return false;
  const nyr_inst_t *def = &f->data[(size_t)defs[value]];
  if (def->op != NYIR_PHI || def->phi_incoming_len != 2)
    return false;
  for (size_t i = 0; i < 2; ++i) {
    int other =
        nyr_vectorize_resolve_value(f, defs, def->phi_incoming[i].value);
    if (other >= 0 && other < f->next_value && other != value &&
        defs[other] >= 0 && f->data[(size_t)defs[other]].op == NYIR_PHI &&
        tbuf_swap_phi_pair(f, defs, value, other, bytes))
      return true;
  }
  return false;
}

static bool affine_add(affine_t a, affine_t b, bool subtract, affine_t *out) {
  if (!a.ok || !b.ok || !out)
    return false;
  if (subtract && b.root >= 0)
    return false;
  if (a.root >= 0 && b.root >= 0)
    return false;
  __int128 off =
      (__int128)a.offset + (subtract ? -(__int128)b.offset : b.offset);
  __int128 stride =
      (__int128)a.stride + (subtract ? -(__int128)b.stride : b.stride);
  if (off < INT64_MIN || off > INT64_MAX || stride < INT64_MIN ||
      stride > INT64_MAX)
    return false;
  *out = (affine_t){.ok = true,
                    .root = a.root >= 0 ? a.root : b.root,
                    .offset = (int64_t)off,
                    .stride = (int64_t)stride};
  return true;
}

static bool value_defined_in_loop(const nyr_func_t *f, const int *defs,
                                  const nyr_cfg_t *cfg, const bool *in_loop,
                                  int value) {
  if (!f || !defs || !cfg || !in_loop || value < 0 || value >= f->next_value)
    return false;
  int di = defs[value];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  size_t block = cfg->inst_block[(size_t)di];
  return block < cfg->block_count && in_loop[block];
}

static affine_t affine_value(const nyr_func_t *f, const int *defs,
                             const nyr_cfg_t *cfg, const bool *in_loop,
                             int value, int iv, unsigned depth) {
  affine_t bad = {0};
  if (!f || !defs || value < 0 || value >= f->next_value || depth > 24)
    return bad;
  if (value == iv)
    return (affine_t){.ok = true, .root = -1, .offset = 0, .stride = 1};
  int di = defs[value];
  if (di < 0)
    return (affine_t){.ok = true, .root = value, .offset = 0, .stride = 0};
  const nyr_inst_t *in = &f->data[(size_t)di];
  if (in->op == NYIR_CONST_I64)
    return (affine_t){.ok = true, .root = -1, .offset = in->imm, .stride = 0};
  if (in->op == NYIR_ALLOCA || in->op == NYIR_ADDR_SYMBOL ||
      in->op == NYIR_ADDR_LOCAL)
    return (affine_t){.ok = true, .root = value, .offset = 0, .stride = 0};
  if (in->op == NYIR_PHI && in->phi_incoming_len == 2) {
    int pre = -1;
    int back = -1;
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int incoming = in->phi_incoming[k].value;
      if (incoming < 0 || incoming >= f->next_value)
        continue;
      int incoming_def = defs[incoming];
      if (incoming_def < 0 ||
          !value_defined_in_loop(f, defs, cfg, in_loop, incoming))
        pre = incoming;
      else
        back = incoming;
    }
    if (pre >= 0 && back >= 0 && defs[back] >= 0) {
      const nyr_inst_t *update = &f->data[(size_t)defs[back]];
      if (update->op == NYIR_ADD_I64) {
        int64_t init_value = 0;
        int64_t step_value = 0;
        int step_operand = -1;
        if (update->a == value)
          step_operand = update->b;
        else if (update->b == value)
          step_operand = update->a;
        if (step_operand >= 0 && nyr_const_i64(f, defs, pre, &init_value) &&
            nyr_const_i64(f, defs, step_operand, &step_value))
          return (affine_t){.ok = true,
                            .root = -1,
                            .offset = init_value,
                            .stride = step_value};
      }
    }
  }
  if (!value_defined_in_loop(f, defs, cfg, in_loop, value))
    return (affine_t){.ok = true, .root = value, .offset = 0, .stride = 0};

  if (in->op == NYIR_COPY)
    return affine_value(f, defs, cfg, in_loop, in->a, iv, depth + 1);
  if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) {
    affine_t a = affine_value(f, defs, cfg, in_loop, in->a, iv, depth + 1);
    affine_t b = affine_value(f, defs, cfg, in_loop, in->b, iv, depth + 1);
    affine_t out = {0};
    if (affine_add(a, b, in->op == NYIR_SUB_I64, &out))
      return out;
    return bad;
  }
  if (in->op == NYIR_MUL_I64) {
    int64_t k = 0;
    int other = -1;
    if (nyr_const_i64(f, defs, in->a, &k))
      other = in->b;
    else if (nyr_const_i64(f, defs, in->b, &k))
      other = in->a;
    else
      return bad;
    affine_t a = affine_value(f, defs, cfg, in_loop, other, iv, depth + 1);
    if (!a.ok || a.root >= 0 || (__int128)a.offset * k < INT64_MIN ||
        (__int128)a.offset * k > INT64_MAX ||
        (__int128)a.stride * k < INT64_MIN ||
        (__int128)a.stride * k > INT64_MAX)
      return bad;
    a.offset *= k;
    a.stride *= k;
    return a;
  }
  if (in->op == NYIR_SHL_I64) {
    int64_t shift = 0;
    if (!nyr_const_i64(f, defs, in->b, &shift) || shift < 0 || shift > 62)
      return bad;
    affine_t a = affine_value(f, defs, cfg, in_loop, in->a, iv, depth + 1);
    if (!a.ok || a.root >= 0)
      return bad;
    __int128 scale = (__int128)1 << shift;
    if ((__int128)a.offset * scale < INT64_MIN ||
        (__int128)a.offset * scale > INT64_MAX ||
        (__int128)a.stride * scale < INT64_MIN ||
        (__int128)a.stride * scale > INT64_MAX)
      return bad;
    a.offset *= (int64_t)scale;
    a.stride *= (int64_t)scale;
    return a;
  }
  return bad;
}

static bool known_alloc_bytes(const nyr_func_t *f, const int *defs, int root,
                              int64_t *out);

static bool root_is_private_memory(const nyr_func_t *f, const int *defs,
                                   int root) {
  if (!f || !defs || root < 0 || root >= f->next_value || defs[root] < 0)
    return false;
  const nyr_inst_t *def = &f->data[(size_t)defs[root]];
  if (def->op == NYIR_ALLOCA || def->op == NYIR_ADDR_LOCAL)
    return true;
  int64_t bytes = 0;
  if (def->op == NYIR_PHI)
    return tbuf_phi_bytes(f, defs, root, &bytes);
  if (def->op != NYIR_CALL || !def->symbol)
    return false;
  if (strcmp(def->symbol, "rt_tbuf_new_raw") == 0 ||
      strcmp(def->symbol, "rt_tbuf_new_typed_raw") == 0)
    return true;
  ny_builtin_alloc_kind_t kind = ny_builtin_alloc_kind(def->symbol);
  return kind == NY_BUILTIN_ALLOC_MALLOC || kind == NY_BUILTIN_ALLOC_CALLOC;
}

static bool known_alloc_bytes(const nyr_func_t *f, const int *defs, int root,
                              int64_t *out) {
  const nyr_inst_t *def = &f->data[(size_t)defs[root]];
  if (def->op == NYIR_PHI && tbuf_phi_bytes(f, defs, root, out))
    return true;
  if (def->op == NYIR_ALLOCA) {
    if (def->imm <= 0)
      return false;
    *out = def->imm;
    return true;
  }
  if (def->op != NYIR_CALL || !def->symbol)
    return false;
  if (strcmp(def->symbol, "rt_tbuf_new_raw") == 0 ||
      strcmp(def->symbol, "rt_tbuf_new_typed_raw") == 0)
    return tbuf_alloc_bytes(f, defs, root, out);
  ny_builtin_alloc_kind_t kind = ny_builtin_alloc_kind(def->symbol);
  if (kind != NY_BUILTIN_ALLOC_MALLOC && kind != NY_BUILTIN_ALLOC_CALLOC)
    return false;
  int args[2] = {-1, -1};
  int argc = 0;
  if (!nyr_call_args(def, f->next_value, args, 2, &argc, NULL, 0) ||
      (kind == NY_BUILTIN_ALLOC_MALLOC ? argc != 1 : argc != 2))
    return false;
  int64_t a = 0;
  int64_t b = 1;
  if (!nyr_const_i64(f, defs, args[0], &a) || a <= 0)
    return false;
  if (kind == NY_BUILTIN_ALLOC_CALLOC &&
      (!nyr_const_i64(f, defs, args[1], &b) || b <= 0))
    return false;
  if (b > INT64_MAX / a)
    return false;
  *out = a * b;
  return true;
}

/*
 * Native tbuf indexing is lowered as a payload pointer plus a byte offset.
 * The offset can include an outer-loop row term, so treating every
 * loop-invariant value as a competing pointer root rejects matrix kernels.
 * Keep provenance and scalar offset analysis separate: only a direct tbuf
 * payload (through copies) may supply the base, while the other addend must
 * still be an affine byte offset for this loop.
 */
typedef struct {
  bool ok;
  bool dynamic;
  bool has_range;
  int64_t offset;
  int64_t stride;
  int64_t min;
  int64_t max;
} tbuf_offset_t;

static int tbuf_payload_root(const nyr_func_t *f, const int *defs, int value,
                             unsigned depth) {
  if (!f || !defs || value < 0 || value >= f->next_value || depth > 16 ||
      defs[value] < 0)
    return -1;
  const nyr_inst_t *in = &f->data[(size_t)defs[value]];
  if (in->op == NYIR_CALL && in->symbol &&
      (strcmp(in->symbol, "rt_tbuf_new_raw") == 0 ||
       strcmp(in->symbol, "rt_tbuf_new_typed_raw") == 0))
    return value;
  if (in->op == NYIR_COPY)
    return tbuf_payload_root(f, defs, in->a, depth + 1);
  int64_t bytes = 0;
  if (in->op == NYIR_PHI && tbuf_phi_bytes(f, defs, value, &bytes))
    return value;
  return -1;
}

static tbuf_offset_t tbuf_offset_value(const nyr_func_t *f, const int *defs,
                                       const nyr_cfg_t *cfg,
                                       const bool *in_loop, int value, int iv,
                                       unsigned depth) {
  tbuf_offset_t bad = {0};
  if (!f || !defs || !cfg || !in_loop || value < 0 || value >= f->next_value ||
      depth > 24 || defs[value] < 0)
    return bad;
  if (value == iv)
    return (tbuf_offset_t){.ok = true, .stride = 1};
  const nyr_inst_t *in = &f->data[(size_t)defs[value]];
  if (in->op == NYIR_CONST_I64)
    return (tbuf_offset_t){.ok = true, .offset = in->imm};
  if (in->op == NYIR_COPY)
    return tbuf_offset_value(f, defs, cfg, in_loop, in->a, iv, depth + 1);
  if (!value_defined_in_loop(f, defs, cfg, in_loop, value)) {
    if (in->op == NYIR_CALL || in->op == NYIR_ALLOCA ||
        in->op == NYIR_ADDR_LOCAL || in->op == NYIR_ADDR_SYMBOL)
      return bad;
    return (tbuf_offset_t){.ok = true, .dynamic = true};
  }
  tbuf_offset_t a = {0}, b = {0}, out = {0};
  if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) {
    a = tbuf_offset_value(f, defs, cfg, in_loop, in->a, iv, depth + 1);
    b = tbuf_offset_value(f, defs, cfg, in_loop, in->b, iv, depth + 1);
    __int128 off = (__int128)a.offset +
                   (in->op == NYIR_SUB_I64 ? -(__int128)b.offset : b.offset);
    __int128 stride = (__int128)a.stride +
                      (in->op == NYIR_SUB_I64 ? -(__int128)b.stride : b.stride);
    if (!a.ok || !b.ok || off < INT64_MIN || off > INT64_MAX ||
        stride < INT64_MIN || stride > INT64_MAX)
      return bad;
    out = (tbuf_offset_t){.ok = true,
                          .dynamic = a.dynamic || b.dynamic,
                          .offset = (int64_t)off,
                          .stride = (int64_t)stride};
    return out;
  }
  int64_t scale = 0;
  int operand = -1;
  if (in->op == NYIR_MUL_I64) {
    if (nyr_const_i64(f, defs, in->a, &scale))
      operand = in->b;
    else if (nyr_const_i64(f, defs, in->b, &scale))
      operand = in->a;
  } else if (in->op == NYIR_SHL_I64 && nyr_const_i64(f, defs, in->b, &scale) &&
             scale >= 0 && scale <= 62) {
    operand = in->a;
    scale = (int64_t)(UINT64_C(1) << (unsigned)scale);
  }
  if (operand < 0)
    return bad;
  a = tbuf_offset_value(f, defs, cfg, in_loop, operand, iv, depth + 1);
  __int128 off = (__int128)a.offset * scale;
  __int128 stride = (__int128)a.stride * scale;
  if (!a.ok || off < INT64_MIN || off > INT64_MAX || stride < INT64_MIN ||
      stride > INT64_MAX)
    return bad;
  return (tbuf_offset_t){.ok = true,
                         .dynamic = a.dynamic,
                         .offset = (int64_t)off,
                         .stride = (int64_t)stride};
}

static bool value_range_under_guards(const nyr_func_t *f, const int *defs,
                                     const nyr_cfg_t *cfg, size_t use_pc,
                                     int value, int loop_iv, int64_t init,
                                     int64_t trip, unsigned depth,
                                     nyr_range_t *out);

/*
 * Split a typed-buffer byte address into its allocation root and affine
 * offset. Lowering commonly builds matrix addresses as
 *   (payload + row_bytes) + column_bytes
 * rather than one add with a direct payload operand. Peel only ADD/COPY nodes
 * that lead to a recognized private allocation; every remaining term still
 * passes the same affine/range and overflow checks as a flat address.
 */
static bool tbuf_address_parts(const nyr_func_t *f, const int *defs,
                               const nyr_cfg_t *cfg, const bool *in_loop,
                               int address, int iv, int64_t init, int64_t trip,
                               size_t use_pc, unsigned depth, int *root_out,
                               tbuf_offset_t *offset_out) {
  if (!f || !defs || !cfg || !in_loop || !root_out || !offset_out ||
      address < 0 || address >= f->next_value || depth > 16 ||
      defs[address] < 0)
    return false;
  const nyr_inst_t *in = &f->data[(size_t)defs[address]];
  if (in->op == NYIR_COPY)
    return tbuf_address_parts(f, defs, cfg, in_loop, in->a, iv, init, trip,
                              use_pc, depth + 1, root_out, offset_out);
  if (in->op != NYIR_ADD_I64)
    return false;

  int direct_root[2] = {tbuf_payload_root(f, defs, in->a, depth + 1),
                        tbuf_payload_root(f, defs, in->b, depth + 1)};
  int values[2] = {in->a, in->b};
  for (unsigned side = 0; side < 2; ++side) {
    int root = direct_root[side];
    tbuf_offset_t base = {.ok = true, .has_range = true};
    if (root < 0 &&
        !tbuf_address_parts(f, defs, cfg, in_loop, values[side], iv, init, trip,
                            use_pc, depth + 1, &root, &base))
      continue;
    int other = values[1u - side];
    tbuf_offset_t term =
        tbuf_offset_value(f, defs, cfg, in_loop, other, iv, depth + 1);
    if (term.ok && (term.dynamic || term.stride != 0)) {
      nyr_range_t range = {0};
      if (!value_range_under_guards(f, defs, cfg, use_pc, other, iv, init, trip,
                                    0, &range) &&
          defs[other] >= 0 && f->data[(size_t)defs[other]].range.has_min &&
          f->data[(size_t)defs[other]].range.has_max)
        range = f->data[(size_t)defs[other]].range;
      if (!range.has_min || !range.has_max)
        term.ok = false;
      else {
        term.has_range = true;
        term.min = range.min;
        term.max = range.max;
      }
    } else if (term.ok) {
      term.has_range = true;
      term.min = term.max = term.offset;
    }
    __int128 offset = (__int128)base.offset + term.offset;
    __int128 stride = (__int128)base.stride + term.stride;
    __int128 min = (__int128)base.min + term.min;
    __int128 max = (__int128)base.max + term.max;
    if (root < 0 || !base.ok || !term.ok || offset < INT64_MIN ||
        offset > INT64_MAX || stride < INT64_MIN || stride > INT64_MAX ||
        !base.has_range || !term.has_range || min < INT64_MIN ||
        max > INT64_MAX) {
      continue;
    }
    *root_out = root;
    *offset_out = (tbuf_offset_t){.ok = true,
                                  .dynamic = base.dynamic || term.dynamic,
                                  .has_range = true,
                                  .offset = (int64_t)offset,
                                  .stride = (int64_t)stride,
                                  .min = (int64_t)min,
                                  .max = (int64_t)max};
    return true;
  }
  return false;
}

static bool block_for_label(const nyr_cfg_t *cfg, int64_t label, size_t *out);

static bool tbuf_affine_memory_safe(const nyr_func_t *f, const int *defs,
                                    const nyr_cfg_t *cfg, const bool *in_loop,
                                    int address, int iv, int64_t init,
                                    int64_t trip, size_t use_pc, affine_t *out,
                                    bool *dynamic_offset) {
  if (!f || !defs || !cfg || !in_loop || !out || !dynamic_offset || trip <= 0 ||
      address < 0 || address >= f->next_value || defs[address] < 0)
    return false;
  int root = -1;
  tbuf_offset_t access = {0};
  bool address_ok = tbuf_address_parts(f, defs, cfg, in_loop, address, iv, init,
                                       trip, use_pc, 0, &root, &access);
  if (!address_ok || root < 0 || !access.ok || access.stride != 8) {
    if (nyr_vector_trace_enabled()) {
      const nyr_inst_t *addr = &f->data[(size_t)defs[address]];
      const nyr_inst_t *a = addr->op == NYIR_ADD_I64 && addr->a >= 0 &&
                                     addr->a < f->next_value &&
                                     defs[addr->a] >= 0
                                 ? &f->data[(size_t)defs[addr->a]]
                                 : NULL;
      const nyr_inst_t *b = addr->op == NYIR_ADD_I64 && addr->b >= 0 &&
                                     addr->b < f->next_value &&
                                     defs[addr->b] >= 0
                                 ? &f->data[(size_t)defs[addr->b]]
                                 : NULL;
      ny_trace_line("VECTORIZE",
                    "tbuf affine: missing root/address v%d root=%d offset=%lld "
                    "addends=v%d(%s)/v%d(%s)",
                    address, root, (long long)access.offset, addr->a,
                    a ? nyr_op_name(a->op) : "none", addr->b,
                    b ? nyr_op_name(b->op) : "none");
    }
    return false;
  }
  /*
   * A loop-invariant dynamic base combined with this loop's induction is a
   * common row-major address shape (payload + row_bytes + column * 8).  The
   * dynamic base is identical for every lane of one widened access because
   * the lane address is (dynamic base) + (induction + lane) * stride, and
   * dependence between distinct accesses stays governed by
   * track_root_access: a dynamic write rejects every other access on its
   * root, and cross-root independence still requires the swap-pair proof.
   * Widening may therefore keep these row/column accesses vector when the
   * bounds proof covers the full lane window; treating the accesses as
   * offset-independent without that proof previously lost native SOR
   * updates, so the stride-8 lane extension below is the only shape allowed
   * past the flat dynamic rule.
   */
  if (access.dynamic && access.stride != 0 && access.stride != 8)
    return false;
  int64_t bytes = 0;
  if (!known_alloc_bytes(f, defs, root, &bytes)) {
    if (nyr_vector_trace_enabled())
      ny_trace_line("VECTORIZE",
                    "tbuf affine: unknown allocation bytes root=v%d", root);
    return false;
  }
  /*
   * Widened lanes never extend past the scalar induction range: the vector
   * body runs lane-0 addresses only up to vec_end - VF and a scalar tail
   * covers the remainder, so every lane address (cc_vec + lane) is an
   * address the scalar loop itself could take.  The single-element bound
   * below is therefore exact for stride 0 and stride 8 alike.
   */
  if (access.dynamic) {
    nyr_range_t range = {.has_min = access.has_range,
                          .has_max = access.has_range,
                          .min = access.min,
                          .max = access.max};
    bool guarded_range = access.has_range;
    if (nyr_vector_trace_enabled())
      ny_trace_line("VECTORIZE",
                    "tbuf affine: range address=v%d guarded=%s [%lld,%lld]",
                    address, guarded_range ? "yes" : "no", (long long)range.min,
                    (long long)range.max);
    if (!range.has_min || !range.has_max || range.min < 0 ||
        range.max > bytes - 8) {
      if (nyr_vector_trace_enabled())
        ny_trace_line("VECTORIZE",
                      "tbuf affine: unsafe dynamic offset v%d "
                      "range={%s%lld,%s%lld} bytes=%lld",
                      address, range.has_min ? "" : "?", (long long)range.min,
                      range.has_max ? "" : "?", (long long)range.max,
                      (long long)bytes);
      return false;
    }
  } else {
    __int128 last =
        (__int128)access.offset + (__int128)access.stride * (trip - 1);
    if (last < 0 || last + 8 > bytes)
      return false;
  }
  *out = (affine_t){.ok = true,
                    .root = root,
                    .offset = access.offset,
                    .stride = access.stride};
  *dynamic_offset = access.dynamic;
  return true;
}

static bool affine_memory_safe(const nyr_func_t *f, const int *defs,
                               const nyr_cfg_t *cfg, const bool *in_loop,
                               int address, int iv, int64_t trip) {
  if (trip <= 0)
    return false;
  affine_t access = affine_value(f, defs, cfg, in_loop, address, iv, 0);
  if (!access.ok || access.root < 0 || access.offset < 0 || access.stride < 0)
    return false;
  int64_t bytes = 0;
  if (!known_alloc_bytes(f, defs, access.root, &bytes))
    return false;
  __int128 last =
      (__int128)access.offset + (__int128)access.stride * (trip - 1);
  return last >= 0 && last + 8 <= bytes;
}

static bool vector_memory_affine(const nyr_func_t *f, const int *defs,
                                 const nyr_cfg_t *cfg, const bool *in_loop,
                                 int address, int iv, int64_t init,
                                 bool variable_trip, int64_t trip,
                                 size_t use_pc, affine_t *out,
                                 bool *dynamic_offset) {
  affine_t access = affine_value(f, defs, cfg, in_loop, address, iv, 0);
  *dynamic_offset = false;
  if (access.ok && access.stride == 8 &&
      root_is_private_memory(f, defs, access.root) &&
      (variable_trip ||
       affine_memory_safe(f, defs, cfg, in_loop, address, iv, trip))) {
    *out = access;
    return true;
  }
  return !variable_trip &&
         tbuf_affine_memory_safe(f, defs, cfg, in_loop, address, iv, init, trip,
                                 use_pc, out, dynamic_offset);
}

/*
 * BCE may remove a typed-buffer check after proving the scalar access. In a
 * read-only loop, widening that access is safe when the checked address is an
 * invariant base plus an affine byte offset: each vector lane is one of the
 * scalar iterations already covered by the check, and no write can introduce
 * an alias dependence. Keep this fallback unavailable to stores and to loops
 * with calls or other memory effects.
 */
static bool vector_checked_read_affine(const nyr_func_t *f, const int *defs,
                                       const nyr_cfg_t *cfg,
                                       const bool *in_loop, int address, int iv,
                                       affine_t *out) {
  if (!f || !defs || !cfg || !in_loop || !out || address < 0 ||
      address >= f->next_value || defs[address] < 0)
    return false;
  const nyr_inst_t *addr = &f->data[(size_t)defs[address]];
  if (addr->op != NYIR_ADD_I64 ||
      (addr->flags & NYIR_INST_F_IN_BOUNDS_ADDR) == 0 ||
      value_defined_in_loop(f, defs, cfg, in_loop, addr->a))
    return false;
  tbuf_offset_t offset =
      tbuf_offset_value(f, defs, cfg, in_loop, addr->b, iv, 0);
  if (!offset.ok || offset.stride != 8)
    return false;
  *out = (affine_t){
      .ok = true, .root = -1, .offset = offset.offset, .stride = offset.stride};
  return true;
}

static bool value_is_scaled_loop_limit(const nyr_func_t *f, const int *defs,
                                       int value, int limit_v, int64_t scale) {
  if (!f || !defs || value < 0 || limit_v < 0 || scale <= 0)
    return false;
  value = nyr_vectorize_resolve_value(f, defs, value);
  limit_v = nyr_vectorize_resolve_value(f, defs, limit_v);
  if (value == limit_v)
    return scale == 1;
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *def = &f->data[(size_t)defs[value]];
  int64_t k = 0;
  if (def->op == NYIR_MUL_I64) {
    int other = -1;
    if (nyr_const_i64(f, defs, def->a, &k))
      other = nyr_vectorize_resolve_value(f, defs, def->b);
    else if (nyr_const_i64(f, defs, def->b, &k))
      other = nyr_vectorize_resolve_value(f, defs, def->a);
    return other == limit_v && k == scale;
  }
  if (def->op == NYIR_SHL_I64 &&
      nyr_vectorize_resolve_value(f, defs, def->a) == limit_v &&
      nyr_const_i64(f, defs, def->b, &k) && k >= 0 && k < 63) {
    uint64_t widened = UINT64_C(1) << (unsigned)k;
    return widened <= INT64_MAX && (int64_t)widened == scale;
  }
  return false;
}

/*
 * A variable-trip loop guarded by `iv < limit` can vectorize its ordinary
 * element bounds check when the check is exactly
 *
 *     iv * element_size < limit * element_size
 *
 * and the byte offset has no extra displacement.  The vector loop's end is
 * rounded down to a multiple of VF, so every lane of every vector chunk still
 * corresponds to a scalar iteration with iv < limit; the scalar tail handles
 * the remainder.  This is the common managed-tbuf indexing shape emitted by
 * lower.c for a retained list-length fact.
 */
static bool bounds_check_safe_variable(const nyr_func_t *f, const int *defs,
                                       const nyr_cfg_t *cfg,
                                       const bool *in_loop,
                                       const nyr_inst_t *check, int iv,
                                       int limit_v, int64_t init) {
  if (!check || check->op != NYIR_BOUNDS_CHECK || check->b < 0 ||
      check->c < 0 || limit_v < 0 || init < 0)
    return false;
  affine_t offset = affine_value(f, defs, cfg, in_loop, check->b, iv, 0);
  if (!offset.ok || offset.root >= 0 || offset.offset != 0 ||
      offset.stride <= 0)
    return false;
  return value_is_scaled_loop_limit(f, defs, check->c, limit_v, offset.stride);
}

static bool bounds_check_safe(const nyr_func_t *f, const int *defs,
                              const nyr_cfg_t *cfg, const bool *in_loop,
                              const nyr_inst_t *check, int iv, int64_t trip) {
  if (!check || check->op != NYIR_BOUNDS_CHECK || check->c >= 0 || trip <= 0)
    return false;
  affine_t base = affine_value(f, defs, cfg, in_loop, check->a, iv, 0);
  affine_t offset = affine_value(f, defs, cfg, in_loop, check->b, iv, 0);
  affine_t access = {0};
  if (!affine_add(base, offset, false, &access) || access.root < 0 ||
      access.offset < 0 || access.stride < 0)
    return false;
  int64_t bytes = 0;
  if (!known_alloc_bytes(f, defs, access.root, &bytes))
    return false;
  __int128 last =
      (__int128)access.offset + (__int128)access.stride * (trip - 1);
  bool ok = last >= 0 && last + 8 <= bytes;
  return ok;
}

static bool block_for_label(const nyr_cfg_t *cfg, int64_t label, size_t *out) {
  if (!cfg)
    return false;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (cfg->block_label[b] == label) {
      if (out)
        *out = b;
      return true;
    }
  }
  return false;
}

/*
 * Refine an outer induction PHI at a use dominated by its loop body edge.
 */
static bool guarded_induction_range(const nyr_func_t *f, const int *defs,
                                    const nyr_cfg_t *cfg, size_t use_pc,
                                    int value, nyr_range_t *out) {
  if (!f || !defs || !cfg || !out || use_pc >= f->len || value < 0 ||
      value >= f->next_value || defs[value] < 0)
    return false;
  size_t use_block = cfg->inst_block[use_pc];
  for (size_t h = 0; h < cfg->block_count; ++h) {
    for (size_t p = cfg->block_start[h]; p < cfg->block_end[h]; ++p) {
      const nyr_inst_t *phi = &f->data[p];
      if (phi->op != NYIR_PHI || phi->dst != value ||
          phi->phi_incoming_len < 2 ||
          phi->phi_incoming_len !=
              cfg->pred_offsets[h + 1] - cfg->pred_offsets[h])
        continue;
      int pre = -1;
      int64_t step = 0;
      size_t pre_block = SIZE_MAX;
      size_t latch_count = 0;
      bool valid = true;
      for (size_t k = 0; k < phi->phi_incoming_len && valid; ++k) {
        size_t pred = SIZE_MAX;
        if (!block_for_label(cfg, phi->phi_incoming[k].predecessor_label,
                             &pred)) {
          valid = false;
          break;
        }
        for (size_t j = 0; j < k; ++j) {
          size_t prior = SIZE_MAX;
          if (!block_for_label(cfg, phi->phi_incoming[j].predecessor_label,
                               &prior) ||
              prior == pred) {
            valid = false;
            break;
          }
        }
        if (!valid)
          break;
        if (!nyr_cfg_is_backedge(cfg, pred, h)) {
          if (pre_block != SIZE_MAX) {
            valid = false;
            break;
          }
          pre_block = pred;
          pre =
              nyr_vectorize_resolve_value(f, defs, phi->phi_incoming[k].value);
          if (pre < 0)
            valid = false;
          continue;
        }

        int back =
            nyr_vectorize_resolve_value(f, defs, phi->phi_incoming[k].value);
        if (back < 0 || back >= f->next_value || defs[back] < 0) {
          valid = false;
          break;
        }
        const nyr_inst_t *update = &f->data[(size_t)defs[back]];
        size_t update_block = cfg->inst_block[(size_t)defs[back]];
        if (update->op != NYIR_ADD_I64 ||
            !nyr_cfg_dominates(cfg, h, update_block) ||
            !nyr_cfg_dominates(cfg, update_block, pred)) {
          valid = false;
          break;
        }
        int step_v = -1;
        if (nyr_vectorize_resolve_value(f, defs, update->a) == value)
          step_v = update->b;
        else if (nyr_vectorize_resolve_value(f, defs, update->b) == value)
          step_v = update->a;
        int64_t latch_step = 0;
        if (step_v < 0 || !nyr_const_i64(f, defs, step_v, &latch_step) ||
            latch_step <= 0 || (latch_count && latch_step != step)) {
          valid = false;
          break;
        }
        step = latch_step;
        ++latch_count;
      }
      /*
       * Every incoming edge must name one distinct CFG predecessor. Accept
       * multiple backedges only when each carries the same positive update;
       * an ambiguous edge or recurrence cannot borrow the simple range.
       */
      if (!valid || pre < 0 || pre_block == SIZE_MAX || latch_count == 0)
        continue;
      bool preds_match = true;
      for (size_t e = cfg->pred_offsets[h]; e < cfg->pred_offsets[h + 1]; ++e) {
        size_t pred = cfg->pred_blocks[e];
        size_t occurrences = 0;
        size_t matches = 0;
        for (size_t prior = cfg->pred_offsets[h];
             prior < cfg->pred_offsets[h + 1]; ++prior)
          occurrences += cfg->pred_blocks[prior] == pred;
        for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
          size_t incoming = SIZE_MAX;
          if (block_for_label(cfg, phi->phi_incoming[k].predecessor_label,
                              &incoming) &&
              incoming == pred)
            ++matches;
        }
        if (occurrences != 1 || matches != 1) {
          preds_match = false;
          break;
        }
      }
      if (!preds_match || defs[pre] < 0)
        continue;
      int64_t init = 0;
      if (!nyr_const_i64(f, defs, pre, &init))
        continue;
      size_t branch = SIZE_MAX;
      for (size_t i = cfg->block_end[h]; i > cfg->block_start[h]; --i)
        if (f->data[i - 1].op == NYIR_BR_IF) {
          branch = i - 1;
          break;
        }
      if (branch == SIZE_MAX)
        continue;
      const nyr_inst_t *br = &f->data[branch];
      size_t body = SIZE_MAX;
      if (!block_for_label(cfg, br->imm, &body) ||
          !nyr_cfg_dominates(cfg, body, use_block) ||
          cfg->pred_offsets[body + 1] - cfg->pred_offsets[body] != 1 ||
          cfg->pred_blocks[cfg->pred_offsets[body]] != h || br->a < 0 ||
          br->a >= f->next_value || defs[br->a] < 0)
        continue;
      int cmp_v = nyr_vectorize_resolve_value(f, defs, br->a);
      const nyr_inst_t *cmp = &f->data[(size_t)defs[cmp_v]];
      int64_t limit = 0;
      bool inclusive = cmp->cmp == NYIR_CMP_LE;
      if (cmp->op != NYIR_CMP_I64 || (cmp->cmp != NYIR_CMP_LT && !inclusive) ||
          nyr_vectorize_resolve_value(f, defs, cmp->a) != value ||
          !nyr_const_i64(f, defs, cmp->b, &limit) ||
          (inclusive ? init > limit : init >= limit))
        continue;
      __int128 distance = (__int128)limit - init - (inclusive ? 0 : 1);
      if (distance < 0)
        continue;
      __int128 n = distance / step;
      __int128 max = (__int128)init + n * step;
      if (max < INT64_MIN || max > INT64_MAX || max + step > INT64_MAX)
        continue;
      *out = (nyr_range_t){
          .has_min = true, .has_max = true, .min = init, .max = (int64_t)max};
      return true;
    }
  }
  return false;
}

static bool value_range_under_guards(const nyr_func_t *f, const int *defs,
                                     const nyr_cfg_t *cfg, size_t use_pc,
                                     int value, int loop_iv, int64_t init,
                                     int64_t trip, unsigned depth,
                                     nyr_range_t *out) {
  if (!f || !defs || !cfg || !out || value < 0 || value >= f->next_value ||
      depth > 32)
    return false;
  if (value == loop_iv) {
    __int128 max = (__int128)init + trip - 1;
    if (trip <= 0 || max > INT64_MAX)
      return false;
    *out = (nyr_range_t){
        .has_min = true, .has_max = true, .min = init, .max = (int64_t)max};
    return true;
  }
  int di = defs[value];
  if (di < 0)
    return false;
  const nyr_inst_t *in = &f->data[(size_t)di];
  if (in->op == NYIR_CONST_I64) {
    *out = (nyr_range_t){
        .has_min = true, .has_max = true, .min = in->imm, .max = in->imm};
    return true;
  }
  if (in->op == NYIR_COPY)
    return value_range_under_guards(f, defs, cfg, use_pc, in->a, loop_iv, init,
                                    trip, depth + 1, out);
  if (in->op == NYIR_PHI) {
    if (guarded_induction_range(f, defs, cfg, use_pc, value, out))
      return true;
  }
  nyr_range_t a = {0}, b = {0};
  bool have_a = value_range_under_guards(f, defs, cfg, use_pc, in->a, loop_iv,
                                         init, trip, depth + 1, &a);
  bool have_b = value_range_under_guards(f, defs, cfg, use_pc, in->b, loop_iv,
                                         init, trip, depth + 1, &b);
  if (have_a && have_b && a.has_min && a.has_max && b.has_min && b.has_max) {
    __int128 lo = 0, hi = 0;
    if (in->op == NYIR_ADD_I64) {
      lo = (__int128)a.min + b.min;
      hi = (__int128)a.max + b.max;
    } else if (in->op == NYIR_SUB_I64) {
      lo = (__int128)a.min - b.max;
      hi = (__int128)a.max - b.min;
    } else if (in->op == NYIR_MUL_I64) {
      __int128 products[4] = {(__int128)a.min * b.min, (__int128)a.min * b.max,
                              (__int128)a.max * b.min, (__int128)a.max * b.max};
      lo = hi = products[0];
      for (size_t k = 1; k < 4; ++k) {
        if (products[k] < lo)
          lo = products[k];
        if (products[k] > hi)
          hi = products[k];
      }
    } else if (in->op == NYIR_SHL_I64 && b.min == b.max && b.min >= 0 &&
               b.min <= 62) {
      __int128 scale = (__int128)1 << (unsigned)b.min;
      lo = (__int128)a.min * scale;
      hi = (__int128)a.max * scale;
    } else {
      lo = (__int128)INT64_MAX + 1;
      hi = (__int128)INT64_MIN - 1;
    }
    if (lo >= INT64_MIN && hi <= INT64_MAX) {
      *out = (nyr_range_t){.has_min = true,
                            .has_max = true,
                            .min = (int64_t)lo,
                            .max = (int64_t)hi};
      return true;
    }
    return false;
  }
  if (in->range.has_min && in->range.has_max) {
    *out = in->range;
    return true;
  }
  return false;
}

static bool unique_loop_preheader(const nyr_cfg_t *cfg, const bool *in_loop,
                                  size_t header, size_t *out_preheader) {
  if (!cfg || !in_loop || !out_preheader || header >= cfg->block_count)
    return false;
  size_t preheader = SIZE_MAX;
  size_t outside = 0;
  for (size_t e = cfg->pred_offsets[header]; e < cfg->pred_offsets[header + 1];
       ++e) {
    size_t pred = cfg->pred_blocks[e];
    if (in_loop[pred])
      continue;
    preheader = pred;
    ++outside;
  }
  if (outside != 1 || preheader >= cfg->block_count)
    return false;
  size_t sb = cfg->succ_offsets[preheader];
  size_t se = cfg->succ_offsets[preheader + 1];
  if (se - sb != 1 || cfg->succ_blocks[sb] != header)
    return false;
  *out_preheader = preheader;
  return true;
}

static size_t block_insert_before_terminator(const nyr_func_t *f,
                                             const nyr_cfg_t *cfg,
                                             size_t block) {
  size_t at = cfg->block_end[block];
  while (at > cfg->block_start[block] && f->data[at - 1].op == NYIR_NOP)
    --at;
  if (at > cfg->block_start[block]) {
    nyr_op_t op = f->data[at - 1].op;
    if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET)
      --at;
  }
  return at;
}

static bool fresh_labels(const nyr_func_t *f, int64_t *a, int64_t *b,
                         int64_t *c) {
  int64_t max_label = -1;
  for (size_t i = 0; f && i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm > max_label)
      max_label = f->data[i].imm;
  if (max_label > INT64_MAX - 3)
    return false;
  *a = max_label + 1;
  *b = max_label + 2;
  *c = max_label + 3;
  return true;
}
static bool use_allowed(const nyr_func_t *f, const nyr_cfg_t *cfg,
                        const bool *in_loop, const bool *vec_value,
                        const bool *broadcast_value, size_t user,
                        size_t reduction_update_idx, int reduction_term) {
  if (!f || !cfg || !in_loop || user >= f->len ||
      !in_loop[cfg->inst_block[user]])
    return false;
  const nyr_inst_t *in = &f->data[user];
  if (user == reduction_update_idx)
    return (in->op == NYIR_ADD_I64 || in->op == NYIR_ADD_F64) &&
           ((in->a == reduction_term && in->b >= 0) ||
            (in->b == reduction_term && in->a >= 0)) &&
           vec_value[reduction_term];
  if (in->op == NYIR_STORE_I64)
    return in->c >= 0 && vec_value[in->c];
  if (in->op == NYIR_COPY)
    return in->a >= 0 && vec_value[in->a];
  if (!op_is_vec_i64_bin(in->op) && !op_is_vec_f64_bin(in->op))
    return false;
  if (in->a < 0 || in->b < 0)
    return false;
  if (broadcast_value)
    return (vec_value[in->a] || broadcast_value[in->a]) &&
           (vec_value[in->b] || broadcast_value[in->b]) &&
           (vec_value[in->a] || vec_value[in->b]);
  return vec_value[in->a] && vec_value[in->b];
}

static void replace_uses_outside_loop(nyr_func_t *f, const nyr_cfg_t *cfg,
                                      const bool *in_loop, int old_value,
                                      int new_value) {
  if (!f || !cfg || !in_loop || old_value < 0 || new_value < 0 ||
      old_value == new_value)
    return;
  for (size_t i = 0; i < f->len; ++i) {
    size_t block = cfg->inst_block[i];
    if (block < cfg->block_count && in_loop[block])
      continue;
    nyr_inst_t *in = &f->data[i];
    int *ops[] = {&in->a, &in->b, &in->c, &in->d, &in->e, &in->f};
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
      if (*ops[k] == old_value)
        *ops[k] = new_value;
    for (size_t k = 0; k < in->extra_args_len; ++k)
      if (in->extra_args[k] == old_value)
        in->extra_args[k] = new_value;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value == old_value)
        in->phi_incoming[k].value = new_value;
  }
}

static bool
emit_i64_vector_accumulator(nyr_func_t *f, const bool *in_loop, int scalar_phi,
                            int scalar_init, int vector_phi, int vector_update,
                            int scalar_result, int64_t preheader_label,
                            int64_t header_label, int64_t latch_label,
                            int64_t exit_label) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;
  size_t preheader, header, exit;
  if (!block_for_label(&cfg, preheader_label, &preheader) ||
      !block_for_label(&cfg, header_label, &header) ||
      !block_for_label(&cfg, exit_label, &exit)) {
    nyr_cfg_free(&cfg);
    return false;
  }

  replace_uses_outside_loop(f, &cfg, in_loop, scalar_phi, scalar_result);

  int zero = f->next_value++;
  int vector_zero = f->next_value++;
  size_t at = block_insert_before_terminator(f, &cfg, preheader);
  if (!nir_ensure_inst_space(f, 2)) {
    nyr_cfg_free(&cfg);
    return false;
  }
  memmove(&f->data[at + 2], &f->data[at], (f->len - at) * sizeof(*f->data));
  f->data[at] = (nyr_inst_t){.op = NYIR_CONST_I64, .dst = zero, .imm = 0};
  f->data[at + 1] =
      (nyr_inst_t){.op = NYIR_VEC4_SET1_I64, .dst = vector_zero, .a = zero};
  f->data[at].effects = nyr_inst_effects(&f->data[at]);
  f->data[at + 1].effects = nyr_inst_effects(&f->data[at + 1]);
  f->len += 2;
  nyr_cfg_free(&cfg);

  if (!nyr_cfg_build(f, &cfg))
    return false;
  if (!block_for_label(&cfg, header_label, &header)) {
    nyr_cfg_free(&cfg);
    return false;
  }
  size_t phi_at = cfg.block_start[header];
  while (phi_at < cfg.block_end[header] &&
         (f->data[phi_at].op == NYIR_LABEL || f->data[phi_at].op == NYIR_PHI))
    ++phi_at;
  if (!nir_ensure_inst_space(f, 1)) {
    nyr_cfg_free(&cfg);
    return false;
  }
  memmove(&f->data[phi_at + 1], &f->data[phi_at],
          (f->len - phi_at) * sizeof(*f->data));
  nyr_inst_t phi = {.op = NYIR_PHI, .dst = vector_phi};
  phi.phi_incoming = calloc(2, sizeof(*phi.phi_incoming));
  if (!phi.phi_incoming) {
    nyr_cfg_free(&cfg);
    return false;
  }
  phi.phi_incoming_len = 2;
  phi.phi_incoming[0] = (nyr_phi_incoming_t){
      .predecessor_label = preheader_label, .value = vector_zero};
  phi.phi_incoming[1] = (nyr_phi_incoming_t){.predecessor_label = latch_label,
                                              .value = vector_update};
  f->data[phi_at] = phi;
  ++f->len;
  for (size_t i = cfg.block_start[header]; i < cfg.block_end[header]; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI || in->dst != scalar_phi)
      continue;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].predecessor_label == latch_label)
        in->phi_incoming[k].value = scalar_init;
    break;
  }
  nyr_cfg_free(&cfg);

  if (!nyr_cfg_build(f, &cfg))
    return false;
  if (!block_for_label(&cfg, exit_label, &exit)) {
    nyr_cfg_free(&cfg);
    return false;
  }
  size_t result_at = cfg.block_start[exit];
  while (result_at < cfg.block_end[exit] &&
         (f->data[result_at].op == NYIR_LABEL ||
          f->data[result_at].op == NYIR_PHI))
    ++result_at;
  if (!nir_ensure_inst_space(f, 1)) {
    nyr_cfg_free(&cfg);
    return false;
  }
  memmove(&f->data[result_at + 1], &f->data[result_at],
          (f->len - result_at) * sizeof(*f->data));
  f->data[result_at] = (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_I64,
                                     .dst = scalar_result,
                                     .a = scalar_init,
                                     .b = vector_phi};
  f->data[result_at].effects = nyr_inst_effects(&f->data[result_at]);
  ++f->len;
  nyr_cfg_free(&cfg);
  return true;
}

/*
 * variable-trip helpers
 */

/*
 * Emit a guarded vector bound:
 *   in_range = limit > init
 *   d = limit - init
 *   d = d & -in_range
 *   vec_end = init + (d & ~(VF-1))
 *
 * The predicate is deliberately computed from the original operands, before
 * the subtraction.  Testing the potentially wrapped difference would turn an
 * overflowing positive range into a zero-trip loop (or a reversed range into
 * work).  NyIR integer arithmetic is wrapping, so masking the wrapped value is
 * safe after the original-order predicate has proved the range non-empty.
 */
#define NYIR_VEC_END_PREHEADER_LEN 8
static int emit_vec_end(nyr_func_t *f, size_t insert_at, int limit_v,
                        int init_v, int vf) {
  int trip = f->next_value++;
  int zero_v = f->next_value++;
  int in_range = f->next_value++;
  int range_mask = f->next_value++;
  int clamped = f->next_value++;
  int mask_v = f->next_value++;
  int vec_trip = f->next_value++;
  int vec_end = f->next_value++;
  int64_t mask = (int64_t)(~((uint64_t)(vf - 1)));

  nyr_inst_t preheader[] = {
      {.op = NYIR_SUB_I64,
       .dst = trip,
       .a = limit_v,
       .b = init_v,
       .c = -1,
       .d = -1,
       .e = -1,
       .f = -1},
      {.op = NYIR_CONST_I64,
       .dst = zero_v,
       .a = -1,
       .b = -1,
       .c = -1,
       .d = -1,
       .e = -1,
       .f = -1,
       .imm = 0},
      {.op = NYIR_CMP_I64,
       .dst = in_range,
       .a = limit_v,
       .b = init_v,
       .c = -1,
       .d = -1,
       .e = -1,
       .f = -1,
       .cmp = NYIR_CMP_GT},
      {.op = NYIR_SUB_I64,
       .dst = range_mask,
       .a = zero_v,
       .b = in_range,
       .c = -1,
       .d = -1,
       .e = -1,
       .f = -1},
      {.op = NYIR_AND_I64,
       .dst = clamped,
       .a = trip,
       .b = range_mask,
       .c = -1,
       .d = -1,
       .e = -1,
       .f = -1},
      {.op = NYIR_CONST_I64,
       .dst = mask_v,
       .a = -1,
       .b = -1,
       .c = -1,
       .d = -1,
       .e = -1,
       .f = -1,
       .imm = mask},
      {.op = NYIR_AND_I64,
       .dst = vec_trip,
       .a = clamped,
       .b = mask_v,
       .c = -1,
       .d = -1,
       .e = -1,
       .f = -1},
      {.op = NYIR_ADD_I64,
       .dst = vec_end,
       .a = init_v,
       .b = vec_trip,
       .c = -1,
       .d = -1,
       .e = -1,
       .f = -1},
  };

  size_t n = sizeof(preheader) / sizeof(preheader[0]);
  if (!nir_ensure_inst_space(f, n))
    return -1;
  memmove(&f->data[insert_at + n], &f->data[insert_at],
          (f->len - insert_at) * sizeof(*f->data));
  memcpy(&f->data[insert_at], preheader, n * sizeof(*preheader));
  f->len += n;
  for (size_t k = 0; k < n; ++k)
    f->data[insert_at + k].effects = nyr_inst_effects(&f->data[insert_at + k]);

  return vec_end;
}

/*
 * Save the scalar loop body (instructions between header CMP and latch BR)
 * into a heap-allocated buffer.  Excludes PHIs, the IV update, the header
 * CMP/BR_IF, labels, and branches.  Returns the count of saved instructions
 * or -1 on OOM.
 */
static int save_loop_body(const nyr_func_t *f, const nyr_cfg_t *cfg,
                          const bool *in_loop, size_t head_idx, size_t back,
                          size_t phi_idx, size_t update_idx, size_t cmp_idx,
                          size_t control_idx, nyr_inst_t **out_body) {
  size_t cap = 64;
  *out_body = malloc(cap * sizeof(nyr_inst_t));
  if (!*out_body)
    return -1;
  size_t count = 0;
  for (size_t i = head_idx + 1; i < back; ++i) {
    if (!cfg || !in_loop || !in_loop[cfg->inst_block[i]])
      continue;
    if (i == phi_idx || i == update_idx || i == cmp_idx || i == control_idx)
      continue;
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_LABEL || op == NYIR_BR || op == NYIR_PHI || op == NYIR_BR_IF)
      continue;
    if (count >= cap) {
      cap *= 2;
      nyr_inst_t *n = realloc(*out_body, cap * sizeof(nyr_inst_t));
      if (!n) {
        free(*out_body);
        *out_body = NULL;
        return -1;
      }
      *out_body = n;
    }
    /*
     * Shallow copy — no phi/extra/arg owned metadata in body ops.
     */
    (*out_body)[count++] = f->data[i];
  }
  return (int)count;
}

/*
 * Duplicate one already-vectorized, straight-line loop body so each backedge
 * processes adjacent vector chunks. This is intentionally limited to a single
 * body/latch block and loops without reductions or non-IV recurrences: every
 * copy is independent and uses iv + chunk*vf, while scalar cleanup handles
 * any remainder after the widened step.
 */
static bool vector_unroll_shape(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                size_t body_block, size_t update_idx) {
  if (!f || !cfg || body_block >= cfg->block_count ||
      cfg->block_label[body_block] < 0 ||
      update_idx < cfg->block_start[body_block] ||
      update_idx >= cfg->block_end[body_block])
    return false;
  size_t end = cfg->block_end[body_block];
  while (end > cfg->block_start[body_block] && f->data[end - 1].op == NYIR_NOP)
    --end;
  if (end <= cfg->block_start[body_block] || f->data[end - 1].op != NYIR_BR)
    return false;
  for (size_t i = cfg->block_start[body_block]; i + 1 < end; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_PHI || op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET ||
        f->data[i].phi_incoming_len != 0 || f->data[i].extra_args_len != 0)
      return false;
  }
  return true;
}

static bool vector_unroll_body(nyr_func_t *f, int64_t body_label,
                               size_t update_idx, int iv, int vf, int factor,
                               size_t *inserted_count) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;
  size_t body_block = SIZE_MAX;
  for (size_t b = 0; b < cfg.block_count; ++b)
    if (cfg.block_label[b] == body_label) {
      body_block = b;
      break;
    }
  if (body_block == SIZE_MAX) {
    nyr_cfg_free(&cfg);
    return false;
  }
  size_t begin = cfg.block_start[body_block];
  size_t end = cfg.block_end[body_block];
  while (end > begin && f->data[end - 1].op == NYIR_NOP)
    --end;
  if (!vector_unroll_shape(f, &cfg, body_block, update_idx) || iv < 0 ||
      iv >= f->next_value || vf <= 0 || factor < 2 || factor > 32 ||
      update_idx >= end) {
    nyr_cfg_free(&cfg);
    return false;
  }

  size_t clone_count = 0;
  for (size_t i = begin; i < update_idx; ++i)
    if (f->data[i].op != NYIR_LABEL && f->data[i].op != NYIR_NOP)
      ++clone_count;
  if (clone_count > SIZE_MAX - 2 ||
      (size_t)(factor - 1) > SIZE_MAX / (clone_count + 2)) {
    nyr_cfg_free(&cfg);
    return false;
  }
  size_t per_chunk = clone_count + 2;
  size_t total_insert = (size_t)(factor - 1) * per_chunk;
  if (!nir_ensure_inst_space(f, total_insert)) {
    nyr_cfg_free(&cfg);
    return false;
  }
  int original_value_count = f->next_value;
  nyr_inst_t *clones = calloc(total_insert, sizeof(*clones));
  int *remap = malloc((size_t)original_value_count * sizeof(*remap));
  int *defs = nyr_build_defs(f);
  if (!clones || !remap || !defs) {
    free(clones);
    free(remap);
    free(defs);
    nyr_cfg_free(&cfg);
    return false;
  }
  size_t out = 0;
  for (int chunk = 1; chunk < factor; ++chunk) {
    for (int v = 0; v < original_value_count; ++v)
      remap[v] = -1;
    int offset_v = f->next_value++;
    int chunk_iv = f->next_value++;
    clones[out] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                .dst = offset_v,
                                .a = -1,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .imm = (int64_t)vf * chunk};
    clones[out].effects = nyr_inst_effects(&clones[out]);
    ++out;
    clones[out] = (nyr_inst_t){.op = NYIR_ADD_I64,
                                .dst = chunk_iv,
                                .a = iv,
                                .b = offset_v,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1};
    clones[out].effects = nyr_inst_effects(&clones[out]);
    ++out;
    remap[iv] = chunk_iv;

    for (size_t i = begin; i < update_idx; ++i) {
      nyr_inst_t in = f->data[i];
      if (in.op == NYIR_LABEL || in.op == NYIR_NOP)
        continue;
      if (in.dst >= 0) {
        if (in.dst >= original_value_count) {
          free(clones);
          free(remap);
          free(defs);
          nyr_cfg_free(&cfg);
          return false;
        }
        remap[in.dst] = f->next_value++;
        in.dst = remap[in.dst];
      }
      int *operands[] = {&in.a, &in.b, &in.c, &in.d, &in.e, &in.f};
      for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k) {
        int v = *operands[k];
        if (v < 0 || v >= original_value_count)
          continue;
        if (remap[v] >= 0)
          *operands[k] = remap[v];
        else if (nyr_vectorize_resolve_value(f, defs, v) == iv)
          /*
           * The loop body can refer to a COPY alias of the IV that was
           * defined in the header rather than in this cloned block.  Remap
           * that alias too, otherwise cloned address calculations keep
           * loading/storing the first vector chunk's addresses.
           */
          *operands[k] = chunk_iv;
      }
      in.effects = nyr_inst_effects(&in);
      nyr_clear_derived_facts(&in);
      clones[out++] = in;
    }
  }
  if (out != total_insert) {
    free(clones);
    free(remap);
    free(defs);
    nyr_cfg_free(&cfg);
    return false;
  }
  memmove(&f->data[update_idx + total_insert], &f->data[update_idx],
          (f->len - update_idx) * sizeof(*f->data));
  memcpy(&f->data[update_idx], clones, total_insert * sizeof(*clones));
  f->len += total_insert;
  if (inserted_count)
    *inserted_count = total_insert;
  free(clones);
  free(remap);
  free(defs);
  nyr_cfg_free(&cfg);
  return true;
}

/*
 * Emit a scalar cleanup loop from `vec_end` to `limit` with stride 1.
 * The body is remapped from `saved_body` — each `dst` gets a new value ID
 * and operands are updated.  Loop-invariant operands keep their original
 * IDs.  The cleanup is appended at the end of `f`.
 */
static bool emit_scalar_tail(nyr_func_t *f, const nyr_inst_t *saved_body,
                             int saved_count, int old_iv, int vec_end_v,
                             int limit_v, int64_t exit_label,
                             int64_t dispatch_label, int64_t scalar_label,
                             int64_t scalar_latch_label, int old_reduction_phi,
                             int vector_reduction_v, int old_reduction_update,
                             int tail_acc_v) {
  /*
   * Allocate remap: old value ID → new value ID.  Size to f->next_value
   * before we start allocating new values.  A variable-trip reduction uses a
   * reserved tail_acc_v so outside-loop consumers can already point at the
   * scalar-tail PHI before this appended cleanup block is materialized.
   */
  int old_max = f->next_value;
  int remap_cap = old_max + saved_count * 2 + 16;
  int *remap = calloc((size_t)remap_cap, sizeof(*remap));
  if (!remap)
    return false;
  for (int v = 0; v < remap_cap; ++v)
    remap[v] = -1;

  int siv = f->next_value++;        /* scalar IV */
  int siv_next_v = f->next_value++; /* scalar IV + 1 */
  int scmp = f->next_value++;       /* scalar cmp */
  if (old_iv >= 0 && old_iv < old_max)
    remap[old_iv] = siv;
  if (old_reduction_phi >= 0) {
    if (tail_acc_v < 0 || tail_acc_v >= old_max || vector_reduction_v < 0 ||
        old_reduction_update < 0 || old_reduction_update >= old_max) {
      free(remap);
      return false;
    }
    remap[old_reduction_phi] = tail_acc_v;
  }

  /*
   * Pre-allocate value IDs for each saved instruction.
   */
  for (int k = 0; k < saved_count; ++k) {
    if (saved_body[k].dst >= 0) {
      if (saved_body[k].dst >= remap_cap) {
        free(remap);
        return false;
      }
      remap[saved_body[k].dst] = f->next_value++;
    }
  }

  /*
   * Emit scalar header: LABEL, IV PHI, optional reduction PHI, CMP, BR_IF.
   */
  size_t header_at = f->len;
  size_t hdr_n = old_reduction_phi >= 0 ? 5u : 4u;
  if (!nir_ensure_inst_space(f, hdr_n)) {
    free(remap);
    return false;
  }
  f->data[f->len++] = (nyr_inst_t){.op = NYIR_LABEL,
                                    .dst = -1,
                                    .imm = scalar_label,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1};
  f->data[f->len++] = (nyr_inst_t){.op = NYIR_PHI,
                                    .dst = siv,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1};
  size_t acc_phi_at = SIZE_MAX;
  if (old_reduction_phi >= 0) {
    acc_phi_at = f->len;
    f->data[f->len++] = (nyr_inst_t){.op = NYIR_PHI,
                                      .dst = tail_acc_v,
                                      .a = -1,
                                      .b = -1,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1};
  }
  f->data[f->len++] = (nyr_inst_t){.op = NYIR_CMP_I64,
                                    .dst = scmp,
                                    .a = siv,
                                    .b = limit_v,
                                    .cmp = NYIR_CMP_GE,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1};
  f->data[f->len++] = (nyr_inst_t){.op = NYIR_BR_IF,
                                    .dst = -1,
                                    .a = scmp,
                                    .imm = exit_label,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1};
  for (size_t i = header_at; i < f->len; ++i)
    f->data[i].effects = nyr_inst_effects(&f->data[i]);

  for (int k = 0; k < saved_count; ++k) {
    nyr_inst_t inst = saved_body[k];
    if (inst.dst >= 0 && inst.dst < remap_cap && remap[inst.dst] >= 0)
      inst.dst = remap[inst.dst];
    if (inst.a >= 0 && inst.a < remap_cap && remap[inst.a] >= 0)
      inst.a = remap[inst.a];
    if (inst.b >= 0 && inst.b < remap_cap && remap[inst.b] >= 0)
      inst.b = remap[inst.b];
    if (inst.c >= 0 && inst.c < remap_cap && remap[inst.c] >= 0)
      inst.c = remap[inst.c];
    if (inst.d >= 0 && inst.d < remap_cap && remap[inst.d] >= 0)
      inst.d = remap[inst.d];
    if (inst.e >= 0 && inst.e < remap_cap && remap[inst.e] >= 0)
      inst.e = remap[inst.e];
    if (inst.f >= 0 && inst.f < remap_cap && remap[inst.f] >= 0)
      inst.f = remap[inst.f];
    if (inst.extra_args) {
      /*
       * Saved variable-trip loop bodies currently exclude CALLs, but keep
       * this defensive path correct if that legality rule changes later.
       */
      for (size_t a = 0; a < inst.extra_args_len; ++a) {
        int v = inst.extra_args[a];
        if (v >= 0 && v < remap_cap && remap[v] >= 0)
          inst.extra_args[a] = remap[v];
      }
    }

    if (!nir_ensure_inst_space(f, 1)) {
      free(remap);
      return false;
    }
    inst.effects = nyr_inst_effects(&inst);
    f->data[f->len++] = inst;
  }

  /*
   * Emit scalar latch: constant one, label, ADD, BR.
   */
  int one_v = f->next_value++;
  if (!nir_ensure_inst_space(f, 4)) {
    free(remap);
    return false;
  }
  f->data[f->len++] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                    .dst = one_v,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = 1};
  f->data[f->len++] = (nyr_inst_t){.op = NYIR_LABEL,
                                    .dst = -1,
                                    .imm = scalar_latch_label,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1};
  f->data[f->len++] = (nyr_inst_t){.op = NYIR_ADD_I64,
                                    .dst = siv_next_v,
                                    .a = siv,
                                    .b = one_v,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1};
  f->data[f->len++] = (nyr_inst_t){.op = NYIR_BR,
                                    .dst = -1,
                                    .imm = scalar_label,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1};
  for (size_t i = f->len - 4; i < f->len; ++i)
    f->data[i].effects = nyr_inst_effects(&f->data[i]);

  nyr_inst_t *iv_phi = &f->data[header_at + 1];
  iv_phi->phi_incoming = malloc(2 * sizeof(*iv_phi->phi_incoming));
  if (!iv_phi->phi_incoming) {
    free(remap);
    return false;
  }
  iv_phi->phi_incoming_len = 2;
  iv_phi->phi_incoming[0].predecessor_label = dispatch_label;
  iv_phi->phi_incoming[0].value = vec_end_v;
  iv_phi->phi_incoming[1].predecessor_label = scalar_latch_label;
  iv_phi->phi_incoming[1].value = siv_next_v;

  if (old_reduction_phi >= 0) {
    int tail_next = remap[old_reduction_update];
    if (tail_next < 0 || acc_phi_at == SIZE_MAX) {
      free(remap);
      return false;
    }
    nyr_inst_t *acc_phi = &f->data[acc_phi_at];
    acc_phi->phi_incoming = malloc(2 * sizeof(*acc_phi->phi_incoming));
    if (!acc_phi->phi_incoming) {
      free(remap);
      return false;
    }
    acc_phi->phi_incoming_len = 2;
    acc_phi->phi_incoming[0].predecessor_label = dispatch_label;
    acc_phi->phi_incoming[0].value = vector_reduction_v;
    acc_phi->phi_incoming[1].predecessor_label = scalar_latch_label;
    acc_phi->phi_incoming[1].value = tail_next;
  }

  /*
   * Keep SSA definitions before the original exit block in linear NYIR order.
   * The verifier intentionally rejects a RET/use that appears textually before
   * its definition even when CFG dominance would make it valid.  The cleanup
   * was easiest to build at the end; move that complete instruction range to
   * immediately before the named exit once all owned PHI metadata is filled.
   */
  size_t tail_end = f->len;
  size_t exit_at = SIZE_MAX;
  for (size_t i = 0; i < header_at; ++i) {
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm == exit_label) {
      exit_at = i;
      break;
    }
  }
  if (exit_at == SIZE_MAX || exit_at >= header_at) {
    free(remap);
    return false;
  }
  size_t tail_n = tail_end - header_at;
  nyr_inst_t *tail = malloc(tail_n * sizeof(*tail));
  if (!tail) {
    free(remap);
    return false;
  }
  memcpy(tail, &f->data[header_at], tail_n * sizeof(*tail));
  memmove(&f->data[exit_at + tail_n], &f->data[exit_at],
          (header_at - exit_at) * sizeof(*f->data));
  memcpy(&f->data[exit_at], tail, tail_n * sizeof(*tail));
  free(tail);

  free(remap);
  return true;
}

/*
 * Return 1 when one loop was transformed, 0 when no candidate exists, -1 OOM.
 *
 * A small target-independent cost model.  Costs are relative scalar issue
 * slots rather than target cycles: memory operations dominate, vector integer
 * operations cost one slot, and loop-control/setup are charged explicitly.
 * Requiring a positive saving prevents widening tiny bodies where the vector
 * setup and scalar cleanup are more expensive than the work removed.
 */
static uint64_t vector_trip_estimate(const nyr_func_t *f, const int *defs,
                                     int limit_v, int64_t init, int vf) {
  uint64_t fallback = (uint64_t)vf * 4u;
  if (!f || !defs || limit_v < 0 || limit_v >= f->next_value ||
      defs[limit_v] < 0 || (size_t)defs[limit_v] >= f->len)
    return fallback;
  const nyr_range_t *range = &f->data[(size_t)defs[limit_v]].range;
  bool have_min = range->has_min && range->min > init;
  bool have_max = range->has_max && range->max > init;
  uint64_t min_trip = 0, max_trip = 0;
  if (have_min) {
    __int128 d = (__int128)range->min - (__int128)init;
    min_trip = d > UINT64_MAX ? UINT64_MAX : (uint64_t)d;
  }
  if (have_max) {
    __int128 d = (__int128)range->max - (__int128)init;
    max_trip = d > UINT64_MAX ? UINT64_MAX : (uint64_t)d;
  } else if (range->has_max) {
    /*
     * The loop is known never to execute.
     */
    return 0;
  }
  if (have_min && have_max)
    return min_trip + (max_trip - min_trip) / 2u;
  if (have_min)
    return min_trip > fallback ? min_trip : fallback;
  if (have_max)
    return max_trip < fallback ? max_trip : fallback;
  return fallback;
}

static uint64_t vectorization_cost(size_t loads, size_t stores,
                                   size_t vector_ops, int vf,
                                   bool variable_trip, uint64_t iterations) {
  if (vf < NYIR_I64_VF_MIN || iterations < (uint64_t)vf)
    return UINT64_MAX;
  uint64_t body = (uint64_t)(loads + stores) * 2u + vector_ops;
  uint64_t setup = variable_trip ? 12u : 2u;
  uint64_t chunks = iterations / (uint64_t)vf;
  uint64_t tail = iterations % (uint64_t)vf;
  uint64_t chunk_cost = body + 2u;

  /*
   * Wider vectors are not free: large packed expression trees increase
   * register pressure and can make a 128-bit pack preferable. This is still a
   * target-independent estimate; target calibration remains a separate task.
   */
  uint64_t pressure = loads + vector_ops;
  if (pressure > 8u)
    chunk_cost += (pressure - 8u + 3u) / 4u;
  if (vf >= 4 && pressure > 4u)
    chunk_cost += (pressure - 4u + 3u) / 4u;

  if (chunks > (UINT64_MAX - setup) / (chunk_cost ? chunk_cost : 1u))
    return UINT64_MAX;
  uint64_t cost = setup + chunks * chunk_cost;
  uint64_t scalar_iter_cost = body + 2u;
  if (tail && tail > (UINT64_MAX - cost) / scalar_iter_cost)
    return UINT64_MAX;
  return cost + tail * scalar_iter_cost;
}

static bool vectorization_profitable(size_t loads, size_t stores,
                                     size_t vector_ops, int vf,
                                     bool variable_trip, uint64_t trip) {
  if (vf < NYIR_I64_VF_MIN || loads + stores == 0)
    return false;
  uint64_t body = (uint64_t)(loads + stores) * 2u + vector_ops;
  if (trip < (uint64_t)vf || body + 2u == 0)
    return false;
  if (trip > UINT64_MAX / (body + 2u))
    return true;
  uint64_t scalar_cost = trip * (body + 2u);
  return vectorization_cost(loads, stores, vector_ops, vf, variable_trip,
                            trip) < scalar_cost;
}

/*
 * Track affine accesses to each private root within a candidate loop body.
 * Reads at several distinct (loop-invariant) offsets are safe: shifted reads
 * cannot alias.  A write pins its offset, so any other offset for that root
 * becomes a potential loop-carried dependency and is rejected.
 */
typedef struct {
  int root;
  int64_t offset;
  bool dynamic_offset;
  bool written;
} root_access_t;

static bool track_root_access(const nyr_func_t *f, const int *defs,
                              root_access_t *seen, size_t *seen_count,
                              size_t seen_cap, int root, int64_t offset,
                              bool dynamic_offset, bool is_store) {
  for (size_t r = 0; r < *seen_count; ++r) {
    if (seen[r].root != root) {
      bool root_is_phi = root >= 0 && root < f->next_value && defs[root] >= 0 &&
                         f->data[(size_t)defs[root]].op == NYIR_PHI;
      int prior = seen[r].root;
      bool prior_is_phi = prior >= 0 && prior < f->next_value &&
                          defs[prior] >= 0 &&
                          f->data[(size_t)defs[prior]].op == NYIR_PHI;
      if ((root_is_phi || prior_is_phi) &&
          !tbuf_swap_phi_pair(f, defs, root, prior, NULL))
        return false;
      continue;
    }
    if (seen[r].dynamic_offset || dynamic_offset) {
      if (seen[r].written || is_store)
        return false;
      return true;
    }
    if (seen[r].offset == offset) {
      if (is_store)
        seen[r].written = true;
      return true;
    }
    if (seen[r].written || is_store)
      return false;
  }
  if (*seen_count >= seen_cap)
    return false;
  seen[*seen_count].root = root;
  seen[*seen_count].offset = offset;
  seen[*seen_count].dynamic_offset = dynamic_offset;
  seen[*seen_count].written = is_store;
  (*seen_count)++;
  return true;
}

static int vectorize_once(nyr_func_t *f) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return -1;
  int *defs = nyr_build_defs(f);
  if (!defs) {
    nyr_cfg_free(&cfg);
    return -1;
  }

  for (size_t back = 0; back < f->len; ++back) {
    nyr_inst_t *br = &f->data[back];
    if (br->op != NYIR_BR || br->imm < 0)
      continue;
    size_t latch = cfg.inst_block[back];
    size_t header = SIZE_MAX;
    size_t head_idx = SIZE_MAX;
    for (size_t b = 0; b < cfg.block_count; ++b) {
      if (cfg.block_label[b] == br->imm) {
        header = b;
        head_idx = cfg.block_start[b];
        break;
      }
    }
    if (header == SIZE_MAX || head_idx >= back ||
        !nyr_cfg_is_backedge(&cfg, latch, header))
      continue;
    bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
    if (!in_loop) {
      free(defs);
      nyr_cfg_free(&cfg);
      return -1;
    }
    if (!nyr_cfg_natural_loop_blocks(&cfg, latch, header, in_loop,
                                      cfg.block_count)) {
      free(in_loop);
      free(defs);
      nyr_cfg_free(&cfg);
      return -1;
    }
    int64_t latch_label = cfg.block_label[latch];

    int iv = -1, init_v = -1, update_v = -1;
    size_t phi_idx = SIZE_MAX;
    for (size_t i = cfg.block_start[header]; i < cfg.block_end[header]; ++i) {
      const nyr_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI || phi->dst < 0 || phi->phi_incoming_len != 2)
        continue;
      int back_in = -1, pre_in = -1;
      for (size_t k = 0; k < 2; ++k) {
        if (phi->phi_incoming[k].predecessor_label == latch_label)
          back_in = phi->phi_incoming[k].value;
        else
          pre_in = phi->phi_incoming[k].value;
      }
      if (back_in < 0 || pre_in < 0 || defs[back_in] < 0)
        continue;
      const nyr_inst_t *update = &f->data[(size_t)defs[back_in]];
      int step_v = -1;
      if (update->op == NYIR_ADD_I64) {
        int update_a = nyr_vectorize_resolve_value(f, defs, update->a);
        int update_b = nyr_vectorize_resolve_value(f, defs, update->b);
        if (update_a == phi->dst)
          step_v = update->b;
        else if (update_b == phi->dst)
          step_v = update->a;
      }
      int64_t step_value = 0;
      if (step_v < 0 || !nyr_const_i64(f, defs, step_v, &step_value) ||
          step_value != 1)
        continue;
      iv = phi->dst;
      init_v = pre_in;
      update_v = back_in;
      phi_idx = i;
      break;
    }
    if (iv < 0 || update_v < 0 || defs[update_v] < 0) {
      nyr_vector_trace(f, head_idx, "reject", "no canonical +1 induction PHI",
                        0);
      free(in_loop);
      continue;
    }
    size_t update_idx = (size_t)defs[update_v];
    if (update_idx <= phi_idx || update_idx >= back ||
        !in_loop[cfg.inst_block[update_idx]]) {
      free(in_loop);
      continue;
    }
    nyr_inst_t *update = &f->data[update_idx];
    if (update->op != NYIR_ADD_I64 || update->dst != update_v) {
      free(in_loop);
      continue;
    }
    int update_a = nyr_vectorize_resolve_value(f, defs, update->a);
    int update_b = nyr_vectorize_resolve_value(f, defs, update->b);
    bool iv_update_has_iv_on_left = update_a == iv;
    int step_v = update_a == iv ? update->b : update_b == iv ? update->a : -1;
    int64_t step = 0, init = 0;
    if (step_v < 0 || !nyr_const_i64(f, defs, step_v, &step) || step != 1 ||
        !nyr_const_i64(f, defs, init_v, &init)) {
      free(in_loop);
      continue;
    }

    /*
     * Only transform the canonical header-controlled form.  Merely finding an
     * unrelated iv < constant comparison somewhere in the lexical loop range
     * is not enough to justify changing the recurrence step.
     */
    if (cfg.block_end[header] <= cfg.block_start[header]) {
      free(in_loop);
      continue;
    }
    /*
     * Find the conditional branch in the header block.  The header may have
     * multiple terminators (e.g., br.if followed by br for loop exit).
     * Scan backwards from the end of the header block to find the br.if.
     */
    size_t control_idx = SIZE_MAX;
    const nyr_inst_t *control = NULL;
    for (size_t i = cfg.block_end[header]; i > cfg.block_start[header]; --i) {
      size_t idx = i - 1;
      const nyr_inst_t *inst = &f->data[idx];
      if (inst->op == NYIR_BR_IF) {
        control_idx = idx;
        control = inst;
        break;
      }
    }
    if (!control || control->op != NYIR_BR_IF || control->a < 0 ||
        control->a >= f->next_value || defs[control->a] < 0) {
      nyr_vector_trace(f, head_idx, "reject",
                        "header has no usable conditional branch", 0);
      free(in_loop);
      continue;
    }
    size_t cmp_idx = (size_t)defs[control->a];
    const nyr_inst_t *cmp = &f->data[cmp_idx];
    bool is_var_trip = false;
    int64_t limit = 0;
    int limit_v = -1;
    if (cmp_idx < cfg.block_start[header] || cmp_idx >= control_idx ||
        cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_LT) {
      nyr_vector_trace(f, control_idx, "reject",
                        "loop guard is not canonical i64 < bound", 0);
      free(in_loop);
      continue;
    }
    int cmp_a_root = nyr_vectorize_resolve_value(f, defs, cmp->a);
    if (cmp_a_root != iv) {
      free(in_loop);
      continue;
    }
    limit_v = cmp->b;
    if (nyr_const_i64(f, defs, cmp->b, &limit)) {
      if (limit <= init) {
        free(in_loop);
        continue;
      }
    } else {
      /*
       * Variable bound: use VF=4 (256-bit).  Safe for any runtime count.
       */
      is_var_trip = true;
    }
    size_t body_target = SIZE_MAX;
    if (!block_for_label(&cfg, control->imm, &body_target) ||
        body_target == header || !in_loop[body_target]) {
      free(in_loop);
      continue;
    }
    f->vectorize_attempted_loops++;
    int vf = 0;
    int64_t fixed_trip = 0;
    uint64_t estimated_trip = 0;
    bool is_f64 = false;
    bool is_i64 = false;
    nyr_op_t vload_op = NYIR_NOP, vstore_op = NYIR_NOP;
    if (is_var_trip) {
      /*
       * Start with VF=4 for variable bounds, then compare VF=2 after the
       * body cost is known. When the trip is smaller than the selected VF,
       * the vector body is skipped and the scalar tail remains correct.
       * Bounds checks remain scalar-only in this mode until a runtime
       * capacity guard is available.
       */
      vf = 4;
      vload_op = NYIR_VEC8_LOAD_I64;
      vstore_op = NYIR_VEC8_STORE_I64;
    } else {
      uint64_t trip = (uint64_t)limit - (uint64_t)init;
      if (trip > (uint64_t)INT64_MAX) {
        free(in_loop);
        continue;
      }
      fixed_trip = (int64_t)trip;
      if (trip >= 8) {
        vf = 4;
        vload_op = NYIR_VEC8_LOAD_I64;
        vstore_op = NYIR_VEC8_STORE_I64;
      } else if (trip >= 4) {
        vf = 2;
        vload_op = NYIR_VEC4_LOAD_I64;
        vstore_op = NYIR_VEC4_STORE_I64;
      }
    }
    if (vf != 0)
      estimated_trip = is_var_trip
                           ? vector_trip_estimate(f, defs, limit_v, init, vf)
                           : (uint64_t)fixed_trip;
    if (vf == 0 || estimated_trip < (uint64_t)vf) {
      f->vectorize_rejected_loops++;
      nyr_vector_trace(
          f, cmp_idx, "reject",
          vf == 0 ? "trip count has no supported profitable vector width"
                  : "range facts prove fewer than one vector chunk",
          vf);
      free(in_loop);
      continue;
    }
    /*
     * Address IVs introduced by earlier loop passes are legal when they are
     * affine recurrences.  They must advance by VF×step in the vector body.
     * A single scalar reduction PHI is widened by reducing each vector chunk
     * back into the scalar accumulator.
     */
    bool extra_phi = false;
    int extra_phi_dst = -1;
    size_t extra_update_idx = SIZE_MAX;
    int64_t extra_step = 0;
    int reduction_phi_dst = -1;
    int reduction_phi_init = -1;
    int64_t reduction_preheader_label = -1;
    size_t reduction_update_idx = SIZE_MAX;
    int reduction_update_value = -1;
    int reduction_term = -1;
    bool reduction_f64 = false;
    for (size_t i = cfg.block_start[header]; i < cfg.block_end[header]; ++i) {
      if (f->data[i].op != NYIR_PHI || f->data[i].dst == iv)
        continue;
      nyr_inst_t *phi = &f->data[i];
      int back_in = -1;
      int pre_in = -1;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        if (phi->phi_incoming[k].predecessor_label == latch_label)
          back_in = phi->phi_incoming[k].value;
        else
          pre_in = phi->phi_incoming[k].value;
      }
      if (back_in < 0 || defs[back_in] < 0) {
        extra_phi = true;
        break;
      }
      const nyr_inst_t *back_def = &f->data[(size_t)defs[back_in]];
      int back_a = nyr_vectorize_resolve_value(f, defs, back_def->a);
      int back_b = nyr_vectorize_resolve_value(f, defs, back_def->b);
      int term = -1;
      bool this_reduction_f64 = false;
      /*
       * The f64 vector reduction extracts and adds lanes in ascending order,
       * starting from the scalar accumulator. That preserves the original
       * left-to-right IEEE addition sequence, so it does not require
       * fast-math. Integer wrapping add remains unconditional as well.
       */
      if (back_def->op == NYIR_ADD_I64 || back_def->op == NYIR_ADD_F64) {
        if (back_a == phi->dst)
          term = back_b;
        else if (back_b == phi->dst)
          term = back_a;
        this_reduction_f64 = back_def->op == NYIR_ADD_F64;
      }
      /*
       * The vector reduction takes the original scalar PHI as its seed, so
       * the seed may be loop-invariant state carried from an enclosing loop.
       * Requiring a literal initializer unnecessarily blocked reductions in
       * nested loops even though the scalar seed is preserved exactly.
       */
      if (term >= 0 && reduction_phi_dst < 0 && pre_in >= 0) {
        reduction_phi_dst = phi->dst;
        reduction_phi_init = pre_in;
        for (size_t k = 0; k < phi->phi_incoming_len; ++k)
          if (phi->phi_incoming[k].value == pre_in &&
              phi->phi_incoming[k].predecessor_label != latch_label)
            reduction_preheader_label = phi->phi_incoming[k].predecessor_label;
        reduction_update_idx = (size_t)defs[back_in];
        reduction_update_value = back_in;
        reduction_term = term;
        reduction_f64 = this_reduction_f64;
        continue;
      }
      affine_t extra = affine_value(f, defs, &cfg, in_loop, phi->dst, iv, 0);
      if (!extra.ok || extra.root >= 0 || extra.stride == 0 ||
          extra_phi_dst >= 0) {
        extra_phi = true;
        break;
      }
      extra_phi_dst = phi->dst;
      extra_update_idx = (size_t)defs[back_in];
      extra_step = extra.stride;
    }
    if (extra_phi) {
      f->vectorize_rejected_loops++;
      nyr_vector_trace(f, phi_idx, "reject", "unsupported loop-carried PHI",
                        vf);
      free(in_loop);
      continue;
    }

    size_t scalar_value_count = (size_t)f->next_value;
    bool *vec = calloc(scalar_value_count, sizeof(*vec));
    bool *rewrite = calloc(f->len, sizeof(*rewrite));
    bool *broadcast_value =
        calloc(scalar_value_count, sizeof(*broadcast_value));
    int *broadcast_map = malloc(scalar_value_count * sizeof(*broadcast_map));
    if (!vec || !rewrite || !broadcast_value || !broadcast_map) {
      free(vec);
      free(rewrite);
      free(broadcast_value);
      free(broadcast_map);
      free(in_loop);
      free(defs);
      nyr_cfg_free(&cfg);
      return -1;
    }
    for (size_t v = 0; v < scalar_value_count; ++v)
      broadcast_map[v] = -1;
    size_t vec_loads = 0;
    size_t vec_stores = 0;
    size_t vec_ops = 0;
    bool bad = false;
    const char *reject_reason = NULL;
    size_t reject_pc = head_idx;
    /*
     * Track affine accesses per private root.  Reads at distinct offsets are
     * safe; a write pins its offset and rejects any other offset.
     */
    root_access_t seen_roots[64];
    size_t seen_count = 0;
    bool read_only_loop = true;
    for (size_t i = 0; i < f->len; ++i) {
      if (!in_loop[cfg.inst_block[i]])
        continue;
      const nyr_inst_t *in = &f->data[i];
      nyr_effect_t effects =
          (nyr_effect_t)(in->effects | nyr_inst_effects(in));
      if (in->op == NYIR_STORE_I64 ||
          (effects &
           (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE |
            NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT))) {
        read_only_loop = false;
        break;
      }
    }

    for (size_t i = head_idx + 1; i < back && !bad; ++i) {
      if (!in_loop[cfg.inst_block[i]])
        continue;
      nyr_inst_t *in = &f->data[i];
      nyr_effect_t effects =
          (nyr_effect_t)(in->effects | nyr_inst_effects(in));
      if (i == phi_idx || i == update_idx || i == reduction_update_idx ||
          i == cmp_idx || i == control_idx || in->op == NYIR_LABEL ||
          in->op == NYIR_BR)
        continue;
      if (in->op == NYIR_PHI && cfg.inst_block[i] == header)
        continue;
      if (in->op == NYIR_BOUNDS_CHECK) {
        /*
         * Keep checked typed-buffer reductions scalar. The native encoder
         * preserves the vector loop's guarded memory semantics, but LLVM's
         * O2 pipeline can fold the resulting managed-buffer reduction away
         * after the check is hoisted. The scalar check remains the portable
         * fallback until LLVM retains the same provenance through vector IR.
         */
        if (nyr_vectorize_is_typed_buffer(f, defs, in->a)) {
          bad = true;
          reject_reason = "managed typed-buffer bounds guard is not portable";
          reject_pc = i;
          break;
        }
        bool safe =
            is_var_trip
                ? bounds_check_safe_variable(f, defs, &cfg, in_loop, in, iv,
                                             limit_v, init)
                : bounds_check_safe(f, defs, &cfg, in_loop, in, iv, fixed_trip);
        if (!safe) {
          bad = true;
          reject_reason = "bounds check is not proven safe for vector body";
          reject_pc = i;
          break;
        }
        /*
         * The legality proof covers every vector lane and the scalar tail, so
         * retaining a per-chunk scalar check only adds hot-loop overhead.
         */
        nyr_inst_discard(in);
        continue;
      }
      if (in->op == NYIR_PHI || in->op == NYIR_BR_IF || in->op == NYIR_RET ||
          (effects &
           (NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_WRITE_LOCAL |
            NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_ALLOCATION |
            NYIR_EFFECT_UNKNOWN_SIDE_EFFECT))) {
        bad = true;
        reject_reason = "control/local/call/volatile/allocation effect blocks "
                        "vectorization";
        reject_pc = i;
        break;
      }
      if (in->op == NYIR_LOAD_I64 && in->dst >= 0) {
        bool mem_f64 = in->scalar_class == NYIR_SCALAR_F64;
        affine_t a = {0};
        bool dynamic_offset = false;
        bool access_ok = vector_memory_affine(f, defs, &cfg, in_loop, in->a, iv,
                                              init, is_var_trip, fixed_trip, i,
                                              &a, &dynamic_offset);
        if (!access_ok && read_only_loop)
          access_ok =
              vector_checked_read_affine(f, defs, &cfg, in_loop, in->a, iv, &a);
        if (!access_ok ||
            (!read_only_loop &&
             !track_root_access(f, defs, seen_roots, &seen_count, 64, a.root,
                                a.offset, dynamic_offset, false))) {
          bad = true;
          reject_reason = "load address is non-affine, aliasing, escaping, or "
                          "out of proven bounds";
          reject_pc = i;
          break;
        }
        if (mem_f64)
          is_f64 = true;
        else
          is_i64 = true;
        vec[in->dst] = true;
        rewrite[i] = true;
        vec_loads++;
        continue;
      }
      if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 && vec[in->a]) {
        vec[in->dst] = true;
        rewrite[i] = true;
        continue;
      }
      if (op_is_vec_i64_bin(in->op) && in->dst >= 0) {
        bool va =
            in->a >= 0 && (size_t)in->a < scalar_value_count && vec[in->a];
        bool vb =
            in->b >= 0 && (size_t)in->b < scalar_value_count && vec[in->b];
        if (va || vb) {
          if (!va) {
            if (in->a < 0 || (size_t)in->a >= scalar_value_count ||
                value_defined_in_loop(f, defs, &cfg, in_loop, in->a)) {
              bad = true;
              reject_reason = "mixed i64 operand is not loop-invariant";
              reject_pc = i;
              break;
            }
            broadcast_value[in->a] = true;
          }
          if (!vb) {
            if (in->b < 0 || (size_t)in->b >= scalar_value_count ||
                value_defined_in_loop(f, defs, &cfg, in_loop, in->b)) {
              bad = true;
              reject_reason = "mixed i64 operand is not loop-invariant";
              reject_pc = i;
              break;
            }
            broadcast_value[in->b] = true;
          }
          vec[in->dst] = true;
          rewrite[i] = true;
          is_i64 = true;
          vec_ops++;
        }
        continue;
      }

      /*
       * f64 vector arithmetic
       */
      if (op_is_vec_f64_bin(in->op) && in->dst >= 0) {
        bool va =
            in->a >= 0 && (size_t)in->a < scalar_value_count && vec[in->a];
        bool vb =
            in->b >= 0 && (size_t)in->b < scalar_value_count && vec[in->b];
        if (va || vb) {
          if (!va) {
            if (in->a < 0 || (size_t)in->a >= scalar_value_count ||
                value_defined_in_loop(f, defs, &cfg, in_loop, in->a)) {
              bad = true;
              reject_reason = "mixed f64 operand is not loop-invariant";
              reject_pc = i;
              break;
            }
            broadcast_value[in->a] = true;
          }
          if (!vb) {
            if (in->b < 0 || (size_t)in->b >= scalar_value_count ||
                value_defined_in_loop(f, defs, &cfg, in_loop, in->b)) {
              bad = true;
              reject_reason = "mixed f64 operand is not loop-invariant";
              reject_pc = i;
              break;
            }
            broadcast_value[in->b] = true;
          }
          vec[in->dst] = true;
          rewrite[i] = true;
          is_f64 = true;
          vec_ops++;
        }
        continue;
      }
      if (in->op == NYIR_STORE_I64 && in->c >= 0 && vec[in->c]) {
        bool mem_f64 = in->scalar_class == NYIR_SCALAR_F64;
        affine_t a = {0};
        bool dynamic_offset = false;
        if (!vector_memory_affine(f, defs, &cfg, in_loop, in->a, iv, init,
                                  is_var_trip, fixed_trip, i, &a,
                                  &dynamic_offset) ||
            !track_root_access(f, defs, seen_roots, &seen_count, 64, a.root,
                               a.offset, dynamic_offset, true)) {
          bad = true;
          reject_reason = "store address is non-affine, aliasing, escaping, or "
                          "out of proven bounds";
          reject_pc = i;
          break;
        }
        if (mem_f64)
          is_f64 = true;
        else
          is_i64 = true;
        rewrite[i] = true;
        vec_stores++;
        continue;
      }
      if (in->op == NYIR_STORE_I64 ||
          (effects & (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
                      NYIR_EFFECT_MAY_TRAP))) {
        bad = true;
        reject_reason = "unmodeled memory/trap effect remains in loop body";
        reject_pc = i;
        break;
      }
    }

    if (!bad && is_f64) {
      if (is_i64) {
        bad = true;
        reject_reason = "mixed i64/f64 vector body";
        reject_pc = head_idx;
      } else if (estimated_trip < 4) {
        bad = true;
        reject_reason = "f64 loop has fewer than four iterations";
        reject_pc = head_idx;
      } else {
        /*
         * VEC4_F64 is four lanes. Keep its iteration step, memory width,
         * scalar tail, and reduction width in lockstep even when the generic
         * integer cost model initially selected VF=2.
         */
        vf = 4;
        vload_op = NYIR_VEC4_LOAD_F64;
        vstore_op = NYIR_VEC4_STORE_F64;
      }
    }

    if (!bad && is_i64 && !is_f64 && vf == 4) {
      bool needs_broadcast = false;
      for (size_t v = 0; v < scalar_value_count; ++v)
        if (broadcast_value[v]) {
          needs_broadcast = true;
          break;
        }
      if (needs_broadcast) {
        /*
         * NYIR currently has a portable 128-bit i64 broadcast.  Prefer a
         * legal two-lane vector over rejecting the loop entirely; the wider
         * broadcast can be added independently when all targets support it.
         */
        vf = 2;
        vload_op = NYIR_VEC4_LOAD_I64;
        vstore_op = NYIR_VEC4_STORE_I64;
        nyr_vector_trace(f, head_idx, "width",
                          "invariant i64 broadcast selects 128-bit width", vf);
      }
    }

    if (!bad && is_i64 && !is_f64 && vf == 4 && estimated_trip >= 4) {
      uint64_t cost4 = vectorization_cost(vec_loads, vec_stores, vec_ops, 4,
                                          is_var_trip, estimated_trip);
      uint64_t cost2 = vectorization_cost(vec_loads, vec_stores, vec_ops, 2,
                                          is_var_trip, estimated_trip);
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "LOOP_VECTORIZE",
            "width model pc=%zu trip=%llu variable=%s streams=%zu load/%zu "
            "store ops=%zu cost(vf=4)=%llu cost(vf=2)=%llu",
            head_idx, (unsigned long long)estimated_trip,
            is_var_trip ? "yes" : "no", vec_loads, vec_stores, vec_ops,
            (unsigned long long)cost4, (unsigned long long)cost2);
      if (cost2 < cost4) {
        vf = 2;
        vload_op = NYIR_VEC4_LOAD_I64;
        vstore_op = NYIR_VEC4_STORE_I64;
        nyr_vector_trace(
            f, head_idx, "width",
            "128-bit width wins estimated setup/tail/pressure cost", vf);
      }
    }

    /*
     * Accumulate fixed-trip integer reductions in vector lanes. Reducing each
     * vector chunk to a scalar keeps a serial dependency in the hot loop.
     */
    if (!bad && reduction_phi_dst >= 0 && !reduction_f64 && !is_var_trip &&
        fixed_trip >= 2 && fixed_trip % 2 == 0) {
      vf = 2;
      vload_op = NYIR_VEC4_LOAD_I64;
      vstore_op = NYIR_VEC4_STORE_I64;
    }

    nyr_use_def_t uses = {0};
    if (!bad && !nyr_build_use_def(f, &uses))
      bad = true;
    int64_t vector_exit_label = -1;
    /*
     * Keep the loop-carried reduction scalar until register allocation can
     * prove a vector PHI remains resident across its CFG edges. The current
     * x86 allocator homes that PHI on every vector iteration, which is more
     * expensive than reducing each vector chunk into the scalar accumulator.
     * The ordinary vector reduction below still widens memory accesses.
     */
    bool vector_accumulator_safe = false;
    /*
     * The reduction update is excluded from the body scan above. This
     * transformation emits one vector add per chunk in place of one scalar
     * add per element, so include it in both sides of the cost comparison.
     */
    if (vector_accumulator_safe)
      vec_ops++;
    if (!vector_accumulator_safe && nyr_vector_trace_enabled())
      ny_trace_line("VECTORIZE",
                    "vector accumulator ineligible: reduction=%d init=%d "
                    "preheader=%lld f64=%s variable=%s trip=%lld vf=%d "
                    "extra_phi=%d body_is_latch=%s",
                    reduction_phi_dst, reduction_phi_init,
                    (long long)reduction_preheader_label,
                    reduction_f64 ? "yes" : "no", is_var_trip ? "yes" : "no",
                    (long long)fixed_trip, vf, extra_phi_dst,
                    body_target == latch ? "yes" : "no");
    if (!bad && vector_accumulator_safe) {
      size_t exit_block = SIZE_MAX;
      for (size_t e = cfg.succ_offsets[header];
           e < cfg.succ_offsets[header + 1]; ++e) {
        size_t succ = cfg.succ_blocks[e];
        if (succ != body_target && !in_loop[succ]) {
          if (exit_block != SIZE_MAX) {
            vector_accumulator_safe = false;
            break;
          }
          exit_block = succ;
        }
      }
      size_t vector_preheader = SIZE_MAX;
      bool has_preheader =
          unique_loop_preheader(&cfg, in_loop, header, &vector_preheader);
      bool single_exit =
          exit_block < cfg.block_count &&
          cfg.pred_offsets[exit_block + 1] - cfg.pred_offsets[exit_block] ==
              1 &&
          cfg.pred_blocks[cfg.pred_offsets[exit_block]] == header;
      int64_t exit_target =
          exit_block < cfg.block_count ? cfg.block_label[exit_block] : -1;
      if (exit_target < 0 && exit_block < cfg.block_count) {
        for (size_t i = cfg.block_start[exit_block];
             i < cfg.block_end[exit_block]; ++i) {
          if (f->data[i].op == NYIR_BR && f->data[i].imm >= 0) {
            exit_target = f->data[i].imm;
            break;
          }
        }
      }
      if (nyr_vector_trace_enabled())
        ny_trace_line("VECTORIZE",
                      "vector accumulator CFG: header=%lld body=%lld "
                      "latch=%lld exit=%lld exit_label=%lld single_exit=%s "
                      "preheader=%zu has_preheader=%s",
                      (long long)cfg.block_label[header],
                      (long long)cfg.block_label[body_target],
                      (long long)latch_label,
                      (long long)(exit_block < cfg.block_count
                                      ? cfg.block_label[exit_block]
                                      : -1),
                      (long long)exit_target, single_exit ? "yes" : "no",
                      vector_preheader, has_preheader ? "yes" : "no");
      vector_accumulator_safe =
          vector_accumulator_safe && exit_block < cfg.block_count &&
          exit_target >= 0 && single_exit && has_preheader;
      if (vector_accumulator_safe)
        vector_exit_label = exit_target;
    }
    if (!bad && vector_accumulator_safe) {
      for (size_t u = uses.offsets[reduction_phi_dst];
           u < uses.offsets[reduction_phi_dst + 1]; ++u) {
        size_t user = uses.users[u];
        if (user >= f->len || !in_loop[cfg.inst_block[user]] ||
            user == reduction_update_idx)
          continue;
        /*
         * A loop-carried scalar accumulator is also used by its own PHI
         * incoming edge. This is the expected backedge, not an observable
         * in-loop scalar use.
         */
        if (f->data[user].op == NYIR_PHI &&
            f->data[user].dst == reduction_phi_dst)
          continue;
        const nyr_inst_t *copy = &f->data[user];
        const nyr_inst_t *reduction_update = &f->data[reduction_update_idx];
        bool accumulator_copy = false;
        if (copy->op == NYIR_COPY && copy->dst >= 0 &&
            copy->dst < f->next_value && copy->a == reduction_phi_dst) {
          size_t copy_use_begin = uses.offsets[copy->dst];
          size_t copy_use_end = uses.offsets[copy->dst + 1];
          accumulator_copy =
              copy_use_end == copy_use_begin + 1 &&
              uses.users[copy_use_begin] == reduction_update_idx &&
              (reduction_update->a == copy->dst ||
               reduction_update->b == copy->dst);
        }
        if (accumulator_copy)
          continue;
        vector_accumulator_safe = false;
        break;
      }
    }
    if (reduction_phi_dst >= 0 && !vector_accumulator_safe &&
        nyr_vector_trace_enabled())
      ny_trace_line("VECTORIZE",
                    "vector accumulator rejected after CFG checks: "
                    "reduction=%d init=%d preheader=%lld exit=%lld vf=%d "
                    "trip=%lld variable=%s",
                    reduction_phi_dst, reduction_phi_init,
                    (long long)reduction_preheader_label,
                    (long long)vector_exit_label, vf, (long long)fixed_trip,
                    is_var_trip ? "yes" : "no");
    if (!bad) {
      for (int v = 0; v < f->next_value && !bad; ++v) {
        if (!vec[v])
          continue;
        for (size_t u = uses.offsets[v]; u < uses.offsets[v + 1]; ++u)
          if (!use_allowed(f, &cfg, in_loop, vec, broadcast_value,
                           uses.users[u], reduction_update_idx,
                           reduction_term)) {
            bad = true;
            reject_reason =
                "vector value has an unsupported scalar/out-of-loop consumer";
            reject_pc = uses.users[u];
            break;
          }
      }
    }
    /*
     * A scalar value computed per original iteration must not escape the loop;
     * halving its dynamic execution count would otherwise change observable
     * results even if the memory stream itself vectorizes.
     */
    if (!bad) {
      for (size_t i = head_idx + 1; i < back && !bad; ++i) {
        if (!in_loop[cfg.inst_block[i]])
          continue;
        int v = f->data[i].dst;
        if (v < 0 || v == iv || v == update_v || v == control->a ||
            v == reduction_phi_dst || v == reduction_term ||
            (v < f->next_value && vec[v]))
          continue;
        for (size_t u = uses.offsets[v]; u < uses.offsets[v + 1]; ++u) {
          size_t user = uses.users[u];
          if (user >= f->len || !in_loop[cfg.inst_block[user]]) {
            bad = true;
            reject_reason = "scalar per-iteration value escapes the loop";
            reject_pc = user;
            break;
          }
        }
      }
    }
    nyr_use_def_free(&uses);
    if (bad || (vec_stores == 0 && vec_loads == 0) ||
        !vectorization_profitable(vec_loads, vec_stores, vec_ops, vf,
                                  is_var_trip, estimated_trip)) {
      f->vectorize_rejected_loops++;
      if (bad)
        nyr_vector_trace(
            f, reject_pc, "reject",
            reject_reason ? reject_reason : "legality check failed", vf);
      else if (vec_stores == 0 && vec_loads == 0)
        nyr_vector_trace(f, head_idx, "reject",
                          "loop has no vectorizable memory stream", vf);
      else {
        if (vec_stores || vec_loads) {
          uint64_t vector_cost = vectorization_cost(
              vec_loads, vec_stores, vec_ops, vf, is_var_trip, estimated_trip);
          uint64_t body_cost =
              (uint64_t)(vec_loads + vec_stores) * 2u + vec_ops + 2u;
          uint64_t scalar_cost =
              body_cost && estimated_trip <= UINT64_MAX / body_cost
                  ? estimated_trip * body_cost
                  : UINT64_MAX;
          if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
            ny_dbg_heuristic_event(
                "LOOP_VECTORIZE",
                "cost model pc=%zu decision=reject trip=%llu vf=%d variable=%s "
                "streams=%zu load/%zu store ops=%zu scalar-cost=%llu "
                "vector-cost=%llu",
                head_idx, (unsigned long long)estimated_trip, vf,
                is_var_trip ? "yes" : "no", vec_loads, vec_stores, vec_ops,
                (unsigned long long)scalar_cost,
                (unsigned long long)vector_cost);
        }
        nyr_vector_trace(f, head_idx, "reject",
                          "cost model predicts no saving", vf);
      }
      free(vec);
      free(rewrite);
      free(broadcast_value);
      free(broadcast_map);
      free(in_loop);
      continue;
    }
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC)) {
      uint64_t vector_cost = vectorization_cost(
          vec_loads, vec_stores, vec_ops, vf, is_var_trip, estimated_trip);
      uint64_t body_cost =
          (uint64_t)(vec_loads + vec_stores) * 2u + vec_ops + 2u;
      uint64_t scalar_cost =
          body_cost && estimated_trip <= UINT64_MAX / body_cost
              ? estimated_trip * body_cost
              : UINT64_MAX;
      ny_dbg_heuristic_event(
          "LOOP_VECTORIZE",
          "cost model decision=accept pc=%zu trip=%llu vf=%d variable=%s "
          "streams=%zu load/%zu store ops=%zu scalar-cost=%llu "
          "vector-cost=%llu saving=%llu",
          head_idx, (unsigned long long)estimated_trip, vf,
          is_var_trip ? "yes" : "no", vec_loads, vec_stores, vec_ops,
          (unsigned long long)scalar_cost, (unsigned long long)vector_cost,
          (unsigned long long)(scalar_cost >= vector_cost
                                   ? scalar_cost - vector_cost
                                   : 0));
    }
    nyr_vector_trace(f, head_idx, "accept", "legal and profitable", vf);
    /*
     * Save the scalar body before widening it when a runtime tail is needed.
     * Fixed-trip tails are admitted only after the selected VF is known.
     */
    nyr_inst_t *saved_body = NULL;
    int saved_count = 0;
    int64_t dispatch_label = -1;
    int64_t scalar_label = -1;
    int64_t scalar_latch_label = -1;
    int64_t exit_label = -1;
    size_t preheader_insert_at = SIZE_MAX;
    int vec_end_v = -1;
    int tail_acc_v = -1;
    int vector_unroll_factor = 1;
    if (!is_var_trip && fixed_trip >= (int64_t)(2 * vf) &&
        reduction_phi_dst < 0 && extra_phi_dst < 0 && body_target == latch &&
        vector_unroll_shape(f, &cfg, latch, update_idx)) {
      uint64_t chunks = (uint64_t)fixed_trip / (uint64_t)vf;
      size_t body_inst_count = cfg.block_end[latch] - cfg.block_start[latch];
      if (chunks >= 2) {
        /*
         * Wide floating-point memory bodies keep several address and vector
         * temporaries live at once.  Keep the straight-line group small to
         * limit native register pressure while still amortizing the loop
         * branch over multiple vectors.
         */
        uint64_t max_factor = 6;
        uint64_t factor = chunks < max_factor ? chunks : max_factor;
        if (body_inst_count <= 2048 / (size_t)factor)
          vector_unroll_factor = (int)factor;
      }
    }
    int vector_step = vector_unroll_factor * vf;
    bool needs_scalar_tail =
        is_var_trip || ((uint64_t)fixed_trip % (uint64_t)vector_step != 0);
    bool vector_accumulator = vector_accumulator_safe && !needs_scalar_tail;
    int vector_acc_phi = -1;
    int vector_acc_update = -1;
    int vector_acc_result = -1;
    if (vector_accumulator) {
      vector_acc_phi = f->next_value++;
      vector_acc_update = f->next_value++;
      vector_acc_result = f->next_value++;
    }
    if (needs_scalar_tail) {
      size_t preheader = SIZE_MAX;
      if (!unique_loop_preheader(&cfg, in_loop, header, &preheader) ||
          !fresh_labels(f, &dispatch_label, &scalar_label,
                        &scalar_latch_label)) {
        f->vectorize_rejected_loops++;
        nyr_vector_trace(
            f, head_idx, "reject",
            "scalar-tail loop needs a unique preheader and fresh labels", vf);
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        continue;
      }
      preheader_insert_at = block_insert_before_terminator(f, &cfg, preheader);
      saved_count =
          save_loop_body(f, &cfg, in_loop, head_idx, back, phi_idx, update_idx,
                         cmp_idx, control_idx, &saved_body);
      if (saved_count < 0) {
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        free(defs);
        nyr_cfg_free(&cfg);
        return -1;
      }

      /*
       * Resolve the original scalar exit while CFG instruction indices are
       * still valid.  Later broadcast/IV insertions shift the instruction
       * array, so consulting cfg.block_start/end after mutation is unsafe.
       */
      size_t false_block = SIZE_MAX;
      for (size_t e = cfg.succ_offsets[header];
           e < cfg.succ_offsets[header + 1]; ++e) {
        size_t succ = cfg.succ_blocks[e];
        if (succ != body_target) {
          false_block = succ;
          break;
        }
      }
      if (false_block < cfg.block_count) {
        if (cfg.block_label[false_block] >= 0) {
          exit_label = cfg.block_label[false_block];
        } else {
          size_t start = cfg.block_start[false_block];
          size_t end = cfg.block_end[false_block];
          while (end > start && f->data[end - 1].op == NYIR_NOP)
            --end;
          if (end == start + 1 && f->data[start].op == NYIR_BR)
            exit_label = f->data[start].imm;
        }
      }
      if (reduction_phi_dst >= 0) {
        /*
         * The tail accumulator directly dominates the original exit only when
         * the loop header was its sole predecessor. More complex join exits
         * need an explicit exit PHI and remain scalar for now.
         */
        if (false_block >= cfg.block_count ||
            cfg.pred_offsets[false_block + 1] - cfg.pred_offsets[false_block] !=
                1) {
          free(saved_body);
          free(vec);
          free(rewrite);
          free(broadcast_value);
          free(broadcast_map);
          free(in_loop);
          f->vectorize_rejected_loops++;
          nyr_vector_trace(
              f, head_idx, "reject",
              "scalar-tail reduction exit is not single-predecessor", vf);
          continue;
        }
        tail_acc_v = f->next_value++;
        replace_uses_outside_loop(f, &cfg, in_loop, reduction_phi_dst,
                                  tail_acc_v);
      }
      if (exit_label < 0) {
        free(saved_body);
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        continue;
      }
    }

    size_t broadcast_count = 0;
    for (size_t v = 0; v < scalar_value_count; ++v) {
      if (!broadcast_value[v])
        continue;
      broadcast_map[v] = f->next_value++;
      ++broadcast_count;
    }

    for (size_t i = head_idx + 1; i < back; ++i) {
      if (!in_loop[cfg.inst_block[i]])
        continue;
      nyr_inst_t *in = &f->data[i];
      if (i == reduction_update_idx) {
        if (vector_accumulator) {
          in->op = NYIR_VEC4_ADD_I64;
          in->dst = vector_acc_update;
          in->a = vector_acc_phi;
          in->b = reduction_term;
          in->c = in->d = in->e = in->f = -1;
          in->flags = 0;
          in->effects = nyr_inst_effects(in);
          nyr_clear_derived_facts(in);
          continue;
        }
        in->op = reduction_f64 ? NYIR_VEC4_REDUCE_ADD_F64
                               : (vf == 4 ? NYIR_VEC8_REDUCE_ADD_I64
                                          : NYIR_VEC4_REDUCE_ADD_I64);
        in->a = reduction_phi_dst;
        in->b = reduction_term;
        in->c = in->d = in->e = in->f = -1;
        in->flags = 0;
        in->effects = nyr_inst_effects(in);
        nyr_clear_derived_facts(in);
        continue;
      }
      if (!rewrite[i])
        continue;
      if (in->op == NYIR_LOAD_I64) {
        in->op = vload_op;
        in->flags &= ~NYIR_INST_F_MEM_BYTE;
        in->effects = nyr_inst_effects(in);
        nyr_clear_derived_facts(in);
      } else if (in->op == NYIR_COPY) {
        /*
         * Vector copies keep the same opcode; the type map carries the
         * widened value class from the defining vector operation.
         */
        nyr_clear_derived_facts(in);
      } else if (in->op == NYIR_STORE_I64) {
        in->op = vstore_op;
        in->b = in->c;
        in->c = -1;
        in->flags &= ~NYIR_INST_F_MEM_BYTE;
        in->effects = nyr_inst_effects(in);
        nyr_clear_derived_facts(in);
      } else {
        if (in->a >= 0 && (size_t)in->a < scalar_value_count &&
            broadcast_map[in->a] >= 0)
          in->a = broadcast_map[in->a];
        if (in->b >= 0 && (size_t)in->b < scalar_value_count &&
            broadcast_map[in->b] >= 0)
          in->b = broadcast_map[in->b];
        if (is_f64)
          in->op = vec_f64_bin(in->op);
        else
          in->op = vec_i64_bin(in->op, vf);
        in->flags = 0;
        in->effects = nyr_inst_effects(in);
        nyr_clear_derived_facts(in);
      }
    }

    /*
     * Materialize loop-invariant scalar splats once in the preheader.
     */
    if (broadcast_count > 0) {
      size_t preheader = SIZE_MAX;
      if (!unique_loop_preheader(&cfg, in_loop, header, &preheader)) {
        free(saved_body);
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        continue;
      }
      size_t at = block_insert_before_terminator(f, &cfg, preheader);
      if (!nir_ensure_inst_space(f, broadcast_count)) {
        free(saved_body);
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        free(defs);
        nyr_cfg_free(&cfg);
        return -1;
      }
      memmove(&f->data[at + broadcast_count], &f->data[at],
              (f->len - at) * sizeof(*f->data));
      size_t w = 0;
      for (size_t v = 0; v < scalar_value_count; ++v) {
        if (broadcast_map[v] < 0)
          continue;
        f->data[at + w] = (nyr_inst_t){.op = is_f64 ? NYIR_VEC4_SET1_F64
                                                     : NYIR_VEC4_SET1_I64,
                                        .dst = broadcast_map[v],
                                        .a = (int)v,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1};
        f->data[at + w].effects = nyr_inst_effects(&f->data[at + w]);
        ++w;
      }
      f->len += broadcast_count;
      if (at <= cmp_idx)
        cmp_idx += broadcast_count;
      if (at <= control_idx)
        control_idx += broadcast_count;
      if (at <= update_idx)
        update_idx += broadcast_count;
      if (extra_update_idx != SIZE_MAX && at <= extra_update_idx)
        extra_update_idx += broadcast_count;
      if (needs_scalar_tail && at <= preheader_insert_at)
        preheader_insert_at += broadcast_count;
    }

    size_t vector_clone_count = 0;
    if (vector_unroll_factor > 1) {
      if (!vector_unroll_body(f, latch_label, update_idx, iv, vf,
                              vector_unroll_factor, &vector_clone_count)) {
        free(saved_body);
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        free(defs);
        nyr_cfg_free(&cfg);
        return -1;
      }
      update_idx += vector_clone_count;
      nyr_vector_trace(f, head_idx, "unroll",
                        "unroll fixed-trip straight-line vector body",
                        vector_unroll_factor);
    }

    /*
     * Advance the scalar address IV by the vector width.  Insert the new
     * constant immediately before the recurrence so dominance is obvious and
     * no unrelated constant users are perturbed.
     */
    int two = f->next_value++;
    if (!nir_ensure_inst_space(f, 1)) {
      free(vec);
      free(rewrite);
      free(broadcast_value);
      free(broadcast_map);
      free(in_loop);
      free(defs);
      nyr_cfg_free(&cfg);
      return -1;
    }
    memmove(&f->data[update_idx + 1], &f->data[update_idx],
            (f->len - update_idx) * sizeof(*f->data));
    f->len++;
    f->data[update_idx] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                        .dst = two,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = vector_step};
    nyr_inst_t *moved_update = &f->data[update_idx + 1];
    /*
     * Recognition accepts copy chains around the induction value.  Mirror
     * that proof when replacing the scalar step: testing the raw operand
     * against `iv` rewrote `copy(iv) + 1` into `vf + 1`, dropping the loop
     * recurrence and causing vectorized loops to repeat or run incorrectly.
     */
    if (iv_update_has_iv_on_left)
      moved_update->b = two;
    else
      moved_update->a = two;
    nyr_clear_derived_facts(moved_update);
    for (size_t i = 0; i < f->len; ++i)
      if (f->data[i].op == NYIR_PHI && f->data[i].dst == iv)
        nyr_clear_derived_facts(&f->data[i]);

    if (extra_phi_dst >= 0) {
      if (extra_update_idx >= update_idx)
        extra_update_idx++;
      int wide_step = f->next_value++;
      __int128 scaled = (__int128)extra_step * vf;
      if (scaled < INT64_MIN || scaled > INT64_MAX) {
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        free(defs);
        nyr_cfg_free(&cfg);
        return -1;
      }
      if (!nir_ensure_inst_space(f, 1)) {
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        free(defs);
        nyr_cfg_free(&cfg);
        return -1;
      }
      memmove(&f->data[extra_update_idx + 1], &f->data[extra_update_idx],
              (f->len - extra_update_idx) * sizeof(*f->data));
      f->len++;
      f->data[extra_update_idx] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                                .dst = wide_step,
                                                .a = -1,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1,
                                                .imm = (int64_t)scaled};
      nyr_inst_t *wide_update = &f->data[extra_update_idx + 1];
      if (wide_update->a == extra_phi_dst)
        wide_update->b = wide_step;
      else
        wide_update->a = wide_step;
    }

    if (needs_scalar_tail) {
      /*
       * All CFG-indexed rewrites above used the original instruction map.
       * Only now insert the guarded vector end into the real preheader, then
       * adjust indices that lie after the insertion point.
       */
      vec_end_v =
          emit_vec_end(f, preheader_insert_at, limit_v, init_v, vector_step);
      if (vec_end_v < 0) {
        free(saved_body);
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        free(defs);
        nyr_cfg_free(&cfg);
        return -1;
      }
      size_t shift = NYIR_VEC_END_PREHEADER_LEN;
      if (preheader_insert_at <= cmp_idx)
        cmp_idx += shift;
      if (preheader_insert_at <= control_idx)
        control_idx += shift;
      if (preheader_insert_at <= update_idx)
        update_idx += shift;
      if (extra_update_idx != SIZE_MAX &&
          preheader_insert_at <= extra_update_idx)
        extra_update_idx += shift;
      f->data[cmp_idx].b = vec_end_v;
      nyr_clear_derived_facts(&f->data[cmp_idx]);

      /*
       * Redirect the vector-header false edge through a named dispatch block
       * so the scalar PHI predecessor is an actual CFG label.
       */
      if (!nir_ensure_inst_space(f, 2)) {
        free(saved_body);
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        free(defs);
        nyr_cfg_free(&cfg);
        return -1;
      }
      memmove(&f->data[control_idx + 3], &f->data[control_idx + 1],
              (f->len - control_idx - 1) * sizeof(*f->data));
      f->len += 2;
      f->data[control_idx + 1] = (nyr_inst_t){.op = NYIR_LABEL,
                                               .dst = -1,
                                               .imm = dispatch_label,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1};
      f->data[control_idx + 1].effects =
          nyr_inst_effects(&f->data[control_idx + 1]);
      f->data[control_idx + 2] = (nyr_inst_t){.op = NYIR_BR,
                                               .dst = -1,
                                               .imm = scalar_label,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1};
      f->data[control_idx + 2].effects =
          nyr_inst_effects(&f->data[control_idx + 2]);

      if (!emit_scalar_tail(f, saved_body, saved_count, iv, vec_end_v, limit_v,
                            exit_label, dispatch_label, scalar_label,
                            scalar_latch_label, reduction_phi_dst,
                            reduction_phi_dst, reduction_update_value,
                            tail_acc_v)) {
        free(saved_body);
        free(vec);
        free(rewrite);
        free(broadcast_value);
        free(broadcast_map);
        free(in_loop);
        free(defs);
        nyr_cfg_free(&cfg);
        return -1;
      }
    }
    if (!needs_scalar_tail)
      nyr_clear_derived_facts(&f->data[cmp_idx]);
    if (vector_accumulator &&
        !emit_i64_vector_accumulator(
            f, in_loop, reduction_phi_dst, reduction_phi_init, vector_acc_phi,
            vector_acc_update, vector_acc_result, reduction_preheader_label,
            cfg.block_label[header], latch_label, vector_exit_label)) {
      free(saved_body);
      free(vec);
      free(rewrite);
      free(broadcast_value);
      free(broadcast_map);
      free(in_loop);
      free(defs);
      nyr_cfg_free(&cfg);
      return -1;
    }
    free(saved_body);

    f->vectorized_loops++;
    free(vec);
    free(rewrite);
    free(broadcast_value);
    free(broadcast_map);
    free(in_loop);
    free(defs);
    nyr_cfg_free(&cfg);
    return 1;
  }

  free(defs);
  nyr_cfg_free(&cfg);
  return 0;
}

bool nyr_loop_vectorize(nyr_func_t *f) {
  if (!f || f->len < 8 || f->next_value <= 0)
    return true;
  if (nyr_vectorizer_no_lexical_backedge(f))
    return true;
  nyr_func_t work = {0};
  if (!nyr_ir_func_clone(f, &work))
    return false;
  bool changed = false;
  for (unsigned round = 0; round < 32; ++round) {
    int rc = vectorize_once(&work);
    if (rc < 0) {
      nyr_ir_func_free(&work);
      return false;
    }
    if (rc == 0)
      break;
    changed = true;
  }
  if (!changed) {
    nyr_ir_func_free(&work);
    return true;
  }
  char err[256] = {0};
  if (!nyr_ir_verify(&work, err, sizeof(err))) {
    if (nyr_vector_trace_enabled())
      ny_trace_line("VECTORIZE",
                    "vectorize: rollback; verifier rejected transform: %s",
                    err[0] ? err : "unknown verifier failure");
    nyr_ir_func_free(&work);
    return true; /* reject the speculative transform, preserve input */
  }
  nyr_func_replace(f, &work);
  if (getenv("NY_TRACE_VECTORIZE_DUMP") && *getenv("NY_TRACE_VECTORIZE_DUMP") &&
      ny_dbg_function_selected(NULL))
    nyr_dump(stderr, f, "post-loop-vectorize");
  return true;
}
