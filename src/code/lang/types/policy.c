/*
 * Versioned proof-obligation / proof-certificate interface and the single
 * policy funnel for compiler proof sites.
 *
 * lib/proof.ny owns the language-level core.proof policy and shares this
 * versioned encoding through the __proof_policy_solve and
 * __proof_policy_check runtime builtins. This file is a compiler-side adapter
 * for compiler proof sites: caller-driven fold first (via
 * ny_proof_policy_mark_fold, mirroring core.proof tactic_fold), then lemma
 * instantiation (cc), known ranges (range), solver kernels
 * (presburger/z3/auto), and the bounded interval candidate — i.e. the
 * lib/proof.ny tactic order fold → range → solver (proof.ny:637-647)
 * with the compiler-only cc/interval steps preserved. Budgets stay
 * compiler-enforced (Ny 8/32/64 in proof.ny:34-37; checker caps larger so
 * it never starves). Before reporting a compiler verdict, every path
 * validates the certificate with the independent bounded checker in
 * policy/check.c, which stays the sole decider. A rejected proof
 * downgrades to unknown; the checker alone never proves a proposition.
 */
#include "code/lang/types/policy.h"
#include "base/strbuf.h"
#include "code/lang/parse/proof.h"
#include "code/lang/priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Canonical propositions legitimately contain '|' (the logical-or
 * operator), so interface fields percent-encode '|' and '%' before
 * assembly.  Digests are computed over the escaped form, which both sides
 * of the interface see verbatim.
 */
void ny_proof_policy_escape_field(const char *in, char *out, size_t out_cap) {
  size_t o = 0;
  if (!out || out_cap == 0)
    return;
  for (const char *p = in ? in : ""; *p; ++p) {
    size_t width = (*p == '%' || *p == '|') ? 3u : 1u;
    if (width > out_cap - 1 - o) {
      out[0] = '\0';
      return;
    }
    if (*p == '%') {
      out[o++] = '%';
      out[o++] = '2';
      out[o++] = '5';
    } else if (*p == '|') {
      out[o++] = '%';
      out[o++] = '7';
      out[o++] = 'c';
    } else {
      out[o++] = *p;
    }
  }
  out[o] = '\0';
}

void ny_proof_policy_unescape_field(const char *in, char *out, size_t out_cap) {
  size_t o = 0;
  if (!out || out_cap == 0)
    return;
  if (!in) {
    out[0] = '\0';
    return;
  }
  for (const char *p = in; *p;) {
    if (o + 1 >= out_cap) {
      out[0] = '\0';
      return;
    }
    if (*p == '%' && p[1] && p[2]) {
      int hi = p[1] >= '0' && p[1] <= '9'
                   ? p[1] - '0'
                   : (p[1] >= 'a' && p[1] <= 'f' ? p[1] - 'a' + 10 : -1);
      int lo = p[2] >= '0' && p[2] <= '9'
                   ? p[2] - '0'
                   : (p[2] >= 'a' && p[2] <= 'f' ? p[2] - 'a' + 10 : -1);
      /*
       * tolerate the uppercase spelling on input for robustness
       */
      if (lo < 0 && p[2] >= 'A' && p[2] <= 'F')
        lo = p[2] - 'A' + 10;
      if (hi < 0 && p[1] >= 'A' && p[1] <= 'F')
        hi = p[1] - 'A' + 10;
      if (hi >= 0 && lo >= 0) {
        out[o++] = (char)((hi << 4) | lo);
        p += 3;
        continue;
      }
    }
    out[o++] = *p++;
  }
  out[o] = '\0';
}

uint64_t ny_proof_policy_digest_text(const char *text) {
  uint64_t h = UINT64_C(1469598103934665603);
  if (!text)
    return h;
  for (const char *p = text; *p; ++p) {
    h ^= (uint64_t)(unsigned char)*p;
    h *= UINT64_C(1099511628211);
  }
  return h;
}

void ny_proof_policy_digest_hex(uint64_t digest, char *out) {
  static const char hex[] = "0123456789abcdef";
  if (!out)
    return;
  for (int i = 15; i >= 0; --i) {
    out[i] = hex[digest & 0xf];
    digest >>= 4;
  }
  out[16] = '\0';
}

/*
 * Canonical tactic tokens.  The raw `by_*` spellings from strategy strings
 * map onto these; any other `by_*` token is a malformed policy.
 */
typedef enum {
  NY_POL_TACTIC_FOLD = 1 << 0,
  NY_POL_TACTIC_RANGE = 1 << 1,
  NY_POL_TACTIC_CC = 1 << 2,
  NY_POL_TACTIC_SOLVER = 1 << 3,
} ny_pol_tactic_t;

typedef enum {
  NY_POL_MODE_DEFAULT = 0, /* use cg->proof_solver verbatim */
  NY_POL_MODE_AUTO,
  NY_POL_MODE_PRESBURGER,
  NY_POL_MODE_Z3,
} ny_pol_mode_t;

typedef struct {
  unsigned tactics; /* 0 = default: every tactic */
  ny_pol_mode_t mode;
  bool malformed;
  char reason[NY_PROOF_POLICY_EXPLAIN_MAX];
} ny_pol_policy_t;

static bool ny_pol_is_sep(char c) {
  return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '\n' ||
         c == '\r';
}

static bool ny_pol_token_known(const char *tok, size_t len, unsigned *flag,
                               ny_pol_mode_t *mode) {
#define NY_POL_TOK(lit) (len == strlen(lit) && strncmp(tok, lit, len) == 0)
  if (NY_POL_TOK("by_fold") || NY_POL_TOK("fold")) {
    *flag = NY_POL_TACTIC_FOLD;
    return true;
  }
  if (NY_POL_TOK("by_range") || NY_POL_TOK("range")) {
    /*
     * Legacy spelling: by_range covers the range tactic and the bounded
     * Presburger kernels (that is what it always gated in the checker).
     */
    *flag = NY_POL_TACTIC_RANGE | NY_POL_TACTIC_SOLVER;
    if (*mode == NY_POL_MODE_DEFAULT)
      *mode = NY_POL_MODE_PRESBURGER;
    return true;
  }
  if (NY_POL_TOK("by_cc") || NY_POL_TOK("cc")) {
    *flag = NY_POL_TACTIC_CC;
    return true;
  }
  if (NY_POL_TOK("by_solver") || NY_POL_TOK("solver") || NY_POL_TOK("auto")) {
    *flag = NY_POL_TACTIC_SOLVER;
    *mode = NY_POL_MODE_AUTO;
    return true;
  }
  if (NY_POL_TOK("by_z3") || NY_POL_TOK("z3")) {
    *flag = NY_POL_TACTIC_SOLVER;
    *mode = NY_POL_MODE_Z3;
    return true;
  }
  if (NY_POL_TOK("kernels")) {
    /*
     * Solver tactic with the CLI-configured mode: for sites that must not
     * run the lemma-instantiation tactic (a lemma's own definition check
     * meets its applications inside propositions).
     */
    *flag = NY_POL_TACTIC_SOLVER;
    return true;
  }
  if (NY_POL_TOK("presburger") || NY_POL_TOK("by_presburger") ||
      NY_POL_TOK("fm") || NY_POL_TOK("fourier-motzkin")) {
    *flag = NY_POL_TACTIC_SOLVER;
    if (*mode != NY_POL_MODE_Z3)
      *mode = NY_POL_MODE_PRESBURGER;
    return true;
  }
  if (NY_POL_TOK("all")) {
    *flag = NY_POL_TACTIC_FOLD | NY_POL_TACTIC_RANGE | NY_POL_TACTIC_CC |
            NY_POL_TACTIC_SOLVER;
    return true;
  }
#undef NY_POL_TOK
  return false;
}

static void ny_pol_policy_parse(const char *policy_text, ny_pol_policy_t *out) {
  memset(out, 0, sizeof(*out));
  out->mode = NY_POL_MODE_DEFAULT;
  if (!policy_text || !*policy_text) {
    /*
     * empty policy: every tactic, solver mode from the CLI setting
     */
    out->tactics = NY_POL_TACTIC_FOLD | NY_POL_TACTIC_RANGE | NY_POL_TACTIC_CC |
                   NY_POL_TACTIC_SOLVER;
    return;
  }
  const char *p = policy_text;
  while (*p) {
    while (*p && ny_pol_is_sep(*p))
      ++p;
    if (!*p)
      break;
    const char *start = p;
    while (*p && !ny_pol_is_sep(*p))
      ++p;
    size_t len = (size_t)(p - start);
    unsigned flag = 0;
    ny_pol_mode_t mode = NY_POL_MODE_DEFAULT;
    if (ny_pol_token_known(start, len, &flag, &mode)) {
      out->tactics |= flag;
      if (mode == NY_POL_MODE_AUTO)
        out->mode = NY_POL_MODE_AUTO;
      else if (mode == NY_POL_MODE_Z3) {
        if (out->mode == NY_POL_MODE_PRESBURGER)
          out->mode = NY_POL_MODE_AUTO;
        else if (out->mode != NY_POL_MODE_AUTO)
          out->mode = NY_POL_MODE_Z3;
      } else if (mode == NY_POL_MODE_PRESBURGER) {
        if (out->mode == NY_POL_MODE_Z3)
          out->mode = NY_POL_MODE_AUTO;
        else if (out->mode == NY_POL_MODE_DEFAULT)
          out->mode = NY_POL_MODE_PRESBURGER;
      }
    } else if (len > 3 && strncmp(start, "by_", 3) == 0) {
      /*
       * Tokens shaped like tactics must name real tactics: malformed
       * policies fail closed instead of degrading to free-form text.
       */
      out->malformed = true;
      snprintf(out->reason, sizeof(out->reason),
               "unknown proof tactic '%.*s' (known: by_cc by_range by_z3 "
               "by_fold)",
               (int)(len > 32 ? 32 : len), start);
      return;
    }
  }
}

bool ny_proof_policy_validate_text(const char *policy_text, char *reason,
                                   size_t reason_cap) {
  ny_pol_policy_t policy;
  ny_pol_policy_parse(policy_text, &policy);
  if (policy.malformed) {
    if (reason && reason_cap > 0)
      snprintf(reason, reason_cap, "%s", policy.reason);
    return false;
  }
  return true;
}

/*
 * Collect the distinct `name:` identifiers of a canonical proposition in
 * order of appearance.  Bounded: at most 64 names, 128 bytes each.
 */
#define NY_POL_MAX_NAMES 64

typedef struct {
  char names[NY_POL_MAX_NAMES][128];
  int count;
  bool overflow;
} ny_pol_names_t;

static void ny_pol_collect_names(const char *prop, ny_pol_names_t *out) {
  memset(out, 0, sizeof(*out));
  if (!prop)
    return;
  const char *p = prop;
  while ((p = strstr(p, "name:")) != NULL) {
    p += 5;
    char name[128];
    size_t n = 0;
    while (*p && *p != ',' && *p != ')' && *p != '>' && n + 1 < sizeof(name))
      name[n++] = *p++;
    name[n] = '\0';
    if (n == 0 || n >= sizeof(name) - 1) {
      if (n > 0)
        out->overflow = true;
    } else {
      bool seen = false;
      for (int i = 0; i < out->count; ++i) {
        if (strcmp(out->names[i], name) == 0) {
          seen = true;
          break;
        }
      }
      if (!seen && out->count < NY_POL_MAX_NAMES) {
        snprintf(out->names[out->count], sizeof(out->names[0]), "%s", name);
        out->count++;
      } else if (!seen) {
        out->overflow = true;
      }
    }
  }
}

static int ny_pol_range_name_cmp(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

/*
 * Serialize the integer ranges known for the proposition's names as
 * `name=lo..hi` joined by ';', sorted by name for digest stability.
 */
static bool ny_pol_ranges_text(codegen_t *cg, scope *scopes, size_t depth,
                               const ny_pol_names_t *names, char *out,
                               size_t out_cap) {
  if (!out || out_cap == 0)
    return false;
  out[0] = '\0';
  if (names->overflow || names->count == 0)
    return !names->overflow;
  const char *sorted[NY_POL_MAX_NAMES];
  for (int i = 0; i < names->count; ++i)
    sorted[i] = names->names[i];
  qsort(sorted, (size_t)names->count, sizeof(sorted[0]), ny_pol_range_name_cmp);
  ny_strbuf_t b;
  ny_strbuf_init(&b);
  bool ok = true;
  for (int i = 0; i < names->count && ok; ++i) {
    const char *name = sorted[i];
    binding *bd =
        cg && scopes
            ? lookup_binding_hash(cg, scopes, depth, name, strlen(name),
                                  ny_hash_name(name, strlen(name)))
            : NULL;
    if (!bd || !bd->has_int_range)
      continue;
    if (b.len)
      ny_strbuf_append_c(&b, ';');
    ny_strbuf_appendf(&b, "%s=%lld..%lld", name, (long long)bd->int_min_raw,
                      (long long)bd->int_max_raw);
  }
  if (b.len >= out_cap)
    ok = false;
  else
    memcpy(out, b.data ? b.data : "", b.len + 1);
  ny_strbuf_free(&b);
  return ok;
}

static bool ny_pol_field_clean(const char *s) {
  return s && !strchr(s, '|') && !strchr(s, '\n') && !strchr(s, '\r');
}

static bool ny_pol_obligation_serialize(const char *prop, const char *ranges,
                                        const char *policy_field, int max_vars,
                                        int max_cons, int max_depth, char *out,
                                        size_t out_cap) {
  if (!ny_pol_field_clean(ranges) || !ny_pol_field_clean(policy_field))
    return false;
  char esc_prop[NY_PROOF_OBLIGATION_MAX];
  char esc_ranges[NY_PROOF_OBLIGATION_MAX];
  char esc_policy[128];
  ny_proof_policy_escape_field(prop ? prop : "", esc_prop, sizeof(esc_prop));
  ny_proof_policy_escape_field(ranges ? ranges : "", esc_ranges,
                               sizeof(esc_ranges));
  ny_proof_policy_escape_field(policy_field ? policy_field : "", esc_policy,
                               sizeof(esc_policy));
  int n = snprintf(out, out_cap, "%s|%s|%s|%s|v%d,c%d,d%d",
                   NY_PROOF_OBLIGATION_FORMAT, esc_prop, esc_ranges, esc_policy,
                   max_vars, max_cons, max_depth);
  return n >= 0 && (size_t)n < out_cap;
}

static bool ny_pol_certificate_serialize(const char *obligation_text,
                                         const char *verdict,
                                         const char *tactic,
                                         const char *backend, const char *cex,
                                         const char *explanation, char *out,
                                         size_t out_cap) {
  char digest_hex[17];
  ny_proof_policy_digest_hex(ny_proof_policy_digest_text(obligation_text),
                             digest_hex);
  if (!verdict || !ny_pol_field_clean(tactic) || !ny_pol_field_clean(backend))
    return false;
  char esc_tactic[64], esc_backend[64], esc_cex[256], esc_expl[512];
  ny_proof_policy_escape_field(tactic ? tactic : "", esc_tactic,
                               sizeof(esc_tactic));
  ny_proof_policy_escape_field(backend ? backend : "", esc_backend,
                               sizeof(esc_backend));
  ny_proof_policy_escape_field(cex ? cex : "", esc_cex, sizeof(esc_cex));
  ny_proof_policy_escape_field(explanation ? explanation : "", esc_expl,
                               sizeof(esc_expl));
  int n = snprintf(out, out_cap, "%s|%s|%s|%s|%s|%s|%s",
                   NY_PROOF_CERTIFICATE_FORMAT, digest_hex, verdict, esc_tactic,
                   esc_backend, esc_cex, esc_expl);
  return n >= 0 && (size_t)n < out_cap;
}

/*
 * Normalize the policy field of an obligation to the canonical token list
 * (never raw strategy text, which doubles as a free-form message).
 */
static void ny_pol_policy_field(const ny_pol_policy_t *policy,
                                bool contract_safe, char *out, size_t out_cap) {
  char tokens[64];
  tokens[0] = '\0';
  if (policy->tactics & NY_POL_TACTIC_CC)
    strncat(tokens, "cc,", sizeof(tokens) - strlen(tokens) - 1);
  if (policy->tactics & NY_POL_TACTIC_RANGE)
    strncat(tokens, "range,", sizeof(tokens) - strlen(tokens) - 1);
  if (policy->tactics & NY_POL_TACTIC_SOLVER) {
    if (policy->mode == NY_POL_MODE_Z3)
      strncat(tokens, "z3,", sizeof(tokens) - strlen(tokens) - 1);
    else if (policy->mode == NY_POL_MODE_PRESBURGER)
      strncat(tokens, "presburger,", sizeof(tokens) - strlen(tokens) - 1);
    else if (policy->mode == NY_POL_MODE_AUTO)
      strncat(tokens, "solver,", sizeof(tokens) - strlen(tokens) - 1);
    else
      strncat(tokens, "kernels,", sizeof(tokens) - strlen(tokens) - 1);
  }
  if (policy->tactics & NY_POL_TACTIC_FOLD)
    strncat(tokens, "fold,", sizeof(tokens) - strlen(tokens) - 1);
  size_t len = strlen(tokens);
  if (len > 0 && tokens[len - 1] == ',')
    tokens[len - 1] = '\0';
  snprintf(out, out_cap, "%s%s", contract_safe ? "contract+" : "", tokens);
}

bool ny_proof_policy_obligation_build(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *proposition,
                                      const char *policy_text,
                                      bool contract_safe, char *out,
                                      size_t out_cap) {
  if (!out || out_cap == 0 || !proposition)
    return false;
  ny_pol_policy_t policy;
  ny_pol_policy_parse(policy_text, &policy);
  if (policy.malformed)
    return false;
  char *proof_type = ny_proof_type_from_expr(proposition);
  if (!proof_type)
    return false;
  const char *body = proof_type;
  size_t blen = strlen(body);
  if (blen > 7 && strncmp(body, "proof<", 6) == 0 && body[blen - 1] == '>') {
    body += 6;
    blen -= 7;
  }
  char *prop = ny_strndup(body, blen);
  free(proof_type);
  if (!prop)
    return false;
  ny_pol_names_t names;
  ny_pol_collect_names(prop, &names);
  char ranges[NY_PROOF_OBLIGATION_MAX];
  bool ok =
      ny_pol_ranges_text(cg, scopes, depth, &names, ranges, sizeof(ranges));
  if (ok) {
    char policy_field[80];
    ny_pol_policy_field(&policy, contract_safe, policy_field,
                        sizeof(policy_field));
    ok = ny_pol_obligation_serialize(
        prop, ranges, policy_field, NY_PROOF_POLICY_MAX_VARS,
        NY_PROOF_POLICY_MAX_CONS, NY_PROOF_POLICY_MAX_DEPTH, out, out_cap);
  }
  free(prop);
  return ok;
}

static bool ny_pol_mark(ny_proof_policy_result_t *out, int verdict,
                        const char *tactic, const char *backend,
                        const char *cex, const char *explanation) {
  if (!out || !out->obligation[0])
    return false;
  out->verdict = verdict;
  snprintf(out->tactic, sizeof(out->tactic), "%s", tactic ? tactic : "");
  snprintf(out->backend, sizeof(out->backend), "%s", backend ? backend : "");
  snprintf(out->cex, sizeof(out->cex), "%s", cex ? cex : "");
  snprintf(out->explanation, sizeof(out->explanation), "%s",
           explanation ? explanation : "");
  out->has_certificate = false;
  if (!ny_pol_certificate_serialize(
          out->obligation,
          verdict == NY_PROOF_VERDICT_PROVED
              ? "proved"
              : (verdict == NY_PROOF_VERDICT_REFUTED ? "refuted" : "unknown"),
          out->tactic, out->backend, out->cex, out->explanation,
          out->certificate, sizeof(out->certificate)))
    return false;
  char reason[NY_PROOF_POLICY_EXPLAIN_MAX];
  out->has_certificate = ny_proof_policy_certificate_check(
      out->obligation, out->certificate, reason, sizeof(reason));
  if (!out->has_certificate && verdict == NY_PROOF_VERDICT_PROVED) {
    /*
     * The independent checker refused to confirm a claimed proof.  This is
     * a kernel/checker disagreement: fail closed to unknown with the
     * checker's reason attached.
     */
    out->verdict = NY_PROOF_VERDICT_UNKNOWN;
    snprintf(out->explanation, sizeof(out->explanation),
             "certificate rejected by independent checker: %s",
             reason[0] ? reason : "no reason given");
    out->cex[0] = '\0';
    if (!ny_pol_certificate_serialize(
            out->obligation, "unknown", out->tactic, out->backend, "",
            out->explanation, out->certificate, sizeof(out->certificate)))
      return false;
    out->has_certificate = ny_proof_policy_certificate_check(
        out->obligation, out->certificate, reason, sizeof(reason));
  }
  return out->has_certificate;
}

/*
 * Caller-driven fold tactic: the caller evaluated the proposition exactly
 * (LLVM constant folding or the NYIR proof evaluator) and reports success
 * here so the verdict still travels through the versioned certificate and
 * the independent checker.
 */
bool ny_proof_policy_mark_fold(ny_proof_policy_result_t *out) {
  if (!out)
    return false;
  return ny_pol_mark(out, NY_PROOF_VERDICT_PROVED, "fold", "fold", NULL,
                     "proved by exact compile-time evaluation");
}

bool ny_proof_policy_decide(codegen_t *cg, scope *scopes, size_t depth,
                            expr_t *proposition, const char *policy_text,
                            bool contract_safe, ny_proof_policy_result_t *out) {
  if (!out || !proposition)
    return false;
  memset(out, 0, sizeof(*out));
  ny_pol_policy_t policy;
  ny_pol_policy_parse(policy_text, &policy);
  if (policy.malformed) {
    snprintf(out->explanation, sizeof(out->explanation), "%s", policy.reason);
    return false;
  }
  if (!ny_proof_policy_obligation_build(
          cg, scopes, depth, proposition, policy_text, contract_safe,
          out->obligation, sizeof(out->obligation))) {
    snprintf(out->explanation, sizeof(out->explanation),
             "obligation could not be built");
    return false;
  }
  /*
   * Tactic order mirrors lib/proof.ny:solve (fold → range → solver).
   * Fold is caller-driven (ny_proof_policy_mark_fold) before this funnel;
   * cc is the compiler-only lemma step, then range, solver kernels, and
   * the bounded interval candidate. policy/check.c re-derives every verdict.
   * 1. lemma instantiation (by_cc)
   */
  if (policy.tactics & NY_POL_TACTIC_CC) {
    if (ny_proof_tactic_lemma_instantiation(cg, scopes, depth, proposition)) {
      if (ny_pol_mark(out, NY_PROOF_VERDICT_PROVED, "cc", "lemma", NULL,
                      "proved by lemma instantiation"))
        return true;
      return false;
    }
  }

  /*
   * 2. known integer ranges (by_range)
   */
  if (policy.tactics & NY_POL_TACTIC_RANGE) {
    const char *trivial_reason = NULL;
    if (ny_proof_tactic_range_truth(cg, scopes, depth, proposition,
                                    &trivial_reason)) {
      if (ny_pol_mark(out, NY_PROOF_VERDICT_PROVED, "range", "range", NULL,
                      trivial_reason && trivial_reason[0]
                          ? trivial_reason
                          : "proved by known integer ranges"))
        return true;
      return false;
    }
  }

  /*
   * 3. solver kernels (by_solver / by_z3 / by_range / default)
   */
  if (policy.tactics & NY_POL_TACTIC_SOLVER) {
    char cex[NY_PROOF_POLICY_CEX_MAX] = {0};
    bool proved;
    const char *saved = cg ? cg->proof_solver : NULL;
    if (cg && policy.mode == NY_POL_MODE_Z3)
      cg->proof_solver = "z3";
    else if (cg && policy.mode == NY_POL_MODE_PRESBURGER)
      cg->proof_solver = "presburger";
    else if (cg && policy.mode == NY_POL_MODE_AUTO)
      cg->proof_solver = "auto";
    if (contract_safe)
      proved = ny_proof_try_contract_solver(cg, scopes, depth, proposition, cex,
                                            sizeof(cex));
    else
      proved =
          ny_proof_try_solver(cg, scopes, depth, proposition, cex, sizeof(cex));
    if (cg)
      cg->proof_solver = saved;
    if (proved) {
      const char *backend = policy.mode == NY_POL_MODE_Z3 ? "z3"
                            : policy.mode == NY_POL_MODE_PRESBURGER
                                ? "presburger"
                            : policy.mode == NY_POL_MODE_AUTO
                                ? "auto"
                                : (saved && saved[0] ? saved : "auto");
      if (ny_pol_mark(out, NY_PROOF_VERDICT_PROVED, "solver", backend, NULL,
                      "proved by solver: negation unsatisfiable"))
        return true;
      return false;
    }
    if (cex[0]) {
      const char *backend = policy.mode == NY_POL_MODE_Z3 ? "z3"
                            : policy.mode == NY_POL_MODE_PRESBURGER
                                ? "presburger"
                            : policy.mode == NY_POL_MODE_AUTO
                                ? "auto"
                                : (saved && saved[0] ? saved : "auto");
      if (ny_pol_mark(out, NY_PROOF_VERDICT_REFUTED, "solver", backend, cex,
                      "solver counterexample candidate"))
        return true;
    }
    /*
     * Unknown (or refuted without a verifiable counterexample) falls
     * through: unknown stays distinct from false at every stage.
     */
  }

  if ((policy.tactics & NY_POL_TACTIC_RANGE) &&
      (strstr(out->obligation, "binary:*(") != NULL ||
       strstr(out->obligation, "binary:/(") != NULL)) {
    char candidate[NY_PROOF_CERTIFICATE_MAX];
    char reason[NY_PROOF_POLICY_EXPLAIN_MAX];
    if (ny_pol_certificate_serialize(
            out->obligation, "proved", "interval", "checker", NULL,
            "candidate bounded interval proof", candidate, sizeof(candidate)) &&
        ny_proof_policy_certificate_check(out->obligation, candidate, reason,
                                          sizeof(reason))) {
      if (ny_pol_mark(out, NY_PROOF_VERDICT_PROVED, "interval", "checker", NULL,
                      "proved by bounded independent interval checker"))
        return true;
      return false;
    }
  }

  ny_pol_mark(out, NY_PROOF_VERDICT_UNKNOWN, "policy", "", NULL,
              "no tactic named by the policy could decide the obligation");
  return true;
}

/*
 * Sweeps a grid of propositions and range sets and cross-examines the two
 * independent implementations:
 *
 *   - whenever the solver kernels prove a proposition, the checker must
 *     confirm it (checker is never weaker on the shared fragment);
 *   - whenever the checker proves one, the kernels must not have refuted
 *     it (a proved certificate with a verified counterexample is a bug).
 *
 * Any disagreement prints a witness and fails.  Unknown never counts as
 * false here: undecided outcomes agree by construction and are counted.
 */
typedef struct {
  const char *prop;
  const char *ranges;
  bool expect_proved; /* known ground truth, for end-to-end pinning */
  bool decided;       /* ground truth is known at all */
} ny_pol_diff_case_t;

static const ny_pol_diff_case_t ny_pol_diff_cases[] = {
    {"binary:>(name:a,int:0)", "a=1..5", true, true},
    {"binary:>(name:a,int:0)", "a=0..5", false, false},
    {"binary:>=(name:a,int:0)", "a=0..9", true, true},
    {"binary:<(name:a,int:10)", "a=0..9", true, true},
    {"binary:<=(name:a,int:9)", "a=0..9", true, true},
    {"binary:&&(binary:>=(name:a,int:0),binary:<=(name:a,int:9))", "a=0..9",
     true, true},
    {"binary:||(binary:>(name:a,int:0),binary:>(name:b,int:0))",
     "a=0..1;b=0..1", false, false},
    {"binary:||(binary:>(name:a,int:0),binary:>(name:b,int:0))",
     "a=1..2;b=1..2", true, true},
    {"binary:==(name:a,name:b)", "a=5..5;b=5..5", true, true},
    {"binary:!=(name:a,name:b)", "a=0..2;b=5..6", true, true},
    {"binary:>=(binary:*(int:2,name:a),int:0)", "a=0..3", true, true},
    {"binary:>(binary:+(name:a,name:b),int:0)", "a=0..0;b=0..0", false, false},
    {"binary:>=(binary:+(name:a,name:b),int:0)", "a=0..3;b=0..3", true, true},
    {"binary:<(binary:*(name:a,name:b),int:0)", "a=0..3;b=0..3", false, true},
    {"binary:<=(binary:*(name:a,name:a),int:81)", "a=-9..9", true, true},
    {"binary:>=(binary:*(name:a,name:a),int:0)", "a=-9..9", true, true},
    {"binary:<=(binary:*(name:a,name:a),int:80)", "a=-9..9", false, false},
    {"binary:<=(binary:*(name:a,name:b),int:6)", "a=0..2;b=1..3", true, true},
    {"binary:<=(binary:*(name:a,name:b),int:5)", "a=0..2;b=1..3", false, true},
    {"binary:<=(binary:/(name:a,int:2),int:5)", "a=0..10", true, true},
    {"binary:>=(binary:/(name:a,int:2),int:-5)", "a=-10..-1", true, true},
    {"binary:==(binary:/(name:a,int:2),int:-4611686018427387904)",
     "a=-9223372036854775808..-9223372036854775808", true, true},
    {"binary:<=(binary:/(name:a,int:2),int:5)", "a=12..12", false, true},
    {"binary:<=(binary:/(name:a,name:b),int:10)", "a=0..10;b=1..5", true, true},
    {"binary:>=(binary:/(name:a,name:b),int:-10)", "a=-10..-1;b=1..5", true,
     true},
    {"binary:<=(binary:/(name:a,name:b),int:5)", "a=12..12;b=1..2", false,
     true},
    {"binary:<=(binary:/(name:a,int:0),int:2)", "a=0..3", false, false},
    {"binary:>(binary:/(name:a,int:-1),int:1)", "a=0..3", false, false},
    {"binary:||(binary:||(binary:<(name:a,int:-9),binary:>(name:a,int:9)),"
     "binary:<=(binary:*(name:a,name:a),int:81))",
     "", true, true},
    {"binary:||(binary:==(binary:+(name:a,int:1),int:-9223372036854775808),"
     "binary:==(binary:*(name:a,name:a),int:2))",
     "", false, true},
};

int ny_proof_policy_differential_selftest(void) {
  ny_pol_policy_t parsed;
  ny_pol_policy_parse("by_range by_z3", &parsed);
  if (parsed.malformed || parsed.mode != NY_POL_MODE_AUTO ||
      (parsed.tactics & (NY_POL_TACTIC_RANGE | NY_POL_TACTIC_SOLVER)) !=
          (NY_POL_TACTIC_RANGE | NY_POL_TACTIC_SOLVER)) {
    fprintf(stderr, "proof policy parser selftest: combined modes failed\n");
    return 1;
  }
  ny_pol_policy_parse("by_z3 by_range", &parsed);
  if (parsed.malformed || parsed.mode != NY_POL_MODE_AUTO) {
    fprintf(stderr,
            "proof policy parser selftest: mode order changed result\n");
    return 1;
  }

  char escaped[32], decoded[32], truncated[4];
  ny_proof_policy_escape_field("pipe|percent%", escaped, sizeof(escaped));
  if (strcmp(escaped, "pipe%7cpercent%25") != 0) {
    fprintf(stderr,
            "proof policy escaping selftest: canonical encoding failed\n");
    return 1;
  }
  ny_proof_policy_unescape_field(escaped, decoded, sizeof(decoded));
  if (strcmp(decoded, "pipe|percent%") != 0) {
    fprintf(stderr, "proof policy escaping selftest: round trip failed\n");
    return 1;
  }
  ny_proof_policy_unescape_field("pipe%7Cpercent%25", decoded, sizeof(decoded));
  if (strcmp(decoded, "pipe|percent%") != 0) {
    fprintf(stderr, "proof policy escaping selftest: uppercase hex rejected\n");
    return 1;
  }
  ny_proof_policy_escape_field("pipe|percent%", truncated, sizeof(truncated));
  if (truncated[0] != '\0') {
    fprintf(stderr, "proof policy escaping selftest: truncation was partial\n");
    return 1;
  }

  size_t cases = sizeof(ny_pol_diff_cases) / sizeof(ny_pol_diff_cases[0]);
  size_t proved_both = 0, undecided = 0;
  for (size_t i = 0; i < cases; ++i) {
    const ny_pol_diff_case_t *tc = &ny_pol_diff_cases[i];
    char esc_prop[NY_PROOF_OBLIGATION_MAX];
    char obligation[NY_PROOF_OBLIGATION_MAX];
    ny_proof_policy_escape_field(tc->prop, esc_prop, sizeof(esc_prop));
    snprintf(obligation, sizeof(obligation), "%s|%s|%s|%s|%s",
             NY_PROOF_OBLIGATION_FORMAT, esc_prop, tc->ranges ? tc->ranges : "",
             "cc,range,solver,fold", "v8,c32,d64");
    char digest_hex[17];
    ny_proof_policy_digest_hex(ny_proof_policy_digest_text(obligation),
                               digest_hex);
    /*
     * Solver verdict.
     */
    char cex[128];
    int kernel =
        ny_proof_solver_check_canonical(tc->prop, tc->ranges ? tc->ranges : "",
                                        "auto", 8, 32, 64, cex, sizeof(cex));
    /*
     * Checker decision, inferred from which certificates it accepts.
     */
    char cert[NY_PROOF_CERTIFICATE_MAX];
    char reason[NY_PROOF_POLICY_EXPLAIN_MAX];
    snprintf(cert, sizeof(cert), "%s|%s|proved|solver|selftest||",
             NY_PROOF_CERTIFICATE_FORMAT, digest_hex);
    bool checker_proved = ny_proof_policy_certificate_check(
        obligation, cert, reason, sizeof(reason));
    snprintf(cert, sizeof(cert), "%s|%s|unknown|solver|selftest||probe",
             NY_PROOF_CERTIFICATE_FORMAT, digest_hex);
    bool checker_undecided = ny_proof_policy_certificate_check(
        obligation, cert, reason, sizeof(reason));
    if (checker_proved && checker_undecided) {
      fprintf(stderr,
              "proof policy differential: checker accepted both "
              "proved and unknown for '%s' '%s'\n",
              tc->prop, tc->ranges ? tc->ranges : "");
      return 1;
    }
    if (kernel == NY_PROOF_VERDICT_PROVED && !checker_proved) {
      fprintf(stderr,
              "proof policy differential: kernels proved '%s' under "
              "'%s' but the checker would not confirm\n",
              tc->prop, tc->ranges ? tc->ranges : "");
      return 1;
    }
    if (kernel == NY_PROOF_VERDICT_REFUTED && checker_proved) {
      fprintf(stderr,
              "proof policy differential: kernels refuted '%s' under "
              "'%s' (cex %s) but the checker proved it\n",
              tc->prop, tc->ranges ? tc->ranges : "", cex);
      return 1;
    }
    if (tc->decided && tc->expect_proved && !checker_proved) {
      fprintf(stderr,
              "proof policy differential: expected proved for '%s' "
              "under '%s', checker undecided\n",
              tc->prop, tc->ranges ? tc->ranges : "");
      return 1;
    }
    if (tc->decided && !tc->expect_proved && checker_proved) {
      fprintf(stderr,
              "proof policy differential: checker proved a false "
              "proposition '%s' under '%s'\n",
              tc->prop, tc->ranges ? tc->ranges : "");
      return 1;
    }
    if (checker_proved && kernel == NY_PROOF_VERDICT_PROVED)
      proved_both++;
    else
      undecided++;
  }
  printf("proof policy differential selftest: %zu case(s), %zu proved by "
         "both, %zu undecided, no disagreements\n",
         cases, proved_both, undecided);
  return 0;
}
