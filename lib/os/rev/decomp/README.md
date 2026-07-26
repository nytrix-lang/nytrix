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
- `elf.ny` owns ELF loading, sections, symbols, relocations, and recovery.
- `abi.ny` owns register aliases, calling-convention profiles, and stack slots.
- `cfg.ny` builds basic blocks, jump tables, and control-flow graphs.
- `smt_proofs.ny` owns proof-backed expression equivalence checks.
- `syscalls.ny` owns Linux syscall number, argument, and ABI decoding.
- `type_library.ny` owns known C-library call signatures.

Keep new low-level helpers in a leaf module and import their named symbols into
`core.ny`. A high-level pass must not import `core.ny`; that would form a cycle.
Validate a split with `./make bin`, the focused `decompiler-c-control-flow`
fixture, and a public `use std.os.rev.decomp` probe.
