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

/*
 * Run argv with stdout and stderr captured into files. Returns the exit
 * status, 128 + signal for a signalled child, RUN_TIMEOUT when the child
 * outlived `timeout_s`, or RUN_SPAWN_FAILED.
 */
static int run_cmd(char *const argv[], const char *out_path,
                   const char *err_path, int timeout_s) {
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
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) {
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

static void gen_builtin(uint64_t seed, bool small, sb_t *out) {
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

/* ------------------------------------------------------------------ */
/* Case checking                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
  ST_OK,
  ST_INVALID,
  ST_UNSUPPORTED,
  ST_MISCOMPILE,
  ST_LINKFAIL,
  ST_COUNT
} status_t;

static const char *const k_status_names[ST_COUNT] = {
    "ok", "invalid", "unsupported", "MISCOMPILE", "link-fail"};

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
  rc = run_cmd(run_ref, want_out, NULL, cfg->timeout_s);
  if (rc != 0)
    return outcome(ST_INVALID, "reference run exited %d", rc);
  sb_t want = {0};
  read_file(want_out, &want);
  outcome_t result = outcome(ST_OK, "");
  for (int o = 0; o < cfg->opt_count && result.status == ST_OK; ++o) {
    const char *opt = cfg->opts[o];
    char obj[PATH_MAX], exe[PATH_MAX], got_out[PATH_MAX], name[48];
    snprintf(name, sizeof(name), "n%s.o", opt);
    path_in(obj, sizeof(obj), c->dir, name);
    snprintf(name, sizeof(name), "n%s", opt);
    path_in(exe, sizeof(exe), c->dir, name);
    snprintf(name, sizeof(name), "n%s.out", opt);
    path_in(got_out, sizeof(got_out), c->dir, name);
    char *nyc_argv[] = {(char *)cfg->nyc, "-c", (char *)opt, inc, (char *)c->nyc_unit,
                        "-o", obj, NULL};
    rc = run_cmd(nyc_argv, NULL, err, cfg->timeout_s * 4);
    if (rc != 0) {
      char why[160];
      first_line(err, why, sizeof(why));
      result = outcome(ST_UNSUPPORTED, "nyc %s rejected: %s", opt, why);
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
      result = outcome(ST_LINKFAIL, "link %s: %s", opt, why);
      break;
    }
    char *run_n[] = {exe, NULL};
    rc = run_cmd(run_n, got_out, NULL, cfg->timeout_s);
    sb_t got = {0};
    read_file(got_out, &got);
    bool same = rc == 0 && got.len == want.len &&
                (!want.len || memcmp(got.data, want.data, want.len) == 0);
    if (!same) {
      char w[64] = "", gtxt[64] = "";
      if (want.data)
        snprintf(w, sizeof(w), "%.*s", (int)strcspn(want.data, "\n"), want.data);
      if (got.data)
        snprintf(gtxt, sizeof(gtxt), "%.*s", (int)strcspn(got.data, "\n"),
                 got.data);
      result = outcome(ST_MISCOMPILE, "%s: want [%s] got [%s] rc=%d", opt, w,
                       gtxt, rc);
    }
    sb_free(&got);
  }
  sb_free(&want);
  return result;
}

/* ------------------------------------------------------------------ */
/* Generators that drive external tools                                */
/* ------------------------------------------------------------------ */

static outcome_t run_builtin_seed(const config_t *cfg, long seed, const char *dir) {
  sb_t src = {0};
  gen_builtin((uint64_t)seed, cfg->small, &src);
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
    bool keep = cfg->keep || o.status == ST_MISCOMPILE || o.status == ST_LINKFAIL;
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
         "miscompiled, %ld link failures\n",
         pool.total, pool.counts[ST_OK], pool.counts[ST_INVALID],
         pool.counts[ST_UNSUPPORTED], pool.counts[ST_MISCOMPILE],
         pool.counts[ST_LINKFAIL]);
  bool failed = pool.counts[ST_MISCOMPILE] || pool.counts[ST_LINKFAIL] ||
                (cfg->strict && pool.counts[ST_UNSUPPORTED]);
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
  if (run_cmd(nyc_argv, NULL, NULL, rd->cfg->timeout_s * 4) != 0)
    return false;
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

static int command_reduce(const config_t *cfg, const char *dir, const char *opt) {
  char path[PATH_MAX];
  path_in(path, sizeof(path), dir, "p.c");
  sb_t text = {0};
  if (!read_file(path, &text) || !text.data) {
    fprintf(stderr, "ny-fuzz: cannot read %s\n", path);
    return 2;
  }
  reducer_t rd = {.cfg = cfg, .dir = dir, .opt = opt};
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
      "       ny-fuzz reduce --dir CASEDIR [--opt -O0] [options]\n"
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
                  .timeout_s = 30};
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  cfg.jobs = cpus > 4 ? 4 : cpus > 1 ? (int)cpus / 2 : 1; /* stay gentle */
  parse_opts(&cfg, "-O0,-O2");
  const char *case_dir = NULL;
  const char *reduce_opt = "-O0";
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
    else if (!strcmp(a, "--opt") && v)
      reduce_opt = argv[++i];
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
    gen_builtin((uint64_t)gen_seed, cfg.small, &src);
    fwrite(src.data, 1, src.len, stdout);
    sb_free(&src);
    return 0;
  }
  if (!strcmp(command, "reduce")) {
    if (!case_dir) {
      usage();
      return 2;
    }
    return command_reduce(&cfg, case_dir, reduce_opt);
  }
  usage();
  return 2;
}
