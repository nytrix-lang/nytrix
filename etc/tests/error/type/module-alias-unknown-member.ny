;; expect: module 'OnlyPi' has no exported member 'TAU'
use std.core
module OnlyPi(
   PI
){
   use std.math.scalar (PI)
}

use OnlyPi as math_alias

print(math_alias.TAU)
