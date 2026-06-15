;; expect: variable declaration: expected str, got int
use std.core

impl PropBox {
   fn value(self box) int {
      7
   }
}

def PropBox box = PropBox({})
def str s = box.value
print(s)
