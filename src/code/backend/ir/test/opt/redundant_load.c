/*
 * Redundant-load elimination must reuse a value only while the memory fact
 * remains valid. In particular, unknown calls and unknown alias classes are
 * conservative barriers.
 */
#include "support.h"

#include <stdint.h>

static int64_t rle_cell_a = 17;
static int64_t rle_cell_b = 29;

static int emit_address(nyr_func_t *f, const void *address) {
  return nyr_opt_const(f, (int64_t)(intptr_t)address);
}

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

static void add_local_anchor(nyr_func_t *f) {
  int zero = nyr_opt_const(f, 0);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = zero,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 0});
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

static void emit_copy_struct(nyr_func_t *f, int destination, int source) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                             .dst = -1,
                             .a = destination,
                             .b = source,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 8,
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_MAY_TRAP});
}

static void emit_unknown_call(nyr_func_t *f) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                             .dst = -1,
                             .a = -1,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .symbol = "unknown_memory_effect",
                             .effects = NYIR_EFFECT_UNKNOWN_SIDE_EFFECT});
}

static bool prepare(nyr_func_t *f, const char *name) {
  char err[256] = {0};
  if (!nyr_alias_analysis(f) || !nyr_ir_verify(f, err, sizeof(err))) {
    fprintf(stderr, "[%s] setup failed: %s\n", name, err);
    return false;
  }
  return true;
}

static int count_loads(const nyr_func_t *f) {
  int count = 0;
  for (size_t i = 0; i < f->len; ++i)
    count += f->data[i].op == NYIR_LOAD_I64;
  return count;
}

static bool run_positive(void) {
  const char *name = "rle-identical-address";
  nyr_func_t f;
  nyr_opt_begin(&f);
  add_local_anchor(&f);
  int address = emit_address(&f, &rle_cell_a);
  int first = emit_load(&f, address);
  int second = emit_load(&f, address);
  int sum = nyr_opt_binop(&f, NYIR_ADD_I64, first, second);
  nyr_opt_ret(&f, sum);
  if (!prepare(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = nyr_opt_eval(&f, name);
  bool ok = nyr_redundant_load_elim(&f) && nyr_opt_verify(&f, name) &&
            count_loads(&f) == 1;
  long long after = ok ? nyr_opt_eval(&f, name) : 0;
  if (!ok) {
    fprintf(stderr, "[%s] expected one load after forwarding\n", name);
    nyr_dump(stderr, &f, name);
  } else {
    nyr_opt_check(name, before, after, (long long)rle_cell_a * 2);
  }
  nyr_ir_func_free(&f);
  return ok && before == after;
}

static bool run_unknown_call_barrier(void) {
  const char *name = "rle-unknown-call-barrier";
  nyr_func_t f;
  nyr_opt_begin(&f);
  add_local_anchor(&f);
  int address = emit_address(&f, &rle_cell_a);
  int first = emit_load(&f, address);
  emit_unknown_call(&f);
  int second = emit_load(&f, address);
  int sum = nyr_opt_binop(&f, NYIR_ADD_I64, first, second);
  nyr_opt_ret(&f, sum);
  if (!prepare(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  bool ok = nyr_redundant_load_elim(&f) && nyr_opt_verify(&f, name) &&
            count_loads(&f) == 2;
  if (!ok)
    fprintf(stderr, "[%s] unknown call failed to invalidate load fact\n", name);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_raw_memset_call_barrier(void) {
  const char *name = "rle-raw-memset-call-barrier";
  nyr_func_t f;
  nyr_opt_begin(&f);
  add_local_anchor(&f);
  int tracked = emit_symbol_address(&f, "rle.memset.tracked");
  int other = emit_symbol_address(&f, "rle.memset.other");
  int fill = nyr_opt_const(&f, 0x5a);
  int length = nyr_opt_const(&f, 8);
  int first = emit_load(&f, tracked);
  nyr_ir_emit(&f, (nyr_inst_t){
                    .op = NYIR_CALL,
                    .dst = -1,
                    .a = other,
                    .b = fill,
                    .c = length,
                    .d = -1,
                    .e = -1,
                    .f = -1,
                    .imm = 3,
                    .symbol = "rt_memset_raw",
                });
  size_t call_at = f.len - 1;
  int second = emit_load(&f, tracked);
  int sum = nyr_opt_binop(&f, NYIR_ADD_I64, first, second);
  nyr_opt_ret(&f, sum);
  if (!prepare(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  bool ok = tracked >= 0 && other >= 0 && fill >= 0 && length >= 0 &&
            first >= 0 && second >= 0 &&
            nyr_call_effect_summary(&f.data[call_at]) ==
                (NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY |
                 NYIR_EFFECT_MAY_TRAP) &&
            nyr_redundant_load_elim(&f) && nyr_opt_verify(&f, name) &&
            count_loads(&f) == 2;
  if (!ok)
    fprintf(stderr,
            "[%s] bounded-looking raw write reused a possibly aliased load\n",
            name);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_unknown_alias_store_barrier(void) {
  const char *name = "rle-unknown-alias-store-barrier";
  nyr_func_t f;
  nyr_opt_begin(&f);
  add_local_anchor(&f);
  int address_a = emit_address(&f, &rle_cell_a);
  int address_b = emit_address(&f, &rle_cell_b);
  int first = emit_load(&f, address_a);
  int replacement = nyr_opt_const(&f, 41);
  emit_store(&f, address_b, replacement);
  int second = emit_load(&f, address_a);
  int sum = nyr_opt_binop(&f, NYIR_ADD_I64, first, second);
  nyr_opt_ret(&f, sum);
  if (!prepare(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  bool ok = nyr_redundant_load_elim(&f) && nyr_opt_verify(&f, name) &&
            count_loads(&f) == 2;
  if (!ok)
    fprintf(stderr, "[%s] unknown alias store reused a stale load\n", name);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_copy_struct_alias_barrier(const char *name,
                                          bool unknown_destination,
                                          bool destination_aliases_cached) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  add_local_anchor(&f);
  int tracked = emit_symbol_address(&f, "rle.copy.tracked");
  int other = emit_symbol_address(&f, "rle.copy.other");
  int same_symbol = emit_symbol_address(&f, "rle.copy.tracked");
  int unknown = nyr_opt_const(&f, (int64_t)(intptr_t)&rle_cell_b);
  int destination = unknown_destination          ? unknown
                    : destination_aliases_cached ? same_symbol
                                                 : other;
  int source = destination_aliases_cached ? other : tracked;
  int stored = nyr_opt_const(&f, 71);
  emit_store(&f, tracked, stored);
  emit_copy_struct(&f, destination, source);
  int loaded = emit_load(&f, tracked);
  nyr_opt_ret(&f, loaded);
  if (!prepare(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  bool should_forward = !unknown_destination && !destination_aliases_cached;
  bool ok = nyr_redundant_load_elim(&f) && nyr_opt_verify(&f, name) &&
            count_loads(&f) == (should_forward ? 0 : 1);
  if (!ok) {
    fprintf(
        stderr,
        "[%s] wrong destination alias invalidation: unknown=%d aliases=%d\n",
        name, unknown_destination, destination_aliases_cached);
    nyr_dump(stderr, &f, name);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_loaded_pointer_has_unknown_value_provenance(void) {
  const char *name = "rle-loaded-pointer-value-provenance-unknown";
  nyr_func_t f;
  nyr_opt_begin(&f);
  add_local_anchor(&f);
  int loaded_from = emit_symbol_address(&f, "rle.copy.other");
  int tracked = emit_symbol_address(&f, "rle.copy.tracked");
  int loaded_pointer = emit_load(&f, loaded_from);
  f.data[f.len - 1].scalar_class = NYIR_SCALAR_POINTER;
  int stored = nyr_opt_const(&f, 83);
  emit_store(&f, tracked, stored);
  /*
   * The load's alias class describes loaded_from, not the pointer value in
   * loaded_pointer. Treating those as the same fact would wrongly preserve
   * the cached tracked value across this potentially aliasing copy.
   */
  emit_copy_struct(&f, loaded_pointer, loaded_from);
  int reloaded = emit_load(&f, tracked);
  nyr_opt_ret(&f, reloaded);
  if (!prepare(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  bool ok = nyr_redundant_load_elim(&f) && nyr_opt_verify(&f, name) &&
            count_loads(&f) == 2;
  if (!ok) {
    fprintf(
        stderr,
        "[%s] load-address provenance was reused as loaded-value provenance\n",
        name);
    nyr_dump(stderr, &f, name);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_local_forward_uses_source_value_provenance(void) {
  const char *name = "rle-local-forward-source-value-provenance";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int pointer = emit_symbol_address(&f, "rle.local.pointer");
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = pointer,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0});
  size_t load_at = f.len;
  int loaded =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = 0,
                                  .scalar_class = NYIR_SCALAR_POINTER});
  nyr_opt_ret(&f, loaded);
  if (!prepare(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  uint32_t source_alias = f.data[(size_t)pointer].alias_class;
  /*
   * Simulate stale slot-access metadata left on the old load.
   */
  f.data[load_at].alias_class = source_alias + 1;
  bool ok = source_alias != 0 && nyr_redundant_load_elim(&f) &&
            nyr_opt_verify(&f, name) && f.data[load_at].op == NYIR_COPY &&
            f.data[load_at].a == pointer &&
            f.data[load_at].alias_class == source_alias;
  if (!ok) {
    fprintf(stderr,
            "[%s] copy did not carry source provenance (source=%u op=%s "
            "alias=%u)\n",
            name, source_alias, nyr_op_name(f.data[load_at].op),
            f.data[load_at].alias_class);
    nyr_dump(stderr, &f, name);
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  return run_positive() && run_unknown_call_barrier() &&
                 run_raw_memset_call_barrier() &&
                 run_unknown_alias_store_barrier() &&
                 run_copy_struct_alias_barrier("copy-struct-disjoint-dst",
                                               false, false) &&
                 run_copy_struct_alias_barrier("copy-struct-aliasing-dst",
                                               false, true) &&
                 run_copy_struct_alias_barrier("copy-struct-unknown-dst", true,
                                               false) &&
                 run_loaded_pointer_has_unknown_value_provenance() &&
                 run_local_forward_uses_source_value_provenance()
             ? 0
             : 1;
}
