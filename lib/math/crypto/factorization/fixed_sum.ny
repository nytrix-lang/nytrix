;; Keywords: factorization fixed-sum math crypto number-theory
;; Integer-factorization routines for factorization from fixed prime sums.
;;
;; Common weak-key pattern:
;;   p + q = 2^L + offset
;; for some L in a small range.
;;
;; If S = p+q is known, recover p,q by solving:
;;   x^2 - S*x + n = 0
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.fixed_sum(factor_from_sum, factor_from_fixed_sum, factor_from_fixed_sum_scan)
use std.math.nt
use std.math.crypto.factorization.known_phi

fn factor_from_sum(any n, any sum_pq) any {
   "Factor semiprime n given exact sum S = p+q. Returns [p, q] (p<=q) or nil."
   def roots = solve_quadratic_roots(Z(sum_pq), Z(n))
   if(roots == nil){ return nil }
   def p, q = roots.get(0), roots.get(1)
   if(p * q != n){ return nil }
   (p < q) ? [p, q] : [q, p]
}

fn factor_from_fixed_sum(any n, any bits, any offset) any {
   "Try factoring semiprime n given S = 2^bits + offset. Returns [p, q] or nil."
   factor_from_sum(n, (Z(1) << bits) + Z(offset))
}

fn factor_from_fixed_sum_scan(any n, int start_bits, int end_bits, any offset) any {
   "Scan bits in [start_bits, end_bits] for S = 2^bits + offset. Returns [bits, p, q] or nil."
   mut bits = start_bits
   while(bits <= end_bits){
      def pq = factor_from_fixed_sum(n, bits, offset)
      if(pq != nil){ return [bits, pq.get(0), pq.get(1)] }
      bits += 1
   }
   nil
}
