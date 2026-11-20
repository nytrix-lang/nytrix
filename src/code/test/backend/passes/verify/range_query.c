/*
 * Singleton intervals must survive both compatible and contradictory edge
 * bounds unchanged; broader intervals must still receive path refinement.
 */
#include "code/backend/passes/util.h"
#include "opt.h"

static int64_t range_query_cell;

static bool check_range(const nyr_func_t *f, const nyr_value_fact_t *facts,
                        int value, size_t at, int64_t lo, int64_t hi,
                        nir_range_query_cache_t *cache) {
  nyr_range_t range = {0}, cached = {0};
  bool ok = nir_value_range_at(f, facts, value, at, &range) && range.has_min &&
            range.has_max && range.min == lo && range.max == hi &&
            nir_value_range_at_cached(f, facts, value, at, &cached, cache) &&
            cached.has_min && cached.has_max && cached.min == lo &&
            cached.max == hi;
  if (!ok)
    fprintf(stderr,
            "[range-query] pc=%zu expected [%lld,%lld], got [%lld,%lld]\n", at,
            (long long)lo, (long long)hi, (long long)range.min,
            (long long)range.max);
  return ok;
}

static bool test_cache_mutations(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_opt_const(&f, (int64_t)(intptr_t)&range_query_cell);
  int value = nyr_opt_emit_abc(&f, NYIR_LOAD_I64, address, -1, -1);
  int bound = nyr_opt_const(&f, 5);
  size_t bound_at = f.len - 1;
  int cond = nyr_opt_cmp(&f, value, bound, NYIR_CMP_LT);
  size_t cmp_at = f.len - 1;
  size_t branch_at = f.len;
  nyr_opt_br_if(&f, cond, 10);
  size_t fallback_at = f.len;
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 10);
  size_t true_at = f.len;
  nyr_opt_ret(&f, value);
  nyr_opt_label(&f, 20);
  size_t false_at = f.len;
  nyr_opt_ret(&f, value);
  nyr_value_fact_t *facts = calloc((size_t)f.next_value + 4, sizeof(*facts));
  nir_range_query_cache_t cache = {0};
  bool ok = facts && nyr_opt_verify(&f, "range-query-cache");
  if (ok) {
    facts[value].range =
        (nyr_range_t){.has_min = true, .has_max = true, .min = 0, .max = 10};
    facts[bound].range =
        (nyr_range_t){.has_min = true, .has_max = true, .min = 5, .max = 5};
    ok = check_range(&f, facts, value, true_at, 0, 4, &cache) &&
         check_range(&f, facts, value, false_at, 5, 10, &cache);
  }
  /*
   * The topology/definition positions stay valid, but the predicate is live.
   */
  if (ok) {
    f.data[cmp_at].cmp = NYIR_CMP_GE;
    ok = check_range(&f, facts, value, true_at, 5, 10, &cache) &&
         check_range(&f, facts, value, false_at, 0, 4, &cache);
  }
  /*
   * Insertion moves every definition and query, possibly without reallocating.
   */
  if (ok && (ok = nir_ensure_inst_space(&f, 1))) {
    memmove(f.data + 1, f.data, f.len * sizeof(*f.data));
    f.data[0] = (nyr_inst_t){.op = NYIR_CONST_I64,
                             .dst = f.next_value++,
                             .a = -1,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 0};
    ++f.len;
    ++bound_at;
    ++cmp_at;
    ++branch_at;
    ++fallback_at;
    ++true_at;
    ++false_at;
    ok = nyr_opt_verify(&f, "range-query-insert") &&
         check_range(&f, facts, value, true_at, 5, 10, &cache);
  }
  /*
   * A storage move alone must also discard the cached definition positions.
   */
  if (ok) {
    nyr_inst_t *moved = malloc(f.cap * sizeof(*moved));
    if (!moved)
      ok = false;
    else {
      memcpy(moved, f.data, f.len * sizeof(*moved));
      free(f.data);
      f.data = moved;
      ok = check_range(&f, facts, value, true_at, 5, 10, &cache);
    }
  }
  /*
   * New value IDs must be represented even when the instruction count is fixed.
   */
  if (ok) {
    int new_bound = f.next_value++;
    f.data[bound_at].dst = new_bound;
    f.data[bound_at].imm = 8;
    nyr_clear_derived_facts(&f.data[bound_at]);
    f.data[cmp_at].b = new_bound;
    facts[new_bound].range =
        (nyr_range_t){.has_min = true, .has_max = true, .min = 8, .max = 8};
    ok = nyr_opt_verify(&f, "range-query-value-count") &&
         check_range(&f, facts, value, true_at, 8, 10, &cache);
  }
  /*
   * Same-size branch edits must invalidate cached topology automatically.
   */
  if (ok) {
    f.data[branch_at].imm = 20;
    f.data[fallback_at].imm = 10;
    ok = nyr_opt_verify(&f, "range-query-branch-edit") &&
         check_range(&f, facts, value, true_at, 0, 7, &cache) &&
         check_range(&f, facts, value, false_at, 8, 10, &cache);
  }
  nir_range_query_cache_free(&cache);
  free(facts);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  const nyr_cmp_t comparisons[] = {NYIR_CMP_EQ, NYIR_CMP_NE, NYIR_CMP_LT,
                                   NYIR_CMP_LE, NYIR_CMP_GT, NYIR_CMP_GE};
  const int64_t singletons[] = {INT64_MIN, -1, 0, 5, INT64_MAX};
  bool ok = true;
  for (size_t c = 0; ok && c < sizeof(comparisons) / sizeof(comparisons[0]);
       ++c) {
    nir_range_query_cache_t cache = {0};
    nyr_func_t f;
    nyr_opt_begin(&f);
    int address = nyr_opt_const(&f, (int64_t)(intptr_t)&range_query_cell);
    int value = nyr_opt_emit_abc(&f, NYIR_LOAD_I64, address, -1, -1);
    int bound = nyr_opt_const(&f, 5);
    int cond = nyr_opt_cmp(&f, value, bound, comparisons[c]);
    nyr_opt_br_if(&f, cond, 10);
    nyr_opt_br(&f, 20);
    nyr_opt_label(&f, 10);
    size_t true_at = f.len;
    nyr_opt_ret(&f, value);
    nyr_opt_label(&f, 20);
    size_t false_at = f.len;
    nyr_opt_ret(&f, value);
    nyr_value_fact_t *facts = calloc((size_t)f.next_value, sizeof(*facts));
    ok = facts && nyr_opt_verify(&f, "range-query");
    if (ok) {
      facts[bound].range =
          (nyr_range_t){.has_min = true, .has_max = true, .min = 5, .max = 5};
      for (size_t s = 0; ok && s < sizeof(singletons) / sizeof(singletons[0]);
           ++s) {
        int64_t v = singletons[s];
        facts[value].range =
            (nyr_range_t){.has_min = true, .has_max = true, .min = v, .max = v};
        ok = check_range(&f, facts, value, true_at, v, v, &cache) &&
             check_range(&f, facts, value, false_at, v, v, &cache);
      }
      if (ok && comparisons[c] == NYIR_CMP_LT) {
        facts[value].range = (nyr_range_t){
            .has_min = true, .has_max = true, .min = 0, .max = 10};
        ok = check_range(&f, facts, value, true_at, 0, 4, &cache) &&
             check_range(&f, facts, value, false_at, 5, 10, &cache);
      }
    }
    nir_range_query_cache_free(&cache);
    free(facts);
    nyr_ir_func_free(&f);
  }
  return ok && test_cache_mutations() ? 0 : 1;
}
