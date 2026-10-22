<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":11,"summary":"Source spellings for Nytrix comments, literals, declarations, blocks, and expressions."} -->
# Syntax

This page is a source-form index. Topic pages define meaning, type rules, and
backend limits. The parser-oriented grammar is in
[Info EBNF](../info/nytrix.ebnf).

## Lexical forms

`;` starts a line comment; it is not a statement separator or terminator. The
lexer also supports delimited multiline comments: `;MARKER ... MARKER;` and
the paired forms `;{ ... };`, `;< ... >;`, `;[ ... ];`, and `;( ... );`.
There are no C-style `/* ... */` comments.

```text
; line comment
value = 1 ; inline comment
;NOTE
ignored documentation block
NOTE;
;{ ignored block };
```

Adjacent statements are separated by the grammar and source layout. Blocks use
braces. Newlines can separate statements; semicolons do not terminate them.

## Literals and values

```text
nil  true  false
123  0xff  0o77  0b1010
1i8 1i16 1i32 1i64  1u8 1u16 1u32 1u64
1.0f32 1.0f64 1.0f128
"text"  'text'  """text"""  '''text'''  f"value={x}"
[1, 2, 3]  {"a": 1, "b": 2}
```

`list(n)` creates an empty list with reserved capacity `n`; it does not create
initialized elements. See [Values](values.md) for value and container behavior.

## Files, modules, and imports

```text
use core
use module.path
use module.path as alias
use module.path (name, other)
use module.path (name as alias)
use module.path:profile
use "./relative.ny" as alias
use "./relative.ny" (name)

module name *
module name (a, b, c)
module name { export group(a, b) internal(_helper) }
module name generated from Spec { key = value emit make_backend(Contract) }

#main { body }
```

`#main` marks direct-entry work in a module. Import and export rules are in
[Units](units.md).

## Bindings and functions

Bindings use `def` for immutable names and `mut` for names that may be
reassigned. Type annotations precede names. Functions can have inferred or
explicit return types; the final expression in the body is the result when no
`return` exits earlier.

```text
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
fn(value) { expression }

parameter
Type parameter
parameter = default
Type parameter = default
...rest
```

See [Functions](functions.md) for calls, defaults, closures, and attributes.

## Types and declarations

```text
T
T<A>
T<A, B>
?T
*T
fnptr
numeric  sequence  indexable  iterable  allocator
handle
c64  c128  complex

struct Vec2 { f64 x, f64 y }
enum Color { Red, Green, Blue }
enum Shape { Circle(int radius), Rect(int width, int height), Empty }
enum Option<T> { Some(T value), None }

trait Show { fn show(any value) str }
impl Shape { fn area(self shape) int { 0 } }
impl Show for Point { fn show(Point value) str { "Point" } }
```

`struct` declares a language value; `layout` declares a native representation.
See [Types](types.md), [Traits and impl](traits.md), and [Native](native.md).

## Statements and control flow

```text
expression
return expression
if (condition) { body } elif (other) { body } else { body }
if condition { body } elif other { body } else { body }
if (def value = expression condition) { body }
while (condition) { body }
for value in iterable { body }
for value, index in iterable { body }
for (mut index = 0 index < limit ++index) { body }
for value in lower..upper { body }
break
continue
match expression { arms }
case expression { arms }
try { body } catch error { handler }
defer { cleanup }
with Type name = expression { body }
```

`case` dispatches values and ranges; `match` handles patterns and ADTs. See
[Control Flow](control-flow.md) and [Patterns](patterns.md).

## Operators

```text
+  -  *  /  %  **  ^^  ^
=  +=  -=  *=  /=  %=  **=  ^^=  ^=
++  --
==  !=  <  <=  >  >=
&&  ||  !
&  |  ^  ~  <<  >>
??  |>  ?.  ? :
call(...)  value[index]  value.member
```

Unary `&expression` is borrow syntax. Binary `&` is bitwise-and, `^` is XOR,
and both `**` and `^^` spell exponentiation. See [Operators](operators.md)
for precedence and associativity.

## Native declarations and attributes

```text
#include <stdlib.h> as "c"
#include "./header.h"
extern "library" { fn symbol(Type argument) Type }
layout Name { Type field, Type other }

@pure
@effects(none|io|alloc|ffi|thread|read|write|all)
@consteval
@constant_time
@inline  @noinline  @tailcall
@returns_owned  @returns_borrow(name)
@borrows(name)  @consumes(name)  @mutates(name)
```

See [Native](native.md) for FFI and ABI rules, and [Functions](functions.md)
for attribute contracts.

## Compile-time and proof forms

```text
comptime { body }
#run expression
#comptime expression
comptime table Name { pattern -> value }
comptime template name(args) { declarations }
comptime emit name(args)
comptime fields(Layout) as field { emit ... }
static_assert(condition, "message")
fn lemma name(params) { proposition }
```

See [Compile-time execution](comptime.md) and [Types](types.md) for proof
forms and their limits.

## Runnable shape

```ny
use core

module sample(add)

fn add(int a, int b) int {
   a + b
}

#main {
   assert_eq(add(19, 23), 42, "add")
}
```

## Related

- [Units](units.md)
- [Values](values.md)
- [Functions](functions.md)
- [Types](types.md)
- [Operators](operators.md)
- [Control Flow](control-flow.md)
