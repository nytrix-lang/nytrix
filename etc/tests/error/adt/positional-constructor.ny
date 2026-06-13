;; expect: too many positional fields for ADT variant 'Shape.Circle'
use std.core

enum Shape {
   Circle(int radius),
   Empty
}

def x = Shape.Circle(1, 2)
print(x)
