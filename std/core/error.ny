;; Keywords: core error
;; Core Error module.

use std.core *
use std.core.reflect *
module std.core.error (
   panic, assert, assert_eq
)

fn panic(msg){
   "Raises a panic: jumps to the nearest surrounding catch handler  if none, prints the message to stderr and exits."
   return __panic(msg)
}

fn assert(cond, msg="assert failed"){
   "Asserts that a condition is true. If false, panics with the provided message."
   if(!cond){ panic(msg) }
   return 0
}

fn assert_eq(a,b,msg="assert eq failed"){
   "Asserts that two values are equal. If not, panics with the provided message."
   if(!eq(a,b)){ panic(msg) }
   return 0
}

fn ok(v) { "Creates an Ok result." return __result_ok(v) }
fn err(e) { "Creates an Err result." return __result_err(e) }
fn is_ok(v) { "Checks if value is an Ok result." return __is_ok(v) }
fn is_err(v) { "Checks if value is an Err result." return __is_err(v) }
fn unwrap(v) { "Unwraps a Result or returns the value. Panics if Err."
   if(is_err(v)){ panic("unwrapped an Err: " + __to_str(__unwrap(v))) }
   return __unwrap(v)
}
fn unwrap_or(v, default) { "Unwraps a Result or returns the default value."
   if(is_ok(v)){ return __unwrap(v) }
   return default
}
