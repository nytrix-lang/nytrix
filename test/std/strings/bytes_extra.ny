use std.io
use std.strings.bytes
use std.core.test

fn test_bytes_extra(){
	print("Testing std.strings.bytes (extra)...")
	def b = bytes_from_str("abc")
	assert(bytes_len(b) == 3, "bytes_len")
	assert(bget(b, 0) == 97, "bget 0")
	bset(b, 1, 120) ; 'x'
	assert(bget(b, 0) == 97, "bset a")
	assert(bget(b, 1) == 120, "bset x")
	assert(bget(b, 2) == 99, "bset c")
	def s = bslice(b, 1, 3)
	assert(bytes_len(s) == 2, "bslice len")
	assert(bget(s, 0) == 120, "bslice x")
	assert(bget(s, 1) == 99, "bslice c")
	def c = bconcat(bytes_from_str("hi"), bytes_from_str("!"))
	assert(bytes_len(c) == 3, "bconcat len")
	assert(bget(c, 0) == 104, "bconcat h")
	assert(bget(c, 1) == 105, "bconcat i")
	assert(bget(c, 2) == 33, "bconcat !")
	def hx = hex_encode(bytes_from_str("hi"))
	assert(bytes_len(hx) == 4, "hex encode len")
	assert(bget(hx, 0) == 54, "hex encode '6'")
	assert(bget(hx, 1) == 56, "hex encode '8'")
	assert(bget(hx, 2) == 54, "hex encode '6'")
	assert(bget(hx, 3) == 57, "hex encode '9'")
	def back = hex_decode("6869")
	assert(bytes_len(back) == 2, "hex decode len")
	assert(bget(back, 0) == 104, "hex decode h")
	assert(bget(back, 1) == 105, "hex decode i")
	print("✓ std.strings.bytes extra passed")
}

test_bytes_extra()
