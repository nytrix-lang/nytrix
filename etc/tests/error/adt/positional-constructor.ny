;; expect: ADT constructor 'Shape.Circle' requires named fields
use std.core

enum Shape {
   Circle(int: radius),
   Empty
}

def x = Shape.Circle(1)
print(x)
