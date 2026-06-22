;; Keywords: factorization primality math crypto number-theory
;; Integer-factorization routines for primality tests and pseudoprime checks.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap4.pdf
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.primality(prp_report, prp, euler_prp_report, euler_prp, strong_prp_report, strong_prp, deterministic_miller_rabin64_report, deterministic_miller_rabin64, aprcl_parameter_plan_report, aprcl_jacobi_seed_report, aprcl_jacobi_normalize_report, aprcl_jacobi_mul_report, aprcl_jacobi_square_report, aprcl_jacobi_pow_report, aprcl_primality_report, fibonacci_prp_report, fibonacci_prp, lucas_prp_report, lucas_prp, strong_lucas_prp_report, strong_lucas_prp, extra_strong_lucas_prp_report, extra_strong_lucas_prp, selfridge_prp_report, selfridge_prp, strong_selfridge_prp_report, strong_selfridge_prp, strong_bpsw_prp_report, strong_bpsw_prp, bpsw_prp_report, bpsw_prp, pratt_certificate_report, pratt_certificate, verify_pratt_certificate_report, verify_pratt_certificate, lucas_nplus1_certificate_report, lucas_nplus1_certificate, verify_lucas_nplus1_certificate_report, verify_lucas_nplus1_certificate, pocklington_certificate_report, pocklington_certificate, primality_certificate_report)
use std.math.nt
use std.math.crypto.number.lucas as lucas
use std.math.crypto.factorization.aprcl_data as aprcl_data
use std.os.clock (ticks)

mut _pc_mr64_bases_cache = nil

fn _pc_mr64_bases() list {
   if _pc_mr64_bases_cache == nil { _pc_mr64_bases_cache = [2, 325, 9375, 28178, 450775, 9780504, 1795265022] }
   _pc_mr64_bases_cache
}

fn _pc_z(any x) bigint { is_bigint(x) ? x : Z(x) }

fn _pc_abs(any x) bigint {
   def z = _pc_z(x)
   z < Z(0) ? -z : z
}

fn _pc_mod(any x, any n) bigint {
   def nz = _pc_z(n)
   mut r = _pc_z(x) % nz
   if r < Z(0) { r = r + nz }
   r
}

fn _pc_mod_z(any x, bigint n) bigint {
   mut r = _pc_z(x) % n
   if r < Z(0) { r = r + n }
   r
}

fn _pc_nontrivial_factor(any g, any n) bool {
   def gz, nz = _pc_z(g), _pc_z(n)
   gz > Z(1) && gz < nz && nz % gz == Z(0)
}

fn _pc_odd_candidate_state(bigint nz) int {
   def z2 = Z(2)
   if nz < z2 { return -1 }
   if nz == z2 { return 1 }
   if nz % z2 == Z(0) { return -1 }
   0
}

fn _pc_factor_product(list facs) bigint {
   mut out = Z(1)
   mut i = 0
   while i < facs.len {
      def row = facs.get(i)
      def p, e = _pc_z(row.get(0, Z(1))), int(row.get(1, 1))
      mut j = 0
      while j < e {
         out = out * p
         j += 1
      }
      i += 1
   }
   out
}

fn _pc_unique_primes(list facs) list {
   mut out = []
   mut i = 0
   while i < facs.len {
      def p = _pc_z(facs.get(i).get(0, Z(1)))
      if p > Z(1) { out = out.append(p) }
      i += 1
   }
   out
}

fn _pc_find_subcertificate(list certs, any q) any {
   def qz = _pc_z(q)
   mut i = 0
   while i < certs.len {
      def c = certs.get(i)
      if is_dict(c) && _pc_z(c.get("n", Z(0))) == qz { return c }
      i += 1
   }
   nil
}

fn _pc_finish(dict out, any t0, str status) dict {
   out.merge({"elapsed_ms": float(ticks() - t0) / 1000000.0, "status": status})
}

fn _pc_finish_with(dict out, any t0, str status, dict fields) dict {
   _pc_finish(out.merge(fields), t0, status)
}

fn _pc_report(str method, any n, dict fields) dict {
   {"method": method, "n": n}.merge(fields)
}

fn _pc_factorization_fields(str factor_key, list facs, any expected) dict {
   def product = _pc_factor_product(facs)
   {"factor_product": product, "complete_factorization": product == _pc_z(expected)}.set(factor_key, facs)
}

fn _pc_verify_pratt_subcerts(list qs, list subs) dict {
   mut verified_nodes, i = 1, 0
   while i < qs.len {
      def q = _pc_z(qs.get(i))
      def sub = _pc_find_subcertificate(subs, q)
      if sub == nil {
         return {"ok": false, "verified_nodes": verified_nodes, "reason": "missing recursive subcertificate", "missing_q": q}
      }
      def vr = verify_pratt_certificate_report(sub)
      verified_nodes += int(vr.get("verified_nodes", 0))
      if !vr.get("proof_valid", false) {
         return {"ok": false, "verified_nodes": verified_nodes, "reason": "recursive subcertificate failed", "missing_q": q, "subproof": vr}
      }
      i += 1
   }
   {"ok": true, "verified_nodes": verified_nodes}
}

fn _pc_build_pratt_subcerts(list qs, int max_base, int max_depth) dict {
   mut subs, sub_size, i = [], 0, 0
   while i < qs.len {
      def q = qs.get(i)
      def sub = pratt_certificate_report(q, max_base, max_depth - 1)
      if !sub.get("prime", false) {
         return {"ok": false, "reason": "recursive factor proof failed", "missing_q": q, "subproof": sub}
      }
      sub_size += int(sub.get("certificate_size", 1))
      subs = subs.append(sub)
      i += 1
   }
   {"ok": true, "subcertificates": subs, "certificate_size": sub_size + 1}
}

fn _pc_finish_failed_subcert(dict out, any t0, dict vr, str status="invalid") dict {
   def reason, missing_q, subproof = vr.get("reason", "recursive subcertificate failed"), vr.get("missing_q", nil), vr.get("subproof", nil)
   if subproof != nil {
      return _pc_finish(out.merge({"reason": reason, "missing_q": missing_q, "subproof": subproof}), t0, status)
   }
   _pc_finish(out.merge({"reason": reason, "missing_q": missing_q}), t0, status)
}

fn _pc_recursive_factor_setup(dict out, any t0, any nz, int max_base, int max_depth, str factor_key, any target, str incomplete_reason) dict {
   if max_depth <= 0 {
      return {"done": true, "report": _pc_finish(out.set("reason", "recursive certificate depth exhausted"), t0, "inconclusive")}
   }
   def screen = bpsw_prp_report(nz)
   out = out.set("screen", screen)
   if !screen.get("probable_prime", false) {
      out = out.set("factor", screen.get("factor", nil))
      return {"done": true, "report": _pc_finish(out.set("reason", screen.get("status", "screen-rejected")), t0, "composite")}
   }
   def facs = factor(target)
   def factor_fields = _pc_factorization_fields(factor_key, facs, target)
   out = out.merge(factor_fields)
   if !factor_fields.get("complete_factorization", false) {
      return {"done": true, "report": _pc_finish(out.set("reason", incomplete_reason), t0, "inconclusive")}
   }
   def qs = _pc_unique_primes(facs)
   def subcerts = _pc_build_pratt_subcerts(qs, max_base, max_depth)
   if !subcerts.get("ok", false) {
      return {"done": true, "report": _pc_finish_failed_subcert(out, t0, subcerts, "inconclusive")}
   }
   {"done": false, "out": out, "qs": qs, "subcerts": subcerts}
}

fn _pc_finish_verified_certificate(dict out, any t0, dict verified) dict {
   out = out.merge({"verification": verified, "verified": verified.get("proof_valid", false)})
   if !verified.get("proof_valid", false) {
      return _pc_finish(out.set("reason", "internal certificate verification failed"), t0, "invalid")
   }
   _pc_finish(out.set("prime", true), t0, "proven-prime")
}

fn _pc_certificate_trivial_case(dict out, any t0, any nz, dict prime2_fields) any {
   if nz < Z(2) { return _pc_finish_with(out, t0, "composite", {"reason": "n < 2"}) }
   if nz == Z(2) { return _pc_finish_with(out, t0, "proven-prime", prime2_fields) }
   if nz % Z(2) == Z(0) {
      return _pc_finish_with(out, t0, "composite", {"factor": Z(2), "cofactor": nz / Z(2)})
   }
   nil
}

fn _pc_witness_setup_failure(dict out, any t0, dict w, str missing_reason, bool include_attempts=false) any {
   def status = w.get("status", "")
   if status == "factor" {
      return _pc_finish_with(out, t0, "composite", {"factor": w.get("factor"), "cofactor": w.get("cofactor")})
   }
   if status != "witness" {
      mut fields = {"reason": missing_reason}
      if include_attempts { fields = fields.set("attempts", w.get("attempts", [])) }
      return _pc_finish_with(out, t0, "inconclusive", fields)
   }
   nil
}

fn _pc_lucas_rank_checks(any nz, any P, any Q, list qs) dict {
   mut checks, i = [], 0
   while i < qs.len {
      def q = _pc_z(qs.get(i))
      def idx = (nz + Z(1)) / q
      def uv = lucas.lucas_uv_mod(P, Q, idx, nz)
      def U = _pc_mod(uv.get(0), nz)
      def g = gcd(U, nz)
      checks = checks.append({"q": q, "index": idx, "U": U, "gcd": g, "ok": g == Z(1)})
      if g != Z(1) {
         return {"ok": false, "q": q, "gcd": g, "checks": checks, "factor": _pc_nontrivial_factor(g, nz) ? g : nil}
      }
      i += 1
   }
   {"ok": true, "checks": checks}
}

fn _pc_finish_certified_proof(dict out, any t0, str proof_system, dict proof, any certificate, bool include_verified=true) any {
   if !proof.get("prime", false) { return nil }
   mut fields = {
      "prime": true, "proof_system": proof_system,
      "certificate": certificate, "certificate_size": proof.get("certificate_size", 0),
   }
   if include_verified { fields = fields.set("verified", proof.get("verified", false)) }
   _pc_finish_with(out, t0, "proven-prime", fields)
}

fn _pc_verify_recursive_setup(any cert, dict out, any t0, str factor_key, int target_offset, str product_reason) dict {
   if !is_dict(cert) {
      return {"done": true, "report": _pc_finish_with(out, t0, "invalid", {"reason": "certificate is not a dict"})}
   }
   def nz = _pc_z(cert.get("n", Z(0)))
   out = out.set("n", nz)
   if nz == Z(2) {
      return {"done": true, "report": _pc_finish_with(out, t0, "verified-prime", {"prime": true, "proof_valid": true, "verified_nodes": 1})}
   }
   if nz < Z(2) || nz % Z(2) == Z(0) {
      return {"done": true, "report": _pc_finish_with(out, t0, "invalid", {"reason": "n is not an odd integer > 2"})}
   }
   def facs = cert.get(factor_key, [])
   def target = nz + Z(target_offset)
   def factor_fields = _pc_factorization_fields(factor_key, facs, target)
   out = out.merge(factor_fields)
   if factor_fields.get("factor_product", Z(0)) != target {
      return {"done": true, "report": _pc_finish_with(out, t0, "invalid", {"reason": product_reason})}
   }
   def qs = _pc_unique_primes(facs)
   def subproofs = _pc_verify_pratt_subcerts(qs, cert.get("subcertificates", []))
   if !subproofs.get("ok", false) {
      return {"done": true, "report": _pc_finish_failed_subcert(out, t0, subproofs)}
   }
   {"done": false, "out": out, "n": nz, "factors": facs, "qs": qs, "verified_nodes": int(subproofs.get("verified_nodes", 1))}
}

fn _pc_strong_prp_base_report(any n, any a) dict {
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("strong-prp", nz, {"base": _pc_z(a), "probable_prime": false})
   if nz < Z(2) { return _pc_finish(out, t0, "composite") }
   if nz == Z(2) { return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true}) }
   if nz % Z(2) == Z(0) { return _pc_finish_with(out, t0, "composite", {"factor": Z(2)}) }
   def az = _pc_mod(a, nz)
   out = out.set("base", az)
   def g = gcd(az, nz)
   if _pc_nontrivial_factor(g, nz) {
      return _pc_finish_with(out, t0, "factor", {"factor": g, "cofactor": nz / g})
   }
   if g != Z(1) {
      return _pc_finish_with(out, t0, "non-coprime-base", {"gcd": g})
   }
   mut d, s = nz - Z(1), 0
   while d % Z(2) == Z(0) {
      d = d / Z(2)
      s += 1
   }
   mut x = power_mod(az, d, nz)
   out = out.merge({"d": d, "s": s})
   if x == Z(1) || x == nz - Z(1) {
      return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true})
   }
   mut r = 1
   while r < s {
      x = _pc_mod(x * x, nz)
      if x == nz - Z(1) {
         return _pc_finish_with(out, t0, "probable-prime", {"witness_round": r, "probable_prime": true})
      }
      if x == Z(1) {
         return _pc_finish(out, t0, "composite")
      }
      r += 1
   }
   _pc_finish(out, t0, "composite")
}

fn prp_report(any n, any a=2) dict {
   "Fermat probable-prime test to base a."
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("fermat-prp", nz, {"base": _pc_z(a), "probable_prime": false})
   if nz < Z(2) { return _pc_finish(out, t0, "composite") }
   if nz == Z(2) { return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true}) }
   if nz % Z(2) == Z(0) { return _pc_finish_with(out, t0, "composite", {"factor": Z(2)}) }
   def az = _pc_mod(a, nz)
   out = out.set("base", az)
   def g = gcd(az, nz)
   if _pc_nontrivial_factor(g, nz) {
      return _pc_finish_with(out, t0, "factor", {"factor": g, "cofactor": nz / g})
   }
   if g != Z(1) {
      return _pc_finish_with(out, t0, "non-coprime-base", {"gcd": g})
   }
   def value = power_mod(az, nz - Z(1), nz)
   if value == Z(1) {
      return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true, "value": value})
   }
   _pc_finish_with(out, t0, "composite", {"value": value})
}

fn prp(any n, any a=2) bool {
   "Runs the prp operation."
   def nz = _pc_z(n)
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   def az = _pc_mod(a, nz)
   if gcd(az, nz) != Z(1) { return false }
   power_mod(az, nz - Z(1), nz) == Z(1)
}

fn euler_prp_report(any n, any a=2) dict {
   "Euler/Solovay-Strassen probable-prime test to base a."
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("euler-prp", nz, {"base": _pc_z(a), "probable_prime": false})
   if nz < Z(2) { return _pc_finish(out, t0, "composite") }
   if nz == Z(2) { return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true}) }
   if nz % Z(2) == Z(0) { return _pc_finish_with(out, t0, "composite", {"factor": Z(2)}) }
   def az = _pc_mod(a, nz)
   out = out.set("base", az)
   def g = gcd(az, nz)
   if _pc_nontrivial_factor(g, nz) {
      return _pc_finish_with(out, t0, "factor", {"factor": g, "cofactor": nz / g})
   }
   if g != Z(1) {
      return _pc_finish_with(out, t0, "non-coprime-base", {"gcd": g})
   }
   def lhs = power_mod(az, (nz - Z(1)) / Z(2), nz)
   def j = jacobi(az, nz)
   def rhs = (j < 0) ? (nz - Z(1)) : Z(j)
   if lhs == rhs {
      return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true, "value": lhs, "jacobi": j})
   }
   _pc_finish_with(out, t0, "composite", {"value": lhs, "expected": rhs, "jacobi": j})
}

fn euler_prp(any n, any a=2) bool {
   "Runs the euler prp operation."
   def nz = _pc_z(n)
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   def az = _pc_mod(a, nz)
   if gcd(az, nz) != Z(1) { return false }
   def lhs = power_mod(az, (nz - Z(1)) / Z(2), nz)
   def j = jacobi(az, nz)
   def rhs = (j < 0) ? (nz - Z(1)) : Z(j)
   lhs == rhs
}

fn strong_prp_report(any n, any a=2) dict {
   "Miller-Rabin strong probable-prime test to base a."
   _pc_strong_prp_base_report(n, a)
}

fn _pc_strong_prp_odd_base_bool(bigint nz, any az) bool {
   def z0, z1, z2 = Z(0), Z(1), Z(2)
   mut d = nz - z1
   mut s = 0
   while d % z2 == z0 {
      d = d / z2
      s += 1
   }
   mut x = power_mod(az, d, nz)
   if x == z1 || x == nz - z1 { return true }
   mut r = 1
   while r < s {
      x = (x * x) % nz
      if x == nz - z1 { return true }
      if x == z1 { return false }
      r += 1
   }
   false
}

fn _pc_strong_prp_base_bool(any n, any a) bool {
   def nz = _pc_z(n)
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   def z1 = Z(1)
   def az = _pc_mod_z(a, nz)
   if gcd(az, nz) != z1 { return false }
   _pc_strong_prp_odd_base_bool(nz, az)
}

fn _pc_strong_prp_base2_bool(bigint nz) bool {
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   _pc_strong_prp_odd_base_bool(nz, Z(2))
}

fn strong_prp(any n, any a=2) bool {
   "Runs the strong prp operation."
   _pc_strong_prp_base_bool(n, a)
}

fn deterministic_miller_rabin64_report(any n) dict {
   "Deterministic Miller-Rabin decision for inputs below 2^64."
   def t0, nz = ticks(), _pc_z(n)
   def bases = _pc_mr64_bases()
   mut out = _pc_report("deterministic-miller-rabin-64", nz, {
         "source_model": "64-bit Miller-Rabin strong probable-prime ladder",
         "proof_system": "deterministic-miller-rabin-64",
         "domain_bits": 64, "bases": bases, "prime": false,
         "probable_prime": false, "deterministic": false,
   })
   if nz < Z(2) { return _pc_finish(out, t0, "composite") }
   if nz == Z(2) || nz == Z(3) {
      return _pc_finish_with(out, t0, "proven-prime", {"prime": true, "probable_prime": true, "deterministic": true})
   }
   if nz % Z(2) == Z(0) {
      return _pc_finish_with(out, t0, "composite", {"factor": Z(2)})
   }
   if bit_length(nz) > 64 {
      return _pc_finish_with(out, t0, "out-of-domain", {"reason": "n exceeds deterministic 64-bit Miller-Rabin domain"})
   }
   mut checks = []
   mut i = 0
   while i < bases.len {
      def b = bases.get(i)
      if b < nz {
         def ok = _pc_strong_prp_base_bool(nz, b)
         checks = checks.append({"base": b, "status": ok ? "probable-prime" : "composite", "probable_prime": ok})
         if !ok {
            return _pc_finish_with(out, t0, "composite", {
                  "base": b, "checks": checks,
            })
         }
      }
      i += 1
   }
   _pc_finish_with(out, t0, "proven-prime", {
         "prime": true, "probable_prime": true, "deterministic": true,
         "checks": checks, "checked_bases": checks.len,
   })
}

fn deterministic_miller_rabin64(any n) bool {
   "Return true when deterministic_miller_rabin64_report proves n prime."
   if is_int(n) && int(n) <= 2147483647 { return is_prime(n) }
   def nz = _pc_z(n)
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   if nz == Z(3) { return true }
   if bit_length(nz) > 64 { return false }
   if bit_length(nz) <= 31 { return is_prime(nz) }
   def bases = _pc_mr64_bases()
   mut i = 0
   while i < bases.len {
      def b = bases[i]
      if b < nz && !_pc_strong_prp_base_bool(nz, b) { return false }
      i += 1
   }
   true
}

fn _pc_take(list xs, int count) list {
   mut out = []
   mut i = 0
   while i < count && i < xs.len {
      out = out.append(xs.get(i))
      i += 1
   }
   out
}

fn _pc_zero_coeffs(int n) list {
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < n {
      out[i] = Z(0)
      i += 1
   }
   out
}

fn _pc_jacobi_coeffs(list coeffs, int PK, any modulus) list {
   mut out = list(PK)
   __list_set_len(out, PK)
   mut i = 0
   while i < PK {
      out[i] = i < coeffs.len ? _pc_mod(coeffs[i], modulus) : Z(0)
      i += 1
   }
   out
}

fn _pc_aprcl_valid_params(int PK, int PL, int PM, int P, any modulus) bool {
   PK > 0 && PL > 0 && PM > 0 && P > 1 && PL <= PK && _pc_z(modulus) > Z(1)
}

fn _pc_jacobi_normalize_coeffs(list coeffs, int PK, int PL, int PM, int P, any modulus) list {
   mut out = _pc_jacobi_coeffs(coeffs, PK, modulus)
   mut I = PL
   while I < PK {
      def t = out[I]
      if t != Z(0) {
         mut J = 1
         while J < P {
            def idx = I - J * PM
            out[idx] = out[idx] - t
            J += 1
         }
         out[I] = Z(0)
      }
      I += 1
   }
   I = 0
   while I < PK {
      out[I] = _pc_mod(out[I], modulus)
      I += 1
   }
   out
}

fn _pc_jacobi_mul_coeffs(list lhs, list rhs, int PK, int PL, int PM, int P, any modulus) list {
   def a = _pc_jacobi_coeffs(lhs, PK, modulus)
   def b = _pc_jacobi_coeffs(rhs, PK, modulus)
   mut tmp = _pc_zero_coeffs(PK)
   mut I = 0
   while I < PL {
      def ai = a[I]
      if ai != Z(0) {
         mut J = 0
         while J < PL {
            def bj = b[J]
            if bj != Z(0) {
               def K = (I + J) % PK
               tmp[K] = tmp[K] + ai * bj
            }
            J += 1
         }
      }
      I += 1
   }
   _pc_jacobi_normalize_coeffs(tmp, PK, PL, PM, P, modulus)
}

fn _pc_jacobi_square_coeffs(list coeffs, int PK, int PL, int PM, int P, any modulus) list {
   def a = _pc_jacobi_coeffs(coeffs, PK, modulus)
   mut tmp = _pc_zero_coeffs(PK)
   mut I = 0
   while I < PL {
      def ai = a[I]
      if ai != Z(0) {
         mut K = (2 * I) % PK
         tmp[K] = tmp[K] + ai * ai
         def twice = ai + ai
         mut J = I + 1
         while J < PL {
            def aj = a[J]
            if aj != Z(0) {
               K = (I + J) % PK
               tmp[K] = tmp[K] + twice * aj
            }
            J += 1
         }
      }
      I += 1
   }
   _pc_jacobi_normalize_coeffs(tmp, PK, PL, PM, P, modulus)
}

fn aprcl_jacobi_normalize_report(list coeffs, int PK, int PL, int PM, int P, any modulus) dict {
   "Normalize APRCL Jacobi-sum coefficients."
   def t0 = ticks()
   mut out = {"method": "aprcl-jacobi-normalize-report", "source_model": "APRCL Jacobi-sum normalization", "PK": PK, "PL": PL, "PM": PM, "P": P, "modulus": _pc_z(modulus)}
   if !_pc_aprcl_valid_params(PK, PL, PM, P, modulus) {
      return _pc_finish_with(out, t0, "invalid-parameters", {"ok": false})
   }
   def normalized = _pc_jacobi_normalize_coeffs(coeffs, PK, PL, PM, P, modulus)
   _pc_finish_with(out, t0, "ok", {"ok": true, "coefficients": normalized, "input_len": coeffs.len})
}

fn aprcl_jacobi_mul_report(list lhs, list rhs, int PK, int PL, int PM, int P, any modulus) dict {
   "Multiply APRCL Jacobi-sum coefficient vectors."
   def t0 = ticks()
   mut out = {"method": "aprcl-jacobi-mul-report", "source_model": "APRCL Jacobi-sum multiply", "PK": PK, "PL": PL, "PM": PM, "P": P, "modulus": _pc_z(modulus)}
   if !_pc_aprcl_valid_params(PK, PL, PM, P, modulus) {
      return _pc_finish_with(out, t0, "invalid-parameters", {"ok": false})
   }
   def product = _pc_jacobi_mul_coeffs(lhs, rhs, PK, PL, PM, P, modulus)
   _pc_finish_with(out, t0, "ok", {"ok": true, "coefficients": product, "mul_terms": PL * PL})
}

fn aprcl_jacobi_square_report(list coeffs, int PK, int PL, int PM, int P, any modulus) dict {
   "Square APRCL Jacobi-sum coefficient vectors."
   def t0 = ticks()
   mut out = {"method": "aprcl-jacobi-square-report", "source_model": "APRCL Jacobi-sum square", "PK": PK, "PL": PL, "PM": PM, "P": P, "modulus": _pc_z(modulus)}
   if !_pc_aprcl_valid_params(PK, PL, PM, P, modulus) {
      return _pc_finish_with(out, t0, "invalid-parameters", {"ok": false})
   }
   def sq = _pc_jacobi_square_coeffs(coeffs, PK, PL, PM, P, modulus)
   _pc_finish_with(out, t0, "ok", {"ok": true, "coefficients": sq, "square_terms": (PL * (PL + 1)) / 2})
}

fn aprcl_jacobi_pow_report(list coeffs, any exponent, int PK, int PL, int PM, int P, any modulus) dict {
   "Raise an APRCL Jacobi-sum vector to exponent E using square-and-multiply over JS_2/JS_JW."
   def t0 = ticks()
   mut out = {"method": "aprcl-jacobi-pow-report", "source_model": "APRCL Jacobi-sum exponentiation", "PK": PK, "PL": PL, "PM": PM, "P": P, "modulus": _pc_z(modulus), "exponent": _pc_z(exponent)}
   if !_pc_aprcl_valid_params(PK, PL, PM, P, modulus) || _pc_z(exponent) < Z(0) {
      return _pc_finish_with(out, t0, "invalid-parameters", {"ok": false})
   }
   mut result = _pc_zero_coeffs(PK)
   result[0] = Z(1)
   mut base = _pc_jacobi_normalize_coeffs(coeffs, PK, PL, PM, P, modulus)
   mut e = _pc_z(exponent)
   mut squares, multiplies = 0, 0
   while e > Z(0) {
      if e % Z(2) == Z(1) {
         result = _pc_jacobi_mul_coeffs(result, base, PK, PL, PM, P, modulus)
         multiplies += 1
      }
      e = e / Z(2)
      if e > Z(0) {
         base = _pc_jacobi_square_coeffs(base, PK, PL, PM, P, modulus)
         squares += 1
      }
   }
   _pc_finish_with(out, t0, "ok", {"ok": true, "coefficients": result, "squares": squares, "multiplies": multiplies})
}

fn aprcl_jacobi_seed_report(int mode, int P, int PL, int Q) dict {
   "Return a JacobiSum seed vector for small checked sls/jpqs entries."
   def t0 = ticks()
   def myP = mode == 1 ? 1 : (mode == 2 ? 4 : P)
   def seeds = [
      {"p": 2, "q": 5, "coefficients": [Z(-1), Z(-2)]},
      {"p": 3, "q": 7, "coefficients": [Z(-1), Z(-3)]},
      {"p": 2, "q": 13, "coefficients": [Z(3), Z(-2)]},
      {"p": 3, "q": 13, "coefficients": [Z(-4), Z(-3)]},
      {"p": 5, "q": 11, "coefficients": [Z(0), Z(2), Z(-2), Z(-1)]},
      {"p": 3, "q": 31, "coefficients": [Z(5), Z(6)]},
      {"p": 5, "q": 31, "coefficients": [Z(2), Z(-4), Z(-1), Z(2)]},
      {"p": 2, "q": 41, "coefficients": [Z(-3), Z(-4), Z(0), Z(-4)]},
      {"p": 1, "q": 41, "coefficients": [Z(-5), Z(0), Z(4), Z(0)]},
      {"p": 4, "q": 41, "coefficients": [Z(-3), Z(-4), Z(0), Z(-4)]},
   ]
   mut out = {"method": "aprcl-jacobi-seed-report", "source_model": "APRCL JacobiSum sls/jpqs seed table", "mode": mode, "P": P, "lookup_p": myP, "PL": PL, "Q": Q}
   mut i = 0
   while i < seeds.len {
      def row = seeds.get(i)
      if int(row.get("p")) == myP && int(row.get("q")) == Q {
         def coeffs = row.get("coefficients")
         if coeffs.len < PL {
            return _pc_finish_with(out, t0, "insufficient-seed-length", {"ok": false, "available_len": coeffs.len})
         }
         return _pc_finish_with(out, t0, "ok", {"ok": true, "coefficients": _pc_take(coeffs, PL), "seed_index": i})
      }
      i += 1
   }
   _pc_finish_with(out, t0, "missing-seed", {"ok": false, "reason": "seed not ported yet"})
}

fn aprcl_parameter_plan_report(any n) dict {
   "Return the APRCL T/P/Q schedule selected by the setup loop."
   def t0, nz = ticks(), _pc_z(n)
   def aiT = [Z(60), Z(5040), Z(55440), Z(720720), Z(4324320), Z(73513440), Z(367567200), Z(1396755360), Z(6983776800)]
   def aiNP = [3, 4, 5, 6, 6, 7, 7, 8, 8]
   def aiP = [2, 3, 5, 7, 11, 13, 17, 19]
   def aiQ = aprcl_data.aprcl_q_prime_table()
   def aiNQ = [8, 27, 45, 81, 134, 245, 351, 424, 618]
   mut level = -1
   mut testing_qs = 0
   mut S = Z(2)
   mut trace = []
   mut i = 0
   while i < aiT.len && level < 0 {
      S = Z(2)
      mut j = 0
      def limit = min(int(aiNQ.get(i)), aiQ.len)
      while j < limit && level < 0 {
         def Q = int(aiQ.get(j))
         def T = _pc_z(aiT.get(i))
         if T % Z(Q - 1) == Z(0) {
            mut U = T * Z(Q)
            mut keep = true
            while keep {
               U = U / Z(Q)
               S = S * Z(Q)
               keep = U % Z(Q) == Z(0)
            }
            if S * S > nz {
               level = i
               testing_qs = j
            }
         }
         j += 1
      }
      trace = trace.append({"level": i, "S": S, "tested_qs": j})
      i += 1
   }
   {
      "method": "aprcl-parameter-plan-report",
      "source_model": "APRCL prime-pair test selection",
      "n": nz, "found": level >= 0, "level": level,
      "T": level >= 0 ? aiT.get(level) : nil,
      "NP": level >= 0 ? aiNP.get(level) : 0,
      "P_primes": level >= 0 ? _pc_take(aiP, int(aiNP.get(level))) : [],
      "testing_qs": testing_qs,
      "Q_primes": level >= 0 ? _pc_take(aiQ, testing_qs + 1) : [],
      "S": S, "S_square_exceeds_n": level >= 0 && S * S > nz,
      "table_coverage": "aprcl-levels-with-618-q-primes",
      "trace": trace, "elapsed_ms": float(ticks() - t0) / 1000000.0,
   }
}

fn aprcl_primality_report(any n, int max_base=128) dict {
   "APRCL-compatible primality report: BPSW screen, deterministic 64-bit proof, then recursive certificates."
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("aprcl-primality-report", nz, {
         "source_model": "APRCL decision pipeline",
         "prime": false, "probable_prime": false,
   })
   out = out.set("aprcl_parameter_plan", aprcl_parameter_plan_report(nz))
   def screen = bpsw_prp_report(nz)
   out = out.set("bpsw", screen)
   if !screen.get("probable_prime", false) {
      return _pc_finish_with(out, t0, "composite", {
            "reason": screen.get("status", "screen-rejected"),
            "factor": screen.get("factor", nil),
      })
   }
   out = out.set("probable_prime", true)
   if bit_length(nz) <= 64 {
      def mr = deterministic_miller_rabin64_report(nz)
      out = out.set("deterministic_miller_rabin64", mr)
      if mr.get("prime", false) {
         return _pc_finish_with(out, t0, "proven-prime", {
               "prime": true, "proof_system": "deterministic-miller-rabin-64",
               "certificate": mr, "certificate_size": int(mr.get("checked_bases", 0)),
         })
      }
      return _pc_finish_with(out, t0, "composite", {"proof_system": "deterministic-miller-rabin-64", "certificate": mr})
   }
   def cert = primality_certificate_report(nz, max_base)
   out = out.set("recursive_certificate", cert)
   if cert.get("prime", false) {
      return _pc_finish_with(out, t0, "proven-prime", {
            "prime": true, "proof_system": cert.get("proof_system", "recursive-certificate"),
            "certificate": cert.get("certificate", cert),
            "certificate_size": int(cert.get("certificate_size", 0)),
      })
   }
   def aprcl = aprcl_data.aprcl_proof_report(nz)
   out = out.set("aprcl_proof", aprcl)
   if aprcl.get("status", "") == "composite" {
      return _pc_finish_with(out, t0, "composite", {
            "reason": aprcl.get("reason", "aprcl rejected"),
            "proof_system": "aprcl",
            "certificate": aprcl.get("certificate", aprcl),
      })
   }
   if aprcl.get("proof_valid", false) {
      return _pc_finish_with(out, t0, "proven-prime", {
            "prime": true, "proof_system": "aprcl",
            "certificate": aprcl.get("certificate", aprcl),
            "verification": aprcl.get("verification", nil),
            "certificate_size": aprcl.get("certificate", dict(0)).get("pairs", []).len,
      })
   }
   _pc_finish_with(out, t0, "probable-prime", {
         "proof_status": cert.get("status", "inconclusive"),
         "aprcl_status": aprcl.get("status", "inconclusive"),
         "reason": "BPSW accepted but deterministic/certificate/APRCL layers did not certify",
   })
}

fn _pc_lucas_common_setup(str method, any n, any P, any Q) dict {
   def t0, nz = ticks(), _pc_z(n)
   def pz, qz = _pc_z(P), _pc_z(Q)
   def D = pz * pz - Z(4) * qz
   mut out = _pc_report(method, nz, {"P": pz, "Q": qz, "D": D, "probable_prime": false})
   if nz < Z(2) { return {"done": true, "report": _pc_finish(out, t0, "composite")} }
   if nz == Z(2) { return {"done": true, "report": _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true})} }
   if nz % Z(2) == Z(0) { return {"done": true, "report": _pc_finish_with(out, t0, "composite", {"factor": Z(2)})} }
   def g = gcd(Z(2) * qz * D, nz)
   out = out.set("discriminant_gcd", g)
   if _pc_nontrivial_factor(g, nz) {
      return {"done": true, "report": _pc_finish_with(out, t0, "factor", {"factor": g, "cofactor": nz / g})}
   }
   if g != Z(1) {
      return {"done": true, "report": _pc_finish_with(out, t0, "invalid-parameters", {"gcd": g})}
   }
   def j = jacobi(D, nz)
   if j == 0 {
      return {"done": true, "report": _pc_finish_with(out, t0, "jacobi-zero", {"jacobi": j})}
   }
   {"done": false, "n": nz, "P": pz, "Q": qz, "D": D, "jacobi": j, "out": out.set("jacobi", j), "t0": t0}
}

fn fibonacci_prp_report(any n, any P=1, any Q=-1) dict {
   "Fibonacci/Lucas V probable-prime test: V_n(P,Q) == P mod n."
   def t0, nz = ticks(), _pc_z(n)
   def pz, qz = _pc_z(P), _pc_z(Q)
   mut out = _pc_report("fibonacci-prp", nz, {"P": pz, "Q": qz, "probable_prime": false})
   if pz <= Z(0) || !(qz == Z(1) || qz == Z(-1)) {
      return _pc_finish_with(out, t0, "invalid-parameters", {"reason": "expected P > 0 and Q = +/-1"})
   }
   if nz < Z(2) { return _pc_finish(out, t0, "composite") }
   if nz == Z(2) { return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true}) }
   if nz % Z(2) == Z(0) { return _pc_finish_with(out, t0, "composite", {"factor": Z(2)}) }
   def Vn = _pc_mod(lucas.lucas_v_mod(pz, nz, nz, qz), nz)
   if Vn == _pc_mod(pz, nz) {
      return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true, "V_n": Vn})
   }
   _pc_finish_with(out, t0, "composite", {"V_n": Vn, "expected": _pc_mod(pz, nz)})
}

fn fibonacci_prp(any n, any P=1, any Q=-1) bool {
   "Runs the fibonacci prp operation."
   def nz, pz, qz = _pc_z(n), _pc_z(P), _pc_z(Q)
   if pz <= Z(0) || !(qz == Z(1) || qz == Z(-1)) { return false }
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   _pc_mod(lucas.lucas_v_mod(pz, nz, nz, qz), nz) == _pc_mod(pz, nz)
}

fn lucas_prp_report(any n, any P, any Q) dict {
   "Lucas probable-prime test with explicit P,Q parameters."
   def setup = _pc_lucas_common_setup("lucas-prp", n, P, Q)
   if setup.get("done", false) { return setup.get("report") }
   def nz, pz, qz, j = setup.get("n"), setup.get("P"), setup.get("Q"), int(setup.get("jacobi"))
   def idx = nz - Z(j)
   def U = _pc_mod(lucas.lucas_u_mod(pz, idx, nz, qz), nz)
   def out = setup.get("out").merge({"index": idx, "U_index": U})
   if U == Z(0) {
      return _pc_finish_with(out, setup.get("t0"), "probable-prime", {"probable_prime": true})
   }
   _pc_finish(out, setup.get("t0"), "composite")
}

fn lucas_prp(any n, any P, any Q) bool {
   "Runs the lucas prp operation."
   def nz, pz, qz = _pc_z(n), _pc_z(P), _pc_z(Q)
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   def D = pz * pz - Z(4) * qz
   def g = gcd(Z(2) * qz * D, nz)
   if g != Z(1) { return false }
   def j = jacobi(D, nz)
   if j == 0 { return false }
   _pc_mod(lucas.lucas_u_mod(pz, nz - Z(j), nz, qz), nz) == Z(0)
}

fn _pc_strong_lucas_accept_bool(any n, any P, any Q, any d, int s) bool {
   def nz = _pc_z(n)
   def z0, z1, z2, z4 = Z(0), Z(1), Z(2), Z(4)
   def pz, qz = _pc_mod_z(P, nz), _pc_mod_z(Q, nz)
   def inv2 = inverse_mod(z2, nz)
   if inv2 == nil || inv2 == 0 {
      def uv = lucas.lucas_uv_mod(pz, qz, d, nz)
      return _pc_mod(uv[0], nz) == z0 || _pc_mod(uv[1], nz) == z0
   }
   def D = _pc_mod_z(pz * pz - z4 * qz, nz)
   def dz = _pc_z(d)
   mut U = z1
   mut V = pz
   mut Qk = qz
   mut b = bit_length(dz) - 2
   while b >= 0 {
      U = (U * V) % nz
      V = (V * V - z2 * Qk) % nz
      if V < z0 { V = V + nz }
      Qk = (Qk * Qk) % nz
      if ((dz >> b) & z1) != z0 {
         def Uo = ((pz * U + V) * inv2) % nz
         def Vo = ((D * U + pz * V) * inv2) % nz
         U, V = Uo, Vo
         Qk = (Qk * qz) % nz
      }
      b -= 1
   }
   if U == z0 || V == z0 { return true }
   mut r = 1
   while r < s {
      V = (V * V - z2 * Qk) % nz
      if V < z0 { V = V + nz }
      Qk = (Qk * Qk) % nz
      if V == z0 { return true }
      r += 1
   }
   false
}

fn _pc_strong_lucas_p1_accept_bool(any n, any Q, any D_raw, any d, int s) bool {
   "Strong Lucas acceptor specialized for Selfridge P=1."
   def nz = _pc_z(n)
   def z0, z1, z2 = Z(0), Z(1), Z(2)
   def qz = _pc_mod_z(Q, nz)
   if nz % z2 == z0 {
      def uv = lucas.lucas_uv_mod(z1, qz, d, nz)
      return _pc_mod(uv[0], nz) == z0 || _pc_mod(uv[1], nz) == z0
   }
   def inv2 = (nz + z1) / z2
   def D = _pc_z(D_raw)
   def dz = _pc_z(d)
   mut U, V, Qk = z1, z1, qz
   mut b = bit_length(dz) - 2
   while b >= 0 {
      U = (U * V) % nz
      V = (V * V - (Qk + Qk)) % nz
      if V < z0 { V = V + nz }
      Qk = (Qk * Qk) % nz
      if ((dz >> b) & z1) != z0 {
         def Uo = ((U + V) * inv2) % nz
         mut Vo = ((D * U + V) * inv2) % nz
         if Vo < z0 { Vo = Vo + nz }
         U, V = Uo, Vo
         Qk = (Qk * qz) % nz
      }
      b -= 1
   }
   if U == z0 || V == z0 { return true }
   mut r = 1
   while r < s {
      V = (V * V - (Qk + Qk)) % nz
      if V < z0 { V = V + nz }
      Qk = (Qk * Qk) % nz
      if V == z0 { return true }
      r += 1
   }
   false
}

fn strong_lucas_prp_report(any n, any P, any Q) dict {
   "Strong Lucas probable-prime test with explicit P,Q parameters."
   def setup = _pc_lucas_common_setup("strong-lucas-prp", n, P, Q)
   if setup.get("done", false) { return setup.get("report") }
   def nz, pz, qz, j = setup.get("n"), setup.get("P"), setup.get("Q"), int(setup.get("jacobi"))
   mut d, s = nz - Z(j), 0
   while d % Z(2) == Z(0) {
      d = d / Z(2)
      s += 1
   }
   def uv = lucas.lucas_uv_mod(pz, qz, d, nz)
   def U = _pc_mod(uv.get(0), nz)
   mut V = _pc_mod(uv.get(1), nz)
   mut Qk = power_mod(_pc_mod(qz, nz), d, nz)
   mut out = setup.get("out").merge({"d": d, "s": s, "U_d": U, "V_d": V})
   if U == Z(0) || V == Z(0) {
      return _pc_finish_with(out, setup.get("t0"), "probable-prime", {"probable_prime": true})
   }
   mut r = 1
   while r < s {
      V = _pc_mod(V * V - Z(2) * Qk, nz)
      Qk = _pc_mod(Qk * Qk, nz)
      if V == Z(0) {
         return _pc_finish_with(out.set("witness_round", r), setup.get("t0"), "probable-prime", {"probable_prime": true})
      }
      r += 1
   }
   _pc_finish(out, setup.get("t0"), "composite")
}

fn strong_lucas_prp(any n, any P, any Q) bool {
   "Runs the strong lucas prp operation."
   def nz, pz, qz = _pc_z(n), _pc_z(P), _pc_z(Q)
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   def D = pz * pz - Z(4) * qz
   def g = gcd(Z(2) * qz * D, nz)
   if g != Z(1) { return false }
   def j = jacobi(D, nz)
   if j == 0 { return false }
   mut d = nz - Z(j)
   mut s = 0
   while d % Z(2) == Z(0) {
      d = d / Z(2)
      s += 1
   }
   _pc_strong_lucas_accept_bool(nz, pz, qz, d, s)
}

fn extra_strong_lucas_prp_report(any n, any P=3) dict {
   "Extra-strong Lucas probable-prime test with Q fixed to 1."
   def setup = _pc_lucas_common_setup("extra-strong-lucas-prp", n, P, 1)
   if setup.get("done", false) { return setup.get("report") }
   def nz, pz, j = setup.get("n"), setup.get("P"), int(setup.get("jacobi"))
   mut d, s = nz - Z(j), 0
   while d % Z(2) == Z(0) {
      d = d / Z(2)
      s += 1
   }
   def uv = lucas.lucas_uv_mod(pz, Z(1), d, nz)
   def U = _pc_mod(uv.get(0), nz)
   mut V = _pc_mod(uv.get(1), nz)
   mut out = setup.get("out").merge({"d": d, "s": s, "U_d": U, "V_d": V})
   if U == Z(0) && (V == Z(2) || V == nz - Z(2)) {
      return _pc_finish_with(out, setup.get("t0"), "probable-prime", {"probable_prime": true, "witness": "extra-strong-u-v"})
   }
   mut r = 0
   while r < s - 1 {
      if V == Z(0) {
         return _pc_finish_with(out.set("witness_round", r), setup.get("t0"), "probable-prime", {"probable_prime": true})
      }
      V = _pc_mod(V * V - Z(2), nz)
      r += 1
   }
   _pc_finish(out, setup.get("t0"), "composite")
}

fn extra_strong_lucas_prp(any n, any P=3) bool {
   "Runs the extra strong lucas prp operation."
   def nz, pz = _pc_z(n), _pc_z(P)
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   def D = pz * pz - Z(4)
   def g = gcd(Z(2) * D, nz)
   if g != Z(1) { return false }
   def j = jacobi(D, nz)
   if j == 0 { return false }
   mut d = nz - Z(j)
   mut s = 0
   while d % Z(2) == Z(0) {
      d = d / Z(2)
      s += 1
   }
   def uv = lucas.lucas_uv_mod(pz, Z(1), d, nz)
   def U = _pc_mod(uv[0], nz)
   mut V = _pc_mod(uv[1], nz)
   if U == Z(0) && (V == Z(2) || V == nz - Z(2)) { return true }
   mut r = 0
   while r < s - 1 {
      if V == Z(0) { return true }
      V = _pc_mod(V * V - Z(2), nz)
      r += 1
   }
   false
}

fn _pc_selfridge_params(any n) dict {
   def nz = _pc_z(n)
   mut D, tries = Z(5), 0
   while tries < 10000 {
      def g = gcd(_pc_abs(D), nz)
      if _pc_nontrivial_factor(g, nz) {
         return {"status": "factor", "factor": g, "cofactor": nz / g, "D": D}
      }
      def j = jacobi(D, nz)
      if j == -1 {
         return {"status": "params", "D": D, "P": Z(1), "Q": (Z(1) - D) / Z(4), "tries": tries + 1}
      }
      if D > Z(0) { D = -(D + Z(2)) } else { D = -D + Z(2) }
      tries += 1
   }
   {"status": "missing", "tries": tries}
}

fn _pc_strong_lucas_selfridge_report(any n) dict {
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("strong-lucas-selfridge", nz, {"probable_prime": false})
   if nz < Z(2) { return _pc_finish(out, t0, "composite") }
   if nz % Z(2) == Z(0) { return _pc_finish_with(out, t0, "composite", {"factor": Z(2)}) }
   def params = _pc_selfridge_params(nz)
   if params.get("status", "") == "factor" {
      return _pc_finish_with(out, t0, "factor", {
            "D": params.get("D"), "factor": params.get("factor"), "cofactor": params.get("cofactor"),
      })
   }
   if params.get("status", "") != "params" {
      return _pc_finish_with(out, t0, "no-selfridge-params", {"params": params})
   }
   def P, Q = params.get("P"), params.get("Q")
   mut d, s = nz + Z(1), 0
   while d % Z(2) == Z(0) {
      d = d / Z(2)
      s += 1
   }
   out = out.merge({
         "D": params.get("D"), "P": P, "Q": Q,
         "d": d, "s": s, "selfridge_tries": params.get("tries", 0),
   })
   if _pc_strong_lucas_p1_accept_bool(nz, Q, params.get("D"), d, s) {
      return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true})
   }
   _pc_finish(out, t0, "composite")
}

fn _pc_strong_lucas_selfridge_bool(any n) bool {
   def nz = _pc_z(n)
   def z0, z1, z2, z4, z5 = Z(0), Z(1), Z(2), Z(4), Z(5)
   if nz < z2 { return false }
   if nz % z2 == z0 { return false }
   mut D = z5
   mut tries = 0
   mut Q = z0
   mut found = false
   while tries < 10000 && !found {
      def g = gcd(_pc_abs(D), nz)
      if _pc_nontrivial_factor(g, nz) { return false }
      def j = jacobi(D, nz)
      if j == -1 {
         Q = (z1 - D) / z4
         found = true
      } else {
         if D > z0 { D = -(D + z2) } else { D = -D + z2 }
         tries += 1
      }
   }
   if !found { return false }
   mut d = nz + z1
   mut s = 0
   while d % z2 == z0 {
      d = d / z2
      s += 1
   }
   _pc_strong_lucas_p1_accept_bool(nz, Q, D, d, s)
}

fn selfridge_prp_report(any n) dict {
   "Lucas-Selfridge probable-prime test using the standard Selfridge D sequence."
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("selfridge-prp", nz, {"probable_prime": false})
   if nz < Z(2) { return _pc_finish(out, t0, "composite") }
   if nz == Z(2) { return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true}) }
   if nz % Z(2) == Z(0) { return _pc_finish_with(out, t0, "composite", {"factor": Z(2)}) }
   if is_square(nz) == 1 {
      return _pc_finish_with(out, t0, "perfect-square", {"square_root": isqrt(nz)})
   }
   def params = _pc_selfridge_params(nz)
   if params.get("status", "") == "factor" {
      return _pc_finish_with(out, t0, "factor", {
            "D": params.get("D"), "factor": params.get("factor"), "cofactor": params.get("cofactor"),
      })
   }
   if params.get("status", "") != "params" {
      return _pc_finish_with(out, t0, "no-selfridge-params", {"params": params})
   }
   def luc = lucas_prp_report(nz, params.get("P"), params.get("Q"))
   out = out.merge({
         "D": params.get("D"), "P": params.get("P"), "Q": params.get("Q"),
         "selfridge_tries": params.get("tries", 0), "lucas": luc,
   })
   if luc.get("probable_prime", false) {
      return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true})
   }
   _pc_finish_with(out, t0, "composite", {"factor": luc.get("factor", nil)})
}

fn selfridge_prp(any n) bool {
   "Runs the selfridge prp operation."
   selfridge_prp_report(n).get("probable_prime", false)
}

fn strong_selfridge_prp_report(any n) dict {
   "Strong Lucas-Selfridge probable-prime test."
   def nz = _pc_z(n)
   if nz > Z(1) && is_square(nz) == 1 {
      def t0 = ticks()
      return _pc_finish_with(_pc_report("strong-selfridge-prp", nz, {"probable_prime": false}), t0, "perfect-square", {"square_root": isqrt(nz)})
   }
   _pc_strong_lucas_selfridge_report(nz).set("method", "strong-selfridge-prp")
}

fn strong_selfridge_prp(any n) bool {
   "Runs the strong selfridge prp operation."
   strong_selfridge_prp_report(n).get("probable_prime", false)
}

fn bpsw_prp_report(any n) dict {
   "Baillie-PSW probable-prime screen with strong base-2 and Lucas-Selfridge reports."
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("bpsw-prp", nz, {"probable_prime": false})
   if nz < Z(2) { return _pc_finish(out, t0, "composite") }
   if nz == Z(2) || nz == Z(3) {
      return _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true})
   }
   if nz % Z(2) == Z(0) { return _pc_finish_with(out, t0, "composite", {"factor": Z(2)}) }
   if is_square(nz) == 1 {
      return _pc_finish_with(out, t0, "perfect-square", {"square_root": isqrt(nz)})
   }
   def mr_ok = _pc_strong_prp_base2_bool(nz)
   def mr = {
      "method": "strong-prp", "n": nz, "base": Z(2),
      "probable_prime": mr_ok, "status": mr_ok ? "probable-prime" : "composite",
      "elapsed_ms": 0.0,
   }
   out = out.merge({"strong_base2": mr})
   if !mr_ok {
      return _pc_finish_with(out, t0, "base2-composite", {"factor": nil})
   }
   def luc = _pc_strong_lucas_selfridge_report(nz)
   out = out.merge({"strong_lucas_selfridge": luc})
   if !luc.get("probable_prime", false) {
      return _pc_finish_with(out, t0, "lucas-composite", {"factor": luc.get("factor", nil)})
   }
   _pc_finish_with(out, t0, "probable-prime", {"probable_prime": true})
}

fn bpsw_prp(any n) bool {
   "Return true when the Baillie-PSW probable-prime screen accepts n."
   if is_int(n) && int(n) <= 2147483647 { return is_prime(n) }
   def nz = _pc_z(n)
   def state = _pc_odd_candidate_state(nz)
   if state != 0 { return state > 0 }
   if nz == Z(3) { return true }
   if bit_length(nz) <= 31 { return is_prime(nz) }
   if bit_length(nz) <= 64 { return deterministic_miller_rabin64(nz) }
   if is_square(nz) == 1 { return false }
   if !_pc_strong_prp_base2_bool(nz) { return false }
   _pc_strong_lucas_selfridge_bool(nz)
}

fn strong_bpsw_prp_report(any n) dict {
   "Strong Baillie-PSW probable-prime screen."
   bpsw_prp_report(n).set("method", "strong-bpsw-prp")
}

fn strong_bpsw_prp(any n) bool {
   "Runs the strong bpsw prp operation."
   bpsw_prp(n)
}

fn _pc_pratt_witness_report(any n, list qs, int max_base) dict {
   def nz = _pc_z(n)
   mut a = Z(2)
   while a <= Z(max_base) && a < nz {
      def g0 = gcd(a, nz)
      if _pc_nontrivial_factor(g0, nz) {
         return {"status": "factor", "a": a, "factor": g0, "cofactor": nz / g0}
      }
      if g0 == Z(1) && power_mod(a, nz - Z(1), nz) == Z(1) {
         mut checks, ok, i = [], true, 0
         while i < qs.len {
            def q = _pc_z(qs.get(i))
            def x = power_mod(a, (nz - Z(1)) / q, nz)
            def g = gcd(x - Z(1), nz)
            checks = checks.append({"q": q, "pow": x, "gcd": g, "ok": g == Z(1)})
            if g != Z(1) { ok = false }
            if _pc_nontrivial_factor(g, nz) {
               return {
                  "status": "factor", "a": a, "q": q,
                  "factor": g, "cofactor": nz / g, "checks": checks,
               }
            }
            i += 1
         }
         if ok {
            return {"status": "witness", "a": a, "checks": checks}
         }
      }
      a += Z(1)
   }
   {"status": "missing", "max_base": max_base}
}

fn verify_pratt_certificate_report(any cert) dict {
   "Verify a recursive Pratt/Pocklington certificate without calling is_prime."
   def t0 = ticks()
   mut out = {"method": "verify-pratt-certificate", "prime": false, "proof_valid": false}
   def setup = _pc_verify_recursive_setup(cert, out, t0, "factors_n_minus_1", -1, "factor product does not equal n-1")
   if setup.get("done", false) { return setup.get("report") }
   out = setup.get("out")
   def nz = setup.get("n")
   def qs = setup.get("qs", [])
   def verified_nodes = setup.get("verified_nodes", 1)
   def witness = cert.get("witness", nil)
   if witness == nil {
      return _pc_finish_with(out, t0, "invalid", {"reason": "missing witness"})
   }
   def a = _pc_z(witness)
   if gcd(a, nz) != Z(1) {
      return _pc_finish_with(out, t0, "invalid", {"reason": "witness not coprime to n", "a": a})
   }
   if power_mod(a, nz - Z(1), nz) != Z(1) {
      return _pc_finish_with(out, t0, "invalid", {"reason": "witness fails Fermat relation", "a": a})
   }
   mut checks, i = [], 0
   while i < qs.len {
      def q = _pc_z(qs.get(i))
      def g = gcd(power_mod(a, (nz - Z(1)) / q, nz) - Z(1), nz)
      checks = checks.append({"q": q, "gcd": g, "ok": g == Z(1)})
      if g != Z(1) {
         return _pc_finish_with(out, t0, "invalid", {"reason": "witness fails prime-factor order check", "a": a, "q": q, "gcd": g, "checks": checks})
      }
      i += 1
   }
   _pc_finish_with(out, t0, "verified-prime", {
         "prime": true, "proof_valid": true, "witness": a,
         "checks": checks, "verified_nodes": verified_nodes,
   })
}

fn verify_pratt_certificate(any cert) bool {
   "Return true when verify_pratt_certificate_report accepts cert."
   verify_pratt_certificate_report(cert).get("proof_valid", false)
}

fn pratt_certificate_report(any n, int max_base=512, int max_depth=64) dict {
   "Build a recursive Pratt/Pocklington certificate when n-1 is fully factorable."
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("pratt-certificate", nz, {
         "max_base": max_base, "max_depth": max_depth, "prime": false,
   })
   def basic = _pc_certificate_trivial_case(out, t0, nz, {
         "prime": true, "witness": Z(1), "factors_n_minus_1": [],
         "subcertificates": [], "certificate_size": 1, "verified": true,
   })
   if basic != nil { return basic }
   def prep = _pc_recursive_factor_setup(out, t0, nz, max_base, max_depth, "factors_n_minus_1", nz - Z(1), "n-1 factorization incomplete")
   if prep.get("done", false) { return prep.get("report") }
   out = prep.get("out")
   def qs = prep.get("qs", [])
   def subcerts = prep.get("subcerts")
   def w = _pc_pratt_witness_report(nz, qs, max_base)
   out = out.set("witness_report", w)
   def wf = _pc_witness_setup_failure(out, t0, w, "no primitive-root witness within max_base")
   if wf != nil { return wf }
   out = out.merge({
         "witness": w.get("a"),
         "witness_checks": w.get("checks", []),
         "subcertificates": subcerts.get("subcertificates", []),
         "certificate_size": subcerts.get("certificate_size", 1),
   })
   def verified = verify_pratt_certificate_report(out)
   _pc_finish_verified_certificate(out, t0, verified)
}

fn pratt_certificate(any n, int max_base=512) bool {
   "Return true when pratt_certificate_report proves n prime."
   pratt_certificate_report(n, max_base).get("prime", false)
}

fn _pc_lucas_nplus1_witness_report(any n, list qs, int max_tries) dict {
   def nz = _pc_z(n)
   mut D = Z(5)
   mut tries = 0
   mut attempts = []
   while tries < max_tries {
      def gD = gcd(_pc_abs(D), nz)
      if _pc_nontrivial_factor(gD, nz) {
         return {"status": "factor", "D": D, "factor": gD, "cofactor": nz / gD, "tries": tries + 1}
      }
      if jacobi(D, nz) == -1 {
         def P = Z(1)
         def Q = (Z(1) - D) / Z(4)
         def g0 = gcd(Z(2) * Q * D, nz)
         if _pc_nontrivial_factor(g0, nz) {
            return {
               "status": "factor", "D": D, "P": P, "Q": Q,
               "factor": g0, "cofactor": nz / g0, "tries": tries + 1,
            }
         }
         if g0 == Z(1) {
            def uvN = lucas.lucas_uv_mod(P, Q, nz + Z(1), nz)
            def UN = _pc_mod(uvN.get(0), nz)
            def rank = _pc_lucas_rank_checks(nz, P, Q, qs)
            def checks = rank.get("checks", [])
            def factor = rank.get("factor", nil)
            if factor != nil {
               return {
                  "status": "factor", "D": D, "P": P, "Q": Q, "q": rank.get("q"),
                  "factor": factor, "cofactor": nz / factor, "checks": checks,
                  "tries": tries + 1,
               }
            }
            if UN == Z(0) && rank.get("ok", false) {
               return {
                  "status": "witness", "D": D, "P": P, "Q": Q,
                  "U_n_plus_1": UN, "checks": checks, "tries": tries + 1,
               }
            }
            if attempts.len < 8 {
               attempts = attempts.append({
                     "D": D, "P": P, "Q": Q, "U_n_plus_1": UN, "checks": checks,
               })
            }
         }
      }
      if D > Z(0) { D = -(D + Z(2)) } else { D = -D + Z(2) }
      tries += 1
   }
   {"status": "missing", "max_tries": max_tries, "attempts": attempts}
}

fn verify_lucas_nplus1_certificate_report(any cert) dict {
   "Verify a recursive Lucas n+1 certificate without calling is_prime."
   def t0 = ticks()
   mut out = {"method": "verify-lucas-nplus1-certificate", "prime": false, "proof_valid": false}
   def setup = _pc_verify_recursive_setup(cert, out, t0, "factors_n_plus_1", 1, "factor product does not equal n+1")
   if setup.get("done", false) { return setup.get("report") }
   out = setup.get("out")
   def nz = setup.get("n")
   def P, Q = _pc_z(cert.get("P", Z(0))), _pc_z(cert.get("Q", Z(0)))
   def D = _pc_z(cert.get("D", P * P - Z(4) * Q))
   out = out.merge({"P": P, "Q": Q, "D": D})
   if P * P - Z(4) * Q != D {
      return _pc_finish_with(out, t0, "invalid", {"reason": "D does not match P^2 - 4Q"})
   }
   if jacobi(D, nz) != -1 {
      return _pc_finish_with(out, t0, "invalid", {"reason": "Jacobi(D,n) is not -1"})
   }
   def g0 = gcd(Z(2) * Q * D, nz)
   out = out.set("discriminant_gcd", g0)
   if g0 != Z(1) {
      return _pc_finish_with(out, t0, "invalid", {"reason": "gcd(2QD,n) is not 1", "factor": _pc_nontrivial_factor(g0, nz) ? g0 : nil})
   }
   def qs, verified_nodes = setup.get("qs", []), setup.get("verified_nodes", 1)
   def uvN = lucas.lucas_uv_mod(P, Q, nz + Z(1), nz)
   def UN = _pc_mod(uvN.get(0), nz)
   out = out.set("U_n_plus_1", UN)
   if UN != Z(0) {
      return _pc_finish_with(out, t0, "invalid", {"reason": "U_(n+1) is not 0 mod n"})
   }
   def rank = _pc_lucas_rank_checks(nz, P, Q, qs)
   def checks = rank.get("checks", [])
   if !rank.get("ok", false) {
      return _pc_finish_with(out, t0, "invalid", {
            "reason": "Lucas rank check failed for a prime factor of n+1",
            "q": rank.get("q"), "gcd": rank.get("gcd"), "checks": checks,
            "factor": rank.get("factor", nil),
      })
   }
   _pc_finish_with(out, t0, "verified-prime", {
         "prime": true, "proof_valid": true,
         "checks": checks, "verified_nodes": verified_nodes,
   })
}

fn verify_lucas_nplus1_certificate(any cert) bool {
   "Return true when verify_lucas_nplus1_certificate_report accepts cert."
   verify_lucas_nplus1_certificate_report(cert).get("proof_valid", false)
}

fn lucas_nplus1_certificate_report(any n, int max_tries=128, int max_depth=64) dict {
   "Build a recursive Lucas n+1 certificate when n+1 is fully factorable."
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("lucas-nplus1-certificate", nz, {
         "max_tries": max_tries, "max_depth": max_depth, "prime": false,
   })
   def basic = _pc_certificate_trivial_case(out, t0, nz, {
         "prime": true, "D": Z(5), "P": Z(1), "Q": Z(-1),
         "factors_n_plus_1": [[Z(3), 1]],
         "subcertificates": [pratt_certificate_report(Z(3), max_tries, max_depth - 1)],
         "certificate_size": 2, "verified": true,
   })
   if basic != nil { return basic }
   def prep = _pc_recursive_factor_setup(out, t0, nz, max_tries, max_depth, "factors_n_plus_1", nz + Z(1), "n+1 factorization incomplete")
   if prep.get("done", false) { return prep.get("report") }
   out = prep.get("out")
   def qs = prep.get("qs", [])
   def subcerts = prep.get("subcerts")
   def w = _pc_lucas_nplus1_witness_report(nz, qs, max_tries)
   out = out.set("witness_report", w)
   def wf = _pc_witness_setup_failure(out, t0, w, "no Lucas n+1 parameters within max_tries", true)
   if wf != nil { return wf }
   out = out.merge({
         "D": w.get("D"), "P": w.get("P"), "Q": w.get("Q"),
         "U_n_plus_1": w.get("U_n_plus_1"),
         "rank_checks": w.get("checks", []),
         "subcertificates": subcerts.get("subcertificates", []),
         "certificate_size": subcerts.get("certificate_size", 1),
   })
   def verified = verify_lucas_nplus1_certificate_report(out)
   _pc_finish_verified_certificate(out, t0, verified)
}

fn lucas_nplus1_certificate(any n, int max_tries=128) bool {
   "Return true when lucas_nplus1_certificate_report proves n prime."
   lucas_nplus1_certificate_report(n, max_tries).get("prime", false)
}

fn _pc_witness_for_q(any n, any q, int max_base) dict {
   def nz, qz = _pc_z(n), _pc_z(q)
   mut a = Z(2)
   while a <= Z(max_base) && a < nz {
      if gcd(a, nz) == Z(1) && power_mod(a, nz - Z(1), nz) == Z(1) {
         def x = power_mod(a, (nz - Z(1)) / qz, nz)
         def g = gcd(x - Z(1), nz)
         if g == Z(1) {
            return {
               "q": qz, "a": a, "status": "witness",
               "pow_n_minus_1": Z(1), "gcd": g,
            }
         }
         if _pc_nontrivial_factor(g, nz) {
            return {
               "q": qz, "a": a, "status": "factor",
               "factor": g, "cofactor": nz / g,
            }
         }
      }
      a += Z(1)
   }
   {"q": qz, "status": "missing", "max_base": max_base}
}

fn pocklington_certificate_report(any n, int max_base=128) dict {
   "Build a Pocklington-style primality certificate when n-1 factors enough."
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("pocklington-certificate", nz, {"max_base": max_base})
   if nz < Z(2) {
      return _pc_finish_with(out, t0, "composite", {"prime": false, "reason": "n < 2"})
   }
   if nz == Z(2) {
      return _pc_finish_with(out, t0, "proven-prime", {"prime": true, "certificate": []})
   }
   if nz % Z(2) == Z(0) {
      return _pc_finish_with(out, t0, "composite", {
            "prime": false, "factor": Z(2), "cofactor": nz / Z(2),
      })
   }
   if !is_prime(nz) {
      return _pc_finish_with(out, t0, "composite", {
            "prime": false, "reason": "base primality screen rejected n",
      })
   }
   def facs = factor(nz - Z(1))
   def factor_fields = _pc_factorization_fields("factors_n_minus_1", facs, nz - Z(1))
   def complete = factor_fields.get("complete_factorization", false)
   out = out.merge(factor_fields)
   if !complete {
      return _pc_finish_with(out, t0, "inconclusive", {
            "prime": false, "reason": "n-1 factorization incomplete",
      })
   }
   def qs = _pc_unique_primes(facs)
   mut cert, i = [], 0
   while i < qs.len {
      def w = _pc_witness_for_q(nz, qs.get(i), max_base)
      if w.get("status", "") == "factor" {
         return _pc_finish_with(out, t0, "composite", {
               "prime": false, "factor": w.get("factor"),
               "cofactor": w.get("cofactor"), "certificate": cert,
         })
      }
      if w.get("status", "") != "witness" {
         return _pc_finish_with(out, t0, "inconclusive", {
               "prime": false, "missing_q": qs.get(i), "certificate": cert,
         })
      }
      cert = cert.append(w)
      i += 1
   }
   _pc_finish_with(out, t0, "proven-prime", {
         "prime": true, "certificate": cert, "certificate_size": cert.len,
   })
}

fn pocklington_certificate(any n, int max_base=128) bool {
   "Return true when pocklington_certificate_report proves primality."
   pocklington_certificate_report(n, max_base).get("prime", false)
}

fn primality_certificate_report(any n, int max_base=128) dict {
   "Return a primality screen plus proof report for factorization orchestration."
   def t0, nz = ticks(), _pc_z(n)
   mut out = _pc_report("primality-certificate", nz, {
         "max_base": max_base, "prime": false, "probable_prime": false,
   })
   def screen = bpsw_prp_report(nz)
   out = out.set("screen", screen)
   if !screen.get("probable_prime", false) {
      return _pc_finish_with(out, t0, "composite", {
            "reason": screen.get("status", "screen-rejected"),
            "factor": screen.get("factor", nil),
      })
   }
   out = out.set("probable_prime", true)
   def pratt = pratt_certificate_report(nz, max_base)
   out = out.set("pratt", pratt)
   def pratt_done = _pc_finish_certified_proof(out, t0, "pratt", pratt, pratt)
   if pratt_done != nil { return pratt_done }
   def lucas_np1 = lucas_nplus1_certificate_report(nz, max_base)
   out = out.set("lucas_nplus1", lucas_np1)
   def lucas_done = _pc_finish_certified_proof(out, t0, "lucas-nplus1", lucas_np1, lucas_np1)
   if lucas_done != nil { return lucas_done }
   def proof = pocklington_certificate_report(nz, max_base)
   out = out.set("proof", proof)
   def proof_done = _pc_finish_certified_proof(out, t0, "pocklington", proof, proof.get("certificate", []), false)
   if proof_done != nil { return proof_done }
   _pc_finish_with(out, t0, "probable-prime", {
         "proof_status": proof.get("status", "inconclusive"),
         "reason": "BPSW accepted but proof layer did not certify",
   })
}

#main {
   fn check_pratt_prime(any n, int max_base) any {
      def r = pratt_certificate_report(Z(n), max_base)
      assert(r.get("status", "") == "proven-prime", "Pratt certificate should prove prime")
      assert(r.get("prime", false) && r.get("verified", false), "Pratt certificate prime and verified flags")
      assert(verify_pratt_certificate(r), "Pratt verifier should accept certificate")
      assert(r.get("certificate_size", 0) > 0, "Pratt certificate should include recursive nodes")
   }
   fn check_lucas_nplus1_prime(any n, int max_tries) any {
      def r = lucas_nplus1_certificate_report(Z(n), max_tries)
      assert(r.get("status", "") == "proven-prime", "Lucas n+1 certificate should prove prime")
      assert(r.get("prime", false) && r.get("verified", false), "Lucas n+1 certificate prime and verified flags")
      assert(verify_lucas_nplus1_certificate(r), "Lucas n+1 verifier should accept certificate")
      assert(r.get("rank_checks", []).len > 0, "Lucas n+1 certificate should include rank checks")
   }
   check_pratt_prime(97, 128)
   check_pratt_prime(65537, 128)
   check_lucas_nplus1_prime(43, 128)
   check_lucas_nplus1_prime(1009, 256)
   def lucas43 = lucas_nplus1_certificate_report(Z(43), 128)
   assert(
      !verify_lucas_nplus1_certificate(lucas43.set("subcertificates", [])),
      "Lucas n+1 verifier rejects missing recursive proofs",
   )
   assert(!verify_lucas_nplus1_certificate(lucas43.set("D", Z(9))), "Lucas n+1 verifier rejects tampered parameters")
   def r97 = primality_certificate_report(Z(97), 128)
   assert(r97.get("status", "") == "proven-prime", "primality certificate proves 97")
   assert(r97.get("proof_system", "") == "pratt", "primality certificate prefers verified Pratt proof")
   assert(r97.get("verified", false), "primality certificate carries verifier result")
   def tampered = r97.get("certificate").set("witness", Z(2))
   assert(!verify_pratt_certificate(tampered), "Pratt verifier rejects tampered witness")
   def c91 = pratt_certificate_report(Z(91), 64)
   assert(!c91.get("prime", true), "Pratt certificate rejects composite")
   def l91 = lucas_nplus1_certificate_report(Z(91), 64)
   assert(!l91.get("prime", true), "Lucas n+1 certificate rejects composite")
   def c2047 = primality_certificate_report(Z(2047), 128)
   assert(c2047.get("status", "") == "composite", "BPSW layer rejects 2047")
   fn show(any n) any {
      def r = primality_certificate_report(n, 128)
      assert(r.get("status", "") != "", "primality demo report status")
   }
   fn expect(str name, bool cond) any {
      if !cond { panic("failed: " + name) }
   }
   show(Z(97))
   show(Z(1000003))
   show(Z(2047))
   def prime = Z(1000003)
   def composite = Z(21)
   def square = Z(121)
   expect("fermat prime", prp(prime, 2))
   expect("fermat carmichael can pass", prp(Z(341), 2))
   expect("euler prime", euler_prp(prime, 2))
   expect("strong prime", strong_prp(prime, 2))
   expect("strong catches composite", !strong_prp(composite, 2))
   expect("strong handles zero", strong_prp_report(0, 2).get("status", "") == "composite")
   expect("lucas explicit prime", lucas_prp(prime, 1, -1))
   expect("strong lucas explicit prime", strong_lucas_prp(prime, 1, -1))
   expect("fibonacci prime", fibonacci_prp(prime, 1, -1))
   expect("extra strong lucas prime", extra_strong_lucas_prp(prime, 3))
   expect("selfridge prime", selfridge_prp(prime))
   expect("strong selfridge prime", strong_selfridge_prp(prime))
   expect("strong bpsw prime", strong_bpsw_prp(prime))
   def sqr = selfridge_prp_report(square)
   expect("square rejected", sqr.get("status", "") == "perfect-square")
   def bpsw = strong_bpsw_prp_report(prime)
   expect("report method", bpsw.get("method", "") == "strong-bpsw-prp")
   expect("report nested mr", bpsw.get("strong_base2", nil) != nil)
   expect("report nested lucas", bpsw.get("strong_lucas_selfridge", nil) != nil)
   print("✓ std.math.crypto.factorization.primality self-test passed")
}