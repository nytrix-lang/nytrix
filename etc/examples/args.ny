#!/bin/ny
use std.cli
use std.io

;; Args (Example)
;; Demonstrates command line argument parsing.

print(f"Argc: {argc()}")
for i in range(argc()) {
   print(f"Arg {i}: {argv(i)}")
}
