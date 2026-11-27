/*
 * Value-use edits must not reinterpret asm-goto labels as SSA values.
 */
#include "nyr/nyr.h"
#include "opt.h"

static bool check_label(int64_t kind, int label) {
  nyr_func_t f;
  nyr_func_init(&f);
  f.untagged_values = true;
  char err[256] = {0};
  nyr_opt_label(&f, 20000);
  int old_value = nyr_opt_const(&f, 0);
  int new_value = nyr_opt_const(&f, 0);
  int address = nyr_opt_emit_imm(&f, NYIR_ALLOCA, 8);
  nyr_inst_t store = nyr_opt_inst(NYIR_STORE_I64);
  store.a = address;
  store.c = old_value;
  bool ok = old_value >= 0 && new_value >= 0 && address >= 0 &&
            nyr_emit(&f, store, NULL, err, sizeof(err));
  nyr_inst_t assembly = nyr_opt_inst(NYIR_INLINE_ASM);
  assembly.a = kind == -14 ? old_value : address;
  assembly.b = kind == -83 ? -1 : new_value;
  assembly.c = label;
  assembly.imm = kind;
  assembly.symbol = kind == -14 ? "nop" : "q";
  size_t asm_index = f.len;
  ok = ok && nyr_emit(&f, assembly, NULL, err, sizeof(err));
  nyr_opt_label(&f, label);
  int loaded = nyr_opt_emit_abc(&f, NYIR_LOAD_I64, address, -1, -1);
  nyr_inst_t ret = nyr_opt_inst(NYIR_RET);
  ret.a = loaded;
  ok = ok && loaded >= 0 && nyr_emit(&f, ret, NULL, err, sizeof(err));
  nyr_use_def_t uses = {0};
  ok = ok && nyr_verify(&f, err, sizeof(err)) && nyr_build_use_def(&f, &uses);
  if (ok) {
    size_t asm_uses = 0;
    for (size_t i = uses.offsets[old_value]; i < uses.offsets[old_value + 1];
         ++i)
      asm_uses += uses.users[i] == asm_index;
    /*
     * Only the jump-label key is a use of v0, never the target L0.
     */
    ok = asm_uses == (kind == -14 ? 1u : 0u) &&
         nyr_replace_all_uses(&f, old_value, new_value) &&
         f.data[asm_index].c == label && nyr_verify(&f, err, sizeof(err));
  }
  if (!ok)
    fprintf(stderr, "[use-def-labels] kind=%lld target=%d: %s\n",
            (long long)kind, label,
            err[0] ? err : "label was counted or rewritten as a value");
  nyr_use_def_free(&uses);
  nyr_func_destroy(&f);
  return ok;
}

int main(void) {
  const int64_t kinds[] = {-14, -82, -83};
  for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i)
    if (!check_label(kinds[i], 0) || !check_label(kinds[i], 10000))
      return 1;
  return 0;
}
