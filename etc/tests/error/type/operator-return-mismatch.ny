;; expect: operator target 'Wrong.add' returns int but operator declares Wrong
use std.core

impl Wrong {
   fn add(self: a, self: b): int {
      1
   }
   operator + self: self = add
}

print(1)
