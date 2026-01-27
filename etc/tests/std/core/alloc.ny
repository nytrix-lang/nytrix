use std.os.time *
use std.core.alloc *
use std.core *

;; Core Alloc (Test)
;; Tests the bump allocator and basic memory allocation strategies.

def state = bump_new(1024)
assert(is_list(state), "bump state is a list")
def p1 = bump_alloc(state, 10)
assert(p1 != 0, "first bump alloc ok")
store8(p1, 100)
mut p2 = bump_alloc(state, 20)
assert(p2 == p1 + 10, "bump alloc is sequential")
bump_reset(state)
mut p3 = bump_alloc(state, 5)
assert(p3 == p1, "bump reset works")

; Test overflow
def state2 = bump_new(8)
assert(bump_alloc(state2, 10) == 0, "bump overflow returns 0")

print("✓ std.core.alloc tests passed")

