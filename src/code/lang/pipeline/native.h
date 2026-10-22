#ifndef NY_LANG_PIPELINE_NATIVE_H
#define NY_LANG_PIPELINE_NATIVE_H

#include "base/options.h"
#include "code/backend/ir/module.h"
#include "code/lang/parse/ast.h"

struct codegen_t;
typedef struct nyr_jit_image_impl_t nyr_jit_image_t;

typedef void (*nyr_link_visitor_t)(const char *library, void *ctx);

const nyr_runtime_services_t *ny_pipeline_runtime_services_get(void);
char *ny_pipeline_native_cache_path(const char *source, const ny_options *opt);

/* Language-owned source adapter: lower a program to neutral NYIR, then hand
 * the module to the backend LLVM emitter. */
bool ny_pipeline_emit_llvm_program(struct codegen_t *cg,
                                   const program_t *program,
                                   const ny_options *options, char *err,
                                   size_t err_len);

bool ny_pipeline_emit_asm(const program_t *program, const ny_options *options,
                          const char *path, char *err, size_t err_len);
bool ny_pipeline_emit_asm_entry(const program_t *program,
                                const ny_options *options, const char *path,
                                const char *entry_name, bool tag_return,
                                char *err, size_t err_len);
bool ny_pipeline_emit_program_object(const program_t *program,
                                     const ny_options *options,
                                     const char *path, const char *entry_name,
                                     bool tag_return, char *err,
                                     size_t err_len);
bool ny_pipeline_eval_ir_for_program(const program_t *program,
                                     const ny_options *options, char *err,
                                     size_t err_len);
bool ny_pipeline_eval_ir_binary_file(const char *path,
                                     const ny_options *options, char *err,
                                     size_t err_len);
bool ny_pipeline_dump_ir_for_program(const program_t *program,
                                     const ny_options *options, char *err,
                                     size_t err_len);
bool ny_pipeline_write_tier_report(const program_t *program,
                                   const ny_options *options, char *err,
                                   size_t err_len);
bool ny_pipeline_result_oracle(const program_t *program,
                               const ny_options *options, char *err,
                               size_t err_len);
bool ny_pipeline_jit_compile(const program_t *program,
                             const ny_options *options, nyr_jit_image_t *image,
                             char *err, size_t err_len);
void ny_pipeline_visit_program_links(const program_t *program,
                                     nyr_link_visitor_t visitor, void *ctx);
void ny_pipeline_visit_program_links_for_options(const program_t *program,
                                                 const ny_options *options,
                                                 nyr_link_visitor_t visitor,
                                                 void *ctx);

#endif
