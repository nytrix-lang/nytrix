;; Keywords: hash birthday math crypto
;; Hash-analysis routines for birthday bounds and collision search.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc1321
;; - https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
;; References:
;; - std.math.crypto.hash
;; - std.math.crypto
module std.math.crypto.hash.birthday(birthday_bound, birthday_collision_prob, find_collision)
use std.core
use std.math.nt

fn birthday_bound(int n) int {
   "Estimate the number of random samples needed from a space of size n to have a 50% chance of collision.
   Uses the approximation sqrt(2 * n * ln(2)) ~ 1.177 * sqrt(n).
   n: size of the output space(number of possible values)
   Returns the estimated number of samples as an integer."
   if(n <= 0){ return 0 }
   def two_ln2 = 138629
   def val = n * two_ln2 / 100000
   if(val <= 0){ return 1 }
   mut x = val
   mut y = (x + 1) / 2
   while(y < x){
      x = y
      y = (x + val / x) / 2
   }
   if(x <= 0){ return 1 }
   x
}

fn birthday_collision_prob(int n, int k) int {
   "Compute the approximate probability of at least one collision.
   Draws k samples uniformly from a space of size n.
   Uses the approximation 1 - exp(-k*(k-1)/(2*n)).
   n: size of the output space
   k: number of samples drawn
   Returns the collision probability as a scaled integer(multiply by 10000 for percentage)."
   if(n <= 0){ return 0 }
   if(k <= 1){ return 0 }
   if(k > n){ return 10000 }
   def kk = k * (k - 1)
   def two_n = 2 * n
   def exponent = kk * 10000 / two_n
   if(exponent > 100000){ return 10000 }
   def x = exponent
   def x2 = x * x / 10000 / 2
   def x3 = x2 * x / 10000 / 3
   def x4 = x3 * x / 10000 / 4
   mut exp_approx = 10000 - x + x2 - x3 + x4
   if(exp_approx < 0){ exp_approx = 0 }
   mut prob = 10000 - exp_approx
   if(prob < 0){ prob = 0 }
   if(prob > 10000){ prob = 10000 }
   prob
}

fn find_collision(fnptr hash_fn, int max_trials) any {
   "Find a hash collision using the birthday attack method.
   hash_fn: a function that takes an input and returns a hash value
   max_trials: maximum number of hash computations before giving up
   Returns [input1, input2, hash_value] if a collision is found, or nil otherwise."
   mut seen_hashes = list(0)
   mut seen_inputs = list(0)
   mut count = 0
   while(count < max_trials){
      def h = hash_fn(count)
      mut idx = 0
      while(idx < seen_hashes.len){
         def existing = seen_hashes.get(idx)
         if(existing == h){
            def prev_input = seen_inputs.get(idx)
            mut result = list(0)
            result = result.append(prev_input)
            result = result.append(count)
            result = result.append(h)
            return result
         }
         idx += 1
      }
      seen_hashes = seen_hashes.append(h)
      seen_inputs = seen_inputs.append(count)
      count += 1
   }
   nil
}
