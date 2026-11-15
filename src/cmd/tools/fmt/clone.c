/*
 * clone.c - shared source representation, C/Nytrix tokenization, exact and
 * parameterized clone matching, bounded near-miss alignment, clone families,
 * baselines and reporting for `ny fmt --dupes`.
 *
 * Included by core.c so the formatter remains one translation unit, matching
 * the repository's existing source-inclusion strategy for ny-fmt.
 *
 * The engine is read-only, deterministic and uses only the C runtime.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

#define CLONE_SCHEMA_VERSION 2
#define CLONE_HASH_BASE 1099511628211ULL
#define CLONE_MAX_OCCURRENCES 64
#define CLONE_MAX_MATCHES 60000
#define CLONE_MAX_FAMILIES 30000
#define CLONE_ALIGN_CELLS 4000000
#define CLONE_MAX_ALIGNMENTS 60000
#define CLONE_NEAR_CANDIDATES 48
#define CLONE_MAP_SLOTS 192
#define CLONE_NAME_MAX 96
#define CLONE_RET_MAX 80
#define CLONE_RENAMABLE_KEY 0x9e3779b97f4a7c15ULL
#define CLONE_NUM_KEY 0x243f6a8885a308d3ULL
#define CLONE_STR_KEY 0x13198a2e03707344ULL
#define CLONE_CHR_KEY 0xa4093822299f31d0ULL
#define CLONE_ID_KEY 0x082efa98ec4e6c89ULL

enum {
  CLK_EXACT = 1,
  CLK_RENAMED = 2,
  CLK_LITERAL = 3,
  CLK_NEAR = 4,
  CLK_STRUCTURAL = 5
};

enum {
  CM_EXACT = 1,
  CM_RENAMED = 2,
  CM_LITERAL = 4,
  CM_NEAR = 8,
  CM_STRUCTURAL = 16,
  CM_ALL = 31
};

enum { CLONE_LANG_C = 1, CLONE_LANG_NY = 2 };

enum {
  CT_IDENT = 1,
  CT_KEYWORD,
  CT_NUMBER,
  CT_STRING,
  CT_CHAR,
  CT_PUNCT,
  CT_INVALID
};

enum { CTF_RENAMABLE = 1u << 0, CTF_LITERAL = 1u << 1 };

typedef struct {
  uint32_t kind;
  uint32_t flags;
  uint32_t sym;
  uint32_t start;
  uint32_t end;
  uint32_t line;
  uint32_t col;
  uint32_t lang;
} CTok;

typedef struct {
  CTok *items;
  size_t len;
  size_t cap;
} CTokVec;

typedef struct {
  uint64_t hash;
  uint32_t off;
  uint32_t len;
} CSym;

typedef struct {
  uint32_t *slots; /* id + 1, zero means empty */
  size_t slot_cap;
  CSym *items;
  size_t len;
  size_t cap;
  char *pool;
  size_t pool_len;
  size_t pool_cap;
} CSymTab;

typedef struct {
  char *path;
  char *content;
  size_t len;
  uint32_t content_hash;
  int lang;
  size_t tok_start;
  size_t tok_end;
  int line_count;
  int lex_errors;
  int directives;
  int malformed;
  int parse_status; /* 1 = lexical only, 2 = function-scanned */
  uint8_t *dup_tok;
  uint8_t *dup_line;
} CSource;

typedef struct {
  uint32_t file;
  size_t fn_start, fn_end;   /* declaration span */
  size_t tok_start, tok_end; /* body span */
  int line_start, line_end;
  int is_static;
  int uncertain;
  char name[CLONE_NAME_MAX];
  char ret[CLONE_RET_MAX];
  uint64_t exact_hash;
  uint64_t shape_hash;
} CFrag;

typedef struct {
  int op; /* 0 change, 1 add, 2 remove */
  int line_a;
  int line_b;
  char a[48];
  char b[48];
} CDiff;

typedef struct {
  int kind;
  uint32_t file_a, file_b;
  size_t tok_a_start, tok_a_end, tok_b_start, tok_b_end;
  int line_a_start, line_a_end, line_b_start, line_b_end;
  double similarity;
  uint64_t content_hash;
  int ndiff;
  CDiff diffs[6];
  int moved_candidate;
} CMatch;

typedef struct {
  uint32_t file;
  size_t tok_start, tok_end;
  int line_start, line_end;
  size_t frag; /* fragment index + 1, zero when unknown */
} COcc;

typedef struct {
  int kind;
  uint32_t lang;
  uint64_t stable_id;
  uint64_t content_hash;
  size_t token_count;
  double similarity;
  double confidence;
  COcc *occs;
  size_t nocc;
  size_t occ_cap;
  char structure[96];
  char action[32];
  char recommendation[112];
  char risk[16];
  char semantic[48];
  char diffs[4][176];
  int ndiff;
  char deps[6][48];
  int ndeps;
  int return_compat; /* 0 unknown, 1 same, 2 differ */
  int side_effects;
  int divergence;
  int moved_candidate;
  int occurrences_capped;
  long score;
  long dup_tokens;
  int affected_files;
} CFamily;

typedef struct {
  int files;
  int c_files;
  int ny_files;
  int read_failures;
  int lex_errors;
  int malformed_files;
  int preprocessing_ambiguous;
  int normalization_failures;
  int directives;
  size_t tokens;
  size_t bytes;
  size_t lines;
  size_t functions;
  int exact_families;
  int renamed_families;
  int literal_families;
  int near_families;
  int structural_families;
  int duplicate_functions;
  size_t duplicate_tokens;
  size_t duplicate_lines;
  size_t verified_pairs;
  size_t alignments;
  size_t near_fragments;
  size_t near_candidates;
  int candidate_cap_hits;
  int hash_verification_failures;
  int subsumed;
  int incomplete;
  int baseline_new;
  int baseline_existing;
  int baseline_resolved;
  int baseline_invalid;
} CStats;

typedef struct {
  int modes;
  int lang; /* 0 = all, CLONE_LANG_C, CLONE_LANG_NY */
  size_t min_tokens;
  int min_lines;
  double similarity;
  int cross_lang;
  int legacy_min_bytes;
  int legacy_min_set;
} CConfig;

typedef struct {
  uint64_t a;
  uint64_t b;
} CKeySlot;

typedef struct {
  CKeySlot *slots;
  size_t cap;
  size_t len;
} CKeySet;

typedef struct {
  CConfig cfg;
  CSource *src;
  size_t nsrc;
  size_t src_cap;
  CTokVec toks;
  uint32_t *tok_file;
  size_t tok_file_cap;
  CSymTab syms;
  CFrag *frags;
  size_t nfrags;
  size_t frag_cap;
  uint32_t *frag_of_tok;
  uint64_t *exact_keys;
  uint64_t *shape_keys;
  uint64_t *literal_keys;
  uint64_t *near_keys;
  uint64_t *neutral_keys;
  CMatch *matches;
  size_t nmatches;
  size_t match_cap;
  CKeySet mset;
  CKeySet pairset;
  CFamily *fams;
  size_t nfams;
  size_t fam_cap;
  CStats stats;
  uint16_t *align_dp;
  size_t align_dp_cap;
  uint32_t *align_row0;
  uint32_t *align_row1;
  size_t align_row_cap;
  int verbose;
} CClone;

typedef struct {
  const char *src;
  size_t len;
  size_t pos;
  int line;
  size_t line_start;
  uint32_t lang;
  CSymTab *syms;
  CTokVec *toks;
  int errors;
  int directives;
  int malformed;
} CLex;

static int clone_grow(void **items, size_t *cap, size_t need, size_t elem) {
  if (!items || !cap || elem == 0)
    return 0;
  if (need <= *cap)
    return 1;
  size_t next = *cap ? *cap : 32;
  while (next < need) {
    if (next > SIZE_MAX / 2) {
      next = need;
      break;
    }
    next *= 2;
  }
  if (next > SIZE_MAX / elem)
    return 0;
  void *grown = realloc(*items, next * elem);
  if (!grown)
    return 0;
  *items = grown;
  *cap = next;
  return 1;
}

static uint64_t clone_hash_bytes(const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return h ? h : 1;
}

static uint64_t clone_mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x ? x : 1;
}

static int csym_rehash(CSymTab *t) {
  size_t next = t->slot_cap ? t->slot_cap * 2 : 256;
  if (next < t->slot_cap || next > SIZE_MAX / sizeof(uint32_t))
    return 0;
  uint32_t *slots = (uint32_t *)calloc(next, sizeof(uint32_t));
  if (!slots)
    return 0;
  for (size_t i = 0; i < t->len; ++i) {
    size_t b = (size_t)t->items[i].hash & (next - 1);
    while (slots[b])
      b = (b + 1) & (next - 1);
    slots[b] = (uint32_t)(i + 1);
  }
  free(t->slots);
  t->slots = slots;
  t->slot_cap = next;
  return 1;
}

static int csym_intern(CSymTab *t, const char *text, size_t len,
                       uint32_t *out_id) {
  if (!t || !out_id || (!text && len))
    return 0;
  uint64_t hash = clone_hash_bytes(text, len);
  if (t->slot_cap == 0 && !csym_rehash(t))
    return 0;
  if ((t->len + 1) * 4 >= t->slot_cap * 3 && !csym_rehash(t))
    return 0;
  size_t b = (size_t)hash & (t->slot_cap - 1);
  while (t->slots[b]) {
    CSym *s = &t->items[t->slots[b] - 1];
    if (s->hash == hash && s->len == len &&
        (len == 0 || memcmp(t->pool + s->off, text, len) == 0)) {
      *out_id = t->slots[b] - 1;
      return 1;
    }
    b = (b + 1) & (t->slot_cap - 1);
  }
  if (!clone_grow((void **)&t->items, &t->cap, t->len + 1,
                  sizeof(*t->items)))
    return 0;
  size_t need = t->pool_len + len + 1;
  if (need < t->pool_len || !clone_grow((void **)&t->pool, &t->pool_cap, need,
                                         sizeof(char)))
    return 0;
  uint32_t id = (uint32_t)t->len;
  memcpy(t->pool + t->pool_len, text ? text : "", len);
  t->pool[t->pool_len + len] = '\0';
  t->items[t->len].hash = hash;
  t->items[t->len].off = (uint32_t)t->pool_len;
  t->items[t->len].len = (uint32_t)len;
  t->pool_len = need;
  t->len++;
  t->slots[b] = id + 1;
  *out_id = id;
  return 1;
}

static const char *csym_text(const CSymTab *t, uint32_t id, size_t *len_out) {
  if (!t || id >= t->len) {
    if (len_out)
      *len_out = 0;
    return "";
  }
  if (len_out)
    *len_out = t->items[id].len;
  return t->pool + t->items[id].off;
}

static int csym_is(const CSymTab *t, uint32_t id, const char *word) {
  size_t n = 0;
  const char *p = csym_text(t, id, &n);
  size_t w = strlen(word);
  return n == w && memcmp(p, word, w) == 0;
}

static uint64_t csym_hash(const CSymTab *t, uint32_t id) {
  return (t && id < t->len) ? t->items[id].hash : 1;
}

static int clone_tok_push(CLex *lx, uint32_t kind, uint32_t flags, size_t start,
                          size_t end, int line, int col) {
  uint32_t sym = 0;
  if (end < start || end > lx->len)
    return 0;
  if (!csym_intern(lx->syms, lx->src + start, end - start, &sym))
    return 0;
  if (!clone_grow((void **)&lx->toks->items, &lx->toks->cap,
                  lx->toks->len + 1, sizeof(*lx->toks->items)))
    return 0;
  CTok *tok = &lx->toks->items[lx->toks->len++];
  tok->kind = kind;
  tok->flags = flags;
  tok->sym = sym;
  tok->start = (uint32_t)start;
  tok->end = (uint32_t)end;
  tok->line = (uint32_t)line;
  tok->col = (uint32_t)(col > 0 ? col : 1);
  tok->lang = lx->lang;
  return 1;
}

static size_t clone_c_punct_len(const char *src, size_t n) {
  static const char *const three[] = {"...", "<<=", ">>=", NULL};
  static const char *const two[] = {
      "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
      "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", NULL};
  for (size_t i = 0; three[i]; ++i)
    if (n >= 3 && memcmp(src, three[i], 3) == 0)
      return 3;
  for (size_t i = 0; two[i]; ++i)
    if (n >= 2 && memcmp(src, two[i], 2) == 0)
      return 2;
  return 1;
}

static size_t clone_ny_punct_len(const char *src, size_t n) {
  static const char *const three[] = {"...", "<<=", ">>=", "^^=", "**=", NULL};
  static const char *const two[] = {
      "..", "??", "?.", "**", "^^", "<<", ">>", "<=", ">=", "==", "!=",
      "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "++",
      "--", "->", "=>", "::", NULL};
  for (size_t i = 0; three[i]; ++i)
    if (n >= 3 && memcmp(src, three[i], 3) == 0)
      return 3;
  for (size_t i = 0; two[i]; ++i)
    if (n >= 2 && memcmp(src, two[i], 2) == 0)
      return 2;
  return 1;
}

static int clone_c_keyword(const char *s, size_t n) {
  static const char *const words[] = {
      "auto",    "break",   "case",     "char",     "const",   "continue",
      "default", "do",      "double",   "else",     "enum",    "extern",
      "float",   "for",     "goto",     "if",       "inline",  "int",
      "long",    "register", "restrict", "return",  "short",   "signed",
      "sizeof",  "static",  "struct",   "switch",   "typedef", "union",
      "unsigned", "void",   "volatile", "while",    "_Alignas", "_Alignof",
      "_Atomic", "_Bool",   "_Complex", "_Generic", "_Imaginary",
      "_Noreturn", "_Static_assert", "_Thread_local", NULL};
  for (size_t i = 0; words[i]; ++i) {
    size_t w = strlen(words[i]);
    if (n == w && memcmp(s, words[i], w) == 0)
      return 1;
  }
  return 0;
}

static int clone_ny_keyword(const char *s, size_t n) {
  static const char *const words[] = {
      "fn",      "return", "if",       "else",    "elif",   "while",
      "for",     "in",     "true",     "false",   "try",    "catch",
      "use",     "goto",   "lambda",   "defer",   "break",  "continue",
      "asm",     "as",     "comptime", "struct",  "match",  "embed",
      "sizeof",  "extern", "def",      "mut",     "nil",    "none",
      "del",     "module", "enum",     "case",    "layout", "quote",
      "NULL",    NULL};
  for (size_t i = 0; words[i]; ++i) {
    size_t w = strlen(words[i]);
    if (n == w && memcmp(s, words[i], w) == 0)
      return 1;
  }
  return 0;
}

static size_t clone_c_string_prefix(const char *s, size_t n) {
  if (n >= 3 && s[0] == 'u' && s[1] == '8' && (s[2] == '"' || s[2] == '\''))
    return 2;
  if (n >= 2 && (s[0] == 'u' || s[0] == 'U' || s[0] == 'L') &&
      (s[1] == '"' || s[1] == '\''))
    return 1;
  return 0;
}

static void clone_line_add(CLex *lx, size_t from, size_t to) {
  for (size_t i = from; i < to; ++i)
    if (lx->src[i] == '\n') {
      lx->line++;
      lx->line_start = i + 1;
    }
}

static int clone_lex_c(const char *src, size_t len, CSymTab *syms,
                       CTokVec *toks, uint32_t lang, int *errors,
                       int *directives, int *malformed) {
  CLex lx;
  memset(&lx, 0, sizeof(lx));
  lx.src = src;
  lx.len = len;
  lx.line = 1;
  lx.line_start = 0;
  lx.lang = lang;
  lx.syms = syms;
  lx.toks = toks;
  size_t i = 0;
  int bol = 1;
  while (i < len) {
    unsigned char ch = (unsigned char)src[i];
    if (ch == '\\' && i + 1 < len && src[i + 1] == '\n') {
      i += 2;
      lx.line++;
      lx.line_start = i;
      bol = 1;
      continue;
    }
    if (ch == '\n') {
      i++;
      lx.line++;
      lx.line_start = i;
      bol = 1;
      continue;
    }
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') {
      i++;
      continue;
    }
    if (ch == '/' && i + 1 < len && src[i + 1] == '/') {
      while (i < len && src[i] != '\n')
        i++;
      bol = 0;
      continue;
    }
    if (ch == '/' && i + 1 < len && src[i + 1] == '*') {
      size_t start = i;
      i += 2;
      while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/'))
        i++;
      if (i + 1 >= len) {
        lx.malformed = 1;
        lx.errors++;
        break;
      }
      i += 2;
      clone_line_add(&lx, start, i);
      bol = 0;
      continue;
    }
    if (ch == '#' && bol) {
      while (i < len && src[i] != '\n') {
        if (src[i] == '\\' && i + 1 < len && src[i + 1] == '\n') {
          i += 2;
          lx.line++;
          lx.line_start = i;
        } else {
          i++;
        }
      }
      lx.directives++;
      bol = 1;
      continue;
    }

    size_t start = i;
    int line = lx.line;
    int col = (int)(i - lx.line_start + 1);
    size_t prefix = clone_c_string_prefix(src + i, len - i);
    if (prefix || ch == '"' || ch == '\'') {
      size_t q = i + prefix;
      char quote = src[q];
      i = q + 1;
      int closed = 0;
      while (i < len) {
        if (src[i] == '\\' && i + 1 < len) {
          if (src[i + 1] == '\n') {
            lx.line++;
            lx.line_start = i + 2;
          }
          i += 2;
          continue;
        }
        if (src[i] == quote) {
          i++;
          closed = 1;
          break;
        }
        if (src[i] == '\n') {
          lx.malformed = 1;
          lx.errors++;
          break;
        }
        i++;
      }
      if (!closed)
        lx.errors++;
      if (!clone_tok_push(&lx, quote == '"' ? CT_STRING : CT_CHAR, CTF_LITERAL,
                          start, i, line, col))
        return 0;
      bol = 0;
      continue;
    }
    if (isalpha(ch) || ch == '_' || ch >= 0x80) {
      i++;
      while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '_' || c >= 0x80)
          i++;
        else
          break;
      }
      int kind = clone_c_keyword(src + start, i - start) ? CT_KEYWORD : CT_IDENT;
      if (!clone_tok_push(&lx, (uint32_t)kind, 0, start, i, line, col))
        return 0;
      bol = 0;
      continue;
    }
    if (isdigit(ch) || (ch == '.' && i + 1 < len && isdigit((unsigned char)src[i + 1]))) {
      i++;
      while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '_' || c == '.' || c == '\'') {
          i++;
          continue;
        }
        if ((c == '+' || c == '-') && i > start &&
            (src[i - 1] == 'e' || src[i - 1] == 'E' || src[i - 1] == 'p' ||
             src[i - 1] == 'P')) {
          i++;
          continue;
        }
        break;
      }
      if (!clone_tok_push(&lx, CT_NUMBER, CTF_LITERAL, start, i, line, col))
        return 0;
      bol = 0;
      continue;
    }
    size_t plen = clone_c_punct_len(src + i, len - i);
    i += plen;
    if (!clone_tok_push(&lx, CT_PUNCT, 0, start, i, line, col))
      return 0;
    bol = 0;
  }
  if (errors)
    *errors += lx.errors;
  if (directives)
    *directives += lx.directives;
  if (malformed)
    *malformed |= lx.malformed;
  return 1;
}

static int clone_ny_comment(CLex *lx, size_t *pos) {
  size_t i = *pos;
  size_t start = i;
  i++; /* ';' */
  if (i >= lx->len) {
    *pos = i;
    return 1;
  }
  char opener = lx->src[i];
  if (opener == '{' || opener == '<' || opener == '[' || opener == '(') {
    char closer = opener == '{' ? '}' : opener == '<' ? '>' : opener == '[' ? ']' : ')';
    i++;
    int found = 0;
    while (i < lx->len) {
      if (lx->src[i] == closer) {
        size_t k = i + 1;
        while (k < lx->len &&
               (lx->src[k] == ' ' || lx->src[k] == '\t' || lx->src[k] == '\r'))
          k++;
        if (k < lx->len && lx->src[k] == ';') {
          i = k + 1;
          found = 1;
          break;
        }
      }
      i++;
    }
    if (!found) {
      lx->errors++;
      lx->malformed = 1;
      i = lx->len;
    }
  } else if (isalpha((unsigned char)opener) || opener == '_') {
    size_t ms = i;
    while (i < lx->len &&
           (isalnum((unsigned char)lx->src[i]) || lx->src[i] == '_'))
      i++;
    size_t ml = i - ms;
    int found = 0;
    while (i < lx->len) {
      if (lx->src[i] == lx->src[ms] && i + ml <= lx->len &&
          memcmp(lx->src + i, lx->src + ms, ml) == 0) {
        char prev = i > 0 ? lx->src[i - 1] : '\0';
        size_t k = i + ml;
        while (k < lx->len &&
               (lx->src[k] == ' ' || lx->src[k] == '\t' || lx->src[k] == '\r'))
          k++;
        if (!(isalnum((unsigned char)prev) || prev == '_') && k < lx->len &&
            lx->src[k] == ';') {
          i = k + 1;
          found = 1;
          break;
        }
      }
      i++;
    }
    if (!found) {
      lx->errors++;
      lx->malformed = 1;
      i = lx->len;
    }
  } else {
    while (i < lx->len && lx->src[i] != '\n')
      i++;
  }
  clone_line_add(lx, start, i);
  *pos = i;
  return 1;
}

static int clone_ny_quoted(CLex *lx, size_t *pos, int triple, char quote,
                           int formatted) {
  size_t i = *pos;
  int depth = 0;
  int closed = 0;
  while (i < lx->len) {
    char c = lx->src[i];
    if (depth > 0) {
      if (c == '{') {
        depth++;
        i++;
      } else if (c == '}') {
        depth--;
        i++;
      } else if (c == '"' || c == '\'') {
        char inner = c;
        i++;
        while (i < lx->len) {
          if (lx->src[i] == '\\' && i + 1 < lx->len) {
            i += 2;
            continue;
          }
          if (lx->src[i] == inner) {
            i++;
            break;
          }
          if (lx->src[i] == '\n')
            break;
          i++;
        }
      } else {
        if (c == '\n') {
          lx->line++;
          lx->line_start = i + 1;
        }
        i++;
      }
      continue;
    }
    if (c == '\\' && i + 1 < lx->len) {
      if (lx->src[i + 1] == '\n') {
        lx->line++;
        lx->line_start = i + 2;
      }
      i += 2;
      continue;
    }
    if (triple) {
      if (i + 2 < lx->len && lx->src[i] == quote &&
          lx->src[i + 1] == quote && lx->src[i + 2] == quote) {
        i += 3;
        closed = 1;
        break;
      }
    } else if (c == quote) {
      i++;
      closed = 1;
      break;
    }
    if (formatted && c == '$' && i + 1 < lx->len && lx->src[i + 1] == '{') {
      depth = 1;
      i += 2;
      continue;
    }
    if (c == '\n') {
      lx->line++;
      lx->line_start = i + 1;
    }
    i++;
  }
  if (!closed) {
    lx->errors++;
    lx->malformed = 1;
  }
  *pos = i;
  return 1;
}

static int clone_lex_ny(const char *src, size_t len, CSymTab *syms,
                        CTokVec *toks, uint32_t lang, int *errors,
                        int *directives, int *malformed) {
  CLex lx;
  memset(&lx, 0, sizeof(lx));
  lx.src = src;
  lx.len = len;
  lx.line = 1;
  lx.line_start = 0;
  lx.lang = lang;
  lx.syms = syms;
  lx.toks = toks;
  size_t i = 0;
  int bol = 1;
  while (i < len) {
    unsigned char ch = (unsigned char)src[i];
    if (ch == '\n') {
      i++;
      lx.line++;
      lx.line_start = i;
      bol = 1;
      continue;
    }
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') {
      i++;
      continue;
    }
    if (ch == ';') {
      clone_ny_comment(&lx, &i);
      bol = 0;
      continue;
    }
    if (ch == '#' && bol) {
      size_t start = i;
      while (i < len && src[i] != '\n')
        i++;
      lx.directives++;
      clone_line_add(&lx, start, i);
      bol = 1;
      continue;
    }

    size_t start = i;
    int line = lx.line;
    int col = (int)(i - lx.line_start + 1);
    if (ch == 'f' && i + 1 < len &&
        (src[i + 1] == '"' || src[i + 1] == '\'')) {
      char quote = src[i + 1];
      int triple = i + 3 < len && src[i + 2] == quote && src[i + 3] == quote;
      i += triple ? 4 : 2;
      clone_ny_quoted(&lx, &i, triple, quote, 1);
      if (!clone_tok_push(&lx, CT_STRING, CTF_LITERAL, start, i, line, col))
        return 0;
      bol = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      char quote = (char)ch;
      int triple = i + 2 < len && src[i + 1] == quote && src[i + 2] == quote;
      i += triple ? 3 : 1;
      clone_ny_quoted(&lx, &i, triple, quote, 0);
      if (!clone_tok_push(&lx, CT_STRING, CTF_LITERAL, start, i, line, col))
        return 0;
      bol = 0;
      continue;
    }
    if (isalpha(ch) || ch == '_' || ch >= 0x80) {
      i++;
      while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '_' || c >= 0x80)
          i++;
        else
          break;
      }
      int kind = clone_ny_keyword(src + start, i - start) ? CT_KEYWORD : CT_IDENT;
      if (!clone_tok_push(&lx, (uint32_t)kind, 0, start, i, line, col))
        return 0;
      bol = 0;
      continue;
    }
    if (isdigit(ch) || (ch == '.' && i + 1 < len &&
                        isdigit((unsigned char)src[i + 1]))) {
      i++;
      while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '_' || c == '.') {
          i++;
          continue;
        }
        if ((c == '+' || c == '-') && i > start &&
            (src[i - 1] == 'e' || src[i - 1] == 'E'))
          i++;
        else
          break;
      }
      if (!clone_tok_push(&lx, CT_NUMBER, CTF_LITERAL, start, i, line, col))
        return 0;
      bol = 0;
      continue;
    }
    if ((unsigned char)ch == 0xe2 && i + 2 < len &&
        (unsigned char)src[i + 1] == 0x86 && (unsigned char)src[i + 2] == 0x92) {
      i += 3;
      if (!clone_tok_push(&lx, CT_PUNCT, 0, start, i, line, col))
        return 0;
      bol = 0;
      continue;
    }
    size_t plen = clone_ny_punct_len(src + i, len - i);
    i += plen;
    if (!clone_tok_push(&lx, CT_PUNCT, 0, start, i, line, col))
      return 0;
    bol = 0;
  }
  if (errors)
    *errors += lx.errors;
  if (directives)
    *directives += lx.directives;
  if (malformed)
    *malformed |= lx.malformed;
  return 1;
}

static int clone_punct_is(const CSymTab *syms, const CTok *tok, const char *p) {
  return tok->kind == CT_PUNCT && csym_is(syms, tok->sym, p);
}

static void clone_mark_flags(CSymTab *syms, CTok *toks, size_t start,
                             size_t end) {
  for (size_t i = start; i < end; ++i) {
    CTok *tok = &toks[i];
    if (tok->kind == CT_NUMBER || tok->kind == CT_STRING ||
        tok->kind == CT_CHAR) {
      tok->flags |= CTF_LITERAL;
      continue;
    }
    if (tok->kind != CT_IDENT)
      continue;
    int member = 0;
    if (i > start) {
      const CTok *prev = &toks[i - 1];
      if (clone_punct_is(syms, prev, ".") ||
          clone_punct_is(syms, prev, "->"))
        member = 1;
    }
    int call = 0;
    if (i + 1 < end) {
      const CTok *next = &toks[i + 1];
      if (clone_punct_is(syms, next, "("))
        call = 1;
    }
    size_t n = 0;
    const char *text = csym_text(syms, tok->sym, &n);
    int reserved = n >= 2 && text[0] == '_' && text[1] == '_';
    if (!member && !call && !reserved)
      tok->flags |= CTF_RENAMABLE;
  }
}

enum { KP_EXACT, KP_SHAPE, KP_LITERAL, KP_NEAR, KP_NEUTRAL };

static uint64_t clone_tok_text_hash(const CClone *c, const CTok *tok) {
  return csym_hash(&c->syms, tok->sym);
}

static uint64_t clone_exact_key(const CClone *c, const CTok *tok) {
  return clone_mix((uint64_t)tok->kind * 0x9e3779b97f4a7c15ULL ^
                   clone_tok_text_hash(c, tok));
}

static uint64_t clone_literal_class_key(uint32_t kind) {
  if (kind == CT_NUMBER)
    return CLONE_NUM_KEY;
  if (kind == CT_STRING)
    return CLONE_STR_KEY;
  if (kind == CT_CHAR)
    return CLONE_CHR_KEY;
  return 0;
}

static uint64_t clone_shape_key(const CClone *c, const CTok *tok) {
  if (tok->flags & CTF_RENAMABLE)
    return CLONE_RENAMABLE_KEY;
  return clone_exact_key(c, tok);
}

static uint64_t clone_literal_key(const CClone *c, const CTok *tok) {
  uint64_t lit = clone_literal_class_key(tok->kind);
  if (lit)
    return lit;
  return clone_exact_key(c, tok);
}

static uint64_t clone_near_key(const CClone *c, const CTok *tok) {
  if (tok->flags & CTF_RENAMABLE)
    return CLONE_RENAMABLE_KEY;
  uint64_t lit = clone_literal_class_key(tok->kind);
  if (lit)
    return lit;
  return clone_exact_key(c, tok);
}

static uint64_t clone_keyword_class(const CClone *c, const CTok *tok) {
  size_t n = 0;
  const char *s = csym_text(&c->syms, tok->sym, &n);
  (void)n;
  if (strcmp(s, "if") == 0 || strcmp(s, "else") == 0 ||
      strcmp(s, "elif") == 0)
    return clone_mix(0x1100000000000001ULL);
  if (strcmp(s, "for") == 0 || strcmp(s, "while") == 0 ||
      strcmp(s, "do") == 0)
    return clone_mix(0x1100000000000002ULL);
  if (strcmp(s, "return") == 0)
    return clone_mix(0x1100000000000003ULL);
  if (strcmp(s, "break") == 0 || strcmp(s, "continue") == 0)
    return clone_mix(0x1100000000000004ULL);
  if (strcmp(s, "switch") == 0 || strcmp(s, "match") == 0 ||
      strcmp(s, "case") == 0)
    return clone_mix(0x1100000000000005ULL);
  if (strcmp(s, "defer") == 0)
    return clone_mix(0x1100000000000006ULL);
  if (strcmp(s, "try") == 0 || strcmp(s, "catch") == 0)
    return clone_mix(0x1100000000000007ULL);
  if (strcmp(s, "use") == 0 || strcmp(s, "include") == 0 ||
      strcmp(s, "import") == 0)
    return clone_mix(0x1100000000000008ULL);
  if (strcmp(s, "struct") == 0 || strcmp(s, "enum") == 0 ||
      strcmp(s, "union") == 0 || strcmp(s, "layout") == 0 ||
      strcmp(s, "typedef") == 0)
    return clone_mix(0x1100000000000009ULL);
  if (strcmp(s, "fn") == 0)
    return clone_mix(0x110000000000000aULL);
  return clone_mix(0x11000000000000ffULL ^ csym_hash(&c->syms, tok->sym));
}

static uint64_t clone_neutral_key(const CClone *c, const CTok *tok) {
  if (tok->kind == CT_IDENT)
    return CLONE_ID_KEY;
  if (tok->kind == CT_KEYWORD)
    return clone_keyword_class(c, tok);
  uint64_t lit = clone_literal_class_key(tok->kind);
  if (lit)
    return lit;
  if (tok->kind == CT_PUNCT)
    return clone_mix(0x2200000000000000ULL ^ clone_tok_text_hash(c, tok));
  return clone_mix(0x3300000000000000ULL ^ (uint64_t)tok->kind);
}

static int clone_build_keys(CClone *c, int profile, uint64_t **out) {
  if (!c || !out)
    return 0;
  *out = NULL;
  if (c->toks.len == 0)
    return 1;
  if (c->toks.len > SIZE_MAX / sizeof(uint64_t))
    return 0;
  uint64_t *keys = (uint64_t *)malloc(c->toks.len * sizeof(uint64_t));
  if (!keys)
    return 0;
  for (size_t i = 0; i < c->toks.len; ++i) {
    const CTok *tok = &c->toks.items[i];
    switch (profile) {
    case KP_SHAPE:
      keys[i] = clone_shape_key(c, tok);
      break;
    case KP_LITERAL:
      keys[i] = clone_literal_key(c, tok);
      break;
    case KP_NEAR:
      keys[i] = clone_near_key(c, tok);
      break;
    case KP_NEUTRAL:
      keys[i] = clone_neutral_key(c, tok);
      break;
    default:
      keys[i] = clone_exact_key(c, tok);
      break;
    }
  }
  *out = keys;
  return 1;
}

static int clone_ensure_keys(CClone *c, uint64_t **slot, int profile) {
  if (*slot)
    return 1;
  return clone_build_keys(c, profile, slot);
}

static int clone_lang_for_path(const char *path) {
  if (nyt_ends_with(path, ".c") || nyt_ends_with(path, ".h"))
    return CLONE_LANG_C;
  if (nyt_ends_with(path, ".ny"))
    return CLONE_LANG_NY;
  return 0;
}

static int clone_add_source(CClone *c, const char *path, int lang) {
  size_t len = 0;
  char *content = ny_read_file_raw(path, &len);
  if (!content) {
    c->stats.read_failures++;
    if (!c->cfg.legacy_min_set)
      ; /* keep the branch explicit: read failures are reported below */
    return 1;
  }
  if (!clone_grow((void **)&c->src, &c->src_cap, c->nsrc + 1,
                  sizeof(*c->src))) {
    free(content);
    return 0;
  }
  uint32_t file_id = (uint32_t)c->nsrc;
  CSource *s = &c->src[c->nsrc++];
  memset(s, 0, sizeof(*s));
  size_t path_n = strlen(path);
  s->path = (char *)malloc(path_n + 1);
  if (!s->path) {
    free(content);
    return 0;
  }
  memcpy(s->path, path, path_n + 1);
  s->content = content;
  s->len = len;
  s->lang = lang;
  s->content_hash = clone_hash_bytes(content, len);
  for (size_t i = 0; i < len; ++i)
    if (content[i] == '\n')
      s->line_count++;
  if (len && content[len - 1] != '\n')
    s->line_count++;
  s->tok_start = c->toks.len;
  int lex_errors = 0;
  int directives = 0;
  int malformed = 0;
  int ok = lang == CLONE_LANG_NY
               ? clone_lex_ny(content, len, &c->syms, &c->toks,
                              (uint32_t)CLONE_LANG_NY, &lex_errors, &directives,
                              &malformed)
               : clone_lex_c(content, len, &c->syms, &c->toks,
                             (uint32_t)CLONE_LANG_C, &lex_errors, &directives,
                             &malformed);
  if (!ok)
    return 0;
  s->tok_end = c->toks.len;
  s->lex_errors = lex_errors;
  s->directives = directives;
  s->malformed = malformed;
  s->parse_status = 1;
  if (!clone_grow((void **)&c->tok_file, &c->tok_file_cap, c->toks.len,
                  sizeof(uint32_t)))
    return 0;
  for (size_t i = s->tok_start; i < s->tok_end; ++i)
    c->tok_file[i] = file_id;
  clone_mark_flags(&c->syms, c->toks.items, s->tok_start, s->tok_end);

  c->stats.files++;
  if (lang == CLONE_LANG_C)
    c->stats.c_files++;
  else
    c->stats.ny_files++;
  c->stats.tokens += s->tok_end - s->tok_start;
  c->stats.bytes += len;
  c->stats.lines += (size_t)s->line_count;
  c->stats.lex_errors += lex_errors;
  c->stats.directives += directives;
  if (malformed)
    c->stats.malformed_files++;
  return 1;
}

static size_t clone_token_at_or_after(const CClone *c, const CSource *s,
                                      size_t offset) {
  size_t lo = s->tok_start, hi = s->tok_end;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c->toks.items[mid].start < offset)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

static void clone_copy_collapsed(const char *src, size_t start, size_t end,
                                 char *out, size_t cap) {
  if (!out || cap == 0)
    return;
  out[0] = '\0';
  size_t w = 0;
  int space = 0;
  for (size_t i = start; i < end && src[i]; ++i) {
    unsigned char ch = (unsigned char)src[i];
    if (isspace(ch)) {
      space = w > 0;
      continue;
    }
    if (space && w + 1 < cap)
      out[w++] = ' ';
    space = 0;
    if (w + 1 < cap)
      out[w++] = (char)ch;
    else
      break;
  }
  out[w] = '\0';
}

static int clone_frag_push(CClone *c, uint32_t file, size_t fn_start,
                           size_t fn_end, size_t tok_start, size_t tok_end,
                           const char *name, const char *ret, int is_static,
                           int uncertain, int line_start, int line_end) {
  if (!clone_grow((void **)&c->frags, &c->frag_cap, c->nfrags + 1,
                  sizeof(*c->frags)))
    return 0;
  CFrag *f = &c->frags[c->nfrags++];
  memset(f, 0, sizeof(*f));
  f->file = file;
  f->fn_start = fn_start;
  f->fn_end = fn_end;
  f->tok_start = tok_start;
  f->tok_end = tok_end;
  f->is_static = is_static;
  f->uncertain = uncertain;
  f->line_start = line_start;
  f->line_end = line_end;
  if (name)
    snprintf(f->name, sizeof(f->name), "%s", name);
  if (ret)
    snprintf(f->ret, sizeof(f->ret), "%s", ret);
  return 1;
}

static int clone_extract_c_fragments(CClone *c, uint32_t file) {
  CSource *s = &c->src[file];
  CScanFunctions funcs;
  memset(&funcs, 0, sizeof(funcs));
  if (!cscan_functions(s->content, s->len, &funcs))
    return 0;
  s->parse_status = 2;
  if (funcs.malformed) {
    s->malformed = 1;
    c->stats.malformed_files++;
  }
  if (funcs.preprocessing_ambiguous)
    c->stats.preprocessing_ambiguous++;
  c->stats.functions += funcs.len;
  for (size_t i = 0; i < funcs.len; ++i) {
    const CScanFunction *fn = &funcs.items[i];
    size_t full = clone_token_at_or_after(c, s, fn->start_offset);
    size_t open = clone_token_at_or_after(c, s, fn->body_open_offset);
    size_t close = clone_token_at_or_after(c, s, fn->body_close_offset);
    if (open >= s->tok_end || close >= s->tok_end || close < open)
      continue;
    if (!clone_punct_is(&c->syms, &c->toks.items[open], "{") ||
        !clone_punct_is(&c->syms, &c->toks.items[close], "}"))
      continue;
    if (full >= s->tok_end)
      full = open;
    char ret[CLONE_RET_MAX];
    clone_copy_collapsed(s->content, fn->start_offset, fn->name_offset, ret,
                         sizeof(ret));
    if (!clone_frag_push(c, file, full, close + 1, open + 1, close, fn->name,
                         ret, fn->is_static, fn->uncertain, fn->start_line,
                         fn->end_line)) {
      cscan_functions_free(&funcs);
      return 0;
    }
  }
  cscan_functions_free(&funcs);
  return 1;
}

static int clone_skip_balanced(CClone *c, size_t open, size_t end,
                               const char *open_text, const char *close_text,
                               size_t *out) {
  if (open >= end || !clone_punct_is(&c->syms, &c->toks.items[open], open_text))
    return 0;
  int depth = 0;
  for (size_t i = open; i < end; ++i) {
    if (clone_punct_is(&c->syms, &c->toks.items[i], open_text)) {
      depth++;
    } else if (clone_punct_is(&c->syms, &c->toks.items[i], close_text)) {
      depth--;
      if (depth == 0) {
        if (out)
          *out = i + 1;
        return 1;
      }
    }
  }
  return 0;
}

static int clone_ny_parse_fn(CClone *c, uint32_t file, size_t fn_idx,
                             const char *name_override, char *name_out,
                             size_t name_cap, size_t *body_start,
                             size_t *body_end, char *ret, size_t ret_cap) {
  CSource *s = &c->src[file];
  size_t end = s->tok_end;
  size_t j = fn_idx + 1;
  char name[CLONE_NAME_MAX];
  name[0] = '\0';
  if (name_override) {
    snprintf(name, sizeof(name), "%s", name_override);
  } else {
    if (j < end && clone_punct_is(&c->syms, &c->toks.items[j], "[")) {
      size_t after = 0;
      if (!clone_skip_balanced(c, j, end, "[", "]", &after))
        return 0;
      j = after;
    }
    if (j >= end || c->toks.items[j].kind != CT_IDENT)
      return 0;
    size_t n = 0;
    const char *t = csym_text(&c->syms, c->toks.items[j].sym, &n);
    if (n >= sizeof(name))
      n = sizeof(name) - 1;
    memcpy(name, t, n);
    name[n] = '\0';
    j++;
    while (j + 1 < end && clone_punct_is(&c->syms, &c->toks.items[j], ".") &&
           c->toks.items[j + 1].kind == CT_IDENT) {
      t = csym_text(&c->syms, c->toks.items[j + 1].sym, &n);
      if (n >= sizeof(name))
        n = sizeof(name) - 1;
      memcpy(name, t, n);
      name[n] = '\0';
      j += 2;
    }
  }
  if (j < end && clone_punct_is(&c->syms, &c->toks.items[j], "[")) {
    size_t after = 0;
    if (!clone_skip_balanced(c, j, end, "[", "]", &after))
      return 0;
    j = after;
  }
  if (j >= end || !clone_punct_is(&c->syms, &c->toks.items[j], "("))
    return 0;
  size_t after_params = 0;
  if (!clone_skip_balanced(c, j, end, "(", ")", &after_params))
    return 0;
  size_t ret_start = after_params;
  j = after_params;
  while (j < end) {
    if (clone_punct_is(&c->syms, &c->toks.items[j], "{") ||
        clone_punct_is(&c->syms, &c->toks.items[j], "="))
      break;
    if (clone_punct_is(&c->syms, &c->toks.items[j], "(") ||
        clone_punct_is(&c->syms, &c->toks.items[j], "[")) {
      const char *op = clone_punct_is(&c->syms, &c->toks.items[j], "(")
                           ? "("
                           : "[";
      const char *cl = op[0] == '(' ? ")" : "]";
      size_t after = 0;
      if (!clone_skip_balanced(c, j, end, op, cl, &after))
        return 0;
      j = after;
      continue;
    }
    if (clone_punct_is(&c->syms, &c->toks.items[j], "}"))
      return 0;
    j++;
  }
  if (j >= end)
    return 0;
  if (ret) {
    ret[0] = '\0';
    if (ret_start < j)
      clone_copy_collapsed(s->content, c->toks.items[ret_start].start,
                           c->toks.items[j].start, ret, ret_cap);
  }
  if (clone_punct_is(&c->syms, &c->toks.items[j], "{")) {
    size_t after = 0;
    if (!clone_skip_balanced(c, j, end, "{", "}", &after))
      return 0;
    *body_start = j + 1;
    *body_end = after - 1;
  } else {
    size_t line = c->toks.items[j].line;
    int base_col = (int)c->toks.items[fn_idx].col;
    size_t e = j + 1;
    while (e < end && (c->toks.items[e].line == line ||
                       (int)c->toks.items[e].col > base_col))
      e++;
    *body_start = j + 1;
    *body_end = e;
  }
  if (*body_end < *body_start)
    *body_end = *body_start;
  if (name_out && name_cap)
    snprintf(name_out, name_cap, "%s", name);
  return 1;
}

static int clone_extract_ny_fragments(CClone *c, uint32_t file) {
  CSource *s = &c->src[file];
  size_t end = s->tok_end;
  for (size_t i = s->tok_start; i < end; ++i) {
    CTok *tok = &c->toks.items[i];
    if (tok->kind == CT_KEYWORD && csym_is(&c->syms, tok->sym, "fn")) {
      char name[CLONE_NAME_MAX];
      char ret[CLONE_RET_MAX];
      size_t body_s = 0, body_e = 0;
      if (!clone_ny_parse_fn(c, file, i, NULL, name, sizeof(name), &body_s,
                             &body_e, ret, sizeof(ret)))
        continue;
      c->stats.functions++;
      if (!clone_frag_push(c, file, i, body_e, body_s, body_e, name, ret, 0, 0,
                           (int)tok->line, (int)tok->line))
        return 0;
    } else if (tok->kind == CT_KEYWORD &&
               csym_is(&c->syms, tok->sym, "def") && i + 4 < end &&
               c->toks.items[i + 1].kind == CT_IDENT &&
               clone_punct_is(&c->syms, &c->toks.items[i + 2], "=") &&
               c->toks.items[i + 3].kind == CT_KEYWORD &&
               csym_is(&c->syms, c->toks.items[i + 3].sym, "fn")) {
      size_t n = 0;
      const char *t = csym_text(&c->syms, c->toks.items[i + 1].sym, &n);
      char name[CLONE_NAME_MAX];
      if (n >= sizeof(name))
        n = sizeof(name) - 1;
      memcpy(name, t, n);
      name[n] = '\0';
      char ret[CLONE_RET_MAX];
      size_t body_s = 0, body_e = 0;
      if (!clone_ny_parse_fn(c, file, i + 3, name, NULL, 0, &body_s, &body_e,
                             ret, sizeof(ret)))
        continue;
      c->stats.functions++;
      if (!clone_frag_push(c, file, i, body_e, body_s, body_e, name, ret, 0, 0,
                           (int)tok->line, (int)tok->line))
        return 0;
    }
  }
  return 1;
}

static void clone_finalize_fragments(CClone *c) {
  if (!c->exact_keys || !c->shape_keys)
    return;
  for (size_t i = 0; i < c->nfrags; ++i) {
    CFrag *f = &c->frags[i];
    size_t n = f->tok_end - f->tok_start;
    uint64_t eh = CLONE_HASH_BASE;
    uint64_t sh = CLONE_HASH_BASE;
    for (size_t j = 0; j < n; ++j) {
      eh = eh * CLONE_HASH_BASE + c->exact_keys[f->tok_start + j];
      sh = sh * CLONE_HASH_BASE + c->shape_keys[f->tok_start + j];
    }
    f->exact_hash = clone_mix(eh ^ (uint64_t)n);
    f->shape_hash = clone_mix(sh ^ (uint64_t)n);
  }
}

static uint64_t clone_hash_keys(const uint64_t *keys, size_t n) {
  uint64_t h = CLONE_HASH_BASE;
  for (size_t i = 0; i < n; ++i)
    h = h * CLONE_HASH_BASE + keys[i];
  return clone_mix(h ^ (uint64_t)n);
}

static size_t clone_range_bytes(const CClone *c, size_t start, size_t end) {
  size_t total = 0;
  for (size_t i = start; i < end; ++i)
    total += c->toks.items[i].end - c->toks.items[i].start;
  return total;
}

static void clone_range_lines(const CClone *c, size_t start, size_t end,
                              int *line_start, int *line_end) {
  if (line_start)
    *line_start = start < end ? (int)c->toks.items[start].line : 0;
  if (line_end)
    *line_end = start < end ? (int)c->toks.items[end - 1].line : 0;
}

static int clone_min_ok(const CClone *c, size_t a0, size_t a1, size_t b0,
                        size_t b1) {
  size_t na = a1 > a0 ? a1 - a0 : 0;
  size_t nb = b1 > b0 ? b1 - b0 : 0;
  if (na < c->cfg.min_tokens || nb < c->cfg.min_tokens)
    return 0;
  if (c->cfg.min_lines > 0) {
    int als = 0, ale = 0, bls = 0, ble = 0;
    clone_range_lines(c, a0, a1, &als, &ale);
    clone_range_lines(c, b0, b1, &bls, &ble);
    if (ale - als + 1 < c->cfg.min_lines || ble - bls + 1 < c->cfg.min_lines)
      return 0;
  }
  if (c->cfg.legacy_min_set && c->cfg.legacy_min_bytes > 0) {
    size_t amin = clone_range_bytes(c, a0, a1);
    size_t bmin = clone_range_bytes(c, b0, b1);
    if (amin < (size_t)c->cfg.legacy_min_bytes ||
        bmin < (size_t)c->cfg.legacy_min_bytes)
      return 0;
  }
  return 1;
}

static int clone_keyset_rehash(CKeySet *t) {
  size_t next = t->cap ? t->cap * 2 : 256;
  if (next < t->cap || next > SIZE_MAX / sizeof(CKeySlot))
    return 0;
  CKeySlot *slots = (CKeySlot *)calloc(next, sizeof(CKeySlot));
  if (!slots)
    return 0;
  for (size_t i = 0; i < t->cap; ++i) {
    if (!t->slots[i].a && !t->slots[i].b)
      continue;
    size_t b = (size_t)clone_mix(t->slots[i].a) & (next - 1);
    while (slots[b].a || slots[b].b)
      b = (b + 1) & (next - 1);
    slots[b] = t->slots[i];
  }
  free(t->slots);
  t->slots = slots;
  t->cap = next;
  return 1;
}

/*
 * Returns 1 when inserted, 0 when already present, -1 on allocation failure.
 */
static int clone_keyset_insert(CKeySet *t, uint64_t a, uint64_t b) {
  if (!a && !b)
    a = 1;
  if (t->cap == 0 && !clone_keyset_rehash(t))
    return -1;
  if ((t->len + 1) * 4 >= t->cap * 3 && !clone_keyset_rehash(t))
    return -1;
  size_t slot = (size_t)clone_mix(a) & (t->cap - 1);
  while (t->slots[slot].a || t->slots[slot].b) {
    if (t->slots[slot].a == a && t->slots[slot].b == b)
      return 0;
    slot = (slot + 1) & (t->cap - 1);
  }
  t->slots[slot].a = a;
  t->slots[slot].b = b;
  t->len++;
  return 1;
}

static int clone_keyset_has(const CKeySet *t, uint64_t a, uint64_t b) {
  if (!t || t->cap == 0)
    return 0;
  if (!a && !b)
    a = 1;
  size_t slot = (size_t)clone_mix(a) & (t->cap - 1);
  for (;;) {
    if (!t->slots[slot].a && !t->slots[slot].b)
      return 0;
    if (t->slots[slot].a == a && t->slots[slot].b == b)
      return 1;
    slot = (slot + 1) & (t->cap - 1);
  }
}

static void clone_canon_pair(uint32_t *fa, size_t *a0, size_t *a1,
                              uint32_t *fb, size_t *b0, size_t *b1) {
  int swap = *fa > *fb || (*fa == *fb && (*a0 > *b0 ||
              (*a0 == *b0 && *a1 > *b1)));
  if (!swap)
    return;
  uint32_t tf = *fa;
  size_t ts = *a0, te = *a1;
  *fa = *fb;
  *a0 = *b0;
  *a1 = *b1;
  *fb = tf;
  *b0 = ts;
  *b1 = te;
}

static void clone_pair_signature(uint32_t fa, size_t a0, size_t a1,
                                 uint32_t fb, size_t b0, size_t b1,
                                 uint64_t *sa, uint64_t *sb) {
  *sa = clone_mix((uint64_t)fa * 0x9e3779b97f4a7c15ULL ^
                  (uint64_t)a0 * 0xbf58476d1ce4e5b9ULL ^ (uint64_t)a1);
  *sb = clone_mix((uint64_t)fb * 0x94d049bb133111ebULL ^
                  (uint64_t)b0 * 0xd6e8feb86659fd93ULL ^ (uint64_t)b1);
  if (!*sa && !*sb)
    *sa = 1;
}

static int clone_have_pair(const CClone *c, uint32_t fa, size_t a0, size_t a1,
                           uint32_t fb, size_t b0, size_t b1) {
  clone_canon_pair(&fa, &a0, &a1, &fb, &b0, &b1);
  uint64_t sa = 0, sb = 0;
  clone_pair_signature(fa, a0, a1, fb, b0, b1, &sa, &sb);
  return clone_keyset_has(&c->pairset, sa, sb);
}

static int clone_record_match(CClone *c, int kind, uint32_t fa, size_t a0,
                              size_t a1, uint32_t fb, size_t b0, size_t b1,
                              double similarity, uint64_t content_hash,
                              const CDiff *diffs, int ndiff) {
  if (fa >= c->nsrc || fb >= c->nsrc)
    return 1;
  if (a0 > a1 || b0 > b1 || a1 > c->toks.len || b1 > c->toks.len)
    return 1;
  clone_canon_pair(&fa, &a0, &a1, &fb, &b0, &b1);
  if (fa == fb && a0 == b0 && a1 == b1)
    return 1;
  if (kind != CLK_STRUCTURAL && c->src[fa].lang != c->src[fb].lang)
    return 1;
  uint64_t pa = 0, pb = 0;
  clone_pair_signature(fa, a0, a1, fb, b0, b1, &pa, &pb);
  int pair_new = clone_keyset_insert(&c->pairset, pa, pb);
  if (pair_new < 0)
    return 0;
  uint64_t sa = clone_mix(pa ^ (uint64_t)kind);
  uint64_t sb = clone_mix(pb ^ (uint64_t)kind * 0x100000001b3ULL);
  int new_match = clone_keyset_insert(&c->mset, sa, sb);
  if (new_match < 0)
    return 0;
  if (!new_match)
    return 1;
  if (c->nmatches >= CLONE_MAX_MATCHES) {
    c->stats.incomplete = 1;
    c->stats.candidate_cap_hits++;
    return 1;
  }
  if (!clone_grow((void **)&c->matches, &c->match_cap, c->nmatches + 1,
                  sizeof(*c->matches)))
    return 0;
  CMatch *m = &c->matches[c->nmatches++];
  memset(m, 0, sizeof(*m));
  m->kind = kind;
  m->file_a = fa;
  m->file_b = fb;
  m->tok_a_start = a0;
  m->tok_a_end = a1;
  m->tok_b_start = b0;
  m->tok_b_end = b1;
  clone_range_lines(c, a0, a1, &m->line_a_start, &m->line_a_end);
  clone_range_lines(c, b0, b1, &m->line_b_start, &m->line_b_end);
  m->similarity = similarity;
  m->content_hash = content_hash;
  m->ndiff = ndiff < 0 ? 0 : ndiff > 6 ? 6 : ndiff;
  for (int i = 0; i < m->ndiff; ++i)
    m->diffs[i] = diffs[i];
  return 1;
}

typedef struct {
  size_t window;
  uint32_t *head;
  uint32_t *next;
  size_t buckets;
} CRoll;

static int clone_exact_verify_extend(const CClone *c, const uint64_t *keys,
                                     size_t a, size_t b, size_t w, size_t *a0,
                                     size_t *a1, size_t *b0, size_t *b1) {
  uint32_t fa = c->tok_file[a], fb = c->tok_file[b];
  if (c->src[fa].lang != c->src[fb].lang)
    return 0;
  for (size_t i = 0; i < w; ++i)
    if (keys[a + i] != keys[b + i])
      return 0;
  size_t as = a, bs = b, ae = a + w, be = b + w;
  size_t astart = c->src[fa].tok_start, bstart = c->src[fb].tok_start;
  size_t aend = c->src[fa].tok_end, bend = c->src[fb].tok_end;
  while (as > astart && bs > bstart && keys[as - 1] == keys[bs - 1]) {
    as--;
    bs--;
  }
  while (ae < aend && be < bend && keys[ae] == keys[be]) {
    ae++;
    be++;
  }
  *a0 = as;
  *a1 = ae;
  *b0 = bs;
  *b1 = be;
  return 1;
}

typedef struct {
  uint32_t a[CLONE_MAP_SLOTS];
  uint32_t b[CLONE_MAP_SLOTS];
  size_t n;
} CMap;

static int clone_cmap_bind(CMap *m, const CClone *c, const CTok *ta,
                           const CTok *tb) {
  if ((ta->flags & CTF_RENAMABLE) && (tb->flags & CTF_RENAMABLE)) {
    for (size_t i = 0; i < m->n; ++i) {
      if (m->a[i] == ta->sym)
        return m->b[i] == tb->sym;
      if (m->b[i] == tb->sym)
        return m->a[i] == ta->sym;
    }
    if (m->n >= CLONE_MAP_SLOTS)
      return 0;
    m->a[m->n] = ta->sym;
    m->b[m->n] = tb->sym;
    m->n++;
    return 1;
  }
  return clone_exact_key(c, ta) == clone_exact_key(c, tb);
}

static int clone_renamed_verify_extend(const CClone *c, const uint64_t *shape,
                                       size_t a, size_t b, size_t w,
                                       size_t *a0, size_t *a1, size_t *b0,
                                       size_t *b1) {
  uint32_t fa = c->tok_file[a], fb = c->tok_file[b];
  if (c->src[fa].lang != c->src[fb].lang)
    return 0;
  CMap map;
  memset(&map, 0, sizeof(map));
  for (size_t i = 0; i < w; ++i) {
    if (shape[a + i] != shape[b + i])
      return 0;
    if (!clone_cmap_bind(&map, c, &c->toks.items[a + i],
                         &c->toks.items[b + i]))
      return 0;
  }
  size_t as = a, bs = b, ae = a + w, be = b + w;
  size_t astart = c->src[fa].tok_start, bstart = c->src[fb].tok_start;
  size_t aend = c->src[fa].tok_end, bend = c->src[fb].tok_end;
  while (as > astart && bs > bstart &&
         clone_cmap_bind(&map, c, &c->toks.items[as - 1],
                         &c->toks.items[bs - 1])) {
    as--;
    bs--;
  }
  while (ae < aend && be < bend &&
         clone_cmap_bind(&map, c, &c->toks.items[ae], &c->toks.items[be])) {
    ae++;
    be++;
  }
  *a0 = as;
  *a1 = ae;
  *b0 = bs;
  *b1 = be;
  return 1;
}

static int clone_raw_exact_equal(const CClone *c, size_t a, size_t b,
                                 size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (c->exact_keys[a + i] != c->exact_keys[b + i])
      return 0;
  return 1;
}

static int clone_has_literal_difference(const CClone *c, size_t a, size_t b,
                                        size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const CTok *ta = &c->toks.items[a + i];
    const CTok *tb = &c->toks.items[b + i];
    if ((ta->flags & CTF_LITERAL) && (tb->flags & CTF_LITERAL) &&
        c->exact_keys[a + i] != c->exact_keys[b + i])
      return 1;
  }
  return 0;
}

/*
 * A maximal exact/renamed match can include shared declaration tokens before
 * the body and the closing brace after it.  When the match covers an entire
 * function body, clamp it to that body so all occurrences of the same function
 * hash and group together.  Partial clones deliberately stay untouched.
 */
static int clone_build_frag_index(CClone *c) {
  if (c->nfrags == 0 || c->toks.len == 0)
    return 1;
  uint32_t *map = (uint32_t *)calloc(c->toks.len, sizeof(uint32_t));
  if (!map)
    return 0;
  for (size_t i = 0; i < c->nfrags; ++i) {
    const CFrag *f = &c->frags[i];
    size_t end = f->fn_end < c->toks.len ? f->fn_end : c->toks.len;
    for (size_t j = f->fn_start; j < end; ++j)
      map[j] = (uint32_t)(i + 1);
  }
  c->frag_of_tok = map;
  return 1;
}

static const CFrag *clone_containing_frag(const CClone *c, uint32_t file,
                                          size_t a0, size_t a1) {
  if (!c->frag_of_tok || a0 >= c->toks.len)
    return NULL;
  uint32_t id = c->frag_of_tok[a0];
  if (!id || id > c->nfrags)
    return NULL;
  const CFrag *f = &c->frags[id - 1];
  if (f->file == file && a0 <= f->tok_start && a1 >= f->tok_end &&
      a0 >= f->fn_start && a1 <= f->fn_end + 1)
    return f;
  return NULL;
}

static int clone_try_profile_pair(CClone *c, const uint64_t *keys, int kind,
                                  size_t a, size_t b, size_t w) {
  if (a == b)
    return 1;
  if (c->tok_file[a] == c->tok_file[b] &&
      (a == b || (a < b ? b - a : a - b) < w))
    return 1;
  size_t a0 = 0, a1 = 0, b0 = 0, b1 = 0;
  int verified = 0;
  if (kind == CLK_RENAMED)
    verified = clone_renamed_verify_extend(c, c->shape_keys, a, b, w, &a0,
                                           &a1, &b0, &b1);
  else
    verified = clone_exact_verify_extend(c, keys, a, b, w, &a0, &a1, &b0,
                                         &b1);
  c->stats.verified_pairs++;
  if (!verified) {
    if (kind == CLK_RENAMED)
      c->stats.normalization_failures++;
    else
      c->stats.hash_verification_failures++;
    return 1;
  }
  const CFrag *cfa = clone_containing_frag(c, c->tok_file[a0], a0, a1);
  const CFrag *cfb = clone_containing_frag(c, c->tok_file[b0], b0, b1);
  if (cfa && cfb) {
    a0 = cfa->tok_start;
    a1 = cfa->tok_end;
    b0 = cfb->tok_start;
    b1 = cfb->tok_end;
  }
  if (!clone_min_ok(c, a0, a1, b0, b1))
    return 1;
  size_t len = a1 - a0;
  if (c->tok_file[a0] == c->tok_file[b0] && len == 0)
    return 1;
  if (kind != CLK_EXACT && clone_raw_exact_equal(c, a0, b0, len))
    return 1;
  if (kind == CLK_LITERAL && !clone_has_literal_difference(c, a0, b0, len))
    return 1;
  const uint64_t *content_keys =
      kind == CLK_RENAMED ? c->shape_keys
      : kind == CLK_LITERAL ? c->literal_keys
                            : c->exact_keys;
  uint64_t content = clone_hash_keys(content_keys + a0, len);
  return clone_record_match(c, kind, c->tok_file[a0], a0, a1,
                            c->tok_file[b0], b0, b1, 1.0, content, NULL, 0);
}

static int clone_scan_windows(CClone *c, const uint64_t *keys, int kind) {
  if (!keys || c->toks.len == 0)
    return 1;
  size_t window = c->cfg.min_tokens;
  if (window < 2)
    window = 2;
  size_t window_count = 0;
  for (size_t f = 0; f < c->nsrc; ++f) {
    CSource *s = &c->src[f];
    size_t n = s->tok_end - s->tok_start;
    if (n >= window)
      window_count += n - window + 1;
  }
  if (window_count < 2)
    return 1;
  if (c->toks.len > UINT32_MAX) {
    nyt_warn("ny-fmt", "clone scan: source is too large for 32-bit window indexes");
    c->stats.incomplete = 1;
    return 1;
  }
  size_t buckets = 64;
  while (buckets < window_count * 2 && buckets < (1u << 26)) {
    if (buckets > SIZE_MAX / 2)
      break;
    buckets *= 2;
  }
  uint32_t *head = (uint32_t *)malloc(buckets * sizeof(uint32_t));
  uint32_t *next = (uint32_t *)malloc(c->toks.len * sizeof(uint32_t));
  uint64_t *powers = (uint64_t *)malloc(window * sizeof(uint64_t));
  uint64_t *wh = (uint64_t *)malloc(c->toks.len * sizeof(uint64_t));
  if (!head || !next || !powers || !wh) {
    free(head);
    free(next);
    free(powers);
    free(wh);
    return 0;
  }
  memset(head, 0xff, buckets * sizeof(uint32_t));
  memset(next, 0xff, c->toks.len * sizeof(uint32_t));
  powers[0] = 1;
  for (size_t i = 1; i < window; ++i)
    powers[i] = powers[i - 1] * CLONE_HASH_BASE;
  uint64_t top = powers[window - 1];

  for (size_t f = 0; f < c->nsrc; ++f) {
    CSource *s = &c->src[f];
    size_t n = s->tok_end - s->tok_start;
    if (n < window)
      continue;
    uint64_t h = 0;
    for (size_t k = 0; k < window; ++k)
      h = h * CLONE_HASH_BASE + keys[s->tok_start + k];
    for (size_t p = s->tok_start; p + window <= s->tok_end; ++p) {
      size_t b = (size_t)h & (buckets - 1);
      next[p] = head[b];
      head[b] = (uint32_t)p;
      wh[p] = h;
      if (p + window < s->tok_end)
        h = (h - keys[p] * top) * CLONE_HASH_BASE + keys[p + window];
    }
  }

  int ok = 1;
  uint32_t occ[CLONE_MAX_OCCURRENCES];
  for (size_t b = 0; b < buckets && ok; ++b) {
    size_t nocc = 0;
    int capped = 0;
    for (uint32_t p = head[b]; p != UINT32_MAX; p = next[p]) {
      if (nocc < CLONE_MAX_OCCURRENCES)
        occ[nocc++] = p;
      else {
        capped = 1;
        break;
      }
    }
    if (nocc < 2)
      continue;
    if (capped) {
      c->stats.candidate_cap_hits++;
      c->stats.incomplete = 1;
    }
    for (size_t i = 1; i < nocc; ++i) {
      uint32_t v = occ[i];
      size_t j = i;
      while (j > 0 &&
             (wh[occ[j - 1]] > wh[v] ||
              (wh[occ[j - 1]] == wh[v] && occ[j - 1] > v))) {
        occ[j] = occ[j - 1];
        j--;
      }
      occ[j] = v;
    }
    for (size_t gi = 0; gi < nocc;) {
      size_t gj = gi + 1;
      while (gj < nocc && wh[occ[gj]] == wh[occ[gi]])
        gj++;
      for (size_t i = gi + 1; i < gj; ++i) {
        if (!clone_try_profile_pair(c, keys, kind, occ[gi], occ[i], window)) {
          ok = 0;
          break;
        }
        if (c->nmatches >= CLONE_MAX_MATCHES) {
          c->stats.incomplete = 1;
          break;
        }
      }
      gi = gj;
      if (!ok)
        break;
    }
  }
  free(head);
  free(next);
  free(powers);
  free(wh);
  return ok;
}

static void clone_diff_set(CDiff *d, CClone *c, int op, size_t ia, size_t ib) {
  memset(d, 0, sizeof(*d));
  d->op = op;
  d->line_a = ia < c->toks.len ? (int)c->toks.items[ia].line : 0;
  d->line_b = ib < c->toks.len ? (int)c->toks.items[ib].line : 0;
  if (op != 1 && ia < c->toks.len) {
    size_t n = 0;
    const char *t = csym_text(&c->syms, c->toks.items[ia].sym, &n);
    if (n >= sizeof(d->a))
      n = sizeof(d->a) - 1;
    for (size_t i = 0; i < n; ++i)
      d->a[i] = (t[i] == '\n' || t[i] == '\r' || t[i] == '\t') ? ' ' : t[i];
    d->a[n] = '\0';
  }
  if (op != 2 && ib < c->toks.len) {
    size_t n = 0;
    const char *t = csym_text(&c->syms, c->toks.items[ib].sym, &n);
    if (n >= sizeof(d->b))
      n = sizeof(d->b) - 1;
    for (size_t i = 0; i < n; ++i)
      d->b[i] = (t[i] == '\n' || t[i] == '\r' || t[i] == '\t') ? ' ' : t[i];
    d->b[n] = '\0';
  }
}

static size_t clone_lcs_rows(CClone *c, const uint64_t *ka, size_t na,
                             const uint64_t *kb, size_t nb) {
  size_t need = nb + 1;
  if (!clone_grow((void **)&c->align_row0, &c->align_row_cap, need,
                  sizeof(uint32_t)))
    return 0;
  if (c->align_row1 == NULL || c->align_row_cap < need) {
    size_t cap = c->align_row_cap;
    if (!clone_grow((void **)&c->align_row1, &cap, need, sizeof(uint32_t)))
      return 0;
    c->align_row_cap = cap;
  }
  memset(c->align_row0, 0, need * sizeof(uint32_t));
  memset(c->align_row1, 0, need * sizeof(uint32_t));
  for (size_t i = 1; i <= na; ++i) {
    c->align_row1[0] = 0;
    for (size_t j = 1; j <= nb; ++j) {
      if (ka[i - 1] == kb[j - 1])
        c->align_row1[j] = c->align_row0[j - 1] + 1;
      else
        c->align_row1[j] = c->align_row0[j] > c->align_row1[j - 1]
                               ? c->align_row0[j]
                               : c->align_row1[j - 1];
    }
    uint32_t *tmp = c->align_row0;
    c->align_row0 = c->align_row1;
    c->align_row1 = tmp;
  }
  return c->align_row0[nb];
}

static double clone_align(CClone *c, const uint64_t *ka_base,
                          const uint64_t *kb_base, size_t na, size_t nb,
                          size_t ta0, size_t tb0, int want_diffs, CDiff *diffs,
                          int max_diffs, int *moved, int *out_ndiff) {
  if (moved)
    *moved = 0;
  if (out_ndiff)
    *out_ndiff = 0;
  if (na == 0 || nb == 0)
    return 0.0;
  /*
   * `ta0` and `tb0` are offsets into the global key arrays.  Align the
   * fragment ranges, never the beginning of the shared token stream.
   */
  const uint64_t *ka = ka_base + ta0;
  const uint64_t *kb = kb_base + tb0;
  size_t lcs = 0;
  size_t cells = 0;
  if (na <= 65534 && nb <= 65534 &&
      na + 1 <= SIZE_MAX / (nb + 1) &&
      (cells = (na + 1) * (nb + 1)) <= CLONE_ALIGN_CELLS) {
    if (!clone_grow((void **)&c->align_dp, &c->align_dp_cap, cells,
                    sizeof(uint16_t)))
      return 0.0;
    size_t w = nb + 1;
    memset(c->align_dp, 0, cells * sizeof(uint16_t));
    for (size_t i = 1; i <= na; ++i) {
      c->align_dp[i * w] = 0;
      for (size_t j = 1; j <= nb; ++j) {
        if (ka[i - 1] == kb[j - 1])
          c->align_dp[i * w + j] = (uint16_t)(c->align_dp[(i - 1) * w + j - 1] + 1);
        else {
          uint16_t up = c->align_dp[(i - 1) * w + j];
          uint16_t left = c->align_dp[i * w + j - 1];
          c->align_dp[i * w + j] = up > left ? up : left;
        }
      }
    }
    lcs = c->align_dp[na * w + nb];
    if (want_diffs) {
      size_t i = na, j = nb;
      int nd = 0;
      while (i > 0 && j > 0) {
        if (ka[i - 1] == kb[j - 1]) {
          i--;
          j--;
          continue;
        }
        if (nd < max_diffs) {
          if (c->align_dp[(i - 1) * w + j - 1] == c->align_dp[i * w + j]) {
            clone_diff_set(&diffs[nd++], c, 0, ta0 + i - 1, tb0 + j - 1);
            i--;
            j--;
          } else if (c->align_dp[(i - 1) * w + j] >=
                     c->align_dp[i * w + j - 1]) {
            clone_diff_set(&diffs[nd++], c, 2, ta0 + i - 1, tb0);
            i--;
          } else {
            clone_diff_set(&diffs[nd++], c, 1, ta0, tb0 + j - 1);
            j--;
          }
        } else {
          i--;
          j--;
        }
      }
      while (i > 0 && nd < max_diffs) {
        clone_diff_set(&diffs[nd++], c, 2, ta0 + i - 1, tb0);
        i--;
      }
      while (j > 0 && nd < max_diffs) {
        clone_diff_set(&diffs[nd++], c, 1, ta0, tb0 + j - 1);
        j--;
      }
      if (out_ndiff)
        *out_ndiff = nd;
      if (moved) {
        for (int x = 0; x < nd; ++x) {
          if (diffs[x].op != 2)
            continue;
          for (int y = 0; y < nd; ++y)
            if (diffs[y].op == 1 && strcmp(diffs[x].a, diffs[y].b) == 0) {
              *moved = 1;
              break;
            }
        }
      }
    }
  } else {
    lcs = clone_lcs_rows(c, ka, na, kb, nb);
    c->stats.incomplete = 1;
  }
  return (2.0 * (double)lcs) / ((double)na + (double)nb);
}

typedef struct {
  uint32_t *items;
  size_t len;
  size_t cap;
} CU32Vec;

typedef struct {
  uint32_t frag;
  uint32_t shared;
  size_t length_delta;
} CCand;

static int clone_cand_cmp(const void *a, const void *b) {
  const CCand *x = (const CCand *)a;
  const CCand *y = (const CCand *)b;
  if (x->shared != y->shared)
    return x->shared > y->shared ? -1 : 1;
  if (x->length_delta != y->length_delta)
    return x->length_delta < y->length_delta ? -1 : 1;
  return x->frag < y->frag ? -1 : x->frag > y->frag ? 1 : 0;
}

static int clone_u32_push(CU32Vec *v, uint32_t value) {
  if (!clone_grow((void **)&v->items, &v->cap, v->len + 1,
                  sizeof(*v->items)))
    return 0;
  v->items[v->len++] = value;
  return 1;
}

static int clone_frag_eligible(const CClone *c, const CFrag *f) {
  if (f->tok_end <= f->tok_start)
    return 0;
  size_t n = f->tok_end - f->tok_start;
  size_t need = c->cfg.min_tokens;
  if (need < 12)
    need = 12;
  return n >= need;
}

static size_t clone_frag_len(const CFrag *f) {
  return f->tok_end > f->tok_start ? f->tok_end - f->tok_start : 0;
}

static void clone_choose_rare(CClone *c, const CFrag *f, const uint32_t *freq,
                              uint32_t *chosen, size_t *nchosen) {
  *nchosen = 0;
  for (size_t i = f->tok_start; i < f->tok_end && *nchosen < 8; ++i) {
    const CTok *tok = &c->toks.items[i];
    if (tok->flags & CTF_RENAMABLE)
      continue;
    uint32_t sym = tok->sym;
    uint32_t df = freq[sym];
    if (df < 2 || df > 96)
      continue;
    int seen = 0;
    for (size_t j = 0; j < *nchosen; ++j)
      if (chosen[j] == sym)
        seen = 1;
    if (!seen)
      chosen[(*nchosen)++] = sym;
  }
  if (*nchosen == 0) {
    for (size_t i = f->tok_start; i < f->tok_end && *nchosen < 4; ++i) {
      uint32_t sym = c->toks.items[i].sym;
      int seen = 0;
      for (size_t j = 0; j < *nchosen; ++j)
        if (chosen[j] == sym)
          seen = 1;
      if (!seen)
        chosen[(*nchosen)++] = sym;
    }
  }
}

static int clone_near_scan(CClone *c, int kind, int cross_language) {
  if (c->nfrags < 2)
    return 1;
  if (!clone_ensure_keys(c, &c->near_keys, KP_NEAR))
    return 0;
  if (cross_language && !clone_ensure_keys(c, &c->neutral_keys, KP_NEUTRAL))
    return 0;
  const uint64_t *keys = cross_language ? c->neutral_keys : c->near_keys;
  for (size_t i = 0; i < c->nfrags; ++i)
    if (clone_frag_eligible(c, &c->frags[i]))
      c->stats.near_fragments++;

  if (!cross_language) {
    uint32_t *freq = (uint32_t *)calloc(c->syms.len ? c->syms.len : 1,
                                        sizeof(uint32_t));
    CU32Vec *post = (CU32Vec *)calloc(c->syms.len ? c->syms.len : 1,
                                      sizeof(CU32Vec));
    uint32_t *seen = (uint32_t *)calloc(c->nfrags, sizeof(uint32_t));
    uint32_t *counts = (uint32_t *)calloc(c->nfrags, sizeof(uint32_t));
    uint32_t *touched = (uint32_t *)malloc(c->nfrags * sizeof(uint32_t));
    if (!freq || !post || !seen || !counts || !touched) {
      free(freq);
      free(post);
      free(seen);
      free(counts);
      free(touched);
      return 0;
    }
    for (size_t i = 0; i < c->nfrags; ++i) {
      CFrag *f = &c->frags[i];
      if (!clone_frag_eligible(c, f))
        continue;
      for (size_t j = f->tok_start; j < f->tok_end; ++j)
        freq[c->toks.items[j].sym]++;
    }
    uint32_t chosen[8];
    size_t nchosen = 0;
    for (size_t i = 0; i < c->nfrags; ++i) {
      CFrag *f = &c->frags[i];
      if (!clone_frag_eligible(c, f))
        continue;
      clone_choose_rare(c, f, freq, chosen, &nchosen);
      for (size_t j = 0; j < nchosen; ++j)
        if (chosen[j] < c->syms.len &&
            !clone_u32_push(&post[chosen[j]], (uint32_t)i)) {
          c->stats.incomplete = 1;
          break;
        }
    }

    CCand *cands = (CCand *)malloc(c->nfrags * sizeof(CCand));
    if (!cands) {
      for (size_t i = 0; i < c->syms.len; ++i)
        free(post[i].items);
      free(freq);
      free(post);
      free(seen);
      free(counts);
      free(touched);
      return 0;
    }
    int ok = 1;
    for (size_t i = 0; i < c->nfrags && ok; ++i) {
      CFrag *fa = &c->frags[i];
      if (!clone_frag_eligible(c, fa))
        continue;
      size_t nta = clone_frag_len(fa);
      uint32_t stamp = (uint32_t)(i + 1);
      if (stamp == 0) {
        memset(seen, 0, c->nfrags * sizeof(uint32_t));
        stamp = 1;
      }
      size_t ntouched = 0;
      clone_choose_rare(c, fa, freq, chosen, &nchosen);
      for (size_t s = 0; s < nchosen; ++s) {
        if (chosen[s] >= c->syms.len)
          continue;
        CU32Vec *list = &post[chosen[s]];
        for (size_t k = 0; k < list->len; ++k) {
          uint32_t j = list->items[k];
          if (j <= i)
            continue;
          if (seen[j] != stamp) {
            seen[j] = stamp;
            counts[j] = 0;
            if (ntouched < c->nfrags)
              touched[ntouched++] = j;
          }
          counts[j]++;
        }
      }
      size_t ncand = 0;
      for (size_t t = 0; t < ntouched; ++t) {
        uint32_t j = touched[t];
        CFrag *fb = &c->frags[j];
        if (!clone_frag_eligible(c, fb) || fb->file == fa->file) {
          if (!clone_frag_eligible(c, fb))
            continue;
        }
        if (c->src[fa->file].lang != c->src[fb->file].lang)
          continue;
        if (counts[j] < 2)
          continue;
        size_t ntb = clone_frag_len(fb);
        size_t shorter = nta < ntb ? nta : ntb;
        size_t longer = nta > ntb ? nta : ntb;
        if (longer == 0 || shorter * 100 < longer * 45)
          continue;
        cands[ncand].frag = j;
        cands[ncand].shared = counts[j];
        cands[ncand].length_delta = nta > ntb ? nta - ntb : ntb - nta;
        ncand++;
      }
      qsort(cands, ncand, sizeof(cands[0]), clone_cand_cmp);
      size_t limit = ncand < CLONE_NEAR_CANDIDATES ? ncand : CLONE_NEAR_CANDIDATES;
      for (size_t k = 0; k < limit && ok; ++k) {
        if (c->stats.alignments >= CLONE_MAX_ALIGNMENTS) {
          c->stats.incomplete = 1;
          c->stats.candidate_cap_hits++;
          break;
        }
        CFrag *fb = &c->frags[cands[k].frag];
        if (clone_have_pair(c, fa->file, fa->tok_start, fa->tok_end,
                            fb->file, fb->tok_start, fb->tok_end))
          continue;
        CDiff diffs[6];
        int moved = 0;
        int ndiff = 0;
        double sim = clone_align(c, c->near_keys, c->near_keys, nta,
                                 clone_frag_len(fb), fa->tok_start,
                                 fb->tok_start, 1, diffs, 6, &moved,
                                 &ndiff);
        c->stats.alignments++;
        c->stats.near_candidates++;
        if (sim < c->cfg.similarity)
          continue;
        if (clone_raw_exact_equal(c, fa->tok_start, fb->tok_start, nta) &&
            nta == clone_frag_len(fb))
          continue;
        uint64_t ha = clone_hash_keys(c->near_keys + fa->tok_start, nta);
        uint64_t hb = clone_hash_keys(c->near_keys + fb->tok_start,
                                      clone_frag_len(fb));
        uint64_t content = clone_mix(ha ^ clone_mix(hb));
        if (!clone_record_match(c, kind, fa->file, fa->tok_start, fa->tok_end,
                                fb->file, fb->tok_start, fb->tok_end, sim,
                                content, diffs, ndiff)) {
          ok = 0;
          break;
        }
        if (moved) {
          CMatch *m = &c->matches[c->nmatches - 1];
          m->moved_candidate = 1;
        }
      }
    }
    free(cands);
    for (size_t i = 0; i < c->syms.len; ++i)
      free(post[i].items);
    free(freq);
    free(post);
    free(seen);
    free(counts);
    free(touched);
    return ok;
  }

  /*
   * Experimental cross-language structural comparison.
   */
  for (size_t i = 0; i < c->nfrags; ++i) {
    CFrag *fa = &c->frags[i];
    if (!clone_frag_eligible(c, fa) ||
        c->src[fa->file].lang != CLONE_LANG_C)
      continue;
    size_t nta = clone_frag_len(fa);
    size_t compared = 0;
    for (size_t j = 0; j < c->nfrags && compared < 24; ++j) {
      CFrag *fb = &c->frags[j];
      if (!clone_frag_eligible(c, fb) ||
          c->src[fb->file].lang != CLONE_LANG_NY)
        continue;
      size_t ntb = clone_frag_len(fb);
      size_t shorter = nta < ntb ? nta : ntb;
      size_t longer = nta > ntb ? nta : ntb;
      if (longer == 0 || shorter * 100 < longer * 60)
        continue;
      if (clone_have_pair(c, fa->file, fa->tok_start, fa->tok_end, fb->file,
                          fb->tok_start, fb->tok_end))
        continue;
      if (c->stats.alignments >= CLONE_MAX_ALIGNMENTS) {
        c->stats.incomplete = 1;
        return 1;
      }
      CDiff diffs[6];
      int moved = 0;
      int ndiff = 0;
      double sim = clone_align(c, keys, keys, nta, ntb, fa->tok_start,
                               fb->tok_start, 1, diffs, 6, &moved, &ndiff);
      c->stats.alignments++;
      compared++;
      if (sim < c->cfg.similarity)
        continue;
      uint64_t ha = clone_hash_keys(keys + fa->tok_start, nta);
      uint64_t hb = clone_hash_keys(keys + fb->tok_start, ntb);
      uint64_t content = clone_mix(ha ^ clone_mix(hb));
      if (!clone_record_match(c, CLK_STRUCTURAL, fa->file, fa->tok_start,
                              fa->tok_end, fb->file, fb->tok_start, fb->tok_end,
                              sim, content, diffs, ndiff))
        return 0;
    }
  }
  return 1;
}

static size_t clone_find_fragment(const CClone *c, uint32_t file, size_t tok) {
  for (size_t i = 0; i < c->nfrags; ++i) {
    const CFrag *f = &c->frags[i];
    if (f->file == file && tok >= f->fn_start && tok < f->fn_end)
      return i + 1;
  }
  return 0;
}

static int clone_family_push(CClone *c, int kind, uint64_t content,
                             size_t token_count, double similarity) {
  if (c->nfams >= CLONE_MAX_FAMILIES) {
    c->stats.incomplete = 1;
    return 0;
  }
  if (!clone_grow((void **)&c->fams, &c->fam_cap, c->nfams + 1,
                  sizeof(*c->fams)))
    return 0;
  CFamily *f = &c->fams[c->nfams++];
  memset(f, 0, sizeof(*f));
  f->kind = kind;
  f->content_hash = content;
  f->token_count = token_count;
  f->similarity = similarity;
  f->stable_id = clone_mix((uint64_t)kind ^ clone_mix(content) ^
                           (uint64_t)token_count * 0x9e3779b97f4a7c15ULL);
  return 1;
}

static int clone_family_add_occ(CClone *c, CFamily *f, uint32_t file,
                                size_t tok_start, size_t tok_end) {
  if (!f || file >= c->nsrc || tok_start >= tok_end || tok_end > c->toks.len)
    return 1;
  if (!clone_grow((void **)&f->occs, &f->occ_cap, f->nocc + 1,
                  sizeof(*f->occs)))
    return 0;
  COcc *o = &f->occs[f->nocc++];
  memset(o, 0, sizeof(*o));
  o->file = file;
  o->tok_start = tok_start;
  o->tok_end = tok_end;
  clone_range_lines(c, tok_start, tok_end, &o->line_start, &o->line_end);
  o->frag = clone_find_fragment(c, file, tok_start);
  if (!f->lang)
    f->lang = (uint32_t)c->src[file].lang;
  return 1;
}

static int clone_occ_cmp(const void *a, const void *b) {
  const COcc *x = (const COcc *)a;
  const COcc *y = (const COcc *)b;
  if (x->file != y->file)
    return x->file < y->file ? -1 : 1;
  if (x->tok_start != y->tok_start)
    return x->tok_start < y->tok_start ? -1 : 1;
  if (x->tok_end != y->tok_end)
    return x->tok_end < y->tok_end ? -1 : 1;
  return 0;
}

static void clone_family_dedupe_occs(CFamily *f) {
  if (f->nocc < 2)
    return;
  qsort(f->occs, f->nocc, sizeof(f->occs[0]), clone_occ_cmp);
  size_t w = 1;
  for (size_t i = 1; i < f->nocc; ++i) {
    if (clone_occ_cmp(&f->occs[i], &f->occs[w - 1]) != 0)
      f->occs[w++] = f->occs[i];
  }
  f->nocc = w;
}

static int clone_family_add_diff(CFamily *f, const char *text) {
  if (f->ndiff >= 4)
    return 1;
  snprintf(f->diffs[f->ndiff], sizeof(f->diffs[0]), "%s", text);
  f->ndiff++;
  return 1;
}

static int clone_token_in(const CClone *c, const CTok *tok,
                          const char *const *words) {
  size_t n = 0;
  const char *t = csym_text(&c->syms, tok->sym, &n);
  for (size_t i = 0; words[i]; ++i)
    if (strcmp(t, words[i]) == 0)
      return 1;
  return 0;
}

static void clone_describe_structure(CClone *c, CFamily *f) {
  if (f->nocc == 0)
    return;
  COcc *o = &f->occs[0];
  static const char *const loop_words[] = {"for", "while", "do", "loop", NULL};
  static const char *const branch_words[] = {"if", "else", "elif", "switch",
                                             "match", "case", "?", NULL};
  int loops = 0, branches = 0, calls = 0, returns = 0;
  for (size_t i = o->tok_start; i < o->tok_end; ++i) {
    const CTok *tok = &c->toks.items[i];
    if (tok->kind == CT_KEYWORD && clone_token_in(c, tok, loop_words))
      loops++;
    else if (tok->kind == CT_KEYWORD && clone_token_in(c, tok, branch_words))
      branches++;
    else if (tok->kind == CT_KEYWORD && csym_is(&c->syms, tok->sym, "return"))
      returns++;
    if (tok->kind == CT_IDENT && i + 1 < o->tok_end &&
        clone_punct_is(&c->syms, &c->toks.items[i + 1], "("))
      calls++;
  }
  snprintf(f->structure, sizeof(f->structure),
           "%d branch(es), %d loop(s), %d call(s), %d return(s)", branches,
           loops, calls, returns);
}

static void clone_collect_deps(CClone *c, CFamily *f) {
  if (f->nocc == 0)
    return;
  COcc *o = &f->occs[0];
  for (size_t i = o->tok_start; i + 1 < o->tok_end && f->ndeps < 6; ++i) {
    const CTok *tok = &c->toks.items[i];
    if (tok->kind != CT_IDENT ||
        !clone_punct_is(&c->syms, &c->toks.items[i + 1], "("))
      continue;
    size_t n = 0;
    const char *t = csym_text(&c->syms, tok->sym, &n);
    int seen = 0;
    for (int j = 0; j < f->ndeps; ++j)
      if (strcmp(f->deps[j], t) == 0)
        seen = 1;
    if (seen)
      continue;
    snprintf(f->deps[f->ndeps], sizeof(f->deps[0]), "%s", t);
    f->ndeps++;
  }
}

static void clone_detect_side_effects(CClone *c, CFamily *f) {
  if (f->nocc == 0)
    return;
  static const char *const effects[] = {
      "free",   "dealloc", "fclose", "close", "write", "read",
      "fwrite", "malloc",  "realloc", "calloc", "atomic", "volatile",
      "goto",   "defer",   "panic",  "abort", "exit",  NULL};
  COcc *o = &f->occs[0];
  for (size_t i = o->tok_start; i < o->tok_end; ++i)
    if (clone_token_in(c, &c->toks.items[i], effects)) {
      f->side_effects = 1;
      return;
    }
}

static void clone_family_recommend(CClone *c, CFamily *f) {
  (void)c;
  f->confidence = f->kind == CLK_EXACT ? 1.0
                  : f->kind == CLK_RENAMED ? 0.95
                  : f->kind == CLK_LITERAL ? 0.90
                  : f->kind == CLK_NEAR ? f->similarity
                                        : f->similarity * 0.80;
  snprintf(f->semantic, sizeof(f->semantic), "%s",
           f->kind == CLK_EXACT ? "verified_token_equality"
           : f->kind == CLK_RENAMED ? "verified_consistent_renaming"
           : f->kind == CLK_LITERAL ? "verified_literal_normalized"
           : f->kind == CLK_NEAR ? "unverified"
                                 : "structurally_related_unverified");
  snprintf(f->risk, sizeof(f->risk), "%s",
           f->side_effects ? "high" : f->ndeps > 0 ? "medium" : "low");
  const char *action = "INVESTIGATE_DIVERGENCE";
  const char *why = "compare behavior and error paths before consolidating";
  if (f->kind == CLK_EXACT) {
    if (f->token_count < 20) {
      action = "KEEP_SEPARATE";
      why = "the shared text is too small to justify a helper";
    } else if (f->nocc >= 2) {
      action = "EXTRACT_SHARED_HELPER";
      why = "extract the identical region and have each site call it";
    }
    if (f->nocc >= 2 && f->affected_files == 1 && f->token_count >= 80) {
      action = "CONSOLIDATE_IMPLEMENTATIONS";
      why = "merge the duplicate implementations in this module";
    }
  } else if (f->kind == CLK_RENAMED || f->kind == CLK_LITERAL) {
    action = "PARAMETERIZE_VARIATION";
    why = "parameterize the consistently varying identifiers or literals";
  } else if (f->kind == CLK_NEAR) {
    if (f->divergence) {
      action = "INVESTIGATE_DIVERGENCE";
      why = "the copies differ in error, cleanup, or control-flow behavior";
    } else if (f->token_count < 60 && f->similarity < 0.92) {
      action = "KEEP_SEPARATE";
      why = "the shared shape is not strong enough for a safe helper";
    } else if (f->similarity >= 0.95) {
      action = "EXTRACT_SHARED_HELPER";
      why = "extract the common region and parameterize the remaining changes";
    } else {
      action = "INVESTIGATE_DIVERGENCE";
      why = "review the changed statements before deciding on a helper";
    }
  } else {
    action = "KEEP_SEPARATE";
    why = "structural similarity alone does not establish semantic equivalence";
  }
  if (f->side_effects && strcmp(action, "EXTRACT_SHARED_HELPER") == 0) {
    action = "INVESTIGATE_DIVERGENCE";
    why = "side effects or resource handling must be reconciled first";
  }
  snprintf(f->action, sizeof(f->action), "%s", action);
  snprintf(f->recommendation, sizeof(f->recommendation), "%s", why);
}

static void clone_family_metadata(CClone *c, CFamily *f) {
  clone_family_dedupe_occs(f);
  if (f->nocc < 2)
    return;
  f->affected_files = 1;
  for (size_t i = 1; i < f->nocc; ++i)
    if (f->occs[i].file != f->occs[i - 1].file)
      f->affected_files++;
  clone_describe_structure(c, f);
  clone_collect_deps(c, f);
  clone_detect_side_effects(c, f);
  f->return_compat = 0;
  if (f->nocc >= 2 && f->occs[0].frag && f->occs[1].frag) {
    const CFrag *fa = &c->frags[f->occs[0].frag - 1];
    const CFrag *fb = &c->frags[f->occs[1].frag - 1];
    if (fa->ret[0] && fb->ret[0])
      f->return_compat = strcmp(fa->ret, fb->ret) == 0 ? 1 : 2;
  }
  for (int i = 0; i < f->ndiff; ++i) {
    if (strstr(f->diffs[i], "free") || strstr(f->diffs[i], "catch") ||
        strstr(f->diffs[i], "err") || strstr(f->diffs[i], "panic") ||
        strstr(f->diffs[i], "alloc"))
      f->divergence = 1;
  }
  clone_family_recommend(c, f);
  f->dup_tokens = f->token_count > 0 && f->nocc > 1
                      ? (long)(f->token_count * (f->nocc - 1))
                      : 0;
  long score = f->dup_tokens > 1000000 ? 1000000 : f->dup_tokens;
  score += (long)(f->confidence * 300.0);
  score += (long)f->nocc * 120;
  if (f->divergence)
    score += 250;
  if (f->side_effects)
    score -= 80;
  f->score = score;
}

static int clone_range_equal_keys(const uint64_t *keys, size_t a, size_t b,
                                  size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (keys[a + i] != keys[b + i])
      return 0;
  return 1;
}

typedef struct {
  uint64_t hash;
  size_t len;
  int kind;
  size_t idx;
} CMatchSort;

static int clone_match_sort_cmp(const void *a, const void *b) {
  const CMatchSort *x = (const CMatchSort *)a;
  const CMatchSort *y = (const CMatchSort *)b;
  if (x->kind != y->kind)
    return x->kind < y->kind ? -1 : 1;
  if (x->hash != y->hash)
    return x->hash < y->hash ? -1 : 1;
  if (x->len != y->len)
    return x->len < y->len ? -1 : 1;
  return x->idx < y->idx ? -1 : x->idx > y->idx ? 1 : 0;
}

static void clone_copy_match_diffs(CFamily *f, const CMatch *m) {
  for (int i = 0; i < m->ndiff && f->ndiff < 4; ++i) {
    char buf[176];
    const CDiff *d = &m->diffs[i];
    if (d->op == 0)
      snprintf(buf, sizeof(buf), "line %d: '%s' -> '%s'", d->line_a, d->a,
               d->b);
    else if (d->op == 1)
      snprintf(buf, sizeof(buf), "line %d: added '%s'", d->line_b, d->b);
    else
      snprintf(buf, sizeof(buf), "line %d: removed '%s'", d->line_a, d->a);
    clone_family_add_diff(f, buf);
  }
  f->moved_candidate = m->moved_candidate;
}

static int clone_build_families(CClone *c) {
  if (c->nmatches == 0)
    return 1;
  CMatchSort *sorted =
      (CMatchSort *)malloc(c->nmatches * sizeof(CMatchSort));
  if (!sorted)
    return 0;
  for (size_t i = 0; i < c->nmatches; ++i) {
    sorted[i].hash = c->matches[i].content_hash;
    sorted[i].len = c->matches[i].tok_a_end - c->matches[i].tok_a_start;
    sorted[i].kind = c->matches[i].kind;
    sorted[i].idx = i;
  }
  qsort(sorted, c->nmatches, sizeof(sorted[0]), clone_match_sort_cmp);
  for (size_t i = 0; i < c->nmatches;) {
    size_t j = i + 1;
    while (j < c->nmatches && sorted[j].kind == sorted[i].kind &&
           sorted[j].hash == sorted[i].hash && sorted[j].len == sorted[i].len)
      j++;
    CMatch *first = &c->matches[sorted[i].idx];
    int kind = first->kind;
    if (kind == CLK_NEAR || kind == CLK_STRUCTURAL) {
      for (size_t k = i; k < j; ++k) {
        CMatch *m = &c->matches[sorted[k].idx];
        if (!clone_family_push(c, kind, m->content_hash,
                               m->tok_a_end - m->tok_a_start, m->similarity))
          break;
        CFamily *f = &c->fams[c->nfams - 1];
        if (!clone_family_add_occ(c, f, m->file_a, m->tok_a_start,
                                  m->tok_a_end) ||
            !clone_family_add_occ(c, f, m->file_b, m->tok_b_start,
                                  m->tok_b_end))
          break;
        clone_copy_match_diffs(f, m);
      }
    } else {
      const uint64_t *keys = kind == CLK_EXACT
                                 ? c->exact_keys
                                 : kind == CLK_RENAMED ? c->shape_keys
                                                       : c->literal_keys;
      CFamily *fam = NULL;
      size_t rep_a = 0;
      int rep_file = -1;
      for (size_t k = i; k < j; ++k) {
        CMatch *m = &c->matches[sorted[k].idx];
        size_t len = m->tok_a_end - m->tok_a_start;
        int use_new = fam == NULL || (int)m->file_a != rep_file ||
                      !clone_range_equal_keys(keys, rep_a, m->tok_a_start, len);
        if (use_new) {
          if (!clone_family_push(c, kind, m->content_hash, len, 1.0))
            break;
          fam = &c->fams[c->nfams - 1];
          rep_a = m->tok_a_start;
          rep_file = (int)m->file_a;
          if (kind == CLK_EXACT)
            clone_family_add_diff(fam, "none: token-identical");
          else if (kind == CLK_RENAMED)
            clone_family_add_diff(fam, "consistent identifier renaming");
          else
            clone_family_add_diff(fam, "literal values differ");
        }
        if (!clone_family_add_occ(c, fam, m->file_a, m->tok_a_start,
                                  m->tok_a_end) ||
            !clone_family_add_occ(c, fam, m->file_b, m->tok_b_start,
                                  m->tok_b_end))
          break;
      }
    }
    i = j;
  }
  free(sorted);
  for (size_t i = 0; i < c->nfams; ++i)
    clone_family_metadata(c, &c->fams[i]);
  return 1;
}

typedef struct {
  size_t start;
  size_t end;
} CInterval;

typedef struct {
  CInterval *items;
  size_t len;
  size_t cap;
} CIntVec;

static int clone_intvec_add(CIntVec *v, size_t start, size_t end) {
  if (end <= start)
    return 1;
  size_t lo = 0, hi = v->len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (v->items[mid].start < start)
      lo = mid + 1;
    else
      hi = mid;
  }
  size_t insert = lo;
  if (insert > 0 && v->items[insert - 1].end >= start) {
    insert--;
    if (v->items[insert].start < start)
      start = v->items[insert].start;
    if (v->items[insert].end > end)
      end = v->items[insert].end;
    v->len--;
    memmove(&v->items[insert], &v->items[insert + 1],
            (v->len - insert) * sizeof(v->items[0]));
  }
  while (insert < v->len && v->items[insert].start <= end) {
    if (v->items[insert].end > end)
      end = v->items[insert].end;
    v->len--;
    memmove(&v->items[insert], &v->items[insert + 1],
            (v->len - insert) * sizeof(v->items[0]));
  }
  if (!clone_grow((void **)&v->items, &v->cap, v->len + 1,
                  sizeof(v->items[0])))
    return 0;
  memmove(&v->items[insert + 1], &v->items[insert],
          (v->len - insert) * sizeof(v->items[0]));
  v->items[insert].start = start;
  v->items[insert].end = end;
  v->len++;
  return 1;
}

static int clone_intvec_contains(const CIntVec *v, size_t start, size_t end) {
  if (!v || !v->len)
    return 0;
  size_t lo = 0, hi = v->len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (v->items[mid].start < start)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == 0)
    return 0;
  return v->items[lo - 1].end >= end;
}

typedef struct {
  size_t idx;
  size_t len;
  uint64_t stable;
} CFamSort;

static int clone_fam_sort_cmp(const void *a, const void *b) {
  const CFamSort *x = (const CFamSort *)a;
  const CFamSort *y = (const CFamSort *)b;
  if (x->len != y->len)
    return x->len > y->len ? -1 : 1;
  if (x->stable != y->stable)
    return x->stable < y->stable ? -1 : 1;
  return x->idx < y->idx ? -1 : x->idx > y->idx ? 1 : 0;
}

static int clone_fam_output_cmp(const void *a, const void *b) {
  const CFamily *x = (const CFamily *)a;
  const CFamily *y = (const CFamily *)b;
  if (x->score != y->score)
    return x->score > y->score ? -1 : 1;
  if (x->dup_tokens != y->dup_tokens)
    return x->dup_tokens > y->dup_tokens ? -1 : 1;
  if (x->stable_id != y->stable_id)
    return x->stable_id < y->stable_id ? -1 : 1;
  return x->kind < y->kind ? -1 : x->kind > y->kind ? 1 : 0;
}

static int clone_subsume_families(CClone *c) {
  if (c->nfams < 2)
    return 1;
  if (c->nfams > 4000) {
    c->stats.incomplete = 1;
    return 1;
  }
  CFamSort *order = (CFamSort *)malloc(c->nfams * sizeof(CFamSort));
  CIntVec *intervals =
      (CIntVec *)calloc(c->nsrc ? c->nsrc : 1, sizeof(CIntVec));
  if (!order || !intervals) {
    free(order);
    free(intervals);
    return 1;
  }
  for (size_t i = 0; i < c->nfams; ++i) {
    order[i].idx = i;
    order[i].len = c->fams[i].token_count;
    order[i].stable = c->fams[i].stable_id;
  }
  qsort(order, c->nfams, sizeof(order[0]), clone_fam_sort_cmp);
  for (size_t oi = 0; oi < c->nfams; ++oi) {
    CFamily *f = &c->fams[order[oi].idx];
    if (f->nocc < 2)
      continue;
    int eligible = f->kind == CLK_EXACT || f->kind == CLK_RENAMED ||
                   f->kind == CLK_LITERAL;
    if (eligible) {
      int all_covered = 1;
      for (size_t i = 0; i < f->nocc; ++i) {
        COcc *o = &f->occs[i];
        if (o->file >= c->nsrc ||
            !clone_intvec_contains(&intervals[o->file], o->tok_start,
                                   o->tok_end)) {
          all_covered = 0;
          break;
        }
      }
      if (all_covered) {
        free(f->occs);
        f->occs = NULL;
        f->nocc = 0;
        f->occ_cap = 0;
        c->stats.subsumed++;
        continue;
      }
    }
    if (eligible) {
      for (size_t i = 0; i < f->nocc; ++i) {
        COcc *o = &f->occs[i];
        if (o->file < c->nsrc &&
            !clone_intvec_add(&intervals[o->file], o->tok_start, o->tok_end)) {
          c->stats.incomplete = 1;
          break;
        }
      }
    }
  }
  for (size_t i = 0; i < c->nsrc; ++i)
    free(intervals[i].items);
  free(intervals);
  free(order);
  return 1;
}

static void clone_mark_duplication(CClone *c) {
  for (size_t i = 0; i < c->nsrc; ++i) {
    CSource *s = &c->src[i];
    size_t nt = s->tok_end - s->tok_start;
    s->dup_tok = (uint8_t *)calloc(nt ? nt : 1, 1);
    s->dup_line = (uint8_t *)calloc((size_t)s->line_count + 2, 1);
  }
  for (size_t i = 0; i < c->nfams; ++i) {
    CFamily *f = &c->fams[i];
    if (f->nocc < 2)
      continue;
    if (f->kind == CLK_EXACT)
      c->stats.exact_families++;
    else if (f->kind == CLK_RENAMED)
      c->stats.renamed_families++;
    else if (f->kind == CLK_LITERAL)
      c->stats.literal_families++;
    else if (f->kind == CLK_NEAR)
      c->stats.near_families++;
    else
      c->stats.structural_families++;
    c->stats.duplicate_functions += (int)f->nocc;
    for (size_t j = 0; j < f->nocc; ++j) {
      COcc *o = &f->occs[j];
      if (o->file >= c->nsrc)
        continue;
      CSource *s = &c->src[o->file];
      if (s->dup_tok && o->tok_end <= s->tok_end) {
        for (size_t k = o->tok_start; k < o->tok_end; ++k)
          s->dup_tok[k - s->tok_start] = 1;
      }
      if (s->dup_line) {
        int ls = o->line_start, le = o->line_end;
        if (ls < 1)
          ls = 1;
        if (le > s->line_count)
          le = s->line_count;
        for (int line = ls; line <= le; ++line)
          s->dup_line[line] = 1;
      }
    }
  }
  for (size_t i = 0; i < c->nsrc; ++i) {
    CSource *s = &c->src[i];
    if (s->dup_tok)
      for (size_t k = 0; k < s->tok_end - s->tok_start; ++k)
        c->stats.duplicate_tokens += s->dup_tok[k] ? 1 : 0;
    if (s->dup_line)
      for (int line = 1; line <= s->line_count; ++line)
        c->stats.duplicate_lines += s->dup_line[line] ? 1 : 0;
  }
}

static void clone_finalize_families(CClone *c) {
  size_t w = 0;
  for (size_t i = 0; i < c->nfams; ++i) {
    CFamily *f = &c->fams[i];
    if (f->nocc < 2) {
      free(f->occs);
      f->occs = NULL;
      continue;
    }
    if (w != i)
      c->fams[w] = c->fams[i];
    w++;
  }
  c->nfams = w;
  clone_subsume_families(c);
  clone_mark_duplication(c);
  qsort(c->fams, c->nfams, sizeof(c->fams[0]), clone_fam_output_cmp);
}

typedef struct {
  uint64_t *items;
  size_t len;
  size_t cap;
} CIdVec;

static int clone_id_cmp(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static int clone_id_add(CIdVec *v, uint64_t id) {
  if (!clone_grow((void **)&v->items, &v->cap, v->len + 1,
                  sizeof(v->items[0])))
    return 0;
  v->items[v->len++] = id;
  return 1;
}

static uint64_t clone_parse_stable_id(const char *s) {
  if (!s)
    return 0;
  if (strncmp(s, "cf-", 3) == 0) {
    char *end = NULL;
    unsigned long long v = strtoull(s + 3, &end, 16);
    if (end && *end == '\0' && v)
      return (uint64_t)v;
  }
  return clone_hash_bytes(s, strlen(s));
}

static int clone_load_baseline(const char *path, CIdVec *out,
                               int *invalid) {
  *invalid = 0;
  if (!path || !*path)
    return 1;
  size_t len = 0;
  char *text = ny_read_file_raw(path, &len);
  if (!text) {
    nyt_err("ny-fmt", "cannot read clone baseline: %s", path);
    return 0;
  }
  const char *needle = "\"stable_id\"";
  const char *p = text;
  while ((p = strstr(p, needle)) != NULL) {
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
    if (*p != ':')
      continue;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
    if (*p != '"')
      continue;
    p++;
    const char *start = p;
    while (*p && *p != '"' && *p != '\n')
      p++;
    if (*p != '"')
      continue;
    size_t n = (size_t)(p - start);
    char id[128];
    if (n >= sizeof(id))
      n = sizeof(id) - 1;
    memcpy(id, start, n);
    id[n] = '\0';
    if (!clone_id_add(out, clone_parse_stable_id(id))) {
      free(text);
      return 0;
    }
  }
  free(text);
  if (out->len == 0) {
    *invalid = 1;
    nyt_warn("ny-fmt", "baseline contains no stable_id fields: %s", path);
  } else if (out->len > 1) {
    qsort(out->items, out->len, sizeof(out->items[0]), clone_id_cmp);
    size_t w = 1;
    for (size_t i = 1; i < out->len; ++i)
      if (out->items[i] != out->items[w - 1])
        out->items[w++] = out->items[i];
    out->len = w;
  }
  return 1;
}

static int clone_idvec_has(const CIdVec *v, uint64_t id) {
  size_t lo = 0, hi = v->len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (v->items[mid] < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < v->len && v->items[lo] == id;
}

static void clone_apply_baseline(CClone *c, const char *path) {
  CIdVec base;
  memset(&base, 0, sizeof(base));
  if (!path || !*path)
    return;
  if (!clone_load_baseline(path, &base, &c->stats.baseline_invalid)) {
    free(base.items);
    return;
  }
  CKeySet current;
  memset(&current, 0, sizeof(current));
  for (size_t i = 0; i < c->nfams; ++i) {
    CFamily *f = &c->fams[i];
    if (f->nocc < 2)
      continue;
    if (clone_keyset_insert(&current, f->stable_id, 1) < 0) {
      c->stats.incomplete = 1;
      break;
    }
    if (clone_idvec_has(&base, f->stable_id))
      c->stats.baseline_existing++;
    else
      c->stats.baseline_new++;
  }
  for (size_t i = 0; i < base.len; ++i)
    if (!clone_keyset_has(&current, base.items[i], 1))
      c->stats.baseline_resolved++;
  free(current.slots);
  free(base.items);
}

static void clone_json_str(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
    if (*p == '"' || *p == '\\') {
      putchar('\\');
      putchar(*p);
    } else if (*p == '\n') {
      fputs("\\n", stdout);
    } else if (*p == '\r') {
      fputs("\\r", stdout);
    } else if (*p == '\t') {
      fputs("\\t", stdout);
    } else if (*p < 0x20) {
      printf("\\u%04x", (unsigned)*p);
    } else {
      putchar(*p);
    }
  }
  putchar('"');
}

static const char *clone_mode_name(const CConfig *cfg) {
  if (cfg->modes == CM_EXACT)
    return "exact";
  if (cfg->modes == CM_RENAMED)
    return "renamed";
  if (cfg->modes == CM_LITERAL)
    return "literal";
  if (cfg->modes == CM_NEAR)
    return "near";
  if (cfg->modes == CM_STRUCTURAL)
    return "structural";
  return "all";
}

static const char *clone_lang_name(int lang) {
  return lang == CLONE_LANG_C ? "c" : lang == CLONE_LANG_NY ? "ny" : "all";
}

static const char *clone_family_lang(const CFamily *f) {
  return f->lang == CLONE_LANG_C ? "c" : f->lang == CLONE_LANG_NY ? "ny" : "mixed";
}

static const char *clone_kind_name(int kind) {
  switch (kind) {
  case CLK_EXACT:
    return "exact";
  case CLK_RENAMED:
    return "renamed";
  case CLK_LITERAL:
    return "literal";
  case CLK_NEAR:
    return "near-miss";
  default:
    return "structural";
  }
}

static const char *clone_return_compat(const CFamily *f) {
  return f->return_compat == 1 ? "same"
         : f->return_compat == 2 ? "differ"
                                  : "unknown";
}

static size_t clone_family_line_count(const CFamily *f) {
  if (f->nocc == 0)
    return 0;
  return f->occs[0].line_end >= f->occs[0].line_start
             ? (size_t)(f->occs[0].line_end - f->occs[0].line_start + 1)
             : 0;
}

static size_t clone_shown_count(const CClone *c, int limit) {
  size_t total = 0;
  for (size_t i = 0; i < c->nfams; ++i)
    if (c->fams[i].nocc >= 2)
      total++;
  if (limit > 0 && total > (size_t)limit)
    return (size_t)limit;
  return total;
}

static void clone_print_occurrences_json(const CClone *c, const CFamily *f) {
  for (size_t i = 0; i < f->nocc; ++i) {
    const COcc *o = &f->occs[i];
    if (i)
      printf(", ");
    printf("{\"file\": ");
    clone_json_str(c->src[o->file].path);
    printf(", \"start_line\": %d, \"end_line\": %d, \"byte_start\": ",
           o->line_start, o->line_end);
    printf("%u, \"byte_end\": %u, \"function\": ",
           c->toks.items[o->tok_start].start,
           c->toks.items[o->tok_end - 1].end);
    clone_json_str(o->frag ? c->frags[o->frag - 1].name : "");
    printf("}");
  }
}

static void clone_print_json(const CClone *c, const CloneOptions *opts) {
  size_t shown = clone_shown_count(c, opts->limit);
  double dup_tok_pct = c->stats.tokens
                           ? 100.0 * (double)c->stats.duplicate_tokens /
                                 (double)c->stats.tokens
                           : 0.0;
  double dup_line_pct = c->stats.lines
                            ? 100.0 * (double)c->stats.duplicate_lines /
                                  (double)c->stats.lines
                            : 0.0;
  printf("{\n");
  printf("  \"schema_version\": %d,\n", CLONE_SCHEMA_VERSION);
  printf("  \"tool\": \"ny fmt --dupes\",\n");
  printf("  \"language\": \"%s\",\n", clone_lang_name(c->cfg.lang));
  printf("  \"profile\": \"%s\",\n", clone_mode_name(&c->cfg));
  printf("  \"analysis_view\": \"%s\",\n",
         c->stats.directives > 0 ? "raw-tokens-preprocessor-aware"
                                 : "raw-tokens-lexical");
  printf("  \"config\": {\"min_tokens\": %zu, \"min_lines\": %d, "
         "\"similarity\": %.4f, \"cross_language\": %s},\n",
         c->cfg.min_tokens, c->cfg.min_lines, c->cfg.similarity,
         c->cfg.cross_lang ? "true" : "false");
  printf("  \"files\": %d, \"c_files\": %d, \"ny_files\": %d,\n",
         c->stats.files, c->stats.c_files, c->stats.ny_files);
  printf("  \"functions\": %zu, \"tokens\": %zu, \"lines\": %zu, "
         "\"bytes\": %zu,\n",
         c->stats.functions, c->stats.tokens, c->stats.lines,
         c->stats.bytes);
  printf("  \"clone_families\": %zu, \"duplicate_functions\": %d, "
         "\"duplicate_tokens\": %zu, \"duplicate_lines\": %zu,\n",
         c->nfams, c->stats.duplicate_functions, c->stats.duplicate_tokens,
         c->stats.duplicate_lines);
  printf("  \"duplication_percent\": {\"tokens\": %.2f, \"lines\": %.2f},\n",
         dup_tok_pct, dup_line_pct);
  printf("  \"normalized\": %zu, \"duplicate_groups\": %zu, "
         "\"min_norm_len\": %d,\n",
         c->nfams, c->nfams, c->cfg.legacy_min_set ? c->cfg.legacy_min_bytes : 30);
  printf("  \"scan_completeness\": {\"files_read\": %d, "
         "\"read_failures\": %d, \"lex_errors\": %d, "
         "\"malformed_files\": %d, "
         "\"preprocessing_ambiguous_files\": %d, \"directives\": %d, "
         "\"candidate_cap_hits\": %d, "
         "\"hash_verification_failures\": %d, "
         "\"normalization_failures\": %d, "
         "\"subsumed_families\": %d, \"alignments\": %zu, "
         "\"near_fragments\": %zu, \"near_candidates\": %zu, "
         "\"incomplete\": %s},\n",
         c->stats.files, c->stats.read_failures, c->stats.lex_errors,
         c->stats.malformed_files, c->stats.preprocessing_ambiguous,
         c->stats.directives, c->stats.candidate_cap_hits,
         c->stats.hash_verification_failures,
         c->stats.normalization_failures, c->stats.subsumed,
         c->stats.alignments, c->stats.near_fragments,
         c->stats.near_candidates,
         (c->stats.incomplete || c->stats.candidate_cap_hits ||
          c->stats.read_failures || c->stats.malformed_files)
             ? "true"
             : "false");
  if (c->stats.baseline_new || c->stats.baseline_existing ||
      c->stats.baseline_resolved || c->stats.baseline_invalid || opts->baseline) {
    printf("  \"baseline\": {\"new\": %d, \"existing\": %d, "
           "\"resolved\": %d, \"invalid\": %s},\n",
           c->stats.baseline_new, c->stats.baseline_existing,
           c->stats.baseline_resolved,
           c->stats.baseline_invalid ? "true" : "false");
  }
  printf("  \"families\": [\n");
  size_t emitted = 0;
  for (size_t i = 0; i < c->nfams && emitted < shown; ++i) {
    const CFamily *f = &c->fams[i];
    if (f->nocc < 2)
      continue;
    if (emitted)
      printf(",\n");
    emitted++;
    printf("    {\"stable_id\": \"cf-%016llx\", \"clone_kind\": \"%s\", "
           "\"profile\": \"%s\", \"language\": \"%s\", "
           "\"similarity\": %.4f, \"confidence\": %.4f, "
           "\"token_count\": %zu, \"line_count\": %zu, "
           "\"occurrence_count\": %zu, \"affected_files\": %d, "
           "\"semantic_status\": ",
           (unsigned long long)f->stable_id, clone_kind_name(f->kind),
           clone_kind_name(f->kind), clone_family_lang(f), f->similarity,
           f->confidence, f->token_count, clone_family_line_count(f), f->nocc,
           f->affected_files);
    clone_json_str(f->semantic);
    printf(", \"score\": %ld, \"structure\": ", f->score);
    clone_json_str(f->structure);
    printf(", \"return_type_compatibility\": \"%s\", "
           "\"moved_statement_candidate\": %s, \"occurrences\": [",
           clone_return_compat(f), f->moved_candidate ? "true" : "false");
    clone_print_occurrences_json(c, f);
    printf("], \"differences\": [");
    for (int d = 0; d < f->ndiff; ++d) {
      if (d)
        printf(", ");
      clone_json_str(f->diffs[d]);
    }
    printf("], \"external_dependencies\": [");
    for (int d = 0; d < f->ndeps; ++d) {
      if (d)
        printf(", ");
      clone_json_str(f->deps[d]);
    }
    printf("], \"recommendation\": {\"action\": ");
    clone_json_str(f->action);
    printf(", \"summary\": ");
    clone_json_str(f->recommendation);
    printf(", \"risk\": \"%s\", \"verification\": "
           "\"build; run affected tests; compare outputs and error paths "
           "before applying any refactor\"}",
           f->risk);
    if (opts->emit) {
      printf(", \"refactor_plan\": {\"helper_boundary\": "
             "\"body token span %zu-%zu; parameters must cover every changed "
             "identifier and literal\", \"return_type_compatibility\": "
             "\"%s\", \"risks\": ",
             f->occs[0].tok_start, f->occs[0].tok_end, clone_return_compat(f));
      clone_json_str(f->recommendation);
      printf(", \"verification_steps\": [\"build the project\", "
             "\"run the tests that cover each occurrence\", "
             "\"compare results and error paths before and after\"]}");
    }
    printf("}");
  }
  printf("\n  ]\n}\n");
}

static void clone_print_text(const CClone *c, const CloneOptions *opts) {
  size_t shown = clone_shown_count(c, opts->limit);
  double dup_tok_pct = c->stats.tokens
                           ? 100.0 * (double)c->stats.duplicate_tokens /
                                 (double)c->stats.tokens
                           : 0.0;
  double dup_line_pct = c->stats.lines
                            ? 100.0 * (double)c->stats.duplicate_lines /
                                  (double)c->stats.lines
                            : 0.0;
  nyt_heading("Nytrix Clone Scan");
  nyt_kv("files", "%d (%d C, %d Ny)", c->stats.files, c->stats.c_files,
         c->stats.ny_files);
  nyt_kv("tokens", "%zu across %zu function(s)", c->stats.tokens,
         c->stats.functions);
  nyt_kv("clone families", "%zu (exact %d, renamed %d, literal %d, near %d, "
                            "structural %d)",
         c->nfams, c->stats.exact_families, c->stats.renamed_families,
         c->stats.literal_families, c->stats.near_families,
         c->stats.structural_families);
  nyt_kv("duplicate lines", "%zu of %zu (%.1f%%)",
         c->stats.duplicate_lines, c->stats.lines, dup_line_pct);
  nyt_kv("duplicate tokens", "%zu of %zu (%.1f%%)",
         c->stats.duplicate_tokens, c->stats.tokens, dup_tok_pct);
  nyt_kv("scan completeness", "%s",
         (c->stats.incomplete || c->stats.candidate_cap_hits ||
          c->stats.read_failures || c->stats.malformed_files)
             ? "partial (see warnings/incomplete flags)"
             : "complete");
  if (c->stats.read_failures)
    nyt_kv("read failures", "%d", c->stats.read_failures);
  if (c->stats.lex_errors || c->stats.malformed_files)
    nyt_kv("lexical issues", "%d error(s), %d malformed file(s)",
           c->stats.lex_errors, c->stats.malformed_files);
  if (c->stats.preprocessing_ambiguous)
    nyt_kv("preprocessing", "%d file(s) with ambiguous raw conditional "
                             "branches; view is raw-token",
           c->stats.preprocessing_ambiguous);
  if (opts->baseline)
    nyt_kv("baseline", "%d new, %d existing, %d resolved%s",
           c->stats.baseline_new, c->stats.baseline_existing,
           c->stats.baseline_resolved,
           c->stats.baseline_invalid ? " (invalid baseline)" : "");
  fputc('\n', stdout);
  if (!shown) {
    printf("%sNo clone families matched the configured thresholds.%s\n\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
    return;
  }
  size_t emitted = 0;
  for (size_t i = 0; i < c->nfams && emitted < shown; ++i) {
    const CFamily *f = &c->fams[i];
    if (f->nocc < 2)
      continue;
    emitted++;
    printf("%s#%zu%s  %s%s%s  %s%s%s  %s%.0f%%%s  %s\n", nyt_clr(NYT_BOLD),
           emitted, nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
           clone_kind_name(f->kind), nyt_clr(NYT_RESET), nyt_clr(NYT_GRAY),
           clone_family_lang(f), nyt_clr(NYT_RESET),
           nyt_clr(NYT_MAGENTA), f->similarity * 100.0, nyt_clr(NYT_RESET),
           f->semantic);
    for (size_t j = 0; j < f->nocc; ++j) {
      const COcc *o = &f->occs[j];
      printf("   %s:%d-%d", c->src[o->file].path, o->line_start, o->line_end);
      if (o->frag)
        printf("  %s", c->frags[o->frag - 1].name);
      fputc('\n', stdout);
    }
    printf("   structure · %s\n", f->structure);
    if (f->ndiff) {
      printf("   differences · " );
      int show = opts->explain ? f->ndiff : (f->ndiff < 2 ? f->ndiff : 2);
      for (int d = 0; d < show; ++d)
        printf("%s%s", d ? "; " : "", f->diffs[d]);
      if (show < f->ndiff)
        printf("; ...");
      fputc('\n', stdout);
    }
    printf("   recommendation · %s (%s risk)\n", f->action, f->risk);
    printf("   action · %s\n", f->recommendation);
    if (opts->explain) {
      printf("   return types · %s\n", clone_return_compat(f));
      if (f->ndeps) {
        printf("   external dependencies · ");
        for (int d = 0; d < f->ndeps; ++d)
          printf("%s%s", d ? ", " : "", f->deps[d]);
        fputc('\n', stdout);
      }
      printf("   stable id · cf-%016llx\n", (unsigned long long)f->stable_id);
    }
    fputc('\n', stdout);
  }
  if (shown < (size_t)c->nfams)
    printf("%s... %zu more family(s); rerun with --limit 0 to show all%s\n",
           nyt_clr(NYT_GRAY), c->nfams - shown, nyt_clr(NYT_RESET));
  if (!opts->emit)
    return;
  nyt_subheading("Refactoring Plans");
  emitted = 0;
  for (size_t i = 0; i < c->nfams && emitted < shown; ++i) {
    const CFamily *f = &c->fams[i];
    if (f->nocc < 2)
      continue;
    emitted++;
    printf("[%zu] %s  cf-%016llx\n", emitted, f->action,
           (unsigned long long)f->stable_id);
    printf("    originals · ");
    for (size_t j = 0; j < f->nocc; ++j) {
      const COcc *o = &f->occs[j];
      printf("%s%s:%d-%d", j ? ", " : "", c->src[o->file].path, o->line_start,
             o->line_end);
    }
    fputc('\n', stdout);
    printf("    matching regions · %zu to %zu tokens per occurrence\n",
           f->token_count, f->token_count);
    printf("    return types · %s\n", clone_return_compat(f));
    if (f->ndeps) {
      printf("    external dependencies · ");
      for (int d = 0; d < f->ndeps; ++d)
        printf("%s%s", d ? ", " : "", f->deps[d]);
      fputc('\n', stdout);
    }
    printf("    helper boundary · body token span %zu-%zu; parameters must "
           "cover every changed identifier and literal\n",
           f->occs[0].tok_start, f->occs[0].tok_end);
    if (f->ndiff) {
      printf("    changed tokens · ");
      for (int d = 0; d < f->ndiff; ++d)
        printf("%s%s", d ? "; " : "", f->diffs[d]);
      fputc('\n', stdout);
    }
    printf("    refactor risk · %s\n", f->risk);
    printf("    verification · build; run affected tests; compare outputs and "
           "error paths before applying\n\n");
  }
}

static void clone_ctx_free(CClone *c) {
  if (!c)
    return;
  for (size_t i = 0; i < c->nsrc; ++i) {
    free(c->src[i].path);
    free(c->src[i].content);
    free(c->src[i].dup_tok);
    free(c->src[i].dup_line);
  }
  free(c->src);
  free(c->toks.items);
  free(c->tok_file);
  free(c->syms.slots);
  free(c->syms.items);
  free(c->syms.pool);
  free(c->frags);
  free(c->frag_of_tok);
  free(c->exact_keys);
  free(c->shape_keys);
  free(c->literal_keys);
  free(c->near_keys);
  free(c->neutral_keys);
  free(c->matches);
  free(c->mset.slots);
  free(c->pairset.slots);
  for (size_t i = 0; i < c->nfams; ++i)
    free(c->fams[i].occs);
  free(c->fams);
  free(c->align_dp);
  free(c->align_row0);
  free(c->align_row1);
  memset(c, 0, sizeof(*c));
}

static int clone_mode_mask(const char *s) {
  if (!s || !*s || strcmp(s, "exact") == 0)
    return CM_EXACT;
  if (strcmp(s, "renamed") == 0)
    return CM_RENAMED;
  if (strcmp(s, "literal") == 0)
    return CM_LITERAL;
  if (strcmp(s, "near") == 0)
    return CM_NEAR;
  if (strcmp(s, "structural") == 0)
    return CM_STRUCTURAL;
  if (strcmp(s, "all") == 0)
    return CM_ALL;
  return 0;
}

int clone_run(const CloneOptions *opts) {
  if (!opts)
    return 2;
  int modes = clone_mode_mask(opts->match);
  if (!modes) {
    nyt_err("ny-fmt", "invalid --dupes-match value: %s", opts->match);
    return 2;
  }
  int lang = 0;
  if (opts->lang && *opts->lang && strcmp(opts->lang, "all") != 0) {
    if (strcmp(opts->lang, "c") == 0)
      lang = CLONE_LANG_C;
    else if (strcmp(opts->lang, "ny") == 0)
      lang = CLONE_LANG_NY;
    else {
      nyt_err("ny-fmt", "invalid --dupes-lang value: %s", opts->lang);
      return 2;
    }
  }
  if (opts->similarity < 0.0 || opts->similarity > 1.0) {
    nyt_err("ny-fmt", "--dupes-similarity must be between 0 and 1");
    return 2;
  }
  CClone c;
  memset(&c, 0, sizeof(c));
  c.cfg.modes = modes;
  c.cfg.lang = lang;
  c.cfg.min_tokens = opts->min_tokens ? opts->min_tokens : 40;
  if (c.cfg.min_tokens < 2)
    c.cfg.min_tokens = 2;
  c.cfg.min_lines = opts->min_lines > 0 ? opts->min_lines : 0;
  c.cfg.similarity = opts->similarity > 0.0 ? opts->similarity : 0.85;
  c.cfg.cross_lang = opts->cross_lang;
  c.cfg.legacy_min_bytes = opts->legacy_min_bytes;
  c.cfg.legacy_min_set = opts->legacy_min_set;
  c.verbose = opts->verbose;

  StrVec files;
  memset(&files, 0, sizeof(files));
  const char *roots[] = {"src", "lib"};
  const char *const *use_paths =
      opts->path_count ? opts->paths : (const char *const *)roots;
  size_t path_count = opts->path_count ? opts->path_count : 2;
  if (!fmt_collect_via_library(use_paths, path_count, "dupes", &files)) {
    nyt_err("ny-fmt", "source collection failed; clone scan aborted");
    sv_free(&files);
    return 1;
  }
  if (!opts->json)
    nyt_msg("DUPES", NYT_CYAN, "scanning %zu source file(s)", files.len);
  for (size_t i = 0; i < files.len; ++i) {
    int file_lang = clone_lang_for_path(files.items[i]);
    if (!file_lang || (lang && file_lang != lang))
      continue;
    if (!clone_add_source(&c, files.items[i], file_lang)) {
      nyt_err("ny-fmt", "out of memory while reading clone sources");
      sv_free(&files);
      clone_ctx_free(&c);
      return 1;
    }
  }
  sv_free(&files);
  if (c.stats.read_failures && !opts->json)
    nyt_warn("ny-fmt", "%d source file(s) could not be read",
             c.stats.read_failures);
  if (!clone_build_keys(&c, KP_EXACT, &c.exact_keys) ||
      !clone_build_keys(&c, KP_SHAPE, &c.shape_keys)) {
    nyt_err("ny-fmt", "out of memory while indexing clone tokens");
    clone_ctx_free(&c);
    return 1;
  }
  for (size_t f = 0; f < c.nsrc; ++f) {
    int ok = c.src[f].lang == CLONE_LANG_NY
                 ? clone_extract_ny_fragments(&c, (uint32_t)f)
                 : clone_extract_c_fragments(&c, (uint32_t)f);
    if (!ok) {
      nyt_err("ny-fmt", "out of memory while extracting functions");
      clone_ctx_free(&c);
      return 1;
    }
  }
  if (!clone_build_frag_index(&c))
    goto oom;
  clone_finalize_fragments(&c);
  if ((modes & CM_EXACT) && !clone_scan_windows(&c, c.exact_keys, CLK_EXACT))
    goto oom;
  if ((modes & CM_RENAMED) &&
      !clone_scan_windows(&c, c.shape_keys, CLK_RENAMED))
    goto oom;
  if (modes & CM_LITERAL) {
    if (!clone_ensure_keys(&c, &c.literal_keys, KP_LITERAL))
      goto oom;
    if (!clone_scan_windows(&c, c.literal_keys, CLK_LITERAL))
      goto oom;
  }
  if ((modes & CM_NEAR) && !clone_near_scan(&c, CLK_NEAR, 0))
    goto oom;
  if ((modes & CM_STRUCTURAL) && !clone_near_scan(&c, CLK_STRUCTURAL, 0))
    goto oom;
  if ((modes & CM_STRUCTURAL) && c.cfg.cross_lang) {
    if (!clone_near_scan(&c, CLK_STRUCTURAL, 1))
      goto oom;
  }
  if (!clone_build_families(&c))
    goto oom;
  clone_finalize_families(&c);
  if (opts->baseline)
    clone_apply_baseline(&c, opts->baseline);
  if (opts->json)
    clone_print_json(&c, opts);
  else
    clone_print_text(&c, opts);
  int rc = 0;
  if (opts->fail_on_new && c.stats.baseline_new > 0) {
    if (!opts->json)
      nyt_msg("DUPES", NYT_RED, "%d new clone family(s) against baseline",
              c.stats.baseline_new);
    rc = 1;
  }
  clone_ctx_free(&c);
  return rc;

oom:
  nyt_err("ny-fmt", "out of memory during clone analysis");
  clone_ctx_free(&c);
  return 1;
}

static void probe_syms_free(CSymTab *s) {
  free(s->slots);
  free(s->items);
  free(s->pool);
  memset(s, 0, sizeof(*s));
}

static void probe_toks_free(CTokVec *v) {
  free(v->items);
  memset(v, 0, sizeof(*v));
}

static int probe_prepare(CSymTab *syms, const char *src, int lang,
                         CTokVec *toks) {
  int errors = 0, directives = 0, malformed = 0;
  int ok = lang == CLONE_LANG_NY
               ? clone_lex_ny(src, strlen(src), syms, toks,
                              (uint32_t)CLONE_LANG_NY, &errors, &directives,
                              &malformed)
               : clone_lex_c(src, strlen(src), syms, toks,
                             (uint32_t)CLONE_LANG_C, &errors, &directives,
                             &malformed);
  if (!ok)
    return 0;
  clone_mark_flags(syms, toks->items, 0, toks->len);
  return 1;
}

static int probe_exact_tokens(CClone *c, const CTokVec *a, const CTokVec *b) {
  if (a->len != b->len)
    return 0;
  for (size_t i = 0; i < a->len; ++i)
    if (clone_exact_key(c, &a->items[i]) != clone_exact_key(c, &b->items[i]))
      return 0;
  return 1;
}

static int probe_renamed_tokens(CClone *c, const CTokVec *a,
                                const CTokVec *b) {
  if (a->len != b->len)
    return 0;
  CMap map;
  memset(&map, 0, sizeof(map));
  for (size_t i = 0; i < a->len; ++i)
    if (!clone_cmap_bind(&map, c, &a->items[i], &b->items[i]))
      return 0;
  return 1;
}

static int probe_pair(CClone *c, CSymTab *syms, const char *a, const char *b,
                      int lang, int renamed, int *exact_out) {
  CTokVec ta, tb;
  memset(&ta, 0, sizeof(ta));
  memset(&tb, 0, sizeof(tb));
  if (!probe_prepare(syms, a, lang, &ta) ||
      !probe_prepare(syms, b, lang, &tb)) {
    probe_toks_free(&ta);
    probe_toks_free(&tb);
    return -1;
  }
  c->syms = *syms;
  int exact = probe_exact_tokens(c, &ta, &tb);
  int result = renamed ? probe_renamed_tokens(c, &ta, &tb) : exact;
  if (exact_out)
    *exact_out = exact;
  probe_toks_free(&ta);
  probe_toks_free(&tb);
  return result;
}

static int clone_selftest_lexers(void) {
  struct {
    const char *a;
    const char *b;
    int lang;
    int renamed;
    int want;
  } cases[] = {
      {"return a + ++b;", "return a++ + b;", CLONE_LANG_C, 0, 0},
      {"return a+b;", "return a + b;", CLONE_LANG_C, 0, 1},
      {"return a + a + b;", "return x + x + y;", CLONE_LANG_C, 1, 1},
      {"return a + a + b;", "return x + y + y;", CLONE_LANG_C, 1, 0},
      {"return a + b;", "return a - b;", CLONE_LANG_C, 0, 0},
      {"if (a == b) x();", "if (a != b) x();", CLONE_LANG_C, 0, 0},
      {"if (a < b) x();", "if (a <= b) x();", CLONE_LANG_C, 0, 0},
      {"if (a && b) x();", "if (a || b) x();", CLONE_LANG_C, 0, 0},
      {"x = a + 1;", "x = a + 2;", CLONE_LANG_C, 0, 0},
      {"return a + a + b;", "return x + x + y;", CLONE_LANG_NY, 1, 1},
      {"return a + a + b;", "return x + y + y;", CLONE_LANG_NY, 1, 0},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    CSymTab syms;
    memset(&syms, 0, sizeof(syms));
    CClone c;
    memset(&c, 0, sizeof(c));
    c.syms = syms;
    int exact = 0;
    int got = probe_pair(&c, &syms, cases[i].a, cases[i].b, cases[i].lang,
                         cases[i].renamed, &exact);
    if (got != cases[i].want || got < 0) {
      fprintf(stderr,
              "ny-fmt clone selftest: lexer/normalization case %zu failed "
              "(got %d, want %d)\n",
              i, got, cases[i].want);
      probe_syms_free(&syms);
      return 1;
    }
    probe_syms_free(&syms);
  }
  return 0;
}

static int clone_selftest_scanner(void) {
  const char *src =
      "int normal(void) { return 1; }\n"
      "static inline int helper(int x) { return x; }\n"
      "int attributed(void)\n"
      "    __attribute__((noinline)) { return 1; }\n"
      "int (*factory(void))(int) { return 0; }\n"
      "int (paren)(void) { return 2; }\n";
  CScanFunctions f;
  memset(&f, 0, sizeof(f));
  if (!cscan_functions(src, strlen(src), &f) || f.len != 5) {
    fprintf(stderr, "ny-fmt clone selftest: C scanner count %zu != 5\n",
            f.len);
    cscan_functions_free(&f);
    return 1;
  }
  const char *names[] = {"normal", "helper", "attributed", "factory", "paren"};
  for (size_t i = 0; i < 5; ++i) {
    if (strcmp(f.items[i].name, names[i]) != 0) {
      fprintf(stderr,
              "ny-fmt clone selftest: C scanner name %zu = '%s', want '%s'\n",
              i, f.items[i].name, names[i]);
      cscan_functions_free(&f);
      return 1;
    }
  }
  if (!f.items[1].is_static || f.items[0].is_static) {
    fprintf(stderr, "ny-fmt clone selftest: static attribution failed\n");
    cscan_functions_free(&f);
    return 1;
  }
  for (size_t i = 0; i < f.len; ++i) {
    if (f.items[i].end_offset <= f.items[i].start_offset ||
        f.items[i].body_close_offset <= f.items[i].body_open_offset) {
      fprintf(stderr, "ny-fmt clone selftest: bad function span %zu\n", i);
      cscan_functions_free(&f);
      return 1;
    }
  }
  cscan_functions_free(&f);

  const char *noise =
      "const char *s = \"int fake(void) { }\";\n"
      "/* int nope(void) { } */\n"
      "int real(void) { if (s) { return 1; } return 0; }\n";
  memset(&f, 0, sizeof(f));
  if (!cscan_functions(noise, strlen(noise), &f) || f.len != 1 ||
      strcmp(f.items[0].name, "real") != 0) {
    fprintf(stderr, "ny-fmt clone selftest: scanner noise recovery failed\n");
    cscan_functions_free(&f);
    return 1;
  }
  cscan_functions_free(&f);

  const char *bad = "int broken(void) {\n";
  memset(&f, 0, sizeof(f));
  if (!cscan_functions(bad, strlen(bad), &f) || !f.malformed || f.len != 0) {
    fprintf(stderr, "ny-fmt clone selftest: scanner malformed recovery failed\n");
    cscan_functions_free(&f);
    return 1;
  }
  cscan_functions_free(&f);
  return 0;
}

static int clone_selftest_nytrix(void) {
  const char *samples[] = {
      "fn sum(int a, int b) int { a + b }\n",
      "fn add(int x, int y) int { x + y }\n",
      "fn identity(int x) int = x\n",
      "def f = fn(x){ x + 1 }\n",
      "; line comment\nfn f() int { 1 }\n",
      ";{ block comment with ; inside };\nfn f() int { 1 }\n",
      ";MARKER\nignored ; text\nMARKER;\nfn f() int { 1 }\n",
      "def s = \"a;b\"\nfn f() int { 1 }\n",
      "def s = \"\"\"hello\nworld\"\"\"\nfn f() int { 1 }\n",
      "def s = f\"x ${a + 1}\"\nfn f() int { 1 }\n",
      "#main\nfn main() int { 0 }\n",
      "fn f(int x) int { match x { case 1 { 1 } case _ { 0 } } }\n",
      "fn f() int { defer close() try { 1 } catch e { 0 } }\n",
  };
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    CSymTab syms;
    memset(&syms, 0, sizeof(syms));
    CTokVec toks;
    memset(&toks, 0, sizeof(toks));
    if (!probe_prepare(&syms, samples[i], CLONE_LANG_NY, &toks) ||
        toks.len == 0) {
      fprintf(stderr, "ny-fmt clone selftest: Ny lexer sample %zu failed\n",
              i);
      probe_toks_free(&toks);
      probe_syms_free(&syms);
      return 1;
    }
    probe_toks_free(&toks);
    probe_syms_free(&syms);
  }
  CSymTab syms;
  memset(&syms, 0, sizeof(syms));
  CTokVec toks;
  memset(&toks, 0, sizeof(toks));
  int errors = 0, directives = 0, malformed = 0;
  if (!clone_lex_ny("def s = \"unterminated\n", 20, &syms, &toks,
                    CLONE_LANG_NY, &errors, &directives, &malformed) ||
      errors == 0) {
    fprintf(stderr, "ny-fmt clone selftest: malformed Ny string accepted\n");
    probe_toks_free(&toks);
    probe_syms_free(&syms);
    return 1;
  }
  probe_toks_free(&toks);
  probe_syms_free(&syms);
  return 0;
}

static int clone_selftest_alignment(void) {
  CSymTab syms;
  memset(&syms, 0, sizeof(syms));
  CTokVec a, b;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  if (!probe_prepare(&syms, "int f(void){ int x=1; x++; return x; }",
                     CLONE_LANG_C, &a) ||
      !probe_prepare(&syms,
                     "int f(void){ int x=1; x++; x++; return x; }",
                     CLONE_LANG_C, &b)) {
    probe_toks_free(&a);
    probe_toks_free(&b);
    probe_syms_free(&syms);
    return 1;
  }
  CClone c;
  memset(&c, 0, sizeof(c));
  c.syms = syms;
  if (!clone_grow((void **)&c.toks.items, &c.toks.cap, a.len + b.len,
                  sizeof(CTok))) {
    probe_toks_free(&a);
    probe_toks_free(&b);
    probe_syms_free(&syms);
    return 1;
  }
  memcpy(c.toks.items, a.items, a.len * sizeof(CTok));
  memcpy(c.toks.items + a.len, b.items, b.len * sizeof(CTok));
  c.toks.len = a.len + b.len;
  uint64_t *keys = NULL;
  if (!clone_build_keys(&c, KP_NEAR, &keys)) {
    free(c.toks.items);
    probe_toks_free(&a);
    probe_toks_free(&b);
    probe_syms_free(&syms);
    return 1;
  }
  CDiff diffs[6];
  int moved = 0;
  int ndiff = 0;
  double sim = clone_align(&c, keys, keys, a.len, b.len, 0, a.len, 1, diffs, 6,
                           &moved, &ndiff);
  if (sim <= 0.5 || sim >= 1.0 || ndiff < 1) {
    fprintf(stderr,
            "ny-fmt clone selftest: near-miss similarity %.3f outside "
            "expected open interval\n",
            sim);
    free(keys);
    free(c.toks.items);
    free(c.align_dp);
    probe_toks_free(&a);
    probe_toks_free(&b);
    probe_syms_free(&syms);
    return 1;
  }
  free(keys);
  free(c.toks.items);
  free(c.align_dp);
  probe_toks_free(&a);
  probe_toks_free(&b);
  probe_syms_free(&syms);
  return 0;
}

static int clone_selftest_grouping(void) {
  CSymTab syms;
  memset(&syms, 0, sizeof(syms));
  CTokVec unit;
  memset(&unit, 0, sizeof(unit));
  if (!probe_prepare(&syms, "return a + b;", CLONE_LANG_C, &unit) ||
      unit.len == 0) {
    probe_toks_free(&unit);
    probe_syms_free(&syms);
    return 1;
  }
  CClone c;
  memset(&c, 0, sizeof(c));
  c.syms = syms;
  c.cfg.min_tokens = 2;
  c.nsrc = 1;
  c.src_cap = 1;
  c.src = (CSource *)calloc(1, sizeof(CSource));
  if (!c.src) {
    probe_toks_free(&unit);
    probe_syms_free(&syms);
    return 1;
  }
  c.src[0].path = (char *)malloc(8);
  if (!c.src[0].path) {
    free(c.src);
    probe_toks_free(&unit);
    probe_syms_free(&syms);
    return 1;
  }
  memcpy(c.src[0].path, "test.c", 7);
  c.src[0].lang = CLONE_LANG_C;
  c.src[0].line_count = 3;
  if (!clone_grow((void **)&c.toks.items, &c.toks.cap, unit.len * 3,
                  sizeof(CTok))) {
    free(c.src[0].path);
    free(c.src);
    probe_toks_free(&unit);
    probe_syms_free(&syms);
    return 1;
  }
  for (int rep = 0; rep < 3; ++rep)
    memcpy(c.toks.items + (size_t)rep * unit.len, unit.items,
           unit.len * sizeof(CTok));
  c.toks.len = unit.len * 3;
  c.src[0].tok_start = 0;
  c.src[0].tok_end = c.toks.len;
  c.tok_file = (uint32_t *)calloc(c.toks.len, sizeof(uint32_t));
  uint64_t *keys = NULL;
  if (!c.tok_file || !clone_build_keys(&c, KP_EXACT, &keys)) {
    free(c.tok_file);
    free(c.toks.items);
    free(c.src[0].path);
    free(c.src);
    probe_toks_free(&unit);
    probe_syms_free(&syms);
    return 1;
  }
  for (size_t i = 0; i < unit.len; ++i)
    c.toks.items[i].line = 1;
  for (size_t i = 0; i < unit.len; ++i)
    c.toks.items[unit.len + i].line = 2;
  for (size_t i = 0; i < unit.len; ++i)
    c.toks.items[2 * unit.len + i].line = 3;
  uint64_t content = clone_hash_keys(keys, unit.len);
  c.exact_keys = keys;
  if (!clone_record_match(&c, CLK_EXACT, 0, 0, unit.len, 0, unit.len,
                          unit.len * 2, 1.0, content, NULL, 0) ||
      !clone_record_match(&c, CLK_EXACT, 0, 0, unit.len, 0, unit.len * 2,
                          unit.len * 3, 1.0, content, NULL, 0) ||
      !clone_build_families(&c)) {
    free(keys);
    free(c.tok_file);
    free(c.toks.items);
    free(c.src[0].path);
    free(c.src);
    probe_toks_free(&unit);
    probe_syms_free(&syms);
    return 1;
  }
  int ok = c.nfams == 1 && c.fams[0].nocc == 3 &&
           c.fams[0].token_count == unit.len;
  if (!ok)
    fprintf(stderr,
            "ny-fmt clone selftest: grouping produced %zu family(s), "
            "%zu occurrence(s)\n",
            c.nfams, c.nfams ? c.fams[0].nocc : 0);
  for (size_t i = 0; i < c.nfams; ++i)
    free(c.fams[i].occs);
  free(c.fams);
  free(c.matches);
  free(c.mset.slots);
  free(c.pairset.slots);
  free(c.syms.slots);
  free(c.syms.items);
  free(c.syms.pool);
  free(keys);
  free(c.tok_file);
  free(c.toks.items);
  free(c.src[0].path);
  free(c.src);
  probe_toks_free(&unit);
  return ok ? 0 : 1;
}

static int clone_selftest_robustness(void) {
  CSymTab syms;
  memset(&syms, 0, sizeof(syms));
  CTokVec toks;
  memset(&toks, 0, sizeof(toks));
  if (!probe_prepare(&syms, "", CLONE_LANG_C, &toks) || toks.len != 0) {
    probe_toks_free(&toks);
    probe_syms_free(&syms);
    return 1;
  }
  probe_toks_free(&toks);
  const char nul_bytes[] = {'i', 'n', 't', ' ', '\0', 'x', ';'};
  int errors = 0, directives = 0, malformed = 0;
  if (!clone_lex_c(nul_bytes, sizeof(nul_bytes), &syms, &toks,
                   CLONE_LANG_C, &errors, &directives, &malformed)) {
    probe_toks_free(&toks);
    probe_syms_free(&syms);
    return 1;
  }
  probe_toks_free(&toks);
  char *long_id = (char *)malloc(5002);
  if (!long_id) {
    probe_syms_free(&syms);
    return 1;
  }
  memset(long_id, 'a', 5000);
  long_id[5000] = ';';
  long_id[5001] = '\0';
  if (!probe_prepare(&syms, long_id, CLONE_LANG_C, &toks) || toks.len != 2) {
    fprintf(stderr, "ny-fmt clone selftest: long identifier handling failed\n");
    free(long_id);
    probe_toks_free(&toks);
    probe_syms_free(&syms);
    return 1;
  }
  free(long_id);
  probe_toks_free(&toks);
  const char invalid_utf8[] = {(char)0xff, (char)0xfe, ' ', 'x', ';', '\0'};
  errors = 0;
  if (!clone_lex_c(invalid_utf8, sizeof(invalid_utf8) - 1, &syms, &toks,
                   CLONE_LANG_C, &errors, &directives, &malformed)) {
    probe_toks_free(&toks);
    probe_syms_free(&syms);
    return 1;
  }
  probe_toks_free(&toks);
  probe_syms_free(&syms);
  return 0;
}

int clone_selftest(void) {
  if (clone_selftest_lexers() != 0)
    return 1;
  if (clone_selftest_scanner() != 0)
    return 1;
  if (clone_selftest_nytrix() != 0)
    return 1;
  if (clone_selftest_alignment() != 0)
    return 1;
  if (clone_selftest_grouping() != 0)
    return 1;
  if (clone_selftest_robustness() != 0)
    return 1;
  printf("ny-fmt selftest: clone tokenization, exact/renamed matching, "
         "scanner attribution, near-miss alignment, grouping and robustness: "
         "ok\n");
  return 0;
}

