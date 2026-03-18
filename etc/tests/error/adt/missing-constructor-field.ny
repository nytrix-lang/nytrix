;; expect: missing field 'radius' for ADT variant 'Shape.Circle'
use std.core

enum Shape {
   Circle(int: radius),
   Empty
}

def x = Shape.Circle()
print(x)
