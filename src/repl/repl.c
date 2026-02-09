#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/common.h"
#include "base/loader.h"
#include "base/util.h"
#include "code/code.h"
#include "code/jit.h"
#include "priv.h"
#include "repl/types.h"
#include "rt/runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static int repl_move_up(int count, int key);
static int repl_move_down(int count, int key);

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

const doc_list_t *g_repl_docs = NULL;
static VEC(char *) g_repl_loading_modules = {0};

static std_mode_t g_repl_std_override = (std_mode_t)-1;
/* effectively the mode REPL was initialized with */
static std_mode_t g_repl_effective_mode = STD_MODE_DEFAULT;
static int g_repl_plain = 0;

// Persistent JIT State
static LLVMContextRef g_repl_ctx = NULL;
static LLVMExecutionEngineRef g_repl_ee = NULL;
static codegen_t g_repl_cg = {0};
static int g_eval_count = 0;
static LLVMBuilderRef g_repl_builder = NULL;
static char *g_std_src_cached_persistent = NULL;
static int g_repl_timing = 0;

static void repl_ensure_module(const char *name, std_mode_t std_mode,
                               doc_list_t *docs);

void ny_repl_set_std_mode(std_mode_t mode) { g_repl_std_override = mode; }
void ny_repl_set_plain(int plain) { g_repl_plain = plain; }

static int repl_move_up(int count, int key) {
  int p = rl_point;
  int last_nl = -1;
  for (int i = p - 1; i >= 0; i--) {
    if (rl_line_buffer[i] == '\n') {
      last_nl = i;
      break;
    }
  }

  if (last_nl != -1) {
    int col = p - (last_nl + 1);
    int prev_nl = -1;
    for (int i = last_nl - 1; i >= 0; i--) {
      if (rl_line_buffer[i] == '\n') {
        prev_nl = i;
        break;
      }
    }
    int prev_start = prev_nl + 1;
    int prev_len = last_nl - prev_start;
    if (col > prev_len)
      col = prev_len;
    rl_point = prev_start + col;
    rl_redisplay();
    return 0;
  }
  return rl_get_previous_history(count, key);
}

static int repl_move_down(int count, int key) {
  int p = rl_point;
  int next_nl = -1;
  for (int i = p; rl_line_buffer[i]; i++) {
    if (rl_line_buffer[i] == '\n') {
      next_nl = i;
      break;
    }
  }

  if (next_nl != -1) {
    int last_nl = -1;
    for (int i = p - 1; i >= 0; i--) {
      if (rl_line_buffer[i] == '\n') {
        last_nl = i;
        break;
      }
    }
    int col = p - (last_nl + 1);
    int next_next_nl = -1;
    for (int i = next_nl + 1; rl_line_buffer[i]; i++) {
      if (rl_line_buffer[i] == '\n') {
        next_next_nl = i;
        break;
      }
    }
    int next_start = next_nl + 1;
    int next_len = (next_next_nl != -1)
                       ? (next_next_nl - next_start)
                       : (int)strlen(rl_line_buffer + next_start);
    if (col > next_len)
      col = next_len;
    rl_point = next_start + col;
    rl_redisplay();
    return 0;
  }
  return rl_get_next_history(count, key);
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

static void map_rt_syms_persistent(LLVMModuleRef mod,
                                   LLVMExecutionEngineRef ee) {
#define RT_DEF(name, p, args, sig, doc) {name, (void *)p},
#define RT_GV(name, p, t, doc) {name, (void *)&p},
  struct {
    const char *n;
    void *p;
  } syms[] = {
#include "rt/defs.h"
      {NULL, NULL}};
#undef RT_DEF
#undef RT_GV
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    if (LLVMGetFunctionAddress(ee, LLVMGetValueName(f)))
      continue;
    const char *fn_name_llvm = LLVMGetValueName(f);
    for (int i = 0; syms[i].n; ++i) {
      size_t slen = strlen(syms[i].n);
      if (strncmp(fn_name_llvm, syms[i].n, slen) == 0 &&
          (fn_name_llvm[slen] == '\0' || fn_name_llvm[slen] == '.')) {
        LLVMAddGlobalMapping(ee, f, syms[i].p);
        break;
      }
    }
  }
}

static void repl_init_engine(std_mode_t mode, doc_list_t *docs) {
  if (g_repl_ctx)
    return;

  LLVMLinkInMCJIT();
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();

  g_repl_ctx = LLVMContextCreate();
  LLVMModuleRef mod =
      LLVMModuleCreateWithNameInContext("repl_base", g_repl_ctx);
  g_repl_builder = LLVMCreateBuilderInContext(g_repl_ctx);

  if (mode != STD_MODE_NONE) {
    const char *prebuilt = getenv("NYTRIX_STD_PREBUILT");
    if (!prebuilt || access(prebuilt, R_OK) != 0) {
      char *exe_dir = ny_get_executable_dir();
      if (exe_dir) {
        static char path_buf[4096];
        snprintf(path_buf, sizeof(path_buf), "%s/std.ny", exe_dir);
        if (access(path_buf, R_OK) == 0)
          prebuilt = path_buf;
        else {
          snprintf(path_buf, sizeof(path_buf), "%s/std_bundle.ny", exe_dir);
          if (access(path_buf, R_OK) == 0)
            prebuilt = path_buf;
          else {
            snprintf(path_buf, sizeof(path_buf), "%s/../share/nytrix/std.ny",
                     exe_dir);
            if (access(path_buf, R_OK) == 0)
              prebuilt = path_buf;
            else {
              snprintf(path_buf, sizeof(path_buf),
                       "%s/../share/nytrix/std_bundle.ny", exe_dir);
              if (access(path_buf, R_OK) == 0)
                prebuilt = path_buf;
            }
          }
        }
      }
    }
    if (!prebuilt || access(prebuilt, R_OK) != 0) {
#ifdef NYTRIX_STD_PATH
      if (access(NYTRIX_STD_PATH, R_OK) == 0)
        prebuilt = NYTRIX_STD_PATH;
#endif
    }
    if (prebuilt && access(prebuilt, R_OK) == 0)
      g_std_src_cached_persistent = repl_read_file(prebuilt);
    if (!g_std_src_cached_persistent)
      g_std_src_cached_persistent = ny_build_std_bundle(NULL, 0, mode, 0, NULL);

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
        codegen_emit(&g_repl_cg);

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
  LLVMInitializeMCJITCompilerOptions(&options, sizeof(options));
  options.CodeModel = LLVMCodeModelLarge;
  options.OptLevel = 0;
  options.EnableFastISel = 0;
  char *err = NULL;
  if (LLVMCreateMCJITCompilerForModule(&g_repl_ee, mod, &options,
                                       sizeof(options), &err) != 0) {
    fprintf(stderr, "JIT Error: %s\n", err);
    LLVMDisposeMessage(err);
  } else {
    map_rt_syms_persistent(mod, g_repl_ee);
    register_jit_symbols(g_repl_ee, mod, &g_repl_cg);
  }
}

static void repl_shutdown_engine(void) {
  if (g_repl_ee) {
    LLVMDisposeExecutionEngine(g_repl_ee);
    g_repl_ee = NULL;
    g_repl_cg.module = NULL; // ee owned it
  } else if (g_repl_cg.module) {
    LLVMDisposeModule(g_repl_cg.module);
    g_repl_cg.module = NULL;
  }

  if (g_repl_builder) {
    LLVMDisposeBuilder(g_repl_builder);
    g_repl_builder = NULL;
    g_repl_cg.builder = NULL;
  }
  if (g_repl_ctx) {
    LLVMContextDispose(g_repl_ctx);
    g_repl_ctx = NULL;
    g_repl_cg.ctx = NULL;
  }

  codegen_dispose(&g_repl_cg);
  memset(&g_repl_cg, 0, sizeof(codegen_t));

  g_eval_count = 0;
  if (g_std_src_cached_persistent) {
    free(g_std_src_cached_persistent);
    g_std_src_cached_persistent = NULL;
  }
  vec_free(&g_repl_loading_modules);
}

static int repl_eval_snippet(const char *full_input, int is_stmt, char *an,
                             std_mode_t std_mode, int tty_in, doc_list_t *docs,
                             int from_init) {
  char *trimmed = ltrim((char *)full_input);
  int show = (!is_stmt && std_mode != STD_MODE_NONE && tty_in);
  int show_an = (an && std_mode != STD_MODE_NONE && tty_in);

  size_t blen = strlen(full_input) + (an ? strlen(an) : 0) + 128;
  char *body = malloc(blen);
  if (show)
    snprintf(body, blen, "repl_show(%s\n)\n", full_input);
  else if (!is_stmt)
    snprintf(body, blen, "return %s\n", full_input);
  else if (show_an)
    snprintf(body, blen, "%s\nrepl_show(%s\n)\n", full_input, an);
  else
    snprintf(body, blen, "%s\n", full_input);

  clock_t t0 = clock();
  parser_t ps;
  parser_init(&ps, body, "<repl_input>");
  program_t *pr = malloc(sizeof(program_t));
  *pr = parse_program(&ps);

  int last_status = 0;
  bool persistent = false;
  if (!ps.had_error) {
    // Check for imports
    for (size_t i = 0; i < pr->body.len; ++i) {
      stmt_t *s = pr->body.data[i];
      if (s->kind == NY_S_USE && s->as.use.module) {
        repl_ensure_module(s->as.use.module, std_mode, docs);
      }
    }

    LLVMModuleRef eval_mod =
        LLVMModuleCreateWithNameInContext("repl_eval", g_repl_ctx);
    LLVMBuilderRef eval_builder = LLVMCreateBuilderInContext(g_repl_ctx);
    codegen_t cg;
    codegen_init_with_context(&cg, pr, ps.arena, eval_mod, g_repl_ctx,
                              eval_builder);

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
    for (size_t i = 0; i < g_repl_cg.enums.len; i++) {
      enum_def_t *e = g_repl_cg.enums.data[i];
      if (e)
        vec_push(&cg.enums, e);
    }
    for (size_t i = 0; i < g_repl_cg.use_modules.len; i++)
      vec_push(&cg.use_modules, ny_strdup(g_repl_cg.use_modules.data[i]));

    codegen_emit(&cg);
    char fn_name[64];
    snprintf(fn_name, sizeof(fn_name), "__eval_%d", g_eval_count++);
    LLVMValueRef eval_fn = codegen_emit_script(&cg, fn_name);
    (void)eval_fn;

    if (cg.had_error) {
      last_status = 1;
    } else if (g_repl_ee) {
      LLVMAddModule(g_repl_ee, eval_mod);
      map_rt_syms_persistent(eval_mod, g_repl_ee);
      register_jit_symbols(g_repl_ee, eval_mod, &cg);

      for (size_t i = 0; i < cg.interns.len; i++) {
        if (cg.interns.data[i].gv)
          LLVMAddGlobalMapping(g_repl_ee, cg.interns.data[i].gv,
                               (void *)((char *)cg.interns.data[i].data - 64));
        if (cg.interns.data[i].val)
          LLVMAddGlobalMapping(g_repl_ee, cg.interns.data[i].val,
                               &cg.interns.data[i].data);
      }

      // Force resolution of new global variables to ensure they are emitted
      for (size_t i = g_repl_cg.global_vars.len; i < cg.global_vars.len; i++) {
        if (cg.global_vars.data[i].name && cg.global_vars.data[i].value) {
          LLVMGetGlobalValueAddress(g_repl_ee, cg.global_vars.data[i].name);
        }
      }

      uint64_t addr = LLVMGetFunctionAddress(g_repl_ee, fn_name);
      if (addr) {
        ((void (*)(void))addr)();
        last_status = 0;

        char *undef_name = NULL;
        if (!strncmp(trimmed, "undef ", 6)) {
          char *up = trimmed + 6;
          while (*up == ' ' || *up == '\t')
            up++;
          char *uend = up;
          while (*uend && !isspace((unsigned char)*uend) && *uend != ';')
            uend++;
          if (uend > up)
            undef_name = ny_strndup(up, (size_t)(uend - up));
        }

        if (undef_name) {
          repl_remove_def(undef_name);
          free(undef_name);
        } else if (is_persistent_def(full_input)) {
          persistent = true;
          if (!from_init)
            repl_append_user_source(full_input);
          repl_update_docs(docs, full_input);

          for (size_t i = 0; i < cg.fun_sigs.len; i++) {
            if (i >= g_repl_cg.fun_sigs.len) {
              fun_sig s = cg.fun_sigs.data[i];
              s.name = ny_strdup(s.name);
              if (s.link_name)
                s.link_name = ny_strdup(s.link_name);
              if (s.return_type)
                s.return_type = ny_strdup(s.return_type);
              s.stmt_t = NULL;
              s.owned = true;
              vec_push(&g_repl_cg.fun_sigs, s);
            }
          }
          for (size_t i = 0; i < cg.global_vars.len; i++) {
            if (i >= g_repl_cg.global_vars.len) {
              binding b = cg.global_vars.data[i];
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
        }
      }

      cg.fun_sigs.len = 0;
      cg.global_vars.len = 0;
      cg.aliases.len = 0;
      cg.import_aliases.len = 0;
      cg.use_modules.len = 0;

      // Move interns to persistent state so they aren't freed by
      // codegen_dispose
      for (size_t i = 0; i < cg.interns.len; i++) {
        vec_push(&g_repl_cg.interns, cg.interns.data[i]);
      }
      cg.interns.len = 0;
    }
    codegen_dispose(&cg);
  } else {
    repl_print_error_snippet(full_input, ps.cur.line, ps.cur.col);
    last_status = 1;
  }

  if (g_repl_timing && !from_init)
    printf("[Eval: %.3f ms]\n",
           (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);

  if (persistent) {
    vec_push(&g_repl_cg.extra_progs, pr);
    vec_push(&g_repl_cg.extra_arenas, ps.arena);
  } else {
    program_free(pr, ps.arena);
    free(pr);
  }
  free(body);
  return last_status;
}

static void repl_ensure_module(const char *name, std_mode_t std_mode,
                               doc_list_t *docs) {
  (void)std_mode;
  char *norm_name = normalize_module_name(name);
  for (size_t i = 0; i < g_repl_cg.use_modules.len; i++) {
    if (strcmp(g_repl_cg.use_modules.data[i], norm_name) == 0) {
      free(norm_name);
      return;
    }
  }
  for (size_t i = 0; i < g_repl_loading_modules.len; i++) {
    if (strcmp(g_repl_loading_modules.data[i], norm_name) == 0) {
      free(norm_name);
      return;
    }
  }

  char *entry_name = ny_strdup(norm_name);
  vec_push(&g_repl_loading_modules, entry_name);

  if (name[0] == '.' || name[0] == '/' || strchr(name, '/') != NULL) {
    char *src = repl_read_file(name);
    if (src) {
      repl_eval_snippet(src, 1, NULL, STD_MODE_NONE, 0, docs, 1);
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
  if (clean_name[0] == '.' || clean_name[0] == '/' ||
      strchr(clean_name, '/') != NULL) {
    static char cwd_buf[PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf)))
      entry_path = cwd_buf;
  }
  if (!entry_path)
    entry_path = ny_src_root();
  char *bundle = ny_build_std_bundle((const char **)&clean_name, 1,
                                     STD_MODE_NONE, 0, entry_path);
  if (bundle) {
    repl_eval_snippet(bundle, 1, NULL, STD_MODE_NONE, 0, docs, 1);
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
  (void)opt_level;
  (void)opt_pipeline;
  const char *plain = getenv("NYTRIX_REPL_PLAIN");
  if (g_repl_plain || (plain && plain[0] != '0') || !isatty(STDOUT_FILENO)) {
    color_mode = 0;
  }
  std_mode_t std_mode = STD_MODE_DEFAULT;
  if (g_repl_std_override != STD_MODE_DEFAULT)
    std_mode = g_repl_std_override;
  else {
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
  if (getenv("NYTRIX_REPL_TIME"))
    g_repl_timing = 1;

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

  if (!init_code && isatty(STDOUT_FILENO)) {
    printf("%sNytrix REPL%s %s(%s)%s - Type :help for commands\n",
           clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET), clr(NY_CLR_GRAY),
           repl_std_mode_name(std_mode), clr(NY_CLR_RESET));
  }

  char history_path[PATH_MAX] = {0};
  const char *home = getenv("HOME");
  if (home) {
    snprintf(history_path, sizeof(history_path), "%s/.nytrix_history", home);
    read_history(history_path);
    stifle_history(1000);
  }

  int tty_in = isatty(STDIN_FILENO);
  int use_readline = tty_in;
  const char *rl_env = getenv("NYTRIX_REPL_RL");
  if (rl_env && (*rl_env == '0' || strcasecmp(rl_env, "false") == 0))
    use_readline = 0;

  if (!use_readline && !tty_in && !init_code) {
    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
      exit(1);
    }
    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
      if (len + 1 >= cap) {
        cap *= 2;
        buf = realloc(buf, cap);
      }
      buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    init_lines = repl_split_lines(buf, &init_lines_len);
    free(buf);
    init_code = "<stdin>";
  }

  if (use_readline) {
    rl_variable_bind("enable-bracketed-paste", "on");
    rl_attempted_completion_function = repl_enhanced_completion;
    rl_completer_quote_characters = "\"";
    rl_filename_quote_characters = "\"";
    rl_basic_word_break_characters = " \t\n\"\\'`@$><=;|&{}.(";
    rl_bind_keyseq("\e[A", repl_move_up);
    rl_bind_keyseq("\e[B", repl_move_down);
    rl_bind_key(0x10, repl_move_up);        // Ctrl-P
    rl_bind_key(0x0e, repl_move_down);      // Ctrl-N
    rl_bind_keyseq("\eOA", repl_move_up);   // Secondary Up
    rl_bind_keyseq("\eOB", repl_move_down); // Secondary Down
    rl_bind_keyseq("\x12", rl_reverse_search_history);
    rl_redisplay_function = repl_redisplay;
    rl_completion_display_matches_hook = repl_display_match_list;
  }

  char *input_buffer = NULL;
  int last_status = 0;

  while (1) {
    repl_reset_redisplay();
    char prompt_buf[128];
    const char *prompt;
    if (input_buffer) {
      snprintf(prompt_buf, sizeof(prompt_buf), "\001%s\002..|\001%s\002",
               clr(NY_CLR_YELLOW), clr(NY_CLR_RESET));
      prompt = prompt_buf;
      repl_indent_next = repl_calc_indent(input_buffer);
      rl_pre_input_hook = repl_pre_input_hook;
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
        base = last_status ? "\001\033[31m\002ny!\001\033[0m\002"
                           : "\001\033[36m\002ny\001\033[0m\002";
      } else {
        base = last_status ? "ny!" : "ny";
      }

      if (mode_tag[0]) {
        if (color_mode) {
          snprintf(prompt_buf, sizeof(prompt_buf),
                   "%s\001\033[90m\002%s\001\033[0m\002> ", base, mode_tag);
        } else {
          snprintf(prompt_buf, sizeof(prompt_buf), "%s%s> ", base, mode_tag);
        }
      } else {
        snprintf(prompt_buf, sizeof(prompt_buf), "%s> ", base);
      }
      prompt = prompt_buf;
      rl_pre_input_hook = NULL;
    }

    char *line = NULL;
    int from_init = 0;
    if (init_line_idx < init_lines_len) {
      line = ny_strdup(init_lines[init_line_idx++]);
      from_init = 1;
    } else if (batch_mode) {
      break;
    } else if (use_readline) {
      line = readline(prompt);
      if (line && tty_in)
        printf("\n");
    } else {
      size_t cap = 0;
      if (tty_in) {
        fputs(prompt, stdout);
        fflush(stdout);
      }
      if (getline(&line, &cap, stdin) == -1) {
        free(line);
        break;
      }
      size_t nl = strlen(line);
      if (nl > 0 && line[nl - 1] == '\n')
        line[nl - 1] = '\0';
    }

    if (!line)
      break;
    if (input_buffer && !from_init && strlen(line) == 0) {
      free(line);
      if (!is_input_complete(input_buffer))
        continue;
      goto process_input;
    }
    if (!from_init && strlen(line) == 0 && !input_buffer) {
      free(line);
      continue;
    }

    if (input_buffer) {
      size_t len = strlen(input_buffer) + strlen(line) + 2;
      input_buffer = realloc(input_buffer, len);
      strcat(input_buffer, "\n");
      strcat(input_buffer, line);
      free(line);
    } else {
      input_buffer = line;
    }

    if (!is_input_complete(input_buffer))
      continue;

  process_input:;
    char *full_input = input_buffer;
    input_buffer = NULL;
    if (!full_input || !*full_input) {
      if (full_input)
        free(full_input);
      continue;
    }
    if (!from_init)
      add_history(full_input);

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
        if (chdir(*p ? p : getenv("HOME")) != 0)
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
        HISTORY_STATE *st = history_get_history_state();
        for (int i = 0; i < st->length; i++)
          if (st->entries[i])
            printf("%d: %s\n", i + history_base, st->entries[i]->line);
        free(st);
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "save")) {
        if (!*p)
          printf("Usage: :save <filename>\n");
        else if (write_history(p) != 0)
          perror("save");
        else
          printf("History saved to %s\n", p);
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "load")) {
        if (!*p)
          printf("Usage: :load <filename>\n");
        else {
          char *src = repl_read_file(p);
          if (src) {
            repl_append_user_source(src);
            printf("Loaded %s (%zu bytes)\n", p, strlen(src));
            parser_t ps;
            parser_init(&ps, src, p);
            program_t pr = parse_program(&ps);
            if (!ps.had_error)
              doclist_add_from_prog(&docs, &pr);
            program_free(&pr, ps.arena);
            free(src);
          } else
            perror("load");
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
      if (!strcmp(cn, "env")) {
        extern char **environ;
        for (char **env = environ; *env; env++)
          printf("%s\n", *env);
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "help")) {
        if (*p) {
          int found = 0;
          int printed = doclist_print(&docs, p);
          if (!printed) {
            if (!strcmp(p, "std")) {
              printf("\n%sStandard Library Packages:%s\n",
                     clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET));
              for (size_t i = 0; i < ny_std_package_count(); ++i)
                printf("  %-15s %s(Package)%s\n", ny_std_package_name(i),
                       clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
              printf("\n%sStandard Library Top-level Modules:%s\n",
                     clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET));
              for (size_t i = 0; i < ny_std_module_count(); ++i) {
                const char *m = ny_std_module_name(i);
                if (!strchr(m, '.'))
                  printf("  %-15s %s(Module)%s\n", m, clr(NY_CLR_GRAY),
                         clr(NY_CLR_RESET));
              }
              found = 1;
            } else if (ny_std_find_module_by_name(p) >= 0) {
              repl_load_module_docs(&docs, p);
              printed = doclist_print(&docs, p);
            }
          }
          if (!found && ny_std_find_module_by_name(p) >= 0) {
            printf("\n%s'%s' contains:%s\n", clr(NY_CLR_BOLD), p,
                   clr(NY_CLR_RESET));
            for (size_t i = 0; i < docs.len; ++i) {
              const char *name = docs.data[i].name;
              if (!strncmp(name, p, strlen(p)) && name[strlen(p)] == '.') {
                const char *sub = name + strlen(p) + 1;
                if (!strchr(sub, '.'))
                  printf("  %-25s %s%-10s%s\n", name, clr(NY_CLR_GRAY),
                         (docs.data[i].kind == 3 ? "Function" : "Symbol"),
                         clr(NY_CLR_RESET));
              }
            }
            found = 1;
          }
          if (!found && !printed) {
            for (size_t i = 0; i < ny_std_package_count(); ++i) {
              const char *pkg = ny_std_package_name(i);
              if (!strcmp(pkg, p)) {
                printf("\n%sPackage '%s' Modules:%s\n", clr(NY_CLR_BOLD), pkg,
                       clr(NY_CLR_RESET));
                for (size_t j = 0; j < ny_std_module_count(); ++j) {
                  const char *m = ny_std_module_name(j);
                  if (!strncmp(m, pkg, strlen(pkg)) &&
                      (m[strlen(pkg)] == '.' || !m[strlen(pkg)])) {
                    const char *sub = m + strlen(pkg) + 1;
                    if (m[strlen(pkg)] == '\0' || !strchr(sub, '.'))
                      printf("  %-15s %s(Module)%s\n", m, clr(NY_CLR_GRAY),
                             clr(NY_CLR_RESET));
                  }
                }
                found = 1;
                break;
              }
            }
          }
          if (!found && !printed)
            printf("%sNo documentation found for '%s'%s\n", clr(NY_CLR_RED), p,
                   clr(NY_CLR_RESET));
        } else {
          printf("%sCommands:%s\n", clr(NY_CLR_BOLD NY_CLR_CYAN),
                 clr(NY_CLR_RESET));
          printf("  %-15s Exit the REPL\n", ":exit/:quit/:q");
          printf("  %-15s Clear the screen\n", ":clear/:cls");
          printf("  %-15s Reset the REPL state\n", ":reset");
          printf("  %-15s Toggle execution timing\n", ":time");
          printf("  %-15s Show persistent source (defs/vars)\n", ":vars");
          printf("  %-15s Show environment variables\n", ":env");
          printf("  %-15s Show command history\n", ":history/:hist");
          printf("  %-15s Print working directory\n", ":pwd");
          printf("  %-15s List files in current directory\n", ":ls");
          printf("  %-15s Change current directory\n", ":cd [path]");
          printf("  %-15s Load a file\n", ":load [file]");
          printf("  %-15s Save session/source to file\n", ":save [file]");
          printf("  %-15s Show standard library info\n", ":std");
          printf("  %-15s Help for [name] (e.g. :help std.io)\n",
                 ":help/:h/:doc [name]");
          printf("\n%sNavigation Examples:%s\n", clr(NY_CLR_BOLD),
                 clr(NY_CLR_RESET));
          printf("  :help std           - List all packages\n");
          printf("  :help std.io        - List members of std.io\n");
          printf("  :help std.io.print  - Show logic of print function\n");
        }
        free(full_input);
        continue;
      }

      printf("Unknown command: :%s\n", cn);
      last_status = 1;
      free(full_input);
      continue;
    }

    // Evaluation Logic
    if (trimmed[0] == ';' || trimmed[0] == '\0') {
      free(full_input);
      continue;
    }

    int is_stmt = is_repl_stmt(full_input);
    if (!strncmp(trimmed, "use ", 4)) {
      char *mod_name = ltrim(trimmed + 4);
      char *end = mod_name;
      while (*end && !isspace((unsigned char)*end) && *end != ';' &&
             *end != '(')
        end++;
      char *name = ny_strndup(mod_name, (size_t)(end - mod_name));
      repl_ensure_module(name, std_mode, &docs);
      free(name);
    }
    char *an = repl_assignment_target(full_input);

    last_status =
        repl_eval_snippet(full_input, is_stmt, an, std_mode, tty_in, &docs, 0);

    if (an)
      free(an);
    free(full_input);
  }

  if (history_path[0])
    write_history(history_path);
  doclist_free(&docs);

  if (init_lines) {
    for (size_t i = 0; i < init_lines_len; i++) {
      free(init_lines[i]);
    }
    free(init_lines);
  }
  repl_shutdown_engine();
}
