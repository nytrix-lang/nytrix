;; expect: declared @pure but inferred effects=
use std.core

@pure
fn bad(){
   print("effect")
   1
}

bad()
