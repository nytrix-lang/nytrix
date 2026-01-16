use std.util.uuid
use std.io
use std.strings.str
use std.core
use std.core.test
use std.strings.str

print("Testing uuid...")

fn test_uuid4(){
	def u = uuid4()
	def i = 0
	while(i < 36){
		def c = load8(u, i)
		if(c == 0){
			print("Zero byte found at index: ", itoa(i))
			panic("Zero byte in UUID")
		}
		i = i + 1
	}
	assert(str_len(u) == 36, "length is 36")
	use std.core
	assert(load8(u, 8) == 45, "dash 1")
	assert(load8(u, 13) == 45, "dash 2")
	assert(load8(u, 18) == 45, "dash 3")
	assert(load8(u, 23) == 45, "dash 4")
	assert(load8(u, 14) == 52, "version 4")
	def v = load8(u, 19)
	def ok = (v == 56 || v == 57 || v == 97 || v == 98)
	assert(ok, "variant 10xx")
}

i = 0
while(i < 10){
	test_uuid4()
	i = i + 1
}

print("✓ std.util.uuid tests passed")
