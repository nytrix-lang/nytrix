use std.io
use std.util.inspect
use std.core
use std.core.test

print("Testing inspect...")

fn test_inspect(){
	def n = 123
	inspect(n)
	def s = "hello"
	inspect(s)
	def l = [1, 2, 3]
	inspect(l)
	def d = { "a": 1 }
	inspect(d)
	inspect(0) ; none
}

test_inspect()

print("✓ std.util.inspect tests passed")
