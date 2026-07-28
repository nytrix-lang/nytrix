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
def Option<int> typed_option = Option.Some(41)
match typed_option {
   Option.Some(v) -> assert_eq(v + 1, 42, "generic ADT match refines payload type")
   Option.None -> assert_eq(0, 1, "unexpected generic ADT empty variant")
}

print("✓ ADT tests passed")

use std.core
use std.core.io

;; Basic Enums
enum Color {
   Red,
   Green,
   Blue
}

enum Status {
   Ok = 0,
   Error = 1,
   Pending = 2
}

print("Testing basic enums...")
def r = Red
def g = Green
def b = Blue
assert(r == 0, "Red should be 0")
assert(g == 1, "Green should be 1")
assert(b == 2, "Blue should be 2")
assert(Color.Red == 0, "Color.Red")
assert(Color.Green == 1, "Color.Green")
assert(Color.Blue == 2, "Color.Blue")
assert(Status.Ok == 0, "Status.Ok")
assert(Status.Error == 1, "Status.Error")
assert(Status.Pending == 2, "Status.Pending")
print("✓ basic enum tests passed")

;; Enum Matching
fn describe_color(c) {
   match c {
      Color.Red -> "it is red"
      Color.Green -> "it is green"
      Color.Blue -> "it is blue"
      _ -> "unknown color"
   }
}

print("Testing enum matching...")
assert(describe_color(Color.Red) == "it is red", "Red match")
assert(describe_color(Color.Green) == "it is green", "Green match")
assert(describe_color(Color.Blue) == "it is blue", "Blue match")
assert(describe_color(5) == "unknown color", "Other match")
print("✓ enum match tests passed")

;; Module Exported Enums
module MyEnums(
   FileMode
)

enum FileMode {
   Read = 0,
   Write = 1,
   Append = 2
}

use MyEnums (FileMode)

print("Testing module exported enums...")
assert(FileMode.Read == 0, "FileMode.Read")
assert(MyEnums.FileMode.Read == 0, "MyEnums.FileMode.Read")
print("✓ module enum tests passed")

;; Cross-Module Enum Usage
module M1(
   CrossColor
)

enum CrossColor {
   CRed,
   CGreen,
   CBlue
}

print("Testing cross-module enums...")
assert(M1.CrossColor.CRed == 0, "M1.CrossColor.CRed")
print("✓ cross-module enum tests passed")
print("✓ all enum tests passed")

use std.core
use std.core.error

print("Testing std.core.match...")
def a = 4
def r1 = match a {
   4 if a > 1 -> "ok"
   4 -> "bad"
   _ -> "no"
}

assert_eq(r1, "ok", "literal match guard")
def x = ok(42)
def r2 = match x {
   ok(v) if v > 40 -> v
   ok(v) -> v + 1
   err(_) -> -1
   _ -> -2
}

assert_eq(r2, 42, "ok(v) guard")
def y = ok(2)
def r3 = match y {
   ok(v) if v > 10 -> 99
   ok(v) -> v
   _ -> 0
}

assert_eq(r3, 2, "guard fallthrough")
print("✓ std.core.match tests passed")

use std.core
use std.core.iter

fn trait_numeric(numeric x) number { x + 1 }

fn trait_sequence(sequence xs) int { count(xs) }

fn trait_indexable(indexable xs) any { get(xs, 0) }

fn trait_iterable(iterable xs) int { len(xs) }

fn trait_allocator(allocator p) allocator { p }
assert(trait_numeric(41) == 42, "numeric static capability")
assert(trait_sequence([1, 2, 3]) == 3, "sequence list capability")
assert(trait_sequence("abc") == 3, "sequence string capability")
assert(trait_indexable([9, 8]) == 9, "indexable capability")
assert(trait_iterable([1, 2]) == 2, "iterable capability")
mut direct_count_xs = [4, 5, 6]
assert(count(direct_count_xs) == 3, "count fast path for known list")
assert(count("fast") == 4, "count fast path for known string")
