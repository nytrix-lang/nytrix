/*
 * Tail Call Optimization: converts SELF-recursive calls in true tail
 * position into argument rebinding plus a jump to the function entry,
 * reusing the current stack frame.
 *
 * References:
 *  Steele — "Debunking the 'Expensive Procedure Call' Myth"
 *   (ACM SIGPLAN Notices 1977). TCO as a compiler optimization.
 *  Clinger — "Proper Tail Recursion and Space Efficiency"
 *   (PLDI 1998). Formal definition and implementation.
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.10 §10.2:
 *   tail call optimization and space efficiency.
 *
 * Correctness contract:
 *  - Self-recursion is only provable when the builder supplies the
 *    current function's name; NYIR instructions carry no callee
 *    identity beyond the symbol string. Without a name the pass does
 *    nothing — guessing "any symbol call" once turned every tail call
 *    into a jump into the wrong function.
 *  - Only TRUE tail positions convert: the trailing RET must return
 *    the call result itself (or both must be void). A call whose
 *    result is discarded before some other return is NOT a tail call.
 *  - Arguments are rebound through the ABI convention that parameters
 *    live in local slots 0..param_count-1 (the machine prologue spills
 *    argument registers there): the conversion stores each argument
 *    into its parameter slot before branching to the entry label.
 *  - Aggregate (SRET) returns and two-register captures never convert.
 */
#include "code/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nyir_tco_trace_enabled(void) {
  return ny_trace_enabled("NY_TRACE_TCO");
}

static void nyir_tco_trace(const nyir_func_t *f, size_t at, const char *action,
                           const char *reason) {
  (void)f;
  if (!nyir_tco_trace_enabled())
    return;
  ny_trace_line("TCO", "tco: %s @%zu: %s", action, at, reason);
}

/*
 * True tail position: the call is immediately followed (NOPs aside) by a
 * RET that returns exactly the call result, or by a void RET of a void
 * call. Anything else in between disqualifies the call.
 */
static bool is_true_tail_call(const nyir_func_t *f, size_t call_idx) {
  if (call_idx >= f->len)
    return false;
  const nyir_inst_t *call = &f->data[call_idx];
  if (call->op != NYIR_CALL)
    return false;

  for (size_t i = call_idx + 1; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;
    if (in->op != NYIR_RET)
      return false;
    if (call->dst >= 0)
      return in->a == call->dst;
    return in->a < 0;
  }
  return false;
}

bool nyir_tail_call(nyir_func_t *f, const char *func_name) {
  if (!f || f->len == 0)
    return true;
  /*
   * No name, no proof. Converting an arbitrary callee's tail call into
   * a jump to THIS function's entry silently replaced the callee with
   * the caller.
   */
  if (!func_name || !func_name[0])
    return true;

  /*
   * Entry label of this function. The conversion branches here; without
   * a labeled entry there is nothing sound to target.
   */
  int64_t entry_label = -1;
  for (size_t j = 0; j < f->len; ++j) {
    if (f->data[j].op == NYIR_LABEL) {
      entry_label = f->data[j].imm;
      break;
    }
  }
  if (entry_label < 0)
    return true;

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *call = &f->data[i];
    if (call->op != NYIR_CALL || !is_true_tail_call(f, i))
      continue;
    if (!call->symbol || strcmp(call->symbol, func_name) != 0)
      continue;
    /*
     * Aggregate returns never convert.
     */
    if ((call->flags & NYIR_INST_F_SRET) != 0)
      continue;

    int args[NYIR_CALL_MAX_ARGS];
    int arity = 0;
    if (!nyir_call_args(call, f->next_value, args, NYIR_CALL_MAX_ARGS,
                        &arity, NULL, 0))
      continue;
    if (arity < 0 || (size_t)arity != f->param_count)
      continue; /* cannot rebind a different signature */

    size_t ret_at = i + 1;
    while (ret_at < f->len && f->data[ret_at].op == NYIR_NOP)
      ++ret_at;
    if (ret_at >= f->len || f->data[ret_at].op != NYIR_RET)
      continue;

    nyir_tco_trace(f, i, "optimize", "self-recursive tail call");

    /*
     * Grow once: param_count argument stores plus one BR. The old CALL
     * slot becomes the first store; the trailing RET (whose value is
     * exactly the call result) becomes dead and is erased, so the
     * cleared call dst leaves no dangling use behind.
     */
    size_t add = (size_t)arity + 1;
    if (!nir_ensure_inst_space(f, add))
      return false;
    memmove(&f->data[i + add], &f->data[i],
            (f->len - i) * sizeof(*f->data));
    f->len += add;

    for (int k = 0; k < arity; ++k) {
      f->data[i + (size_t)k] = (nyir_inst_t){
          .op = NYIR_STORE_LOCAL,
          .dst = -1, .a = args[k], .b = -1, .c = -1, .d = -1,
          .e = -1, .f = -1,
          .imm = (int64_t)k,
          .effects = NYIR_EFFECT_WRITE_LOCAL};
    }
    f->data[i + (size_t)arity] = (nyir_inst_t){
        .op = NYIR_BR,
        .dst = -1, .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
        .imm = entry_label,
        .flags = 0,
        .effects = NYIR_EFFECT_CONTROL};

    /*
     * The RET moved to i + arity + 1. It returned the call result (or
     * was void) and is unreachable after the unconditional branch;
     * erase it so no operand references the erased call dst.
     */
    nyir_inst_discard(&f->data[i + (size_t)arity + 1]);

    /*
     * Skip past the rewritten region; indices shifted by `add`.
     */
    i += add;
  }

  return true;
}
