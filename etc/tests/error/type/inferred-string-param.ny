;; expect: function argument: expected str, got int
use std.core

fn suffix(x){
   x + "!"
}

print(suffix(1))
