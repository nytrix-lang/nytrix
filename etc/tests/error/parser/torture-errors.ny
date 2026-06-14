;; expect: missing '}' before end of file
use std.core

fn bad_inference(x) {
   ;; This should challenge the HM-inference if types are contradictory
   if x > 0 {
      return x + 1
   } else {
      return "not a number"
   }
}

fn deeply_nested() {
   ;; Torture the parser with deep nesting
   ((((((((((((((((((((((((((((((((((((((((((((((((((42))))))))))))))))))))))))))))))))))))))))))))))))))
}

fn mismatched_braces() {
   if true {
      {
         {
            print("missing braces")
         }
         ;; Missing one here
      }
   }
   fn main() {
      bad_inference(10)
      deeply_nested()
      mismatched_braces()
   }
