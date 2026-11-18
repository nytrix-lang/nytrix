<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":7,"summary":"Full Linux-kernel build acceptance for the NYC compiler."} -->
# NYC Linux kernel acceptance

## Required acceptance

- [ ] Produce a **clean Linux 7.3 x86-64 `bzImage` and modules** build from
  unmodified sources, compiling every target C translation unit with NYC and
  assembling with GNU `as`. Host tools may use the host compiler. No target-C
  fallback, GCC dependency-generation workaround, stale output reuse, or
  modified kernel sources may count as acceptance.
- [ ] Review the complete compiler, linker, modpost, and `objtool` diagnostics.
  Fix unsafe output and optimizer verifier fallbacks; do not suppress warnings
  or remove configured protections to obtain artifacts.
- [ ] Establish assembly/direct-object/physical-machine parity over the native
  benchmark set, including unsigned and 16/32-bit forms. Support PIC,
  retpolines, named sections, stack guards, and frame-pointer omission **without
  the stack encoder**, then remove the obsolete fallback.

Remove this file only after these requirements pass. No kernel image, complete
module set, configured-target percentage, or final warning acceptance is proven.

## Current build evidence

The SysV aggregate/variadic boundary audit is complete and removed from the
checklist: `sysv-boundary` matches GCC output, and `abi-cross-*` links NYC- and
host-compiled counterparts in both directions for INTEGER/SSE, SSE/SSE, and
MEMORY-class returns. That cross-check fixed acall-result capture that read `xmm1` for a mixed INTEGER/SSE return instead of
`xmm0`.Code-generation semantics are verified with NYC itself in both `-c` and `-S`:
`-fstack-protector`, `-fstack-protector-strong`, and `-fstack-protector-all`
emit canaries (and nothing is emitted without the flag);
`-mindirect-branch=thunk-extern` emits `__x86_indirect_thunk_*` and
`thunk-inline` emits its inline speculation trap; `-mfunction-return=
thunk-extern` emits `__x86_return_thunk`; `-fcf-protection=branch` emits
`endbr64`; and `-pg -mfentry` emits `__fentry__` while
`__attribute__((no_instrument_function))` suppresses it. The object-mode
return-thunk and IBT paths are pinned by runtime tests (a stub-thunk execution
and a landing-pad byte check at the compiled entry). Compiler capability probes
use NYC with `-x c` and standard input: file, stdin, `-x c` stdin, and
`-E -x c -` all succeed.

Object-mode `-fomit-frame-pointer` removes the rbp frame for leaf, call, loop,
array, and `volatile` local shapes, pinned by the `omit-fp` runtime check;
volatile locals now occupy ordinary slots with volatile accesses instead of an
alloca, so they no longer force a frame. The assembly route now uses
GOT-relative data addressing under `-fPIC` (`movq sym@GOTPCREL(%rip)`) and
keeps direct `(%rip)` addressing without it, matching the object route.
The assembly route already emits rsp-relative slots and no rbp frame for
leaf functions; the remaining gap is non-leaf omission, where calls and
callee-saved pushes need rsp-delta tracking before the rbp base can go.
That is the open parity item.

Assembly/direct-object parity for unsigned and 16/32-bit shapes is pinned by
`width-parity`: unsigned divide/remainder, logical shift, 64-bit unsigned
division, 16-bit truncating add/multiply/divide, and sign-versus-zero
extension produce the same `1431655765 0 1 -32768 0 250`,
`1844674407370955161 5 9362 65533 1`, `5 4 -4096` output on both routes, and
both routes preserve `__attribute__((section("mydata")))` and `.mytext`.
The object route defaults to the frame encoder first and the machine encoder
second, so a silent `NYC_TRACE_MACHINE_FALLBACK` there does **not** prove that
the register-allocating encoder ran. Forcing it with `NYC_USE_MACH_OBJECT=1` on
the mixed floating-point/aggregate/array/control-flow program produces identical
output with no fallback marker, and that is now pinned by the `mixed-shapes`test. The assembly-route machine text emitter now encodes the C shapes it
previously refused: width-aware loads and stores (`movzwl`, `movslq`, `movl`,
`movsbq`, `movswq`, plus `movw`/`movl`/`movb` stores), `sext32` as `movslq`,
unsigned `divq`, and `SHR`. `asm-machine-reject` now requires these shapes to
emit **no** machine fallback and to return the right answer.

Two latent defects surfaced as soon as C code reached this emitter and are
fixed: the `-S` machine block never set `mach.c_abi`, so functions lowered as
NY-language code, and the LEA+STORE fusion selected `movb` from `byte_width`
alone, truncating every 16/32-bit store. The former also explains why isolated
array programs could pass by accident: only the low byte was stored, and clean
stack bytes hid the corruption until the mixed sweep ran with a dirty stack.
Both routes now pass the 120-program integer-width sweep and the 80-program
floating-point/aggregate/control-flow sweep against GCC, and the full frontend
suite is green with the machine path active.

`-S -fPIC` now reaches the machine emitter as well: symbol addresses use
`movq sym@GOTPCREL(%rip), %rax`, and the specialized leaf emitters are bypassed
under PIC because they address symbols directly. `pic-asm` pins GOTPCREL
presence with `-fPIC` and its absence without, and the PIE link still runs.

## Linux 7.3 kernel build attempt (2026-11-18)

Downloaded `linux-7.3.5.tar.xz`, ran `make x86_64_defconfig`, then
`make -j96 -k CC=build/release/nyc HOSTCC=gcc bzImage`. The build produced 204
objects and stopped with 561 `error:` lines: 316
`objtool: can't decode instruction at .text:...`, the remaining objtool
start-instruction and symbol failures that follow from undecodable text, and 2
direct compiler failures for the inline-asm template
`lock btsq %rdx, (%rax)` in `drivers/hid/usbhid/hid-core.c`. No `bzImage` or
modules were produced. The decode failures are unsafe output, not a
warning-policy problem, so before the compiler, linker, modpost, and objtool
diagnostics can be reviewed end to end, the object encoder has to stop emitting
instruction bytes objtool cannot decode and the remaining inline-asm templates
need support.

Three fallbacks remain before the NYIR assembly emitter can be deleted:
stack-guarded functions (machine lowering rejects `NYIR_STACK_GUARD_LOAD` and
`NYIR_STACK_GUARD_STORE`), retpoline options (the driver skips the machine
block for `-mindirect-branch=*` rather than reporting a fallback, so those
functions still use the NYIR emitter), and non-leaf frame omission (the machine
prologue always establishes `%rbp`). Stack guards and retpolines are the
kernel-relevant ones. Compiling all 523 corpus C
sources in `src/code/test/lang/c` with the trace enabled produced 396
successful objects and **zero** `NYC object fallback` events; the 127
non-compiling entries are host-side or fixture files that need their owning
test's include/link context. The `width-parity` test now fails if its shapes
ever emit that fallback marker.

Fallback inventory (audited): `backend/mach/stack.c` is the shared
stack-machine text emitter and the **only** NYIR route for ARM, AArch64, i386,
and RISC-V, because `emit_mach_scalar` is `NULL` for those targets in
`mach/dispatch.c`; it is not the removable fallback. On x86-64 the fallback is
the NYIR text emitter: `-S` reaches `nyr_x86_64_emit_ir_with_options` when
machine lowering or emission rejects a function, and unconditionally for
`-fPIC`, `-mindirect-branch=*`, and `-mfunction-return=thunk-extern`. `-c` has
no NYIR fallback: the frame encoder runs first, machine form second, and a
function neither can encode is a hard error. The JIT keeps the documented NYIR
path for unsupported shapes. Removing the obsolete fallback therefore means
giving `-S` machine-form PIC and retpoline support, not deleting `stack.c`.

The JIT machine-form gate now fails closed: an unsupported shape still falls
back to the NYIR encoder, but a machine-verifier rejection after successful
lowering is reported and aborts compilation instead of continuing with an
error string the caller never sees.

A randomized differential sweep (120 generated programs, each exercising
three independent unsigned/16/32/64-bit shapes with masked shift counts and
nonzero divisors, compiled by GCC and NYC at `-O2` through both `-c` and
`-S`) produced zero output mismatches. An earlier sweep that did not mask
shifts and divisors disagreed with GCC in 80 of 120 cases, but every one of
those programs contained undefined behavior; the corrected generator compares
only fully unsigned, defined arithmetic. A second generator exercise (
floating point with exactly representable coefficients, a struct passed and
returned by value with a 16-bit member and an address-taken member, arrays,
`for`/`while` with `break`/`continue`, `switch`, and pointer writes) added
80 programs with zero mismatches on both routes at `-O2`, and 80 more each at
`-O0` and at `-O2 -fPIC`, again with zero mismatches and zero compile or link
failures. A fixed-input version of the mixed generator is now the permanent
`mixed-shapes` fixture: it pins
`42 87 32.7500 16.3750 16.3750 65 230 1` on both routes and asserts that
neither reaches the stack encoder.

`asm-hardening` pins the assembly route's guard and retpoline output:
`-S -fstack-protector-all` calls `__stack_chk_fail`, and
`-mindirect-branch=thunk-extern` routes the indirect call through
`__x86_indirect_thunk_r11`; the linked binary prints `2 65` with stubbed
thunks, so both shapes execute rather than merely appear in the text.

The complex work covers locals, members, and component access. Complex locals
are declared as aggregate objects in the entry-block aggregate area with
synthetic two-field types (an F32 pair, an F64 pair, and a long-double pair)
rather than single eight-byte local homes, so `y = x` lowers to the aggregate
copy path and matches GCC at both `-O0` and `-O2`; the earlier single-slot
layout overlapped a neighbour's high half and produced `2.5` for `y.real`.
`complex-locals` pins `16 16 1.5 2.5 1.5 2.5`, `1.75 2.5`, `0.5 -1.5`, and
`1.25 -2.5` at both optimization levels, alongside `complex-components` for
member and pointer component access. Complex parameters and returns use the SysV two-SSE form: signatures are
lowered as synthetic aggregate shapes, so `double _Complex` returns travel in
`xmm0`/`xmm1`, `float _Complex` returns pack into `xmm0`, and MEMORY-class
shapes take the hidden buffer. `complex-locals` matches GCC at `-O0` and
`-O2` for parameter and return calls, component arithmetic (`+` and `-` are
component-wise; double `*` and `/` call `__muldc3`/`__divdc3`), imaginary
literals (`2.0i`, `1.5fi`), and scalar-to-complex assignment. `float _Complex` multiplication and division call `__mulsc3`/`__divsc3`,
`1.0iF`-style imaginary suffixes are accepted, float/double complex assignment
converts lanes, and complex local initializers accept complex, imaginary, or
real values. `long double _Complex` follows nyc's eight-byte `long double`
model, so it is a 16-byte SSE/SSE object; mixed scalar/complex arithmetic
(`2.0 * I`, `I * I`), complex comparisons, and unary negation are also
covered. The complex checklist item is removed.

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
Other kernel-compatible flags still need a truthful capability audit; see the
2026-11-17 code-generation semantics probe below for what is honored and what
is rejected today.

Earlier clean diagnostics in `/tmp/nyc-clean-kernel.wHQhQ50y` and
`/tmp/nyc-strict-acceptance.ll52Dd2k` also failed before acceptance. Historical
forced-O0, mixed-GCC-wrapper, or incremental links cannot establish current
coverage. Previous object-count percentages are not acceptance evidence.

### Code-generation semantics probe (2026-11-17)

Every probe below used `build/release/nyc -S` on a one-function translation
unit, and the standard-input route was checked directly
(`printf 'int f(void){return 0;}' | nyc -x c - -S -o /tmp/stdin.s`, exit 0).

Honored, with the expected instructions in the output:

- `-fstack-protector`/`-strong`/`-all` place the frame guard (`%fs:40` load and
  store) and branch to `__stack_chk_fail` at every normal return. On a local
  array that `-O2` scalarizes away, the guard is correctly omitted, matching
  GCC `-O2` on the same input. Address-taken arrays are protected at `-O2`.
- `-fcf-protection=branch` emits `endbr64` at function entries.
- `-mfunction-return=thunk-extern` and `=thunk-inline` replace `ret` with
  `__x86_return_thunk`.
- `-mindirect-branch=thunk-extern` emits `__x86_indirect_thunk_*` for indirect
  calls; `=thunk-inline` emits an `lfence` retpoline. `=keep` is rejected
  (exit 2) even though GCC accepts it.

`-pg` now inserts the profiling entry hook, and `-mfentry` selects
`__fentry__` instead of `mcount`; `no_instrument_function` opts a function out
of both. Verified in assembly and object output and by running linked stubs:
`mcount`/`__fentry__` were entered once per live function, arguments survived
the call, and an opted-out function produced no hook. `-mrecord-mcount` and
`-fpatchable-function-entry=N[,M]` are still rejected loudly (exit 2, never
silently ignored), as is `-finstrument-functions`; `__mcount_loc` records and
patchable NOP prefixes are not implemented, so Kbuild's
`cc-option,-mfentry -mrecord-mcount` probe fails and a build must take the
`-pg` plus `recordmcount` fallback. The tracing half of the code-generation
item stays open.

`_Complex` has no representation in the C frontend's own type model, so no
complex declaration, expression, or ABI path exists:

- `double _Complex g;` and `double _Complex f(void)` fail with "function
  declaration requires a parameter list", which is a misleading diagnostic for
  a type problem.
- `_Complex double g;` fails with "unsupported C type", and a complex
  parameter fails with "expected ',', found 'a'".
- `__real__`/`__imag__` are not operators in the frontend, and imaginary
  literals are unsupported.

Progress (2026-11-17): the frontend now has `C_C64`/`C_C128` type tags with
8/16-byte sizes, pointer forms, and `_Complex` accepted both before and after
the base type. `double _Complex g;`, `_Complex double h;`, `float _Complex f;`
and `double _Complex *p;` parse and compile; `long double _Complex` and
non-floating `_Complex` fail closed with specific messages. `sizeof` and
`_Alignof` are correct for type names, globals, locals, and dereferenced
pointers (`16 8 8 4`, matching GCC), pinned by the `complex-layout` regression.
Complex struct fields now lay out and classify as SSE/SSE, and member type
resolution returns the complex tag; reading a complex member as a value still
fails closed, and arrays of complex fields are rejected. Still open: every
arithmetic and conversion path (`__real__`, `__imag__`, `+ - * /`), ABI proof,
and complex value support. `long double _Complex` (32 bytes, 16-byte aligned)
is now a type, so the system `<complex.h>` declarations parse; sizes and
layouts match GCC (`32 16 32` for size, alignment, and the containing struct). Complex component access (`__real__`/`__imag__`) is implemented for double and
float complex objects, including lvalue assignment, and verified against GCC;
`long double _Complex` components and complex arrays are rejected. There is
still no complex rvalue form: every unimplemented value path fails closed —
complex parameters and returns, local and global reads, complex casts,
`*p = *p`, `p[0] = p[1]`, and `s->z = s->z` produce diagnostics, never wrong
code. Next: a two-lane rvalue form with temporary storage, then `+`/`-`/`*`/`/`
with an explicit C99 `__muldc3`/`__divdc3` decision, then the two-SSE
argument/return ABI with a GCC-differential test.

Design decision for that rvalue form: carry a complex value as
`(address, C_AGG, synthetic two-field aggregate)` instead of adding a new
`c_value_t` shape. Reusing the aggregate tag makes the established 16-byte copy
path, the SSE/SSE argument classification, and the two-eightbyte aggregate
return apply unchanged, while arithmetic keeps rejecting `C_AGG` operands
rather than treating the address as an integer. Component access already works
from an address, so `__real__`/`__imag__` need only recognize the synthetic
aggregate marker to keep their current lowering. File-scope complex arrays inherit the existing limitation that
file-scope floating-point arrays are unsupported (plain `double arr[3];` fails
the same way). The item must not be removed until the open pieces work.

The layout helper (`ny_ctype_layout`, `2 * sizeof(T)` with element alignment)
and the FFI mapping (c64/c128) exist, but nothing reaches them from a parsed C
translation unit, so `complex.h` still cannot be compiled.

### SysV aggregate/variadic audit (2026-11-17)

A differential probe compiled by NYC and by GCC `-O2` (one source, one runtime)
produced identical output for:

- `struct { long; double; }` returned by value and passed both in registers and
  after the register banks are exhausted (mixed INTEGER/SSE class).
- `struct { double, double; }` and `struct { long, long; }` returns, covering
  SSE/SSE and INTEGER/INTEGER classification.
- A 32-byte aggregate returned through a hidden sret pointer, with the copy
  visible at the call site.
- Register-bank rollback: five integer arguments followed by a mixed aggregate,
  a two-SSE aggregate, a two-integer aggregate, and a trailing integer.
- `va_start` after an aggregate named argument, and scalar `va_arg` reads after
  partially consumed integer and SSE banks.

Remaining defect: `va_arg` of an aggregate is rejected unless it occupies
exactly one INTEGER eightbyte and no further class. `va_arg(ap, struct { long;
  double; })` fails at `src/code/lang/c/lower/parts/expr.h:4616` with "va_arg
aggregate must fit one integer register", so two-register aggregate varargs
(mixed or same-class) are unsupported. `NYIR_VA_ARG` consumes a single class,
so supporting them needs a register-bank-aware variant that either takes all
required classes or falls back to the overflow area as a unit. The boundary item
stays open for that case. The passing cases are pinned by the
`sysv-boundary` regression in `src/code/test/lang/run.cmake`.

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

`complex.h` still fails because the C frontend has no complex type (see the
2026-11-17 probe record). Physical machine lowering still falls back to stack
encoders, including for the new stack-guard operations. Kernel tracing flags
`-mrecord-mcount` and `-fpatchable-function-entry` are rejected rather than
implemented; `-pg` and `-mfentry` are implemented. The next full build must use a newly frozen
compiler and fresh output tree after these blockers are addressed.

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

Kernel 7.3.5 diagnostic retry (2026-11-18, real objtool, no
`NYC_KERNEL_COMPAT`): 3,038 objects compile and 24 objects fail, with zero
encoder or text-assembly failures; every remaining failure is a frontend
diagnosis. Block `_Static_assert` diagnostics fell from 2,298 to 0 after
`typeof(*p)` for a `void *` was fixed to denote void (this frontend spells
void `C_BAD`), which is the `container_of()` pointer-type assert. Assembly
inputs now follow the standard assembler-with-cpp pipeline: `.S` and `.lds.S`
are preprocessed in assembler mode (16- and 32-bit included) with correct
dependency targets, then assembled by GNU `as`. Zero padding after code was
replaced with `0x90` NOPs at every x86-64 alignment site, which removed all
objtool decode failures on the sampled objects. Inline-asm forms with no
direct-object encoding are completed through GNU `as` with a per-unit note;
section-bearing asm remains rejected for ordinary direct-object builds, while
`-mcmodel=kernel` builds accept it because objtool needs those annotations.
i386 whole-unit C also uses its assembly emitter plus `as`, producing real
objects for vdso32. `extern typeof(object) object;` redeclarations reuse the
existing aggregate array (numa, netfilter core), register-addressed
byte/word/long inline-asm loads and stores lower to typed memory operations
(MMCONFIG), per-CPU IRQ stacks may request 16 KiB data alignment, and
string-literal conditions in file-scope constant expressions select their
taken operand (SIP headers). The C frontend suite passes (55.09 s). The build
still emits 2,412 compiler/objtool warnings, dominated by
`__stack_chk_fail() missing __noreturn` (the attribute is not yet carried
into the emitted symbol) and redundant UACCESS disables; neither is
suppressed. Remaining failures are frontend limits: i915 designated
initializers with `[TRANSCODER_...]` enumerators, `typeof(...)` cast call
targets, aggregate pointer depth through casts, 64-byte local array
alignment, constant `sizeof` of anonymous structs with `_Static_assert`,
`&"literal"` integer initializers, pointer-array range designators,
member-array decay in call arguments, macro lvalue swaps, and integer-to-enum
field assignment. `bzImage` is therefore still not produced and this
paragraph is diagnostic evidence, not acceptance.
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

i386 setup zero-fill gate (2026-11-15): the C frontend zero-filled every
uninitialized aggregate local at all optimization levels, so every
address-taken setup local emitted an inline fill. The `lower_zero_fill`
pass in `src/code/lang/c/lower/parts/initializer.h` now runs only when the
translation unit was compiled without optimization
(`nyc_source_t.optimization == 0`), which the driver records in
`src/code/lang/c/pipeline/driver.c`, `parse.h` and `parse.c`. A byte/word
member written later still zero-fills because the partial-store lowering
depends on it; only full-length fills are gated. A reproducer fell from 145
to 49 assembly lines at `-Os`. The focused setup link falls from 38,307 to
33,387 bytes of `.text` (`.text32` ends at `0x85da`), leaving 5,890 bytes to
the `0x7000` threshold. The i386 portable-runtime fixture passes and a
differential run of the 523-test C corpus through the old and new compilers
shows no behavioral differences (the single differing case,
`i386-lsl-alternative`, reads RDPID hardware state and varies run to run
under either binary). The `nyr-c-frontend` CTest remains blocked by the
uncommitted `object64.c` regression in the active freebuff session
(`builtin-ffs` segfaults under both the old and new binaries), so the
differential harness stands in for it.

i386 displacement-folded addressing (2026-11-15): the i386 register-run
emitter now resolves a chain of constant address arithmetic into one
displacement at its final user. When an `ADD`/`SUB` of a constant inside a
run has a single-user chain that ends at a byte, word or long load or store
in the same run, every link is skipped and the user emits
`mov X, disp(%base)` instead of materializing the address; chain totals are
gated to 16-bit range, every link must have one definition, and the base
counts as live up to the user so eviction still homes it. This removes the
address-accumulator pattern (`mov base; addl $imm; mov X, (%reg)`).
`src/code/backend/mach/arch/i386/registers.h` gained `sm_i386_sole_user`,
`sm_i386_resolve_fold` and folded load, store and sign-extend branches. The
differential corpus run and both cold i386 runtime fixtures pass, and a new
reusable real-mode QEMU probe (`/tmp/nyc-setup-qemu-probe.sh` with
`/tmp/nyc-fold-probe.c`) boots a probe-built setup image under
`qemu-system-i386 -kernel` and prints identical `FOLD` hashes for the old
and new binaries. The focused setup link falls another 769 bytes:
`.text` is now 32,618 bytes and `.text32` ends at `0x82d9`, leaving 4,825
bytes to the `0x7000` threshold. Address-accumulator `addl $imm, %reg`
sites in the setup assembly fell from 287 to 109.

i386 register copy propagation and dead SARL brackets (2026-11-16): two
spill-cleanup passes in `src/code/backend/mach/arch/i386/spills.h` remove
whole instructions without touching stack homes, so the home-coloring and
home-ranking passes see byte-identical inputs and cannot regress.

1. Register copy propagation. A plain `movl %A, %B` whose `%B` is read once
   by the next instruction and is dead afterwards is folded: that instruction
   reads `%A` and the copy is deleted. Only register-to-register copies qualify
   (never an immediate or a home), so no operand form or home reference changes.
   It fires only when `%B` is dead at the next instruction and at the
   successor; liveness is recomputed between rounds.

2. Dead `pushl %R; sarl $31, %R; popl %R` bracket removal. This three-line
   sequence restores `%R` and only alters flags, so it is dropped when no later
   instruction consumes those flags before overwriting them. The scan walks
   mov/lea/push/pop/cltd/nop, follows single `jmp` edges (bounded), stops at any
   flag writer, and refuses to cross a label with a live incoming edge. The
   earlier revision compared the whole `sar` operand string (`$31, %eax`)
   against `push`/`pop` (`%eax`) and therefore never matched; the fix splits the
   shift operands and compares only the destination register.

A rejected predecessor used the same idea on home reads (`movl home,%R;
testl %R,%R` → `cmpl $0,home`). It is byte-identical in isolation but moves a
home read across a label, which perturbs the heuristic home-coloring pass and
netted +81 bytes across the corpus, so it was replaced by the register-only
copy propagation above.

Measured with the pre-edit compiler `/tmp/nyc-preedit` (built from
`6eed9a3e^`) over the 20 focused setup C units, default per-function variant
choice, `-Os`: `.text` falls from 32,409 to 31,908 bytes (501 bytes). Every
one of the 20 units shrinks or stays equal, and every forced register variant
(`baseline`, `compact`, `four`, `three`, `promoted`, `promoted-four`,
`colored`, `colored-four`) shrinks or stays equal; no unit grows under any
variant. Per-pass contribution: copy propagation −284 bytes, SARL-bracket
removal −205 bytes, move-forwarding 0 bytes (it never fires; the remaining
adjacent register copies are already handled by copy propagation or carry a
second use). The focused link with `/tmp/setup-permissive.ld` reports
`.text` 32,530 → 32,029 and `.text32` end `0x8281` → `0x808c`, leaving 4,172
bytes to the `0x7000` threshold. The 523-test differential C corpus shows no
behavioral change other than the pre-existing flaky `i386-lsl-alternative`
fixture (it reads RDPID hardware state and returns different values run to run
under either binary), and the real-mode QEMU probe prints identical `FOLD`
hashes. The original Linux setup linker still rejects the image at its
64-sector cap. For reference, compiling the same 20 units with the host GCC
16.2.1 yields 11,664 bytes of `.text`, so the remaining gap is code-generation
quality rather than one outlier object.

i386 copy-propagation extension to memory/immediate sources (2026-11-16):
the register-only copy propagation was extended to fold `movl home,%eax;
movl %eax,%R` and `movl $imm,%eax; movl %eax,%R` patterns. The def instruction
is restricted to `movl` (width-safe; excludes `leal`). Rewriting uses operand-
level replacement: the source operand of the consumer is changed from the
destination register to the original register/home/immediate, and the consumer
line is kept while the def is removed. Because a stack-home reference merely
moves from the removed def onto the adjacent use, the home-use multiset is
unchanged — home ranking and coloring see byte-identical inputs. Verified:
20-unit `.text` 31,265 → 31,134 (−131 B); link `.text` 31,255; `.text32` END
`0x7d86`; 523/523 differential corpus identical (only pre-existing flaky
`i386-lsl-alternative`); QEMU probe `FOLD 149d8d80` PASS; i386 runtime probes
(spill-arithmetic, register-runs, aggregate-abi) 72 cases 0 differences;
forced-variant probes 96 cases 0 differences; valgrind clean.

i386 register-only cleanup for promoted variants (2026-11-16): the three
promoted variants (`promoted`, `promoted-four`, `promoted-three`) previously
skipped `sm_i386_cleanup_spills` entirely. Added a `registers_only` parameter
that runs only the register-copy-propagation, dead-register-def removal, and
terminal-branch cleanup passes while skipping RMW fold, home-compare fold,
home ranking/coloring, and `reg_removable` logic (which could misclassify
promoted-frame offsets). The three promoted variants now receive this pass
after `sm_i386_promote_homes`. Verified: 20-unit `.text` 31,134 → 30,873
(−261 B); link `.text` 30,994; `.text32` END `0x7c81`; all correctness gates
identical to above.

i386 register-run fallback and constant-fold opcode guard (2026-11-17): two
regressions introduced by later passes and by the copy-propagation extension
above stopped the Linux setup tree from assembling/compiling at `HEAD`, so they
are fixed together.

1. Register-run allocation fallback. The select-chain folding pass emits
   `NYIR_SELECT_I64` with constant operands. The i386 run emitter loads all
   three inputs (`a`, `b`, `c`) and the result into registers; with the `three`
   variant (`i386_register_limit` 3) a select whose condition and both values
   stay live across the instruction needs four registers, so
   `sm_i386_emit_register_run` returned `-1` and aborted the whole function with
   "i386 local register allocation failed" (reproduced on
   `arch/x86/boot/early_serial_console.c`). A register run is an optimization,
   so the emitter now returns 0 — leaving the whole run to the general emitter,
   which always produces a correct sequence — when allocation cannot fit it.
   No output is committed to the real writer before the fallback.

2. Constant-fold opcode guard. The memory/immediate copy propagation above
   folded an immediate into a consumer without checking the opcode, emitting
   `cmovnel $imm, %reg` (invalid) in `arch/x86/boot/cpu.c`. Constant folding is
   now restricted by `sm_i386_spill_imm_source` to the `mov`/ALU forms whose
   source operand accepts an immediate; `cmov`, `set`, `movz`, `movs` and `lea`
   are excluded.

Verified with both fixes: all 20 setup units compile and assemble; the
permissive link reports `.text` END `0x7ca8` and `.text32` END `0x7cc6` (69
bytes above the `683dd0f0`-based `0x7c81`, the cost of the newer middle-end
passes, not the fixes); 522/523 differential corpus identical (only the
pre-existing flaky `i386-lsl-alternative`); real-mode QEMU probe `FOLD 149d8d80`
PASS; i386 runtime probes 72 cases 0 differences.

Full-kernel compile+assemble scan and i386 setup size diagnosis (2026-11-17): the
frozen Linux 7.3 Kbuild command corpus (3,025 saved `.<obj>.o.cmd` files) was
replayed through `nyc-fix2` (`HEAD` plus both fixes) with `-S` in place of `-c`
and GNU `as` in place of the assembler (`--64` for the kernel, `--32` for the
`-m16`/`-m32` setup units). Output names are keyed on the full source path so
that units sharing a stem (`core.c`, `main.c`, ...) do not overwrite each other.

- Every kernel-target C unit compiles and assembles: 2,974/2,974, with no warning
  suppression and no fallback. This is stricter than the build, because each
  unit's flags are re-derived from its own saved command.
- 51 units are host-side build tools under `tools/`, `scripts/` and
  `arch/x86/tools/`. 31 have no source in the frozen tree (only the `.o.cmd`
  was saved), 2 compile, and 18 expose NYC gaps: one option gap
  (`-falign-jumps=1`, on the synthetic `scripts/mod/.empty.o.cmd`) and 17 genuine
  C-frontend errors on `scripts/kconfig/*`, `scripts/mod/*`,
  `scripts/sorttable.c`, `scripts/tracepoint-update.c` and
  `arch/x86/tools/relocs_*.c` (recurring classes: `address-of parenthesized array
  subscript requires an lvalue`, `C array parameter requires constant extents` in
  `/usr/include/regex.h`, unsupported `__may_alias__`, and undeclared `popen`).
  The acceptance explicitly permits the host compiler for host tools, so these do
  not block the kernel target build; they are recorded as frontend-capability
  gaps, not acceptance failures.

Size diagnosis for `arch/x86/boot/setup.elf`: the permissive link reports `.text`
END `0x7ca8` and `.text32` END `0x7cc6` against the `0x7000` milestone. The real
`arch/x86/boot/setup.ld` requires `_end <= 0x8000`. Because `.pecompat` is
`salign = 0x1000`-aligned (`header.S`), `_end = ALIGN(text32_end, 0x1000) +
0x18c0`, so `_end <= 0x8000` actually needs `.text32` END `<= 0x6000` — a
~7,366-byte (23.7%) text cut, not the 3,270 bytes the `0x7000` milestone implies.
Instrumenting the run emitter shows register runs already cover 70.6% of NYIR
instructions but average only 4.4 instructions each: the opcode rejections are
`BR` (38%), `LABEL` (34%), `CALL` (19%) and `RET` (7%) — i.e. control-flow run
boundaries — while the `definitions != 1` gate is a non-issue (`defs = 0`; NYIR
values are SSA) and wide values are only 0.8%. The dominant `movl` traffic (44.6%
of code bytes; 2,169 adjacent `movl`/`movl` pairs) is short-run boundary
spill/reload plus the general emitter's stack-home style, i.e. a whole-function
register-allocation problem, not a fixable gate. Frame-idiom overhead is ~797
instructions (~9.6%); eliding the frame for the 104 functions whose bodies never
address `%bp` saves only ~397 bytes and dead `%ebx` saves ~85 bytes. These
micro-wins cannot close the gap; reaching the 32 KiB setup bound needs
architectural value-in-register allocation, not peepholes.

Configured target-coverage audit (2026-11-17): the frozen Kbuild command corpus
in `/tmp/nyc-current-kernel-strict-20261114` holds 3,152 `.<obj>.o.cmd` files:
2,974 target C units, including 12 generated `.mod.c` module-metadata units; 51
host-tool C units; 123 assembly, objcopy and other commands; and four non-C
stubs (two `fixdep` "cannot find" records and two `objcopy
--prefix-symbols=__pi_` steps). The configured target-C count is the
denominator, not a Kbuild progress counter.

Replaying all 2,974 saved target commands with the current `HEAD` compiler
(`nyc` rebuilt from `ed55d4eb`), `NYC_KERNEL_COMPAT=1`, each unit's own
re-derived flags, `-S` in place of `-c`, and GNU `as --64` (or `--32` for the
`-m16`/`-m32` units) compiled and assembled **2,974/2,974**, with no compile,
assemble or timeout failures. Host-tool C is excluded because acceptance permits
the host compiler for host tools. The corpus is the command set of a configured
tree whose C-compile phase completed before the `setup.elf` link failure, so a
configuration change requires regenerating the denominator.

Capability probes were re-checked at `HEAD`: `nyc -x c -` reading C from
standard input, `-x c` on a file without a `.c` suffix, and a plain `.c` file
each produce a NYC object, so explicit-language and standard-input probes no
longer require a wrapper to route them away from the host compiler.

i386 cross-block run tracing experiment (2026-11-17): the straight-line i386
register runs were extended across one unconditional branch into a block with
exactly one predecessor. The implementation tracked a per-instruction coverage
map so the outer emitter skips the block emitted early, reset the
adjacent-instruction reuse state at each skip, homed live-out values before
both the intra-run branch and the traced tail, and emitted an explicit jump to
the traced tail's continuation.

Soundness required three restrictions. Runs whose allocation can reach
EBX/ESI/EDI were excluded, because their save/restore wraps the whole body and
a jump out of the body would leave the saved words on the stack; only variants
limited to the three caller-saved registers could cross. The instruction before
the target label had to be terminal, so no implicit fallthrough enters the
target. The traced tail had to end at a branch target or at the function end, so
control leaves the run explicitly instead of relying on the assembly order.

Measured on the same 20 focused setup C units: `.text` 30,884 → 30,880 bytes
(4 bytes, END `0x7bf5` → `0x7bf1`) and `.text32` END `0x7c13` → `0x7c0f`. All 72
i386 runtime probe cases (`i386-spill-arithmetic`, `i386-register-runs`,
`i386-aggregate-abi`; regparm 0 through 3 at `-O0` through `-O3`, `-Os`, `-Oz`)
produced identical exit statuses to the pre-change binary. The 523-test
differential C corpus was identical except the pre-existing
`i386-lsl-alternative` hardware flake: one binary returned 77, 45, 44, 90 and
90 on five consecutive runs of the same executable, so that case is not a
regression signal. The change was reverted because 4 bytes is noise against the
7,366-byte setup gap, and the reason is structural: a crossing run cannot hold
values in callee-saved registers under the current per-run save/restore, so
only the 3-register variants can cross and the per-function selector rarely
picks them. Reaching the setup bound needs function-scoped register
save/restore plus block reordering that makes single-predecessor successors
physical fallthrough, so that runs are contiguous and can own EBX/ESI/EDI
across boundaries.

Pre-existing failure at `HEAD`, unrelated to that experiment: the
`nyr-c-frontend` CTest fails in `i386-spill-arithmetic O0 regparm=0` ("returned
incorrect results: 1") with and without the tracing change. Because the suite
is red at `HEAD`, the differential corpus and the runtime probes were used as
the correctness gate for this experiment.

i386 spill RMW-fold cache desynchronization (2026-11-17): EXISTING DEFECT —
`nyc -m32 -mregparm=0 -O0` returned wrong results for every 32-bit binary
arithmetic family, which is the reason `nyr-c-frontend` failed in
`i386-spill-arithmetic O0 regparm=0`. A minimal
`add32(unsigned a, unsigned b) { return a + b; }` emitted
`movl -24(%ebp),%eax; movl %eax,-12(%ebp); movl %ecx,%eax`: no `addl`, and a
return value from a register that was never written. `-O1` and `-O2` were
correct, a GCC build of the same probe passed, and the defect disappeared with
`NYC_DISABLE_I386_RMW_FOLD=1`, `NYC_DISABLE_I386_SPILL_CLEANUP=1`, or
`NYC_I386_REGISTER_VARIANT=baseline`.

FIX — the RMW fold in `sm_i386_cleanup_spills` rewrites an adjacent
`movl home,%r` / `addl %x,%r` / `movl %r,home` triplet into `addl %x,home` and
marks the load's old store removed. The cached-home reload pass that follows
still visited that removed store and recorded `%r` as caching `home`, so the
next reload of `home` was rewritten to the stale `%r`; the folded memory form
was then dropped as a dead store because nothing appeared to read it. The cached
reload pass now skips removed lines, so a deleted store can no longer claim a
live register copy of the home it used to write. The change is three lines in
`src/code/backend/mach/arch/i386/spills.h`.

REGRESSION/EVIDENCE — the isolated repro and `i386-spill-arithmetic` at `-O0`
with regparm 0 through 3 now exit 0 (previously 1). The 72-case i386 runtime
probe (`i386-spill-arithmetic`, `i386-register-runs`, `i386-aggregate-abi`;
regparm 0 through 3 at `-O0` through `-O3`, `-Os`, `-Oz`) changes in exactly
eight cases, all `old=1 new=0`, with the other 64 identical. The 523-test
differential C corpus is otherwise quiet: 517 identical, five cases where the
frozen pre-change compiler's output exited 139 (SIGSEGV) and the fixed build
returns 0, and the known `i386-lsl-alternative` hardware flake whose exit status
varies between runs of one executable (35/34, then 62/65). The focused 20-unit
setup link is unchanged (`.text` 30,884 bytes END `0x7bf5`, `.text32` END
`0x7c13`), so the 7,366-byte setup gap is unaffected.

i386 boolean-branch fold emitted the stale condition (2026-11-17): EXISTING
DEFECT — after the spill fix, `nyr-c-frontend` stopped at `asm-bit-string-rmw
O3 regparm=0 returned incorrect results: 3`, which the frozen pre-change binary
also returns at regparm 0 and 2 for `-O1` through `-O3`. Minimisation: the
original file with that check split into three separate `if`s passes, and a
stripped probe keeping only the compound `if (!bit_test(b,-1) ||
!bit_test(b,33) || bit_test(b,0))` also passes; adding `bit_toggle` and
`bit_clear` after it reproduces at `-O3` (regparm 0 and 2) but not at `-O0` or
`-O1`, and moving those two calls to a second array makes it pass.
`NYC_DISABLE_I386_SPILL_CLEANUP=1` alone fixes it, so the defect is in the
machine cleanup rather than the C frontend. The emitted code tested `!b` as
`cmpl $0,%eax; jnz`, returning 3 when `b != 0` instead of when `b == 0`.

FIX — `sm_i386_cleanup_spills`' boolean-branch fold computes the correct new
condition for a `set<cc>` / `movzbl` / `testl` / `jcc` quadruple (`sete` with
`jnz` must become `je`), but it rewrote only the parsed `op`, while emission
prints the line's `text`/`replacement`. The old condition was therefore still
assembled. The fold now builds the rewritten branch (`text`, `args`,
`replacement` and `op`), so the computed polarity is what gets emitted. For the
common `setne` with `jnz` case the old text happened to be an alias of the
computed `jne`, which hid the defect.

REGRESSION/EVIDENCE — the stripped repro and the whole `asm-bit-string-rmw`
matrix (regparm 0 through 3 at `-O1` through `-O3`) exit 0, and `ctest -R
'^nyr-c-frontend$'` is 100% green, where it had stopped at this probe once the
spill case was fixed. The 72-case i386 runtime probe still shows only the eight
`i386-spill-arithmetic`/`i386-register-runs` improvements (`old=1 new=0`); the
523-test differential corpus is unchanged (517 identical, the same five
pre-change SIGSEGV cases and the `i386-lsl-alternative` flake); the focused
20-unit setup link is unchanged (`.text` 30,884 bytes END `0x7bf5`, `.text32`
END `0x7c13`).

i386 setup-size lever measurements (2026-11-17): `bzImage` remains the only
missing artifact — `vmlinux`, `vmlinux.o`, `vmlinux.unstripped`,
`vmlinux.symvers` and twelve `.ko` files exist in
`/tmp/nyc-kernel-final-20261113` and `/tmp/nyc-current-kernel-strict-20261114`.
Three candidate levers for the 32 KiB setup bound were measured on the 20
focused setup C units and rejected:

- Register-variant selection: default and `preallocated` both give `.text`
  30,884 bytes with `.text32` END `0x7c13`; `four` gives 32,896, `three`
  32,433, `promoted` 34,975 and `baseline` 37,646. The selector already chooses
  the smallest variant, so preferring another one cannot help.
- Per-call 16-byte stack alignment: the boot units already compile with
  `frame_align == 4` and the simple call sequence, so the whole
  `pushl %ebx` / `movl %esp,%ebx` / `andl $-16,%esp` / ... / `movl %ebx,%esp` /
  `popl %ebx` idiom costs 6 bytes across the twenty objects, not the several
  kilobytes a call-count estimate suggested.
- Size level: all 24 saved boot-unit commands use `-Os`, and rebuilding them at
  `-Oz` gives byte-identical totals (`.text` 30,884, `.text32` END `0x7c13`), so
  `-Os` is not withholding `-Oz` wins on this code.

The 7,187 bytes (`0x7c13 - 0x6000`) that separate `.text32` from the required
bound therefore stay where the earlier diagnosis put them: function-scoped
register allocation (callee-saved values with save/restore around the whole
body) plus block reordering that makes single-predecessor successors physical
fallthrough, so register runs are contiguous and can own EBX/ESI/EDI across
blocks. No peephole lever tested so far moves that number.

General `__seg_fs` / `__seg_gs` C address spaces: implementation map (2026-11-17).
The qualifier is consumed and discarded in `parse_type`'s qualifier loop
(`src/code/lang/c/parser/parse.c:4903-4909`, accepting both spellings), and the
same tokens are skipped in the declarator paths at 4381, 4439, 4490 and 4587.
Only Linux inline asm honours GS today, via the `__seg_gs` token checks in
`lower/parts/asm.h` (8736, 8759, 10486) and the `gs_add_width` path, so a C
dereference through a `__seg_gs`/`__seg_fs` pointer silently becomes a plain
access. Bits 22 and 23 are free in `src/include/nyr/ir.h` next to
`NYIR_INST_F_MEM_VOLATILE` (128u) and `NYIR_INST_F_MEM_U16` (256u), so
`NYIR_INST_F_MEM_SEG_FS` (1u<<22) and `NYIR_INST_F_MEM_SEG_GS` (1u<<23) can
carry the fact on `NYIR_LOAD_I64`/`NYIR_STORE_I64` without disturbing the
verifier's existing mask. Propagation can reuse the `pointee_volatile` channel:
parser state `last_type_volatile` (parse.c:336, reset at 4410, propagated at
3151 and 12900-12941), the `c_volatile_pointee_marker`/`c_value_pointee_volatile`
pair (parse.c:525 and 565), the cast plumbing in `expr.h:3812` and 4191-4257,
and the `local->pointee_volatile` sites in `statement.h:2576/2805/2945` with
their deref consumers in `expr.h:2897-2936, 5421-5674, 7554-7710` and
`initializer.h:3317-3418`. Emission sites that would need the prefix: x86-64
textual `machine64.c:3282` (indexed and plain load) and 3325 (plain and
indexed store), i386 textual `stack.c:4026`/`4065`, and, for parity, the
direct-object paths `arch/x86/object.c:1315/3805` and the LLVM emitter at
`emit/llvm/emitter.c:1477`, which must either encode the prefix byte or reject
the flag explicitly instead of emitting a plain access.

This is deliberately not half-landed. The frontend has no single choke point for
memory-access flag composition, so a partial wiring leaves some dereference
sites silently unprefixed — a wrong-address miscompile that compiles and links.
It also has no way to be proven at runtime in this repository's harness: i386
user mode cannot set a GS/FS base (no `arch_prctl`/FSGSBASE there), and there is
no bootable `bzImage` yet, so the only available evidence would be static
(`%gs:`/`%fs:` in the emitted assembly and GNU `as` acceptance). The next step
is a helper that composes the memory flags once, used by every deref site, so
the flag cannot be dropped by omission.

## Correctness verification programme

Merged from `docs/todo/kcc.md` (2026-11-15). The artifact requirements in
[Required acceptance](#required-acceptance) above remain canonical; this section
adds the correctness, verification and reproducibility work needed to back those
claims with evidence.

Reference: <https://github.com/LiterateDrivenDevelopment/kcc>. Extract only
useful testing, reproducibility and semantic-verification ideas. Do not copy
KCC's architecture, literate programming, backend, bootstrap or test harness.
Nytrix already has its own compiler architecture, a native NYR backend, an LLVM
backend, a C frontend, native object generation and the Linux kernel build work
recorded above.

Mission: turn NYC into a compiler whose correctness claims are backed by
reproducible, adversarial, independent tests. Audit before editing. Prefer
removing duplication and fixing real weaknesses over adding new systems, flags,
frameworks or abstractions.

### P0 — Make the test harness trustworthy

Audit `./make check`, the C frontend tests, backend tests, benchmarks and the
existing differential test infrastructure. Find:

- False positives and silently ignored failures.
- Tests that compare only truncated exit codes.
- Compilation failures misclassified as success.
- Tests accepting matching linker/runtime failures.
- Stale outputs accidentally passing.
- Unsupported cases excluded without accounting.
- Fragile expected-failure rules.
- Duplicate or obsolete test implementations.

Build a strict common result model: `PASS`, `MISCOMPILE`, `COMPILE_FAIL`,
`LINK_FAIL`, `RUNTIME_FAIL`, `TIMEOUT`, `UNSUPPORTED`, `INVALID_TEST`. Every real
regression must produce a nonzero test status.

Introduce deliberate fault-injection tests proving the harness detects wrong
output, bad assembly, crashes, link failures, timeouts and stale binaries. A
harness that fails to catch injected bugs is defective.

### P1 — Differential compiler validation

Compare NYC → NYR, NYC → LLVM, Clang and GCC on well-defined C programs with
deterministic inputs. Compare actual behavior: full output, exit status and
signal, binary files and observable memory results, exact integer values,
appropriate floating-point semantics, ABI-visible results and external effects.

Never assume GCC/Clang agreement proves correctness; use standard-defined
semantics to adjudicate disputes, and filter undefined and unspecified behavior
appropriately. Run each case across supported optimization levels. Separate
compiler acceptance coverage from execution correctness, and report both
denominators honestly.

### P2 — Automated bug discovery

Integrate existing fuzzing infrastructure or lightweight adapters to
established generators where useful: Csmith, YARPGen, compiler torture tests and
AST fuzzing. Target NYC's real failure-prone areas:

- Integer promotions and arithmetic conversions.
- Pointer provenance, aliasing and casts.
- Struct/union layout, bitfields and alignment.
- Variadic calls, ABI lowering and return values.
- GNU C attributes and inline assembly.
- Atomics, volatile and memory barriers.
- SSA dominance, phi nodes and CFG rewrites.
- Constant folding, CSE and loop optimizations.
- Register allocation, spills and stack layout.
- ELF relocations, symbols and linker compatibility.

For every mismatch: reproduce it with a stable seed; determine whether the
source is well-defined; identify the earliest divergent compiler stage; minimize
with C-Reduce or IR reduction; fix the underlying compiler defect; preserve a
permanent regression test. Group repeated failures by root cause, not by
superficial error-message similarity.

### P3 — Compiler invariant checking

Audit existing checks before introducing new ones. Strengthen invariant
validation at major boundaries: AST → typed IR → SSA → optimized IR → MACH →
ELF. Check:

- Type and operand consistency.
- Def-use validity and dominance.
- CFG edges and phi predecessor mappings.
- Register allocation and liveness consistency.
- Spill/reload and stack-frame correctness.
- ABI operand constraints.
- Symbol, section and relocation integrity.

Make invalid states fail close to their origin with actionable diagnostics
rather than becoming machine-code miscompilations later. Keep expensive
verification optional for normal builds, but enabled in tests and debug
configurations.

### P4 — Semantic conformance

Build on NYC's existing C17/GNU C feature coverage. Create focused positive and
negative test pairs for the important C constructs NYC actually supports:
positive tests must compile and behave correctly, negative tests must be
rejected for the right reason. Prioritize features required by the Linux kernel,
SQLite, system headers and other existing workloads. Use fast
syntax/semantic-only checks if already present; add them only if missing and
independently useful. Do not inflate conformance numbers with untested features.

### P5 — Kernel acceptance as hard evidence

Preserve the existing Linux 7.3 x86-64 goal and do not restart or duplicate
kernel work already completed. Treat these as separate milestones, not one
boolean:

- Unmodified Linux sources.
- Reproducible configuration and toolchain manifest.
- Every configured C translation unit compiled by NYC.
- GNU `as` permitted for assembly.
- Zero hidden C fallback to Clang/GCC.
- Valid `vmlinux`, `bzImage` and configured modules.
- Verified build-object provenance.
- Reviewed `objtool`/linker/assembler diagnostics.
- Successful QEMU boot and initramfs smoke tests.

Count actual configured C translation units and successful NYC compilations, not
estimated Kbuild progress. Track failures, unsupported features and workarounds.
Preserve the kernel build as a permanent regression suite when acceptance is
achieved. The artifact rules in [Required acceptance](#required-acceptance)
still govern what may count as acceptance.

### P6 — Reproducibility and bootstrap

Improve reproducible build records: compiler revision, flags, dependencies,
target, checksums, seeds, object provenance and exact reproduction commands.
Consider self-hosting only where NYC's current build architecture makes it
practical. Do not add a bootstrap project purely to imitate KCC, and do not
mistake identical binaries for semantic proof.

### P7 — Performance without testing bloat

Keep the verification infrastructure small and maintainable, using the existing
build/test conventions (`./make check`, `./make bench`). Add a consolidated
verification entry point only if it genuinely simplifies existing workflows.

Separate three tiers: `FAST` (deterministic checks after every code change),
`DEEP` (differential fuzzing, sanitizers, stress tests) and `SYSTEM` (full
kernel build, modules and QEMU boot). Cache reproducible inputs and compiler
outputs safely, parallelize independent tests, retain failing artifacts and
discard unnecessary success logs. Do not slow down ordinary production
compilation.

### Implementation rules

- Inspect the current repository first.
- Respect the existing Nytrix architecture.
- Reuse existing test fixtures and helpers.
- Prefer compact C implementations.
- Do not add Python or shell wrappers unnecessarily.
- No massive documentation framework.
- No new backend or register allocator.
- No unrelated refactoring.
- No speculative rewrites.
- No cosmetic changes presented as improvements.
- No suppressed compiler warnings to fake success.
- No hardcoded passing metrics.
- No invented benchmark results.
- No tests marked PASS without execution.

For every change, explain EXISTING DEFECT → FIX → REGRESSION → EVIDENCE. Use
small reversible patches with a stable baseline.

### Execution priorities

1. Find and eliminate false-positive tests.
2. Build a strict NYR/LLVM/GCC/Clang differential oracle.
3. Find and minimize genuine NYC miscompilations.
4. Strengthen IR and machine-code invariants.
5. Close conformance gaps revealed by real programs.
6. Complete and independently validate kernel acceptance.
7. Simplify the resulting code and remove redundant tools.

Do not stop after writing a plan: implement, execute tests, fix defects, rerun
and measure.

### Final delivery

Provide the actual patches/source changes, before/after correctness results, new
bugs discovered and minimized, tests proving each fix, fuzzing coverage and
limitations, current kernel build/boot status, compilation time and memory
regression measurements, and remaining unsupported features and known failures.

### Success criteria

A genuinely stronger compiler, not a larger repository. NYC must reject invalid
input reliably, correctly compile more well-defined C programs, detect
intentionally injected miscompilations, produce reproducible native machine
code, pass strict independent differential tests, maintain or improve compiler
performance, and demonstrate kernel acceptance with actual artifacts.

The objective is not to imitate KCC. It is to take the useful engineering
lessons, improve them, and produce verifiable evidence that Nytrix's existing
compiler is more correct, capable and reliable.

## FS/GS address spaces: implementation record

EXISTING DEFECT. `__seg_gs`/`__seg_fs` were accepted syntactically and then
discarded. Four separate drops were proven with emitted assembly, not inferred:

1. `parse_type`'s leading-qualifier loop skipped both qualifiers (its comment
   claimed accesses only ever appear in inline asm), so no type carried a
   segment.
2. `starts_scalar_type` did not list the qualifiers, so
   `__seg_gs int *p = ...;` inside a function was parsed as an expression and
   rejected with "reference `__seg_gs` must name a function parameter or
   declared local".
3. The dereference built `uint8_t memory_flags`, truncating the new bits:
   `0x800040` became `0x40`, so the backend saw only the width bit.
4. Operator precedence in the same block applied
   `| c_value_seg_flags(address)` to the *else* arm of the width conditional,
   so `char`/`i8` pointers lost the segment while `int` pointers kept it. This
   is why the first static probes showed `%fs:` for `int` and nothing for
   `unsigned char`.

FIX. The segment is now part of the C value's pointee state. A segment marker
is encoded in the existing `c_value_t.agg` marker channel (eight new markers,
const/volatile/segment orthogonal), so no positional initializer changes were
needed. `NYIR_INST_F_MEM_SEG_FS` (`1u<<22`) and `_GS` (`1u<<23`) are set on
loads and stores by the unary-deref, pointer-index, local-index, compound
assignment and initializer paths, and emitted as `%fs:`/`%gs:` prefixes by the
textual x86-64 (`machine64.c`) and i386 (`stack.c`) emitters. The x86-64
machine/register form has no segment field, so `nyr_mach_lower_nir` rejects
functions containing segmented accesses and the existing per-function NYIR
fallback emits them. Backends that cannot express a segment (i386, x86-64 and
AArch64 object writers, portable, LLVM) now report a hard error instead of
silently emitting an unprefixed access.

REGRESSION. `ctest -R '^nyr-c-frontend$'` 100% (53.4 s). C corpus 523 cases:
517 identical, 6 differences that are the same baseline set (five inputs that
segfault under the frozen HEAD binary and now exit 0, plus the known
`i386-lsl-alternative` flake). i386 probe matrix: 72 cases, the same 8
baseline differences (`i386-register-runs` O0). Setup link `.text` unchanged at
30,884 bytes.

EVIDENCE. Both `-S` and `-S -m32` emit the prefix for every supported form:
byte load via cast, 8-byte store, byte store, load through a segmented local,
load and store through segmented parameters, with `%fs:` and `%gs:` both
observed. Example (`-S`): `movzbl %fs:(%rax), %eax`, `movq %r10, %gs:(%rax)`.

A segment declared on a global pointer object (`__seg_gs int *g; *g`, including
subscripts and stores) is now recorded in `c_global_t.pointee_seg` and carried
on the value's marker, so it emits the same prefixes. Halfword loads and
plain-assignment stores are covered too: `lower_load_u16_field_seg` and
`lower_store_u16_field_seg` carry the flag, giving `movzwl %gs:(%eax), %ecx` on
i386 and two prefixed byte accesses on x86-64.

BOUNDARIES (not claimed as done). Segment-qualified non-pointer objects
(`extern __seg_gs int x;`) are still not lowered with a segment base, and
compound-assignment stores to a segmented `i16`/`u16` lvalue still call the
unsegmented helper. Runtime proof remains impossible in this environment: i386
user mode cannot set FS/GS bases and no bootable `bzImage` exists (`.text32`
END `0x7c13` versus the `0x6000` window).
