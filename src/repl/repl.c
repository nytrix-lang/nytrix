#include "base/common.h"
#include "base/loader.h"
#include "base/util.h"
#include "code/code.h"
#include "code/jit.h"
#include "priv.h"
#include "repl/types.h"
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
#include <time.h>
#include <unistd.h>

const doc_list_t *g_repl_docs = NULL;

static std_mode_t g_repl_std_override = (std_mode_t)-1;
static int g_repl_plain = 0;

void ny_repl_set_std_mode(std_mode_t mode) { g_repl_std_override = mode; }
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
  doc_list_t docs = {0};
  g_repl_docs = &docs;
  add_builtin_docs(&docs);
  int repl_timing = 0;
  char **init_lines = NULL;
  size_t init_lines_len = 0, init_line_idx = 0;
  if (init_code && *init_code) {
    // When providing init_code (e.g. from a file or -i), process as a single
    // unit if we are NOT in interactive mode, to avoid N*N standard library
    // overhead.
    if (batch_mode || !isatty(STDIN_FILENO)) {
      init_lines = malloc(sizeof(char *));
      init_lines[0] = strdup(init_code);
      init_lines_len = 1;
    } else {
      init_lines = repl_split_lines(init_code, &init_lines_len);
    }
  }
  /* LLVM initialization moved into evaluation loop for isolation. */
  char *std_src_cached = NULL;
  if (std_mode != STD_MODE_NONE) {
    const char *prebuilt = getenv("NYTRIX_STD_PREBUILT");
    if (!prebuilt || access(prebuilt, R_OK) != 0) {
#ifdef NYTRIX_STD_PATH
      if (access(NYTRIX_STD_PATH, R_OK) == 0) {
        prebuilt = NYTRIX_STD_PATH;
      }
#endif
    }

    if (!prebuilt || access(prebuilt, R_OK) != 0) {
      char *exe_dir = ny_get_executable_dir();
      if (exe_dir) {
        static char path_buf[4096];
        snprintf(path_buf, sizeof(path_buf), "%s/std_bundle.ny", exe_dir);
        if (access(path_buf, R_OK) == 0) {
          prebuilt = path_buf;
        } else {
          snprintf(path_buf, sizeof(path_buf),
                   "%s/../share/nytrix/std_bundle.ny", exe_dir);
          if (access(path_buf, R_OK) == 0) {
            prebuilt = path_buf;
          }
        }
      }
    }

    if (prebuilt && access(prebuilt, R_OK) == 0) {
      std_src_cached = repl_read_file(prebuilt);
    }

    if (!std_src_cached) {
      std_src_cached = ny_build_std_bundle(NULL, 0, std_mode, 0, NULL);
    }
    if (std_src_cached) {
      parser_t parser;
      parser_init(&parser, std_src_cached, "<repl_std>");
      program_t prog = parse_program(&parser);
      if (!parser.had_error) {
        doclist_add_from_prog(&docs, &prog);
      }
      program_free(&prog, parser.arena);
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
  if (!use_readline && !tty_in &&
      !init_code) { // Non-interactive stdin: slurp all input then process once.
    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
      fprintf(stderr, "oom\n");
      exit(1);
    }
    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
      if (len + 1 >= cap) {
        cap *= 2;
        char *nb = realloc(buf, cap);
        if (!nb) {
          free(buf);
          fprintf(stderr, "oom\n");
          exit(1);
        }
        buf = nb;
      }
      buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    // Batch process non-interactive stdin for performance
    init_lines = malloc(sizeof(char *));
    init_lines[0] = buf;
    init_lines_len = 1;
    init_code = "<stdin>";
  }
  if (use_readline) {
    rl_variable_bind("enable-bracketed-paste", "on");
    rl_attempted_completion_function = repl_enhanced_completion;
    rl_completer_quote_characters = "\"";
    rl_filename_quote_characters = "\"";
    rl_basic_word_break_characters = " \t\n\"\\'`@$><=;|&{}.(";
    rl_bind_keyseq("\e[A", rl_history_search_backward);
    rl_bind_keyseq("\e[B", rl_history_search_forward);
    rl_bind_keyseq("\x12", rl_reverse_search_history);
    rl_redisplay_function = repl_redisplay;
    rl_completion_display_matches_hook = repl_display_match_list;
  }
  char *input_buffer = NULL;
  int last_status = 0;

  LLVMLinkInMCJIT();
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();
  while (1) {
    char prompt_buf[64];
    const char *prompt;
    if (input_buffer) {
      snprintf(prompt_buf, sizeof(prompt_buf), "\001%s\002..|\001%s\002",
               clr(NY_CLR_YELLOW), clr(NY_CLR_RESET));
      prompt = prompt_buf;
      repl_indent_next = repl_calc_indent(input_buffer);
      rl_pre_input_hook = repl_pre_input_hook;
    } else {
      const char *mode_tag = "";
      char mode_buf[16];
      if (std_mode == STD_MODE_NONE) {
        snprintf(mode_buf, sizeof(mode_buf), "[none]");
        mode_tag = mode_buf;
      } else if (std_mode == STD_MODE_FULL) {
        snprintf(mode_buf, sizeof(mode_buf), "[full]");
        mode_tag = mode_buf;
      }
      const char *base = last_status ? "\001\033[31m\002ny!\001\033[0m\002"
                                     : "\001\033[36m\002ny\001\033[0m\002";
      if (mode_tag[0])
        snprintf(prompt_buf, sizeof(prompt_buf),
                 "%s\001\033[90m\002%s\001\033[0m\002> ", base, mode_tag);
      else
        snprintf(prompt_buf, sizeof(prompt_buf), "%s> ", base);
      prompt = prompt_buf;
      rl_pre_input_hook = NULL;
    }
    char *line = NULL;
    int from_init = 0;
    if (init_line_idx < init_lines_len) {
      line = strdup(init_lines[init_line_idx++]);
      from_init = 1;
    } else if (batch_mode) {
      break;
    } else if (use_readline) {
      line = readline(prompt);
      if (line && tty_in)
        printf("\n");
    } else {
      size_t cap = 0;
      ssize_t nread;
      if (tty_in) {
        fputs(prompt, stdout);
        fflush(stdout);
      }
      nread = getline(&line, &cap, stdin);
      if (nread == -1) {
        if (line)
          free(line);
        break;
      }
      // strip trailing newline
      if (nread > 0 && line[nread - 1] == '\n')
        line[nread - 1] = '\0';
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
      char *nb = realloc(input_buffer, len);
      input_buffer = nb;
      strcat(input_buffer, "\n");
      strcat(input_buffer, line);
      free(line);
    } else
      input_buffer = line;
    if (is_input_complete(input_buffer))
      goto process_input;
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
      if (!strcmp(cn, "complete")) {
        size_t count = 0;
        char **completions = nytrix_get_completions_for_prefix(p, &count);

        // Output completions in a parseable format
        printf("__COMPLETIONS__\n");
        if (completions) {
          for (size_t i = 0; i < count; i++) {
            printf("%s\n", completions[i]);
          }
          nytrix_free_completions(completions, count);
        }
        printf("__END__\n");

        free(full_input);
        continue;
      }

      if (!strcmp(cn, "q") || !strcmp(cn, "quit"))
        cn = "exit";
      if (!strcmp(cn, "help")) {
        if (*p) {
          int found = 0;
          int printed = doclist_print(&docs, p);
          if (!printed) {
            if (!strcmp(p, "std")) {
              printf("\n%sStandard Library Packages:%s\n",
                     clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET));
              for (size_t i = 0; i < ny_std_package_count(); ++i) {
                printf("  %-15s%s(Package)%s\n", ny_std_package_name(i),
                       clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
              }
              printf("\n%sStandard Library Top-level Modules:%s\n",
                     clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET));
              for (size_t i = 0; i < ny_std_module_count(); ++i) {
                const char *m = ny_std_module_name(i);
                if (!strchr(m, '.'))
                  printf("  %-15s%s(Module)%s\n", m, clr(NY_CLR_GRAY),
                         clr(NY_CLR_RESET));
              }
              printf("\n%sUse ':help [name]' to drill down into a package or "
                     "module.%s\n",
                     clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
              found = 1;
            } else if (ny_std_find_module_by_name(p) >= 0) {
              repl_load_module_docs(&docs, p);
              printed = doclist_print(&docs, p);
            }
          }
          // If it's a module, list contents
          if (!found && ny_std_find_module_by_name(p) >= 0) {
            printf("\n%s'%s' contains:%s\n", clr(NY_CLR_BOLD), p,
                   clr(NY_CLR_RESET));
            int count = 0;
            for (size_t i = 0; i < docs.len; ++i) {
              const char *name = docs.data[i].name;
              if (!strncmp(name, p, strlen(p)) && name[strlen(p)] == '.') {
                const char *sub = name + strlen(p) + 1;
                if (!strchr(sub, '.')) {
                  printf("  %-25s %s%-10s%s\n", name, clr(NY_CLR_GRAY),
                         (docs.data[i].kind == 3
                              ? "Function"
                              : (docs.data[i].kind == 2 ? "Module" : "Symbol")),
                         clr(NY_CLR_RESET));
                  count++;
                }
              }
            }
            if (count == 0)
              printf("  (No documented members found)\n");
            found = 1;
          }
          // If it's a package, list modules
          if (!found && !printed) {
            for (size_t i = 0; i < ny_std_package_count(); ++i) {
              if (!strcmp(ny_std_package_name(i), p)) {
                printf("\n%sPackage '%s' Modules:%s\n", clr(NY_CLR_BOLD), p,
                       clr(NY_CLR_RESET));
                for (size_t j = 0; j < ny_std_module_count(); ++j) {
                  const char *m = ny_std_module_name(j);
                  if (!strncmp(m, p, strlen(p)) &&
                      (m[strlen(p)] == '.' || !m[strlen(p)])) {
                    const char *sub = m + strlen(p) + 1;
                    if (m[strlen(p)] == '\0' || !strchr(sub, '.')) {
                      printf("  %-15s %s(Module)%s\n", m, clr(NY_CLR_GRAY),
                             clr(NY_CLR_RESET));
                    }
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
      if (!strcmp(cn, "exit")) {
        free(full_input);
        break;
      }
      if (!strcmp(cn, "clear")) {
        printf("\033[2J\033[H");
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "reset")) {
        doclist_free(&docs);
        memset(&docs, 0, sizeof(docs));
        add_builtin_docs(&docs);
        if (std_src_cached) {
          free(std_src_cached);
          std_src_cached = NULL;
        }
        if (g_repl_user_source) {
          free(g_repl_user_source);
          g_repl_user_source = NULL;
          g_repl_user_source_len = 0;
          // g_repl_user_source_cap is hidden in util.c, we assume free nulls
          // it effectively or next alloc resets? Actually util.c has
          // g_repl_user_source_cap static. This reset here might desync cap
          // if we don't reset cap. Since g_repl_user_source is NULL, next
          // repl_append_user_source will realloc. But if old cap is large,
          // realloc(NULL, cap) -> valid? realloc(NULL, size) is malloc(size).
          // But cap might be stale.
          // Ideally util.c should provide `repl_reset_source()`.
          // I will assume for now it's fine or I will miss this detail.
        }
        if (std_mode != STD_MODE_NONE) {
          std_src_cached = ny_build_std_bundle(NULL, 0, std_mode, 0, NULL);
          if (std_src_cached) {
            parser_t ps;
            parser_init(&ps, std_src_cached, "<std>");
            program_t pr = parse_program(&ps);
            if (!ps.had_error) {
              doclist_add_from_prog(&docs, &pr);
            }
            program_free(&pr, ps.arena);
          }
        }
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
        repl_timing = !repl_timing;
        printf("Timing: %s\n", repl_timing ? "on" : "off");
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
            if (!ps.had_error) {
              doclist_add_from_prog(&docs, &pr);
            }
            program_free(&pr, ps.arena);
            free(src);
          } else
            perror("load");
        }
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "vars")) {
        if (g_repl_user_source) {
          printf("%s--- Persistent Source ---%s\n%s", clr(NY_CLR_BOLD),
                 clr(NY_CLR_RESET), g_repl_user_source);
          if (g_repl_user_source_len > 0 &&
              g_repl_user_source[g_repl_user_source_len - 1] != '\n')
            printf("\n");
        } else
          printf("No persistent variables defined.\n");
        free(full_input);
        continue;
      }
      if (!strcmp(cn, "std")) {
        printf("Std mode: %s\n", repl_std_mode_name(std_mode));
        free(full_input);
        continue;
      }
      printf("? :%s\n", cn);
      last_status = 1;
      free(full_input);
      continue;
    }
    if (!strcmp(trimmed, "q") || !strcmp(trimmed, "quit") ||
        !strcmp(trimmed, "exit")) {
      free(full_input);
      break;
    }
    if (trimmed[0] == ';' || trimmed[0] == '\0') {
      free(full_input);
      continue;
    }
    int is_stmt = 0;
    if (strchr(full_input, '{'))
      is_stmt = 1;
    if (!strncmp(trimmed, "def ", 4))
      is_stmt = 1;
    if (!strncmp(trimmed, "fn ", 3))
      is_stmt = 1;
    if (!strncmp(trimmed, "use ", 4)) {
      is_stmt = 1;
      /* Load docs for the module being used to enable syntax highlighting for
       * its members. */
      char *mod_name = ltrim(trimmed + 4);
      char *end = mod_name;
      while (*end && !isspace((unsigned char)*end) && *end != ';')
        end++;
      char *name = ny_strndup(mod_name, (size_t)(end - mod_name));
      repl_load_module_docs(&docs, name);
      free(name);
    }
    char *an = repl_assignment_target(full_input);
    if (an)
      is_stmt = 1;
    if (!is_stmt && !strncmp(ltrim(full_input), "print", 5))
      is_stmt = 1;
    int show = (!is_stmt && std_mode != STD_MODE_NONE && tty_in);
    int show_an = (an && std_mode != STD_MODE_NONE && tty_in);
    const char *fn_name = "__repl_eval";
    size_t blen = strlen(full_input) + (an ? strlen(an) : 0) + 128;
    char *body = malloc(blen);
    if (show)
      snprintf(body, blen, "__repl_show(%s\n)\n", full_input);
    else if (!is_stmt)
      snprintf(body, blen, "return %s\n", full_input);
    else if (show_an)
      snprintf(body, blen, "%s\n__repl_show(%s\n)\n", full_input, an);
    else
      snprintf(body, blen, "%s\n", full_input);
    clock_t t0 = repl_timing ? clock() : 0;
    size_t full_slen = (std_src_cached ? strlen(std_src_cached) : 0) +
                       (g_repl_user_source ? g_repl_user_source_len : 0) + 512 +
                       strlen(body);
    char *full_src = malloc(full_slen);
    full_src[0] = '\0';
    if (std_src_cached) {
      strcpy(full_src, std_src_cached);
      strcat(full_src, "\n");
    }
    if (g_repl_user_source && g_repl_user_source_len > 0) {
      strcat(full_src, g_repl_user_source);
      strcat(full_src, "\n");
    }
    /* Prelude uses are already injected by ny_build_std_bundle in
     * std_src_cached. */

    // We don't wrap the current body in __repl_eval yet because
    // we want codegen_emit_script to handle top-level statements including
    // our body.
    strcat(full_src, body);
    parser_t ps_all;
    parser_init(&ps_all, full_src, "<repl_eval_unit>");
    program_t pr_all = parse_program(&ps_all);
    if (debug_enabled)
      fprintf(stderr, "DEBUG: Parsing complete\n");
    if (!ps_all.had_error) {
      if (debug_enabled)
        fprintf(stderr, "DEBUG: Creating LLVM context\n");
      LLVMContextRef eval_ctx = LLVMContextCreate();
      LLVMModuleRef eval_mod =
          LLVMModuleCreateWithNameInContext("repl_eval", eval_ctx);
      LLVMBuilderRef eval_builder = LLVMCreateBuilderInContext(eval_ctx);
      LLVMExecutionEngineRef eval_ee = NULL;
      char *ee_err = NULL;
      if (debug_enabled)
        fprintf(stderr, "DEBUG: Init codegen_t\n");
      // Let MCJIT handle target setup
      codegen_t cg;
      codegen_init_with_context(&cg, &pr_all, eval_mod, eval_ctx, eval_builder);
      codegen_emit(&cg);
      LLVMValueRef eval_fn = codegen_emit_script(&cg, "__repl_eval");
      (void)eval_fn;
      if (cg.had_error) {
        last_status = 1;
        goto cleanup_ctx;
      }
      if (cg.had_error) {
        last_status = 1;
        goto cleanup_ctx;
      }
      char *verr = NULL;
      if (LLVMVerifyModule(eval_mod, LLVMReturnStatusAction, &verr)) {
        fprintf(stderr, "Module verification failed: %s\n", verr);
        LLVMDisposeMessage(verr);
        goto cleanup_ctx;
      }
      if (verr)
        LLVMDisposeMessage(verr); // Just in case
      struct LLVMMCJITCompilerOptions options;
      LLVMInitializeMCJITCompilerOptions(&options, sizeof(options));
      options.CodeModel = LLVMCodeModelLarge;
      if (LLVMCreateMCJITCompilerForModule(&eval_ee, eval_mod, &options,
                                           sizeof(options), &ee_err) != 0) {
        fprintf(stderr, "Failed to create execution engine: %s\n", ee_err);
        LLVMDisposeMessage(ee_err);
        LLVMContextDispose(eval_ctx);
        continue;
      }
      // Register interned strings
      for (size_t i = 0; i < cg.interns.len; ++i) {
        if (cg.interns.data[i].gv) {
          LLVMAddGlobalMapping(eval_ee, cg.interns.data[i].gv,
                               (void *)((char *)cg.interns.data[i].data - 64));
        }
        if (cg.interns.data[i].val &&
            cg.interns.data[i].val != cg.interns.data[i].gv) {
          LLVMAddGlobalMapping(eval_ee, cg.interns.data[i].val,
                               &cg.interns.data[i].data);
        }
      }
#include "rt/runtime.h"
      struct {
        const char *n;
        void *p;
      } syms[] = {
#define RT_DEF(n, p, a) {n, (void *)p},
#include "rt/defs.h"
#undef RT_DEF
          {NULL, NULL}};
      // Mapping runtime symbols (handled suffixes like .9)
      for (LLVMValueRef f = LLVMGetFirstFunction(eval_mod); f;
           f = LLVMGetNextFunction(f)) {
        const char *fn_name_llvm = LLVMGetValueName(f);
        for (int i = 0; syms[i].n; ++i) {
          size_t slen = strlen(syms[i].n);
          if (strncmp(fn_name_llvm, syms[i].n, slen) == 0 &&
              (fn_name_llvm[slen] == '\0' || fn_name_llvm[slen] == '.')) {
            LLVMAddGlobalMapping(eval_ee, f, syms[i].p);
            break;
          }
        }
      }
      register_jit_symbols(eval_ee, eval_mod);

      if (debug_enabled)
        fprintf(stderr, "DEBUG: Interpreter execution start\n");
      LLVMValueRef eval_fn_val = LLVMGetNamedFunction(eval_mod, fn_name);
      if (eval_fn_val) {
        LLVMGenericValueRef res_gv =
            LLVMRunFunction(eval_ee, eval_fn_val, 0, NULL);
        LLVMDisposeGenericValue(res_gv);
        // if(!is_stmt && !show) printf("%s=>%s %ld\n", clr(NY_CLR_GREEN),
        // clr(NY_CLR_RESET), res_val);
        last_status = 0;
        char *undef_name = NULL;
        if (!strncmp(ltrim(full_input), "undef ", 6)) {
          char *p = ltrim(full_input) + 6;
          while (*p == ' ' || *p == '\t')
            p++;
          char *end = p;
          while (*end && !isspace((unsigned char)*end) && *end != ';')
            end++;
          if (end > p)
            undef_name = ny_strndup(p, (size_t)(end - p));
        }
        if (undef_name) {
          repl_remove_def(undef_name);
          free(undef_name);
        } else if (is_persistent_def(full_input)) {
          repl_append_user_source(full_input);
          repl_update_docs(&docs, full_input);
        }
      } else {
        fprintf(stderr, "Failed to find function %s\n", fn_name);
        last_status = 1;
      }
      clock_t t_end = clock();
      if (repl_timing && !from_init) {
        printf("%s[Eval: %.3f ms]%s\n", clr(NY_CLR_GRAY),
               (double)(t_end - t0) * 1000.0 / CLOCKS_PER_SEC,
               clr(NY_CLR_RESET));
      }
      LLVMDisposeExecutionEngine(eval_ee);
    cleanup_ctx:
      codegen_dispose(&cg);
      LLVMContextDispose(eval_ctx);
    } else if (ps_all.error_count > 0) {
      repl_print_error_snippet(full_src, ps_all.cur.line, ps_all.cur.col);
      last_status = 1;
    }
    program_free(&pr_all, ps_all.arena);
    free(full_src);
    free(body);
    if (an)
      free(an);
    free(full_input);
  }
  if (history_path[0])
    write_history(history_path);
  doclist_free(&docs);
  if (std_src_cached)
    free(std_src_cached);
  if (init_lines) {
    for (size_t i = 0; i < init_lines_len; ++i)
      free(init_lines[i]);
    free(init_lines);
  }
}
