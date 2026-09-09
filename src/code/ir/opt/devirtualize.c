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
#include "code/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void nyir_devirt_trace(const nyir_func_t *f, size_t at,
                              const char *action, const char *reason) {
  if (!ny_trace_enabled("NY_TRACE_DEVIRT"))
    return;
  (void)f;
  ny_trace_line("DEVIRT", "devirt: %s @%zu: %s", action, at, reason);
}

/*
 * Check if a value is a known function symbol (constant address or ADDR_SYMBOL).
 * Traverses COPY chains to find the defining symbol.
 */
static const char *get_function_name(const nyir_func_t *f, const int *defs, int value) {
  int depth = 0;
  while (value >= 0 && value < f->next_value && depth++ < 16) {
    int def = defs[value];
    if (def < 0 || (size_t)def >= f->len)
      return NULL;
    const nyir_inst_t *in = &f->data[def];
    if ((in->op == NYIR_CONST_I64 || in->op == NYIR_ADDR_SYMBOL) && in->symbol != NULL)
      return in->symbol;
    if (in->op == NYIR_COPY && in->a >= 0) {
      value = in->a;
      continue;
    }
    break;
  }
  return NULL;
}


/*
 * Main devirtualization pass.
 * Scans for CALL instructions with function pointer arguments and replaces
 * them with direct calls when the target is known.
 */
bool nyir_devirtualize(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  int *defs = nyir_build_defs(f);
  if (!defs)
    return false;

  bool changed = false;

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *call = &f->data[i];
    if (call->op != NYIR_CALL)
      continue;

    if (call->symbol) {
      continue;
    }

    if (call->a >= 0 && call->a < f->next_value) {
      const char *fname = get_function_name(f, defs, call->a);
      if (fname) {
        nyir_devirt_trace(f, i, "devirtualize", fname);
        call->symbol = fname;
        changed = true;
        continue;
      }
    }

    for (size_t k = 0; k < call->extra_args_len; ++k) {
      int arg = call->extra_args[k];
      if (arg >= 0 && arg < f->next_value) {
        const char *fname = get_function_name(f, defs, arg);
        if (fname) {
          nyir_devirt_trace(f, i, "devirtualize", fname);
          call->symbol = fname;
          if (k > 0) {
            call->extra_args[k] = call->a;
            call->a = arg;
          }
          changed = true;
          break;
        }
      }
    }
  }

  free(defs);
  return changed || true;
}