;; Keywords: factorization nfs math crypto number-theory
;; Integer-factorization routines for number-field-sieve data preparation and support.
;; Reference:
;; - https://en.wikipedia.org/wiki/General_number_field_sieve
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.nfs(nfs_polynomial_report, snfs_shape_report, nfs_factor_base_report, nfs_trial_relation_report, nfs_line_relation_report, nfs_lattice_sieve_report, nfs_relation_filter_report, nfs_dependency_report, nfs_algebraic_product_report, nfs_qadic_prime_for_sqrt_report, nfs_qadic_initial_inverse_sqrt_report, nfs_qadic_newton_sqrt_report, nfs_qadic_sqrt_report, nfs_square_root_report, nfs_factor_report, nfs_factor)
use std.core
use std.math.nt
use std.math.crypto.factorization.classical as classical
use std.os.clock (ticks)

fn _nfs_elapsed_ms(any t0) number { float(ticks() - t0) / 1000000.0 }

fn _nfs_z(any x) bigint { is_bigint(x) ? x : Z(x) }

fn _nfs_abs(any x) bigint {
   def z = _nfs_z(x)
   z < Z(0) ? -z : z
}

fn _nfs_pow(any a, int e) bigint {
   mut out = Z(1)
   mut i = 0
   while i < e {
      out = out * _nfs_z(a)
      i += 1
   }
   out
}

fn _nfs_pow_mod(any a, int e, any m) bigint {
   def mz = _nfs_z(m)
   if mz == Z(1) { return Z(0) }
   mut out = Z(1)
   mut b = _nfs_z(a) % mz
   mut ee = e
   while ee > 0 {
      if (ee % 2) == 1 { out = (out * b) % mz }
      b = (b * b) % mz
      ee = ee / 2
   }
   out
}

fn _nfs_degree_for_bits(int bits) int {
   if bits < 80 { return 3 }
   if bits < 160 { return 4 }
   if bits < 320 { return 5 }
   6
}

fn _nfs_coeffs_base_m(any n, any m, int degree) list {
   mut rem = _nfs_z(n)
   def mz = _nfs_z(m)
   mut coeffs = []
   mut i = 0
   while i <= degree || rem > Z(0) {
      coeffs = coeffs.append(rem % mz)
      rem = rem / mz
      i += 1
   }
   coeffs
}

fn _nfs_poly_eval(list coeffs, any x) bigint {
   mut acc = Z(0)
   def xz = _nfs_z(x)
   mut i = coeffs.len - 1
   while i >= 0 {
      acc = acc * xz + _nfs_z(coeffs.get(i))
      i -= 1
   }
   acc
}

fn _nfs_poly_homogeneous_eval(list coeffs, any a, any b) bigint {
   def az = _nfs_z(a)
   def bz = _nfs_z(b)
   def degree = coeffs.len - 1
   if degree < 0 { return Z(0) }
   mut acc = _nfs_z(coeffs.get(degree))
   mut bpow = Z(1)
   mut i = degree - 1
   while i >= 0 {
      bpow = bpow * bz
      acc = acc * az + _nfs_z(coeffs.get(i)) * bpow
      i -= 1
   }
   acc
}

fn _nfs_prime_base(int bound) list {
   mut out = []
   mut p = Z(2)
   while p <= Z(bound) {
      out = out.append(p)
      p = next_prime(p)
   }
   out
}

fn _nfs_zero_counts(int width) list {
   mut out = []
   mut i = 0
   while i < width {
      out = out.append(0)
      i += 1
   }
   out
}

fn _nfs_factor_over_base(any v, list base) dict {
   mut rem = _nfs_abs(v)
   mut exps = []
   mut i = 0
   while i < base.len {
      def p = _nfs_z(base.get(i))
      mut e = 0
      while rem > Z(0) && rem % p == Z(0) {
         rem = rem / p
         e += 1
      }
      exps = exps.append(e)
      i += 1
   }
   {"smooth": rem == Z(1), "exponents": exps, "remaining": rem}
}

fn _nfs_parity_row(any value, list exps) list {
   mut row = [(_nfs_z(value) < Z(0)) ? 1 : 0]
   mut i = 0
   while i < exps.len {
      row = row.append(int(exps.get(i, 0)) % 2)
      i += 1
   }
   row
}

fn _nfs_relation_parity(any rat_value, list rat_exps, any alg_value, list alg_exps) list {
   def r = _nfs_parity_row(rat_value, rat_exps)
   def a = _nfs_parity_row(alg_value, alg_exps)
   mut out = []
   mut i = 0
   while i < r.len {
      out = out.append(r.get(i))
      i += 1
   }
   i = 0
   while i < a.len {
      out = out.append(a.get(i))
      i += 1
   }
   out
}

fn nfs_factor_base_report(int rational_bound=64, int algebraic_bound=64) dict {
   "Return rational and algebraic factor bases used by the small NFS relation pipeline."
   def t0 = ticks()
   def rb = _nfs_prime_base(rational_bound)
   def ab = _nfs_prime_base(algebraic_bound)
   {
      "method": "nfs-factor-base",
      "rational_bound": rational_bound, "algebraic_bound": algebraic_bound,
      "rational_base": rb, "algebraic_base": ab,
      "rational_base_size": rb.len, "algebraic_base_size": ab.len,
      "parity_width": rb.len + ab.len + 2, "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn nfs_polynomial_report(any n, int degree=0) dict {
   "Choose a base-m polynomial f such that f(m)=n and report its shape."
   def t0 = ticks()
   def nz = _nfs_z(n)
   def bits = bit_length(nz)
   def d = degree > 0 ? degree : _nfs_degree_for_bits(bits)
   mut m = nth_root(nz, d)
   if m < Z(2) { m = Z(2) }
   while _nfs_pow(m + Z(1), d) <= nz { m += Z(1) }
   while _nfs_pow(m, d) > nz && m > Z(2) { m -= Z(1) }
   def coeffs = _nfs_coeffs_base_m(nz, m, d)
   def value = _nfs_poly_eval(coeffs, m)
   {
      "method": "base-m-polynomial-selection", "n": nz, "bits": bits,
      "degree": d, "m": m, "coefficients": coeffs,
      "leading_coeff": coeffs.get(coeffs.len - 1, Z(0)),
      "constant_coeff": coeffs.get(0, Z(0)),
      "valid": value == nz, "f_of_m": value, "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn snfs_shape_report(any n, int max_degree=8, int max_base=100000) dict {
   "Search for a small special-form description n ~= a^k +/- c."
   def t0 = ticks()
   def nz = _nfs_z(n)
   mut best = nil
   mut best_score = nil
   mut k = 2
   while k <= max_degree {
      def a0 = nth_root(nz, k)
      mut candidates = [a0, a0 + Z(1)]
      mut i = 0
      while i < candidates.len {
         def a = candidates.get(i)
         if a > Z(1) && a <= Z(max_base) {
            def val = _nfs_pow(a, k)
            def diff = nz - val
            def score = _nfs_abs(diff)
            if best == nil || score < best_score {
               best = {
                  "base": a, "degree": k, "offset": diff,
                  "score": score, "form": diff >= Z(0) ? "a^k+c" : "a^k-c",
               }
               best_score = score
            }
         }
         i += 1
      }
      k += 1
   }
   {
      "method": "special-form-search", "n": nz,
      "max_degree": max_degree, "max_base": max_base,
      "found": best != nil, "best": best, "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn _nfs_line_relation(any a, any b, any m, list coeffs, list rbase, list abase) any {
   def az = _nfs_z(a)
   def bz = _nfs_z(b)
   def rat = az - bz * _nfs_z(m)
   if rat == Z(0) { return nil }
   def alg = _nfs_poly_homogeneous_eval(coeffs, az, bz)
   if alg == Z(0) { return nil }
   def rf = _nfs_factor_over_base(rat, rbase)
   if !rf.get("smooth", false) { return nil }
   def af = _nfs_factor_over_base(alg, abase)
   if !af.get("smooth", false) { return nil }
   def rexps = rf.get("exponents", [])
   def aexps = af.get("exponents", [])
   {
      "a": az, "b": bz, "rational_value": rat, "algebraic_value": alg,
      "rational_exponents": rexps, "algebraic_exponents": aexps,
      "parity": _nfs_relation_parity(rat, rexps, alg, aexps), "smooth": true,
   }
}

fn nfs_trial_relation_report(any n, int factor_base_bound=64, int sieve_radius=32, int degree=0) dict {
   "Collect trial algebraic-side smooth values for the selected NFS polynomial."
   def t0 = ticks()
   def poly = nfs_polynomial_report(n, degree)
   def coeffs = poly.get("coefficients", [])
   def base = _nfs_prime_base(factor_base_bound)
   mut relations = []
   mut x = 0 - sieve_radius
   while x <= sieve_radius {
      def val = _nfs_poly_eval(coeffs, Z(x))
      def av = _nfs_abs(val)
      if av > Z(1) && is_smooth(av, factor_base_bound) {
         relations = relations.append({"x": x, "value": val, "abs_value": av, "smooth": true})
      }
      x += 1
   }
   {
      "method": "trial-nfs-relation-collector", "n": _nfs_z(n),
      "polynomial": poly, "factor_base_bound": factor_base_bound,
      "factor_base_size": base.len, "sieve_radius": sieve_radius,
      "relations": relations, "relation_count": relations.len,
      "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn nfs_line_relation_report(any n, int rational_bound=64, int algebraic_bound=64, int sieve_radius=32, int degree=0, int b_bound=1) dict {
   "Collect small NFS line-sieve relations with rational and algebraic smoothness."
   def t0 = ticks()
   def poly = nfs_polynomial_report(n, degree)
   def coeffs = poly.get("coefficients", [])
   def m = poly.get("m", Z(0))
   def fb = nfs_factor_base_report(rational_bound, algebraic_bound)
   def rbase = fb.get("rational_base", [])
   def abase = fb.get("algebraic_base", [])
   mut relations = []
   mut b = 1
   while b <= max(1, b_bound) {
      mut a = 0 - sieve_radius
      while a <= sieve_radius {
         if gcd(_nfs_abs(Z(a)), Z(b)) == Z(1) {
            def rel = _nfs_line_relation(Z(a), Z(b), m, coeffs, rbase, abase)
            if rel != nil { relations = relations.append(rel) }
         }
         a += 1
      }
      b += 1
   }
   {
      "method": "nfs-line-relation-collector", "n": _nfs_z(n),
      "polynomial": poly, "factor_base": fb,
      "rational_bound": rational_bound, "algebraic_bound": algebraic_bound,
      "sieve_radius": sieve_radius, "b_bound": max(1, b_bound),
      "relations": relations, "relation_count": relations.len,
      "parity_width": fb.get("parity_width", 0), "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn nfs_lattice_sieve_report(any n, int rational_bound=64, int algebraic_bound=64, int a_radius=128, int b_start=1, int b_count=16, int degree=0, int target_relations=0, int segment_size=4) dict {
   "Collect segmented NFS lattice-sieve relations with reusable polynomial and factor bases."
   def t0 = ticks()
   def poly = nfs_polynomial_report(n, degree)
   def coeffs = poly.get("coefficients", [])
   def m = poly.get("m", Z(0))
   def fb = nfs_factor_base_report(rational_bound, algebraic_bound)
   def rbase = fb.get("rational_base", [])
   def abase = fb.get("algebraic_base", [])
   def first_b = max(1, b_start)
   def total_b = max(1, b_count)
   def seg = max(1, segment_size)
   def stop_b = first_b + total_b
   mut relations = []
   mut segment_reports = []
   mut candidates = 0
   mut coprime_pairs = 0
   mut rational_smooth = 0
   mut algebraic_smooth = 0
   mut b0 = first_b
   mut stopped_early = false
   while b0 < stop_b && !stopped_early {
      def b1 = min(stop_b, b0 + seg)
      mut seg_candidates = 0
      mut seg_coprime = 0
      mut seg_rational_smooth = 0
      mut seg_algebraic_smooth = 0
      mut seg_relations = 0
      mut b = b0
      while b < b1 && !stopped_early {
         mut a = 0 - a_radius
         while a <= a_radius && !stopped_early {
            candidates += 1
            seg_candidates += 1
            if gcd(_nfs_abs(Z(a)), Z(b)) == Z(1) {
               coprime_pairs += 1
               seg_coprime += 1
               def az = Z(a)
               def bz = Z(b)
               def rat = az - bz * _nfs_z(m)
               def alg = _nfs_poly_homogeneous_eval(coeffs, az, bz)
               if rat != Z(0) && alg != Z(0) {
                  def rf = _nfs_factor_over_base(rat, rbase)
                  if rf.get("smooth", false) {
                     rational_smooth += 1
                     seg_rational_smooth += 1
                     def af = _nfs_factor_over_base(alg, abase)
                     if af.get("smooth", false) {
                        algebraic_smooth += 1
                        seg_algebraic_smooth += 1
                        def rexps = rf.get("exponents", [])
                        def aexps = af.get("exponents", [])
                        relations = relations.append({
                              "a": az, "b": bz, "rational_value": rat, "algebraic_value": alg,
                              "rational_exponents": rexps, "algebraic_exponents": aexps,
                              "parity": _nfs_relation_parity(rat, rexps, alg, aexps), "smooth": true,
                              "segment_start_b": b0, "segment_end_b": b1 - 1,
                        })
                        seg_relations += 1
                        if target_relations > 0 && relations.len >= target_relations { stopped_early = true }
                     }
                  }
               }
            }
            a += 1
         }
         b += 1
      }
      segment_reports = segment_reports.append({
            "b_start": b0, "b_end": b1 - 1,
            "candidates": seg_candidates, "coprime_pairs": seg_coprime,
            "rational_smooth": seg_rational_smooth, "algebraic_smooth": seg_algebraic_smooth,
            "relations": seg_relations,
      })
      b0 = b1
   }
   {
      "method": "nfs-lattice-sieve-relation-collector", "source_model": "GNFS line-sieving relation collector",
      "sieve_model": "segmented-line-lattice-sieve", "n": _nfs_z(n),
      "polynomial": poly, "factor_base": fb,
      "rational_bound": rational_bound, "algebraic_bound": algebraic_bound,
      "a_radius": a_radius, "b_start": first_b, "b_count": total_b,
      "segment_size": seg, "segment_count": segment_reports.len,
      "segments": segment_reports, "target_relations": target_relations,
      "stopped_early": stopped_early, "candidate_pairs": candidates,
      "coprime_pairs": coprime_pairs, "rational_smooth": rational_smooth,
      "algebraic_smooth": algebraic_smooth, "relations": relations,
      "relation_count": relations.len, "parity_width": fb.get("parity_width", 0),
      "large_lattice_sieving": a_radius * total_b >= 512,
      "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn _nfs_singleton_prune(list unique, int w, bool prune_singletons) dict {
   mut kept = unique
   mut singleton_rounds = 0
   mut singleton_dropped = 0
   mut changed = prune_singletons
   while changed {
      changed = false
      mut counts = _nfs_zero_counts(w)
      mut i = 0
      while i < kept.len {
         def parity = kept.get(i).get("parity", [])
         mut j = 0
         while j < w {
            if (int(parity.get(j, 0)) & 1) != 0 { counts[j] = counts.get(j, 0) + 1 }
            j += 1
         }
         i += 1
      }
      mut next = []
      i = 0
      while i < kept.len {
         def rel = kept.get(i)
         def parity = rel.get("parity", [])
         mut drop = false
         mut j = 0
         while j < w && !drop {
            if (int(parity.get(j, 0)) & 1) != 0 && counts.get(j, 0) <= 1 { drop = true }
            j += 1
         }
         if drop {
            singleton_dropped += 1
            changed = true
         } else {
            next = next.append(rel)
         }
         i += 1
      }
      if changed {
         kept = next
         singleton_rounds += 1
      }
   }
   {"relations": kept, "rounds": singleton_rounds, "dropped": singleton_dropped}
}

fn nfs_relation_filter_report(list relations, int width=0, bool prune_singletons=true) dict {
   "Filter NFS relations with duplicate(a,b) removal and singleton-column pruning."
   def t0 = ticks()
   mut w = width
   if w <= 0 {
      mut i = 0
      while i < relations.len {
         def p = relations.get(i).get("parity", [])
         if p.len > w { w = p.len }
         i += 1
      }
   }
   mut seen = dict()
   mut unique = []
   mut duplicate_ab = 0
   mut zero_parity = 0
   mut i = 0
   while i < relations.len {
      def rel = relations.get(i)
      def key = to_str(rel.get("a", "")) + "," + to_str(rel.get("b", ""))
      def parity = rel.get("parity", [])
      mut wt = 0
      mut j = 0
      while j < parity.len {
         if (int(parity.get(j, 0)) & 1) != 0 { wt += 1 }
         j += 1
      }
      if wt == 0 { zero_parity += 1 }
      if seen.contains(key) {
         duplicate_ab += 1
      } else {
         seen = seen.set(key, true)
         unique = unique.append(rel)
      }
      i += 1
   }
   def pruned = _nfs_singleton_prune(unique, w, prune_singletons)
   def kept = pruned.get("relations", [])
   {
      "method": "nfs-relation-filter", "input_relations": relations.len,
      "unique_relations": unique.len, "filtered_relations": kept,
      "filtered_count": kept.len, "width": w, "duplicate_ab": duplicate_ab,
      "zero_parity": zero_parity, "singleton_prune": prune_singletons,
      "singleton_rounds": pruned.get("rounds", 0), "singleton_dropped": pruned.get("dropped", 0),
      "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn _nfs_dependency_matrix(list relations, int width) list {
   mut rows = []
   mut j = 0
   while j < width {
      mut row = []
      mut i = 0
      while i < relations.len {
         row = row.append(int(relations.get(i).get("parity", []).get(j, 0)) & 1)
         i += 1
      }
      rows = rows.append(row)
      j += 1
   }
   rows
}

fn _nfs_zero_vec(int width) list {
   mut out = []
   mut i = 0
   while i < width {
      out = out.append(0)
      i += 1
   }
   out
}

fn _nfs_xor_vec(list a, list b) list {
   mut out = []
   mut i = 0
   def n = max(a.len, b.len)
   while i < n {
      out = out.append((int(a.get(i, 0)) ^^ int(b.get(i, 0))) & 1)
      i += 1
   }
   out
}

fn _nfs_dependency_candidates(list basis, int width, int max_count=4096) list {
   if basis.len == 0 { return [] }
   if basis.len > 14 { return basis }
   mut out = []
   def limit = 1 << basis.len
   mut mask = 1
   while mask < limit && out.len < max_count {
      mut dep = _nfs_zero_vec(width)
      mut i = 0
      while i < basis.len {
         if ((mask >> i) & 1) == 1 { dep = _nfs_xor_vec(dep, basis.get(i)) }
         i += 1
      }
      out = out.append(dep)
      mask += 1
   }
   out
}

fn _nfs_selected_relations(list relations, list dependency) list {
   mut out = []
   mut i = 0
   while i < dependency.len && i < relations.len {
      if (int(dependency.get(i, 0)) & 1) != 0 { out = out.append(relations.get(i)) }
      i += 1
   }
   out
}

fn _nfs_sum_exponents(list relations, str key, int width) list {
   mut out = _nfs_zero_counts(width)
   mut i = 0
   while i < relations.len {
      def exps = relations.get(i).get(key, [])
      mut j = 0
      while j < width {
         out[j] = int(out.get(j, 0)) + int(exps.get(j, 0))
         j += 1
      }
      i += 1
   }
   out
}

fn _nfs_negative_count(list relations, str key) int {
   mut out = 0
   mut i = 0
   while i < relations.len {
      if _nfs_z(relations.get(i).get(key, Z(1))) < Z(0) { out += 1 }
      i += 1
   }
   out
}

fn _nfs_exponents_even(list exps) bool {
   mut i = 0
   while i < exps.len {
      if (int(exps.get(i, 0)) % 2) != 0 { return false }
      i += 1
   }
   true
}

fn _nfs_sqrt_from_exponents_mod(list base, list exps, any modulus) bigint {
   def mz = _nfs_z(modulus)
   mut out = Z(1)
   mut i = 0
   while i < base.len && i < exps.len {
      def e = int(exps.get(i, 0)) / 2
      if e > 0 { out = (out * _nfs_pow_mod(base.get(i), e, mz)) % mz }
      i += 1
   }
   out
}

fn _nfs_zero_poly(int n) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(Z(0))
      i += 1
   }
   out
}

fn _nfs_poly_mul_mod_monic(list p, list q, list mod_coeffs) list {
   def d = mod_coeffs.len - 1
   if d <= 0 { return [Z(0)] }
   mut prod = _nfs_zero_poly(p.len + q.len - 1)
   mut i = 0
   while i < p.len {
      mut j = 0
      while j < q.len {
         prod[i + j] = _nfs_z(prod.get(i + j)) + _nfs_z(p.get(i)) * _nfs_z(q.get(j))
         j += 1
      }
      i += 1
   }
   mut k = prod.len - 1
   while k >= d {
      def lead = _nfs_z(prod.get(k))
      if lead != Z(0) {
         mut j = 0
         while j < d {
            prod[k - d + j] = _nfs_z(prod.get(k - d + j)) - lead * _nfs_z(mod_coeffs.get(j))
            j += 1
         }
      }
      k -= 1
   }
   mut out = []
   i = 0
   while i < d {
      out = out.append(_nfs_z(prod.get(i, Z(0))))
      i += 1
   }
   out
}

fn _nfs_poly_eval_mod(list coeffs, any x, any modulus) bigint {
   def mz = _nfs_z(modulus)
   mut acc = Z(0)
   mut i = coeffs.len - 1
   while i >= 0 {
      acc = (acc * _nfs_z(x) + _nfs_z(coeffs.get(i))) % mz
      i -= 1
   }
   acc
}

fn _nfs_coeff_mod(any x, any modulus) bigint {
   def mz = _nfs_z(modulus)
   mut r = _nfs_z(x) % mz
   if r < Z(0) { r += mz }
   r
}

fn _nfs_poly_coeff_mod(list coeffs, any modulus) list {
   mut out = []
   mut i = 0
   while i < coeffs.len {
      out = out.append(_nfs_coeff_mod(coeffs.get(i), modulus))
      i += 1
   }
   out
}

fn _nfs_poly_mul_mod_monic_q(list p, list q, list mod_coeffs, any modulus) list {
   _nfs_poly_coeff_mod(_nfs_poly_mul_mod_monic(p, q, mod_coeffs), modulus)
}

fn _nfs_poly_centered_mod(list coeffs, any modulus) list {
   def mz = _nfs_z(modulus)
   def half = mz / Z(2)
   mut out = []
   mut i = 0
   while i < coeffs.len {
      mut c = _nfs_coeff_mod(coeffs.get(i), mz)
      if c > half { c -= mz }
      out = out.append(c)
      i += 1
   }
   out
}

fn _nfs_poly_total_bits(list coeffs) int {
   mut total = 0
   mut i = 0
   while i < coeffs.len {
      total += bit_length(_nfs_abs(coeffs.get(i)))
      i += 1
   }
   total
}

fn _nfs_poly_one(int degree) list {
   mut out = []
   mut i = 0
   while i < degree {
      out = out.append(i == 0 ? Z(1) : Z(0))
      i += 1
   }
   out
}

fn _nfs_poly_candidate_from_index(int index, int degree, int q) list {
   mut out = []
   mut x = index
   mut i = 0
   while i < degree {
      out = out.append(Z(x % q))
      x = x / q
      i += 1
   }
   out
}

fn nfs_qadic_initial_inverse_sqrt_report(list product_poly, list monic_poly, any q, int max_candidates=100000) dict {
   "Find an initial reciprocal square root modulo q for the GNFS q-adic lift."
   def t0 = ticks()
   def qz = _nfs_z(q)
   def qi = int(qz)
   def degree = monic_poly.len - 1
   def monic = degree > 0 && _nfs_z(monic_poly.get(degree, Z(0))) == Z(1)
   def target = _nfs_poly_one(degree)
   mut checked = 0
   mut found = nil
   mut idx = 0
   while monic && qi > 1 && idx < max_candidates && found == nil {
      def cand = _nfs_poly_candidate_from_index(idx, degree, qi)
      def check = _nfs_poly_mul_mod_monic_q(_nfs_poly_mul_mod_monic_q(_nfs_poly_coeff_mod(product_poly, qz), cand, monic_poly, qz), cand, monic_poly, qz)
      checked += 1
      if check == target { found = cand }
      idx += 1
   }
   {
      "method": "nfs-qadic-initial-inverse-sqrt-report",
      "source_model": "GNFS initial inverse square-root step",
      "q": qz, "degree": degree, "monic": monic,
      "checked_candidates": checked, "max_candidates": max_candidates,
      "found": found != nil, "inverse_sqrt_mod_q": found,
      "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn _nfs_poly_has_root_mod_q(list poly, any q) bool {
   def qi = int(_nfs_z(q))
   mut x = 0
   while x < qi {
      if _nfs_poly_eval_mod(poly, Z(x), Z(qi)) == Z(0) { return true }
      x += 1
   }
   false
}

fn nfs_qadic_prime_for_sqrt_report(list monic_poly, int min_q=3, int max_tries=64) dict {
   "Select a small prime q whose reduction is root-free for the GNFS q-adic square-root lift."
   def t0 = ticks()
   def degree = monic_poly.len - 1
   def monic = degree > 0 && _nfs_z(monic_poly.get(degree, Z(0))) == Z(1)
   mut attempts = []
   mut p = next_prime(max(2, min_q - 1))
   mut tries = 0
   mut selected = nil
   while monic && tries < max_tries && selected == nil {
      def has_root = _nfs_poly_has_root_mod_q(monic_poly, p)
      attempts = attempts.append({"q": p, "root_free": !has_root})
      if !has_root { selected = p }
      p = next_prime(p)
      tries += 1
   }
   {
      "method": "nfs-qadic-prime-for-sqrt-report",
      "source_model": "GNFS square-root prime selection",
      "degree": degree, "monic": monic, "min_q": min_q,
      "tries": tries, "found": selected != nil, "q": selected,
      "irreducibility_model": degree <= 3 ? "root-free-exact-for-degree-2-3" : "root-free-squarefree-heuristic",
      "attempts": attempts, "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn nfs_qadic_newton_sqrt_report(list product_poly, list monic_poly, list inverse_sqrt_mod_q, any q, int iterations=4) dict {
   "Lift a reciprocal square root with the q-adic Newton step used by GNFS sqrt_a."
   def t0 = ticks()
   def q0 = _nfs_z(q)
   def iters = max(0, iterations)
   def degree = monic_poly.len - 1
   def monic = degree > 0 && _nfs_z(monic_poly.get(degree, Z(0))) == Z(1)
   mut modulus = q0
   mut R = _nfs_poly_coeff_mod(inverse_sqrt_mod_q, modulus)
   mut trace = []
   mut i = 0
   while i < iters && monic {
      modulus = modulus * modulus
      def prod_mod = _nfs_poly_coeff_mod(product_poly, modulus)
      mut tmp = _nfs_poly_mul_mod_monic_q(prod_mod, R, monic_poly, modulus)
      tmp = _nfs_poly_mul_mod_monic_q(tmp, R, monic_poly, modulus)
      mut corr = []
      mut j = 0
      while j < degree {
         mut c = (j == 0 ? Z(3) : Z(0)) - _nfs_z(tmp.get(j, Z(0)))
         c = _nfs_coeff_mod(c, modulus)
         if (c % Z(2)) != Z(0) { c += modulus }
         corr = corr.append(_nfs_coeff_mod(c / Z(2), modulus))
         j += 1
      }
      R = _nfs_poly_mul_mod_monic_q(R, corr, monic_poly, modulus)
      def check = _nfs_poly_mul_mod_monic_q(_nfs_poly_mul_mod_monic_q(prod_mod, R, monic_poly, modulus), R, monic_poly, modulus)
      trace = trace.append({"iteration": i + 1, "modulus": modulus, "check": check, "check_constant": check.get(0, Z(0))})
      i += 1
   }
   mut sqrt_poly = []
   mut verified = false
   if monic {
      sqrt_poly = _nfs_poly_centered_mod(_nfs_poly_mul_mod_monic_q(R, _nfs_poly_coeff_mod(product_poly, modulus), monic_poly, modulus), modulus)
      def square = _nfs_poly_mul_mod_monic(sqrt_poly, sqrt_poly, monic_poly)
      def target = _nfs_poly_centered_mod(product_poly, modulus)
      verified = _nfs_poly_coeff_mod(square, modulus) == _nfs_poly_coeff_mod(target, modulus)
   }
   {
      "method": "nfs-qadic-newton-sqrt-report",
      "source_model": "GNFS final square-root step",
      "q": q0, "iterations": iters, "final_modulus": modulus,
      "degree": degree, "monic": monic, "reciprocal_sqrt": R,
      "sqrt_polynomial": sqrt_poly, "verified_square_mod_final_modulus": verified,
      "sqrt_total_bits": _nfs_poly_total_bits(sqrt_poly),
      "trace": trace, "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn nfs_qadic_sqrt_report(list product_poly, list monic_poly, int min_q=3, int iterations=4, int max_initial_candidates=100000) dict {
   "Select q, find the initial reciprocal square root, and run the q-adic GNFS sqrt lift."
   def t0 = ticks()
   def prime = nfs_qadic_prime_for_sqrt_report(monic_poly, min_q)
   mut out = {
      "method": "nfs-qadic-sqrt-report",
      "source_model": "GNFS combined square-root pipeline",
      "prime_report": prime, "success": false,
   }
   if !prime.get("found", false) {
      return out.merge({"status": "no-sqrt-prime", "elapsed_ms": _nfs_elapsed_ms(t0)})
   }
   def q = prime.get("q")
   def init = nfs_qadic_initial_inverse_sqrt_report(product_poly, monic_poly, q, max_initial_candidates)
   out = out.set("initial_inverse_sqrt_report", init)
   if !init.get("found", false) {
      return out.merge({"status": "no-initial-inverse-sqrt", "elapsed_ms": _nfs_elapsed_ms(t0)})
   }
   def lift = nfs_qadic_newton_sqrt_report(product_poly, monic_poly, init.get("inverse_sqrt_mod_q"), q, iterations)
   out.merge({
         "newton_report": lift, "sqrt_polynomial": lift.get("sqrt_polynomial", []),
         "success": lift.get("verified_square_mod_final_modulus", false),
         "status": lift.get("verified_square_mod_final_modulus", false) ? "verified-square" : "unverified-square",
         "elapsed_ms": _nfs_elapsed_ms(t0),
   })
}

fn nfs_algebraic_product_report(any n, list relations, any polynomial=nil, list dependency=[], bool include_qadic=true) dict {
   "Multiply selected relation polynomials modulo the monic algebraic polynomial."
   def t0 = ticks()
   def nz = _nfs_z(n)
   def poly = polynomial == nil ? nfs_polynomial_report(nz, 0) : polynomial
   def coeffs = poly.get("coefficients", [])
   def m = poly.get("m", Z(0))
   def degree = coeffs.len - 1
   def monic = degree > 0 && _nfs_z(coeffs.get(degree, Z(0))) == Z(1)
   def selected = dependency.len > 0 ? _nfs_selected_relations(relations, dependency) : relations
   mut prod = [Z(1)]
   mut i = 0
   while i < selected.len && monic {
      def r = selected.get(i)
      def rel_poly = [_nfs_z(r.get("a", Z(0))), Z(0) - _nfs_z(r.get("b", Z(1)))]
      prod = _nfs_poly_mul_mod_monic(prod, rel_poly, coeffs)
      i += 1
   }
   def eval_mod_n = monic ? _nfs_poly_eval_mod(prod, m, nz) : nil
   def qadic = monic && include_qadic ? nfs_qadic_sqrt_report(prod, coeffs, 3, 2, 4096) : nil
   {
      "method": "nfs-algebraic-product-report",
      "source_model": "GNFS direct relation product",
      "n": nz, "relation_count": relations.len, "selected_count": selected.len,
      "degree": degree, "monic": monic, "polynomial": poly,
      "product_polynomial_mod_f": monic ? prod : [],
      "product_eval_mod_n": eval_mod_n,
      "qadic_sqrt_report": qadic,
      "qadic_square_root_attempted": qadic != nil,
      "qadic_square_root_success": qadic != nil && qadic.get("success", false),
      "q_adic_newton_sqrt_pending": false,
      "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn _nfs_try_square_root_candidate(any n, list relations, list dependency, list rbase, list abase) dict {
   def nz = _nfs_z(n)
   def selected = _nfs_selected_relations(relations, dependency)
   def rexps = _nfs_sum_exponents(selected, "rational_exponents", rbase.len)
   def aexps = _nfs_sum_exponents(selected, "algebraic_exponents", abase.len)
   def rneg = _nfs_negative_count(selected, "rational_value")
   def aneg = _nfs_negative_count(selected, "algebraic_value")
   def even = selected.len > 0 && (rneg % 2) == 0 && (aneg % 2) == 0 && _nfs_exponents_even(rexps) && _nfs_exponents_even(aexps)
   mut out = {
      "selected_count": selected.len, "dependency_weight": selected.len,
      "rational_negative_count": rneg, "algebraic_negative_count": aneg,
      "rational_exponents": rexps, "algebraic_exponents": aexps,
      "even_exponents": even, "factor": nil, "success": false,
   }
   if !even { return out }
   def rroot = _nfs_sqrt_from_exponents_mod(rbase, rexps, nz)
   def aroot = _nfs_sqrt_from_exponents_mod(abase, aexps, nz)
   def g1 = gcd((rroot - aroot) % nz, nz)
   def g2 = gcd((rroot + aroot) % nz, nz)
   mut factor = nil
   if g1 > Z(1) && g1 < nz { factor = g1 }
   if factor == nil && g2 > Z(1) && g2 < nz { factor = g2 }
   out.merge({
         "rational_sqrt_mod_n": rroot, "algebraic_norm_sqrt_mod_n": aroot,
         "gcd_minus": g1, "gcd_plus": g2, "factor": factor, "success": factor != nil,
   })
}

fn nfs_dependency_report(any n, int rational_bound=64, int algebraic_bound=64, int sieve_radius=32, int degree=0, int b_bound=1) dict {
   "Collect, filter, and run GF(2) dependency analysis on small NFS relations."
   def t0 = ticks()
   def rels = b_bound > 1 ? nfs_lattice_sieve_report(n, rational_bound, algebraic_bound, sieve_radius, 1, b_bound, degree, 0, min(8, max(1, b_bound))) : nfs_line_relation_report(n, rational_bound, algebraic_bound, sieve_radius, degree, b_bound)
   def width = rels.get("parity_width", 0)
   mut filter = nfs_relation_filter_report(rels.get("relations", []), width, true)
   mut used_unpruned_fallback = false
   if filter.get("filtered_count", 0) == 0 && rels.get("relation_count", 0) > 0 {
      filter = nfs_relation_filter_report(rels.get("relations", []), width, false)
      used_unpruned_fallback = true
   }
   def frels = filter.get("filtered_relations", [])
   def matrix = _nfs_dependency_matrix(frels, width)
   def la = classical.block_lanczos_gf2_report(matrix, frels.len, 8, 0, true)
   {
      "method": "nfs-dependency-report", "n": _nfs_z(n),
      "relation_report": rels, "relation_filter": filter,
      "used_unpruned_fallback": used_unpruned_fallback,
      "relations": frels, "relation_count": frels.len, "parity_width": width,
      "linear_algebra": la, "dependency_count": la.get("verified_count", 0),
      "ready_for_square_root": la.get("verified_count", 0) > 0,
      "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn nfs_square_root_report(any n, int rational_bound=64, int algebraic_bound=64, int sieve_radius=32, int degree=0, int max_dependencies=4096, int b_bound=1) dict {
   "Try square-root/GCD extraction from verified NFS dependencies."
   def t0 = ticks()
   def nz = _nfs_z(n)
   def dep = nfs_dependency_report(nz, rational_bound, algebraic_bound, sieve_radius, degree, b_bound)
   def rels = dep.get("relations", [])
   def fb = dep.get("relation_report").get("factor_base")
   def rbase = fb.get("rational_base", [])
   def abase = fb.get("algebraic_base", [])
   def basis = dep.get("linear_algebra").get("basis", [])
   def candidates = _nfs_dependency_candidates(basis, rels.len, max_dependencies)
   mut attempts = []
   mut factor = nil
   mut qadic_success_count = 0
   mut i = 0
   while i < candidates.len && factor == nil {
      def a = _nfs_try_square_root_candidate(nz, rels, candidates.get(i), rbase, abase)
      def prod = nfs_algebraic_product_report(nz, rels, dep.get("relation_report").get("polynomial"), candidates.get(i))
      mut attempt = a.set("algebraic_product_report", prod)
      if prod.get("monic", false) {
         def qadic0 = prod.get("qadic_sqrt_report", nil)
         def qadic = qadic0 == nil ? nfs_qadic_sqrt_report(prod.get("product_polynomial_mod_f", []), prod.get("polynomial").get("coefficients", []), 3, 2, 4096) : qadic0
         attempt = attempt.set("qadic_sqrt_report", qadic)
         if qadic.get("success", false) {
            qadic_success_count += 1
            def qroot = _nfs_poly_eval_mod(qadic.get("sqrt_polynomial", []), prod.get("polynomial").get("m", Z(0)), nz)
            def rroot = attempt.get("rational_sqrt_mod_n", nil)
            if rroot != nil {
               def g1q = gcd((rroot - qroot) % nz, nz)
               def g2q = gcd((rroot + qroot) % nz, nz)
               mut qfactor = nil
               if g1q > Z(1) && g1q < nz { qfactor = g1q }
               if qfactor == nil && g2q > Z(1) && g2q < nz { qfactor = g2q }
               attempt = attempt.merge({
                     "algebraic_qadic_sqrt_mod_n": qroot,
                     "qadic_gcd_minus": g1q, "qadic_gcd_plus": g2q,
                     "qadic_factor": qfactor,
               })
               if factor == nil && qfactor != nil { factor = qfactor }
            }
         }
      }
      attempts = attempts.append(attempt)
      if factor == nil && a.get("success", false) { factor = a.get("factor") }
      i += 1
   }
   {
      "method": "nfs-square-root-report", "n": nz,
      "dependency_report": dep, "candidate_count": candidates.len,
      "attempts": attempts, "attempt_count": attempts.len,
      "factor": factor, "success": factor != nil,
      "algebraic_square_root_model": "norm-product-trial",
      "qadic_number_field_square_root_attempted": qadic_success_count > 0,
      "qadic_square_root_success_count": qadic_success_count,
      "full_number_field_square_root": qadic_success_count > 0,
      "elapsed_ms": _nfs_elapsed_ms(t0),
   }
}

fn nfs_factor_report(any n, int rational_bound=64, int algebraic_bound=64, int sieve_radius=32, int degree=0, int b_bound=1) dict {
   "Run the small NFS relation, dependency, square-root, and GCD pipeline."
   def r = nfs_square_root_report(n, rational_bound, algebraic_bound, sieve_radius, degree, 4096, b_bound)
   r.set("method", "nfs-factor-report")
}

fn nfs_factor(any n, int rational_bound=64, int algebraic_bound=64, int sieve_radius=32, int degree=0, int b_bound=1) any {
   "Return one factor from nfs_factor_report, or nil."
   nfs_factor_report(n, rational_bound, algebraic_bound, sieve_radius, degree, b_bound).get("factor", nil)
}
