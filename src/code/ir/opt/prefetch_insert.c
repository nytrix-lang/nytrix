/*
 * Software Prefetch Insertion: identify hot counted loops containing
 * loads with data-dependent addresses (pointer chasing) or strided
 * access, and insert prefetch_read calls with appropriate locality
 * hints to hide memory latency.
 *
 * References:
 *  Mowry — "Tolerating Memory Latency in Multiprocessors" (PhD,
 *   Stanford 1994). §3: software prefetch scheduling.
 *  Luk & Mowry — "Compiler-Based Prefetching for Recursive Data
 *   Structures" (ASPLOS '96). Pointer-chaining prefetch distance.
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.9 §9.4.
 *  Fog — Optimizing software in C++ (Vol.1), Ch.15: cache prefetch.
 */
#include "code/ir/opt/util.h"
#include "code/ir/opt/loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nyir_pf_trace_enabled(void) {
  return ny_trace_enabled("NY_TRACE_PF");
}

static void nyir_pf_trace(const nyir_func_t *f, size_t at, const char *action,
                           const char *reason) {
  (void)f;
  if (!nyir_pf_trace_enabled())
    return;
  ny_trace_line("PF", "prefetch: %s @%zu: %s", action, at, reason);
}

/*
 * Check if `value` depends on induction variable `iv` through a chain
 * of arithmetic, copy, or load definitions.  Does not chase through
 * PHIs (backedge) or memory.
 */
static bool value_depends_on_iv(const nyir_func_t *f, const int *defs,
                                int value, int iv, int depth) {
  if (depth > 16 || value < 0 || value >= f->next_value)
    return false;
  if (value == iv)
    return true;
  int def = defs[value];
  if (def < 0)
    return false;
  const nyir_inst_t *in = &f->data[def];
  switch (in->op) {
  case NYIR_PHI:
    return false;
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_COPY:
    return value_depends_on_iv(f, defs, in->a, iv, depth + 1) ||
           value_depends_on_iv(f, defs, in->b, iv, depth + 1);
  case NYIR_CONST_I64:
    return false;
  default:
    return false;
  }
}

/*
 * Return whether an address has exactly one rt_tbuf_new_raw payload base.
 * Only copies and additions preserve the origin; a second derived operand
 * makes the resulting address ambiguous.
 */
static bool tbuf_payload_address(const nyir_func_t *f, const int *defs,
                                 int value, unsigned depth) {
  if (!f || !defs || depth > 16 || value < 0 || value >= f->next_value)
    return false;
  int def = defs[value];
  if (def < 0)
    return false;
  const nyir_inst_t *in = &f->data[def];
  if (in->op == NYIR_CALL)
    return in->symbol && strcmp(in->symbol, "rt_tbuf_new_raw") == 0;
  if (in->op == NYIR_COPY)
    return tbuf_payload_address(f, defs, in->a, depth + 1);
  if (in->op != NYIR_ADD_I64)
    return false;

  bool a = tbuf_payload_address(f, defs, in->a, depth + 1);
  bool b = tbuf_payload_address(f, defs, in->b, depth + 1);
  return a != b;
}

/*
 * Insert a prefetch call: dst = call __simmd_prefetch(addr, rw, locality)
 * The CALL instruction uses a..f for the first 6 args.
 * For 3 args: a=addr, b=rw, c=locality.
 * We need CONST_I64 for rw and locality; emit them before the call.
 */
static bool insert_prefetch_call(nyir_func_t *f, size_t pos, int addr_val) {
  if (!nir_ensure_inst_space(f, 3))
    return false;

  /*
   * Make room for 2 CONST_I64 + 1 CALL.
   */
  memmove(&f->data[pos + 3], &f->data[pos],
          (f->len - pos) * sizeof(*f->data));
  f->len += 3;

  /*
   * rw = 0 (read)
   */
  int rw_dst = f->next_value++;
  f->data[pos] = (nyir_inst_t){.op = NYIR_CONST_I64, .dst = rw_dst, .imm = 0};
  /*
   * locality = 1 (L2)
   */
  int loc_dst = f->next_value++;
  f->data[pos + 1] =
      (nyir_inst_t){.op = NYIR_CONST_I64, .dst = loc_dst, .imm = 1};
  /*
   * CALL __simmd_prefetch(addr, rw=0, locality=1)
   */
  int call_dst = f->next_value++;
  f->data[pos + 2] = (nyir_inst_t){
      .op = NYIR_CALL,
      .dst = call_dst,
      .a = addr_val,
      .b = rw_dst,
      .c = loc_dst,
      .d = -1,
      .e = -1,
      .f = -1,
      .symbol = "__simmd_prefetch",
      .imm = 3};
  /*
   * Compound-literal construction leaves effects at zero; without the
   * CALL mask a later DCE would consider the prefetch side-effect
   * free and delete it.
   */
  f->data[pos + 2].effects = nyir_inst_effects(&f->data[pos + 2]);

  return true;
}

bool nyir_prefetch_insert(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  nyir_cfg_t cfg = {0};
  int *defs = nyir_build_defs(f);

  if (!defs || !nyir_cfg_build(f, &cfg)) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }

  nyir_scev_info_t info = {0};
  if (!nyir_scev_analyze(f, &info)) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }

  /*
   * Scan first, insert last. Inserting while scanning shifts instruction
   * indices by 3 per prefetch, silently invalidating cfg block ranges and
   * the defs map for the rest of the scan (misplaced duplicate
   * prefetches). Recorded positions refer to the ORIGINAL layout, so the
   * deferred inserts are applied in DESCENDING position order — earlier
   * positions stay valid.
   */
  typedef struct {
    size_t at;
    int addr_val;
  } pf_req_t;
  pf_req_t *reqs = NULL;
  size_t req_count = 0, req_cap = 0;

  for (size_t li = 0; li < info.count; ++li) {
    const nyir_scev_loop_t *loop = &info.loops[li];
    if (!loop->trip_count_known)
      continue;

    uint64_t loop_heat = ny_native_profile_loop_hot(loop->header_block);
    bool large_trip = loop->trip_count >= 64;
    if (loop_heat < 4 && !large_trip)
      continue;

    size_t h = loop->header_block;
    size_t k = loop->latch_block;
    int iv = loop->iv;

    bool *in_loop = calloc(cfg.block_count, sizeof(bool));
    if (!in_loop)
      continue;
    if (!nyir_cfg_natural_loop_blocks(&cfg, k, h, in_loop, cfg.block_count)) {
      free(in_loop);
      continue;
    }

    /*
     * Count prefetches per loop (limit to avoid code bloat).
     */
    int pf_count = 0;
    const int MAX_PF_PER_LOOP = 4;
    int prefetched_addresses[MAX_PF_PER_LOOP];

    for (size_t b = 0; b < cfg.block_count && pf_count < MAX_PF_PER_LOOP; ++b) {
      if (!in_loop[b])
        continue;
      for (size_t i = cfg.block_start[b]; i < cfg.block_end[b] &&
           pf_count < MAX_PF_PER_LOOP; ++i) {
        nyir_inst_t *in = &f->data[i];
        if (in->op != NYIR_LOAD_I64)
          continue;
        int address = in->a;
        if (!value_depends_on_iv(f, defs, address, iv, 0))
          continue;
        if (!tbuf_payload_address(f, defs, address, 0))
          continue;

        /*
         * Strided load: address is IV + constant offset.
         */
        bool stride_load = false;
        if (address >= 0 && address < f->next_value) {
          int addr_def = defs[address];
          if (addr_def >= 0) {
            const nyir_inst_t *addr_op = &f->data[addr_def];
            if (addr_op->op == NYIR_ADD_I64) {
              bool a_dep = value_depends_on_iv(f, defs, addr_op->a, iv, 0);
              bool b_dep = value_depends_on_iv(f, defs, addr_op->b, iv, 0);
              stride_load = (a_dep && !b_dep) || (!a_dep && b_dep);
            }
          }
        }

        if (!stride_load)
          continue;
        bool duplicate = false;
        for (int p = 0; p < pf_count; ++p) {
          if (prefetched_addresses[p] == address) {
            duplicate = true;
            break;
          }
        }
        if (duplicate)
          continue;

        nyir_pf_trace(f, i, "stride-prefetch", "strided load in hot loop");
        if (req_count == req_cap) {
          size_t next_cap = req_cap ? req_cap * 2 : 16;
          if (next_cap < req_cap ||
              next_cap > (size_t)-1 / sizeof(*reqs)) {
            free(in_loop);
            free(reqs);
            nyir_scev_free(&info);
            free(defs);
            nyir_cfg_free(&cfg);
            return false;
          }
          pf_req_t *grown = realloc(reqs, next_cap * sizeof(*reqs));
          if (!grown) {
            free(in_loop);
            free(reqs);
            nyir_scev_free(&info);
            free(defs);
            nyir_cfg_free(&cfg);
            return false;
          }
          reqs = grown;
          req_cap = next_cap;
        }
        reqs[req_count++] = (pf_req_t){.at = i, .addr_val = address};
        prefetched_addresses[pf_count++] = address;
      }
    }
    free(in_loop);
  }

  /*
   * Apply deferred insertions newest-position-first so recorded original
   * positions stay accurate for every remaining request.
   */
  for (size_t a = 1; a < req_count; ++a) {
    pf_req_t key = reqs[a];
    size_t b = a;
    while (b > 0 && reqs[b - 1].at < key.at) {
      reqs[b] = reqs[b - 1];
      --b;
    }
    reqs[b] = key;
  }
  for (size_t a = 0; a < req_count; ++a)
    insert_prefetch_call(f, reqs[a].at, reqs[a].addr_val);

  /*
   * Pass contract: the boolean result reports success, not "changed".
   * Finding no prefetch sites is a normal outcome.
   */
  nyir_scev_free(&info);
  free(defs);
  nyir_cfg_free(&cfg);
  free(reqs);
  return true;
}
