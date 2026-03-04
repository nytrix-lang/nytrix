;; Keywords: math random
;; Math Random for Nytrix

module std.math.random (
   rand, seed, random, uniform, randint, randrange, choice, shuffle, sample,
   new, next, float, int
)

use std.core *
use std.math *
use std.core.reflect *
use std.math.float *

fn rand(){
   "Return a random 63-bit positive integer using the global system RNG."
   def r = __rand64() & 0x7FFFFFFFFFFFFFFF
   from_int(r)
}

fn seed(n){
   "Sets the global random seed for the system PRNG to `n`."
   return __srand(n)
}

fn random(){
   "Return a random float in [0, 1) using the global system RNG."
   def m = 0x1FFFFFFFFFFFFF
   fdiv(float(rand() & m), float(m + 1))
}

fn uniform(a, b){
   "Return a random float in [a, b] using the global system RNG."
   fadd(float(a), fmul(random(), fsub(float(b), float(a))))
}

fn _rand_range(a, b, inclusive){
   if(a == b){ return a }
   def range = b - a + (inclusive ? 1 : 0)
   if(range <= 0){ return a }
   return a + (rand() % range)
}

fn randint(a, b){
   "Return a random integer in [a, b] using the global system RNG."
   _rand_range(a, b, true)
}

fn randrange(a, b){
   "Return a random integer in [a, b) using the global system RNG."
   _rand_range(a, b, false)
}

fn choice(xs){
   "Return a random element from a non-empty sequence xs using the global system RNG."
   mut n = len(xs)
   if(n == 0){ return 0 }
   return get(xs, (rand() % n))
}

fn shuffle(xs){
   "Shuffles the elements of list `xs` in-place using the Fisher-Yates algorithm."
   def n = len(xs)
   if(n <= 1){ return xs }
   mut i = n - 1
   while(i > 0){
      def j = (rand() % (i + 1))
      def tmp = get(xs, i)
      set_idx(xs, i, get(xs, j))
      set_idx(xs, j, tmp)
      i -= 1
   }
   return xs
}

fn sample(xs, k){
   "Returns a new list containing `k` unique elements randomly chosen from sequence `xs`."
   def n = len(xs)
   if(k > n){ k = n }
   mut res = list(8)
   mut indices = list(n)
   mut i = 0
   while(i < n){ indices = append(indices, i) i += 1 }
   shuffle(indices)
   i = 0
   while(i < k){
      res = append(res, get(xs, get(indices, i)))
      i += 1
   }
   return res
}

fn _rotl32(x, k){
   ((x << k) | (x >> (32 - k))) & 0xFFFFFFFF
}

fn new(seed_val){
   "Creates a new deterministic RNG context (Xoroshiro64). Uses a list for mutable state."
   mut s = (seed_val == 0) ? 0xDEADBEEF : seed_val
   mut s0 = s & 0xFFFFFFFF
   mut s1 = (s >> 32) & 0xFFFFFFFF
   if(s1 == 0){ s1 = 0x12345678 }
   [s0, s1]
}

fn next(ctx){
   "Advances state and returns a 32-bit random integer. Modifies ctx list in-place."
   mut s0 = get(ctx, 0)
   mut s1 = get(ctx, 1)

   def res = (_rotl32((s0 * 0x9E3779BB) & 0xFFFFFFFF, 5) * 5) & 0xFFFFFFFF

   s1 = s1 ^ s0
   set_idx(ctx, 0, (_rotl32(s0, 26) ^ s1 ^ ((s1 << 9) & 0xFFFFFFFF)) & 0xFFFFFFFF)
   set_idx(ctx, 1, _rotl32(s1, 13))

   res
}

fn float(ctx){
   "Returns a random float in [0, 1)."
   fdiv(from_int(next(ctx)), 4294967296.0)
}

fn int(ctx, a, b){
   "Returns a random integer in [a, b]."
   def range = b - a + 1
   if(range <= 0){ return a }
   a + (next(ctx) % range)
}

if(comptime{__main()}){
   use std.math.random *
   use std.core *
   use std.core.error *

   def rng = new(12345)
   def v1 = next(rng)
   def v2 = next(rng)
   assert(v1 != v2, "sequence failed (state not updating?)")

   def rng2 = new(12345)
   assert(next(rng2) == v1, "determinism failed")

   print("✓ std.math.random (list-based state) verified")
}
