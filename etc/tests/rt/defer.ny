use std.core
use std.core.str

mut log = ""

fn test_unwind() {
   "Checks defer execution order across panic unwinding."
   log = log + "1"
   defer { log = log + "2" }
   try {
      defer { log = log + "3" }
      log = log + "P"
      panic("Something went wrong")
      log = log + "X"
   } catch err {
      _ = err
   log = log + "C"  }
   log = log + "4"
}

test_unwind()
assert(log == "1P3C42", "Defer unwind order")

fn test_basic_order() {
   "Checks basic LIFO defer order."
   log = ""
   log = log + "S|"
   defer { log = log + "D1|" }
   defer { log = log + "D2|" }
   log = log + "E|"
}

test_basic_order()
assert(log == "S|E|D2|D1|", "Basic defer execution order")

fn test_scoping() {
   "Checks that block-local defers run at block exit."
   log = ""
   {
      defer { log = log + "ID|" }
      log = log + "IS|"
   }
   log = log + "OS|"
}

test_scoping()
assert(log == "IS|ID|OS|", "Defer scoping behavior")

assert(__pop_run_defer() == nil, "__pop_run_defer accepts empty stack")
assert(__push_defer(nil, nil) == nil, "__push_defer accepts null callback")
assert(__run_defers_to(0) == nil, "__run_defers_to drains null callback")
assert(__push_defer(nil, nil) == nil, "__push_defer accepts second null callback")
assert(__pop_run_defer() == nil, "__pop_run_defer drains null callback")

print("✓ runtime defer tests passed")
