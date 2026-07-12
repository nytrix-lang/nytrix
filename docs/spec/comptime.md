# Compile time

Compile-time forms run during compilation and produce tables, matches,
templates, emitted declarations, and generated modules.

## Compile-time blocks

```ny
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

```ny
use std.core

def xs = comptime{ range(4).map(fn(i){ i + 1 }) }
assert_eq(to_str(xs), "[1, 2, 3, 4]", "comptime imports")
```

```ny
use std

def base = comptime{ 2^5 }
def xs = comptime{ range(4).map(fn(i){ i + base }) }
```

For runtime data, put the value inside the block or bind it first as an
immutable compile-time value.

`return expr` or a final value-producing expression sets the value of a
compile-time block. A block with only declarations or non-value statements
evaluates to `nil`.

## Tables

```ny
comptime table Name {
   pattern -> value
   _ -> fallback
}
```

Compile-time tables make static dispatch visible. Use them for lookup
surfaces, generated classifier logic, and branch-free native output.

The compiler also emits a compatibility helper named from the table. For
`comptime table KeyMap`, call `_key_map(key)` or `_key_map(key, fallback)`.
Prefer `comptime match KeyMap(key, fallback)` in new code.

## Match helpers

```ny
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

```ny
comptime emit name(args)
for axis in comptime ["x", "y", "z"] {
   emit make_axis_family(axis)
}
```

`emit` inserts generated declarations from a template or generator.
Compile-time `for` iterates a compile-time list and lets the body emit
declarations for each value.

## Reflection loops

```ny
comptime fields(LayoutName) as f {
   emit assert(__layout_offset("LayoutName", f.name) == f.offset, "field")
}

comptime exports(ModuleName) as name {
   emit assert(name != "", "export name")
}
```

`fields` exposes `f.name`, `f.offset`, `f.index`, and `f.type` for each layout
field. `exports` exposes each exported module name as a string.

## Diagnostic rules

```ny
comptime diagnostic rule bad_layout_store {
   when call.name == "store_layout" && !is_literal(call.arg(1))
   error "store_layout needs a string literal layout name"
   fix "use store_layout(dst, \"LayoutName\", ...)"
}
```

Diagnostic rules let compile-time code reject a known bad call pattern. The
current rule surface supports call predicates such as `call.name`,
`call.arg(N)`, and helpers such as `is_literal`.

## Compile-time proofs

Compile-time assertions move safety checks into compilation. The `proof` type
in [types.md](types.md) provides the carrier for dependent and refinement
facts backed by the same engine.

```ny
assert_compile((4 * 11) == 44, "folded arithmetic")
static_assert((3 * 7) == 21, "folded arithmetic")
assert_compile_range(i, 0, 3, "loop index range")
assert_compile_index(xs, i, "list index bounds")
def proof arithmetic = prove((4 * 11) == 44, "arithmetic witness")
```

`static_assert` and `assert_compile` fail compilation when the condition is
known false.
`assert_compile_range` requires the compiler to prove an integer is within a
closed range. `assert_compile_index` requires the compiler to prove that an
index is in bounds for the container. `range_proven(value, lo, hi)` and
`index_proven(container, index)` expose the same proof engine as compile-time
booleans.

`prove(condition[, message])` is the only implicit-free constructor for the
`proof` type. It rejects false obligations and obligations whose truth is not
known during compilation. A proof has no inspectable payload and currently
witnesses construction success rather than carrying a proposition-indexed
proof term.

Use these checks at safety boundaries: parser tables, byte decoders, crypto
code, native buffers, and loops where an out-of-range value would break memory
or correctness.

`--safe-mode` uses the same proof engine for compiler-tracked raw memory
accesses. If an allocation size is known, `load8`/`store8` and wider raw
loads/stores require a byte offset proven to stay inside the allocation.

See [types.md](types.md) for `proof` type, dependent params, and refinement.

## Generated modules

```ny
module pkg.generated generated from Spec {
   native_prefix = "x"
   emit make_backend(Contract)
}
```

Generated modules attach compile-time configuration to a module and emit the
declarations that become part of the module surface.

## Boundaries

Compile-time work emits declarations. Generated names stay stable, and emitted
public surfaces stay visible.

## Platform selection

Platform guards select source at compile time:

```ny
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

```ny
use std.core.syntax as syntax

mut reg = syntax.new_registry()
reg = syntax.register_macro_in(reg, "double", handler)
syntax.expand_macro_in(reg, "double", [21])
```

The surface covers process-wide and local registries, macro handlers,
attribute handlers, form construction, deep/fixpoint expansion, registry
clone/merge operations, and deterministic rewrite passes. Missing macro
handlers return `nil`; missing attribute handlers return the original node.

## Result ownership

Heap results cross the comptime boundary by value. Strings, lists, tuples,
ranges, and dictionaries are reconstructed in the receiving program before the
temporary evaluator is destroyed; pointers into evaluator-owned storage are
never exposed. Nested containers are supported with bounded depth and size.
Invalid intermediate or generated evaluator modules are rejected before JIT
execution.

## Related

- [source.md](source.md) for generated module syntax.
- [metaprogramming.md](../learn/metaprogramming.md) for practical usage.
- [tooling.md](../learn/tooling.md) for `ny fmt --metaprog`.
