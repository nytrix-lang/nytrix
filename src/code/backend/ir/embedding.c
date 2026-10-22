#include <nytrix/nytrix.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *name;
  const nyr_func_t *func;
} nytrix_function_entry_t;

struct nytrix_vm {
  nytrix_function_entry_t *functions;
  size_t function_count;
  size_t function_capacity;
  size_t max_steps;
  size_t max_call_depth;
  size_t call_depth;
  nytrix_host_call_fn host_call;
  void *host_userdata;
};

static void nytrix_set_error(char *err, size_t err_len, const char *message) {
  if (err && err_len)
    snprintf(err, err_len, "%s", message);
}

static void nytrix_set_named_error(char *err, size_t err_len,
                                   const char *prefix, const char *name) {
  if (err && err_len)
    snprintf(err, err_len, "%s '%s'", prefix, name ? name : "");
}

static nytrix_function_entry_t *nytrix_find_function(nytrix_vm_t *vm,
                                                     const char *name) {
  if (!vm || !name)
    return NULL;
  for (size_t i = 0; i < vm->function_count; ++i)
    if (strcmp(vm->functions[i].name, name) == 0)
      return &vm->functions[i];
  return NULL;
}

nytrix_vm_t *nytrix_vm_create(const nytrix_vm_config_t *config, char *err,
                              size_t err_len) {
  nytrix_vm_t *vm = calloc(1, sizeof(*vm));
  if (!vm) {
    nytrix_set_error(err, err_len, "out of memory creating Nytrix VM");
    return NULL;
  }
  vm->max_steps = config && config->max_steps ? config->max_steps
                                              : NYTRIX_VM_DEFAULT_MAX_STEPS;
  vm->max_call_depth = config && config->max_call_depth
                           ? config->max_call_depth
                           : NYTRIX_VM_DEFAULT_MAX_CALL_DEPTH;
  vm->host_call = config ? config->host_call : NULL;
  vm->host_userdata = config ? config->host_userdata : NULL;
  if (err && err_len)
    err[0] = '\0';
  return vm;
}

void nytrix_vm_destroy(nytrix_vm_t *vm) {
  if (!vm)
    return;
  for (size_t i = 0; i < vm->function_count; ++i)
    free(vm->functions[i].name);
  free(vm->functions);
  free(vm);
}

bool nytrix_vm_register_function(nytrix_vm_t *vm, const char *name,
                                 const nyr_func_t *func, char *err,
                                 size_t err_len) {
  if (!vm || !name || !*name || !func) {
    nytrix_set_error(err, err_len,
                     "VM, non-empty function name, and function are required");
    return false;
  }
  if (vm->call_depth != 0) {
    nytrix_set_error(err, err_len, "cannot change VM functions during a call");
    return false;
  }
  if (nytrix_find_function(vm, name)) {
    nytrix_set_named_error(err, err_len, "function already registered:", name);
    return false;
  }
  if (!nyr_verify(func, err, err_len))
    return false;

  if (vm->function_count == vm->function_capacity) {
    size_t next_capacity =
        vm->function_capacity ? vm->function_capacity * 2 : 8;
    if (next_capacity < vm->function_capacity ||
        next_capacity > SIZE_MAX / sizeof(*vm->functions)) {
      nytrix_set_error(err, err_len, "VM function registry capacity overflow");
      return false;
    }
    nytrix_function_entry_t *next =
        realloc(vm->functions, next_capacity * sizeof(*vm->functions));
    if (!next) {
      nytrix_set_error(err, err_len,
                       "out of memory growing VM function registry");
      return false;
    }
    vm->functions = next;
    vm->function_capacity = next_capacity;
  }

  size_t name_len = strlen(name);
  if (name_len == SIZE_MAX) {
    nytrix_set_error(err, err_len, "function name is too long");
    return false;
  }
  char *name_copy = malloc(name_len + 1);
  if (!name_copy) {
    nytrix_set_error(err, err_len, "out of memory copying VM function name");
    return false;
  }
  memcpy(name_copy, name, name_len + 1);
  vm->functions[vm->function_count++] =
      (nytrix_function_entry_t){.name = name_copy, .func = func};
  if (err && err_len)
    err[0] = '\0';
  return true;
}

bool nytrix_vm_unregister_function(nytrix_vm_t *vm, const char *name, char *err,
                                   size_t err_len) {
  if (!vm || !name || !*name) {
    nytrix_set_error(err, err_len,
                     "VM and non-empty function name are required");
    return false;
  }
  if (vm->call_depth != 0) {
    nytrix_set_error(err, err_len, "cannot change VM functions during a call");
    return false;
  }
  for (size_t i = 0; i < vm->function_count; ++i) {
    if (strcmp(vm->functions[i].name, name) != 0)
      continue;
    free(vm->functions[i].name);
    if (i + 1 < vm->function_count)
      memmove(&vm->functions[i], &vm->functions[i + 1],
              (vm->function_count - i - 1) * sizeof(*vm->functions));
    --vm->function_count;
    if (err && err_len)
      err[0] = '\0';
    return true;
  }
  nytrix_set_named_error(err, err_len, "function not registered:", name);
  return false;
}

static bool nytrix_eval_registered(nytrix_vm_t *vm, const char *name,
                                   const int64_t *args, size_t arg_count,
                                   int64_t *value, nyr_eval_result_t *result,
                                   char *err, size_t err_len);

static bool nytrix_resolve_call(void *userdata, const char *symbol,
                                const int64_t *args, size_t arg_count,
                                int64_t *result, char *err, size_t err_len) {
  nytrix_vm_t *vm = userdata;
  if (!symbol || !*symbol) {
    nytrix_set_error(err, err_len, "NYIR call has no symbol");
    return false;
  }
  if (nytrix_find_function(vm, symbol))
    return nytrix_eval_registered(vm, symbol, args, arg_count, result, NULL,
                                  err, err_len);
  if (vm->host_call) {
    if (vm->host_call(vm->host_userdata, symbol, args, arg_count, result, err,
                      err_len))
      return true;
    if (!err || !err_len || !err[0])
      nytrix_set_named_error(err, err_len, "host call failed:", symbol);
    return false;
  }
  nytrix_set_named_error(err, err_len, "unresolved NYIR call:", symbol);
  return false;
}

static bool nytrix_eval_registered(nytrix_vm_t *vm, const char *name,
                                   const int64_t *args, size_t arg_count,
                                   int64_t *value, nyr_eval_result_t *result,
                                   char *err, size_t err_len) {
  nytrix_function_entry_t *entry = nytrix_find_function(vm, name);
  if (!entry) {
    nytrix_set_named_error(err, err_len, "function not registered:", name);
    return false;
  }
  const nyr_func_t *func = entry->func;
  if (arg_count != func->param_count || (arg_count && !args)) {
    if (err && err_len)
      snprintf(err, err_len, "function '%s' expects %zu argument(s), got %zu",
               name, func->param_count, arg_count);
    return false;
  }
  if (vm->call_depth >= vm->max_call_depth) {
    nytrix_set_error(err, err_len, "Nytrix VM call-depth limit exceeded");
    return false;
  }
  size_t local_count = nyr_max_local(func);
  if (local_count < func->param_count)
    local_count = func->param_count;
  if (local_count > SIZE_MAX / sizeof(int64_t)) {
    nytrix_set_error(err, err_len, "function local frame size overflow");
    return false;
  }
  int64_t *locals = local_count ? calloc(local_count, sizeof(*locals)) : NULL;
  if (local_count && !locals) {
    nytrix_set_error(err, err_len, "out of memory allocating VM local frame");
    return false;
  }
  if (arg_count)
    memcpy(locals, args, arg_count * sizeof(*locals));

  nyr_eval_result_t nested_result;
  bool owns_result = !result && value;
  if (owns_result)
    nyr_eval_result_init(&nested_result);
  nyr_eval_result_t *eval_result =
      result ? result : (owns_result ? &nested_result : NULL);
  ++vm->call_depth;
  bool ok =
      nyr_ir_eval_with_calls(func, locals, local_count, vm->max_steps,
                           eval_result, nytrix_resolve_call, vm, err, err_len);
  --vm->call_depth;
  free(locals);
  if (!ok) {
    if (eval_result)
      nyr_eval_result_destroy(eval_result);
    return false;
  }
  if (value)
    *value = eval_result ? eval_result->result : 0;
  if (owns_result)
    nyr_eval_result_destroy(&nested_result);
  if (err && err_len)
    err[0] = '\0';
  return true;
}

bool nytrix_vm_call(nytrix_vm_t *vm, const char *name, const int64_t *args,
                    size_t arg_count, size_t max_steps,
                    nyr_eval_result_t *result, char *err, size_t err_len) {
  if (!vm || !name || !*name) {
    nytrix_set_error(err, err_len,
                     "VM and non-empty function name are required");
    return false;
  }
  size_t saved_max_steps = vm->max_steps;
  if (max_steps)
    vm->max_steps = max_steps;
  if (result)
    nyr_eval_result_init(result);
  bool ok = nytrix_eval_registered(vm, name, args, arg_count, NULL, result, err,
                                   err_len);
  vm->max_steps = saved_max_steps;
  return ok;
}
