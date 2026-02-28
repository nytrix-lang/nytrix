#!/bin/ny
;; Args (Example)

use std.core *
use std.os.args *

print(f"Argc: {argc()}")

def argv = args()
mut i = 0
while(i < len(argv)){
   def arg = get(argv, i, "")
   def show = arg ? arg : "none"
   print(f"Argv[{i}]: {show}")
   i += 1
}
