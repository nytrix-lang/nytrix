;; Keywords: rsa partial-key-exposure math crypto
;; RSA partial-key-exposure attacks routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; This module covers the bounded-enumeration cases directly and routes into
;; known-d / known-CRT-exponent recovery when enough structure is available.
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.partial_key_exposure(partial_key_exposure_known_d, partial_key_exposure_known_crt, partial_key_exposure_attack, partial_key_exposure_attack_report, partial_key_exposure_known_p, partial_key_exposure_known_p_msb, partial_key_exposure_known_p_msb_coppersmith, partial_key_exposure_known_p_msb_report)
use std.core
use std.os.clock (ticks)
use std.math.nt
use std.math.crypto.number.partial
use std.math.crypto.rsa.op (compute_phi, compute_d)
use std.math.crypto.rsa.known_d
use std.math.crypto.rsa.known_crt_exponents
use std.math.crypto.lattice.coppersmith as coppersmith

fn _pke_factor_pair_from_p(any n, any p) any {
   def nn, pp = Z(n), Z(p)
   if(pp <= Z(1) || pp >= nn || nn % pp != Z(0)){ return nil }
   def q = nn / pp
   (pp < q) ? [pp, q] : [q, pp]
}

fn _pke_elapsed_ms(any t0) f64 { float(ticks() - t0) / 1000000.0 }

fn _pke_success_report(dict out, str method, any result, any t0) dict {
   out = out.set("success", true)
   out = out.set("method", method)
   out = out.set("result", result)
   out.set("elapsed_ms", _pke_elapsed_ms(t0))
}

fn _pke_fail_report(dict out, str reason, any t0) dict {
   out = out.set("success", false)
   out = out.set("method", "none")
   out = out.set("reason", reason)
   out.set("elapsed_ms", _pke_elapsed_ms(t0))
}

fn _pke_partial_report(any pi) dict {
   "Summarize a partial integer without expanding candidates."
   mut out = dict(8)
   if(pi == nil){
      out = out.set("present", false)
      return out
   }
   out = out.set("present", true)
   out = out.set("unknowns", _pi_unknowns_count(pi))
   out = out.set("bounds", partial_integer_unknown_bounds(pi))
   out = out.set("known_lsb", partial_integer_known_lsb(pi))
   out = out.set("known_msb", partial_integer_known_msb(pi))
   out
}

fn _pke_single_unknown_values(any partial_pi, int max_unknown_bits) any {
   if(_pi_unknowns_count(partial_pi) != 1){ return nil }
   def bounds = partial_integer_unknown_bounds(partial_pi)
   if(bounds.len != 1){ return nil }
   def bound = bounds.get(0)
   if(bound <= 0){ return nil }
   def bits = bit_length(bound - 1)
   if(bits > max_unknown_bits){ return nil }
   mut vals = []
   mut v = 0
   while(v < bound){
      vals = vals.append(v)
      v += 1
   }
   vals
}

fn _pi_unknowns_count(any partial_pi) int {
   def bounds = partial_integer_unknown_bounds(partial_pi)
   is_list(bounds) ? bounds.len : 0
}

fn _pke_try_known_d(any n, any e, any partial_d, int max_unknown_bits) any {
   if(partial_d == nil){ return nil }
   if(_pi_unknowns_count(partial_d) == 0){
      def d0 = partial_integer_to_int(partial_d)
      if(d0 == nil){ return nil }
      def facs = known_d_factorize(n, e, d0)
      if(facs == nil || facs == 0){ return nil }
      return [facs.get(0), facs.get(1), d0]
   }
   def vals = _pke_single_unknown_values(partial_d, max_unknown_bits)
   if(vals == nil){ return nil }
   mut i = 0
   while(i < vals.len){
      def d0 = partial_integer_sub(partial_d, [vals.get(i)])
      def facs = known_d_factorize(n, e, d0)
      if(facs != nil && facs != 0){ return [facs.get(0), facs.get(1), d0] }
      i += 1
   }
   nil
}

fn _pke_crt_candidates(any e, any n, any partial_dp, any partial_dq, int max_unknown_bits) any {
   mut dp_vals, dq_vals = nil, nil
   if(partial_dp != nil){
      if(_pi_unknowns_count(partial_dp) == 0){ dp_vals = [partial_integer_to_int(partial_dp)] } else {
         def vals = _pke_single_unknown_values(partial_dp, max_unknown_bits)
         if(vals == nil){ return nil }
         dp_vals = []
         mut i = 0
         while(i < vals.len){
            dp_vals = dp_vals.append(partial_integer_sub(partial_dp, [vals.get(i)]))
            i += 1
         }
      }
   }
   if(partial_dq != nil){
      if(_pi_unknowns_count(partial_dq) == 0){ dq_vals = [partial_integer_to_int(partial_dq)] } else {
         def vals = _pke_single_unknown_values(partial_dq, max_unknown_bits)
         if(vals == nil){ return nil }
         dq_vals = []
         mut i = 0
         while(i < vals.len){
            dq_vals = dq_vals.append(partial_integer_sub(partial_dq, [vals.get(i)]))
            i += 1
         }
      }
   }
   [dp_vals, dq_vals]
}

fn partial_key_exposure_known_d(any n, any e, any partial_d, int max_unknown_bits=18) any {
   "Recover [p, q, d] from a partial private exponent when the unknown chunk
   is small enough for direct enumeration."
   _pke_try_known_d(n, e, partial_d, max_unknown_bits)
}

fn partial_key_exposure_known_crt(any n, any e, any partial_dp=nil, any partial_dq=nil, int max_unknown_bits=16) any {
   "Recover [p, q, dp, dq] from partial dp and/or dq using bounded
   enumeration plus CRT-exponent candidate generation."
   if(partial_dp == nil && partial_dq == nil){ return nil }
   def cands = _pke_crt_candidates(e, n, partial_dp, partial_dq, max_unknown_bits)
   if(cands == nil){ return nil }
   def dp_vals, dq_vals = cands.get(0), cands.get(1)
   if(dp_vals != nil && dq_vals != nil){
      mut i = 0
      while(i < dp_vals.len){
         mut j = 0
         while(j < dq_vals.len){
            def got = possible_prime_factors_from_crt_exponents(e, e + 2, n, dp_vals.get(i), dq_vals.get(j))
            if(got.len > 0){
               def pq = got.get(0)
               return [pq.get(0), pq.get(1), dp_vals.get(i), dq_vals.get(j)]
            }
            j += 1
         }
         i += 1
      }
      return nil
   }
   if(dp_vals != nil){
      mut i = 0
      while(i < dp_vals.len){
         def got = possible_prime_factors_from_crt_exponents(e, e + 2, n, dp_vals.get(i), nil)
         if(got.len > 0){
            def pq = got.get(0)
            return [pq.get(0), pq.get(1), dp_vals.get(i), nil]
         }
         i += 1
      }
      return nil
   }
   mut i = 0
   while(i < dq_vals.len){
      def got = possible_prime_factors_from_crt_exponents(e, e + 2, n, nil, dq_vals.get(i))
      if(got.len > 0){
         def pq = got.get(0)
         return [pq.get(0), pq.get(1), nil, dq_vals.get(i)]
      }
      i += 1
   }
   nil
}

fn partial_key_exposure_known_p(any n, any partial_p, int max_unknown_bits=22) any {
   "Recover [p, q] from a partial prime p when the unknown segment is small.
   The partial integer is little-endian by segment, matching
   std.math.crypto.number.partial. This is the bounded counterpart to the
   common RSA high-bits-known Coppersmith setup."
   if(partial_p == nil){ return nil }
   if(_pi_unknowns_count(partial_p) == 0){
      def p0 = partial_integer_to_int(partial_p)
      return p0 == nil ? nil : _pke_factor_pair_from_p(n, p0)
   }
   def vals = _pke_single_unknown_values(partial_p, max_unknown_bits)
   if(vals == nil){ return nil }
   mut i = 0
   while(i < vals.len){
      def p = partial_integer_sub(partial_p, [vals.get(i)])
      def pq = _pke_factor_pair_from_p(n, p)
      if(pq != nil){ return pq }
      i += 1
   }
   nil
}

fn partial_key_exposure_known_p_msb_coppersmith(any n, any p_msb, any unknown_bits, any beta=0.5, int m=2, int t=1, str reduction_method="ny") any {
   "Recover [p, q] from public(N, p_msb, unknown_bits) by building the
   univariate Coppersmith problem f(x)=p_msb+x. Every recovered root is
   validated before returning."
   def ub = int(unknown_bits)
   if(ub < 0){ return nil }
   def nn = Z(n)
   def base = Z(p_msb)
   def X = bigint_lshift(Z(1), ub)
   def roots = coppersmith.coppersmith_univariate([base, Z(1)], nn, X, beta, m, t, nil, reduction_method)
   mut i = 0
   while(i < roots.len){
      def r = Z(roots.get(i))
      if(r >= Z(0) && r < X){
         def pq = _pke_factor_pair_from_p(nn, base + r)
         if(pq != nil){ return pq }
      }
      i += 1
   }
   nil
}

fn partial_key_exposure_known_p_msb_report(any n, any p_msb, any unknown_bits, int max_unknown_bits=22, any beta=0.5, int m=2, int t=1, str reduction_method="ny") dict {
   "Explain the RSA high-bits-known recovery path.
   The report records bounded enumeration, Coppersmith parameters, root counts,
   and the validated factor pair when recovery succeeds."
   def t0 = ticks()
   mut out = dict(20)
   def ub = int(unknown_bits)
   out = out.set("n_bits", bit_length(Z(n)))
   out = out.set("p_msb_bits", bit_length(Z(p_msb)))
   out = out.set("unknown_bits", ub)
   out = out.set("max_unknown_bits", int(max_unknown_bits))
   out = out.set("reduction_method", reduction_method)
   if(ub < 0){ return _pke_fail_report(out, "unknown_bits must be non-negative", t0) }
   def X = bigint_lshift(Z(1), ub)
   out = out.set("X", X)
   def plan = coppersmith.coppersmith_univariate_plan([Z(p_msb), Z(1)], Z(n), X, beta, m, t, nil, reduction_method)
   out = out.set("coppersmith_plan", plan)
   out = out.set("coppersmith_root_count", 0)
   if(ub <= max_unknown_bits){
      out = out.set("bounded_attempted", true)
      out = out.set("bounded_candidates", bigint_lshift(Z(1), ub))
      def known_bits = bit_length(Z(p_msb)) - ub
      out = out.set("known_bits", known_bits)
      if(known_bits > 0){
         mut pi = partial_integer_new()
         pi = partial_integer_add_unknown(pi, ub)
         pi = partial_integer_add_known(pi, Z(p_msb) >> Z(ub), known_bits)
         out = out.set("partial_p", _pke_partial_report(pi))
         def direct = partial_key_exposure_known_p(n, pi, max_unknown_bits)
         if(direct != nil){ return _pke_success_report(out, "bounded", direct, t0) }
      } else {
         out = out.set("bounded_reason", "no known high bits")
      }
   } else {
      out = out.set("bounded_attempted", false)
      out = out.set("bounded_reason", "unknown bit count exceeds max_unknown_bits")
   }
   if(!plan.get("valid", false)){ return _pke_fail_report(out, "invalid Coppersmith plan: " + to_str(plan.get("reason", "unknown")), t0) }
   def c_report = coppersmith.coppersmith_univariate_report([Z(p_msb), Z(1)], Z(n), X, beta, m, t, nil, reduction_method)
   out = out.set("coppersmith_report", c_report)
   def roots = c_report.get("roots", [])
   out = out.set("coppersmith_roots", roots)
   out = out.set("coppersmith_root_count", roots.len)
   mut i = 0
   while(i < roots.len){
      def r = Z(roots.get(i))
      if(r >= Z(0) && r < X){
         def pq = _pke_factor_pair_from_p(n, Z(p_msb) + r)
         if(pq != nil){
            out = out.set("root", r)
            return _pke_success_report(out, "coppersmith", pq, t0)
         }
      }
      i += 1
   }
   _pke_fail_report(out, "no validated factor from bounded or Coppersmith roots", t0)
}

fn partial_key_exposure_known_p_msb(any n, any p_msb, any unknown_bits, int max_unknown_bits=22, str reduction_method="ny") any {
   "Recover [p, q] when the most-significant bits of p are known and the
   bottom `unknown_bits` bits are bounded enough for direct Ny enumeration."
   def report = partial_key_exposure_known_p_msb_report(n, p_msb, unknown_bits, max_unknown_bits, 0.5, 2, 1, reduction_method)
   report.get("success", false) ? report.get("result") : nil
}

fn partial_key_exposure_attack_report(any n, any e, any partial_d=nil, any partial_dp=nil, any partial_dq=nil, int max_unknown_bits=16, any partial_p=nil) dict {
   "Explain the staged RSA partial-key recovery pipeline."
   def t0 = ticks()
   mut out = dict(20)
   mut stages = []
   out = out.set("n_bits", bit_length(Z(n)))
   out = out.set("e", e)
   out = out.set("max_unknown_bits", int(max_unknown_bits))
   out = out.set("partial_d", _pke_partial_report(partial_d))
   out = out.set("partial_dp", _pke_partial_report(partial_dp))
   out = out.set("partial_dq", _pke_partial_report(partial_dq))
   out = out.set("partial_p", _pke_partial_report(partial_p))
   stages = stages.append("known_d")
   def by_d = partial_key_exposure_known_d(n, e, partial_d, max_unknown_bits)
   if(by_d != nil){
      out = out.set("stages", stages)
      return _pke_success_report(out, "known_d", by_d, t0)
   }
   stages = stages.append("known_crt")
   def by_crt = partial_key_exposure_known_crt(n, e, partial_dp, partial_dq, max_unknown_bits)
   if(by_crt != nil){
      out = out.set("stages", stages)
      return _pke_success_report(out, "known_crt", by_crt, t0)
   }
   stages = stages.append("partial_p")
   def by_p = partial_key_exposure_known_p(n, partial_p, max_unknown_bits)
   if(by_p != nil){
      out = out.set("stages", stages)
      return _pke_success_report(out, "partial_p", by_p, t0)
   }
   out = out.set("stages", stages)
   _pke_fail_report(out, "no staged partial-key method recovered factors", t0)
}

fn partial_key_exposure_attack(any n, any e, any partial_d=nil, any partial_dp=nil, any partial_dq=nil, int max_unknown_bits=16, any partial_p=nil) any {
   "Recover RSA factors from practical partial-key leakage. Tries partial d
   first, then partial CRT exponents, then a bounded partial-prime leak."
   def report = partial_key_exposure_attack_report(n, e, partial_d, partial_dp, partial_dq, max_unknown_bits, partial_p)
   report.get("success", false) ? report.get("result") : nil
}
