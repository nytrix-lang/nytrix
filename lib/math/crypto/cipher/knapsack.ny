;; Keywords: cipher knapsack math crypto
;; Knapsack cipher solving and meet-in-the-middle search routines.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap12.pdf
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.knapsack(knapsack_solve, knapsack_density, knapsack_lo, knapsack_cjloss, knapsack_mitm_table, knapsack_mitm_right_table, knapsack_mitm_solve_prepared, knapsack_mitm_solve_many_prepared, knapsack_mitm_solve)
use std.core
use std.math.nt
use std.math.scalar (ceil, sqrt)
use std.math.crypto.lattice.lll

fn vec_zero(int n) list {
   "Internal: Create a zero vector of length n with bigint elements."
   mut v, i = list(0), 0
   while i < n {
      v = v.append(Z(0))
      i += 1
   }
   v
}

fn _sum_weighted(list a, list bits) bigint {
   mut s, i = Z(0), 0
   while i < bits.len {
      if int(bits.get(i, 0)) != 0 { s = s + Z(a.get(i, 0)) }
      i += 1
   }
   s
}

fn _knapsack_mask_sum(list a, int start, int count, int mask) bigint {
   mut s = Z(0)
   mut i = 0
   while i < count {
      if ((mask >> i) & 1) != 0 { s += Z(a.get(start + i, 0)) }
      i += 1
   }
   s
}

fn _knapsack_bits_from_masks(int n, int split, int left_mask, int right_mask) list {
   mut bits = []
   mut i = 0
   while i < split {
      bits = bits.append((left_mask >> i) & 1)
      i += 1
   }
   i = 0
   while split + i < n {
      bits = bits.append((right_mask >> i) & 1)
      i += 1
   }
   bits
}

fn knapsack_mitm_table(list a, int split=0) dict {
   "Build a meet-in-the-middle subset-sum table for the first split items.
   Sums are stored directly as bigint keys."
   def n = a.len
   if split <= 0 { split = n / 2 }
   mut table = dict(1 << split)
   mut mask = 0
   def limit = 1 << split
   while mask < limit {
      def s = _knapsack_mask_sum(a, 0, split, mask)
      if table.get(s, nil) == nil { table[s] = mask + 1 }
      mask += 1
   }
   table
}

fn knapsack_mitm_right_table(list a, int split=0) list {
   "Build [sum, mask] pairs for the second half of a MITM subset-sum instance."
   def n = a.len
   if split <= 0 { split = n / 2 }
   def right_count = n - split
   def limit = 1 << right_count
   mut pairs = []
   mut mask = 0
   while mask < limit {
      pairs = pairs.append([_knapsack_mask_sum(a, split, right_count, mask), mask])
      mask += 1
   }
   pairs
}

fn knapsack_mitm_solve_prepared(list a, any s, dict table, int split=0) any {
   "Solve a subset-sum instance using a precomputed first-half MITM table.
   Returns a bit vector in the same order as a, or nil."
   def n = a.len
   if n <= 0 { return nil }
   if split <= 0 { split = n / 2 }
   def right_count = n - split
   mut right_mask = 0
   def limit = 1 << right_count
   def target = Z(s)
   while right_mask < limit {
      def rs = _knapsack_mask_sum(a, split, right_count, right_mask)
      def need = target - rs
      def packed = table.get(need, nil)
      if packed != nil {
         return _knapsack_bits_from_masks(n, split, int(packed) - 1, right_mask)
      }
      right_mask += 1
   }
   nil
}

fn knapsack_mitm_solve_many_prepared(list a, list targets, dict left_table, any right_pairs=nil, int split=0) list {
   "Solve many subset-sum targets with one precomputed left table and right-pair table.
   Returns a list of bit vectors or nil entries in target order."
   def n = a.len
   if n <= 0 { return [] }
   if split <= 0 { split = n / 2 }
   def pairs = right_pairs == nil ? knapsack_mitm_right_table(a, split) : right_pairs
   mut out = []
   mut ti = 0
   while ti < targets.len {
      def target = Z(targets[ti])
      mut found = nil
      mut ri = 0
      while found == nil && ri < pairs.len {
         def pair = pairs[ri]
         def left_packed = left_table.get(target - pair[0], nil)
         if left_packed != nil {
            found = _knapsack_bits_from_masks(n, split, int(left_packed) - 1, int(pair[1]))
         }
         ri += 1
      }
      out = out.append(found)
      ti += 1
   }
   out
}

fn knapsack_mitm_solve(list a, any s, int split=0) any {
   "Exact meet-in-the-middle subset-sum solver. Practical for about 32-40 items."
   if split <= 0 { split = a.len / 2 }
   knapsack_mitm_solve_prepared(a, s, knapsack_mitm_table(a, split), split)
}

fn _max_abs(list a) bigint {
   mut mx = Z(0)
   mut i = 0
   while i < a.len {
      def v0 = Z(a.get(i, 0))
      def v = v0 < 0 ? -v0 : v0
      if v > mx { mx = v }
      i += 1
   }
   mx
}

fn knapsack_density(list a) f64 {
   "Return subset-sum density n/log2(max(a))."
   def mx = _max_abs(a)
   mx <= 1 ? float(a.len) : float(a.len) / float(bit_length(mx))
}

fn _lll_rows(list basis) list {
   def red = lll(basis)
   red.get(2, [])
}

fn _knapsack_zero_basis(int n) list {
   mut basis = []
   mut i = 0
   while i <= n {
      basis = basis.append(vec_zero(n + 1))
      i += 1
   }
   basis
}

fn _knapsack_decode_lo(list v, int n) any {
   mut bits = []
   mut j = 0
   while j < n {
      def x = Z(v.get(j, 0))
      if x < -1 || x > 0 { return nil }
      bits = bits.append(int(-x))
      j += 1
   }
   bits
}

fn _knapsack_decode_cjloss(list v, int n) any {
   mut bits = []
   mut j = 0
   while j < n {
      def x = Z(v.get(j, 0))
      if x != -1 && x != 1 { return nil }
      bits = bits.append(int((-x + 1) / 2))
      j += 1
   }
   bits
}

fn _knapsack_decoded_solution(
   list rows,
   list a,
   any s,
   fnptr decode,
   bool require_zero_tail,
) any {
   def n = a.len
   mut i = 0
   while i < rows.len {
      def v = rows.get(i, [])
      if is_list(v) && (!require_zero_tail || Z(v.get(n, 0)) == 0) {
         def bits = decode(v, n)
         case bits {
            nil -> {}
            _ if _sum_weighted(a, bits) == Z(s) -> { return bits }
            _ -> {}
         }
      }
      i += 1
   }
   nil
}

fn _knapsack_zero_tail_solution(list rows, list a, any s, fnptr decode) any {
   _knapsack_decoded_solution(rows, a, s, decode, true)
}

fn knapsack_lo(list a, any s, bool try_on_high_density=false) any {
   "Lagarias-Odlyzko low-density subset-sum attack returning a bit vector or nil."
   if a.len == 0 { return nil }
   def dens = knapsack_density(a)
   if dens >= 0.6463 && !try_on_high_density { return nil }
   def n = a.len
   mut basis = _knapsack_zero_basis(n)
   def N = max(1, int(ceil(sqrt(float(n)) / 2.0)))
   mut i = 0
   while i < n {
      def row = basis.get(i, [])
      row[i] = Z(1)
      row[n] = Z(N) * Z(a.get(i, 0))
      basis[i] = row
      i += 1
   }
   def tr = basis.get(n, [])
   tr[n] = Z(N) * Z(s)
   basis[n] = tr
   def rows = _lll_rows(basis)
   _knapsack_zero_tail_solution(rows, a, s, _knapsack_decode_lo)
}

fn knapsack_cjloss(list a, any s, bool try_on_high_density=false) any {
   "Coster-Joux-LaMacchia-Odlyzko-Schnorr-Stern low-density attack returning a bit vector or nil."
   if a.len == 0 { return nil }
   def dens = knapsack_density(a)
   if dens >= 0.9408 && !try_on_high_density { return nil }
   def n = a.len
   mut basis = _knapsack_zero_basis(n)
   def N = max(1, int(ceil(sqrt(float(n)) / 2.0)))
   mut i = 0
   while i <= n {
      mut j = 0
      mut row = basis.get(i, [])
      while j <= n {
         case j {
            _ if j == n && i < n -> { row[j] = Z(2 * N) * Z(a.get(i, 0)) }
            _ if j == n -> { row[j] = Z(2 * N) * Z(s) }
            _ if i == j -> { row[j] = Z(2) }
            _ if i == n -> { row[j] = Z(1) }
            _ -> { row[j] = Z(0) }
         }
         j += 1
      }
      basis[i] = row
      i += 1
   }
   def rows = _lll_rows(basis)
   _knapsack_zero_tail_solution(rows, a, s, _knapsack_decode_cjloss)
}

fn _knapsack_embed_legacy(list a, any s) any {
   def n = a.len
   mut basis = _knapsack_zero_basis(n)
   def factor = isqrt(n) / 2 + 1
   mut i = 0
   while i < n {
      def row = basis.get(i, [])
      row[i] = Z(2)
      row[n] = Z(a.get(i, 0)) * factor * 2
      basis[i] = row
      i += 1
   }
   mut target_row = basis.get(n, [])
   i = 0
   while i < n {
      target_row[i] = Z(1)
      i += 1
   }
   target_row[n] = Z(s) * factor * 2
   basis[n] = target_row
   _knapsack_decoded_solution(_lll_rows(basis), a, s, _knapsack_decode_cjloss, false)
}

fn _knapsack_bruteforce_small(list a, any s) any {
   def n = a.len
   if n <= 0 || n > 28 { return nil }
   def limit = Z(1) << n
   mut mask = Z(0)
   while mask < limit {
      mut bits = []
      mut i = 0
      while i < n {
         bits = bits.append(int((mask >> i) & Z(1)))
         i += 1
      }
      if _sum_weighted(a, bits) == Z(s) { return bits }
      mask = mask + Z(1)
   }
   nil
}

fn knapsack_solve(list a, any s) any {
   "Subset-sum solver using low-density lattice attacks.
   Tries CJLOSS first, then LO."
   if a.len > 0 && a.len <= 40 {
      def mitm = knapsack_mitm_solve(a, s)
      if mitm != nil { return mitm }
   }
   def c = knapsack_cjloss(a, s, true)
   if c != nil { return c }
   def lo = knapsack_lo(a, s, true)
   if lo != nil { return lo }
   def legacy = _knapsack_embed_legacy(a, s)
   if legacy != nil { return legacy }
   _knapsack_bruteforce_small(a, s)
}
