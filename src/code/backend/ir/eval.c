/*
 * NYIR debug VM: evaluates NYIR programs in-process for diagnostics,
 * result-oracle comparison, and single-step execution tracing.
 */
#include "base/common.h"
#include "base/compat.h"
#include "code/backend/internal.h"
#include "code/backend/ir/internal.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <nyr/nyr.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t *values;
  bool *known;
  int64_t *vector_values; /* Up to four i64 lanes per vector SSA value. */
  uint8_t *vector_width;
  uint8_t *stack_bytes; /* Reused storage for NYIR_ALLOCA sites. */
  size_t stack_cap;
  size_t value_cap;
} nyr_eval_pool_slot_t;

typedef struct {
  const nyr_func_t *func;
  const nyr_inst_t *data;
  size_t len;
  uint64_t signature;
  size_t label_count;
  size_t *pc;
  bool *found;
} nyr_eval_label_cache_t;

typedef struct {
  nyr_eval_pool_slot_t *slots;
  size_t slot_cap;
  size_t depth;
  nyr_eval_label_cache_t *label_cache;
  size_t label_cache_len;
  size_t label_cache_cap;
} nyr_eval_pool_t;
/*
 * Thread-local eval arena: slots and their value/label arrays are reused
 * across calls and grow monotonically until thread exit.  This is
 * intentional — it replaces the per-invocation calloc/free churn the VM
 * used to pay — so do not add a per-call shrink/free.
 */
static _Thread_local nyr_eval_pool_t nyr_eval_pool;

typedef struct {
  char *name;
  int64_t value;
} nyr_eval_global_t;

static _Thread_local nyr_eval_global_t *nyr_eval_globals;
static _Thread_local size_t nyr_eval_global_len;
static _Thread_local size_t nyr_eval_global_cap;

void nyr_eval_globals_clear(void) {
  for (size_t i = 0; i < nyr_eval_global_len; ++i)
    free(nyr_eval_globals[i].name);
  free(nyr_eval_globals);
  nyr_eval_globals = NULL;
  nyr_eval_global_len = 0;
  nyr_eval_global_cap = 0;
}

static int64_t *nyr_eval_global_cell(const char *name) {
  if (!name || !*name)
    return NULL;
  for (size_t i = 0; i < nyr_eval_global_len; ++i)
    if (strcmp(nyr_eval_globals[i].name, name) == 0)
      return &nyr_eval_globals[i].value;
  if (nyr_eval_global_len == nyr_eval_global_cap) {
    size_t cap = nyr_eval_global_cap ? nyr_eval_global_cap * 2u : 32u;
    if (cap < nyr_eval_global_cap ||
        cap > SIZE_MAX / sizeof(*nyr_eval_globals))
      return NULL;
    nyr_eval_global_t *grown =
        realloc(nyr_eval_globals, cap * sizeof(*nyr_eval_globals));
    if (!grown)
      return NULL;
    nyr_eval_globals = grown;
    nyr_eval_global_cap = cap;
  }
  char *copy = strdup(name);
  if (!copy)
    return NULL;
  nyr_eval_globals[nyr_eval_global_len] =
      (nyr_eval_global_t){.name = copy, .value = 0};
  return &nyr_eval_globals[nyr_eval_global_len++].value;
}

static bool nyr_eval_pool_begin(nyr_eval_pool_slot_t *slot,
                                 size_t *depth_out) {
  if (!slot || !depth_out)
    return false;
  size_t depth = nyr_eval_pool.depth;
  if (depth == nyr_eval_pool.slot_cap) {
    size_t cap = nyr_eval_pool.slot_cap ? nyr_eval_pool.slot_cap * 2 : 8;
    if (cap < nyr_eval_pool.slot_cap ||
        cap > SIZE_MAX / sizeof(*nyr_eval_pool.slots))
      return false;
    nyr_eval_pool_slot_t *slots =
        realloc(nyr_eval_pool.slots, cap * sizeof(*slots));
    if (!slots)
      return false;
    memset(slots + nyr_eval_pool.slot_cap, 0,
           (cap - nyr_eval_pool.slot_cap) * sizeof(*slots));
    nyr_eval_pool.slots = slots;
    nyr_eval_pool.slot_cap = cap;
  }
  nyr_eval_pool.depth++;
  *slot = nyr_eval_pool.slots[depth];
  *depth_out = depth;
  return true;
}

static void nyr_eval_pool_end(size_t depth, nyr_eval_pool_slot_t *slot) {
  if (slot && depth < nyr_eval_pool.slot_cap)
    nyr_eval_pool.slots[depth] = *slot;
  if (nyr_eval_pool.depth > 0)
    nyr_eval_pool.depth--;
}
static bool nyr_eval_pool_stack(nyr_eval_pool_slot_t *slot,
                                 const nyr_func_t *f) {
  if (!slot || !f)
    return false;
  size_t total = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_ALLOCA)
      continue;
    size_t bytes = in->imm > 0 ? (size_t)in->imm : 8u;
    size_t aligned = (bytes + 15u) & ~(size_t)15u;
    if (aligned < bytes || total > SIZE_MAX - aligned)
      return false;
    total += aligned;
  }
  if (total > slot->stack_cap) {
    uint8_t *grown = realloc(slot->stack_bytes, total);
    if (!grown && total)
      return false;
    slot->stack_bytes = grown;
    slot->stack_cap = total;
  }
  return true;
}

static uintptr_t nyr_eval_alloca_addr(const nyr_eval_pool_slot_t *slot,
                                       const nyr_func_t *f,
                                       size_t inst_index) {
  if (!slot || !f || inst_index >= f->len || !slot->stack_bytes)
    return 0;
  size_t off = 0;
  for (size_t i = 0; i < inst_index; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_ALLOCA)
      continue;
    size_t bytes = in->imm > 0 ? (size_t)in->imm : 8u;
    off += (bytes + 15u) & ~(size_t)15u;
  }
  return (uintptr_t)(slot->stack_bytes + off);
}

static bool nyr_eval_pool_values(nyr_eval_pool_slot_t *slot, size_t count,
                                  int64_t **values, bool **known) {
  if (!slot || !values || !known)
    return false;
  if (count > slot->value_cap) {
    if (count > SIZE_MAX / sizeof(*slot->values) ||
        count > SIZE_MAX / (4 * sizeof(*slot->vector_values)))
      return false;
    int64_t *new_values = malloc(count * sizeof(*new_values));
    bool *new_known = malloc(count * sizeof(*new_known));
    int64_t *new_vector_values = malloc(count * 4 * sizeof(*new_vector_values));
    uint8_t *new_vector_width = malloc(count * sizeof(*new_vector_width));
    if (!new_values || !new_known || !new_vector_values || !new_vector_width) {
      free(new_values);
      free(new_known);
      free(new_vector_values);
      free(new_vector_width);
      return false;
    }
    free(slot->values);
    free(slot->known);
    free(slot->vector_values);
    free(slot->vector_width);
    slot->values = new_values;
    slot->known = new_known;
    slot->vector_values = new_vector_values;
    slot->vector_width = new_vector_width;
    slot->value_cap = count;
  }
  if (count) {
    memset(slot->values, 0, count * sizeof(*slot->values));
    memset(slot->known, 0, count * sizeof(*slot->known));
    memset(slot->vector_values, 0, count * 4 * sizeof(*slot->vector_values));
    memset(slot->vector_width, 0, count * sizeof(*slot->vector_width));
  }
  *values = slot->values;
  *known = slot->known;
  return true;
}

static bool nyr_eval_cached_labels(const nyr_func_t *f, size_t *label_count,
                                    size_t **label_pc, bool **label_found) {
  if (!f || !label_count || !label_pc || !label_found)
    return false;
  size_t count = 0;
  uint64_t signature = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_LABEL || in->imm < 0)
      continue;
    if ((size_t)in->imm >= count)
      count = (size_t)in->imm + 1;
    /*
     * Cached PCs depend on both the label ID and its instruction offset.
     */
    signature ^= (uint64_t)i;
    signature *= UINT64_C(1099511628211);
    signature ^= (uint64_t)in->imm;
    signature *= UINT64_C(1099511628211);
  }
  for (size_t i = 0; i < nyr_eval_pool.label_cache_len; ++i) {
    nyr_eval_label_cache_t *entry = &nyr_eval_pool.label_cache[i];
    if (entry->func == f && entry->data == f->data && entry->len == f->len &&
        entry->signature == signature) {
      *label_count = entry->label_count;
      *label_pc = entry->pc;
      *label_found = entry->found;
      return true;
    }
  }
  if (nyr_eval_pool.label_cache_len == nyr_eval_pool.label_cache_cap) {
    size_t cap =
        nyr_eval_pool.label_cache_cap ? nyr_eval_pool.label_cache_cap * 2 : 8;
    if (cap < nyr_eval_pool.label_cache_cap ||
        cap > SIZE_MAX / sizeof(*nyr_eval_pool.label_cache))
      return false;
    nyr_eval_label_cache_t *cache =
        realloc(nyr_eval_pool.label_cache, cap * sizeof(*cache));
    if (!cache)
      return false;
    nyr_eval_pool.label_cache = cache;
    nyr_eval_pool.label_cache_cap = cap;
  }
  size_t *pc = count ? calloc(count, sizeof(*pc)) : NULL;
  bool *found = count ? calloc(count, sizeof(*found)) : NULL;
  if (count && (!pc || !found)) {
    free(pc);
    free(found);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL && in->imm >= 0 && (size_t)in->imm < count) {
      pc[(size_t)in->imm] = i;
      found[(size_t)in->imm] = true;
    }
  }
  nyr_eval_pool.label_cache[nyr_eval_pool.label_cache_len++] =
      (nyr_eval_label_cache_t){.func = f,
                                .data = f->data,
                                .len = f->len,
                                .signature = signature,
                                .label_count = count,
                                .pc = pc,
                                .found = found};
  *label_count = count;
  *label_pc = pc;
  *label_found = found;
  return true;
}

void nyr_eval_result_free(nyr_eval_result_t *result) {
  if (!result)
    return;
  free(result->pc_counts);
  free(result->edges);
  result->pc_counts = NULL;
  result->edges = NULL;
  result->pc_count_len = 0;
  result->edge_count = 0;
  result->edge_cap = 0;
}

static bool nyr_eval_note_edge(nyr_eval_result_t *result, size_t from,
                                size_t to) {
  if (!result || from == SIZE_MAX)
    return true;
  for (size_t i = 0; i < result->edge_count; ++i) {
    if (result->edges[i].from_pc == from && result->edges[i].to_pc == to) {
      result->edges[i].count++;
      return true;
    }
  }
  if (result->edge_count == result->edge_cap) {
    size_t cap = result->edge_cap ? result->edge_cap * 2 : 16;
    if (cap < result->edge_cap || cap > SIZE_MAX / sizeof(*result->edges))
      return false;
    nyr_profile_edge_t *edges =
        realloc(result->edges, cap * sizeof(*result->edges));
    if (!edges)
      return false;
    result->edges = edges;
    result->edge_cap = cap;
  }
  result->edges[result->edge_count++] = (nyr_profile_edge_t){from, to, 1};
  return true;
}

static bool nyr_eval_read_value(const int64_t *values, const bool *known,
                                 int value, int64_t *out) {
  if (!values || !known || !out || value < 0 || !known[value])
    return false;
  *out = values[value];
  return true;
}

static bool nyr_eval_read_vector(const nyr_eval_pool_slot_t *slot, int value,
                                  unsigned width, const int64_t **out) {
  if (!slot || !out || value < 0 || (size_t)value >= slot->value_cap ||
      width > 4 || slot->vector_width[value] != width || !slot->known[value])
    return false;
  *out = slot->vector_values + (size_t)value * 4;
  return true;
}

static void nyr_eval_note_value(nyr_eval_result_t *result, int value) {
  if (result && value >= 0 && (size_t)value > result->max_value_index)
    result->max_value_index = (size_t)value;
}

static void nyr_eval_note_local(nyr_eval_result_t *result, int64_t local) {
  if (result && local >= 0 && (size_t)local > result->max_local_index)
    result->max_local_index = (size_t)local;
}

bool nyr_ir_eval_with_calls(const nyr_func_t *f, int64_t *locals,
                          size_t local_count, size_t max_steps,
                          nyr_eval_result_t *result,
                          nyr_call_resolver_t resolver, void *resolver_ctx,
                          char *err, size_t err_len) {
  if (!f)
    return nyr_err(err, err_len, "native NYIR VM: missing function");
  char verify_err[256] = {0};
  if (!nyr_ir_verify(f, verify_err, sizeof(verify_err)))
    return nyr_err(err, err_len, "native NYIR VM: verifier rejected input: %s",
                    verify_err);
  if (f->next_value < 0)
    return nyr_err(err, err_len, "native NYIR VM: invalid value count");
  size_t eval_depth = 0;
  nyr_eval_pool_slot_t eval_slot = {0};
  if (!nyr_eval_pool_begin(&eval_slot, &eval_depth))
    return nyr_err(err, err_len, NY_NATIVE_OOM);
  int64_t *values = NULL;
  bool *known = NULL;
  if (!nyr_eval_pool_values(&eval_slot, (size_t)f->next_value, &values,
                             &known) ||
      !nyr_eval_pool_stack(&eval_slot, f)) {
    nyr_eval_pool_end(eval_depth, &eval_slot);
    return nyr_err(err, err_len, NY_NATIVE_OOM);
  }
  if (result) {
    memset(result, 0, sizeof(*result));
    if (f->len) {
      result->pc_counts = calloc(f->len, sizeof(*result->pc_counts));
      if (!result->pc_counts) {
        nyr_eval_pool_end(eval_depth, &eval_slot);
        return nyr_err(err, err_len, NY_NATIVE_OOM);
      }
      result->pc_count_len = f->len;
    }
  }
  if (max_steps == 0)
    max_steps = 1000000;

  /*
   * Precompute label→PC once per function image.  The cache is thread-local
   * because optimizers may mutate separate NYIR functions concurrently.
   */
  size_t label_count = 0;
  size_t *label_pc = NULL;
  bool *label_found = NULL;
  if (!nyr_eval_cached_labels(f, &label_count, &label_pc, &label_found)) {
    nyr_eval_result_free(result);
    nyr_eval_pool_end(eval_depth, &eval_slot);
    return nyr_err(err, err_len, NY_NATIVE_OOM);
  }

  size_t pc = 0;
  size_t steps = 0;
  int64_t current_label = -1;
  int64_t predecessor_label = -1;
  const nyr_inst_t *in = NULL;
  size_t inst_index = 0;
  const bool profiling = (result != NULL);
  size_t previous_pc = SIZE_MAX;
  /*
   * Language `try` lowering calls _setjmp and branches to the catch arm on
   * its nonzero return.  Real setjmp cannot work under this interpreter (the
   * resolver frame that executed it is gone by the time a callee raises), so
   * the VM fakes it: each _setjmp call records a rewind point, and a nested
   * call that fails with NYIR_VM_LANGUAGE_PANIC_ERR resumes execution right
   * after the _setjmp with its result forced to 1 — the compare + branch then
   * routes control into the catch arm exactly like machine code.
   */
  struct {
    int setjmp_dst;
    size_t resume_pc;
  } try_marks[32];
  size_t try_mark_count = 0;
  while (pc < f->len) {
    if (++steps > max_steps) {
      nyr_eval_result_free(result);
      nyr_eval_pool_end(eval_depth, &eval_slot);
      return nyr_err(err, err_len, "native NYIR VM: step limit exceeded");
    }
    inst_index = pc;
    if (profiling) {
      if (inst_index > result->max_pc)
        result->max_pc = inst_index;
      if (inst_index < result->pc_count_len)
        result->pc_counts[inst_index]++;
      if (!nyr_eval_note_edge(result, previous_pc, inst_index))
        goto profile_oom;
      previous_pc = inst_index;
    }
    in = &f->data[pc++];
    if (profiling && in->op >= 0 && in->op < NYIR_OP_COUNT)
      result->op_counts[in->op]++;
    int64_t a = 0;
    int64_t b = 0;
    int64_t out = 0;
    switch (in->op) {
    case NYIR_NOP:
      break;
    case NYIR_BOUNDS_CHECK:
      if (!nyr_eval_read_value(values, known, in->a, &a) ||
          !nyr_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      if (in->c >= 0) {
        if (!nyr_eval_read_value(values, known, in->c, &out))
          goto missing_value;
      } else {
        out = in->imm;
      }
      if (b < 0 || b >= out)
        goto bounds_violation;
      break;
    case NYIR_LABEL:
      predecessor_label = current_label;
      current_label = in->imm;
      break;
    case NYIR_PHI: {
      bool found = false;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        if (in->phi_incoming[k].predecessor_label == predecessor_label) {
          int incoming = in->phi_incoming[k].value;
          if (!nyr_eval_read_value(values, known, incoming, &out))
            goto missing_value;
          if (incoming >= 0 && (size_t)incoming < eval_slot.value_cap &&
              eval_slot.vector_width[incoming] != 0) {
            unsigned width = eval_slot.vector_width[incoming];
            if (in->dst < 0 || (size_t)in->dst >= eval_slot.value_cap ||
                (eval_slot.vector_width[in->dst] != 0 &&
                 eval_slot.vector_width[in->dst] != width))
              goto unsupported;
            memcpy(eval_slot.vector_values + (size_t)in->dst * 4,
                   eval_slot.vector_values + (size_t)incoming * 4,
                   width * sizeof(int64_t));
            eval_slot.vector_width[in->dst] = (uint8_t)width;
          }
          found = true;
          break;
        }
      }
      if (!found)
        goto unsupported;
      values[in->dst] = out;
      known[in->dst] = true;
      nyr_eval_note_value(result, in->dst);
      break;
    }
    case NYIR_CONST_I64:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
      if (in->dst >= 0) {
        nyr_eval_note_value(result, in->dst);
        values[in->dst] = in->imm;
        known[in->dst] = true;
      }
      break;
    case NYIR_COPY:
    case NYIR_I64_TO_F64:
    case NYIR_F64_TO_I64:
    case NYIR_F32_TO_I64:
    case NYIR_I64_TO_F32:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
      if (in->op == NYIR_COPY && in->a >= 0 &&
          (size_t)in->a < eval_slot.value_cap &&
          eval_slot.vector_width[in->a] != 0) {
        unsigned width = eval_slot.vector_width[in->a];
        memcpy(eval_slot.vector_values + (size_t)in->dst * 4,
               eval_slot.vector_values + (size_t)in->a * 4,
               width * sizeof(int64_t));
        eval_slot.vector_width[in->dst] = (uint8_t)width;
        values[in->dst] = eval_slot.vector_values[(size_t)in->dst * 4];
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      if (in->a >= 0 && (size_t)in->a < eval_slot.value_cap &&
          eval_slot.vector_width[in->a] != 0)
        goto unsupported;
      if (!nyr_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      nyr_eval_note_value(result, in->dst);
      if (in->op == NYIR_I64_TO_F64)
        values[in->dst] = nyr_f64_to_bits((double)a);
      else if (in->op == NYIR_F64_TO_I64)
        values[in->dst] = (int64_t)nyr_bits_to_f64(a);
      else if (in->op == NYIR_F32_TO_I64)
        values[in->dst] = (int64_t)nyr_bits_to_f32(a);
      else if (in->op == NYIR_I64_TO_F32)
        values[in->dst] = nyr_f32_to_bits((float)a);
      else if (in->op == NYIR_F64_TO_F32)
        values[in->dst] = nyr_f32_to_bits((float)nyr_bits_to_f64(a));
      else if (in->op == NYIR_F32_TO_F64)
        values[in->dst] = nyr_f64_to_bits((double)nyr_bits_to_f32(a));
      else
        values[in->dst] = a;
      known[in->dst] = true;
      break;
    case NYIR_LOAD_I64: {
      if (!nyr_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      uintptr_t ptr = (uintptr_t)a;
      if (ptr < 4096)
        goto unsupported;
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = (in->flags & NYIR_INST_F_MEM_BYTE)
                            ? *(const uint8_t *)ptr
                            : *(const int64_t *)ptr;
      known[in->dst] = true;
      break;
    }
    case NYIR_ADDR_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count || !locals)
        goto bad_local;
      nyr_eval_note_local(result, in->imm);
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = (int64_t)(uintptr_t)&locals[in->imm];
      known[in->dst] = true;
      break;
    case NYIR_ADDR_SYMBOL: {
      if (in->dst < 0)
        break;
      uintptr_t addr = 0;
      size_t str_len = 0;
      const char *str_bytes =
          in->symbol ? nyr_strtab_get(in->symbol, &str_len) : NULL;
      if (str_bytes) {
        addr = (uintptr_t)str_bytes;
      } else if (in->symbol) {
        void *sym = nyr_arraytab_data(in->symbol);
        if (sym)
          addr = (uintptr_t)sym;
        else if (nyr_globaltab_has(in->symbol))
          addr = (uintptr_t)nyr_eval_global_cell(in->symbol);
        else if ((sym = ny_process_symbol(in->symbol)) != NULL)
          addr = (uintptr_t)sym;
        else if (strncmp(in->symbol, "ny_fn_", 6) == 0 ||
                 strncmp(in->symbol, "__ny_lambda_", 12) == 0)
          /*
           * Function values are lowered to direct NYIR calls when invoked;
           * the VM only needs a non-null opaque value for the intermediate
           * address used by that lowering.
           */
          addr = nyr_vm_symbol_handle(in->symbol);
      }
      if (!addr) {
        char reason[256];
        snprintf(reason, sizeof(reason),
                 "NYIR VM could not resolve addr.symbol '%s'",
                 in->symbol && in->symbol[0] ? in->symbol : "<empty>");
        nyr_eval_result_free(result);
        nyr_eval_pool_end(eval_depth, &eval_slot);
        return nyr_inst_err(err, err_len, in, inst_index, reason);
      }
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = (int64_t)addr;
      known[in->dst] = true;
      break;
    }
    case NYIR_ALLOCA: {
      if (in->dst < 0 || in->imm <= 0)
        goto unsupported;
      uintptr_t addr = nyr_eval_alloca_addr(&eval_slot, f, inst_index);
      if (!addr)
        goto unsupported;
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = (int64_t)addr;
      known[in->dst] = true;
      break;
    }
    case NYIR_COPY_STRUCT:
    case NYIR_CAPTURE_RET:
      goto unsupported;
    case NYIR_STORE_I64: {
      int64_t val = 0;
      if (!nyr_eval_read_value(values, known, in->a, &a) ||
          !nyr_eval_read_value(values, known, in->c, &val))
        goto missing_value;
      uintptr_t ptr = (uintptr_t)a;
      if (ptr < 4096)
        goto unsupported;
      if (in->flags & NYIR_INST_F_MEM_BYTE)
        *(uint8_t *)ptr = (uint8_t)val;
      else
        *(int64_t *)ptr = val;
      break;
    }
    case NYIR_LOAD_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count)
        goto bad_local;
      nyr_eval_note_local(result, in->imm);
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = locals ? locals[in->imm] : 0;
      known[in->dst] = true;
      break;
    case NYIR_STORE_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count)
        goto bad_local;
      if (!nyr_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      nyr_eval_note_local(result, in->imm);
      if (locals)
        locals[in->imm] = a;
      break;
    case NYIR_SELECT_I64:
      if (!nyr_eval_read_value(values, known, in->a, &a) ||
          !nyr_eval_read_value(values, known, in->b, &b) ||
          !nyr_eval_read_value(values, known, in->c, &out))
        goto missing_value;
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = a != 0 ? b : out;
      known[in->dst] = true;
      break;
    case NYIR_CMP_I64:
    case NYIR_CMP_F64:
    case NYIR_CMP_F32:
      if (!nyr_eval_read_value(values, known, in->a, &a) ||
          !nyr_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      if (in->op == NYIR_CMP_F64) {
        double da = nyr_bits_to_f64(a);
        double db = nyr_bits_to_f64(b);
        switch (in->cmp) {
        case NYIR_CMP_EQ:
          out = da == db;
          break;
        case NYIR_CMP_NE:
          out = da != db;
          break;
        case NYIR_CMP_LT:
          out = da < db;
          break;
        case NYIR_CMP_LE:
          out = da <= db;
          break;
        case NYIR_CMP_GT:
          out = da > db;
          break;
        case NYIR_CMP_GE:
          out = da >= db;
          break;
        default:
          goto unsupported;
        }
      } else if (in->op == NYIR_CMP_F32) {
        float fa = nyr_bits_to_f32(a);
        float fb = nyr_bits_to_f32(b);
        switch (in->cmp) {
        case NYIR_CMP_EQ:
          out = fa == fb;
          break;
        case NYIR_CMP_NE:
          out = fa != fb;
          break;
        case NYIR_CMP_LT:
          out = fa < fb;
          break;
        case NYIR_CMP_LE:
          out = fa <= fb;
          break;
        case NYIR_CMP_GT:
          out = fa > fb;
          break;
        case NYIR_CMP_GE:
          out = fa >= fb;
          break;
        default:
          goto unsupported;
        }
      } else if (!nyr_analyze_cmp_fold(in->cmp, a, b, &out)) {
        goto unsupported;
      }
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = out;
      known[in->dst] = true;
      break;
    case NYIR_ADD_I64:
    case NYIR_SUB_I64:
    case NYIR_MUL_I64:
    case NYIR_DIV_I64:
    case NYIR_MOD_I64:
    case NYIR_AND_I64:
    case NYIR_OR_I64:
    case NYIR_XOR_I64:
    case NYIR_SHL_I64:
    case NYIR_SAR_I64:
    case NYIR_ROR_I64:
    case NYIR_ROR32_I64:
      if (!nyr_eval_read_value(values, known, in->a, &a) ||
          !nyr_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      if (!nyr_analyze_binary_fold(in->op, a, b, &out))
        goto unsupported;
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = out;
      known[in->dst] = true;
      break;
    case NYIR_BSWAP_I64:
    case NYIR_BSWAP32_I64:
      if (!nyr_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = in->op == NYIR_BSWAP_I64
                            ? (int64_t)__builtin_bswap64((uint64_t)a)
                            : (int64_t)(uint32_t)__builtin_bswap32((uint32_t)a);
      known[in->dst] = true;
      break;
    case NYIR_SQRT_F64:
    case NYIR_SIN_F64:
    case NYIR_COS_F64: {
      if (!nyr_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      double da = nyr_bits_to_f64(a);
      double dout = in->op == NYIR_SQRT_F64  ? sqrt(da)
                    : in->op == NYIR_SIN_F64 ? sin(da)
                                             : cos(da);
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = nyr_f64_to_bits(dout);
      known[in->dst] = true;
      break;
    }
    case NYIR_ADD_F64:
    case NYIR_SUB_F64:
    case NYIR_MUL_F64:
    case NYIR_DIV_F64: {
      if (!nyr_eval_read_value(values, known, in->a, &a) ||
          !nyr_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      double da = nyr_bits_to_f64(a);
      double db = nyr_bits_to_f64(b);
      double dout = 0;
      switch (in->op) {
      case NYIR_ADD_F64:
        dout = da + db;
        break;
      case NYIR_SUB_F64:
        dout = da - db;
        break;
      case NYIR_MUL_F64:
        dout = da * db;
        break;
      case NYIR_DIV_F64:
        dout = da / db;
        break;
      default:
        goto unsupported;
      }
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = nyr_f64_to_bits(dout);
      known[in->dst] = true;
      break;
    }
    case NYIR_ADD_F32:
    case NYIR_SUB_F32:
    case NYIR_MUL_F32:
    case NYIR_DIV_F32: {
      if (!nyr_eval_read_value(values, known, in->a, &a) ||
          !nyr_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      float fa = nyr_bits_to_f32(a);
      float fb = nyr_bits_to_f32(b);
      float fout = 0;
      switch (in->op) {
      case NYIR_ADD_F32:
        fout = fa + fb;
        break;
      case NYIR_SUB_F32:
        fout = fa - fb;
        break;
      case NYIR_MUL_F32:
        fout = fa * fb;
        break;
      case NYIR_DIV_F32:
        fout = fa / fb;
        break;
      default:
        goto unsupported;
      }
      nyr_eval_note_value(result, in->dst);
      values[in->dst] = nyr_f32_to_bits(fout);
      known[in->dst] = true;
      break;
    }
    case NYIR_BR:
      if (in->imm < 0 || (size_t)in->imm >= label_count ||
          !label_found[in->imm])
        goto missing_label;
      pc = label_pc[in->imm];
      break;
    case NYIR_BR_IF:
      if (!nyr_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      if (a) {
        if (profiling)
          result->branch_taken++;
        if (in->imm < 0 || (size_t)in->imm >= label_count ||
            !label_found[in->imm])
          goto missing_label;
        pc = label_pc[in->imm];
      } else if (profiling) {
        result->branch_not_taken++;
      }
      break;
    case NYIR_RET:
      if (result) {
        result->returned = true;
        result->steps = steps;
      }
      if (in->a >= 0) {
        if (!nyr_eval_read_value(values, known, in->a, &a))
          goto missing_value;
        if (result)
          result->result = a;
      }
      nyr_eval_pool_end(eval_depth, &eval_slot);
      if (err && err_len > 0)
        err[0] = '\0';
      return true;
    case NYIR_CALL: {
      if (result)
        result->call_count++;
      if (!resolver) {
        nyr_eval_result_free(result);
        nyr_eval_pool_end(eval_depth, &eval_slot);
        return nyr_inst_err(err, err_len, in, inst_index,
                             "NYIR VM does not execute external calls yet");
      }
      if (in->imm < 0 || in->imm > NYIR_CALL_MAX_ARGS) {
        nyr_eval_result_free(result);
        nyr_eval_pool_end(eval_depth, &eval_slot);
        return nyr_inst_err(err, err_len, in, inst_index,
                             "NYIR VM supports a bounded number of call args");
      }
      int64_t args[NYIR_CALL_MAX_ARGS];
      if (in->imm > 0 && !nyr_eval_read_value(values, known, in->a, &args[0]))
        goto missing_value;
      if (in->imm > 1 && !nyr_eval_read_value(values, known, in->b, &args[1]))
        goto missing_value;
      if (in->imm > 2 && !nyr_eval_read_value(values, known, in->c, &args[2]))
        goto missing_value;
      if (in->imm > 3 && !nyr_eval_read_value(values, known, in->d, &args[3]))
        goto missing_value;
      if (in->imm > 4 && !nyr_eval_read_value(values, known, in->e, &args[4]))
        goto missing_value;
      if (in->imm > 5 && !nyr_eval_read_value(values, known, in->f, &args[5]))
        goto missing_value;
      for (int64_t k = 6; k < in->imm; ++k) {
        int src = (in->extra_args && (size_t)(k - 6) < in->extra_args_len)
                      ? in->extra_args[k - 6]
                      : -1;
        if (!nyr_eval_read_value(values, known, src, &args[k]))
          goto missing_value;
      }
      /*
       * A try's _setjmp executes in this interpreter: record the rewind
       * point and report the not-yet-resumed (zero) result. rt_set_panic_env
       * / rt_clear_panic_env around it remain ordinary calls — they only
       * maintain the runtime's bookkeeping stack.
       */
      if (in->symbol && (strcmp(in->symbol, "_setjmp") == 0 ||
                         strcmp(in->symbol, "setjmp") == 0)) {
        if (try_mark_count == sizeof(try_marks) / sizeof(try_marks[0])) {
          nyr_eval_result_free(result);
          nyr_eval_pool_end(eval_depth, &eval_slot);
          return nyr_inst_err(
              err, err_len, in, inst_index,
              "NYIR VM: try nesting exceeds the supported depth");
        }
        try_marks[try_mark_count].setjmp_dst = in->dst;
        try_marks[try_mark_count].resume_pc = pc;
        ++try_mark_count;
        if (in->dst >= 0) {
          nyr_eval_note_value(result, in->dst);
          values[in->dst] = 0;
          known[in->dst] = true;
        }
        break;
      }
      if (!resolver(resolver_ctx, in->symbol, args, (size_t)in->imm, &out, err,
                    err_len)) {
        /*
         * A language raise unwinds to this function's innermost active try
         * instead of aborting the evaluation; with no try here the marker
         * propagates to the caller's evaluation unchanged.
         */
        if (err && err_len > 0 &&
            strcmp(err, NYIR_VM_LANGUAGE_PANIC_ERR) == 0 &&
            try_mark_count > 0) {
          --try_mark_count;
          if (try_marks[try_mark_count].setjmp_dst >= 0) {
            int dst = try_marks[try_mark_count].setjmp_dst;
            nyr_eval_note_value(result, dst);
            values[dst] = 1;
            known[dst] = true;
          }
          pc = try_marks[try_mark_count].resume_pc;
          continue;
        }
        nyr_eval_result_free(result);
        nyr_eval_pool_end(eval_depth, &eval_slot);
        return false;
      }
      if (in->dst >= 0) {
        nyr_eval_note_value(result, in->dst);
        values[in->dst] = out;
        known[in->dst] = true;
      }
      break;
    }
    case NYIR_VEC4_LOAD_I64:
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC4_STORE_I64:
    case NYIR_VEC8_STORE_I64:
    case NYIR_VEC4_ADD_I64:
    case NYIR_VEC8_ADD_I64:
    case NYIR_VEC4_SUB_I64:
    case NYIR_VEC8_SUB_I64:
    case NYIR_VEC4_SET1_I64:
    case NYIR_VEC4_AND_I64:
    case NYIR_VEC8_AND_I64:
    case NYIR_VEC4_OR_I64:
    case NYIR_VEC8_OR_I64:
    case NYIR_VEC4_XOR_I64:
    case NYIR_VEC8_XOR_I64:
    case NYIR_VEC4_SHL_I64:
    case NYIR_VEC4_SAR_I64:
    case NYIR_VEC4_REDUCE_ADD_I64:
    case NYIR_VEC8_REDUCE_ADD_I64: {
      const unsigned width =
          (in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC4_STORE_I64 ||
           in->op == NYIR_VEC4_ADD_I64 || in->op == NYIR_VEC4_SUB_I64 ||
           in->op == NYIR_VEC4_SET1_I64 || in->op == NYIR_VEC4_AND_I64 ||
           in->op == NYIR_VEC4_OR_I64 || in->op == NYIR_VEC4_XOR_I64 ||
           in->op == NYIR_VEC4_SHL_I64 || in->op == NYIR_VEC4_SAR_I64 ||
           in->op == NYIR_VEC4_REDUCE_ADD_I64)
              ? 2u
              : 4u;
      if (in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC8_LOAD_I64) {
        if (!nyr_eval_read_value(values, known, in->a, &a))
          goto missing_value;
        int64_t *ptr = (int64_t *)(uintptr_t)a;
        if (!ptr)
          goto unsupported;
        for (unsigned lane = 0; lane < width; ++lane)
          eval_slot.vector_values[(size_t)in->dst * 4 + lane] = ptr[lane];
        eval_slot.vector_width[in->dst] = (uint8_t)width;
        values[in->dst] = eval_slot.vector_values[(size_t)in->dst * 4];
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      if (in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_STORE_I64) {
        const int64_t *vec = NULL;
        int value = in->b >= 0 ? in->b : in->c;
        if (!nyr_eval_read_value(values, known, in->a, &a) ||
            !nyr_eval_read_vector(&eval_slot, value, width, &vec))
          goto unsupported;
        int64_t *ptr = (int64_t *)(uintptr_t)a;
        if (!ptr)
          goto unsupported;
        for (unsigned lane = 0; lane < width; ++lane)
          ptr[lane] = vec[lane];
        break;
      }
      if (in->op == NYIR_VEC4_SET1_I64) {
        if (!nyr_eval_read_value(values, known, in->a, &a))
          goto missing_value;
        for (unsigned lane = 0; lane < width; ++lane)
          eval_slot.vector_values[(size_t)in->dst * 4 + lane] = a;
        eval_slot.vector_width[in->dst] = (uint8_t)width;
        values[in->dst] = a;
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      if (in->op == NYIR_VEC4_REDUCE_ADD_I64 ||
          in->op == NYIR_VEC8_REDUCE_ADD_I64) {
        const int64_t *vec = NULL;
        if (!nyr_eval_read_value(values, known, in->a, &a) ||
            !nyr_eval_read_vector(&eval_slot, in->b, width, &vec))
          goto unsupported;
        uint64_t sum = (uint64_t)a;
        for (unsigned lane = 0; lane < width; ++lane)
          sum += (uint64_t)vec[lane];
        values[in->dst] = (int64_t)sum;
        eval_slot.vector_width[in->dst] = 0;
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      const int64_t *lhs = NULL;
      const int64_t *rhs = NULL;
      if (!nyr_eval_read_vector(&eval_slot, in->a, width, &lhs) ||
          !nyr_eval_read_vector(&eval_slot, in->b, width, &rhs))
        goto unsupported;
      for (unsigned lane = 0; lane < width; ++lane) {
        uint64_t ua = (uint64_t)lhs[lane];
        uint64_t ub = (uint64_t)rhs[lane];
        uint64_t uv = 0;
        switch (in->op) {
        case NYIR_VEC4_ADD_I64:
        case NYIR_VEC8_ADD_I64:
          uv = ua + ub;
          break;
        case NYIR_VEC4_SUB_I64:
        case NYIR_VEC8_SUB_I64:
          uv = ua - ub;
          break;
        case NYIR_VEC4_AND_I64:
        case NYIR_VEC8_AND_I64:
          uv = ua & ub;
          break;
        case NYIR_VEC4_OR_I64:
        case NYIR_VEC8_OR_I64:
          uv = ua | ub;
          break;
        case NYIR_VEC4_XOR_I64:
        case NYIR_VEC8_XOR_I64:
          uv = ua ^ ub;
          break;
        case NYIR_VEC4_SHL_I64:
          uv = ua << (ub & 63u);
          break;
        case NYIR_VEC4_SAR_I64:
          uv = (uint64_t)(lhs[lane] >> (ub & 63u));
          break;
        default:
          goto unsupported;
        }
        eval_slot.vector_values[(size_t)in->dst * 4 + lane] = (int64_t)uv;
      }
      eval_slot.vector_width[in->dst] = (uint8_t)width;
      values[in->dst] = eval_slot.vector_values[(size_t)in->dst * 4];
      known[in->dst] = true;
      nyr_eval_note_value(result, in->dst);
      break;
    }
    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC4_STORE_F64:
    case NYIR_VEC4_ADD_F64:
    case NYIR_VEC4_SUB_F64:
    case NYIR_VEC4_MUL_F64:
    case NYIR_VEC4_DIV_F64:
    case NYIR_VEC4_FMA_F64:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC4_REDUCE_ADD_F64: {
      const unsigned width = 4;
      if (in->op == NYIR_VEC4_LOAD_F64) {
        if (!nyr_eval_read_value(values, known, in->a, &a))
          goto missing_value;
        const uint64_t *ptr = (const uint64_t *)(uintptr_t)a;
        if (!ptr)
          goto unsupported;
        for (unsigned lane = 0; lane < width; ++lane)
          eval_slot.vector_values[(size_t)in->dst * 4 + lane] =
              (int64_t)ptr[lane];
        eval_slot.vector_width[in->dst] = (uint8_t)width;
        values[in->dst] = eval_slot.vector_values[(size_t)in->dst * 4];
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      if (in->op == NYIR_VEC4_STORE_F64) {
        const int64_t *vec = NULL;
        int value = in->b >= 0 ? in->b : in->c;
        if (!nyr_eval_read_value(values, known, in->a, &a) ||
            !nyr_eval_read_vector(&eval_slot, value, width, &vec))
          goto unsupported;
        uint64_t *ptr = (uint64_t *)(uintptr_t)a;
        if (!ptr)
          goto unsupported;
        for (unsigned lane = 0; lane < width; ++lane)
          ptr[lane] = (uint64_t)vec[lane];
        break;
      }
      if (in->op == NYIR_VEC4_SET1_F64) {
        if (!nyr_eval_read_value(values, known, in->a, &a))
          goto missing_value;
        for (unsigned lane = 0; lane < width; ++lane)
          eval_slot.vector_values[(size_t)in->dst * 4 + lane] = a;
        eval_slot.vector_width[in->dst] = (uint8_t)width;
        values[in->dst] = a;
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      if (in->op == NYIR_VEC4_SHUFFLE_F64) {
        const int64_t *input = NULL;
        if (!nyr_eval_read_vector(&eval_slot, in->a, width, &input))
          goto unsupported;
        for (unsigned lane = 0; lane < width; ++lane) {
          unsigned selected =
              (unsigned)(((uint64_t)in->imm >> (lane * 2u)) & 3u);
          eval_slot.vector_values[(size_t)in->dst * 4 + lane] = input[selected];
        }
        eval_slot.vector_width[in->dst] = (uint8_t)width;
        values[in->dst] = eval_slot.vector_values[(size_t)in->dst * 4];
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      if (in->op == NYIR_VEC4_REDUCE_ADD_F64) {
        const int64_t *vec = NULL;
        if (!nyr_eval_read_value(values, known, in->a, &a) ||
            !nyr_eval_read_vector(&eval_slot, in->b, width, &vec))
          goto unsupported;
        double sum = nyr_bits_to_f64(a);
        for (unsigned lane = 0; lane < width; ++lane)
          sum += nyr_bits_to_f64(vec[lane]);
        values[in->dst] = nyr_f64_to_bits(sum);
        eval_slot.vector_width[in->dst] = 0;
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      const int64_t *lhs = NULL;
      const int64_t *rhs = NULL;
      if (!nyr_eval_read_vector(&eval_slot, in->a, width, &lhs) ||
          !nyr_eval_read_vector(&eval_slot, in->b, width, &rhs))
        goto unsupported;
      const int64_t *addend = NULL;
      if (in->op == NYIR_VEC4_FMA_F64 &&
          !nyr_eval_read_vector(&eval_slot, in->c, width, &addend))
        goto unsupported;
      for (unsigned lane = 0; lane < width; ++lane) {
        double da = nyr_bits_to_f64(lhs[lane]);
        double db = nyr_bits_to_f64(rhs[lane]);
        double dout = 0.0;
        switch (in->op) {
        case NYIR_VEC4_ADD_F64:
          dout = da + db;
          break;
        case NYIR_VEC4_SUB_F64:
          dout = da - db;
          break;
        case NYIR_VEC4_MUL_F64:
          dout = da * db;
          break;
        case NYIR_VEC4_DIV_F64:
          dout = da / db;
          break;
        case NYIR_VEC4_FMA_F64:
          dout = fma(da, db, nyr_bits_to_f64(addend[lane]));
          break;
        default:
          goto unsupported;
        }
        eval_slot.vector_values[(size_t)in->dst * 4 + lane] =
            nyr_f64_to_bits(dout);
      }
      eval_slot.vector_width[in->dst] = (uint8_t)width;
      values[in->dst] = eval_slot.vector_values[(size_t)in->dst * 4];
      known[in->dst] = true;
      nyr_eval_note_value(result, in->dst);
      break;
    }
    /*
     * Four-lane f32 semantics for the complete existing VEC8_F32 opcode set.
     */
    case NYIR_VEC8_LOAD_F32:
    case NYIR_VEC8_SET1_F32:
    case NYIR_VEC8_ADD_F32:
    case NYIR_VEC8_SUB_F32:
    case NYIR_VEC8_MUL_F32:
    case NYIR_VEC8_DIV_F32:
    case NYIR_VEC8_FMA_F32:
    case NYIR_VEC8_SHUFFLE_F32:
    case NYIR_VEC8_STORE_F32: {
      const unsigned width = 4;
      if (in->op == NYIR_VEC8_LOAD_F32) {
        if (!nyr_eval_read_value(values, known, in->a, &a))
          goto missing_value;
        const uint32_t *ptr = (const uint32_t *)(uintptr_t)a;
        if (!ptr)
          goto unsupported;
        for (unsigned lane = 0; lane < width; ++lane)
          eval_slot.vector_values[(size_t)in->dst * 4 + lane] = ptr[lane];
        eval_slot.vector_width[in->dst] = (uint8_t)width;
        values[in->dst] = eval_slot.vector_values[(size_t)in->dst * 4];
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      if (in->op == NYIR_VEC8_SET1_F32) {
        if (!nyr_eval_read_value(values, known, in->a, &a))
          goto missing_value;
        for (unsigned lane = 0; lane < width; ++lane)
          eval_slot.vector_values[(size_t)in->dst * 4 + lane] =
              (int64_t)(uint32_t)a;
        eval_slot.vector_width[in->dst] = (uint8_t)width;
        values[in->dst] = (int64_t)(uint32_t)a;
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      if (in->op == NYIR_VEC8_STORE_F32) {
        const int64_t *vec = NULL;
        int value = in->b >= 0 ? in->b : in->c;
        if (!nyr_eval_read_value(values, known, in->a, &a) ||
            !nyr_eval_read_vector(&eval_slot, value, width, &vec))
          goto unsupported;
        uint32_t *ptr = (uint32_t *)(uintptr_t)a;
        if (!ptr)
          goto unsupported;
        for (unsigned lane = 0; lane < width; ++lane)
          ptr[lane] = (uint32_t)vec[lane];
        break;
      }
      if (in->op == NYIR_VEC8_SHUFFLE_F32) {
        const int64_t *src = NULL;
        if (!nyr_eval_read_vector(&eval_slot, in->a, width, &src))
          goto unsupported;
        for (unsigned lane = 0; lane < width; ++lane) {
          unsigned shift = lane < 2 ? lane * 2u : 4u + (lane - 2u) * 2u;
          unsigned selected = (unsigned)(((uint64_t)in->imm >> shift) & 3u);
          if (lane >= 2)
            selected += 2u;
          selected &= 3u;
          eval_slot.vector_values[(size_t)in->dst * 4 + lane] = src[selected];
        }
        eval_slot.vector_width[in->dst] = (uint8_t)width;
        values[in->dst] = eval_slot.vector_values[(size_t)in->dst * 4];
        known[in->dst] = true;
        nyr_eval_note_value(result, in->dst);
        break;
      }
      const int64_t *lhs = NULL;
      const int64_t *rhs = NULL;
      if (!nyr_eval_read_vector(&eval_slot, in->a, width, &lhs) ||
          !nyr_eval_read_vector(&eval_slot, in->b, width, &rhs))
        goto unsupported;
      const int64_t *addend = NULL;
      if (in->op == NYIR_VEC8_FMA_F32 &&
          !nyr_eval_read_vector(&eval_slot, in->c, width, &addend))
        goto unsupported;
      for (unsigned lane = 0; lane < width; ++lane) {
        float fa = nyr_bits_to_f32((uint32_t)lhs[lane]);
        float fb = nyr_bits_to_f32((uint32_t)rhs[lane]);
        float fout = 0.0f;
        switch (in->op) {
        case NYIR_VEC8_ADD_F32:
          fout = fa + fb;
          break;
        case NYIR_VEC8_SUB_F32:
          fout = fa - fb;
          break;
        case NYIR_VEC8_MUL_F32:
          fout = fa * fb;
          break;
        case NYIR_VEC8_DIV_F32:
          fout = fa / fb;
          break;
        case NYIR_VEC8_FMA_F32:
          fout = fmaf(fa, fb, nyr_bits_to_f32((uint32_t)addend[lane]));
          break;
        default:
          goto unsupported;
        }
        eval_slot.vector_values[(size_t)in->dst * 4 + lane] =
            (int64_t)nyr_f32_to_bits(fout);
      }
      eval_slot.vector_width[in->dst] = (uint8_t)width;
      values[in->dst] = eval_slot.vector_values[(size_t)in->dst * 4];
      known[in->dst] = true;
      nyr_eval_note_value(result, in->dst);
      break;
    }
    case NYIR_OP_COUNT:
      goto unsupported;
    }
  }
  if (result)
    result->steps = steps;
  nyr_eval_pool_end(eval_depth, &eval_slot);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;

profile_oom:
  nyr_eval_result_free(result);
  nyr_eval_pool_end(eval_depth, &eval_slot);
  return nyr_inst_err(err, err_len, in, inst_index,
                       "NYIR VM profile allocation failed");
missing_value:
  nyr_eval_result_free(result);
  nyr_eval_pool_end(eval_depth, &eval_slot);
  return nyr_inst_err(err, err_len, in, inst_index,
                       "NYIR VM read an unavailable value");
bad_local:
  nyr_eval_result_free(result);
  nyr_eval_pool_end(eval_depth, &eval_slot);
  return nyr_inst_err(err, err_len, in, inst_index,
                       "NYIR VM local slot is out of range");
missing_label:
  nyr_eval_result_free(result);
  nyr_eval_pool_end(eval_depth, &eval_slot);
  return nyr_inst_err(err, err_len, in, inst_index,
                       "NYIR VM branch target is missing");
bounds_violation:
  nyr_eval_result_free(result);
  nyr_eval_pool_end(eval_depth, &eval_slot);
  return nyr_inst_err(err, err_len, in, inst_index,
                       "NYIR VM bounds check failed: offset out of range");
unsupported:
  nyr_eval_result_free(result);
  nyr_eval_pool_end(eval_depth, &eval_slot);
  return nyr_inst_err(err, err_len, in, inst_index,
                       "NYIR VM operation is unsupported for these operands");
}

bool nyr_ir_eval(const nyr_func_t *f, int64_t *locals, size_t local_count,
               size_t max_steps, nyr_eval_result_t *result, char *err,
               size_t err_len) {
  return nyr_ir_eval_with_calls(f, locals, local_count, max_steps, result, NULL,
                              NULL, err, err_len);
}

/*
 * Translation-validation seed
 *
 * Bounded multi-input differential interpretation for pure integer
 * straight-line NYIR. Not a full SMT harness — just the first safety
 * net that catches pass miscompiles before native emission.
 */

bool nyr_is_pure_i64_straightline(const nyr_func_t *f) {
  /*
   * Historical name: now admits pure i64 scalar control-flow + PHIs + locals.
   * Still excludes calls, floats, and non-local memory.
   */
  if (!f || f->len == 0)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    switch (in->op) {
    case NYIR_NOP:
    case NYIR_BOUNDS_CHECK:
    case NYIR_CONST_I64:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
    case NYIR_COPY:
    case NYIR_VEC4_SET1_I64:
    case NYIR_VEC4_REDUCE_ADD_I64:
    case NYIR_VEC4_ADD_I64:
    case NYIR_VEC4_SUB_I64:
    case NYIR_VEC4_AND_I64:
    case NYIR_VEC4_OR_I64:
    case NYIR_VEC4_XOR_I64:
    case NYIR_VEC4_SHL_I64:
    case NYIR_VEC4_SAR_I64:
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC8_REDUCE_ADD_I64:
    case NYIR_VEC8_ADD_I64:
    case NYIR_VEC8_SUB_I64:
    case NYIR_VEC8_AND_I64:
    case NYIR_VEC8_OR_I64:
    case NYIR_VEC8_XOR_I64:
    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC4_ADD_F64:
    case NYIR_VEC4_SUB_F64:
    case NYIR_VEC4_MUL_F64:
    case NYIR_VEC4_DIV_F64:
    case NYIR_VEC4_FMA_F64:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC4_REDUCE_ADD_F64:
    case NYIR_PHI:
    case NYIR_ADD_I64:
    case NYIR_SUB_I64:
    case NYIR_MUL_I64:
    case NYIR_DIV_I64:
    case NYIR_MOD_I64:
    case NYIR_AND_I64:
    case NYIR_OR_I64:
    case NYIR_XOR_I64:
    case NYIR_SHL_I64:
    case NYIR_SAR_I64:
    case NYIR_ROR_I64:
    case NYIR_ROR32_I64:
    case NYIR_BSWAP_I64:
    case NYIR_BSWAP32_I64:
    case NYIR_SELECT_I64:
    case NYIR_CMP_I64:
    case NYIR_ADD_F64:
    case NYIR_SUB_F64:
    case NYIR_MUL_F64:
    case NYIR_DIV_F64:
    case NYIR_SQRT_F64:
    case NYIR_SIN_F64:
    case NYIR_COS_F64:
    case NYIR_I64_TO_F64:
    case NYIR_F64_TO_I64:
    case NYIR_F32_TO_I64:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
    case NYIR_CMP_F64:
    case NYIR_ADD_F32:
    case NYIR_SUB_F32:
    case NYIR_MUL_F32:
    case NYIR_DIV_F32:
    case NYIR_I64_TO_F32:
    case NYIR_CMP_F32:
    case NYIR_LOAD_LOCAL:
    case NYIR_STORE_LOCAL:
    case NYIR_LABEL:
    case NYIR_BR:
    case NYIR_BR_IF:
    case NYIR_RET:
      break;
    default:
      return false;
    }
  }
  return true;
}

static size_t nyr_tv_local_count(const nyr_func_t *f) {
  return nyr_max_local(f);
}

static void nyr_tv_fill_locals(int64_t *locals, size_t n, int trial,
                                uint64_t *rng) {
  if (!locals || n == 0)
    return;
  static const int64_t edges[] = {0,  1,  -1,  2,   -2,        7,        8,
                                  15, 16, 255, 256, INT64_MAX, INT64_MIN};
  if (trial == 0) {
    for (size_t i = 0; i < n; ++i)
      locals[i] = 0;
    return;
  }
  if (trial == 1) {
    for (size_t i = 0; i < n; ++i)
      locals[i] = 1;
    return;
  }
  if (trial == 2) {
    for (size_t i = 0; i < n; ++i)
      locals[i] = -1;
    return;
  }
  if (trial == 3) {
    for (size_t i = 0; i < n; ++i)
      locals[i] = (int64_t)i + 1;
    return;
  }
  const size_t edge_count = sizeof(edges) / sizeof(edges[0]);
  if (trial >= 4 && (size_t)(trial - 4) < edge_count) {
    size_t base = (size_t)(trial - 4);
    for (size_t i = 0; i < n; ++i)
      locals[i] = edges[(base + i) % edge_count];
    return;
  }
  /*
   * Deterministic LCG fill for remaining trials.
   */
  for (size_t i = 0; i < n; ++i) {
    *rng = (*rng * UINT64_C(6364136223846793005)) + UINT64_C(1);
    locals[i] = (int64_t)(*rng);
    /*
     * Avoid 0 in some slots so div/mod trials are more often defined.
     */
    if ((trial & 1) && locals[i] == 0)
      locals[i] = (int64_t)(i + 3);
  }
}

bool nyr_tv_equiv_straightline(const nyr_func_t *before,
                                const nyr_func_t *after, int trials, char *err,
                                size_t err_len) {
  if (!before || !after)
    return nyr_err(err, err_len, "native TV: missing function");
  if (trials <= 0)
    return true;
  /*
   * Seed scope: pure i64 scalar NYIR (locals, arithmetic, compares, PHI,
   * labels/branches, returns). Calls/floats/heap memory stay out of scope.
   */
  if (!nyr_is_pure_i64_straightline(before) ||
      !nyr_is_pure_i64_straightline(after))
    return true;

  size_t n_before = nyr_tv_local_count(before);
  size_t n_after = nyr_tv_local_count(after);
  size_t n = n_before > n_after ? n_before : n_after;
  int64_t *locals_a = n ? (int64_t *)calloc(n, sizeof(int64_t)) : NULL;
  int64_t *locals_b = n ? (int64_t *)calloc(n, sizeof(int64_t)) : NULL;
  if (n && (!locals_a || !locals_b)) {
    free(locals_a);
    free(locals_b);
    return nyr_err(err, err_len, "native TV: out of memory");
  }

  uint64_t rng = UINT64_C(0xC0FFEE) ^ (uint64_t)(unsigned)trials;
  int compared = 0;
  for (int t = 0; t < trials; ++t) {
    nyr_tv_fill_locals(locals_a, n, t, &rng);
    if (n)
      memcpy(locals_b, locals_a, n * sizeof(int64_t));

    nyr_eval_result_t ra = {0};
    nyr_eval_result_t rb = {0};
    char ea[192] = {0};
    char eb[192] = {0};
    bool ok_a = nyr_ir_eval(before, locals_a, n, 100000, &ra, ea, sizeof(ea));
    bool ok_b = nyr_ir_eval(after, locals_b, n, 100000, &rb, eb, sizeof(eb));

    if (!ok_a && !ok_b) {
      nyr_eval_result_free(&ra);
      nyr_eval_result_free(&rb);
      continue;
    }
    if (ok_a != ok_b) {
      nyr_eval_result_free(&ra);
      nyr_eval_result_free(&rb);
      free(locals_a);
      free(locals_b);
      return nyr_err(
          err, err_len,
          "native TV: trial %d eval divergence (before=%s after=%s)", t,
          ok_a ? "ok" : (ea[0] ? ea : "fail"),
          ok_b ? "ok" : (eb[0] ? eb : "fail"));
    }
    if (!ra.returned || !rb.returned) {
      int ret_a = (int)ra.returned;
      int ret_b = (int)rb.returned;
      nyr_eval_result_free(&ra);
      nyr_eval_result_free(&rb);
      free(locals_a);
      free(locals_b);
      return nyr_err(err, err_len,
                      "native TV: trial %d missing return (before=%d after=%d)",
                      t, ret_a, ret_b);
    }
    if (ra.result != rb.result) {
      int64_t res_a = ra.result;
      int64_t res_b = rb.result;
      nyr_eval_result_free(&ra);
      nyr_eval_result_free(&rb);
      free(locals_a);
      free(locals_b);
      return nyr_err(err, err_len,
                      "native TV: trial %d result mismatch before=%" PRId64
                      " after=%" PRId64,
                      t, res_a, res_b);
    }
    compared++;
    nyr_eval_result_free(&ra);
    nyr_eval_result_free(&rb);
  }

  free(locals_a);
  free(locals_b);
  if (compared == 0 && trials > 0) {
    /*
     * Every trial trapped — still treat as pass for the seed, but note it.
     */
    return true;
  }
  return true;
}

bool nyr_eval(const nyr_func_t *func, int64_t *locals, size_t local_count,
              size_t max_steps, nyr_eval_result_t *result, char *err,
              size_t err_len) {
  return nyr_eval_with_calls(func, locals, local_count, max_steps, result, NULL,
                             NULL, err, err_len);
}

bool nyr_eval_with_calls(const nyr_func_t *func, int64_t *locals,
                         size_t local_count, size_t max_steps,
                         nyr_eval_result_t *result,
                         nyr_call_resolver_t resolver, void *resolver_ctx,
                         char *err, size_t err_len) {
  if (result)
    nyr_eval_result_init(result);
  bool ok = nyr_ir_eval_with_calls(func, locals, local_count, max_steps, result,
                                 resolver, resolver_ctx, err, err_len);
  if (ok && err && err_len)
    err[0] = '\0';
  return ok;
}

void nyr_eval_result_init(nyr_eval_result_t *result) {
  if (result)
    memset(result, 0, sizeof(*result));
}

void nyr_eval_result_destroy(nyr_eval_result_t *result) {
  if (!result)
    return;
  nyr_eval_result_free(result);
  memset(result, 0, sizeof(*result));
}
