# Values

Values cover literals, strings, containers, receiver methods, equality, and
representation.

## Literal classes

| Class | Examples |
| --- | --- |
| Nil and booleans | `nil`, `true`, `false` |
| Integers | `123`, `0xff`, `0o77`, `0b1010`, sized integer literals |
| Floats | `3.14`, sized float literals |
| Strings | `"text"`, `'text'`, triple-quoted strings, formatted strings |
| Lists | `[1, 2, 3]` |
| Dicts | `{"name": "ny"}` |
| Sets | Set values from standard-library helpers. |
| Tuples | `(1, 2, 3)` and `()`. |
| Ranges | Range values used by iteration and `case`. |
| Functions | Named functions and `fn(...) { ... }` values. |
| Native values | Pointers, handles, layouts, extern values. |

## Strings

Strings are byte-length values. Generic string slicing uses UTF-8 code-point
indices. APIs that cross native, file, socket, or binary-parser boundaries
document whether they expect text or raw bytes.

Formatted strings use expression interpolation:

```ny
f"name={name} count={n}"
```

A trailing top-level `=` keeps the expression text as a label:

```ny
f"{name=}"
f"{count + 1=}"
```

## Lists

Lists are ordered mutable sequences. `list(n)` creates an empty list with
reserved capacity `n`. It does not create `n` initialized elements; the list
starts with zero elements. Use `append` or a literal to fill it.

```ny
def xs = [1, 2, 3]
mut out = list(16)
out = out.append(4)
add(out, 5)
```

`append` returns the updated list. Assign the result back to keep the new
value.

`add(xs, value)` mutates lists and sets in place and returns the container.
Code may use it as a statement when the binding already points at the mutable
container. Prefer one style inside a function: receiver `append` with
assignment, or free `add` for in-place mutation.

Indexing a reserved-but-empty slot is not valid. Use `append`, a literal, or a
standard-library helper that fills the list before reading by index.

## Indexing

Lists, tuples, strings, bytes, and ranges support integer indexing:

```ny
xs[0]
xs[-1]
(4, 5, 6)[1]
"abcd"[2]
range(2, 8, 2)[-1]
```

Negative indices count from the end. Out-of-range or non-integer indices panic;
`try`/`catch` can catch that panic. Dict indexing returns the stored value or
the runtime default for a missing key. `get(key, fallback)` names the fallback.

## Dicts

Dicts map keys to values. `value.get(key, fallback)` returns `fallback` when
the key is absent.

```ny
def empty = {}
def cfg = {"port": 8080}
def port = cfg.get("port", 80)
def host = cfg.get("host", "127.0.0.1")
```

`{}` is the empty dict literal in expression context. Non-empty dict literals
use key/value pairs, for example `{"key": value}`.

## Receiver methods

Receiver methods wrap module helpers. The module API owns receiver
availability; values do not gain receiver methods on their own.

When exact behavior matters, check the module page:

```bash
ny doc get std.core.str
ny doc search --symbols append
```

Runtime tests cover these receiver surfaces:

- sequence properties such as `.len`;
- string methods such as `.strip()`, `.upper()`, `.split()`, `.byte_at()`;
- list/dict/set methods such as `.get()`, `.set()`, `.keys()`, `.contains()`;
- iterator methods such as `.map()`, `.filter()`, `.reduce()`, `.chunk()`;
- byte/integer conversion properties such as `.long`, `.bytes`, `.to_bytes`,
  `.unhex`, and `.text` where the owning module exports them.

## Sequence operations

Strings and lists can be repeated with `*`:

```ny
"ha" * 3
[1, 2] * 2
```

Standard sequence helpers include `sort`, `sorted`, `swapped`, `slice`,
`keys`, `values`, and `items`. The exact exported names and receiver aliases
belong to `std.core`, `std.core.str`, and `std.core.iter`.

`sort(xs)` sorts `xs` in place and returns the same sorted list. `sorted(xs)`
returns a sorted copy and leaves `xs` unchanged.

## Equality and representation

Value kind controls equality. Debug text, display text, and serialization have
separate APIs. Use encoder/parser APIs when you need a stable external
representation.

## Related

- [types.md](types.md) for static type expressions.
- [library.md](../learn/library.md) for parser and encoder modules.
- [troubleshooting.md](../learn/troubleshooting.md) for string/byte and list-capacity pitfalls.
