use std.math.random *
use std.core.error *

;; std.math.random (Test)
;; Tests random float and randint bounds.

print("Testing random...")

def r = random()
assert(is_float(r), "random returns float")
assert(r >= 0.0, "random >= 0")
assert(r < 1.0, "random < 1")

def ri = randint(10, 20)
assert(ri >= 10, "randint >= 10")
assert(ri < 21, "randint < 21")

print("✓ std.math.random tests passed")
