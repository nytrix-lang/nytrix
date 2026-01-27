#!/bin/ny
;; Args (Example)

use std.core *
use std.os.args *

print(f"Argc: {argc()}")
mut i = 0
while(i < argc()){
    print(f"Argv[{i}]: {argv(i)}")
    i += 1
}
