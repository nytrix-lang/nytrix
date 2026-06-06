# Metaprogramming

Compile-time features turn static data and repeated declaration patterns into
ordinary generated source.

Compile-time generation fits repeated declarations and static checks. Runtime
behavior remains ordinary functions, tests, or data files.

## Use cases

| Shape | Use |
| --- | --- |
| Compile-time table | Static lookup, byte classifier, token kind, known mapping. |
| Compile-time match | Query a table with fallback behavior. |
| Template | Emit repeated declarations without copy/paste. |
| Generated module | Bind a data contract to emitted module declarations. |
| Syntax registry | Register and expand explicit macro/attribute/rewrite handlers. |
| Embedded file | Compile file content into a program with `embed(path)`. |

## Tables

```ny
comptime table Classify {
   9, 10, 13 -> "space"
   32 -> "space"
   48..57 -> "digit"
   _ -> "other"
}
```

Tables fit mappings that are data rather than business logic.

## Templates

```ny
comptime template make_getter(name, field) {
   declarations
}
```

Templates emit declarations. Generated names remain stable and searchable.

## Generated modules

```ny
comptime template make_backend(name) {
   fn name() int { 1 }
}

module pkg.generated generated from Spec {
   emit make_backend(run)
}

run()
```

Generated modules fit contracts that define a family of related helpers.

## Audit commands

```bash
ny fmt --metaprog file.ny
ny fmt --specialize file.ny
ny fmt --trim --check file.ny
```

These commands report repeated code, static dispatch, and source that can move
to a compile-time form.

## Syntax registry

`std.core.syntax` is the public API for explicit syntax extension experiments:

```ny
use std.core.syntax as syntax

fn double(node){
   def args = node.get("args", [])
   args.get(0, 0) * 2
}

mut reg = syntax.new_registry()
reg = syntax.register_macro_in(reg, "double", double)
assert(syntax.expand_macro_in(reg, "double", [21]) == 42, "macro")
```

Local registries fit tests and generators. The process-wide registry is for
extensions that are intentionally global. Rewriters run registered passes in
deterministic order until the value stabilizes or the configured limit is
reached.

## Embedded files and target code

```ny
def source = embed("docs/README.md")
```

`embed(path)` is for static fixtures, shaders, generated tables, and source
snippets that need to ship inside the compiled program. Inline assembly belongs
at native boundaries and belongs behind target checks such as `#x86_64` or
`#if(arch() == "aarch64")`.

## Boundaries

Generated declarations expose ordinary APIs after expansion. They do not
substitute for missing runtime APIs.

Generated names are ordinary public names after expansion and are searchable
with `ny doc search --symbols`.

## Related

- [comptime.md](../spec/comptime.md) for exact forms.
- [tooling.md](tooling.md) for audit commands.
- [programs.md](programs.md) for module and export shape.
