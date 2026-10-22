/*
 * Direct global-slot reads are non-trapping invariant loads when their loop
 * has no memory writes or calls. LICM may move them to the preheader; a write
 * in the loop must keep the read in place.
 */
#include "support.h"

static int emit_symbol_address(nyr_func_t *f, const char *symbol) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .symbol = symbol,
                                    .scalar_class = NYIR_SCALAR_POINTER});
}

static int emit_load(nyr_func_t *f, int address) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                    .dst = -1,
                                    .a = address,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1});
}

static void emit_store(nyr_func_t *f, int address, int value) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = value,
                             .d = -1,
                             .e = -1,
                             .f = -1});
}

static bool build_loop(nyr_func_t *f, bool write_in_loop) {
  nyr_opt_begin(f);
  int address = emit_symbol_address(f, "licm_symbol_load_global");
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int limit = nyr_opt_const(f, 8);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, 0);
  int acc = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, 0);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_label(f, 12);
  nyr_opt_ret(f, acc);
  nyr_opt_label(f, 11);
  int value = emit_load(f, address);
  int next_acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, value);
  if (write_in_loop)
    emit_store(f, address, next_acc);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  /*
   * Patch the forward-declared backedge values in the two header PHIs.
   */
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    for (size_t k = 0; k < f->data[i].phi_incoming_len; ++k) {
      if (f->data[i].phi_incoming[k].predecessor_label != 11)
        continue;
      f->data[i].phi_incoming[k].value = i == 6 ? next_iv : next_acc;
    }
  }
  return nyr_alias_analysis(f) && nyr_opt_verify(f, "licm-symbol-load-before");
}

static size_t first_label(const nyr_func_t *f, int64_t label) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm == label)
      return i;
  return SIZE_MAX;
}

static size_t load_index(const nyr_func_t *f) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LOAD_I64)
      return i;
  return SIZE_MAX;
}

static bool run_case(bool write_in_loop) {
  const char *name =
      write_in_loop ? "licm-symbol-store-barrier" : "licm-symbol-slot-hoist";
  nyr_func_t f;
  if (!build_loop(&f, write_in_loop)) {
    nyr_ir_func_free(&f);
    return false;
  }
  size_t header_before = first_label(&f, 10);
  size_t load_before = load_index(&f);
  bool ok = header_before != SIZE_MAX && load_before > header_before &&
            nyr_licm(&f) && nyr_opt_verify(&f, name);
  size_t header_after = first_label(&f, 10);
  size_t load_after = load_index(&f);
  if (write_in_loop)
    ok = ok && load_after > header_after;
  else
    ok = ok && load_after < header_after;
  if (!ok) {
    fprintf(stderr, "[%s] wrong global-slot load motion\n", name);
    nyr_dump(stderr, &f, name);
  } else {
    printf("[%s] ok load=%zu header=%zu\n", name, load_after, header_after);
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) { return run_case(false) && run_case(true) ? 0 : 1; }
