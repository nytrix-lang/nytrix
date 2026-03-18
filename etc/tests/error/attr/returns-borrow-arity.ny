;; expect: @returns_borrow(...) requires exactly one parameter name
use std.core

@returns_borrow(x, y)
fn f(x, y){ x }
print(f(1, 2))
