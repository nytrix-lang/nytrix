#include "code/lang/pipeline/native.h"
#include "code/lang/pipeline/legacy_backend.h"
#include "code/lang/pipeline/native_vm.h"

#include "code/backend/internal.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/module.h"
#include "code/backend/llvm/bridge.h"
#include "code/backend/native.h"
#include "code/backend/wire/cache.h"
#include "code/lang/code.h"
#include "code/lang/llvm/jit.h"
#include "code/lang/lower.h"
#include "code/lang/priv.h"
#include "code/lang/runtime/runtime.h"
#include "code/lang/runtime/shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

static void *ny_pipeline_resolve_runtime_symbol(void *userdata,
                                                const char *name) {
  (void)userdata;
  if (!name || !*name)
    return NULL;
  if (strncmp(name, "ny_fn_", 6) == 0) {
    const char *runtime_name = nyr_runtime_symbol(name + 6);
    if (runtime_name)
      return ny_pipeline_resolve_runtime_symbol(userdata, runtime_name);
  }
#define RT_DEF(public_name, implementation, args, sig, doc)                    \
  if (strcmp(name, public_name) == 0 || strcmp(name, #implementation) == 0)    \
    return (void *)(uintptr_t)(implementation);
#define RT_DEF_F64(public_name, implementation, args, sig, doc)                \
  if (strcmp(name, public_name) == 0 || strcmp(name, #implementation) == 0)    \
    return (void *)(uintptr_t)(implementation);
#define RT_GV(public_name, implementation, type, doc)                          \
  if (strcmp(name, public_name) == 0 || strcmp(name, #implementation) == 0)    \
    return (void *)&implementation;
#include "code/lang/runtime/defs.h"
#undef RT_GV
#undef RT_DEF_F64
#undef RT_DEF
#define NY_RUNTIME_HELPER(public_name, implementation)                         \
  if (strcmp(name, #implementation) == 0 || strcmp(name, public_name) == 0)    \
    return (void *)(uintptr_t)(implementation);
  NY_RUNTIME_HELPER("rt_malloc_i64", rt_malloc_i64)
  NY_RUNTIME_HELPER("rt_malloc_raw_i64", rt_malloc_raw_i64)
  NY_RUNTIME_HELPER("rt_ptr_add_i64", rt_ptr_add_i64)
  NY_RUNTIME_HELPER("rt_ptr_sub_i64", rt_ptr_sub_i64)
  NY_RUNTIME_HELPER("rt_simmd_bswap32_i64", rt_simmd_bswap32_i64)
  NY_RUNTIME_HELPER("rt_simmd_bswap64_i64", rt_simmd_bswap64_i64)
  NY_RUNTIME_HELPER("rt_simmd_clz32_i64", rt_simmd_clz32_i64)
  NY_RUNTIME_HELPER("rt_simmd_clz64_i64", rt_simmd_clz64_i64)
  NY_RUNTIME_HELPER("rt_simmd_ctz32_i64", rt_simmd_ctz32_i64)
  NY_RUNTIME_HELPER("rt_simmd_ctz64_i64", rt_simmd_ctz64_i64)
  NY_RUNTIME_HELPER("rt_simmd_popcnt32_i64", rt_simmd_popcnt32_i64)
  NY_RUNTIME_HELPER("rt_simmd_popcnt64_i64", rt_simmd_popcnt64_i64)
  NY_RUNTIME_HELPER("rt_simmd_rotl32_i64", rt_simmd_rotl32_i64)
  NY_RUNTIME_HELPER("rt_simmd_rotl64_i64", rt_simmd_rotl64_i64)
  NY_RUNTIME_HELPER("rt_simmd_rotr32_i64", rt_simmd_rotr32_i64)
  NY_RUNTIME_HELPER("rt_simmd_rotr64_i64", rt_simmd_rotr64_i64)
#undef NY_RUNTIME_HELPER
#ifndef _WIN32
  void *address = dlsym(RTLD_DEFAULT, name);
  if (address)
    return address;
#endif
  return ny_jit_resolve_symbol(name);
}

static void ny_pipeline_register_jit_mapping(void *userdata, uintptr_t address,
                                             size_t size) {
  (void)userdata;
  if (address && size)
    rt_map_oracle_add(address, size);
}

static bool ny_pipeline_evaluate_ir(void *userdata, const void *options,
                                    nyr_func_t *entry, nyr_func_t *functions,
                                    const char *const *function_names,
                                    size_t function_count,
                                    nyr_eval_result_t *result, char *err,
                                    size_t err_len) {
  (void)userdata;
  return ny_lang_eval_ir_value(entry, functions, function_names, function_count,
                               (const ny_options *)options, result, err,
                               err_len);
}

static bool ny_pipeline_collect_vm_profile(void *userdata, const void *options,
                                           nyr_func_t *entry,
                                           nyr_func_t *functions,
                                           const char *const *function_names,
                                           size_t function_count,
                                           nyr_eval_result_t *profile,
                                           char *err, size_t err_len) {
  (void)userdata;
  return ny_lang_collect_vm_profile(entry, functions, function_names,
                                    function_count, (const ny_options *)options,
                                    profile, err, err_len);
}

static const nyr_runtime_services_t ny_pipeline_runtime_services = {
    .resolve_symbol = ny_pipeline_resolve_runtime_symbol,
    .register_mapping = ny_pipeline_register_jit_mapping,
    .evaluate_ir = ny_pipeline_evaluate_ir,
    .collect_vm_profile = ny_pipeline_collect_vm_profile,
};

const nyr_runtime_services_t *ny_pipeline_runtime_services_get(void) {
  return &ny_pipeline_runtime_services;
}

static void ny_pipeline_load_link(const char *library, void *ctx) {
  (void)ctx;
  (void)ny_jit_load_library(library);
}

bool ny_pipeline_jit_compile(const program_t *program,
                             const ny_options *options, nyr_jit_image_t *image,
                             char *err, size_t err_len) {
  if (!program || !options || !image) {
    nyr_set_error(err, err_len, "native JIT: missing input");
    return false;
  }
  nyr_func_t entry = {0};
  nyr_func_t *functions = calloc(NYR_BUNDLE_MAX_FUNCS, sizeof(*functions));
  const char **names = calloc(NYR_BUNDLE_MAX_FUNCS, sizeof(*names));
  size_t count = 0;
  if (!functions || !names) {
    free(functions);
    free(names);
    nyr_set_error(err, err_len, "native JIT function pool allocation failed");
    return false;
  }
  if (!nyr_build_ir(program, options, &entry, functions, &count, names,
                    NYR_BUNDLE_MAX_FUNCS, err, err_len)) {
    free(functions);
    free(names);
    return false;
  }

  for (size_t i = 0; i < options->link_libs.len; ++i)
    (void)ny_jit_load_library(options->link_libs.data[i]);
  ny_pipeline_visit_program_links_for_options(program, options,
                                              ny_pipeline_load_link, NULL);

  char *cache_path = NULL;
#ifndef _WIN32
  if (program->raw_src && program->raw_src_len > 0)
    cache_path = ny_pipeline_native_cache_path(program->raw_src, options);
#endif
  nyr_backend_module_t module = {&entry, functions, names, count,
                                 &ny_pipeline_runtime_services};
  bool ok = nyr_jit_compile(&module, options, cache_path, image, err, err_len);
  free(cache_path);
  nyr_ir_func_free(&entry);
  for (size_t i = 0; i < count; ++i)
    nyr_ir_func_free(&functions[i]);
  free(functions);
  free(names);
  return ok;
}

bool ny_pipeline_write_tier_report(const program_t *program,
                                   const ny_options *options, char *err,
                                   size_t err_len) {
  if (!program || !options || !options->native_tier_report)
    return true;
  size_t capacity = NYR_BUNDLE_MAX_FUNCS;
  nyr_func_t entry = {0};
  nyr_func_t *functions = calloc(capacity, sizeof(*functions));
  const char **names = calloc(capacity, sizeof(*names));
  if (!functions || !names) {
    free(functions);
    free(names);
    nyr_set_error(err, err_len,
                  "native tier report: function pool allocation failed");
    return false;
  }
  size_t count = 0;
  char local_err[512] = {0};
  if (!nyr_build_ir(program, options, &entry, functions, &count, names,
                    capacity, local_err, sizeof(local_err))) {
    nyr_set_error(err, err_len, "native tier report: %s",
                  local_err[0] ? local_err : "failed to build NYIR");
    free(functions);
    free(names);
    return false;
  }
  nyr_backend_module_t module = {&entry, functions, names, count,
                                 &ny_pipeline_runtime_services};
  bool ok = nyr_write_tier_report_for_module(&module, options, err, err_len);
  nyr_ir_func_free(&entry);
  for (size_t i = 0; i < count; ++i)
    nyr_ir_func_free(&functions[i]);
  free(functions);
  free(names);
  return ok;
}

bool ny_pipeline_result_oracle(const program_t *program,
                               const ny_options *options, char *err,
                               size_t err_len) {
  if (!program || !options || !options->native_result_oracle)
    return true;
  size_t capacity = NYR_BUNDLE_MAX_FUNCS;
  nyr_func_t entry = {0};
  nyr_func_t *functions = calloc(capacity, sizeof(*functions));
  const char **names = calloc(capacity, sizeof(*names));
  if (!functions || !names) {
    free(functions);
    free(names);
    nyr_set_error(err, err_len,
                  "native oracle: function pool allocation failed");
    return false;
  }
  size_t count = 0;
  char local_err[512] = {0};
  if (!nyr_build_ir(program, options, &entry, functions, &count, names,
                    capacity, local_err, sizeof(local_err))) {
    nyr_set_error(err, err_len, "native oracle: %s",
                  local_err[0] ? local_err : "failed to build NYIR");
    free(functions);
    free(names);
    return false;
  }
  bool ok = nyr_native_result_oracle_for_nir(
      &entry, functions, names, count, options, &ny_pipeline_runtime_services,
      err, err_len);
  nyr_ir_func_free(&entry);
  for (size_t i = 0; i < count; ++i)
    nyr_ir_func_free(&functions[i]);
  free(functions);
  free(names);
  return ok;
}

typedef struct ny_pipeline_origin_slot_t {
  const char *name;
  const char *section;
} ny_pipeline_origin_slot_t;

typedef struct ny_pipeline_llvm_userdata_t {
  codegen_t *cg;
  ny_pipeline_origin_slot_t *origins;
  size_t origins_cap;
} ny_pipeline_llvm_userdata_t;

static uint64_t ny_pipeline_origin_hash(const char *name) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
    hash ^= *p;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static void ny_pipeline_origin_insert(ny_pipeline_llvm_userdata_t *userdata,
                                      const char *name, const char *section) {
  if (!userdata || !userdata->origins || !userdata->origins_cap || !name ||
      !*name || !section)
    return;
  size_t mask = userdata->origins_cap - 1;
  size_t at = (size_t)ny_pipeline_origin_hash(name) & mask;
  while (userdata->origins[at].name) {
    if (strcmp(userdata->origins[at].name, name) == 0)
      return; /* Preserve the original lookup's first-match precedence. */
    at = (at + 1) & mask;
  }
  userdata->origins[at] =
      (ny_pipeline_origin_slot_t){.name = name, .section = section};
}

static void ny_pipeline_origin_index_init(ny_pipeline_llvm_userdata_t *userdata,
                                          codegen_t *cg) {
  if (!userdata || !cg)
    return;
  userdata->cg = cg;
  size_t entries = cg->fun_sigs.len;
  if (cg->global_vars.len > (SIZE_MAX - entries) / 2)
    return;
  entries += cg->global_vars.len * 2;
  if (entries > SIZE_MAX / 2)
    return;
  size_t cap = 8;
  size_t wanted = entries * 2;
  while (cap < wanted) {
    if (cap > SIZE_MAX / 2)
      return;
    cap *= 2;
  }
  userdata->origins = calloc(cap, sizeof(*userdata->origins));
  if (!userdata->origins)
    return;
  userdata->origins_cap = cap;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    const fun_sig *sig = &cg->fun_sigs.data[i];
    if (sig->name && sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC)
      ny_pipeline_origin_insert(userdata, sig->name,
                                ny_is_stdlib_tok(sig->stmt_t->tok) ? "ny.std"
                                                                   : "ny.user");
  }
  for (size_t i = 0; i < cg->global_vars.len; ++i) {
    const binding *global = &cg->global_vars.data[i];
    if (!global->stmt_t || !global->stmt_t->tok.module_features)
      continue;
    const char *section =
        ny_is_stdlib_tok(global->stmt_t->tok) ? "ny.std" : "ny.user";
    ny_pipeline_origin_insert(userdata, global->name, section);
    if (global->value)
      ny_pipeline_origin_insert(userdata, LLVMGetValueName(global->value),
                                section);
  }
}

static void
ny_pipeline_origin_index_dispose(ny_pipeline_llvm_userdata_t *userdata) {
  if (!userdata)
    return;
  free(userdata->origins);
  userdata->origins = NULL;
  userdata->origins_cap = 0;
}

static const char *ny_pipeline_llvm_origin_section(void *userdata_ptr,
                                                   const char *name) {
  ny_pipeline_llvm_userdata_t *userdata = userdata_ptr;
  const codegen_t *cg = userdata ? userdata->cg : NULL;
  if (!name)
    return NULL;
  if (userdata && userdata->origins && userdata->origins_cap) {
    size_t mask = userdata->origins_cap - 1;
    size_t at = (size_t)ny_pipeline_origin_hash(name) & mask;
    while (userdata->origins[at].name) {
      if (strcmp(userdata->origins[at].name, name) == 0)
        return userdata->origins[at].section;
      at = (at + 1) & mask;
    }
    return NULL;
  }
  for (size_t i = 0; cg && i < cg->fun_sigs.len; ++i) {
    const fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig->name || strcmp(sig->name, name) != 0 || !sig->stmt_t ||
        sig->stmt_t->kind != NY_S_FUNC)
      continue;
    return ny_is_stdlib_tok(sig->stmt_t->tok) ? "ny.std" : "ny.user";
  }
  for (size_t i = 0; cg && i < cg->global_vars.len; ++i) {
    const binding *global = &cg->global_vars.data[i];
    if (!global->stmt_t || !global->stmt_t->tok.module_features)
      continue;
    bool matches = global->name && strcmp(global->name, name) == 0;
    if (!matches && global->value) {
      const char *value_name = LLVMGetValueName(global->value);
      matches = value_name && strcmp(value_name, name) == 0;
    }
    if (matches)
      return ny_is_stdlib_tok(global->stmt_t->tok) ? "ny.std" : "ny.user";
  }
  return NULL;
}

static LLVMMetadataRef ny_pipeline_llvm_debug_scope(void *userdata,
                                                    const char *filename,
                                                    unsigned line,
                                                    unsigned column) {
  ny_pipeline_llvm_userdata_t *pipeline = userdata;
  codegen_t *cg = pipeline ? pipeline->cg : NULL;
  token_t tok = {0};
  tok.filename = filename;
  tok.line = (int)line;
  tok.col = (int)column;
  return codegen_debug_loc_scope(cg, tok);
}

static LLVMMetadataRef
ny_pipeline_llvm_debug_subprogram(void *userdata, LLVMValueRef function,
                                  const char *name, const char *filename,
                                  unsigned line, unsigned column) {
  ny_pipeline_llvm_userdata_t *pipeline = userdata;
  codegen_t *cg = pipeline ? pipeline->cg : NULL;
  token_t tok = {0};
  tok.filename = filename;
  tok.line = (int)line;
  tok.col = (int)column;
  return codegen_debug_subprogram(cg, function, name, tok);
}

static LLVMValueRef ny_pipeline_llvm_global_symbol(void *userdata,
                                                   const char *name) {
  ny_pipeline_llvm_userdata_t *pipeline = userdata;
  return codegen_emit_string_or_array_global(pipeline ? pipeline->cg : NULL,
                                             name);
}

bool ny_pipeline_emit_llvm_program(codegen_t *cg, const program_t *program,
                                   const ny_options *options, char *err,
                                   size_t err_len) {
  if (!cg || !program)
    return false;
  size_t function_cap = options && options->emit_module && *options->emit_module
                            ? NY_NATIVE_PRECOMPILE_MAX_FUNCS
                            : NY_NATIVE_LIVE_MAX_FUNCS;
  nyr_func_t *functions = calloc(function_cap, sizeof(*functions));
  const char **names = calloc(function_cap, sizeof(*names));
  if (!functions || !names) {
    free(functions);
    free(names);
    if (err && err_len)
      snprintf(err, err_len, "NYIR function pool allocation failed");
    return false;
  }

  nyr_func_t entry = {0};
  size_t function_count = 0;
  char build_error[512] = {0};
  /*
   * LLVM consumes SSA form directly; the direct encoders' PHI lowering is not
   * valid for LLVM's block-edge representation.
   */
  bool previous_preserve_phis = nyr_get_preserve_phis();
  nyr_set_preserve_phis(true);
  bool trace_emit_timing = ny_env_enabled("NYTRIX_TRACE_NYIR_EMIT_TIMING");
  ny_tick_t t_ir_build = trace_emit_timing ? ny_ticks_now() : 0;
  const program_t *ir_program = program;
  program_t module_program = {0};
  stmt_t **module_body = NULL;
  bool built = true;
  if (options && options->emit_module && *options->emit_module &&
      cg->extra_progs.len) {
    size_t body_len = program->body.len;
    for (size_t i = 0; i < cg->extra_progs.len; ++i) {
      const program_t *extra = cg->extra_progs.data[i];
      if (!extra || extra == program || extra->body.len > SIZE_MAX - body_len)
        continue;
      body_len += extra->body.len;
    }
    if (body_len > SIZE_MAX / sizeof(*module_body)) {
      snprintf(build_error, sizeof(build_error),
               "NYIR module AST index size overflow");
      built = false;
    } else {
      module_body = malloc(body_len * sizeof(*module_body));
      if (!module_body) {
        snprintf(build_error, sizeof(build_error),
                 "NYIR module AST index allocation failed");
        built = false;
      } else {
        size_t at = 0;
        for (size_t pi = 0; pi <= cg->extra_progs.len; ++pi) {
          const program_t *source =
              pi == 0 ? program : cg->extra_progs.data[pi - 1];
          if (!source || (pi > 0 && source == program))
            continue;
          for (size_t si = 0; si < source->body.len; ++si) {
            stmt_t *stmt = source->body.data[si];
            bool duplicate_module = false;
            if (stmt && stmt->kind == NY_S_MODULE && stmt->as.module.name) {
              for (size_t prior = 0; prior < at; ++prior) {
                const stmt_t *other = module_body[prior];
                if (other && other->kind == NY_S_MODULE &&
                    other->as.module.name &&
                    strcmp(other->as.module.name, stmt->as.module.name) == 0) {
                  duplicate_module = true;
                  break;
                }
              }
            }
            if (!duplicate_module)
              module_body[at++] = stmt;
          }
        }
        module_program = *program;
        module_program.body.data = module_body;
        module_program.body.len = at;
        module_program.body.cap = at;
        ir_program = &module_program;
      }
    }
  }
  if (built)
    built =
        nyr_build_ir(ir_program, options, &entry, functions, &function_count,
                     names, function_cap, build_error, sizeof(build_error));
  free(module_body);
  ny_tick_t t_ir_emit = trace_emit_timing ? ny_ticks_now() : 0;
  nyr_set_preserve_phis(previous_preserve_phis);
  bool ok = false;
  if (!built) {
    if (err && err_len)
      snprintf(err, err_len, "NYIR build failed: %s", build_error);
  } else {
    nyr_backend_module_t module = {&entry, functions, names, function_count,
                                   NULL};
    ny_pipeline_llvm_userdata_t pipeline_userdata = {0};
    ny_pipeline_origin_index_init(&pipeline_userdata, cg);
    ny_llvm_context_t context = {
        .context = cg->ctx,
        .module = cg->module,
        .builder = cg->builder,
        .type_i8 = cg->type_i8,
        .type_i64 = cg->type_i64,
        .type_f32 = cg->type_f32,
        .type_f64 = cg->type_f64,
        .di_builder = cg->di_builder,
        .di_loc = cg->di_loc,
        .di_scope = cg->di_scope,
        .debug_symbols = cg->debug_symbols,
        /*
         * std.bc is a shared implementation cache, so its module selection
         * must emit callable NYIR bodies. The legacy LLVM frontend still
         * emits interface declarations for the selected module; the NYIR
         * emitter supplies the bodies that consumers link from the cache.
         */
        .module_declarations_only =
            cg->emit_module_decls_only &&
            !ny_env_enabled("NYTRIX_STD_BC_EMIT_BODIES"),
        .emit_entry =
            !(options && options->emit_module && *options->emit_module),
        .default_filename = cg->source_main_file,
        .origin_section = ny_pipeline_llvm_origin_section,
        .debug_scope = ny_pipeline_llvm_debug_scope,
        .debug_subprogram = ny_pipeline_llvm_debug_subprogram,
        .global_symbol = ny_pipeline_llvm_global_symbol,
        .userdata = &pipeline_userdata,
    };
    ok = ny_llvm_emit_nyir_module(&context, &module, err, err_len);
    ny_pipeline_origin_index_dispose(&pipeline_userdata);
  }
  if (trace_emit_timing) {
    ny_tick_t t_done = ny_ticks_now();
    fprintf(stderr,
            "[nyir-emit-timing] functions=%zu NYIR build=%.3f LLVM emit="
            "%.3f total=%.3f s\n",
            function_count, ny_ticks_delta_sec(t_ir_build, t_ir_emit),
            ny_ticks_delta_sec(t_ir_emit, t_done),
            ny_ticks_delta_sec(t_ir_build, t_done));
  }

  nyr_ir_func_free(&entry);
  for (size_t i = 0; i < function_count; ++i)
    nyr_ir_func_free(&functions[i]);
  free(functions);
  free(names);
  return ok;
}

/*
 * The JIT route dlopens every #link library with RTLD_GLOBAL before running
 * generated code, so `dlsym(RTLD_DEFAULT, ...)` finds its exports. The NYIR
 * VM resolves unknown externals through the same process-wide lookup, so it
 * must load the program's link libraries too — otherwise a C harness linked
 * by an interop fixture resolves to nothing and every call into it fails.
 */
static void ny_native_vm_load_link_stmt(stmt_t *s) {
  if (!s)
    return;
  if (s->kind == NY_S_LINK) {
    if (s->as.link.lib)
      ny_jit_load_library(s->as.link.lib);
    return;
  }
  if (s->kind == NY_S_INCLUDE) {
    /*
     * `#include "h" link "lib"` carries the shared object on the include.
     */
    if (s->as.inc.lib)
      ny_jit_load_library(s->as.inc.lib);
    return;
  }
  switch (s->kind) {
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_vm_load_link_stmt(s->as.module.body.data[i]);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_vm_load_link_stmt(s->as.block.body.data[i]);
    break;
  default:
    break;
  }
}

bool ny_pipeline_eval_ir_for_program(const program_t *prog,
                                     const ny_options *opt, char *err,
                                     size_t err_len) {
  if (opt && opt->nyr_run_bin_path && opt->nyr_run_bin_path[0])
    return ny_pipeline_eval_ir_binary_file(opt->nyr_run_bin_path, opt, err,
                                           err_len);
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_native_vm_load_link_stmt(prog->body.data[i]);
  nyr_func_t rt_main = {0};
  size_t func_cap = NYR_BUNDLE_MAX_FUNCS;
  nyr_func_t *funcs = calloc(func_cap, sizeof(*funcs));
  const char **names = calloc(func_cap, sizeof(*names));
  size_t count = 0;
  if (!funcs || !names) {
    free(funcs);
    free(names);
    nyr_set_error(err, err_len,
                  "native evaluator function pool allocation failed");
    return false;
  }
  /*
   * The interpreter executes SSA PHIs directly. Keep them intact instead of
   * routing VM programs through phi-elimination's local-memory lowering,
   * which is intended only for backends without PHI support.
   */
  bool previous_preserve_phis = nyr_get_preserve_phis();
  nyr_set_preserve_phis(true);
  bool built = nyr_build_ir(prog, opt, &rt_main, funcs, &count, names, func_cap,
                            err, err_len);
  nyr_set_preserve_phis(previous_preserve_phis);
  if (!built) {
    free(funcs);
    free(names);
    return false;
  }
  nyr_eval_globals_clear();
  bool ok = ny_lang_run_ir(&rt_main, funcs, names, count, opt, "rt_main", err,
                           err_len);
  nyr_ir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    nyr_ir_func_free(&funcs[i]);
  free(funcs);
  free(names);
  return ok;
}

bool ny_pipeline_eval_ir_binary_file(const char *path,
                                     const ny_options *options, char *err,
                                     size_t err_len) {
  return ny_lang_eval_ir_binary_file(path, options, err, err_len);
}

static bool ny_native_stmt_in_dump_scope(const ny_options *opt,
                                         const stmt_t *s) {
  if (!s)
    return false;
  ny_dump_scope_t scope = opt ? opt->dump_scope : NY_DUMP_SCOPE_PROGRAM;
  if (scope == NY_DUMP_SCOPE_BOTH)
    return true;
  bool is_std = ny_is_stdlib_tok(s->tok);
  return scope == NY_DUMP_SCOPE_LIB ? is_std : !is_std;
}

static bool ny_native_program_dump_view(const program_t *prog,
                                        const ny_options *opt, program_t *view,
                                        stmt_t ***storage) {
  if (!prog || !view || !storage)
    return false;
  *view = *prog;
  *storage = NULL;
  if (!prog->body.len)
    return true;
  stmt_t **data = calloc(prog->body.len, sizeof(*data));
  if (!data)
    return false;
  size_t len = 0;
  for (size_t i = 0; i < prog->body.len; ++i)
    if (ny_native_stmt_in_dump_scope(opt, prog->body.data[i]))
      data[len++] = prog->body.data[i];
  view->body.data = data;
  view->body.len = view->body.cap = len;
  *storage = data;
  return true;
}

bool ny_pipeline_dump_ir_for_program(const program_t *prog,
                                     const ny_options *opt, char *err,
                                     size_t err_len) {
  if (!opt || !opt->native_dump_ir)
    return true;
  bool defer_metadata_bin_report =
      opt->nyr_metadata_report && opt->nyr_metadata_bin_path &&
      opt->nyr_metadata_bin_path[0] && opt->nyr_dump_bin &&
      opt->nyr_dump_bin_path && opt->nyr_dump_bin_path[0] &&
      strcmp(opt->nyr_metadata_bin_path, opt->nyr_dump_bin_path) == 0;
  if (!defer_metadata_bin_report &&
      !nyr_write_ir_metadata_report(prog, opt, err, err_len))
    return false;
  bool run_binary_after_dump = opt->nyr_run && opt->nyr_run_bin_path &&
                               opt->nyr_run_bin_path[0] && opt->nyr_dump_bin;
  /*
   * Emit-only is used by callers that request an artifact/preflight while
   * selecting NYIR as the execution backend. It must build and verify the
   * program without running user code; otherwise an expected runtime panic
   * (for example, a forged container handle) is misreported as a dump error.
   */
  if (opt->nyr_run && !opt->emit_only && !run_binary_after_dump) {
    if (!ny_pipeline_eval_ir_for_program(prog, opt, err, err_len))
      return false;
    if (!opt->nyr_dump_text && !opt->nyr_dump_bin)
      return true;
  }
  if (!opt->nyr_dump_text && !opt->nyr_dump_bin)
    return true;
  if (opt->nyr_dump_bin) {
    FILE *bout = stderr;
    if (opt->nyr_dump_bin_path && opt->nyr_dump_bin_path[0]) {
      bout = fopen(opt->nyr_dump_bin_path, "wb");
      if (!bout) {
        nyr_set_error(err, err_len,
                      "native NYIR binary dump: failed to open %s: %s",
                      opt->nyr_dump_bin_path, strerror(errno));
        return false;
      }
    }
    char berr[512] = {0};
    bool bok = nyr_dump_program_binary(bout, prog, opt, berr, sizeof(berr));
    if (bout != stderr)
      fclose(bout);
    if (!bok) {
      /*
       * A named dump is an interchange artifact, not a best-effort log. Do
       * not leave an empty or partial file that a later --nyir-run-bin call
       * could treat as the requested program.
       */
      if (opt->nyr_dump_bin_path && opt->nyr_dump_bin_path[0])
        remove(opt->nyr_dump_bin_path);
      nyr_set_error(err, err_len, "%s",
                    berr[0] ? berr : "native NYIR binary dump failed");
      return false;
    }
    if (run_binary_after_dump) {
      if (!opt->nyr_dump_bin_path || !opt->nyr_dump_bin_path[0]) {
        nyr_set_error(err, err_len,
                      "native NYIR VM: --nyir-run-bin with same-process "
                      "dump requires --nyir-dump-bin=PATH");
        return false;
      }
      if (!ny_pipeline_eval_ir_binary_file(opt->nyr_run_bin_path, opt, err,
                                           err_len))
        return false;
    }
    if (defer_metadata_bin_report) {
      /*
       * The just-written NYIP bundle contains multiple NYIR members. Its
       * metadata is the same freshly-built program view, while the legacy
       * --nyir-metadata-bin reader accepts one NYIR member. Avoid reopening
       * a multi-function bundle through that single-function compatibility
       * reader.
       */
      ny_options metadata_opt = *opt;
      metadata_opt.nyr_metadata_bin_path = NULL;
      if (!nyr_write_ir_metadata_report(prog, &metadata_opt, err, err_len))
        return false;
    }
    if (!opt->nyr_dump_text)
      return true;
  }
  FILE *out = stderr;
  if (opt->native_dump_ir_path && opt->native_dump_ir_path[0]) {
    out = fopen(opt->native_dump_ir_path, "wb");
    if (!out) {
      nyr_set_error(err, err_len, "native NYIR dump: failed to open %s: %s",
                    opt->native_dump_ir_path, strerror(errno));
      return false;
    }
  }

  bool attempted_any = false;
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_FUNC || !ny_native_stmt_in_dump_scope(opt, s))
      continue;
    attempted_any = true;
    char local_err[512] = {0};
    if (!nyr_dump_function(out, prog, s, local_err, sizeof(local_err), opt)) {
      fprintf(out, "native NYIR dump unavailable for function %s: %s\n",
              s->as.fn.name ? s->as.fn.name : "<anon>",
              local_err[0] ? local_err : "unsupported shape");
    }
  }

  program_t dump_view = {0};
  stmt_t **dump_storage = NULL;
  if (!ny_native_program_dump_view(prog, opt, &dump_view, &dump_storage)) {
    if (out != stderr)
      fclose(out);
    nyr_set_error(err, err_len, "native NYIR dump: out of memory");
    return false;
  }
  if (dump_view.body.len) {
    attempted_any = true;
    char local_err[512] = {0};
    if (!nyr_dump_rt_main(out, &dump_view, local_err, sizeof(local_err), opt)) {
      fprintf(out, "%s\n",
              local_err[0]
                  ? local_err
                  : "native NYIR dump unavailable: unsupported program shape");
    }
  }
  free(dump_storage);

  if (!attempted_any)
    fputs("native NYIR dump unavailable: program has no dumpable body\n", out);
  if (out != stderr)
    fclose(out);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

static void ny_pipeline_visit_stmt_links(const stmt_t *stmt,
                                         const ny_options *opt,
                                         nyr_link_visitor_t visitor,
                                         void *ctx) {
  if (!stmt)
    return;
  switch (stmt->kind) {
  case NY_S_INCLUDE:
    if (stmt->as.inc.lib && stmt->as.inc.lib[0])
      visitor(stmt->as.inc.lib, ctx);
    return;
  case NY_S_LINK:
    if (stmt->as.link.lib && stmt->as.link.lib[0])
      visitor(stmt->as.link.lib, ctx);
    return;
  case NY_S_MODULE:
    for (size_t i = 0; i < stmt->as.module.body.len; ++i)
      ny_pipeline_visit_stmt_links(stmt->as.module.body.data[i], opt, visitor,
                                   ctx);
    return;
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      ny_pipeline_visit_stmt_links(stmt->as.block.body.data[i], opt, visitor,
                                   ctx);
    return;
  case NY_S_IF: {
    bool selected = false;
    ny_pipeline_visit_stmt_links(stmt->as.iff.init, opt, visitor, ctx);
    if (stmt->as.iff.test && stmt->as.iff.test->kind == NY_E_COMPTIME &&
        nyr_target_eval_bool(opt, stmt->as.iff.test, &selected)) {
      ny_pipeline_visit_stmt_links(
          selected ? stmt->as.iff.conseq : stmt->as.iff.alt, opt, visitor, ctx);
      return;
    }
    ny_pipeline_visit_stmt_links(stmt->as.iff.conseq, opt, visitor, ctx);
    ny_pipeline_visit_stmt_links(stmt->as.iff.alt, opt, visitor, ctx);
    return;
  }
  case NY_S_WHILE:
    ny_pipeline_visit_stmt_links(stmt->as.whl.init, opt, visitor, ctx);
    ny_pipeline_visit_stmt_links(stmt->as.whl.body, opt, visitor, ctx);
    ny_pipeline_visit_stmt_links(stmt->as.whl.update, opt, visitor, ctx);
    return;
  case NY_S_FOR:
    ny_pipeline_visit_stmt_links(stmt->as.fr.init, opt, visitor, ctx);
    ny_pipeline_visit_stmt_links(stmt->as.fr.body, opt, visitor, ctx);
    ny_pipeline_visit_stmt_links(stmt->as.fr.update, opt, visitor, ctx);
    return;
  case NY_S_TRY:
    ny_pipeline_visit_stmt_links(stmt->as.tr.body, opt, visitor, ctx);
    ny_pipeline_visit_stmt_links(stmt->as.tr.handler, opt, visitor, ctx);
    return;
  case NY_S_DEFER:
    ny_pipeline_visit_stmt_links(stmt->as.de.body, opt, visitor, ctx);
    return;
  case NY_S_MATCH:
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i)
      ny_pipeline_visit_stmt_links(stmt->as.match.arms.data[i].conseq, opt,
                                   visitor, ctx);
    ny_pipeline_visit_stmt_links(stmt->as.match.default_conseq, opt, visitor,
                                 ctx);
    return;
  default:
    return;
  }
}

void ny_pipeline_visit_program_links_for_options(const program_t *prog,
                                                 const ny_options *opt,
                                                 nyr_link_visitor_t visitor,
                                                 void *ctx) {
  if (!prog || !visitor)
    return;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_pipeline_visit_stmt_links(prog->body.data[i], opt, visitor, ctx);
}

void ny_pipeline_visit_program_links(const program_t *prog,
                                     nyr_link_visitor_t visitor, void *ctx) {
  ny_pipeline_visit_program_links_for_options(prog, NULL, visitor, ctx);
}
