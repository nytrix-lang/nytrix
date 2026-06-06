;; expect: @consumes(...) references unknown parameter 'y'
use std.core

@consumes(y)
fn f(x) { x }
print(f(1))
