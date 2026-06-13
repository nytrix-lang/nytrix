;; expect: unknown field 'diameter' for ADT variant 'Shape.Circle'
use std.core

enum Shape {
   Circle(int radius),
   Empty
}

def x = Shape.Circle(diameter: 2)
print(x)
