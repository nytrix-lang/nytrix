/*
 * Solver-backed proofs.
 *
 * Two backends, both used only as *acceptance* evidence:
 *
 *   1. ny_proof_try_presburger — dependency-free Presburger subset using
 *      Fourier–Motzkin elimination over linear integer arithmetic
 *      (`+`, `-`, `*const`, comparisons, `&&`/`||`/`!`).  Bounded to
 *      16 variables / 64 constraints; bails to "unknown" on anything else.
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
#include "code/lang/parse/proof.h"
#include "code/lang/parse/frontend/visitor.h"
#include "code/lang/priv.h"

#ifdef NYTRIX_HAS_Z3
#include <z3.h>
#endif

#include <stdarg.h>

#define NY_SMT_MAX_VARS 16
#define NY_SMT_MAX_CONS 128
#define NY_SMT_MAX_BRANCHES 16
#define NY_SMT_MAX_DEPTH 64

/*
 * Proof-system tracing.
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
  if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "off") == 0 ||
      strcmp(v, "no") == 0)
    return 0;
  char *end = NULL;
  long n = strtol(v, &end, 10);
  if (end && *end == '\0' && n >= 1 && n <= 9)
    return (int)n;
  return 1;
}
bool ny_proof_debug_enabled(void) { return ny_proof_debug_level() > 0; }

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
  fprintf(stderr,
          "[proof debug] %s:%d:%d: ", tok.filename ? tok.filename : "unknown",
          tok.line, tok.col);
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
  __int128 rhs;
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

static bool ny_smt_const_i64_expr(const expr_t *expr, int64_t *out) {
  if (!expr || !out)
    return false;
  if (expr->kind == NY_E_LITERAL && expr->as.literal.kind == NY_LIT_INT) {
    if (expr->tok.kind == NY_T_NIL ||
        !ny_literal_is_signed_i64(&expr->as.literal))
      return false;
    *out = expr->as.literal.as.i;
    return true;
  }
  if (expr->kind == NY_E_UNARY && expr->as.unary.op &&
      strcmp(expr->as.unary.op, "-") == 0) {
    int64_t operand;
    if (!ny_smt_const_i64_expr(expr->as.unary.right, &operand) ||
        operand == INT64_MIN)
      return false;
    *out = -operand;
    return true;
  }
  if (expr->kind != NY_E_BINARY || !expr->as.binary.op)
    return false;
  int64_t left, right;
  if (!ny_smt_const_i64_expr(expr->as.binary.left, &left) ||
      !ny_smt_const_i64_expr(expr->as.binary.right, &right))
    return false;
  __int128 result;
  if (strcmp(expr->as.binary.op, "+") == 0)
    result = (__int128)left + right;
  else if (strcmp(expr->as.binary.op, "-") == 0)
    result = (__int128)left - right;
  else if (strcmp(expr->as.binary.op, "*") == 0)
    result = (__int128)left * right;
  else if (strcmp(expr->as.binary.op, "/") == 0 ||
           strcmp(expr->as.binary.op, "%") == 0) {
    if (right == 0 || (left == INT64_MIN && right == -1))
      return false;
    *out = strcmp(expr->as.binary.op, "/") == 0 ? left / right : left % right;
    return true;
  } else {
    return false;
  }
  if (result < INT64_MIN || result > INT64_MAX)
    return false;
  *out = (int64_t)result;
  return true;
}

static bool ny_smt_contract_safe(codegen_t *cg, scope *scopes, size_t depth,
                                 expr_t *expr, expr_t *premise, unsigned rec) {
  if (!expr || rec > NY_SMT_MAX_DEPTH)
    return false;
  if (expr->kind == NY_E_IDENT)
    return true;
  if (expr->kind == NY_E_LITERAL)
    return expr->as.literal.kind == NY_LIT_BOOL ||
           (expr->tok.kind != NY_T_NIL &&
            ny_literal_is_signed_i64(&expr->as.literal));
  if (expr->kind == NY_E_LOGICAL && expr->as.logical.op &&
      expr->as.logical.left && expr->as.logical.right) {
    const char *op = expr->as.logical.op;
    if (strcmp(op, "&&") != 0 && strcmp(op, "||") != 0)
      return false;
    expr_t *left = expr->as.logical.left;
    if (!ny_smt_contract_safe(cg, scopes, depth, left, premise, rec + 1))
      return false;
    expr_t negated = {.kind = NY_E_UNARY, .tok = expr->tok};
    negated.as.unary.op = "!";
    negated.as.unary.right = left;
    expr_t *guard = strcmp(op, "&&") == 0 ? left : &negated;
    expr_t conjunction = {.kind = NY_E_LOGICAL, .tok = expr->tok};
    conjunction.as.logical.op = "&&";
    conjunction.as.logical.left = premise;
    conjunction.as.logical.right = guard;
    return ny_smt_contract_safe(cg, scopes, depth, expr->as.logical.right,
                                premise ? &conjunction : guard, rec + 1);
  }
  const char *op = NULL;
  if (expr->kind == NY_E_UNARY) {
    op = expr->as.unary.op;
    if (!op || !ny_smt_contract_safe(cg, scopes, depth, expr->as.unary.right,
                                     premise, rec + 1))
      return false;
  } else if (expr->kind == NY_E_BINARY) {
    op = expr->as.binary.op;
    if (!op ||
        !ny_smt_contract_safe(cg, scopes, depth, expr->as.binary.left, premise,
                              rec + 1) ||
        !ny_smt_contract_safe(cg, scopes, depth, expr->as.binary.right, premise,
                              rec + 1))
      return false;
  } else {
    return false;
  }
  bool arithmetic = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                    strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                    strcmp(op, "%") == 0;
  if (!arithmetic)
    return true;
  for (size_t i = 0; i < cg->operators.len; ++i) {
    const ny_operator_def_t *definition = &cg->operators.data[i];
    if (definition->op && strcmp(definition->op, op) == 0 &&
        definition->left_type && definition->right_type &&
        strcmp(definition->left_type, "int") == 0 &&
        strcmp(definition->right_type, "int") == 0)
      return false;
  }
  int64_t ignored;
  if (ny_smt_const_i64_expr(expr, &ignored))
    return true;
  if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
    /*
     * Both operations trap for a zero divisor and MIN/-1. Prove those
     * preconditions in the current path context instead of rejecting every
     * symbolic divisor. The independent certificate checker models C's
     * truncating division and signed remainder, so a Z3 convention mismatch
     * cannot certify an invalid range.
     */
    int64_t constant_divisor;
    if (expr->as.binary.right &&
        ny_smt_const_i64_expr(expr->as.binary.right, &constant_divisor)) {
      if (constant_divisor == 0)
        return false;
      if (constant_divisor != -1)
        return true;
    }
    expr_t zero = {.kind = NY_E_LITERAL, .tok = expr->tok};
    zero.as.literal.kind = NY_LIT_INT;
    zero.as.literal.as.i = 0;
    expr_t nonzero = {.kind = NY_E_BINARY, .tok = expr->tok};
    nonzero.as.binary.op = "!=";
    nonzero.as.binary.left = expr->as.binary.right;
    nonzero.as.binary.right = &zero;

    expr_t minimum = zero;
    minimum.as.literal.as.i = INT64_MIN;
    expr_t minus_one = zero;
    minus_one.as.literal.as.i = -1;
    expr_t not_minimum = {.kind = NY_E_BINARY, .tok = expr->tok};
    not_minimum.as.binary.op = "!=";
    not_minimum.as.binary.left = expr->as.binary.left;
    not_minimum.as.binary.right = &minimum;
    expr_t not_minus_one = {.kind = NY_E_BINARY, .tok = expr->tok};
    not_minus_one.as.binary.op = "!=";
    not_minus_one.as.binary.left = expr->as.binary.right;
    not_minus_one.as.binary.right = &minus_one;
    expr_t no_overflow = {.kind = NY_E_LOGICAL, .tok = expr->tok};
    no_overflow.as.logical.op = "||";
    no_overflow.as.logical.left = &not_minimum;
    no_overflow.as.logical.right = &not_minus_one;
    expr_t safe = {.kind = NY_E_LOGICAL, .tok = expr->tok};
    safe.as.logical.op = "&&";
    safe.as.logical.left = &nonzero;
    safe.as.logical.right = &no_overflow;
    expr_t negated_premise = {.kind = NY_E_UNARY, .tok = expr->tok};
    negated_premise.as.unary.op = "!";
    negated_premise.as.unary.right = premise;
    expr_t implication = {.kind = NY_E_LOGICAL, .tok = expr->tok};
    implication.as.logical.op = "||";
    implication.as.logical.left = &negated_premise;
    implication.as.logical.right = &safe;
    ny_proof_policy_result_t result = {0};
    bool safe_in_context =
        ny_proof_policy_decide(cg, scopes, depth,
                               premise ? &implication : &safe, "kernels", false,
                               &result) &&
        result.verdict == NY_PROOF_VERDICT_PROVED;
    return safe_in_context;
  }
  expr_t minimum = {.kind = NY_E_LITERAL, .tok = expr->tok};
  minimum.as.literal.kind = NY_LIT_INT;
  minimum.as.literal.as.i = INT64_MIN;
  expr_t maximum = minimum;
  maximum.as.literal.as.i = INT64_MAX;
  expr_t lower = {.kind = NY_E_BINARY, .tok = expr->tok};
  lower.as.binary.op = ">=";
  lower.as.binary.left = expr;
  lower.as.binary.right = &minimum;
  expr_t upper = lower;
  upper.as.binary.op = "<=";
  upper.as.binary.right = &maximum;
  expr_t bounds = {.kind = NY_E_LOGICAL, .tok = expr->tok};
  bounds.as.logical.op = "&&";
  bounds.as.logical.left = &lower;
  bounds.as.logical.right = &upper;
  expr_t negated = {.kind = NY_E_UNARY, .tok = expr->tok};
  negated.as.unary.op = "!";
  negated.as.unary.right = premise;
  expr_t implication = {.kind = NY_E_LOGICAL, .tok = expr->tok};
  implication.as.logical.op = "||";
  implication.as.logical.left = &negated;
  implication.as.logical.right = &bounds;
  ny_proof_policy_result_t result = {0};
  bool proved = ny_proof_policy_decide(cg, scopes, depth,
                                       premise ? &implication : &bounds,
                                       "kernels", false, &result) &&
                result.verdict == NY_PROOF_VERDICT_PROVED;
  if (!proved)
    ny_proof_debug_at(2, expr->tok, "contract overflow guard: %s",
                      result.explanation);
  return proved;
}

/*
 * Call-site contracts execute with wrapping signed-int arithmetic, while the
 * general proof solver models mathematical integers. Restrict this boundary
 * to arithmetic whose intermediates provably fit. Short-circuit guards
 * supply premises, but are checked themselves before becoming evidence.
 */
bool ny_proof_try_contract_solver(codegen_t *cg, scope *scopes, size_t depth,
                                  expr_t *proposition, char *cex,
                                  size_t cex_cap) {
  if (!cg || !proposition)
    return false;
  if (!ny_smt_contract_safe(cg, scopes, depth, proposition, NULL, 0)) {
    ny_proof_debug_at(
        2, proposition->tok,
        "contract solver: custom or potentially overflowing arithmetic "
        "cannot be proved; proof left unknown");
    return false;
  }
  return ny_proof_try_solver(cg, scopes, depth, proposition, cex, cex_cap);
}

/*
 * Linear expression builder (Presburger).
 */

typedef struct {
  int64_t coeff[NY_SMT_MAX_VARS];
  __int128 cconst;
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
    for (int i = 0; i < env->nvars; ++i) {
      if (__builtin_sub_overflow((int64_t)0, inner.coeff[i], &out->coeff[i]))
        return (out->ok = false);
    }
    if (__builtin_sub_overflow((__int128)0, inner.cconst, &out->cconst))
      return (out->ok = false);
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
      bool overflow =
          sgn > 0 ? __builtin_add_overflow(l.cconst, r.cconst, &out->cconst)
                  : __builtin_sub_overflow(l.cconst, r.cconst, &out->cconst);
      if (overflow)
        return (out->ok = false);
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
      bool lok = ny_smt_lin_build(cg, scopes, depth, env, e->as.binary.left, &l,
                                  rec + 1);
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
        if (__builtin_mul_overflow(l.cconst, r.cconst, &out->cconst))
          return (out->ok = false);
        return true;
      }
      if (r_const) {
        __int128 k = r.cconst;
        for (int i = 0; i < env->nvars; ++i) {
          __int128 p;
          if (__builtin_mul_overflow((__int128)l.coeff[i], k, &p) ||
              p < INT64_MIN || p > INT64_MAX)
            return (out->ok = false);
          out->coeff[i] = (int64_t)p;
        }
        if (__builtin_mul_overflow(l.cconst, k, &out->cconst))
          return (out->ok = false);
        return true;
      }
      if (l_const) {
        __int128 k = l.cconst;
        for (int i = 0; i < env->nvars; ++i) {
          __int128 p;
          if (__builtin_mul_overflow((__int128)r.coeff[i], k, &p) ||
              p < INT64_MIN || p > INT64_MAX)
            return (out->ok = false);
          out->coeff[i] = (int64_t)p;
        }
        if (__builtin_mul_overflow(r.cconst, k, &out->cconst))
          return (out->ok = false);
        return true;
      }
      return (out->ok = false); /* nonlinear: not Presburger */
    }
    return (out->ok = false);
  }
  return (out->ok = false);
}

/*
 * Fourier-Motzkin elimination.
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
    ny_smt_cons_t up[NY_SMT_MAX_CONS], lo[NY_SMT_MAX_CONS], zr[NY_SMT_MAX_CONS];
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
                __builtin_sub_overflow(p1, p2, &nc.coeff[i]))
              ok = false;
          }
          __int128 t1 = 0, t2 = 0;
          if (ok && (__builtin_mul_overflow((__int128)a_u, lo[l].rhs, &t1) ||
                     __builtin_mul_overflow((__int128)a_l, up[u].rhs, &t2) ||
                     __builtin_sub_overflow(t1, t2, &nc.rhs)))
            ok = false;
          if (ok) {
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
    c.rhs = -(__int128)env->vars[v].lo; /* -x ≤ -lo  ⟺  x ≥ lo */
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
 * Proposition to DNF of its negation.
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
  /*
   * Branch arrays grow only as needed; nested boolean formulas must not put
   * the full bounded DNF matrix on each C stack frame.
   */
  ny_smt_branch_t *br;
  int nbr;
  int cap;
} ny_smt_dnf_t;

static bool ny_smt_dnf_append(ny_smt_dnf_t *d, const ny_smt_branch_t *b) {
  if (!d || !b || d->nbr >= NY_SMT_MAX_BRANCHES)
    return false;
  if (d->nbr == d->cap) {
    int cap = d->cap ? d->cap * 2 : 2;
    if (cap > NY_SMT_MAX_BRANCHES)
      cap = NY_SMT_MAX_BRANCHES;
    ny_smt_branch_t *branches = realloc(d->br, sizeof(*branches) * (size_t)cap);
    if (!branches)
      return false;
    d->br = branches;
    d->cap = cap;
  }
  d->br[d->nbr++] = *b;
  return true;
}

static void ny_smt_dnf_dispose(ny_smt_dnf_t *d) {
  if (!d)
    return;
  free(d->br);
  *d = (ny_smt_dnf_t){0};
}

static void ny_smt_dnf_reset(ny_smt_dnf_t *d) { ny_smt_dnf_dispose(d); }

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
  if (__builtin_sub_overflow(ll.cconst, rr.cconst, &diff.cconst))
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
    __int128 rhs = 0;
    switch (rel) {
    case 0: /* LT: diff <= -1 */
      if (__builtin_sub_overflow(-(__int128)1, diff.cconst, &rhs))
        return 0;
      break;
    case 1: /* LE: diff <= 0 */
      if (__builtin_sub_overflow((__int128)0, diff.cconst, &rhs))
        return 0;
      break;
    case 2: /* GT: -diff <= -1 */
      if (__builtin_sub_overflow(diff.cconst, (__int128)1, &rhs))
        return 0;
      break;
    default: /* GE: -diff <= 0 */
      rhs = diff.cconst;
      break;
    }
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
      __int128 rhs_forward = 0;
      if (__builtin_sub_overflow((__int128)0, diff.cconst, &rhs_forward))
        return 0;
      NY_SMT_PUSH_CONS(&b, diff.coeff, rhs_forward);
      for (int i = 0; i < env->nvars; ++i) {
        if (!ny_smt_i128_to_i64(-(__int128)diff.coeff[i], &b.cons[1].coeff[i]))
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
    __int128 rhs1 = 0, rhs2 = 0;
    if (__builtin_sub_overflow(-(__int128)1, diff.cconst, &rhs1) ||
        __builtin_sub_overflow(diff.cconst, (__int128)1, &rhs2))
      return 0;
    NY_SMT_PUSH_CONS(&b1, diff.coeff, rhs1);
    for (int i = 0; i < env->nvars; ++i) {
      if (!ny_smt_i128_to_i64(-(__int128)diff.coeff[i], &b2.cons[0].coeff[i]))
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

/*
 * A lemma call contributes truth only after its definition was itself proved;
 * unresolved calls stay outside the solver's trusted proposition fragment.
 */
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
    if (strcmp(op, "<") != 0 && strcmp(op, "<=") != 0 && strcmp(op, ">") != 0 &&
        strcmp(op, ">=") != 0 && strcmp(op, "==") != 0 && strcmp(op, "!=") != 0)
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
    ny_smt_dnf_t l = {0}, r = {0};
    bool l_ok = ny_smt_dnf_build(cg, scopes, depth, env, e->as.logical.left,
                                 negate, &l, rec + 1);
    bool r_ok = ny_smt_dnf_build(cg, scopes, depth, env, e->as.logical.right,
                                 negate, &r, rec + 1);
    if (!l_ok || !r_ok) {
      ny_smt_dnf_dispose(&l);
      ny_smt_dnf_dispose(&r);
      return false;
    }
    if (strcmp(op, "&&") == 0) {
      if (negate) {
        /*
         * ¬(A∧B) = ¬A ∨ ¬B : union of branch lists
         */
        for (int i = 0; i < l.nbr; ++i) {
          if (!ny_smt_dnf_append(out, &l.br[i])) {
            ny_smt_dnf_dispose(&l);
            ny_smt_dnf_dispose(&r);
            return false;
          }
        }
        for (int i = 0; i < r.nbr; ++i) {
          if (!ny_smt_dnf_append(out, &r.br[i])) {
            ny_smt_dnf_dispose(&l);
            ny_smt_dnf_dispose(&r);
            return false;
          }
        }
      } else {
        /*
         * A∧B : cross product (both must hold)
         */
        for (int i = 0; i < l.nbr; ++i) {
          for (int j = 0; j < r.nbr; ++j) {
            ny_smt_branch_t b;
            memset(&b, 0, sizeof(b));
            if (l.br[i].ncons + r.br[j].ncons > NY_SMT_MAX_CONS) {
              ny_smt_dnf_dispose(&l);
              ny_smt_dnf_dispose(&r);
              return false;
            }
            memcpy(b.cons, l.br[i].cons,
                   sizeof(ny_smt_cons_t) * (size_t)l.br[i].ncons);
            b.ncons = l.br[i].ncons;
            memcpy(b.cons + b.ncons, r.br[j].cons,
                   sizeof(ny_smt_cons_t) * (size_t)r.br[j].ncons);
            b.ncons += r.br[j].ncons;
            if (!ny_smt_dnf_append(out, &b)) {
              ny_smt_dnf_dispose(&l);
              ny_smt_dnf_dispose(&r);
              return false;
            }
          }
        }
      }
      ny_smt_dnf_dispose(&l);
      ny_smt_dnf_dispose(&r);
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
          if (l.br[i].ncons + r.br[j].ncons > NY_SMT_MAX_CONS) {
            ny_smt_dnf_dispose(&l);
            ny_smt_dnf_dispose(&r);
            return false;
          }
          memcpy(b.cons, l.br[i].cons,
                 sizeof(ny_smt_cons_t) * (size_t)l.br[i].ncons);
          b.ncons = l.br[i].ncons;
          memcpy(b.cons + b.ncons, r.br[j].cons,
                 sizeof(ny_smt_cons_t) * (size_t)r.br[j].ncons);
          b.ncons += r.br[j].ncons;
          if (!ny_smt_dnf_append(out, &b)) {
            ny_smt_dnf_dispose(&l);
            ny_smt_dnf_dispose(&r);
            return false;
          }
        }
      }
    } else {
      /*
       * A∨B : union
       */
      for (int i = 0; i < l.nbr; ++i) {
        if (!ny_smt_dnf_append(out, &l.br[i])) {
          ny_smt_dnf_dispose(&l);
          ny_smt_dnf_dispose(&r);
          return false;
        }
      }
      for (int i = 0; i < r.nbr; ++i) {
        if (!ny_smt_dnf_append(out, &r.br[i])) {
          ny_smt_dnf_dispose(&l);
          ny_smt_dnf_dispose(&r);
          return false;
        }
      }
    }
    ny_smt_dnf_dispose(&l);
    ny_smt_dnf_dispose(&r);
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
 *
 * Core decision over a caller-owned environment.  `env` may arrive
 * pre-populated with known ranges (the canonical-obligation front end);
 * names not yet present are added without ranges.
 */
static bool ny_proof_presburger_env(codegen_t *cg, scope *scopes, size_t depth,
                                    ny_smt_env_t *env, expr_t *proposition) {
  if (!proposition)
    return false;
  ny_smt_dnf_t dnf = {0};
  if (!ny_smt_dnf_build(cg, scopes, depth, env, proposition, true, &dnf, 0)) {
    ny_proof_debug_at(2, proposition->tok,
                      "presburger: DNF build failed for '%s'",
                      proposition->tok.lexeme ? proposition->tok.lexeme : "?");
    ny_smt_dnf_dispose(&dnf);
    return false;
  }
  if (dnf.nbr == 0) {
    ny_proof_debug_at(1, proposition->tok,
                      "presburger: negation empty -> trivially true");
    ny_smt_dnf_dispose(&dnf);
    return true; /* negation is empty: proposition is trivially true */
  }
  ny_proof_debug_at(2, proposition->tok,
                    "presburger: %d variable(s), %d branch(es)", env->nvars,
                    dnf.nbr);
  for (int b = 0; b < dnf.nbr; ++b) {
    ny_smt_cons_t sys[NY_SMT_MAX_CONS];
    int n = 0;
    if (!ny_smt_add_premises(env, sys, &n)) {
      ny_proof_debug_at(2, proposition->tok,
                        "presburger: branch %d premise overflow -> unknown", b);
      ny_smt_dnf_dispose(&dnf);
      return false;
    }
    if (n + dnf.br[b].ncons > NY_SMT_MAX_CONS) {
      ny_proof_debug_at(
          2, proposition->tok,
          "presburger: branch %d exceeds constraint cap (%d premises + %d "
          "constraints > %d) -> unknown",
          b, n, dnf.br[b].ncons, NY_SMT_MAX_CONS);
      ny_smt_dnf_dispose(&dnf);
      return false;
    }
    memcpy(sys + n, dnf.br[b].cons,
           sizeof(ny_smt_cons_t) * (size_t)dnf.br[b].ncons);
    n += dnf.br[b].ncons;
    if (!ny_smt_fm_unsat(sys, n, env->nvars)) {
      ny_proof_debug_at(1, proposition->tok,
                        "presburger: branch %d satisfiable -> cannot prove", b);
      ny_smt_dnf_dispose(&dnf);
      return false; /* this branch is satisfiable: cannot prove P */
    }
  }
  ny_proof_debug_at(1, proposition->tok,
                    "presburger: all %d branch(es) unsatisfiable -> proved",
                    dnf.nbr);
  ny_smt_dnf_dispose(&dnf);
  return true;
}

static bool ny_proof_try_presburger(codegen_t *cg, scope *scopes, size_t depth,
                                    expr_t *proposition) {
  if (!proposition)
    return false;
  ny_smt_env_t env;
  memset(&env, 0, sizeof(env));
  return ny_proof_presburger_env(cg, scopes, depth, &env, proposition);
}

/*
 * Z3-backed path.
 */

#ifdef NYTRIX_HAS_Z3

typedef struct {
  Z3_context ctx;
  Z3_sort isort;
  Z3_ast vars[NY_SMT_MAX_VARS];
  bool ok;
} ny_smt_z3_t;

static Z3_ast ny_smt_z3_term(ny_smt_z3_t *z, ny_smt_env_t *env, codegen_t *cg,
                             scope *scopes, size_t depth, expr_t *e,
                             unsigned rec);

static Z3_ast ny_smt_z3_bool(ny_smt_z3_t *z, ny_smt_env_t *env, codegen_t *cg,
                             scope *scopes, size_t depth, expr_t *e,
                             unsigned rec) {
  if (!e || rec > NY_SMT_MAX_DEPTH)
    return (z->ok = false, (Z3_ast)0);
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_BOOL)
    return e->as.literal.as.b ? Z3_mk_true(z->ctx) : Z3_mk_false(z->ctx);
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      !ny_expr_is_nil_literal(e))
    return e->as.literal.as.i != 0 ? Z3_mk_true(z->ctx) : Z3_mk_false(z->ctx);
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
    Z3_ast l =
        ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.left, rec + 1);
    Z3_ast r =
        ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.right, rec + 1);
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
    Z3_ast l =
        ny_smt_z3_bool(z, env, cg, scopes, depth, e->as.logical.left, rec + 1);
    Z3_ast r =
        ny_smt_z3_bool(z, env, cg, scopes, depth, e->as.logical.right, rec + 1);
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

static Z3_ast ny_smt_z3_term(ny_smt_z3_t *z, ny_smt_env_t *env, codegen_t *cg,
                             scope *scopes, size_t depth, expr_t *e,
                             unsigned rec) {
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
      Z3_ast inner =
          ny_smt_z3_term(z, env, cg, scopes, depth, e->as.unary.right, rec + 1);
      if (!inner)
        return (Z3_ast)0;
      return Z3_mk_unary_minus(z->ctx, inner);
    }
    return (z->ok = false, (Z3_ast)0);
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op && e->as.binary.left &&
      e->as.binary.right) {
    const char *op = e->as.binary.op;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "/") == 0) {
      Z3_ast l =
          ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.left, rec + 1);
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
      Z3_ast l =
          ny_smt_z3_term(z, env, cg, scopes, depth, e->as.binary.left, rec + 1);
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

static bool ny_proof_z3_env(codegen_t *cg, scope *scopes, size_t depth,
                            ny_smt_env_t *env, expr_t *proposition, char *cex,
                            size_t cex_cap, bool *counterexample_found) {
  if (counterexample_found)
    *counterexample_found = false;
  if (!proposition)
    return false;
  Z3_config cfg = Z3_mk_config();
  Z3_set_param_value(cfg, "model", "true");
  Z3_context ctx = Z3_mk_context(cfg);
  ny_smt_z3_t z;
  memset(&z, 0, sizeof(z));
  z.ctx = ctx;
  z.isort = Z3_mk_int_sort(ctx);
  z.ok = true;

  Z3_ast prop = ny_smt_z3_bool(&z, env, cg, scopes, depth, proposition, 0);
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
  for (int v = 0; v < env->nvars; ++v) {
    if (!env->vars[v].has_range || !z.vars[v])
      continue;
    Z3_ast lo =
        Z3_mk_ge(ctx, z.vars[v], Z3_mk_int64(ctx, env->vars[v].lo, z.isort));
    Z3_ast hi =
        Z3_mk_le(ctx, z.vars[v], Z3_mk_int64(ctx, env->vars[v].hi, z.isort));
    Z3_solver_assert(ctx, s, lo);
    Z3_solver_assert(ctx, s, hi);
  }
  Z3_solver_assert(ctx, s, neg);
  Z3_lbool res = Z3_solver_check(ctx, s);
  if (counterexample_found)
    *counterexample_found = res == Z3_L_TRUE;
  bool proved = res == Z3_L_FALSE;
  ny_proof_debug_at(proved ? 1 : 2, proposition->tok,
                    "z3: check = %s (%d asserted premise range(s))",
                    res == Z3_L_FALSE
                        ? "unsat (proved)"
                        : (res == Z3_L_TRUE ? "sat (not proved)" : "unknown"),
                    env->nvars);
  if (!proved && cex && cex_cap > 0 && res == Z3_L_TRUE) {
    Z3_model m = Z3_solver_get_model(ctx, s);
    if (m) {
      Z3_model_inc_ref(ctx, m);
      const char *name =
          env->nvars > 0 && env->vars[0].name ? env->vars[0].name : "x";
      int64_t ival = 0;
      Z3_ast eval = 0;
      if (env->nvars > 0 && z.vars[0] &&
          Z3_model_eval(ctx, m, z.vars[0], 1, &eval))
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

static bool ny_proof_try_z3(codegen_t *cg, scope *scopes, size_t depth,
                            expr_t *proposition, char *cex, size_t cex_cap,
                            bool *counterexample_found) {
  ny_smt_env_t env;
  memset(&env, 0, sizeof(env));
  return ny_proof_z3_env(cg, scopes, depth, &env, proposition, cex, cex_cap,
                         counterexample_found);
}

#else /* !NYTRIX_HAS_Z3 */

static bool ny_proof_try_z3(codegen_t *cg, scope *scopes, size_t depth,
                            expr_t *proposition, char *cex, size_t cex_cap,
                            bool *counterexample_found) {
  (void)cg;
  (void)scopes;
  (void)depth;
  (void)proposition;
  (void)cex;
  (void)cex_cap;
  if (counterexample_found)
    *counterexample_found = false;
  return false;
}

#endif

/*
 * Dispatcher.
 */

enum { NY_PROOF_SOLVER_CACHE = 256, NY_PROOF_SOLVER_CEX_CAP = 128 };

typedef struct {
  uint64_t prop_hash;
  uint64_t ctx_hash;
  uint64_t mode_hash;
  bool result;
  bool has_cex;
  char cex[NY_PROOF_SOLVER_CEX_CAP];
} ny_proof_try_solverr_cache_entry_t;

static _Thread_local ny_proof_try_solverr_cache_entry_t
    ny_proof_try_solverr_cache[NY_PROOF_SOLVER_CACHE];
static _Thread_local size_t ny_proof_try_solverr_cache_len;
static _Thread_local size_t ny_proof_try_solverr_cache_next;

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

static uint64_t ny_proof_try_solverr_context_hash(scope *scopes, size_t depth,
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

static ny_proof_try_solverr_cache_entry_t *
ny_proof_try_solverr_cache_lookup(uint64_t prop_hash, uint64_t ctx_hash,
                                  uint64_t mode_hash) {
  for (size_t i = 0; i < ny_proof_try_solverr_cache_len; ++i) {
    ny_proof_try_solverr_cache_entry_t *e = &ny_proof_try_solverr_cache[i];
    if (e->prop_hash == prop_hash && e->ctx_hash == ctx_hash &&
        e->mode_hash == mode_hash)
      return e;
  }
  return NULL;
}

static void ny_proof_try_solverr_cache_store(uint64_t prop_hash,
                                             uint64_t ctx_hash,
                                             uint64_t mode_hash, bool result,
                                             const char *cex) {
  size_t slot = ny_proof_try_solverr_cache_len < NY_PROOF_SOLVER_CACHE
                    ? ny_proof_try_solverr_cache_len++
                    : ny_proof_try_solverr_cache_next++ % NY_PROOF_SOLVER_CACHE;
  ny_proof_try_solverr_cache_entry_t *e = &ny_proof_try_solverr_cache[slot];
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

static bool ny_proof_try_solverr_cache_make_key(
    scope *scopes, size_t depth, expr_t *proposition, const char *mode,
    uint64_t *prop_hash, uint64_t *ctx_hash, uint64_t *mode_hash) {
  char *canonical = ny_proof_type_from_expr(proposition);
  if (!canonical)
    return false;
  *prop_hash = ny_hash64_cstr(canonical);
  *ctx_hash = ny_proof_try_solverr_context_hash(scopes, depth, canonical);
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
  bool have_cache_key = ny_proof_try_solverr_cache_make_key(
      scopes, depth, proposition, mode, &prop_hash, &ctx_hash, &mode_hash);
  if (have_cache_key) {
    ny_proof_try_solverr_cache_entry_t *cached =
        ny_proof_try_solverr_cache_lookup(prop_hash, ctx_hash, mode_hash);
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
                    "solver: mode '%s', trying to prove a %s proposition", mode,
                    proposition->tok.lexeme ? proposition->tok.lexeme : "?");
  bool want_pres = strcmp(mode, "presburger") == 0 || strcmp(mode, "fm") == 0 ||
                   strcmp(mode, "fourier-motzkin") == 0;
  bool want_z3 = strcmp(mode, "z3") == 0;
  bool is_auto = strcmp(mode, "auto") == 0;
  bool proved = false;
  if (is_auto) {
    bool pres_proved = ny_proof_try_presburger(cg, scopes, depth, proposition);
    bool z3_proved = false;
    bool z3_counterexample = false;
    char z3_cex[128] = {0};
#ifdef NYTRIX_HAS_Z3
    z3_proved = ny_proof_try_z3(cg, scopes, depth, proposition, z3_cex,
                                sizeof(z3_cex), &z3_counterexample);
#endif
    if (z3_cex[0] && cex && cex_cap > 0)
      snprintf(cex, cex_cap, "%s", z3_cex);

    /*
     * Differential check: contradiction between backends is treated as a
     * compiler bug
     */
    if (pres_proved && z3_counterexample) {
      ny_proof_debug_at(
          1, proposition->tok,
          "differential proof coherence: CONTRADICTION detected between "
          "backends; "
          "Presburger proved proposition, but Z3 found counterexample%s%s",
          z3_cex[0] ? ": " : "", z3_cex);
      if (have_cache_key)
        ny_proof_try_solverr_cache_store(prop_hash, ctx_hash, mode_hash, false,
                                         z3_cex);
      return false;
    }

    proved = pres_proved || z3_proved;
    if (ny_proof_debug_level() >= 2) {
      ny_proof_debug_at(
          2, proposition->tok, "solver coherence: presburger=%s, z3=%s -> %s",
          pres_proved ? "proved" : "unknown", z3_proved ? "proved" : "unknown",
          proved ? "proved" : "unknown");
    }
    if (have_cache_key)
      ny_proof_try_solverr_cache_store(prop_hash, ctx_hash, mode_hash, proved,
                                       cex);
    return proved;
  }
  if (want_z3) {
#ifdef NYTRIX_HAS_Z3
    proved =
        ny_proof_try_z3(cg, scopes, depth, proposition, cex, cex_cap, NULL);
    if (have_cache_key)
      ny_proof_try_solverr_cache_store(prop_hash, ctx_hash, mode_hash, proved,
                                       cex);
    return proved;
#else
    if (have_cache_key)
      ny_proof_try_solverr_cache_store(prop_hash, ctx_hash, mode_hash, false,
                                       cex);
    return false;
#endif
  }
  if (want_pres) {
    proved = ny_proof_try_presburger(cg, scopes, depth, proposition);
    if (have_cache_key)
      ny_proof_try_solverr_cache_store(prop_hash, ctx_hash, mode_hash, proved,
                                       cex);
    return proved;
  }
  if (have_cache_key)
    ny_proof_try_solverr_cache_store(prop_hash, ctx_hash, mode_hash, proved,
                                     cex);
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

static expr_t ny_smt_test_logical(expr_t *left, const char *op, expr_t *right) {
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

static bool ny_smt_test_cmp_truth(int64_t left, const char *op, int64_t right) {
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
          decision > 0   ? "proved"
          : decision < 0 ? "disproved"
                         : "unknown");
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
          if (!ny_smt_test_expect(&p, pt, "comparison", ops[oi], NULL, a, b, c,
                                  &checks) ||
              !ny_smt_test_expect(&not_p, !pt, "negation", ops[oi], NULL, a, b,
                                  c, &checks))
            return 1;
          for (size_t oj = 0; oj < sizeof(ops) / sizeof(ops[0]); ++oj) {
            expr_t q = ny_smt_test_binary(&bv, ops[oj], &cv);
            bool qt = ny_smt_test_cmp_truth(b, ops[oj], c);
            expr_t both = ny_smt_test_logical(&p, "&&", &q);
            expr_t either = ny_smt_test_logical(&p, "||", &q);
            expr_t not_both = ny_smt_test_not(&both);
            expr_t not_either = ny_smt_test_not(&either);
            if (!ny_smt_test_expect(&both, pt && qt, "and", ops[oi], ops[oj], a,
                                    b, c, &checks) ||
                !ny_smt_test_expect(&either, pt || qt, "or", ops[oi], ops[oj],
                                    a, b, c, &checks) ||
                !ny_smt_test_expect(&not_both, !(pt && qt), "not-and", ops[oi],
                                    ops[oj], a, b, c, &checks) ||
                !ny_smt_test_expect(&not_either, !(pt || qt), "not-or", ops[oi],
                                    ops[oj], a, b, c, &checks))
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

/*
 * Canonical-obligation front end
 */

/*
 * The policy surface (Ny module core.proof and the versioned obligation
 * interface) speaks canonical proposition strings, not ASTs.  This front
 * end parses the canonical encoding into synthesized expression nodes and
 * drives the same kernels ny_proof_try_solver uses.  It is the search
 * side: its results carry no trust until the independent bounded checker
 * (policy/check.c) re-derives them.
 */

#define NY_POLC_MAX_NODES 256
#define NY_POLC_MAX_NAMES 16
#define NY_POLC_MAX_NAME_LEN 64
#define NY_POLC_MAX_TEXT 2048

typedef struct {
  expr_t *nodes;
  int count;
  char (*names)[NY_POLC_MAX_NAME_LEN];
  int name_count;
  char *ops;
  int op_count;
  char **strings;
  size_t string_count;
  size_t string_cap;
  size_t cap;
} ny_polc_ctx_t;

static bool ny_polc_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static const char *ny_polc_intern(ny_polc_ctx_t *cx, const char *at,
                                  size_t len) {
  if (len == 0 || len >= NY_POLC_MAX_NAME_LEN)
    return NULL;
  for (int i = 0; i < cx->name_count; ++i) {
    if (strlen(cx->names[i]) == len && strncmp(cx->names[i], at, len) == 0)
      return cx->names[i];
  }
  if (cx->name_count >= NY_POLC_MAX_NAMES)
    return NULL;
  memcpy(cx->names[cx->name_count], at, len);
  cx->names[cx->name_count][len] = '\0';
  return cx->names[cx->name_count++];
}

static const char *ny_polc_op(ny_polc_ctx_t *cx, const char *at, size_t len) {
  if (len == 0 || len >= 8)
    return NULL;
  for (int i = 0; i < cx->op_count; ++i) {
    if (strlen(&cx->ops[i * 8]) == len &&
        strncmp(&cx->ops[i * 8], at, len) == 0)
      return &cx->ops[i * 8];
  }
  if (cx->op_count >= 32)
    return NULL;
  char *slot = &cx->ops[cx->op_count * 8];
  memcpy(slot, at, len);
  slot[len] = '\0';
  cx->op_count++;
  return slot;
}

static const char *ny_polc_keep_string(ny_polc_ctx_t *cx, char *value) {
  if (!cx || !value)
    return NULL;
  if (cx->string_count == cx->string_cap) {
    size_t cap = cx->string_cap ? cx->string_cap * 2 : 4;
    if (cap < cx->string_cap || cap > SIZE_MAX / sizeof(*cx->strings))
      return NULL;
    char **next = realloc(cx->strings, cap * sizeof(*cx->strings));
    if (!next)
      return NULL;
    cx->strings = next;
    cx->string_cap = cap;
  }
  cx->strings[cx->string_count++] = value;
  return value;
}

static int ny_polc_hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static char *ny_polc_decode_json_string(const char *text, size_t len) {
  if (!text || len < 2 || text[0] != '"' || text[len - 1] != '"')
    return NULL;
  char *out = malloc(len);
  if (!out)
    return NULL;
  size_t used = 0;
  for (size_t i = 1; i + 1 < len; ++i) {
    unsigned char ch = (unsigned char)text[i];
    if (ch == '\\') {
      if (++i + 1 >= len)
        goto fail;
      switch (text[i]) {
      case '"':
        ch = '"';
        break;
      case '\\':
        ch = '\\';
        break;
      case 'n':
        ch = '\n';
        break;
      case 'r':
        ch = '\r';
        break;
      case 't':
        ch = '\t';
        break;
      case 'u': {
        if (i + 4 >= len - 1)
          goto fail;
        unsigned value = 0;
        for (int j = 1; j <= 4; ++j) {
          int digit = ny_polc_hex_value(text[i + (size_t)j]);
          if (digit < 0)
            goto fail;
          value = (value << 4) | (unsigned)digit;
        }
        if (value > 0x7f)
          goto fail;
        ch = (unsigned char)value;
        i += 4;
        break;
      }
      default:
        goto fail;
      }
    } else if (ch < 0x20) {
      goto fail;
    }
    out[used++] = (char)ch;
  }
  out[used] = '\0';
  return out;
fail:
  free(out);
  return NULL;
}

static int ny_polc_parse(ny_polc_ctx_t *cx, const char *s, size_t len,
                         int depth) {
  if (!cx || depth > NY_SMT_MAX_DEPTH || cx->count >= (int)cx->cap)
    return -1;
  if (len > 5 && strncmp(s, "name:", 5) == 0) {
    for (size_t i = 5; i < len; ++i) {
      if (!ny_polc_name_char(s[i]))
        return -1;
    }
    const char *name = ny_polc_intern(cx, s + 5, len - 5);
    if (!name)
      return -1;
    int idx = cx->count++;
    expr_t *e = &cx->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->kind = NY_E_IDENT;
    e->as.ident.name = name;
    return idx;
  }
  if (len > 4 && strncmp(s, "int:", 4) == 0) {
    if (len > 24)
      return -1;
    char buf[24];
    memcpy(buf, s + 4, len - 4);
    buf[len - 4] = '\0';
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (!end || *end)
      return -1;
    int idx = cx->count++;
    expr_t *e = &cx->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->kind = NY_E_LITERAL;
    e->as.literal.kind = NY_LIT_INT;
    e->as.literal.as.i = (int64_t)v;
    return idx;
  }
  if (len > 6 && strncmp(s, "float:", 6) == 0) {
    char *buf = ny_strndup(s + 6, len - 6);
    if (!buf)
      return -1;
    char *end = NULL;
    double value = strtod(buf, &end);
    bool parsed = end && !*end;
    free(buf);
    if (!parsed)
      return -1;
    int idx = cx->count++;
    expr_t *e = &cx->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->kind = NY_E_LITERAL;
    e->as.literal.kind = NY_LIT_FLOAT;
    e->as.literal.as.f = value;
    return idx;
  }
  if (len == 9 && strncmp(s, "bool:true", 9) == 0) {
    int idx = cx->count++;
    expr_t *e = &cx->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->kind = NY_E_LITERAL;
    e->as.literal.kind = NY_LIT_BOOL;
    e->as.literal.as.b = true;
    return idx;
  }
  if (len == 10 && strncmp(s, "bool:false", 10) == 0) {
    int idx = cx->count++;
    expr_t *e = &cx->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->kind = NY_E_LITERAL;
    e->as.literal.kind = NY_LIT_BOOL;
    e->as.literal.as.b = false;
    return idx;
  }
  if (len >= 6 && strncmp(s, "str:", 4) == 0) {
    char *value = ny_polc_decode_json_string(s + 4, len - 4);
    const char *stored = value ? ny_polc_keep_string(cx, value) : NULL;
    if (!stored) {
      free(value);
      return -1;
    }
    int idx = cx->count++;
    expr_t *e = &cx->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->kind = NY_E_LITERAL;
    e->as.literal.kind = NY_LIT_STR;
    e->as.literal.as.s.data = stored;
    e->as.literal.as.s.len = strlen(stored);
    return idx;
  }
  if (len > 9 && strncmp(s, "unary:!(", 8) == 0 && s[len - 1] == ')') {
    int inner = ny_polc_parse(cx, s + 8, len - 9, depth + 1);
    if (inner < 0)
      return -1;
    const char *op = ny_polc_op(cx, "!", 1);
    if (!op)
      return -1;
    int idx = cx->count++;
    expr_t *e = &cx->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->kind = NY_E_UNARY;
    e->as.unary.op = op;
    e->as.unary.right = &cx->nodes[inner];
    return idx;
  }
  if (len > 9 && strncmp(s, "unary:-(", 8) == 0 && s[len - 1] == ')') {
    int inner = ny_polc_parse(cx, s + 8, len - 9, depth + 1);
    if (inner < 0)
      return -1;
    const char *op = ny_polc_op(cx, "-", 1);
    if (!op)
      return -1;
    int idx = cx->count++;
    expr_t *e = &cx->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->kind = NY_E_UNARY;
    e->as.unary.op = op;
    e->as.unary.right = &cx->nodes[inner];
    return idx;
  }
  if (len > 8 && strncmp(s, "binary:", 7) == 0) {
    const char *open = memchr(s + 7, '(', len - 7);
    if (!open || s[len - 1] != ')')
      return -1;
    size_t oplen = (size_t)(open - (s + 7));
    if (oplen == 0 || oplen >= 8)
      return -1;
    const char *body = open + 1;
    size_t body_len = len - (size_t)(body - s) - 1;
    if (body_len == 0)
      return -1;
    int paren = 0;
    const char *comma = NULL;
    for (size_t i = 0; i < body_len; ++i) {
      if (body[i] == '(')
        ++paren;
      else if (body[i] == ')') {
        if (paren == 0)
          return -1;
        --paren;
      } else if (body[i] == ',' && paren == 0 && !comma)
        comma = body + i;
    }
    if (paren != 0 || !comma)
      return -1;
    int l = ny_polc_parse(cx, body, (size_t)(comma - body), depth + 1);
    if (l < 0)
      return -1;
    int r = ny_polc_parse(cx, comma + 1, body_len - (size_t)(comma - body) - 1,
                          depth + 1);
    if (r < 0)
      return -1;
    const char *op = ny_polc_op(cx, s + 7, oplen);
    if (!op)
      return -1;
    bool logical = strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
    int idx = cx->count++;
    expr_t *e = &cx->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->kind = logical ? NY_E_LOGICAL : NY_E_BINARY;
    if (logical) {
      e->as.logical.op = op;
      e->as.logical.left = &cx->nodes[l];
      e->as.logical.right = &cx->nodes[r];
    } else {
      e->as.binary.op = op;
      e->as.binary.left = &cx->nodes[l];
      e->as.binary.right = &cx->nodes[r];
    }
    return idx;
  }
  return -1;
}

static void ny_polc_ctx_free(ny_polc_ctx_t *cx) {
  if (!cx)
    return;
  free(cx->nodes);
  free(cx->names);
  free(cx->ops);
  for (size_t i = 0; i < cx->string_count; ++i)
    free(cx->strings[i]);
  free(cx->strings);
  memset(cx, 0, sizeof(*cx));
}

static bool ny_polc_ctx_init(ny_polc_ctx_t *cx) {
  memset(cx, 0, sizeof(*cx));
  cx->cap = NY_POLC_MAX_NODES;
  cx->nodes = malloc(sizeof(expr_t) * cx->cap);
  cx->names = malloc(sizeof(char[NY_POLC_MAX_NAME_LEN]) * NY_POLC_MAX_NAMES);
  cx->ops = malloc(8 * 32);
  if (!cx->nodes || !cx->names || !cx->ops) {
    ny_polc_ctx_free(cx);
    return false;
  }
  return true;
}

/*
 * Parse `name=lo..hi;...` into the solver environment.  `name_pool` backs
 * the stored names (ny_smt_env_t keeps pointers, so the range text cannot
 * own them).  Returns false on a malformed range field.
 */
static bool ny_polc_parse_ranges(ny_smt_env_t *env, const char *ranges,
                                 char (*name_pool)[NY_POLC_MAX_NAME_LEN]) {
  if (!ranges || !*ranges)
    return true;
  const char *p = ranges;
  while (*p) {
    char name[NY_POLC_MAX_NAME_LEN];
    size_t i = 0;
    while (*p && *p != '=' && *p != ';') {
      if (i + 1 >= sizeof(name) || !ny_polc_name_char(*p))
        return false;
      name[i++] = *p++;
    }
    name[i] = '\0';
    if (i == 0 || *p != '=')
      return false;
    ++p;
    const char *lo = p;
    while (*p && *p != '.' && *p != ';')
      ++p;
    if (*p != '.' || p[1] != '.')
      return false;
    int vi = ny_smt_env_add(env, name);
    if (vi < 0)
      return false;
    /*
     * The intern table stores the caller's pointer; re-point it at the
     * front end's persistent pool so it survives this function.
     */
    if (strlen(name) < NY_POLC_MAX_NAME_LEN) {
      memcpy(name_pool[vi], name, strlen(name) + 1);
      env->vars[vi].name = name_pool[vi];
    }
    env->vars[vi].lo = strtoll(lo, NULL, 10);
    p += 2;
    const char *hi = p;
    while (*p && *p != ';')
      ++p;
    env->vars[vi].hi = strtoll(hi, NULL, 10);
    env->vars[vi].has_range = true;
    if (env->vars[vi].lo > env->vars[vi].hi)
      return false;
    if (*p == ';')
      ++p;
  }
  return true;
}

/*
 * Exact evaluation of the canonical proposition under a full integer
 * assignment, with C semantics.  Used to verify counterexamples before a
 * refuted verdict is reported.  Returns false when the evaluation cannot
 * be carried out (division by zero, unbound name, broken invariant).
 */
static bool ny_polc_eval_expr(expr_t *e, ny_smt_env_t *env, const int64_t *vals,
                              int64_t *out, int depth) {
  if (!e || depth > NY_SMT_MAX_DEPTH)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT) {
    *out = e->as.literal.as.i;
    return true;
  }
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_BOOL) {
    *out = e->as.literal.as.b ? 1 : 0;
    return true;
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    int vi = ny_smt_env_add(env, e->as.ident.name);
    if (vi < 0 || vi >= NY_SMT_MAX_VARS)
      return false;
    *out = vals[vi];
    return true;
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op) {
    int64_t v = 0;
    if (!ny_polc_eval_expr(e->as.unary.right, env, vals, &v, depth + 1))
      return false;
    if (strcmp(e->as.unary.op, "!") == 0) {
      *out = v == 0 ? 1 : 0;
      return true;
    }
    if (strcmp(e->as.unary.op, "-") == 0) {
      if (v == INT64_MIN)
        return false;
      *out = -v;
      return true;
    }
    return false;
  }
  if (e->kind == NY_E_LOGICAL && e->as.logical.op) {
    int64_t l = 0, r = 0;
    if (!ny_polc_eval_expr(e->as.logical.left, env, vals, &l, depth + 1) ||
        !ny_polc_eval_expr(e->as.logical.right, env, vals, &r, depth + 1))
      return false;
    bool lb = l != 0, rb = r != 0;
    *out = strcmp(e->as.logical.op, "&&") == 0 ? (lb && rb ? 1 : 0)
                                               : (lb || rb ? 1 : 0);
    return true;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op) {
    int64_t l = 0, r = 0;
    if (!ny_polc_eval_expr(e->as.binary.left, env, vals, &l, depth + 1) ||
        !ny_polc_eval_expr(e->as.binary.right, env, vals, &r, depth + 1))
      return false;
    const char *op = e->as.binary.op;
    if (strcmp(op, "<") == 0) {
      *out = l < r;
      return true;
    }
    if (strcmp(op, "<=") == 0) {
      *out = l <= r;
      return true;
    }
    if (strcmp(op, ">") == 0) {
      *out = l > r;
      return true;
    }
    if (strcmp(op, ">=") == 0) {
      *out = l >= r;
      return true;
    }
    if (strcmp(op, "==") == 0) {
      *out = l == r;
      return true;
    }
    if (strcmp(op, "!=") == 0) {
      *out = l != r;
      return true;
    }
    if (strcmp(op, "&") == 0) {
      *out = l & r;
      return true;
    }
    if (strcmp(op, "|") == 0) {
      *out = l | r;
      return true;
    }
    if (strcmp(op, "^") == 0) {
      *out = l ^ r;
      return true;
    }
    if (strcmp(op, "<<") == 0) {
      if (r < 0 || r >= 64)
        return false;
      *out = (int64_t)((uint64_t)l << (unsigned)r);
      return true;
    }
    if (strcmp(op, ">>") == 0) {
      if (r < 0 || r >= 64)
        return false;
      *out = l >> r;
      return true;
    }
    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
      if (r == 0 || (l == INT64_MIN && r == -1))
        return false;
      *out = strcmp(op, "/") == 0 ? l / r : l % r;
      return true;
    }
    __int128 wide;
    if (strcmp(op, "+") == 0)
      wide = (__int128)l + r;
    else if (strcmp(op, "-") == 0)
      wide = (__int128)l - r;
    else if (strcmp(op, "*") == 0)
      wide = (__int128)l * r;
    else
      return false;
    if (wide < INT64_MIN || wide > INT64_MAX)
      return false;
    *out = (int64_t)wide;
    return true;
  }
  return false;
}

int ny_proof_solver_check_canonical(const char *prop, const char *ranges,
                                    const char *mode, int max_vars,
                                    int max_cons, int max_depth, char *cex,
                                    size_t cex_cap) {
  (void)max_cons;
  (void)max_depth;
  if (cex && cex_cap > 0)
    cex[0] = '\0';
  if (!prop || strlen(prop) > NY_POLC_MAX_TEXT)
    return NY_PROOF_VERDICT_UNKNOWN;
  ny_smt_env_t env;
  memset(&env, 0, sizeof(env));
  char range_names[NY_SMT_MAX_VARS][NY_POLC_MAX_NAME_LEN];
  if (!ny_polc_parse_ranges(&env, ranges, range_names))
    return NY_PROOF_VERDICT_UNKNOWN;
  if (env.nvars > (max_vars > 0 ? max_vars : NY_SMT_MAX_VARS) ||
      env.nvars > NY_SMT_MAX_VARS)
    return NY_PROOF_VERDICT_UNKNOWN;
  ny_polc_ctx_t cx;
  if (!ny_polc_ctx_init(&cx))
    return NY_PROOF_VERDICT_UNKNOWN;
  /*
   * The kernels read binding ranges through a codegen context; the
   * canonical front end carries all context in the obligation, so a zeroed
   * codegen (no scopes, no registry) is the correct environment here -
   * the same arrangement the in-file differential selftest uses.
   */
  codegen_t null_cg;
  memset(&null_cg, 0, sizeof(null_cg));
  int root = ny_polc_parse(&cx, prop, strlen(prop), 0);
  int verdict = NY_PROOF_VERDICT_UNKNOWN;
  do {
    if (root < 0)
      break;
    const char *m = mode && *mode ? mode : "auto";
    bool want_pres = strcmp(m, "presburger") == 0 || strcmp(m, "fm") == 0 ||
                     strcmp(m, "fourier-motzkin") == 0 ||
                     strcmp(m, "auto") == 0;
    bool want_z3 = strcmp(m, "z3") == 0 || strcmp(m, "auto") == 0;
    bool pres_proved = false;
    bool z3_proved = false;
    bool z3_counterexample = false;
    char z3_cex[128] = {0};
    if (want_pres)
      pres_proved =
          ny_proof_presburger_env(&null_cg, NULL, 0, &env, &cx.nodes[root]);
#ifdef NYTRIX_HAS_Z3
    if (want_z3 && !pres_proved)
      z3_proved = ny_proof_z3_env(&null_cg, NULL, 0, &env, &cx.nodes[root],
                                  z3_cex, sizeof(z3_cex), &z3_counterexample);
#endif
    if (pres_proved || z3_proved) {
      verdict = NY_PROOF_VERDICT_PROVED;
      break;
    }
    if (z3_counterexample && z3_cex[0] && env.nvars > 0) {
      /*
       * A refuted verdict needs a full verified counterexample.  Bind the
       * model hint, evaluate exactly, and only then report refutation;
       * anything short of that stays unknown.
       */
      int64_t vals[NY_SMT_MAX_VARS] = {0};
      const char *eq = strchr(z3_cex, '=');
      if (eq) {
        char name[NY_POLC_MAX_NAME_LEN];
        size_t n = (size_t)(eq - z3_cex);
        if (n > 0 && n < sizeof(name)) {
          memcpy(name, z3_cex, n);
          name[n] = '\0';
          for (int v = 0; v < env.nvars; ++v) {
            if (strcmp(env.vars[v].name, name) == 0)
              vals[v] = strtoll(eq + 1, NULL, 10);
          }
        }
      }
      int64_t result = 0;
      int nvars_before_eval = env.nvars;
      bool evaluated =
          ny_polc_eval_expr(&cx.nodes[root], &env, vals, &result, 0);
      /*
       * Evaluating must not invent bindings: a name the kernels never saw
       * would silently read as zero and could fake a refutation.
       */
      if (evaluated && env.nvars == nvars_before_eval && result == 0) {
        verdict = NY_PROOF_VERDICT_REFUTED;
        if (cex && cex_cap > 0)
          snprintf(cex, cex_cap, "%s", z3_cex);
      }
    }
  } while (0);
  ny_polc_ctx_free(&cx);
  return verdict;
}

expr_t *ny_proof_expr_from_canonical(arena_t *arena, const char *text) {
  if (!arena || !text)
    return NULL;
  const char *body = text;
  size_t len = strlen(text);
  if (len >= 6 && strncmp(body, "proof<", 6) == 0) {
    if (len < 7 || body[len - 1] != '>')
      return NULL;
    body += 6;
    len -= 7;
  }
  if (!len || len > 16384)
    return NULL;

  ny_polc_ctx_t cx;
  if (!ny_polc_ctx_init(&cx))
    return NULL;
  int root = ny_polc_parse(&cx, body, len, 0);
  if (root < 0) {
    ny_polc_ctx_free(&cx);
    return NULL;
  }

  expr_t *nodes = arena_alloc(arena, sizeof(*nodes) * (size_t)cx.count);
  if (!nodes) {
    ny_polc_ctx_free(&cx);
    return NULL;
  }
  for (int i = 0; i < cx.count; ++i) {
    nodes[i] = cx.nodes[i];
    if (nodes[i].kind == NY_E_IDENT) {
      const char *name = cx.nodes[i].as.ident.name;
      size_t name_len = name ? strlen(name) : 0;
      char *owned = arena_alloc(arena, name_len + 1);
      if (!owned) {
        ny_polc_ctx_free(&cx);
        return NULL;
      }
      if (name_len)
        memcpy(owned, name, name_len);
      owned[name_len] = '\0';
      nodes[i].as.ident.name = owned;
    } else if (nodes[i].kind == NY_E_LITERAL &&
               nodes[i].as.literal.kind == NY_LIT_STR) {
      size_t string_len = cx.nodes[i].as.literal.as.s.len;
      char *owned = arena_alloc(arena, string_len + 1);
      if (!owned) {
        ny_polc_ctx_free(&cx);
        return NULL;
      }
      if (string_len)
        memcpy(owned, cx.nodes[i].as.literal.as.s.data, string_len);
      owned[string_len] = '\0';
      nodes[i].as.literal.as.s.data = owned;
    } else if (nodes[i].kind == NY_E_UNARY) {
      const char *op = cx.nodes[i].as.unary.op;
      size_t op_len = op ? strlen(op) : 0;
      char *owned = arena_alloc(arena, op_len + 1);
      if (!owned) {
        ny_polc_ctx_free(&cx);
        return NULL;
      }
      if (op_len)
        memcpy(owned, op, op_len);
      owned[op_len] = '\0';
      nodes[i].as.unary.op = owned;
      ptrdiff_t child = cx.nodes[i].as.unary.right - cx.nodes;
      if (child < 0 || child >= cx.count) {
        ny_polc_ctx_free(&cx);
        return NULL;
      }
      nodes[i].as.unary.right = &nodes[child];
    } else if (nodes[i].kind == NY_E_BINARY || nodes[i].kind == NY_E_LOGICAL) {
      const char *op = nodes[i].kind == NY_E_LOGICAL ? cx.nodes[i].as.logical.op
                                                     : cx.nodes[i].as.binary.op;
      size_t op_len = op ? strlen(op) : 0;
      char *owned = arena_alloc(arena, op_len + 1);
      if (!owned) {
        ny_polc_ctx_free(&cx);
        return NULL;
      }
      if (op_len)
        memcpy(owned, op, op_len);
      owned[op_len] = '\0';
      if (nodes[i].kind == NY_E_LOGICAL) {
        ptrdiff_t left = cx.nodes[i].as.logical.left - cx.nodes;
        ptrdiff_t right = cx.nodes[i].as.logical.right - cx.nodes;
        if (left < 0 || left >= cx.count || right < 0 || right >= cx.count) {
          ny_polc_ctx_free(&cx);
          return NULL;
        }
        nodes[i].as.logical.op = owned;
        nodes[i].as.logical.left = &nodes[left];
        nodes[i].as.logical.right = &nodes[right];
      } else {
        ptrdiff_t left = cx.nodes[i].as.binary.left - cx.nodes;
        ptrdiff_t right = cx.nodes[i].as.binary.right - cx.nodes;
        if (left < 0 || left >= cx.count || right < 0 || right >= cx.count) {
          ny_polc_ctx_free(&cx);
          return NULL;
        }
        nodes[i].as.binary.op = owned;
        nodes[i].as.binary.left = &nodes[left];
        nodes[i].as.binary.right = &nodes[right];
      }
    }
  }
  expr_t *result = &nodes[root];
  ny_polc_ctx_free(&cx);
  return result;
}
