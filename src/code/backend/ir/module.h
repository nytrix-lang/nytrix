#ifndef NY_BACKEND_IR_MODULE_H
#define NY_BACKEND_IR_MODULE_H

#include <nyr/ir.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Optional language/runtime services used only when NYIR is turned into
 * executable code. The backend owns no runtime manifest or ABI policy. */
typedef struct {
  void *userdata;
  void *(*resolve_symbol)(void *userdata, const char *name);
  void (*register_mapping)(void *userdata, uintptr_t address, size_t size);
  bool (*evaluate_ir)(void *userdata, const void *options,
                      nyr_func_t *entry, nyr_func_t *functions,
                      const char *const *function_names, size_t function_count,
                      nyr_eval_result_t *result, char *err, size_t err_len);
  bool (*collect_vm_profile)(void *userdata, const void *options,
                             nyr_func_t *entry, nyr_func_t *functions,
                             const char *const *function_names,
                             size_t function_count,
                             nyr_eval_result_t *profile, char *err,
                             size_t err_len);
} nyr_runtime_services_t;

/* Language-neutral NYIR module view. Functions and names are owned by the
 * caller; an emitter may explicitly document when it consumes function
 * bodies, while read-only operations borrow them. */
typedef struct {
  nyr_func_t *entry;
  nyr_func_t *functions;
  const char *const *function_names;
  size_t function_count;
  const nyr_runtime_services_t *runtime_services;
} nyr_backend_module_t;

typedef struct {
  bool json;
  int depth;
} nyr_callgraph_options_t;

bool nyr_write_callgraph_for_ir(const nyr_backend_module_t *module,
                                const nyr_callgraph_options_t *options,
                                char *err, size_t err_len);

#endif
