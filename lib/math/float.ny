;; Keywords: float floating-point math
;; Double-precision Floating Point Mathematics for Nytrix
;; Reference:
;; - https://en.wikipedia.org/wiki/IEEE_754
;; References:
;; - std.math
module std.math.float(_box, float, int, trunc, is_float, fadd, fsub, fmul, fdiv, flt, fgt, feq, floor, ceil, round, abs, nan_val, inf, is_nan, is_inf)
use std.core
use std.core.str (atof)

fn _box(any bits) any {
   "Boxes raw IEEE-754 bits returned by low-level float helpers."
   __flt_box_val(bits)
}

fn float(any x) any {
   "Converts `x` (integer, string, or box) to a boxed Nytrix float(double-precision)."
   if(__is_int(x)){ return __flt_box_val(__flt_from_int(x)) }
   if(__is_float_obj(x)){ return x }
   if(__is_str_obj(x)){ return atof(x) }
   __flt_box_val(0)
}

fn int(any x) int {
   "Converts float `x` to a tagged integer by truncating the fractional part."
   if(is_int(x)){ x }
   elif(!is_float(x)){ 0 }
   else { __flt_to_int(x) }
}

fn trunc(any x) int {
   "Truncates the fractional part of float `x`, returning the result as a tagged integer."
   if(is_int(x)){ x }
   elif(!is_float(x)){ 0 }
   else { __flt_trunc(x) }
}

fn is_float(any x) bool {
   "Returns true if `x` is a boxed floating-point object."
   __is_float_obj(x)
}

fn fadd(any a, any b) any {
   "Returns the sum of two values `a` and `b` treated as floats."
   __flt_add(a, b)
}

fn fsub(any a, any b) any {
   "Returns the difference between `a` and `b` treated as floats."
   __flt_sub(a, b)
}

fn fmul(any a, any b) any {
   "Returns the product of `a` and `b` treated as floats."
   __flt_mul(a, b)
}

fn fdiv(any a, any b) any {
   "Returns the quotient of `a` and `b` (a/b) treated as floats."
   __flt_div(a, b)
}

fn flt(any a, any b) bool {
   "Returns true if float `a` is less than float `b`."
   __flt_lt(a, b)
}

fn fgt(any a, any b) bool {
   "Returns true if float `a` is greater than float `b`."
   __flt_gt(a, b)
}

fn feq(any a, any b) bool {
   "Returns true if float `a` is equal to float `b`."
   __flt_eq(a, b)
}

fn floor(any x) int {
   "Returns the largest integer less than or equal to float `x`."
   def i = int(x)
   def f_i = float(i)
   fgt(f_i, x) ? i - 1 : i
}

fn ceil(any x) int {
   "Returns the smallest integer greater than or equal to float `x`."
   def i = int(x)
   def f_i = float(i)
   flt(f_i, x) ? i + 1 : i
}

def HALF = 0.5
def NAN_VAL = __flt_nan()
def INF_VAL = __flt_inf()

fn round(any x) int {
   "Rounds float `x` to the nearest integer."
   if(flt(x, float(0))){ ceil(fsub(float(x), HALF)) } else { floor(fadd(float(x), HALF)) }
}

fn abs(any x) any {
   "Returns the absolute value of `x` (works for both int and float types)."
   if(is_int(x)){ return x < 0 ? -x : x }
   if(flt(x, float(0))){ return fsub(float(0), x) }
   x
}

fn nan_val() any {
   "Returns a quiet NaN(Not-a-Number) float."
   NAN_VAL
}

fn inf() any {
   "Returns an infinity float."
   INF_VAL
}

fn is_nan(any x) bool {
   "Returns true if float `x` is Not-a-Number."
   if(!is_float(x)){ return false }
   __flt_is_nan(x)
}

fn is_inf(any x) bool {
   "Returns true if float `x` is positive or negative infinity."
   if(!is_float(x)){ return false }
   __flt_is_inf(x)
}

#main {
   assert(is_nan(atof("nan")), "float atof nan")
   assert(is_inf(atof("+Infinity")) && is_inf(atof("-inf")) && atof("-inf") < 0.0, "float atof infinity")
   def half_a = 0.5
   def half_b = atof("0.5")
   mut fd = dict(8)
   fd[half_a] = "half"
   mut fs = set()
   fs = fs.add(half_a)
   assert(fd.get(half_b, "") == "half" && fs.contains(half_b), "float structural keys")
   print("✓ std.math.float self-test passed")
}
