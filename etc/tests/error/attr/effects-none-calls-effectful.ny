;; expect: declared @effects(none) but inferred effects=
use std.core

@effects(none)
fn bad(){
   print("effect")
   1
}

bad()
