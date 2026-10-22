#ifndef NY_BACKEND_NATIVE_H
#define NY_BACKEND_NATIVE_H

#include "base/options.h"
#include "code/backend/ir/module.h"
#include <nyr/ir.h>
#include <nyr/target.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct nyr_tier_plan_impl_t {
  size_t compile_budget;
  size_t hot_threshold;
  size_t cold_threshold;
  unsigned cache_score;
  bool prefer_nir_vm;
  bool prefer_ast_fallback;
  const char *backend_name;
  const char *requested_tier;
  const char *resolved_tier;
} nyr_tier_plan_t;

typedef struct nyr_handoff_summary_impl_t {
  size_t entry_points;
  size_t return_points;
  size_t call_points;
  size_t branch_points;
  size_t label_points;
  size_t deopt_safe_points;
} nyr_handoff_summary_t;

typedef struct nyr_jit_image_impl_t {
  void *memory;
  size_t size;
  void *entry;
  /* Writable data pool (.Lnyarr literal lists); kept RW while code is RX. */
  void *data_memory;
  size_t data_size;
} nyr_jit_image_t;

bool nyr_vector_selftest(char *err, size_t err_len);
bool nyr_tier_plan_init(nyr_tier_plan_t *plan,
                        const ny_native_target_info_t *target,
                        const ny_options *opt);
bool nyr_handoff_summary(const nyr_func_t *nyir,
                         nyr_handoff_summary_t *summary);
bool nyr_write_tier_report_for_module(nyr_backend_module_t *module,
                                      const ny_options *opt, char *err,
                                      size_t err_len);

/*
 * Build optimized NYIR for a program's rt_main and all user functions.
 * On success, the caller owns *out and must free it with nyr_ir_func_free.
 * Returns true if at least one function was lowered; false on error.
 * If only functions (not rt_main) lowered, *rt_main_out is left empty.
 */
bool nyr_jit_compile(nyr_backend_module_t *module, const ny_options *opt,
                     const char *stencil_cache_path,
                     nyr_jit_image_t *image, char *err, size_t err_len);
void nyr_jit_image_free(nyr_jit_image_t *image);
/* Load an unrelocated native image, resolving symbols in this process. */
bool nyr_jit_cache_load(const char *path,
                         const nyr_runtime_services_t *runtime_services,
                         nyr_jit_image_t *image, char *err, size_t err_len);
bool nyr_oracle_fuzz(const ny_options *opt, int count,
                     const nyr_runtime_services_t *runtime_services,
                     char *err, size_t err_len);

#endif
