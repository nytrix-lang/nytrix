;; expect: duplicate field 'x' in layout 'Bad'
use std.core

layout Bad {
   int: x
   int: x
}

print(1)
