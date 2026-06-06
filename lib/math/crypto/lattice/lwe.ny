;; Keywords: lattice lwe math crypto number-theory
;; Lattice routines for Learning With Errors lattice workflows.
;; Reference:
;; - https://cims.nyu.edu/~regev/papers/qcrypto.pdf
;; - https://cims.nyu.edu/~regev/papers/lwesurvey.pdf
;; References:
;; - std.math.crypto.lattice
;; - std.math.crypto
module std.math.crypto.lattice.lwe(lwe_dot_product, lwe_centered_error, lwe_check, lwe_sample, lwe_samples, lwe_matrix_samples, lwe_recover_secret, lwe_brute_force)
use std.math.nt

fn _lwe_next(int state) int { (state * 1103515245 + 12345) & 0x7fffffff }

fn _lwe_rand_mod(int state, int modulus) list {
   def next = _lwe_next(state)
   [next, next % modulus]
}

fn _lwe_rand_error(int state, int error_bound) list {
   def span = error_bound * 2 + 1
   def r = _lwe_rand_mod(state, span)
   [r.get(0), r.get(1) - error_bound]
}

fn lwe_dot_product(list v1, list v2, int modulus) int {
   "Computes a vector dot product modulo `modulus`."
   mut result = 0
   mut i = 0
   while(i < v1.len){
      result = (result + v1.get(i) * v2.get(i)) % modulus
      i += 1
   }
   result
}

fn lwe_centered_error(int b, int dot, int modulus) int {
   "Returns the centered representative of `b - dot mod modulus`."
   mut error = (b - dot) % modulus
   if(error < 0){ error += modulus }
   if(error > modulus / 2){ error -= modulus }
   error
}

fn lwe_check(list secret, list samples, int modulus, int error_bound) bool {
   "Checks that every sample has centered error within `error_bound`."
   mut i = 0
   while(i < samples.len){
      def sample = samples.get(i)
      def error = lwe_centered_error(sample.get(1), lwe_dot_product(secret, sample.get(0), modulus), modulus)
      if(error < -error_bound || error > error_bound){ return false }
      i += 1
   }
   true
}

fn lwe_sample(list secret, int modulus, int error_bound, int seed=1) dict {
   "Builds one deterministic LWE sample `{a,b,e,seed}` from `secret`."
   mut state = seed
   mut a = []
   mut i = 0
   while(i < secret.len){
      def r = _lwe_rand_mod(state, modulus)
      state = r.get(0)
      a = a.append(r.get(1))
      i += 1
   }
   def er = _lwe_rand_error(state, error_bound)
   state = er.get(0)
   def e = er.get(1)
   mut out = dict(4)
   out["a"] = a
   out["b"] = (lwe_dot_product(secret, a, modulus) + e + modulus) % modulus
   out["e"] = e
   out["seed"] = state
   out
}

fn lwe_samples(list secret, int modulus, int count, int error_bound, int seed=1) dict {
   "Builds `count` deterministic LWE samples and returns `{samples, seed}`."
   mut state = seed
   mut samples = []
   mut i = 0
   while(i < count){
      def s = lwe_sample(secret, modulus, error_bound, state)
      samples = samples.append([s.get("a"), s.get("b")])
      state = s.get("seed")
      i += 1
   }
   mut out = dict(2)
   out["samples"] = samples
   out["seed"] = state
   out
}

fn lwe_matrix_samples(list matrix_a, list secret, list errors, int modulus) list {
   "Converts matrix rows, secret vector, and errors into standard `[a,b]` samples."
   mut out = []
   mut i = 0
   while(i < matrix_a.len){
      def a, e = matrix_a.get(i), i < errors.len ? errors.get(i) : 0
      out = out.append([a, (lwe_dot_product(secret, a, modulus) + e + modulus) % modulus])
      i += 1
   }
   out
}

fn lwe_recover_secret(list samples, int modulus, int error_bound) ?list {
   "Recovers a small LWE secret by exhaustive search over `[0, modulus)`."
   if(samples.len == 0){ return nil }
   lwe_brute_force(samples, modulus, samples.get(0).get(0).len, error_bound)
}

fn _lwe_increment_guess(list guess, int modulus) bool {
   mut i = 0
   while(i < guess.len){
      def next = guess.get(i) + 1
      if(next < modulus){
         guess[i] = next
         return true
      }
      guess[i] = 0
      i += 1
   }
   false
}

fn lwe_brute_force(list samples, int modulus, int dim, int error_bound) ?list {
   "Brute-force searches small secrets modulo `modulus`; returns secret or nil."
   if(dim <= 0 || modulus <= 1){ return nil }
   mut guess = []
   mut i = 0
   while(i < dim){
      guess = guess.append(0)
      i += 1
   }
   while(true){
      if(lwe_check(guess, samples, modulus, error_bound)){ return clone(guess) }
      if(!_lwe_increment_guess(guess, modulus)){ break }
   }
   nil
}
