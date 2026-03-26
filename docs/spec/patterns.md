# Patterns

Patterns describe value dispatch in `case` and `match` forms. A branch can be
selected by a literal value, literal set, range, wildcard, or value shape.

## Case patterns

`case` performs value dispatch:

```text
case byte {
   9, 10, 13 -> "space"
   32..126 -> "printable"
   _ -> "other"
}
```

Supported arm shapes include literals, literal lists, ranges, and wildcard
arms.

`case` arms can be compact while staying explicit:

```text
case int(b) {
   9, 10, 13 -> "control-space"
   32..126 -> "ascii"
   _ -> "other"
}
```

Common `case` inputs include byte classifiers, token kinds, command names,
enum-like values, and numeric ranges.

This is a typical shape for byte classifiers:

```ny
fn ascii_score(int: b): int {
   case b {
      9, 10, 13 -> 1
      32..126 -> 10
      _ -> -40
   }
}
```

Generated or shared classifiers can move to `comptime` tables or helper
functions.

## Match patterns

`match` is for shape-oriented dispatch:

```text
match value {
   Shape.Circle(radius: r) -> r * r
   Shape.Rect(width: w, height: h) -> w * h
   _ -> fallback
}
```

`match` is for branches selected by value structure rather than only literal
equality. Fallback arms are explicit.

ADT patterns use the constructor name and named payload fields. Payload fields
must bind to an identifier or `_`.

```text
match shape {
   Shape.Circle(radius: r) if r > 0 -> r
   Shape.Circle(radius: _) -> 0
   Shape.Empty -> 0
}
```

The compiler checks duplicate variants, missing constructor fields, unknown
payload fields, and non-exhaustive ADT matches.

If every arm is a literal or range, `case` is the direct form.

## Wildcard

```text
_ -> fallback
```

The wildcard arm matches values not handled by earlier arms. Put it last.

## Arm order

Arms are checked in source order. Put specific arms before broad arms:

```text
case code {
   404 -> "missing"
   400..499 -> "client-error"
   500..599 -> "server-error"
   _ -> "other"
}
```

Reversing the first two arms would make `404` unreachable as a special case.

Place special cases before a broad range that catches many values.

## Guards and clarity

Use an `if` block when the dispatch depends on several boolean conditions or
requires multi-step setup:

```text
if(x < 0){
   "negative"
} else {
   case x {
      0 -> "zero"
      _ -> "positive"
   }
}
```

## Comptime tables

Repeated classifiers can be represented as compile-time data:

```text
case kind {
   "add", "sub", "mul", "div" -> "arithmetic"
   "and", "or", "xor" -> "bitwise"
   _ -> "other"
}
```

Large or generated tables can use a `comptime` table or generated module
instead of a long hand-written `case`.

There is no fixed arm-count cutoff. Move logic out when arms hide helper calls
or multi-step setup.

## Diagnostics

Pattern bugs include:

| Symptom | Likely cause |
| --- | --- |
| Fallback always runs | The tested value has a different type or spelling. |
| Specific arm never runs | A broader earlier arm captured the value. |
| Parser error near `->` | Missing braces, comma, or malformed range. |
| Multi-step logic in one arm | Use `if` or a helper function inside the branch. |

## Related

- [control-flow.md](control-flow.md) for `case`, `match`, loops, and cleanup.
- [operators.md](operators.md) for comparisons and boolean operators.
- [syntax.md](syntax.md) for source spellings.
