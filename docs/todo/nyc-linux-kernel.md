<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":7,"summary":"Full Linux-kernel build acceptance for the NYC compiler."} -->
# NYC Linux kernel acceptance

- [ ] Produce a clean Linux 7.3 x86-64 `bzImage` and modules build from
  unmodified sources, compiling every C translation unit with NYC and using
  GNU `as`. Require both artifacts, no C fallback, configured-object coverage,
  and reviewed compiler/linker/`objtool` warnings.

The latest broad build exited 2 without either final artifact. Remaining
blockers span x86 inline assembly (string/exception-table, SHA, SIMD), HDA/PCI
initializers, Zstd/decompression, networking and aggregate typing, and i915
parsing/ABI/assembly. Targeted advances, regression results, and warning
locations are in the [changelog](../CHANGELOG.md); support boundaries are in
[Native code generation](../spec/native.md). Re-run a complete configured-object
inventory before treating this blocker list as exhaustive.
