;; Keywords: math float
;; Math Float module.

module std.math.float (
   _box, float, int, trunc, is_float, fadd, fsub, fmul, fdiv, flt, fgt, feq, floor, ceil,
   round, abs, nan, inf, is_nan, is_inf
)
use std.core *

fn _box(bits){
   "Internal: box raw float bits into a Nytrix float object."
   def p = malloc(8)
   store64(p - 8, 110)
   store64(p, bits)
   p
}

fn float(x){
   "Create a float from conversion."
   if(is_ptr(x)){
      if(is_float(x)){ return x }
   }
   if(is_int(x)){
      __flt_box_val(__flt_unbox_val(x))
   } else {
      x
   }
}

fn int(x){
   "Convert a float to an integer (truncates)."
   if(is_int(x)){ x }
   elif(!is_float(x)){ 0 }
   else { __flt_to_int(x) }
}

fn trunc(x){
   "Truncate a float to integer."
   if(is_int(x)){ x }
   elif(!is_float(x)){ 0 }
   else { __flt_trunc(x) }
}

fn is_float(x){
   "Check if value is a float."
   if(!is_ptr(x)){ return false }
   mut tag = load64(x, -8)
   tag == 110 || tag == 221
}

fn fadd(a, b){
   "Add two numbers as floats."
   __flt_add(a, b)
}

fn fsub(a, b){
   "Subtract two numbers as floats."
   __flt_sub(a, b)
}

fn fmul(a, b){
   "Multiply two numbers as floats."
   __flt_mul(a, b)
}

fn fdiv(a, b){
   "Divide two numbers as floats."
   __flt_div(a, b)
}

fn flt(a, b){
   "Return true if a < b."
   __flt_lt(a, b)
}

fn fgt(a, b){
   "Return true if a > b."
   __flt_gt(a, b)
}

fn feq(a, b){
   "Return true if a == b."
   __flt_eq(a, b)
}

fn floor(x){
   "Return the largest integer less than or equal to x."
   def i = int(x)
   def f_i = float(i)
   if(fgt(f_i, x)){ ; i > x
      i - 1
   } else {
      i
   }
}

fn ceil(x){
   "Return the smallest integer greater than or equal to x."
   def i = int(x)
   def f_i = float(i)
   if(flt(f_i, x)){ ; i < x
      i + 1
   } else {
      i
   }
}

fn round(x){
   "Round to nearest integer."
   mut half = float(5)
   half = fdiv(half, float(10)) ; 0.5
   if(flt(x, float(0))){
      ceil(fsub(float(x), half))
   } else {
      floor(fadd(float(x), half))
   }
}

fn abs(x){
   "Return absolute value for int or float."
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
   "Box a quiet NaN."
   fdiv(float(0), float(0))
}

fn inf(){
   "Box infinity."
   fdiv(float(1), float(0))
}

fn is_nan(x){
   "Return true if x is a NaN float."
   if(!is_float(x)){ return 0 }
   def bits = load64(x)
   def mask = 0x7ff0000000000000
   def payload = 0x000fffffffffffff
   if((bits & mask) != (mask & mask)){ return 0 }
   return (bits & payload) != 0
}

fn is_inf(x){
   "Return true if x is positive or negative infinity."
   if(!is_float(x)){ return false }
   def bits = load64(x)
   def mask = 0x7fffffffffffffff
   def inf_bits = 0x7ff0000000000000
   return (bits & mask) == (inf_bits & inf_bits)
}

