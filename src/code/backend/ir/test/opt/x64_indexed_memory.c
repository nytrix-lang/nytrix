/*
 * Typed-buffer address chains should use x86-64 scaled-index memory forms.
 */
#include "code/backend/internal.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int indexed_address(nyr_func_t *f, int base, int index) {
  int scale = nyr_opt_const(f, 3);
  int scaled = nyr_opt_binop(f, NYIR_SHL_I64, index, scale);
  return nyr_opt_binop(f, NYIR_ADD_I64, base, scaled);
}

static size_t count_text(const char *text, const char *needle) {
  size_t count = 0;
  size_t length = strlen(needle);
  while ((text = strstr(text, needle)) != NULL) {
    ++count;
    text += length;
  }
  return count;
}

int main(void) {
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int base = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_ALLOCA,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = 64,
                                          .scalar_class = NYIR_SCALAR_POINTER});
  int index = nyr_opt_const(&ir, 2);
  int store_address = indexed_address(&ir, base, index);
  int value = nyr_opt_const(&ir, 42);
  (void)nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = store_address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
  int load_address = indexed_address(&ir, base, index);
  int loaded = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                            .dst = -1,
                                            .a = load_address,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  nyr_opt_ret(&ir, loaded);

  bool ok = nyr_opt_verify(&ir, "x64-indexed-memory");
  int64_t result = ok ? nyr_opt_eval(&ir, "x64-indexed-memory") : 0;
  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64,
      .abi = NY_NATIVE_ABI_SYSV,
      .object_format = "elf",
      .symbol_prefix = "",
  };
  nyr_writer_t assembly = {0};
  char error[256] = {0};
  ok = ok && result == 42 &&
       nyr_x86_64_emit_ir(&assembly, &target, &ir, "indexed_memory", false,
                          error, sizeof(error));
  size_t indexed_loads =
      assembly.data ? count_text(assembly.data, "(%rcx,%rax,8), %rax") : 0;
  size_t indexed_stores =
      assembly.data ? count_text(assembly.data, "%r11, (%rcx,%rax,8)") : 0;
  ok = ok && indexed_loads == 1 && indexed_stores == 1 &&
       (!assembly.data || strstr(assembly.data, "\tshlq\t$3,") == NULL);
  if (!ok) {
    fprintf(stderr,
            "[x64-indexed-memory] expected result 42, one scaled load and "
            "store (got result=%lld load=%zu store=%zu): %s\n%s",
            (long long)result, indexed_loads, indexed_stores,
            error[0] ? error : "assertion failed\n",
            assembly.data ? assembly.data : "<no assembly>\n");
  }
  free(assembly.data);
  nyr_ir_func_free(&ir);
  if (ok)
    puts("[x64-indexed-memory] scaled-index load/store verified");
  return ok ? 0 : 1;
}
