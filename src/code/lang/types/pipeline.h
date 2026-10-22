#ifndef NY_CODE_TYPEPIPELINE_H
#define NY_CODE_TYPEPIPELINE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct program_t program_t;
typedef struct codegen_t codegen_t;
typedef struct stmt_t stmt_t;

/* Run the existing trusted effect visitor for one function and retain its
 * per-call summaries on the AST for read-only semantic artifacts. */
void ny_analyze_function_call_effect_facts(codegen_t *cg, stmt_t *fn_stmt);

/* Read-only callable effect summary for semantic artifacts: true with the
 * closed effect mask when the declaration row is known or an audited runtime
 * builtin contract exists; false leaves `effects` untouched. */
bool ny_sig_callable_effect_facts(const fun_sig *sig, uint32_t *effects);

typedef enum ny_type_pipeline_stage_t {
  NY_TYPE_PIPELINE_STAGE_OK = 0,
  NY_TYPE_PIPELINE_STAGE_HM = 1,
  NY_TYPE_PIPELINE_STAGE_TRAIT = 2,
  NY_TYPE_PIPELINE_STAGE_ABI = 3,
} ny_type_pipeline_stage_t;

typedef enum ny_comptime_effect_status_t {
  NY_COMPTIME_EFFECTS_SAFE = 0,
  NY_COMPTIME_EFFECTS_IO = 1,
  NY_COMPTIME_EFFECTS_INCOMPLETE = 2,
} ny_comptime_effect_status_t;

char *ny_type_pipeline_typed_json(program_t *prog, codegen_t *cg,
                                  const char *source_name, bool include_std);
char *ny_type_pipeline_resolved_json(program_t *prog, codegen_t *cg,
                                     const char *source_name, bool include_std);
char *ny_type_pipeline_refined_json(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std);
char *ny_type_pipeline_lowered_json(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std);
int ny_type_pipeline_validate_hm(program_t *prog, codegen_t *cg,
                                 const char *source_name, bool include_std,
                                 bool emit_diagnostics);
int ny_type_pipeline_list_fallbacks(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std);
int ny_type_pipeline_explain_semantics(program_t *prog, codegen_t *cg,
                                       const char *source_name,
                                       bool include_std, const char *topic);
int ny_type_pipeline_validate_trait(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std,
                                    bool emit_diagnostics);
int ny_type_pipeline_validate_abi(program_t *prog, codegen_t *cg,
                                  const char *source_name, bool include_std,
                                  bool emit_diagnostics);
int ny_type_pipeline_validate_semantics(
    program_t *prog, codegen_t *cg, const char *source_name, bool include_std,
    ny_type_pipeline_stage_t max_stage, ny_type_pipeline_stage_t *failed_stage,
    bool emit_diagnostics, char **errors_json_out);
/* Rebuild call-site evidence when type validation came from a trusted artifact
 * or parent compilation. Proofs remain tied to the current solver policy. */
bool ny_type_pipeline_verify_proof_claims(program_t *prog, codegen_t *cg,
                                          const char *source_name);
/* Persist backend-facing semantic facts after lazy import/codegen discovery has
 * completed. This pass emits no code and no diagnostics. */
void ny_type_pipeline_persist_native_facts(program_t *prog, codegen_t *cg,
                                           const char *source_name);
void ny_type_pipeline_persist_native_reachable_facts(
    program_t *prog, const stmt_t *const *functions, size_t function_count,
    const char *source_name);
ny_comptime_effect_status_t ny_stmt_comptime_effect_status(codegen_t *cg,
                                                            stmt_t *stmt);

#endif
