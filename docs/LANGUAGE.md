# Nytrix Language Spec

Compact, predictable, expression-oriented systems language.  

> WorkInProgress: Behavior may change. Tests are the source of truth.

## System
- compiler: LLVM-based, AOT
- evaluation: expression-oriented
- entry: global scope, top-to-bottom

## Lexical
- identifiers: `[A-Za-z_][A-Za-z0-9_]*`
- comments: `;` single line
- ignored blocks: standalone non-docstring strings
- docstrings: string immediately after `fn`
- strings: `'...'`, `"..."`, `'''...'''`, `"""..."""`, UTF-8, `f"{expr}"`

## Types & Values
- raw: `i32`, `i64`, `f64`
- core: `bool`, `str`, `ptr`, `fn`
- advanced: `Result`, `Option`, `List`, `Dict`, `Set`, `Tuple`
- dynamic: `any`
- literals:
  - int: decimal, `0x`, `0o`, `0b`
  - float: `1.0`, `.5`, `1e-3`
  - bool: `true`, `false`
  - nil: `nil`
  - list: `[a, b]`
  - set: `{a, b}`
  - dict: `{"k": v}`
  - tuple: `(a, b,)` or `tuple(x)`

## Syntax
```ny
def x = 10
mut y = 20
fn f(a, b=default){...}

if(c){...} elif(c){...} else{...}
while(c){...}
for(x in it){...}

match v { p -> e, _ -> d }
try{...} catch(e){...}
defer{...}

def fd = open(...) ?
````

## Operators

* arithmetic: `+ - * / %`
* compare: `== != < > <= >=`
* logical: `&& || !`
* unary: `-`
* error: `?`

## Modules

* export: `module m(a,b)` | `module m *` | `module m`
* import: `use path as a` | `use path (a, b as c)` | `use path *`
* resolution: `std.*` → standard library; relative/bare → current dir, then std/lib

## Low-level

`extern fn` `asm` `syscall`

## Standard Library

* `std.core`: base types, `Result`, `Option`
* `std.str.io`: formatted I/O
* `std.os.sys`: syscalls
* `std.os.process`: processes
