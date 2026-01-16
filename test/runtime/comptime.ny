use std.core
use std.core.error

fn test_comptime_basic(){
	print("Testing comptime basics...")
	def v = comptime { return 1 + 2 + 3 }
	assert(v == 6, "comptime basic")
}

fn test_comptime_fallthrough(){
	print("Testing comptime fallthrough...")
	def v = comptime { def x = 10 }
	assert(v == 0, "comptime fallthrough returns 0")
}

fn test_comptime_control_flow(){
	print("Testing comptime control flow...")
	def v = comptime {
		def sum = 0
		def i = 0
		while(i < 5){
			sum = sum + i
			i = i + 1
		}
		if(sum == 10){ return sum }
		return 0
	}
	assert(v == 10, "comptime control flow")
}

fn test_comptime_nested(){
	print("Testing comptime nested...")
	def v = comptime {
		def inner = comptime { return 5 }
		return inner * 2
	}
	assert(v == 10, "comptime nested")
}

fn test_main(){
	test_comptime_basic()
	test_comptime_fallthrough()
	test_comptime_control_flow()
	test_comptime_nested()
	print("✓ Runtime comptime tests passed")
}

test_main()
