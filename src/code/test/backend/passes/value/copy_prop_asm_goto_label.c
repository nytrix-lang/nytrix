/*
 * Copy propagation must treat asm-goto's .c operand as a label, not an SSA
 * value, even when the label number overlaps a value with a copy alias.
 */
#include "opt.h"

int main(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);

  int value = nyr_opt_const(&f, 7);
  int alias = nyr_opt_emit_abc(&f, NYIR_COPY, value, -1, -1);
  int label_alias = nyr_opt_emit_abc(&f, NYIR_COPY, value, -1, -1);
  const int target_label = label_alias;

  size_t asm_at = f.len;
  nyr_ir_emit(
      &f, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                       .dst = -1,
                       .a = value,
                       .b = alias,
                       .c = target_label,
                       .d = -1,
                       .e = -1,
                       .f = -1,
                       .imm = -14,
                       .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE,
                       .symbol = "jmp %l0"});
  nyr_opt_label(&f, target_label);
  nyr_opt_ret(&f, alias);

  bool ok =
      target_label == 2 && nyr_opt_verify(&f, "copy-prop-asm-goto/before") &&
      nyr_copy_prop(&f) && nyr_opt_verify(&f, "copy-prop-asm-goto/after") &&
      f.data[asm_at].c == target_label;
  if (!ok)
    nyr_dump(stderr, &f, "copy-prop-asm-goto");
  nyr_ir_func_free(&f);
  return ok ? 0 : 1;
}
