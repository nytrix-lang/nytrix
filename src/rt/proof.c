#include "rt/shared.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

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
  if (!is_v_str(value))
    return false;
  *text = (const char *)(uintptr_t)value;
  *len = rt_tagged_str_len(value);
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
  return (int64_t)(hash & UINT64_C(0x1fffffffffffffff));
}

int64_t rt_proof_cert_digest(int64_t canonical_v, int64_t module_version_v,
                             int64_t dependency_digest_v,
                             int64_t checker_version_v) {
  int64_t digest = rt_proof_digest_value(canonical_v, module_version_v,
                                         dependency_digest_v,
                                         checker_version_v);
  return digest < 0 ? NY_IMM_NIL : rt_tag_v(digest);
}

static bool rt_proof_parse_decimal(rt_proof_parser_t *p, size_t *value) {
  if (p->pos >= p->len || p->text[p->pos] < '0' || p->text[p->pos] > '9')
    return false;
  size_t out = 0;
  while (p->pos < p->len && p->text[p->pos] >= '0' &&
         p->text[p->pos] <= '9') {
    unsigned digit = (unsigned)(p->text[p->pos++] - '0');
    if (out > (SIZE_MAX - digit) / 10)
      return false;
    out = out * 10 + digit;
  }
  if (p->pos >= p->len || p->text[p->pos++] != ':')
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

static int rt_proof_parse_node(rt_proof_parser_t *p, int depth,
                               size_t end) {
  if (depth > p->max_depth || p->pos >= end ||
      p->node_count >= p->max_nodes)
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
  bool left = rt_proof_eval(nodes, node->left, assignment, steps, max_steps, ok);
  if (!*ok)
    return false;
  if (node->kind == 'N')
    return !left;
  bool right = rt_proof_eval(nodes, node->right, assignment, steps, max_steps, ok);
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
                            int64_t checker_version_v,
                            int64_t max_variables_v, int64_t max_nodes_v,
                            int64_t max_depth_v, int64_t max_steps_v,
                            int64_t max_memory_v) {
  if (!is_int(digest_v) || !is_int(max_variables_v) || !is_int(max_nodes_v) ||
      !is_int(max_depth_v) || !is_int(max_steps_v) || !is_int(max_memory_v))
    return NY_IMM_FALSE;
  int64_t expected = rt_proof_digest_value(
      canonical_v, module_version_v, dependency_digest_v, checker_version_v);
  if (expected < 0 || rt_untag_v(digest_v) != expected)
    return NY_IMM_FALSE;
  int64_t max_variables = rt_untag_v(max_variables_v);
  int64_t max_nodes = rt_untag_v(max_nodes_v);
  int64_t max_depth = rt_untag_v(max_depth_v);
  int64_t max_steps = rt_untag_v(max_steps_v);
  int64_t max_memory = rt_untag_v(max_memory_v);
  if (max_variables < 0 || max_variables > 20 || max_nodes <= 0 ||
      max_nodes > 1000000 || max_depth <= 0 || max_depth > 4096 ||
      max_steps <= 0 || max_memory <= 0 || max_nodes > max_memory)
    return NY_IMM_FALSE;
  const char *canonical = NULL;
  size_t canonical_len = 0;
  if (!rt_proof_string(canonical_v, &canonical, &canonical_len))
    return NY_IMM_FALSE;
  rt_proof_node_t *nodes = calloc((size_t)max_nodes, sizeof(*nodes));
  const char **atoms = calloc((size_t)(max_variables ? max_variables : 1),
                              sizeof(*atoms));
  size_t *atom_lens = calloc((size_t)(max_variables ? max_variables : 1),
                             sizeof(*atom_lens));
  if (!nodes || !atoms || !atom_lens) {
    free(nodes); free(atoms); free(atom_lens);
    return NY_IMM_FALSE;
  }
  rt_proof_parser_t parser = {
      canonical, canonical_len, 0, nodes, 0, (int)max_nodes,
      atoms, atom_lens, 0, (int)max_variables, (int)max_depth};
  int root = rt_proof_parse_node(&parser, 0, canonical_len);
  bool valid = root >= 0 && parser.pos == canonical_len;
  int64_t steps = 0;
  if (valid) {
    uint64_t assignments = UINT64_C(1) << parser.atom_count;
    for (uint64_t mask = 0; mask < assignments && valid; ++mask) {
      bool within_budget = true;
      valid = rt_proof_eval(nodes, root, mask, &steps, max_steps,
                            &within_budget) && within_budget;
    }
  }
  free(nodes); free(atoms); free(atom_lens);
  return valid ? NY_IMM_TRUE : NY_IMM_FALSE;
}
