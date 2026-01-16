;; Keywords: core error
;; Core Error module.

use std.core
use std.core.reflect
module std.core.error (
   panic, assert, asse__eq
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

fn asse__eq(a,b,msg="assert eq failed"){
   "Asserts that two values are equal. If not, panics with the provided message."
   if(!eq(a,b)){ panic(msg) }
   return 0
}