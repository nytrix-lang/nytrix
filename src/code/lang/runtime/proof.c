/*
 * Proof runtime: compile-time proof evaluation and erasure bridge
 * for proof<P>-annotated safe-mode operations and bounds checks.
 *
 * Fin<n> (NY_BT_FIN) compiles literal-bound checks and erases to
 * plain int.  Symbolic bounds need the comptime evaluator.
 * rt_proof_cert_check handles the remaining runtime proof-evaluation
 * path until comptime covers every current prove() shape.
 */
#include "code/lang/runtime/shared.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

extern int64_t rt_alloc_string_len(const char *s, size_t len);

/*
 * Policy interface constants and functions from the compiler side
 * (src/code/lang/types/policy.h); the runtime links the compiler object
 * library, so direct calls are available.
 */
#include "code/lang/types/policy.h"

typedef struct {
  char kind;
  int left;
  int right;
  int atom;
} rt_proof_node_t;

typedef struct {
  const char *text;
  size_t len;
  size_t pos;
  rt_proof_node_t *nodes;
  int node_count;
  int max_nodes;
  const char **atoms;
  size_t *atom_lens;
  int atom_count;
  int max_variables;
  int max_depth;
} rt_proof_parser_t;

static bool rt_proof_string(int64_t value, const char **text, size_t *len) {
  if (is_v_str(value)) {
    *text = (const char *)(uintptr_t)value;
    *len = rt_tagged_str_len(value);
    return true;
  }
  /*
   * Statically typed `str` parameters use the native C-string ABI. Dynamic
   * dictionary reads normally arrive boxed, but optimizer and native-only
   * paths can preserve the C-string representation. Accept that ABI here
   * using the runtime's page-aware bounded scanner; an integer such as 101
   * is rejected without an unsafe strlen/strcmp read.
   */
  const char *raw = (const char *)(uintptr_t)value;
  size_t n = 0;
  if ((uintptr_t)value < 4096 || !rt_checked_cstr_len(raw, 1u << 20, &n))
    return false;
  *text = raw;
  *len = n;
  return true;
}

static uint64_t rt_proof_hash_bytes(uint64_t hash, const char *text,
                                    size_t len) {
  for (size_t i = 0; i < len; ++i)
    hash = (hash ^ (unsigned char)text[i]) * UINT64_C(1099511628211);
  return hash;
}

static int64_t rt_proof_digest_value(int64_t canonical_v,
                                     int64_t module_version_v,
                                     int64_t dependency_digest_v,
                                     int64_t checker_version_v) {
  const char *parts[4];
  size_t lens[4];
  if (!rt_proof_string(canonical_v, &parts[0], &lens[0]) ||
      !rt_proof_string(module_version_v, &parts[1], &lens[1]) ||
      !rt_proof_string(dependency_digest_v, &parts[2], &lens[2]) ||
      !rt_proof_string(checker_version_v, &parts[3], &lens[3]))
    return -1;
  uint64_t hash = UINT64_C(1469598103934665603);
  static const char separator = '\0';
  for (size_t i = 0; i < 4; ++i) {
    hash = rt_proof_hash_bytes(hash, parts[i], lens[i]);
    hash = rt_proof_hash_bytes(hash, &separator, 1);
  }
  return (int64_t)(hash & UINT64_C(0x000fffffffffffff));
}

int64_t rt_proof_cert_digest(int64_t canonical_v, int64_t module_version_v,
                             int64_t dependency_digest_v,
                             int64_t checker_version_v) {
  int64_t digest = rt_proof_digest_value(
      canonical_v, module_version_v, dependency_digest_v, checker_version_v);
  return digest < 0 ? NY_IMM_NIL : rt_tag_v(digest);
}

static bool rt_proof_parse_decimal(rt_proof_parser_t *p, size_t *value) {
  if (p->pos >= p->len || p->text[p->pos] < '0' || p->text[p->pos] > '9')
    return false;
  size_t start = p->pos;
  size_t out = 0;
  while (p->pos < p->len && p->text[p->pos] >= '0' && p->text[p->pos] <= '9') {
    unsigned digit = (unsigned)(p->text[p->pos++] - '0');
    if (out > (SIZE_MAX - digit) / 10)
      return false;
    out = out * 10 + digit;
  }
  if (p->pos >= p->len || p->text[p->pos++] != ':')
    return false;
  if (p->pos - start > 2 && p->text[start] == '0')
    return false;
  *value = out;
  return true;
}

static int rt_proof_atom_index(rt_proof_parser_t *p, const char *name,
                               size_t len) {
  for (int i = 0; i < p->atom_count; ++i)
    if (p->atom_lens[i] == len && memcmp(p->atoms[i], name, len) == 0)
      return i;
  if (p->atom_count >= p->max_variables)
    return -1;
  int index = p->atom_count++;
  p->atoms[index] = name;
  p->atom_lens[index] = len;
  return index;
}

static int rt_proof_parse_node(rt_proof_parser_t *p, int depth, size_t end) {
  if (depth > p->max_depth || p->pos >= end || p->node_count >= p->max_nodes)
    return -1;
  int index = p->node_count++;
  rt_proof_node_t *node = &p->nodes[index];
  memset(node, 0, sizeof(*node));
  node->left = node->right = node->atom = -1;
  node->kind = p->text[p->pos++];
  if (node->kind == 'T' || node->kind == 'F')
    return p->pos == end ? index : -1;
  size_t child_len = 0;
  if (!rt_proof_parse_decimal(p, &child_len) || child_len > end - p->pos)
    return -1;
  size_t child_end = p->pos + child_len;
  if (node->kind == 'A') {
    if (child_len == 0)
      return -1;
    node->atom = rt_proof_atom_index(p, p->text + p->pos, child_len);
    p->pos = child_end;
    return node->atom >= 0 && p->pos == end ? index : -1;
  }
  if (node->kind == 'N') {
    node->left = rt_proof_parse_node(p, depth + 1, child_end);
    return node->left >= 0 && p->pos == end ? index : -1;
  }
  if (node->kind != '&' && node->kind != '|' && node->kind != '>' &&
      node->kind != '=')
    return -1;
  node->left = rt_proof_parse_node(p, depth + 1, child_end);
  if (node->left < 0 || p->pos != child_end ||
      !rt_proof_parse_decimal(p, &child_len) || child_len > end - p->pos)
    return -1;
  child_end = p->pos + child_len;
  node->right = rt_proof_parse_node(p, depth + 1, child_end);
  return node->right >= 0 && p->pos == end ? index : -1;
}

static bool rt_proof_eval(const rt_proof_node_t *nodes, int index,
                          uint64_t assignment, int64_t *steps,
                          int64_t max_steps, bool *ok) {
  if (index < 0 || *steps >= max_steps) {
    *ok = false;
    return false;
  }
  (*steps)++;
  const rt_proof_node_t *node = &nodes[index];
  if (node->kind == 'T')
    return true;
  if (node->kind == 'F')
    return false;
  if (node->kind == 'A')
    return ((assignment >> node->atom) & 1u) != 0;
  bool left =
      rt_proof_eval(nodes, node->left, assignment, steps, max_steps, ok);
  if (!*ok)
    return false;
  if (node->kind == 'N')
    return !left;
  bool right =
      rt_proof_eval(nodes, node->right, assignment, steps, max_steps, ok);
  if (!*ok)
    return false;
  if (node->kind == '&')
    return left && right;
  if (node->kind == '|')
    return left || right;
  if (node->kind == '>')
    return !left || right;
  return left == right;
}

int64_t rt_proof_cert_check(int64_t canonical_v, int64_t digest_v,
                            int64_t module_version_v,
                            int64_t dependency_digest_v,
                            int64_t checker_version_v, int64_t max_variables_v,
                            int64_t max_nodes_v, int64_t max_depth_v,
                            int64_t max_steps_v, int64_t max_memory_v) {
  int64_t expected = rt_proof_digest_value(
      canonical_v, module_version_v, dependency_digest_v, checker_version_v);
  /*
   * The runtime declaration is dynamically typed: the digest and budgets
   * cross this boundary as tagged Ny integers.
   */
  if (!is_int(max_variables_v) || !is_int(max_nodes_v) ||
      !is_int(max_depth_v) || !is_int(max_steps_v) || !is_int(max_memory_v))
    return NY_IMM_FALSE;
  /*
   * Runtime-call lowering may preserve the digest as a tagged Ny integer or
   * pass its raw integer payload. Accept either representation only when it
   * equals the recomputed digest; never infer validity from parity alone.
   */
  bool digest_matches = digest_v == expected ||
                        (is_int(digest_v) && rt_untag_v(digest_v) == expected);
  if (expected < 0 || !digest_matches)
    return NY_IMM_FALSE;
  int64_t max_variables = rt_untag_v(max_variables_v);
  int64_t max_nodes = rt_untag_v(max_nodes_v);
  int64_t max_depth = rt_untag_v(max_depth_v);
  int64_t max_steps = rt_untag_v(max_steps_v);
  int64_t max_memory = rt_untag_v(max_memory_v);
  if (max_variables < 0 || max_variables > 20 || max_nodes <= 0 ||
      max_nodes > 1000000 || max_depth <= 0 || max_depth > 4096 ||
      max_steps <= 0 || max_memory <= 0)
    return NY_IMM_FALSE;
  /*
   * The language-level memory budget counts working-set cells: one per
   * possible node and one each for an atom pointer and its length.
   */
  int64_t atom_metadata_cells = max_variables * 2;
  if (max_memory <= atom_metadata_cells)
    return NY_IMM_FALSE;
  int64_t node_budget = max_memory - atom_metadata_cells;
  if (max_nodes > node_budget)
    max_nodes = node_budget;
  const char *canonical = NULL;
  size_t canonical_len = 0;
  if (!rt_proof_string(canonical_v, &canonical, &canonical_len))
    return NY_IMM_FALSE;
  rt_proof_node_t *nodes = calloc((size_t)max_nodes, sizeof(*nodes));
  const char **atoms =
      calloc((size_t)(max_variables ? max_variables : 1), sizeof(*atoms));
  size_t *atom_lens =
      calloc((size_t)(max_variables ? max_variables : 1), sizeof(*atom_lens));
  if (!nodes || !atoms || !atom_lens) {
    free(nodes);
    free(atoms);
    free(atom_lens);
    return NY_IMM_FALSE;
  }
  rt_proof_parser_t parser = {
      canonical,      canonical_len, 0,         nodes, 0,
      (int)max_nodes, atoms,         atom_lens, 0,     (int)max_variables,
      (int)max_depth};
  int root = rt_proof_parse_node(&parser, 0, canonical_len);
  bool valid = root >= 0 && parser.pos == canonical_len;
  int64_t steps = 0;
  if (valid) {
    uint64_t assignments = UINT64_C(1) << parser.atom_count;
    for (uint64_t mask = 0; mask < assignments && valid; ++mask) {
      bool within_budget = true;
      valid =
          rt_proof_eval(nodes, root, mask, &steps, max_steps, &within_budget) &&
          within_budget;
    }
  }
  free(nodes);
  free(atoms);
  free(atom_lens);
  return valid ? NY_IMM_TRUE : NY_IMM_FALSE;
}

/*
 * Versioned proof-policy surface (module core.proof).
 *
 * rt_proof_policy_solve is the search side: it runs the compiler's solver
 * kernels over a canonical obligation field set and answers with a tri-state
 * verdict string - "proved", "refuted:<bindings>", or "unknown:<reason>".
 * Unknown is a first-class answer here: it is never collapsed into false,
 * and callers must treat it as "no decision".
 *
 * rt_proof_policy_check is the trust boundary: it forwards to the
 * independent bounded checker, which re-derives the certificate's verdict
 * with its own parser and decision procedure.  Search results only count
 * when this returns true.
 */
int64_t rt_proof_policy_solve(int64_t canonical_v, int64_t ranges_v,
                              int64_t mode_v, int64_t max_vars_v,
                              int64_t max_cons_v, int64_t max_depth_v) {
  if (!is_int(max_vars_v) || !is_int(max_cons_v) || !is_int(max_depth_v))
    return rt_alloc_string_len("unknown:budgets must be integers", 27);
  const char *canonical = NULL;
  const char *ranges = NULL;
  const char *mode = NULL;
  size_t canonical_len = 0, ranges_len = 0, mode_len = 0;
  char ranges_buf[NY_PROOF_OBLIGATION_MAX];
  char mode_buf[32];
  if (!rt_proof_string(canonical_v, &canonical, &canonical_len))
    return rt_alloc_string_len("unknown:malformed proposition", 28);
  if (ranges_v == 0 || ranges_v == NY_IMM_NIL) {
    ranges = "";
  } else if (rt_proof_string(ranges_v, &ranges, &ranges_len)) {
    if (ranges_len >= sizeof(ranges_buf))
      return rt_alloc_string_len("unknown:range field oversized", 28);
    memcpy(ranges_buf, ranges, ranges_len + 1);
    ranges = ranges_buf;
  } else {
    return rt_alloc_string_len("unknown:malformed ranges", 24);
  }
  if (mode_v == 0 || mode_v == NY_IMM_NIL) {
    mode = "auto";
  } else if (rt_proof_string(mode_v, &mode, &mode_len)) {
    if (mode_len >= sizeof(mode_buf))
      return rt_alloc_string_len("unknown:mode oversized", 22);
    memcpy(mode_buf, mode, mode_len + 1);
    mode = mode_buf;
  } else {
    return rt_alloc_string_len("unknown:malformed mode", 22);
  }
  int64_t max_vars = rt_untag_v(max_vars_v);
  int64_t max_cons = rt_untag_v(max_cons_v);
  int64_t max_depth = rt_untag_v(max_depth_v);
  if (max_vars < 1 || max_vars > 8 || max_cons < 1 || max_cons > 32 ||
      max_depth < 1 || max_depth > 64)
    return rt_alloc_string_len("unknown:budgets out of range", 27);
  char cex[128];
  int verdict = ny_proof_solver_check_canonical(
      canonical, ranges, mode, (int)max_vars, (int)max_cons, (int)max_depth,
      cex, sizeof(cex));
  if (verdict == NY_PROOF_VERDICT_PROVED)
    return rt_alloc_string_len("proved", 6);
  if (verdict == NY_PROOF_VERDICT_REFUTED) {
    char out[160];
    int n = snprintf(out, sizeof(out), "refuted:%s", cex);
    if (n < 0 || (size_t)n >= sizeof(out))
      return rt_alloc_string_len("unknown:counterexample oversized", 31);
    return rt_alloc_string_len(out, (size_t)n);
  }
  return rt_alloc_string_len("unknown:no backend could decide", 30);
}

/*
 * The obligation digest as 16 lowercase hex characters.  Certificates bind
 * to their obligation through this digest; the checker recomputes it from
 * the obligation text and never trusts the certificate's copy.
 */
int64_t rt_proof_policy_digest_hex(int64_t text_v) {
  const char *text = NULL;
  size_t text_len = 0;
  if (!rt_proof_string(text_v, &text, &text_len))
    return NY_IMM_NIL;
  char hex[17];
  ny_proof_policy_digest_hex(ny_proof_policy_digest_text(text), hex);
  return rt_alloc_string_len(hex, 16);
}

int64_t rt_proof_policy_check(int64_t obligation_v, int64_t certificate_v) {
  const char *obligation = NULL;
  const char *certificate = NULL;
  size_t obligation_len = 0, certificate_len = 0;
  char obligation_buf[NY_PROOF_OBLIGATION_MAX];
  char certificate_buf[NY_PROOF_CERTIFICATE_MAX];
  if (!rt_proof_string(obligation_v, &obligation, &obligation_len) ||
      obligation_len >= sizeof(obligation_buf))
    return NY_IMM_FALSE;
  memcpy(obligation_buf, obligation, obligation_len + 1);
  if (!rt_proof_string(certificate_v, &certificate, &certificate_len) ||
      certificate_len >= sizeof(certificate_buf))
    return NY_IMM_FALSE;
  memcpy(certificate_buf, certificate, certificate_len + 1);
  char reason[NY_PROOF_POLICY_EXPLAIN_MAX];
  bool ok = ny_proof_policy_certificate_check(obligation_buf, certificate_buf,
                                              reason, sizeof(reason));
  if (!ok && reason[0] && getenv("NYTRIX_PROOF_DEBUG")) {
    fprintf(stderr, "[proof debug] policy check rejected certificate: %s\n",
            reason);
  }
  return ok ? NY_IMM_TRUE : NY_IMM_FALSE;
}
