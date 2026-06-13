;; expect: unknown field 'diameter' in ADT match pattern 'Shape.Circle'
use std.core

enum Shape {
   Circle(int radius),
   Empty
}

def s = Shape.Circle(1)
match s {
   Shape.Circle(diameter: d) -> d
   Shape.Empty -> 0
}
