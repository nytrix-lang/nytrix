/*
 * Escape analysis for SROA (Scalar Replacement of Aggregates).
 *
 * Determines which local slots escape the current function. A slot escapes if:
 *   - Its address is taken (ADDR_LOCAL)
 *   - It's passed to a call (argument that might capture it)
 *   - It's stored to memory that might be accessed externally
 *   - It's returned directly
 *
 * Non-escaping slots can be promoted to SSA values and eliminated.
 */
#include "code/native/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nyir_escape_sroa(nyir_func_t *f);

bool nyir_escape_sroa(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  size_t slot_count = nyir_local_slot_count(f);
  if (!slot_count)
    return true;
  nyir_local_escape_info_t stk_escape[64] = {0};
  bool stk_loaded[64] = {0}, stk_stored[64] = {0};
  nyir_local_escape_info_t *escape = slot_count <= 64 ? stk_escape : calloc(slot_count, sizeof(*escape));
  bool *loaded = slot_count <= 64 ? stk_loaded : calloc(slot_count, sizeof(*loaded));
  bool *stored = slot_count <= 64 ? stk_stored : calloc(slot_count, sizeof(*stored));
  if (!escape || !loaded || !stored ||
      !nyir_analyze_local_escapes(f, escape, slot_count)) {
    if (slot_count > 64) { free(escape); free(loaded); free(stored); }
    return false;
  }

  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->imm < 0 || (size_t)in->imm >= slot_count)
      continue;
    if (in->op == NYIR_LOAD_LOCAL)
      loaded[in->imm] = true;
    else if (in->op == NYIR_STORE_LOCAL)
      stored[in->imm] = true;
  }

  const char *trace = getenv("NY_TRACE_ESCAPE");
  if (trace && trace[0] && strcmp(trace, "0") != 0) {
    for (size_t slot = 0; slot < slot_count; ++slot) {
      const nyir_local_escape_info_t *e = &escape[slot];
      if (!e->escapes)
        continue;
      size_t pc = e->first_escape_pc;
      const nyir_inst_t *in = pc < f->len ? &f->data[pc] : NULL;
      const char *file = in && in->debug.file ? in->debug.file : "<unknown>";
      const char *reason = e->thread_escape ? "thread"
                           : e->ffi_escape ? "ffi"
                           : e->unknown_call_escape ? "unknown-call"
                           : e->returned ? "return"
                           : e->stored_to_memory ? "stored-to-memory"
                           : e->passed_to_call ? "call-capture"
                           : "address-taken";
      fprintf(stderr,
              "nyir escape: escaped slot=%zu pc=%zu at %s:%u:%u reason=%s\n",
              slot, pc, file, in ? in->debug.line : 0,
              in ? in->debug.column : 0, reason);
    }
  }

  /*
   * Dead stores to private, unread slots are unobservable.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && !escape[in->imm].escapes &&
        !loaded[in->imm])
      *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
  }

  /*
   * Uninitialized private locals have the language-defined zero value.
   * Parameter slots (0 .. f->param_count - 1) are initialized by caller.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
        (size_t)in->imm >= f->param_count &&
        (size_t)in->imm < slot_count && !escape[in->imm].escapes &&
        !stored[in->imm])
      *in = (nyir_inst_t){.op = NYIR_CONST_I64, .dst = in->dst, .imm = 0};
  }

  if (slot_count > 64) {
    free(escape);
    free(loaded);
    free(stored);
  }
  return true;
}
