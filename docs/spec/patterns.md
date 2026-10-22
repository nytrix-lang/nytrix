<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":170,"summary":"Pattern matching forms, bindings, ranges, and exhaustiveness behavior."} -->
# Patterns

Patterns describe value dispatch in `case` and `match` forms. A branch can be
selected by a literal value, literal set, range, wildcard, or value shape.

## Case patterns

`case` performs value dispatch:

```ny
use core

fn byte_class(int byte) str {
   case byte {
      9, 10, 13 -> "space"
      32..126 -> "printable"
      _ -> "other"
   }
}

assert_eq(byte_class(9), "space", "control whitespace")
assert_eq(byte_class(65), "printable", "ASCII range")
assert_eq(byte_class(200), "other", "fallback")
```

Supported arm shapes include literals, literal lists, ranges, and wildcard
arms. Common inputs include byte classes, token kinds, command names, and
numeric ranges. Generated or shared classifiers can use
[compile-time tables](comptime.md#tables).

## Match patterns

`match` is for shape-oriented dispatch:

```ny
use core

enum Shape {
   Circle(int radius),
   Rect(int width, int height),
   Empty
}

fn area(any shape) int {
   match shape {
      Shape.Circle(r) -> r * r
      Shape.Rect(w, h) -> w * h
      _ -> 0
   }
}

assert(area(Shape.Circle(5)) == 25, "circle area")
assert(area(Shape.Rect(3, 4)) == 12, "rectangle area")
```

`match` is for branches selected by value structure rather than only literal
equality. Fallback arms are explicit.

ADT patterns use the constructor name and positional payload bindings. Payload
slots bind to an identifier or `_`.

```ny
use core

enum Shape { Circle(int radius), Empty }

fn positive_radius(any shape) int {
   match shape {
      Shape.Circle(r) if r > 0 -> r
      Shape.Circle(_) -> 0
      Shape.Empty -> 0
   }
}

assert(positive_radius(Shape.Circle(4)) == 4, "guard matches")
assert(positive_radius(Shape.Circle(0)) == 0, "guard falls through")
```

When arms use only literals or ranges, `case` is the direct form.

## Wildcard

```ny
_ -> fallback
```

The wildcard arm matches values not handled by earlier arms. Put it last.

## Arm order

Arms are checked in source order. Put specific arms before broad arms:

```ny
use core

fn status_class(int code) str {
   case code {
      404 -> "missing"
      400..499 -> "client-error"
      500..599 -> "server-error"
      _ -> "other"
   }
}

assert_eq(status_class(404), "missing", "specific arm first")
assert_eq(status_class(401), "client-error", "client range")
```

Reversing the first two arms would select `"client-error"` for `404` too.

## Exhaustiveness and binding

A `match` over an ADT is exhaustive when the arms cover every variant; the
compiler rejects a non-exhaustive ADT match. A `case` is exhaustive only with a
wildcard arm; without one, an unmatched value falls through. The compiler
checks duplicate variants, payload arity, unknown constructors, and
non-exhaustive ADT matches.

Arm payload slots bind to an identifier or `_`. A payload binding is scoped to
its arm. Guards evaluate after the pattern matches; a guard that fails falls
through to the next arm in source order.

## Guards and clarity

Use an `if` block when the dispatch depends on several boolean conditions or
requires multi-step setup:

```ny
use core

fn sign_name(int x) str {
   if(x < 0){
      "negative"
   } else {
      case x {
         0 -> "zero"
         _ -> "positive"
      }
   }
}

assert_eq(sign_name(-1), "negative", "negative case")
assert_eq(sign_name(0), "zero", "zero case")
```

## Diagnostics

Pattern bugs include:

| Symptom | Likely cause |
| --- | --- |
| Fallback catches the value | The tested value has a different type or spelling. |
| Specific arm gets skipped | A broader earlier arm captured the value. |
| Parser error near `->` | Missing braces, comma, or malformed range. |
| Multi-step logic in one arm | Use `if` or a helper function inside the branch. |

## Related

- [Control Flow](control-flow.md) for `case`, `match`, loops, and cleanup.
- [Operators](operators.md) for comparisons and boolean operators.
- [Syntax](syntax.md) for source spellings.
