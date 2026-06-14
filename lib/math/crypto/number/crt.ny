;; Keywords: number-theory crt math crypto
;; Chinese-remainder operations for recombining modular residues.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap14.pdf
;; References:
;; - std.math.crypto.number
;; - std.math.crypto
module std.math.crypto.number.crt(fast_crt)
use std.math.nt

fn _crt_z(any x) any { is_bigint(x) ? x : Z(x) }

fn _crt_out(any a, any b) list { [a, b] }

fn _crt_pair(any x0, any m0, any x1, any m1) any {
   def mm0, mm1 = _crt_z(m0), _crt_z(m1)
   def xr0, xr1 = mod(_crt_z(x0), mm0), mod(_crt_z(x1), mm1)
   def g = gcd(mm0, mm1)
   def diff = xr1 - xr0
   if mod(diff, g) != Z(0) { return nil }
   def m0g, m1g = mm0 / g, mm1 / g
   def inv = inverse_mod(m0g, m1g)
   if inv == Z(0) { return nil }
   def step = mod((diff / g) * inv, m1g)
   def modl = lcm(mm0, mm1)
   _crt_out(mod(xr0 + mm0 * step, modl), modl)
}

fn fast_crt(list xs, list ms, int segment_size=8) any {
   "Combines congruences x = xs[i] mod ms[i] using divide-and-conquer CRT."
   if !is_list(xs) || !is_list(ms) || xs.len != ms.len || xs.len <= 0 { return nil }
   mut xr, mr = clone(xs), clone(ms)
   while xr.len > 1 {
      mut xn, mn = [], []
      mut i = 0
      while i < xr.len {
         def last = min(i + int(segment_size), xr.len)
         mut chunk_xs, chunk_ms = [], []
         mut j = i
         while j < last {
            chunk_xs, chunk_ms = chunk_xs.append(xr.get(j)), chunk_ms.append(mr.get(j))
            j += 1
         }
         mut chunk_x, chunk_m = chunk_xs.get(0), chunk_ms.get(0)
         j = 1
         while j < chunk_xs.len {
            def comb = _crt_pair(chunk_x, chunk_m, chunk_xs.get(j), chunk_ms.get(j))
            if comb == nil { return nil }
            chunk_x, chunk_m = comb.get(0), comb.get(1)
            j += 1
         }
         xn, mn = xn.append(chunk_x), mn.append(chunk_m)
         i = last
      }
      xr, mr = xn, mn
   }
   _crt_out(xr.get(0), mr.get(0))
}
