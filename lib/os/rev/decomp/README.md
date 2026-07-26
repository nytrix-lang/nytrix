# Decompiler modules

`core.ny` owns the public `std.os.rev.decomp` API and high-level ELF analysis,
lifting, dataflow, structuring, pseudocode, and decompilation passes.

Leaf modules have no dependency on `core.ny`:

- `source.ny` normalizes paths, bytes, and loaded analysis records.
- `bytes.ny` provides bounds-checked binary reads and byte-string extraction.
- `elf_types.ny` classifies ELF headers, symbols, and segment permissions.
- `collections.ny` and `cfg_sets.ny` provide the shared set operations used by
  CFG, def-use, and structuring passes.
- `symbols.ny` owns register aliases and token extraction for def-use slicing.
- `tools.ny` contains optional external-tool probing and demangling.

Keep new low-level helpers in a leaf module and import their named symbols into
`core.ny`. A high-level pass must not import `core.ny`; that would form a cycle.
Validate a split with `./make bin`, the focused `decompiler-c-control-flow`
fixture, and a public `use std.os.rev.decomp` probe.
