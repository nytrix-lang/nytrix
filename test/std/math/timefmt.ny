use std.io
use std.math.timefmt
use std.core
use std.core.test
use std.strings.str

print("Testing timefmt...")

fn test_format_time(){
	def ts = 1673712000 ; 2023-01-14 16:00:00 UTC
	def formatted = format_time(ts)
	assert(eq(formatted, "2023-01-14 16:00:00"), "format_time")
}

test_format_time()

print("✓ std.math.timefmt tests passed")
