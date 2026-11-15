/* Unicode 18.0 table generator: updates lib/unicode.ny data tables and parser XID tables in place. */
#define _POSIX_C_SOURCE 200809L
#include "unicode.h"

#include "base/common.h"
#include "base/util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define UMAX 0x110000
#define UCD_VERSION "18.0.0"

typedef struct {
  uint32_t *data;
  size_t len, cap;
} u32vec;


typedef struct {
  char *data;
  size_t len, cap;
} strbuf;


typedef struct {
  uint32_t a, b, cp;
} triple;

typedef struct {
  uint32_t cp;
  uint8_t kind;
  u32vec seq;
} decomp_entry;

typedef struct {
  uint32_t cp;
  u32vec seq;
} seq_entry;

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q && n) {
    fprintf(stderr, "unicode: out of memory\n");
    exit(1);
  }
  return q;
}
static void *xmalloc(size_t n) { return xrealloc(NULL, n ? n : 1); }
static char *xstrdup(const char *s) {
  size_t n = strlen(s);
  char *p = xmalloc(n + 1);
  memcpy(p, s, n + 1);
  return p;
}

static void u32_push(u32vec *v, uint32_t x) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 64;
    v->data = xrealloc(v->data, v->cap * sizeof(*v->data));
  }
  v->data[v->len++] = x;
}
static void sb_init(strbuf *b) { memset(b, 0, sizeof(*b)); }
static void sb_reserve(strbuf *b, size_t add) {
  size_t need = b->len + add + 1;
  if (need <= b->cap) return;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < need) cap *= 2;
  b->data = xrealloc(b->data, cap);
  b->cap = cap;
}
static void sb_append(strbuf *b, const char *s, size_t n) {
  sb_reserve(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}
static void sb_puts(strbuf *b, const char *s) { sb_append(b, s, strlen(s)); }
static void sb_printf(strbuf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void sb_printf(strbuf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) { va_end(ap2); return; }
  sb_reserve(b, (size_t)n);
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
  b->len += (size_t)n;
  va_end(ap2);
}

static char *read_file_all(const char *path, size_t *out_len) {
  return ny_read_file_raw(path, out_len);
}

static bool write_if_changed_path(const char *path, const char *data, size_t len,
                                  bool *changed) {
  *changed = false;
  size_t old_len = 0;
  char *old = read_file_all(path, &old_len);
  if (old && old_len == len && memcmp(old, data, len) == 0) {
    free(old);
    return true;
  }
  free(old);
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  if (len && fwrite(data, 1, len, f) != len) { fclose(f); unlink(tmp); return false; }
  if (fclose(f) != 0) { unlink(tmp); return false; }
  if (rename(tmp, path) != 0) { unlink(tmp); return false; }
  *changed = true;
  return true;
}

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
  return s;
}

static uint32_t parse_hex(const char *s) {
  return (uint32_t)strtoul(s, NULL, 16);
}

static bool file_ok_hash(const char *path, const char *want_hex) {
  char got[65];
  if (!ny_sha256_file_hex(path, got)) return false;
  return strcmp(got, want_hex) == 0;
}



/* read a UCD member: try unzip -p */
static char *read_ucd_member(const char *zip_path, const char *member,
                             size_t *len) {
  int fds[2];
  if (pipe(fds) != 0) return NULL;
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    execlp("unzip", "unzip", "-p", zip_path, member, (char *)NULL);
    _exit(127);
  }
  close(fds[1]);
  if (pid < 0) { close(fds[0]); return NULL; }
  strbuf b; sb_init(&b);
  char buf[65536];
  for (;;) {
    ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n > 0) sb_append(&b, buf, (size_t)n);
    else break;
  }
  close(fds[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { free(b.data); return NULL; }
  if (len) *len = b.len;
  if (!b.data) { b.data = xstrdup(""); b.len = 0; }
  return b.data;
}

static int cmp_seq_entry(const void *a, const void *b) {
  uint32_t x = ((const seq_entry *)a)->cp, y = ((const seq_entry *)b)->cp;
  return (x > y) - (x < y);
}
static int cmp_decomp(const void *a, const void *b) {
  uint32_t x = ((const decomp_entry *)a)->cp, y = ((const decomp_entry *)b)->cp;
  return (x > y) - (x < y);
}
static int cmp_triple(const void *a, const void *b) {
  const triple *x = a, *y = b;
  if (x->a != y->a) return (x->a > y->a) - (x->a < y->a);
  if (x->b != y->b) return (x->b > y->b) - (x->b < y->b);
  return 0;
}

/* Ensure cached source files exist and match hashes */
static int ensure_sources(const char *root) {
  struct {
    const char *rel, *url, *sha;
  } files[] = {
    {"UCD.zip",
     "https://www.unicode.org/Public/18.0.0/ucd/UCD.zip",
     "7b3e555514060b92290d154f53655c5eb0fa62b16eb04c03434ff72d1a66a0d8"},
    {"security/confusables.txt",
     "https://www.unicode.org/Public/security/latest/confusables.txt",
     "6ed3ee967c9dfdf6677d563c9985182fbc50a2efb7d6059cd57b2e2ce18f5b92"},
    {"security/IdentifierStatus.txt",
     "https://www.unicode.org/Public/security/latest/IdentifierStatus.txt",
     "5863c7d99ca18f213c41c7318aa5528bebfb6d32ec0f1d5944e37192c119aebd"},
    {"security/IdentifierType.txt",
     "https://www.unicode.org/Public/security/latest/IdentifierType.txt",
     "fa24851acc669e58670e354e7b98a4ec8f52a809ec4f80524b6a60efdb868831"},
  };
  char base[PATH_MAX];
  snprintf(base, sizeof(base), "%s/etc/assets/dict/unicode/%s", root, UCD_VERSION);
  for (size_t i = 0; i < 4; i++) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", base, files[i].rel);
    if (file_ok_hash(path, files[i].sha)) continue;
    fprintf(stderr, "unicode: missing or invalid cached source: %s\n", path);
    fprintf(stderr, "unicode: expected sha256 %s\n", files[i].sha);
    fprintf(stderr, "unicode: fetch %s and place it at the path above\n", files[i].url);
    return -1;
  }
  /* update manifest if needed */
  char manifest_path[PATH_MAX];
  snprintf(manifest_path, sizeof(manifest_path),
           "%s/etc/assets/dict/unicode/manifest.json", root);
  strbuf m; sb_init(&m);
  sb_puts(&m, "{\n");
  sb_puts(&m, "  \"files\": [\n");
  for (size_t i = 0; i < 4; i++) {
    sb_printf(&m, "    {\n      \"path\": \"%s\",\n      \"sha256\": \"%s\",\n      \"url\": \"%s\"\n    }%s\n",
              files[i].rel, files[i].sha, files[i].url, i + 1 < 4 ? "," : "");
  }
  sb_puts(&m, "  ],\n");
  sb_puts(&m, "  \"generated\": [\n");
  sb_puts(&m, "    \"lib/unicode.ny\",\n");
  sb_puts(&m, "    \"src/code/lang/ny/parser/unicode.c\",\n");
  sb_puts(&m, "    \"src/code/lang/ny/parser/unicode.h\"\n");
  sb_puts(&m, "  ],\n");
  sb_puts(&m, "  \"source\": \"Unicode Character Database and UTS #39 security data\",\n");
  sb_printf(&m, "  \"version\": \"%s\"\n}\n", UCD_VERSION);
  bool mch = false;
  if (!write_if_changed_path(manifest_path, m.data ? m.data : "", m.len, &mch)) {
    free(m.data);
    return -1;
  }
  free(m.data);
  return 0;
}

/* ---------- parsing helpers ---------- */

static int gc_name_to_id(const char *s) {
  static const char *names[] = {
    "Cn","Lu","Ll","Lt","Lm","Lo","Mn","Mc","Me","Nd","Nl","No",
    "Pc","Pd","Ps","Pe","Pi","Pf","Po","Sm","Sc","Sk","So","Zs","Zl","Zp",
    "Cc","Cf","Cs","Co", NULL
  };
  for (int i = 0; names[i]; i++) if (strcmp(s, names[i]) == 0) return i;
  return 0;
}

static void parse_unicode_data(const char *text,
                               uint8_t *gc, uint8_t *ccc,
                               uint32_t *upper, uint32_t *lower, uint32_t *title,
                               decomp_entry **decomps, size_t *ndecomp, size_t *cdecomp) {
  *decomps = NULL; *ndecomp = 0; *cdecomp = 0;
  bool have_pending = false;
  uint32_t pending_cp = 0;
  int pending_gc = 0, pending_ccc = 0;
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[4096];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll);
    line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    /* split by ; */
    char *fields[16] = {0};
    int nf = 0;
    char *cur = line;
    while (nf < 16) {
      fields[nf++] = cur;
      char *semi = strchr(cur, ';');
      if (!semi) break;
      *semi = '\0';
      cur = semi + 1;
    }
    if (nf < 15) continue;
    uint32_t cp = parse_hex(fields[0]);
    const char *name = fields[1];
    size_t nlen = strlen(name);
    bool is_first = nlen >= 6 && strcmp(name + nlen - 6, "First>") == 0;
    bool is_last = nlen >= 5 && strcmp(name + nlen - 5, "Last>") == 0;
    if (is_first) {
      have_pending = true;
      pending_cp = cp;
      pending_gc = gc_name_to_id(fields[2]);
      pending_ccc = atoi(fields[3]);
      continue;
    }
    if (is_last && have_pending) {
      have_pending = false;
      for (uint32_t c = pending_cp; c <= cp && c < UMAX; c++) {
        gc[c] = (uint8_t)pending_gc;
        ccc[c] = (uint8_t)pending_ccc;
      }
      continue;
    }
    if (cp < UMAX) {
      gc[cp] = (uint8_t)gc_name_to_id(fields[2]);
      ccc[cp] = (uint8_t)atoi(fields[3]);
      if (fields[12] && *fields[12]) upper[cp] = parse_hex(fields[12]);
      if (fields[13] && *fields[13]) lower[cp] = parse_hex(fields[13]);
      if (fields[14] && *fields[14]) title[cp] = parse_hex(fields[14]);
      if (fields[5] && *fields[5]) {
        char *d = fields[5];
        while (*d == ' ' || *d == '\t') d++;
        if (*d) {
          uint8_t kind = 1;
          if (*d == '<') {
            char *end = strchr(d, '>');
            if (end) { kind = 2; d = end + 1; while (*d == ' ' || *d == '\t') d++; }
          }
          /* parse hex sequence */
          u32vec seq = {0};
          char *tok = d;
          while (*tok) {
            while (*tok == ' ' || *tok == '\t') tok++;
            if (!*tok) break;
            char *nxt = tok;
            while (*nxt && *nxt != ' ' && *nxt != '\t' && *nxt != '\r') nxt++;
            char saved = *nxt; *nxt = '\0';
            if (*tok) u32_push(&seq, parse_hex(tok));
            *nxt = saved;
            tok = nxt;
          }
          if (seq.len > 0) {
            if (*ndecomp == *cdecomp) {
              *cdecomp = *cdecomp ? *cdecomp * 2 : 256;
              *decomps = xrealloc(*decomps, *cdecomp * sizeof(**decomps));
            }
            (*decomps)[*ndecomp].cp = cp;
            (*decomps)[*ndecomp].kind = kind;
            (*decomps)[*ndecomp].seq = seq;
            (*ndecomp)++;
          } else {
            free(seq.data);
          }
        }
      }
    }
  }
  qsort(*decomps, *ndecomp, sizeof(**decomps), cmp_decomp);
}

typedef struct { uint32_t cp, to; } simple_map_entry;
static int cmp_simple(const void *a, const void *b) {
  uint32_t x = ((const simple_map_entry *)a)->cp, y = ((const simple_map_entry *)b)->cp;
  return (x > y) - (x < y);
}

static void parse_case_folding(const char *text,
                               simple_map_entry **simple, size_t *ns,
                               seq_entry **full, size_t *nf) {
  *simple = NULL; *ns = 0; *full = NULL; *nf = 0;
  size_t cs = 0, cf = 0;
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[2048];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    char *parts[4] = {0}; int np = 0;
    char *cur = s;
    while (np < 4) {
      parts[np++] = cur;
      char *semi = strchr(cur, ';');
      if (!semi) break;
      *semi = '\0'; cur = semi + 1;
    }
    if (np < 3) continue;
    for (int i = 0; i < np; i++) parts[i] = trim(parts[i]);
    uint32_t cp = parse_hex(parts[0]);
    char status = parts[1][0];
    /* parse seq */
    u32vec seq = {0};
    char *tok = parts[2];
    while (*tok) {
      while (*tok == ' ' || *tok == '\t') tok++;
      if (!*tok) break;
      char *nxt = tok;
      while (*nxt && *nxt != ' ' && *nxt != '\t') nxt++;
      char saved = *nxt; *nxt = '\0';
      u32_push(&seq, parse_hex(tok));
      *nxt = saved; tok = nxt;
    }
    if (seq.len == 0) { free(seq.data); continue; }
    if (status == 'C' || status == 'S') {
      if (*ns == cs) { cs = cs ? cs * 2 : 64; *simple = xrealloc(*simple, cs * sizeof(**simple)); }
      (*simple)[*ns].cp = cp; (*simple)[*ns].to = seq.data[0]; (*ns)++;
    }
    if (status == 'C' || status == 'F') {
      if (*nf == cf) { cf = cf ? cf * 2 : 64; *full = xrealloc(*full, cf * sizeof(**full)); }
      (*full)[*nf].cp = cp; (*full)[*nf].seq = seq; (*nf)++;
    } else {
      free(seq.data);
    }
  }
  qsort(*simple, *ns, sizeof(**simple), cmp_simple);
  qsort(*full, *nf, sizeof(**full), cmp_seq_entry);
}

static void parse_special_casing(const char *text,
                                 seq_entry **up, size_t *nup,
                                 seq_entry **lo, size_t *nlo,
                                 seq_entry **ti, size_t *nti) {
  *up = NULL; *nup = 0; *lo = NULL; *nlo = 0; *ti = NULL; *nti = 0;
  size_t cu = 0, cl = 0, ct = 0;
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[2048];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    char *parts[6] = {0}; int np = 0;
    char *cur = s;
    while (np < 6) {
      parts[np++] = cur;
      char *semi = strchr(cur, ';');
      if (!semi) break;
      *semi = '\0'; cur = semi + 1;
    }
    if (np < 5) continue;
    for (int i = 0; i < np; i++) parts[i] = trim(parts[i]);
    if (parts[4] && *parts[4]) continue; /* conditional */
    uint32_t cp = parse_hex(parts[0]);
    u32vec slo = {0}, sti = {0}, sup = {0};
    char *tok;
    tok = parts[1]; while (*tok) { while (*tok==' '||*tok=='\t') tok++; if(!*tok) break; char *n=tok; while(*n&&*n!=' '&&*n!='\t')n++; char sv=*n;*n='\0'; u32_push(&slo,parse_hex(tok)); *n=sv; tok=n; }
    tok = parts[2]; while (*tok) { while (*tok==' '||*tok=='\t') tok++; if(!*tok) break; char *n=tok; while(*n&&*n!=' '&&*n!='\t')n++; char sv=*n;*n='\0'; u32_push(&sti,parse_hex(tok)); *n=sv; tok=n; }
    tok = parts[3]; while (*tok) { while (*tok==' '||*tok=='\t') tok++; if(!*tok) break; char *n=tok; while(*n&&*n!=' '&&*n!='\t')n++; char sv=*n;*n='\0'; u32_push(&sup,parse_hex(tok)); *n=sv; tok=n; }
    if (*nlo == cl) { cl = cl?cl*2:32; *lo = xrealloc(*lo, cl*sizeof(**lo)); }
    (*lo)[*nlo].cp = cp; (*lo)[*nlo].seq = slo; (*nlo)++;
    if (*nti == ct) { ct = ct?ct*2:32; *ti = xrealloc(*ti, ct*sizeof(**ti)); }
    (*ti)[*nti].cp = cp; (*ti)[*nti].seq = sti; (*nti)++;
    if (*nup == cu) { cu = cu?cu*2:32; *up = xrealloc(*up, cu*sizeof(**up)); }
    (*up)[*nup].cp = cp; (*up)[*nup].seq = sup; (*nup)++;
  }
  qsort(*lo, *nlo, sizeof(**lo), cmp_seq_entry);
  qsort(*ti, *nti, sizeof(**ti), cmp_seq_entry);
  qsort(*up, *nup, sizeof(**up), cmp_seq_entry);
}

static void parse_security_confusables(const char *text,
                                       seq_entry **out, size_t *nout) {
  *out = NULL; *nout = 0; size_t cap = 0;
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[4096];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    char *parts[4] = {0}; int np = 0;
    char *cur = s;
    while (np < 4) {
      parts[np++] = cur;
      char *semi = strchr(cur, ';');
      if (!semi) break;
      *semi = '\0'; cur = semi + 1;
    }
    if (np < 2) continue;
    for (int i = 0; i < np; i++) parts[i] = trim(parts[i]);
    uint32_t cp = parse_hex(parts[0]);
    u32vec seq = {0};
    char *tok = parts[1];
    while (*tok) {
      while (*tok == ' ' || *tok == '\t') tok++;
      if (!*tok) break;
      char *nxt = tok;
      while (*nxt && *nxt != ' ' && *nxt != '\t') nxt++;
      char sv = *nxt; *nxt = '\0';
      u32_push(&seq, parse_hex(tok));
      *nxt = sv; tok = nxt;
    }
    if (seq.len == 0 || (seq.len == 1 && seq.data[0] == cp)) { free(seq.data); continue; }
    if (*nout == cap) { cap = cap ? cap * 2 : 64; *out = xrealloc(*out, cap * sizeof(**out)); }
    (*out)[*nout].cp = cp; (*out)[*nout].seq = seq; (*nout)++;
  }
  qsort(*out, *nout, sizeof(**out), cmp_seq_entry);
}

/* iterate lines of "XXXX..YYYY ; Name" or "XXXX ; Name" returning ranges */
static void apply_property_ranges(const char *text, const char *target_name,
                                  int mask, uint32_t *prop) {
  /* For DerivedCoreProperties, target_name can be "Alphabetic" etc or "InCB; Linker" matched specially */
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[2048];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    /* split fields */
    char *parts[4] = {0}; int np = 0;
    char *cur = s;
    while (np < 4) {
      parts[np++] = cur;
      char *semi = strchr(cur, ';');
      if (!semi) break;
      *semi = '\0'; cur = semi + 1;
    }
    if (np < 2) continue;
    for (int i = 0; i < np; i++) parts[i] = trim(parts[i]);
    /* check if first field is hex/range */
    bool is_range_field = false;
    for (char *q = parts[0]; *q; q++) {
      if (isxdigit((unsigned char)*q) || *q == '.' ) { is_range_field = true; }
      else { is_range_field = false; break; }
    }
    if (!is_range_field) continue;
    /* match name */
    bool match = false;
    int apply_mask = mask;
    if (strcmp(parts[1], "InCB") == 0 && np >= 3) {
      if (strcmp(parts[2], "Linker") == 0 && strcmp(target_name, "InCB; Linker") == 0) { match = true; apply_mask = 1024; }
      else if (strcmp(parts[2], "Consonant") == 0 && strcmp(target_name, "InCB; Consonant") == 0) { match = true; apply_mask = 2048; }
      else if (strcmp(parts[2], "Extend") == 0 && strcmp(target_name, "InCB; Extend") == 0) { match = true; apply_mask = 4096; }
    } else if (strcmp(parts[1], target_name) == 0) {
      match = true;
    }
    if (!match) continue;
    uint32_t lo, hi;
    char *dots = strstr(parts[0], "..");
    if (dots) { *dots = '\0'; lo = parse_hex(parts[0]); hi = parse_hex(dots + 2); }
    else { lo = hi = parse_hex(parts[0]); }
    for (uint32_t cp = lo; cp <= hi && cp < UMAX; cp++) prop[cp] |= (uint32_t)apply_mask;
  }
}

static void apply_emoji_ep(const char *text, uint32_t *prop) {
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[2048];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    char *parts[3] = {0}; int np = 0;
    char *cur = s;
    while (np < 3) { parts[np++]=cur; char *semi=strchr(cur,';'); if(!semi) break; *semi='\0'; cur=semi+1; }
    if (np < 2) continue;
    for (int i=0;i<np;i++) parts[i]=trim(parts[i]);
    if (strcmp(parts[1], "Extended_Pictographic") != 0) continue;
    uint32_t lo, hi;
    char *dots = strstr(parts[0], "..");
    if (dots) { *dots='\0'; lo=parse_hex(parts[0]); hi=parse_hex(dots+2); } else { lo=hi=parse_hex(parts[0]); }
    for (uint32_t cp=lo; cp<=hi && cp<UMAX; cp++) prop[cp] |= 512;
  }
}

/* scripts */

static void parse_property_value_aliases(const char *text,
                                         char ***script_names, int **script_ids, size_t *nsc) {
  *script_names = NULL; *script_ids = NULL; *nsc = 0;
  size_t cap = 0;
  int next_id = 1;
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[1024];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    if (strncmp(s, "sc ;", 4) != 0) continue;
    char *parts[5] = {0}; int np = 0;
    char *cur = s;
    while (np < 5) { parts[np++]=cur; char *semi=strchr(cur,';'); if(!semi) break; *semi='\0'; cur=semi+1; }
    if (np < 3) continue;
    for (int i=0;i<np;i++) parts[i]=trim(parts[i]);
    /* parts[2] is long name */
    const char *name = parts[2];
    bool exists = false;
    for (size_t i=0;i<*nsc;i++) if (strcmp((*script_names)[i], name)==0) { exists=true; break; }
    if (exists) continue;
    if (*nsc == cap) { cap = cap?cap*2:32; *script_names = xrealloc(*script_names, cap*sizeof(char*)); *script_ids = xrealloc(*script_ids, cap*sizeof(int)); }
    (*script_names)[*nsc] = xstrdup(name);
    (*script_ids)[*nsc] = next_id++;
    (*nsc)++;
  }
}

static int script_id_for(char **names, int *ids, size_t n, const char *name) {
  for (size_t i=0;i<n;i++) if (strcmp(names[i], name)==0) return ids[i];
  return 0;
}

static void apply_scripts(const char *text, char **names, int *ids, size_t nsc, uint16_t *script) {
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[1024];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    char *parts[3] = {0}; int np = 0;
    char *cur = s;
    while (np < 3) { parts[np++]=cur; char *semi=strchr(cur,';'); if(!semi) break; *semi='\0'; cur=semi+1; }
    if (np < 2) continue;
    for (int i=0;i<np;i++) parts[i]=trim(parts[i]);
    bool is_range = false;
    for (char *q=parts[0]; *q; q++) { if(!isxdigit((unsigned char)*q) && *q!='.') {is_range=false; goto skip;} is_range=true; }
    if (!is_range) { skip: continue; }
    uint32_t lo, hi;
    char *dots = strstr(parts[0], "..");
    if (dots) { *dots='\0'; lo=parse_hex(parts[0]); hi=parse_hex(dots+2); } else { lo=hi=parse_hex(parts[0]); }
    int sid = script_id_for(names, ids, nsc, parts[1]);
    for (uint32_t cp=lo; cp<=hi && cp<UMAX; cp++) script[cp] = (uint16_t)sid;
  }
}

static int break_name_id(const char *name, const char **names, int n) {
  for (int i=0;i<n;i++) if (strcmp(name,names[i])==0) return i;
  return 0;
}

static void parse_break_property(const char *text, uint8_t *arr, const char **names, int nnames) {
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[1024];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    char *parts[3] = {0}; int np = 0;
    char *cur = s;
    while (np < 3) { parts[np++]=cur; char *semi=strchr(cur,';'); if(!semi) break; *semi='\0'; cur=semi+1; }
    if (np < 2) continue;
    for (int i=0;i<np;i++) parts[i]=trim(parts[i]);
    bool ok=true; for(char *q=parts[0];*q;q++) if(!isxdigit((unsigned char)*q)&&*q!='.'){ok=false;break;}
    if(!ok) continue;
    uint32_t lo, hi; char *dots=strstr(parts[0],"..");
    if(dots){*dots='\0';lo=parse_hex(parts[0]);hi=parse_hex(dots+2);} else {lo=hi=parse_hex(parts[0]);}
    int val = break_name_id(parts[1], names, nnames);
    for(uint32_t cp=lo; cp<=hi && cp<UMAX; cp++) arr[cp] = (uint8_t)val;
  }
}

static void parse_id_type(const char *text, uint8_t *arr) {
  const char *names[] = {"", "Not_XID","Not_NFKC","Exclusion","Obsolete","Technical","Uncommon_Use"};
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[1024];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    char *parts[3] = {0}; int np = 0;
    char *cur = s;
    while (np < 3) { parts[np++]=cur; char *semi=strchr(cur,';'); if(!semi) break; *semi='\0'; cur=semi+1; }
    if (np < 2) continue;
    for (int i=0;i<np;i++) parts[i]=trim(parts[i]);
    bool ok=true; for(char *q=parts[0];*q;q++) if(!isxdigit((unsigned char)*q)&&*q!='.'){ok=false;break;}
    if(!ok) continue;
    uint32_t lo, hi; char *dots=strstr(parts[0],"..");
    if(dots){*dots='\0';lo=parse_hex(parts[0]);hi=parse_hex(dots+2);} else {lo=hi=parse_hex(parts[0]);}
    int val = 0;
    for (int i=1;i<=6;i++) if(strcmp(parts[1],names[i])==0){val=i;break;}
    for(uint32_t cp=lo; cp<=hi && cp<UMAX; cp++) arr[cp] = (uint8_t)val;
  }
}

static void parse_id_status(const char *text, bool *allowed) {
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);
    char line[1024];
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = '\0';
    if (eol) p = eol + 1; else p += ll;
    char *hash = strchr(line, '#'); if (hash) *hash = '\0';
    char *s = trim(line); if (!*s) continue;
    char *parts[3] = {0}; int np = 0;
    char *cur = s;
    while (np < 3) { parts[np++]=cur; char *semi=strchr(cur,';'); if(!semi) break; *semi='\0'; cur=semi+1; }
    if (np < 2) continue;
    for (int i=0;i<np;i++) parts[i]=trim(parts[i]);
    if (strcmp(parts[1], "Allowed") != 0) continue;
    bool ok=true; for(char *q=parts[0];*q;q++) if(!isxdigit((unsigned char)*q)&&*q!='.'){ok=false;break;}
    if(!ok) continue;
    uint32_t lo, hi; char *dots=strstr(parts[0],"..");
    if(dots){*dots='\0';lo=parse_hex(parts[0]);hi=parse_hex(dots+2);} else {lo=hi=parse_hex(parts[0]);}
    for(uint32_t cp=lo; cp<=hi && cp<UMAX; cp++) allowed[cp] = true;
  }
}

/* ---------- compression ---------- */

static void compress_ranges_u8(const uint8_t *arr, uint32_t *dflt_opt_ignored,
                               u32vec *starts, u32vec *vals) {
  (void)dflt_opt_ignored;
  starts->len = 0; vals->len = 0;
  size_t i = 0, n = UMAX;
  while (i < n) {
    uint8_t v = arr[i];
    size_t j = i + 1;
    while (j < n && arr[j] == v) j++;
    u32_push(starts, (uint32_t)i);
    u32_push(vals, (uint32_t)v);
    i = j;
  }
}
static void compress_ranges_u32(const uint32_t *arr, u32vec *starts, u32vec *vals) {
  starts->len = 0; vals->len = 0;
  size_t i = 0, n = UMAX;
  while (i < n) {
    uint32_t v = arr[i];
    size_t j = i + 1;
    while (j < n && arr[j] == v) j++;
    u32_push(starts, (uint32_t)i);
    u32_push(vals, v);
    i = j;
  }
}
static void compress_ranges_u16(const uint16_t *arr, u32vec *starts, u32vec *vals) {
  starts->len = 0; vals->len = 0;
  size_t i = 0, n = UMAX;
  while (i < n) {
    uint16_t v = arr[i];
    size_t j = i + 1;
    while (j < n && arr[j] == v) j++;
    u32_push(starts, (uint32_t)i);
    u32_push(vals, (uint32_t)v);
    i = j;
  }
}
static void compress_binary(const bool *arr, u32vec *pairs) {
  pairs->len = 0;
  size_t i = 0, n = UMAX;
  while (i < n) {
    if (!arr[i]) { i++; continue; }
    size_t j = i + 1;
    while (j < n && arr[j]) j++;
    u32_push(pairs, (uint32_t)i);
    u32_push(pairs, (uint32_t)(j - 1));
    i = j;
  }
}

static void simple_map_from_array(const uint32_t *arr, u32vec *starts, u32vec *vals) {
  starts->len = 0; vals->len = 0;
  for (uint32_t cp = 0; cp < UMAX; cp++) {
    if (arr[cp] != cp) { u32_push(starts, cp); u32_push(vals, arr[cp]); }
  }
}
static void simple_map_from_entries(const simple_map_entry *ents, size_t n,
                                    u32vec *starts, u32vec *vals) {
  starts->len = 0; vals->len = 0;
  for (size_t i = 0; i < n; i++) {
    if (ents[i].cp != ents[i].to) { u32_push(starts, ents[i].cp); u32_push(vals, ents[i].to); }
  }
}


static void seq_table_build(seq_entry *ents, size_t n,
                            u32vec *starts, u32vec *offs, u32vec *lens, u32vec *pool) {
  starts->len = offs->len = lens->len = pool->len = 0;
  for (size_t i = 0; i < n; i++) {
    u32_push(starts, ents[i].cp);
    u32_push(offs, (uint32_t)pool->len);
    u32_push(lens, (uint32_t)ents[i].seq.len);
    for (size_t j = 0; j < ents[i].seq.len; j++) u32_push(pool, ents[i].seq.data[j]);
  }
}

static void ny_list_u32(strbuf *b, const char *name, const uint32_t *vals, size_t n) {
  sb_printf(b, "def list<int> %s = [", name);
  if (n == 0) {
    sb_puts(b, "]\n");
    return;
  }
  sb_puts(b, "\n   ");
  for (size_t i = 0; i < n; i++) {
    if (i > 0) {
      sb_puts(b, ", ");
      if (i % 24 == 0) sb_puts(b, "\n   ");
    }
    sb_printf(b, "%u", vals[i]);
  }
  sb_puts(b, "\n]\n");
}

static void ny_list_ascii_u8(strbuf *b, const char *name, const uint8_t *vals) {
  uint32_t tmp[128];
  for (int i = 0; i < 128; i++) tmp[i] = vals[i];
  ny_list_u32(b, name, tmp, 128);
}

static void ny_list_ascii_u32(strbuf *b, const char *name, const uint32_t *vals) {
  uint32_t tmp[128];
  for (int i = 0; i < 128; i++) tmp[i] = vals[i];
  ny_list_u32(b, name, tmp, 128);
}


static int generate_unicode(const char *root) {
  if (ensure_sources(root) != 0) return 1;
  char ucd_zip[PATH_MAX], sec_dir[PATH_MAX];
  snprintf(ucd_zip, sizeof(ucd_zip), "%s/etc/assets/dict/unicode/%s/UCD.zip", root, UCD_VERSION);
  snprintf(sec_dir, sizeof(sec_dir), "%s/etc/assets/dict/unicode/%s/security", root, UCD_VERSION);

  /* read UCD members */
  size_t len_ud=0,len_dcp=0,len_pl=0,len_emoji=0,len_pva=0,len_scr=0,len_gb=0,len_wb=0,len_sb=0,len_ce=0,len_cf=0,len_sc=0;
  char *ud = read_ucd_member(ucd_zip, "UnicodeData.txt", &len_ud);
  char *dcp = read_ucd_member(ucd_zip, "DerivedCoreProperties.txt", &len_dcp);
  char *pl = read_ucd_member(ucd_zip, "PropList.txt", &len_pl);
  char *emoji = read_ucd_member(ucd_zip, "emoji/emoji-data.txt", &len_emoji);
  char *pva = read_ucd_member(ucd_zip, "PropertyValueAliases.txt", &len_pva);
  char *scr = read_ucd_member(ucd_zip, "Scripts.txt", &len_scr);
  char *gbp = read_ucd_member(ucd_zip, "auxiliary/GraphemeBreakProperty.txt", &len_gb);
  char *wbp = read_ucd_member(ucd_zip, "auxiliary/WordBreakProperty.txt", &len_wb);
  char *sbp = read_ucd_member(ucd_zip, "auxiliary/SentenceBreakProperty.txt", &len_sb);
  char *cex = read_ucd_member(ucd_zip, "CompositionExclusions.txt", &len_ce);
  char *cff = read_ucd_member(ucd_zip, "CaseFolding.txt", &len_cf);
  char *spc = read_ucd_member(ucd_zip, "SpecialCasing.txt", &len_sc);
  if (!ud||!dcp||!pl||!emoji||!pva||!scr||!gbp||!wbp||!sbp||!cex||!cff||!spc) {
    fprintf(stderr, "unicode: failed to read UCD members from %s (need unzip)\n", ucd_zip);
    return 1;
  }

  uint8_t *gc = xmalloc(UMAX);
  uint8_t *ccc = xmalloc(UMAX);
  uint32_t *upper = xmalloc(UMAX * sizeof(uint32_t));
  uint32_t *lower = xmalloc(UMAX * sizeof(uint32_t));
  uint32_t *title = xmalloc(UMAX * sizeof(uint32_t));
  uint32_t *prop = xmalloc(UMAX * sizeof(uint32_t));
  uint16_t *script = xmalloc(UMAX * sizeof(uint16_t));
  uint8_t *gb = xmalloc(UMAX);
  uint8_t *wb = xmalloc(UMAX);
  uint8_t *sb = xmalloc(UMAX);
  uint8_t *id_type = xmalloc(UMAX);
  bool *id_allowed = xmalloc(UMAX * sizeof(bool));
  memset(gc, 0, UMAX);
  memset(ccc, 0, UMAX);
  for (uint32_t i=0;i<UMAX;i++){ upper[i]=lower[i]=title[i]=i; prop[i]=0; script[i]=0; }
  memset(gb,0,UMAX); memset(wb,0,UMAX); memset(sb,0,UMAX); memset(id_type,0,UMAX);
  memset(id_allowed,0,UMAX*sizeof(bool));

  decomp_entry *decomps = NULL; size_t ndecomp = 0, cdecomp = 0;
  parse_unicode_data(ud, gc, ccc, upper, lower, title, &decomps, &ndecomp, &cdecomp);

  simple_map_entry *simple_fold = NULL; size_t ns_fold = 0;
  seq_entry *full_fold = NULL; size_t nf_fold = 0;
  parse_case_folding(cff, &simple_fold, &ns_fold, &full_fold, &nf_fold);
  seq_entry *sp_up = NULL, *sp_lo = NULL, *sp_ti = NULL;
  size_t nspu=0, nspl=0, nspt=0;
  parse_special_casing(spc, &sp_up, &nspu, &sp_lo, &nspl, &sp_ti, &nspt);

  /* properties */
  apply_property_ranges(dcp, "Alphabetic", 1, prop);
  apply_property_ranges(dcp, "Uppercase", 8, prop);
  apply_property_ranges(dcp, "Lowercase", 16, prop);
  apply_property_ranges(dcp, "XID_Start", 32, prop);
  apply_property_ranges(dcp, "XID_Continue", 64, prop);
  apply_property_ranges(dcp, "Default_Ignorable_Code_Point", 128, prop);
  apply_property_ranges(dcp, "Cased", 16384, prop);
  apply_property_ranges(dcp, "Case_Ignorable", 32768, prop);
  apply_property_ranges(dcp, "InCB; Linker", 1024, prop);
  apply_property_ranges(dcp, "InCB; Consonant", 2048, prop);
  apply_property_ranges(dcp, "InCB; Extend", 4096, prop);
  apply_property_ranges(pl, "White_Space", 4, prop);
  apply_property_ranges(pl, "Bidi_Control", 256, prop);
  apply_emoji_ep(emoji, prop);
  for (uint32_t cp=0; cp<UMAX; cp++) {
    if (gc[cp] >= 9 && gc[cp] <= 11) prop[cp] |= 2;
    if (gc[cp] != 0) prop[cp] |= 8192;
  }

  /* scripts */
  char **sc_names = NULL; int *sc_ids = NULL; size_t nsc = 0;
  parse_property_value_aliases(pva, &sc_names, &sc_ids, &nsc);
  /* build ordered script name list by id */
  int max_sid = 0; for(size_t i=0;i<nsc;i++) if(sc_ids[i]>max_sid) max_sid=sc_ids[i];
  char **script_name_list = xmalloc((size_t)(max_sid + 1) * sizeof(char*));
  for (int i=0;i<=max_sid;i++) script_name_list[i] = "Unknown";
  for (size_t i=0;i<nsc;i++) script_name_list[sc_ids[i]] = sc_names[i];
  apply_scripts(scr, sc_names, sc_ids, nsc, script);

  /* breaks */
  const char *gb_names_arr[] = {"Other","CR","LF","Control","Extend","ZWJ","Regional_Indicator","Prepend","SpacingMark","L","V","T","LV","LVT"};
  const char *wb_names_arr[] = {"Other","CR","LF","Newline","Extend","ZWJ","Regional_Indicator","Format","Katakana","Hebrew_Letter","ALetter","Single_Quote","Double_Quote","MidNumLet","MidLetter","MidNum","Numeric","ExtendNumLet","WSegSpace"};
  const char *sb_names_arr[] = {"Other","CR","LF","Extend","Sep","Format","Sp","Lower","Upper","OLetter","Numeric","ATerm","SContinue","STerm","Close"};
  parse_break_property(gbp, gb, gb_names_arr, 14);
  parse_break_property(wbp, wb, wb_names_arr, 19);
  parse_break_property(sbp, sb, sb_names_arr, 15);

  /* composition exclusions */
  bool *excl = xmalloc(UMAX * sizeof(bool));
  memset(excl, 0, UMAX * sizeof(bool));
  {
    const char *p = cex;
    while (*p) {
      const char *eol = strchr(p,'\n'); size_t ll = eol?(size_t)(eol-p):strlen(p);
      char line[256]; if(ll>=sizeof(line)) ll=sizeof(line)-1; memcpy(line,p,ll); line[ll]='\0';
      if(eol) p=eol+1; else p+=ll;
      char *hash=strchr(line,'#'); if(hash)*hash='\0'; char *s=trim(line); if(!*s) continue;
      if (strstr(s,"..")) continue;
      uint32_t cpv = parse_hex(s); if (cpv<UMAX) excl[cpv]=true;
    }
  }
  /* composition map: (a,b)->cp */
  triple *comp = NULL; size_t ncomp = 0, ccomp = 0;
  for (size_t i=0;i<ndecomp;i++) {
    if (decomps[i].kind == 1 && decomps[i].seq.len == 2 && !excl[decomps[i].cp]) {
      uint32_t a = decomps[i].seq.data[0], b = decomps[i].seq.data[1];
      if (ccc[a] == 0 && !(decomps[i].cp >= 0xAC00 && decomps[i].cp <= 0xD7A3)) {
        if (ncomp == ccomp) { ccomp = ccomp?ccomp*2:256; comp = xrealloc(comp, ccomp*sizeof(*comp)); }
        comp[ncomp].a = a; comp[ncomp].b = b; comp[ncomp].cp = decomps[i].cp; ncomp++;
      }
    }
  }
  qsort(comp, ncomp, sizeof(*comp), cmp_triple);
  /* build by_comp grouped by base a */
  u32vec comp_bases={0}, comp_offs={0}, comp_combs={0}, comp_vals={0};
  size_t ci = 0;
  while (ci < ncomp) {
    uint32_t a = comp[ci].a;
    u32_push(&comp_bases, a);
    u32_push(&comp_offs, (uint32_t)comp_combs.len);
    while (ci < ncomp && comp[ci].a == a) {
      u32_push(&comp_combs, comp[ci].b);
      u32_push(&comp_vals, comp[ci].cp);
      ci++;
    }
  }

  /* full case mappings */
  /* build simple fold lookup */
  /* full_upper/lower/title from special casing where not simple one-to-one same as basic */
  seq_entry *full_upper = NULL, *full_lower = NULL, *full_title = NULL;
  size_t nfu=0, nfl=0, nft=0, cfu=0, cfl=0, cft=0;
  for (size_t i=0;i<nspu;i++) {
    if (!(sp_up[i].seq.len==1 && sp_up[i].seq.data[0]==upper[sp_up[i].cp])) {
      if(nfu==cfu){cfu=cfu?cfu*2:16;full_upper=xrealloc(full_upper,cfu*sizeof(*full_upper));}
      full_upper[nfu++]=sp_up[i];
    } else free(sp_up[i].seq.data);
  }
  for (size_t i=0;i<nspl;i++) {
    if (!(sp_lo[i].seq.len==1 && sp_lo[i].seq.data[0]==lower[sp_lo[i].cp])) {
      if(nfl==cfl){cfl=cfl?cfl*2:16;full_lower=xrealloc(full_lower,cfl*sizeof(*full_lower));}
      full_lower[nfl++]=sp_lo[i];
    } else free(sp_lo[i].seq.data);
  }
  for (size_t i=0;i<nspt;i++) {
    if (!(sp_ti[i].seq.len==1 && sp_ti[i].seq.data[0]==title[sp_ti[i].cp])) {
      if(nft==cft){cft=cft?cft*2:16;full_title=xrealloc(full_title,cft*sizeof(*full_title));}
      full_title[nft++]=sp_ti[i];
    } else free(sp_ti[i].seq.data);
  }
  /* full_fold: keep only non-simple entries */
  seq_entry *full_fold2 = NULL; size_t nff=0, cff2=0;
  for (size_t i=0;i<nf_fold;i++) {
    /* find in simple_fold */
    size_t lo=0,hi=ns_fold; uint32_t sval=full_fold[i].cp;
    while(lo<hi){size_t mid=(lo+hi)/2; if(simple_fold[mid].cp<full_fold[i].cp) lo=mid+1; else hi=mid;}
    if(lo<ns_fold && simple_fold[lo].cp==full_fold[i].cp){ sval=simple_fold[lo].to; }
    else { sval = full_fold[i].cp; }
    if (!(full_fold[i].seq.len==1 && full_fold[i].seq.data[0]==sval)) {
      if(nff==cff2){cff2=cff2?cff2*2:32;full_fold2=xrealloc(full_fold2,cff2*sizeof(*full_fold2));}
      full_fold2[nff++]=full_fold[i];
    } else {
      free(full_fold[i].seq.data);
    }
  }

  /* security */
  char conf_path[PATH_MAX], idst_path[PATH_MAX], idty_path[PATH_MAX];
  snprintf(conf_path,sizeof(conf_path),"%s/confusables.txt",sec_dir);
  snprintf(idst_path,sizeof(idst_path),"%s/IdentifierStatus.txt",sec_dir);
  snprintf(idty_path,sizeof(idty_path),"%s/IdentifierType.txt",sec_dir);
  size_t _l1=0; char *conf_txt = read_file_all(conf_path, &_l1);
  size_t _l2=0; char *idst_txt = read_file_all(idst_path, &_l2);
  size_t _l3=0; char *idty_txt = read_file_all(idty_path, &_l3);
  if(!conf_txt||!idst_txt||!idty_txt){ fprintf(stderr,"unicode: failed to read security files\n"); return 1; }
  seq_entry *conf = NULL; size_t nconf=0;
  parse_security_confusables(conf_txt, &conf, &nconf);
  parse_id_status(idst_txt, id_allowed);
  parse_id_type(idty_txt, id_type);

  /* compress */
  u32vec gc_s={0},gc_v={0},ccc_s={0},ccc_v={0},prop_s={0},prop_v={0};
  u32vec scr_s={0},scr_v={0},gb_s={0},gb_v={0},wb_s={0},wb_v={0},sb_s={0},sb_v={0};
  u32vec id_s={0},id_v={0},id_pairs={0};
  compress_ranges_u8(gc,0,&gc_s,&gc_v);
  compress_ranges_u8(ccc,0,&ccc_s,&ccc_v);
  compress_ranges_u32(prop,&prop_s,&prop_v);
  compress_ranges_u16(script,&scr_s,&scr_v);
  compress_ranges_u8(gb,0,&gb_s,&gb_v);
  compress_ranges_u8(wb,0,&wb_s,&wb_v);
  compress_ranges_u8(sb,0,&sb_s,&sb_v);
  compress_ranges_u8(id_type,0,&id_s,&id_v);
  compress_binary(id_allowed,&id_pairs);

  u32vec up_s={0},up_v={0},lo_s={0},lo_v={0},ti_s={0},ti_v={0},fold_s={0},fold_v={0};
  simple_map_from_array(upper,&up_s,&up_v);
  simple_map_from_array(lower,&lo_s,&lo_v);
  simple_map_from_array(title,&ti_s,&ti_v);
  simple_map_from_entries(simple_fold, ns_fold, &fold_s, &fold_v);

  u32vec up_seq_s={0},up_seq_o={0},up_seq_l={0},up_seq_p={0};
  u32vec lo_seq_s={0},lo_seq_o={0},lo_seq_l={0},lo_seq_p={0};
  u32vec ti_seq_s={0},ti_seq_o={0},ti_seq_l={0},ti_seq_p={0};
  u32vec fold_seq_s={0},fold_seq_o={0},fold_seq_l={0},fold_seq_p={0};
  seq_table_build(full_upper,nfu,&up_seq_s,&up_seq_o,&up_seq_l,&up_seq_p);
  seq_table_build(full_lower,nfl,&lo_seq_s,&lo_seq_o,&lo_seq_l,&lo_seq_p);
  seq_table_build(full_title,nft,&ti_seq_s,&ti_seq_o,&ti_seq_l,&ti_seq_p);
  seq_table_build(full_fold2,nff,&fold_seq_s,&fold_seq_o,&fold_seq_l,&fold_seq_p);

  u32vec decomp_starts={0},decomp_flags={0},decomp_offs={0},decomp_lens={0},decomp_pool={0};
  for(size_t i=0;i<ndecomp;i++){
    u32_push(&decomp_starts,decomps[i].cp);
    u32_push(&decomp_flags,decomps[i].kind);
    u32_push(&decomp_offs,(uint32_t)decomp_pool.len);
    u32_push(&decomp_lens,(uint32_t)decomps[i].seq.len);
    for(size_t j=0;j<decomps[i].seq.len;j++) u32_push(&decomp_pool,decomps[i].seq.data[j]);
  }
  u32vec conf_starts={0},conf_offs={0},conf_lens={0},conf_pool={0};
  for(size_t i=0;i<nconf;i++){
    u32_push(&conf_starts,conf[i].cp);
    u32_push(&conf_offs,(uint32_t)conf_pool.len);
    u32_push(&conf_lens,(uint32_t)conf[i].seq.len);
    for(size_t j=0;j<conf[i].seq.len;j++) u32_push(&conf_pool,conf[i].seq.data[j]);
  }

  /* Regenerate the generated-data region in lib/unicode.ny in place.
   *
   * Region markers in lib/unicode.ny:
   *   ;; ny-make:unicode:data:begin
   *   ;; ny-make:unicode:data:end
   *
   * The header keywords line is kept first for source indexing; remaining
   * content before :data:begin and everything after :data:end are preserved.
   * Only the table definitions between the markers are rewritten. */
  char data_out_path[PATH_MAX];
  snprintf(data_out_path, sizeof(data_out_path), "%s/lib/unicode.ny", root);
  size_t old_len = 0;
  char *old_ny = read_file_all(data_out_path, &old_len);
  if (!old_ny) {
    fprintf(stderr, "unicode: failed to read %s\n", data_out_path);
    return 1;
  }

  const char *begin_mark = ";; ny-make:unicode:data:begin\n";
  const char *end_mark = ";; ny-make:unicode:data:end\n";
  char *data_begin = strstr(old_ny, begin_mark);
  char *data_end = strstr(old_ny, end_mark);
  if (!data_begin || !data_end || data_end <= data_begin) {
    fprintf(stderr, "unicode: could not locate generated-data markers in %s\n", data_out_path);
    free(old_ny);
    return 1;
  }
  data_begin += strlen(begin_mark);

  strbuf out;
  sb_init(&out);
  char *keywords = strstr(old_ny, ";; Keywords:");
  if (!keywords || keywords >= data_begin) {
    fprintf(stderr, "unicode: missing header keywords line in %s\n", data_out_path);
    free(old_ny);
    return 1;
  }
  char *keywords_end = memchr(keywords, '\n', (size_t)(data_begin - keywords));
  if (!keywords_end) {
    fprintf(stderr, "unicode: unterminated header keywords line in %s\n",
            data_out_path);
    free(old_ny);
    return 1;
  }
  if (keywords != old_ny) {
    sb_append(&out, keywords, (size_t)(keywords_end + 1 - keywords));
    sb_append(&out, old_ny, (size_t)(keywords - old_ny));
    sb_append(&out, keywords_end + 1,
              (size_t)(data_begin - (keywords_end + 1)));
  } else {
    sb_append(&out, old_ny, (size_t)(data_begin - old_ny));
  }

  ny_list_u32(&out, "_gc_starts", gc_s.data, gc_s.len);
  ny_list_u32(&out, "_gc_vals", gc_v.data, gc_v.len);
  ny_list_u32(&out, "_ccc_starts", ccc_s.data, ccc_s.len);
  ny_list_u32(&out, "_ccc_vals", ccc_v.data, ccc_v.len);
  ny_list_u32(&out, "_prop_starts", prop_s.data, prop_s.len);
  ny_list_u32(&out, "_prop_vals", prop_v.data, prop_v.len);
  ny_list_u32(&out, "_script_starts", scr_s.data, scr_s.len);
  ny_list_u32(&out, "_script_vals", scr_v.data, scr_v.len);
  ny_list_u32(&out, "_gb_starts", gb_s.data, gb_s.len);
  ny_list_u32(&out, "_gb_vals", gb_v.data, gb_v.len);
  ny_list_u32(&out, "_wb_starts", wb_s.data, wb_s.len);
  ny_list_u32(&out, "_wb_vals", wb_v.data, wb_v.len);
  ny_list_u32(&out, "_sb_starts", sb_s.data, sb_s.len);
  ny_list_u32(&out, "_sb_vals", sb_v.data, sb_v.len);
  ny_list_u32(&out, "_id_type_starts", id_s.data, id_s.len);
  ny_list_u32(&out, "_id_type_vals", id_v.data, id_v.len);
  ny_list_u32(&out, "_id_allowed", id_pairs.data, id_pairs.len);
  ny_list_u32(&out, "_comp_bases", comp_bases.data, comp_bases.len);
  ny_list_u32(&out, "_comp_offs", comp_offs.data, comp_offs.len);
  ny_list_u32(&out, "_comp_combs", comp_combs.data, comp_combs.len);
  ny_list_u32(&out, "_comp_vals", comp_vals.data, comp_vals.len);
  ny_list_u32(&out, "_decomp_starts", decomp_starts.data, decomp_starts.len);
  ny_list_u32(&out, "_decomp_flags", decomp_flags.data, decomp_flags.len);
  ny_list_u32(&out, "_decomp_offs", decomp_offs.data, decomp_offs.len);
  ny_list_u32(&out, "_decomp_lens", decomp_lens.data, decomp_lens.len);
  ny_list_u32(&out, "_decomp_pool", decomp_pool.data, decomp_pool.len);
  ny_list_u32(&out, "_conf_starts", conf_starts.data, conf_starts.len);
  ny_list_u32(&out, "_conf_offs", conf_offs.data, conf_offs.len);
  ny_list_u32(&out, "_conf_lens", conf_lens.data, conf_lens.len);
  ny_list_u32(&out, "_conf_pool", conf_pool.data, conf_pool.len);
  ny_list_u32(&out, "_up_starts", up_seq_s.data, up_seq_s.len);
  ny_list_u32(&out, "_up_offs", up_seq_o.data, up_seq_o.len);
  ny_list_u32(&out, "_up_lens", up_seq_l.data, up_seq_l.len);
  ny_list_u32(&out, "_up_pool", up_seq_p.data, up_seq_p.len);
  ny_list_u32(&out, "_lo_starts", lo_seq_s.data, lo_seq_s.len);
  ny_list_u32(&out, "_lo_offs", lo_seq_o.data, lo_seq_o.len);
  ny_list_u32(&out, "_lo_lens", lo_seq_l.data, lo_seq_l.len);
  ny_list_u32(&out, "_lo_pool", lo_seq_p.data, lo_seq_p.len);
  ny_list_u32(&out, "_ti_starts", ti_seq_s.data, ti_seq_s.len);
  ny_list_u32(&out, "_ti_offs", ti_seq_o.data, ti_seq_o.len);
  ny_list_u32(&out, "_ti_lens", ti_seq_l.data, ti_seq_l.len);
  ny_list_u32(&out, "_ti_pool", ti_seq_p.data, ti_seq_p.len);
  ny_list_u32(&out, "_fold_starts", fold_seq_s.data, fold_seq_s.len);
  ny_list_u32(&out, "_fold_offs", fold_seq_o.data, fold_seq_o.len);
  ny_list_u32(&out, "_fold_lens", fold_seq_l.data, fold_seq_l.len);
  ny_list_u32(&out, "_fold_pool", fold_seq_p.data, fold_seq_p.len);
  ny_list_u32(&out, "_up_map_starts", up_s.data, up_s.len);
  ny_list_u32(&out, "_up_map_vals", up_v.data, up_v.len);
  ny_list_u32(&out, "_lo_map_starts", lo_s.data, lo_s.len);
  ny_list_u32(&out, "_lo_map_vals", lo_v.data, lo_v.len);
  ny_list_u32(&out, "_ti_map_starts", ti_s.data, ti_s.len);
  ny_list_u32(&out, "_ti_map_vals", ti_v.data, ti_v.len);
  ny_list_u32(&out, "_fold_map_starts", fold_s.data, fold_s.len);
  ny_list_u32(&out, "_fold_map_vals", fold_v.data, fold_v.len);
  ny_list_ascii_u8(&out, "_ascii_gc", gc);
  ny_list_ascii_u32(&out, "_ascii_prop", prop);
  ny_list_ascii_u32(&out, "_ascii_up", upper);
  ny_list_ascii_u32(&out, "_ascii_lo", lower);
  ny_list_ascii_u32(&out, "_ascii_ti", title);
  sb_puts(&out, "def list<str> _script_names = [\n   ");
  for (int i = 0; i <= max_sid; i++) {
    if (i > 0) sb_puts(&out, ",\n   ");
    sb_printf(&out, "\"%s\"", script_name_list[i]);
  }
  sb_puts(&out, "\n]\n");
  sb_puts(&out, data_end);

  bool changed_data = false, changed_h = false, changed_c = false;
  if (!write_if_changed_path(data_out_path, out.data, out.len, &changed_data)) {
    fprintf(stderr, "unicode: failed to write %s\n", data_out_path);
    free(out.data);
    free(old_ny);
    return 1;
  }
  free(out.data);
  free(old_ny);

  /* generate XID tables for parser */
  bool *xid_start = xmalloc(UMAX*sizeof(bool));
  bool *xid_cont = xmalloc(UMAX*sizeof(bool));
  for(uint32_t cp=0; cp<UMAX; cp++){ xid_start[cp]=(prop[cp]&32)!=0; xid_cont[cp]=(prop[cp]&64)!=0; }
  u32vec start_pairs={0}, cont_pairs={0};
  compress_binary(xid_start, &start_pairs);
  compress_binary(xid_cont, &cont_pairs);

  char xid_h_path[PATH_MAX], xid_c_path[PATH_MAX];
  snprintf(xid_h_path,sizeof(xid_h_path),"%s/src/code/lang/ny/parser/unicode.h", root);
  snprintf(xid_c_path,sizeof(xid_c_path),"%s/src/code/lang/ny/parser/unicode.c", root);

  strbuf h; sb_init(&h);
  sb_puts(&h, "/* Generated by ./make unicode. Do not edit. */\n");
  sb_puts(&h, "#ifndef NYTRIX_UNICODE_H\n#define NYTRIX_UNICODE_H\n");
  sb_puts(&h, "#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n");
  sb_puts(&h, "bool ny_unicode_xid_start(uint32_t cp);\n");
  sb_puts(&h, "bool ny_unicode_xid_continue(uint32_t cp);\n");
  sb_puts(&h, "#endif\n");
  if (!write_if_changed_path(xid_h_path, h.data, h.len, &changed_h)) {
    fprintf(stderr,"unicode: failed to write %s\n", xid_h_path); return 1;
  }

  strbuf c; sb_init(&c);
  sb_puts(&c, "/* Generated by ./make unicode from Unicode 18.0.0. Do not edit. */\n");
  sb_puts(&c, "#include \"unicode.h\"\n");
  sb_puts(&c, "typedef struct { uint32_t lo, hi; } ny_unicode_range_t;\n");
  sb_puts(&c, "static const ny_unicode_range_t xid_start_ranges[] = {\n");
  for(size_t i=0;i<start_pairs.len;i+=2){
    sb_printf(&c,"  {0x%X, 0x%X}%s\n", start_pairs.data[i], start_pairs.data[i+1], (i+2<start_pairs.len)?",":"");
  }
  sb_puts(&c, "};\n");
  sb_puts(&c, "static const ny_unicode_range_t xid_continue_ranges[] = {\n");
  for(size_t i=0;i<cont_pairs.len;i+=2){
    sb_printf(&c,"  {0x%X, 0x%X}%s\n", cont_pairs.data[i], cont_pairs.data[i+1], (i+2<cont_pairs.len)?",":"");
  }
  sb_puts(&c, "};\n\n");
  sb_puts(&c,
"static bool in_ranges(const ny_unicode_range_t *ranges, size_t n, uint32_t cp) {\n"
"  size_t lo = 0, hi = n;\n"
"  while (lo < hi) {\n"
"    size_t mid = lo + (hi - lo) / 2;\n"
"    if (ranges[mid].hi < cp) lo = mid + 1;\n"
"    else hi = mid;\n"
"  }\n"
"  return lo < n && ranges[lo].lo <= cp;\n"
"}\n"
"bool ny_unicode_xid_start(uint32_t cp) {\n"
"  if (cp < 128) return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_';\n"
"  return in_ranges(xid_start_ranges, sizeof(xid_start_ranges) / sizeof(xid_start_ranges[0]), cp);\n"
"}\n"
"bool ny_unicode_xid_continue(uint32_t cp) {\n"
"  if (cp < 128) return ny_unicode_xid_start(cp) || (cp >= '0' && cp <= '9');\n"
"  return in_ranges(xid_continue_ranges, sizeof(xid_continue_ranges) / sizeof(xid_continue_ranges[0]), cp);\n"
"}\n");
  if (!write_if_changed_path(xid_c_path, c.data, c.len, &changed_c)) {
    fprintf(stderr,"unicode: failed to write %s\n", xid_c_path); return 1;
  }

  bool changed = changed_data || changed_h || changed_c;
  fprintf(stderr, "unicode %s — %s\n", UCD_VERSION, changed ? "generated" : "verified, unchanged");

  /* cleanup (best effort) */
  (void)len_ud; (void)len_dcp; (void)len_pl; (void)len_emoji; (void)len_pva; (void)len_scr; (void)len_gb; (void)len_wb; (void)len_sb; (void)len_ce; (void)len_cf; (void)len_sc;
  return 0;
}

int ny_unicode_generate_main(const char *root, int argc, char **argv) {
  (void)argc; (void)argv;
  if (!root || !*root) {
    fprintf(stderr, "unicode: repository root not set\n");
    return 1;
  }
  return generate_unicode(root);
}
