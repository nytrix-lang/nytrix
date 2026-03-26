# Compile time

Compile-time forms run during compilation and can produce tables, matches,
templates, emitted declarations, and generated modules.

## Compile-time blocks

```text
comptime { body }
```

Rules:

- Runs during compilation.
- Returns a value embedded in the program.
- Uses the same resolved imports and aliases as the surrounding source unit.
- Emits only the standard-library helpers reached by the block.
- Can reuse earlier immutable compile-time constants.
- Does not run unrelated top-level user code.
- Does not capture runtime globals.

```text
use std.core

def xs = comptime{ range(4).map(fn(i){ i + 1 }) }
assert_eq(to_str(xs), "[1, 2, 3, 4]", "comptime imports")
```

```text
use std

def base = comptime{ 2^5 }
def xs = comptime{ range(4).map(fn(i){ i + base }) }
```

For runtime data, put the value inside the block or bind it first as an
immutable compile-time value.

## Tables

```text
comptime table Name {
   pattern -> value
   _ -> fallback
}
```

Compile-time tables make static dispatch explicit. They fit compact lookup
surfaces, generated classifier logic, and branch-free native output where the
compiler can lower the shape.

## Match helpers

```text
comptime match Name(key, fallback)
```

`comptime match` queries a compile-time table by key and returns a fallback
when no arm matches.

## Templates

```ny
comptime template name(args) {
   declarations
}
```

Templates are hygienic AST templates. Template bodies emit declarations, not
text pasted into the source file.

## Emit

```text
comptime emit name(args)
```

`emit` inserts generated declarations from a template or generator.

## Compile-time proofs

Compile-time assertions make safety checks part of compilation:

```text
assert_compile((4 * 11) == 44, "folded arithmetic")
assert_compile_range(i, 0, 3, "loop index range")
assert_compile_index(xs, i, "list index bounds")
```

`assert_compile` fails compilation when the condition is known false.
`assert_compile_range` requires the compiler to prove an integer is within a
closed range. `assert_compile_index` requires the compiler to prove that an
index is in bounds for the container. `range_proven(value, lo, hi)` and
`index_proven(container, index)` expose the same proof engine as compile-time
booleans.

These checks apply to safety boundaries such as parser tables, byte decoders,
crypto code, native buffers, and loops where an out-of-range value would become
a memory or correctness bug.

`--safe-mode` uses the same proof engine for compiler-tracked raw memory
accesses. If an allocation size is known, `load8`/`store8` and wider raw
loads/stores require a byte offset proven to stay inside the allocation.

## Generated modules

```text
module pkg.generated generated from Spec {
   native_prefix = "x"
   emit make_backend(Contract)
}
```

Generated modules attach compile-time configuration to a module and emit the
declarations that become part of the module surface.

## Boundaries

Compile-time work emits ordinary declarations. Generated names remain stable,
and emitted public surfaces remain explicit.

## Platform selection

Platform guards select source at compile time:

```text
#linux { os_tag = "linux" }
#elif macos { os_tag = "macos" }
#elif windows { os_tag = "windows" }
#else { os_tag = "other" }
#endif

#if(arch() == "x86_64"){ return 64 }
```

Guard names include OS families and CPU families. `comptime{ ... }` can call
compile-time facts such as `arch()`, `os()`, `__os_name()`, and `__main()`.

## Embedded files

```ny
def text = embed("etc/tests/rt/embed.ny")
```

`embed(path)` reads a file into the compiled program. Paths are resolved from
the source checkout or active source root used by the compiler.

## Syntax extension registry

`std.core.syntax` exposes the runtime/comptime syntax registry used by the
extension tests:

```text
use std.core.syntax as syntax

mut reg = syntax.new_registry()
reg = syntax.register_macro_in(reg, "double", handler)
syntax.expand_macro_in(reg, "double", [21])
```

The surface covers process-wide and local registries, macro handlers,
attribute handlers, form construction, deep/fixpoint expansion, registry
clone/merge operations, and deterministic rewrite passes. Missing macro
handlers return `nil`; missing attribute handlers return the original node.

## Related

- [source.md](source.md) for generated module syntax.
- [metaprogramming.md](../learn/metaprogramming.md) for practical usage.
- [tooling.md](../learn/tooling.md) for `ny fmt --metaprog`.
