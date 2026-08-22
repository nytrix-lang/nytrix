<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":200,"summary":"Primitive and compound values, collections, mutability, ownership, and representation boundaries."} -->
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

## Typed and dynamic representation

The language distinguishes semantic values from their runtime transport
representation. Typed arithmetic, function parameters, layout fields, and
native ABI integer slots use raw signed 64-bit `int` values. Dynamic values
such as `any` and heterogeneous containers may box values so the runtime can
distinguish integers, heap objects, and native pointers.

Programs must not depend on a tag bit, pointer shape, or other box encoding.
Use type predicates and conversion APIs instead. `bigint` is the explicit
arbitrary-precision type for values outside the signed 64-bit range. See
[Numbers](../learn/numbers.md) for the numeric tower and conversion rules.

## Nil

`nil` and integer `0` are distinct values, although both are falsy. Their type
identity is preserved through compile-time evaluation, `any` values, function
values, and dictionaries. APIs that use absence, such as `filter_map`, must
return `nil` rather than integer `0`.

```ny
use std.core
assert(nil != 0, "nil identity")
assert(!nil && !0, "falsy values")
assert(to_str(nil) == "nil", "nil string")
def any boxed = nil
assert(is_nil(boxed) && type(boxed) == "nil", "any identity")
mut d = {"_": 0}
d = d.delete("_")
d = d.set(nil, "missing")
d = d.set(0, "zero")
assert(d.get(nil) == "missing" && d.get(0) == "zero", "dictionary keys")
```

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
starts with zero elements, so `list(4).len` is `0`. Use `append` or a literal
to fill it.

```ny
def reserved = list(4)
assert(reserved.len == 0, "capacity is not length")

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

### Frozen and mutable literals

A bare `{}` is immutable and `set` silently leaves it unchanged. Use `dict()`
for a mutable accumulator, seed a literal with a value, or clone an existing
literal:

```ny
mut out = dict()
out.set("ok", true)
```

Pre-populated literals such as `{"ok": true}` are mutable.

### Lookup, insertion, and identity

Dict keys are compared by value identity: `nil`, `0`, and `"0"` are three
distinct keys. `get(key, fallback)` returns the fallback for a missing key;
indexing a missing key returns the runtime default value.

A dict grows its table when the load threshold is reached; a growing dict
allocates a new table and reinserts existing entries. A reference to the dict
value stays valid across growth - the dict is a single managed object, not a
pointer to its table. Iteration order over `keys`, `values`, and `items` is not
a stable program contract across insertions and resizes.

## List mutability

Lists are value-typed. `list.append` and `list.extend` return new lists. They
do not modify the source. Always reassign:

```ny
mut xs = []
xs = xs.append(1)
xs = xs.extend([2, 3])
```

## Core idioms

| Form | Meaning |
| --- | --- |
| `;` | Comment marker, not a statement terminator. Newlines separate statements. |
| `cond ? a : b` | Ternary selection. |
| `value ?? fallback` | Nil coalescing - selects fallback when left side is `nil`. |
| `value?.member` | Optional chaining - returns `nil` when receiver is `nil`. |
| `dict.get(key, default)` | Safe lookup with fallback for missing keys. |
| `dict.set(key, value)` | Mutates dict in place, returns the same dict. |
| `list.append(item)` | Returns a new list; reassign the result. |
| `list.extend(other)` | Returns a new list; reassign the result. |
| `clone(value)` | Detached mutable copy of a dict or list. |
| `is_dict(v)`, `is_list(v)`, `is_int(v)`,... | Runtime type predicates. |

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

Value kind controls equality. Scalars compare by value: integers, floats,
booleans, strings, nil, and ranges. Lists, tuples, and dicts compare by
element/value equality. `nil` is not equal to `0` and is not equal to the empty
string. Type predicates such as `is_str`, `is_int`, `is_dict`, and `is_nil`
report the runtime kind; use them instead of inspecting a tag or pointer.

Debug text, display text, and serialization have separate APIs. Use
encoder/parser APIs when you need a stable external representation. `to_str`
produces a display string; `repr` produces a debuggable spelling; parsers and
encoders such as `json_encode`/`json_decode` own the stable wire form.

## Related

- [Types](types.md) for static type expressions.
- [Library](../learn/library.md) for parser and encoder modules.
- [Numbers](../learn/numbers.md) for the numeric tower and conversions.
- [Troubleshooting](../learn/troubleshooting.md) for string/byte and list-capacity pitfalls.