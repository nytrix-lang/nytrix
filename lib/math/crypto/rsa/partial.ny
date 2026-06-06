;; Keywords: rsa partial math crypto
;; RSA partial-integer modeling for known and unknown bit ranges routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.partial(recover_p_from_d_partial, coppersmith_factor_known_bits, partial_key_recovery, partial_bit_length)
use std.math.nt

fn partial_bit_length(any n) int {
   "Return the number of bits required to represent integer n."
   mut bl = 0
   mut v = n < 0 ? (0 - n) : n
   while(v != 0){
      bl += 1
      v = v / 2
   }
   bl
}

fn gcd(any a, any b) any {
   "Compute the greatest common divisor of a and b."
   while(b != 0){
      mut t = b
      b, a = a % b, t
   }
   a
}

fn _factor_from_phi(any n, any phi) any {
   def sum_pq = n - phi + 1
   def diff_sq = sum_pq * sum_pq - 4 * n
   if(diff_sq < 0){ return 0 }
   def diff = isqrt(diff_sq)
   if(diff * diff != diff_sq){ return 0 }
   def p, q = (sum_pq - diff) / 2, (sum_pq + diff) / 2
   (p > 1 && p * q == n) ? p : 0
}

fn recover_p_from_d_partial(any e, any d_partial, any bit_mask, any n) any {
   "Attempt to recover prime factor p of n from partial knowledge of the
   private exponent d.  Returns p or 0 on failure."
   def ed_minus_1 = e * d_partial - 1
   if(ed_minus_1 <= 0){ return 0 }
   mut k = 1
   while(k <= e){
      if(ed_minus_1 % k == 0){
         def p = _factor_from_phi(n, ed_minus_1 / k)
         if(p != 0){ return p }
      }
      k += 1
   }
   0
}

fn coppersmith_factor_known_bits(any n, any partial_p, any known_mask, any bits_known, any total_bits) any {
   "Factor n using Coppersmith's method when some bits of a prime factor p are known.
   partial_p holds the known high bits.  Returns the full factor p or 0."
   def shift = total_bits - bits_known
   def p0 = partial_p << shift
   def X = 1 << shift
   mut result = 0
   mut x = 0
   while(x < X){
      def candidate = p0 + x
      if(n % candidate == 0){
         result = candidate
         break
      }
      x += 1
   }
   result
}

fn partial_key_recovery(any e, any n, list known_d_bits, list bit_positions) list {
   "Recover RSA keys [p, q, d] from partial bit knowledge of the private exponent d.
   known_d_bits contains known bit values and bit_positions their positions.
   Returns [p, q, d_reconstructed] or [0, 0, 0] on failure."
   def nbits = known_d_bits.len
   def npos = bit_positions.len
   mut d_reconstructed = 0
   mut i = 0
   while(i < npos){
      def pos = bit_positions.get(i)
      def bit_val = (i < nbits) ? known_d_bits.get(i) : 0
      d_reconstructed = d_reconstructed + (bit_val << pos)
      i += 1
   }
   def p = recover_p_from_d_partial(e, d_reconstructed, 0, n)
   if(p == 0){ return [0, 0, 0] }
   def q = n / p
   (p * q == n) ? [p, q, d_reconstructed] : [0, 0, 0]
}
