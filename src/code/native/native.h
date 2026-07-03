#ifndef NY_NATIVE_BACKEND_H
#define NY_NATIVE_BACKEND_H

#include "base/options.h"
#include "code/native/ir.h"
#include "parse/ast.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  NY_NATIVE_TARGET_UNKNOWN = 0,
  NY_NATIVE_TARGET_X86_64,
  NY_NATIVE_TARGET_X86,
  NY_NATIVE_TARGET_AARCH64,
  NY_NATIVE_TARGET_AMDGPU,
  NY_NATIVE_TARGET_ARM,
  NY_NATIVE_TARGET_AVR,
  NY_NATIVE_TARGET_BPF,
  NY_NATIVE_TARGET_MIPS,
  NY_NATIVE_TARGET_POWERPC,
  NY_NATIVE_TARGET_RISCV,
  NY_NATIVE_TARGET_WASM,
} ny_native_target_t;

typedef enum {
  NY_NATIVE_CAP_NIR_ASM = 1u << 0,
  NY_NATIVE_CAP_AST_FALLBACK = 1u << 1,
  NY_NATIVE_CAP_ASM_OBJECT = 1u << 2,
  NY_NATIVE_CAP_NIR_VM = 1u << 3,
  NY_NATIVE_CAP_ELF_OBJECT = 1u << 4,
  NY_NATIVE_CAP_COFF_OBJECT = 1u << 5,
  NY_NATIVE_CAP_MACHO_OBJECT = 1u << 6,
} ny_native_target_cap_t;

typedef struct ny_native_target_info_t {
  ny_native_target_t target;
  ny_native_abi_t abi;
  const char *target_name;
  const char *abi_name;
  const char *object_format;
  const char *symbol_prefix;
  const char *float_abi_name;
  size_t pointer_bits;
  const char *gp_arg_regs[6];
  size_t gp_arg_reg_count;
  size_t shadow_space_bytes;
  size_t stack_align;
  unsigned caps;
  bool red_zone;
} ny_native_target_info_t;

typedef struct ny_native_tier_plan_t {
  size_t compile_budget;
  size_t hot_threshold;
  size_t cold_threshold;
  unsigned cache_score;
  bool prefer_nir_vm;
  bool prefer_ast_fallback;
  const char *backend_name;
} ny_native_tier_plan_t;

typedef struct ny_native_handoff_summary_t {
  size_t entry_points;
  size_t return_points;
  size_t call_points;
  size_t branch_points;
  size_t label_points;
  size_t deopt_safe_points;
} ny_native_handoff_summary_t;

bool ny_native_target_info_init(ny_native_target_info_t *info,
                                const ny_options *opt);
bool ny_native_tier_plan_init(ny_native_tier_plan_t *plan,
                              const ny_native_target_info_t *target,
                              const ny_options *opt);
bool ny_native_handoff_summary(const ny_nir_func_t *nir,
                               ny_native_handoff_summary_t *summary);
bool ny_native_write_tier_report_for_program(const program_t *prog,
                                             const ny_options *opt, char *err,
                                             size_t err_len);

/*
 * Build optimized NIR for a program's rt_main and all user functions.
 * On success, the caller owns *out and must free it with ny_nir_func_free.
 * Returns true if at least one function was lowered; false on error.
 * If only functions (not rt_main) lowered, *rt_main_out is left empty.
 */
bool ny_native_build_nir(const program_t *prog, const ny_options *opt,
                         ny_nir_func_t *rt_main_out,
                         ny_nir_func_t *funcs_out, size_t *func_count,
                         size_t max_funcs, char *err, size_t err_len);

bool ny_native_emit_asm(const program_t *prog, const ny_options *opt,
                        const char *path, char *err, size_t err_len);
bool ny_native_emit_asm_entry(const program_t *prog, const ny_options *opt,
                              const char *path, const char *entry_name,
                              bool tag_return, char *err, size_t err_len);
bool ny_native_emit_object(const program_t *prog, const ny_options *opt,
                           const char *path, const char *entry_name,
                           bool tag_return, char *err, size_t err_len);
bool ny_native_dump_ir_for_program(const program_t *prog, const ny_options *opt,
                                   char *err, size_t err_len);
bool ny_native_eval_ir_for_program(const program_t *prog, const ny_options *opt,
                                   char *err, size_t err_len);
bool ny_native_eval_ir_binary_file(const char *path, const ny_options *opt,
                                   char *err, size_t err_len);
bool ny_native_result_oracle_for_program(const program_t *prog,
                                         const ny_options *opt, char *err,
                                         size_t err_len);

#endif
