use std.core

enum Shape {
   Circle(int radius),
   Rect(int width, int height),
   Empty
}

fn area(s) {
   match s {
      Shape.Circle(r) -> r * r
      Shape.Rect(w, h) -> w * h
      Shape.Empty -> 0
   }
}

fn positive_radius(s) {
   match s {
      Shape.Circle(r) if r > 0 -> r
      Shape.Circle(_) -> 0
      Shape.Rect(_, _) -> 0
      Shape.Empty -> 0
   }
}

impl Shape {
   fn method_area(self s) int {
      match s {
         Shape.Circle(r) -> r * r
         Shape.Rect(w, h) -> w * h
         Shape.Empty -> 0
      }
   }
}

def c = Shape.Circle(4)
def c2 = Circle(2)
def r = Shape.Rect(3, 7)
assert_eq(area(c), 16, "Circle payload match")
assert_eq(area(c2), 4, "unqualified ADT constructor")
assert_eq(area(r), 21, "Rect payload match")
assert_eq(area(Shape.Empty), 0, "payload-less mixed enum match")
assert_eq(positive_radius(Shape.Circle(9)), 9, "ADT guard match")
assert_eq(positive_radius(Shape.Circle(-2)), 0, "ADT wildcard payload field")
def xs = [Shape.Circle(5), Shape.Empty]
assert_eq(area(xs[0]), 25, "ADT in list")
assert_eq(area(xs[1]), 0, "payload-less ADT in list")
assert_eq(Shape.Circle(5).method_area(), 25, "payload ADT method")
assert_eq(Shape.Empty.method_area(), 0, "payload-less mixed ADT method")
def empty_shape = Shape.Empty
assert_eq(empty_shape.method_area(), 0, "payload-less ADT method through binding")

enum Option<T> {
   Some(T value),
   None
}

fn unwrap_or_zero(o) {
   match o {
      Option.Some(v) -> v
      Option.None -> 0
   }
}

assert_eq(unwrap_or_zero(Option.Some(12)), 12, "generic ADT payload")
assert_eq(unwrap_or_zero(Option.None), 0, "generic ADT empty variant")
def Option<int>: typed_option = Option.Some(41)
match typed_option {
   Option.Some(v) -> assert_eq(v + 1, 42, "generic ADT match refines payload type")
   Option.None -> assert_eq(0, 1, "unexpected generic ADT empty variant")
}

print("✓ ADT tests passed")
