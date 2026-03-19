;; flags: --strict-types
;; expect: hm-strict-dynamic-member
use std.core

fn read_member(any: x){
   x.missing
}

read_member(1)
