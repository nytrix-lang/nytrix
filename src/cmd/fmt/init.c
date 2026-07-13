#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "fmt.h"
#include "base/args.h"
#include "base/process.h"
#include "base/util.h"
#include "../tools/repo.h"
#include "../tools/tool.h"


#include "core.c"
static void usage(void) {
  nyt_heading("Nytrix Format And Audit");
  printf("%susage:%s %sny fmt%s %s[mode] [options] [paths ...]%s\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET),
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --cloc%s %s[--full] [--top N] [paths ...]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --dupes%s %s[--dupes-min N] [--dupes-emit] [--json] [paths ...]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --conv%s %s--input file.texi --name NAME [--format man|md] [-o out]%s\n\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("%smodes:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %s--check --fix --analyze --audit --trim --syntax --types --dead%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--smart --overhaul --bugs --checks --bloat --modules --profiles --layouts --loops%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--contracts --ffi --constants --specialize --metaprog --constfold%s\n\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("%soptions:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %s--json --tidy --optimize --apply --diff%s\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--color MODE --limit N --threshold N --root DIR --dirs DIR%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--min-sev CRIT|HIGH|MED|LOW --types-strict -v%s\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("  audit modes compose; use %s--audit=loops,trim%s to find continue/guard-loop flattening wins\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  accept justified smells with %sny-fmt: accept NYAUDxxxx reason%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
}

static char *str_replace_all(const char *in, const char *pat, const char *rep) {
  if (!in || !pat || !*pat || !rep)
    return in ? strdup(in) : NULL;
  size_t in_n = strlen(in), p_n = strlen(pat), r_n = strlen(rep);
  size_t count = 0;
  for (const char *p = strstr(in, pat); p; p = strstr(p + p_n, pat))
    count++;
  size_t out_n = in_n + count * (r_n - p_n) + 1;
  char *out = (char *)malloc(out_n);
  if (!out)
    return NULL;
  char *dst = out;
  const char *cur = in;
  while (1) {
    const char *p = strstr(cur, pat);
    if (!p) {
      size_t tail = strlen(cur);
      memcpy(dst, cur, tail);
      dst += tail;
      break;
    }
    size_t chunk = (size_t)(p - cur);
    memcpy(dst, cur, chunk);
    dst += chunk;
    memcpy(dst, rep, r_n);
    dst += r_n;
    cur = p + p_n;
  }
  *dst = '\0';
  return out;
}

typedef struct {
  const char *pat;
  const char *rep;
} ReplaceRule;

static void replace_rules_owned(char **s, const ReplaceRule *rules) {
  if (!s || !*s || !rules)
    return;
  for (int i = 0; rules[i].pat; i++) {
    char *tmp = str_replace_all(*s, rules[i].pat, rules[i].rep ? rules[i].rep : "");
    free(*s);
    *s = tmp ? tmp : strdup("");
  }
}

static char *convert_texi_basic(const char *input, const char *name, const char *fmt, const char *section) {
  char *s = strdup(input ? input : "");
  if (!s)
    return NULL;
  const ReplaceRule drops[] = {{"@contents", ""},      {"@appendix", ""},
                               {"@printindex", ""},    {"@node", ""},
                               {"@menu", ""},          {"@dircategory", ""},
                               {"@direntry", ""},      {"@titlepage", ""},
                               {"\\input texinfo", ""}, {"@setfilename", ""},
                               {"@settitle", ""},      {NULL, NULL}};
  replace_rules_owned(&s, drops);
  if (strcmp(fmt, "md") == 0) {
    const ReplaceRule md_rules[] = {{"@chapter ", "# "}, {"@section ", "## "},
                                    {"@subsection ", "### "}, {"@code{", "`"},
                                    {"}", "`"}, {NULL, NULL}};
    replace_rules_owned(&s, md_rules);
    char head[256];
    snprintf(head, sizeof(head), "# %s\n\n", name ? name : "Nytrix");
    size_t n = strlen(head) + strlen(s) + 2;
    char *out = (char *)malloc(n);
    if (!out) {
      free(s);
      return NULL;
    }
    snprintf(out, n, "%s%s", head, s);
    free(s);
    return out;
  }

  char header[512];
  snprintf(header, sizeof(header), ".TH %s %s \"\" \"\" \"Nytrix\"\n", name ? name : "nytrix",
           section ? section : "1");
  const ReplaceRule man_rules[] = {{"@chapter ", ".SH "}, {"@section ", ".SH "},
                                   {"@subsection ", ".SS "}, {"@code{", "\\fB"},
                                   {"}", "\\fP"}, {NULL, NULL}};
  replace_rules_owned(&s, man_rules);
  size_t n = strlen(header) + strlen(s) + 2;
  char *out = (char *)malloc(n);
  if (!out) {
    free(s);
    return NULL;
  }
  snprintf(out, n, "%s%s", header, s);
  free(s);
  return out;
}

typedef struct { char *data; size_t len; size_t cap; } sb_t2;
static int sb2_add(sb_t2 *b, const char *s) {
  size_t sl = strlen(s);
  size_t need = b->len + sl + 1;
  if (need > b->cap) {
    size_t newcap = b->cap ? b->cap : 4096;
    while (newcap < need) newcap *= 2;
    char *p = (char *)realloc(b->data, newcap);
    if (!p) return 0;
    b->data = p;
    b->cap = newcap;
  }
  memcpy(b->data + b->len, s, sl);
  b->len += sl;
  b->data[b->len] = '\0';
  return 1;
}
static int sb2_addn(sb_t2 *b, const char *s, size_t n) {
  size_t need = b->len + n + 1;
  if (need > b->cap) {
    size_t newcap = b->cap ? b->cap : 4096;
    while (newcap < need) newcap *= 2;
    char *p = (char *)realloc(b->data, newcap);
    if (!p) return 0;
    b->data = p;
    b->cap = newcap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 1;
}

static const char *c2ny_map_type(const char *ct) {
  if (!ct || !*ct) return "any";
  if (strcmp(ct, "int") == 0 || strcmp(ct, "signed") == 0) return "int";
  if (strcmp(ct, "unsigned") == 0 || strcmp(ct, "unsigned int") == 0) return "int";
  if (strcmp(ct, "float") == 0 || strcmp(ct, "double") == 0) return "f64";
  if (strcmp(ct, "long") == 0 || strcmp(ct, "long long") == 0) return "int";
  if (strcmp(ct, "unsigned long") == 0) return "int";
  if (strcmp(ct, "size_t") == 0) return "int";
  if (strcmp(ct, "char") == 0) return "int";
  if (strcmp(ct, "void") == 0) return "any";
  if (strcmp(ct, "_Bool") == 0 || strcmp(ct, "bool") == 0) return "bool";
  if (strcmp(ct, "uint8_t") == 0 || strcmp(ct, "int8_t") == 0) return "int";
  if (strcmp(ct, "uint16_t") == 0 || strcmp(ct, "int16_t") == 0) return "int";
  if (strcmp(ct, "uint32_t") == 0) return "int";
  if (strcmp(ct, "uint64_t") == 0 || strcmp(ct, "int64_t") == 0) return "int";
  if (strstr(ct, "*") || strstr(ct, "const char")) return "str";
  return "any";
}

typedef struct { char *data; size_t len; size_t cap; } sb_t;
static int sb_grow(sb_t *b, size_t need) {
  if (need <= b->cap) return 1;
  size_t nc = b->cap ? b->cap : 4096;
  while (nc < need) nc *= 2;
  char *p = realloc(b->data, nc);
  if (!p) return 0;
  b->data = p; b->cap = nc;
  return 1;
}
static void sb_add(sb_t *b, const char *s) {
  size_t n = strlen(s);
  if (sb_grow(b, b->len + n + 1)) { memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0; }
}
static void sb_addc(sb_t *b, char c) {
  if (sb_grow(b, b->len + 2)) { b->data[b->len++] = c; b->data[b->len] = 0; }
}
static void sb_addn(sb_t *b, const char *s, size_t n) {
  if (sb_grow(b, b->len + n + 1)) { memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0; }
}

static const char *skip_ws(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static int is_preproc(const char *s) { return *s == '#'; }

static int is_keyword(const char *s) {
  static const char *kws[] = {"if","for","while","switch","return","goto","break","continue","else","case","default","sizeof",NULL};
  for (int i = 0; kws[i]; i++) {
    size_t l = strlen(kws[i]);
    if (strncmp(s, kws[i], l) == 0 && (s[l] == ' ' || s[l] == '(' || s[l] == 0 || s[l] == ';'))
      return 1;
  }
  return 0;
}

static int is_func_def(const char *line) {
  const char *s = skip_ws(line);
  if (is_keyword(s)) return 0;
  if (strncmp(s, "static ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "inline ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "extern ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "const ", 6) == 0) s = skip_ws(s + 6);
  if (is_keyword(s)) return 0;
  if (!*s || *s == '*' || *s == '(' || *s == '}' || *s == '{') return 0;

  if (strncmp(s, "int", 3) != 0 && strncmp(s, "void", 4) != 0 && strncmp(s, "float", 5) != 0 &&
      strncmp(s, "double", 6) != 0 && strncmp(s, "char", 4) != 0 && strncmp(s, "long", 4) != 0 &&
      strncmp(s, "short", 5) != 0 && strncmp(s, "unsigned", 8) != 0 && strncmp(s, "size_t", 6) != 0 &&
      strncmp(s, "bool", 4) != 0 && strncmp(s, "_Bool", 5) != 0 && strncmp(s, "uint8", 5) != 0 &&
      strncmp(s, "uint16", 6) != 0 && strncmp(s, "uint32", 6) != 0 && strncmp(s, "uint64", 6) != 0 &&
      strncmp(s, "int8", 4) != 0 && strncmp(s, "int16", 5) != 0 && strncmp(s, "int32", 5) != 0 &&
      strncmp(s, "int64", 5) != 0 && strncmp(s, "const ", 6) != 0 && strncmp(s, "static ", 7) != 0 &&
      strncmp(s, "struct ", 7) != 0 && strncmp(s, "enum ", 5) != 0)
    return 0;

  while (*s && *s != '(' && *s != '{' && *s != ';' && *s != '=') s++;
  return *s == '(' && !strchr(line, '=');
}

static void parse_func_sig(const char *line, char *name, size_t nsz, char *rtype, size_t rsz) {
  const char *s = skip_ws(line);

  if (strncmp(s, "static ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "inline ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "extern ", 7) == 0) s = skip_ws(s + 7);

  const char *paren = strchr(s, '(');
  if (!paren) { name[0] = 0; snprintf(rtype, rsz, "any"); return; }

  const char *name_end = paren;
  while (name_end > s && (name_end[-1] == ' ' || name_end[-1] == '\t' || name_end[-1] == '*')) name_end--;
  const char *name_start = name_end;
  while (name_start > s && name_start[-1] != ' ' && name_start[-1] != '\t' && name_start[-1] != '*') name_start--;

  size_t nl = name_end - name_start;
  if (nl >= nsz) nl = nsz - 1;
  memcpy(name, name_start, nl); name[nl] = 0;

  size_t rl = name_start - s;
  if (rl >= rsz) rl = rsz - 1;
  while (rl > 0 && (s[rl-1] == ' ' || s[rl-1] == '\t')) rl--;
  memcpy(rtype, s, rl); rtype[rl] = 0;
  if (!rtype[0]) snprintf(rtype, rsz, "int");
}

static int c2ny_line(const char *line, sb_t *out, int *indent, int *in_func) {
  const char *s = skip_ws(line);
  if (!*s) { sb_add(out, "\n"); return 1; }

  if (*s == '#') {
    if (strncmp(s, "#include", 8) == 0) {
      sb_add(out, "#include"); sb_add(out, s + 8); sb_add(out, "\n");
    } else if (strncmp(s, "#define", 7) == 0) {
      sb_add(out, "def "); sb_addn(out, s + 8, strlen(s) - 8); sb_add(out, "\n");
    } else if (strncmp(s, "#if", 3) == 0 || strncmp(s, "#ifdef", 6) == 0 || strncmp(s, "#ifndef", 7) == 0) {
      sb_add(out, "#if"); sb_addn(out, s + (s[1]=='i'&&s[2]=='f'?3:s[1]=='i'&&s[2]=='f'&&s[3]=='d'?6:7), strlen(s)- (s[1]=='i'&&s[2]=='f'?3:6)); sb_add(out, " {\n");
      *indent += 1;
    } else if (strncmp(s, "#else", 5) == 0) {
      sb_add(out, "} #else {\n");
    } else if (strncmp(s, "#endif", 6) == 0) {
      *indent -= 1; sb_add(out, "}\n");
    } else {
      sb_add(out, ";; "); sb_add(out, s + 1); sb_add(out, "\n");
    }
    return 1;
  }

  if (s[0] == '/' && s[1] == '/') { sb_add(out, ";;"); sb_add(out, s + 2); sb_add(out, "\n"); return 1; }
  if (s[0] == '/' && s[1] == '*') { sb_add(out, ";;"); sb_addn(out, s + 2, strlen(s) - 4); sb_add(out, "\n"); return 1; }

  if (*s == '}') {
    *indent -= 1;
    if (*in_func && *indent == 0) { *in_func = 0; return 0; }
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "}\n");
    return 1;
  }

  if (strncmp(s, "struct ", 7) == 0 || strncmp(s, "typedef ", 8) == 0 || strncmp(s, "enum ", 5) == 0) {
    sb_add(out, ";; "); sb_addn(out, s, strlen(s)); sb_add(out, "\n");
    return 1;
  }

  if (is_func_def(line)) {
    char name[256], rtype[256];
    parse_func_sig(line, name, sizeof(name), rtype, sizeof(rtype));
    if (strcmp(name, "main") == 0) {
      sb_add(out, "#main {\n");
    } else {
      const char *ny_type = c2ny_map_type(rtype);

      const char *paren = strchr(line, '(');
      const char *close = paren ? strchr(paren, ')') : NULL;
      sb_add(out, "fn "); sb_add(out, name); sb_add(out, "(");
      if (paren && close && close > paren + 1) {
        char params[4096];
        size_t plen = (size_t)(close - paren - 1);
        if (plen >= sizeof(params)) plen = sizeof(params) - 1;
        memcpy(params, paren + 1, plen); params[plen] = 0;
        if (strcmp(params, "void") != 0 && params[0]) {

          char *ctx = NULL;
          char *tok = strtok_r(params, ",", &ctx);
          int first = 1;
          while (tok) {
            while (*tok == ' ' || *tok == '\t') tok++;

            char *name_part = strrchr(tok, ' ');
            if (!name_part) name_part = tok;
            else name_part++;

            char type_part[256] = {0};
            if (name_part > tok) {
              size_t tlen = (size_t)(name_part - tok);
              while (tlen > 0 && (tok[tlen-1] == ' ' || tok[tlen-1] == '\t' || tok[tlen-1] == '*')) tlen--;
              if (tlen >= sizeof(type_part)) tlen = sizeof(type_part) - 1;
              memcpy(type_part, tok, tlen);
            }
            if (!first) sb_add(out, ", ");
            sb_add(out, c2ny_map_type(type_part)); sb_add(out, " "); sb_add(out, name_part);
            first = 0;
            tok = strtok_r(NULL, ",", &ctx);
          }
        }
      }
      sb_add(out, ") "); sb_add(out, ny_type); sb_add(out, " {\n");
    }
    *indent += 1; *in_func = 1;
    return 1;
  }

  if ((strncmp(s, "int ", 4) == 0 || strncmp(s, "int*", 4) == 0 || strncmp(s, "float ", 6) == 0 || strncmp(s, "double ", 7) == 0 ||
       strncmp(s, "char ", 5) == 0 || strncmp(s, "long ", 5) == 0 || strncmp(s, "short ", 6) == 0 ||
       strncmp(s, "unsigned ", 9) == 0 || strncmp(s, "size_t ", 7) == 0 || strncmp(s, "bool ", 5) == 0 ||
       strncmp(s, "_Bool ", 6) == 0 || strncmp(s, "uint", 4) == 0 || strncmp(s, "int", 3) == 0 ||
       strncmp(s, "const ", 6) == 0 || strncmp(s, "static ", 7) == 0 || strncmp(s, "auto ", 5) == 0 ||
       strncmp(s, "void *", 6) == 0 || strncmp(s, "void*", 5) == 0 || strncmp(s, "FILE *", 6) == 0 ||
       strncmp(s, "int *", 5) == 0 || strncmp(s, "int*", 4) == 0 || strncmp(s, "char *", 6) == 0 || strncmp(s, "char*", 5) == 0 ||
       strncmp(s, "float *", 7) == 0 || strncmp(s, "float*", 6) == 0) &&
      strchr(s, '=') && !strchr(s, '(')) {

    const char *var = s;
    while (*var && *var != ' ' && *var != '\t' && *var != '*') var++;
    if (strncmp(var, " *", 2) == 0) var += 2;
    while (*var == ' ' || *var == '\t' || *var == '*') var++;

    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "def ");

    const char *eq = strchr(var, '=');
    size_t name_len = eq ? (size_t)(eq - var) : strlen(var);
    while (name_len > 0 && (var[name_len-1] == ' ' || var[name_len-1] == '\t')) name_len--;
    sb_addn(out, var, name_len);
    sb_add(out, " = ");
    if (eq) {
      const char *val = skip_ws(eq + 1);
      size_t vlen = strlen(val);
      while (vlen > 0 && val[vlen-1] == ';') vlen--;
      sb_addn(out, val, vlen);
    }
    sb_add(out, "\n");
    return 1;
  }

  if ((strncmp(s, "int ", 4) == 0 || strncmp(s, "int*", 4) == 0 || strncmp(s, "float ", 6) == 0 || strncmp(s, "double ", 7) == 0 ||
       strncmp(s, "char ", 5) == 0 || strncmp(s, "size_t ", 7) == 0 || strncmp(s, "bool ", 5) == 0 ||
       strncmp(s, "uint", 4) == 0) && !strchr(s, '(')) {
    const char *var = s;
    while (*var && *var != ' ' && *var != '\t') var++;
    while (*var == ' ' || *var == '\t' || *var == '*') var++;
    size_t vlen = strlen(var);
    while (vlen > 0 && var[vlen-1] == ';') vlen--;
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "mut "); sb_addn(out, var, vlen); sb_add(out, " = 0\n");
    return 1;
  }

  if (strncmp(s, "return", 6) == 0 && (s[6] == ' ' || s[6] == ';' || s[6] == 0 || s[6] == '\n' || s[6] == '(')) {
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "return");
    if (s[6] != ';' && s[6] != 0) {
      const char *rest = skip_ws(s + 6);
      if (*rest && *rest != ';') { sb_add(out, " "); sb_add(out, rest); }
    }
    sb_add(out, "\n");
    return 1;
  }

  if (strncmp(s, "if (", 4) == 0) {
    const char *cond_end = strstr(s + 4, ") {");
    if (cond_end) {
      for (int i = 0; i < *indent; i++) sb_add(out, "   ");
      sb_add(out, "if "); sb_addn(out, s + 4, (size_t)(cond_end - s - 4)); sb_add(out, " {\n");
      *indent += 1;
      return 1;
    }
  }

  if (strncmp(s, "} else if (", 11) == 0 || strncmp(s, "else if (", 9) == 0) {
    const char *start = strstr(s, "if (");
    const char *cond_end = start ? strstr(start + 4, ") {") : NULL;
    if (start && cond_end) {
      *indent -= 1;
      for (int i = 0; i < *indent; i++) sb_add(out, "   ");
      sb_add(out, "} elif "); sb_addn(out, start + 4, (size_t)(cond_end - start - 4)); sb_add(out, " {\n");
      *indent += 1;
      return 1;
    }
  }

  if (strncmp(s, "} else {", 8) == 0 || strcmp(s, "else {") == 0) {
    *indent -= 1;
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "} else {\n");
    *indent += 1;
    return 1;
  }

  if (strncmp(s, "while (", 7) == 0) {
    const char *cond_end = strstr(s + 7, ") {");
    if (cond_end) {
      for (int i = 0; i < *indent; i++) sb_add(out, "   ");
      sb_add(out, "while "); sb_addn(out, s + 7, (size_t)(cond_end - s - 7)); sb_add(out, " {\n");
      *indent += 1;
      return 1;
    }
  }

  if (strncmp(s, "for (", 5) == 0) {

    char buf[4096];
    strncpy(buf, s, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    char *init = buf + 5;
    char *semi1 = strchr(init, ';');
    char *cond = semi1 ? semi1 + 1 : NULL;
    char *semi2 = cond ? strchr(cond, ';') : NULL;
    char *incr = semi2 ? semi2 + 1 : NULL;
    char *close = incr ? strchr(incr, ')') : NULL;

    if (semi1 && cond && semi2 && close) {
      *semi1 = 0; *semi2 = 0; *close = 0;

      char *sp = init;
      while (*sp == ' ' || *sp == '\t') sp++;

      while (*sp && *sp != ' ' && *sp != '\t') sp++;
      while (*sp == ' ' || *sp == '\t') sp++;
      char *varname = sp;
      char *eq = strchr(varname, '=');
      if (eq) { *eq = 0; while (eq > varname && (eq[-1]==' '||eq[-1]=='\t')) *--eq = 0; }

      char cbuf[256] = {0};
      char *cs = cond;
      while (*cs == ' ' || *cs == '\t') cs++;
      strncpy(cbuf, cs, sizeof(cbuf)-1);

      char ibuf[256] = {0};
      char *is = incr;
      while (*is == ' ' || *is == '\t') is++;
      strncpy(ibuf, is, sizeof(ibuf)-1);

      for (int i = 0; i < *indent; i++) sb_add(out, "   ");
      sb_add(out, "for "); sb_add(out, varname); sb_add(out, " in range(");

      const char *start_val = "0";
      if (eq) {
        const char *sv = skip_ws(eq + 1);

        while (*sv == ' ' || *sv == '\t') sv++;
        start_val = sv;
      }

      const char *end_val = cbuf;
      char *lt = strchr(cbuf, '<');
      if (lt) {
        *lt = 0;
        const char *cond_var = skip_ws(cbuf);

        if (strcmp(cond_var, varname) != 0) start_val = cond_var;
        end_val = skip_ws(lt + 1);
        if (*end_val == '=') end_val = skip_ws(end_val + 1);
      }

      sb_add(out, start_val); sb_add(out, ", "); sb_add(out, end_val);

      if (strstr(ibuf, "++") && !strstr(ibuf, "+=")) {

      } else if (strstr(ibuf, "+=")) {
        sb_add(out, ", ");
        const char *step_val = strstr(ibuf, "+=") + 2;
        sb_add(out, skip_ws(step_val));
      } else if (strstr(ibuf, "--")) {
        sb_add(out, ", -1");
      }

      sb_add(out, ")\n");
      *indent += 1;
      return 1;
    }

    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, ";; for: "); sb_addn(out, s, strlen(s)); sb_add(out, "\n");
    return 1;
  }

  if (strncmp(s, "printf(", 7) == 0) {
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "print(");
    const char *rest = s + 7;

    const char *close = strrchr(rest, ')');
    if (close) { sb_addn(out, rest, (size_t)(close - rest)); sb_add(out, ")"); }
    else sb_add(out, rest);
    sb_add(out, "\n");
    return 1;
  }

  if (strncmp(s, "malloc(", 7) == 0 || strncmp(s, "calloc(", 7) == 0 || strncmp(s, "free(", 5) == 0 ||
      strncmp(s, "realloc(", 8) == 0 || strncmp(s, "memset(", 7) == 0 || strncmp(s, "memcpy(", 7) == 0) {
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");

    size_t slen = strlen(s);
    while (slen > 0 && s[slen-1] == ';') slen--;
    sb_addn(out, s, slen); sb_add(out, "\n");
    return 1;
  }

  if (strstr(s, "NULL")) {
    char buf[4096];
    strncpy(buf, s, sizeof(buf)-1); buf[sizeof(buf)-1]=0;

    char *npos;
    while ((npos = strstr(buf, "NULL")) != NULL) {
      memmove(npos + 3, npos + 4, strlen(npos + 4) + 1);
      memcpy(npos, "nil", 3);
    }
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, buf); sb_add(out, "\n");
    return 1;
  }

  for (int i = 0; i < *indent; i++) sb_add(out, "   ");
  size_t slen = strlen(s);
  while (slen > 0 && (s[slen-1] == ';' || s[slen-1] == ' ' || s[slen-1] == '\t')) slen--;
  sb_addn(out, s, slen); sb_add(out, "\n");
  return 1;
}

static int run_c2ny(const char *input_path, const char *output_path) {
  size_t n = 0;
  char *src = ny_read_file_raw(input_path, &n);
  if (!src) {
    nyt_err("ny-fmt", "c2ny: failed to read %s", input_path);
    return 1;
  }

  sb_t out = {0};
  sb_add(&out, ";; Generated by ny-fmt --c2ny from "); sb_add(&out, input_path); sb_add(&out, "\n");
  sb_add(&out, "use std.core\n\n");

  int indent = 0, in_func = 0;
  char *line = src, *end = src + n;

  while (line < end) {
    char *nl = memchr(line, '\n', (size_t)(end - line));
    size_t llen = nl ? (size_t)(nl - line) : (size_t)(end - line);

    while (llen > 0 && line[llen-1] == '\r') llen--;

    char lbuf[8192];
    size_t copy = llen < sizeof(lbuf) - 1 ? llen : sizeof(lbuf) - 1;
    memcpy(lbuf, line, copy);
    lbuf[copy] = 0;

    c2ny_line(lbuf, &out, &indent, &in_func);

    line = nl ? nl + 1 : end;
  }

  while (indent > 0) {
    indent--;
    for (int i = 0; i < indent; i++) sb_add(&out, "   ");
    sb_add(&out, "}\n");
  }

  free(src);

  if (!write_file(output_path, out.data, out.len)) {
    nyt_err("ny-fmt", "c2ny: failed to write %s", output_path);
    free(out.data);
    return 1;
  }

  printf("c2ny: %s -> %s (%zu bytes)\n", input_path, output_path, out.len);
  free(out.data);
  return 0;
}

static int run_conv(const FmtOpts *o) {
  if (!o->conv_input || !o->conv_name) {
    nyt_err("ny-fmt", "--conv requires --input and --name");
    return 2;
  }
  size_t n = 0;
  char *src = ny_read_file_raw(o->conv_input, &n);
  if (!src) {
    nyt_err("ny-fmt", "conv: failed to read %s", o->conv_input);
    return 1;
  }
  const char *fmt = o->conv_format ? o->conv_format : "man";
  char *out = convert_texi_basic(src, o->conv_name, fmt, o->conv_section ? o->conv_section : "1");
  free(src);
  if (!out) {
    nyt_err("ny-fmt", "conv: conversion failed");
    return 1;
  }
  int rc = 0;
  if (o->conv_output) {
    if (!write_file(o->conv_output, out, strlen(out))) {
      nyt_err("ny-fmt", "conv: failed to write %s", o->conv_output);
      rc = 1;
    }
  } else {
    fputs(out, stdout);
  }
  free(out);
  return rc;
}

typedef struct {
  const char *arg;
  const char *mode;
} FmtAuditAlias;

static const FmtAuditAlias k_fmt_audit_aliases[] = {
    {"audit", "all"},      {"all", "all"},             {"bloat", "bloat"},
    {"modules", "modules"}, {"profiles", "profiles"},   {"batteries", "batteries"},
    {"bugs", "bugs"},      {"bug", "bugs"},             {"correctness", "bugs"},
    {"lint", "bugs"},      {"checks", "bugs"},          {"bugchecks", "bugs"},
    {"sanity", "bugs"},
    {"trim", "trim"},      {"layouts", "layouts"},     {"layout", "layouts"},
    {"contracts", "contracts"}, {"backend-contracts", "contracts"},
    {"specialize", "specialize"}, {"specialization", "specialize"},
    {"constfold", "specialize"}, {"partial", "specialize"},
    {"metaprog", "metaprog"}, {"meta", "metaprog"}, {"roadmap", "metaprog"},
    {"codebase", "metaprog"}, {"features", "metaprog"},
    {"ffi", "ffi"},        {"dead", "dead"},           {"calls", "calls"},
    {"similarities", "calls"}, {"types", "types"},      {"legacy", "legacy"},
    {"methods", "methods"}, {"method-syntax", "methods"}, {"syntax", "methods"},
    {"smart", "smart"},    {"overhaul", "smart"},      {"constants", "constants"},
    {"consts", "constants"},
};

static const char *fmt_audit_mode_for_arg(const char *arg) {
  if (!arg || !*arg)
    return NULL;
  if (arg[0] == '-' && arg[1] == '-')
    arg += 2;
  for (size_t i = 0; i < sizeof(k_fmt_audit_aliases) / sizeof(k_fmt_audit_aliases[0]); i++) {
    if (strcmp(arg, k_fmt_audit_aliases[i].arg) == 0)
      return k_fmt_audit_aliases[i].mode;
  }
  return NULL;
}

static void fmt_audit_mode_set(FmtOpts *o, const char *mode) {
  if (!o)
    return;
  snprintf(o->audit_mode_buf, sizeof(o->audit_mode_buf), "%s",
           (mode && *mode) ? mode : "all");
  o->audit_mode = o->audit_mode_buf;
}

static void fmt_audit_mode_add(FmtOpts *o, const char *mode) {
  if (!o || !mode || !*mode)
    return;
  if (strcmp(mode, "all") == 0) {
    fmt_audit_mode_set(o, "all");
    return;
  }
  if (strcmp(o->audit_mode_buf, "all") == 0)
    o->audit_mode_buf[0] = '\0';
  if (token_list_contains(o->audit_mode_buf, mode)) {
    o->audit_mode = o->audit_mode_buf;
    return;
  }
  size_t used = strlen(o->audit_mode_buf);
  if (used > 0 && used + 1 < sizeof(o->audit_mode_buf)) {
    o->audit_mode_buf[used++] = '|';
    o->audit_mode_buf[used] = '\0';
  }
  if (used < sizeof(o->audit_mode_buf) - 1) {
    strncat(o->audit_mode_buf, mode, sizeof(o->audit_mode_buf) - used - 1);
  }
  o->audit_mode = o->audit_mode_buf[0] ? o->audit_mode_buf : "all";
}

static int parse_args(int argc, char **argv, FmtOpts *o) {
  memset(o, 0, sizeof(*o));
  o->c2ny_output = "out.ny";
  o->min_sev = "LOW";
  o->conv_format = "man";
  o->conv_section = "1";
  o->color = -2;
  o->limit = 80;
  o->cloc_top = 20;
  o->dupes_min = 30;
  o->audit_mode = "all";
  char err[256];
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage();
      return 1;
    }
    int color_mode = -2;
    int color_idx = i;
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode, err, sizeof(err));
    if (color_rc < 0) {
      nyt_err("ny-fmt", "%s", err);
      return 0;
    }
    if (color_rc > 0) {
      o->color = color_mode;
      i = color_idx;
      continue;
    }

    const char *audit_mode = fmt_audit_mode_for_arg(a);
    if (audit_mode) {
      o->audit = 1;
      fmt_audit_mode_add(o, audit_mode);
    } else if (strcmp(a, "--analyze") == 0) {
      o->analyze = 1;
    } else if (strcmp(a, "--cloc") == 0 || strcmp(a, "cloc") == 0) {
      o->cloc = 1;
    } else if (strcmp(a, "--dupes") == 0 || strcmp(a, "--duplicates") == 0 ||
               strcmp(a, "dupes") == 0 || strcmp(a, "duplicates") == 0) {
      o->dupes = 1;
    } else if (strcmp(a, "--dupes-emit") == 0 || strcmp(a, "dupes-emit") == 0) {
      o->dupes = 1;
      o->dupes_emit = 1;
    } else if (strcmp(a, "--dupes-min") == 0 && i + 1 < argc) {
      o->dupes = 1;
      o->dupes_min = atoi(argv[++i]);
    } else if (strncmp(a, "--dupes-min=", 12) == 0) {
      o->dupes = 1;
      o->dupes_min = atoi(a + 12);
    } else if (strcmp(a, "--full") == 0 || strcmp(a, "-f") == 0) {
      o->cloc_full = 1;
    } else if (strcmp(a, "--top") == 0 && i + 1 < argc) {
      o->cloc_top = atoi(argv[++i]);
    } else if (strncmp(a, "--top=", 6) == 0) {
      o->cloc_top = atoi(a + 6);
    } else if (strcmp(a, "--audit-mode") == 0 && i + 1 < argc) {
      o->audit = 1;
      fmt_audit_mode_set(o, argv[++i]);
    } else if (strncmp(a, "--audit-mode=", 13) == 0) {
      o->audit = 1;
      fmt_audit_mode_set(o, a + 13);
    } else if (strcmp(a, "--check") == 0) {
      o->check = 1;
    } else if (strcmp(a, "--fix") == 0) {
      o->fix = 1;
    } else if (strcmp(a, "--json") == 0) {
      o->json = 1;
    } else if (strcmp(a, "--types-strict") == 0) {
      o->audit = 1;
      fmt_audit_mode_add(o, "types");
      o->types_strict = 1;
    } else if (strcmp(a, "--limit") == 0 && i + 1 < argc) {
      o->limit = atoi(argv[++i]);
    } else if (strncmp(a, "--limit=", 8) == 0) {
      o->limit = atoi(a + 8);
    } else if (strcmp(a, "--threshold") == 0 && i + 1 < argc) {
      i++;
    } else if (strncmp(a, "--threshold=", 12) == 0) {

    } else if (strcmp(a, "--root") == 0 && i + 1 < argc) {
      i++;
    } else if (strncmp(a, "--root=", 7) == 0) {

    } else if (strcmp(a, "--dirs") == 0 && i + 1 < argc) {
      sv_push(&o->paths, argv[++i]);
    } else if (strncmp(a, "--dirs=", 7) == 0) {
      sv_push(&o->paths, a + 7);
    } else if (strcmp(a, "--tidy") == 0) {
      o->tidy = 1;
    } else if (strcmp(a, "--optimize") == 0) {
      o->optimize = 1;
    } else if (strcmp(a, "--apply") == 0) {
      o->apply = 1;
    } else if (strcmp(a, "--diff") == 0) {
      o->diff = 1;
    } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
      o->verbose = 1;
    } else if (strcmp(a, "--align") == 0 || strcmp(a, "--align-macros") == 0) {
      o->align_macros = 1;
    } else if (strcmp(a, "--c2ny") == 0) {
      o->c2ny = 1;
    } else if (strcmp(a, "--conv") == 0) {
      o->conv = 1;
    } else if (strcmp(a, "--input") == 0 && i + 1 < argc) {
      o->conv_input = argv[++i];
    } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
      o->conv_name = argv[++i];
    } else if (strcmp(a, "--format") == 0 && i + 1 < argc) {
      o->conv_format = argv[++i];
    } else if (strcmp(a, "--section") == 0 && i + 1 < argc) {
      o->conv_section = argv[++i];
    } else if ((strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) && i + 1 < argc) {
      o->conv_output = argv[++i];
    } else if (strcmp(a, "--min-sev") == 0 && i + 1 < argc) {
      o->min_sev = argv[++i];
    } else if (strncmp(a, "--min-sev=", 10) == 0) {
      o->min_sev = a + 10;
    } else if (a[0] == '-') {
      nyt_err("ny-fmt", "unknown option: %s", a);
      return 0;
    } else {
      sv_push(&o->paths, a);
    }
  }
  return 2;
}

static void run_check_mode(const FmtOpts *opts) {
  StrVec files = {0};
  if (opts->paths.len == 0) {
    collect_files_rec("lib", &files, 1);
    collect_files_rec("etc/tests", &files, 1);
  } else {
    for (size_t i = 0; i < opts->paths.len; i++)
      collect_files_rec(opts->paths.items[i], &files, 1);
  }

  size_t check_count = 0;
  for (size_t i = 0; i < files.len; i++) {
    if (!is_expected_error_fixture(files.items[i]))
      check_count++;
  }
  nyt_msg("CHECK", NYT_CYAN, "scanning %zu files for parse bugs", check_count);

  int failed = 0;
  for (size_t i = 0; i < files.len; i++) {
    if (is_expected_error_fixture(files.items[i]))
      continue;
    int issue = 0;
    brace_check_file(files.items[i], opts->fix, opts->verbose, &issue);
    if (issue)
      failed++;
  }
  if (failed == 0)
    nyt_msg("OK", NYT_GREEN, "check complete: all %zu files OK", check_count);
  else
    nyt_msg("CHECK", NYT_RED, "%d file(s) with issues", failed);
  sv_free(&files);
}

static int run_align_macros_mode(const FmtOpts *opts) {
  StrVec files = {0};
  if (opts->paths.len == 0) {
    collect_c_files_rec("src", &files);
  } else {
    for (size_t i = 0; i < opts->paths.len; i++)
      collect_c_files_rec(opts->paths.items[i], &files);
  }
  int changed = 0;
  for (size_t i = 0; i < files.len; i++) {
    size_t n = 0;
    char *src = ny_read_file_raw(files.items[i], &n);
    if (!src) continue;
    char *dst = malloc(n * 2 + 1);
    if (!dst) { free(src); continue; }
    size_t di = 0, si = 0;
    int block_changed = 0;
    while (si < n) {
      const char *line_start = src + si;
      const char *nl = memchr(line_start, '\n', n - si);
      size_t line_len = nl ? (size_t)(nl - line_start) : n - si;
      const char *trimmed = line_start;
      while (trimmed < line_start + line_len && (*trimmed == ' ' || *trimmed == '\t'))
        trimmed++;
      if (strncmp(trimmed, "#define ", 8) == 0 && nl && si + line_len + 1 < n) {

        const char *next = src + si + line_len + 1;
        const char *next_trim = next;
        while (next_trim < src + n && (*next_trim == ' ' || *next_trim == '\t'))
          next_trim++;
        const char *next_nl = memchr(next, '\n', n - (next - src));

        const char *bs = memchr(next, '\\', next_nl ? (size_t)(next_nl - next) : (n - (next - src)));
        if (bs && strncmp(next_trim, "do {", 4) == 0) {

          memcpy(dst + di, line_start, line_len);
          di += line_len; si += line_len + 1;
          if (dst[di - 1] != '\n') dst[di++] = '\n';

          while (si < n) {
            const char *bl = src + si;
            const char *bnl = memchr(bl, '\n', n - si);
            size_t bl_len = bnl ? (size_t)(bnl - bl) : n - si;
            memcpy(dst + di, bl, bl_len);
            di += bl_len; si += bl_len + 1;
            if (!bnl) break;

            if (strstr(bl, "while (0)") || strstr(bl, "while(0)")) {
              if (dst[di - 1] != '\n') dst[di++] = '\n';
              break;
            }
          }
          continue;
        }
      }

      memcpy(dst + di, line_start, line_len);
      di += line_len;
      si += line_len + 1;
      if (si < n && dst[di - 1] != '\n') dst[di++] = '\n';
    }
    dst[di] = '\0';
    if (block_changed) {
      if (write_file(files.items[i], dst, di))
        changed++;
    }
    free(dst);
    free(src);
  }
  nyt_msg("ALIGN", changed ? NYT_GREEN : NYT_GRAY, "aligned macros in %d file(s)", changed);
  sv_free(&files);
  return 0;
}

int ny_fmt_main(int argc, char **argv) {
  char root[PATH_MAX];
  if (!ensure_repo_root(root, sizeof(root))) {
    nyt_err("ny-fmt", "could not locate repository root");
    return 1;
  }
  if (chdir(root) != 0) {
    nyt_err("ny-fmt", "failed to chdir to root: %s", root);
    return 1;
  }

  FmtOpts opts;
  int ps = parse_args(argc, argv, &opts);
  if (ps == 0) {
    sv_free(&opts.paths);
    return 2;
  }
  if (ps == 1) {
    sv_free(&opts.paths);
    return 0;
  }
  if (opts.json)
    ny_setenv("NYTRIX_TOOL_COLOR", "never", 1);
  else if (opts.color == 1)
    ny_setenv("NYTRIX_TOOL_COLOR", "always", 1);
  else if (opts.color == 0)
    ny_setenv("NYTRIX_TOOL_COLOR", "never", 1);
  else if (opts.color == -1)
    ny_setenv("NYTRIX_TOOL_COLOR", "auto", 1);
  if (opts.limit < 0)
    opts.limit = 0;

  if (opts.tidy) {
    opts.check = 1;
    opts.analyze = 1;
  }

  if (opts.c2ny) {
    const char *in = opts.paths.len > 0 ? opts.paths.items[0] : NULL;
    const char *out = opts.conv_output ? opts.conv_output : "out.ny";
    if (!in) { nyt_err("ny-fmt", "--c2ny requires an input C file"); sv_free(&opts.paths); return 2; }
    int rc = run_c2ny(in, out);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.align_macros) {
    int rc = run_align_macros_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.conv) {
    int rc = run_conv(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.cloc) {
    int rc = run_cloc_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.dupes) {
    int rc = run_dupes_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  int only_default_fmt =
      !(opts.analyze || opts.audit || opts.check || opts.optimize || opts.tidy || opts.dupes);

  if (only_default_fmt || opts.tidy) {
    StrVec files = {0};
    if (opts.paths.len == 0) {
      collect_files_rec("src", &files, 0);
      collect_files_rec("lib", &files, 0);
      collect_files_rec("etc/tests", &files, 0);
    } else {
      for (size_t i = 0; i < opts.paths.len; i++)
        collect_files_rec(opts.paths.items[i], &files, 0);
    }
    int changed = 0;
    for (size_t i = 0; i < files.len; i++) {
      int chg = 0;
      if (format_file(files.items[i], &chg) && chg)
        changed++;
    }
    nyt_msg("FMT", changed ? NYT_GREEN : NYT_GRAY, "complete (%d files updated)", changed);
    sv_free(&files);
  }

  if (opts.check || opts.tidy)
    run_check_mode(&opts);

  if (opts.analyze || opts.optimize || opts.tidy)
    run_analyze_simple(&opts.paths, opts.json, opts.limit);

  int audit_rc = 0;
  if (opts.audit)
    audit_rc = run_audit_simple(&opts.paths, opts.audit_mode, opts.json, opts.limit,
                                opts.min_sev, opts.types_strict);

  if (opts.optimize && opts.apply) {
    StrVec files = {0};
    if (opts.paths.len == 0) {
      collect_files_rec("src", &files, 1);
      collect_files_rec("lib", &files, 1);
      collect_files_rec("etc/tests", &files, 1);
    } else {
      for (size_t i = 0; i < opts.paths.len; i++)
        collect_files_rec(opts.paths.items[i], &files, 1);
    }
    int changed = 0;
    for (size_t i = 0; i < files.len; i++) {
      int chg = 0;
      if (format_file(files.items[i], &chg) && chg)
        changed++;
    }
    if (opts.diff)
      nyt_warn("ny-fmt", "optimize --diff is not yet implemented in C mode");
    nyt_msg("OPT", NYT_GREEN, "applied updates to %d file(s)", changed);
    sv_free(&files);
  }

  sv_free(&opts.paths);
  return audit_rc;
}
