;; Keywords: gf galois-field finite-field math crypto
;; Finite-field arithmetic for prime fields, GF(2), and extension-field elements.
;; Reference: HAC Ch.2/11/14 (https://cacr.uwaterloo.ca/hac/about/chap2.pdf)
;; References:
;; - std.math.crypto
module std.math.crypto.gf(gfp_elem, gfp_add, gfp_sub, gfp_neg, gfp_mul, gfp_div, gfp_inv,
   gfp_pow, gfp_sqrt, gfp_is_qr, gfp_legendre, gfp_order, gfp_discrete_log_bsgs,
   gfpk_add, gfpk_sub, gfpk_neg, gfpk_mul,
   gfpk_mod, gfpk_pow, gfpk_inv, gfpk_eq, gfpk_is_zero, gfpk_scalar_mul, gfpk_degree, gfpk_frobenius,
   gf2_add, gf2_mul, gf2_mul_mod, gf2_mod,
   gf2_div_q, gf2_gcd, gf2_inv, gf2_pow, gf2_deg, gf2, gf2e, GF2, GF2Elem, gf2_field, gf2_elem,
   GF, GFElem, gf, gfe, gf_field, gf_elem,
   gf_trace, gf_norm, gf_conjugates, gf_minpoly, gf_frobenius,
   gf_multiplicative_order, gf_is_primitive, gf_primitive_element, gf_discrete_log,
   solve_gf2, num2vec, vec2num, _clone_list,
   GF2BVBitVec, gf2bv_bitvec,
GF2BVLinearSystem, gf2bv_linear_system, gf2bv_bv, gf2bv_linear)

use std.core
use std.core.primitives (bxor, bshl)
use std.math.nt
use std.math.bin (bit_count)

comptime template _gf_ctor_alias1(alias_name, target_name){
   fn ${alias_name}(any arg0) gf2 { ${target_name}(arg0) }
}

comptime template _gfe_ctor_alias2(alias_name, target_name){
   fn ${alias_name}(any arg0, any arg1) gfe { ${target_name}(arg0, arg1) }
}

comptime template _gf2e_ctor_alias2(alias_name, target_name){
   fn ${alias_name}(any arg0, any arg1) gf2e { ${target_name}(arg0, arg1) }
}

fn _gf_list_copy(list lst) list {
   def n = lst.len
   mut r, i = [], 0
   while(i < n){
      r = r.append(lst[i])
      i += 1
   }
   r
}

fn gfp_elem(any a, any p) any {
   "Normalise integer a into [0, p). Handles negative inputs."
   def r = a % p
   (r < 0) ? r + p : r
}

fn gfp_add(any a, any b, any p) any {
   "Add two GF(p) elements. Returns(a + b) mod p."
   gfp_elem(a + b, p)
}

fn gfp_sub(any a, any b, any p) any {
   "Subtract b from a in GF(p). Returns(a - b) mod p."
   gfp_elem(a - b, p)
}

fn gfp_neg(any a, any p) any {
   "Negate a GF(p) element. Returns(-a) mod p."
   gfp_elem(0 - a, p)
}

fn gfp_mul(any a, any b, any p) any {
   "Multiply two GF(p) elements. Returns(a * b) mod p."
   gfp_elem(a * b, p)
}

fn gfp_inv(any a, any p) any {
   "Compute modular inverse of a in GF(p) via extended Euclidean. Returns 0 if a = 0."
   def aa = gfp_elem(a, p)
   if(aa == 0){ return 0 }
   mut old_r = p
   mut r = aa
   mut old_s = 0
   mut s = 1
   while(r != 0){
      def q = old_r / r
      def tmp_r = r
      r = old_r - q * r
      old_r = tmp_r
      def tmp_s = s
      s = old_s - q * s
      old_s = tmp_s
   }
   gfp_elem(old_s, p)
}

fn gfp_div(any a, any b, any p) any {
   "Divide a by b in GF(p). Returns a * b^-1 mod p."
   gfp_mul(a, gfp_inv(b, p), p)
}

fn gfp_pow(any a, any e, any p) any {
   "Compute a^e in GF(p) using binary exponentiation. Handles negative exponents via inverse."
   if(e == 0){ return 1 }
   if(e < 0){ return gfp_pow(gfp_inv(a, p), 0 - e, p) }
   mut base = gfp_elem(a, p)
   mut exp = e
   mut result = 1
   while(exp > 0){
      if(exp & 1 != 0){ result = gfp_mul(result, base, p) }
      base = gfp_mul(base, base, p)
      exp = exp >> 1
   }
   result
}

fn gfp_legendre(any a, any p) int {
   "Legendre symbol(a|p). Returns 0 if p|a, 1 if a is a QR mod p, -1 if a is a QNR mod p."
   def aa = gfp_elem(a, p)
   if(aa == 0){ return 0 }
   def ls = gfp_pow(aa, (p - 1) / 2, p)
   if(ls == 1){ return 1 }
   -1
}

fn gfp_is_qr(any a, any p) bool {
   "Test whether a is a quadratic residue mod p(p odd prime). Returns true/false."
   gfp_legendre(a, p) >= 0
}

fn gfp_sqrt(any a, any p) any {
   "Compute a square root of a in GF(p) using Tonelli-Shanks.
   Returns r with r^2 = a mod p, or -1 if no square root exists.
   p must be an odd prime."
   def aa = gfp_elem(a, p)
   if(aa == 0){ return 0 }
   if(gfp_legendre(aa, p) != 1){ return -1 }
   if(p % 4 == 3){ return gfp_pow(aa, (p + 1) / 4, p) }
   mut Q, S = p - 1, 0
   while(Q % 2 == 0){
      Q = Q / 2
      S += 1
   }
   mut z = 2
   while(gfp_legendre(z, p) != -1){ z += 1 }
   mut M_ts, c_ts = S, gfp_pow(z, Q, p)
   mut t_ts, R_ts = gfp_pow(aa, Q, p), gfp_pow(aa, (Q + 1) / 2, p)
   while(true){
      if(t_ts == 1){ return R_ts }
      if(t_ts == 0){ return 0 }
      mut i_ts = 1
      mut tmp_ts = gfp_mul(t_ts, t_ts, p)
      while(tmp_ts != 1){
         tmp_ts = gfp_mul(tmp_ts, tmp_ts, p)
         i_ts += 1
      }
      mut b_ts, k_ts = c_ts, 0
      while(k_ts < M_ts - i_ts - 1){
         b_ts = gfp_mul(b_ts, b_ts, p)
         k_ts += 1
      }
      M_ts, c_ts = i_ts, gfp_mul(b_ts, b_ts, p)
      t_ts, R_ts = gfp_mul(t_ts, c_ts, p), gfp_mul(R_ts, b_ts, p)
   }
   R_ts
}

fn gfp_order(any g, any p) int {
   "Compute the multiplicative order of g in GF(p)^*. Returns smallest k > 0 with g^k = 1 mod p."
   if(gfp_elem(g, p) == 0){ return 0 }
   mut k = 1
   mut cur = gfp_elem(g, p)
   while(cur != 1){
      cur = gfp_mul(cur, g, p)
      k += 1
   }
   k
}

fn gfp_discrete_log_bsgs(any g, any h, any p) int {
   "Baby-step giant-step discrete log: find x in [0, p-1) with g^x = h mod p.
   Returns x or -1 if not found. Runs in O(sqrt(p)) time and space."
   def n = p
   mut m = isqrt(n)
   if(m * m < n){ m += 1 }
   mut table = dict()
   mut baby = 1
   mut j = 0
   while(j < m){
      table = table.set(to_str(baby), j + 1)
      baby = gfp_mul(baby, g, p)
      j += 1
   }
   def gm_inv = gfp_pow(g, 0 - m, p)
   mut giant = gfp_elem(h, p)
   mut i = 0
   while(i <= m){
      def found_j = table.get(to_str(giant), 0) - 1
      if(found_j >= 0){
         def x = i * m + found_j
         if(x < n){ return bigint_to_int(x) }
      }
      giant = gfp_mul(giant, gm_inv, p)
      i += 1
   }
   -1
}

fn gfpk_degree(list poly) int {
   "Return degree of polynomial(index of highest non-zero coeff), or -1 for zero poly."
   def n = poly.len
   mut i = n - 1
   while(i >= 0){
      if(poly.get(i) != 0){ return i }
      i = i - 1
   }
   -1
}

fn _gfpk_trim(list poly, any p) list {
   def n = poly.len
   mut result = []
   mut i = 0
   while(i < n){
      result = result.append(gfp_elem(poly.get(i), p))
      i += 1
   }
   mut sz = result.len
   while(sz > 1 && result.get(sz - 1) == 0){
      mut trimmed = []
      mut j = 0
      while(j < sz - 1){
         trimmed = trimmed.append(result.get(j))
         j += 1
      }
      result = trimmed
      sz = sz - 1
   }
   result
}

fn gfpk_is_zero(list a) bool {
   "Return true if polynomial a is the zero element."
   def n = a.len
   mut i = 0
   while(i < n){
      if(a.get(i) != 0){ return false }
      i += 1
   }
   true
}

fn gfpk_eq(list a, list b, any p) bool {
   "Test equality of two GF(p^k) elements after normalisation."
   def ta, tb = _gfpk_trim(a, p), _gfpk_trim(b, p)
   if(ta.len != tb.len){ return false }
   mut i = 0
   while(i < ta.len){
      if(ta.get(i) != tb.get(i)){ return false }
      i += 1
   }
   true
}

fn gfpk_neg(list a, any p) list {
   "Negate a GF(p^k) element coefficient-wise."
   mut result = []
   mut i = 0
   while(i < a.len){
      result = result.append(gfp_neg(a.get(i), p))
      i += 1
   }
   result
}

fn gfpk_add(list a, list b, any p) list {
   "Add two GF(p^k) elements. Returns sum."
   def na, nb = a.len, b.len
   def nmax = (na > nb) ? na : nb
   mut result = []
   mut i = 0
   while(i < nmax){
      def ai, bi = (i < na) ? a.get(i) : 0, (i < nb) ? b.get(i) : 0
      result = result.append(gfp_elem(ai + bi, p))
      i += 1
   }
   _gfpk_trim(result, p)
}

fn gfpk_sub(list a, list b, any p) list {
   "Subtract b from a in GF(p^k)."
   gfpk_add(a, gfpk_neg(b, p), p)
}

fn gfpk_scalar_mul(list a, any c, any p) list {
   "Multiply GF(p^k) element a by scalar c in GF(p)."
   def sc = gfp_elem(c, p)
   mut result = []
   mut i = 0
   while(i < a.len){
      result = result.append(gfp_mul(a.get(i), sc, p))
      i += 1
   }
   _gfpk_trim(result, p)
}

fn gfpk_mul(list a, list b, any p) list {
   "Multiply two GF(p^k) polynomials(raw, without reduction). Apply gfpk_mod to reduce."
   def na, nb = a.len, b.len
   if(na == 0 || nb == 0){ return [0] }
   def nr = na + nb - 1
   mut result = []
   mut i = 0
   while(i < nr){
      result = result.append(0)
      i += 1
   }
   i = 0
   while(i < na){
      def ai = gfp_elem(a.get(i), p)
      if(ai != 0){
         mut j = 0
         while(j < nb){
            def idx = i + j
            def cur = result.get(idx)
            def prod = gfp_mul(ai, b.get(j), p)
            result[idx] = gfp_elem(cur + prod, p)
            j += 1
         }
      }
      i += 1
   }
   _gfpk_trim(result, p)
}

fn gfpk_mod(list poly, list irred, any p) list {
   "Reduce polynomial poly modulo irreducible polynomial irred in GF(p)[x]."
   mut f = _gfpk_trim(poly, p)
   def deg_m = gfpk_degree(irred)
   if(deg_m <= 0){ return [0] }
   def inv_lead_m = gfp_inv(irred.get(deg_m), p)
   while(gfpk_degree(f) >= deg_m){
      def deg_f = gfpk_degree(f)
      def shift = deg_f - deg_m
      def lead_f = f.get(deg_f)
      def scale = gfp_mul(lead_f, inv_lead_m, p)
      mut i = 0
      while(i <= deg_m){
         def fidx = i + shift
         def old = f.get(fidx)
         def coeff = gfp_mul(scale, irred.get(i), p)
         f[fidx] = gfp_elem(old - coeff, p)
         i += 1
      }
      f = _gfpk_trim(f, p)
   }
   f
}

fn _gfpk_poly_divmod(list a, list b, any p) list {
   mut rem = _gfpk_trim(a, p)
   def deg_b = gfpk_degree(b)
   if(deg_b < 0){ return [[0], [0]] }
   def inv_lead_b = gfp_inv(b.get(deg_b), p)
   mut q = [0]
   while(gfpk_degree(rem) >= deg_b){
      def deg_rem = gfpk_degree(rem)
      def shift = deg_rem - deg_b
      def c = gfp_mul(rem.get(deg_rem), inv_lead_b, p)
      mut mono = []
      mut ki = 0
      while(ki < shift){
         mono = mono.append(0)
         ki += 1
      }
      mono = mono.append(c)
      q = gfpk_add(q, mono, p)
      mut sub_poly = []
      ki = 0
      while(ki < shift){
         sub_poly = sub_poly.append(0)
         ki += 1
      }
      def scaled_b = gfpk_mul([c], b, p)
      ki = 0
      while(ki < scaled_b.len){
         sub_poly = sub_poly.append(scaled_b.get(ki))
         ki += 1
      }
      rem = gfpk_sub(rem, sub_poly, p)
      rem = _gfpk_trim(rem, p)
   }
   [_gfpk_trim(q, p), rem]
}

fn gfpk_inv(list a, list irred, any p) list {
   "Compute inverse of a in GF(p)[x] / irred. Returns a^-1 or [0] if not invertible."
   def af = _gfpk_trim(a, p)
   if(gfpk_is_zero(af)){ return [0] }
   mut r0, r1 = irred, gfpk_mod(af, irred, p)
   mut t0, t1 = [0], [1]
   while(!gfpk_is_zero(r1)){
      def dm = _gfpk_poly_divmod(r0, r1, p)
      def q_poly = dm.get(0)
      def rem = dm.get(1)
      def tmp_r = r1
      r1, r0 = rem, tmp_r
      def tmp_t = t1
      t1, t0 = gfpk_sub(t0, gfpk_mod(gfpk_mul(q_poly, t1, p), irred, p), p), tmp_t
   }
   if(gfpk_degree(r0) > 0){ return [0] }
   def lead = r0.get(0)
   if(lead == 0){ return [0] }
   def inv_lead = gfp_inv(lead, p)
   gfpk_mod(gfpk_scalar_mul(t0, inv_lead, p), irred, p)
}

fn gfpk_pow(list a, any e, list irred, any p) list {
   "Compute a^e in GF(p^k) = GF(p)[x]/irred using square-and-multiply."
   if(e == 0){ return [1] }
   if(e < 0){ return gfpk_pow(gfpk_inv(a, irred, p), 0 - e, irred, p) }
   mut base = gfpk_mod(a, irred, p)
   mut exp = e
   mut result = [1]
   while(exp > 0){
      if(exp & 1 != 0){ result = gfpk_mod(gfpk_mul(result, base, p), irred, p) }
      base = gfpk_mod(gfpk_mul(base, base, p), irred, p)
      exp = exp >> 1
   }
   result
}

fn gfpk_frobenius(list a, list irred, any p) list {
   "Apply the Frobenius endomorphism: compute a^p in GF(p^k). Returns a^p mod irred."
   gfpk_pow(a, p, irred, p)
}

fn _gf2_xor(any a, any b) any {
   if(is_int(a) && is_int(b)){ return a ^^ b }
   bxor(a, b)
}

fn _gf2_shl(any a, int shift) any {
   if(is_int(a) && shift < 62){ return a << shift }
   bshl(a, shift)
}

fn _gf2_pow2(int shift) any {
   if(shift < 62){ return 1 << shift }
   bshl(bigint_from_int(1), shift)
}

fn gf2_deg(any a) int {
   "Degree of GF(2) polynomial encoded as integer(highest set bit). Returns -1 for 0."
   bit_length(a) - 1
}

fn gf2_add(any a, any b) any {
   "Add two GF(2) polynomials encoded as integers."
   _gf2_xor(a, b)
}

fn gf2_mul(any a, any b) any {
   "Carryless multiply in GF(2): returns a * b."
   mut result = 0
   mut va = a
   mut vb = b
   while(vb != 0){
      if(vb & 1 != 0){ result = _gf2_xor(result, va) }
      va, vb = _gf2_shl(va, 1), vb >> 1
   }
   result
}

fn gf2_mul_mod(any a, any b, any m) any {
   "Carryless multiply reduced modulo m in GF(2)."
   __bigint_gf2_mulmod(a, b, m)
}

fn gf2_mod(any a, any m) any {
   "Reduce polynomial a modulo m in GF(2). Returns a mod m."
   __bigint_gf2_mod(a, m)
}

fn gf2_div_q(any a, any b) any {
   "Quotient of GF(2) polynomial division a / b."
   def deg_b = gf2_deg(b)
   if(deg_b < 0){ return 0 }
   mut q = 0
   mut va = a
   mut va_deg = gf2_deg(va)
   while(va_deg >= deg_b){
      def shift = va_deg - deg_b
      q = _gf2_xor(q, _gf2_pow2(shift))
      va = _gf2_xor(va, _gf2_shl(b, shift))
      va_deg = gf2_deg(va)
   }
   q
}

fn gf2_gcd(any a, any b) any {
   "GCD of two GF(2) polynomials using Euclidean algorithm."
   mut va, vb = a, b
   while(vb != 0){
      def tmp = vb
      vb, va = gf2_mod(va, vb), tmp
   }
   va
}

fn gf2_inv(any a, any m) any {
   "Modular inverse of polynomial a modulo m in GF(2). Returns inverse or 0 if none."
   __bigint_gf2_inv(a, m)
}

fn gf2_pow(any a, any e, any m) any {
   "Compute a^e mod m in GF(2) using square-and-multiply."
   if(e < 0){ return gf2_pow(gf2_inv(a, m), 0 - e, m) }
   mut result = 1
   mut base = gf2_mod(a, m)
   mut exp = e
   while(exp > 0){
      if(exp & 1 != 0){ result = gf2_mul_mod(result, base, m) }
      base = gf2_mul_mod(base, base, m)
      exp = exp >> 1
   }
   result
}

impl gf2 {}

impl gf2e {}
fn GF2(any modulus) gf2 {
   "Create a GF(2^k) binary field from an irreducible modulus polynomial bit-vector."
   {"__type":"gf2", "modulus":modulus, "degree":gf2_deg(modulus)}
}

comptime emit _gf_ctor_alias1(gf2, GF2)
comptime emit _gf_ctor_alias1(gf2_field, GF2)

fn _gf2_field_modulus(any field_or_modulus) any {
   if(is_dict(field_or_modulus)){ return field_or_modulus.get("modulus", 0) }
   field_or_modulus
}

fn GF2Elem(any field_or_modulus, any value) gf2e {
   "Create a typed element in a GF(2^k) binary field."
   def modulus = _gf2_field_modulus(field_or_modulus)
   {"__type":"gf2e", "modulus":modulus, "value":gf2_mod(value, modulus)}
}

comptime emit _gf2e_ctor_alias2(gf2e, GF2Elem)
comptime emit _gf2e_ctor_alias2(gf2_elem, GF2Elem)

fn _gf2e_mod(any x) any { x.get("modulus", 0) }

fn _gf2e_val(any x) any { x.get("value", 0) }

fn _gf2e_is_elem(any x) bool { is_dict(x) && x.get("__type", "") == "gf2e" }

fn _gf2e_check_same(any a, any b) any {
   if(_gf2e_mod(a) != _gf2e_mod(b)){ panic("GF(2) element modulus mismatch") }
   _gf2e_mod(a)
}

fn _gf2_value_for(gf2 field, any x) any {
   if(_gf2e_is_elem(x)){
      def gf2e: e = x
      if(_gf2e_mod(e) != field.modulus){ panic("GF(2) element modulus mismatch") }
      return _gf2e_val(e)
   }
   gf2_mod(x, field.modulus)
}

comptime template _gf2e_scalar_overloads(name, helper){
   fn ${name}_int(gf2e a, int b) gf2e { helper(a, b) }
   fn ${name}_bigint(gf2e a, bigint b) gf2e { helper(a, b) }
}

fn _gf2e_add_scalar(gf2e a, any b) gf2e { GF2Elem(_gf2e_mod(a), gf2_add(_gf2e_val(a), b)) }

fn _gf2e_mul_scalar(gf2e a, any b) gf2e { GF2Elem(_gf2e_mod(a), gf2_mul_mod(_gf2e_val(a), b, _gf2e_mod(a))) }

fn _gf2e_pow_scalar(gf2e a, any e) gf2e {
   if(e < 0){ return _gf2e_pow_scalar(a.inv, 0 - e) }
   GF2Elem(_gf2e_mod(a), gf2_pow(_gf2e_val(a), e, _gf2e_mod(a)))
}

fn _gf2e_add_values(any a, any b, any _modulus) any { gf2_add(a, b) }

fn _gf2e_mul_values(any a, any b, any modulus) any { gf2_mul_mod(a, b, modulus) }

fn _gf2_generic_field(gf2 f) gf { GF(2, f.modulus) }

fn _gf2_generic_elem(gf2 f, any value) gfe { GFElem(_gf2_generic_field(f), value) }

fn _gf2_generic_for(gf2 f, any x) gfe { _gf2_generic_elem(f, _gf2_value_for(f, x)) }

fn _gf2e_as_gfe(gf2e a) gfe { _gf2_generic_elem(a.field, _gf2e_val(a)) }

fn _gf2e_from_gfe(gf2 f, gfe a) gf2e { GF2Elem(f, _gfe_value(a)) }

fn _gf2_frobenius_value(gf2 f, any value, int power=1) any { _gfe_value(gf_frobenius(_gf2_generic_elem(f, value), power)) }

fn _gf2_conjugates(gf2 f, any value) list {
   def vals = _gf_conjugate_values(_gf2_generic_elem(f, value), true)
   mut out = []
   mut i = 0
   while(i < vals.len){
      out = out.append(GF2Elem(f, vals.get(i)))
      i += 1
   }
   out
}

comptime template _gf2e_elem_binary_overload(name, helper){
   fn ${name}(gf2e a, gf2e b) gf2e {
      def modulus = _gf2e_check_same(a, b)
      GF2Elem(modulus, helper(_gf2e_val(a), _gf2e_val(b), modulus))
   }
}

impl gf2 {
   fn characteristic(gf2 f) int { 2 }
   fn p(gf2 f) int { 2 }
   fn modulus(gf2 f) any { f.get("modulus", 0) }
   fn degree(gf2 f) int { f.get("degree", gf2_deg(f.modulus)) }
   fn order(gf2 f) any { _gf_plain_pow(2, f.degree) }
   fn elem(gf2 f, any value) gf2e { GF2Elem(f, value) }
   fn zero(gf2 f) gf2e { GF2Elem(f, 0) }
   fn one(gf2 f) gf2e { GF2Elem(f, 1) }
   fn add(gf2 f, any a, any b) gf2e { GF2Elem(f, gf2_add(_gf2_value_for(f, a), _gf2_value_for(f, b))) }
   fn mul(gf2 f, any a, any b) gf2e { GF2Elem(f, gf2_mul_mod(_gf2_value_for(f, a), _gf2_value_for(f, b), f.modulus)) }
   fn inv(gf2 f, any a) gf2e { GF2Elem(f, gf2_inv(_gf2_value_for(f, a), f.modulus)) }
   fn pow(gf2 f, any a, any e) gf2e { GF2Elem(f, gf2_pow(_gf2_value_for(f, a), e, f.modulus)) }
   fn trace(gf2 f, any a) any { gf_trace(_gf2_generic_for(f, a)) }
   fn norm(gf2 f, any a) any { gf_norm(_gf2_generic_for(f, a)) }
   fn minpoly(gf2 f, any a) list { gf_minpoly(_gf2_generic_for(f, a)) }
   fn conjugates(gf2 f, any a) list { _gf2_conjugates(f, _gf2_value_for(f, a)) }
   fn frobenius(gf2 f, any a, int power=1) gf2e { GF2Elem(f, _gf2_frobenius_value(f, _gf2_value_for(f, a), power)) }
   fn primitive_element(gf2 f, any max_scan=nil) ?gf2e {
      def candidate = gf_primitive_element(_gf2_generic_field(f), max_scan)
      if(candidate == nil){ return nil }
      def gfe: ge = candidate
      _gf2e_from_gfe(f, ge)
   }
   fn discrete_log(gf2 f, any base, any target, any order=nil) any { gf_discrete_log(_gf2_generic_for(f, base), _gf2_generic_for(f, target), order) }
   fn log(gf2 f, any base, any target, any order=nil) any { f.discrete_log(base, target, order) }
}

impl gf2e {
   fn value(gf2e a) any { _gf2e_val(a) }
   fn modulus(gf2e a) any { _gf2e_mod(a) }
   fn field(gf2e a) gf2 { GF2(_gf2e_mod(a)) }
   fn hex(gf2e a) str { bigint_to_hex(_gf2e_val(a)) }
   fn str(gf2e a) str { bigint_to_str(_gf2e_val(a)) }
   fn add(gf2e a, gf2e b) gf2e {
      def modulus = _gf2e_check_same(a, b)
      GF2Elem(modulus, _gf2e_add_values(_gf2e_val(a), _gf2e_val(b), modulus))
   }
   fn add_int(gf2e a, int b) gf2e { _gf2e_add_scalar(a, b) }
   fn add_bigint(gf2e a, bigint b) gf2e { _gf2e_add_scalar(a, b) }
   fn mul(gf2e a, gf2e b) gf2e {
      def modulus = _gf2e_check_same(a, b)
      GF2Elem(modulus, _gf2e_mul_values(_gf2e_val(a), _gf2e_val(b), modulus))
   }
   fn mul_int(gf2e a, int b) gf2e { _gf2e_mul_scalar(a, b) }
   fn mul_bigint(gf2e a, bigint b) gf2e { _gf2e_mul_scalar(a, b) }
   fn inv(gf2e a) gf2e { GF2Elem(_gf2e_mod(a), gf2_inv(_gf2e_val(a), _gf2e_mod(a))) }
   fn div(gf2e a, gf2e b) gf2e { a.mul(b.inv) }
   fn pow_int(gf2e a, int b) gf2e { _gf2e_pow_scalar(a, b) }
   fn pow_bigint(gf2e a, bigint b) gf2e { _gf2e_pow_scalar(a, b) }
   fn trace(gf2e a) any { gf_trace(_gf2e_as_gfe(a)) }
   fn norm(gf2e a) any { gf_norm(_gf2e_as_gfe(a)) }
   fn minpoly(gf2e a) list { gf_minpoly(_gf2e_as_gfe(a)) }
   fn conjugates(gf2e a) list { _gf2_conjugates(a.field, _gf2e_val(a)) }
   fn frobenius(gf2e a, int power=1) gf2e { GF2Elem(_gf2e_mod(a), _gf2_frobenius_value(a.field, _gf2e_val(a), power)) }
   fn multiplicative_order(gf2e a) any { gf_multiplicative_order(_gf2e_as_gfe(a)) }
   fn is_primitive(gf2e a) bool { gf_is_primitive(_gf2e_as_gfe(a)) }
   fn discrete_log(gf2e a, gf2e target, any order=nil) any { gf_discrete_log(_gf2e_as_gfe(a), _gf2e_as_gfe(target), order) }
   fn log(gf2e a, gf2e target, any order=nil) any { gf_discrete_log(_gf2e_as_gfe(a), _gf2e_as_gfe(target), order) }
   fn same(gf2e a, gf2e b) bool { _gf2e_mod(a) == _gf2e_mod(b) && _gf2e_val(a) == _gf2e_val(b) }
   fn different(gf2e a, gf2e b) bool { !a.same(b) }
   operator + gf2e: gf2e = add
   operator + int: gf2e = add_int
   operator + bigint: gf2e = add_bigint
   operator - gf2e: gf2e = add
   operator - int: gf2e = add_int
   operator - bigint: gf2e = add_bigint
   operator * gf2e: gf2e = mul
   operator * int: gf2e = mul_int
   operator * bigint: gf2e = mul_bigint
   operator / gf2e: gf2e = div
   operator ^ int: gf2e = pow_int
   operator ^ bigint: gf2e = pow_bigint
   operator == gf2e: bool = same
   operator != gf2e: bool = different
}

impl gf {}

impl gfe {}
fn GF(any p, any modulus=nil) gf {
   "Create a finite field over GF(p), GF(p^k), or binary GF(2^k)."
   if(modulus == nil){ return {"__type":"gf", "p":p, "kind":"prime", "degree":1} }
   if(p == 2 && !is_list(modulus)){ return {"__type":"gf", "p":p, "kind":"binary", "modulus":modulus, "degree":gf2_deg(modulus)} }
   {"__type":"gf", "p":p, "kind":"poly", "modulus":modulus, "degree":gfpk_degree(modulus)}
}

fn gf(any p, any modulus=nil) gf {
   "Alias for GF."
   GF(p, modulus)
}

fn gf_field(any p, any modulus=nil) gf {
   "Alias for GF."
   GF(p, modulus)
}

fn _gf_kind(any x) str { x.get("kind", "prime") }

fn _gf_p(any x) any { x.get("p", 0) }

fn _gf_modulus(any x) any { x.get("modulus", nil) }

fn _gf_normalize_value(any field, any value) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return gfp_elem(value, _gf_p(field)) }
   if(kind == "binary"){ return gf2_mod(value, _gf_modulus(field)) }
   def p = _gf_p(field)
   def irred = _gf_modulus(field)
   if(is_list(value)){ return gfpk_mod(value, irred, p) }
   gfpk_mod([value], irred, p)
}

fn GFElem(any field, any value) gfe {
   "Create a typed element in a finite field created by `GF(...)`."
   {"__type":"gfe",
      "kind":_gf_kind(field),
      "p":_gf_p(field),
      "modulus":_gf_modulus(field),
   "value":_gf_normalize_value(field, value)}
}

comptime emit _gfe_ctor_alias2(gfe, GFElem)
comptime emit _gfe_ctor_alias2(gf_elem, GFElem)

fn _gfe_value(any x) any { x.get("value", 0) }

fn _gfe_kind(any x) str { x.get("kind", "prime") }

fn _gfe_p(any x) any { x.get("p", 0) }

fn _gfe_modulus(any x) any { x.get("modulus", nil) }

fn _gfe_field(any x) gf {
   def kind = _gfe_kind(x)
   if(kind == "prime"){ return GF(_gfe_p(x)) }
   GF(_gfe_p(x), _gfe_modulus(x))
}

fn _gfe_is_elem(any x) bool { is_dict(x) && x.get("__type", "") == "gfe" }

fn _gfe_check_same(gfe a, gfe b) gf {
   if(_gfe_kind(a) != _gfe_kind(b) || _gfe_p(a) != _gfe_p(b) || _gfe_modulus(a) != _gfe_modulus(b)){ panic("finite-field element field mismatch") }
   _gfe_field(a)
}

fn _gf_value_for(gf field, any x) any {
   if(_gfe_is_elem(x)){
      def gfe: e = x
      def gf: ef = _gfe_field(e)
      if(_gf_kind(field) != _gf_kind(ef) || _gf_p(field) != _gf_p(ef) || _gf_modulus(field) != _gf_modulus(ef)){ panic("finite-field element field mismatch") }
      return _gfe_value(e)
   }
   _gf_normalize_value(field, x)
}

fn _gf_add_values(gf field, any a, any b) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return gfp_add(a, b, _gf_p(field)) }
   if(kind == "binary"){ return gf2_add(a, b) }
   gfpk_add(a, b, _gf_p(field))
}

fn _gf_sub_values(gf field, any a, any b) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return gfp_sub(a, b, _gf_p(field)) }
   if(kind == "binary"){ return gf2_add(a, b) }
   gfpk_sub(a, b, _gf_p(field))
}

fn _gf_neg_value(gf field, any a) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return gfp_neg(a, _gf_p(field)) }
   if(kind == "binary"){ return a }
   gfpk_neg(a, _gf_p(field))
}

fn _gf_mul_values(gf field, any a, any b) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return gfp_mul(a, b, _gf_p(field)) }
   if(kind == "binary"){ return gf2_mul_mod(a, b, _gf_modulus(field)) }
   gfpk_mod(gfpk_mul(a, b, _gf_p(field)), _gf_modulus(field), _gf_p(field))
}

fn _gf_inv_value(gf field, any a) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return gfp_inv(a, _gf_p(field)) }
   if(kind == "binary"){ return gf2_inv(a, _gf_modulus(field)) }
   gfpk_inv(a, _gf_modulus(field), _gf_p(field))
}

fn _gf_pow_value(gf field, any a, any e) any {
   if(e < 0){ return _gf_pow_value(field, _gf_inv_value(field, a), 0 - e) }
   def kind = _gf_kind(field)
   if(kind == "prime"){ return gfp_pow(a, e, _gf_p(field)) }
   if(kind == "binary"){ return gf2_pow(a, e, _gf_modulus(field)) }
   gfpk_pow(a, e, _gf_modulus(field), _gf_p(field))
}

fn _gf_plain_pow(any base, any exp) any {
   mut result = 1
   mut b = base
   mut e = exp
   while(e > 0){
      if(e % 2 != 0){ result *= b }
      e = e / 2
      if(e > 0){ b *= b }
   }
   result
}

comptime template _gfe_scalar_overloads(name, helper){
   fn ${name}_int(gfe a, int b) gfe { helper(a, b) }
   fn ${name}_bigint(gfe a, bigint b) gfe { helper(a, b) }
}

fn _gf_div_values(gf field, any a, any b) any { _gf_mul_values(field, a, _gf_inv_value(field, b)) }

fn _gf_value_eq(gf field, any a, any b) bool {
   def kind = _gf_kind(field)
   if(kind == "poly"){ return gfpk_eq(a, b, _gf_p(field)) }
   a == b
}

fn _gf_base_scalar(gf field, any value) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return gfp_elem(value, _gf_p(field)) }
   if(kind == "binary"){ return value & 1 }
   if(!is_list(value)){ return gfp_elem(value, _gf_p(field)) }
   value.len == 0 ? 0 : gfp_elem(value.get(0), _gf_p(field))
}

fn _gf_zero_value(gf field) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return 0 }
   if(kind == "binary"){ return 0 }
   [0]
}

fn _gf_one_value(gf field) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return 1 }
   if(kind == "binary"){ return 1 }
   [1]
}

fn _gf_list_key(any value) str {
   if(!is_list(value)){ return "[" + to_str(value) + "]" }
   mut out = "["
   mut i = 0
   while(i < value.len){
      if(i > 0){ out += "," }
      out += to_str(value.get(i))
      i += 1
   }
   out + "]"
}

fn _gf_value_key(gf field, any value) str {
   def kind = _gf_kind(field)
   if(kind == "poly"){ return _gf_list_key(_gf_normalize_value(field, value)) }
   to_str(value)
}

fn _gf_value_from_index(gf field, any idx) any {
   def kind = _gf_kind(field)
   if(kind == "prime"){ return gfp_elem(idx, _gf_p(field)) }
   if(kind == "binary"){ return gf2_mod(idx, _gf_modulus(field)) }
   def p = _gf_p(field)
   def deg = field.degree
   mut coeffs = []
   mut x = Z(idx)
   mut i = 0
   while(i < deg){
      coeffs = coeffs.append(x % Z(p))
      x = x / Z(p)
      i += 1
   }
   _gf_normalize_value(field, coeffs)
}

fn _gf_group_order(gf field) bigint { Z(field.order) - Z(1) }

fn _gf_conjugate_values(gfe a, bool unique=true) list {
   "Return Frobenius conjugate raw values for a finite-field element."
   def gf: f = _gfe_field(a)
   def deg = f.degree
   mut out = []
   mut cur = _gfe_value(a)
   mut i = 0
   while(i < deg){
      if(unique){
         mut seen = false
         mut j = 0
         while(j < out.len){
            if(_gf_value_eq(f, out.get(j), cur)){ seen = true }
            j += 1
         }
         if(seen){ return out }
      }
      out = out.append(cur)
      cur = _gf_pow_value(f, cur, _gf_p(f))
      i += 1
   }
   out
}

fn gf_conjugates(gfe a) list {
   "Return the distinct Frobenius conjugates of a finite-field element."
   def gf: f = _gfe_field(a)
   def vals = _gf_conjugate_values(a, true)
   mut out = []
   mut i = 0
   while(i < vals.len){
      out = out.append(GFElem(f, vals.get(i)))
      i += 1
   }
   out
}

fn gf_frobenius(gfe a, int power=1) gfe {
   "Apply the Frobenius automorphism `power` times: a -> a^(p^power)."
   def gf: f = _gfe_field(a)
   def deg = f.degree
   mut steps = power
   if(deg > 0){
      steps = power % deg
      if(steps < 0){ steps += deg }
   }
   mut cur = _gfe_value(a)
   mut i = 0
   while(i < steps){
      cur = _gf_pow_value(f, cur, _gf_p(f))
      i += 1
   }
   GFElem(f, cur)
}

fn gf_trace(gfe a) any {
   "Return the field trace down to the prime field as a scalar."
   def gf: f = _gfe_field(a)
   def deg = f.degree
   mut acc = _gf_zero_value(f)
   mut cur = _gfe_value(a)
   mut i = 0
   while(i < deg){
      acc = _gf_add_values(f, acc, cur)
      cur = _gf_pow_value(f, cur, _gf_p(f))
      i += 1
   }
   _gf_base_scalar(f, acc)
}

fn gf_norm(gfe a) any {
   "Return the field norm down to the prime field as a scalar."
   def gf: f = _gfe_field(a)
   if(f.degree <= 1){ return _gf_base_scalar(f, _gfe_value(a)) }
   if(_gf_value_eq(f, _gfe_value(a), _gf_zero_value(f))){ return 0 }
   def exp = (f.order - Z(1)) / (Z(_gf_p(f)) - Z(1))
   _gf_base_scalar(f, _gf_pow_value(f, _gfe_value(a), exp))
}

fn gf_minpoly(gfe a) list {
   "Return the monic minimal polynomial of a over the prime field, low coefficients first."
   def gf: f = _gfe_field(a)
   def vals = _gf_conjugate_values(a, true)
   mut coeffs = [_gf_one_value(f)]
   mut i = 0
   while(i < vals.len){
      def root = vals.get(i)
      mut next = []
      mut j = 0
      while(j < coeffs.len + 1){
         next = next.append(_gf_zero_value(f))
         j += 1
      }
      j = 0
      while(j < coeffs.len){
         def c = coeffs.get(j)
         next[j] = _gf_sub_values(f, next.get(j), _gf_mul_values(f, c, root))
         next[j + 1] = _gf_add_values(f, next.get(j + 1), c)
         j += 1
      }
      coeffs = next
      i += 1
   }
   mut out = []
   i = 0
   while(i < coeffs.len){
      out = out.append(_gf_base_scalar(f, coeffs.get(i)))
      i += 1
   }
   out
}

fn gf_multiplicative_order(gfe a) bigint {
   "Return the multiplicative order of a finite-field element. Zero has order 0."
   def gf: f = _gfe_field(a)
   def av = _gfe_value(a)
   if(_gf_value_eq(f, av, _gf_zero_value(f))){ return Z(0) }
   mut ord = _gf_group_order(f)
   def factors = factor(ord, false, false)
   mut i = 0
   while(i < factors.len){
      def q = Z(factors.get(i).get(0))
      while(ord % q == Z(0)){
         def candidate = ord / q
         if(_gf_value_eq(f, _gf_pow_value(f, av, candidate), _gf_one_value(f))){
            ord = candidate
         } else {
            break
         }
      }
      i += 1
   }
   ord
}

fn gf_is_primitive(gfe a) bool {
   "Return true when the element generates the finite field's multiplicative group."
   def gf: f = _gfe_field(a)
   gf_multiplicative_order(a) == _gf_group_order(f)
}

fn gf_primitive_element(gf f, any max_scan=nil) ?gfe {
   "Find a generator of the finite field's multiplicative group by scanning field elements.
   Returns nil if no generator is found before max_scan candidates."
   def group_order = _gf_group_order(f)
   if(group_order <= Z(0)){ return nil }
   mut idx = Z(1)
   mut scanned = Z(0)
   while(idx < Z(f.order)){
      if(max_scan != nil && scanned >= Z(max_scan)){ return nil }
      def value = _gf_value_from_index(f, idx)
      def elem = GFElem(f, value)
      if(gf_multiplicative_order(elem) == group_order){ return elem }
      idx = idx + Z(1)
      scanned = scanned + Z(1)
   }
   nil
}

fn gf_discrete_log(gfe base, gfe target, any order=nil) any {
   "Baby-step giant-step discrete log over a finite-field multiplicative group.
   Returns x with base^x = target, or -1 if no log is found."
   def gf: f = _gfe_check_same(base, target)
   def bv, tv = _gfe_value(base), _gfe_value(target)
   if(_gf_value_eq(f, tv, _gf_one_value(f))){ return Z(0) }
   if(_gf_value_eq(f, bv, _gf_zero_value(f)) || _gf_value_eq(f, tv, _gf_zero_value(f))){ return -1 }
   def ord = order == nil ? gf_multiplicative_order(base) : Z(order)
   def m = isqrt(ord) + Z(1)
   mut table = dict()
   mut cur = _gf_one_value(f)
   mut i = Z(0)
   while(i < m){
      def key = _gf_value_key(f, cur)
      if(!table.contains(key)){ table = table.set(key, i + Z(1)) }
      cur = _gf_mul_values(f, cur, bv)
      i = i + Z(1)
   }
   def factor = _gf_pow_value(f, bv, 0 - m)
   mut gamma = tv
   mut j = Z(0)
   while(j <= m){
      def key = _gf_value_key(f, gamma)
      if(table.contains(key)){
         def x = j * m + table.get(key) - Z(1)
         if(x < ord && _gf_value_eq(f, _gf_pow_value(f, bv, x), tv)){ return x }
      }
      gamma = _gf_mul_values(f, gamma, factor)
      j = j + Z(1)
   }
   -1
}

comptime template _gf_field_binary_overload(name, helper){
   fn ${name}(gf f, any a, any b) gfe {
      def av, bv = _gf_value_for(f, a), _gf_value_for(f, b)
      GFElem(f, helper(f, av, bv))
   }
}

fn _gfe_div_values(gf field, any a, any b) any { _gf_mul_values(field, a, _gf_inv_value(field, b)) }

comptime template _gfe_elem_binary_overload(name, helper){
   fn ${name}(gfe a, gfe b) gfe {
      def gf: f = _gfe_check_same(a, b)
      def av, bv = _gfe_value(a), _gfe_value(b)
      GFElem(f, helper(f, av, bv))
   }
}

comptime template _gfe_scalar_binary(name, helper){
   fn ${name}(gfe a, any b) gfe {
      def gf: f = _gfe_field(a)
      def av, bv = _gfe_value(a), _gf_value_for(f, b)
      GFElem(f, helper(f, av, bv))
   }
}

comptime emit _gfe_scalar_binary(_gfe_add_scalar, _gf_add_values)
comptime emit _gfe_scalar_binary(_gfe_sub_scalar, _gf_sub_values)
comptime emit _gfe_scalar_binary(_gfe_mul_scalar, _gf_mul_values)

fn _gfe_pow_scalar(gfe a, any e) gfe {
   def gf: f = _gfe_field(a)
   GFElem(f, _gf_pow_value(f, _gfe_value(a), e))
}

impl gf {
   fn characteristic(gf f) any { _gf_p(f) }
   fn p(gf f) any { _gf_p(f) }
   fn kind(gf f) str { _gf_kind(f) }
   fn modulus(gf f) any { _gf_modulus(f) }
   fn degree(gf f) int { f.get("degree", 1) }
   fn order(gf f) any {
      def deg = f.degree
      if(deg <= 1){ return _gf_p(f) }
      _gf_plain_pow(_gf_p(f), deg)
   }
   fn elem(gf f, any value) gfe { GFElem(f, value) }
   fn zero(gf f) gfe { GFElem(f, 0) }
   fn one(gf f) gfe { GFElem(f, 1) }
   fn add(gf f, any a, any b) gfe {
      def av, bv = _gf_value_for(f, a), _gf_value_for(f, b)
      GFElem(f, _gf_add_values(f, av, bv))
   }
   fn sub(gf f, any a, any b) gfe {
      def av, bv = _gf_value_for(f, a), _gf_value_for(f, b)
      GFElem(f, _gf_sub_values(f, av, bv))
   }
   fn neg(gf f, any a) gfe {
      def av, rv = _gf_value_for(f, a), _gf_neg_value(f, av)
      GFElem(f, rv)
   }
   fn mul(gf f, any a, any b) gfe {
      def av, bv = _gf_value_for(f, a), _gf_value_for(f, b)
      GFElem(f, _gf_mul_values(f, av, bv))
   }
   fn inv(gf f, any a) gfe {
      def av, rv = _gf_value_for(f, a), _gf_inv_value(f, av)
      GFElem(f, rv)
   }
   fn div(gf f, any a, any b) gfe {
      def av, bv = _gf_value_for(f, a), _gf_value_for(f, b)
      GFElem(f, _gf_div_values(f, av, bv))
   }
   fn pow(gf f, any a, any e) gfe {
      def av, rv = _gf_value_for(f, a), _gf_pow_value(f, av, e)
      GFElem(f, rv)
   }
   fn trace(gf f, any a) any { gf_trace(GFElem(f, _gf_value_for(f, a))) }
   fn norm(gf f, any a) any { gf_norm(GFElem(f, _gf_value_for(f, a))) }
   fn minpoly(gf f, any a) list { gf_minpoly(GFElem(f, _gf_value_for(f, a))) }
   fn conjugates(gf f, any a) list { gf_conjugates(GFElem(f, _gf_value_for(f, a))) }
   fn frobenius(gf f, any a, int power=1) gfe { gf_frobenius(GFElem(f, _gf_value_for(f, a)), power) }
   fn primitive_element(gf f, any max_scan=nil) ?gfe { gf_primitive_element(f, max_scan) }
   fn discrete_log(gf f, any base, any target, any order=nil) any { gf_discrete_log(GFElem(f, _gf_value_for(f, base)), GFElem(f, _gf_value_for(f, target)), order) }
   fn log(gf f, any base, any target, any order=nil) any { f.discrete_log(base, target, order) }
}

impl gfe {
   fn value(gfe a) any { _gfe_value(a) }
   fn field(gfe a) gf { _gfe_field(a) }
   fn characteristic(gfe a) any { _gfe_p(a) }
   fn p(gfe a) any { _gfe_p(a) }
   fn modulus(gfe a) any { _gfe_modulus(a) }
   fn kind(gfe a) str { _gfe_kind(a) }
   fn hex(gfe a) str {
      def v = _gfe_value(a)
      is_list(v) ? to_str(v) : bigint_to_hex(v)
   }
   fn str(gfe a) str { to_str(_gfe_value(a)) }
   fn add(gfe a, gfe b) gfe {
      def gf: f = _gfe_check_same(a, b)
      GFElem(f, _gf_add_values(f, _gfe_value(a), _gfe_value(b)))
   }
   fn add_int(gfe a, int b) gfe { _gfe_add_scalar(a, b) }
   fn add_bigint(gfe a, bigint b) gfe { _gfe_add_scalar(a, b) }
   fn sub(gfe a, gfe b) gfe {
      def gf: f = _gfe_check_same(a, b)
      GFElem(f, _gf_sub_values(f, _gfe_value(a), _gfe_value(b)))
   }
   fn sub_int(gfe a, int b) gfe { _gfe_sub_scalar(a, b) }
   fn sub_bigint(gfe a, bigint b) gfe { _gfe_sub_scalar(a, b) }
   fn mul(gfe a, gfe b) gfe {
      def gf: f = _gfe_check_same(a, b)
      GFElem(f, _gf_mul_values(f, _gfe_value(a), _gfe_value(b)))
   }
   fn mul_int(gfe a, int b) gfe { _gfe_mul_scalar(a, b) }
   fn mul_bigint(gfe a, bigint b) gfe { _gfe_mul_scalar(a, b) }
   fn div(gfe a, gfe b) gfe {
      def gf: f = _gfe_check_same(a, b)
      GFElem(f, _gfe_div_values(f, _gfe_value(a), _gfe_value(b)))
   }
   fn inv(gfe a) gfe {
      def gf: f = _gfe_field(a)
      def av, rv = _gfe_value(a), _gf_inv_value(f, av)
      GFElem(f, rv)
   }
   fn pow_int(gfe a, int b) gfe { _gfe_pow_scalar(a, b) }
   fn pow_bigint(gfe a, bigint b) gfe { _gfe_pow_scalar(a, b) }
   fn trace(gfe a) any { gf_trace(a) }
   fn norm(gfe a) any { gf_norm(a) }
   fn minpoly(gfe a) list { gf_minpoly(a) }
   fn conjugates(gfe a) list { gf_conjugates(a) }
   fn frobenius(gfe a, int power=1) gfe { gf_frobenius(a, power) }
   fn multiplicative_order(gfe a) bigint { gf_multiplicative_order(a) }
   fn is_primitive(gfe a) bool { gf_is_primitive(a) }
   fn discrete_log(gfe a, gfe target, any order=nil) any { gf_discrete_log(a, target, order) }
   fn log(gfe a, gfe target, any order=nil) any { gf_discrete_log(a, target, order) }
   fn same(gfe a, gfe b) bool {
      _gfe_kind(a) == _gfe_kind(b) && _gfe_p(a) == _gfe_p(b) &&
      _gfe_modulus(a) == _gfe_modulus(b) && _gfe_value(a) == _gfe_value(b)
   }
   fn different(gfe a, gfe b) bool { !a.same(b) }
   operator + gfe: gfe = add
   operator + int: gfe = add_int
   operator + bigint: gfe = add_bigint
   operator - gfe: gfe = sub
   operator - int: gfe = sub_int
   operator - bigint: gfe = sub_bigint
   operator * gfe: gfe = mul
   operator * int: gfe = mul_int
   operator * bigint: gfe = mul_bigint
   operator / gfe: gfe = div
   operator ^ int: gfe = pow_int
   operator ^ bigint: gfe = pow_bigint
   operator == gfe: bool = same
   operator != gfe: bool = different
}

fn _clone_list(list lst) list { _gf_list_copy(lst) }

fn num2vec(any x, int w) list {
   "Convert integer x to binary vector of width w(MSB first)."
   mut result = []
   mut i = w - 1
   while(i >= 0){
      result = result.append((x >> i) & 1)
      i = i - 1
   }
   result
}

fn vec2num(list v) int {
   "Convert binary vector to integer(MSB first)."
   mut result = 0
   def n = v.len
   mut i = 0
   while(i < n){
      result = (result << 1) | v[i]
      i += 1
   }
   result
}

fn solve_gf2(list A, list b) any {
   "Solve Ax = b over GF(2) via Gaussian elimination. Returns x or nil if inconsistent."
   def nr, nc = A.len, len(A[0])
   mut M, i = [], 0
   while(i < nr){
      mut row = _clone_list(A[i])
      row = row.append(b[i])
      M = M.append(row)
      i += 1
   }
   mut pivot_row = 0
   mut pivot_col = 0
   while(pivot_row < nr && pivot_col < nc){
      mut r = pivot_row
      mut found = false
      while(r < nr){
         if(M[r][pivot_col] == 1){
            def tmp = M[pivot_row]
            M[pivot_row] = M[r]
            M[r] = tmp
            found = true
            break
         }
         r += 1
      }
      if(found){
         mut j = 0
         while(j < nr){
            if(j != pivot_row && M[j][pivot_col] == 1){
               def rj, rp = M[j], M[pivot_row]
               mut k = pivot_col
               while(k <= nc){
                  rj[k] = rj[k] ^^ rp[k]
                  k += 1
               }
            }
            j += 1
         }
         pivot_row += 1
      }
      pivot_col += 1
   }
   mut x = []
   i = 0
   while(i < nc){
      x = x.append(0)
      i += 1
   }
   i = 0
   while(i < nr){
      mut first_one = -1
      mut j = 0
      while(j < nc){
         if(M[i][j] == 1){
            first_one = j
            break
         }
         j += 1
      }
      if(first_one != -1){ x[first_one] = M[i][nc] } else { if(M[i][nc] == 1){ return nil } }
      i += 1
   }
   x
}

impl gf2bv_bv {}

impl gf2bv_linear {}
fn _gf2bv_fail(str msg) any { panic("std.math.crypto.gf.gf2bv: " + msg) }

fn GF2BVBitVec(any bits) gf2bv_bv {
   "Create a symbolic GF(2) bit-vector."
   mut out = []
   if(is_list(bits) || is_tuple(bits)){ out = _clone_list(bits) } elif(is_bytes(bits)){
      mut i = 0
      while(i < bits.len){
         out = out.append(load8(bits, i))
         i += 1
      }
   } else {
      _gf2bv_fail("bits must be a list")
   }
   {"__type":"gf2bv_bv", "bits":out}
}

fn gf2bv_bitvec(any bits) gf2bv_bv {
   "Alias for GF2BVBitVec."
   GF2BVBitVec(bits)
}

fn _gf2bv_bits(gf2bv_bv bv) list {
   if(!is_dict(bv) || bv.get("__type", "") != "gf2bv_bv"){ _gf2bv_fail("invalid bitvec") }
   bv.get("bits", [])
}

fn _gf2bv_to_bits_le(int n, any num) list {
   mut out = []
   mut i = 0
   while(i < n){
      out = out.append((num >> i) & 1)
      i += 1
   }
   out
}

fn _gf2bv_fill(int n, any v) list {
   mut out = []
   mut i = 0
   while(i < n){
      out = out.append(v)
      i += 1
   }
   out
}

fn _gf2bv_append_fill(list xs, int n, any v) list {
   mut out = _clone_list(xs)
   mut i = 0
   while(i < n){
      out = out.append(v)
      i += 1
   }
   out
}

fn _gf2bv_slice_list(list xs, int start, int stop) list {
   mut s, e = start, stop
   if(s < 0){ s = 0 }
   if(e < s){ e = s }
   if(e > xs.len){ e = xs.len }
   mut out = []
   mut i = s
   while(i < e){
      out = out.append(xs.get(i, 0))
      i += 1
   }
   out
}

fn _gf2bv_all_ones(list bits) bool {
   mut i = 0
   while(i < bits.len){
      if(!bits.get(i, 0)){ return false }
      i += 1
   }
   true
}

fn _gf2bv_join(list a, list b) list {
   mut out = _clone_list(a)
   mut i = 0
   while(i < b.len){
      out = out.append(b.get(i, 0))
      i += 1
   }
   out
}

fn _gf2bv_xor_bits(list a, list b) list {
   if(a.len != b.len){ _gf2bv_fail("cannot xor bit-vectors of different sizes") }
   mut out = []
   mut i = 0
   while(i < a.len){
      out = out.append((a.get(i, 0) ^^ b.get(i, 0)))
      i += 1
   }
   out
}

fn _gf2bv_tuple_where(list cond, any a, any b) list {
   mut out = []
   mut i = 0
   while(i < cond.len){
      if(cond.get(i, 0)){ out = out.append(is_list(a) ? a.get(i, 0) : a) }
      else { out = out.append(is_list(b) ? b.get(i, 0) : b) }
      i += 1
   }
   out
}

fn _gf2bv_pack_solution(list sizes, any sol) any {
   if(sol == nil || !is_list(sol)){ _gf2bv_fail("solution must be a list") }
   if(sol.len != sizes.len){ _gf2bv_fail("solution arity mismatch") }
   mut raw = 0
   mut shift = 0
   mut i = 0
   while(i < sizes.len){
      def sz = sizes.get(i, 0)
      def mask = (sz <= 0) ? 0 : ((1 << sz) - 1)
      raw = raw | ((sol.get(i, 0) & mask) << shift)
      shift += sz
      i += 1
   }
   raw
}

fn _gf2bv_convert_raw(list sizes, any raw) list {
   mut out = []
   mut s = raw
   mut i = 0
   while(i < sizes.len){
      def sz = sizes.get(i, 0)
      def mask = (sz <= 0) ? 0 : ((1 << sz) - 1)
      out = out.append(s & mask)
      s = s >> sz
      i += 1
   }
   out
}

fn _gf2bv_parse_zeros(list zeros) list {
   mut eqs = []
   mut i = 0
   while(i < zeros.len){
      def z = zeros.get(i, 0)
      if(is_dict(z) && z.get("__type", "") == "gf2bv_bv"){
         def bits = _gf2bv_bits(z)
         mut j = 0
         while(j < bits.len){
            def eq = bits.get(j, 0)
            if(eq != 0){ eqs = eqs.append(eq) }
            j += 1
         }
      } elif(is_int(z) || is_bigint(z)){
         if(z != 0){ eqs = eqs.append(z) }
      }
      i += 1
   }
   eqs
}

fn _gf2bv_reduce_space(list eqs, int cols) any {
   mut rows = []
   mut i = 0
   while(i < eqs.len){
      def e = eqs.get(i, 0)
      def coeff = e >> 1
      def rhs = e & 1
      rows = rows.append(coeff | (rhs << cols))
      i += 1
   }
   mut pivots = []
   mut row = 0
   mut col = 0
   while(row < rows.len && col < cols){
      mut pivot = -1
      mut r = row
      while(r < rows.len){
         if(((rows.get(r, 0) >> col) & 1) == 1){
            pivot = r
            r = rows.len
         }
         r += 1
      }
      if(pivot == -1){ col += 1 } else {
         if(pivot != row){
            def tmp = rows.get(row, 0)
            rows[row] = rows.get(pivot, 0)
            rows[pivot] = tmp
         }
         r = 0
         while(r < rows.len){
            if(r != row && (((rows.get(r, 0) >> col) & 1) == 1)){ rows[r] = rows.get(r, 0) ^^ rows.get(row, 0) }
            r += 1
         }
         pivots = pivots.append(col)
         row += 1
         col += 1
      }
   }
   def coeff_mask = (cols <= 0) ? 0 : ((1 << cols) - 1)
   i = 0
   while(i < rows.len){
      def rv = rows.get(i, 0)
      def coeff = rv & coeff_mask
      def rhs = (rv >> cols) & 1
      if(coeff == 0 && rhs == 1){ return nil }
      i += 1
   }
   mut is_pivot = []
   i = 0
   while(i < cols){
      is_pivot = is_pivot.append(0)
      i += 1
   }
   i = 0
   while(i < pivots.len){
      is_pivot[pivots.get(i, 0)] = 1
      i += 1
   }
   mut origin = 0
   i = 0
   while(i < pivots.len){
      def pc = pivots.get(i, 0)
      def rhs = (rows.get(i, 0) >> cols) & 1
      if(rhs == 1){ origin = origin | (1 << pc) }
      i += 1
   }
   mut basis = []
   mut free_col = 0
   while(free_col < cols){
      if(is_pivot.get(free_col, 0) == 0){
         mut vec = 1 << free_col
         i = 0
         while(i < pivots.len){
            def pc = pivots.get(i, 0)
            if(((rows.get(i, 0) >> free_col) & 1) == 1){ vec = vec ^^ (1 << pc) }
            i += 1
         }
         basis = basis.append(vec)
      }
      free_col += 1
   }
   {"origin": origin, "basis": basis}
}

fn _gf2bv_eval_mask(any mask, any raw_solution) int { (bit_count(mask & ((raw_solution << 1) | 1)) & 1) }

fn GF2BVLinearSystem(list sizes) gf2bv_linear {
   "Create a symbolic GF(2) linear system for bit-vector variables."
   if(!is_list(sizes)){ _gf2bv_fail("sizes must be a list") }
   mut sizes_copy = _clone_list(sizes)
   mut cols = 0
   mut i = 0
   while(i < sizes_copy.len){
      cols += sizes_copy.get(i, 0)
      i += 1
   }
   mut basis = []
   i = 0
   while(i <= cols){
      basis = basis.append(1 << i)
      i += 1
   }
   mut vars = []
   mut cur = 1
   i = 0
   while(i < sizes_copy.len){
      def sz = sizes_copy.get(i, 0)
      mut bits = []
      mut j = 0
      while(j < sz){
         bits = bits.append(basis.get(cur + j, 0))
         j += 1
      }
      vars = vars.append(GF2BVBitVec(bits))
      cur += sz
      i += 1
   }
   {"__type":"gf2bv_linear", "sizes":sizes_copy, "cols":cols, "basis":basis, "vars":vars}
}

fn gf2bv_linear_system(list sizes) gf2bv_linear {
   "Alias for GF2BVLinearSystem."
   GF2BVLinearSystem(sizes)
}

fn _gf2bv_linear_sizes(gf2bv_linear x) list { x.get("sizes", []) }

fn _gf2bv_linear_cols(gf2bv_linear x) int { x.get("cols", 0) }

fn _gf2bv_linear_vars(gf2bv_linear x) list { x.get("vars", []) }

impl gf2bv_bv {
   fn bits(gf2bv_bv a) list { _gf2bv_bits(a) }
   fn len(gf2bv_bv a) int { _gf2bv_bits(a).len }
   fn slice(gf2bv_bv a, int start, any stop=nil) gf2bv_bv {
      def bs = _gf2bv_bits(a)
      mut s, e = start, (stop == nil) ? bs.len : stop
      if(s < 0){ s = 0 }
      if(e > bs.len){ e = bs.len }
      if(e < s){ e = s }
      GF2BVBitVec(_gf2bv_slice_list(bs, s, e))
   }
   fn bit(gf2bv_bv a, int idx) gf2bv_bv { a.slice(idx, idx + 1) }
   fn xor(gf2bv_bv a, gf2bv_bv b) gf2bv_bv { GF2BVBitVec(_gf2bv_xor_bits(_gf2bv_bits(a), _gf2bv_bits(b))) }
   fn xor_int(gf2bv_bv a, int other) gf2bv_bv {
      def bs, os = _gf2bv_bits(a), _gf2bv_to_bits_le(bs.len, other)
      GF2BVBitVec(_gf2bv_xor_bits(bs, os))
   }
   fn shr(gf2bv_bv a, int n) gf2bv_bv {
      if(n <= 0){ return a }
      def bs = _gf2bv_bits(a)
      if(n >= bs.len){ return GF2BVBitVec(_gf2bv_fill(bs.len, 0)) }
      mut out = _gf2bv_slice_list(bs, n, bs.len)
      out = _gf2bv_append_fill(out, n, 0)
      GF2BVBitVec(out)
   }
   fn shl(gf2bv_bv a, int n) gf2bv_bv {
      if(n <= 0){ return a }
      def bs = _gf2bv_bits(a)
      if(n >= bs.len){ return GF2BVBitVec(_gf2bv_fill(bs.len, 0)) }
      mut out = _gf2bv_fill(n, 0)
      out = _gf2bv_join(out, _gf2bv_slice_list(bs, 0, bs.len - n))
      GF2BVBitVec(out)
   }
   fn lshift_ext(gf2bv_bv a, int n) gf2bv_bv {
      if(n <= 0){ return a }
      mut out = _gf2bv_fill(n, 0)
      out = _gf2bv_join(out, _gf2bv_bits(a))
      GF2BVBitVec(out)
   }
   fn and_mask(gf2bv_bv a, int mask) gf2bv_bv {
      def bs, ms = _gf2bv_bits(a), _gf2bv_to_bits_le(bs.len, mask)
      if(ms.len == 0){ return GF2BVBitVec([]) }
      if(_gf2bv_all_ones(ms)){ return a }
      GF2BVBitVec(_gf2bv_tuple_where(ms, bs, 0))
   }
   fn or_mask(gf2bv_bv a, int mask) gf2bv_bv {
      def bs, ms = _gf2bv_bits(a), _gf2bv_to_bits_le(bs.len, mask)
      if(_gf2bv_all_ones(ms)){ return GF2BVBitVec(ms) }
      GF2BVBitVec(_gf2bv_tuple_where(ms, 1, bs))
   }
   fn mod_pow2(gf2bv_bv a, int n) gf2bv_bv {
      if(n <= 0){ return GF2BVBitVec([0]) }
      if((n & (n - 1)) != 0){ _gf2bv_fail("modulo non-power-of-2 is not linear") }
      a.and_mask(n - 1)
   }
   fn rotr(gf2bv_bv a, int n) gf2bv_bv {
      def bs = _gf2bv_bits(a)
      if(bs.len == 0){ return a }
      def k = ((n % bs.len) + bs.len) % bs.len
      if(k == 0){ return a }
      GF2BVBitVec(_gf2bv_join(_gf2bv_slice_list(bs, k, bs.len), _gf2bv_slice_list(bs, 0, k)))
   }
   fn rotl(gf2bv_bv a, int n) gf2bv_bv {
      def bs = _gf2bv_bits(a)
      if(bs.len == 0){ return a }
      def k = ((n % bs.len) + bs.len) % bs.len
      if(k == 0){ return a }
      GF2BVBitVec(_gf2bv_join(_gf2bv_slice_list(bs, bs.len - k, bs.len), _gf2bv_slice_list(bs, 0, bs.len - k)))
   }
   fn sum(gf2bv_bv a) gf2bv_bv {
      def bs = _gf2bv_bits(a)
      mut acc = 0
      mut i = 0
      while(i < bs.len){
         acc = acc ^^ bs.get(i, 0)
         i += 1
      }
      GF2BVBitVec([acc])
   }
   fn zeroext(gf2bv_bv a, int n) gf2bv_bv {
      if(n <= 0){ return a }
      def bs = _gf2bv_bits(a)
      GF2BVBitVec(_gf2bv_append_fill(bs, n, 0))
   }
   fn signext(gf2bv_bv a, int n) gf2bv_bv {
      if(n <= 0){ return a }
      def bs = _gf2bv_bits(a)
      if(bs.len == 0){ return GF2BVBitVec([]) }
      def s = bs.get(bs.len - 1, 0)
      GF2BVBitVec(_gf2bv_append_fill(bs, n, s))
   }
   fn broadcast(gf2bv_bv a, int idx, int n) gf2bv_bv {
      def bs = _gf2bv_bits(a)
      if(idx < 0 || idx >= bs.len){ _gf2bv_fail("broadcast index out of range") }
      GF2BVBitVec(_gf2bv_fill(n, bs.get(idx, 0)))
   }
   fn dup(gf2bv_bv a, int n) gf2bv_bv {
      if(n <= 0){ return GF2BVBitVec([]) }
      def bs = _gf2bv_bits(a)
      mut out = []
      mut i = 0
      while(i < n){
         out = _gf2bv_join(out, bs)
         i += 1
      }
      GF2BVBitVec(out)
   }
   fn concat(gf2bv_bv a, gf2bv_bv b) gf2bv_bv { GF2BVBitVec(_gf2bv_join(_gf2bv_bits(a), _gf2bv_bits(b))) }
   fn evaluate_raw(gf2bv_bv a, any raw_solution) int {
      def bs = _gf2bv_bits(a)
      mut out = 0
      mut i = bs.len - 1
      while(i >= 0){
         out = (out << 1) | _gf2bv_eval_mask(bs.get(i, 0), raw_solution)
         i -= 1
      }
      out
   }
   fn same(gf2bv_bv a, gf2bv_bv b) bool { _gf2bv_bits(a) == _gf2bv_bits(b) }
   fn different(gf2bv_bv a, gf2bv_bv b) bool { !a.same(b) }
   operator ^^ gf2bv_bv: gf2bv_bv = xor
   operator ^^ int: gf2bv_bv = xor_int
   operator >> int: gf2bv_bv = shr
   operator << int: gf2bv_bv = shl
   operator & int: gf2bv_bv = and_mask
   operator | int: gf2bv_bv = or_mask
   operator % int: gf2bv_bv = mod_pow2
   operator == gf2bv_bv: bool = same
   operator != gf2bv_bv: bool = different
}

impl gf2bv_linear {
   fn sizes(gf2bv_linear l) list { _gf2bv_linear_sizes(l) }
   fn cols(gf2bv_linear l) int { _gf2bv_linear_cols(l) }
   fn gens(gf2bv_linear l) list { _gf2bv_linear_vars(l) }
   fn get_eqs(gf2bv_linear l, list zeros) list { _gf2bv_parse_zeros(zeros) }
   fn convert_raw(gf2bv_linear l, any raw) list { _gf2bv_convert_raw(l.sizes, raw) }
   fn solve_raw_space(gf2bv_linear l, list zeros) any {
      def eqs = l.get_eqs(zeros)
      _gf2bv_reduce_space(eqs, l.cols)
   }
   fn solve_raw_one(gf2bv_linear l, list zeros) any {
      def space = l.solve_raw_space(zeros)
      if(space == nil){ return nil }
      space.get("origin", 0)
   }
   fn solve_one(gf2bv_linear l, list zeros) any {
      def raw = l.solve_raw_one(zeros)
      if(raw == nil){ return nil }
      l.convert_raw(raw)
   }
   fn solve_all(gf2bv_linear l, list zeros, int max_dimension=16) list {
      def space = l.solve_raw_space(zeros)
      if(space == nil){ return [] }
      def basis = space.get("basis", [])
      def origin = space.get("origin", 0)
      if(basis.len > max_dimension){ _gf2bv_fail("solution space dimension " + str(basis.len) + " exceeds max_dimension " + str(max_dimension)) }
      mut all = []
      def total = 1 << basis.len
      mut mask = 0
      while(mask < total){
         mut raw = origin
         mut i = 0
         while(i < basis.len){
            if(((mask >> i) & 1) == 1){ raw = raw ^^ basis.get(i, 0) }
            i += 1
         }
         all = all.append(l.convert_raw(raw))
         mask += 1
      }
      all
   }
   fn evaluate(gf2bv_linear l, gf2bv_bv bv, list sol) int {
      def raw = _gf2bv_pack_solution(l.sizes, sol)
      bv.evaluate_raw(raw)
   }
}
