#!/bin/ny
use std.core *
use std.os.args *

;; Args (Example)

print(f"Argc: {argc()}")
for i in range(argc()) {
    print(f"Argv[{i}]: {argv(i)}")
}
