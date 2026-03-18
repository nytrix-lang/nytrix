;; expect: ADT match field 'radius' must bind to an identifier or '_'
use std.core

enum Shape {
   Circle(int: radius),
   Empty
}

def s = Shape.Circle(radius: 1)
match s {
   Shape.Circle(radius: 1) -> 1
   Shape.Empty -> 0
}
