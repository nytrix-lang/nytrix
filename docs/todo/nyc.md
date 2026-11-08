<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":7,"summary":"Full Linux-kernel build acceptance for the NYC compiler."} -->
# NYC Linux kernel acceptance

- [ ] Produce a clean Linux 7.3 x86-64 `bzImage` and modules build from
  unmodified sources, compiling every C translation unit with NYC and using
  GNU `as`. Require both artifacts, no C fallback, configured-object coverage,
  and reviewed compiler/linker/`objtool` warnings.

**2026-11-08 progress (not yet accepted):** Using NYC
`0.10.0+git.227a9dc3dc54` (rebuilt from the working tree) with
`NYC_KERNEL_COMPAT=1` and a Kbuild CC wrapper that routes `.S`/`.s`
translation units to host `gcc`/GNU `as` while compiling `.c` translation
units with NYC, `make ... prepare` completed in the configured out-of-tree
acceptance tree `/tmp/nyc-linux73-acceptance-final` (source
`/home/e/nytrix-work/nyc/upstream/linux-7.3`, unmodified). NYC successfully
compiled kernel C preparation/early objects including
`scripts/mod/empty.c`, `scripts/mod/devicetable-offsets.c`,
`kernel/bounds.c`, `arch/x86/kernel/asm-offsets.c`,
`kernel/sched/rq-offsets.c`, and (after a backend fix)
`init/do_mounts.c`. NYC frontend tests still pass (`ctest -R
^nyr-c-frontend$`, 100%).

NYC fixes made in this pass:
- Driver/Kbuild compatibility: `-dumpversion`/`-dumpmachine` probes, GNU-`as`
  version probing, and `-fms-anonymous-structs` handling; accept common
  GCC/Kbuild `-f*`/`-m*`/`--param`/`-pipe` forms in `NYC_KERNEL_COMPAT` mode
  without swallowing NYC-handled options such as `-include` or semantically
  unsupported `-mcmodel=*` values.
- C frontend flag/layout handling: recognize `-fms-anonymous-structs` /
  `-fno-ms-anonymous-structs` and enable MS anonymous-member parsing. This
  fixes Linux `struct filename` layout/static-assert failures caused by tagged
  anonymous members (`struct __filename_head;`) under kernel flags.
- x86 object writer: accept zero-length globals that carry allocated bytes,
  with a more specific diagnostic for invalid global data.
- Build integration (local wrapper, kernel sources unmodified): route `.c`
  `-c` compilations through NYC `-S` plus GNU `as` when helpful, bypassing NYC
  object-writer inline-asm encoding limits while still compiling the C TU with
  NYC and assembling with GNU `as`; answer Kbuild `cc-version.sh` /
  `as-version.sh` probes. This let NYC compile additional objects such as
  `arch/x86/events/probe.o` and `arch/x86/boot/startup/gdt_idt.o`.

A broad `bzImage modules` attempt in the same tree still exits 2 without
either final artifact. Current blocker classes observed in that run:
NYC/NYIR x86 inline-assembly support for kernel patterns (e.g.
`addq`/`cmpxchgq` with `%gs:` operands, `lgdt`, `rdmsr_safe` slot forms,
segment-load helpers, and other operand-bearing templates rejected by the
x86-64 emitter/object writer), plus remaining "invalid global data" / C
frontend/backend cases in other translation units. Kbuild assembler/`objtool`
probes, configured-object coverage, and compiler/linker/`objtool` warning
review still need a complete rerun after the inline-asm/backend blockers are
addressed.

Frontend fixes already noted before this pass covered cleanup attributes on
scalar `for` initializers (`scoped_guard` / `scoped_class`), early-clobber
`rep stos{b,w,l,q}`, and `__builtin_types_compatible_p` of a named function
against an abstract function-pointer type. The only local edit under
`upstream/linux-7.3`, a spaced `TRACE_INCLUDE_PATH` in `vsyscall_trace.h`,
was restored from the upstream archive. Named classes that still need a fresh
inventory are x86 inline assembly (string/exception-table, SHA, SIMD),
HDA/PCI initializers, Zstd/decompression, networking and aggregate typing, and
i915 parsing/ABI/assembly. Targeted advances, regression results, and warning
locations are in the [changelog](../CHANGELOG.md); support boundaries are in
[Native code generation](../spec/native.md). Re-run a complete configured-object
inventory before treating this blocker list as exhaustive.
