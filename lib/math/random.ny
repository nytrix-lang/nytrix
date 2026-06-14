;; Keywords: random rng pseudorandom math
;; Math Random for Nytrix
;; References:
;; - std.math
module std.math.random(rand, seed, random, uniform, randint, randrange, choice, shuffle, sample, new, next, float, int)
use std.core
use std.math
use std.core.reflect
use std.math.float
use std.math.float as flt
use std.math.simmd as simmd

fn rand() int {
   "Return a random 63-bit positive integer using the global system RNG."
   def r = __rand64() & 0x7FFFFFFFFFFFFFFF
   from_int(r)
}

fn seed(int n) any {
   "Sets the global random seed for the system PRNG to `n`."
   return __srand(n)
}

fn random() any {
   "Return a random float in [0, 1) using the global system RNG."
   def int scale = 0x20000000000000
   def int r = rand()
   def int q = r / scale
   def int rem = r - (q * scale)
   fdiv(flt.float(rem), flt.float(scale))
}

fn uniform(any a, any b) any {
   "Return a random float in [a, b] using the global system RNG."
   fadd(flt.float(a), fmul(random(), fsub(flt.float(b), flt.float(a))))
}

fn _rand_range(int a, int b, bool inclusive) int {
   if a == b { return a }
   def range = b - a + (inclusive ? 1 : 0)
   if range <= 0 { return a }
   def int r = rand()
   def int q = r / range
   def int rem = r - (q * range)
   return a + rem
}

fn randint(int a, int b) int {
   "Return a random integer in [a, b] using the global system RNG."
   _rand_range(a, b, true)
}

fn randrange(int a, int b) int {
   "Return a random integer in [a, b) using the global system RNG."
   _rand_range(a, b, false)
}

fn choice(seq xs) any {
   "Return a random element from a non-empty sequence xs using the global system RNG."
   mut n = xs.len
   if n == 0 { return 0 }
   def int r = rand()
   def int q = r / n
   def int rem = r - (q * n)
   return xs.get(rem)
}

fn shuffle(list xs) list {
   "Shuffles the elements of list `xs` in-place using the Fisher-Yates algorithm."
   def n = xs.len
   if n <= 1 { return xs }
   mut i = n - 1
   while i > 0 {
      def int r = rand()
      def int span = i + 1
      def int q = r / span
      def int j = r - (q * span)
      def tmp = xs.get(i)
      xs.set(i, xs.get(j))
      xs.set(j, tmp)
      i -= 1
   }
   return xs
}

fn sample(seq xs, int k) list {
   "Returns a new list containing `k` unique elements randomly chosen from sequence `xs`."
   def n = xs.len
   if k > n { k = n }
   mut res = list(8)
   mut indices = list(n)
   mut i = 0
   while i < n { indices = indices.append(i) i += 1 }
   shuffle(indices)
   i = 0
   while i < k {
      res = res.append(xs.get(indices.get(i)))
      i += 1
   }
   return res
}

fn _rotl32(int x, int k) int { simmd.rotl32(x, k) }

fn new(int seed_val) list {
   "Creates a new deterministic RNG context(Xoroshiro64). Uses a list for mutable state."
   mut s = (seed_val == 0) ? 0xDEADBEEF : seed_val
   mut s0 = s & 0xFFFFFFFF
   mut s1 = (s >> 32) & 0xFFFFFFFF
   if s1 == 0 { s1 = 0x12345678 }
   [s0, s1]
}

fn next(list ctx) int {
   "Advances state and returns a 32-bit random integer. Modifies ctx list in-place."
   mut s0, s1 = ctx.get(0), ctx.get(1)
   def res = (_rotl32((s0 * 0x9E3779BB) & 0xFFFFFFFF, 5) * 5) & 0xFFFFFFFF
   s1 = s1 ^^ s0
   ctx.set(0, (_rotl32(s0, 26) ^^ s1 ^^ ((s1 << 9) & 0xFFFFFFFF)) & 0xFFFFFFFF)
   ctx.set(1, _rotl32(s1, 13))
   res
}

fn float(list ctx) any {
   "Returns a random float in [0, 1)."
   fdiv(from_int(next(ctx)), 4294967296.0)
}

fn int(list ctx, int a, int b) int {
   "Returns a random integer in [a, b]."
   def range = b - a + 1
   if range <= 0 { return a }
   a + (next(ctx) % range)
}
