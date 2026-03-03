;; Keywords: math float ieee754
;; Math Float module.
;; Reference: https://en.wikipedia.org/wiki/IEEE_754

module std.math.float (
   _box, float, int, trunc, is_float, fadd, fsub, fmul, fdiv, flt, fgt, feq, floor, ceil,
   round, abs, nan, inf, is_nan, is_inf
)
use std.core *
use std.str as txt

fn _box(bits){
   "Internal: box raw float bits into a Nytrix float object."
   def p = malloc(16) + 8
   store64(p, 110, -8) ; Tag for float
   store64(p, bits) ; Payload
   p
}

fn float(x){
   "Converts `x` (integer, string, or box) to a boxed Nytrix float (double-precision)."
   if(is_ptr(x)){
      if(is_float(x)){ return x }
      if(is_str(x)){
         return txt.atof(x)
      }
   }
   if(is_int(x)){
      return __flt_box_val(__flt_unbox_val(x))
   }
   __flt_box_val(x)
}

fn int(x){
   "Converts float `x` to a tagged integer by truncating the fractional part."
   if(is_int(x)){ x }
   elif(!is_float(x)){ 0 }
   else { __flt_to_int(x) }
}

fn trunc(x){
   "Truncates the fractional part of float `x`, returning the result as a tagged integer."
   if(is_int(x)){ x }
   elif(!is_float(x)){ 0 }
   else { __flt_trunc(x) }
}

fn is_float(x){
   "Returns true if `x` is a boxed floating-point object."
   if(!is_ptr(x)){ return false }
   __is_float_obj(x)
}

fn fadd(a, b){
   "Returns the sum of two values `a` and `b` treated as floats."
   __flt_add(a, b)
}

fn fsub(a, b){
   "Returns the difference between `a` and `b` treated as floats."
   __flt_sub(a, b)
}

fn fmul(a, b){
   "Returns the product of `a` and `b` treated as floats."
   __flt_mul(a, b)
}

fn fdiv(a, b){
   "Returns the quotient of `a` and `b` (a/b) treated as floats."
   __flt_div(a, b)
}

fn flt(a, b){
   "Returns true if float `a` is less than float `b`."
   __flt_lt(a, b)
}

fn fgt(a, b){
   "Returns true if float `a` is greater than float `b`."
   __flt_gt(a, b)
}

fn feq(a, b){
   "Returns true if float `a` is equal to float `b`."
   __flt_eq(a, b)
}

fn floor(x){
   "Returns the largest integer less than or equal to float `x`."
   def i = int(x)
   def f_i = float(i)
   if(fgt(f_i, x)){ ; i > x
      i - 1
   } else {
      i
   }
}

fn ceil(x){
   "Returns the smallest integer greater than or equal to float `x`."
   def i = int(x)
   def f_i = float(i)
   if(flt(f_i, x)){ ; i < x
      i + 1
   } else {
      i
   }
}

def PI = _box(0x400921fb54442d18)

def HALF = _box(0x3fe0000000000000) ; 0.5
def NAN_VAL = _box(0x7ff8000000000000)
def INF_VAL = _box(0x7ff0000000000000)

fn round(x){
   "Rounds float `x` to the nearest integer."
   if(flt(x, float(0))){
      ceil(fsub(float(x), HALF))
   } else {
      floor(fadd(float(x), HALF))
   }
}

fn abs(x){
   "Returns the absolute value of `x` (works for both int and float types)."
   if(is_int(x)){
      if(x < 0){ -x }
      else { x }
   }
   elif(flt(x, float(0))){
      fsub(float(0), x)
   }
   else { x }
}

fn nan(){
   "Returns a quiet NaN (Not-a-Number) float."
   NAN_VAL
}

fn inf(){
   "Returns an infinity float."
   INF_VAL
}

fn is_nan(x){
   "Returns true if float `x` is Not-a-Number."
   if(!is_float(x)){ return 0 }
   def bits = load64(x)
   def mask = 0x7ff0000000000000
   def payload = 0x000fffffffffffff
   if((bits & mask) != (mask & mask)){ return 0 }
   return (bits & payload) != 0
}

fn is_inf(x){
   "Returns true if float `x` is positive or negative infinity."
   if(!is_float(x)){ return false }
   def bits = load64(x)
   def mask = 0x7fffffffffffffff
   def inf_bits = 0x7ff0000000000000
   return (bits & mask) == (inf_bits & inf_bits)
}

if(comptime{__main()}){
   use std.math.float *
   use std.core.error *

   assert(feq(fadd(float(1), float(2)), float(3)), "add")
   assert(feq(fsub(float(3), float(2)), float(1)), "sub")
   assert(feq(fmul(float(2), float(3)), float(6)), "mul")
   assert(feq(fdiv(float(6), float(2)), float(3)), "div")

   assert(flt(float(1), float(2)), "lt")
   assert(fgt(float(2), float(1)), "gt")

   assert(floor(float(1)) == 1, "floor int")
   assert(floor(fadd(float(1), float(0))) == 1, "floor 1.0")

   def f3 = float(3)
   def f2 = float(2)
   def f1_5 = fdiv(f3, f2)
   assert(floor(f1_5) == 1, "floor 1.5")
   assert(ceil(f1_5) == 2, "ceil 1.5")

   def f0 = float(0)
   def fn1_5 = fsub(f0, f1_5)
   assert(floor(fn1_5) == -2, "floor -1.5")
   assert(ceil(fn1_5) == -1, "ceil -1.5")

   assert(is_nan(nan()), "is_nan")
   assert(is_inf(inf()), "is_inf")

   print("✓ std.math.float tests passed")
}
