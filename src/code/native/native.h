#ifndef NY_NATIVE_BACKEND_H
#define NY_NATIVE_BACKEND_H

#include "base/options.h"
#include "code/native/ir.h"
#include "code/native/ir/machine.h"
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
  /* The target has a Nytrix-owned in-memory executable encoder for the host
   * architecture.  Text assembly or an object writer alone must not imply
   * that `--native-only` can execute it live. */
  NY_NATIVE_CAP_LIVE_JIT = 1u << 7,
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
  const char *gp_arg_regs[8];
  size_t gp_arg_reg_count;
  /* Floating-point argument registers are target/ABI-owned.  Keep them
   * separate from GP argument registers because SysV assigns each class an
   * independent sequence, while other ABIs may impose different rules. */
  const char *fp_arg_regs[8];
  size_t fp_arg_reg_count;
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
  const char *requested_tier;
  const char *resolved_tier;
} ny_native_tier_plan_t;

typedef struct ny_native_handoff_summary_t {
  size_t entry_points;
  size_t return_points;
  size_t call_points;
  size_t branch_points;
  size_t label_points;
  size_t deopt_safe_points;
} ny_native_handoff_summary_t;

typedef struct ny_native_jit_image_t {
  void *memory;
  size_t size;
  void *entry;
} ny_native_jit_image_t;

typedef void (*ny_native_link_visitor_t)(const char *library, void *ctx);

void ny_native_visit_program_links(const program_t *prog,
                                   ny_native_link_visitor_t visitor,
                                   void *ctx);

bool ny_native_target_info_init(ny_native_target_info_t *info,
                                const ny_options *opt);
bool ny_native_tier_plan_init(ny_native_tier_plan_t *plan,
                              const ny_native_target_info_t *target,
                              const ny_options *opt);
bool ny_native_handoff_summary(const nyir_func_t *nyir,
                               ny_native_handoff_summary_t *summary);
bool ny_native_write_tier_report_for_program(const program_t *prog,
                                             const ny_options *opt, char *err,
                                             size_t err_len);

/*
 * Build optimized NYIR for a program's rt_main and all user functions.
 * On success, the caller owns *out and must free it with nyir_func_free.
 * Returns true if at least one function was lowered; false on error.
 * If only functions (not rt_main) lowered, *rt_main_out is left empty.
 */
bool ny_native_build_nir(const program_t *prog, const ny_options *opt,
                         nyir_func_t *rt_main_out,
                         nyir_func_t *funcs_out, size_t *func_count,
                         size_t max_funcs, char *err, size_t err_len);

bool ny_native_emit_asm(const program_t *prog, const ny_options *opt,
                        const char *path, char *err, size_t err_len);
bool ny_native_emit_asm_entry(const program_t *prog, const ny_options *opt,
                              const char *path, const char *entry_name,
                              bool tag_return, char *err, size_t err_len);
bool ny_native_emit_object(const program_t *prog, const ny_options *opt,
                           const char *path, const char *entry_name,
                           bool tag_return, char *err, size_t err_len);
bool ny_native_jit_compile(const program_t *prog, const ny_options *opt,
                           ny_native_jit_image_t *image, char *err,
                           size_t err_len);
void ny_native_jit_image_free(ny_native_jit_image_t *image);
bool ny_native_dump_ir_for_program(const program_t *prog, const ny_options *opt,
                                   char *err, size_t err_len);
bool ny_native_eval_ir_for_program(const program_t *prog, const ny_options *opt,
                                   char *err, size_t err_len);
bool ny_native_eval_ir_binary_file(const char *path, const ny_options *opt,
                                   char *err, size_t err_len);
bool ny_native_result_oracle_for_program(const program_t *prog,
                                         const ny_options *opt, char *err,
                                         size_t err_len);
bool ny_native_oracle_fuzz(const ny_options *opt, int count, char *err,
                           size_t err_len);

#endif
