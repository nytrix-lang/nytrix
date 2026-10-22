/*
 * Language-owned source-program adapters for native artifact emission.
 */
#include "base/common.h"
#include "base/debug.h"
#include "base/time.h"
#include "base/trace.h"
#include "base/util.h"
#include "code/backend/abi.h"
#include "code/backend/internal.h"
#include "code/backend/ir/module.h"
#include "code/backend/native.h"
#include "code/backend/object/internal.h"
#include "code/lang/lower.h"
#include "code/lang/pipeline/build.h"
#include "code/lang/pipeline/legacy_backend.h"
#include "code/lang/pipeline/native.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool ny_native_target_has(const ny_native_target_info_t *target,
                                 nyr_target_cap_t cap) {
  return target && (target->caps & (unsigned)cap) != 0;
}

static bool ny_native_x64_win64_aggregate_supported(const nyr_func_t *func,
                                                    const char *name, char *err,
                                                    size_t err_len) {
  if (!func)
    return true;
  bool aggregate = func->return_aggregate_size > 0;
  for (size_t i = 0; !aggregate && i < func->len; ++i) {
    const nyr_inst_t *ret = &func->data[i];
    if (ret->op != NYIR_RET || ret->a < 0)
      continue;
    for (size_t j = 0; j < func->len; ++j) {
      const nyr_inst_t *call = &func->data[j];
      if (call->op == NYIR_CALL &&
          call->scalar_class == NYIR_SCALAR_AGGREGATE &&
          (call->dst == ret->a || call->a == ret->a)) {
        aggregate = true;
        break;
      }
    }
  }
  if (!aggregate)
    return true;
  nyr_set_error(err, err_len,
                "native x86-64 Win64: aggregate return ABI is unsupported in "
                "function %s",
                name && name[0] ? name : "<anonymous>");
  return false;
}

static bool ny_wasm_func_uses_runtime_helpers(const nyr_func_t *func) {
  for (size_t i = 0; func && i < func->len; ++i) {
    const nyr_inst_t *in = &func->data[i];
    if (in->op == NYIR_CALL && in->symbol &&
        (strcmp(in->symbol, "rt_malloc") == 0 ||
         strcmp(in->symbol, "rt_malloc_i64") == 0 ||
         strcmp(in->symbol, "rt_zalloc_raw") == 0 ||
         strcmp(in->symbol, "rt_cstr_to_str") == 0 ||
         strcmp(in->symbol, "rt_alloc_string") == 0 ||
         strcmp(in->symbol, "rt_panic") == 0 ||
         strcmp(in->symbol, "rt_cstr_concat") == 0 ||
         strcmp(in->symbol, "rt_cstr_len") == 0 ||
         strcmp(in->symbol, "rt_i64_to_cstr_raw") == 0))
      return true;
  }
  return false;
}

typedef struct {
  const char *symbol;
  int argc;
  uint64_t i32_args;
  uint64_t f32_args;
  uint64_t f64_args;
  nyr_wasm_import_type_t result_type;
} ny_wasm_import_t;

static bool ny_wasm_external_name(const char *name) {
  if (!name || !name[0] ||
      !(name[0] == '_' || (name[0] >= 'A' && name[0] <= 'Z') ||
        (name[0] >= 'a' && name[0] <= 'z')))
    return false;
  for (const unsigned char *p = (const unsigned char *)name + 1; *p; ++p)
    if (!(*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9')))
      return false;
  return true;
}

static bool ny_wasm_collect_imports(const nyr_func_t *funcs, size_t func_count,
                                    const nyr_func_t *rt_main,
                                    ny_wasm_import_t **imports_out,
                                    size_t *import_count_out, char *err,
                                    size_t err_len) {
  size_t capacity = rt_main ? rt_main->len : 0;
  for (size_t i = 0; i < func_count; ++i) {
    if (funcs[i].len > SIZE_MAX - capacity) {
      nyr_set_error(err, err_len, "WebAssembly import scan size overflow");
      return false;
    }
    capacity += funcs[i].len;
  }
  if (capacity > SIZE_MAX / sizeof(ny_wasm_import_t)) {
    nyr_set_error(err, err_len, "WebAssembly import table size overflow");
    return false;
  }
  ny_wasm_import_t *imports =
      capacity ? calloc(capacity, sizeof(*imports)) : NULL;
  if (capacity && !imports) {
    nyr_set_error(err, err_len, "WebAssembly import table allocation failed");
    return false;
  }
  size_t count = 0;
  for (size_t f = 0; f <= func_count; ++f) {
    const nyr_func_t *func = f == func_count ? rt_main : &funcs[f];
    for (size_t i = 0; func && i < func->len; ++i) {
      const nyr_inst_t *in = &func->data[i];
      if (in->op != NYIR_CALL || !(in->flags & NYIR_INST_F_EXTERN) ||
          !in->symbol || strncmp(in->symbol, "rt_", 3) == 0 ||
          ny_native_runtime_abi_decl(in->symbol) ||
          !ny_wasm_external_name(in->symbol))
        continue;
      int args[NYIR_CALL_MAX_ARGS];
      int argc = 0;
      if (!nyr_call_args(in, func->next_value, args, NYIR_CALL_MAX_ARGS, &argc,
                          NULL, 0) ||
          argc < 0)
        continue;
      nyr_wasm_import_type_t arg_types[NYIR_CALL_MAX_ARGS];
      nyr_wasm_import_type_t result_type = NYR_WASM_IMPORT_UNSUPPORTED;
      if (!nyr_wasm_call_signature(func, in, args, argc, arg_types,
                                   &result_type))
        continue;
      uint64_t i32_args = 0;
      uint64_t f32_args = 0;
      uint64_t f64_args = 0;
      for (int arg = 0; arg < argc; ++arg) {
        if (arg_types[arg] == NYR_WASM_IMPORT_I32)
          i32_args |= UINT64_C(1) << arg;
        else if (arg_types[arg] == NYR_WASM_IMPORT_F32)
          f32_args |= UINT64_C(1) << arg;
        else if (arg_types[arg] == NYR_WASM_IMPORT_F64)
          f64_args |= UINT64_C(1) << arg;
      }
      size_t existing = 0;
      for (; existing < count; ++existing)
        if (strcmp(imports[existing].symbol, in->symbol) == 0)
          break;
      if (existing < count) {
        if (imports[existing].argc != argc ||
            imports[existing].i32_args != i32_args ||
            imports[existing].f32_args != f32_args ||
            imports[existing].f64_args != f64_args ||
            imports[existing].result_type != result_type) {
          nyr_set_error(err, err_len,
                        "WebAssembly import '%s' has inconsistent signatures",
                        in->symbol);
          free(imports);
          return false;
        }
        continue;
      }
      imports[count++] = (ny_wasm_import_t){in->symbol, argc,     i32_args,
                                            f32_args,   f64_args, result_type};
    }
  }
  *imports_out = imports;
  *import_count_out = count;
  return true;
}

static bool ny_wasm_emit_imports(nyr_writer_t *w,
                                 const ny_wasm_import_t *imports,
                                 size_t import_count) {
  for (size_t i = 0; i < import_count; ++i) {
    if (!nyr_printf(w, "  (import \"env\" \"%s\" (func $%s", imports[i].symbol,
                    imports[i].symbol))
      return false;
    for (int arg = 0; arg < imports[i].argc; ++arg) {
      const char *type = (imports[i].i32_args & (UINT64_C(1) << arg))   ? "i32"
                         : (imports[i].f64_args & (UINT64_C(1) << arg)) ? "f64"
                         : (imports[i].f32_args & (UINT64_C(1) << arg)) ? "f32"
                                                                        : "i64";
      if (!nyr_printf(w, " (param %s)", type))
        return false;
    }
    if (imports[i].result_type == NYR_WASM_IMPORT_VOID) {
      if (!nyr_put(w, "))\n"))
        return false;
    } else {
      const char *result =
          imports[i].result_type == NYR_WASM_IMPORT_I32   ? "i32"
          : imports[i].result_type == NYR_WASM_IMPORT_F64 ? "f64"
          : imports[i].result_type == NYR_WASM_IMPORT_F32 ? "f32"
                                                          : "i64";
      if (!nyr_printf(w, " (result %s)))\n", result))
        return false;
    }
  }
  return true;
}

static bool ny_wasm_emit_runtime_helpers(nyr_writer_t *w, char *err,
                                         size_t err_len) {
  size_t data_end = 1024;
  size_t string_count = 0;
  for (size_t i = 0;; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "ny_str_%zu", i);
    size_t len = 0;
    const char *bytes = nyr_strtab_get(name, &len);
    if (!bytes)
      break;
    if (len >= SIZE_MAX - data_end || data_end + len > UINT32_MAX) {
      nyr_set_error(err, err_len,
                    "WebAssembly panic string data exceeds linear memory");
      return false;
    }
    data_end += len + 1;
    ++string_count;
  }

  uint64_t pages =
      data_end > UINT64_MAX - 65535 ? 0 : (data_end + 65535) / 65536;
  if (!pages || pages > 65536) {
    nyr_set_error(err, err_len,
                  "WebAssembly panic string data exceeds linear memory");
    return false;
  }
  if (!nyr_put(w, "  (import \"wasi_snapshot_preview1\" \"fd_write\"\n"
                  "    (func $ny_wasi_fd_write (param i32 i32 i32 i32) "
                  "(result i32)))\n") ||
      !nyr_printf(w, "  (memory (export \"memory\") %" PRIu64 ")\n", pages))
    return false;

  size_t address = 1024;
  for (size_t i = 0; i < string_count; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "ny_str_%zu", i);
    size_t len = 0;
    const char *bytes = nyr_strtab_get(name, &len);
    if (!bytes || !nyr_printf(w, "  (data (i32.const %zu) \"", address))
      return false;
    for (size_t byte = 0; byte < len; ++byte)
      if (!nyr_printf(w, "\\%02x", (unsigned char)bytes[byte]))
        return false;
    if (!nyr_put(w, "\\00\")\n"))
      return false;
    address += len + 1;
  }

  if (!nyr_printf(w, "  (global $ny_heap (mut i32) (i32.const %zu))\n",
                  address) ||
      !nyr_put(w, "  (func $ny_alloc (param $size i32) (result i32)\n"
                  "    (local $base i32) (local $end i32) (local $pages i32)\n"
                  "    (local $current i32)\n"
                  "    global.get $ny_heap\n"
                  "    local.tee $base\n"
                  "    local.get $size\n"
                  "    i32.add\n"
                  "    local.tee $end\n"
                  "    local.get $base\n"
                  "    i32.lt_u\n"
                  "    if unreachable end\n"
                  "    local.get $end\n"
                  "    i32.const -65536\n"
                  "    i32.gt_u\n"
                  "    if unreachable end\n"
                  "    local.get $end\n"
                  "    i32.const 65535\n"
                  "    i32.add\n"
                  "    i32.const 16\n"
                  "    i32.shr_u\n"
                  "    local.set $pages\n"
                  "    memory.size\n"
                  "    local.tee $current\n"
                  "    local.get $pages\n"
                  "    i32.lt_u\n"
                  "    if\n"
                  "      local.get $pages\n"
                  "      local.get $current\n"
                  "      i32.sub\n"
                  "      memory.grow\n"
                  "      i32.const -1\n"
                  "      i32.eq\n"
                  "      if unreachable end\n"
                  "    end\n"
                  "    local.get $end\n"
                  "    global.set $ny_heap\n"
                  "    local.get $base)\n"
                  "  (func $rt_cstr_len (param $text i64) (result i64)\n"
                  "    (local $pointer i32) (local $length i32)\n"
                  "    local.get $text\n"
                  "    i32.wrap_i64\n"
                  "    local.set $pointer\n"
                  "    block $length_done\n"
                  "      loop $length_scan\n"
                  "        local.get $pointer\n"
                  "        local.get $length\n"
                  "        i32.add\n"
                  "        i32.load8_u\n"
                  "        i32.eqz\n"
                  "        br_if $length_done\n"
                  "        local.get $length\n"
                  "        i32.const 1\n"
                  "        i32.add\n"
                  "        local.set $length\n"
                  "        br $length_scan\n"
                  "      end\n"
                  "    end\n"
                  "    local.get $length\n"
                  "    i64.extend_i32_u)\n"))
    return false;

  if (!nyr_put(w,
               "  (func $rt_cstr_concat (param $left i64) (param $right i64) "
               "(result i64)\n"
               "    (local $left_pointer i32) (local $right_pointer i32)\n"
               "    (local $left_length i32) (local $right_length i32)\n"
               "    (local $output i32) (local $index i32)\n"
               "    local.get $left\n"
               "    i32.wrap_i64\n"
               "    local.set $left_pointer\n"
               "    local.get $right\n"
               "    i32.wrap_i64\n"
               "    local.set $right_pointer\n"
               "    local.get $left\n"
               "    call $rt_cstr_len\n"
               "    i32.wrap_i64\n"
               "    local.set $left_length\n"
               "    local.get $right\n"
               "    call $rt_cstr_len\n"
               "    i32.wrap_i64\n"
               "    local.set $right_length\n"
               "    local.get $left_length\n"
               "    local.get $right_length\n"
               "    i32.add\n"
               "    i32.const 1\n"
               "    i32.add\n"
               "    call $ny_alloc\n"
               "    local.set $output\n"
               "    block $left_done\n"
               "      loop $left_copy\n"
               "        local.get $index\n"
               "        local.get $left_length\n"
               "        i32.ge_u\n"
               "        br_if $left_done\n"
               "        local.get $output\n"
               "        local.get $index\n"
               "        i32.add\n"
               "        local.get $left_pointer\n"
               "        local.get $index\n"
               "        i32.add\n"
               "        i32.load8_u\n"
               "        i32.store8\n"
               "        local.get $index\n"
               "        i32.const 1\n"
               "        i32.add\n"
               "        local.set $index\n"
               "        br $left_copy\n"
               "      end\n"
               "    end\n"
               "    block $right_done\n"
               "      loop $right_copy\n"
               "        local.get $index\n"
               "        local.get $left_length\n"
               "        local.get $right_length\n"
               "        i32.add\n"
               "        i32.ge_u\n"
               "        br_if $right_done\n"
               "        local.get $output\n"
               "        local.get $index\n"
               "        i32.add\n"
               "        local.get $right_pointer\n"
               "        local.get $index\n"
               "        local.get $left_length\n"
               "        i32.sub\n"
               "        i32.add\n"
               "        i32.load8_u\n"
               "        i32.store8\n"
               "        local.get $index\n"
               "        i32.const 1\n"
               "        i32.add\n"
               "        local.set $index\n"
               "        br $right_copy\n"
               "      end\n"
               "    end\n"
               "    local.get $output\n"
               "    local.get $index\n"
               "    i32.add\n"
               "    i32.const 0\n"
               "    i32.store8\n"
               "    local.get $output\n"
               "    i64.extend_i32_u)\n"))
    return false;

  if (!nyr_put(w,
               "  (func $rt_i64_to_cstr_raw (param $value i64) (result i64)\n"
               "    (local $magnitude i64) (local $negative i32)\n"
               "    (local $output i32) (local $cursor i32)\n"
               "    local.get $value\n"
               "    i64.const 0\n"
               "    i64.lt_s\n"
               "    if\n"
               "      i32.const 1\n"
               "      local.set $negative\n"
               "      i64.const 0\n"
               "      local.get $value\n"
               "      i64.sub\n"
               "      local.set $magnitude\n"
               "    else\n"
               "      local.get $value\n"
               "      local.set $magnitude\n"
               "    end\n"
               "    i32.const 22\n"
               "    call $ny_alloc\n"
               "    local.set $output\n"
               "    local.get $output\n"
               "    i32.const 21\n"
               "    i32.add\n"
               "    local.set $cursor\n"
               "    local.get $cursor\n"
               "    i32.const 0\n"
               "    i32.store8\n"
               "    local.get $cursor\n"
               "    i32.const 1\n"
               "    i32.sub\n"
               "    local.set $cursor\n"
               "    block $digits_done\n"
               "      loop $digits\n"
               "        local.get $cursor\n"
               "        local.get $magnitude\n"
               "        i64.const 10\n"
               "        i64.rem_u\n"
               "        i32.wrap_i64\n"
               "        i32.const 48\n"
               "        i32.add\n"
               "        i32.store8\n"
               "        local.get $magnitude\n"
               "        i64.const 10\n"
               "        i64.div_u\n"
               "        local.set $magnitude\n"
               "        local.get $magnitude\n"
               "        i64.eqz\n"
               "        br_if $digits_done\n"
               "        local.get $cursor\n"
               "        i32.const 1\n"
               "        i32.sub\n"
               "        local.set $cursor\n"
               "        br $digits\n"
               "      end\n"
               "    end\n"
               "    local.get $negative\n"
               "    if\n"
               "      local.get $cursor\n"
               "      i32.const 1\n"
               "      i32.sub\n"
               "      local.set $cursor\n"
               "      local.get $cursor\n"
               "      i32.const 45\n"
               "      i32.store8\n"
               "    end\n"
               "    local.get $cursor\n"
               "    i64.extend_i32_u)\n"))
    return false;

  return nyr_put(w,
                 "  (func $rt_malloc_i64 (param $size i64) (result i64)\n"
                 "    (local $alloc_size i32)\n"
                 "    local.get $size\n"
                 "    i64.const 0\n"
                 "    i64.lt_s\n"
                 "    if unreachable end\n"
                 "    local.get $size\n"
                 "    i64.const 4294967295\n"
                 "    i64.gt_u\n"
                 "    if unreachable end\n"
                 "    local.get $size\n"
                 "    i32.wrap_i64\n"
                 "    local.tee $alloc_size\n"
                 "    i32.eqz\n"
                 "    if\n"
                 "      i32.const 1\n"
                 "      local.set $alloc_size\n"
                 "    end\n"
                 "    local.get $alloc_size\n"
                 "    call $ny_alloc\n"
                 "    i64.extend_i32_u)\n"
                 "  (func $rt_malloc (param $size i64) (result i64)\n"
                 "    local.get $size\n"
                 "    call $rt_malloc_i64)\n"
                 "  (func $rt_zalloc_raw (param $size i64) (result i64)\n"
                 "    (local $alloc_size i32) (local $output i32)\n"
                 "    local.get $size\n"
                 "    i64.const 0\n"
                 "    i64.lt_s\n"
                 "    if unreachable end\n"
                 "    local.get $size\n"
                 "    i64.const 4294967295\n"
                 "    i64.gt_u\n"
                 "    if unreachable end\n"
                 "    local.get $size\n"
                 "    i32.wrap_i64\n"
                 "    local.tee $alloc_size\n"
                 "    call $ny_alloc\n"
                 "    local.tee $output\n"
                 "    i32.const 0\n"
                 "    local.get $alloc_size\n"
                 "    memory.fill\n"
                 "    local.get $output\n"
                 "    i64.extend_i32_u)\n"
                 "  (func $rt_cstr_to_str (param $message i64) (result i64)\n"
                 "    local.get $message)\n"
                 "  (func $rt_alloc_string (param $message i64) (result i64)\n"
                 "    local.get $message)\n"
                 "  (func $rt_panic (param $message i64) (result i64)\n"
                 "    (local $pointer i32) (local $length i32)\n"
                 "    local.get $message\n"
                 "    i32.wrap_i64\n"
                 "    local.set $pointer\n"
                 "    i32.const 0\n"
                 "    local.set $length\n"
                 "    block $string_end\n"
                 "      loop $string_scan\n"
                 "        local.get $pointer\n"
                 "        local.get $length\n"
                 "        i32.add\n"
                 "        i32.load8_u\n"
                 "        i32.eqz\n"
                 "        br_if $string_end\n"
                 "        local.get $length\n"
                 "        i32.const 1\n"
                 "        i32.add\n"
                 "        local.set $length\n"
                 "        br $string_scan\n"
                 "      end\n"
                 "    end\n"
                 "    i32.const 0\n"
                 "    local.get $pointer\n"
                 "    i32.store\n"
                 "    i32.const 4\n"
                 "    local.get $length\n"
                 "    i32.store\n"
                 "    i32.const 1\n"
                 "    i32.const 0\n"
                 "    i32.const 1\n"
                 "    i32.const 8\n"
                 "    call $ny_wasi_fd_write\n"
                 "    drop\n"
                 "    unreachable)\n");
}

bool ny_pipeline_emit_asm_entry(const program_t *prog, const ny_options *opt,
                                const char *path, const char *entry_name,
                                bool tag_return, char *err, size_t err_len) {
  nyr_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyr_set_preserve_phis(true);
  nyr_set_pass_controls(opt ? opt->nyr_disable_pass : NULL,
                         opt ? opt->nyr_stop_after : NULL);
  nyr_set_verify_each_pass(opt && opt->nyr_ir_verify);
  nyr_set_rollback_on_verify_fail(opt && opt->nyr_rollback_on_verify_fail);
  nyr_set_global_inst_cap(opt ? opt->nyr_global_inst_cap : 0);
  nyr_set_pass_growth_factor(opt ? opt->nyr_pass_growth_factor : 0.0);
  nyr_set_track_value(opt ? opt->nyr_track_value : -1);
  ny_dbg_init_from_verbosity(opt ? opt->verbosity : 0);
  nyr_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  if (!opt) {
    nyr_set_error(err, err_len, "native backend is not enabled");
    return false;
  }
  const nyr_target_request_t target_request = nyr_target_request_make(
      opt->native_backend, opt->native_abi, opt->host_triple, opt->opt_size);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &target_request)) {
    nyr_set_error(err, err_len, "native backend is not enabled");
    return false;
  }
  bool target_has_ast_fallback =
      ny_native_target_has(&target, NY_NATIVE_CAP_AST_FALLBACK);
  if (!ny_native_target_has(&target, NY_NATIVE_CAP_NIR_ASM)) {
    nyr_set_error(err, err_len,
                  "native backend target '%s' is registered (abi=%s "
                  "object=%s ptr=%zub) but no emitter is implemented yet",
                  target.target_name, target.abi_name, target.object_format,
                  target.pointer_bits);
    return false;
  }
  /*
   * Try the NYIR-first codegen path: build, optimize, verify, emit from IR.
   */
  nyr_func_t rt_main_nir = {0};
  size_t func_cap = NYR_BUNDLE_MAX_FUNCS;
  nyr_func_t *func_nirs = calloc(func_cap, sizeof(*func_nirs));
  const char **func_names = calloc(func_cap, sizeof(*func_names));
  if (!func_nirs || !func_names) {
    free(func_nirs);
    free(func_names);
    nyr_set_error(err, err_len, "native function pool allocation failed");
    return false;
  }
  size_t func_count = 0;
  char nir_err[512] = {0};
  bool nir_ok = nyr_build_ir(prog, opt, &rt_main_nir, func_nirs, &func_count,
                             func_names, func_cap, nir_err, sizeof(nir_err));
  if (nir_ok && target.abi == NY_NATIVE_ABI_WIN64) {
    for (size_t i = 0; nir_ok && i < func_count; ++i)
      nir_ok = ny_native_x64_win64_aggregate_supported(
          &func_nirs[i], func_names && func_names[i] ? func_names[i] : NULL,
          err, err_len);
    if (nir_ok)
      nir_ok = ny_native_x64_win64_aggregate_supported(
          &rt_main_nir, entry_name && entry_name[0] ? entry_name : "rt_main",
          err, err_len);
  }
  if (nir_ok && opt->native_only && opt->callgraph) {
    char callgraph_err[512] = {0};
    ny_dbg_enter_context("callgraph from compiled NYIR");
    const nyr_backend_module_t module = {&rt_main_nir, func_nirs, func_names,
                                         func_count, NULL};
    const nyr_callgraph_options_t callgraph_options = {opt->callgraph_json,
                                                       opt->callgraph_depth};
    bool callgraph_ok = nyr_write_callgraph_for_ir(
        &module, &callgraph_options, callgraph_err, sizeof(callgraph_err));
    ny_dbg_exit_context("callgraph from compiled NYIR");
    if (!callgraph_ok) {
      nyr_set_error(err, err_len, "native emitter callgraph failed: %s",
                    callgraph_err[0] ? callgraph_err : "unknown error");
      nir_ok = false;
    }
  }
  if (nir_ok && !ny_pipeline_dump_ir_for_program(prog, opt, err, err_len)) {
    free(func_nirs);
    free(func_names);
    return false;
  }

  /*
   * Keep PHI instructions intact so machine lowerer (ny_mach_lower_nir) lowers
   * them into register-level parallel copies without stack memory roundtrips.
   */

  nyr_writer_t w = {0};
  bool ok = false;

  if (nir_ok && rt_main_nir.len > 0) {
    bool is_wasm = target.target == NY_NATIVE_TARGET_WASM;
    /*
     * Emit header comment.
     */
    if (is_wasm) {
      if (!nyr_printf(&w, "(module\n  ;; Nytrix native %s backend output\n",
                      target.target_name))
        nir_ok = false;
    } else if (!nyr_printf(&w,
                           "# Nytrix native %s backend output (NYIR path)\n",
                           target.target_name))
      nir_ok = false;
    if (nir_ok && !is_wasm &&
        !nyr_printf(&w,
                    "# target=%s abi=%s object=%s ptr=%zub "
                    "red_zone=%s shadow_space=%zu\n",
                    target.target_name, target.abi_name, target.object_format,
                    target.pointer_bits, target.red_zone ? "yes" : "no",
                    target.shadow_space_bytes))
      nir_ok = false;
    bool wasm_panic_helpers = false;
    ny_wasm_import_t *wasm_imports = NULL;
    size_t wasm_import_count = 0;
    if (is_wasm) {
      for (size_t i = 0; i < func_count && !wasm_panic_helpers; ++i)
        wasm_panic_helpers = ny_wasm_func_uses_runtime_helpers(&func_nirs[i]);
      wasm_panic_helpers =
          wasm_panic_helpers || ny_wasm_func_uses_runtime_helpers(&rt_main_nir);
      if (!ny_wasm_collect_imports(func_nirs, func_count, &rt_main_nir,
                                   &wasm_imports, &wasm_import_count, err,
                                   err_len))
        nir_ok = false;
    }
    if (nir_ok && is_wasm &&
        !ny_wasm_emit_imports(&w, wasm_imports, wasm_import_count))
      nir_ok = false;
    free(wasm_imports);
    if (nir_ok && wasm_panic_helpers &&
        !ny_wasm_emit_runtime_helpers(&w, err, err_len))
      nir_ok = false;
    nyr_tier_plan_t tier = {0};
    if (nir_ok && nyr_tier_plan_init(&tier, &target, opt) && !is_wasm &&
        !nyr_printf(&w,
                    "# tier budget=%zu hot=%zu cold=%zu cache=%u vm=%s "
                    "ast_fallback=%s\n",
                    tier.compile_budget, tier.hot_threshold,
                    tier.cold_threshold, tier.cache_score,
                    tier.prefer_nir_vm ? "yes" : "no",
                    tier.prefer_ast_fallback ? "yes" : "no"))
      nir_ok = false;

    /*
     * Emit user functions (raw return, no tagging).
     */
    if (nir_ok) {
      for (size_t i = 0; i < func_count; ++i) {
        const char *fn_name = func_names[i] ? func_names[i] : "unknown_fn";
        char label[256];
        snprintf(label, sizeof(label), "ny_fn_%s", fn_name);
        bool emitted = nyr_emit_func(&w, &target, &func_nirs[i], label, false,
                                     err, err_len);
        if (!emitted) {
          nir_ok = false;
          break;
        }
      }
    }

    /*
     * Emit the top-level entry.
     */
    if (nir_ok) {
      const char *top_name =
          entry_name && entry_name[0] ? entry_name : "rt_main";
      bool emitted = nyr_emit_func(&w, &target, &rt_main_nir, top_name,
                                   tag_return, err, err_len);
      if (!emitted)
        nir_ok = false;
    }
    if (nir_ok && is_wasm) {
      if (!nyr_put(&w, ")\n"))
        nir_ok = false;
    } else if (nir_ok && (!nyr_strtab_append_asm(&w, err, err_len) ||
                          !nyr_arraytab_append_asm(&w, err, err_len))) {
      nir_ok = false;
    }
    if (nir_ok)
      ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;

    if (verbose_enabled >= 1 && nir_ok) {
      fprintf(stderr,
              "native asm: %zu functions + %s (%zu NYIR insts total)"
              " -> %s (%zu bytes)\n",
              func_count, entry_name && entry_name[0] ? entry_name : "rt_main",
              rt_main_nir.len, path ? path : "(stdout)", w.len);
    }

    /*
     * Clean up NYIR.
     */
    nyr_ir_func_free(&rt_main_nir);
    for (size_t i = 0; i < func_count; ++i)
      nyr_ir_func_free(&func_nirs[i]);
  }
  free(func_nirs);
  free(func_names);

  if (!nir_ok || !ok) {
    /*
     * NYIR path failed or produced no output; only x86-64 has AST fallback.
     */
    if (!nir_ok && nir_err[0] && err && err_len > 0 && err[0] == '\0')
      nyr_set_error(err, err_len, "%s", nir_err);
    free(w.data);
    w = (nyr_writer_t){0};
    if (!target_has_ast_fallback) {
      if (err && err_len > 0 && err[0] == '\0')
        nyr_set_error(err, err_len,
                      "native %s backend requires NYIR-supported input",
                      target.target_name);
      return false;
    }
    if (entry_name && entry_name[0] && strcmp(entry_name, "rt_main") != 0) {
      if (err && err_len > 0 && err[0] == '\0')
        nyr_set_error(
            err, err_len,
            "native NYIR path failed before executable entry emission");
      return false;
    }
    if (verbose_enabled >= 1)
      fprintf(stderr, "native asm: NYIR path failed, falling back to AST\n");
    char fallback_err[512] = {0};
    ok = nyr_x86_64_emit_rt_main(&w, &target, prog, fallback_err,
                                 sizeof(fallback_err));
    if (ok)
      ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;
    if (!ok && err && err_len > 0 && err[0] == '\0')
      nyr_set_error(err, err_len, "%s",
                    fallback_err[0] ? fallback_err
                                    : "native AST fallback failed");
    free(w.data);
    return ok;
  }

  if (!ok && err && err_len > 0 && err[0] == '\0')
    nyr_set_error(err, err_len, "failed to write native assembly to %s: %s",
                  path ? path : "(null)", strerror(errno));
  free(w.data);
  return ok;
}

bool ny_pipeline_emit_asm(const program_t *prog, const ny_options *opt,
                          const char *path, char *err, size_t err_len) {
  return ny_pipeline_emit_asm_entry(prog, opt, path, "rt_main", true, err,
                                    err_len);
}

/*
 * Assemble the same scalar machine form text stream used by --emit-asm into an
 * object. This is a narrow bridge while the internal object encoders are
 * migrated; false means "not this supported subset", so callers retain their
 * established NYIR object writer.
 */
static bool ny_native_try_emit_x64_machine_object(
    const nyr_func_t *top, const nyr_func_t *funcs, const char *const *names,
    size_t func_count, const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  if (!top || !target || target->target != NY_NATIVE_TARGET_X86_64)
    return false;
  nyr_writer_t text = {0};
  char machine_err[256] = {0};
  bool ok = true;
  for (size_t i = 0; ok && i < func_count; ++i) {
    char label[256];
    snprintf(label, sizeof(label), "ny_fn_%s",
             names[i] ? names[i] : "unknown_fn");
    ok = nyr_emit_func(&text, target, &funcs[i], label, false, machine_err,
                       sizeof(machine_err));
  }
  if (ok)
    ok = nyr_emit_func(&text, target, top, entry_symbol, tag_return,
                       machine_err, sizeof(machine_err));
  if (ok && (!nyr_strtab_append_asm(&text, machine_err, sizeof(machine_err)) ||
             !nyr_arraytab_append_asm(&text, machine_err, sizeof(machine_err))))
    ok = false;
  if (!ok) {
    nyr_set_error(err, err_len, "machine form: %s",
                  machine_err[0] ? machine_err : "unsupported machine shape");
    free(text.data);
    return false;
  }
  char asm_path[4096];
  char asm_name[96];
  snprintf(asm_name, sizeof(asm_name), "ny_machine_%ld_%llu.s", (long)getpid(),
           (unsigned long long)ny_ticks_now());
  ny_join_path(asm_path, sizeof(asm_path), ny_get_temp_dir(), asm_name);
  nyr_ensure_parent_dir_for_path(path);
  ok = ny_write_file(asm_path, text.data ? text.data : "", text.len) == 0;
  free(text.data);
  if (!ok)
    return false;
  const char *argv[] = {
      ny_builder_choose_cc(), "-c", asm_path, "-o", path, NULL};
  int rc = ny_exec_spawn(argv);
  if (!getenv("NY_KEEP_ASM"))
    unlink(asm_path);
  else
    fprintf(stderr, "[asm kept] %s\n", asm_path);
  return rc == 0;
}

bool ny_pipeline_emit_program_object(const program_t *prog,
                                     const ny_options *opt, const char *path,
                                     const char *entry_name, bool tag_return,
                                     char *err, size_t err_len) {
  nyr_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyr_set_pass_controls(opt ? opt->nyr_disable_pass : NULL,
                         opt ? opt->nyr_stop_after : NULL);
  nyr_set_verify_each_pass(opt && opt->nyr_ir_verify);
  nyr_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  if (!prog || !opt || !path || !*path) {
    nyr_set_error(err, err_len,
                  "native object emission: missing input or output path");
    return false;
  }
  const nyr_target_request_t target_request = nyr_target_request_make(
      opt->native_backend, opt->native_abi, opt->host_triple, opt->opt_size);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &target_request)) {
    nyr_set_error(err, err_len, "native object emission: backend disabled");
    return false;
  }
  if (!ny_native_target_has(&target, NY_NATIVE_CAP_ASM_OBJECT)) {
    nyr_set_error(err, err_len,
                  "native object emission for target '%s' is not enabled "
                  "yet; use --emit-asm for assembly output",
                  target.target_name ? target.target_name : "unknown");
    return false;
  }
  if (!ny_pipeline_dump_ir_for_program(prog, opt, err, err_len))
    return false;
  if (ny_native_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT) ||
      ny_native_target_has(&target, NY_NATIVE_CAP_COFF_OBJECT) ||
      ny_native_target_has(&target, NY_NATIVE_CAP_MACHO_OBJECT)) {
    nyr_func_t rt_main_nir = {0};
    size_t func_cap = NYR_BUNDLE_MAX_FUNCS;
    nyr_func_t *func_nirs = calloc(func_cap, sizeof(*func_nirs));
    const char **func_names = calloc(func_cap, sizeof(*func_names));
    size_t func_count = 0;
    char nir_err[512] = {0};
    if (!func_nirs || !func_names) {
      free(func_nirs);
      free(func_names);
      nyr_set_error(err, err_len,
                    "native object function pool allocation failed");
      return false;
    }
    bool nir_built =
        nyr_build_ir(prog, opt, &rt_main_nir, func_nirs, &func_count,
                     func_names, func_cap, nir_err, sizeof(nir_err));
    if (nir_built && target.abi == NY_NATIVE_ABI_WIN64) {
      for (size_t i = 0; nir_built && i < func_count; ++i)
        nir_built = ny_native_x64_win64_aggregate_supported(
            &func_nirs[i], func_names && func_names[i] ? func_names[i] : NULL,
            err, err_len);
      if (nir_built)
        nir_built = ny_native_x64_win64_aggregate_supported(
            &rt_main_nir, entry_name && entry_name[0] ? entry_name : "rt_main",
            err, err_len);
    }
    if (nir_built && rt_main_nir.len > 0) {
      const char *dump_obj_nyir = getenv("NY_DUMP_OBJ_NYIR");
      if (dump_obj_nyir && strcmp(dump_obj_nyir, "full") == 0)
        nyr_dump(stderr, &rt_main_nir, "obj-rt_main");
      else if (dump_obj_nyir)
        nyr_dump_compact(stderr, &rt_main_nir, "obj-rt_main");
      const char *obj_symbol =
          entry_name && entry_name[0] ? entry_name : "rt_main";
      char obj_err[512] = {0};
      bool obj_ok = false;
      if (ny_native_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT)) {
        if (target.target == NY_NATIVE_TARGET_X86) {
          obj_ok = nyr_native_emit_elf32_i386_object_from_nirs(
              &rt_main_nir, func_nirs, func_names, func_count, &target, path,
              obj_symbol, tag_return, obj_err, sizeof(obj_err));
        } else if (target.target == NY_NATIVE_TARGET_AARCH64) {
          obj_ok = nyr_native_emit_elf64_aarch64_object_from_nirs(
              &rt_main_nir, func_nirs, func_names, func_count, &target, path,
              obj_symbol, tag_return, obj_err, sizeof(obj_err));
        } else {
          obj_ok = nyr_native_emit_elf64_object_from_nirs(
              &rt_main_nir, func_nirs, func_names, func_count, &target, path,
              obj_symbol, tag_return, obj_err, sizeof(obj_err));
        }
      } else if (ny_native_target_has(&target, NY_NATIVE_CAP_COFF_OBJECT)) {
        obj_ok = nyr_native_emit_coff_x64_object_from_nirs(
            &rt_main_nir, func_nirs, func_names, func_count, &target, path,
            obj_symbol, tag_return, obj_err, sizeof(obj_err));
      } else if (ny_native_target_has(&target, NY_NATIVE_CAP_MACHO_OBJECT)) {
        obj_ok = nyr_native_emit_macho_x64_object_from_nirs(
            &rt_main_nir, func_nirs, func_names, func_count, &target, path,
            obj_symbol, tag_return, obj_err, sizeof(obj_err));
      }
      if (!obj_ok && getenv("NY_DUMP_MACH"))
        fprintf(stderr, "native object writer failed: %s\n",
                obj_err[0] ? obj_err : "<no detail>");
      if (obj_ok) {
        if (opt && opt->nyr_dump_stats) {
          unsigned long long mach_ok = 0, nir_fb = 0;
          unsigned long long ra_segments = 0, ra_colored = 0, ra_spilled = 0;
          unsigned long long ra_reloads = 0, ra_peak_live = 0;
          nyr_native_mach_encode_stats(&mach_ok, &nir_fb);
          nyr_native_mach_regalloc_stats(&ra_segments, &ra_colored, &ra_spilled,
                                         &ra_reloads, &ra_peak_live);
          fprintf(stderr, "mach encode mach_ok=%llu nir_fallback=%llu\n",
                  mach_ok, nir_fb);
          fprintf(stderr,
                  "mach regalloc segments=%llu colored=%llu spilled=%llu "
                  "reloads=%llu peak_live=%llu\n",
                  ra_segments, ra_colored, ra_spilled, ra_reloads,
                  ra_peak_live);
        }
        for (size_t i = 0; i < func_count; ++i)
          nyr_ir_func_free(&func_nirs[i]);
        nyr_ir_func_free(&rt_main_nir);
        if (err && err_len > 0)
          err[0] = '\0';
        return true;
      }
      /*
       * Keep the host-assembler bridge behind the internal writers.  Native
       * object emission is normally self-contained; this only retains the
       * existing machine form coverage while its direct byte encoder grows.
       */
      if (ny_native_try_emit_x64_machine_object(
              &rt_main_nir, func_nirs, func_names, func_count, &target, path,
              obj_symbol, tag_return, obj_err, sizeof(obj_err))) {
        for (size_t i = 0; i < func_count; ++i)
          nyr_ir_func_free(&func_nirs[i]);
        nyr_ir_func_free(&rt_main_nir);
        if (err && err_len > 0)
          err[0] = '\0';
        return true;
      }
      if (opt->native_only && obj_err[0]) {
        nyr_set_error(err, err_len, "%s", obj_err);
        for (size_t i = 0; i < func_count; ++i)
          nyr_ir_func_free(&func_nirs[i]);
        nyr_ir_func_free(&rt_main_nir);
        return false;
      }
      if (target.target == NY_NATIVE_TARGET_X86 &&
          ny_native_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT)) {
        nyr_set_error(err, err_len, "%s",
                      obj_err[0] ? obj_err : "i386 ELF object writer failed");
        for (size_t i = 0; i < func_count; ++i)
          nyr_ir_func_free(&func_nirs[i]);
        nyr_ir_func_free(&rt_main_nir);
        return false;
      }
    }
    for (size_t i = 0; i < func_count; ++i)
      nyr_ir_func_free(&func_nirs[i]);
    nyr_ir_func_free(&rt_main_nir);
    free(func_nirs);
    free(func_names);
    if (!nir_built && nir_err[0]) {
      /*
       * A clean NYIR lowering error (R3: body-less stdlib/module calls) is
       * the real diagnostic; surface it instead of masking it with the asm
       * fallback's generic "before executable entry emission" message.
       */
      nyr_set_error(err, err_len, "%s", nir_err);
      return false;
    }
    if (err && err_len > 0)
      err[0] = '\0';
  }
  char asm_path[4096];
  char asm_name[96];
  snprintf(asm_name, sizeof(asm_name), "ny_native_%ld_%llu.s", (long)getpid(),
           (unsigned long long)ny_ticks_now());
  ny_join_path(asm_path, sizeof(asm_path), ny_get_temp_dir(), asm_name);
  if (!ny_pipeline_emit_asm_entry(prog, opt, asm_path, entry_name, tag_return,
                                  err, err_len))
    return false;
  nyr_ensure_parent_dir_for_path(path);
  const char *cc = ny_builder_choose_cc();
  const char *argv[] = {cc, "-c", asm_path, "-o", path, NULL};
  int rc = ny_exec_spawn(argv);
  if (!getenv("NY_KEEP_ASM"))
    unlink(asm_path);
  else
    fprintf(stderr, "[asm kept] %s\n", asm_path);
  if (rc != 0) {
    nyr_set_error(err, err_len,
                  "native object emission: assembler failed with rc=%d", rc);
    return false;
  }
  struct stat st;
  if (stat(path, &st) != 0 || st.st_size <= 0) {
    nyr_set_error(err, err_len,
                  "native object emission: assembler produced no object");
    return false;
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
