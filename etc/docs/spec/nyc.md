<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":340,"summary":"NYC's C compiler driver, targets, ABI behavior, and verification evidence."} -->
# NYC C compiler

`nyc` compiles C translation units through the [NYIR](ir.md) optimizer and
native encoders. It implements a subset of GNU C; choosing a GNU dialect
does not imply GCC compatibility for every source construct or flag.

## Compile and link

```sh
nyc -std=gnu11 -O2 -Iinclude -c main.c -o main.o
cc main.o -o app
```

`nyc` compiles; the platform link driver links executables and libraries. A
build that uses `CC` for linking must keep its link command separate. Use
`nyc -E main.c` to inspect preprocessed input and `nyc -S main.c -o main.s`
to inspect GNU assembly. `--all SOURCE.c OUTPUT.o` is an alternative whole-unit
object command; legacy `SOURCE.c OUTPUT.o SYMBOL` emits one named function.
On supported Linux x86-64 builds, `ny nyc OPTIONS...` dispatches to the
standalone executable. `nyc --help` and `./make nyc -h` list driver options,
including which are compatibility-only. [Native](native.md) covers C header
imports and foreign calls from Nytrix source, not standalone C compilation.

## Preprocessing and headers

```sh
nyc -E -P -I include -D FEATURE=1 -U DEBUG -o main.i main.c
nyc -x c -E -
```

Without `-P`, output retains source filenames and line markers. Explicit
`-D`/`-U` apply in argument order. `-pthread` defines `_REENTRANT=1` before
explicit macros regardless of option order; pass `-pthread` to the link
driver separately. `-fmacro-prefix-map=OLD=NEW` remaps `__FILE__` and
`__BASE_FILE__`, using the last matching prefix.

Quoted includes first search the containing file's directory. Explicit
`-I` paths precede modeled headers and native system-header fallback. On
Linux, fallback for the default native target searches `/usr/local/include`,
the host multiarch include directory, and `/usr/include`. `-nostdinc`
disables fallback; explicit targets and embedded frontend use do not enable
it implicitly. Nested includes share macro state; include guards and
`#pragma once` apply across equivalent paths. Missing headers, active
`#error`, and unbalanced file-local conditionals are errors.

Object and function macros expand in declarations, function bodies, and
conditional expressions. Arguments expand before substitution except
operands of `#` and `##`, which keep their unexpanded tokens. Continued
replacement lists preserve comments as whitespace, and `__LINE__` keeps its
source location. `defined` does not expand its operand. Integer and character
conditionals support arithmetic, shifts, bitwise and relational operators,
and short-circuit `&&`/`||`; inactive branches and later `#elif` expressions
after a match are skipped. Do not infer GCC or Clang feature support from
a `__has_*` probe.

`-include HEADER` is repeatable. Kbuild's `-Wp,-MMD,FILE` and `-Wp,-MD,FILE`
produce dependencies on the source and filesystem headers loaded during
preprocessing, including forced headers that emit no tokens. The main source
is the first prerequisite, as required by Linux `fixdep`. Options may appear
before or after the source; `--` ends option parsing.

Modeled standard headers expose only selected declarations and layouts.
`<stdio.h>` supplies variadic `printf`; `<stdbool.h>` models `bool` as
`unsigned char`, which is not the same as the `_Bool` type.
`<assert.h>` checks `NDEBUG` on each inclusion: a disabled assertion does
not evaluate its argument; an enabled assertion evaluates once and aborts
on failure. Common `<ctype.h>`, `<string.h>`, `<stdlib.h>`, and `<sys/stat.h>`
declarations are modeled, with Linux x86-64 coverage for `passwd`, `dirent`,
`utimbuf`, and `rusage`. Finding a system header does not establish support for
all its declarations or for another libc ABI.

`__NYC__`, `__NYC_MAJOR__`, `__NYC_MINOR__`, and `__NYC_PATCHLEVEL__`
identify the frontend. `nyc --version` reports the build identity. The
frontend defines `__STDC__` and advertises GNU compatibility integers
`12.0.0`; it does not define `__clang__`. These are preprocessor contracts,
not full GCC or Clang compatibility.

## C language subset

### Scalars, pointers, and arrays

- Supported scalars include signed and unsigned integers through 64 bits,
  `float` and `double`. Narrow parameters, returns, assignments and memory
  operations preserve their declared width. Explicit signed and unsigned
  byte and halfword loads sign-extend or zero-extend respectively. Bare
  `char` is signed by default on x86-64, but some bare-`char` loads still
  zero-extend; use explicit `signed char` where high-bit interpretation
  matters. `-funsigned-char` selects unsigned plain `char`.
- Unsigned 32- and 64-bit operations preserve wrapping, comparisons,
  division, remainder, and logical right shifts. Suffixed 64-bit constants
  keep their unsigned bit pattern. The tested GNU integer builtins include
  unsigned overflow arithmetic, `__builtin_clz[l]`, and `__builtin_ctzl`.
- Typed pointers support arithmetic, dereference, indexed loads and stores.
  Object-pointer relational comparisons use unsigned address ordering.
  GNU `void *` arithmetic uses byte scaling. `_Bool *` remains a pointer
  through assignment, parameter passing and return.
- A pointer to a row such as `struct entry (*rows)[512]` advances by the
  entire row, including when a subobject address is taken.
  `&table[i][j]` on `struct entry **table` uses pointer size for the first
  subscript and record size for the second. Array parameters decay to
  pointers; i386 aggregate-pointer tables use four-byte elements.
- Address-taken floating locals retain stack-backed reads after the address
  escapes. A cast to `void` preserves side effects without producing a value;
  nonvolatile discarded lvalues do not force a memory read.

### Declarations, layout, and initialization

- Tagged and anonymous aggregates support nested records and arrays, pointer
  tables, bitfields, and promoted anonymous members. Bitfield updates keep
  neighboring bits; signed fields sign-extend, and unnamed fields retain
  padding and offsets. Taking a member's address does not read it.
- File-scope enums support tags, typedefs, explicit integer expressions, and
  implicit successors. Enumerators can appear in array extents, initializers,
  parameters and results. Constants must fit `int`; duplicate and empty
  enumerations are rejected. Only supported block-scope enum shapes work.
- Typedefs include supported scalar, pointer, aggregate, and function-pointer
  types, with chains and `const` qualifiers. Function pointers and
  pointers-to-pointers can be stored, passed, returned, or used through
  supported indirect calls. GNU `asm` labels on external declarations are
  parsed; using those aliases as call targets, data addresses, or initializer
  values remains limited by the applicable symbol path.
- `sizeof`, alignment queries and `__builtin_offsetof` use resolved layouts
  and the target's unsigned `size_t` width. File-scope `_Static_assert`,
  GNU `typeof`, scalar/pointer `__auto_type` and C23 `auto` initializers,
  `__builtin_choose_expr`, `__builtin_types_compatible_p`, GNU scalar statement
  expressions, and switch case ranges have targeted coverage.
- File-scope scalar, pointer, and aggregate objects support the implemented
  constant initializers, including string data, local-static function and
  pointer relocations, and selected self-referential aggregate members.
  Integer and pointer globals support updates; a pointer to `const` is not
  necessarily a const pointer. Static objects and functions retain local
  binding across translation units. Static locals retain storage and
  addresses across calls; block-scope `extern` resolves to the C symbol.
- Fixed arrays retain their declared size; missing elements zero-initialize.
  Character arrays accept concatenated strings, zero padding, and exact-width
  strings without a terminator. Supported incomplete pointer and character
  arrays can be completed by compatible definitions. GNU zero-size outer
  bounds and empty initializers retain size zero. Not all nonzero global
  aggregate-pointer array initializers are supported.
- Supported runtime aggregate initialization includes nested brace lists,
  bitfields, arrays, and aggregate compound literals. Omitted fields and
  elements zero-initialize on each evaluation. Automatic fixed-size storage
  is reserved once per invocation, but initialization still runs at the point
  of evaluation, including within loops.
- Natural object alignment and accepted GNU `aligned` and `section`
  attributes affect emitted storage, including prefix attributes before
  `__typeof__` and trailing or inherited record alignment. On ELF, const data
  uses `.rodata` where no dynamic data relocations are required, and
  `.data.rel.ro` for relocatable const data unless PIC/PIE was explicitly
  disabled. An explicit section takes precedence.

GNU `nothrow`, `leaf`, `cold`, `hot`, `malloc` (including the deallocator
form), and `access` attributes are accepted but do not imply GCC-equivalent
optimization or analysis. Explicit aliases, bitfields, and unnamed or nested
aggregate declarators each have shape-specific limits.

## Targets and output routes

| Route | Output and boundaries |
| --- | --- |
| x86-64 ELF | `-c` emits objects; `-S` emits GNU assembly. Object output can use the register-allocated machine encoder, a direct NYIR encoder, or GNU `as` for supported assembly forms the object writer cannot encode. |
| i386 ELF | `-m32` selects 32-bit code. NYC can emit assembly or assemble it with GNU `as` for `-c`. `-mregparm=0` through `3` selects the integer argument register convention; match it on both sides of a call. |
| x86 real mode | `-m16` emits `.code16gcc` assembly or an ELF32 relocatable object. It keeps the 32-bit C ABI; it does not produce a boot image or switch the processor into real mode. |
| Other targets | Support is shape- and mode-dependent; see [Native object support](native.md#nyir-artifacts-and-native-objects) and the detailed records below. |

### Aggregate and variadic ABI

| Target ABI | Supported classes and boundary |
| --- | --- |
| SysV x86-64 | INTEGER/SSE combinations through two eightbytes; larger values use memory classification. Mixed returns use RAX and XMM0 in either class order. Unsupported layouts/class shapes are rejected. |
| AAPCS64 ELF | Homogeneous aggregates of one to four `f64` fields and integer aggregates through 16 bytes, including 16-byte alignment; larger values use memory classification. AArch64 Windows is unsupported. |
| Win64 x86-64 | One-eightbyte INTEGER aggregates are direct; larger by-value arguments and returns use the indirect ABI. The direct field model is limited to supported eight-byte shapes. |
| SysV i386 | Stack aggregate arguments and hidden result pointers, including register-placement variations from `-mregparm=N`. |

The acceptance matrix tests cross-compiler aggregate calls, including
AAPCS64 under QEMU and Win64 under Wine when available. This C matrix is
separate from checks for Nytrix `layout` values and the NYIR JIT.

On x86-64, scalar varargs obey default promotions. Variadic definitions
use the SysV register-save and overflow areas; `-mno-sse` preserves integer
varargs without touching vector registers and rejects floating-point or
SIMD operations requiring them. SysV `va_arg` accepts supported INTEGER,
SSE, mixed two-eightbyte, and MEMORY aggregates with alignment up to
16 bytes. If either register bank lacks space, extraction uses the overflow
area without advancing either bank. Aggregate layouts outside the
classifier's supported subset remain unsupported.

On i386, `-mregparm=0` through `3` and the GNU `regparm(N)` attribute select
integer argument registers, including supported small aggregates and 64-bit
register pairs. Floating scalars and single-real-member aggregates stay on
the stack. An aggregate too large for the remaining bank also sends later
integer arguments to the stack. A register-passed hidden result pointer is
not callee-popped. Variadic arguments stay on the stack, with pointer-shaped
`va_list`, `va_copy`, and mixed-width extraction. With nonzero regparm,
variadic aggregate return uses GCC's caller-pop convention rather than
Clang's default. i386 x87 conversion retains the full integer width and
restores the caller's control word after truncation.

## Code generation flags

`-O0` through `-O3` select the NYR level (default `-O2`). `-Os` uses a
size-oriented `-O2` configuration that disables costly growth passes;
`-fconserve-stack` caps the optimization level at `-O1`. At `-O1` and above,
eligible locally bound definitions can inline even if their out-of-line
symbols are retained. Interposable PIC globals, weak definitions,
`noinline`, variadic-list operations, and assembly definitions that would
duplicate global symbols constrain inlining.

Whole-unit x86-64 `-fPIC`/`-fPIE` use GOT-relative data addressing and PLT
call relocations where required. `-fomit-frame-pointer` supports RSP-relative
object frames for leaf and calling functions with spills, callee saves,
and incoming stack arguments. Dynamic `alloca` and variadic-list operations
can retain a frame pointer. The single-function route rejects omission;
physical assembly and direct-object parity without the stack encoder
remains unfinished.

`-m64`, `-march=x86-64`, and `-mcmodel=kernel` select or require x86-64.
`-mcmodel=small` accepts the default model. The linker checks the range of
RIP-relative relocations. `-falign-functions=16` selects the supported object
entry alignment. `-fshort-wchar` controls `wchar_t` and the corresponding
predefines; the normal Linux x86-64 wide character types use 32-bit ABI
widths. `-fno-strict-aliasing` matches the frontend's conservative aliasing.
Accepted warning, unwind, or recognized compatibility flags do not provide
GCC-equivalent diagnostics or metadata. A supported stack-boundary option
is target-specific; do not infer i386 frame rules from the x86-64 settings.

### Stack protection and indirect calls

Whole-unit SysV x86-64 supports `-fstack-protector`,
`-fstack-protector-strong`, `-fstack-protector-all`, and `-fno-stack-protector`.
Last option wins; `no_stack_protector` opts a function out. Basic protection
covers qualifying frame objects; strong includes address-taken locals, and
all includes scalar functions. Normal returns check a fixed frame word and
call `__stack_chk_fail` on mismatch. Other target ABIs and single-function
compilation reject enabled stack protection.

The default guard uses `%fs` offset 40. `-mstack-protector-guard=tls|global`,
`-mstack-protector-guard-reg=fs|gs`, `-mstack-protector-guard-offset=NUMBER`,
and `-mstack-protector-guard-symbol=IDENTIFIER` select the guard source. A
TLS symbol overrides the numeric offset. `__SSP__`, `__SSP_STRONG__`, or
`__SSP_ALL__` is defined before explicit macro overrides. Guard support is
not general FS/GS address-space support.

`-mindirect-branch=thunk-extern` requires the linked indirect-call thunk
`__x86_indirect_thunk_r11`; `thunk-inline` emits a local retpoline. The
single-function route rejects these options. For other implemented x86-64
protection and instrumentation modes, see the evidence below; `-pg` and
`-mfentry` are distinct from unsupported kernel tracing options.

## GNU assembly

`-x assembler` and `-x assembler-with-cpp` accept files or standard input.
GNU `as` assembles objects; `-Wa,--version` queries it and
`-Wa,--fatal-warnings` forwards fatal-warning behavior. File-scope assembly
supports common section changes, alignment, data directives, local labels,
and cross-section PC-relative relocations. Supported operand-bearing inline
assembly is template- and target-dependent; an arbitrary GCC operand or
constraint combination is not guaranteed to work.

The x86-64 GNU C template `leaq %c1(%%rip), %0` with an `"i"` symbol operand
emits a direct PC-relative address even under `-fPIC`. If an inline helper
passes the operand, it must resolve to a link-time symbol after inlining.
Ordinary interposable PIC references can still use the GOT. Supported forms
also include `pause`, `nop`, empty templates with a memory clobber or a
matching input/output tie, selected `=rm` forms such as `pushf; pop %0`,
input-only `"i"` expressions, target-specific string operations, and
supported asm-goto constraints. Volatile reads of a global `rsp` register
binding are supported; taking its address and writing it are not. The
read-write constraint used for call clobbers is limited to a single direct
call. See the evidence below for tested operand shapes and register,
segment, annotation, and encoding limits.

## Linux kernel status

Protected Linux 7.3.5 builds have linked `vmlinux`, the compressed kernel,
and configured modules using NYC for target C and GNU `as` for assembly.
Those runs did not produce a bootable `bzImage`. The latest setup replay
meets the 64-sector limit but fails the separate 32 KiB setup memory limit.
QEMU's direct PVH ELF boot is useful for diagnosing startup, but it is not
a `bzImage` or userspace boot test. See the
[verification evidence](#verification-evidence) for the remaining gates and the
[changelog](../CHANGELOG.md) for user-visible changes.

## Remaining acceptance

NYC is accepted for the kernel only when every gate below passes. Until
then, the [verification evidence](#verification-evidence) records partial
results; it does not replace these gates. For each fix, record the defect,
change, regression, and evidence in this specification and summarize
user-visible changes in the [changelog](../CHANGELOG.md).

### Required acceptance

- [ ] From unmodified upstream Linux 7.3 sources and a fresh output tree,
  produce x86-64 `vmlinux`, `bzImage`, and all configured modules. Compile every
  target C translation unit with NYC, including dependency generation; GNU
  `as` may assemble the output and the host compiler may build host tools.
  No target-C fallback, stale output reuse, GCC dependency-generation
  workaround, or modified kernel sources count.
- [ ] Fit both upstream real-mode setup constraints. Boot the resulting
  protected `bzImage` in QEMU and verify initramfs `/init` executes. Direct
  PVH ELF boots and focused relinks are diagnostic, not acceptance.
- [ ] Review the complete compiler, assembler, linker, modpost, and `objtool`
  diagnostics. Fix unsafe output and optimizer verifier failures without
  suppressing warnings or removing configured protections.
- [ ] Record the configuration, source and compiler identity, toolchain,
  exact commands, configured target object inventory, and compiler provenance
  for every object. Count actual target C units; classify failures,
  unsupported cases, and any workarounds. Keep the accepted build and QEMU
  smoke test as a permanent regression.
- [ ] Establish assembly/direct-object/physical-machine parity over the native
  benchmark set, including unsigned and 16/32-bit forms. Support PIC,
  retpolines, named sections, stack guards, and frame-pointer omission without
  the stack encoder, then remove the obsolete fallback.
- [ ] Close the open C boundary cases: remaining complex forms, kernel tracing
  flags, direct-object assembly annotations, and incomplete FS/GS address-space
  forms and execution coverage. The specification records the narrower verified
  subsets.

### Correctness verification programme

- [ ] **P0 — Harness.** Audit the existing test, differential, and benchmark
  lanes for false positives, truncated exit comparisons, matching failures,
  stale output, silent exclusions, fragile expected failures, and duplication.
  Distinguish pass, miscompile, compile/link/runtime failure, timeout,
  unsupported, and invalid test. Inject faults to prove the harness detects
  wrong output, bad assembly, link failures, crashes, timeouts, and stale
  binaries; every real regression must fail the test command.
- [ ] **P1 — Differential execution.** Compare NYC/NYR, NYC/LLVM, GCC, and
  Clang at supported optimization levels on well-defined, deterministic C.
  Compare full output, status and signal, binary and memory results, exact
  integers, appropriate floating-point semantics, ABI results, and side
  effects. Separate compile acceptance from execution coverage; resolve
  oracle disputes against the language contract.
- [ ] **P2 — Automated bug discovery.** Reuse existing generators, torture
  suites, or fuzzers for integer conversions, aliasing/casts, aggregates and
  bitfields, variadics, GNU attributes/asm, atomics, SSA/CFG, scalar and loop
  optimization, register allocation and spills, and ELF/linking. Retain
  deterministic seeds, rule out undefined behavior, minimize mismatches to
  the first divergent stage, and add focused regressions by root cause.
- [ ] **P3 — Compiler invariants.** Check typed AST/IR, SSA dominance and PHI
  edges, optimizer rewrites, machine liveness and allocation, spill and
  frame layout, ABI constraints, and ELF sections/symbols/relocations. Fail
  at the first invalid boundary. Enable expensive checks in test and debug
  builds without imposing them on normal compilation.
- [ ] **P4 — Conformance.** Pair positive execution and negative diagnostic
  tests for supported C and GNU C constructs encountered in Linux, SQLite,
  system headers, and other real workloads. Do not count untested features.
- [ ] **P5 — System acceptance.** Fulfil the protected kernel artifact,
  provenance, diagnostic, and boot gates above.
- [ ] **P6 — Reproducibility.** Preserve compiler revision, flags, dependencies,
  target, checksums, seeds, object provenance, and reproduction commands.
  Consider self-hosting only if it serves the existing architecture; identical
  binaries are not proof of semantics.
- [ ] **P7 — Cost.** Keep fast deterministic checks separate from deep
  differential/fuzzing/sanitizer lanes and full system checks. Reuse the
  current harness, safely cache reproducible inputs, parallelize independent
  cases, retain failures, and report compiler time and peak memory against a
  stable baseline without slowing ordinary builds.

## Verification evidence

<details>
<summary>Build, boot, ABI, diagnostic, and code-generation investigations</summary>

The records below describe specific runs and compiler snapshots. Later
results supersede older failures only for the cases they actually exercise.
Paths under `/tmp` identify local run artifacts, not files shipped with NYC.

### Real-mode setup code size

The one-call inlining allowance applies to local definitions. An exported
definition still needs an out-of-line body, so it must meet the tiny-helper
limit even when the current translation unit calls it once.

Two NYC binaries built with the same backend objects replayed all 20 Linux
7.3.5 setup C recipes. The new rule reduced `cpucheck.o` text from 1,886
to 1,844 bytes through a smaller `check_cpu`. The other 19 text sizes
were unchanged. Both upstream setup links failed with `_end=0x88d0`.
The maps are in `/tmp/nyc-setup-size-matrix.b0hdjbqj`.

The i386 setup emitter materializes nonzero signed-byte constants with
`pushl` and `popl` when reloading a constant into a register. The pair
preserves flags and saves one byte over a 32-bit immediate move in this
mode. Across the 20 Linux 7.3.5 setup C recipes, text fell from 25,179 to
25,116 bytes. The upstream linker still rejects setup. The map in
`/tmp/nyc-setup-size-matrix.1fjcgstu/setup.map` places the end of
`.text32` at `0x6604` and `_end` at `0x88d0`.

The setup emitter also substitutes accumulator `xchgl` for register copies
when the source is a dead caller-saved register. Its size model accounts for
the one-byte accumulator opcode. This lowered setup C object text from
25,116 to 25,069 bytes across the same 20 recipes. The upstream linker
still rejects setup with `.text32` ending at `0x65d5` and `_end` at
`0x88d0`. The map is in `/tmp/nyc-setup-size-matrix.77abeyyi/setup.map`.
The separate real-mode QEMU probe emitted the accumulator exchange and
matched the reference marker `FOLD 149d8d80`.

The setup emitter folds register-indirect accesses with a constant
displacement into one symbol-relative memory operand when the base register
is dead after the access. The assembled CPU vendor loads retain
`R_386_32` relocations with addends of 4 and 8. A real-mode QEMU probe
executed both loads and matched the reference marker `FOLD 000003dd`.
Across all 20 setup recipes the change reduced text from 25,069 to 24,895
bytes. The upstream linker still rejects the result. Its map at
`/tmp/nyc-setup-size-matrix.xw_v83ep/setup.map` places the end of
`.text32` at `0x6527`.

The same fold can cross independent moves within a block and can encode
direct comparisons, tests, and immediate stores. The replay emitted
symbol-relative comparisons in `video.o` and byte, word, and dword stores
in the setup objects. Text shrank by 31 bytes to 24,864 bytes. In the
upstream link map at `/tmp/nyc-setup-size-matrix.to_rpa5u/setup.map`,
`.text32` ends at `0x6508` and `_end` stays at `0x88d0`.

When an address adjustment's flags are overwritten within the same block,
the emitter can add the adjustment to the relocation and drop the
arithmetic instruction. The real-mode QEMU probe performed a read-modify-write
through the folded address and matched the reference marker `FOLD 7645bbed`.
The 20 setup recipes fell from 24,864 to 24,830 text bytes. The upstream link
map at `/tmp/nyc-setup-size-matrix.7a4fu5qq/setup.map` places the end of
`.text32` at `0x64e6` and `_end` at `0x88d0`.

In setup mode the emitter folds flag tests into a stack slot when the
loaded register is dead. The 20 setup recipes dropped from 24,830 to
24,780 text bytes. A real-mode probe linked the actual Linux setup
`string.o` and verified `simple_strtoull` across hexadecimal, octal,
decimal, and 64-bit inputs with end-pointer checks. Both builds printed
`FOLD 0000000f`. The C frontend suite passed. The upstream link still
fails with `.text32` ending at `0x64b4` and `_end` at `0x88d0`.
See `/tmp/nyc-setup-size-matrix.a39ou9pk/setup.map`.

Extending the comparison fold from stack slots to ordinary indirect operands
saved 14 more bytes. The setup objects contain direct comparisons of CPU and
video data with the same memory width. The upstream link still fails with
`.text32` ending at `0x64a6`. See
`/tmp/nyc-setup-size-matrix.ts3d1tr1/setup.map`.
The real-mode QEMU probe exercised an indirect signed comparison against
both negative and positive values and matched `FOLD 00001907`.

The C frontend preserves volatile qualification on file-scope integer
arrays and carries it through indexed loads, stores, and multidimensional
rows. Before the fix, `-Os` could forward a write into a subsequent volatile
read and drop discarded-value reads. The `volatile-global-array.c`
regression checks optimized NYIR in both x86 modes for all expected
32-bit reads and a single 16-bit store and load. The full C frontend suite
passed. Replaying the Linux setup recipes with the qualifier fix preserved
their text sizes.

For real-mode setup at `-Os`, the frontend now allows scalar promotion in
straight-line functions. Promotion across control flow remains disabled
because the stack emitter materializes PHI edge copies. With the same
backend objects, unrestricted promotion grew the 20 setup objects from
24,768 to 25,452 text bytes. Straight-line promotion reduced them to
24,680 bytes. The QEMU probe using Linux setup `string.o` still reports
`FOLD 0000000f`. The upstream linker still rejects setup with
`.text32` ending at `0x6450`. See
`/tmp/nyc-setup-size-matrix.ytno6jox/straight.map`.

Setup size builds now promote individual locals when pruned SSA placement
finds no required PHI for that local, including in branching functions.
The SSA regression checks both paths through a join and verifies that the
local needing a PHI stays in memory. The 20 setup objects dropped from
24,680 to 23,950 text bytes. The `string.o` QEMU probe and C frontend suite
passed. The upstream setup link still fails with `.text32` ending at
`0x6176`. See `/tmp/nyc-setup-size-matrix.a0bqofhq/setup.map`.

The compiler was also tested with experimental PHI edge budgets. Selecting
the smallest assembled object per translation unit landed at 56 sectors
and `_end=0x78d0` with the original upstream linker script. That diagnostic
selection used budgets of zero, two, and three. See
`/tmp/nyc-setup-size-matrix.wqkdqfaw/smallest.map`.
The resulting setup, packaged with the saved NYC compressed kernel, produced
a diagnostic `bzImage`. QEMU emitted no serial output or `/init` marker.
A monitor sample found CPU 0 executing BIOS code in real mode. Image and
logs are in `/tmp/nyc-mixed-bzimage.XXRa2r`.

With early serial output enabled, the diagnostic image reported an
undefined normal VGA mode. Sending Space at the BIOS video menu let it
decompress Linux and enter 64-bit startup. A page fault at
`__pi___startup_64+0xb5` came from a store through a linked virtual
address before that address was mapped. The startup recipe forced
`include/linux/hidden.h`, but NYC discarded its visibility pragma.
The object used a GOT-relative address that the kernel link relaxed to
an absolute immediate.

The C preprocessor now tracks nested GCC visibility scopes. SysV x86-64
addresses of hidden declarations use PC-relative addressing and the
assembly output carries ELF hidden visibility. The regression checks
hidden and default declarations in the same object. Exact Kbuild recipe
replays for `map_kernel.o` and `gdt_idt.o` emitted zero GOT-relative or
absolute relocations. The corrected `__pgtable_l5_enabled` relocation is
`R_X86_64_PC32`. Hidden metadata for instruction references is emitted
with the surviving instruction. Internal-linkage declarations and
optimized-away references exposed spurious undefined hidden symbols in
the first two relinks.

A diagnostic relink with those unused symbols removed cleared the early
page fault. It reached the filesystem initcalls and stopped in
`xas_load+0x21d` while initializing iomap. The saved `xa_is_sibling`
used a signed comparison of a tagged address against `0xfe`, admitting
an address with the top bit set as a sibling index. NYC now lowers
ordered opaque object-pointer comparisons using unsigned address order.
The executable parity fixture exercises both operand orders. The boot
trace is in `/tmp/nyc-hidden-relink.43jslooo/serial.log`.

The usual arithmetic conversions zero-extend both operands of an unsigned
32-bit comparison. NYC uses their signed 64-bit ordering directly and
reserves sign-bit bias for full-width unsigned values. The width parity
fixture covers ordering across `INT_MAX`. With the same backend as the
previous replay, this moved the stock setup link's `.text32` end from
`0x6107` to `0x60f1`. The upstream limit is `0x6000`.

The real-mode emitter folds an immediate push, register pop, and
frame-home store into a push and direct memory pop when the caller-saved
temporary is dead. A compiler rebuilt with only this fold disabled
provides the control. The fold saves 46 bytes and moves the end of
`.text32` from `0x6102` to `0x60d4`.
The C frontend suite passed and the real-mode QEMU string probe printed
the same `FOLD 0000000f` marker for both compiler variants.

The i386 backend now compares its register-allocation candidates after
frame finalization, using the shorter accumulator encodings in its size
estimate. Real-mode size inlining uses a tighter work budget and keeps
ordinary calls and register-constrained inline assembly inside the
helper that contains both. The 20 default setup recipes now link with
the upstream script. The linked setup uses 56 total sectors,
`.text32` ends at `0x5fee`, and `_end=0x78d0`. The diagnostic
bzImage reused the saved kernel payload and reached initramfs unpacking.
It still stalled during serial IRQ probing.

The saved APIC interrupt object's alternate-stack branch switched the
stack and called the timer handler, then restored the stack directly.
The inline assembly matched both the IRQ wrapper and plain stack-call
shapes. NYC selected the plain form, which dropped `irq_enter_rcu` and
`irq_exit_rcu`. The frontend now chooses the IRQ wrapper when both calls
are present and diagnoses an incomplete pair. The C regression checks
the ordered calls in both shapes. Replayed `apic.o` and `irq.o` carry
both IRQ boundary call relocations on their alternate-stack branches.

That relink ran the serial IRQ probe to completion, then exposed a
workqueue fault. The worker pool pointer was 16 slots past the
per-CPU array base because `bool highpri = wq->flags & WQ_HIGHPRI`
retained the mask value `16`. Automatic scalar `_Bool` initialization
and assignment now use truth conversion before narrowing. The
workqueue replay emits a compare and `setne` before computing the pool
address. The C parity regression covers a masked array index,
a high-bit assignment, a compound assignment, and floating inputs.

With the corrected IRQ and workqueue objects and the default setup
objects, the focused protected bzImage ran the initramfs `/init`. The
serial log contains `NYC_KERNEL_INIT_REACHED` at the end of the boot.
The run used a Space key to continue through the BIOS video prompt and
reused other kernel objects. See
`/tmp/nyc-hidden-relink.y4m9fybn/serial.log`.

An assembler size comparison forced each of the nine existing i386 register
allocation variants across all 20 setup C recipes. The smallest measured
variant per function would save only nine bytes over the current selector.
Scoring after frame finalization saved five bytes in a separate trial. The
selector is retained. Further setup reductions need smaller emitted
instruction sequences. The assembled variants are in
`/tmp/nyc-setup-size-matrix.vihb2r5k`.

### C frontend harness: artifact freshness

The common executable regressions in `src/code/test/lang/run.cmake` remove
the previous output before each compile and link, then require a nonempty
artifact after a successful command. Previously, a compiler or linker that
exited zero without writing anything could pass by reusing the last run's
output.

Fault injection with known-good preexisting artifacts rejected missing and
empty compiler and linker outputs on both the assembly and object routes.
The assembly route also rejected a nonzero compiler exit, a nonzero linker
exit, a wrong program exit, a signal, and a timeout. This evidence applies to
the common executable helpers, not to every differential or benchmark lane.

The width and mixed-shape regressions exercise assembly output, explicitly
selected direct objects, and the default machine object route. The machine
route must show a disassembly of `main`. The retpoline checks require one for
the variadic indirect caller. Named sections are checked in both object
modes. The forced-include and preprocessing checks require fresh dependency
files. Fault injection rejected missing or empty assembly, invalid assembly,
link failure, stale executables, wrong output or status, a crash, a timeout,
and silently bypassed machine output.

The cross-target C frontend checks now require fresh nonempty AArch64,
Win64, COFF, and Mach-O artifacts. The unsupported-input cases require
exit status 1, the case-specific diagnostic on stderr, and no object.
Previously the runner ignored the expected diagnostic field and could accept
an unrelated rejection. `ctest --test-dir build/release -R '^nyr-c-frontend$'
--output-on-failure` passed with the existing release compiler. Repeating
the runner with fault-injecting tools rejected a stale AArch64 object, a
stale COFF executable, an unrelated diagnostic, and a crash that printed
the expected diagnostic. Logs are in
`/tmp/nyc-cross-target-faults.6bm28mpw`.

`bench-gcc` now requires a successful exit and exactly one checksum from
each executable before it measures instructions. Fixture paths identify
build outputs, so cases with the same basename cannot overwrite each
other's inputs or binaries. Fault injection verified nonzero command
status for two missing checksums, matching nonzero exits, a signal,
missing object or executable output, a checksum mismatch, an unavailable
instruction count, and an unknown fixture. Two passing fixtures with the
same basename were both reported. The benchmark function passed
`memory/bitcount` for NYC and NYR with the existing release binaries. The
normal command's rebuild stopped at compile errors in the pending
`loop_vectorize.c` changes.

The two parity programs and six retained frontend stress programs were run
with GCC 16.2.1, Clang 23.1.5, and three NYC routes at `-O0`, `-O1`, `-O2`,
`-O3`, `-Os`, and `-Oz`. All 240 executions agreed on status and every byte
of stdout and stderr. The report at
`/tmp/nyc-retained-differential._8ctn54u/results.json` records commands, input
hashes, and outputs. Its machine-preferred object route may select the direct
writer for individual functions.

### Current build evidence

A fresh protected Linux 7.3.5 attempt in
`/tmp/nyc-nov20-kbuild.QxR9yW` used the upstream tarball and
`NYC_KERNEL_COMPAT=1 make ARCH=x86_64 CC=<frozen-nyc> HOSTCC=gcc AS=as LD=ld
-j12 V=1 --output-sync=target vmlinux modules bzImage`. It stopped on four
NYIR verifier failures in `skbuff.o`, `lz4_decompress.o`, `client.o`, and
`ttm_bo_util.o`. If-conversion can use a select of two boolean values as
the condition of a second select. The verifier now accepts that case and
continues to reject either arm if it can produce a non-boolean value.
All four saved Kbuild commands then produced new objects and dependency
files in `/tmp/nyc-fixed-select-replay.fpckwxqa`. The setup replay with the
earlier frozen binary reached 64 sectors and `_end=0x88d0`, above the
`0x8000` limit.

The protected configuration requests `-fpatchable-function-entry=16,16`
with `CONFIG_CALL_PADDING=y`. A focused C object probe confirmed that
kernel compatibility mode accepts the flag without emitting the padding
or the `__patchable_function_entries` section. GCC put the function at
offset `0x10` and emitted that section. NYC put it at offset zero. This
configured protection remains an acceptance blocker.

The fresh retry in `/tmp/nyc-protected-retry.OyKdYd` used the same upstream
sources and a new output directory. It linked `vmlinux`, the compressed
kernel, and all 12 configured modules. All 3,002 successful NYC C object
messages match unique saved Kbuild recipes one for one. Each object exists
and each recipe passes NYC the dependency flag. All 19 GCC C recipes build
host tools. The run records 61 objtool warnings (53 redundant UACCESS
disables and eight calls leaving `.noinstr.text`) and 18 modpost section
mismatches. GNU ld rejected `setup.elf` at `Setup too big!`, so no
`bzImage` was available for the QEMU smoke test. Replaying the setup
recipes with that compiler also gave 64 sectors and `_end=0x88d0`.
The run directory contains `command.txt`, `inputs.sha256`,
`object-provenance.json`, `warnings.json`, and the build log. The frozen
compiler SHA-256 is
`45d9eabe8156b2becc9e35965426828aa35151da37b3b19c4019126551cf5b74`.

Fresh Linux 7.3.5 runs used the upstream tarball (SHA-256
`8f293f83d54f0b7b9be56863ebc5e4ea79d73e530b4ae55c572be97ef39f0990`),
a new `O=` directory per run, NYC as `CC`, GCC as `HOSTCC`, and GNU `as`.
The protected configuration selects IBT, retpolines, return thunks, strong
stack protection, objtool, ORC, and modules. Earlier runs stopped on duplicate
GNU `extern inline` and weak functions, vDSO relocation failures, modpost
export records, and generated `__ksymtab` assembly. The fresh
`final-out.MgJ5r6` build linked `vmlinux`, `arch/x86/boot/compressed/vmlinux`,
and every `.ko` in its 12-entry `modules.order`. Its log records 3,001 distinct
NYC target C object outputs, 66 objtool warnings, and 40 modpost warnings.
The 3,001 output paths matched Kbuild's saved C object recipes exactly:
every listed object existed and every recipe named the frozen NYC executable.
The 19 GCC C object recipes were host tools under `scripts/` and
`arch/x86/tools/`. This audits the completed build stages, not an image.
Constant control-flow cleanup removed unreachable per-CPU size fallbacks
and disabled AMD memory-encryption references without changing the config.

The later protected `latest-clean-bzimage.UYrbzx` build linked `vmlinux`,
the compressed kernel, and 12 configured modules. Its log records
61 objtool warnings and 18 modpost warnings. GNU ld
again rejected `setup.elf` at the memory assertion; there is no `bzImage`.
The object audit matched 3,001 recorded NYC target-C recipes to their
outputs and compiler messages, including one message interleaved with
another compiler's output. Four other messages were C-to-assembly
generators, not additional `.o` files. All 19 GCC C recipes were host
objects under `scripts/` or `arch/x86/tools/`; all 12 `.ko` files exist.

The only failed build target was the real-mode setup link. An exact replay
of the upstream linker script using the fresh build's setup objects still
gave 72 sectors (limit 64), `.text32` ending at `0x7cf8`, and `_end=0x98d0`
(limit `0x8000`). The image was not produced. The frozen compiler SHA-256
was `e32d914c8dcce5123303c2d698d6b140804f6ec5f757bcaf7312fda4b05a7547`;
the configuration SHA-256 was
`45506b09f39d30174e26d5ad5d13bd2cfff195302229a2c116f2a640f772f963`.
Allowing register runs to return directly and saving allocatable nonvolatile
registers once in the function frame reduced the same 20 setup C objects from
31,792 to 31,625 bytes by GNU `size`. The frontend suite and 24 forced i386
register-variant executions passed. The clean confirmation build
`notype-out.pKABGi` again linked `vmlinux` and all 12 modules; it reported
64 objtool and 24 modpost warnings. It stopped at the same setup link.

Direct pushes for scalar stack arguments, CFG-aware coalescing of private
homes, and preserving live nonvolatile registers across calls cut the 20
setup C objects to 29,429 bytes. Zero-extending byte/word masks, omitting
unneeded frames, and passing already-allocated scalar call arguments without
spilling cut that to 28,743 bytes. Emitting indirect calls and pushes from
their existing frame homes, retaining private local homes at `-Os` when
promotion would introduce edge copies, and using memory operands in wide
comparisons and multiplication reduced that to 27,191 bytes. An exact
replay of the 20 Kbuild C recipes now meets the upstream 64-sector limit,
but the separate setup memory limit still fails: `.text32` occupies
`0x6ae1..0x6aff`, `.pecompat` aligns to `0x7000`, and `_end=0x88e0`
exceeds `0x8000`. Before alignment, `.text32` must end at or below `0x6000`.
This replay honors the declared 16-byte alignment of `boot_params`; before
that fix `_end` was `0x88d0`. Omitting unused integer call-result stores
and retaining low-word results in EAX then shrank the linked `.text` by
417 bytes; `.text32` now ends at `0x695e` and `_end` is unchanged. All 32
i386 GCC cross-links passed across four `regparm` settings and four
emitter variants. A separate real-mode QEMU fixture passed
with both NYC and GCC: it checks a loop carrying distinct
values over branch joins and a 64-bit argument between two 32-bit arguments.
The four-byte stack calling convention also passed 16 GCC cross-links with
`regparm` 0–3 and tied or permuted register arguments. These are not kernel
boots.

For `-m16 -D_SETUP`, fixed frames without intervening register saves now
use `enterl` when the frame size fits its immediate. The existing real-mode
fixture executed those prologues successfully in QEMU for both `-c` and
`-S`. In the 20-object Kbuild replay, linked `.text` shrank by another
260 bytes. `.text32` ends at `0x685a`, 2,138 bytes past the `0x6000`
alignment boundary. The replay still uses 64 sectors and
GNU ld still rejects `_end=0x88e0`.

Folding adjacent absolute symbol loads and stores shortened the linked
`.text` by 60 bytes in an A/B replay of the same 20 setup recipes using
one frozen compiler (`NYC_DISABLE_I386_ABSOLUTE_MEMORY_FOLD=1` for the
baseline). The fold uses an explicit address-size prefix and preserves
`R_386_32` relocations; it does not assume a 16-bit symbol address.
The QEMU real-mode probe passed with GCC and NYC `-S` and `-c`, checking
volatile byte and word access, sign extension, and dword updates.
The linked `.text32` now ends at `0x681e`, 2,078 bytes beyond `0x6000`.
The same A/B reports 64 sectors and `_end=0x88d0` in both runs; the
upstream setup memory assertion still fails. Allowing spill-home coloring
to use nonvolatiles already saved but unused by the function body did not
change setup size in a separate trial; that change was not retained.

A focused `arch/x86/kernel/cpu/vmware.o` replay removed the objtool
`return with modified stack frame` warning. The compiler now saves RBX
around the fixed-register VMware calls and represents the six output
addresses in IR, so optimized code consumes the returned values instead of
the locals' initial zeroes. The replay checked the generated stores and
unwind diagnostic; it did not execute a hypercall.

A separate clean `CONFIG_PVH=y` diagnostic build (all listed protections
retained) reaches the kernel's 64-bit startup through QEMU's direct ELF boot.
The first run triple-faulted while loading segment registers. NYC now passes
the packed descriptor's address to `lgdt`/`lidt` and emits a direct PC-relative
relocation for explicit `leaq %c1(%%rip), %0` asm even under `-fPIC`. The
next fresh PVH build passed the GDT/IDT setup and faulted after the initial
page-table switch. The PUD and PMD were sharing one page: NYC emitted an
8-byte subscript stride for a pointer to a 4,096-byte row. The row-stride fix
passed the C regression at `-O0` and `-O2`; a focused relink also crossed the
page-table switch in QEMU. Recompiling the final early-IDT load then let
the guest handle its initial page faults. It stopped on a generated SSE
register save in `_printk` while SSE was disabled, before `/init`.
The integer-only SysV variadic save now emits no SSE instructions in
`_printk` or `early_printk`. Register and overflow-stack arguments passed
under all three x86-64 encoders. The physical encoder also now advances the
`va_arg` cursor before assigning its result to a register, which prevented
the cursor from being overwritten by a result allocated to R11.
The next clean PVH `vmlinux modules` build linked all 12 modules and entered
`start_kernel` in QEMU with serial console output. It reported 62 objtool
and 24 modpost warnings. In that run, taking `&sections[root][slot]` used
an 8-byte stride instead of the 16-byte record size, so sparsemem initialized
only even-numbered sections and then dereferenced an uninitialized usage
pointer. Replaying the two affected Kbuild recipes with the stride fix let
QEMU pass sparsemem and reach per-CPU setup. The C frontend was dropping
the section attribute before `__typeof__(type)`, so the original linked
per-CPU section was empty. A fresh protected PVH build with the attribute
fix linked 187,056 bytes of per-CPU data and booted through per-CPU setup.
It stopped in slab initialization: an omitted pointer field in
`&(struct kmem_cache_args){.align = ...}` contained stale stack data.
Automatic record and record-array compound literals now zero omitted
fields on each evaluation. A focused `mm/vmalloc.o` replay moved QEMU
through slab initialization and into scheduler startup. A later focused link
with the workqueue, scheduler, and radix-tree objects rebuilt reached early
trace-event registration, then stopped on another slab constructor call.
Other objects in that link predate the literal fix. A clean PVH rebuild
stopped earlier on six C translation units: per-pass NYIR verification
isolated `inline_small` leaving a return capture without its call.
Keeping those call/capture chains intact fixed all six saved Kbuild recipes.
A standalone GNU C repro executes at `-O1` and `-O2`; the previous compiler
rejected it at both levels. At `-O3`, GVN also had to stop treating captures
from separate calls as equal values. A fresh protected PVH build after the
inliner fix linked `vmlinux` and all 12 configured modules and reached IRQ
initialization. The QEMU run reported early page-attribute and IRQ warnings
and did not reach `/init` before its timeout. This diagnostic does not
produce or substitute for `bzImage`.

The x86-64 text emitter was reusing the source's frame slot before a fused
bit test consumed it. Recompiling the three trace-event objects retained the
input until `TEST`. A focused PVH relink with those objects and rebuilt SMP
and IRQ objects removed the `top_trace_array` and
`smp_call_function_many_cond` warnings. `__irq_startup` still warned, and
the guest did not progress past early SRCU initialization before timeout.
Attaching GDB to that guest located the stall in
`set_normalized_timespec64`: the peephole range query walked a loop backedge
through the implicit function-entry edge and folded the loop's exit away.
The range query now stops at entry and ignores a condition when both branch
outcomes lead to the same block. The real `kernel/time/time.o` replay has
both guarded loops and both result stores. A standalone regression covering
positive, negative, and already-normal nanoseconds matched GCC and passed
NYC assembly and object emission at `-O0` through `-O3`.
With the repaired time object, the focused QEMU link passed timekeeper
initialization and registered the jiffies clocksource. It then faulted on
a null store in `numa_policy_init`, before `/init`. A block-local aggregate
declared with `typeof(value)` had followed scalar lowering and obtained a
null value instead of record storage. The standalone nodemask-shaped
regression now matches GCC at `-O0` through `-O3` for assembly and object
emission. The real `mm/mempolicy.o` recipe also recompiles.

The focused NUMA relink passed `numa_policy_init` and reached APIC timer
setup, then faulted switching to the interrupt stack. `common_interrupt`
loaded the per-CPU stack pointer without the GS prefix because its C
address was a pointer to an aggregate pointer. The corrected IRQ object
loads through `%gs`; the resulting aggregate pointer remains an ordinary
pointer. A Linux GS-base probe with distinct pointer slots and record values
passed in assembly and object mode at `-O0` and `-O2`; the prior compiler
selected the wrong slot in all four cases. Replaying the nine IRQ-stack
objects let the PVH guest take its timer interrupt, calibrate the TSC, and
enter CPU finalization. It then warned in `patch_retpoline` and hit the
`cpa_flush` assertion at `arch/x86/mm/pat/set_memory.c:472`.

The next replay restored array-typedef dimensions and signed row-distance
scaling for the retpoline thunk array. A standalone regression checked
both subtraction directions and both row and element addresses. Another
fix made x86 `=rm` inline asm outputs explicit memory writes, so the
real `cpa_flush` object reads the saved flags and tests IF instead of
using a constant. The array probe matched GCC at `-O0` through `-O3`;
the x86-64 and i386 asm probes matched GCC through those levels and at
`-Os`. Both `-S` and `-c` were exercised. A focused protected PVH replay
no longer warned in `patch_retpoline`, but `__irq_startup` still warned and
the corrected `cpa_flush` assertion still fired. Replaying the console
objects repaired one IRQ-state loss: GDB saw `nbcon_atomic_flush_pending`
restore IF and `arch_cpu_finalize_init` enter with IF set. A separate replay of
`arch/x86/kernel/cpu/mce/core.o` restored the IRQ state across `cr4_set_bits`;
GDB then found IF lost across `fpu__init_system` and, after replaying the
FPU objects, across `int3_selftest`. Replaying the notifier, trap, entry,
and RCU objects got past `cpa_flush` and into VFS initialization, where
the mixed-snapshot guest timed out after netlink and kobject warnings.

The fresh `pvh-irq-clean.fSBNfa` diagnostic build retained PVH, IBT,
retpolines, return thunks, strong stack protection, ORC, and modules.
`vmlinux` and all 12 modules linked in 607 seconds; the log contains
61 objtool warnings and 18 modpost warnings. All 2,971 saved NYC C-object
recipes matched existing objects and build output, including four
interleaved messages. All 50 GCC C recipes were host tools under
`scripts/`, `arch/x86/tools/`, or `tools/objtool/`. With all inputs
rebuilt, the first timer IRQ exposed a
different defect: `set_irq_regs` read the old pointer through GS but
stored the new pointer without GS, so `tick_periodic` dereferenced a
null `get_irq_regs()` result. Both the standalone statement store and
assignment-expression store now preserve the segment. The Linux GS-base
probe matched GCC for `-S` and `-c` at `-O0` through `-O3`, and the
C frontend regression lane passed. Replaying `arch/x86/kernel/irq.o`
confirmed a GS-prefixed load and store. The focused PVH relink passed
the first timer IRQ and got to SELinux initialization, where it still
faulted in `static_call_key`. No `/init` execution was observed.

At the SELinux fault, a GDB watchpoint showed `lsm_order_append` writing
an LSM identity into the adjacent static-call key. The earlier incomplete
`extern` declaration of `lsm_idlist` was merged with its tentative
definition without updating its size or shape. NYC now completes the
pointer array and emits its zero-initialized storage. `security/security.o`
changed from a zero-byte `lsm_idlist` to a 16-byte object in `.bss`.
The regression matched GCC in both output modes at `-O0` through `-O3`.
The resulting PVH relink passed SELinux initialization and timed out
in VFS setup after netlink and kobject warnings.

GDB then showed `slab_want_init_on_alloc` returning zero for flags
`0xdc0`, even though `__GFP_ZERO` (`0x100`) was set. The compiler had
truncated the integer mask to eight bits rather than converting it to
`_Bool`. Scalar returns from `_Bool` functions now use a truth comparison.
The GCC differential probe covered a high-bit mask, an unsigned-byte input
already the width of `_Bool`, a nonzero floating-point input, and signed
zero, using both NYC output modes at `-O0` through `-O3`.
Replaying the slab, kobject, netlink, and IRQ chip objects removed the
earlier IRQ-startup, netlink, and kobject warnings in that run. The guest
mounted rootfs and reached `rest_init`, then tried to call a null
`cgroup_can_fork` hook. `cgroup_init_subsys` was truncating the function
pointer for `(bool)ss->can_fork` instead of setting exactly one mask bit
when the pointer was nonnull. Explicit scalar `_Bool` casts now use the
same truth conversion as returns. Integer, floating-point, and function
pointer cases passed the GCC differential probe in both output modes.

Replaying the cgroup and task-creation objects passed the null callback
and reached the first scheduler context switch. It then faulted on the
local APIC timer path, which also inlines `set_irq_regs`. Replaying the
configured IRQ-register writers with the GS store fix let the guest start
`kthreadd`. The guest still warned about a corrupted preemption count
(`0x6aa9` in the APIC replay, `0x6b41` in the preceding run) and stopped
at `BUG_ON(pi && in_interrupt())` in
`__sched_setscheduler`. Disassembly showed a preemption-count `orq` and
`decq` without the GS prefix. Named `+m` asm operands now retain the
segment recorded on their C lvalue, including with a flag output. The
runtime GS-base probe matched GCC through `-S` and `-c` at `-O0` through
`-O3`; it checks that the operation updates the CPU-local slot, not the
unsegmented decoy. NYC still routes these asm forms through GNU `as` in
`-c` mode. The first kernel replay confirmed the GS-prefixed instructions
but still hit the scheduler assertion. Disassembly also found a plain
`cmpxchgq` in the preemption-count reset; named compare-exchange now
preserves the segment too. The GCC differential checks both success and
failure, including the returned expected value on failure, in the same
eight NYC modes. The strict C frontend lane passed. A second PVH replay
confirmed `cmpxchgq` through GS in `finish_task_switch`; neither its
preemption-count warning nor the `__sched_setscheduler` assertion recurred.
`kthreadd` started, but the guest hit RCU and idle-scheduler warnings and
timed out before `/init`. This is not a clean PVH or `bzImage` boot.

Replaying the architecture context-switch object restored the GS store
to `current_task` in `__switch_to`. A focused relink with that object and
the RCU and scheduler objects no longer showed the preceding RCU and idle
warnings. It reached a worker thread and stopped at a divide exception in
`avg_vruntime`, after the `set_next_entity` warning. GDB observed a saved
runqueue weight of `-0x100000`; adding the current entity's `0x100000`
weight made the divisor zero. `/init` did not run.

The clean `pvh-seg-fresh.fuYNGn` rebuild reproduced that worker-thread
failure without the earlier SMP, RCU, or idle-scheduler warnings. The
frozen NYC SHA-256 was
`18729b65db2c7af7e96ad4727f52b47ffd6d7c551bc9e0cdb099cea4a632d6a0`
and the protected PVH config SHA-256 was
`bf21689337389b522d3f0e33edbe22c1a450d5781349fb0a13358ae3fcf7dfb3`.
Kbuild used `NYC_KERNEL_COMPAT=1`, `ARCH=x86_64`, the frozen NYC as
`CC`, `HOSTCC=gcc`, `AS=as`, and `LD=ld`, first running `olddefconfig`
and then `-j20 V=1 vmlinux modules`. All 2,971 NYC target C recipes
matched both the compiler messages and their object files. `vmlinux`
and the 12 configured modules linked, with 61 objtool and 18 modpost
warnings. QEMU still timed out before `/init`.

A GDB watchpoint traced the negative runqueue weight to a second
`sched_change_end` call for the same guard in `__sched_setscheduler`.
The frontend compared the backing addresses of the local and label
spellings instead of their order after macro expansion, treating a goto
within the Linux `scoped_guard` as an exit. Cleanup edges now use token
positions. The protected `kernel/sched/build_policy.o` replay went from
two destructor call sites to one, and the strict C frontend lane passed.
A backward jump to a macro-expanded label before a cleanup declaration
also exercises the opposite case: the old compiler failed on `-S` and
`-c`, and the corrected compiler matched GCC in both modes.
With the scheduler objects relinked, QEMU no longer reported the fair
scheduler warning or divide exception. It unpacked the initramfs, freed
its 12 KiB input, and reached NFS driver initialization. GDB found PID 1
blocked later in `serial8250_init` → `probe_irq_on` → `msleep`, with the
boot CPU idle. The guest timed out without executing `/init`.

The fresh protected rebuild `pvh-cleanup-fresh.sxjZ8F` also linked
`vmlinux` and modules, using the same configuration and frozen NYC
SHA-256 `d630eed91fa72079716bafaff8bb67c8f51bb03a6bef6825a8cae655209b7e81`.
The QEMU run with `initcall_debug` timed out without the userspace marker.
Its last serial message announced `init_sunrpc`, but serial output alone
does not identify where PID 1 blocked. In the separate focused relink,
GDB saw the serial-probe sleep timer still queued after its expiry:
jiffies advanced, the timer softirq pending bit remained set, and the
timer wheel did not advance in two snapshots. APIC timer entry and earlier
timer callbacks were observed. The dispatch failure was not isolated to a
specific compiler defect.

Two Kbuild object replays now place the const `init_funcs` and
`memmap_ktype` tables in `.rodata`. Eight ELF compile-link-run checks across
assembly and object output verified const table placement, mutable
pointer-to-const storage, functioning callback relocations, and dynamic
relocation without text relocations. The focused Kbuild replays establish
section placement, not a clean modpost result.

The vDSO failure was an ELF symbol-index cache that survived sorting the
definition table. The same saved Kbuild C command, rerun with the cache fix,
produced an object which linked with the upstream vDSO linker script and
`ld --no-undefined`. That is a focused link, not a complete clean build.
The C retpoline fixture also passed all six combinations of external or
inline thunks with assembly, direct-object, and physical-machine emission;
each executable called both an integer target and a variadic floating-point
target. The frontend suite passed with hard optimizer rejection and with the
SCCP fix for stale facts after an inlined asm-goto edge. Scoped cleanups now
run on C and asm-goto exit edges; a focused `fs/readdir.o` replay eliminated
five objtool `return with UACCESS enabled` warnings, with redundant-disable
warnings still open. Mandatory static-branch inlining now precedes optional
stackless inlining so Intel PMU `asm goto` immediate operands are specialized
even in large files. Real replays of `lib/strnlen_user.c` and
`lib/strncpy_from_user.c` produced no objtool warnings.

`sysv-boundary` matches GCC output. `abi-cross-*` links NYC- and
host-compiled counterparts in both directions for INTEGER/INTEGER, both
mixed INTEGER/SSE orders, SSE/SSE, and MEMORY-class returns, including
`-S -fconserve-stack`. Returns allocate GP and SSE registers independently.
The fixes also let `vsprintf` keep its format pointer during the PVH boot.
The next early console fault was passing a `_Bool *` as scalar `_Bool`;
runtime tests now check pointer identity, writes, and conversion of a pointer
to a separate scalar `_Bool` parameter.

Code-generation semantics are verified with NYC itself in both `-c` and `-S`:
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
The object route attempts machine emission for eligible functions and uses
the direct frame encoder for functions that bypass machine lowering or
fail emission. The `mixed-shapes` test checks equivalent output and requires
a machine disassembly of `main`. The assembly-route machine text emitter
now encodes the C shapes it
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

### SQLite 3.46.0

The unmodified amalgamation compiled in whole-unit mode. With a GCC shell
and the NYC-compiled library, SQL output matched GCC across transactions,
joins, indexes, window functions, recursive queries, JSON and blobs, plus a
1,000-row on-disk database with updates, rollback, `VACUUM`, integrity
checks, and reopening. An NYC-compiled shell and core also printed the
version, created a table, inserted `42`, and read it back. Those are
functional checks, not a performance measurement or suite pass.

A prior `SQLITE_TEST` run passed `expert1.test` 89/89 and `intck1.test` 70/70,
then failed `alter-3.1.1` because casted aggregate indexing used an 8-byte
stride instead of the record size. With that fixed, the test-enabled core
linked to the upstream runner with compiler-rt and non-PIE flags and passed
`alter.test` 120/120 and `ext/intck/intck1.test` 70/70. Correct aggregate
local lowering for mixed pointer/record declarations fixed `autoindex1-900`;
`autoindex1.test` passed 37/37. A broader `extraquick.test` run still aborted
at `json101-4.2`, with Valgrind identifying an invalid free in
`jsonCacheInsert`; disabling register allocation reproduced it. The system
header Makefile route stopped on glibc `_Float32`; an earlier
`-U__GNUC__` attempt instead stopped on a mismatched `__isnanf` argument.
Full upstream SQLite acceptance is not established.

### Early Kbuild preparation

Earlier Linux 7.3 runs generated device-table offsets and time constants
with a host `bc`, passed preparation's modpost step, and preprocessed
`init/main.i` with dependency output. The isolated `fixdep.c` executable
matched GCC. Historical failures involved incomplete `sizeof` types in
`find.h`, `__u128_halves` initializers in `cmpxchg_64.h`, and inline-asm
operands in `bug.h`, `preempt.h`, and per-CPU code. Linux 6.9 preparation
also passed early inline-asm and array-declarator blockers before stopping
on `__builtin_object_size`. These are old preparation snapshots; the clean
build and artifact records above supersede them for Linux 7.3.5.

### Linux 7.3 kernel build attempt (2026-11-18)

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

At that attempt, three fallbacks remained: stack guards, retpolines, and
non-leaf frame omission. Retpolines now have the focused parity evidence
above; stack-guarded functions still reject `NYIR_STACK_GUARD_LOAD` and
`NYIR_STACK_GUARD_STORE` in machine lowering, and non-leaf frame omission
still needs a physical-machine prologue without `%rbp`. Compiling all 523 corpus C
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

Unmodified source: an upstream Linux 7.3 tree checked out outside the
repository.

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

#### Code-generation semantics probe (2026-11-17)

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

#### SysV aggregate/variadic audit (2026-11-17)

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

The passing call and scalar-`va_arg` cases are pinned by `sysv-boundary`.
The same audit found that aggregate `va_arg` only accepted one INTEGER
eightbyte. The frontend now checks both register banks before advancing
either cursor, reconstructs noncontiguous register eightbytes in object
order, and aligns overflow addresses before consuming the whole argument.
Scalar `NYIR_VA_ARG` is unchanged.

The `aggregate-variadic-va-arg` regression links a host caller against NYC
assembly and object output. It covers INTEGER/INTEGER, SSE/SSE, both mixed
orders, single-eightbyte records, register rollback in both banks, a
forwarded `va_list`, and 8- and 16-byte-aligned overflow arguments with
following scalar reads. GCC and Clang callers passed both output modes at
`-O0` through `-O3`, `-Os`, and `-Oz`; the previous NYC rejected the SSE
record. The same fixture is rejected under `-mno-sse` at the SSE aggregate
extraction.

### Verified continuation fixes

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

### Diagnostic history and unresolved families

`complex.h` still fails because the C frontend has no complex type (see the
2026-11-17 probe record). Physical machine lowering still falls back to stack
encoders, including for the new stack-guard operations. Kernel tracing flags
`-mrecord-mcount` and `-fpatchable-function-entry` are rejected rather than
implemented; `-pg` and `-mfentry` are implemented. The next full build must use a newly frozen
compiler and fresh output tree after these blockers are addressed.

Historical logs contained unreachable-instruction, missing-`noreturn`,
UACCESS-state, section-mismatch, and other `objtool` diagnostics. Classify the
complete current log and fix each unsafe family; earlier partial warning counts
are not a final review. See [Native code generation](native.md) for
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

### FS/GS address spaces: implementation record

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

Scalar increment and decrement now use the same segment-aware memory store
as assignment expressions. The x86-64 assembly emitter also keeps the
segment prefix on `movss` loads and stores instead of silently reading and
writing the default segment. The Linux `segment-aggregate-pointer-load`
runtime regression now checks unsigned halfword postfix decrement and float
prefix increment with a nonzero GS base, a decoy slot, and adjacent
sentinels. The previous compiler failed both checks. Results matched GCC at
`-O0` through `-O3` for `-S` and `-c`; this `-c` case uses GNU `as`.

Named file-scope integer scalars now retain the object segment separately
from a pointer's pointee segment. Direct loads, assignments, and
read/modify/write operations use that segment; `&name` keeps the offset and
carries the qualifier into a subsequent dereference. This works for
comma-separated external declarations, and incompatible scalar segment
redeclarations are rejected.

The Linux GS regression covers two comma-declared unsigned objects and a
halfword object. The old compiler touched the unsegmented decoys. The
corrected result matched GCC from `-O0` through `-O3` with `-fPIC` for
both `-S` and `-c`. A separate freestanding i386 execution check set FS
with `set_thread_area` and checked a named load, compound store and
address-of dereference against GCC. These tests do not establish support
for all segment-qualified declarators, native encoder parity, or kernel
boot.

### Kernel boot continuation: setup size, IRQ, workqueue, and xarray

These runs continue the protected Linux 7.3 builds above. Each item names its
run directory under `/tmp`.

- All 20 setup C recipes replayed with the default NYC compiler and
  linked with unmodified upstream `setup.ld`. The setup uses 56 total
  sectors, `.text32` ends at `0x5fee`, and `_end=0x78d0`. The
  map is `/tmp/nyc-setup-size-matrix.keti7sbz/setup.map`. A diagnostic
  image using those default setup objects unpacked the initramfs and
  reproduced the serial IRQ-probe stall. A fresh protected build with
  the compiler and config frozen is underway in
  `/tmp/nyc-protected-default.ZzOHHVtU`.

- The IRQ boundary fix restored both stack-switch calls in the saved APIC
  and IRQ recipes. The resulting image passed serial IRQ probing and
  exposed a workqueue fault. The high-priority pool selector retained
  the flag value 16 in a local `_Bool`, yielding an out-of-bounds
  array index. The frontend now canonicalizes automatic `_Bool`
  initialization and assignment. The workqueue replay emits `setne`,
  the C frontend suite passed, and the setup boundary stayed at
  `0x5fee`. A fresh protected compiler snapshot with both changes is
  building in `/tmp/nyc-protected-corrected.Fncoo4ZO`.
  The focused bzImage with both fixes reached `/init` and printed
  `NYC_KERNEL_INIT_REACHED`. This reused other kernel objects and
  passed the setup video prompt with a Space key. See
  `/tmp/nyc-hidden-relink.y4m9fybn/serial.log`.

- The earlier default setup replay ends `.text32` at `0x6107`, 263
  bytes beyond the first acceptable linker boundary. See
  `/tmp/nyc-setup-size-matrix.3q9ecc3v/setup.map`.

- Eliminating unnecessary sign-bit bias on already zero-extended
  unsigned 32-bit comparisons moved the isolated setup boundary from
  `0x6107` to `0x60f1`. The current full compiler replay ends at
  `0x6102` with concurrent optimizer changes included. See
  `/tmp/nyc-setup-size-matrix.dytb4del/setup.map`. The C frontend suite
  passed with operands above `INT_MAX`.

- Folding immediate push/pop/store chains through dead caller-saved
  registers into direct frame-home pops saved 46 bytes against a
  compiler rebuilt with only that fold disabled. The latest
  replay with concurrent backend changes ends `.text32` at `0x60d4`,
  212 bytes over the boundary. The real-mode QEMU string probe matched
  the baseline marker and the frontend suite passed. See
  `/tmp/nyc-setup-size-matrix.7gh83ouw/setup.map`.

- Replaying the startup units and `lib/xarray.o` with the corrected
  compiler and reusing the saved kernel objects produced a diagnostic
  bzImage. The iomap initcall returned 0 and initramfs unpacked. An
  unmodified serial-probe run left PID 1 sleeping in
  `probe_irq_on → msleep → schedule_timeout`. See
  `/tmp/nyc-hidden-relink.3o_iq089/pid1-stack.log`. The full C frontend
  suite, C lexer, backend IR acceptance, and shared API acceptance passed.
  A separate diagnostic boot bypassed that serial initcall and omitted
  the emulated NIC. The ATA IRQ then exposed a null list predecessor in
  `insert_work+0xca`, reached from `__queue_work` via the active pool's
  worklist. See `/tmp/nyc-hidden-relink.3o_iq089/serial-minimal.log`.
  Repeated live snapshots found the same queued PID 1 timer after its
  deadline had passed, with jiffies advancing between samples. A
  separate run confirmed timer softirq bit 1 pending while the
  registered `ksoftirqd/0` thread slept. See
  `/tmp/nyc-hidden-relink.3o_iq089/softirq-worker-stack.log` and
  `timer-stack-{1,2}.log` in the same directory.

- A diagnostic bzImage with fresh `softirq.o`, `timer.o`,
  `smpboot.o`, `apic.o`, and `workqueue.o` also reached the serial
  probe with timer softirq bit 1 pending and PID 1's timer past expiry.
  After the guest settled, QEMU reported the one-shot local APIC timer
  at zero with empty local APIC interrupt request and in-service
  registers. See `/tmp/nyc-hidden-relink.xed2pkay/pid1-softirq-stack.log`
  and `interrupt-controller.log` in the same directory.

- The fresh retry in `/tmp/nyc-continue-kernel-20261121042155` reproduced
  the setup link failure. Its diagnostic map at `/tmp/nyc-setup-map.txt`
  places `.pecompat` at `0x7000` and `_end` at `0x88d0`. With
  the following sections the same size, `.text32` must end at or before
  `0x6000` to move `.pecompat` back a page. A replay of the 20 setup C
  recipes at `-Os`, `-O1`, `-O2`, `-O3`, and `-Os` with size
  inlining disabled saved 929 bytes by selecting the smallest object per
  recipe. The upstream link still rejected the result. The replay map
  in `/tmp/nyc-setup-size-matrix.1lqg0d4r/smallest.map` shows
  `.text32` ending at `0x62d3` and `_end` still at `0x88d0`.

- Cross-target artifact freshness and case-specific negative diagnostics
  passed the C frontend suite and fault injection. `bench-gcc` now rejects
  matching failures and missing checksums and isolates duplicate fixture
  basenames. Both routes passed the `bitcount` smoke test using existing
  binaries. The normal rebuild is blocked by pending vectorizer changes.

- The width and mixed-shape lanes now require fresh artifacts and exact
  output. Fault injection caught stale executables, invalid assembly,
  compile or link artifact failures, wrong output or status, a crash,
  a timeout, and bypassed machine emission.

- The same configuration requests 16 bytes of function-entry padding.
  A direct NYC/GCC object comparison showed that NYC's kernel compatibility
  mode accepts the flag without emitting the padding or patch table.

- Closed four small boundary gaps found by the microbenchmarks. The
  built-in headers now declare `atof`, `posix_memalign`, `getppid`, and
  `CLOCK_MONOTONIC_RAW`. A conditional whose true arm is floating and whose
  false arm is an integer or another floating type (`c ? 1.5 : 0`) converts
  the false arm instead of failing; the reverse order (`c ? 0 : 1.5`) still
  fails closed. Function-scope `_Pragma` directives other than `GCC unroll`
  are ignored, as GCC ignores unknown pragmas. The `"+r"` inline-asm
  constraint already worked. The `mixed-arithmetic-conditional` fixture
  covers the first and last, and the C frontend suite passed in a private
  build under `tmp/lab/nyc32`.

- Fresh protected-style kernel retry (2026-11-22, Linux 7.3 from
  `tmp/lab/benchmarks/linux-7.3`, `x86_64_defconfig` plus `olddefconfig`,
  `KCFLAGS=-O2`, `NYC_KERNEL_COMPAT=1`, `-j32`, output tree
  `tmp/lab/kbuild-20261122/out`, strict NYC-then-`as` wrapper). The first
  pass found three defects, each now fixed and exercised by a replay of the
  failing unit:
  - Inline-asm templates that write C locals through `nyc_slots=` (the
    `__put_user_*` helpers, `rdmsr_safe`, `wrmsr_safe`) were not counted in
    the x86-64 slot table or kept in memory, so a register-promoted result
    slot failed emission (`lsm_syscalls`, `read_write`, `sunrpc/cache`,
    `events/core`, `events/intel/p4`, `apic/io_apic`, `cpu/aperfmperf`).
    The result slots are now parsed generically from the template.
  - GNU `as` writes no `.symtab` for a unit without symbols, so objtool
    rejected `lib/iomem_copy.o`, `mm/highmem.o`, `lib/zstd/common/debug.o`
    and `arch/x86/kernel/trace.o`. `-S` now emits a `.file` directive on ELF
    targets. The `empty-unit` check in the frontend suite covers it.
  - `memcombine` is the SLP vectorizer but ignored the no-vectorization
    switch, so word-copy runs became `vec4` loads and stores and the no-SSE
    guard rejected 76 functions (`io_nop`, `__sock_recv_timestamp`, ...).
    Both runs are now gated; `nyc_no_sse_copy4` fails without the gate.
  After these fixes all 3,005 target C units compile, `vmlinux` links, and
  the only remaining failure is the known setup size gate: GNU ld reports
  more than 64 sectors. Per-object, NYC `-Os -m16` code is roughly 1.5 to 2.5
  times the size of GCC's (for example `cpuflags.o` 995 against 389 bytes,
  `video.o` 4,151 against 1,776). NYC inlines `cpuid_count`, keeps values in
  frame homes behind a two-step `sub` frame, and uses 32-bit displacement
  forms. The gap lies in the 16-bit register allocation and frame code, not
  in any one function. No `bzImage` was produced, and objtool and modpost
  diagnostics were not reviewed.

- Setup size follow-up (2026-11-22). A per-unit harness
  (`tmp/lab/kbuild-20261122/setupsize.sh`) compiles the 17 real-mode setup
  units with NYC, GCC, and Clang from the recorded Kbuild commands and
  compares `.text`; `relink.sh` rebuilds only `arch/x86/boot` and relinks
  `setup.elf`. Totals went from 28,841 bytes (NYC), 13,318 (GCC), and 13,606
  (Clang) to 23,367 for NYC.
  - `-m16` now defaults to the four-byte i386 stack. Every call previously
    carried a `push ebx; mov ebx,esp; and esp,-16; ... mov esp,ebx; pop ebx`
    realignment (about 16 bytes). This saved 5,100 bytes.
  - `movl $sym, %r` followed by `addl $n, %r` fuses to `movl $sym+n, %r`, and
    a store directly followed by a reload of the same register and slot drops
    the reload (text peephole on i386 function bodies).
  With these, `.text32` ends at `0x5eb3`, `.pecompat` stays on `0x6000`,
  `_end` is `0x78c0`, GNU ld accepts the setup, and Kbuild produces a
  `bzImage`.
  - Two real i386 miscompiles surfaced while booting that image. Storing a
    pointer through a struct member (`c->name = p`) or to a file-scope
    pointer (`HEAP = p`) wrote eight bytes, clobbering the next field. The
    setup `video_vga` card lost its `set_mode` pointer, and the `HEAP`
    allocator clobbered its neighbors, which produced "Undefined video mode
    number: ffff" on every boot. Both stores now use the four-byte pointer
    form; `i386-pointer-member-store` runs the member case at every regparm.
  - The `bzImage` now passes the real-mode setup without the video prompt
    and enters the 64-bit kernel. Boot then spins in
    `acpi_tb_print_table_header` (called from `acpi_tb_parse_root_table`),
    with no `/init` yet; the same stop occurs with GCC-built setup objects,
    so it is a kernel-side miscompile, not a setup one.
  - Still open: NYC setup code is 1.75 times Clang's. The remaining cost is
    frame traffic (26% of instructions reference a frame slot) because the
    i386 allocator is local to straight-line runs and flushes at every
    targeted label, plus boolean materialization before branches and 64-bit
    high words on values that never use them. `long long *` indexing on i386
    scales by four instead of eight, because the pointer class does not
    carry the element size; the setup units do not use it.

- Kernel boot progress (2026-11-22). A fresh `x86_64_defconfig` build with the
  strict NYC-then-`as` wrapper compiles all 3,005 target C units, links
  `vmlinux`, and produces a `bzImage` whose setup passes the upstream limits.
  Booting that image in QEMU (`tmp/lab/kbuild-20261122/boot.sh`) now reaches
  PID 1 and the async SCSI probe at about 1.3 s of guest time. Five defects
  were found and fixed on the way, each by tracing a hang or fault to a
  replayed unit:
  - Inlining a callee whose body starts with a `while` put the formal stores
    after the loop-header label, so every iteration reset the formals
    (`acpi_tb_fix_string` never advanced, hanging ACPI table parsing).
  - Dead-store elimination dropped stores into a local that only an
    inline-asm memory operand reads. `load_direct_gdt` therefore executed
    `lgdt` on an uninitialized descriptor, which faulted in `ltr`. Inline-asm
    operands now count as escapes.
  - Pointer stores into struct members and file-scope pointers wrote eight
    bytes on i386 (see above).
  - The x86-64 slot table ignored `nyc_slots=` result slots of put_user,
    `rdmsr_safe`, and `wrmsr_safe` templates.
  - `memcombine` (the SLP vectorizer) ignored the no-vectorization switch.
  Frame size is the remaining blocker. Functions containing inline asm, which
  is most of the kernel, take the stack encoder, which keeps every live value
  in a frame home; its frames are 3 to 5 times GCC's. A 42-frame SCSI probe
  path (`sr_probe` through `try_to_wake_up`) then overflows the 16 KiB task
  stack. Interval coloring of cross-block values and loop-carried copies, and
  sharing of nested-block and inlined alloca storage, cut frames by 10 to 20
  percent (`resource_string` 2,048 to 896 bytes, `try_to_wake_up` 752 to 576)
  but not enough. Splitting shared constants per use made frames larger and
  was dropped. The real fix is to accept inline asm in the register-allocating
  machine form so those functions leave the stack encoder; `stack.guard.*`
  lowering for that form is in place. `/init` has not run.
  Debug switches added for this work: `NYC_DUMP_FUNC`, `NYC_DUMP_PASSES`,
  `NYC_STOP_AFTER`, `NYC_FRAME_STATS`.

- Kernel boots to `/init` (2026-11-22, later the same day). With the fixes
  below, the default-config `bzImage` boots in QEMU past the SCSI probe,
  starts a freestanding static `/init` from an initramfs, and that program
  writes `NYC_KERNEL_INIT_REACHED` to `/dev/console` and powers off
  (`tmp/lab/kbuild-20261122/boot.sh` with `INITRD=initrd3.gz`). A static glibc
  `/init` still fails: `exec` returns `-EFAULT` before the `clear_user`
  fix and, after it, the kernel faults zeroing an anonymous page whose
  direct-map alias lies in `.rodata` (`memset_orig` from `prep_new_page`). The
  allocator was not handed that page by `memblock_free_all` or
  `free_reserved_area`; the cause is still open.
  - A `typeof` of an integer literal always produced `int`. The syscall
    wrappers declare their register parameters as
    `typeof(__builtin_choose_expr(..., 0LL, 0L))`, so every `long` argument
    (user pointers, sizes) was truncated to 32 bits. Literals now take their
    type from suffix and magnitude (`typeof-literal-width`).
  - The `clear_user()` alternative called `rep_stos_alternative` with C-ABI
    registers; the helper takes its length in `%rcx` and returns the residue
    there. The call now goes through a thunk emitted into the unit.
  - Stack overflow on a 42-frame SCSI probe path was removed by: sharing
    frame homes by exact per-instruction occupancy, interval-coloring
    cross-block values, renumbering C local slots by interval coloring,
    sharing nested-block and inlined alloca storage, and `stack.guard.*`
    lowering for the machine form. Task-stack depth at the deepest probe path
    went from over 16 KiB to about 10 KiB of headroom.
  Still open from the acceptance list: the glibc-init fault above, the objtool
  and modpost diagnostic review, per-object provenance, a retained boot test
  in the suite, native parity and frame-pointer-omission without the stack
  encoder, and the remaining C boundary cases.

- Kernel boots a static glibc `/init` (2026-11-22, final pass). The two
  remaining faults behind the page fault in `memset_orig` were found by
  printing the faulting page's history from a patched copy of `mm/memory.c`
  and `mm/page_alloc.c` (the sources were restored afterwards):
  - A file-scope `uint8_t`/`char` array without an initializer lost its
    `aligned(N)` attribute, and inherited the previous declaration's value.
    `empty_zero_page` (`const uint8_t [4096] __aligned(4096)`) therefore sat
    unaligned in `.rodata`. The attribute is now kept (`global-byte-array-
    alignment`).
  - A non-zero integer passed as a `_Bool` argument was not normalized to 1,
    so `__vm_normal_page(..., pte_special(pte), ...)` saw the low byte of
    `0x200` as false, treated the shared zero page as an ordinary page, and
    freed it into the page allocator after copy-on-write. Integer arguments
    and values stored to `_Bool` struct members are now converted
    (`bool-argument-normalization`). (Superseded by the next entry.)
  `src/code/test/lang/kernel/kernel-boot-smoke.sh` builds the kernel with NYC,
  boots it with a freestanding initramfs `/init`, and fails unless
  `NYC_KERNEL_INIT_REACHED` appears; both a freestanding and a static glibc
  `/init` pass. It takes about fifteen minutes and is not part of `ctest`.

- Smoke gate run end to end (2026-11-23). `kernel-boot-smoke.sh` built Linux 7.3
  `x86_64_defconfig` from a fresh output tree with the final compiler and
  booted both a freestanding and a static glibc `/init` to
  `NYC_KERNEL_INIT_REACHED`; both passed. `src/code/test/lang/kernel/kernel-provenance.py` pairs every
  `# CC` step of a `V=1` Kbuild log with NYC's own `compiled` line (or, for an
  interleaved line, a `.comment` free of `GCC:`). On the earlier `V=1` log it
  matched 2,973 steps with none left unpaired; the smoke script's log is not
  `V=1`, so there it only counts NYC's `compiled` lines (2,973). Changes made on the way:
  - `clear_user` now calls `rep_stos_alternative` directly with the length in
    `%rcx`, instead of through a thunk; objtool's "naked return" warning count
    for that path went from 21 to 0 and "call with UACCESS enabled" from 29 to
    4.
  - A statement-level call to a `noreturn` function is followed by a marker
    `ud2`, and code behind it is dropped before SSA construction (dropping it
    after optimization left PHIs naming vanished predecessors in
    `get_signal` and `intel_iommu_init`). Objtool still reports 205
    "unreachable instruction" and 209 "missing __noreturn" warnings, so that
    diagnostic review is not finished.
- `_Bool` store paths (2026-11-23). Three defects, all found by a differential
  fixture against GCC at `-O0` and `-O2`:
  - NYC's built-in `<stdbool.h>` defined `bool` as `unsigned char`, so every
    `bool` in code that includes it silently truncated; it is now `_Bool`.
    (The kernel defines its own `bool` and was unaffected.)
  - `_Bool` globals, global and local arrays, `bool *` locals, parameters and
    globals, `p[i]`, `*p = v`, `b++`/`b--` and `static bool x = 0x100` now
    convert to 0 or 1 before the byte store narrows the value. The
    `bool-store-paths` fixture (`-O0` and `-O2`) covers each form.
  - Still open: a `_Bool` argument of an indirect call (function pointers carry
    no parameter types in the frontend, only the result type), `bool **`, and
    compound assignment through a `bool *`.
- objtool (2026-11-23). A statement-level noreturn call, and
  `__builtin_unreachable()`, now end the path: code behind them is dropped
  before SSA construction, the result store is omitted, and the marker `ud2`
  stays out of the assembly, matching GCC. In a full Linux build "missing
  __noreturn" warnings fell from 209 to 40 and "unreachable instruction"
  stayed at 207 before the `__builtin_unreachable` change.
  A static `always_inline` function with no remaining reference is no longer
  emitted (it put references to `__init` data into `.text`). The final
  `V=1` build with `bzImage` and all 12 configured modules from a fresh output
  tree (all 2,973 `CC` steps NYC, `src/code/test/lang/kernel/kernel-provenance.py` rc 0, both `/init`
  boots passing) still reports 181 "unreachable instruction", 45 "missing
  __noreturn", 4 "call with UACCESS enabled", 9 "redundant UACCESS", 11 "leaves
  .noinstr.text" and 17 modpost section-mismatch warnings. The remaining
  unreachable instructions follow a `__builtin_unreachable()` inside a
  `do { } while (0)` block: the marker is not a CFG terminator, so the code
  after the loop survives. Fixing that needs an unreachable terminator in
  NYIR; the diagnostic review is therefore not complete. The three modpost
  warnings on `smp_ops` come from modpost naming the object by NYC's local
  `__nyc_static_N` symbol, which defeats its `*_ops` allowance.
- Full `ctest` (2026-11-23): 61 of 63 pass. `ny-stdlib-debug-path` (a debug-info
  directory string) and `nytrix-addr-probe-device` (a `[vvar]` page reads as
  readable under Linux 7.3.5) fail; neither touches the C frontend, and I did
  not run them against an unmodified tree.
- objtool and modpost, final pass (2026-11-23). The `nyc-kernel-boot` ctest
  (registered with `-DNYC_LINUX_SRC=<linux-7.3>`, about ten minutes) builds
  `bzImage` and all 12 configured modules from a fresh output tree, runs the
  provenance check (every `CC` step NYC) and boots a freestanding and a static
  glibc `/init`; it passes. Warning counts for that build against the 181
  "unreachable instruction" and 45 "missing __noreturn" measured before:
  - 1 unreachable instruction (`apply_fineibt`), 5 missing `__noreturn`
    (`io_worker_exit`, `wait_for_panic`, `__sysvec_reboot`, which GCC infers
    as noreturn; NYC does not), 1 unsupported instruction in
    `native_machine_emergency_restart`;
  - 6 "call leaves `.noinstr.text`" (four are indirect calls) and 2 "call
    with UACCESS enabled" (down from 11 and 4), 8 "redundant UACCESS disable"
    in `.altinstr_replacement`, 2 "return with UACCESS enabled" in ALSA;
  - 9 modpost section mismatches (six from `__calc_tpm2_event_size`, a plain
    `static inline` that NYC emits out of line; three from `smp_ops`, which
    modpost names by NYC's local `__nyc_static_599` symbol).
  Changes behind the drop: the `ud2 # noreturn` marker is emitted at expression
  level (noreturn calls, `__builtin_unreachable()`); the driver removes code
  that no path reaches, before SSA construction and again before emission
  (with phi-edge fix-up, on a copy that must still verify); the marker and any
  straight-line instructions behind it, and an epilogue nothing jumps to, are
  left out of the assembly; `always_inline` callees bypass the inliner's cost
  model (legality still applies), which closed calls from `noinstr` functions
  into `static __always_inline` helpers.
- Test status: `ctest` runs 62 of 64 tests green, with `nyc-kernel-boot`
  included. `ny-stdlib-debug-path` and `nytrix-addr-probe-device` fail the same
  way on an unmodified `HEAD` build (checked in a private worktree).
- Diagnostics after noreturn inference (2026-11-23). The final clean
  `nyc-kernel-boot` log (`bzImage` plus 12 modules) has no objtool, modpost or
  other warnings at all (about 450 at the start of the day), and both `/init`
  boots pass.
  - `native_machine_emergency_restart`: the `ljmp` of `machine_real_restart`
    was inlined into a callable function; noreturn functions are now kept out
    of line, as GCC does.
  - `default_idle` and `acpi_safe_halt`: NYC used to lower the kernel's
    paravirt call asm to a load of the member and a call through the retpoline
    thunk, so the call carried no `pv_ops` relocation, which is how objtool
    recognises a paravirt call. The call now keeps the kernel's own text
    (`call *pv_ops+off(%rip)` with its retpoline-safe annotation and its
    `ALT_CALL_ALWAYS` alternative entry), so boot-time patching turns it into
    a direct call as with GCC. The member must be a plain `symbol + offset`;
    other shapes keep the thunk call. The machine-form emitter declines such a
    call and the function goes through the NYIR emitter.
  The UACCESS diagnostics came from three NYC differences from GCC, found by
  building the same translation units with GCC and by `objtool --trace`:
  the scoped user access macros are three nested `for (done = false; !done;
  done = true)` loops whose `!done` test NYC kept after the cleanup (they now
  lower without a back edge, when the body only ever stores 1 to the flag);
  jump threading declined a branch-only block that held a leftover constant
  (such constants now move to the entry block), and the branch the threaded
  edge left behind (`br.if const == const`) was not decided (it is now,
  before emission). An indirect call through a function symbol is folded to a
  direct call and, after targeted inlining, inlined (the `bsearch()` callback
  `patch_cmp`). A `_Bool` argument of an indirect call is still not
  converted: the frontend stores only a function pointer's result type, never
  its parameter types, at about fifteen propagation sites. The three
  `smp_ops` modpost mismatches came from a zero-length local object
  (`__nyc_static_599` in `cpuid.o`) sitting at the end of `.data`; zero-length
  local objects now go to `.bss` (`zero-length-static` fixture). Fixtures for
  the above: `scoped-once-loops`, `jump-thread-implied-branch`,
  `indirect-call-folding`, `zero-length-static`.
  Changes behind these numbers:
  - A function whose every path ends in a noreturn call or an endless loop is
    inferred noreturn within the unit (as GCC does for visible callers); its
    callers get the marker, and code behind it is removed.
  - Unreachable-code removal now runs for every function, not only those with
    a marker, on a copy that must verify (a failing function keeps its body).
  - An unreferenced `static inline` function is no longer emitted, and a
    static function placed in a section by attribute (`__init`) is now
    discardable when nothing references it.
  - `always_inline` functions with automatic arrays are inlined (the parser
    marked every function with an automatic array noinline), and the
    loop-pressure limit no longer rejects them; the `always-inline-array`
    fixture covers this at `-O0` and `-O2` and checks the call is gone.
    The six `__calc_tpm2_event_size` modpost mismatches went away with it.
  - A compound assignment through a `bool *` (`*p |= v`) converts to 0 or 1.
  - Jump threading also uses a dominating branch: when a block tests `v == 0`
    (or `!= 0`) and a predecessor chain of single-predecessor blocks leaves a
    `br.if v`, the edge is threaded to the target the repeated test would
    choose (`if (ok) enter(); if (!ok) goto fail;` no longer leaves the entered
    path able to reach `fail`). The `jump-thread-implied-branch` fixture covers
    it. The two ALSA "return with UACCESS enabled" diagnostics persist: the
    final IR (`NYC_DUMP_FINAL=<function>` prints it just before emission) shows
    no re-test: the `stac` block goes straight to the user loads, every error
    exit reaches the `clac` block, and the error code travels through address-
    taken local slots. objtool's trace takes the `stac` replacement and then
    the `nop` original of the `clac` site. These are not false positives:
    `sound/core/pcm_native.c` compiled with GCC from the same Kbuild command
    line (`-fms-extensions` in place of `-fms-anonymous-structs`) gives an
    object with no objtool diagnostics at all, while the NYC object reports
    both. The nine "redundant UACCESS disable" diagnostics are NYC-specific
    in the same way: the six objects that carry them (`net/socket`,
    `sys_ia32`, `signal_32`, `entry/common`, `futex/core`, `rseq`) are clean
    when built with GCC. Root cause found with
    `objtool --trace=<function>`: the kernel's `scoped_user_*_access()` is
    three nested `for (bool done = false; !done; done = true)` loops, the
    innermost declaring the `__cleanup` variable. After the body, the
    cleanup (`clac`) runs and control goes through `done = true` and the
    `!done` test. GCC folds that test (the loop runs once, and the exit
    skips the loop header); NYC leaves it, so objtool follows the infeasible
    path that loops back into the body, reaches a second `clac` with UACCESS
    already off ("redundant UACCESS disable"), and in `snd_pcm_sync_ptr` a
    `return` on a path that skipped the `clac` ("return with UACCESS
    enabled"). The runtime behaviour is correct (a counting `cleanup`
    fixture runs the handler exactly once for normal exit, `return` and
    `goto`). The fix is in jump threading: the latch edge carries the
    constant `done = 1`, but the loop header holds more phis whose values are
    used after the exit, and `thread_edge_ex` declines when a skipped block
    defines values used later. Threading must instead forward each such phi's
    incoming value from the latch (a new phi in the exit block). Sharing one
    cleanup trampoline per target and scope set, which is in the tree, did
    not change the counts.
  - An indirect call whose target is the address of a named function (how
    Linux writes `static_call()`) is folded to a direct call, as GCC does; the
    `indirect-call-folding` fixture checks the result and that no indirect
    thunk remains. Two "{dynamic}" noinstr diagnostics went away with it.
- P0 harness (2026-11-23), first piece. The C frontend helpers
  (`nyc_execute_ok`, `nyc_execute_output`, `nyc_require_artifact`,
  `nyc_remove_artifact`) moved to `src/code/test/lang/harness.cmake`, and
  `nyr-c-harness-fault-injection` runs each against an injected fault in a
  child process and requires it to fail: a nonzero exit, a crash (SIGABRT), a
  timeout, source that does not compile, a link failure, a missing, an empty
  and a stale artifact. Disabling the failure check in the helper makes the
  test fail, so it is not vacuous. The rest of P0 (false positives in the
  differential and benchmark lanes, silent exclusions, expected-failure
  fragility, duplication) is not audited.
- Indirect-call `_Bool` arguments (2026-11-23). A call through a function
  pointer converts a `_Bool` parameter's argument to 0 or 1 when the pointer is
  a struct member, a local variable or a function parameter: the parameter list
  of the declarator is scanned for plain `_Bool` or `bool` parameters, and the
  mask travels with the declaration to the call. The `bool-store-paths`
  fixture calls through all three with `0x200`, `0x300` and `0x400`. Two traps
  found on the way: the mask must be reset wherever a call's signature is set
  (a `_Generic`-selected call to `kmem_cache_create` inherited the mask of an
  earlier pointer and turned its `flags` into 1, which failed the boot), and a
  typedef's recorded bool flag can be stale, so only `_Bool` and `bool` count.
  The same mask comes from typedef'd function-pointer types and reaches
  globals, fields, locals and parameters declared with them (the fixture
  covers `bool_fn_t` in each position). Arrays of function pointers and
  function-pointer returns do not carry it.
- Kernel tracing flags (2026-11-24). `-fpatchable-function-entry=N[,M]` and
  `-mrecord-mcount` are implemented for assembly output: M NOPs go before each
  function symbol and N-M after it (after an `endbr64`), with a
  `__patchable_function_entries` record of the first NOP, and each `-pg` call
  to `__fentry__`/`mcount` gets a `__mcount_loc` entry. `-c` rejects both
  options rather than ignoring them, because the direct object encoder does
  not print this text. The kernel build, which passes
  `-fpatchable-function-entry=16,16`, now produces the padding and records on
  every unit and still boots; function alignment is not adjusted, so the
  16-byte prefix is not guaranteed to be 16-byte aligned as with GCC's
  `-fmin-function-alignment=16`. The `patchable-entry` fixture checks the
  three-before, two-after layout and both sections after the host assembler.
- FS/GS address-space forms (2026-11-24). A probe of ten forms against the
  assembly output found that a `__seg_gs`/`__seg_fs` qualifier on an aggregate
  pointer (`p->b`), a struct field holding a segment pointer, a typedef of a
  segment type and an array of segment pointers all emitted no `%gs:` prefix,
  silently reading the wrong segment. Casts and scalar pointers (the forms the
  kernel uses: `*(__seg_gs int *)offset`, subscripts, stores, increments) were
  correct. Aggregate-pointer parameters and locals and globals, fields and
  typedefs are now rejected with "qualify only scalar pointers and casts";
  arrays of segment pointers (file-scope, local and parameter) are now rejected
  the same way. The `segment-unsupported-forms` fixture checks each rejection
  and the cast form.
  The kernel builds without warnings and boots with the rejections in place.
- Direct-object assembly annotations (2026-11-24). Section-bearing inline asm
  (`.pushsection .discard.*`, bug tables) used to be rejected in ordinary `-c`
  builds and accepted only with `-mcmodel=kernel`. Every `-c` build now lets
  GNU as complete such a unit, with the per-unit "emitted through GNU as" note
  and the annotation sections preserved; nothing is dropped silently. The
  resumable-asm regression checks the note and that `.discard.test` is in the
  object.
- Complex forms (2026-11-24). A GCC differential over fifteen complex forms
  found, and fixed: complex arguments to calls (a prototype's complex
  parameter was compared as a scalar, and a call to `creal`, `cimag`, `conj` or
  any libm complex function failed or crashed because no argument-size table
  was allocated); a complex result from a prototype; `a * b + c` and `x * a - a`
  (the multiply and divide helper returned from the expression loop, so the
  next operator was a syntax error); `(double _Complex)float_complex_value`
  casts; and complex descriptors of the same width that were compared by
  pointer. `complex-forms` prints each result and must match GCC's output at
  `-O0` and `-O2`. Compound assignment on complex values
  (`a += b`, `-=`, `*=`, `/=`) and implicit float-to-double complex conversion
  in call arguments were added afterwards (`complex-compound`, also compared
  with GCC at `-O0` and `-O2`); the complex arithmetic moved into
  `lower_complex_binary` so both paths share it. Subscripts on complex pointers
  (`p[i]` read and write, `p[i] * q[i]` in loops) work through a host driver
  (`complex-pointer-index`, compared with GCC at `-O0` and `-O2`). Local arrays of
  complex values (brace initializers, `a[i]` read and write, `a[i] op= x`,
  `p[i] op= x`) follow in `complex-arrays`, also GCC-compared at `-O0` and
  `-O2`. File-scope `float`/`double`
  arrays and complex variables and arrays now work too (`global-float-arrays`,
  GCC-compared at `-O0` and `-O2`): the earlier "plain `double arr[3];` fails"
  limitation is gone. Complex global initializers take a real constant only
  (`= 2.0`), not `a + b*I`. Still unsupported: `long double _Complex`.
- Differential fuzzing (2026-11-24). `ny-fuzz` (an installed C tool,
  `src/cmd/tools/fuzz`) compiles seeded programs with a reference compiler and
  with NYC at `-O0`/`-O2` and compares program output. Its built-in generator
  emits undefined-behaviour-free integer programs in three-address form;
  `yarpgen` and `csmith` are used when installed (csmith also needs its runtime
  headers, `CSMITH_INCLUDE`). Programs NYC rejects count as "unsupported", not
  as failures, unless `--strict`; `ny-fuzz reduce` shrinks a failing case by
  deleting lines and simplifying temporaries; the `ny-fuzz-smoke` ctest runs
  forty seeds of every available generator. The first 300 built-in seeds found
  two defects. (1) A store to a file-scope `short` or `signed char` wrote eight
  bytes and overwrote the neighbouring globals; file-scope `float` scalars were
  also rejected, and an `int` assigned to a `float`/`double` global failed to
  compile. Fixed; `global-narrow-stores` is the regression. (2) In the
  direct-object encoder (`-c`, `-O0`) the register allocator could mark a
  segment as carried from a predecessor whose segment had been demoted to its
  stack home, so a later block read a register that held another value (seed
  245: `-S` through GNU as was correct, `-c` returned 20334 instead of
  15757574049326185246). `nyr_mach_regalloc_repair_carried` now turns such
  segments, and the segments carried from them, into reloads;
  `regalloc-carry-spill` is the regression. After the fix 1500 built-in seeds
  and the yarpgen seeds that NYC accepts agree with GCC.
</details>
