/*
 * Devirtualization: converts indirect calls (function pointer calls,
 * virtual calls) into direct calls when the target can be determined
 * at compile time. This enables inlining and further optimization.
 *
 * References:
 *  Calder & Grunwald — "Reducing Indirect Function Call Overhead
 *   in C++ Programs" (POPL 1994). Class hierarchy analysis + profiling.
 *  Ishizaki et al. — "Optimizing Dynamically-Dispatched Method
 *   Calls in Java" (OOPSLA 2000). Guarded devirtualization + inlining.
 *  Dean, Grove, Chambers — "Optimization of Object-Oriented
 *   Programs Using Static Class Hierarchy Analysis" (ECOOP 1995).
 *   CHA (Class Hierarchy Analysis) for devirtualization.
 */
#include "base/debug.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void nyr_devirt_trace(const nyr_func_t *f, size_t at,
                              const char *action, const char *reason) {
  if (!ny_dbg_is_enabled(NY_DBG_HEURISTIC) &&
      !ny_trace_enabled("NY_TRACE_DEVIRT"))
    return;
  (void)f;
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("DEVIRT", "%s @%zu: %s", action, at, reason);
  else
    ny_trace_line("DEVIRT", "devirt: %s @%zu: %s", action, at, reason);
}

/*
 * Check if a value is a known function symbol (constant address or
 * ADDR_SYMBOL). Traverses COPY chains to find the defining symbol.
 */
static const char *get_function_name(const nyr_func_t *f, const int *defs,
                                     int value) {
  int depth = 0;
  while (value >= 0 && value < f->next_value && depth++ < 16) {
    int def = defs[value];
    if (def < 0 || (size_t)def >= f->len)
      return NULL;
    const nyr_inst_t *in = &f->data[def];
    if ((in->op == NYIR_CONST_I64 || in->op == NYIR_ADDR_SYMBOL) &&
        in->symbol != NULL)
      return in->symbol;
    if (in->op == NYIR_COPY && in->a >= 0) {
      value = in->a;
      continue;
    }
    break;
  }
  return NULL;
}

static int *nyr_devirt_call_arg_slot(nyr_inst_t *call, size_t index) {
  if (index == 0)
    return &call->a;
  if (index == 1)
    return &call->b;
  if (index == 2)
    return &call->c;
  if (index == 3)
    return &call->d;
  if (index == 4)
    return &call->e;
  if (index == 5)
    return &call->f;
  index -= 6;
  return call->extra_args && index < call->extra_args_len
             ? &call->extra_args[index]
             : NULL;
}

static bool nyr_devirt_swap_call_args(nyr_inst_t *call, size_t a, size_t b) {
  int *arg_a = nyr_devirt_call_arg_slot(call, a);
  int *arg_b = nyr_devirt_call_arg_slot(call, b);
  if (!arg_a || !arg_b)
    return false;

  int value = *arg_a;
  *arg_a = *arg_b;
  *arg_b = value;

  nyr_scalar_class_t class_a = nyr_call_arg_class_get(call, a);
  nyr_scalar_class_t class_b = nyr_call_arg_class_get(call, b);
  nyr_call_arg_class_set(call->arg_scalar_classes, a, class_b);
  nyr_call_arg_class_set(call->arg_scalar_classes, b, class_a);

  uint8_t ownership_a = nyr_call_arg_ownership_get(call, a);
  uint8_t ownership_b = nyr_call_arg_ownership_get(call, b);
  nyr_call_arg_ownership_set(call, a, ownership_b);
  nyr_call_arg_ownership_set(call, b, ownership_a);

  uint8_t contract_a = nyr_call_arg_contract_get(call, a);
  uint8_t contract_b = nyr_call_arg_contract_get(call, b);
  nyr_call_arg_contract_set(call, a, contract_b);
  nyr_call_arg_contract_set(call, b, contract_a);

  if (call->arg_sizes) {
    uint32_t size = call->arg_sizes[a];
    call->arg_sizes[a] = call->arg_sizes[b];
    call->arg_sizes[b] = size;
  }
  return true;
}

/*
 * Main devirtualization pass.
 * Scans for CALL instructions with function pointer arguments and replaces
 * them with direct calls when the target is known.
 */
bool nyr_devirtualize(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  int *defs = nyr_build_defs(f);
  if (!defs)
    return false;

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *call = &f->data[i];
    if (call->op != NYIR_CALL)
      continue;

    if (call->symbol) {
      nyr_devirt_trace(f, i, "keep-direct", call->symbol);
      continue;
    }

    int args[NYIR_CALL_MAX_ARGS];
    int argc = 0;
    if (!nyr_call_args(call, f->next_value, args, NYIR_CALL_MAX_ARGS, &argc,
                        NULL, 0) ||
        argc == 0)
      continue;

    size_t target_index = 0;
    const char *fname = NULL;
    for (; target_index < (size_t)argc; ++target_index) {
      fname = get_function_name(f, defs, args[target_index]);
      if (fname)
        break;
    }
    if (fname) {
      if (target_index > 0 &&
          !nyr_devirt_swap_call_args(call, 0, target_index)) {
        nyr_devirt_trace(f, i, "keep-indirect",
                          "function target has no writable argument slot");
        continue;
      }
      nyr_devirt_trace(f, i, "devirtualize", fname);
      call->symbol = fname;
    } else {
      nyr_devirt_trace(f, i, "keep-indirect",
                        "no call argument resolves to a function symbol "
                        "through the supported COPY chain");
    }
  }

  free(defs);
  return true;
}
