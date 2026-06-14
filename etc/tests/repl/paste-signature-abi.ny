;; repl-expect: REPL_MAIN_OK
#include "etc/tests/rt/ffi/fficonsts.h" as ""

use std.core
use std.math

layout ReplColor {
   u8 r,
   u8 g,
   u8 b,
   u8 a
}

extern "" {
   fn repl_accept_color(ReplColor color)
}

fn repl_default_get() {
   def xs = [10, 20]
   assert(xs.get(0) == 10, "member default arity survives paste")
}

fn repl_lerp(number a, number b, number t) number {
   math.lerp(a, b, t)
}

fn repl_ffi_by_value() {
   def c = ReplColor(1, 2, 3, 4)
   repl_accept_color(c)
   free(c)
}

fn main() {
   repl_default_get()
   assert(repl_lerp(1, 3, 0.5) == 2, "numeric param types survive paste")
   assert(NYTRIX_FFI_CONST_MASK == (42 | 32), "FFI constants survive pasted include")
   print("REPL_MAIN_" + "OK")
}
