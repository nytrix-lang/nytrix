use std.io

; Test comments in various positions

; Single line comment at start
def x = 1  ; Inline comment

; Multiple single line comments
; Comment line 1
; Comment line 2
; Comment line 3
def y = 2

; Comment before function
fn add(a, b) {
	; Comment inside function
	return a + b  ; Return comment
}

def result = add(x, y)
assert(result == 3, "function with comments")

; Comment before if
if x == 1 {
	; Comment inside if
	def z = 3
}

; Comment before loop
i = 0
while i < 5 {
	; Comment in loop
	i = i + 1
}
assert(i == 5, "loop with comments")

; Empty lines and comments

; Multiple empty lines above

def a = 10
def b = 20
def c = a + b  ; Inline math
assert(c == 30, "operations with comments")

; Nested comments in nested structures
if 1 == 1 {
	; Outer comment
	if 2 == 2 {
		; Inner comment
		def nested = 42
	}
}

; Comment at end of block
fn test() {
	def val = 100
	; Last comment in function
}

; Comments with special characters
; @#$%^&*()_+-=[]{}|;':",.<>?/

; Unicode in comments
; 世界 🚀 ñ

; Very long comment line that goes on and on and on and on and on and on and on and on and on

print("✓ Comments test passed")

; Final comment at end of file
