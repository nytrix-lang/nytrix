;; flags: --strict-types
;; expect: hm-strict-dynamic-index
use std.core

fn read_index(any x) {
   x[0]
}

read_index(1)
