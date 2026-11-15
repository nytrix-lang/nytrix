/*
 * Command-line driver and option dispatch for the standalone C compiler.
 */
#include "base/debug.h"
#include "code/lang/c/parser/parse.h"

#include "base/version.h"
#include "code/backend/core/internal.h"
#include "code/backend/core/api.h"
#include "code/backend/core/nyr.h"
#include "code/backend/lower/machine.h"
#include "code/backend/mach/object.h"
#include "nyc_help.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <nyr/nyr.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

static char *read_source(const char *path, size_t *length) {
  if (strcmp(path, "-") == 0) {
    size_t capacity = 4096, used = 0;
    char *source = malloc(capacity);
    if (!source)
      return NULL;
    for (;;) {
      size_t read = fread(source + used, 1, capacity - used - 1, stdin);
      used += read;
      if (ferror(stdin)) {
        free(source);
        return NULL;
      }
      if (feof(stdin))
        break;
      if (capacity > SIZE_MAX / 2) {
        free(source);
        return NULL;
      }
      capacity *= 2;
      char *grown = realloc(source, capacity);
      if (!grown) {
        free(source);
        return NULL;
      }
      source = grown;
    }
    source[used] = '\0';
    *length = used;
    return source;
  }
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0 || (uintmax_t)size > SIZE_MAX - 1 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  size_t bytes = (size_t)size;
  char *source = malloc(bytes + 1);
  if (!source) {
    fclose(file);
    return NULL;
  }
  size_t read = fread(source, 1, bytes, file);
  bool ok = read == bytes && !ferror(file);
  fclose(file);
  if (!ok) {
    free(source);
    return NULL;
  }
  source[bytes] = '\0';
  *length = bytes;
  return source;
}

static int nyc_handle_probe_options(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-Wa,--version") == 0) {
#if defined(__unix__) || defined(__APPLE__)
      execlp("as", "as", "--version", (char *)NULL);
      perror("nyc: cannot execute assembler 'as'");
#else
      fputs("nyc: assembler version query is unavailable on this host\n",
            stderr);
#endif
      return 1;
    }
    if (strcmp(argv[i], "-dumpversion") == 0) {
      puts("12.0.0");
      return 0;
    }
    if (strcmp(argv[i], "-dumpmachine") == 0) {
      puts("x86_64-linux-gnu");
      return 0;
    }
  }
  return -1;
}

static nyc_abi_t nyc_abi_for_target(const char *triple) {
  if (!triple || !*triple)
    return NYC_ABI_UNSUPPORTED;
  if ((strncmp(triple, "x86_64", 6) == 0 || strncmp(triple, "amd64", 5) == 0) &&
      (strstr(triple, "windows") || strstr(triple, "mingw") ||
       strstr(triple, "msvc")))
    return NYC_ABI_WIN64;
  if ((strncmp(triple, "aarch64", 7) == 0 ||
       strncmp(triple, "arm64", 5) == 0) &&
      strstr(triple, "linux") && !strstr(triple, "windows") &&
      !strstr(triple, "msvc") && !strstr(triple, "mingw"))
    return NYC_ABI_AAPCS64;
  if ((strncmp(triple, "i386", 4) == 0 || strncmp(triple, "i486", 4) == 0 ||
       strncmp(triple, "i586", 4) == 0 || strncmp(triple, "i686", 4) == 0) &&
      !strstr(triple, "windows") && !strstr(triple, "mingw") &&
      !strstr(triple, "msvc"))
    return NYC_ABI_I386;
  if ((strncmp(triple, "x86_64", 6) == 0 || strncmp(triple, "amd64", 5) == 0) &&
      !strstr(triple, "windows") && !strstr(triple, "mingw") &&
      !strstr(triple, "msvc"))
    return NYC_ABI_SYSV_X86_64;
  return NYC_ABI_UNSUPPORTED;
}

static bool nyc_func_uses_fp_or_simd(const nyr_func_t *func);

static bool nyc_eliminate_pure_self_calls(nyc_unit_t *unit) {
  for (size_t f = 0; f < unit->count; ++f) {
    if (!unit->function_local[f] || nyc_func_uses_fp_or_simd(&unit->funcs[f]))
      continue;
    nyr_func_t *func = &unit->funcs[f];
    bool pure = true, recursive = false;
    for (size_t i = 0; i < func->len; ++i) {
      const nyr_inst_t *in = &func->data[i];
      if (in->op == NYIR_CALL) {
        if (!in->symbol || strcmp(in->symbol, unit->names[f]) != 0) {
          pure = false;
          break;
        }
        recursive = true;
      } else if (in->op == NYIR_INLINE_ASM ||
                 (nyr_effective_effects(in) &
                  ~(NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_WRITE_LOCAL |
                    NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP))) {
        pure = false;
        break;
      }
    }
    if (!pure || !recursive)
      continue;
    for (size_t i = 0; i < func->len; ++i) {
      nyr_inst_t *in = &func->data[i];
      if (in->op == NYIR_CALL && in->symbol &&
          strcmp(in->symbol, unit->names[f]) == 0) {
        in->flags |= NYIR_INST_F_EFFECTS_KNOWN;
        in->effects = NYIR_EFFECT_CALL;
      }
    }
    if (!nyr_cse(func) || !nyr_copy_prop(func) || !nyr_dce(func) ||
        !nyr_compact(func))
      return false;
    nyr_refresh_metadata(func);
  }
  return true;
}

static bool nyc_func_uses_va_list(const nyr_func_t *func) {
  if (!func)
    return false;
  for (size_t i = 0; i < func->len; ++i) {
    nyr_op_t op = func->data[i].op;
    if (op == NYIR_VA_SAVE || op == NYIR_VA_START || op == NYIR_VA_ARG)
      return true;
  }
  return false;
}

/*
 * VA_START owns the ABI-specific initialization sequence and VA_SAVE may
 * expose the register-save area. Keep those functions out of the generic
 * optimizer until its alias handling models that state precisely. A plain
 * VA_ARG consumer operates on an incoming va_list and can still benefit from
 * ordinary scalar optimization; the VA_ARG instruction itself remains
 * effectful in NYIR.
 */
static bool nyc_func_initializes_va_list(const nyr_func_t *func) {
  if (!func)
    return false;
  for (size_t i = 0; i < func->len; ++i) {
    nyr_op_t op = func->data[i].op;
    if (op == NYIR_VA_SAVE || op == NYIR_VA_START)
      return true;
  }
  return false;
}

/*
 * Before SSA promotion, C scalar parameters use ABI local slots 0..N-1,
 * just like the native frontend. Re-entering a frame is only safe when no
 * stack object can escape to a recursive invocation.
 */
static bool nyc_func_can_rebind_tail_args(const nyr_func_t *func) {
  if (func->return_aggregate_size || nyc_func_uses_va_list(func))
    return false;
  for (size_t i = 0; i < func->param_count; ++i)
    if (!func->param_types || func->param_types[i] != NYIR_PARAM_I64)
      return false;
  for (size_t i = 0; i < func->len; ++i)
    if (func->data[i].op == NYIR_ADDR_LOCAL || func->data[i].op == NYIR_ALLOCA)
      return false;
  return true;
}

static bool nyc_func_uses_fp_or_simd(const nyr_func_t *func) {
  if (!func)
    return false;
  for (size_t i = 0; i < func->param_count; ++i) {
    if (func->param_types[i] == NYIR_PARAM_F32 ||
        func->param_types[i] == NYIR_PARAM_F64)
      return true;
    if (func->param_types[i] == NYIR_PARAM_AGGREGATE && func->param_abi) {
      uint32_t abi = func->param_abi[i];
      if (NYIR_ARG_AGG_CLASS(abi, 0) == NYIR_ARG_CLASS_SSE ||
          NYIR_ARG_AGG_CLASS(abi, 1) == NYIR_ARG_CLASS_SSE)
        return true;
    }
  }
  if (func->return_aggregate_classes[0] == NYIR_ARG_CLASS_SSE ||
      func->return_aggregate_classes[1] == NYIR_ARG_CLASS_SSE)
    return true;
  for (size_t i = 0; i < func->len; ++i) {
    const nyr_inst_t *inst = &func->data[i];
    nyr_op_t op = inst->op;
    if (inst->scalar_class == NYIR_SCALAR_F32 ||
        inst->scalar_class == NYIR_SCALAR_F64 ||
        (op >= NYIR_CONST_F64 && op <= NYIR_SQRT_F64) ||
        (op >= NYIR_CONST_F32 && op <= NYIR_CMP_F32) ||
        (op >= NYIR_VEC4_LOAD_F64 && op <= NYIR_VEC8_REDUCE_ADD_I64) ||
        op == NYIR_VEC4_REDUCE_ADD_F64 || op == NYIR_VEC4_SET1_I64 ||
        op == NYIR_VEC8_SET1_I64 || op == NYIR_SIN_F64 || op == NYIR_COS_F64)
      return true;
    if (op == NYIR_CALL) {
      if (inst->ret_aggregate_classes[0] == NYIR_ARG_CLASS_SSE ||
          inst->ret_aggregate_classes[1] == NYIR_ARG_CLASS_SSE)
        return true;
      for (size_t arg = 0; arg < NYIR_CALL_MAX_ARGS; ++arg) {
        nyr_scalar_class_t cls = nyr_call_arg_class_get(inst, arg);
        if (cls == NYIR_SCALAR_F32 || cls == NYIR_SCALAR_F64)
          return true;
      }
      for (size_t arg = 0; inst->arg_sizes && arg < (size_t)inst->imm; ++arg) {
        uint32_t abi = inst->arg_sizes[arg];
        if (NYIR_ARG_AGG_CLASS(abi, 0) == NYIR_ARG_CLASS_SSE ||
            NYIR_ARG_AGG_CLASS(abi, 1) == NYIR_ARG_CLASS_SSE)
          return true;
      }
    }
  }
  return false;
}

typedef struct {
  const char *target;
  const char **include_dirs;
  size_t include_count;
  size_t include_capacity;
  const char **forced_includes;
  size_t forced_include_count;
  size_t forced_include_capacity;
  nyc_macro_option_t *macros;
  size_t macro_count;
  size_t macro_capacity;
  nyc_path_map_t *path_maps;
  size_t path_map_count;
  size_t path_map_capacity;
  const char *output;
  const char *dependency_file;
  nyr_opt_level_t optimization;
  bool optimize_size;
  bool compile_unit;
  bool assembly_output;
  bool code16;
  unsigned i386_regparm;
  unsigned i386_stack_alignment;
  bool verbose_assembly;
  bool preprocess;
  bool line_markers;
  bool require_x86;
  bool require_x86_64;
  bool short_wchar;
  bool unsigned_char;
  bool no_builtin;
  bool no_zero_initialized_in_bss;
  bool ms_extensions;
  bool plan9_extensions;
  bool no_std_includes;
  bool threaded;
  bool disable_vectorization;
  bool conserve_stack;
  bool omit_frame_pointer;
  bool position_independent;
  bool retpoline_indirect_calls;
  bool retpoline_inline_calls;
  bool function_return_thunk_extern;
  bool indirect_branch_cs_prefix;
  bool cf_protection_branch;
  unsigned stack_protector; /* 0: off, 1: frame objects, 2: strong, 3: all. */
  bool stack_guard_global;
  bool stack_guard_gs;
  int64_t stack_guard_offset;
  const char *stack_guard_symbol;
  bool dump_macros;
  bool print_file_name_plugin;
} nyc_driver_t;

static nyr_inst_t nyc_guard_inst(nyr_op_t op) {
  return (nyr_inst_t){.op = op,
                      .dst = -1,
                      .a = -1,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .scalar_class = NYIR_SCALAR_I64_RAW};
}

static int nyc_guard_source(nyr_inst_t *data, size_t *len, int *next,
                            const nyc_driver_t *driver, const char *symbol) {
  int address = -1;
  if (driver->stack_guard_global || !symbol) {
    nyr_inst_t in = nyc_guard_inst(driver->stack_guard_global ? NYIR_ADDR_SYMBOL
                                                              : NYIR_CONST_I64);
    address = in.dst = (*next)++;
    in.symbol = driver->stack_guard_global ? symbol : NULL;
    in.imm = driver->stack_guard_global ? 0 : driver->stack_guard_offset;
    data[(*len)++] = in;
  }
  nyr_inst_t load = nyc_guard_inst(
      driver->stack_guard_global ? NYIR_LOAD_I64 : NYIR_INLINE_ASM);
  load.dst = (*next)++;
  load.a = address;
  if (driver->stack_guard_global)
    load.flags = NYIR_INST_F_MEM_VOLATILE;
  else {
    load.imm = driver->stack_guard_gs ? -185 : -184;
    load.symbol = symbol ? symbol : "";
  }
  data[(*len)++] = load;
  return load.dst;
}

static bool nyc_protect_stack(nyr_func_t *func, const nyc_driver_t *driver,
                              char *err, size_t err_len) {
  size_t returns = 0;
  bool eligible = driver->stack_protector == 3;
  int64_t label = -1;
  for (size_t i = 0; i < func->len; ++i) {
    const nyr_inst_t *in = &func->data[i];
    returns += in->op == NYIR_RET;
    eligible |= (in->op == NYIR_ALLOCA &&
                 (driver->stack_protector >= 2 || in->imm >= 8)) ||
                (driver->stack_protector >= 2 && in->op == NYIR_ADDR_LOCAL) ||
                (in->op == NYIR_INLINE_ASM && in->symbol &&
                 strcmp(in->symbol, "__nyc_builtin_alloca_dynamic") == 0);
    if (in->op == NYIR_LABEL && in->imm > label)
      label = in->imm;
  }
  if (!eligible || !returns)
    return true;
  const char *symbol = driver->stack_guard_symbol;
  if (driver->stack_guard_global) {
    if (!symbol)
      symbol = "__nyc_extern___stack_chk_guard";
    else {
      char reference[256];
      int length =
          snprintf(reference, sizeof(reference), "__nyc_extern_%s", symbol);
      if (length < 0 || (size_t)length >= sizeof(reference) ||
          !(symbol = nyr_func_own_symbol_copy(func, reference))) {
        snprintf(err, err_len, "cannot name stack guard object");
        return false;
      }
    }
  }
  if (returns > (SIZE_MAX - 5) / 5 || func->len > SIZE_MAX - 5 - returns * 5 ||
      func->len + 5 + returns * 5 > SIZE_MAX / sizeof(*func->data) ||
      func->next_value > INT_MAX - 2 ||
      returns > (size_t)(INT_MAX - func->next_value - 2) / 4 ||
      label == INT64_MAX) {
    snprintf(err, err_len, "stack protector exceeds native IR limits");
    return false;
  }
  size_t capacity = func->len + 5 + returns * 5, len = 0;
  nyr_inst_t *data = malloc(capacity * sizeof(*data));
  if (!data) {
    snprintf(err, err_len, "out of memory adding stack protection");
    return false;
  }
  ++label;
  int next = func->next_value;
  int guard = nyc_guard_source(data, &len, &next, driver, symbol);
  nyr_inst_t store = nyc_guard_inst(NYIR_STACK_GUARD_STORE);
  store.a = guard;
  data[len++] = store;
  for (size_t i = 0; i < func->len; ++i) {
    if (func->data[i].op == NYIR_RET) {
      /*
       * Read the fixed frame word here, never through a pointer spilled
       * before user code: a buffer overflow could replace that pointer.
       */
      nyr_inst_t saved = nyc_guard_inst(NYIR_STACK_GUARD_LOAD);
      saved.dst = next++;
      data[len++] = saved;
      int current = nyc_guard_source(data, &len, &next, driver, symbol);
      nyr_inst_t compare = nyc_guard_inst(NYIR_CMP_I64);
      compare.dst = next++;
      compare.a = saved.dst;
      compare.b = current;
      compare.cmp = NYIR_CMP_NE;
      data[len++] = compare;
      nyr_inst_t branch = nyc_guard_inst(NYIR_BR_IF);
      branch.a = compare.dst;
      branch.imm = label;
      data[len++] = branch;
    }
    data[len++] = func->data[i];
  }
  nyr_inst_t failure = nyc_guard_inst(NYIR_LABEL);
  failure.imm = label;
  data[len++] = failure;
  failure = nyc_guard_inst(NYIR_CALL);
  failure.symbol = "__stack_chk_fail";
  failure.flags = NYIR_INST_F_EXTERN;
  data[len++] = failure;
  free(func->data);
  func->data = data;
  func->len = len;
  func->cap = capacity;
  func->next_value = next;
  func->has_symbol_calls = true;
  nyr_refresh_metadata(func);
  return nyr_verify(func, err, err_len);
}

static bool add_macro_option(nyc_driver_t *driver, const char *text,
                             bool undefine) {
  if (driver->macro_count == driver->macro_capacity) {
    size_t capacity = driver->macro_capacity ? driver->macro_capacity * 2 : 8;
    if (capacity < driver->macro_capacity ||
        capacity > SIZE_MAX / sizeof(*driver->macros))
      return false;
    nyc_macro_option_t *macros =
        realloc(driver->macros, capacity * sizeof(*macros));
    if (!macros)
      return false;
    driver->macros = macros;
    driver->macro_capacity = capacity;
  }
  driver->macros[driver->macro_count++] =
      (nyc_macro_option_t){.text = text, .undefine = undefine};
  return true;
}

static bool nyc_setup_macro_enabled(const nyc_driver_t *driver) {
  bool enabled = false;
  for (size_t i = 0; i < driver->macro_count; ++i) {
    const nyc_macro_option_t *option = &driver->macros[i];
    const char *text = option->text;
    if (strncmp(text, "_SETUP", 6) != 0 ||
        (text[6] != '\0' && text[6] != '='))
      continue;
    enabled = !option->undefine;
  }
  return enabled;
}

static void usage(FILE *file) {
  ny_help_heading(file, "C compiler (NYC)");
  ny_help_print(file, nyc_compiler_help,
                sizeof(nyc_compiler_help) / sizeof(nyc_compiler_help[0]));
}

static bool nyc_assemble_source(const char *source, const char *output,
                                const char *target,
                                const char *const *include_dirs,
                                size_t include_count) {
#if defined(__unix__) || defined(__APPLE__)
  nyr_target_t host = {0};
  char target_err[256] = {0};
  if (!target) {
    if (!nyr_host_target(&host, target_err, sizeof(target_err))) {
      fprintf(stderr, "%s\n",
              target_err[0] ? target_err : "cannot determine host target");
      return false;
    }
    target = host.target;
  }
  nyc_abi_t abi = nyc_abi_for_target(target);
  if (abi != NYC_ABI_SYSV_X86_64) {
    fprintf(stderr, "nyc: assembly input currently requires x86-64 ELF\n");
    return false;
  }
  if (include_count > (SIZE_MAX - 5) / 2) {
    fputs("nyc: too many assembler include directories\n", stderr);
    return false;
  }
  size_t arg_count = 5 + include_count * 2;
  char **args = calloc(arg_count + 1, sizeof(*args));
  if (!args) {
    fputs("nyc: out of memory preparing assembler command\n", stderr);
    return false;
  }
  size_t at = 0;
  args[at++] = "as";
  args[at++] = "--64";
  for (size_t i = 0; i < include_count; ++i) {
    args[at++] = "-I";
    args[at++] = (char *)include_dirs[i];
  }
  args[at++] = "-o";
  args[at++] = (char *)output;
  args[at++] = (char *)source;
  args[at] = NULL;

  pid_t child = fork();
  if (child == 0) {
    execvp(args[0], args);
    perror("nyc: as");
    _exit(127);
  }
  if (child < 0) {
    perror("nyc: cannot start assembler");
    free(args);
    return false;
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  free(args);
  if (waited < 0) {
    perror("nyc: cannot wait for assembler");
    return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
  (void)source;
  (void)output;
  (void)target;
  (void)include_dirs;
  (void)include_count;
  fputs("nyc: assembly input is unsupported on this platform\n", stderr);
  return false;
#endif
}

static bool append_quoted_include(char *buffer, size_t capacity, size_t *used,
                                  const char *path) {
  static const char prefix[] = "#include \"";
  size_t path_len = strlen(path);
  if (path_len > (SIZE_MAX - sizeof(prefix) - 2) / 2)
    return false;
  size_t needed = sizeof(prefix) - 1 + path_len + 2;
  for (size_t i = 0; i < path_len; ++i)
    if (path[i] == '\\' || path[i] == '"')
      ++needed;
  if (*used > capacity || needed > capacity - *used)
    return false;
  memcpy(buffer + *used, prefix, sizeof(prefix) - 1);
  *used += sizeof(prefix) - 1;
  for (size_t i = 0; i < path_len; ++i) {
    if (path[i] == '\\' || path[i] == '"')
      buffer[(*used)++] = '\\';
    buffer[(*used)++] = path[i];
  }
  buffer[(*used)++] = '"';
  buffer[(*used)++] = '\n';
  return true;
}

static bool add_forced_include(nyc_driver_t *driver, const char *path) {
  if (driver->forced_include_count == driver->forced_include_capacity) {
    size_t capacity = driver->forced_include_capacity
                          ? driver->forced_include_capacity * 2
                          : 4;
    if (capacity < driver->forced_include_capacity ||
        capacity > SIZE_MAX / sizeof(*driver->forced_includes))
      return false;
    const char **includes =
        realloc(driver->forced_includes, capacity * sizeof(*includes));
    if (!includes)
      return false;
    driver->forced_includes = includes;
    driver->forced_include_capacity = capacity;
  }
  driver->forced_includes[driver->forced_include_count++] = path;
  return true;
}

static bool add_path_map(nyc_driver_t *driver, const char *argument) {
  const char *separator = strchr(argument, '=');
  if (!separator || separator == argument)
    return false;
  size_t from_len = (size_t)(separator - argument);
  char *from = strndup(argument, from_len);
  char *to = strdup(separator + 1);
  if (!from || !to) {
    free(from);
    free(to);
    return false;
  }
  if (driver->path_map_count == driver->path_map_capacity) {
    size_t capacity =
        driver->path_map_capacity ? driver->path_map_capacity * 2 : 4;
    if (capacity < driver->path_map_capacity ||
        capacity > SIZE_MAX / sizeof(*driver->path_maps)) {
      free(from);
      free(to);
      return false;
    }
    nyc_path_map_t *maps = realloc(driver->path_maps, capacity * sizeof(*maps));
    if (!maps) {
      free(from);
      free(to);
      return false;
    }
    driver->path_maps = maps;
    driver->path_map_capacity = capacity;
  }
  driver->path_maps[driver->path_map_count++] =
      (nyc_path_map_t){.from = from, .to = to};
  return true;
}

static bool supported_by_default_flag(const char *option) {
  /*
   * Each listed switch requests behavior already guaranteed by the current
   * frontend/backend defaults (for example, no unwind/stack-protector
   * metadata, and one shared text/data section).
   * Options that need a distinct code-generation mode must be handled in
   * main() instead of being added here.
   */
  static const char *const flags[] = {
      "-std=gnu11",
      "-ffreestanding",
      "-fno-common",
      "-fno-PIE",
      "-fno-strict-aliasing",
      "-fno-strict-overflow",
      "-fno-asynchronous-unwind-tables",
      "-fno-delete-null-pointer-checks",
      "-fno-stack-protector",
      "-fno-builtin-wcslen",
      "-fno-function-sections",
      "-fno-data-sections",
      "-fno-jump-tables",
      "-fno-gcse",
      "-mtune=generic",
      "-mno-red-zone",
      "-fno-stack-check",
      "-mcmodel=small",
  };
  for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
    if (strcmp(option, flags[i]) == 0)
      return true;
  /*
   * Warning controls are accepted for GCC driver compatibility, but NYC
   * currently does not implement warning diagnostics.
   */
  return strncmp(option, "-W", 2) == 0;
}

static bool write_make_path(FILE *file, const char *path, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    char c = path[i];
    if (c == '$') {
      if (fputs("$$", file) == EOF)
        return false;
    } else {
      if ((c == ' ' || c == '#' || c == ':' || c == '\\') &&
          fputc('\\', file) == EOF)
        return false;
      if (fputc(c, file) == EOF)
        return false;
    }
  }
  return true;
}

typedef struct {
  FILE *file;
  const char *main_path;
  bool first;
} nyc_dependency_output_t;

static int write_dependency_path(const char *path, void *context) {
  nyc_dependency_output_t *output = context;
  if (output->main_path && strcmp(path, output->main_path) == 0)
    return 1;
  if ((!output->first && fputs(" \\\n  ", output->file) == EOF) ||
      !write_make_path(output->file, path, strlen(path)))
    return 0;
  output->first = false;
  return 1;
}

static bool write_dependency_file(const nyc_source_t *source, nyc_abi_t abi,
                                  const char *target, const char *path,
                                  char *err, size_t err_len) {
  FILE *dependencies = fopen(path, "wb");
  if (!dependencies) {
    snprintf(err, err_len, "cannot open dependency output '%s'", path);
    return false;
  }
  bool ok = write_make_path(dependencies, target, strlen(target)) &&
            fputs(": ", dependencies) != EOF;
  const char *main_path = source->base_path ? source->base_path : source->path;
  if (main_path && main_path[0] == '<')
    main_path = NULL;
  if (ok && main_path)
    ok = write_make_path(dependencies, main_path, strlen(main_path));
  nyc_dependency_output_t output = {
      .file = dependencies, .main_path = main_path, .first = !main_path};
  if (ok)
    ok = nyc_frontend_visit_dependencies(source, abi, write_dependency_path,
                                         &output, err, err_len);
  if (ok && fputs("\n", dependencies) == EOF)
    ok = false;
  if (dependencies && fclose(dependencies) != 0)
    ok = false;
  if (!ok && !err[0])
    snprintf(err, err_len, "cannot write dependency output '%s'", path);
  return ok;
}

static bool write_assembly_quoted_string(nyr_writer_t *writer,
                                         const char *text) {
  if (!nyr_put(writer, "\""))
    return false;
  for (const unsigned char *at = (const unsigned char *)text; *at; ++at) {
    if (*at == '\\' || *at == '"') {
      if (!nyr_printf(writer, "\\%c", *at))
        return false;
    } else if (*at < 0x20 || *at >= 0x7f) {
      if (!nyr_printf(writer, "\\%03o", *at))
        return false;
    } else {
      if (!nyr_printf(writer, "%c", *at))
        return false;
    }
  }
  return nyr_put(writer, "\"");
}

static bool write_assembly_data_bytes(nyr_writer_t *writer,
                                      const unsigned char *bytes,
                                      size_t length) {
  for (size_t offset = 0; offset < length;) {
    size_t count = length - offset;
    if (count > 16)
      count = 16;
    if (!nyr_put(writer, "\t.byte "))
      return false;
    for (size_t i = 0; i < count; ++i) {
      if ((i && !nyr_put(writer, ",")) ||
          !nyr_printf(writer, "0x%02x", bytes[offset + i]))
        return false;
    }
    if (!nyr_put(writer, "\n"))
      return false;
    offset += count;
  }
  return true;
}

typedef struct {
  size_t function;
  bool eligible;
  uint64_t seen;
  uint64_t blocked;
  uint64_t rejected;
  uint64_t applied;
  int64_t values[64];
} nyc_parameter_facts_t;

static uint64_t nyc_symbol_hash(const char *symbol, size_t length) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < length; ++i)
    hash = (hash ^ (unsigned char)symbol[i]) * UINT64_C(1099511628211);
  return hash;
}

static nyc_parameter_facts_t *
nyc_parameter_symbol(const nyc_unit_t *unit, nyc_parameter_facts_t *facts,
                     size_t mask, const char *symbol, size_t length) {
  if (!symbol || !length)
    return NULL;
  static const char prefix[] = "__nyc_extern_";
  if (length >= sizeof(prefix) - 1 &&
      memcmp(symbol, prefix, sizeof(prefix) - 1) == 0) {
    symbol += sizeof(prefix) - 1;
    length -= sizeof(prefix) - 1;
  }
  size_t slot = (size_t)nyc_symbol_hash(symbol, length) & mask;
  while (facts[slot].function) {
    const char *name = unit->names[facts[slot].function - 1];
    if (strlen(name) == length && memcmp(name, symbol, length) == 0)
      return &facts[slot];
    slot = (slot + 1) & mask;
  }
  return NULL;
}

static void nyc_parameter_asm_escapes(const nyc_unit_t *unit,
                                      nyc_parameter_facts_t *facts, size_t mask,
                                      const char *assembly) {
  if (!assembly)
    return;
  for (const char *at = assembly; *at;) {
    if (!(isalnum((unsigned char)*at) || *at == '_' || *at == '.' ||
          *at == '$')) {
      ++at;
      continue;
    }
    const char *begin = at++;
    while (isalnum((unsigned char)*at) || *at == '_' || *at == '.' ||
           *at == '$')
      ++at;
    nyc_parameter_facts_t *entry =
        nyc_parameter_symbol(unit, facts, mask, begin, (size_t)(at - begin));
    if (entry)
      entry->eligible = false;
  }
}

/* Keep signatures and calls intact. Only private, non-address-taken scalar
 * formals with the same constant at every direct call receive entry facts. */
static bool nyc_specialize_private_parameters(nyc_unit_t *unit,
                                              const nyc_driver_t *driver,
                                              char *err, size_t err_len) {
  if (!unit->count || unit->count > SIZE_MAX / 2)
    return unit->count == 0;
  size_t capacity = 16;
  while (capacity < unit->count * 2) {
    if (capacity > SIZE_MAX / 2)
      return false;
    capacity *= 2;
  }
  nyc_parameter_facts_t *facts = calloc(capacity, sizeof(*facts));
  if (!facts)
    return false;
  size_t mask = capacity - 1;
  for (size_t f = 0; f < unit->count; ++f) {
    const char *name = unit->names[f];
    size_t slot = (size_t)nyc_symbol_hash(name, strlen(name)) & mask;
    while (facts[slot].function)
      slot = (slot + 1) & mask;
    facts[slot].function = f + 1;
    const nyr_func_t *func = &unit->funcs[f];
    facts[slot].eligible = unit->function_discardable[f] && func->param_count &&
                           func->param_count <= 64 &&
                           !func->return_aggregate_size &&
                           !nyc_func_uses_va_list(func);
    for (size_t p = 0; p < func->param_count && facts[slot].eligible; ++p)
      if (!func->param_types || func->param_types[p] != NYIR_PARAM_I64)
        facts[slot].blocked |= UINT64_C(1) << p;
    for (size_t i = 0; facts[slot].eligible && i < func->len; ++i)
      if (func->data[i].op == NYIR_ADDR_LOCAL && func->data[i].imm >= 0 &&
          (uint64_t)func->data[i].imm < func->param_count)
        facts[slot].blocked |= UINT64_C(1) << func->data[i].imm;
  }
  for (size_t d = 0; d < unit->data_count; ++d)
    for (size_t r = 0; r < unit->data[d].reloc_count; ++r) {
      const char *symbol = unit->data[d].relocs[r].symbol;
      nyc_parameter_facts_t *entry = nyc_parameter_symbol(
          unit, facts, mask, symbol, symbol ? strlen(symbol) : 0);
      if (entry)
        entry->eligible = false;
    }
  for (size_t a = 0; a < unit->alias_count; ++a) {
    const char *symbol = unit->aliases[a].target;
    nyc_parameter_facts_t *entry = nyc_parameter_symbol(
        unit, facts, mask, symbol, symbol ? strlen(symbol) : 0);
    if (entry)
      entry->eligible = false;
  }
  nyc_parameter_asm_escapes(unit, facts, mask, unit->raw_file_scope_asm);
  bool changed = true, ok = true;
  while (ok && changed) {
    changed = false;
    for (size_t s = 0; s < capacity; ++s) {
      facts[s].seen = 0;
      facts[s].rejected = facts[s].blocked;
    }
    for (size_t f = 0; ok && f < unit->count; ++f) {
      const nyr_func_t *func = &unit->funcs[f];
      if (func->next_value <= 0)
        continue;
      size_t values = (size_t)func->next_value;
      if (values > SIZE_MAX / (sizeof(int64_t) + 1)) {
        ok = false;
        break;
      }
      int64_t *constants = calloc(values, sizeof(*constants) + 1);
      if (!constants) {
        ok = false;
        break;
      }
      unsigned char *known = (unsigned char *)(constants + values);
      for (size_t i = 0; i < func->len; ++i) {
        const nyr_inst_t *in = &func->data[i];
        if (in->dst >= 0 && (size_t)in->dst < values) {
          if (in->op == NYIR_CONST_I64) {
            constants[in->dst] = in->imm;
            known[in->dst] = 1;
          } else if (in->op == NYIR_COPY && in->a >= 0 &&
                     (size_t)in->a < values && known[in->a]) {
            constants[in->dst] = constants[in->a];
            known[in->dst] = 1;
          }
        }
        if (in->op == NYIR_INLINE_ASM) {
          nyc_parameter_asm_escapes(unit, facts, mask, in->symbol);
          continue;
        }
        if (!in->symbol)
          continue;
        nyc_parameter_facts_t *entry = nyc_parameter_symbol(
            unit, facts, mask, in->symbol, strlen(in->symbol));
        if (!entry || !entry->eligible)
          continue;
        if (in->op == NYIR_ADDR_SYMBOL) {
          entry->eligible = false;
          continue;
        }
        if (in->op != NYIR_CALL)
          continue;
        const nyr_func_t *callee = &unit->funcs[entry->function - 1];
        int args[NYIR_CALL_MAX_ARGS], argc = 0;
        if (in->imm != (int64_t)callee->param_count ||
            !nyr_call_args(in, func->next_value, args, NYIR_CALL_MAX_ARGS,
                           &argc, NULL, 0)) {
          entry->eligible = false;
          continue;
        }
        for (int p = 0; p < argc; ++p) {
          uint64_t bit = UINT64_C(1) << p;
          int arg = args[p];
          if (arg < 0 || (size_t)arg >= values || !known[arg] ||
              ((entry->seen & bit) && entry->values[p] != constants[arg]))
            entry->rejected |= bit;
          else
            entry->values[p] = constants[arg];
          entry->seen |= bit;
        }
      }
      free(constants);
    }
    for (size_t s = 0; ok && s < capacity; ++s) {
      nyc_parameter_facts_t *entry = &facts[s];
      uint64_t apply = entry->seen & ~entry->rejected & ~entry->applied;
      if (!entry->eligible || !apply)
        continue;
      nyr_func_t *func = &unit->funcs[entry->function - 1];
      size_t count = (size_t)__builtin_popcountll(apply);
      if (count > (size_t)(INT_MAX - func->next_value) ||
          func->len > SIZE_MAX / sizeof(*func->data) - 2 * count) {
        ok = false;
        break;
      }
      nyr_inst_t *data = calloc(func->len + 2 * count, sizeof(*data));
      if (!data) {
        ok = false;
        break;
      }
      size_t at = 0;
      for (size_t p = 0; p < func->param_count; ++p) {
        if (!(apply & (UINT64_C(1) << p)))
          continue;
        int value = func->next_value++;
        nyr_inst_t constant = nyc_guard_inst(NYIR_CONST_I64);
        constant.dst = value;
        constant.imm = entry->values[p];
        data[at++] = constant;
        nyr_inst_t store = nyc_guard_inst(NYIR_STORE_LOCAL);
        store.a = value;
        store.imm = (int64_t)p;
        data[at++] = store;
      }
      memcpy(data + at, func->data, func->len * sizeof(*data));
      free(func->data);
      func->data = data;
      func->len += at;
      func->cap = func->len;
      entry->applied |= apply;
      changed = true;
      nyr_refresh_metadata(func);
      ok = nyr_optimize_with_options(func, driver->optimization,
                                     driver->disable_vectorization
                                         ? NYR_OPT_DISABLE_VECTORIZATION
                                         : 0,
                                     err, err_len) &&
           nyr_verify(func, err, err_len);
    }
  }
  free(facts);
  if (!ok && !err[0])
    snprintf(err, err_len, "failed specializing private C parameter constants");
  return ok;
}

static bool write_assembly_data(nyr_writer_t *writer, const nyc_data_t *data,
                                const ny_native_target_info_t *target,
                                char *err, size_t err_len) {
  if (!data->name || !data->name[0] || (!data->bytes && data->len)) {
    snprintf(err, err_len, "NYC assembly output has invalid file-scope data");
    return false;
  }
  if (data->section) {
    bool zero_fill = strcmp(data->section, ".bss") == 0 ||
                     strncmp(data->section, ".bss.", 5) == 0;
    bool note = strncmp(data->section, ".note.", 6) == 0;
    if (zero_fill) {
      bool nonzero = data->reloc_count != 0;
      for (size_t i = 0; i < data->len && !nonzero; ++i)
        nonzero = data->bytes[i] != 0;
      if (nonzero) {
        snprintf(err, err_len, "nonzero initializer in BSS section `%s`",
                 data->section);
        return false;
      }
    }
    uint64_t flags = data->section_flags ? data->section_flags : 3;
    if (flags & ~(UINT64_C(1) | UINT64_C(2) | UINT64_C(4) | UINT64_C(0x10) |
                  UINT64_C(0x20) | UINT64_C(0x400))) {
      snprintf(err, err_len,
               "NYC assembly output does not support data-section flags "
               "0x%llx for `%s`",
               (unsigned long long)flags, data->section);
      return false;
    }
    if ((flags & 0x10) && !data->section_entsize) {
      snprintf(err, err_len,
               "NYC assembly output requires an entity size for merged "
               "section `%s`",
               data->section);
      return false;
    }
    char flag_text[8] = {0};
    size_t flag_len = 0;
    if (flags & 2)
      flag_text[flag_len++] = 'a';
    if (flags & 1)
      flag_text[flag_len++] = 'w';
    if (flags & 4)
      flag_text[flag_len++] = 'x';
    if (flags & 0x10)
      flag_text[flag_len++] = 'M';
    if (flags & 0x20)
      flag_text[flag_len++] = 'S';
    if (flags & 0x400)
      flag_text[flag_len++] = 'T';
    if (!nyr_put(writer, "\t.section ") ||
        !write_assembly_quoted_string(writer, data->section) ||
        !nyr_printf(writer, ",\"%s\",@%s", flag_text,
                    zero_fill ? "nobits"
                    : note    ? "note"
                              : "progbits") ||
        ((flags & 0x10) &&
         !nyr_printf(writer, ",%zu", data->section_entsize)) ||
        !nyr_put(writer, "\n"))
      return false;
  } else if (!nyr_put(writer, "\t.data\n")) {
    return false;
  }
  size_t alignment = data->alignment ? data->alignment : 1;
  if ((alignment & (alignment - 1)) || alignment > ((size_t)1 << 30)) {
    snprintf(err, err_len,
             "NYC assembly output has invalid data alignment %zu for `%s`",
             alignment, data->name ? data->name : "<unnamed>");
    return false;
  }
  if (!nyr_printf(writer, "\t.balign %zu\n", alignment))
    return false;
  if (data->is_local) {
    if (!nyr_printf(writer, "\t.local %s%s\n", target->symbol_prefix,
                    data->name))
      return false;
  } else if (data->is_weak) {
    if (!nyr_printf(writer, "\t.weak %s%s\n", target->symbol_prefix,
                    data->name))
      return false;
  } else if (!nyr_printf(writer, "\t.globl %s%s\n", target->symbol_prefix,
                         data->name)) {
    return false;
  }
  if (!nyr_printf(writer, "\t.type %s%s,%s\n%s%s:\n", target->symbol_prefix,
                  data->name, data->function_symbol ? "@function" : "@object",
                  target->symbol_prefix, data->name))
    return false;
  size_t cursor = 0;
  for (size_t emitted = 0; emitted < data->reloc_count; ++emitted) {
    const nyc_data_reloc_t *reloc = NULL;
    for (size_t i = 0; i < data->reloc_count; ++i) {
      if (data->relocs[i].offset >= cursor &&
          (!reloc || data->relocs[i].offset < reloc->offset))
        reloc = &data->relocs[i];
    }
    if (!reloc) {
      snprintf(err, err_len,
               "NYC assembly output has overlapping data relocations in `%s`",
               data->name);
      return false;
    }
    size_t width = reloc->width ? reloc->width : sizeof(uint64_t);
    bool absolute32 = reloc->type == NYC_DATA_RELOC_ABS32;
    bool absolute32s = reloc->type == NYC_DATA_RELOC_ABS32S;
    if (!reloc->symbol || !reloc->symbol[0] || reloc->offset > data->len ||
        width > data->len - reloc->offset ||
        (reloc->type == NYC_DATA_RELOC_PC32 && width != sizeof(uint32_t)) ||
        ((absolute32 || absolute32s) && width != sizeof(uint32_t)) ||
        (reloc->type != NYC_DATA_RELOC_PC32 &&
         reloc->type != NYC_DATA_RELOC_ABS64 && !absolute32 && !absolute32s)) {
      snprintf(err, err_len,
               "NYC assembly output has invalid data relocation in `%s` "
               "(type=%u width=%zu offset=%zu symbol=%s)",
               data->name, (unsigned)reloc->type, width, reloc->offset,
               reloc->symbol ? reloc->symbol : "(null)");
      return false;
    }
    if (!write_assembly_data_bytes(writer, data->bytes + cursor,
                                   reloc->offset - cursor))
      return false;
    if (reloc->type == NYC_DATA_RELOC_PC32) {
      if (!nyr_printf(writer, "\t.long %s", reloc->symbol))
        return false;
      if (reloc->addend && !nyr_printf(writer, "%+lld", reloc->addend))
        return false;
      if (!nyr_put(writer, "-.\n"))
        return false;
    } else if (absolute32s) {
      if (!nyr_printf(writer, "\t.reloc ., R_X86_64_32S, %s", reloc->symbol))
        return false;
      if (reloc->addend && !nyr_printf(writer, "%+lld", reloc->addend))
        return false;
      if (!nyr_put(writer, "\n\t.long 0\n"))
        return false;
    } else if (absolute32) {
      if (!nyr_printf(writer, "\t.long %s", reloc->symbol))
        return false;
      if (reloc->addend && !nyr_printf(writer, "%+lld", reloc->addend))
        return false;
      if (!nyr_put(writer, "\n"))
        return false;
    } else {
      if (!nyr_printf(writer, "\t.quad %s", reloc->symbol))
        return false;
      if (reloc->addend && !nyr_printf(writer, "%+lld", reloc->addend))
        return false;
      if (!nyr_put(writer, "\n"))
        return false;
    }
    cursor = reloc->offset + width;
  }
  if (!write_assembly_data_bytes(writer, data->bytes + cursor,
                                 data->len - cursor) ||
      !nyr_printf(writer, "\t.size %s%s,%zu\n", target->symbol_prefix,
                  data->name, data->len))
    return false;
  if (data->function_symbol)
    return true;
  if (data->section && strncmp(data->section, ".export_symbol", 14) == 0)
    return true;
  return nyr_printf(writer,
                    "\t.local %s__nyc_data_%s\n"
                    "\t.set %s__nyc_data_%s,%s%s\n",
                    target->symbol_prefix, data->name, target->symbol_prefix,
                    data->name, target->symbol_prefix, data->name);
}

static bool
write_c_function_assembly(nyr_writer_t *assembly, const char *function_assembly,
                          const char *name, const nyr_func_t *function,
                          bool cf_protection_branch, bool is_x86_64) {
  if (!cf_protection_branch)
    return nyr_put(assembly, function_assembly);
  size_t name_len = strlen(name);
  size_t addressed_count = 0, capacity = 1;
  int64_t max_label = -1;
  for (size_t i = 0; i < function->len; ++i)
    if (function->data[i].op == NYIR_LABEL &&
        (function->data[i].flags & NYIR_INST_F_LABEL_ADDRESS_TAKEN)) {
      ++addressed_count;
      if (function->data[i].imm > max_label)
        max_label = function->data[i].imm;
    }
  if (addressed_count > SIZE_MAX / 2)
    return false;
  while (capacity < addressed_count * 2) {
    if (capacity > SIZE_MAX / 2)
      return false;
    capacity *= 2;
  }
  uint64_t *addressed =
      addressed_count ? calloc(capacity, sizeof(*addressed)) : NULL;
  if (addressed_count && !addressed)
    return false;
  for (size_t i = 0; i < function->len && addressed; ++i)
    if (function->data[i].op == NYIR_LABEL &&
        (function->data[i].flags & NYIR_INST_F_LABEL_ADDRESS_TAKEN) &&
        function->data[i].imm >= 0) {
      uint64_t id = (uint64_t)function->data[i].imm;
      size_t slot = (size_t)id & (capacity - 1);
      while (addressed[slot] && addressed[slot] != id + 1)
        slot = (slot + 1) & (capacity - 1);
      addressed[slot] = id + 1;
    }
  bool found_entry = false, ok = true;
  const char *copied = function_assembly;
  const char *endbr = is_x86_64 ? "\tendbr64\n" : "\tendbr32\n";
  for (const char *line = function_assembly; *line;) {
    const char *end = strchr(line, '\n');
    if (!end)
      break;
    size_t line_len = (size_t)(end - line);
    bool landing = line_len == name_len + 1 &&
                   memcmp(line, name, name_len) == 0 && line[name_len] == ':';
    found_entry |= landing;
    if (!landing && addressed && line_len > name_len + 10 &&
        memcmp(line, ".Lnyir_", 7) == 0 &&
        memcmp(line + 7, name, name_len) == 0 &&
        memcmp(line + 7 + name_len, "_L", 2) == 0) {
      const char *digits = line + 9 + name_len;
      char *tail = NULL;
      uint64_t id = strtoull(digits, &tail, 10);
      if (*digits >= '0' && *digits <= '9' && tail == end - 1 && *tail == ':' &&
          max_label >= 0 && id <= (uint64_t)max_label) {
        size_t slot = (size_t)id & (capacity - 1);
        while (addressed[slot] && addressed[slot] != id + 1)
          slot = (slot + 1) & (capacity - 1);
        landing = addressed[slot] != 0;
      }
    }
    if (landing) {
      size_t count = (size_t)(end + 1 - copied);
      if (count > INT_MAX ||
          !nyr_printf(assembly, "%.*s%s", (int)count, copied, endbr)) {
        ok = false;
        break;
      }
      copied = end + 1;
    }
    line = end + 1;
  }
  if (ok && found_entry)
    ok = nyr_put(assembly, copied);
  free(addressed);
  return ok && found_entry;
}

static bool write_c_unit_assembly(
    const nyc_unit_t *unit, const ny_native_target_info_t *target,
    const char *path, bool verbose, bool code16, bool code16_low_stack,
    bool position_independent,
    unsigned i386_stack_alignment,
    bool retpoline_indirect_calls,
    bool retpoline_inline_calls, bool function_return_thunk_extern,
    bool cf_protection_branch, char *err, size_t err_len) {
  nyr_writer_t assembly = {0};
  bool ok = !code16 || nyr_put(&assembly, "\t.code16gcc\n");
  if (ok && unit->raw_file_scope_asm)
    ok = nyr_put(&assembly, unit->raw_file_scope_asm);
  for (size_t i = 0; ok && i < unit->data_count; ++i) {
    ok = write_assembly_data(&assembly, &unit->data[i], target, err, err_len);
  }
  for (size_t i = 0; ok && i < unit->count; ++i) {
    if (unit->function_no_emit && unit->function_no_emit[i])
      continue;
    bool is_local = unit->function_local && unit->function_local[i];
    bool is_inline = unit->function_inline && unit->function_inline[i];
    if (is_local) {
      bool has_jump_label_asm = false;
      bool has_unspecialized_alt_wrmsr = false;
      for (size_t inst = 0; inst < unit->funcs[i].len; ++inst)
        if (unit->funcs[i].data[inst].op == NYIR_INLINE_ASM) {
          has_jump_label_asm |= unit->funcs[i].data[inst].imm == -14;
          has_unspecialized_alt_wrmsr |= unit->funcs[i].data[inst].imm == -100;
        }
      if ((is_inline && has_jump_label_asm) || has_unspecialized_alt_wrmsr) {
        bool referenced = false;
        for (size_t caller = 0; caller < unit->count && !referenced; ++caller)
          for (size_t inst = 0; inst < unit->funcs[caller].len; ++inst) {
            const nyr_inst_t *call = &unit->funcs[caller].data[inst];
            referenced |=
                (call->op == NYIR_CALL || call->op == NYIR_ADDR_SYMBOL) &&
                call->symbol && strcmp(call->symbol, unit->names[i]) == 0;
          }
        /*
         * A data initializer (`static const struct pv_irq_ops ops = {
         * .halt = native_halt }`) requires the out-of-line copy exactly
         * like a call does; dropping it leaves the table with an
         * undefined reference.
         */
        for (size_t data = 0; data < unit->data_count && !referenced; ++data)
          for (size_t reloc = 0; reloc < unit->data[data].reloc_count;
               ++reloc) {
            const char *symbol = unit->data[data].relocs[reloc].symbol;
            referenced |= symbol && strcmp(symbol, unit->names[i]) == 0;
          }
        if (!referenced)
          continue;
      }
    }
    const char *section =
        unit->function_sections ? unit->function_sections[i] : NULL;
    if (section) {
      ok = nyr_put(&assembly, "\t.section ") &&
           write_assembly_quoted_string(&assembly, section) &&
           nyr_put(&assembly, ",\"ax\",@progbits\n");
    } else {
      ok = nyr_put(&assembly, "\t.text\n");
    }
    if (verbose)
      ok = ok && nyr_printf(&assembly, "# NYC verbose assembly: %s\n",
                            unit->names[i]);
    if (ok) {
      bool is_local = unit->function_local && unit->function_local[i];
      if (target->target == NY_NATIVE_TARGET_X86_64) {
        bool emitted_machine = false;
        bool has_memory_access = false;
        for (size_t inst = 0; inst < unit->funcs[i].len; ++inst)
          has_memory_access |=
              unit->funcs[i].data[inst].op == NYIR_ADDR_LOCAL ||
              unit->funcs[i].data[inst].op == NYIR_LOAD_I64 ||
              unit->funcs[i].data[inst].op == NYIR_STORE_I64 ||
              unit->funcs[i].data[inst].op == NYIR_ALLOCA ||
              unit->funcs[i].data[inst].op == NYIR_COPY_STRUCT;
        if (strcmp(target->object_format, "elf") == 0 &&
            target->abi == NY_NATIVE_ABI_SYSV &&
            (!has_memory_access || !getenv("NYC_DISABLE_MACH_MEM_ASM")) &&
            !retpoline_indirect_calls && !retpoline_inline_calls) {
          nyr_mach_func_t machine = {0};
          nyr_writer_t native = {0};
          char machine_err[256] = {0};
          bool lowered =
              nyr_mach_lower_nir(&unit->funcs[i], &machine, target->caps,
                                 machine_err, sizeof(machine_err));
          if (lowered && nyr_x86_64_emit_mach_scalar(
                             &native, target, &machine, unit->names[i], false,
                             function_return_thunk_extern, machine_err,
                             sizeof(machine_err))) {
            if (is_local && native.data) {
              char *global = strstr(native.data, "\t.globl\t");
              if (global)
                memcpy(global + 1, ".local", sizeof(".local") - 1);
            }
            ok = write_c_function_assembly(
                &assembly, native.data ? native.data : "", unit->names[i],
                &unit->funcs[i], cf_protection_branch,
                target->target == NY_NATIVE_TARGET_X86_64);
            emitted_machine = ok;
          }
          if (!emitted_machine && getenv("NYC_TRACE_MACHINE_FALLBACK"))
            fprintf(stderr, "NYC machine fallback: %s: %s\n", unit->names[i],
                    machine_err[0] ? machine_err
                                   : "emitter rejected machine IR");
          free(native.data);
          nyr_mach_func_free(&machine);
        }
        if (ok && !emitted_machine) {
          nyr_writer_t function_assembly = {0};
          ok = nyr_x86_64_emit_ir_with_options(
              &function_assembly, target, &unit->funcs[i], unit->names[i],
              false, is_local, retpoline_indirect_calls, retpoline_inline_calls,
              function_return_thunk_extern, err, err_len);
          if (ok)
            ok = write_c_function_assembly(
                &assembly, function_assembly.data ? function_assembly.data : "",
                unit->names[i], &unit->funcs[i], cf_protection_branch,
                target->target == NY_NATIVE_TARGET_X86_64);
          free(function_assembly.data);
        }
        if (!ok && err && err_len && !err[0])
          snprintf(err, err_len,
                   "x86-64 assembly emission failed for C function %s",
                   unit->names[i]);
      } else {
        nyr_writer_t function_assembly = {0};
        ok = nyr_i386_emit_ir_with_linkage(&function_assembly, target,
                                           &unit->funcs[i], unit->names[i],
                                           false, is_local, position_independent,
                                           i386_stack_alignment,
                                           code16_low_stack,
                                           err, err_len);
        if (ok)
          ok = write_c_function_assembly(
              &assembly, function_assembly.data ? function_assembly.data : "",
              unit->names[i], &unit->funcs[i], cf_protection_branch, false);
        free(function_assembly.data);
        if (!ok && err && err_len && !err[0])
          snprintf(err, err_len,
                   "i386 assembly emission failed for C function %s",
                   unit->names[i]);
      }
    }
    if (ok && unit->function_weak && unit->function_weak[i])
      ok = nyr_printf(&assembly, "\t.weak %s%s\n", target->symbol_prefix,
                      unit->names[i]);
  }
  if (ok && target->target == NY_NATIVE_TARGET_X86_64) {
    const char *r = function_return_thunk_extern ? "\tjmp\t__x86_return_thunk\n"
                                                 : "\tret\n";
    const char *endbr = cf_protection_branch ? "\tendbr64\n" : "";
    char helpers[1024];
    snprintf(helpers, sizeof(helpers),
             "\t.text\n"
             "\t.weak __floatundidf\n\t.type "
             "__floatundidf,@function\n__floatundidf:\n%s"
             "\ttestq %%rdi, %%rdi\n\tjs 1f\n\tcvtsi2sdq %%rdi, %%xmm0\n%s"
             "1:\n\tmovq %%rdi, %%rax\n\tmovq %%rdi, %%rcx\n\tandq $1, "
             "%%rcx\n\tshrq $1, %%rax\n\torq %%rcx, %%rax\n\tcvtsi2sdq %%rax, "
             "%%xmm0\n\taddsd %%xmm0, %%xmm0\n%s"
             "\t.size __floatundidf, .-__floatundidf\n"
             "\t.weak __floatundisf\n\t.type "
             "__floatundisf,@function\n__floatundisf:\n%s"
             "\ttestq %%rdi, %%rdi\n\tjs 2f\n\tcvtsi2ssq %%rdi, %%xmm0\n%s"
             "2:\n\tmovq %%rdi, %%rax\n\tmovq %%rdi, %%rcx\n\tandq $1, "
             "%%rcx\n\tshrq $1, %%rax\n\torq %%rcx, %%rax\n\tcvtsi2ssq %%rax, "
             "%%xmm0\n\taddss %%xmm0, %%xmm0\n%s"
             "\t.size __floatundisf, .-__floatundisf\n",
             endbr, r, r, endbr, r, r);
    ok = nyr_put(&assembly, helpers);
  }
  for (size_t i = 0; ok && i < unit->alias_count; ++i) {
    const nyc_function_alias_t *alias = &unit->aliases[i];
    const char *binding = alias->is_weak ? ".weak" : ".globl";
    if (alias->target_local) {
      ok = nyr_printf(&assembly,
                      "\t%s %s%s\n"
                      "\t.type %s%s,@function\n"
                      "\t.set %s%s,%s\n"
                      "\t.globl %s__nyc_extern_%s\n"
                      "\t.set %s__nyc_extern_%s,%s\n"
                      "\t.globl %sny_fn_%s\n"
                      "\t.set %sny_fn_%s,%s\n",
                      binding, target->symbol_prefix, alias->name,
                      target->symbol_prefix, alias->name, target->symbol_prefix,
                      alias->name, alias->target, target->symbol_prefix,
                      alias->name, target->symbol_prefix, alias->name,
                      alias->target, target->symbol_prefix, alias->name,
                      target->symbol_prefix, alias->name, alias->target);
      continue;
    }
    ok = nyr_printf(&assembly,
                    "\t%s %s%s\n"
                    "\t.type %s%s,@function\n"
                    "\t.set %s%s,%s%s\n"
                    "\t.globl %s__nyc_extern_%s\n"
                    "\t.set %s__nyc_extern_%s,%s%s\n"
                    "\t.globl %sny_fn_%s\n"
                    "\t.set %sny_fn_%s,%s%s\n",
                    binding, target->symbol_prefix, alias->name,
                    target->symbol_prefix, alias->name, target->symbol_prefix,
                    alias->name, target->symbol_prefix, alias->target,
                    target->symbol_prefix, alias->name, target->symbol_prefix,
                    alias->name, target->symbol_prefix, alias->target,
                    target->symbol_prefix, alias->name, target->symbol_prefix,
                    alias->name, target->symbol_prefix, alias->target);
  }
  for (size_t i = 0; ok && i < unit->variable_alias_count; ++i) {
    const nyc_variable_alias_t *alias = &unit->variable_aliases[i];
    ok = nyr_printf(&assembly,
                    "\t.%s %s%s\n"
                    "\t.type %s%s,@object\n"
                    "\t.set %s%s,%s%s\n",
                    alias->is_local ? "local" : "globl", target->symbol_prefix,
                    alias->name, target->symbol_prefix, alias->name,
                    target->symbol_prefix, alias->name, target->symbol_prefix,
                    alias->target);
  }
  if (ok)
    ok = nyr_put(&assembly, "\t.section .note.GNU-stack,\"\",@progbits\n");
  if (ok) {
    FILE *output = strcmp(path, "-") == 0 ? stdout : fopen(path, "wb");
    if (!output) {
      snprintf(err, err_len, "cannot open assembly output '%s'", path);
      ok = false;
    } else {
      ok = !assembly.len ||
           fwrite(assembly.data, 1, assembly.len, output) == assembly.len;
      if (output == stdout) {
        if (fflush(output) != 0)
          ok = false;
      } else if (fclose(output) != 0) {
        ok = false;
      }
      if (!ok)
        snprintf(err, err_len, "cannot write assembly output '%s'", path);
    }
  }
  free(assembly.data);
  return ok;
}

static int compile_source(int argc, char **argv, int source_arg,
                          const nyc_driver_t *driver) {
  const char *target = driver->target;
  bool compile_unit = driver->compile_unit;
  int expected_args = driver->preprocess ? 1 : compile_unit ? 2 : 3;
  if (!driver->preprocess && driver->output)
    --expected_args;
  if (argc != source_arg + expected_args) {
    usage(stderr);
    return 2;
  }
  size_t source_len = 0;
  char *source = read_source(argv[source_arg], &source_len);
  if (!source) {
    fprintf(stderr, "%s: cannot read C source\n", argv[source_arg]);
    return 1;
  }
  char err[512] = {0};
  nyr_target_t host;
  const char *triple = target;
  if (!triple) {
    if (!nyr_host_target(&host, err, sizeof(err))) {
      fprintf(stderr, "%s\n", err[0] ? err : "cannot determine host target");
      free(source);
      return 1;
    }
    triple = host.target;
  }
  nyc_abi_t abi = nyc_abi_for_target(triple);
  if (driver->stack_protector && !driver->preprocess &&
      abi != NYC_ABI_SYSV_X86_64) {
    fprintf(stderr, "nyc: stack protection requires SysV x86-64\n");
    free(source);
    return 2;
  }
  if (driver->i386_regparm && abi != NYC_ABI_I386) {
    fprintf(stderr, "nyc: -mregparm requires an i386 target\n");
    free(source);
    return 2;
  }
  if (driver->require_x86 && abi != NYC_ABI_I386 &&
      abi != NYC_ABI_SYSV_X86_64 && abi != NYC_ABI_WIN64) {
    fprintf(stderr,
            "nyc: x86 instruction options require an x86 target, got '%s'\n",
            triple);
    free(source);
    return 1;
  }
  if (driver->require_x86_64 && abi != NYC_ABI_SYSV_X86_64 &&
      abi != NYC_ABI_WIN64) {
    fprintf(stderr, "nyc: -m64 requires an x86-64 target, got '%s'\n", triple);
    free(source);
    return 1;
  }
  char *forced_source = NULL;
  size_t forced_source_len = 0;
  if (driver->forced_include_count) {
    size_t capacity = 1;
    size_t include_count =
        driver->forced_include_count + (strcmp(argv[source_arg], "-") != 0);
    for (size_t i = 0; i < include_count; ++i) {
      const char *include = i == driver->forced_include_count
                                ? argv[source_arg]
                                : driver->forced_includes[i];
      size_t length = strlen(include);
      if (length > (SIZE_MAX - capacity - 16) / 2) {
        fprintf(stderr, "nyc: forced include path is too long\n");
        free(source);
        return 2;
      }
      capacity += 12 + length * 2;
    }
    if (strcmp(argv[source_arg], "-") == 0) {
      if (source_len > SIZE_MAX - capacity - 1) {
        fputs("nyc: standard input is too large with forced includes\n",
              stderr);
        free(source);
        return 2;
      }
      capacity += source_len + 1;
    }
    forced_source = malloc(capacity);
    if (!forced_source) {
      fputs("nyc: out of memory preparing forced includes\n", stderr);
      free(source);
      return 1;
    }
    bool included = true;
    for (size_t i = 0; included && i < include_count; ++i) {
      const char *include = i == driver->forced_include_count
                                ? argv[source_arg]
                                : driver->forced_includes[i];
      included = append_quoted_include(forced_source, capacity,
                                       &forced_source_len, include);
    }
    if (included && strcmp(argv[source_arg], "-") == 0) {
      memcpy(forced_source + forced_source_len, source, source_len);
      forced_source_len += source_len;
    }
    if (!included) {
      fputs("nyc: cannot prepare forced include list\n", stderr);
      free(forced_source);
      free(source);
      return 2;
    }
    forced_source[forced_source_len] = '\0';
    free(source);
    source = forced_source;
    source_len = forced_source_len;
  }
  char *size_disabled_passes = NULL;
  nyc_source_t input = {
      .text = source,
      .length = source_len,
      .path = driver->forced_include_count         ? "<nyc-driver-input>"
              : strcmp(argv[source_arg], "-") == 0 ? "<stdin>"
                                                   : argv[source_arg],
      .base_path =
          strcmp(argv[source_arg], "-") == 0 ? "<stdin>" : argv[source_arg],
      .include_dirs = driver->include_dirs,
      .include_dir_count = driver->include_count,
      .macros = driver->macros,
      .macro_count = driver->macro_count,
      .path_maps = driver->path_maps,
      .path_map_count = driver->path_map_count,
      .short_wchar = driver->short_wchar,
      .unsigned_char = driver->unsigned_char,
      .no_builtin = driver->no_builtin,
      .i386_regparm = driver->i386_regparm,
      .ms_extensions = driver->ms_extensions,
      .plan9_extensions = driver->plan9_extensions,
      .system_includes = !driver->no_std_includes && !driver->target};
  input.optimization = (unsigned)driver->optimization;
  const char *output_path = driver->output       ? driver->output
                            : driver->preprocess ? NULL
                                                 : argv[source_arg + 1];
  bool ok = false;
  if (driver->preprocess) {
    FILE *output = output_path ? fopen(output_path, "wb") : stdout;
    if (output) {
      ok =
          driver->dump_macros
              ? nyc_frontend_dump_macros(&input, abi, output, err, sizeof(err))
              : nyc_frontend_preprocess(&input, abi, output,
                                        driver->line_markers, err, sizeof(err));
      if ((output_path ? fclose(output) : fflush(output)) != 0) {
        snprintf(err, sizeof(err), "cannot finish preprocessed C output");
        ok = false;
      }
    } else {
      snprintf(err, sizeof(err), "cannot open preprocessed C output '%s'",
               output_path);
    }
  } else if (compile_unit) {
    nyc_unit_t unit = {0};
    ny_native_target_info_t target_info = {0};
    ok = nyc_frontend_compile_unit(&input, abi, &unit, err, sizeof(err));
    if (ok) {
      /*
       * The frontend has already removed data owned by unreferenced static
       * functions. Keep addressability anchors owned by surviving functions:
       * later optimization/inlining can erase their otherwise-unused IR
       * address, but objtool still requires the section relocation.
       */
      for (size_t data = 0; data < unit.data_count; ++data) {
        nyc_data_t *anchor = &unit.data[data];
        if (!anchor->owner_function || !anchor->section ||
            strcmp(anchor->section, ".discard.addressable") != 0)
          continue;
        free(anchor->owner_function);
        anchor->owner_function = NULL;
      }
    }
    bool assembly_target = abi == NYC_ABI_SYSV_X86_64 ||
                           (abi == NYC_ABI_I386 && driver->assembly_output);
    if (ok &&
        (!assembly_target ||
         !nyr_target_info_for_triple(triple, &target_info, err, sizeof(err)))) {
      if (!err[0])
        snprintf(err, sizeof(err),
                 "whole-unit output currently requires x86-64 ELF, or i386 "
                 "GNU assembly output");
      ok = false;
    }
    nyr_inline_callee_t *inline_callees = NULL;
    size_t inline_callee_count = 0;
    if (ok && unit.count) {
      inline_callees = calloc(unit.count, sizeof(*inline_callees));
      if (!inline_callees) {
        snprintf(err, sizeof(err), "out of memory preparing C inline callees");
        ok = false;
      } else {
        for (size_t i = 0; i < unit.count; ++i) {
          bool has_static_feature_asm = false;
          for (size_t inst = 0; inst < unit.funcs[i].len; ++inst)
            has_static_feature_asm |=
                unit.funcs[i].data[inst].op == NYIR_INLINE_ASM &&
                unit.funcs[i].data[inst].imm == -100;
          /*
           * Retention and inlining are independent. The selected optimizer
           * applies its own size limits to the optimized body, not the
           * unoptimized declaration. PIC global definitions can be interposed;
           * local definitions and non-weak non-PIC definitions bind here.
           */
          bool binds_locally =
              unit.function_local[i] ||
              (!driver->position_independent && !unit.function_weak[i]);
          bool auto_inline = driver->optimization >= 1 && binds_locally;
          if (unit.function_noinline[i] ||
              nyc_func_uses_va_list(&unit.funcs[i]) ||
              (driver->optimize_size && !has_static_feature_asm &&
               !(unit.function_always_inline && unit.function_always_inline[i])) ||
              (!unit.function_inline[i] && !has_static_feature_asm &&
               !auto_inline))
            continue;
          inline_callees[inline_callee_count++] = (nyr_inline_callee_t){
              .name = unit.names[i], .func = &unit.funcs[i]};
        }
      }
    }
    if (ok && inline_callee_count) {
      nyr_inline_set_c_unit_abi(true);
      nyr_set_inline_callees(inline_callees, inline_callee_count);
    }
    if (ok && getenv("NYC_DISABLE_PASS"))
      nyr_set_pass_controls(getenv("NYC_DISABLE_PASS"), NULL);
    if (ok && driver->optimize_size) {
      static const char disabled[] =
          "loop_unroll,unroll_jam,loop_peel,loop_unswitch,loop_versioning,"
          "loop_predication,softpipe_rewrite,loop_vectorize,slp_vectorize";
      const char *user = getenv("NYC_DISABLE_PASS");
      size_t bytes = sizeof(disabled) + (user ? strlen(user) : 0) + 1;
      size_disabled_passes = malloc(bytes);
      if (!size_disabled_passes) {
        snprintf(err, sizeof(err), "cannot prepare size optimization controls");
        ok = false;
      } else {
        snprintf(size_disabled_passes, bytes, "%s,%s", disabled, user ? user : "");
        nyr_set_pass_controls(size_disabled_passes, NULL);
      }
    }
    /*
     * The object writer's register-allocating path resolves PHIs into edge
     * copies. Destroying them in the optimizer sends every merged value
     * through memory. Text assembly and fallback encoders still get them
     * destroyed further down.
     */
    nyr_set_preserve_phis(true);
    for (size_t i = 0; ok && i < unit.count; i++) {
      const char *stage = "before optimization";
      ok = nyr_verify(&unit.funcs[i], err, sizeof(err));
      if (ok && driver->disable_vectorization &&
          nyc_func_uses_fp_or_simd(&unit.funcs[i])) {
        snprintf(err, sizeof(err),
                 "floating-point or SIMD code is incompatible with "
                 "the selected no-SSE target");
        ok = false;
      }
      /*
       * C values are never tagged, so raw-class copies are plain aliases.
       */
      unit.funcs[i].untagged_values = true;
      if (ok && driver->optimization >= 2 &&
          nyc_func_can_rebind_tail_args(&unit.funcs[i])) {
        stage = "during tail-call optimization";
        ok = nyr_tail_call(&unit.funcs[i], unit.names[i]);
      }
      if (ok && !nyc_func_initializes_va_list(&unit.funcs[i])) {
        stage = "during optimization";
        if (!nyr_optimize_with_options(&unit.funcs[i], driver->optimization,
                                       driver->disable_vectorization
                                           ? NYR_OPT_DISABLE_VECTORIZATION
                                           : 0,
                                       err, sizeof(err))) {
          fprintf(
              stderr,
              "NYC: warning: %s for function %s: %s (continuing unoptimized)\n",
              stage, unit.names[i], err[0] ? err : "optimization failed");
          err[0] = '\0';
          ok = true;
          stage = "after optimization fallback";
        }
      }
      /*
       * Keep NYC's whole-unit pipeline aligned with the native language
       * compiler for the verified integer-add recursion pattern. The general
       * tail-call rebinding runs above, before SSA replaces parameter slots.
       */
      if (ok && driver->optimization >= 2) {
        stage = "during recursive-add optimization";
        size_t before_tail_call = unit.funcs[i].len;
        ok = nyr_recursive_add_to_loop(&unit.funcs[i], unit.names[i]);
        if (ok && unit.funcs[i].len != before_tail_call) {
          stage = "during recursive-add cleanup";
          unit.funcs[i].untagged_values = true;
          if (!nyr_optimize_with_options(&unit.funcs[i], driver->optimization,
                                         driver->disable_vectorization
                                             ? NYR_OPT_DISABLE_VECTORIZATION
                                             : 0,
                                         err, sizeof(err))) {
            fprintf(stderr,
                    "NYC: warning: %s for function %s: %s (continuing "
                    "unoptimized)\n",
                    stage, unit.names[i], err[0] ? err : "optimization failed");
            err[0] = '\0';
          }
        }
      }
      if (ok && driver->disable_vectorization &&
          nyc_func_uses_fp_or_simd(&unit.funcs[i])) {
        snprintf(err, sizeof(err),
                 "optimizer introduced floating-point or SIMD code under "
                 "the selected no-SSE target");
        ok = false;
      }
      if (ok) {
        stage = "after optimization";
        ok = nyr_verify(&unit.funcs[i], err, sizeof(err));
      }
      if (!ok) {
        char detail[sizeof(err)];
        memcpy(detail, err, sizeof(detail));
        detail[sizeof(detail) - 1] = '\0';
        snprintf(err, sizeof(err), "%s for function %s: %.360s", stage,
                 unit.names[i],
                 detail[0] ? detail : "NYIR verification failed");
      }
    }
    /*
     * C's always_inline contract applies at every optimization level. Static
     * alternative helpers also need their feature arguments specialized before
     * their inline assembly is emitted. Run the cloner only on those required
     * callees; ordinary inline candidates keep the selected optimization
     * level's policy.
     */
    nyr_inline_callee_t *static_feature_callees = NULL;
    size_t static_feature_callee_count = 0;
    if (ok && inline_callee_count) {
      static_feature_callees =
          calloc(inline_callee_count, sizeof(*static_feature_callees));
      if (!static_feature_callees) {
        snprintf(err, sizeof(err),
                 "out of memory preparing static-feature inline helpers");
        ok = false;
      } else {
        for (size_t c = 0; c < inline_callee_count; ++c) {
          bool required = false;
          for (size_t f = 0; f < unit.count; ++f)
            required |= inline_callees[c].func == &unit.funcs[f] &&
                        unit.function_always_inline[f];
          if (required) {
            static_feature_callees[static_feature_callee_count++] =
                inline_callees[c];
            continue;
          }
          for (size_t inst = 0; inst < inline_callees[c].func->len; ++inst)
            if (inline_callees[c].func->data[inst].op == NYIR_INLINE_ASM &&
                inline_callees[c].func->data[inst].imm == -100) {
              static_feature_callees[static_feature_callee_count++] =
                  inline_callees[c];
              break;
            }
        }
      }
      if (ok && static_feature_callee_count) {
        nyr_set_inline_callees(static_feature_callees,
                               static_feature_callee_count);
        for (size_t i = 0; ok && i < unit.count; ++i) {
          bool has_static_feature_call = false;
          for (size_t inst = 0; inst < unit.funcs[i].len; ++inst) {
            const nyr_inst_t *call = &unit.funcs[i].data[inst];
            if (call->op != NYIR_CALL || !call->symbol)
              continue;
            for (size_t c = 0; c < static_feature_callee_count; ++c)
              has_static_feature_call |=
                  strcmp(call->symbol, static_feature_callees[c].name) == 0;
          }
          if (!has_static_feature_call)
            continue;
          if (!nyr_inline_general(&unit.funcs[i])) {
            snprintf(err, sizeof(err),
                     "failed to specialize static-feature calls in %s",
                     unit.names[i]);
            ok = false;
            break;
          }
          unit.funcs[i].untagged_values = true;
          if (!nyr_optimize_with_options(&unit.funcs[i], driver->optimization,
                                         driver->disable_vectorization
                                             ? NYR_OPT_DISABLE_VECTORIZATION
                                             : 0,
                                         err, sizeof(err))) {
            fprintf(stderr,
                    "NYC: warning: NYIR cleanup after static-feature inlining "
                    "for %s: %s (continuing unoptimized)\n",
                    unit.names[i], err[0] ? err : "optimization failed");
            err[0] = '\0';
          }
          if (!nyr_verify(&unit.funcs[i], err, sizeof(err))) {
            char detail[sizeof(err)];
            memcpy(detail, err, sizeof(detail));
            detail[sizeof(detail) - 1] = '\0';
            snprintf(err, sizeof(err),
                     "NYIR verify after static-feature inlining for %s: "
                     "%.360s",
                     unit.names[i], detail);
            ok = false;
          }
        }
        nyr_set_inline_callees(inline_callees, inline_callee_count);
      }
    }
    free(static_feature_callees);
    /* Size builds can shrink a one-use helper by removing its call boundary,
     * including helpers with bounded control flow and fixed-size stack
     * objects. Reject dynamic-size allocation and external branch targets;
     * keep the body budget small and decide after the earlier optimization. */
    if (ok && driver->optimize_size && !getenv("NYC_DISABLE_SIZE_INLINE")) {
      size_t original_count = inline_callee_count;
      for (size_t i = 0; i < unit.count; ++i) {
        const nyr_func_t *callee = &unit.funcs[i];
        bool eligible = !unit.function_noinline[i] &&
                        !nyc_func_uses_va_list(callee) &&
                        (unit.function_local[i] ||
                         (!driver->position_independent && !unit.function_weak[i]));
        size_t work = 0;
        for (size_t k = 0; eligible && k < callee->len; ++k) {
          const nyr_inst_t *in = &callee->data[k];
          switch (in->op) {
          case NYIR_ALLOCA:
            eligible &= in->imm > 0;
            work += 2;
            break;
          case NYIR_BR: case NYIR_BR_IF: {
            bool internal = false;
            for (size_t target = 0; target < callee->len; ++target)
              internal |= callee->data[target].op == NYIR_LABEL &&
                          callee->data[target].imm == in->imm;
            eligible &= internal;
            work += in->op == NYIR_BR_IF;
            break;
          }
          case NYIR_LABEL: case NYIR_NOP: case NYIR_RET:
          case NYIR_LOAD_LOCAL:
            break;
          default:
            ++work;
            break;
          }
        }
        size_t call_sites = 0;
        for (size_t caller = 0; caller < unit.count; ++caller)
          for (size_t inst = 0; inst < unit.funcs[caller].len; ++inst) {
            const nyr_inst_t *call = &unit.funcs[caller].data[inst];
            if (call->op == NYIR_CALL && call->symbol &&
                strcmp(call->symbol, unit.names[i]) == 0)
              ++call_sites;
          }
        eligible &= call_sites == 1;
        for (size_t c = 0; c < inline_callee_count; ++c)
          if (inline_callees[c].func == callee) eligible = false;
        if (eligible && work <= 112 && callee->len <= 224)
          inline_callees[inline_callee_count++] = (nyr_inline_callee_t){
              .name = unit.names[i], .func = callee};
      }
      if (inline_callee_count > original_count) {
        nyr_inline_set_c_unit_abi(true);
        nyr_set_inline_callees(inline_callees, inline_callee_count);
        for (size_t i = 0; ok && i < unit.count; ++i) {
          nyr_func_t *func = &unit.funcs[i];
          if (!func->has_symbol_calls || nyc_func_initializes_va_list(func))
            continue;
          ok = nyr_inline_general(func) &&
               nyr_optimize_with_options(func, driver->optimization,
                   driver->disable_vectorization ? NYR_OPT_DISABLE_VECTORIZATION : 0,
                   err, sizeof(err));
        }
        if (!ok && !err[0])
          snprintf(err, sizeof(err), "size-oriented C helper inlining failed");
      }
    }
    /*
     * A prototype can place a caller before its callee in the unit table.
     * Revisit small calls after every body has been optimized, so disabled
     * feature stubs do not depend on declaration order. Keep this bounded:
     * the small inliner does not introduce calls or clone arbitrary CFGs.
     */
    if (ok && driver->optimization >= 1 && inline_callee_count) {
      nyr_set_inline_callees(inline_callees, inline_callee_count);
      for (size_t i = 0; ok && i < unit.count; ++i) {
        nyr_func_t *func = &unit.funcs[i];
        if (!func->has_symbol_calls)
          continue;
        size_t length_before = func->len;
        size_t before = 0, after = 0;
        for (size_t k = 0; k < func->len; ++k)
          before += func->data[k].op == NYIR_CALL;
        ok = nyr_inline_small(func);
        if (!ok)
          break;
        for (size_t k = 0; k < func->len; ++k)
          after += func->data[k].op == NYIR_CALL;
        if (after == before && func->len == length_before)
          continue;
        ok = nyr_const_fold(func) && nyr_copy_prop(func) && nyr_dce(func) &&
             nyr_cfg_simplify(func) && nyr_dce(func) && nyr_compact(func);
        if (ok) {
          nyr_refresh_metadata(func);
          ok = nyr_verify(func, err, sizeof(err));
        }
      }
      if (!ok && !err[0])
        snprintf(err, sizeof(err), "late C small-call cleanup failed");
    }
    if (ok && driver->optimization >= 1)
      ok = nyc_specialize_private_parameters(&unit, driver, err, sizeof(err));
    if (inline_callee_count) {
      nyr_set_inline_callees(NULL, 0);
      nyr_inline_set_c_unit_abi(false);
    }
    free(inline_callees);
    if (ok && driver->optimization >= 2 &&
        !nyc_eliminate_pure_self_calls(&unit)) {
      snprintf(err, sizeof(err), "pure recursive call CSE failed");
      ok = false;
    }
    if (ok)
      ok = nyc_unit_prune_static(&unit, err, sizeof(err));
    if (ok && !driver->no_zero_initialized_in_bss &&
        strcmp(target_info.object_format, "elf") == 0) {
      for (size_t i = 0; ok && i < unit.data_count; ++i) {
        nyc_data_t *data = &unit.data[i];
        if (data->section || data->reloc_count || data->function_symbol ||
            !data->len || (data->section_flags && !(data->section_flags & 1)))
          continue;
        bool zero = true;
        for (size_t b = 0; zero && b < data->len; ++b)
          zero = data->bytes[b] == 0;
        if (zero) {
          data->section = strdup(".bss");
          if (!data->section) {
            snprintf(err, sizeof(err), "out of memory selecting C BSS data");
            ok = false;
          }
        }
      }
    }
    if (ok && (driver->assembly_output || abi == NYC_ABI_I386)) {
      for (size_t i = 0; ok && i < unit.count; ++i) {
        if (!nyr_phi_elim(&unit.funcs[i])) {
          snprintf(err, sizeof(err), "phi elimination failed for %s",
                   unit.names[i]);
          ok = false;
        }
      }
    }
    for (size_t i = 0; ok && driver->stack_protector && i < unit.count; ++i)
      if (!unit.function_no_stack_protector[i])
        ok = nyc_protect_stack(&unit.funcs[i], driver, err, sizeof(err));
    if (ok && driver->assembly_output)
      ok = write_c_unit_assembly(
          &unit, &target_info, output_path, driver->verbose_assembly,
          driver->code16, driver->code16 && nyc_setup_macro_enabled(driver),
          driver->position_independent,
          driver->i386_stack_alignment, driver->retpoline_indirect_calls,
          driver->retpoline_inline_calls, driver->function_return_thunk_extern,
          driver->cf_protection_branch, err, sizeof(err));
    if (ok && !driver->assembly_output) {
      size_t data_count = unit.data_count + unit.variable_alias_count;
      nyr_c_object_data_t *data =
          data_count ? calloc(data_count, sizeof(*data)) : NULL;
      nyr_c_object_data_reloc_t **data_relocs =
          ok && unit.data_count ? calloc(unit.data_count, sizeof(*data_relocs))
                                : NULL;
      if (ok && data_count && !data) {
        snprintf(err, sizeof(err), "out of memory preparing C global data");
        ok = false;
      }
      if (ok && unit.data_count && !data_relocs) {
        snprintf(err, sizeof(err),
                 "out of memory preparing C data relocations");
        ok = false;
      }
      for (size_t i = 0; ok && i < unit.data_count; ++i) {
        if (unit.data[i].reloc_count) {
          data_relocs[i] =
              calloc(unit.data[i].reloc_count, sizeof(*data_relocs[i]));
          if (!data_relocs[i]) {
            snprintf(err, sizeof(err),
                     "out of memory preparing C data relocations");
            ok = false;
            break;
          }
          for (size_t r = 0; r < unit.data[i].reloc_count; ++r)
            data_relocs[i][r] = (nyr_c_object_data_reloc_t){
                .offset = unit.data[i].relocs[r].offset,
                .symbol = unit.data[i].relocs[r].symbol,
                .addend = unit.data[i].relocs[r].addend,
                .width = unit.data[i].relocs[r].width,
                .type = unit.data[i].relocs[r].type == NYC_DATA_RELOC_PC32
                            ? NY_RELOC_PC32
                        : unit.data[i].relocs[r].type == NYC_DATA_RELOC_ABS32S
                            ? NY_RELOC_X86_64_32S
                            : NY_RELOC_X86_64_64};
        }
        data[i] = (nyr_c_object_data_t){
            .name = unit.data[i].name,
            .bytes = unit.data[i].bytes,
            .len = unit.data[i].len,
            .section = unit.data[i].section,
            .section_flags = unit.data[i].section_flags,
            .alignment = unit.data[i].alignment,
            .section_entsize = unit.data[i].section_entsize,
            .function_symbol = unit.data[i].function_symbol,
            .is_local = unit.data[i].is_local,
            .is_weak = unit.data[i].is_weak,
            .relocs = data_relocs[i],
            .reloc_count = unit.data[i].reloc_count};
      }
      for (size_t i = 0; ok && i < unit.variable_alias_count; ++i)
        data[unit.data_count + i] = (nyr_c_object_data_t){
            .name = unit.variable_aliases[i].name,
            .alias_target = unit.variable_aliases[i].target,
            .is_local = unit.variable_aliases[i].is_local};
      if (ok)
        ok = nyr_native_emit_elf64_c_object_from_nirs_with_data(
            unit.funcs, (const char *const *)unit.names, unit.count,
            (const char *const *)unit.function_sections, unit.function_local,
            driver->omit_frame_pointer, driver->position_independent,
            driver->retpoline_indirect_calls, driver->retpoline_inline_calls,
            data, data_count, &target_info, output_path, err, sizeof(err));
      free(data);
      if (data_relocs) {
        for (size_t i = 0; i < unit.data_count; ++i)
          free(data_relocs[i]);
        free(data_relocs);
      }
    }
    if (!ok && !err[0])
      snprintf(err, sizeof(err), "C translation-unit compilation failed");
    if (ok)
      fprintf(stderr, "compiled %s -> %s (%zu functions, %zu data symbols)\n",
              argv[source_arg], output_path, unit.count, unit.data_count);
    nyc_unit_destroy(&unit);
  } else {
    nyr_func_t ir;
    nyr_func_init(&ir);
    ok = nyc_frontend_compile(&input, argv[argc - 1], abi, &ir, err,
                              sizeof(err)) &&
         nyr_verify(&ir, err, sizeof(err));
    if (ok && driver->disable_vectorization && nyc_func_uses_fp_or_simd(&ir)) {
      snprintf(err, sizeof(err),
               "floating-point or SIMD code is incompatible with "
               "the selected no-SSE target");
      ok = false;
    }
    if (ok && getenv("NYC_DISABLE_PASS"))
      nyr_set_pass_controls(getenv("NYC_DISABLE_PASS"), NULL);
    if (ok && !nyc_func_uses_va_list(&ir) &&
        !nyr_optimize_with_options(
            &ir, driver->optimization,
            driver->disable_vectorization ? NYR_OPT_DISABLE_VECTORIZATION : 0,
            err, sizeof(err))) {
      fprintf(stderr,
              "NYC: warning: optimization failed for %s: %s (continuing "
              "unoptimized)\n",
              argv[argc - 1], err[0] ? err : "optimization failed");
      err[0] = '\0';
    }
    if (ok && driver->disable_vectorization && nyc_func_uses_fp_or_simd(&ir)) {
      snprintf(err, sizeof(err),
               "optimizer introduced floating-point or SIMD code under "
               "the selected no-SSE target");
      ok = false;
    }
    ok = ok && nyr_verify(&ir, err, sizeof(err)) &&
         (target ? nyr_emit_object_for_target(&ir, output_path, argv[argc - 1],
                                              target, err, sizeof(err))
                 : nyr_emit_object(&ir, output_path, argv[argc - 1], err,
                                   sizeof(err)));
    if (ok)
      fprintf(stderr, "compiled %s -> %s (%s)\n", argv[source_arg], output_path,
              argv[argc - 1]);
    nyr_func_destroy(&ir);
  }
  if (!ok)
    fprintf(stderr, "%s: %s\n", argv[source_arg],
            err[0] ? err : "C compilation failed");
  if (ok && driver->dependency_file)
    ok = write_dependency_file(&input, abi,
                               output_path ? output_path : argv[source_arg],
                               driver->dependency_file, err, sizeof(err));
  if (!ok && err[0])
    fprintf(stderr, "%s: %s\n", argv[source_arg], err);
  if (size_disabled_passes) {
    nyr_set_pass_controls(NULL, NULL);
    free(size_disabled_passes);
  }
  free(source);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  int assembler_status = nyc_handle_probe_options(argc, argv);
  if (assembler_status >= 0)
    return assembler_status;
  nyc_driver_t driver = {.optimization =
                             getenv("NYC_OPT_O0") ? NYR_OPT_O0 : NYR_OPT_O2,
                         .line_markers = true,
                         .stack_guard_offset = 40};
  int result = 2;
  char *operands[4] = {argv[0]};
  int operand_count = 1;
  bool end_options = false;
  bool object_mode = false;
  for (int argument = 1; argument < argc; ++argument) {
    const char *option = argv[argument];
    if (end_options || option[0] != '-' || !option[1]) {
      if (operand_count == 4) {
        fputs("nyc: too many input/output operands\n", stderr);
        goto done;
      }
      operands[operand_count++] = argv[argument];
      continue;
    }
    if (strcmp(option, "--") == 0) {
      end_options = true;
      continue;
    }
    if (strncmp(option, "--verbosity=", 12) == 0) {
      ny_dbg_init_from_verbosity(atoi(option + 12));
      continue;
    }
    if (strcmp(option, "--mixed-disasm") == 0) {
      nyr_set_mixed_disasm(true, NULL);
      continue;
    }
    if (strncmp(option, "--mixed-disasm=", 15) == 0) {
      nyr_set_mixed_disasm(true, option + 15);
      continue;
    }
    if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
      usage(stdout);
      result = 0;
      goto done;
    }
    if (strcmp(option, "--version") == 0) {
      printf("NYC (Nytrix C frontend, NYR backend) %s\n", ny_version_id());
      result = 0;
      goto done;
    }
    if (strcmp(option, "-E") == 0) {
      driver.preprocess = true;
      continue;
    }
    if (strcmp(option, "-nostdinc") == 0) {
      driver.no_std_includes = true;
      continue;
    }
    if (strcmp(option, "-dM") == 0) {
      driver.dump_macros = true;
      continue;
    }
    if (strcmp(option, "-print-file-name=plugin") == 0) {
      driver.print_file_name_plugin = true;
      continue;
    }
    if (strcmp(option, "-c") == 0) {
      object_mode = driver.compile_unit = true;
      continue;
    }
    if (strcmp(option, "-S") == 0) {
      driver.compile_unit = true;
      driver.assembly_output = true;
      continue;
    }
    if (strcmp(option, "-P") == 0) {
      driver.line_markers = false;
      continue;
    }
    if (option[0] == '-' && option[1] == 'O' && option[2] >= '0' &&
        option[2] <= '3' && !option[3]) {
      driver.optimization = (nyr_opt_level_t)(option[2] - '0');
      driver.optimize_size = false;
      continue;
    }
    if (strcmp(option, "-Os") == 0 || strcmp(option, "-Oz") == 0) {
      driver.optimization = NYR_OPT_O3;
      driver.optimize_size = true;
      continue;
    }
    if (strcmp(option, "-fconserve-stack") == 0) {
      driver.conserve_stack = true;
      continue;
    }
    if (strcmp(option, "-fno-stack-protector") == 0 ||
        strcmp(option, "-fstack-protector") == 0 ||
        strcmp(option, "-fstack-protector-strong") == 0 ||
        strcmp(option, "-fstack-protector-all") == 0) {
      driver.stack_protector = strcmp(option, "-fno-stack-protector") == 0 ? 0
                               : strcmp(option, "-fstack-protector") == 0  ? 1
                               : strcmp(option, "-fstack-protector-strong") == 0
                                   ? 2
                                   : 3;
      continue;
    }
    if (strncmp(option, "-mstack-protector-guard=", 24) == 0) {
      const char *kind = option + 24;
      if (strcmp(kind, "global") && strcmp(kind, "tls")) {
        fprintf(stderr, "nyc: unsupported stack guard kind '%s'\n", kind);
        goto done;
      }
      driver.stack_guard_global = strcmp(kind, "global") == 0;
      continue;
    }
    if (strncmp(option, "-mstack-protector-guard-reg=", 28) == 0) {
      const char *reg = option + 28;
      if (strcmp(reg, "fs") && strcmp(reg, "gs")) {
        fprintf(stderr, "nyc: unsupported stack guard register '%s'\n", reg);
        goto done;
      }
      driver.stack_guard_gs = strcmp(reg, "gs") == 0;
      continue;
    }
    if (strncmp(option, "-mstack-protector-guard-offset=", 31) == 0) {
      const char *number = option + 31;
      char *end = NULL;
      errno = 0;
      int64_t offset = strtoll(number, &end, 0);
      if (errno || !*number || *end || offset < INT32_MIN ||
          offset > INT32_MAX) {
        fprintf(stderr, "nyc: invalid stack guard offset '%s'\n", number);
        goto done;
      }
      driver.stack_guard_offset = offset;
      continue;
    }
    if (strncmp(option, "-mstack-protector-guard-symbol=", 31) == 0) {
      const char *symbol = option + 31;
      bool valid =
          *symbol && (isalpha((unsigned char)*symbol) || *symbol == '_');
      for (size_t i = 1; valid && symbol[i]; ++i)
        valid = isalnum((unsigned char)symbol[i]) || symbol[i] == '_';
      if (!valid) {
        fprintf(stderr, "nyc: invalid stack guard symbol '%s'\n", symbol);
        goto done;
      }
      driver.stack_guard_symbol = symbol;
      continue;
    }
    if (strncmp(option, "-fstack-protector", 17) == 0 ||
        strncmp(option, "-mstack-protector-", 18) == 0) {
      fprintf(stderr, "nyc: unsupported stack protection option '%s'\n",
              option);
      goto done;
    }
    if (strcmp(option, "-x") == 0 || strcmp(option, "-o") == 0 ||
        strcmp(option, "-include") == 0) {
      if (++argument == argc) {
        fprintf(stderr, "nyc: %s requires an argument\n", option);
        goto done;
      }
      if (option[1] == 'o')
        driver.output = argv[argument];
      else if (strcmp(option, "-include") == 0) {
        if (!add_forced_include(&driver, argv[argument])) {
          fputs("nyc: cannot store forced include path\n", stderr);
          result = 1;
          goto done;
        }
      } else if (strcmp(argv[argument], "c") != 0) {
        fprintf(stderr, "nyc: unsupported input language '%s'\n",
                argv[argument]);
        goto done;
      }
      continue;
    }
    if (strcmp(option, "-pthread") == 0) {
      if (!driver.threaded) {
        if (!add_macro_option(&driver, "_REENTRANT=1", false)) {
          fputs("nyc: cannot store pthread feature macro\n", stderr);
          result = 1;
          goto done;
        }
        /*
         * Driver feature macros precede explicit -D/-U options, regardless
         * of where -pthread occurs on the command line.
         */
        nyc_macro_option_t feature = driver.macros[driver.macro_count - 1];
        memmove(driver.macros + 1, driver.macros,
                (driver.macro_count - 1) * sizeof(*driver.macros));
        driver.macros[0] = feature;
        driver.threaded = true;
      }
      continue;
    }
    if (option[0] == '-' && (option[1] == 'D' || option[1] == 'U')) {
      const char *text = option + 2;
      if (!*text) {
        if (++argument == argc) {
          fprintf(stderr, "nyc: -%c requires an argument\n", option[1]);
          goto done;
        }
        text = argv[argument];
      }
      if (!add_macro_option(&driver, text, option[1] == 'U')) {
        fputs("nyc: cannot store macro option\n", stderr);
        result = 1;
        goto done;
      }
      continue;
    }
    if (strcmp(option, "--all") == 0) {
      driver.compile_unit = true;
      continue;
    }
    if (strncmp(option, "--target=", 9) == 0 && option[9]) {
      driver.target = option + 9;
      continue;
    }
    if (strncmp(option, "-I", 2) == 0) {
      const char *directory = option + 2;
      if (!*directory) {
        if (++argument == argc) {
          fputs("nyc: -I requires a directory\n", stderr);
          goto done;
        }
        directory = argv[argument];
      }
      if (driver.include_count == driver.include_capacity) {
        size_t capacity =
            driver.include_capacity ? driver.include_capacity * 2 : 8;
        if (capacity < driver.include_capacity ||
            capacity > SIZE_MAX / sizeof(*driver.include_dirs)) {
          fputs("nyc: too many include directories\n", stderr);
          goto done;
        }
        const char **directories =
            realloc(driver.include_dirs, capacity * sizeof(*directories));
        if (!directories) {
          fputs("nyc: out of memory storing include directories\n", stderr);
          result = 1;
          goto done;
        }
        driver.include_dirs = directories;
        driver.include_capacity = capacity;
      }
      driver.include_dirs[driver.include_count++] = directory;
      continue;
    }
    if (strncmp(option, "-include", 8) == 0 && option[8]) {
      if (!add_forced_include(&driver, option + 8)) {
        fputs("nyc: cannot store forced include path\n", stderr);
        result = 1;
        goto done;
      }
      continue;
    }
    if (strncmp(option, "-Wp,-MMD,", 9) == 0 && option[9]) {
      driver.dependency_file = option + 9;
      continue;
    }
    if (strncmp(option, "-Wp,-MD,", 8) == 0 && option[8]) {
      driver.dependency_file = option + 8;
      continue;
    }
    if (strncmp(option, "-fmacro-prefix-map=", 19) == 0) {
      if (!add_path_map(&driver, option + 19)) {
        fprintf(stderr, "nyc: invalid -fmacro-prefix-map argument '%s'\n",
                option + 19);
        goto done;
      }
      continue;
    }
    if (strcmp(option, "-fshort-wchar") == 0) {
      driver.short_wchar = true;
      continue;
    }
    if (strcmp(option, "-fzero-initialized-in-bss") == 0 ||
        strcmp(option, "-fno-zero-initialized-in-bss") == 0) {
      driver.no_zero_initialized_in_bss = option[2] == 'n';
      continue;
    }
    if (strcmp(option, "-funsigned-char") == 0) {
      driver.unsigned_char = true;
      continue;
    }
    if (strcmp(option, "-fno-builtin") == 0 ||
        strcmp(option, "-ffreestanding") == 0) {
      driver.no_builtin = true;
      continue;
    }
    if (strcmp(option, "-fms-extensions") == 0) {
      driver.ms_extensions = true;
      continue;
    }
    if (strcmp(option, "-fplan9-extensions") == 0) {
      driver.plan9_extensions = true;
      continue;
    }
    if (strcmp(option, "-fms-extensions") == 0 ||
        strcmp(option, "-fms-anonymous-structs") == 0) {
      driver.ms_extensions = true;
      continue;
    }
    if (strcmp(option, "-fno-ms-extensions") == 0 ||
        strcmp(option, "-fno-ms-anonymous-structs") == 0) {
      driver.ms_extensions = false;
      continue;
    }
    if (strcmp(option, "-fomit-frame-pointer") == 0) {
      driver.omit_frame_pointer = true;
      continue;
    }
    if (strcmp(option, "-fno-omit-frame-pointer") == 0) {
      driver.omit_frame_pointer = false;
      continue;
    }
    if (strcmp(option, "-g") == 0)
      continue;
    if (strcmp(option, "-foptimize-sibling-calls") == 0)
      continue;
    if (strcmp(option, "-fasynchronous-unwind-tables") == 0)
      continue;
    if (strcmp(option, "-fverbose-asm") == 0) {
      driver.verbose_assembly = true;
      continue;
    }
    if (strcmp(option, "-fPIC") == 0 || strcmp(option, "-fpic") == 0 ||
        strcmp(option, "-fPIE") == 0) {
      driver.position_independent = true;
      continue;
    }
    if (strcmp(option, "-fno-pic") == 0 || strcmp(option, "-fno-PIC") == 0 ||
        strcmp(option, "-fno-pie") == 0 || strcmp(option, "-fno-PIE") == 0) {
      driver.position_independent = false;
      continue;
    }
    if (strcmp(option, "-mindirect-branch=thunk-extern") == 0) {
      driver.retpoline_indirect_calls = true;
      continue;
    }
    if (strcmp(option, "-mindirect-branch=thunk-inline") == 0) {
      driver.retpoline_inline_calls = true;
      continue;
    }
    if (strcmp(option, "-fcf-protection=branch") == 0) {
      driver.cf_protection_branch = true;
      continue;
    }
    if (strcmp(option, "-fcf-protection=none") == 0) {
      driver.cf_protection_branch = false;
      continue;
    }
    if (strncmp(option, "-fcf-protection=", 16) == 0) {
      fprintf(stderr, "nyc: unsupported control-flow protection mode '%s'\n",
              option + 16);
      goto done;
    }
    if (strcmp(option, "-mfunction-return=thunk-extern") == 0) {
      driver.function_return_thunk_extern = true;
      continue;
    }
    if (strcmp(option, "-mindirect-branch-register") == 0 ||
        strcmp(option, "-mindirect-branch-cs-prefix") == 0) {
      driver.indirect_branch_cs_prefix =
          strcmp(option, "-mindirect-branch-cs-prefix") == 0 ||
          driver.indirect_branch_cs_prefix;
      continue;
    }
    if (strncmp(option, "-mpreferred-stack-boundary=", 27) == 0 &&
        option[27] >= '2' && option[27] <= '4' && option[28] == '\0') {
      driver.i386_stack_alignment = 1u << (unsigned)(option[27] - '0');
      continue;
    }
    if (strncmp(option, "-mregparm=", 10) == 0 && option[10] >= '0' &&
        option[10] <= '3' && option[11] == '\0') {
      driver.i386_regparm = (unsigned)(option[10] - '0');
      continue;
    }
    if (strcmp(option, "-march=i386") == 0)
      continue;
    if (strcmp(option, "-m32") == 0) {
      if (!driver.target)
        driver.target = "i386-unknown-linux-gnu";
      driver.require_x86_64 = false;
      continue;
    }
    if (strcmp(option, "-m16") == 0) {
      if (!driver.target)
        driver.target = "i386-unknown-linux-gnu";
      driver.require_x86_64 = false;
      driver.assembly_output = true;
      driver.code16 = true;
      continue;
    }
    if (strcmp(option, "-m64") == 0) {
      driver.require_x86_64 = true;
      continue;
    }
    if (strcmp(option, "-mno-sse") == 0 || strcmp(option, "-mno-sse2") == 0 ||
        strcmp(option, "-mno-mmx") == 0 || strcmp(option, "-mno-3dnow") == 0 ||
        strcmp(option, "-mno-avx") == 0 || strcmp(option, "-mno-sse4a") == 0 ||
        strcmp(option, "-mno-80387") == 0 ||
        strcmp(option, "-mno-fp-ret-in-387") == 0) {
      driver.require_x86 = true;
      driver.disable_vectorization = true;
      continue;
    }
    if (strcmp(option, "-march=x86-64") == 0) {
      driver.require_x86_64 = true;
      continue;
    }
    if (strcmp(option, "-mcmodel=kernel") == 0) {
      driver.require_x86_64 = true;
      /*
       * The x86-64 emitter uses RIP-relative symbol references, leaving the
       * kernel model's signed 2 GiB range check to the final linker.
       */
      continue;
    }
    if (strcmp(option, "-falign-functions=16") == 0) {
      /*
       * The x86-64 object emitter aligns function entries to 16 bytes. The
       * requested setting is therefore already the backend's enforced
       * function-alignment policy, not an ignored tuning option.
       */
      continue;
    }
    if (strcmp(option, "-mskip-rax-setup") == 0 && driver.disable_vectorization)
      continue;
    if (supported_by_default_flag(option))
      continue;
    if (option[0] == '-' && option[1]) {
      bool kernel_compat = getenv("NYC_KERNEL_COMPAT") != NULL;
      if (kernel_compat) {
        if (strncmp(option, "-mcmodel=", 9) == 0 &&
            strcmp(option, "-mcmodel=kernel") != 0 &&
            strcmp(option, "-mcmodel=small") != 0) {
          /*
           * Keep rejecting semantically unsupported code models.
           */
        } else if (strncmp(option, "-f", 2) == 0 ||
                   strncmp(option, "-m", 2) == 0 ||
                   strncmp(option, "--param=", 8) == 0 ||
                   strcmp(option, "-pipe") == 0 ||
                   strcmp(option, "-Qunused-arguments") == 0 ||
                   strncmp(option, "-fplugin", 8) == 0 ||
                   strncmp(option, "--param", 7) == 0)
          continue;
        if ((strcmp(option, "-isystem") == 0 ||
             strcmp(option, "-idirafter") == 0 ||
             strcmp(option, "-iquote") == 0) &&
            argument + 1 < argc) {
          ++argument;
          continue;
        }
        if (strncmp(option, "-isystem", 8) == 0 ||
            strncmp(option, "-idirafter", 10) == 0 ||
            strncmp(option, "-iquote", 7) == 0)
          continue;
      }
      fprintf(stderr, "nyc: unknown option '%s'\n", option);
      goto done;
    }
  }
  if (driver.print_file_name_plugin) {
    if (fputs("plugin\n", stdout) == EOF || fflush(stdout) != 0)
      result = 1;
    else
      result = 0;
    goto done;
  }
  if (driver.stack_protector) {
    const char *macro = driver.stack_protector == 3   ? "__SSP_ALL__=2"
                        : driver.stack_protector == 2 ? "__SSP_STRONG__=3"
                                                      : "__SSP__=1";
    if (!add_macro_option(&driver, macro, false)) {
      fputs("nyc: cannot store stack protector macro\n", stderr);
      result = 1;
      goto done;
    }
    nyc_macro_option_t builtin = driver.macros[driver.macro_count - 1];
    memmove(driver.macros + 1, driver.macros,
            (driver.macro_count - 1) * sizeof(*driver.macros));
    driver.macros[0] = builtin;
    if (!driver.compile_unit && !driver.preprocess) {
      fputs("nyc: stack protection requires whole-unit mode (-c or -S)\n",
            stderr);
      goto done;
    }
  }
  /*
   * Lower optimization pressure when Kbuild explicitly conserves stack.
   */
  if (driver.conserve_stack && driver.optimization > NYR_OPT_O1)
    driver.optimization = NYR_OPT_O1;
  if (object_mode && !driver.preprocess && !driver.output) {
    fputs("nyc: -c requires -o FILE\n", stderr);
    goto done;
  }
  if (driver.assembly_output && !driver.output) {
    fputs("nyc: -S requires -o FILE\n", stderr);
    goto done;
  }
  if (object_mode && driver.assembly_output) {
    fputs("nyc: -c and -S cannot be combined\n", stderr);
    goto done;
  }
  if (driver.cf_protection_branch && !driver.preprocess &&
      (!driver.compile_unit || !driver.assembly_output)) {
    fputs("nyc: -fcf-protection=branch currently requires -S output\n", stderr);
    goto done;
  }
  if (driver.omit_frame_pointer && !driver.compile_unit && !driver.preprocess) {
    fputs("nyc: -fomit-frame-pointer requires whole-unit object mode (-c)\n",
          stderr);
    goto done;
  }
  if (driver.position_independent && !driver.compile_unit &&
      !driver.preprocess) {
    fputs("nyc: -fPIC/-fPIE require whole-unit object mode (-c)\n", stderr);
    goto done;
  }
  if ((driver.retpoline_indirect_calls || driver.retpoline_inline_calls) &&
      !driver.compile_unit && !driver.preprocess) {
    fputs("nyc: retpoline options require whole-unit object mode (-c)\n",
          stderr);
    goto done;
  }
  if (driver.compile_unit && !driver.assembly_output && !driver.preprocess &&
      driver.output && operand_count == 2) {
    const char *input = operands[1];
    size_t input_len = strlen(input);
    if (input_len >= 2 && input[input_len - 2] == '.' &&
        input[input_len - 1] == 's') {
      result = nyc_assemble_source(input, driver.output, driver.target,
                                   driver.include_dirs, driver.include_count)
                   ? 0
                   : 1;
      goto done;
    }
  }
  result = compile_source(operand_count, operands, 1, &driver);
done:
  free(driver.include_dirs);
  free(driver.forced_includes);
  free(driver.macros);
  for (size_t i = 0; i < driver.path_map_count; ++i) {
    free((char *)driver.path_maps[i].from);
    free((char *)driver.path_maps[i].to);
  }
  free(driver.path_maps);
  return result;
}
