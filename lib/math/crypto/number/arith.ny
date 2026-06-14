;; Keywords: number-theory arith math crypto
;; Crypto number-theory routines for number-theory arithmetic used by crypto attacks.
;; References:
;; - std.math.crypto.number
;; - std.math.crypto
module std.math.crypto.number.arith(int_to_bits_le, bits_to_int_le, floor_div, ceil_div, square_root_or_nil, symmetric_mod, factor_divisors, make_square_free, largest_prime_factor, primitive_pythagorean_triple_for_area, modinv_range, modinv_list, roots_of_unity_mod_prime, rth_roots_mod_prime, least_significant_bits, two_adic_valuation, mod_sqrt_power2, is_blum_prime, has_blum_prime, random_blum_prime)
use std.math.nt
use std.math.crypto.gf

fn int_to_bits_le(any x, int count) list {
   "Convert integer x to count little-endian bits."
   mut bits = []
   mut n = x
   mut i = 0
   while i < count {
      bits = bits.append(n & 1)
      n = n >> 1
      i += 1
   }
   bits
}

fn bits_to_int_le(list bits, any count=nil) any {
   "Convert little-endian bits to an integer."
   def limit = count == nil ? bits.len : int(count)
   mut x, i = 0, 0
   while i < limit && i < bits.len {
      x = x | ((bits.get(i) & 1) << i)
      i += 1
   }
   x
}

fn floor_div(any a, any b) any {
   "Integer floor(a / b), including negative values."
   def q, r = a / b, a % b
   ((r != 0) && ((r > 0) != (b > 0))) ? (q - 1) : q
}

fn ceil_div(any a, any b) any {
   "Integer ceil(a / b), including negative values."
   0 - floor_div(0 - a, b)
}

fn square_root_or_nil(any x) any {
   "Return sqrt(x) when x is a perfect square, otherwise nil."
   if x < 0 { return nil }
   def y = isqrt(x)
   (y * y == x) ? y : nil
}

fn two_adic_valuation(any x, int zero_bits=0) int {
   "Return v2(x), the exponent of the largest power of two dividing x.
   For x == 0, returns zero_bits."
   mut n = Z(x)
   if n == Z(0) { return zero_bits }
   if n < Z(0) { n = -n }
   mut v = 0
   while (n & Z(1)) == Z(0) {
      n = n >> Z(1)
      v += 1
   }
   v
}

fn _arith_append_unique(list xs, any x) list {
   xs.contains(x) ? xs : xs.append(x)
}

fn _mod_sqrt_power2_odd(any a, int bits) list {
   if bits == 1 { return [Z(1)] }
   if bits == 2 { return [Z(1), Z(3)] }
   if mod(a, Z(8)) != Z(1) { return [] }
   def modulus = Z(1) << bits
   mut out = []
   mut base_i = 0
   while base_i < 2 {
      mut x = base_i == 0 ? Z(1) : Z(3)
      mut k = 3
      while k < bits {
         def bit = ((x * x - a) / (Z(1) << k)) & Z(1)
         if bit != Z(0) { x = x + (Z(1) << (k - 1)) }
         k += 1
      }
      x = mod(x, modulus)
      out = _arith_append_unique(out, x)
      out = _arith_append_unique(out, mod(modulus - x, modulus))
      base_i += 1
   }
   out
}

fn mod_sqrt_power2(any a, int bits, int max_roots=4096) list {
   "Return roots x with x*x == a mod 2^bits.
   Odd a has roots only when a == 1 mod 8. Even roots are enumerated only when
   the lift count is at most max_roots."
   if bits <= 0 { return [Z(0)] }
   def modulus = Z(1) << bits
   def aa = mod(Z(a), modulus)
   if bits == 1 { return [aa & Z(1)] }
   if bits == 2 {
      if aa == Z(0) { return [Z(0), Z(2)] }
      if aa == Z(1) { return [Z(1), Z(3)] }
      return []
   }
   if (aa & Z(1)) == Z(1) { return _mod_sqrt_power2_odd(aa, bits) }
   if aa == Z(0) {
      def step = Z(1) << ((bits + 1) / 2)
      def count = int(modulus / step)
      if count > max_roots { return [] }
      mut out = []
      mut i = 0
      while i < count {
         out = out.append(step * Z(i))
         i += 1
      }
      return out
   }
   def v = two_adic_valuation(aa, bits)
   if (v & 1) == 1 { return [] }
   def half = v / 2
   def reduced_bits = bits - v
   def reduced = aa >> v
   def base_roots = _mod_sqrt_power2_odd(reduced, reduced_bits)
   def lift_count = int(Z(1) << half)
   if base_roots.len * lift_count > max_roots { return [] }
   mut out = []
   mut i = 0
   while i < base_roots.len {
      mut j = 0
      while j < lift_count {
         def root = mod((base_roots.get(i) + (Z(j) << reduced_bits)) << half, modulus)
         out = _arith_append_unique(out, root)
         j += 1
      }
      i += 1
   }
   out
}

fn symmetric_mod(any x, any m) any {
   "Reduce x into the symmetric interval around zero: [-m/2, m/2)."
   mod(x + m + (m / 2), m) - (m / 2)
}

fn factor_divisors(list factors) list {
   "Compute all divisors from factor list [[p,e], ...]."
   mut out = [Z(1)]
   mut i = 0
   while i < factors.len {
      def p, e = factors.get(i).get(0), factors.get(i).get(1)
      def old = clone(out)
      mut j = 0
      mut pk = Z(1)
      while j < e {
         pk = pk * p
         mut k = 0
         while k < old.len {
            out = out.append(old.get(k) * pk)
            k += 1
         }
         j += 1
      }
      i += 1
   }
   out
}

fn make_square_free(any x, any factors=nil) any {
   "Remove square factors from x. factors may be [[p,e], ...]; omitted factors are computed."
   mut y = Z(x)
   def fs = factors == nil ? factor(y) : factors
   mut i = 0
   while i < fs.len {
      def p, e = fs.get(i).get(0), fs.get(i).get(1)
      mut j = 0
      while j < e / 2 {
         y = y / (p * p)
         j += 1
      }
      i += 1
   }
   y
}

fn largest_prime_factor(any n) any {
   "Return the largest prime factor of |n|, or nil when |n| < 2."
   def fs = factor(n)
   if fs.len == 0 { return nil }
   mut best = Z(0)
   mut i = 0
   while i < fs.len {
      def p = Z(fs.get(i).get(0))
      if p > best { best = p }
      i += 1
   }
   best
}

fn primitive_pythagorean_triple_for_area(any area, int max_m=10000) list {
   "Find a primitive Pythagorean triple [x, y, z] with x < y < z and x*y/2 == area.
   Returns [] when no triple is found for 2 <= m < max_m."
   def A = Z(area)
   mut m = 2
   while m < max_m {
      mut n = 1
      while n < m {
         if ((m - n) & 1) == 1 && gcd(m, n) == 1 {
            def mz = Z(m)
            def nz = Z(n)
            if mz * nz * (mz * mz - nz * nz) == A {
               mut x = mz * mz - nz * nz
               mut y = Z(2) * mz * nz
               if x > y { def t = x x = y y = t }
               def z = mz * mz + nz * nz
               if gcd(gcd(x, y), z) == Z(1) { return [x, y, z] }
            }
         }
         n += 1
      }
      m += 1
   }
   []
}

fn modinv_range(any n, any p) list {
   "Return inverses of 1..n-1 modulo p using the linear-time recurrence."
   if n <= 1 { return [] }
   mut inv = []
   mut i = 0
   while i < n { inv = inv.append(0) i += 1 }
   inv[1] = 1
   mut out = [1]
   i = 2
   while i < n {
      def v = mod((p - (p / i)) * inv.get(p % i), p)
      inv[i] = v
      out = out.append(v)
      i += 1
   }
   out
}

fn modinv_list(list values, any p) list {
   "Return modular inverses for a list of values; returns [] if any value is not invertible."
   def n = values.len
   if n == 0 { return [] }
   mut out = []
   mut i = 0
   while i < n {
      def inv = inverse_mod(values.get(i), p)
      if inv == 0 { return [] }
      out = out.append(inv)
      i += 1
   }
   out
}

fn _arith_unique_prime_factors(any n) list {
   def fs = factor(n)
   mut out = []
   mut i = 0
   while i < fs.len {
      out = out.append(fs.get(i).get(0))
      i += 1
   }
   out
}

fn _arith_primitive_root_prime(any p) any {
   if p == 2 { return 1 }
   def phi = p - 1
   def primes = _arith_unique_prime_factors(phi)
   mut g = 2
   while g < p {
      mut ok = true
      mut i = 0
      while i < primes.len {
         if power_mod(g, phi / primes.get(i), p) == 1 { ok = false }
         i += 1
      }
      if ok { return g }
      g += 1
   }
   0
}

fn roots_of_unity_mod_prime(any p, any r) list {
   "Return the r-th roots of unity modulo prime p. Requires r | p-1."
   if (p - 1) % r != 0 { return [] }
   def g = _arith_primitive_root_prime(p)
   if g == 0 { return [] }
   def w = power_mod(g, (p - 1) / r, p)
   mut out = []
   mut i = 0
   while i < r {
      out = out.append(power_mod(w, i, p))
      i += 1
   }
   out
}

fn rth_roots_mod_prime(any delta, any r, any p) list {
   "Return all x modulo prime p such that x^r = delta."
   def d = mod(delta, p)
   if d == 0 { return [0] }
   def g = _arith_primitive_root_prime(p)
   if g == 0 { return [] }
   def u = gfp_discrete_log_bsgs(g, d, p)
   if u < 0 { return [] }
   def m = p - 1
   def sols = solve_linear_congruence(r, u, m)
   mut out = []
   mut i = 0
   while i < sols.len {
      out = out.append(power_mod(g, sols.get(i), p))
      i += 1
   }
   out
}

fn least_significant_bits(any n, any k) list {
   "Return up to k least significant bits of |n|, MSB first."
   mut x = Z(n)
   if x < Z(0) { x = 0 - x }
   mut kk = int(k)
   if kk <= 0 { return [] }
   if x == Z(0) { return [0] }
   mut bits = []
   while kk > 0 && x > Z(0) {
      bits = bits.append(bigint_to_int(x % Z(2)))
      x = x / Z(2)
      kk -= 1
   }
   mut out = []
   mut i = bits.len - 1
   while i >= 0 {
      out = out.append(bits.get(i))
      i -= 1
   }
   out
}

fn is_blum_prime(any n) bool {
   "Return true when n is prime and n == 3 mod 4."
   def z = Z(n)
   z >= Z(0) && is_prime(z) && mod(z, Z(4)) == Z(3)
}

fn has_blum_prime(any lbound, any ubound) bool {
   "Return true if the closed interval [lbound, ubound] contains a Blum prime."
   def lo = Z(lbound)
   def hi = Z(ubound)
   if lo <= Z(2) || hi <= Z(2) { panic("has_blum_prime: bounds must be > 2") }
   if lo == hi { panic("has_blum_prime: bounds must be distinct") }
   if lo > hi { panic("has_blum_prime: lower bound must be less than upper bound") }
   mut p = next_prime(lo - Z(1))
   while p <= hi {
      if p % Z(4) == Z(3) { return true }
      p = next_prime(p)
   }
   false
}

fn random_blum_prime(any lbound, any ubound, int ntries=100) any {
   "Return a Blum prime in [lbound, ubound]. Uses random attempts then deterministic fallback."
   def lo = Z(lbound)
   def hi = Z(ubound)
   if !has_blum_prime(lo, hi) { panic("random_blum_prime: no Blum primes within interval") }
   mut i = 0
   while i < ntries {
      mut p = next_prime(randint(lo, hi))
      if p <= hi && p % Z(4) == Z(3) { return p }
      i += 1
   }
   mut p = next_prime(lo - Z(1))
   while p <= hi {
      if p % Z(4) == Z(3) { return p }
      p = next_prime(p)
   }
   nil
}
