use std.core
use std.io
use std.core.error

;; Parser operator precedence (Test)

def i = 15
assert((i + 1) % 16 == 0, "mod inner parens")
assert(((i + 1) % 16) == 0, "mod outer parens")

def j = 16
assert(j % 8 == 0, "mod no parens")

def x = 10
def y = 5
assert((x + y) * 2 % 10 == 0, "complex precedence")

print("✓ Parser operator precedence tests passed")
