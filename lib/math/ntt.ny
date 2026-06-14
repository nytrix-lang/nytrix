;; Keywords: ntt number-theoretic-transform math crypto
;; Number Theoretic Transform (NTT) for fast polynomial multiplication.
;; Reference:
;; - https://en.wikipedia.org/wiki/Number-theoretic_transform
;; - https://cacr.uwaterloo.ca/hac/about/chap14.pdf
;; References:
;; - std.math.crypto
module std.math.crypto.ntt(ntt, intt, ntt_mul, ntt_is_power_of_2, ntt_get_root)
use std.core
use std.math.nt
use std.math.bin as bin

fn ntt_is_power_of_2(int n) bool {
   "Returns true when n is a positive power of two."
   if n <= 0 { return false }
   (n & (n - 1)) == 0
}

fn ntt_get_root(int n, any p) any {
   "Find an n-th root of unity in GF(p). p must be prime and n | (p - 1)."
   def q = Z(p)
   if (q - 1) % n != 0 { return nil }
   def phi = q - 1
   def k = phi / n
   mut g = Z(2)
   while g < q {
      def root = power_mod(g, k, q)
      if power_mod(root, n / 2, q) != 1 { return root }
      g += 1
   }
   nil
}

fn _ntt_mod(any x, any p) any {
   def r = mod(x, p)
   r < 0 ? r + p : r
}

fn _ntt_bit_reverse_copy(list a, int n) list {
   mut res = list(n)
   mut i = 0
   while i < n {
      res = res.append(0)
      i += 1
   }
   i = 0
   def bits = bit_length(n) - 1
   while i < n {
      def rev = bin.bit_reverse(i) >> (32 - bits)
      res[rev] = a.get(i)
      i += 1
   }
   res
}

fn ntt(list a, any p, any g) list {
   "Iterative NTT on list a modulo p with n-th root of unity g.
   n must be a power of 2 and a.len == n."
   def n = a.len
   if !ntt_is_power_of_2(n) { panic("NTT: length must be power of 2") }
   def q = Z(p)
   def root = Z(g)
   mut A, s = _ntt_bit_reverse_copy(a, n), 1
   while (1 << s) <= n {
      def m = 1 << s
      def m2 = m / 2
      def wm = power_mod(root, n / m, q)
      mut k = 0
      while k < n {
         mut w, j = Z(1), 0
         while j < m2 {
            def t, u = _ntt_mod(w * A.get(k + j + m2), q), A.get(k + j)
            A[k + j] = _ntt_mod(u + t, q)
            A[k + j + m2] = _ntt_mod(u - t, q)
            w = _ntt_mod(w * wm, q)
            j += 1
         }
         k += m
      }
      s += 1
   }
   A
}

fn intt(list a, any p, any g) list {
   "Inverse NTT: returns original polynomial coefficients mod p."
   def n, q = a.len, Z(p)
   def gi = inverse_mod(Z(g), q)
   def res = ntt(a, q, gi)
   def ni = inverse_mod(Z(n), q)
   mut i = 0
   while i < n {
      res[i] = _ntt_mod(res.get(i) * ni, q)
      i += 1
   }
   res
}

fn ntt_mul(list a, list b, any p) list {
   "Fast polynomial multiplication mod p using NTT.
   Pads input to next power of 2."
   def n1, n2 = a.len, b.len
   def min_n = n1 + n2 - 1
   mut n = 1
   while n < min_n { n = n << 1 }
   mut a_padded = clone(a)
   while a_padded.len < n { a_padded = a_padded.append(0) }
   mut b_padded = clone(b)
   while b_padded.len < n { b_padded = b_padded.append(0) }
   def q, g = Z(p), ntt_get_root(n, q)
   if g == nil { panic(f"NTT: no {n}-th root of unity modulo {p}") }
   def fa, fb = ntt(a_padded, q, g), ntt(b_padded, q, g)
   mut fc = list(n)
   mut i = 0
   while i < n {
      fc = fc.append(_ntt_mod(fa.get(i) * fb.get(i), q))
      i += 1
   }
   intt(fc, q, g)
}
