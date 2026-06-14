;; Keywords: nt number-theory modular math
;; Number-theory operations for primes, modular arithmetic, CRT, residues, and arithmetic functions.
;; References:
;; - std.math
module std.math.nt(Z, ZZ, Int, Integer, nt_bigint, is_bigint, bigint, bigint_from_int,
   bigint_from_str, bigint_add, bigint_sub, bigint_mul, bigint_div, bigint_mod,
   bigint_cmp, bigint_eq, bigint_neq, bigint_lt, bigint_le, bigint_gt, bigint_ge,
   bigint_neg, bigint_abs, bigint_clone, bigint_pow, bigint_divmod,
   bigint_bit_length, bigint_to_int, bigint_random, bigint_random_bits,
   bigint_lshift, bigint_or, bigint_xor, bigint_popcount, bigint_nth_root,
   __add, __sub, __mul, __div, __mod, __pow, __neg, __eq, __neq, __lt, __le,
   __gt, __ge, __str, __len,
   gcd, lcm, xgcd, egcd,
   mod, power_mod, inverse_mod,
   is_prime, is_prime_power, next_prime, prev_prime, prime, prime_range, primes_first_n,
   factor, factordb_factor, _factor_product, factorint, divisors,
   euler_phi, carmichael_lambda, moebius, legendre, jacobi, kronecker,
   crt, CRT, crt2, garner,
   extended_gcd, bezout_coefficients,
   solve_linear_congruence,
   tonelli_shanks, cipolla, mod_nth_roots_prime,
   mod_quadratic_roots_prime,
   continued_fraction, cf_convergents,
   rational_reconstruction, mqrr_rational_reconstruction,
   is_smooth, smooth_part,
   random_prime, randint,
   int_to_bytes, bytes_to_int,
   int_to_hex, bigint_to_str, bigint_to_hex, hex_to_int, hex_to_bigint, bit_length, is_square,
   is_perfect_square, bigint_to_bytes, bytes_to_bigint, bytes_to_long, long_to_bytes, xor_bytes,
   str_to_bytes, bytes_to_str, isqrt, nth_root, _mont_init, _mont_redc, _mont_mul, _mont_to, _mont_from,
   _barrett_init, _barrett_reduce, mod_ctx_new, mod_kernel_mul, mod_kernel_pow, mod_kernel_inv, mod_add,
mod_sub, mod_mul)

use std.core
use std.math.big
use std.core.str
use std.core.str as str_mod
use std.os.net (requests_get_parsed)
use std.parse.data.json (json_decode)

def _TAG_BIGINT = __runtime_tag("bigint")
def _TAG_LIST = __runtime_tag("list")
mut _factor_ext_cache = dict()

fn is_bigint(any x) bool {
   "Check if x is a BigInt(internal type marker 107)."
   __has_tag(x, _TAG_BIGINT) || (is_ptr(x) && __tagof(x) == _TAG_LIST && x.get(0) == 107)
}

fn Z(any n) bigint {
   "Construct BigInt from int or string. SageMath style: Z(123) or Z('123...')."
   nt_bigint(n)
}

fn ZZ(any n) bigint {
   "Construct BigInt. SageMath notation: ZZ(123)."
   nt_bigint(n)
}

fn Int(any n) bigint {
   "Construct BigInt. Python style: Int(123)."
   nt_bigint(n)
}

fn Integer(any n) bigint {
   "Construct BigInt. Explicit: Integer(123)."
   nt_bigint(n)
}

fn nt_bigint(any x) bigint {
   "Convert int, string, or BigInt to BigInt."
   if is_bigint(x) { return x }
   if is_int(x) { return bigint_from_int(x) }
   if is_str(x) {
      def s0 = str_mod.strip(x)
      if str_mod.startswith(s0, "-0x") || str_mod.startswith(s0, "-0X") { return bigint_neg(hex_to_bigint(str_mod.str_slice(s0, 3, s0.len))) }
      if str_mod.startswith(s0, "0x") || str_mod.startswith(s0, "0X") { return hex_to_bigint(str_mod.str_slice(s0, 2, s0.len)) }
      return bigint_from_str(s0)
   }
   bigint_from_int(0)
}

fn __add(any a, any b) any {
   if !is_bigint(a) || !is_bigint(b) { return nil }
   bigint_add(a, b)
}

fn __sub(any a, any b) any {
   if !is_bigint(a) || !is_bigint(b) { return nil }
   bigint_sub(a, b)
}

fn __mul(any a, any b) any {
   if !is_bigint(a) || !is_bigint(b) { return nil }
   bigint_mul(a, b)
}

fn __div(any a, any b) any {
   if !is_bigint(a) || !is_bigint(b) { return nil }
   bigint_div(a, b)
}

fn __mod(any a, any b) any {
   if !is_bigint(a) || !is_bigint(b) { return nil }
   bigint_mod(a, b)
}

fn __pow(any a, any b) any {
   if !is_bigint(a) || !is_bigint(b) { return nil }
   bigint_pow(a, b)
}

fn __neg(any a) any {
   if !is_bigint(a) { return nil }
   bigint_neg(a)
}

fn __eq(any a, any b) bool {
   if !is_bigint(a) || !is_bigint(b) { return false }
   bigint_eq(a, b)
}

fn __neq(any a, any b) bool {
   if !is_bigint(a) || !is_bigint(b) { return true }
   !bigint_eq(a, b)
}

fn __lt(any a, any b) bool {
   if !is_bigint(a) || !is_bigint(b) { return false }
   bigint_lt(a, b)
}

fn __le(any a, any b) bool {
   if !is_bigint(a) || !is_bigint(b) { return false }
   bigint_le(a, b)
}

fn __gt(any a, any b) bool {
   if !is_bigint(a) || !is_bigint(b) { return false }
   bigint_gt(a, b)
}

fn __ge(any a, any b) bool {
   if !is_bigint(a) || !is_bigint(b) { return false }
   bigint_ge(a, b)
}

fn __str(any a) any {
   if !is_bigint(a) { return nil }
   bigint_to_str(a)
}

fn __len(any a) int {
   if !is_bigint(a) { return 0 }
   bigint_bit_length(a)
}

fn gcd(any a, any b) bigint {
   "Greatest common divisor."
   Z(__bigint_gcd(Z(a), Z(b)))
}

fn lcm(any a, any b) bigint {
   "Least common multiple."
   def a_big, b_big = Z(a), Z(b)
   if bigint_eq(a_big, Z(0)) || bigint_eq(b_big, Z(0)) { return Z(0) }
   bigint_div(bigint_abs(bigint_mul(a_big, b_big)), gcd(a_big, b_big))
}

fn xgcd(any a, any b) list {
   "Extended GCD: returns [g, u, v] where a*u + b*v = g. Uses GMP-backed builtin."
   __bigint_xgcd(Z(a), Z(b))
}

fn egcd(any a, any b) list {
   "Extended GCD alias."
   xgcd(a, b)
}

fn mod(any a, any b) bigint {
   "Modular reduction: a mod b."
   def m, r = Z(b), bigint_mod(Z(a), m)
   if bigint_lt(r, Z(0)) { return bigint_add(r, m) }
   r
}

fn power_mod(any base, any exp, any modulus) bigint {
   "Modular exponentiation: base^exp mod modulus.
   Uses GMP-backed builtin for performance."
   def modulus_big = Z(modulus)
   if bigint_eq(modulus_big, Z(0)) { return Z(base) }
   if bigint_lt(Z(exp), Z(0)) { return Z(base) }
   Z(__bigint_powmod(Z(base), Z(exp), modulus_big))
}

fn _bigint_get_bit(any a, int bit) bool {
   if bit < 0 { return false }
   def mask = bigint_lshift(Z(1), bit)
   bigint_mod(bigint_div(Z(a), mask), Z(2)) != Z(0)
}

fn _mont_window(any e_big, int bit, int window_size) list<int> {
   mut last_bit = bit
   mut j = 1
   while j < window_size && (bit - j) >= 0 {
      if _bigint_get_bit(e_big, bit - j) { last_bit = bit - j }
      j += 1
   }
   mut w_val = 0
   mut k = bit
   while k >= last_bit {
      w_val = (w_val << 1) | (_bigint_get_bit(e_big, k) ? 1 : 0)
      k -= 1
   }
   [last_bit, w_val]
}

fn power_mod_montgomery(any base, any exp, any n, int window_size=4) bigint {
   "Modular exponentiation using Montgomery reduction and sliding window algorithm.
   Uses a path for large BigInt operations where division is expensive."
   def n_big, e_big = Z(n), Z(exp)
   if bigint_eq(n_big, Z(1)) { return Z(0) }
   if bigint_eq(e_big, Z(0)) { return Z(1) }
   if n_big % Z(2) == Z(0) { return power_mod(base, exp, n_big) }
   def ctx = _mont_init(n_big)
   def b_bar = _mont_to(Z(base), ctx)
   def num_precomputed = 1 << (window_size - 1)
   mut powers = [b_bar]
   def b2 = _mont_mul(b_bar, b_bar, ctx)
   mut i = 1
   while i < num_precomputed {
      powers = powers.append(_mont_mul(powers.get(i - 1), b2, ctx))
      i += 1
   }
   mut res_bar = _mont_to(Z(1), ctx)
   mut bit = bigint_bit_length(e_big) - 1
   while bit >= 0 {
      if !_bigint_get_bit(e_big, bit) {
         res_bar = _mont_mul(res_bar, res_bar, ctx)
         bit -= 1
      } else {
         def win = _mont_window(e_big, bit, window_size)
         def last_bit, w_val = win[0], win[1]
         mut s = 0
         while s <= (bit - last_bit) {
            res_bar = _mont_mul(res_bar, res_bar, ctx)
            s += 1
         }
         res_bar = _mont_mul(res_bar, powers.get(w_val >> 1), ctx)
         bit = last_bit - 1
      }
   }
   _mont_from(res_bar, ctx)
}

fn _barrett_init(any n) any {
   def n_big = Z(n)
   def k = bigint_bit_length(n_big)
   def mu = bigint_div(bigint_lshift(Z(1), 2 * k), n_big)
   [n_big, k, mu]
}

fn _barrett_reduce(any a, any ctx) bigint {
   def n, k = ctx.get(0), ctx.get(1)
   def mu = ctx.get(2)
   def a_big = Z(a)
   def q1 = bigint_div(a_big, bigint_lshift(Z(1), k - 1))
   def q2 = bigint_mul(q1, mu)
   def q3 = bigint_div(q2, bigint_lshift(Z(1), k + 1))
   mut r = bigint_sub(a_big, bigint_mul(q3, n))
   while bigint_ge(r, n) { r = bigint_sub(r, n) }
   r
}

fn mod_ctx_new(any n, str backend="auto") any {
   "Create a modular arithmetic context for modulus n."
   def n_big = Z(n)
   mut use_backend = backend
   if use_backend == "auto" { use_backend = "montgomery" }
   if use_backend == "montgomery" {
      def ctx = _mont_init(n_big)
      return ["montgomery", n_big, ctx]
   }
   if use_backend == "barrett" {
      def ctx = _barrett_init(n_big)
      return ["barrett", n_big, ctx]
   }
   ["naive", n_big]
}

fn mod_kernel_mul(any a, any b, any ctx) bigint {
   "Unified modular multiplication."
   def type = ctx.get(0)
   if type == "montgomery" {
      def m_ctx = ctx.get(2)
      return _mont_from(_mont_mul(_mont_to(Z(a), m_ctx), _mont_to(Z(b), m_ctx), m_ctx), m_ctx)
   }
   if type == "barrett" { return _barrett_reduce(bigint_mul(Z(a), Z(b)), ctx.get(2)) }
   bigint_mod(bigint_mul(Z(a), Z(b)), ctx.get(1))
}

fn mod_kernel_pow(any base, any exp, any ctx) bigint {
   "Unified modular exponentiation."
   def type = ctx.get(0)
   if type == "montgomery" { return power_mod_montgomery(base, exp, ctx.get(1)) }
   power_mod(base, exp, ctx.get(1))
}

fn mod_kernel_inv(any a, any ctx) bigint {
   "Unified modular inversion."
   inverse_mod(a, ctx.get(1))
}

fn mod_add(any a, any b, any p) bigint {
   "a + b mod p(handles large integers)."
   mod(bigint_add(Z(a), Z(b)), Z(p))
}

fn mod_sub(any a, any b, any p) bigint {
   "a - b mod p(handles negative results)."
   def res = bigint_mod(bigint_sub(Z(a), Z(b)), Z(p))
   if _big_sign(res) < 0 { return bigint_add(res, Z(p)) }
   res
}

fn mod_mul(any a, any b, any p) bigint {
   "a * b mod p."
   bigint_mod(bigint_mul(Z(a), Z(b)), Z(p))
}

fn mod_nth_roots_prime(any m, any exp, any p) list {
   "Return a list of roots r such that r^exp = m(mod prime p), for common easy cases.
   Supported fast cases:
   1) gcd(exp, p-1) == 1: unique root r = m^(exp^{-1} mod(p-1))
   2) exp | (p-1) and v_exp(p-1) == 1(i.e. p-1 = exp * t, gcd(exp, t) == 1):
   check residue: m^t == 1. Then one root is r0 = m^(exp^{-1} mod t),
   and all roots are r0 * w^i where w is a primitive exp-th root of unity.
   Returns [] if the case is not handled or no roots exist."
   def pp = Z(p)
   def e = Z(exp)
   if pp <= Z(2) || e <= Z(0) { return [] }
   def pm1 = pp - Z(1)
   def g = gcd(e, pm1)
   def mm = mod(m, pp)
   if g == Z(1) {
      def inv_e = inverse_mod(e, pm1)
      if inv_e == nil { return [] }
      return [power_mod(mm, inv_e, pp)]
   }
   if g != e { return [] }
   if pm1 % e != Z(0) { return [] }
   def t = pm1 / e
   if t % e == Z(0) { return [] }
   if power_mod(mm, t, pp) != Z(1) { return [] }
   def inv_e2 = inverse_mod(e, t)
   if inv_e2 == nil { return [] }
   def r0 = power_mod(mm, inv_e2, pp)
   mut w = nil
   mut base = Z(2)
   while base < Z(200) && w == nil {
      def cand = power_mod(base, t, pp)
      if cand != Z(1) { w = cand }
      base = base + Z(1)
   }
   if w == nil { return [] }
   def e_int = bigint_to_int(e)
   if e_int <= 0 || e_int > (1 << 20) { return [] }
   mut roots = []
   mut i = 0
   mut wi = Z(1)
   while i < e_int {
      roots = roots.append(mod(r0 * wi, pp))
      wi = mod(wi * w, pp)
      i += 1
   }
   roots
}

fn inverse_mod(any a, any m) bigint {
   "Modular inverse: a^-1 mod m."
   def m_big = Z(m)
   if bigint_eq(m_big, Z(0)) { return Z(0) }
   mut t = Z(0)
   mut new_t = Z(1)
   mut r = m_big
   mut new_r = mod(Z(a), m_big)
   while !bigint_eq(new_r, Z(0)) {
      def q = bigint_div(r, new_r)
      def tmp_t = new_t
      new_t = bigint_sub(t, bigint_mul(q, new_t))
      t = tmp_t
      def tmp_r = new_r
      new_r = bigint_sub(r, bigint_mul(q, new_r))
      r = tmp_r
   }
   if !bigint_eq(bigint_abs(r), Z(1)) { return Z(0) }
   if bigint_lt(t, Z(0)) { t = bigint_add(t, m_big) }
   mod(t, m_big)
}

fn _mont_init(any n) any {
   def n_big = Z(n)
   mut bits = bit_length(n_big)
   if bits <= 0 { bits = 1 }
   mut R = bigint_lshift(Z(1), bits)
   if R <= n_big {
      bits += 1
      R = bigint_lshift(Z(1), bits)
   }
   def n_prime = mod(Z(0) - inverse_mod(n, R), R)
   [n_big, bits, R, n_prime]
}

fn _mont_redc(any T, any ctx) bigint {
   def n = ctx.get(0)
   def R = ctx.get(2)
   def n_prime = ctx.get(3)
   def m = mod(bigint_mul(mod(T, R), n_prime), R)
   mut t = bigint_div(bigint_add(Z(T), bigint_mul(m, n)), R)
   if bigint_ge(t, n) { t = bigint_sub(t, n) }
   t
}

fn _mont_mul(any a_bar, any b_bar, any ctx) bigint { _mont_redc(bigint_mul(a_bar, b_bar), ctx) }

fn _mont_to(any a, any ctx) bigint {
   def n, R = ctx.get(0), ctx.get(2)
   mod(bigint_mul(a, R), n)
}

fn _mont_from(any a_bar, any ctx) bigint { _mont_redc(a_bar, ctx) }

@inline
fn _nt_mul_mod_i31(int a, int b, int m) int {
   (a * b) % m
}

fn _nt_pow_mod_i31(int a, int e0, int m) int {
   mut base = a % m
   if base < 0 { base += m }
   mut exp = e0
   mut acc = 1 % m
   while exp > 0 {
      if (exp & 1) != 0 { acc = _nt_mul_mod_i31(acc, base, m) }
      exp = exp >> 1
      if exp > 0 { base = _nt_mul_mod_i31(base, base, m) }
   }
   acc
}

fn _nt_strong_prp_i31(int n, int a) bool {
   if a >= n { return true }
   mut d = n - 1
   mut s = 0
   while (d & 1) == 0 {
      d = d >> 1
      s += 1
   }
   mut x = _nt_pow_mod_i31(a, d, n)
   if x == 1 || x == n - 1 { return true }
   mut r = 1
   while r < s {
      x = _nt_mul_mod_i31(x, x, n)
      if x == n - 1 { return true }
      if x == 1 { return false }
      r += 1
   }
   false
}

fn _nt_is_prime_i31(int n) bool {
   if n < 2 { return false }
   if n == 2 || n == 3 || n == 5 || n == 7 || n == 11 || n == 13 || n == 17 || n == 19 || n == 23 || n == 29 || n == 31 || n == 37 { return true }
   if (n & 1) == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0 || n % 19 == 0 || n % 23 == 0 || n % 29 == 0 || n % 31 == 0 || n % 37 == 0 { return false }
   if !_nt_strong_prp_i31(n, 2) { return false }
   if !_nt_strong_prp_i31(n, 7) { return false }
   _nt_strong_prp_i31(n, 61)
}

fn is_prime(any n) bool {
   "Check if n is prime using Miller-Rabin over the bigint arithmetic layer."
   if is_int(n) && int(n) <= 2147483647 { return _nt_is_prime_i31(int(n)) }
   def nn = Z(n)
   if nn < Z(2) { return false }
   if nn == Z(2) || nn == Z(3) { return true }
   if nn % Z(2) == Z(0) { return false }
   if bit_length(nn) <= 31 { return _nt_is_prime_i31(bigint_to_int(nn)) }
   mut d, s = nn - Z(1), 0
   while d % Z(2) == Z(0) {
      d = d / Z(2)
      s += 1
   }
   def bases = [Z(2), Z(3), Z(5), Z(7), Z(11), Z(13), Z(17), Z(19), Z(23), Z(29), Z(31), Z(37)]
   mut bi = 0
   while bi < bases.len {
      def a = bases.get(bi)
      if a >= nn {
         bi += 1
         continue
      }
      mut x = power_mod(a, d, nn)
      if x == Z(1) {
         bi += 1
         continue
      }
      if x == nn - Z(1) {
         bi += 1
         continue
      }
      mut witness = true
      mut r = 1
      while r < s {
         x = (x * x) % nn
         if x == nn - Z(1) {
            witness = false
            break
         }
         r += 1
      }
      if witness { return false }
      bi += 1
   }
   true
}

fn is_prime_power(any n) bool {
   "Check if n is a prime power."
   is_prime_power(Z(n))
}

fn next_prime(any n) bigint {
   "Next prime after n."
   mut x = Z(n) + Z(1)
   if x <= Z(2) { return Z(2) }
   if x % Z(2) == Z(0) { x = x + Z(1) }
   while !is_prime(x) { x = x + Z(2) }
   x
}

fn prev_prime(any n) bigint {
   "Previous prime before n."
   mut x = Z(n) - Z(1)
   if x < Z(2) { return Z(0) }
   if x == Z(2) { return Z(2) }
   if x % Z(2) == Z(0) { x = x - Z(1) }
   while x >= Z(2) && !is_prime(x) { x = x - Z(2) }
   x
}

fn prime(any n) bigint {
   "The n-th prime(0-indexed)."
   def target = Z(n)
   if target < Z(0) { return Z(0) }
   mut idx = Z(0)
   mut p = Z(2)
   while idx < target {
      p = next_prime(p)
      idx = idx + Z(1)
   }
   p
}

fn prime_range(any start, any end) list {
   "List of primes in [start, end)."
   def start_big = Z(start)
   def end_big = Z(end)
   mut primes = list(0)
   mut p = next_prime(start_big - Z(1))
   while p < end_big {
      primes = primes.append(p)
      p = next_prime(p)
   }
   primes
}

fn primes_first_n(any n) list {
   "First n primes."
   def n_big = Z(n)
   mut primes = list(0)
   mut p = Z(2)
   mut count = Z(0)
   while count < n_big {
      primes = primes.append(p)
      p = next_prime(p)
      count = count + Z(1)
   }
   primes
}

fn _factor_pack(list flat) list {
   mut out = []
   mut i = 0
   while i < flat.len {
      def p = Z(flat.get(i))
      mut e, j = 1, i + 1
      while j < flat.len {
         if Z(flat.get(j)) == p { e += 1 }
         j += 1
      }
      mut seen = false
      j = 0
      while j < out.len {
         if out.get(j).get(0) == p { seen = true }
         j += 1
      }
      if !seen { out = out.append([p, e]) }
      i += 1
   }
   out
}

fn _factor_product(list facs) bigint {
   mut prod = Z(1)
   mut i = 0
   while i < facs.len {
      def ent = facs.get(i)
      def p = Z(ent.get(0))
      mut e = ent.get(1)
      while e > 0 {
         prod = prod * p
         e = e - 1
      }
      i += 1
   }
   prod
}

fn _factor_cache_key(any n) str { bigint_to_str(Z(n)) }

fn _factor_cache_get(any n) any {
   def key = _factor_cache_key(n)
   if _factor_ext_cache.contains(key) { return _factor_ext_cache.get(key, nil) }
   nil
}

fn _factor_cache_set(any n, list facs) list {
   def key = _factor_cache_key(n)
   _factor_ext_cache.set(key, facs)
   facs
}

fn _factor_parse_factordb_body(str body, any n) any {
   def nn = Z(n)
   if !is_str(body) || str_mod.find(body, "\"status\":\"FF\"") < 0 { return nil }
   mut pos = str_mod.find(body, "\"factors\":[")
   if pos < 0 { return nil }
   pos += 11
   mut facs = []
   while pos < body.len {
      def open = str_mod.find_from(body, "[\"", pos)
      if open < 0 { break }
      def p0 = open + 2
      def p1 = str_mod.find_from(body, "\"", p0)
      if p1 < 0 { return nil }
      def comma = str_mod.find_from(body, ",", p1)
      def close = str_mod.find_from(body, "]", p1)
      if comma < 0 || close < 0 || comma > close { return nil }
      facs = facs.append([Z(str_mod.str_slice(body, p0, p1)), str_mod.parse_int(str_mod.str_slice(body, comma + 1, close), 10)])
      pos = close + 1
   }
   (_factor_product(facs) == nn) ? _factor_cache_set(nn, facs) : nil
}

fn _factor_from_factordb(any n) any {
   def nn = Z(n)
   if nn < Z(2) { return [] }
   def cached = _factor_cache_get(nn)
   if cached != nil { return cached }
   def query = str_mod.str_slice(bigint_to_str(nn), 0, bigint_to_str(nn).len)
   mut resp = nil
   try {
      resp = requests_get_parsed("http://factordb.com/api?query=" + query)
   } catch err {
      resp = nil
   }
   mut attempt = 1
   while (resp == nil || resp.get("status", 0) != 200) && attempt < 4 {
      try {
         resp = requests_get_parsed("http://factordb.com/api?query=" + query)
      } catch err {
         resp = nil
      }
      attempt += 1
   }
   if resp == nil || resp.get("status", 0) != 200 { return nil }
   def body = resp.get("body", "")
   if !is_str(body) || body.len == 0 { return nil }
   def parsed_body = _factor_parse_factordb_body(body, nn)
   if parsed_body != nil { return parsed_body }
   def doc = json_decode(body)
   if !is_dict(doc) || doc.get("status", "") != "FF" { return nil }
   def raw = doc.get("factors", nil)
   if !is_list(raw) { return nil }
   mut facs = []
   mut i = 0
   while i < raw.len {
      def ent = raw.get(i)
      if !is_list(ent) || ent.len < 2 { return nil }
      facs = facs.append([Z(ent.get(0)), int(ent.get(1))])
      i += 1
   }
   (_factor_product(facs) == nn) ? _factor_cache_set(nn, facs) : nil
}

fn factordb_factor(any n, bool fallback=false) any {
   "Query FactorDB for a complete prime-power factorization; pass fallback=true to run the built-in schedule when FactorDB is incomplete."
   mut nn = Z(n)
   if nn < Z(0) { nn = -nn }
   if nn < Z(2) { return [] }
   def fd = _factor_from_factordb(nn)
   if fd != nil { return fd }
   fallback ? _factor_local(nn) : nil
}

fn _factor_local(any n) list {
   "Factorization with small-prime trial division and recursive Pollard rho.
   Uses small-prime trial division + recursive Pollard rho.
   Implemented without nested functions to avoid runtime/JIT instability."
   mut nn = Z(n)
   if nn < Z(0) { nn = -nn }
   if nn < Z(2) { return [] }
   def flat = _factor_flat_rec(nn, [])
   _factor_pack(flat)
}

fn _factor_mul_mod(any a, any b, any m) any { (a % m) * (b % m) % m }

fn _factor_rho_f(any n, any x, any c) any { (_factor_mul_mod(x, x, n) + c) % n }

fn _factor_pollard_rho(any n) bigint {
   if n % Z(2) == Z(0) { return Z(2) }
   if n % Z(3) == Z(0) { return Z(3) }
   mut c = Z(1)
   while c <= Z(256) {
      mut x, y = Z(2) + c, x
      mut d = Z(1)
      mut it = 0
      while d == Z(1) && it < 2000000 {
         x, y = _factor_rho_f(n, x, c), _factor_rho_f(n, _factor_rho_f(n, y, c), c)
         def diff = (x > y) ? (x - y) : (y - x)
         d = gcd(diff, n)
         it += 1
      }
      if d > Z(1) && d < n { return d }
      c = c + Z(1)
   }
   Z(0)
}

fn _factor_flat_rec(any n, list flat) list {
   if n == Z(1) { return flat }
   if is_prime(n) { return flat.append(n) }
   def small_primes = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47]
   mut nn = n
   mut out = flat
   mut i = 0
   while i < small_primes.len {
      def p = Z(small_primes.get(i))
      while nn % p == Z(0) {
         nn = nn / p
         out = out.append(p)
      }
      i += 1
   }
   if nn == Z(1) { return out }
   if is_prime(nn) { return out.append(nn) }
   def f = _factor_pollard_rho(nn)
   if f == Z(0) || f == nn {
      mut k = Z(53)
      while k * k <= nn && k <= Z(200000) {
         if nn % k == Z(0) {
            out = _factor_flat_rec(k, out)
            return _factor_flat_rec(nn / k, out)
         }
         k = k + Z(2)
      }
      return out.append(nn)
   }
   out = _factor_flat_rec(f, out)
   _factor_flat_rec(nn / f, out)
}

fn factor(any n, bool fdb=false, bool reserved=false) list {
   "Factor n into prime powers: [[p1, e1], [p2, e2], ...].
   Uses the built-in schedule ; fdb is accepted for compatibility and does not add a network dependency."
   mut nn = Z(n)
   if nn < Z(0) { nn = -nn }
   if nn < Z(2) { return [] }
   if is_prime(nn) { return [[nn, 1]] }
   if fdb {
      def fd = factordb_factor(nn, false)
      if fd != nil { return fd }
   }
   _factor_local(nn)
}

fn factorint(any n, bool fdb=false, bool reserved=false) dict {
   "Factor n as dict {prime: exponent}."
   mut facs = factor(n, fdb, reserved)
   mut d = dict()
   mut i = 0
   while i < facs.len {
      def ent = facs.get(i)
      d = d.set(ent.get(0), ent.get(1))
      i += 1
   }
   d
}

fn divisors(any n, bool fdb=false, bool reserved=false) list {
   "List of all divisors of n."
   mut facs = factor(n, fdb, reserved)
   mut ds = [Z(1)]
   mut i = 0
   while i < facs.len {
      def ent = facs.get(i)
      def p = ent.get(0)
      def e = ent.get(1)
      mut next_ds = []
      mut di = 0
      while di < ds.len {
         def base = ds.get(di)
         mut pe = Z(1)
         mut k = 0
         while k <= e {
            next_ds = next_ds.append(base * pe)
            pe = pe * p
            k += 1
         }
         di += 1
      }
      ds = next_ds
      i += 1
   }
   ds
}

fn euler_phi(any n) bigint {
   "Euler's totient function."
   def nn = Z(n)
   if nn <= Z(0) { return Z(0) }
   mut res = nn
   mut facs = factor(nn)
   mut i = 0
   while i < facs.len {
      def p = facs.get(i).get(0)
      res = (res / p) * (p - Z(1))
      i += 1
   }
   res
}

fn carmichael_lambda(any n) bigint {
   "Carmichael lambda function."
   def nn = Z(n)
   if nn <= Z(0) { return Z(0) }
   mut facs = factor(nn)
   mut res = Z(1)
   mut i = 0
   while i < facs.len {
      def ent = facs.get(i)
      def p = ent.get(0)
      def e = ent.get(1)
      mut term = Z(1)
      if p == Z(2) && e >= 3 { term = Z(1) << Z(e - 2) } else {
         mut pk_1 = Z(1)
         mut j = 1
         while j < e {
            pk_1 = pk_1 * p
            j += 1
         }
         term = (p - Z(1)) * pk_1
      }
      res = lcm(res, term)
      i += 1
   }
   res
}

fn moebius(any n) int {
   "Mobius function."
   def nn = Z(n)
   if nn == Z(1) { return 1 }
   mut facs = factor(nn)
   mut i = 0
   while i < facs.len {
      if facs.get(i).get(1) > 1 { return 0 }
      i += 1
   }
   (facs.len % 2 == 0) ? 1 : -1
}

fn legendre(any a, any p) int {
   "Legendre symbol(a/p)."
   __bigint_legendre(Z(a), Z(p))
}

fn jacobi(any a, any n) int {
   "Jacobi symbol(a/n)."
   __bigint_jacobi(Z(a), Z(n))
}

fn kronecker(any a, any n) int {
   "Kronecker symbol(a/n)."
   __bigint_kronecker(Z(a), Z(n))
}

fn crt(list rems, list mods) any {
   "Chinese Remainder Theorem. Supports compatible non-coprime moduli."
   if rems.len != mods.len { return nil }
   if rems.len == 0 { return Z(0) }
   mut x, m = mod(rems.get(0), mods.get(0)), Z(mods.get(0))
   mut i = 1
   while i < rems.len {
      def mi, ri = Z(mods.get(i)), mod(rems.get(i), mi)
      def g = gcd(m, mi)
      def delta = ri - mod(x, mi)
      if mod(delta, g) != Z(0) { return nil }
      def m0 = m / g
      def m1 = mi / g
      def inv = inverse_mod(m0, m1)
      if bigint_eq(inv, Z(0)) { return nil }
      def t = mod((delta / g) * inv, m1)
      def next_m = m * m1
      x = mod(x + m * t, next_m)
      m = next_m
      i += 1
   }
   x
}

fn CRT(list rems, list mods) any {
   "CRT alias."
   crt(rems, mods)
}

fn random_prime(any bits) bigint {
   "Random prime with given bit length."
   def b = int(bits)
   if b <= 1 { return Z(2) }
   def lo = Z(1) << Z(b - 1)
   def hi = (Z(1) << Z(b)) - Z(1)
   mut x = randint(lo, hi)
   if x % Z(2) == Z(0) { x = x + Z(1) }
   while !is_prime(x) {
      x = x + Z(2)
      if x > hi { x = lo + Z(1) }
   }
   x
}

fn randint(any a, any b) bigint {
   "Random integer in [a, b]."
   def a_big, b_big = Z(a), Z(b)
   def range_big = bigint_add(bigint_sub(b_big, a_big), Z(1))
   bigint_add(a_big, bigint_mod(bigint_random(range_big), range_big))
}

fn long_to_bytes(any n, int length=0) list<int> {
   "Convert integer to bytes list(big-endian), left-padding to length when provided."
   def raw = bigint_to_bytes(Z(n))
   if length <= raw.len { return raw }
   mut out = []
   mut i = 0
   while i < length - raw.len {
      out = out.append(0)
      i += 1
   }
   i = 0
   while i < raw.len {
      out = out.append(raw[i])
      i += 1
   }
   out
}

fn bytes_to_long(list bytes) bigint {
   "Convert bytes list to integer(big-endian)."
   mut out = Z(0)
   mut i = 0
   while i < bytes.len {
      out = out * Z(256) + Z(bytes.get(i) & 255)
      i += 1
   }
   out
}

fn xor_bytes(list a, list b) list {
   "XOR two byte lists."
   def n = a.len < b.len ? a.len : b.len
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < n {
      __store_item_fast(out, i, (a.get(i) ^^ b.get(i)) & 255)
      i += 1
   }
   out
}

fn str_to_bytes(str s) list<int> {
   "Convert string to bytes list."
   def n = s.len
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < n {
      __store_item_fast(out, i, load8(s, i) & 255)
      i += 1
   }
   out
}

fn bytes_to_str(list bytes) str {
   "Convert bytes list to string."
   def n = bytes.len
   def p = malloc(n)
   if !p { return "" }
   mut i = 0
   while i < n {
      store8(p, bytes.get(i) & 255, i)
      i += 1
   }
   init_str(p, n)
}

fn isqrt(any n) bigint {
   "Integer square root."
   def n_big = Z(n)
   if bigint_lt(n_big, Z(0)) { return Z(0) }
   if bigint_eq(n_big, Z(0)) { return Z(0) }
   Z(__bigint_isqrt(n_big))
}

fn nth_root(any n, any k) bigint {
   "Integer k-th root."
   bigint_nth_root(Z(n), k)
}

fn tonelli_shanks(any n, any p) bigint {
   "Tonelli-Shanks: compute a square root of n mod p. Returns -1 if n is not a QR mod p."
   def n_big, p_big = mod(Z(n), Z(p)), Z(p)
   def zero = Z(0)
   def one = Z(1)
   if bigint_eq(n_big, zero) { return zero }
   if legendre(n_big, p_big) != 1 { return bigint_from_int(-1) }
   def p_mod4 = bigint_to_int(bigint_mod(p_big, Z(4)))
   if p_mod4 == 3 { return power_mod(n_big, bigint_div(bigint_add(p_big, one), Z(4)), p_big) }
   mut Q, S = bigint_sub(p_big, one), 0
   while bigint_to_int(bigint_mod(Q, Z(2))) == 0 {
      Q = bigint_div(Q, Z(2))
      S += 1
   }
   mut z = Z(2)
   while legendre(z, p_big) != -1 { z = bigint_add(z, one) }
   mut M, c = S, power_mod(z, Q, p_big)
   mut t, R = power_mod(n_big, Q, p_big), power_mod(n_big, bigint_div(bigint_add(Q, one), Z(2)), p_big)
   while true {
      if bigint_eq(t, one) { return R }
      mut i = 1
      mut tmp = bigint_mul(t, t)
      tmp = bigint_mod(tmp, p_big)
      while !bigint_eq(tmp, one) {
         tmp = bigint_mul(tmp, tmp)
         tmp = bigint_mod(tmp, p_big)
         i += 1
      }
      mut b, j = c, 0
      while j < M - i - 1 {
         b = bigint_mod(bigint_mul(b, b), p_big)
         j += 1
      }
      M, c = i, bigint_mod(bigint_mul(b, b), p_big)
      t, R = bigint_mod(bigint_mul(t, c), p_big), bigint_mod(bigint_mul(R, b), p_big)
   }
   bigint_from_int(-1)
}

fn mod_quadratic_roots_prime(any a, any b, any c, any p) list {
   "Solve a*x^2 + b*x + c = 0 over prime field F_p. Returns distinct roots."
   def pp = Z(p)
   def aa = mod(a, pp)
   def bb = mod(b, pp)
   def cc = mod(c, pp)
   mut roots = []
   if aa == Z(0) {
      if bb == Z(0) {
         if cc == Z(0) { roots = roots.append(Z(0)) }
         return roots
      }
      def binv = inverse_mod(bb, pp)
      if binv == Z(0) { return roots }
      return roots.append(mod(-cc * binv, pp))
   }
   def disc = mod(bb * bb - Z(4) * aa * cc, pp)
   def sr = tonelli_shanks(disc, pp)
   if sr == Z(-1) { return roots }
   def denom_inv = inverse_mod(Z(2) * aa, pp)
   if denom_inv == Z(0) { return roots }
   def r0 = mod((-bb + sr) * denom_inv, pp)
   roots = roots.append(r0)
   def r1 = mod((-bb - sr) * denom_inv, pp)
   if r1 != r0 { roots = roots.append(r1) }
   roots
}

fn cipolla(any n, any p) bigint {
   "Cipolla's algorithm: compute sqrt(n) mod p. Returns -1 if n is a non-residue."
   def n_big, p_big = mod(Z(n), Z(p)), Z(p)
   def zero = Z(0)
   def one = Z(1)
   if bigint_eq(n_big, zero) { return zero }
   if legendre(n_big, p_big) != 1 { return bigint_from_int(-1) }
   mut a = one
   mut w2 = zero
   while true {
      w2 = mod(bigint_sub(bigint_mul(a, a), n_big), p_big)
      if legendre(w2, p_big) == -1 { break }
      a = bigint_add(a, one)
   }
   def exp = bigint_div(bigint_add(p_big, one), Z(2))
   mut r0, r1 = one, zero
   mut b0, b1 = a, one
   mut e = exp
   while bigint_gt(e, zero) {
      if bigint_to_int(bigint_mod(e, Z(2))) == 1 {
         def new_r0 = mod(bigint_add(bigint_mul(r0, b0), bigint_mul(bigint_mul(r1, b1), w2)), p_big)
         def new_r1 = mod(bigint_add(bigint_mul(r0, b1), bigint_mul(r1, b0)), p_big)
         r0, r1 = new_r0, new_r1
      }
      def new_b0 = mod(bigint_add(bigint_mul(b0, b0), bigint_mul(bigint_mul(b1, b1), w2)), p_big)
      def new_b1 = mod(bigint_mul(bigint_mul(b0, b1), Z(2)), p_big)
      b0, b1 = new_b0, new_b1
      e = bigint_div(e, Z(2))
   }
   r0
}

fn continued_fraction(any p, any q) list {
   "Continued fraction expansion of p/q. Returns list of partial quotients [a0, a1, ...]."
   mut num = Z(p)
   mut den = Z(q)
   def zero = Z(0)
   mut cf = list(0)
   while !bigint_eq(den, zero) {
      def a = bigint_div(num, den)
      cf = cf.append(a)
      def r = bigint_mod(num, den)
      num = den
      den = r
   }
   cf
}

fn cf_convergents(list cf) list {
   "Convergents of a continued fraction. Returns list of [p, q] pairs."
   def n = cf.len
   if n == 0 { return list(0) }
   mut result = list(0)
   mut p_prev = Z(1)
   mut q_prev = Z(0)
   mut p_curr = cf[0]
   mut q_curr = Z(1)
   result = result.append([p_curr, q_curr])
   mut i = 1
   while i < n {
      def a = cf[i]
      def p_next = bigint_add(bigint_mul(a, p_curr), p_prev)
      def q_next = bigint_add(bigint_mul(a, q_curr), q_prev)
      p_prev, q_prev = p_curr, q_curr
      p_curr, q_curr = p_next, q_next
      result = result.append([p_curr, q_curr])
      i += 1
   }
   result
}

fn rational_reconstruction(any t, any m) any {
   "Rational reconstruction: find p/q such that p/q ≡ t(mod m) and |p|, q < sqrt(m/2).
   Returns [p, q] or nil if no solution exists."
   def m_big = Z(m)
   def zero = Z(0)
   def one = Z(1)
   if m_big <= zero { return nil }
   def t_big = mod(Z(t), m_big)
   mut r0, r1 = m_big, t_big
   mut s0, s1 = zero, one
   def bound = Z(__bigint_isqrt(bigint_div(m_big, Z(2))))
   while bigint_gt(r1, bound) {
      def q = bigint_div(r0, r1)
      def r2 = bigint_sub(r0, bigint_mul(q, r1))
      def s2 = bigint_sub(s0, bigint_mul(q, s1))
      r0, r1 = r1, r2
      s0, s1 = s1, s2
   }
   if bigint_eq(s1, zero) { return nil }
   def denom = bigint_abs(s1)
   if bigint_gt(denom, bound) { return nil }
   def p_val = (bigint_lt(s1, zero)) ? bigint_neg(r1) : r1
   if bigint_gt(bigint_abs(p_val), bound) { return nil }
   if gcd(p_val, denom) != one { return nil }
   if gcd(denom, m_big) != one { return nil }
   def den_inv = inverse_mod(denom, m_big)
   if den_inv == zero { return nil }
   if mod(p_val * den_inv - t_big, m_big) != zero { return nil }
   [p_val, denom]
}

fn mqrr_rational_reconstruction(any u, any m, any T) any {
   "Maximal quotient rational reconstruction. Returns [n, d] or nil."
   def uu, mm, tt = Z(u), Z(m), Z(T)
   if uu < Z(0) || mm <= uu || tt <= Z(0) { return nil }
   if uu == Z(0) {
      if mm > tt { return [Z(0), Z(1)] }
      return nil
   }
   mut n = Z(0)
   mut d = Z(0)
   mut bestT = tt
   mut t0, r0 = Z(0), mm
   mut t1, r1 = Z(1), uu
   while r1 != Z(0) && r0 > bestT {
      def q = r0 / r1
      if q > bestT {
         n = r1
         d = t1
         bestT = q
      }
      def nr = r0 - q * r1
      def nt = t0 - q * t1
      r0 = r1
      r1 = nr
      t0 = t1
      t1 = nt
   }
   if d != Z(0) && gcd(n, d) == Z(1) { return [n, d] }
   nil
}

fn is_smooth(any n, any B) bool {
   "Returns true if n is B-smooth(all prime factors <= B)."
   def n_big = bigint_abs(Z(n))
   def one = Z(1)
   if bigint_le(n_big, one) { return true }
   mut rem = n_big
   mut p = Z(2)
   def B_big = Z(B)
   while bigint_le(p, B_big) {
      while bigint_eq(bigint_mod(rem, p), Z(0)) { rem = bigint_div(rem, p) }
      if bigint_le(rem, one) { return true }
      p = next_prime(p)
   }
   bigint_le(rem, one)
}

fn smooth_part(any n, any B) bigint {
   "Returns the B-smooth part of n(product of all prime factors <= B with their full multiplicity)."
   def n_big = bigint_abs(Z(n))
   def one = Z(1)
   mut rem = n_big
   mut smooth = Z(1)
   mut p = Z(2)
   def B_big = Z(B)
   while bigint_le(p, B_big) && bigint_gt(rem, one) {
      while bigint_eq(bigint_mod(rem, p), Z(0)) {
         rem = bigint_div(rem, p)
         smooth = bigint_mul(smooth, p)
      }
      p = next_prime(p)
   }
   smooth
}

fn bigint_to_bytes(any a) list<int> {
   "Convert BigInt to bytes list(big-endian)."
   mut x = bigint_abs(Z(a))
   if x == Z(0) { return [0] }
   def n = (bit_length(x) + 7) / 8
   mut out = list(n)
   __list_set_len(out, n)
   mut i = n
   while i > 0 {
      i -= 1
      __store_item_fast(out, i, int(x & Z(255)))
      x = x >> Z(8)
   }
   out
}

fn bytes_to_bigint(list bytes) bigint {
   "Convert bytes list to BigInt(big-endian)."
   def b256 = Z(256)
   mut result = Z(0)
   mut i = 0
   while i < bytes.len {
      result = bigint_add(bigint_mul(result, b256), Z(bytes[i]))
      i += 1
   }
   result
}

impl bigint {
   @inline
   fn mod(bigint a, any m) bigint { mod(a, m) }
   @inline
   fn powmod(bigint a, any exp, any m) bigint { power_mod(a, exp, m) }
   @inline
   fn invmod(bigint a, any m) bigint { inverse_mod(a, m) }
   @inline
   fn gcd(bigint a, any b) bigint { gcd(a, b) }
   @inline
   fn lcm(bigint a, any b) bigint { lcm(a, b) }
   @inline
   fn xgcd(bigint a, any b) list { xgcd(a, b) }
   @inline
   fn divmod(bigint a, any b) list { bigint_divmod(a, b) }
   @inline
   fn sqrt(bigint a) bigint { isqrt(a) }
   @inline
   fn sqrt_mod(bigint a, any p) bigint { tonelli_shanks(a, p) }
   @inline
   fn quadratic_roots_mod(bigint a, any b, any c, any p) list { mod_quadratic_roots_prime(a, b, c, p) }
   @inline
   fn root(bigint a, any k) bigint { nth_root(a, k) }
   @inline
   fn nth_root(bigint a, any k) bigint { nth_root(a, k) }
   @inline
   fn is_square(bigint a) bool { is_square(a) == 1 }
   @inline
   fn is_prime(bigint a) bool { is_prime(a) }
   @inline
   fn next_prime(bigint a) bigint { next_prime(a) }
   @inline
   fn prev_prime(bigint a) bigint { prev_prime(a) }
   @inline
   fn factor(bigint a) list { factor(a) }
   @inline
   fn phi(bigint a) bigint { euler_phi(a) }
   @inline
   fn carmichael(bigint a) bigint { carmichael_lambda(a) }
   @inline
   fn bytes(bigint a) list<int> { bigint_to_bytes(a) }
   @inline
   fn hex(bigint a) str { bigint_to_hex(a) }
   @inline
   fn as_bytes(bigint a) list<int> { bigint_to_bytes(a) }
   @inline
   fn as_hex(bigint a) str { bigint_to_hex(a) }
   @inline
   fn bitlen(bigint a) int { bit_length(a) }
   fn xor(bigint a, bigint b) bigint { bigint_xor(a, b) }
   fn bxor(bigint a, bigint b) bigint { bigint_xor(a, b) }
}

impl int {
   @inline
   fn Z(int n) bigint { Z(n) }
   @inline
   fn bigint(int n) bigint { Z(n) }
   @inline
   fn mod(int a, any m) bigint { mod(a, m) }
   @inline
   fn powmod(int a, any exp, any m) bigint { power_mod(a, exp, m) }
   @inline
   fn invmod(int a, any m) bigint { inverse_mod(a, m) }
   @inline
   fn gcd(int a, any b) bigint { gcd(a, b) }
   @inline
   fn lcm(int a, any b) bigint { lcm(a, b) }
   @inline
   fn xgcd(int a, any b) list { xgcd(a, b) }
   @inline
   fn sqrt_mod(int a, any p) bigint { tonelli_shanks(a, p) }
   @inline
   fn quadratic_roots_mod(int a, any b, any c, any p) list { mod_quadratic_roots_prime(a, b, c, p) }
   @inline
   fn is_prime(int a) bool { is_prime(a) }
   @inline
   fn next_prime(int a) bigint { next_prime(a) }
   @inline
   fn prev_prime(int a) bigint { prev_prime(a) }
   @inline
   fn factor(int a) list { factor(a) }
   @inline
   fn phi(int a) bigint { euler_phi(a) }
   @inline
   fn bitlen(int a) int { bit_length(a) }
}

impl str {
   @inline
   fn Z(str s) bigint { Z(s) }
   @inline
   fn bigint(str s) bigint { Z(s) }
   @inline
   fn hex_int(str s) int { hex_to_int(s) }
   @inline
   fn hex_bigint(str s) bigint { hex_to_bigint(s) }
}

fn extended_gcd(int a, int b) list {
   "Extended Euclidean(plain int). Returns [g, x, y] s.t. a*x + b*y = g = gcd(a,b)."
   if b == 0 { return [a, 1, 0] }
   def res = extended_gcd(b, a % b)
   def g = res[0]
   def x1 = res[1]
   def y1 = res[2]
   [g, y1, x1 - (a / b) * y1]
}

fn bezout_coefficients(int a, int b) list {
   "Returns [x, y] such that a*x + b*y = gcd(a, b)."
   def res = extended_gcd(a, b)
   [res[1], res[2]]
}

fn solve_linear_congruence(int a, int b, int m) list {
   "Solve a*x = b(mod m). Returns list of solutions mod m, or [] if none."
   def res = extended_gcd(a, m)
   def g = res[0]
   if b % g != 0 { return [] }
   def x0 = (res[1] * (b / g)) % m
   mut sols = []
   mut i = 0
   while i < g {
      sols = sols.append((x0 + i * (m / g)) % m)
      i += 1
   }
   sols
}

fn crt2(int a1, int m1, int a2, int m2) int {
   "CRT for two congruences x = a1 mod m1, x = a2 mod m2. Returns x mod lcm(m1,m2), or 0 if inconsistent."
   def res = extended_gcd(m1, m2)
   def g = res[0]
   def p = res[1]
   if (a2 - a1) % g != 0 { return 0 }
   def lcm_val = m1 / g * m2
   ((a1 + m1 * ((a2 - a1) / g % (m2 / g) * p % (m2 / g))) % lcm_val + lcm_val) % lcm_val
}

fn garner(list rems, list mods) any {
   "Garner's algorithm for CRT reconstruction(plain int). Returns unique solution mod product(mods)."
   def n = rems.len
   if n == 0 { return 0 }
   mut coeffs = []
   mut i = 0
   while i < n {
      coeffs = coeffs.append(rems[i])
      i += 1
   }
   i = 1
   while i < n {
      mut j = 0
      while j < i {
         def mi, mj = mods[i], mods[j]
         def inv = inverse_mod(mj, mi)
         def ci = coeffs[i]
         def cj = coeffs[j]
         mut new_ci = (ci - cj) % mi
         if new_ci < 0 { new_ci = new_ci + mi }
         new_ci = (new_ci * inv) % mi
         coeffs[i] = new_ci
         j += 1
      }
      i += 1
   }
   mut result = 0
   mut M = 1
   i = 0
   while i < n {
      result = result + coeffs[i] * M
      M = M * mods[i]
      i += 1
   }
   result
}

fn int_to_bytes(any n) list<int> {
   "Convert non-negative integer to big-endian byte list."
   bigint_to_bytes(Z(n))
}

fn bytes_to_int(list bytes) bigint {
   "Convert big-endian byte list to integer."
   bytes.long
}

fn int_to_hex(int n) str {
   "Convert integer to lowercase hex string with '0x' prefix."
   if n == 0 { return "0x0" }
   def hex_chars = "0123456789abcdef"
   mut nib = 0
   mut tmp = n
   while tmp > 0 { tmp = tmp >> 4 nib += 1 }
   def total = nib + 2
   def out = malloc(total + 1)
   if !out { return "" }
   init_str(out, total)
   store8(out, 48, 0) store8(out, 120, 1)
   mut i, v = total - 1, n
   while v > 0 {
      store8(out, load8(hex_chars, v & 15), i)
      v = v >> 4
      i -= 1
   }
   store8(out, 0, total)
   out
}

fn bigint_to_hex(any n) str {
   "Convert a non-negative BigInt to lowercase hex without a prefix."
   mut v = Z(n)
   if v == Z(0) { return "0" }
   def hex_chars = "0123456789abcdef"
   mut nib = 0
   mut tmp = v
   while tmp > Z(0) {
      tmp = tmp >> Z(4)
      nib += 1
   }
   def out = malloc(nib + 1)
   if !out { return "" }
   init_str(out, nib)
   mut i = nib - 1
   while v > Z(0) {
      def digit = bigint_to_int(v % Z(16))
      store8(out, load8(hex_chars, digit), i)
      v = v >> Z(4)
      i -= 1
   }
   store8(out, 0, nib)
   out
}

fn hex_to_int(str s) int {
   "Convert hex string(with or without 0x prefix) to integer."
   def n = s.len
   mut start = 0
   if n >= 2 && load8(s, 0) == 48 && (load8(s, 1) == 120 || load8(s, 1) == 88) { start = 2 }
   mut result = 0
   mut i = start
   while i < n {
      def c, v = load8(s, i), (c >= 97) ? (c - 87) : ((c >= 65) ? (c - 55) : (c - 48))
      result = (result << 4) | v
      i += 1
   }
   result
}

fn _hex_skip_byte(int c) bool {
   case c {
      9, 10, 13, 32, 58, 95 -> true
      _ -> false
   }
}

fn hex_to_bigint(str s) bigint {
   "Convert hex string(with or without 0x prefix) to BigInt."
   def n = s.len
   mut start = 0
   if n >= 2 && load8(s, 0) == 48 && (load8(s, 1) == 120 || load8(s, 1) == 88) { start = 2 }
   mut result = Z(0)
   mut i = start
   while i < n {
      def c = load8(s, i)
      if _hex_skip_byte(c) {
         i += 1
         continue
      }
      def v = (c >= 97) ? (c - 87) : ((c >= 65) ? (c - 55) : (c - 48))
      result = bigint_add(bigint_mul(result, Z(16)), Z(v))
      i += 1
   }
   result
}

fn bit_length(any n) int {
   "Number of bits needed to represent abs(n). Returns 0 for n=0. Uses GMP-backed builtin."
   if n == 0 { return 0 }
   __bigint_bitlen(Z(n))
}

fn is_square(any n) int {
   "Returns 1 if n is a perfect square, 0 otherwise. Uses GMP-backed builtin."
   if n < 0 { return 0 }
   __bigint_is_perfect_square(Z(n))
}

fn is_perfect_square(any n) bool {
   "Returns true if n is a perfect square."
   is_square(n) == 1
}
