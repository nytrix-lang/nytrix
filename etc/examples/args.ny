#!/bin/ny
;; Args Example
use std.cli *
use std.io *

print(f"Argc: {argc()}")
for i in range(argc()) {
   print(f"Arg {i}: {argv(i)}")
}
