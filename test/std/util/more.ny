use std.io
use std.util.uuid
use std.collections.dict
use std.collections.set
use std.core.reflect
use std.core.test

print("Testing Util Extras...")

fn test_uuid_properties(){
	print("Checking UUIDv4 format...")
	def u = uuid4()
	assert(str_len(u) == 36, "uuid length")
	; Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
	assert(load8(u, 8) == 45, "hyphen 1")
	assert(load8(u, 13) == 45, "hyphen 2")
	assert(load8(u, 18) == 45, "hyphen 3")
	assert(load8(u, 23) == 45, "hyphen 4")
	assert(load8(u, 14) == 52, "version 4 digit")
	def y = load8(u, 19)
	def ok = (y == 56 || y == 57 || y == 97 || y == 98)
	assert(ok, "variant 10xx digit")
	print("UUID format passed")
}

fn test_uuid_uniqueness(){
	print("Checking UUID uniqueness (50 samples)...")
	def s = set()
	def i = 0
	while(i < 50){
		def u = uuid4()
		if(contains(s, u)){
			print("Collision on: ", u)
			panic("UUID collision! (Probability is tiny, check RNG)")
		}
		add(s, u)
		i = i + 1
	}
	print("UUID uniqueness passed")
}

test_uuid_properties()
test_uuid_uniqueness()

print("✓ Util Extras passed")
