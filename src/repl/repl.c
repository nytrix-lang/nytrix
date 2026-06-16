#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/common.h"
#include "base/loader.h"
#include "base/util.h"
#include "code/code.h"
#include "code/jit.h"
#include "code/llvm.h"
#include "code/priv.h"
#include "parse/json.h"
#include "priv.h"
#include "repl/types.h"
#include "rt/runtime.h"
#include "rt/shared.h"
#include "wire/build.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
extern char **environ;
#endif

#include "repl/read.h"

#ifndef _WIN32
#include <setjmp.h>
typedef struct {
  int64_t fn;
  int64_t env;
} _rpd;
typedef struct {
  _rpd *data;
  size_t len;
  size_t cap;
} _rpdv;
extern _rpdv g_defer_stack;
#endif
typedef struct {
  jmp_buf *env;
  size_t defer_base;
} _rpe;
typedef struct {
  _rpe *data;
  size_t len;
  size_t cap;
} _rpev;
extern _rpev g_panic_env_stack;

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

const doc_list_t *g_repl_docs = NULL;
static VEC(char *) g_repl_loading_modules = {0};
static VEC(char *) g_repl_persistent_sources = {0};

static std_mode_t g_repl_std_override = STD_MODE_DEFAULT;
static int g_repl_has_std_override = 0;

static std_mode_t g_repl_effective_mode = STD_MODE_DEFAULT;
static int g_repl_plain = 0;
static int g_repl_max_errors = -1;
static int g_repl_exit_hook_registered = 0;

static LLVMContextRef g_repl_ctx = NULL;
static LLVMExecutionEngineRef g_repl_ee = NULL;
static codegen_t g_repl_cg = {0};
static int g_eval_count = 0;
static LLVMBuilderRef g_repl_builder = NULL;
static char *g_std_src_cached_persistent = NULL;
static char *g_repl_last_expand_source = NULL;
static char *g_repl_last_expand_name = NULL;
static int g_repl_timing = 0;
static int g_repl_phase_trace = 0;
static int g_repl_exec_trace_enabled = 0;
static int g_repl_exec_trace_values = 0;
static int g_repl_exec_trace_verbose = 0;
static int g_repl_exec_trace_calls = 0;
static int g_repl_exec_trace_compile = 0;
static int g_repl_exec_trace_json = 0;
static int g_repl_exec_trace_ir = 0;
static char *g_repl_exec_trace_filter = NULL;
static int g_repl_opt_level = 1;
static int g_repl_std_root_lazy = 0;
static int g_repl_lazy_docs_loaded = 0;
volatile sig_atomic_t g_repl_sigint = 0;
extern int g_trace_requested;
extern int g_trace_suspended;

static bool repl_fast_batch_exit_enabled(int batch_mode) {
  if (!batch_mode)
    return false;
  if (isatty(STDIN_FILENO))
    return false;
  return ny_env_enabled("NYTRIX_TEST_MODE") ||
         ny_env_enabled("NYTRIX_REPL_FAST_EXIT");
}

#ifndef _WIN32
static volatile sig_atomic_t g_repl_eval_active = 0;
static __attribute__((unused)) sigjmp_buf g_repl_eval_jmp;
#endif

static void repl_on_sigint(int sig) {
  (void)sig;
  g_repl_sigint = 1;
}

static void repl_ensure_module(const char *name, std_mode_t std_mode,
                               doc_list_t *docs);
static void repl_init_engine(std_mode_t mode, doc_list_t *docs);
static void repl_shutdown_engine(void);
static int repl_eval_snippet(const char *full_input, int is_stmt, char *an,
                             std_mode_t std_mode, int tty_in, doc_list_t *docs,
                             int from_init);
static int repl_print_namespace_help(doc_list_t *docs, const char *query);

static void repl_free_persistent_sources(void) {
  for (size_t i = 0; i < g_repl_persistent_sources.len; ++i)
    free(g_repl_persistent_sources.data[i]);
  vec_free(&g_repl_persistent_sources);
}

static void repl_debug_stage(const char *stage) {
  if (!getenv("NYTRIX_REPL_DEBUG_STAGE"))
    return;
  fprintf(stderr, "[repl-stage] %s\n", stage ? stage : "?");
  fflush(stderr);
}

static void repl_debug_ir(LLVMModuleRef mod) {
  if (!mod || !getenv("NYTRIX_REPL_DEBUG_IR"))
    return;
  char *ir = LLVMPrintModuleToString(mod);
  if (!ir)
    return;
  fprintf(stderr, "%s\n", ir);
  fflush(stderr);
  LLVMDisposeMessage(ir);
}

typedef struct {
  const char *name;
  const char *module;
} repl_lazy_std_hint_t;

static const repl_lazy_std_hint_t k_repl_lazy_std_hints[] = {
    {"abs", "std.math"},           {"sin", "std.math"},
    {"cos", "std.math"},           {"tan", "std.math"},
    {"asin", "std.math"},          {"acos", "std.math"},
    {"atan", "std.math"},          {"atan2", "std.math"},
    {"sqrt", "std.math"},          {"pow", "std.math"},
    {"exp", "std.math"},           {"log", "std.math"},
    {"log2", "std.math"},          {"log10", "std.math"},
    {"floor", "std.math"},         {"ceil", "std.math"},
    {"round", "std.math"},         {"fmod", "std.math"},
    {"min", "std.math"},           {"max", "std.math"},
    {"clamp", "std.math"},         {"clamp01", "std.math"},
    {"lerp", "std.math"},          {"gcd", "std.math"},
    {"lcm", "std.math"},           {"factorial", "std.math"},
    {"is_prime", "std.math.nt"},   {"next_prime", "std.math.nt"},
    {"prev_prime", "std.math.nt"}, {"range", "std.core"},
    {"assert", "std.core"},        {"assert_eq", "std.core"},
    {"to_str", "std.core"},        {NULL, NULL}};

static void repl_enable_lazy_std_root(std_mode_t std_mode, doc_list_t *docs) {
  (void)std_mode;
  (void)docs;
  g_repl_std_root_lazy = 1;
}

static void repl_normalize_doc_query(const char *raw, char *out,
                                     size_t out_cap) {
  if (!out || out_cap == 0)
    return;
  out[0] = '\0';
  if (!raw || !*raw)
    return;
  snprintf(out, out_cap, "%s", raw);
  size_t len = strlen(out);
  while (len > 0 && isspace((unsigned char)out[len - 1]))
    out[--len] = '\0';
  while (len > 0 && (out[len - 1] == ';' || out[len - 1] == ','))
    out[--len] = '\0';
  if (len >= 2 && out[len - 1] == ')' && out[len - 2] == '(') {
    out[len - 2] = '\0';
    len -= 2;
  }
  while (len > 0 && isspace((unsigned char)out[len - 1]))
    out[--len] = '\0';
}

static void repl_set_last_expand_source(const char *source, const char *name) {
  free(g_repl_last_expand_source);
  g_repl_last_expand_source = source ? ny_strdup(source) : NULL;
  free(g_repl_last_expand_name);
  g_repl_last_expand_name = name ? ny_strdup(name) : NULL;
}

static void repl_print_pretty_json(const char *json, const char *title) {
  if (title && *title)
    printf("%s%s%s\n", clr(NY_CLR_BOLD NY_CLR_CYAN), title, clr(NY_CLR_RESET));
  if (!json || !*json) {
    printf("  %s(empty)%s\n", clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
    return;
  }
  int indent = 0;
  int in_str = 0;
  int escape = 0;
  int need_indent = 1;
  printf("  ");
  for (const unsigned char *p = (const unsigned char *)json; *p; ++p) {
    unsigned char ch = *p;
    if (in_str) {
      putchar((int)ch);
      if (escape)
        escape = 0;
      else if (ch == '\\')
        escape = 1;
      else if (ch == '"')
        in_str = 0;
      continue;
    }
    switch (ch) {
    case '"':
      if (need_indent) {
        for (int i = 0; i < indent; ++i)
          printf("  ");
        need_indent = 0;
      }
      in_str = 1;
      putchar('"');
      break;
    case '{':
    case '[':
      if (need_indent) {
        for (int i = 0; i < indent; ++i)
          printf("  ");
        need_indent = 0;
      }
      putchar((int)ch);
      putchar('\n');
      indent++;
      need_indent = 1;
      break;
    case '}':
    case ']':
      putchar('\n');
      indent--;
      if (indent < 0)
        indent = 0;
      for (int i = 0; i < indent; ++i)
        printf("  ");
      putchar((int)ch);
      need_indent = 0;
      break;
    case ',':
      putchar(',');
      putchar('\n');
      need_indent = 1;
      break;
    case ':':
      printf(": ");
      break;
    default:
      if (!isspace(ch)) {
        if (need_indent) {
          for (int i = 0; i < indent; ++i)
            printf("  ");
          need_indent = 0;
        }
        putchar((int)ch);
      }
      break;
    }
  }
  putchar('\n');
}

static void repl_print_llvm_ir(const char *module_ir, const char *fn_name) {
  if (!module_ir || !*module_ir || !fn_name || !*fn_name)
    return;
  char needle[128];
  snprintf(needle, sizeof(needle), "@%s(", fn_name);
  const char *section = NULL;
  const char *line = module_ir;
  while (*line) {
    const char *next = strchr(line, '\n');
    size_t len = next ? (size_t)(next - line) : strlen(line);
    if (len >= 7 && ny_memmem(line, len, "define ", 7) &&
        ny_memmem(line, len, needle, strlen(needle))) {
      section = line;
      break;
    }
    if (!next)
      break;
    line = next + 1;
  }
  printf("%sllvm-ir%s\n", clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET));
  if (!section) {
    printf("  %s(no eval function body found for %s)%s\n\n", clr(NY_CLR_GRAY),
           fn_name, clr(NY_CLR_RESET));
    return;
  }
  line = section;
  int line_count = 0;
  while (*line) {
    const char *next = strchr(line, '\n');
    size_t len = next ? (size_t)(next - line) : strlen(line);
    printf("  %.*s\n", (int)len, line);
    line_count++;
    if (len == 1 && line[0] == '}')
      break;
    if (!next)
      break;
    line = next + 1;
    if (line_count >= 160) {
      printf("  %s... truncated ...%s\n", clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
      break;
    }
  }
  putchar('\n');
}

static int repl_expand_source(const char *source, const char *source_name,
                              const char *filter, int meta_trace,
                              int include_json, int include_ir) {
  if (!source || !*source) {
    printf("No source available to expand.\n");
    return 1;
  }
  parser_t parser;
  parser_init(&parser, source, source_name ? source_name : "<repl_expand>");
  program_t prog = parse_program(&parser);
  if (parser.had_error) {
    repl_print_error_snippet(source, parser.cur.line, parser.cur.col);
    program_free(&prog, parser.arena);
    return 1;
  }

  bool single_expr = prog.body.len == 1 && prog.body.data[0] &&
                     prog.body.data[0]->kind == NY_S_EXPR;
  printf("%sExpand%s %s\n", clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET),
         source_name ? source_name : "<repl>");
  printf(
      "%s----------------------------------------------------------------%s\n",
      clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
  printf("%smode%s    %s\n", clr(NY_CLR_GRAY), clr(NY_CLR_RESET),
         include_json
             ? (meta_trace ? "graph + trace + ast-json" : "graph + ast-json")
             : (meta_trace ? "graph + trace" : "graph"));
  printf("%ssource%s  %s\n", clr(NY_CLR_GRAY), clr(NY_CLR_RESET),
         source_name ? source_name : "<repl>");
  if (filter && *filter)
    printf("%sfilter%s  %s\n", clr(NY_CLR_GRAY), clr(NY_CLR_RESET), filter);
  if (include_ir)
    printf("%sir%s      on\n", clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
  printf("\n");

  if (single_expr) {
    char *expr_json = ny_expr_to_json(prog.body.data[0]->as.expr.expr);
    repl_print_pretty_json(expr_json, "expr-ast");
    printf("\n");
    if (expr_json)
      rt_free((int64_t)(uintptr_t)expr_json);
  } else {
    char *ast_json = ny_ast_to_json_filtered(
        &prog, source_name ? source_name : "<repl_expand>");
    repl_print_pretty_json(ast_json, "ast");
    printf("\n");
    if (ast_json)
      rt_free((int64_t)(uintptr_t)ast_json);
  }

  char *report = ny_ast_expand_report(
      &prog, source_name ? source_name : "<repl_expand>",
      filter && *filter ? filter : NULL, NULL, meta_trace, include_json);
  if (report && *report) {
    const char *body = report;
    const char *first_nl = strchr(report, '\n');
    if (first_nl) {
      body = first_nl + 1;
      const char *second_nl = strchr(body, '\n');
      if (second_nl &&
          strncmp(body, "--------------------------------", 32) == 0)
        body = second_nl + 1;
    }
    printf("%s", body);
    size_t report_len = strlen(body);
    if (report_len == 0 || body[report_len - 1] != '\n')
      printf("\n");
    rt_free((int64_t)(uintptr_t)report);
  }

  program_free(&prog, parser.arena);
  return 0;
}

static void repl_setenv_force(const char *key, const char *value) {
  if (!key || !*key)
    return;
#ifdef _WIN32
  _putenv_s(key, value ? value : "");
#else
  if (value)
    ny_setenv(key, value, 1);
  else
    ny_unsetenv(key);
#endif
}

static void repl_unsetenv_force(const char *key) {
  if (!key || !*key)
    return;
#ifdef _WIN32
  _putenv_s(key, "");
#else
  ny_unsetenv(key);
#endif
}

static char *repl_dup_env_value(const char *key) {
  const char *value = getenv(key);
  return value ? ny_strdup(value) : NULL;
}

static void repl_set_exec_trace_filter(const char *filter) {
  free(g_repl_exec_trace_filter);
  g_repl_exec_trace_filter = (filter && *filter) ? ny_strdup(filter) : NULL;
}

static void repl_apply_exec_trace_env(void) {
  if (g_repl_exec_trace_enabled) {
    repl_setenv_force("NYTRIX_TRACE", "1");
    repl_setenv_force("NYTRIX_TRACE_CALLS",
                      g_repl_exec_trace_calls ? "1" : "0");
    if (g_repl_exec_trace_values)
      repl_setenv_force("NYTRIX_TRACE_VALUES", "1");
    else
      repl_unsetenv_force("NYTRIX_TRACE_VALUES");
    if (g_repl_exec_trace_verbose)
      repl_setenv_force("NYTRIX_TRACE_VERBOSE", "1");
    else
      repl_unsetenv_force("NYTRIX_TRACE_VERBOSE");
    if (g_repl_exec_trace_filter && *g_repl_exec_trace_filter)
      repl_setenv_force("NYTRIX_TRACE_FILTER", g_repl_exec_trace_filter);
    else
      repl_unsetenv_force("NYTRIX_TRACE_FILTER");
    g_trace_requested = 1;
  } else {
    repl_unsetenv_force("NYTRIX_TRACE");
    repl_unsetenv_force("NYTRIX_TRACE_CALLS");
    repl_unsetenv_force("NYTRIX_TRACE_VALUES");
    repl_unsetenv_force("NYTRIX_TRACE_VERBOSE");
    repl_unsetenv_force("NYTRIX_TRACE_FILTER");
    g_trace_requested = 0;
  }
  rt_trace_refresh_env();
}

static void repl_print_trace_status(void) {
  if (!g_repl_exec_trace_enabled) {
    if (g_repl_exec_trace_compile)
      printf("Trace: compiler-only");
    else
      printf("Trace: off");
  } else {
    const char *mode = g_repl_exec_trace_compile
                           ? (g_repl_exec_trace_json ? "full" : "compiler")
                       : g_repl_exec_trace_verbose ? "verbose"
                       : g_repl_exec_trace_values  ? "values"
                                                   : "calls";
    printf("Trace: on");
    printf("  mode=%s", mode);
    if (g_repl_exec_trace_filter && *g_repl_exec_trace_filter)
      printf("  filter=%s", g_repl_exec_trace_filter);
  }
  if (g_repl_exec_trace_ir)
    printf("  ir=on");
  if (g_repl_exec_trace_compile && !g_repl_exec_trace_enabled &&
      g_repl_exec_trace_filter && *g_repl_exec_trace_filter)
    printf("  filter=%s", g_repl_exec_trace_filter);
  printf("  phase=%s\n", g_repl_phase_trace ? "on" : "off");
}

static int repl_rebuild_engine_from_persistent(std_mode_t std_mode,
                                               doc_list_t *docs) {
  char *saved = (g_repl_user_source && *g_repl_user_source)
                    ? ny_strdup(g_repl_user_source)
                    : NULL;
  g_repl_std_root_lazy = 0;
  repl_shutdown_engine();
  repl_init_engine(std_mode, docs);
  if (saved && *saved) {
    int status = repl_eval_snippet(saved, 1, NULL, std_mode, 0, docs, 1);
    free(saved);
    return status;
  }
  free(saved);
  return 0;
}

static void repl_copy_str_list(ny_str_list *dst, const ny_str_list *src) {
  if (!dst || !src)
    return;
  memset(dst, 0, sizeof(*dst));
  for (size_t i = 0; i < src->len; i++)
    vec_push(dst, src->data[i] ? ny_strdup(src->data[i]) : NULL);
}

static fun_sig repl_owned_fun_sig_copy(const fun_sig *src) {
  fun_sig dst = *src;
  dst.name = src->name ? ny_strdup(src->name) : NULL;
  dst.module_name = src->module_name ? ny_strdup(src->module_name) : NULL;
  dst.source_file = src->source_file ? ny_strdup(src->source_file) : NULL;
  dst.link_name = src->link_name ? ny_strdup(src->link_name) : NULL;
  dst.return_type = src->return_type ? ny_strdup(src->return_type) : NULL;
  dst.abi_return_type =
      src->abi_return_type ? ny_strdup(src->abi_return_type) : NULL;
  dst.inferred_return_type =
      src->inferred_return_type ? ny_strdup(src->inferred_return_type) : NULL;
  repl_copy_str_list(&dst.param_types, &src->param_types);
  dst.returns_borrow =
      src->returns_borrow ? ny_strdup(src->returns_borrow) : NULL;
  repl_copy_str_list(&dst.borrows, &src->borrows);
  repl_copy_str_list(&dst.consumes, &src->consumes);
  repl_copy_str_list(&dst.mutates, &src->mutates);
  repl_copy_str_list(&dst.releases, &src->releases);
  repl_copy_str_list(&dst.forgets, &src->forgets);
  dst.stmt_t = NULL;
  dst.owned = true;
  return dst;
}

static layout_def_t *repl_owned_layout_copy(const layout_def_t *src) {
  if (!src)
    return NULL;
  layout_def_t *dst = malloc(sizeof(*dst));
  if (!dst)
    return NULL;
  *dst = *src;
  dst->name = src->name ? ny_strdup(src->name) : NULL;
  memset(&dst->fields, 0, sizeof(dst->fields));
  for (size_t i = 0; i < src->fields.len; i++) {
    layout_field_info_t field = src->fields.data[i];
    field.name = field.name ? ny_strdup(field.name) : NULL;
    field.type_name = field.type_name ? ny_strdup(field.type_name) : NULL;
    vec_push(&dst->fields, field);
  }
  dst->stmt = NULL;
  dst->heap_allocated = true;
  return dst;
}

static void repl_rebind_persistent_symbols(codegen_t *cg) {
  if (!cg || !cg->module)
    return;
  for (size_t i = 0; i < cg->fun_sigs.len; i++) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig->type || !sig->name || !*sig->name)
      continue;
    const char *link_name =
        (sig->link_name && *sig->link_name) ? sig->link_name : sig->name;
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, link_name);
    if (!fn)
      fn = LLVMAddFunction(cg->module, link_name, sig->type);
    sig->value = fn;
  }
  for (size_t i = 0; i < cg->global_vars.len; i++) {
    binding *b = &cg->global_vars.data[i];
    if (!b->name || !*b->name)
      continue;
    LLVMTypeRef ty = cg->type_i64;
    if (b->is_f64_slot)
      ty = cg->type_f64;
    else if (b->is_f32_slot)
      ty = cg->type_f32;
    LLVMValueRef gv = LLVMGetNamedGlobal(cg->module, b->name);
    if (!gv) {
      gv = LLVMAddGlobal(cg->module, ty, b->name);
      LLVMSetLinkage(gv, LLVMExternalLinkage);
    }
    b->value = gv;
  }
}

typedef struct repl_pending_fn_mapping_t {
  LLVMValueRef fn;
  uint64_t addr;
  bool trampoline_defined;
} repl_pending_fn_mapping_t;

static repl_pending_fn_mapping_t *
repl_collect_existing_jit_function_mappings(LLVMExecutionEngineRef ee,
                                            LLVMModuleRef mod,
                                            codegen_t *cg,
                                            size_t *out_len) {
  if (out_len)
    *out_len = 0;
  if (!ee || !mod || !out_len)
    return NULL;
  repl_pending_fn_mapping_t *items = NULL;
  size_t len = 0;
  size_t cap = 0;
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    if (!LLVMIsDeclaration(f))
      continue;
    const char *name = LLVMGetValueName(f);
    if (!name || !*name || strncmp(name, "llvm.", 5) == 0)
      continue;
    bool std_decl = strncmp(name, "std.", 4) == 0;
    if (!std_decl && !LLVMGetFirstUse(f))
      continue;
    const char *tail = strrchr(name, '.');
    if (ny_jit_resolve_symbol(name) ||
        (!std_decl && tail && tail[1] && ny_jit_resolve_symbol(tail + 1)) ||
        strncmp(name, "__", 2) == 0 || strncmp(name, "rt_", 3) == 0 ||
        strncmp(name, "ny_", 3) == 0 || strcmp(name, "_setjmp") == 0)
      continue;
    if (getenv("NYTRIX_REPL_DEBUG_STAGE")) {
      fprintf(stderr, "[repl-stage] collect-fn %s\n", name);
      fflush(stderr);
    }
    uint64_t addr = LLVMGetFunctionAddress(ee, name);
    if (!addr && cg) {
      const char *resolved = ny_resolve_used_module_export_alias(cg, name);
      if (!resolved || !*resolved || strcmp(resolved, name) == 0)
        resolved = resolve_import_alias(cg, name);
      if (resolved && *resolved && strcmp(resolved, name) != 0) {
        addr = LLVMGetFunctionAddress(ee, resolved);
        if (addr && getenv("NYTRIX_REPL_DEBUG_STAGE")) {
          fprintf(stderr, "[repl-stage] map-fn-alias %s -> %s\n", name,
                  resolved);
          fflush(stderr);
        }
      }
    }
    if (!addr)
      continue;
    LLVMAddSymbol(name, (void *)(uintptr_t)addr);
    if (getenv("NYTRIX_REPL_DEBUG_STAGE")) {
      fprintf(stderr, "[repl-stage] map-fn %s=0x%llx\n", name,
              (unsigned long long)addr);
      fflush(stderr);
    }
    if (len == cap) {
      size_t next_cap = cap ? cap * 2 : 16;
      repl_pending_fn_mapping_t *next =
          realloc(items, next_cap * sizeof(*items));
      if (!next) {
        free(items);
        *out_len = 0;
        return NULL;
      }
      items = next;
      cap = next_cap;
    }
    items[len++] = (repl_pending_fn_mapping_t){.fn = f, .addr = addr};
  }
  *out_len = len;
  return items;
}

static bool repl_define_jit_function_trampoline(LLVMValueRef fn,
                                                uint64_t addr) {
  if (!fn || !addr || !LLVMIsDeclaration(fn))
    return false;
  LLVMModuleRef mod = LLVMGetGlobalParent(fn);
  if (!mod)
    return false;
  LLVMTypeRef fty = LLVMGlobalGetValueType(fn);
  if (!fty || LLVMGetTypeKind(fty) != LLVMFunctionTypeKind)
    return false;
  unsigned argc = LLVMCountParams(fn);
  if (argc > 64)
    return false;
  LLVMContextRef ctx = LLVMGetModuleContext(mod);
  LLVMBuilderRef b = LLVMCreateBuilderInContext(ctx);
  LLVMBasicBlockRef bb =
      LLVMAppendBasicBlockInContext(ctx, fn, "repl_jit_trampoline");
  LLVMPositionBuilderAtEnd(b, bb);
  LLVMValueRef args[64];
  for (unsigned i = 0; i < argc; ++i)
    args[i] = LLVMGetParam(fn, i);
  LLVMValueRef raw =
      LLVMConstInt(LLVMInt64TypeInContext(ctx), addr, false);
  LLVMValueRef callee =
      LLVMBuildIntToPtr(b, raw, LLVMTypeOf(fn), "repl_jit_target");
  LLVMValueRef ret =
      LLVMBuildCall2(b, fty, callee, args, argc, "repl_jit_result");
  LLVMTypeRef ret_ty = LLVMGetReturnType(fty);
  if (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind)
    LLVMBuildRetVoid(b);
  else
    LLVMBuildRet(b, ret);
  LLVMDisposeBuilder(b);
  return true;
}

static void repl_define_existing_jit_function_trampolines(
    repl_pending_fn_mapping_t *items, size_t len) {
#ifdef __APPLE__
  if (!items)
    return;
  for (size_t i = 0; i < len; ++i) {
    if (items[i].fn && items[i].addr)
      items[i].trampoline_defined =
          repl_define_jit_function_trampoline(items[i].fn, items[i].addr);
  }
#else
  (void)items;
  (void)len;
#endif
}

static void repl_apply_existing_jit_function_mappings(
    LLVMExecutionEngineRef ee, repl_pending_fn_mapping_t *items, size_t len) {
  if (!ee || !items)
    return;
  for (size_t i = 0; i < len; i++) {
    if (items[i].trampoline_defined)
      continue;
    if (items[i].fn && items[i].addr)
      LLVMAddGlobalMapping(ee, items[i].fn,
                           (void *)(uintptr_t)items[i].addr);
  }
}

static int repl_print_help_query(doc_list_t *docs, const char *query) {
  if (!docs || !query || !*query)
    return 0;
  repl_ensure_docs_for_query(docs, query);
  int printed = repl_print_namespace_help(docs, query);
  if (!printed)
    printed = doclist_print(docs, query);
  if (!printed && !strchr(query, '.')) {
    for (size_t i = 0; i < ny_std_module_count(); ++i)
      repl_load_module_docs(docs, ny_std_module_name(i));
    printed = repl_print_namespace_help(docs, query);
    if (!printed)
      printed = doclist_print(docs, query);
  }
  return printed;
}

static void repl_print_panic_value(int64_t msg) {
  rt_print_flush();
  if (is_int(msg)) {
    fprintf(stderr, "Panic (int): %lld\n", (long long)rt_untag_v(msg));
  } else if (is_v_str(msg)) {
    const char *s = (const char *)(uintptr_t)msg;
    size_t len = rt_tagged_str_len(msg);
    fprintf(stderr, "Panic: %.*s\n", (int)len, s);
  } else if (is_v_err(msg)) {
    fprintf(stderr, "Panic (err): ");
    repl_print_panic_value(rt_unwrap(msg));
  } else {
    fprintf(stderr, "Panic (raw): 0x%llx\n", (unsigned long long)msg);
  }
}

static bool repl_name_suffix_matches(const char *full, const char *suffix) {
  if (!full || !suffix || !*full || !*suffix)
    return false;
  if (strcmp(full, suffix) == 0)
    return true;
  size_t full_len = strlen(full);
  size_t suffix_len = strlen(suffix);
  return full_len > suffix_len && full[full_len - suffix_len - 1] == '.' &&
         strcmp(full + full_len - suffix_len, suffix) == 0;
}

static char *repl_module_name_for_query(const char *query) {
  if (!query || !*query)
    return NULL;
  if (ny_std_find_module_by_name(query) >= 0)
    return ny_strdup(query);
  char *probe = ny_strdup(query);
  if (!probe)
    return NULL;
  while (1) {
    char *dot = strrchr(probe, '.');
    if (!dot)
      break;
    *dot = '\0';
    if (*probe && ny_std_find_module_by_name(probe) >= 0)
      return probe;
  }
  free(probe);
  return NULL;
}

void repl_ensure_docs_for_query(doc_list_t *docs, const char *query) {
  if (!docs || !query || !*query)
    return;
  char *module = repl_module_name_for_query(query);
  if (module) {
    repl_load_module_docs(docs, module);
    free(module);
    return;
  }
  const char *dot = strrchr(query, '.');
  if (!dot) {
    for (size_t i = 0; i < g_repl_cg.user_use_modules.len; ++i)
      repl_load_module_docs(docs, g_repl_cg.user_use_modules.data[i]);
    return;
  }
  size_t prefix_len = (size_t)(dot - query);
  if (prefix_len == 0 || prefix_len >= 512)
    return;
  char prefix[512];
  memcpy(prefix, query, prefix_len);
  prefix[prefix_len] = '\0';
  for (size_t i = 0; i < g_repl_cg.user_use_modules.len; ++i) {
    const char *used = g_repl_cg.user_use_modules.data[i];
    if (repl_name_suffix_matches(used, prefix))
      repl_load_module_docs(docs, used);
  }
}

static int repl_program_has_bare_std_use(program_t *prog) {
  if (!prog)
    return 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    stmt_t *s = prog->body.data[i];
    if (ny_stmt_is_bare_std_use(s))
      return 1;
  }
  return 0;
}

static int repl_lazy_std_hint(const char *name, const char **out_module) {
  if (!name || !*name)
    return 0;
  for (int i = 0; k_repl_lazy_std_hints[i].name; ++i) {
    if (strcmp(k_repl_lazy_std_hints[i].name, name) == 0) {
      if (out_module)
        *out_module = k_repl_lazy_std_hints[i].module;
      return 1;
    }
  }
  return 0;
}

static int repl_lazy_ident_ignored(const char *name) {
  if (!name || !*name)
    return 1;
  static const char *const ignored[] = {
      "fn",       "def",    "mut",     "if",     "else",     "elif",
      "while",    "for",    "in",      "return", "break",    "continue",
      "use",      "module", "as",      "lambda", "defer",    "try",
      "catch",    "throw",  "finally", "case",   "match",    "enum",
      "struct",   "layout", "extern",  "embed",  "comptime", "impl",
      "operator", "self",   "true",    "false",  "nil",      "none",
      "del",      "export", "type",    "std",    "int",      "float",
      "number",   "str",    "list",    "dict",   "set",      "tuple",
      "bool",     "any",    "bytes",   "ptr",    "handle",
      "fnptr",    NULL};
  for (int i = 0; ignored[i]; ++i) {
    if (strcmp(ignored[i], name) == 0)
      return 1;
  }
  return 0;
}

static void repl_lazy_load_all_std_docs(doc_list_t *docs) {
  if (!docs || g_repl_lazy_docs_loaded)
    return;
  for (size_t i = 0; i < ny_std_module_count(); ++i)
    repl_load_module_docs(docs, ny_std_module_name(i));
  g_repl_lazy_docs_loaded = 1;
}

static char *repl_lazy_module_prefix_for_doc_name(const char *name) {
  if (!name || strncmp(name, "std.", 4) != 0)
    return NULL;
  char buf[512];
  int nw = snprintf(buf, sizeof(buf), "%s", name);
  if (nw <= 0 || (size_t)nw >= sizeof(buf))
    return NULL;
  while (1) {
    char *dot = strrchr(buf, '.');
    if (!dot)
      return NULL;
    *dot = '\0';
    if (ny_std_find_module_by_name(buf) >= 0)
      return ny_strdup(buf);
  }
}

static int repl_lazy_module_score(const char *module) {
  if (!module || !*module)
    return -1000000;
  int score = 10000 - (int)strlen(module) * 4;
  int depth = 0;
  for (const char *p = module; *p; ++p) {
    if (*p == '.')
      depth++;
  }
  score -= depth * 80;
  if (strcmp(module, "std.math") == 0)
    score += 900;
  if (strncmp(module, "std.core", 8) == 0)
    score += 200;
  if (strstr(module, ".crypto."))
    score -= 700;
  if (strstr(module, ".ui."))
    score -= 700;
  return score;
}

static int repl_doc_leaf_equals(const char *full, const char *leaf) {
  if (!full || !leaf || !*leaf)
    return 0;
  const char *dot = strrchr(full, '.');
  const char *got = dot ? dot + 1 : full;
  return strcmp(got, leaf) == 0;
}

static const char *repl_name_leaf(const char *name) {
  if (!name || !*name)
    return NULL;
  const char *dot = strrchr(name, '.');
  return dot ? dot + 1 : name;
}

static char *repl_lazy_std_module_for_leaf(doc_list_t *docs, const char *leaf) {
  if (!docs || !leaf || !*leaf || repl_lazy_ident_ignored(leaf))
    return NULL;
  for (size_t i = 0; i < docs->len; ++i) {
    const ny_doc_entry *e = &docs->data[i];
    if (!e->name || strncmp(e->name, "std.", 4) == 0)
      continue;
    if ((e->kind == 3 || e->kind == 4) && strcmp(e->name, leaf) == 0)
      return NULL;
  }
  const char *hint_module = NULL;
  if (repl_lazy_std_hint(leaf, &hint_module) && hint_module &&
      ny_std_find_module_by_name(hint_module) >= 0)
    return ny_strdup(hint_module);
  return NULL;
}

static char *repl_lazy_std_module_for_member_chain(doc_list_t *docs,
                                                   const char *chain) {
  (void)docs;
  if (!chain || !*chain || !strchr(chain, '.'))
    return NULL;
  if (strncmp(chain, "std.", 4) == 0)
    return NULL;
  const char *dot = strchr(chain, '.');
  if (!dot || dot == chain || !dot[1] || strchr(dot + 1, '.'))
    return NULL;
  size_t alias_len = (size_t)(dot - chain);
  struct alias_map_t {
    const char *alias;
    const char *module;
  };
  static const struct alias_map_t aliases[] = {
      {"math", "std.math"},     {"nt", "std.math.nt"},
      {"bin", "std.math.bin"},  {"str", "std.core.str"},
      {"os", "std.os"},         {NULL, NULL},
  };
  for (int i = 0; aliases[i].alias; ++i) {
    if (strlen(aliases[i].alias) == alias_len &&
        strncmp(chain, aliases[i].alias, alias_len) == 0 &&
        ny_std_find_module_by_name(aliases[i].module) >= 0)
      return ny_strdup(aliases[i].module);
  }
  return NULL;
}

static int repl_lazy_std_rewrite_for_full_chain(doc_list_t *docs,
                                                const char *chain, char *out,
                                                size_t out_cap) {
  (void)docs;
  if (!chain || strncmp(chain, "std.", 4) != 0 || !out || out_cap == 0)
    return 0;
  struct rewrite_map_t {
    const char *prefix;
    const char *alias;
  };
  static const struct rewrite_map_t rewrites[] = {
      {"std.math.nt.", "nt."},      {"std.math.bin.", "bin."},
      {"std.math.", "math."},      {"std.core.str.", "str."},
      {"std.os.", "os."},          {"std.core.", ""},
      {NULL, NULL},
  };
  for (int i = 0; rewrites[i].prefix; ++i) {
    size_t n = strlen(rewrites[i].prefix);
    if (strncmp(chain, rewrites[i].prefix, n) != 0 || !chain[n])
      continue;
    int wrote = snprintf(out, out_cap, "%s%s", rewrites[i].alias, chain + n);
    return wrote > 0 && (size_t)wrote < out_cap;
  }
  return 0;
}

static int repl_seen_name(const char *seen, size_t stride, int seen_len,
                          const char *name) {
  for (int i = 0; i < seen_len; ++i) {
    if (strcmp(seen + (size_t)i * stride, name) == 0)
      return 1;
  }
  return 0;
}

static int repl_reserve_char_capacity(char **out, size_t *cap, size_t need,
                                      size_t initial_cap) {
  if (!out || !cap)
    return 0;
  if (need <= *cap)
    return 1;
  size_t new_cap = *cap ? *cap : (initial_cap ? initial_cap : 256u);
  while (new_cap < need) {
    if (new_cap > (size_t)-1 / 2u)
      return 0;
    new_cap *= 2u;
  }
  char *grown = realloc(*out, new_cap);
  if (!grown)
    return 0;
  *out = grown;
  *cap = new_cap;
  return 1;
}

static int repl_lazy_append_bytes(char **out, size_t *len, size_t *cap,
                                  const char *src, size_t src_len) {
  if (!out || !len || !cap || (!src && src_len > 0))
    return 0;
  size_t need = *len + src_len + 1;
  if (!repl_reserve_char_capacity(out, cap, need, 256))
    return 0;
  if (src_len)
    memcpy(*out + *len, src, src_len);
  *len += src_len;
  (*out)[*len] = '\0';
  return 1;
}

static int repl_lazy_append_use(char **out, size_t *len, size_t *cap,
                                const char *module) {
  if (!out || !len || !cap || !module || !*module)
    return 0;
  size_t module_len = strlen(module);
  size_t need = *len + 4 + module_len + 1 + 1;
  if (!repl_reserve_char_capacity(out, cap, need, 128))
    return 0;
  memcpy(*out + *len, "use ", 4);
  *len += 4;
  memcpy(*out + *len, module, module_len);
  *len += module_len;
  (*out)[(*len)++] = '\n';
  (*out)[*len] = '\0';
  return 1;
}

static int repl_lazy_skip_comment_or_string(const char **cursor) {
  if (!cursor || !*cursor)
    return 0;
  const char *p = *cursor;
  if (*p == ';' || *p == '#') {
    while (*p && *p != '\n')
      p++;
    *cursor = p;
    return 1;
  }
  if (*p == '"' || *p == '\'') {
    char q = *p++;
    while (*p) {
      if (*p == '\\' && p[1]) {
        p += 2;
        continue;
      }
      if (*p++ == q)
        break;
    }
    *cursor = p;
    return 1;
  }
  return 0;
}

static int repl_lazy_read_ident_chain(const char *start, char *out,
                                      size_t out_cap, const char **out_end,
                                      int *out_segments) {
  if (out_end)
    *out_end = start;
  if (out_segments)
    *out_segments = 0;
  if (!start || !out || out_cap == 0 ||
      !(isalpha((unsigned char)*start) || *start == '_'))
    return 0;

  const char *p = start;
  size_t out_len = 0;
  int segments = 0;
  while (1) {
    const char *seg = p;
    if (!(isalpha((unsigned char)*seg) || *seg == '_'))
      break;
    const char *seg_end = seg + 1;
    while (isalnum((unsigned char)*seg_end) || *seg_end == '_')
      seg_end++;
    size_t seg_len = (size_t)(seg_end - seg);
    size_t extra = (segments > 0 ? 1u : 0u) + seg_len;
    if (out_len + extra >= out_cap)
      return 0;
    if (segments > 0)
      out[out_len++] = '.';
    memcpy(out + out_len, seg, seg_len);
    out_len += seg_len;
    out[out_len] = '\0';
    segments++;
    p = seg_end;
    if (*p != '.' || !(isalpha((unsigned char)p[1]) || p[1] == '_'))
      break;
    p++;
  }
  if (out_end)
    *out_end = p;
  if (out_segments)
    *out_segments = segments;
  return segments > 0;
}

static char *repl_rewrite_lazy_std_qualified_calls(const char *src,
                                                   doc_list_t *docs) {
  if (!src || !*src || !docs || !g_repl_std_root_lazy)
    return NULL;
  char *out = NULL;
  size_t out_len = 0;
  size_t out_cap = 0;
  int changed = 0;
  const char *chunk = src;
  const char *p = src;
  while (*p) {
    if (repl_lazy_skip_comment_or_string(&p))
      continue;
    int before_boundary =
        p == src || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
    if (!before_boundary || strncmp(p, "std.", 4) != 0) {
      p++;
      continue;
    }
    char chain[256];
    const char *q = p;
    int segments = 0;
    if (!repl_lazy_read_ident_chain(p, chain, sizeof(chain), &q, &segments) ||
        segments < 2) {
      p++;
      continue;
    }
    const char *after_chain = q;
    while (*after_chain && isspace((unsigned char)*after_chain))
      after_chain++;
    if (*after_chain != '(') {
      p++;
      continue;
    }
    char replacement[160];
    if (!repl_lazy_std_rewrite_for_full_chain(docs, chain, replacement,
                                              sizeof(replacement))) {
      p++;
      continue;
    }
    if (!repl_lazy_append_bytes(&out, &out_len, &out_cap, chunk,
                                (size_t)(p - chunk)) ||
        !repl_lazy_append_bytes(&out, &out_len, &out_cap, replacement,
                                strlen(replacement))) {
      free(out);
      return NULL;
    }
    changed = 1;
    p = q;
    chunk = p;
  }
  if (!changed) {
    free(out);
    return NULL;
  }
  if (!repl_lazy_append_bytes(&out, &out_len, &out_cap, chunk, strlen(chunk))) {
    free(out);
    return NULL;
  }
  return out;
}

static int repl_line_is_bare_std_use(const char *line, const char *line_end) {
  if (!line || !line_end || line > line_end)
    return 0;
  const char *p = line;
  while (p < line_end && (*p == ' ' || *p == '\t' || *p == '\r'))
    p++;
  if (p >= line_end || *p == ';' || *p == '#')
    return 0;
  if ((size_t)(line_end - p) < 3 || strncmp(p, "use", 3) != 0)
    return 0;
  if (p + 3 < line_end && (isalnum((unsigned char)p[3]) || p[3] == '_'))
    return 0;
  const char *q = p + 3;
  if (q >= line_end || !isspace((unsigned char)*q))
    return 0;
  while (q < line_end && isspace((unsigned char)*q))
    q++;
  if ((size_t)(line_end - q) < 3 || strncmp(q, "std", 3) != 0)
    return 0;
  if (q + 3 < line_end) {
    unsigned char next = (unsigned char)q[3];
    if (isalnum(next) || next == '_' || next == '.')
      return 0;
  }
  q += 3;
  while (q < line_end && (*q == ' ' || *q == '\t' || *q == '\r'))
    q++;
  return q >= line_end || *q == ';' || *q == '#';
}

static char *repl_strip_bare_std_use_statements(const char *src) {
  if (!src || !*src)
    return NULL;
  char *out = NULL;
  size_t out_len = 0;
  size_t out_cap = 0;
  int changed = 0;
  const char *p = src;
  while (*p) {
    const char *line = p;
    const char *line_end = line;
    while (*line_end && *line_end != '\n')
      line_end++;
    int has_newline = *line_end == '\n';
    if (repl_line_is_bare_std_use(line, line_end)) {
      changed = 1;
      if (has_newline &&
          !repl_lazy_append_bytes(&out, &out_len, &out_cap, "\n", 1)) {
        free(out);
        return NULL;
      }
    } else {
      size_t len = (size_t)(line_end - line) + (has_newline ? 1u : 0u);
      if (!repl_lazy_append_bytes(&out, &out_len, &out_cap, line, len)) {
        free(out);
        return NULL;
      }
    }
    p = has_newline ? line_end + 1 : line_end;
  }
  if (!changed) {
    free(out);
    return NULL;
  }
  if (!out && !repl_lazy_append_bytes(&out, &out_len, &out_cap, "", 0))
    return NULL;
  return out;
}

static int repl_source_has_bare_std_use_text(const char *src) {
  if (!src)
    return 0;
  const char *p = src;
  while (*p) {
    const char *line = p;
    const char *line_end = line;
    while (*line_end && *line_end != '\n')
      line_end++;
    if (repl_line_is_bare_std_use(line, line_end))
      return 1;
    p = *line_end == '\n' ? line_end + 1 : line_end;
  }
  return 0;
}

static void repl_push_user_use_module_unique(const char *module) {
  if (!module || !*module)
    return;
  for (size_t i = 0; i < g_repl_cg.user_use_modules.len; ++i) {
    if (g_repl_cg.user_use_modules.data[i] &&
        strcmp(g_repl_cg.user_use_modules.data[i], module) == 0)
      return;
  }
  vec_push(&g_repl_cg.user_use_modules, ny_strdup(module));
}

static void repl_preload_lazy_imports(const char *imports,
                                      std_mode_t std_mode,
                                      doc_list_t *docs) {
  if (!imports || !*imports || std_mode == STD_MODE_NONE)
    return;
  const char *p = imports;
  while (*p) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (strncmp(p, "use", 3) != 0 ||
        (p[3] && !isspace((unsigned char)p[3]))) {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    p += 3;
    while (*p && isspace((unsigned char)*p))
      p++;
    const char *start = p;
    while (*p && !isspace((unsigned char)*p) && *p != ';')
      p++;
    if (p > start) {
      char *module = ny_strndup(start, (size_t)(p - start));
      if (module) {
        repl_ensure_module(module, std_mode, docs);
        repl_push_user_use_module_unique(module);
        free(module);
      }
    }
    while (*p && *p != '\n')
      p++;
  }
}

static int repl_lazy_import_std_for_source(const char *src, std_mode_t std_mode,
                                           doc_list_t *docs,
                                           char **out_imports) {
  if (out_imports)
    *out_imports = NULL;
  if (std_mode == STD_MODE_NONE || std_mode == STD_MODE_FULL || !docs || !src || !*src ||
      !g_repl_std_root_lazy)
    return 0;
  char seen[128][64];
  char seen_modules[64][128];
  int seen_len = 0;
  int seen_module_len = 0;
  int imported = 0;
  char *imports = NULL;
  size_t imports_len = 0;
  size_t imports_cap = 0;
  const char *p = src;
  while (*p) {
    if (repl_lazy_skip_comment_or_string(&p))
      continue;
    unsigned char ch = (unsigned char)*p;
    if (!(isalpha(ch) || ch == '_')) {
      p++;
      continue;
    }
    const char *start = p++;
    while (isalnum((unsigned char)*p) || *p == '_')
      p++;
    size_t len = (size_t)(p - start);
    if (len == 0 || len >= sizeof(seen[0]))
      continue;
    if (start > src && start[-1] == '.')
      continue;
    if (*p == '.') {
      char chain[160];
      const char *chain_end = p;
      int segments = 0;
      if (repl_lazy_read_ident_chain(start, chain, sizeof(chain), &chain_end,
                                     &segments) &&
          segments > 1) {
        const char *after_chain = chain_end;
        while (*after_chain && isspace((unsigned char)*after_chain))
          after_chain++;
        if (*after_chain == '(') {
          char *module = repl_lazy_std_module_for_member_chain(docs, chain);
          if (module) {
            int module_seen = repl_seen_name((const char *)seen_modules,
                                             sizeof(seen_modules[0]),
                                             seen_module_len, module);
            if (!module_seen &&
                seen_module_len <
                    (int)(sizeof(seen_modules) / sizeof(seen_modules[0])))
              snprintf(seen_modules[seen_module_len++], sizeof(seen_modules[0]),
                       "%s", module);
            repl_ensure_module(module, std_mode, docs);
            if (!module_seen) {
              if (out_imports)
                repl_lazy_append_use(&imports, &imports_len, &imports_cap,
                                     module);
              imported++;
            }
            free(module);
          }
        }
      }
      p = chain_end > p ? chain_end : p + 1;
      continue;
    }
    const char *after = p;
    while (*after && isspace((unsigned char)*after))
      after++;
    if (*after != '(')
      continue;
    char name[64];
    memcpy(name, start, len);
    name[len] = '\0';
    if (repl_lazy_ident_ignored(name) ||
        repl_seen_name((const char *)seen, sizeof(seen[0]), seen_len, name))
      continue;
    if (seen_len < (int)(sizeof(seen) / sizeof(seen[0])))
      snprintf(seen[seen_len++], sizeof(seen[0]), "%s", name);
    char *module = repl_lazy_std_module_for_leaf(docs, name);
    if (!module)
      continue;
    int module_seen =
        repl_seen_name((const char *)seen_modules, sizeof(seen_modules[0]),
                       seen_module_len, module);
    if (!module_seen &&
        seen_module_len < (int)(sizeof(seen_modules) / sizeof(seen_modules[0])))
      snprintf(seen_modules[seen_module_len++], sizeof(seen_modules[0]), "%s",
               module);
    repl_ensure_module(module, std_mode, docs);
    if (!module_seen) {
      if (out_imports)
        repl_lazy_append_use(&imports, &imports_len, &imports_cap, module);
      imported++;
    }
    free(module);
  }
  if (out_imports)
    *out_imports = imports;
  else
    free(imports);
  return imported;
}

static bool repl_is_procedure_name(const char *name) {
  const char *base = ny_name_leaf(name);
  if (!base || !*base)
    return false;
  return strcmp(base, "print") == 0 || strcmp(base, "eprint") == 0 ||
         strcmp(base, "assert") == 0 || strcmp(base, "assert_eq") == 0 ||
         strcmp(base, "panic") == 0 || strcmp(base, "main") == 0;
}

static bool repl_should_echo_expr(expr_t *expr) {
  if (!expr)
    return false;
  if (expr->kind == NY_E_CALL) {
    expr_t *callee = expr->as.call.callee;
    if (callee && callee->kind == NY_E_IDENT &&
        repl_is_procedure_name(callee->as.ident.name))
      return false;
  } else if (expr->kind == NY_E_MEMCALL) {
    if (repl_is_procedure_name(expr->as.memcall.name))
      return false;
  }
  return true;
}

void ny_repl_set_std_mode(std_mode_t mode) {
  g_repl_std_override = mode;
  g_repl_has_std_override = 1;
}
void ny_repl_set_plain(int plain) { g_repl_plain = plain; }
void ny_repl_set_max_errors(int max_errors) { g_repl_max_errors = max_errors; }

static void repl_apply_parser_limits(parser_t *p) {
  if (p && g_repl_max_errors >= 0)
    p->error_limit = g_repl_max_errors;
}

static const char *repl_std_mode_name(std_mode_t mode) {
  switch (mode) {
  case STD_MODE_NONE:
    return "none";
  case STD_MODE_FULL:
    return "full";
  case STD_MODE_DEFAULT:
    return "default";
  case STD_MODE_MINIMAL:
    return "minimal";
  default:
    return "unknown";
  }
}

#define REPL_SNAPSHOT_MAGIC "#!nytrix-repl-snapshot v1"
#define REPL_SNAPSHOT_BEGIN "; nytrix-snapshot-begin"

static void repl_ensure_parent_dir_for_path(const char *path) {
  if (!path || !*path)
    return;
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
#ifdef _WIN32
  char *backslash = strrchr(dir, '\\');
  if (!slash || (backslash && backslash > slash))
    slash = backslash;
#endif
  if (!slash || slash == dir)
    return;
  *slash = '\0';
  ny_ensure_dir_recursive(dir);
}

static char *repl_next_command_arg(char **cursor) {
  if (!cursor || !*cursor)
    return NULL;
  char *p = *cursor;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (!*p) {
    *cursor = p;
    return NULL;
  }
  char quote = '\0';
  if (*p == '"' || *p == '\'') {
    quote = *p;
    p++;
  }
  char *start = p;
  while (*p) {
    if (quote) {
      if (*p == quote)
        break;
    } else if (isspace((unsigned char)*p)) {
      break;
    }
    p++;
  }
  char *out = ny_strndup(start, (size_t)(p - start));
  if (quote && *p == quote)
    p++;
  while (*p && isspace((unsigned char)*p))
    p++;
  *cursor = p;
  return out;
}

static char *repl_snapshot_payload_copy(const char *src) {
  if (!src ||
      strncmp(src, REPL_SNAPSHOT_MAGIC, sizeof(REPL_SNAPSHOT_MAGIC) - 1) != 0)
    return NULL;
  const char *begin = strstr(src, REPL_SNAPSHOT_BEGIN);
  if (!begin)
    return ny_strdup("");
  const char *payload = strchr(begin, '\n');
  payload = payload ? payload + 1 : begin + strlen(begin);
  return ny_strdup(payload);
}

static size_t repl_snapshot_source_len(void) {
  return (g_repl_user_source && *g_repl_user_source)
             ? strlen(g_repl_user_source)
             : 0;
}

static char *repl_build_snapshot_image(std_mode_t std_mode) {
  const char *payload =
      (g_repl_user_source && *g_repl_user_source) ? g_repl_user_source : "";
  size_t payload_len = strlen(payload);
  char header[512];
  snprintf(header, sizeof(header),
           "%s\n"
           "; nytrix-snapshot-format: source-v1\n"
           "; nytrix-snapshot-std: %s\n"
           "; nytrix-snapshot-created: %lld\n"
           "%s\n",
           REPL_SNAPSHOT_MAGIC, repl_std_mode_name(std_mode),
           (long long)time(NULL), REPL_SNAPSHOT_BEGIN);
  size_t header_len = strlen(header);
  int add_newline = payload_len > 0 && payload[payload_len - 1] != '\n';
  char *out = malloc(header_len + payload_len + (add_newline ? 1 : 0) + 1);
  if (!out)
    return NULL;
  memcpy(out, header, header_len);
  if (payload_len)
    memcpy(out + header_len, payload, payload_len);
  if (add_newline)
    out[header_len + payload_len++] = '\n';
  out[header_len + payload_len] = '\0';
  return out;
}

static int repl_write_snapshot_image(const char *path, std_mode_t std_mode) {
  if (!path || !*path)
    return -1;
  char *image = repl_build_snapshot_image(std_mode);
  if (!image)
    return -1;
  repl_ensure_parent_dir_for_path(path);
  int rc = ny_write_file(path, image, strlen(image));
  free(image);
  return rc;
}

static int repl_compile_snapshot_image(const char *snapshot_path,
                                       const char *output_path) {
  if (!snapshot_path || !*snapshot_path || !output_path || !*output_path)
    return 1;
  repl_ensure_parent_dir_for_path(output_path);
  char *exe = ny_get_executable_path();
  const char *argv[] = {exe && *exe ? exe : "ny", "-o", output_path,
                        snapshot_path, NULL};
  int rc = ny_exec_spawn(argv);
  return rc;
}

static void repl_add_docs_from_source(doc_list_t *docs, const char *src,
                                      const char *name) {
  if (!docs)
    return;
  if (!src || !*src)
    return;
  parser_t ps;
  parser_init(&ps, src, name ? name : "<repl:snapshot>");
  program_t pr = parse_program(&ps);
  if (!ps.had_error)
    doclist_add_from_prog(docs, &pr);
  program_free(&pr, ps.arena);
  repl_update_docs(docs, src);
}

static void repl_reset_docs_from_source(doc_list_t *docs, const char *src,
                                        const char *name) {
  if (!docs)
    return;
  doclist_free(docs);
  memset(docs, 0, sizeof(*docs));
  g_repl_lazy_docs_loaded = 0;
  add_builtin_docs(docs);
  repl_add_docs_from_source(docs, src, name);
}

static int repl_load_snapshot_image(const char *path, const char *src,
                                    std_mode_t std_mode, doc_list_t *docs) {
  char *payload = repl_snapshot_payload_copy(src);
  if (!payload)
    return -1;
  char *old_source = (g_repl_user_source && *g_repl_user_source)
                         ? ny_strdup(g_repl_user_source)
                         : NULL;
  repl_set_user_source(payload);
  repl_set_last_expand_source(payload, path);
  repl_reset_docs_from_source(docs, NULL, NULL);
  g_repl_std_root_lazy = 0;
  int status = repl_rebuild_engine_from_persistent(std_mode, docs);
  if (status != 0) {
    repl_set_user_source(old_source);
    repl_reset_docs_from_source(docs, NULL, NULL);
    (void)repl_rebuild_engine_from_persistent(std_mode, docs);
    repl_add_docs_from_source(docs, old_source, "<repl:restore>");
    free(old_source);
    free(payload);
    return status;
  }
  repl_add_docs_from_source(docs, payload, path);
  printf("Loaded snapshot %s (%zu bytes)\n", path, strlen(payload));
  free(old_source);
  free(payload);
  return 0;
}

static void repl_restore_terminal_state(void) {
  (void)rt_tty_raw(0);
  if (!isatty(STDOUT_FILENO) || !ny_readline_vt_output_ok())
    return;
  fputs("\033[?2004l\033[0m\033[?25h\033[?7h", stdout);
  fflush(stdout);
}

static bool repl_is_known_package(const char *name) {
  if (!name || !*name)
    return false;
  for (size_t i = 0; i < ny_std_package_count(); ++i) {
    if (strcmp(ny_std_package_name(i), name) == 0)
      return true;
  }
  return false;
}

static bool repl_namespace_has_children(const char *root) {
  if (!root || !*root)
    return false;
  size_t root_len = strlen(root);
  for (size_t i = 0; i < ny_std_module_count(); ++i) {
    const char *name = ny_std_module_name(i);
    if (strncmp(name, root, root_len) == 0 && name[root_len] == '.')
      return true;
  }
  return false;
}

static void repl_trim_help_root(const char *raw, char *out, size_t out_cap,
                                bool *wants_children) {
  if (out_cap == 0)
    return;
  out[0] = '\0';
  if (wants_children)
    *wants_children = false;
  if (!raw || !*raw)
    return;
  snprintf(out, out_cap, "%s", raw);
  size_t len = strlen(out);
  while (len > 0 && isspace((unsigned char)out[len - 1]))
    out[--len] = '\0';
  if (len >= 2 && out[len - 2] == '.' && out[len - 1] == '*') {
    out[len - 2] = '\0';
    if (wants_children)
      *wants_children = true;
  } else if (len > 0 && out[len - 1] == '.') {
    out[len - 1] = '\0';
    if (wants_children)
      *wants_children = true;
  }
}

static const char *repl_doc_kind_name(int kind) {
  switch (kind) {
  case 2:
    return "Module";
  case 3:
    return "Function";
  case 4:
    return "Symbol";
  case 5:
    return "Method";
  default:
    return "Entry";
  }
}

typedef struct {
  char *name;
  const char *kind;
} repl_help_item_t;

typedef struct {
  repl_help_item_t *data;
  size_t len;
  size_t cap;
} repl_help_items_t;

static void repl_help_items_free(repl_help_items_t *items) {
  if (!items)
    return;
  for (size_t i = 0; i < items->len; ++i)
    free(items->data[i].name);
  free(items->data);
  items->data = NULL;
  items->len = 0;
  items->cap = 0;
}

static void repl_help_items_add(repl_help_items_t *items, const char *name,
                                const char *kind) {
  if (!items || !name || !*name)
    return;
  for (size_t i = 0; i < items->len; ++i) {
    if (strcmp(items->data[i].name, name) == 0)
      return;
  }
  if (items->len == items->cap) {
    size_t new_cap = items->cap ? items->cap * 2 : 16;
    repl_help_item_t *data = realloc(items->data, new_cap * sizeof(*data));
    if (!data)
      return;
    items->data = data;
    items->cap = new_cap;
  }
  items->data[items->len].name = ny_strdup(name);
  items->data[items->len].kind = kind;
  items->len += 1;
}

static void repl_collect_namespace_modules(repl_help_items_t *items,
                                           const char *root) {
  if (!items || !root || !*root)
    return;
  size_t root_len = strlen(root);
  for (size_t i = 0; i < ny_std_module_count(); ++i) {
    const char *name = ny_std_module_name(i);
    if (strncmp(name, root, root_len) != 0 || name[root_len] != '.')
      continue;
    const char *next = name + root_len + 1;
    size_t seg_len = 0;
    while (next[seg_len] && next[seg_len] != '.')
      seg_len++;
    if (seg_len == 0 || root_len + 1 + seg_len >= 512)
      continue;
    char child[512];
    snprintf(child, sizeof(child), "%s.%.*s", root, (int)seg_len, next);
    repl_help_items_add(items, child,
                        next[seg_len] == '\0' ? "Module" : "Namespace");
  }
}

static void repl_collect_namespace_members(repl_help_items_t *items,
                                           const doc_list_t *docs,
                                           const char *root) {
  if (!items || !docs || !root || !*root)
    return;
  size_t root_len = strlen(root);
  for (size_t i = 0; i < docs->len; ++i) {
    const ny_doc_entry *entry = &docs->data[i];
    if (!entry->name || strncmp(entry->name, root, root_len) != 0 ||
        entry->name[root_len] != '.')
      continue;
    const char *sub = entry->name + root_len + 1;
    if (!*sub || strchr(sub, '.') || entry->kind == 2)
      continue;
    repl_help_items_add(items, entry->name, repl_doc_kind_name(entry->kind));
  }
}

static void repl_print_help_items(const char *title,
                                  const repl_help_items_t *items) {
  if (!items || items->len == 0)
    return;
  printf("\n%s%s:%s\n", clr(NY_CLR_BOLD NY_CLR_CYAN), title, clr(NY_CLR_RESET));
  for (size_t i = 0; i < items->len; ++i) {
    printf("  %-32s %s%s%s\n", items->data[i].name, clr(NY_CLR_GRAY),
           items->data[i].kind, clr(NY_CLR_RESET));
  }
}

static int repl_print_namespace_help(doc_list_t *docs, const char *query) {
  char root[256];
  bool wants_children = false;
  repl_trim_help_root(query, root, sizeof(root), &wants_children);
  if (!root[0])
    return 0;

  bool is_pkg = repl_is_known_package(root);
  int mod_idx = is_pkg ? -1 : ny_std_find_module_by_name(root);
  const char *canon =
      (mod_idx >= 0) ? ny_std_module_name((size_t)mod_idx) : root;
  bool has_children = repl_namespace_has_children(canon);
  if (!is_pkg && mod_idx < 0 && !has_children)
    return 0;

  if (mod_idx >= 0)
    repl_load_module_docs(docs, canon);

  int printed = 0;
  if (mod_idx >= 0 && !wants_children) {
    printed = doclist_print(docs, canon);
  } else if (is_pkg) {
    printf("\n%sPackage '%s'%s\n", clr(NY_CLR_BOLD), root, clr(NY_CLR_RESET));
    printed = 1;
  } else if (has_children) {
    printf("\n%sNamespace '%s'%s\n", clr(NY_CLR_BOLD), canon,
           clr(NY_CLR_RESET));
    printed = 1;
  }

  repl_help_items_t modules = {0};
  repl_help_items_t members = {0};
  repl_collect_namespace_modules(&modules, canon);
  if (!is_pkg)
    repl_collect_namespace_members(&members, docs, canon);
  repl_print_help_items(mod_idx >= 0 ? "Submodules" : "Namespaces", &modules);
  if (!is_pkg)
    repl_print_help_items("Members", &members);
  if (modules.len == 0 && members.len == 0 && printed &&
      (is_pkg || wants_children || has_children)) {
    printf("  %s(no direct members)%s\n", clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
  }
  printed = printed || modules.len > 0 || members.len > 0;
  repl_help_items_free(&modules);
  repl_help_items_free(&members);
  if (printed)
    printf("\n");
  return printed;
}

static void map_rt_syms_persistent(LLVMModuleRef mod,
                                   LLVMExecutionEngineRef ee) {
#define RT_DEF(name, p, args, sig, doc) {name, (void *)p}, {#p, (void *)p},
#define RT_GV(name, p, t, doc) {name, (void *)&p},
#ifdef _WIN32
#ifdef rt_argc
#undef rt_argc
#endif
#ifdef rt_argv
#undef rt_argv
#endif
#endif
  struct {
    const char *n;
    void *p;
  } syms[] = {
#include "rt/defs.h"
      {NULL, NULL}};
#undef RT_DEF
#undef RT_GV
  LLVMContextRef _ctx2 = LLVMGetModuleContext(mod);
  LLVMTypeRef _i64_2 = LLVMInt64TypeInContext(_ctx2);
  for (int i = 0; syms[i].n; ++i) {
    LLVMValueRef _ex2 = LLVMGetNamedFunction(mod, syms[i].n);
    if (_ex2) {
      LLVMAddGlobalMapping(ee, _ex2, syms[i].p);
    } else {
      LLVMTypeRef _p2[3] = {_i64_2, _i64_2, _i64_2};
      LLVMAddGlobalMapping(
          ee,
          LLVMAddFunction(mod, syms[i].n, LLVMFunctionType(_i64_2, _p2, 3, 0)),
          syms[i].p);
    }
    for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
         f = LLVMGetNextFunction(f)) {
      if (!LLVMIsDeclaration(f))
        continue;
      const char *fn = LLVMGetValueName(f);
      const char *tail = fn ? strrchr(fn, '.') : NULL;
      if (tail && strcmp(tail + 1, syms[i].n) == 0)
        LLVMAddGlobalMapping(ee, f, syms[i].p);
    }
  }
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    if (!LLVMIsDeclaration(f) || !LLVMGetFirstUse(f))
      continue;
    const char *name = LLVMGetValueName(f);
    if (!name || !*name || strncmp(name, "llvm.", 5) == 0)
      continue;
    void *ptr = ny_jit_resolve_symbol(name);
    const char *tail = strrchr(name, '.');
    if (!ptr && tail && tail[1])
      ptr = ny_jit_resolve_symbol(tail + 1);
    if (ptr)
      LLVMAddGlobalMapping(ee, f, ptr);
  }
}

static void repl_init_engine(std_mode_t mode, doc_list_t *docs) {
  if (g_repl_ctx)
    return;

  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();

  g_repl_ctx = LLVMContextCreate();
  LLVMModuleRef mod =
      LLVMModuleCreateWithNameInContext("repl_base", g_repl_ctx);

  ny_llvm_prepare_module(mod, 1);
  g_repl_builder = LLVMCreateBuilderInContext(g_repl_ctx);

  const char *std_init_fn_name = NULL;
  if (mode != STD_MODE_NONE) {

    const char *prebuilt = getenv("NYTRIX_STD_PREBUILT");
    if ((!prebuilt || ny_access(prebuilt, R_OK) != 0)) {
      const char *build_std = getenv("NYTRIX_BUILD_STD_PATH");
      if (build_std && ny_access(build_std, R_OK) == 0)
        prebuilt = build_std;
    }
    if (!prebuilt || ny_access(prebuilt, R_OK) != 0) {
      char *exe_dir = ny_get_executable_dir();
      if (exe_dir) {
        static char path_buf[4096];
        snprintf(path_buf, sizeof(path_buf), "%s/std.ny", exe_dir);
        if (ny_access(path_buf, R_OK) == 0)
          prebuilt = path_buf;
        else {
          snprintf(path_buf, sizeof(path_buf), "%s/../share/nytrix/std.ny",
                   exe_dir);
          if (ny_access(path_buf, R_OK) == 0)
            prebuilt = path_buf;
        }
      }
    }
    if (!prebuilt || ny_access(prebuilt, R_OK) != 0) {
#ifdef NYTRIX_STD_PATH
      if (ny_access(NYTRIX_STD_PATH, R_OK) == 0)
        prebuilt = NYTRIX_STD_PATH;
#endif
    }

    if (mode == STD_MODE_FULL && prebuilt && ny_access(prebuilt, R_OK) == 0) {
      g_std_src_cached_persistent = repl_read_file(prebuilt);
    }
    if (!g_std_src_cached_persistent)
      g_std_src_cached_persistent = ny_build_std_source(NULL, 0, mode, 0, NULL);

    if (g_std_src_cached_persistent) {
      parser_t parser;
      parser_init(&parser, g_std_src_cached_persistent, "<repl_std>");
      program_t *prog = malloc(sizeof(program_t));
      *prog = parse_program(&parser);
      if (!parser.had_error) {
        if (docs)
          doclist_add_from_prog(docs, prog);
        codegen_init_with_context(&g_repl_cg, prog, parser.arena, mod,
                                  g_repl_ctx, g_repl_builder);
        g_repl_cg.prog_owned = true;
        codegen_prepare(&g_repl_cg);
        codegen_emit(&g_repl_cg);
        if (!g_repl_cg.had_error) {
          codegen_emit_script(&g_repl_cg, "__repl_std_init");
          std_init_fn_name = "__repl_std_init";
        }
        ny_llvm_apply_host_attrs(mod);
      } else {
        program_free(prog, parser.arena);
        free(prog);
      }
    }
  } else {
    codegen_init_with_context(&g_repl_cg, NULL, NULL, mod, g_repl_ctx,
                              g_repl_builder);
  }
  struct LLVMMCJITCompilerOptions options;
  ny_jit_init_options(&options, mod);
  options.OptLevel = (unsigned)g_repl_opt_level;
  {
    const char *jit_opt = getenv("NYTRIX_REPL_JIT_OPT");
    if (jit_opt && *jit_opt) {
      int lvl = atoi(jit_opt);
      if (lvl < 0)
        lvl = 0;
      if (lvl > 3)
        lvl = 3;
      options.OptLevel = (unsigned)lvl;
    }
  }
  {
    const char *fast_isel = getenv("NYTRIX_REPL_FAST_ISEL");
    if (fast_isel && *fast_isel)
      options.EnableFastISel = ny_env_is_truthy(fast_isel) ? 1 : 0;
    else if (!ny_module_target_is_apple_arm64(mod))
      options.EnableFastISel = 1;
  }
  char *err = NULL;
  if (LLVMCreateMCJITCompilerForModule(&g_repl_ee, mod, &options,
                                       sizeof(options), &err) != 0) {
    fprintf(stderr, "JIT Error: %s\n", err);
    LLVMDisposeMessage(err);
  } else {
    map_rt_syms_persistent(mod, g_repl_ee);
    register_jit_symbols(g_repl_ee, mod, &g_repl_cg);
    if (std_init_fn_name) {
      uint64_t init_addr = LLVMGetFunctionAddress(g_repl_ee, std_init_fn_name);
      if (init_addr) {
        char *saved_trace = repl_dup_env_value("NYTRIX_TRACE");
        char *saved_calls = repl_dup_env_value("NYTRIX_TRACE_CALLS");
        char *saved_values = repl_dup_env_value("NYTRIX_TRACE_VALUES");
        char *saved_verbose = repl_dup_env_value("NYTRIX_TRACE_VERBOSE");
        char *saved_filter = repl_dup_env_value("NYTRIX_TRACE_FILTER");
        int saved_trace_requested = g_trace_requested;
        int saved_trace_suspended = g_trace_suspended;
        repl_unsetenv_force("NYTRIX_TRACE");
        repl_unsetenv_force("NYTRIX_TRACE_CALLS");
        repl_unsetenv_force("NYTRIX_TRACE_VALUES");
        repl_unsetenv_force("NYTRIX_TRACE_VERBOSE");
        repl_unsetenv_force("NYTRIX_TRACE_FILTER");
        g_trace_requested = 0;
        g_trace_suspended = 1;
        rt_trace_refresh_env();
        ((int64_t (*)(void))init_addr)();
        if (saved_trace)
          repl_setenv_force("NYTRIX_TRACE", saved_trace);
        if (saved_calls)
          repl_setenv_force("NYTRIX_TRACE_CALLS", saved_calls);
        if (saved_values)
          repl_setenv_force("NYTRIX_TRACE_VALUES", saved_values);
        if (saved_verbose)
          repl_setenv_force("NYTRIX_TRACE_VERBOSE", saved_verbose);
        if (saved_filter)
          repl_setenv_force("NYTRIX_TRACE_FILTER", saved_filter);
        if (!saved_trace)
          repl_unsetenv_force("NYTRIX_TRACE");
        if (!saved_calls)
          repl_unsetenv_force("NYTRIX_TRACE_CALLS");
        if (!saved_values)
          repl_unsetenv_force("NYTRIX_TRACE_VALUES");
        if (!saved_verbose)
          repl_unsetenv_force("NYTRIX_TRACE_VERBOSE");
        if (!saved_filter)
          repl_unsetenv_force("NYTRIX_TRACE_FILTER");
        g_trace_requested = saved_trace_requested;
        g_trace_suspended = saved_trace_suspended;
        rt_trace_refresh_env();
        free(saved_trace);
        free(saved_calls);
        free(saved_values);
        free(saved_verbose);
        free(saved_filter);
      }
    }
  }
}

static void repl_shutdown_engine(void) {
  LLVMExecutionEngineRef ee = g_repl_ee;
  LLVMModuleRef module = g_repl_cg.module;
  LLVMContextRef ctx = g_repl_ctx;

  g_repl_ee = NULL;
  g_repl_builder = NULL;
  g_repl_ctx = NULL;
  g_repl_cg.ee = NULL;
  g_repl_cg.llvm_ctx_owned = false;
  if (g_repl_cg.alloca_builder) {
    LLVMDisposeBuilder(g_repl_cg.alloca_builder);
    g_repl_cg.alloca_builder = NULL;
  }
  if (g_repl_cg.builder) {
    LLVMDisposeBuilder(g_repl_cg.builder);
    g_repl_cg.builder = NULL;
  }
  if (ee) {
    LLVMDisposeExecutionEngine(ee);
    module = NULL;
    g_repl_cg.module = NULL;
  } else if (module) {
    LLVMDisposeModule(module);
    g_repl_cg.module = NULL;
  }
  codegen_dispose(&g_repl_cg);
  memset(&g_repl_cg, 0, sizeof(codegen_t));

  if (ctx) {
    LLVMContextDispose(ctx);
  }
  g_eval_count = 0;
  if (g_std_src_cached_persistent) {
    free(g_std_src_cached_persistent);
    g_std_src_cached_persistent = NULL;
  }
  vec_free(&g_repl_loading_modules);
  repl_free_persistent_sources();
}

static void repl_drop_engine_refs_on_exit(void) {
  g_repl_ee = NULL;
  g_repl_builder = NULL;
  g_repl_ctx = NULL;
  memset(&g_repl_cg, 0, sizeof(codegen_t));
  g_eval_count = 0;
  if (g_std_src_cached_persistent) {
    free(g_std_src_cached_persistent);
    g_std_src_cached_persistent = NULL;
  }
  vec_free(&g_repl_loading_modules);
  repl_free_persistent_sources();
}

static bool repl_stmt_defines_zero_arg_main(stmt_t *stmt) {
  return stmt && stmt->kind == NY_S_FUNC && stmt->as.fn.name &&
         strcmp(stmt->as.fn.name, "main") == 0 && stmt->as.fn.params.len == 0;
}

static bool repl_program_has_zero_arg_main(program_t *prog) {
  if (!prog)
    return false;
  for (size_t i = 0; i < prog->body.len; ++i) {
    if (repl_stmt_defines_zero_arg_main(prog->body.data[i]))
      return true;
  }
  return false;
}

static bool repl_source_mentions_main_guard(const char *src) {
  return src && strstr(src, "__main") != NULL;
}

static bool repl_program_wants_auto_main(program_t *prog, const char *src,
                                         int from_init) {
  return !from_init && !repl_source_mentions_main_guard(src) &&
         repl_program_has_zero_arg_main(prog) &&
         !ny_program_has_top_zero_arg_call_named(prog, "main");
}

static char *repl_body_with_auto_main(const char *body) {
  static const char call[] = "\nif(true){\n   main()\n}\n";
  size_t body_len = body ? strlen(body) : 0;
  size_t call_len = sizeof(call) - 1;
  char *out = malloc(body_len + call_len + 1);
  if (!out)
    return NULL;
  if (body_len)
    memcpy(out, body, body_len);
  memcpy(out + body_len, call, call_len + 1);
  return out;
}

static bool repl_source_wants_auto_main(const char *src, int from_init) {
  if (!src || !strstr(src, "fn main") || repl_source_mentions_main_guard(src))
    return false;
  parser_t ps;
  parser_init_quiet(&ps, src, "<repl_input>");
  ps.exit_on_limit = false;
  repl_apply_parser_limits(&ps);
  program_t prog = parse_program(&ps);
  bool wants =
      !ps.had_error && repl_program_wants_auto_main(&prog, src, from_init);
  program_free(&prog, ps.arena);
  return wants;
}

static int repl_eval_snippet(const char *full_input, int is_stmt, char *an,
                             std_mode_t std_mode, int tty_in, doc_list_t *docs,
                             int from_init) {
  const char *eval_input = full_input;
  char *eval_input_owned = NULL;
  char *trimmed = ltrim((char *)full_input);
  if (trimmed[0] == '#' && trimmed[1] == '!') {
    const char *nl = strchr(trimmed, '\n');
    if (nl && nl[1] != '\0') {
      eval_input_owned = ny_strdup(nl + 1);
      eval_input = eval_input_owned;
      trimmed = ltrim(eval_input_owned);
    } else {
      return 0;
    }
  }
  bool auto_main_after_rebuild =
      repl_source_wants_auto_main(eval_input, from_init);
  if (!from_init && g_repl_user_source && *g_repl_user_source) {
    char *persistent_src = NULL;
    if (is_persistent_def(full_input))
      persistent_src = repl_extract_persistent_source(full_input);
    size_t prior_len = strlen(g_repl_user_source);
    size_t input_len = strlen(full_input);
    size_t combined_len = prior_len + input_len + 2;
    char *combined = malloc(combined_len);
    if (!combined) {
      free(persistent_src);
      free(eval_input_owned);
      return 1;
    }
    memcpy(combined, g_repl_user_source, prior_len);
    combined[prior_len] = '\n';
    memcpy(combined + prior_len + 1, full_input, input_len + 1);
    repl_shutdown_engine();
    repl_init_engine(std_mode, docs);
    int status =
        repl_eval_snippet(combined, is_stmt, an, std_mode, tty_in, docs, 1);
    if (status == 0 && persistent_src && *persistent_src) {
      repl_append_user_source(persistent_src);
      repl_update_docs(docs, full_input);
    }
    if (status == 0 && auto_main_after_rebuild)
      status = repl_eval_snippet("if(true){\n   main()\n}", 1, NULL, std_mode,
                                 0, docs, 1);
    free(combined);
    free(persistent_src);
    free(eval_input_owned);
    return status;
  }
  int show_an = (an && std_mode != STD_MODE_NONE && tty_in);
  if (show_an) {
    if (strchr(eval_input, '\n')) {
      show_an = 0;
    } else if (!strncmp(trimmed, "def ", 4) || !strncmp(trimmed, "mut ", 4) ||
               !strncmp(trimmed, "fn ", 3) || !strncmp(trimmed, "use ", 4) ||
               !strncmp(trimmed, "while", 5) || !strncmp(trimmed, "for", 3) ||
               !strncmp(trimmed, "if", 2)) {
      show_an = 0;
    }
  }
  const char *use_inspect =
      (std_mode != STD_MODE_NONE && tty_in) ? "use std.core.inspect\n" : "";
  const char *use_core = "";
  const char *use_os_prim = "";
  int has_bare_std_use =
      std_mode != STD_MODE_NONE && repl_source_has_bare_std_use_text(eval_input);
  if (has_bare_std_use)
    g_repl_std_root_lazy = 1;
  const char *compile_input = eval_input;
  char *compile_input_owned = NULL;
  if (std_mode != STD_MODE_NONE && g_repl_std_root_lazy) {
    compile_input_owned =
        repl_rewrite_lazy_std_qualified_calls(eval_input, docs);
    if (compile_input_owned)
      compile_input = compile_input_owned;

    char *stripped = repl_strip_bare_std_use_statements(compile_input);
    if (stripped) {
      free(compile_input_owned);
      compile_input_owned = stripped;
      compile_input = compile_input_owned;
    }
    if (has_bare_std_use && *ltrim((char *)compile_input) == '\0') {
      repl_enable_lazy_std_root(std_mode, docs);
      free(compile_input_owned);
      free(eval_input_owned);
      return 0;
    }
  }
  char *lazy_imports = NULL;
  repl_lazy_import_std_for_source(compile_input, std_mode, docs, &lazy_imports);
  repl_preload_lazy_imports(lazy_imports, std_mode, docs);
  const char *use_lazy = lazy_imports ? lazy_imports : "";
  if (!from_init && g_repl_exec_trace_compile)
    repl_expand_source(eval_input, "<repl:trace>", g_repl_exec_trace_filter, 1,
                       g_repl_exec_trace_json, g_repl_exec_trace_ir);
  size_t blen = strlen(compile_input) + (an ? strlen(an) : 0) +
                strlen(use_inspect) + strlen(use_core) + strlen(use_os_prim) +
                strlen(use_lazy) + 128;
  char *body = malloc(blen);
  if (!body) {
    free(compile_input_owned);
    free(lazy_imports);
    free(eval_input_owned);
    return 1;
  }
  if (!is_stmt && !tty_in)
    snprintf(body, blen, "%s%s%s%sreturn %s\n", use_core, use_os_prim,
             use_inspect, use_lazy, compile_input);
  else if (show_an)
    snprintf(body, blen, "%s%s%s%s%s\nrepl_show(%s\n)\n", use_core, use_os_prim,
             use_inspect, use_lazy, compile_input, an);
  else
    snprintf(body, blen, "%s%s%s%s%s\n", use_core, use_os_prim, use_inspect,
             use_lazy, compile_input);
  ny_tick_t t0 = ny_ticks_now();
  ny_tick_t t_parse0 = t0;
  parser_t ps;
  parser_init(&ps, body, "<repl_input>");
  ps.exit_on_limit = false;
  repl_apply_parser_limits(&ps);
  program_t *pr = malloc(sizeof(program_t));
  if (!pr) {
    program_free(&(program_t){0}, ps.arena);
    free(body);
    free(compile_input_owned);
    free(lazy_imports);
    free(eval_input_owned);
    return 1;
  }
  *pr = parse_program(&ps);
  if (!ps.had_error &&
      repl_program_wants_auto_main(pr, eval_input, from_init)) {
    char *body_with_main = repl_body_with_auto_main(body);
    if (body_with_main) {
      program_free(pr, ps.arena);
      free(body);
      body = body_with_main;
      parser_init(&ps, body, "<repl_input>");
      ps.exit_on_limit = false;
      repl_apply_parser_limits(&ps);
      *pr = parse_program(&ps);
    }
  }
  ny_tick_t t_parse1 = ny_ticks_now();
  int last_status = 0;
  bool persistent = false;
  bool rebuild_persistent = false;
  if (!ps.had_error) {
    repl_debug_stage("parsed");
    if (repl_program_has_bare_std_use(pr))
      g_repl_std_root_lazy = 1;
    if (std_mode != STD_MODE_NONE && tty_in && !show_an && pr->body.len > 0) {
      for (size_t i = 0; i < pr->body.len; ++i) {
        stmt_t *stmt = pr->body.data[i];
        if (stmt->kind != NY_S_EXPR ||
            !repl_should_echo_expr(stmt->as.expr.expr))
          continue;
        expr_t *callee = expr_new(ps.arena, NY_E_IDENT, stmt->tok);
        callee->as.ident.name = "repl_show";
        expr_t *call = expr_new(ps.arena, NY_E_CALL, stmt->tok);
        call->as.call.callee = callee;
        call_arg_t arg = {0};
        arg.val = stmt->as.expr.expr;
        vec_push(&call->as.call.args, arg);
        stmt->as.expr.expr = call;
      }
    }
    ny_tick_t t_preload0 = ny_ticks_now();
    for (size_t i = 0; i < pr->body.len; ++i) {
      stmt_t *s = pr->body.data[i];
      if (s->kind == NY_S_USE && s->as.use.module) {
        repl_debug_stage("preload-use");
        repl_ensure_module(s->as.use.module, std_mode, docs);
      }
    }
    repl_debug_stage("preload-done");
    ny_tick_t t_preload1 = ny_ticks_now();
    LLVMModuleRef eval_mod =
        LLVMModuleCreateWithNameInContext("repl_eval", g_repl_ctx);
    ny_llvm_prepare_module(eval_mod, g_repl_opt_level);
    LLVMBuilderRef eval_builder = LLVMCreateBuilderInContext(g_repl_ctx);
    codegen_t cg;
    codegen_init_with_context(&cg, pr, ps.arena, eval_mod, g_repl_ctx,
                              eval_builder);
    cg.is_repl = true;
    cg.auto_purity_infer = false;
    bool embed_repl_std = false;
#ifdef __APPLE__
    embed_repl_std = (std_mode != STD_MODE_NONE && g_repl_cg.prog != NULL);
#endif
    cg.skip_stdlib = (std_mode != STD_MODE_NONE && !embed_repl_std);
    if (embed_repl_std)
      vec_push(&cg.extra_progs, g_repl_cg.prog);
    for (size_t i = 0; i < g_repl_cg.fun_sigs.len; i++) {
      fun_sig s = g_repl_cg.fun_sigs.data[i];
      s.owned = false;
      vec_push(&cg.fun_sigs, s);
    }
    for (size_t i = 0; i < g_repl_cg.global_vars.len; i++) {
      binding b = g_repl_cg.global_vars.data[i];
      b.owned = false;
      vec_push(&cg.global_vars, b);
    }
    for (size_t i = 0; i < g_repl_cg.aliases.len; i++) {
      binding b = g_repl_cg.aliases.data[i];
      b.owned = false;
      vec_push(&cg.aliases, b);
    }
    for (size_t i = 0; i < g_repl_cg.import_aliases.len; i++) {
      binding b = g_repl_cg.import_aliases.data[i];
      b.owned = false;
      vec_push(&cg.import_aliases, b);
    }
    for (size_t i = 0; i < g_repl_cg.user_import_aliases.len; i++) {
      binding b = g_repl_cg.user_import_aliases.data[i];
      b.owned = false;
      vec_push(&cg.user_import_aliases, b);
    }
    for (size_t i = 0; i < g_repl_cg.enums.len; i++) {
      enum_def_t *e = g_repl_cg.enums.data[i];
      if (e)
        vec_push(&cg.enums, e);
    }
    for (size_t i = 0; i < g_repl_cg.layouts.len; i++) {
      layout_def_t *layout = g_repl_cg.layouts.data[i];
      if (layout)
        vec_push(&cg.layouts, layout);
    }
    if (std_mode == STD_MODE_NONE) {
      for (size_t i = 0; i < g_repl_cg.use_modules.len; i++)
        vec_push(&cg.use_modules, ny_strdup(g_repl_cg.use_modules.data[i]));
    }
    for (size_t i = 0; i < g_repl_cg.user_use_modules.len; i++) {
      vec_push(&cg.user_use_modules,
               ny_strdup(g_repl_cg.user_use_modules.data[i]));
    }
    for (size_t i = 0; i < g_repl_cg.links.len; i++) {
      vec_push(&cg.links, ny_strdup(g_repl_cg.links.data[i]));
    }
    repl_rebind_persistent_symbols(&cg);
    ny_tick_t t_codegen0 = ny_ticks_now();
    repl_debug_stage("codegen-prepare");
    codegen_prepare(&cg);
    repl_debug_stage("codegen-emit");
    codegen_emit(&cg);
    char fn_name[64];
    snprintf(fn_name, sizeof(fn_name), "__eval_%d", g_eval_count++);
    repl_debug_stage("codegen-script");
    LLVMValueRef eval_fn = codegen_emit_script(&cg, fn_name);
    (void)eval_fn;
    ny_llvm_apply_host_attrs(eval_mod);
    repl_debug_stage("codegen-done");
    if (!from_init && g_repl_exec_trace_compile && !cg.had_error &&
        g_repl_exec_trace_ir) {
      char *module_ir = LLVMPrintModuleToString(eval_mod);
      repl_print_llvm_ir(module_ir, fn_name);
      LLVMDisposeMessage(module_ir);
    }
    ny_tick_t t_codegen1 = ny_ticks_now();
    if (cg.had_error) {
      if (!from_init)
        fprintf(stderr, "REPL input failed during compilation.\n");
      last_status = 1;
    } else if (g_repl_ee) {
      ny_tick_t t_jit0 = ny_ticks_now();
      size_t existing_map_len = 0;
      repl_pending_fn_mapping_t *existing_maps =
          repl_collect_existing_jit_function_mappings(g_repl_ee, eval_mod,
                                                      &cg, &existing_map_len);
      repl_define_existing_jit_function_trampolines(existing_maps,
                                                    existing_map_len);
      repl_debug_ir(eval_mod);
      repl_debug_stage("jit-apply-maps-pre");
      repl_apply_existing_jit_function_mappings(g_repl_ee, existing_maps,
                                                existing_map_len);
      repl_debug_stage("jit-add-module");
      LLVMAddModule(g_repl_ee, eval_mod);
      repl_debug_stage("jit-apply-maps");
      repl_apply_existing_jit_function_mappings(g_repl_ee, existing_maps,
                                                existing_map_len);
      free(existing_maps);
      repl_debug_stage("jit-map-rt");
      map_rt_syms_persistent(eval_mod, g_repl_ee);
      repl_debug_stage("jit-register-symbols");
      register_jit_symbols(g_repl_ee, eval_mod, &cg);
      for (size_t i = 0; i < cg.interns.len; i++) {
        if (cg.interns.data[i].gv)
          LLVMAddGlobalMapping(g_repl_ee, cg.interns.data[i].gv,
                               (void *)((char *)cg.interns.data[i].data - 64));
        if (cg.interns.data[i].val)
          LLVMAddGlobalMapping(g_repl_ee, cg.interns.data[i].val,
                               &cg.interns.data[i].data);
      }
      (void)cg.global_vars;
      repl_debug_stage("jit-get-address");
      uint64_t addr = LLVMGetFunctionAddress(g_repl_ee, fn_name);
      if (addr) {
        repl_debug_stage("jit-call");
        int interrupted = 0;
#ifndef _WIN32
        int panicked = 0;
#endif
        int saved_trace_suspended = g_trace_suspended;
        if (from_init)
          g_trace_suspended = 1;
#ifndef _WIN32
        g_repl_eval_active = 1;
        if (!g_repl_sigint) {
          jmp_buf _pb2;
          _rpe _pe2;
          _pe2.env = &_pb2;
          _pe2.defer_base = 0;
          if (g_panic_env_stack.len >= g_panic_env_stack.cap) {
            size_t _nc2 = g_panic_env_stack.cap ? g_panic_env_stack.cap * 2 : 8;
            g_panic_env_stack.data =
                realloc(g_panic_env_stack.data, _nc2 * sizeof(_rpe));
            g_panic_env_stack.cap = _nc2;
          }
          g_panic_env_stack.data[g_panic_env_stack.len++] = _pe2;
          if (_setjmp(_pb2) == 0) {
            rt_trace_func(0);
            ((void (*)(void))addr)();
            repl_debug_stage("jit-return");
          } else {
            panicked = 1;
          }
          g_panic_env_stack.len--;
        }
        g_repl_eval_active = 0;
        if (g_repl_sigint) {
          interrupted = 1;
          last_status = 1;
        } else if (panicked) {
          repl_print_panic_value(rt_get_panic_val());
          last_status = 1;
        } else {
          last_status = 0;
        }
#else
        ((void (*)(void))addr)();
        last_status = 0;
#endif
        rt_print_flush();
        g_trace_suspended = saved_trace_suspended;
        ny_tick_t t_jit1 = ny_ticks_now();
        char *del_name = NULL;
        if (!interrupted && !strncmp(trimmed, "del ", 4)) {
          char *up = trimmed + 4;
          while (*up == ' ' || *up == '\t')
            up++;
          char *uend = up;
          while (*uend && !isspace((unsigned char)*uend) && *uend != ';')
            uend++;
          if (uend > up)
            del_name = ny_strndup(up, (size_t)(uend - up));
        }
        if (del_name) {
          repl_remove_def(del_name);
          free(del_name);
        } else if (!interrupted && is_persistent_def(eval_input)) {
          persistent = true;
          if (!from_init) {
            char *persistent_src = repl_extract_persistent_source(eval_input);
            if (persistent_src && *persistent_src) {
              repl_append_user_source(persistent_src);
            }
            free(persistent_src);
          }
          if (!from_init)
            repl_update_docs(docs, eval_input);
          for (size_t i = 0; i < cg.fun_sigs.len; i++) {
            if (i >= g_repl_cg.fun_sigs.len) {
              fun_sig s = repl_owned_fun_sig_copy(&cg.fun_sigs.data[i]);
              vec_push(&g_repl_cg.fun_sigs, s);
            }
          }
          for (size_t i = 0; i < cg.global_vars.len; i++) {
            if (i >= g_repl_cg.global_vars.len) {
              binding b = cg.global_vars.data[i];
              if (b.value)
                LLVMSetLinkage(b.value, LLVMExternalLinkage);
              b.name = ny_strdup(b.name);
              b.owned = true;
              vec_push(&g_repl_cg.global_vars, b);
            }
          }
          for (size_t i = 0; i < cg.aliases.len; i++) {
            if (i >= g_repl_cg.aliases.len) {
              binding alias_b = cg.aliases.data[i];
              alias_b.name = ny_strdup(alias_b.name);
              alias_b.stmt_t = (void *)ny_strdup((char *)alias_b.stmt_t);
              alias_b.owned = true;
              vec_push(&g_repl_cg.aliases, alias_b);
            }
          }
          for (size_t i = 0; i < cg.import_aliases.len; i++) {
            if (i >= g_repl_cg.import_aliases.len) {
              binding import_b = cg.import_aliases.data[i];
              import_b.name = ny_strdup(import_b.name);
              import_b.stmt_t = (void *)ny_strdup((char *)import_b.stmt_t);
              import_b.owned = true;
              vec_push(&g_repl_cg.import_aliases, import_b);
            }
          }
          for (size_t i = 0; i < cg.user_import_aliases.len; i++) {
            if (i >= g_repl_cg.user_import_aliases.len) {
              binding user_import_b = cg.user_import_aliases.data[i];
              user_import_b.name = ny_strdup(user_import_b.name);
              user_import_b.stmt_t =
                  (void *)ny_strdup((char *)user_import_b.stmt_t);
              user_import_b.owned = true;
              vec_push(&g_repl_cg.user_import_aliases, user_import_b);
            }
          }
          for (size_t i = 0; i < cg.enums.len; i++) {
            enum_def_t *src = cg.enums.data[i];
            if (!src || !src->name)
              continue;
            int exists = 0;
            for (size_t j = 0; j < g_repl_cg.enums.len; j++) {
              enum_def_t *dst = g_repl_cg.enums.data[j];
              if (dst && dst->name && strcmp(dst->name, src->name) == 0) {
                exists = 1;
                break;
              }
            }
            if (exists)
              continue;
            enum_def_t *dst = malloc(sizeof(*dst));
            if (!dst)
              continue;
            memset(dst, 0, sizeof(*dst));
            dst->name = ny_strdup(src->name);
            dst->stmt = NULL;
            if (src->members.len > 0) {
              dst->members.cap = src->members.len;
              dst->members.len = src->members.len;
              dst->members.data =
                  malloc(sizeof(enum_member_def_t) * dst->members.cap);
              if (dst->members.data) {
                for (size_t k = 0; k < src->members.len; k++) {
                  enum_member_def_t *sm = &src->members.data[k];
                  dst->members.data[k].name =
                      sm->name ? ny_strdup(sm->name) : NULL;
                  dst->members.data[k].value = sm->value;
                }
              } else {
                dst->members.cap = 0;
                dst->members.len = 0;
              }
            }
            vec_push(&g_repl_cg.enums, dst);
          }
          for (size_t i = 0; i < cg.layouts.len; i++) {
            layout_def_t *src = cg.layouts.data[i];
            if (!src || !src->name)
              continue;
            int exists = 0;
            for (size_t j = 0; j < g_repl_cg.layouts.len; j++) {
              layout_def_t *dst = g_repl_cg.layouts.data[j];
              if (dst && dst->name && strcmp(dst->name, src->name) == 0) {
                exists = 1;
                break;
              }
            }
            if (!exists) {
              layout_def_t *copy = repl_owned_layout_copy(src);
              if (copy)
                vec_push(&g_repl_cg.layouts, copy);
            }
          }
          for (size_t i = 0; i < cg.use_modules.len; i++) {
            int exists = 0;
            for (size_t j = 0; j < g_repl_cg.use_modules.len; j++) {
              if (strcmp(g_repl_cg.use_modules.data[j],
                         cg.use_modules.data[i]) == 0) {
                exists = 1;
                break;
              }
            }
            if (!exists)
              vec_push(&g_repl_cg.use_modules,
                       ny_strdup(cg.use_modules.data[i]));
          }
          for (size_t i = 0; i < cg.user_use_modules.len; i++) {
            int exists = 0;
            for (size_t j = 0; j < g_repl_cg.user_use_modules.len; j++) {
              if (strcmp(g_repl_cg.user_use_modules.data[j],
                         cg.user_use_modules.data[i]) == 0) {
                exists = 1;
                break;
              }
            }
            if (!exists) {
              vec_push(&g_repl_cg.user_use_modules,
                       ny_strdup(cg.user_use_modules.data[i]));
            }
          }
          for (size_t i = 0; i < cg.links.len; i++) {
            int exists = 0;
            for (size_t j = 0; j < g_repl_cg.links.len; j++) {
              if (strcmp(g_repl_cg.links.data[j], cg.links.data[i]) == 0) {
                exists = 1;
                break;
              }
            }
            if (!exists)
              vec_push(&g_repl_cg.links, ny_strdup(cg.links.data[i]));
          }
        }
        if (g_repl_phase_trace) {
          fprintf(stderr,
                  "[repl-trace] parse=%.3fms preload=%.3fms codegen=%.3fms "
                  "jit=%.3fms total=%.3fms\n",
                  ny_ticks_delta_ms(t_parse0, t_parse1),
                  ny_ticks_delta_ms(t_preload0, t_preload1),
                  ny_ticks_delta_ms(t_codegen0, t_codegen1),
                  ny_ticks_delta_ms(t_jit0, t_jit1), ny_ticks_elapsed_ms(t0));
        }
      }
      for (size_t i = 0; i < cg.interns.len; i++) {
        vec_push(&g_repl_cg.interns, cg.interns.data[i]);
      }
      cg.interns.len = 0;
    }
    cg.fun_sigs.len = 0;
    cg.global_vars.len = 0;
    cg.aliases.len = 0;
    cg.import_aliases.len = 0;
    cg.user_import_aliases.len = 0;
    cg.enums.len = 0;
    cg.layouts.len = 0;
    cg.extra_progs.len = 0;
    cg.extra_arenas.len = 0;
    cg.use_modules.len = 0;
    cg.user_use_modules.len = 0;
    cg.links.len = 0;
    repl_debug_stage("cleanup-codegen");
    codegen_dispose(&cg);
    repl_debug_stage("cleanup-codegen-done");
  } else {
    repl_print_error_snippet(eval_input, ps.cur.line, ps.cur.col);
    if (!from_init)
      fprintf(stderr, "REPL input failed with %d parse error%s.\n",
              ps.error_count, ps.error_count == 1 ? "" : "s");
    last_status = 1;
  }
  if (g_repl_timing && !from_init)
    printf("[Eval: %.3f ms]\n", ny_ticks_elapsed_ms(t0));
  if (persistent) {
    vec_push(&g_repl_cg.extra_progs, pr);
    vec_push(&g_repl_cg.extra_arenas, ps.arena);
    vec_push(&g_repl_persistent_sources, body);
    body = NULL;
  } else {
    repl_debug_stage("cleanup-program");
    program_free(pr, ps.arena);
    free(pr);
    repl_debug_stage("cleanup-program-done");
  }
  free(body);
  free(compile_input_owned);
  free(lazy_imports);
  if (eval_input_owned)
    free(eval_input_owned);
  (void)rebuild_persistent;
  return last_status;
}

static void repl_ensure_module(const char *name, std_mode_t std_mode,
                               doc_list_t *docs) {
  char *norm_name = normalize_module_name(name);
  for (size_t i = 0; i < g_repl_cg.use_modules.len; i++) {
    if (strcmp(g_repl_cg.use_modules.data[i], norm_name) == 0) {
      free(norm_name);
      return;
    }
  }

  if (std_mode != STD_MODE_NONE && strcmp(norm_name, "std") == 0) {
    vec_push(&g_repl_cg.use_modules, ny_strdup(norm_name));
    free(norm_name);
    return;
  }
  for (size_t i = 0; i < g_repl_loading_modules.len; i++) {
    if (strcmp(g_repl_loading_modules.data[i], norm_name) == 0) {
      free(norm_name);
      return;
    }
  }
  char *entry_name = ny_strdup(norm_name);
  vec_push(&g_repl_loading_modules, entry_name);
  if (name[0] == '.' || name[0] == '/' || name[0] == '\\' ||
      strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
    char *src = repl_read_file(name);
    if (src) {
      char *masked = repl_mask_main_guards(src);
      repl_eval_snippet(masked ? masked : src, 1, NULL, STD_MODE_NONE, 0, docs,
                        1);
      free(masked);
      free(src);
      g_repl_loading_modules.len--;
      vec_push(&g_repl_cg.use_modules, entry_name);
      return;
    }
  }
  char *clean_name = ny_strdup(name);
  if (clean_name[0] == '"') {
    size_t len = strlen(clean_name);
    if (len > 2) {
      memmove(clean_name, clean_name + 1, len - 2);
      clean_name[len - 2] = '\0';
    }
  }
  const char *entry_path = NULL;
  if (clean_name[0] == '.' || clean_name[0] == '/' || clean_name[0] == '\\' ||
      strchr(clean_name, '/') != NULL || strchr(clean_name, '\\') != NULL) {
    static char cwd_buf[PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf)))
      entry_path = cwd_buf;
  }
  if (!entry_path)
    entry_path = ny_src_root();
  char *bundle = ny_build_std_source((const char **)&clean_name, 1,
                                     STD_MODE_DEFAULT, 0, entry_path);
  if (bundle) {
    char *masked = repl_mask_main_guards(bundle);
    repl_eval_snippet(masked ? masked : bundle, 1, NULL, STD_MODE_NONE, 0, docs,
                      1);
    free(masked);
    free(bundle);
    g_repl_loading_modules.len--;
    vec_push(&g_repl_cg.use_modules, entry_name);
  } else {
    g_repl_loading_modules.len--;
    free(entry_name);
    free(clean_name);
    free(norm_name);
    return;
  }
  free(clean_name);
  free(norm_name);
}

void ny_repl_run(int opt_level, const char *opt_pipeline, const char *init_code,
                 int batch_mode) {
  if (!g_repl_exit_hook_registered) {
    atexit(repl_restore_terminal_state);
    g_repl_exit_hook_registered = 1;
  }
  g_repl_std_root_lazy = 0;
  g_repl_lazy_docs_loaded = 0;
  bool fast_batch_exit = repl_fast_batch_exit_enabled(batch_mode);
  g_repl_opt_level = batch_mode ? 0 : opt_level;
  if (g_repl_opt_level < 0)
    g_repl_opt_level = 0;
  if (g_repl_opt_level > 3)
    g_repl_opt_level = 3;
  (void)opt_pipeline;
#ifdef _WIN32
  ny_readline_prepare_console();
#endif
  const char *plain = getenv("NYTRIX_REPL_PLAIN");
  if (g_repl_plain || (plain && plain[0] != '0') || !isatty(STDOUT_FILENO)) {
    color_mode = 0;
  }
  bool quiet_repl = ny_env_enabled("NYTRIX_REPL_QUIET");
  bool banner_off = quiet_repl || ny_env_enabled("NYTRIX_REPL_NO_BANNER");
#ifdef _WIN32
  if (!ny_readline_vt_output_ok())
    color_mode = 0;
#endif
  std_mode_t std_mode =
      g_repl_has_std_override ? g_repl_std_override : STD_MODE_DEFAULT;
  if (!g_repl_has_std_override) {
    const char *env_std = getenv("NYTRIX_REPL_STD");
    if (env_std) {
      if (strcmp(env_std, "none") == 0)
        std_mode = STD_MODE_NONE;
      else if (strcmp(env_std, "full") == 0)
        std_mode = STD_MODE_FULL;
    }
  }
  if (getenv("NYTRIX_REPL_NO_STD"))
    std_mode = STD_MODE_NONE;
#ifdef __APPLE__
  if (!g_repl_has_std_override && std_mode == STD_MODE_DEFAULT)
    std_mode = STD_MODE_FULL;
#endif
  g_repl_effective_mode = std_mode;
  doc_list_t docs = {0};
  doc_list_t *p_docs = NULL;
  if (!batch_mode) {
    g_repl_docs = &docs;
    add_builtin_docs(&docs);
    p_docs = &docs;
  }
  repl_init_engine(std_mode, p_docs);
  g_repl_timing = 0;
  g_repl_phase_trace = 0;
  g_repl_exec_trace_enabled = 0;
  g_repl_exec_trace_values = 0;
  g_repl_exec_trace_verbose = 0;
  g_repl_exec_trace_calls = 0;
  g_repl_exec_trace_compile = 0;
  g_repl_exec_trace_json = 0;
  g_repl_exec_trace_ir = 0;
  repl_set_exec_trace_filter(NULL);
  if (getenv("NYTRIX_REPL_TIME"))
    g_repl_timing = 1;
  if (getenv("NYTRIX_REPL_TRACE"))
    g_repl_phase_trace = 1;
  if (ny_env_enabled("NYTRIX_TRACE") ||
      ny_env_enabled("NYTRIX_REPL_EXEC_TRACE"))
    g_repl_exec_trace_enabled = 1;
  if (ny_env_enabled("NYTRIX_TRACE_VALUES"))
    g_repl_exec_trace_values = 1;
  if (ny_env_enabled("NYTRIX_TRACE_VERBOSE"))
    g_repl_exec_trace_verbose = 1;
  if (ny_env_enabled("NYTRIX_TRACE_CALLS") || g_repl_exec_trace_enabled)
    g_repl_exec_trace_calls = 1;
  if (ny_env_enabled("NYTRIX_REPL_TRACE_COMPILE"))
    g_repl_exec_trace_compile = 1;
  if (ny_env_enabled("NYTRIX_REPL_TRACE_JSON")) {
    g_repl_exec_trace_compile = 1;
    g_repl_exec_trace_json = 1;
  }
  if (ny_env_enabled("NYTRIX_REPL_TRACE_IR")) {
    g_repl_exec_trace_compile = 1;
    g_repl_exec_trace_ir = 1;
  }
  if (ny_env_str_nonempty("NYTRIX_TRACE_FILTER"))
    repl_set_exec_trace_filter(ny_env_str_nonempty("NYTRIX_TRACE_FILTER"));
  repl_apply_exec_trace_env();
  char **init_lines = NULL;
  size_t init_lines_len = 0, init_line_idx = 0;
  if (init_code && *init_code) {
    if (batch_mode || !isatty(STDIN_FILENO)) {
      init_lines = malloc(sizeof(char *));
      init_lines[0] = ny_strdup(init_code);
      init_lines_len = 1;
    } else {
      init_lines = repl_split_lines(init_code, &init_lines_len);
    }
  }
  if (!banner_off && !init_code && isatty(STDOUT_FILENO)) {
    printf("%sNytrix REPL%s %s(%s)%s - Type :help for commands\n",
           clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET), clr(NY_CLR_GRAY),
           repl_std_mode_name(std_mode), clr(NY_CLR_RESET));
    fflush(stdout);
  }
  char history_path[PATH_MAX] = {0};
  const char *home = getenv("HOME");
#ifdef _WIN32
  if (!home)
    home = getenv("USERPROFILE");
#endif
  if (home) {
    ny_join_path(history_path, sizeof(history_path), home, ".nytrix_history");
    ny_readline_read_history(history_path);
    ny_readline_stifle_history(1000);
  }
  int tty_in = isatty(STDIN_FILENO);
  if (!tty_in && !init_code) {
    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf)
      exit(1);
    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
      if (len + 1 >= cap &&
          !repl_reserve_char_capacity(&buf, &cap, len + 2, 1024)) {
        free(buf);
        exit(1);
      }
      buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    if (batch_mode) {
      init_lines = malloc(sizeof(char *));
      if (!init_lines) {
        free(buf);
        exit(1);
      }
      init_lines[0] = buf;
      init_lines_len = 1;
    } else {
      init_lines = repl_split_lines(buf, &init_lines_len);
      free(buf);
      if (!init_lines)
        exit(1);
    }
    init_code = "<stdin>";
  }
  char *input_buffer = NULL;
  int last_status = 0;
#ifdef _WIN32
  void(__cdecl * prev_sigint)(int) = signal(SIGINT, repl_on_sigint);
#else
  struct sigaction sa_int;
  struct sigaction prev_sigint;
  memset(&sa_int, 0, sizeof(sa_int));
  sa_int.sa_handler = repl_on_sigint;
  sigemptyset(&sa_int.sa_mask);
  sa_int.sa_flags = 0;
  sigaction(SIGINT, &sa_int, &prev_sigint);
#endif
  while (1) {
    g_repl_sigint = 0;
    repl_reset_redisplay();
    char prompt_buf[512];
    const char *prompt;
    if (quiet_repl) {
      prompt = "";
      if (input_buffer)
        repl_indent_next = repl_calc_indent(input_buffer);
    } else if (input_buffer) {
      snprintf(prompt_buf, sizeof(prompt_buf), "%s..|%s", clr(NY_CLR_YELLOW),
               clr(NY_CLR_RESET));
      prompt = prompt_buf;
      repl_indent_next = repl_calc_indent(input_buffer);
    } else {
      const char *mode_tag = "";
      char mode_buf[32];
      if (std_mode == STD_MODE_NONE) {
        snprintf(mode_buf, sizeof(mode_buf), "[none]");
        mode_tag = mode_buf;
      } else if (std_mode == STD_MODE_FULL) {
        snprintf(mode_buf, sizeof(mode_buf), "[full]");
        mode_tag = mode_buf;
      }
      const char *base;
      if (color_mode) {
        base = last_status ? "\033[31mny!\033[0m" : "\033[36mny\033[0m";
      } else {
        base = last_status ? "ny!" : "ny";
      }
      if (mode_tag[0]) {
        if (color_mode) {
          snprintf(prompt_buf, sizeof(prompt_buf), "%s\033[90m%s\033[0m>", base,
                   mode_tag);
        } else {
          snprintf(prompt_buf, sizeof(prompt_buf), "%s%s>", base, mode_tag);
        }
      } else {
        snprintf(prompt_buf, sizeof(prompt_buf), "%s>", base);
      }
      prompt = prompt_buf;
    }
    char *line = NULL;
    int from_init = 0;
    if (init_line_idx < init_lines_len) {
      line = ny_strdup(init_lines[init_line_idx++]);
      from_init = 1;
    } else if (batch_mode) {
      break;
    } else if (tty_in) {
      line = ny_readline(prompt);
      if (line && g_repl_sigint) {
        free(line);
        line = NULL;
      }
      if (line && tty_in) {
      }
    } else {
      size_t cap = 256;
      size_t len = 0;
      char *buf = malloc(cap);
      if (!buf)
        break;
      int ch;
      while ((ch = fgetc(stdin)) != EOF) {
        if (ch == '\n')
          break;
        if (len + 1 >= cap &&
            !repl_reserve_char_capacity(&buf, &cap, len + 2, 256)) {
          free(buf);
          buf = NULL;
          break;
        }
        buf[len++] = (char)ch;
      }
      if (!buf)
        break;
      if (len == 0 && ch == EOF) {
        free(buf);
        buf = NULL;
        break;
      }
      buf[len] = '\0';
      line = buf;
    }
    if (!line) {
      if (input_buffer) {
        free(input_buffer);
        input_buffer = NULL;
        printf("\nCanceled multiline input\n");
        last_status = 0;
        continue;
      }
      if (g_repl_sigint) {
        if (!input_buffer) {
          repl_restore_terminal_state();
          break;
        }
        last_status = 0;
        if (input_buffer) {
          free(input_buffer);
          input_buffer = NULL;
          printf("Canceled multiline input\n");
        }
        repl_restore_terminal_state();
        continue;
      }
      break;
    }
    if (g_repl_sigint && !from_init) {
      free(line);
      if (input_buffer) {
        free(input_buffer);
        input_buffer = NULL;
        last_status = 0;
        printf("Canceled multiline input\n");
        repl_restore_terminal_state();
        continue;
      }
      repl_restore_terminal_state();
      break;
    }
    if (input_buffer && !from_init) {
      char *line_trimmed = ltrim(line);
      rtrim_inplace(line_trimmed);
      if (!strcmp(line_trimmed, ":cancel") || !strcmp(line_trimmed, ":c")) {
        free(line);
        free(input_buffer);
        input_buffer = NULL;
        last_status = 0;
        printf("Canceled multiline input\n");
        continue;
      }
    }
    if (input_buffer && !from_init && strlen(line) == 0) {
      free(line);
      if (!is_input_complete(input_buffer)) {
        continue;
      }
      goto process_input;
    }
    if (!from_init && strlen(line) == 0 && !input_buffer) {
      free(line);
      continue;
    }
    if (input_buffer) {
      size_t old_len = strlen(input_buffer);
      size_t line_len = strlen(line);
      size_t len = old_len + line_len + 2;
      char *grown = realloc(input_buffer, len);
      if (!grown) {
        fprintf(stderr, "oom\n");
        free(input_buffer);
        free(line);
        input_buffer = NULL;
        break;
      }
      input_buffer = grown;
      input_buffer[old_len] = '\n';
      memcpy(input_buffer + old_len + 1, line, line_len + 1);
      free(line);
    } else {
      input_buffer = line;
    }
    if (!is_input_complete(input_buffer)) {
      continue;
    }
  process_input:;
    char *full_input = input_buffer;
    input_buffer = NULL;
    if (!full_input || !*full_input) {
      if (full_input)
        free(full_input);
      continue;
    }
    if (!from_init)
      ny_readline_add_history(full_input);
    char *trimmed = ltrim(full_input);
    if (trimmed[0] == ':') {
      char *p = ltrim(trimmed + 1);
      char *cmd = p;
      while (*p && !isspace((unsigned char)*p))
        p++;
      if (*p) {
        *p = '\0';
        p++;
      }
      p = ltrim(p);
      rtrim_inplace(p);
      const char *cn = cmd;
      if (!strcmp(cn, "h") || !strcmp(cn, "doc"))
        cn = "help";
      if (!strcmp(cn, "q") || !strcmp(cn, "quit"))
        cn = "exit";
      if (!strcmp(cn, "complete")) {
        size_t count = 0;
        char **completions = nytrix_get_completions_for_prefix(p, &count);
        printf("__COMPLETIONS__\n");
        if (completions) {
          for (size_t i = 0; i < count; i++)
            printf("%s\n", completions[i]);
          nytrix_free_completions(completions, count);
        }
        printf("__END__\n");
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "exit")) {
        free(full_input);
        break;
      }
      if (!strcmp(cn, "clear") || !strcmp(cn, "cls")) {
        printf("\033[2J\033[H");
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "reset")) {
        doclist_free(&docs);
        memset(&docs, 0, sizeof(docs));
        add_builtin_docs(&docs);
        repl_shutdown_engine();
        if (g_repl_user_source) {
          free(g_repl_user_source);
          g_repl_user_source = NULL;
          g_repl_user_source_len = 0;
        }
        g_repl_std_root_lazy = 0;
        g_repl_lazy_docs_loaded = 0;
        repl_set_last_expand_source(NULL, NULL);
        repl_init_engine(std_mode, &docs);
        printf("Reset\n");
        last_status = 0;
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "pwd")) {
        char buf[PATH_MAX];
        if (getcwd(buf, sizeof(buf)))
          printf("%s\n", buf);
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "cd")) {
        const char *dst = *p ? p : getenv("HOME");
#ifdef _WIN32
        if (!dst)
          dst = getenv("USERPROFILE");
#endif
        if (chdir(dst ? dst : ".") != 0)
          perror("cd");
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "ls")) {
        const char *path = *p ? p : ".";
        DIR *d = opendir(path);
        if (d) {
          struct dirent *de;
          while ((de = readdir(d))) {
            if (de->d_name[0] == '.')
              continue;
            printf("%s  ", de->d_name);
          }
          printf("\n");
          closedir(d);
        } else
          perror("ls");
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "time")) {
        g_repl_timing = !g_repl_timing;
        printf("Timing: %s\n", g_repl_timing ? "on" : "off");
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "history") || !strcmp(cn, "hist")) {
        printf("History fully implemented using arrows, to list just open the "
               "file.\n");
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "save")) {
        if (!*p)
          printf("Usage: :save <filename>\n");
        else if (repl_write_session_source(p) != 0)
          perror("save");
        else if (g_repl_user_source && *g_repl_user_source)
          printf("Session source saved to %s\n", p);
        else
          printf("Session source saved to %s (empty)\n", p);
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "snapshot")) {
        char *argp = p;
        char *snap_path = repl_next_command_arg(&argp);
        char *out_path = NULL;
        char *extra = NULL;
        bool usage_error = false;
        if (snap_path &&
            (!strcmp(snap_path, "-o") || !strcmp(snap_path, "--output"))) {
          free(snap_path);
          snap_path = NULL;
          out_path = repl_next_command_arg(&argp);
          if (!out_path)
            usage_error = true;
        }
        while ((extra = repl_next_command_arg(&argp)) != NULL) {
          if (!strcmp(extra, "-o") || !strcmp(extra, "--output")) {
            free(extra);
            free(out_path);
            out_path = repl_next_command_arg(&argp);
            if (!out_path) {
              usage_error = true;
              break;
            }
            continue;
          }
          if (!out_path) {
            out_path = extra;
          } else {
            free(extra);
            usage_error = true;
            break;
          }
        }
        const char *path =
            (snap_path && *snap_path) ? snap_path : "repl.snapshot.nys";
        if (usage_error) {
          printf("Usage: :snapshot [snapfile] [-o executable]\n");
          last_status = 1;
        } else if (repl_write_snapshot_image(path, std_mode) != 0) {
          perror("snapshot");
          last_status = 1;
        } else {
          printf("Snapshot saved to %s (%zu bytes source)\n", path,
                 repl_snapshot_source_len());
          fflush(stdout);
          last_status = 0;
          if (out_path && *out_path) {
            int rc = repl_compile_snapshot_image(path, out_path);
            if (rc == 0) {
              printf("AOT snapshot exported to %s\n", out_path);
            } else {
              printf("AOT snapshot export failed with status %d\n", rc);
              last_status = 1;
            }
          }
        }
        free(snap_path);
        free(out_path);
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "load")) {
        if (!*p)
          printf("Usage: :load <filename>\n");
        else {
          char *src = repl_read_file(p);
          if (src) {
            if (strncmp(src, REPL_SNAPSHOT_MAGIC,
                        sizeof(REPL_SNAPSHOT_MAGIC) - 1) == 0) {
              last_status =
                  repl_load_snapshot_image(p, src, std_mode, &docs) == 0 ? 0
                                                                         : 1;
            } else {
              repl_set_last_expand_source(src, p);
              repl_append_user_source(src);
              printf("Loaded %s (%zu bytes)\n", p, strlen(src));
              parser_t ps;
              parser_init(&ps, src, p);
              program_t pr = parse_program(&ps);
              if (!ps.had_error)
                doclist_add_from_prog(&docs, &pr);
              program_free(&pr, ps.arena);
              last_status = 0;
            }
            free(src);
          } else {
            perror("load");
            last_status = 1;
          }
        }
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "run")) {
        if (!*p) {
          printf("Usage: :run <filename>\n");
        } else {
          char *src = repl_read_file(p);
          if (src) {
            repl_set_last_expand_source(src, p);
            last_status =
                repl_eval_snippet(src, 1, NULL, std_mode, tty_in, &docs, 0);
            free(src);
            repl_restore_terminal_state();
          } else {
            perror("run");
            last_status = 1;
          }
        }
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "vars")) {
        if (g_repl_user_source)
          printf("%s--- Persistent Source ---%s\n%s", clr(NY_CLR_BOLD),
                 clr(NY_CLR_RESET), g_repl_user_source);
        else
          printf("No persistent variables defined.\n");
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "std")) {
        printf("Std mode: %s\n", repl_std_mode_name(std_mode));
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "trace")) {
        if (!*p) {
          repl_print_trace_status();
          printf("Usage: :trace "
                 "[on|off|calls|values|verbose|compiler|json|ir|full|phase] "
                 "[filter]\n");
          free(full_input);
          continue;
        }
        char mode[32];
        size_t mi = 0;
        while (*p && !isspace((unsigned char)*p) && mi + 1 < sizeof(mode))
          mode[mi++] = *p++;
        mode[mi] = '\0';
        while (*p && isspace((unsigned char)*p))
          p++;
        const char *filter = *p ? p : NULL;
        int rebuild = 0;
        if (!strcmp(mode, "off") || !strcmp(mode, "0") ||
            !strcmp(mode, "disable")) {
          g_repl_exec_trace_enabled = 0;
          g_repl_exec_trace_values = 0;
          g_repl_exec_trace_verbose = 0;
          g_repl_exec_trace_calls = 0;
          g_repl_exec_trace_compile = 0;
          g_repl_exec_trace_json = 0;
          g_repl_exec_trace_ir = 0;
          repl_set_exec_trace_filter(NULL);
          repl_apply_exec_trace_env();
          rebuild = 1;
          printf("Trace disabled");
        } else if (!strcmp(mode, "phase")) {
          g_repl_phase_trace = !g_repl_phase_trace;
          printf("Phase trace %s", g_repl_phase_trace ? "enabled" : "disabled");
        } else {
          if (!strcmp(mode, "on") || !strcmp(mode, "calls")) {
            g_repl_exec_trace_enabled = 1;
            g_repl_exec_trace_calls = 1;
            g_repl_exec_trace_values = 0;
            g_repl_exec_trace_verbose = 0;
            g_repl_exec_trace_compile = 0;
            g_repl_exec_trace_json = 0;
            g_repl_exec_trace_ir = 0;
          } else if (!strcmp(mode, "values")) {
            g_repl_exec_trace_enabled = 1;
            g_repl_exec_trace_calls = 1;
            g_repl_exec_trace_values = 1;
            g_repl_exec_trace_verbose = 0;
            g_repl_exec_trace_compile = 0;
            g_repl_exec_trace_json = 0;
            g_repl_exec_trace_ir = 0;
          } else if (!strcmp(mode, "verbose")) {
            g_repl_exec_trace_enabled = 1;
            g_repl_exec_trace_calls = 1;
            g_repl_exec_trace_values = 1;
            g_repl_exec_trace_verbose = 1;
            g_repl_exec_trace_compile = 0;
            g_repl_exec_trace_json = 0;
            g_repl_exec_trace_ir = 0;
          } else if (!strcmp(mode, "full") || !strcmp(mode, "deep")) {
            g_repl_exec_trace_enabled = 1;
            g_repl_exec_trace_calls = 1;
            g_repl_exec_trace_values = 1;
            g_repl_exec_trace_verbose = 1;
            g_repl_exec_trace_compile = 1;
            g_repl_exec_trace_json = 1;
            g_repl_exec_trace_ir = 1;
          } else if (!strcmp(mode, "compiler") || !strcmp(mode, "compile") ||
                     !strcmp(mode, "expand")) {
            g_repl_exec_trace_enabled = 0;
            g_repl_exec_trace_calls = 0;
            g_repl_exec_trace_values = 0;
            g_repl_exec_trace_verbose = 0;
            g_repl_exec_trace_compile = 1;
            g_repl_exec_trace_json = 0;
            g_repl_exec_trace_ir = 0;
          } else if (!strcmp(mode, "json") || !strcmp(mode, "ast")) {
            g_repl_exec_trace_enabled = 0;
            g_repl_exec_trace_calls = 0;
            g_repl_exec_trace_values = 0;
            g_repl_exec_trace_verbose = 0;
            g_repl_exec_trace_compile = 1;
            g_repl_exec_trace_json = 1;
            g_repl_exec_trace_ir = 0;
          } else if (!strcmp(mode, "ir") || !strcmp(mode, "llvm")) {
            g_repl_exec_trace_enabled = 0;
            g_repl_exec_trace_calls = 0;
            g_repl_exec_trace_values = 0;
            g_repl_exec_trace_verbose = 0;
            g_repl_exec_trace_compile = 1;
            g_repl_exec_trace_json = 0;
            g_repl_exec_trace_ir = 1;
          } else {
            g_repl_exec_trace_enabled = 1;
            g_repl_exec_trace_calls = 1;
            g_repl_exec_trace_values = 1;
            g_repl_exec_trace_verbose = 1;
            g_repl_exec_trace_compile = 1;
            g_repl_exec_trace_json = 1;
            g_repl_exec_trace_ir = 1;
            filter = mode;
          }
          repl_set_exec_trace_filter(filter);
          repl_apply_exec_trace_env();
          rebuild = 1;
          printf("Trace enabled");
          if (g_repl_exec_trace_compile && !g_repl_exec_trace_enabled)
            printf(" (compiler)");
          else if (g_repl_exec_trace_values)
            printf(" (values)");
          else
            printf(" (calls)");
          if (g_repl_exec_trace_verbose)
            printf(" +loc");
          if (g_repl_exec_trace_compile) {
            if (g_repl_exec_trace_json)
              printf(" +graph +ast");
            else
              printf(" +graph");
          }
          if (g_repl_exec_trace_ir)
            printf(" +ir");
          if (g_repl_exec_trace_filter && *g_repl_exec_trace_filter)
            printf(" filter=%s", g_repl_exec_trace_filter);
        }
        if (rebuild) {
          int rebuild_status =
              repl_rebuild_engine_from_persistent(std_mode, &docs);
          printf("; reloaded persistent defs%s\n",
                 g_repl_user_source && *g_repl_user_source
                     ? " (runtime state reset)"
                     : "");
          last_status = rebuild_status;
        } else {
          printf("\n");
          last_status = 0;
        }
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "expand")) {
        int meta_trace = 0;
        int include_json = 0;
        int from_file = 0;
        while (*p && isspace((unsigned char)*p))
          p++;
        while (*p) {
          char mode[32];
          size_t mi = 0;
          char *mode_start = p;
          while (*p && !isspace((unsigned char)*p) && mi + 1 < sizeof(mode))
            mode[mi++] = *p++;
          mode[mi] = '\0';
          while (*p && isspace((unsigned char)*p))
            p++;
          if (!strcmp(mode, "trace") || !strcmp(mode, "--trace")) {
            meta_trace = 1;
            continue;
          }
          if (!strcmp(mode, "json") || !strcmp(mode, "ast") ||
              !strcmp(mode, "--json")) {
            include_json = 1;
            continue;
          }
          if (!strcmp(mode, "full") || !strcmp(mode, "deep")) {
            meta_trace = 1;
            include_json = 1;
            continue;
          }
          if (!strcmp(mode, "file") || !strcmp(mode, "--file")) {
            from_file = 1;
            continue;
          }
          p = mode_start;
          break;
        }
        while (*p && isspace((unsigned char)*p))
          p++;
        const char *payload = *p ? p : NULL;
        const char *source = NULL;
        const char *source_name = NULL;
        char *owned_source = NULL;
        if (from_file && (!payload || strcmp(payload, "last") == 0)) {
          printf("Usage: :expand file <path>\n");
          last_status = 1;
          free(full_input);
          continue;
        }
        if (payload && strcmp(payload, "last") != 0) {
          if (from_file) {
            owned_source = repl_read_file(payload);
            if (!owned_source) {
              perror("expand");
              last_status = 1;
              free(full_input);
              continue;
            }
            source = owned_source;
            source_name = payload;
            repl_set_last_expand_source(owned_source, payload);
          } else {
            source = payload;
            source_name = "<repl:expand>";
            repl_set_last_expand_source(payload, source_name);
          }
        } else if (g_repl_last_expand_source && *g_repl_last_expand_source) {
          source = g_repl_last_expand_source;
          source_name = (g_repl_last_expand_name && *g_repl_last_expand_name)
                            ? g_repl_last_expand_name
                            : "<repl:last>";
        } else {
          printf("Usage: :expand [trace|json|full] [code]\n");
          printf("       :expand file <path>\n");
          printf("       :expand [trace|json|full] last\n");
          last_status = 1;
          free(full_input);
          continue;
        }
        last_status = repl_expand_source(source, source_name, NULL, meta_trace,
                                         include_json, 0);
        free(owned_source);
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "env")) {
#ifdef _WIN32
        char **envp = _environ;
#else
        char **envp = environ;
#endif
        for (char **env = envp; env && *env; env++)
          printf("%s\n", *env);
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "help")) {
        if (*p) {
          if (!strcmp(p, "syntax")) {
            printf("%sNytrix Syntax Quick Reference:%s\n", clr(NY_CLR_BOLD),
                   clr(NY_CLR_RESET));
            printf("  %-30s %s\n", "def/mut name = val",
                   "Declare variable (not 'let')");
            printf("  %-30s %s\n", "fn name(params) Ret { }",
                   "Function with return type");
            printf("  %-30s %s\n", "Type name field syntax",
                   "Function param / typed field");
            printf("  %-30s %s\n", ";", "Line comment (not '//')");
            printf("  %-30s %s\n", "dict() / []", "Empty dict / list literal");
            printf("  %-30s %s\n", "{key: val} / {a, b, c}",
                   "Dict literal / set literal");
            printf("  %-30s %s\n", "case val { pat -> expr }",
                   "Pattern match (not 'switch')");
            printf("  %-30s %s\n", "list.append(x)",
                   "Returns new list (not in-place)");
            printf("  %-30s %s\n", "f\"{expr}\"", "String interpolation");
            printf("  %-30s %s\n", "if expr { } else { }",
                   "If-else (no parentheses)");
            printf("  %-30s %s\n", "while expr { }", "While loop");
            printf("  %-30s %s\n", "for x in iter { }", "For loop");
            printf("  %-30s %s\n", "struct Name { T: a }",
                   "Struct with type-first fields");
            printf("  %-30s %s\n", "Name(val1, val2)",
                   "Struct constructor (positional)");
            printf("  %-30s %s\n", "use std.core", "Import module");
            printf("  %-30s %s\n", "embed \"file\"",
                   "Embed file at compile time");
            printf("\n%sTip:%s Use ':doc <name>' to look up any symbol\n",
                   clr(NY_CLR_YELLOW), clr(NY_CLR_RESET));
          } else {
            char query[512];
            repl_normalize_doc_query(p, query, sizeof(query));
            const char *lookup = query[0] ? query : p;
            int printed = repl_print_help_query(&docs, lookup);
            if (!printed)
              printf("%sNo documentation found for '%s'%s\n", clr(NY_CLR_RED),
                     lookup, clr(NY_CLR_RESET));
          }
        } else {
          printf("%sCommands:%s\n", clr(NY_CLR_BOLD NY_CLR_CYAN),
                 clr(NY_CLR_RESET));
          printf("  %-15s Exit the REPL\n", ":exit/:quit/:q");
          printf("  %-15s Clear the screen\n", ":clear/:cls");
          printf("  %-15s Reset the REPL state\n", ":reset");
          printf("  %-15s Cancel current multiline input\n", ":cancel/:c");
          printf("  %-15s Toggle execution timing\n", ":time");
          printf("  %-15s Toggle scoped execution tracing\n",
                 ":trace [mode] [filter]");
          printf("  %-15s Inspect AST/expansion of code or last input\n",
                 ":expand [mode] [code]");
          printf("  %-15s Show persistent source (defs/vars)\n", ":vars");
          printf("  %-15s Show environment variables\n", ":env");
          printf("  %-15s Show command history\n", ":history/:hist");
          printf("  %-15s Print working directory\n", ":pwd");
          printf("  %-15s List files in current directory\n", ":ls");
          printf("  %-15s Change current directory\n", ":cd [path]");
          printf("  %-15s Save a REPL image, optionally AOT-export it\n",
                 ":snapshot [file]");
          printf("  %-15s Load a file\n", ":load [file]");
          printf("  %-15s Evaluate and run a file\n", ":run [file]");
          printf("  %-15s Save session/source to file\n", ":save [file]");
          printf("  %-15s Show standard library info\n", ":std");
          printf("  %-15s Help for commands, modules, and symbols\n",
                 ":help/:h/:doc [name]");
          printf("\n%sPackages:%s\n", clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
          for (size_t i = 0; i < ny_std_package_count(); ++i)
            printf("  %s\n", ny_std_package_name(i));
          printf("\n%sNavigation Examples:%s\n", clr(NY_CLR_BOLD),
                 clr(NY_CLR_RESET));
          printf("  :help std             - Browse the std package\n");
          printf("  :help std.*           - List top-level std namespaces\n");
          printf(
              "  :help std.os.         - List direct children under std.os\n");
          printf("  :help std.os.path     - Show module docs and members\n");
          printf("  :help std.os.path.sep - Show a specific symbol\n");
          printf("  :help sep             - Resolve a symbol from imported "
                 "modules\n");
          printf("  :trace values aes     - Trace matching calls with return "
                 "values\n");
          printf("  :trace compiler       - Show compiler-side AST/expand info "
                 "per eval\n");
          printf("  :trace ir             - Show compact LLVM IR for the "
                 "current eval\n");
          printf("  :trace full std.os    - Graph+AST+IR plus runtime trace "
                 "for matching calls\n");
          printf("  :expand for(x in xs){x} - Show compact AST/expansion for "
                 "inline code\n");
          printf("  :expand trace last      - Re-expand the last snippet with "
                 "call/op graph\n");
          printf("  :expand file foo.ny     - Inspect a whole file without "
                 "executing it\n");
          printf("  :snapshot app.nys -o app - Save image-backed source and "
                 "standalone binary\n");
          printf("  :load app.nys          - Restore a REPL snapshot image\n");
        }
        free(full_input);
        continue;
      }
      printf("Unknown command: :%s\n", cn);
      last_status = 1;
      free(full_input);
      continue;
    }
    trimmed = repl_skip_leading_noncode(full_input);
    if (trimmed[0] == '\0') {
      free(full_input);
      continue;
    }
    if (trimmed[0] == '#' && trimmed[1] == '!') {
      char *nl = strchr(trimmed, '\n');
      if (!nl) {
        free(full_input);
        continue;
      }
      memmove(trimmed, nl + 1, strlen(nl + 1) + 1);
      trimmed = ltrim(full_input);
      if (trimmed[0] == '\0') {
        free(full_input);
        continue;
      }
    }
    int is_stmt = is_repl_stmt(full_input);
    char *eval_input = full_input;
    if (!strncmp(trimmed, "use ", 4)) {
      char *mod_name = ltrim(trimmed + 4);
      char *end = mod_name;
      while (*end && !isspace((unsigned char)*end) && *end != ';' &&
             *end != '(')
        end++;
      char *name = ny_strndup(mod_name, (size_t)(end - mod_name));
      const char *stmt_end = trimmed;
      while (*stmt_end && *stmt_end != '\n')
        stmt_end++;
      bool bare_std = repl_line_is_bare_std_use(trimmed, stmt_end);
      if (bare_std)
        repl_enable_lazy_std_root(std_mode, &docs);
      else
        repl_ensure_module(name, std_mode, &docs);
      free(name);
    }
    repl_set_last_expand_source(full_input, "<repl:last>");
    char *an = repl_assignment_target(eval_input);
    last_status =
        repl_eval_snippet(eval_input, is_stmt, an, std_mode, tty_in, &docs, 0);
    repl_restore_terminal_state();
    if (an)
      free(an);
    free(full_input);
  }
  repl_set_last_expand_source(NULL, NULL);
  if (history_path[0] && !fast_batch_exit)
    ny_readline_write_history(history_path);
  if (!fast_batch_exit)
    doclist_free(&docs);
  if (init_lines) {
    for (size_t i = 0; i < init_lines_len; i++) {
      free(init_lines[i]);
    }
    free(init_lines);
  }
  if (!fast_batch_exit) {
#ifdef __APPLE__
    repl_shutdown_engine();
#else
    repl_drop_engine_refs_on_exit();
#endif
  }
#ifdef _WIN32
  signal(SIGINT, prev_sigint);
#else
  sigaction(SIGINT, &prev_sigint, NULL);
#endif
#ifdef __APPLE__
  if (!fast_batch_exit) {
    repl_restore_terminal_state();
    fflush(NULL);
    _Exit(0);
  }
#endif
}
