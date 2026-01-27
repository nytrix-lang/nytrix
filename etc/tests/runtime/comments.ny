use std.io
use std.core.error

;; Comments parsing (Test)

; Single line comment at start
def x = 1  ; inline

; Multiple single line comments
; Comment line 1
; Comment line 2
; Comment line 3
def y = 2

fn add(a, b){
   ; inside function
   a + b  ; return comment
}

def result = add(x, y)
assert(result == 3, "fn with comments")

if x == 1 {
   ; inside if
   def z = 3
}

def i = 0
while i < 5 {
   ; inside loop
   i = i + 1
}
assert(i == 5, "loop with comments")

; Empty lines and comments


def a = 10
def b = 20
def c = a + b  ; inline math
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
