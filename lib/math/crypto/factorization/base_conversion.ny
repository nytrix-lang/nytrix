;; Keywords: factorization base-conversion math crypto number-theory
;; Integer-factorization routines that exploit base-conversion structure.
;; This Ny port looks for bases where the digit polynomial of N has a short,
;; Z-factorable representation and evaluates those factors back at the base.
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.base_conversion(base_conversion_factor, base_conversion_factor_pow2)
use std.math.nt

fn _absz(any x) any { bigint_lt(x, Z(0)) ? (-x) : x }

fn _digits_poly(any n, any base) list {
   case Z(n){
      Z(0) -> [Z(0)]
      _ -> {
         mut nn = Z(n)
         mut ds = []
         while nn > Z(0) {
            ds = ds.append(nn % Z(base))
            nn = nn / Z(base)
         }
         ds
      }
   }
}

fn _poly_trim(list p) list {
   mut out = clone(p)
   while out.len > 1 && out.get(out.len - 1) == Z(0) { out = slice(out, 0, out.len - 1) }
   out
}

fn _poly_eval_int(list p, any x) any {
   mut acc = Z(0)
   mut i = p.len - 1
   while i >= 0 {
      acc = acc * Z(x) + p.get(i)
      i -= 1
   }
   acc
}

fn _poly_div_linear(list p, any root) any {
   def pp = _poly_trim(p)
   if pp.len <= 1 { return nil }
   def r = Z(root)
   mut hi = []
   mut carry = pp.get(pp.len - 1)
   hi = hi.append(carry)
   mut i = pp.len - 2
   while i > 0 {
      carry = pp.get(i) + carry * r
      hi = hi.append(carry)
      i -= 1
   }
   def rem = pp.get(0) + carry * r
   if rem != Z(0) { return nil }
   mut q = []
   i = hi.len - 1
   while i >= 0 {
      q = q.append(hi.get(i))
      i -= 1
   }
   _poly_trim(q)
}

fn _divisors_z(any n) list {
   def nn = _absz(n)
   case nn {
      Z(0) -> [Z(0)]
      _ -> {
         mut ds = []
         mut d = Z(1)
         while d * d <= nn {
            case nn % d {
               Z(0) -> {
                  ds = ds.append(d)
                  if d * d != nn { ds = ds.append(nn / d) }
               }
               _ -> {}
            }
            d += Z(1)
         }
         ds
      }
   }
}

fn _root_candidate(list pp, any d) any {
   case _poly_eval_int(pp, d){
      Z(0) -> d
      _ -> {
         case _poly_eval_int(pp, -d){
            Z(0) -> -d
            _ -> nil
         }
      }
   }
}

fn _find_integer_root(list p) any {
   def pp = _poly_trim(p)
   if pp.len <= 1 { return nil }
   def c0 = pp.get(0)
   case c0 {
      Z(0) -> Z(0)
      _ -> {
         def ds = _divisors_z(c0)
         mut i = 0
         while i < ds.len {
            def hit = _root_candidate(pp, ds.get(i))
            case hit {
               nil -> {}
               _ -> { return hit }
            }
            i += 1
         }
         nil
      }
   }
}

fn _factor_integer_poly(list p) any {
   "Repeatedly splits off integer linear factors. Any irreducible residual is
   kept as one final factor."
   mut work = _poly_trim(p)
   mut facs = []
   while work.len > 1 {
      def root = _find_integer_root(work)
      case root {
         nil -> {
            facs = facs.append(work)
            work = [Z(1)]
         }
         _ -> {
            facs = facs.append([(-Z(root)), Z(1)])
            work = _poly_div_linear(work, root)
            case work {
               nil -> { return nil }
               _ -> {}
            }
         }
      }
   }
   facs.len == 0 ? [work] : facs
}

fn _subset_partition(list vals) any {
   def n = vals.len
   mut best = nil
   mut best_gap = Z(0)
   mut mask = 1
   while mask < (1 << n) - 1 {
      mut a, b = Z(1), Z(1)
      mut i = 0
      while i < n {
         if band(mask, 1 << i) { a *= vals.get(i) } else { b *= vals.get(i) }
         i += 1
      }
      if a > Z(1) && b > Z(1) {
         def lo, hi = min(a, b), max(a, b)
         def gap = hi - lo
         if best == nil || gap < best_gap {
            best, best_gap = [lo, hi], gap
         }
      }
      mask += 1
   }
   best
}

fn _factor_at_base(list poly, any base) any {
   def facs = _factor_integer_poly(poly)
   if facs == nil || facs.len < 2 { return nil }
   mut vals = []
   mut i = 0
   while i < facs.len {
      def v = _absz(_poly_eval_int(facs.get(i), base))
      if v <= Z(1) { return nil }
      vals = vals.append(v)
      i += 1
   }
   _subset_partition(vals)
}

fn _base_try(any n, any base, int coefficient_threshold) any {
   def poly = _digits_poly(n, base)
   if poly.len >= coefficient_threshold { return nil }
   def pq = _factor_at_base(poly, base)
   if pq == nil { return nil }
   def p, q = pq.get(0), pq.get(1)
   if p * q == Z(n) { return [min(p, q), max(p, q), Z(base)] }
   nil
}

fn base_conversion_factor(any n, int coefficient_threshold=32, int max_base=256) any {
   "Searches consecutive bases."
   mut base = 2
   while base <= max_base {
      def hit = _base_try(n, base, coefficient_threshold)
      if hit != nil { return hit }
      base += 1
   }
   nil
}

fn base_conversion_factor_pow2(any n, int coefficient_threshold=32, int max_base=(1 << 16)) any {
   "Searches bases of the form 2^k."
   mut base = 2
   while base <= max_base {
      def hit = _base_try(n, base, coefficient_threshold)
      if hit != nil { return hit }
      base *= 2
   }
   nil
}
