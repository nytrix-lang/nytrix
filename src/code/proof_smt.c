/*
 * Solver-backed proofs.
 *
 * Two backends, both used only as *acceptance* evidence:
 *
 *   1. ny_proof_try_presburger — dependency-free Presburger subset using
 *      Fourier–Motzkin elimination over linear integer arithmetic
 *      (`+`, `-`, `*const`, comparisons, `&&`/`||`/`!`).  Bounded to
 *      8 variables / 32 constraints; bails to "unknown" on anything else.
 *
 *   2. ny_proof_try_z3 — full Z3-backed path (only when NYTRIX_HAS_Z3 is
 *      defined; the compiler is configured with `NYTRIX_ENABLE_Z3`).
 *
 * Both return true only when the negation of the proposition is proven
 * unsatisfiable, so they can never admit a false proof obligation.  They
 * are conservative: every unknown result falls through to the existing
 * constant-evaluation path in the caller.
 *
 * The caller (ny_try_static_assert_builtin) invokes them after the
 */
#include "priv.h"
#include "parse/proof.h"

#ifdef NYTRIX_HAS_Z3
#include <z3.h>
#endif

#include <stdarg.h>

#define NY_SMT_MAX_VARS 8
#define NY_SMT_MAX_CONS 32
#define NY_SMT_MAX_BRANCHES 16
#define NY_SMT_MAX_DEPTH 64

/*
 * ---------- proof-system tracing ----------
 *
 * Gated by NYTRIX_PROOF_DEBUG (fallback NY_PROOF_DEBUG). Level 0 disables,
 * an integer sets the level, any other truthy value sets level 1. Output goes
 * to stderr with a "[proof debug]" prefix; it is purely diagnostic and never
 * feeds back into a decision.
 */
int ny_proof_debug_level(void) {
  const char *v = getenv("NYTRIX_PROOF_DEBUG");
  if (!v || !*v)
    v = getenv("NY_PROOF_DEBUG");
  if (!v || !*v)
    return 0;
  if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
      strcmp(v, "off") == 0 || strcmp(v, "no") == 0)
    return 0;
  char *end = NULL;
  long n = strtol(v, &end, 10);
  if (end && *end == '\0' && n >= 1 && n <= 9)
    return (int)n;
  return 1;
}

bool ny_proof_debug_enabled(void) {
  return ny_proof_debug_level() > 0;
}

void ny_proof_debug(int level, const char *fmt, ...) {
  if (level <= 0 || ny_proof_debug_level() < level || !fmt)
    return;
  fputs("[proof debug] ", stderr);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

void ny_proof_debug_at(int level, token_t tok, const char *fmt, ...) {
  if (level <= 0 || ny_proof_debug_level() < level || !fmt)
    return;
  fprintf(stderr, "[proof debug] %s:%d:%d: ",
          tok.filename ? tok.filename : "unknown", tok.line, tok.col);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

/*
 * A variable is a named integer binding with an optional known range.
 */
typedef struct {
  const char *name;
  int64_t lo, hi;
  bool has_range;
} ny_smt_var_t;

/*
 * Σ coeff[i]*x_i ≤ rhs over the first nvars variables of an env.
 */
typedef struct {
  int64_t coeff[NY_SMT_MAX_VARS];
  int64_t rhs;
  int nvars;
} ny_smt_cons_t;

typedef struct {
  ny_smt_var_t vars[NY_SMT_MAX_VARS];
  int nvars;
} ny_smt_env_t;

static int ny_smt_env_add(ny_smt_env_t *env, const char *name) {
  for (int i = 0; i < env->nvars; ++i) {
    if (strcmp(env->vars[i].name, name) == 0)
      return i;
  }
  if (env->nvars >= NY_SMT_MAX_VARS)
    return -1;
  int i = env->nvars++;
  env->vars[i].name = name;
  env->vars[i].lo = 0;
  env->vars[i].hi = 0;
  env->vars[i].has_range = false;
  return i;
}

static int ny_smt_env_add_binding(codegen_t *cg, scope *scopes, size_t depth,
                                  ny_smt_env_t *env, const char *name) {
  int i = ny_smt_env_add(env, name);
  if (i < 0)
    return -1;
  if (env->vars[i].has_range)
    return i;
  size_t name_len = strlen(name);
  binding *b = lookup_binding_hash(cg, scopes, depth, name, name_len,
                                   ny_hash_name(name, name_len));
  if (b && b->has_int_range) {
    env->vars[i].lo = b->int_min_raw;
    env->vars[i].hi = b->int_max_raw;
    env->vars[i].has_range = true;
  }
  return i;
}

static bool ny_smt_mul_i64(int64_t a, int64_t b, int64_t *out) {
  __int128 p = (__int128)a * b;
  if (p < INT64_MIN || p > INT64_MAX)
    return false;
  *out = (int64_t)p;
  return true;
}

/*
 * ---------- linear expression builder (presburger) ----------
 */

typedef struct {
  int64_t coeff[NY_SMT_MAX_VARS];
  int64_t cconst;
  bool ok;
} ny_smt_lin_t;

static void ny_smt_lin_zero(ny_smt_lin_t *lin) {
  memset(lin, 0, sizeof(*lin));
  lin->ok = true;
}

static bool ny_smt_lin_build(codegen_t *cg, scope *scopes, size_t depth,
                             ny_smt_env_t *env, expr_t *e, ny_smt_lin_t *out,
                             unsigned rec) {
  ny_smt_lin_zero(out);
  if (!e || rec > NY_SMT_MAX_DEPTH)
    return (out->ok = false);
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT) {
    if (ny_expr_is_nil_literal(e))
      return (out->ok = false);
    out->cconst = e->as.literal.as.i;
    return true;
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    int vi = ny_smt_env_add_binding(cg, scopes, depth, env, e->as.ident.name);
    if (vi < 0)
      return (out->ok = false);
    out->coeff[vi] = 1;
    return true;
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op && e->as.unary.right &&
      strcmp(e->as.unary.op, "-") == 0) {
    ny_smt_lin_t inner;
    if (!ny_smt_lin_build(cg, scopes, depth, env, e->as.unary.right, &inner,
                          rec + 1))
      return false;
    for (int i = 0; i < env->nvars; ++i)
      out->coeff[i] = -inner.coeff[i];
    out->cconst = -inner.cconst;
    return true;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op && e->as.binary.left &&
      e->as.binary.right) {
    const char *op = e->as.binary.op;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) {
      ny_smt_lin_t l, r;
      if (!ny_smt_lin_build(cg, scopes, depth, env, e->as.binary.left, &l,
                            rec + 1) ||
          !ny_smt_lin_build(cg, scopes, depth, env, e->as.binary.right, &r,
                            rec + 1))
        return false;
      int sgn = strcmp(op, "+") == 0 ? 1 : -1;
      __int128 c = (__int128)l.cconst + (__int128)sgn * r.cconst;
      if (c < INT64_MIN || c > INT64_MAX)
        return (out->ok = false);
      out->cconst = (int64_t)c;
      for (int i = 0; i < env->nvars; ++i) {
        __int128 t = (__int128)l.coeff[i] + (__int128)sgn * r.coeff[i];
        if (t < INT64_MIN || t > INT64_MAX)
          return (out->ok = false);
        out->coeff[i] = (int64_t)t;
      }
      return true;
    }
    if (strcmp(op, "*") == 0) {
      /*
       * only *const is linear (Presburger)
       */
      ny_smt_lin_t l, r;
      bool lok = ny_smt_lin_build(cg, scopes, depth, env, e->as.binary.left,
                                  &l, rec + 1);
      bool rok = ny_smt_lin_build(cg, scopes, depth, env, e->as.binary.right,
                                  &r, rec + 1);
      if (!lok || !rok)
        return false;
      bool l_const = true, r_const = true;
      for (int i = 0; i < env->nvars; ++i) {
        if (l.coeff[i] != 0)
          l_const = false;
        if (r.coeff[i] != 0)
          r_const = false;
      }
      if (l_const && r_const) {
        __int128 p = (__int128)l.cconst * r.cconst;
        if (p < INT64_MIN || p > INT64_MAX)
          return (out->ok = false);
        out->cconst = (int64_t)p;
        return true;
      }
      if (r_const) {
        __int128 k = r.cconst;
        for (int i = 0; i < env->nvars; ++i) {
          __int128 p = (__int128)l.coeff[i] * k;
          if (p < INT64_MIN || p > INT64_MAX)
            return (out->ok = false);
          out->coeff[i] = (int64_t)p;
        }
        __int128 p = (__int128)l.cconst * k;
        if (p < INT64_MIN || p > INT64_MAX)
          return (out->ok = false);
        out->cconst = (int64_t)p;
        return true;
      }
      if (l_const) {
        __int128 k = l.cconst;
        for (int i = 0; i < env->nvars; ++i) {
          __int128 p = (__int128)r.coeff[i] * k;
          if (p < INT64_MIN || p > INT64_MAX)
            return (out->ok = false);
          out->coeff[i] = (int64_t)p;
        }
        __int128 p = (__int128)r.cconst * k;
        if (p < INT64_MIN || p > INT64_MAX)
          return (out->ok = false);
        out->cconst = (int64_t)p;
        return true;
      }
      return (out->ok = false); /* nonlinear: not Presburger */
    }
    return (out->ok = false);
  }
  return (out->ok = false);
}

/*
 * ---------- Fourier–Motzkin elimination ----------
 */

/*
 * Test feasibility of a constraint system over the first nvars variables.
 * Returns true when the system is proven UNSAT (infeasible), false when it
 * may be feasible (or the check was abandoned).  Sound by construction:
 * elimination only ever produces equisatisfiable systems, so a derived
 * constant contradiction proves genuine infeasibility.
 */
static bool ny_smt_fm_unsat(const ny_smt_cons_t *cons, int ncons, int nvars) {
  if (ncons > NY_SMT_MAX_CONS)
    return false;
  ny_smt_cons_t work[NY_SMT_MAX_CONS];
  memcpy(work, cons, sizeof(ny_smt_cons_t) * (size_t)ncons);
  int nwork = ncons;
  for (int k = 0; k < nvars; ++k) {
    int nu = 0, nl = 0, nz = 0;
    ny_smt_cons_t up[NY_SMT_MAX_CONS], lo[NY_SMT_MAX_CONS],
        zr[NY_SMT_MAX_CONS];
    for (int c = 0; c < nwork; ++c) {
      int64_t a = work[c].coeff[k];
      if (a > 0) {
        if (nu < NY_SMT_MAX_CONS)
          up[nu++] = work[c];
      } else if (a < 0) {
        if (nl < NY_SMT_MAX_CONS)
          lo[nl++] = work[c];
      } else {
        if (nz < NY_SMT_MAX_CONS)
          zr[nz++] = work[c];
      }
    }
    int nnew = 0;
    ny_smt_cons_t next[NY_SMT_MAX_CONS];
    for (int i = 0; i < nz && nnew < NY_SMT_MAX_CONS; ++i)
      next[nnew++] = zr[i];
    if (nu > 0 && nl > 0) {
      /*
       * Combine every (upper, lower) pair: a_u*x_k ≤ b_u - R_u and
       * a_l*x_k ≤ b_l - R_l with a_u>0, a_l<0 give
       *   a_u*R_l - a_l*R_u ≤ a_u*b_l - a_l*b_u
       * (derived by cross-multiplying x_k ≤ (b_u-R_u)/a_u against
       *  x_k ≥ (b_l-R_l)/a_l and flipping on the negative product).
       */
      for (int u = 0; u < nu && nnew < NY_SMT_MAX_CONS; ++u) {
        for (int l = 0; l < nl && nnew < NY_SMT_MAX_CONS; ++l) {
          int64_t a_u = up[u].coeff[k];
          int64_t a_l = lo[l].coeff[k];
          ny_smt_cons_t nc;
          memset(&nc, 0, sizeof(nc));
          nc.nvars = nvars;
          bool ok = true;
          for (int i = 0; i < nvars && ok; ++i) {
            if (i == k)
              continue;
            int64_t p1 = 0, p2 = 0;
            if (!ny_smt_mul_i64(a_u, lo[l].coeff[i], &p1) ||
                !ny_smt_mul_i64(a_l, up[u].coeff[i], &p2) ||
                p1 - p2 < INT64_MIN || p1 - p2 > INT64_MAX)
              ok = false;
            else
              nc.coeff[i] = p1 - p2;
          }
          int64_t t1 = 0, t2 = 0;
          if (ok &&
              (!ny_smt_mul_i64(a_u, lo[l].rhs, &t1) ||
               !ny_smt_mul_i64(a_l, up[u].rhs, &t2) || t1 - t2 < INT64_MIN ||
               t1 - t2 > INT64_MAX))
            ok = false;
          if (ok) {
            nc.rhs = t1 - t2;
            next[nnew++] = nc;
          } else {
            return false; /* overflow: abandon, soundly */
          }
        }
      }
    }
    if (nnew > NY_SMT_MAX_CONS)
      return false;
    memcpy(work, next, sizeof(ny_smt_cons_t) * (size_t)nnew);
    nwork = nnew;
  }
  for (int c = 0; c < nwork; ++c) {
    bool all_zero = true;
    for (int i = 0; i < nvars; ++i) {
      if (work[c].coeff[i] != 0) {
        all_zero = false;
        break;
      }
    }
    if (all_zero && work[c].rhs < 0)
      return true; /* 0 ≤ negative: contradiction */
  }
  return false;
}

/*
 * Push the premises (known variable ranges) for a branch.
 */
static bool ny_smt_add_premises(const ny_smt_env_t *env, ny_smt_cons_t *out,
                                int *nout) {
  for (int v = 0; v < env->nvars; ++v) {
    if (!env->vars[v].has_range)
      continue;
    if (*nout >= NY_SMT_MAX_CONS)
      return false;
    ny_smt_cons_t c;
    memset(&c, 0, sizeof(c));
    c.nvars = env->nvars;
    c.coeff[v] = -1;
    c.rhs = -env->vars[v].lo; /* -x ≤ -lo  ⟺  x ≥ lo */
    if (c.rhs < INT64_MIN)
      return false;
    out[(*nout)++] = c;
    if (*nout >= NY_SMT_MAX_CONS)
      return false;
    memset(&c, 0, sizeof(c));
    c.nvars = env->nvars;
    c.coeff[v] = 1;
    c.rhs = env->vars[v].hi; /* x ≤ hi */
    out[(*nout)++] = c;
  }
  return true;
}

/*
 * ---------- proposition → DNF of its negation ----------
 */

/*
 * A branch is a conjunction of linear constraints; the DNF of ¬P is a list
 * of branches.  P is proven when every branch of ¬P is UNSAT under the
 * premises.
 */
typedef struct {
  ny_smt_cons_t cons[NY_SMT_MAX_CONS];
  int ncons;
} ny_smt_branch_t;

typedef struct {
  ny_smt_branch_t br[NY_SMT_MAX_BRANCHES];
  int nbr;
} ny_smt_dnf_t;

static void ny_smt_dnf_reset(ny_smt_dnf_t *d) { memset(d, 0, sizeof(*d)); }

static bool ny_smt_dnf_append(ny_smt_dnf_t *d, const ny_smt_branch_t *b) {
  if (d->nbr >= NY_SMT_MAX_BRANCHES)
    return false;
  d->br[d->nbr++] = *b;
  return true;
}

/*
 * Convert a checked linear constraint bound back to the storage type.  The
 * Presburger builder must return unknown rather than wrap an overflowing
 * coefficient or bound into a different proposition.
 */
static bool ny_smt_i128_to_i64(__int128 value, int64_t *out) {
  if (!out || value < INT64_MIN || value > INT64_MAX)
    return false;
  *out = (int64_t)value;
  return true;
}

/*
 * Convert a comparison into one conjunction of constraints.  `negate`
 * selects the DNF of the negation (¬cmp); otherwise the DNF of cmp itself.
 * Returns the number of branches produced (>= 1), or 0 on unsupported or
 * overflowing forms (== and != may produce two branches).
 */
static int ny_smt_cmp_branches(codegen_t *cg, scope *scopes, size_t depth,
                               ny_smt_env_t *env, expr_t *l, const char *op,
                               expr_t *r, bool negate, ny_smt_branch_t *out,
                               int out_cap) {
  if (!env || !out || out_cap <= 0 || out_cap > NY_SMT_MAX_BRANCHES)
    return 0;
  ny_smt_lin_t ll, rr;
  if (!ny_smt_lin_build(cg, scopes, depth, env, l, &ll, 0) ||
      !ny_smt_lin_build(cg, scopes, depth, env, r, &rr, 0))
    return 0;

  /*
   * diff = ll - rr
   */
  ny_smt_lin_t diff;
  ny_smt_lin_zero(&diff);
  for (int i = 0; i < env->nvars; ++i) {
    if (!ny_smt_i128_to_i64((__int128)ll.coeff[i] - rr.coeff[i],
                            &diff.coeff[i]))
      return 0;
  }
  if (!ny_smt_i128_to_i64((__int128)ll.cconst - rr.cconst, &diff.cconst))
    return 0;

  /*
   * Push a single constraint Σ coeff[i]*x_i ≤ rhs into branch b.  The macro
   * parameters are named `coeffs` and `rhs_val` (not `coeff`/`rhs`) so they
   * cannot collide with the ny_smt_cons_t members during substitution.
   */
#define NY_SMT_PUSH_CONS(branch, coeffs, rhs_val)                              \
  do {                                                                         \
    if ((branch)->ncons >= NY_SMT_MAX_CONS)                                    \
      return 0;                                                                \
    ny_smt_cons_t *c = &(branch)->cons[(branch)->ncons++];                     \
    memset(c, 0, sizeof(*c));                                                  \
    c->nvars = env->nvars;                                                     \
    for (int i = 0; i < env->nvars; ++i)                                       \
      c->coeff[i] = (coeffs)[i];                                               \
    c->rhs = (rhs_val);                                                        \
  } while (0)

  int nbr = 0;
  if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 ||
      strcmp(op, ">=") == 0) {
    /*
     * Encode `l op r` (or its negation when `negate`) as integer constraints
     * on diff = l - r.  Each comparison maps to one of four linear forms:
     *   LT: diff <= -1   (l < r)
     *   LE: diff <=  0   (l <= r)
     *   GT: -diff <= -1  (l > r)
     *   GE: -diff <=  0  (l >= r)
     * `negate` flips the relation first:
     *   ¬(a<b)=a>=b, ¬(a<=b)=a>b, ¬(a>b)=a<=b, ¬(a>=b)=a<b.
     * With diff = Σ coeff·x + cconst, the stored form Σcoeff'·x <= rhs is
     *   diff <= k  ⟺  Σcoeff·x <= k - cconst
     *   -diff <= k ⟺  -Σcoeff·x <= k + cconst
     */
    int rel = 0; /* 0=LT 1=LE 2=GT 3=GE */
    if (strcmp(op, "<") == 0)
      rel = negate ? 3 : 0;
    else if (strcmp(op, "<=") == 0)
      rel = negate ? 2 : 1;
    else if (strcmp(op, ">") == 0)
      rel = negate ? 1 : 2;
    else
      rel = negate ? 0 : 3;
    int64_t neg_coeff[NY_SMT_MAX_VARS];
    for (int i = 0; i < env->nvars; ++i) {
      if (!ny_smt_i128_to_i64(-(__int128)diff.coeff[i], &neg_coeff[i]))
        return 0;
    }
    ny_smt_branch_t b;
    memset(&b, 0, sizeof(b));
    int64_t rhs = 0;
    __int128 rhs128 = 0;
    switch (rel) {
    case 0: /* LT: diff <= -1 */
      rhs128 = -(__int128)1 - diff.cconst;
      break;
    case 1: /* LE: diff <= 0 */
      rhs128 = -(__int128)diff.cconst;
      break;
    case 2: /* GT: -diff <= -1 */
      rhs128 = (__int128)diff.cconst - 1;
      break;
    default: /* GE: -diff <= 0 */
      rhs128 = diff.cconst;
      break;
    }
    if (!ny_smt_i128_to_i64(rhs128, &rhs))
      return 0;
    NY_SMT_PUSH_CONS(&b, rel >= 2 ? neg_coeff : diff.coeff, rhs);
    out[nbr++] = b;
    return nbr;
  }
  if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
    bool eq = (strcmp(op, "==") == 0) == !negate;
    if (eq) {
      /*
       * a==b ⟺ (a-b <= 0) ∧ (b-a <= 0) : one branch, two constraints.
       */
      if (out_cap < 1)
        return 0;
      ny_smt_branch_t b;
      memset(&b, 0, sizeof(b));
      int64_t rhs_forward = 0;
      if (!ny_smt_i128_to_i64(-(__int128)diff.cconst, &rhs_forward))
        return 0;
      NY_SMT_PUSH_CONS(&b, diff.coeff, rhs_forward);
      for (int i = 0; i < env->nvars; ++i) {
        if (!ny_smt_i128_to_i64(-(__int128)diff.coeff[i],
                                &b.cons[1].coeff[i]))
          return 0;
      }
      b.cons[1].nvars = env->nvars;
      b.cons[1].rhs = diff.cconst;
      b.ncons = 2;
      out[nbr++] = b;
      return nbr;
    }
    /*
     * a!=b ⟺ (a-b <= -1) ∨ (b-a <= -1) : two branches.
     */
    if (out_cap < 2)
      return 0;
    ny_smt_branch_t b1, b2;
    memset(&b1, 0, sizeof(b1));
    memset(&b2, 0, sizeof(b2));
    int64_t rhs1 = 0, rhs2 = 0;
    if (!ny_smt_i128_to_i64(-(__int128)1 - diff.cconst, &rhs1) ||
        !ny_smt_i128_to_i64(diff.cconst - (__int128)1, &rhs2))
      return 0;
    NY_SMT_PUSH_CONS(&b1, diff.coeff, rhs1);
    for (int i = 0; i < env->nvars; ++i) {
      if (!ny_smt_i128_to_i64(-(__int128)diff.coeff[i],
                              &b2.cons[0].coeff[i]))
        return 0;
    }
    b2.cons[0].nvars = env->nvars;
    b2.cons[0].rhs = rhs2;
    b2.ncons = 1;
    out[nbr++] = b1;
    out[nbr++] = b2;
    return nbr;
  }
#undef NY_SMT_PUSH_CONS
  return 0;
}

static bool ny_smt_lemma_call_proved(codegen_t *cg, expr_t *e) {
  if (!cg || !e)
    return false;
  const char *name = NULL;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT)
    name = e->as.call.callee->as.ident.name;
  else if (e->kind == NY_E_MEMCALL)
    name = e->as.memcall.name;
  if (!name || !*name)
    return false;
  for (size_t i = 0; i < cg->registry.lemmas.len; ++i) {
    lemma_def_t *def = cg->registry.lemmas.data[i];
    if (def && def->proved && def->name && strcmp(def->name, name) == 0)
      return true;
  }
  return false;
}

/*
 * DNF of expr (or its negation when `negate`).
 */
static bool ny_smt_dnf_build(codegen_t *cg, scope *scopes, size_t depth,
                             ny_smt_env_t *env, expr_t *e, bool negate,
                             ny_smt_dnf_t *out, unsigned rec) {
  if (!e || rec > NY_SMT_MAX_DEPTH)
    return false;
  ny_smt_dnf_reset(out);
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_BOOL) {
    bool val = e->as.literal.as.b ^ negate;
    if (val) {
      /*
       * DNF of `true` is the empty conjunction (one branch, no cons).
       */
      ny_smt_branch_t b;
      memset(&b, 0, sizeof(b));
      return ny_smt_dnf_append(out, &b);
    }
    return true; /* DNF of `false` has no branches */
  }
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      !ny_expr_is_nil_literal(e)) {
    bool val = (e->as.literal.as.i != 0) ^ negate;
    if (val) {
      ny_smt_branch_t b;
      memset(&b, 0, sizeof(b));
      return ny_smt_dnf_append(out, &b);
    }
    return true;
  }
  if ((e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL) &&
      ny_smt_lemma_call_proved(cg, e)) {
    if (negate)
      return true; /* a universally proved lemma call is true */
    ny_smt_branch_t b;
    memset(&b, 0, sizeof(b));
    return ny_smt_dnf_append(out, &b);
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op && e->as.binary.left &&
      e->as.binary.right) {
    /*
     * Only relational comparisons are valid proposition atoms here.  A
     * non-comparison binary expression (e.g. a bare `x + 1`) or a linear
     * form we cannot build must be treated as unknown — never as a
     * trivially-true empty negation.
     */
    const char *op = e->as.binary.op;
    if (strcmp(op, "<") != 0 && strcmp(op, "<=") != 0 &&
        strcmp(op, ">") != 0 && strcmp(op, ">=") != 0 &&
        strcmp(op, "==") != 0 && strcmp(op, "!=") != 0)
      return false;
    ny_smt_branch_t brs[2];
    int nb = ny_smt_cmp_branches(cg, scopes, depth, env, e->as.binary.left,
                                 e->as.binary.op, e->as.binary.right, negate,
                                 brs, 2);
    if (nb == 0)
      return false; /* unsupported / non-linear operand: unknown */
    for (int i = 0; i < nb; ++i) {
      if (!ny_smt_dnf_append(out, &brs[i]))
        return false;
    }
    return true;
  }
  if (e->kind == NY_E_LOGICAL && e->as.logical.op && e->as.logical.left &&
      e->as.logical.right) {
    const char *op = e->as.logical.op;
    ny_smt_dnf_t l, r;
    bool l_ok = ny_smt_dnf_build(cg, scopes, depth, env, e->as.logical.left,
                                 negate, &l, rec + 1);
    bool r_ok = ny_smt_dnf_build(cg, scopes, depth, env, e->as.logical.right,
                                 negate, &r, rec + 1);
    if (!l_ok || !r_ok)
      return false;
    if (strcmp(op, "&&") == 0) {
      if (negate) {
        /*
         * ¬(A∧B) = ¬A ∨ ¬B : union of branch lists
         */
        for (int i = 0; i < l.nbr; ++i) {
          if (!ny_smt_dnf_append(out, &l.br[i]))
            return false;
        }
        for (int i = 0; i < r.nbr; ++i) {
          if (!ny_smt_dnf_append(out, &r.br[i]))
            return false;
        }
      } else {
        /*
         * A∧B : cross product (both must hold)
         */
        for (int i = 0; i < l.nbr; ++i) {
          for (int j = 0; j < r.nbr; ++j) {
            ny_smt_branch_t b;
            memset(&b, 0, sizeof(b));
            if (l.br[i].ncons + r.br[j].ncons > NY_SMT_MAX_CONS)
              return false;
            memcpy(b.cons, l.br[i].cons,
                   sizeof(ny_smt_cons_t) * (size_t)l.br[i].ncons);
            b.ncons = l.br[i].ncons;
            memcpy(b.cons + b.ncons, r.br[j].cons,
                   sizeof(ny_smt_cons_t) * (size_t)r.br[j].ncons);
            b.ncons += r.br[j].ncons;
            if (!ny_smt_dnf_append(out, &b))
              return false;
          }
        }
      }
      return true;
    }
    /*
     * ||
     */
    if (negate) {
      /*
       * ¬(A∨B) = ¬A ∧ ¬B : cross product
       */
      for (int i = 0; i < l.nbr; ++i) {
        for (int j = 0; j < r.nbr; ++j) {
          ny_smt_branch_t b;
          memset(&b, 0, sizeof(b));
          if (l.br[i].ncons + r.br[j].ncons > NY_SMT_MAX_CONS)
            return false;
          memcpy(b.cons, l.br[i].cons,
                 sizeof(ny_smt_cons_t) * (size_t)l.br[i].ncons);
          b.ncons = l.br[i].ncons;
          memcpy(b.cons + b.ncons, r.br[j].cons,
                 sizeof(ny_smt_cons_t) * (size_t)r.br[j].ncons);
          b.ncons += r.br[j].ncons;
          if (!ny_smt_dnf_append(out, &b))
            return false;
        }
      }
    } else {
      /*
       * A∨B : union
       */
      for (int i = 0; i < l.nbr; ++i) {
        if (!ny_smt_dnf_append(out, &l.br[i]))
          return false;
      }
      for (int i = 0; i < r.nbr; ++i) {
        if (!ny_smt_dnf_append(out, &r.br[i]))
          return false;
      }
    }
    return true;
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op && e->as.unary.right &&
      strcmp(e->as.unary.op, "!") == 0) {
    return ny_smt_dnf_build(cg, scopes, depth, env, e->as.unary.right, !negate,
                            out, rec + 1);
  }
  return false;
}

/*
 * Dependency-free Presburger check: P is proven when every branch of its
 * negation is infeasible under the known variable ranges.
 */
static bool ny_proof_try_presburger(codegen_t *cg, scope *scopes, size_t depth,
                                    expr_t *proposition) {
  if (!cg || !proposition)
    return false;
  ny_smt_env_t env;
  memset(&env, 0, sizeof(env));
  ny_smt_dnf_t dnf;
  if (!ny_smt_dnf_build(cg, scopes, depth, &env, proposition, true, &dnf, 0)) {
    ny_proof_debug_at(2, proposition->tok,
                      "presburger: DNF build failed for '%s'",
                      proposition->tok.lexeme ? proposition->tok.lexeme : "?");
    return false;
  }
  if (dnf.nbr == 0) {
    ny_proof_debug_at(1, proposition->tok,
                      "presburger: negation empty -> trivially true");
    return true; /* negation is empty: proposition is trivially true */
  }
  ny_proof_debug_at(2, proposition->tok,
                    "presburger: %d variable(s), %d branch(es)",
                    env.nvars, dnf.nbr);
  for (int b = 0; b < dnf.nbr; ++b) {
    ny_smt_cons_t sys[NY_SMT_MAX_CONS];
    int n = 0;
    if (!ny_smt_add_premises(&env, sys, &n)) {
      ny_proof_debug_at(2, proposition->tok,
                        "presburger: branch %d premise overflow -> unknown", b);
      return false;
    }
    if (n + dnf.br[b].ncons > NY_SMT_MAX_CONS) {
      ny_proof_debug_at(2, proposition->tok,
                        "presburger: branch %d exceeds constraint cap -> unknown", b);
      return false;
    }
    memcpy(sys + n, dnf.br[b].cons,
           sizeof(ny_smt_cons_t) * (size_t)dnf.br[b].ncons);
    n += dnf.br[b].ncons;
    if (!ny_smt_fm_unsat(sys, n, env.nvars)) {
      ny_proof_debug_at(1, proposition->tok,
                        "presburger: branch %d satisfiable -> cannot prove", b);
      return false; /* this branch is satisfiable: cannot prove P */
    }
  }
  ny_proof_debug_at(1, proposition->tok,
                    "presburger: all %d branch(es) unsatisfiable -> proved", dnf.nbr);
  return true;
}

/*
 * ---------- Z3-backed path ----------
 */

#ifdef NYTRIX_HAS_Z3

typedef struct {
  Z3_context ctx;
  Z3_sort isort;
  Z3_ast vars[NY_SMT_MAX_VARS];
  bool ok;
} ny_smt_z3_t;

static Z3_ast ny_smt_z3_term(ny_smt_z3_t *z, ny_smt_env_t *env,
                             codegen_t *cg, scope *scopes, size_t depth,
                             expr_t *e, unsigned rec);

static Z3_ast ny_smt_z3_bool(ny_smt_z3_t *z, ny_smt_env_t *env,
                             codegen_t *cg, scope *scopes, size_t depth,
                             expr_t *e, unsigned rec) {
  if (!e || rec > NY_SMT_MAX_DEPTH)
    return (z->ok = false, (Z3_ast)0);
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_BOOL)
    return e->as.literal.as.b ? Z3_mk_true(z->ctx) : Z3_mk_false(z->ctx);
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      !ny_expr_is_nil_literal(e))
    return e->as.literal.as.i != 0 ? Z3_mk_true(z->ctx)
                                   : Z3_mk_false(z->ctx);
  if (e->kind == NY_E_UNARY && e->as.unary.op && e->as.unary.right &&
      strcmp(e->as.unary.op, "!") == 0) {
    Z3_ast inner =
        ny_smt_z3_bool(z, env, cg, scopes, depth, e->as.unary.right, rec + 1);
    if (!inner)
      return (Z3_ast)0;
    return Z3_mk_not(z->ctx, inner);
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op && e->as.binary.left &&
      e->as.binary.right) {
    const char *op = e->as.binary.op;
    Z3_ast l = ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.left,
                              rec + 1);
    Z3_ast r = ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.right,
                              rec + 1);
    if (!l || !r)
      return (z->ok = false, (Z3_ast)0);
    if (strcmp(op, "<") == 0)
      return Z3_mk_lt(z->ctx, l, r);
    if (strcmp(op, "<=") == 0)
      return Z3_mk_le(z->ctx, l, r);
    if (strcmp(op, ">") == 0)
      return Z3_mk_gt(z->ctx, l, r);
    if (strcmp(op, ">=") == 0)
      return Z3_mk_ge(z->ctx, l, r);
    if (strcmp(op, "==") == 0)
      return Z3_mk_eq(z->ctx, l, r);
    if (strcmp(op, "!=") == 0)
      return Z3_mk_not(z->ctx, Z3_mk_eq(z->ctx, l, r));
    return (z->ok = false, (Z3_ast)0);
  }
  if (e->kind == NY_E_LOGICAL && e->as.logical.op && e->as.logical.left &&
      e->as.logical.right) {
    const char *op = e->as.logical.op;
    Z3_ast l = ny_smt_z3_bool(z, env, cg, scopes, depth, e->as.logical.left,
                              rec + 1);
    Z3_ast r = ny_smt_z3_bool(z, env, cg, scopes, depth, e->as.logical.right,
                              rec + 1);
    if (!l || !r)
      return (z->ok = false, (Z3_ast)0);
    if (strcmp(op, "&&") == 0) {
      Z3_ast ands[2] = {l, r};
      return Z3_mk_and(z->ctx, 2, ands);
    }
    Z3_ast ors[2] = {l, r};
    return Z3_mk_or(z->ctx, 2, ors);
  }
  return (z->ok = false, (Z3_ast)0);
}

static Z3_ast ny_smt_z3_term(ny_smt_z3_t *z, ny_smt_env_t *env,
                             codegen_t *cg, scope *scopes, size_t depth,
                             expr_t *e, unsigned rec) {
  if (!e || rec > NY_SMT_MAX_DEPTH)
    return (z->ok = false, (Z3_ast)0);
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      !ny_expr_is_nil_literal(e))
    return Z3_mk_int64(z->ctx, e->as.literal.as.i, z->isort);
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    int vi = ny_smt_env_add_binding(cg, scopes, depth, env, e->as.ident.name);
    if (vi < 0)
      return (z->ok = false, (Z3_ast)0);
    if (!z->vars[vi]) {
      Z3_symbol sym = Z3_mk_string_symbol(z->ctx, env->vars[vi].name);
      z->vars[vi] = Z3_mk_const(z->ctx, sym, z->isort);
    }
    return z->vars[vi];
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op && e->as.unary.right) {
    if (strcmp(e->as.unary.op, "-") == 0) {
      Z3_ast inner = ny_smt_z3_term(z, env, cg, scopes, depth,
                                    e->as.unary.right, rec + 1);
      if (!inner)
        return (Z3_ast)0;
      return Z3_mk_unary_minus(z->ctx, inner);
    }
    return (z->ok = false, (Z3_ast)0);
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op && e->as.binary.left &&
      e->as.binary.right) {
    const char *op = e->as.binary.op;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
        strcmp(op, "*") == 0 || strcmp(op, "/") == 0) {
      Z3_ast l = ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.left,
                                rec + 1);
      Z3_ast r = ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.right,
                                rec + 1);
      if (!l || !r)
        return (z->ok = false, (Z3_ast)0);
      if (strcmp(op, "+") == 0)
        return Z3_mk_add(z->ctx, 2, (Z3_ast[]){l, r});
      if (strcmp(op, "-") == 0)
        return Z3_mk_sub(z->ctx, 2, (Z3_ast[]){l, r});
      if (strcmp(op, "*") == 0)
        return Z3_mk_mul(z->ctx, 2, (Z3_ast[]){l, r});
      return Z3_mk_div(z->ctx, l, r);
    }
    if (strcmp(op, "%") == 0) {
      Z3_ast l = ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.left,
                                rec + 1);
      Z3_ast r = ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.right,
                                rec + 1);
      if (!l || !r)
        return (z->ok = false, (Z3_ast)0);
      return Z3_mk_mod(z->ctx, l, r);
    }
    return (z->ok = false, (Z3_ast)0);
  }
  return (z->ok = false, (Z3_ast)0);
}

static bool ny_proof_try_z3(codegen_t *cg, scope *scopes, size_t depth,
                            expr_t *proposition, char *cex, size_t cex_cap) {
  if (!cg || !proposition)
    return false;
  Z3_config cfg = Z3_mk_config();
  Z3_set_param_value(cfg, "model", "true");
  Z3_context ctx = Z3_mk_context(cfg);
  ny_smt_z3_t z;
  memset(&z, 0, sizeof(z));
  z.ctx = ctx;
  z.isort = Z3_mk_int_sort(ctx);
  z.ok = true;

  ny_smt_env_t env;
  memset(&env, 0, sizeof(env));

  Z3_ast prop = ny_smt_z3_bool(&z, &env, cg, scopes, depth, proposition, 0);
  if (!z.ok || !prop) {
    Z3_del_context(ctx);
    Z3_del_config(cfg);
    return false;
  }
  Z3_ast neg = Z3_mk_not(ctx, prop);
  if (!z.ok || !neg) {
    Z3_del_context(ctx);
    Z3_del_config(cfg);
    return false;
  }

  Z3_solver s = Z3_mk_solver(ctx);
  Z3_solver_inc_ref(ctx, s);
  /*
   * Known binding ranges become asserted premises.
   */
  for (int v = 0; v < env.nvars; ++v) {
    if (!env.vars[v].has_range || !z.vars[v])
      continue;
    Z3_ast lo =
        Z3_mk_ge(ctx, z.vars[v], Z3_mk_int64(ctx, env.vars[v].lo, z.isort));
    Z3_ast hi =
        Z3_mk_le(ctx, z.vars[v], Z3_mk_int64(ctx, env.vars[v].hi, z.isort));
    Z3_solver_assert(ctx, s, lo);
    Z3_solver_assert(ctx, s, hi);
  }
  Z3_solver_assert(ctx, s, neg);
  Z3_lbool res = Z3_solver_check(ctx, s);
  bool proved = res == Z3_L_FALSE;
  ny_proof_debug_at(proved ? 1 : 2, proposition->tok,
                    "z3: check = %s (%d asserted premise range(s))",
                    res == Z3_L_FALSE ? "unsat (proved)"
                                      : (res == Z3_L_TRUE ? "sat (not proved)"
                                                          : "unknown"),
                    env.nvars);
  if (!proved && cex && cex_cap > 0 && res == Z3_L_TRUE) {
    Z3_model m = Z3_solver_get_model(ctx, s);
    if (m) {
      Z3_model_inc_ref(ctx, m);
      const char *name = env.nvars > 0 && env.vars[0].name ? env.vars[0].name
                                                           : "x";
      int64_t ival = 0;
      Z3_ast eval = 0;
      if (env.nvars > 0 && z.vars[0] && Z3_model_eval(ctx, m, z.vars[0], 1, &eval))
        Z3_get_numeral_int64(ctx, eval, &ival);
      snprintf(cex, cex_cap, "%s=%lld", name, (long long)ival);
      Z3_model_dec_ref(ctx, m);
    }
  }
  Z3_solver_dec_ref(ctx, s);
  Z3_del_context(ctx);
  Z3_del_config(cfg);
  return proved;
}

#else /* !NYTRIX_HAS_Z3 */

static bool ny_proof_try_z3(codegen_t *cg, scope *scopes, size_t depth,
                            expr_t *proposition, char *cex, size_t cex_cap) {
  (void)cg;
  (void)scopes;
  (void)depth;
  (void)proposition;
  (void)cex;
  (void)cex_cap;
  return false;
}

#endif

/*
 * ---------- dispatcher ----------
 */


enum { NY_PROOF_SOLVER_CACHE = 256, NY_PROOF_SOLVER_CEX_CAP = 128 };

typedef struct {
  uint64_t prop_hash;
  uint64_t ctx_hash;
  uint64_t mode_hash;
  bool result;
  bool has_cex;
  char cex[NY_PROOF_SOLVER_CEX_CAP];
} ny_proof_solver_cache_entry_t;

static _Thread_local ny_proof_solver_cache_entry_t
    ny_proof_solver_cache[NY_PROOF_SOLVER_CACHE];
static _Thread_local size_t ny_proof_solver_cache_len;
static _Thread_local size_t ny_proof_solver_cache_next;

static uint64_t ny_proof_hash_mix_u64(uint64_t h, uint64_t v) {
  h ^= v + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
  return h ? h : UINT64_C(0x9e3779b97f4a7c15);
}

static uint64_t ny_proof_hash_mix_bytes(uint64_t h, const char *data,
                                        size_t len) {
  for (size_t i = 0; i < len; ++i) {
    h ^= (uint64_t)(unsigned char)data[i];
    h *= UINT64_C(1099511628211);
  }
  return h ? h : UINT64_C(0x9e3779b97f4a7c15);
}

static uint64_t ny_proof_solver_context_hash(scope *scopes, size_t depth,
                                             const char *canonical) {
  uint64_t h = UINT64_C(1469598103934665603);
  const char *p = canonical;
  while (p && (p = strstr(p, "name:")) != NULL) {
    p += 5;
    const char *start = p;
    while (*p && *p != ',' && *p != ')' && *p != '>')
      ++p;
    size_t len = (size_t)(p - start);
    h = ny_proof_hash_mix_u64(h, len);
    h = ny_proof_hash_mix_bytes(h, start, len);
    char name[256];
    size_t n = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
    memcpy(name, start, n);
    name[n] = '\0';
    binding *b = n > 0 && scopes
                     ? lookup_binding_hash_no_mark(scopes, depth, name, n,
                                                   ny_hash_name(name, n))
                     : NULL;
    if (b && b->has_int_range) {
      h = ny_proof_hash_mix_u64(h, 1);
      h = ny_proof_hash_mix_u64(h, (uint64_t)b->int_min_raw);
      h = ny_proof_hash_mix_u64(h, (uint64_t)b->int_max_raw);
    } else {
      h = ny_proof_hash_mix_u64(h, 0);
    }
  }
  return h;
}

static ny_proof_solver_cache_entry_t *
ny_proof_solver_cache_lookup(uint64_t prop_hash, uint64_t ctx_hash,
                             uint64_t mode_hash) {
  for (size_t i = 0; i < ny_proof_solver_cache_len; ++i) {
    ny_proof_solver_cache_entry_t *e = &ny_proof_solver_cache[i];
    if (e->prop_hash == prop_hash && e->ctx_hash == ctx_hash &&
        e->mode_hash == mode_hash)
      return e;
  }
  return NULL;
}

static void ny_proof_solver_cache_store(uint64_t prop_hash, uint64_t ctx_hash,
                                        uint64_t mode_hash, bool result,
                                        const char *cex) {
  size_t slot = ny_proof_solver_cache_len < NY_PROOF_SOLVER_CACHE
                    ? ny_proof_solver_cache_len++
                    : ny_proof_solver_cache_next++ % NY_PROOF_SOLVER_CACHE;
  ny_proof_solver_cache_entry_t *e = &ny_proof_solver_cache[slot];
  e->prop_hash = prop_hash;
  e->ctx_hash = ctx_hash;
  e->mode_hash = mode_hash;
  e->result = result;
  e->has_cex = cex && *cex;
  if (e->has_cex) {
    snprintf(e->cex, sizeof(e->cex), "%s", cex);
  } else {
    e->cex[0] = '\0';
  }
}

static bool ny_proof_solver_cache_make_key(scope *scopes, size_t depth,
                                           expr_t *proposition,
                                           const char *mode,
                                           uint64_t *prop_hash,
                                           uint64_t *ctx_hash,
                                           uint64_t *mode_hash) {
  char *canonical = ny_proof_type_from_expr(proposition);
  if (!canonical)
    return false;
  *prop_hash = ny_hash64_cstr(canonical);
  *ctx_hash = ny_proof_solver_context_hash(scopes, depth, canonical);
  *mode_hash = ny_hash64_cstr(mode ? mode : "auto");
  free(canonical);
  return true;
}
/*
 * Public entry point: prove `proposition` with the configured solver.
 * Mode is the raw --proof-solver string ("none"|"presburger"|"z3"|"auto").
 * Returns true only on a genuine proof (negation unsatisfiable).
 */
bool ny_proof_try_solver(codegen_t *cg, scope *scopes, size_t depth,
                         expr_t *proposition, char *cex, size_t cex_cap) {
  if (!cg || !proposition)
    return false;
  const char *mode = cg->proof_solver ? cg->proof_solver : "auto";
  if (!mode || strcmp(mode, "none") == 0 || strcmp(mode, "off") == 0)
    return false;
  uint64_t prop_hash = 0, ctx_hash = 0, mode_hash = 0;
  bool have_cache_key = ny_proof_solver_cache_make_key(
      scopes, depth, proposition, mode, &prop_hash, &ctx_hash, &mode_hash);
  if (have_cache_key) {
    ny_proof_solver_cache_entry_t *cached =
        ny_proof_solver_cache_lookup(prop_hash, ctx_hash, mode_hash);
    if (cached) {
      ny_proof_debug_at(2, proposition->tok,
                        "solver: cache hit for mode '%s' -> %s", mode,
                        cached->result ? "proved" : "unknown");
      if (!cached->result && cached->has_cex && cex && cex_cap > 0)
        snprintf(cex, cex_cap, "%s", cached->cex);
      return cached->result;
    }
  }
  ny_proof_debug_at(2, proposition->tok,
                    "solver: mode '%s', trying to prove a %s proposition",
                    mode,
                    proposition->tok.lexeme ? proposition->tok.lexeme : "?");
  bool want_pres = strcmp(mode, "presburger") == 0 ||
                   strcmp(mode, "fm") == 0 ||
                   strcmp(mode, "fourier-motzkin") == 0;
  bool want_z3 = strcmp(mode, "z3") == 0;
  bool is_auto = strcmp(mode, "auto") == 0;
  bool proved = false;
#ifdef NYTRIX_HAS_Z3
  if (want_z3 || is_auto) {
    proved = ny_proof_try_z3(cg, scopes, depth, proposition, cex, cex_cap);
    if (!proved && want_z3) {
      if (have_cache_key)
        ny_proof_solver_cache_store(prop_hash, ctx_hash, mode_hash, false,
                                    cex);
      return false;
    }
  }
#else
  if (want_z3) {
    if (have_cache_key)
      ny_proof_solver_cache_store(prop_hash, ctx_hash, mode_hash, false, cex);
    return false;
  }
#endif
  if (!proved && (want_pres || is_auto)) {
    proved = ny_proof_try_presburger(cg, scopes, depth, proposition);
    if (!proved && want_pres) {
      if (have_cache_key)
        ny_proof_solver_cache_store(prop_hash, ctx_hash, mode_hash, false,
                                    cex);
      return false;
    }
  }
  if (have_cache_key)
    ny_proof_solver_cache_store(prop_hash, ctx_hash, mode_hash, proved, cex);
  return proved;
}


static expr_t ny_smt_test_int(int64_t value) {
  expr_t e;
  memset(&e, 0, sizeof(e));
  e.kind = NY_E_LITERAL;
  e.as.literal.kind = NY_LIT_INT;
  e.as.literal.as.i = value;
  return e;
}

static expr_t ny_smt_test_binary(expr_t *left, const char *op, expr_t *right) {
  expr_t e;
  memset(&e, 0, sizeof(e));
  e.kind = NY_E_BINARY;
  e.as.binary.left = left;
  e.as.binary.op = op;
  e.as.binary.right = right;
  return e;
}

static expr_t ny_smt_test_logical(expr_t *left, const char *op,
                                  expr_t *right) {
  expr_t e;
  memset(&e, 0, sizeof(e));
  e.kind = NY_E_LOGICAL;
  e.as.logical.left = left;
  e.as.logical.op = op;
  e.as.logical.right = right;
  return e;
}

static expr_t ny_smt_test_not(expr_t *right) {
  expr_t e;
  memset(&e, 0, sizeof(e));
  e.kind = NY_E_UNARY;
  e.as.unary.op = "!";
  e.as.unary.right = right;
  return e;
}

static bool ny_smt_test_cmp_truth(int64_t left, const char *op,
                                  int64_t right) {
  if (strcmp(op, "<") == 0)
    return left < right;
  if (strcmp(op, "<=") == 0)
    return left <= right;
  if (strcmp(op, ">") == 0)
    return left > right;
  if (strcmp(op, ">=") == 0)
    return left >= right;
  if (strcmp(op, "==") == 0)
    return left == right;
  return left != right;
}

static int ny_smt_test_decide(expr_t *proposition) {
  codegen_t cg;
  memset(&cg, 0, sizeof(cg));
  if (ny_proof_try_presburger(&cg, NULL, 0, proposition))
    return 1;
  expr_t negated = ny_smt_test_not(proposition);
  if (ny_proof_try_presburger(&cg, NULL, 0, &negated))
    return -1;
  return 0;
}

static bool ny_smt_test_expect(expr_t *proposition, bool truth,
                               const char *shape, const char *left_op,
                               const char *right_op, int64_t a, int64_t b,
                               int64_t c, size_t *checks) {
  int decision = ny_smt_test_decide(proposition);
  (*checks)++;
  if (decision == (truth ? 1 : -1))
    return true;
  fprintf(stderr,
          "proof solver differential selftest: %s mismatch for "
          "a=%lld b=%lld c=%lld ops=%s,%s: expected %s, got %s\n",
          shape, (long long)a, (long long)b, (long long)c, left_op,
          right_op ? right_op : "-", truth ? "proved" : "disproved",
          decision > 0 ? "proved" : decision < 0 ? "disproved" : "unknown");
  return false;
}

int ny_proof_solver_differential_selftest(void) {
  static const char *ops[] = {"<", "<=", ">", ">=", "==", "!="};
  size_t checks = 0;
  for (int64_t a = -4; a <= 4; ++a) {
    for (int64_t b = -4; b <= 4; ++b) {
      for (int64_t c = -4; c <= 4; ++c) {
        expr_t av = ny_smt_test_int(a), bv = ny_smt_test_int(b);
        expr_t cv = ny_smt_test_int(c);
        for (size_t oi = 0; oi < sizeof(ops) / sizeof(ops[0]); ++oi) {
          expr_t p = ny_smt_test_binary(&av, ops[oi], &bv);
          bool pt = ny_smt_test_cmp_truth(a, ops[oi], b);
          expr_t not_p = ny_smt_test_not(&p);
          if (!ny_smt_test_expect(&p, pt, "comparison", ops[oi], NULL, a,
                                  b, c, &checks) ||
              !ny_smt_test_expect(&not_p, !pt, "negation", ops[oi], NULL,
                                  a, b, c, &checks))
            return 1;
          for (size_t oj = 0; oj < sizeof(ops) / sizeof(ops[0]); ++oj) {
            expr_t q = ny_smt_test_binary(&bv, ops[oj], &cv);
            bool qt = ny_smt_test_cmp_truth(b, ops[oj], c);
            expr_t both = ny_smt_test_logical(&p, "&&", &q);
            expr_t either = ny_smt_test_logical(&p, "||", &q);
            expr_t not_both = ny_smt_test_not(&both);
            expr_t not_either = ny_smt_test_not(&either);
            if (!ny_smt_test_expect(&both, pt && qt, "and", ops[oi], ops[oj],
                                    a, b, c, &checks) ||
                !ny_smt_test_expect(&either, pt || qt, "or", ops[oi], ops[oj],
                                    a, b, c, &checks) ||
                !ny_smt_test_expect(&not_both, !(pt && qt), "not-and",
                                    ops[oi], ops[oj], a, b, c, &checks) ||
                !ny_smt_test_expect(&not_either, !(pt || qt), "not-or",
                                    ops[oi], ops[oj], a, b, c, &checks))
              return 1;
          }
        }
      }
    }
  }
  printf("proof solver differential selftest: %zu exhaustive checks over "
         "[-4,4] passed\n",
         checks);
  return 0;
}
