use std.core
module test_module ( local_add, local_greet )

fn local_add(x, y) {
	return x + y
}

fn local_greet(name) {
	return "Hello, " + name + " from local module!"
}
