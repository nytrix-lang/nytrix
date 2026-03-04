;; Keywords: math bigint bigfloat arbitrary-precision
;; arbitrary-precision integer and fixed-point float arithmetic.

module std.math.big (
   ;; BigInt
   is_bigint, bigint, bigint_from_str, bigint_to_str,
   bigint_add, bigint_sub, bigint_mul, bigint_div, bigint_mod,
   bigint_cmp, bigint_eq,
   ;; Internal (available for advanced use)
   _big_make, _big_digits, _big_sign, _big_abs_cmp, _big_from_int,
   _big_add_abs, _big_sub_abs, _big_mul_abs, _big_mul_small, _big_add_small,
   _digits_prepend, _big_divmod_abs,
   ;; BigFloat
   BF_SCALE,
   bf_zero, bf_one, bf_from_float, bf_to_float,
   bf_add, bf_sub, bf_mul, bf_div,
   bf_neg, bf_abs, bf_sign,
   bf_eq, bf_lt, bf_gt, bf_le, bf_ge,
   bf_sqrt, bf_pow_int
)

use std.core *
use std.core as core
use std.core.error *
use std.str *
use std.str.io *
use std.math *

;; BigInt Implementation

fn is_bigint(x){
   "Returns true if `x` is a BigInt object."
   if(!is_ptr(x)){ return false }
   if(__tagof(x) != 100){ return false }
   get(x, 0) == 107
}

fn _big_make(sign, digits, owned=false){
   "Internal: build bigint with sign and digits (lsf), normalize."
   mut actual_digits = (owned ? digits : list_clone(digits))
   mut n_actual = core.len(actual_digits)
   while(n_actual > 0 && get(actual_digits, n_actual - 1) == 0){
      pop(actual_digits)
      n_actual -= 1
   }
   if(n_actual == 0){ sign = 0 }
   mut out = [107, sign, actual_digits]
   out
}

fn _big_digits(b){
   "Internal: return digits list."
   get(b, 2)
}

fn _big_sign(b){
   "Internal: return sign (-1, 0, or 1)."
   get(b, 1)
}

fn _big_abs_cmp(a, b){
   "Internal: compare |a| and |b|, returns -1/0/1."
   mut da = _big_digits(a)
   def db = _big_digits(b)
   mut na = core.len(da)
   mut nb = core.len(db)
   if(na < nb){ return -1 }
   if(na > nb){ return 1 }
   mut i = na - 1
   while(i >= 0){
      mut va = get(da, i)
      mut vb = get(db, i)
      if(va < vb){ return -1 }
      if(va > vb){ return 1 }
      i -= 1
   }
   return 0
}

fn _big_from_int(n){
   "Internal: build bigint from int."
   if(n == 0){ return _big_make(0, list(0)) }
   mut sign = 1
   if(n < 0){ sign = -1  n = -n }
   mut digits = list(4)
   while(n > 0){
       digits = append(digits, n % 1000000000)
      n = n / 1000000000
   }
   _big_make(sign, digits, true)
}

fn bigint(x){
   "Converts an integer, string, or existing bigint to a BigInt object."
   if(is_bigint(x)){ return x }
   if(is_int(x)){ return _big_from_int(x) }
   if(is_str(x)){ return bigint_from_str(x) }
   _big_make(0, list(0))
}

fn bigint_from_str(s){
   "Parses a decimal string into a BigInt."
   if(str_len(s) == 0){ return _big_make(0, list(0)) }
   mut sign = 1
   mut i = 0
   if(load8(s, 0) == 45){
      sign = -1
      i = 1
   }
   mut res = _big_make(0, list(0))
   mut n = str_len(s)
   while(i < n){
      def c = load8(s, i)
      if(c >= 48 && c <= 57){
         res = _big_mul_small(res, 10)
         res = _big_add_small(res, c - 48)
      }
      i += 1
   }
   def digs = _big_digits(res)
   if(core.len(digs) == 0){ return _big_make(0, list(0)) }
   _big_make(sign, digs)
}

fn bigint_to_str(b){
   "Converts a BigInt to its decimal string representation."
   b = bigint(b)
   mut sign = _big_sign(b)
   def digits = _big_digits(b)
   mut n = core.len(digits)
   if(sign == 0 || n == 0){ return "0" }
   mut out = ""
   mut i = n - 1
   out = to_str(get(digits, i))
   i -= 1
   while(i >= 0){
      def part = to_str(get(digits, i))
      mut pad = 9 - str_len(part)
      while(pad > 0){
         out = out + "0"
         pad -= 1
      }
      out = out + part
      i -= 1
   }
   if(sign < 0){ out = "-" + out }
   out
}

fn _big_add_abs(a, b){
   "Internal: add |a| + |b|."
   mut da = _big_digits(a)
   def db = _big_digits(b)
   mut na = core.len(da)
   mut nb = core.len(db)
   mut out = []
   mut carry = 0
   mut i = 0
   while(i < na || i < nb){
      mut va = 0
      if(i < na){ va = get(da, i) }
      mut vb = 0
      if(i < nb){ vb = get(db, i) }
      mut sum = va + vb + carry
      if(sum >= 1000000000){
         sum -= 1000000000
         carry = 1
      } else {
         carry = 0
      }
       out = append(out, sum)
      i += 1
   }
   if(carry > 0){
       out = append(out, carry)
   }
   _big_make(1, out, true)
}

fn _big_sub_abs(a, b){
   "Internal: compute |a| - |b| where |a| >= |b|."
   mut da = _big_digits(a)
   def db = _big_digits(b)
   mut na = core.len(da)
   mut nb = core.len(db)
   mut out = []
   mut borrow = 0
   mut i = 0
   while(i < na){
      mut va = get(da, i) - borrow
      mut vb = 0
      if(i < nb){ vb = get(db, i) }
      if(va < vb){
         va += 1000000000
         borrow = 1
      } else {
         borrow = 0
      }
       out = append(out, va - vb)
      i += 1
   }
   _big_make(1, out, true)
}

fn bigint_add(a, b){
   "Adds two BigInts together."
   a = bigint(a)
   b = bigint(b)
   mut sa = _big_sign(a)
   mut sb = _big_sign(b)
   if(sa == 0){ return b }
   if(sb == 0){ return a }
   if(sa == sb){
      def res = _big_add_abs(a, b)
      return _big_make(sa, _big_digits(res), true)
   }
   mut cmp = _big_abs_cmp(a, b)
   if(cmp == 0){ return _big_make(0, list(0)) }
   if(cmp > 0){
      def res = _big_sub_abs(a, b)
      return _big_make(sa, _big_digits(res), true)
   }
   def res = _big_sub_abs(b, a)
   _big_make(sb, _big_digits(res), true)
}

fn bigint_sub(a, b){
   "Subtracts BigInt `b` from BigInt `a`."
   a = bigint(a)
   b = bigint(b)
   mut sb = _big_sign(b)
   def neg = _big_make(-sb, list_clone(_big_digits(b)))
   bigint_add(a, neg)
}

fn _big_mul_abs(a, b){
   "Internal: multiply |a| * |b|."
   mut da = _big_digits(a)
   def db = _big_digits(b)
   mut na = core.len(da)
   mut nb = core.len(db)
   if(na == 0 || nb == 0){ return _big_make(0, []) }
   mut out = []
   mut i = 0
   while(i < na + nb + 1){  out = append(out, 0)  i += 1 }
   i = 0
   while(i < na){
      mut carry = 0
      mut j = 0
      while(j < nb){
         mut idx = i + j
         def cur = get(out, idx)
         mut prod = cur + get(da, i) * get(db, j) + carry
         carry = prod / 1000000000
         prod %= 1000000000
         set_idx(out, idx, prod)
         j += 1
      }
      if(carry > 0){
         def idx2 = i + nb
         set_idx(out, idx2, get(out, idx2) + carry)
      }
      i += 1
   }
   _big_make(1, out, true)
}

fn bigint_mul(a, b){
   "Multiplies two BigInts."
   a = bigint(a)
   b = bigint(b)
   mut sa = _big_sign(a)
   mut sb = _big_sign(b)
   def res = _big_mul_abs(a, b)
   _big_make(sa * sb, _big_digits(res), true)
}

fn _big_mul_small(a, m){
   "Internal: multiply bigint by small int m."
   if(m == 0){ return _big_make(0, []) }
   mut da = _big_digits(a)
   def na = core.len(da)
   mut out = []
   mut carry = 0
   mut i = 0
   while(i < na){
      mut prod = get(da, i) * m + carry
      carry = prod / 1000000000
      prod %= 1000000000
       out = append(out, prod)
      i += 1
   }
   if(carry > 0){  out = append(out, carry) }
   _big_make(_big_sign(a), out, true)
}

fn _big_add_small(a, v){
   "Internal: add small int v to bigint."
   mut da = list_clone(_big_digits(a))
   mut i = 0
   mut carry = v
   while(carry > 0){
      if(i >= core.len(da)){  da = append(da, 0) }
      mut sum = get(da, i) + carry
      if(sum >= 1000000000){
         set_idx(da, i, sum - 1000000000)
         carry = 1
      } else {
         set_idx(da, i, sum)
         carry = 0
      }
      i += 1
   }
   mut s = _big_sign(a)
   if(core.len(da) > 0){ s = 1 }
   _big_make(s, da)
}

fn _digits_prepend(digits, v){
   "Internal: prepend v to digits list."
   mut out = []
   out = append(out, v)
   mut i = 0
   def n = core.len(digits)
   while(i < n){
       out = append(out, get(digits, i))
      i += 1
   }
   out
}

fn _big_divmod_abs(a, b){
   "Internal: divmod |a| / |b| -> [q, r]."
   if(_big_sign(b) == 0){ panic("bigint division by zero") }
   mut cmp = _big_abs_cmp(a, b)
   if(cmp < 0){ return [_big_make(0, []), a] }
   if(cmp == 0){ return [_big_make(1, [1]), _big_make(0, [])] }
   def da = _big_digits(a)
   def n = core.len(da)
   mut qdigits = []
   mut i = 0
   while(i < n){  qdigits = append(qdigits, 0)  i += 1 }
   mut r = _big_make(0, [])
   mut idx = n - 1
   while(idx >= 0){
      mut rd = list_clone(_big_digits(r))
      rd = _digits_prepend(rd, get(da, idx))
      r = _big_make(_big_sign(r), rd)
      mut lo = 0  mut hi = 1000000000 - 1
      mut best = 0
      while(lo <= hi){
         mut mid = (lo + hi) / 2
         mut prod = _big_mul_small(b, mid)
         def c = _big_abs_cmp(prod, r)
         if(c <= 0){
         best = mid
         lo = mid + 1
         } else {
         hi = mid - 1
         }
      }
      if(best > 0){
         mut prod2 = _big_mul_small(b, best)
         r = _big_sub_abs(r, prod2)
      }
      set_idx(qdigits, idx, best)
      idx -= 1
   }
   mut q = _big_make(1, qdigits)
   return [q, r]
}

fn bigint_div(a, b){
   "Integer division of BigInts."
   a = bigint(a)
   b = bigint(b)
   mut sa = _big_sign(a)
   mut sb = _big_sign(b)
   if(sb == 0){ panic("bigint division by zero") }
   if(sa == 0){ return _big_make(0, list(0)) }
   def res = _big_divmod_abs(a, b)
   def q = get(res, 0)
   _big_make(sa * sb, list_clone(_big_digits(q)))
}

fn bigint_mod(a, b){
   "Modulo of BigInts."
   a = bigint(a)
   b = bigint(b)
   mut sb = _big_sign(b)
   if(sb == 0){ panic("bigint division by zero") }
   def res = _big_divmod_abs(a, b)
   def r = get(res, 1)
   _big_make(_big_sign(a), list_clone(_big_digits(r)))
}

fn bigint_cmp(a, b){
   "Compares two BigInts. Returns -1 if a < b, 1 if a > b, 0 if equal."
   a = bigint(a)
   b = bigint(b)
   mut sa = _big_sign(a)
   def sb = _big_sign(b)
   if(sa < sb){ return -1 }
   if(sa > sb){ return 1 }
   if(sa == 0){ return 0 }
   def c = _big_abs_cmp(a, b)
   c * sa
}

fn bigint_eq(a, b){
   "Returns true if BigInts `a` and `b` are equal."
   bigint_cmp(a, b) == 0
}

;; BigFloat Implementation (fixed-point at 10^-60)

;; Scale factor = 10^60. All BigFloat values are integers in units of 10^-60.
def BF_SCALE = bigint_from_str("1000000000000000000000000000000000000000000000000000000000000")

fn bf_zero(){
   "Returns the BigFloat value 0."
   bigint(0)
}

fn bf_one(){
   "Returns the BigFloat value 1.0 in BigFloat representation."
   BF_SCALE
}

fn bf_from_float(f){
   "Converts a standard float `f` to a BigFloat. Supports all magnitudes safely."
   if(f == 0.0){ return bigint(0) }
   def neg = f < 0.0
   mut af = f if(neg){ af = 0.0 - f }
   def e = floor(log10(af))
   def m = af / pow(10.0, e)
   ; 14 digits of precision
   def m_int = bigint(int(m * 100000000000000.0))
   mut p = int(46.0 + e)
   mut res = m_int
   if(p >= 0){
       mut s = "1"
       mut i = 0 while(i < p){ s = s + "0" i += 1 }
       res = bigint_mul(m_int, bigint_from_str(s))
   } else {
       mut s = "1"
       mut i = 0 while(i < (0-p)){ s = s + "0" i += 1 }
       res = bigint_div(m_int, bigint_from_str(s))
   }
   if(neg){ res = bigint_sub(bigint(0), res) }
   res
}

fn bf_to_float(a){
   "Converts a BigFloat `a` back to a standard float (loses precision beyond ~15 digits)."
   def neg = _big_sign(a) < 0
   def abs_a = neg ? bigint_sub(bigint(0), a) : a
   def s = bigint_to_str(abs_a)
   def n = len(s)
   if(n == 0){ return 0.0 }
   mut d = 0.0
   if(n > 15){ d = float(slice(s, 0, 15)) * pow(10.0, float(n - 60 - 15)) }
   else { d = float(s) * pow(10.0, -60.0) }
   if(neg){ return 0.0 - d }
   d
}

fn bf_add(a, b){
   "Returns a + b (BigFloat)."
   bigint_add(a, b)
}

fn bf_sub(a, b){
   "Returns a - b (BigFloat)."
   bigint_sub(a, b)
}

fn bf_mul(a, b){
   "Returns a * b (BigFloat)."
   bigint_div(bigint_mul(a, b), BF_SCALE)
}

fn bf_div(a, b){
   "Returns a / b (BigFloat). Returns zero if b is zero."
   if(_big_sign(b) == 0){ return bigint(0) }
   bigint_div(bigint_mul(a, BF_SCALE), b)
}

fn bf_neg(a){
   "Returns -a (BigFloat)."
   bigint_sub(bigint(0), a)
}

fn bf_abs(a){
   "Returns |a| (BigFloat)."
   if(_big_sign(a) < 0){ return bigint_sub(bigint(0), a) }
   a
}

fn bf_sign(a){
   "Returns -1, 0, or 1 depending on the sign of BigFloat `a`."
   _big_sign(a)
}

fn bf_eq(a, b){ "Returns true if a == b (BigFloat)." bigint_eq(a, b) }
fn bf_lt(a, b){ "Returns true if a < b (BigFloat)." _big_sign(bigint_sub(a, b)) < 0 }
fn bf_gt(a, b){ "Returns true if a > b (BigFloat)." _big_sign(bigint_sub(a, b)) > 0 }
fn bf_le(a, b){ "Returns true if a <= b (BigFloat)." !bf_gt(a, b) }
fn bf_ge(a, b){ "Returns true if a >= b (BigFloat)." !bf_lt(a, b) }

fn bf_sqrt(a){
   "Returns sqrt(a) via Newton's method in BigFloat precision (20 iterations)."
   if(_big_sign(a) <= 0){ return bigint(0) }
   def fa = bf_to_float(a)
   mut r = bf_from_float(sqrt(fa))
   def two = bf_from_float(2.0)
   mut i = 0
   while(i < 20){
      r = bf_div(bf_add(r, bf_div(a, r)), two)
      i += 1
   }
   r
}

fn bf_pow_int(a, n){
   "Returns a^n for integer exponent n >= 0 (BigFloat)."
   if(n == 0){ return BF_SCALE }
   mut res = BF_SCALE
   mut base = a
   mut exp = n
   while(exp > 0){
      if(exp % 2 == 1){ res = bf_mul(res, base) }
      base = bf_mul(base, base)
      exp = exp / 2
   }
   res
}

if(comptime{__main()}){
   use std.math.big *
   use std.core.error *
   use std.math *

   ;; BigInt tests (2.0)
   def half = bf_from_float(0.5)
   assert(bf_to_float(zero) == 0.0, "bf zero")
   assert(near_bf(one, bf_from_float(1.0), 1e-10), "bf one")
   assert(near_bf(bf_add(half, half), one, 1e-10), "bf 0.5+0.5=1")
   assert(near_bf(bf_mul(two, half), one, 1e-10), "bf 2*0.5=1")
   assert(near_bf(bf_div(one, two), half, 1e-10), "bf 1/2=0.5")
   assert(near_bf(bf_sub(one, half), half, 1e-10), "bf 1-0.5=0.5")
   assert(bf_lt(half, one), "bf 0.5 < 1")
   assert(bf_gt(one, half), "bf 1 > 0.5")
   ;; Zoom roundtrip: 50 zooms in, 50 zooms out
   mut z = bf_from_float(0.6)
   mut i = 0
   while(i < 50){ z = bf_mul(z, bf_from_float(1.15)) i += 1 }
   while(i > 0){ z = bf_div(z, bf_from_float(1.15)) i -= 1 }
   assert(near_bf(z, bf_from_float(0.6), 1e-6), "bf zoom roundtrip")
   print("✓ BigFloat tests passed")
}
