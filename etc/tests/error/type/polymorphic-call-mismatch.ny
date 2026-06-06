;; expect: function argument: expected str, got int
use std.core

fn id(x) {
   x
}

fn need_str(str s) str {
   s
}

print(need_str(id(1)))
