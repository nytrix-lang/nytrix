#include "base/loader.h"
#include "base/util.h"
#include "parse/parser.h"
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

void doclist_set(doc_list_t *dl, const char *name, const char *doc, const char *def,
                 const char *src, int kind) {
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

static void doclist_add_function_entry(doc_list_t *dl, stmt_t *s, const char *prefix, int kind,
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
    const char *vararg = (s->as.fn.is_variadic && j + 1 == s->as.fn.params.len) ? "..." : "";
    if (p->type && *p->type) {
      repl_doc_buf_appendf(&def, "%s%s %s%s", vararg, p->type, p->name,
                           p->def ? "=..." : "");
    } else {
      repl_doc_buf_appendf(&def, "%s%s%s", vararg, p->name, p->def ? "=..." : "");
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
    src = ny_strndup(s->as.fn.src_start, (size_t)(s->as.fn.src_end - s->as.fn.src_start));
  }
  doclist_set(dl, name, s->as.fn.doc, def_buf, src, kind);
  free(def_buf);
  if (src)
    free(src);
  free(owned_name);
}

void doclist_add_recursive(doc_list_t *dl, ny_stmt_list *body, const char *prefix) {
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
      if (s->as.module.src_start && s->as.module.src_end > s->as.module.src_start) {
        src = ny_strndup(s->as.module.src_start,
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

static char *doclist_source_without_leading_docstring(const char *src) {
  if (!src)
    return NULL;
  const char *brace = strchr(src, '{');
  if (!brace)
    return NULL;
  const char *p = brace + 1;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p != '"' && *p != '\'')
    return NULL;
  char quote = *p++;
  int escaped = 0;
  int closed = 0;
  while (*p) {
    if (escaped) {
      escaped = 0;
    } else if (*p == '\\') {
      escaped = 1;
    } else if (*p == quote) {
      p++;
      closed = 1;
      break;
    }
    p++;
  }
  if (!closed)
    return NULL;
  while (*p && *p != '\n')
    p++;
  if (*p == '\n')
    p++;
  size_t prefix_len = (size_t)(brace - src + 1);
  size_t rest_len = strlen(p);
  char *out = malloc(prefix_len + 1 + rest_len + 1);
  if (!out)
    return NULL;
  memcpy(out, src, prefix_len);
  out[prefix_len] = '\n';
  memcpy(out + prefix_len + 1, p, rest_len + 1);
  return out;
}

static int doclist_dotted_suffix_match(const char *left, size_t left_len, const char *right,
                                       size_t right_len) {
  if (left_len > right_len)
    return left[left_len - right_len - 1] == '.' && strcmp(left + left_len - right_len, right) == 0;
  if (right_len > left_len)
    return right[right_len - left_len - 1] == '.' && strcmp(right + right_len - left_len, left) == 0;
  return 0;
}

int doclist_print(const doc_list_t *dl, const char *name) {
  if (!dl || !name || !*name)
    return 0;
  const char *base_name = strrchr(name, '.');
  if (base_name)
    base_name += 1;
  else
    base_name = name;
  int found_idx = -1;
  for (size_t i = 0; i < dl->len; ++i) {
    if (strcmp(dl->data[i].name, name) == 0) {
      found_idx = (int)i;
      goto found;
    }
  }
  int match_idx = -1;
  int match_count = 0;
  size_t name_len = strlen(name);
  for (size_t i = 0; i < dl->len; ++i) {
    const char *entry_name = dl->data[i].name;
    size_t entry_len = strlen(entry_name);
    if (doclist_dotted_suffix_match(entry_name, entry_len, name, name_len)) {
      match_idx = (int)i;
      match_count++;
    }
  }
  if (match_count == 1) {
    found_idx = match_idx;
    goto found;
  } else if (match_count > 1) {
    printf("%sMultiple matches found for '%s':%s\n", clr(NY_CLR_YELLOW), name, clr(NY_CLR_RESET));
    for (size_t i = 0; i < dl->len; ++i) {
      const char *en = dl->data[i].name;
      size_t el = strlen(en);
      if (doclist_dotted_suffix_match(en, el, name, name_len)) {
        printf("  - %s\n", en);
      }
    }
    return 1;
  }
  match_idx = -1;
  match_count = 0;
  for (size_t i = 0; i < dl->len; ++i) {
    const char *entry_name = dl->data[i].name;
    const char *entry_base = strrchr(entry_name, '.');
    entry_base = entry_base ? entry_base + 1 : entry_name;
    if (strcmp(entry_base, base_name) == 0) {
      match_idx = (int)i;
      match_count++;
    }
  }
  if (match_count == 1) {
    found_idx = match_idx;
    goto found;
  } else if (match_count > 1) {
    printf("%sMultiple basename matches found for '%s':%s\n", clr(NY_CLR_YELLOW), name,
           clr(NY_CLR_RESET));
    for (size_t i = 0; i < dl->len; ++i) {
      const char *entry_name = dl->data[i].name;
      const char *entry_base = strrchr(entry_name, '.');
      entry_base = entry_base ? entry_base + 1 : entry_name;
      if (strcmp(entry_base, base_name) == 0)
        printf("  - %s\n", entry_name);
    }
    return 1;
  }
  return 0;
found: {
  ny_doc_entry *e = &dl->data[found_idx];
  const char *k_name = "Symbol";
  if (e->kind == 1)
    k_name = "Package";
  else if (e->kind == 2)
    k_name = "Module";
  else if (e->kind == 3)
    k_name = "Function";
  else if (e->kind == 5)
    k_name = "Method";
  printf("\n%s%s%s%s %s%s%s\n", clr(NY_CLR_BOLD), clr(NY_CLR_MAGENTA), k_name,
         clr(NY_CLR_RESET), clr(NY_CLR_CYAN), e->name, clr(NY_CLR_RESET));
  if (e->def)
    printf("  %s%s%s\n", clr(NY_CLR_GREEN), e->def, clr(NY_CLR_RESET));
  if (e->doc && strlen(e->doc) > 0) {
    printf("  %s\n", e->doc);
  } else {
    printf("  %s(no documentation string available)%s\n", clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
  }
  if (e->src && (e->kind == 3 || e->kind == 5) && doclist_show_source()) {
    printf("\n%sSource%s\n", clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
    const char *s = e->src;
    while (*s && isspace((unsigned char)*s))
      s++;
    char *clean_src = doclist_source_without_leading_docstring(s);
    repl_highlight_line(clean_src ? clean_src : s);
    free(clean_src);
    printf("\n");
  }
  printf("\n");
  return 1;
}
}

void add_builtin_docs(doc_list_t *docs) {
#define RT_DEF(name, p, args, sig, doc) doclist_set(docs, name, doc, sig, NULL, 3);
#define RT_GV(name, p, t, doc) doclist_set(docs, name, doc, "global", NULL, 4);
#ifdef _WIN32
#ifdef rt_argc
#undef rt_argc
#endif
#ifdef rt_argv
#undef rt_argv
#endif
#endif
#include "rt/defs.h"
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
