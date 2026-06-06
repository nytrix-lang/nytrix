;; Keywords: rsa cherkaoui-semmouni math crypto
;; RSA Cherkaoui-Semmouni RSA attack routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; This exposes the close-prime/small-d regime as a first-class RSA attack.
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.cherkaoui_semmouni(cherkaoui_semmouni_attack)
use std.math.nt
use std.math.scalar (log, sqrt, pow)
use std.math.crypto.factorization.fermat
use std.math.crypto.rsa.wiener
use std.math.crypto.rsa.op (compute_phi, compute_d)

fn _cs_private_from_factors(number e, number p, number q) any {
   def d = compute_d(e, compute_phi(p, q))
   if(d == nil || d <= 0){ return nil }
   d
}

fn _cs_bound_ok(number e, number n, any beta, any delta) bool {
   if(beta == nil || delta == nil){ return true }
   def n_f = float(n)
   if(n_f <= 1.0){ return false }
   def alpha = log(float(e)) / log(n_f)
   delta < (2.0 - sqrt(2.0 * alpha * beta))
}

fn _cs_max_iter(number n, any beta) int {
   if(beta == nil){ return 1000000 }
   mut budget = int(2.0 * pow(float(n), beta))
   if(budget < 64){ budget = 64 }
   budget
}

fn cherkaoui_semmouni_attack(number n, number e, any beta=nil, any delta=nil, any max_iter=nil, bool check_bounds=true) any {
   "Recover [p, q, d] when the RSA primes are unusually close.
   Uses the trusted Fermat factorization path and derives d after factoring.
   Returns nil on failure."
   if(check_bounds && !_cs_bound_ok(e, n, beta, delta)){ return nil }
   if(max_iter == nil){ max_iter = _cs_max_iter(n, beta) }
   def pq = fermat_attack(n, max_iter)
   if(pq == nil){
      def wd = wiener_attack(n, e)
      if(wd == nil){ return nil }
      return [wd[1], wd[2], wd[0]]
   }
   def p, q = pq[0], pq[1]
   def d = _cs_private_from_factors(e, p, q)
   if(d == nil){ return nil }
   [p, q, d]
}
