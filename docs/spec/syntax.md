<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":11,"summary":"Source spellings for Nytrix comments, literals, declarations, blocks, and expressions."} -->
# Syntax

Topic references define meaning, type rules, and backend limits for these source
forms. The parser-oriented grammar is in
[Info EBNF](../info/nytrix.ebnf).

## Contents

- [Comments and literals](#lexical-forms)
- [Bindings and functions](#bindings)
- [Statements, operators, and attributes](#statements-and-control-flow)

> **Note:** The blocks below show grammar forms, not standalone programs.
> [Runnable shape](#runnable-shape) is a complete example.

## Lexical forms

### Spelling: comments

`;` starts a line comment; it is not a statement separator or terminator. The
lexer also supports delimited multiline comments: `;MARKER ... MARKER;` and
the paired forms `;{ ... };`, `;< ... >;`, `;[ ... ];`, and `;( ... );`.
There are no C-style `/* ... */` comments.

```ny
; line comment
value = 1 ; inline comment
;NOTE
ignored documentation block
NOTE;
;{ ignored block };
```

### Meaning: statement boundaries

Adjacent statements are separated by the grammar and source layout. Blocks use
braces. Newlines can separate statements; semicolons do not terminate them.

Some spellings are reserved or retired so the diagnostics can name the
supported form. `let`, `func`, `import`, `var`, `void`, and `printf` are
rejected with a hint pointing at `def`/`mut`, `fn`, `use`, `none`, and `print`;
`macro` is a retired statement form whose hint names `quote` with `${...}`
splicing. Words that name live forms are documented where the form lives:
`impl` and `operator` in [Traits](traits.md), `with` in
[Control flow](control-flow.md#with). Nothing else is advertised as a future
form; new syntax is announced in the changelog when it lands.

## Literals

### Spelling

| Kind | Spellings |
| --- | --- |
| Nil and booleans | `nil`, `true`, `false` |
| Integer bases | Decimal `123`; hexadecimal `0xff`; octal `0o77`; binary `0b1010` |
| Fixed-width integers | `1i8`, `1i16`, `1i32`, `1i64`, `1u8`, `1u16`, `1u32`, `1u64` |
| Floating point | `1.0f32`, `1.0f64`, `1.0f128` |
| Strings | `"text"`, `'text'`, `"""text"""`, `'''text'''` |
| Formatted strings | `f"value={x}"` |
| Collections | `[1, 2, 3]`, `{"a": 1}` |

```ny
def count = 123
def message = f"value={count}"
def items = [1, 2, 3]
```

### Meaning

Integer suffixes and literal interpretation depend on type context. `list(n)`
creates an empty list with reserved capacity `n`; it does not create initialized
elements. See [Values](values.md) for value and container behavior.

## Bindings

Bindings use `def` for immutable names and `mut` for names that may be
reassigned. Type annotations precede names.

```ny
def name = expr
mut name = expr
def Type name = expr
mut Type name = expr
def a, b = expr
mut a, b
del name
```

## Functions

Functions can have inferred or explicit return types; the final expression in
the body is the result when no `return` exits earlier.

```ny
fn name(params) { body }
fn name(params) Type { body }
fn name(params) Type = expr
fn(value) { expression }
fn(value) expression

parameter
Type parameter
parameter = default
Type parameter = default
...rest
```

See [Functions](functions.md) for calls, defaults, closures, and attributes.

## Types and declarations

```ny
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

```ny
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

```ny
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

## Attributes

```ny
@pure
@effects(none|io|alloc|ffi|thread|read|write|all)
@consteval
@constant_time
@inline  @noinline  @tailcall
@returns_owned  @returns_borrow(name)
@borrows(name)  @consumes(name)  @mutates(name)
```

See [Functions](functions.md) for attribute contracts.

## Native declarations

```ny
#include <stdlib.h> as "c"
#include "./header.h"
extern "library" { fn symbol(Type argument) Type }
layout Name { Type field, Type other }
```

See [Native](native.md) for FFI and ABI rules.

## Files, modules, and imports

```ny
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

## Compile-time and proof forms

```ny
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
