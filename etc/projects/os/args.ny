#!/usr/bin/env ny

;; Keywords: os args cli argv example
;; Print argc/argv values.
use std.core
use std.os.args

print(f"Argc: {argc()}")
def argv = args()
mut i = 0
while(i < len(argv)){
   def arg = argv.get(i, "")
   def show = arg.len > 0 ? arg : "none"
   print(f"Argv[{i}]: {show}")
   i += 1
}
