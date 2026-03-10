;; Keywords: scalar elementary-math
;; Scalar numeric operations for common arithmetic, clamping, interpolation, and comparisons.
module std.math.scalar(PI, E, LN2, LN10, float, int, floor, ceil, round, abs, min, max, clamp, clamp01, pow, sqrt, log, log2, log10, exp, fmod)
use std.core
use std.math.float (float, int, floor, ceil, round, abs)

def f64: PI   = 3.14159265358979323846
def f64: E    = 2.71828182845904523536
def f64: LN2  = 0.69314718055994530941
def f64: LN10 = 2.30258509299404568402

fn min(number: a, number: b): number { a < b ? a : b }

fn max(number: a, number: b): number { a > b ? a : b }

fn clamp(number: x, number: lo, number: hi): number {
   if(x < lo){ return lo }
   if(x > hi){ return hi }
   x
}

fn clamp01(number: x): f64 {
   def v = float(x)
   if(v < 0.0){ return 0.0 }
   if(v > 1.0){ return 1.0 }
   v
}

fn pow(any: a, any: b): f64 { __flt_pow(float(a), float(b)) }

fn sqrt(any: x): f64 { __flt_sqrt(float(x)) }

fn log(any: x): f64 { __flt_log(float(x)) }

fn log2(any: x): f64 { log(x) / LN2 }

fn log10(any: x): f64 { log(x) / LN10 }

fn exp(any: x): f64 { __flt_exp(float(x)) }

fn fmod(any: a, any: b): f64 { __flt_fmod(float(a), float(b)) }
