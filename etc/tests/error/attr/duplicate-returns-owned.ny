;; expect: duplicate attribute '@returns_owned'
use std.core

@returns_owned
@returns_owned
fn f() { [1] }
print(f())
