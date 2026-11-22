/*
 * Editor commands owned by the language server.
 *
 * Editors never hard-code how Nytrix tools are invoked. They ask for the
 * command list (`nytrix/commands`), show it however they like, and run a
 * command through `workspace/executeCommand`. The result is a portable
 * action the editor performs as-is:
 *
 *   {"kind":"process","title":..,"command":ABS,"args":[..],"cwd":..,"save":URI}
 *   {"kind":"debug","save":URI,"configuration":{"type":"nytrix",...}}
 *   {"kind":"message","text":..}
 *
 * Code lenses and formatting are served here too. Adding or changing a
 * command, its flags, or the tool it runs needs no editor change.
 *
 * Included by lsp/main.c after the JSON, buffer and document helpers.
 */

#ifdef _WIN32
#define LSP_EXE_SUFFIX ".exe"
#else
#define LSP_EXE_SUFFIX ""
#endif

enum {
  LSP_ON_NY = 1u << 0,     /* .ny documents */
  LSP_ON_NSHAPE = 1u << 1, /* .nshape fixtures */
  LSP_ON_WORKSPACE = 1u << 2,
};

typedef struct {
  const char *id;
  const char *title;
  const char *tool;          /* sibling executable, NULL for debug */
  const char *const *argv;   /* "{file}" and "{input}" are substituted */
  unsigned scope;
  const char *input_prompt;  /* asks the user for {input} when set */
  bool modifies_file;        /* the editor reloads the file afterwards */
} lsp_command_t;

#define LSP_ARGS(...)                                                          \
  (const char *const[]) { __VA_ARGS__, NULL }

static const lsp_command_t lsp_commands[] = {
    {"nytrix.run", "Run File", "ny", LSP_ARGS("{file}"), LSP_ON_NY, NULL,
     false},
    {"nytrix.debug", "Debug File", NULL, NULL, LSP_ON_NY | LSP_ON_NSHAPE,
     NULL, false},
    {"nytrix.check", "Check File", "ny",
     LSP_ARGS("--diag-compact", "-emit-only", "{file}"), LSP_ON_NY, NULL,
     false},
    {"nytrix.repl", "Start REPL", "ny", LSP_ARGS("-i"),
     LSP_ON_NY | LSP_ON_WORKSPACE, NULL, false},
    {"nytrix.trace", "Trace File", "ny", LSP_ARGS("-trace", "-run", "{file}"),
     LSP_ON_NY, NULL, false},
    {"nytrix.expand", "Expand Macros", "ny", LSP_ARGS("--expand", "{file}"),
     LSP_ON_NY, NULL, false},
    {"nytrix.dumpAst", "Show AST", "ny", LSP_ARGS("-dump-ast", "{file}"),
     LSP_ON_NY, NULL, false},
    {"nytrix.dumpLlvm", "Show LLVM IR", "ny", LSP_ARGS("-dump-llvm", "{file}"),
     LSP_ON_NY, NULL, false},
    {"nytrix.dumpStats", "Show Compile Statistics", "ny",
     LSP_ARGS("-dump-stats", "-time", "{file}"), LSP_ON_NY, NULL, false},
    {"nytrix.analyze", "Analyze File", "ny-fmt",
     LSP_ARGS("--analyze", "{file}"), LSP_ON_NY, NULL, false},
    {"nytrix.optimize", "Apply Source Optimizations", "ny-fmt",
     LSP_ARGS("--optimize", "--apply", "{file}"), LSP_ON_NY, NULL, true},
    {"nytrix.profile", "Profile File", "ny-perf",
     LSP_ARGS("profile", "{file}", "--"), LSP_ON_NY, NULL, false},
    {"nytrix.shape", "Run Shape Fixture", "ny-test", LSP_ARGS("{file}"),
     LSP_ON_NSHAPE, NULL, false},
    {"nytrix.tests", "Run Test Suite", "ny-test", LSP_ARGS("--with-stdlib"),
     LSP_ON_WORKSPACE, NULL, false},
    {"nytrix.docs", "Search Documentation", "ny-doc",
     LSP_ARGS("search", "{input}"), LSP_ON_NY | LSP_ON_NSHAPE |
     LSP_ON_WORKSPACE, "Search stdlib modules, symbols and docs", false},
};

static char *lsp_workspace_root;

static void lsp_commands_set_root(const char *initialize_body) {
  char *uri = json_extract_string(initialize_body, "rootUri");
  free(lsp_workspace_root);
  lsp_workspace_root = uri ? uri_to_path(uri) : NULL;
  free(uri);
}

static unsigned lsp_scope_for_path(const char *path) {
  if (!path)
    return LSP_ON_WORKSPACE;
  size_t n = strlen(path);
  if (n >= 7 && strcmp(path + n - 7, ".nshape") == 0)
    return LSP_ON_NSHAPE;
  return LSP_ON_NY;
}

/* Absolute path of a toolchain executable next to this server. */
static void lsp_tool_path(char *out, size_t cap, const char *tool) {
  const char *dir = ny_get_executable_dir(); /* static buffer */
  if (dir && *dir)
    snprintf(out, cap, "%s/%s%s", dir, tool, LSP_EXE_SUFFIX);
  else
    snprintf(out, cap, "%s%s", tool, LSP_EXE_SUFFIX);
}

static char *lsp_dirname(const char *path) {
  char *dir = strdup(path ? path : ".");
  if (!dir)
    return NULL;
  char *slash = strrchr(dir, '/');
#ifdef _WIN32
  char *bs = strrchr(dir, '\\');
  if (bs && (!slash || bs > slash))
    slash = bs;
#endif
  if (slash && slash != dir)
    *slash = '\0';
  else if (!slash)
    snprintf(dir, strlen(dir) + 1, ".");
  return dir;
}

static const lsp_command_t *lsp_find_command(const char *id) {
  for (size_t i = 0; id && i < sizeof(lsp_commands) / sizeof(lsp_commands[0]);
       ++i)
    if (strcmp(lsp_commands[i].id, id) == 0)
      return &lsp_commands[i];
  return NULL;
}

static void lsp_append_scope(sbuf_t *b, unsigned scope) {
  sb_append(b, "[");
  bool first = true;
  if (scope & LSP_ON_NY) {
    sb_append(b, "\"nytrix\"");
    first = false;
  }
  if (scope & LSP_ON_NSHAPE) {
    sb_append(b, first ? "\"nshape\"" : ",\"nshape\"");
    first = false;
  }
  if (scope & LSP_ON_WORKSPACE)
    sb_append(b, first ? "\"workspace\"" : ",\"workspace\"");
  sb_append(b, "]");
}

/* `nytrix/commands`: what an editor may offer, with display titles. */
static void handle_command_list(const char *id) {
  sbuf_t b = {0};
  sb_append(&b, "{\"commands\":[");
  for (size_t i = 0; i < sizeof(lsp_commands) / sizeof(lsp_commands[0]); ++i) {
    const lsp_command_t *c = &lsp_commands[i];
    sb_append(&b, i ? ",{\"id\":" : "{\"id\":");
    sb_append_json(&b, c->id);
    sb_append(&b, ",\"title\":");
    sb_append_json(&b, c->title);
    sb_append(&b, ",\"scope\":");
    lsp_append_scope(&b, c->scope);
    if (c->input_prompt) {
      sb_append(&b, ",\"input\":");
      sb_append_json(&b, c->input_prompt);
    }
    sb_append(&b, "}");
  }
  sb_append(&b, "]}");
  send_result(id, b.data);
  free(b.data);
}

static void lsp_append_command_ids(sbuf_t *b) {
  for (size_t i = 0; i < sizeof(lsp_commands) / sizeof(lsp_commands[0]); ++i) {
    if (i)
      sb_append(b, ",");
    sb_append_json(b, lsp_commands[i].id);
  }
}

static void lsp_send_message_action(const char *id, const char *text) {
  sbuf_t b = {0};
  sb_append(&b, "{\"kind\":\"message\",\"text\":");
  sb_append_json(&b, text);
  sb_append(&b, "}");
  send_result(id, b.data);
  free(b.data);
}

static void handle_execute_command(const char *id, const char *body) {
  char *command_id = json_extract_string(body, "command");
  const lsp_command_t *c = lsp_find_command(command_id);
  const char *args = strstr(body, "\"arguments\"");
  char *uri = args ? json_extract_string(args, "uri") : NULL;
  char *input = args ? json_extract_string(args, "input") : NULL;
  int line = 0;
  if (args)
    (void)json_extract_int_near(args, NULL, "line", &line);
  char *file = uri ? uri_to_path(uri) : NULL;
  unsigned scope = file ? lsp_scope_for_path(file) : LSP_ON_WORKSPACE;
  if (!c) {
    lsp_send_message_action(id, "Unknown Nytrix command.");
  } else if (!(c->scope & scope) && !(c->scope & LSP_ON_WORKSPACE)) {
    char text[256];
    snprintf(text, sizeof(text), "%s needs an open %s.", c->title,
             (c->scope & LSP_ON_NSHAPE) && !(c->scope & LSP_ON_NY)
                 ? ".nshape fixture"
                 : "Nytrix file");
    lsp_send_message_action(id, text);
  } else if (c->input_prompt && (!input || !*input)) {
    lsp_send_message_action(id, "This command needs input.");
  } else {
    if (strcmp(c->id, "nytrix.check") == 0 && uri) {
      lsp_doc_t *doc = doc_find(uri);
      if (doc)
        check_document(uri, doc->text);
    }
    /* Workspace-only commands (tests) run from the root, not the file's dir. */
    bool workspace_only = (c->scope & LSP_ON_WORKSPACE) &&
                          !(c->scope & (LSP_ON_NY | LSP_ON_NSHAPE));
    char *cwd = (file && !(workspace_only && lsp_workspace_root))
                    ? lsp_dirname(file)
                    : strdup(lsp_workspace_root ? lsp_workspace_root : ".");
    sbuf_t b = {0};
    if (!c->tool) {
      const char *base = file ? strrchr(file, '/') : NULL;
      base = base ? base + 1 : (file ? file : "program");
      sb_append(&b, "{\"kind\":\"debug\",\"save\":");
      sb_append_json(&b, uri ? uri : "");
      sb_append(&b, ",\"configuration\":{\"type\":\"nytrix\","
                    "\"request\":\"launch\",\"name\":");
      char name[512];
      snprintf(name, sizeof(name), "Nytrix: Debug %s", base);
      sb_append_json(&b, name);
      sb_append(&b, ",\"program\":");
      sb_append_json(&b, file ? file : "");
      sb_append(&b, ",\"cwd\":");
      sb_append_json(&b, cwd ? cwd : ".");
      sb_appendf(&b, ",\"sourceLine\":%d}}", line);
    } else {
      char tool[4096];
      lsp_tool_path(tool, sizeof(tool), c->tool);
      char title[256];
      snprintf(title, sizeof(title), "Nytrix: %s", c->title);
      sb_append(&b, "{\"kind\":\"process\",\"title\":");
      sb_append_json(&b, title);
      sb_append(&b, ",\"command\":");
      sb_append_json(&b, tool);
      sb_append(&b, ",\"args\":[");
      for (size_t i = 0; c->argv[i]; ++i) {
        const char *arg = c->argv[i];
        if (strcmp(arg, "{file}") == 0)
          arg = file ? file : "";
        else if (strcmp(arg, "{input}") == 0)
          arg = input ? input : "";
        if (i)
          sb_append(&b, ",");
        sb_append_json(&b, arg);
      }
      sb_append(&b, "],\"cwd\":");
      sb_append_json(&b, cwd ? cwd : ".");
      bool uses_file = false;
      for (size_t i = 0; c->argv[i]; ++i)
        uses_file |= strcmp(c->argv[i], "{file}") == 0;
      if (uri && uses_file) {
        sb_append(&b, ",\"save\":");
        sb_append_json(&b, uri);
      }
      if (c->modifies_file && uri) {
        sb_append(&b, ",\"reload\":");
        sb_append_json(&b, uri);
      }
      sb_append(&b, "}");
    }
    send_result(id, b.data);
    free(b.data);
    free(cwd);
  }
  free(file);
  free(input);
  free(uri);
  free(command_id);
}

/* Code lenses                                                              */

static void lsp_append_lens(sbuf_t *b, bool *first, int line, const char *title,
                            const char *command, const char *uri) {
  sb_append(b, *first ? "{\"range\":" : ",{\"range\":");
  *first = false;
  append_range(b, line, 0, line, 0);
  sb_append(b, ",\"command\":{\"title\":");
  sb_append_json(b, title);
  sb_append(b, ",\"command\":");
  sb_append_json(b, command);
  sb_append(b, ",\"arguments\":[{\"uri\":");
  sb_append_json(b, uri);
  sb_appendf(b, ",\"line\":%d}]}}", line);
}

static void handle_code_lens(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  lsp_doc_t *doc = uri ? doc_find(uri) : NULL;
  sbuf_t b = {0};
  sb_append(&b, "[");
  bool first = true;
  if (doc && doc->text) {
    char *path = uri_to_path(uri);
    if (lsp_scope_for_path(path) == LSP_ON_NSHAPE) {
      int line = 0;
      for (const char *p = doc->text; *p; ++line) {
        const char *q = p;
        while (*q == ' ' || *q == '\t')
          ++q;
        if (strncmp(q, "shape ", 6) == 0 && q == p) {
          lsp_append_lens(&b, &first, line, "▶ Run shape", "nytrix.shape",
                          uri);
          lsp_append_lens(&b, &first, line, "Debug", "nytrix.debug", uri);
        } else if (strncmp(q, "source ny", 9) == 0) {
          lsp_append_lens(&b, &first, line, "Debug this source",
                          "nytrix.debug", uri);
        }
        const char *eol = strchr(p, '\n');
        if (!eol)
          break;
        p = eol + 1;
      }
    } else {
      lsp_append_lens(&b, &first, 0, "▶ Run", "nytrix.run", uri);
      lsp_append_lens(&b, &first, 0, "Debug", "nytrix.debug", uri);
    }
    free(path);
  }
  sb_append(&b, "]");
  send_result(id, b.data);
  free(b.data);
  free(uri);
}

/* Formatting through ny-fmt                                                */

static void handle_formatting(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  lsp_doc_t *doc = uri ? doc_find(uri) : NULL;
  char *path = uri ? uri_to_path(uri) : NULL;
  char *formatted = NULL;
  if (doc && doc->text && path) {
    const char *dot = strrchr(path, '.');
    char temp[4096];
    const char *tmp = ny_get_temp_dir();
#ifdef _WIN32
    unsigned long pid = GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    snprintf(temp, sizeof(temp), "%s/nytrix-lsp-fmt-%lu%s",
             tmp && *tmp ? tmp : ".", pid, dot ? dot : ".ny");
    FILE *f = fopen(temp, "wb");
    if (f) {
      fwrite(doc->text, 1, strlen(doc->text), f);
      fclose(f);
      char fmt[4096];
      lsp_tool_path(fmt, sizeof(fmt), "ny-fmt");
      const char *argv[] = {fmt, "--fix", temp, NULL};
      char *ignored = NULL;
      if (ny_process_capture(argv, &ignored, true) == 0)
        formatted = ny_read_file(temp);
      free(ignored);
      remove(temp);
    }
  }
  if (!formatted || strcmp(formatted, doc->text) == 0) {
    send_result(id, "[]");
  } else {
    int last_line = 0;
    for (const char *p = doc->text; *p; ++p)
      if (*p == '\n')
        ++last_line;
    sbuf_t b = {0};
    sb_append(&b, "[{\"range\":");
    append_range(&b, 0, 0, last_line + 1, 0);
    sb_append(&b, ",\"newText\":");
    sb_append_json(&b, formatted);
    sb_append(&b, "}]");
    send_result(id, b.data);
    free(b.data);
  }
  free(formatted);
  free(path);
  free(uri);
}
