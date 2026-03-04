;; Keywords: math random
;; Math Random for Nytrix

module std.math.random (
   rand, seed, random, uniform, randint, randrange, choice, shuffle, sample
)
use std.core *
use std.math *
use std.core.reflect *
use std.math.float *

fn rand(){
   "Return a random 63-bit positive integer."
   def r = __rand64() & 0x7FFFFFFFFFFFFFFF
   from_int(r) ; Convert raw to tagged int
}

fn seed(n){
   "Sets the random seed for the pseudo-random number generator to `n`."
   return __srand(n)
}

fn random(){
   "Return a random float in [0, 1)."
   def m = 0x1FFFFFFFFFFFFF
   return fdiv(float(rand() & m), float(m + 1))
}

fn uniform(a, b){
   "Return a random float in [a, b]."
   return fadd(float(a), fmul(random(), fsub(float(b), float(a))))
}

fn _rand_range(a, b, inclusive){
   "Internal: common logic for randint and randrange."
   if(a == b){ return a }
   def range = b - a + (inclusive ? 1 : 0)
   if(range == 0){ return a }
   return a + (rand() % range)
}

fn randint(a, b){
   "Return a random integer in [a, b]."
   _rand_range(a, b, true)
}

fn randrange(a, b){
   "Return a random integer in [a, b)."
   _rand_range(a, b, false)
}

fn choice(xs){
   "Return a random element from a non-empty sequence xs."
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
      def j = (rand() % i + 1)
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
   mut indices = list(8)
   mut i = 0
   while(i < n){  indices = append(indices, i) i += 1 }
   shuffle(indices)
   i = 0
   while(i < k){
       res = append(res, get(xs, get(indices, i)))
      i += 1
   }
   return res
}

if(comptime{__main()}){
   use std.math.random *
   use std.core *
   use std.core.error *

   print("Testing random...")

   def r = random()
   assert(is_float(r), "random returns float")
   assert(r >= 0.0, "random >= 0")
   assert(r < 1.0, "random < 1")

   def ri = randint(10, 20)
   assert(ri >= 10, "randint >= 10")
   assert(ri < 21, "randint < 21")

   print("✓ std.math.random tests passed")
}
