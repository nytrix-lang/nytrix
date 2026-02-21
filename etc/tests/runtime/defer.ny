use std.core *
use std.str *

mut log = ""

fn test_unwind(){
  log = log + "1"
  defer { log = log + "2" }
  try {
     defer { log = log + "3" }
     log = log + "P"
     panic("Something went wrong")
     log = log + "X"
     } catch err {
        print("Caught error:", err)
        log = log + "C"  }
  log = log + "4"
}

test_unwind()
;; Order:
;; 1: Start
;; P: Before panic
;; 3: Inner defer (run on unwinding)
;; C: Catch block
;; 4: After try/catch
;; 2: Outer defer (run on function exit)
assert(log == "1P3C42", "Defer unwind order")

fn test_basic_order(){
  log = ""
  log = log + "S|"
  defer { log = log + "D1|" }
  defer { log = log + "D2|" }
  log = log + "E|"
}

test_basic_order()
assert(log == "S|E|D2|D1|", "Basic defer execution order")

fn test_scoping(){
  log = ""
  {
    defer { log = log + "ID|" }
    log = log + "IS|"
  }
  log = log + "OS|"
}

test_scoping()
assert(log == "IS|ID|OS|", "Defer scoping behavior")

print("✓ runtime defer tests passed")
