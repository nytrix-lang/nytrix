#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/loader.h"
#ifdef _WIN32
#include "base/compat.h"
#endif
#include "base/util.h"
#include "parse/parser.h"
#include "priv.h"
#include "repl/types.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <sys/stat.h>

typedef enum { CTX_NORMAL, CTX_MEMBER, CTX_STRING, CTX_COMMAND, CTX_USE } compl_ctx_t;

static const char *g_sort_prefix = NULL;
static compl_ctx_t g_sort_ctx = CTX_NORMAL;

static const char *const k_repl_commands[] = {
    ":help",  ":h",    ":doc",     ":exit",  ":quit", ":q",    ":clear",
    ":cls",   ":reset", ":time",    ":trace", ":expand",       ":vars",
    ":env",   ":history",          ":hist",  ":pwd",  ":ls",   ":cd",
    ":load",  ":run",  ":save",    ":std",   ":cancel",        ":c",
    ":complete", NULL};

static const char *const k_syntax_words[] = {
    "fn",       "def",      "mut",      "if",       "else",     "elif",
    "while",    "for",      "in",       "return",   "break",    "continue",
    "use",      "module",   "as",       "lambda",   "defer",    "try",
    "catch",    "throw",    "finally",  "case",     "match",    "enum",
    "struct",   "layout",   "extern",   "embed",    "comptime", "impl",
    "operator", "self",     "true",     "false",    "nil",      "none",
    "del",      "goto",     "export",   "type",     NULL};

static const char *const k_type_words[] = {
    "any",     "bool",      "int",      "i8",       "i16",      "i32",
    "i64",     "i128",      "u8",       "u16",      "u32",      "u64",
    "u128",    "f32",       "f64",      "f128",     "float",    "number",
    "numeric", "integer",   "bigint",   "str",      "char",     "bytes",
    "list",    "tuple",     "dict",     "set",      "range",    "ptr",
    "handle",  "fnptr",     "seq",      "sequence", "iterable", "indexable",
    "collection", "container", "allocator", "complex", "c64", "c128",
    NULL};

static const char *const k_member_words[] = {
    "len",      "get",       "set",       "put",       "add",       "append",
    "pop",      "extend",    "contains",  "slice",     "keys",      "values",
    "items",    "merge",     "map",       "filter",    "reduce",    "each",
    "count",    "count_if",  "first",     "last",      "take",      "drop",
    "reverse",  "compact",   "chunk",     "windowed",  "delete",    "remove",
    "clear",    "clone",     "long",      "to_bytes",  "to_list",   "unhex",
    "bytes",    "as_bytes",  "hex",       "base64",    "text",      "le32",
    "be32",     "le64",      "be64",      "u8",        "u16le",     "u16be",
    "u32le",    "u32be",     "u64le",     "u64be",     "xor",       "concat",
    "repeat",   "rev",       "trim0",     "bytes_long", "type_shape", "is_shape",
    "require_shape", "abs",  "min",       "max",        "pow",        "mod",
    "clamp",    "clamp01",   "sign",      "sqrt",      "lerp",      "sin",
    "cos",      "tan",       "atan",      "asin",      "acos",      "exp",
    "log",      "log2",      "log10",     "fmod",      "floor",     "ceil",
    "round",    "gcd",       "lcm",       "factorial", "x",         "y",
    "z",        "w",         "r",         "g",         "b",         "a",
    NULL};

static const char *const k_any_member_words[] = {
    "long", "to_bytes", "as_bytes", "unhex", "type_shape", "is_shape",
    "require_shape", "len", "get", "set", "put", "add", "append", "contains",
    "slice", NULL};

static const char *const k_list_member_words[] = {
    "len",      "get",       "set",       "put",      "add",      "append",
    "pop",      "extend",    "clear",     "contains", "slice",    "map",
    "filter",   "reduce",    "each",      "count",    "count_if", "first",
    "last",     "take",      "drop",      "reverse",  "compact",  "chunk",
    "windowed", "long",      "to_bytes",  "as_bytes", "bytes",    "hex",
    "base64",   "text",      "le32",      "be32",     "le64",     "be64",
    "xor",      "concat",    "repeat",    "rev",      "trim0",    "type_shape",
    "is_shape", "require_shape", NULL};

static const char *const k_str_member_words[] = {
    "len",      "get",       "contains", "slice",   "map",      "filter",
    "reduce",   "each",      "count",    "count_if", "first",    "last",
    "take",     "drop",      "reverse",  "chunk",   "windowed", "long",
    "to_bytes", "as_bytes",  "bytes",    "unhex",   "hex",      "base64",
    "base64_decode", "bytes_long", "u8", "le16",    "be16",     "le32",
    "be32",     "le64",      "be64",     "type_shape", "is_shape",
    "require_shape", NULL};

static const char *const k_bytes_member_words[] = {
    "len",      "get",      "set",     "put",      "long",     "to_list",
    "to_bytes", "as_bytes", "bytes",   "hex",      "base64",   "text",
    "u8",       "le16",     "be16",    "le32",     "be32",     "le64",
    "be64",     "type_shape", "is_shape", "require_shape", NULL};

static const char *const k_dict_member_words[] = {
    "len",    "clone", "get",    "set",   "put",   "contains", "delete",
    "remove", "clear", "keys",   "values", "items", "merge",    "type_shape",
    "is_shape", "require_shape", NULL};

static const char *const k_set_member_words[] = {
    "len", "add", "sub", "remove", "delete", "contains", "clear", "values",
    "type_shape", "is_shape", "require_shape", NULL};

static const char *const k_tuple_member_words[] = {
    "len",    "get",     "contains", "map",    "filter", "reduce", "count",
    "count_if", "first", "last",     "take",   "drop",   "reverse", "compact",
    "chunk", "windowed", "type_shape", "is_shape", "require_shape", NULL};

static const char *const k_range_member_words[] = {
    "len",     "get",      "keys",    "values", "items",   "map",
    "filter",  "reduce",   "count",   "count_if", "first", "last",
    "take",    "drop",     "reverse", "chunk",  "windowed", "contains",
    "type_shape", "is_shape", "require_shape", NULL};

static const char *const k_number_member_words[] = {
    "abs",    "min",       "max",       "pow",      "mod",       "clamp",
    "clamp01", "sign",     "sqrt",      "lerp",     "sin",       "cos",
    "tan",    "atan",      "asin",      "acos",     "exp",       "log",
    "log2",   "log10",     "fmod",      "floor",    "ceil",      "round",
    "gcd",    "lcm",       "factorial", "Z",        "bigint",    "powmod",
    "invmod", "sqrt_mod",  "is_prime",  "next_prime", "prev_prime", "factor",
    "phi",    "bitlen",    "bytes",     "as_bytes", "hex",       "as_hex",
    "type_shape", "is_shape", "require_shape", NULL};

static void add_match(const char *s);

#define MATCH_HT_INIT 256
static const char **match_ht = NULL;
static size_t match_ht_cap = 0;
static size_t match_ht_used = 0;

static uint32_t _match_hash(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 16777619u;
  }
  return h;
}

static bool match_ht_contains(const char *s) {
  if (!match_ht || match_ht_cap == 0)
    return false;
  uint32_t h = _match_hash(s);
  size_t idx = h & (match_ht_cap - 1);
  for (size_t probe = 0; probe < match_ht_cap; probe++) {
    const char *slot = match_ht[(idx + probe) & (match_ht_cap - 1)];
    if (!slot)
      return false;
    if (strcmp(slot, s) == 0)
      return true;
  }
  return false;
}

static void match_ht_insert(const char *s) {
  if (!match_ht || match_ht_used * 2 >= match_ht_cap) {
    size_t new_cap = match_ht_cap ? match_ht_cap * 2 : MATCH_HT_INIT;
    const char **new_ht = calloc(new_cap, sizeof(const char *));
    if (!new_ht)
      return;
    if (match_ht) {
      for (size_t i = 0; i < match_ht_cap; i++) {
        if (match_ht[i]) {
          uint32_t h2 = _match_hash(match_ht[i]);
          size_t idx2 = h2 & (new_cap - 1);
          while (new_ht[idx2])
            idx2 = (idx2 + 1) & (new_cap - 1);
          new_ht[idx2] = match_ht[i];
        }
      }
      free(match_ht);
    }
    match_ht = new_ht;
    match_ht_cap = new_cap;
  }
  uint32_t h = _match_hash(s);
  size_t idx = h & (match_ht_cap - 1);
  while (match_ht[idx])
    idx = (idx + 1) & (match_ht_cap - 1);
  match_ht[idx] = s;
  match_ht_used++;
}

static void match_ht_reset(void) {
  if (match_ht) {
    memset(match_ht, 0, match_ht_cap * sizeof(const char *));
  }
  match_ht_used = 0;
}

static compl_ctx_t get_context(const char *line, int pos) {
  if (!line || pos < 0)
    return CTX_NORMAL;
  if (line[0] == ':')
    return CTX_COMMAND;
  int in_str = 0;
  for (int i = 0; i < pos; i++) {
    if (line[i] == '"' && (i == 0 || line[i - 1] != '\\'))
      in_str = !in_str;
  }
  if (in_str)
    return CTX_STRING;
  if (pos >= 4 && strncmp(line, "use ", 4) == 0)
    return CTX_USE;
  if (pos > 0 && line[pos - 1] == '.')
    return CTX_MEMBER;
  for (int i = pos - 1; i >= 0; i--) {
    if (isspace((unsigned char)line[i]))
      break;
    if (line[i] == '.')
      return CTX_MEMBER;
  }
  return CTX_NORMAL;
}

static int is_break_char(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\\' || c == '\'' || c == '`' ||
         c == '@' || c == '$' || c == '>' || c == '<' || c == '=' || c == ';' || c == '|' ||
         c == '&' || c == '{' || c == '}' || c == '.' || c == '(';
}

static int is_member_break_char(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\\' || c == '\'' || c == '`' ||
         c == '@' || c == '$' || c == '>' || c == '<' || c == '=' || c == ';' || c == '|' ||
         c == '&' || c == '{' || c == '}' || c == '(';
}

static void extract_prefix(const char *line, int pos, char *out, size_t out_cap) {
  if (!out || out_cap == 0) {
    return;
  }
  out[0] = '\0';
  if (!line || pos <= 0) {
    return;
  }
  if ((size_t)pos > strlen(line))
    pos = (int)strlen(line);
  int start = pos;
  while (start > 0 && !is_break_char(line[start - 1])) {
    start--;
  }
  int len = pos - start;
  if (len <= 0)
    return;
  if ((size_t)len >= out_cap)
    len = (int)out_cap - 1;
  memcpy(out, line + start, (size_t)len);
  out[len] = '\0';
}

static void extract_command_arg(const char *line, int cursor, char *out, size_t out_cap) {
  if (!out || out_cap == 0)
    return;
  out[0] = '\0';
  if (!line || cursor <= 0 || line[0] != ':')
    return;
  int start = 1;
  while (line[start] && !isspace((unsigned char)line[start]))
    start++;
  while (line[start] && isspace((unsigned char)line[start]))
    start++;
  if (start >= cursor)
    return;
  int len = cursor - start;
  if ((size_t)len >= out_cap)
    len = (int)out_cap - 1;
  memcpy(out, line + start, (size_t)len);
  out[len] = '\0';
}

static void add_list_completions(const char *const *items, const char *text) {
  if (!items)
    return;
  for (int i = 0; items[i]; i++) {
    if (repl_fuzzy_score(items[i], text, 1) > 0)
      add_match(items[i]);
  }
}

static void add_member_word_completions(const char *const *items, const char *text) {
  if (!items)
    return;
  for (int i = 0; items[i]; i++) {
    if (!text || !*text || repl_starts_with_ci(items[i], text))
      add_match(items[i]);
  }
}

static const char *last_segment(const char *s) {
  const char *dot = strrchr(s, '.');
  const char *slash = strrchr(s, '/');
  const char *bslash = strrchr(s, '\\');
  const char *cut = dot;
  if (!cut || (slash && slash > cut))
    cut = slash;
  if (!cut || (bslash && bslash > cut))
    cut = bslash;
  return cut ? cut + 1 : s;
}

static int completion_symbol_matches(const char *cand, const char *text) {
  if (!cand)
    return 0;
  if (!text || !*text)
    return 1;
  size_t len = strlen(text);
  if (strchr(text, '.'))
    return len < 2 ? repl_starts_with_ci(cand, text) : repl_fuzzy_score(cand, text, 1) > 0;
  if (repl_starts_with_ci(cand, text))
    return 1;
  if (len < 2)
    return 0;
  return repl_fuzzy_score(last_segment(cand), text, 1) > 0;
}

static int completion_allows_private(const char *text) {
  return text && text[0] == '_';
}

static int command_matches(const char *line, const char *name) {
  if (!line || !name || line[0] != ':')
    return 0;
  size_t name_len = strlen(name);
  if (strncmp(line + 1, name, name_len) != 0)
    return 0;
  char next = line[1 + name_len];
  return next == '\0' || isspace((unsigned char)next);
}

static int completion_score(const char *cand, const char *text, compl_ctx_t ctx) {
  if (!cand)
    return 0;
  if (!text || !*text) {
    if (ctx == CTX_COMMAND && cand[0] == ':')
      return 500;
    if (ctx == CTX_USE && strncmp(cand, "std.", 4) == 0)
      return 450;
    return 100;
  }
  int score = repl_fuzzy_score(cand, text, 1);
  const char *base = last_segment(cand);
  if (strcmp(cand, text) == 0)
    score += 1000;
  else if (strcasecmp(cand, text) == 0)
    score += 900;
  if (strncmp(cand, text, strlen(text)) == 0)
    score += 700;
  if (repl_starts_with_ci(base, text))
    score += 650;
  const char *inside = repl_strcasestr(base, text);
  if (inside)
    score += 200 - (int)(inside - base > 64 ? 64 : (inside - base));
  if (ctx == CTX_COMMAND && cand[0] == ':')
    score += 80;
  if (ctx == CTX_USE && strncmp(cand, "std.", 4) == 0)
    score += 60;
  return score;
}

static char **matches = NULL;
static int matches_len = 0;
static int matches_cap = 0;

static void add_match(const char *s) {
  if (!s || !*s)
    return;
  if (match_ht_contains(s))
    return;
  if (matches_len >= matches_cap) {
    int new_cap = matches_cap ? matches_cap * 2 : 64;
    char **new_matches = realloc(matches, (size_t)new_cap * sizeof(char *));
    if (!new_matches)
      return;
    matches = new_matches;
    matches_cap = new_cap;
  }
  char *dup = ny_strdup(s);
  if (!dup)
    return;
  matches[matches_len++] = dup;
  match_ht_insert(dup);
}

static int match_cmp(const void *a, const void *b) {
  const char *sa = *(const char *const *)a;
  const char *sb = *(const char *const *)b;
  int as = completion_score(sa, g_sort_prefix, g_sort_ctx);
  int bs = completion_score(sb, g_sort_prefix, g_sort_ctx);
  if (as != bs)
    return (bs - as);
  return strcasecmp(sa, sb);
}

static void sort_matches(const char *prefix, compl_ctx_t ctx) {
  if (matches_len <= 1)
    return;
  g_sort_prefix = prefix;
  g_sort_ctx = ctx;
  qsort(matches, (size_t)matches_len, sizeof(char *), match_cmp);
  g_sort_prefix = NULL;
}

static void add_files(const char *text) {
  char dir_path[512] = ".";
  const char *prefix = text;
  const char *last_slash = strrchr(text, '/');
  const char *last_bslash = strrchr(text, '\\');
  const char *last_sep =
      (!last_slash)
          ? last_bslash
          : (!last_bslash ? last_slash : (last_bslash > last_slash ? last_bslash : last_slash));
  char sep = last_sep && *last_sep == '\\' ? '\\' : '/';
  if (last_sep) {
    size_t dlen = (size_t)(last_sep - text);
    if (dlen < sizeof(dir_path)) {
      memcpy(dir_path, text, dlen);
      dir_path[dlen] = '\0';
      if (dir_path[0] == '\0') {
        dir_path[0] = '/';
        dir_path[1] = '\0';
      }
      if (dlen == 2 && dir_path[1] == ':' && dlen + 1 < sizeof(dir_path)) {
        dir_path[dlen] = '\\';
        dir_path[dlen + 1] = '\0';
      }
    }
    prefix = last_sep + 1;
  }
  DIR *d = opendir(dir_path);
  if (!d)
    return;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.' && (prefix[0] != '.'))
      continue;
    if (strncmp(de->d_name, prefix, strlen(prefix)) == 0) {
      char full[1024];
      if (strcmp(dir_path, ".") == 0) {
        snprintf(full, sizeof(full), "%s", de->d_name);
      } else if (strcmp(dir_path, "/") == 0) {
        snprintf(full, sizeof(full), "/%s", de->d_name);
      } else {
        snprintf(full, sizeof(full), "%s%c%s", dir_path, sep, de->d_name);
      }
      struct stat st;
      if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t fl = strlen(full);
        if (fl + 2 < sizeof(full)) {
          full[fl] = sep;
          full[fl + 1] = '\0';
        }
      }
      add_match(full);
    }
  }
  closedir(d);
}

static int module_is_top_level_std(const char *m) {
  if (!m || strncmp(m, "std.", 4) != 0)
    return 0;
  return strchr(m + 4, '.') == NULL;
}

static void add_normal_completions(const char *text) {
  int empty = (!text || !*text);
  for (size_t i = 0; i < ny_std_package_count(); ++i) {
    const char *pkg = ny_std_package_name(i);
    if (completion_symbol_matches(pkg, text))
      add_match(pkg);
  }
  for (size_t i = 0; i < ny_std_module_count(); i++) {
    const char *m = ny_std_module_name(i);
    if ((empty && module_is_top_level_std(m)) || (!empty && completion_symbol_matches(m, text)))
      add_match(m);
  }
  add_list_completions(k_syntax_words, text);
  add_list_completions(k_type_words, text);
  if (!g_repl_docs || empty)
    return;
  repl_ensure_docs_for_query((doc_list_t *)g_repl_docs, text);
  const doc_list_t *d = (const doc_list_t *)g_repl_docs;
  for (size_t i = 0; i < d->len; i++) {
    const char *base = last_segment(d->data[i].name);
    if (!completion_allows_private(text) && base[0] == '_')
      continue;
    if (completion_symbol_matches(d->data[i].name, text))
      add_match(d->data[i].name);
    if (strlen(text) >= 2 && repl_fuzzy_score(base, text, 1) > 0)
      add_match(base);
  }
}

static void split_member_expr(const char *line, int cursor, char *head, size_t head_cap, char *tail,
                              size_t tail_cap) {
  if (head_cap)
    head[0] = '\0';
  if (tail_cap)
    tail[0] = '\0';
  if (!line || cursor <= 0)
    return;
  int start = cursor;
  while (start > 0 && !is_member_break_char(line[start - 1]))
    start--;
  int dot = -1;
  for (int i = cursor - 1; i >= start; i--) {
    if (line[i] == '.') {
      dot = i;
      break;
    }
  }
  if (dot < 0) {
    extract_prefix(line, cursor, tail, tail_cap);
    return;
  }
  int hlen = dot - start;
  if (hlen > 0 && (size_t)hlen < head_cap) {
    memcpy(head, line + start, (size_t)hlen);
    head[hlen] = '\0';
  }
  int tlen = cursor - dot - 1;
  if (tlen > 0 && (size_t)tlen < tail_cap) {
    memcpy(tail, line + dot + 1, (size_t)tlen);
    tail[tlen] = '\0';
  }
}

static void add_use_completions(const char *text) {
  char head[256] = {0};
  char tail[256] = {0};
  const char *dot = strrchr(text ? text : "", '.');
  if (dot) {
    size_t hlen = (size_t)(dot - text);
    if (hlen < sizeof(head)) {
      memcpy(head, text, hlen);
      head[hlen] = '\0';
      snprintf(tail, sizeof(tail), "%s", dot + 1);
    }
  } else {
    snprintf(tail, sizeof(tail), "%s", text ? text : "");
  }
  size_t mod_count = ny_std_module_count();
  for (size_t i = 0; i < mod_count; i++) {
    const char *m = ny_std_module_name(i);
    if (head[0]) {
      size_t head_len = strlen(head);
      if (strncmp(m, head, head_len) != 0 || m[head_len] != '.')
        continue;
      const char *next = m + head_len + 1;
      char seg[256] = {0};
      size_t j = 0;
      while (next[j] && next[j] != '.' && j + 1 < sizeof(seg)) {
        seg[j] = next[j];
        j++;
      }
      seg[j] = '\0';
      if (repl_fuzzy_score(seg, tail, 1) > 0)
        add_match(seg);
    } else if (repl_fuzzy_score(m, tail, 1) > 0) {
      add_match(m);
    }
  }
}

static int completion_head_is_namespace(const char *head) {
  if (!head || !*head)
    return 0;
  if (ny_std_find_module_by_name(head) >= 0)
    return 1;
  size_t head_len = strlen(head);
  for (size_t i = 0; i < ny_std_module_count(); i++) {
    const char *m = ny_std_module_name(i);
    if (strncmp(m, head, head_len) == 0 && m[head_len] == '.')
      return 1;
  }
  return 0;
}

typedef enum {
  RECV_UNKNOWN = 0,
  RECV_ANY,
  RECV_LIST,
  RECV_STR,
  RECV_BYTES,
  RECV_DICT,
  RECV_SET,
  RECV_TUPLE,
  RECV_RANGE,
  RECV_NUMBER,
  RECV_BOOL
} receiver_kind_t;

static void trim_receiver_head(const char *head, char *out, size_t out_cap) {
  if (!out || out_cap == 0)
    return;
  out[0] = '\0';
  if (!head)
    return;
  while (isspace((unsigned char)*head))
    head++;
  size_t len = strlen(head);
  while (len > 0 && isspace((unsigned char)head[len - 1]))
    len--;
  if (len >= out_cap)
    len = out_cap - 1;
  memcpy(out, head, len);
  out[len] = '\0';
  while (len >= 2 && out[0] == '(' && out[len - 1] == ')') {
    memmove(out, out + 1, len - 2);
    len -= 2;
    out[len] = '\0';
    while (len > 0 && isspace((unsigned char)out[0])) {
      memmove(out, out + 1, len);
      len--;
    }
    while (len > 0 && isspace((unsigned char)out[len - 1]))
      out[--len] = '\0';
  }
}

static receiver_kind_t classify_member_receiver(const char *head) {
  char h[256];
  trim_receiver_head(head, h, sizeof(h));
  if (!h[0])
    return RECV_UNKNOWN;
  if (h[0] == '[')
    return RECV_LIST;
  if (h[0] == '"' || h[0] == '\'')
    return RECV_STR;
  if ((h[0] == 'b' || h[0] == 'B') && (h[1] == '"' || h[1] == '\''))
    return RECV_BYTES;
  if (h[0] == '{')
    return strchr(h, ':') ? RECV_DICT : RECV_SET;
  if (strncmp(h, "range(", 6) == 0 || strstr(h, ".."))
    return RECV_RANGE;
  if (strcmp(h, "true") == 0 || strcmp(h, "false") == 0)
    return RECV_BOOL;
  if (repl_head_is_number(h))
    return RECV_NUMBER;
  return RECV_UNKNOWN;
}

static receiver_kind_t receiver_from_type_word(const char *type) {
  if (!type || !*type)
    return RECV_UNKNOWN;
  if (!strcmp(type, "list") || !strncmp(type, "list[", 5))
    return RECV_LIST;
  if (!strcmp(type, "str") || !strcmp(type, "string"))
    return RECV_STR;
  if (!strcmp(type, "bytes"))
    return RECV_BYTES;
  if (!strcmp(type, "dict") || !strncmp(type, "dict[", 5))
    return RECV_DICT;
  if (!strcmp(type, "set") || !strncmp(type, "set[", 4))
    return RECV_SET;
  if (!strcmp(type, "tuple") || !strncmp(type, "tuple[", 6))
    return RECV_TUPLE;
  if (!strcmp(type, "range"))
    return RECV_RANGE;
  if (!strcmp(type, "bool"))
    return RECV_BOOL;
  if (!strcmp(type, "int") || !strcmp(type, "float") || !strcmp(type, "number") ||
      !strcmp(type, "numeric") || !strcmp(type, "integer") || !strcmp(type, "bigint") ||
      !strcmp(type, "f32") || !strcmp(type, "f64"))
    return RECV_NUMBER;
  if (!strcmp(type, "any"))
    return RECV_ANY;
  return RECV_UNKNOWN;
}

static receiver_kind_t infer_receiver_from_expr(const char *expr) {
  if (!expr)
    return RECV_UNKNOWN;
  while (isspace((unsigned char)*expr))
    expr++;
  while (*expr == '(') {
    expr++;
    while (isspace((unsigned char)*expr))
      expr++;
  }
  if (*expr == '[')
    return RECV_LIST;
  if (*expr == '"' || *expr == '\'')
    return RECV_STR;
  if ((*expr == 'b' || *expr == 'B') && (expr[1] == '"' || expr[1] == '\''))
    return RECV_BYTES;
  if (*expr == '{')
    return strchr(expr, ':') ? RECV_DICT : RECV_SET;
  if (!strncmp(expr, "range(", 6))
    return RECV_RANGE;
  if (!strncmp(expr, "dict(", 5))
    return RECV_DICT;
  if (!strncmp(expr, "set(", 4))
    return RECV_SET;
  if (!strncmp(expr, "list(", 5))
    return RECV_LIST;
  if (!strncmp(expr, "str(", 4))
    return RECV_STR;
  if (!strncmp(expr, "bytes(", 6))
    return RECV_BYTES;
  if (!strncmp(expr, "true", 4) || !strncmp(expr, "false", 5))
    return RECV_BOOL;
  char head[128];
  size_t n = 0;
  while (expr[n] && !isspace((unsigned char)expr[n]) && strchr(",)]};", expr[n]) == NULL &&
         n + 1 < sizeof(head)) {
    head[n] = expr[n];
    n++;
  }
  head[n] = '\0';
  if (repl_head_is_number(head))
    return RECV_NUMBER;
  return RECV_UNKNOWN;
}

static int parse_ident_token(const char **pp, char *out, size_t out_cap) {
  if (!pp || !*pp || !out || out_cap == 0)
    return 0;
  const char *p = *pp;
  while (isspace((unsigned char)*p))
    p++;
  if (!(isalpha((unsigned char)*p) || *p == '_'))
    return 0;
  size_t n = 0;
  while ((isalnum((unsigned char)*p) || *p == '_') && n + 1 < out_cap)
    out[n++] = *p++;
  out[n] = '\0';
  while (isalnum((unsigned char)*p) || *p == '_')
    p++;
  *pp = p;
  return n > 0;
}

static receiver_kind_t infer_receiver_var_from_source(const char *src, const char *var) {
  if (!src || !var || !*var)
    return RECV_UNKNOWN;
  receiver_kind_t best = RECV_UNKNOWN;
  size_t var_len = strlen(var);
  const char *p = src;
  while (*p) {
    const char *line = p;
    const char *end = strchr(p, '\n');
    if (!end)
      end = p + strlen(p);
    while (line < end && isspace((unsigned char)*line))
      line++;
    const char *q = line;
    int decl = 0;
    if (end - q > 4 && !strncmp(q, "def ", 4)) {
      q += 4;
      decl = 1;
    } else if (end - q > 4 && !strncmp(q, "mut ", 4)) {
      q += 4;
      decl = 1;
    }
    if (decl) {
      char first[128] = {0};
      if (parse_ident_token(&q, first, sizeof(first))) {
        while (q < end && isspace((unsigned char)*q))
          q++;
        if (q < end && *q == ':') {
          q++;
          char second[128] = {0};
          const char *after_colon = q;
          if (parse_ident_token(&after_colon, second, sizeof(second))) {
            if (!strcmp(first, var)) {
              receiver_kind_t typed = receiver_from_type_word(second);
              if (typed != RECV_UNKNOWN)
                best = typed;
            } else if (!strcmp(second, var)) {
              receiver_kind_t typed = receiver_from_type_word(first);
              if (typed != RECV_UNKNOWN)
                best = typed;
            }
          }
          q = after_colon;
        } else if (!strcmp(first, var)) {
          while (q < end && isspace((unsigned char)*q))
            q++;
          if (q < end && *q == '=') {
            receiver_kind_t typed = infer_receiver_from_expr(q + 1);
            if (typed != RECV_UNKNOWN)
              best = typed;
          }
        }
      }
    } else if ((size_t)(end - q) > var_len && !strncmp(q, var, var_len) &&
               !(isalnum((unsigned char)q[var_len]) || q[var_len] == '_')) {
      q += var_len;
      while (q < end && isspace((unsigned char)*q))
        q++;
      if (q < end && *q == '=') {
        receiver_kind_t typed = infer_receiver_from_expr(q + 1);
        if (typed != RECV_UNKNOWN)
          best = typed;
      }
    }
    p = *end ? end + 1 : end;
  }
  return best;
}

static receiver_kind_t infer_member_receiver_from_variable(const char *line, int cursor,
                                                          const char *head) {
  char h[256];
  trim_receiver_head(head, h, sizeof(h));
  if (!h[0])
    return RECV_UNKNOWN;
  for (size_t i = 0; h[i]; ++i) {
    if (!(isalnum((unsigned char)h[i]) || h[i] == '_'))
      return RECV_UNKNOWN;
  }
  receiver_kind_t recv = infer_receiver_var_from_source(g_repl_user_source, h);
  if (recv != RECV_UNKNOWN)
    return recv;
  if (line && cursor > 0) {
    char *prefix = ny_strndup(line, (size_t)cursor);
    if (prefix) {
      recv = infer_receiver_var_from_source(prefix, h);
      free(prefix);
    }
  }
  return recv;
}

static int method_owner_leaf_is(const char *name, const char *want) {
  if (!name || !want)
    return 0;
  const char *end = strrchr(name, '.');
  if (!end)
    return 0;
  const char *start = end;
  while (start > name && start[-1] != '.')
    start--;
  size_t got_len = (size_t)(end - start);
  return strlen(want) == got_len && strncmp(start, want, got_len) == 0;
}

static int member_word_in(const char *const *items, const char *word) {
  if (!items || !word)
    return 0;
  for (int i = 0; items[i]; i++) {
    if (strcmp(items[i], word) == 0)
      return 1;
  }
  return 0;
}

static const char *const *receiver_member_words(receiver_kind_t recv) {
  switch (recv) {
  case RECV_LIST:
    return k_list_member_words;
  case RECV_STR:
    return k_str_member_words;
  case RECV_BYTES:
    return k_bytes_member_words;
  case RECV_DICT:
    return k_dict_member_words;
  case RECV_SET:
    return k_set_member_words;
  case RECV_TUPLE:
    return k_tuple_member_words;
  case RECV_RANGE:
    return k_range_member_words;
  case RECV_NUMBER:
    return k_number_member_words;
  case RECV_ANY:
    return k_any_member_words;
  default:
    return NULL;
  }
}

static const char *receiver_owner_name(receiver_kind_t recv) {
  switch (recv) {
  case RECV_LIST:
    return "list";
  case RECV_STR:
    return "str";
  case RECV_BYTES:
    return "bytes";
  case RECV_DICT:
    return "dict";
  case RECV_SET:
    return "set";
  case RECV_TUPLE:
    return "tuple";
  case RECV_RANGE:
    return "range";
  case RECV_BOOL:
    return "bool";
  default:
    return NULL;
  }
}

static int curated_member_word_matches_receiver(receiver_kind_t recv, const char *word) {
  const char *const *items = receiver_member_words(recv);
  if (!items)
    return 1;
  return member_word_in(items, word) || (recv == RECV_NUMBER && repl_starts_with_ci(word, "ascii_"));
}

static int method_matches_receiver(const char *name, receiver_kind_t recv) {
  if (recv == RECV_UNKNOWN)
    return 1;
  if (method_owner_leaf_is(name, "any"))
    return curated_member_word_matches_receiver(recv, last_segment(name));
  const char *owner = receiver_owner_name(recv);
  if (owner)
    return method_owner_leaf_is(name, owner);
  if (recv == RECV_NUMBER) {
    return method_owner_leaf_is(name, "int") || method_owner_leaf_is(name, "f32") ||
           method_owner_leaf_is(name, "f64") || method_owner_leaf_is(name, "float") ||
           method_owner_leaf_is(name, "number") || method_owner_leaf_is(name, "bigint");
  }
  return 1;
}

static void add_receiver_member_words(receiver_kind_t recv, const char *tail) {
  const char *const *items = receiver_member_words(recv);
  add_member_word_completions(items ? items : k_member_words, tail);
}

static void add_member_completions(const char *line, int cursor) {
  char head[256];
  char tail[256];
  split_member_expr(line, cursor, head, sizeof(head), tail, sizeof(tail));
  if (!tail[0] && !head[0])
    return;
  int namespace_like = completion_head_is_namespace(head);
  receiver_kind_t recv = namespace_like ? RECV_UNKNOWN : classify_member_receiver(head);
  if (!namespace_like && recv == RECV_UNKNOWN)
    recv = infer_member_receiver_from_variable(line, cursor, head);
  if (!namespace_like)
    add_receiver_member_words(recv, tail);
  if (g_repl_docs && (namespace_like || tail[0])) {
    repl_ensure_docs_for_query((doc_list_t *)g_repl_docs, namespace_like && head[0] ? head : tail);
    const doc_list_t *d = (const doc_list_t *)g_repl_docs;
    for (size_t i = 0; i < d->len; i++) {
      const char *name = d->data[i].name;
      if (namespace_like && head[0]) {
        size_t head_len = strlen(head);
        if (strncmp(name, head, head_len) != 0 || name[head_len] != '.')
          continue;
      } else if (d->data[i].kind != 5) {
        continue;
      }
      if (!namespace_like && !method_matches_receiver(name, recv))
        continue;
      const char *base = last_segment(name);
      if (!completion_allows_private(tail) && base[0] == '_')
        continue;
      if (!tail[0] || repl_fuzzy_score(base, tail, 1) > 0)
        add_match(base);
    }
  }
  if (!namespace_like)
    return;
  size_t mod_count = ny_std_module_count();
  for (size_t i = 0; i < mod_count; i++) {
    const char *m = ny_std_module_name(i);
    if (!head[0])
      continue;
    size_t head_len = strlen(head);
    if (strncmp(m, head, head_len) != 0 || m[head_len] != '.')
      continue;
    const char *next = m + head_len + 1;
    char seg[256] = {0};
    size_t j = 0;
    while (next[j] && next[j] != '.' && j + 1 < sizeof(seg)) {
      seg[j] = next[j];
      j++;
    }
    seg[j] = '\0';
    if (repl_fuzzy_score(seg, tail, 1) > 0)
      add_match(seg);
  }
}

char *repl_enhanced_completion_generator(const char *text, int state) {
  (void)text;
  (void)state;
  return NULL;
}

char **repl_enhanced_completion(const char *text, int start, int end) {
  (void)text;
  (void)start;
  (void)end;
  return NULL;
}

char **nytrix_get_completions_for_prefix(const char *prefix, size_t *out_count) {
  if (matches) {
    for (int i = 0; i < matches_len; i++)
      free(matches[i]);
    free(matches);
  }
  matches = NULL;
  matches_len = 0;
  matches_cap = 0;
  match_ht_reset();
  int is_cmd_pref = (prefix && prefix[0] == ':');
  int is_empty = (!prefix || !*prefix);
  if (is_cmd_pref || is_empty) {
    for (int i = 0; k_repl_commands[i]; i++) {
      if (is_empty || strncmp(k_repl_commands[i], prefix, strlen(prefix)) == 0)
        add_match(k_repl_commands[i]);
    }
  }
  if (!is_cmd_pref) {
    if (prefix && strchr(prefix, '.')) {
      add_member_completions(prefix, (int)strlen(prefix));
      sort_matches(prefix, CTX_MEMBER);
    } else {
      add_normal_completions(prefix);
      sort_matches(prefix, CTX_NORMAL);
    }
  } else {
    sort_matches(prefix, CTX_COMMAND);
  }
  if (out_count)
    *out_count = (size_t)matches_len;
  char **res = matches;
  matches = NULL;
  matches_len = 0;
  matches_cap = 0;
  return res;
}

char **nytrix_get_completions_for_line(const char *line, int cursor, size_t *out_count) {
  if (matches) {
    for (int i = 0; i < matches_len; i++)
      free(matches[i]);
    free(matches);
  }
  matches = NULL;
  matches_len = 0;
  matches_cap = 0;
  match_ht_reset();
  char prefix[256];
  extract_prefix(line, cursor, prefix, sizeof(prefix));
  compl_ctx_t ctx = get_context(line, cursor);
  int is_cmd = (line && line[0] == ':');
  int wants_files = 0;
  int help_query = 0;
  if (ctx == CTX_STRING)
    wants_files = 1;
  if (ctx == CTX_COMMAND &&
      (command_matches(line, "load") || command_matches(line, "cd") || command_matches(line, "run") ||
       command_matches(line, "save")))
    wants_files = 1;
  if (ctx == CTX_COMMAND &&
      (command_matches(line, "help") || command_matches(line, "h") || command_matches(line, "doc")) &&
      prefix[0] != ':') {
    help_query = 1;
  }
  if ((ctx == CTX_COMMAND || is_cmd) && !wants_files && !help_query) {
    for (int i = 0; k_repl_commands[i]; i++) {
      if (!prefix[0] || strncmp(k_repl_commands[i], prefix, strlen(prefix)) == 0)
        add_match(k_repl_commands[i]);
    }
  } else if (wants_files) {
    add_files(prefix);
  } else if (help_query) {
    char query[256];
    extract_command_arg(line, cursor, query, sizeof(query));
    size_t qlen = strlen(query);
    if (qlen >= 2 && query[qlen - 2] == '.' && query[qlen - 1] == '*') {
      query[qlen - 1] = '\0';
      qlen--;
    }
    if (strchr(query, '.'))
      add_member_completions(query, (int)strlen(query));
    else
      add_normal_completions(query);
  } else if (ctx == CTX_USE) {
    add_use_completions(prefix);
  } else if (ctx == CTX_MEMBER) {
    add_member_completions(line, cursor);
  } else {
    add_normal_completions(prefix);
  }
  sort_matches(prefix, ctx);
  if (out_count)
    *out_count = (size_t)matches_len;
  char **res = matches;
  matches = NULL;
  matches_len = 0;
  matches_cap = 0;
  return res;
}

void nytrix_free_completions(char **completions, size_t count) {
  if (!completions)
    return;
  for (size_t i = 0; i < count; i++)
    free(completions[i]);
  free(completions);
}
