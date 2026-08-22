<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":110,"summary":"Generate checked declarations, tables, and source during compilation."} -->
# Comptime

Compile-time code produces ordinary Nytrix declarations. Keep the generated
result small, named, and covered by an executable assertion.

## Start with a value

```ny
use std.core

def scale = comptime { 2 ^ 5 }
assert(scale == 32, "compile-time value")
```

A compile-time block containing only declarations evaluates to `nil`. Return or
end with an expression when the block must produce a value.

## Use a table for fixed classification

```ny
use std.core

comptime table Opcode {
   0x00 -> "halt"
   0x10..0x1f -> "load"
   0x20..0x2f -> "math"
   _ -> "data"
}

fn opcode_name(i32 raw) str = comptime match Opcode(raw, "data")
assert(opcode_name(0x24) == "math", "opcode class")
```

Tables keep a static classification beside the code that consumes it. Use
`case` for ordinary literal and range dispatch.

## Emit repeated declarations

`emit` and `comptime for` generate source from a static input. Use this only
when repeated declarations are clearer than a runtime loop. Run
`ny fmt --metaprog file.ny` before reviewing the generated form.

## Reflect over known structure

`comptime fields(Layout)` and `comptime exports(Module)` iterate static
metadata. Generate checks, adapters, or tables from those loops. Keep a
runtime assertion that proves the generated declaration has the required
behavior.

## Generated modules and embedded files

Generated modules follow the same module and export rules as handwritten
files. `embed(path)` reads a build-time resource into the source contract.
Keep paths repository-relative and ensure the build input owns the file.

## Diagnostic rules

`comptime diagnostic rule` declares a compile-time condition, error, and repair
for a project-specific invalid form. Put a diagnostic rule near the generated
contract it protects. Use one clear error for one repairable condition.

## Tooling

```bash
ny fmt --metaprog file.ny
ny fmt --specialize file.ny
ny fmt --trim --check file.ny
ny --strict-types file.ny
```

`--metaprog` and `--specialize` are review tools. They report candidates and
do not replace validation of the generated program.

## Related

- [Compile-time execution](../spec/comptime.md)
- [Syntax](syntax.md)
- [Proofs](proofs.md)