;; Keywords: factorization xor-factor math crypto number-theory
;; Integer-factorization routines for factorization with XOR constraints.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap8.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.xor_factor(xor_check, xor_factor_pairs, xor_factor_pairs_report, xor_factor_pairs_with_target, xor_factor_with_target_report, xor_factor_from_target)
use std.core
use std.math.nt
use std.math.crypto.factorization.known_phi
use std.os.clock (ticks)

fn xor_check(any a, any b) any {
   "Compute the bitwise XOR of a and b."
   a ^^ b
}

fn _xorfac_z(any x) any { is_bigint(x) ? x : Z(x) }

fn _xorfac_pair(any a, any b) list {
   def az, bz = _xorfac_z(a), _xorfac_z(b)
   def p = az <= bz ? az : bz
   def q = az <= bz ? bz : az
   [p, q, p ^^ q]
}

fn xor_factor_pairs_report(any n, int max_divisor_scan=0) dict {
   "Scan factor pairs of n and report [p, q, p^q] entries plus audit counters.
   max_divisor_scan=0 means scan to sqrt(n)."
   def t0 = ticks()
   def nz = _xorfac_z(n)
   mut out = dict(14)
   out = out.set("method", "xor-factor-divisor-scan")
   out = out.set("n", nz)
   out = out.set("max_divisor_scan", max_divisor_scan)
   if(nz < Z(1)){
      return out.set("pairs", []).set("pair_count", 0).set("success", false).set("elapsed_ms", float(ticks() - t0) / 1000000.0)
   }
   mut end = isqrt(nz)
   mut hit_limit = false
   if(max_divisor_scan > 0 && Z(max_divisor_scan) < end){
      end = Z(max_divisor_scan)
      hit_limit = true
   }
   mut pairs = list(0)
   mut tested = 0
   mut a = Z(1)
   while(a <= end){
      tested += 1
      if(nz % a == Z(0)){
         pairs = pairs.append(_xorfac_pair(a, nz / a))
      }
      a = a + Z(1)
   }
   out = out.set("pairs", pairs)
   out = out.set("pair_count", pairs.len)
   out = out.set("divisors_tested", tested)
   out = out.set("hit_limit", hit_limit)
   out = out.set("success", pairs.len > 0 && !hit_limit)
   out = out.set("elapsed_ms", float(ticks() - t0) / 1000000.0)
   out
}

fn xor_factor_pairs(any n) list {
   "Find all factor pairs(p, q) of n and return [p, q, p^q] entries."
   xor_factor_pairs_report(n).get("pairs", [])
}

fn xor_factor_with_target_report(any n, any target, int max_shared_masks=0) dict {
   "Recover factor pairs of n with p^q == target using the shared-bit sum identity.
   If x = p^q and c = p&q, then p+q = x + 2c and c&x == 0.
   For each compatible shared-bit candidate c, solve t^2 - (x+2c)t + n = 0."
   def t0 = ticks()
   def nz = _xorfac_z(n)
   def xz = _xorfac_z(target)
   mut out = dict(18)
   out = out.set("method", "xor-target-shared-bit-sum")
   out = out.set("n", nz)
   out = out.set("target_xor", xz)
   out = out.set("max_shared_masks", max_shared_masks)
   if(nz < Z(1) || xz < Z(0)){
      return out.set("pairs", []).set("pair_count", 0).set("success", false).set("elapsed_ms", float(ticks() - t0) / 1000000.0)
   }
   def max_shared = isqrt(nz)
   mut pairs = []
   mut tried = 0
   mut skipped_overlap = 0
   mut discriminants_tested = 0
   mut shared = Z(0)
   mut hit_limit = false
   while(shared <= max_shared && !hit_limit){
      if((shared & xz) == Z(0)){
         tried += 1
         def sum_pq = xz + Z(2) * shared
         if(sum_pq > Z(0)){
            discriminants_tested += 1
            def roots = solve_quadratic_roots(sum_pq, nz)
            if(roots != nil){
               def p, q = roots.get(0), roots.get(1)
               if(p > Z(0) && q > Z(0) && p * q == nz && (p ^^ q) == xz && !pairs.contains(_xorfac_pair(p, q))){
                  pairs = pairs.append(_xorfac_pair(p, q))
               }
            }
         }
         if(max_shared_masks > 0 && tried >= max_shared_masks && shared < max_shared){ hit_limit = true }
      } else {
         skipped_overlap += 1
      }
      shared = shared + Z(1)
      while(shared <= max_shared && (shared & xz) != Z(0)){
         skipped_overlap += 1
         shared = shared + Z(1)
      }
   }
   out = out.set("pairs", pairs)
   out = out.set("pair_count", pairs.len)
   out = out.set("shared_masks_tried", tried)
   out = out.set("overlap_masks_skipped", skipped_overlap)
   out = out.set("discriminants_tested", discriminants_tested)
   out = out.set("max_shared", max_shared)
   out = out.set("hit_limit", hit_limit)
   out = out.set("success", pairs.len > 0 && !hit_limit)
   out = out.set("elapsed_ms", float(ticks() - t0) / 1000000.0)
   out
}

fn xor_factor_pairs_with_target(any n, any target) list {
   "Find factor pairs(p, q) of n where p^q equals target."
   xor_factor_with_target_report(n, target).get("pairs", [])
}

fn xor_factor_from_target(any n, any target) any {
   "Return one [p, q] pair where p*q == n and p^q == target, or nil."
   def pairs = xor_factor_pairs_with_target(n, target)
   if(pairs.len == 0){ return nil }
   def first = pairs.get(0)
   [first.get(0), first.get(1)]
}
