;; expect: duplicate attribute '@returns_borrow(...)'
use std.core

@returns_borrow(x)
@returns_borrow(x)
fn f(x) { x }
print(f(1))
