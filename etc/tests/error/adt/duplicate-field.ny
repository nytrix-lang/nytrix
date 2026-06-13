;; expect: duplicate enum payload field
use std.core

enum Bad {
   A(int x, int x),
   B
}

print(1)
