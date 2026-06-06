;; Keywords: rsa fixed-points math crypto
;; RSA fixed-point analysis routines.
;; The number of fixed points is gcd(e-1, p-1) * gcd(e-1, q-1).
;; Finding all fixed points uses CRT on roots mod p and mod q.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.fixed_points(count_fixed_points, find_fixed_points)
use std.core
use std.math.nt

fn _find_roots_of_xe_eq_x(any e, any p) list {
   "Find all m in [0, p) such that m^e = m mod p.
   Returns a list of all such roots.
   e: public exponent, p: prime modulus."
   mut roots = list(0)
   mut x = 0
   while(x < p){
      def xe = power_mod(x, e, p)
      if(xe == x){ roots = roots.append(x) }
      x += 1
   }
   roots
}

fn count_fixed_points(any e, any p, any q) any {
   "Count the number of fixed points of RSA encryption mod n = p*q.
   The number of fixed points equals(gcd(e-1, p-1) + 1) * (gcd(e-1, q-1) + 1).
   e: public exponent, p: first prime, q: second prime.
   Returns the count of fixed points."
   def g1, g2 = gcd(e - 1, p - 1), gcd(e - 1, q - 1)
   def count = (g1 + 1) * (g2 + 1)
   count
}

fn find_fixed_points(any e, any p, any q) list {
   "Find all fixed points m such that m^e = m mod n where n = p*q.
   Uses brute-force root finding mod p and mod q, then combines via CRT.
   e: public exponent, p: first prime, q: second prime.
   Returns a list of all fixed points in [0, n)."
   def roots_p, roots_q = _find_roots_of_xe_eq_x(e, p), _find_roots_of_xe_eq_x(e, q)
   def n_val = p * q
   def np = roots_p.len
   def nq = roots_q.len
   mut result = list(0)
   def p_inv, q_inv = inverse_mod(p, q), inverse_mod(q, p)
   mut i = 0
   while(i < np){
      def a_p = roots_p.get(i)
      mut j = 0
      while(j < nq){
         def a_q = roots_q.get(j)
         def term1 = a_p * q * q_inv
         def term2 = a_q * p * p_inv
         def m = (term1 + term2) % n_val
         result = result.append(m)
         j += 1
      }
      i += 1
   }
   result
}
