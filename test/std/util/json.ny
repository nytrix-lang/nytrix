use std.io
use std.util.json
use std.core.error
use std.collections.dict
use std.core.reflect

fn test_encode(){
	print("Testing JSON encode...")
	assert(eq(json_encode(123), "123"), "encode int")
	assert(eq(json_encode("hello"), "\"hello\""), "encode string")
	def lst = list()
	lst = append(lst, 1)
	lst = append(lst, "a")
	assert(eq(json_encode(lst), "[1,\"a\"]"), "encode list")
	def d = dict()
	setitem(d, "k", "v")
	assert(eq(json_encode(d), "{\"k\":\"v\"}"), "encode dict")
	print("Encode passed")
}

fn test_decode(){
	print("Testing JSON decode...")
	assert(json_decode("123") == 123, "decode int")
	def d1 = json_decode("\"hello\"")
	assert(eq(d1, "hello"), "decode string")
	def lst = json_decode("[1, \"a\"]")
	assert(list_len(lst) == 2, "decode list len")
	assert(get(lst, 0) == 1, "decode list 0")
	assert(eq(get(lst, 1), "a"), "decode list 1")
	def d = json_decode("{\"k\": \"v\"}")
	assert(getitem(d, "k", 0) != 0, "decode dict get")
	assert(eq(getitem(d, "k", 0), "v"), "decode dict val")
	print("Decode passed")
}

fn test_main(){
	test_encode()
	test_decode()
	print("✓ std.util.json passed")
}

test_main()
