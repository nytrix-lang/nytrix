;; Keywords: ring algebra math
;; Packed algebra facades over the low-level number theory, finite-field, and polynomial kernels.
;; References:
;; - std.math
module std.math.ring(Zmod, Integers, ZmodElem, zmod_ring, zmod_elem, is_zmod_ring, is_zmod, PolynomialRing, Poly, poly_ring, poly_elem, is_poly_ring, is_poly_elem)
use std.core
use std.math.nt
use std.math.crypto.gf
use std.math.crypto.poly as rawpoly

impl zmod_ring {}

impl zmod {}

impl poly_ring {}

impl poly_elem {}
fn is_zmod_ring(any x) bool { "Returns true for Zmod ring dictionaries." is_dict(x) && x.get("__type", "") == "zmod_ring" }

fn is_zmod(any x) bool { "Returns true for Zmod element dictionaries." is_dict(x) && x.get("__type", "") == "zmod" }

fn _zmod_modulus(any x) bigint { x.get("modulus", Z(0)) }

fn _zmod_value(any x) bigint { x.get("value", Z(0)) }

fn Zmod(any n) zmod_ring {
   "Return the ring of integers modulo n."
   def m = Z(n)
   if(m <= Z(0)){ panic("Zmod: modulus must be positive") }
   {"__type":"zmod_ring", "modulus":m}
}

fn Integers(any n) zmod_ring {
   "Sage-style alias for Zmod(n)."
   Zmod(n)
}

fn ZmodElem(any ring_or_modulus, any value) zmod {
   "Create an element of Z/nZ."
   def zmod_ring: R = is_zmod_ring(ring_or_modulus) ? ring_or_modulus : Zmod(ring_or_modulus)
   def m = _zmod_modulus(R)
   {"__type":"zmod", "modulus":m, "value":mod(value, m)}
}

fn zmod_elem(any ring_or_modulus, any value) zmod { ZmodElem(ring_or_modulus, value) }

fn _zmod_ring_from(zmod a) zmod_ring { Zmod(_zmod_modulus(a)) }

fn _zmod_check_same(zmod a, zmod b) zmod_ring {
   if(_zmod_modulus(a) != _zmod_modulus(b)){ panic("zmod: modulus mismatch") }
   _zmod_ring_from(a)
}

impl zmod_ring {
   fn modulus(zmod_ring R) bigint { _zmod_modulus(R) }
   fn characteristic(zmod_ring R) bigint { _zmod_modulus(R) }
   fn order(zmod_ring R) bigint { _zmod_modulus(R) }
   fn elem(zmod_ring R, any value) zmod { ZmodElem(R, value) }
   fn zero(zmod_ring R) zmod { ZmodElem(R, 0) }
   fn one(zmod_ring R) zmod { ZmodElem(R, 1) }
}

impl zmod {
   fn value(zmod a) bigint { _zmod_value(a) }
   fn lift(zmod a) bigint { _zmod_value(a) }
   fn modulus(zmod a) bigint { _zmod_modulus(a) }
   fn ring(zmod a) zmod_ring { _zmod_ring_from(a) }
   fn add(zmod a, zmod b) zmod {
      def zmod_ring: R = _zmod_check_same(a, b)
      ZmodElem(R, _zmod_value(a) + _zmod_value(b))
   }
   fn add_int(zmod a, int b) zmod { ZmodElem(a.ring, _zmod_value(a) + b) }
   fn add_bigint(zmod a, bigint b) zmod { ZmodElem(a.ring, _zmod_value(a) + b) }
   fn sub(zmod a, zmod b) zmod {
      def zmod_ring: R = _zmod_check_same(a, b)
      ZmodElem(R, _zmod_value(a) - _zmod_value(b))
   }
   fn sub_int(zmod a, int b) zmod { ZmodElem(a.ring, _zmod_value(a) - b) }
   fn sub_bigint(zmod a, bigint b) zmod { ZmodElem(a.ring, _zmod_value(a) - b) }
   fn neg(zmod a) zmod { ZmodElem(a.ring, 0 - _zmod_value(a)) }
   fn mul(zmod a, zmod b) zmod {
      def zmod_ring: R = _zmod_check_same(a, b)
      ZmodElem(R, _zmod_value(a) * _zmod_value(b))
   }
   fn mul_int(zmod a, int b) zmod { ZmodElem(a.ring, _zmod_value(a) * b) }
   fn mul_bigint(zmod a, bigint b) zmod { ZmodElem(a.ring, _zmod_value(a) * b) }
   fn inv(zmod a) zmod {
      def inva = inverse_mod(_zmod_value(a), _zmod_modulus(a))
      if(inva == Z(0)){ panic("zmod: element is not a unit") }
      ZmodElem(a.ring, inva)
   }
   fn div(zmod a, zmod b) zmod {
      def zmod_ring: R = _zmod_check_same(a, b)
      ZmodElem(R, _zmod_value(a) * _zmod_value(b.inv))
   }
   fn pow_int(zmod a, int e) zmod { ZmodElem(a.ring, power_mod(_zmod_value(a), e, _zmod_modulus(a))) }
   fn pow_bigint(zmod a, bigint e) zmod { ZmodElem(a.ring, power_mod(_zmod_value(a), e, _zmod_modulus(a))) }
   fn same(zmod a, zmod b) bool { _zmod_modulus(a) == _zmod_modulus(b) && _zmod_value(a) == _zmod_value(b) }
   fn different(zmod a, zmod b) bool { !a.same(b) }
   fn str(zmod a) str { to_str(_zmod_value(a)) + " (mod " + to_str(_zmod_modulus(a)) + ")" }
   operator + zmod: zmod = add
   operator + int: zmod = add_int
   operator + bigint: zmod = add_bigint
   operator - zmod: zmod = sub
   operator - int: zmod = sub_int
   operator - bigint: zmod = sub_bigint
   operator * zmod: zmod = mul
   operator * int: zmod = mul_int
   operator * bigint: zmod = mul_bigint
   operator / zmod: zmod = div
   operator ^ int: zmod = pow_int
   operator ^ bigint: zmod = pow_bigint
   operator == zmod: bool = same
   operator != zmod: bool = different
}

fn is_poly_ring(any x) bool { "Returns true for polynomial ring dictionaries." is_dict(x) && x.get("__type", "") == "poly_ring" }

fn is_poly_elem(any x) bool { "Returns true for polynomial element dictionaries." is_dict(x) && x.get("__type", "") == "poly_elem" }

fn PolynomialRing(any base=nil, str name="x") poly_ring {
   "Create a univariate polynomial ring over ZZ, GF(p), or Zmod(n)."
   def ring_base = base == nil ? "ZZ" : base
   {"__type":"poly_ring", "base":ring_base, "name":name}
}

fn _poly_base_kind(poly_ring R) str {
   def b = R.get("base", "ZZ")
   if(is_dict(b) && b.get("__type", "") == "gf"){ return "gf" }
   if(is_zmod_ring(b)){ return "zmod" }
   "zz"
}

fn _poly_base_modulus(poly_ring R) any {
   def b = R.get("base", "ZZ")
   if(is_dict(b) && b.get("__type", "") == "gf"){ return b.get("p", 0) }
   if(is_zmod_ring(b)){ return _zmod_modulus(b) }
   nil
}

fn _poly_coeff_value(poly_ring R, any c) any {
   def kind = _poly_base_kind(R)
   if(kind == "gf"){
      def gf: F = R.get("base")
      if(is_dict(c) && c.get("__type", "") == "gfe"){ return c.value }
      return mod(c, F.get("p", 0))
   }
   if(kind == "zmod"){
      def zmod_ring: ZR = R.get("base")
      if(is_zmod(c)){ return c.value }
      return mod(c, _zmod_modulus(ZR))
   }
   Z(c)
}

fn _poly_normalize_coeffs(poly_ring R, list coeffs) list {
   mut out = []
   mut i = 0
   while(i < coeffs.len){
      out = out.append(_poly_coeff_value(R, coeffs.get(i)))
      i += 1
   }
   while(out.len > 1 && out.get(out.len - 1) == 0){ out = out.slice(0, out.len - 1) }
   if(out.len == 0){ out = [0] }
   out
}

fn Poly(any ring_or_base, list coeffs, str name="x") poly_elem {
   "Create a packed univariate polynomial element."
   def poly_ring: R = is_poly_ring(ring_or_base) ? ring_or_base : PolynomialRing(ring_or_base, name)
   {"__type":"poly_elem", "ring":R, "coeffs":_poly_normalize_coeffs(R, coeffs)}
}

fn _poly_ring(poly_elem f) poly_ring { f.get("ring") }

fn _poly_coeffs(poly_elem f) list { f.get("coeffs", [0]) }

fn _poly_check_same(poly_elem a, poly_elem b) poly_ring {
   def poly_ring: Ra = _poly_ring(a)
   def poly_ring: Rb = _poly_ring(b)
   if(Ra.get("name", "x") != Rb.get("name", "x")){ panic("poly: variable mismatch") }
   if(Ra.get("base", "ZZ") != Rb.get("base", "ZZ")){ panic("poly: base ring mismatch") }
   Ra
}

fn _poly_coeffs_equal(list a, list b) bool {
   if(a.len != b.len){ return false }
   mut i = 0
   while(i < a.len){
      if(a.get(i) != b.get(i)){ return false }
      i += 1
   }
   true
}

fn _poly_neg_coeffs(poly_ring R, list coeffs) list {
   mut out = []
   mut i = 0
   while(i < coeffs.len){
      out = out.append(0 - coeffs.get(i))
      i += 1
   }
   _poly_normalize_coeffs(R, out)
}

fn _poly_pow(poly_elem f, int e) poly_elem {
   if(e < 0){ panic("poly: negative exponent is not supported") }
   def poly_ring: R = _poly_ring(f)
   mut result = [1]
   mut base = _poly_coeffs(f)
   mut exp = e
   while(exp > 0){
      if(exp % 2 == 1){ result = _poly_normalize_coeffs(R, rawpoly.poly_mul(result, base)) }
      exp = exp / 2
      if(exp > 0){ base = _poly_normalize_coeffs(R, rawpoly.poly_mul(base, base)) }
   }
   Poly(R, result)
}

impl poly_ring {
   fn base(poly_ring R) any { R.get("base", "ZZ") }
   fn variable(poly_ring R) str { R.get("name", "x") }
   fn gen(poly_ring R) poly_elem { Poly(R, [0, 1]) }
   fn zero(poly_ring R) poly_elem { Poly(R, [0]) }
   fn one(poly_ring R) poly_elem { Poly(R, [1]) }
   fn elem(poly_ring R, list coeffs) poly_elem { Poly(R, coeffs) }
}

impl poly_elem {
   fn ring(poly_elem f) poly_ring { _poly_ring(f) }
   fn parent(poly_elem f) poly_ring { _poly_ring(f) }
   fn coeffs(poly_elem f) list { _poly_coeffs(f) }
   fn degree(poly_elem f) int {
      def c = _poly_coeffs(f)
      (c.len == 1 && c.get(0) == 0) ? -1 : c.len - 1
   }
   fn lc(poly_elem f) any { _poly_coeffs(f).get(_poly_coeffs(f).len - 1) }
   fn monic(poly_elem f) poly_elem {
      def poly_ring: R = _poly_ring(f)
      def kind = _poly_base_kind(R)
      def coeffs = _poly_coeffs(f)
      def lead = f.lc
      if(lead == 0){ return f }
      if(kind == "zz"){
         if(lead == 1){ return f }
         mut out = []
         mut i = 0
         while(i < coeffs.len){
            out = out.append(coeffs.get(i) / lead)
            i += 1
         }
         return Poly(R, out)
      }
      def p = _poly_base_modulus(R)
      def inv = inverse_mod(lead, p)
      if(inv == 0){ panic("poly: leading coefficient is not a unit") }
      mut out = []
      mut i = 0
      while(i < coeffs.len){
         out = out.append(mod(coeffs.get(i) * inv, p))
         i += 1
      }
      Poly(R, out)
   }
   fn derivative(poly_elem f) poly_elem {
      def poly_ring: R = _poly_ring(f)
      def p, c = _poly_base_modulus(R), _poly_coeffs(f)
      def d = p == nil ? rawpoly.poly_derivative(c) : rawpoly.poly_derivative_mod(c, p)
      Poly(R, d)
   }
   fn evaluate(poly_elem f, any x) any {
      def poly_ring: R = _poly_ring(f)
      def kind = _poly_base_kind(R)
      if(kind == "zz"){ return rawpoly.poly_eval(_poly_coeffs(f), Z(x)) }
      def p = _poly_base_modulus(R)
      def xv = kind == "zmod" && is_zmod(x) ? x.value : (is_dict(x) && x.get("__type", "") == "gfe" ? x.value : x)
      def y = rawpoly.poly_mod_eval(_poly_coeffs(f), xv, p)
      def base = R.get("base", "ZZ")
      if(kind == "zmod"){ return ZmodElem(base, y) }
      GFElem(base, y)
   }
   fn eval(poly_elem f, any x) any { f.evaluate(x) }
   fn at(poly_elem f, any x) any { f.evaluate(x) }
   fn roots_mod(poly_elem f, any p=nil) list {
      def poly_ring: R = _poly_ring(f)
      def pp = p == nil ? _poly_base_modulus(R) : p
      if(pp == nil){ panic("poly.roots_mod: modulus required for integer polynomials") }
      if(Z(pp) <= Z(100000)){
         def pi = bigint_to_int(Z(pp))
         mut roots = []
         mut x = 0
         while(x < pi){
            if(rawpoly.poly_mod_eval(_poly_coeffs(f), Z(x), Z(pp)) == 0){ roots = roots.append(x) }
            x += 1
         }
         return roots
      }
      rawpoly.poly_mod_roots(_poly_normalize_coeffs(PolynomialRing(GF(pp), R.get("name", "x")), _poly_coeffs(f)), pp)
   }
   fn gcd(poly_elem f, poly_elem g) poly_elem {
      def poly_ring: R = _poly_check_same(f, g)
      def p = _poly_base_modulus(R)
      if(p == nil){ panic("poly.gcd: integer polynomial gcd is not implemented in packed facade yet") }
      Poly(R, rawpoly.poly_mod_gcd(_poly_coeffs(f), _poly_coeffs(g), p))
   }
   fn resultant(poly_elem f, poly_elem g) any {
      def poly_ring: R = _poly_check_same(f, g)
      def p = _poly_base_modulus(R)
      p == nil ? rawpoly.poly_resultant(_poly_coeffs(f), _poly_coeffs(g)) : rawpoly.poly_resultant_mod(_poly_coeffs(f), _poly_coeffs(g), p)
   }
   fn add(poly_elem f, poly_elem g) poly_elem {
      def poly_ring: R = _poly_check_same(f, g)
      Poly(R, rawpoly.poly_add(_poly_coeffs(f), _poly_coeffs(g)))
   }
   fn sub(poly_elem f, poly_elem g) poly_elem {
      def poly_ring: R = _poly_check_same(f, g)
      Poly(R, rawpoly.poly_add(_poly_coeffs(f), _poly_neg_coeffs(R, _poly_coeffs(g))))
   }
   fn neg(poly_elem f) poly_elem { Poly(_poly_ring(f), _poly_neg_coeffs(_poly_ring(f), _poly_coeffs(f))) }
   fn mul(poly_elem f, poly_elem g) poly_elem {
      def poly_ring: R = _poly_check_same(f, g)
      Poly(R, rawpoly.poly_mul(_poly_coeffs(f), _poly_coeffs(g)))
   }
   fn poly_pow_int(poly_elem f, int e) poly_elem { _poly_pow(f, e) }
   fn eq_poly(poly_elem f, any g) bool {
      if(!is_poly_elem(g)){ return false }
      def Ra, Rb = f.get("ring"), g.get("ring")
      if(Ra.get("name", "x") != Rb.get("name", "x")){ return false }
      if(Ra.get("base", "ZZ") != Rb.get("base", "ZZ")){ return false }
      return _poly_coeffs_equal(_poly_coeffs(f), _poly_coeffs(g))
   }
   fn equals(poly_elem f, poly_elem g) bool { f.eq_poly(g) }
   fn same(poly_elem f, poly_elem g) bool { f.equals(g) }
   fn different(poly_elem f, poly_elem g) bool { !f.equals(g) }
   operator + poly_elem: poly_elem = add
   operator - poly_elem: poly_elem = sub
   operator * poly_elem: poly_elem = mul
   operator ^ int: poly_elem = poly_pow_int
   operator == poly_elem: bool = eq_poly
   operator != poly_elem: bool = different
}

fn poly_ring(any base=nil, str name="x") poly_ring { PolynomialRing(base, name) }

fn poly_elem(any ring_or_base, list coeffs, str name="x") poly_elem { Poly(ring_or_base, coeffs, name) }
