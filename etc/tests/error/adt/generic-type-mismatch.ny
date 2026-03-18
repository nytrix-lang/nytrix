;; expect: type mismatch: expected 'Option<int>', got 'Option<str>'
use std.core

enum Option<T> {
   Some(T: value),
   None
}

def Option<int>: x = Option.Some(value: "hi")
print(x)
