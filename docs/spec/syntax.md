<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":11,"summary":"The complete source syntax: comments, literals, declarations, blocks, and expressions."} -->
# Syntax

Syntax records source spellings. Topic pages define behavior.

## Lexical

```text
comment        := ";" text-until-newline | ";" marker text-block marker ";"
marker         := [A-Za-z_][A-Za-z0-9_]*
identifier     := [A-Za-z_][A-Za-z0-9_]*
module-path    := identifier ("." identifier)*
block          := "{" source* "}"
```

There are no standard C-style block comments. Heredoc-style multiline comments can be written as `;MARKER ... MARKER;`. Semicolons are comments, not statement terminators.

## Grammar

The statement grammar is a sequence of statements, declarations, or expressions
separated by newlines. A block is `{` statements-and-expressions `}`.

```text
source          := (statement | declaration)*
statement       := expression
                 | "return" expression
                 | "if" "(" condition ")" block ("elif" "(" condition ")" block)* ("else" block)?
                 | "if" condition block ("elif" condition block)* ("else" block)?
                 | "while" "(" condition ")" block
                 | "while" "(" init condition update ")" block
                 | "for" pattern "in" expression block
                 | "for" pattern "," pattern "in" expression block
                 | "for" "(" pattern "in" expression ")" block
                 | "for" "(" init condition update ")" block
                 | "match" expression "{" arm* "}"
                 | "case" expression "{" arm* "}"
                 | "try" block "catch" binding block
                 | "defer" block
                 | "with" type ":" binding "=" expression block
                 | "break" | "continue"
                 | label ":" statement
                 | "goto" label
arm             := pattern ("if" expression)? "->" expression
init            := binding "=" expression
condition       := expression
update          := expression
binding         := identifier | type identifier
pattern         := literal | identifier | "(" pattern ")" | "_"
```

`if` with a condition and no parentheses uses the same branch forms as the
parenthesized spelling. In binding or expression position each branch needs one
value-producing statement, such as a final expression or a nested
value-producing `if`.

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
with Type name = expr { body }
```

Declarations use `Type name`. Resource scopes are also type-first: write
`with Type name = value { ... }`; the legacy colon spelling is rejected.

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
exponentiation. `^^` is bitwise XOR. See [Operators](operators.md#precedence)
for the precedence and associativity table.

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

Portable source should use attributes that state a semantic contract. Effects,
ownership, compile-time evaluation, and constant-time intent are checked or
preserved by Nytrix independently of a particular machine-code backend.

```ny
@pure
@effects(none|io|alloc|ffi|thread|all)
@async_effects
@thread
@naked
@consteval
@constant_time
@optimize(0|1|2|3)
@inline @noinline @tailcall
@hot @cold @flatten
@accel @accel(spirv)
@returns_owned @returns_borrow(x)
@borrows(x) @consumes(x) @mutates(x) @releases(x) @forgets(x)
```

Choose behavior through typed APIs and standard-library modules first. Backend
tuning annotations are compatibility details and are intentionally not the
normal way to express program logic.

## Proof declarations

```ny
fn lemma name(params) { proposition }
```

A lemma body is a single proposition. `A → B` is implication syntax and is
equivalent to `!A || B`. See [Proofs](../learn/proofs.md) for witness
construction and use.

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
intrinsic("ctpop.i64", value)
argc()
__os_name()
__main()
__tagof(value)
__runtime_tag("name")
__layout_size("Name")
__layout_align("Name")
__layout_offset("Name", "field")
```

`embed` reads file content at compile time. `asm` and `intrinsic(...)` are
real target-specific forms; their supported backends are checked at
compilation.
Prefer public wrappers such as `argc()` over double-underscore runtime helpers
when one exists.

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

- [Units](units.md)
- [Values](values.md)
- [Functions](functions.md)
- [Types](types.md)
- [Operators](operators.md)
- [Control Flow](control-flow.md)