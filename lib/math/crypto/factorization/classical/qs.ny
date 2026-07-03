;; Keywords: factorization classical qs mpqs siqs quadratic-sieve relation polynomial
;; References: std.math.crypto.factorization.classical.misc
module std.math.crypto.factorization.classical.qs(_mpqs_a_divisor_cycle_from_base, _mpqs_a_divisor_disabled_fields, _mpqs_a_divisor_fields, _mpqs_a_divisor_pool, _mpqs_a_divisor_pool_from_schedule, _mpqs_a_divisor_target, _mpqs_adiv_count_for_base, _mpqs_adiv_product_from_mask, _mpqs_advance_center_mods, _mpqs_append_sign_polys, _mpqs_attempt_apply_step, _mpqs_attempt_collect_pass, _mpqs_attempt_collect_state, _mpqs_attempt_collect_windows, _mpqs_attempt_finish_pass, _mpqs_attempt_policy_report, _mpqs_base_without_pool, _mpqs_best_adiv_mask, _mpqs_bit_bucket_counts, _mpqs_bound_for_target_base_count, _mpqs_bucketed_prime_loop_report, _mpqs_build_adiv_polys, _mpqs_byte_disabled_fields, _mpqs_byte_sieve_add_roots_raw, _mpqs_byte_sieve_survivor_scan, _mpqs_byte_sieve_survivor_stats, _mpqs_byte_sieve_tail_fields, _mpqs_byte_sieve_window_int_roots_into, _mpqs_byte_sieve_window_summary, _mpqs_byte_total_fields, _mpqs_byte_totals_add, _mpqs_byte_window_choice, _mpqs_collect_byte_window_relations, _mpqs_collect_fallback_window_relations, _mpqs_collect_result, _mpqs_collect_window_step, _mpqs_default_work_plan, _mpqs_direct_byte_total_fields, _mpqs_early_dependency, _mpqs_finish_attempt_report, _mpqs_has_prime_bits, _mpqs_interp_int, _mpqs_inv_mod_int, _mpqs_list_has_z, _mpqs_maybe_early_dependency, _mpqs_mod_int_norm, _mpqs_pack_survivor, _mpqs_param_dict, _mpqs_param_result, _mpqs_pick_scheduled_prime, _mpqs_plain_root_filter, _mpqs_poly_byte_sieve_window_into, _mpqs_poly_from_primes_with_signs, _mpqs_popcount_mask, _mpqs_prime_bits, _mpqs_primes_from_mask, _mpqs_raw_byte_sieve_add_pos, _mpqs_raw_byte_sieve_root_pos, _mpqs_raw_byte_sieve_score_window_int_roots, _mpqs_raw_byte_sieve_start_rem, _mpqs_relation_buffer, _mpqs_relation_buffer_trim, _mpqs_root_filter_hit, _mpqs_root_filter_list_to_raw_shifted, _mpqs_root_filter_list_to_raw_shifted32, _mpqs_roots_ints, _mpqs_sieve_params_for_bits, _mpqs_sieve_score_min, _mpqs_source_adjust_factor_bits, _mpqs_source_factor_bit_schedule, _mpqs_source_start_bits, _mpqs_sqrt_roots, _mpqs_start_mods, _mpqs_used_attempt_fields, _mpqs_window_attempt_report, _mpqs_window_finish_fields, _mpqs_window_report, _mpqs_window_solve_fields, _mpqs_window_stats_add, _qs_batch_extract_smooth, _qs_batch_merge_tree_reports, _qs_batch_process_leaf, _qs_batch_product_range, _qs_batch_remainder_tree, _qs_batch_work_items, _qs_best_multiplier_fast, _qs_clean_prime_list, _qs_congruence_factor, _qs_dependency_matrix, _qs_dependency_solve_filtered_report, _qs_dependency_solve_report, _qs_dependency_sparse_rows, _qs_factor_base_bigints, _qs_factor_base_ints, _qs_factor_base_report, _qs_factor_base_result, _qs_factor_over_base_filtered_int_raw_scan_limited, _qs_factor_over_base_filtered_int_raw_scan_pos, _qs_factor_over_base_filtered_int_raw_scan_pos_collect32, _qs_factor_over_base_filtered_int_raw_scan_pos_limited, _qs_factor_over_base_filtered_int_raw_scan_pos_limited_collect32, _qs_factor_over_base_intbase_scan, _qs_factor_over_base_profile, _qs_factor_over_base_profile_filtered, _qs_factor_over_base_profile_filtered_int, _qs_factor_over_base_profile_int, _qs_factor_over_base_profile_intbase, _qs_has_singleton_parity, _qs_large_prime_acceptance_report, _qs_large_prime_relation_record, _qs_large_prime_tuple_key, _qs_legendre_int, _qs_lp_cycle_graph, _qs_lp_hist_set, _qs_lp_normalize_square_pairs, _qs_lp_root, _qs_lp_roots_same, _qs_lp_singleton_prune, _qs_lp_surviving_relations, _qs_lp_union_roots, _qs_lp_unique_records, _qs_lp_vertices, _qs_multiplier_candidates, _qs_multiplier_score_int, _qs_multiplier_summary_report, _qs_parity_weight, _qs_pow_mod_int, _qs_prime_base, _qs_prime_base_int, _qs_prime_product_report, _qs_prune_singletons, _qs_relation, _qs_relation_cofactor_values, _qs_relation_collection_fields, _qs_relation_compact, _qs_relation_int_raw_mpqs, _qs_relation_int_sparse_mpqs, _qs_relation_large_primes, _qs_relation_parity, _qs_relation_width, _qs_sieve_add_root, _qs_sieve_score_min, _qs_sieve_scores_siqs_raw_into, _qs_sieve_scores_siqs_with_roots, _qs_singleton_prune_round, _qs_smooth_metric_fields, _qs_smooth_metric_fields_from, _qs_sort_large_primes, _qs_square_relation_factor, _qs_try_dependency_candidates_mod_report, _qs_try_verified_dependency_mod, _qs_unique_relation_report, _qs_zero_counts, _qs_zero_scores, _siqs_a_exponents, _siqs_apply_a_exponents, _siqs_collect_poly_pass, _siqs_collect_poly_pass_int_raw, _siqs_collect_poly_pass_int_raw_loop, _siqs_collect_relations, _siqs_collect_state, _siqs_cutoff_candidates, _siqs_cutoff_disabled, _siqs_cutoff_measurement, _siqs_cutoff_measurement_raw, _siqs_cutoff_tune_from_polys, _siqs_factor_relation_fields, _siqs_factor_sieve_fields, _siqs_factor_solve_fields, _siqs_poly_from_primes, _siqs_poly_int_scan_allowed, _siqs_poly_report_fields, _siqs_poly_scan_result, _siqs_polynomial_report, _siqs_relation_fields, _siqs_relation_final_fields, _siqs_relation_intbase, _siqs_relation_intbase_for_mode, _siqs_relation_report, _siqs_residue_primes, mpqs_a_divisor_cycle_report, mpqs_byte_sieve_report, mpqs_factor, mpqs_factor_report, mpqs_multiplier_report, mpqs_sieve_parameter_report, mpqs_source_factor, mpqs_source_factor_report, mpqs_source_work_plan_report, mpqs_work_plan_report, qs_batch_cofactor_report, qs_large_prime_filter_report, qs_multiplier_report, qs_relation_filter_report, quadratic_sieve_factor, quadratic_sieve_factor_report, siqs_cutoff_tune_report, siqs_factor, siqs_factor_report, siqs_polynomial_report, siqs_relation_report)
use std.math.nt
use std.math.scalar as math
use std.math.matrix as matrix
use std.math.bin (bit_count)
use std.os (ticks)
use std.math.crypto.factorization.classical.misc
use std.math.crypto.factorization.classical.dixon
use std.math.crypto.factorization.classical.gf2

fn _qs_smooth_metric_fields(
   int candidate_count, int smooth_tests, int smooth_hits,
   int trial_prime_tests, int trial_divisions, int nonzero_exponent_terms,
) list {
   [
      ["candidate_count", candidate_count], ["smooth_tests", smooth_tests], ["smooth_hits", smooth_hits],
      ["smooth_misses", smooth_tests - smooth_hits], ["trial_division_prime_tests", trial_prime_tests], ["trial_divisions", trial_divisions],
      ["nonzero_exponent_terms", nonzero_exponent_terms],
   ]
}

fn _qs_relation_collection_fields(
   any rel_t0, int candidate_count, int smooth_tests, int smooth_hits,
   int trial_prime_tests, int trial_divisions, int nonzero_exponent_terms,
) list {
   _fields_extend([["relation_collection_elapsed_ms", _elapsed_ms(rel_t0)]], _qs_smooth_metric_fields(candidate_count, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_exponent_terms))
}

fn _qs_smooth_metric_fields_from(dict report, int smooth_hits_default=0) list {
   _qs_smooth_metric_fields(int(report.get("candidate_count", 0)), int(report.get("smooth_tests", 0)), int(report.get("smooth_hits", smooth_hits_default)), int(report.get("trial_division_prime_tests", 0)), int(report.get("trial_divisions", 0)), int(report.get("nonzero_exponent_terms", 0)))
}

fn _mpqs_a_divisor_fields(dict cycle, bool enabled, int polynomial_count, int sieve_base_size, int window_count, int relation_count) list {
   [
      ["a_divisor_cycling_enabled", enabled],
      ["a_divisor_cycle", cycle],
      ["a_divisor_polynomial_count", polynomial_count],
      ["a_divisor_pool_count", cycle.get("adiv_total", 0)],
      ["a_divisor_selected_count", cycle.get("adiv_count", 0)],
      ["a_divisor_removed_from_sieve_base_count", cycle.get("removed_from_sieve_base_count", 0)],
      ["a_divisor_root_precompute_count", cycle.get("root_precompute_count", 0)],
      ["a_divisor_inverse_precompute_count", cycle.get("inverse_precompute_count", 0)],
      ["a_divisor_shared_factor_base", cycle.get("shared_factor_base", false)],
      ["a_divisor_sieve_base_size", sieve_base_size],
      ["a_divisor_window_count", window_count],
      ["a_divisor_relation_count", relation_count],
   ]
}

fn _mpqs_a_divisor_disabled_fields() list {
   _mpqs_a_divisor_fields(dict(), false, 0, 0, 0, 0)
}

fn _mpqs_byte_totals_add(dict totals, dict report) dict {
   totals = _dict_add_int_fields(totals, report, [
         ["byte_sieve_candidate_count", "candidate_count"],
         ["byte_sieve_survivor_count", "survivor_count"],
         ["byte_sieve_skipped_count", "skipped_count"],
         ["byte_sieve_trial_division_avoidance_count", "trial_division_avoidance_count"],
         ["byte_sieve_marked_roots", "marked_roots"],
         ["byte_sieve_byte_adds_estimate", "byte_adds_estimate"],
         ["byte_sieve_prefill_byte_adds", "prefill_byte_adds"],
         ["byte_sieve_bucket_byte_adds", "bucket_byte_adds"],
         ["byte_sieve_score_loads", "score_loads"],
         ["byte_sieve_score_stores", "score_stores"],
         ["byte_sieve_buffer_clears", "buffer_clears"],
      ])
   _dict_max_int_fields(totals, report, [
         ["byte_sieve_bucket_count", "bucket_count"],
         ["byte_sieve_tiny_prime_count", "tiny_prime_count"],
         ["byte_sieve_tiny_prime_product", "tiny_prime_product", 1, 1],
      ])
}

fn _mpqs_byte_total_fields(dict totals, list window_reports, int window_radius) list {
   def candidates = _dict_int(totals, "byte_sieve_candidate_count")
   def survivors = _dict_int(totals, "byte_sieve_survivor_count")
   mut fields = [
      ["byte_sieve_collector_enabled", true],
      ["byte_sieve_collector", "mpqs-byte-block-survivor"],
      ["byte_sieve_byte_logbound", 128],
      ["byte_sieve_raw_buffer", true],
      ["byte_sieve_score_list_materialized", false],
      ["byte_sieve_window_reports", window_reports],
      ["byte_sieve_candidate_count", candidates],
      ["byte_sieve_survivor_count", survivors],
   ]
   fields = _append_used_defaults(fields, totals, [
         ["byte_sieve_skipped_count", 0],
         ["byte_sieve_trial_division_avoidance_count", 0],
         ["byte_sieve_marked_roots", 0],
      ])
   fields = fields.append(["byte_sieve_survivor_rate_x1000", candidates > 0 ? (survivors * 1000) / candidates : 0])
   fields = _append_used_defaults(fields, totals, [
         ["byte_sieve_byte_adds_estimate", 0],
         ["byte_sieve_prefill_byte_adds", 0],
         ["byte_sieve_bucket_byte_adds", 0],
         ["byte_sieve_score_loads", 0],
         ["byte_sieve_score_stores", 0],
      ])
   fields = fields.append(["byte_sieve_score_buffer_bytes", window_radius * 2 + 1])
   fields = _append_used_defaults(fields, totals, [
         ["byte_sieve_buffer_clears", 0],
         ["byte_sieve_bucket_count", 0],
         ["byte_sieve_tiny_prime_count", 0],
         ["byte_sieve_tiny_prime_product", 1],
      ])
   _fields_extend(fields, [
         ["byte_sieve_buffer_reuse_count", _dict_int(totals, "byte_sieve_buffer_clears")],
      ])
}

fn _mpqs_byte_disabled_fields() list {
   [
      ["byte_sieve_collector_enabled", false],
      ["byte_sieve_collector", ""],
      ["byte_sieve_byte_logbound", 0],
      ["byte_sieve_raw_buffer", false],
      ["byte_sieve_score_list_materialized", true],
      ["byte_sieve_window_reports", []],
      ["byte_sieve_candidate_count", 0],
      ["byte_sieve_survivor_count", 0],
      ["byte_sieve_skipped_count", 0],
      ["byte_sieve_trial_division_avoidance_count", 0],
      ["byte_sieve_marked_roots", 0],
      ["byte_sieve_survivor_rate_x1000", 0],
      ["byte_sieve_byte_adds_estimate", 0],
      ["byte_sieve_prefill_byte_adds", 0],
      ["byte_sieve_bucket_byte_adds", 0],
      ["byte_sieve_score_loads", 0],
      ["byte_sieve_score_stores", 0],
      ["byte_sieve_score_buffer_bytes", 0],
      ["byte_sieve_buffer_clears", 0],
      ["byte_sieve_buffer_reuse_count", 0],
      ["byte_sieve_bucket_count", 0],
      ["byte_sieve_tiny_prime_count", 0],
      ["byte_sieve_tiny_prime_product", 1],
   ]
}

fn _mpqs_direct_byte_total_fields(dict totals, list window_reports, int window_radius, int max_score) list {
   def candidates = _dict_int(totals, "byte_sieve_candidate_count")
   def survivors = _dict_int(totals, "byte_sieve_survivor_count")
   [
      ["tiny_prime_count", _dict_int(totals, "byte_sieve_tiny_prime_count")],
      ["tiny_prime_product", _dict_int(totals, "byte_sieve_tiny_prime_product", 1)],
      ["bucket_count", _dict_int(totals, "byte_sieve_bucket_count")],
      ["byte_adds_estimate", _dict_int(totals, "byte_sieve_byte_adds_estimate")],
      ["prefill_byte_adds", _dict_int(totals, "byte_sieve_prefill_byte_adds")],
      ["bucket_byte_adds", _dict_int(totals, "byte_sieve_bucket_byte_adds")],
      ["score_loads", _dict_int(totals, "byte_sieve_score_loads")],
      ["score_stores", _dict_int(totals, "byte_sieve_score_stores")],
      ["score_buffer_bytes", window_radius * 2 + 1],
      ["buffer_clears", _dict_int(totals, "byte_sieve_buffer_clears")],
      ["buffer_reuse_count", _dict_int(totals, "byte_sieve_buffer_clears")],
      ["window_reports", window_reports],
      ["candidate_count", candidates],
      ["survivor_count", survivors],
      ["skipped_count", _dict_int(totals, "byte_sieve_skipped_count")],
      ["trial_division_avoidance_count", _dict_int(totals, "byte_sieve_trial_division_avoidance_count")],
      ["marked_roots", _dict_int(totals, "byte_sieve_marked_roots")],
      ["max_score", max_score],
      ["survivor_rate_x1000", candidates > 0 ? (survivors * 1000) / candidates : 0],
   ]
}

fn _qs_prime_base(int bound) list {
   mut out = []
   if bound < 2 { return out }
   mut composite = list(bound + 1)
   __list_set_len(composite, bound + 1)
   mut i = 0
   while i <= bound {
      composite[i] = false
      i += 1
   }
   mut p = 2
   while p <= bound {
      if !bool(composite[p]) {
         out = out.append(Z(p))
         if p <= bound / p {
            mut k = p * p
            while k <= bound {
               composite[k] = true
               k += p
            }
         }
      }
      p += p == 2 ? 1 : 2
   }
   out
}

fn _qs_factor_base_result(any factor, list base, int tested, int residue_hits, int modulus_divisors) dict {
   {
      "factor": factor,
      "base": base,
      "prime_count": base.len,
      "tested": tested,
      "residue_hits": residue_hits,
      "modulus_divisors": modulus_divisors,
   }
}

fn _qs_factor_base_report(any n, any modulus, int bound) dict {
   def nz, mz = _z(n), _z(modulus)
   def base0 = _qs_prime_base(bound)
   mut base = []
   mut tested = 0
   mut residue_hits = 0
   mut modulus_divisors = 0
   mut i = 0
   while i < base0.len {
      def p, g = _z(base0.get(i)), gcd(_z(base0.get(i)), nz)
      if _is_nontrivial_factor(g, nz) {
         return _qs_factor_base_result(g, base, tested, residue_hits, modulus_divisors)
      }
      if g == Z(1) {
         tested += 1
         if p == Z(2) {
            base = base.append(p)
         } else {
            def gm = gcd(p, mz)
            if gm != Z(1) {
               base = base.append(p)
               modulus_divisors += 1
            } elif legendre(mod(mz, p), p) == 1 {
               base = base.append(p)
               residue_hits += 1
            }
         }
      }
      i += 1
   }
   _qs_factor_base_result(nil, base, tested, residue_hits, modulus_divisors)
}

fn _qs_sieve_score_min(list base) int {
   case base.len {
      0..23 -> 1
      24..31 -> 4
      _ -> 5
   }
}

fn _siqs_a_exponents(dict poly, list base) list {
   mut exps = list(base.len)
   mut i = 0
   while i < base.len {
      exps[i] = 0
      i += 1
   }
   def primes = poly.get("prime_factors_A", [])
   i = 0
   while i < primes.len {
      def p = _z(primes[i])
      mut j = 0
      while j < base.len {
         if _z(base[j]) == p {
            exps[j] = int(exps[j]) + 1
            j = base.len
         } else {
            j += 1
         }
      }
      i += 1
   }
   exps
}

fn _siqs_apply_a_exponents(list exps, list a_exps) list {
   mut out = exps
   mut i = 0
   while i < out.len && i < a_exps.len {
      def add = int(a_exps[i])
      if add != 0 { out[i] = int(out[i]) + add }
      i += 1
   }
   out
}

fn _siqs_poly_int_scan_allowed(any A, any B, any C, int radius) bool {
   def r = Z(max(0, radius))
   def a, b, c = bigint_abs(_z(A)), bigint_abs(_z(B)), bigint_abs(_z(C))
   def q_bound, x_bound = a * r * r + Z(2) * b * r + c, a * r + b
   bit_length(a) <= 60 && bit_length(b) <= 60 && bit_length(c) <= 60 && bit_length(q_bound) <= 60 && bit_length(x_bound) <= 60
}

fn _siqs_cutoff_candidates(int default_cutoff) list {
   mut out = [default_cutoff]
   out = out.append(max(0, default_cutoff - 1))
   out.append(default_cutoff + 1)
}

fn _siqs_cutoff_measurement_raw(any modulus, list base, list sqrt_roots, list plist, int cutoff, int sample_polynomials, int sample_radius) any {
   mut pi = 0
   while pi < plist.len && pi < sample_polynomials {
      def poly = plist.get(pi)
      if !_siqs_poly_int_scan_allowed(poly.get("A"), poly.get("B"), poly.get("C"), sample_radius) { return nil }
      pi += 1
   }
   def t0 = ticks()
   mut candidates = 0
   mut tested = 0
   mut skipped = 0
   mut smooth_hits = 0
   mut marked_roots = 0
   mut skipped_noninvertible = 0
   def trial_base = _qs_factor_base_ints(base)
   with ptr raw_scores = malloc(sample_radius * 2 + 1){
      if !raw_scores { return nil }
      with ptr raw_counts = malloc(24){
         if !raw_counts { return nil }
         memset(raw_counts, 0, 24)
         pi = 0
         while pi < plist.len && pi < sample_polynomials {
            def poly = plist.get(pi)
            def A = _z(poly.get("A"))
            def B = _z(poly.get("B"))
            def C = _z(poly.get("C"))
            def score_report = _qs_sieve_scores_siqs_raw_into(raw_scores, nil, modulus, base, sqrt_roots, A, B, sample_radius)
            marked_roots += int(score_report.get("marked_roots", 0))
            skipped_noninvertible += int(score_report.get("skipped_noninvertible", 0))
            def int Ai = bigint_to_int(A)
            def int Bi = bigint_to_int(B)
            def int Ci = bigint_to_int(C)
            mut int t = 0 - sample_radius
            def int twoA = 2 * Ai
            def int twoB = 2 * Bi
            mut int qv = Ai * t * t + twoB * t + Ci
            mut int q_delta = twoA * t + Ai + twoB
            while t <= sample_radius {
               def int pos = t + sample_radius
               if qv > 0 {
                  candidates += 1
                  def int score = int(load8(raw_scores, pos))
                  if score >= cutoff {
                     tested += 1
                     if _qs_factor_over_base_intbase_scan(qv, trial_base, raw_counts) == 1 { smooth_hits += 1 }
                  } else {
                     skipped += 1
                  }
               }
               qv += q_delta
               q_delta += twoA
               t += 1
            }
            pi += 1
         }
         def elapsed = _elapsed_ms(t0)
         def tested_safe = max(1, tested)
         def cand_safe = max(1, candidates)
         return _dict_with(18, [
               ["cutoff", cutoff], ["sample_polynomials", min(sample_polynomials, plist.len)],
               ["sample_radius", sample_radius], ["candidates", candidates],
               ["tested", tested], ["skipped", skipped], ["smooth_hits", smooth_hits],
               ["hit_rate_per_test", float(smooth_hits) / float(tested_safe)],
               ["hit_rate_per_candidate", float(smooth_hits) / float(cand_safe)],
               ["tests_per_hit", smooth_hits > 0 ? (float(tested) / float(smooth_hits)) : float(tested)],
               ["marked_roots", marked_roots], ["skipped_noninvertible", skipped_noninvertible],
               ["trial_division_prime_tests", load64_i(raw_counts, 0)],
               ["trial_divisions", load64_i(raw_counts, 8)],
               ["nonzero_exponent_terms", load64_i(raw_counts, 16)],
               ["elapsed_ms", elapsed],
               ["rels_per_ms", elapsed > 0.0 ? (float(smooth_hits) / elapsed) : float(smooth_hits)],
               ["raw_scores", true],
            ])
      }
   }
   nil
}

fn _siqs_cutoff_measurement(any modulus, list base, list sqrt_roots, list plist, int cutoff, int sample_polynomials, int sample_radius) dict {
   def raw = _siqs_cutoff_measurement_raw(modulus, base, sqrt_roots, plist, cutoff, sample_polynomials, sample_radius)
   if raw != nil { return raw }
   def t0 = ticks()
   mut candidates = 0
   mut tested = 0
   mut skipped = 0
   mut smooth_hits = 0
   mut trial_prime_tests = 0
   mut trial_divisions = 0
   mut nonzero_exponent_terms = 0
   mut marked_roots = 0
   mut skipped_noninvertible = 0
   def trial_base = _qs_factor_base_ints(base)
   mut pi = 0
   while pi < plist.len && pi < sample_polynomials {
      def poly = plist.get(pi)
      def A = _z(poly.get("A"))
      def B = _z(poly.get("B"))
      def C = _z(poly.get("C"))
      def score_report = _qs_sieve_scores_siqs_with_roots(modulus, base, sqrt_roots, A, B, sample_radius)
      def scores = score_report.get("scores", [])
      marked_roots += int(score_report.get("marked_roots", 0))
      skipped_noninvertible += int(score_report.get("skipped_noninvertible", 0))
      mut t = 0 - sample_radius
      while t <= sample_radius {
         def tz = Z(t)
         def qv = A * tz * tz + Z(2) * B * tz + C
         def pos = t + sample_radius
         if qv > Z(0) {
            candidates += 1
            def score = int(scores[pos])
            if score >= cutoff {
               tested += 1
               def smooth = bit_length(qv) <= 60 ? _qs_factor_over_base_profile_intbase(bigint_to_int(qv), trial_base) : _qs_factor_over_base_profile(qv, base)
               trial_prime_tests += int(smooth[3])
               trial_divisions += int(smooth[4])
               nonzero_exponent_terms += int(smooth[5])
               if smooth[0] { smooth_hits += 1 }
            } else {
               skipped += 1
            }
         }
         t += 1
      }
      pi += 1
   }
   def elapsed = _elapsed_ms(t0)
   def tested_safe = max(1, tested)
   def cand_safe = max(1, candidates)
   _dict_with(16, [
         ["cutoff", cutoff], ["sample_polynomials", min(sample_polynomials, plist.len)],
         ["sample_radius", sample_radius], ["candidates", candidates],
         ["tested", tested], ["skipped", skipped], ["smooth_hits", smooth_hits],
         ["hit_rate_per_test", float(smooth_hits) / float(tested_safe)],
         ["hit_rate_per_candidate", float(smooth_hits) / float(cand_safe)],
         ["tests_per_hit", smooth_hits > 0 ? (float(tested) / float(smooth_hits)) : float(tested)],
         ["marked_roots", marked_roots], ["skipped_noninvertible", skipped_noninvertible],
         ["trial_division_prime_tests", trial_prime_tests],
         ["trial_divisions", trial_divisions],
         ["nonzero_exponent_terms", nonzero_exponent_terms],
         ["elapsed_ms", elapsed],
         ["rels_per_ms", elapsed > 0.0 ? (float(smooth_hits) / elapsed) : float(smooth_hits)],
      ])
}

fn _siqs_cutoff_tune_from_polys(any modulus, list base, list plist, int default_cutoff, int sieve_radius, bool double_large_prime=false) dict {
   def t0 = ticks()
   mut sample_polys = min(3, plist.len)
   if sample_polys < 1 { sample_polys = plist.len }
   mut sample_radius = min(48, max(8, sieve_radius / 4))
   if sample_radius > sieve_radius { sample_radius = sieve_radius }
   def margin = double_large_prime ? 1.05 : 1.02
   def cutoffs = _siqs_cutoff_candidates(default_cutoff)
   def sqrt_roots = _mpqs_sqrt_roots(modulus, base)
   mut measurements = []
   mut i = 0
   while i < cutoffs.len {
      measurements = measurements.append(_siqs_cutoff_measurement(modulus, base, sqrt_roots, plist, int(cutoffs.get(i)), sample_polys, sample_radius))
      i += 1
   }
   mut baseline = measurements.len > 0 ? measurements.get(0) : _dict_with(4, [["cutoff", default_cutoff], ["rels_per_ms", 0.0], ["smooth_hits", 0], ["tested", 0]])
   mut selected = baseline
   mut selected_reason = "baseline"
   def baseline_score = float(baseline.get("rels_per_ms", 0.0))
   i = 1
   while i < measurements.len {
      def m = measurements.get(i)
      def score = float(m.get("rels_per_ms", 0.0))
      def hits = int(m.get("smooth_hits", 0))
      def baseline_hits = int(baseline.get("smooth_hits", 0))
      if score > baseline_score * margin && hits >= baseline_hits {
         selected = m
         selected_reason = "measured-throughput-margin"
      }
      i += 1
   }
   _report_with("siqs-adaptive-cutoff", t0, [
         ["default_cutoff", default_cutoff],
         ["selected_cutoff", int(selected.get("cutoff", default_cutoff))],
         ["selected_reason", selected_reason],
         ["margin", margin], ["sample_polynomials", sample_polys],
         ["sample_radius", sample_radius],
         ["measurement_count", measurements.len],
         ["measurements", measurements],
         ["selected_measurement", selected],
         ["baseline_measurement", baseline],
         ["changed", int(selected.get("cutoff", default_cutoff)) != default_cutoff],
         ["selection_metric", "relations-per-ms"],
         ["source_model", "SIQS small-factor cutoff relation-rate sampling"],
         ["policy", "measure default,-1,+1 and require deterministic relation-throughput margin"],
      ])
}

fn siqs_cutoff_tune_report(any n, int factor_base_bound=64, int polynomial_count=8, int sieve_radius=256, bool double_large_prime=false) dict {
   "Measure SIQS sieve prefilter cutoffs on a small polynomial sample and report the selected threshold."
   def t0 = ticks()
   def nz = _z(n)
   def polys = _siqs_polynomial_report(nz, factor_base_bound, polynomial_count, sieve_radius, false)
   def modulus = polys.get("sieve_modulus", nz)
   def base_report = _qs_factor_base_report(nz, modulus, factor_base_bound)
   def base = base_report.get("base", [])
   def default_cutoff = _qs_sieve_score_min(base)
   mut tune = _siqs_cutoff_tune_from_polys(modulus, base, polys.get("polynomials", []), default_cutoff, sieve_radius, double_large_prime)
   _finish_report_with(tune, t0, [
         ["n", nz], ["factor_base_bound", factor_base_bound],
         ["polynomial_count", polynomial_count], ["sieve_radius", sieve_radius],
         ["polynomial_report", polys], ["factor_base_report", base_report],
      ])
}

fn _mpqs_sieve_score_min(list base) int {
   case base.len {
      0..47 -> _qs_sieve_score_min(base)
      _ -> 6
   }
}

fn _qs_zero_scores(int n) list {
   mut out = list(n)
   mut i = 0
   while i < n {
      out[i] = 0
      i += 1
   }
   out
}

fn _qs_sieve_add_root(list scores, int radius, int p, int root) list {
   if p <= 0 { return scores }
   def size = radius * 2 + 1
   mut start_rem = (0 - radius) % p
   if start_rem < 0 { start_rem += p }
   mut pos = root - start_rem
   if pos < 0 { pos += p }
   while pos < size {
      scores[pos] = int(scores[pos]) + 1
      pos += p
   }
   scores
}

fn _qs_sieve_scores_siqs_with_roots(any modulus, list base, list sqrt_roots, any A, any B, int radius) dict {
   mut scores = _qs_zero_scores(radius * 2 + 1)
   mut marked_roots = 0
   mut skipped_noninvertible = 0
   def no_root = Z(-1)
   mut i = 0
   while i < base.len {
      def pz = _z(base[i])
      def p = bigint_to_int(pz)
      if p > 2 {
         def Am = mod(A, pz)
         if Am == Z(0) {
            skipped_noninvertible += 1
         } else {
            def r = i < sqrt_roots.len ? sqrt_roots[i] : no_root
            if r != Z(-1) {
               def invA = inverse_mod(Am, pz)
               def Bm = mod(B, pz)
               def root1 = bigint_to_int(mod((r - Bm) * invA, pz))
               def root2 = bigint_to_int(mod((Z(0) - r - Bm) * invA, pz))
               scores = _qs_sieve_add_root(scores, radius, p, root1)
               marked_roots += 1
               if root2 != root1 {
                  scores = _qs_sieve_add_root(scores, radius, p, root2)
                  marked_roots += 1
               }
            }
         }
      }
      i += 1
   }
   {"scores": scores, "marked_roots": marked_roots, "skipped_noninvertible": skipped_noninvertible}
}

fn _qs_sieve_scores_siqs_raw_into(ptr scores, ptr root_filters, any modulus, list base, list sqrt_roots, any A, any B, int radius) dict {
   def size = radius * 2 + 1
   memset(scores, 0, size)
   if root_filters { memset(root_filters, 0, base.len * 32) }
   mut marked_roots = 0
   mut skipped_noninvertible = 0
   def no_root = Z(-1)
   def A_z = _z(A)
   def B_z = _z(B)
   def A_abs = A_z < Z(0) ? -A_z : A_z
   def B_abs = B_z < Z(0) ? -B_z : B_z
   def int_mod_path = bit_length(A_abs) <= 62 && bit_length(B_abs) <= 62
   def A_i = int_mod_path ? bigint_to_int(A_z) : 0
   def B_i = int_mod_path ? bigint_to_int(B_z) : 0
   mut i = 0
   while i < base.len {
      def k = i * 32
      def pz = _z(base[i])
      def p = bigint_to_int(pz)
      if root_filters { store64_i(root_filters, p, k) }
      if p > 2 {
         def use_int_mod = int_mod_path && p <= 1000000000
         def Am_i = use_int_mod ? _mpqs_mod_int_norm(A_i, p) : bigint_to_int(mod(A_z, pz))
         if Am_i == 0 {
            skipped_noninvertible += 1
         } else {
            def r = i < sqrt_roots.len ? sqrt_roots[i] : no_root
            if r != no_root {
               def invA_i = use_int_mod ? _mpqs_inv_mod_int(Am_i, p) : bigint_to_int(inverse_mod(Z(Am_i), pz))
               def Bm_i = use_int_mod ? _mpqs_mod_int_norm(B_i, p) : bigint_to_int(mod(B_z, pz))
               def r_i = bigint_to_int(r)
               def root1 = _mpqs_mod_int_norm((r_i - Bm_i) * invA_i, p)
               def root2 = _mpqs_mod_int_norm((0 - r_i - Bm_i) * invA_i, p)
               mut int start_rem = (0 - radius) % p
               if start_rem < 0 { start_rem += p }
               mut int pos1 = root1 - start_rem
               mut int pos2 = root2 - start_rem
               if pos1 < 0 { pos1 += p }
               if pos2 < 0 { pos2 += p }
               if root_filters {
                  store64_i(root_filters, pos1, k + 8)
                  store64_i(root_filters, pos2, k + 16)
                  store64_i(root_filters, 1, k + 24)
               }
               _mpqs_raw_byte_sieve_add_pos(scores, size, p, pos1)
               marked_roots += 1
               if root2 != root1 {
                  _mpqs_raw_byte_sieve_add_pos(scores, size, p, pos2)
                  marked_roots += 1
               }
            }
         }
      }
      i += 1
   }
   {"marked_roots": marked_roots, "skipped_noninvertible": skipped_noninvertible, "raw_scores": true}
}

fn _mpqs_bucketed_prime_loop_report(list base, int sieve_len) dict {
   "Report MPQS sieve prime buckets by expected loop count."
   mut le_quarter = 0
   mut le_third = 0
   mut le_half = 0
   mut le_full = 0
   mut gt_full = 0
   mut roots_est = 0
   mut hit_est = 0
   mut i = 0
   while i < base.len {
      def p = bigint_to_int(_z(base[i]))
      if p > 2 {
         roots_est += 2
         def hits = 2 * ((sieve_len + p - 1) / p)
         hit_est += hits
         if p <= sieve_len / 4 {
            le_quarter += 1
         } elif p <= sieve_len / 3 {
            le_third += 1
         } elif p <= sieve_len / 2 {
            le_half += 1
         } elif p <= sieve_len {
            le_full += 1
         } else {
            gt_full += 1
         }
      }
      i += 1
   }
   _dict_with(12, [
         ["sieve_len", sieve_len],
         ["le_quarter", le_quarter],
         ["le_third", le_third],
         ["le_half", le_half],
         ["le_full", le_full],
         ["gt_full", gt_full],
         ["bucket_count", (le_quarter > 0 ? 1 : 0) + (le_third > 0 ? 1 : 0) + (le_half > 0 ? 1 : 0) + (le_full > 0 ? 1 : 0) + (gt_full > 0 ? 1 : 0)],
         ["root_streams_estimate", roots_est],
         ["byte_adds_estimate", hit_est],
      ])
}

@inline
fn _mpqs_raw_byte_sieve_start_rem(int radius, int p) int {
   mut start_rem = (0 - radius) % p
   if start_rem < 0 { start_rem += p }
   start_rem
}

@inline
fn _mpqs_raw_byte_sieve_root_pos(int start_rem, int p, int root) int {
   mut pos = root - start_rem
   if pos < 0 { pos += p }
   pos
}

@inline
fn _mpqs_raw_byte_sieve_add_pos(ptr scores, int size, int p, int pos) int {
   if p <= 0 { return 0 }
   mut writes = 0
   while pos < size {
      def v = int(load8(scores, pos))
      if v < 255 { store8(scores, v + 1, pos) }
      writes += 1
      pos += p
   }
   writes
}

fn _mpqs_byte_sieve_tail_fields(
   int tiny_limit, int tiny_prime_count, int tiny_product, dict buckets,
   int score_stores, int prefill_byte_adds, int bucket_byte_adds,
   int score_loads, int sieve_len,
) list {
   [
      ["tiny_prime_prefill", _dict_with(8, [
               ["limit", tiny_limit], ["tiny_prime_count", tiny_prime_count],
               ["tiny_prime_product", tiny_product],
               ["prefill_repeats_estimate", tiny_product > 0 ? max(1, sieve_len / tiny_product) : 0],
               ["byte_adds", prefill_byte_adds],
            ])],
      ["tiny_prime_count", tiny_prime_count],
      ["tiny_prime_product", tiny_product],
      ["bucketed_prime_loops", buckets],
      ["bucket_count", buckets.get("bucket_count", 0)],
      ["byte_adds_estimate", score_stores],
      ["prefill_byte_adds", prefill_byte_adds],
      ["bucket_byte_adds", bucket_byte_adds],
      ["score_loads", score_loads],
      ["score_stores", score_stores],
      ["score_buffer_bytes", sieve_len],
      ["buffer_clears", 1],
   ]
}

fn _mpqs_byte_sieve_add_roots_raw(ptr scratch, int sieve_len, int radius, int p, int root1, int root2, int tiny_limit, ptr totals) any {
   def tiny_product = load64_i(totals, 0)
   def tiny_prime = tiny_product * p < tiny_limit
   mut streams = load64_i(totals, 16)
   mut total_writes = load64_i(totals, 24)
   mut prefill_writes = load64_i(totals, 32)
   mut bucket_writes = load64_i(totals, 40)
   if tiny_prime {
      store64_i(totals, tiny_product * p, 0)
      store64_i(totals, load64_i(totals, 8) + 1, 8)
   }
   def start_rem = _mpqs_raw_byte_sieve_start_rem(radius, p)
   def writes1 = _mpqs_raw_byte_sieve_add_pos(scratch, sieve_len, p, _mpqs_raw_byte_sieve_root_pos(start_rem, p, root1))
   streams += 1
   total_writes += writes1
   if tiny_prime {
      prefill_writes += writes1
   } else {
      bucket_writes += writes1
   }
   if root2 != root1 {
      def writes2 = _mpqs_raw_byte_sieve_add_pos(scratch, sieve_len, p, _mpqs_raw_byte_sieve_root_pos(start_rem, p, root2))
      streams += 1
      total_writes += writes2
      if tiny_prime {
         prefill_writes += writes2
      } else {
         bucket_writes += writes2
      }
   }
   store64_i(totals, streams, 16)
   store64_i(totals, total_writes, 24)
   store64_i(totals, prefill_writes, 32)
   store64_i(totals, bucket_writes, 40)
   nil
}

@inline
fn _mpqs_pack_survivor(int pos, int score) int { (pos << 8) | (score & 255) }

fn _mpqs_byte_sieve_survivor_scan(ptr scratch, int sieve_len, int radius, int threshold, any center=nil, any min_x=nil) list {
   mut survivors = []
   mut candidate_count = 0
   mut skipped = 0
   mut max_score = 0
   mut sum_score = 0
   mut score_loads = 0
   mut i = 0
   if min_x == nil {
      while i < sieve_len {
         def s = int(load8(scratch, i))
         score_loads += 1
         candidate_count += 1
         if s >= threshold { survivors = survivors.append(_mpqs_pack_survivor(i, s)) } else { skipped += 1 }
         if s > max_score { max_score = s }
         sum_score += s
         i += 1
      }
   } else {
      if is_int(min_x) && is_int(center) {
         def first_i = int(min_x) - int(center) + radius
         if first_i > 0 { i = first_i >= sieve_len ? sieve_len : first_i }
      } else {
         def first_z = _z(min_x) - _z(center) + Z(radius)
         if first_z > Z(0) {
            i = first_z >= Z(sieve_len) ? sieve_len : bigint_to_int(first_z)
         }
      }
      while i < sieve_len {
         def s = int(load8(scratch, i))
         score_loads += 1
         candidate_count += 1
         if s >= threshold { survivors = survivors.append(_mpqs_pack_survivor(i, s)) } else { skipped += 1 }
         if s > max_score { max_score = s }
         sum_score += s
         i += 1
      }
   }
   [survivors, candidate_count, skipped, max_score, sum_score, score_loads]
}

fn _mpqs_byte_sieve_survivor_stats(ptr scratch, int sieve_len, int radius, int threshold, any center=nil, any min_x=nil) dict {
   def scan = _mpqs_byte_sieve_survivor_scan(scratch, sieve_len, radius, threshold, center, min_x)
   _dict_with(10, [
         ["survivors", scan[0]], ["candidate_count", scan[1]],
         ["skipped_count", scan[2]], ["max_score", scan[3]],
         ["sum_score", scan[4]], ["score_loads", scan[5]],
      ])
}

fn _mpqs_mod_int_norm(int x, int p) int {
   mut r = x % p
   if r < 0 { r += p }
   r
}

fn _mpqs_inv_mod_int(int a, int m) int {
   mut t = 0
   mut new_t = 1
   mut r = m
   mut new_r = _mpqs_mod_int_norm(a, m)
   while new_r != 0 {
      def q = r / new_r
      def tmp_t = new_t
      new_t = t - q * new_t
      t = tmp_t
      def tmp_r = new_r
      new_r = r - q * new_r
      r = tmp_r
   }
   if r != 1 { return 0 }
   _mpqs_mod_int_norm(t, m)
}

fn _mpqs_roots_ints(list sqrt_roots) list<int> {
   mut list<int> out = list(sqrt_roots.len)
   __list_set_len(out, sqrt_roots.len)
   mut i = 0
   while i < sqrt_roots.len {
      out[i] = int(sqrt_roots[i])
      i += 1
   }
   out
}

fn _mpqs_start_mods(list<int> base_ints, any start, int stride) list {
   mut list<int> center_mods = list(base_ints.len)
   mut list<int> stride_mods = list(base_ints.len)
   __list_set_len(center_mods, base_ints.len)
   __list_set_len(stride_mods, base_ints.len)
   mut i = 0
   while i < base_ints.len {
      def p = base_ints[i]
      if p > 0 {
         center_mods[i] = is_int(start) ? _mpqs_mod_int_norm(int(start), p) : int(mod(start, Z(p)))
         stride_mods[i] = stride % p
      } else {
         center_mods[i] = 0
         stride_mods[i] = 0
      }
      i += 1
   }
   [center_mods, stride_mods]
}

fn _mpqs_advance_center_mods(list<int> center_mods, list<int> stride_mods, list<int> base_ints) any {
   mut i = 0
   while i < center_mods.len {
      def p = base_ints[i]
      if p > 0 { center_mods[i] = (center_mods[i] + stride_mods[i]) % p }
      i += 1
   }
   nil
}

fn _mpqs_byte_sieve_window_int_roots_into(ptr scratch, list<int> base_ints, list<int> sqrt_roots_int, list<int> center_mods, dict buckets, any center, int radius, int threshold, any min_x) dict {
   def sieve_len = radius * 2 + 1
   memset(scratch, 0, sieve_len)
   def tiny_limit = max(1, sieve_len / 16)
   mut skipped_noninvertible = 0
   mut list<int> root_filters = list(base_ints.len * 4)
   with ptr totals_raw = malloc(48){
      if !totals_raw { panic("mpqs byte-sieve counter allocation failed") }
      memset(totals_raw, 0, 48)
      store64_i(totals_raw, 1, 0)
      mut i = 0
      while i < base_ints.len {
         def k, p = i * 4, base_ints[i]
         if p > 2 {
            def r = i < sqrt_roots_int.len ? sqrt_roots_int[i] : -1
            if r < 0 {
               skipped_noninvertible += 1
               root_filters[k] = p
               root_filters[k + 1] = 0
               root_filters[k + 2] = 0
               root_filters[k + 3] = 0
            } else {
               def cm = center_mods[i]
               def root1 = _mpqs_mod_int_norm(r - cm, p)
               def root2 = _mpqs_mod_int_norm(0 - r - cm, p)
               root_filters[k] = p
               root_filters[k + 1] = root1
               root_filters[k + 2] = root2
               root_filters[k + 3] = 1
               _mpqs_byte_sieve_add_roots_raw(scratch, sieve_len, radius, p, root1, root2, tiny_limit, totals_raw)
            }
         } else {
            root_filters[k] = p
            root_filters[k + 1] = 0
            root_filters[k + 2] = 0
            root_filters[k + 3] = 0
         }
         i += 1
      }
      def scan = _mpqs_byte_sieve_survivor_scan(scratch, sieve_len, radius, threshold, center, min_x)
      def survivors = scan[0]
      def candidate_count = int(scan[1])
      def skipped_count = int(scan[2])
      def max_score = int(scan[3])
      def sum_score = int(scan[4])
      def score_loads = int(scan[5])
      _dict_with(36, _fields_extend([
               ["method", "mpqs-byte-sieve-window"],
               ["collector", "mpqs-byte-block-survivor"],
               ["byte_logbound", 128],
               ["raw_byte_buffer", true],
               ["score_list_materialized", false],
               ["center", center],
               ["radius", radius],
               ["sieve_len", sieve_len],
               ["score_threshold", threshold],
               ["survivors", survivors],
               ["root_filters", root_filters],
               ["candidate_count", candidate_count],
               ["survivor_count", survivors.len],
               ["skipped_count", skipped_count],
               ["trial_division_avoidance_count", skipped_count],
               ["marked_roots", load64_i(totals_raw, 16)],
               ["skipped_noninvertible", skipped_noninvertible],
               ["max_score", max_score],
               ["avg_score_x1000", candidate_count > 0 ? (sum_score * 1000) / candidate_count : 0],
               ["survivor_rate_x1000", candidate_count > 0 ? (survivors.len * 1000) / candidate_count : 0],
            ], _mpqs_byte_sieve_tail_fields(tiny_limit, load64_i(totals_raw, 8), load64_i(totals_raw, 0), buckets, load64_i(totals_raw, 24), load64_i(totals_raw, 32), load64_i(totals_raw, 40), score_loads, sieve_len)))
   }
}

fn _mpqs_raw_byte_sieve_score_window_int_roots(ptr scratch, ptr root_filters, list<int> base_ints, list<int> sqrt_roots_int, list<int> center_mods, int radius) any {
   def sieve_len = radius * 2 + 1
   memset(scratch, 0, sieve_len)
   mut i = 0
   while i < base_ints.len {
      def ko = i * 32
      def p = base_ints[i]
      if p > 2 {
         def r = i < sqrt_roots_int.len ? sqrt_roots_int[i] : -1
         if r >= 0 {
            def cm = center_mods[i]
            def root1 = _mpqs_mod_int_norm(r - cm, p)
            def root2 = _mpqs_mod_int_norm(0 - r - cm, p)
            store64_i(root_filters, p, ko)
            store64_i(root_filters, root1, ko + 8)
            store64_i(root_filters, root2, ko + 16)
            store64_i(root_filters, 1, ko + 24)
            def start_rem = _mpqs_raw_byte_sieve_start_rem(radius, p)
            _mpqs_raw_byte_sieve_add_pos(scratch, sieve_len, p, _mpqs_raw_byte_sieve_root_pos(start_rem, p, root1))
            if root2 != root1 { _mpqs_raw_byte_sieve_add_pos(scratch, sieve_len, p, _mpqs_raw_byte_sieve_root_pos(start_rem, p, root2)) }
         } else {
            store64_i(root_filters, p, ko)
            store64_i(root_filters, 0, ko + 8)
            store64_i(root_filters, 0, ko + 16)
            store64_i(root_filters, 0, ko + 24)
         }
      } else {
         store64_i(root_filters, p, ko)
         store64_i(root_filters, 0, ko + 8)
         store64_i(root_filters, 0, ko + 16)
         store64_i(root_filters, 0, ko + 24)
      }
      i += 1
   }
   nil
}

fn _mpqs_popcount_mask(int mask) int {
   mut x, c = mask, 0
   while x > 0 {
      if (x & 1) != 0 { c += 1 }
      x = x >> 1
   }
   c
}

fn _mpqs_list_has_z(list xs, any value) bool {
   def vz = _z(value)
   mut i = 0
   while i < xs.len {
      if _z(xs.get(i)) == vz { return true }
      i += 1
   }
   false
}

fn _mpqs_base_without_pool(list base, list pool) list {
   mut out = []
   mut i = 0
   while i < base.len {
      def p = base[i]
      if !_mpqs_list_has_z(pool, p) { out = out.append(p) }
      i += 1
   }
   out
}

fn _mpqs_adiv_count_for_base(int usable_count) int {
   if usable_count < 5 { return max(1, usable_count) }
   if usable_count < 18 { return 2 }
   if usable_count < 48 { return 3 }
   if usable_count < 96 { return 4 }
   5
}

fn _mpqs_source_start_bits(int a_bits) int {
   if a_bits > 210 { return 15 }
   if a_bits > 190 { return 13 }
   if a_bits > 180 { return 12 }
   11
}

fn _mpqs_prime_bits(any p) int { bit_length(_z(p)) }

fn _mpqs_has_prime_bits(list usable, int bits) bool {
   mut i = 0
   while i < usable.len {
      if _mpqs_prime_bits(usable.get(i)) == bits { return true }
      i += 1
   }
   false
}

fn _mpqs_bit_bucket_counts(list usable) dict {
   mut counts = dict()
   mut i = 0
   while i < usable.len {
      def b = _mpqs_prime_bits(usable.get(i))
      if b <= 15 {
         def key = to_str(b)
         counts = counts.set(key, int(counts.get(key, 0)) + 1)
      }
      i += 1
   }
   counts
}

fn _mpqs_source_adjust_factor_bits(list bits) list {
   def n = bits.len
   if n < 8 || n >= 15 { return bits }
   if int(bits.get(0, 0)) > int(bits.get(n - 1, 0)) {
      if n > 9 {
         bits[3] = int(bits.get(3, 0)) - 1
         bits[2] = int(bits.get(2, 0)) - 1
      }
      if n >= 9 { bits[1] = int(bits.get(1, 0)) - 1 }
      bits[0] = int(bits.get(0, 0)) - 1
   } else {
      if n > 9 {
         bits[n - 4] = int(bits.get(n - 4, 0)) - 1
         bits[n - 3] = int(bits.get(n - 3, 0)) - 1
      }
      if n >= 9 { bits[n - 2] = int(bits.get(n - 2, 0)) - 1 }
      bits[n - 1] = int(bits.get(n - 1, 0)) - 1
   }
   bits
}

fn _mpqs_source_factor_bit_schedule(list usable, int a_bits) dict {
   def start_bits = _mpqs_source_start_bits(a_bits)
   def counts = _mpqs_bit_bucket_counts(usable)
   mut chosen_bits = 0
   mut num_factors = 0
   mut rem = 0
   mut i = start_bits
   while i >= 7 {
      num_factors = a_bits / i
      rem = a_bits % i
      if _mpqs_has_prime_bits(usable, i) && num_factors != 1 {
         if rem == 0 {
            if num_factors > 2 && _mpqs_has_prime_bits(usable, i + 1) {
               chosen_bits = i
               break
            }
         } else if rem <= num_factors {
            if num_factors > 2 && _mpqs_has_prime_bits(usable, i + 1) && _mpqs_has_prime_bits(usable, i + 2) {
               chosen_bits = i
               break
            }
         } else if (i - rem) <= num_factors {
            if _mpqs_has_prime_bits(usable, i + 1) && _mpqs_has_prime_bits(usable, i - 1) {
               chosen_bits = i
               break
            }
         }
      }
      i -= 1
   }
   if chosen_bits < 7 || num_factors < 2 {
      return _dict_with(10, [
            ["source_poly_init_model", false],
            ["schedule_found", false],
            ["a_bits", a_bits],
            ["start_bits", start_bits],
            ["bit_bucket_counts", counts],
            ["reason", "factor-bit schedule not available for current factor base"],
         ])
   }
   mut bits = []
   mut j = 0
   while j < num_factors {
      bits = bits.append(chosen_bits)
      j += 1
   }
   if rem <= num_factors {
      j = 0
      while j < rem {
         bits[j] = int(bits.get(j, chosen_bits)) + 1
         j += 1
      }
   } else {
      bits = bits.append(chosen_bits)
      num_factors += 1
      j = 0
      while j < (chosen_bits - rem) {
         bits[j] = int(bits.get(j, chosen_bits)) - 1
         j += 1
      }
   }
   bits = _mpqs_source_adjust_factor_bits(bits)
   mut bit_sum = 0
   j = 0
   while j < bits.len {
      bit_sum += int(bits.get(j, 0))
      j += 1
   }
   _dict_with(18, [
         ["source_poly_init_model", true],
         ["schedule_found", bit_sum > 0],
         ["a_bits", a_bits],
         ["start_bits", start_bits],
         ["chosen_base_bits", chosen_bits],
         ["factor_count", bits.len],
         ["factor_bits", bits],
         ["factor_bit_sum", bit_sum],
         ["target_bit_sum", a_bits],
         ["bit_bucket_counts", counts],
         ["derived_polynomial_count", 1 << max(0, bits.len - 1)],
      ])
}

fn _mpqs_poly_from_primes_with_signs(any modulus, list primes, int sign_mask) any {
   if primes.len == 0 { return nil }
   mut A = Z(1)
   def n = primes.len
   mut roots, mods, i = list(n), list(n), 0
   while i < n {
      def p, r = _z(primes.get(i)), tonelli_shanks(mod(modulus, p), p)
      if r == Z(-1) { return nil }
      def bit = (sign_mask >> i) & 1
      roots[i] = bit == 0 ? r : mod(Z(0) - r, p)
      mods[i] = p
      A = A * p
      i += 1
   }
   mut B = crt(roots, mods)
   if B == nil { return nil }
   B = mod(B, A)
   def Cnum = B * B - _z(modulus)
   if Cnum % A != Z(0) { return nil }
   _dict_with(14, [
         ["A", A], ["B", B], ["C", Cnum / A],
         ["prime_factors_A", primes], ["roots", roots], ["mods", mods],
         ["sign_mask", sign_mask], ["check_mod_A", mod(B * B - _z(modulus), A)],
      ])
}

fn _mpqs_adiv_product_from_mask(list pool, int mask) any {
   mut A, i = Z(1), 0
   while i < pool.len {
      if ((mask >> i) & 1) != 0 { A = A * _z(pool.get(i)) }
      i += 1
   }
   A
}

fn _mpqs_primes_from_mask(list pool, int mask) list {
   mut out = []
   mut i = 0
   while i < pool.len {
      if ((mask >> i) & 1) != 0 { out = out.append(pool.get(i)) }
      i += 1
   }
   out
}

fn _mpqs_a_divisor_target(any modulus, list base, int window_radius) dict {
   def usable = _siqs_residue_primes(modulus, base)
   def sieve_len = window_radius * 2 + 1
   mut target_A = isqrt(Z(8) * _z(modulus)) / Z(max(1, sieve_len))
   if target_A < Z(2) { target_A = Z(2) }
   def a_bits = bit_length(target_A)
   def schedule = _mpqs_source_factor_bit_schedule(usable, a_bits)
   def scheduled_count = int(schedule.get("factor_count", 0))
   def fallback_count = _mpqs_adiv_count_for_base(usable.len)
   def n_adiv = scheduled_count > 0 ? scheduled_count : fallback_count
   def n_total = min(10, min(usable.len, max(n_adiv, n_adiv + 4)))
   _dict_with(18, [
         ["usable", usable], ["adiv_count", n_adiv], ["adiv_total", n_total],
         ["sieve_len", sieve_len], ["target_A", target_A],
         ["a_bits", a_bits],
         ["target_prime", int(math.exp(math.log(float(target_A)) / float(max(1, n_adiv))) + 0.5)],
         ["source_poly_init_model", schedule.get("source_poly_init_model", false)],
         ["factor_bit_schedule_found", schedule.get("schedule_found", false)],
         ["factor_bit_schedule", schedule],
         ["factor_bits", schedule.get("factor_bits", [])],
         ["derived_polynomial_count", schedule.get("derived_polynomial_count", 0)],
      ])
}

fn _mpqs_a_divisor_pool(list usable, int n_total, int target_prime) dict {
   mut best_idx = 0
   mut best_dist = 0
   mut ui = 0
   while ui < usable.len {
      def pi = bigint_to_int(_z(usable.get(ui)))
      def d = pi > target_prime ? pi - target_prime : target_prime - pi
      if ui == 0 || d < best_dist {
         best_dist = d
         best_idx = ui
      }
      ui += 1
   }
   mut pool_start = best_idx - (n_total / 2)
   if pool_start < 0 { pool_start = 0 }
   if pool_start + n_total > usable.len { pool_start = max(0, usable.len - n_total) }
   mut pool = []
   ui = 0
   while ui < n_total {
      pool = pool.append(usable.get(pool_start + ui))
      ui += 1
   }
   _dict_with(4, [["pool_start", pool_start], ["pool", pool]])
}

fn _mpqs_pick_scheduled_prime(list usable, int bits, int target_prime, dict used) dict {
   mut best_idx = -1
   mut best_dist = 0
   mut i = 0
   while i < usable.len {
      def key = to_str(i)
      if !used.contains(key) && _mpqs_prime_bits(usable.get(i)) == bits {
         def pi = bigint_to_int(_z(usable.get(i)))
         def d = pi > target_prime ? pi - target_prime : target_prime - pi
         if best_idx < 0 || d < best_dist {
            best_idx = i
            best_dist = d
         }
      }
      i += 1
   }
   _dict_with(4, [["index", best_idx], ["distance", best_dist]])
}

fn _mpqs_a_divisor_pool_from_schedule(list usable, int n_total, int target_prime, list factor_bits) dict {
   def fallback = _mpqs_a_divisor_pool(usable, n_total, target_prime)
   if factor_bits.len == 0 {
      return _set_fields(fallback, [
            ["pool_strategy", "target-prime-window"],
            ["scheduled_prime_count", 0],
            ["fallback_prime_count", fallback.get("pool", []).len],
         ])
   }
   mut used = dict()
   mut pool = []
   mut i = 0
   while i < factor_bits.len {
      def picked = _mpqs_pick_scheduled_prime(usable, int(factor_bits.get(i, 0)), target_prime, used)
      def idx = int(picked.get("index", -1))
      if idx >= 0 {
         pool = pool.append(usable.get(idx))
         used = used.set(to_str(idx), true)
      }
      i += 1
   }
   def scheduled_count = pool.len
   def fallback_pool = fallback.get("pool", [])
   i = 0
   while i < fallback_pool.len && pool.len < n_total {
      def p = fallback_pool.get(i)
      if !_mpqs_list_has_z(pool, p) { pool = pool.append(p) }
      i += 1
   }
   i = 0
   while i < usable.len && pool.len < n_total {
      def p = usable.get(i)
      if !_mpqs_list_has_z(pool, p) { pool = pool.append(p) }
      i += 1
   }
   _dict_with(10, [
         ["pool_start", fallback.get("pool_start", 0)],
         ["pool", pool],
         ["pool_strategy", "source-factor-bit-schedule"],
         ["scheduled_prime_count", scheduled_count],
         ["fallback_prime_count", pool.len - scheduled_count],
      ])
}

fn _mpqs_best_adiv_mask(list pool, int n_adiv, any target_A, dict seen_masks) dict {
   mut best_mask = 0
   mut best_score = 0.0
   def mask_limit = pool.len > 0 ? (1 << pool.len) : 0
   mut mask = 1
   while mask < mask_limit {
      def key = to_str(mask)
      if _mpqs_popcount_mask(mask) == n_adiv && !seen_masks.contains(key) {
         def diff = math.log(float(_mpqs_adiv_product_from_mask(pool, mask))) - math.log(float(target_A))
         def score = diff < 0.0 ? 0.0 - diff : diff
         if best_mask == 0 || score < best_score {
            best_mask = mask
            best_score = score
         }
      }
      mask += 1
   }
   _dict_with(4, [["mask", best_mask], ["score", best_score]])
}

fn _mpqs_append_sign_polys(list polys_in, any modulus, list primes, int best_mask, any target_A, int target_prime, any best_score, int max_polynomials) dict {
   mut polys = polys_in
   mut root_precomputes = 0
   mut inverse_precomputes = 0
   def sign_limit = max(1, 1 << max(0, primes.len - 1))
   mut sign = 0
   while sign < sign_limit && polys.len < max_polynomials {
      def poly = _mpqs_poly_from_primes_with_signs(modulus, primes, sign)
      if poly != nil {
         root_precomputes += primes.len
         inverse_precomputes += primes.len
         polys = polys.append(_set_fields(poly, [
                  ["index", polys.len], ["mask", best_mask],
                  ["target_A", target_A], ["target_prime", target_prime],
                  ["score", best_score],
               ]))
      }
      sign += 1
   }
   _dict_with(6, [
         ["polys", polys],
         ["root_precomputes", root_precomputes],
         ["inverse_precomputes", inverse_precomputes],
      ])
}

fn _mpqs_build_adiv_polys(any modulus, list pool, int n_adiv, any target_A, int target_prime, int max_polynomials) dict {
   mut polys = []
   mut seen_masks = dict()
   mut root_precomputes = 0
   mut inverse_precomputes = 0
   mut mask_count = 0
   def mask_limit = pool.len > 0 ? (1 << pool.len) : 0
   while polys.len < max_polynomials && seen_masks.len < mask_limit {
      def best = _mpqs_best_adiv_mask(pool, n_adiv, target_A, seen_masks)
      def best_mask = int(best.get("mask", 0))
      if best_mask == 0 { break }
      seen_masks = seen_masks.set(to_str(best_mask), true)
      mask_count += 1
      def added = _mpqs_append_sign_polys(polys, modulus, _mpqs_primes_from_mask(pool, best_mask), best_mask, target_A, target_prime, best.get("score", 0.0), max_polynomials)
      polys = added.get("polys", polys)
      root_precomputes += int(added.get("root_precomputes", 0))
      inverse_precomputes += int(added.get("inverse_precomputes", 0))
   }
   _dict_with(8, [
         ["polys", polys], ["mask_count", mask_count],
         ["root_precomputes", root_precomputes],
         ["inverse_precomputes", inverse_precomputes],
      ])
}

fn _mpqs_a_divisor_cycle_from_base(any modulus, list base, int window_radius, int max_polynomials) dict {
   def t0 = ticks()
   def target = _mpqs_a_divisor_target(modulus, base, window_radius)
   def usable = target.get("usable", [])
   def pool_report = _mpqs_a_divisor_pool_from_schedule(usable, int(target.get("adiv_total", 0)), int(target.get("target_prime", 0)), target.get("factor_bits", []))
   def pool = pool_report.get("pool", [])
   def sieve_base = _mpqs_base_without_pool(base, pool)
   def built = _mpqs_build_adiv_polys(modulus, pool, int(target.get("adiv_count", 0)), target.get("target_A"), int(target.get("target_prime", 0)), max_polynomials)
   def polys = built.get("polys", [])
   _finish_report_with(_report("mpqs-a-divisor-cycle", 42), t0, [
         ["source_model", "MPQS A-divisor target-size cycling"],
         ["source_poly_init_model", target.get("source_poly_init_model", false)],
         ["factor_bit_schedule_found", target.get("factor_bit_schedule_found", false)],
         ["factor_bit_schedule", target.get("factor_bit_schedule", dict())],
         ["factor_bits", target.get("factor_bits", [])],
         ["a_bits", target.get("a_bits", 0)],
         ["derived_polynomial_count", target.get("derived_polynomial_count", 0)],
         ["sieve_len", target.get("sieve_len")],
         ["window_radius", window_radius],
         ["usable_prime_count", usable.len],
         ["adiv_count", target.get("adiv_count")],
         ["adiv_total", pool.len],
         ["pool_start", pool_report.get("pool_start")],
         ["pool_strategy", pool_report.get("pool_strategy", "target-prime-window")],
         ["scheduled_prime_count", pool_report.get("scheduled_prime_count", 0)],
         ["fallback_prime_count", pool_report.get("fallback_prime_count", 0)],
         ["target_A", target.get("target_A")],
         ["target_prime", target.get("target_prime")],
         ["adiv_pool", pool],
         ["sieve_base", sieve_base],
         ["sieve_base_size", sieve_base.len],
         ["removed_from_sieve_base_count", base.len - sieve_base.len],
         ["mask_count", built.get("mask_count", 0)],
         ["polynomial_count", polys.len],
         ["polynomials", polys],
         ["root_precompute_count", built.get("root_precomputes", 0)],
         ["inverse_precompute_count", built.get("inverse_precomputes", 0)],
         ["shared_factor_base", true],
         ["success", polys.len > 0],
      ])
}

fn mpqs_a_divisor_cycle_report(any n, int factor_base_bound=337, int window_radius=4000, int max_polynomials=8, int multiplier=0) dict {
   "Report source-style MPQS A-divisor selection, CRT root setup, and shared sieve base."
   def t0 = ticks()
   def nz = _z(n)
   def selected_multiplier = multiplier > 0 ? multiplier : int(qs_multiplier_report(nz, factor_base_bound).get("best_multiplier", 1))
   def modulus = nz * Z(selected_multiplier)
   def base_report = _qs_factor_base_report(nz, modulus, factor_base_bound)
   if base_report.get("factor", nil) != nil {
      return _finish_report_with(_report("mpqs-a-divisor-cycle", 12), t0, [
            ["n", nz], ["multiplier", selected_multiplier],
            ["factor_base_bound", factor_base_bound],
            ["factor", base_report.get("factor")], ["success", true],
            ["reason", "factor found while building factor base"],
         ])
   }
   def cycle = _mpqs_a_divisor_cycle_from_base(modulus, base_report.get("base", []), window_radius, max_polynomials)
   _finish_report_with(cycle, t0, [
         ["n", nz],
         ["multiplier", selected_multiplier],
         ["sieve_modulus", modulus],
         ["factor_base_bound", factor_base_bound],
         ["factor_base", base_report.get("base", [])],
         ["factor_base_size", base_report.get("base", []).len],
         ["factor_base_report", base_report],
      ])
}

fn _mpqs_poly_byte_sieve_window_into(ptr scratch, list<int> sieve_base_ints, list<int> root_base_ints, list<int> root_sqrt_roots_int, dict buckets, any A, any B, int radius, int threshold) dict {
   def sieve_len = radius * 2 + 1
   memset(scratch, 0, sieve_len)
   def tiny_limit = max(1, sieve_len / 16)
   mut skipped_noninvertible = 0
   mut list<int> root_filters = list(root_base_ints.len * 4)
   def A_z, B_z = _z(A), _z(B)
   def A_abs, B_abs, int_mod_path = A_z < Z(0) ? -A_z : A_z, B_z < Z(0) ? -B_z : B_z, bit_length(A_abs) <= 62 && bit_length(B_abs) <= 62
   def A_i, B_i = int_mod_path ? bigint_to_int(A_z) : 0, int_mod_path ? bigint_to_int(B_z) : 0
   with ptr totals_raw = malloc(48){
      if !totals_raw { panic("mpqs byte-sieve counter allocation failed") }
      memset(totals_raw, 0, 48)
      store64_i(totals_raw, 1, 0)
      mut i, mark_i = 0, 0
      while i < root_base_ints.len {
         def k, p = i * 4, root_base_ints[i]
         while mark_i < sieve_base_ints.len && sieve_base_ints[mark_i] < p { mark_i += 1 }
         def mark_root = mark_i < sieve_base_ints.len && sieve_base_ints[mark_i] == p
         if p > 2 {
            def Am = int_mod_path ? _mpqs_mod_int_norm(A_i, p) : bigint_to_int(mod(A_z, Z(p)))
            if Am == 0 {
               skipped_noninvertible += 1
               root_filters[k] = p
               root_filters[k + 1] = 0
               root_filters[k + 2] = 0
               root_filters[k + 3] = 0
            } else {
               def r = i < root_sqrt_roots_int.len ? root_sqrt_roots_int[i] : -1
               if r < 0 {
                  skipped_noninvertible += 1
                  root_filters[k] = p
                  root_filters[k + 1] = 0
                  root_filters[k + 2] = 0
                  root_filters[k + 3] = 0
               } else {
                  def invA = _mpqs_inv_mod_int(Am, p)
                  if invA == 0 {
                     skipped_noninvertible += 1
                     root_filters[k] = p
                     root_filters[k + 1] = 0
                     root_filters[k + 2] = 0
                     root_filters[k + 3] = 0
                  } else {
                     def Bm = int_mod_path ? _mpqs_mod_int_norm(B_i, p) : bigint_to_int(mod(B_z, Z(p)))
                     def root1 = _mpqs_mod_int_norm((r - Bm) * invA, p)
                     def root2 = _mpqs_mod_int_norm((0 - r - Bm) * invA, p)
                     root_filters[k] = p
                     root_filters[k + 1] = root1
                     root_filters[k + 2] = root2
                     root_filters[k + 3] = 1
                     if mark_root { _mpqs_byte_sieve_add_roots_raw(scratch, sieve_len, radius, p, root1, root2, tiny_limit, totals_raw) }
                  }
               }
            }
         } else {
            root_filters[k] = p
            root_filters[k + 1] = 0
            root_filters[k + 2] = 0
            root_filters[k + 3] = 0
         }
         i += 1
      }
      def scan = _mpqs_byte_sieve_survivor_stats(scratch, sieve_len, radius, threshold)
      def survivors = scan.get("survivors", [])
      _dict_with(40, _fields_extend([
               ["method", "mpqs-byte-sieve-window"],
               ["collector", "mpqs-byte-block-survivor"],
               ["polynomial_source", "mpqs-a-divisor-cycle"],
               ["byte_logbound", 128],
               ["raw_byte_buffer", true],
               ["score_list_materialized", false],
               ["A", A], ["B", B],
               ["center", B],
               ["radius", radius],
               ["sieve_len", sieve_len],
               ["score_threshold", threshold],
               ["survivors", survivors],
               ["root_filters", root_filters],
               ["candidate_count", sieve_len],
               ["survivor_count", survivors.len],
               ["skipped_count", scan.get("skipped_count", 0)],
               ["trial_division_avoidance_count", scan.get("skipped_count", 0)],
               ["marked_roots", load64_i(totals_raw, 16)],
               ["skipped_noninvertible", skipped_noninvertible],
               ["max_score", scan.get("max_score", 0)],
               ["avg_score_x1000", sieve_len > 0 ? (int(scan.get("sum_score", 0)) * 1000) / sieve_len : 0],
               ["survivor_rate_x1000", sieve_len > 0 ? (survivors.len * 1000) / sieve_len : 0],
            ], _mpqs_byte_sieve_tail_fields(tiny_limit, load64_i(totals_raw, 8), load64_i(totals_raw, 0), buckets, load64_i(totals_raw, 24), load64_i(totals_raw, 32), load64_i(totals_raw, 40), int(scan.get("score_loads", 0)), sieve_len)))
   }
}

fn _mpqs_byte_sieve_window_summary(dict rep, int window, int relations_found) dict {
   _dict_with(40, [
         ["window", window],
         ["method", rep.get("method", "")],
         ["collector", rep.get("collector", "")],
         ["polynomial_source", rep.get("polynomial_source", "")],
         ["A", rep.get("A", nil)],
         ["B", rep.get("B", nil)],
         ["raw_byte_buffer", rep.get("raw_byte_buffer", false)],
         ["score_list_materialized", rep.get("score_list_materialized", true)],
         ["center", rep.get("center", Z(0))],
         ["radius", rep.get("radius", 0)],
         ["sieve_len", rep.get("sieve_len", 0)],
         ["score_threshold", rep.get("score_threshold", 0)],
         ["relations", relations_found],
         ["candidate_count", rep.get("candidate_count", 0)],
         ["survivor_count", rep.get("survivor_count", 0)],
         ["skipped_count", rep.get("skipped_count", 0)],
         ["trial_division_avoidance_count", rep.get("trial_division_avoidance_count", 0)],
         ["marked_roots", rep.get("marked_roots", 0)],
         ["max_score", rep.get("max_score", 0)],
         ["survivor_rate_x1000", rep.get("survivor_rate_x1000", 0)],
         ["tiny_prime_count", rep.get("tiny_prime_count", 0)],
         ["bucket_count", rep.get("bucket_count", 0)],
         ["byte_adds_estimate", rep.get("byte_adds_estimate", 0)],
         ["prefill_byte_adds", rep.get("prefill_byte_adds", 0)],
         ["bucket_byte_adds", rep.get("bucket_byte_adds", 0)],
         ["score_loads", rep.get("score_loads", 0)],
         ["score_stores", rep.get("score_stores", 0)],
         ["score_buffer_bytes", rep.get("score_buffer_bytes", 0)],
         ["buffer_clears", rep.get("buffer_clears", 0)],
      ])
}

fn mpqs_byte_sieve_report(any n, int factor_base_bound=337, int windows=3, int window_radius=4000, int multiplier=0) dict {
   "Report the MPQS byte-sieve survivor pipeline: tiny-prime prefill, bucketed root marking, and threshold survivors."
   def t0 = ticks()
   def nz = _z(n)
   def selected_multiplier = multiplier > 0 ? multiplier : int(qs_multiplier_report(nz, factor_base_bound).get("best_multiplier", 1))
   def modulus = nz * Z(selected_multiplier)
   mut out = _set_fields(_report("mpqs-byte-sieve", 22), [
         ["n", nz],
         ["source_model", "MPQS byte-threshold survivor scan"],
         ["multiplier", selected_multiplier],
         ["factor_base_bound", factor_base_bound],
         ["windows", windows],
         ["window_radius", window_radius],
         ["sieve_len", window_radius * 2 + 1],
         ["byte_logbound", 128],
      ])
   if nz <= Z(1) {
      return _finish_report_with(out, t0, [["success", false], ["reason", "n must be greater than 1"]])
   }
   def base_report = _qs_factor_base_report(nz, modulus, factor_base_bound)
   def base = base_report.get("base", [])
   mut start = isqrt(modulus)
   if start * start < modulus { start = start + Z(1) }
   def sieve_len = window_radius * 2 + 1
   def scratch = malloc(sieve_len)
   if !scratch {
      return _finish_report_with(out, t0, [["success", false], ["reason", "byte sieve allocation failed"]])
   }
   def threshold = _mpqs_sieve_score_min(base)
   def stride = max(1, sieve_len)
   def sqrt_roots = _mpqs_sqrt_roots(modulus, base)
   def base_ints = _qs_factor_base_ints(base)
   def sqrt_roots_int = _mpqs_roots_ints(sqrt_roots)
   def start_mods = _mpqs_start_mods(base_ints, start, stride)
   def list<int> center_mods = start_mods[0]
   def list<int> stride_mods = start_mods[1]
   def buckets = _mpqs_bucketed_prime_loop_report(base, sieve_len)
   mut window_reports = []
   mut byte_totals = dict()
   mut max_score = 0
   mut center = start
   def center_stride = Z(stride)
   mut w = 0
   while w < windows {
      def window = _mpqs_byte_sieve_window_int_roots_into(scratch, base_ints, sqrt_roots_int, center_mods, buckets, center, window_radius, threshold, start)
      byte_totals = _mpqs_byte_totals_add(byte_totals, window)
      max_score = max(max_score, int(window.get("max_score", 0)))
      window_reports = window_reports.append(_mpqs_byte_sieve_window_summary(window, w, 0))
      _mpqs_advance_center_mods(center_mods, stride_mods, base_ints)
      center += center_stride
      w += 1
   }
   free(scratch)
   def fields = [
      ["success", true],
      ["factor_base", base],
      ["factor_base_size", base.len],
      ["factor_base_report", base_report],
      ["score_threshold", threshold],
      ["raw_byte_buffer", true],
      ["score_list_materialized", false],
   ]
   _finish_report_with(out, t0, _fields_extend(fields, _mpqs_direct_byte_total_fields(byte_totals, window_reports, window_radius, max_score)))
}

fn _qs_factor_over_base_profile(any v, list base) list {
   if !is_bigint(v) || bit_length(v) <= 62 { return _qs_factor_over_base_profile_int(is_bigint(v) ? bigint_to_int(v) : int(v), base) }
   mut rem = _z(v)
   def z0 = Z(0)
   def z1 = Z(1)
   mut idxs = nil
   mut vals = nil
   mut prime_tests = 0
   mut divisions = 0
   mut i = 0
   while i < base.len && rem != z1 {
      def p = base[i]
      mut e = 0
      while rem % p == z0 {
         rem = rem / p
         e += 1
         divisions += 1
      }
      prime_tests += 1
      if e > 0 {
         if idxs == nil {
            idxs = []
            vals = []
         }
         idxs = idxs.append(i)
         vals = vals.append(e)
      }
      i += 1
   }
   mut exps = []
   if rem == z1 {
      mut j = 0
      exps = list(base.len)
      while j < base.len {
         exps[j] = 0
         j += 1
      }
      if idxs != nil {
         mut nz = 0
         def idxs_n = idxs.len
         while nz < idxs_n {
            exps[int(idxs[nz])] = vals[nz]
            nz += 1
         }
      }
   }
   [rem == z1, exps, rem, prime_tests, divisions, idxs == nil ? 0 : idxs.len]
}

fn _qs_factor_over_base_profile_int(int v, list base) list {
   _qs_factor_over_base_profile_intbase(v, _qs_factor_base_ints(base))
}

@inline
fn _qs_factor_over_base_profile_intbase(int v, list<int> base) list {
   mut rem = v
   if rem < 0 { rem = 0 - rem }
   mut prime_tests = 0
   mut divisions = 0
   mut nonzero_terms = 0
   mut i = 0
   while i < base.len && rem != 1 {
      def p = base[i]
      if p > rem { break }
      mut e = 0
      while rem != 1 && rem % p == 0 {
         rem = rem / p
         e += 1
         divisions += 1
      }
      prime_tests += 1
      if e > 0 { nonzero_terms += 1 }
      i += 1
   }
   mut exps = []
   if rem == 1 {
      mut rem2 = v
      if rem2 < 0 { rem2 = 0 - rem2 }
      mut j = 0
      exps = list(base.len)
      while j < base.len {
         mut e = 0
         if rem2 != 1 {
            def p = base[j]
            if p != 0 {
               while rem2 % p == 0 {
                  rem2 = rem2 / p
                  e += 1
               }
            }
         }
         exps[j] = e
         j += 1
      }
   }
   [rem == 1, exps, rem, prime_tests, divisions, nonzero_terms]
}

@inline
fn _qs_factor_over_base_intbase_scan(int v, list<int> base, ptr counters) int {
   mut int rem = v
   if rem < 0 { rem = 0 - rem }
   mut int prime_tests = 0
   mut int divisions = 0
   mut int nonzero_terms = 0
   mut int i = 0
   def int n = base.len
   while i < n && rem != 1 {
      def int p = base[i]
      if p > rem { break }
      mut int e = 0
      while rem != 1 && rem % p == 0 {
         rem = rem / p
         e += 1
         divisions += 1
      }
      prime_tests += 1
      if e > 0 { nonzero_terms += 1 }
      i += 1
   }
   store64_i(counters, load64_i(counters, 0) + prime_tests, 0)
   store64_i(counters, load64_i(counters, 8) + divisions, 8)
   store64_i(counters, load64_i(counters, 16) + nonzero_terms, 16)
   rem
}

@inline
fn _siqs_relation_intbase(any x, any relation_value, int qv, list<int> base, list a_exps, bool include_detail=true) dict {
   mut rem = qv
   if rem < 0 { rem = 0 - rem }
   mut exps = list(base.len)
   mut j = 0
   def n = base.len
   def a_len = a_exps.len
   while j < n {
      def p = base[j]
      mut e = j < a_len ? int(a_exps[j]) : 0
      if p > rem {
         exps[j] = e
         j += 1
         continue
      }
      while rem != 1 && rem % p == 0 {
         rem = rem / p
         e += 1
      }
      exps[j] = e
      j += 1
   }
   include_detail ? {"x": x, "residue": relation_value, "exponents": exps} : {"x": x, "exponents": exps}
}

@inline
fn _siqs_relation_intbase_for_mode(any x, any A, int qv, list<int> base, list a_exps, bool include_detail) dict {
   if include_detail { return _siqs_relation_intbase(x, A * Z(qv), qv, base, a_exps, true) }
   _siqs_relation_intbase(x, nil, qv, base, a_exps, false)
}

fn _qs_factor_base_ints(list base) list<int> {
   mut out = list(base.len)
   mut i = 0
   while i < base.len {
      out[i] = is_bigint(base[i]) ? bigint_to_int(base[i]) : int(base[i])
      i += 1
   }
   out
}

fn _mpqs_sqrt_roots(any modulus, list base) list {
   mut roots = list(base.len)
   def no_root = Z(-1)
   mut i = 0
   while i < base.len {
      def pz = _z(base[i])
      def p = is_bigint(base[i]) ? bigint_to_int(base[i]) : int(base[i])
      if p > 2 {
         def r = tonelli_shanks(mod(modulus, pz), pz)
         roots[i] = r == no_root ? no_root : r
      } else {
         roots[i] = no_root
      }
      i += 1
   }
   roots
}

fn _mpqs_plain_root_filter(any modulus, list base, any center) list {
   mut roots = list(base.len * 4)
   mut i = 0
   while i < base.len {
      def k = i * 4
      def pz = _z(base[i])
      def p = bigint_to_int(pz)
      if p > 2 {
         def r = tonelli_shanks(mod(modulus, pz), pz)
         if r != Z(-1) {
            def cm = mod(center, pz)
            roots[k] = p
            roots[k + 1] = bigint_to_int(mod(r - cm, pz))
            roots[k + 2] = bigint_to_int(mod(Z(0) - r - cm, pz))
            roots[k + 3] = 1
         } else {
            roots[k] = p
            roots[k + 1] = 0
            roots[k + 2] = 0
            roots[k + 3] = 0
         }
      } else {
         roots[k] = p
         roots[k + 1] = 0
         roots[k + 2] = 0
         roots[k + 3] = 0
      }
      i += 1
   }
   roots
}

fn _mpqs_root_filter_hit(list roots, int i, int off) bool {
   def k = i * 4
   if k < 0 || k + 3 >= roots.len { return true }
   if int(roots[k + 3]) == 0 { return true }
   def p = int(roots[k])
   if p <= 0 { return true }
   mut om = off % p
   if om < 0 { om += p }
   om == int(roots[k + 1]) || om == int(roots[k + 2])
}

fn _qs_factor_over_base_profile_filtered(any v, list base, list root_filters, int off) list {
   if !is_bigint(v) || bit_length(v) <= 62 { return _qs_factor_over_base_profile_filtered_int(is_bigint(v) ? bigint_to_int(v) : int(v), base, root_filters, off) }
   mut rem = _z(v)
   def z0 = Z(0)
   def z1 = Z(1)
   mut idxs = nil
   mut vals = nil
   mut prime_tests = 0
   mut divisions = 0
   mut i = 0
   while i < base.len && rem != z1 {
      mut e = 0
      if _mpqs_root_filter_hit(root_filters, i, off) {
         def p = base[i]
         while rem % p == z0 {
            rem = rem / p
            e += 1
            divisions += 1
         }
         prime_tests += 1
      }
      if e > 0 {
         if idxs == nil {
            idxs = []
            vals = []
         }
         idxs = idxs.append(i)
         vals = vals.append(e)
      }
      i += 1
   }
   mut exps = []
   if rem == z1 {
      mut j = 0
      exps = list(base.len)
      while j < base.len {
         exps[j] = 0
         j += 1
      }
      if idxs != nil {
         mut nz = 0
         def idxs_n = idxs.len
         while nz < idxs_n {
            exps[int(idxs[nz])] = vals[nz]
            nz += 1
         }
      }
   }
   [rem == z1, exps, rem, prime_tests, divisions, idxs == nil ? 0 : idxs.len]
}

@inline
fn _qs_factor_over_base_profile_filtered_int(int v, list base, list root_filters, int off) list {
   mut rem = v
   if rem < 0 { rem = 0 - rem }
   mut prime_tests = 0
   mut divisions = 0
   mut nonzero_terms = 0
   mut i = 0
   while i < base.len && rem != 1 {
      mut e = 0
      def k = i * 4
      mut hit = true
      def p = int(root_filters[k])
      if p > rem { break }
      if int(root_filters[k + 3]) != 0 && p > 0 {
         mut om = off % p
         if om < 0 { om += p }
         hit = om == int(root_filters[k + 1]) || om == int(root_filters[k + 2])
      }
      if hit {
         while rem != 1 && rem % p == 0 {
            rem = rem / p
            e += 1
            divisions += 1
         }
         prime_tests += 1
      }
      if e > 0 { nonzero_terms += 1 }
      i += 1
   }
   mut exps = []
   if rem == 1 {
      mut rem2 = v
      if rem2 < 0 { rem2 = 0 - rem2 }
      mut j = 0
      exps = list(base.len)
      while j < base.len {
         mut e = 0
         if rem2 != 1 {
            def k = j * 4
            def p = int(root_filters[k])
            if p != 0 {
               while rem2 % p == 0 {
                  rem2 = rem2 / p
                  e += 1
               }
            }
         }
         exps[j] = e
         j += 1
      }
   }
   [rem == 1, exps, rem, prime_tests, divisions, nonzero_terms]
}

@inline
fn _qs_factor_over_base_filtered_int_raw_scan_limited(int v, int base_len, ptr root_filters, int off, int hit_limit, ptr counters) int {
   mut int rem = v
   if rem < 0 { rem = 0 - rem }
   mut int prime_tests = 0
   mut int divisions = 0
   mut int nonzero_terms = 0
   mut int i = 0
   def int n = base_len
   while i < n && rem != 1 && nonzero_terms < hit_limit {
      def int k = i * 32
      def int p = load64_i(root_filters, k)
      if p > rem { break }
      def int has_roots = load64_i(root_filters, k + 24)
      mut bool hit = true
      if has_roots != 0 {
         def int r1 = load64_i(root_filters, k + 8)
         def int r2 = load64_i(root_filters, k + 16)
         mut int om = off % p
         if om < 0 { om += p }
         hit = om == r1 || om == r2
      }
      if hit {
         mut bool divisible = rem % p == 0
         if divisible { nonzero_terms += 1 }
         while divisible && rem != 1 {
            rem = rem / p
            divisions += 1
            divisible = rem != 1 && rem % p == 0
         }
         prime_tests += 1
      }
      i += 1
   }
   store64_i(counters, load64_i(counters, 0) + prime_tests, 0)
   store64_i(counters, load64_i(counters, 8) + divisions, 8)
   store64_i(counters, load64_i(counters, 16) + nonzero_terms, 16)
   rem
}

@inline
fn _qs_factor_over_base_filtered_int_raw_scan_pos(int v, int base_len, ptr root_filters, int off, ptr counters) int {
   mut int rem = v
   if rem < 0 { rem = 0 - rem }
   mut int prime_tests = 0
   mut int divisions = 0
   mut int nonzero_terms = 0
   mut int i = 0
   def int n = base_len
   while i < n && rem != 1 {
      def int k = i * 32
      def int p = load64_i(root_filters, k)
      if p > rem { break }
      def int has_roots = load64_i(root_filters, k + 24)
      mut bool hit = true
      if has_roots != 0 {
         def int r1 = load64_i(root_filters, k + 8)
         def int r2 = load64_i(root_filters, k + 16)
         def int om = off % p
         hit = om == r1 || om == r2
      }
      if hit {
         mut bool divisible = rem % p == 0
         if divisible { nonzero_terms += 1 }
         while divisible && rem != 1 {
            rem = rem / p
            divisions += 1
            divisible = rem != 1 && rem % p == 0
         }
         prime_tests += 1
      }
      i += 1
   }
   store64_i(counters, load64_i(counters, 0) + prime_tests, 0)
   store64_i(counters, load64_i(counters, 8) + divisions, 8)
   store64_i(counters, load64_i(counters, 16) + nonzero_terms, 16)
   rem
}

@inline
fn _qs_factor_over_base_filtered_int_raw_scan_pos_collect32(int v, int base_len, ptr root_filters, int off, ptr counters, ptr idxs, ptr vals) int {
   mut int rem = v
   if rem < 0 { rem = 0 - rem }
   mut int prime_tests = 0
   mut int divisions = 0
   mut int nonzero_terms = 0
   mut int i = 0
   def int n = base_len
   while i < n && rem != 1 {
      def int k = i * 16
      def int p = load32(root_filters, k)
      if p > rem { break }
      def int has_roots = load32(root_filters, k + 12)
      mut bool hit = true
      if has_roots != 0 {
         def int r1 = load32(root_filters, k + 4)
         def int r2 = load32(root_filters, k + 8)
         def int om = off % p
         hit = om == r1 || om == r2
      }
      if hit {
         mut int e = 0
         mut bool divisible = rem % p == 0
         while divisible && rem != 1 {
            rem = rem / p
            divisions += 1
            e += 1
            divisible = rem != 1 && rem % p == 0
         }
         if e > 0 {
            def int dst = nonzero_terms * 8
            store64_i(idxs, i, dst)
            store64_i(vals, e, dst)
            nonzero_terms += 1
         }
         prime_tests += 1
      }
      i += 1
   }
   store64_i(counters, load64_i(counters, 0) + prime_tests, 0)
   store64_i(counters, load64_i(counters, 8) + divisions, 8)
   store64_i(counters, load64_i(counters, 16) + nonzero_terms, 16)
   store64_i(counters, nonzero_terms, 24)
   rem
}

@inline
fn _qs_factor_over_base_filtered_int_raw_scan_pos_limited(int v, int base_len, ptr root_filters, int off, int hit_limit, ptr counters) int {
   mut int rem = v
   if rem < 0 { rem = 0 - rem }
   mut int prime_tests = 0
   mut int divisions = 0
   mut int nonzero_terms = 0
   mut int i = 0
   def int n = base_len
   while i < n && rem != 1 && nonzero_terms < hit_limit {
      def int k = i * 32
      def int p = load64_i(root_filters, k)
      if p > rem { break }
      def int has_roots = load64_i(root_filters, k + 24)
      mut bool hit = true
      if has_roots != 0 {
         def int r1 = load64_i(root_filters, k + 8)
         def int r2 = load64_i(root_filters, k + 16)
         def int om = off % p
         hit = om == r1 || om == r2
      }
      if hit {
         mut bool divisible = rem % p == 0
         if divisible { nonzero_terms += 1 }
         while divisible && rem != 1 {
            rem = rem / p
            divisions += 1
            divisible = rem != 1 && rem % p == 0
         }
         prime_tests += 1
      }
      i += 1
   }
   store64_i(counters, load64_i(counters, 0) + prime_tests, 0)
   store64_i(counters, load64_i(counters, 8) + divisions, 8)
   store64_i(counters, load64_i(counters, 16) + nonzero_terms, 16)
   rem
}

@inline
fn _qs_factor_over_base_filtered_int_raw_scan_pos_limited_collect32(int v, int base_len, ptr root_filters, int off, int hit_limit, ptr counters, ptr idxs, ptr vals) int {
   mut int rem = v
   if rem < 0 { rem = 0 - rem }
   mut int prime_tests = 0
   mut int divisions = 0
   mut int nonzero_terms = 0
   mut int i = 0
   def int n = base_len
   while i < n && rem != 1 && nonzero_terms < hit_limit {
      def int k = i * 16
      def int p = load32(root_filters, k)
      if p > rem { break }
      def int has_roots = load32(root_filters, k + 12)
      mut bool hit = true
      if has_roots != 0 {
         def int r1 = load32(root_filters, k + 4)
         def int r2 = load32(root_filters, k + 8)
         def int om = off % p
         hit = om == r1 || om == r2
      }
      if hit {
         mut int e = 0
         mut bool divisible = rem % p == 0
         while divisible && rem != 1 {
            rem = rem / p
            divisions += 1
            e += 1
            divisible = rem != 1 && rem % p == 0
         }
         if e > 0 {
            def int dst = nonzero_terms * 8
            store64_i(idxs, i, dst)
            store64_i(vals, e, dst)
            nonzero_terms += 1
         }
         prime_tests += 1
      }
      i += 1
   }
   store64_i(counters, load64_i(counters, 0) + prime_tests, 0)
   store64_i(counters, load64_i(counters, 8) + divisions, 8)
   store64_i(counters, load64_i(counters, 16) + nonzero_terms, 16)
   store64_i(counters, nonzero_terms, 24)
   rem
}

@inline
fn _qs_relation_int_raw_mpqs(int x, int residue, list<int> base, ptr root_filters) dict {
   mut rem = residue
   if rem < 0 { rem = 0 - rem }
   mut exps = list(base.len)
   mut j = 0
   def n = base.len
   while j < n {
      mut e = 0
      if rem != 1 {
         def k = j * 32
         def p = load64_i(root_filters, k)
         while rem % p == 0 {
            rem = rem / p
            e += 1
         }
      }
      exps[j] = e
      j += 1
   }
   {"x": x, "exponents": exps}
}

@inline
fn _qs_relation_int_sparse_mpqs(int x, int base_len, ptr idxs, ptr vals, int nonzero_terms) dict {
   mut exps = list(base_len)
   mut j = 0
   while j < base_len {
      exps[j] = 0
      j += 1
   }
   j = 0
   while j < nonzero_terms {
      def int off = j * 8
      exps[load64_i(idxs, off)] = load64_i(vals, off)
      j += 1
   }
   {"x": x, "exponents": exps}
}

fn _mpqs_root_filter_list_to_raw_shifted(ptr raw, list roots, int count, int radius) any {
   mut i = 0
   while i < count {
      def src = i * 4
      def dst = i * 32
      def p = int(roots[src])
      store64_i(raw, p, dst)
      if p > 0 && int(roots[src + 3]) != 0 {
         mut start_rem = (0 - radius) % p
         if start_rem < 0 { start_rem += p }
         mut pos1 = int(roots[src + 1]) - start_rem
         mut pos2 = int(roots[src + 2]) - start_rem
         if pos1 < 0 { pos1 += p }
         if pos2 < 0 { pos2 += p }
         store64_i(raw, pos1, dst + 8)
         store64_i(raw, pos2, dst + 16)
         store64_i(raw, 1, dst + 24)
      } else {
         store64_i(raw, 0, dst + 8)
         store64_i(raw, 0, dst + 16)
         store64_i(raw, 0, dst + 24)
      }
      i += 1
   }
   nil
}

fn _mpqs_root_filter_list_to_raw_shifted32(ptr raw, list roots, int count, int radius) any {
   mut i = 0
   while i < count {
      def src = i * 4
      def dst = i * 16
      def p = int(roots[src])
      store32(raw, p, dst)
      if p > 0 && int(roots[src + 3]) != 0 {
         mut start_rem = (0 - radius) % p
         if start_rem < 0 { start_rem += p }
         mut pos1 = int(roots[src + 1]) - start_rem
         mut pos2 = int(roots[src + 2]) - start_rem
         if pos1 < 0 { pos1 += p }
         if pos2 < 0 { pos2 += p }
         store32(raw, pos1, dst + 4)
         store32(raw, pos2, dst + 8)
         store32(raw, 1, dst + 12)
      } else {
         store32(raw, 0, dst + 4)
         store32(raw, 0, dst + 8)
         store32(raw, 0, dst + 12)
      }
      i += 1
   }
   nil
}

fn _qs_factor_base_bigints(list base) list {
   mut out = list(base.len)
   mut i = 0
   while i < base.len {
      out[i] = _z(base[i])
      i += 1
   }
   out
}

fn _qs_relation(any x, any residue, list exps) dict {
   mut parity = list(exps.len)
   mut i = 0
   while i < exps.len {
      parity[i] = int(exps[i]) % 2
      i += 1
   }
   {"x": _z(x), "residue": _z(residue), "exponents": exps, "parity": parity}
}

@inline
fn _qs_relation_compact(any x, list exps) dict { {"x": x, "exponents": exps} }

fn _qs_parity_weight(list parity) int {
   mut w, i = 0, 0
   while i < parity.len {
      if (int(parity[i]) & 1) != 0 { w += 1 }
      i += 1
   }
   w
}

fn _qs_relation_parity(dict rel) list {
   def p = rel.get("parity", nil)
   p == nil ? rel.get("exponents", []) : p
}

fn _qs_zero_counts(int width) list {
   mut out = list(width)
   __list_set_len(out, width)
   mut i = 0
   while i < width {
      out[i] = 0
      i += 1
   }
   out
}

fn _qs_relation_width(list relations, int width) int {
   mut w = width
   if w > 0 { return w }
   mut i = 0
   while i < relations.len {
      def p = _qs_relation_parity(relations[i])
      if p.len > w { w = p.len }
      i += 1
   }
   w
}

fn _qs_unique_relation_report(list relations) dict {
   mut seen = dict()
   mut unique = list(relations.len)
   __list_set_len(unique, relations.len)
   mut unique_count = 0
   mut duplicate_x = 0
   mut zero_parity = 0
   mut max_weight = 0
   mut i = 0
   while i < relations.len {
      def rel = relations[i]
      def key = to_str(rel.get("x", ""))
      def wt = _qs_parity_weight(_qs_relation_parity(rel))
      if wt == 0 { zero_parity += 1 }
      if wt > max_weight { max_weight = wt }
      if seen.contains(key) {
         duplicate_x += 1
      } else {
         seen = seen.set(key, true)
         unique[unique_count] = rel
         unique_count += 1
      }
      i += 1
   }
   __list_set_len(unique, unique_count)
   _dict_with(8, [["relations", unique], ["duplicate_x", duplicate_x], ["zero_parity", zero_parity], ["max_weight", max_weight]])
}

fn _qs_has_singleton_parity(list parity, list counts, int width) bool {
   mut j = 0
   def limit = min(width, parity.len)
   while j < limit {
      if (int(parity[j]) & 1) != 0 && counts[j] <= 1 { return true }
      j += 1
   }
   false
}

fn _qs_singleton_prune_round(list kept, int width) dict {
   mut counts = _qs_zero_counts(width)
   mut parities = list(kept.len)
   __list_set_len(parities, kept.len)
   mut i = 0
   while i < kept.len {
      def parity = _qs_relation_parity(kept[i])
      parities[i] = parity
      mut j = 0
      def limit = min(width, parity.len)
      while j < limit {
         if (int(parity[j]) & 1) != 0 { counts[j] = counts[j] + 1 }
         j += 1
      }
      i += 1
   }
   mut next = list(kept.len)
   __list_set_len(next, kept.len)
   mut next_count = 0
   mut dropped = 0
   i = 0
   while i < kept.len {
      def rel = kept[i]
      if _qs_has_singleton_parity(parities[i], counts, width) {
         dropped += 1
      } else {
         next[next_count] = rel
         next_count += 1
      }
      i += 1
   }
   __list_set_len(next, next_count)
   _dict_with(6, [["relations", next], ["dropped", dropped], ["changed", dropped > 0]])
}

fn _qs_prune_singletons(list relations, int width, bool prune_singletons) dict {
   mut kept = relations
   mut rounds = 0
   mut dropped = 0
   mut changed = prune_singletons
   while changed {
      def step = _qs_singleton_prune_round(kept, width)
      changed = step.get("changed", false)
      if changed {
         kept = step.get("relations")
         dropped += int(step.get("dropped", 0))
         rounds += 1
      }
   }
   {"relations": kept, "rounds": rounds, "dropped": dropped}
}

fn qs_relation_filter_report(list relations, int width=0, bool prune_singletons=true) dict {
   "Filter QS/MPQS relations with duplicate-x removal and singleton-row pruning."
   def t0 = ticks()
   def w = _qs_relation_width(relations, width)
   def unique_report = _qs_unique_relation_report(relations)
   def unique = unique_report.get("relations", [])
   def pruned = _qs_prune_singletons(unique, w, prune_singletons)
   def kept = pruned.get("relations", unique)
   _report_with("qs-relation-filter", t0, [
         ["input_relations", relations.len],
         ["unique_relations", unique.len],
         ["filtered_relations", kept],
         ["filtered_count", kept.len],
         ["width", w], ["duplicate_x", unique_report.get("duplicate_x", 0)],
         ["zero_parity", unique_report.get("zero_parity", 0)],
         ["max_weight", unique_report.get("max_weight", 0)],
         ["singleton_prune", prune_singletons],
         ["singleton_rounds", pruned.get("rounds", 0)],
         ["singleton_dropped", pruned.get("dropped", 0)],
      ])
}

fn _qs_relation_large_primes(dict rel) list {
   mut raw = rel.get("large_primes", nil)
   if raw == nil { raw = rel.get("large_prime", nil) }
   mut out = []
   if is_list(raw) {
      mut i = 0
      while i < raw.len {
         def p = _z(raw.get(i, 1))
         if p > Z(1) { out = out.append(p) }
         i += 1
      }
   } else {
      def keys = ["lp0", "lp1", "lp2", "lp3", "large_prime0", "large_prime1", "large_prime2", "large_prime3"]
      mut i = 0
      while i < keys.len {
         def v = rel.get(keys.get(i), nil)
         if v != nil {
            def p = _z(v)
            if p > Z(1) { out = out.append(p) }
         }
         i += 1
      }
   }
   out
}

fn _qs_sort_large_primes(list lps) list {
   mut out = []
   mut i = 0
   while i < lps.len {
      def p = _z(lps.get(i))
      mut next = []
      mut inserted = false
      mut j = 0
      while j < out.len {
         def q = _z(out.get(j))
         if !inserted && p < q {
            next = next.append(p)
            inserted = true
         }
         next = next.append(q)
         j += 1
      }
      if !inserted { next = next.append(p) }
      out = next
      i += 1
   }
   out
}

fn _qs_large_prime_tuple_key(list lps) str {
   def sorted = _qs_sort_large_primes(lps)
   mut key = ""
   mut i = 0
   while i < sorted.len {
      if i > 0 { key = key + "," }
      key = key + to_str(sorted.get(i))
      i += 1
   }
   key
}

fn _qs_large_prime_relation_record(dict rel, int idx) dict {
   def lps = _qs_sort_large_primes(_qs_relation_large_primes(rel))
   _dict_with(8, [
         ["index", idx], ["relation", rel], ["large_primes", lps],
         ["large_prime_count", lps.len],
         ["large_prime_key", _qs_large_prime_tuple_key(lps)],
         ["x_key", to_str(rel.get("x", ""))],
      ])
}

fn _qs_lp_hist_set(dict hist, int k) dict { hist.set(to_str(k), int(hist.get(to_str(k), 0)) + 1) }

fn _qs_lp_normalize_square_pairs(list lps) dict {
   def sorted = _qs_sort_large_primes(lps)
   mut out = []
   mut square_pairs = 0
   mut i = 0
   while i < sorted.len {
      if i + 1 < sorted.len && _z(sorted.get(i)) == _z(sorted.get(i + 1)) {
         square_pairs += 1
         i += 2
      } else {
         out = out.append(sorted.get(i))
         i += 1
      }
   }
   _dict_with(4, [["large_primes", out], ["square_pairs", square_pairs]])
}

fn _qs_large_prime_acceptance_report(list relations, int max_large_primes=2, any max_large_prime=0) dict {
   mut accepted = []
   mut accepted_hist = dict()
   mut rejected_hist = dict()
   mut rejected_too_many = 0
   mut rejected_bound = 0
   mut square_closed = 0
   def bound = _z(max_large_prime)
   mut i = 0
   while i < relations.len {
      def rel = relations.get(i)
      def raw = _qs_relation_large_primes(rel)
      def normalized = _qs_lp_normalize_square_pairs(raw)
      def lps = normalized.get("large_primes", [])
      square_closed += int(normalized.get("square_pairs", 0))
      mut over_bound = false
      mut j = 0
      while j < lps.len && !over_bound {
         if bound > Z(0) && _z(lps.get(j)) > bound { over_bound = true }
         j += 1
      }
      if lps.len > max_large_primes {
         rejected_too_many += 1
         rejected_hist = _qs_lp_hist_set(rejected_hist, lps.len)
      } else if over_bound {
         rejected_bound += 1
         rejected_hist = _qs_lp_hist_set(rejected_hist, lps.len)
      } else {
         accepted_hist = _qs_lp_hist_set(accepted_hist, lps.len)
         accepted = accepted.append(rel.set("large_primes", lps))
      }
      i += 1
   }
   _dict_with(18, [
         ["source_model", "QS large-prime acceptance policy"],
         ["input_relations", relations.len],
         ["accepted_relations", accepted],
         ["accepted_relation_count", accepted.len],
         ["rejected_relation_count", relations.len - accepted.len],
         ["max_large_primes", max_large_primes],
         ["max_large_prime", max_large_prime],
         ["rejected_too_many_large_primes", rejected_too_many],
         ["rejected_large_prime_bound", rejected_bound],
         ["square_large_prime_pairs_closed", square_closed],
         ["accepted_large_prime_histogram", accepted_hist],
         ["rejected_large_prime_histogram", rejected_hist],
      ])
}

fn _qs_lp_unique_records(list relations) dict {
   mut unique = []
   mut seen_x, seen_lp = dict(), dict()
   mut duplicate_x, duplicate_large_prime_tuples = 0, 0
   mut full_relations, partial_relations, max_lp_seen = 0, 0, 0
   mut i = 0
   while i < relations.len {
      def rec = _qs_large_prime_relation_record(relations.get(i), i)
      def lp_count = int(rec.get("large_prime_count", 0))
      if lp_count > max_lp_seen { max_lp_seen = lp_count }
      if lp_count == 0 { full_relations += 1 } else { partial_relations += 1 }
      def x_key = rec.get("x_key", "")
      def lp_key = rec.get("large_prime_key", "")
      mut drop = false
      if x_key != "" && seen_x.contains(x_key) {
         duplicate_x += 1
         drop = true
      } else if x_key != "" {
         seen_x = seen_x.set(x_key, true)
      }
      if lp_count > 0 && seen_lp.contains(lp_key) {
         duplicate_large_prime_tuples += 1
         drop = true
      } else if lp_count > 0 {
         seen_lp = seen_lp.set(lp_key, true)
      }
      if !drop { unique = unique.append(rec) }
      i += 1
   }
   {"records": unique, "duplicate_x": duplicate_x, "duplicate_large_prime_tuples": duplicate_large_prime_tuples, "full_relations": full_relations, "partial_relations": partial_relations, "max_large_primes_seen": max_lp_seen}
}

fn _qs_lp_singleton_prune(list unique, bool prune_singletons) dict {
   mut kept = unique
   mut singleton_rounds, singleton_dropped = 0, 0
   mut changed = prune_singletons
   while changed {
      changed = false
      mut counts = dict()
      mut i = 0
      while i < kept.len {
         def lps = kept.get(i).get("large_primes", [])
         mut j = 0
         while j < lps.len {
            def key = to_str(lps.get(j))
            counts = counts.set(key, int(counts.get(key, 0)) + 1)
            j += 1
         }
         i += 1
      }
      mut next = []
      i = 0
      while i < kept.len {
         def rec = kept.get(i)
         def lps = rec.get("large_primes", [])
         mut drop = false
         mut j = 0
         while j < lps.len && !drop {
            if int(counts.get(to_str(lps.get(j)), 0)) <= 1 { drop = true }
            j += 1
         }
         if drop {
            singleton_dropped += 1
            changed = true
         } else {
            next = next.append(rec)
         }
         i += 1
      }
      if changed {
         kept = next
         singleton_rounds += 1
      }
   }
   {"records": kept, "singleton_rounds": singleton_rounds, "singleton_dropped": singleton_dropped}
}

fn _qs_lp_vertices(list records) list {
   mut seen, vertices = dict(), []
   mut i = 0
   while i < records.len {
      def lps = records.get(i).get("large_primes", [])
      mut j = 0
      while j < lps.len {
         def key = to_str(lps.get(j))
         if !seen.contains(key) {
            seen = seen.set(key, true)
            vertices = vertices.append(key)
         }
         j += 1
      }
      i += 1
   }
   vertices
}

fn _qs_lp_root(dict parent, str key) str {
   mut p = to_str(parent.get(key, key))
   while p != to_str(parent.get(p, p)) {
      p = to_str(parent.get(p, p))
   }
   p
}

fn _qs_lp_roots_same(list roots) bool {
   if roots.len <= 1 { return true }
   def first = roots.get(0)
   mut i = 1
   while i < roots.len {
      if roots.get(i) != first { return false }
      i += 1
   }
   true
}

fn _qs_lp_union_roots(dict parent, list vertices, list roots) dict {
   if roots.len == 0 { return parent }
   def first = roots.get(0)
   mut out = parent
   mut j = 0
   while j < roots.len {
      def old = roots.get(j)
      mut k = 0
      while k < vertices.len {
         def vk = vertices.get(k)
         if to_str(out.get(vk, vk)) == old { out = out.set(vk, first) }
         k += 1
      }
      j += 1
   }
   out
}

fn _qs_lp_cycle_graph(list kept) dict {
   def vertices = _qs_lp_vertices(kept)
   mut parent = dict()
   mut i = 0
   while i < vertices.len {
      parent = parent.set(vertices.get(i), vertices.get(i))
      i += 1
   }
   mut cycle_relations, cycle_hist, union_edges = [], dict(), 0
   i = 0
   while i < kept.len {
      def rec = kept.get(i)
      def lps = rec.get("large_primes", [])
      if lps.len >= 2 {
         mut roots = []
         mut j = 0
         while j < lps.len {
            roots = roots.append(_qs_lp_root(parent, to_str(lps.get(j))))
            j += 1
         }
         if _qs_lp_roots_same(roots) {
            cycle_relations = cycle_relations.append(rec)
            cycle_hist = _qs_lp_hist_set(cycle_hist, lps.len)
         } else {
            parent = _qs_lp_union_roots(parent, vertices, roots)
         }
         union_edges += 1
      }
      i += 1
   }
   mut component_seen, components = dict(), 0
   i = 0
   while i < vertices.len {
      def root = _qs_lp_root(parent, vertices.get(i))
      if !component_seen.contains(root) {
         component_seen = component_seen.set(root, true)
         components += 1
      }
      i += 1
   }
   {"vertices": vertices.len, "edges": union_edges, "components": components, "cycles_estimate": max(0, union_edges - max(0, vertices.len - components)), "cycle_relation_count": cycle_relations.len, "cycle_length_histogram": cycle_hist, "cycle_relations": cycle_relations}
}

fn _qs_lp_surviving_relations(list kept) dict {
   mut surviving_relations, surviving_partial_relations = [], 0
   mut i = 0
   while i < kept.len {
      def rec = kept.get(i)
      surviving_relations = surviving_relations.append(rec.get("relation"))
      if int(rec.get("large_prime_count", 0)) > 0 { surviving_partial_relations += 1 }
      i += 1
   }
   {"surviving_relations": surviving_relations, "surviving_partial_relations": surviving_partial_relations}
}

fn qs_large_prime_filter_report(list relations, int max_large_primes=2, bool prune_singletons=true, any max_large_prime=0) dict {
   "Filter QS/SIQS/MPQS large-prime partial relations and report graph cycles."
   def t0 = ticks()
   def acceptance = _qs_large_prime_acceptance_report(relations, max_large_primes, max_large_prime)
   def accepted_relations = acceptance.get("accepted_relations", [])
   def dedup = _qs_lp_unique_records(accepted_relations)
   def unique = dedup.get("records", [])
   def prune = _qs_lp_singleton_prune(unique, prune_singletons)
   def kept = prune.get("records", [])
   def graph = _qs_lp_cycle_graph(kept)
   def surviving = _qs_lp_surviving_relations(kept)
   _report_with("qs-large-prime-filter", t0, _fields_extend(_fields_extend([
               ["source_model", "QS large-prime acceptance, duplicate purge, singleton removal, cycle graph"],
               ["input_relations", relations.len], ["unique_relations", unique.len],
               ["full_relations", dedup.get("full_relations", 0)],
               ["partial_relations", dedup.get("partial_relations", 0)],
               ["max_large_primes_requested", max_large_primes],
               ["max_large_prime_bound", max_large_prime],
               ["max_large_primes_seen", dedup.get("max_large_primes_seen", 0)],
               ["accepted_relation_count", acceptance.get("accepted_relation_count", accepted_relations.len)],
               ["rejected_relation_count", acceptance.get("rejected_relation_count", 0)],
               ["rejected_too_many_large_primes", acceptance.get("rejected_too_many_large_primes", 0)],
               ["rejected_large_prime_bound", acceptance.get("rejected_large_prime_bound", 0)],
               ["large_prime_pair_closure_count", acceptance.get("square_large_prime_pairs_closed", 0)],
               ["square_large_prime_pairs_closed", acceptance.get("square_large_prime_pairs_closed", 0)],
               ["accepted_large_prime_histogram", acceptance.get("accepted_large_prime_histogram", dict())],
               ["rejected_large_prime_histogram", acceptance.get("rejected_large_prime_histogram", dict())],
               ["duplicate_x", dedup.get("duplicate_x", 0)],
               ["duplicate_large_prime_tuples", dedup.get("duplicate_large_prime_tuples", 0)],
               ["singleton_prune", prune_singletons],
               ["singleton_rounds", prune.get("singleton_rounds", 0)],
               ["singleton_dropped", prune.get("singleton_dropped", 0)],
               ["surviving_count", surviving.get("surviving_relations", []).len],
               ["graph_policy", "large-prime hyperedges with iterative singleton pruning"],
            ], [["surviving_relations", surviving.get("surviving_relations", [])], ["surviving_partial_relations", surviving.get("surviving_partial_relations", 0)]]), [
            ["vertices", graph.get("vertices", 0)], ["edges", graph.get("edges", 0)],
            ["components", graph.get("components", 0)], ["cycles_estimate", graph.get("cycles_estimate", 0)],
            ["cycle_relation_count", graph.get("cycle_relation_count", 0)],
            ["cycle_length_histogram", graph.get("cycle_length_histogram", dict())],
            ["cycle_relations", graph.get("cycle_relations", [])],
         ]))
}

fn _qs_clean_prime_list(list primes) list {
   mut out = []
   mut seen = dict()
   mut i = 0
   while i < primes.len {
      def p = _z(primes.get(i))
      if p > Z(1) {
         def key = to_str(p)
         if !seen.contains(key) {
            seen = seen.set(key, true)
            out = out.append(p)
         }
      }
      i += 1
   }
   out
}

fn _qs_prime_product_report(list primes) dict {
   mut prod = Z(1)
   mut minp = Z(0)
   mut maxp = Z(0)
   mut i = 0
   while i < primes.len {
      def p = _z(primes.get(i))
      prod = prod * p
      if minp == Z(0) || p < minp { minp = p }
      if p > maxp { maxp = p }
      i += 1
   }
   _dict_with(8, [
         ["prime_count", primes.len], ["prime_product", prod],
         ["prime_product_bits", bit_length(prod)],
         ["min_prime", minp], ["max_prime", maxp],
      ])
}

fn _qs_relation_cofactor_values(dict rel) list {
   mut out = []
   def direct = rel.get("cofactors", nil)
   if is_list(direct) {
      mut i = 0
      while i < direct.len {
         mut c = _z(direct.get(i))
         if c < Z(0) { c = -c }
         if c > Z(1) {
            out = out.append(_dict_with(4, [["field", "cofactors"], ["slot", i], ["value", c]]))
         }
         i += 1
      }
   }
   def keys = ["cofactor", "unfactored", "unfactored_r", "unfactored_a", "cofactor_r", "cofactor_a"]
   mut k = 0
   while k < keys.len {
      def key = keys.get(k)
      def v = rel.get(key, nil)
      if v != nil {
         mut c = _z(v)
         if c < Z(0) { c = -c }
         if c > Z(1) {
            out = out.append(_dict_with(4, [["field", key], ["slot", 0], ["value", c]]))
         }
      }
      k += 1
   }
   out
}

fn _qs_batch_work_items(list relations) list {
   mut items = []
   mut i = 0
   while i < relations.len {
      def rel = relations.get(i)
      def vals = _qs_relation_cofactor_values(rel)
      mut j = 0
      while j < vals.len {
         def v = vals.get(j)
         items = items.append(_dict_with(8, [
                  ["relation_index", i], ["cofactor_index", j],
                  ["field", v.get("field", "cofactor")], ["slot", v.get("slot", 0)],
                  ["cofactor", _z(v.get("value"))], ["cofactor_bits", bit_length(_z(v.get("value")))],
                  ["relation", rel],
               ]))
         j += 1
      }
      i += 1
   }
   items
}

fn _qs_batch_product_range(list items, int lo, int hi) any {
   mut prod = Z(1)
   mut i = lo
   while i <= hi && i < items.len {
      prod = prod * _z(items.get(i).get("cofactor"))
      i += 1
   }
   prod
}

fn _qs_batch_extract_smooth(any c, any g, list primes) dict {
   mut rem = _z(c)
   mut factors = []
   mut smooth = Z(1)
   mut trial_divisions = 0
   mut i = 0
   while i < primes.len {
      def p = _z(primes.get(i))
      if g % p == Z(0) {
         while rem % p == Z(0) {
            rem = rem / p
            smooth = smooth * p
            factors = factors.append(p)
            trial_divisions += 1
         }
      }
      i += 1
   }
   _dict_with(8, [
         ["smooth_part", smooth], ["remaining_cofactor", rem],
         ["factors", factors], ["factor_count", factors.len],
         ["trial_divisions", trial_divisions],
         ["fully_smooth", rem == Z(1)],
      ])
}

fn _qs_batch_process_leaf(list items, int lo, int hi, any numerator, list primes) dict {
   mut item_reports = []
   mut gcd_checks = 0
   mut full_smooth = 0
   mut partial_smooth = 0
   mut unsmoothed = 0
   mut extracted_factors = 0
   mut trial_divisions = 0
   mut i = lo
   while i <= hi && i < items.len {
      def item = items.get(i)
      def c = _z(item.get("cofactor"))
      def g = gcd(numerator, c)
      gcd_checks += 1
      def ex = _qs_batch_extract_smooth(c, g, primes)
      def smooth = _z(ex.get("smooth_part"))
      def rem = _z(ex.get("remaining_cofactor"))
      if rem == Z(1) { full_smooth += 1 }
      else if smooth > Z(1) { partial_smooth += 1 }
      else { unsmoothed += 1 }
      extracted_factors += int(ex.get("factor_count", 0))
      trial_divisions += int(ex.get("trial_divisions", 0))
      item_reports = item_reports.append(_dict_with(16, [
               ["item_index", i], ["relation_index", item.get("relation_index")],
               ["cofactor_index", item.get("cofactor_index")],
               ["field", item.get("field")], ["cofactor", c],
               ["cofactor_bits", item.get("cofactor_bits")],
               ["gcd_smooth_part", g], ["smooth_part", smooth],
               ["remaining_cofactor", rem], ["factors", ex.get("factors")],
               ["factor_count", ex.get("factor_count")],
               ["fully_smooth", rem == Z(1)],
            ]))
      i += 1
   }
   _dict_with(12, [
         ["item_reports", item_reports], ["gcd_checks", gcd_checks],
         ["full_smooth", full_smooth], ["partial_smooth", partial_smooth],
         ["unsmoothed", unsmoothed], ["extracted_factor_count", extracted_factors],
         ["trial_divisions", trial_divisions],
      ])
}

fn _qs_batch_merge_tree_reports(dict a, dict b) dict {
   _dict_with(20, [
         ["item_reports", a.get("item_reports", []) + b.get("item_reports", [])],
         ["tree_nodes", int(a.get("tree_nodes", 0)) + int(b.get("tree_nodes", 0))],
         ["product_nodes", int(a.get("product_nodes", 0)) + int(b.get("product_nodes", 0))],
         ["remainder_mods", int(a.get("remainder_mods", 0)) + int(b.get("remainder_mods", 0))],
         ["remainder_passthroughs", int(a.get("remainder_passthroughs", 0)) + int(b.get("remainder_passthroughs", 0))],
         ["leaf_count", int(a.get("leaf_count", 0)) + int(b.get("leaf_count", 0))],
         ["gcd_checks", int(a.get("gcd_checks", 0)) + int(b.get("gcd_checks", 0))],
         ["full_smooth", int(a.get("full_smooth", 0)) + int(b.get("full_smooth", 0))],
         ["partial_smooth", int(a.get("partial_smooth", 0)) + int(b.get("partial_smooth", 0))],
         ["unsmoothed", int(a.get("unsmoothed", 0)) + int(b.get("unsmoothed", 0))],
         ["extracted_factor_count", int(a.get("extracted_factor_count", 0)) + int(b.get("extracted_factor_count", 0))],
         ["trial_divisions", int(a.get("trial_divisions", 0)) + int(b.get("trial_divisions", 0))],
         ["max_depth", max(int(a.get("max_depth", 0)), int(b.get("max_depth", 0)))],
         ["max_relation_product_bits", max(int(a.get("max_relation_product_bits", 0)), int(b.get("max_relation_product_bits", 0)))],
         ["max_numerator_bits", max(int(a.get("max_numerator_bits", 0)), int(b.get("max_numerator_bits", 0)))],
      ])
}

fn _qs_batch_remainder_tree(list items, int lo, int hi, any numerator, list primes, int leaf_bits, int depth) dict {
   if lo > hi {
      return _dict_with(8, [["item_reports", []], ["tree_nodes", 0], ["max_depth", depth]])
   }
   def nbits = bit_length(numerator)
   if lo == hi || nbits <= leaf_bits {
      def leaf = _qs_batch_process_leaf(items, lo, hi, numerator, primes)
      return _set_fields(leaf, [
            ["tree_nodes", 1], ["product_nodes", 0], ["remainder_mods", 0],
            ["remainder_passthroughs", 0], ["leaf_count", 1],
            ["max_depth", depth], ["max_relation_product_bits", 0],
            ["max_numerator_bits", nbits],
         ])
   }
   def prod = _qs_batch_product_range(items, lo, hi)
   def pbits = bit_length(prod)
   mut next_num = numerator
   mut mods = 0
   mut passthroughs = 0
   if numerator >= prod {
      next_num = numerator % prod
      mods = 1
   } else {
      passthroughs = 1
   }
   def mid = (lo + hi) / 2
   def left = _qs_batch_remainder_tree(items, lo, mid, next_num, primes, leaf_bits, depth + 1)
   def right = _qs_batch_remainder_tree(items, mid + 1, hi, next_num, primes, leaf_bits, depth + 1)
   def merged = _qs_batch_merge_tree_reports(left, right)
   _set_fields(merged, [
         ["tree_nodes", int(merged.get("tree_nodes", 0)) + 1],
         ["product_nodes", int(merged.get("product_nodes", 0)) + 1],
         ["remainder_mods", int(merged.get("remainder_mods", 0)) + mods],
         ["remainder_passthroughs", int(merged.get("remainder_passthroughs", 0)) + passthroughs],
         ["max_relation_product_bits", max(int(merged.get("max_relation_product_bits", 0)), pbits)],
         ["max_numerator_bits", max(int(merged.get("max_numerator_bits", 0)), nbits)],
      ])
}

fn qs_batch_cofactor_report(list relations, list primes, int leaf_bits=1024) dict {
   "Batch-factor QS/SIQS/MPQS relation cofactors with a product/remainder-tree GCD pass."
   def t0 = ticks()
   def clean_primes = _qs_clean_prime_list(primes)
   def pp = _qs_prime_product_report(clean_primes)
   def items = _qs_batch_work_items(relations)
   mut out = _set_fields(_report("qs-batch-cofactorization", 24), [
         ["source_model", "Gerbicz product/remainder batch factor tree"],
         ["relation_count", relations.len],
         ["cofactor_item_count", items.len],
         ["leaf_bits", leaf_bits],
         ["prime_count", clean_primes.len],
         ["prime_product_bits", pp.get("prime_product_bits", 0)],
         ["min_prime", pp.get("min_prime", Z(0))],
         ["max_prime", pp.get("max_prime", Z(0))],
         ["tree_policy", "balanced remainder tree with direct leaf GCD extraction"],
      ])
   if items.len == 0 || clean_primes.len == 0 {
      return _finish_report_with(out, t0, [
            ["success_count", 0], ["partial_count", 0], ["unsmoothed_count", items.len],
            ["item_reports", []], ["success", false],
         ])
   }
   def tree = _qs_batch_remainder_tree(items, 0, items.len - 1, pp.get("prime_product"), clean_primes, max(8, leaf_bits), 0)
   def full_smooth = int(tree.get("full_smooth", 0))
   def partial_smooth = int(tree.get("partial_smooth", 0))
   def unsmoothed = int(tree.get("unsmoothed", 0))
   _finish_report_with(out, t0, [
         ["item_reports", tree.get("item_reports", [])],
         ["tree_nodes", tree.get("tree_nodes", 0)],
         ["product_nodes", tree.get("product_nodes", 0)],
         ["remainder_mods", tree.get("remainder_mods", 0)],
         ["remainder_passthroughs", tree.get("remainder_passthroughs", 0)],
         ["leaf_count", tree.get("leaf_count", 0)],
         ["max_depth", tree.get("max_depth", 0)],
         ["max_relation_product_bits", tree.get("max_relation_product_bits", 0)],
         ["max_numerator_bits", tree.get("max_numerator_bits", 0)],
         ["gcd_checks", tree.get("gcd_checks", 0)],
         ["trial_divisions", tree.get("trial_divisions", 0)],
         ["extracted_factor_count", tree.get("extracted_factor_count", 0)],
         ["success_count", full_smooth],
         ["partial_count", partial_smooth],
         ["unsmoothed_count", unsmoothed],
         ["success", full_smooth > 0],
      ])
}

fn _qs_congruence_factor(any x, any y, any n) any {
   def nz = _z(n)
   def gx = gcd(bigint_abs(_z(x) - _z(y)), nz)
   if _is_nontrivial_factor(gx, nz) { return gx }
   def gy = gcd(bigint_abs(_z(x) + _z(y)), nz)
   if _is_nontrivial_factor(gy, nz) { return gy }
   nil
}

fn _qs_try_verified_dependency_mod(any original_n, any modulus, list base, list relations, list dependency) any {
   def nz, mz = _z(original_n), _z(modulus)
   mut sums = list(base.len)
   mut i = 0
   def base_len = base.len
   while i < base_len {
      sums[i] = 0
      i += 1
   }
   mut X = Z(1)
   i = 0
   def rel_len = relations.len
   def dep_len = dependency.len
   while i < rel_len {
      if i < dep_len && (int(dependency[i]) & 1) == 1 {
         def rel = relations[i]
         X = mod(X * _z(rel.get("x")), mz)
         def exps = rel.get("exponents")
         mut j = 0
         def exp_len = exps.len
         while j < exp_len {
            sums[j] = int(sums[j]) + int(exps[j])
            j += 1
         }
      }
      i += 1
   }
   mut Y = Z(1)
   i = 0
   while i < base_len {
      def half = sums[i] / 2
      if half > 0 { Y = mod(Y * power_mod(_z(base[i]), Z(half), mz), mz) }
      i += 1
   }
   _qs_congruence_factor(X, Y, nz)
}

fn _qs_multiplier_candidates() list {
   [1, 2, 3, 5, 6, 7, 10, 11, 13, 14, 15, 17, 19, 21, 22, 23,
      26, 29, 30, 31, 33, 34, 35, 37, 38, 39, 41, 42, 43, 46, 47,
      51, 53, 55, 57, 58, 59, 61, 62, 65, 66, 67, 69, 70, 71, 73]
}

fn _qs_prime_base_int(int bound) list<int> {
   mut list<int> out = list(0)
   if bound < 2 { return out }
   mut composite = list(bound + 1)
   __list_set_len(composite, bound + 1)
   mut i = 0
   while i <= bound {
      composite[i] = false
      i += 1
   }
   mut p = 2
   while p <= bound {
      if !bool(composite[p]) {
         out = out.append(p)
         if p <= bound / p {
            mut k = p * p
            while k <= bound {
               composite[k] = true
               k += p
            }
         }
      }
      p += p == 2 ? 1 : 2
   }
   out
}

fn _qs_pow_mod_int(int a, int e, int p) int {
   mut base = a % p
   if base < 0 { base += p }
   mut exp = e
   mut acc = 1 % p
   while exp > 0 {
      if (exp & 1) != 0 { acc = (acc * base) % p }
      base = (base * base) % p
      exp = exp >> 1
   }
   acc
}

fn _qs_legendre_int(int a, int p) int {
   def r = a % p
   if r == 0 { return 0 }
   def v = _qs_pow_mod_int(r, (p - 1) / 2, p)
   v == 1 ? 1 : (v == p - 1 ? -1 : 0)
}

fn _qs_multiplier_score_int(int n_mod_8, list<int> n_mod_primes, int k, list<int> score_primes, list score_contribs) dict {
   def ln2 = 0.6931471805599453
   mut score = 0.5 * math.log(float(k))
   def kn8 = (k * n_mod_8) % 8
   if kn8 == 1 { score = score - 2.0 * ln2 }
   elif kn8 == 5 { score = score - ln2 }
   elif kn8 == 3 || kn8 == 7 { score = score - 0.5 * ln2 }
   mut qr_hits = 0
   mut div_hits = 0
   mut tested = 0
   mut i = 0
   while i < score_primes.len {
      def pi = int(score_primes[i])
      if pi > 2 {
         tested += 1
         def knp = (k * int(n_mod_primes[i])) % pi
         def contrib = float(score_contribs[i])
         if knp == 0 {
            score = score - contrib
            div_hits += 1
         } elif _qs_legendre_int(knp, pi) == 1 {
            score = score - 2.0 * contrib
            qr_hits += 1
         }
      }
      i += 1
   }
   {
      "k": k,
      "score": score,
      "kn_mod_8": kn8,
      "prime_tests": tested,
      "quadratic_residue_hits": qr_hits,
      "divisible_hits": div_hits,
   }
}

fn _qs_best_multiplier_fast(any n, int factor_base_bound) int {
   def nz = _z(n)
   def score_primes = _qs_prime_base_int(factor_base_bound)
   mut list<int> n_mod_primes = list(score_primes.len)
   mut score_contribs = list(score_primes.len)
   __list_set_len(n_mod_primes, score_primes.len)
   __list_set_len(score_contribs, score_primes.len)
   mut i = 0
   while i < score_primes.len {
      def p = score_primes[i]
      n_mod_primes[i] = bigint_to_int(mod(nz, Z(p)))
      score_contribs[i] = p > 2 ? math.log(float(p)) / (float(p) - 1.0) : 0.0
      i += 1
   }
   def candidates = _qs_multiplier_candidates()
   def n_mod_8 = bigint_to_int(mod(nz, Z(8)))
   def ln2 = 0.6931471805599453
   mut best_k = 1
   mut best_score = 1.0e100
   i = 0
   while i < candidates.len {
      def k = int(candidates[i])
      mut score = 0.5 * math.log(float(k))
      def kn8 = (k * n_mod_8) % 8
      if kn8 == 1 { score = score - 2.0 * ln2 }
      elif kn8 == 5 { score = score - ln2 }
      elif kn8 == 3 || kn8 == 7 { score = score - 0.5 * ln2 }
      mut j = 0
      while j < score_primes.len {
         def p = score_primes[j]
         if p > 2 {
            def knp = (k * n_mod_primes[j]) % p
            def contrib = float(score_contribs[j])
            if knp == 0 { score = score - contrib }
            elif _qs_legendre_int(knp, p) == 1 { score = score - 2.0 * contrib }
         }
         j += 1
      }
      if score < best_score {
         best_score = score
         best_k = k
      }
      i += 1
   }
   best_k
}

fn qs_multiplier_report(any n, int factor_base_bound=64) dict {
   "Score Knuth-Schroeppel multipliers for quadratic-sieve style relation collection."
   def t0 = ticks()
   def nz = _z(n)
   def score_primes_i = _qs_prime_base_int(factor_base_bound)
   mut list<int> n_mod_primes = list(score_primes_i.len)
   mut score_contribs = list(score_primes_i.len)
   __list_set_len(n_mod_primes, score_primes_i.len)
   __list_set_len(score_contribs, score_primes_i.len)
   mut pi = 0
   while pi < score_primes_i.len {
      def p = score_primes_i[pi]
      n_mod_primes[pi] = bigint_to_int(mod(nz, Z(p)))
      score_contribs[pi] = p > 2 ? math.log(float(p)) / (float(p) - 1.0) : 0.0
      pi += 1
   }
   def candidates = _qs_multiplier_candidates()
   mut best_k = 1
   mut best_score = 1.0e100
   mut trace = list(candidates.len)
   __list_set_len(trace, candidates.len)
   mut i = 0
   def n_mod_8 = bigint_to_int(mod(nz, Z(8)))
   while i < candidates.len {
      def k = int(candidates.get(i))
      def rep = _qs_multiplier_score_int(n_mod_8, n_mod_primes, k, score_primes_i, score_contribs)
      def s = rep.get("score", 0.0)
      trace[i] = rep
      if s < best_score {
         best_score = s
         best_k = k
      }
      i += 1
   }
   _report_with("knuth-schroeppel-multiplier", t0, [
         ["n", nz], ["factor_base_bound", factor_base_bound],
         ["score_prime_count", score_primes_i.len],
         ["candidate_count", candidates.len],
         ["best_multiplier", best_k],
         ["best_score", best_score],
         ["scores", trace],
      ])
}

fn mpqs_multiplier_report(any n, int factor_base_bound=64) dict {
   "Alias for qs_multiplier_report used by MPQS callers."
   qs_multiplier_report(n, factor_base_bound)
}

fn _qs_multiplier_summary_report(any n, int factor_base_bound, int best_multiplier) dict {
   _dict_with(12, [
         ["method", "knuth-schroeppel-multiplier"],
         ["n", _z(n)],
         ["factor_base_bound", factor_base_bound],
         ["best_multiplier", best_multiplier],
         ["scores", []],
         ["scores_elided", true],
      ])
}

fn _siqs_residue_primes(any modulus, list base) list {
   mut out = list(base.len)
   __list_set_len(out, base.len)
   mut count = 0
   mut i = 0
   while i < base.len {
      def p = _z(base[i])
      if p > Z(2) && gcd(p, modulus) == Z(1) && legendre(modulus, p) == 1 {
         out[count] = p
         count += 1
      }
      i += 1
   }
   __list_set_len(out, count)
   out
}

fn _siqs_poly_from_primes(any modulus, list primes) any {
   if primes.len == 0 { return nil }
   mut A = Z(1)
   mut roots = list(primes.len)
   mut mods = list(primes.len)
   __list_set_len(roots, primes.len)
   __list_set_len(mods, primes.len)
   mut i = 0
   while i < primes.len {
      def p, r = _z(primes.get(i)), tonelli_shanks(modulus, _z(primes.get(i)))
      if r == Z(-1) { return nil }
      A = A * p
      roots[i] = r
      mods[i] = p
      i += 1
   }
   mut B = crt(roots, mods)
   if B == nil { return nil }
   B = mod(B, A)
   if B > A / Z(2) { B = A - B }
   def num = B * B - _z(modulus)
   if num % A != Z(0) { return nil }
   def C = num / A
   _dict_with(10, [
         ["A", A], ["B", B], ["C", C],
         ["prime_factors_A", primes], ["roots", roots], ["mods", mods],
         ["check_mod_A", mod(B * B - _z(modulus), A)],
      ])
}

fn _siqs_polynomial_report(any n, int factor_base_bound, int polynomial_count, int sieve_radius, bool detailed_multiplier) dict {
   def t0 = ticks()
   def nz = _z(n)
   mut mult = dict()
   mut k = 1
   if detailed_multiplier {
      mult = qs_multiplier_report(nz, factor_base_bound)
      k = int(mult.get("best_multiplier", 1))
   } else {
      k = _qs_best_multiplier_fast(nz, factor_base_bound)
      mult = _qs_multiplier_summary_report(nz, factor_base_bound, k)
   }
   def modulus = nz * Z(k)
   def base = _qs_prime_base(factor_base_bound)
   def usable = _siqs_residue_primes(modulus, base)
   mut target_A = isqrt(modulus) / Z(max(1, sieve_radius))
   if target_A < Z(2) { target_A = Z(2) }
   mut polys = []
   mut seen = dict()
   mut start = 0
   while start < usable.len && polys.len < polynomial_count {
      mut primes = []
      mut A = Z(1)
      mut j = start
      while j < usable.len && A < target_A && primes.len < 6 {
         primes = primes.append(usable.get(j))
         A = A * _z(usable.get(j))
         j += 1
      }
      if primes.len > 0 {
         def poly = _siqs_poly_from_primes(modulus, primes)
         if poly != nil {
            def key = to_str(poly.get("A")) + ":" + to_str(poly.get("B"))
            if !seen.contains(key) {
               seen = seen.set(key, true)
               polys = polys.append(_set_fields(poly, [
                        ["index", polys.len], ["target_A", target_A],
                        ["radius", sieve_radius], ["score", bigint_abs(poly.get("A") - target_A)],
                     ]))
            }
         }
      }
      start += 1
   }
   _report_with("siqs-polynomial-generation", t0, [
         ["n", nz], ["multiplier", k], ["sieve_modulus", modulus],
         ["factor_base_bound", factor_base_bound], ["usable_prime_count", usable.len],
         ["target_A", target_A], ["sieve_radius", sieve_radius],
         ["polynomial_count", polys.len], ["polynomials", polys],
         ["multiplier_report", mult],
      ])
}

fn siqs_polynomial_report(any n, int factor_base_bound=64, int polynomial_count=8, int sieve_radius=256) dict {
   "Generate SIQS polynomial candidates Q(x)=(A*x+B)^2-kN with B^2 == kN mod A."
   _siqs_polynomial_report(n, factor_base_bound, polynomial_count, sieve_radius, true)
}

fn _siqs_relation_fields(dict poly, int pi, int t, any A, any B, any C, any qv, any relation_value, int multiplier, int score) list {
   [
      ["poly_index", poly.get("index", pi)], ["t", t],
      ["A", A], ["B", B], ["C", C],
      ["q_value", qv], ["relation_value", relation_value], ["multiplier", multiplier], ["sieve_score", score],
   ]
}

fn _siqs_poly_report_fields(dict poly, int pi, any A, any B, int found, int candidates, int smooth_tests, int prime_tests, int divisions, int skipped_negative, dict score_report) list {
   [
      ["poly_index", poly.get("index", pi)], ["A", A], ["B", B],
      ["relations", found], ["candidates", candidates], ["smooth_tests", smooth_tests], ["smooth_hits", found],
      ["trial_division_prime_tests", prime_tests], ["trial_divisions", divisions], ["skipped_nonpositive", skipped_negative],
      ["sieve_marked_roots", score_report.get("marked_roots", 0)],
      ["sieve_noninvertible", score_report.get("skipped_noninvertible", 0)],
   ]
}

fn _siqs_relation_final_fields(
   any rel_t0, int candidate_count, int smooth_tests, int smooth_hits, int trial_prime_tests,
   int trial_divisions, int nonzero_exponent_terms, int total_skipped_negative, int score_min, int default_score_min,
   bool tune_cutoff, dict cutoff_report, int sieve_marked_roots, int sieve_noninvertible,
   int prefilter_tested, int prefilter_skipped, int fallback_tests, list relations,
) list {
   _fields_extend(
      _qs_relation_collection_fields(rel_t0, candidate_count, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_exponent_terms),
      [
         ["skipped_nonpositive", total_skipped_negative],
         ["sieve_prefilter_enabled", true], ["sieve_score_min", score_min],
         ["default_sieve_score_min", default_score_min], ["cutoff_tuning_enabled", tune_cutoff],
         ["cutoff_tune_report", cutoff_report], ["cutoff_tune_changed", cutoff_report.get("changed", false)],
         ["cutoff_tune_measurements", cutoff_report.get("measurements", [])],
         ["sieve_marked_roots", sieve_marked_roots], ["sieve_noninvertible", sieve_noninvertible],
         ["prefilter_tested", prefilter_tested], ["prefilter_skipped", prefilter_skipped],
         ["fallback_tests", fallback_tests], ["success", relations.len > 0],
      ],
   )
}

fn _siqs_cutoff_disabled(int default_score_min) dict {
   _dict_with(8, [
         ["method", "siqs-adaptive-cutoff"], ["default_cutoff", default_score_min],
         ["selected_cutoff", default_score_min], ["changed", false],
         ["measurement_count", 0], ["measurements", []], ["selected_reason", "disabled"],
      ])
}

fn _siqs_collect_state() dict {
   _dict_with(24, [
         ["candidate_count", 0], ["smooth_tests", 0], ["smooth_hits", 0],
         ["trial_prime_tests", 0], ["trial_divisions", 0], ["nonzero_exponent_terms", 0],
         ["total_skipped_negative", 0], ["prefilter_skipped", 0], ["prefilter_tested", 0],
         ["fallback_tests", 0], ["sieve_marked_roots", 0], ["sieve_noninvertible", 0],
      ])
}

fn _siqs_poly_scan_result(
   list relations, dict state, dict poly, int pi, any A, any B, int found,
   int poly_candidates, int poly_smooth_tests, int poly_trial_prime_tests, int poly_trial_divisions,
   int skipped_negative, dict score_report, bool include_report,
) dict {
   _dict_with(8, [
         ["relations", relations], ["state", state],
         ["poly_report", include_report ? _dict_with(14, _siqs_poly_report_fields(poly, pi, A, B, found, poly_candidates, poly_smooth_tests, poly_trial_prime_tests, poly_trial_divisions, skipped_negative, score_report)) : nil],
      ])
}

fn _siqs_collect_poly_pass_int_raw_loop(
   ptr scores, ptr root_filters, list relations_in, int relation_count, dict poly, any A,
   int Ai, int Bi, int Ci, list base, list<int> trial_base, int sieve_radius,
   int score_min, int max_relations, bool pass0, bool has_root_filters,
   bool include_relation_details, ptr raw_counts
) dict {
   mut relations = relations_in
   mut a_exps = []
   mut a_exps_ready = false
   mut int found = 0
   mut int skipped_negative = 0
   mut int poly_candidates = 0
   mut int poly_smooth_tests = 0
   mut int candidate_count = 0
   mut int smooth_tests = 0
   mut int smooth_hits = 0
   mut int prefilter_skipped = 0
   mut int prefilter_tested = 0
   mut int fallback_tests = 0
   mut int t = 0 - sieve_radius
   def int twoA = 2 * Ai
   def int twoB = 2 * Bi
   mut int qv = Ai * t * t + twoB * t + Ci
   mut int q_delta = twoA * t + Ai + twoB
   def int trial_base_len = trial_base.len
   while t <= sieve_radius && relation_count < max_relations {
      def int pos = t + sieve_radius
      if pass0 {
         candidate_count += 1
         poly_candidates += 1
      }
      if qv > 0 {
         def int score = int(load8(scores, pos))
         def should_test = pass0 ? score >= score_min : score < score_min
         if should_test {
            if pass0 { prefilter_tested += 1 } else { fallback_tests += 1 }
            smooth_tests += 1
            poly_smooth_tests += 1
            mut rem_i = 0
            if has_root_filters {
               if pass0 {
                  rem_i = _qs_factor_over_base_filtered_int_raw_scan_pos(qv, trial_base_len, root_filters, pos, raw_counts)
               } else {
                  rem_i = _qs_factor_over_base_filtered_int_raw_scan_pos_limited(qv, trial_base_len, root_filters, pos, score + 1, raw_counts)
               }
            } else {
               rem_i = _qs_factor_over_base_intbase_scan(qv, trial_base, raw_counts)
            }
            if rem_i == 1 {
               smooth_hits += 1
               if !a_exps_ready {
                  a_exps = _siqs_a_exponents(poly, base)
                  a_exps_ready = true
               }
               def int x = Ai * t + Bi
               relations[relation_count] = _siqs_relation_intbase_for_mode(x, A, qv, trial_base, a_exps, include_relation_details)
               relation_count += 1
               found += 1
            }
         } elif pass0 {
            prefilter_skipped += 1
         }
      } elif pass0 {
         skipped_negative += 1
      }
      qv += q_delta
      q_delta += twoA
      t += 1
   }
   _dict_with(16, [
         ["relations", relations], ["relation_count", relation_count],
         ["found", found], ["skipped_negative", skipped_negative],
         ["poly_candidates", poly_candidates], ["poly_smooth_tests", poly_smooth_tests],
         ["candidate_count", candidate_count], ["smooth_tests", smooth_tests],
         ["smooth_hits", smooth_hits], ["prefilter_skipped", prefilter_skipped],
         ["prefilter_tested", prefilter_tested], ["fallback_tests", fallback_tests],
      ])
}

fn _siqs_collect_poly_pass_int_raw(
   ptr scores, ptr root_filters, list relations_in, dict state_in, dict poly, int pi, int pass, any A, any B, any C,
   dict score_report, list base, list<int> trial_base, int sieve_radius, int score_min, int max_relations, int multiplier,
   bool include_relation_details=true, bool include_poly_report=true,
) dict {
   mut relation_count = relations_in.len
   mut relations = _mpqs_relation_buffer(relations_in, max_relations)
   mut state_candidate_count = int(state_in.get("candidate_count", 0))
   mut state_smooth_tests = int(state_in.get("smooth_tests", 0))
   mut state_smooth_hits = int(state_in.get("smooth_hits", 0))
   mut state_trial_prime_tests = int(state_in.get("trial_prime_tests", 0))
   mut state_trial_divisions = int(state_in.get("trial_divisions", 0))
   mut state_nonzero_terms = int(state_in.get("nonzero_exponent_terms", 0))
   mut state_skipped_negative = int(state_in.get("total_skipped_negative", 0))
   mut state_prefilter_skipped = int(state_in.get("prefilter_skipped", 0))
   mut state_prefilter_tested = int(state_in.get("prefilter_tested", 0))
   mut state_fallback_tests = int(state_in.get("fallback_tests", 0))
   mut state_marked_roots = int(state_in.get("sieve_marked_roots", 0))
   mut state_noninvertible = int(state_in.get("sieve_noninvertible", 0))
   if pass == 0 {
      state_marked_roots += int(score_report.get("marked_roots", 0))
      state_noninvertible += int(score_report.get("skipped_noninvertible", 0))
   }
   def int Ai = bigint_to_int(A)
   def int Bi = bigint_to_int(B)
   def int Ci = bigint_to_int(C)
   mut int found = 0
   mut int skipped_negative = 0
   mut int poly_candidates = 0
   mut int poly_smooth_tests = 0
   mut int poly_trial_prime_tests = 0
   mut int poly_trial_divisions = 0
   def bool has_root_filters = root_filters ? true : false
   with ptr raw_counts = malloc(24){
      if !raw_counts { return _siqs_poly_scan_result(relations_in, state_in, poly, pi, A, B, found, poly_candidates, poly_smooth_tests, poly_trial_prime_tests, poly_trial_divisions, skipped_negative, score_report, include_poly_report && pass == 0) }
      memset(raw_counts, 0, 24)
      def scan = _siqs_collect_poly_pass_int_raw_loop(scores, root_filters, relations, relation_count, poly, A, Ai, Bi, Ci, base, trial_base, sieve_radius, score_min, max_relations, pass == 0, has_root_filters, include_relation_details, raw_counts)
      relations = scan.get("relations", relations)
      relation_count = int(scan.get("relation_count", relation_count))
      found = int(scan.get("found", 0))
      skipped_negative = int(scan.get("skipped_negative", 0))
      poly_candidates = int(scan.get("poly_candidates", 0))
      poly_smooth_tests = int(scan.get("poly_smooth_tests", 0))
      state_candidate_count += int(scan.get("candidate_count", 0))
      state_smooth_tests += int(scan.get("smooth_tests", 0))
      state_smooth_hits += int(scan.get("smooth_hits", 0))
      state_skipped_negative += skipped_negative
      state_prefilter_skipped += int(scan.get("prefilter_skipped", 0))
      state_prefilter_tested += int(scan.get("prefilter_tested", 0))
      state_fallback_tests += int(scan.get("fallback_tests", 0))
      poly_trial_prime_tests += load64_i(raw_counts, 0)
      poly_trial_divisions += load64_i(raw_counts, 8)
      state_trial_prime_tests += poly_trial_prime_tests
      state_trial_divisions += poly_trial_divisions
      state_nonzero_terms += load64_i(raw_counts, 16)
      def state = _set_fields(state_in, [
            ["candidate_count", state_candidate_count], ["smooth_tests", state_smooth_tests], ["smooth_hits", state_smooth_hits],
            ["trial_prime_tests", state_trial_prime_tests], ["trial_divisions", state_trial_divisions], ["nonzero_exponent_terms", state_nonzero_terms],
            ["total_skipped_negative", state_skipped_negative], ["prefilter_skipped", state_prefilter_skipped], ["prefilter_tested", state_prefilter_tested],
            ["fallback_tests", state_fallback_tests], ["sieve_marked_roots", state_marked_roots], ["sieve_noninvertible", state_noninvertible],
         ])
      _siqs_poly_scan_result(_mpqs_relation_buffer_trim(relations, relation_count), state, poly, pi, A, B, found, poly_candidates, poly_smooth_tests, poly_trial_prime_tests, poly_trial_divisions, skipped_negative, score_report, include_poly_report && pass == 0)
   }
}

fn _siqs_collect_poly_pass(
   list relations, dict state, dict poly, int pi, int pass, any modulus, list base,
   list<int> trial_base, list sqrt_roots, int sieve_radius, int score_min, int max_relations, int multiplier,
   bool include_relation_details=true, bool include_poly_report=true,
) dict {
   def A = _z(poly.get("A"))
   def B = _z(poly.get("B"))
   def C = _z(poly.get("C"))
   if _siqs_poly_int_scan_allowed(A, B, C, sieve_radius) {
      with ptr raw_scores = malloc(sieve_radius * 2 + 1){
         if raw_scores {
            with ptr raw_root_filters = malloc(base.len * 32){
               def raw_report = _qs_sieve_scores_siqs_raw_into(raw_scores, raw_root_filters, modulus, base, sqrt_roots, A, B, sieve_radius)
               return _siqs_collect_poly_pass_int_raw(raw_scores, raw_root_filters, relations, state, poly, pi, pass, A, B, C, raw_report, base, trial_base, sieve_radius, score_min, max_relations, multiplier, include_relation_details, include_poly_report)
            }
            def raw_report = _qs_sieve_scores_siqs_raw_into(raw_scores, nil, modulus, base, sqrt_roots, A, B, sieve_radius)
            return _siqs_collect_poly_pass_int_raw(raw_scores, nil, relations, state, poly, pi, pass, A, B, C, raw_report, base, trial_base, sieve_radius, score_min, max_relations, multiplier, include_relation_details, include_poly_report)
         }
      }
   }
   def score_report = _qs_sieve_scores_siqs_with_roots(modulus, base, sqrt_roots, A, B, sieve_radius)
   def scores = score_report.get("scores", [])
   mut state_candidate_count = int(state.get("candidate_count", 0))
   mut state_smooth_tests = int(state.get("smooth_tests", 0))
   mut state_smooth_hits = int(state.get("smooth_hits", 0))
   mut state_trial_prime_tests = int(state.get("trial_prime_tests", 0))
   mut state_trial_divisions = int(state.get("trial_divisions", 0))
   mut state_nonzero_terms = int(state.get("nonzero_exponent_terms", 0))
   mut state_skipped_negative = int(state.get("total_skipped_negative", 0))
   mut state_prefilter_skipped = int(state.get("prefilter_skipped", 0))
   mut state_prefilter_tested = int(state.get("prefilter_tested", 0))
   mut state_fallback_tests = int(state.get("fallback_tests", 0))
   mut state_marked_roots = int(state.get("sieve_marked_roots", 0))
   mut state_noninvertible = int(state.get("sieve_noninvertible", 0))
   if pass == 0 {
      state_marked_roots += int(score_report.get("marked_roots", 0))
      state_noninvertible += int(score_report.get("skipped_noninvertible", 0))
   }
   mut a_exps = []
   mut a_exps_ready = false
   mut found, skipped_negative, poly_candidates, poly_smooth_tests = 0, 0, 0, 0
   mut poly_trial_prime_tests, poly_trial_divisions = 0, 0
   mut t = 0 - sieve_radius
   def twoA = Z(2) * A
   def twoB = Z(2) * B
   mut tz = Z(t)
   mut qv = A * tz * tz + twoB * tz + C
   mut q_delta = twoA * tz + A + twoB
   while t <= sieve_radius && relations.len < max_relations {
      def pos = t + sieve_radius
      if pass == 0 {
         state_candidate_count += 1
         poly_candidates += 1
      }
      if qv > Z(0) {
         def score = int(scores[pos])
         def should_test = (pass == 0 && score >= score_min) || (pass == 1 && score < score_min)
         if should_test {
            if pass == 0 { state_prefilter_tested += 1 } else { state_fallback_tests += 1 }
            def smooth = _qs_factor_over_base_profile(qv, base)
            state_smooth_tests += 1
            state_trial_prime_tests += int(smooth[3])
            state_trial_divisions += int(smooth[4])
            state_nonzero_terms += int(smooth[5])
            poly_smooth_tests += 1
            poly_trial_prime_tests += int(smooth[3])
            poly_trial_divisions += int(smooth[4])
            if smooth[0] {
               state_smooth_hits += 1
               if !a_exps_ready {
                  a_exps = _siqs_a_exponents(poly, base)
                  a_exps_ready = true
               }
               def relation_exps = _siqs_apply_a_exponents(smooth[1], a_exps)
               def x = A * Z(t) + B
               if include_relation_details {
                  def relation_value = A * qv
                  relations = relations.append(_set_fields(_qs_relation(x, relation_value, relation_exps), _siqs_relation_fields(poly, pi, t, A, B, C, qv, relation_value, multiplier, score)))
               } else {
                  relations = relations.append(_qs_relation_compact(x, relation_exps))
               }
               found += 1
            }
         } elif pass == 0 {
            state_prefilter_skipped += 1
         }
      } elif pass == 0 {
         skipped_negative += 1
         state_skipped_negative += 1
      }
      qv += q_delta
      q_delta += twoA
      t += 1
   }
   state = _set_fields(state, [
         ["candidate_count", state_candidate_count], ["smooth_tests", state_smooth_tests], ["smooth_hits", state_smooth_hits],
         ["trial_prime_tests", state_trial_prime_tests], ["trial_divisions", state_trial_divisions], ["nonzero_exponent_terms", state_nonzero_terms],
         ["total_skipped_negative", state_skipped_negative], ["prefilter_skipped", state_prefilter_skipped], ["prefilter_tested", state_prefilter_tested],
         ["fallback_tests", state_fallback_tests], ["sieve_marked_roots", state_marked_roots], ["sieve_noninvertible", state_noninvertible],
      ])
   _siqs_poly_scan_result(relations, state, poly, pi, A, B, found, poly_candidates, poly_smooth_tests, poly_trial_prime_tests, poly_trial_divisions, skipped_negative, score_report, include_poly_report && pass == 0)
}

fn _siqs_collect_relations(any modulus, list base, list polys, int sieve_radius, int score_min, int max_relations, int multiplier, bool include_relation_details=true, bool include_poly_reports=true) dict {
   mut relations, poly_reports = [], []
   mut state = _siqs_collect_state()
   def trial_base = _qs_factor_base_ints(base)
   def sqrt_roots = _mpqs_sqrt_roots(modulus, base)
   mut pass = 0
   while pass < 2 && relations.len < max_relations {
      mut pi = 0
      while pi < polys.len && relations.len < max_relations {
         def scan = _siqs_collect_poly_pass(relations, state, polys.get(pi), pi, pass, modulus, base, trial_base, sqrt_roots, sieve_radius, score_min, max_relations, multiplier, include_relation_details, include_poly_reports)
         relations = scan.get("relations")
         state = scan.get("state")
         if include_poly_reports && pass == 0 { poly_reports = poly_reports.append(scan.get("poly_report")) }
         pi += 1
      }
      pass += 1
   }
   state.set("relations", relations).set("poly_reports", poly_reports)
}

fn _siqs_relation_report(any n, int factor_base_bound=64, int polynomial_count=8, int sieve_radius=256, int max_relations=32, bool tune_cutoff=true, bool detailed=true) dict {
   "Collect smooth SIQS relations from generated polynomials Q(t)=(A*t+B)^2-kN."
   def t0 = ticks()
   def nz = _z(n)
   mut out = _set_fields(_report("siqs-relation-collector", 24), [
         ["n", nz], ["factor_base_bound", factor_base_bound],
         ["polynomial_count", polynomial_count], ["sieve_radius", sieve_radius],
         ["max_relations", max_relations],
         ["cutoff_tuning_enabled", tune_cutoff],
         ["relation_detail", detailed ? "full" : "factor-compact"],
      ])
   if nz <= Z(1) {
      return _set_fields(out, [["relations", []], ["relation_count", 0], ["success", false]])
   }
   if nz % Z(2) == Z(0) {
      return _set_fields(out, [["factor", Z(2)], ["success", true], ["relations", []], ["relation_count", 0]])
   }
   def polys = _siqs_polynomial_report(nz, factor_base_bound, polynomial_count, sieve_radius, detailed)
   def modulus = polys.get("sieve_modulus", nz)
   def multiplier = int(polys.get("multiplier", 1))
   def base_report = _qs_factor_base_report(nz, modulus, factor_base_bound)
   if base_report.get("factor", nil) != nil {
      return _finish_report_with(out, t0, [
            ["factor", base_report.get("factor")], ["success", true],
            ["polynomial_report", polys], ["factor_base_report", base_report],
            ["relations", []], ["relation_count", 0],
         ])
   }
   def base = base_report.get("base", [])
   def profile_base = _qs_factor_base_bigints(base)
   def default_score_min = _qs_sieve_score_min(base)
   def plist = polys.get("polynomials", [])
   def cutoff_report = tune_cutoff ? _siqs_cutoff_tune_from_polys(modulus, base, plist, default_score_min, sieve_radius, false) : _siqs_cutoff_disabled(default_score_min)
   def rel_t0 = ticks()
   def score_min = int(cutoff_report.get("selected_cutoff", default_score_min))
   def collected = _siqs_collect_relations(modulus, profile_base, plist, sieve_radius, score_min, max_relations, multiplier, detailed, detailed)
   def relations = collected.get("relations", [])
   def poly_reports = collected.get("poly_reports", [])
   mut fields = _fields_extend([
         ["multiplier", multiplier], ["sieve_modulus", modulus],
         ["factor_base", base], ["factor_base_size", base.len],
         ["factor_base_report", base_report], ["polynomial_report", polys],
         ["poly_reports", poly_reports], ["relations", relations],
         ["relation_count", relations.len],
      ], _siqs_relation_final_fields(
         rel_t0, int(collected.get("candidate_count", 0)), int(collected.get("smooth_tests", 0)), int(collected.get("smooth_hits", 0)),
         int(collected.get("trial_prime_tests", 0)), int(collected.get("trial_divisions", 0)), int(collected.get("nonzero_exponent_terms", 0)),
         int(collected.get("total_skipped_negative", 0)), score_min, default_score_min, tune_cutoff, cutoff_report,
         int(collected.get("sieve_marked_roots", 0)), int(collected.get("sieve_noninvertible", 0)),
         int(collected.get("prefilter_tested", 0)), int(collected.get("prefilter_skipped", 0)), int(collected.get("fallback_tests", 0)), relations,
      ))
   _finish_report_with(out, t0, fields)
}

fn siqs_relation_report(any n, int factor_base_bound=64, int polynomial_count=8, int sieve_radius=256, int max_relations=32, bool tune_cutoff=true) dict {
   "Runs the siqs relation report operation."
   _siqs_relation_report(n, factor_base_bound, polynomial_count, sieve_radius, max_relations, tune_cutoff, true)
}

fn _siqs_factor_relation_fields(dict rel_report, list base, list raw_relations, list selected_relations) list {
   [
      ["multiplier", rel_report.get("multiplier", 1)],
      ["sieve_modulus", rel_report.get("sieve_modulus", rel_report.get("n", 0))],
      ["factor_base", base], ["factor_base_size", base.len],
      ["factor_base_report", rel_report.get("factor_base_report", dict())],
      ["relation_report", rel_report], ["raw_relation_count", raw_relations.len],
      ["relations", selected_relations], ["relation_count", selected_relations.len],
      ["relation_collection_elapsed_ms", rel_report.get("relation_collection_elapsed_ms", rel_report.get("elapsed_ms", -1.0))],
   ]
}

fn _siqs_factor_sieve_fields(dict rel_report) list {
   [
      ["sieve_prefilter_enabled", rel_report.get("sieve_prefilter_enabled", false)],
      ["sieve_score_min", rel_report.get("sieve_score_min", 0)],
      ["default_sieve_score_min", rel_report.get("default_sieve_score_min", 0)],
      ["cutoff_tuning_enabled", rel_report.get("cutoff_tuning_enabled", false)],
      ["cutoff_tune_report", rel_report.get("cutoff_tune_report", nil)],
      ["cutoff_tune_changed", rel_report.get("cutoff_tune_changed", false)],
      ["sieve_marked_roots", rel_report.get("sieve_marked_roots", 0)],
      ["prefilter_tested", rel_report.get("prefilter_tested", 0)],
      ["prefilter_skipped", rel_report.get("prefilter_skipped", 0)],
      ["fallback_tests", rel_report.get("fallback_tests", 0)],
   ]
}

fn _siqs_factor_solve_fields(dict solved, dict solve, list selected_relations) list {
   [
      ["selected_relation_count", selected_relations.len],
      ["linear_algebra_relation_count", solve.get("linear_algebra_relation_count", solve.get("relation_count", selected_relations.len))],
      ["relation_filter", solved.get("filter_report", dict())],
      ["used_relation_filter", solved.get("used_filter", false)],
      ["dependency_solve", solve], ["raw_solve", solved.get("raw_solve", nil)],
      ["linear_algebra", solve.get("linear_algebra", dict())],
      ["dependency_count", solve.get("dependency_count", 0)],
      ["dependencies_tried", solve.get("dependencies_tried", 0)],
      ["factor", solve.get("factor", nil)], ["success", solve.get("success", false)],
   ]
}

fn _qs_dependency_matrix(list relations, int width) list {
   mut parities = list(relations.len)
   mut i = 0
   while i < relations.len {
      parities[i] = _qs_relation_parity(relations[i])
      i += 1
   }
   mut rows = list(width)
   mut j = 0
   while j < width {
      mut row = list(relations.len)
      i = 0
      while i < relations.len {
         def parity = parities[i]
         row[i] = j < parity.len ? (int(parity[j]) & 1) : 0
         i += 1
      }
      rows[j] = row
      j += 1
   }
   rows
}

fn _qs_dependency_sparse_rows(list relations, int width) list {
   mut rows = list(width)
   __list_set_len(rows, width)
   mut j = 0
   while j < width {
      rows[j] = []
      j += 1
   }
   mut i = 0
   while i < relations.len {
      def parity = _qs_relation_parity(relations[i])
      j = 0
      def limit = min(width, parity.len)
      while j < limit {
         if (int(parity[j]) & 1) != 0 {
            rows[j] = rows[j].append(i)
         }
         j += 1
      }
      i += 1
   }
   rows
}

fn _qs_try_dependency_candidates_mod_report(any original_n, any modulus, list base, list relations, list basis, int width, int max_count=8192) dict {
   def t0 = ticks()
   mut w = width
   if w <= 0 && basis.len > 0 { w = _gf2_dense_width([basis.get(0)], 0) }
   mut cap = max_count
   if cap <= 0 { cap = 8192 }
   def large_passthrough = basis.len > 14
   def expected = large_passthrough ? basis.len : ((1 << basis.len) - 1)
   def candidate_count = min(cap, expected)
   mut factor = nil
   mut deps = 0
   if large_passthrough {
      mut i = 0
      while i < basis.len && deps < cap && factor == nil {
         deps += 1
         factor = _qs_try_verified_dependency_mod(original_n, modulus, base, relations, basis[i])
         i += 1
      }
   } else {
      def limit = 1 << basis.len
      mut mask = 1
      while mask < limit && deps < cap && factor == nil {
         mut dep = _gf2_zero_vec(w)
         mut i = 0
         while i < basis.len {
            if ((mask >> i) & 1) == 1 { _gf2_xor_vec_into(dep, basis[i], w) }
            i += 1
         }
         deps += 1
         factor = _qs_try_verified_dependency_mod(original_n, modulus, base, relations, dep)
         mask += 1
      }
   }
   _report_with("gf2-dependency-candidates", t0, [
         ["basis_count", basis.len], ["width", w], ["max_count", cap],
         ["expected_combination_count", expected],
         ["large_basis_passthrough", large_passthrough],
         ["candidate_count", candidate_count], ["truncated", candidate_count < expected],
         ["candidates", []], ["candidate_weights", []],
         ["materialized", false], ["tried_count", deps],
         ["factor", factor], ["success", factor != nil],
      ])
}

fn _qs_dependency_solve_report(any original_n, any modulus, list base, list relations, int max_candidates=8192, str backend="packed") dict {
   def t0 = ticks()
   def use_precondition = backend == "lanczos" || backend == "block-lanczos"
   def sparse_initial = relations.len >= 128 && (backend == "lanczos" || backend == "block-lanczos" || backend == "wiedemann" || backend == "block-wiedemann")
   def sparse_rows = sparse_initial ? _qs_dependency_sparse_rows(relations, base.len) : []
   mut dep_matrix = []
   mut pipeline = dict()
   if sparse_initial {
      pipeline = _gf2_dependency_sparse_pipeline_report(sparse_rows, relations.len, backend, 8, 0, max_candidates)
   } else {
      pipeline = gf2_dependency_pipeline_report(_qs_dependency_matrix(relations, base.len), relations.len, backend, use_precondition, 8, 0, false, max_candidates)
   }
   mut initial_lin = nil
   mut initial_pipeline = nil
   mut candidate_report = _qs_try_dependency_candidates_mod_report(original_n, modulus, base, relations, pipeline.get("basis", []), relations.len, max_candidates)
   mut initial_candidate_report = nil
   mut factor = candidate_report.get("factor", nil)
   mut deps = int(candidate_report.get("tried_count", 0))
   mut immediate_deps_tried = 0
   if factor == nil && (backend == "lanczos" || backend == "block-lanczos") {
      initial_pipeline = pipeline
      initial_lin = pipeline.get("linear_algebra", dict())
      initial_candidate_report = candidate_report
      dep_matrix = _qs_dependency_matrix(relations, base.len)
      pipeline = gf2_dependency_pipeline_report(dep_matrix, relations.len, backend, use_precondition, 8, 0, true, max_candidates)
      candidate_report = _qs_try_dependency_candidates_mod_report(original_n, modulus, base, relations, pipeline.get("basis", []), relations.len, max_candidates)
      deps += int(candidate_report.get("tried_count", 0))
      factor = candidate_report.get("factor", nil)
   }
   _report_with("qs-dependency-solve", t0, [
         ["backend", backend], ["input_relation_count", relations.len],
         ["relation_count", pipeline.get("solve_cols", relations.len)],
         ["linear_algebra_relation_count", pipeline.get("solve_cols", relations.len)],
         ["factor_base_size", base.len], ["dependency_pipeline", pipeline],
         ["initial_dependency_pipeline", initial_pipeline],
         ["matrix_precondition", pipeline.get("matrix_precondition", nil)],
         ["linear_algebra", pipeline.get("linear_algebra", dict())],
         ["initial_linear_algebra", initial_lin],
         ["dependency_candidate_report", candidate_report],
         ["initial_dependency_candidate_report", initial_candidate_report],
         ["used_exact_closure", initial_lin != nil],
         ["dependency_count", int(candidate_report.get("candidate_count", 0))], ["dependencies_tried", deps],
         ["immediate_dependencies_tried", immediate_deps_tried],
         ["factor", factor], ["success", factor != nil],
      ])
}

fn quadratic_sieve_factor_report(any n, int factor_base_bound=64, int scan=20000, int max_relations=18) dict {
   "Small quadratic-sieve report using smooth relations and GF(2) dependency solving."
   def t0 = ticks()
   def nz = _z(n)
   mut out = _set_fields(_report("quadratic-sieve-small", 16), [
         ["n", nz], ["factor_base_bound", factor_base_bound],
         ["scan", scan], ["max_relations", max_relations],
      ])
   if nz <= Z(1) { return _set_factor_status(out, nil, false) }
   if nz % Z(2) == Z(0) { return _set_factor_status(out, Z(2), true) }
   def base0 = _qs_prime_base(factor_base_bound)
   mut base = []
   mut base_i = 0
   while base_i < base0.len {
      def p, g = _z(base0.get(base_i)), gcd(_z(base0.get(base_i)), nz)
      if _is_nontrivial_factor(g, nz) {
         return _finish_factor_status(out, t0, g, true)
      }
      if g == Z(1) { base = base.append(p) }
      base_i += 1
   }
   def base_report = _dict_with(8, [
         ["base", base], ["prime_count", base.len], ["mode", "full-residue-scan"],
      ])
   mut relations = []
   mut x = isqrt(nz)
   if x * x < nz { x = x + Z(1) }
   def trial_base = _qs_factor_base_ints(base)
   mut scanned = 0
   def rel_t0 = ticks()
   mut smooth_tests = 0
   mut smooth_hits = 0
   mut trial_prime_tests = 0
   mut trial_divisions = 0
   mut nonzero_exponent_terms = 0
   def small_scan = bit_length(nz) <= 60 && bit_length(x + Z(scan) + Z(1)) <= 31
   if small_scan {
      def int n_i = bigint_to_int(nz)
      mut int x_i = bigint_to_int(x)
      with ptr raw_counts = malloc(24){
         if !raw_counts { return _finish_report_with(out, t0, [["factor", nil], ["success", false], ["relations", relations], ["reason", "qs scan counter allocation failed"]]) }
         memset(raw_counts, 0, 24)
         while scanned < scan && relations.len < max_relations {
            mut int residue_i = (x_i * x_i) % n_i
            if residue_i < 0 { residue_i += n_i }
            if residue_i == 0 {
               def gx = gcd(Z(x_i), nz)
               if _is_nontrivial_factor(gx, nz) {
                  return _finish_report_with(out, t0, [
                        ["factor", gx], ["success", true], ["relations", relations],
                     ])
               }
            } else {
               def rem_i = _qs_factor_over_base_intbase_scan(residue_i, trial_base, raw_counts)
               smooth_tests += 1
               if rem_i == 1 {
                  def smooth = _qs_factor_over_base_profile_intbase(residue_i, trial_base)
                  smooth_hits += 1
                  relations = relations.append(_qs_relation(Z(x_i), Z(residue_i), smooth[1]))
               }
            }
            x_i += 1
            scanned += 1
         }
         trial_prime_tests += load64_i(raw_counts, 0)
         trial_divisions += load64_i(raw_counts, 8)
         nonzero_exponent_terms += load64_i(raw_counts, 16)
      }
   } else {
      while scanned < scan && relations.len < max_relations {
         def residue = mod(x * x, nz)
         if residue == Z(0) {
            def gx = gcd(x, nz)
            if _is_nontrivial_factor(gx, nz) {
               return _finish_report_with(out, t0, [
                     ["factor", gx], ["success", true], ["relations", relations],
                  ])
            }
         } else {
            def smooth = bit_length(residue) <= 60 ? _qs_factor_over_base_profile_intbase(bigint_to_int(residue), trial_base) : _qs_factor_over_base_profile(residue, base)
            smooth_tests += 1
            trial_prime_tests += int(smooth[3])
            trial_divisions += int(smooth[4])
            nonzero_exponent_terms += int(smooth[5])
            if smooth[0] {
               smooth_hits += 1
               relations = relations.append(_qs_relation(x, residue, smooth[1]))
            }
         }
         x = x + Z(1)
         scanned += 1
      }
   }
   def lin = sparse_gf2_nullspace_report(_qs_dependency_sparse_rows(relations, base.len), relations.len)
   def candidate_report = _qs_try_dependency_candidates_mod_report(nz, nz, base, relations, lin.get("basis", []), relations.len, 4096)
   def factor = candidate_report.get("factor", nil)
   def deps = int(candidate_report.get("tried_count", 0))
   mut fields = _fields_extend([
         ["factor_base", base], ["factor_base_size", base.len],
         ["factor_base_report", base_report], ["relations", relations],
         ["relation_count", relations.len],
      ], _qs_relation_collection_fields(rel_t0, scanned, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_exponent_terms))
   _finish_report_with(out, t0, _fields_extend(fields, [
            ["linear_algebra", lin], ["dependency_candidate_report", candidate_report],
            ["dependency_count", int(candidate_report.get("candidate_count", 0))], ["dependencies_tried", deps],
            ["factor", factor], ["success", factor != nil],
         ]))
}

fn quadratic_sieve_factor(any n, int factor_base_bound=64, int scan=20000, int max_relations=18) any {
   "Return one non-trivial factor found by quadratic_sieve_factor_report, or nil."
   quadratic_sieve_factor_report(n, factor_base_bound, scan, max_relations).get("factor", nil)
}

fn siqs_factor_report(any n, int factor_base_bound=64, int polynomial_count=8, int sieve_radius=256, int max_relations=32, bool tune_cutoff=true) dict {
   "Self-initializing quadratic-sieve report using SIQS polynomial relation collection."
   def t0 = ticks()
   def nz = _z(n)
   mut out = _set_fields(_report("self-initializing-quadratic-sieve", 26), [
         ["n", nz], ["factor_base_bound", factor_base_bound],
         ["polynomial_count", polynomial_count], ["sieve_radius", sieve_radius],
         ["max_relations", max_relations],
      ])
   if nz <= Z(1) { return _set_factor_status(out, nil, false) }
   if nz % Z(2) == Z(0) { return _set_factor_status(out, Z(2), true) }
   def rel_report = _siqs_relation_report(nz, factor_base_bound, polynomial_count, sieve_radius, max_relations, tune_cutoff, false)
   if rel_report.get("factor", nil) != nil {
      return _finish_report_with(out, t0, [
            ["factor", rel_report.get("factor")], ["success", true],
            ["relation_report", rel_report],
         ])
   }
   def base = rel_report.get("factor_base", [])
   def raw_relations = rel_report.get("relations", [])
   def filter_report = qs_relation_filter_report(raw_relations, base.len, true)
   def solved = _qs_dependency_solve_filtered_report(nz, rel_report.get("sieve_modulus", nz), base, raw_relations, filter_report, 8192)
   def solve = solved.get("solve")
   def selected_relations = solved.get("relations", raw_relations)
   mut fields = _fields_extend(_siqs_factor_relation_fields(rel_report, base, raw_relations, selected_relations), _qs_smooth_metric_fields_from(rel_report, raw_relations.len))
   fields = _fields_extend(fields, _siqs_factor_sieve_fields(rel_report))
   _finish_report_with(out, t0, _fields_extend(fields, _siqs_factor_solve_fields(solved, solve, selected_relations)))
}

fn siqs_factor(any n, int factor_base_bound=64, int polynomial_count=8, int sieve_radius=256, int max_relations=32) any {
   "Return one factor from siqs_factor_report, or nil."
   siqs_factor_report(n, factor_base_bound, polynomial_count, sieve_radius, max_relations).get("factor", nil)
}

fn _qs_dependency_solve_filtered_report(any n, any modulus, list base, list relations, dict filter_report, int max_dependencies=8192) dict {
   def filtered_relations = filter_report.get("filtered_relations", [])
   def use_filtered = filtered_relations.len >= 2 && filtered_relations.len < relations.len
   mut solve = _qs_dependency_solve_report(n, modulus, base, use_filtered ? filtered_relations : relations, max_dependencies, "lanczos")
   mut raw_solve = nil
   if !solve.get("success", false) && use_filtered {
      raw_solve = _qs_dependency_solve_report(n, modulus, base, relations, max_dependencies, "lanczos")
      if raw_solve.get("success", false) { solve = raw_solve }
   }
   def solved_with_filter = use_filtered && raw_solve == nil
   _dict_with(8, [
         ["solve", solve],
         ["raw_solve", raw_solve],
         ["filter_report", filter_report],
         ["used_filter", solved_with_filter],
         ["relations", solved_with_filter ? filtered_relations : relations],
         ["relation_set", solved_with_filter ? "filtered" : "raw"],
      ])
}

fn _qs_square_relation_factor(any x, any residue, any nz) any {
   if residue != 0 { return nil }
   def gx = gcd(x, nz)
   _is_nontrivial_factor(gx, nz) ? gx : nil
}

fn _mpqs_window_report(dict byte_summary, int w, any center, int window_radius, bool adiv_enabled, list adiv_polys, int found, int window_candidates, int window_smooth_tests, int window_trial_prime_tests, int window_trial_divisions, dict byte_report) dict {
   _set_fields(byte_summary, [
         ["window", w], ["center", center], ["radius", window_radius],
         ["poly_index", adiv_enabled ? adiv_polys.get(w % adiv_polys.len).get("index", -1) : -1],
         ["relations", found], ["candidates", window_candidates],
         ["smooth_tests", window_smooth_tests], ["smooth_hits", found],
         ["trial_division_prime_tests", window_trial_prime_tests], ["trial_divisions", window_trial_divisions],
         ["sieve_marked_roots", byte_report.get("marked_roots", 0)],
      ])
}

fn _mpqs_early_dependency(any solve=nil, bool used=false, int pass=-1, int window=-1, int relation_count=0) dict {
   _dict_with(5, [
         ["solve", solve], ["used", used],
         ["pass", pass], ["window", window],
         ["relation_count", relation_count],
      ])
}

fn _mpqs_window_solve_fields(list solved_relations, dict early_dependency, bool early_dependency_used, dict solve, dict filter_report, bool solved_with_filter, str solve_relation_set, any raw_solve, list window_reports) list {
   [
      ["selected_relation_count", solved_relations.len],
      ["early_dependency_solve_used", early_dependency_used],
      ["early_dependency_pass", early_dependency.get("pass", -1)],
      ["early_dependency_window", early_dependency.get("window", -1)],
      ["early_dependency_relation_count", early_dependency.get("relation_count", 0)],
      ["solved_relation_count", solve.get("relation_count", solved_relations.len)],
      ["linear_algebra_relation_count", solve.get("linear_algebra_relation_count", solve.get("relation_count", solved_relations.len))],
      ["relation_filter", filter_report], ["used_relation_filter", solved_with_filter],
      ["solve_relation_set", solve_relation_set], ["dependency_solve", solve], ["raw_solve", raw_solve],
      ["window_reports", window_reports], ["linear_algebra", solve.get("linear_algebra", dict())],
      ["dependency_count", solve.get("dependency_count", 0)], ["dependencies_tried", solve.get("dependencies_tried", 0)],
      ["factor", solve.get("factor", nil)], ["success", solve.get("success", false)],
   ]
}

fn _mpqs_collect_result(
   any factor, list relations, int found, int smooth_tests, int smooth_hits,
   int trial_prime_tests, int trial_divisions, int nonzero_terms,
   int prefilter_tested, int fallback_tests, int a_divisor_relations
) dict {
   _dict_with(14, [
         ["factor", factor], ["relations", relations], ["found", found],
         ["smooth_tests", smooth_tests], ["smooth_hits", smooth_hits],
         ["trial_prime_tests", trial_prime_tests], ["trial_divisions", trial_divisions],
         ["nonzero_exponent_terms", nonzero_terms],
         ["prefilter_tested", prefilter_tested], ["fallback_tests", fallback_tests],
         ["a_divisor_relation_count", a_divisor_relations],
      ])
}

fn _mpqs_relation_buffer(list relations, int max_relations) list {
   def initial = relations.len
   def cap = max(initial, max_relations)
   mut out = list(cap)
   __list_set_len(out, cap)
   mut i = 0
   while i < initial {
      out[i] = relations[i]
      i += 1
   }
   out
}

fn _mpqs_relation_buffer_trim(list relations, int relation_count) list {
   __list_set_len(relations, relation_count)
   relations
}

fn _mpqs_byte_window_choice(
   ptr scratch, any modulus, list base, list base_sqrt_roots, list<int> base_ints, list<int> base_sqrt_roots_int,
   list<int> center_mods, dict base_buckets, list<int> adiv_sieve_base_ints, dict adiv_buckets, list adiv_polys, bool adiv_enabled,
   int w, any center, int window_radius, int score_min, any start
) list {
   def poly = adiv_enabled ? adiv_polys.get(w % adiv_polys.len) : nil
   if poly != nil {
      return [poly, _mpqs_poly_byte_sieve_window_into(scratch, adiv_sieve_base_ints, base_ints, base_sqrt_roots_int, adiv_buckets, poly.get("A"), poly.get("B"), window_radius, max(1, score_min - 2)), 1]
   }
   [nil, _mpqs_byte_sieve_window_int_roots_into(scratch, base_ints, base_sqrt_roots_int, center_mods, base_buckets, center, window_radius, score_min, start), 0]
}

fn _mpqs_collect_byte_window_relations_int(any nz, int modulus_i, list factor_base, list<int> trial_base, list<int> survivors, int survivor_count, int radius, int fallback_score_min, bool has_poly, int poly_A_i, int poly_B_i, int center_i, list<int> root_filters, int max_relations, list relations_in, bool large_relation_target) dict {
   mut relation_count = relations_in.len
   mut relations = _mpqs_relation_buffer(relations_in, max_relations)
   mut found, smooth_tests, smooth_hits = 0, 0, 0
   mut trial_prime_tests, trial_divisions, nonzero_terms = 0, 0, 0
   mut prefilter_tested, a_divisor_relations = 0, 0
   def trial_base_len = trial_base.len
   def root_filter_bytes = max(32, factor_base.len * 16)
   def exp_scratch_bytes = max(8, trial_base_len * 16)
   with ptr raw_root_filters = malloc(root_filter_bytes + 32 + exp_scratch_bytes){
      if raw_root_filters {
         _mpqs_root_filter_list_to_raw_shifted32(raw_root_filters, root_filters, factor_base.len, radius)
         def raw_counts = ptr_add(raw_root_filters, root_filter_bytes)
         def raw_exp_idxs = ptr_add(raw_counts, 32)
         def raw_exp_vals = ptr_add(raw_exp_idxs, trial_base_len * 8)
         memset(raw_counts, 0, 32)
         if has_poly {
            mut ri = 0
            while ri < survivor_count && relation_count < max_relations {
               def survivor = int(survivors[ri])
               def pos = survivor >> 8
               def off = pos - radius
               def score = survivor & 255
               def x_i = poly_A_i * off + poly_B_i
               mut residue_i = (x_i * x_i) % modulus_i
               if residue_i < 0 { residue_i += modulus_i }
               if residue_i == 0 {
                  def gx = _qs_square_relation_factor(Z(x_i), Z(0), nz)
                  if gx != nil { return _mpqs_collect_result(gx, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations) }
               } else {
                  prefilter_tested += 1
                  smooth_tests += 1
                  mut rem_i = _qs_factor_over_base_filtered_int_raw_scan_pos_collect32(residue_i, trial_base_len, raw_root_filters, pos, raw_counts, raw_exp_idxs, raw_exp_vals)
                  mut exps_i = nil
                  if rem_i != 1 && a_divisor_relations == 0 && factor_base.len >= 50 && (!large_relation_target || score >= fallback_score_min) {
                     def smooth = _qs_factor_over_base_profile_intbase(residue_i, trial_base)
                     trial_prime_tests += int(smooth[3])
                     trial_divisions += int(smooth[4])
                     nonzero_terms += int(smooth[5])
                     if smooth[0] {
                        rem_i = 1
                        exps_i = smooth[1]
                     }
                  }
                  if rem_i == 1 {
                     smooth_hits += 1
                     if exps_i == nil {
                        relations[relation_count] = _qs_relation_int_sparse_mpqs(x_i, trial_base_len, raw_exp_idxs, raw_exp_vals, load64_i(raw_counts, 24))
                     } else {
                        relations[relation_count] = _qs_relation_compact(x_i, exps_i)
                     }
                     relation_count += 1
                     found += 1
                     a_divisor_relations += 1
                  }
               }
               ri += 1
            }
            trial_prime_tests += load64_i(raw_counts, 0)
            trial_divisions += load64_i(raw_counts, 8)
            nonzero_terms += load64_i(raw_counts, 16)
            return _mpqs_collect_result(nil, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations)
         }
         mut ri = 0
         while ri < survivor_count && relation_count < max_relations {
            def survivor = int(survivors[ri])
            def pos = survivor >> 8
            def off = pos - radius
            def score = survivor & 255
            def x_i = center_i + off
            mut residue_i = (x_i * x_i) % modulus_i
            if residue_i < 0 { residue_i += modulus_i }
            if residue_i == 0 {
               def gx = _qs_square_relation_factor(Z(x_i), Z(0), nz)
               if gx != nil { return _mpqs_collect_result(gx, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations) }
            } else {
               prefilter_tested += 1
               smooth_tests += 1
               def rem_i = _qs_factor_over_base_filtered_int_raw_scan_pos_limited_collect32(residue_i, trial_base_len, raw_root_filters, pos, score + 1, raw_counts, raw_exp_idxs, raw_exp_vals)
               if rem_i == 1 {
                  smooth_hits += 1
                  relations[relation_count] = _qs_relation_int_sparse_mpqs(x_i, trial_base_len, raw_exp_idxs, raw_exp_vals, load64_i(raw_counts, 24))
                  relation_count += 1
                  found += 1
               }
            }
            ri += 1
         }
         trial_prime_tests += load64_i(raw_counts, 0)
         trial_divisions += load64_i(raw_counts, 8)
         nonzero_terms += load64_i(raw_counts, 16)
         return _mpqs_collect_result(nil, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations)
      }
   }
   mut fi = 0
   while fi < survivor_count && relation_count < max_relations {
      def survivor = int(survivors[fi])
      def off = (survivor >> 8) - radius
      def score = survivor & 255
      def x_i = has_poly ? (poly_A_i * off + poly_B_i) : center_i + off
      mut residue_i = (x_i * x_i) % modulus_i
      if residue_i < 0 { residue_i += modulus_i }
      if residue_i == 0 {
         def gx = _qs_square_relation_factor(Z(x_i), Z(0), nz)
         if gx != nil { return _mpqs_collect_result(gx, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations) }
      } else {
         prefilter_tested += 1
         smooth_tests += 1
         mut smooth = _qs_factor_over_base_profile_filtered_int(residue_i, factor_base, root_filters, off)
         trial_prime_tests += int(smooth[3])
         trial_divisions += int(smooth[4])
         nonzero_terms += int(smooth[5])
         if !smooth[0] && has_poly && a_divisor_relations == 0 && factor_base.len >= 50 && (!large_relation_target || score >= fallback_score_min) {
            smooth = _qs_factor_over_base_profile_intbase(residue_i, trial_base)
            trial_prime_tests += int(smooth[3])
            trial_divisions += int(smooth[4])
            nonzero_terms += int(smooth[5])
         }
         if smooth[0] {
            smooth_hits += 1
            relations[relation_count] = _qs_relation_compact(x_i, smooth[1])
            relation_count += 1
            found += 1
            if has_poly { a_divisor_relations += 1 }
         }
      }
      fi += 1
   }
   _mpqs_collect_result(nil, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations)
}

fn _mpqs_collect_byte_window_relations_big(any nz, any modulus, list factor_base, list<int> trial_base, list<int> survivors, int survivor_count, int radius, int fallback_score_min, bool has_poly, any raw_poly_A, any raw_poly_B, any center, list<int> root_filters, int max_relations, list relations_in, bool large_relation_target, int modulus_bits) dict {
   mut relation_count = relations_in.len
   mut relations = _mpqs_relation_buffer(relations_in, max_relations)
   mut found, smooth_tests, smooth_hits = 0, 0, 0
   mut trial_prime_tests, trial_divisions, nonzero_terms = 0, 0, 0
   mut prefilter_tested, a_divisor_relations = 0, 0
   def trial_base_len = trial_base.len
   def poly_A = has_poly ? _z(raw_poly_A) : Z(0)
   def poly_B = has_poly ? _z(raw_poly_B) : Z(0)
   def raw_root_filter_bytes2 = max(32, factor_base.len * 32)
   with ptr raw_root_filters2 = malloc(raw_root_filter_bytes2 + 24){
      if raw_root_filters2 {
         _mpqs_root_filter_list_to_raw_shifted(raw_root_filters2, root_filters, factor_base.len, radius)
         def raw_counts2 = ptr_add(raw_root_filters2, raw_root_filter_bytes2)
         memset(raw_counts2, 0, 24)
         mut si = 0
         while si < survivor_count && relation_count < max_relations {
            def survivor = int(survivors[si])
            def pos = survivor >> 8
            def off = pos - radius
            def score = survivor & 255
            def x = has_poly ? (poly_A * off + poly_B) : center + off
            def residue = mod(x * x, modulus)
            if residue == 0 {
               def gx = _qs_square_relation_factor(x, residue, nz)
               if gx != nil { return _mpqs_collect_result(gx, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations) }
            } else {
               prefilter_tested += 1
               smooth_tests += 1
               def x_abs = x < 0 ? -x : x
               if bit_length(residue) <= 62 && bit_length(x_abs) <= 62 {
                  def residue_i = bigint_to_int(residue)
                  def x_i = bigint_to_int(x)
                  mut rem_i = _qs_factor_over_base_filtered_int_raw_scan_pos(residue_i, trial_base_len, raw_root_filters2, pos, raw_counts2)
                  mut exps_i = nil
                  if rem_i != 1 && has_poly && a_divisor_relations == 0 && factor_base.len >= 50 && (!large_relation_target || score >= fallback_score_min) {
                     def smooth = _qs_factor_over_base_profile_intbase(residue_i, trial_base)
                     trial_prime_tests += int(smooth[3])
                     trial_divisions += int(smooth[4])
                     nonzero_terms += int(smooth[5])
                     if smooth[0] {
                        rem_i = 1
                        exps_i = smooth[1]
                     }
                  }
                  if rem_i == 1 {
                     smooth_hits += 1
                     if exps_i == nil {
                        relations[relation_count] = _qs_relation_int_raw_mpqs(x_i, residue_i, trial_base, raw_root_filters2)
                     } else {
                        relations[relation_count] = _qs_relation_compact(x, exps_i)
                     }
                     relation_count += 1
                     found += 1
                     if has_poly { a_divisor_relations += 1 }
                  }
               } else {
                  mut smooth = _qs_factor_over_base_profile_filtered(residue, factor_base, root_filters, off)
                  trial_prime_tests += int(smooth[3])
                  trial_divisions += int(smooth[4])
                  nonzero_terms += int(smooth[5])
                  if !smooth[0] && has_poly && a_divisor_relations == 0 && factor_base.len >= 50 && (!large_relation_target || score >= fallback_score_min) {
                     smooth = _qs_factor_over_base_profile(residue, factor_base)
                     trial_prime_tests += int(smooth[3])
                     trial_divisions += int(smooth[4])
                     nonzero_terms += int(smooth[5])
                  }
                  if smooth[0] {
                     smooth_hits += 1
                     relations[relation_count] = _qs_relation_compact(x, smooth[1])
                     relation_count += 1
                     found += 1
                     if has_poly { a_divisor_relations += 1 }
                  }
               }
            }
            si += 1
         }
         trial_prime_tests += load64_i(raw_counts2, 0)
         trial_divisions += load64_i(raw_counts2, 8)
         nonzero_terms += load64_i(raw_counts2, 16)
         return _mpqs_collect_result(nil, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations)
      }
   }
   mut si = 0
   while si < survivor_count && relation_count < max_relations {
      def survivor = int(survivors[si])
      def off = (survivor >> 8) - radius
      def score = survivor & 255
      def x = has_poly ? (poly_A * off + poly_B) : center + off
      def residue = mod(x * x, modulus)
      if residue == 0 {
         def gx = _qs_square_relation_factor(x, residue, nz)
         if gx != nil { return _mpqs_collect_result(gx, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations) }
      } else {
         prefilter_tested += 1
         smooth_tests += 1
         mut smooth = modulus_bits <= 62 ? _qs_factor_over_base_profile_filtered_int(bigint_to_int(residue), factor_base, root_filters, off) : _qs_factor_over_base_profile_filtered(residue, factor_base, root_filters, off)
         trial_prime_tests += int(smooth[3])
         trial_divisions += int(smooth[4])
         nonzero_terms += int(smooth[5])
         if !smooth[0] && has_poly && a_divisor_relations == 0 && factor_base.len >= 50 && (!large_relation_target || score >= fallback_score_min) {
            smooth = modulus_bits <= 62 ? _qs_factor_over_base_profile_intbase(bigint_to_int(residue), trial_base) : _qs_factor_over_base_profile(residue, factor_base)
            trial_prime_tests += int(smooth[3])
            trial_divisions += int(smooth[4])
            nonzero_terms += int(smooth[5])
         }
         if smooth[0] {
            smooth_hits += 1
            relations[relation_count] = _qs_relation_compact(x, smooth[1])
            relation_count += 1
            found += 1
            if has_poly { a_divisor_relations += 1 }
         }
      }
      si += 1
   }
   _mpqs_collect_result(nil, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, prefilter_tested, 0, a_divisor_relations)
}

fn _mpqs_collect_byte_window_relations(any nz, any modulus, list factor_base, list<int> trial_base, dict byte_report, any poly, int w, any center, int max_relations, list relations_in) dict {
   def list<int> survivors = byte_report.get("survivors", [])
   def survivor_count = survivors.len
   if survivor_count == 0 {
      return _mpqs_collect_result(nil, _mpqs_relation_buffer_trim(_mpqs_relation_buffer(relations_in, max_relations), relations_in.len), 0, 0, 0, 0, 0, 0, 0, 0, 0)
   }
   def large_relation_target = max_relations >= 256 || factor_base.len >= 80
   def has_poly = poly != nil
   def raw_poly_A = has_poly ? poly.get("A") : 0
   def raw_poly_B = has_poly ? poly.get("B") : 0
   mut list<int> root_filters = byte_report.get("root_filters", [])
   if root_filters.len != factor_base.len * 4 { root_filters = _mpqs_plain_root_filter(modulus, factor_base, center) }
   def radius = int(byte_report.get("radius", 0))
   def fallback_score_min = int(byte_report.get("score_threshold", 0)) + 2
   def modulus_bits = bit_length(modulus)
   mut int_residue_path = false
   mut modulus_i, center_i, poly_A_i, poly_B_i = 0, 0, 0, 0
   if modulus_bits <= 62 {
      modulus_i = is_int(modulus) ? int(modulus) : bigint_to_int(modulus)
      if has_poly {
         if bit_length(raw_poly_A) <= 31 && bit_length(raw_poly_B) <= 62 {
            poly_A_i = is_int(raw_poly_A) ? int(raw_poly_A) : bigint_to_int(raw_poly_A)
            poly_B_i = is_int(raw_poly_B) ? int(raw_poly_B) : bigint_to_int(raw_poly_B)
            def high_i = poly_A_i * radius + poly_B_i
            def low_i = poly_B_i - poly_A_i * radius
            int_residue_path = (high_i < 0 ? -high_i : high_i) <= 3037000499 && (low_i < 0 ? -low_i : low_i) <= 3037000499
         }
      } else if is_int(center) || bit_length(center) <= 62 {
         center_i = is_int(center) ? int(center) : bigint_to_int(center)
         def high_i = center_i + radius
         def low_i = center_i - radius
         int_residue_path = (high_i < 0 ? -high_i : high_i) <= 3037000499 && (low_i < 0 ? -low_i : low_i) <= 3037000499
      }
   }
   if int_residue_path {
      _mpqs_collect_byte_window_relations_int(nz, modulus_i, factor_base, trial_base, survivors, survivor_count, radius, fallback_score_min, has_poly, poly_A_i, poly_B_i, center_i, root_filters, max_relations, relations_in, large_relation_target)
   } else {
      _mpqs_collect_byte_window_relations_big(nz, modulus, factor_base, trial_base, survivors, survivor_count, radius, fallback_score_min, has_poly, raw_poly_A, raw_poly_B, center, root_filters, max_relations, relations_in, large_relation_target, modulus_bits)
   }
}

fn _mpqs_collect_fallback_window_relations(ptr scratch, any nz, any modulus, list base, list base_sqrt_roots, list<int> base_ints, list<int> base_sqrt_roots_int, list<int> center_mods, list factor_base, list<int> trial_base, any center, any start, int window_radius, int score_min, int w, int max_relations, list relations_in) dict {
   mut relation_count = relations_in.len
   mut relations = _mpqs_relation_buffer(relations_in, max_relations)
   mut found, smooth_tests, smooth_hits = 0, 0, 0
   mut trial_prime_tests, trial_divisions, nonzero_terms, fallback_tests = 0, 0, 0, 0
   def root_filter_bytes = max(32, base.len * 32)
   with ptr root_filters = malloc(root_filter_bytes + 24){
      if !root_filters {
         return _mpqs_collect_result(nil, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, 0, fallback_tests, 0)
      }
      _mpqs_raw_byte_sieve_score_window_int_roots(scratch, root_filters, base_ints, base_sqrt_roots_int, center_mods, window_radius)
      def sieve_len = window_radius * 2 + 1
      def high_x = center + window_radius
      def low_x = center - window_radius
      def int_residue_path = bit_length(modulus) <= 62 && bit_length(high_x < 0 ? -high_x : high_x) <= 31 && bit_length(low_x < 0 ? -low_x : low_x) <= 31 && bit_length(start < 0 ? -start : start) <= 31
      if int_residue_path {
         def modulus_i = is_int(modulus) ? int(modulus) : bigint_to_int(modulus)
         def center_i = is_int(center) ? int(center) : bigint_to_int(center)
         def start_i = is_int(start) ? int(start) : bigint_to_int(start)
         def raw_counts = ptr_add(root_filters, root_filter_bytes)
         def trial_base_len = trial_base.len
         memset(raw_counts, 0, 24)
         mut pos_i = max(0, start_i - center_i + window_radius)
         while pos_i < sieve_len && relation_count < max_relations {
            def off = pos_i - window_radius
            def x_i = center_i + off
            def score = int(load8(scratch, pos_i))
            if score > 0 && score < score_min {
               mut residue_i = (x_i * x_i) % modulus_i
               if residue_i < 0 { residue_i += modulus_i }
               if residue_i == 0 {
                  def gx = _qs_square_relation_factor(Z(x_i), Z(0), nz)
                  if gx != nil { return _mpqs_collect_result(gx, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, 0, fallback_tests, 0) }
               } else {
                  fallback_tests += 1
                  smooth_tests += 1
                  def rem_i = off >= 0 ? _qs_factor_over_base_filtered_int_raw_scan_pos_limited(residue_i, trial_base_len, root_filters, off, score + 1, raw_counts) : _qs_factor_over_base_filtered_int_raw_scan_limited(residue_i, trial_base_len, root_filters, off, score + 1, raw_counts)
                  if rem_i == 1 {
                     smooth_hits += 1
                     relations[relation_count] = _qs_relation_int_raw_mpqs(x_i, residue_i, trial_base, root_filters)
                     relation_count += 1
                     found += 1
                  }
               }
            }
            pos_i += 1
         }
         trial_prime_tests += load64_i(raw_counts, 0)
         trial_divisions += load64_i(raw_counts, 8)
         nonzero_terms += load64_i(raw_counts, 16)
         return _mpqs_collect_result(nil, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, 0, fallback_tests, 0)
      }
      def root_filters_list = _mpqs_plain_root_filter(modulus, factor_base, center)
      mut pos = 0
      if center < start {
         pos = int(start - center) + window_radius
         if pos < 0 { pos = 0 }
      }
      while pos < sieve_len && relation_count < max_relations {
         def off = pos - window_radius
         def x = center + off
         def score = int(load8(scratch, pos))
         if score > 0 && score < score_min {
            def residue = mod(x * x, modulus)
            if residue == 0 {
               def gx = _qs_square_relation_factor(x, residue, nz)
               if gx != nil { return _mpqs_collect_result(gx, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, 0, fallback_tests, 0) }
            } else {
               fallback_tests += 1
               smooth_tests += 1
               def smooth = _qs_factor_over_base_profile_filtered(residue, factor_base, root_filters_list, off)
               trial_prime_tests += int(smooth[3])
               trial_divisions += int(smooth[4])
               nonzero_terms += int(smooth[5])
               if smooth[0] {
                  smooth_hits += 1
                  relations[relation_count] = _qs_relation_compact(x, smooth[1])
                  relation_count += 1
                  found += 1
               }
            }
         }
         pos += 1
      }
      _mpqs_collect_result(nil, _mpqs_relation_buffer_trim(relations, relation_count), found, smooth_tests, smooth_hits, trial_prime_tests, trial_divisions, nonzero_terms, 0, fallback_tests, 0)
   }
}

fn _mpqs_window_stats_add(dict stats, dict step) dict {
   _dict_add_int_fields(stats, step, [
         ["candidate_count", "candidate_count"],
         ["smooth_tests", "smooth_tests"],
         ["smooth_hits", "smooth_hits"],
         ["trial_prime_tests", "trial_prime_tests"],
         ["trial_divisions", "trial_divisions"],
         ["nonzero_exponent_terms", "nonzero_exponent_terms"],
         ["prefilter_skipped", "prefilter_skipped"],
         ["prefilter_tested", "prefilter_tested"],
         ["fallback_tests", "fallback_tests"],
         ["sieve_marked_roots", "sieve_marked_roots"],
         ["a_divisor_window_count", "a_divisor_window_count"],
         ["a_divisor_relation_count", "a_divisor_relation_count"],
      ])
}

fn _mpqs_collect_window_step(
   ptr scratch, any nz, any modulus, list base, list base_sqrt_roots, list<int> base_ints, list<int> base_sqrt_roots_int,
   list factor_base, list<int> trial_base, dict base_buckets, list<int> adiv_sieve_base_ints, dict adiv_buckets, list adiv_polys,
   list<int> center_mods,
   bool adiv_enabled, int pass, int w, any center, any start, int window_radius,
   int score_min, int max_relations, list relations_in
) dict {
   mut relations = relations_in
   mut found, window_candidates, window_smooth_tests = 0, 0, 0
   mut window_trial_prime_tests, window_trial_divisions = 0, 0
   mut byte_report = dict()
   mut step = dict(24)
   if pass == 0 {
      def byte_choice = _mpqs_byte_window_choice(scratch, modulus, base, base_sqrt_roots, base_ints, base_sqrt_roots_int, center_mods, base_buckets, adiv_sieve_base_ints, adiv_buckets, adiv_polys, adiv_enabled, w, center, window_radius, score_min, start)
      def poly = byte_choice[0]
      byte_report = byte_choice[1]
      window_candidates = int(byte_report.get("candidate_count", 0))
      def collected = _mpqs_collect_byte_window_relations(nz, modulus, factor_base, trial_base, byte_report, poly, w, center, max_relations, relations)
      def gx = collected.get("factor", nil)
      if gx != nil { return _set_fields(step, [["factor", gx], ["relations", collected.get("relations", relations)]]) }
      relations = collected.get("relations", relations)
      found = int(collected.get("found", 0))
      window_smooth_tests = int(collected.get("smooth_tests", 0))
      window_trial_prime_tests = int(collected.get("trial_prime_tests", 0))
      window_trial_divisions = int(collected.get("trial_divisions", 0))
      def byte_summary = _mpqs_byte_sieve_window_summary(byte_report, w, found)
      return _set_fields(step, [
            ["factor", nil], ["relations", relations], ["found", found],
            ["byte_report", byte_report], ["byte_summary", byte_summary],
            ["window_report", _mpqs_window_report(byte_summary, w, center, window_radius, adiv_enabled, adiv_polys, found, window_candidates, window_smooth_tests, window_trial_prime_tests, window_trial_divisions, byte_report)],
            ["candidate_count", window_candidates],
            ["prefilter_skipped", int(byte_report.get("skipped_count", 0))],
            ["sieve_marked_roots", int(byte_report.get("marked_roots", 0))],
            ["a_divisor_window_count", int(byte_choice[2])],
            ["a_divisor_relation_count", int(collected.get("a_divisor_relation_count", 0))],
            ["prefilter_tested", int(collected.get("prefilter_tested", 0))],
            ["smooth_tests", window_smooth_tests],
            ["smooth_hits", int(collected.get("smooth_hits", 0))],
            ["trial_prime_tests", window_trial_prime_tests],
            ["trial_divisions", window_trial_divisions],
            ["nonzero_exponent_terms", int(collected.get("nonzero_exponent_terms", 0))],
         ])
   }
   def collected = _mpqs_collect_fallback_window_relations(scratch, nz, modulus, base, base_sqrt_roots, base_ints, base_sqrt_roots_int, center_mods, factor_base, trial_base, center, start, window_radius, score_min, w, max_relations, relations)
   def gx = collected.get("factor", nil)
   if gx != nil { return _set_fields(step, [["factor", gx], ["relations", collected.get("relations", relations)]]) }
   relations = collected.get("relations", relations)
   found = int(collected.get("found", 0))
   window_smooth_tests = int(collected.get("smooth_tests", 0))
   window_trial_prime_tests = int(collected.get("trial_prime_tests", 0))
   window_trial_divisions = int(collected.get("trial_divisions", 0))
   _set_fields(step, [
         ["factor", nil], ["relations", relations], ["found", found],
         ["fallback_tests", int(collected.get("fallback_tests", 0))],
         ["smooth_tests", window_smooth_tests],
         ["smooth_hits", int(collected.get("smooth_hits", 0))],
         ["trial_prime_tests", window_trial_prime_tests],
         ["trial_divisions", window_trial_divisions],
         ["nonzero_exponent_terms", int(collected.get("nonzero_exponent_terms", 0))],
      ])
}

fn _mpqs_maybe_early_dependency(any nz, any modulus, list base, list relations, dict early_dependency, int pass, int w, int found) dict {
   if bool(early_dependency.get("used", false)) || found <= 0 || relations.len < 2 { return early_dependency }
   if base.len >= 64 {
      def stride = base.len >= 512 ? 32 : (base.len >= 128 ? 16 : 8)
      def before = max(0, relations.len - found)
      if relations.len < stride || before / stride == relations.len / stride { return early_dependency }
   }
   def trial_solve = _qs_dependency_solve_report(nz, modulus, base, relations, 8192, "lanczos")
   trial_solve.get("success", false) ? _mpqs_early_dependency(trial_solve, true, pass, w, relations.len) : early_dependency
}

fn _mpqs_window_finish_fields(
   any nz, any modulus, list base, dict base_report, list relations,
   dict adiv_cycle, bool adiv_enabled, list adiv_polys, list adiv_sieve_base,
   dict stats, any rel_t0, dict byte_totals, list byte_sieve_window_reports,
   int window_radius, dict early_dependency, list window_reports
) list {
   def filter_report = qs_relation_filter_report(relations, base.len, true)
   def early_dependency_used = bool(early_dependency.get("used", false))
   mut solve, raw_solve = early_dependency.get("solve"), nil
   mut solved_with_filter = false
   mut solved_relations, solve_relation_set = relations, "raw"
   if !early_dependency_used {
      def solved = _qs_dependency_solve_filtered_report(nz, modulus, base, relations, filter_report, 8192)
      solve = solved.get("solve")
      raw_solve = solved.get("raw_solve", nil)
      solved_with_filter = solved.get("used_filter", false)
      solved_relations = solved.get("relations", relations)
      solve_relation_set = solved.get("relation_set", "raw")
   }
   mut fields = _fields_extend([
         ["factor_base", base], ["factor_base_size", base.len],
         ["factor_base_report", base_report], ["raw_relations", relations],
      ], _mpqs_a_divisor_fields(adiv_cycle, adiv_enabled, adiv_polys.len, adiv_sieve_base.len, _dict_int(stats, "a_divisor_window_count"), _dict_int(stats, "a_divisor_relation_count")))
   fields = _fields_extend(fields, [
         ["raw_relation_count", relations.len], ["relations", solved_relations],
         ["relation_count", relations.len],
         ["sieve_prefilter_enabled", true], ["sieve_score_min", _mpqs_sieve_score_min(base)],
         ["sieve_marked_roots", _dict_int(stats, "sieve_marked_roots")],
         ["prefilter_tested", _dict_int(stats, "prefilter_tested")],
         ["prefilter_skipped", _dict_int(stats, "prefilter_skipped")],
         ["fallback_tests", _dict_int(stats, "fallback_tests")],
      ])
   fields = _fields_extend(fields, _qs_relation_collection_fields(rel_t0, _dict_int(stats, "candidate_count"), _dict_int(stats, "smooth_tests"), _dict_int(stats, "smooth_hits"), _dict_int(stats, "trial_prime_tests"), _dict_int(stats, "trial_divisions"), _dict_int(stats, "nonzero_exponent_terms")))
   fields = _fields_extend(fields, _mpqs_byte_total_fields(byte_totals, byte_sieve_window_reports, window_radius))
   _fields_extend(fields, _mpqs_window_solve_fields(solved_relations, early_dependency, early_dependency_used, solve, filter_report, solved_with_filter, solve_relation_set, raw_solve, window_reports))
}

fn _mpqs_attempt_collect_state() dict {
   _dict_with(8, [
         ["relations", []],
         ["window_reports", []],
         ["stats", dict()],
         ["byte_sieve_window_reports", []],
         ["byte_totals", dict()],
         ["early_dependency", _mpqs_early_dependency()],
         ["factor", nil],
      ])
}

fn _mpqs_attempt_apply_step(dict state, dict step, int pass, int w, any nz, any modulus, list base, int windows) dict {
   def gx = step.get("factor", nil)
   if gx != nil { return state.set("factor", gx).set("relations", step.get("relations", state.get("relations", []))) }
   mut relations = step.get("relations", state.get("relations", []))
   mut stats = _mpqs_window_stats_add(state.get("stats", dict()), step)
   mut byte_totals = state.get("byte_totals", dict())
   mut byte_sieve_window_reports = state.get("byte_sieve_window_reports", [])
   mut window_reports = state.get("window_reports", [])
   def found = int(step.get("found", 0))
   if pass == 0 {
      def byte_report = step.get("byte_report", dict())
      byte_totals = _mpqs_byte_totals_add(byte_totals, byte_report)
      byte_sieve_window_reports = byte_sieve_window_reports.append(step.get("byte_summary", dict()))
      window_reports = window_reports.append(step.get("window_report", dict()))
   }
   mut early_dependency = state.get("early_dependency", _mpqs_early_dependency())
   def short_attempt = windows <= 3 && base.len >= 32
   def periodic_early_solve = base.len < 90 || (((w + 1) % 4) == 0 && relations.len <= 16)
   if (!short_attempt && (relations.len > base.len || periodic_early_solve)) || (short_attempt && relations.len > base.len) {
      early_dependency = _mpqs_maybe_early_dependency(nz, modulus, base, relations, early_dependency, pass, w, found)
   }
   _set_fields(state, [
         ["relations", relations],
         ["stats", stats],
         ["byte_totals", byte_totals],
         ["byte_sieve_window_reports", byte_sieve_window_reports],
         ["window_reports", window_reports],
         ["early_dependency", early_dependency],
         ["stop", bool(early_dependency.get("used", false))],
      ])
}

fn _mpqs_attempt_finish_pass(dict state, int pass, int windows, any nz, any modulus, list base) dict {
   def relations = state.get("relations", [])
   if pass == 0 && relations.len >= 2 {
      def early_dependency = _mpqs_maybe_early_dependency(nz, modulus, base, relations, state.get("early_dependency", _mpqs_early_dependency()), pass, windows - 1, 1)
      return state.set("early_dependency", early_dependency).set("pass", bool(early_dependency.get("used", false)) ? 2 : pass + 1)
   }
   state.set("pass", pass + 1)
}

fn _mpqs_attempt_collect_pass(
   dict state, ptr scratch, any nz, any modulus, list base, list base_sqrt_roots, list<int> base_ints, list<int> base_sqrt_roots_int,
   list factor_base, list<int> trial_base, dict base_buckets, list<int> adiv_sieve_base_ints, dict adiv_buckets, list adiv_polys, bool adiv_enabled, int pass, any start, int windows,
   int window_radius, int score_min, int max_relations
) dict {
   def stride = max(1, window_radius * 2 + 1)
   def start_mods = _mpqs_start_mods(base_ints, start, stride)
   mut list<int> center_mods = start_mods[0]
   mut list<int> stride_mods = start_mods[1]
   mut current = state
   mut w = 0
   while w < windows && current.get("relations", []).len < max_relations {
      def center = start + w * stride
      def step = _mpqs_collect_window_step(scratch, nz, modulus, base, base_sqrt_roots, base_ints, base_sqrt_roots_int, factor_base, trial_base, base_buckets, adiv_sieve_base_ints, adiv_buckets, adiv_polys, center_mods, adiv_enabled, pass, w, center, start, window_radius, score_min, max_relations, current.get("relations", []))
      current = _mpqs_attempt_apply_step(current, step, pass, w, nz, modulus, base, windows)
      if current.get("factor", nil) != nil || bool(current.get("stop", false)) {
         return current.set("pass", 2)
      }
      _mpqs_advance_center_mods(center_mods, stride_mods, base_ints)
      w += 1
   }
   _mpqs_attempt_finish_pass(current, pass, windows, nz, modulus, base)
}

fn _mpqs_attempt_collect_windows(
   ptr scratch, any nz, any modulus, list base, list factor_base, list<int> trial_base, list adiv_sieve_base,
   list adiv_polys, bool adiv_enabled, any start, int windows,
   int window_radius, int max_relations
) dict {
   def score_min = _mpqs_sieve_score_min(base)
   def base_sqrt_roots = _mpqs_sqrt_roots(modulus, base)
   def base_ints = _qs_factor_base_ints(base)
   def base_sqrt_roots_int = _mpqs_roots_ints(base_sqrt_roots)
   def base_buckets = _mpqs_bucketed_prime_loop_report(base, window_radius * 2 + 1)
   def adiv_sieve_base_ints = _qs_factor_base_ints(adiv_sieve_base)
   def adiv_buckets = _mpqs_bucketed_prime_loop_report(adiv_sieve_base, window_radius * 2 + 1)
   mut state = _mpqs_attempt_collect_state().set("pass", 0)
   while int(state.get("pass", 0)) < 2 && state.get("relations", []).len < max_relations {
      state = _mpqs_attempt_collect_pass(state, scratch, nz, modulus, base, base_sqrt_roots, base_ints, base_sqrt_roots_int, factor_base, trial_base, base_buckets, adiv_sieve_base_ints, adiv_buckets, adiv_polys, adiv_enabled, int(state.get("pass", 0)), start, windows, window_radius, score_min, max_relations)
   }
   state
}

fn _mpqs_window_attempt_report(any n, int multiplier, int factor_base_bound, int windows, int window_radius, int max_relations) dict {
   def t0 = ticks()
   def nz = _z(n)
   def modulus = nz * Z(multiplier)
   mut out = _set_fields(_report("mpqs-window-attempt", 22), [
         ["n", nz], ["multiplier", multiplier], ["sieve_modulus", modulus],
         ["factor_base_bound", factor_base_bound], ["windows", windows],
         ["window_radius", window_radius], ["max_relations", max_relations],
      ])
   if nz <= Z(1) { return _set_factor_status(out, nil, false) }
   if nz % Z(2) == Z(0) { return _set_factor_status(out, Z(2), true) }
   def base_report = _qs_factor_base_report(nz, modulus, factor_base_bound)
   if base_report.get("factor", nil) != nil {
      return _finish_report_with(out, t0, [
            ["factor", base_report.get("factor")], ["success", true],
            ["factor_base", base_report.get("base", [])], ["factor_base_report", base_report],
         ])
   }
   def base = base_report.get("base", [])
   def factor_base = _qs_factor_base_bigints(base)
   def trial_base = _qs_factor_base_ints(factor_base)
   def adiv_cycle = _mpqs_a_divisor_cycle_from_base(modulus, base, window_radius, max(1, windows))
   def adiv_polys = adiv_cycle.get("polynomials", [])
   def adiv_sieve_base = adiv_cycle.get("sieve_base", base)
   def adiv_enabled = adiv_polys.len > 0
   mut start = isqrt(modulus)
   if start * start < modulus { start = start + Z(1) }
   def start_for_collect = bit_length(start) <= 62 ? bigint_to_int(start) : start
   def rel_t0 = ticks()
   def byte_sieve_scratch = malloc(window_radius * 2 + 1)
   if !byte_sieve_scratch {
      return _finish_report_with(out, t0, [["factor", nil], ["success", false], ["reason", "byte sieve allocation failed"]])
   }
   defer { free(byte_sieve_scratch) }
   def collected = _mpqs_attempt_collect_windows(byte_sieve_scratch, nz, modulus, base, factor_base, trial_base, adiv_sieve_base, adiv_polys, adiv_enabled, start_for_collect, windows, window_radius, max_relations)
   def gx = collected.get("factor", nil)
   if gx != nil { return _finish_report_with(out, t0, [["factor", gx], ["success", true], ["relations", collected.get("relations", [])]]) }
   _finish_report_with(out, t0, _mpqs_window_finish_fields(nz, modulus, base, base_report, collected.get("relations", []), adiv_cycle, adiv_enabled, adiv_polys, adiv_sieve_base, collected.get("stats", dict()), rel_t0, collected.get("byte_totals", dict()), collected.get("byte_sieve_window_reports", []), window_radius, collected.get("early_dependency", _mpqs_early_dependency()), collected.get("window_reports", [])))
}

fn _mpqs_attempt_policy_report(any n, int selected_multiplier, int factor_base_bound, int windows, int window_radius, int max_relations) dict {
   def t0 = ticks()
   def nz = _z(n)
   def input_bits = bit_length(nz < Z(0) ? -nz : nz)
   def bounded_preflight = selected_multiplier != 1 && input_bits >= 32 && input_bits <= 52 && factor_base_bound <= 1201 && window_radius <= 32000 && max_relations <= 256
   mut unit_preflight = nil
   mut selected_attempt = nil
   mut fallback = nil
   mut used = nil
   mut unit_preflight_used = false
   if bounded_preflight {
      unit_preflight = _mpqs_window_attempt_report(nz, 1, factor_base_bound, windows, window_radius, max_relations)
      if unit_preflight.get("success", false) {
         used = unit_preflight
         unit_preflight_used = true
      }
   }
   if used == nil {
      selected_attempt = _mpqs_window_attempt_report(nz, selected_multiplier, factor_base_bound, windows, window_radius, max_relations)
      used = selected_attempt
      if !selected_attempt.get("success", false) && selected_multiplier != 1 {
         fallback = unit_preflight == nil ? _mpqs_window_attempt_report(nz, 1, factor_base_bound, windows, window_radius, max_relations) : unit_preflight
         if fallback.get("success", false) { used = fallback }
      }
   }
   _finish_report_with(_report("mpqs-attempt-policy", 18), t0, [
         ["selected_multiplier", selected_multiplier],
         ["input_bits", input_bits],
         ["bounded_unit_preflight", bounded_preflight],
         ["unit_multiplier_preflight_attempt", unit_preflight],
         ["unit_multiplier_preflight_used", unit_preflight_used],
         ["selected_multiplier_attempt", selected_attempt],
         ["selected_multiplier_attempt_run", selected_attempt != nil],
         ["primary_attempt", selected_attempt == nil ? unit_preflight : selected_attempt],
         ["fallback_attempt", fallback],
         ["used_attempt", used],
         ["used_multiplier", used == nil ? selected_multiplier : used.get("multiplier", selected_multiplier)],
         ["used_fallback", fallback != nil && used != nil && used.get("multiplier", selected_multiplier) == 1],
         ["success", used != nil && used.get("success", false)],
      ])
}

fn mpqs_source_parameter_rows() list { [
      [64, 100, 40, 65536], [128, 450, 40, 65536], [183, 2000, 40, 65536],
      [200, 3000, 50, 65536], [212, 5400, 50, 3 * 65536], [233, 10000, 100, 3 * 65536],
      [249, 27000, 100, 3 * 65536], [266, 50000, 100, 3 * 65536], [283, 55000, 80, 3 * 65536],
      [298, 60000, 80, 9 * 65536], [315, 80000, 150, 9 * 65536], [332, 100000, 150, 9 * 65536],
      [348, 140000, 150, 9 * 65536], [363, 210000, 150, 13 * 65536], [379, 300000, 150, 17 * 65536],
      [395, 400000, 150, 21 * 65536], [415, 500000, 150, 25 * 65536], [440, 700000, 150, 33 * 65536],
      [465, 900000, 150, 50 * 65536], [490, 1100000, 150, 75 * 65536], [512, 1300000, 150, 100 * 65536],
   ] }

fn _mpqs_param_dict(list row) dict {
   def fb = int(row.get(1, 100))
   def large_mult = int(row.get(2, 40))
   def sieve_size = int(row.get(3, 65536))
   _dict_with(10, [
         ["bits", int(row.get(0, 64))],
         ["factor_base_size", max(100, fb)],
         ["large_prime_multiplier", large_mult],
         ["sieve_size", sieve_size],
         ["large_prime_max_estimate", max(100, fb) * large_mult],
         ["sieve_blocks_64k", max(1, sieve_size / 65536)],
      ])
}

fn _mpqs_param_result(list row, int bits, bool interpolated, any bracket) dict { _set_fields(_mpqs_param_dict(row), [["input_bits", bits], ["interpolated", interpolated], ["source_bracket", bracket]]) }

fn _mpqs_interp_int(int lo, int hi, int ibits, int lbits, int hbits) int {
   def dist = max(1, hbits - lbits)
   int(((float(lo) * float(hbits - ibits)) + (float(hi) * float(ibits - lbits))) / float(dist) + 0.5)
}

fn _mpqs_sieve_params_for_bits(int bits) dict {
   def rows = mpqs_source_parameter_rows()
   if bits <= int(rows.get(0).get(0)) { return _mpqs_param_result(rows.get(0), bits, false, "floor") }
   def last = rows.get(rows.len - 1)
   if bits >= int(last.get(0)) { return _mpqs_param_result(last, bits, false, "ceiling") }
   mut i = 0
   while i < rows.len - 1 {
      def lo = rows.get(i)
      def hi = rows.get(i + 1)
      def lbits = int(lo.get(0))
      def hbits = int(hi.get(0))
      if bits < hbits {
         def fb = _mpqs_interp_int(int(lo.get(1)), int(hi.get(1)), bits, lbits, hbits)
         def large_mult = _mpqs_interp_int(int(lo.get(2)), int(hi.get(2)), bits, lbits, hbits)
         def sieve_size = _mpqs_interp_int(int(lo.get(3)), int(hi.get(3)), bits, lbits, hbits)
         return _mpqs_param_result([bits, fb, large_mult, sieve_size], bits, true, [lbits, hbits])
      }
      i += 1
   }
   _mpqs_param_result(last, bits, false, "fallback")
}

fn mpqs_sieve_parameter_report(any n, bool input_is_bits=false) dict {
   "Return source-derived MPQS sieve parameters: factor-base size, large-prime multiplier, and sieve size."
   def t0 = ticks()
   def bits = input_is_bits ? int(n) : bit_length(_z(n) < Z(0) ? -_z(n) : _z(n))
   def params = _mpqs_sieve_params_for_bits(bits)
   _finish_report_with(_report("mpqs-source-sieve-parameters", 18), t0, [
         ["input_bits", bits],
         ["source_model", "MPQS prebuilt-parameter linear interpolation"],
         ["source_rows", mpqs_source_parameter_rows().len],
         ["parameters", params],
         ["factor_base_size", params.get("factor_base_size")],
         ["large_prime_multiplier", params.get("large_prime_multiplier")],
         ["large_prime_max_estimate", params.get("large_prime_max_estimate")],
         ["sieve_size", params.get("sieve_size")],
         ["sieve_blocks_64k", params.get("sieve_blocks_64k")],
         ["interpolated", params.get("interpolated", false)],
         ["source_bracket", params.get("source_bracket", nil)],
      ])
}

fn _mpqs_bound_for_target_base_count(any n, any modulus, int target_count, int max_prime_bound) dict {
   def t0 = ticks()
   def nz, mz = _z(n), _z(modulus)
   mut bound = 31
   mut rep = _qs_factor_base_report(nz, mz, bound)
   mut probes = 1
   while rep.get("factor", nil) == nil && rep.get("base", []).len < target_count && bound < max_prime_bound {
      bound = min(max_prime_bound, max(bound + 2, int(float(bound) * 1.35) + 8))
      rep = _qs_factor_base_report(nz, mz, bound)
      probes += 1
   }
   _finish_report_with(_report("mpqs-factor-base-count-to-bound", 14), t0, [
         ["target_factor_base_count", target_count],
         ["max_prime_bound", max_prime_bound],
         ["selected_prime_bound", bound],
         ["actual_factor_base_count", rep.get("base", []).len],
         ["reached_target", rep.get("base", []).len >= target_count],
         ["hit_prime_bound_cap", bound >= max_prime_bound && rep.get("base", []).len < target_count],
         ["probe_count", probes],
         ["factor_base_report", rep],
         ["factor", rep.get("factor", nil)],
      ])
}

fn mpqs_source_work_plan_report(any n, int max_factor_base_count=96, int max_prime_bound=1201, int max_windows=24, int max_window_radius=32000, int max_relations=256) dict {
   "Return a bounded MPQS work plan derived from the source MPQS parameter table."
   def t0 = ticks()
   def nz = _z(n)
   def bits = bit_length(nz < Z(0) ? -nz : nz)
   def source = mpqs_sieve_parameter_report(nz)
   def source_count = int(source.get("factor_base_size", 100))
   mut target_count = min(max(1, max_factor_base_count), source_count)
   if bits >= 43 && bits <= 44 && target_count >= 48 { target_count = 36 }
   def selected_mult = _qs_best_multiplier_fast(nz, max(31, min(max_prime_bound, 2 * max(31, target_count))))
   def modulus = nz * Z(selected_mult)
   def bound = _mpqs_bound_for_target_base_count(nz, modulus, target_count, max_prime_bound)
   def blocks64 = max(1, int(source.get("sieve_blocks_64k", 1)))
   def windows = min(max(1, max_windows), max(3, blocks64 * 3))
   def raw_radius = max(128, int(int(source.get("sieve_size", 65536)) / max(2, windows * 2)))
   def fallback_radius_cap = (bits >= 43 && bits <= 44 && target_count <= 36) ? 2400 : (target_count >= 48 ? target_count * 50 : raw_radius)
   def radius = min(max_window_radius, min(raw_radius, fallback_radius_cap))
   def rels = min(max_relations, max(32, target_count + 8))
   _finish_report_with(_report("mpqs-source-work-plan", 24), t0, [
         ["bits", bits],
         ["source_driven", true],
         ["source_parameter_report", source],
         ["source_factor_base_size", source_count],
         ["target_factor_base_count", target_count],
         ["factor_base_count_capped", target_count < source_count],
         ["factor_base_bound_report", bound],
         ["factor_base_bound", bound.get("selected_prime_bound", max_prime_bound)],
         ["actual_factor_base_count", bound.get("actual_factor_base_count", 0)],
         ["factor_base_target_reached", bound.get("reached_target", false)],
         ["selected_multiplier", selected_mult],
         ["sieve_modulus", modulus],
         ["source_sieve_size", source.get("sieve_size", 65536)],
         ["source_sieve_blocks_64k", blocks64],
         ["windows", windows],
         ["raw_window_radius", raw_radius],
         ["window_radius", radius],
         ["window_radius_capped", radius < raw_radius],
         ["max_relations", rels],
         ["max_factor_base_count", max_factor_base_count],
         ["max_prime_bound", max_prime_bound],
         ["max_windows", max_windows],
         ["max_window_radius", max_window_radius],
         ["requested_max_relations", max_relations],
      ])
}

fn _mpqs_default_work_plan(int bits) list {
   case bits {
      _ if bits <= 16 -> [31, 3, 128, 20]
      _ if bits <= 28 -> [213, 3, 2000, 40]
      _ if bits <= 36 -> [337, 3, 4000, 44]
      _ if bits <= 42 -> [401, 3, 8000, 53]
      _ if bits <= 46 -> [503, 3, 4000, 56]
      _ if bits <= 50 -> [601, 3, 6000, 64]
      _ if bits <= 56 -> [1031, 3, 10000, 80]
      _ if bits <= 64 -> [1201, 16, 9600, 96]
      _ -> [1201, 24, 12000, 256]
   }
}

fn mpqs_work_plan_report(any n, int factor_base_bound=64, int windows=4, int window_radius=256, int max_relations=32) dict {
   "Return the MPQS work plan used by default-size calls."
   def nz = _z(n)
   def bits = bit_length(nz < Z(0) ? -nz : nz)
   def source_params = mpqs_sieve_parameter_report(nz)
   def use_default = factor_base_bound == 64 && windows == 4 && window_radius == 256 && max_relations == 32
   mut fb = factor_base_bound
   mut win = windows
   mut radius = window_radius
   mut rels = max_relations
   mut compacted = false
   if use_default {
      def plan = _mpqs_default_work_plan(bits)
      fb = int(plan.get(0))
      win = int(plan.get(1))
      radius = int(plan.get(2))
      rels = int(plan.get(3))
   } else if bits <= 44 && factor_base_bound >= 337 && windows > 3 && window_radius >= 2400 {
      def compact_radius = 2400
      def compact_rels = 56
      if window_radius > compact_radius || max_relations > compact_rels {
         win = 3
         radius = min(window_radius, compact_radius)
         rels = min(max_relations, compact_rels)
         compacted = true
      }
   }
   _dict_with(18, [
         ["method", "mpqs-work-plan"], ["bits", bits], ["autotuned", use_default],
         ["factor_base_bound", fb], ["windows", win], ["window_radius", radius],
         ["oversized_plan_compacted", compacted],
         ["max_relations", rels], ["requested_factor_base_bound", factor_base_bound],
         ["requested_windows", windows], ["requested_window_radius", window_radius],
         ["requested_max_relations", max_relations],
         ["source_parameter_report", source_params],
         ["source_factor_base_size", source_params.get("factor_base_size", 0)],
         ["source_large_prime_multiplier", source_params.get("large_prime_multiplier", 0)],
         ["source_large_prime_max_estimate", source_params.get("large_prime_max_estimate", 0)],
         ["source_sieve_size", source_params.get("sieve_size", 0)],
      ])
}

fn _mpqs_used_attempt_fields(list fields, any used, any nz) list {
   fields = fields.append(["sieve_modulus", used.get("sieve_modulus", nz)])
   fields = _append_used_defaults(fields, used, [
         ["factor_base", []], ["factor_base_size", 0], ["factor_base_report", dict()],
         ["relations", []], ["relation_count", 0], ["candidate_count", 0],
         ["smooth_tests", 0], ["smooth_misses", 0], ["trial_division_prime_tests", 0],
         ["trial_divisions", 0], ["nonzero_exponent_terms", 0],
         ["sieve_prefilter_enabled", false], ["sieve_score_min", 0],
         ["sieve_marked_roots", 0], ["prefilter_tested", 0],
         ["prefilter_skipped", 0], ["fallback_tests", 0],
      ])
   fields = _append_used_defaults(fields, used, _mpqs_a_divisor_disabled_fields())
   fields = fields.append(["relation_collection_elapsed_ms", used.get("relation_collection_elapsed_ms", used.get("elapsed_ms", -1.0))])
   fields = fields.append(["smooth_hits", used.get("smooth_hits", used.get("relation_count", 0))])
   fields = _append_used_defaults(fields, used, _mpqs_byte_disabled_fields())
   fields = _append_used_defaults(fields, used, [
         ["window_reports", []],
         ["linear_algebra", dict()], ["dependency_solve", nil],
         ["dependency_count", 0], ["dependencies_tried", 0],
         ["factor", nil], ["success", false],
      ])
   fields = fields.append(["selected_relation_count", used.get("selected_relation_count", used.get("relation_count", 0))])
   fields = fields.append(["linear_algebra_relation_count", used.get("linear_algebra_relation_count", used.get("solved_relation_count", used.get("relation_count", 0)))])
   fields
}

fn _mpqs_finish_attempt_report(
   dict out, any t0, any nz, int selected_multiplier, dict attempts, list leading_fields
) dict {
   def primary = attempts.get("primary_attempt", nil)
   def fallback = attempts.get("fallback_attempt", nil)
   def used = attempts.get("used_attempt", primary)
   mut fields = [
      ["selected_multiplier", selected_multiplier],
      ["multiplier", used.get("multiplier", selected_multiplier)],
   ]
   fields = _fields_extend(fields, leading_fields)
   fields = _fields_extend(fields, [
         ["attempt_policy", attempts],
         ["primary_attempt", primary], ["fallback_attempt", fallback],
         ["unit_multiplier_preflight_attempt", attempts.get("unit_multiplier_preflight_attempt", nil)],
         ["unit_multiplier_preflight_used", attempts.get("unit_multiplier_preflight_used", false)],
         ["selected_multiplier_attempt", attempts.get("selected_multiplier_attempt", nil)],
         ["selected_multiplier_attempt_run", attempts.get("selected_multiplier_attempt_run", false)],
         ["used_fallback", attempts.get("used_fallback", false)],
      ])
   _finish_report_with(out, t0, _mpqs_used_attempt_fields(fields, used, nz))
}

fn mpqs_source_factor_report(any n, int max_factor_base_count=96, int max_prime_bound=1201, int max_windows=24, int max_window_radius=32000, int max_relations=256) dict {
   "Run MPQS using a bounded source-derived work plan."
   def t0 = ticks()
   def nz = _z(n)
   def plan = mpqs_source_work_plan_report(nz, max_factor_base_count, max_prime_bound, max_windows, max_window_radius, max_relations)
   def planned_factor_base_bound = int(plan.get("factor_base_bound", max_prime_bound))
   def planned_windows = int(plan.get("windows", max_windows))
   def planned_window_radius = int(plan.get("window_radius", max_window_radius))
   def planned_max_relations = int(plan.get("max_relations", max_relations))
   mut out = _set_fields(_report("multi-window-quadratic-sieve-source-plan", 30), [
         ["n", nz], ["source_driven", true],
         ["factor_base_bound", planned_factor_base_bound],
         ["windows", planned_windows], ["window_radius", planned_window_radius],
         ["max_relations", planned_max_relations], ["work_plan", plan],
         ["source_work_plan", plan],
         ["autotuned", true],
      ])
   if nz <= Z(1) { return _finish_factor_status(out, t0, nil, false) }
   if nz % Z(2) == Z(0) { return _finish_factor_status(out, t0, Z(2), true) }
   def selected_multiplier = int(plan.get("selected_multiplier", 1))
   def attempts = _mpqs_attempt_policy_report(nz, selected_multiplier, planned_factor_base_bound, planned_windows, planned_window_radius, planned_max_relations)
   _mpqs_finish_attempt_report(out, t0, nz, selected_multiplier, attempts, [])
}

fn mpqs_factor_report(any n, int factor_base_bound=64, int windows=4, int window_radius=256, int max_relations=32) dict {
   "Multi-window quadratic-sieve report with multiplier scoring, window collection, and GF(2) dependencies."
   def t0 = ticks()
   def nz = _z(n)
   def plan = mpqs_work_plan_report(nz, factor_base_bound, windows, window_radius, max_relations)
   def planned_factor_base_bound = int(plan.get("factor_base_bound", factor_base_bound))
   def planned_windows = int(plan.get("windows", windows))
   def planned_window_radius = int(plan.get("window_radius", window_radius))
   def planned_max_relations = int(plan.get("max_relations", max_relations))
   mut out = _set_fields(_report("multi-window-quadratic-sieve", 26), [
         ["n", nz], ["factor_base_bound", planned_factor_base_bound],
         ["windows", planned_windows], ["window_radius", planned_window_radius],
         ["max_relations", planned_max_relations], ["work_plan", plan],
         ["autotuned", plan.get("autotuned", false)],
      ])
   if nz <= Z(1) { return _finish_factor_status(out, t0, nil, false) }
   if nz % Z(2) == Z(0) { return _finish_factor_status(out, t0, Z(2), true) }
   def selected_multiplier = _qs_best_multiplier_fast(nz, planned_factor_base_bound)
   def mult_report = _qs_multiplier_summary_report(nz, planned_factor_base_bound, selected_multiplier)
   def attempts = _mpqs_attempt_policy_report(nz, selected_multiplier, planned_factor_base_bound, planned_windows, planned_window_radius, planned_max_relations)
   _mpqs_finish_attempt_report(out, t0, nz, selected_multiplier, attempts, [["multiplier_report", mult_report]])
}

fn mpqs_factor(any n, int factor_base_bound=64, int windows=4, int window_radius=256, int max_relations=32) any {
   "Return one factor from mpqs_factor_report, or nil."
   mpqs_factor_report(n, factor_base_bound, windows, window_radius, max_relations).get("factor", nil)
}

fn mpqs_source_factor(any n, int max_factor_base_count=96, int max_prime_bound=1201, int max_windows=24, int max_window_radius=32000, int max_relations=256) any {
   "Return one factor from mpqs_source_factor_report, or nil."
   mpqs_source_factor_report(n, max_factor_base_count, max_prime_bound, max_windows, max_window_radius, max_relations).get("factor", nil)
}
