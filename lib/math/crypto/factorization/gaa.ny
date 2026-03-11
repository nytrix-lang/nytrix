;; Keywords: factorization gaa
;; Integer-factorization routines for GAA-style factorization.
;; Reference:
;; - "A New LSB Attack on Special-Structured RSA Primes"
module std.math.crypto.factorization.gaa(gaa_factor, nearest_sqrt_int)
use std.math.nt

fn nearest_sqrt_int(any: n): any {
   "Return the nearest integer to sqrt(n), rounding to the closer square."
   def s = isqrt(n)
   def lo = n - s * s
   def sp = s + 1
   def hi = sp * sp - n
   hi < lo ? sp : s
}

fn _gaa_roots(any: z, any: sigma, any: rp, any: rq): any {
   def disc = z * z - 4 * sigma * rp * rq
   if(disc < 0 || !is_perfect_square(disc)){ return nil }
   def t = isqrt(disc)
   mut roots = []
   if(((z + t) % 2) == 0){ roots = roots.append((z + t) / 2) }
   if(t != 0 && ((z - t) % 2) == 0){ roots = roots.append((z - t) / 2) }
   if(roots.len > 0){ return roots }
   nil
}

fn gaa_factor(any: n, any: rp, any: rq, int: max_iter): any {
   "Recover [p, q] from n = p*q using the Ghafar-Ariffin-Asbullah attack.
   rp and rq are the structural parameters from the prime construction.
   Returns [p, q] with p <= q, or nil."
   if(n <= 0 || rp <= 0 || rq <= 0){ return nil }
   mut i = isqrt(rp * rq)
   if(i * i < rp * rq){ i += 1 }
   def s = nearest_sqrt_int(n)
   mut it = 0
   while(it < max_iter){
      def delta = s - i
      def sigma = delta * delta
      if(sigma != 0){
         def z = mod(n - rp * rq, sigma)
         def roots = _gaa_roots(z, sigma, rp, rq)
         if(roots != nil){
            mut ri = 0
            while(ri < roots.len){
               def x0 = roots.get(ri)
               if((x0 % rp) == 0){
                  def p = (x0 / rp) + rq
                  if(p > 1 && (n % p) == 0){
                     def q = n / p
                     if(p < q){ return [p, q] }
                     return [q, p]
                  }
               }
               if((x0 % rq) == 0){
                  def p = (x0 / rq) + rp
                  if(p > 1 && (n % p) == 0){
                     def q = n / p
                     if(p < q){ return [p, q] }
                     return [q, p]
                  }
               }
               ri += 1
            }
         }
      }
      i += 1
      it += 1
   }
   nil
}
