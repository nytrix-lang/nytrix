use std.io
use std.core.alloc
use std.core

fn test_bump(){
	def state = bump_new(1024)
	assert(is_list(state), "bump state is a list")
	def p1 = bump_alloc(state, 10)
	assert(p1 != 0, "first bump alloc ok")
	store8(p1, 100)
	def p2 = bump_alloc(state, 20)
	assert(p2 == p1 + 10, "bump alloc is sequential")
	bump_reset(state)
	def p3 = bump_alloc(state, 5)
	assert(p3 == p1, "bump reset works")
	; Test overflow
	def state2 = bump_new(8)
	assert(bump_alloc(state2, 10) == 0, "bump overflow returns 0")
}

test_bump()
print("✓ std.core.alloc tests passed")
