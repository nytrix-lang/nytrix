# TODO

## Roadmap

- [ ] Struct ABI (sret/byval) for x86-64 and i386.
- [ ] Real libc/static/dynamic library linking in the internal ELF64/ELF32 linkers.
- [ ] Multi-level archive and chained-library resolution.
- [ ] Internal C frontend: by-value aggregate params, variadics, C-to-NYIR coverage.
- [ ] Persisted PGO/profile/deopt machinery.
- [ ] Promote non-x86 emitters (aarch64, riscv, arm, mips, powerpc, wasm, bpf, avr) from asm-only toward ELF link/run parity.
- [ ] Proper hot-reloading at the lang level.

## Native Definition

A feature is truly native only when it works in both the NYIR VM and native codegen, without AST fallback or external compiler infrastructure. LLVM and libclang can be useful references or fallbacks, but they are not the compiler’s core.

## Completion Snapshot

| Area                          | Status | Main blockers                                      |
| ----------------------------- | -----: | -------------------------------------------------- |
| NYIR metadata/VM              |    94% | persisted profiles, runtime deopt                  |
| x86-64 native ABI             |    98% | struct ABI (sret/byval)                            |
| x86-64 object/executable      |    96% | real libc/dynamic linking, multi-level archive     |
| Non-x86 emitters              |    51% | ABI/runtime execution coverage                     |
| Internal C frontend           |    55% | by-value aggregates, variadics, C-to-NYIR          |
| Tiering/deopt                 |    30% | persisted PGO, runtime deopt transitions           |
| Overall compiler independence |    89% | struct ABI, full native linker, C import lowering  |
