/*
 * A heap base plus (symbol address - heap base) must not be called private.
 */
#include "support.h"

extern bool nyr_loop_global_promote(nyr_func_t *f);

static void build_heap_to_symbol_alias(nyr_func_t *f) {
  nyr_opt_begin(f);
  int one = nyr_opt_const(f, 1);
  int zero = nyr_opt_const(f, 0);
  int heap = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .symbol = "rt_malloc_raw"});
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  int cond = nyr_opt_cmp(f, iv, one, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 20);

  nyr_opt_label(f, 11);
  int global = nyr_opt_emit_symbol(f, NYIR_ADDR_SYMBOL, "lgp_alias_global");
  int old_value = nyr_opt_emit_abc(f, NYIR_LOAD_I64, global, -1, -1);
  int delta = nyr_opt_binop(f, NYIR_SUB_I64, global, heap);
  int alias = nyr_opt_binop(f, NYIR_ADD_I64, heap, delta);
  (void)nyr_opt_emit_abc(f, NYIR_LOAD_I64, alias, -1, -1);
  int new_value = nyr_opt_binop(f, NYIR_ADD_I64, old_value, one);
  (void)nyr_opt_emit_abc(f, NYIR_STORE_I64, global, -1, new_value);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next;

  nyr_opt_label(f, 20);
  nyr_opt_ret(f, zero);
}

static void build_heap_offset_read(nyr_func_t *f, bool dynamic_offset) {
  nyr_opt_begin(f);
  int one = nyr_opt_const(f, 1);
  int zero = nyr_opt_const(f, 0);
  int heap = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .symbol = "rt_malloc_raw"});
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 30);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 31, zero);
  int cond = nyr_opt_cmp(f, iv, one, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 31);
  nyr_opt_br(f, 40);

  nyr_opt_label(f, 31);
  int offset = dynamic_offset ? iv : zero;
  int address = nyr_opt_binop(f, NYIR_ADD_I64, heap, offset);
  (void)nyr_opt_emit_abc(f, NYIR_LOAD_I64, address, -1, -1);
  int global = nyr_opt_emit_symbol(f, NYIR_ADDR_SYMBOL, "lgp_indexed_global");
  int old_value = nyr_opt_emit_abc(f, NYIR_LOAD_I64, global, -1, -1);
  int new_value = nyr_opt_binop(f, NYIR_ADD_I64, old_value, one);
  (void)nyr_opt_emit_abc(f, NYIR_STORE_I64, global, -1, new_value);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 30);
  f->data[iv_phi].phi_incoming[1].value = next;

  nyr_opt_label(f, 40);
  nyr_opt_ret(f, zero);
}

static bool has_local_store(const nyr_func_t *f) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_STORE_LOCAL)
      return true;
  return false;
}

static bool has_direct_symbol_store_in_block(const nyr_func_t *f,
                                             int64_t block_label,
                                             const char *symbol) {
  int64_t label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL) {
      label = in->imm;
      continue;
    }
    if (label != block_label || in->op != NYIR_STORE_I64)
      continue;
    for (size_t j = 0; j < f->len; ++j)
      if (f->data[j].dst == in->a && f->data[j].op == NYIR_ADDR_SYMBOL &&
          f->data[j].symbol && strcmp(f->data[j].symbol, symbol) == 0)
        return true;
  }
  return false;
}

int main(void) {
  nyr_func_t f;
  build_heap_to_symbol_alias(&f);
  if (!nyr_opt_verify(&f, "lgp-alias-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  if (!nyr_loop_global_promote(&f)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  bool promoted = has_local_store(&f);
  if (promoted) {
    fprintf(stderr, "[loop-global-promote] aliased symbol was promoted\n");
    nyr_ir_func_free(&f);
    return 1;
  }
  if (!has_direct_symbol_store_in_block(&f, 11, "lgp_alias_global")) {
    fprintf(stderr, "[loop-global-promote] alias case lost in-loop symbol store\n");
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);

  build_heap_offset_read(&f, true);
  if (!nyr_opt_verify(&f, "lgp-indexed-before") ||
      !nyr_loop_global_promote(&f)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  promoted = has_local_store(&f);
  if (promoted) {
    fprintf(stderr, "[loop-global-promote] dynamic offset was treated as private\n");
    nyr_ir_func_free(&f);
    return 1;
  }
  if (!has_direct_symbol_store_in_block(&f, 31, "lgp_indexed_global")) {
    fprintf(stderr,
            "[loop-global-promote] dynamic-offset case lost in-loop symbol store\n");
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);

  build_heap_offset_read(&f, false);
  if (!nyr_opt_verify(&f, "lgp-zero-offset-before") ||
      !nyr_loop_global_promote(&f)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  promoted = has_local_store(&f);
  nyr_ir_func_free(&f);
  if (!promoted) {
    fprintf(stderr, "[loop-global-promote] zero offset was not preserved\n");
    return 1;
  }
  puts("[loop-global-promote] alias and dynamic offset rejected; +0 accepted");
  return 0;
}
