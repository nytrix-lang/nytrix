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
| Tuples | Tuple values from tuple syntax or helpers. |
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

Lists are ordered mutable sequences. `list(n)` reserves capacity and does not
create `n` initialized elements.

```ny
def xs = [1, 2, 3]
mut out = list(16)
out = out.append(4)
```

`append` returns the updated list. Assign the result back when you want to keep
the new value.

Indexing an uninitialized reserved slot is not defined as a valid list element.

## Indexing

Lists, tuples, strings, bytes, and ranges support integer indexing:

```ny
xs[0]
xs[-1]
"abcd"[2]
range(2, 8, 2)[-1]
```

Negative indices count from the end. Out-of-range or non-integer indices panic
and can be caught with `try`/`catch`. Dict indexing returns the stored value or
the runtime default for a missing key; `get(key, fallback)` names the fallback
explicitly.

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

Receiver methods are convenience forms over module helpers. Receiver
availability is part of the module API, not a universal operation on every
value.

When exact behavior matters, check the module page:

```bash
ny doc get std.core.str
ny doc search --symbols append
```

Runtime-tested receiver surfaces include:

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

## Equality and representation

Equality compares according to value kind. Debug text, display text, and
serialization are separate concerns. Use explicit encoder/parser APIs when a
stable external representation is required.

## Related

- [types.md](types.md) for static type expressions.
- [library.md](../learn/library.md) for parser and encoder modules.
- [troubleshooting.md](../learn/troubleshooting.md) for string/byte and list-capacity pitfalls.
