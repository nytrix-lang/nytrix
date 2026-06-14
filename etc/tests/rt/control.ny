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
      log = log + "C"
   }
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
def jmp_size = __jmpbuf_size()
def jmp_align = __jmpbuf_align()
assert(jmp_size > 0, "__jmpbuf_size returns a positive integer")
assert(jmp_align > 0, "__jmpbuf_align returns a positive integer")
def env_buf = malloc(jmp_size + jmp_align)
assert(env_buf != 0, "panic env buffer allocates")
assert(__set_panic_env(env_buf) == nil, "__set_panic_env pushes an env frame")
assert(__clear_panic_env() == nil, "__clear_panic_env pops an env frame")
free(env_buf)

;; Clear panic value from test_unwind before starting panic tests
try {
   panic(nil)
} catch _ {
}

assert(__get_panic_val() == nil, "__get_panic_val starts empty")
mut caught = nil
try {
   panic("panic marker")
} catch e {
   caught = e
}

assert(caught == "panic marker", "try/catch receives panic payload")
assert(__get_panic_val() == "panic marker", "__get_panic_val keeps last panic payload")
def bt = __get_backtrace(0)
assert(is_list(bt), "__get_backtrace returns a list")
print("✓ runtime panic tests passed")

use std.core.error
use std.core.iter as it

mut caught_msg = ""

fn id(v) {
   v
}

fn any_id(any v) any {
   v
}

fn capture(thunk) {
   try {
      thunk()
      return nil
   } catch e {
      return e
   }
}

assert(error_kind(err("test")) == ERR, "error_kind on Err result")
assert(is_ok(ok(1)), "ok result is ok")
assert(is_err(err("fail")), "err result is err")
print("✓ runtime result and error tests passed")
