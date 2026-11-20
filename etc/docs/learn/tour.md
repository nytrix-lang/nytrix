<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":90,"summary":"Read one small Ny program using functions, cases, loops, and assertions."} -->
# Tour

Prerequisites: a working Nytrix compiler; save this complete program as
`tour.ny` and run `ny tour.ny`.

Each comment labels the source line that follows it.

```ny
; Imports standard helpers used by assertions.
use core

; Declares a function that maps an integer to a string.
fn label(int value) str {
   ; Selects one result from ordered matching arms.
   case value {
      ; Returns "zero" for the exact value zero.
      0 -> "zero"
      ; Returns "one" for the exact value one.
      1 -> "one"
      ; Handles values not matched above.
      _ -> "other"
   }
}

; Creates the input list.
def values = [0, 1, 2]
; Declares a binding that the loop will update.
mut total = 0
; Visits each list element and adds it to total.
for value in values {
   ; Adds the current element to the running total.
   total += value
}

; Checks that the matching case arm is selected.
assert_eq(label(1), "one", "case dispatch")
; Checks the accumulated sum.
assert_eq(total, 3, "loop accumulation")
; Prints "other".
print(label(total))
```

The final expression in a function body is its result. `case` selects the first
matching arm; `_` handles values not matched earlier. The loop visits each list
element.

> **Warning:** `mut` allows reassignment; prefer `def` unless it is required.

Next:

- [Programs](programs.md) — file and module structure.
- [Types](../spec/types.md) — type rules and declarations.
- [Library](library.md) — search standard-library APIs.
