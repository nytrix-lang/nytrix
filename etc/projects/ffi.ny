#!/usr/bin/env ny
;; Keywords: native ffi c header sqrt
;; Call C sqrt with a computed f64 value through an imported header.
use core

#include <math.h> as "c"

fn near(f64 a, f64 b) bool {
   def d = a - b
   d > -0.0001 && d < 0.0001
}

def f64 x = 3.0
def f64 y = 4.0
def f64 length = c.sqrt(x * x + y * y)
assert(near(length, 5.0), "C sqrt through direct ABI")
print("C sqrt ABI check passed")
