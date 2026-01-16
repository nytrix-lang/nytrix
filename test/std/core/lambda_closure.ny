use std.io
use std
use std.core ; for assertions/debug

fn test_basic_capture() {
	print("Testing basic capture...")
	def x = 10
	def f = lambda(y){ return x + y }
	assert(f(20) == 30, "Capture x=10")
	x = 20 ; Reassign local - does capture see it?
	; In Nytrix, captures are currently by value (copy) at creation time?
	; Let's verify this behavior.
	; If it's by copy, f(20) should still be 10+20=30?
	; If it's by reference/upvalue, it should be 40.
	; Based on previous debugging, it seemed to be by copy ("Create local slot (capture-by-copy)").
	assert(f(20) == 30, "Capture is by value (snapshot)")
}

fn test_multiple_captures(){
	print("Testing multiple captures...")
	def a = 1
	def b = 2
	def c = 3
	def f = lambda(){ return a + b + c }
	assert(f() == 6, "Capture a,b,c")
}

fn test_nested_closures(){
	print("Testing nested closures...")
	def x = 10
	def outer = lambda(y){
		def inner = lambda(z){
			return x + y + z
		}
		return inner
	}
	def fn_inner = outer(20)
	assert(fn_inner(30) == 60, "Nested capture x,y,z")
}

fn test_escaping_closure(){
	print("Testing escaping closure...")
	fn make_adder(n){
		return lambda(x){ return x + n }
	}
	def add5 = make_adder(5)
	def add10 = make_adder(10)
	assert(add5(10) == 15, "add5")
	assert(add10(10) == 20, "add10")
}

fn test_mutable_object_capture(){
	print("Testing mutable object capture...")
	; Since capture is by value (pointer copy), capturing a pointer to mutable memory should allow shared state.
	def list_ref = list()
	list_ref = append(list_ref, 1)
	def add_to_list = lambda(v){
		append(list_ref, v)
		return 0
	}
	add_to_list(2)
	assert(list_len(list_ref) == 2, "List length 2")
	assert(get(list_ref, 1) == 2, "List has 2")
}

fn test_capture_shadowing(){
	print("Testing capture shadowing...")
	def x = 10
	def f = lambda(x){ return x * 2 } ; Shadows outer x
	assert(f(5) == 10, "Inner x used")
	assert(x == 10, "Outer x untouched")
}

fn higher_order_map(lst, f){
	def out = list()
	def i = 0
	def n = list_len(lst)
	while(i < n){
		out = append(out, f(get(lst, i)))
		i = i + 1
	}
	return out
}

fn test_map_lambda(){
	print("Testing map with lambda...")
	def l = [1, 2, 3]
	def res = higher_order_map(l, lambda(x){ return x * 10 })
	assert(get(res, 0) == 10, "Map 10")
	assert(get(res, 1) == 20, "Map 20")
	assert(get(res, 2) == 30, "Map 30")
}

fn test_main(){
	test_basic_capture()
	test_multiple_captures()
	test_nested_closures()
	test_escaping_closure()
	test_mutable_object_capture()
	test_capture_shadowing()
	test_map_lambda()
	print("✓ std.core.lambda_closure passed")
}

test_main()
