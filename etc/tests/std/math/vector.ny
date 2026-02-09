use std.math.vector as v
use std.core *

;; std.math.vector (Test)

def a = v.vec3(1, 2, 3)
def b = v.vec3(4, 5, 6)

def s = a + b
assert(get(s, 0, 0) == 5, "vector add x")
assert(get(s, 1, 0) == 7, "vector add y")
assert(get(s, 2, 0) == 9, "vector add z")

assert(v.dot(a, b) == 32, "vector dot")

def c = v.cross3(a, b)
assert(get(c, 0, 0) == -3, "vector cross x")
assert(get(c, 1, 0) == 6, "vector cross y")
assert(get(c, 2, 0) == -3, "vector cross z")

def h = v.hadamard(a, b)
assert(get(h, 0, 0) == 4, "vector hadamard x")
assert(get(h, 1, 0) == 10, "vector hadamard y")
assert(get(h, 2, 0) == 18, "vector hadamard z")

print("✓ std.math.vector tests passed")
