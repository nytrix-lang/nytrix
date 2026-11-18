/*
 * Discarding an unused C read must preserve volatile reads, live reads and
 * the trapping semantics of the native language. Speculation facts survive.
 */
#include "nyr/nyr.h"
#include "opt.h"

static bool check(bool (*pass)(nyr_func_t *), bool c_mode, bool is_volatile,
                    bool live) {
  nyr_func_t f;
  nyr_func_init(&f);
  f.untagged_values = c_mode;
  char err[256] = {0};
  int address = nyr_opt_emit_imm(&f, NYIR_ALLOCA, 8);
  int result = nyr_opt_const(&f, 7);
  nyr_inst_t store = nyr_opt_inst(NYIR_STORE_I64);
  store.a = address;
  store.c = result;
  bool initialized = nyr_emit(&f, store, NULL, err, sizeof(err));
  nyr_inst_t load = nyr_opt_inst(NYIR_LOAD_I64);
  load.a = address;
  load.effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP;
  if (is_volatile) {
    load.effects |= NYIR_EFFECT_VOLATILE;
    load.flags |= NYIR_INST_F_MEM_VOLATILE;
  }
  int loaded = nyr_ir_emit(&f, load);
  nyr_inst_t ret = nyr_opt_inst(NYIR_RET);
  ret.a = live ? loaded : result;
  bool ok = initialized && loaded >= 0 &&
            nyr_emit(&f, ret, NULL, err, sizeof(err)) &&
            nyr_verify(&f, err, sizeof(err)) && pass(&f) &&
            nyr_verify(&f, err, sizeof(err));
  size_t remaining = 0;
  for (size_t i = 0; ok && i < f.len; ++i) {
    if (f.data[i].op == NYIR_LOAD_I64) {
      ++remaining;
      ok = (nyr_effective_effects(&f.data[i]) & NYIR_EFFECT_MAY_TRAP) != 0;
    }
  }
  ok = ok && remaining == (size_t)(!c_mode || is_volatile || live);
  if (!ok)
    fprintf(stderr, "C load DCE mismatch: c=%d volatile=%d live=%d: %s\n",
            c_mode, is_volatile, live, err);
  nyr_func_destroy(&f);
  return ok;
}

int main(void) {
  bool (*passes[])(nyr_func_t *) = {nyr_dce, nyr_adce};
  for (size_t p = 0; p < sizeof(passes) / sizeof(passes[0]); ++p)
    for (int c = 0; c < 2; ++c)
      for (int v = 0; v < 2; ++v)
        for (int l = 0; l < 2; ++l)
          if (!check(passes[p], c, v, l)) return 1;
  return 0;
}
