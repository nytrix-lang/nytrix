;; expect: operator target 'Bad.add' must take exactly two arguments
use std.core

impl Bad {
   fn add(self: x): self {
      x
   }
   operator + self: self = add
}

print(1)
