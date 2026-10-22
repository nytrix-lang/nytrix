#include <nyr/nyr.h>

#include "base/util.h"
#include "code/backend/ir/api_internal.h"
#include "code/backend/ir/internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

bool nyr_emit_object_for_target(const nyr_func_t *func, const char *path,
                                const char *symbol, const char *target_triple,
                                char *err, size_t err_len) {
  if (!func || !path || !symbol || !*symbol) {
    nyr_api_error(err, err_len, "function, path, and symbol are required");
    return false;
  }
  if (!target_triple || !*target_triple) {
    nyr_api_error(err, err_len, "target triple is required");
    return false;
  }
  if (!nyr_ir_verify(func, err, err_len))
    return false;
  ny_native_target_info_t target;
  if (!nyr_target_info_for_triple(target_triple, &target, err, err_len))
    return false;
  if (target.pointer_bits != 64 ||
      !nyr_api_target_has(&target, NY_NATIVE_CAP_ASM_OBJECT)) {
    nyr_api_error(err, err_len,
                  "Nyr object emission target or object format is unsupported");
    return false;
  }
  bool ok = false;
  if (nyr_api_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT)) {
    if (target.target == NY_NATIVE_TARGET_AARCH64)
      ok = nyr_native_emit_elf64_aarch64_object_from_nirs(
          func, NULL, NULL, 0, &target, path, symbol, false, err, err_len);
    else
      ok = nyr_native_emit_elf64_object_from_nir(func, &target, path, symbol,
                                                 false, err, err_len);
  } else if (nyr_api_target_has(&target, NY_NATIVE_CAP_COFF_OBJECT))
    ok = nyr_native_emit_coff_x64_object_from_nir(func, &target, path, symbol,
                                                  false, err, err_len);
  else if (nyr_api_target_has(&target, NY_NATIVE_CAP_MACHO_OBJECT))
    ok = nyr_native_emit_macho_x64_object_from_nir(func, &target, path, symbol,
                                                   false, err, err_len);
  else {
    nyr_api_error(err, err_len,
                  "Nyr object emission target or object format is unsupported");
    return false;
  }
  if (ok)
    nyr_api_clear_error(err, err_len);
  return ok;
}

bool nyr_emit_assembly(const nyr_func_t *func, const char *path,
                       const char *symbol, char *err, size_t err_len) {
  if (!func || !path || !*path || !symbol || !*symbol) {
    nyr_api_error(err, err_len, "function, path, and symbol are required");
    return false;
  }
  if (!nyr_verify(func, err, err_len))
    return false;
  nyr_target_t host;
  if (!nyr_host_target(&host, err, err_len))
    return false;
  nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_X86_64, NY_NATIVE_ABI_AUTO, host.target, false);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &request) ||
      target.target != NY_NATIVE_TARGET_X86_64 ||
      !nyr_api_target_has(&target, NY_NATIVE_CAP_NIR_ASM)) {
    nyr_api_error(err, err_len,
                  "Nyr assembly text is unavailable for the host target");
    return false;
  }

  nyr_func_t copy;
  nyr_func_init(&copy);
  if (!nyr_func_clone(func, &copy, err, err_len))
    return false;

  nyr_writer_t assembly = {0};
  bool ok = nyr_put(&assembly, "# Nyr native x86-64 assembly\n\t.text\n") &&
            nyr_x86_64_emit_ir(&assembly, &target, &copy, symbol, false, err,
                               err_len);
  if (ok) {
    ok = ny_write_file(path, assembly.data ? assembly.data : "",
                       assembly.len) == 0;
    if (!ok)
      nyr_api_error(err, err_len, "failed to write Nyr assembly output");
  }
  free(assembly.data);
  nyr_func_destroy(&copy);
  if (ok)
    nyr_api_clear_error(err, err_len);
  else if (!err || !err_len || !err[0])
    nyr_api_error(err, err_len, "Nyr x86-64 assembly emission failed");
  return ok;
}

bool nyr_emit_object(const nyr_func_t *func, const char *path,
                     const char *symbol, char *err, size_t err_len) {
  nyr_target_t target;
  if (!nyr_host_target(&target, err, err_len))
    return false;
  return nyr_emit_object_for_target(func, path, symbol, target.target, err,
                                    err_len);
}

nyr_func_t *nyr_func_create(void) {
  nyr_func_t *func = malloc(sizeof(*func));
  if (func)
    nyr_func_init(func);
  return func;
}

void nyr_func_free(nyr_func_t *func) {
  if (!func)
    return;
  nyr_func_destroy(func);
  free(func);
}

void nyr_func_init(nyr_func_t *func) {
  if (func)
    memset(func, 0, sizeof(*func));
}

void nyr_func_destroy(nyr_func_t *func) { nyr_ir_func_free(func); }

bool nyr_func_clone(const nyr_func_t *src, nyr_func_t *dst, char *err,
                    size_t err_len) {
  if (!src || !dst) {
    nyr_api_error(err, err_len,
                  "source and destination functions are required");
    return false;
  }
  if (src == dst) {
    nyr_api_error(err, err_len, "source and destination functions must differ");
    return false;
  }
  if (!nyr_ir_func_clone(src, dst)) {
    nyr_api_error(err, err_len,
                  "function clone failed; destination must be empty and source "
                  "metadata valid");
    return false;
  }
  nyr_api_clear_error(err, err_len);
  return true;
}

bool nyr_func_set_signature(nyr_func_t *func, const nyr_param_desc_t *params,
                            size_t param_count, uint32_t return_aggregate_size,
                            const uint8_t return_aggregate_classes[2],
                            char *err, size_t err_len) {
  if (!func || (param_count && !params) ||
      (return_aggregate_size && !return_aggregate_classes)) {
    nyr_api_error(err, err_len, "function signature has invalid arguments");
    return false;
  }
  if (func->len || func->param_count || func->param_types || func->param_abi ||
      func->return_aggregate_size) {
    nyr_api_error(err, err_len,
                  "function signature must be set before instructions");
    return false;
  }
  if (param_count > SIZE_MAX / sizeof(nyr_param_type_t)) {
    nyr_api_error(err, err_len, "function signature is too large");
    return false;
  }
  nyr_param_type_t *types = NULL;
  uint32_t *abi = NULL;
  bool has_aggregate = false;
  if (param_count) {
    types = malloc(param_count * sizeof(*types));
    if (!types)
      goto signature_allocation_failed;
    for (size_t i = 0; i < param_count; ++i) {
      if (params[i].type < NYIR_PARAM_I64 ||
          params[i].type > NYIR_PARAM_AGGREGATE ||
          (params[i].type != NYIR_PARAM_AGGREGATE && params[i].aggregate_abi)) {
        free(types);
        nyr_api_error(err, err_len, "function parameter descriptor is invalid");
        return false;
      }
      types[i] = params[i].type;
      has_aggregate |= params[i].type == NYIR_PARAM_AGGREGATE;
    }
    if (has_aggregate) {
      if (param_count > SIZE_MAX / sizeof(*abi) ||
          !(abi = calloc(param_count, sizeof(*abi))))
        goto signature_allocation_failed;
      for (size_t i = 0; i < param_count; ++i)
        abi[i] = params[i].aggregate_abi;
    }
  }
  if (!return_aggregate_size && return_aggregate_classes &&
      (return_aggregate_classes[0] || return_aggregate_classes[1])) {
    free(types);
    free(abi);
    nyr_api_error(err, err_len,
                  "aggregate return classes require a nonzero size");
    return false;
  }
  func->param_types = types;
  func->param_abi = abi;
  func->param_count = param_count;
  func->return_aggregate_size = return_aggregate_size;
  if (return_aggregate_size) {
    func->return_aggregate_classes[0] = return_aggregate_classes[0];
    func->return_aggregate_classes[1] = return_aggregate_classes[1];
  }
  nyr_api_clear_error(err, err_len);
  return true;

signature_allocation_failed:
  free(types);
  free(abi);
  nyr_api_error(err, err_len, "function signature allocation failed");
  return false;
}

bool nyr_emit(nyr_func_t *func, nyr_inst_t inst, int *value_id, char *err,
              size_t err_len) {
  if (!func) {
    nyr_api_error(err, err_len, "function must not be null");
    return false;
  }
  if (inst.op < 0 || inst.op >= NYIR_OP_COUNT) {
    nyr_api_error(err, err_len, "instruction opcode is out of range");
    return false;
  }
  int *extra_args = NULL;
  uint32_t *arg_sizes = NULL;
  nyr_phi_incoming_t *phi_incoming = NULL;
  if ((inst.extra_args_len && !inst.extra_args) ||
      (inst.phi_incoming_len && !inst.phi_incoming) ||
      (inst.arg_sizes && (inst.op != NYIR_CALL || inst.imm < 0 ||
                          inst.imm > NYIR_CALL_MAX_ARGS)) ||
      (inst.extra_args_len && inst.op != NYIR_CALL) ||
      (inst.phi_incoming_len && inst.op != NYIR_PHI) ||
      (inst.op == NYIR_CALL &&
       (inst.imm < 0 || inst.imm > NYIR_CALL_MAX_ARGS ||
        (inst.imm > 6 &&
         (!inst.extra_args || inst.extra_args_len != (size_t)(inst.imm - 6))) ||
        (inst.imm <= 6 && inst.extra_args_len != 0)))) {
    nyr_api_error(err, err_len, "instruction metadata has an invalid length");
    return false;
  }
  if (!inst.extra_args_len)
    inst.extra_args = NULL;
  if (!inst.phi_incoming_len)
    inst.phi_incoming = NULL;
  if (inst.extra_args_len) {
    if (inst.extra_args_len > SIZE_MAX / sizeof(*extra_args) ||
        !(extra_args = malloc(inst.extra_args_len * sizeof(*extra_args))))
      goto metadata_allocation_failed;
    memcpy(extra_args, inst.extra_args,
           inst.extra_args_len * sizeof(*extra_args));
    inst.extra_args = extra_args;
  }
  if (inst.arg_sizes) {
    size_t count = (size_t)inst.imm;
    if (count == 0) {
      inst.arg_sizes = NULL;
    } else {
      if (count > SIZE_MAX / sizeof(*arg_sizes) ||
          !(arg_sizes = malloc(count * sizeof(*arg_sizes))))
        goto metadata_allocation_failed;
      memcpy(arg_sizes, inst.arg_sizes, count * sizeof(*arg_sizes));
      inst.arg_sizes = arg_sizes;
    }
  }
  if (inst.phi_incoming_len) {
    if (inst.phi_incoming_len > SIZE_MAX / sizeof(*phi_incoming) ||
        !(phi_incoming = malloc(inst.phi_incoming_len * sizeof(*phi_incoming))))
      goto metadata_allocation_failed;
    memcpy(phi_incoming, inst.phi_incoming,
           inst.phi_incoming_len * sizeof(*phi_incoming));
    inst.phi_incoming = phi_incoming;
  }
  size_t before = func->len;
  int value = nyr_ir_emit(func, inst);
  if (func->len == before) {
    free(extra_args);
    free(arg_sizes);
    free(phi_incoming);
    nyr_api_error(err, err_len, "instruction append failed");
    return false;
  }
  if (value_id)
    *value_id = value;
  nyr_api_clear_error(err, err_len);
  return true;

metadata_allocation_failed:
  free(extra_args);
  free(arg_sizes);
  free(phi_incoming);
  nyr_api_error(err, err_len, "instruction metadata allocation failed");
  return false;
}

nyr_inst_t nyr_inst_init(nyr_op_t op) {
  return (nyr_inst_t){.op = op,
                      .dst = -1,
                      .a = -1,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1};
}

bool nyr_emit_const_i64(nyr_func_t *func, int64_t value, int *value_id,
                        char *err, size_t err_len) {
  nyr_inst_t inst = nyr_inst_init(NYIR_CONST_I64);
  inst.imm = value;
  inst.scalar_class = NYIR_SCALAR_I64_RAW;
  return nyr_emit(func, inst, value_id, err, err_len);
}

bool nyr_emit_const_f64(nyr_func_t *func, double value, int *value_id,
                        char *err, size_t err_len) {
  nyr_inst_t inst = nyr_inst_init(NYIR_CONST_F64);
  memcpy(&inst.imm, &value, sizeof(value));
  inst.scalar_class = NYIR_SCALAR_F64;
  return nyr_emit(func, inst, value_id, err, err_len);
}

bool nyr_emit_binary(nyr_func_t *func, nyr_op_t op, int lhs, int rhs,
                     nyr_scalar_class_t result_class, int *value_id, char *err,
                     size_t err_len) {
  nyr_inst_t inst = nyr_inst_init(op);
  inst.a = lhs;
  inst.b = rhs;
  inst.scalar_class = result_class;
  return nyr_emit(func, inst, value_id, err, err_len);
}

bool nyr_emit_load_local(nyr_func_t *func, int64_t slot, int *value_id,
                         char *err, size_t err_len) {
  if (slot < 0) {
    nyr_api_error(err, err_len, "local load requires a non-negative slot");
    return false;
  }
  nyr_inst_t inst = nyr_inst_init(NYIR_LOAD_LOCAL);
  inst.imm = slot;
  inst.scalar_class = NYIR_SCALAR_I64_RAW;
  return nyr_emit(func, inst, value_id, err, err_len);
}

bool nyr_emit_store_local(nyr_func_t *func, int64_t slot, int value, char *err,
                          size_t err_len) {
  if (slot < 0 || value < 0) {
    nyr_api_error(err, err_len,
                  "local store requires a non-negative slot and value");
    return false;
  }
  nyr_inst_t inst = nyr_inst_init(NYIR_STORE_LOCAL);
  inst.imm = slot;
  inst.a = value;
  inst.scalar_class = NYIR_SCALAR_I64_RAW;
  return nyr_emit(func, inst, NULL, err, err_len);
}

bool nyr_emit_load_i64(nyr_func_t *func, int address, int64_t offset,
                       int *value_id, char *err, size_t err_len) {
  if (address < 0) {
    nyr_api_error(err, err_len, "memory load requires a valid address value");
    return false;
  }
  nyr_inst_t inst = nyr_inst_init(NYIR_LOAD_I64);
  inst.a = address;
  inst.imm = offset;
  inst.scalar_class = NYIR_SCALAR_I64_RAW;
  return nyr_emit(func, inst, value_id, err, err_len);
}

bool nyr_emit_store_i64(nyr_func_t *func, int address, int value,
                        int64_t offset, char *err, size_t err_len) {
  if (address < 0 || value < 0) {
    nyr_api_error(err, err_len,
                  "memory store requires valid address and value IDs");
    return false;
  }
  nyr_inst_t inst = nyr_inst_init(NYIR_STORE_I64);
  inst.a = address;
  inst.c = value;
  inst.imm = offset;
  inst.scalar_class = NYIR_SCALAR_I64_RAW;
  return nyr_emit(func, inst, NULL, err, err_len);
}

static bool nyr_emit_call_with_flags(
  nyr_func_t *func, const char *symbol, const int *args, size_t arg_count,
  nyr_scalar_class_t result_class, unsigned flags, int *value_id, char *err,
  size_t err_len) {
  if (!symbol || !*symbol || arg_count > NYIR_CALL_MAX_ARGS ||
      (arg_count && !args)) {
    nyr_api_error(err, err_len, "call requires a symbol and valid arguments");
    return false;
  }
  nyr_inst_t inst = nyr_inst_init(NYIR_CALL);
  int *operands[] = {&inst.a, &inst.b, &inst.c, &inst.d, &inst.e, &inst.f};
  for (size_t i = 0; i < arg_count && i < 6; ++i)
    *operands[i] = args[i];
  inst.imm = (int64_t)arg_count;
  inst.symbol = symbol;
  inst.scalar_class = result_class;
  inst.flags = flags;
  if (arg_count > 6) {
    inst.extra_args = (int *)(args + 6);
    inst.extra_args_len = arg_count - 6;
  }
  return nyr_emit(func, inst, value_id, err, err_len);
}

bool nyr_emit_call(nyr_func_t *func, const char *symbol, const int *args,
                   size_t arg_count, nyr_scalar_class_t result_class,
                   int *value_id, char *err, size_t err_len) {
  return nyr_emit_call_with_flags(func, symbol, args, arg_count, result_class,
                                  0, value_id, err, err_len);
}

bool nyr_emit_extern_call(nyr_func_t *func, const char *symbol,
                          const int *args, size_t arg_count,
                          nyr_scalar_class_t result_class, int *value_id,
                          char *err, size_t err_len) {
  return nyr_emit_call_with_flags(func, symbol, args, arg_count, result_class,
                                  NYIR_INST_F_EXTERN, value_id, err, err_len);
}

bool nyr_emit_phi(nyr_func_t *func, const nyr_phi_incoming_t *incoming,
                  size_t incoming_count, int *value_id, char *err,
                  size_t err_len) {
  if (incoming_count && !incoming) {
    nyr_api_error(err, err_len, "PHI incoming values must not be null");
    return false;
  }
  nyr_inst_t inst = nyr_inst_init(NYIR_PHI);
  inst.phi_incoming = (nyr_phi_incoming_t *)incoming;
  inst.phi_incoming_len = incoming_count;
  return nyr_emit(func, inst, value_id, err, err_len);
}

bool nyr_emit_compare_i64(nyr_func_t *func, int lhs, int rhs, nyr_cmp_t cmp,
                          int *value_id, char *err, size_t err_len) {
  nyr_inst_t inst = nyr_inst_init(NYIR_CMP_I64);
  inst.a = lhs;
  inst.b = rhs;
  inst.cmp = cmp;
  inst.scalar_class = NYIR_SCALAR_I64_RAW;
  return nyr_emit(func, inst, value_id, err, err_len);
}

static bool nyr_emit_control(nyr_func_t *func, nyr_op_t op, int value,
                             int64_t label, char *err, size_t err_len) {
  nyr_inst_t inst = nyr_inst_init(op);
  inst.imm = label;
  if (op == NYIR_BR_IF)
    inst.a = value;
  else if (op == NYIR_RET)
    inst.a = value;
  return nyr_emit(func, inst, NULL, err, err_len);
}

bool nyr_emit_label(nyr_func_t *func, int64_t label, char *err,
                    size_t err_len) {
  return nyr_emit_control(func, NYIR_LABEL, -1, label, err, err_len);
}

bool nyr_emit_branch(nyr_func_t *func, int64_t label, char *err,
                     size_t err_len) {
  return nyr_emit_control(func, NYIR_BR, -1, label, err, err_len);
}

bool nyr_emit_branch_if(nyr_func_t *func, int condition, int64_t label,
                        char *err, size_t err_len) {
  return nyr_emit_control(func, NYIR_BR_IF, condition, label, err, err_len);
}

bool nyr_emit_return(nyr_func_t *func, int value, char *err, size_t err_len) {
  return nyr_emit_control(func, NYIR_RET, value, 0, err, err_len);
}

bool nyr_verify(const nyr_func_t *func, char *err, size_t err_len) {
  bool ok = nyr_ir_verify(func, err, err_len);
  if (ok)
    nyr_api_clear_error(err, err_len);
  return ok;
}

bool nyr_optimize(nyr_func_t *func, nyr_opt_level_t opt_level, char *err,
                  size_t err_len) {
  if (!func) {
    nyr_api_error(err, err_len, "function must not be null");
    return false;
  }
  if (opt_level < NYR_OPT_O0 || opt_level > NYR_OPT_O3) {
    nyr_api_error(err, err_len, "optimization level must be between O0 and O3");
    return false;
  }
  nyr_func_t candidate;
  nyr_func_init(&candidate);
  if (!nyr_func_clone(func, &candidate, err, err_len))
    return false;
  if (!nyr_ir_optimize(&candidate, (int)opt_level)) {
    nyr_func_destroy(&candidate);
    nyr_api_error(err, err_len, "NYIR optimization pipeline failed");
    return false;
  }
  nyr_func_destroy(func);
  *func = candidate;
  nyr_api_clear_error(err, err_len);
  return true;
}
