/*
 * REPL documentation: `:doc` command implementation that extracts
 * and displays function/type docstrings from the loaded module set.
 */
#include "base/loader.h"
#include "base/util.h"
#include "code/lang/parse/parser.h"
#include "nytrix/nytrix.h"
#include "priv.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} repl_doc_buf_t;

static int repl_doc_buf_reserve(repl_doc_buf_t *b, size_t need) {
  if (!b)
    return 0;
  if (need <= b->cap)
    return 1;
  size_t cap = b->cap ? b->cap : 128;
  while (cap < need) {
    if (cap > ((size_t)-1) / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  char *data = realloc(b->data, cap);
  if (!data)
    return 0;
  b->data = data;
  b->cap = cap;
  return 1;
}

static int repl_doc_buf_appendf(repl_doc_buf_t *b, const char *fmt, ...) {
  if (!b || !fmt)
    return 0;
  va_list ap;
  va_start(ap, fmt);
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (n < 0) {
    va_end(ap);
    return 0;
  }
  size_t need = b->len + (size_t)n + 1;
  if (!repl_doc_buf_reserve(b, need)) {
    va_end(ap);
    return 0;
  }
  vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
  return 1;
}

static char *repl_doc_buf_take(repl_doc_buf_t *b) {
  if (!b)
    return NULL;
  if (!b->data) {
    b->data = ny_strdup("");
    b->cap = b->data ? 1 : 0;
  }
  char *data = b->data;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  return data;
}

static char *repl_doc_qualified_name(const char *prefix, const char *name) {
  if (!prefix || !*prefix || !name || !*name)
    return NULL;
  size_t prefix_len = strlen(prefix);
  if (strncmp(name, prefix, prefix_len) == 0)
    return NULL;
  size_t name_len = strlen(name);
  char *out = malloc(prefix_len + 1 + name_len + 1);
  if (!out)
    return NULL;
  memcpy(out, prefix, prefix_len);
  out[prefix_len] = '.';
  memcpy(out + prefix_len + 1, name, name_len + 1);
  return out;
}

static int doclist_find_name(const doc_list_t *dl, const char *name) {
  if (!dl || !name || !*name)
    return 0;
  for (size_t i = 0; i < dl->len; ++i) {
    if (dl->data[i].name && strcmp(dl->data[i].name, name) == 0)
      return 1;
  }
  return 0;
}

static int doclist_has_module_docs(const doc_list_t *dl, const char *name) {
  if (!dl || !name || !*name)
    return 0;
  if (doclist_find_name(dl, name))
    return 1;
  size_t name_len = strlen(name);
  for (size_t i = 0; i < dl->len; ++i) {
    const char *entry = dl->data[i].name;
    if (!entry)
      continue;
    if (strncmp(entry, name, name_len) == 0 && entry[name_len] == '.')
      return 1;
  }
  return 0;
}

void doclist_set(doc_list_t *dl, const char *name, const char *doc,
                 const char *def, const char *src, int kind) {
  if (!dl || !name)
    return;
  for (size_t i = 0; i < dl->len; ++i) {
    if (dl->data[i].name && dl->data[i].name[0] == name[0] &&
        strcmp(dl->data[i].name, name) == 0) {
      if (doc) {
        free(dl->data[i].doc);
        dl->data[i].doc = ny_strdup(doc);
      }
      if (def) {
        free(dl->data[i].def);
        dl->data[i].def = ny_strdup(def);
      }
      if (src) {
        free(dl->data[i].src);
        dl->data[i].src = ny_strdup(src);
      }
      if (kind != 0)
        dl->data[i].kind = kind;
      return;
    }
  }
  if (dl->len == dl->cap) {
    size_t new_cap = dl->cap ? dl->cap * 2 : 64;
    ny_doc_entry *nd = realloc(dl->data, new_cap * sizeof(ny_doc_entry));
    if (!nd)
      return;
    memset(nd + dl->len, 0, (new_cap - dl->len) * sizeof(ny_doc_entry));
    dl->data = nd;
    dl->cap = new_cap;
  }
  char *owned_name = ny_strdup(name);
  if (!owned_name)
    return;
  dl->data[dl->len].name = owned_name;
  dl->data[dl->len].doc = doc ? ny_strdup(doc) : NULL;
  dl->data[dl->len].def = def ? ny_strdup(def) : NULL;
  dl->data[dl->len].src = src ? ny_strdup(src) : NULL;
  dl->data[dl->len].kind = kind;
  dl->len += 1;
}

static void doclist_add_function_entry(doc_list_t *dl, stmt_t *s,
                                       const char *prefix, int kind,
                                       int qualify_with_prefix) {
  if (!dl || !s || s->kind != NY_S_FUNC)
    return;
  char *owned_name = NULL;
  const char *name = s->as.fn.name;
  if (qualify_with_prefix)
    owned_name = repl_doc_qualified_name(prefix, name);
  if (owned_name)
    name = owned_name;

  repl_doc_buf_t def = {0};
  repl_doc_buf_appendf(&def, "fn %s(", name);
  for (size_t j = 0; j < s->as.fn.params.len; ++j) {
    const char *sep = (j + 1 < s->as.fn.params.len) ? ", " : "";
    const param_t *p = &s->as.fn.params.data[j];
    const char *vararg =
        (s->as.fn.is_variadic && j + 1 == s->as.fn.params.len) ? "..." : "";
    if (p->type && *p->type) {
      repl_doc_buf_appendf(&def, "%s%s %s%s", vararg, p->type, p->name,
                           p->def ? "=..." : "");
    } else {
      repl_doc_buf_appendf(&def, "%s%s%s", vararg, p->name,
                           p->def ? "=..." : "");
    }
    if (sep[0])
      repl_doc_buf_appendf(&def, "%s", sep);
  }
  repl_doc_buf_appendf(&def, ")");
  if (s->as.fn.return_type && *s->as.fn.return_type)
    repl_doc_buf_appendf(&def, " %s", s->as.fn.return_type);
  char *def_buf = repl_doc_buf_take(&def);
  char *src = NULL;
  if (s->as.fn.src_start && s->as.fn.src_end > s->as.fn.src_start) {
    src = ny_strndup(s->as.fn.src_start,
                     (size_t)(s->as.fn.src_end - s->as.fn.src_start));
  }
  doclist_set(dl, name, s->as.fn.doc, def_buf, src, kind);
  free(def_buf);
  if (src)
    free(src);
  free(owned_name);
}

void doclist_add_recursive(doc_list_t *dl, ny_stmt_list *body,
                           const char *prefix) {
  for (size_t i = 0; i < body->len; ++i) {
    stmt_t *s = body->data[i];
    if (s->kind == NY_S_FUNC) {
      doclist_add_function_entry(dl, s, prefix, 3, 1);
    } else if (s->kind == NY_S_MODULE) {
      char *owned_name = NULL;
      const char *name = s->as.module.name;
      owned_name = repl_doc_qualified_name(prefix, name);
      if (owned_name)
        name = owned_name;
      char *src = NULL;
      if (s->as.module.src_start &&
          s->as.module.src_end > s->as.module.src_start) {
        src =
            ny_strndup(s->as.module.src_start,
                       (size_t)(s->as.module.src_end - s->as.module.src_start));
      }
      doclist_set(dl, name, "Module", "module", src, 2);
      if (src)
        free(src);
      doclist_add_recursive(dl, &s->as.module.body, name);
      free(owned_name);
    } else if (s->kind == NY_S_IMPL) {
      for (size_t j = 0; j < s->as.impl.methods.len; ++j) {
        stmt_t *method = s->as.impl.methods.data[j];
        if (method && method->kind == NY_S_FUNC)
          doclist_add_function_entry(dl, method, s->as.impl.type_name, 5, 1);
      }
    }
  }
}

void doclist_add_from_prog(doc_list_t *dl, program_t *prog) {
  if (!dl || !prog)
    return;
  doclist_add_recursive(dl, &prog->body, NULL);
}

void doclist_free(doc_list_t *dl) {
  if (!dl || !dl->data)
    return;
  for (size_t i = 0; i < dl->len; ++i) {
    free(dl->data[i].name);
    if (dl->data[i].doc)
      free(dl->data[i].doc);
    if (dl->data[i].def)
      free(dl->data[i].def);
    if (dl->data[i].src)
      free(dl->data[i].src);
  }
  free(dl->data);
}

static int doclist_show_source(void) {
  const char *v = getenv("NYTRIX_REPL_HELP_SOURCE");
  return !v || ny_env_truthy(v);
}

/*
 * Source text of a doc entry without its leading docstring line, which the
 * description already shows. Returns a malloc'd string (never NULL on OK).
 */
static char *doc_source_body(const char *source) {
  while (*source && isspace((unsigned char)*source))
    ++source;
  const char *brace = strchr(source, '{');
  if (brace) {
    const char *p = brace + 1;
    while (*p && isspace((unsigned char)*p))
      ++p;
    if (*p == '"' || *p == '\'') {
      char quote = *p++;
      int escaped = 0, closed = 0;
      for (; *p && !closed; ++p) {
        if (escaped)
          escaped = 0;
        else if (*p == '\\')
          escaped = 1;
        else if (*p == quote)
          closed = 1;
      }
      if (closed) {
        while (*p && *p != '\n')
          ++p;
        if (*p)
          ++p;
        size_t head = (size_t)(brace - source) + 1, tail = strlen(p);
        char *out = malloc(head + 1 + tail + 1);
        if (!out)
          return NULL;
        memcpy(out, source, head);
        out[head] = '\n';
        memcpy(out + head + 1, p, tail + 1);
        return out;
      }
    }
  }
  return ny_strdup(source);
}

static int doclist_name_match(const char *name, const char *query, int suffix) {
  if (!name || !query)
    return 0;
  if (!suffix) {
    const char *leaf = strrchr(name, '.');
    leaf = leaf ? leaf + 1 : name;
    const char *qleaf = strrchr(query, '.');
    qleaf = qleaf ? qleaf + 1 : query;
    return strcmp(leaf, qleaf) == 0;
  }
  size_t nn = strlen(name), qn = strlen(query);
  return qn < nn && name[nn - qn - 1] == '.' &&
         strcmp(name + nn - qn, query) == 0;
}

/*
 * Resolve REPL documentation queries directly in C.
 */
static int doclist_resolve(const doc_list_t *dl, const char *query,
                           int *index_out, char **matches_out,
                           char **rendered_out) {
  if (index_out)
    *index_out = -1;
  if (matches_out)
    *matches_out = NULL;
  if (rendered_out)
    *rendered_out = NULL;
  if (!dl || !query || !*query || !index_out || !matches_out || !rendered_out ||
      dl->len > 100000)
    return 0;
  size_t match_count = 0, only_match = 0;
  for (size_t i = 0; i < dl->len; ++i)
    if (dl->data[i].name && strcmp(dl->data[i].name, query) == 0) {
      *index_out = (int)i;
      match_count = 1;
      only_match = i;
      break;
    }
  if (!match_count) {
    for (size_t i = 0; i < dl->len; ++i) {
      const char *name = dl->data[i].name;
      if (!doclist_name_match(name, query, 1))
        continue;
      if (match_count++ == 0)
        only_match = i;
    }
    if (!match_count)
      for (size_t i = 0; i < dl->len; ++i)
        if (doclist_name_match(dl->data[i].name, query, 0) &&
            match_count++ == 0)
          only_match = i;
    if (match_count == 1)
      *index_out = (int)only_match;
  }
  repl_doc_buf_t matches = {0}, rendered = {0};
  int ok = 1;
  if (match_count) {
    size_t n = match_count == 1 ? only_match : 0, emitted = 0;
    for (size_t i = 0; i < dl->len; ++i) {
      const char *name = dl->data[i].name;
      if (!name)
        continue;
      int mode = 1;
      if (match_count > 1) {
        int any_suffix = 0;
        for (size_t j = 0; j < dl->len; ++j)
          any_suffix |= doclist_name_match(dl->data[j].name, query, 1);
        mode = any_suffix ? 1 : 0;
      }
      int selected =
          match_count == 1 ? i == n : (doclist_name_match(name, query, mode));
      if (!selected)
        continue;
      if (emitted++)
        ok = ok && repl_doc_buf_appendf(&matches, ",");
      ok = ok && repl_doc_buf_appendf(&matches, "%zu", i);
      if (match_count > 1) {
        const char *y = clr(NY_CLR_YELLOW), *r = clr(NY_CLR_RESET);
        if (!emitted)
          ok = 0;
        if (emitted == 1)
          ok = ok && repl_doc_buf_appendf(
                         &rendered, "%sMultiple matches found for '%s':%s\n",
                         y ? y : "", query, r ? r : "");
        ok = ok && repl_doc_buf_appendf(&rendered, "  - %s\n", name);
      }
    }
  } else {
    const char *red = clr(NY_CLR_RED), *reset = clr(NY_CLR_RESET);
    ok =
        repl_doc_buf_appendf(&rendered, "%sNo documentation found for '%s'%s\n",
                             red ? red : "", query, reset ? reset : "");
  }
  *matches_out = ok ? repl_doc_buf_take(&matches) : NULL;
  *rendered_out = ok ? repl_doc_buf_take(&rendered) : NULL;
  free(matches.data);
  free(rendered.data);
  if (!*matches_out || !*rendered_out) {
    free(*matches_out);
    free(*rendered_out);
    *matches_out = NULL;
    *rendered_out = NULL;
    return 0;
  }
  return 1;
}

static int doclist_render(const ny_doc_entry *entry, char **rendered_out) {
  if (rendered_out)
    *rendered_out = NULL;
  if (!entry || !entry->name || !rendered_out)
    return 0;
  static const char *const labels[] = {"Symbol",   "Package", "Module",
                                       "Function", "Symbol",  "Method"};
  const char *label =
      entry->kind >= 0 && entry->kind < 6 ? labels[entry->kind] : "Symbol";
  const char *bold = clr(NY_CLR_BOLD), *magenta = clr(NY_CLR_MAGENTA),
             *reset = clr(NY_CLR_RESET), *cyan = clr(NY_CLR_CYAN),
             *green = clr(NY_CLR_GREEN), *gray = clr(NY_CLR_GRAY);
  repl_doc_buf_t rendered = {0};
  int ok = repl_doc_buf_appendf(
      &rendered, "\n%s%s%s %s%s%s\n", bold ? bold : "", magenta ? magenta : "",
      label, cyan ? cyan : "", entry->name, reset ? reset : "");
  if (ok && entry->def && entry->def[0])
    ok = repl_doc_buf_appendf(&rendered, "  %s%s%s\n", green ? green : "",
                              entry->def, reset ? reset : "");
  if (ok && entry->doc && entry->doc[0])
    ok = repl_doc_buf_appendf(&rendered, "  %s\n", entry->doc);
  else if (ok)
    ok = repl_doc_buf_appendf(&rendered,
                              "  %s(no documentation string available)%s\n",
                              gray ? gray : "", reset ? reset : "");
  if (ok)
    ok = repl_doc_buf_appendf(&rendered, "\n");
  *rendered_out = ok ? repl_doc_buf_take(&rendered) : NULL;
  free(rendered.data);
  return *rendered_out != NULL;
}

static int doclist_print_entry(const ny_doc_entry *e) {
  int has_source =
      e->src && (e->kind == 3 || e->kind == 5) && doclist_show_source();
  char *rendered = NULL;
  if (!doclist_render(e, &rendered))
    return 0;
  fputs(rendered, stdout);
  free(rendered);
  if (has_source) {
    /*
     * Same highlighter as the input line (fn names, types, comments).
     */
    const char *b = clr(NY_CLR_BOLD), *r = clr(NY_CLR_RESET);
    printf("%sSource%s\n", b ? b : "", r ? r : "");
    char *body = doc_source_body(e->src);
    if (body)
      repl_highlight_line(body);
    free(body);
    printf("\n");
  }
  printf("\n");
  return 1;
}

/*
 * Print docs for `name`. With best_of_many, an ambiguous name prints the
 * entry with the shortest qualified name (core.print over core.io.print)
 * instead of the "Multiple matches" list.
 */
static int doclist_print_impl(const doc_list_t *dl, const char *name,
                              int best_of_many) {
  if (!dl || !name || !*name)
    return 0;
  int found_idx = -1;
  char *matches = NULL;
  char *resolution_text = NULL;
  if (!doclist_resolve(dl, name, &found_idx, &matches, &resolution_text))
    return 0;
  int printed = 0;
  if (found_idx >= 0) {
    printed = doclist_print_entry(&dl->data[found_idx]);
  } else if (best_of_many && matches && *matches) {
    size_t best = 0, best_len = (size_t)-1;
    for (const char *p = matches; *p;) {
      char *end = NULL;
      size_t i = strtoul(p, &end, 10);
      if (end == p)
        break;
      size_t l = i < dl->len && dl->data[i].name ? strlen(dl->data[i].name)
                                                 : (size_t)-1;
      if (l < best_len) {
        best_len = l;
        best = i;
      }
      p = *end == ',' ? end + 1 : end;
    }
    if (best_len != (size_t)-1)
      printed = doclist_print_entry(&dl->data[best]);
  } else if (matches && *matches && resolution_text && *resolution_text) {
    fputs(resolution_text, stdout);
    printed = 1;
  }
  free(matches);
  free(resolution_text);
  return printed;
}

int doclist_print(const doc_list_t *dl, const char *name) {
  return doclist_print_impl(dl, name, 0);
}

int doclist_print_best(const doc_list_t *dl, const char *name) {
  return doclist_print_impl(dl, name, 1);
}

int doclist_print_missing(const doc_list_t *dl, const char *name) {
  int found_idx = -1;
  char *matches = NULL;
  char *rendered = NULL;
  if (!doclist_resolve(dl, name, &found_idx, &matches, &rendered))
    return 0;
  int missing = found_idx < 0 && (!matches || !*matches);
  int printed = missing && rendered && *rendered;
  if (printed)
    fputs(rendered, stdout);
  free(matches);
  free(rendered);
  return printed;
}

void add_builtin_docs(doc_list_t *docs) {
#define RT_DEF(name, p, args, sig, doc)                                        \
  doclist_set(docs, name, doc, sig, NULL, 3);
#define RT_GV(name, p, t, doc) doclist_set(docs, name, doc, "global", NULL, 4);
#ifdef _WIN32
#ifdef rt_argc
#undef rt_argc
#endif
#ifdef rt_argv
#undef rt_argv
#endif
#endif
#include "code/backend/runtime/defs.h"
#undef RT_DEF
#undef RT_GV
}

void repl_load_module_docs(doc_list_t *docs, const char *name) {
  if (!docs || !name || !*name)
    return;
  int idx = ny_std_find_module_by_name(name);
  if (idx < 0)
    return;
  const char *canon = ny_std_module_name((size_t)idx);
  if (!canon || !*canon)
    canon = name;
  if (doclist_has_module_docs(docs, canon))
    return;
  const char *path = ny_std_module_path((size_t)idx);
  char *src = repl_read_file(path);
  if (!src)
    return;
  parser_t ps;
  parser_init(&ps, src, path);
  program_t pr = parse_program(&ps);
  if (!ps.had_error) {
    if (pr.doc)
      doclist_set(docs, canon, pr.doc, "module", NULL, 2);
    doclist_add_recursive(docs, &pr.body, canon);
  }
  program_free(&pr, ps.arena);
  free(src);
}
