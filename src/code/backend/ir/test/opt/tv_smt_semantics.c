/*
 * The Z3 translation evaluator must preserve NYIR's hardware-masked shifts.
 */
#include "code/backend/ir/opt/loop.h"
#include "support.h"
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <time.h>

static int load_local(nyr_func_t *f, int64_t slot) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = slot});
}

static int emit_load_i64(nyr_func_t *f, int address);

static bool build_alloca_case(nyr_func_t *f, int64_t size, int64_t offset,
                              int64_t stored) {
  nyr_opt_begin(f);
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = size,
                                        .scalar_class = NYIR_SCALAR_POINTER});
  int displacement = nyr_opt_const(f, offset);
  int address = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ADD_I64,
                                           .dst = -1,
                                           .a = base,
                                           .b = displacement,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  int value = nyr_opt_const(f, stored);
  if (base < 0 || displacement < 0 || address < 0 || value < 0)
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = address,
                                   .b = -1,
                                   .c = value,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  int result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = address,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-alloca");
}

static bool checks_alloca_extent_and_offsets(void) {
  nyr_func_t valid = {0}, same = {0}, changed = {0}, at_end = {0},
              one_past = {0}, negative = {0}, mismatched_size = {0};
  bool built = build_alloca_case(&valid, 16, 8, 17) &&
               build_alloca_case(&same, 16, 8, 17) &&
               build_alloca_case(&changed, 16, 8, 18) &&
               build_alloca_case(&at_end, 16, 8, 17) &&
               build_alloca_case(&one_past, 16, 16, 17) &&
               build_alloca_case(&negative, 16, -1, 17) &&
               build_alloca_case(&mismatched_size, 8, 0, 17);
  char err[256] = {0};
  bool equivalent = built && nyr_tv_smt_equiv(&valid, &same, err, sizeof(err));
  err[0] = '\0';
  bool rejects_changed_value =
      built && !nyr_tv_smt_equiv(&valid, &changed, err, sizeof(err));
  err[0] = '\0';
  bool rejects_one_past =
      built && !nyr_tv_smt_equiv(&at_end, &one_past, err, sizeof(err));
  err[0] = '\0';
  bool rejects_negative =
      built && !nyr_tv_smt_equiv(&at_end, &negative, err, sizeof(err));
  err[0] = '\0';
  bool shape_inapplicable =
      built && !nyr_tv_smt_equiv(&valid, &mismatched_size, err, sizeof(err)) &&
      err[0] == '\0';
  if (!built || !equivalent || !rejects_changed_value || !rejects_one_past ||
      !rejects_negative || !shape_inapplicable)
    fprintf(stderr,
            "[tv-smt-alloca] built=%s equivalent=%s changed=%s one-past=%s "
            "negative=%s shape-inapplicable=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_changed_value ? "rejected" : "not rejected",
            rejects_one_past ? "rejected" : "not rejected",
            rejects_negative ? "rejected" : "not rejected",
            shape_inapplicable ? "yes" : "no", err);
  nyr_ir_func_free(&valid);
  nyr_ir_func_free(&same);
  nyr_ir_func_free(&changed);
  nyr_ir_func_free(&at_end);
  nyr_ir_func_free(&one_past);
  nyr_ir_func_free(&negative);
  nyr_ir_func_free(&mismatched_size);
  return built && equivalent && rejects_changed_value && rejects_one_past &&
         rejects_negative && shape_inapplicable;
}

static bool build_alloca_vec8_roundtrip(nyr_func_t *f, int64_t offset) {
  nyr_opt_begin(f);
  int allocation =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 32,
                                 .scalar_class = NYIR_SCALAR_POINTER});
  int displacement = nyr_opt_const(f, offset);
  int address = nyr_opt_binop(f, NYIR_ADD_I64, allocation, displacement);
  int loaded = address < 0
                   ? -1
                   : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_LOAD_I64,
                                                .dst = -1,
                                                .a = address,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
  if (allocation < 0 || displacement < 0 || address < 0 || loaded < 0)
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_STORE_I64,
                                   .dst = -1,
                                   .a = address,
                                   .b = loaded,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  int result = nyr_opt_const(f, 0);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-alloca-vec8");
}

static bool checks_alloca_vec8_extent(void) {
  nyr_func_t valid = {0}, same = {0}, out_of_bounds = {0};
  bool built = build_alloca_vec8_roundtrip(&valid, 0) &&
               build_alloca_vec8_roundtrip(&same, 0) &&
               build_alloca_vec8_roundtrip(&out_of_bounds, 8);
  char err[256] = {0};
  bool equivalent = built && nyr_tv_smt_equiv(&valid, &same, err, sizeof(err));
  err[0] = '\0';
  bool rejects_out_of_bounds =
      built && !nyr_tv_smt_equiv(&valid, &out_of_bounds, err, sizeof(err));
  if (!built || !equivalent || !rejects_out_of_bounds)
    fprintf(stderr,
            "[tv-smt-alloca-vec8] built=%s equivalent=%s "
            "32-byte-out-of-bounds=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_out_of_bounds ? "rejected" : "not rejected", err);
  nyr_ir_func_free(&valid);
  nyr_ir_func_free(&same);
  nyr_ir_func_free(&out_of_bounds);
  return built && equivalent && rejects_out_of_bounds;
}

static bool build_cfg_alloca_case(nyr_func_t *f, int64_t offset,
                                  int64_t stored) {
  nyr_opt_begin(f);
  int allocation =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 16,
                                 .scalar_class = NYIR_SCALAR_POINTER});
  int displacement = nyr_opt_const(f, offset);
  int address = nyr_opt_binop(f, NYIR_ADD_I64, allocation, displacement);
  int value = nyr_opt_const(f, stored);
  int condition = load_local(f, 0);
  if (allocation < 0 || displacement < 0 || address < 0 || value < 0 ||
      condition < 0)
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = address,
                                   .b = -1,
                                   .c = value,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  nyr_opt_br_if(f, condition, 1);
  int fallthrough = emit_load_i64(f, address);
  if (fallthrough < 0)
    return false;
  nyr_opt_ret(f, fallthrough);
  nyr_opt_label(f, 1);
  int taken = emit_load_i64(f, address);
  if (taken < 0)
    return false;
  nyr_opt_ret(f, taken);
  return nyr_opt_verify(f, "tv-smt-cfg-alloca");
}

static bool checks_cfg_alloca_extent(void) {
  nyr_func_t valid = {0}, same = {0}, changed = {0}, at_end = {0},
              one_past = {0};
  bool built = build_cfg_alloca_case(&valid, 8, 17) &&
               build_cfg_alloca_case(&same, 8, 17) &&
               build_cfg_alloca_case(&changed, 8, 18) &&
               build_cfg_alloca_case(&at_end, 8, 17) &&
               build_cfg_alloca_case(&one_past, 16, 17);
  char err[256] = {0};
  bool equivalent = built && nyr_tv_smt_equiv(&valid, &same, err, sizeof(err));
  err[0] = '\0';
  bool rejects_value_change =
      built && !nyr_tv_smt_equiv(&valid, &changed, err, sizeof(err));
  err[0] = '\0';
  bool rejects_oob =
      built && !nyr_tv_smt_equiv(&at_end, &one_past, err, sizeof(err));
  if (!built || !equivalent || !rejects_value_change || !rejects_oob)
    fprintf(stderr,
            "[tv-smt-cfg-alloca] built=%s equivalent=%s value-change=%s "
            "out-of-bounds=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_value_change ? "rejected" : "not rejected",
            rejects_oob ? "rejected" : "not rejected", err);
  nyr_ir_func_free(&valid);
  nyr_ir_func_free(&same);
  nyr_ir_func_free(&changed);
  nyr_ir_func_free(&at_end);
  nyr_ir_func_free(&one_past);
  return built && equivalent && rejects_value_change && rejects_oob;
}

static bool build_copy_struct_case(nyr_func_t *f, bool manual_copy,
                                   bool change_second_word, bool with_branch) {
  nyr_opt_begin(f);
  int destination =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 16,
                                 .scalar_class = NYIR_SCALAR_POINTER});
  int source = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = 16,
                                          .scalar_class = NYIR_SCALAR_POINTER});
  if (destination < 0 || source < 0)
    return false;
  int offset = nyr_opt_const(f, 8);
  int destination_second = nyr_opt_binop(f, NYIR_ADD_I64, destination, offset);
  int source_second = nyr_opt_binop(f, NYIR_ADD_I64, source, offset);
  if (offset < 0 || destination_second < 0 || source_second < 0)
    return false;
  int first_value = nyr_opt_const(f, 17);
  int second_value = nyr_opt_const(f, 29);
  if (first_value < 0 || second_value < 0)
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = source,
                                   .b = -1,
                                   .c = first_value,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = source_second,
                                   .b = -1,
                                   .c = second_value,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  if (with_branch) {
    int condition = load_local(f, 0);
    int zero = nyr_opt_const(f, 0);
    if (condition < 0 || zero < 0)
      return false;
    nyr_opt_br_if(f, condition, 1);
    nyr_opt_ret(f, zero);
    nyr_opt_label(f, 1);
  }
  if (manual_copy) {
    int first = emit_load_i64(f, source);
    int second = emit_load_i64(f, change_second_word ? source : source_second);
    if (first < 0 || second < 0)
      return false;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = destination,
                                     .b = -1,
                                     .c = first,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = destination_second,
                                     .b = -1,
                                     .c = second,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
  } else {
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                                     .dst = -1,
                                     .a = destination,
                                     .b = source,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = 16});
    for (unsigned i = 0; i < 3; ++i)
      (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_NOP,
                                       .dst = -1,
                                       .a = -1,
                                       .b = -1,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1});
  }
  int first_result = emit_load_i64(f, destination);
  int second_result = emit_load_i64(f, destination_second);
  int result =
      first_result < 0 || second_result < 0
          ? -1
          : nyr_opt_binop(f, NYIR_ADD_I64, first_result, second_result);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, with_branch ? "tv-smt-cfg-copy-struct"
                                       : "tv-smt-copy-struct");
}

static bool checks_copy_struct_memory_model(void) {
  nyr_func_t copy = {0}, manual = {0}, changed = {0}, cfg_copy = {0},
              cfg_manual = {0}, cfg_changed = {0};
  bool built = build_copy_struct_case(&copy, false, false, false) &&
               build_copy_struct_case(&manual, true, false, false) &&
               build_copy_struct_case(&changed, true, true, false) &&
               build_copy_struct_case(&cfg_copy, false, false, true) &&
               build_copy_struct_case(&cfg_manual, true, false, true) &&
               build_copy_struct_case(&cfg_changed, true, true, true);
  char straightline_err[256] = {0};
  char straightline_change_err[256] = {0};
  char cfg_err[256] = {0};
  char cfg_change_err[256] = {0};
  bool straightline_equivalent =
      built && nyr_tv_smt_equiv(&copy, &manual, straightline_err,
                                 sizeof(straightline_err));
  bool straightline_rejects_changed =
      built && !nyr_tv_smt_equiv(&copy, &changed, straightline_change_err,
                                  sizeof(straightline_change_err));
  bool cfg_equivalent = built && nyr_tv_smt_equiv(&cfg_copy, &cfg_manual,
                                                   cfg_err, sizeof(cfg_err));
  bool cfg_rejects_changed =
      built && !nyr_tv_smt_equiv(&cfg_copy, &cfg_changed, cfg_change_err,
                                  sizeof(cfg_change_err));
  if (!built || !straightline_equivalent || !straightline_rejects_changed ||
      !cfg_equivalent || !cfg_rejects_changed)
    fprintf(
        stderr,
        "[tv-smt-copy-struct] built=%s straightline=%s/%s (%s) cfg=%s/%s "
        "(%s)\n",
        built ? "yes" : "no", straightline_equivalent ? "equivalent" : "failed",
        straightline_rejects_changed ? "change-rejected" : "change-accepted",
        straightline_err[0]          ? straightline_err
        : straightline_change_err[0] ? straightline_change_err
                                     : "no solver error",
        cfg_equivalent ? "equivalent" : "failed",
        cfg_rejects_changed ? "change-rejected" : "change-accepted",
        cfg_err[0]          ? cfg_err
        : cfg_change_err[0] ? cfg_change_err
                            : "no solver error");
  nyr_ir_func_free(&copy);
  nyr_ir_func_free(&manual);
  nyr_ir_func_free(&changed);
  nyr_ir_func_free(&cfg_copy);
  nyr_ir_func_free(&cfg_manual);
  nyr_ir_func_free(&cfg_changed);
  return built && straightline_equivalent && straightline_rejects_changed &&
         cfg_equivalent && cfg_rejects_changed;
}

static bool build_symbol_load_case(nyr_func_t *f, const char *symbol,
                                   bool with_branch) {
  nyr_opt_begin(f);
  if (with_branch) {
    int condition = load_local(f, 0);
    int zero = nyr_opt_const(f, 0);
    if (condition < 0 || zero < 0)
      return false;
    nyr_opt_br_if(f, condition, 1);
    nyr_opt_ret(f, zero);
    nyr_opt_label(f, 1);
  }
  int address =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .symbol = symbol,
                                 .scalar_class = NYIR_SCALAR_POINTER});
  int value = address < 0 ? -1 : emit_load_i64(f, address);
  if (address < 0 || value < 0)
    return false;
  nyr_opt_ret(f, value);
  return nyr_opt_verify(f, with_branch ? "tv-smt-cfg-symbol" : "tv-smt-symbol");
}

static bool checks_symbol_address_model(void) {
  nyr_func_t straightline = {0}, same = {0}, changed = {0}, cfg = {0},
              cfg_same = {0}, cfg_changed = {0};
  bool built = build_symbol_load_case(&straightline, "tv_symbol_a", false) &&
               build_symbol_load_case(&same, "tv_symbol_a", false) &&
               build_symbol_load_case(&changed, "tv_symbol_b", false) &&
               build_symbol_load_case(&cfg, "tv_symbol_a", true) &&
               build_symbol_load_case(&cfg_same, "tv_symbol_a", true) &&
               build_symbol_load_case(&cfg_changed, "tv_symbol_b", true);
  char straightline_err[256] = {0};
  char straightline_change_err[256] = {0};
  char cfg_err[256] = {0};
  char cfg_change_err[256] = {0};
  bool straightline_equivalent =
      built && nyr_tv_smt_equiv(&straightline, &same, straightline_err,
                                 sizeof(straightline_err));
  bool straightline_rejects_changed =
      built &&
      !nyr_tv_smt_equiv(&straightline, &changed, straightline_change_err,
                         sizeof(straightline_change_err));
  bool cfg_equivalent =
      built && nyr_tv_smt_equiv(&cfg, &cfg_same, cfg_err, sizeof(cfg_err));
  bool cfg_rejects_changed =
      built && !nyr_tv_smt_equiv(&cfg, &cfg_changed, cfg_change_err,
                                  sizeof(cfg_change_err));
  if (!built || !straightline_equivalent || !straightline_rejects_changed ||
      !cfg_equivalent || !cfg_rejects_changed)
    fprintf(
        stderr,
        "[tv-smt-symbol] built=%s straightline=%s/%s cfg=%s/%s "
        "errors=%s/%s/%s/%s\n",
        built ? "yes" : "no", straightline_equivalent ? "equivalent" : "failed",
        straightline_rejects_changed ? "change-rejected" : "change-accepted",
        cfg_equivalent ? "equivalent" : "failed",
        cfg_rejects_changed ? "change-rejected" : "change-accepted",
        straightline_err, straightline_change_err, cfg_err, cfg_change_err);
  nyr_ir_func_free(&straightline);
  nyr_ir_func_free(&same);
  nyr_ir_func_free(&changed);
  nyr_ir_func_free(&cfg);
  nyr_ir_func_free(&cfg_same);
  nyr_ir_func_free(&cfg_changed);
  return built && straightline_equivalent && straightline_rejects_changed &&
         cfg_equivalent && cfg_rejects_changed;
}

static int emit_addr_local(nyr_func_t *f, int64_t slot) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = slot,
                                    .scalar_class = NYIR_SCALAR_POINTER});
}

static bool build_addr_local_load(nyr_func_t *f, bool one_past) {
  nyr_opt_begin(f);
  int address = emit_addr_local(f, 0);
  if (one_past) {
    int offset = nyr_opt_const(f, 8);
    address = address < 0 || offset < 0
                  ? -1
                  : nyr_opt_binop(f, NYIR_ADD_I64, address, offset);
  }
  int value = address < 0 ? -1 : emit_load_i64(f, address);
  if (address < 0 || value < 0)
    return false;
  nyr_opt_ret(f, value);
  return nyr_opt_verify(f, "tv-smt-addr-local-load");
}

static bool build_addr_local_store(nyr_func_t *f, bool through_address,
                                   bool with_branch) {
  nyr_opt_begin(f);
  int address = through_address ? emit_addr_local(f, 0) : -1;
  int value = load_local(f, 1);
  if ((through_address && address < 0) || value < 0)
    return false;
  if (with_branch) {
    int condition = load_local(f, 2);
    if (condition < 0)
      return false;
    nyr_opt_br_if(f, condition, 1);
    int fallthrough = load_local(f, 0);
    if (fallthrough < 0)
      return false;
    nyr_opt_ret(f, fallthrough);
    nyr_opt_label(f, 1);
  }
  if (through_address) {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    if (f->len == old_len)
      return false;
  } else {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                     .dst = -1,
                                     .a = value,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = 0});
    if (f->len == old_len)
      return false;
  }
  int result = load_local(f, 0);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, with_branch ? "tv-smt-cfg-addr-local-store"
                                       : "tv-smt-addr-local-store");
}

static bool checks_addr_local_memory_model(void) {
  nyr_func_t address_load = {0}, local_load = {0}, out_of_bounds = {0},
              address_store = {0}, local_store = {0}, cfg_address_store = {0},
              cfg_local_store = {0};
  bool built = build_addr_local_load(&address_load, false) &&
               build_addr_local_load(&local_load, false) &&
               build_addr_local_load(&out_of_bounds, true) &&
               build_addr_local_store(&address_store, true, false) &&
               build_addr_local_store(&local_store, false, false) &&
               build_addr_local_store(&cfg_address_store, true, true) &&
               build_addr_local_store(&cfg_local_store, false, true);
  char load_err[256] = {0}, bounds_err[256] = {0}, store_err[256] = {0},
       cfg_err[256] = {0};
  bool load_equivalent = built && nyr_tv_smt_equiv(&address_load, &local_load,
                                                    load_err, sizeof(load_err));
  bool rejects_out_of_bounds =
      built && !nyr_tv_smt_equiv(&address_load, &out_of_bounds, bounds_err,
                                  sizeof(bounds_err));
  bool store_equivalent =
      built && nyr_tv_smt_equiv(&address_store, &local_store, store_err,
                                 sizeof(store_err));
  bool cfg_store_equivalent =
      built && nyr_tv_smt_equiv(&cfg_address_store, &cfg_local_store, cfg_err,
                                 sizeof(cfg_err));
  if (!built || !load_equivalent || !rejects_out_of_bounds ||
      !store_equivalent || !cfg_store_equivalent)
    fprintf(stderr,
            "[tv-smt-addr-local] built=%s load=%s bounds=%s store=%s cfg=%s "
            "errors=%s/%s/%s/%s\n",
            built ? "yes" : "no", load_equivalent ? "equivalent" : "failed",
            rejects_out_of_bounds ? "rejected" : "accepted",
            store_equivalent ? "equivalent" : "failed",
            cfg_store_equivalent ? "equivalent" : "failed", load_err,
            bounds_err, store_err, cfg_err);
  nyr_ir_func_free(&address_load);
  nyr_ir_func_free(&local_load);
  nyr_ir_func_free(&out_of_bounds);
  nyr_ir_func_free(&address_store);
  nyr_ir_func_free(&local_store);
  nyr_ir_func_free(&cfg_address_store);
  nyr_ir_func_free(&cfg_local_store);
  return built && load_equivalent && rejects_out_of_bounds &&
         store_equivalent && cfg_store_equivalent;
}

static bool build_cfg_loop_alloca_case(nyr_func_t *f) {
  nyr_opt_begin(f);
  int condition = load_local(f, 0);
  if (condition < 0)
    return false;
  nyr_opt_label(f, 1);
  int allocation =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 8,
                                 .scalar_class = NYIR_SCALAR_POINTER});
  if (allocation < 0)
    return false;
  nyr_opt_br_if(f, condition, 2);
  nyr_opt_br(f, 1);
  nyr_opt_label(f, 2);
  int result = nyr_opt_const(f, 0);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-cfg-loop-alloca");
}

static bool build_counted_cfg_alloca_case(nyr_func_t *f, int64_t trips,
                                          int64_t stored) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int limit = nyr_opt_const(f, trips);
  if (zero < 0 || one < 0 || limit < 0)
    return false;
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  size_t result_phi = f->len;
  int result = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  int condition = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  if (condition < 0)
    return false;
  nyr_opt_br_if(f, condition, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 11);
  int allocation =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 8,
                                 .scalar_class = NYIR_SCALAR_POINTER});
  int value = nyr_opt_const(f, stored);
  if (allocation < 0 || value < 0)
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = allocation,
                                   .b = -1,
                                   .c = value,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  int loaded = emit_load_i64(f, allocation);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  if (loaded < 0 || next_iv < 0)
    return false;
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[result_phi].phi_incoming[1].value = loaded;
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 12);
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-counted-cfg-alloca");
}

static int emit_scalar_store(nyr_func_t *f, int address, int value);

static bool emit_tv_store_local(nyr_func_t *f, int slot, int value) {
  size_t old_len = f->len;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                   .dst = -1,
                                   .a = value,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = slot});
  return f->len != old_len;
}

static bool build_cfg_repeated_alloca_alias_case(nyr_func_t *f,
                                                 bool duplicate_first_pointer) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int two = nyr_opt_const(f, 2);
  int seventeen = nyr_opt_const(f, 17);
  if (zero < 0 || one < 0 || two < 0 || seventeen < 0)
    return false;
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  int condition = nyr_opt_cmp(f, iv, two, NYIR_CMP_LT);
  if (condition < 0)
    return false;
  nyr_opt_br_if(f, condition, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 11);
  int allocation =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 8,
                                 .scalar_class = NYIR_SCALAR_POINTER});
  int value = nyr_opt_binop(f, NYIR_ADD_I64, iv, seventeen);
  if (allocation < 0 || value < 0 ||
      emit_scalar_store(f, allocation, value) < 0)
    return false;
  int first_iteration = nyr_opt_cmp(f, iv, zero, NYIR_CMP_EQ);
  if (first_iteration < 0)
    return false;
  nyr_opt_br_if(f, first_iteration, 13);
  if (!emit_tv_store_local(f, 1, allocation))
    return false;
  nyr_opt_br(f, 14);
  nyr_opt_label(f, 13);
  if (!emit_tv_store_local(f, 0, allocation))
    return false;
  nyr_opt_label(f, 14);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  if (next_iv < 0)
    return false;
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 12);
  int later_allocation =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 8,
                                 .scalar_class = NYIR_SCALAR_POINTER});
  int later_value = nyr_opt_const(f, 30);
  if (later_allocation < 0 || later_value < 0 ||
      emit_scalar_store(f, later_allocation, later_value) < 0)
    return false;
  int first = load_local(f, 0);
  int second_slot = load_local(f, 1);
  int second = duplicate_first_pointer ? first : second_slot;
  if (first < 0 || second_slot < 0)
    return false;
  first = emit_load_i64(f, first);
  second = emit_load_i64(f, second);
  if (first < 0 || second < 0)
    return false;
  int sum = nyr_opt_binop(f, NYIR_ADD_I64, first, second);
  if (sum < 0)
    return false;
  nyr_opt_ret(f, sum);
  return nyr_opt_verify(f, "tv-smt-repeated-alloca-alias");
}

static bool checks_repeated_alloca_site_reuses_storage(void) {
  nyr_func_t both_slots = {0}, same_slot_twice = {0};
  bool built = build_cfg_repeated_alloca_alias_case(&both_slots, false) &&
               build_cfg_repeated_alloca_alias_case(&same_slot_twice, true);
  long long indirect_result =
      built ? nyr_opt_eval(&both_slots, "tv-smt-repeated-alloca-site") : 0;
  long long scalar_result =
      built ? nyr_opt_eval(&same_slot_twice, "tv-smt-repeated-alloca-same-slot")
            : 0;
  bool concrete_equivalent = built && indirect_result == scalar_result;
  char err[256] = {0};
  bool same_site_aliases =
      concrete_equivalent &&
      nyr_tv_smt_equiv(&both_slots, &same_slot_twice, err, sizeof(err));
  if (!built || !same_site_aliases || indirect_result != 36)
    fprintf(stderr,
            "[tv-smt-repeated-alloca-reuse] built=%s concrete=%lld/%lld "
            "equivalent=%s: %s\n",
            built ? "yes" : "no", indirect_result, scalar_result,
            same_site_aliases ? "yes" : "no",
            err[0] ? err : "repeated site did not reuse its frame slot");
  if (built && !same_site_aliases) {
    nyr_dump(stderr, &both_slots, "tv-smt-repeated-alloca-both-slots");
    nyr_dump(stderr, &same_slot_twice, "tv-smt-repeated-alloca-same-slot");
  }
  nyr_ir_func_free(&both_slots);
  nyr_ir_func_free(&same_slot_twice);
  return built && same_site_aliases && indirect_result == 36;
}

static bool checks_bounded_cfg_allocations(void) {
  nyr_func_t zero_a = {0}, zero_b = {0}, one_a = {0}, one_b = {0},
              one_changed = {0}, repeated_a = {0}, repeated_b = {0},
              repeated_changed = {0}, three_a = {0}, three_b = {0},
              three_changed = {0}, max_a = {0}, max_b = {0}, over_limit_a = {0},
              over_limit_b = {0};
  bool built = build_counted_cfg_alloca_case(&zero_a, 0, 17) &&
               build_counted_cfg_alloca_case(&zero_b, 0, 18) &&
               build_counted_cfg_alloca_case(&one_a, 1, 17) &&
               build_counted_cfg_alloca_case(&one_b, 1, 17) &&
               build_counted_cfg_alloca_case(&one_changed, 1, 18) &&
               build_counted_cfg_alloca_case(&repeated_a, 2, 17) &&
               build_counted_cfg_alloca_case(&repeated_b, 2, 17) &&
               build_counted_cfg_alloca_case(&repeated_changed, 2, 18) &&
               build_counted_cfg_alloca_case(&three_a, 3, 17) &&
               build_counted_cfg_alloca_case(&three_b, 3, 17) &&
               build_counted_cfg_alloca_case(&three_changed, 3, 18) &&
               build_counted_cfg_alloca_case(&max_a, 8, 17) &&
               build_counted_cfg_alloca_case(&max_b, 8, 17) &&
               build_counted_cfg_alloca_case(&over_limit_a, 9, 17) &&
               build_counted_cfg_alloca_case(&over_limit_b, 9, 17);
  nyr_scev_info_t zero_scev = {0}, one_scev = {0}, repeated_scev = {0},
                   max_scev = {0};
  bool trip_counts = built && nyr_scev_analyze(&zero_a, &zero_scev) &&
                     nyr_scev_analyze(&one_a, &one_scev) &&
                     nyr_scev_analyze(&repeated_a, &repeated_scev) &&
                     nyr_scev_analyze(&max_a, &max_scev);
  bool found_zero = false, found_one = false, found_repeated = false,
       found_max = false;
  for (size_t i = 0; trip_counts && i < zero_scev.count; ++i)
    found_zero |= zero_scev.loops[i].trip_count_known &&
                  zero_scev.loops[i].trip_count == 0;
  for (size_t i = 0; trip_counts && i < one_scev.count; ++i)
    found_one |=
        one_scev.loops[i].trip_count_known && one_scev.loops[i].trip_count == 1;
  for (size_t i = 0; trip_counts && i < repeated_scev.count; ++i)
    found_repeated |= repeated_scev.loops[i].trip_count_known &&
                      repeated_scev.loops[i].trip_count == 2;
  for (size_t i = 0; trip_counts && i < max_scev.count; ++i)
    found_max |=
        max_scev.loops[i].trip_count_known && max_scev.loops[i].trip_count == 8;
  nyr_scev_free(&zero_scev);
  nyr_scev_free(&one_scev);
  nyr_scev_free(&repeated_scev);
  nyr_scev_free(&max_scev);
  char zero_err[256] = {0}, one_err[256] = {0}, changed_err[256] = {0};
  char repeated_err[256] = {0}, repeated_changed_err[256] = {0};
  bool zero_skips_alloca =
      built && nyr_tv_smt_equiv(&zero_a, &zero_b, zero_err, sizeof(zero_err));
  bool single_execution =
      built && nyr_tv_smt_equiv(&one_a, &one_b, one_err, sizeof(one_err));
  bool detects_changed_store =
      built && !nyr_tv_smt_equiv(&one_a, &one_changed, changed_err,
                                  sizeof(changed_err));
  bool repeated_allocations =
      built && nyr_tv_smt_equiv(&repeated_a, &repeated_b, repeated_err,
                                 sizeof(repeated_err));
  bool detects_repeated_changed_store =
      built &&
      !nyr_tv_smt_equiv(&repeated_a, &repeated_changed, repeated_changed_err,
                         sizeof(repeated_changed_err));
  bool repeated_site_reuses_storage =
      checks_repeated_alloca_site_reuses_storage();
  char three_err[256] = {0}, three_changed_err[256] = {0};
  char max_err[256] = {0}, over_limit_err[256] = {0};
  bool three_trip_proved =
      built &&
      nyr_tv_smt_equiv(&three_a, &three_b, three_err, sizeof(three_err));
  bool detects_three_changed_store =
      built && !nyr_tv_smt_equiv(&three_a, &three_changed, three_changed_err,
                                  sizeof(three_changed_err));
  bool max_trip_proved =
      built && nyr_tv_smt_equiv(&max_a, &max_b, max_err, sizeof(max_err));
  bool over_limit_inapplicable =
      built &&
      !nyr_tv_smt_equiv(&over_limit_a, &over_limit_b, over_limit_err,
                         sizeof(over_limit_err)) &&
      over_limit_err[0] == '\0';
  if (!built || !trip_counts || !found_zero || !found_one || !found_repeated ||
      !found_max || !zero_skips_alloca || !single_execution ||
      !detects_changed_store || !repeated_allocations ||
      !detects_repeated_changed_store || !repeated_site_reuses_storage ||
      !three_trip_proved || !detects_three_changed_store || !max_trip_proved ||
      !over_limit_inapplicable)
    fprintf(
        stderr,
        "[tv-smt-counted-cfg-alloca] built=%s zero-trip=%s one-trip=%s "
        "scev-trips=%s changed-store=%s repeated=%s "
        "repeated-changed=%s same-site-reuse=%s three-trip=%s "
        "three-changed=%s max-trip=%s over-limit=%s: %s\n",
        built ? "yes" : "no", zero_skips_alloca ? "proved" : "failed",
        single_execution ? "proved" : "failed",
        (trip_counts && found_zero && found_one && found_repeated && found_max)
            ? "yes"
            : "no",
        detects_changed_store ? "detected" : "missed",
        repeated_allocations ? "proved" : "failed",
        detects_repeated_changed_store ? "detected" : "missed",
        repeated_site_reuses_storage ? "proved" : "failed",
        three_trip_proved ? "proved" : "failed",
        detects_three_changed_store ? "detected" : "missed",
        max_trip_proved ? "proved" : "failed",
        over_limit_inapplicable ? "inapplicable" : "admitted",
        zero_err[0]               ? zero_err
        : one_err[0]              ? one_err
        : changed_err[0]          ? changed_err
        : repeated_err[0]         ? repeated_err
        : repeated_changed_err[0] ? repeated_changed_err
        : three_err[0]            ? three_err
        : three_changed_err[0]    ? three_changed_err
        : max_err[0]              ? max_err
        : over_limit_err[0]       ? over_limit_err
                                  : "allocation loop validity mismatch");
  nyr_ir_func_free(&zero_a);
  nyr_ir_func_free(&zero_b);
  nyr_ir_func_free(&one_a);
  nyr_ir_func_free(&one_b);
  nyr_ir_func_free(&one_changed);
  nyr_ir_func_free(&repeated_a);
  nyr_ir_func_free(&repeated_b);
  nyr_ir_func_free(&repeated_changed);
  nyr_ir_func_free(&three_a);
  nyr_ir_func_free(&three_b);
  nyr_ir_func_free(&three_changed);
  nyr_ir_func_free(&max_a);
  nyr_ir_func_free(&max_b);
  nyr_ir_func_free(&over_limit_a);
  nyr_ir_func_free(&over_limit_b);
  return built && trip_counts && found_zero && found_one && found_repeated &&
         found_max && zero_skips_alloca && single_execution &&
         detects_changed_store && repeated_allocations &&
         detects_repeated_changed_store && repeated_site_reuses_storage &&
         three_trip_proved && detects_three_changed_store && max_trip_proved &&
         over_limit_inapplicable;
}

static bool rejects_repeated_cfg_allocations(void) {
  nyr_func_t before = {0}, after = {0};
  bool built =
      build_cfg_loop_alloca_case(&before) && build_cfg_loop_alloca_case(&after);
  char err[256] = {0};
  bool inapplicable = built &&
                      !nyr_tv_smt_equiv(&before, &after, err, sizeof(err)) &&
                      err[0] == '\0';
  if (!built || !inapplicable)
    fprintf(stderr, "[tv-smt-cfg-loop-alloca] built=%s inapplicable=%s: %s\n",
            built ? "yes" : "no", inapplicable ? "yes" : "no", err);
  nyr_ir_func_free(&before);
  nyr_ir_func_free(&after);
  return built && inapplicable;
}

static bool build_two_alloca_case(nyr_func_t *f, int64_t second_value) {
  nyr_opt_begin(f);
  int first = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 8,
                                         .scalar_class = NYIR_SCALAR_POINTER});
  int first_value = nyr_opt_const(f, 11);
  int second = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = 8,
                                          .scalar_class = NYIR_SCALAR_POINTER});
  int replacement = nyr_opt_const(f, second_value);
  if (first < 0 || first_value < 0 || second < 0 || replacement < 0)
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = first,
                                   .b = -1,
                                   .c = first_value,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = second,
                                   .b = -1,
                                   .c = replacement,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  int result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = first,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-two-alloca");
}

static bool checks_alloca_regions_are_disjoint(void) {
  nyr_func_t before = {0}, after = {0};
  bool built =
      build_two_alloca_case(&before, 22) && build_two_alloca_case(&after, 33);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&before, &after, err, sizeof(err));
  if (!built || !equivalent)
    fprintf(stderr, "[tv-smt-alloca-regions] built=%s equivalent=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no", err);
  nyr_ir_func_free(&before);
  nyr_ir_func_free(&after);
  return built && equivalent;
}

static bool build_uninitialized_local_pointer_load(nyr_func_t *f,
                                                   bool keep_memory_load) {
  nyr_opt_begin(f);
  int pointer = load_local(f, 0);
  if (pointer < 0)
    return false;
  int discarded = nyr_ir_emit(
      f, (nyr_inst_t){.op = keep_memory_load ? NYIR_LOAD_I64 : NYIR_COPY,
                       .dst = -1,
                       .a = pointer,
                       .b = -1,
                       .c = -1,
                       .d = -1,
                       .e = -1,
                       .f = -1});
  int allocation =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 8,
                                 .scalar_class = NYIR_SCALAR_POINTER});
  if (discarded < 0 || allocation < 0)
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                   .dst = -1,
                                   .a = allocation,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = 0});
  int result = nyr_opt_const(f, 0);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-alloca-local-order");
}

static bool checks_local_pointer_provenance_order(void) {
  nyr_func_t before = {0}, after = {0};
  bool built = build_uninitialized_local_pointer_load(&before, true) &&
               build_uninitialized_local_pointer_load(&after, false);
  char err[256] = {0};
  bool rejects_erased_load =
      built && !nyr_tv_smt_equiv(&before, &after, err, sizeof(err));
  if (!built || !rejects_erased_load)
    fprintf(stderr, "[tv-smt-alloca-local-order] built=%s rejected=%s: %s\n",
            built ? "yes" : "no", rejects_erased_load ? "yes" : "no", err);
  nyr_ir_func_free(&before);
  nyr_ir_func_free(&after);
  return built && rejects_erased_load;
}

static bool build_shift(nyr_func_t *f, nyr_op_t op, int64_t count_mask,
                        bool explicit_mask) {
  nyr_opt_begin(f);
  int value = load_local(f, 0);
  int count = load_local(f, 1);
  if (value < 0 || count < 0)
    return false;
  if (explicit_mask) {
    int mask = nyr_opt_const(f, count_mask);
    count = nyr_opt_binop(f, NYIR_AND_I64, count, mask);
    if (count < 0)
      return false;
  }
  int shifted = nyr_opt_binop(f, op, value, count);
  if (shifted < 0)
    return false;
  nyr_opt_ret(f, shifted);
  return nyr_opt_verify(f, "tv-smt-shift");
}

static bool equivalent_shift(nyr_op_t op, int64_t count_mask,
                             const char *name) {
  nyr_func_t hardware = {0}, masked = {0};
  bool built = build_shift(&hardware, op, count_mask, false) &&
               build_shift(&masked, op, count_mask, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&hardware, &masked, err, sizeof(err));
  if (!equivalent)
    fprintf(stderr, "[tv-smt-semantics] %s: %s\n", name,
            err[0] ? err : "construction failed");
  nyr_ir_func_free(&hardware);
  nyr_ir_func_free(&masked);
  return equivalent;
}

static bool build_select_i64(nyr_func_t *f, bool branch) {
  nyr_opt_begin(f);
  int condition = load_local(f, 0);
  int false_value = load_local(f, 1);
  int true_value = load_local(f, 2);
  int zero = nyr_opt_const(f, 0);
  int predicate = condition < 0 || zero < 0
                      ? -1
                      : nyr_opt_cmp(f, condition, zero, NYIR_CMP_NE);
  if (condition < 0 || false_value < 0 || true_value < 0 || predicate < 0)
    return false;
  if (branch) {
    nyr_opt_br_if(f, predicate, 1);
    nyr_opt_ret(f, false_value);
    nyr_opt_label(f, 1);
    nyr_opt_ret(f, true_value);
  } else {
    int selected = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_SELECT_I64,
                                              .dst = -1,
                                              .a = predicate,
                                              .b = true_value,
                                              .c = false_value,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1});
    if (selected < 0)
      return false;
    nyr_opt_ret(f, selected);
  }
  return nyr_opt_verify(f, "tv-smt-select-i64");
}

static bool checks_select_i64_smt(void) {
  nyr_func_t selected = {0}, branched = {0};
  bool built =
      build_select_i64(&selected, false) && build_select_i64(&branched, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&selected, &branched, err, sizeof(err));
  if (!built || !equivalent)
    fprintf(stderr, "[tv-smt-select-i64] built=%s equivalent=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            err[0] ? err : "select differs from conditional control flow");
  nyr_ir_func_free(&selected);
  nyr_ir_func_free(&branched);
  return built && equivalent;
}

static bool build_i64_cmp_case(nyr_func_t *f, int64_t lhs, int64_t rhs,
                               nyr_cmp_t cmp) {
  nyr_opt_begin(f);
  int left = nyr_opt_const(f, lhs);
  int right = nyr_opt_const(f, rhs);
  int actual = left < 0 || right < 0 ? -1 : nyr_opt_cmp(f, left, right, cmp);
  if (actual < 0)
    return false;
  nyr_opt_ret(f, actual);
  return nyr_opt_verify(f, "tv-smt-cmp-i64");
}

static bool build_i64_cmp_reference(nyr_func_t *f, bool expected) {
  nyr_opt_begin(f);
  int result = nyr_opt_const(f, expected ? 1 : 0);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-cmp-i64-reference");
}

static bool checks_i64_cmp_smt(void) {
  static const nyr_cmp_t predicates[] = {
      NYIR_CMP_EQ, NYIR_CMP_NE, NYIR_CMP_LT,
      NYIR_CMP_LE, NYIR_CMP_GT, NYIR_CMP_GE,
  };
  static const int64_t operands[][2] = {
      {INT64_MIN, INT64_MIN},
      {INT64_MAX, INT64_MAX},
      {INT64_MIN, -1},
      {-1, 0},
      {0, -1},
      {1, 0},
      {0, 1},
      {INT64_MAX, INT64_MIN},
      {INT64_MIN, INT64_MAX},
      {42, 42},
  };
  bool ok = true;
  for (size_t p = 0; p < sizeof(predicates) / sizeof(predicates[0]) && ok;
       ++p) {
    for (size_t i = 0; i < sizeof(operands) / sizeof(operands[0]) && ok; ++i) {
      int64_t lhs = operands[i][0];
      int64_t rhs = operands[i][1];
      bool expected = false;
      switch (predicates[p]) {
      case NYIR_CMP_EQ:
        expected = lhs == rhs;
        break;
      case NYIR_CMP_NE:
        expected = lhs != rhs;
        break;
      case NYIR_CMP_LT:
        expected = lhs < rhs;
        break;
      case NYIR_CMP_LE:
        expected = lhs <= rhs;
        break;
      case NYIR_CMP_GT:
        expected = lhs > rhs;
        break;
      case NYIR_CMP_GE:
        expected = lhs >= rhs;
        break;
      }
      nyr_func_t actual = {0}, reference = {0};
      bool built = build_i64_cmp_case(&actual, lhs, rhs, predicates[p]) &&
                   build_i64_cmp_reference(&reference, expected);
      char err[256] = {0};
      bool equivalent =
          built && nyr_tv_smt_equiv(&actual, &reference, err, sizeof(err));
      if (!equivalent)
        fprintf(stderr,
                "[tv-smt-cmp-i64] predicate=%d lhs=%" PRId64 " rhs=%" PRId64
                " expected=%d: %s\n",
                (int)predicates[p], lhs, rhs, expected,
                err[0] ? err : "comparison differs from signed reference");
      nyr_ir_func_free(&actual);
      nyr_ir_func_free(&reference);
      ok = built && equivalent;
    }
  }
  return ok;
}

static bool build_i64_arithmetic_case(nyr_func_t *f, nyr_op_t op, int64_t lhs,
                                      int64_t rhs) {
  nyr_opt_begin(f);
  int left = nyr_opt_const(f, lhs);
  int right = nyr_opt_const(f, rhs);
  int result = left < 0 || right < 0 ? -1 : nyr_opt_binop(f, op, left, right);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-i64-arithmetic");
}

static bool build_i64_arithmetic_reference(nyr_func_t *f, int64_t value) {
  nyr_opt_begin(f);
  int result = nyr_opt_const(f, value);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-i64-arithmetic-reference");
}

static int64_t expected_i64_arithmetic(nyr_op_t op, int64_t lhs, int64_t rhs) {
  if (op == NYIR_ADD_I64 || op == NYIR_SUB_I64 || op == NYIR_MUL_I64) {
    uint64_t a = (uint64_t)lhs;
    uint64_t b = (uint64_t)rhs;
    uint64_t bits = op == NYIR_ADD_I64   ? a + b
                    : op == NYIR_SUB_I64 ? a - b
                                         : a * b;
    int64_t result;
    memcpy(&result, &bits, sizeof(result));
    return result;
  }
  return op == NYIR_DIV_I64 ? lhs / rhs : lhs % rhs;
}

static bool checks_i64_arithmetic_smt(void) {
  static const nyr_op_t operations[] = {
      NYIR_ADD_I64, NYIR_SUB_I64, NYIR_MUL_I64, NYIR_DIV_I64, NYIR_MOD_I64};
  static const int64_t operands[][2] = {
      {INT64_MAX, 1},  {INT64_MIN, -1}, {INT64_MIN, 1},
      {INT64_MAX, -1}, {-17, 6},        {-7, 3},
      {INT64_MIN, 3},  {INT64_MAX, 2},  {-1, INT64_MIN},
  };
  bool ok = true;
  for (size_t o = 0; o < sizeof(operations) / sizeof(operations[0]) && ok;
       ++o) {
    for (size_t i = 0; i < sizeof(operands) / sizeof(operands[0]) && ok; ++i) {
      nyr_op_t op = operations[o];
      int64_t lhs = operands[i][0], rhs = operands[i][1];
      if ((op == NYIR_DIV_I64 || op == NYIR_MOD_I64) &&
          (rhs == 0 || (lhs == INT64_MIN && rhs == -1)))
        continue;
      int64_t expected = expected_i64_arithmetic(op, lhs, rhs);
      nyr_func_t actual = {0}, reference = {0};
      bool built = build_i64_arithmetic_case(&actual, op, lhs, rhs) &&
                   build_i64_arithmetic_reference(&reference, expected);
      char err[256] = {0};
      bool equivalent =
          built && nyr_tv_smt_equiv(&actual, &reference, err, sizeof(err));
      if (!equivalent)
        fprintf(stderr,
                "[tv-smt-i64-arithmetic] op=%s lhs=%" PRId64 " rhs=%" PRId64
                " expected=%" PRId64 ": %s\n",
                nyr_op_name(op), lhs, rhs, expected,
                err[0] ? err
                       : "arithmetic differs from signed/wrapping result");
      nyr_ir_func_free(&actual);
      nyr_ir_func_free(&reference);
      ok = built && equivalent;
    }
  }
  return ok;
}

static bool build_i64_xor_case(nyr_func_t *f, bool expanded,
                               bool omit_intersection) {
  nyr_opt_begin(f);
  int lhs = load_local(f, 0);
  int rhs = load_local(f, 1);
  int result = -1;
  if (lhs >= 0 && rhs >= 0 && !expanded) {
    result = nyr_opt_binop(f, NYIR_XOR_I64, lhs, rhs);
  } else if (lhs >= 0 && rhs >= 0) {
    int either = nyr_opt_binop(f, NYIR_OR_I64, lhs, rhs);
    if (either >= 0) {
      if (omit_intersection) {
        result = either;
      } else {
        int both = nyr_opt_binop(f, NYIR_AND_I64, lhs, rhs);
        result = both < 0 ? -1 : nyr_opt_binop(f, NYIR_SUB_I64, either, both);
      }
    }
  }
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-i64-xor");
}

static bool checks_i64_xor_smt(void) {
  nyr_func_t direct = {0}, expanded = {0}, wrong = {0};
  bool built = build_i64_xor_case(&direct, false, false) &&
               build_i64_xor_case(&expanded, true, false) &&
               build_i64_xor_case(&wrong, true, true);
  char equivalent_err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&direct, &expanded, equivalent_err,
                                 sizeof(equivalent_err));
  char rejection_err[256] = {0};
  bool rejects_wrong_result =
      built &&
      !nyr_tv_smt_equiv(&direct, &wrong, rejection_err, sizeof(rejection_err));
  if (!built || !equivalent || !rejects_wrong_result)
    fprintf(stderr,
            "[tv-smt-i64-xor] built=%s identity=%s wrong-result=%s: %s\n",
            built ? "yes" : "no", equivalent ? "proved" : "failed",
            rejects_wrong_result ? "rejected" : "missed",
            equivalent_err[0]  ? equivalent_err
            : rejection_err[0] ? rejection_err
                               : "XOR disagrees with OR minus AND");
  nyr_ir_func_free(&direct);
  nyr_ir_func_free(&expanded);
  nyr_ir_func_free(&wrong);
  return built && equivalent && rejects_wrong_result;
}

static bool build_byte_swap(nyr_func_t *f, unsigned bytes, bool direct,
                            bool cfg) {
  if (bytes != 4 && bytes != 8)
    return false;
  nyr_opt_begin(f);
  int input = load_local(f, 0);
  if (input < 0)
    return false;
  if (cfg) {
    int condition = load_local(f, 1);
    int zero = nyr_opt_const(f, 0);
    if (condition < 0 || zero < 0)
      return false;
    nyr_opt_br_if(f, condition, 1);
    nyr_opt_ret(f, zero);
    nyr_opt_label(f, 1);
  }
  int result = -1;
  if (direct) {
    result = nyr_ir_emit(
        f, (nyr_inst_t){.op = bytes == 4 ? NYIR_BSWAP32_I64 : NYIR_BSWAP_I64,
                         .dst = -1,
                         .a = input,
                         .b = -1,
                         .c = -1,
                         .d = -1,
                         .e = -1,
                         .f = -1});
  } else {
    int byte_mask = nyr_opt_const(f, 0xff);
    int input_mask = nyr_opt_const(f, bytes == 4 ? INT64_C(0xffffffff) : -1);
    int masked_input =
        input_mask < 0 ? -1 : nyr_opt_binop(f, NYIR_AND_I64, input, input_mask);
    result = nyr_opt_const(f, 0);
    for (unsigned byte = 0; byte < bytes && result >= 0; ++byte) {
      int shift_right = nyr_opt_const(f, (int64_t)byte * 8);
      int shift_left = nyr_opt_const(f, (int64_t)(bytes - byte - 1) * 8);
      int shifted =
          masked_input < 0 || shift_right < 0
              ? -1
              : nyr_opt_binop(f, NYIR_SAR_I64, masked_input, shift_right);
      int value = shifted < 0 || byte_mask < 0
                      ? -1
                      : nyr_opt_binop(f, NYIR_AND_I64, shifted, byte_mask);
      int placed = value < 0 || shift_left < 0
                       ? -1
                       : nyr_opt_binop(f, NYIR_SHL_I64, value, shift_left);
      result = placed < 0 ? -1 : nyr_opt_binop(f, NYIR_OR_I64, result, placed);
    }
  }
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-byte-swap");
}

static bool checks_byte_swap_smt(void) {
  for (unsigned bytes = 4; bytes <= 8; bytes += 4) {
    for (unsigned cfg = 0; cfg < 2; ++cfg) {
      nyr_func_t direct = {0}, expanded = {0};
      bool built = build_byte_swap(&direct, bytes, true, cfg) &&
                   build_byte_swap(&expanded, bytes, false, cfg);
      char err[256] = {0};
      bool equivalent =
          built && nyr_tv_smt_equiv(&direct, &expanded, err, sizeof(err));
      if (!built || !equivalent)
        fprintf(stderr,
                "[tv-smt-byte-swap] bytes=%u cfg=%s built=%s equivalent=%s: "
                "%s\n",
                bytes, cfg ? "yes" : "no", built ? "yes" : "no",
                equivalent ? "yes" : "no",
                err[0] ? err : "byte swap differs from bytewise construction");
      nyr_ir_func_free(&direct);
      nyr_ir_func_free(&expanded);
      if (!built || !equivalent)
        return false;
    }
  }
  return true;
}

static bool build_overflowing_division(nyr_func_t *f, nyr_op_t op) {
  nyr_opt_begin(f);
  int dividend = load_local(f, 0);
  int minus_one = nyr_opt_const(f, -1);
  if (dividend < 0 || minus_one < 0)
    return false;
  int result = nyr_opt_binop(f, op, dividend, minus_one);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-overflowing-division");
}

static bool build_nontrapping_rewrite(nyr_func_t *f, nyr_op_t op) {
  nyr_opt_begin(f);
  int result = -1;
  if (op == NYIR_DIV_I64) {
    int dividend = load_local(f, 0);
    int zero = nyr_opt_const(f, 0);
    if (dividend < 0 || zero < 0)
      return false;
    result = nyr_opt_binop(f, NYIR_SUB_I64, zero, dividend);
  } else {
    result = nyr_opt_const(f, 0);
  }
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-nontrapping-rewrite");
}

static bool build_zero_divisor_case(nyr_func_t *f, nyr_op_t op,
                                    bool trapping) {
  nyr_opt_begin(f);
  int lhs = trapping ? nyr_opt_const(f, 0) : -1;
  int rhs = trapping ? load_local(f, 0) : -1;
  int result;
  if (trapping) {
    if (lhs < 0 || rhs < 0)
      return false;
    result = nyr_opt_binop(f, op, lhs, rhs);
  } else {
    result = nyr_opt_const(f, 0);
  }
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-zero-divisor");
}

static bool rejects_erased_zero_trap(nyr_op_t op, const char *name) {
  nyr_func_t trapping = {0}, rewritten = {0};
  bool built = build_zero_divisor_case(&trapping, op, true) &&
               build_zero_divisor_case(&rewritten, op, false);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&trapping, &rewritten, err, sizeof(err));
  char fallback_err[256] = {0};
  bool fallback_equivalent =
      built && nyr_tv_equiv_straightline(&trapping, &rewritten, 32,
                                          fallback_err, sizeof(fallback_err));
  if (equivalent || fallback_equivalent || !built) {
    const char *reason =
        built ? "zero-divisor trap was erased" : "construction failed";
    if (fallback_err[0])
      reason = fallback_err;
    if (err[0])
      reason = err;
    fprintf(stderr, "[tv-smt-semantics] %s: %s\n", name, reason);
  }
  nyr_ir_func_free(&trapping);
  nyr_ir_func_free(&rewritten);
  return built && !equivalent && !fallback_equivalent;
}

static bool rejects_changed_trap(nyr_op_t op, const char *name) {
  nyr_func_t trapping = {0}, rewritten = {0};
  bool built = build_overflowing_division(&trapping, op) &&
               build_nontrapping_rewrite(&rewritten, op);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&trapping, &rewritten, err, sizeof(err));
  char fallback_err[256] = {0};
  bool fallback_equivalent =
      built && nyr_tv_equiv_straightline(&trapping, &rewritten, 32,
                                          fallback_err, sizeof(fallback_err));
  if (equivalent || fallback_equivalent || !built) {
    const char *reason =
        built ? "trap treated as a value" : "construction failed";
    if (fallback_err[0])
      reason = fallback_err;
    if (err[0])
      reason = err;
    fprintf(stderr, "[tv-smt-semantics] %s: %s\n", name, reason);
  }
  nyr_ir_func_free(&trapping);
  nyr_ir_func_free(&rewritten);
  return built && !equivalent && !fallback_equivalent;
}

static bool build_bounds_case(nyr_func_t *f, bool checked,
                              bool dynamic_length) {
  nyr_opt_begin(f);
  int base = load_local(f, 0);
  int offset = load_local(f, 1);
  int byte_len = dynamic_length ? load_local(f, 2) : -1;
  if (base < 0 || offset < 0 || (dynamic_length && byte_len < 0))
    return false;
  if (checked &&
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                                 .dst = -1,
                                 .a = base,
                                 .b = offset,
                                 .c = byte_len,
                                 .imm = dynamic_length ? 0 : 16}) < 0)
    return false;
  nyr_opt_ret(f, offset);
  return nyr_opt_verify(f, "tv-smt-bounds-check");
}

static bool checks_bounds_check_smt_semantics(void) {
  nyr_func_t checked_static = {0}, checked_static_copy = {0};
  nyr_func_t unchecked_static = {0}, checked_dynamic = {0};
  nyr_func_t checked_dynamic_copy = {0}, unchecked_dynamic = {0};
  bool built = build_bounds_case(&checked_static, true, false) &&
               build_bounds_case(&checked_static_copy, true, false) &&
               build_bounds_case(&unchecked_static, false, false) &&
               build_bounds_case(&checked_dynamic, true, true) &&
               build_bounds_case(&checked_dynamic_copy, true, true) &&
               build_bounds_case(&unchecked_dynamic, false, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&checked_static, &checked_static_copy, err,
                                 sizeof(err));
  bool dynamic_equivalent =
      built && nyr_tv_smt_equiv(&checked_dynamic, &checked_dynamic_copy, err,
                                 sizeof(err));
  err[0] = '\0';
  bool rejects_erasure =
      built &&
      !nyr_tv_smt_equiv(&checked_static, &unchecked_static, err, sizeof(err));
  bool solver_rejected = rejects_erasure && strstr(err, "SMT TV") != NULL;
  err[0] = '\0';
  bool rejects_dynamic_erasure =
      built && !nyr_tv_smt_equiv(&checked_dynamic, &unchecked_dynamic, err,
                                  sizeof(err));
  bool dynamic_solver_rejected =
      rejects_dynamic_erasure && strstr(err, "SMT TV") != NULL;
  if (!built || !equivalent || !dynamic_equivalent || !solver_rejected ||
      !dynamic_solver_rejected) {
    fprintf(stderr,
            "[tv-smt-bounds-check] built=%s static-eq=%s dynamic-eq=%s "
            "static-trap=%s dynamic-trap=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            dynamic_equivalent ? "yes" : "no", solver_rejected ? "yes" : "no",
            dynamic_solver_rejected ? "yes" : "no",
            err[0] ? err : "bounds trap semantics were not preserved");
    if (built) {
      nyr_dump(stderr, &checked_static, "checked static bounds test IR");
      nyr_dump(stderr, &unchecked_static, "unchecked static bounds test IR");
      fprintf(stderr, "[tv-smt-bounds-check] pure=%d fallback=%d\n",
              nyr_is_pure_i64_straightline(&checked_static),
              nyr_tv_equiv_straightline(&checked_static, &unchecked_static, 32,
                                         err, sizeof(err)));
    }
  }
  nyr_ir_func_free(&checked_static);
  nyr_ir_func_free(&checked_static_copy);
  nyr_ir_func_free(&unchecked_static);
  nyr_ir_func_free(&checked_dynamic);
  nyr_ir_func_free(&checked_dynamic_copy);
  nyr_ir_func_free(&unchecked_dynamic);
  return built && equivalent && dynamic_equivalent && solver_rejected &&
         dynamic_solver_rejected;
}

static int emit_f32_constant(nyr_func_t *f, uint32_t bits);

static bool build_fp_cmp_constant_result(nyr_func_t *f, bool single,
                                         double lhs, double rhs, nyr_cmp_t cmp,
                                         int64_t expected) {
  nyr_opt_begin(f);
  int a = -1, b = -1;
  if (single) {
    float lhs_f32 = (float)lhs, rhs_f32 = (float)rhs;
    uint32_t lhs_bits = 0, rhs_bits = 0;
    memcpy(&lhs_bits, &lhs_f32, sizeof(lhs_bits));
    memcpy(&rhs_bits, &rhs_f32, sizeof(rhs_bits));
    a = emit_f32_constant(f, lhs_bits);
    b = emit_f32_constant(f, rhs_bits);
  } else {
    a = nyr_opt_const_f64(f, lhs);
    b = nyr_opt_const_f64(f, rhs);
  }
  nyr_op_t op = single ? NYIR_CMP_F32 : NYIR_CMP_F64;
  int result = a < 0 || b < 0 ? -1
                              : nyr_ir_emit(f, (nyr_inst_t){.op = op,
                                                           .dst = -1,
                                                           .a = a,
                                                           .b = b,
                                                           .c = -1,
                                                           .d = -1,
                                                           .e = -1,
                                                           .f = -1,
                                                           .cmp = cmp});
  if (result < 0 || (expected != 0 && expected != 1))
    return false;
  nyr_opt_br_if(f, result, 1);
  int false_value = nyr_opt_const(f, 0);
  if (false_value < 0)
    return false;
  nyr_opt_ret(f, false_value);
  nyr_opt_label(f, 1);
  int true_value = nyr_opt_const(f, 1);
  if (true_value < 0)
    return false;
  nyr_opt_ret(f, true_value);
  return nyr_opt_verify(f, "tv-smt-fp-cmp-edge");
}

static bool checks_fp_cmp_edge_semantics(void) {
  static const struct {
    double lhs;
    double rhs;
    nyr_cmp_t cmp;
    int64_t expected;
  } cases[] = {
      {NAN, 1.0, NYIR_CMP_EQ, 0},  {NAN, 1.0, NYIR_CMP_NE, 1},
      {NAN, 1.0, NYIR_CMP_LT, 0},  {NAN, 1.0, NYIR_CMP_LE, 0},
      {NAN, 1.0, NYIR_CMP_GT, 0},  {NAN, 1.0, NYIR_CMP_GE, 0},
      {0.0, -0.0, NYIR_CMP_EQ, 1}, {0.0, -0.0, NYIR_CMP_NE, 0},
      {0.0, -0.0, NYIR_CMP_LT, 0}, {0.0, -0.0, NYIR_CMP_LE, 1},
      {0.0, -0.0, NYIR_CMP_GT, 0}, {0.0, -0.0, NYIR_CMP_GE, 1},
  };
  for (unsigned single = 0; single < 2; ++single) {
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      nyr_func_t actual = {0}, expected = {0};
      bool built = build_fp_cmp_constant_result(
          &actual, single != 0, cases[i].lhs, cases[i].rhs, cases[i].cmp,
          cases[i].expected);
      nyr_opt_begin(&expected);
      int expected_value = nyr_opt_const(&expected, cases[i].expected);
      bool expected_built = expected_value >= 0;
      if (expected_built) {
        nyr_opt_ret(&expected, expected_value);
        expected_built = nyr_opt_verify(&expected, "tv-smt-fp-cmp-reference");
      }
      built = built && expected_built;
      char err[256] = {0}, eval_err[192] = {0};
      nyr_eval_result_t evaluated = {0};
      bool eval_matches = built &&
                          nyr_ir_eval(&actual, NULL, 0, 100, &evaluated, eval_err,
                                    sizeof(eval_err)) &&
                          evaluated.returned &&
                          evaluated.result == cases[i].expected;
      bool smt_matches =
          built && nyr_tv_smt_equiv(&actual, &expected, err, sizeof(err));
      nyr_eval_result_free(&evaluated);
      nyr_ir_func_free(&actual);
      nyr_ir_func_free(&expected);
      if (!built || !eval_matches || !smt_matches) {
        fprintf(stderr,
                "[tv-smt-fp-cmp-edge] precision=%s case=%zu cmp=%d built=%s "
                "eval=%s smt=%s: %s\n",
                single ? "f32" : "f64", i, cases[i].cmp, built ? "yes" : "no",
                eval_matches ? "matched" : "mismatch",
                smt_matches ? "matched" : "mismatch",
                err[0]        ? err
                : eval_err[0] ? eval_err
                              : "floating comparison edge");
        return false;
      }
    }
  }
  return true;
}

static bool build_cfg_float_compare(nyr_func_t *f, nyr_cmp_t cmp) {
  nyr_opt_begin(f);
  int x = load_local(f, 0);
  int one = nyr_opt_const_f64(f, 1.0);
  int condition = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CMP_F64,
                                             .dst = -1,
                                             .a = x,
                                             .b = one,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .cmp = cmp});
  if (x < 0 || one < 0 || condition < 0)
    return false;
  nyr_opt_br_if(f, condition, 1);
  int no = nyr_opt_const(f, 0);
  nyr_opt_ret(f, no);
  nyr_opt_label(f, 1);
  int yes = nyr_opt_const(f, 1);
  nyr_opt_ret(f, yes);
  return nyr_opt_verify(f, "tv-smt-f64-cfg");
}

static bool rejects_cfg_float_compare_change(void) {
  nyr_func_t before = {0}, after = {0};
  bool built = build_cfg_float_compare(&before, NYIR_CMP_LT) &&
               build_cfg_float_compare(&after, NYIR_CMP_LE);
  double one_value = 1.0;
  int64_t one_bits = 0;
  memcpy(&one_bits, &one_value, sizeof(one_bits));
  int64_t input_before[1] = {one_bits};
  nyr_eval_result_t rb = {0}, ra = {0};
  char eval_err[192] = {0};
  bool eval_ok = built && nyr_ir_eval(&before, input_before, 1, 100, &rb,
                                    eval_err, sizeof(eval_err));
  int64_t after_input[1] = {one_bits};
  eval_ok =
      eval_ok &&
      nyr_ir_eval(&after, after_input, 1, 100, &ra, eval_err, sizeof(eval_err)) &&
      rb.returned && ra.returned && rb.result != ra.result;
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&before, &after, err, sizeof(err));
  if (!built || !eval_ok || equivalent) {
    fprintf(stderr,
            "[tv-smt-f64-cfg] failed to reject changed float branch: %s\n",
            err[0]        ? err
            : eval_err[0] ? eval_err
                          : "unexpected equivalence");
  }
  nyr_eval_result_free(&rb);
  nyr_eval_result_free(&ra);
  nyr_ir_func_free(&before);
  nyr_ir_func_free(&after);
  return built && eval_ok && !equivalent;
}

static bool build_cfg_sqrt(nyr_func_t *f, bool expected_true) {
  nyr_opt_begin(f);
  int four = nyr_opt_const_f64(f, 4.0);
  int root = four < 0 ? -1
                      : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_SQRT_F64,
                                                   .dst = -1,
                                                   .a = four,
                                                   .b = -1,
                                                   .c = -1,
                                                   .d = -1,
                                                   .e = -1,
                                                   .f = -1});
  int two = nyr_opt_const_f64(f, 2.0);
  int equal = root < 0 || two < 0
                  ? -1
                  : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CMP_F64,
                                               .dst = -1,
                                               .a = root,
                                               .b = two,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .cmp = NYIR_CMP_EQ});
  if (four < 0 || root < 0 || two < 0 || equal < 0)
    return false;
  nyr_opt_br_if(f, equal, 1);
  int no = nyr_opt_const(f, expected_true ? 0 : 1);
  nyr_opt_ret(f, no);
  nyr_opt_label(f, 1);
  int yes = nyr_opt_const(f, expected_true ? 1 : 0);
  nyr_opt_ret(f, yes);
  return nyr_opt_verify(f, "tv-smt-sqrt-cfg");
}

static bool checks_cfg_sqrt(void) {
  nyr_func_t correct = {0}, equivalent = {0}, wrong = {0};
  bool built = build_cfg_sqrt(&correct, true) &&
               build_cfg_sqrt(&equivalent, true) &&
               build_cfg_sqrt(&wrong, false);
  char err[256] = {0};
  bool same =
      built && nyr_tv_smt_equiv(&correct, &equivalent, err, sizeof(err));
  err[0] = '\0';
  bool rejects_wrong =
      built && !nyr_tv_smt_equiv(&correct, &wrong, err, sizeof(err));
  if (!built || !same || !rejects_wrong)
    fprintf(stderr, "[tv-smt-sqrt-cfg] built=%s same=%s rejects-wrong=%s: %s\n",
            built ? "yes" : "no", same ? "yes" : "no",
            rejects_wrong ? "yes" : "no",
            err[0] ? err : "solver did not establish expected result");
  nyr_ir_func_free(&correct);
  nyr_ir_func_free(&equivalent);
  nyr_ir_func_free(&wrong);
  return built && same && rejects_wrong;
}

static bool build_cfg_trig_predicate(nyr_func_t *f, nyr_op_t trig,
                                     bool copy_input) {
  nyr_opt_begin(f);
  int input = load_local(f, 0);
  int operand = copy_input && input >= 0
                    ? nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_COPY,
                                                 .dst = -1,
                                                 .a = input,
                                                 .b = -1,
                                                 .c = -1,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1})
                    : input;
  int result = operand < 0 ? -1
                           : nyr_ir_emit(f, (nyr_inst_t){.op = trig,
                                                        .dst = -1,
                                                        .a = operand,
                                                        .b = -1,
                                                        .c = -1,
                                                        .d = -1,
                                                        .e = -1,
                                                        .f = -1});
  int zero = nyr_opt_const_f64(f, 0.0);
  int predicate = result < 0 || zero < 0
                      ? -1
                      : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CMP_F64,
                                                   .dst = -1,
                                                   .a = result,
                                                   .b = zero,
                                                   .c = -1,
                                                   .d = -1,
                                                   .e = -1,
                                                   .f = -1,
                                                   .cmp = NYIR_CMP_EQ});
  if (input < 0 || operand < 0 || result < 0 || zero < 0 || predicate < 0)
    return false;
  nyr_opt_br_if(f, predicate, 1);
  int no = nyr_opt_const(f, 0);
  nyr_opt_ret(f, no);
  nyr_opt_label(f, 1);
  int yes = nyr_opt_const(f, 1);
  nyr_opt_ret(f, yes);
  return no >= 0 && yes >= 0 && nyr_opt_verify(f, "tv-smt-trig-cfg");
}

static bool checks_cfg_transcendental_congruence(void) {
  nyr_func_t sine = {0}, copied_sine = {0}, cosine = {0};
  bool built = build_cfg_trig_predicate(&sine, NYIR_SIN_F64, false) &&
               build_cfg_trig_predicate(&copied_sine, NYIR_SIN_F64, true) &&
               build_cfg_trig_predicate(&cosine, NYIR_COS_F64, false);
  char err[256] = {0};
  bool same = built && nyr_tv_smt_equiv(&sine, &copied_sine, err, sizeof(err));
  err[0] = '\0';
  bool rejects_cos =
      built && !nyr_tv_smt_equiv(&sine, &cosine, err, sizeof(err));

  double zero = 0.0;
  int64_t input[1] = {0};
  memcpy(&input[0], &zero, sizeof(zero));
  nyr_eval_result_t rs = {0}, rc = {0};
  char eval_err[192] = {0};
  bool concrete_diff =
      built &&
      nyr_ir_eval(&sine, input, 1, 100, &rs, eval_err, sizeof(eval_err)) &&
      nyr_ir_eval(&cosine, input, 1, 100, &rc, eval_err, sizeof(eval_err)) &&
      rs.returned && rc.returned && rs.result == 1 && rc.result == 0;
  if (!built || !same || !rejects_cos || !concrete_diff)
    fprintf(stderr,
            "[tv-smt-trig-cfg] built=%s congruent=%s rejects-cos=%s "
            "concrete=%s: %s\n",
            built ? "yes" : "no", same ? "yes" : "no",
            rejects_cos ? "yes" : "no", concrete_diff ? "yes" : "no",
            err[0]        ? err
            : eval_err[0] ? eval_err
                          : "proof result mismatch");
  nyr_eval_result_free(&rs);
  nyr_eval_result_free(&rc);
  nyr_ir_func_free(&sine);
  nyr_ir_func_free(&copied_sine);
  nyr_ir_func_free(&cosine);
  return built && same && rejects_cos && concrete_diff;
}

static bool build_cfg_float_arithmetic(nyr_func_t *f, bool single,
                                       nyr_op_t op) {
  nyr_opt_begin(f);
  int x = load_local(f, 0);
  int one =
      single ? nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CONST_F32,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = (int64_t)UINT32_C(0x3f800000)})
             : nyr_opt_const_f64(f, 1.0);
  int result = nyr_opt_binop(f, op, x, one);
  int zero = single ? nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CONST_F32,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .c = -1,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1,
                                                 .imm = 0})
                    : nyr_opt_const_f64(f, 0.0);
  int predicate =
      nyr_ir_emit(f, (nyr_inst_t){.op = single ? NYIR_CMP_F32 : NYIR_CMP_F64,
                                 .dst = -1,
                                 .a = result,
                                 .b = zero,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .cmp = NYIR_CMP_GT});
  int condition = load_local(f, 1);
  if (x < 0 || one < 0 || result < 0 || zero < 0 || predicate < 0 ||
      condition < 0)
    return false;
  nyr_opt_br_if(f, condition, 1);
  nyr_opt_ret(f, predicate);
  nyr_opt_label(f, 1);
  nyr_opt_ret(f, predicate);
  return nyr_opt_verify(f, "tv-smt-fp-arithmetic-cfg");
}

static bool rejects_cfg_float_arithmetic_change(bool single) {
  nyr_func_t before = {0}, after = {0};
  nyr_op_t add = single ? NYIR_ADD_F32 : NYIR_ADD_F64;
  nyr_op_t sub = single ? NYIR_SUB_F32 : NYIR_SUB_F64;
  bool built = build_cfg_float_arithmetic(&before, single, add) &&
               build_cfg_float_arithmetic(&after, single, sub);
  int64_t inputs_before[2] = {0, 0};
  int64_t inputs_after[2] = {0, 0};
  nyr_eval_result_t rb = {0}, ra = {0};
  char eval_err[192] = {0};
  bool eval_ok = built &&
                 nyr_ir_eval(&before, inputs_before, 2, 100, &rb, eval_err,
                           sizeof(eval_err)) &&
                 nyr_ir_eval(&after, inputs_after, 2, 100, &ra, eval_err,
                           sizeof(eval_err)) &&
                 rb.returned && ra.returned && rb.result != ra.result;
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&before, &after, err, sizeof(err));
  if (!built || !eval_ok || equivalent) {
    fprintf(stderr,
            "[tv-smt-fp-arithmetic-cfg] failed to reject %s arithmetic change: "
            "%s\n",
            single ? "f32" : "f64",
            err[0]        ? err
            : eval_err[0] ? eval_err
                          : "unexpected equivalence");
  }
  nyr_eval_result_free(&rb);
  nyr_eval_result_free(&ra);
  nyr_ir_func_free(&before);
  nyr_ir_func_free(&after);
  return built && eval_ok && !equivalent;
}

static bool build_float_to_i64(nyr_func_t *f, bool single, int64_t result) {
  nyr_opt_begin(f);
  int value = single ? nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CONST_F32,
                                                  .dst = -1,
                                                  .a = -1,
                                                  .b = -1,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1,
                                                  .imm = (int64_t)UINT32_C(
                                                      0xc0700000)}) /* -3.75f */
                     : nyr_opt_const_f64(f, -3.75);
  int converted =
      value < 0 ? -1
                : nyr_ir_emit(f, (nyr_inst_t){.op = single ? NYIR_F32_TO_I64
                                                          : NYIR_F64_TO_I64,
                                             .dst = -1,
                                             .a = value,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1});
  if (value < 0 || converted < 0)
    return false;
  nyr_opt_label(f,
                7); /* Route through CFG SMT, including scalar return typing. */
  nyr_opt_ret(f, result ? nyr_opt_const(f, result) : converted);
  return nyr_opt_verify(f, "tv-smt-fp-to-i64");
}

static bool checks_float_to_i64(bool single) {
  nyr_func_t converted = {0}, same = {0}, wrong = {0};
  bool built = build_float_to_i64(&converted, single, 0) &&
               build_float_to_i64(&same, single, 0) &&
               build_float_to_i64(&wrong, single, -4);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&converted, &same, err, sizeof(err));
  err[0] = '\0';
  bool rejected =
      built && !nyr_tv_smt_equiv(&converted, &wrong, err, sizeof(err));
  if (!built || !equivalent || !rejected)
    fprintf(stderr, "[tv-smt-fp-to-i64] %s: %s\n", single ? "f32" : "f64",
            err[0] ? err : "conversion equivalence/rejection failed");
  nyr_ir_func_free(&converted);
  nyr_ir_func_free(&same);
  nyr_ir_func_free(&wrong);
  return built && equivalent && rejected;
}

static bool build_f64_f32_roundtrip_cmp(nyr_func_t *f, double input) {
  nyr_opt_begin(f);
  int original = nyr_opt_const_f64(f, input);
  int narrowed = original < 0
                     ? -1
                     : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_F64_TO_F32,
                                                  .dst = -1,
                                                  .a = original,
                                                  .b = -1,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1});
  int widened = narrowed < 0 ? -1
                             : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_F32_TO_F64,
                                                          .dst = -1,
                                                          .a = narrowed,
                                                          .b = -1,
                                                          .c = -1,
                                                          .d = -1,
                                                          .e = -1,
                                                          .f = -1});
  int same = widened < 0 ? -1
                         : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CMP_F64,
                                                      .dst = -1,
                                                      .a = original,
                                                      .b = widened,
                                                      .c = -1,
                                                      .d = -1,
                                                      .e = -1,
                                                      .f = -1,
                                                      .cmp = NYIR_CMP_EQ});
  if (original < 0 || narrowed < 0 || widened < 0 || same < 0)
    return false;
  nyr_opt_ret(f, same);
  return nyr_opt_verify(f, "tv-smt-f32-roundtrip");
}

static bool checks_f64_f32_roundtrip(void) {
  nyr_func_t exact = {0}, exact_true = {0}, inexact = {0}, inexact_false = {0};
  bool built = build_f64_f32_roundtrip_cmp(&exact, 1.5) &&
               build_f64_f32_roundtrip_cmp(&inexact, 1.1);
  nyr_opt_begin(&exact_true);
  int yes = nyr_opt_const(&exact_true, 1);
  built = built && yes >= 0;
  if (built)
    nyr_opt_ret(&exact_true, yes);
  built = built && nyr_opt_verify(&exact_true, "tv-smt-f32-roundtrip-exact");
  nyr_opt_begin(&inexact_false);
  int no = nyr_opt_const(&inexact_false, 0);
  built = built && no >= 0;
  if (built)
    nyr_opt_ret(&inexact_false, no);
  built =
      built && nyr_opt_verify(&inexact_false, "tv-smt-f32-roundtrip-inexact");

  char err[256] = {0};
  bool exact_ok =
      built && nyr_tv_smt_equiv(&exact, &exact_true, err, sizeof(err));
  err[0] = '\0';
  bool inexact_ok =
      built && nyr_tv_smt_equiv(&inexact, &inexact_false, err, sizeof(err));
  err[0] = '\0';
  bool rejected =
      built && !nyr_tv_smt_equiv(&inexact, &exact_true, err, sizeof(err));
  if (!built || !exact_ok || !inexact_ok || !rejected)
    fprintf(stderr, "[tv-smt-f32-roundtrip] %s\n",
            err[0] ? err : "exact/inexact conversion result was mishandled");
  nyr_ir_func_free(&exact);
  nyr_ir_func_free(&exact_true);
  nyr_ir_func_free(&inexact);
  nyr_ir_func_free(&inexact_false);
  return built && exact_ok && inexact_ok && rejected;
}

/*
 * Straightline (no CFG) float-flavored functions.  The input local carries
 * f64 bit patterns; results are i64, so the SMT straightline path must model
 * the float intermediates exactly instead of falling back to the bounded
 * interpreter.
 */
static bool build_straightline_float(nyr_func_t *f, int mode) {
  nyr_opt_begin(f);
  int x = load_local(f, 0);
  int result = -1;
  if (mode == 0 || mode == 2) { /* (int64)(x * 2.0) / (int64)(x * 2.5) */
    int factor = nyr_opt_const_f64(f, mode == 0 ? 2.0 : 2.5);
    int product = factor < 0 ? -1 : nyr_opt_binop(f, NYIR_MUL_F64, x, factor);
    result = product < 0 ? -1
                         : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_F64_TO_I64,
                                                      .dst = -1,
                                                      .a = product,
                                                      .b = -1,
                                                      .c = -1,
                                                      .d = -1,
                                                      .e = -1,
                                                      .f = -1});
  } else if (mode == 1) { /* (int64)(x + x) */
    int doubled = nyr_opt_binop(f, NYIR_ADD_F64, x, x);
    result = doubled < 0 ? -1
                         : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_F64_TO_I64,
                                                      .dst = -1,
                                                      .a = doubled,
                                                      .b = -1,
                                                      .c = -1,
                                                      .d = -1,
                                                      .e = -1,
                                                      .f = -1});
  } else if (mode == 3) { /* (int64)(double)x */
    int widened = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_I64_TO_F64,
                                             .dst = -1,
                                             .a = x,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1});
    result = widened < 0 ? -1
                         : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_F64_TO_I64,
                                                      .dst = -1,
                                                      .a = widened,
                                                      .b = -1,
                                                      .c = -1,
                                                      .d = -1,
                                                      .e = -1,
                                                      .f = -1});
  } else if (mode == 4) { /* identity */
    result = x;
  }
  if (x < 0 || result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-straightline-float");
}

static bool checks_straightline_float_conversions(void) {
  nyr_func_t scaled = {0}, doubled = {0}, other = {0};
  bool built = build_straightline_float(&scaled, 0) &&
               build_straightline_float(&doubled, 1) &&
               build_straightline_float(&other, 2);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&scaled, &doubled, err, sizeof(err));
  err[0] = '\0';
  bool rejected =
      built && !nyr_tv_smt_equiv(&scaled, &other, err, sizeof(err));
  /*
   * The rejection must come from the solver, not the interpreter fallback:
   * the fallback's message is "native TV: trial N eval divergence", while a
   * solver rejection reports "SMT TV: counterexample".  Asserting the SMT
   * message pins the straightline float encoding (MUL_F64/ADD_F64 and the
   * F64_TO_I64 bridge) rather than any concrete sampling behavior.
   */
  bool solver_rejected = rejected && strstr(err, "SMT TV") != NULL;
  if (!built || !equivalent || !solver_rejected)
    fprintf(stderr,
            "[tv-smt-straightline-float] scaling: built=%s equivalent=%s "
            "smt-rejects=%s message=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejected ? "yes" : "no", solver_rejected ? "yes" : "no",
            err[0] ? err : "conversion equivalence/rejection failed");

  /*
   * The f64 round trip loses precision for |x| >= 2^53; the solver must
   * reject the lossy rewrite through the I64_TO_F64 encoding.
   */
  nyr_func_t roundtrip = {0}, identity = {0};
  bool built_pair = build_straightline_float(&roundtrip, 3) &&
                    build_straightline_float(&identity, 4);
  err[0] = '\0';
  bool smt_rejects =
      built_pair && !nyr_tv_smt_equiv(&roundtrip, &identity, err, sizeof(err));
  bool solver_rejected_roundtrip = smt_rejects && strstr(err, "SMT TV") != NULL;
  if (!built_pair || !solver_rejected_roundtrip)
    fprintf(
        stderr,
        "[tv-smt-straightline-float] roundtrip: built=%s smt-rejects=%s: %s\n",
        built_pair ? "yes" : "no", solver_rejected_roundtrip ? "yes" : "no",
        err[0] ? err : "solver missed the 2^53 counterexample");
  nyr_ir_func_free(&scaled);
  nyr_ir_func_free(&doubled);
  nyr_ir_func_free(&other);
  nyr_ir_func_free(&roundtrip);
  nyr_ir_func_free(&identity);
  return built && equivalent && solver_rejected && built_pair &&
         solver_rejected_roundtrip;
}

/*
 * Straightline transcendental congruence through the shared uninterpreted
 * function: sin(x) must match sin(copy(x)) bit-for-bit, and the zero-input
 * distinction must separate sine from cosine.
 */
static bool build_straightline_trig(nyr_func_t *f, nyr_op_t trig,
                                    bool copy_input) {
  nyr_opt_begin(f);
  int x = load_local(f, 0);
  int operand = x;
  if (copy_input && x >= 0)
    operand = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_COPY,
                                         .dst = -1,
                                         .a = x,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
  int applied = operand < 0 ? -1
                            : nyr_ir_emit(f, (nyr_inst_t){.op = trig,
                                                         .dst = -1,
                                                         .a = operand,
                                                         .b = -1,
                                                         .c = -1,
                                                         .d = -1,
                                                         .e = -1,
                                                         .f = -1});
  int zero = nyr_opt_const_f64(f, 0.0);
  int is_zero = applied < 0 || zero < 0
                    ? -1
                    : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CMP_F64,
                                                 .dst = -1,
                                                 .a = applied,
                                                 .b = zero,
                                                 .c = -1,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1,
                                                 .cmp = NYIR_CMP_EQ});
  if (x < 0 || operand < 0 || applied < 0 || zero < 0 || is_zero < 0)
    return false;
  nyr_opt_ret(f, is_zero);
  return nyr_opt_verify(f, "tv-smt-straightline-trig");
}

static bool checks_straightline_trig_congruence(void) {
  nyr_func_t sine = {0}, copied = {0}, cosine = {0};
  bool built = build_straightline_trig(&sine, NYIR_SIN_F64, false) &&
               build_straightline_trig(&copied, NYIR_SIN_F64, true) &&
               build_straightline_trig(&cosine, NYIR_COS_F64, false);
  char err[256] = {0};
  bool congruent = built && nyr_tv_smt_equiv(&sine, &copied, err, sizeof(err));
  err[0] = '\0';
  bool rejects_cos =
      built && !nyr_tv_smt_equiv(&sine, &cosine, err, sizeof(err));
  if (!built || !congruent || !rejects_cos)
    fprintf(
        stderr,
        "[tv-smt-straightline-trig] built=%s congruent=%s rejects-cos=%s: %s\n",
        built ? "yes" : "no", congruent ? "yes" : "no",
        rejects_cos ? "yes" : "no",
        err[0] ? err : "uninterpreted congruence failed");
  nyr_ir_func_free(&sine);
  nyr_ir_func_free(&copied);
  nyr_ir_func_free(&cosine);
  return built && congruent && rejects_cos;
}

static int emit_i64_vector_reduction(nyr_func_t *f, int base, int seed) {
  int packed = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_I64,
                                          .dst = -1,
                                          .a = seed,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  if (packed < 0)
    return -1;
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_I64,
                                    .dst = -1,
                                    .a = base,
                                    .b = packed,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1});
}

static int emit_scalar_two_lane_sum(nyr_func_t *f, int base, int seed,
                                    bool wrong) {
  int first = nyr_opt_binop(f, NYIR_ADD_I64, base, seed);
  int second = wrong ? nyr_opt_const(f, 1) : seed;
  return first < 0 || second < 0
             ? -1
             : nyr_opt_binop(f, NYIR_ADD_I64, first, second);
}

static bool build_straightline_vector_reduction(nyr_func_t *f, bool vectorized,
                                                bool wrong) {
  nyr_opt_begin(f);
  int base = load_local(f, 0);
  int seed = load_local(f, 1);
  int result = base < 0 || seed < 0 ? -1
               : vectorized ? emit_i64_vector_reduction(f, base, seed)
                            : emit_scalar_two_lane_sum(f, base, seed, wrong);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-vector-reduction-straightline");
}

static bool build_vector_reduction_case(nyr_func_t *f, bool vectorized,
                                        bool wrong_second_arm) {
  nyr_opt_begin(f);
  int condition = load_local(f, 0);
  int zero = nyr_opt_const(f, 0);
  int is_zero = condition < 0 || zero < 0
                    ? -1
                    : nyr_opt_cmp(f, condition, zero, NYIR_CMP_EQ);
  if (is_zero < 0)
    return false;
  nyr_opt_br_if(f, is_zero, 1);

  int base = load_local(f, 1);
  int seed = load_local(f, 2);
  int result = base < 0 || seed < 0 ? -1
               : vectorized ? emit_i64_vector_reduction(f, base, seed)
                            : emit_scalar_two_lane_sum(f, base, seed, false);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);

  nyr_opt_label(f, 1);
  base = load_local(f, 1);
  seed = load_local(f, 2);
  result = base < 0 || seed < 0 ? -1
           : vectorized
               ? emit_i64_vector_reduction(f, base, seed)
               : emit_scalar_two_lane_sum(f, base, seed, wrong_second_arm);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-vector-reduction");
}

static bool checks_cfg_integer_vector_reduction(void) {
  nyr_func_t vectorized = {0}, scalar = {0}, wrong = {0};
  nyr_func_t straight_vector = {0}, straight_scalar = {0};
  nyr_func_t straight_wrong = {0};
  bool built =
      build_vector_reduction_case(&vectorized, true, false) &&
      build_vector_reduction_case(&scalar, false, false) &&
      build_vector_reduction_case(&wrong, false, true) &&
      build_straightline_vector_reduction(&straight_vector, true, false) &&
      build_straightline_vector_reduction(&straight_scalar, false, false) &&
      build_straightline_vector_reduction(&straight_wrong, false, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vectorized, &scalar, err, sizeof(err));
  bool fallback_equivalent =
      built &&
      nyr_tv_equiv_straightline(&vectorized, &scalar, 64, err, sizeof(err));
  err[0] = '\0';
  bool rejects_changed_lane =
      built && !nyr_tv_smt_equiv(&vectorized, &wrong, err, sizeof(err));
  bool straight_equivalent =
      built &&
      nyr_tv_smt_equiv(&straight_vector, &straight_scalar, err, sizeof(err));
  err[0] = '\0';
  bool straight_rejects_changed_lane =
      built &&
      !nyr_tv_smt_equiv(&straight_vector, &straight_wrong, err, sizeof(err));
  if (!built || !equivalent || !fallback_equivalent || !rejects_changed_lane ||
      !straight_equivalent || !straight_rejects_changed_lane)
    fprintf(stderr,
            "[tv-smt-vector-reduction] built=%s equivalent=%s fallback=%s "
            "rejects-changed-lane=%s straight=%s straight-reject=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            fallback_equivalent ? "yes" : "no",
            rejects_changed_lane ? "yes" : "no",
            straight_equivalent ? "yes" : "no",
            straight_rejects_changed_lane ? "yes" : "no",
            err[0] ? err : "lane semantics mismatch");
  nyr_ir_func_free(&vectorized);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&wrong);
  nyr_ir_func_free(&straight_vector);
  nyr_ir_func_free(&straight_scalar);
  nyr_ir_func_free(&straight_wrong);
  return built && equivalent && fallback_equivalent && rejects_changed_lane &&
         straight_equivalent && straight_rejects_changed_lane;
}

static nyr_op_t scalar_i64_equivalent(nyr_op_t op) {
  switch (op) {
  case NYIR_VEC4_ADD_I64:
    return NYIR_ADD_I64;
  case NYIR_VEC4_SUB_I64:
    return NYIR_SUB_I64;
  case NYIR_VEC4_AND_I64:
    return NYIR_AND_I64;
  case NYIR_VEC4_OR_I64:
    return NYIR_OR_I64;
  case NYIR_VEC4_XOR_I64:
    return NYIR_XOR_I64;
  case NYIR_VEC4_SHL_I64:
    return NYIR_SHL_I64;
  case NYIR_VEC4_SAR_I64:
    return NYIR_SAR_I64;
  default:
    return NYIR_OP_COUNT;
  }
}

static int emit_vector_binary_reduction(nyr_func_t *f, nyr_op_t vector_op,
                                        int base, int lhs, int rhs,
                                        bool vectorized, bool wrong) {
  if (vectorized) {
    int left = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_I64,
                                          .dst = -1,
                                          .a = lhs,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
    int right = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_I64,
                                           .dst = -1,
                                           .a = rhs,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
    int packed = left < 0 || right < 0
                     ? -1
                     : nyr_ir_emit(f, (nyr_inst_t){.op = vector_op,
                                                  .dst = -1,
                                                  .a = left,
                                                  .b = right,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1});
    return packed < 0
               ? -1
               : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_I64,
                                            .dst = -1,
                                            .a = base,
                                            .b = packed,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  }
  nyr_op_t scalar_op = scalar_i64_equivalent(vector_op);
  if (scalar_op == NYIR_OP_COUNT)
    return -1;
  int first = nyr_opt_binop(f, scalar_op, lhs, rhs);
  int second = first;
  if (wrong) {
    int one = nyr_opt_const(f, 1);
    second = one < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, first, one);
  }
  int sum = first < 0 || second < 0
                ? -1
                : nyr_opt_binop(f, NYIR_ADD_I64, first, second);
  return sum < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, base, sum);
}

static bool build_vector_binary_reduction(nyr_func_t *f, nyr_op_t vector_op,
                                          bool vectorized, bool wrong) {
  nyr_opt_begin(f);
  int base = load_local(f, 0);
  int lhs = load_local(f, 1);
  int rhs = load_local(f, 2);
  if (base < 0 || lhs < 0 || rhs < 0)
    return false;
  int result = emit_vector_binary_reduction(f, vector_op, base, lhs, rhs,
                                            vectorized, wrong);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-vector-binary");
}

static bool build_cfg_vector_binary_reduction(nyr_func_t *f,
                                              nyr_op_t vector_op,
                                              bool vectorized, bool wrong) {
  nyr_opt_begin(f);
  int condition = load_local(f, 3);
  int zero = nyr_opt_const(f, 0);
  int is_zero = condition < 0 || zero < 0
                    ? -1
                    : nyr_opt_cmp(f, condition, zero, NYIR_CMP_EQ);
  if (is_zero < 0)
    return false;
  nyr_opt_br_if(f, is_zero, 1);

  int base = load_local(f, 0);
  int lhs = load_local(f, 1);
  int rhs = load_local(f, 2);
  int result = base < 0 || lhs < 0 || rhs < 0
                   ? -1
                   : emit_vector_binary_reduction(f, vector_op, base, lhs, rhs,
                                                  vectorized, wrong);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  nyr_opt_label(f, 1);
  base = load_local(f, 0);
  lhs = load_local(f, 1);
  rhs = load_local(f, 2);
  result = base < 0 || lhs < 0 || rhs < 0
               ? -1
               : emit_vector_binary_reduction(f, vector_op, base, lhs, rhs,
                                              vectorized, false);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-vector-cfg");
}

static bool checks_packed_integer_vector_ops(void) {
  static const nyr_op_t ops[] = {
      NYIR_VEC4_ADD_I64, NYIR_VEC4_SUB_I64, NYIR_VEC4_AND_I64, NYIR_VEC4_OR_I64,
      NYIR_VEC4_XOR_I64, NYIR_VEC4_SHL_I64, NYIR_VEC4_SAR_I64,
  };
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    nyr_func_t vector = {0}, scalar = {0}, wrong = {0};
    nyr_func_t cfg_vector = {0}, cfg_scalar = {0}, cfg_wrong = {0};
    bool built = build_vector_binary_reduction(&vector, ops[i], true, false) &&
                 build_vector_binary_reduction(&scalar, ops[i], false, false) &&
                 build_vector_binary_reduction(&wrong, ops[i], false, true);
    char err[256] = {0};
    bool equivalent =
        built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
    bool fallback = built && nyr_tv_equiv_straightline(&vector, &scalar, 64,
                                                        err, sizeof(err));
    err[0] = '\0';
    bool rejects =
        built && !nyr_tv_smt_equiv(&vector, &wrong, err, sizeof(err));
    bool cfg_built =
        build_cfg_vector_binary_reduction(&cfg_vector, ops[i], true, false) &&
        build_cfg_vector_binary_reduction(&cfg_scalar, ops[i], false, false) &&
        build_cfg_vector_binary_reduction(&cfg_wrong, ops[i], false, true);
    err[0] = '\0';
    bool cfg_equivalent =
        cfg_built &&
        nyr_tv_smt_equiv(&cfg_vector, &cfg_scalar, err, sizeof(err));
    bool cfg_fallback =
        cfg_built && nyr_tv_equiv_straightline(&cfg_vector, &cfg_scalar, 64,
                                                err, sizeof(err));
    err[0] = '\0';
    bool cfg_rejects = cfg_built && !nyr_tv_smt_equiv(&cfg_vector, &cfg_wrong,
                                                       err, sizeof(err));
    bool cfg_fallback_rejects =
        cfg_built && !nyr_tv_equiv_straightline(&cfg_vector, &cfg_wrong, 64,
                                                 err, sizeof(err));
    if (!built || !equivalent || !fallback || !rejects || !cfg_built ||
        !cfg_equivalent || !cfg_fallback || !cfg_rejects ||
        !cfg_fallback_rejects)
      fprintf(stderr,
              "[tv-smt-vector-binary] op=%s straight=%s/%s/%s/%s cfg=%s/%s/%s: "
              "%s\n",
              nyr_op_name(ops[i]), built ? "yes" : "no",
              equivalent ? "equiv" : "bad", fallback ? "fallback" : "bad",
              rejects ? "reject" : "bad", cfg_built ? "built" : "bad",
              cfg_equivalent ? "equiv" : "bad",
              cfg_fallback && cfg_rejects && cfg_fallback_rejects
                  ? "fallback+reject"
                  : "bad",
              err[0] ? err : "semantic mismatch");
    nyr_ir_func_free(&vector);
    nyr_ir_func_free(&scalar);
    nyr_ir_func_free(&wrong);
    nyr_ir_func_free(&cfg_vector);
    nyr_ir_func_free(&cfg_scalar);
    nyr_ir_func_free(&cfg_wrong);
    if (!built || !equivalent || !fallback || !rejects || !cfg_built ||
        !cfg_equivalent || !cfg_fallback || !cfg_rejects ||
        !cfg_fallback_rejects)
      return false;
  }
  return true;
}

static int emit_memory_vector_sum(nyr_func_t *f, bool vectorized,
                                  bool omit_second_lane) {
  int address = load_local(f, 0);
  int seed = load_local(f, 1);
  if (address < 0 || seed < 0)
    return -1;
  if (vectorized) {
    int packed = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_I64,
                                            .dst = -1,
                                            .a = address,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
    return packed < 0
               ? -1
               : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_I64,
                                            .dst = -1,
                                            .a = seed,
                                            .b = packed,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  }
  int first = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
  if (first < 0)
    return -1;
  int result = nyr_opt_binop(f, NYIR_ADD_I64, seed, first);
  if (omit_second_lane)
    return result;
  int eight = nyr_opt_const(f, 8);
  int second_address =
      eight < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, address, eight);
  int second = second_address < 0
                   ? -1
                   : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1,
                                                .a = second_address,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
  return result < 0 || second < 0
             ? -1
             : nyr_opt_binop(f, NYIR_ADD_I64, result, second);
}

static bool build_memory_vector_sum(nyr_func_t *f, bool vectorized,
                                    bool omit_second_lane) {
  nyr_opt_begin(f);
  int result = emit_memory_vector_sum(f, vectorized, omit_second_lane);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-memory-vector-straightline");
}

static bool build_cfg_memory_vector_sum(nyr_func_t *f, bool vectorized,
                                        bool wrong_second_arm) {
  nyr_opt_begin(f);
  int condition = load_local(f, 2);
  int zero = nyr_opt_const(f, 0);
  int is_zero = condition < 0 || zero < 0
                    ? -1
                    : nyr_opt_cmp(f, condition, zero, NYIR_CMP_EQ);
  if (is_zero < 0)
    return false;
  nyr_opt_br_if(f, is_zero, 1);
  int result = emit_memory_vector_sum(f, vectorized, false);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  nyr_opt_label(f, 1);
  result = emit_memory_vector_sum(f, vectorized, wrong_second_arm);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-memory-vector-cfg");
}

static int emit_memory_vec8_sum(nyr_func_t *f, bool vectorized,
                                bool omit_last_lane) {
  int address = load_local(f, 0);
  int seed = load_local(f, 1);
  if (address < 0 || seed < 0)
    return -1;
  if (vectorized) {
    int packed = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_LOAD_I64,
                                            .dst = -1,
                                            .a = address,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
    return packed < 0
               ? -1
               : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_REDUCE_ADD_I64,
                                            .dst = -1,
                                            .a = seed,
                                            .b = packed,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  }
  int sum = seed;
  for (unsigned lane = 0; lane < (omit_last_lane ? 3u : 4u); ++lane) {
    int offset = nyr_opt_const(f, (int64_t)lane * 8);
    int ptr =
        lane == 0
            ? address
            : (offset < 0 ? -1
                          : nyr_opt_binop(f, NYIR_ADD_I64, address, offset));
    int value = ptr < 0 ? -1
                        : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                     .dst = -1,
                                                     .a = ptr,
                                                     .b = -1,
                                                     .c = -1,
                                                     .d = -1,
                                                     .e = -1,
                                                     .f = -1});
    sum = value < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, sum, value);
    if (sum < 0)
      return -1;
  }
  return sum;
}

static bool build_memory_vec8_sum(nyr_func_t *f, bool vectorized,
                                  bool omit_last_lane) {
  nyr_opt_begin(f);
  int result = emit_memory_vec8_sum(f, vectorized, omit_last_lane);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-memory-vec8");
}

static bool build_cfg_memory_vec8_sum(nyr_func_t *f, bool vectorized,
                                      bool omit_last_lane) {
  nyr_opt_begin(f);
  int condition = load_local(f, 2);
  int zero = nyr_opt_const(f, 0);
  int is_zero = condition < 0 || zero < 0
                    ? -1
                    : nyr_opt_cmp(f, condition, zero, NYIR_CMP_EQ);
  if (is_zero < 0)
    return false;
  nyr_opt_br_if(f, is_zero, 1);
  int result = emit_memory_vec8_sum(f, vectorized, false);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  nyr_opt_label(f, 1);
  result = emit_memory_vec8_sum(f, vectorized, omit_last_lane);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-memory-vec8-cfg");
}

static bool checks_memory_vec8_semantics(void) {
  nyr_func_t vector = {0}, scalar = {0}, wrong = {0};
  nyr_func_t cfg_vector = {0}, cfg_scalar = {0}, cfg_wrong = {0};
  bool built = build_memory_vec8_sum(&vector, true, false) &&
               build_memory_vec8_sum(&scalar, false, false) &&
               build_memory_vec8_sum(&wrong, false, true) &&
               build_cfg_memory_vec8_sum(&cfg_vector, true, false) &&
               build_cfg_memory_vec8_sum(&cfg_scalar, false, false) &&
               build_cfg_memory_vec8_sum(&cfg_wrong, false, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
  err[0] = '\0';
  bool rejects_missing_lane =
      built && !nyr_tv_smt_equiv(&vector, &wrong, err, sizeof(err));
  err[0] = '\0';
  bool cfg_equivalent =
      built && nyr_tv_smt_equiv(&cfg_vector, &cfg_scalar, err, sizeof(err));
  err[0] = '\0';
  bool cfg_rejects_missing_lane =
      built && !nyr_tv_smt_equiv(&cfg_vector, &cfg_wrong, err, sizeof(err));
  if (!built || !equivalent || !rejects_missing_lane || !cfg_equivalent ||
      !cfg_rejects_missing_lane)
    fprintf(stderr,
            "[tv-smt-memory-vec8] built=%s straight=%s/%s cfg=%s/%s: %s\n",
            built ? "yes" : "no", equivalent ? "equiv" : "bad",
            rejects_missing_lane ? "reject" : "bad",
            cfg_equivalent ? "equiv" : "bad",
            cfg_rejects_missing_lane ? "reject" : "bad",
            err[0] ? err : "four-lane memory mismatch");
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&wrong);
  nyr_ir_func_free(&cfg_vector);
  nyr_ir_func_free(&cfg_scalar);
  nyr_ir_func_free(&cfg_wrong);
  return built && equivalent && rejects_missing_lane && cfg_equivalent &&
         cfg_rejects_missing_lane;
}

static nyr_op_t scalar_vec8_op(nyr_op_t op) {
  switch (op) {
  case NYIR_VEC8_ADD_I64:
    return NYIR_ADD_I64;
  case NYIR_VEC8_SUB_I64:
    return NYIR_SUB_I64;
  case NYIR_VEC8_AND_I64:
    return NYIR_AND_I64;
  case NYIR_VEC8_OR_I64:
    return NYIR_OR_I64;
  case NYIR_VEC8_XOR_I64:
    return NYIR_XOR_I64;
  default:
    return NYIR_OP_COUNT;
  }
}

static bool checks_memory_vec8_binary_semantics(void) {
  static const nyr_op_t ops[] = {NYIR_VEC8_ADD_I64, NYIR_VEC8_SUB_I64,
                                  NYIR_VEC8_AND_I64, NYIR_VEC8_OR_I64,
                                  NYIR_VEC8_XOR_I64};
  for (size_t op_index = 0; op_index < sizeof(ops) / sizeof(ops[0]);
       ++op_index) {
    nyr_op_t vector_op = ops[op_index];
    nyr_op_t scalar_op = scalar_vec8_op(vector_op);
    nyr_func_t vector = {0}, scalar = {0};
    nyr_opt_begin(&vector);
    int left = load_local(&vector, 0);
    int right = load_local(&vector, 1);
    int seed = load_local(&vector, 2);
    int lhs = nyr_ir_emit(&vector, (nyr_inst_t){.op = NYIR_VEC8_LOAD_I64,
                                               .dst = -1,
                                               .a = left,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
    int rhs = nyr_ir_emit(&vector, (nyr_inst_t){.op = NYIR_VEC8_LOAD_I64,
                                               .dst = -1,
                                               .a = right,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
    int packed = nyr_ir_emit(&vector, (nyr_inst_t){.op = vector_op,
                                                  .dst = -1,
                                                  .a = lhs,
                                                  .b = rhs,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1});
    int total = nyr_ir_emit(&vector, (nyr_inst_t){.op = NYIR_VEC8_REDUCE_ADD_I64,
                                                 .dst = -1,
                                                 .a = seed,
                                                 .b = packed,
                                                 .c = -1,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1});
    bool built = left >= 0 && right >= 0 && seed >= 0 && lhs >= 0 && rhs >= 0 &&
                 packed >= 0 && total >= 0;
    if (built) {
      nyr_opt_ret(&vector, total);
      built = nyr_opt_verify(&vector, "tv-smt-memory-vec8-add");
    }
    nyr_opt_begin(&scalar);
    left = load_local(&scalar, 0);
    right = load_local(&scalar, 1);
    total = load_local(&scalar, 2);
    for (unsigned lane = 0; built && lane < 4; ++lane) {
      int offset = nyr_opt_const(&scalar, (int64_t)lane * 8);
      int lp = lane ? nyr_opt_binop(&scalar, NYIR_ADD_I64, left, offset) : left;
      int rp =
          lane ? nyr_opt_binop(&scalar, NYIR_ADD_I64, right, offset) : right;
      int lv = nyr_ir_emit(&scalar, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1,
                                                .a = lp,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
      int rv = nyr_ir_emit(&scalar, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1,
                                                .a = rp,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
      int sum = nyr_opt_binop(&scalar, scalar_op, lv, rv);
      total = nyr_opt_binop(&scalar, NYIR_ADD_I64, total, sum);
      built = offset >= 0 && lp >= 0 && rp >= 0 && lv >= 0 && rv >= 0 &&
              sum >= 0 && total >= 0;
    }
    if (built) {
      nyr_opt_ret(&scalar, total);
      built = nyr_opt_verify(&scalar, "tv-smt-memory-vec8-add-scalar");
    }
    char err[256] = {0};
    bool equivalent =
        built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
    if (!built || !equivalent)
      fprintf(stderr, "[tv-smt-memory-vec8-add] built=%s equivalent=%s: %s\n",
              built ? "yes" : "no", equivalent ? "yes" : "no",
              err[0] ? err : "four-lane add mismatch");
    nyr_ir_func_free(&vector);
    nyr_ir_func_free(&scalar);
    if (!built || !equivalent)
      return false;
  }
  return true;
}

static int emit_f64_to_i64(nyr_func_t *f, int value) {
  return value < 0 ? -1
                   : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_F64_TO_I64,
                                                .dst = -1,
                                                .a = value,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
}

static int emit_load_i64(nyr_func_t *f, int address) {
  return address < 0 ? -1
                     : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                  .dst = -1,
                                                  .a = address,
                                                  .b = -1,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1});
}

static bool build_store_load(nyr_func_t *f, int64_t stored_value,
                             bool byte_store) {
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int value = nyr_opt_const(f, stored_value);
  if (address < 0 || value < 0)
    return false;
  (void)nyr_ir_emit(
      f, (nyr_inst_t){.op = NYIR_STORE_I64,
                       .dst = -1,
                       .a = address,
                       .b = -1,
                       .c = value,
                       .d = -1,
                       .e = -1,
                       .f = -1,
                       .flags = byte_store ? NYIR_INST_F_MEM_BYTE : 0});
  int loaded = emit_load_i64(f, address);
  if (loaded < 0)
    return false;
  nyr_opt_ret(f, loaded);
  return nyr_opt_verify(f, "tv-smt-store-load");
}

static bool build_split_byte_store_load(nyr_func_t *f, uint64_t stored_value) {
  nyr_opt_begin(f);
  int base = load_local(f, 0);
  if (base < 0)
    return false;
  for (unsigned i = 0; i < 8; ++i) {
    int address = base;
    if (i != 0) {
      int offset = nyr_opt_const(f, i);
      address = offset < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
    }
    int byte = nyr_opt_const(f, (stored_value >> (i * 8)) & 0xffu);
    if (address < 0 || byte < 0)
      return false;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = byte,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .flags = NYIR_INST_F_MEM_BYTE});
  }
  int loaded = emit_load_i64(f, base);
  if (loaded < 0)
    return false;
  nyr_opt_ret(f, loaded);
  return nyr_opt_verify(f, "tv-smt-split-byte-store-load");
}

static int emit_scalar_store_flags(nyr_func_t *f, int address, int value,
                                   unsigned flags) {
  if (address < 0 || value < 0)
    return -1;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = address,
                                   .b = -1,
                                   .c = value,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .flags = flags});
  return 0;
}

static int emit_scalar_store(nyr_func_t *f, int address, int value) {
  return emit_scalar_store_flags(f, address, value, 0);
}

static bool emit_store_bytes(nyr_func_t *f, int base, uint64_t value,
                             unsigned byte_count) {
  for (unsigned i = 0; i < byte_count; ++i) {
    int address = base;
    if (i != 0) {
      int offset = nyr_opt_const(f, i);
      address = offset < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
    }
    int byte = nyr_opt_const(f, (value >> (i * 8)) & 0xffu);
    if (emit_scalar_store_flags(f, address, byte, NYIR_INST_F_MEM_BYTE) < 0)
      return false;
  }
  return true;
}

static bool build_cfg_store_load(nyr_func_t *f,
                                 unsigned alternate_byte_count) {
  const uint64_t stored = UINT64_C(0x0123456789abcdef);
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int condition = load_local(f, 1);
  int zero = nyr_opt_const(f, 0);
  int is_zero = address < 0 || condition < 0 || zero < 0
                    ? -1
                    : nyr_opt_cmp(f, condition, zero, NYIR_CMP_EQ);
  int value = nyr_opt_const(f, (int64_t)stored);
  if (is_zero < 0 || value < 0)
    return false;

  nyr_opt_br_if(f, is_zero, 1);
  if (emit_scalar_store(f, address, value) < 0)
    return false;
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 1);
  if (alternate_byte_count == 0) {
    if (emit_scalar_store(f, address, value) < 0)
      return false;
  } else if (!emit_store_bytes(f, address, stored, alternate_byte_count)) {
    return false;
  }
  nyr_opt_label(f, 2);
  int loaded = emit_load_i64(f, address);
  if (loaded < 0)
    return false;
  nyr_opt_ret(f, loaded);
  return nyr_opt_verify(f, "tv-smt-cfg-store-load");
}

static bool checks_cfg_path_local_store_semantics(void) {
  nyr_func_t full_store = {0}, split_store = {0}, partial_store = {0};
  bool built = build_cfg_store_load(&full_store, 0) &&
               build_cfg_store_load(&split_store, 8) &&
               build_cfg_store_load(&partial_store, 1);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&full_store, &split_store, err, sizeof(err));
  err[0] = '\0';
  bool rejects_partial =
      built &&
      !nyr_tv_smt_equiv(&full_store, &partial_store, err, sizeof(err));
  uint64_t vm_memory[2] = {0};
  int64_t vm_inputs[2] = {(int64_t)(uintptr_t)vm_memory, 0};
  nyr_eval_result_t reference_eval = {0}, split_eval = {0};
  nyr_eval_result_t partial_eval = {0};
  char eval_err[192] = {0};
  bool bounded_agrees =
      built &&
      nyr_ir_eval(&full_store, vm_inputs, 2, 1000, &reference_eval, eval_err,
                sizeof(eval_err)) &&
      nyr_ir_eval(&split_store, vm_inputs, 2, 1000, &split_eval, eval_err,
                sizeof(eval_err)) &&
      reference_eval.returned && split_eval.returned &&
      reference_eval.result == INT64_C(0x0123456789abcdef) &&
      split_eval.result == reference_eval.result;
  vm_inputs[1] = 1;
  bounded_agrees = bounded_agrees &&
                   nyr_ir_eval(&full_store, vm_inputs, 2, 1000, &reference_eval,
                             eval_err, sizeof(eval_err)) &&
                   nyr_ir_eval(&split_store, vm_inputs, 2, 1000, &split_eval,
                             eval_err, sizeof(eval_err)) &&
                   reference_eval.returned && split_eval.returned &&
                   reference_eval.result == INT64_C(0x0123456789abcdef) &&
                   split_eval.result == reference_eval.result;
  vm_memory[0] = 0;
  vm_memory[1] = 0;
  vm_inputs[1] = 0;
  bounded_agrees = bounded_agrees &&
                   nyr_ir_eval(&partial_store, vm_inputs, 2, 1000, &partial_eval,
                             eval_err, sizeof(eval_err)) &&
                   partial_eval.returned &&
                   partial_eval.result != INT64_C(0x0123456789abcdef);
  if (!built || !equivalent || !rejects_partial || !bounded_agrees)
    fprintf(stderr,
            "[tv-smt-cfg-path-local-store] built=%s full-vs-bytes=%s "
            "rejects-partial=%s bounded=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_partial ? "yes" : "no", bounded_agrees ? "yes" : "no",
            err[0] ? err
                   : (eval_err[0] ? eval_err
                                  : "CFG store/load memory state mismatch"));
  nyr_ir_func_free(&full_store);
  nyr_ir_func_free(&split_store);
  nyr_ir_func_free(&partial_store);
  return built && equivalent && rejects_partial && bounded_agrees;
}

static bool build_vector_store_roundtrip(nyr_func_t *f, unsigned lanes,
                                         bool vectorized,
                                         bool omit_last_store) {
  nyr_opt_begin(f);
  int source = load_local(f, 0);
  int destination = load_local(f, 1);
  if (source < 0 || destination < 0 || (lanes != 2 && lanes != 4))
    return false;

  int result = -1;
  if (vectorized) {
    int packed =
        nyr_ir_emit(f, (nyr_inst_t){.op = lanes == 2 ? NYIR_VEC4_LOAD_I64
                                                    : NYIR_VEC8_LOAD_I64,
                                   .dst = -1,
                                   .a = source,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
    if (packed < 0)
      return false;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = lanes == 2 ? NYIR_VEC4_STORE_I64
                                                      : NYIR_VEC8_STORE_I64,
                                     .dst = -1,
                                     .a = destination,
                                     .b = packed,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    int loaded =
        nyr_ir_emit(f, (nyr_inst_t){.op = lanes == 2 ? NYIR_VEC4_LOAD_I64
                                                    : NYIR_VEC8_LOAD_I64,
                                   .dst = -1,
                                   .a = destination,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
    int zero = nyr_opt_const(f, 0);
    result = loaded < 0 || zero < 0
                 ? -1
                 : nyr_ir_emit(f, (nyr_inst_t){
                                    .op = lanes == 2 ? NYIR_VEC4_REDUCE_ADD_I64
                                                     : NYIR_VEC8_REDUCE_ADD_I64,
                                    .dst = -1,
                                    .a = zero,
                                    .b = loaded,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1});
  } else {
    int values[4] = {-1, -1, -1, -1};
    for (unsigned lane = 0; lane < lanes; ++lane) {
      int offset = nyr_opt_const(f, (int64_t)lane * 8);
      int address = lane == 0 ? source
                    : offset < 0
                        ? -1
                        : nyr_opt_binop(f, NYIR_ADD_I64, source, offset);
      values[lane] = address < 0 ? -1 : emit_load_i64(f, address);
      if (values[lane] < 0)
        return false;
    }
    for (unsigned lane = 0; lane < lanes; ++lane) {
      if (omit_last_store && lane + 1 == lanes)
        continue;
      int offset = nyr_opt_const(f, (int64_t)lane * 8);
      int address = lane == 0 ? destination
                    : offset < 0
                        ? -1
                        : nyr_opt_binop(f, NYIR_ADD_I64, destination, offset);
      if (emit_scalar_store(f, address, values[lane]) < 0)
        return false;
    }
    int zero = nyr_opt_const(f, 0);
    result = zero;
    for (unsigned lane = 0; lane < lanes; ++lane) {
      int offset = nyr_opt_const(f, (int64_t)lane * 8);
      int address = lane == 0 ? destination
                    : offset < 0
                        ? -1
                        : nyr_opt_binop(f, NYIR_ADD_I64, destination, offset);
      int value = address < 0 ? -1 : emit_load_i64(f, address);
      result = value < 0 || result < 0
                   ? -1
                   : nyr_opt_binop(f, NYIR_ADD_I64, result, value);
    }
  }
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-vector-store-roundtrip");
}

static bool checks_vector_store_semantics(unsigned lanes) {
  nyr_func_t vector = {0}, scalar = {0}, missing_lane = {0};
  bool built = build_vector_store_roundtrip(&vector, lanes, true, false) &&
               build_vector_store_roundtrip(&scalar, lanes, false, false) &&
               build_vector_store_roundtrip(&missing_lane, lanes, false, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
  err[0] = '\0';
  bool rejects_missing_lane =
      built && !nyr_tv_smt_equiv(&vector, &missing_lane, err, sizeof(err));
  if (!built || !equivalent || !rejects_missing_lane)
    fprintf(stderr,
            "[tv-smt-vector-store-%u] built=%s equivalent=%s "
            "rejects-missing-lane=%s: %s\n",
            lanes, built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_missing_lane ? "yes" : "no",
            err[0] ? err : "packed store semantics mismatch");
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&missing_lane);
  return built && equivalent && rejects_missing_lane;
}

static bool emit_copy_to_destination(nyr_func_t *f, int source,
                                     int destination, unsigned lanes,
                                     bool vectorized, bool omit_last_store) {
  if (vectorized) {
    int packed =
        nyr_ir_emit(f, (nyr_inst_t){.op = lanes == 2 ? NYIR_VEC4_LOAD_I64
                                                    : NYIR_VEC8_LOAD_I64,
                                   .dst = -1,
                                   .a = source,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
    if (packed < 0)
      return false;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = lanes == 2 ? NYIR_VEC4_STORE_I64
                                                      : NYIR_VEC8_STORE_I64,
                                     .dst = -1,
                                     .a = destination,
                                     .b = packed,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    return true;
  }

  int values[4] = {-1, -1, -1, -1};
  for (unsigned lane = 0; lane < lanes; ++lane) {
    int offset = nyr_opt_const(f, (int64_t)lane * 8);
    int address = lane == 0    ? source
                  : offset < 0 ? -1
                               : nyr_opt_binop(f, NYIR_ADD_I64, source, offset);
    values[lane] = address < 0 ? -1 : emit_load_i64(f, address);
    if (values[lane] < 0)
      return false;
  }
  for (unsigned lane = 0; lane < lanes; ++lane) {
    if (omit_last_store && lane + 1 == lanes)
      continue;
    int offset = nyr_opt_const(f, (int64_t)lane * 8);
    int address = lane == 0 ? destination
                  : offset < 0
                      ? -1
                      : nyr_opt_binop(f, NYIR_ADD_I64, destination, offset);
    if (emit_scalar_store(f, address, values[lane]) < 0)
      return false;
  }
  return true;
}

static int emit_destination_sum(nyr_func_t *f, int destination, unsigned lanes,
                                bool vectorized) {
  if (vectorized) {
    int loaded =
        nyr_ir_emit(f, (nyr_inst_t){.op = lanes == 2 ? NYIR_VEC4_LOAD_I64
                                                    : NYIR_VEC8_LOAD_I64,
                                   .dst = -1,
                                   .a = destination,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
    int zero = nyr_opt_const(f, 0);
    return loaded < 0 || zero < 0
               ? -1
               : nyr_ir_emit(f, (nyr_inst_t){
                                  .op = lanes == 2 ? NYIR_VEC4_REDUCE_ADD_I64
                                                   : NYIR_VEC8_REDUCE_ADD_I64,
                                  .dst = -1,
                                  .a = zero,
                                  .b = loaded,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1});
  }
  int sum = nyr_opt_const(f, 0);
  for (unsigned lane = 0; lane < lanes; ++lane) {
    int offset = nyr_opt_const(f, (int64_t)lane * 8);
    int address = lane == 0 ? destination
                  : offset < 0
                      ? -1
                      : nyr_opt_binop(f, NYIR_ADD_I64, destination, offset);
    int value = address < 0 ? -1 : emit_load_i64(f, address);
    sum =
        value < 0 || sum < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, sum, value);
  }
  return sum;
}

static bool build_cfg_vector_store_roundtrip(nyr_func_t *f, unsigned lanes,
                                             bool vectorized,
                                             bool omit_last_store) {
  nyr_opt_begin(f);
  int source = load_local(f, 0);
  int destination = load_local(f, 1);
  int condition = load_local(f, 2);
  int zero = nyr_opt_const(f, 0);
  int branch_condition = condition < 0 || zero < 0
                             ? -1
                             : nyr_opt_cmp(f, condition, zero, NYIR_CMP_EQ);
  if (source < 0 || destination < 0 || branch_condition < 0 ||
      (lanes != 2 && lanes != 4))
    return false;
  nyr_opt_br_if(f, branch_condition, 10);
  if (!emit_copy_to_destination(f, source, destination, lanes, vectorized,
                                omit_last_store))
    return false;
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 10);
  if (!emit_copy_to_destination(f, source, destination, lanes, vectorized,
                                omit_last_store))
    return false;
  nyr_opt_label(f, 20);
  int result = emit_destination_sum(f, destination, lanes, vectorized);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-cfg-vector-store-roundtrip");
}

static bool checks_cfg_vector_store_semantics(unsigned lanes) {
  nyr_func_t vector = {0}, scalar = {0}, missing_lane = {0};
  bool built =
      build_cfg_vector_store_roundtrip(&vector, lanes, true, false) &&
      build_cfg_vector_store_roundtrip(&scalar, lanes, false, false) &&
      build_cfg_vector_store_roundtrip(&missing_lane, lanes, false, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
  err[0] = '\0';
  bool rejects_missing_lane =
      built && !nyr_tv_smt_equiv(&vector, &missing_lane, err, sizeof(err));
  if (!built || !equivalent || !rejects_missing_lane)
    fprintf(stderr,
            "[tv-smt-cfg-vector-store-%u] built=%s equivalent=%s "
            "rejects-missing-lane=%s: %s\n",
            lanes, built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_missing_lane ? "yes" : "no",
            err[0] ? err : "CFG packed store mismatch");
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&missing_lane);
  return built && equivalent && rejects_missing_lane;
}

static bool checks_path_local_store_semantics(void) {
  const uint64_t expected = UINT64_C(0x0123456789abcdef);
  nyr_func_t full_store = {0}, split_store = {0}, byte_store = {0};
  bool built = build_store_load(&full_store, (int64_t)expected, false) &&
               build_split_byte_store_load(&split_store, expected) &&
               build_store_load(&byte_store, (int64_t)expected, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&full_store, &split_store, err, sizeof(err));
  err[0] = '\0';
  bool rejects_changed =
      built && !nyr_tv_smt_equiv(&full_store, &byte_store, err, sizeof(err));
  if (!built || !equivalent || !rejects_changed)
    fprintf(stderr,
            "[tv-smt-path-local-store] built=%s full-vs-bytes=%s "
            "rejects-partial=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_changed ? "yes" : "no",
            err[0] ? err : "store/load memory state mismatch");
  nyr_ir_func_free(&full_store);
  nyr_ir_func_free(&split_store);
  nyr_ir_func_free(&byte_store);
  return built && equivalent && rejects_changed;
}

static bool build_external_pointer_store_result(nyr_func_t *f,
                                                bool load_second_pointer,
                                                bool return_stored_value) {
  nyr_opt_begin(f);
  int destination = load_local(f, 0);
  int source = load_second_pointer ? load_local(f, 1) : destination;
  int value = nyr_opt_const(f, 17);
  if (destination < 0 || source < 0 || value < 0 ||
      emit_scalar_store(f, destination, value) < 0)
    return false;
  int result = return_stored_value ? value : emit_load_i64(f, source);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-external-pointer-alias");
}

static bool checks_external_pointer_aliasing(void) {
  nyr_func_t load_same = {0}, constant_same = {0}, load_unknown = {0},
              constant_unknown = {0};
  bool built =
      build_external_pointer_store_result(&load_same, false, false) &&
      build_external_pointer_store_result(&constant_same, false, true) &&
      build_external_pointer_store_result(&load_unknown, true, false) &&
      build_external_pointer_store_result(&constant_unknown, true, true);
  char err[256] = {0};
  bool proves_same_address =
      built && nyr_tv_smt_equiv(&load_same, &constant_same, err, sizeof(err));
  err[0] = '\0';
  bool rejects_unknown_alias =
      built &&
      !nyr_tv_smt_equiv(&load_unknown, &constant_unknown, err, sizeof(err));
  if (!built || !proves_same_address || !rejects_unknown_alias)
    fprintf(stderr,
            "[tv-smt-external-pointer-alias] built=%s same-address=%s "
            "unknown-alias=%s: %s\n",
            built ? "yes" : "no", proves_same_address ? "proved" : "not proved",
            rejects_unknown_alias ? "rejected" : "not rejected",
            err[0] ? err : "external-pointer alias proof was unsound");
  nyr_ir_func_free(&load_same);
  nyr_ir_func_free(&constant_same);
  nyr_ir_func_free(&load_unknown);
  nyr_ir_func_free(&constant_unknown);
  return built && proves_same_address && rejects_unknown_alias;
}

static bool build_unknown_effect_call(nyr_func_t *f, bool include_call) {
  nyr_opt_begin(f);
  int input = load_local(f, 0);
  int result = input;
  if (input < 0)
    return false;
  if (include_call) {
    result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                        .dst = -1,
                                        .a = input,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 1,
                                        .symbol = "tv_unknown_effect"});
    if (result < 0)
      return false;
  }
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-unknown-effect-call");
}

static bool keeps_unknown_effect_calls_inapplicable(void) {
  nyr_func_t with_call = {0}, without_call = {0};
  bool built = build_unknown_effect_call(&with_call, true) &&
               build_unknown_effect_call(&without_call, false);
  char err[256] = {0};
  bool inapplicable =
      built &&
      !nyr_tv_smt_equiv(&with_call, &without_call, err, sizeof(err)) &&
      err[0] == '\0';
  if (!built || !inapplicable)
    fprintf(stderr,
            "[tv-smt-unknown-effect-call] built=%s inapplicable=%s: %s\n",
            built ? "yes" : "no", inapplicable ? "yes" : "no",
            err[0] ? err : "unsupported call was not kept inapplicable");
  nyr_ir_func_free(&with_call);
  nyr_ir_func_free(&without_call);
  return built && inapplicable;
}

static int emit_tv_call(nyr_func_t *f, int arg, const char *symbol,
                        unsigned effects, bool effects_known) {
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = arg,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = 1,
                      .symbol = symbol,
                      .flags = effects_known ? NYIR_INST_F_EFFECTS_KNOWN : 0,
                      .effects = effects,
                      .scalar_class = NYIR_SCALAR_I64_RAW};
  nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_I64_RAW);
  int result = nyr_ir_emit(f, call);
  if (result >= 0)
    f->data[f->len - 1].effects = effects;
  return result;
}

static bool build_tv_call_result(nyr_func_t *f, const char *symbol,
                                 unsigned effects, bool effects_known,
                                 bool repeat_call, bool increment_arg) {
  nyr_opt_begin(f);
  int argument = load_local(f, 0);
  if (argument < 0)
    return false;
  if (increment_arg) {
    int one = nyr_opt_const(f, 1);
    if (one < 0 ||
        (argument = nyr_opt_binop(f, NYIR_ADD_I64, argument, one)) < 0)
      return false;
  }
  int result = emit_tv_call(f, argument, symbol, effects, effects_known);
  if (result < 0)
    return false;
  if (repeat_call) {
    result = emit_tv_call(f, argument, symbol, effects, effects_known);
    if (result < 0)
      return false;
  }
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-pure-call");
}

static bool checks_known_pure_call_congruence(void) {
  nyr_func_t repeated = {0}, single = {0}, changed_arg = {0},
              different_symbol = {0};
  bool built = build_tv_call_result(&repeated, "tv.pure.step", NYIR_EFFECT_CALL,
                                    true, true, false) &&
               build_tv_call_result(&single, "tv.pure.step", NYIR_EFFECT_CALL,
                                    true, false, false) &&
               build_tv_call_result(&changed_arg, "tv.pure.step",
                                    NYIR_EFFECT_CALL, true, false, true) &&
               build_tv_call_result(&different_symbol, "tv.pure.other",
                                    NYIR_EFFECT_CALL, true, false, false);
  char err[256] = {0};
  bool removes_duplicate =
      built && nyr_tv_smt_equiv(&repeated, &single, err, sizeof(err));
  err[0] = '\0';
  bool detects_changed_argument =
      built && !nyr_tv_smt_equiv(&single, &changed_arg, err, sizeof(err)) &&
      strstr(err, "counterexample") != NULL;
  err[0] = '\0';
  bool distinguishes_callees =
      built &&
      !nyr_tv_smt_equiv(&single, &different_symbol, err, sizeof(err)) &&
      strstr(err, "counterexample") != NULL;
  char self_err[256] = {0};
  bool self_equivalent =
      built && nyr_tv_smt_equiv(&single, &single, self_err, sizeof(self_err));
  if (!built || !removes_duplicate || !detects_changed_argument ||
      !distinguishes_callees || !self_equivalent) {
    const nyr_inst_t *call = NULL;
    for (size_t i = 0; i < single.len; ++i)
      if (single.data[i].op == NYIR_CALL) {
        call = &single.data[i];
        break;
      }
    int call_args[NYIR_CALL_MAX_ARGS];
    int call_argc = 0;
    bool call_args_valid =
        call && nyr_call_args(call, single.next_value, call_args,
                               NYIR_CALL_MAX_ARGS, &call_argc, NULL, 0);
    fprintf(stderr,
            "[tv-smt-pure-call] built=%s duplicate=%s changed-arg=%s "
            "callee-identity=%s self=%s call=%s flags=0x%x effects=0x%x "
            "argc=%d args-valid=%s: %s\n",
            built ? "yes" : "no", removes_duplicate ? "proved" : "failed",
            detects_changed_argument ? "rejected" : "missed",
            distinguishes_callees ? "rejected" : "missed",
            self_equivalent ? "proved" : "inapplicable",
            call ? call->symbol : "<missing>", call ? call->flags : 0,
            call ? call->effects : 0, call_argc, call_args_valid ? "yes" : "no",
            self_err[0] ? self_err
            : err[0]    ? err
                        : "pure call congruence mismatch");
  }
  nyr_ir_func_free(&repeated);
  nyr_ir_func_free(&single);
  nyr_ir_func_free(&changed_arg);
  nyr_ir_func_free(&different_symbol);
  return built && removes_duplicate && detects_changed_argument &&
         distinguishes_callees;
}

static int emit_tv_write_call_load(nyr_func_t *f, int address, int value,
                                   const char *symbol, unsigned effects,
                                   bool store_before_call) {
  if (store_before_call && emit_scalar_store(f, address, value) < 0)
    return -1;
  if (emit_tv_call(f, address, symbol, effects, true) < 0)
    return -1;
  if (!store_before_call && emit_scalar_store(f, address, value) < 0)
    return -1;
  return emit_load_i64(f, address);
}

static bool build_tv_write_call_load(nyr_func_t *f, const char *symbol,
                                     unsigned effects, bool store_before_call,
                                     bool duplicate_cfg_paths) {
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int value = load_local(f, 1);
  if (address < 0 || value < 0)
    return false;
  if (duplicate_cfg_paths) {
    int condition = load_local(f, 2);
    int zero = nyr_opt_const(f, 0);
    if (condition < 0 || zero < 0)
      return false;
    int is_zero = nyr_opt_cmp(f, condition, zero, NYIR_CMP_EQ);
    if (is_zero < 0)
      return false;
    nyr_opt_br_if(f, is_zero, 1);
    int first = emit_tv_write_call_load(f, address, value, symbol, effects,
                                        store_before_call);
    if (first < 0)
      return false;
    nyr_opt_ret(f, first);
    nyr_opt_label(f, 1);
    int second = emit_tv_write_call_load(f, address, value, symbol, effects,
                                         store_before_call);
    if (second < 0)
      return false;
    nyr_opt_ret(f, second);
    return nyr_opt_verify(f, "tv-smt-cfg-write-call");
  }
  int result = emit_tv_write_call_load(f, address, value, symbol, effects,
                                       store_before_call);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-write-call");
}

static bool build_tv_write_call_result(nyr_func_t *f, unsigned effects,
                                       bool store_before_call) {
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int value = load_local(f, 1);
  if (address < 0 || value < 0)
    return false;
  if (store_before_call && emit_scalar_store(f, address, value) < 0)
    return false;
  int result = emit_tv_call(f, address, "tv.write.read.result", effects, true);
  if (result < 0)
    return false;
  if (!store_before_call && emit_scalar_store(f, address, value) < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-write-call-result");
}

static bool checks_writable_call_memory_model(void) {
  const unsigned writes = NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY;
  const unsigned reads_and_writes =
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY;
  nyr_func_t write_before = {0}, write_after = {0}, write_cfg = {0},
              write_direct = {0}, result_write_before = {0},
              result_write_after = {0}, result_readwrite_before = {0},
              result_readwrite_after = {0};
  bool built = build_tv_write_call_load(&write_before, "tv.write.memory",
                                        writes, true, false) &&
               build_tv_write_call_load(&write_after, "tv.write.memory", writes,
                                        false, false) &&
               build_tv_write_call_load(&write_cfg, "tv.write.memory", writes,
                                        true, true) &&
               build_tv_write_call_load(&write_direct, "tv.write.memory",
                                        writes, true, false) &&
               build_tv_write_call_result(&result_write_before, writes, true) &&
               build_tv_write_call_result(&result_write_after, writes, false) &&
               build_tv_write_call_result(&result_readwrite_before,
                                          reads_and_writes, true) &&
               build_tv_write_call_result(&result_readwrite_after,
                                          reads_and_writes, false);
  char changed_memory_err[256] = {0}, cfg_err[256] = {0};
  char write_result_err[256] = {0}, readwrite_result_err[256] = {0};
  bool detects_changed_memory =
      built &&
      !nyr_tv_smt_equiv(&write_before, &write_after, changed_memory_err,
                         sizeof(changed_memory_err)) &&
      strstr(changed_memory_err, "counterexample") != NULL;
  bool cfg_call_congruent =
      built &&
      nyr_tv_smt_equiv(&write_cfg, &write_direct, cfg_err, sizeof(cfg_err));
  bool write_result_ignores_memory =
      built && nyr_tv_smt_equiv(&result_write_before, &result_write_after,
                                 write_result_err, sizeof(write_result_err));
  bool readwrite_result_observes_memory =
      built &&
      !nyr_tv_smt_equiv(&result_readwrite_before, &result_readwrite_after,
                         readwrite_result_err, sizeof(readwrite_result_err)) &&
      strstr(readwrite_result_err, "counterexample") != NULL;
  if (!built || !detects_changed_memory || !cfg_call_congruent ||
      !write_result_ignores_memory || !readwrite_result_observes_memory)
    fprintf(
        stderr,
        "[tv-smt-write-call] built=%s changed-memory=%s cfg=%s "
        "write-result-no-read=%s readwrite-result=%s errors=%s/%s/%s/%s\n",
        built ? "yes" : "no", detects_changed_memory ? "rejected" : "missed",
        cfg_call_congruent ? "equivalent" : "failed",
        write_result_ignores_memory ? "equivalent" : "failed",
        readwrite_result_observes_memory ? "rejected" : "missed",
        changed_memory_err, cfg_err, write_result_err, readwrite_result_err);
  nyr_ir_func_free(&write_before);
  nyr_ir_func_free(&write_after);
  nyr_ir_func_free(&write_cfg);
  nyr_ir_func_free(&write_direct);
  nyr_ir_func_free(&result_write_before);
  nyr_ir_func_free(&result_write_after);
  nyr_ir_func_free(&result_readwrite_before);
  nyr_ir_func_free(&result_readwrite_after);
  return built && detects_changed_memory && cfg_call_congruent &&
         write_result_ignores_memory && readwrite_result_observes_memory;
}

static bool checks_write_may_trap_call_memory_model(void) {
  const unsigned writes =
      NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP;
  const unsigned reads_and_writes = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                    NYIR_EFFECT_WRITE_MEMORY |
                                    NYIR_EFFECT_MAY_TRAP;
  nyr_func_t write_before = {0}, write_after = {0}, write_cfg = {0},
              write_direct = {0}, readwrite_before = {0}, readwrite_after = {0};
  bool built =
      build_tv_write_call_load(&write_before, "tv.write.memory.trap", writes,
                               true, false) &&
      build_tv_write_call_load(&write_after, "tv.write.memory.trap", writes,
                               false, false) &&
      build_tv_write_call_load(&write_cfg, "tv.write.memory.trap", writes, true,
                               true) &&
      build_tv_write_call_load(&write_direct, "tv.write.memory.trap", writes,
                               true, false) &&
      build_tv_write_call_load(&readwrite_before, "tv.read.write.memory.trap",
                               reads_and_writes, true, false) &&
      build_tv_write_call_load(&readwrite_after, "tv.read.write.memory.trap",
                               reads_and_writes, false, false);
  char write_memory_err[256] = {0}, cfg_err[256] = {0};
  char readwrite_memory_err[256] = {0};
  bool detects_write_memory_change =
      built &&
      !nyr_tv_smt_equiv(&write_before, &write_after, write_memory_err,
                         sizeof(write_memory_err)) &&
      strstr(write_memory_err, "counterexample") != NULL;
  bool cfg_call_congruent =
      built &&
      nyr_tv_smt_equiv(&write_cfg, &write_direct, cfg_err, sizeof(cfg_err));
  bool detects_readwrite_memory_change =
      built &&
      !nyr_tv_smt_equiv(&readwrite_before, &readwrite_after,
                         readwrite_memory_err, sizeof(readwrite_memory_err)) &&
      strstr(readwrite_memory_err, "counterexample") != NULL;
  if (!built || !detects_write_memory_change || !cfg_call_congruent ||
      !detects_readwrite_memory_change)
    fprintf(stderr,
            "[tv-smt-write-trap-call] built=%s write-memory=%s cfg=%s "
            "readwrite-memory=%s errors=%s/%s/%s\n",
            built ? "yes" : "no",
            detects_write_memory_change ? "rejected" : "missed",
            cfg_call_congruent ? "equivalent" : "failed",
            detects_readwrite_memory_change ? "rejected" : "missed",
            write_memory_err, cfg_err, readwrite_memory_err);
  nyr_ir_func_free(&write_before);
  nyr_ir_func_free(&write_after);
  nyr_ir_func_free(&write_cfg);
  nyr_ir_func_free(&write_direct);
  nyr_ir_func_free(&readwrite_before);
  nyr_ir_func_free(&readwrite_after);
  return built && detects_write_memory_change && cfg_call_congruent &&
         detects_readwrite_memory_change;
}

static bool build_tv_discarded_call(nyr_func_t *f, unsigned effects,
                                    bool emit_call, bool repeat_call,
                                    bool with_branch) {
  nyr_opt_begin(f);
  int argument = load_local(f, 0);
  if (argument < 0)
    return false;
  if (with_branch) {
    int condition = load_local(f, 1);
    int zero = nyr_opt_const(f, 0);
    if (condition < 0 || zero < 0)
      return false;
    int is_zero = nyr_opt_cmp(f, condition, zero, NYIR_CMP_EQ);
    if (is_zero < 0)
      return false;
    nyr_opt_br_if(f, is_zero, 1);
  }
  if (emit_call && emit_tv_call(f, argument, "tv.may.trap", effects, true) < 0)
    return false;
  if (repeat_call &&
      emit_tv_call(f, argument, "tv.may.trap", effects, true) < 0)
    return false;
  nyr_opt_ret(f, argument);
  if (with_branch) {
    nyr_opt_label(f, 1);
    nyr_opt_ret(f, argument);
  }
  return nyr_opt_verify(f, with_branch ? "tv-smt-cfg-may-trap-call"
                                       : "tv-smt-may-trap-call");
}

static bool checks_may_trap_call_definedness(void) {
  const unsigned may_trap = NYIR_EFFECT_CALL | NYIR_EFFECT_MAY_TRAP;
  nyr_func_t repeated = {0}, single = {0}, no_call = {0};
  nyr_func_t cfg_repeated = {0}, cfg_single = {0}, cfg_no_call = {0};
  nyr_func_t conflicting_effect = {0};
  bool built =
      build_tv_discarded_call(&repeated, may_trap, true, true, false) &&
      build_tv_discarded_call(&single, may_trap, true, false, false) &&
      build_tv_discarded_call(&no_call, may_trap, false, false, false) &&
      build_tv_discarded_call(&cfg_repeated, may_trap, true, true, true) &&
      build_tv_discarded_call(&cfg_single, may_trap, true, false, true) &&
      build_tv_discarded_call(&cfg_no_call, may_trap, false, false, true) &&
      build_tv_discarded_call(&conflicting_effect, NYIR_EFFECT_CALL, true,
                              false, false);
  char repeat_err[256] = {0}, direct_erase_err[256] = {0};
  char cfg_repeat_err[256] = {0}, cfg_erase_err[256] = {0};
  char conflict_err[256] = {0};
  bool repeated_equivalent =
      built &&
      nyr_tv_smt_equiv(&repeated, &single, repeat_err, sizeof(repeat_err));
  bool rejects_call_erasure =
      built &&
      !nyr_tv_smt_equiv(&single, &no_call, direct_erase_err,
                         sizeof(direct_erase_err)) &&
      strstr(direct_erase_err, "counterexample") != NULL;
  bool cfg_repeated_equivalent =
      built && nyr_tv_smt_equiv(&cfg_repeated, &cfg_single, cfg_repeat_err,
                                 sizeof(cfg_repeat_err));
  bool cfg_rejects_call_erasure =
      built &&
      !nyr_tv_smt_equiv(&cfg_single, &cfg_no_call, cfg_erase_err,
                         sizeof(cfg_erase_err)) &&
      strstr(cfg_erase_err, "counterexample") != NULL;
  bool rejects_conflicting_effect =
      built &&
      !nyr_tv_smt_equiv(&single, &conflicting_effect, conflict_err,
                         sizeof(conflict_err)) &&
      strstr(conflict_err, "signature conflict") != NULL &&
      strstr(conflict_err, "counterexample") == NULL;
  if (!built || !repeated_equivalent || !rejects_call_erasure ||
      !cfg_repeated_equivalent || !cfg_rejects_call_erasure ||
      !rejects_conflicting_effect)
    fprintf(stderr,
            "[tv-smt-may-trap-call] built=%s repeated=%s erase=%s "
            "cfg-repeat=%s cfg-erase=%s conflict=%s errors=%s/%s/%s/%s/%s\n",
            built ? "yes" : "no", repeated_equivalent ? "equivalent" : "failed",
            rejects_call_erasure ? "rejected" : "accepted",
            cfg_repeated_equivalent ? "equivalent" : "failed",
            cfg_rejects_call_erasure ? "rejected" : "accepted",
            rejects_conflicting_effect ? "inapplicable" : "modeled", repeat_err,
            direct_erase_err, cfg_repeat_err, cfg_erase_err, conflict_err);
  nyr_ir_func_free(&repeated);
  nyr_ir_func_free(&single);
  nyr_ir_func_free(&no_call);
  nyr_ir_func_free(&cfg_repeated);
  nyr_ir_func_free(&cfg_single);
  nyr_ir_func_free(&cfg_no_call);
  nyr_ir_func_free(&conflicting_effect);
  return built && repeated_equivalent && rejects_call_erasure &&
         cfg_repeated_equivalent && cfg_rejects_call_erasure &&
         rejects_conflicting_effect;
}

static bool build_tv_memory_trap_order(nyr_func_t *f, bool store_before_call,
                                       bool with_branch) {
  const unsigned effects =
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP;
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int value = nyr_opt_const(f, 17);
  if (address < 0 || value < 0)
    return false;
  if (with_branch) {
    int condition = load_local(f, 1);
    int zero = nyr_opt_const(f, 0);
    int is_zero = condition < 0 || zero < 0
                      ? -1
                      : nyr_opt_cmp(f, condition, zero, NYIR_CMP_EQ);
    if (is_zero < 0)
      return false;
    nyr_opt_br_if(f, is_zero, 1);
  }
  if (store_before_call && emit_scalar_store(f, address, value) < 0)
    return false;
  if (emit_tv_call(f, address, "tv.read.trap.memory", effects, true) < 0)
    return false;
  if (!store_before_call && emit_scalar_store(f, address, value) < 0)
    return false;
  nyr_opt_ret(f, address);
  if (with_branch) {
    nyr_opt_label(f, 1);
    if (store_before_call && emit_scalar_store(f, address, value) < 0)
      return false;
    if (emit_tv_call(f, address, "tv.read.trap.memory", effects, true) < 0)
      return false;
    if (!store_before_call && emit_scalar_store(f, address, value) < 0)
      return false;
    nyr_opt_ret(f, address);
  }
  return nyr_opt_verify(f, with_branch ? "tv-smt-cfg-read-trap-memory"
                                       : "tv-smt-read-trap-memory");
}

static bool checks_read_memory_may_trap_calls(void) {
  const unsigned reads_may_trap =
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP;
  const unsigned reads = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY;
  nyr_func_t repeated = {0}, single = {0}, no_call = {0};
  nyr_func_t cfg_repeated = {0}, cfg_single = {0}, cfg_no_call = {0};
  nyr_func_t conflicting_effect = {0}, memory_before = {0}, memory_after = {0};
  nyr_func_t cfg_memory_before = {0}, cfg_memory_after = {0};
  bool built =
      build_tv_discarded_call(&repeated, reads_may_trap, true, true, false) &&
      build_tv_discarded_call(&single, reads_may_trap, true, false, false) &&
      build_tv_discarded_call(&no_call, reads_may_trap, false, false, false) &&
      build_tv_discarded_call(&cfg_repeated, reads_may_trap, true, true,
                              true) &&
      build_tv_discarded_call(&cfg_single, reads_may_trap, true, false, true) &&
      build_tv_discarded_call(&cfg_no_call, reads_may_trap, false, false,
                              true) &&
      build_tv_discarded_call(&conflicting_effect, reads, true, false, false) &&
      build_tv_memory_trap_order(&memory_before, true, false) &&
      build_tv_memory_trap_order(&memory_after, false, false) &&
      build_tv_memory_trap_order(&cfg_memory_before, true, true) &&
      build_tv_memory_trap_order(&cfg_memory_after, false, true);
  char repeat_err[256] = {0}, erase_err[256] = {0};
  char cfg_repeat_err[256] = {0}, cfg_erase_err[256] = {0};
  char conflict_err[256] = {0}, memory_err[256] = {0};
  char cfg_memory_err[256] = {0};
  bool repeated_equivalent =
      built &&
      nyr_tv_smt_equiv(&repeated, &single, repeat_err, sizeof(repeat_err));
  bool rejects_call_erasure =
      built &&
      !nyr_tv_smt_equiv(&single, &no_call, erase_err, sizeof(erase_err)) &&
      strstr(erase_err, "counterexample") != NULL;
  bool cfg_repeated_equivalent =
      built && nyr_tv_smt_equiv(&cfg_repeated, &cfg_single, cfg_repeat_err,
                                 sizeof(cfg_repeat_err));
  bool cfg_rejects_call_erasure =
      built &&
      !nyr_tv_smt_equiv(&cfg_single, &cfg_no_call, cfg_erase_err,
                         sizeof(cfg_erase_err)) &&
      strstr(cfg_erase_err, "counterexample") != NULL;
  bool rejects_conflicting_effect =
      built &&
      !nyr_tv_smt_equiv(&single, &conflicting_effect, conflict_err,
                         sizeof(conflict_err)) &&
      strstr(conflict_err, "signature conflict") != NULL &&
      strstr(conflict_err, "counterexample") == NULL;
  bool detects_memory_sensitive_validity =
      built &&
      !nyr_tv_smt_equiv(&memory_before, &memory_after, memory_err,
                         sizeof(memory_err)) &&
      strstr(memory_err, "counterexample") != NULL;
  bool detects_cfg_memory_sensitive_validity =
      built &&
      !nyr_tv_smt_equiv(&cfg_memory_before, &cfg_memory_after, cfg_memory_err,
                         sizeof(cfg_memory_err)) &&
      strstr(cfg_memory_err, "counterexample") != NULL;
  if (!built || !repeated_equivalent || !rejects_call_erasure ||
      !cfg_repeated_equivalent || !cfg_rejects_call_erasure ||
      !rejects_conflicting_effect || !detects_memory_sensitive_validity ||
      !detects_cfg_memory_sensitive_validity)
    fprintf(stderr,
            "[tv-smt-read-trap-call] built=%s repeated=%s erase=%s "
            "cfg-repeat=%s cfg-erase=%s conflict=%s memory-key=%s "
            "cfg-memory-key=%s errors=%s/%s/%s/%s/%s/%s/%s\n",
            built ? "yes" : "no", repeated_equivalent ? "equivalent" : "failed",
            rejects_call_erasure ? "rejected" : "accepted",
            cfg_repeated_equivalent ? "equivalent" : "failed",
            cfg_rejects_call_erasure ? "rejected" : "accepted",
            rejects_conflicting_effect ? "inapplicable" : "modeled",
            detects_memory_sensitive_validity ? "sensitive" : "ignored",
            detects_cfg_memory_sensitive_validity ? "sensitive" : "ignored",
            repeat_err, erase_err, cfg_repeat_err, cfg_erase_err, conflict_err,
            memory_err, cfg_memory_err);
  nyr_ir_func_free(&repeated);
  nyr_ir_func_free(&single);
  nyr_ir_func_free(&no_call);
  nyr_ir_func_free(&cfg_repeated);
  nyr_ir_func_free(&cfg_single);
  nyr_ir_func_free(&cfg_no_call);
  nyr_ir_func_free(&conflicting_effect);
  nyr_ir_func_free(&memory_before);
  nyr_ir_func_free(&memory_after);
  nyr_ir_func_free(&cfg_memory_before);
  nyr_ir_func_free(&cfg_memory_after);
  return built && repeated_equivalent && rejects_call_erasure &&
         cfg_repeated_equivalent && cfg_rejects_call_erasure &&
         rejects_conflicting_effect && detects_memory_sensitive_validity &&
         detects_cfg_memory_sensitive_validity;
}

static bool build_tv_capture_result(nyr_func_t *f, const char *symbol,
                                    int64_t selector, bool repeat_capture,
                                    bool increment_arg, bool with_branch) {
  nyr_opt_begin(f);
  int argument = load_local(f, 0);
  if (argument < 0)
    return false;
  if (increment_arg) {
    int one = nyr_opt_const(f, 1);
    if (one < 0 ||
        (argument = nyr_opt_binop(f, NYIR_ADD_I64, argument, one)) < 0)
      return false;
  }
  if (with_branch) {
    int condition = load_local(f, 1);
    int zero = nyr_opt_const(f, 0);
    if (condition < 0 || zero < 0)
      return false;
    nyr_opt_br_if(f, condition, 1);
    nyr_opt_ret(f, zero);
    nyr_opt_label(f, 1);
  }
  if (emit_tv_call(f, argument, symbol, NYIR_EFFECT_CALL, true) < 0)
    return false;
  int result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = selector,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
  if (result < 0)
    return false;
  if (repeat_capture) {
    result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = selector,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
    if (result < 0)
      return false;
  }
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, with_branch ? "tv-smt-cfg-capture-ret"
                                       : "tv-smt-capture-ret");
}

static bool checks_capture_ret_model(void) {
  nyr_func_t repeated = {0}, single = {0}, changed_selector = {0},
              changed_arg = {0}, cfg_repeated = {0}, cfg_single = {0};
  bool built = build_tv_capture_result(&repeated, "tv.pure.pair", 0, true,
                                       false, false) &&
               build_tv_capture_result(&single, "tv.pure.pair", 0, false, false,
                                       false) &&
               build_tv_capture_result(&changed_selector, "tv.pure.pair", 1,
                                       false, false, false) &&
               build_tv_capture_result(&changed_arg, "tv.pure.pair", 0, false,
                                       true, false) &&
               build_tv_capture_result(&cfg_repeated, "tv.pure.pair", 0, true,
                                       false, true) &&
               build_tv_capture_result(&cfg_single, "tv.pure.pair", 0, false,
                                       false, true);
  char repeated_err[256] = {0}, selector_err[256] = {0};
  char argument_err[256] = {0}, cfg_err[256] = {0};
  bool repeated_capture_equivalent =
      built &&
      nyr_tv_smt_equiv(&repeated, &single, repeated_err, sizeof(repeated_err));
  bool rejects_selector_change =
      built &&
      !nyr_tv_smt_equiv(&single, &changed_selector, selector_err,
                         sizeof(selector_err)) &&
      strstr(selector_err, "counterexample") != NULL;
  bool rejects_argument_change =
      built &&
      !nyr_tv_smt_equiv(&single, &changed_arg, argument_err,
                         sizeof(argument_err)) &&
      strstr(argument_err, "counterexample") != NULL;
  bool cfg_capture_equivalent =
      built &&
      nyr_tv_smt_equiv(&cfg_repeated, &cfg_single, cfg_err, sizeof(cfg_err));
  if (!built || !repeated_capture_equivalent || !rejects_selector_change ||
      !rejects_argument_change || !cfg_capture_equivalent)
    fprintf(stderr,
            "[tv-smt-capture-ret] built=%s repeat=%s selector=%s arg=%s "
            "cfg=%s errors=%s/%s/%s/%s\n",
            built ? "yes" : "no",
            repeated_capture_equivalent ? "equivalent" : "failed",
            rejects_selector_change ? "rejected" : "accepted",
            rejects_argument_change ? "rejected" : "accepted",
            cfg_capture_equivalent ? "equivalent" : "failed", repeated_err,
            selector_err, argument_err, cfg_err);
  nyr_ir_func_free(&repeated);
  nyr_ir_func_free(&single);
  nyr_ir_func_free(&changed_selector);
  nyr_ir_func_free(&changed_arg);
  nyr_ir_func_free(&cfg_repeated);
  nyr_ir_func_free(&cfg_single);
  return built && repeated_capture_equivalent && rejects_selector_change &&
         rejects_argument_change && cfg_capture_equivalent;
}

static bool keeps_volatile_calls_inapplicable(void) {
  nyr_func_t write_may_trap = {0}, read_write_may_trap = {0};
  bool built =
      build_tv_call_result(&write_may_trap, "tv.effectful.write.volatile",
                           NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY |
                               NYIR_EFFECT_VOLATILE,
                           true, false, false) &&
      build_tv_call_result(&read_write_may_trap,
                           "tv.effectful.read.write.trap.volatile",
                           NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               NYIR_EFFECT_VOLATILE,
                           true, false, false);
  char write_trap_err[256] = {0}, read_write_trap_err[256] = {0};
  bool write_trap_inapplicable =
      built &&
      !nyr_tv_smt_equiv(&write_may_trap, &write_may_trap, write_trap_err,
                         sizeof(write_trap_err)) &&
      write_trap_err[0] == '\0';
  bool read_write_trap_inapplicable =
      built &&
      !nyr_tv_smt_equiv(&read_write_may_trap, &read_write_may_trap,
                         read_write_trap_err, sizeof(read_write_trap_err)) &&
      read_write_trap_err[0] == '\0';
  if (!built || !write_trap_inapplicable || !read_write_trap_inapplicable)
    fprintf(stderr,
            "[tv-smt-effectful-call] built=%s write-volatile-inapplicable=%s "
            "read-write-volatile-inapplicable=%s: %s\n",
            built ? "yes" : "no", write_trap_inapplicable ? "yes" : "no",
            read_write_trap_inapplicable ? "yes" : "no",
            write_trap_err[0]        ? write_trap_err
            : read_write_trap_err[0] ? read_write_trap_err
                                     : "volatile call was modeled");
  nyr_ir_func_free(&write_may_trap);
  nyr_ir_func_free(&read_write_may_trap);
  return built && write_trap_inapplicable && read_write_trap_inapplicable;
}

static bool build_tv_readonly_memory_call_offset(nyr_func_t *f,
                                                 bool offset_pointer) {
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  if (address < 0)
    return false;
  if (offset_pointer) {
    int offset = nyr_opt_const(f, 8);
    if (offset < 0 ||
        (address = nyr_opt_binop(f, NYIR_ADD_I64, address, offset)) < 0)
      return false;
  }
  int result = emit_tv_call(f, address, "tv.read.memory.pointer",
                            NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-readonly-memory-pointer");
}

static bool rejects_conflicting_call_effect_summaries(void) {
  nyr_func_t pure = {0}, read_memory = {0};
  bool built = build_tv_call_result(&pure, "tv.effect.summary",
                                    NYIR_EFFECT_CALL, true, false, false) &&
               build_tv_call_result(&read_memory, "tv.effect.summary",
                                    NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY,
                                    true, false, false);
  char err[256] = {0};
  bool inapplicable =
      built && !nyr_tv_smt_equiv(&pure, &read_memory, err, sizeof(err)) &&
      strstr(err, "signature conflict") != NULL &&
      strstr(err, "counterexample") == NULL;
  if (!built || !inapplicable)
    fprintf(stderr,
            "[tv-smt-call-effect-conflict] built=%s inapplicable=%s: %s\n",
            built ? "yes" : "no", inapplicable ? "yes" : "no",
            err[0] ? err : "inconsistent callee effects were modeled");
  nyr_ir_func_free(&pure);
  nyr_ir_func_free(&read_memory);
  return built && inapplicable;
}

static bool build_tv_readonly_memory_call(nyr_func_t *f, bool write_before,
                                          bool write_after, bool repeat_call) {
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int value = load_local(f, 1);
  if (address < 0 || value < 0)
    return false;
  if (write_before) {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    if (f->len == old_len)
      return false;
  }
  int result = emit_tv_call(f, address, "tv.read.memory",
                            NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true);
  if (result < 0)
    return false;
  if (repeat_call &&
      (result =
           emit_tv_call(f, address, "tv.read.memory",
                        NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true)) < 0)
    return false;
  if (write_after) {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    if (f->len == old_len)
      return false;
  }
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-readonly-memory-call");
}

static bool build_tv_readonly_memory_capture(nyr_func_t *f, bool write_before,
                                             bool write_after,
                                             bool repeat_capture) {
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int value = load_local(f, 1);
  if (address < 0 || value < 0)
    return false;
  if (write_before) {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    if (f->len == old_len)
      return false;
  }
  if (emit_tv_call(f, address, "tv.read.memory.capture",
                   NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true) < 0)
    return false;
  int result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = 0,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
  if (result < 0)
    return false;
  if (repeat_capture &&
      (result = nyr_ir_emit(
           f, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                            .dst = -1,
                            .a = -1,
                            .b = -1,
                            .c = -1,
                            .d = -1,
                            .e = -1,
                            .f = -1,
                            .imm = 0,
                            .scalar_class = NYIR_SCALAR_I64_RAW})) < 0)
    return false;
  if (write_after) {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    if (f->len == old_len)
      return false;
  }
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-readonly-memory-capture");
}

static bool build_cfg_tv_readonly_memory_call(nyr_func_t *f,
                                              bool write_before_call,
                                              bool repeat_call) {
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int value = load_local(f, 1);
  int condition = load_local(f, 2);
  if (address < 0 || value < 0 || condition < 0)
    return false;
  nyr_opt_br_if(f, condition, 1);
  int result = emit_tv_call(f, address, "tv.read.memory",
                            NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true);
  if (result < 0)
    return false;
  if (repeat_call &&
      (result =
           emit_tv_call(f, address, "tv.read.memory",
                        NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true)) < 0)
    return false;
  nyr_opt_ret(f, result);
  nyr_opt_label(f, 1);
  if (write_before_call) {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    if (f->len == old_len)
      return false;
  }
  result = emit_tv_call(f, address, "tv.read.memory",
                        NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true);
  if (result < 0)
    return false;
  if (repeat_call &&
      (result =
           emit_tv_call(f, address, "tv.read.memory",
                        NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true)) < 0)
    return false;
  if (!write_before_call) {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    if (f->len == old_len)
      return false;
  }
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-cfg-readonly-memory-call");
}

static bool build_cfg_tv_readonly_memory_capture(nyr_func_t *f,
                                                 bool write_before_call,
                                                 bool repeat_capture) {
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int value = load_local(f, 1);
  int condition = load_local(f, 2);
  if (address < 0 || value < 0 || condition < 0)
    return false;
  nyr_opt_br_if(f, condition, 1);
  if (emit_tv_call(f, address, "tv.read.memory.capture",
                   NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true) < 0)
    return false;
  int result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = 0,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
  if (result < 0)
    return false;
  if (repeat_capture &&
      (result = nyr_ir_emit(
           f, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                            .dst = -1,
                            .a = -1,
                            .b = -1,
                            .c = -1,
                            .d = -1,
                            .e = -1,
                            .f = -1,
                            .imm = 0,
                            .scalar_class = NYIR_SCALAR_I64_RAW})) < 0)
    return false;
  nyr_opt_ret(f, result);
  nyr_opt_label(f, 1);
  if (write_before_call) {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    if (f->len == old_len)
      return false;
  }
  if (emit_tv_call(f, address, "tv.read.memory.capture",
                   NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY, true) < 0)
    return false;
  result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                      .dst = -1,
                                      .a = -1,
                                      .b = -1,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1,
                                      .imm = 0,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
  if (result < 0)
    return false;
  if (repeat_capture &&
      (result = nyr_ir_emit(
           f, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                            .dst = -1,
                            .a = -1,
                            .b = -1,
                            .c = -1,
                            .d = -1,
                            .e = -1,
                            .f = -1,
                            .imm = 0,
                            .scalar_class = NYIR_SCALAR_I64_RAW})) < 0)
    return false;
  if (!write_before_call) {
    size_t old_len = f->len;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
    if (f->len == old_len)
      return false;
  }
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-cfg-readonly-memory-capture");
}

static bool checks_readonly_call_memory_congruence(void) {
  nyr_func_t repeated = {0}, single = {0}, write_before = {0},
              write_after = {0}, cfg_repeated = {0}, cfg_single = {0},
              cfg_write_after = {0}, capture_repeated = {0},
              capture_single = {0}, capture_write_before = {0},
              capture_write_after = {0}, pointer_same = {0},
              pointer_offset = {0}, cfg_capture_repeated = {0},
              cfg_capture_single = {0}, cfg_capture_write_after = {0};
  bool built =
      build_tv_readonly_memory_call(&repeated, false, false, true) &&
      build_tv_readonly_memory_call(&single, false, false, false) &&
      build_tv_readonly_memory_call(&write_before, true, false, false) &&
      build_tv_readonly_memory_call(&write_after, false, true, false) &&
      build_cfg_tv_readonly_memory_call(&cfg_repeated, true, true) &&
      build_cfg_tv_readonly_memory_call(&cfg_single, true, false) &&
      build_cfg_tv_readonly_memory_call(&cfg_write_after, false, false) &&
      build_tv_readonly_memory_capture(&capture_repeated, false, false, true) &&
      build_tv_readonly_memory_capture(&capture_single, false, false, false) &&
      build_tv_readonly_memory_capture(&capture_write_before, true, false,
                                       false) &&
      build_tv_readonly_memory_capture(&capture_write_after, false, true,
                                       false) &&
      build_cfg_tv_readonly_memory_capture(&cfg_capture_repeated, true, true) &&
      build_cfg_tv_readonly_memory_capture(&cfg_capture_single, true, false) &&
      build_cfg_tv_readonly_memory_capture(&cfg_capture_write_after, false,
                                           false) &&
      build_tv_readonly_memory_call_offset(&pointer_same, false) &&
      build_tv_readonly_memory_call_offset(&pointer_offset, true);
  char err[256] = {0};
  bool duplicate_equivalent =
      built && nyr_tv_smt_equiv(&repeated, &single, err, sizeof(err));
  err[0] = '\0';
  bool memory_change_rejected =
      built &&
      !nyr_tv_smt_equiv(&write_before, &write_after, err, sizeof(err)) &&
      strstr(err, "counterexample") != NULL;
  err[0] = '\0';
  bool cfg_duplicate_equivalent =
      built && nyr_tv_smt_equiv(&cfg_repeated, &cfg_single, err, sizeof(err));
  err[0] = '\0';
  bool cfg_memory_change_rejected =
      built &&
      !nyr_tv_smt_equiv(&cfg_repeated, &cfg_write_after, err, sizeof(err)) &&
      strstr(err, "counterexample") != NULL;
  err[0] = '\0';
  bool capture_equivalent =
      built &&
      nyr_tv_smt_equiv(&capture_repeated, &capture_single, err, sizeof(err));
  err[0] = '\0';
  bool capture_memory_change_rejected =
      built &&
      !nyr_tv_smt_equiv(&capture_write_before, &capture_write_after, err,
                         sizeof(err)) &&
      strstr(err, "counterexample") != NULL;
  err[0] = '\0';
  bool pointer_change_rejected =
      built &&
      !nyr_tv_smt_equiv(&pointer_same, &pointer_offset, err, sizeof(err)) &&
      strstr(err, "counterexample") != NULL;
  err[0] = '\0';
  bool cfg_capture_equivalent =
      built && nyr_tv_smt_equiv(&cfg_capture_repeated, &cfg_capture_single,
                                 err, sizeof(err));
  err[0] = '\0';
  bool cfg_capture_memory_change_rejected =
      built &&
      !nyr_tv_smt_equiv(&cfg_capture_repeated, &cfg_capture_write_after, err,
                         sizeof(err)) &&
      strstr(err, "counterexample") != NULL;
  if (!built || !duplicate_equivalent || !memory_change_rejected ||
      !cfg_duplicate_equivalent || !cfg_memory_change_rejected ||
      !capture_equivalent || !capture_memory_change_rejected ||
      !pointer_change_rejected || !cfg_capture_equivalent ||
      !cfg_capture_memory_change_rejected)
    fprintf(stderr,
            "[tv-smt-readonly-call] built=%s duplicate=%s changed-memory=%s "
            "cfg-duplicate=%s cfg-changed-memory=%s captured=%s "
            "captured-changed-memory=%s changed-pointer=%s "
            "cfg-captured=%s cfg-captured-changed-memory=%s: %s\n",
            built ? "yes" : "no", duplicate_equivalent ? "proved" : "failed",
            memory_change_rejected ? "rejected" : "missed",
            cfg_duplicate_equivalent ? "proved" : "failed",
            cfg_memory_change_rejected ? "rejected" : "missed",
            capture_equivalent ? "proved" : "failed",
            capture_memory_change_rejected ? "rejected" : "missed",
            pointer_change_rejected ? "rejected" : "missed",
            cfg_capture_equivalent ? "proved" : "failed",
            cfg_capture_memory_change_rejected ? "rejected" : "missed",
            err[0] ? err : "read-only call memory congruence mismatch");
  nyr_ir_func_free(&repeated);
  nyr_ir_func_free(&single);
  nyr_ir_func_free(&write_before);
  nyr_ir_func_free(&write_after);
  nyr_ir_func_free(&cfg_repeated);
  nyr_ir_func_free(&cfg_single);
  nyr_ir_func_free(&cfg_write_after);
  nyr_ir_func_free(&capture_repeated);
  nyr_ir_func_free(&capture_single);
  nyr_ir_func_free(&capture_write_before);
  nyr_ir_func_free(&capture_write_after);
  nyr_ir_func_free(&pointer_same);
  nyr_ir_func_free(&pointer_offset);
  nyr_ir_func_free(&cfg_capture_repeated);
  nyr_ir_func_free(&cfg_capture_single);
  nyr_ir_func_free(&cfg_capture_write_after);
  return built && duplicate_equivalent && memory_change_rejected &&
         cfg_duplicate_equivalent && cfg_memory_change_rejected &&
         capture_equivalent && capture_memory_change_rejected &&
         pointer_change_rejected && cfg_capture_equivalent &&
         cfg_capture_memory_change_rejected;
}

static int emit_f64_stack_array(nyr_func_t *f, const double values[4]);

static bool build_memory_f64_vec4_sum(nyr_func_t *f, bool vectorized,
                                      bool omit_last_lane, bool use_alloca) {
  nyr_opt_begin(f);
  static const double values[4] = {1.0, 2.0, 4.0, 8.0};
  int address = use_alloca ? emit_f64_stack_array(f, values) : load_local(f, 0);
  if (address < 0)
    return false;
  int sum = -1;
  if (vectorized) {
    int vector = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                            .dst = -1,
                                            .a = address,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
    int zero = nyr_opt_const_f64(f, 0.0);
    sum = vector < 0 || zero < 0
              ? -1
              : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_F64,
                                           .dst = -1,
                                           .a = zero,
                                           .b = vector,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  } else {
    int zero = nyr_opt_const_f64(f, 0.0);
    sum = zero;
    for (unsigned lane = 0; lane < 4 && sum >= 0; ++lane) {
      if (omit_last_lane && lane == 3)
        continue;
      int offset = nyr_opt_const(f, (int64_t)lane * 8);
      int lane_address = lane == 0 ? address
                         : offset < 0
                             ? -1
                             : nyr_opt_binop(f, NYIR_ADD_I64, address, offset);
      int value = lane_address < 0 ? -1 : emit_load_i64(f, lane_address);
      sum = value < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, sum, value);
    }
  }
  int result = emit_f64_to_i64(f, sum);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-memory-f64-vec4");
}

static bool checks_memory_f64_vec4_semantics(void) {
  nyr_func_t vector = {0}, scalar = {0}, bounded_vector = {0}, wrong = {0};
  bool built = build_memory_f64_vec4_sum(&vector, true, false, false) &&
               build_memory_f64_vec4_sum(&scalar, false, false, false) &&
               build_memory_f64_vec4_sum(&bounded_vector, true, false, true) &&
               build_memory_f64_vec4_sum(&wrong, false, true, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
  err[0] = '\0';
  /*
   * Use fully valid, distinct stack lanes for the negative witness. The
   * symbolic external-pointer pair above covers arbitrary addresses; this
   * pair isolates the omitted lane's value difference from pointer traps.
   */
  bool rejects_missing_lane =
      built && !nyr_tv_smt_equiv(&bounded_vector, &wrong, err, sizeof(err));
  if (!built || !equivalent || !rejects_missing_lane)
    fprintf(stderr,
            "[tv-smt-memory-f64-vec4] built=%s equivalent=%s rejects=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_missing_lane ? "yes" : "no",
            err[0] ? err : "four-lane f64 memory mismatch");
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&bounded_vector);
  nyr_ir_func_free(&wrong);
  return built && equivalent && rejects_missing_lane;
}

static bool emit_f64_vector_store_copy(nyr_func_t *f, int source,
                                       int destination, bool vectorized,
                                       bool omit_last_store) {
  if (vectorized) {
    int packed = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                            .dst = -1,
                                            .a = source,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
    return packed >= 0 && nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_STORE_F64,
                                                     .dst = -1,
                                                     .a = destination,
                                                     .b = packed,
                                                     .c = -1,
                                                     .d = -1,
                                                     .e = -1,
                                                     .f = -1}) >= 0;
  }

  int values[4] = {-1, -1, -1, -1};
  for (unsigned lane = 0; lane < 4; ++lane) {
    int offset = nyr_opt_const(f, (int64_t)lane * 8);
    int address = lane == 0    ? source
                  : offset < 0 ? -1
                               : nyr_opt_binop(f, NYIR_ADD_I64, source, offset);
    values[lane] = address < 0 ? -1 : emit_load_i64(f, address);
    if (values[lane] < 0)
      return false;
  }
  for (unsigned lane = 0; lane < 4; ++lane) {
    if (omit_last_store && lane == 3)
      continue;
    int offset = nyr_opt_const(f, (int64_t)lane * 8);
    int address = lane == 0 ? destination
                  : offset < 0
                      ? -1
                      : nyr_opt_binop(f, NYIR_ADD_I64, destination, offset);
    if (emit_scalar_store(f, address, values[lane]) < 0)
      return false;
  }
  return true;
}

static int emit_f64_vector_store_sum(nyr_func_t *f, int destination,
                                     bool vectorized) {
  if (vectorized) {
    int packed = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                            .dst = -1,
                                            .a = destination,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
    int zero = nyr_opt_const_f64(f, 0.0);
    return packed < 0 || zero < 0
               ? -1
               : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_F64,
                                            .dst = -1,
                                            .a = zero,
                                            .b = packed,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  }

  int sum = nyr_opt_const_f64(f, 0.0);
  for (unsigned lane = 0; lane < 4; ++lane) {
    int offset = nyr_opt_const(f, (int64_t)lane * 8);
    int address = lane == 0 ? destination
                  : offset < 0
                      ? -1
                      : nyr_opt_binop(f, NYIR_ADD_I64, destination, offset);
    int value = address < 0 ? -1 : emit_load_i64(f, address);
    sum =
        sum < 0 || value < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, sum, value);
  }
  return sum;
}

static bool build_f64_vector_store_roundtrip(nyr_func_t *f, bool vectorized,
                                             bool omit_last_store, bool cfg) {
  nyr_opt_begin(f);
  int source = load_local(f, 0);
  int destination = load_local(f, 1);
  if (source < 0 || destination < 0)
    return false;

  if (cfg) {
    int condition = load_local(f, 2);
    int zero = nyr_opt_const(f, 0);
    int branch = condition < 0 || zero < 0
                     ? -1
                     : nyr_opt_cmp(f, condition, zero, NYIR_CMP_NE);
    if (branch < 0)
      return false;
    nyr_opt_br_if(f, branch, 10);
    if (!emit_f64_vector_store_copy(f, source, destination, vectorized,
                                    omit_last_store))
      return false;
    nyr_opt_br(f, 20);
    nyr_opt_label(f, 10);
    if (!emit_f64_vector_store_copy(f, source, destination, vectorized,
                                    omit_last_store))
      return false;
    nyr_opt_label(f, 20);
  } else if (!emit_f64_vector_store_copy(f, source, destination, vectorized,
                                         omit_last_store)) {
    return false;
  }

  int sum = emit_f64_vector_store_sum(f, destination, vectorized);
  int result = sum < 0 ? -1 : emit_f64_to_i64(f, sum);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, cfg ? "tv-smt-cfg-f64-vector-store"
                               : "tv-smt-f64-vector-store");
}

static bool checks_f64_vector_store_semantics(bool cfg) {
  nyr_func_t vector = {0}, scalar = {0}, missing_lane = {0};
  bool built =
      build_f64_vector_store_roundtrip(&vector, true, false, cfg) &&
      build_f64_vector_store_roundtrip(&scalar, false, false, cfg) &&
      build_f64_vector_store_roundtrip(&missing_lane, false, true, cfg);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
  err[0] = '\0';
  bool rejects_missing_lane =
      built && !nyr_tv_smt_equiv(&vector, &missing_lane, err, sizeof(err));
  if (!built || !equivalent || !rejects_missing_lane)
    fprintf(stderr,
            "[tv-smt-%sf64-vector-store] built=%s equivalent=%s "
            "rejects-missing-lane=%s: %s\n",
            cfg ? "cfg-" : "", built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_missing_lane ? "yes" : "no",
            err[0] ? err : "floating vector store semantics mismatch");
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&missing_lane);
  return built && equivalent && rejects_missing_lane;
}

static bool build_f64_shuffle_lane(nyr_func_t *f, int64_t mask,
                                   unsigned output_lane, bool cfg) {
  nyr_opt_begin(f);
  if (cfg) {
    nyr_opt_br(f, 13);
    nyr_opt_label(f, 13);
  }
  int address = load_local(f, 0);
  int loaded = address < 0
                   ? -1
                   : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                                .dst = -1,
                                                .a = address,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
  int shuffled = loaded < 0
                     ? -1
                     : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SHUFFLE_F64,
                                                  .dst = -1,
                                                  .a = loaded,
                                                  .b = -1,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1,
                                                  .imm = mask});
  if (address < 0 || loaded < 0 || shuffled < 0 || output_lane > 3)
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_STORE_F64,
                                   .dst = -1,
                                   .a = address,
                                   .b = shuffled,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  int offset = nyr_opt_const(f, (int64_t)output_lane * 8);
  int selected_address =
      offset < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, address, offset);
  int result = selected_address < 0 ? -1 : emit_load_i64(f, selected_address);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f,
                        cfg ? "tv-smt-cfg-f64-shuffle" : "tv-smt-f64-shuffle");
}

static bool checks_f64_shuffle_semantics(void) {
  bool ok = true;
  for (unsigned cfg = 0; cfg < 2 && ok; ++cfg) {
    for (unsigned lane = 0; lane < 4 && ok; ++lane) {
      nyr_func_t funcs[3] = {{0}};
      bool built = build_f64_shuffle_lane(&funcs[0], 0, lane, cfg) &&
                   build_f64_shuffle_lane(&funcs[1], 0, lane, cfg) &&
                   build_f64_shuffle_lane(&funcs[2], INT64_C(1) << (lane * 2u),
                                          lane, cfg);
      char err[256] = {0};
      bool equivalent =
          built && nyr_tv_smt_equiv(&funcs[0], &funcs[1], err, sizeof(err));
      err[0] = '\0';
      bool rejects_changed_lane =
          built && !nyr_tv_smt_equiv(&funcs[0], &funcs[2], err, sizeof(err));
      uint64_t same_memory[4] = {
          UINT64_C(0x3ff0000000000000), UINT64_C(0x4000000000000000),
          UINT64_C(0x4008000000000000), UINT64_C(0x4010000000000000)};
      uint64_t changed_memory[4] = {
          UINT64_C(0x3ff0000000000000), UINT64_C(0x4000000000000000),
          UINT64_C(0x4008000000000000), UINT64_C(0x4010000000000000)};
      int64_t same_input = (int64_t)(uintptr_t)same_memory;
      int64_t changed_input = (int64_t)(uintptr_t)changed_memory;
      nyr_eval_result_t same_eval = {0}, changed_eval = {0};
      char eval_err[256] = {0};
      bool bounded_agrees =
          built &&
          nyr_ir_eval(&funcs[0], &same_input, 1, 1000, &same_eval, eval_err,
                    sizeof(eval_err)) &&
          nyr_ir_eval(&funcs[2], &changed_input, 1, 1000, &changed_eval, eval_err,
                    sizeof(eval_err)) &&
          same_eval.returned && changed_eval.returned &&
          (uint64_t)same_eval.result == UINT64_C(0x3ff0000000000000) &&
          (uint64_t)changed_eval.result == UINT64_C(0x4000000000000000);
      if (!built || !equivalent || !rejects_changed_lane)
        fprintf(stderr,
                "[tv-smt-%sf64-shuffle] built=%s lane=%u same=%s changed=%s: "
                "%s\n",
                cfg ? "cfg-" : "", built ? "yes" : "no", lane,
                equivalent ? "equivalent" : "failed",
                rejects_changed_lane ? "rejected" : "not rejected", err);
      if (!bounded_agrees)
        fprintf(stderr,
                "[tv-smt-%sf64-shuffle] bounded lane=%u result mismatch: %s\n",
                cfg ? "cfg-" : "", lane, eval_err);
      nyr_eval_result_free(&same_eval);
      nyr_eval_result_free(&changed_eval);
      for (size_t i = 0; i < sizeof(funcs) / sizeof(funcs[0]); ++i)
        nyr_ir_func_free(&funcs[i]);
      ok = built && equivalent && rejects_changed_lane && bounded_agrees;
    }
  }
  return ok;
}

static bool build_f64_vec4_add(nyr_func_t *f, bool vectorized,
                               bool omit_last_lane) {
  nyr_opt_begin(f);
  int sum = -1;
  if (vectorized) {
    int lhs_scalar = nyr_opt_const_f64(f, 1.25);
    int rhs_scalar = nyr_opt_const_f64(f, 2.5);
    int lhs = lhs_scalar < 0
                  ? -1
                  : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                               .dst = -1,
                                               .a = lhs_scalar,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
    int rhs = rhs_scalar < 0
                  ? -1
                  : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                               .dst = -1,
                                               .a = rhs_scalar,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
    int added = lhs < 0 || rhs < 0
                    ? -1
                    : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_ADD_F64,
                                                 .dst = -1,
                                                 .a = lhs,
                                                 .b = rhs,
                                                 .c = -1,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1});
    int zero = nyr_opt_const_f64(f, 0.0);
    sum = added < 0 || zero < 0
              ? -1
              : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_F64,
                                           .dst = -1,
                                           .a = zero,
                                           .b = added,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  } else {
    int zero = nyr_opt_const_f64(f, 0.0);
    int lhs = nyr_opt_const_f64(f, 1.25);
    int rhs = nyr_opt_const_f64(f, 2.5);
    sum = zero;
    unsigned lanes = omit_last_lane ? 3u : 4u;
    for (unsigned lane = 0; lane < lanes && sum >= 0; ++lane) {
      int product =
          lhs < 0 || rhs < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, lhs, rhs);
      sum = product < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, sum, product);
    }
  }
  int result = emit_f64_to_i64(f, sum);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-f64-vec4-add");
}

static bool checks_f64_vec4_add(void) {
  nyr_func_t vector = {0}, scalar = {0}, wrong = {0};
  bool built = build_f64_vec4_add(&vector, true, false) &&
               build_f64_vec4_add(&scalar, false, false) &&
               build_f64_vec4_add(&wrong, false, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
  err[0] = '\0';
  bool rejects_changed_lane =
      built && !nyr_tv_smt_equiv(&vector, &wrong, err, sizeof(err));
  if (!built || !equivalent || !rejects_changed_lane)
    fprintf(stderr,
            "[tv-smt-f64-vec4-add] built=%s equivalent=%s rejects=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_changed_lane ? "yes" : "no",
            err[0] ? err : "four-lane f64 add mismatch");
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&wrong);
  return built && equivalent && rejects_changed_lane;
}

static bool build_cfg_f64_vec4_binary(nyr_func_t *f, bool vectorized,
                                      bool omit_last_lane) {
  static const nyr_op_t vector_ops[] = {NYIR_VEC4_ADD_F64, NYIR_VEC4_SUB_F64,
                                         NYIR_VEC4_MUL_F64, NYIR_VEC4_DIV_F64};
  static const nyr_op_t scalar_ops[] = {NYIR_ADD_F64, NYIR_SUB_F64,
                                         NYIR_MUL_F64, NYIR_DIV_F64};
  static const double lhs_values[] = {1.25, 5.5, 1.5, 8.0};
  static const double rhs_values[] = {2.5, 2.0, 2.5, 2.0};
  nyr_opt_begin(f);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int sum = nyr_opt_const_f64(f, 0.0);
  if (sum < 0)
    return false;
  for (size_t op = 0; op < sizeof(vector_ops) / sizeof(vector_ops[0]); ++op) {
    int lhs = nyr_opt_const_f64(f, lhs_values[op]);
    int rhs = nyr_opt_const_f64(f, rhs_values[op]);
    int op_sum = nyr_opt_const_f64(f, 0.0);
    if (lhs < 0 || rhs < 0 || op_sum < 0)
      return false;
    if (vectorized) {
      int left = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                            .dst = -1,
                                            .a = lhs,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
      int right = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                             .dst = -1,
                                             .a = rhs,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1});
      int packed = left < 0 || right < 0
                       ? -1
                       : nyr_ir_emit(f, (nyr_inst_t){.op = vector_ops[op],
                                                    .dst = -1,
                                                    .a = left,
                                                    .b = right,
                                                    .c = -1,
                                                    .d = -1,
                                                    .e = -1,
                                                    .f = -1});
      op_sum = packed < 0
                   ? -1
                   : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_F64,
                                                .dst = -1,
                                                .a = op_sum,
                                                .b = packed,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
    } else {
      unsigned lanes =
          omit_last_lane && op + 1 == sizeof(vector_ops) / sizeof(vector_ops[0])
              ? 3u
              : 4u;
      for (unsigned lane = 0; lane < lanes && op_sum >= 0; ++lane) {
        int value = nyr_opt_binop(f, scalar_ops[op], lhs, rhs);
        op_sum = value < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, op_sum, value);
      }
    }
    sum = op_sum < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, sum, op_sum);
    if (sum < 0)
      return false;
  }
  int result = sum < 0 ? -1 : emit_f64_to_i64(f, sum);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-cfg-f64-vec4-binary");
}

static bool checks_cfg_f64_vec4_binary(void) {
  nyr_func_t vector = {0}, scalar = {0}, wrong = {0};
  bool built = build_cfg_f64_vec4_binary(&vector, true, false) &&
               build_cfg_f64_vec4_binary(&scalar, false, false) &&
               build_cfg_f64_vec4_binary(&wrong, false, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
  err[0] = '\0';
  bool rejects_changed_lane =
      built && !nyr_tv_smt_equiv(&vector, &wrong, err, sizeof(err));
  if (!built || !equivalent || !rejects_changed_lane)
    fprintf(stderr,
            "[tv-smt-cfg-f64-vec4-arithmetic] built=%s equivalent=%s "
            "changed-lane=%s: %s\n",
            built ? "yes" : "no", equivalent ? "proved" : "failed",
            rejects_changed_lane ? "rejected" : "missed",
            err[0] ? err : "four-lane f64 CFG arithmetic mismatch");
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&wrong);
  return built && equivalent && rejects_changed_lane;
}

static bool build_f64_vec4_fma(nyr_func_t *f, bool vectorized, double addend,
                               bool cfg) {
  nyr_opt_begin(f);
  if (cfg) {
    nyr_opt_br(f, 13);
    nyr_opt_label(f, 13);
  }
  int sum = -1;
  if (vectorized) {
    int lhs_scalar = nyr_opt_const_f64(f, 2.0);
    int rhs_scalar = nyr_opt_const_f64(f, 3.0);
    int addend_scalar = nyr_opt_const_f64(f, addend);
    int lhs = lhs_scalar < 0
                  ? -1
                  : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                               .dst = -1,
                                               .a = lhs_scalar,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
    int rhs = rhs_scalar < 0
                  ? -1
                  : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                               .dst = -1,
                                               .a = rhs_scalar,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
    int addend_vector =
        addend_scalar < 0 ? -1
                          : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                                       .dst = -1,
                                                       .a = addend_scalar,
                                                       .b = -1,
                                                       .c = -1,
                                                       .d = -1,
                                                       .e = -1,
                                                       .f = -1});
    int fused = lhs < 0 || rhs < 0 || addend_vector < 0
                    ? -1
                    : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_FMA_F64,
                                                 .dst = -1,
                                                 .a = lhs,
                                                 .b = rhs,
                                                 .c = addend_vector,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1});
    int zero = nyr_opt_const_f64(f, 0.0);
    sum = fused < 0 || zero < 0
              ? -1
              : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_F64,
                                           .dst = -1,
                                           .a = zero,
                                           .b = fused,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  } else {
    int lhs = nyr_opt_const_f64(f, 2.0);
    int rhs = nyr_opt_const_f64(f, 3.0);
    int c = nyr_opt_const_f64(f, addend);
    sum = nyr_opt_const_f64(f, 0.0);
    for (unsigned lane = 0; lane < 4 && sum >= 0; ++lane) {
      int product =
          lhs < 0 || rhs < 0 ? -1 : nyr_opt_binop(f, NYIR_MUL_F64, lhs, rhs);
      int fused = product < 0 || c < 0
                      ? -1
                      : nyr_opt_binop(f, NYIR_ADD_F64, product, c);
      sum = fused < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, sum, fused);
    }
  }
  int result = sum < 0 ? -1 : emit_f64_to_i64(f, sum);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, cfg ? "tv-smt-cfg-f64-vec4-fma"
                               : "tv-smt-f64-vec4-fma");
}

static bool checks_f64_vec4_fma(void) {
  nyr_func_t vector = {0}, scalar = {0}, changed = {0};
  nyr_func_t cfg_vector = {0}, cfg_scalar = {0}, cfg_changed = {0};
  bool built = build_f64_vec4_fma(&vector, true, 4.0, false) &&
               build_f64_vec4_fma(&scalar, false, 4.0, false) &&
               build_f64_vec4_fma(&changed, true, 5.0, false) &&
               build_f64_vec4_fma(&cfg_vector, true, 4.0, true) &&
               build_f64_vec4_fma(&cfg_scalar, false, 4.0, true) &&
               build_f64_vec4_fma(&cfg_changed, true, 5.0, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
  char cfg_err[256] = {0};
  bool cfg_equivalent = built && nyr_tv_smt_equiv(&cfg_vector, &cfg_scalar,
                                                   cfg_err, sizeof(cfg_err));
  err[0] = '\0';
  bool rejects_changed_addend =
      built && !nyr_tv_smt_equiv(&vector, &changed, err, sizeof(err));
  char cfg_changed_err[256] = {0};
  bool cfg_rejects_changed_addend =
      built && !nyr_tv_smt_equiv(&cfg_vector, &cfg_changed, cfg_changed_err,
                                  sizeof(cfg_changed_err));
  nyr_eval_result_t vector_result = {0}, scalar_result = {0};
  char eval_err[192] = {0};
  bool bounded_agrees = built &&
                        nyr_ir_eval(&vector, NULL, 0, 1000, &vector_result,
                                  eval_err, sizeof(eval_err)) &&
                        nyr_ir_eval(&scalar, NULL, 0, 1000, &scalar_result,
                                  eval_err, sizeof(eval_err)) &&
                        vector_result.returned && scalar_result.returned &&
                        vector_result.result == scalar_result.result;
  nyr_eval_result_t cfg_vector_result = {0}, cfg_scalar_result = {0};
  bool cfg_bounded_agrees =
      built &&
      nyr_ir_eval(&cfg_vector, NULL, 0, 1000, &cfg_vector_result, eval_err,
                sizeof(eval_err)) &&
      nyr_ir_eval(&cfg_scalar, NULL, 0, 1000, &cfg_scalar_result, eval_err,
                sizeof(eval_err)) &&
      cfg_vector_result.returned && cfg_scalar_result.returned &&
      cfg_vector_result.result == cfg_scalar_result.result;
  if (!built || !equivalent || !rejects_changed_addend || !cfg_equivalent ||
      !cfg_rejects_changed_addend)
    fprintf(stderr,
            "[tv-smt-f64-vec4-fma] built=%s straight=%s/%s cfg=%s/%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_changed_addend ? "rejects-changed" : "missed-change",
            cfg_equivalent ? "equivalent" : "failed",
            cfg_rejects_changed_addend ? "rejects-changed" : "missed-change",
            err[0]               ? err
            : cfg_err[0]         ? cfg_err
            : cfg_changed_err[0] ? cfg_changed_err
                                 : "four-lane fused f64 mismatch");
  if (!bounded_agrees || !cfg_bounded_agrees)
    fprintf(stderr,
            "[tv-smt-f64-vec4-fma] straight-interpreter=%s "
            "cfg-interpreter=%s: %s\n",
            bounded_agrees ? "agrees" : "failed",
            cfg_bounded_agrees ? "agrees" : "failed",
            eval_err[0] ? eval_err : "vector/scalar results differ");
  nyr_eval_result_free(&vector_result);
  nyr_eval_result_free(&scalar_result);
  nyr_eval_result_free(&cfg_vector_result);
  nyr_eval_result_free(&cfg_scalar_result);
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&changed);
  nyr_ir_func_free(&cfg_vector);
  nyr_ir_func_free(&cfg_scalar);
  nyr_ir_func_free(&cfg_changed);
  return built && equivalent && rejects_changed_addend && bounded_agrees &&
         cfg_equivalent && cfg_rejects_changed_addend && cfg_bounded_agrees;
}

static int emit_f64_scalar_binary(nyr_func_t *f, nyr_op_t vector_op, int lhs,
                                  int rhs) {
  nyr_op_t scalar_op = NYIR_NOP;
  switch (vector_op) {
  case NYIR_VEC4_ADD_F64:
    scalar_op = NYIR_ADD_F64;
    break;
  case NYIR_VEC4_SUB_F64:
    scalar_op = NYIR_SUB_F64;
    break;
  case NYIR_VEC4_MUL_F64:
    scalar_op = NYIR_MUL_F64;
    break;
  case NYIR_VEC4_DIV_F64:
    scalar_op = NYIR_DIV_F64;
    break;
  default:
    return -1;
  }
  return nyr_opt_binop(f, scalar_op, lhs, rhs);
}

static int emit_f64_stack_array(nyr_func_t *f, const double values[4]) {
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 32,
                                        .scalar_class = NYIR_SCALAR_POINTER});
  if (base < 0)
    return -1;
  for (unsigned lane = 0; lane < 4; ++lane) {
    int offset = nyr_opt_const(f, (int64_t)lane * 8);
    int address = lane == 0    ? base
                  : offset < 0 ? -1
                               : nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
    int64_t bits = 0;
    memcpy(&bits, &values[lane], sizeof(bits));
    int value = nyr_opt_const(f, bits);
    if (value < 0)
      return -1;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1,
                                     .a = address,
                                     .b = -1,
                                     .c = value,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .scalar_class = NYIR_SCALAR_F64});
  }
  return base;
}

static bool build_f64_vec4_binary_memory(nyr_func_t *f, nyr_op_t op,
                                         bool vectorized) {
  nyr_opt_begin(f);
  static const double lhs_values[4] = {1.25, -2.0, 3.5, 8.0};
  static const double rhs_values[4] = {0.5, 4.0, -2.0, 2.0};
  int lhs_address = -1, rhs_address = -1;
  if (vectorized) {
    lhs_address = emit_f64_stack_array(f, lhs_values);
    rhs_address = emit_f64_stack_array(f, rhs_values);
    if (lhs_address < 0 || rhs_address < 0)
      return false;
  }
  int sum = nyr_opt_const_f64(f, 0.0);
  if (sum < 0)
    return false;
  if (vectorized) {
    int lhs = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                         .dst = -1,
                                         .a = lhs_address,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
    int rhs = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                         .dst = -1,
                                         .a = rhs_address,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
    int result = lhs < 0 || rhs < 0 ? -1
                                    : nyr_ir_emit(f, (nyr_inst_t){.op = op,
                                                                 .dst = -1,
                                                                 .a = lhs,
                                                                 .b = rhs,
                                                                 .c = -1,
                                                                 .d = -1,
                                                                 .e = -1,
                                                                 .f = -1});
    sum = result < 0
              ? -1
              : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_F64,
                                           .dst = -1,
                                           .a = sum,
                                           .b = result,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  } else {
    for (unsigned lane = 0; lane < 4 && sum >= 0; ++lane) {
      int lhs = nyr_opt_const_f64(f, lhs_values[lane]);
      int rhs = nyr_opt_const_f64(f, rhs_values[lane]);
      int result =
          lhs < 0 || rhs < 0 ? -1 : emit_f64_scalar_binary(f, op, lhs, rhs);
      sum = result < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, sum, result);
    }
  }
  int result = sum < 0 ? -1 : emit_f64_to_i64(f, sum);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-f64-vec4-binary-memory");
}

static bool build_external_f64_vec4_binary(nyr_func_t *f, nyr_op_t op,
                                           bool vectorized) {
  nyr_opt_begin(f);
  int lhs_address = load_local(f, 0);
  int rhs_address = load_local(f, 1);
  int sum = nyr_opt_const_f64(f, 0.0);
  if (lhs_address < 0 || rhs_address < 0 || sum < 0)
    return false;
  if (vectorized) {
    int lhs = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                         .dst = -1,
                                         .a = lhs_address,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
    int rhs = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                         .dst = -1,
                                         .a = rhs_address,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
    int result = lhs < 0 || rhs < 0 ? -1
                                    : nyr_ir_emit(f, (nyr_inst_t){.op = op,
                                                                 .dst = -1,
                                                                 .a = lhs,
                                                                 .b = rhs,
                                                                 .c = -1,
                                                                 .d = -1,
                                                                 .e = -1,
                                                                 .f = -1});
    sum = result < 0
              ? -1
              : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_F64,
                                           .dst = -1,
                                           .a = sum,
                                           .b = result,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  } else {
    for (unsigned lane = 0; lane < 4 && sum >= 0; ++lane) {
      int offset = nyr_opt_const(f, (int64_t)lane * 8);
      int lhs_lane = lane == 0 ? lhs_address
                     : offset < 0
                         ? -1
                         : nyr_opt_binop(f, NYIR_ADD_I64, lhs_address, offset);
      int rhs_lane = lane == 0 ? rhs_address
                     : offset < 0
                         ? -1
                         : nyr_opt_binop(f, NYIR_ADD_I64, rhs_address, offset);
      int lhs = emit_load_i64(f, lhs_lane);
      int rhs = emit_load_i64(f, rhs_lane);
      int result =
          lhs < 0 || rhs < 0 ? -1 : emit_f64_scalar_binary(f, op, lhs, rhs);
      sum = result < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, sum, result);
    }
  }
  int result = sum < 0 ? -1 : emit_f64_to_i64(f, sum);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-f64-external-vec4-binary");
}

static bool checks_external_f64_vec4_binary_smt(void) {
  const nyr_op_t ops[] = {NYIR_VEC4_ADD_F64, NYIR_VEC4_SUB_F64,
                           NYIR_VEC4_MUL_F64, NYIR_VEC4_DIV_F64};
  bool ok = true;
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]) && ok; ++i) {
    nyr_func_t vector = {0}, scalar = {0};
    bool built = build_external_f64_vec4_binary(&vector, ops[i], true) &&
                 build_external_f64_vec4_binary(&scalar, ops[i], false);
    char err[192] = {0};
    bool equivalent =
        built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
    if (!built || !equivalent)
      fprintf(stderr,
              "[tv-smt-f64-external-memory] op=%d built=%s equivalent=%s: %s\n",
              ops[i], built ? "yes" : "no", equivalent ? "proved" : "failed",
              err[0] ? err : "SMT proof inapplicable");
    ok &= built && equivalent;
    nyr_ir_func_free(&vector);
    nyr_ir_func_free(&scalar);
  }
  return ok;
}

static bool checks_f64_vec4_binary_operations(void) {
  const nyr_op_t ops[] = {NYIR_VEC4_ADD_F64, NYIR_VEC4_SUB_F64,
                           NYIR_VEC4_MUL_F64, NYIR_VEC4_DIV_F64};
  static const int64_t expected[] = {15, 6, 1, 4};
  bool ok = true;
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]) && ok; ++i) {
    nyr_func_t vector = {0}, scalar = {0};
    bool built = build_f64_vec4_binary_memory(&vector, ops[i], true) &&
                 build_f64_vec4_binary_memory(&scalar, ops[i], false);
    nyr_eval_result_t vector_result = {0}, scalar_result = {0};
    char eval_err[192] = {0};
    bool interpreter_agrees = built &&
                              nyr_ir_eval(&vector, NULL, 0, 1000, &vector_result,
                                        eval_err, sizeof(eval_err)) &&
                              nyr_ir_eval(&scalar, NULL, 0, 1000, &scalar_result,
                                        eval_err, sizeof(eval_err)) &&
                              vector_result.returned &&
                              scalar_result.returned &&
                              vector_result.result == scalar_result.result &&
                              vector_result.result == expected[i];
    if (!built || !interpreter_agrees)
      fprintf(stderr,
              "[tv-smt-f64-vec4-binary] op=%d built=%s interpreter=%s: %s\n",
              ops[i], built ? "yes" : "no",
              interpreter_agrees ? "agrees" : "failed",
              eval_err[0] ? eval_err : "four-lane operation mismatch");
    ok &= built && interpreter_agrees;
    nyr_eval_result_free(&vector_result);
    nyr_eval_result_free(&scalar_result);
    nyr_ir_func_free(&vector);
    nyr_ir_func_free(&scalar);
  }
  return ok;
}

static bool build_f64_vec4_fma_memory(nyr_func_t *f) {
  nyr_opt_begin(f);
  int lhs_address = load_local(f, 0);
  int rhs_address = load_local(f, 1);
  int addend_address = load_local(f, 2);
  if (lhs_address < 0 || rhs_address < 0 || addend_address < 0)
    return false;
  int sum = nyr_opt_const_f64(f, 0.0);
  if (sum < 0)
    return false;
  int lhs = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                       .dst = -1,
                                       .a = lhs_address,
                                       .b = -1,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1});
  int rhs = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                       .dst = -1,
                                       .a = rhs_address,
                                       .b = -1,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1});
  int addend = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                                          .dst = -1,
                                          .a = addend_address,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  int fused = lhs < 0 || rhs < 0 || addend < 0
                  ? -1
                  : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_FMA_F64,
                                               .dst = -1,
                                               .a = lhs,
                                               .b = rhs,
                                               .c = addend,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
  sum = fused < 0 ? -1
                  : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_F64,
                                               .dst = -1,
                                               .a = sum,
                                               .b = fused,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
  int result = sum < 0 ? -1 : emit_f64_to_i64(f, sum);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-f64-vec4-fma-memory");
}

static bool checks_f64_vec4_fma_distinct_lanes(void) {
  uint64_t lhs_memory[4] = {
      UINT64_C(0x3ff0000000000000), UINT64_C(0x4000000000000000),
      UINT64_C(0x4008000000000000), UINT64_C(0x4010000000000000)};
  uint64_t rhs_memory[4] = {
      UINT64_C(0x4000000000000000), UINT64_C(0x4008000000000000),
      UINT64_C(0x4010000000000000), UINT64_C(0x4014000000000000)};
  uint64_t addend_memory[4] = {
      UINT64_C(0x3ff0000000000000), UINT64_C(0x4000000000000000),
      UINT64_C(0x4008000000000000), UINT64_C(0x4010000000000000)};
  int64_t args[] = {(int64_t)(uintptr_t)lhs_memory,
                    (int64_t)(uintptr_t)rhs_memory,
                    (int64_t)(uintptr_t)addend_memory};
  nyr_func_t vector = {0};
  bool built = build_f64_vec4_fma_memory(&vector);
  char eval_err[192] = {0};
  nyr_eval_result_t vector_result = {0};
  bool interpreter_agrees = built &&
                            nyr_ir_eval(&vector, args, 3, 1000, &vector_result,
                                      eval_err, sizeof(eval_err)) &&
                            vector_result.returned &&
                            vector_result.result == 50;
  if (!built || !interpreter_agrees)
    fprintf(stderr, "[tv-smt-f64-vec4-fma-lanes] built=%s interpreter=%s: %s\n",
            built ? "yes" : "no", interpreter_agrees ? "returns 50" : "failed",
            eval_err[0] ? eval_err : "four-lane FMA mismatch");
  nyr_eval_result_free(&vector_result);
  nyr_ir_func_free(&vector);
  return built && interpreter_agrees;
}

static int build_f64_fma_rounding_sign(nyr_func_t *f, bool fused) {
  const double delta = 0x1p-27;
  nyr_opt_begin(f);
  int sum = nyr_opt_const_f64(f, 0.0);
  if (fused) {
    int a_scalar = nyr_opt_const_f64(f, 1.0 + delta);
    int b_scalar = nyr_opt_const_f64(f, 1.0 - delta);
    int c_scalar = nyr_opt_const_f64(f, -1.0);
    int a = a_scalar < 0 ? -1
                         : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                                      .dst = -1,
                                                      .a = a_scalar,
                                                      .b = -1,
                                                      .c = -1,
                                                      .d = -1,
                                                      .e = -1,
                                                      .f = -1});
    int b = b_scalar < 0 ? -1
                         : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                                      .dst = -1,
                                                      .a = b_scalar,
                                                      .b = -1,
                                                      .c = -1,
                                                      .d = -1,
                                                      .e = -1,
                                                      .f = -1});
    int c = c_scalar < 0 ? -1
                         : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                                      .dst = -1,
                                                      .a = c_scalar,
                                                      .b = -1,
                                                      .c = -1,
                                                      .d = -1,
                                                      .e = -1,
                                                      .f = -1});
    int product = a < 0 || b < 0 || c < 0
                      ? -1
                      : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_FMA_F64,
                                                   .dst = -1,
                                                   .a = a,
                                                   .b = b,
                                                   .c = c,
                                                   .d = -1,
                                                   .e = -1,
                                                   .f = -1});
    sum = product < 0
              ? -1
              : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC4_REDUCE_ADD_F64,
                                           .dst = -1,
                                           .a = sum,
                                           .b = product,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  } else {
    int a = nyr_opt_const_f64(f, 1.0 + delta);
    int b = nyr_opt_const_f64(f, 1.0 - delta);
    int c = nyr_opt_const_f64(f, -1.0);
    for (unsigned lane = 0; lane < 4 && sum >= 0; ++lane) {
      int product = a < 0 || b < 0 ? -1 : nyr_opt_binop(f, NYIR_MUL_F64, a, b);
      int adjusted = product < 0 || c < 0
                         ? -1
                         : nyr_opt_binop(f, NYIR_ADD_F64, product, c);
      sum = adjusted < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, sum, adjusted);
    }
  }
  int zero = nyr_opt_const_f64(f, 0.0);
  int negative = sum < 0 || zero < 0
                     ? -1
                     : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CMP_F64,
                                                  .dst = -1,
                                                  .a = sum,
                                                  .b = zero,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1,
                                                  .cmp = NYIR_CMP_LT});
  if (negative < 0)
    return -1;
  nyr_opt_ret(f, negative);
  return nyr_opt_verify(f, "tv-smt-f64-fma-rounding-sign") ? 0 : -1;
}

static bool build_i64_constant(nyr_func_t *f, int64_t value) {
  nyr_opt_begin(f);
  int result = nyr_opt_const(f, value);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-i64-constant");
}

static bool checks_f64_fma_rounding(void) {
  nyr_func_t fused = {0}, separate = {0}, expected_fused = {0};
  nyr_func_t expected_separate = {0};
  bool built = build_f64_fma_rounding_sign(&fused, true) == 0 &&
               build_f64_fma_rounding_sign(&separate, false) == 0 &&
               build_i64_constant(&expected_fused, 1) &&
               build_i64_constant(&expected_separate, 0);
  char err[256] = {0};
  bool fused_is_negative =
      built && nyr_tv_smt_equiv(&fused, &expected_fused, err, sizeof(err));
  err[0] = '\0';
  bool separate_is_zero =
      built &&
      nyr_tv_smt_equiv(&separate, &expected_separate, err, sizeof(err));
  err[0] = '\0';
  bool distinguishes_rounding =
      built && !nyr_tv_smt_equiv(&fused, &separate, err, sizeof(err));
  nyr_eval_result_t fused_eval = {0}, separate_eval = {0};
  char eval_err[192] = {0};
  bool bounded_distinguishes =
      built &&
      nyr_ir_eval(&fused, NULL, 0, 1000, &fused_eval, eval_err,
                sizeof(eval_err)) &&
      nyr_ir_eval(&separate, NULL, 0, 1000, &separate_eval, eval_err,
                sizeof(eval_err)) &&
      fused_eval.returned && separate_eval.returned && fused_eval.result == 1 &&
      separate_eval.result == 0;
  if (!built || !fused_is_negative || !separate_is_zero ||
      !distinguishes_rounding || !bounded_distinguishes)
    fprintf(stderr,
            "[tv-smt-f64-fma-rounding] built=%s fused-negative=%s "
            "separate-zero=%s distinguishes=%s bounded=%s: %s\n",
            built ? "yes" : "no", fused_is_negative ? "yes" : "no",
            separate_is_zero ? "yes" : "no",
            distinguishes_rounding ? "yes" : "no",
            bounded_distinguishes ? "yes" : "no",
            eval_err[0] ? eval_err
            : err[0]    ? err
                        : "fused rounding was not distinguished");
  nyr_eval_result_free(&fused_eval);
  nyr_eval_result_free(&separate_eval);
  nyr_ir_func_free(&fused);
  nyr_ir_func_free(&separate);
  nyr_ir_func_free(&expected_fused);
  nyr_ir_func_free(&expected_separate);
  return built && fused_is_negative && separate_is_zero &&
         distinguishes_rounding && bounded_distinguishes;
}

static int emit_f32_constant(nyr_func_t *f, uint32_t bits) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CONST_F32,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = (int64_t)bits});
}

static int emit_vec4_f32_splat(nyr_func_t *f, int scalar) {
  return scalar < 0 ? -1
                    : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_SET1_F32,
                                                 .dst = -1,
                                                 .a = scalar,
                                                 .b = -1,
                                                 .c = -1,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1});
}

static int emit_f32_byte_store(nyr_func_t *f, int base, unsigned offset,
                               int value, unsigned byte) {
  unsigned address_offset = offset + byte;
  int byte_offset = address_offset ? nyr_opt_const(f, address_offset) : -1;
  int address = address_offset == 0
                    ? base
                    : (byte_offset < 0
                           ? -1
                           : nyr_opt_binop(f, NYIR_ADD_I64, base, byte_offset));
  int shift = byte ? nyr_opt_const(f, (int64_t)byte * 8) : -1;
  int shifted =
      byte == 0
          ? value
          : (shift < 0 ? -1 : nyr_opt_binop(f, NYIR_SAR_I64, value, shift));
  int mask = nyr_opt_const(f, 0xff);
  int low_byte = shifted < 0 || mask < 0
                     ? -1
                     : nyr_opt_binop(f, NYIR_AND_I64, shifted, mask);
  if (address < 0 || low_byte < 0)
    return -1;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = address,
                                   .b = -1,
                                   .c = low_byte,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .flags = NYIR_INST_F_MEM_BYTE});
  return 0;
}

static bool build_f32_vec4_memory_result(nyr_func_t *f, bool vectorized,
                                         nyr_op_t vector_op,
                                         nyr_op_t scalar_op, uint32_t a_bits,
                                         uint32_t b_bits, bool cfg,
                                         uint32_t expected_bits) {
  nyr_opt_begin(f);
  if (cfg) {
    nyr_opt_br(f, 10);
    nyr_opt_label(f, 10);
  }
  int destination = load_local(f, 0);
  if (destination < 0)
    return false;
  if (vectorized) {
    int a_scalar = emit_f32_constant(f, a_bits);
    int b_scalar = emit_f32_constant(f, b_bits);
    int a = emit_vec4_f32_splat(f, a_scalar);
    int b = emit_vec4_f32_splat(f, b_scalar);
    int result = a < 0 || b < 0 ? -1
                                : nyr_ir_emit(f, (nyr_inst_t){.op = vector_op,
                                                             .dst = -1,
                                                             .a = a,
                                                             .b = b,
                                                             .c = -1,
                                                             .d = -1,
                                                             .e = -1,
                                                             .f = -1});
    if (result < 0)
      return false;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_STORE_F32,
                                     .dst = -1,
                                     .a = destination,
                                     .b = result,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
  } else {
    int a = emit_f32_constant(f, a_bits);
    int b = emit_f32_constant(f, b_bits);
    int result = a < 0 || b < 0 ? -1 : nyr_opt_binop(f, scalar_op, a, b);
    if (result < 0)
      return false;
    for (unsigned lane = 0; lane < 4; ++lane)
      for (unsigned byte = 0; byte < 4; ++byte)
        if (emit_f32_byte_store(f, destination, lane * 4, result, byte) < 0)
          return false;
  }
  int second_offset = nyr_opt_const(f, 8);
  int second_address =
      second_offset < 0
          ? -1
          : nyr_opt_binop(f, NYIR_ADD_I64, destination, second_offset);
  int low = emit_load_i64(f, destination);
  int high = emit_load_i64(f, second_address);
  uint64_t expected_pair = ((uint64_t)expected_bits << 32) | expected_bits;
  int expected = nyr_opt_const(f, (int64_t)expected_pair);
  int low_delta = low < 0 || expected < 0
                      ? -1
                      : nyr_opt_binop(f, NYIR_XOR_I64, low, expected);
  int high_delta = high < 0 || expected < 0
                       ? -1
                       : nyr_opt_binop(f, NYIR_XOR_I64, high, expected);
  int zero = nyr_opt_const(f, 0);
  int low_ok = low_delta < 0 || zero < 0
                   ? -1
                   : nyr_opt_cmp(f, low_delta, zero, NYIR_CMP_EQ);
  int high_ok = high_delta < 0 || zero < 0
                    ? -1
                    : nyr_opt_cmp(f, high_delta, zero, NYIR_CMP_EQ);
  int result = low_ok < 0 || high_ok < 0
                   ? -1
                   : nyr_opt_binop(f, NYIR_AND_I64, low_ok, high_ok);
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "tv-smt-f32-vec4-memory");
}

static bool checks_f32_vec4_memory_semantics(void) {
  static const struct {
    nyr_op_t vector_op;
    nyr_op_t scalar_op;
    uint32_t a;
    uint32_t b;
    uint32_t result;
  } cases[] = {
      {NYIR_VEC8_ADD_F32, NYIR_ADD_F32, UINT32_C(0x3fa00000),
       UINT32_C(0x40200000), UINT32_C(0x40700000)},
      {NYIR_VEC8_SUB_F32, NYIR_SUB_F32, UINT32_C(0x40200000),
       UINT32_C(0x3fa00000), UINT32_C(0x3fa00000)},
      {NYIR_VEC8_MUL_F32, NYIR_MUL_F32, UINT32_C(0x3fc00000),
       UINT32_C(0x40000000), UINT32_C(0x40400000)},
      {NYIR_VEC8_DIV_F32, NYIR_DIV_F32, UINT32_C(0x40c00000),
       UINT32_C(0x40000000), UINT32_C(0x40400000)},
      /*
       * Halfway cases must round to the even significand.
       */
      {NYIR_VEC8_ADD_F32, NYIR_ADD_F32, UINT32_C(0x3f800000),
       UINT32_C(0x33800000), UINT32_C(0x3f800000)},
      {NYIR_VEC8_MUL_F32, NYIR_MUL_F32, UINT32_C(0x3f800800),
       UINT32_C(0x3f800800), UINT32_C(0x3f801000)},
      /*
       * 1.0f / 3.0f is inexact and rounds to 0x3eaaaaab.
       */
      {NYIR_VEC8_DIV_F32, NYIR_DIV_F32, UINT32_C(0x3f800000),
       UINT32_C(0x40400000), UINT32_C(0x3eaaaaab)},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    nyr_func_t vector = {0}, scalar = {0}, wrong = {0};
    bool built =
        build_f32_vec4_memory_result(&vector, true, cases[i].vector_op,
                                     cases[i].scalar_op, cases[i].a, cases[i].b,
                                     false, cases[i].result) &&
        build_f32_vec4_memory_result(&scalar, false, cases[i].vector_op,
                                     cases[i].scalar_op, cases[i].a, cases[i].b,
                                     false, cases[i].result) &&
        build_f32_vec4_memory_result(
            &wrong, true,
            cases[i].vector_op == NYIR_VEC8_ADD_F32 ? NYIR_VEC8_SUB_F32
                                                    : NYIR_VEC8_ADD_F32,
            cases[i].scalar_op, cases[i].a, cases[i].b, false, cases[i].result);
    char err[256] = {0};
    bool equivalent =
        built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
    err[0] = '\0';
    bool rejects_wrong =
        built && !nyr_tv_smt_equiv(&vector, &wrong, err, sizeof(err));
    uint32_t vm_memory[4] = {0};
    int64_t vm_inputs[1] = {(int64_t)(uintptr_t)vm_memory};
    nyr_eval_result_t vector_eval = {0}, scalar_eval = {0}, wrong_eval = {0};
    char eval_err[192] = {0};
    bool bounded_agrees = built &&
                          nyr_ir_eval(&vector, vm_inputs, 1, 1000, &vector_eval,
                                    eval_err, sizeof(eval_err)) &&
                          nyr_ir_eval(&scalar, vm_inputs, 1, 1000, &scalar_eval,
                                    eval_err, sizeof(eval_err)) &&
                          nyr_ir_eval(&wrong, vm_inputs, 1, 1000, &wrong_eval,
                                    eval_err, sizeof(eval_err)) &&
                          vector_eval.returned && scalar_eval.returned &&
                          wrong_eval.returned && vector_eval.result == 1 &&
                          vector_eval.result == scalar_eval.result &&
                          wrong_eval.result == 0;
    if (!built || !equivalent || !rejects_wrong || !bounded_agrees)
      fprintf(stderr,
              "[tv-smt-f32-vec4-%zu] built=%s equivalent=%s rejects=%s "
              "bounded=%s: %s\n",
              i, built ? "yes" : "no", equivalent ? "yes" : "no",
              rejects_wrong ? "yes" : "no", bounded_agrees ? "agrees" : "bad",
              eval_err[0] ? eval_err
              : err[0]    ? err
                          : "four-lane f32 vector mismatch");
    nyr_eval_result_free(&vector_eval);
    nyr_eval_result_free(&scalar_eval);
    nyr_eval_result_free(&wrong_eval);
    nyr_ir_func_free(&vector);
    nyr_ir_func_free(&scalar);
    nyr_ir_func_free(&wrong);
    if (!built || !equivalent || !rejects_wrong || !bounded_agrees)
      return false;
  }
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    nyr_func_t vector = {0}, scalar = {0}, wrong = {0};
    nyr_op_t wrong_op = cases[i].vector_op == NYIR_VEC8_ADD_F32
                             ? NYIR_VEC8_SUB_F32
                             : NYIR_VEC8_ADD_F32;
    bool built =
        build_f32_vec4_memory_result(&vector, true, cases[i].vector_op,
                                     cases[i].scalar_op, cases[i].a, cases[i].b,
                                     true, cases[i].result) &&
        build_f32_vec4_memory_result(&scalar, false, cases[i].vector_op,
                                     cases[i].scalar_op, cases[i].a, cases[i].b,
                                     true, cases[i].result) &&
        build_f32_vec4_memory_result(&wrong, true, wrong_op, cases[i].scalar_op,
                                     cases[i].a, cases[i].b, true,
                                     cases[i].result);
    char err[256] = {0};
    bool equivalent =
        built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
    err[0] = '\0';
    bool rejects_wrong =
        built && !nyr_tv_smt_equiv(&vector, &wrong, err, sizeof(err));
    uint32_t vm_memory[4] = {0};
    int64_t vm_inputs[1] = {(int64_t)(uintptr_t)vm_memory};
    nyr_eval_result_t vector_eval = {0}, scalar_eval = {0}, wrong_eval = {0};
    char eval_err[192] = {0};
    bool bounded_agrees = built &&
                          nyr_ir_eval(&vector, vm_inputs, 1, 1000, &vector_eval,
                                    eval_err, sizeof(eval_err)) &&
                          nyr_ir_eval(&scalar, vm_inputs, 1, 1000, &scalar_eval,
                                    eval_err, sizeof(eval_err)) &&
                          nyr_ir_eval(&wrong, vm_inputs, 1, 1000, &wrong_eval,
                                    eval_err, sizeof(eval_err)) &&
                          vector_eval.returned && scalar_eval.returned &&
                          wrong_eval.returned && vector_eval.result == 1 &&
                          vector_eval.result == scalar_eval.result &&
                          wrong_eval.result == 0;
    if (!built || !equivalent || !rejects_wrong || !bounded_agrees)
      fprintf(stderr,
              "[tv-smt-f32-vec4-cfg-%zu] built=%s equivalent=%s rejects=%s "
              "bounded=%s: %s\n",
              i, built ? "yes" : "no", equivalent ? "yes" : "no",
              rejects_wrong ? "yes" : "no", bounded_agrees ? "agrees" : "bad",
              eval_err[0] ? eval_err
              : err[0]    ? err
                          : "CFG four-lane f32 mismatch");
    nyr_eval_result_free(&vector_eval);
    nyr_eval_result_free(&scalar_eval);
    nyr_eval_result_free(&wrong_eval);
    nyr_ir_func_free(&vector);
    nyr_ir_func_free(&scalar);
    nyr_ir_func_free(&wrong);
    if (!built || !equivalent || !rejects_wrong || !bounded_agrees)
      return false;
  }
  return true;
}

static bool build_f32_vec4_fma_result(nyr_func_t *f, bool vectorized,
                                      uint32_t addend_bits, bool cfg) {
  static const uint32_t a_bits = UINT32_C(0x3fc00000);
  static const uint32_t b_bits = UINT32_C(0x40000000);
  static const uint32_t result_bits = UINT32_C(0x40600000);
  nyr_opt_begin(f);
  if (cfg) {
    nyr_opt_br(f, 11);
    nyr_opt_label(f, 11);
  }
  int destination = load_local(f, 0);
  int result = -1;
  if (destination < 0)
    return false;
  if (vectorized) {
    int a = emit_f32_constant(f, a_bits);
    int b = emit_f32_constant(f, b_bits);
    int addend = emit_f32_constant(f, addend_bits);
    int va = a < 0 ? -1 : emit_vec4_f32_splat(f, a);
    int vb = b < 0 ? -1 : emit_vec4_f32_splat(f, b);
    int vc = addend < 0 ? -1 : emit_vec4_f32_splat(f, addend);
    result = va < 0 || vb < 0 || vc < 0
                 ? -1
                 : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_FMA_F32,
                                              .dst = -1,
                                              .a = va,
                                              .b = vb,
                                              .c = vc,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1});
    if (result >= 0)
      (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_STORE_F32,
                                       .dst = -1,
                                       .a = destination,
                                       .b = result,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1});
  } else {
    result = emit_f32_constant(f, result_bits);
    for (unsigned lane = 0; result >= 0 && lane < 4; ++lane)
      for (unsigned byte = 0; byte < 4; ++byte)
        if (emit_f32_byte_store(f, destination, lane * 4, result, byte) < 0)
          return false;
  }
  if (result < 0)
    return false;
  int second_offset = nyr_opt_const(f, 8);
  int second_address =
      second_offset < 0
          ? -1
          : nyr_opt_binop(f, NYIR_ADD_I64, destination, second_offset);
  int low = emit_load_i64(f, destination);
  int high = emit_load_i64(f, second_address);
  uint64_t expected_pair = ((uint64_t)result_bits << 32) | result_bits;
  int expected = nyr_opt_const(f, (int64_t)expected_pair);
  int zero = nyr_opt_const(f, 0);
  int low_delta = low < 0 || expected < 0
                      ? -1
                      : nyr_opt_binop(f, NYIR_XOR_I64, low, expected);
  int high_delta = high < 0 || expected < 0
                       ? -1
                       : nyr_opt_binop(f, NYIR_XOR_I64, high, expected);
  int low_ok = low_delta < 0 || zero < 0
                   ? -1
                   : nyr_opt_cmp(f, low_delta, zero, NYIR_CMP_EQ);
  int high_ok = high_delta < 0 || zero < 0
                    ? -1
                    : nyr_opt_cmp(f, high_delta, zero, NYIR_CMP_EQ);
  int ok = low_ok < 0 || high_ok < 0
               ? -1
               : nyr_opt_binop(f, NYIR_AND_I64, low_ok, high_ok);
  if (ok < 0)
    return false;
  nyr_opt_ret(f, ok);
  return nyr_opt_verify(f, "tv-smt-f32-vec4-fma");
}

static bool checks_f32_vec4_fma_semantics(void) {
  nyr_func_t vector = {0}, reference = {0}, wrong = {0};
  nyr_func_t cfg_vector = {0}, cfg_reference = {0}, cfg_wrong = {0};
  bool built =
      build_f32_vec4_fma_result(&vector, true, UINT32_C(0x3f000000), false) &&
      build_f32_vec4_fma_result(&reference, false, 0, false) &&
      build_f32_vec4_fma_result(&wrong, true, 0, false) &&
      build_f32_vec4_fma_result(&cfg_vector, true, UINT32_C(0x3f000000),
                                true) &&
      build_f32_vec4_fma_result(&cfg_reference, false, 0, true) &&
      build_f32_vec4_fma_result(&cfg_wrong, true, 0, true);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &reference, err, sizeof(err));
  err[0] = '\0';
  bool rejects_wrong =
      built && !nyr_tv_smt_equiv(&vector, &wrong, err, sizeof(err));
  err[0] = '\0';
  bool cfg_equivalent =
      built && nyr_tv_smt_equiv(&cfg_vector, &cfg_reference, err, sizeof(err));
  err[0] = '\0';
  bool cfg_rejects =
      built && !nyr_tv_smt_equiv(&cfg_vector, &cfg_wrong, err, sizeof(err));
  uint32_t vm_memory[4] = {0};
  int64_t vm_inputs[1] = {(int64_t)(uintptr_t)vm_memory};
  nyr_eval_result_t vector_eval = {0}, reference_eval = {0}, wrong_eval = {0};
  char eval_err[192] = {0};
  bool bounded_agrees =
      built &&
      nyr_ir_eval(&vector, vm_inputs, 1, 1000, &vector_eval, eval_err,
                sizeof(eval_err)) &&
      nyr_ir_eval(&reference, vm_inputs, 1, 1000, &reference_eval, eval_err,
                sizeof(eval_err)) &&
      nyr_ir_eval(&wrong, vm_inputs, 1, 1000, &wrong_eval, eval_err,
                sizeof(eval_err)) &&
      vector_eval.returned && reference_eval.returned && wrong_eval.returned &&
      vector_eval.result == 1 && vector_eval.result == reference_eval.result &&
      wrong_eval.result == 0;
  if (!built || !equivalent || !rejects_wrong || !cfg_equivalent ||
      !cfg_rejects || !bounded_agrees)
    fprintf(stderr,
            "[tv-smt-f32-vec4-fma] built=%s equivalent=%s rejects=%s "
            "cfg=%s/%s bounded=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_wrong ? "yes" : "no", cfg_equivalent ? "equivalent" : "bad",
            cfg_rejects ? "reject" : "bad", bounded_agrees ? "agrees" : "bad",
            eval_err[0] ? eval_err
            : err[0]    ? err
                        : "four-lane f32 FMA mismatch");
  nyr_eval_result_free(&vector_eval);
  nyr_eval_result_free(&reference_eval);
  nyr_eval_result_free(&wrong_eval);
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&reference);
  nyr_ir_func_free(&wrong);
  nyr_ir_func_free(&cfg_vector);
  nyr_ir_func_free(&cfg_reference);
  nyr_ir_func_free(&cfg_wrong);
  return built && equivalent && rejects_wrong && cfg_equivalent &&
         cfg_rejects && bounded_agrees;
}

static int emit_f32_vector_load(nyr_func_t *f, int address) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_LOAD_F32,
                                    .dst = -1,
                                    .a = address,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1});
}

static int emit_f32_vector_store(nyr_func_t *f, int address, int value) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_STORE_F32,
                                    .dst = -1,
                                    .a = address,
                                    .b = value,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1});
}

static bool build_f32_vec4_fma_distinct_lanes(nyr_func_t *f) {
  nyr_opt_begin(f);
  int lhs_ptr = load_local(f, 0);
  int rhs_ptr = load_local(f, 1);
  int addend_ptr = load_local(f, 2);
  int out_ptr = load_local(f, 3);
  if (lhs_ptr < 0 || rhs_ptr < 0 || addend_ptr < 0 || out_ptr < 0)
    return false;
  int lhs = emit_f32_vector_load(f, lhs_ptr);
  int rhs = emit_f32_vector_load(f, rhs_ptr);
  int addend = emit_f32_vector_load(f, addend_ptr);
  int fused = lhs < 0 || rhs < 0 || addend < 0
                  ? -1
                  : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_FMA_F32,
                                               .dst = -1,
                                               .a = lhs,
                                               .b = rhs,
                                               .c = addend,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
  if (fused < 0 || emit_f32_vector_store(f, out_ptr, fused) < 0)
    return false;
  int ok = nyr_opt_const(f, 1);
  if (ok < 0)
    return false;
  nyr_opt_ret(f, ok);
  return nyr_opt_verify(f, "tv-smt-f32-fma-distinct-lanes");
}

static bool checks_f32_vec4_fma_distinct_lanes(void) {
  static const uint32_t lhs_bits[4] = {
      UINT32_C(0x3fc00000), UINT32_C(0x3f400000), UINT32_C(0xc0000000),
      UINT32_C(0x40800000)};
  static const uint32_t rhs_bits[4] = {
      UINT32_C(0x40000000), UINT32_C(0x40800000), UINT32_C(0x3f000000),
      UINT32_C(0xc0400000)};
  static const uint32_t addend_bits[4] = {
      UINT32_C(0x3f000000), UINT32_C(0xbf800000), UINT32_C(0x3e800000),
      UINT32_C(0x40000000)};
  uint32_t result_bits[4] = {0};
  int64_t inputs[4] = {
      (int64_t)(uintptr_t)lhs_bits, (int64_t)(uintptr_t)rhs_bits,
      (int64_t)(uintptr_t)addend_bits, (int64_t)(uintptr_t)result_bits};
  nyr_func_t f = {0};
  bool built = build_f32_vec4_fma_distinct_lanes(&f);
  nyr_eval_result_t result = {0};
  char err[192] = {0};
  bool evaluated = built &&
                   nyr_ir_eval(&f, inputs, 4, 1000, &result, err, sizeof(err)) &&
                   result.returned && result.result == 1;
  bool matches = evaluated;
  for (unsigned lane = 0; lane < 4 && matches; ++lane) {
    float lhs, rhs, addend;
    memcpy(&lhs, &lhs_bits[lane], sizeof(lhs));
    memcpy(&rhs, &rhs_bits[lane], sizeof(rhs));
    memcpy(&addend, &addend_bits[lane], sizeof(addend));
    uint32_t expected = 0;
    float fused = fmaf(lhs, rhs, addend);
    memcpy(&expected, &fused, sizeof(expected));
    matches = result_bits[lane] == expected;
  }
  if (!built || !evaluated || !matches)
    fprintf(stderr, "[tv-smt-f32-fma-lanes] built=%s evaluated=%s: %s\n",
            built ? "yes" : "no", evaluated ? "yes" : "no",
            err[0] ? err : "lane-wise fused results differ");
  nyr_eval_result_free(&result);
  nyr_ir_func_free(&f);
  return built && evaluated && matches;
}

static bool build_f32_vec4_fma_rounding(nyr_func_t *f, bool fused, bool cfg) {
  nyr_opt_begin(f);
  if (cfg) {
    nyr_opt_br(f, 12);
    nyr_opt_label(f, 12);
  }
  int destination = load_local(f, 0);
  int a_scalar = emit_f32_constant(f, UINT32_C(0x3f800400));
  int b_scalar = emit_f32_constant(f, UINT32_C(0x3f7ff800));
  int c_scalar = emit_f32_constant(f, UINT32_C(0xbf800000));
  int a = a_scalar < 0 ? -1 : emit_vec4_f32_splat(f, a_scalar);
  int b = b_scalar < 0 ? -1 : emit_vec4_f32_splat(f, b_scalar);
  int c = c_scalar < 0 ? -1 : emit_vec4_f32_splat(f, c_scalar);
  if (destination < 0 || a < 0 || b < 0 || c < 0)
    return false;
  int result = -1;
  if (fused) {
    result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_FMA_F32,
                                        .dst = -1,
                                        .a = a,
                                        .b = b,
                                        .c = c,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1});
  } else {
    int product = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_MUL_F32,
                                             .dst = -1,
                                             .a = a,
                                             .b = b,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1});
    result = product < 0 ? -1
                         : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_ADD_F32,
                                                      .dst = -1,
                                                      .a = product,
                                                      .b = c,
                                                      .c = -1,
                                                      .d = -1,
                                                      .e = -1,
                                                      .f = -1});
  }
  if (result < 0)
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_STORE_F32,
                                   .dst = -1,
                                   .a = destination,
                                   .b = result,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1});
  int result_pair = emit_load_i64(f, destination);
  if (result_pair < 0)
    return false;
  nyr_opt_ret(f, result_pair);
  return nyr_opt_verify(f, "tv-smt-f32-vec4-fma-rounding");
}

static bool checks_f32_vec4_fma_rounding(void) {
  nyr_func_t fused = {0}, separate = {0}, cfg_fused = {0}, cfg_separate = {0};
  bool built = build_f32_vec4_fma_rounding(&fused, true, false) &&
               build_f32_vec4_fma_rounding(&separate, false, false) &&
               build_f32_vec4_fma_rounding(&cfg_fused, true, true) &&
               build_f32_vec4_fma_rounding(&cfg_separate, false, true);
  char err[256] = {0};
  bool distinguishes =
      built && !nyr_tv_smt_equiv(&fused, &separate, err, sizeof(err));
  err[0] = '\0';
  bool cfg_distinguishes =
      built && !nyr_tv_smt_equiv(&cfg_fused, &cfg_separate, err, sizeof(err));
  uint32_t vm_memory[4] = {0};
  int64_t vm_inputs[1] = {(int64_t)(uintptr_t)vm_memory};
  nyr_eval_result_t fused_eval = {0}, separate_eval = {0};
  nyr_eval_result_t cfg_fused_eval = {0}, cfg_separate_eval = {0};
  char eval_err[192] = {0};
  bool bounded_distinguishes =
      built &&
      nyr_ir_eval(&fused, vm_inputs, 1, 1000, &fused_eval, eval_err,
                sizeof(eval_err)) &&
      nyr_ir_eval(&separate, vm_inputs, 1, 1000, &separate_eval, eval_err,
                sizeof(eval_err)) &&
      nyr_ir_eval(&cfg_fused, vm_inputs, 1, 1000, &cfg_fused_eval, eval_err,
                sizeof(eval_err)) &&
      nyr_ir_eval(&cfg_separate, vm_inputs, 1, 1000, &cfg_separate_eval, eval_err,
                sizeof(eval_err)) &&
      fused_eval.returned && separate_eval.returned &&
      cfg_fused_eval.returned && cfg_separate_eval.returned &&
      fused_eval.result != separate_eval.result &&
      cfg_fused_eval.result != cfg_separate_eval.result;
  if (!built || !distinguishes || !cfg_distinguishes || !bounded_distinguishes)
    fprintf(stderr,
            "[tv-smt-f32-vec4-fma-rounding] built=%s straight=%s cfg=%s "
            "bounded=%s: %s\n",
            built ? "yes" : "no", distinguishes ? "distinguished" : "bad",
            cfg_distinguishes ? "distinguished" : "bad",
            bounded_distinguishes ? "distinguished" : "bad",
            eval_err[0] ? eval_err
            : err[0]
                ? err
                : "fused and separate f32 rounding were not distinguished");
  nyr_eval_result_free(&fused_eval);
  nyr_eval_result_free(&separate_eval);
  nyr_eval_result_free(&cfg_fused_eval);
  nyr_eval_result_free(&cfg_separate_eval);
  nyr_ir_func_free(&fused);
  nyr_ir_func_free(&separate);
  nyr_ir_func_free(&cfg_fused);
  nyr_ir_func_free(&cfg_separate);
  return built && distinguishes && cfg_distinguishes && bounded_distinguishes;
}

static bool build_f32_vec4_shuffle_result(nyr_func_t *f, bool vectorized,
                                          int64_t mask, bool cfg) {
  static const uint32_t input_bits[4] = {
      UINT32_C(0x3f800000), UINT32_C(0x40000000), UINT32_C(0x40400000),
      UINT32_C(0x40800000)};
  static const uint32_t output_bits[4] = {
      UINT32_C(0x40800000), UINT32_C(0x40400000), UINT32_C(0x40000000),
      UINT32_C(0x3f800000)};
  nyr_opt_begin(f);
  if (cfg) {
    nyr_opt_br(f, 13);
    nyr_opt_label(f, 13);
  }
  int destination = load_local(f, 0);
  if (destination < 0)
    return false;
  if (vectorized) {
    for (unsigned lane = 0; lane < 4; ++lane) {
      int value = emit_f32_constant(f, input_bits[lane]);
      if (value < 0)
        return false;
      for (unsigned byte = 0; byte < 4; ++byte)
        if (emit_f32_byte_store(f, destination, lane * 4, value, byte) < 0)
          return false;
    }
    int input = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_LOAD_F32,
                                           .dst = -1,
                                           .a = destination,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
    int shuffled = input < 0
                       ? -1
                       : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_SHUFFLE_F32,
                                                    .dst = -1,
                                                    .a = input,
                                                    .b = -1,
                                                    .c = -1,
                                                    .d = -1,
                                                    .e = -1,
                                                    .f = -1,
                                                    .imm = mask});
    if (shuffled < 0)
      return false;
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_VEC8_STORE_F32,
                                     .dst = -1,
                                     .a = destination,
                                     .b = shuffled,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
  } else {
    for (unsigned lane = 0; lane < 4; ++lane) {
      int value = emit_f32_constant(f, output_bits[lane]);
      if (value < 0)
        return false;
      for (unsigned byte = 0; byte < 4; ++byte)
        if (emit_f32_byte_store(f, destination, lane * 4, value, byte) < 0)
          return false;
    }
  }
  int eight = nyr_opt_const(f, 8);
  int second_address =
      eight < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, destination, eight);
  int low = emit_load_i64(f, destination);
  int high = emit_load_i64(f, second_address);
  uint64_t expected_low = ((uint64_t)output_bits[1] << 32) | output_bits[0];
  uint64_t expected_high = ((uint64_t)output_bits[3] << 32) | output_bits[2];
  int expected_lo = nyr_opt_const(f, (int64_t)expected_low);
  int expected_hi = nyr_opt_const(f, (int64_t)expected_high);
  int zero = nyr_opt_const(f, 0);
  int low_delta = low < 0 || expected_lo < 0
                      ? -1
                      : nyr_opt_binop(f, NYIR_XOR_I64, low, expected_lo);
  int high_delta = high < 0 || expected_hi < 0
                       ? -1
                       : nyr_opt_binop(f, NYIR_XOR_I64, high, expected_hi);
  int low_ok = low_delta < 0 || zero < 0
                   ? -1
                   : nyr_opt_cmp(f, low_delta, zero, NYIR_CMP_EQ);
  int high_ok = high_delta < 0 || zero < 0
                    ? -1
                    : nyr_opt_cmp(f, high_delta, zero, NYIR_CMP_EQ);
  int ok = low_ok < 0 || high_ok < 0
               ? -1
               : nyr_opt_binop(f, NYIR_AND_I64, low_ok, high_ok);
  if (ok < 0)
    return false;
  nyr_opt_ret(f, ok);
  return nyr_opt_verify(f, "tv-smt-f32-vec4-shuffle");
}

static bool checks_f32_vec4_shuffle_semantics(void) {
  nyr_func_t vector = {0}, reference = {0}, wrong = {0};
  nyr_func_t cfg_vector = {0}, cfg_reference = {0}, cfg_wrong = {0};
  nyr_func_t identity = {0};
  bool built = build_f32_vec4_shuffle_result(&vector, true, 187, false) &&
               build_f32_vec4_shuffle_result(&reference, false, 0, false) &&
               build_f32_vec4_shuffle_result(&wrong, true, 0, false) &&
               build_f32_vec4_shuffle_result(&cfg_vector, true, 187, true) &&
               build_f32_vec4_shuffle_result(&cfg_reference, false, 0, true) &&
               build_f32_vec4_shuffle_result(&cfg_wrong, true, 0, true) &&
               build_f32_vec4_shuffle_result(&identity, true, 68, false);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &reference, err, sizeof(err));
  err[0] = '\0';
  bool rejects_wrong =
      built && !nyr_tv_smt_equiv(&vector, &wrong, err, sizeof(err));
  err[0] = '\0';
  bool cfg_equivalent =
      built && nyr_tv_smt_equiv(&cfg_vector, &cfg_reference, err, sizeof(err));
  err[0] = '\0';
  bool cfg_rejects =
      built && !nyr_tv_smt_equiv(&cfg_vector, &cfg_wrong, err, sizeof(err));
  uint32_t memory[4] = {0};
  int64_t inputs[1] = {(int64_t)(uintptr_t)memory};
  nyr_eval_result_t vector_eval = {0}, reference_eval = {0};
  nyr_eval_result_t wrong_eval = {0}, identity_eval = {0};
  char eval_err[192] = {0};
  bool bounded_agrees = built &&
                        nyr_ir_eval(&vector, inputs, 1, 1000, &vector_eval,
                                  eval_err, sizeof(eval_err)) &&
                        nyr_ir_eval(&reference, inputs, 1, 1000, &reference_eval,
                                  eval_err, sizeof(eval_err)) &&
                        nyr_ir_eval(&wrong, inputs, 1, 1000, &wrong_eval,
                                  eval_err, sizeof(eval_err)) &&
                        nyr_ir_eval(&identity, inputs, 1, 1000, &identity_eval,
                                  eval_err, sizeof(eval_err)) &&
                        vector_eval.returned && reference_eval.returned &&
                        wrong_eval.returned && identity_eval.returned &&
                        vector_eval.result == 1 &&
                        vector_eval.result == reference_eval.result &&
                        wrong_eval.result == 0 && identity_eval.result == 0;
  if (!built || !equivalent || !rejects_wrong || !cfg_equivalent ||
      !cfg_rejects || !bounded_agrees)
    fprintf(stderr,
            "[tv-smt-f32-vec4-shuffle] built=%s equivalent=%s rejects=%s "
            "cfg=%s/%s bounded=%s: %s\n",
            built ? "yes" : "no", equivalent ? "yes" : "no",
            rejects_wrong ? "yes" : "no", cfg_equivalent ? "equivalent" : "bad",
            cfg_rejects ? "reject" : "bad", bounded_agrees ? "agrees" : "bad",
            eval_err[0] ? eval_err
            : err[0]    ? err
                        : "four-lane f32 shuffle mismatch");
  nyr_eval_result_free(&vector_eval);
  nyr_eval_result_free(&reference_eval);
  nyr_eval_result_free(&wrong_eval);
  nyr_eval_result_free(&identity_eval);
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&reference);
  nyr_ir_func_free(&wrong);
  nyr_ir_func_free(&cfg_vector);
  nyr_ir_func_free(&cfg_reference);
  nyr_ir_func_free(&cfg_wrong);
  nyr_ir_func_free(&identity);
  return built && equivalent && rejects_wrong && cfg_equivalent &&
         cfg_rejects && bounded_agrees;
}

static bool build_discarded_memory_load(nyr_func_t *f, bool keep_second) {
  nyr_opt_begin(f);
  int address = load_local(f, 0);
  int first = address < 0 ? -1
                          : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                       .dst = -1,
                                                       .a = address,
                                                       .b = -1,
                                                       .c = -1,
                                                       .d = -1,
                                                       .e = -1,
                                                       .f = -1});
  if (first < 0)
    return false;
  if (keep_second) {
    int eight = nyr_opt_const(f, 8);
    int second_address =
        eight < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, address, eight);
    int second = second_address < 0
                     ? -1
                     : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                  .dst = -1,
                                                  .a = second_address,
                                                  .b = -1,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1});
    if (second < 0)
      return false;
  }
  nyr_opt_ret(f, first);
  return nyr_opt_verify(f, "tv-smt-discarded-memory-load");
}

static bool checks_memory_backed_vectors(void) {
  nyr_func_t vector = {0}, scalar = {0}, wrong = {0};
  nyr_func_t cfg_vector = {0}, cfg_scalar = {0}, cfg_wrong = {0};
  nyr_func_t with_discarded_load = {0}, without_discarded_load = {0};
  bool built = build_memory_vector_sum(&vector, true, false) &&
               build_memory_vector_sum(&scalar, false, false) &&
               build_memory_vector_sum(&wrong, false, true) &&
               build_cfg_memory_vector_sum(&cfg_vector, true, false) &&
               build_cfg_memory_vector_sum(&cfg_scalar, false, false) &&
               build_cfg_memory_vector_sum(&cfg_wrong, false, true) &&
               build_discarded_memory_load(&with_discarded_load, true) &&
               build_discarded_memory_load(&without_discarded_load, false);
  char err[256] = {0};
  bool equivalent =
      built && nyr_tv_smt_equiv(&vector, &scalar, err, sizeof(err));
  err[0] = '\0';
  bool rejects = built && !nyr_tv_smt_equiv(&vector, &wrong, err, sizeof(err));
  err[0] = '\0';
  bool cfg_equivalent =
      built && nyr_tv_smt_equiv(&cfg_vector, &cfg_scalar, err, sizeof(err));
  err[0] = '\0';
  bool cfg_rejects =
      built && !nyr_tv_smt_equiv(&cfg_vector, &cfg_wrong, err, sizeof(err));
  err[0] = '\0';
  bool rejects_erased_trap =
      built && !nyr_tv_smt_equiv(&with_discarded_load, &without_discarded_load,
                                  err, sizeof(err));
  if (!built || !equivalent || !rejects || !cfg_equivalent || !cfg_rejects ||
      !rejects_erased_trap)
    fprintf(stderr,
            "[tv-smt-memory-vector] built=%s straight=%s/%s cfg=%s/%s "
            "discarded-load=%s: %s\n",
            built ? "yes" : "no", equivalent ? "equiv" : "bad",
            rejects ? "reject" : "bad", cfg_equivalent ? "equiv" : "bad",
            cfg_rejects ? "reject" : "bad",
            rejects_erased_trap ? "reject" : "bad",
            err[0] ? err : "symbolic memory lane mismatch");
  nyr_ir_func_free(&vector);
  nyr_ir_func_free(&scalar);
  nyr_ir_func_free(&wrong);
  nyr_ir_func_free(&cfg_vector);
  nyr_ir_func_free(&cfg_scalar);
  nyr_ir_func_free(&cfg_wrong);
  nyr_ir_func_free(&with_discarded_load);
  nyr_ir_func_free(&without_discarded_load);
  return built && equivalent && rejects && cfg_equivalent && cfg_rejects &&
         rejects_erased_trap;
}

static double monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return -1.0;
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/*
 * The TODO requires cold solver and fallback cost to be measured separately.
 * Report the first (cold) solver-backed equivalence, a warm repeat, and the
 * interpreter fallback over the same pair.
 */
static void report_costs(void) {
  nyr_func_t a = {0}, b = {0};
  if (!build_straightline_float(&a, 0) || !build_straightline_float(&b, 1))
    return;
  char err[256] = {0};
  double t0 = monotonic_ms();
  nyr_tv_smt_equiv(&a, &b, err, sizeof(err));
  double t1 = monotonic_ms();
  nyr_tv_smt_equiv(&a, &b, err, sizeof(err));
  double t2 = monotonic_ms();
  nyr_tv_equiv_straightline(&a, &b, 64, err, sizeof(err));
  double t3 = monotonic_ms();
  printf("[tv-smt-semantics] cost_ms: cold_solver=%.3f warm_solver=%.3f "
         "interpreter_fallback(64)=%.3f\n",
         t1 - t0, t2 - t1, t3 - t2);
  nyr_ir_func_free(&a);
  nyr_ir_func_free(&b);
}

/*
 * Run every semantics group. Shared by the standalone CLI (no selector)
 * and the generated registry build, which calls the test entry with no
 * arguments.
 */
static int tv_smt_run_all(void) {
  bool ok =
      checks_alloca_extent_and_offsets() && checks_alloca_vec8_extent() &&
      checks_cfg_alloca_extent() && checks_bounded_cfg_allocations() &&
      checks_copy_struct_memory_model() && checks_symbol_address_model() &&
      checks_addr_local_memory_model() && rejects_repeated_cfg_allocations() &&
      checks_alloca_regions_are_disjoint() &&
      checks_local_pointer_provenance_order() &&
      equivalent_shift(NYIR_SHL_I64, 63, "shl count mask") &&
      equivalent_shift(NYIR_SAR_I64, 63, "sar count mask") &&
      equivalent_shift(NYIR_ROR_I64, 63, "ror64 count mask") &&
      equivalent_shift(NYIR_ROR32_I64, 31, "ror32 count mask") &&
      checks_select_i64_smt() && checks_i64_cmp_smt() &&
      checks_i64_arithmetic_smt() && checks_i64_xor_smt() &&
      checks_byte_swap_smt() &&
      rejects_erased_zero_trap(NYIR_DIV_I64, "division by zero") &&
      rejects_erased_zero_trap(NYIR_MOD_I64, "remainder by zero") &&
      checks_bounds_check_smt_semantics() &&
      rejects_changed_trap(NYIR_DIV_I64, "signed division overflow") &&
      rejects_changed_trap(NYIR_MOD_I64, "signed remainder overflow") &&
      rejects_cfg_float_compare_change() && checks_fp_cmp_edge_semantics() &&
      checks_cfg_sqrt() && checks_cfg_transcendental_congruence() &&
      rejects_cfg_float_arithmetic_change(false) &&
      rejects_cfg_float_arithmetic_change(true) && checks_float_to_i64(false) &&
      checks_float_to_i64(true) && checks_f64_f32_roundtrip() &&
      checks_straightline_float_conversions() &&
      checks_straightline_trig_congruence() &&
      checks_cfg_integer_vector_reduction() &&
      checks_external_f64_vec4_binary_smt() &&
      checks_packed_integer_vector_ops() && checks_memory_backed_vectors() &&
      checks_path_local_store_semantics() &&
      checks_external_pointer_aliasing() &&
      keeps_unknown_effect_calls_inapplicable() &&
      checks_known_pure_call_congruence() && checks_capture_ret_model() &&
      checks_readonly_call_memory_congruence() &&
      checks_writable_call_memory_model() &&
      checks_write_may_trap_call_memory_model() &&
      checks_may_trap_call_definedness() &&
      checks_read_memory_may_trap_calls() &&
      keeps_volatile_calls_inapplicable() &&
      rejects_conflicting_call_effect_summaries() &&
      checks_cfg_path_local_store_semantics() &&
      checks_memory_vec8_semantics() && checks_memory_vec8_binary_semantics();
  ok =
      ok && checks_vector_store_semantics(2) &&
      checks_vector_store_semantics(4) && checks_memory_f64_vec4_semantics() &&
      checks_cfg_vector_store_semantics(2) &&
      checks_cfg_vector_store_semantics(4) && checks_f64_vec4_add() &&
      checks_f64_vec4_binary_operations() && checks_cfg_f64_vec4_binary() &&
      checks_f64_vec4_fma() && checks_f64_vec4_fma_distinct_lanes() &&
      checks_f64_fma_rounding() && checks_f32_vec4_memory_semantics() &&
      checks_f32_vec4_fma_semantics() && checks_f32_vec4_fma_distinct_lanes() &&
      checks_f32_vec4_fma_rounding() && checks_f32_vec4_shuffle_semantics() &&
      checks_f64_vector_store_semantics(false) &&
      checks_f64_shuffle_semantics() && checks_f64_vector_store_semantics(true);
  report_costs();
  if (ok)
    puts("[tv-smt-semantics] bounded ALLOCA extents/offsets/disjoint regions, "
         "zero/one/two/three/eight-trip CFG allocations admitted, nine-trip "
         "and unknown cycles inapplicable, "
         "straightline/CFG symbolic addresses and fixed-size aggregate byte "
         "copies, address-taken locals share byte memory in straightline/CFG, "
         "shifts, traps, bounds, CFG sqrt/trig/float, "
         "straightline/CFG scalar/integer/floating vector stores and "
         "four-lane f64 add/subtract/multiply/divide/FMA, "
         "four-lane f32 arithmetic, memory, FMA, shuffle, and fused rounding "
         "in straightline and CFG paths, "
         "byte/full-width aliasing, "
         "same and unknown external-pointer aliases, "
         "pure/read-only/write/read-write call models and memory transitions, "
         "secondary-return congruence, exact argument- and memory-keyed "
         "may-trap call validity, write-capable may-trap calls and memory "
         "transitions, "
         "unknown/volatile/conflicting call effects remain inapplicable, "
         "path-local CFG writes, "
         "two- "
         "and four-lane memory-backed integer vectors, reductions, "
         "conversions, and float-to-i64 semantics match NYIR");
  return ok ? 0 : 1;
  return ok ? 0 : 1;
}

#ifdef main
/*
 * run.cmake defines `main` as the registry entry name and calls it with
 * no arguments; expose the void-prototype run-all entry.
 */
int main(void) { return tv_smt_run_all(); }
#else
int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--alloca-cfg") == 0) {
    bool ok = checks_alloca_extent_and_offsets() &&
              checks_alloca_vec8_extent() && checks_cfg_alloca_extent() &&
              checks_bounded_cfg_allocations() &&
              rejects_repeated_cfg_allocations() &&
              checks_alloca_regions_are_disjoint() &&
              checks_local_pointer_provenance_order();
    if (ok)
      puts(
          "[tv-smt-alloca-cfg] bounded allocation loops and validity verified");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--external-memory") == 0) {
    bool ok = checks_symbol_address_model() &&
              checks_addr_local_memory_model() &&
              checks_path_local_store_semantics() &&
              checks_external_pointer_aliasing();
    if (ok)
      puts("[tv-smt-external-memory] symbolic addresses and pointer aliases "
           "verified");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--call-effects") == 0) {
    bool ok = keeps_unknown_effect_calls_inapplicable() &&
              checks_known_pure_call_congruence() &&
              checks_capture_ret_model() &&
              checks_readonly_call_memory_congruence() &&
              checks_writable_call_memory_model() &&
              checks_write_may_trap_call_memory_model() &&
              checks_may_trap_call_definedness() &&
              checks_read_memory_may_trap_calls() &&
              keeps_volatile_calls_inapplicable() &&
              rejects_conflicting_call_effect_summaries();
    if (ok)
      puts("[tv-smt-call-effects] known, trapping, and unsupported effects "
           "verified");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--write-trap") == 0) {
    bool ok = checks_write_may_trap_call_memory_model();
    if (ok)
      puts("[tv-smt-write-trap-group] modeled known write/trap effects");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--volatile") == 0) {
    bool ok = keeps_volatile_calls_inapplicable();
    if (ok)
      puts("[tv-smt-volatile-group] volatile effects remain inapplicable");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--scalar-opcodes") == 0) {
    bool ok = equivalent_shift(NYIR_SHL_I64, 63, "shl count mask") &&
              equivalent_shift(NYIR_SAR_I64, 63, "sar count mask") &&
              equivalent_shift(NYIR_ROR_I64, 63, "ror64 count mask") &&
              equivalent_shift(NYIR_ROR32_I64, 31, "ror32 count mask") &&
              checks_select_i64_smt() && checks_i64_cmp_smt() &&
              checks_i64_arithmetic_smt() && checks_i64_xor_smt() &&
              checks_byte_swap_smt() &&
              rejects_erased_zero_trap(NYIR_DIV_I64, "division by zero") &&
              rejects_erased_zero_trap(NYIR_MOD_I64, "remainder by zero") &&
              rejects_changed_trap(NYIR_DIV_I64, "signed division overflow") &&
              rejects_changed_trap(NYIR_MOD_I64, "signed remainder overflow") &&
              checks_bounds_check_smt_semantics();
    if (ok)
      puts("[tv-smt-scalar-opcodes] shifts, select, signed comparisons, "
           "wrapping arithmetic, division/remainder traps, bounds, XOR, and "
           "byte swaps match references");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--fp-compare") == 0) {
    bool ok = checks_fp_cmp_edge_semantics();
    if (ok)
      puts("[tv-smt-fp-compare] f32/f64 NaN and signed-zero comparisons match "
           "NYIR");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--f64-cfg") == 0) {
    bool ok = checks_cfg_f64_vec4_binary() && checks_f64_vec4_fma();
    if (ok)
      puts("[tv-smt-f64-cfg-group] four-lane arithmetic and FMA verified");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--f64-memory") == 0) {
    bool ok = checks_f64_vec4_binary_operations() &&
              checks_f64_vec4_fma_distinct_lanes();
    if (ok)
      puts("[tv-smt-f64-memory-group] four-lane distinct-memory arithmetic "
           "and FMA agree with scalar oracles");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--f64-binary-memory-tv") == 0) {
    bool ok = checks_external_f64_vec4_binary_smt();
    if (ok)
      puts(
          "[tv-smt-f64-binary-memory] four-lane and scalar-memory proofs pass");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--f64-scalar-memory-tv") == 0) {
    bool ok = checks_memory_f64_vec4_semantics();
    if (ok)
      puts("[tv-smt-f64-scalar-memory] vector memory agrees with scalar lanes "
           "and rejects an omitted lane");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--f64-binary-memory") == 0) {
    bool ok = checks_f64_vec4_binary_operations();
    if (ok)
      puts("[tv-smt-f64-binary-memory-group] four-lane arithmetic verified");
    return ok ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "--f64-fma-memory") == 0) {
    bool ok = checks_f64_vec4_fma_distinct_lanes();
    if (ok)
      puts("[tv-smt-f64-fma-memory-group] distinct-lane FMA verified");
    return ok ? 0 : 1;
  }
  if (argc != 1) {
    fprintf(stderr, "usage: tv_smt_semantics "
                    "[--alloca-cfg|--external-memory|--call-effects|"
                    "--write-trap|--volatile|--scalar-opcodes|"
                    "--fp-compare|--f64-cfg|--f64-memory|"
                    "--f64-binary-memory-tv|"
                    "--f64-scalar-memory-tv|"
                    "--f64-binary-memory|--f64-fma-memory]\n");
    return 2;
  }
  return tv_smt_run_all();
}
#endif
