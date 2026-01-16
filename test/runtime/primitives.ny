use std.core
use std.io

fn test_memory() {
	print("Testing rt_malloc/rt_load8/rt_store8...")
	def p = rt_malloc(10)
	assert(p != 0, "malloc success")
	rt_store8(p, 65)
	rt_store8_idx(p, 1, 66)
	assert(rt_load8(p) == 65, "load8 index 0")
	assert(rt_load8_idx(p, 1) == 66, "load8 index 1")
	; Test offset arithmetic
	def offset = 1
	assert(rt_load8_idx(p, offset) == 66, "load8 with variable offset")
	rt_free(p)
	print("✓ Memory primitives passed")
}

fn test_integer_arithmetic() {
	print("Testing integer arithmetic...")
	def a = 10
	def b = 20
	assert(a + b == 30, "add")
	assert(b - a == 10, "sub")
	assert(a * b == 200, "mul")
	assert(b / a == 2, "div")
	assert(b % 3 == 2, "mod")
	print("✓ Arithmetic primitives passed")
}

fn test_recursion() {
	print("Testing recursion...")
	fn fib(n) {
		if (n < 2) { return n }
		return fib(n - 1) + fib(n - 2)
	}
	assert(fib(10) == 55, "fib(10)")
	print("✓ Recursion passed")
}

fn test_closures_basic() {
	print("Testing basic closures...")
	fn adder(x) {
		return lambda(y) { return x + y }
	}
	def add5 = adder(5)
	assert(add5(10) == 15, "closure add5")
	def add10 = adder(10)
	assert(add10(10) == 20, "closure add10")
	print("✓ Closures passed")
}

fn test_nil_undef() {
	print("Testing nil/undef...")
	def x = 42
	undef x
	assert(x == 0, "undef clears binding")
	def y = nil
	assert(y == 0, "nil is none")
	print("✓ nil/undef passed")
}

fn test_main() {
	test_memory()
	test_integer_arithmetic()
	test_recursion()
	test_closures_basic()
	test_nil_undef()
	print("✓ Runtime primitives comprehensive test passed")
}

test_main()
