#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/common.h"
#include "base/loader.h"
#include "base/util.h"
#include "code/code.h"
#include "code/jit.h"
#include "code/llvm.h"
#include "priv.h"
#include "repl/types.h"
#include "rt/runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "repl/read.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

const doc_list_t *g_repl_docs = NULL;
static VEC(char *) g_repl_loading_modules = {0};

static std_mode_t g_repl_std_override = STD_MODE_DEFAULT;
static int g_repl_has_std_override = 0;
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
volatile sig_atomic_t g_repl_sigint = 0;

#ifndef _WIN32
static volatile sig_atomic_t g_repl_eval_active = 0;
static sigjmp_buf g_repl_eval_jmp;
#endif

static void repl_on_sigint(int sig) {
  (void)sig;
  g_repl_sigint = 1;
#ifndef _WIN32
  if (g_repl_eval_active) {
    g_repl_eval_active = 0;
    siglongjmp(g_repl_eval_jmp, 1);
  }
#endif

#ifndef _WIN32
  const char nl = '\n';
  (void)write(STDOUT_FILENO, &nl, 1);
#endif
}

static void repl_ensure_module(const char *name, std_mode_t std_mode,
                               doc_list_t *docs);

void ny_repl_set_std_mode(std_mode_t mode) {
  g_repl_std_override = mode;
  g_repl_has_std_override = 1;
}
void ny_repl_set_plain(int plain) { g_repl_plain = plain; }

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

static void repl_restore_terminal_state(void) {
  /*
   * TUI snippets can leave the terminal in raw/hidden/no-wrap/alt-buffer state
   * on errors or manual interruption. Force a sane interactive REPL baseline.
   */
  (void)__tty_raw(0);
  fputs("\033[0m\033[?25h\033[?7h\033[?1049l", stdout);
  fflush(stdout);
}

static void map_rt_syms_persistent(LLVMModuleRef mod,
                                   LLVMExecutionEngineRef ee) {
#define RT_DEF(name, p, args, sig, doc) {name, (void *)p}, {#p, (void *)p},
#define RT_GV(name, p, t, doc) {name, (void *)&p},
#ifdef _WIN32
#ifdef __argc
#undef __argc
#endif
#ifdef __argv
#undef __argv
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
  ny_llvm_prepare_module(mod);
  g_repl_builder = LLVMCreateBuilderInContext(g_repl_ctx);
  const char *std_init_fn_name = NULL;

  if (mode != STD_MODE_NONE) {
    const char *prebuilt = getenv("NYTRIX_STD_PREBUILT");
    if ((!prebuilt || access(prebuilt, R_OK) != 0)) {
      const char *build_std = getenv("NYTRIX_BUILD_STD_PATH");
      if (build_std && access(build_std, R_OK) == 0)
        prebuilt = build_std;
    }
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
  LLVMInitializeMCJITCompilerOptions(&options, sizeof(options));
  // Favor fast interactive compile latency by default.
  options.CodeModel = LLVMCodeModelJITDefault;
  options.OptLevel = 0;
  options.EnableFastISel = 1;
  {
    const char *cm = getenv("NYTRIX_JIT_CODE_MODEL");
    if (cm && *cm) {
      if (strcmp(cm, "large") == 0)
        options.CodeModel = LLVMCodeModelLarge;
      else if (strcmp(cm, "medium") == 0)
        options.CodeModel = LLVMCodeModelMedium;
      else if (strcmp(cm, "small") == 0)
        options.CodeModel = LLVMCodeModelSmall;
    }
  }
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
    if (fast_isel && *fast_isel && !ny_env_is_truthy(fast_isel)) {
      options.EnableFastISel = 0;
    }
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
        ((int64_t (*)(void))init_addr)();
      }
    }
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
      // Shebang-only snippet: treat as no-op.
      return 0;
    }
  }

  int show = (!is_stmt && std_mode != STD_MODE_NONE && tty_in);
  int show_an = (an && std_mode != STD_MODE_NONE && tty_in);
  if (show_an) {
    /*
     * Avoid flooding REPL output for pasted blocks/TUI setup where assignment
     * targets can be large buffers (canvas/list/bytes/etc).
     */
    if (strchr(eval_input, '\n')) {
      show_an = 0;
    } else if (!strncmp(trimmed, "def ", 4) || !strncmp(trimmed, "mut ", 4) ||
               !strncmp(trimmed, "fn ", 3) || !strncmp(trimmed, "use ", 4) ||
               !strncmp(trimmed, "while", 5) || !strncmp(trimmed, "for", 3) ||
               !strncmp(trimmed, "if", 2)) {
      show_an = 0;
    }
  }
  const char *use_inspect = (std_mode != STD_MODE_NONE && (show || show_an))
                                ? "use std.util.inspect\n"
                                : "";

  size_t blen =
      strlen(eval_input) + (an ? strlen(an) : 0) + strlen(use_inspect) + 128;
  char *body = malloc(blen);
  if (show)
    snprintf(body, blen, "%srepl_show(%s\n)\n", use_inspect, eval_input);
  else if (!is_stmt)
    snprintf(body, blen, "%sreturn %s\n", use_inspect, eval_input);
  else if (show_an)
    snprintf(body, blen, "%s%s\nrepl_show(%s\n)\n", use_inspect, eval_input,
             an);
  else
    snprintf(body, blen, "%s%s\n", use_inspect, eval_input);

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
    ny_llvm_prepare_module(eval_mod);
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
    for (size_t i = 0; i < g_repl_cg.use_modules.len; i++)
      vec_push(&cg.use_modules, ny_strdup(g_repl_cg.use_modules.data[i]));
    for (size_t i = 0; i < g_repl_cg.user_use_modules.len; i++) {
      vec_push(&cg.user_use_modules,
               ny_strdup(g_repl_cg.user_use_modules.data[i]));
    }

    codegen_emit(&cg);
    char fn_name[64];
    snprintf(fn_name, sizeof(fn_name), "__eval_%d", g_eval_count++);
    LLVMValueRef eval_fn = codegen_emit_script(&cg, fn_name);
    (void)eval_fn;
    ny_llvm_apply_host_attrs(eval_mod);

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

      for (size_t i = g_repl_cg.global_vars.len; i < cg.global_vars.len; i++) {
        if (cg.global_vars.data[i].name && cg.global_vars.data[i].value) {
          LLVMGetGlobalValueAddress(g_repl_ee, cg.global_vars.data[i].name);
        }
      }

      uint64_t addr = LLVMGetFunctionAddress(g_repl_ee, fn_name);
      if (addr) {
        int interrupted = 0;
#ifndef _WIN32
        if (sigsetjmp(g_repl_eval_jmp, 1) == 0) {
          g_repl_eval_active = 1;
          ((void (*)(void))addr)();
          g_repl_eval_active = 0;
          last_status = 0;
        } else {
          interrupted = 1;
          g_repl_eval_active = 0;
          last_status = 1;
        }
#else
        ((void (*)(void))addr)();
        last_status = 0;
#endif

        char *undef_name = NULL;
        if (!interrupted && !strncmp(trimmed, "undef ", 6)) {
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
        } else if (!interrupted && is_persistent_def(eval_input)) {
          persistent = true;
          if (!from_init)
            repl_append_user_source(eval_input);
          repl_update_docs(docs, eval_input);

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
        }
      }

      // Move interns to persistent state so they aren't freed by
      // codegen_dispose.
      for (size_t i = 0; i < cg.interns.len; i++) {
        vec_push(&g_repl_cg.interns, cg.interns.data[i]);
      }
      cg.interns.len = 0;
    }
    /*
     * This eval context borrows persistent symbol/alias/module/enum entries
     * from g_repl_cg. Keep disposal from freeing shared names/pointers after
     * failed evals, which otherwise corrupts future REPL lookups.
     */
    cg.fun_sigs.len = 0;
    cg.global_vars.len = 0;
    cg.aliases.len = 0;
    cg.import_aliases.len = 0;
    cg.user_import_aliases.len = 0;
    cg.enums.len = 0;
    cg.use_modules.len = 0;
    cg.user_use_modules.len = 0;
    codegen_dispose(&cg);
  } else {
    repl_print_error_snippet(eval_input, ps.cur.line, ps.cur.col);
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
  if (eval_input_owned)
    free(eval_input_owned);
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

  if (name[0] == '.' || name[0] == '/' || name[0] == '\\' ||
      strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
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
  if (clean_name[0] == '.' || clean_name[0] == '/' || clean_name[0] == '\\' ||
      strchr(clean_name, '/') != NULL || strchr(clean_name, '\\') != NULL) {
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

  /*
   * Streaming stdin line-by-line is much faster for test-runner REPL mode.
   * The full-stdin slurp path remains opt-in for debugging.
   */
  if (!tty_in && !init_code) {
    const char *slurp = getenv("NYTRIX_REPL_SLURP_STDIN");
    if (slurp && slurp[0] != '0') {
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
      init_lines = malloc(sizeof(char *));
      if (!init_lines) {
        free(buf);
        exit(1);
      }
      init_lines[0] = buf;
      init_lines_len = 1;
      init_code = "<stdin>";
    }
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
    char prompt_buf[128];
    const char *prompt;
    if (input_buffer) {
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
          snprintf(prompt_buf, sizeof(prompt_buf), "%s\033[90m%s\033[0m> ",
                   base, mode_tag);
        } else {
          snprintf(prompt_buf, sizeof(prompt_buf), "%s%s> ", base, mode_tag);
        }
      } else {
        snprintf(prompt_buf, sizeof(prompt_buf), "%s> ", base);
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
        // ny_readline already prints the final newline
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
        if (len + 1 >= cap) {
          cap *= 2;
          buf = realloc(buf, cap);
        }
        buf[len++] = (char)ch;
      }
      if (len == 0 && ch == EOF) {
        free(buf);
        buf = NULL;
        break;
      }
      buf[len] = '\0';
      line = buf;
    }

    if (!line) {
      if (g_repl_sigint) {
        if (!input_buffer) {
          repl_restore_terminal_state();
          break;
        }
        last_status = 1;
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
        last_status = 1;
        printf("Canceled multiline input\n");
        repl_restore_terminal_state();
        continue;
      }
      repl_restore_terminal_state();
      break;
    }
    // ny_readline already fully manages its own multiline buffer internally,
    // so when it returns `line`, it is the *complete* block of code!
    // We don't need to manually read line-by-line or concatenate `input_buffer`
    // here for TTY input.
    if (tty_in && !from_init) {
      if (input_buffer)
        free(input_buffer);
      input_buffer = line;
      goto process_input;
    }

    if (input_buffer && !from_init) {
      char *line_trimmed = ltrim(line);
      rtrim_inplace(line_trimmed);
      if (!strcmp(line_trimmed, ":cancel") || !strcmp(line_trimmed, ":c")) {
        free(line);
        free(input_buffer);
        input_buffer = NULL;
        last_status = 1;
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
      size_t len = strlen(input_buffer) + strlen(line) + 2;
      input_buffer = realloc(input_buffer, len);
      strcat(input_buffer, "\n");
      strcat(input_buffer, line);
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
        else if (ny_readline_write_history(p) != 0)
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
      if (!strcmp(cn, "run")) {
        if (!*p) {
          printf("Usage: :run <filename>\n");
        } else {
          char *src = repl_read_file(p);
          if (src) {
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
      if (!strcmp(cn, "env")) {
#ifdef _WIN32
        char **envp = _environ;
#else
        extern char **environ;
        char **envp = environ;
#endif
        for (char **env = envp; env && *env; env++)
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
          printf("  %-15s Cancel current multiline input\n", ":cancel/:c");
          printf("  %-15s Toggle execution timing\n", ":time");
          printf("  %-15s Show persistent source (defs/vars)\n", ":vars");
          printf("  %-15s Show environment variables\n", ":env");
          printf("  %-15s Show command history\n", ":history/:hist");
          printf("  %-15s Print working directory\n", ":pwd");
          printf("  %-15s List files in current directory\n", ":ls");
          printf("  %-15s Change current directory\n", ":cd [path]");
          printf("  %-15s Load a file\n", ":load [file]");
          printf("  %-15s Evaluate and run a file\n", ":run [file]");
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
    repl_restore_terminal_state();

    if (an)
      free(an);
    free(full_input);
  }

  if (history_path[0])
    ny_readline_write_history(history_path);
  doclist_free(&docs);

  if (init_lines) {
    for (size_t i = 0; i < init_lines_len; i++) {
      free(init_lines[i]);
    }
    free(init_lines);
  }
  repl_shutdown_engine();
#ifdef _WIN32
  signal(SIGINT, prev_sigint);
#else
  sigaction(SIGINT, &prev_sigint, NULL);
#endif
}
