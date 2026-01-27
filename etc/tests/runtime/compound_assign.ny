use std.io
use std.core.error

;; Compound assignment operator tests

def x = 10
x += 5
assert(x == 15, "x += 5")
x -= 3
assert(x == 12, "x -= 3")
x *= 2
assert(x == 24, "x *= 2")
x /= 4
assert(x == 6, "x /= 4")
x %= 5
assert(x == 1, "x %= 5")

def y = 100
y += 10
y -= 5
y *= 2
y /= 3
y %= 50
assert(y == 20, "compound chain")

print("✓ Compound assignment tests passed")
