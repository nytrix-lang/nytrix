use std.io
use std.core.tuple
use std.core.reflect
use std.core

fn test_tuple_creation(){
	def t = tuple([1, 2, 3])
	assert(is_tuple(t), "is_tuple returns true")
	assert(list_len(t) == 3, "tuple length is 3")
	assert(get(t, 0) == 1, "tuple get(0)")
	assert(get(t, 1) == 2, "tuple get(1)")
	assert(get(t, 2) == 3, "tuple get(2)")
}

fn test_tuple_immutability_convention(){
	; Nytrix doesn't strictly enforce immutability at runtime,
	; but we expect it not to be a list according to is_list
	def t = tuple([10, 20])
	assert(!is_list(t), "tuple is not a list")
}

fn test_tuple_type(){
	def t = tuple([1, 2])
	assert(eq(type(t), "tuple"), "type(t) returns 'tuple'")
}

fn test_tuple_none(){
	def t = tuple(0)
	assert(is_tuple(t), "tuple(none) returns empty tuple")
	assert(list_len(t) == 0, "empty tuple length is 0")
}

test_tuple_creation()
test_tuple_immutability_convention()
test_tuple_type()
test_tuple_none()

print("✓ std.core.tuple tests passed")
