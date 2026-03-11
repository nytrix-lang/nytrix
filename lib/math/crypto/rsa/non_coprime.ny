;; Keywords: rsa non-coprime
;; RSA recovery from non-coprime parameters routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.rsa.non_coprime(non_coprime_attack, non_coprime_exponent_attack)
use std.math.nt
use std.math.crypto.gf
use std.math.crypto.factorization.known_phi

fn _unique_prime_factors(any: n): list {
   mut out = []
   mut nn = n
   mut d = 2
   while(d * d <= nn){
      if(nn % d == 0){
         out = out.append(d)
         while(nn % d == 0){ nn = nn / d }
      }
      d = d + (d == 2 ? 1 : 2)
   }
   if(nn > 1){ out = out.append(nn) }
   out
}

fn _primitive_root_prime(any: p): any {
   if(p == 2){ return 1 }
   def phi = p - 1
   def primes = _unique_prime_factors(phi)
   mut g = 2
   while(g < p){
      mut ok = true
      mut i = 0
      while(i < primes.len){
         def q = primes.get(i)
         if(power_mod(g, phi / q, p) == 1){
            ok = false
            break
         }
         i += 1
      }
      if(ok){ return g }
      g += 1
   }
   0
}

fn _eth_roots_prime(any: c, any: e, any: p): list {
   def cc = mod(c, p)
   if(p == 2){ return [cc] }
   if(cc == 0){ return [0] }
   def phi = p - 1
   def g = _primitive_root_prime(p)
   if(g == 0){ return [] }
   def u = gfp_discrete_log_bsgs(g, cc, p)
   if(u < 0){ return [] }
   def d = gcd(e, phi)
   if(u % d != 0){ return [] }
   def e_red, u_red = e / d, u / d
   def phi_red = phi / d
   def e_inv = inverse_mod(e_red, phi_red)
   if(e_inv == 0){ return [] }
   def t0 = mod(u_red * e_inv, phi_red)
   mut roots = []
   mut i = 0
   while(i < d){
      roots = roots.append(power_mod(g, t0 + i * phi_red, p))
      i += 1
   }
   roots
}

fn non_coprime_attack(any: n, any: e, any: phi, any: c): any {
   "Decrypt ciphertext c when public exponent e is not coprime with phi(n).
   Returns a list of candidate plaintexts or nil."
   def factors = factor_from_phi(n, phi)
   if(factors == nil){ return nil }
   def p, q = factors.get(0), factors.get(1)
   def roots_p, roots_q = _eth_roots_prime(c, e, p), _eth_roots_prime(c, e, q)
   if(roots_p.len == 0){ return nil }
   if(roots_q.len == 0){ return nil }
   mut results = []
   mut i = 0
   while(i < roots_p.len){
      def rp = roots_p.get(i)
      mut j = 0
      while(j < roots_q.len){
         def rq = roots_q.get(j)
         results = results.append(crt([rp, rq], [p, q]))
         j += 1
      }
      i += 1
   }
   results
}

fn non_coprime_exponent_attack(any: n, any: e, any: phi, any: c): any {
   "Entry point for RSA with an exponent not coprime to phi(n)."
   non_coprime_attack(n, e, phi, c)
}
