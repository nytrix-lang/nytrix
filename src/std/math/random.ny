;;; random.ny --- math random module

;; Keywords: math random

;;; Commentary:

;; Math Random module.

use std.core
use std.math
use std.core.reflect
use std.math.float
module std.math.random (
	rand, seed, random, uniform, randint, randrange, choice, shuffle, sample
)

fn rand(){
	"Return a random 63-bit positive integer."
	return rt_rand64() & 0x7FFFFFFFFFFFFFFF
}

fn seed(n){
	"Seed the PRNG."
	return rt_srand(n)
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

fn randint(a, b){
	"Return a random integer in [a, b]."
	if(a == b){ return a }
	return a + mod(rand(), (b - a + 1))
}

fn randrange(a, b){
	"Return a random integer in [a, b)."
	if(a == b){ return a }
	return a + mod(rand(), (b - a))
}

fn choice(xs){
	"Return a random element from a non-empty sequence xs."
	def n = len(xs)
	if(n == 0){ return 0 }
	return get(xs, mod(rand(), n))
}

fn shuffle(xs){
	"Shuffle the list xs in place."
	def n = len(xs)
	if(n <= 1){ return xs }
	def i = n - 1
	while(i > 0){
		def j = mod(rand(), i + 1)
		def tmp = get(xs, i)
		set_idx(xs, i, get(xs, j))
		set_idx(xs, j, tmp)
		i = i - 1
	}
	return xs
}

fn sample(xs, k){
	"Return a k-length list of unique elements chosen from xs."
	def n = len(xs)
	if(k > n){ k = n }
	def res = list(8)
	def indices = list(8)
	def i = 0
	while(i < n){ append(indices, i) i = i + 1 }
	shuffle(indices)
	i = 0
	while(i < k){
		append(res, get(xs, get(indices, i)))
		i = i + 1
	}
	return res
}
