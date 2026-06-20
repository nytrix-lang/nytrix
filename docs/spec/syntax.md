# Syntax

Syntax records source spellings. Topic pages define behavior.

## Lexical

```ny
comment        := ";" text-until-newline | ";" marker text-block marker ";"
marker         := [A-Za-z_][A-Za-z0-9_]*
identifier     := [A-Za-z_][A-Za-z0-9_]*
module-path    := identifier ("." identifier)*
block          := "{" source* "}"
```

There are no standard C-style block comments. Heredoc-style multiline comments can be written as `;MARKER ... MARKER;`. Semicolons are comments, not statement terminators.

## Literals

```ny
nil  true  false
123  0xff  0o77  0b1010
1i8 1i16 1i32 1i64  1u8 1u16 1u32 1u64
1.0f32 1.0f64 1.0f128
"text"  'text'  """text"""  '''text'''  f"value={x}"
[1, 2, 3]  {"a": 1, "b": 2}
```

`list(n)` creates an empty list with reserved capacity `n`; it does not create
`n` initialized elements.

## Source Structure

```ny
use std
use module.path
use module.path, other.module
use module.path as alias
use std module.path as alias
use module.path (name, other)
use module.path (name as alias)
use module.path:profile
use module.path:profile *
use "./relative.ny" as alias
use "./relative.ny" (name)
use "./relative.ny":debug

module name *
module name (a, b, c)
module name { export group(a, b) internal(_helper) }
module name generated from Spec { key = value emit make_backend(Contract) }

#main { body }
```

`#main` is the direct-entry guard. Prefer it over manual `__main()` checks.

## Bindings And Functions

```ny
def name = expr
mut name = expr
def Type name = expr
mut Type name = expr
def a, b = expr
mut a, b
del name

fn name(params) { body }
fn name(params) Type { body }
fn name(params) Type = expr
fn(v){ expr }
```

Parameters:

```ny
name
Type name
name = default
Type name = default
...rest
```

## Types

```ny
T
T<A>
T<A, B>
?T
*T
fnptr
seq | sequence
numeric
indexable
iterable
allocator
handle
c64 | c128 | complex
```

Common generic forms include `list<int>`, `dict<str, int>`, `set<str>`,
`Result<T, E>`, and ADTs such as `Option<int>`.

## Data Declarations

```ny
struct Vec2 { f64 x, f64 y }

enum Color {
   Red,
   Green,
   Blue
}

enum Shape {
   Circle(int radius),
   Rect(int width, int height),
   Empty
}

enum Option<T> {
   Some(T value),
   None
}

impl Shape { fn area(self s) int { 0 } }
impl int, f32 { fn twice(self x) self { x + x } }
impl Meter { operator + self: self = add }
```

## Statements

```ny
expr
return expr
if(cond){ body } elif(other){ body } else { body }
if cond { body } elif other { body } else { body }
if(def x = value x > 0){ body }
def x = if(cond){ value } else { fallback }
while(cond){ body }
while(mut i = 0 i < n ++i){ body }
for value in expr { body }
for value, index in expr { body }
for(index in expr){ body }
for(mut i = 0 i < n ++i){ body }
for value in lo..hi { body }
match expr { arms }
case expr { arms }
try { body } catch name { body }
try { body } catch(_) { handler }
defer { body }
with Type: name = expr { body }
```

Declarations use `Type name`. Resource scopes are also type-first, but keep the
colon separator: `with Type: name = value { ... }`.

## Operators

```ny
a + b   a - b   a * b   a / b   a % b   a ^ b
a = b   a += b  a -= b  a *= b  a /= b  a %= b
++a      --a
a == b  a != b  a < b   a <= b  a > b   a >= b
a && b  a || b  !a
&a      a & b   a | b   a ^^ b  ~a      a << b  a >> b
a ?? b  a |> f  value?.member   cond ? a : b
fn_name(arg)  value[index]  value.member  module.helper(value)
```

Unary `&expr` is borrow syntax. Binary `&` is bitwise-and. `^` is
exponentiation. `^^` is bitwise XOR.

## Dispatch

```ny
case value {
   literal -> expr
   a, b, c -> expr
   lo..hi -> expr
   _ -> expr
}

match value {
   Pattern(value) -> expr
   _ -> expr
}
```

## Native Forms

```ny
#include <stdlib.h> as "c"
#include "./header.h" as ""

extern "library" { fn symbol(Type arg) Type }
extern { fn process_symbol(Type arg) Type }

layout Name { Type field, Type field2 }
layout Packed pack(1){ u8 tag, i32 value }
layout record Row derive(default, eq, hash, debug_str) pack(4){ i32 id }
layout shape Header derive(load, store, zero) pack(8){ str sender }
layout guard Header h = value else { fallback }
```

Fields use the short `Type name` spelling.

## Attributes

```ny
@pure
@effects(none|io|alloc|ffi|thread|all)
@async_effects
@jit
@thread
@naked
@consteval
@constant_time
@llvm(noinline)
@llvm("frame-pointer", "all")
@readnone @readonly @writeonly @argmemonly @nounwind
@mustprogress @willreturn @hot @cold @flatten
@returns_owned @returns_borrow(x)
@borrows(x) @consumes(x) @mutates(x) @releases(x) @forgets(x)
```

## Compile Time

```ny
comptime { body }
comptime table Name { pattern -> value }
comptime match Name(key, fallback)
comptime template name(args) { declarations }
comptime emit name(args)
for name in comptime [values...] { emit template(name) }
comptime fields(Layout) as f { emit ... }
comptime exports(Module) as name { emit ... }
comptime diagnostic rule name { when predicate error "message" fix "hint" }
static_assert(cond, "message")
assert_compile(cond, "message")
assert_compile_range(value, lo, hi, "message")
assert_compile_index(container, index, "message")

#linux { body }
#elif macos { body }
#elif windows { body }
#else { body }
#endif
#if(arch() == "x86_64"){ body }
```

## Builtins

```ny
embed("path")
asm("template", "constraints", args...)
llvm("ctpop.i64", value)
argc()
__os_name()
__main()
__tagof(value)
__runtime_tag("name")
__layout_size("Name")
__layout_align("Name")
__layout_offset("Name", "field")
```

`embed` reads file content at compile time. `asm` lowers inline assembly for
the active backend. `llvm` calls LLVM intrinsics. Prefer public wrappers such
as `argc()` over double-underscore runtime helpers when one exists.

## Runnable Shape

```ny
use std.core

module sample(add)

fn add(int a, int b) int {
   a + b
}

#main {
   assert_eq(add(1, 2), 3, "add")
}
```

## Related

- [source.md](source.md)
- [values.md](values.md)
- [functions.md](functions.md)
- [types.md](types.md)
- [operators.md](operators.md)
- [control-flow.md](control-flow.md)
