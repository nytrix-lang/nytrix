;; Keywords: rsa nitaj-crt
;; RSA Nitaj CRT-RSA attack routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; This targets the practical regime where dp and/or dq are very small and can
;; be recovered by bounded search, then lifted to factors via CRT identities.
module std.math.crypto.rsa.nitaj_crt_rsa(nitaj_crt_rsa_attack)
use std.math.nt
use std.math.scalar (log, sqrt)
use std.math.crypto.rsa.known_crt_exponents
use std.math.crypto.rsa.op (compute_phi, compute_d)

fn _nitaj_try_small_dp(any: n, any: e, any: max_dp): any {
   mut dp = Z(1)
   while(dp <= Z(max_dp)){
      def got = possible_prime_factors_from_crt_exponents(e, e + Z(2), n, dp, nil)
      if(got.len > 0){
         def pq = got.get(0)
         return [pq.get(0), pq.get(1), dp, nil]
      }
      dp += Z(1)
   }
   nil
}

fn _nitaj_try_small_dq(any: n, any: e, any: max_dq): any {
   mut dq = Z(1)
   while(dq <= Z(max_dq)){
      def got = possible_prime_factors_from_crt_exponents(e, e + Z(2), n, nil, dq)
      if(got.len > 0){
         def pq = got.get(0)
         return [pq.get(0), pq.get(1), nil, dq]
      }
      dq += Z(1)
   }
   nil
}

fn nitaj_crt_rsa_attack(any: n, any: e, any: delta=nil, any: max_dp=65536, any: max_dq=65536, bool: check_bounds=false): any {
   "Recover [p, q, dp, dq] when one CRT exponent is unusually small.
   delta is accepted for API parity with the literature ; this implementation
   uses bounded practical search over dp and dq."
   if(check_bounds && delta != nil){
      def n_f = float(n)
      if(n_f > 1.0){
         def alpha = log(float(e)) / log(n_f)
         if(2.0 * delta >= (sqrt(2.0) / 2.0 - alpha)){ return nil }
      }
   }
   def by_dp = _nitaj_try_small_dp(n, e, max_dp)
   if(by_dp != nil){ return by_dp }
   _nitaj_try_small_dq(n, e, max_dq)
}
