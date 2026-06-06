;; expect: conflicting attributes '@pure' and '@effects(...)'
use std.core

@pure
@effects(io)
fn f() int {
   1
}

print(f())
