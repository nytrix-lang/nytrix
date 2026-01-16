use std.io
use std.math
use std.core
use std.core.reflect
use std.math.random

fn run_random_tests(){
	print("Testing random...")
	def r = random()
	if(!is_float(r)){ panic("random returns float") }
	assert(r >= 0.0, "random >= 0")
	assert(r < 1.0, "random < 1")
	fn test_rand_int(){
		def ri = randint(10, 20)
		assert(ri >= 10, "randint >= 10")
		assert(ri < 21, "randint < 21")
	}
	test_rand_int()
}

run_random_tests()

print("✓ std.math.random passed")
