<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":110,"summary":"Evaluate constants and generate declarations during compilation."} -->
# Compile-time programming

Use compile-time evaluation for values or declarations that can be determined
from source inputs. The compiler checks generated declarations as ordinary
source; compile-time code cannot use runtime results to rewrite the program.

## Evaluate a constant

```ny
use core

static_assert(2 ** 5 == 32, "power is exponentiation")
def scale = #run 2 ** 5

#main {
   assert_eq(scale, 32, "compile-time value")
}
```

`#run` and `#comptime` evaluate an expression during compilation. Use `**` for
power; `^` is bitwise XOR. Compile-time evaluation has a shared fuel budget;
see [Comptime](../spec/comptime.md#boundaries).

## Generate repeated declarations

Use a table for fixed compile-time lookup and a template when source
structure must be generated. The specification lists the supported table,
reflection, splice, and module forms. Keep generated names deterministic and
make the generated API no broader than the handwritten contract it replaces.

When a generated declaration is hard to understand, inspect the expansion,
then compile or test the resulting program through its intended backend.

## Boundaries

- Compile-time execution is not an unrestricted second runtime. Unsupported
  operations and exhausted budgets produce diagnostics.
- Generated source still goes through normal parsing and semantic checks.
- Embedded files are build inputs; use stable project-relative paths and make
  sure the build owns the referenced file.
- Syntax extensions and source rewriters are a separate mechanism. Prefer
  ordinary functions or templates unless the source grammar itself needs to
  change.

## Inspect and verify

```bash
ny fmt --metaprog file.ny
ny fmt --specialize file.ny
ny --expand file.ny
```

These commands help inspect candidates or expansions; they do not replace
compiling and running the generated program. See [Comptime specification](../spec/comptime.md),
[Syntax extensions](syntax.md), and [Testing](testing.md).
