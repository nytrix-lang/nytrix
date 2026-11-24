/* ny-fuzz: seeded differential fuzzer and test-case reducer, reference C compiler versus NYC. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} sb_t;

static void sb_reserve(sb_t *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap)
    return;
  size_t cap = b->cap ? b->cap : 1024;
  while (cap < b->len + extra + 1)
    cap *= 2;
  char *next = realloc(b->data, cap);
  if (!next) {
    fputs("ny-fuzz: out of memory\n", stderr);
    exit(2);
  }
  b->data = next;
  b->cap = cap;
}

static void sb_addf(sb_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list copy;
  va_copy(copy, ap);
  int need = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (need < 0) {
    va_end(ap);
    return;
  }
  sb_reserve(b, (size_t)need);
  vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
  b->len += (size_t)need;
  va_end(ap);
}

static void sb_free(sb_t *b) {
  free(b->data);
  *b = (sb_t){0};
}

static bool read_file(const char *path, sb_t *out) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    sb_reserve(out, n);
    memcpy(out->data + out->len, chunk, n);
    out->len += n;
  }
  fclose(f);
  sb_reserve(out, 0);
  out->data[out->len] = '\0';
  return true;
}

static bool write_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  bool ok = fwrite(data, 1, len, f) == len;
  return fclose(f) == 0 && ok;
}

static void make_dir(const char *path) {
  if (mkdir(path, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr, "ny-fuzz: cannot create %s: %s\n", path, strerror(errno));
    exit(2);
  }
}

static void remove_tree(const char *path) {
  char *argv[] = {"rm", "-rf", (char *)path, NULL};
  pid_t pid;
  if (posix_spawnp(&pid, "rm", NULL, NULL, argv, environ) == 0) {
    int status = 0;
    (void)waitpid(pid, &status, 0);
  }
}

/* ------------------------------------------------------------------ */
/* Process execution                                                   */
/* ------------------------------------------------------------------ */

#define RUN_TIMEOUT 124
#define RUN_SPAWN_FAILED 127

static _Thread_local long g_child_cpu_ms;
static _Thread_local long g_child_rss_kb;

/* Totals over every NYC compile of a run. */
static long g_nyc_cpu_ms;
static long g_nyc_rss_kb;
static long g_nyc_compiles;

static void note_nyc_compile(void) {
  __atomic_fetch_add(&g_nyc_cpu_ms, g_child_cpu_ms, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_nyc_compiles, 1, __ATOMIC_RELAXED);
  long seen = __atomic_load_n(&g_nyc_rss_kb, __ATOMIC_RELAXED);
  while (g_child_rss_kb > seen &&
         !__atomic_compare_exchange_n(&g_nyc_rss_kb, &seen, g_child_rss_kb, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

/* Resource use of the most recent child in this thread (see run_cmd). */

/*
 * Run argv with stdout and stderr captured into files. Returns the exit
 * status, 128 + signal for a signalled child, RUN_TIMEOUT when the child
 * outlived `timeout_s`, or RUN_SPAWN_FAILED. CPU time and peak RSS of the
 * child land in g_child_cpu_ms and g_child_rss_kb.
 */
static int run_cmd(char *const argv[], const char *out_path,
                   const char *err_path, int timeout_s) {
  g_child_cpu_ms = 0;
  g_child_rss_kb = 0;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, out_path ? out_path : "/dev/null",
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_addopen(&actions, 2, err_path ? err_path : "/dev/null",
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  pid_t pid;
  int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0)
    return RUN_SPAWN_FAILED;
  struct timespec nap = {0, 5 * 1000 * 1000};
  long waited_ms = 0;
  for (;;) {
    int status = 0;
    struct rusage usage;
    memset(&usage, 0, sizeof(usage));
    pid_t done = wait4(pid, &status, WNOHANG, &usage);
    if (done == pid) {
      g_child_cpu_ms = usage.ru_utime.tv_sec * 1000 + usage.ru_utime.tv_usec / 1000 +
                       usage.ru_stime.tv_sec * 1000 + usage.ru_stime.tv_usec / 1000;
      g_child_rss_kb = usage.ru_maxrss;
      if (WIFEXITED(status))
        return WEXITSTATUS(status);
      if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
      return RUN_SPAWN_FAILED;
    }
    if (done < 0 && errno != EINTR)
      return RUN_SPAWN_FAILED;
    if (waited_ms > (long)timeout_s * 1000) {
      kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
      return RUN_TIMEOUT;
    }
    nanosleep(&nap, NULL);
    waited_ms += 5;
  }
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

enum { GEN_BUILTIN = 1, GEN_YARPGEN = 2, GEN_CSMITH = 4 };

typedef struct {
  const char *nyc;
  const char *ref;
  const char *outdir;
  const char *csmith_include;
  const char *ref2;     /* second reference compiler used as a tie-breaker */
  const char *baseline; /* file holding the compile-cost baseline */
  bool save_baseline;
  int routes;           /* bit 0 object, 1 direct encoder, 2 assembly */
  int profile;          /* -1 all, 0 integer ... 6 gnu */
  long seed_lo;
  long seed_hi;
  int jobs;
  int gens;
  int timeout_s;
  bool keep;
  bool strict;
  bool small;
  char opts[4][8];
  int opt_count;
} config_t;

/* ------------------------------------------------------------------ */
/* Builtin generator: undefined-behaviour-free integer programs         */
/* ------------------------------------------------------------------ */

typedef struct {
  uint64_t state;
} rng_t;

static uint64_t rng_next(rng_t *r) {
  uint64_t z = (r->state += UINT64_C(0x9e3779b97f4a7c15));
  z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
  return z ^ (z >> 31);
}

static unsigned rng_below(rng_t *r, unsigned n) {
  return (unsigned)(rng_next(r) % n);
}

static const char *const k_types[] = {"uint8_t",  "int8_t",  "uint16_t",
                                      "int16_t",  "uint32_t", "int32_t",
                                      "uint64_t", "int64_t"};
#define TYPE_COUNT 8

#define MAX_VARS 64
typedef struct {
  rng_t rng;
  sb_t *stmts;
  char vars[MAX_VARS][16];
  int var_count;
  int temp;
  bool in_loop;
} gen_t;

typedef struct {
  char text[24];
} name_t;

static const char *pick_type(gen_t *g) {
  return k_types[rng_below(&g->rng, TYPE_COUNT)];
}

static name_t atom(gen_t *g) {
  name_t n;
  if (g->var_count && rng_below(&g->rng, 10) < 7) {
    snprintf(n.text, sizeof(n.text), "%s", g->vars[rng_below(&g->rng, (unsigned)g->var_count)]);
    return n;
  }
  if (g->in_loop && rng_below(&g->rng, 10) < 3) {
    snprintf(n.text, sizeof(n.text), "(uint64_t)i");
    return n;
  }
  static const unsigned widths[] = {4, 8, 16, 31};
  unsigned bits = widths[rng_below(&g->rng, 4)];
  snprintf(n.text, sizeof(n.text), "%uu", rng_below(&g->rng, 1u << bits));
  return n;
}

static name_t emit_temp(gen_t *g, const char *fmt, ...) {
  name_t n;
  snprintf(n.text, sizeof(n.text), "e%d", ++g->temp);
  sb_addf(g->stmts, "  uint64_t %s = ", n.text);
  va_list ap;
  va_start(ap, fmt);
  char body[512];
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  sb_addf(g->stmts, "%s;\n", body);
  return n;
}

/* One binary or select operation per temporary (three-address form). */
static name_t expr(gen_t *g, int depth) {
  if (depth == 0)
    return atom(g);
  unsigned k = rng_below(&g->rng, 11);
  name_t a = expr(g, depth - 1);
  name_t b = expr(g, depth - 1);
  switch (k) {
  case 0:
    return emit_temp(g, "(uint64_t)(%s) + (uint64_t)(%s)", a.text, b.text);
  case 1:
    return emit_temp(g, "(uint64_t)(%s) - (uint64_t)(%s)", a.text, b.text);
  case 2:
    return emit_temp(g, "(uint64_t)(%s) * (uint64_t)(%s)", a.text, b.text);
  case 3:
    return emit_temp(g, "(uint64_t)(%s) & (uint64_t)(%s)", a.text, b.text);
  case 4:
    return emit_temp(g, "(uint64_t)(%s) | (uint64_t)(%s)", a.text, b.text);
  case 5:
    return emit_temp(g, "(uint64_t)(%s) ^ (uint64_t)(%s)", a.text, b.text);
  case 6:
    return emit_temp(g, "(uint64_t)(%s) << (%s & 63u)", a.text, b.text);
  case 7:
    return emit_temp(g, "(uint64_t)(%s) >> (%s & 63u)", a.text, b.text);
  case 8:
    return emit_temp(g, "(uint64_t)(%s)(%s)", pick_type(g), a.text);
  case 9: {
    static const char *const cmps[] = {"<", "<=", "==", "!=", ">", ">="};
    const char *c = cmps[rng_below(&g->rng, 6)];
    const char *t = pick_type(g);
    name_t x = expr(g, depth - 1);
    name_t y = expr(g, depth - 1);
    return emit_temp(g, "(%s)(%s) %s (%s)(%s) ? (uint64_t)(%s) : (uint64_t)(%s)",
                     t, a.text, c, t, b.text, x.text, y.text);
  }
  default:
    return emit_temp(g, "(uint64_t)(%s) %s ((uint64_t)(%s) | 1u)", a.text,
                     rng_below(&g->rng, 2) ? "%" : "/", b.text);
  }
}

static void gen_integer(uint64_t seed, bool small, sb_t *out) {
  gen_t g = {.rng = {.state = seed * UINT64_C(0x2545f4914f6cdd1d) + 1}};
  rng_t *r = &g.rng;
  sb_addf(out, "#include <stdint.h>\n#include <stdio.h>\n");
  sb_addf(out, "static uint64_t sink = 1469598103934665603ull;\n");
  sb_addf(out, "static void mix(uint64_t v) { sink = (sink ^ v) * "
               "1099511628211ull; }\n");
  int nglobals = 3 + (int)rng_below(r, 5);
  const char *global_type[16];
  for (int i = 0; i < nglobals; ++i) {
    global_type[i] = pick_type(&g);
    sb_addf(out, "static %s g%d = %u;\n", global_type[i], i,
            rng_below(r, 1u << 31));
  }
  int nfun = small ? 1 : 2 + (int)rng_below(r, 3);
  char proto[8][160];
  int nparams[8];
  const char *ptypes[8][4];
  for (int f = 0; f < nfun; ++f) {
    sb_t body = {0};
    g.stmts = &body;
    g.var_count = 0;
    g.temp = f * 1000;
    for (int i = 0; i < nglobals; ++i)
      snprintf(g.vars[g.var_count++], 16, "g%d", i);
    nparams[f] = 1 + (int)rng_below(r, 3);
    proto[f][0] = '\0';
    for (int p = 0; p < nparams[f]; ++p) {
      ptypes[f][p] = pick_type(&g);
      char piece[48];
      snprintf(piece, sizeof(piece), "%s%s a%d", p ? ", " : "", ptypes[f][p], p);
      strncat(proto[f], piece, sizeof(proto[f]) - strlen(proto[f]) - 1);
      snprintf(g.vars[g.var_count++], 16, "a%d", p);
    }
    int ntemps = small ? 1 : 2 + (int)rng_below(r, 4);
    char last[16] = "";
    for (int k = 0; k < ntemps; ++k) {
      name_t v = expr(&g, small ? 1 + (int)rng_below(r, 2)
                                : 1 + (int)rng_below(r, 3));
      snprintf(last, sizeof(last), "t%d", k);
      sb_addf(&body, "  %s %s = (uint64_t)(%s);\n", pick_type(&g), last, v.text);
      snprintf(g.vars[g.var_count++], 16, "%s", last);
    }
    if (!small && rng_below(r, 10) < 6) {
      sb_t inner = {0};
      sb_t *saved = g.stmts;
      g.stmts = &inner;
      g.in_loop = true;
      name_t v = expr(&g, 2);
      g.in_loop = false;
      g.stmts = saved;
      sb_addf(&body, "  for (int i = 0; i < %u; ++i) {\n%s    %s = "
                     "(uint64_t)(%s);\n  }\n",
              2 + rng_below(r, 5), inner.data ? inner.data : "", last, v.text);
      sb_free(&inner);
    }
    if (!small) {
      name_t v = expr(&g, 2);
      sb_addf(&body, "  g%u = (%s)(%s);\n", rng_below(r, (unsigned)nglobals),
              global_type[0], v.text);
    }
    name_t ret = expr(&g, small ? 1 : 3);
    sb_addf(&body, "  return (uint64_t)(%s);\n", ret.text);
    sb_addf(out, "static __attribute__((noinline)) uint64_t f%d(%s) {\n%s}\n", f,
            proto[f], body.data ? body.data : "");
    sb_free(&body);
  }
  sb_addf(out, "int main(void) {\n");
  unsigned calls = small ? 2 : 4 + rng_below(r, 8);
  static const unsigned bit_choices[] = {6, 20, 31};
  for (unsigned c = 0; c < calls; ++c) {
    int f = (int)rng_below(r, (unsigned)nfun);
    sb_addf(out, "  mix(f%d(", f);
    for (int p = 0; p < nparams[f]; ++p)
      sb_addf(out, "%s(%s)%uu", p ? ", " : "", ptypes[f][p],
              rng_below(r, 1u << bit_choices[rng_below(r, 3)]));
    sb_addf(out, "));\n");
  }
  for (int i = 0; i < nglobals; ++i)
    sb_addf(out, "  mix((uint64_t)g%d);\n", i);
  sb_addf(out, "  printf(\"%%llu\\n\", (unsigned long long)sink);\n  return 0;\n}\n");
}


/* Binary operators over unsigned 64-bit operands; every one is defined. */
static const char *const k_binops[] = {"+", "-", "*", "^", "|", "&"};

static void gen_preamble(sb_t *out) {
  sb_addf(out, "#include <stdarg.h>\n#include <stdint.h>\n#include <stdio.h>\n");
  sb_addf(out, "static uint64_t sink = 1469598103934665603ull;\n");
  sb_addf(out, "static void mix(uint64_t v) { sink = (sink ^ v) * "
               "1099511628211ull; }\n");
}

static void gen_epilogue(sb_t *out) {
  sb_addf(out, "  printf(\"%%llu\\n\", (unsigned long long)sink);\n  return 0;\n}\n");
}

/* Structures passed and returned by value, by pointer, and copied in arrays. */
static void gen_struct(uint64_t seed, sb_t *out) {
  rng_t r = {.state = seed * UINT64_C(0x9e3779b97f4a7c15) + 7};
  int nf = 3 + (int)rng_below(&r, 4);
  const char *ft[8];
  gen_preamble(out);
  sb_addf(out, "struct S {\n");
  for (int i = 0; i < nf; ++i) {
    ft[i] = k_types[rng_below(&r, TYPE_COUNT)];
    sb_addf(out, "  %s f%d;\n", ft[i], i);
  }
  sb_addf(out, "};\n");
  int nfun = 2 + (int)rng_below(&r, 3);
  bool pointer_form[8] = {false};
  for (int f = 0; f < nfun; ++f) {
    bool by_pointer = rng_below(&r, 3) == 0;
    pointer_form[f] = by_pointer;
    if (by_pointer)
      sb_addf(out, "static __attribute__((noinline)) void u%d(struct S *s, "
                   "uint64_t k) {\n", f);
    else
      sb_addf(out, "static __attribute__((noinline)) struct S u%d(struct S s, "
                   "uint64_t k) {\n", f);
    const char *sel = by_pointer ? "s->" : "s.";
    unsigned stmts = 2 + rng_below(&r, 4);
    for (unsigned k = 0; k < stmts; ++k) {
      int a = (int)rng_below(&r, (unsigned)nf), b = (int)rng_below(&r, (unsigned)nf);
      const char *op = k_binops[rng_below(&r, 6)];
      if (rng_below(&r, 4) == 0)
        sb_addf(out, "  %sf%d = (%s)((uint64_t)%sf%d >> ((uint64_t)%sf%d & 31u));\n",
                sel, a, ft[a], sel, a, sel, b);
      else
        sb_addf(out, "  %sf%d = (%s)((uint64_t)%sf%d %s ((uint64_t)%sf%d %s k));\n",
                sel, a, ft[a], sel, a, op, sel, b, k_binops[rng_below(&r, 6)]);
    }
    if (!by_pointer)
      sb_addf(out, "  return s;\n");
    sb_addf(out, "}\n");
  }
  sb_addf(out, "static void mix_struct(struct S s) {\n");
  for (int i = 0; i < nf; ++i)
    sb_addf(out, "  mix((uint64_t)s.f%d);\n", i);
  sb_addf(out, "}\nint main(void) {\n  struct S a[3];\n");
  for (int e = 0; e < 3; ++e) {
    sb_addf(out, "  a[%d] = (struct S){", e);
    for (int i = 0; i < nf; ++i)
      sb_addf(out, "%s(%s)%uu", i ? ", " : "", ft[i], rng_below(&r, 1u << 30));
    sb_addf(out, "};\n");
  }
  unsigned calls = 4 + rng_below(&r, 6);
  for (unsigned c = 0; c < calls; ++c) {
    int f = (int)rng_below(&r, (unsigned)nfun);
    int dst = (int)rng_below(&r, 3), src = (int)rng_below(&r, 3);
    sb_addf(out, "  a[%d] = a[%d];\n", dst, src);
    if (pointer_form[f])
      sb_addf(out, "  u%d(&a[%d], %uu);\n", f, dst, rng_below(&r, 1u << 20));
    else
      sb_addf(out, "  a[%d] = u%d(a[%d], %uu);\n", dst, f, dst,
              rng_below(&r, 1u << 20));
  }
  for (int e = 0; e < 3; ++e)
    sb_addf(out, "  mix_struct(a[%d]);\n", e);
  gen_epilogue(out);
}

/* Unsigned bit-fields: truncating stores, compound updates, copies. */
static void gen_bits(uint64_t seed, sb_t *out) {
  rng_t r = {.state = seed * UINT64_C(0xd1b54a32d192ed03) + 11};
  int nf = 3 + (int)rng_below(&r, 5);
  unsigned width[10];
  gen_preamble(out);
  sb_addf(out, "struct B {\n");
  for (int i = 0; i < nf; ++i) {
    width[i] = 1 + rng_below(&r, 31);
    sb_addf(out, "  unsigned f%d : %u;\n", i, width[i]);
  }
  sb_addf(out, "  uint64_t tail;\n};\n");
  int nfun = 2 + (int)rng_below(&r, 3);
  for (int f = 0; f < nfun; ++f) {
    sb_addf(out, "static __attribute__((noinline)) struct B u%d(struct B b, "
                 "unsigned k) {\n", f);
    unsigned stmts = 2 + rng_below(&r, 5);
    for (unsigned k = 0; k < stmts; ++k) {
      int a = (int)rng_below(&r, (unsigned)nf), c = (int)rng_below(&r, (unsigned)nf);
      switch (rng_below(&r, 4)) {
      case 0:
        sb_addf(out, "  b.f%d = b.f%d %s k;\n", a, c, k_binops[rng_below(&r, 6)]);
        break;
      case 1:
        sb_addf(out, "  b.f%d += b.f%d;\n", a, c);
        break;
      case 2:
        sb_addf(out, "  b.f%d = (unsigned)((uint64_t)b.f%d << (k & 7u));\n", a, c);
        break;
      default:
        sb_addf(out, "  b.tail += (uint64_t)b.f%d * (b.f%d + 1u);\n", a, c);
      }
    }
    sb_addf(out, "  return b;\n}\n");
  }
  sb_addf(out, "static void mix_bits(struct B b) {\n");
  for (int i = 0; i < nf; ++i)
    sb_addf(out, "  mix((uint64_t)b.f%d);\n", i);
  sb_addf(out, "  mix(b.tail);\n}\nint main(void) {\n  struct B b = {0};\n");
  for (int i = 0; i < nf; ++i)
    sb_addf(out, "  b.f%d = %uu;\n", i, rng_below(&r, 1u << 30));
  unsigned calls = 3 + rng_below(&r, 6);
  for (unsigned c = 0; c < calls; ++c)
    sb_addf(out, "  b = u%u(b, %uu);\n  mix_bits(b);\n",
            rng_below(&r, (unsigned)nfun), rng_below(&r, 1u << 20));
  gen_epilogue(out);
}

/* Variadic functions reading mixed integer and double arguments. */
static void gen_varargs(uint64_t seed, sb_t *out) {
  rng_t r = {.state = seed * UINT64_C(0xa0761d6478bd642f) + 13};
  gen_preamble(out);
  sb_addf(out, "static __attribute__((noinline)) uint64_t vmix(const char *fmt, "
               "...) {\n  va_list ap;\n  uint64_t acc = 7;\n  "
               "va_start(ap, fmt);\n  for (; *fmt; ++fmt) {\n    switch (*fmt) "
               "{\n    case 'i': acc = acc * 31 + (uint64_t)(uint32_t)va_arg(ap, "
               "int); break;\n    case 'l': acc = acc * 31 + (uint64_t)va_arg(ap, "
               "long long); break;\n    case 'u': acc = acc * 31 + "
               "(uint64_t)va_arg(ap, unsigned); break;\n    case 'd': acc = acc * "
               "31 + (uint64_t)(uint32_t)va_arg(ap, double); break;\n    }\n  }\n"
               "  va_end(ap);\n  return acc;\n}\nint main(void) {\n");
  unsigned calls = 4 + rng_below(&r, 6);
  static const char kinds[] = "iludi";
  for (unsigned c = 0; c < calls; ++c) {
    char fmt[16];
    unsigned n = 1 + rng_below(&r, 12);
    for (unsigned i = 0; i < n; ++i)
      fmt[i] = kinds[rng_below(&r, 5)];
    fmt[n] = '\0';
    sb_addf(out, "  mix(vmix(\"%s\"", fmt);
    for (unsigned i = 0; i < n; ++i) {
      unsigned v = rng_below(&r, 1u << 24);
      switch (fmt[i]) {
      case 'i': sb_addf(out, ", %d", (int)v - (1 << 20)); break;
      case 'l': sb_addf(out, ", %lluLL", ((unsigned long long)v << 20) | v); break;
      case 'u': sb_addf(out, ", %uu", v); break;
      default: sb_addf(out, ", %u.0", v);
      }
    }
    sb_addf(out, "));\n");
  }
  gen_epilogue(out);
}


/* Atomic builtins on plain objects, single threaded and so deterministic. */
static void gen_atomic(uint64_t seed, sb_t *out) {
  rng_t r = {.state = seed * UINT64_C(0xe7037ed1a0b428db) + 17};
  gen_preamble(out);
  static const char *const types[] = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"};
  int n = 3 + (int)rng_below(&r, 4);
  for (int i = 0; i < n; ++i)
    sb_addf(out, "static %s a%d = %uu;\n", types[rng_below(&r, 4)], i,
            rng_below(&r, 1u << 20));
  sb_addf(out, "static __attribute__((noinline)) void step(unsigned k) {\n");
  unsigned stmts = 5 + rng_below(&r, 8);
  for (unsigned i = 0; i < stmts; ++i) {
    int v = (int)rng_below(&r, (unsigned)n), w = (int)rng_below(&r, (unsigned)n);
    switch (rng_below(&r, 8)) {
    case 0: sb_addf(out, "  mix(__atomic_fetch_add(&a%d, k, __ATOMIC_SEQ_CST));\n", v); break;
    case 1: sb_addf(out, "  mix(__atomic_fetch_sub(&a%d, k + 3, __ATOMIC_SEQ_CST));\n", v); break;
    case 2: sb_addf(out, "  mix(__atomic_fetch_xor(&a%d, k | 1, __ATOMIC_RELAXED));\n", v); break;
    case 3: sb_addf(out, "  mix(__atomic_fetch_and(&a%d, ~(k & 7), __ATOMIC_ACQ_REL));\n", v); break;
    case 4: sb_addf(out, "  mix(__atomic_exchange_n(&a%d, a%d + k, __ATOMIC_SEQ_CST));\n", v, w); break;
    case 5:
      sb_addf(out, "  { __typeof__(a%d) e = a%d; mix(__atomic_compare_exchange_n(&a%d, "
                   "&e, e + k, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); mix(e); }\n",
              v, v, v);
      break;
    case 6: sb_addf(out, "  __atomic_store_n(&a%d, __atomic_load_n(&a%d, __ATOMIC_SEQ_CST) ^ k, "
                         "__ATOMIC_SEQ_CST);\n", v, w); break;
    default: sb_addf(out, "  mix(__atomic_fetch_or(&a%d, k << 1, __ATOMIC_SEQ_CST));\n", v);
    }
  }
  sb_addf(out, "}\nint main(void) {\n");
  unsigned calls = 3 + rng_below(&r, 5);
  for (unsigned c = 0; c < calls; ++c)
    sb_addf(out, "  step(%uu);\n", rng_below(&r, 1u << 16));
  for (int i = 0; i < n; ++i)
    sb_addf(out, "  mix((uint64_t)a%d);\n", i);
  gen_epilogue(out);
}

/* Type punning through unions, character pointers and memcpy. */
static void gen_alias(uint64_t seed, sb_t *out) {
  rng_t r = {.state = seed * UINT64_C(0x8ebc6af09c88c6e3) + 19};
  gen_preamble(out);
  sb_addf(out, "#include <string.h>\n");
  sb_addf(out, "union U { uint64_t q; uint32_t w[2]; uint16_t h[4]; uint8_t b[8]; };\n");
  sb_addf(out, "static uint64_t buf[8];\n");
  sb_addf(out, "static __attribute__((noinline)) void poke(unsigned k) {\n");
  sb_addf(out, "  unsigned char *p = (unsigned char *)buf;\n  union U u;\n  uint32_t t;\n");
  unsigned stmts = 5 + rng_below(&r, 8);
  for (unsigned i = 0; i < stmts; ++i) {
    unsigned a = rng_below(&r, 64), b = rng_below(&r, 8);
    switch (rng_below(&r, 6)) {
    case 0: sb_addf(out, "  p[%u] = (unsigned char)(p[%u] + k);\n", a, (a * 5 + 3) % 64 ? (a * 5 + 3) % 64 : 1); break;
    case 1: sb_addf(out, "  u.q = buf[%u]; u.w[%u] ^= k; buf[%u] = u.q;\n", b, rng_below(&r, 2), (b + 1) % 8); break;
    case 2: sb_addf(out, "  memcpy(&t, p + %u, sizeof t); t += k; memcpy(p + %u, &t, sizeof t);\n", a % 61, (a + 7) % 61 ? (a + 7) % 61 : 1); break;
    case 3: sb_addf(out, "  u.q = buf[%u]; u.h[%u] = (uint16_t)(u.h[%u] * 3 + k); buf[%u] = u.q;\n", b, rng_below(&r, 4), rng_below(&r, 4), b); break;
    case 4: sb_addf(out, "  buf[%u] = (buf[%u] << (k & 31)) | (buf[%u] >> 7);\n", b, (b + 3) % 8, (b + 5) % 8); break;
    default: sb_addf(out, "  { unsigned char *q = p + %u; q[0] ^= (unsigned char)k; q[1] += q[0]; }\n", a % 62);
    }
  }
  sb_addf(out, "}\nint main(void) {\n  for (int i = 0; i < 8; ++i) buf[i] = %uu * (i + 1);\n",
          1 + rng_below(&r, 1u << 20));
  unsigned calls = 3 + rng_below(&r, 5);
  for (unsigned c = 0; c < calls; ++c)
    sb_addf(out, "  poke(%uu);\n", rng_below(&r, 1u << 16));
  sb_addf(out, "  for (int i = 0; i < 8; ++i) mix(buf[i]);\n");
  gen_epilogue(out);
}

/* GNU attributes, builtins and register-constrained inline assembly. */
static void gen_gnu(uint64_t seed, sb_t *out) {
  rng_t r = {.state = seed * UINT64_C(0x589965cc75374cc3) + 23};
  gen_preamble(out);
  sb_addf(out, "struct __attribute__((packed)) P { uint8_t a; uint32_t b; uint16_t c; "
               "uint64_t d; };\n");
  sb_addf(out, "static struct P gp = {1, 2, 3, 4};\n");
  sb_addf(out, "static uint64_t tab[4] __attribute__((aligned(32))) = {1, 2, 3, 4};\n");
  sb_addf(out, "static __attribute__((noinline)) uint64_t f(uint64_t x, uint64_t y) {\n");
  sb_addf(out, "  uint64_t r = x;\n");
  unsigned stmts = 4 + rng_below(&r, 8);
  for (unsigned i = 0; i < stmts; ++i) {
    switch (rng_below(&r, 9)) {
    case 0: sb_addf(out, "  r += (uint64_t)__builtin_popcountll(r ^ y);\n"); break;
    case 1: sb_addf(out, "  r ^= (uint64_t)__builtin_clzll(r | 1) + y;\n"); break;
    case 2: sb_addf(out, "  r = __builtin_bswap64(r) + (uint64_t)__builtin_ctzll(y | 1);\n"); break;
    case 3: sb_addf(out, "  r += __builtin_bswap32((uint32_t)y);\n"); break;
    case 4: sb_addf(out, "  __asm__(\"addq %%1, %%0\" : \"+r\"(r) : \"r\"(y));\n"); break;
    case 5: sb_addf(out, "  __asm__ volatile(\"\" ::: \"memory\");\n  gp.b += (uint32_t)r; gp.c ^= (uint16_t)y; gp.d += gp.b;\n"); break;
    case 6: sb_addf(out, "  tab[(r >> 3) & 3] += __builtin_expect(r & 1, 0) ? y : r;\n"); break;
    case 7: { sb_addf(out, "  { uint64_t o; r += __builtin_add_overflow(r, y, &o) ? 5 : o; }\n"); break; }
    default: sb_addf(out, "  r = (r << 5) | (r >> 59);\n");
    }
  }
  sb_addf(out, "  return r + gp.a + gp.d;\n}\nint main(void) {\n");
  unsigned calls = 3 + rng_below(&r, 5);
  for (unsigned c = 0; c < calls; ++c)
    sb_addf(out, "  mix(f(%lluull, %uu));\n",
            ((unsigned long long)rng_below(&r, 1u << 30) << 20) | rng_below(&r, 1u << 20),
            rng_below(&r, 1u << 24));
  sb_addf(out, "  for (int i = 0; i < 4; ++i) mix(tab[i]);\n  mix(gp.b); mix(gp.c);\n");
  gen_epilogue(out);
}

static void gen_builtin(uint64_t seed, bool small, int profile, sb_t *out) {
  int pick = profile >= 0 ? profile : (int)(seed % 7);
  if (pick == 1) {
    gen_struct(seed, out);
    return;
  }
  if (pick == 2)
    gen_bits(seed, out);
  else if (pick == 3)
    gen_varargs(seed, out);
  else if (pick == 4)
    gen_atomic(seed, out);
  else if (pick == 5)
    gen_alias(seed, out);
  else if (pick == 6)
    gen_gnu(seed, out);
  else
    gen_integer(seed, small, out);
}

/* ------------------------------------------------------------------ */
/* Case checking                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
  ST_OK,
  ST_INVALID,
  ST_UNSUPPORTED,
  ST_DISPUTE,
  ST_MISCOMPILE,
  ST_LINKFAIL,
  ST_COUNT
} status_t;

static const char *const k_status_names[ST_COUNT] = {
    "ok", "invalid", "unsupported", "oracle-dispute", "MISCOMPILE", "link-fail"};

typedef struct {
  status_t status;
  char detail[240];
} outcome_t;

static outcome_t outcome(status_t s, const char *fmt, ...) {
  outcome_t o = {.status = s};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(o.detail, sizeof(o.detail), fmt, ap);
  va_end(ap);
  return o;
}

static void first_line(const char *path, char *out, size_t cap) {
  out[0] = '\0';
  sb_t text = {0};
  if (read_file(path, &text) && text.data) {
    size_t n = strcspn(text.data, "\n");
    if (n >= cap)
      n = cap - 1;
    memcpy(out, text.data, n);
    out[n] = '\0';
  }
  sb_free(&text);
}

static void path_in(char *out, size_t cap, const char *dir, const char *name) {
  int n = snprintf(out, cap, "%s/%s", dir, name);
  if (n < 0 || (size_t)n >= cap)
    out[0] = '\0';
}

/* Compare NYC's build of one case at every optimization level. */
typedef struct {
  const char *dir;
  const char *driver;       /* compiled by the reference compiler, or NULL */
  const char *nyc_unit;     /* the translation unit NYC compiles */
  const char *ref_inputs[3];
  const char *include;      /* extra -I directory or NULL */
  const char *model;        /* code-model flag for large data, or NULL */
} case_t;

static outcome_t check_case(const config_t *cfg, const case_t *c) {
  char ref_exe[PATH_MAX], want_out[PATH_MAX], err[PATH_MAX], inc[PATH_MAX];
  path_in(ref_exe, sizeof(ref_exe), c->dir, "ref");
  path_in(want_out, sizeof(want_out), c->dir, "ref.out");
  path_in(err, sizeof(err), c->dir, "err.txt");
  if (snprintf(inc, sizeof(inc), "-I%s", c->include ? c->include : c->dir) >=
      (int)sizeof(inc))
    inc[0] = '\0';
  char *ref_argv[13];
  int n = 0;
  ref_argv[n++] = (char *)cfg->ref;
  ref_argv[n++] = "-w";
  ref_argv[n++] = "-O0";
  ref_argv[n++] = inc;
  if (c->model)
    ref_argv[n++] = (char *)c->model;
  for (int i = 0; c->ref_inputs[i] && i < 3; ++i)
    ref_argv[n++] = (char *)c->ref_inputs[i];
  ref_argv[n++] = "-o";
  ref_argv[n++] = ref_exe;
  ref_argv[n++] = "-lm";
  ref_argv[n] = NULL;
  int rc = run_cmd(ref_argv, NULL, err, cfg->timeout_s * 4);
  if (rc != 0) {
    char why[160];
    first_line(err, why, sizeof(why));
    return outcome(ST_INVALID, "reference compile failed: %s", why);
  }
  char *run_ref[] = {ref_exe, NULL};
  int want_rc = run_cmd(run_ref, want_out, NULL, cfg->timeout_s);
  if (want_rc >= 128)
    return outcome(ST_INVALID, "reference run died with status %d", want_rc);
  sb_t want = {0};
  read_file(want_out, &want);
  if (cfg->ref2) {
    /* A second compiler settles disputes about what the program means. */
    char ref2_exe[PATH_MAX], ref2_out[PATH_MAX];
    path_in(ref2_exe, sizeof(ref2_exe), c->dir, "ref2");
    path_in(ref2_out, sizeof(ref2_out), c->dir, "ref2.out");
    char *argv2[13];
    memcpy(argv2, ref_argv, sizeof(argv2));
    argv2[0] = (char *)cfg->ref2;
    for (int i = 0; argv2[i]; ++i)
      if (argv2[i] == ref_exe)
        argv2[i] = ref2_exe;
    if (run_cmd(argv2, NULL, err, cfg->timeout_s * 4) == 0) {
      char *run2[] = {ref2_exe, NULL};
      int rc2 = run_cmd(run2, ref2_out, NULL, cfg->timeout_s);
      sb_t other = {0};
      read_file(ref2_out, &other);
      bool agree = rc2 == want_rc && other.len == want.len &&
                   (!want.len || memcmp(other.data, want.data, want.len) == 0);
      sb_free(&other);
      if (!agree) {
        sb_free(&want);
        return outcome(ST_DISPUTE, "%s and %s disagree (status %d vs %d)",
                       cfg->ref, cfg->ref2, want_rc, rc2);
      }
    }
  }
  outcome_t result = outcome(ST_OK, "");
  for (int o = 0; o < cfg->opt_count && result.status == ST_OK; ++o) {
    const char *opt = cfg->opts[o];
    for (int rt = 0; rt < 3 && result.status == ST_OK; ++rt) {
      if (!(cfg->routes & (1 << rt)))
        continue;
      static const char *const route_names[] = {"object", "direct", "asm"};
      char obj[PATH_MAX], exe[PATH_MAX], got_out[PATH_MAX], asm_path[PATH_MAX],
          name[64], label[40];
      snprintf(label, sizeof(label), "%s/%s", opt, route_names[rt]);
      snprintf(name, sizeof(name), "n%s-%s.o", opt, route_names[rt]);
      path_in(obj, sizeof(obj), c->dir, name);
      snprintf(name, sizeof(name), "n%s-%s", opt, route_names[rt]);
      path_in(exe, sizeof(exe), c->dir, name);
      snprintf(name, sizeof(name), "n%s-%s.out", opt, route_names[rt]);
      path_in(got_out, sizeof(got_out), c->dir, name);
      snprintf(name, sizeof(name), "n%s-%s.s", opt, route_names[rt]);
      path_in(asm_path, sizeof(asm_path), c->dir, name);
      if (rt == 2) {
        char *asm_argv[] = {(char *)cfg->nyc, "-S", (char *)opt, inc,
                            (char *)c->nyc_unit, "-o", asm_path, NULL};
        rc = run_cmd(asm_argv, NULL, err, cfg->timeout_s * 4);
        note_nyc_compile();
        if (rc == 0) {
          char *as_argv[] = {(char *)cfg->ref, "-w", "-c", asm_path, "-o", obj, NULL};
          rc = run_cmd(as_argv, NULL, err, cfg->timeout_s * 4);
        }
      } else if (rt == 1) {
        char *direct_argv[] = {"env", "NYC_NO_MACH_OBJECT=1", (char *)cfg->nyc, "-c",
                               (char *)opt, inc, (char *)c->nyc_unit, "-o", obj, NULL};
        rc = run_cmd(direct_argv, NULL, err, cfg->timeout_s * 4);
        note_nyc_compile();
      } else {
        char *nyc_argv[] = {(char *)cfg->nyc, "-c", (char *)opt, inc,
                            (char *)c->nyc_unit, "-o", obj, NULL};
        rc = run_cmd(nyc_argv, NULL, err, cfg->timeout_s * 4);
        note_nyc_compile();
      }
      if (rc != 0) {
        char why[160];
        first_line(err, why, sizeof(why));
        result = outcome(ST_UNSUPPORTED, "nyc %s rejected: %s", label, why);
        break;
      }
      char *link_argv[11];
      int m = 0;
      link_argv[m++] = (char *)cfg->ref;
      link_argv[m++] = "-w";
      link_argv[m++] = inc;
      if (c->model)
        link_argv[m++] = (char *)c->model;
      if (c->driver)
        link_argv[m++] = (char *)c->driver;
      link_argv[m++] = obj;
      link_argv[m++] = "-o";
      link_argv[m++] = exe;
      link_argv[m++] = "-lm";
      link_argv[m] = NULL;
      rc = run_cmd(link_argv, NULL, err, cfg->timeout_s * 4);
      if (rc != 0) {
        char why[160];
        first_line(err, why, sizeof(why));
        result = outcome(ST_LINKFAIL, "link %s: %s", label, why);
        break;
      }
      char *run_n[] = {exe, NULL};
      rc = run_cmd(run_n, got_out, NULL, cfg->timeout_s);
      sb_t got = {0};
      read_file(got_out, &got);
      bool same = rc == want_rc && got.len == want.len &&
                  (!want.len || memcmp(got.data, want.data, want.len) == 0);
      if (!same) {
        char w[64] = "", gtxt[64] = "";
        if (want.data)
          snprintf(w, sizeof(w), "%.*s", (int)strcspn(want.data, "\n"), want.data);
        if (got.data)
          snprintf(gtxt, sizeof(gtxt), "%.*s", (int)strcspn(got.data, "\n"),
                   got.data);
        result = outcome(ST_MISCOMPILE, "%s: want [%s] got [%s] rc=%d", label, w,
                         gtxt, rc);
      }
      sb_free(&got);
      }
  }
  sb_free(&want);
  return result;
}

/* ------------------------------------------------------------------ */
/* Generators that drive external tools                                */
/* ------------------------------------------------------------------ */

static outcome_t run_builtin_seed(const config_t *cfg, long seed, const char *dir) {
  sb_t src = {0};
  gen_builtin((uint64_t)seed, cfg->small, cfg->profile, &src);
  char path[PATH_MAX];
  path_in(path, sizeof(path), dir, "p.c");
  if (!write_file(path, src.data, src.len)) {
    sb_free(&src);
    return outcome(ST_INVALID, "cannot write %s", path);
  }
  sb_free(&src);
  case_t c = {.dir = dir, .nyc_unit = path, .ref_inputs = {path}};
  return check_case(cfg, &c);
}

static outcome_t run_yarpgen_seed(const config_t *cfg, long seed, const char *dir) {
  char seed_arg[48];
  snprintf(seed_arg, sizeof(seed_arg), "--seed=%ld", seed + 1);
  char out_arg[PATH_MAX + 16];
  snprintf(out_arg, sizeof(out_arg), "--out-dir=%s", dir);
  char *argv[] = {"yarpgen", "--std=c", seed_arg, out_arg, NULL};
  char err[PATH_MAX];
  path_in(err, sizeof(err), dir, "gen.txt");
  if (run_cmd(argv, NULL, err, cfg->timeout_s * 4) != 0)
    return outcome(ST_INVALID, "yarpgen failed");
  char driver[PATH_MAX], func[PATH_MAX];
  path_in(driver, sizeof(driver), dir, "driver.c");
  path_in(func, sizeof(func), dir, "func.c");
  case_t c = {.dir = dir,
              .driver = driver,
              .nyc_unit = func,
              .ref_inputs = {driver, func},
              .model = "-mcmodel=medium"};
  return check_case(cfg, &c);
}

static outcome_t run_csmith_seed(const config_t *cfg, long seed, const char *dir) {
  char path[PATH_MAX], seed_arg[48], err[PATH_MAX];
  path_in(path, sizeof(path), dir, "p.c");
  path_in(err, sizeof(err), dir, "gen.txt");
  snprintf(seed_arg, sizeof(seed_arg), "%ld", seed);
  char *argv[] = {"csmith", "--seed", seed_arg, "--output", path, NULL};
  if (run_cmd(argv, NULL, err, cfg->timeout_s * 4) != 0)
    return outcome(ST_INVALID, "csmith failed");
  case_t c = {.dir = dir,
              .nyc_unit = path,
              .ref_inputs = {path},
              .include = cfg->csmith_include};
  return check_case(cfg, &c);
}

static bool tool_exists(const char *name) {
  const char *path = getenv("PATH");
  if (!path)
    return false;
  char *copy = strdup(path);
  bool found = false;
  for (char *dir = strtok(copy, ":"); dir && !found; dir = strtok(NULL, ":")) {
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    found = access(full, X_OK) == 0;
  }
  free(copy);
  return found;
}

static const char *find_csmith_include(void) {
  const char *env = getenv("CSMITH_INCLUDE");
  if (env && *env)
    return env;
  static const char *const candidates[] = {
      "/usr/include/csmith", "/usr/include/csmith-2.3.0",
      "/usr/local/include/csmith", "/usr/local/include/csmith-2.3.0",
      "/usr/share/csmith/runtime"};
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/csmith.h", candidates[i]);
    if (access(probe, R_OK) == 0)
      return candidates[i];
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Parallel driver                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
  const config_t *cfg;
  long next;
  long total;
  int gen_list[3];
  int gen_count;
  long counts[ST_COUNT];
  pthread_mutex_t lock;
} pool_t;

static const char *gen_name(int gen) {
  return gen == GEN_BUILTIN ? "builtin" : gen == GEN_YARPGEN ? "yarpgen" : "csmith";
}

/* Everything needed to rebuild a failing case: versions, checksum, commands. */
static void tool_first_line(char *const argv[], char *out, size_t cap) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "/tmp/ny-fuzz-ver-%d-%lu", (int)getpid(),
           (unsigned long)pthread_self());
  out[0] = '\0';
  if (run_cmd(argv, tmp, NULL, 20) == 0)
    first_line(tmp, out, cap);
  unlink(tmp);
}

static void write_repro(const config_t *cfg, const char *dir, int gen, long seed,
                        const outcome_t *o) {
  char path[PATH_MAX], ref_version[200], nyc_version[200], sum[200] = "";
  path_in(path, sizeof(path), dir, "repro.txt");
  char *ref_v[] = {(char *)cfg->ref, "--version", NULL};
  char *nyc_v[] = {(char *)cfg->nyc, "--version", NULL};
  tool_first_line(ref_v, ref_version, sizeof(ref_version));
  tool_first_line(nyc_v, nyc_version, sizeof(nyc_version));
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "/tmp/ny-fuzz-sum-%d-%lu", (int)getpid(),
           (unsigned long)pthread_self());
  char *sha_v[] = {"sha256sum", (char *)cfg->nyc, NULL};
  if (run_cmd(sha_v, tmp, NULL, 20) == 0) {
    first_line(tmp, sum, sizeof(sum));
    char *space = strchr(sum, ' ');
    if (space)
      *space = '\0';
  }
  unlink(tmp);
  sb_t b = {0};
  sb_addf(&b, "generator: %s\nseed: %ld\nstatus: %s\ndetail: %s\n",
          gen_name(gen), seed, k_status_names[o->status], o->detail);
  sb_addf(&b, "reference: %s (%s)\nnyc: %s\nnyc sha256: %s\nlevels:", cfg->ref,
          ref_version, nyc_version, sum);
  for (int i = 0; i < cfg->opt_count; ++i)
    sb_addf(&b, " %s", cfg->opts[i]);
  sb_addf(&b, "\nreproduce:\n");
  if (gen == GEN_BUILTIN)
    sb_addf(&b, "  ny-fuzz gen --seed %ld --profile %d%s > p.c\n", seed,
            cfg->profile, cfg->small ? " --small" : "");
  else if (gen == GEN_YARPGEN)
    sb_addf(&b, "  yarpgen --std=c --seed=%ld --out-dir=.\n", seed + 1);
  else
    sb_addf(&b, "  csmith --seed %ld --output p.c\n", seed);
  sb_addf(&b, "  ny-fuzz run --nyc %s --ref %s --seeds %ld:%ld --gen %s --opts ",
          cfg->nyc, cfg->ref, seed, seed + 1, gen_name(gen));
  for (int i = 0; i < cfg->opt_count; ++i)
    sb_addf(&b, "%s%s", i ? "," : "", cfg->opts[i]);
  sb_addf(&b, "\n");
  write_file(path, b.data, b.len);
  sb_free(&b);
}

static void *worker(void *arg) {
  pool_t *pool = arg;
  const config_t *cfg = pool->cfg;
  for (;;) {
    pthread_mutex_lock(&pool->lock);
    long index = pool->next++;
    pthread_mutex_unlock(&pool->lock);
    if (index >= pool->total)
      return NULL;
    int gen = pool->gen_list[index % pool->gen_count];
    long seed = cfg->seed_lo + index / pool->gen_count;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s-%ld", cfg->outdir, gen_name(gen), seed);
    make_dir(dir);
    outcome_t o = gen == GEN_BUILTIN   ? run_builtin_seed(cfg, seed, dir)
                  : gen == GEN_YARPGEN ? run_yarpgen_seed(cfg, seed, dir)
                                       : run_csmith_seed(cfg, seed, dir);
    pthread_mutex_lock(&pool->lock);
    pool->counts[o.status]++;
    if (o.status != ST_OK &&
        ((o.status != ST_INVALID && o.status != ST_UNSUPPORTED) || cfg->strict))
      printf("%s seed %ld: %s %s\n", gen_name(gen), seed,
             k_status_names[o.status], o.detail);
    pthread_mutex_unlock(&pool->lock);
    bool keep = cfg->keep || o.status == ST_MISCOMPILE ||
                o.status == ST_LINKFAIL || o.status == ST_DISPUTE;
    if (o.status == ST_MISCOMPILE || o.status == ST_LINKFAIL ||
        o.status == ST_DISPUTE)
      write_repro(cfg, dir, gen, seed, &o);
    if (!keep)
      remove_tree(dir);
  }
}

static int command_run(config_t *cfg) {
  pool_t pool = {.cfg = cfg, .lock = PTHREAD_MUTEX_INITIALIZER};
  if (cfg->gens & GEN_BUILTIN)
    pool.gen_list[pool.gen_count++] = GEN_BUILTIN;
  if (cfg->gens & GEN_YARPGEN) {
    if (tool_exists("yarpgen"))
      pool.gen_list[pool.gen_count++] = GEN_YARPGEN;
    else
      fputs("ny-fuzz: yarpgen is not installed; skipping that generator\n", stderr);
  }
  if (cfg->gens & GEN_CSMITH) {
    cfg->csmith_include = find_csmith_include();
    if (tool_exists("csmith") && cfg->csmith_include)
      pool.gen_list[pool.gen_count++] = GEN_CSMITH;
    else
      fputs("ny-fuzz: csmith (or its runtime headers; set CSMITH_INCLUDE) is "
            "not installed; skipping that generator\n",
            stderr);
  }
  if (!pool.gen_count) {
    fputs("ny-fuzz: no generator is available\n", stderr);
    return 2;
  }
  make_dir(cfg->outdir);
  long seeds = cfg->seed_hi - cfg->seed_lo;
  pool.total = seeds * pool.gen_count;
  int jobs = cfg->jobs > 0 ? cfg->jobs : 1;
  pthread_t *threads = calloc((size_t)jobs, sizeof(*threads));
  for (int i = 0; i < jobs; ++i)
    pthread_create(&threads[i], NULL, worker, &pool);
  for (int i = 0; i < jobs; ++i)
    pthread_join(threads[i], NULL);
  free(threads);
  printf("%ld cases: %ld ok, %ld invalid, %ld unsupported by NYC, %ld "
         "oracle disputes, %ld miscompiled, %ld link failures\n",
         pool.total, pool.counts[ST_OK], pool.counts[ST_INVALID],
         pool.counts[ST_UNSUPPORTED], pool.counts[ST_DISPUTE],
         pool.counts[ST_MISCOMPILE], pool.counts[ST_LINKFAIL]);
  long compiles = g_nyc_compiles ? g_nyc_compiles : 1;
  long per_compile_ms = g_nyc_cpu_ms / compiles;
  printf("nyc compile cost: %ld compiles, %ld ms CPU each, peak RSS %ld MB\n",
         g_nyc_compiles, per_compile_ms, g_nyc_rss_kb / 1024);
  bool failed = pool.counts[ST_MISCOMPILE] || pool.counts[ST_LINKFAIL] ||
                (cfg->strict && pool.counts[ST_UNSUPPORTED]);
  if (cfg->baseline) {
    long base_ms = 0, base_kb = 0;
    FILE *f = fopen(cfg->baseline, "r");
    if (f && fscanf(f, "%ld %ld", &base_ms, &base_kb) == 2 && !cfg->save_baseline) {
      /* Generous bounds: this guards against regressions, not noise. */
      if (base_ms > 0 && per_compile_ms > 2 * base_ms + 50) {
        printf("compile-time regression: %ld ms against a %ld ms baseline\n",
               per_compile_ms, base_ms);
        failed = true;
      }
      if (base_kb > 0 && g_nyc_rss_kb > 2 * base_kb) {
        printf("peak-memory regression: %ld MB against a %ld MB baseline\n",
               g_nyc_rss_kb / 1024, base_kb / 1024);
        failed = true;
      }
    }
    if (f)
      fclose(f);
    if (cfg->save_baseline) {
      FILE *out = fopen(cfg->baseline, "w");
      if (out) {
        fprintf(out, "%ld %ld\n", per_compile_ms, g_nyc_rss_kb);
        fclose(out);
      }
    }
  }
  return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Reducer                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
  char **line;
  size_t count;
} lines_t;

static lines_t split_lines(const char *text) {
  lines_t l = {0};
  size_t cap = 0;
  const char *p = text;
  while (*p) {
    const char *end = strchr(p, '\n');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (l.count == cap) {
      cap = cap ? cap * 2 : 128;
      l.line = realloc(l.line, cap * sizeof(*l.line));
    }
    l.line[l.count++] = strndup(p, n);
    p += n + (end ? 1 : 0);
  }
  return l;
}

static void free_lines(lines_t *l) {
  for (size_t i = 0; i < l->count; ++i)
    free(l->line[i]);
  free(l->line);
  *l = (lines_t){0};
}

static char *join_lines(char *const *line, size_t count, size_t skip_from,
                        size_t skip_len, size_t replace_at,
                        const char *replacement) {
  sb_t b = {0};
  for (size_t i = 0; i < count; ++i) {
    if (i >= skip_from && i < skip_from + skip_len)
      continue;
    sb_addf(&b, "%s\n", i == replace_at && replacement ? replacement : line[i]);
  }
  sb_reserve(&b, 0);
  if (!b.data)
    b.data = strdup("");
  return b.data;
}

typedef struct {
  const config_t *cfg;
  const char *dir;
  const char *opt;
  const char *reject; /* reduce a compile failure whose message has this text */
  int route;          /* 0 object, 1 direct encoder, 2 assembly */
} reducer_t;

/* GCC must accept the program cleanly and NYC must still differ. */
static bool still_bad(const reducer_t *rd, const char *text) {
  char src[PATH_MAX], exe_ref[PATH_MAX], exe_n[PATH_MAX], obj[PATH_MAX],
      out_ref[PATH_MAX], out_n[PATH_MAX];
  path_in(src, sizeof(src), rd->dir, "r.c");
  path_in(exe_ref, sizeof(exe_ref), rd->dir, "rg");
  path_in(exe_n, sizeof(exe_n), rd->dir, "rn");
  path_in(obj, sizeof(obj), rd->dir, "r.o");
  path_in(out_ref, sizeof(out_ref), rd->dir, "rg.out");
  path_in(out_n, sizeof(out_n), rd->dir, "rn.out");
  if (!write_file(src, text, strlen(text)))
    return false;
  char *ref_argv[] = {(char *)rd->cfg->ref, "-Wall", "-Werror=return-type",
                      "-Werror=uninitialized", "-Wno-overflow", "-Wno-unused",
                      "-O1", src, "-o", exe_ref, NULL};
  if (run_cmd(ref_argv, NULL, NULL, rd->cfg->timeout_s * 4) != 0)
    return false;
  char *run_ref[] = {exe_ref, NULL};
  if (run_cmd(run_ref, out_ref, NULL, rd->cfg->timeout_s) != 0)
    return false;
  char *nyc_argv[] = {(char *)rd->cfg->nyc, "-c", (char *)rd->opt, src, "-o", obj,
                      NULL};
  if (rd->reject) {
    char err_path[PATH_MAX];
    path_in(err_path, sizeof(err_path), rd->dir, "r.err");
    int status = run_cmd(nyc_argv, NULL, err_path, rd->cfg->timeout_s * 4);
    sb_t text = {0};
    read_file(err_path, &text);
    bool match = status != 0 && text.data && strstr(text.data, rd->reject);
    sb_free(&text);
    return match;
  }
  if (rd->route == 2) {
    char asm_path[PATH_MAX];
    path_in(asm_path, sizeof(asm_path), rd->dir, "r.s");
    char *asm_argv[] = {(char *)rd->cfg->nyc, "-S", (char *)rd->opt, src, "-o",
                        asm_path, NULL};
    if (run_cmd(asm_argv, NULL, NULL, rd->cfg->timeout_s * 4) != 0)
      return false;
    char *as_argv[] = {(char *)rd->cfg->ref, "-w", "-c", asm_path, "-o", obj,
                       NULL};
    if (run_cmd(as_argv, NULL, NULL, rd->cfg->timeout_s * 4) != 0)
      return false;
  } else if (rd->route == 1) {
    char *direct_argv[] = {"env", "NYC_NO_MACH_OBJECT=1", (char *)rd->cfg->nyc,
                           "-c", (char *)rd->opt, src, "-o", obj, NULL};
    if (run_cmd(direct_argv, NULL, NULL, rd->cfg->timeout_s * 4) != 0)
      return false;
  } else if (run_cmd(nyc_argv, NULL, NULL, rd->cfg->timeout_s * 4) != 0) {
    return false;
  }
  char *link_argv[] = {(char *)rd->cfg->ref, obj, "-o", exe_n, NULL};
  if (run_cmd(link_argv, NULL, NULL, rd->cfg->timeout_s * 4) != 0)
    return false;
  char *run_n[] = {exe_n, NULL};
  int rc = run_cmd(run_n, out_n, NULL, rd->cfg->timeout_s);
  sb_t a = {0}, b = {0};
  read_file(out_ref, &a);
  read_file(out_n, &b);
  bool differ = rc != 0 || a.len != b.len ||
                (a.len && memcmp(a.data, b.data, a.len) != 0);
  sb_free(&a);
  sb_free(&b);
  return differ;
}

static bool is_temp_line(const char *line, char **prefix_out) {
  /* "  uint64_t eNN = <expr>;" */
  const char *p = line;
  if (strncmp(p, "  uint64_t e", 12) != 0)
    return false;
  const char *eq = strstr(p, " = ");
  if (!eq)
    return false;
  size_t len = strlen(line);
  if (len < 2 || line[len - 1] != ';')
    return false;
  *prefix_out = strndup(line, (size_t)(eq - line) + 3);
  return true;
}

static int command_reduce(const config_t *cfg, const char *dir, const char *opt,
                          const char *reject, int route) {
  char path[PATH_MAX];
  path_in(path, sizeof(path), dir, "p.c");
  sb_t text = {0};
  if (!read_file(path, &text) || !text.data) {
    fprintf(stderr, "ny-fuzz: cannot read %s\n", path);
    return 2;
  }
  reducer_t rd = {.cfg = cfg, .dir = dir, .opt = opt, .reject = reject, .route = route};
  if (!still_bad(&rd, text.data)) {
    fprintf(stderr, "ny-fuzz: %s does not differ at %s\n", path, opt);
    sb_free(&text);
    return 1;
  }
  lines_t lines = split_lines(text.data);
  sb_free(&text);
  /* Phase 1: delete chunks of lines, halving the chunk size. */
  for (size_t chunk = lines.count / 2 ? lines.count / 2 : 1; chunk >= 1;
       chunk /= 2) {
    bool changed = true;
    while (changed) {
      changed = false;
      for (long i = (long)lines.count - (long)chunk; i >= 0; i -= (long)chunk) {
        char *trial = join_lines(lines.line, lines.count, (size_t)i, chunk,
                                 SIZE_MAX, NULL);
        if (still_bad(&rd, trial)) {
          free_lines(&lines);
          lines = split_lines(trial);
          changed = true;
        }
        free(trial);
        if ((size_t)i >= lines.count)
          i = (long)lines.count;
      }
    }
    if (chunk == 1)
      break;
  }
  /* Phase 2: replace each temporary's initialiser with 0 or 1. */
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < lines.count; ++i) {
      char *prefix = NULL;
      if (!is_temp_line(lines.line[i], &prefix))
        continue;
      for (int v = 0; v < 2; ++v) {
        char replacement[256];
        snprintf(replacement, sizeof(replacement), "%s%d;", prefix, v);
        if (strcmp(replacement, lines.line[i]) == 0)
          break;
        char *trial =
            join_lines(lines.line, lines.count, SIZE_MAX, 0, i, replacement);
        if (still_bad(&rd, trial)) {
          free_lines(&lines);
          lines = split_lines(trial);
          changed = true;
          free(trial);
          break;
        }
        free(trial);
      }
      free(prefix);
      if (changed)
        break;
    }
  }
  char *final_text = join_lines(lines.line, lines.count, SIZE_MAX, 0, SIZE_MAX, NULL);
  char min_path[PATH_MAX];
  path_in(min_path, sizeof(min_path), dir, "min.c");
  write_file(min_path, final_text, strlen(final_text));
  fputs(final_text, stdout);
  free(final_text);
  free_lines(&lines);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

static void usage(void) {
  fputs(
      "usage: ny-fuzz run    [options]\n"
      "       ny-fuzz gen    --seed N [--small]\n"
      "       ny-fuzz reduce --dir CASEDIR [--opt -O0] [--reject TEXT] [options]\n"
      "\n"
      "Compiles seeded, undefined-behaviour-free C programs with a reference\n"
      "compiler and with NYC and compares program output. Failing cases stay\n"
      "in the output directory; `reduce` shrinks one by deleting lines.\n"
      "\n"
      "options:\n"
      "  --nyc PATH     NYC driver (default: nyc)\n"
      "  --ref CC       reference compiler (default: gcc)\n"
      "  --out DIR      case directory (default: ny-fuzz-out)\n"
      "  --seeds A:B    seed range, A inclusive, B exclusive (default 0:200)\n"
      "  --gen LIST     builtin, yarpgen, csmith or all (comma separated)\n"
      "  --opts LIST    NYC optimization levels (default -O0,-O2)\n"
      "  --jobs N       parallel workers (default: half the CPUs, at most 4)\n"
      "  --timeout S    per-command timeout in seconds (default 30)\n"
      "  --profile P    builtin program shape: all (default), int, struct, bits, va, atomic, alias, gnu\n"
      "  --routes LIST  NYC code routes: object (default), direct, asm\n"
      "  --ref2 CC      second reference compiler; disagreements are oracle disputes\n"
      "  --baseline F   compare compile cost with F (--save-baseline writes it)\n"
      "  --small        smaller builtin programs\n"
      "  --keep         keep passing cases\n"
      "  --strict       count programs NYC rejects as failures\n"
      "\n"
      "yarpgen and csmith run only when installed; csmith also needs its\n"
      "runtime headers (set CSMITH_INCLUDE).\n",
      stderr);
}

static int parse_gens(const char *list) {
  int mask = 0;
  char *copy = strdup(list);
  for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
    if (!strcmp(tok, "all"))
      mask |= GEN_BUILTIN | GEN_YARPGEN | GEN_CSMITH;
    else if (!strcmp(tok, "builtin"))
      mask |= GEN_BUILTIN;
    else if (!strcmp(tok, "yarpgen"))
      mask |= GEN_YARPGEN;
    else if (!strcmp(tok, "csmith"))
      mask |= GEN_CSMITH;
    else
      mask = -1;
  }
  free(copy);
  return mask;
}

static void parse_opts(config_t *cfg, const char *list) {
  cfg->opt_count = 0;
  char *copy = strdup(list);
  for (char *tok = strtok(copy, ","); tok && cfg->opt_count < 4;
       tok = strtok(NULL, ","))
    snprintf(cfg->opts[cfg->opt_count++], sizeof(cfg->opts[0]), "%s", tok);
  free(copy);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const char *command = argv[1];
  config_t cfg = {.nyc = "nyc",
                  .ref = "gcc",
                  .outdir = "ny-fuzz-out",
                  .seed_lo = 0,
                  .seed_hi = 200,
                  .gens = GEN_BUILTIN,
                  .profile = -1,
                  .routes = 1,
                  .timeout_s = 30};
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  cfg.jobs = cpus > 4 ? 4 : cpus > 1 ? (int)cpus / 2 : 1; /* stay gentle */
  parse_opts(&cfg, "-O0,-O2");
  const char *case_dir = NULL;
  const char *reduce_opt = "-O0";
  const char *reduce_reject = NULL;
  long gen_seed = 0;
  for (int i = 2; i < argc; ++i) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--nyc") && v)
      cfg.nyc = argv[++i];
    else if (!strcmp(a, "--ref") && v)
      cfg.ref = argv[++i];
    else if (!strcmp(a, "--out") && v)
      cfg.outdir = argv[++i];
    else if (!strcmp(a, "--dir") && v)
      case_dir = argv[++i];
    else if (!strcmp(a, "--reject") && v)
      reduce_reject = argv[++i];
    else if (!strcmp(a, "--opt") && v)
      reduce_opt = argv[++i];
    else if (!strcmp(a, "--routes") && v) {
      cfg.routes = 0;
      char *list = strdup(argv[++i]);
      for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ","))
        cfg.routes |= !strcmp(tok, "object") ? 1 : !strcmp(tok, "direct") ? 2
                      : !strcmp(tok, "asm")  ? 4 : 0;
      free(list);
      if (!cfg.routes) {
        usage();
        return 2;
      }
    } else if (!strcmp(a, "--ref2") && v)
      cfg.ref2 = argv[++i];
    else if (!strcmp(a, "--baseline") && v)
      cfg.baseline = argv[++i];
    else if (!strcmp(a, "--profile") && v) {
      const char *name = argv[++i];
      cfg.profile = !strcmp(name, "all")      ? -1
                    : !strcmp(name, "int")    ? 0
                    : !strcmp(name, "struct") ? 1
                    : !strcmp(name, "bits")   ? 2
                    : !strcmp(name, "va")     ? 3
                    : !strcmp(name, "atomic") ? 4
                    : !strcmp(name, "alias")  ? 5
                    : !strcmp(name, "gnu")    ? 6
                                              : atoi(name);
    } else if (!strcmp(a, "--save-baseline"))
      cfg.save_baseline = true;
    else if (!strcmp(a, "--opts") && v)
      parse_opts(&cfg, argv[++i]);
    else if (!strcmp(a, "--jobs") && v)
      cfg.jobs = atoi(argv[++i]);
    else if (!strcmp(a, "--timeout") && v)
      cfg.timeout_s = atoi(argv[++i]);
    else if (!strcmp(a, "--seed") && v)
      gen_seed = atol(argv[++i]);
    else if (!strcmp(a, "--seeds") && v) {
      if (sscanf(argv[++i], "%ld:%ld", &cfg.seed_lo, &cfg.seed_hi) != 2) {
        usage();
        return 2;
      }
    } else if (!strcmp(a, "--gen") && v) {
      cfg.gens = parse_gens(argv[++i]);
      if (cfg.gens <= 0) {
        usage();
        return 2;
      }
    } else if (!strcmp(a, "--small"))
      cfg.small = true;
    else if (!strcmp(a, "--keep"))
      cfg.keep = true;
    else if (!strcmp(a, "--strict"))
      cfg.strict = true;
    else {
      usage();
      return 2;
    }
  }
  if (!strcmp(command, "run"))
    return command_run(&cfg);
  if (!strcmp(command, "gen")) {
    sb_t src = {0};
    gen_builtin((uint64_t)gen_seed, cfg.small, cfg.profile, &src);
    fwrite(src.data, 1, src.len, stdout);
    sb_free(&src);
    return 0;
  }
  if (!strcmp(command, "reduce")) {
    if (!case_dir) {
      usage();
      return 2;
    }
    return command_reduce(&cfg, case_dir, reduce_opt, reduce_reject,
                          cfg.routes == 4 ? 2 : cfg.routes == 2 ? 1 : 0);
  }
  usage();
  return 2;
}
