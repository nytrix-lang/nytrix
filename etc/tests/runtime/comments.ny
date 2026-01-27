use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; Comments parsing (Test)

; Single line comment at start
mut x = 1  ; inline

; Multiple single line comments
; Comment line 1
; Comment line 2
; Comment line 3
def y = 2

fn add(a, b){
   ; inside function
   a + b  ; return comment
}

mut result = add(x, y)
assert(result == 3, "fn with comments")

if x == 1 {
   ; inside if
   def z = 3
}

mut i = 0
while i < 5 {
   ; inside loop
   i = i + 1
}
assert(i == 5, "loop with comments")

; Empty lines and comments


def a = 10
def b = 20
mut c = a + b  ; inline math
assert(c == 30, "ops with comments")

if 1 == 1 {
   ; outer
   if 2 == 2 {
      ; inner
      def nested = 42
   }
}

fn test(){
   def val = 100
   ; last comment
}

; @#$%^&*()_+-=[]{}|;':",.<>?/
; 世界 🚀 ñ
; very long comment very long comment very long comment very long comment very long comment

print("✓ comments tests passed")

