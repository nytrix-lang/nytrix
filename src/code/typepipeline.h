#ifndef NY_CODE_TYPEPIPELINE_H
#define NY_CODE_TYPEPIPELINE_H

#include <stdbool.h>

typedef struct program_t program_t;
typedef struct codegen_t codegen_t;

typedef enum ny_type_pipeline_stage_t {
  NY_TYPE_PIPELINE_STAGE_OK = 0,
  NY_TYPE_PIPELINE_STAGE_HM = 1,
  NY_TYPE_PIPELINE_STAGE_TRAIT = 2,
  NY_TYPE_PIPELINE_STAGE_ABI = 3,
} ny_type_pipeline_stage_t;

char *ny_type_pipeline_typed_json(program_t *prog, codegen_t *cg, const char *source_name,
                                  bool include_std);
char *ny_type_pipeline_resolved_json(program_t *prog, codegen_t *cg, const char *source_name,
                                     bool include_std);
char *ny_type_pipeline_refined_json(program_t *prog, codegen_t *cg, const char *source_name,
                                    bool include_std);
char *ny_type_pipeline_lowered_json(program_t *prog, codegen_t *cg, const char *source_name,
                                    bool include_std);
int ny_type_pipeline_validate_hm(program_t *prog, codegen_t *cg, const char *source_name,
                                 bool include_std, bool emit_diagnostics);
int ny_type_pipeline_validate_trait(program_t *prog, codegen_t *cg, const char *source_name,
                                    bool include_std, bool emit_diagnostics);
int ny_type_pipeline_validate_abi(program_t *prog, codegen_t *cg, const char *source_name,
                                  bool include_std, bool emit_diagnostics);
int ny_type_pipeline_validate_semantics(program_t *prog, codegen_t *cg, const char *source_name,
                                        bool include_std, ny_type_pipeline_stage_t max_stage,
                                        ny_type_pipeline_stage_t *failed_stage,
                                        bool emit_diagnostics, char **errors_json_out);

#endif
