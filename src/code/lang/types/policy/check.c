/*
 * Independent bounded certificate checker.
 *
 * This file is the trust boundary of the proof policy.  It shares no code
 * with the solver kernels (typing/proof.c), the canonicalizer
 * (parse/proof.c), or the policy funnel (policy.c): it carries its own
 * obligation/certificate parsers, its own canonical-proposition parser,
 * its own linear constraint extraction, its own Fourier-Motzkin
 * elimination, and its own exact evaluator.  A certificate is accepted
 * only when this checker can re-derive the claimed verdict itself:
 *
 *   proved   - the checker's own bounded decision proves the proposition
 *              under the obligation's ranges: every branch of the negated
 *              proposition is shown infeasible by its own elimination;
 *   refuted  - the certificate supplies a counterexample whose bindings
 *              lie inside the obligation's ranges and whose substitution
 *              makes the proposition evaluate false, and the checker's own
 *              decision does not prove the proposition;
 *   unknown  - accepted only with a non-empty explanation and only while
 *              the checker's own bounded decision is also non-decisive:
 *              unknown never stands in for a decidable verdict, and it is
 *              never read as false.
 *
 * Every stage is bounded: field counts, name counts, node counts, depth,
 * constraint counts, elimination rounds, and evaluation steps all carry
 * hard budgets.  Overflow, malformed input, or an exhausted budget fails
 * closed (reject), never open.
 */
#include "code/lang/types/policy.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Checker-local FNV-1a (deliberately a private re-implementation).
 */
static uint64_t pol_digest(const char *text) {
  uint64_t h = UINT64_C(1469598103934665603);
  if (!text)
    return h;
  for (const char *p = text; *p; ++p) {
    h ^= (uint64_t)(unsigned char)*p;
    h *= UINT64_C(1099511628211);
  }
  return h;
}

/*
 * Hard checker caps (independent of, and at least as generous as, the
 * solver-side bounds so checker re-derivation is never budget-starved).
 */
#define POL_MAX_FIELDS 8
#define POL_MAX_VARS 16
#define POL_MAX_CONS 128
#define POL_MAX_DEPTH 96
#define POL_MAX_INTERVAL_SPLIT_DEPTH 8
#define POL_MAX_INTERVAL_SPLITS 255
#define POL_MAX_NODES 1024
#define POL_MAX_NAME 64
#define POL_MAX_TEXT 4096

typedef struct {
  char reason[NY_PROOF_POLICY_EXPLAIN_MAX];
  bool failed;
} pol_status_t;

static void pol_fail(pol_status_t *st, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void pol_fail(pol_status_t *st, const char *fmt, ...) {
  if (!st || st->failed)
    return;
  st->failed = true;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(st->reason, sizeof(st->reason), fmt, ap);
  va_end(ap);
}

typedef struct {
  const char *fields[POL_MAX_FIELDS];
  size_t lens[POL_MAX_FIELDS];
  int count;
} pol_fields_t;

static bool pol_split(const char *text, pol_fields_t *out, int expect,
                      pol_status_t *st) {
  memset(out, 0, sizeof(*out));
  if (!text || strlen(text) > POL_MAX_TEXT) {
    pol_fail(st, "text missing or over %d bytes", POL_MAX_TEXT);
    return false;
  }
  const char *p = text;
  while (1) {
    if (out->count >= POL_MAX_FIELDS) {
      pol_fail(st, "too many '|' fields");
      return false;
    }
    out->fields[out->count] = p;
    const char *bar = strchr(p, '|');
    if (bar) {
      out->lens[out->count] = (size_t)(bar - p);
      p = bar + 1;
    } else {
      out->lens[out->count] = strlen(p);
      p = NULL;
    }
    out->count++;
    if (!p)
      break;
  }
  if (out->count != expect) {
    pol_fail(st, "expected %d fields, found %d", expect, out->count);
    return false;
  }
  for (int i = 0; i < out->count; ++i) {
    for (size_t j = 0; j < out->lens[i]; ++j) {
      char c = out->fields[i][j];
      if (c == '\n' || c == '\r' || (unsigned char)c < 0x20) {
        pol_fail(st, "control byte in field %d", i);
        return false;
      }
    }
  }
  return true;
}

static bool pol_field_eq(const pol_fields_t *f, int i, const char *lit) {
  return i < f->count && f->lens[i] == strlen(lit) &&
         strncmp(f->fields[i], lit, f->lens[i]) == 0;
}

enum {
  POL_OP_AND,
  POL_OP_OR,
  POL_OP_LT,
  POL_OP_LE,
  POL_OP_GT,
  POL_OP_GE,
  POL_OP_EQ,
  POL_OP_NE,
  POL_OP_ADD,
  POL_OP_SUB,
  POL_OP_MUL,
  POL_OP_DIV,
  POL_OP_MOD,
  POL_OP_BAND,
  POL_OP_BOR,
  POL_OP_BXOR,
  POL_OP_SHL,
  POL_OP_SHR,
};

enum { POL_NODE_CONST, POL_NODE_NAME, POL_NODE_BIN, POL_NODE_NEG };

typedef struct {
  int kind;
  bool bval; /* NEG only: true = logical not, false = arithmetic minus */
  int64_t ival;
  int name;
  int op;
  int left, right;
} pol_node_t;

typedef struct {
  pol_node_t nodes[POL_MAX_NODES];
  int count;
  char names[POL_MAX_VARS][POL_MAX_NAME];
  int name_count;
  int max_depth;
} pol_parser_t;

static int pol_name_intern(pol_parser_t *ps, const char *at, size_t len,
                           pol_status_t *st) {
  if (len == 0 || len >= POL_MAX_NAME) {
    pol_fail(st, "proposition name length out of bounds");
    return -1;
  }
  for (int i = 0; i < ps->name_count; ++i) {
    if (strlen(ps->names[i]) == len && strncmp(ps->names[i], at, len) == 0)
      return i;
  }
  if (ps->name_count >= POL_MAX_VARS) {
    pol_fail(st, "more than %d distinct names", POL_MAX_VARS);
    return -1;
  }
  memcpy(ps->names[ps->name_count], at, len);
  ps->names[ps->name_count][len] = '\0';
  return ps->name_count++;
}

static bool pol_parse_decimal(const char *at, size_t len, int64_t *out) {
  if (len == 0 || len > 20)
    return false;
  size_t i = 0;
  bool neg = false;
  if (at[0] == '-') {
    neg = true;
    i = 1;
    if (len == 1)
      return false;
  }
  uint64_t v = 0;
  uint64_t limit = (uint64_t)INT64_MAX + (neg ? 1u : 0u);
  for (; i < len; ++i) {
    if (at[i] < '0' || at[i] > '9')
      return false;
    unsigned digit = (unsigned)(at[i] - '0');
    if (v > (limit - digit) / 10)
      return false;
    v = v * 10 + digit;
  }
  *out = neg ? (v == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)v)
             : (int64_t)v;
  return true;
}

static bool pol_parse_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static int pol_new_node(pol_parser_t *ps, pol_status_t *st) {
  if (ps->count >= POL_MAX_NODES) {
    pol_fail(st, "proposition node budget exhausted");
    return -1;
  }
  return ps->count++;
}

static int pol_parse_node(pol_parser_t *ps, const char *s, size_t len,
                          int depth, pol_status_t *st) {
  if (st->failed)
    return -1;
  if (depth > ps->max_depth) {
    pol_fail(st, "proposition depth over %d", ps->max_depth);
    return -1;
  }
  /*
   * leaf: name:IDENT
   */
  if (len > 5 && strncmp(s, "name:", 5) == 0) {
    for (size_t i = 5; i < len; ++i) {
      if (!pol_parse_name_char(s[i])) {
        pol_fail(st, "bad name character in proposition");
        return -1;
      }
    }
    int idx = pol_new_node(ps, st);
    if (idx < 0)
      return -1;
    pol_node_t *n = &ps->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->kind = POL_NODE_NAME;
    n->name = pol_name_intern(ps, s + 5, len - 5, st);
    if (n->name < 0)
      return -1;
    return idx;
  }
  /*
   * leaf: int:N / bool:true / bool:false
   */
  if (len > 4 && strncmp(s, "int:", 4) == 0) {
    int64_t v = 0;
    if (!pol_parse_decimal(s + 4, len - 4, &v)) {
      pol_fail(st, "malformed int literal in proposition");
      return -1;
    }
    int idx = pol_new_node(ps, st);
    if (idx < 0)
      return -1;
    pol_node_t *n = &ps->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->kind = POL_NODE_CONST;
    n->bval = v != 0;
    n->ival = v;
    return idx;
  }
  if (len == 9 && strncmp(s, "bool:true", 9) == 0) {
    int idx = pol_new_node(ps, st);
    if (idx < 0)
      return -1;
    pol_node_t *n = &ps->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->kind = POL_NODE_CONST;
    n->bval = true;
    n->ival = 1;
    return idx;
  }
  if (len == 10 && strncmp(s, "bool:false", 10) == 0) {
    int idx = pol_new_node(ps, st);
    if (idx < 0)
      return -1;
    pol_node_t *n = &ps->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->kind = POL_NODE_CONST;
    n->bval = false;
    n->ival = 0;
    return idx;
  }
  /*
   * unary:!(X) - logical negation
   */
  if (len > 9 && strncmp(s, "unary:!(", 8) == 0 && s[len - 1] == ')') {
    int inner = pol_parse_node(ps, s + 8, len - 9, depth + 1, st);
    if (inner < 0)
      return -1;
    int idx = pol_new_node(ps, st);
    if (idx < 0)
      return -1;
    pol_node_t *n = &ps->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->kind = POL_NODE_NEG;
    n->bval = true;
    n->left = inner;
    return idx;
  }
  /*
   * unary:-(X) - arithmetic negation
   */
  if (len > 9 && strncmp(s, "unary:-(", 8) == 0 && s[len - 1] == ')') {
    int inner = pol_parse_node(ps, s + 8, len - 9, depth + 1, st);
    if (inner < 0)
      return -1;
    int idx = pol_new_node(ps, st);
    if (idx < 0)
      return -1;
    pol_node_t *n = &ps->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->kind = POL_NODE_NEG;
    n->bval = false;
    n->left = inner;
    return idx;
  }
  /*
   * binary:OP(X,Y)
   */
  if (len > 8 && strncmp(s, "binary:", 7) == 0) {
    const char *open = memchr(s + 7, '(', len - 7);
    if (!open) {
      pol_fail(st, "malformed binary node in proposition");
      return -1;
    }
    size_t oplen = (size_t)(open - (s + 7));
    int op = -1;
#define POL_OP_LIT(lit, val)                                                   \
  if (oplen == strlen(lit) && strncmp(s + 7, lit, oplen) == 0)                 \
    op = val;
    POL_OP_LIT("&&", POL_OP_AND)
    POL_OP_LIT("||", POL_OP_OR)
    POL_OP_LIT("<", POL_OP_LT)
    POL_OP_LIT("<=", POL_OP_LE)
    POL_OP_LIT(">", POL_OP_GT)
    POL_OP_LIT(">=", POL_OP_GE)
    POL_OP_LIT("==", POL_OP_EQ)
    POL_OP_LIT("!=", POL_OP_NE)
    POL_OP_LIT("+", POL_OP_ADD)
    POL_OP_LIT("-", POL_OP_SUB)
    POL_OP_LIT("*", POL_OP_MUL)
    POL_OP_LIT("/", POL_OP_DIV)
    POL_OP_LIT("%", POL_OP_MOD)
    POL_OP_LIT("&", POL_OP_BAND)
    POL_OP_LIT("|", POL_OP_BOR)
    POL_OP_LIT("^", POL_OP_BXOR)
    POL_OP_LIT("<<", POL_OP_SHL)
    POL_OP_LIT(">>", POL_OP_SHR)
#undef POL_OP_LIT
    if (op < 0) {
      pol_fail(st, "unsupported operator in proposition");
      return -1;
    }
    /*
     * split X,Y at the top-level comma inside the parens
     */
    if (s[len - 1] != ')') {
      pol_fail(st, "binary node lacks a closing ')'");
      return -1;
    }
    const char *body = open + 1;
    size_t body_len = len - (size_t)(body - s) - 1; /* drop trailing ')' */
    if (body_len == 0) {
      pol_fail(st, "empty binary node body");
      return -1;
    }
    int depth_paren = 0;
    const char *comma = NULL;
    for (size_t i = 0; i < body_len; ++i) {
      char c = body[i];
      if (c == '(')
        depth_paren++;
      else if (c == ')') {
        if (depth_paren == 0) {
          pol_fail(st, "unbalanced ')' in proposition");
          return -1;
        }
        depth_paren--;
      } else if (c == ',' && depth_paren == 0 && !comma) {
        comma = body + i;
      }
    }
    if (depth_paren != 0 || !comma) {
      pol_fail(st, "binary node lacks a top-level ',' or is unbalanced");
      return -1;
    }
    int l = pol_parse_node(ps, body, (size_t)(comma - body), depth + 1, st);
    if (l < 0)
      return -1;
    int r = pol_parse_node(ps, comma + 1, body_len - (size_t)(comma - body) - 1,
                           depth + 1, st);
    if (r < 0)
      return -1;
    int idx = pol_new_node(ps, st);
    if (idx < 0)
      return -1;
    pol_node_t *n = &ps->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->kind = POL_NODE_BIN;
    n->op = op;
    n->left = l;
    n->right = r;
    return idx;
  }
  pol_fail(st, "unsupported proposition token (float, str, or foreign form)");
  return -1;
}

typedef struct {
  char name[POL_MAX_NAME];
  int64_t lo, hi;
} pol_range_t;

typedef struct {
  char prop[POL_MAX_TEXT];
  char ranges_text[POL_MAX_TEXT];
  char policy_text[POL_MAX_TEXT];
  pol_range_t ranges[POL_MAX_VARS];
  int nrange;
  bool policy_cc, policy_range, policy_fold;
  bool policy_solver, policy_z3, policy_presburger, policy_contract;
  int max_vars, max_cons, max_depth;
} pol_obligation_t;

typedef struct {
  char digest_hex[17];
  int verdict; /* NY_PROOF_VERDICT_* */
  char tactic[32];
  char backend[32];
  char cex[POL_MAX_TEXT];
  char explanation[POL_MAX_TEXT];
} pol_certificate_t;

static bool pol_copy_field(const pol_fields_t *f, int i, char *out, size_t cap,
                           pol_status_t *st, const char *what) {
  if (i >= f->count || f->lens[i] >= cap) {
    pol_fail(st, "%s field missing or oversized", what);
    return false;
  }
  memcpy(out, f->fields[i], f->lens[i]);
  out[f->lens[i]] = '\0';
  return true;
}

static bool pol_parse_obligation(const char *text, pol_obligation_t *ob,
                                 pol_status_t *st) {
  memset(ob, 0, sizeof(*ob));
  pol_fields_t f;
  if (!pol_split(text, &f, 5, st))
    return false;
  if (!pol_field_eq(&f, 0, NY_PROOF_OBLIGATION_FORMAT)) {
    pol_fail(st, "obligation format mismatch");
    return false;
  }
  {
    char escaped[POL_MAX_TEXT];
    if (!pol_copy_field(&f, 1, escaped, sizeof(escaped), st, "proposition"))
      return false;
    ny_proof_policy_unescape_field(escaped, ob->prop, sizeof(ob->prop));
  }
  {
    char ranges[POL_MAX_TEXT];
    if (!pol_copy_field(&f, 2, ranges, sizeof(ranges), st, "ranges"))
      return false;
    ny_proof_policy_unescape_field(ranges, ob->ranges_text,
                                   sizeof(ob->ranges_text));
    const char *p = ob->ranges_text;
    while (*p) {
      if (ob->nrange >= POL_MAX_VARS) {
        pol_fail(st, "too many range entries");
        return false;
      }
      pol_range_t *r = &ob->ranges[ob->nrange];
      size_t i = 0;
      while (*p && *p != '=' && *p != ';') {
        if (i + 1 >= POL_MAX_NAME || !pol_parse_name_char(*p)) {
          pol_fail(st, "malformed range name");
          return false;
        }
        r->name[i++] = *p++;
      }
      r->name[i] = '\0';
      if (i == 0 || *p != '=') {
        pol_fail(st, "range entry lacks '='");
        return false;
      }
      ++p;
      const char *dash = p;
      while (*p && *p != '.' && *p != ';')
        ++p;
      if (*p != '.' || p[1] != '.') {
        pol_fail(st, "range entry lacks '..'");
        return false;
      }
      if (!pol_parse_decimal(dash, (size_t)(p - dash), &r->lo)) {
        pol_fail(st, "malformed range low bound");
        return false;
      }
      p += 2;
      const char *hi = p;
      while (*p && *p != ';')
        ++p;
      if (!pol_parse_decimal(hi, (size_t)(p - hi), &r->hi)) {
        pol_fail(st, "malformed range high bound");
        return false;
      }
      if (r->lo > r->hi) {
        pol_fail(st, "range low exceeds high");
        return false;
      }
      ob->nrange++;
      if (*p == ';')
        ++p;
    }
  }
  {
    char policy[POL_MAX_TEXT];
    if (!pol_copy_field(&f, 3, policy, sizeof(policy), st, "policy"))
      return false;
    ny_proof_policy_unescape_field(policy, ob->policy_text,
                                   sizeof(ob->policy_text));
    memmove(policy, ob->policy_text, strlen(ob->policy_text) + 1);
    if (strncmp(policy, "contract+", 9) == 0) {
      ob->policy_contract = true;
      memmove(policy, policy + 9, strlen(policy + 9) + 1);
    }
    if (*policy) {
      char *tok = policy;
      while (tok && *tok) {
        char *comma = strchr(tok, ',');
        if (comma)
          *comma = '\0';
        if (strcmp(tok, "cc") == 0)
          ob->policy_cc = true;
        else if (strcmp(tok, "range") == 0)
          ob->policy_range = true;
        else if (strcmp(tok, "fold") == 0)
          ob->policy_fold = true;
        else if (strcmp(tok, "solver") == 0)
          ob->policy_solver = true;
        else if (strcmp(tok, "z3") == 0)
          ob->policy_z3 = true;
        else if (strcmp(tok, "presburger") == 0)
          ob->policy_presburger = true;
        else if (strcmp(tok, "kernels") == 0)
          ob->policy_solver = true;
        else {
          pol_fail(st, "unknown policy token '%s'", tok);
          return false;
        }
        tok = comma ? comma + 1 : NULL;
      }
    }
  }
  {
    char budgets[POL_MAX_TEXT];
    if (!pol_copy_field(&f, 4, budgets, sizeof(budgets), st, "budgets"))
      return false;
    int max_vars = 0, max_cons = 0, max_depth = 0;
    if (sscanf(budgets, "v%d,c%d,d%d", &max_vars, &max_cons, &max_depth) != 3) {
      pol_fail(st, "malformed budget field");
      return false;
    }
    if (max_vars < 1 || max_vars > POL_MAX_VARS || max_cons < 1 ||
        max_cons > POL_MAX_CONS || max_depth < 1 || max_depth > POL_MAX_DEPTH) {
      pol_fail(st, "budgets outside checker caps");
      return false;
    }
    ob->max_vars = max_vars;
    ob->max_cons = max_cons;
    ob->max_depth = max_depth;
  }
  return true;
}

static bool pol_parse_certificate(const char *text, pol_certificate_t *cert,
                                  pol_status_t *st) {
  memset(cert, 0, sizeof(*cert));
  pol_fields_t f;
  if (!pol_split(text, &f, 7, st))
    return false;
  if (!pol_field_eq(&f, 0, NY_PROOF_CERTIFICATE_FORMAT)) {
    pol_fail(st, "certificate format mismatch");
    return false;
  }
  if (!pol_copy_field(&f, 1, cert->digest_hex, sizeof(cert->digest_hex), st,
                      "digest"))
    return false;
  if (strlen(cert->digest_hex) != 16) {
    pol_fail(st, "digest must be 16 hex digits");
    return false;
  }
  for (int i = 0; i < 16; ++i) {
    char c = cert->digest_hex[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      pol_fail(st, "digest must be lowercase hex");
      return false;
    }
  }
  if (pol_field_eq(&f, 2, "proved"))
    cert->verdict = NY_PROOF_VERDICT_PROVED;
  else if (pol_field_eq(&f, 2, "refuted"))
    cert->verdict = NY_PROOF_VERDICT_REFUTED;
  else if (pol_field_eq(&f, 2, "unknown"))
    cert->verdict = NY_PROOF_VERDICT_UNKNOWN;
  else {
    pol_fail(st, "verdict must be proved|refuted|unknown");
    return false;
  }
  {
    char buf[POL_MAX_TEXT];
    if (!pol_copy_field(&f, 3, buf, sizeof(buf), st, "tactic"))
      return false;
    ny_proof_policy_unescape_field(buf, cert->tactic, sizeof(cert->tactic));
    if (!pol_copy_field(&f, 4, buf, sizeof(buf), st, "backend"))
      return false;
    ny_proof_policy_unescape_field(buf, cert->backend, sizeof(cert->backend));
    if (!pol_copy_field(&f, 5, buf, sizeof(buf), st, "counterexample"))
      return false;
    ny_proof_policy_unescape_field(buf, cert->cex, sizeof(cert->cex));
    if (!pol_copy_field(&f, 6, buf, sizeof(buf), st, "explanation"))
      return false;
    ny_proof_policy_unescape_field(buf, cert->explanation,
                                   sizeof(cert->explanation));
  }
  return true;
}

typedef struct {
  int64_t coeff[POL_MAX_VARS];
  __int128 cst; /* Σ coeff·x + cst <= 0; endpoint differences need 65 bits. */
  bool interval_safe;
  int interval_left, interval_right;
} pol_cons_t;

/*
 * Own linear-form builder over checker nodes.
 */
typedef struct {
  int64_t coeff[POL_MAX_VARS];
  __int128 cst;
} pol_lin_t;

static bool pol_add_i64(int64_t a, int64_t b, int64_t *out) {
  __int128 r = (__int128)a + b;
  if (r < INT64_MIN || r > INT64_MAX)
    return false;
  *out = (int64_t)r;
  return true;
}

static bool pol_mul_i64(int64_t a, int64_t b, int64_t *out) {
  __int128 r = (__int128)a * b;
  if (r < INT64_MIN || r > INT64_MAX)
    return false;
  *out = (int64_t)r;
  return true;
}

static bool pol_lin_scale(const pol_lin_t *in, __int128 k, pol_lin_t *out) {
  for (int i = 0; i < POL_MAX_VARS; ++i) {
    __int128 value;
    if (__builtin_mul_overflow((__int128)in->coeff[i], k, &value) ||
        value < INT64_MIN || value > INT64_MAX)
      return false;
    out->coeff[i] = (int64_t)value;
  }
  return !__builtin_mul_overflow(in->cst, k, &out->cst);
}

static bool pol_lin_add(const pol_lin_t *a, const pol_lin_t *b,
                        pol_lin_t *out) {
  for (int i = 0; i < POL_MAX_VARS; ++i) {
    if (!pol_add_i64(a->coeff[i], b->coeff[i], &out->coeff[i]))
      return false;
  }
  return !__builtin_add_overflow(a->cst, b->cst, &out->cst);
}

static bool pol_build_linear(pol_parser_t *ps, const pol_obligation_t *ob,
                             int idx, pol_lin_t *out, int depth,
                             pol_status_t *st) {
  if (st->failed || depth > ob->max_depth) {
    pol_fail(st, "linear build depth exhausted");
    return false;
  }
  memset(out, 0, sizeof(*out));
  const pol_node_t *n = &ps->nodes[idx];
  switch (n->kind) {
  case POL_NODE_CONST:
    out->cst = n->ival;
    return true;
  case POL_NODE_NAME:
    if (n->name < 0 || n->name >= POL_MAX_VARS) {
      pol_fail(st, "name index out of range");
      return false;
    }
    out->coeff[n->name] = 1;
    return true;
  case POL_NODE_NEG: {
    pol_lin_t inner;
    if (!pol_build_linear(ps, ob, n->left, &inner, depth + 1, st))
      return false;
    if (!pol_lin_scale(&inner, -1, out)) {
      pol_fail(st, "overflow in arithmetic negation");
      return false;
    }
    return true;
  }
  case POL_NODE_BIN: {
    if (n->op == POL_OP_ADD || n->op == POL_OP_SUB) {
      pol_lin_t l, r, sr;
      if (!pol_build_linear(ps, ob, n->left, &l, depth + 1, st) ||
          !pol_build_linear(ps, ob, n->right, &r, depth + 1, st))
        return false;
      if (n->op == POL_OP_SUB && !pol_lin_scale(&r, -1, &sr)) {
        pol_fail(st, "overflow in subtraction");
        return false;
      }
      const pol_lin_t *rr = n->op == POL_OP_SUB ? &sr : &r;
      if (!pol_lin_add(&l, rr, out)) {
        pol_fail(st, "overflow in addition");
        return false;
      }
      return true;
    }
    if (n->op == POL_OP_MUL) {
      pol_lin_t l, r;
      if (!pol_build_linear(ps, ob, n->left, &l, depth + 1, st) ||
          !pol_build_linear(ps, ob, n->right, &r, depth + 1, st))
        return false;
      bool l_const = true, r_const = true;
      for (int i = 0; i < POL_MAX_VARS; ++i) {
        if (l.coeff[i])
          l_const = false;
        if (r.coeff[i])
          r_const = false;
      }
      if (l_const && r_const) {
        if (__builtin_mul_overflow(l.cst, r.cst, &out->cst)) {
          pol_fail(st, "overflow in constant multiply");
          return false;
        }
        return true;
      }
      if (r_const) {
        if (!pol_lin_scale(&l, r.cst, out)) {
          pol_fail(st, "overflow in multiply");
          return false;
        }
        return true;
      }
      if (l_const) {
        if (!pol_lin_scale(&r, l.cst, out)) {
          pol_fail(st, "overflow in multiply");
          return false;
        }
        return true;
      }
      pol_fail(st, "nonlinear product is not decidable by this checker");
      return false;
    }
    if (n->op == POL_OP_DIV || n->op == POL_OP_MOD) {
      /*
       * Only closed operands have checker-decidable division semantics.
       */
      pol_lin_t l, r;
      if (!pol_build_linear(ps, ob, n->left, &l, depth + 1, st) ||
          !pol_build_linear(ps, ob, n->right, &r, depth + 1, st))
        return false;
      for (int i = 0; i < POL_MAX_VARS; ++i) {
        if (l.coeff[i] || r.coeff[i]) {
          pol_fail(st, "division over variables is not checker-decidable");
          return false;
        }
      }
      if (l.cst < INT64_MIN || l.cst > INT64_MAX || r.cst < INT64_MIN ||
          r.cst > INT64_MAX || r.cst == 0 ||
          (l.cst == INT64_MIN && r.cst == -1)) {
        pol_fail(st, "division undefined under the obligation ranges");
        return false;
      }
      out->cst = n->op == POL_OP_DIV ? l.cst / r.cst : l.cst % r.cst;
      return true;
    }
    pol_fail(st, "operator cannot appear inside a linear form");
    return false;
  }
  default:
    pol_fail(st, "unexpected node kind in linear build");
    return false;
  }
}

/*
 * The checker's own Fourier-Motzkin feasibility test over Σa·x + c <= 0
 * systems.  Elimination only produces equisatisfiable systems, so a derived
 * constant contradiction proves genuine infeasibility.  Overflow or a
 * budget breach reports "undecided" (0), never infeasible.
 */
static int pol_fm_unsat(const pol_cons_t *cons, int ncons, int nvars,
                        int max_cons) {
  if (ncons > max_cons || ncons > POL_MAX_CONS)
    return 0;
  pol_cons_t work[POL_MAX_CONS];
  memcpy(work, cons, sizeof(pol_cons_t) * (size_t)ncons);
  int nwork = ncons;
  for (int k = 0; k < nvars; ++k) {
    pol_cons_t pos[POL_MAX_CONS], neg[POL_MAX_CONS], zero[POL_MAX_CONS];
    int np = 0, nn = 0, nz = 0;
    for (int c = 0; c < nwork; ++c) {
      if (work[c].coeff[k] > 0 && np < POL_MAX_CONS)
        pos[np++] = work[c];
      else if (work[c].coeff[k] < 0 && nn < POL_MAX_CONS)
        neg[nn++] = work[c];
      else if (nz < POL_MAX_CONS)
        zero[nz++] = work[c];
      else
        return 0;
    }
    pol_cons_t next[POL_MAX_CONS];
    int nnext = 0;
    for (int c = 0; c < nz; ++c)
      next[nnext++] = zero[c];
    for (int i = 0; i < np; ++i) {
      for (int j = 0; j < nn; ++j) {
        if (nnext >= POL_MAX_CONS)
          return 0;
        int64_t ap = pos[i].coeff[k], an = neg[j].coeff[k];
        pol_cons_t merged;
        memset(&merged, 0, sizeof(merged));
        bool ok = true;
        for (int v = 0; v < nvars && ok; ++v) {
          if (v == k)
            continue;
          int64_t p1, p2;
          if (!pol_mul_i64(ap, neg[j].coeff[v], &p1) ||
              !pol_mul_i64(an, pos[i].coeff[v], &p2) ||
              __builtin_sub_overflow(p1, p2, &merged.coeff[v]))
            ok = false;
        }
        __int128 r1, r2, rc;
        if (ok && (__builtin_mul_overflow((__int128)ap, neg[j].cst, &r1) ||
                   __builtin_mul_overflow((__int128)an, pos[i].cst, &r2) ||
                   __builtin_sub_overflow(r1, r2, &rc)))
          ok = false;
        if (!ok)
          return 0;
        merged.cst = rc;
        next[nnext++] = merged;
      }
    }
    memcpy(work, next, sizeof(pol_cons_t) * (size_t)nnext);
    nwork = nnext;
  }
  for (int c = 0; c < nwork; ++c) {
    bool all_zero = true;
    for (int v = 0; v < nvars; ++v) {
      if (work[c].coeff[v]) {
        all_zero = false;
        break;
      }
    }
    if (all_zero && work[c].cst > 0)
      return 1;
  }
  return 0;
}

/*
 * Constraint-branch materialization of the (possibly negated) proposition.
 *
 * The negation of the proposition is expanded into a disjunction of
 * conjunctions of linear constraints, exactly the shape the elimination
 * below consumes.  Branch and constraint counts are bounded by the
 * obligation budgets; overflow or an unsupported shape marks the walk
 * undecided (never infeasible).  Temporary branch lists live on the heap
 * so deep propositions cannot exhaust the stack.
 */
#define POL_MAX_BRANCHES 64
#define POL_DNF_MAX_DEPTH POL_MAX_DEPTH

typedef struct {
  int node;
  bool negate;
} pol_pending_interval_t;

#define POL_MAX_INTERVAL_ATOMS 32

typedef struct {
  pol_cons_t cons[POL_MAX_CONS];
  int count;
  pol_pending_interval_t interval_atoms[POL_MAX_INTERVAL_ATOMS];
  int interval_count;
} pol_branch_t;

typedef struct {
  pol_branch_t *branches; /* heap array of POL_MAX_BRANCHES */
  int count;
  bool undecided;
  const pol_obligation_t *ob;
} pol_dnf_t;

static bool pol_dnf_append(pol_dnf_t *d, const pol_branch_t *b,
                           pol_status_t *st) {
  if (st && st->failed) {
    d->undecided = true;
    return false;
  }
  if (d->count >= POL_MAX_BRANCHES || d->count >= d->ob->max_cons) {
    d->undecided = true;
    return false;
  }
  d->branches[d->count++] = *b;
  return true;
}

static bool pol_dnf_build(pol_parser_t *ps, pol_dnf_t *d, int idx, bool negate,
                          int depth, pol_status_t *st);

/*
 * Bounded polynomial identity check for nonlinear equality atoms.
 *
 * The checker's linear kernel cannot decide identities like
 * a*(b+c) == a*b + a*c, yet such propositions are exactly what lemma
 * declarations contribute.  This routine expands both sides of an `==`
 * into multivariate coefficient form with its own normalization (no code
 * shared with the compiler canonicalizer) and compares.  Equality of
 * normalized polynomials proves the identity; anything unsupported stays
 * undecided.  Bounded: 8 variables, degree 8, 64 terms per side, and
 * coefficient arithmetic guarded in int128.
 */
#define POL_POLY_MAX_TERMS 64
#define POL_POLY_MAX_DEGREE 8

typedef struct {
  int64_t coeff;
  uint8_t exponents[POL_MAX_VARS];
} pol_poly_term_t;

typedef struct {
  pol_poly_term_t terms[POL_POLY_MAX_TERMS];
  int count;
  bool overflow;
} pol_poly_t;

static void pol_poly_add_term(pol_poly_t *p, int64_t coeff,
                              const uint8_t *exponents) {
  if (coeff == 0)
    return;
  for (int i = 0; i < p->count; ++i) {
    if (memcmp(p->terms[i].exponents, exponents, POL_MAX_VARS) == 0) {
      __int128 sum = (__int128)p->terms[i].coeff + coeff;
      if (sum < INT64_MIN || sum > INT64_MAX) {
        p->overflow = true;
        return;
      }
      p->terms[i].coeff = (int64_t)sum;
      if (p->terms[i].coeff == 0) {
        p->terms[i] = p->terms[p->count - 1];
        p->count--;
      }
      return;
    }
  }
  if (p->count >= POL_POLY_MAX_TERMS) {
    p->overflow = true;
    return;
  }
  p->terms[p->count].coeff = coeff;
  memcpy(p->terms[p->count].exponents, exponents, POL_MAX_VARS);
  p->count++;
}

static void pol_poly_mul(pol_poly_t *out, const pol_poly_t *a,
                         const pol_poly_t *b) {
  memset(out, 0, sizeof(*out));
  for (int i = 0; i < a->count; ++i) {
    for (int j = 0; j < b->count; ++j) {
      __int128 c = (__int128)a->terms[i].coeff * b->terms[j].coeff;
      if (c < INT64_MIN || c > INT64_MAX) {
        out->overflow = true;
        return;
      }
      uint8_t exp[POL_MAX_VARS];
      bool deg_ok = true;
      for (int v = 0; v < POL_MAX_VARS; ++v) {
        exp[v] = (uint8_t)(a->terms[i].exponents[v] + b->terms[j].exponents[v]);
        if (exp[v] > POL_POLY_MAX_DEGREE)
          deg_ok = false;
      }
      if (!deg_ok) {
        out->overflow = true;
        return;
      }
      pol_poly_add_term(out, (int64_t)c, exp);
      if (out->overflow)
        return;
    }
  }
}

static void pol_poly_accum(pol_poly_t *out, const pol_poly_t *add, int sign) {
  for (int i = 0; i < add->count; ++i) {
    __int128 c = (__int128)add->terms[i].coeff * sign;
    if (c < INT64_MIN || c > INT64_MAX) {
      out->overflow = true;
      return;
    }
    pol_poly_add_term(out, (int64_t)c, add->terms[i].exponents);
    if (out->overflow)
      return;
  }
}

/*
 * Expands a subtree into polynomial form; false when unsupported.
 */
static bool pol_poly_build(pol_parser_t *ps, int idx, pol_poly_t *out,
                           int depth) {
  if (depth > POL_MAX_DEPTH) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  const pol_node_t *n = &ps->nodes[idx];
  if (n->kind == POL_NODE_CONST) {
    uint8_t zero[POL_MAX_VARS] = {0};
    pol_poly_add_term(out, n->ival, zero);
    return !out->overflow;
  }
  if (n->kind == POL_NODE_NAME) {
    if (n->name < 0 || n->name >= POL_MAX_VARS)
      return false;
    uint8_t exp[POL_MAX_VARS] = {0};
    exp[n->name] = 1;
    pol_poly_add_term(out, 1, exp);
    return !out->overflow;
  }
  if (n->kind == POL_NODE_NEG) {
    pol_poly_t inner;
    if (!pol_poly_build(ps, n->left, &inner, depth + 1))
      return false;
    pol_poly_accum(out, &inner, -1);
    return !out->overflow;
  }
  /*
   * POL_NODE_BIN: + - * only
   */
  if (n->op != POL_OP_ADD && n->op != POL_OP_SUB && n->op != POL_OP_MUL)
    return false;
  pol_poly_t l, r;
  if (!pol_poly_build(ps, n->left, &l, depth + 1) ||
      !pol_poly_build(ps, n->right, &r, depth + 1))
    return false;
  if (n->op == POL_OP_MUL) {
    pol_poly_mul(out, &l, &r);
    return !out->overflow;
  }
  pol_poly_accum(out, &l, 1);
  if (out->overflow)
    return false;
  pol_poly_accum(out, &r, n->op == POL_OP_SUB ? -1 : 1);
  return !out->overflow;
}

/*
 * True when both sides expand to the same polynomial.
 */
static bool pol_poly_identity(pol_parser_t *ps, const pol_node_t *n,
                              bool *supported) {
  *supported = false;
  if (n->kind != POL_NODE_BIN)
    return false;
  pol_poly_t l, r;
  if (!pol_poly_build(ps, n->left, &l, 0) ||
      !pol_poly_build(ps, n->right, &r, 0))
    return false;
  *supported = true;
  if (l.count != r.count)
    return false;
  for (int i = 0; i < l.count; ++i) {
    bool found = false;
    for (int j = 0; j < r.count; ++j) {
      if (l.terms[i].coeff == r.terms[j].coeff &&
          memcmp(l.terms[i].exponents, r.terms[j].exponents, POL_MAX_VARS) ==
              0) {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

typedef struct {
  int64_t lo, hi;
} pol_interval_t;

typedef struct {
  int64_t lo[POL_MAX_VARS];
  int64_t hi[POL_MAX_VARS];
  bool inconsistent;
} pol_interval_env_t;

static bool pol_i128_to_i64(__int128 value, int64_t *out) {
  if (value < INT64_MIN || value > INT64_MAX)
    return false;
  *out = (int64_t)value;
  return true;
}

static bool pol_interval_env_init(const pol_parser_t *ps,
                                  const pol_obligation_t *ob,
                                  pol_interval_env_t *env) {
  for (int i = 0; i < POL_MAX_VARS; ++i) {
    env->lo[i] = INT64_MIN;
    env->hi[i] = INT64_MAX;
  }
  env->inconsistent = false;
  for (int r = 0; r < ob->nrange; ++r) {
    for (int i = 0; i < ps->name_count; ++i) {
      if (strcmp(ps->names[i], ob->ranges[r].name) == 0) {
        env->lo[i] = ob->ranges[r].lo;
        env->hi[i] = ob->ranges[r].hi;
        if (env->lo[i] > env->hi[i])
          env->inconsistent = true;
        break;
      }
    }
  }
  return true;
}

static __int128 pol_floor_div(__int128 numerator, __int128 denominator,
                              bool *ok) {
  const __int128 min_i128 = -((__int128)1 << 126) * 2;
  if (denominator == 0 || (numerator == min_i128 && denominator == -1)) {
    *ok = false;
    return 0;
  }
  __int128 quotient = numerator / denominator;
  __int128 remainder = numerator % denominator;
  if (remainder != 0 && ((remainder < 0) != (denominator < 0)))
    quotient--;
  *ok = true;
  return quotient;
}

static __int128 pol_ceil_div(__int128 numerator, __int128 denominator,
                             bool *ok) {
  const __int128 min_i128 = -((__int128)1 << 126) * 2;
  if (denominator == 0 || (numerator == min_i128 && denominator == -1)) {
    *ok = false;
    return 0;
  }
  __int128 quotient = numerator / denominator;
  __int128 remainder = numerator % denominator;
  if (remainder != 0 && ((remainder < 0) == (denominator < 0)))
    quotient++;
  *ok = true;
  return quotient;
}

static bool pol_interval_add_product(__int128 *sum, int64_t coefficient,
                                     int64_t value) {
  __int128 product;
  return !__builtin_mul_overflow((__int128)coefficient, (__int128)value,
                                 &product) &&
         !__builtin_add_overflow(*sum, product, sum);
}

static bool pol_interval_constraint_bound(const pol_cons_t *constraint,
                                          const pol_interval_env_t *env,
                                          int excluded, bool maximum,
                                          __int128 *bound) {
  if (!constraint || !env || !bound)
    return false;
  __int128 sum = constraint->cst;
  for (int i = 0; i < POL_MAX_VARS; ++i) {
    if (i == excluded || !constraint->coeff[i])
      continue;
    int64_t coefficient = constraint->coeff[i];
    int64_t endpoint;
    if (maximum) {
      endpoint = coefficient > 0 ? env->hi[i] : env->lo[i];
      if ((coefficient > 0 && endpoint == INT64_MAX) ||
          (coefficient < 0 && endpoint == INT64_MIN))
        return false;
    } else {
      endpoint = coefficient > 0 ? env->lo[i] : env->hi[i];
      if ((coefficient > 0 && endpoint == INT64_MIN) ||
          (coefficient < 0 && endpoint == INT64_MAX))
        return false;
    }
    if (!pol_interval_add_product(&sum, coefficient, endpoint))
      return false;
  }
  *bound = sum;
  return true;
}

static void pol_interval_env_add_constraint(pol_interval_env_t *env,
                                            const pol_cons_t *constraint) {
  if (!constraint->interval_safe)
    return;
  __int128 minimum;
  if (pol_interval_constraint_bound(constraint, env, -1, false, &minimum) &&
      minimum > 0) {
    env->inconsistent = true;
    return;
  }
  for (int i = 0; i < POL_MAX_VARS; ++i) {
    int64_t coefficient = constraint->coeff[i];
    if (!coefficient)
      continue;
    __int128 maximum_other, numerator;
    if (!pol_interval_constraint_bound(constraint, env, i, true,
                                       &maximum_other) ||
        __builtin_sub_overflow((__int128)0, maximum_other, &numerator))
      continue;
    bool div_ok = false;
    __int128 bound = coefficient > 0
                         ? pol_floor_div(numerator, coefficient, &div_ok)
                         : pol_ceil_div(numerator, coefficient, &div_ok);
    if (!div_ok)
      continue;
    if (coefficient > 0) {
      if (bound < INT64_MIN)
        env->inconsistent = true;
      else if (bound < INT64_MAX && bound >= INT64_MIN &&
               (int64_t)bound < env->hi[i])
        env->hi[i] = (int64_t)bound;
    } else {
      if (bound > INT64_MAX)
        env->inconsistent = true;
      else if (bound > INT64_MIN && bound <= INT64_MAX &&
               (int64_t)bound > env->lo[i])
        env->lo[i] = (int64_t)bound;
    }
    if (env->lo[i] > env->hi[i])
      env->inconsistent = true;
  }
}

static bool pol_interval_supported_node(const pol_parser_t *ps, int idx,
                                        int depth, int max_depth) {
  if (depth > max_depth || depth > POL_MAX_DEPTH || idx < 0 || idx >= ps->count)
    return false;
  const pol_node_t *n = &ps->nodes[idx];
  if (n->kind == POL_NODE_CONST)
    return true;
  if (n->kind == POL_NODE_NAME)
    return n->name >= 0 && n->name < ps->name_count;
  if (n->kind == POL_NODE_NEG)
    return !n->bval &&
           pol_interval_supported_node(ps, n->left, depth + 1, max_depth);
  if (n->kind != POL_NODE_BIN)
    return false;
  if (n->op == POL_OP_DIV || n->op == POL_OP_MOD) {
    return pol_interval_supported_node(ps, n->left, depth + 1, max_depth) &&
           pol_interval_supported_node(ps, n->right, depth + 1, max_depth);
  }
  return (n->op == POL_OP_ADD || n->op == POL_OP_SUB || n->op == POL_OP_MUL) &&
         pol_interval_supported_node(ps, n->left, depth + 1, max_depth) &&
         pol_interval_supported_node(ps, n->right, depth + 1, max_depth);
}

static bool pol_interval_divide_value(int64_t numerator, int64_t divisor,
                                      int64_t *quotient) {
  if (!quotient || divisor == 0 || (numerator == INT64_MIN && divisor == -1))
    return false;
  *quotient = numerator / divisor;
  return true;
}

static bool pol_nodes_same(const pol_parser_t *ps, int left, int right,
                           int depth) {
  if (left == right)
    return true;
  if (depth > POL_MAX_DEPTH || left < 0 || right < 0 || left >= ps->count ||
      right >= ps->count)
    return false;
  const pol_node_t *l = &ps->nodes[left];
  const pol_node_t *r = &ps->nodes[right];
  if (l->kind != r->kind)
    return false;
  switch (l->kind) {
  case POL_NODE_CONST:
    return l->ival == r->ival;
  case POL_NODE_NAME:
    return l->name == r->name;
  case POL_NODE_NEG:
    return l->bval == r->bval &&
           pol_nodes_same(ps, l->left, r->left, depth + 1);
  case POL_NODE_BIN:
    return l->op == r->op && pol_nodes_same(ps, l->left, r->left, depth + 1) &&
           pol_nodes_same(ps, l->right, r->right, depth + 1);
  default:
    return false;
  }
}

/*
 * Independent bounded interval evaluation for arithmetic integer terms.
 * Every intermediate interval must stay in int64, so a proved relation
 * cannot rely on signed overflow. Unsupported operators, missing ranges,
 * and wider intermediate values remain undecided.
 */
static bool pol_interval_eval(const pol_parser_t *ps,
                              const pol_obligation_t *ob,
                              const pol_interval_env_t *env, int idx, int depth,
                              pol_interval_t *out) {
  if (depth > ob->max_depth || depth > POL_MAX_DEPTH || idx < 0 ||
      idx >= ps->count)
    return false;
  const pol_node_t *n = &ps->nodes[idx];
  if (n->kind == POL_NODE_CONST) {
    out->lo = n->ival;
    out->hi = n->ival;
    return true;
  }
  if (n->kind == POL_NODE_NAME) {
    if (n->name < 0 || n->name >= ps->name_count)
      return false;
    out->lo = env->lo[n->name];
    out->hi = env->hi[n->name];
    return true;
  }
  if (n->kind == POL_NODE_NEG) {
    if (n->bval)
      return false;
    pol_interval_t inner;
    if (!pol_interval_eval(ps, ob, env, n->left, depth + 1, &inner))
      return false;
    return pol_i128_to_i64(-(__int128)inner.hi, &out->lo) &&
           pol_i128_to_i64(-(__int128)inner.lo, &out->hi);
  }
  if (n->kind != POL_NODE_BIN ||
      (n->op != POL_OP_ADD && n->op != POL_OP_SUB && n->op != POL_OP_MUL &&
       n->op != POL_OP_DIV && n->op != POL_OP_MOD))
    return false;
  pol_interval_t left, right;
  if (!pol_interval_eval(ps, ob, env, n->left, depth + 1, &left) ||
      !pol_interval_eval(ps, ob, env, n->right, depth + 1, &right))
    return false;
  if (n->op == POL_OP_DIV) {
    if (right.lo <= 0 && right.hi >= 0)
      return false;
    int64_t quotients[4];
    if (!pol_interval_divide_value(left.lo, right.lo, &quotients[0]) ||
        !pol_interval_divide_value(left.lo, right.hi, &quotients[1]) ||
        !pol_interval_divide_value(left.hi, right.lo, &quotients[2]) ||
        !pol_interval_divide_value(left.hi, right.hi, &quotients[3]))
      return false;
    out->lo = out->hi = quotients[0];
    for (int i = 1; i < 4; ++i) {
      if (quotients[i] < out->lo)
        out->lo = quotients[i];
      if (quotients[i] > out->hi)
        out->hi = quotients[i];
    }
    return true;
  }
  if (n->op == POL_OP_MOD) {
    if ((right.lo <= 0 && right.hi >= 0) ||
        (left.lo == INT64_MIN && right.lo <= -1 && right.hi >= -1))
      return false;
    __int128 abs_lo = right.lo < 0 ? -(__int128)right.lo : right.lo;
    __int128 abs_hi = right.hi < 0 ? -(__int128)right.hi : right.hi;
    __int128 max_remainder = (abs_lo > abs_hi ? abs_lo : abs_hi) - 1;
    __int128 lo = -max_remainder;
    __int128 hi = max_remainder;
    if (left.lo >= 0) {
      lo = 0;
      if (hi > left.hi)
        hi = left.hi;
    } else if (left.hi <= 0) {
      hi = 0;
      if (lo < left.lo)
        lo = left.lo;
    }
    return pol_i128_to_i64(lo, &out->lo) && pol_i128_to_i64(hi, &out->hi);
  }
  __int128 lo, hi;
  if (n->op == POL_OP_ADD) {
    lo = (__int128)left.lo + right.lo;
    hi = (__int128)left.hi + right.hi;
  } else if (n->op == POL_OP_SUB) {
    lo = (__int128)left.lo - right.hi;
    hi = (__int128)left.hi - right.lo;
  } else if (pol_nodes_same(ps, n->left, n->right, 0)) {
    __int128 a = (__int128)left.lo * left.lo;
    __int128 b = (__int128)left.hi * left.hi;
    lo = left.lo <= 0 && left.hi >= 0 ? 0 : (a < b ? a : b);
    hi = a > b ? a : b;
  } else {
    __int128 products[4] = {
        (__int128)left.lo * right.lo, (__int128)left.lo * right.hi,
        (__int128)left.hi * right.lo, (__int128)left.hi * right.hi};
    lo = hi = products[0];
    for (int i = 1; i < 4; ++i) {
      if (products[i] < lo)
        lo = products[i];
      if (products[i] > hi)
        hi = products[i];
    }
  }
  return pol_i128_to_i64(lo, &out->lo) && pol_i128_to_i64(hi, &out->hi);
}

/*
 * Return a known truth value only when direct interval bounds decide it.
 */
static bool pol_interval_relation_direct(pol_parser_t *ps,
                                         const pol_obligation_t *ob,
                                         const pol_interval_env_t *env,
                                         const pol_node_t *n, bool *known,
                                         bool *holds) {
  *known = false;
  if (n->kind != POL_NODE_BIN ||
      (n->op != POL_OP_LT && n->op != POL_OP_LE && n->op != POL_OP_GT &&
       n->op != POL_OP_GE && n->op != POL_OP_EQ && n->op != POL_OP_NE))
    return false;
  pol_interval_t left, right;
  if (!pol_interval_eval(ps, ob, env, n->left, 0, &left) ||
      !pol_interval_eval(ps, ob, env, n->right, 0, &right))
    return false;
  switch (n->op) {
  case POL_OP_LT:
    if (left.hi < right.lo) {
      *known = true;
      *holds = true;
    } else if (left.lo >= right.hi) {
      *known = true;
      *holds = false;
    }
    break;
  case POL_OP_LE:
    if (left.hi <= right.lo) {
      *known = true;
      *holds = true;
    } else if (left.lo > right.hi) {
      *known = true;
      *holds = false;
    }
    break;
  case POL_OP_GT:
    if (left.lo > right.hi) {
      *known = true;
      *holds = true;
    } else if (left.hi <= right.lo) {
      *known = true;
      *holds = false;
    }
    break;
  case POL_OP_GE:
    if (left.lo >= right.hi) {
      *known = true;
      *holds = true;
    } else if (left.hi < right.lo) {
      *known = true;
      *holds = false;
    }
    break;
  case POL_OP_EQ:
  case POL_OP_NE: {
    bool equal =
        left.lo == left.hi && right.lo == right.hi && left.lo == right.lo;
    bool unequal = left.hi < right.lo || right.hi < left.lo;
    if (equal || unequal) {
      *known = true;
      *holds = n->op == POL_OP_EQ ? equal : unequal;
    }
    break;
  }
  default:
    break;
  }
  return true;
}

static void pol_interval_split_candidate(const pol_parser_t *ps,
                                         const pol_interval_env_t *env, int idx,
                                         int depth, int *best_var,
                                         __int128 *best_width) {
  if (idx < 0 || idx >= ps->count || depth > POL_MAX_DEPTH)
    return;
  const pol_node_t *node = &ps->nodes[idx];
  if (node->kind == POL_NODE_NAME) {
    if (node->name < 0 || node->name >= ps->name_count ||
        env->lo[node->name] == INT64_MIN || env->hi[node->name] == INT64_MAX ||
        env->lo[node->name] >= env->hi[node->name])
      return;
    __int128 width =
        (__int128)env->hi[node->name] - (__int128)env->lo[node->name];
    if (width > *best_width) {
      *best_width = width;
      *best_var = node->name;
    }
    return;
  }
  if (node->kind == POL_NODE_NEG) {
    pol_interval_split_candidate(ps, env, node->left, depth + 1, best_var,
                                 best_width);
    return;
  }
  if (node->kind == POL_NODE_BIN) {
    pol_interval_split_candidate(ps, env, node->left, depth + 1, best_var,
                                 best_width);
    pol_interval_split_candidate(ps, env, node->right, depth + 1, best_var,
                                 best_width);
  }
}

/*
 * Refine only finite, explicitly bounded variable intervals. Each split
 * partitions the integer domain exactly; a relation is known only when both
 * child domains prove the same truth value. Budgets leave harder cases
 * undecided rather than weakening the proof.
 */
static bool pol_interval_relation_split(pol_parser_t *ps,
                                        const pol_obligation_t *ob,
                                        const pol_interval_env_t *env,
                                        const pol_node_t *n, int depth,
                                        int *budget, bool *known, bool *holds) {
  *known = false;
  bool direct_known = false;
  bool direct_holds = false;
  if (!pol_interval_relation_direct(ps, ob, env, n, &direct_known,
                                    &direct_holds))
    return false;
  if (direct_known) {
    *known = true;
    *holds = direct_holds;
    return true;
  }
  if (depth >= POL_MAX_INTERVAL_SPLIT_DEPTH || *budget <= 0)
    return true;
  int split_var = -1;
  __int128 split_width = 0;
  pol_interval_split_candidate(ps, env, n->left, 0, &split_var, &split_width);
  pol_interval_split_candidate(ps, env, n->right, 0, &split_var, &split_width);
  if (split_var < 0)
    return true;
  --*budget;

  int64_t lo = env->lo[split_var];
  int64_t hi = env->hi[split_var];
  int64_t mid = (int64_t)((__int128)lo + ((__int128)hi - (__int128)lo) / 2);
  pol_interval_env_t lower = *env;
  pol_interval_env_t upper = *env;
  lower.hi[split_var] = mid;
  upper.lo[split_var] = mid + 1;
  bool lower_known = false;
  bool lower_holds = false;
  bool upper_known = false;
  bool upper_holds = false;
  if (!pol_interval_relation_split(ps, ob, &lower, n, depth + 1, budget,
                                   &lower_known, &lower_holds) ||
      !pol_interval_relation_split(ps, ob, &upper, n, depth + 1, budget,
                                   &upper_known, &upper_holds))
    return false;
  if (lower_known && upper_known && lower_holds == upper_holds) {
    *known = true;
    *holds = lower_holds;
  }
  return true;
}

static bool pol_interval_relation(pol_parser_t *ps, const pol_obligation_t *ob,
                                  const pol_interval_env_t *env,
                                  const pol_node_t *n, bool *known,
                                  bool *holds) {
  int budget = POL_MAX_INTERVAL_SPLITS;
  return pol_interval_relation_split(ps, ob, env, n, 0, &budget, known, holds);
}

static void pol_interval_refine_branch(pol_parser_t *ps,
                                       const pol_obligation_t *ob,
                                       pol_branch_t *branch,
                                       pol_interval_env_t *env) {
  /*
   * Add only linear constraints whose source arithmetic is overflow-safe.
   * Revisit the branch because a direct bound can make a later expression
   * safe (for example, x <= 9 makes x + 1 representable).
   */
  for (int pass = 0; pass <= branch->count; ++pass) {
    int64_t old_lo[POL_MAX_VARS], old_hi[POL_MAX_VARS];
    memcpy(old_lo, env->lo, sizeof(old_lo));
    memcpy(old_hi, env->hi, sizeof(old_hi));
    bool old_inconsistent = env->inconsistent;
    bool any_new_safe = false;
    for (int i = 0; i < branch->count; ++i) {
      pol_cons_t *constraint = &branch->cons[i];
      if (!constraint->interval_safe && constraint->interval_left >= 0 &&
          constraint->interval_right >= 0) {
        pol_interval_t left, right;
        if (pol_interval_eval(ps, ob, env, constraint->interval_left, 0,
                              &left) &&
            pol_interval_eval(ps, ob, env, constraint->interval_right, 0,
                              &right)) {
          constraint->interval_safe = true;
          any_new_safe = true;
        }
      }
      pol_interval_env_add_constraint(env, constraint);
    }
    if (!any_new_safe && !memcmp(old_lo, env->lo, sizeof(old_lo)) &&
        !memcmp(old_hi, env->hi, sizeof(old_hi)) &&
        old_inconsistent == env->inconsistent)
      return;
  }
}

static bool pol_dnf_append_interval_atom(pol_dnf_t *d, int idx, bool negate,
                                         pol_status_t *st) {
  if (d->ob->max_cons < 1 || POL_MAX_INTERVAL_ATOMS < 1) {
    d->undecided = true;
    return false;
  }
  pol_branch_t branch;
  memset(&branch, 0, sizeof(branch));
  branch.interval_atoms[0] =
      (pol_pending_interval_t){.node = idx, .negate = negate};
  branch.interval_count = 1;
  return pol_dnf_append(d, &branch, st);
}

static bool pol_dnf_atom(pol_parser_t *ps, pol_dnf_t *d, int idx, bool negate,
                         int depth, pol_status_t *st) {
  const pol_node_t *n = &ps->nodes[idx];
  if (n->op >= POL_OP_ADD) {
    if (n->op >= POL_OP_BAND) {
      pol_fail(st, "bitwise operator used as a proposition");
      return false;
    }
    pol_fail(st, "arithmetic used as a proposition");
    return false;
  }
  pol_lin_t l, r, diff, nd;
  if (!pol_build_linear(ps, d->ob, n->left, &l, depth + 1, st) ||
      !pol_build_linear(ps, d->ob, n->right, &r, depth + 1, st)) {
    /*
     * Outside the linear fragment: an equality or disequality between
     * nonlinear sides may still be a polynomial identity (distributivity
     * and friends).  Decide those by the checker's own bounded expansion;
     * everything else stays undecided.
     */
    st->failed = false;
    st->reason[0] = '\0';
    pol_interval_env_t interval_env;
    pol_interval_env_init(ps, d->ob, &interval_env);
    bool interval_known = false, interval_holds = false;
    if (pol_interval_relation(ps, d->ob, &interval_env, n, &interval_known,
                              &interval_holds) &&
        interval_known) {
      if (negate)
        interval_holds = !interval_holds;
      if (!interval_holds)
        return true;
      pol_branch_t empty;
      memset(&empty, 0, sizeof(empty));
      return pol_dnf_append(d, &empty, st);
    }
    if (n->op == POL_OP_EQ || n->op == POL_OP_NE) {
      bool supported = false;
      bool identity = pol_poly_identity(ps, n, &supported);
      if (supported && identity) {
        bool atom_true = n->op == POL_OP_EQ;
        if (negate)
          atom_true = !atom_true;
        if (!atom_true) {
          /*
           * impossible leaf: contributes no branches to the disjunction
           */
          return true;
        }
        pol_branch_t empty;
        memset(&empty, 0, sizeof(empty));
        return pol_dnf_append(d, &empty, st);
      }
    }
    bool relational = n->op == POL_OP_LT || n->op == POL_OP_LE ||
                      n->op == POL_OP_GT || n->op == POL_OP_GE ||
                      n->op == POL_OP_EQ || n->op == POL_OP_NE;
    if (relational &&
        pol_interval_supported_node(ps, n->left, depth + 1, d->ob->max_depth) &&
        pol_interval_supported_node(ps, n->right, depth + 1, d->ob->max_depth))
      return pol_dnf_append_interval_atom(d, idx, negate, st);
    d->undecided = true;
    return false;
  }
  if (!pol_lin_scale(&r, -1, &nd) || !pol_lin_add(&l, &nd, &diff)) {
    pol_fail(st, "overflow building comparison difference");
    return false;
  }
  pol_interval_env_t interval_env;
  pol_interval_env_init(ps, d->ob, &interval_env);
  pol_interval_t interval_left, interval_right;
  bool interval_constraint_safe =
      pol_interval_eval(ps, d->ob, &interval_env, n->left, 0, &interval_left) &&
      pol_interval_eval(ps, d->ob, &interval_env, n->right, 0, &interval_right);
  int op = n->op;
  if (negate) {
    if (op == POL_OP_LT)
      op = POL_OP_GE;
    else if (op == POL_OP_LE)
      op = POL_OP_GT;
    else if (op == POL_OP_GT)
      op = POL_OP_LE;
    else if (op == POL_OP_GE)
      op = POL_OP_LT;
    else if (op == POL_OP_EQ)
      op = POL_OP_NE;
    else
      op = POL_OP_EQ;
  }
#define POL_DNF_PUSH(b, lin, k)                                                \
  do {                                                                         \
    if ((b)->count >= d->ob->max_cons || (b)->count >= POL_MAX_CONS) {         \
      d->undecided = true;                                                     \
      return false;                                                            \
    }                                                                          \
    pol_cons_t *slot = &(b)->cons[(b)->count++];                               \
    memset(slot, 0, sizeof(*slot));                                            \
    for (int i = 0; i < POL_MAX_VARS; ++i)                                     \
      slot->coeff[i] = (lin).coeff[i];                                         \
    slot->interval_safe = interval_constraint_safe;                            \
    slot->interval_left = n->left;                                             \
    slot->interval_right = n->right;                                           \
    if (__builtin_add_overflow((lin).cst, (__int128)(k), &slot->cst)) {        \
      pol_fail(st, "overflow building a constraint");                          \
      return false;                                                            \
    }                                                                          \
  } while (0)

  if (op == POL_OP_LT || op == POL_OP_LE) {
    pol_branch_t b;
    memset(&b, 0, sizeof(b));
    POL_DNF_PUSH(&b, diff, op == POL_OP_LT ? 1 : 0);
    return pol_dnf_append(d, &b, st);
  }
  if (op == POL_OP_GT || op == POL_OP_GE) {
    if (!pol_lin_scale(&diff, -1, &nd)) {
      pol_fail(st, "overflow flipping comparison");
      return false;
    }
    pol_branch_t b;
    memset(&b, 0, sizeof(b));
    POL_DNF_PUSH(&b, nd, op == POL_OP_GT ? 1 : 0);
    return pol_dnf_append(d, &b, st);
  }
  if (op == POL_OP_EQ) {
    if (!pol_lin_scale(&diff, -1, &nd)) {
      pol_fail(st, "overflow splitting equality");
      return false;
    }
    pol_branch_t b;
    memset(&b, 0, sizeof(b));
    POL_DNF_PUSH(&b, diff, 0);
    POL_DNF_PUSH(&b, nd, 0);
    return pol_dnf_append(d, &b, st);
  }
  /*
   * POL_OP_NE: two alternative branches
   */
  if (!pol_lin_scale(&diff, -1, &nd)) {
    pol_fail(st, "overflow splitting disequality");
    return false;
  }
  pol_branch_t b1, b2;
  memset(&b1, 0, sizeof(b1));
  memset(&b2, 0, sizeof(b2));
  POL_DNF_PUSH(&b1, diff, 1);
  POL_DNF_PUSH(&b2, nd, 1);
  if (!pol_dnf_append(d, &b1, st))
    return false;
  return pol_dnf_append(d, &b2, st);
#undef POL_DNF_PUSH
}

static bool pol_dnf_build(pol_parser_t *ps, pol_dnf_t *d, int idx, bool negate,
                          int depth, pol_status_t *st) {
  if (st->failed)
    return false;
  if (depth > d->ob->max_depth || depth > POL_DNF_MAX_DEPTH) {
    d->undecided = true;
    return false;
  }
  const pol_node_t *n = &ps->nodes[idx];
  if (n->kind == POL_NODE_CONST) {
    bool val = (n->ival != 0) ^ negate;
    if (!val)
      return true; /* false contributes no branches */
    pol_branch_t b;
    memset(&b, 0, sizeof(b));
    return pol_dnf_append(d, &b, st); /* the empty conjunction */
  }
  if (n->kind == POL_NODE_NAME) {
    pol_fail(st, "bare name used as a proposition");
    return false;
  }
  if (n->kind == POL_NODE_NEG) {
    if (n->bval)
      return pol_dnf_build(ps, d, n->left, !negate, depth + 1, st);
    pol_fail(st, "arithmetic negation used as a proposition");
    return false;
  }
  /*
   * POL_NODE_BIN
   */
  if (n->op == POL_OP_AND || n->op == POL_OP_OR) {
    pol_branch_t *l = malloc(sizeof(pol_branch_t) * POL_MAX_BRANCHES);
    pol_branch_t *r = malloc(sizeof(pol_branch_t) * POL_MAX_BRANCHES);
    if (!l || !r) {
      free(l);
      free(r);
      pol_fail(st, "out of memory expanding the proposition");
      return false;
    }
    pol_dnf_t ld = {l, 0, false, d->ob};
    pol_dnf_t rd = {r, 0, false, d->ob};
    /*
     * De Morgan: sides inherit the negation unchanged for both AND and OR
     * (!!(A op B) distributes the flip onto the sides); only the
     * combination differs - a negated operator swaps union and
     * cross-product.  A negated disjunction therefore cross-products the
     * negated sides, exactly !A and !B of !AvB = !A^!B.
     */
    bool lnegate = negate;
    bool rnegate = negate;
    bool lok = pol_dnf_build(ps, &ld, n->left, lnegate, depth + 1, st);
    bool rok = st->failed
                   ? false
                   : pol_dnf_build(ps, &rd, n->right, rnegate, depth + 1, st);
    bool ok = lok && rok && !st->failed;
    if (ok) {
      bool cross = ((n->op == POL_OP_AND) == !negate);
      if (!cross) {
        for (int i = 0; i < ld.count && ok; ++i)
          ok = pol_dnf_append(d, &l[i], st);
        for (int i = 0; i < rd.count && ok; ++i)
          ok = pol_dnf_append(d, &r[i], st);
      } else {
        for (int i = 0; i < ld.count && ok; ++i) {
          for (int j = 0; j < rd.count && ok; ++j) {
            if (l[i].count + r[j].count > d->ob->max_cons ||
                l[i].count + r[j].count > POL_MAX_CONS) {
              d->undecided = true;
              ok = false;
              break;
            }
            pol_branch_t b;
            memset(&b, 0, sizeof(b));
            memcpy(b.cons, l[i].cons, sizeof(pol_cons_t) * (size_t)l[i].count);
            b.count = l[i].count;
            memcpy(b.cons + b.count, r[j].cons,
                   sizeof(pol_cons_t) * (size_t)r[j].count);
            b.count += r[j].count;
            if (l[i].interval_count + r[j].interval_count >
                POL_MAX_INTERVAL_ATOMS) {
              d->undecided = true;
              ok = false;
              break;
            }
            memcpy(b.interval_atoms, l[i].interval_atoms,
                   sizeof(pol_pending_interval_t) *
                       (size_t)l[i].interval_count);
            b.interval_count = l[i].interval_count;
            memcpy(b.interval_atoms + b.interval_count, r[j].interval_atoms,
                   sizeof(pol_pending_interval_t) *
                       (size_t)r[j].interval_count);
            b.interval_count += r[j].interval_count;
            ok = pol_dnf_append(d, &b, st);
          }
        }
      }
    }
    if (!ok && st->failed)
      d->undecided = true;
    free(l);
    free(r);
    return ok;
  }
  return pol_dnf_atom(ps, d, idx, negate, depth, st);
}

/*
 * Decision entry: 1 = the proposition is proved under the obligation
 * ranges (every branch of its negation infeasible), 0 = undecided.
 * Undecided covers satisfiable branches, unsupported shapes, and budget
 * exhaustion - it is never read as refutation.
 */
static int pol_decide(pol_parser_t *ps, const pol_obligation_t *ob, int root,
                      pol_status_t *st) {
  if (st->failed)
    return 0;
  if (ps->name_count > ob->max_vars) {
    st->failed = false;
    return 0;
  }
  pol_branch_t *branches = malloc(sizeof(pol_branch_t) * POL_MAX_BRANCHES);
  if (!branches) {
    pol_fail(st, "out of memory deciding the proposition");
    return 0;
  }
  pol_dnf_t d = {branches, 0, false, ob};
  int result = 0;
  int build_ok = pol_dnf_build(ps, &d, root, true, 0, st);
  if (build_ok && !st->failed) {
    if (d.count == 0) {
      result = 1; /* the negation is empty: trivially proved */
    } else {
      /*
       * Prepend the obligation ranges to every branch, then eliminate.
       */
      result = 1;
      for (int b = 0; b < d.count && result == 1; ++b) {
        pol_cons_t sys[POL_MAX_CONS];
        int nsys = 0;
        bool ok = true;
        pol_interval_env_t interval_env;
        pol_interval_env_init(ps, ob, &interval_env);
        for (int r = 0; r < ob->nrange && ok; ++r) {
          int v = -1;
          for (int x = 0; x < ps->name_count; ++x) {
            if (strcmp(ps->names[x], ob->ranges[r].name) == 0) {
              v = x;
              break;
            }
          }
          if (v < 0)
            continue;
          if (nsys + 2 > POL_MAX_CONS || nsys + 2 > ob->max_cons) {
            ok = false;
            break;
          }
          memset(&sys[nsys], 0, sizeof(pol_cons_t));
          sys[nsys].coeff[v] = -1;
          sys[nsys].cst = ob->ranges[r].lo; /* -x + lo <= 0 */
          nsys++;
          memset(&sys[nsys], 0, sizeof(pol_cons_t));
          sys[nsys].coeff[v] = 1;
          sys[nsys].cst = -(__int128)ob->ranges[r].hi; /* x - hi <= 0 */
          nsys++;
        }
        if (ok && nsys + branches[b].count <= POL_MAX_CONS &&
            nsys + branches[b].count <= ob->max_cons) {
          pol_interval_refine_branch(ps, ob, &branches[b], &interval_env);
          pol_cons_t safe_sys[POL_MAX_CONS];
          memcpy(safe_sys, sys, sizeof(pol_cons_t) * (size_t)nsys);
          int safe_count = nsys;
          for (int c = 0; c < branches[b].count; ++c) {
            if (!branches[b].cons[c].interval_safe)
              continue;
            if (safe_count >= POL_MAX_CONS || safe_count >= ob->max_cons) {
              ok = false;
              break;
            }
            safe_sys[safe_count++] = branches[b].cons[c];
          }
          if (!ok) {
            result = 0; /* bound checker budget: undecided */
            continue;
          }
          int unsat =
              pol_fm_unsat(safe_sys, safe_count, ps->name_count, ob->max_cons);
          if (!unsat && !interval_env.inconsistent) {
            bool impossible = false;
            for (int atom = 0; atom < branches[b].interval_count; ++atom) {
              pol_pending_interval_t *pending =
                  &branches[b].interval_atoms[atom];
              bool known = false, holds = false;
              if (pending->node < 0 || pending->node >= ps->count ||
                  !pol_interval_relation(ps, ob, &interval_env,
                                         &ps->nodes[pending->node], &known,
                                         &holds) ||
                  !known)
                continue;
              if (pending->negate)
                holds = !holds;
              if (!holds) {
                impossible = true;
                break;
              }
            }
            if (!impossible)
              result = 0; /* possible branch: unknown, never a proof */
          }
        } else {
          result = 0; /* budget: undecided, never proved */
        }
      }
    }
  } else {
    result = 0; /* undecided or unsupported */
  }
  free(branches);
  st->failed = false;
  st->reason[0] = '\0';
  return result;
}

typedef struct {
  char names[POL_MAX_VARS][POL_MAX_NAME];
  int64_t values[POL_MAX_VARS];
  int count;
} pol_env_t;

static bool pol_eval(pol_parser_t *ps, int idx, const pol_env_t *env,
                     int64_t *out, bool *out_bool, int depth,
                     pol_status_t *st) {
  if (st->failed || depth > POL_MAX_DEPTH) {
    pol_fail(st, "evaluation depth exhausted");
    return false;
  }
  const pol_node_t *n = &ps->nodes[idx];
  switch (n->kind) {
  case POL_NODE_CONST:
    *out = n->ival;
    if (out_bool)
      *out_bool = n->ival != 0;
    return true;
  case POL_NODE_NAME: {
    for (int i = 0; i < env->count; ++i) {
      if (strcmp(env->names[i], ps->names[n->name]) == 0) {
        *out = env->values[i];
        if (out_bool)
          *out_bool = *out != 0;
        return true;
      }
    }
    pol_fail(st, "counterexample does not bind '%s'", ps->names[n->name]);
    return false;
  }
  case POL_NODE_NEG: {
    int64_t v = 0;
    if (!pol_eval(ps, n->left, env, &v, NULL, depth + 1, st))
      return false;
    if (v == INT64_MIN) {
      pol_fail(st, "negation overflow during evaluation");
      return false;
    }
    *out = -v;
    if (out_bool)
      *out_bool = *out != 0;
    return true;
  }
  case POL_NODE_BIN: {
    if (n->op == POL_OP_AND || n->op == POL_OP_OR) {
      int64_t lv = 0, rv = 0;
      bool lb = false, rb = false;
      if (!pol_eval(ps, n->left, env, &lv, &lb, depth + 1, st) ||
          !pol_eval(ps, n->right, env, &rv, &rb, depth + 1, st))
        return false;
      (void)lv;
      (void)rv;
      bool v = n->op == POL_OP_AND ? (lb && rb) : (lb || rb);
      *out = v ? 1 : 0;
      if (out_bool)
        *out_bool = v;
      return true;
    }
    int64_t l = 0, r = 0;
    bool lb = false, rb = false;
    if (!pol_eval(ps, n->left, env, &l, &lb, depth + 1, st) ||
        !pol_eval(ps, n->right, env, &r, &rb, depth + 1, st))
      return false;
    switch (n->op) {
    case POL_OP_LT:
      *out = l < r;
      break;
    case POL_OP_LE:
      *out = l <= r;
      break;
    case POL_OP_GT:
      *out = l > r;
      break;
    case POL_OP_GE:
      *out = l >= r;
      break;
    case POL_OP_EQ:
      *out = l == r;
      break;
    case POL_OP_NE:
      *out = l != r;
      break;
    case POL_OP_ADD:
      *out = (int64_t)((__int128)l + r);
      break;
    case POL_OP_SUB:
      *out = (int64_t)((__int128)l - r);
      break;
    case POL_OP_MUL:
      *out = (int64_t)((__int128)l * r);
      break;
    case POL_OP_DIV:
    case POL_OP_MOD:
      if (r == 0 || (l == INT64_MIN && r == -1)) {
        pol_fail(st, "division undefined during evaluation");
        return false;
      }
      *out = n->op == POL_OP_DIV ? l / r : l % r;
      break;
    case POL_OP_BAND:
      *out = l & r;
      break;
    case POL_OP_BOR:
      *out = l | r;
      break;
    case POL_OP_BXOR:
      *out = l ^ r;
      break;
    case POL_OP_SHL:
      if (r < 0 || r >= 64) {
        pol_fail(st, "shift width out of range during evaluation");
        return false;
      }
      *out = (int64_t)((uint64_t)l << (unsigned)r);
      break;
    case POL_OP_SHR:
      if (r < 0 || r >= 64) {
        pol_fail(st, "shift width out of range during evaluation");
        return false;
      }
      *out = l >> r;
      break;
    default:
      pol_fail(st, "unsupported operator during evaluation");
      return false;
    }
    if (out_bool)
      *out_bool = *out != 0;
    return true;
  }
  default:
    pol_fail(st, "unexpected node during evaluation");
    return false;
  }
}

static bool pol_parse_cex(const char *cex, pol_env_t *env, pol_status_t *st) {
  memset(env, 0, sizeof(*env));
  if (!cex || !*cex)
    return true; /* closed proposition */
  const char *p = cex;
  while (*p) {
    if (env->count >= POL_MAX_VARS) {
      pol_fail(st, "counterexample binds too many names");
      return false;
    }
    char name[POL_MAX_NAME];
    size_t i = 0;
    while (*p && *p != '=') {
      if (i + 1 >= POL_MAX_NAME || !pol_parse_name_char(*p)) {
        pol_fail(st, "malformed counterexample name");
        return false;
      }
      name[i++] = *p++;
    }
    name[i] = '\0';
    if (i == 0 || *p != '=') {
      pol_fail(st, "counterexample entry lacks '='");
      return false;
    }
    ++p;
    const char *start = p;
    while (*p && *p != ';')
      ++p;
    int64_t value = 0;
    if (!pol_parse_decimal(start, (size_t)(p - start), &value)) {
      pol_fail(st, "malformed counterexample value");
      return false;
    }
    memcpy(env->names[env->count], name, i + 1);
    env->values[env->count] = value;
    env->count++;
    if (*p == ';')
      ++p;
  }
  return true;
}

bool ny_proof_policy_certificate_check(const char *obligation_text,
                                       const char *certificate_text,
                                       char *reason, size_t reason_cap) {
  pol_status_t st;
  memset(&st, 0, sizeof(st));
  pol_obligation_t ob;
  pol_certificate_t cert;
  if (reason && reason_cap > 0)
    reason[0] = '\0';
  if (!pol_parse_obligation(obligation_text, &ob, &st) ||
      !pol_parse_certificate(certificate_text, &cert, &st))
    goto reject;
  {
    /*
     * Digest binding: the certificate must name this exact obligation.
     */
    char expect_hex[17];
    ny_proof_policy_digest_hex(pol_digest(obligation_text), expect_hex);
    if (strcmp(cert.digest_hex, expect_hex) != 0) {
      pol_fail(&st, "certificate digest does not match the obligation");
      goto reject;
    }
  }
  {
    /*
     * Parse the proposition with the checker's own parser and budgets.
     */
    pol_parser_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.max_depth = ob.max_depth;
    size_t plen = strlen(ob.prop);
    int root = pol_parse_node(&ps, ob.prop, plen, 0, &st);
    if (root < 0) {
      pol_fail(&st, "proposition is not checker-decidable");
      goto reject;
    }
    if (ps.name_count > ob.max_vars) {
      pol_fail(&st, "proposition names exceed the obligation budget");
      goto reject;
    }
    /*
     * Every range must be for a name the proposition mentions; ranges for
     * unknown names would silently widen the premise set.
     */
    for (int r = 0; r < ob.nrange; ++r) {
      bool found = false;
      for (int v = 0; v < ps.name_count; ++v) {
        if (strcmp(ps.names[v], ob.ranges[r].name) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        pol_fail(&st, "range for name '%s' is not used by the proposition",
                 ob.ranges[r].name);
        goto reject;
      }
    }
    int decision = 0;
    {
      decision = pol_decide(&ps, &ob, root, &st);
      if (st.failed)
        goto reject;
    }
    if (cert.verdict == NY_PROOF_VERDICT_PROVED) {
      if (decision != 1) {
        pol_fail(&st, "claimed proved but the checker's decision is undecided");
        goto reject;
      }
      if (reason && reason_cap > 0)
        snprintf(reason, reason_cap, "checker re-derived the proof");
      return true;
    }
    if (cert.verdict == NY_PROOF_VERDICT_REFUTED) {
      if (decision == 1) {
        pol_fail(&st, "claimed refuted but the checker proved the proposition");
        goto reject;
      }
      pol_env_t env;
      /*
       * A closed proposition (no names) is refuted by evaluation alone;
       * any named proposition must carry its bindings.
       */
      if (!cert.cex[0] && ps.name_count > 0) {
        pol_fail(&st, "refutation without a counterexample");
        goto reject;
      }
      if (!pol_parse_cex(cert.cex, &env, &st))
        goto reject;
      /*
       * every binding must sit inside the obligation ranges
       */
      for (int i = 0; i < env.count; ++i) {
        bool known = false;
        for (int v = 0; v < ps.name_count; ++v) {
          if (strcmp(ps.names[v], env.names[i]) == 0)
            known = true;
        }
        if (!known) {
          pol_fail(&st, "counterexample binds unknown name '%s'", env.names[i]);
          goto reject;
        }
        for (int r = 0; r < ob.nrange; ++r) {
          if (strcmp(ob.ranges[r].name, env.names[i]) == 0 &&
              (env.values[i] < ob.ranges[r].lo ||
               env.values[i] > ob.ranges[r].hi)) {
            pol_fail(&st, "counterexample value for '%s' leaves its range",
                     env.names[i]);
            goto reject;
          }
        }
      }
      int64_t result = 0;
      bool result_bool = false;
      if (!pol_eval(&ps, root, &env, &result, &result_bool, 0, &st))
        goto reject;
      if (result_bool) {
        pol_fail(&st, "counterexample does not falsify the proposition");
        goto reject;
      }
      if (reason && reason_cap > 0)
        snprintf(reason, reason_cap, "checker verified the counterexample");
      return true;
    }
    /*
     * unknown
     */
    if (!cert.explanation[0]) {
      pol_fail(&st, "unknown verdict requires an explanation");
      goto reject;
    }
    if (decision != 0) {
      pol_fail(&st, "claimed unknown but the checker decided the obligation");
      goto reject;
    }
    if (reason && reason_cap > 0)
      snprintf(reason, reason_cap,
               "unknown accepted: checker is also undecided");
    return true;
  }

reject:
  if (reason && reason_cap > 0 && st.reason[0])
    snprintf(reason, reason_cap, "%s", st.reason);
  return false;
}
