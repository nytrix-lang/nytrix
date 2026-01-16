use std.io
; Test std.core.error - Error handling and assertions
use std.core.error
use std.core

print("Testing std.core.error...")

; Assert success
assert(true, "true assertion")
assert(1 == 1, "equality assertion")
assert(5 > 3, "comparison assertion")

; AssertEqual
asse__eq(42, 42, "integers equal")
asse__eq("hello", "hello", "strings equal")
asse__eq([1, 2, 3], [1, 2, 3], "lists equal")

fn test_catch(){
   def caught = false
   try {
      panic("boom")
   } catch e {
      caught = true
      if(e != "boom"){ panic("wrong error message") }
   }
   assert(caught, "should have caught panic")
}

fn test_catch_nested(){
   def code = 0
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
   asse__eq(code, 2, "nested catch")
}

test_catch()
test_catch_nested()

print("✓ std.core.error tests passed")
