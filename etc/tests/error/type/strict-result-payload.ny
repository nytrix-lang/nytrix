;; flags: --strict-types
;; expect: hm-strict-result-payload
use std.core

fn bad_unwrap(any x) {
   unwrap(x)
}

bad_unwrap(1)
