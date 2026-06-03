#!/bin/ny

;; Args
use std.core
use std.os.args

print(f"Argc: {argc()}")
def argv = args()
mut i = 0
while(i < argv.len){
   def arg = argv.get(i, "")
   def show = arg ? arg : "none"
   print(f"Argv[{i}]: {show}")
   i += 1
}
