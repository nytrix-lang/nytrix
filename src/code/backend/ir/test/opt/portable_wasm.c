/*
 * The portable WebAssembly emitter must preserve NYIR's low-32 rotate
 * semantics instead of falling back to unsupported i64 operations.
 */
#include "code/backend/internal.h"
#include "support.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef bool (*portable_emitter_fn)(nyr_writer_t *,
                                    const ny_native_target_info_t *,
                                    const nyr_func_t *, const char *, bool,
                                    char *, size_t);

static bool check_case(int64_t value, int64_t count, int64_t expected) {
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int value_id = nyr_opt_const(&ir, value);
  int count_id = nyr_opt_const(&ir, count);
  int rotated = nyr_opt_binop(&ir, NYIR_ROR32_I64, value_id, count_id);
  nyr_opt_ret(&ir, rotated);

  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_WASM,
      .abi = NY_NATIVE_ABI_SYSV,
      .target_name = "wasm",
      .object_format = "elf",
      .symbol_prefix = "",
  };
  nyr_writer_t wat = {0};
  char err[256] = {0};
  bool ok =
      nyr_opt_verify(&ir, "portable-wasm-ror32") &&
      nyr_opt_eval(&ir, "portable-wasm-ror32") == expected &&
      nyr_wasm_emit_ir(&wat, &target, &ir, "ror32", false, err, sizeof(err));
  if (ok) {
    const char *first_wrap = wat.data ? strstr(wat.data, "i32.wrap_i64") : NULL;
    const char *second_wrap =
        first_wrap ? strstr(first_wrap + 1, "i32.wrap_i64") : NULL;
    const char *rotate =
        second_wrap ? strstr(second_wrap + 1, "i32.rotr") : NULL;
    const char *extend = rotate ? strstr(rotate + 1, "i64.extend_i32_u") : NULL;
    ok = first_wrap && second_wrap && rotate && extend;
  }
  if (!ok)
    fprintf(stderr, "[portable-wasm] ROR32 value=%lld count=%lld failed: %s\n",
            (long long)value, (long long)count,
            err[0] ? err : "missing WebAssembly rotate sequence");
  free(wat.data);
  nyr_ir_func_free(&ir);
  return ok;
}

static bool check_import_signatures(void) {
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int scalar = nyr_opt_const_f64(&ir, 1.0);
  int vector = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                            .dst = -1,
                                            .a = scalar,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  int pointer =
      nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .symbol = "wasm_import_pointer",
                                   .scalar_class = NYIR_SCALAR_POINTER});
  int pointer_copy = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_COPY,
                                                  .dst = -1,
                                                  .a = pointer,
                                                  .b = -1,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1});
  nyr_opt_ret(&ir, scalar);

  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = vector,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = 1,
                      .symbol = "host_vector"};
  nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_F64);
  int args[1] = {vector};
  nyr_wasm_import_type_t arg_type = NYR_WASM_IMPORT_UNSUPPORTED;
  nyr_wasm_import_type_t result_type = NYR_WASM_IMPORT_UNSUPPORTED;
  bool vector_rejected =
      vector >= 0 &&
      !nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type);

  args[0] = scalar;
  nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_AGGREGATE);
  bool aggregate_arg_rejected =
      scalar >= 0 &&
      !nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type);

  uint32_t aggregate_arg_size =
      8u | (NYIR_ARG_CLASS_SSE << NYIR_ARG_AGG_CLASS0_SHIFT);
  call.a = pointer_copy;
  call.arg_sizes = &aggregate_arg_size;
  nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_AGGREGATE);
  args[0] = pointer_copy;
  bool aggregate_sse_arg_supported =
      pointer >= 0 && pointer_copy >= 0 &&
      nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type) &&
      arg_type == NYR_WASM_IMPORT_F64 && result_type == NYR_WASM_IMPORT_VOID;

  aggregate_arg_size =
      8u | (NYIR_ARG_CLASS_INTEGER << NYIR_ARG_AGG_CLASS0_SHIFT);
  bool aggregate_integer_arg_supported =
      nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type) &&
      arg_type == NYR_WASM_IMPORT_I64;

  aggregate_arg_size =
      16u | (NYIR_ARG_CLASS_MEMORY << NYIR_ARG_AGG_CLASS0_SHIFT);
  bool aggregate_memory_arg_supported =
      nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type) &&
      arg_type == NYR_WASM_IMPORT_I32;

  aggregate_arg_size = 16u | (NYIR_ARG_CLASS_SSE << NYIR_ARG_AGG_CLASS0_SHIFT) |
                       (NYIR_ARG_CLASS_SSE << NYIR_ARG_AGG_CLASS1_SHIFT);
  bool aggregate_multiregister_arg_rejected =
      !nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type);
  call.arg_sizes = NULL;

  call.dst = scalar;
  call.a = -1;
  call.imm = 0;
  call.scalar_class = NYIR_SCALAR_AGGREGATE;
  call.ret_aggregate_size = 16;
  call.flags = NYIR_INST_F_EXTERN | NYIR_INST_F_SRET;
  bool aggregate_result_rejected =
      scalar >= 0 &&
      !nyr_wasm_call_signature(&ir, &call, args, 0, &arg_type, &result_type);

  call.scalar_class = NYIR_SCALAR_F64;
  call.ret_aggregate_size = 8;
  call.ret_aggregate_classes[0] = NYIR_ARG_CLASS_SSE;
  call.ret_aggregate_classes[1] = NYIR_ARG_CLASS_NONE;
  call.flags = NYIR_INST_F_EXTERN;
  bool aggregate_sse_result_supported =
      scalar >= 0 &&
      nyr_wasm_call_signature(&ir, &call, args, 0, &arg_type, &result_type) &&
      result_type == NYR_WASM_IMPORT_F64;

  call.scalar_class = NYIR_SCALAR_AGGREGATE;
  call.ret_aggregate_size = 8;
  call.ret_aggregate_classes[0] = NYIR_ARG_CLASS_INTEGER;
  bool aggregate_integer_result_supported =
      scalar >= 0 &&
      nyr_wasm_call_signature(&ir, &call, args, 0, &arg_type, &result_type) &&
      result_type == NYR_WASM_IMPORT_I64;
  call.ret_aggregate_size = 16;
  call.ret_aggregate_classes[0] = NYIR_ARG_CLASS_SSE;
  call.ret_aggregate_classes[1] = NYIR_ARG_CLASS_SSE;
  bool aggregate_multivalue_result_rejected =
      !nyr_wasm_call_signature(&ir, &call, args, 0, &arg_type, &result_type);

  call.a = scalar;
  call.imm = 1;
  call.scalar_class = NYIR_SCALAR_OBJECT;
  call.ret_aggregate_size = 0;
  call.flags = NYIR_INST_F_EXTERN | NYIR_INST_F_SRET;
  nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_POINTER);
  bool sret_import_supported =
      scalar >= 0 &&
      nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type) &&
      arg_type == NYR_WASM_IMPORT_I32 && result_type == NYR_WASM_IMPORT_VOID;

  nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_I64_RAW);
  bool sret_nonpointer_rejected =
      scalar >= 0 &&
      !nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type);

  call.a = pointer_copy;
  memset(call.arg_scalar_classes, 0, sizeof(call.arg_scalar_classes));
  args[0] = pointer_copy;
  bool inferred_sret_import_supported =
      pointer >= 0 && pointer_copy >= 0 &&
      nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type) &&
      arg_type == NYR_WASM_IMPORT_I32 && result_type == NYR_WASM_IMPORT_VOID;

  call.dst = scalar;
  call.a = pointer_copy;
  call.imm = 1;
  call.scalar_class = NYIR_SCALAR_F64;
  call.ret_aggregate_size = 0;
  call.flags = NYIR_INST_F_EXTERN;
  memset(call.arg_scalar_classes, 0, sizeof(call.arg_scalar_classes));
  args[0] = pointer_copy;
  bool inferred_pointer_import_supported =
      pointer >= 0 && pointer_copy >= 0 &&
      nyr_wasm_call_signature(&ir, &call, args, 1, &arg_type, &result_type) &&
      arg_type == NYR_WASM_IMPORT_I32 && result_type == NYR_WASM_IMPORT_F64;

  if (!vector_rejected || !aggregate_arg_rejected ||
      !aggregate_sse_arg_supported || !aggregate_integer_arg_supported ||
      !aggregate_memory_arg_supported ||
      !aggregate_multiregister_arg_rejected || !aggregate_result_rejected ||
      !aggregate_sse_result_supported || !aggregate_integer_result_supported ||
      !aggregate_multivalue_result_rejected || !sret_import_supported ||
      !sret_nonpointer_rejected || !inferred_sret_import_supported ||
      !inferred_pointer_import_supported)
    fprintf(stderr, "[portable-wasm] import signature boundary failed\n");
  nyr_ir_func_free(&ir);
  return vector_rejected && aggregate_arg_rejected &&
         aggregate_sse_arg_supported && aggregate_integer_arg_supported &&
         aggregate_memory_arg_supported &&
         aggregate_multiregister_arg_rejected && aggregate_result_rejected &&
         aggregate_sse_result_supported && aggregate_integer_result_supported &&
         aggregate_multivalue_result_rejected && sret_import_supported &&
         sret_nonpointer_rejected && inferred_sret_import_supported &&
         inferred_pointer_import_supported;
}

static bool check_wasm_bounds_check(void) {
  const nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_WASM, NY_NATIVE_ABI_AUTO, NULL, false);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &request))
    return false;

  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int base = nyr_opt_const(&ir, 4096);
  int offset = nyr_opt_const(&ir, 8);
  int length = nyr_opt_const(&ir, 64);
  nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                               .dst = -1,
                               .a = base,
                               .b = offset,
                               .c = length,
                               .d = -1,
                               .e = -1,
                               .f = -1});
  nyr_opt_ret(&ir, base);

  nyr_writer_t wat = {0};
  char err[256] = {0};
  bool ok =
      nyr_ir_verify(&ir, err, sizeof(err)) &&
      nyr_wasm_emit_ir(&wat, &target, &ir, "bounds", false, err, sizeof(err)) &&
      wat.data && strstr(wat.data, "i64.ge_s") &&
      strstr(wat.data, "unreachable");
  if (!ok)
    fprintf(stderr, "[portable-wasm] bounds check emission failed: %s\n",
            err[0] ? err : "missing signed range checks");
  free(wat.data);
  nyr_ir_func_free(&ir);
  return ok;
}

static bool check_bounds_rejected(nyr_backend_t backend,
                                  portable_emitter_fn emit, const char *name) {
  const nyr_target_request_t request =
      nyr_target_request_make(backend, NY_NATIVE_ABI_AUTO, NULL, false);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &request)) {
    fprintf(stderr, "[portable-wasm] target init failed for %s\n", name);
    return false;
  }

  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int base = nyr_opt_const(&ir, 0);
  int offset = nyr_opt_const(&ir, 0);
  int bytes = nyr_opt_const(&ir, 8);
  nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                               .dst = -1,
                               .a = base,
                               .b = offset,
                               .c = bytes,
                               .d = -1,
                               .e = -1,
                               .f = -1});
  nyr_opt_ret(&ir, base);

  nyr_writer_t output = {0};
  char err[256] = {0};
  bool rejected = nyr_ir_verify(&ir, err, sizeof(err)) &&
                  !emit(&output, &target, &ir, name, false, err, sizeof(err)) &&
                  strstr(err, "bounds checks are unsupported") != NULL;
  if (!rejected)
    fprintf(stderr, "[portable-wasm] %s did not reject bounds check: %s\n",
            name, err[0] ? err : "unexpected success");
  free(output.data);
  nyr_ir_func_free(&ir);
  return rejected;
}

static bool check_bounds_supported(nyr_backend_t backend,
                                   portable_emitter_fn emit, const char *name,
                                   const char *first, const char *second,
                                   const char *trap) {
  const nyr_target_request_t request =
      nyr_target_request_make(backend, NY_NATIVE_ABI_AUTO, NULL, false);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &request)) {
    fprintf(stderr, "[portable-wasm] target init failed for %s\n", name);
    return false;
  }

  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int base = nyr_opt_const(&ir, 0);
  int offset = nyr_opt_const(&ir, 0);
  int bytes = nyr_opt_const(&ir, 8);
  nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                               .dst = -1,
                               .a = base,
                               .b = offset,
                               .c = bytes,
                               .d = -1,
                               .e = -1,
                               .f = -1});
  nyr_opt_ret(&ir, base);

  nyr_writer_t output = {0};
  char err[256] = {0};
  bool ok = nyr_ir_verify(&ir, err, sizeof(err)) &&
            emit(&output, &target, &ir, name, false, err, sizeof(err)) &&
            output.data && strstr(output.data, first) &&
            strstr(output.data, second) && strstr(output.data, trap);
  if (!ok)
    fprintf(stderr, "[portable-wasm] %s bounds lowering failed: %s\n", name,
            err[0] ? err : "missing comparison or trap sequence");
  free(output.data);
  nyr_ir_func_free(&ir);
  return ok;
}

static bool check_executable_wat(void) {
#ifdef _WIN32
  fprintf(stderr,
          "[portable-wasm] skipping runtime smoke: external runner check "
          "uses POSIX command lookup\n");
  return true;
#else
  if (system("command -v wat2wasm >/dev/null 2>&1 && "
             "command -v wasmtime >/dev/null 2>&1") != 0) {
    fprintf(stderr, "[portable-wasm] skipping runtime smoke: wat2wasm/wasmtime "
                    "unavailable\n");
    return true;
  }

  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_WASM,
      .abi = NY_NATIVE_ABI_SYSV,
      .target_name = "wasm",
      .object_format = "elf",
      .symbol_prefix = "",
  };
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int condition = nyr_opt_const(&ir, 1);
  nyr_opt_br_if(&ir, condition, 10);
  int untaken = nyr_opt_const(&ir, 11);
  nyr_opt_ret(&ir, untaken);
  nyr_opt_label(&ir, 10);
  int source = nyr_opt_const(&ir, INT64_C(0x01234567));
  int swapped = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_BSWAP32_I64,
                                             .dst = -1,
                                             .a = source,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1});
  int source64 = nyr_opt_const(&ir, (int64_t)UINT64_C(0x0123456789abcdef));
  int swapped64 = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_BSWAP_I64,
                                               .dst = -1,
                                               .a = source64,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
  int mixed = nyr_opt_binop(&ir, NYIR_XOR_I64, swapped, swapped64);
  int mask = nyr_opt_const(&ir, (int64_t)(UINT64_C(0xefcdab8967452301) ^
                                          UINT64_C(0x67452301) ^ UINT64_C(42)));
  int answer = nyr_opt_binop(&ir, NYIR_XOR_I64, mixed, mask);
  int address64 = nyr_opt_const(&ir, 16);
  nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = address64,
                               .b = -1,
                               .c = answer,
                               .d = -1,
                               .e = -1,
                               .f = -1});
  int loaded64 = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                              .dst = -1,
                                              .a = address64,
                                              .b = -1,
                                              .c = -1,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1});
  int address8 = nyr_opt_const(&ir, 24);
  int byte_value = nyr_opt_const(&ir, 90);
  nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = address8,
                               .b = -1,
                               .c = byte_value,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .flags = NYIR_INST_F_MEM_BYTE});
  int loaded8 = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                             .dst = -1,
                                             .a = address8,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .flags = NYIR_INST_F_MEM_BYTE});
  int result = nyr_opt_binop(&ir, NYIR_ADD_I64, loaded64, loaded8);
  nyr_opt_ret(&ir, result);

  nyr_writer_t wat = {0};
  char err[256] = {0};
  bool ok =
      nyr_opt_verify(&ir, "portable-wasm-executable") &&
      nyr_put(&wat, "(module\n(memory 1)\n") &&
      nyr_wasm_emit_ir(&wat, &target, &ir, "rt_main", true, err, sizeof(err)) &&
      nyr_put(&wat, ")\n");
  char wat_path[128], wasm_path[128], result_path[128], command[512];
  snprintf(wat_path, sizeof(wat_path), "/tmp/nyr-wasm-%ld.wat", (long)getpid());
  snprintf(wasm_path, sizeof(wasm_path), "/tmp/nyr-wasm-%ld.wasm",
           (long)getpid());
  snprintf(result_path, sizeof(result_path), "/tmp/nyr-wasm-%ld.out",
           (long)getpid());
  FILE *file = ok ? fopen(wat_path, "wb") : NULL;
  if (ok && !file)
    ok = false;
  if (file) {
    if (fwrite(wat.data, 1, wat.len, file) != wat.len)
      ok = false;
    if (fclose(file) != 0)
      ok = false;
  }
  if (ok) {
    snprintf(command, sizeof(command), "wat2wasm %s -o %s", wat_path,
             wasm_path);
    ok = system(command) == 0;
  }
  if (ok) {
    snprintf(command, sizeof(command),
             "wasmtime run --invoke rt_main %s >%s 2>/dev/null", wasm_path,
             result_path);
    ok = system(command) == 0;
  }
  if (ok) {
    file = fopen(result_path, "r");
    long result = 0;
    char extra = '\0';
    ok = file && fscanf(file, "%ld%c", &result, &extra) == 2 && result == 265 &&
         extra == '\n';
    if (file)
      fclose(file);
  }

  if (!ok)
    fprintf(stderr, "[portable-wasm] executable WAT smoke failed: %s\n",
            err[0] ? err : "module assembly or runtime result mismatch");
  unlink(wat_path);
  unlink(wasm_path);
  unlink(result_path);
  free(wat.data);
  nyr_ir_func_free(&ir);
  return ok;
#endif
}

static bool check_wasm_float_ops(void) {
#ifdef _WIN32
  return true;
#else
  if (system("command -v wat2wasm >/dev/null 2>&1 && "
             "command -v wasmtime >/dev/null 2>&1") != 0)
    return true;

  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_WASM,
      .abi = NY_NATIVE_ABI_SYSV,
      .target_name = "wasm",
      .object_format = "elf",
      .symbol_prefix = "",
  };
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int i64_five = nyr_opt_const(&ir, 5);
  int f64_one_half = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_CONST_F64,
                         .dst = -1,
                         .imm = (int64_t)UINT64_C(0x3ff8000000000000)});
  int f64_two = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_CONST_F64,
                         .dst = -1,
                         .imm = (int64_t)UINT64_C(0x4000000000000000)});
  int f64_sum = nyr_ir_emit(&ir, (nyr_inst_t){
                                   .op = NYIR_ADD_F64,
                                   .dst = -1,
                                   .a = f64_one_half,
                                   .b = f64_two,
                               });
  int f64_sum_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F64_TO_I64, .dst = -1, .a = f64_sum});
  int f64_diff = nyr_ir_emit(&ir, (nyr_inst_t){
                                    .op = NYIR_SUB_F64,
                                    .dst = -1,
                                    .a = f64_two,
                                    .b = f64_one_half,
                                });
  int f64_diff_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F64_TO_I64, .dst = -1, .a = f64_diff});
  int f64_product = nyr_ir_emit(&ir, (nyr_inst_t){
                                       .op = NYIR_MUL_F64,
                                       .dst = -1,
                                       .a = f64_one_half,
                                       .b = f64_two,
                                   });
  int f64_product_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F64_TO_I64, .dst = -1, .a = f64_product});
  int f64_quotient = nyr_ir_emit(&ir, (nyr_inst_t){
                                        .op = NYIR_DIV_F64,
                                        .dst = -1,
                                        .a = f64_two,
                                        .b = f64_one_half,
                                    });
  int f64_quotient_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F64_TO_I64, .dst = -1, .a = f64_quotient});
  int f64_less = nyr_ir_emit(&ir, (nyr_inst_t){
                                    .op = NYIR_CMP_F64,
                                    .dst = -1,
                                    .a = f64_one_half,
                                    .b = f64_two,
                                    .cmp = NYIR_CMP_LT,
                                });
  int f64_from_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_I64_TO_F64, .dst = -1, .a = i64_five});
  int f64_to_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F64_TO_I64, .dst = -1, .a = f64_from_i64});
  int f32_one_half =
      nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_CONST_F32,
                                   .dst = -1,
                                   .imm = (int64_t)UINT32_C(0x3fc00000)});
  int f32_two =
      nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_CONST_F32,
                                   .dst = -1,
                                   .imm = (int64_t)UINT32_C(0x40000000)});
  int f32_sum = nyr_ir_emit(&ir, (nyr_inst_t){
                                   .op = NYIR_ADD_F32,
                                   .dst = -1,
                                   .a = f32_one_half,
                                   .b = f32_two,
                               });
  int f32_sum_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F32_TO_I64, .dst = -1, .a = f32_sum});
  int f32_less = nyr_ir_emit(&ir, (nyr_inst_t){
                                    .op = NYIR_CMP_F32,
                                    .dst = -1,
                                    .a = f32_one_half,
                                    .b = f32_two,
                                    .cmp = NYIR_CMP_LT,
                                });
  int f32_from_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_I64_TO_F32, .dst = -1, .a = i64_five});
  int f32_to_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F32_TO_I64, .dst = -1, .a = f32_from_i64});
  int f32_to_f64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F32_TO_F64, .dst = -1, .a = f32_one_half});
  int f64_to_f32 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F64_TO_F32, .dst = -1, .a = f64_two});
  int f64_to_f32_to_f64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F32_TO_F64, .dst = -1, .a = f64_to_f32});
  int f32_to_f64_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F64_TO_I64, .dst = -1, .a = f32_to_f64});
  int f64_to_f32_i64 = nyr_ir_emit(
      &ir,
      (nyr_inst_t){.op = NYIR_F64_TO_I64, .dst = -1, .a = f64_to_f32_to_f64});
  int sqrt_two = nyr_ir_emit(&ir, (nyr_inst_t){
                                    .op = NYIR_SQRT_F64,
                                    .dst = -1,
                                    .a = f64_two,
                                });
  int sqrt_two_i64 = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = NYIR_F64_TO_I64, .dst = -1, .a = sqrt_two});
  int terms[] = {f64_sum_i64, f64_diff_i64,   f64_product_i64, f64_quotient_i64,
                 f64_less,    f64_to_i64,     f32_sum_i64,     f32_less,
                 f32_to_i64,  f32_to_f64_i64, f64_to_f32_i64,  sqrt_two_i64};
  int result = nyr_opt_const(&ir, 0);
  for (size_t i = 0; result >= 0 && i < sizeof(terms) / sizeof(terms[0]); ++i) {
    if (terms[i] < 0) {
      result = -1;
      break;
    }
    result = nyr_opt_binop(&ir, NYIR_ADD_I64, result, terms[i]);
  }
  nyr_opt_ret(&ir, result);

  nyr_writer_t wat = {0};
  char err[256] = {0};
  bool ok =
      result >= 0 && nyr_opt_verify(&ir, "portable-wasm-float-ops") &&
      nyr_wasm_emit_ir(&wat, &target, &ir, "rt_main", true, err, sizeof(err)) &&
      nyr_put(&wat, "\n");
  char wat_path[128], wasm_path[128], result_path[128], command[512];
  snprintf(wat_path, sizeof(wat_path), "/tmp/nyr-wasm-float-%ld.wat",
           (long)getpid());
  snprintf(wasm_path, sizeof(wasm_path), "/tmp/nyr-wasm-float-%ld.wasm",
           (long)getpid());
  snprintf(result_path, sizeof(result_path), "/tmp/nyr-wasm-float-%ld.out",
           (long)getpid());
  FILE *file = ok ? fopen(wat_path, "wb") : NULL;
  if (ok && !file)
    ok = false;
  if (file) {
    if (fwrite(wat.data, 1, wat.len, file) != wat.len)
      ok = false;
    if (fclose(file) != 0)
      ok = false;
  }
  if (ok) {
    snprintf(command, sizeof(command), "wat2wasm %s -o %s", wat_path,
             wasm_path);
    ok = system(command) == 0;
  }
  if (ok) {
    snprintf(command, sizeof(command),
             "wasmtime run --invoke rt_main %s >%s 2>/dev/null", wasm_path,
             result_path);
    ok = system(command) == 0;
  }
  if (ok) {
    file = fopen(result_path, "r");
    long runtime_result = 0;
    char extra = '\0';
    ok = file && fscanf(file, "%ld%c", &runtime_result, &extra) == 2 &&
         runtime_result == 53 && extra == '\n';
    if (file)
      fclose(file);
  }
  if (!ok)
    fprintf(stderr, "[portable-wasm] float operation smoke failed: %s\n",
            err[0] ? err : "module assembly or runtime result mismatch");
  unlink(wat_path);
  unlink(wasm_path);
  unlink(result_path);
  free(wat.data);
  nyr_ir_func_free(&ir);
  return ok;
#endif
}

static bool check_wasm_memory_traps(void) {
#ifdef _WIN32
  fprintf(stderr,
          "[portable-wasm] skipping memory-address runtime smoke: external "
          "runner check uses POSIX command lookup\n");
  return true;
#else
  if (system("command -v wat2wasm >/dev/null 2>&1 && "
             "command -v wasmtime >/dev/null 2>&1") != 0) {
    fprintf(stderr, "[portable-wasm] skipping memory-address smoke: "
                    "wat2wasm/wasmtime unavailable\n");
    return true;
  }

  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_WASM,
      .abi = NY_NATIVE_ABI_SYSV,
      .target_name = "wasm",
      .object_format = "elf",
      .symbol_prefix = "",
  };
  struct memory_trap_case {
    int64_t address;
    bool store;
    bool byte;
    const char *name;
  } cases[] = {
      {INT64_C(0x100000000), false, false, "high-address-load"},
      {INT64_C(0x100000000), true, true, "high-address-byte-store"},
      {INT64_C(65536), false, true, "out-of-bounds-byte-load"},
      {INT64_C(65529), true, false, "out-of-bounds-word-store"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    nyr_func_t ir;
    nyr_opt_begin(&ir);
    int address = nyr_opt_const(&ir, cases[i].address);
    int result = -1;
    if (address < 0) {
      nyr_ir_func_free(&ir);
      return false;
    }
    if (cases[i].store) {
      int value = nyr_opt_const(&ir, INT64_C(0x123456789));
      size_t before_store = ir.len;
      if (value < 0) {
        nyr_ir_func_free(&ir);
        return false;
      }
      (void)nyr_ir_emit(&ir,
                      (nyr_inst_t){
                          .op = NYIR_STORE_I64,
                          .dst = -1,
                          .a = address,
                          .b = -1,
                          .c = value,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .flags = cases[i].byte ? NYIR_INST_F_MEM_BYTE : 0,
                      });
      if (ir.len == before_store) {
        nyr_ir_func_free(&ir);
        return false;
      }
      result = nyr_opt_const(&ir, 0);
    } else {
      result =
          nyr_ir_emit(&ir, (nyr_inst_t){
                             .op = NYIR_LOAD_I64,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .flags = cases[i].byte ? NYIR_INST_F_MEM_BYTE : 0,
                         });
    }
    nyr_opt_ret(&ir, result);

    nyr_writer_t wat = {0};
    char err[256] = {0};
    bool ok = result >= 0 && nyr_opt_verify(&ir, "portable-wasm-memory-trap") &&
              nyr_put(&wat, "(module\n(memory 1)\n") &&
              nyr_wasm_emit_ir(&wat, &target, &ir, "rt_main", false, err,
                               sizeof(err)) &&
              nyr_put(&wat, ")\n");
    char wat_path[128], wasm_path[128], command[512];
    snprintf(wat_path, sizeof(wat_path), "/tmp/nyr-wasm-mem-%ld-%zu.wat",
             (long)getpid(), i);
    snprintf(wasm_path, sizeof(wasm_path), "/tmp/nyr-wasm-mem-%ld-%zu.wasm",
             (long)getpid(), i);
    FILE *file = ok ? fopen(wat_path, "wb") : NULL;
    if (ok && !file)
      ok = false;
    if (file) {
      if (fwrite(wat.data, 1, wat.len, file) != wat.len)
        ok = false;
      if (fclose(file) != 0)
        ok = false;
    }
    if (ok) {
      snprintf(command, sizeof(command), "wat2wasm %s -o %s", wat_path,
               wasm_path);
      ok = system(command) == 0;
    }
    if (ok) {
      snprintf(command, sizeof(command),
               "wasmtime run --invoke rt_main %s >/dev/null 2>&1", wasm_path);
      ok = system(command) != 0;
    }
    if (!ok)
      fprintf(stderr, "[portable-wasm] %s did not trap: %s\n", cases[i].name,
              err[0] ? err : "module assembly or runtime result mismatch");
    unlink(wat_path);
    unlink(wasm_path);
    free(wat.data);
    nyr_ir_func_free(&ir);
    if (!ok)
      return false;
  }
  return true;
#endif
}

static bool check_wasm_copy_struct(void) {
#ifdef _WIN32
  return true;
#else
  if (system("command -v wat2wasm >/dev/null 2>&1 && "
             "command -v wasmtime >/dev/null 2>&1") != 0) {
    fprintf(stderr, "[portable-wasm] skipping struct-copy smoke: "
                    "wat2wasm/wasmtime unavailable\n");
    return true;
  }

  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_WASM,
      .abi = NY_NATIVE_ABI_SYSV,
      .target_name = "wasm",
      .object_format = "elf",
      .symbol_prefix = "",
  };
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int source = nyr_opt_const(&ir, 1024);
  int destination = nyr_opt_const(&ir, 1032);
  int copied = -1;
  if (source >= 0 && destination >= 0)
    copied = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                                          .dst = -1,
                                          .a = destination,
                                          .b = source,
                                          .imm = 8});
  int result = copied >= 0 ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                          .dst = -1,
                                                          .a = destination})
                           : -1;
  nyr_opt_ret(&ir, result);

  nyr_writer_t wat = {0};
  char err[256] = {0};
  bool ok = result >= 0 && nyr_opt_verify(&ir, "portable-wasm-copy-struct") &&
            nyr_put(&wat, "(module\n(memory 1)\n"
                          "(data (i32.const 1024) "
                          "\"\\2a\\00\\00\\00\\00\\00\\00\\00\")\n") &&
            nyr_wasm_emit_ir(&wat, &target, &ir, "rt_main", false, err,
                             sizeof(err)) &&
            nyr_put(&wat, ")\n");

  /*
   * The backend must reject sizes that cannot be represented by memory.copy's
   * i32 length operand instead of silently truncating them.
   */
  if (ok) {
    for (size_t i = 0; i < ir.len; ++i)
      if (ir.data[i].op == NYIR_COPY_STRUCT) {
        ir.data[i].imm = (int64_t)INT32_MAX + 1;
        break;
      }
    nyr_writer_t rejected = {0};
    char reject_err[256] = {0};
    bool oversized_accepted =
        nyr_wasm_emit_ir(&rejected, &target, &ir, "oversized_copy", false,
                         reject_err, sizeof(reject_err));
    ok = !oversized_accepted &&
         strstr(reject_err, "struct copy size exceeds i32") != NULL;
    free(rejected.data);
  }

  char wat_path[128], wasm_path[128], result_path[128], command[512];
  snprintf(wat_path, sizeof(wat_path), "/tmp/nyr-wasm-copy-%ld.wat",
           (long)getpid());
  snprintf(wasm_path, sizeof(wasm_path), "/tmp/nyr-wasm-copy-%ld.wasm",
           (long)getpid());
  snprintf(result_path, sizeof(result_path), "/tmp/nyr-wasm-copy-%ld.out",
           (long)getpid());
  FILE *file = ok ? fopen(wat_path, "wb") : NULL;
  if (ok && !file)
    ok = false;
  if (file) {
    if (fwrite(wat.data, 1, wat.len, file) != wat.len)
      ok = false;
    if (fclose(file) != 0)
      ok = false;
  }
  if (ok) {
    snprintf(command, sizeof(command), "wat2wasm %s -o %s", wat_path,
             wasm_path);
    ok = system(command) == 0;
  }
  if (ok) {
    snprintf(command, sizeof(command),
             "wasmtime run --invoke rt_main %s >%s 2>/dev/null", wasm_path,
             result_path);
    ok = system(command) == 0;
  }
  if (ok) {
    file = fopen(result_path, "r");
    long runtime_result = 0;
    char extra = '\0';
    ok = file && fscanf(file, "%ld%c", &runtime_result, &extra) == 2 &&
         runtime_result == 42 && extra == '\n';
    if (file)
      fclose(file);
  }
  if (ok) {
    for (size_t i = 0; i < ir.len; ++i)
      if (ir.data[i].dst == source && ir.data[i].op == NYIR_CONST_I64) {
        ir.data[i].imm = INT64_C(0x100000400);
        break;
      }
    for (size_t i = 0; i < ir.len; ++i)
      if (ir.data[i].op == NYIR_COPY_STRUCT)
        ir.data[i].imm = 8;
    nyr_writer_t trap_wat = {0};
    char trap_err[256] = {0};
    bool trap_module = nyr_put(&trap_wat, "(module\n(memory 1)\n") &&
                       nyr_wasm_emit_ir(&trap_wat, &target, &ir, "rt_main",
                                        false, trap_err, sizeof(trap_err)) &&
                       nyr_put(&trap_wat, ")\n");
    char trap_wat_path[128], trap_wasm_path[128];
    snprintf(trap_wat_path, sizeof(trap_wat_path),
             "/tmp/nyr-wasm-copy-trap-%ld.wat", (long)getpid());
    snprintf(trap_wasm_path, sizeof(trap_wasm_path),
             "/tmp/nyr-wasm-copy-trap-%ld.wasm", (long)getpid());
    file = trap_module ? fopen(trap_wat_path, "wb") : NULL;
    if (trap_module && !file)
      trap_module = false;
    if (file) {
      if (fwrite(trap_wat.data, 1, trap_wat.len, file) != trap_wat.len)
        trap_module = false;
      if (fclose(file) != 0)
        trap_module = false;
    }
    if (trap_module) {
      snprintf(command, sizeof(command), "wat2wasm %s -o %s", trap_wat_path,
               trap_wasm_path);
      trap_module = system(command) == 0;
    }
    if (trap_module) {
      snprintf(command, sizeof(command),
               "wasmtime run --invoke rt_main %s >/dev/null 2>&1",
               trap_wasm_path);
      trap_module = system(command) != 0;
    }
    if (!trap_module)
      fprintf(stderr,
              "[portable-wasm] high-address struct copy did not trap: %s\n",
              trap_err[0] ? trap_err : "module assembly or runtime mismatch");
    ok = trap_module;
    unlink(trap_wat_path);
    unlink(trap_wasm_path);
    free(trap_wat.data);
  }
  if (!ok)
    fprintf(stderr, "[portable-wasm] struct-copy smoke failed: %s\n",
            err[0] ? err : "module assembly or runtime result mismatch");
  unlink(wat_path);
  unlink(wasm_path);
  unlink(result_path);
  free(wat.data);
  nyr_ir_func_free(&ir);
  return ok;
#endif
}

static bool check_cross_struct_copy(nyr_backend_t backend,
                                    portable_emitter_fn emitter,
                                    const char *target_name,
                                    const char *qemu_command) {
#ifdef _WIN32
  (void)backend;
  (void)emitter;
  (void)target_name;
  (void)qemu_command;
  return true;
#else
  char tool_check[128];
  snprintf(tool_check, sizeof(tool_check),
           "command -v clang >/dev/null 2>&1 && "
           "command -v %s >/dev/null 2>&1",
           qemu_command);
  if (system(tool_check) != 0) {
    fprintf(stderr, "[portable-copy] skipping %s runtime: toolchain missing\n",
            target_name);
    return true;
  }
  nyr_target_request_t request =
      nyr_target_request_make(backend, NY_NATIVE_ABI_AUTO, NULL, false);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &request))
    return false;

  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int source = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                            .dst = -1,
                                            .symbol = "copy_source"});
  int destination = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                 .dst = -1,
                                                 .symbol = "copy_destination"});
  int result = -1;
  if (source >= 0 && destination >= 0) {
    size_t before_copy = ir.len;
    /*
     * This side-effecting NYIR operation has no result value.
     */
    (void)nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                                       .dst = -1,
                                       .a = destination,
                                       .b = source,
                                       .imm = 8});
    if (ir.len == before_copy + 1) {
      int copied = nyr_ir_emit(
          &ir, (nyr_inst_t){.op = NYIR_LOAD_I64, .dst = -1, .a = destination});
      int copy_expected = nyr_opt_const(&ir, 42);
      int copy_ok = nyr_opt_cmp(&ir, copied, copy_expected, NYIR_CMP_EQ);
      int rotate_value = nyr_opt_const(&ir, INT64_C(0x40000000));
      int rotate_count = nyr_opt_const(&ir, 33);
      int rotated =
          nyr_opt_binop(&ir, NYIR_ROR32_I64, rotate_value, rotate_count);
      int rotate_expected = nyr_opt_const(&ir, INT64_C(0x20000000));
      int rotate_ok = nyr_opt_cmp(&ir, rotated, rotate_expected, NYIR_CMP_EQ);
      int swapped = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_BSWAP32_I64,
                                                 .dst = -1,
                                                 .a = rotate_value,
                                                 .b = -1});
      int swap_expected = nyr_opt_const(&ir, 64);
      int swap_ok = nyr_opt_cmp(&ir, swapped, swap_expected, NYIR_CMP_EQ);
      int both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, copy_ok, rotate_ok);
      both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, swap_ok);
      if (backend != NY_NATIVE_BACKEND_ARM) {
        int swap64_value = nyr_opt_const(&ir, INT64_C(0x0123456789abcdef));
        int swapped64 = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_BSWAP_I64,
                                                     .dst = -1,
                                                     .a = swap64_value,
                                                     .b = -1});
        int swap64_expected =
            nyr_opt_const(&ir, (int64_t)UINT64_C(0xefcdab8967452301));
        int swap64_ok =
            nyr_opt_cmp(&ir, swapped64, swap64_expected, NYIR_CMP_EQ);
        int swap64_wrong_high = nyr_opt_const(&ir, INT64_C(0x0000000067452301));
        int swap64_high_neq =
            nyr_opt_cmp(&ir, swapped64, swap64_wrong_high, NYIR_CMP_NE);
        int zero = nyr_opt_const(&ir, 0);
        int one = nyr_opt_const(&ir, 1);
        int signed_lt =
            nyr_opt_cmp(&ir, swapped64, swap64_wrong_high, NYIR_CMP_LT);
        int signed_lt_ok = nyr_opt_select(&ir, signed_lt, one, zero);
        int signed_ge =
            nyr_opt_cmp(&ir, swapped64, swap64_wrong_high, NYIR_CMP_GE);
        int signed_ge_false = nyr_opt_select(&ir, signed_ge, zero, one);
        int signed_le =
            nyr_opt_cmp(&ir, swapped64, swap64_wrong_high, NYIR_CMP_LE);
        int signed_le_ok = nyr_opt_select(&ir, signed_le, one, zero);
        int signed_gt =
            nyr_opt_cmp(&ir, swapped64, swap64_wrong_high, NYIR_CMP_GT);
        int signed_gt_false = nyr_opt_select(&ir, signed_gt, zero, one);
        int swap64_low_next =
            nyr_opt_const(&ir, (int64_t)UINT64_C(0xefcdab8967452302));
        int low_lt = nyr_opt_cmp(&ir, swapped64, swap64_low_next, NYIR_CMP_LT);
        int low_lt_ok = nyr_opt_select(&ir, low_lt, one, zero);
        int low_le = nyr_opt_cmp(&ir, swapped64, swap64_low_next, NYIR_CMP_LE);
        int low_le_ok = nyr_opt_select(&ir, low_le, one, zero);
        int bitwise_a = nyr_opt_const(&ir, INT64_C(0x1000000000000001));
        int bitwise_b = nyr_opt_const(&ir, INT64_C(0x0000000100000001));
        int bitwise_and =
            nyr_opt_binop(&ir, NYIR_AND_I64, bitwise_a, bitwise_b);
        int bitwise_and_expected = nyr_opt_const(&ir, 1);
        int bitwise_and_ok =
            nyr_opt_cmp(&ir, bitwise_and, bitwise_and_expected, NYIR_CMP_EQ);
        int bitwise_or = nyr_opt_binop(&ir, NYIR_OR_I64, bitwise_a, bitwise_b);
        int bitwise_or_expected =
            nyr_opt_const(&ir, INT64_C(0x1000000100000001));
        int bitwise_or_ok =
            nyr_opt_cmp(&ir, bitwise_or, bitwise_or_expected, NYIR_CMP_EQ);
        int bitwise_xor =
            nyr_opt_binop(&ir, NYIR_XOR_I64, bitwise_a, bitwise_b);
        int bitwise_xor_expected =
            nyr_opt_const(&ir, INT64_C(0x1000000100000000));
        int bitwise_xor_ok =
            nyr_opt_cmp(&ir, bitwise_xor, bitwise_xor_expected, NYIR_CMP_EQ);
        int left_value = nyr_opt_const(&ir, 1);
        int left_count = nyr_opt_const(&ir, 40);
        int left_shift =
            nyr_opt_binop(&ir, NYIR_SHL_I64, left_value, left_count);
        int left_expected = nyr_opt_const(&ir, INT64_C(0x0000010000000000));
        int left_ok = nyr_opt_cmp(&ir, left_shift, left_expected, NYIR_CMP_EQ);
        int right_value = nyr_opt_const(&ir, -INT64_C(0x4000000000000000));
        int right_count = nyr_opt_const(&ir, 33);
        int right_shift =
            nyr_opt_binop(&ir, NYIR_SAR_I64, right_value, right_count);
        int right_expected = nyr_opt_const(&ir, -INT64_C(0x20000000));
        int right_ok =
            nyr_opt_cmp(&ir, right_shift, right_expected, NYIR_CMP_EQ);
        int left32_count = nyr_opt_const(&ir, 32);
        int left32 = nyr_opt_binop(&ir, NYIR_SHL_I64, left_value, left32_count);
        int left32_expected = nyr_opt_const(&ir, INT64_C(0x0000000100000000));
        int left32_ok = nyr_opt_cmp(&ir, left32, left32_expected, NYIR_CMP_EQ);
        int right1_count = nyr_opt_const(&ir, 1);
        int right1 =
            nyr_opt_binop(&ir, NYIR_SAR_I64, right_value, right1_count);
        int right1_expected = nyr_opt_const(&ir, -INT64_C(0x2000000000000000));
        int right1_ok = nyr_opt_cmp(&ir, right1, right1_expected, NYIR_CMP_EQ);
        int minus_one = nyr_opt_const(&ir, -1);
        int right63_count = nyr_opt_const(&ir, 63);
        int right63 =
            nyr_opt_binop(&ir, NYIR_SAR_I64, minus_one, right63_count);
        int right63_ok = nyr_opt_cmp(&ir, right63, minus_one, NYIR_CMP_EQ);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, swap64_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, swap64_high_neq);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, signed_lt_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, signed_ge_false);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, signed_le_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, signed_gt_false);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, low_lt_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, low_le_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, bitwise_and_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, bitwise_or_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, bitwise_xor_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, left_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, right_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, left32_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, right1_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, right63_ok);
        if (backend == NY_NATIVE_BACKEND_X86) {
          int add_left = nyr_opt_const(&ir, INT64_C(0x00000001ffffffff));
          int add_one = nyr_opt_const(&ir, 1);
          int add_result = nyr_opt_binop(&ir, NYIR_ADD_I64, add_left, add_one);
          int add_expected = nyr_opt_const(&ir, INT64_C(0x0000000200000000));
          int add_ok = nyr_opt_cmp(&ir, add_result, add_expected, NYIR_CMP_EQ);
          int sub_left = nyr_opt_const(&ir, INT64_C(0x0000000200000000));
          int sub_result = nyr_opt_binop(&ir, NYIR_SUB_I64, sub_left, add_one);
          int sub_expected = nyr_opt_const(&ir, INT64_C(0x00000001ffffffff));
          int sub_ok = nyr_opt_cmp(&ir, sub_result, sub_expected, NYIR_CMP_EQ);
          int mul_value = nyr_opt_const(&ir, INT64_C(0x0000000100000001));
          int mul_result =
              nyr_opt_binop(&ir, NYIR_MUL_I64, mul_value, mul_value);
          int mul_expected = nyr_opt_const(&ir, INT64_C(0x0000000200000001));
          int mul_ok = nyr_opt_cmp(&ir, mul_result, mul_expected, NYIR_CMP_EQ);
          int store_value = nyr_opt_const(&ir, INT64_C(0x1122334455667788));
          (void)nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                             .dst = -1,
                                             .a = destination,
                                             .b = -1,
                                             .c = store_value});
          int stored_value = nyr_ir_emit(
              &ir,
              (nyr_inst_t){.op = NYIR_LOAD_I64, .dst = -1, .a = destination});
          int store_ok =
              nyr_opt_cmp(&ir, stored_value, store_value, NYIR_CMP_EQ);
          both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, add_ok);
          both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, sub_ok);
          both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, mul_ok);
          both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, store_ok);
        }
      }
      if (backend == NY_NATIVE_BACKEND_ARM) {
        int arm_value = nyr_opt_const(&ir, INT64_C(0x0123456789abcdef));
        int arm_swapped = nyr_ir_emit(
            &ir, (nyr_inst_t){
                     .op = NYIR_BSWAP_I64, .dst = -1, .a = arm_value, .b = -1});
        int arm_expected =
            nyr_opt_const(&ir, (int64_t)UINT64_C(0xefcdab8967452301));
        int arm_swap_ok =
            nyr_opt_cmp(&ir, arm_swapped, arm_expected, NYIR_CMP_EQ);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, arm_swap_ok);

        int add_left = nyr_opt_const(&ir, INT64_C(0x00000001ffffffff));
        int add_one = nyr_opt_const(&ir, 1);
        int add_result = nyr_opt_binop(&ir, NYIR_ADD_I64, add_left, add_one);
        int add_expected = nyr_opt_const(&ir, INT64_C(0x0000000200000000));
        int add_ok = nyr_opt_cmp(&ir, add_result, add_expected, NYIR_CMP_EQ);
        int sub_result =
            nyr_opt_binop(&ir, NYIR_SUB_I64, add_expected, add_one);
        int sub_expected = nyr_opt_const(&ir, INT64_C(0x00000001ffffffff));
        int sub_ok = nyr_opt_cmp(&ir, sub_result, sub_expected, NYIR_CMP_EQ);
        int mul_value = nyr_opt_const(&ir, INT64_C(0x0000000100000001));
        int mul_result = nyr_opt_binop(&ir, NYIR_MUL_I64, mul_value, mul_value);
        int mul_expected = nyr_opt_const(&ir, INT64_C(0x0000000200000001));
        int mul_ok = nyr_opt_cmp(&ir, mul_result, mul_expected, NYIR_CMP_EQ);
        int bitwise_a = nyr_opt_const(&ir, INT64_C(0x1000000000000001));
        int bitwise_b = nyr_opt_const(&ir, INT64_C(0x0000000100000001));
        int and_expected = nyr_opt_const(&ir, 1);
        int bitwise_and =
            nyr_opt_binop(&ir, NYIR_AND_I64, bitwise_a, bitwise_b);
        int bitwise_and_ok =
            nyr_opt_cmp(&ir, bitwise_and, and_expected, NYIR_CMP_EQ);
        int bitwise_or = nyr_opt_binop(&ir, NYIR_OR_I64, bitwise_a, bitwise_b);
        int or_expected = nyr_opt_const(&ir, INT64_C(0x1000000100000001));
        int bitwise_or_ok =
            nyr_opt_cmp(&ir, bitwise_or, or_expected, NYIR_CMP_EQ);
        int bitwise_xor =
            nyr_opt_binop(&ir, NYIR_XOR_I64, bitwise_a, bitwise_b);
        int xor_expected = nyr_opt_const(&ir, INT64_C(0x1000000100000000));
        int bitwise_xor_ok =
            nyr_opt_cmp(&ir, bitwise_xor, xor_expected, NYIR_CMP_EQ);
        int left_value = nyr_opt_const(&ir, 1);
        int left_count = nyr_opt_const(&ir, 40);
        int left_shift =
            nyr_opt_binop(&ir, NYIR_SHL_I64, left_value, left_count);
        int left_expected = nyr_opt_const(&ir, INT64_C(0x0000010000000000));
        int left_ok = nyr_opt_cmp(&ir, left_shift, left_expected, NYIR_CMP_EQ);
        int right_value = nyr_opt_const(&ir, -INT64_C(0x4000000000000000));
        int right_count = nyr_opt_const(&ir, 33);
        int right_shift =
            nyr_opt_binop(&ir, NYIR_SAR_I64, right_value, right_count);
        int right_expected = nyr_opt_const(&ir, -INT64_C(0x20000000));
        int right_ok =
            nyr_opt_cmp(&ir, right_shift, right_expected, NYIR_CMP_EQ);
        int arm_left32_count = nyr_opt_const(&ir, 32);
        int arm_left32 =
            nyr_opt_binop(&ir, NYIR_SHL_I64, left_value, arm_left32_count);
        int arm_left32_expected =
            nyr_opt_const(&ir, INT64_C(0x0000000100000000));
        int arm_left32_ok =
            nyr_opt_cmp(&ir, arm_left32, arm_left32_expected, NYIR_CMP_EQ);
        int arm_left63_count = nyr_opt_const(&ir, 63);
        int arm_left63 =
            nyr_opt_binop(&ir, NYIR_SHL_I64, left_value, arm_left63_count);
        int arm_left63_expected = nyr_opt_const(&ir, INT64_MIN);
        int arm_left63_ok =
            nyr_opt_cmp(&ir, arm_left63, arm_left63_expected, NYIR_CMP_EQ);
        int arm_right32_count = nyr_opt_const(&ir, 32);
        int arm_right32 =
            nyr_opt_binop(&ir, NYIR_SAR_I64, right_value, arm_right32_count);
        int arm_right32_expected = nyr_opt_const(&ir, -INT64_C(0x40000000));
        int arm_right32_ok =
            nyr_opt_cmp(&ir, arm_right32, arm_right32_expected, NYIR_CMP_EQ);
        int arm_minus_one = nyr_opt_const(&ir, -1);
        int arm_right63_count = nyr_opt_const(&ir, 63);
        int arm_right63 =
            nyr_opt_binop(&ir, NYIR_SAR_I64, arm_minus_one, arm_right63_count);
        int arm_right63_ok =
            nyr_opt_cmp(&ir, arm_right63, arm_minus_one, NYIR_CMP_EQ);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, add_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, sub_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, mul_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, bitwise_and_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, bitwise_or_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, bitwise_xor_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, left_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, arm_left32_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, arm_left63_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, right_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, arm_right32_ok);
        both_ok = nyr_opt_binop(&ir, NYIR_AND_I64, both_ok, arm_right63_ok);
      }
      if (backend == NY_NATIVE_BACKEND_X86) {
        int low_result =
            nyr_opt_binop(&ir, NYIR_MUL_I64, both_ok,
                          nyr_opt_const(&ir, INT64_C(0x0000000089abcdef)));
        int high_result = nyr_opt_binop(&ir, NYIR_MUL_I64, both_ok,
                                        nyr_opt_const(&ir, 0x01234567));
        int high_count = nyr_opt_const(&ir, 32);
        int high_part =
            nyr_opt_binop(&ir, NYIR_SHL_I64, high_result, high_count);
        result = nyr_opt_binop(&ir, NYIR_OR_I64, high_part, low_result);
      } else {
        int success_code = nyr_opt_const(&ir, 42);
        result = nyr_opt_binop(&ir, NYIR_MUL_I64, both_ok, success_code);
      }
    }
  }
  nyr_opt_ret(&ir, result);
  if (backend == NY_NATIVE_BACKEND_X86)
    ir.data[ir.len - 1].scalar_class = NYIR_SCALAR_I64_RAW;

  nyr_writer_t assembly = {0};
  char err[256] = {0};
  const char *start =
      backend == NY_NATIVE_BACKEND_MIPS
          ? ".set noreorder\n.text\n.globl _start\n_start:\n"
            "  jal rt_main\n  nop\n  move $a0, $v0\n"
            "  li $v0, 5058\n  syscall\n"
      : backend == NY_NATIVE_BACKEND_AARCH64
          ? ".text\n.globl _start\n_start:\n"
            "  bl rt_main\n  mov x8, #93\n  svc #0\n"
      : backend == NY_NATIVE_BACKEND_ARM
          ? ".text\n.globl _start\n_start:\n"
            "  bl rt_main\n  cmp r0, #42\n  bne .Larm_return_fail\n"
            "  bl rt_bswap64\n"
            "  ldr r2, =0x67452301\n  cmp r0, r2\n"
            "  bne .Larm_return_fail\n"
            "  ldr r2, =0xefcdab89\n  cmp r1, r2\n"
            "  bne .Larm_return_fail\n"
            "  mov r0, #42\n  b .Larm_return_exit\n"
            ".Larm_return_fail:\n  mov r0, #1\n"
            ".Larm_return_exit:\n  mov r7, #1\n  svc #0\n"
      : backend == NY_NATIVE_BACKEND_X86
          ? ".text\n.globl _start\n_start:\n"
            "  call rt_main\n"
            "  cmpl $0x89abcdef, %eax\n  jne .Lreturn_fail\n"
            "  cmpl $0x01234567, %edx\n  jne .Lreturn_fail\n"
            "  movl $42, %ebx\n  jmp .Lreturn_exit\n"
            ".Lreturn_fail:\n  movl $1, %ebx\n"
            ".Lreturn_exit:\n  movl $1, %eax\n"
            "  int $0x80\n"
      : backend == NY_NATIVE_BACKEND_RISCV
          ? ".text\n.globl _start\n_start:\n"
            "  call rt_main\n  li a7, 93\n  ecall\n"
          : ".text\n.globl _start\n_start:\n"
            "  bl rt_main\n  nop\n  li 0, 1\n  sc\n";
  bool ok =
      result >= 0 && nyr_opt_verify(&ir, "portable-copy-struct-cross-target") &&
      nyr_put(&assembly,
              ".data\n.balign 8\n.globl copy_source\ncopy_source:\n"
              "  .quad 42\n.globl copy_destination\ncopy_destination:\n"
              "  .quad 0\n") &&
      emitter(&assembly, &target, &ir, "rt_main", false, err, sizeof(err));
  if (ok && backend == NY_NATIVE_BACKEND_ARM) {
    nyr_func_t bswap_ir;
    nyr_opt_begin(&bswap_ir);
    int value = nyr_opt_const(&bswap_ir, INT64_C(0x0123456789abcdef));
    int swapped = nyr_ir_emit(
        &bswap_ir,
        (nyr_inst_t){.op = NYIR_BSWAP_I64, .dst = -1, .a = value, .b = -1});
    nyr_opt_ret(&bswap_ir, swapped);
    bswap_ir.data[bswap_ir.len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
    ok = value >= 0 && swapped >= 0 &&
         nyr_opt_verify(&bswap_ir, "arm-bswap-i64-return-pair") &&
         emitter(&assembly, &target, &bswap_ir, "rt_bswap64", false, err,
                 sizeof(err));
    nyr_ir_func_free(&bswap_ir);
  }

  char code_path[128], start_path[128], exe_path[128], command[1024];
  snprintf(code_path, sizeof(code_path), "/tmp/nyr-copy-%ld-%s.s",
           (long)getpid(), target_name);
  snprintf(start_path, sizeof(start_path), "/tmp/nyr-copy-%ld-%s-start.s",
           (long)getpid(), target_name);
  snprintf(exe_path, sizeof(exe_path), "/tmp/nyr-copy-%ld-%s", (long)getpid(),
           target_name);
  const char *failed_stage = ok ? NULL : "NYIR verify or emission";
  int link_status = -1;
  int runtime_status = -1;
  FILE *file = ok ? fopen(code_path, "wb") : NULL;
  if (ok && !file) {
    ok = false;
    failed_stage = "open assembly output";
  }
  if (file) {
    if (fwrite(assembly.data, 1, assembly.len, file) != assembly.len) {
      ok = false;
      failed_stage = "write assembly output";
    }
    if (fclose(file) != 0) {
      ok = false;
      failed_stage = "close assembly output";
    }
  }
  file = ok ? fopen(start_path, "wb") : NULL;
  if (ok && !file) {
    ok = false;
    failed_stage = "open startup output";
  }
  if (file) {
    size_t start_len = strlen(start);
    if (fwrite(start, 1, start_len, file) != start_len) {
      ok = false;
      failed_stage = "write startup output";
    }
    if (fclose(file) != 0) {
      ok = false;
      failed_stage = "close startup output";
    }
  }
  if (ok) {
    if (backend == NY_NATIVE_BACKEND_MIPS) {
      snprintf(command, sizeof(command),
               "clang --target=mips64-unknown-linux-gnuabi64 -mno-abicalls "
               "-fno-pic -fuse-ld=lld -nostdlib -static -Wl,-e,_start "
               "-Wl,--build-id=none -o %s %s %s",
               exe_path, code_path, start_path);
    } else if (backend == NY_NATIVE_BACKEND_AARCH64) {
      snprintf(command, sizeof(command),
               "clang --target=aarch64-unknown-linux-gnu -fuse-ld=lld "
               "-nostdlib -static -Wl,-e,_start -Wl,--build-id=none "
               "-o %s %s %s",
               exe_path, code_path, start_path);
    } else if (backend == NY_NATIVE_BACKEND_ARM) {
      snprintf(command, sizeof(command),
               "clang --target=armv7-linux-gnueabi -march=armv7-a -marm "
               "-fuse-ld=lld "
               "-nostdlib -static -Wl,-e,_start -Wl,--build-id=none "
               "-o %s %s %s",
               exe_path, code_path, start_path);
    } else if (backend == NY_NATIVE_BACKEND_X86) {
      snprintf(command, sizeof(command),
               "clang --target=i386-unknown-linux-gnu -fuse-ld=lld "
               "-nostdlib -static -Wl,-e,_start -Wl,--build-id=none "
               "-o %s %s %s",
               exe_path, code_path, start_path);
    } else if (backend == NY_NATIVE_BACKEND_RISCV) {
      snprintf(command, sizeof(command),
               "clang --target=riscv64-unknown-linux-gnu -fuse-ld=lld "
               "-nostdlib -static -Wl,-e,_start -Wl,--build-id=none "
               "-o %s %s %s",
               exe_path, code_path, start_path);
    } else {
      snprintf(command, sizeof(command),
               "clang --target=powerpc64-unknown-linux-gnu -fuse-ld=lld "
               "-nostdlib -static -Wl,-e,_start -Wl,--build-id=none "
               "-o %s %s %s",
               exe_path, code_path, start_path);
    }
    link_status = system(command);
    ok = link_status == 0;
    if (!ok)
      failed_stage = "assemble/link";
  }
  if (ok) {
    snprintf(command, sizeof(command), "%s %s", qemu_command, exe_path);
    runtime_status = system(command);
    ok = runtime_status != -1 && WIFEXITED(runtime_status) &&
         WEXITSTATUS(runtime_status) == 42;
    if (!ok)
      failed_stage = "QEMU result";
  }
  if (ok && backend != NY_NATIVE_BACKEND_AVR) {
    const bool checks_i32_size =
        backend == NY_NATIVE_BACKEND_ARM || backend == NY_NATIVE_BACKEND_X86;
    const size_t rejected_count = checks_i32_size ? 2 : 1;
    for (size_t rejection = 0; rejection < rejected_count; ++rejection) {
      nyr_func_t bad_ir = ir;
      const bool oversized = rejection != 0;
      for (size_t i = 0; i < bad_ir.len; ++i)
        if (bad_ir.data[i].op == NYIR_COPY_STRUCT)
          bad_ir.data[i].imm = oversized ? (int64_t)INT32_MAX + 1 : -1;
      nyr_writer_t rejected = {0};
      char reject_err[256] = {0};
      const char *expected =
          oversized ? "32-bit address space" : "negative struct copy size";
      const bool rejected_size =
          !emitter(&rejected, &target, &bad_ir, "rejected_copy_size", false,
                   reject_err, sizeof(reject_err)) &&
          strstr(reject_err, expected) != NULL;
      free(rejected.data);
      if (!rejected_size) {
        ok = false;
        failed_stage =
            oversized ? "oversized-size rejection" : "negative-size rejection";
        break;
      }
    }
  }
  if (!ok)
    fprintf(stderr,
            "[portable-copy] %s struct copy failed at %s (link=%d run=%d): "
            "%s; artifacts: %s %s %s\n",
            target_name, failed_stage ? failed_stage : "unknown", link_status,
            runtime_status, err[0] ? err : "result mismatch", code_path,
            start_path, exe_path);
  if (ok) {
    unlink(code_path);
    unlink(start_path);
    unlink(exe_path);
  }
  free(assembly.data);
  nyr_ir_func_free(&ir);
  return ok;
#endif
}

static bool check_bounds_case(int64_t offset, int64_t limit, bool dynamic,
                              bool should_trap) {
#ifdef _WIN32
  (void)offset;
  (void)limit;
  (void)dynamic;
  (void)should_trap;
  return true;
#else
  if (system("command -v wat2wasm >/dev/null 2>&1 && "
             "command -v wasmtime >/dev/null 2>&1") != 0)
    return true;

  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_WASM,
      .abi = NY_NATIVE_ABI_SYSV,
      .target_name = "wasm",
      .object_format = "elf",
      .symbol_prefix = "",
  };
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int base = nyr_opt_const(&ir, 0);
  int index = nyr_opt_const(&ir, offset);
  int length = dynamic ? nyr_opt_const(&ir, limit) : -1;
  nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                               .dst = -1,
                               .a = base,
                               .b = index,
                               .c = length,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .imm = limit});
  int result = nyr_opt_const(&ir, 42);
  nyr_opt_ret(&ir, result);

  nyr_writer_t wat = {0};
  char err[256] = {0};
  bool ok = nyr_opt_verify(&ir, "portable-wasm-bounds") &&
            nyr_put(&wat, "(module\n") &&
            nyr_wasm_emit_ir(&wat, &target, &ir, "rt_main", false, err,
                             sizeof(err)) &&
            nyr_put(&wat, ")\n");
  if (ok && (!strstr(wat.data, "i64.lt_s") || !strstr(wat.data, "i64.ge_s") ||
             !strstr(wat.data, "unreachable")))
    ok = false;

  char wat_path[128], wasm_path[128], result_path[128], command[512];
  const long id = (long)getpid();
  snprintf(wat_path, sizeof(wat_path), "/tmp/nyr-wasm-bounds-%ld.wat", id);
  snprintf(wasm_path, sizeof(wasm_path), "/tmp/nyr-wasm-bounds-%ld.wasm", id);
  snprintf(result_path, sizeof(result_path), "/tmp/nyr-wasm-bounds-%ld.out",
           id);
  FILE *file = ok ? fopen(wat_path, "wb") : NULL;
  if (ok && !file)
    ok = false;
  if (file) {
    if (fwrite(wat.data, 1, wat.len, file) != wat.len)
      ok = false;
    if (fclose(file) != 0)
      ok = false;
  }
  if (ok) {
    snprintf(command, sizeof(command), "wat2wasm %s -o %s", wat_path,
             wasm_path);
    ok = system(command) == 0;
  }
  if (ok) {
    snprintf(command, sizeof(command),
             "wasmtime run --invoke rt_main %s >%s 2>/dev/null", wasm_path,
             result_path);
    const int status = system(command);
    if (should_trap) {
      ok = status != 0;
    } else if (status == 0) {
      file = fopen(result_path, "r");
      long actual = 0;
      char extra = '\0';
      ok = file && fscanf(file, "%ld%c", &actual, &extra) == 2 &&
           actual == 42 && extra == '\n';
      if (file)
        fclose(file);
    } else {
      ok = false;
    }
  }
  if (!ok)
    fprintf(stderr,
            "[portable-wasm] bounds offset=%lld limit=%lld dynamic=%d "
            "trap=%d failed: %s\n",
            (long long)offset, (long long)limit, dynamic, should_trap,
            err[0] ? err : "module assembly or runtime result mismatch");
  unlink(wat_path);
  unlink(wasm_path);
  unlink(result_path);
  free(wat.data);
  nyr_ir_func_free(&ir);
  return ok;
#endif
}

static bool check_avr_bswap(bool is32, uint64_t value, uint64_t expected) {
  const nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_AVR, NY_NATIVE_ABI_AUTO, NULL, false);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &request))
    return false;

  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int source = nyr_opt_const(&ir, (int64_t)value);
  int result = nyr_ir_emit(
      &ir, (nyr_inst_t){.op = is32 ? NYIR_BSWAP32_I64 : NYIR_BSWAP_I64,
                         .dst = -1,
                         .a = source,
                         .b = -1,
                         .c = -1,
                         .d = -1,
                         .e = -1,
                         .f = -1});
  nyr_opt_ret(&ir, result);

  nyr_writer_t output = {0};
  char err[256] = {0};
  bool ok =
      result >= 0 && nyr_ir_verify(&ir, err, sizeof(err)) &&
      (uint64_t)nyr_opt_eval(&ir, is32 ? "portable-avr-bswap32"
                                       : "portable-avr-bswap64") == expected &&
      nyr_avr_emit_ir(&output, &target, &ir, "bswap", false, err, sizeof(err));
  if (ok) {
    ok = output.data && strstr(output.data, "mov\tr0, r18") &&
         (is32 ? strstr(output.data, "clr\tr25")
               : strstr(output.data, "mov\tr25, r0"));
  }
  if (!ok)
    fprintf(stderr, "[portable-avr] bswap%d failed: %s\n", is32 ? 32 : 64,
            err[0] ? err : "value or byte permutation mismatch");
  free(output.data);
  nyr_ir_func_free(&ir);
  return ok;
}

static bool check_avr_copy_struct(void) {
#ifdef _WIN32
  return true;
#else
  const char *toolchain = "command -v clang >/dev/null 2>&1 && "
                          "command -v ld.lld >/dev/null 2>&1 && "
                          "command -v qemu-system-avr >/dev/null 2>&1";
  if (system(toolchain) != 0) {
    fprintf(stderr, "[portable-avr] skipping struct-copy runtime: toolchain "
                    "missing\n");
    return true;
  }
  const nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_AVR, NY_NATIVE_ABI_AUTO, NULL, false);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &request))
    return false;

  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int destination = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                 .dst = -1,
                                                 .symbol = "copy_destination"});
  int source = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                            .dst = -1,
                                            .symbol = "copy_source"});
  int value = nyr_opt_const(&ir, INT64_C(0x0123456789abcdef));
  int result = -1;
  if (destination >= 0 && source >= 0 && value >= 0) {
    size_t before_store = ir.len;
    /*
     * Side-effect-only instructions append successfully but return -1.
     */
    (void)nyr_ir_emit(
        &ir, (nyr_inst_t){
                 .op = NYIR_STORE_I64, .dst = -1, .a = source, .c = value});
    if (ir.len == before_store + 1) {
      size_t before_copy = ir.len;
      (void)nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                                         .dst = -1,
                                         .a = destination,
                                         .b = source,
                                         .imm = 8});
      if (ir.len == before_copy + 1) {
        int copied = nyr_ir_emit(
            &ir,
            (nyr_inst_t){.op = NYIR_LOAD_I64, .dst = -1, .a = destination});
        int copy_ok = nyr_opt_cmp(&ir, copied, value, NYIR_CMP_EQ);
        int rotate_value = nyr_opt_const(&ir, INT64_C(0x40000080));
        int rotate_count = nyr_opt_const(&ir, 33);
        int rotated =
            nyr_opt_binop(&ir, NYIR_ROR32_I64, rotate_value, rotate_count);
        int rotate_expected = nyr_opt_const(&ir, INT64_C(0x20000040));
        int rotate_ok = nyr_opt_cmp(&ir, rotated, rotate_expected, NYIR_CMP_EQ);
        int zero = nyr_opt_const(&ir, 0);
        int rotated_result = nyr_opt_select(&ir, rotate_ok, copied, zero);
        result = nyr_opt_select(&ir, copy_ok, rotated_result, zero);
      }
    }
  }
  nyr_opt_ret(&ir, result);

  nyr_writer_t assembly = {0};
  char err[256] = {0};
  bool ok = result >= 0 && nyr_opt_verify(&ir, "portable-avr-copy-struct") &&
            nyr_put(&assembly, ".data\n.globl copy_source\ncopy_source:\n"
                               "  .zero 8\n.globl copy_destination\n"
                               "copy_destination:\n  .zero 8\n") &&
            nyr_avr_emit_ir(&assembly, &target, &ir, "rt_main", true, err,
                            sizeof(err));
  char code_path[128], start_path[128], obj_path[128], start_obj_path[128];
  char exe_path[128], command[1024];
  const long pid = (long)getpid();
  snprintf(code_path, sizeof(code_path), "/tmp/nyr-copy-avr-%ld.s", pid);
  snprintf(start_path, sizeof(start_path), "/tmp/nyr-copy-avr-%ld-start.s",
           pid);
  snprintf(obj_path, sizeof(obj_path), "/tmp/nyr-copy-avr-%ld.o", pid);
  snprintf(start_obj_path, sizeof(start_obj_path),
           "/tmp/nyr-copy-avr-%ld-start.o", pid);
  snprintf(exe_path, sizeof(exe_path), "/tmp/nyr-copy-avr-%ld.elf", pid);
  const char *failed_stage = ok ? NULL : "NYIR verify or emission";
  FILE *file = ok ? fopen(code_path, "wb") : NULL;
  if (ok && !file) {
    ok = false;
    failed_stage = "open assembly output";
  }
  if (file) {
    if (fwrite(assembly.data, 1, assembly.len, file) != assembly.len) {
      ok = false;
      failed_stage = "write assembly output";
    }
    if (fclose(file) != 0) {
      ok = false;
      failed_stage = "close assembly output";
    }
  }
  static const char startup[] = ".text\n.global _start\n_start:\n"
                                "  call rt_main\n"
                                "  ldi r16, 0\n  sts 0x00C5, r16\n"
                                "  ldi r16, 103\n  sts 0x00C4, r16\n"
                                "  ldi r16, 0x06\n  sts 0x00C2, r16\n"
                                "  ldi r16, 0x08\n  sts 0x00C1, r16\n"
                                "  cpi r18, 0xDF\n  brne 3f\n"
                                "  cpi r19, 0x9B\n  brne 3f\n"
                                "  cpi r20, 0x57\n  brne 3f\n"
                                "  cpi r21, 0x13\n  brne 3f\n"
                                "  cpi r22, 0xCF\n  brne 3f\n"
                                "  cpi r23, 0x8A\n  brne 3f\n"
                                "  cpi r24, 0x46\n  brne 3f\n"
                                "  cpi r25, 0x02\n  brne 3f\n";
  static const char startup_tail[] =
      "  ldi r16, 'U'\n  rjmp 4f\n"
      "3:\n  ldi r16, 'E'\n"
      "4:\n1:\n  lds r17, 0x00C0\n  sbrs r17, 5\n"
      "  rjmp 1b\n  sts 0x00C6, r16\n"
      "2:\n  rjmp 2b\n";
  file = ok ? fopen(start_path, "wb") : NULL;
  if (ok && !file) {
    ok = false;
    failed_stage = "open startup output";
  }
  if (file) {
    if (fwrite(startup, 1, sizeof(startup) - 1, file) != sizeof(startup) - 1 ||
        fwrite(startup_tail, 1, sizeof(startup_tail) - 1, file) !=
            sizeof(startup_tail) - 1) {
      ok = false;
      failed_stage = "write startup output";
    }
    if (fclose(file) != 0) {
      ok = false;
      failed_stage = "close startup output";
    }
  }
  if (ok) {
    snprintf(command, sizeof(command),
             "clang --target=avr -mmcu=atmega328p -c %s -o %s", code_path,
             obj_path);
    if (system(command) != 0) {
      ok = false;
      failed_stage = "assemble generated code";
    }
  }
  if (ok) {
    snprintf(command, sizeof(command),
             "clang --target=avr -mmcu=atmega328p -c %s -o %s", start_path,
             start_obj_path);
    if (system(command) != 0) {
      ok = false;
      failed_stage = "assemble startup";
    }
  }
  if (ok) {
    snprintf(command, sizeof(command),
             "ld.lld -N --image-base=0 -Ttext=0 -e _start %s %s -o %s",
             start_obj_path, obj_path, exe_path);
    if (system(command) != 0) {
      ok = false;
      failed_stage = "link executable";
    }
  }
  int runtime_status = -1;
  if (ok) {
    snprintf(command, sizeof(command),
             "timeout 2s qemu-system-avr -M uno -display none -monitor none "
             "-serial stdio -bios %s 2>/dev/null",
             exe_path);
    FILE *runtime = popen(command, "r");
    char output[64] = {0};
    size_t len = runtime ? fread(output, 1, sizeof(output) - 1, runtime) : 0;
    if (runtime) {
      output[len] = '\0';
      runtime_status = pclose(runtime);
    }
    ok = runtime && WIFEXITED(runtime_status) &&
         WEXITSTATUS(runtime_status) == 124 && strchr(output, 'U') != NULL;
    if (!ok)
      failed_stage = "QEMU copy result";
  }

  const int64_t rejected_sizes[] = {-1, (int64_t)UINT16_MAX + 1};
  char reject_err[256] = {0};
  for (size_t rejection = 0;
       ok && rejection < sizeof(rejected_sizes) / sizeof(rejected_sizes[0]);
       ++rejection) {
    nyr_func_t bad_ir = ir;
    for (size_t i = 0; i < bad_ir.len; ++i)
      if (bad_ir.data[i].op == NYIR_COPY_STRUCT)
        bad_ir.data[i].imm = rejected_sizes[rejection];
    nyr_writer_t rejected = {0};
    reject_err[0] = '\0';
    const bool verified = rejected_sizes[rejection] < 0 ||
                          nyr_opt_verify(&bad_ir, "portable-avr-copy-size");
    const bool rejected_size =
        verified &&
        !nyr_avr_emit_ir(&rejected, &target, &bad_ir, "rejected_copy_size",
                         false, reject_err, sizeof(reject_err)) &&
        strstr(reject_err, "16-bit address space");
    free(rejected.data);
    if (!rejected_size) {
      ok = false;
      failed_stage = rejected_sizes[rejection] < 0 ? "negative-size rejection"
                                                   : "oversized-size rejection";
    }
  }
  if (!ok)
    fprintf(stderr, "[portable-avr] struct copy failed at %s: %s\n",
            failed_stage ? failed_stage : "unknown stage",
            err[0] ? err : (reject_err[0] ? reject_err : "runtime mismatch"));

  if (ok) {
    unlink(code_path);
    unlink(start_path);
    unlink(obj_path);
    unlink(start_obj_path);
    unlink(exe_path);
  } else {
    fprintf(stderr, "[portable-avr] artifacts: %s %s %s %s %s\n", code_path,
            start_path, obj_path, start_obj_path, exe_path);
  }
  free(assembly.data);
  nyr_ir_func_free(&ir);
  return ok;
#endif
}

int main(void) {
  if (!check_import_signatures() || !check_wasm_bounds_check())
    return 1;
  static const struct {
    int64_t count;
    int64_t result;
  } cases[] = {
      {0, INT64_C(0xf2345678)},  {1, INT64_C(0x791a2b3c)},
      {8, INT64_C(0x78f23456)},  {31, INT64_C(0xe468acf1)},
      {32, INT64_C(0xf2345678)}, {-1, INT64_C(0xe468acf1)},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (!check_case(INT64_C(0x01234567f2345678), cases[i].count,
                    cases[i].result))
      return 1;
  }
  if (!check_avr_bswap(false, UINT64_C(0x0123456789abcdef),
                       UINT64_C(0xefcdab8967452301)) ||
      !check_avr_bswap(true, UINT64_C(0x0123456789abcdef),
                       UINT64_C(0x00000000efcdab89)) ||
      !check_avr_copy_struct())
    return 1;
  if (!check_executable_wat())
    return 1;
  if (!check_wasm_float_ops())
    return 1;
  if (!check_wasm_memory_traps())
    return 1;
  if (!check_wasm_copy_struct())
    return 1;
  if (!check_cross_struct_copy(NY_NATIVE_BACKEND_MIPS, nyr_mips_emit_ir,
                               "mips64", "qemu-mips64") ||
      !check_cross_struct_copy(NY_NATIVE_BACKEND_POWERPC, nyr_powerpc_emit_ir,
                               "powerpc64", "qemu-ppc64") ||
      !check_cross_struct_copy(NY_NATIVE_BACKEND_AARCH64, nyr_aarch64_emit_ir,
                               "aarch64", "qemu-aarch64") ||
      !check_cross_struct_copy(NY_NATIVE_BACKEND_ARM, nyr_arm_emit_ir, "arm",
                               "qemu-arm") ||
      !check_cross_struct_copy(NY_NATIVE_BACKEND_X86, nyr_i386_emit_ir, "i386",
                               "qemu-i386") ||
      !check_cross_struct_copy(NY_NATIVE_BACKEND_RISCV, nyr_riscv_emit_ir,
                               "riscv64", "qemu-riscv64"))
    return 1;
  if (!check_bounds_case(0, 8, false, false) ||
      !check_bounds_case(8, 8, false, true) ||
      !check_bounds_case(-1, 8, true, true) ||
      !check_bounds_case(0, 0, true, true))
    return 1;
  static const struct {
    nyr_backend_t backend;
    portable_emitter_fn emit;
    const char *name;
  } portable_targets[] = {
      {NY_NATIVE_BACKEND_BPF, nyr_bpf_emit_ir, "bpf"},
      {NY_NATIVE_BACKEND_MIPS, nyr_mips_emit_ir, "mips"},
      {NY_NATIVE_BACKEND_POWERPC, nyr_powerpc_emit_ir, "powerpc"},
      {NY_NATIVE_BACKEND_AVR, nyr_avr_emit_ir, "avr"},
      {NY_NATIVE_BACKEND_WASM, nyr_wasm_emit_ir, "wasm"},
  };
  for (size_t i = 0; i < sizeof(portable_targets) / sizeof(portable_targets[0]);
       ++i) {
    if (portable_targets[i].backend == NY_NATIVE_BACKEND_MIPS) {
      if (!check_bounds_supported(
              portable_targets[i].backend, portable_targets[i].emit,
              portable_targets[i].name, "bltz", "slt", "break"))
        return 1;
      continue;
    }
    if (portable_targets[i].backend == NY_NATIVE_BACKEND_POWERPC) {
      if (!check_bounds_supported(
              portable_targets[i].backend, portable_targets[i].emit,
              portable_targets[i].name, "cmpdi", "cmpd", "tw"))
        return 1;
      continue;
    }
    if (portable_targets[i].backend == NY_NATIVE_BACKEND_WASM ||
        portable_targets[i].backend == NY_NATIVE_BACKEND_AVR)
      continue;
    if (!check_bounds_rejected(portable_targets[i].backend,
                               portable_targets[i].emit,
                               portable_targets[i].name))
      return 1;
  }
  return 0;
}
