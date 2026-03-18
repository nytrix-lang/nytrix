;; expect: non-exhaustive match for enum 'Shape' (missing: Empty)
use std.core

enum Shape {
   Circle(int: radius),
   Empty
}

def s = Shape.Circle(radius: 1)
match s {
   Shape.Circle(radius: r) -> r
}
