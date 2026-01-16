use std.core.test
use std.core
use std.io

print("Testing case...")

def g = 0
fn set_g(v){
	g = v
	return 0
}

fn case_multi(tag){
	g = 0
	case tag {
		0x4c495354, 0x44494354 -> set_g(1)
		_ -> set_g(2)
	}
	return g
}

assert(case_multi(0x4c495354) == 1, "case multi")
assert(case_multi(0x5455504c) == 2, "case default")

fn case_return(tag){
	case tag {
		0x44494354 -> { print("ciao") print("ciao") return 5 }
		_ -> { return 3 }
	}
	return 0
}

assert(case_return(0x44494354) == 5, "case return")
assert(case_return(0) == 3, "case return default")

fn case_expr(tag){
	def out = 0
	case tag {
		0x44494354 -> { out = 7  5 }
		_ -> { out = 9 }
	}
	return out
}

assert(case_expr(0x44494354) == 7, "case expr block")

fn case_load(tag, ptr){
	def out = 0
	case tag {
		0x4c495354, 0x5345545f, 0x5455504c -> { out = load64(ptr_add(ptr, 8)) }
		_ -> { out = 0 }
	}
	return out
}

def mem = rt_malloc(24)
store64(mem, 0x44494354, 8)
assert(case_load(0x4c495354, mem) == 0x44494354, "case load64")

fn case_wild(tag){
	def out = 0
	case tag {
		_ -> { out = 11 }
	}
	return out
}

assert(case_wild(123) == 11, "case wildcard")

fn case_str(tag){
	case tag {
		0x44494354, 0x44494354 -> { return "ciao" }
		_ -> { return "no" }
	}
}

assert(case_str(0x44494354) == "ciao", "case string")

fn do_something(){
	return 42
}

fn case_load_list(tag, x){
	def out = 0
	case tag {
		0x4c495354, 0x44494354, 0x5345545f, 0x5455504c -> { out = load64(x + 8) }
		_ -> { out = 0 }
	}
	return out
}

fn case_load_space(tag, x){
	def out = 0
	case tag {
		0x4c495354 0x44494354 0x5345545f 0x5455504c -> { out = load64(x + 8) }
		_ -> { out = 0 }
	}
	return out
}

fn case_expr_only(tag){
	def out = 1
	case tag {
		0x44494354 -> 3
		_ -> 5
	}
	return out
}

fn case_call_expr(tag){
	g = 0
	case tag {
		0x44494354 -> set_g(3)
		_ -> set_g(4)
	}
	return g
}

fn case_return_block(tag){
	case tag {
		0x44494354 -> { print("ciao") print("ciao") return 5 }
		_ -> { return 3 }
	}
}

fn case_string_expr(tag){
	case tag {
		0x44494354 -> "ciao"
		_ -> "no"
	}
	return "after"
}

fn case_return_string(tag){
	case tag {
		0x44494354, 0x44494354 -> { return "ciao" }
		_ -> { return "no" }
	}
}

fn case_return_string_space(tag){
	case tag {
		0x44494354 0x44494354 -> { return "ciao" }
		_ -> { return "no" }
	}
}

def mem2 = rt_malloc(24)
store64(mem2, 0x44494354, 8)
assert(case_load_list(0x4c495354, mem2) == 0x44494354, "case quirks load64")
assert(case_load_list(0x11111111, mem2) == 0, "case quirks load64 default")
assert(case_load_space(0x4c495354, mem2) == 0x44494354, "case quirks load64 space")
assert(case_load_space(0x11111111, mem2) == 0, "case quirks load64 space default")
assert(case_expr_only(0x44494354) == 1, "case expr no effect")
assert(case_call_expr(0x44494354) == 3, "case call expr")
assert(case_call_expr(0x0) == 4, "case call expr default")
assert(case_return_block(0x44494354) == 5, "case return block")
assert(case_return_block(0x0) == 3, "case return block default")
assert(case_string_expr(0x44494354) == "after", "case string expr no effect")
assert(case_return_string(0x44494354) == "ciao", "case return string")
assert(case_return_string(0x0) == "no", "case return string default")
assert(case_return_string_space(0x44494354) == "ciao", "case return string space")
assert(case_return_string_space(0x0) == "no", "case return string space default")

fn case_as_expr(tag) {
	def res = case tag {
		"hello" -> 1
		"world" -> 2
		_ -> 3
	}
	return res
}

assert(case_as_expr("hello") == 1, "case as expr 1")
assert(case_as_expr("world") == 2, "case as expr 2")
assert(case_as_expr("anything") == 3, "case as expr 3")

print("\u2713 case passed")

fn test_int_edges(){
	print("Testing integer edge cases...")
	def max_small = 4611686018427387903 ; 2^62 - 1
	def min_small = -4611686018427387904 ; -2^62
	; Test wrapping behavior or precision
	assert(max_small > 0, "max_small positive")
	assert(min_small < 0, "min_small negative")
	; Nytrix ints are 63-bit tagged?
	def a = 1
	def b = 0
	; Division by zero check?
	; def panic = a / b ; This would crash if not handled. We skip for now unless we want to catch it.
	print("✓ Integer edges passed")
}

fn test_string_edges(){
	print("Testing string edge cases...")
	def empty = ""
	assert(len(empty) == 0, "empty len")
	use std.strings.str
	def s = "   "
	assert(len(strip(s)) == 0, "strip whitespace only")
	def s2 = "a"
	assert(len(split(s2, "b")) == 1, "split no delim")
	assert(get(split(s2, "b"), 0) == "a", "split content")
	def s3 = "aba"
	def parts = split(s3, "b")
	assert(len(parts) == 2, "split middle")
	assert(get(parts, 0) == "a", "p1")
	assert(get(parts, 1) == "a", "p2")
	print("✓ String edges passed")
}

fn test_list_edges(){
	print("Testing list edge cases...")
	def l = list(0)
	assert(len(l) == 0, "empty list len")
	; Test growth
	def i = 0
	while(i < 100){
		l = append(l, i)
		i = i + 1
	}
	assert(len(l) == 100, "list growth")
	assert(get(l, 99) == 99, "list get last")
	print("✓ List edges passed")
}

fn test_main(){
	test_int_edges()
	test_string_edges()
	test_list_edges()
	print("✓ Edge cases passed")
}

test_main()
