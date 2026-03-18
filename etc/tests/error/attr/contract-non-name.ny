;; expect: expected parameter name in @consumes(...)
use std.core

@consumes(1)
fn f(x){ x }
print(f(1))
