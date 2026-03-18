;; expect: redefinition of enum member 'A' in enum 'Bad'
use std.core

enum Bad {
   A,
   A
}

print(1)
