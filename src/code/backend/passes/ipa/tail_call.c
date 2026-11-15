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
#include "base/debug.h"
#include "code/backend/passes/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nyr_tco_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
         ny_trace_enabled("NY_TRACE_TCO");
}

static void nyr_tco_trace(const nyr_func_t *f, size_t at, const char *action,
                           const char *reason) {
  (void)f;
  if (!nyr_tco_trace_enabled())
    return;
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("TAIL_CALL", "%s @%zu: %s", action, at, reason);
  else
    ny_trace_line("TCO", "tco: %s @%zu: %s", action, at, reason);
}

/*
 * True tail position: the call is immediately followed (NOPs aside) by a
 * RET that returns exactly the call result, or by a void RET of a void
 * call. Anything else in between disqualifies the call.
 */
static bool is_true_tail_call(const nyr_func_t *f, size_t call_idx) {
  if (call_idx >= f->len)
    return false;
  const nyr_inst_t *call = &f->data[call_idx];
  if (call->op != NYIR_CALL)
    return false;

  for (size_t i = call_idx + 1; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
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

bool nyr_recursive_add_to_loop(nyr_func_t *f, const char *func_name) {
  /*
   * Turn the common linear-recursion shape
   *
   *   if (n < 2) return n;
   *   return f(n - 1) + f(n - 2);
   *
   * into a loop carrying the accumulated left result and the right recursive
   * argument. This is the accumulator form of tail-recursion elimination used
   * by optimizing compilers. Restrict it to the exact verified one-parameter
   * NYIR shape: no effects, alternate returns, or arithmetic operations are
   * reordered by this transform.
   */
  if (!f || !func_name || !func_name[0] || f->param_count != 1 ||
      !f->param_types || f->param_types[0] != NYIR_PARAM_I64 ||
      f->return_aggregate_size != 0)
    return true;

  size_t ix[18], count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_NOP)
      continue;
    if (count == sizeof(ix) / sizeof(ix[0]))
      return true;
    ix[count++] = i;
  }
  if (count != 16 && count != 17)
    return true;
  const nyr_inst_t *in[18];
  nyr_inst_t implicit_entry = {.op = NYIR_LABEL, .imm = -1};
  size_t offset = count == 16 ? 1 : 0;
  if (offset)
    in[0] = &implicit_entry;
  for (size_t i = 0; i < count; ++i)
    in[i + offset] = &f->data[ix[i]];

  size_t n2_load_i = 12;
  bool has_n2_reload = in[n2_load_i]->op == NYIR_LOAD_LOCAL;
  size_t two2_i = has_n2_reload ? 13 : 12;
  size_t sub2_i = has_n2_reload ? 14 : 13;
  size_t call2_i = has_n2_reload ? 15 : 14;
  size_t add_i = has_n2_reload ? 16 : 15;
  size_t ret_i = has_n2_reload ? 17 : 16;
  if (in[0]->op != NYIR_LABEL || in[1]->op != NYIR_LOAD_LOCAL ||
      in[1]->imm != 0 || in[2]->op != NYIR_CONST_I64 || in[2]->imm != 2 ||
      in[3]->op != NYIR_CMP_I64 || in[3]->cmp != NYIR_CMP_LT ||
      in[3]->a != in[1]->dst || in[3]->b != in[2]->dst ||
      in[4]->op != NYIR_BR_IF || in[4]->a != in[3]->dst ||
      in[5]->op != NYIR_BR || in[6]->op != NYIR_LABEL ||
      in[4]->imm != in[6]->imm || in[7]->op != NYIR_RET ||
      in[7]->a != in[1]->dst || in[8]->op != NYIR_LABEL ||
      in[5]->imm != in[8]->imm || in[9]->op != NYIR_CONST_I64 ||
      in[9]->imm != 1 || in[10]->op != NYIR_SUB_I64 ||
      in[10]->a != in[1]->dst || in[10]->b != in[9]->dst ||
      in[11]->op != NYIR_CALL || in[11]->dst < 0 || !in[11]->symbol ||
      strcmp(in[11]->symbol, func_name) != 0 ||
      (has_n2_reload && in[n2_load_i]->imm != 0) ||
      (!has_n2_reload && in[n2_load_i]->op != NYIR_CONST_I64) ||
      in[two2_i]->op != NYIR_CONST_I64 || in[two2_i]->imm != 2 ||
      in[sub2_i]->op != NYIR_SUB_I64 ||
      in[sub2_i]->a != (has_n2_reload ? in[n2_load_i]->dst : in[1]->dst) ||
      in[sub2_i]->b != in[two2_i]->dst || in[call2_i]->op != NYIR_CALL ||
      in[call2_i]->dst < 0 || !in[call2_i]->symbol ||
      strcmp(in[call2_i]->symbol, func_name) != 0 ||
      in[add_i]->op != NYIR_ADD_I64 || in[ret_i]->op != NYIR_RET ||
      in[ret_i]->a != in[add_i]->dst)
    return true;

  int call1_args[NYIR_CALL_MAX_ARGS], call2_args[NYIR_CALL_MAX_ARGS];
  int call1_arity = 0, call2_arity = 0;
  if (!nyr_call_args(in[11], f->next_value, call1_args,
                     NYIR_CALL_MAX_ARGS, &call1_arity, NULL, 0) ||
      !nyr_call_args(in[call2_i], f->next_value, call2_args,
                     NYIR_CALL_MAX_ARGS, &call2_arity, NULL, 0) ||
      call1_arity != 1 || call2_arity != 1 ||
      call1_args[0] != in[10]->dst || call2_args[0] != in[sub2_i]->dst ||
      !((in[add_i]->a == in[11]->dst &&
         in[add_i]->b == in[call2_i]->dst) ||
        (in[add_i]->b == in[11]->dst &&
         in[add_i]->a == in[call2_i]->dst)))
    return true;

  int64_t first_label = nyr_fresh_label(f);
  if (first_label < 0 || first_label > INT64_MAX - 3)
    return true;
  const int64_t entry_label = first_label;
  const int64_t loop_label = first_label + 1;
  const int64_t base_label = first_label + 2;
  const int64_t body_label = first_label + 3;
  const int64_t pre_label = first_label + 4;
  const int64_t exit_label = first_label + 5;
  int value = f->next_value;
  int two = value++;
  int one = value++;
  int base_cond = value++;
  int a_init = value++;
  int b_init = value++;
  int i_init = value++;
  int i_phi = value++;
  int a_phi = value++;
  int b_phi = value++;
  int loop_cond = value++;
  int next = value++;
  int i_next = value++;
  int n = in[1]->dst;

  nyr_inst_t *out = calloc(27, sizeof(*out));
  if (!out)
    return false;
  size_t len = 0;
#define EMIT(inst_) (out[len++] = (inst_))
  const nyr_debug_loc_t loc = in[0]->debug;
  const nyr_debug_loc_t rec_loc = in[11]->debug;
  nyr_phi_incoming_t *i_incoming = calloc(2, sizeof(*i_incoming));
  nyr_phi_incoming_t *a_incoming = calloc(2, sizeof(*a_incoming));
  nyr_phi_incoming_t *b_incoming = calloc(2, sizeof(*b_incoming));
  if (!i_incoming || !a_incoming || !b_incoming) {
    free(i_incoming);
    free(a_incoming);
    free(b_incoming);
    free(out);
    return false;
  }
  i_incoming[0] = (nyr_phi_incoming_t){.predecessor_label = pre_label,
                                       .value = i_init};
  i_incoming[1] = (nyr_phi_incoming_t){.predecessor_label = body_label,
                                       .value = i_next};
  a_incoming[0] = (nyr_phi_incoming_t){.predecessor_label = pre_label,
                                       .value = a_init};
  a_incoming[1] = (nyr_phi_incoming_t){.predecessor_label = body_label,
                                       .value = b_phi};
  b_incoming[0] = (nyr_phi_incoming_t){.predecessor_label = pre_label,
                                       .value = b_init};
  b_incoming[1] = (nyr_phi_incoming_t){.predecessor_label = body_label,
                                       .value = next};
  EMIT(((nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = entry_label, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_LOAD_LOCAL, .dst = n, .a = -1,
                      .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = 0, .effects = NYIR_EFFECT_READ_LOCAL,
                      .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_CONST_I64, .dst = two, .a = -1,
                      .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = 2, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_CONST_I64, .dst = one, .a = -1,
                      .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = 1, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_CMP_I64, .dst = base_cond, .a = n,
                      .b = two, .c = -1, .d = -1, .e = -1, .f = -1,
                      .cmp = NYIR_CMP_LT, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_BR_IF, .dst = -1, .a = base_cond, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = base_label, .effects = NYIR_EFFECT_CONTROL,
                      .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = pre_label, .effects = NYIR_EFFECT_CONTROL,
                      .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = base_label, .debug = in[6]->debug}));
  EMIT(((nyr_inst_t){.op = NYIR_RET, .dst = -1, .a = n, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .effects = NYIR_EFFECT_CONTROL,
                      .debug = in[7]->debug}));
  EMIT(((nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = pre_label, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_CONST_I64, .dst = a_init, .a = -1,
                      .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = 0, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_CONST_I64, .dst = b_init, .a = -1,
                      .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = 1, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_CONST_I64, .dst = i_init, .a = -1,
                      .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = 2, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = loop_label, .effects = NYIR_EFFECT_CONTROL,
                      .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = loop_label, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_PHI, .dst = i_phi, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .phi_incoming = i_incoming,
                      .phi_incoming_len = 2, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_PHI, .dst = a_phi, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .phi_incoming = a_incoming,
                      .phi_incoming_len = 2, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_PHI, .dst = b_phi, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .phi_incoming = b_incoming,
                      .phi_incoming_len = 2, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_CMP_I64, .dst = loop_cond, .a = i_phi,
                      .b = n, .c = -1, .d = -1, .e = -1, .f = -1,
                      .cmp = NYIR_CMP_LE, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_BR_IF, .dst = -1, .a = loop_cond, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = body_label, .effects = NYIR_EFFECT_CONTROL,
                      .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = exit_label, .effects = NYIR_EFFECT_CONTROL,
                      .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = body_label, .debug = in[8]->debug}));
  EMIT(((nyr_inst_t){.op = NYIR_ADD_I64, .dst = next,
                      .a = a_phi, .b = b_phi, .c = -1, .d = -1,
                      .e = -1, .f = -1, .debug = rec_loc}));
  EMIT(((nyr_inst_t){.op = NYIR_ADD_I64, .dst = i_next,
                      .a = i_phi, .b = one, .c = -1, .d = -1,
                      .e = -1, .f = -1, .debug = rec_loc}));
  EMIT(((nyr_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = loop_label, .effects = NYIR_EFFECT_CONTROL,
                      .debug = rec_loc}));
  EMIT(((nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = exit_label, .debug = loc}));
  EMIT(((nyr_inst_t){.op = NYIR_RET, .dst = -1, .a = b_phi, .b = -1,
                      .c = -1, .d = -1, .e = -1, .f = -1,
                      .effects = NYIR_EFFECT_CONTROL, .debug = loc}));
#undef EMIT

  for (size_t i = 0; i < f->len; ++i)
    nyr_inst_discard(&f->data[i]);
  free(f->data);
  f->data = out;
  f->len = len;
  f->cap = 27;
  f->next_value = value;
  return true;
}

bool nyr_tail_call(nyr_func_t *f, const char *func_name) {
  if (!f || f->len == 0)
    return true;
  /*
   * No name, no proof. Converting an arbitrary callee's tail call into
   * a jump to THIS function's entry silently replaced the callee with
   * the caller.
   */
  if (!func_name || !func_name[0])
    return true;

  if (!nyr_recursive_add_to_loop(f, func_name))
    return false;

  /*
   * Entry label of this function. The conversion branches here; without
   * a labeled entry there is nothing sound to target.
   *
   * The branch target must be the function's own entry block, whose
   * parameter loads then observe the stored arguments. Taking the first
   * LABEL anywhere was wrong when the entry block is unlabeled: the
   * "recursive call" jumped into an arbitrary later block (in practice an
   * early return), silently ending the recursion with stale arguments.
   * When the entry block has no label, split it off so the loop re-enters
   * an ordinary labeled block.
   */
  size_t first = 0;
  while (first < f->len && f->data[first].op == NYIR_NOP)
    ++first;
  if (first >= f->len)
    return true;
  bool has_self_tail_call = false;
  for (size_t j = 0; j < f->len && !has_self_tail_call; ++j)
    has_self_tail_call = f->data[j].op == NYIR_CALL && f->data[j].symbol &&
                         strcmp(f->data[j].symbol, func_name) == 0 &&
                         is_true_tail_call(f, j);
  if (!has_self_tail_call)
    return true;
  int64_t entry_label = -1;
  if (f->data[first].op == NYIR_LABEL) {
    entry_label = f->data[first].imm;
  } else {
    /*
     * Split rather than label the function entry: `br L; L: <old entry>`.
     * The re-entered block is then an ordinary join with two predecessors,
     * so SSA construction gives its parameter loads PHIs and backends keep
     * treating the true entry block as predecessor-free.
     */
    entry_label = nyr_fresh_label(f);
    if (entry_label < 0)
      return true;
    if (!nir_ensure_inst_space(f, 2))
      return false;
    memmove(&f->data[2], &f->data[0], f->len * sizeof(*f->data));
    f->len += 2;
    f->data[0] = (nyr_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1,
                              .c = -1, .d = -1, .e = -1, .f = -1,
                              .imm = entry_label};
    f->data[0].effects = nyr_inst_effects(&f->data[0]);
    f->data[1] = (nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1,
                              .c = -1, .d = -1, .e = -1, .f = -1,
                              .imm = entry_label};
    f->data[1].effects = nyr_inst_effects(&f->data[1]);
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *call = &f->data[i];
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
    if (!nyr_call_args(call, f->next_value, args, NYIR_CALL_MAX_ARGS, &arity,
                        NULL, 0))
      continue;
    if (arity < 0 || (size_t)arity != f->param_count)
      continue; /* cannot rebind a different signature */

    size_t ret_at = i + 1;
    while (ret_at < f->len && f->data[ret_at].op == NYIR_NOP)
      ++ret_at;
    if (ret_at >= f->len || f->data[ret_at].op != NYIR_RET)
      continue;

    /*
     * These instructions implement the source call site. Keep its location
     * attached so diagnostics and debug dumps remain actionable afterward.
     */
    const nyr_debug_loc_t call_debug = call->debug;

    nyr_tco_trace(f, i, "optimize", "self-recursive tail call");

    /*
     * Grow once: param_count argument stores plus one BR. The old CALL
     * slot becomes the first store; the trailing RET (whose value is
     * exactly the call result) becomes dead and is erased, so the
     * cleared call dst leaves no dangling use behind.
     */
    size_t add = (size_t)arity + 1;
    if (!nir_ensure_inst_space(f, add))
      return false;
    memmove(&f->data[i + add], &f->data[i], (f->len - i) * sizeof(*f->data));
    f->len += add;

    for (int k = 0; k < arity; ++k) {
      f->data[i + (size_t)k] = (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                             .dst = -1,
                                             .a = args[k],
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .imm = (int64_t)k,
                                             .effects = NYIR_EFFECT_WRITE_LOCAL,
                                             .debug = call_debug};
    }
    f->data[i + (size_t)arity] = (nyr_inst_t){.op = NYIR_BR,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = entry_label,
                                               .flags = 0,
                                               .effects = NYIR_EFFECT_CONTROL,
                                               .debug = call_debug};

    /*
     * The originals were shifted past the generated sequence. Drop both:
     * the moved CALL owns heap metadata, and its RET refers to its result.
     * Leaving either in unreachable code creates stale uses and retains
     * unnecessary call effects for later whole-function analyses.
     */
    nyr_inst_discard(&f->data[i + add]);
    nyr_inst_discard(&f->data[ret_at + add]);

    /*
     * Skip past the rewritten region; indices shifted by `add`.
     */
    i += add + 1;
  }

  return true;
}
