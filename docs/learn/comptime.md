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
- Syntax extensions and source rewriters change source processing. Prefer
  ordinary functions or templates unless the grammar itself needs to change.

## Syntax extensions

| Need | Mechanism |
| --- | --- |
| Compute a compile-time value | `#run`, `#comptime`, or `static_assert` |
| Generate declarations from known inputs | `comptime template` and `comptime emit` |
| Check a declaration contract | A supported function attribute |
| Introduce a new source form | A macro handler |
| Rewrite an existing form | A deterministic rewriter |

The syntax registry lives under `core.compiler.syntax`. Macro and rewrite
handlers transform accepted source forms into expanded syntax; their output
still needs to compile and run on the intended backend. See the
[registry reference](../spec/comptime.md#syntax-extension-registry) for local
and process-wide registries, handler lookup, and expansion operations.

```bash
ny doc get core.compiler.syntax
ny doc search --symbols register_macro_in
```

## Inspect and verify

```bash
ny fmt --metaprog file.ny
ny fmt --specialize file.ny
ny --expand file.ny
```

These commands inspect candidates or expansions, not runtime correctness.
Compile and run the generated program; see the
[Comptime specification](../spec/comptime.md) and [Testing](testing.md).
