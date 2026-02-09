use std.core *
use std.str.io *

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
module MyEnums (
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
module M1 (
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
