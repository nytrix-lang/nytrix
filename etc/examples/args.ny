#!/bin/ny
;; Args (Example)

use std.core *
use std.os.args *

print(f"Argc: {argc()}")
def av = args()
def n = len(av)
mut i = 0
while(i < n){
    def arg = get(av, i, 0)
    def show = arg ? arg : "none"
    print("Argv[" + to_str(i) + "]: " + show)
    i += 1
}
