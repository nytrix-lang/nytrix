use std.io
use std.strings.bytes
use std.core.test
use std.core
use std.strings.str

fn test_bytes(){
	print("Testing Strings Bytes...")
	def b = bytes_from_str("hello")
	assert(is_bytes(b), "is_bytes")
	assert(is_str(b) == false, "bytes not str")
	assert(bytes_len(b) == 5, "bytes_len")
	assert(bget(b, 0) == 104, "bget 'h'")
	bset(b, 0, 97)
	assert(bget(b, 0) == 97, "bset 'a'")
	assert(beq(b, bytes_from_str("aello")), "cmp bytes")
	def sub = bslice(b, 1, 3)
	assert(beq(sub, bytes_from_str("el")), "bslice")
	def h = hex_encode(bytes_from_str("abc"))
	assert(beq(h, bytes_from_str("616263")), "hex_encode")
	def decoded = hex_decode("616263")
	assert(beq(decoded, bytes_from_str("abc")), "hex_decode")
	print("✓ std.strings.bytes passed")
}

test_bytes()
