;; flags: --strict-types
;; expect: hm-strict-dynamic-arithmetic
use std.core

fn add_one(any: x){
   x + 1
}

add_one(1)
