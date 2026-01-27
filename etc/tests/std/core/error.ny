use std.os.time *
use std.core.error *
use std.core *
use std.str.io *
use std.core.reflect *

;; Core Error (Test)
;; Tests error handling and try-catch mechanisms.

print("Testing std.core.error...")

; Assert success
assert(true, "true assertion")
assert(1 == 1, "equality assertion")
assert(5 > 3, "comparison assertion")

; AssertEqual
assert_eq(42, 42, "integers equal")
assert_eq("hello", "hello", "strings equal")
assert_eq("hello", "hello", "strings equal")
print("Testing list eq:")
def l1 = [1, 2, 3]
def l2 = [1, 2, 3]
print(f"l1: {l1} l2: {l2}")
print(f"eq: {eq(l1, l2)}")
assert_eq(l1, l2, "lists equal")

print("Testing catch...")
mut caught = false
try {
   panic("boom")
} catch e {
   caught = true
   if(e != "boom"){ panic("wrong error message") }
}
assert(caught, "should have caught panic")

print("Testing nested catch...")
mut code = 0
try {
   try {
      panic("inner")
   } catch e {
      code = 1
      panic("outer")
   }
} catch e2 {
   if(code == 1 && e2 == "outer"){
      code = 2
   }
}
assert_eq(code, 2, "nested catch")

print("✓ std.core.error tests passed")
