#include "code/lang/code.h"
#include "code/lang/priv.h"

bool ny_eval_comptime_if(codegen_t *cg, stmt_t *s, bool *truthy) {
  if (!s || s->kind != NY_S_IF || !s->as.iff.test)
    return false;
  if (s->as.iff.test->kind != NY_E_COMPTIME)
    return false;
  LLVMValueRef val =
      gen_comptime_eval(cg, s->as.iff.test->as.comptime_expr.body);
  if (val && LLVMIsAConstantInt(val)) {
    uint64_t raw = LLVMConstIntGetZExtValue(val);
    if (truthy)
      *truthy = (raw != NY_IMM_NIL && raw != NY_IMM_FALSE && raw != 1);
    return true;
  }
  return false;
}
