/*
 * The descriptor-table store form has no NYIR result, even though inline asm
 * normally receives an automatically allocated destination.
 */
#include <nyr/ir.h>
#include <stdio.h>

int main(void) {
  nyr_func_t func = {0};
  int result = nyr_ir_emit(&func, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                               .dst = -1,
                                               .a = 0,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = -152,
                                               .symbol = "sldt %0"});
  bool ok = result == -1 && func.len == 1 && func.next_value == 0 &&
            func.data[0].dst == -1;
  if (!ok)
    fprintf(stderr,
            "[sldt-inline-asm] emitted a spurious destination: result=%d "
            "next_value=%d dst=%d\n",
            result, func.next_value, func.len ? func.data[0].dst : -2);
  nyr_ir_func_free(&func);
  return ok ? 0 : 1;
}
