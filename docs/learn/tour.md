<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":90,"summary":"Read one small Ny program using functions, cases, loops, and assertions."} -->
# Tour

This program shows a function, value dispatch, a list, a loop, and executable
checks. Save it as `tour.ny` and run `ny tour.ny`.

```ny
use core

fn label(int value) str {
   case value {
      0 -> "zero"
      1 -> "one"
      _ -> "other"
   }
}

def values = [0, 1, 2]
mut total = 0
for value in values {
   total += value
}

assert_eq(label(1), "one", "case dispatch")
assert_eq(total, 3, "loop accumulation")
print(label(total))
```

The final expression in a function body is its result. `case` selects a value
from ordered arms; `_` handles the remaining values. The loop visits each list
element, and `mut` marks the binding that is reassigned.

Next: [Programs](programs.md) for file and module structure,
[Types](../spec/types.md) for static types, or [Library](library.md) to search
for APIs.
