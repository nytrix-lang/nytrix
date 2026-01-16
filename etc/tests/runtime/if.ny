use std.core

fn early_return(x) {
   if (x < 0) { return 0 }
   if (x == 10) {
      return 20
   } else {
      return x * 2
   }
}

fn test_if_strict() {
   print("Testing if strict syntax...")
   assert(early_return(-1) == 0, "simple if return")
   assert(early_return(10) == 20, "simple if else return")
   assert(early_return(5) == 10, "fallthrough return")
   if (1) { print("Strict if works") }
   if (0) {
      panic("if(0) taken")
   } else {
      print("if(0) else works")
   }
   def val = 0
   if (1) {
      val = 1
      val = 2
   }
   assert(val == 2, "block exec")
   if (0) {
      val = 10
   }
   val = 20
   assert(val == 20, "if (0) skips block")
   print("✓ if strict syntax passed")
}

test_if_strict()
