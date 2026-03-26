# Syntax

Syntax records source spellings. Semantic behavior belongs to the topic pages
linked from [language.md](language.md).

## Lexical forms

```text
comment        := ";" text-until-newline
identifier     := [A-Za-z_][A-Za-z0-9_]*
module-path    := identifier ("." identifier)*
block          := "{" source* "}"
```

There are no block comments. Semicolons are comments, not statement
terminators.

## Literals

```text
nil
true
false
123
0xff
0o77
0b1010
1i8 1i16 1i32 1i64
1u8 1u16 1u32 1u64
1.0f32 1.0f64 1.0f128
"text"
'text'
"""text"""
'''text'''
f"value={x}"
[1, 2, 3]
{"a": 1, "b": 2}
```

## Imports

```text
use std
use module.path
use module.path as alias
use std module.path as alias
use module.path (name, other)
use module.path (name as alias)
use module.path:profile
use module.path:profile *
use "./relative.ny" as alias
use "./relative.ny" (name)
use "./relative.ny":debug
```

## Modules

```text
module name *
module name (a, b, c)
module name {
   export group(a, b)
   internal(_helper)
}
module name generated from Spec {
   key = value
   emit make_backend(Contract)
}
```

## Bindings

```text
def name = expr
mut name = expr
def Type: name = expr
mut Type: name = expr
def a, b = expr
mut a, b
del name
```

## Functions

```text
fn name(params) { body }
fn name(params): Type { body }
fn name(params): Type = expr
fn(v){ expr }
fn(a, b){ body }
```

Parameters:

```text
name
Type: name
name = default
Type: name = default
...rest
```

## Types

```text
T
T<A>
T<A, B>
?T
*T
fnptr
seq
sequence
numeric
indexable
iterable
allocator
handle
c64
c128
complex
```

Generic type arguments are written with angle brackets. This includes standard
containers such as `list<int>`, `dict<str, int>`, `set<str>`, `Result<T, E>`,
and user ADTs such as `Option<int>`.

## Structs, ADTs, and impl blocks

```text
struct Vec2 {
   f64: x
   f64: y
}

enum Shape {
   Circle(int: radius),
   Rect(int: width, int: height),
   Empty
}

enum Option<T> {
   Some(T: value),
   None
}

impl Shape {
   fn area(self: s): int { 0 }
}

impl int, f32 {
   fn twice(self: x): self { x + x }
}

impl Meter {
   operator + self: self = add
   operator == self: bool = same
}
```

## Statements

```text
expr
return expr
if(cond){ body } else { body }
if cond { body } elif other { body } else { body }
if(def x = value x > 0){ body }
def x = if(cond){ value } else { fallback }
while(cond){ body }
while(mut i = 0 i < n ++i){ body }
for name in expr { body }
for value, index in expr { body }
for name in lo..hi { body }
match expr { arms }
case expr { arms }
try { body } catch name { body }
try { body } catch(_) { handler }
defer { body }
with Type: name = expr { body }
```

## Operators

```text
a + b
a - b
a * b
a / b
a % b
a == b
a != b
a < b
a <= b
a > b
a >= b
a && b
a || b
!a
&a
a & b
a | b
a ^ b
a ^^ b
~a
a << b
a >> b
a ?? b
a |> f
value?.member
cond ? a : b
```

Unary `&expr` is borrow syntax and lowers to the ownership helper
`borrow(expr)`. Binary `a & b` remains bitwise-and. Binary `a ^ b` is
exponentiation. Binary `a ^^ b` is bitwise XOR.

## Match and case

```text
case value {
   literal -> expr
   a, b, c -> expr
   lo..hi -> expr
   _ -> expr
}

match value {
   pattern -> expr
   _ -> expr
}
```

## Layouts

```text
layout Name {
   Type: field,
   Type: field2
}

layout Packed pack(1){
   u8: tag,
   i32: value
}

layout record Row derive(default, eq, hash, debug_str) pack(4){
   i32: id
}

layout shape Header derive(load, store, zero) pack(8){
   str: sender
}

layout guard Header: h = value else { fallback }
```

## Externs

```text
#include <stdlib.h> as "c"
#include "./header.h" as ""

extern "library" {
   fn symbol(Type: arg): Type
}

extern "" {
   fn process_symbol(Type: arg): Type
}
```

## Attributes

```text
@pure
@effects(none)
@effects(io)
@effects(alloc)
@effects(ffi)
@effects(thread)
@effects(all)
@async_effects
@jit
@thread
@naked
@consteval
@constant_time
@llvm(noinline)
@llvm("frame-pointer", "all")
@readnone
@readonly
@writeonly
@argmemonly
@nounwind
@mustprogress
@willreturn
@hot
@cold
@flatten
@returns_owned
@returns_borrow(x)
@borrows(x)
@consumes(x)
@mutates(x)
@releases(x)
@forgets(x)
```

## Compile time

```text
comptime { body }
comptime table Name { pattern -> value }
comptime match Name(key, fallback)
comptime template name(args) { declarations }
comptime emit name(args)
assert_compile(cond, "message")
assert_compile_range(value, lo, hi, "message")
assert_compile_index(container, index, "message")
```

## Platform guards

```text
#linux { body }
#elif macos { body }
#elif windows { body }
#else { body }
#endif

#if(arm && !aarch64){ body }
```

Platform guards are compile-time selection forms. Common guard names include
`linux`, `macos`, `windows`, `unix`, `x86`, `x86_64`, `arm`, and `aarch64`.

## Builtins

```text
embed("path")
asm("template", "constraints", args...)
llvm("ctpop.i64", value)
__main()
```

`embed` reads file content at compile time. `asm` lowers inline assembly for
the current backend. `llvm` calls LLVM intrinsics. `__main()` identifies direct
script execution.

## Runnable file shape

```ny
use std.core

module sample(add)

fn add(int: a, int: b): int {
   a + b
}

assert_eq(add(1, 2), 3, "add")
```

## Related

- [source.md](source.md) for imports, modules, and scripts.
- [values.md](values.md) for literal behavior.
- [functions.md](functions.md) for blocks, parameters, and returns.
- [types.md](types.md) for type expression behavior.
- [operators.md](operators.md) for operator behavior.
- [control-flow.md](control-flow.md) for branch and loop semantics.
