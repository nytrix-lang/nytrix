#ifndef NY_TYPING_POLICY_H
#define NY_TYPING_POLICY_H

#include "code/lang/parse/ast.h"

/*
 * Versioned proof-obligation and proof-certificate interface.
 *
 * The proof *policy* — which tactics to try, in which order, with which
 * budgets, and how to combine backend results — is authored in Ny
 * (lib/proof.ny, module core.proof).  The compiler and the Ny policy
 * speak through the versioned obligation/certificate texts defined here.
 * Trust never crosses that boundary by assertion: every certificate is
 * validated by the independent bounded checker in policy/check.c,
 * which re-derives the verdict with its own parser and decision procedure and
 * shares no code with the solver kernels in typing/proof.c.
 *
 * Obligation text (one line, no newlines):
 *
 *   ny-proof-obligation-v1|<prop>|<ranges>|<policy>|<budgets>
 *
 *   <prop>     canonical proposition (the proof<...> body form)
 *   <ranges>   name=lo..hi joined by ';' and sorted by name (may be empty)
 *   <policy>   ','-joined sorted tactic tokens (empty means "all"):
 *              fold range solver presburger z3 auto all
 *   <budgets>  v<max_vars>,c<max_cons>,d<max_depth>
 *
 * Certificate text (one line, no newlines):
 *
 *   ny-proof-certificate-v1|<digest>|<verdict>|<tactic>|<backend>|<cex>|<explanation>
 *
 *   <digest>      16 hex digits: FNV-1a of the full obligation text
 *   <verdict>     proved | refuted | unknown   (unknown is distinct from
 *                 false: it means "no decision", never "decided false")
 *   <cex>         counterexample bindings `name=value;...` (refuted only)
 *   <explanation> human-readable reason; '|' is rejected by the serializers
 */

#define NY_PROOF_OBLIGATION_FORMAT "ny-proof-obligation-v1"
#define NY_PROOF_CERTIFICATE_FORMAT "ny-proof-certificate-v1"

#define NY_PROOF_OBLIGATION_MAX 4096
#define NY_PROOF_CERTIFICATE_MAX 1024
#define NY_PROOF_POLICY_EXPLAIN_MAX 256
#define NY_PROOF_POLICY_CEX_MAX 160

/* Tri-state verdicts. 0 is unknown, deliberately distinct from refuted. */
enum {
  NY_PROOF_VERDICT_REFUTED = -1,
  NY_PROOF_VERDICT_UNKNOWN = 0,
  NY_PROOF_VERDICT_PROVED = 1,
};

/* Bounded default budgets for obligation records. */
#define NY_PROOF_POLICY_MAX_VARS 16
#define NY_PROOF_POLICY_MAX_CONS 128
#define NY_PROOF_POLICY_MAX_DEPTH 64

typedef struct {
  int verdict; /* NY_PROOF_VERDICT_* */
  char tactic[32];
  char backend[32];
  char cex[NY_PROOF_POLICY_CEX_MAX];
  char explanation[NY_PROOF_POLICY_EXPLAIN_MAX];
  char obligation[NY_PROOF_OBLIGATION_MAX];
  char certificate[NY_PROOF_CERTIFICATE_MAX];
  bool has_certificate; /* certificate text present AND checker-validated */
} ny_proof_policy_result_t;

/*
 * Build the versioned obligation text for `proposition` under the integer
 * ranges currently known for the names it mentions.  `policy_text` is the
 * raw strategy string (may contain `by_*` tokens and free-form message
 * text; only `by_*` tokens shape the policy).  Returns false when the
 * proposition cannot be canonicalized or the policy is malformed.
 */
bool ny_proof_policy_obligation_build(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *proposition,
                                      const char *policy_text,
                                      bool contract_safe, char *out,
                                      size_t out_cap);

/*
 * Validate `by_*` tokens in a strategy/message string without evaluating
 * anything.  Returns false (and writes a reason) when a token starts with
 * "by_" but names no known tactic — malformed policies fail closed.
 */
bool ny_proof_policy_validate_text(const char *policy_text, char *reason,
                                   size_t reason_cap);

/*
 * Single funnel for compiler proof sites.  Builds the obligation, runs the
 * tactics named by the policy (lemma instantiation, range decisions, then
 * the solver kernels, then exact folding) and fills `out` with a tri-state
 * verdict, the tactic/backend that decided it, and a checker-validated
 * certificate whenever a decision was reached.  Never reports proved
 * without `has_certificate`.
 */
bool ny_proof_policy_decide(codegen_t *cg, scope *scopes, size_t depth,
                            expr_t *proposition, const char *policy_text,
                            bool contract_safe, ny_proof_policy_result_t *out);

/* Mark a proposition proved by exact compile-time evaluation. */
bool ny_proof_policy_mark_fold(ny_proof_policy_result_t *out);

/*
 * The independent bounded checker (policy/check.c).  Recomputes the
 * obligation digest, re-parses both texts with its own parser, re-derives
 * the verdict with its own decision procedure, and verifies refutation
 * counterexamples.  Returns true only when the certificate stands; `reason`
 * always receives a short explanation of the outcome either way.
 */
bool ny_proof_policy_certificate_check(const char *obligation_text,
                                       const char *certificate_text,
                                       char *reason, size_t reason_cap);

/*
 * Solver kernels over the canonical obligation encoding (typing/proof.c).
 * `prop` is a canonical proposition, `ranges` the obligation range field.
 * Runs the named backend ("presburger", "z3", or "auto" with differential
 * coherence) under the given bounds and returns NY_PROOF_VERDICT_*; `cex`
 * receives counterexample bindings when the verdict is refuted.  Every
 * backend returns through the same normalization: out-of-budget or
 * unsupported shapes are unknown, and refuted requires a verified
 * counterexample.
 */
int ny_proof_solver_check_canonical(const char *prop, const char *ranges,
                                    const char *mode, int max_vars,
                                    int max_cons, int max_depth, char *cex,
                                    size_t cex_cap);

/*
 * Field escaping: '|' separates fields and '%' introduces escapes, so both
 * are percent-encoded inside every field.  The digest is always computed
 * over the escaped obligation text.
 */
void ny_proof_policy_escape_field(const char *in, char *out, size_t out_cap);
void ny_proof_policy_unescape_field(const char *in, char *out, size_t out_cap);

/*
 * Differential selftest: sweeps propositions and range sets and requires
 * the solver kernels and the independent checker to agree (see
 * --proof-policy-selftest).  Returns 0 on agreement.
 */
int ny_proof_policy_differential_selftest(void);

/* FNV-1a over bytes, seeded and finalized for digest fields. */
uint64_t ny_proof_policy_digest_text(const char *text);

/* Hex-encode a digest into `out` (17 bytes minimum, NUL-terminated). */
void ny_proof_policy_digest_hex(uint64_t digest, char *out);

#endif
