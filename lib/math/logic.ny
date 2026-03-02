;; Keywords: math logic
;; Math Logic module.

module std.math.logic (
   any, all
)
use std.math *
use std.core *
use std.core.reflect *

fn any(xs){
   "Any true?"
   if(is_list(xs)==0){ return bool(xs)  }
   mut i =0
   while(i<len(xs)){
      if(bool(get(xs,i))==1){ return 1  }
      i=i+1
   }
   return 0
}

fn all(xs){
   "All true?"
   if(is_list(xs)==0){ return bool(xs)  }
   mut i =0
   while(i<len(xs)){
      if(bool(get(xs,i))==0){ return 0  }
      i=i+1
   }
   return 1
}

if(comptime{__main()}){
    use std.math.logic *
    use std.math *
    use std.core.error *

    assert(gcd(12, 8) == 4, "gcd 12 8")
    assert(gcd(17, 19) == 1, "gcd coprime")
    assert(gcd(100, 50) == 50, "gcd factor")

    assert(lcm(4, 6) == 12, "lcm 4 6")
    assert(lcm(3, 5) == 15, "lcm coprime")

    assert(factorial(0) == 1, "0!")
    assert(factorial(1) == 1, "1!")
    assert(factorial(5) == 120, "5!")
    assert(factorial(6) == 720, "6!")

    print("✓ std.math.logic tests passed")
}
