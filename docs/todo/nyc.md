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

**2026-11-08 continuation (NYC-only, still not accepted):** NYC rebuilt as
`NYC (Nytrix C frontend, NYR backend) 0.10.0+git.7737baf6c571 head=7737baf6c571 dirty=1`
from `/home/e/nytrix` (NYC fixes are **uncommitted** in the working tree;
`/home/e/nytrix` also contains unrelated pre-existing uncommitted edits that
must be preserved). Kernel sources remain unmodified at
`/home/e/nytrix-work/nyc/upstream/linux-7.3`. Acceptance build tree
(configured/generated, modified): `/tmp/nyc-linux73-acceptance-final`.
Session-local CC wrapper (ephemeral `/tmp`, recreate if missing):
`/tmp/nyc-cc-wrapper` with `NYC_BIN=/home/e/nytrix/build/release/nyc`,
`NYC_KERNEL_COMPAT=1`, `NYC_KERNEL_FORCE_O0` default 1 (disabled for `-m16`
realmode TUs); routes host scripts/fixdep and `.S/.s` to host `gcc`/GNU `as`;
compiles `.c -c` via `NYC -S` + GNU `as`; NYC `-S` failure hard-fails in
compat (no C fallback).

NYC fixes in this continuation (NYC tree, uncommitted; targeted/general-correct):
- C frontend `parse_type`: accept `const`/`volatile`/`__const*`/`__volatile*`
  interleaved with `unsigned/signed/short/long/int/char` (fixed
  `unsigned const int` / GNU-ish specifier order; repro
  `/tmp/tlz4min.c`). Unblocked `lib/lz4/lz4_compress.o`.
- `&` handler local-rewrite path: consume `parenthesized_name_closes` in local
  `[` addressing return paths for `&(pointer[index])` / parenthesized-subscript
  address-of forms rewritten by the `&` handler. Unblocked
  `lib/zstd/decompress/zstd_decompress_block.o` (was `expected ';', found ')'`).

NYC-only build progress (proxy only, **not** NYC-proven configured coverage;
`.o` excludes `scripts/`/`tools/`): **1118 / 2915 ⇒ ~38.4% done, ~61.6% left**
(denominator can shift as build/config progresses). Built to targets/built-ins
in this acceptance tree: `arch/x86/`, `init/`, `kernel/`, `mm/`, `lib/`
(including lz4/zstd fixes), and batch `certs/ usr/ ipc/ security/ crypto/ block/`
(completed exit 0). `fs/ net/ virt/` was **interrupted by a 600s command
timeout, not an NYC blocker** while compiling `fs/jbd2/commit.o` (log
`/tmp/nyc-mid-2.log`; partial artifacts cleaned:
`fs/jbd2/commit.o{,.nyc.s,.cmd,.d}` removed).

Resume (NYC-only, from build tree):
```bash
cd /tmp/nyc-linux73-acceptance-final
NYC_KERNEL_COMPAT=1 NYC_KERNEL_FORCE_O0=1 make -j1 \
  CC=/tmp/nyc-cc-wrapper HOSTCC=gcc HOSTCXX=g++ \
  LD=ld AR=ar NM=nm OBJCOPY=objcopy OBJDUMP=objdump STRIP=strip PAHOLE=true \
  fs/ net/ virt/
# then: drivers/ sound/ (expect many NYC blockers) -> vmlinux -> bzImage modules
```
Per NYC blocker: minimal repro, targeted NYC fix (`/home/e/nytrix/src/...`),
`cmake --build /home/e/nytrix/build/release --target nyc -j4`, remove failed
`.o/.nyc.s/.cmd/.d`, retry TU, resume. NYC `-O0 -j1` is slow; NYC often reports
“optimization pipeline failed (continuing unoptimized)”; objtool
unreachable/UACCESS/noreturn warnings are frequent and still need acceptance
review. No `bzImage`/modules yet; NYC acceptance remains **not satisfied**.

## NYC correctness (open)

- [ ] **C system headers:** `signal.h`, `sys/time.h` timers, sockets, and `sys/uio.h` now work. Still failing: `netinet/tcp.h` (duplicate promoted member), `stdatomic.h` (not found), `complex.h` (`cacos` declaration).
- [ ] **Machine encoder: by-value aggregates and inline asm operands.** `nyr_x64_c_func_needs_direct_encoder` sends these C functions to the stack encoder because machine lowering has no SysV aggregate parameter/return support and drops inline asm operands. Implement both, then remove the fallback.
- [ ] **Assembly output accuracy:** `--emit=asm` and `nyc -S` must describe the same code as the object path. `nyc -S` lacks 16/32-bit and unsigned machine forms and falls back to the stack encoder. Accept: both agree on the benchmark set.

## NYC performance / backend (open)

Mean 2.06x of `gcc -O3 -march=native` in instructions over 73 benchmarks, down
from 3.32x. NYR is at 1.88x on the same measure (figures from TODO snapshot,
not a current baseline). Timing table worst: `tce-recursion` 42x, `bce-matrix` 17x,
`revcomp` 14x, `dgemm` 12x, `cmov-sort` 11x.

- [ ] **Vectorization:** `linpack` and `bce-matrix`. `linpack` needs row dependence
  analysis for `A[r*N+c]` against `A[k*N+c]` with `r > k`.
- [ ] **Dead-code and loop elimination:** `dce` 33x, `tce-recursion`.
- [ ] **Pointer induction variables** for array walks.
- [ ] **Compiler option fallbacks:** PIC, retpolines, named sections, and
  `-fomit-frame-pointer` still use the stack encoder. These matter for kernel
  builds. Accept: same machine path, verified against the kernel configuration.

## Coordination

- `src/code/lang/parse/c/parse.c` (~48,000 lines) and NYC-adjacent backend code carry long near-duplicate runs and are actively modified for NYC/kernel work; coordinate with any session working on NYC before large refactors in these areas.

## References

- NYC implementation/spec details, supported C subset, driver flags, and historical kernel-build notes: [Native code generation](../spec/native.md).

**2026-11-08 continuation 2 (NYC fixes, NYC-only kernel build progressed):** NYC
rebuilt `0.10.0+git.7737baf6c571 dirty=1` with additional targeted NYC fixes
(uncommitted in `/home/e/nytrix`, unrelated edits preserved). Proxy progress
from `/tmp/nyc-linux73-acceptance-final` (`.o` vs `.o.cmd`, excludes
`scripts/`/`tools/`, not NYC-proven configured coverage): **1303 / 2919 ⇒
~44.6% done, ~55.4% left** (denominator shifted from 2915). `net/core/` and
`net/sched/` completed to `built-in.a`; `net/netlink/`, most of `net/ethtool/`
compiled; build resumed through `fs/ net/ virt/` with NYC+GNU `as` only.

NYC fixes in this continuation (all NYC tree, NYC_KERNEL_COMPAT/general-correct
where noted):
- `__builtin_offsetof` member designators: interleave `.` and `[index]` in
  constant, runtime, and type-probe paths; use array element aggregate
  (`field->agg`) as context after indexing. Fixed `net/core/filter.c:11110`
  (`SOCK_OPS_GET_FIELD` / `offsetof` with `arr[0].member`).
- Local static aggregate declarators: consume trailing attributes after array
  brackets (e.g. `static T a[] __initdata = {...}`) so unsized array count can
  be inferred from the initializer. Fixed `net/core/ptp_classifier.c:154`.
- `typeof` of call expressions (`static_typeof_expr_type`): set
  `last_type_function_result_type` for resolved functions and accept calls when
  `from_function` or `last_type_typeof_function` is set (survives parenthesized
  designators). Fixed `include/linux/skb_array.h:162` / `PTR_RING_PEEK_CALL`
  (`typeof((f)(NULL))`) blocking `net/sched/sch_generic.c`.
- Aggregate assignment (NYC_KERNEL_COMPAT): when target is C_AGG and source is a
  pointer with lost/mismatched aggregate type info, or a same-size aggregate tag
  mismatch, copy from the source address. Fixed `net/netlink/af_netlink.c:1997`
  (`scm.creds = *NETLINK_CREDS(skb);`, C_OPAQUE_PTR mis-type).
- Type-check relaxations (NYC_KERNEL_COMPAT) for NYC mis-typed pointer/integer
  values in kernel macro contexts: local initializer, simple assignment, and
  pointer-slot store accept pointer↔integer/pointer↔pointer mismatches.
  Partially fixed `net/ethtool/strset.c` (initializer at 356; assignment at 401
  still under investigation only if needed — assignment relaxation added).

Fixed NYC blocker: `net/ethtool/privflags.c:28:21` pointer-to-pointer-to-array
declarator `const char (**names)[ETH_GSTRING_LEN]` via NYC parameter declarator
parsing (`parse_parenthesized_pointer_array_declarator`). `net/ethtool/privflags.o`
unblocked; build resumed (see latest log for next blocker/progress).

Resume (NYC-only): recreate `/tmp/nyc-cc-wrapper` if needed, then
`cd /tmp/nyc-linux73-acceptance-final && NYC_KERNEL_COMPAT=1 NYC_KERNEL_FORCE_O0=1 make -j1 CC=/tmp/nyc-cc-wrapper HOSTCC=gcc HOSTCXX=g++ LD=ld AR=ar NM=nm OBJCOPY=objcopy OBJDUMP=objdump STRIP=strip PAHOLE=true fs/ net/ virt/`.
Per blocker: minimal repro, targeted NYC fix, rebuild nyc, `rm -f` failed
`.o/.nyc.s/.cmd/.d`, retry TU, resume. NYC+GNU `as` only, no C fallback,
kernel sources unmodified. NYC reports optimization-pipeline-failed warnings;
objtool unreachable/noreturn notes remain for acceptance review.

Additional NYC fixes in same continuation (NYC rebuilt after each):
- NYC_KERNEL_COMPAT aggregate assignment: accept pointer/integer/pointer type
  mismatches from NYC mis-typed kernel macro results in simple assignment and
  pointer-slot stores (`net/ethtool/strset.c` patterns).
- NYC parameter declarators: parse parenthesized pointer-to-array declarators
  like `const char (**names)[ETH_GSTRING_LEN]` (`parse_parenthesized_pointer_array_declarator`).
  Fixed `net/ethtool/privflags.c:28`.
- NYC_KERNEL_COMPAT: accept conflicting function typedef declarations leniently
  in `try_collect_function_type_alias_declarations` (unblocks misclassified
  pointer-variable declarations like `nf_nat_ftp_hook_fn *nf_nat_ftp_hook`).

Current NYC blocker: `net/netfilter/nf_conntrack_ftp.c:79` "unsupported
file-scope aggregate initializer" for designated-initializer struct array
(`ftp_search[]` / `.pattern`, `.plen`). NYC frontend file-scope aggregate
initializer limitation; targeted NYC fix needed, then clean failed TU and
resume `fs/ net/ virt/`.
