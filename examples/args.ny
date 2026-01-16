#!/bin/ny
use std.io
use std.core
use std.iter

io.print("Argc: " + core.to_str(argc()))
for i in iter.range(argc()) {
	io.print("Arg " + core.to_str(i) + ": " + argv(i))
}
