;; Keywords: math mod
;; Math Mod module.

use std.core
use std.core.reflect
use std.math.float
module std.math (
   abs, min, max, pow, mod, clamp, sign, sqrt, gcd, lcm, factorial, lerp
)

fn abs(x){
   "Return the absolute value of `x`."
   if(x < 0){ return 0 - x }
   return x
}

fn min(a,b){
   "Return the smaller of two values `a` and `b`."
   if(a<b){ return a  } return b
}

fn max(a,b){
   "Return the larger of two values `a` and `b`."
   if(a>b){ return a  } return b
}

fn pow(a,b){
   "Return `a` raised to the power of `b` (a^b) using an iterative loop."
   def res = 1  def i = 0
   while(i < b){ res = res * a  i = i + 1 }
   return res
}

fn mod(a,b){
   "Return the remainder of `a` divided by `b`. The result always has the same sign as `b`."
   def res = a - (a / b) * b
   if(res < 0){ res = res + abs(b) }
   return res
}

fn clamp(x, lo, hi){
   "Clamps `x` to the range [lo, hi]."
   if(x < lo){ return lo  }
   if(x > hi){ return hi  }
   return x
}

fn sign(x){
   "Return the sign of `x`: -1 if negative, 0 if zero, 1 if positive."
   if(x == 0){ return 0  }
   if(x < 0){ return -1  }
   return 1
}

fn sqrt(x){
   "Return the square root of `x` using Newton's method (16 iterations)."
   if(x <= 0){ return 0  }
   def r = x
   def i = 0
   while(i < 16){
      r = (r + x / r) / 2
      i = i + 1
   }
   return r
}

fn gcd(a,b){
   "Return the greatest common divisor of `a` and `b`."
   def x = abs(a)  def y = abs(b)
   while(y != 0){
      def t = mod(x, y)
      x = y
      y = t
   }
   return x
}

fn lcm(a,b){
   "Return the least common multiple of `a` and `b`."
   if(a == 0 || b == 0){ return 0  }
   return abs((a / gcd(a,b)) * b)
}

fn factorial(n){
   "Return the factorial of `n`."
   if(n <= 1){ return 1 }
   def res = 1
   def i = 2
   while(i <= n){
      res = res * i
      i = i + 1
   }
   return res
}

fn lerp(a,b,t){
   "Performs linear interpolation between `a` and `b` using factor `t` (usually 0.0 to 1.0)."
   return a + (b - a) * t
}