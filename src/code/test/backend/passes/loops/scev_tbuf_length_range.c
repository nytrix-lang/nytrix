/*
 * Length-set/query bounds retain null-allocation behavior and stop at barriers.
 */
#include "code/backend/passes/verify/loop.h"
#include "opt.h"

static int call(nyr_func_t *f, const char *symbol, int a, int b,
                unsigned flags) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                     .dst = -1,
                                     .a = a,
                                     .b = b,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .symbol = symbol,
                                     .imm = b >= 0 ? 2 : 1,
                                     .flags = flags});
}

static bool check_case(int variant, bool eliminate) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  nyr_opt_label(&f, 1);
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int width = nyr_opt_const(&f, 8);
  int capacity = nyr_opt_const(&f, 4);
  int count = nyr_opt_const(&f, variant == 4 ? -1 : variant == 5 ? 5 : 4);
  int bytes = nyr_opt_const(&f, 32);
  int base = call(&f, variant == 8 ? "foreign_allocator" : "rt_list_new",
                  capacity, -1, variant == 8 ? NYIR_INST_F_EXTERN : 0);
  int length = -1;
  if (variant == 6)
    length = call(&f, "rt_tbuf_len_raw", base, -1, 0);
  int set =
      call(&f, variant == 7 ? "foreign_set_length" : "rt_list_set_len_raw",
           base, count, variant == 7 ? NYIR_INST_F_EXTERN : 0);
  if (variant == 2)
    (void)call(&f, "unknown_mutator", base, -1, 0);
  if (variant == 3)
    nyr_opt_emit_abc(&f, NYIR_STORE_I64, base, -1, zero);
  if (variant == 9) {
    nyr_opt_br(&f, 5);
    nyr_opt_label(&f, 5);
  }
  if (length < 0)
    length = call(&f, "rt_tbuf_len_raw", variant == 1 ? set : base, -1, 0);
  nyr_opt_br(&f, 2);
  nyr_opt_label(&f, 2);
  size_t phi = f.len;
  int iv = f.next_value;
  nyr_opt_phi(&f, variant == 9 ? 5 : 1, zero, 3, zero);
  int cond = nyr_opt_cmp(&f, iv, length, NYIR_CMP_LT);
  if (variant == 10) {
    int offset = nyr_opt_binop(&f, NYIR_MUL_I64, iv, width);
    nyr_opt_emit_abc(&f, NYIR_BOUNDS_CHECK, base, offset, bytes);
  }
  nyr_opt_br_if(&f, cond, 3);
  nyr_opt_br(&f, 4);
  nyr_opt_label(&f, 3);
  if (variant != 10) {
    int offset = nyr_opt_binop(&f, NYIR_MUL_I64, iv, width);
    nyr_opt_emit_abc(&f, NYIR_BOUNDS_CHECK, base, offset, bytes);
  }
  int next = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 2);
  nyr_opt_label(&f, 4);
  nyr_opt_ret(&f, iv);
  f.data[phi].phi_incoming[1].value = next;

  bool ok = nyr_opt_verify(&f, "length-range-before") &&
            nyr_bounds_check_elim(&f) &&
            nyr_opt_verify(&f, "length-range-after");
  size_t checks = 0, queries = 0;
  for (size_t i = 0; i < f.len; ++i) {
    checks += f.data[i].op == NYIR_BOUNDS_CHECK;
    queries += f.data[i].op == NYIR_CALL && f.data[i].symbol &&
               strcmp(f.data[i].symbol, "rt_tbuf_len_raw") == 0;
  }
  ok = ok && checks == (eliminate ? 0u : 1u) && queries == 1;
  if (!ok)
    fprintf(stderr, "[scev-tbuf-length] variant=%d checks=%zu queries=%zu\n",
            variant, checks, queries);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  for (int i = 0; i <= 10; ++i)
    if (!check_case(i, i <= 1))
      return 1;
  puts("[scev-tbuf-length] 11 setter/query, barrier, null, and guard cases "
       "pass");
  return 0;
}
