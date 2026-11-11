<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":7,"summary":"Full Linux-kernel build acceptance for the NYC compiler."} -->
# NYC Linux kernel acceptance

- [ ] Produce a clean Linux 7.3 x86-64 `bzImage` and modules build from
  unmodified sources, compiling every C translation unit with NYC and using
  GNU `as`. Require both artifacts, configured-object coverage, no C fallback,
  and reviewed compiler/linker/`objtool` warnings.

**Status (2026-11-11):** Acceptance remains open. The prior audited Kbuild
snapshot recorded 2,932 NYC outputs from 2,974 emitted C targets. Sixteen
subsequent strict retries produced `alps.o`, `r8169_main.o`,
`r8169_phy_config.o`, `8139cp.o`, `8139too.o`, `r8169_leds.o`, USB `quirks.o`,
`e1000_ethtool.o`, `e1000_param.o`, `e1000e/82571.o`, `gen8_ppgtt.o`,
`intel_rc6.o`, `intel_gt_sysfs_pm.o`, `intel_workarounds.o`, `i915_query.o`,
and `intel_sseu.o`; that historical inventory plus those retries gives
2,948/2,974 (99.13%, 26 targets remaining by subtraction), not a current
count. A later full `-j16` rerun compiled many more objects but failed again
on `%gs`-segmented inline assembly and the VDSO32 `__seg_fs` parser error; no
fresh coverage inventory was taken. No `bzImage` or modules exist. Object
counts do not establish configured-object coverage.

The subsequent `-j1` `bzImage modules` continuation exceeded its 3,600-second
command deadline while building `drivers/acpi`; it did not produce a final
acceptance result. It exposed the repeated `%gs` segmented `+m` inline-assembly
blocker across x86 translation units and a VDSO32 parser failure at
`__seg_fs`. A `-j4` continuation was stopped after 13 minutes and restarted as
`-j16` against the same output tree, reusing completed artifacts. The earlier
`-j16` run exited 2 after 4m58s, exposing repeated `%gs` inline-assembly
failures, EFI stub absolute relocations, and aggregate-parameter `va_start`.
The fixed-aggregate `va_start` layout now has runtime coverage and
`intel_dpio_phy.o` compiles successfully (53 functions, 499 data symbols).
After the full run, the VDSO32 target also compiles with FS qualifier parsing
(32 functions, 155 data symbols); its generated assembly contains no FS/GS
qualified memory accesses.
After preserving the output tree and synchronizing its config with
`olddefconfig`, a full `-j16 bzImage modules` rerun exited 2 after reaching
the same `%gs` failures in core objects and a VDSO32 parser error at
`__seg_fs`. No fresh coverage count or kernel artifacts resulted.

Acceptance trees: `/tmp/nyc-linux73-acceptance-final` and the fresh
`/tmp/nyc-linux73-acceptance-final-fresh`; unmodified Linux source:
`/home/e/nytrix-work/nyc/upstream/linux-7.3`. Strict retries use
`NYC_KERNEL_COMPAT=1`, `NYC_KERNEL_FORCE_O0=1`, and
`/tmp/nyc-cc-wrapper-fixed`; `.c` sources go through NYC and GNU `as`, with no
C fallback.

## Open failure families

| Family | Current evidence | Required work |
| --- | --- | --- |
| x86 inline assembly and segment accesses | Strict NYC plus GNU `as` target builds now pass for GS `+m`/`xadd`, REP MOVSL tails (`arch/x86/lib/iomem.o`), user-load exception-table assembly (`mm/maccess.o` and the `alternative.c` target), raw-byte XSTORE (`via-rng.o`), MOVNTDQA (`drm_cache.o`), function-pointer XCHG (`intel_execlists_submission.o`), and PM trace metadata (`drivers/base/power/main.o`). Focused runtime regressions cover REP MOVSL tails, XCHG values, and trace metadata relocation. VDSO32 accepts `__seg_fs`; generated assembly contains no FS/GS-qualified memory instructions. General `__seg_fs`/`__seg_gs`-qualified C memory accesses still lose their address-space qualifier. | Finish a fresh full kernel build, refresh the failure inventory, and preserve address spaces for ordinary C memory operations. |
| SysV aggregate and variadic ABI | `intel_dpio_phy.o` now compiles after `va_start` accounts for fixed aggregate parameters, hidden sret pointers, register-bank rollback, and stack-passed aggregates. Runtime coverage exercises mixed SSE/integer aggregates and stack rollback. | Continue auditing aggregate calls, returns, and variadic boundaries; this isolated target does not prove the whole ABI. |
| C parsing, types, and initializers | The previously listed strict failures in `gen8_ppgtt.c`, `intel_execlists_submission.c`, `intel_gt_sysfs_pm.c`, and `intel_rc6.c` now pass the frontend; `gen8_ppgtt.o`, `intel_gt_sysfs_pm.o`, and `intel_rc6.o` produce objects. VDSO32's `__seg_fs` parsing failure is fixed and its target compiles. Int128 multiply/bitwise operations now compile `lib/crypto/gf128hash.o`, and mixed-width signed int128 comparisons compile `kernel/sched/fair.o`. General FS/GS-qualified C memory operations still need type preservation and segment-aware lowering. | Refresh the full failure inventory; do not count backend/ABI errors as parser failures. |
| EFI stub objects and relocations | Strict NYC `drivers/firmware/efi/libstub/x86-stub.stub.o` now passes `STUBCOPY_RELOC`. Function-local static objects are tracked to their owning function and removed only when that static function is unreachable; objects referenced by reachable, inlined code remain with their relocations intact. The new `static-local-data-dead-function.c` fixture verifies dead addressable data does not leak unresolved relocations, and existing static-local pointer-array coverage still passes. The unsized local wide-string array parses; `x86-stub.o` emits 22 functions and 435 data symbols. | Recheck this target in the next full strict kernel build. |
| C system headers | `stdatomic.h` lookup and scalar relaxed `atomic_store`/`atomic_load` frontend regression pass. `complex.h` still fails at `cacos`, whose prototype uses `double _Complex`; NYC's standalone type model has no complex representation. | Implement complex type and ABI support; do not substitute `double` or an unclassified aggregate. |
| NYIR / `objtool` correctness | Optimizer verification fallback, unreachable instructions, UACCESS-state, and missing-noreturn warnings remain across kernel objects; `intel_dp_aux.o` additionally reports fallback for `srcu_check_read_flavor`. | Classify each warning family and fix unsafe output before acceptance. |
| Assembly output parity | `--emit=asm` and `nyc -S` must match the object path; the `-S` path lacks some 16/32-bit and unsigned machine forms and uses a stack-encoder fallback. Direct `nyc -c` still rejects the new function-pointer xchg template in the object writer (`xchgq %rdx, %P1`, kind `-46`), although the strict kernel `-S` plus GNU `as` route and runtime fixture pass. | Compare both paths over the benchmark set and remove unsupported fallbacks only after parity. |

## Other open compiler work

- [ ] PIC, retpolines, named sections, and `-fomit-frame-pointer` without the
  stack encoder; verify against the kernel configuration.

## Last full strict retry

Last full strict `-j16 bzImage modules` retry (after the VDSO32, aggregate
`va_start`, wide-string array, and indirect-jump fixes) exited 2 after 4m20s.
The earlier int128 multiplication/bitwise and signed-comparison failures are
fixed; strict `lib/crypto/gf128hash.o` and `kernel/sched/fair.o` compile.
GS `xadd` is fixed; strict `fs/buffer.o` and `arch/x86/mm/tlb.o` compile. The
run failed on `arch/x86/lib/iomem.c` (`rep movsl` and tail labels),
`mm/maccess.c` and `arch/x86/kernel/alternative.c` (user-load exception-table
labels), `via-rng.c` (raw-byte xstore), and `drm_cache.c` (`movntdqa` SIMD).
The `alternative.c` ENDBR immediate-and-NOT sequence now has direct lowering
and runtime fixture coverage, but its translation unit still hits the separate
user-load asm. These failures predate the latest inline-asm and EFI fixes; a
fresh full retry is still required. Optimizer fallback and `objtool`
!ENDBR/unreachable warnings remain. No fresh configured-target coverage count
or `bzImage`/modules artifacts were produced. The prior 99.13% figure remains
historical, not current.

The acceptance inventory and warning review must be rerun after the remaining
backend and frontend blockers are addressed. See [Native code generation](../spec/native.md)
for the supported subset, driver behavior, and implementation details.

Fresh-build update (2026-11-11): a clean `-j16 bzImage modules` run using the
fresh output tree above and an `olddefconfig`-normalized copy of the prior
configuration exited 2 after about 8 minutes. NYC compiled many additional
translation units, but `arch/x86/lib/iomem.c` rejected `rep movsl` with
tail-label references and `mm/maccess.c` rejected user-load exception-table
labels. The run produced neither required artifact and did not take a full
configured-target inventory. The previously documented 99.13% remains only a
historical figure.

Follow-up (2026-11-11): faultable `asm goto` outputs now accept writable
memory lvalues, with runtime coverage in `linux-user-load-goto.c`. Added
targeted `rep movsl` plus word/byte tail lowering and runtime coverage for
lengths 0–17, unaligned offsets, copied bytes, and output registers; the
`nyr-c-frontend` test passed. A strict retry produced NYC assembly for both
`arch/x86/lib/iomem.c` (16 functions) and `mm/maccess.c` (24 functions) without
those parser errors, but Kbuild then failed in `fixdep` because
`arch/x86/lib/.iomem.o.d` was absent. The wrapper was adjusted to ask GCC for
Kbuild dependency files, but the isolated retry still failed at `fixdep`.
Therefore neither object completion nor `bzImage`/modules acceptance is proven;
no fresh target inventory was taken. Manually assembling the emitted `.s`
files did produce ELF64 relocatable objects for both translation units, but
that does not make the Kbuild targets complete because their dependency-file
step remains broken.

Latest targeted checks (2026-11-11): XSTORE lowering now compiles
`drivers/char/hw_random/via-rng.o` through NYC and GNU `as` (12 functions,
198 data symbols). MOVNTDQA with aligned/unaligned stores, in one- and
four-vector forms, compiles `drivers/gpu/drm/drm_cache.o` (24 functions,
234 data symbols). The C frontend suite passes with both assembly fixtures.
XSTORE execution was not tested on VIA hardware. A separate SSE4.1 runtime
smoke passed for all four MOVNTDQA forms, including all 16 destination
alignment offsets for unaligned stores and untouched destination guards.

The temporary dependency workaround initially overwrote NYC objects with GCC
objects. That continuation is **not strict NYC acceptance evidence**. The
wrapper now runs GCC first for dependency generation and assembles NYC output
last; a new output tree, `/tmp/nyc-linux73-acceptance-final-xstore`, is being
built with `-j16`. GCC is still used for the dependency workaround and C-to-assembly
Kbuild helper invocations, so the all-C-translation-units criterion requires
further wrapper/driver work and an audited command inventory. No current
configured-target percentage or final artifact acceptance is established.

The mixed-wrapper run was subsequently stopped and superseded by
`/tmp/nyc-cc-strict-native`, using `/tmp/nyc-movntdqa-check/nyc` for all target
C invocations (including `-S` helpers), with GNU `as` for NYC assembly and GCC
only for non-C driver probes and assembly inputs. NYC's existing dependency
writer works directly; no GCC C dependency workaround is needed. A focused
Kbuild retry passed `drm_cache.o` plus `devicetable-offsets.s`, `bounds.s`,
`asm-offsets.s`, and `rq-offsets.s` on this route. A `-j16 bzImage modules`
rebuild is running against the same xstore output tree with the changed
compiler command, which forces affected targets to rebuild.

That NYC-only `-j16` run exited 2 after 569 seconds. The recorded-command
inventory contained 2,375 target C objects with NYC assembly sidecars; this is
not the complete configured-target denominator. After the function-pointer
XCHG fix and strict object rebuild, the next `-j16` run exited 2 after 84
seconds with the same isolated `.tracedata` blocker in
`drivers/base/power/main.c`. Its strict-command inventory now contains 2,669
target C objects with NYC assembly sidecars; this is still not the configured
target denominator. The observed stopping errors
are `drivers/base/power/main.c` (`.tracedata` inline assembly with a line
number and filename relocation) and
`drivers/gpu/drm/i915/gt/intel_execlists_submission.c`
(`xchgq %q0, %1` with a function-pointer register operand; now fixed and
strictly compiled). No `bzImage` was produced; final linking, module
acceptance, and warning review remain open.

The PM trace asm is now represented as ordinary NYIR address generation plus
a local `.tracedata` data entry containing the 16-bit line and an ABS64
relocation to the `__FILE__` string. Its standalone fixture checks the line
record and relocated filename at runtime; the `nyr-c-frontend` test passes.
Strict Kbuild now compiles `drivers/base/power/main.o` (142 functions, 681
data symbols). Starting a fresh output tree is required to validate the full
kernel with the updated compiler rather than reuse prior objects.

The fresh run first exhausted `/tmp` while retaining every generated `.nyc.s`
sidecar; it did not fail on a compiler diagnostic. The strict wrapper now
deletes each NYC assembly sidecar only after GNU `as` successfully emits its
object. The next retry reached `drivers/ptp/ptp_vmclock.c` and exposed
unsigned-int128 compound `>>=` and `+=` handling. Both now lower through the
existing int128 shift/add helpers; runtime coverage checks shifts and
cross-word carry, `nyr-c-frontend` passes, and strict Kbuild compiles
`drivers/ptp/ptp_vmclock.o` (65 functions, 347 data symbols). A clean rebuild
is needed so all targets use the same updated compiler revision.

That clean build then exposed address-taking of a member reached through a
global pointer-to-pointer subscript in `drivers/cpufreq/intel_pstate.c`.
Address-of lowering now recognizes indexed members from any global pointer,
not only arrays of pointers. The existing indexed-pointer regression now
covers both pointer-array and pointer-to-pointer cases; `nyr-c-frontend`
passes, and strict Kbuild compiles `drivers/cpufreq/intel_pstate.o` (201
functions, 695 data symbols). The incremental `bzImage modules` retry is
running; the next final acceptance still requires a clean full build after
all blockers are fixed.

The next blocker was HID's static local callback tables
(`static int (*dispatch_type[])(...)` and `typeof(callback) (* const table[])`).
NYC now lowers these arrays through the static function-pointer relocation path;
the frontend runtime fixture covers both direct function signatures and
`typeof` callback types. `nyr-c-frontend` passes and strict Kbuild compiles
`drivers/hid/hid-core.o` (150 functions, 564 data symbols). NYIR optimization
fallback warnings remain; the incremental full build is the next check.

The incremental link then found duplicate synthetic file-assembly blob symbols
across translation units (`__nyc_asm_blob_0`, etc.). Anonymous blobs now use a
stable hash of the translation-unit path plus a unit-local counter. The
file-scope assembly test now names its probe blob explicitly instead of relying
on an undocumented generated symbol; the complete `nyr-c-frontend` suite
passes. All kernel objects must be rebuilt with the new names before linking
again.

The first clean rerun still used the previous NYC executable; the parser
source had changed but the compiler target had not yet been rebuilt. After
rebuilding NYC, `nyr-c-frontend` passes again. A strict build of
`init/main.o` and `lib/bug.o` produced no duplicate global symbols other than
the expected shared weak float-conversion helpers. The final clean build must
run with this rebuilt compiler executable.

The failure was reproduced in isolated `drivers/base/power/main.o` builds, not
only parallel builds. A standalone pointer-valued `xchgq %q0, %1` case
compiles, assembles, and passes a runtime exchange/old-value check; this does
not reproduce the i915 kernel context. That context exposed a distinct parser
type-check gap for identical function-pointer operands; the frontend now
accepts that exact pointer-type pair. Its regression exercises an exchanged
function pointer, checks both returned and stored values, and passed the
`nyr-c-frontend` test. Strict Kbuild then compiled
`drivers/gpu/drm/i915/gt/intel_execlists_submission.o` through NYC plus GNU
`as` (288 functions, 585 data symbols). The full log also contains repeated NYIR
optimization-pipeline fallback warnings (including
`instrument_atomic_check_alignment`, `srcu_check_read_flavor`, and
static-feature cleanup); these remain acceptance issues, not harmlessly
cleared warnings.

An inventory of the completed prior tree found 2,141 recorded wrapper C-object
commands with both an object and NYC assembly present, and no `bzImage`.
These are encountered targets, not the configured target denominator; the
GCC-overwrite issue above also prevents treating their objects as NYC proof.
