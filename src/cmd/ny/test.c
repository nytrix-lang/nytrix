/*
 * CLI-only conformance checks for the public embedding API.
 */
#include "cmd/ny/test.h"
#include <nytrix/nytrix.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nyr_embed_return(nyr_func_t *func, int value, char *err,
                             size_t err_len) {
  return nyr_emit(func,
                  (nyr_inst_t){.op = NYIR_RET,
                                .dst = -1,
                                .a = value,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1},
                  NULL, err, err_len);
}

static bool nyr_embed_const_function(nyr_func_t *func, int64_t constant,
                                     size_t param_count, char *err,
                                     size_t err_len) {
  nyr_func_init(func);
  func->param_count = param_count;
  if (param_count) {
    func->param_types = calloc(param_count, sizeof(*func->param_types));
    if (!func->param_types) {
      if (err && err_len)
        snprintf(err, err_len, "out of memory preparing test parameters");
      return false;
    }
    for (size_t i = 0; i < param_count; ++i)
      func->param_types[i] = NYIR_PARAM_I64;
  }
  int value = -1;
  if (!nyr_emit(func,
                (nyr_inst_t){.op = NYIR_CONST_I64,
                              .dst = -1,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = constant,
                              .scalar_class = NYIR_SCALAR_I64_RAW},
                &value, err, err_len))
    return false;
  return nyr_embed_return(func, value, err, err_len);
}

static bool nyr_embed_call_function(nyr_func_t *func, const char *symbol,
                                    const int64_t *constants, size_t arg_count,
                                    char *err, size_t err_len) {
  nyr_func_init(func);
  int args[2] = {-1, -1};
  if (arg_count > 2)
    return false;
  for (size_t i = 0; i < arg_count; ++i) {
    if (!nyr_emit(func,
                  (nyr_inst_t){.op = NYIR_CONST_I64,
                                .dst = -1,
                                .a = -1,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .imm = constants[i],
                                .scalar_class = NYIR_SCALAR_I64_RAW},
                  &args[i], err, err_len))
      return false;
  }
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = arg_count > 0 ? args[0] : -1,
                      .b = arg_count > 1 ? args[1] : -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = (int64_t)arg_count,
                      .symbol = symbol,
                      .effects = NYIR_EFFECT_CALL,
                      .scalar_class = NYIR_SCALAR_I64_RAW};
  int result = -1;
  if (!nyr_emit(func, call, &result, err, err_len))
    return false;
  return nyr_embed_return(func, result, err, err_len);
}

static bool nyr_embed_host_sum(void *userdata, const char *symbol,
                               const int64_t *args, size_t arg_count,
                               int64_t *result, char *err, size_t err_len) {
  (void)userdata;
  if (strcmp(symbol, "host_sum") != 0 || arg_count != 2) {
    if (err && err_len)
      snprintf(err, err_len, "unexpected host callback request");
    return false;
  }
  *result = args[0] + args[1];
  return true;
}

static int nyr_embed_fail(char *err, const char *message) {
  fprintf(stderr, "embedding test: %s%s%s\n", message,
          err && err[0] ? ": " : "", err && err[0] ? err : "");
  return 1;
}

int nytrix_embedding_test(void) {
  char err[256] = {0};
  nyr_func_t leaf, parent, host_caller, argumented;
  bool leaf_init = false, parent_init = false, host_init = false,
       argumented_init = false;
  nytrix_vm_t *vm = NULL;
  nyr_eval_result_t result = {0};
  char *artifact = NULL;
  bool embed_started = false;
  int status = 1;

  nyr_version_t version = nyr_version();
  nyr_target_t target = {0};
  if (version.major != NYR_API_MAJOR || version.minor != NYR_API_MINOR ||
      !nyr_host_target(&target, err, sizeof(err)) || err[0] || !target.target ||
      !*target.target || !target.object_format || !*target.object_format ||
      target.pointer_bits == 0 || !(target.capabilities & NYR_CAP_VM)) {
    if (!err[0])
      snprintf(err, sizeof(err), "public Nyr version/target discovery failed");
    goto fail;
  }
  if (!nytrix_embed_startup(err, sizeof(err)))
    goto fail;
  embed_started = true;

  if (!nyr_embed_const_function(&leaf, 41, 0, err, sizeof(err)))
    goto fail;
  leaf_init = true;
  snprintf(err, sizeof(err), "stale diagnostic");
  if (!nyr_verify(&leaf, err, sizeof(err)) || err[0]) {
    if (!err[0])
      snprintf(err, sizeof(err), "successful verification left an error");
    goto fail;
  }
  snprintf(err, sizeof(err), "stale diagnostic");
  if (!nyr_eval(&leaf, NULL, 0, 100, &result, err, sizeof(err)) ||
      result.result != 41 || err[0]) {
    if (!err[0])
      snprintf(err, sizeof(err), "successful evaluation left an error");
    goto fail;
  }
  nyr_eval_result_destroy(&result);
  if (!nyr_embed_call_function(&parent, "leaf", NULL, 0, err, sizeof(err)))
    goto fail;
  parent_init = true;
  const int64_t host_args[] = {19, 23};
  if (!nyr_embed_call_function(&host_caller, "host_sum", host_args, 2, err,
                               sizeof(err)))
    goto fail;
  host_init = true;
  if (nyr_eval(&host_caller, NULL, 0, 100, &result, err, sizeof(err)) ||
      !strstr(err, "does not execute external calls")) {
    if (!err[0])
      snprintf(err, sizeof(err),
               "no-resolver evaluation did not reject NYIR_CALL");
    goto fail;
  }
  nyr_eval_result_destroy(&result);
  snprintf(err, sizeof(err), "stale diagnostic");
  if (!nyr_eval_with_calls(&host_caller, NULL, 0, 100, &result,
                           nyr_embed_host_sum, NULL, err, sizeof(err)) ||
      result.result != 42 || err[0]) {
    if (!err[0])
      snprintf(err, sizeof(err), "public Nyr resolver evaluation returned %lld",
               (long long)result.result);
    goto fail;
  }
  nyr_eval_result_destroy(&result);
  if (!nyr_embed_const_function(&argumented, 7, 1, err, sizeof(err)))
    goto fail;
  argumented_init = true;

  const nytrix_vm_config_t config = {
      .max_steps = 1000, .max_call_depth = 4, .host_call = nyr_embed_host_sum};
  vm = nytrix_vm_create(&config, err, sizeof(err));
  if (!vm)
    goto fail;
  if (!nytrix_vm_register_function(vm, "leaf", &leaf, err, sizeof(err)) ||
      !nytrix_vm_register_function(vm, "parent", &parent, err, sizeof(err)) ||
      !nytrix_vm_register_function(vm, "host_caller", &host_caller, err,
                                   sizeof(err)) ||
      !nytrix_vm_register_function(vm, "argumented", &argumented, err,
                                   sizeof(err)))
    goto fail;

  if (!nytrix_vm_call(vm, "parent", NULL, 0, 0, &result, err, sizeof(err)) ||
      result.result != 41) {
    if (!err[0])
      snprintf(err, sizeof(err), "registered nested call returned %lld",
               (long long)result.result);
    goto fail;
  }
  nyr_eval_result_destroy(&result);
  if (!nytrix_vm_call(vm, "host_caller", NULL, 0, 0, &result, err,
                      sizeof(err)) ||
      result.result != 42) {
    if (!err[0])
      snprintf(err, sizeof(err), "host callback returned %lld",
               (long long)result.result);
    goto fail;
  }
  nyr_eval_result_destroy(&result);
  const int64_t one_arg[] = {5};
  if (!nytrix_vm_call(vm, "argumented", one_arg, 1, 0, &result, err,
                      sizeof(err)) ||
      result.result != 7) {
    if (!err[0])
      snprintf(err, sizeof(err), "argumented call returned %lld",
               (long long)result.result);
    goto fail;
  }
  nyr_eval_result_destroy(&result);

  if (nytrix_vm_call(vm, "argumented", NULL, 0, 0, &result, err, sizeof(err))) {
    snprintf(err, sizeof(err), "wrong argument count was accepted");
    goto fail;
  }
  if (!nytrix_vm_unregister_function(vm, "parent", err, sizeof(err)) ||
      nytrix_vm_call(vm, "parent", NULL, 0, 0, &result, err, sizeof(err))) {
    if (!err[0])
      snprintf(err, sizeof(err), "unregistered function remained callable");
    goto fail;
  }

  const char *source = "fn main() int { return 0 }\n";
  nytrix_embed_run_config_t run_config = {.opt_level = 2, .reserved = 0};
  if (nytrix_embed_run_source(source, &run_config) != 0) {
    snprintf(err, sizeof(err), "embedded source execution failed");
    goto fail;
  }
  if (nytrix_embed_compile_source(source, NULL, &artifact) != 0 || !artifact ||
      !strstr(artifact, "\"schema\":\"ny.semantic-artifact.v1\"")) {
    snprintf(err, sizeof(err),
             "compile-only semantic artifact was not returned");
    goto fail;
  }
  nytrix_embed_compile_artifact_free(artifact);
  artifact = NULL;
  if (nytrix_embed_compile_source(
          "fn main() int { return \"not an integer\" }\n", NULL, &artifact) ==
          0 ||
      artifact) {
    snprintf(err, sizeof(err), "ill-typed source produced an artifact");
    goto fail;
  }
  status = 0;
  puts("Nytrix embedding test: NYIR VM, host calls, source execution, "
       "compile-only artifacts, argument checks, and unregister passed");
  goto cleanup;

fail:
  status = nyr_embed_fail(err, "embedding VM contract failed");
cleanup:
  nytrix_embed_compile_artifact_free(artifact);
  nyr_eval_result_destroy(&result);
  nytrix_vm_destroy(vm);
  if (argumented_init)
    nyr_func_destroy(&argumented);
  if (host_init)
    nyr_func_destroy(&host_caller);
  if (parent_init)
    nyr_func_destroy(&parent);
  if (leaf_init)
    nyr_func_destroy(&leaf);
  if (embed_started && !nytrix_embed_shutdown(err, sizeof(err)) && status == 0)
    status = nyr_embed_fail(err, "embedding shutdown failed");
  return status;
}
