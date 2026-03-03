;; Keywords: core error
;; Core Error module.

module std.core.error (
   panic, ok, err, is_ok, is_err, unwrap, unwrap_or,
   format_backtrace
)
use std.core *
use std.core.reflect as core_ref

fn format_backtrace(entries){
   "Returns a formatted string of the backtrace entries (list of [file, line, col, fn])."
   mut out = ""
   mut i = 0
   def n = len(entries)
   while(i < n){
      def f = get(entries, i)
      def file = get(f, 0)
      def line = get(f, 1)
      def col = get(f, 2)
      def f_name = get(f, 3)
      out = f"{out}  at {file}:{line}:{col} (fn {f_name})\n"
      i += 1
   }
   out
}

fn panic(msg){
   "Raises a panic: jumps to the nearest surrounding catch handler  if none, prints the message to stderr and exits."
   eprint("Panic: ", msg)
   return __panic(msg)
}

fn ok(v){
   "Creates an **Ok** result."
   return __result_ok(v)
}

fn err(e){
   "Creates an **Err** result."
   return __result_err(e)
}

fn is_ok(v){
   "Returns **true** if `v` is an **Ok** result."
   return __is_ok(v)
}

fn is_err(v){
   "Returns **true** if `v` is an **Err** result."
   return __is_err(v)
}

fn unwrap(v){
   "Unwraps a Result or returns the value. Panics if **Err**."
   if(is_err(v)){ panic("unwrapped an Err: " + __to_str(__unwrap(v))) }
   return __unwrap(v)
}

fn unwrap_or(v, default){
   "Unwraps a Result or returns the default value."
   if(is_ok(v)){ return __unwrap(v) }
   return default
}

if(comptime{__main()}){
   use std.os.time *
   use std.core.error *
   use std.core *
   use std.str.io *
   use std.core.reflect *

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
   print(f"eq: {(l1 == l2)}")
   assert_eq(l1, l2, "lists equal")

   print("Testing catch...")
   mut caught = false
   try {
       panic("boom")
   } catch e {
       print("Caught error:", e)
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
       print("Caught nested error:", e2)
       if(code == 1 && e2 == "outer"){
          code = 2
       }
   }
   assert_eq(code, 2, "nested catch")

   print("✓ std.core.error tests passed")
}
