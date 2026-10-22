/*
 * NYIR SSA builder: constructs static-single-assignment form from
 * lowered IR with dominance frontiers and phi-node insertion.
 */
#include "base/compat.h"
#include "base/debug.h"
#include "base/trace.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <limits.h>
#include <nyr/ir.h>
#include <stdlib.h>
#include <string.h>

/*
 * CFG construction deliberately lives with NYIR instead of a backend: SSA,
 * verifier tooling, and future allocators all need the same edge semantics.
 */
typedef struct {
  int64_t *keys;
  size_t *values;
  bool *used;
  size_t cap;
} nyr_label_map_t;

static size_t cfg_label_hash(int64_t label, size_t cap) {
  uint64_t x = (uint64_t)label;
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  x *= UINT64_C(0xc4ceb9fe1a85ec53);
  x ^= x >> 33;
  return (size_t)(x % cap);
}

static void cfg_label_map_free(nyr_label_map_t *map) {
  if (!map)
    return;
  free(map->keys);
  free(map->values);
  free(map->used);
  *map = (nyr_label_map_t){0};
}

static bool cfg_label_map_init(nyr_label_map_t *map, size_t count) {
  if (!map)
    return false;
  if (count > SIZE_MAX / 2)
    return false;
  size_t need = count * 2;
  size_t cap = 16;
  while (cap < need) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  if (cap > SIZE_MAX / sizeof(*map->keys) ||
      cap > SIZE_MAX / sizeof(*map->values)) {
    return false;
  }
  map->keys = ny_calloc_array(cap, sizeof(*map->keys));
  map->values = ny_calloc_array(cap, sizeof(*map->values));
  map->used = ny_calloc_array(cap, sizeof(*map->used));
  if (!map->keys || !map->values || !map->used) {
    cfg_label_map_free(map);
    return false;
  }
  map->cap = cap;
  return true;
}

static bool cfg_label_map_insert(nyr_label_map_t *map, int64_t label,
                                 size_t block) {
  if (!map || map->cap == 0)
    return false;
  size_t slot = cfg_label_hash(label, map->cap);
  for (size_t probe = 0; probe < map->cap; ++probe) {
    if (!map->used[slot]) {
      map->used[slot] = true;
      map->keys[slot] = label;
      map->values[slot] = block;
      return true;
    }
    if (map->keys[slot] == label)
      return false;
    slot = (slot + 1) % map->cap;
  }
  return false;
}

static int cfg_label_map_find(const nyr_label_map_t *map, int64_t label) {
  if (!map || map->cap == 0)
    return -1;
  size_t slot = cfg_label_hash(label, map->cap);
  for (size_t probe = 0; probe < map->cap; ++probe) {
    if (!map->used[slot])
      return -1;
    if (map->keys[slot] == label)
      return map->values[slot] <= INT_MAX ? (int)map->values[slot] : -1;
    slot = (slot + 1) % map->cap;
  }
  return -1;
}

static bool cfg_add_successor(size_t *successors, size_t *counts, size_t from,
                              size_t to) {
  if (!successors || !counts || counts[from] >= 2)
    return false;
  for (size_t i = 0; i < counts[from]; ++i)
    if (successors[from * 2 + i] == to)
      return true;
  successors[from * 2 + counts[from]++] = to;
  return true;
}

static bool cfg_dominates_bit(const nyr_cfg_t *cfg, size_t block,
                              size_t dominator) {
  if (!cfg || block >= cfg->block_count || dominator >= cfg->block_count ||
      !cfg->dominators || cfg->dominator_words == 0)
    return false;
  return (cfg->dominators[block * cfg->dominator_words + dominator / 64] &
          (UINT64_C(1) << (dominator % 64))) != 0;
}

static bool cfg_bit_get(const uint64_t *bits, size_t words, size_t row,
                        size_t column) {
  return bits && words &&
         (bits[row * words + column / 64] & (UINT64_C(1) << (column % 64))) !=
             0;
}

static void cfg_bit_set(uint64_t *bits, size_t words, size_t row,
                        size_t column) {
  bits[row * words + column / 64] |= UINT64_C(1) << (column % 64);
}

static void cfg_set_dominates_bit(nyr_cfg_t *cfg, size_t block,
                                  size_t dominator) {
  cfg->dominators[block * cfg->dominator_words + dominator / 64] |=
      UINT64_C(1) << (dominator % 64);
}

void nyr_cfg_free(nyr_cfg_t *cfg) {
  if (!cfg)
    return;
  free(cfg->block_start);
  free(cfg->block_end);
  free(cfg->block_label);
  free(cfg->inst_block);
  free(cfg->succ_offsets);
  free(cfg->succ_blocks);
  free(cfg->pred_offsets);
  free(cfg->pred_blocks);
  free(cfg->reachable);
  free(cfg->backedge_edges);
  free(cfg->dominators);
  free(cfg->frontiers);
  free(cfg->idom);
  *cfg = (nyr_cfg_t){0};
}

bool nyr_cfg_reverse_postorder(const nyr_cfg_t *cfg, size_t **out_blocks,
                                size_t *out_len) {
  if (!out_blocks || !out_len)
    return false;
  *out_blocks = NULL;
  *out_len = 0;
  if (!cfg || cfg->block_count == 0)
    return true;
  size_t count = cfg->block_count;
  bool *seen = ny_calloc_array(count, sizeof(*seen));
  size_t *stack = ny_malloc_array(count, sizeof(*stack));
  size_t *next_succ = ny_calloc_array(count, sizeof(*next_succ));
  size_t *post = ny_malloc_array(count, sizeof(*post));
  if (!seen || !stack || !next_succ || !post) {
    free(seen);
    free(stack);
    free(next_succ);
    free(post);
    return false;
  }
  size_t depth = 0;
  size_t post_len = 0;
  stack[depth++] = 0;
  seen[0] = true;
  while (depth > 0) {
    size_t block = stack[depth - 1];
    size_t next = next_succ[depth - 1];
    size_t succ_len = cfg->succ_offsets[block + 1] - cfg->succ_offsets[block];
    if (next < succ_len) {
      size_t succ = cfg->succ_blocks[cfg->succ_offsets[block] + next];
      next_succ[depth - 1] = next + 1;
      if (!seen[succ]) {
        seen[succ] = true;
        stack[depth] = succ;
        next_succ[depth] = 0;
        ++depth;
      }
      continue;
    }
    post[post_len++] = block;
    --depth;
  }
  size_t *rpo = ny_malloc_array(post_len, sizeof(*rpo));
  if (!rpo) {
    free(seen);
    free(stack);
    free(next_succ);
    free(post);
    return false;
  }
  for (size_t i = 0; i < post_len; ++i)
    rpo[i] = post[post_len - 1 - i];
  free(seen);
  free(stack);
  free(next_succ);
  free(post);
  *out_blocks = rpo;
  *out_len = post_len;
  return true;
}

static bool nyr_cfg_build_impl(const nyr_func_t *f, nyr_cfg_t *cfg,
                                bool need_dominance) {
  if (!f || !cfg)
    return false;
  nyr_cfg_free(cfg);
  size_t n = f->len;
  bool *boundary = ny_calloc_array(n + 1, sizeof(*boundary));
  if (!boundary)
    return false;
  boundary[0] = true;
  for (size_t i = 0; i < n; ++i) {
    if (f->data[i].op == NYIR_LABEL)
      boundary[i] = true;
    if ((f->data[i].op == NYIR_BR || f->data[i].op == NYIR_BR_IF ||
         f->data[i].op == NYIR_RET) &&
        i + 1 < n)
      boundary[i + 1] = true;
  }
  size_t blocks = 0;
  for (size_t i = 0; i < n; ++i)
    if (boundary[i])
      ++blocks;
  if (blocks == 0) {
    free(boundary);
    return true;
  }
  /*
   * CFG topology is bounded by two outgoing edges per block.
   */
  if (blocks > SIZE_MAX - 63 || blocks > SIZE_MAX / 2) {
    free(boundary);
    return false;
  }
  cfg->block_count = blocks;
  cfg->block_start = ny_calloc_array(blocks, sizeof(*cfg->block_start));
  cfg->block_end = ny_calloc_array(blocks, sizeof(*cfg->block_end));
  cfg->block_label = ny_malloc_array(blocks, sizeof(*cfg->block_label));
  cfg->inst_block = n ? ny_calloc_array(n, sizeof(*cfg->inst_block)) : NULL;
  cfg->reachable = ny_calloc_array(blocks, sizeof(*cfg->reachable));
  if (!cfg->block_start || !cfg->block_end || !cfg->block_label ||
      (n && !cfg->inst_block) || !cfg->reachable) {
    free(boundary);
    nyr_cfg_free(cfg);
    return false;
  }
  size_t b = 0;
  for (size_t i = 0; i < n; ++i)
    if (boundary[i])
      cfg->block_start[b++] = i;
  for (b = 0; b < blocks; ++b) {
    cfg->block_end[b] = b + 1 < blocks ? cfg->block_start[b + 1] : n;
    cfg->block_label[b] = f->data[cfg->block_start[b]].op == NYIR_LABEL
                              ? f->data[cfg->block_start[b]].imm
                              : -1;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i)
      cfg->inst_block[i] = b;
  }
  free(boundary);
  nyr_label_map_t labels = {0};
  if (!cfg_label_map_init(&labels, blocks)) {
    nyr_cfg_free(cfg);
    return false;
  }
  for (b = 0; b < blocks; ++b) {
    if (f->data[cfg->block_start[b]].op == NYIR_LABEL &&
        !cfg_label_map_insert(&labels, cfg->block_label[b], b)) {
      cfg_label_map_free(&labels);
      nyr_cfg_free(cfg);
      return false;
    }
  }
  size_t successor_cap = 0;
  if (!ny_size_mul_ok(blocks, 2, &successor_cap)) {
    cfg_label_map_free(&labels);
    nyr_cfg_free(cfg);
    return false;
  }
  size_t *successors = ny_calloc_array(successor_cap, sizeof(*successors));
  size_t *successor_counts = ny_calloc_array(blocks, sizeof(*successor_counts));
  if (!successors || !successor_counts) {
    free(successors);
    free(successor_counts);
    cfg_label_map_free(&labels);
    nyr_cfg_free(cfg);
    return false;
  }
  for (b = 0; b < blocks; ++b) {
    size_t end = nyr_block_last_non_nop(f, cfg, b);
    const nyr_inst_t *term =
        end > cfg->block_start[b] ? &f->data[end - 1] : NULL;
    if (term && (term->op == NYIR_BR || term->op == NYIR_BR_IF)) {
      int to = cfg_label_map_find(&labels, term->imm);
      if (to >= 0 &&
          !cfg_add_successor(successors, successor_counts, b, (size_t)to)) {
        free(successors);
        free(successor_counts);
        cfg_label_map_free(&labels);
        nyr_cfg_free(cfg);
        return false;
      }
    }
    if (!term || (term->op != NYIR_BR && term->op != NYIR_RET)) {
      if (b + 1 < blocks &&
          !cfg_add_successor(successors, successor_counts, b, b + 1)) {
        free(successors);
        free(successor_counts);
        cfg_label_map_free(&labels);
        nyr_cfg_free(cfg);
        return false;
      }
    }
  }
  cfg_label_map_free(&labels);
  cfg->succ_offsets = ny_calloc_array(blocks + 1, sizeof(*cfg->succ_offsets));
  cfg->pred_offsets = ny_calloc_array(blocks + 1, sizeof(*cfg->pred_offsets));
  if (!cfg->succ_offsets || !cfg->pred_offsets) {
    free(successors);
    free(successor_counts);
    nyr_cfg_free(cfg);
    return false;
  }
  for (size_t from = 0; from < blocks; ++from) {
    cfg->succ_offsets[from + 1] = successor_counts[from];
    for (size_t i = 0; i < successor_counts[from]; ++i)
      ++cfg->pred_offsets[successors[from * 2 + i] + 1];
  }
  for (size_t block = 0; block < blocks; ++block) {
    cfg->succ_offsets[block + 1] += cfg->succ_offsets[block];
    cfg->pred_offsets[block + 1] += cfg->pred_offsets[block];
  }
  size_t edge_count = cfg->succ_offsets[blocks];
  cfg->succ_blocks =
      edge_count ? ny_calloc_array(edge_count, sizeof(*cfg->succ_blocks))
                 : NULL;
  cfg->pred_blocks =
      edge_count ? ny_calloc_array(edge_count, sizeof(*cfg->pred_blocks))
                 : NULL;
  if (edge_count && (!cfg->succ_blocks || !cfg->pred_blocks)) {
    free(successors);
    free(successor_counts);
    nyr_cfg_free(cfg);
    return false;
  }
  size_t *pred_next = ny_malloc_array(blocks, sizeof(*pred_next));
  if (!pred_next) {
    free(successors);
    free(successor_counts);
    nyr_cfg_free(cfg);
    return false;
  }
  memcpy(pred_next, cfg->pred_offsets, blocks * sizeof(*pred_next));
  for (size_t from = 0; from < blocks; ++from)
    for (size_t i = 0; i < successor_counts[from]; ++i) {
      size_t to = successors[from * 2 + i];
      cfg->succ_blocks[cfg->succ_offsets[from] + i] = to;
      cfg->pred_blocks[pred_next[to]++] = from;
    }
  free(successors);
  free(successor_counts);
  free(pred_next);
  size_t *reach_work = ny_malloc_array(blocks, sizeof(*reach_work));
  if (!reach_work) {
    nyr_cfg_free(cfg);
    return false;
  }
  size_t reach_len = 0;
  cfg->reachable[0] = true;
  reach_work[reach_len++] = 0;
  for (size_t i = 0; i < reach_len; ++i) {
    size_t from = reach_work[i];
    for (size_t edge = cfg->succ_offsets[from];
         edge < cfg->succ_offsets[from + 1]; ++edge) {
      size_t to = cfg->succ_blocks[edge];
      if (!cfg->reachable[to]) {
        cfg->reachable[to] = true;
        reach_work[reach_len++] = to;
      }
    }
  }
  free(reach_work);
  if (!need_dominance)
    return true;
  cfg->dominator_words = (blocks + 63) / 64;
  cfg->frontier_words = cfg->dominator_words;
  if (cfg->dominator_words > SIZE_MAX / blocks) {
    nyr_cfg_free(cfg);
    return false;
  }
  cfg->dominators =
      ny_calloc_array(blocks * cfg->dominator_words, sizeof(*cfg->dominators));
  cfg->frontiers =
      ny_calloc_array(blocks * cfg->frontier_words, sizeof(*cfg->frontiers));
  cfg->idom = ny_malloc_array(blocks, sizeof(*cfg->idom));
  cfg->backedge_edges =
      edge_count ? ny_calloc_array(edge_count, sizeof(*cfg->backedge_edges))
                 : NULL;
  if (!cfg->dominators || !cfg->frontiers || !cfg->idom ||
      (edge_count && !cfg->backedge_edges)) {
    nyr_cfg_free(cfg);
    return false;
  }
  /*
   * Iterative packed-bitset dominators. This keeps the required dominance
   * relation while avoiding one byte per block-pair.
   */
  uint64_t *dom_row = ny_malloc_array(cfg->dominator_words, sizeof(*dom_row));
  if (!dom_row) {
    nyr_cfg_free(cfg);
    return false;
  }
  uint64_t last_mask =
      blocks % 64 ? (UINT64_C(1) << (blocks % 64)) - 1 : UINT64_MAX;
  for (b = 0; b < blocks; ++b) {
    uint64_t *row = &cfg->dominators[b * cfg->dominator_words];
    for (size_t word = 0; word < cfg->dominator_words; ++word)
      row[word] = b == 0 ? 0 : UINT64_MAX;
    row[cfg->dominator_words - 1] &= last_mask;
    if (b == 0)
      cfg_set_dominates_bit(cfg, b, b);
    cfg->idom[b] = -1;
  }
  bool changed;
  do {
    changed = false;
    for (b = 1; b < blocks; ++b) {
      bool have_pred = false;
      for (size_t word = 0; word < cfg->dominator_words; ++word)
        dom_row[word] = UINT64_MAX;
      dom_row[cfg->dominator_words - 1] &= last_mask;
      for (size_t edge = cfg->pred_offsets[b]; edge < cfg->pred_offsets[b + 1];
           ++edge) {
        size_t p = cfg->pred_blocks[edge];
        /*
         * Dead fall-through blocks (created after explicit terminators) are
         * unreachable with dom={self}; intersecting them would empty the
         * merge's dominator set and strand the merge with idom=-1.
         */
        if (!cfg->reachable[p])
          continue;
        if (!have_pred) {
          memcpy(dom_row, &cfg->dominators[p * cfg->dominator_words],
                 cfg->dominator_words * sizeof(*dom_row));
          have_pred = true;
        } else {
          for (size_t word = 0; word < cfg->dominator_words; ++word)
            dom_row[word] &= cfg->dominators[p * cfg->dominator_words + word];
        }
      }
      if (!have_pred) {
        memset(dom_row, 0, cfg->dominator_words * sizeof(*dom_row));
        dom_row[b / 64] |= UINT64_C(1) << (b % 64);
      } else {
        dom_row[b / 64] |= UINT64_C(1) << (b % 64);
      }
      uint64_t *row = &cfg->dominators[b * cfg->dominator_words];
      for (size_t word = 0; word < cfg->dominator_words; ++word)
        if (row[word] != dom_row[word]) {
          row[word] = dom_row[word];
          changed = true;
        }
    }
  } while (changed);
  /*
   * Strict dominators of one block form a chain in the dominator tree.
   * Therefore the immediate dominator is the strict dominator with the
   * deepest dominator set.  Count each row once instead of testing every
   * (block, candidate, ancestor) triple: the former cubic scan dominated
   * large branch-heavy functions.
   */
  size_t *dom_depth = ny_calloc_array(blocks, sizeof(*dom_depth));
  if (!dom_depth) {
    free(dom_row);
    nyr_cfg_free(cfg);
    return false;
  }
  for (size_t d = 0; d < blocks; ++d) {
    const uint64_t *row = &cfg->dominators[d * cfg->dominator_words];
    for (size_t word = 0; word < cfg->dominator_words; ++word) {
      uint64_t bits = row[word];
      while (bits) {
        bits &= bits - 1;
        ++dom_depth[d];
      }
    }
  }
  for (b = 1; b < blocks; ++b) {
    size_t best_depth = 0;
    const uint64_t *row = &cfg->dominators[b * cfg->dominator_words];
    for (size_t word = 0; word < cfg->dominator_words; ++word) {
      uint64_t bits = row[word];
      while (bits) {
#if SIZE_MAX == UINT64_MAX
        size_t bit = (size_t)__builtin_ctzll((unsigned long long)bits);
#else
        size_t bit = (size_t)__builtin_ctzl((unsigned long)bits);
#endif
        size_t d = word * 64 + bit;
        bits &= bits - 1;
        if (d >= blocks || d == b || dom_depth[d] <= best_depth)
          continue;
        cfg->idom[b] = (int)d;
        best_depth = dom_depth[d];
      }
    }
  }
  free(dom_depth);
  for (size_t pred = 0; pred < blocks; ++pred)
    for (size_t edge = cfg->succ_offsets[pred];
         edge < cfg->succ_offsets[pred + 1]; ++edge) {
      size_t succ = cfg->succ_blocks[edge];
      cfg->backedge_edges[edge] =
          pred != succ && cfg_dominates_bit(cfg, pred, succ);
    }
  for (b = 0; b < blocks; ++b) {
    size_t pred_count = cfg->pred_offsets[b + 1] - cfg->pred_offsets[b];
    if (pred_count < 2)
      continue;
    for (size_t edge = cfg->pred_offsets[b]; edge < cfg->pred_offsets[b + 1];
         ++edge) {
      size_t p = cfg->pred_blocks[edge];
      int runner = (int)p;
      while (runner >= 0 && runner != cfg->idom[b]) {
        cfg_bit_set(cfg->frontiers, cfg->frontier_words, (size_t)runner, b);
        runner = cfg->idom[runner];
      }
    }
  }
  free(dom_row);
  return true;
}

bool nyr_cfg_build(const nyr_func_t *f, nyr_cfg_t *cfg) {
  return nyr_cfg_build_impl(f, cfg, true);
}

bool nyr_cfg_build_topology(const nyr_func_t *f, nyr_cfg_t *cfg) {
  return nyr_cfg_build_impl(f, cfg, false);
}

bool nyr_prune_phis(nyr_func_t *f) {
  if (!f)
    return false;
  bool has_phi = false;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_PHI) {
      has_phi = true;
      break;
    }
  if (!has_phi)
    return true;
  nyr_cfg_t cfg = {0};
  /*
   * Phi pruning only needs block membership and predecessor edges. Building
   * dominators and frontiers here repeats expensive work that this pass never
   * queries, especially on generated CFGs with many join blocks.
   */
  if (!nyr_cfg_build_topology(f, &cfg))
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    size_t block = cfg.inst_block[i];
    size_t out = 0;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      int64_t label = phi->phi_incoming[k].predecessor_label;
      bool live = false;
      for (size_t e = cfg.pred_offsets[block]; e < cfg.pred_offsets[block + 1];
           ++e) {
        size_t pred = cfg.pred_blocks[e];
        if (cfg.block_label[pred] == label) {
          live = true;
          break;
        }
      }
      if (live)
        phi->phi_incoming[out++] = phi->phi_incoming[k];
    }
    if (out == 0) {
      nyr_cfg_free(&cfg);
      return false;
    }
    if (out == 1) {
      int src = phi->phi_incoming[0].value;
      free(phi->phi_incoming);
      nir_make_copy(phi, src);
      continue;
    }
    phi->phi_incoming_len = out;
  }
  nyr_cfg_free(&cfg);
  return true;
}

bool nyr_cfg_dominates(const nyr_cfg_t *cfg, size_t dominator, size_t block) {
  return cfg_dominates_bit(cfg, block, dominator);
}

bool nyr_cfg_is_backedge(const nyr_cfg_t *cfg, size_t predecessor,
                          size_t successor) {
  if (!cfg || predecessor >= cfg->block_count || successor >= cfg->block_count)
    return false;
  for (size_t edge = cfg->succ_offsets[predecessor];
       edge < cfg->succ_offsets[predecessor + 1]; ++edge)
    if (cfg->succ_blocks[edge] == successor)
      return cfg->backedge_edges[edge];
  return false;
}

bool nyr_cfg_natural_loop_blocks(const nyr_cfg_t *cfg, size_t latch,
                                  size_t header, bool *member,
                                  size_t member_count) {
  if (!cfg || !member || member_count < cfg->block_count ||
      latch >= cfg->block_count || header >= cfg->block_count ||
      !nyr_cfg_is_backedge(cfg, latch, header))
    return false;
  memset(member, 0, cfg->block_count * sizeof(*member));
  size_t *work = ny_malloc_array(cfg->block_count, sizeof(*work));
  if (!work)
    return false;
  size_t len = 0;
  member[header] = true;
  if (latch != header) {
    member[latch] = true;
    work[len++] = latch;
  }
  while (len) {
    size_t block = work[--len];
    for (size_t e = cfg->pred_offsets[block]; e < cfg->pred_offsets[block + 1];
         ++e) {
      size_t pred = cfg->pred_blocks[e];
      if (member[pred] || !nyr_cfg_dominates(cfg, header, pred))
        continue;
      member[pred] = true;
      if (pred != header)
        work[len++] = pred;
    }
  }
  free(work);
  return true;
}

static bool insert_inst(nyr_func_t *f, size_t at, nyr_inst_t in) {
  if (f->len == f->cap) {
    size_t cap = f->cap ? f->cap * 2 : 32;
    nyr_inst_t *data = ny_realloc_array(f->data, cap, sizeof(*data));
    if (!data)
      return false;
    f->data = data;
    f->cap = cap;
  }
  memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
  f->data[at] = in;
  ++f->len;
  return true;
}

static int64_t next_label(const nyr_func_t *f) {
  int64_t max = 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm >= max)
      max = f->data[i].imm + 1;
  return max;
}

/*
 * Find a zero in the entry block only. A function-wide constant search can
 * return a value from a branch that does not dominate the fallback use.
 */
static int entry_zero_value(const nyr_func_t *f, nyr_op_t op) {
  if (!f)
    return -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (i > 0 && in->op == NYIR_LABEL)
      break;
    if (in->op == NYIR_BR || in->op == NYIR_BR_IF || in->op == NYIR_RET)
      break;
    if (in->op == op && in->imm == 0 && in->dst >= 0)
      return in->dst;
  }
  return -1;
}

static int phi_at(const nyr_func_t *f, const nyr_cfg_t *cfg, size_t b,
                  int local) {
  size_t i = cfg->block_start[b];
  if (i < cfg->block_end[b] && f->data[i].op == NYIR_LABEL)
    ++i;
  for (; i < cfg->block_end[b] && f->data[i].op == NYIR_PHI; ++i)
    if ((f->data[i].flags & NYIR_INST_F_MEM2REG_PHI) && f->data[i].imm == local)
      return (int)i;
  return -1;
}
static int zero_for_local(const nyr_type_map_t *types, size_t local,
                          int zero_v, int zero_f64_v, int zero_f32_v) {
  if (types && types->local_f64 && types->local_f64[local])
    return zero_f64_v >= 0 ? zero_f64_v : zero_v;
  if (types && types->local_f32 && types->local_f32[local])
    return zero_f32_v >= 0 ? zero_f32_v : zero_v;
  return zero_v;
}

static bool phi_set_incoming(nyr_inst_t *phi, int64_t predecessor_label,
                             int value) {
  if (!phi || !phi->phi_incoming)
    return false;
  for (size_t i = 0; i < phi->phi_incoming_len; ++i) {
    if (phi->phi_incoming[i].predecessor_label != predecessor_label)
      continue;
    phi->phi_incoming[i].value = value;
    return true;
  }
  return false;
}

typedef struct {
  int *data;
  size_t len, cap;
} value_stack_t;
static bool stack_push(value_stack_t *s, int v) {
  if (s->len == s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 8;
    int *p = ny_realloc_array(s->data, cap, sizeof(*p));
    if (!p)
      return false;
    s->data = p;
    s->cap = cap;
  }
  s->data[s->len++] = v;
  return true;
}

/*
 * Each push belongs to the active dominator subtree. Undoing those pushes
 * on exit prevents a sibling's stores from becoming another sibling's facts.
 */
static bool rename_push(value_stack_t *stacks, size_t *undo, size_t *undo_len,
                        size_t local, int value) {
  if (!stack_push(&stacks[local], value))
    return false;
  undo[(*undo_len)++] = local;
  return true;
}

typedef struct {
  size_t block, undo_base;
  bool exit;
} rename_frame_t;

/*
 * Iterative dominator-tree DFS avoids both C recursion and a dense
 * block-by-local exit-value table. PHI operands are filled on each CFG edge
 * using its predecessor's state, including loop backedges.
 */
static bool rename_ssa(nyr_func_t *f, const nyr_cfg_t *cfg, bool *promote,
                       size_t locals, int zero_v, const nyr_type_map_t *types,
                       int zero_f64_v, int zero_f32_v) {
  if (!f || !cfg)
    return false;
  if (!cfg->block_count)
    return true;
  size_t frame_cap;
  if (!ny_size_mul_ok(cfg->block_count, 2, &frame_cap))
    return false;
  size_t *first_child = ny_malloc_array(cfg->block_count, sizeof(*first_child));
  size_t *next_child = ny_malloc_array(cfg->block_count, sizeof(*next_child));
  size_t *undo = ny_malloc_array(f->len ? f->len : 1, sizeof(*undo));
  rename_frame_t *frames = ny_malloc_array(frame_cap, sizeof(*frames));
  value_stack_t *stacks = ny_calloc_array(locals, sizeof(*stacks));
  if (!first_child || !next_child || !undo || !frames || (locals && !stacks)) {
    free(first_child);
    free(next_child);
    free(undo);
    free(frames);
    free(stacks);
    return false;
  }
  for (size_t b = 0; b < cfg->block_count; ++b)
    first_child[b] = next_child[b] = SIZE_MAX;
  for (size_t b = 1; b < cfg->block_count; ++b) {
    int parent = cfg->idom ? cfg->idom[b] : -1;
    if (parent < 0)
      continue;
    next_child[b] = first_child[parent];
    first_child[parent] = b;
  }
  size_t frame_len = 1, undo_len = 0;
  frames[0] = (rename_frame_t){.block = 0};
  bool ok = true;
  while (frame_len && ok) {
    rename_frame_t frame = frames[--frame_len];
    size_t b = frame.block;
    if (frame.exit) {
      while (undo_len > frame.undo_base)
        --stacks[undo[--undo_len]].len;
      continue;
    }
    size_t undo_base = undo_len;
    size_t start = cfg->block_start[b], end = cfg->block_end[b];
    for (size_t i = start; i < end && ok; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_PHI && (in->flags & NYIR_INST_F_MEM2REG_PHI) &&
          in->imm >= 0 && (size_t)in->imm < locals && promote[in->imm]) {
        if (!rename_push(stacks, undo, &undo_len, in->imm, in->dst))
          ok = false;
      } else if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < locals && promote[in->imm]) {
        if (in->a < 0 || !rename_push(stacks, undo, &undo_len, in->imm, in->a))
          ok = false;
        else
          nyr_inst_discard(in);
      } else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < locals && promote[in->imm]) {
        if (stacks[in->imm].len) {
          nir_make_copy(in, stacks[in->imm].data[stacks[in->imm].len - 1]);
        } else if ((size_t)in->imm < f->param_count) {
          if (!rename_push(stacks, undo, &undo_len, in->imm, in->dst))
            ok = false;
        } else {
          int zero_for_l = zero_for_local(types, (size_t)in->imm, zero_v,
                                          zero_f64_v, zero_f32_v);
          nir_make_copy(in, zero_for_l);
        }
      }
    }
    if (!ok)
      break;

    /*
     * Fill PHI operands on successors.
     */
    for (size_t edge = cfg->succ_offsets[b];
         edge < cfg->succ_offsets[b + 1] && ok; ++edge) {
      size_t s = cfg->succ_blocks[edge];
      for (size_t l = 0; l < locals; ++l) {
        if (!promote[l])
          continue;
        int pi = phi_at(f, cfg, s, (int)l);
        if (pi < 0)
          continue;
        int zero_for_l =
            zero_for_local(types, l, zero_v, zero_f64_v, zero_f32_v);
        int val =
            stacks[l].len ? stacks[l].data[stacks[l].len - 1] : zero_for_l;
        if (!phi_set_incoming(&f->data[pi], cfg->block_label[b], val)) {
          if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
            ny_dbg_heuristic_event(
                "MEM2REG_PHI",
                "rename failed: local=%zu pred_block=%zu pred_label=%lld "
                "successor_block=%zu phi_inst=%d incoming=%zu",
                l, b, (long long)cfg->block_label[b], s, pi,
                f->data[pi].phi_incoming_len);
          ok = false;
          break;
        }
      }
    }
    if (!ok)
      break;
    frames[frame_len++] =
        (rename_frame_t){.block = b, .undo_base = undo_base, .exit = true};
    for (size_t child = first_child[b]; child != SIZE_MAX;
         child = next_child[child])
      frames[frame_len++] = (rename_frame_t){.block = child};
  }
  /*
   * Dominator DFS starts at entry and intentionally skips unreachable code.
   * Such blocks can still appear as syntactic predecessors in dead tails, so
   * complete their PHI slots with a typed zero. Their values are unobservable
   * at runtime, while the verifier still requires a well formed incoming set.
   */
  if (ok)
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI || !(phi->flags & NYIR_INST_F_MEM2REG_PHI) ||
          phi->imm < 0 || (size_t)phi->imm >= locals)
        continue;
      int zero = zero_for_local(types, (size_t)phi->imm, zero_v,
                                zero_f64_v, zero_f32_v);
      for (size_t k = 0; k < phi->phi_incoming_len; ++k)
        if (phi->phi_incoming[k].value < 0)
          phi->phi_incoming[k].value = zero;
    }
  for (size_t l = 0; l < locals; ++l)
    free(stacks[l].data);
  free(stacks);
  free(undo);
  free(frames);
  free(first_child);
  free(next_child);
  return ok;
}

/*
 * Nested promotion is enabled by default after the full downstream parity
 * matrix passed. NYTRIX_NESTED_MEM2REG=0 retains a per-run rollback switch.
 */
static bool nyr_nested_mem2reg_enabled = true;
static bool nyr_nested_mem2reg_env_read;
void nyr_set_nested_mem2reg_enabled(bool enabled) {
  nyr_nested_mem2reg_enabled = enabled;
}
static bool nyr_nested_mem2reg_active(void) {
  if (!nyr_nested_mem2reg_env_read) {
    nyr_nested_mem2reg_env_read = true;
    const char *v = getenv("NYTRIX_NESTED_MEM2REG");
    if (v && *v)
      nyr_nested_mem2reg_enabled = strcmp(v, "0") != 0;
  }
  return nyr_nested_mem2reg_enabled;
}

bool nyr_mem2reg(nyr_func_t *f) {
  if (!f)
    return false;
  size_t conditional_branches = 0;
  for (size_t i = 0; i < f->len; ++i)
    conditional_branches += f->data[i].op == NYIR_BR_IF;
  if (conditional_branches > 1 && !nyr_nested_mem2reg_active()) {
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event(
          "MEM2REG",
          "rejected conditional-branches=%zu: downstream nested-SSA parity "
          "unproven; locals remain in memory",
          conditional_branches);
    return true;
  }
  nyr_func_t backup = {0};
  if (!nyr_ir_func_clone(f, &backup))
    return false;
  /*
   * Give entry a concrete predecessor label for phi metadata.
   */
  if (f->len && f->data[0].op != NYIR_LABEL) {
    nyr_inst_t label = {
        .op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1, .imm = next_label(f)};
    if (!insert_inst(f, 0, label)) {
      nyr_ir_func_free(&backup);
      return false;
    }
  }
  /*
   * The undefined-local fallback is an IR zero, and every PHI predecessor
   * must be dominated by its definition. A function-wide first-zero search
   * is invalid when that constant lives in a later branch or loop body. Keep
   * a dedicated canonical zero in the entry block.
   */
  int zero_v = entry_zero_value(f, NYIR_CONST_I64);
  if (zero_v < 0) {
    zero_v = f->next_value++;
    nyr_inst_t zero_inst = {
        .op = NYIR_CONST_I64, .dst = zero_v, .a = -1, .b = -1, .imm = 0};
    size_t entry_at = (f->len && f->data[0].op == NYIR_LABEL) ? 1 : 0;
    if (!insert_inst(f, entry_at, zero_inst)) {
      nyr_ir_func_free(&backup);
      return false;
    }
  }
  int zero_f64_v = -1;
  int zero_f32_v = -1;
  /*
   * A parameter is defined on entry even if its first source load is at a
   * join. Seed its incoming value before renaming so an untouched predecessor
   * cannot acquire the undefined-local zero fallback. Preserve the original
   * load's representation rather than guessing it from the source type.
   */
  for (size_t local = 0; local < f->param_count; ++local) {
    bool stored = false, addressed = false, loaded = false;
    nyr_inst_t seed = {0};
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->imm != (int64_t)local)
        continue;
      if (in->op == NYIR_STORE_LOCAL)
        stored = true;
      else if (in->op == NYIR_ADDR_LOCAL)
        addressed = true;
      else if (in->op == NYIR_LOAD_LOCAL && !loaded) {
        seed = *in;
        loaded = true;
      }
    }
    if (!stored || addressed || !loaded)
      continue;
    seed.dst = f->next_value++;
    size_t at = (f->len && f->data[0].op == NYIR_LABEL) ? 1 : 0;
    if (!insert_inst(f, at, seed)) {
      nyr_ir_func_free(&backup);
      return false;
    }
  }
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg)) {
    nyr_ir_func_free(&backup);
    return false;
  }
  /*
   * Every block that can feed a PHI needs a stable label.  Loop bodies and
   * other fall-through regions may be CFG blocks without a LABEL in the
   * instruction stream; assign fresh labels before PHI placement.
   */
  for (size_t b = cfg.block_count; b-- > 0;) {
    if (cfg.block_label[b] >= 0)
      continue;
    nyr_inst_t label = {
        .op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1, .imm = next_label(f)};
    if (!insert_inst(f, cfg.block_start[b], label)) {
      nyr_cfg_free(&cfg);
      return false;
    }
  }
  if (cfg.block_count > 0) {
    nyr_cfg_free(&cfg);
    if (!nyr_cfg_build(f, &cfg))
      return false;
  }
  size_t locals = 0;
  for (size_t i = 0; i < f->len; ++i)
    if ((f->data[i].op == NYIR_LOAD_LOCAL ||
         f->data[i].op == NYIR_STORE_LOCAL ||
         f->data[i].op == NYIR_ADDR_LOCAL) &&
        f->data[i].imm >= 0 && (size_t)f->data[i].imm + 1 > locals)
      locals = (size_t)f->data[i].imm + 1;
  nyr_type_map_t types = {0};
  bool *promote = NULL, *addr = NULL, *has_load = NULL, *has_store = NULL;
  if (!nyr_type_map_init(&types, f, locals))
    goto oom;
  promote = ny_calloc_array(locals, sizeof(*promote));
  addr = ny_calloc_array(locals, sizeof(*addr));
  has_load = ny_calloc_array(locals, sizeof(*has_load));
  has_store = ny_calloc_array(locals, sizeof(*has_store));
  if ((locals && (!promote || !addr || !has_load || !has_store)))
    goto oom;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->imm < 0 || (size_t)in->imm >= locals)
      continue;
    if (in->op == NYIR_ADDR_LOCAL)
      addr[in->imm] = true;
    else if (in->op == NYIR_LOAD_LOCAL)
      has_load[in->imm] = true;
    else if (in->op == NYIR_STORE_LOCAL)
      has_store[in->imm] = true;
  }
  /*
   * Any non-address-taken local with both loads and stores can be
   * promoted: Cytron PHI placement at dominance frontiers handles loop
   * back-edges and join points correctly.  The old per-load dominator
   * check incorrectly rejected every loop induction variable (back-edge
   * stores never dominate the loop header), forcing them to live in stack
   * memory across every iteration.
   */
  bool has_f64_promote = false;
  bool has_f32_promote = false;
  size_t requested_promotions = 0;
  for (size_t l = 0; l < locals; ++l) {
    if (!addr[l] && has_load[l] && has_store[l]) {
      promote[l] = true;
      requested_promotions++;
      if (types.local_f64 && types.local_f64[l])
        has_f64_promote = true;
      if (types.local_f32 && types.local_f32[l])
        has_f32_promote = true;
    }
  }
  if (has_f64_promote) {
    zero_f64_v = entry_zero_value(f, NYIR_CONST_F64);
    if (zero_f64_v < 0) {
      zero_f64_v = f->next_value++;
      nyr_inst_t zero_f64_inst = {
          .op = NYIR_CONST_F64, .dst = zero_f64_v, .a = -1, .b = -1, .imm = 0};
      size_t at = (f->len && f->data[0].op == NYIR_LABEL) ? 1 : 0;
      if (!insert_inst(f, at, zero_f64_inst)) {
        free(promote);
        free(addr);
        free(has_load);
        free(has_store);
        nyr_type_map_free(&types);
        nyr_cfg_free(&cfg);
        nyr_ir_func_free(&backup);
        return false;
      }
    }
  }
  if (has_f32_promote) {
    zero_f32_v = entry_zero_value(f, NYIR_CONST_F32);
    if (zero_f32_v < 0) {
      zero_f32_v = f->next_value++;
      nyr_inst_t zero_f32_inst = {
          .op = NYIR_CONST_F32, .dst = zero_f32_v, .a = -1, .b = -1, .imm = 0};
      size_t at = (f->len && f->data[0].op == NYIR_LABEL) ? 1 : 0;
      if (!insert_inst(f, at, zero_f32_inst)) {
        free(promote);
        free(addr);
        free(has_load);
        free(has_store);
        nyr_type_map_free(&types);
        nyr_cfg_free(&cfg);
        nyr_ir_func_free(&backup);
        return false;
      }
    }
  }
  /*
   * Pruned Cytron SSA: only place a PHI at a dominance frontier where this
   * local is live-in. This avoids join values that cannot be observed.
   */
  for (size_t l = 0; l < locals; ++l)
    if (promote[l]) {
      size_t n = cfg.block_count;
      bool *defs = ny_calloc_array(n, sizeof(*defs));
      bool *uses = ny_calloc_array(n, sizeof(*uses));
      bool *live_in = ny_calloc_array(n, sizeof(*live_in));
      bool *live_out = ny_calloc_array(n, sizeof(*live_out));
      bool *placed = ny_calloc_array(n, sizeof(*placed));
      bool *work = ny_calloc_array(n, sizeof(*work));
      if (!defs || !uses || !live_in || !live_out || !placed || !work) {
        free(defs);
        free(uses);
        free(live_in);
        free(live_out);
        free(placed);
        free(work);
        goto oom;
      }
      for (size_t b = 0; b < n; ++b) {
        bool seen_def = false;
        for (size_t i = cfg.block_start[b]; i < cfg.block_end[b]; ++i) {
          const nyr_inst_t *in = &f->data[i];
          if (in->op == NYIR_LOAD_LOCAL && in->imm == (int64_t)l && !seen_def)
            uses[b] = true;
          if (in->op == NYIR_STORE_LOCAL && in->imm == (int64_t)l) {
            defs[b] = true;
            seen_def = true;
          }
        }
        work[b] = defs[b];
      }
      bool changed;
      do {
        changed = false;
        for (size_t b = n; b-- > 0;) {
          bool out = false;
          for (size_t edge = cfg.succ_offsets[b];
               edge < cfg.succ_offsets[b + 1]; ++edge)
            out |= live_in[cfg.succ_blocks[edge]];
          bool in = uses[b] || (out && !defs[b]);
          if (out != live_out[b] || in != live_in[b]) {
            live_out[b] = out;
            live_in[b] = in;
            changed = true;
          }
        }
      } while (changed);
      bool more;
      do {
        more = false;
        for (size_t x = 0; x < n; ++x)
          if (work[x]) {
            work[x] = false;
            for (size_t y = 0; y < n; ++y)
              if (cfg_bit_get(cfg.frontiers, cfg.frontier_words, x, y) &&
                  live_in[y] && !placed[y]) {
                placed[y] = true;
                if (!defs[y])
                  work[y] = true;
                more = true;
              }
          }
      } while (more);
      for (size_t b = n; b-- > 0;)
        if (placed[b]) {
          size_t pred_count = cfg.pred_offsets[b + 1] - cfg.pred_offsets[b];
          /*
           * A dominance-frontier PHI must name every incoming CFG edge before
           * rename_ssa assigns its value. Leaving the list empty makes the
           * rename silently fail at the first predecessor and rolls back all
           * otherwise-safe promotions, including loop induction variables.
           */
          if (pred_count == 0)
            continue;
          size_t at = cfg.block_start[b];
          if (f->data[at].op == NYIR_LABEL)
            ++at;
          nyr_inst_t phi = {
              .op = NYIR_PHI,
              .dst = f->next_value++,
              .a = -1,
              .b = -1,
              .imm = (int64_t)l,
              .flags = NYIR_INST_F_MEM2REG_PHI,
              .scalar_class =
                  types.local_f64 && types.local_f64[l]   ? NYIR_SCALAR_F64
                  : types.local_f32 && types.local_f32[l] ? NYIR_SCALAR_F32
                                                          : NYIR_SCALAR_UNKNOWN,
              .phi_incoming_len = pred_count};
          phi.phi_incoming =
              ny_calloc_array(pred_count, sizeof(*phi.phi_incoming));
          if (!phi.phi_incoming) {
            free(defs);
            free(uses);
            free(live_in);
            free(live_out);
            free(placed);
            free(work);
            goto oom;
          }
          for (size_t edge = 0; edge < pred_count; ++edge) {
            size_t pred = cfg.pred_blocks[cfg.pred_offsets[b] + edge];
            if (cfg.block_label[pred] < 0) {
              free(phi.phi_incoming);
              free(defs);
              free(uses);
              free(live_in);
              free(live_out);
              free(placed);
              free(work);
              goto oom;
            }
            phi.phi_incoming[edge] = (nyr_phi_incoming_t){
                .predecessor_label = cfg.block_label[pred], .value = -1};
          }
          if (!insert_inst(f, at, phi)) {
            free(phi.phi_incoming);
            free(defs);
            free(uses);
            free(live_in);
            free(live_out);
            free(placed);
            free(work);
            goto oom;
          }
        }
      free(defs);
      free(uses);
      free(live_in);
      free(live_out);
      free(placed);
      free(work);
      /*
       * Insertions shift instruction indices; rebuild before considering the
       * next local rather than reusing stale block boundaries.
       */
      nyr_cfg_free(&cfg);
      if (!nyr_cfg_build(f, &cfg))
        goto oom;
    }
  nyr_cfg_free(&cfg);
  if (!nyr_cfg_build(f, &cfg))
    goto oom;
  bool ok = rename_ssa(f, &cfg, promote, locals, zero_v, &types, zero_f64_v,
                       zero_f32_v);
  if (ok)
    for (size_t i = 0; i < f->len; ++i)
      f->data[i].flags &= ~NYIR_INST_F_MEM2REG_PHI;
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC) && requested_promotions)
    ny_dbg_heuristic_event("MEM2REG",
                           "attempt locals=%zu promoted=%zu rename=%s", locals,
                           requested_promotions, ok ? "ok" : "failed");
  free(promote);
  free(addr);
  free(has_load);
  free(has_store);
  nyr_type_map_free(&types);
  nyr_cfg_free(&cfg);
  if (ok) {
    nyr_refresh_metadata(f);
    char verify_err[256] = {0};
    if (nyr_ir_verify(f, verify_err, sizeof(verify_err))) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC) && requested_promotions)
        ny_dbg_heuristic_event("MEM2REG", "committed promoted=%zu",
                               requested_promotions);
      nyr_ir_func_free(&backup);
      return true;
    }
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event(
          "MEM2REG", "rollback: generated SSA failed verification: %s",
          verify_err[0] ? verify_err : "unspecified verifier failure");
    /*
     * Unsupported CFG shapes must retain local memory form. Returning false
     * means allocation failure to the pass runner, not "no transformation".
     */
    nyr_ir_func_free(f);
    *f = backup;
    return true;
  }
  nyr_ir_func_free(f);
  *f = backup;
  return true;
oom:
  free(promote);
  free(addr);
  free(has_load);
  free(has_store);
  nyr_type_map_free(&types);
  nyr_cfg_free(&cfg);
  nyr_ir_func_free(f);
  *f = backup;
  return false;
}

/*
 * MemorySSA for NYIR locals (and a single global memory chain).
 *
 * Each STORE_LOCAL creates a new version of that local. LOAD_LOCAL
 * reads the dominating version. CALL / unknown memory ops clobber the
 * global chain and, conservatively, all locals that may escape.
 *
 * Forwarding rewrites LOADs to COPYs of the store value when the
 * version is a concrete store in the same straight-line region (or
 * after single-pred labels without intervening clobber). Multi-pred
 * joins clobber until full memory PHIs land.
 */
typedef struct {
  int *local_def; /* value id that last stored each local, or -1 */
  size_t local_n;
  int mem_def; /* last general memory def (for NYIR_STORE_I64) */
} ny_mssa_state_t;

bool nyr_memory_ssa_forward(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;
  size_t local_n = nyr_max_local(f);
  if (!local_n)
    return true;

  /*
   * A local whose address was taken (ADDR_LOCAL) may be written through a
   * general memory store (STORE_I64).  Any such store invalidates every
   * address-taken local's forwarded value; without this, a LOAD_LOCAL after
   * a pointer store would be rewritten to the stale pre-store value.
   * addr_taken is function-wide (not per-encounter) so a STORE_I64 that
   * appears before its ADDR_LOCAL in program order still clobbers it.
   */
  bool *addr_taken = calloc(local_n, sizeof(bool));
  if (!addr_taken)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_ADDR_LOCAL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < local_n)
      addr_taken[f->data[i].imm] = true;
  }

  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg) || cfg.block_count == 0) {
    nyr_cfg_free(&cfg);
    /*
     * Fall back: straight-line only forwarding.
     */
    int *def = ny_malloc_array(local_n, sizeof(int));
    if (!def) {
      free(addr_taken);
      return false;
    }
    for (size_t i = 0; i < local_n; ++i)
      def[i] = -1;
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_LABEL || in->op == NYIR_BR_IF || in->op == NYIR_CALL) {
        for (size_t l = 0; l < local_n; ++l)
          def[l] = -1;
      } else if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < local_n) {
        def[in->imm] = -1;
      } else if (in->op == NYIR_STORE_I64) {
        for (size_t l = 0; l < local_n; ++l)
          if (addr_taken[l])
            def[l] = -1;
      } else if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < local_n && in->a >= 0) {
        def[in->imm] = in->a;
      } else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < local_n && def[in->imm] >= 0 &&
                 in->dst >= 0) {
        nir_make_copy(in, def[in->imm]);
      }
    }
    free(def);
    free(addr_taken);
    return true;
  }

  /*
   * This forwarder computes joins in one RPO walk; loop-carried definitions
   * need an iterative MemorySSA fixed point instead.  Keep those locals in
   * memory rather than forwarding a body-only PHI into a loop exit.
   */
  for (size_t b = 0; b < cfg.block_count; ++b) {
    for (size_t edge = cfg.succ_offsets[b]; edge < cfg.succ_offsets[b + 1];
         ++edge) {
      if (nyr_cfg_is_backedge(&cfg, b, cfg.succ_blocks[edge])) {
        nyr_cfg_free(&cfg);
        free(addr_taken);
        return true;
      }
    }
  }

  /*
   * exit_def[b*local_n + l] = value id of local l at end of block b, or -1.
   */
  size_t exit_def_count = 0;
  if (!ny_size_mul_ok(cfg.block_count, local_n, &exit_def_count)) {
    free(addr_taken);
    nyr_cfg_free(&cfg);
    return false;
  }
  int *exit_def = ny_malloc_array(exit_def_count, sizeof(*exit_def));
  int *cur = ny_malloc_array(local_n, sizeof(*cur));
  size_t *rpo = NULL;
  size_t rpo_n = 0;
  if (!exit_def || !cur || !nyr_cfg_reverse_postorder(&cfg, &rpo, &rpo_n)) {
    free(exit_def);
    free(cur);
    free(rpo);
    free(addr_taken);
    nyr_cfg_free(&cfg);
    return false;
  }
  for (size_t i = 0; i < exit_def_count; ++i)
    exit_def[i] = -1;
  /*
   * Pass 1: compute exit_def with same-value / const-equal joins.
   */
  for (size_t ri = 0; ri < rpo_n; ++ri) {
    size_t b = rpo[ri];
    size_t pc = cfg.pred_offsets[b + 1] - cfg.pred_offsets[b];
    for (size_t l = 0; l < local_n; ++l) {
      int agreed = -2;
      if (pc == 0) {
        cur[l] = -1;
        continue;
      }
      int p0 = -1, p1 = -1;
      size_t n_def = 0;
      for (size_t e = cfg.pred_offsets[b]; e < cfg.pred_offsets[b + 1]; ++e) {
        size_t p = cfg.pred_blocks[e];
        int v = exit_def[p * local_n + l];
        if (v >= 0) {
          if (n_def == 0)
            p0 = v;
          else if (n_def == 1)
            p1 = v;
          n_def++;
        }
        if (agreed == -2)
          agreed = v;
        else if (agreed != v)
          agreed = -1;
      }
      if (agreed == -1 && n_def == 2 && p0 >= 0 && p1 >= 0 && p0 != p1) {
        int64_t c0 = 0, c1 = 1;
        bool k0 = false, k1 = false;
        for (size_t i = 0; i < f->len; ++i) {
          if (f->data[i].op == NYIR_CONST_I64 && f->data[i].dst == p0) {
            k0 = true;
            c0 = f->data[i].imm;
          }
          if (f->data[i].op == NYIR_CONST_I64 && f->data[i].dst == p1) {
            k1 = true;
            c1 = f->data[i].imm;
          }
        }
        if (k0 && k1 && c0 == c1)
          agreed = p0;
      }
      cur[l] = agreed == -2 ? -1 : agreed;
    }
    for (size_t i = cfg.block_start[b]; i < cfg.block_end[b]; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_CALL) {
        for (size_t l = 0; l < local_n; ++l)
          cur[l] = -1;
      } else if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < local_n) {
        cur[in->imm] = -1;
      } else if (in->op == NYIR_STORE_I64) {
        for (size_t l = 0; l < local_n; ++l)
          if (addr_taken[l])
            cur[l] = -1;
      } else if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < local_n && in->a >= 0) {
        cur[in->imm] = in->a;
      }
    }
    for (size_t l = 0; l < local_n; ++l)
      exit_def[b * local_n + l] = cur[l];
  }

  /*
   * Pass 2: insert real PHIs when every pred has a concrete def, all pred
   * labels are known, and values disagree. Skip blocks that already have a
   * PHI for that local (mem2reg). Process high→low so indices stay valid.
   */
  for (size_t bi = cfg.block_count; bi-- > 0;) {
    size_t pc = cfg.pred_offsets[bi + 1] - cfg.pred_offsets[bi];
    if (pc < 2 || cfg.block_label[bi] < 0)
      continue;
    bool labels_ok = true;
    for (size_t e = cfg.pred_offsets[bi]; e < cfg.pred_offsets[bi + 1]; ++e)
      if (cfg.block_label[cfg.pred_blocks[e]] < 0)
        labels_ok = false;
    if (!labels_ok)
      continue;
    for (size_t l = 0; l < local_n; ++l) {
      bool all_def = true;
      int first = -2;
      bool disagree = false;
      for (size_t e = cfg.pred_offsets[bi]; e < cfg.pred_offsets[bi + 1]; ++e) {
        int v = exit_def[cfg.pred_blocks[e] * local_n + l];
        if (v < 0) {
          all_def = false;
          break;
        }
        if (first == -2)
          first = v;
        else if (first != v)
          disagree = true;
      }
      if (!all_def || !disagree)
        continue;
      size_t at = cfg.block_start[bi];
      if (at < cfg.block_end[bi] && f->data[at].op == NYIR_LABEL)
        at++;
      bool have = false;
      for (size_t i = at; i < cfg.block_end[bi] && f->data[i].op == NYIR_PHI;
           ++i)
        if (f->data[i].imm == (int64_t)l) {
          have = true;
          break;
        }
      if (have)
        continue;
      int phi_dst = f->next_value++;
      nyr_inst_t phi = {
          .op = NYIR_PHI, .dst = phi_dst, .a = -1, .b = -1, .imm = (int64_t)l};
      phi.phi_incoming_len = pc;
      phi.phi_incoming = ny_calloc_array(pc, sizeof(*phi.phi_incoming));
      if (!phi.phi_incoming) {
        free(exit_def);
        free(cur);
        free(rpo);
        free(addr_taken);
        nyr_cfg_free(&cfg);
        return false;
      }
      size_t pi = 0;
      for (size_t e = cfg.pred_offsets[bi]; e < cfg.pred_offsets[bi + 1]; ++e) {
        size_t p = cfg.pred_blocks[e];
        phi.phi_incoming[pi].predecessor_label = cfg.block_label[p];
        phi.phi_incoming[pi].value = exit_def[p * local_n + l];
        pi++;
      }
      if (!insert_inst(f, at, phi)) {
        free(phi.phi_incoming);
        free(exit_def);
        free(cur);
        free(rpo);
        free(addr_taken);
        nyr_cfg_free(&cfg);
        return false;
      }
      for (size_t bb = 0; bb < cfg.block_count; ++bb) {
        if (cfg.block_start[bb] > at)
          cfg.block_start[bb]++;
        if (cfg.block_end[bb] > at)
          cfg.block_end[bb]++;
      }
      if (cfg.block_end[bi] <= at)
        cfg.block_end[bi] = at + 1;
      else
        cfg.block_end[bi]++;
      /*
       * Seed exit_def so successors of this block see the PHI.
       */
      exit_def[bi * local_n + l] = phi_dst;
    }
  }

  /*
   * Pass 3: forward loads using joins + PHIs.
   */
  for (size_t ri = 0; ri < rpo_n; ++ri) {
    size_t b = rpo[ri];
    size_t pc = cfg.pred_offsets[b + 1] - cfg.pred_offsets[b];
    for (size_t l = 0; l < local_n; ++l) {
      int agreed = -2;
      if (pc == 0) {
        cur[l] = -1;
        continue;
      }
      for (size_t e = cfg.pred_offsets[b]; e < cfg.pred_offsets[b + 1]; ++e) {
        int v = exit_def[cfg.pred_blocks[e] * local_n + l];
        if (agreed == -2)
          agreed = v;
        else if (agreed != v)
          agreed = -1;
      }
      cur[l] = agreed == -2 ? -1 : agreed;
    }
    for (size_t i = cfg.block_start[b]; i < cfg.block_end[b]; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_PHI && in->imm >= 0 && (size_t)in->imm < local_n &&
          in->dst >= 0) {
        cur[in->imm] = in->dst;
      } else if (in->op == NYIR_CALL) {
        for (size_t l = 0; l < local_n; ++l)
          cur[l] = -1;
      } else if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < local_n) {
        cur[in->imm] = -1;
      } else if (in->op == NYIR_STORE_I64) {
        for (size_t l = 0; l < local_n; ++l)
          if (addr_taken[l])
            cur[l] = -1;
      } else if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < local_n && in->a >= 0) {
        cur[in->imm] = in->a;
      } else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
                 (size_t)in->imm < local_n && cur[in->imm] >= 0 &&
                 in->dst >= 0) {
        nir_make_copy(in, cur[in->imm]);
      }
    }
    for (size_t l = 0; l < local_n; ++l)
      exit_def[b * local_n + l] = cur[l];
  }

  free(exit_def);
  free(cur);
  free(rpo);
  free(addr_taken);
  nyr_cfg_free(&cfg);
  return true;
}
