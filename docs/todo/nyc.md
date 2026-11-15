<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":7,"summary":"Full Linux-kernel build acceptance for the NYC compiler."} -->
# NYC Linux kernel acceptance

## Required acceptance

- [ ] Produce a **clean Linux 7.3 x86-64 `bzImage` and modules** build from
  unmodified sources, compiling every target C translation unit with NYC and
  assembling with GNU `as`. Host tools may use the host compiler. No target-C
  fallback, GCC dependency-generation workaround, stale output reuse, or
  modified kernel sources may count as acceptance.
- [ ] Audit configured target coverage from Kbuild commands, including generated
  C and module metadata. A completed-command count is not a denominator.
- [ ] Review the complete compiler, linker, modpost, and `objtool` diagnostics.
  Fix unsafe output and optimizer verifier fallbacks; do not suppress warnings
  or remove configured protections to obtain artifacts.
- [ ] Preserve the configuration's stack protection, retpolines, return thunks,
  IBT, tracing, and other required code-generation semantics. Compiler capability
  probes must use NYC, including explicit `-x c` and standard-input probes.
- [ ] Preserve and lower general `__seg_fs` / `__seg_gs` C address spaces for
  actual memory accesses, not merely parse their qualifiers.
- [ ] Implement C complex types and their ABI, including system `complex.h`
  declarations. Substituting a scalar or unclassified aggregate is not support.
- [ ] Complete the SysV aggregate/variadic boundary audit: mixed integer/SSE
  classes, register-bank rollback, hidden sret pointers, stack aggregates,
  fixed aggregate arguments to `va_start`, calls, and returns.
- [ ] Establish assembly/direct-object/physical-machine parity over the native
  benchmark set, including unsigned and 16/32-bit forms. Support PIC,
  retpolines, named sections, stack guards, and frame-pointer omission **without
  the stack encoder**, then remove the obsolete fallback.

Remove this file only after these requirements pass. No kernel image, complete
module set, configured-target percentage, or final warning acceptance is proven.

## Current build evidence

Unmodified source: `/home/e/nytrix-work/nyc/upstream/linux-7.3`.

A fresh output tree at `/tmp/nyc-kernel-final-20261113` was configured from the
previous run's `.config` with `olddefconfig`, then built with the current NYC
binary, strict Kbuild wrapper, `-j24`, and `KCFLAGS=-O2`. It recorded 3,006
NYC-compiled target C units and thirteen module-link steps, then linked
`vmlinux.unstripped`. The boot and compressed C units all compiled, but GNU ld
rejected `arch/x86/boot/setup.elf` because setup exceeds the 64-sector / 32 KiB
limit; no `bzImage` was produced. The final log is
`/tmp/nyc-kernel-final-20261113.log`. Its 68 raw objtool warning lines include
UACCESS-state and `.noinstr` escapes. The completed-command count still has no
configured-target denominator, and the warning audit is not complete.

The next i386 emitter update reads arithmetic operands directly from spill
homes and fuses adjacent 32-bit comparisons with their conditional branches.
It preserves materialized booleans with other users, including backward-edge
and PHI users. Frozen before/after compilers under the same saved Kbuild flags
compile all twenty real-mode C objects with identical symbol inventories;
their combined `.text` decreases from 145,273 to 144,301 bytes (972 bytes).
`printf.o` decreases from 19,130 to 19,094 bytes. These isolated object results
do not meet the setup limit or establish a new full kernel acceptance result.
Regression coverage compares 32/64-bit arithmetic, carry/borrow, signed and
unsigned comparisons and conditional branches with GCC under i386 regparm
settings 0 through 3.

The subsequent i386 pass infers bounded sign-extension/mask ranges, reuses EAX
between adjacent operations, coalesces dying copies, specializes constant
shifts and omits stores only for immediate single-use register consumers.
It also fixes three correctness defects exposed by the new regressions:
shifts overwrote neighboring four-byte value homes with an unused high word;
signed 64-bit division/remainder used only the low 32 bits; and mixed signed
64-bit/unsigned 32-bit expressions incorrectly selected unsigned arithmetic.
Shift counts now preserve the promoted left operand's signedness and type.
All twenty real-mode C objects still compile with matching symbol inventories;
their combined `.text` is now 122,788 bytes, down 22,485 bytes from the earlier
145,273-byte baseline. `printf.o` is 15,506 bytes. This remains far above the
setup limit and is not a new kernel acceptance result. The integer oracle
passes i386 O0 through O3 with regparm settings 0 through 3 and covers signed
and unsigned full-width quotients/remainders, narrow promotions, mixed-width
comparisons, constant/variable shifts, overflow, carry and borrow.
Both GCC and Clang host oracles pass those sixteen i386 configurations. The
frozen full C frontend harness passes in `/tmp/nyc-final-arithmetic-proof`,
including x86-64 assembly/direct-object comparisons at O2 and O3.

The latest resumed compatibility build reused completed Kbuild outputs in
`/tmp/nyc-current-diagnostic.r__uopqq` and used the current NYC compiler. The
multi-declarator pointer-qualifier fix allowed the previously failing
`i915_gem_shmem.o` to compile. It linked `vmlinux.unstripped`, linked
thirteen modules, and compiled the real-mode and compressed boot C units,
including `pm.o`, `video.o`, and `compressed/ident_map_64.o`. GNU ld then
rejected `arch/x86/boot/setup.elf`: setup exceeds Linux's 64-sector / 32 KiB
limit, with consequent truncated setup-header relocations. This resumed build
is incremental evidence, not a clean image acceptance run. Its log contains
68 raw objtool warning lines, including UACCESS state and `.noinstr` escapes;
these remain unreviewed and unaccepted. A separate size comparison found NYC's
`printf.o` `.text` at 19,130 bytes versus 1,765 bytes for a GCC reference, so
real-mode code size is a primary current blocker.

A follow-up compile of that same real-mode `printf.c` with identical Kbuild
flags measured 19,170 bytes of NYC `.text` with the normal `-Os` mapping and
19,512 bytes when forcing NYC `-O1`. The current `-Os` to O2 mapping is smaller
for this object; changing the optimization-level mapping does not address the
setup-size blocker.

For that isolated comparison, `prepare` was run afterward with GCC so the
Kconfig-generated headers were available to the manual NYC compile. The
`/tmp/nyc-kernel-final-20261113` output tree is now diagnostic-only and must
not be reused as a strict continuation tree.

The latest completed clean diagnostic used frozen NYC and
`/tmp/nyc-cc-strict-all-c` in
`/tmp/nyc-current-diagnostic.r__uopqq`. It exited 2 after 453.08 seconds,
recording **2,982 completed C commands**, not configured-source coverage.
It linked `vmlinux.unstripped` with all five previously missing helpers
resolved and recorded thirteen module-link commands. The boot and decompressor
stages then failed in ten C translation units: `cpuflags`, `a20`, `string`,
`printf`, `pm`, `cpucheck`, `video`, and compressed `cpuflags`, `string` and
`ident_map_64`. Remaining forms include CPUID constraints, segment-register
operations, real-mode string assembly, i386 varargs, a far call, two-stage REP
copy and a fixed-register memory load. No `bzImage` or audited complete module
set was established.

This snapshot still reports 324 objtool diagnostic lines and 42 modpost
warning lines. A subsequent IBT fix emits ENDBR at address-taken labels,
including addresses taken after a label definition. Runtime checks read the
actual landing bytes at both O2 and O3; a strict BPF interpreter object has
ENDBR at all 256 data-relocation targets. Delayed branch comparisons also use
their materialized boolean unless adjacency-based fusion was proven before
stack-home assignment. The C frontend suite passes in 41.37 seconds. These
changes have not yet been included in a new complete build.

Subsequent CPUID and i386 varargs fixes pass the C frontend suite and real
runtime interoperability, including GCC regparm settings 0 through 3.
`arch/x86/boot/printf.o`, both real-mode and compressed `cpuflags.o`, and
`cpucheck.o` now compile under their actual Kbuild commands. Bit-string
read-modify-write lowering preserves LOCK policy, signed indices across word
boundaries and carry outputs. x87 status writes invalidate local constant facts
and preserve the untouched bytes above their 16-bit output. Both native output
routes and i386 runtime regressions pass; the C frontend suite passes in
42.74 seconds. The real-mode CR0 access has explicit operand lowering. Raw MSR low/high-word
reads and writes pass ring-zero QEMU execution through x86-64 assembly/direct
objects and i386 assembly. These targeted checks do not replace the next clean
image/module build or the warning/protection audit.

Repeated CMPSB now passes forward/backward, cursor/count writeback, zero-count
null-pointer and final-byte-mismatch regressions through both x86-64 output
routes and i386 regparm settings 0 through 3. The C frontend suite passes in
43.30 seconds. This exposed and fixed an i386 local-load high-word write into
a four-byte value home.

The subsequent `divl` operand form and two-stage REP word/byte copies now pass
runtime regressions through assembly/direct objects and all four i386 regparm
settings. The frozen compiler's full C frontend harness passes in
`/tmp/nyc-repeat-copy-tail-proof.36bWr7Mv`. Both actual boot string and compressed
string commands compile in `/tmp/nyc-real-boot-focus.o43ettph`, bringing the
targeted pass count to six of the ten originally failed boot units. The other
four remain blocked; this is still not a fresh full image/module build.
The fixed R9 load now also passes an actual protected-page fault/resume and
register-value regression through both native output routes. The frozen full
frontend harness passes in 69 seconds in `/tmp/nyc-fixed-r9-proof.B4hoqupm`;
`compressed/ident_map_64.o` compiles under its real Kbuild command in
`/tmp/nyc-real-boot-focus.jbug81jn`. Seven of the original ten failed boot units
now pass targeted checks; `a20`, `pm` and `video` remain.
Explicit FS/GS selector and byte/word/dword assembly now passes actual
distinct-base memory reads/writes through both native routes and i386
regparm 0 through 3. The tests use unmapped flat offsets, exposing any extra
ordinary C operand read. The frozen frontend harness passes in 68 seconds in
`/tmp/nyc-segment-memory-proof.Eyw08onR`. The real `a20.o` command passes in
`/tmp/nyc-real-boot-focus.ulbzktmu`: eight of the original ten now pass targeted
checks. `pm` still stops at its far call; `video` at its ES string-store block.
This does not complete general FS/GS-qualified C types or a clean kernel build.
A preceding non-frozen harness run failed at `conditional-expression.c`
without a captured diagnostic; the frozen isolated GDB compile and subsequent
full harness passed. That earlier termination has no established cause.

The preceding clean diagnostic used frozen NYC and
`/tmp/nyc-cc-strict-all-c` in
`/tmp/nyc-current-diagnostic.uz1k0wll`. It exited 2 after 457.62 seconds,
recording **2,925 completed C commands**, not configured-source coverage.
Unlike older wrappers, it routes explicit-language and stdin C probes through
NYC and reports NYC's version. It does not compile target C with GCC.

It linked `vmlinux.o`, reached modpost, compiled module metadata with NYC,
and produced module objects. The final `vmlinux.unstripped` link failed on
five helper symbols: `dax_invalidate_mapping_entry_sync`,
`dax_delete_mapping_entry`, `vma_thp_gfp_mask`, `dm_zone_endio`, and
`dax_get_private`. No image or complete audited module set was established.
The linked-image log contains 324 `objtool` diagnostic lines and 42 modpost
warning lines, including UACCESS state, a VMware modified-frame return,
noinstr escapes, BPF label targets without ENDBR, and section mismatches.
These are raw line counts, not distinct issues or accepted warnings.
Constant-result propagation now preserves effectful calls while exposing
their known return values. Fresh strict truncate/device-mapper objects no
longer reference the two DAX mapping helpers or `dm_zone_endio`. Private
file-scope data now stops rooting callbacks when no function, initializer,
alias or assembly references it; explicit used definitions, named sections,
public data and function-owned statics remain retained. A fresh device-mapper
object also has no `dax_get_private` reference. Closed-world constant scalar
parameter propagation now removes the `vma_thp_gfp_mask` reference from a fresh
strict `swap_state.o`. It preserves the ABI, calls, noinline behavior,
address-taken formals and escaping functions. This exposed and fixed physical
machine aggregate-parameter homing: register aggregates require addressable
local storage, not a scalar register value mistaken for a pointer. Both output
routes pass mixed integer/SSE, register rollback, stack aggregate and
by-value isolation regressions. The C frontend suite passes in 40.36 seconds.
All five helpers were absent in targeted objects; the latest full diagnostic
now verifies their final-link resolution.

The preceding clean diagnostic in `/tmp/nyc-current-diagnostic.vr1ilbdt`
completed 2,928 C commands before declaration-binding lookahead failed in
both 32-bit and
64-bit VDSO `vclock_gettime` objects. The new declaration-aware scanner reused
a prior function alias target when inspecting another declaration; distinct
weak alias targets require fresh declaration-local probe state. No image or
complete module set was produced. Declaration probes now reset alias state;
fresh strict 32-bit and 64-bit VDSO objects compile, both distinct-alias runtime
cases pass, and the C frontend suite passes. The latest diagnostic includes
that fix.

The preceding protected diagnostic in
`/tmp/nyc-protected-diagnostic.3unCuui8` completed 2,956 C commands and
reached the `vmlinux.o` link, which failed on duplicate `pm_power_off`
definitions in the generic and x86 reboot objects. Repeated callback
declarations now merge into one object; weak data has weak ELF binding, and
defining-unit accesses follow strong interposition rather than private aliases.
A newly frozen focused build of both reboot objects passes `ld -r`: the
generic definition is weak and the x86 definition strong. The latest
diagnostic's aggregate link no longer has that duplicate. That older diagnostic
also reports an unreachable instruction
in `dst_metrics_write_ptr` from `net/ipv4/ip_tunnel.o`. Source and disassembly
identify a zero-offset jump following the fatal BUG trap, not an unsafe
reachable path. The native text emitter now omits branches to the immediately
following label. A fresh focused `ip_tunnel.o` compile passes its original
`objtool` command without that warning; complete-build warning review remains
open.

An earlier clean diagnostic in `/tmp/nyc-all-c-diagnostic.UaGFnFJf`
completed 2,922 C commands before repeated port-I/O assembly in `lib/iomap.c`
and GCC rejecting the C-only `-fms-anonymous-structs` option on assembly inputs.
Repeated INS/OUTS now have explicit operand lowering, and strict
`lib/iomap.o` recompilation passes. Both native output routes also pass
ring-zero QEMU execution for byte/word/dword INS/OUTS, including input data,
boundaries, updated pointer/count operands, zero counts, and byte debug-console
output. This is virtualized execution, not a physical port-device test.
The wrapper omits that C-only option only
on its non-C GNU assembly route. A fresh focused Kbuild run in
`/tmp/nyc-realmode-proof.KybGcFDB` produces `realmode.bin` and the enclosing
realmode archive, including NYC's 16-bit C objects. Neither targeted success
establishes the full image/module acceptance.

That earlier snapshot predates real stack protection: its enabled
`-fstack-protector-strong` flag was silently ignored under kernel compatibility.
That snapshot must not count as a protected build. The latest diagnostic
includes real stack protection. Stack protection has
runtime corruption regressions for both output routes: FS/GS numeric and
symbolic guards, default/custom global guards, option ordering, predefines,
function opt-outs, and all-mode scalar functions. The guard word sits above
addressable frame storage and is checked directly at every normal return.
Other kernel-compatible flags still need a truthful capability audit; in
particular patchable function entries and requested asynchronous unwind tables
must not remain silently ignored.

Earlier clean diagnostics in `/tmp/nyc-clean-kernel.wHQhQ50y` and
`/tmp/nyc-strict-acceptance.ll52Dd2k` also failed before acceptance. Historical
forced-O0, mixed-GCC-wrapper, or incremental links cannot establish current
coverage. Previous object-count percentages are not acceptance evidence.

## Verified continuation fixes

- Prefix and exact-name ELF sections, read-only flags, ELF note types, scalar
  and aggregate aliases, static device-table aliases, local file-assembly
  symbols, and signed trampoline relocations have targeted coverage.
- Locally bound and retained helpers can inline without discarding required
  definitions. Pure void-call folding, dominated comparisons, private integer
  stack-field promotion, and discarded nonvolatile lvalue reads remove disabled
  kernel paths without inventing missing definitions.
- Late inline cleanup refreshes effects. Asm-goto targets remain labels during
  use-def analysis and SSA replacement. All eleven affected static-feature
  kernel objects compile without the prior fourteen cleanup warnings.
- Discarded-void comma expressions compile both previously failing DRM objects.
- Global binding collection uses one name index instead of a complete token
  rescan per object; the previously slow wireless trace object completes.
- Repeated ordinary and typedef callback declarations share storage. Both
  native output routes cover weak fallback, strong override, and defining-unit
  reads and writes through the interposed symbol.
- Storage bindings follow actual file-scope declarators rather than identifier
  occurrences in function bodies, prototypes, or initializers. GNU weak
  attributes distinguish declaration-wide prefixes from individual pointer
  and suffix attributes. GCC-oracle and both native output routes cover
  interposition, neighboring declarators, static linkage, mixed pointer/scalar
  initialization, and tentative redeclarations; the C frontend suite passes.
- Resumable UD2 assembly no longer discards its C continuation. Signal-handler
  regressions execute plain traps in both output routes and labelled,
  section-annotated traps through GNU assembly. Direct-object annotation
  support remains open and fails closed rather than dropping metadata.
- Frame-pointer option order is honored: cancelling omission restores the
  single-function route, verified with positive/negative eight-argument calls.
- Single MOVS and narrow/function-pointer XCHG have runtime parity coverage.
  Locked/unlocked CMPXCHG16B preserves old-value fields with or without a
  condition output. Fixed-register asm no longer leaves live values in
  clobbered caller-saved registers.
- PadLock SHA256 has explicit register/operand encoding and strict kernel
  object coverage. Hardware hashing is not verified. Speculative GS-zero and
  unspecified-operand exchange fallbacks are removed and rejected by regressions.
- Existing aggregate/variadic regressions cover fixed aggregate arguments,
  hidden sret pointers, mixed register classes, and stack rollback, but do not
  replace the remaining boundary audit.

## Remaining diagnostic families

General FS/GS C qualifiers still lose address-space semantics. `complex.h`
still fails because the type model has no complex representation. Physical
machine lowering still falls back to stack encoders, including for the new
stack-guard operations. Unsupported kernel-compatible code-generation flags
remain over-advertised. The next full build must use a newly frozen compiler
and fresh output tree after these blockers are addressed.

Historical logs contained unreachable-instruction, missing-`noreturn`,
UACCESS-state, section-mismatch, and other `objtool` diagnostics. Classify the
complete current log and fix each unsafe family; earlier partial warning counts
are not a final review. See [Native code generation](../spec/native.md) for
supported behavior and current implementation limits.

Fresh strict retry (2026-11-14): `/tmp/nyc-recovered-setup-20261114` was
prepared from the prior diagnostic configuration and built with the frozen
NYC candidate `/tmp/nyc-recovered-candidate`, GNU `as`, and
`/tmp/nyc-cc-strict-all-c`. It recorded 3,001 NYC target-C compilations and
twelve module links, then failed at `arch/x86/boot/setup.elf`; no `bzImage`
was produced. A diagnostic link with `--noinhibit-exec` measured the setup
ELF at 50,797 bytes of text and 6,301 bytes of data, with `_end` at `0xe8c0`
against the 32 KiB / 64-sector limit. The largest boot objects are
`video.o` (6,908 bytes of text), `printf.o` (6,169), and `string.o` (4,994).
The strict log contains 64 objtool warning lines and 40 modpost warning lines;
these have not been audited. The raw C-command count is not a configured
target-coverage percentage. The i386 `asm-repeat-cmpsb` regression passes
with the corrected candidate at `-O3` for regparm 0 through 3; the attempted
pointer-width local normalization was reverted after that regression exposed
a failure.

Follow-up i386 size pass (2026-11-14): the high-word analysis no longer forces
an EDX spill for every C call result; downstream wide uses request it when
needed, while 32-bit calls may leave EDX undefined. The C frontend harness
exited 0, and the focused `asm-repeat-cmpsb` O3 runtime checks pass for regparm
0 through 3. Recompiling the real boot `video.c`,
`printf.c`, and `string.c` with their saved Kbuild flags reduced their combined
text by 12 bytes (6 each from `video.o` and `string.o`; `printf.o` unchanged).
This is verified but too small to resolve the setup-size blocker; the change
has not yet been included in a fresh kernel run.

Fresh strict retry (2026-11-14): rebuilt `nyc` from the current checkout and
used the frozen binary `/tmp/nyc-current-candidate-20261114`, strict Kbuild
wrapper `/tmp/nyc-cc-strict-all-c`, and a fresh `olddefconfig` output tree at
`/tmp/nyc-current-kernel-strict-20261114`. With `NYC_KERNEL_COMPAT=1`,
`KCFLAGS=-O2`, GNU `as`, and `-j24`, the run completed 3,004 NYC target-C
commands and thirteen module-link commands, then linked `vmlinux` and the
compressed kernel. It stopped at `arch/x86/boot/setup.elf`: GNU ld again
reported setup exceeds 64 sectors, so no `bzImage` was produced. The raw
command count is not configured-target coverage. The log is
`/tmp/nyc-current-kernel-strict-20261114.log`; it records 64 objtool warning
lines and 40 modpost section-mismatch warnings, neither audited or accepted.
The real-mode `video.o`, `printf.o`, and `string.o` text sizes are 6,902,
6,169, and 4,988 bytes respectively, effectively unchanged from the prior
diagnostic; the earlier 12-byte improvement did not resolve setup size. A
first invocation omitted `NYC_KERNEL_COMPAT=1` and failed on intentionally
lenient block `_Static_assert` handling; that output tree is diagnostic-only
and is not part of this result.

Follow-up i386 register-run pass (2026-11-14): two-instruction straight-line
runs may now use the local register allocator. The C frontend CTest
`nyr-c-frontend` passes (54.88 seconds). Recompiling the real-mode boot
`video.c`, `printf.c`, and `string.c` with their saved Kbuild commands reduced
their combined `.text` by 112 bytes. Linking those updated objects with the
remaining prior-run setup objects using `--noinhibit-exec` measures setup at
50,607 bytes of text, down from 50,797 bytes; `_end` remains `0xe8c0` and GNU
ld still rejects the 64-sector limit. This is a targeted size diagnostic, not
a fresh full-build acceptance result or proof of configured-target coverage.

Single-instruction run follow-up (2026-11-14): allowing one-instruction i386
register runs passes `nyr-c-frontend` (56.09 seconds). On the same three boot
objects it saves another 19 bytes of `.text`; the diagnostic setup text falls
to 50,588 bytes. This is a small incremental gain and does not materially
change the 32 KiB blocker. The x86-64 stack-home and object-encoder paths were
inspected during this pass, but no x86-64 size change is claimed here.

Kernel size follow-up (2026-11-14): NYC now places eligible zero-initialized
ELF globals in `.bss` (with an opt-out flag), ranks private i386 spill homes by
use frequency, and proves bounded high-word values through supported local
copy cycles. Added a zero-initialized-data runtime fixture and extended the
i386 spill arithmetic regression for loop-carried values. Direct ELF and
GNU-assembler runtime checks passed for the BSS fixture; the C frontend suite
passed after the BSS, spill-home, and cycle-range changes. Recompiling all 20
real-mode C objects from the saved Linux Kbuild commands produced 48,334 bytes
of `.text` with the frozen NYC candidate, compared with 11,403 bytes for GCC
on those same commands. A diagnostic setup link with the updated objects
measures 49,222 bytes of text, 1,389 bytes of data, and `_end=0xe8d0`; Linux's
64-sector and 32 KiB setup bounds still fail. `.pecompat` alignment to 4 KiB
also absorbs some text reductions. Repeating the object comparison with NYC
`-Oz` saves only 121 bytes across the 20 objects (48,213 bytes total); it does
not close the compiler code-generation gap. The final frozen frontend harness
reached the Nyr-backed C and AArch64 aggregate ABI checks and exited 0. A fresh
`-j32 bzImage modules` build with this frozen compiler compiled 3,002 target C
commands, linked thirteen modules, and linked both `vmlinux` and the compressed
kernel. It then failed at `setup.elf` with Linux's 64-sector limit. The
diagnostic setup ELF has 49,222 bytes of text, 1,389 bytes of data, 4,914 bytes
of bss, and `_end=0xe8d0`; no `bzImage` was produced. The log is
`/tmp/nyc-kernel-size-final-20261114.log`. This is a new clean-build result,
but does not meet the 32 KiB setup target.

Focused setup-size iteration (2026-11-14): the i386 C lowering now marks
pointer index scaling, pointer addition/subtraction, and constant field
address offsets as 32-bit operations. These flags are enabled only for the
i386 ABI. The C size inliner was widened to inline eligible small functions
with estimated work up to 28 instructions and at most 56 IR instructions;
testing a 40/80 threshold produced no additional setup reduction and was
reverted. The frozen compiler `/tmp/nyc-setup-final-candidate` was then used
to rebuild only the 20 C translation units listed by the saved
`arch/x86/boot/.*.o.cmd` Kbuild commands, assemble those outputs with GNU
`as --32`, and link them with the four setup assembly objects using
`arch/x86/boot/setup.ld`. This avoids repeating the unrelated kernel and module
build. The diagnostic link at `/tmp/nyc-setup-only-inline2/setup.elf` has
47,569 bytes of text, 1,389 bytes of data, and 4,914 bytes of bss; GNU ld
still rejects the 64-sector bound. This is 1,653 bytes less text than the
49,222-byte baseline, but the setup target remains over 32 KiB. The frozen
candidate's C frontend harness reached the integer/control-flow/memory,
double/aggregate reference comparison and AArch64 aggregate ABI parity checks;
the i386 pointer arithmetic runtime harness also exited successfully. A
larger inline threshold did not improve the measured image. Continue optimizing
the large setup routines (notably `vsprintf`, `check_cpu`, command-line
parsing, and video setup) and relink this focused object set after each change.

Pointer-home follow-up: i386 loads of C pointer locals now carry the explicit
32-bit pointer range into the NYIR verifier and stack emitter. The i386 entry
path consequently zero-extends pointer parameters when a later wide consumer
needs their high word, instead of sign-extending them as signed 64-bit values.
Rebuilding and assembling the same 20 C units with this candidate and linking
the same setup assembly objects reduces diagnostic setup text from 47,569 to
47,433 bytes (136 bytes); data, bss, and `_end=0xd8d0` are unchanged. The
`nyr-c-frontend` CTest passes (53.11 seconds), and all 20 focused setup C units
compile and assemble. GNU ld still rejects the 64-sector limit, so this
improvement does not complete the setup image. Two measured experiments were
discarded: widening home promotion to a third register increased setup text
by 93 bytes, and omitting homes for unused parameters changed none of the 20
objects. Continue from the 47,433-byte result.

i386 register-save follow-up: local register-allocation candidates with three
or four registers now save EBX around their runs or in the function prologue,
without reserving ESI/EDI globally. The six-register candidate retains the
preallocated ESI/EDI homes. This produces 47,375 bytes of text in the focused
20-object setup link; the `nyr-c-frontend` CTest passes. A four-register
candidate that also reserved ESI/EDI globally measured 47,882 bytes and was
discarded. A parallel GCC build of the same 20 setup C units links at 14,388
bytes of text, confirming that the remaining gap is concentrated in NYC's
i386 code generation. The diagnostic ELF reports `setup_sects=104` against the
64-sector maximum.

An experimental copy of the x86 setup `string.c` marked its division helpers
`__always_inline`; this removed the two out-of-line helpers and measured 47,228
bytes of text. That source change was reverted because acceptance requires an
unmodified Linux tree. The valid focused result remains 47,375 bytes of text,
1,392 bytes of data, and 4,914 bytes of bss; GNU ld rejects the 64-sector
limit. Continue reducing NYC-generated code against the unmodified source.

Inlining audit: the generic C size-inliner rejects `NYIR_ALLOCA` helpers
because cloned automatic storage changes stack-slot lifetimes. A temporary
experiment allowed fixed allocas in small `static inline` helpers and made the
inliner clone them. It removed the two string division helpers but grew other
setup objects; the full focused link increased to 47,908 bytes. The compiler
change was reverted. Any future fix needs an escape/lifetime proof and a
whole-setup size comparison, rather than broadening this gate.

Fallthrough register carry experiment: keeping i386 low-word values live over
one conditional branch and its fallthrough block passed the C frontend test,
but raised the focused setup link to 47,963 bytes. `vsprintf` grew by 342
bytes, so the allocator change was reverted. The next spill-reduction work
needs to account for encoded object size per function before choosing a
control-flow allocation candidate.

Optimization-level and size-pass check (2026-11-14): compiling the same 20
setup C translation units with `-O1` instead of the kernel's `-Os` produced
48,252 bytes of text (877 bytes larger). Recompiling the four largest C
objects (`printf`, `video`, `cpucheck`, and `cmdline`) at plain `-O3` while
holding all other setup objects fixed produced 49,076 bytes (1,701 bytes
larger). Disabling `block_layout` and `inline_small` together across all 20
objects left the linked text unchanged at 47,375 bytes. These are rejected
size candidates; the stable result remains over Linux's 32 KiB/64-sector
limit. The biggest measured gaps versus GCC remain `vsprintf` (+2,415 bytes),
`number` (+1,354), `__cmdline_find_option` (+1,530), and `check_cpu` (+1,439);
further work should target i386 lowering or encoding in those functions.

i386 local-home width reduction (2026-11-14): source-local homes now use four
bytes when backward high-word demand proves no consumer can read their upper
32 bits; homes whose address escapes or whose wide value is observed retain
eight bytes. Parameter homing skips an unused upper-word store so a narrowed
home cannot overwrite the next local. The same 20 setup C units compile and
link at 47,299 bytes of text, 76 bytes below the 47,375-byte baseline. Spill
cleanup now uses the compact local offsets and the correct per-candidate
register-home displacement when protecting address-taken locals. The
`nyr-c-frontend` CTest passes (56.27 seconds), and
`code/targets/i386/portable-runtime.nshape` passes. The link still fails the
64-sector assertion; this is a verified reduction, not kernel completion.
Disabling spill cleanup grew the four largest setup objects by 2,996 bytes
combined; disabling its ESI/EDI home coloring grew them by 559 bytes.

Typed i386 32-bit arithmetic (2026-11-14): C `int` and `unsigned int`
arithmetic now carries the narrow-result fact into NYIR, allowing direct
32-bit add, subtract, multiply, bitwise, division, and remainder lowering;
signed `int` shifts also use direct 32-bit shift instructions. The focused
20-object setup link is now 46,795 bytes of text, 580 bytes below the 47,375
byte baseline. Signed shifts reduced the preceding 46,811-byte build by a
further 16 bytes; adding typed division and remainder did not change size.
`nyr-c-frontend` passes (54.70 seconds), as does the cold i386 portable
runtime test. The setup linker still rejects the image at its 64-sector limit,
so the kernel does not yet build.

i386 divmod pairing (2026-11-14): the stack emitter now recognizes matching
unsigned quotient and remainder operations within a basic block when both
inputs are proven to fit unsigned 32 bits, including equivalent duplicated
pure operand expressions. It emits one `divl` and homes both results. Spill
liveness starts both fused outputs at the first division; a freestanding i386
runtime probe caught and fixed an initial overlapping-home error. The 32-bit
probe passes three quotient/remainder cases, and a wide unsigned 64-bit probe
passes without taking the narrow fusion path. Linux setup's `number` routine
now contains one division instruction where NYC previously emitted two;
`printf.o` shrank by 20 bytes. After the liveness correction the focused setup
link is 46,762 bytes of text, 613 bytes below the 47,375-byte baseline. The
frontend CTest and cold i386 portable runtime pass. A reversed spill-home
order reduced `printf.o` by 96 bytes but grew the whole setup link by 413
bytes, so that layout experiment was reverted. The setup remains above the
32 KiB link limit.

i386 fixed-allocation alignment (2026-11-14): setup's saved Kbuild commands
request a 4-byte preferred stack boundary, but fixed C stack allocations were
rounded and realigned to 16 bytes. The i386 emitter now rounds those fixed
allocations to the requested frame boundary and omits the redundant 16-byte
stack realignment when the ABI requires only 4-byte alignment. The same 20
setup C units compile and assemble, and their focused link drops from 46,762
to 46,635 bytes of text (127 bytes). `nyr-c-frontend` passes (55.12 seconds),
and the cold i386 portable-runtime fixture passes. GNU ld still rejects the
setup's 64-sector/32-KiB limit; this is a verified size reduction, not kernel
completion. A forced 2-register allocator candidate was measured and rejected:
it increased setup text to 49,401 bytes.

i386 signed memory-load fusion (2026-11-14): the register-run emitter now
recognizes a signed byte or halfword load followed immediately by the canonical
shift-left/arithmetic-shift-right sign extension, and emits one `movsbl` or
`movswl` directly from memory. The matcher checks value bounds and requires a
single-use load, preserving one memory access and avoiding unsigned-load
changes. Rebuilding and assembling the same 20 setup C units and linking the
same four setup assembly objects reduces text from 46,635 to 46,483 bytes
(152 bytes); `printf.o` shrinks by 80 bytes and `string.o` by 50 bytes. The
`nyr-c-frontend` CTest passes (55.48 seconds), and the cold i386
`portable-runtime.nshape` fixture passes. GNU ld still rejects the setup's
64-sector/32-KiB limit; this is a verified size reduction, not kernel
completion.

i386 size inlining follow-up (2026-11-14): the post-optimization size inliner
now considers small static helpers with calls when there is exactly one call
site in the translation unit; it continues to reject `alloca` and variadic
helpers. This lets the existing general inliner clone a one-use helper without
expanding repeated hot helpers. The same focused setup compile/link is now
46,360 bytes of text, 123 bytes below the 46,483-byte signed-load result. The
`nyr-c-frontend` CTest passes (54.89 seconds), and the cold i386
`portable-runtime.nshape` fixture passes. Broadly inlining multiple-call-site
helpers increased setup text to 47,775 bytes; a stricter 16/32 body limit
measured 46,718 bytes, so both candidates were rejected at that point. Setup
still exceeds Linux's 32 KiB/64-sector limit and does not link.

i386 zero rematerialization (2026-11-14): register-run constant loads now use
`xorl reg, reg` for a zero low word instead of a full immediate `movl`. The
i386 emitter materializes compare/branch flags after loading their operands,
so this does not replace flags that the IR still needs. The focused setup
link drops from 46,360 to 46,149 bytes of text (211 bytes). The
`nyr-c-frontend` CTest passes (53.80 seconds), and the cold i386 portable
runtime fixture passes. GNU ld still reports setup exceeds 64 sectors; the
setup-size gate remains open.

i386 one-call-site size-inline threshold (2026-11-14): widening the small
helper budget from 28 operations / 56 IR instructions to 40 / 80 while keeping
the single-call-site restriction reduces the focused setup from 46,092 to
46,043 bytes of text (49 bytes). The `nyr-c-frontend` CTest passes (54.13
seconds), and the cold i386 portable-runtime fixture passes. This is the
first validated threshold result; GNU ld still rejects the 32 KiB/64-sector
limit.

i386 one-call-site size-inline sweep (2026-11-14): widening that limit again
to 52 operations / 104 IR instructions reduces focused setup text by another
206 bytes, from 46,043 to 45,837 bytes. `string.o` shrinks by 151 bytes and
`early_serial_console.o` by 55 bytes. The `nyr-c-frontend` CTest passes (58.46
seconds), as does the cold i386 portable-runtime fixture. GNU ld still rejects
the setup image at 64 sectors.

i386 one-call-site size-inline sweep (continued, 2026-11-14): a 64/128
operation/instruction limit reduces setup text by another 21 bytes to 45,816;
the entire gain is from `video-mode.o`. The `nyr-c-frontend` CTest passes
(59.64 seconds), and the cold i386 portable-runtime fixture passes. The
setup link remains over 64 sectors.

i386 one-call-site size-inline sweep (continued, 2026-11-14): an 80/160
operation/instruction limit reduces setup text by another 121 bytes to
45,695. `string.o` shrinks by 92 bytes and `video.o` by 29 bytes. The
`nyr-c-frontend` CTest passes (57.74 seconds), as does the cold i386
portable-runtime fixture. A 96/192 limit produced identical output, so 80/160
remains selected. Setup still exceeds the 32 KiB limit.

i386 CFG spill-home coloring (2026-11-14): the i386 value-home allocator now
computes block live-in/live-out sets and builds value interference before
assigning stack homes. This reuses slots across mutually exclusive control
flow while keeping simultaneous operands separate; functions with multiple
definitions or inline assembly use the existing conservative allocator. The
same focused setup link drops from 46,149 to 46,092 bytes of text (57 bytes);
the largest object reductions are `cmdline.o` (-89 bytes) and `string.o`
(-59 bytes). `nyr-c-frontend` passes (54.51 seconds) with the new allocator as
the default, and the cold i386 portable-runtime fixture passes. The linked
setup still exceeds the 32 KiB/64-sector limit.

i386 small-constant rematerialization (2026-11-14): register-run loads of
zero, one, and minus one now use `xor` followed by zero, increment, or
decrement as appropriate, instead of full 32-bit immediate moves. The
emitter materializes compare/branch flags after operand loads. The focused
setup link shrinks by 28 bytes, from 45,695 to 45,667. The `nyr-c-frontend`
CTest passes (56.10 seconds), and the cold i386 portable-runtime fixture
passes. GNU ld still rejects setup at the 32 KiB/64-sector boundary.
Widening the budget to 96/192, 128/256, and 256/512 produced no change;
allowing two call sites grew setup text to 47,467 bytes, so the 80/160
single-call-site policy remains selected.

i386 setup optimization-level sweep (2026-11-14): replacing Linux's `-Os`
with NYC `-Oz` produces identical setup text (45,667 bytes). Uniform `-O1`
grows it to 46,800 bytes, and `-O0` grows it to 55,315 bytes. These modes do
not close the 12,899-byte gap to the 32 KiB linker limit; further reductions
need to target the largest setup functions and objects. Applying `-O1` only
to `video`, `printf`, `string`, `video-vga`, and `video-mode` yields 45,533
bytes; `-O3` on those same files grows setup to 49,842 bytes.

i386 GCC setup baseline (2026-11-14): compiling the same configured setup C
translation units with GCC and linking the same four upstream assembly
objects produces 14,388 bytes of text, 208 bytes of data, and 4,960 bytes of
BSS. NYC produces 45,667 bytes of text with the current stable changes. The
largest text gaps are `video.o` (+4,577 bytes), `printf.o` (+4,105),
`string.o` (+3,119), `video-vga.o` (+2,301), and `video-mode.o` (+2,142), so
the 32 KiB failure is primarily code generation quality rather than one
outlier translation unit.

i386 zero-compare lowering (2026-11-14): emit `testl reg, reg` instead of
`cmpl $0, reg` for 32-bit comparisons against zero in both the register-run
and stack paths. The same focused setup link drops 60 bytes to 45,607. The
cold i386 portable-runtime fixture passes, and `nyr-c-frontend` passes
(53.36 seconds). Setup still fails the Linux 32 KiB linker cap.
A follow-up attempt to zero stack words with `andl $0, slot` instead of
`movl $0, slot` grew setup to 46,031 bytes under GNU `as` and was discarded.

i386 unit-step arithmetic (2026-11-14): narrow 32-bit additions and
subtractions by `1` or `-1` now use `inc`/`dec` in both the stack and
register-run emitters. Condition flags are not carried across these NYIR
operations; fused comparisons and branches emit their flags at the consumer.
The focused setup link drops another 192 bytes to 45,415. The cold i386
portable-runtime fixture passes, and `nyr-c-frontend` passes (52.56 seconds).
Setup remains above the 32 KiB linker limit.

i386 setup address-size reduction (2026-11-14): Linux setup C builds define
`_SETUP` and keep their stack in the low 64 KiB. Only this `-m16 -D_SETUP`
mode rewrites generated frame-slot references from 32-bit `(%ebp)` to 16-bit
`(%bp)`, avoiding an address-size prefix on each reference. Ordinary `-m16`
output retains full EBP addressing, and raw inline assembly remains verbatim.
The focused setup link measures 41,715 bytes of text, a 3,700-byte reduction
from 45,415. The emitter's candidate cost model now accounts for the removed
prefixes and saves a further 46 bytes versus the first BP rewrite. A QEMU
real-mode probe ran the generated C local-array write/read and returned its
expected result; a separate inline-assembly probe confirms an explicit
`(%ebp)` is preserved. The cold i386 portable-runtime fixture passes and
`nyr-c-frontend` passes (56.94 seconds). The original Linux setup linker still
rejects the image at 64 sectors; text exceeds the 32 KiB limit by 8,947 bytes.

i386 one-use size inlining follow-up (2026-11-14): permit fixed-size stack
objects and internal backward branches for one-call-site helpers, with a
112-operation / 224-instruction cap. A 96/192 cap grows the image by 104 bytes;
112/224 produces byte-identical objects to 128/256. Compared with the setup
address-size result above, the focused setup text falls another 122 bytes to
41,593 bytes.
The five largest objects were also swept across register-run variants; the
automatic per-function choice remains smaller than any forced whole-object
variant. The cold portable-runtime fixture and real-mode QEMU probe pass. The
`nyr-c-frontend` CTest passed with the same generated setup objects under the
larger experimental inlining cap. The Linux setup link still fails its
64-sector assertion, leaving 8,825 bytes to the 32 KiB text target. A
multi-call-site tiny-helper trial enlarged setup by 493 bytes and was dropped.

i386 setup displacement-size model (2026-11-14): the candidate cost model now
counts setup BP frame displacements as one byte when signed 8-bit, otherwise
two bytes, and omits the address-size prefix. The focused setup text drops
another 20 bytes to 41,573. The cold i386 runtime fixture, real-mode QEMU
probe, and `nyr-c-frontend` pass. The original Linux setup linker still fails
the 64-sector assertion, leaving 8,805 bytes to the 32 KiB target.

Follow-up setup sweep: direct EAX forwarding into a single-argument `regparm`
call reduced the setup text by one byte (41,572) and passed the C frontend
suite. Extending that forwarding across a register-run boundary saved another
six bytes, but caused the `i386-aggregate-abi` `regparm=1` frontend case to
segfault; the register-run change was reverted. A whole-setup `-O1` sweep grew
the text to 43,147 bytes. The current measured candidate remains 41,572 bytes,
8,804 bytes above the 32 KiB target, and the original linker cap still fails.
Raising the one-use inliner caps from 112/224 to 256/512 produced byte-identical
setup output, so the smaller caps remain in place.
An in-place memory-immediate update trial allowed a dying low-word operand to
share its destination spill home and emitted direct memory arithmetic. Across
the focused setup objects this grew text by 140 bytes to 41,712, so that change
was reverted. The CFG coloring experiment for multiply-defined values also
produced byte-identical setup output and was reverted.

i386 stack-path constant rematerialization (2026-11-14): ordinary stack loads
of constant 0 now use `xor reg,reg`, and constants 1 and -1 use XOR followed
by INC or DEC. The focused setup link drops 956 bytes, from 41,572 to 40,616
bytes of text, leaving 7,848 bytes above the 32 KiB target. The frontend CTest
and cold i386 portable-runtime fixture pass. A real-mode QEMU local-array
probe reaches its success marker (`12P`); the full Linux setup link still
exceeds the configured sector cap.

i386 conditional-branch register carry (2026-11-14): register runs now emit
conditional branches in place, home live-out values for the taken edge, and
keep the register mapping on the linear fallthrough path. This saves another
217 bytes across the setup objects, for 40,399 bytes of focused setup text and
a 7,631-byte gap to 32 KiB. The frontend CTest, cold i386 portable-runtime
fixture, and real-mode local-array success-marker probe pass. The original
Linux setup linker still reports that setup exceeds 64 sectors.

i386 signed-byte constant rematerialization (2026-11-14): constants in the
signed 8-bit range other than 0 and +/-1 now use `pushl`/`popl` to materialize
the 32-bit value in a register. Constants 128..255 also use zero plus an
8-bit register move when the chosen register has a byte form. Together these
changes drop focused setup text 73 bytes to 40,326. The frontend CTest and
cold i386 portable-runtime fixture pass. The setup remains 7,558 bytes above
the 32 KiB text target; the original linker cap still fails.

i386 instruction-cost and stack-immediate follow-up (2026-11-14): candidate
selection now accounts for local short-branch relaxation and charges operand-
and address-size prefixes only in the 16-bit setup mode. The stack emitter
uses compact zero, signed-byte, and byte-register materialization for small
constants. The focused link is now 40,320 bytes of text and 1,392 bytes of
data. In the actual Linux link layout, `.text32` ends at `0x9d77`, `.pecompat`
aligns to `0xa000`, and `setup_size` rounds to `0xb000` (88 sectors); GNU ld
still rejects the 64-sector cap. Because `.pecompat` has 4 KiB alignment and
the later data/signature occupy about 1.4 KiB, the next acceptance threshold
is to bring `.text32` to at most `0x7000`, about 11,639 bytes below its current
end. The frontend CTest passes after these emitter-selection changes.

Same-command reference (2026-11-14): rebuilding the setup C objects with GCC
from the saved Linux Kbuild command lines and linking them with the original
`setup.ld` succeeds at `setup_size = 0x5000` (40 sectors). NYC's focused
`-Os` objects currently link at `0xb000` (88 sectors). The setup text totals
12,000 bytes with GCC versus 39,432 bytes in the NYC linked `.text` section.
The NYC source commands already use `-Os`; forced `-O1`, `-O2`, and `-O3`
experiments linked at `0xc000`, `0xe000`, and `0xd000`, respectively. Allowing
general `-Os` inlining or limiting it to callees of at most 8 or 24 NYIR
instructions did not reduce the linked setup size, so those experiments were
reverted. This confirms the remaining gap is in the i386 stack-based code
generation and its value/register allocation, rather than a missing size flag
or the linker layout.

i386 leaf-frame cleanup (2026-11-14): the stack emitter now removes the frame
setup, allocation, and `leave` from leaf functions whose emitted body has no
stack references or explicit stack manipulation. The focused linked `.text`
falls from 39,432 to 39,311 bytes, and `.text32` ends at `0x9ce0`; linker
`setup_size` remains `0xb000` because the aligned `.pecompat` section still
starts at `0xa000`. The cold i386 portable-runtime fixture and
`nyr-c-frontend` CTest pass. This is a small code-size improvement; it does
not yet meet the 64-sector setup limit. An initial third-register spill-home
trial reduced the linked text to 39,185 bytes, but `i386-spill-arithmetic`
failed under `-O1 -mregparm=0`; that implementation was reverted. A later,
narrower saved-EBX variant is recorded below.

i386 spill-home promotion sweep (2026-11-14): the two-register colorer now
considers private homes accessed at least twice instead of requiring four
accesses. It reduces the focused linked `.text` to 38,824 bytes; `.text32`
ends at `0x9b17`, while alignment still leaves `setup_size = 0xb000` (88
sectors), and the original `setup.ld` still rejects the image. The complete
`nyr-c-frontend` test and cold i386 portable-runtime fixture pass, including
the `i386-spill-arithmetic` regparm variants. A zero-access threshold produced
byte-identical setup output and was reverted. The remaining linker-layout gap
to `.text32 <= 0x7000` is 11,031 bytes.

i386 saved-EBX home coloring (2026-11-14): use EBX as a third colored home
register only when the selected variant already saves EBX and the function
body leaves it unused. The two additional saved registers retain their full
16-byte stack adjustment, avoiding the alignment bug in the broader trial.
Degree-squared selection of homes for the saved registers cuts the focused
linked `.text` to 38,430 bytes, 881 bytes below the leaf-frame version.
`.text32` ends at `0x998d`; `setup_size` remains `0xb000` (88 sectors), and
original `setup.ld` still rejects the image. `nyr-c-frontend` passes, including
the regparm=0 spill-arithmetic regression, and the cold i386 portable-runtime
fixture passes. The original broad three-register experiment remains reverted.
The remaining gap to `.text32 <= 0x7000` is 10,637 bytes.
An EBX-only coloring follow-up for functions already using ESI/EDI generated
byte-identical setup output, so that allowance was reverted. A whole-setup
forced-variant sweep also confirmed that the automatic per-function selector
is smaller (38,430 bytes of `.text`) than every forced candidate; the forced
outputs ranged from 38,762 to 44,835 bytes.

i386 byte-compare folding (2026-11-14): when a widened byte load is dead after
its comparison, final cleanup compares the memory byte directly. Signed-byte
loads retain signed conditions; zero-extended byte comparisons with constants
in 0..255 convert signed branches to equivalent unsigned branches. The
focused setup `.text` drops from 38,430 to 38,386 bytes; `.text32` ends at
`0x9961`, and `setup_size` remains `0xb000` (88 sectors). The original
`setup.ld` still rejects the image, with 10,593 bytes between the end of
`.text32` and the `0x7000` placement threshold. The cold i386 portable-runtime
fixture and `nyr-c-frontend` CTest pass.

i386 memory-form cleanup (2026-11-14): fold adjacent stack-home
load/update/store triplets into direct memory updates and fold dead stack-home
loads into `cmpl` operands while preserving operand order. The focused setup
link falls from 38,386 to 38,307 bytes of `.text`; `.text32` ends at `0x9912`,
leaving 10,514 bytes to the `0x7000` placement threshold. The original Linux
linker still rejects the setup at 88 sectors. The cold i386 portable-runtime
fixture passes. `nyr-c-frontend` timed out at its configured 120-second limit
while a separate 54-job sanitizer run was active, so that run is not counted as
a pass. A caller-saved spill-home coloring experiment grew `.text` to 39,181
bytes and was reverted.
