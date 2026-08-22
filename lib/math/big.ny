;; Keywords: big bigint bignum math
;; arbitrary-precision integer and native binary floating-point arithmetic.
;; References:
;; - std.math
module std.math.big(is_bigint, bigint, bigint_from_int, bigint_from_str, bigint_to_str,
   bigint_add, bigint_sub, bigint_mul, bigint_div, bigint_mod, bigint_cmp,
   bigint_eq, bigint_neq, bigint_lt, bigint_le, bigint_gt, bigint_ge,
   bigint_neg, bigint_abs, bigint_clone, bigint_pow, bigint_divmod,
   bigint_bit_length, bigint_to_int, bigint_random, bigint_random_bits,
   bigint_lshift, bigint_or, bigint_xor, bigint_popcount, bigint_nth_root,
   _big_make, _big_digits, _big_sign, _big_abs_cmp,
   _big_from_int, _big_add_abs, _big_sub_abs, _big_mul_abs, _big_mul_small, _big_add_small, _digits_prepend,
   _big_divmod_abs,
   BF_PRECISION, BF_SCALE, is_bigfloat, bf_zero, bf_one, bf_with_precision,
   bf_from_int, bf_from_float, bf_to_float, bf_to_str, bf_precision,
   bf_add, bf_sub, bf_mul, bf_div, bf_neg, bf_abs, bf_sign,
   bf_eq, bf_lt, bf_gt, bf_le, bf_ge, bf_sqrt, bf_pow_int)
use std.core
use std.core.str
use std.math.float (float)

def _TAG_BIGINT = __runtime_tag("bigint")
def _TAG_LIST = __runtime_tag("list")

fn _big_float_sqrt(number x) f64 { __flt_sqrt(float(x)) }

fn _big_float_log10(number x) f64 { __flt_log(float(x)) / 2.30258509299404523536 }

fn _big_float_floor(number x) int { __flt_floor(float(x)) }

fn _big_rand() int { from_int(__rand64() & 0x7fffffffffffffff) }

fn _big_randrange(int a, int b) int {
   if a == b { return a }
   def range = b - a
   if range <= 0 { return a }
   a + (_big_rand() % range)
}

fn _big_randint(int a, int b) int {
   if a == b { return a }
   def range = b - a + 1
   if range <= 0 { return a }
   a + (_big_rand() % range)
}

fn _big_is_rt(any x) bool { __has_tag(x, _TAG_BIGINT) }

fn _big_zero() bigint { __bigint_from_int(0) }

fn is_bigint(any x) bool {
   "Returns true if `x` is a BigInt object."
   if _big_is_rt(x) { return true }
   is_ptr(x) && __tagof(x) == std.math.big._TAG_LIST && x.get(0) == 107
}

fn _big_make(int sign, list digits, bool owned=false) any {
   mut actual_digits = digits
   if !owned { actual_digits = clone(digits) }
   mut n_actual = actual_digits.len
   while n_actual > 0 && actual_digits.get(n_actual - 1) == 0 {
      actual_digits.pop()
      n_actual -= 1
   }
   if n_actual == 0 { sign = 0 }
   mut out = [107, sign, actual_digits]
   out
}

fn _big_digits(any b) list { b.get(2) }

fn _big_sign(any b) int {
   if _big_is_rt(b) { return __untag(load64(b, 0)) }
   b.get(1)
}

fn _big_abs_cmp(any a, any b) int {
   mut da = _big_digits(a)
   def db = _big_digits(b)
   mut na, nb = da.len, db.len
   if na < nb { return -1 }
   if na > nb { return 1 }
   mut i = na - 1
   while i >= 0 {
      mut va, vb = da.get(i), db.get(i)
      if va < vb { return -1 }
      if va > vb { return 1 }
      i -= 1
   }
   return 0
}

fn _big_from_int(int n) bigint { __bigint_from_int(n) }

fn bigint(any x) bigint {
   "Converts an integer, string, or existing bigint to a BigInt object."
   if _big_is_rt(x) || is_bigint(x) { return x }
   if is_int(x) { return __bigint_from_int(x) }
   if is_str(x) { return bigint_from_str(x) }
   _big_zero()
}

fn bigint_from_str(str s) bigint {
   "Parses a decimal string into a BigInt."
   if s.len == 0 { return _big_zero() }
   def r = __bigint_from_str(s)
   r ? r : _big_zero()
}

fn bigint_to_str(bigint b) str {
   "Converts a BigInt to its decimal string representation."
   if _big_is_rt(b) { return __bigint_to_str(b) }
   b = bigint(b)
   if _big_is_rt(b) { return __bigint_to_str(b) }
   mut sign = _big_sign(b)
   def digits = _big_digits(b)
   mut n = digits.len
   if sign == 0 || n == 0 { return "0" }
   mut out = Builder(max(16, n * 9 + 8))
   mut i = n - 1
   out = builder_append(out, to_str(digits.get(i)))
   i -= 1
   while i >= 0 {
      def part = to_str(digits.get(i))
      mut pad = 9 - part.len
      while pad > 0 {
         out = builder_append(out, "0")
         pad -= 1
      }
      out = builder_append(out, part)
      i -= 1
   }
   mut out_s = builder_to_str(out)
   builder_free(out)
   if sign < 0 { out_s = "-" + out_s }
   out_s
}

fn _big_add_abs(any a, any b) any {
   def da, db = _big_digits(a), _big_digits(b)
   def na, nb = da.len, db.len
   mut nmax = na
   if nb > nmax { nmax = nb }
   mut result = list(0)
   mut carry = 0
   mut i = 0
   while i < nmax {
      mut va = 0
      if i < na { va = da.get(i) }
      mut vb = 0
      if i < nb { vb = db.get(i) }
      mut sum = va + vb + carry
      if sum >= 1000000000 {
         sum -= 1000000000
         carry = 1
      } else {
         carry = 0
      }
      result = result.append(sum)
      i += 1
   }
   if carry > 0 { result = result.append(carry) }
   if result.len == 0 { return _big_make(0, list(0)) }
   _big_make(1, result, true)
}

fn _big_sub_abs(any a, any b) any {
   def da, db = _big_digits(a), _big_digits(b)
   def na, nb = da.len, db.len
   mut out = list(0)
   mut borrow = 0
   mut i = 0
   while i < na {
      def va = da.get(i)
      mut vb = 0
      if i < nb { vb = db.get(i) }
      mut cur = va - vb - borrow
      if cur < 0 {
         cur = cur + 1000000000
         borrow = 1
      } else {
         borrow = 0
      }
      out = out.append(cur)
      i += 1
   }
   mut end_idx = out.len
   while end_idx > 0 && out.get(end_idx - 1) == 0 { end_idx -= 1 }
   if end_idx == 0 { return _big_make(0, list(0)) }
   mut trimmed = list(0)
   i = 0
   while i < end_idx {
      trimmed = trimmed.append(out.get(i))
      i += 1
   }
   _big_make(1, trimmed, true)
}

fn bigint_add(any a, any b) bigint {
   "Adds two BigInts together."
   __bigint_add(bigint(a), bigint(b))
}

fn bigint_sub(any a, any b) bigint {
   "Subtracts BigInt `b` from BigInt `a`."
   __bigint_sub(bigint(a), bigint(b))
}

fn _big_mul_abs(any a, any b) any {
   def da, db = _big_digits(a), _big_digits(b)
   def na, nb = da.len, db.len
   if na == 0 || nb == 0 { return _big_make(0, []) }
   mut out = list(0)
   mut i = 0
   while i < na + nb {
      out = out.append(0)
      i += 1
   }
   i = 0
   while i < na {
      mut carry = 0
      mut j = 0
      while j < nb {
         def idx = i + j
         def prod = out.get(idx) + da.get(i) * db.get(j) + carry
         carry = prod / 1000000000
         out.set(idx, prod % 1000000000)
         j += 1
      }
      if carry > 0 {
         def idx2 = i + nb
         out.set(idx2, out.get(idx2) + carry)
      }
      i += 1
   }
   mut end_idx = out.len
   while end_idx > 1 && out.get(end_idx - 1) == 0 { end_idx -= 1 }
   mut trimmed = list(0)
   i = 0
   while i < end_idx {
      trimmed = trimmed.append(out.get(i))
      i += 1
   }
   _big_make(1, trimmed, true)
}

fn bigint_mul(any a, any b) bigint {
   "Multiplies two BigInts."
   __bigint_mul(bigint(a), bigint(b))
}

fn _big_mul_small(any a, int m) any {
   if m == 0 { return _big_make(0, []) }
   mut da = _big_digits(a)
   def na = da.len
   mut out = []
   mut carry = 0
   mut i = 0
   while i < na {
      mut prod = da.get(i) * m + carry
      carry = prod / 1000000000
      prod %= 1000000000
      out = out.append(prod)
      i += 1
   }
   if carry > 0 {  out = out.append(carry) }
   _big_make(_big_sign(a), out, true)
}

fn _big_add_small(any a, int v) any {
   mut da = clone(_big_digits(a))
   mut i = 0
   mut carry = v
   while carry > 0 {
      if i >= da.len {  da = da.append(0) }
      mut sum = da.get(i) + carry
      if sum >= 1000000000 {
         da.set(i, sum - 1000000000)
         carry = 1
      } else {
         da.set(i, sum)
         carry = 0
      }
      i += 1
   }
   mut s = _big_sign(a)
   if da.len > 0 { s = 1 }
   _big_make(s, da)
}

fn _digits_prepend(list digits, any v) list {
   mut out = []
   out = out.append(v)
   mut i = 0
   def n = digits.len
   while i < n {
      out = out.append(digits.get(i))
      i += 1
   }
   out
}

fn _big_divmod_abs(any a, any b) list {
   if _big_sign(b) == 0 { panic("bigint division by zero") }
   mut cmp = _big_abs_cmp(a, b)
   if cmp < 0 { return [_big_make(0, []), a] }
   if cmp == 0 { return [_big_make(1, [1]), _big_make(0, [])] }
   def da = _big_digits(a)
   def n = da.len
   mut qdigits = []
   mut r = _big_make(0, [])
   mut idx = n - 1
   while idx >= 0 {
      mut rd = clone(_big_digits(r))
      rd = _digits_prepend(rd, da.get(idx))
      r = _big_make(1, rd)
      mut lo, hi = 0, 1000000000 - 1
      mut best = 0
      while lo <= hi {
         mut mid = (lo + hi) / 2
         mut prod = _big_mul_small(b, mid)
         def c = _big_abs_cmp(prod, r)
         if c <= 0 {
            best = mid
            lo = mid + 1
         } else {
            hi = mid - 1
         }
      }
      if best > 0 {
         mut prod2 = _big_mul_small(b, best)
         r = _big_sub_abs(r, prod2)
      }
      qdigits = _digits_prepend(qdigits, best)
      idx -= 1
   }
   mut q = _big_make(1, qdigits)
   return [q, r]
}

fn bigint_div(any a, any b) bigint {
   "Integer division of BigInts."
   def bb = bigint(b)
   if __bigint_cmp(bb, _big_zero()) == 0 { panic("bigint division by zero") }
   __bigint_div(bigint(a), bb)
}

fn bigint_mod(any a, any b) bigint {
   "Modulo of BigInts."
   def bb = bigint(b)
   if __bigint_cmp(bb, _big_zero()) == 0 { panic("bigint division by zero") }
   __bigint_mod(bigint(a), bb)
}

fn bigint_cmp(any a, any b) int {
   "Compares two BigInts. Returns -1 if a < b, 1 if a > b, 0 if equal."
   __bigint_cmp(bigint(a), bigint(b))
}

fn bigint_eq(any a, any b) bool {
   "Returns true if BigInts `a` and `b` are equal."
   bigint_cmp(a, b) == 0
}

fn bigint_neq(any a, any b) bool {
   "Returns true if BigInts `a` and `b` are not equal."
   !bigint_eq(a, b)
}

fn bigint_lt(any a, any b) bool {
   "Returns true if a < b."
   bigint_cmp(a, b) == -1
}

fn bigint_le(any a, any b) bool {
   "Returns true if a <= b."
   def c = bigint_cmp(a, b)
   c == -1 || c == 0
}

fn bigint_gt(any a, any b) bool {
   "Returns true if a > b."
   bigint_cmp(a, b) == 1
}

fn bigint_ge(any a, any b) bool {
   "Returns true if a >= b."
   def c = bigint_cmp(a, b)
   c == 1 || c == 0
}

fn bigint_from_int(int n) bigint {
   "Build BigInt from int."
   __bigint_from_int(n)
}

fn bigint_neg(any a) bigint {
   "Negate BigInt."
   bigint_sub(_big_zero(), a)
}

fn bigint_abs(bigint a) bigint {
   "Absolute value of BigInt."
   def x = bigint(a)
   if bigint_cmp(x, _big_zero()) < 0 { return bigint_neg(x) }
   x
}

fn bigint_clone(bigint a) bigint {
   "Clone BigInt."
   bigint_add(bigint(a), _big_zero())
}

fn bigint_pow(any a, any b) bigint {
   "Power: a^b using binary exponentiation."
   __bigint_pow(bigint(a), bigint(b))
}

fn bigint_divmod(any a, any b) list {
   "Division with remainder: returns [quotient, remainder]."
   [bigint_div(a, b), bigint_mod(a, b)]
}

fn bigint_bit_length(bigint a) int {
   "Number of bits needed to represent BigInt."
   __bigint_bitlen(bigint_abs(a))
}

fn bigint_to_int(any a) int {
   "Convert BigInt to int(may overflow for large values)."
   if is_int(a) { return a }
   __bigint_to_int(bigint(a))
}

fn bigint_random(any n) bigint {
   "Random BigInt in [0, n)."
   def n_int = bigint_to_int(n)
   if n_int <= 0 { return bigint_from_int(0) }
   bigint_from_int(_big_randrange(0, n_int))
}

fn bigint_random_bits(int bits) bigint {
   "Random BigInt with given bit length."
   def one = bigint_from_int(1)
   mut result = bigint_from_int(0)
   mut i = 0
   while i < bits {
      def bit = _big_randint(0, 1)
      if bit == 1 { result = bigint_add(result, bigint_lshift(one, i)) }
      i += 1
   }
   result
}

fn bigint_lshift(bigint a, int n) bigint {
   "Left shift: a << n(multiply by 2^n)."
   bigint_mul(bigint(a), bigint_pow(bigint_from_int(2), bigint_from_int(n)))
}

fn bigint_or(bigint a, bigint b) bigint {
   "Bitwise OR for non-negative BigInts."
   __bigint_or(bigint(a), bigint(b))
}

fn bigint_xor(bigint a, bigint b) bigint {
   "Bitwise XOR for non-negative BigInts."
   __bigint_xor(bigint(a), bigint(b))
}

fn bigint_popcount(bigint a) int {
   "Count set bits in a non-negative BigInt."
   __bigint_popcount(bigint_abs(a))
}

fn _big_pow_small(any base, int exp) bigint {
   mut acc = bigint_from_int(1)
   mut i = 0
   while i < exp {
      acc = bigint_mul(acc, base)
      i += 1
   }
   acc
}

fn bigint_nth_root(bigint a, int n) bigint {
   "Integer n-th root using Newton iteration."
   def aa = bigint_abs(a)
   if n <= 0 { return bigint_from_int(0) }
   if bigint_eq(aa, bigint_from_int(0)) { return bigint_from_int(0) }
   if n == 1 { return bigint_clone(aa) }
   def n_big = bigint_from_int(n)
   def nm1_big = bigint_from_int(n - 1)
   def bits = bigint_bit_length(aa)
   def guess_bits = (bits + n - 1) / n + 1
   mut x = bigint_pow(bigint_from_int(2), bigint_from_int(guess_bits))
   mut hi_bound = x
   while true {
      def x_nm1 = _big_pow_small(x, n - 1)
      if bigint_eq(x_nm1, bigint_from_int(0)) { return bigint_from_int(0) }
      def term = bigint_div(aa, x_nm1)
      def y = bigint_div(bigint_add(bigint_mul(nm1_big, x), term), n_big)
      if bigint_ge(y, x) {
         mut lo = x
         mut hi = hi_bound
         if bigint_gt(_big_pow_small(lo, n), aa) {
            hi = lo
            lo = bigint_div(lo, bigint_from_int(2))
         }
         while bigint_gt(_big_pow_small(lo, n), aa) {
            hi = lo
            lo = bigint_div(lo, bigint_from_int(2))
         }
         while bigint_lt(lo, hi) {
            def mid = bigint_div(bigint_add(bigint_add(lo, hi), bigint_from_int(1)), bigint_from_int(2))
            if bigint_le(_big_pow_small(mid, n), aa) { lo = mid } else { hi = bigint_sub(mid, bigint_from_int(1)) }
         }
         return lo
      }
      hi_bound = x
      x = y
   }
}

impl bigint {
   fn add(bigint a, bigint b) bigint { bigint_add(a, b) }
   fn add_int(bigint a, int b) bigint { bigint_add(a, b) }
   fn sub(bigint a, bigint b) bigint { bigint_sub(a, b) }
   fn sub_int(bigint a, int b) bigint { bigint_sub(a, b) }
   fn mul(bigint a, bigint b) bigint { bigint_mul(a, b) }
   fn mul_int(bigint a, int b) bigint { bigint_mul(a, b) }
   fn div(bigint a, bigint b) bigint { bigint_div(a, b) }
   fn div_int(bigint a, int b) bigint { bigint_div(a, b) }
   fn rem(bigint a, bigint b) bigint { bigint_mod(a, b) }
   fn rem_int(bigint a, int b) bigint { bigint_mod(a, b) }
   fn pow(bigint a, bigint b) bigint { bigint_pow(a, b) }
   fn pow_int(bigint a, int b) bigint { bigint_pow(a, b) }
   fn xor(bigint a, bigint b) bigint { bigint_xor(a, b) }
   fn xor_int(bigint a, int b) bigint { bigint_xor(a, bigint(b)) }
   fn cmp(bigint a, bigint b) int { bigint_cmp(a, b) }
   fn eq(bigint a, bigint b) bool { bigint_eq(a, b) }
   fn ne(bigint a, bigint b) bool { bigint_neq(a, b) }
   fn lt(bigint a, bigint b) bool { bigint_lt(a, b) }
   fn le(bigint a, bigint b) bool { bigint_le(a, b) }
   fn gt(bigint a, bigint b) bool { bigint_gt(a, b) }
   fn ge(bigint a, bigint b) bool { bigint_ge(a, b) }
   fn neg(bigint a) bigint { bigint_neg(a) }
   fn abs(bigint a) bigint { bigint_abs(a) }
   fn clone(bigint a) bigint { bigint_clone(a) }
   fn str(bigint a) str { bigint_to_str(a) }
   fn int(bigint a) int { bigint_to_int(a) }
   fn bits(bigint a) int { bigint_bit_length(a) }
   fn bit_length(bigint a) int { bigint_bit_length(a) }
   operator + bigint: bigint = add
   operator + int: bigint = add_int
   operator - bigint: bigint = sub
   operator - int: bigint = sub_int
   operator * bigint: bigint = mul
   operator * int: bigint = mul_int
   operator / bigint: bigint = div
   operator / int: bigint = div_int
   operator % bigint: bigint = rem
   operator % int: bigint = rem_int
   operator ^ bigint: bigint = pow
   operator ^ int: bigint = pow_int
   operator ^^ bigint: bigint = xor
   operator ^^ int: bigint = xor_int
   operator == bigint: bool = eq
   operator != bigint: bool = ne
   operator < bigint: bool = lt
   operator <= bigint: bool = le
   operator > bigint: bool = gt
   operator >= bigint: bool = ge
}

impl int {
   fn add_bigint(int a, bigint b) bigint { bigint_add(a, b) }
   fn sub_bigint(int a, bigint b) bigint { bigint_sub(a, b) }
   fn mul_bigint(int a, bigint b) bigint { bigint_mul(a, b) }
   fn div_bigint(int a, bigint b) bigint { bigint_div(a, b) }
   fn rem_bigint(int a, bigint b) bigint { bigint_mod(a, b) }
   fn pow_bigint(int a, bigint b) bigint { bigint_pow(a, b) }
   fn xor_bigint(int a, bigint b) bigint { bigint_xor(bigint(a), b) }
   operator + bigint: bigint = add_bigint
   operator - bigint: bigint = sub_bigint
   operator * bigint: bigint = mul_bigint
   operator / bigint: bigint = div_bigint
   operator % bigint: bigint = rem_bigint
   operator ^ bigint: bigint = pow_bigint
   operator ^^ bigint: bigint = xor_bigint
}

def BF_PRECISION = 256
def BF_SCALE = __bigfloat_one(BF_PRECISION)

fn is_bigfloat(any x) bool { __bigfloat_is(x) }

fn bf_zero() any { "Returns native arbitrary-precision zero." __bigfloat_zero(BF_PRECISION) }

fn bf_one() any { "Returns native arbitrary-precision one." __bigfloat_one(BF_PRECISION) }

fn bf_with_precision(any x, int precision) any {
   "Converts x to a native BigFloat with the requested binary precision."
   __bigfloat_from_value(x, precision)
}

fn bf_from_int(any x) any {
   "Converts an integer or BigInt to native BigFloat."
   __bigfloat_from_value(x, BF_PRECISION)
}

fn bf_from_float(f64 x) any {
   "Converts f64 to native BigFloat."
   __bigfloat_from_value(x, BF_PRECISION)
}

fn bf_to_float(any x) f64 { "Converts BigFloat to approximate f64." __bigfloat_to_f64(x) }

fn bf_to_str(any x) str { "Formats a native BigFloat." __bigfloat_to_str(x) }

fn bf_precision(any x) int { "Returns the binary precision of x." __bigfloat_precision(x) }

fn bf_add(any a, any b) any { "Returns a + b." __bigfloat_add(a, b) }

fn bf_sub(any a, any b) any { "Returns a - b." __bigfloat_sub(a, b) }

fn bf_mul(any a, any b) any { "Returns a * b." __bigfloat_mul(a, b) }

fn bf_div(any a, any b) any { "Returns a / b, or zero when b is zero." __bigfloat_div(a, b) }

fn bf_neg(any a) any { "Returns -a." __bigfloat_neg(a) }

fn bf_abs(any a) any { "Returns |a|." __bigfloat_abs(a) }

fn bf_sign(any a) int { "Returns -1, 0, or 1." __bigfloat_cmp(a, bf_zero()) }

fn bf_eq(any a, any b) bool { "Returns true if a == b." __bigfloat_cmp(a, b) == 0 }

fn bf_lt(any a, any b) bool { "Returns true if a < b." __bigfloat_cmp(a, b) < 0 }

fn bf_gt(any a, any b) bool { "Returns true if a > b." __bigfloat_cmp(a, b) > 0 }

fn bf_le(any a, any b) bool { "Returns true if a <= b." __bigfloat_cmp(a, b) <= 0 }

fn bf_ge(any a, any b) bool { "Returns true if a >= b." __bigfloat_cmp(a, b) >= 0 }

fn bf_sqrt(any a) any { "Returns sqrt(a)." __bigfloat_sqrt(a) }

fn bf_pow_int(any a, int n) any { "Returns a^n for an integer exponent." __bigfloat_pow_int(a, n) }

#main {
   def a = bigint_from_str("12345678901234567890")
   def b = bigint_from_int(10)
   assert(bigint_to_str(bigint_add(a, b)) == "12345678901234567900", "bigint add")
   assert(bigint_to_str(bigint_mul(bigint(12), bigint(12))) == "144", "bigint mul")
   assert(bigint_to_str(bigint_pow(bigint(2), bigint(10))) == "1024", "bigint pow")
   assert(bigint_cmp(bigint(-1), bigint(0)) < 0, "bigint comparison")
   print("✓ std.math.big self-test passed")
}
