#ifndef NY_LANG_PIPELINE_NATIVE_VM_H
#define NY_LANG_PIPELINE_NATIVE_VM_H

#include "base/options.h"
#include <nyr/ir.h>

bool ny_lang_eval_ir_value(nyr_func_t *entry, nyr_func_t *functions,
                           const char *const *function_names,
                           size_t function_count, const ny_options *options,
                           nyr_eval_result_t *result, char *err,
                           size_t err_len);
bool ny_lang_run_ir(nyr_func_t *entry, nyr_func_t *functions,
                    const char *const *function_names,
                    size_t function_count, const ny_options *options,
                    const char *name, char *err, size_t err_len);
bool ny_lang_collect_vm_profile(nyr_func_t *entry, nyr_func_t *functions,
                                const char *const *function_names,
                                size_t function_count,
                                const ny_options *options,
                                nyr_eval_result_t *profile, char *err,
                                size_t err_len);
bool ny_lang_eval_ir_binary_file(const char *path,
                                 const ny_options *options, char *err,
                                 size_t err_len);

#endif
