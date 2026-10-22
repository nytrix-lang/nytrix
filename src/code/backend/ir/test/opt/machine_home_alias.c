/*
 * Regression: a cross-block SSA copy is coalesced into its dynamic source.
 */
#include "code/backend/ir/machine.h"
#include "support.h"

static int64_t alias_source = 21;

int main(void) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  char err[256] = {0};
  nyr_opt_begin(&ir);
  int source_address = nyr_opt_const(&ir, (int64_t)(intptr_t)&alias_source);
  int source = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                            .dst = -1,
                                            .a = source_address,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  int alias = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_COPY,
                                           .dst = -1,
                                           .a = source,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  int take_copy = nyr_opt_const(&ir, 1);
  nyr_opt_br_if(&ir, take_copy, 2);
  nyr_opt_br(&ir, 1);
  nyr_opt_label(&ir, 1);
  int zero = nyr_opt_const(&ir, 0);
  nyr_opt_ret(&ir, zero);
  nyr_opt_label(&ir, 2);
  int result = nyr_opt_binop(&ir, NYIR_ADD_I64, source, alias);
  nyr_opt_ret(&ir, result);

  bool ok = nyr_opt_verify(&ir, "machine-home-alias") &&
            nyr_opt_eval(&ir, "machine-home-alias") == 42 &&
            nyr_mach_lower_nir(&ir, &mach, 0, err, sizeof(err));
  int add_left = -1, add_right = -1, loaded_value = -1;
  for (size_t i = 0; ok && i < mach.inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach.insts[i];
    if (in->opcode == NYR_MACH_LOAD && in->dst.kind == NYR_MACH_OPERAND_VREG)
      loaded_value = (int)in->dst.as.reg;
    if (in->opcode == NYR_MACH_ADD && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->src0.kind == NYR_MACH_OPERAND_VREG &&
        in->src1.kind == NYR_MACH_OPERAND_VREG) {
      add_left = (int)in->src0.as.reg;
      add_right = (int)in->src1.as.reg;
    }
  }
  ok = ok && loaded_value >= 0 && add_left == loaded_value &&
       add_right == loaded_value;

  if (!ok)
    fprintf(stderr, "[machine-home-alias] %s\n",
            err[0] ? err
                   : "cross-block COPY was not coalesced into its source");
  else
    puts("[machine-home-alias] cross-block COPY coalesces with source");
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok ? 0 : 1;
}
