<!-- nytrix-doc: {"audience":"user","featured":true,"group":"project","order":10,"summary":"Release highlights with expandable development records for each version."} -->
# Changelog

Each release has short user-facing highlights followed by one expandable
details section. Dates, measurements, fixture paths, and local
artifacts there describe the recorded work, not a fresh test of this checkout.

## [0.10] - 2026-10-13

### Highlights

- NYIR verification now rejects functions whose scalar exits disagree on
  the integer, f32, or f64 return calling convention. Tagged values and
  handles retain their shared integer convention; aggregate return metadata
  remains authoritative. The new `return_abi` regression checks 100 class
  pairs and executes both paths of compatible pairs. It and `scalar_class`
  pass, together with 20 cold ABI, fallthrough, argument-binding, and
  global-shadowing variants. The completed verifier item was removed from
  the TODO. The obsolete shadow-lookup tracing wrapper and its forced
  no-inline/return-address instrumentation were also removed.
- Dynamic accessor lengths now dispatch by the actual tagged value instead
  of a contextual string guess. Cold runs pass `extensible`, `prolog`, both
  parser fixtures, `proof-logic`, and `tactic-simp-ring`; Prolog completes in
  4.8 seconds instead of timing out inside unification. The boolean ternary
  fixture also passes all five routes after correcting its sibling import.
  These resolved failures were removed from the TODO.
- Five-argument binding coverage now checks individual slots, mixed dynamic
  and string parameters, recursive calls, tail calls, runtime inputs, and
  parameters shadowing module globals. All six cold NYR/LLVM AOT and JIT
  variants pass. The obsolete shared-misbinding investigation was removed
  from the TODO; the separately tracked parser failures remain open.
- Guard-duplication execution coverage now includes a potentially trapping
  division guard, which remains un-cloned. Zero/one/many-trip cases, live-out
  PHIs, memory guards, and the cold native PHI/edge-copy fixtures pass.
- The read-only x64 loop regression executes signed DIV/MOD with boundary and
  random payloads under both SysV and Win64, including powers of two, general
  and negative divisors, and zero/negative trip counts. The former
  `x64_readonly_loop_gpr`, `licm_address_component`, and `portable_wasm`
  failures do not reproduce in the current focused runs.
- Benchmark documentation counts checksum/execution failures separately from
  low-time/noisy measurements and fixture budget misses. Failure rows include
  NYC checksums, route return codes, and NYC diagnostics; the synthetic report
  regression checks classification and Markdown output.
- Edge-ADD sinking bounds forward searches and instruction moves to 64
  instructions. Boundary cases and an 8,192-instruction gap pass the scheduler
  regression, avoiding quadratic scans in generated blocks.
- NYC now keeps array dimensions through `typeof`-based pointer-to-array casts
  when the array elements are aggregate pointers; Linux 7.3
  `arch/x86/kernel/apic/vector.o` compiles with NYC and GNU `as`. File-scope
  array sizing also accepts Linux's `__roundup_pow_of_two` constant builtin,
  verified by the C frontend regression and `mm/vmalloc.o`.
- C preprocessing now keeps multiline block comments inside continued
  directives without truncating macro replacement lists. The runtime frontend
  regression passes, and Linux 7.3 `fs/nfs/callback_xdr.o` compiles with NYC
  and GNU `as`.
- Block-scope C23 `auto` inference and GNU statement expressions ending in
  control flow after multiple declarations now compile. Runtime frontend
  regressions cover type preservation and aggregate locals; targeted Linux 7.3
  `crypto/aes.o` compilation succeeds with NYC and GNU `as`.
- GNU `typeof` now resolves `__builtin_constant_p` and `__builtin_ffs` result
  types, indirect `void` calls remain valueless in conditional expressions,
  and preprocessor branch state survives a macro argument closing inside a
  conditional. Runtime frontend regressions cover each case; NYC compiles
  Linux 7.3 `block/blk-merge.o`, `io_uring/tw.o`, and
  `lib/decompress_inflate.o`.
- Local aggregate arrays support arrow-member assignment through their first
  element, and function-pointer arrays accept casted null initializers.
  Runtime frontend regressions pass; Linux `io_uring/cmd_net.o` and
  `block/partitions/core.o` compile with NYC and GNU `as`. The complete Linux
  kernel acceptance build remains unresolved.
- Aggregate fields now retain up to 32 array dimensions, and array-shape
  metadata survives indexing through nested aggregate fields. A rank-four
  field-access regression passes in the C frontend suite; Linux kernel
  acceptance remains in progress.
- The C frontend and x86-64 emitter now support `rep stosw`, `rep stosl`, and
  `rep stosq` alongside `rep stosb`. Runtime coverage passes for all four
  widths; Linux 7.3 `vgacon.o`, `iommu_amdv1.o`, `gen6_ppgtt.o`,
  `gen7_renderclear.o`, and `intel_ggtt.o` compile with NYC and GNU `as`.
- The C frontend now models the tied-output `mull %%edx` inline-assembly form
  used by Linux x86 delay calibration, the fixed-register `mwaitx` instruction,
  and the `delay_loop` timing sequence. Runtime C frontend regressions pass;
  Linux `arch/x86/lib/delay.o` now compiles with NYC and GNU `as`.
- Positional `mulq %3` inline assembly now lowers the low and high halves of
  its 64×64 product. The runtime regression passes and Linux
  `arch/x86/lib/kaslr.o` compiles with NYC and GNU `as`.
- Linux's 4×8, 1×8, and 1×4 `movnti` usercopy loops now compile with NYC and
  GNU `as`; runtime regression coverage verifies each store width.
- File-scope integer arrays now infer incomplete outer bounds for two-
  dimensional tables, including nested integer rows and character string rows
  with sparse row designators. Rank-two string tables beginning with an
  undesignated row now use the aggregate initializer path instead of the
  flat-string path. The
  runtime C frontend regressions cover sparse integer rows, `nonstring`/
  `section` attributes, and the undesignated first character row; Linux
  `lib/base64.o`, `drivers/acpi/tables.o`, and `drivers/tty/vt/consolemap.o`
  compile with NYC and GNU `as`.
- Local comma-separated pointer declarators now keep pointer depth per
  declarator. Runtime coverage exercises nested pointer-array stores; Linux
  `drivers/tty/vt/consolemap.o` compiles with NYC and GNU `as`.
- Global pointer arrays now accept multidimensional designated initializers.
  The C frontend regression checks a sparse pointer table, and Linux
  `arch/x86/lib/inat.o` compiles with NYC and GNU `as`.
- Opaque `void **` locals now retain enough pointer depth for indexed stores.
  The runtime frontend regression exercises a copied pointer slot, and Linux
  `lib/memcat_p.o` compiles with NYC and GNU `as`.
- Aggregate-pointer globals now accept a following pointer declarator in the
  same declaration, and aggregate-pointer arrays consume trailing file-scope
  placement attributes. The C frontend regression suite passes. Linux
  `net/netfilter/nf_log.o` advances past its `__read_mostly` declaration but
  still fails on a multidimensional pointer-array subscript expression.
- Rank-one aggregate arrays of pointers now decay to pointers to their
  elements. The runtime regression checks `void **` subtraction against an
  aggregate `void *` array, and Linux `lib/radix-tree.o` compiles with NYC and
  GNU `as`; `objtool` still reports four unreachable-instruction warnings
  there.
- NYC lowers x86 `__builtin_extract_return_addr` as an identity operation.
  The frontend regression verifies the returned pointer; Linux
  `lib/vsprintf.o` now compiles with NYC and GNU `as`.
- Pointers to `va_list` arrays now retain their va-list pointee through
  aggregate fields and dereference. A variadic runtime regression copies a
  list through such a field; `lib/vsprintf.o` compiles with one `objtool`
  unreachable-instruction warning still under review.
- Local comma-separated C declarations now accept attributes before each
  declarator. A runtime frontend regression covers `__attribute__((unused))`,
  and Linux `block/blk-iocost.o` compiles with NYC and GNU `as`.
- Address-of now preserves lvalues through nested aggregate-array subscripts.
  The runtime regression checks a rank-three struct array element, and Linux
  `block/kyber-iosched.o` compiles with NYC and GNU `as`.
- Linux 7.3 `drivers/pci/pci.o` now compiles with NYC and GNU `as`.
- External multidimensional aggregate arrays now retain their declared
  extents through address-of subscripting. The runtime frontend regression
  passes; Linux 7.3 `security/selinux/hooks.o` compiles with NYC and GNU `as`,
  with `objtool` warnings still under review.
- Function-like macro expansion now accommodates up to 1,024 arguments for
  generated Linux trace events. `fs/nfs/nfs4trace.o` compiles with NYC and GNU
  `as`; the frontend regression suite passes.
- NYC now lowers Linux's x86-64 `lock cmpxchg16b` try-compare template, including
  its flag output and updated expected-value pair. The runtime frontend
  regression covers successful and failed comparisons; Linux 7.3 `mm/slub.o`
  compiles with NYC and GNU `as`. `objtool` still reports three unreachable
  instructions and one missing-`__noreturn` warning in that object.
- Aggregate-field assignment now supports a second subscript through pointer
  array elements, covering Linux's `sym_val_to_name[value - 1]` update.
  A runtime frontend regression covers the two-index store; Linux 7.3
  `security/selinux/ss/policydb.o` compiles with NYC and GNU `as`, with four
  unreachable-instruction warnings still to review.
- File-scope function pointers returning aggregate pointers now pass through
  the function-pointer declaration parser before aggregate declaration
  collection. The runtime frontend regression checks an indirect call; Linux
  7.3 `fs/nfs/pnfs_nfs.o` compiles with NYC and GNU `as`.
- NYC now lowers Linux x86-64 futex compare-exchange exception-table asm and
  its runtime-mask relocation. Linux 7.3 `kernel/futex/core.o` and
  `kernel/futex/pi.o` compile with NYC and GNU `as`; `objtool` still reports
  UACCESS-state warnings in both objects.
- Faultable futex compare-exchange, exchange, and add asm now have runtime
  regressions for successful and failed comparisons, memory updates, and the
  error output register. Qualified nested function-pointer parameters now
  parse and execute correctly; Linux 7.3 `crypto/ahash.o` compiles with NYC
  and GNU `as`, with one unreachable-instruction warning.
- Local pointer arrays now support indexed stores and nested pointer
  dereferences; aggregate-pointer initializer slots remain valid while their
  initializer is parsed. Runtime frontend regressions pass, and Linux 7.3
  `security/selinux/ss/mls.o` and `fs/9p/vfs_super.o` compile with NYC and
  GNU `as`. `kernel/futex/waitwake.o` now compiles after lowering the futex
  bitwise retry loop; `objtool` reports three UACCESS-state warnings in
  `arch_futex_atomic_op_inuser`, `futex_get_value_locked`, and
  `futex_robust_unlock`.
- File-scope aggregate initializers now size flexible scalar arrays from GNU
  range designators, accept GNU zero-length aggregate arrays, and concatenate
  braced adjacent narrow strings. File-scope GCC diagnostic `_Pragma` tokens
  are accepted. Runtime frontend regressions cover those cases and aggregate
  array compound literals. Linux 7.3 `mm/init-mm.o`, `fs/coredump.o`,
  `kernel/crash_reserve.o`, and `kernel/cgroup/rstat.o` compile with NYC and
  GNU `as`; the full kernel build remains blocked.
- NYC accepts pointer-valued `put_user` operands; Linux 7.3
  `kernel/futex/syscalls.o` compiles with NYC and GNU `as`. Segment-register
  inline-assembly outputs to structure fields now lower, and
  `kernel/crash_core.o` compiles with NYC and GNU `as`. Nested GNU statement
  expression pointer stores now compile in `fs/quota/dquot.o`. Local static
  multidimensional character arrays accept range-designated string rows; the
  runtime regression passes and `fs/proc/task_mmu.o` compiles with NYC and
  GNU `as`. Aggregate `__auto_type` initializers now copy into independent
  local storage, covered by a runtime regression; `fs/proc/base.o` compiles.
  Aggregate-array pointer shape survives nested pointer indexing, allowing
  `kernel/cgroup/cpuset.o` to compile; its `objtool` unreachable-instruction
  warning remains under review. Self-referential static address
  initializers now retain their declared object's name during relocation
  lowering; a runtime regression passes and `fs/kernfs/file.o` compiles.
  `typeof` now resolves calls through function-pointer aggregate members;
  `fs/netfs/objects.o` compiles. GNU omitted-middle conditional `?:` is
  supported in `typeof`, with a runtime regression. Aggregate pointer fields
  support postfix decrement, covered by a runtime regression; `fs/ext4/extents.o`
  compiles. Known array-member bounds are now returned for
  `__builtin_object_size` expressions without evaluating the operand, with a
  runtime regression; `fs/ext4/ioctl.o` compiles. Function parameters
  declared as arrays of pointers now decay to the correctly nested pointer
  type, covered by an assignment regression; `mm/migrate.o` compiles with one
  remaining `objtool` unreachable-instruction warning.
- `const typeof(...)` preserves leading qualifiers through static type
  resolution, fixing the Linux UDP `WRITE_ONCE` store; `fs/nfs/nfsroot.o` and
  `kernel/audit.o` compile. GNU attributes after labels are parsed, and case
  expressions with unary operators are no longer mistaken for C labels;
  `fs/ext4/super.o` and `fs/nfs/nfs4proc.o` compile. `fs/ext4/super.o` still
  emits unreachable-instruction and missing-`__noreturn` warnings. The
  frontend regression suite passes; the full Linux build remains blocked by
  other parser and inline-assembly failures.
- File-scope aggregate compound literals now accept grouping parentheses in
  pointer initializers and preserve relocation offsets through selected array
  elements and members. Static null-based aggregate subobject addresses now
  compute checked member and array offsets, and `&*` around a cast null pointer
  remains null. Top-level comma expressions after indexed assignments bypass
  the assignment-specific fast path. Aggregate array compound literals now
  materialize elements in local storage for calls and pointer use; file-scope
  and local character matrices accept unbraced string rows. The recorded
  frontend regressions pass; targeted Linux 7.3 builds include
  `drivers/gpu/drm/i915/{i915_hwmon,i915_pmu}.o`, `drivers/tty/sysrq.o`,
  `net/core/neighbour.o`, `lib/zlib_deflate/deftree.o`,
  `sound/hda/{core/i915,controllers/intel,core/intel-dsp-config}.o`. Some
  objects retain `objtool` warnings, which remain under review.
- Further recovered NYC kernel work resolved duplicate-compatible `_Generic`
  associations in ACPI and retained boolean identity and pointer-member
  qualifiers through aggregate fields. Targeted objects now compile with NYC
  and GNU `as`, including ACPI helpers, `drivers/gpu/drm/i915/i915_params.o`,
  `drivers/tty/vt/keyboard.o`, AES/Blake2s, selected Zstd objects,
  `sound/core/seq/seq.o`, `net/sched/cls_api.o`, and `net/sunrpc/xprtsock.o`.
  The full kernel build still fails before producing final artifacts; Zstd
  decompression and other parser, typing, inline-assembly, and warning-review
  work remains open.
- Function-like macro invocations can span an active conditional branch and
  its `#endif`, including the `pr_err` call in Linux 7.3's module namespace
  check. The frontend regression passes and `kernel/module/main.o` compiles
  with NYC and GNU `as`.
- NYC pointer casts now update pointee qualifiers even when the underlying
  pointer type is unchanged, and dereference stores use the resulting
  expression qualifiers. Linux 7.3 `fs/binfmt_script.o` compiles with NYC and
  GNU `as`. Valid uninitialized `const` local declarations are accepted;
  `kernel/rcu/tree.o` also compiles, and the focused C frontend suite passes.
- NYC preserves nested pointer-array typedef dimensions when completing an
  external declaration and retains leading `const` on inline aggregate
  definitions. The C frontend suite passes with a nested-array runtime
  regression; Linux 7.3 `mm/slab_common.o` now compiles using NYC and GNU `as`.
  Its three `objtool` warnings about `panic()` no-return metadata remain under
  review; this is not a completed kernel acceptance build.
- `./make nyc` builds the standalone C compiler and its backend dependencies;
  the native `ny make` frontend accepts the same target. Focused NYC help works
  before rebuilding `ny-make`, and both frontends accept the documented
  `--debug` selector. `./make nyc -h` and `nyc --help` now expose one complete
  compiler catalog, including target limits, compatibility-only flags, and
  compile/link examples. Compiler and native build-tool help use Ny's shared
  flag renderer with automatic colors, themed sections, intact option names,
  and adaptive columns; bootstrap help also respects narrow terminal widths.
- Compiler source, bitcode, native-image, and module caches now use bounded
  atomic publication: 256 MiB and 4096 files per managed directory by default,
  with a positive `NYTRIX_CACHE_MAX_MB` override. Active core/module inputs are
  leased through consumption, and abandoned compiler temporaries are reclaimed.
  Suite wrappers release frozen core snapshots while retaining diagnostics;
  copied compiler snapshots no longer duplicate standard-source entries solely
  because their executable pathname changed.
- NYC's unevaluated-expression probes own their scratch value metadata,
  preventing `sizeof` and related lookahead from invalidating the active
  parser's allocation tables. Array-member and subarray sizes retain their
  complete type instead of decaying to pointer size, including Linux's
  extra-parenthesized `sizeof_field` expansion. Casts inside GNU `typeof` retain
  the cast target's type metadata rather than inheriting an operand's array
  shape, including Linux signal-frame byte-array casts. The Linux mac80211
  header assertion and `net/mac80211/main.c` now compile with NYC; its assembled
  object still reports an `objtool` unreachable-instruction warning. `typeof`
  preserves global array types across the kernel's export redeclarations, and
  flexible array members may specify fixed inner extents and infer their
  outer extent from nested static initializers. `libata-scsi.c` and
  `ata_piix.c` compile with NYC; both objects assemble with GNU `as`, and
  `ata_piix.o` passes the configured `objtool` checks without warnings.
  Unsigned `__builtin_bswap32`/`__builtin_bswap64` expressions now fold in
  constant contexts; their width and value checks pass, and
  `net/sunrpc/rpcb_clnt.c` compiles with NYC, assembles with GNU `as`, and
  passes the configured `objtool` checks without warnings.
- File-scope function-pointer array initializers now accept function-type
  attributes between the declarator and `=`. The section-attributed direct
  function-pointer array and `ms_abi` function-pointer typedef parser fixtures
  pass in the complete C frontend suite, alongside the AArch64 aggregate ABI
  check.
  The same suite verifies `typeof` redeclarations after fixed-size initialized
  arrays and inferred-size `const` arrays declared initially with incomplete
  bounds, and the wide null character constant used in Linux EFI `vsprintf.c`.
- Local indexed assignment now accepts every object-pointer type, including
  opaque pointers, while still excluding function pointers. Its runtime
  regression writes through `void **` with an unsigned index. Fixed-size local
  character arrays also accept a string whose characters exactly fill the
  array; the regression checks the final character at runtime without copying
  the omitted terminator out of bounds.
- Variadic builtins now accept the array-backed x86 `va_list` as well as
  pointers that dereference to it. Runtime regressions copy a `va_list` and
  fetch an argument through a `va_list *`, covering the operand shapes used by
  Linux's EFI formatting and tracing code.
- `unsigned short **` is now recognized as a pointer by NYC's common pointer
  compatibility checks. A compile regression verifies its conversion to
  `const void *`; this fixes that pointer classification gap but does not yet
  unblock the Linux APIC per-CPU array expression.
- File-scope, fixed-extent pointer arrays without initializers now receive
  zero-initialized storage, preserving tentative-definition linkage. A runtime
  regression checks their null entries, and Linux `kernel/power/suspend.c`
  now compiles with NYC and assembles with GNU `as`; configured `objtool`
  reports three unreachable-instruction warnings in that object.
- Aggregate field arrays now accept extents above 65,536, and `sizeof` on an
  indexed global aggregate member resolves the member rather than stopping at
  the closing bracket. Type lookahead for GNU `offsetof` accepts runtime array
  indices without evaluating them. Runtime regressions cover large aggregate
  size, member-array `sizeof`, and dynamic-index `offsetof` type resolution;
  the complete C frontend suite passes. Isolated NYC probes compile Linux
  `kernel/power/swap.c` and `kernel/printk/printk.c`; this does not establish
  full kernel acceptance.
- Local `_Bool` assignments now use the same pointer-to-boolean conversion as
  initializers and parameters. Static pointer initializers resolve a function
  designator ahead of a same-named `typeof(function)` object declaration; the
  aggregate-return function-pointer regression and complete C frontend suite
  pass.
- NYC lowers Linux's x86 runtime-constant right-shift asm and preserves its
  allocated `runtime_shift_*` relocation section. A compile-link-run regression
  checks the shifted value. With saved Linux 7.3 Kbuild flags, `fs/dcache.c`
  now compiles and assembles with GNU `as`; configured `objtool` reports eight
  unreachable instructions in that object. `kernel/irq/proc.c` also compiles
  and assembles with those flags without configured `objtool` warnings.
  Trailing attributes on inline aggregate global definitions are covered by a
  compile-link-run frontend regression. NYC also lowers Linux's unsigned
  runtime-pointer clamp asm, with a compile-link-run regression. Saved Kbuild
  builds of `fs/readdir.c` and `fs/select.c` now complete; `objtool` reports
  UACCESS-state warnings in both objects. Linux's locked `btsl` flag-output
  pattern now compiles with a compile-link-run regression.
- With saved Linux 7.3 x86-64 Kbuild flags, NYC compiles `lib/ctype.c`,
  `drivers/usb/storage/scsiglue.c`, and EFI `vsprintf.c`; GNU `as` assembles
  all three outputs. Configured `objtool` emits no warnings for `ctype` or
  `scsiglue`; it reports unreachable instructions in `vsnprintf+0x1271` and
  `get_number+0x341` for `vsprintf`. This is targeted TU coverage, not a
  complete kernel build.
- GNU asm memory inputs evaluate their address once without reading the
  referenced object first, including nonfaulting prefetch operands. Repeated
  references to one memory operand survive alternative instruction templates.
- Local x86 asm outputs using the legacy `=rm` form now remain observable to
  NYIR optimization and reload their actual stack-home values after the asm.
  The existing compile-link-run memory-output regression passes. GNU
  `+r`/`+m` qword `xadd` now preserves an unprefixed template and supports a
  bare local memory operand; a compile-link-run regression checks both outputs.
  GNU `nonnull` attributes now accept the standard no-argument form; the
  pointer-attribute fixture compiles, links, and runs.
- Statement-level inline asm beginning with unconditional `ud2` now terminates
  C control flow through nested `do` statements, including named-immediate
  bug-table templates. The regression fixture compiles and emits no branch
  after `ud2`; Linux `sock_diag_init` still needs its separate objtool warning
  investigated.
- Linux-style tied `addl`/`adcl` register outputs now lower their 32-bit carry
  semantics for both `adcl $0xffff` and `adcl $0` forms. Runtime regressions
  cover carry and no-carry boundaries. Chained 32-bit checksum assembly with
  three general inputs and a tied initial value is also lowered; the focused
  fixture and Linux `selftests.c` / `tcp_output.c` translation units pass.
  `net/ipv6/udp_offload.c` lowers through its separate five-operand qword
  add-with-carry template.
- The x86-64 IPv6 checksum's chained `addq`/`adcq` memory-input asm now lowers
  as explicit wrapping additions with propagated carry. A runtime fixture
  covers consecutive carries; NYC-generated `udp_offload.c` and `exthdrs.c`
  assemble with GNU `as` and pass the configured `objtool` checks without
  warnings.
- NYC preserves aggregate-pointer array elements in uninitialized local
  arrays, GNU attributes between pointer levels, aligned aggregate locals,
  and unsigned wide-enum aliases. Automatic i386 pointer arrays use four-byte
  slots and strides. Target-width size expressions and host varargs
  consumers exercise `sizeof`, alignment, and member offsets across all four
  `regparm` settings. Narrow symbol-address results no longer overwrite a
  neighboring live i386 spill slot.
- NYC completes tentative pointer-array declarations, zero-fills omitted
  pointer initializers, and preserves GNU zero-length arrays. Signed and
  unsigned character arrays accept string initializers.
- Kbuild dependency files put the main source before forced headers, as
  required by `fixdep`. Structured x86-64 assembly output also resolves C
  external data names consistently with the legacy emitter.
- Native Linux NYC invocations can discover system headers after explicit
  include paths and modeled headers; `-nostdinc`, explicit targets, and
  embedded frontend calls do not implicitly import the host include tree.
  Abstract pointer-to-function-pointer parameters support real GMP header
  declarations.
- NYC's O2/O3 heuristic inliner considers ordinary discardable static helpers,
  not only explicitly inline functions. GNU `noinline` survives declarations,
  definitions, and unit compaction; address-taken and recursive helpers retain
  correct executable behavior.
- The shared optimizer removes redundant repeated sign extension to the same
  or a wider integer width, preserving narrower truncations and signed
  boundary values. A focused matched NYC `binary` comparison measured
  12.652 ms before these two changes and 10.811 ms afterward, with checksum
  `3360874588549334`; this does not close the runtime competitiveness target.
- NYC accepts `-pthread` with the `_REENTRANT=1` header contract. Explicit
  `-D`/`-U` options retain precedence regardless of the flag's position;
  NYC-generated pthread create/join code passes a real execution smoke.
- Mock rendering skips no-op TTF bitmap blits when glyphs have no CPU pixel
  data, while still resolving and caching glyphs.
- Anonymous functions accept a single brace-free expression body with an
  implicit return; existing block bodies remain unchanged.
- Manual memory remains the default; tracing garbage collection is optional.
- String repetition and matrix workloads received performance and measurement
  improvements.
- Fresh `malloc_raw` allocations skip the raw-pointer registry's duplicate
  lookup while retaining the registry entry needed for raw-pointer ownership
  checks.
- Raw-pointer unregistration avoids calling `free` when no registry slab
  became empty.
- Once the append-only map-oracle range table is full, allocation tracking
  skips the per-thread page-dedup cache, which no longer affects oracle
  behavior.
- Loop-vector unrolling avoids cloning dense vector bodies, reducing live
  vector pressure in the x86-64 native backend.
- `core.calloc_raw` allocates zero-initialized unmanaged memory that can be
  released with `free_raw`.
- Native allocation calls keep their runtime entry point consistent with the
  lowered argument representation. Generated core link names no longer send
  a raw odd byte count to the legacy tagged-integer allocator (`17` previously
  became `8`). Typed, dynamic, and inlined allocation-size boundaries pass the
  focused native/LLVM fixture; explicit user C bindings retain their names.
- NYC's x86-64 segment-register inline-assembly matcher now builds cleanly
  under `-Werror=format-extra-args`.
- The x86-64 encoder keeps a final-use remainder operand in its assigned
  non-clobbered register instead of writing an unnecessary spill home.
- Loop unswitching now handles natural loops whose preheader falls through
  directly into the loop header; a focused optimizer regression checks both
  semantics and that the loop is cloned.
- Command-line help now has a concise default and registry-generated topic
  references; Bash and Fish completions include valid values for registered
  enums.
- Compiler options now offer consolidated output, optimization, native-tuning,
  DWARF, and NYR-pass controls while retaining legacy spellings.
- `core.os.args.positionals_from` now keeps ordinary arguments when no `--`
  separator is present and discards pre-separator options when one is present.
- `core.os.args.flag_from` now honors the last positive or `--no-` flag, and
  short value options accept `-o=value`.
- Bare mode rejects inspection flags that its fast path cannot honor instead
  of silently producing ordinary compiler output.
- Invalid values for compiler mode options now report allowed choices and exit
  with a usage error.
- Raw OS descriptor reads retry when interrupted by `EINTR`.
- The Brainfuck showcase now accepts inline source and writes assembly only
  when requested; generated assembly no longer contaminates the program output.
- The C frontend handles `__func__`, function attributes between the result
  type and name, GNU function alias attributes, aggregate declarations in
  `for`-loop initializers, fixed-register and external-global memory
  inline-assembly inputs, and memory operands for scalar and aggregate lvalues,
  including memory-plus-register bit operations and condition-code outputs, in
  emitted x86-64 assembly.
  Multidimensional aggregate arrays
  and pointer-to-array aggregate fields and local declarations are supported,
  along with file-scope `sizeof` array type constants.
- The C frontend's CTest harness now keeps its C inputs as 190 checked-in
  fixtures instead of generating them with embedded `file(WRITE)` snippets in
  `run.cmake`. This preserves the test sources for review, avoids source-order
  overwrites, and removes workspace-specific paths from the inline-assembly
  immediate regression. Relocation inspection now uses CMake's discovered
  `readelf` or `llvm-readelf` path and skips only those inspection checks when
  neither tool is available. The frontend CTest passes after the migration.
- `typeof` statement-expression lookahead now uses its own lvalue table rather
  than sharing storage with the active parser, preventing table growth from
  invalidating or freeing live parser state. A translation-unit regression
  grows the lvalue table past its initial capacity; the Linux `core.c` input
  also compiles and assembles with zero Valgrind errors.
- NYC lowers Linux x86 clear-page alternatives through `memset`, clear-user
  alternatives through the fault-aware `rep_stos_alternative` helper, and
  LKGS plus typed segment-register save/load forms with exception-table
  entries. Focused frontend regressions pass; current `fork.c` and `ldt.c`
  preprocessed inputs compile with NYC and assemble with GNU `as`.
- NYC models the common `<stdint.h>` integer types and limits/macros, evaluates
  enum constants in automatic aggregate-array extents, and accepts floating
  compound assignments, zero-initialized aggregate arrays, and larger local
  scalar arrays; focused benchmark C blocks verify assembly generation.
- The x86-64 C frontend lowers locked 32-bit memory add/sub, bitwise RMW, and
  byte XOR condition-code inline assembly, and 64-bit `xadd`, preserving memory
  updates, returned old values, and reported flags.
- Locked qword OR assembly accepts the three-character `orq` mnemonic without
  misclassifying it as a 32-bit operation; a compile-link-run regression checks
  the memory update.
- Classic byte and word compare-exchange assembly preserves memory boundaries
  and old-value results on success and failure. Byte compare-exchange has a
  distinct NYIR marker from trailing-zero-count assembly.
- i386 `-mregparm=0..3` and GNU `regparm(N)` attributes now control argument
  placement instead of accepting the real-mode register convention as a no-op.
  Runtime host/NYC interoperability covers each register count, small and large
  aggregates, hidden result pointers, scalar 64-bit values, floating-point
  records, callbacks, and explicit cdecl overrides.
- Integer literals select `int`, unsigned `int`, or the required wider
  suffix/range type, including 32-bit i386 `long` suffixes. Unsuffixed small
  integers no longer become 64-bit callback arguments on i386.
- Static array-address initializers use the declared element/row stride rather
  than applying pointer-size scaling to scalar element types.
- NYIR copy propagation preserves `asm goto` targets as labels when label IDs
  overlap aliased SSA values, preventing control-flow corruption after inlining.
- NYC accepts three-level aggregate pointers in structure fields, parameters,
  locals, and globals, including nested indexing, pointer subtraction, and
  nested pointer updates; a compile-link-run regression covers their layout
  and runtime behavior.
- Aggregate-pointer arrays retain complete typedef dimensions through
  `typeof`, address casts, and indexed assignment, including multidimensional
  pointer tables used by per-CPU IRQ vectors.
- Typed pointer elements in aggregate arrays now preserve aggregate pointer
  depth past the `C_BAD` type-tag sentinel. A compile-link-run regression
  exercises pointer-to-pointer array reads, member assignment, and a volatile
  `WRITE_ONCE` store; Linux `kernel/rcu/rcu_segcblist.c` compiles with NYC and
  assembles through the configured GNU `as` route.
- Address-of now accepts aggregate array elements reached through a function
  returning a pointer-to-pointer, and expression statements preserve indexing
  after such a call for pointer assignment. A compile-link-run regression checks
  both the returned address and the indexed store.
- Transparent-union pointer parameters accept compatible pointer arguments and
  lower them through the union's aggregate ABI representation. A
  compile-link-run frontend regression passes; Linux `fs/splice.c` now compiles
  with NYC and assembles with GNU `as`. Const-qualified pointer declarations in
  `for` initializers are accepted; Linux `fs/binfmt_misc.c` also compiles and
  assembles with the saved Kbuild configuration.
- Parenthesized address-of now preserves direct global lvalues used by
  `WRITE_ONCE`; the focused C frontend suite passes, and Linux `mm/folio.c`
  compiles with NYC and assembles with GNU `as`.
- NYC lowers runtime-constant pointer inline assembly for arbitrary C symbol
  section names while retaining the immediate/constraint checks; a
  compile-link-run regression covers a non-default section symbol.
- NYC lowers faultable x86-64 user-memory loads with asm-goto exception-table
  fixups for byte through qword widths, including aggregate-typed memory
  operands; a compile-link-run regression checks returned values and preserves
  the fault target through NYIR passes.
- NYC lowers the non-asm-goto x86-64 user-load fallback with typed exception
  table entries and register outputs; a compile-link-run regression exercises
  a word load into a wider destination.
- GNU `void *` increment now advances by one byte, matching Linux's
  no-fault copy loops; a compile-link-run frontend fixture checks the result.
- NYC accepts GNU `cleanup` attributes in the type-specifier position and runs
  the cleanup function at scope exit; a compile-link-run regression checks it.
- NYC's i386 GNU assembly route passes aggregates by value on the stack,
  returns aggregates through the SysV hidden result pointer, and preserves
  exact copy sizes and floating-point argument placement. A freestanding
  32-bit execution regression exercises NYC/GCC interoperability in both
  directions for three-byte, mixed-field, and larger aggregates.
- i386 spill homes preserve their full byte ranges and loop-invariant
  lifetimes across backedges, preventing overlapping homes from corrupting
  floating-point fields and aggregate loops.
- C variadic functions retain their call boundary instead of inlining
  frame-dependent `va_start`/`va_arg` operations into callers.
- Alias-based store sinking respects inline-assembly memory effects,
  volatile operations, and other observable side effects; an optimized
  atomic qword OR regression checks ordering with an earlier store.
- Unsupported assembly in dead branches is discarded after control-flow
  folding for non-static functions too; reachable unsupported assembly still
  fails code generation. Linux's size-selected compare-exchange macros can
  therefore discard their non-selected operand-width alternatives.
- Aggregate C declarations now accept GNU type attributes after qualifiers,
  including `__seg_gs __attribute__((no_instrument_function))`; a frontend
  parser regression covers this Linux per-CPU declaration spelling.
- The C frontend accepts the Linux flexible-array `__counted_by__` field
  attribute, and `__typeof_unqual__` is recognized in type positions used by
  `READ_ONCE`; focused parser regressions cover both forms.
- NYC supports `-fplan9-extensions` for tagged aggregate members without a
  declarator, preserving their layout and promoted fields; a static layout
  regression checks the anonymous member offset and total size.
- Block-scope function prototypes accept leading and trailing GNU attributes,
  and NYC lowers Linux `unsigned __int128` multiply-by-32-bit and
  multiply-by-64-bit, add, and right-shift sequences; linked regressions cover
  `mul_u64_u32_add_u64_shr` and cross-limb 64-bit products. Casts to and from
  the internal 128-bit representation also retain the correct low and high
  words.
- External aggregate references preserve their declared mutability when
  taking member addresses, and explicit pointer casts retain pointee
  qualifiers; a frontend regression covers the kernel's volatile `WRITE_ONCE`
  cast pattern.
- `__typeof_unqual__` participates in type lookahead inside nested `typeof`
  expressions, covering Linux per-CPU pointer casts with a frontend regression.
- The block-scope fallthrough statement accepts both `fallthrough` and
  `__fallthrough__` GNU attribute spellings used by Linux switch helpers.
- x86-64 inline-assembly `xaddb`, `xaddw`, and `xaddl` now lower alongside
  `xaddq`; signed byte and word plus unsigned byte and signed dword and qword
  compile-link-run cases check the updated memory and correctly extended prior
  value.
- Read-write qword `xchg` inline assembly now accepts compatible pointer
  operands; compile-link-run coverage exercises integer and pointer exchanges
  through direct and pointer-based memory lvalues.
- `sizeof expression` now handles unparenthesized aggregate-member arrays, and
  qword compare-exchange accepts compatible object-pointer operands such as a
  `void *` null expected value. `typeof(function)` pointer casts retain a
  callable function type; file-scope assembler definitions may provide symbols
  declared as external callable functions.
- `typeof(aggregate_pointer->scalar_field)` no longer inherits stale function-
  pointer metadata from the expression that produced the aggregate pointer.
  A compile-link-run regression covers volatile stores through that inferred
  scalar type, matching Linux `WRITE_ONCE` expansions.
- `typeof(*ops->callback)` declarations retain function-designator semantics;
  indirect calls through a GNU statement expression now have a compile-link-run
  regression, covering Linux static-call trampoline expansions.
- x86-64 `syscall` lowering accepts fixed-register locals only when each
  argument's declared register matches the syscall ABI slot. A getpid
  compile-link-run regression exercises `rdi`/`rsi`/`rdx` bindings.
- Static-function pruning now traverses reachable calls and asm references
  once with a worklist instead of rescanning every reachable function per
  discovery round; the C frontend regression suite passes.
- Function declaration lookup now uses an open-addressed name index instead
  of scanning the growing declaration table. Typedef-name lookup uses an
  lazily rebuilt index after scope rollback; a compile-link-run regression
  covers nested block typedef lookup and restoration.
- C tokenization now dispatches multi-character punctuator checks by leading
  byte instead of probing the full operator table for every punctuation token;
  lexer and frontend regressions pass.
- C tokenization skips runs of horizontal whitespace in one bounded scan while
  preserving source columns; the lexer regression covers tab, form-feed,
  vertical-tab, and spaces.
- C token text equality rejects mismatched leading bytes before scanning the
  candidate spelling; lexer regressions cover exact and near-match tokens.
- Variadic functions that reserve a register-save area now use a frame whose
  epilogue restores the dynamically adjusted stack pointer.
- NYC specializes Linux's always-inline alternative WRMSR helper so its CPU
  feature immediate remains static, emits the alternative and replacement
  sections, and assembles the saved Linux 7.3 `efi_64.c` input with GNU `as`.
  A compile-and-assemble fixture covers the metadata and replacement.
- NYIR's general inliner now materializes a dead dummy result when cloning a
  void call whose builder-assigned destination is unused, allowing the
  alternative WRMSR helper's void return to inline safely.

<details>
<summary>Details</summary>

- Tuple representation inference now bounds recursive global-alias and
  `clone` traversal, conservatively handling cyclic initializers instead of
  recursing until the compiler hangs. The focused
  `recursive-global-alias-classification` fixture passes.
- Dynamic-list classification now tracks active global/member aliases and
  bounds recursive query depth. Cyclic aliases conservatively classify as
  dynamic instead of overflowing the compiler stack. Cold focused runs passed
  both the recursive-alias regression and Wayland repeated-focus fixture with
  `NYTRIX_CORE_BC_THREADS=1`.
- x86-64 fused bit-mask selects now keep the tested value live through the
  select, preventing stack-home reuse from changing the branch result. A C
  regression checks both set and clear mask cases; it also fixes SQLite's
  `WITHOUT ROWID` root-page initialization path. Variadic `va_start` also
  accounts for saved callee registers when locating stack-passed arguments;
  the variadic regression covers a callee-saved loop home and an overflow
  argument.

### Performance

- NYR redundant-load elimination forwards vector store values to later loads
  from equivalent addresses when no intervening memory write or effectful call
  can invalidate them. Tests cover equivalent computed addresses and the call
  barrier. The `dgemm` checksum remains `1`; a focused 3+1 run measured
  211 µs NYR versus 47 µs LLVM and 18 µs C, so its runtime target remains open.
- The x86-64 AVX encoder now uses a spilled, unseeded vector operand's canonical
  stack home directly when it is a binary operation's memory-capable RHS. The
  SOR object no longer reloads and re-saves its broadcast multiplier each inner
  iteration; checksum `1725461952` passed. Focused 3+1 and 7+2 runs measured
  2.84 ms and 2.83 ms NYR, respectively, so this code-generation reduction has
  not established a runtime speedup; SOR remains slower than LLVM and C.
- The default benchmark `NYC` column reports execution of NYC-generated C code,
  not compile throughput. The host driver assembles and links NYC output;
  the C and NYC routes share their timing wrapper and checksum contract.
  Build and execution failures remain visible instead of falling back to
  host-compiled C. Terminal, JSON, CSV, and Markdown reports retain NYC runtime
  samples, compiler identity, and diagnostics. `NYC/C` compares NYC runtime
  against host-C runtime, with lower values indicating faster NYC execution.
- `straight` now repeats the same runtime-seeded 31-bit integer recurrence in
  Ny and C rather than timing a sum folded to a constant. NYR, LLVM, host C,
  and NYC retain checksum `5050` and execute observable work above the
  unchanged 1µs signal floor. The empty `startup` control remains empty.
- The `revcomp` fixture processes four reverse-order bytes per loop iteration
  and retains a scalar tail. NYR and C produce checksum `150000100000`; an
  isolated 7+2 run measured 0.099 ms NYR and 0.011 ms C.
- The `bce-matrix` benchmark now uses a contiguous `i64buf` matching the C
  reference's 64-bit element layout. The focused benchmark passed with checksum
  `14439925760000`; 3+1 samples measured 16.9 ms NYR versus 10.3 ms LLVM
  (1.64x), within the native/LLVM gate.
- The native backend recognizes exact bounded integer list fills and initializes
  the typed buffer through a range helper instead of emitting an element-wise
  append loop. `list-any-append` retained checksum `2,999,999`; seven isolated
  samples measured 8.35 ms NYR versus 9.34 ms LLVM (0.89x).
- Dynamic integer appends to descriptor-backed `Any` lists now write the
  in-capacity slot directly after validating the buffer header, avoiding a
  redundant generic append and tag-stamping pass. `list-any-append` retained
  checksum `2,999,999`; seven isolated samples measured 2.63 ms NYR versus
  3.80 ms LLVM (0.69x).
- The `dgemm` benchmark imports only its required time and typed-buffer
  modules instead of the umbrella core module. A cold phase profile measured
  83.9 ms JIT compilation versus the prior 2.63 s, with checksum `1`; runtime
  remains a separate optimization target.
- BigInt now provides `bigint_mul_div` for a scalar multiply followed by scalar
  division. Its runtime path performs both operations over one temporary limb
  buffer and allocates only the final result; the PIDigits fixture uses it
  without changing its recurrence. Focused BigInt tests cover multi-limb values,
  signs, and truncation; the PIDigits performance comparison remains pending.
- Native-only AOT now compiles runtime helpers with the selected speed profile
  instead of always using size optimization. The cold 7+2 `append-scaling`
  benchmark improved from 8.10 ms to 2.33 ms NYR; NYR/LLVM fell from 4.53x
  to 1.08x, with checksum `15000003`.
- The unified-dispatch parity probe now compares `get` and `install` help
  byte-for-byte with canonical `pkg` commands and reports command failures
  separately from output mismatches.
- Benchmark rows with sub-1µs measurements retain nanosecond timings and report
  `LOW` instead of `ok`; their ratios remain excluded below the signal floor.
- Focused 3+1 runs report `dce` and `escape-loop` above the timer floor, with
  matching NYR/C checksums (`16636` and `549732`). `gcbench` now allocates
  zeroed 96-byte raw node blocks and a raw retained-pointer table matching its
  C reference's `calloc`/`malloc` layout and release pattern. Raw pointer-range
  removal is constant-time, and the provenance table has 65,536 buckets for
  high-live-set workloads. The focused run preserved checksum `554061` and
  measured 84.3 ms NYR, 88.0 ms LLVM, and 22.8 ms C, down from the earlier
  140.1 ms NYR baseline; a 3.70× NYR/C gap remains.
- The raw-pointer registry reuses metadata nodes from 256-node slabs and
  releases excess empty slabs, avoiding one heap allocation per registration.
  Its hash table now has 262,144 buckets rather than 65,536 to shorten chains
  for large live sets. Focused `gcbench` runs preserved checksum `554061` and
  measured 55.5–61.1 ms NYR, versus 66.0–73.4 ms controls; C was 22.2–23.7 ms
  and LLVM was 51.2–64.6 ms across those runs. NYR still trails both
  references, and environmental variation prevents claiming a stable
  controlled improvement. `rt_free_raw` now checks the raw-allocation
  registry first, skipping typed-buffer and managed-heap checks for registered
  raw blocks. Checksums remained `554061`; with that change, the 65,536-bucket
  control measured 56.7 ms NYR and the 262,144-bucket candidate measured
  51.8 ms. A five-fixture startup cohort measured 49.8 ms NYR, 45.8 ms LLVM,
  and 23.2 ms C on `gcbench`. These results suggest shorter chains help but
  do not isolate the fast-free change against a baseline.
- Signed division by a positive power of two now lowers through the exact
  truncation-toward-zero bias formula when range analysis cannot prove the
  dividend nonnegative. The x86-64 machine lowering fuses the single-use
  sequence back to its immediate-divisor form. Focused signed-boundary and
  machine-fusion tests pass; the `binary` fixture checksum passes with 4.75 ms
  NYR, 438 µs LLVM, and 1.08 ms C, so NYR remains slower than both references.
- In-loop if-conversion now admits up to 20 cost units and can hoist raw-i64
  local reads only when a dominating store or parameter initializes the slot
  and the slot is not address-taken. It also converts matched raw-i64 local
  stores from both arms into a select and one store; unmatched writes and
  arm-mutated reads remain excluded. Focused tests verify the paired-store
  transformation, loop result, and rejection of uninitialized reads. The
  focused `binary` run measured 1.86 ms NYR, 317 µs LLVM, 1.04 ms C, and
  8.81 ms NYC with checksum passing; the captured optimized `branch_mix` still
  contains a conditional branch, so the earlier 1.82–1.83 ms versus 4.78 ms
  budget-10 timing difference is not attributed to this change. A focused
  `sor` run measured 2.99 ms NYR, 571 µs LLVM, 593 µs C, and 10.9 ms NYC with
  checksum passing. NYR remains slower than both runtime references.
- The `sor` fixture now computes left/right row bases once per row in both Ny
  and C, preserving the operation order and checksum `1725461952`. A focused
  3+1 run measured 2.938 ms NYR versus 0.555 ms C, down from 3.347 ms NYR;
  the remaining NYR/C gap is 5.29×. The one-vector unroll candidate was slower
  and was not retained.
- The x86-64 vector emitter now folds a YMM RHS load into the consuming
  arithmetic instruction when its SIB index is spilled, reloading that index
  into reserved RAX and using it directly in the memory operand. The SOR
  checksum remained `1725461952`; a focused same-revision 3+1 comparison
  measured 2.96 ms NYR for control and 2.83 ms with the fold; a repeat measured
  2.80 ms. Separately, disabling/enabling RSI/RDI loop homes measured 2.88 ms
  and 2.81 ms in focused 3+1 runs with checksum `1725461952`. NYR remains
  slower than LLVM and C on SOR. The native read-only loop regression now
  executes a variable shift and integer division while preserving its carried
  values; focused loop-call, fixed-register, and vector tests pass.
- SLP vectorization rejects store groups with non-contiguous destination
  addresses before cloning candidate IR; full legality checks still govern
  accepted pairs and quads.
- SLP candidate checks now share the per-round use-def analysis instead of
  rebuilding it for every candidate.
- Integer-reduction loops can unroll two vector chunks while chaining each
  cloned reduction into the loop-carried accumulator. Fixed-trip reductions
  retain the scalar epilogue for non-multiple trip counts; `bce-matrix` kept
  checksum `14439925760000` and measured 14.41 ms NYR versus 1.58 ms C in a
  focused seven-run sample. A 101×101 tail case also matched NYR/C checksums.

### Runtime correctness

- String slices with an omitted start and stop and step `-1` now reverse
  Unicode strings in the REPL and native execution paths.
- Boolean ternary joins now normalize imported boolean calls and literal
  arms consistently; this restores blending for OpenGL atlas text. Raw
  callback stores preserve callable addresses, and JIT execution clears
  artifact-origin code sections that could truncate callback relocations.
  Focused regression and UI evidence is recorded in
  [the consolidated TODO evidence record](TODO.md#archived-compiler-replay-and-ui-acceptance-evidence).
- The native x86-64 encoder invalidates cached address-home tracking after
  dereferencing memory, preserving loaded bytes when stack homes are reused.
  Repeated snapshot ownership and PNG checksum/pixel checks now pass native,
  MCJIT, ORC, and VM.
- Sequence indexed stores decode boxed `any` indices before calling raw-index
  runtime setters, fixing out-of-range writes during Vulkan font loading.
- UI profiling clocks consistently use raw monotonic timestamps. The native
  dictionary provenance cache keeps saturated-cache misses bounded while
  retaining checked fallback for uncached dictionaries.
- Wayland initializes its window registry at registration and normalizes
  surface-address keys, restoring live keyboard lookup and paddle input.
- Wayland xdg configure and close callbacks now stage per-window state in native
  callback storage; the event pump applies resized geometry and close requests
  in normal runtime context. Live Hyprland tiling now updates the client and
  framebuffer sizes instead of leaving Pong at its initial 1280×720 extent.
- Wayland cursor constraints now preserve manager proxies, destroy each
  constrained-pointer proxy only once, and reuse existing locked/confined
  proxies on repeated cursor-mode updates. This prevents live cursor-selection
  updates from invalidating Wayland protocol objects or constraining a surface
  twice.
- UI auto-dump readiness now has an explicit `bool` return type, preserving its
  result when assigned to native capture state. A live Vulkan run produced and
  read back a PNG capture.
- Pong scales the court and HUD to live Wayland framebuffer dimensions, keeps
  side margins proportional in tiled layouts, clamps the ball after a resize,
  and retains readable minimum HUD sizes. Wayland cancels keyboard repeat when
  focus leaves; Pong frame pacing remains bounded when vsync is advisory.
- Dynamically typed list indexing now uses the strict source-level bounds
  contract, so out-of-range reads panic instead of returning the fallback value
  used by generic `get` operations.
- Native calls from within a stdlib module now retain their runtime bridge
  binding when the callee is in the same module. This prevents AOT output from
  referencing an un-emitted stdlib wrapper body.
- `core.os.file_write` now returns the raw byte count in its `Result<int, int>`
  payload instead of exposing the tagged integer encoding to AOT callers.
- Core `Vector2`/`Vector3`/`Vector4` constructors now preserve floating-point
  component carriers when forwarding optional components in native AOT code.
- Sorting a native typed-buffer list now clones its buffer layout before
  sorting, preserving element data and list/tuple tags instead of interpreting
  the buffer header as a managed-list payload.
- Documentation search subsequence scoring now uses the candidate position as
  its match sentinel. This keeps private-name scores consistent between AOT
  and native-only execution.
- JSON string parsing now retains escape detection across its scan, so short
  escapes and Unicode escapes—including U+0000 followed by more bytes—are
  decoded instead of copied verbatim.
- Image loading keeps file-read bytes string-typed and decode results dynamic
  across the generic `Result` and `any` boundaries, preserving PNG dispatch.
- UI replay traces report each scripted event once even when the event is
  applied to multiple eligible windows.
- NYIR VM nested calls skip per-instruction profile buffers and counters unless
  profile data is requested; the existing evaluator API keeps its profiling
  behavior unchanged.
- Nested NYIR VM calls verify each immutable callee once per VM evaluation,
  while the public evaluator entry points continue to verify every standalone
  input.
- Per-fixture test timeouts can now reach 420 seconds while the global
  `--timeout` cap remains 300 seconds. The engine VM replay now uses a
  five-frame event sequence, retaining the resize, key press/release, and close
  checks while removing one redundant render from the slow VM route.

### C frontend

- Incomplete array typedefs now retain array identity and adjust to element
  pointers in function parameters. A compile regression covers an incomplete
  array typedef over a tagged aggregate.
- Block-scope `extern` declarations for functions and external integer arrays
  are now collected into the translation-unit symbol table. Frontend coverage
  includes a late function prototype and an incomplete unsigned-byte array.
- Aggregate double-pointer casts now retain the cast target's struct metadata
  when the source already has the same pointer depth. A compile-link-run
  regression covers SQLite's `sqlite3_vtab *` to `echo_vtab **` member access
  pattern through a nested base struct.
- Aggregate type qualifiers after a struct or union tag are now parsed before
  pointer declarators, including `struct T const *` parameter forms.
- The x86-64 machine backend now lowers SysV `va_start` and scalar `va_arg` for
  integer and double values. A compile-link-run regression exercises both
  register save areas, and the preprocessed SQLite `test_syscall.c` compiles
  and assembles with NYC.
- x86-64 constant rematerialization no longer recursively rescans earlier
  shift instructions while checking for fixed-register clobbers. This lets the
  preprocessed SQLite `test8.c` compile and assemble with NYC.
- Function typedef declarations can be completed by later matching function
  definitions, including the `Tcl_ObjCmdProc` pattern in SQLite's test sources.
  Opaque pointer-array indexing, pointer atomics, `unsigned short **` stores,
  and addresses of file-scope character-array elements now lower through the
  C frontend.
- A file-scope definition can now complete a compatible external string
  pointer array declaration, preserving the signed `char *` element type. The
  frontend regression links and checks the initialized second string. Nested
  aggregate initializers also accept one-dimensional array designators such
  as `.extent[0]`; the new regression runs successfully, and Linux 7.3's
  `kernel/user.o` compiles with NYC (with an outstanding `objtool` warning
  about `panic()`'s missing visible `__noreturn` annotation).
- The x86-64 emitter supports functions with more than 16,384 virtual values.
  Indexed memory fusion now extends base and index live ranges to the access,
  preventing stack-home reuse from corrupting fused pointer-array stores.
  Compiler-generated static aggregate initializer data also receives local
  ELF binding, so separate translation units can use the same private symbol.
- NYC assembly output provides weak unsigned-64-to-float conversion helpers.
  The feature-flagged SQLite 3.46.0 testfixture now compiles and links with NYC,
  GNU `as`, Tcl, and the host linker. x86-64 stack-argument loads now account
  for saved callee registers in the prologue, and narrow global-array indexing
  uses byte and halfword element strides. A compile-link-run regression covers
  calculated indexes into a file-scope byte array, and the C frontend suite
  passes. File-scope scalar-array address lowering now uses the element pointer
  type, fixing SQLite keyword lookup's character-table offset; the fixture
  executes `select 1`. Fresh-output JSON, cache, and lookaside suites pass under
  Valgrind with no errors or leaks; a separate NYC-built shell without optional
  line editing passes SQL, persistence, transaction, JSON/blob, `VACUUM`, and
  integrity-check smoke queries. The upstream full `testrunner.tcl all` run is
  underway.
- `__builtin_ffs` now lowers zero and nonzero integer inputs correctly; a
  compile-link-run regression covers both and checks several bit positions.
- The x86-64 `bsfl` output plus `rm` input and tied `0` input form used by
  Linux's `__ffs` lowers through the count-trailing-zero sequence while
  preserving the `-1` zero-input result. A runtime regression checks both
  zero and set-bit cases.
- Linux file-scope asm now accepts bare section switches, default section
  shorthands, `.global`, numeric `.short`, filled `.balign` and `.skip`,
  `endbr64`, and PC-relative direct jumps. NYC also lowers x86 `inb`/`inw`/`inl`,
  `outb`/`outw`/`outl` with Linux's `Nd` constraint, and volatile 32-bit
  memory-load asm. The linked frontend regression passes, including a runtime
  load check. Preserving Kbuild's `-fconserve-stack` flag selects NYC O1, and
  targeted `kernel/sys.o` and `arch/x86/kernel/quirks.o` builds succeed with
  NYC and GNU `as` without `objtool` warnings; full Linux `bzImage` and modules
  acceptance remains open.
- Port-output inline-asm IR now retains the recognized instruction template,
  satisfying verifier checks for `outb`/`outw`/`outl`. The Linux frontend
  fixture also checks a 32-bit-shaped paravirtual-call operand list; its linked
  executable passes, and GNU `as` assembles the port-I/O function without
  executing the privileged instruction.
- Struct fields can use the SysV x86-64 `va_list` array type, with its 24-byte
  size and 8-byte alignment. The frontend regression checks the resulting
  structure size.
- Indexed writes to file-scope character arrays now use byte elements instead
  of treating the array as pointer storage. A compile-link-run regression
  covers indexed writes observed through a pointer alias.
- NYC recognizes `_Float32`, `_Float64`, and `_Float32x` as the corresponding
  32-bit and 64-bit floating types. An x86-64 host-linked runtime probe checks
  their argument and return ABI; SQLite fixture compilation now advances past
  these declarations and stops at the unsupported `_Float128` type.
- Function-name literals and function type attributes between the return type
  and declarator are lowered, covering Linux kernel declarations such as
  section-attributed functions.
- Linux's `mov` input to x86 debug registers now lowers through both NYC's
  direct object writer and x86-64 machine-form encoder. The C frontend suite
  passes, and `arch/x86/kernel/traps.o` compiles with NYC and GNU `as`.
- Function-scope aggregate definitions can shadow file-scope tags. A
  compile-link-run regression checks a local `struct item` against a global
  tag with the same name; the upstream SQLite `test1.c` probe now advances
  through the previous duplicate-tag diagnostic.
- Static initializer relocations can refer to a function-local static object,
  using its generated data symbol. x86-64 NYC lowering also supports
  truncating an F64 value to I64; the preprocessed SQLite `test1.c` now
  compiles and assembles with the testfixture feature flags.
- x86-64 `a`, `c`, and `d` inline-assembly input constraints lower to their
  fixed registers. Scalar and aggregate memory inputs lower to address operands
  in assembly output; memory plus `Ir` register inputs lower through `%rdx`.
  External C data and function references use their linker-visible names in
  emitted assembly, while internal NYIR names remain distinct.
  Signed 32-bit `+r`/`+m` read-write pairs lower for x86-64 atomic
  instructions such as `lock xaddl`; a host runtime regression checks both
  returned register and updated memory values. The `=@ccz`, `+m`, `+a`, and
  `r` compare-exchange combination preserves the observed old value and success
  condition for signed and unsigned 32-bit values; runtime regressions cover
  success, mismatch, and zero-extension. Named immediate-only asm inputs
  preserve integer constants initialized through `__auto_type`. Named
  `er` inputs are accepted with memory outputs and zero-flag results.
  Named memory-asm register operands use the register width implied by the C
  input type, including 32-bit operands such as `addl`.
  x86-64 function emission accepts aggregate parameters and returns aggregate
  register classes through the SysV result registers.
  Function-type parameters written as direct function declarators are adjusted
  to function pointers in function declarations.
  File-scope integer constant expressions support bitwise AND, XOR, OR, and
  complement, including aligned array bounds used by Linux headers.
  Global function-pointer variables can be called from C functions, chained
  aggregate array elements retain modifiable lvalues, and
  `__builtin_prefetch` is accepted as a non-observable optimization hint.
  `typeof` preserves the return type of aggregate-member function pointers
  through local declarations and indirect calls.
  Callback signatures now survive `_Generic` selections, including the
  statement-expression and volatile dereference pattern used by Linux's
  `READ_ONCE`. Taking an aggregate-member address through a selected `_Generic`
  lvalue is supported. Parenthesized addresses of external aggregate-pointer
  array elements now retain the pointer-to-pointer type, covering the
  `READ_ONCE` cast; a compile-link-run regression checks the loaded element.
  Label collection ignores `default:` associations in nested expressions
  rather than treating them as duplicate function labels.
  Aggregate local assignments and mixed-width integer inputs to signed
  32-bit overflow builtins are supported. Empty aggregate parameters retain
  zero-sized ABI metadata and consume no argument registers. File-scope
  integer-array assignments use the element pointer type for indexed stores;
  indexed signed and unsigned 32-bit global array reads retain modifiable
  element lvalues in expression assignments, and pointer-array elements retain
  their addresses. Assignments to file-scope pointer objects remain writable
  when their pointee type is const. NYC assembly output preserves ELF section
  merge, string, and thread-local flags and emits file-scope data relocations
  in offset order;
  `__builtin_memcmp` resolves to the standard comparison function. GNU
  `,##__VA_ARGS__` removes the comma when optional variadic arguments are empty.
  Falling off a non-aggregate non-void function emits a zero return for C's
  undefined fallthrough case. Comma expressions parse through complete
  `if`/`while`/`do-while` conditions. Prefix/postfix pointer increments retain
  the pointed-to aggregate type for subsequent member access. Taking the
  address of a function designator preserves its function-pointer type,
  including when the designator is returned from a GNU statement expression
  and called immediately.
  Address-of on parenthesized character-array members preserves their address
  for kernel control-block casts.
  Switch cases accept constant expressions using `__builtin_bswap16`.
  Static local arrays can carry section attributes and retain their static
  storage duration. The x86-64 `tzcnt %1,%0` inline-assembly form with an
  `=r` result and `rm` input lowers through the hardware `tzcntq` instruction.
  Carry-flag output constraints `=@ccc` lower to a byte result after the
  assembly template, and `=@ccnz` supports memory inputs plus register-valued
  masks. Empty volatile asm with a register input and `memory` clobber is
  accepted as a compiler barrier.
  Named immediate and absolute operands substitute `%c[name]` and `%a[name]`
  placeholders; integer locals initialized with integer constant expressions
  are accepted as named `i` operands until written or address-taken. Named
  Named `=q`/`=r` register outputs and global-address immediate inputs are supported.
  Whole-unit optimization inlines `static inline` C helpers before assembly
  emission; x86-64 Linux jump-label `asm goto` carries its label edge through
  NYIR CFG construction, DCE, SSA compaction, and symbolic operand substitution.
  `lock incq %0` with matching `=m` output and `m` input constraints lowers to
  an atomic memory increment; a compile-link-run regression checks the update.
  The `=@ccz`, `+a`, `+m`, and register-input `cmpxchgq` form supports 64-bit
  operands with a GS-segment memory override and runtime coverage.
  Optimized return-address and frame-address builtins now store through their
  allocated NYIR value homes; an optimized assembly runtime regression covers
  the frame-address path.
  Indexed byte loads use a valid 32-bit destination register, and named
  `movl` register outputs select the matching 32-bit register spelling.
  `typeof` on a string literal preserves its character-array extent for
  aggregate member declarations such as Linux ELF notes. File-scope
  `_Generic` selections initialize integer and pointer fields; constant pointer
  conditionals select their evaluated arm. Incomplete tentative character
  arrays can be completed by a later initialized declaration, and a matching
  `extern typeof(object)` declaration remains valid after its definition.
  Comma-separated uninitialized scalar globals with section attributes are
  collected as one declaration; file-scope function-pointer arrays accept
  designated indices and preserve zero-initialized gaps. `typeof` recognizes
  string literals nested in `sizeof` expressions used by constant array bounds.
  `_Generic` association expressions are type-checked without evaluating every
  branch; void-valued GNU statement expressions remain valid associations.
  Address-of scalar compound literals, parenthesized aggregate compound
  literals in constant initializers, direct aggregate-member addresses,
  parenthesized file-scope object addresses in pointer initializers, pointer
  arithmetic and indexed stores through file-scope pointers, and
  dereferencing pointers to scalar typedef arrays are supported. Braced scalar
  globals, scalar-array index/range designators, and aggregate array
  designators preserve zero-initialized gaps;
  conditional expressions infer compatible static types for `typeof` and
  `sizeof`. x86 Linux static-CPU-feature asm-goto sequences lower to equivalent
  runtime capability-byte tests; the x86 LA57 `task_size_max` alternative
  selects the small/large task limit from `boot_cpu_data`, and the IBPB
  alternative calls `write_ibpb` only when the boot CPU has the feature;
  the AMD DIV0 workaround executes its safe division only for affected CPUs;
  x86 register outputs unescape GNU `%%` register literals before emission.
  Both `__attribute__` spellings are
  accepted in type and function declarations, and file-scope typedef-based
  pointer-array definitions complete matching extern declarations. The x86-64
  frontend lowers dynamic `__builtin_alloca` sizes to aligned stack allocation;
  GNU `__label__` declarations resolve repeated local label names by position,
  and `__auto_type` locals with cleanup attributes are supported in `for`
  initializers.
  Linux-focused regressions assemble and execute these paths.
  The native x86-64 backend reads global register variables bound to `%rsp`.
  Named write-only memory outputs in GS-relative x86 asm lower with their
  input operand preserved; a runtime regression assembles and exercises the
  GS-relative byte store.
  The x86 unsigned index-mask `cmp`/`sbb` idiom lowers to an unsigned bounds
  comparison and all-ones/zero mask, with boundary runtime coverage.
  File-scope object declarations accept GNU `visibility("hidden")` and
  `visibility("default")` attributes used by the Linux vDSO data page.
  Constant-size memory-copy builtins lower to inline struct copies, and
  unreferenced function-local static pointer tables are omitted so vDSO
  objects do not gain dynamic relocations from dead tables.
  Static array-element pointer relocations remain recognized through nested
  casts and aggregate fields named after function typedefs.
  Empty GNU asm statements with read-write `+rm` operands preserve the
  operand while emitting no machine instruction.
  The x86 ordered-RDTSC alternative lowers to `lfence; rdtsc`, then splits
  EDX:EAX into its requested high/low C locals.
  The x86-64 vDSO syscall asm lowers to the Linux syscall call path, including
  tied-number and fixed-argument-register constraints; the vDSO CPU/node
  alternative uses its LSL implementation rather than feature-specific RDPID.
  Local `rdi`/`rsi` register bindings used as syscall inputs are treated as
  ordinary C locals because NYC passes their values through the syscall path.
  The vDSO robust-futex unlock idiom lowers to 32-bit atomic compare-exchange,
  preserving its pending-operation clear on success; unsigned division,
  remainder, and right shift lower to native x86-64 instructions without
  compiler-runtime helpers.
  The x86 runtime pointer-constant asm retains its runtime relocation section
  while returning the immediate through its register output.
  The NYC driver accepts lower-case `-fpic` and enables its position-
  independent code path for Linux vDSO translation units; the explicit
  `-fno-omit-frame-pointer`, `-foptimize-sibling-calls`, and
  `-fasynchronous-unwind-tables` spellings are accepted for kernel builds.
  NYC does not yet emit unwind-table metadata.
  x86-64 variadic functions save the SysV GP and vector argument registers in
  the 176-byte register-save area; variadic C frontend runtime tests pass.
  The C preprocessor's `__COUNTER__` builtin increments for each expansion.
  Macro-expanded angle/quoted include names retry with path-separator
  whitespace compacted, covering stringified Linux trace include paths.
  External objects may be declared with an incomplete enum tag before that
  enum is defined later in the translation unit.
  `typeof` of an aggregate's function-pointer member retains its function
  result type when used to declare a function symbol.
  Aggregate field layout supports three-dimensional arrays.
  Frontend regressions assemble and execute the forms that are safe to run.
- Multidimensional external aggregate arrays retain row strides during
  indexing. Pointer-to-array aggregate fields retain their pointee size for
  `sizeof`; local pointer-to-array declarations are accepted. Runtime
  regressions cover field sizing and local declaration behavior. Pointers
  obtained with `typeof(&external_array)` retain multidimensional scalar array
  extents through dereference and indexing. `sizeof(typeof(u64[64]))` also
  uses the complete array size. External globals declared through
  `typeof(u64[64])` retain their array extent, element type, and 64-bit indexed
  loads; parenthesized element addresses preserve the closing delimiters.
  `typeof(&multidimensional_array)` preserves pointee extents in local pointer
  declarations and applies the correct stride at each index.
  Address-of on a function-call aggregate member preserves pointer-valued
  fields, and locals shadow same-named function declarations in address
  expressions. `typeof` keeps aggregate-pointer type across `*&` expressions.
  Parenthesized string pointers in static aggregate initializers retain their
  relocations, and assignment of a compatible aggregate to a global emits a
  byte-preserving copy. x86-64 `get_user` inline assembly lowers its helper
  call and both register outputs without losing local values under
  optimization, including local `asm("%rdx")` output declarations. Read-write
  `bsrq` constraints preserve the initialized output when the input is zero
  and return the most-significant-set-bit index otherwise. Read-write x86-64
  `xchg` assembly supports byte, word, dword, and qword operands, including
  signed byte and word results; compile-link-run coverage exercises each width.
  Locked byte `xorb` supports `=@ccs` output and returns the post-operation
  sign flag; a compile-link-run regression checks the flag and updated byte.
  Locked 32-bit `addl` supports a read-write memory operand and register or
  immediate input; runtime coverage checks the atomic memory result.
  x86 POPCNT alternative templates use their software helper call,
  preserving correct behavior without CPU-feature patching. `rdmsr` and
  `rdpmc` retain their fixed-register contracts; `rdmsr` exception-table entries
  are emitted with the instruction;
  safe reads and writes preserve error-register encodings in exception fixups.
  Read-write `addq`/`subq` memory operands lower to ordered volatile
  load/operation/store sequences.
  `__builtin_strlen` of adjacent narrow string literals is constant-evaluated
  in file-scope expressions such as module metadata assertions.
  `typeof` infers arithmetic expressions rooted in numeric or character literals.
  File-scope aggregate arrays accept C99 index designators.
  File-scope integer arrays retain section attributes after the declarator.
  File-scope multidimensional integer arrays accept nested index designators.
  File-scope arrays of pointer-array typedefs preserve the typedef's element
  count and row stride; compile-link-run coverage checks row addresses.
  Static pointer initializers may decay an aggregate's array member to the
  member's address, with compile-link-run relocation coverage.
  Parenthesized function-typed parameters adjust to function pointers and can
  be invoked; a compile-link-run regression covers the call.
  x86-64 assembly output honors external and inline retpoline options for
  indirect calls; compile-link-run regressions exercise both modes.
  Opaque object pointers support element-scaled pointer arithmetic.
  File-scope function-pointer variables declared through typedef aliases
  retain function-symbol relocations.
  x86-64 `cpuid` inline assembly preserves fixed-register inputs and writes
  all four outputs through their destination addresses; a compile-link-run
  regression compares its results with the host compiler.
  x86-64 named register-output asm and GS-relative per-CPU reads, increments,
  decrements, register adds, and decrement-and-test outputs now lower to
  machine code; per-CPU symbol reads use signed 32-bit ELF relocations;
  `pushf`/`pop` save-flags outputs also compile. Tests assemble or compile each
  form.
  x86-64 `__put_user_%c[size]` inline assembly lowers the Linux user-copy
  helper's `%rcx` pointer, `%rax` value, and `%ecx` result convention; a
  compile-link-run regression verifies the store and return value.
  x86-64 read-write `cmpxchgq` outputs also accept pointer-typed values, as
  required by Linux's pointer ownership compare-exchange path.
  The classic `"=a"`, `"+m"`, `"r"`, and `"0"` qword `cmpxchgq` form used by
  Linux's `__raw_cmpxchg` now has compile-link-run success and failure coverage.
  `__builtin_ctzll` lowers 64-bit trailing-zero counts; compile-link-run
  coverage checks low, multi-bit, and high-bit inputs.
  Unary bitwise complement follows integer promotions and retains its 32-bit
  result width for signed and unsigned operands.
  Constant integer casts now truncate and sign-extend according to the
  destination width.
  GNU `__volatile__` is accepted as a field type qualifier without changing
  aggregate layout.
  Aggregate `deprecated` and `unused` attributes are accepted as
  layout-neutral metadata.
  Function-pointer typedef variables with file-scope initializers no longer
  collide with function-type declaration parsing; GNU placement attributes
  retain the initializer relocation.
  File-scope assembly labels are recognized only outside quoted operands, so
  colons inside adjacent `.ascii` strings remain data instead of being parsed
  as labels.
  `sizeof` on a local array uses its full element count and element size,
  including arrays of complete aggregates.
  Overflow builtins accept unsigned 32-bit result pointers and detect wrap
  for addition, subtraction, and multiplication.
  GNU zero-length array typedefs retain array identity and contribute zero
  bytes to aggregate layout; static assertions check the following field offset
  and containing struct size. Fixed-size, zero-initialized file-scope static
  pointer arrays now emit their storage, with compile-link-run coverage for
  character and aggregate pointers. Addressing a subscripted static pointer
  array through a parenthesized array expression also preserves the element
  pointer type, as required by Linux parameter-array declarations.
  x86-64 `lock andl`/`orl`/`xorl` accept a 32-bit `+m` output with an `ir`
  input and lower to locked memory operations; the C frontend test assembles
  and runs all three.
  GNU `bsrl` with a tied `-1` output/input now preserves the zero-input sentinel
  and reports the correct 32-bit highest-set-bit index.
  GNU statement expressions can now end at a local label before their value
  expression, including `goto` exits used by Linux wait macros; both paths have
  runtime coverage.
  Frontend regressions exercise the shadowing, type, helper-call, bit-scan,
  and privileged-instruction paths.
  Integer function-address initializers now
  retain relocations, and file-scope x86 assembly accepts `ret`, `int3`, and
  `nop`. File-scope aggregate-pointer arrays infer incomplete bounds from
  initializers and preserve member-address relocations. Compile-link-run
  regressions cover the array values, both relocation forms, and assembly shim.
  Repeated compatible `extern` aggregate-pointer declarations coalesce with a
  later definition instead of being treated as duplicate objects.
  Aggregate fields of type `void *` retain generic pointer semantics,
  including GNU comparisons with function-pointer sentinels and stores through
  struct members and `void **` lvalues.
  Incomplete aggregate arrays infer bounds from the highest C99 index
  designator, continue positional elements after it, and zero-initialize
  skipped entries, including local statics.
  File-scope string-pointer arrays accept enum-valued C99 index designators,
  preserve positional continuation, zero-fill gaps, and replace prior string
  or null elements when an index is designated again.
- File-scope aggregate initializers evaluate `sizeof` array type operands such
  as `sizeof(char[8])` and aggregate member operands; the frontend suite covers
  both constant initializers. Aggregate array bounds also evaluate valid
  arithmetic with parenthesized negative operands.

### Memory and locality

- Native programs retain the manual-memory default; tracing garbage collection
  remains opt-in.
- `str.repeat` now uses a runtime helper with one exact-size allocation and
  locality-friendly pattern copies. Copy blocks stay aligned to the source
  length, preventing repeated strings from changing phase at the 4096-byte
  boundary. The regression checks a non-divisor pattern across that boundary;
  both affected word-frequency fixtures pass cold correctness and focused
  benchmark runs. The seven-run focused benchmark measured 22.6 ms on NYR,
  78.9 ms on LLVM, and 92.5 ms on C; its checksum passed.
- `bce-matrix` runs a data-dependent row-major traversal long enough to clear
  the timer floor. Its focused benchmark measured 34.8 ms on NYR, 11.0 ms on
  LLVM, and 1.59 ms on C; NYR/LLVM was 3.16x, below the per-fixture limit.
- Linux sound dispatch converts dictionary-backed backend IDs before selecting
  open/start/write/stop operations. This fixes output initialization and
  playback through the standard PulseAudio protocol, including PipeWire's
  PulseAudio server in Wayland sessions; backend selection stays compositor-
  independent. The shared one-time library probe now returns a retained handle
  when its required symbol is present.
- The Brainfuck showcase links `libnytrixrt.so` relative to its source file,
  so running the script from outside the repository root no longer depends on
  the caller's working directory.
- Added `core.os.cli` for declared flags, typed and repeatable values, short
  clusters and families, generated help/completions, subcommands, conflicts,
  and response files. The Brainfuck example uses it for `-c CODE`,
  `SOURCE.bf`, and optional `--asm[=PATH]`; program output remains on stdout,
  while implicit assembly output goes to stderr.

### Verification — 2026-10-29

- HM guard narrowing now retains the binding's pre-guard type for assignments,
  so flow-only refinements do not become new assignment obligations. Rebinding
  expires the narrowed fact, including through nested guards. Cold tests pass
  for `mutable_aggregate_length_refinement_survives_literal_prefix`,
  `mutable_aggregate_append_preserves_nonempty_fact`,
  `mutable_aggregate_element_store_preserves_nonempty_fact`, and the
  constant/general-bound, exact-length/content, and nested-guard invalidation
  cases across all four execution variants.
- `json_for` now names its generated dictionary local consistently with the
  working `load_fields_for` template; the four-backend `codegen-load` fixture
  passes cold.
- Mutable declarations inside `comptime{}` now shadow rather than overwrite
  same-named outer HM bindings. The long-loop compile-time-limit negative case
  again reaches its intended diagnostic and passes all four variants cold.
- The reversed mutable-list length-bound regression now has a 120-second
  fixture timeout; its four cold execution variants pass in 77.1 seconds.
- The conjunctive mutable-list refinement transport regression now has a
  120-second fixture timeout; its four cold execution variants pass in
  81.0 seconds.
- The known-suffix indexed refinement regression now has a 120-second fixture
  timeout; its four cold execution variants pass in 88.6 seconds.
- The constant-expression mutable refinement store regression now has a
  120-second fixture timeout; its four cold execution variants pass in
  78.8 seconds.
- The proven indexed-store refinement regression now has a 120-second fixture
  timeout; its four cold execution variants pass in 89.9 seconds.
- The disjunctive indexed-refinement transport regression now has a
  120-second fixture timeout; its four cold execution variants pass in
  77.3 seconds.
- The local-write-before-guard regression now has a 120-second fixture
  timeout; its four cold execution variants pass in 76.1 seconds.
- The readonly fresh-aggregate refinement regression now has a 120-second
  fixture timeout; its four cold execution variants pass in 75.6 seconds.
- The UI frame-history probe now has a 120-second fixture timeout; all three
  cold variants pass in 122.5 seconds total.
- The HTTP server and fixture helpers use a disambiguated `core.os.args`
  alias, avoiding collision with `core.os.cli`; the CLI parser and fixture
  protocol probes pass cold.
- CLI help now renders a registry-based `--help all` reference and focused
  option groups from registry metadata; `--help gpu` combines GPU and parallel
  tuning options, including workload thresholds and asynchronous/fast-math
  switches. `-Os` now selects size-oriented optimization, while bare `-O`
  requires an explicit level and exits 2. Short help marks value-taking flags,
  lists the run/check/dump and other topic references, and explains the script
  argument boundary while staying within the 40-line limit. Bash enum
  completions are
  syntax-checked and exercised; Fish output includes enum candidates and no
  longer interprets legacy single-dash words such as `-run` as short letters.
  Compiler boundary scanning now gets separate-value behavior only from the
  option registry; the duplicated value-flag list was removed.
  `--gpu-opt=...` now consolidates GPU offload threshold, mode, target, object,
  async, and fast-math settings; invalid items and an off/force contradiction
  fail with exit 2. Equals and separate-value forms have cold regression
  coverage. `--opt-level=0|1|2|3|s|z` now accepts equals and separate values;
  `s` and `z` select size optimization consistently with `-Os` and `-Oz`.
  `--use=artifact=PATH,refinement-interface=PATH,effect-interface=PATH,`
  `native-bundle=PATH` now provides one canonical input-artifact option while
  the older specialized spellings remain accepted. Refinement/effect interface
  fixtures exercise the canonical form through NYR execution.
  `--nyr-limit=global-inst-cap=N,pass-growth=X,run-steps=N,run-recursion=N`
  groups NYR optimizer and VM limits; registry help and parser coverage expose
  its accepted keys, and the existing individual flags remain available.
  `--jit-opt=engine=...,opt-level=...,code-model=...,fast-isel,perf-map`
  consolidates JIT configuration without removing the original individual
  switches. `--nyr-check=verify,bisect,rollback,flaky,fingerprint,oracle,`
  `oracle-per-pass,tv-seed=N,fuzz=N` consolidates NYR verification and native
  translation-validation controls while retaining their individual legacy
  options. Invalid list items report the complete accepted item set and exit 2.
  `--native-tuning=budget=N,hot=N,cold=N,cache-score=N,prefer=vm|asm` and
  `--dwarf=version=N,locals,no-locals,split-inlining,no-split-inlining,`
  `profile-info,no-profile-info` consolidate existing native tier and DWARF
  controls. `--nyr-pass=disable:NAME,stop-after:NAME` groups NYR pass controls;
  all three lists validate items and preserve the original individual switches.
  `--emit=KIND[=PATH],...` now selects assembly, LLVM IR/bitcode, object, Wasm,
  NYR, metadata, report, artifact/interface, shapes, and native-bundle outputs.
  Omitted output paths derive from the input stem; explicit per-kind paths keep
  multiple outputs separate. The CLI registry probe runs its emission checks
  from an isolated temporary directory and removes their generated outputs
  instead of leaving files in the repository root. Object output uses the
  compiler's existing object emitter, and Wasm still requires a Wasm-compatible
  target. The Brainfuck CLI emits assembly only when `--asm` is present; its
  probe now runs in a disposable working directory so accidental relative
  outputs cannot pollute the repository.
  `-n` / `--print-flags[=json]` now prints recognized compiler options in
  canonical form and separates the exact program-argument array from them;
  JSON mode escapes code and argument strings. Deprecated value-taking aliases
  retain their values when rendered in canonical form.
  The short-family clusters documented by `--help short` now normalize through
  the same compiler option parser, including attached `-jN`, `-tTRIPLE`,
  `-oPATH`, and `-cCODE` values. Legacy spellings are recognized before
  cluster expansion; repeated list-family clusters merge into one canonical
  option. The legacy `-timeout` spelling is now preserved before `-tTRIPLE`
  expansion, so it canonicalizes to `--timeout` instead of being mistaken for
  a target. The legacy `-prof=LIST` spelling now accepts equals-form lists and
  canonicalizes to `--prof=LIST`. Ordinary long-option invocations bypass the
  expansion buffer.
  `+debug`, `+release`, `+ci`, `+inspect`, and `+perf` expand to discoverable
  built-in presets; later command-line flags retain override precedence.
  `--trace=LIST` selects lexer, parser, scope, inference, LLVM, register
  allocation, execution, and metadata traces; it accepts equals and separate
  values with exit-2 validation. `--dump=LIST` selects the available AST,
  tokens, LLVM, NYR, VM, escape, proof, native, and expansion inspection
  outputs, also with validated equals and separate forms.
  `cli-help-registry.nshape` covers generated help, JSON metadata,
  completions, option parsing, one-row-per-registered-option structure, and
  parser-to-registry membership. Setting `NYTRIX_CHECK_OPTION_REGISTRY`
  checks each successfully parsed option; CLI fixture runs exposed missing
  registry rows for `--emit-bc`, `--eval`, and `-e`, which are now documented
  and accepted as metadata aliases. CLI argument regressions now include executable
  shebangs, the first watch-mode run, and signal termination across JIT, AOT,
  and native execution. AOT child processes return the conventional
  `128 + signal` status instead of collapsing signal death into `-1`.
  `ny -c 'print(1)'` startup samples were
  54/57/55 ms before the latest registry/parser changes
  and 56/55/49 ms afterward (53.3 ms mean versus 55.3 ms); no measurable
  startup regression appeared in these small samples. Follow-up samples after
  `--gpu-opt` were 49/58/56 ms (54.3 ms mean), within the observed run-to-run
  variation. Three direct `ny -c 'print(1)'` smoke samples after the latest
  help and registry changes were 48/56/51 ms (51.7 ms mean), still within that
  run-to-run variation. Three direct samples after the latest parser fixes
  were 49/51/73 ms (57.7 ms mean); the 73 ms sample is an outlier against the
  earlier 48–56 ms range, so these samples do not establish a startup change.
  The option registry
  still does not cover every parsed spelling, so this is not yet a complete
  CLI reference. Registry-generated flag names, `=VALUE` placeholders, value
  choices, descriptions, and group headings now use the shared terminal color
  policy; `-h <topic>` routes to the same focused option references as
  `--help <topic>`; the `completions` topic documents supported shell names.
  Native translation-validation, NYR bisect/fingerprint,
  and debug-trace switches missing from the generated reference are now grouped
  under `dump`, `nyr`, `native`, and `trace`, including call-graph inspection
  and NYR dump-path controls. The `--dump-nyr` reference now states that the
  binary dump defaults to stdout and accepts `=PATH` to keep binary NYR separate
  from program output. The generated reference also includes the
  release/debug presets and compiler progress, glyph, linker, and
  source-extraction controls. `--help config` and `--help presets` now expose
  the existing configuration flags, environment path overrides, and compiler
  preset flags. Effect, alias, standard-operator, debug, DWARF, optimizer, and
  strip controls are represented in the option metadata. Legacy safety,
  ownership, stdlib, timeout, profiling, and execution spellings now have
  explicit replacements and are hidden from human help and completions while
  remaining in JSON metadata. Focused `--help --flag`
  lookups now suggest the nearest registered option for a misspelling.
  Deprecated spellings with replacements emit a note under `--warn=all`;
  post-script arguments are excluded. The
  registry also records Wasm output, safe-run limits, fallback listing,
  strict-type disable, gprof disable, cache-cleaning, native, core-bitcode,
  and LTO compatibility spellings.
  `--emit-report`
  now accepts both `--emit-report=PATH` and `--emit-report PATH`, including
  `-` for stdout. The native oracle's fuzz-count and profile-input options now
  accept registry-backed equals and separated values. Optimization pipeline,
  bundle-output, and dump-directory paths now accept separated values too.
- `argv-empty-boundary.nshape` covers `positionals_from` with and without `--`,
  last-flag precedence, short equals-form values, and dash-safe `value_from`.
  A missing option value no longer consumes a following flag; lone `-` remains
  a valid value.
- `cli-bare-inspection.nshape` checks 17 inspection options. Unsupported
  requests now exit 2 with a concise `--mode=bare` diagnostic; the matrix
  passes against a fresh compiler build.
- `cli-conflicts.nshape` checks the 12 documented contradictory-option pairs.
  The matrix caught two gaps: `-gc` with `-no-gc` was order-dependent, and
  `--repl` with `-c` escaped validation because the flag scanner skips command
  strings. Both now fail with exit 2 and name the conflicting options.
- Canonical `--std` and legacy `-std` now accept equals and separate-value
  forms, and `--target=host|wasm|TRIPLE` routes through the existing target
  settings.
  Invalid-choice handling for run, runtime mode, heap, native backend,
  optimization profile, GPU, parallelism, sanitizers, color, completions,
  warnings, standard-library mode, stop stage, native tier, and JIT settings
  plus native ABI, C frontend, solver, dump scope, accelerator target/object,
  legacy GPU target, ARM float ABI, debug format, desugaring, JIT
  optimization-level, profiling-mode, optimizer-list, check-list, and
  warning, ownership-check, and dump-scope values are checked by 36
  sequential compiler invocations in the cold
  `cli-enum-errors.nshape` regression. A compile blocker in the C parser was
  also fixed by declaring `statement_end` before its first use.
- Added registry metadata for previously omitted options including
  `--safe-mode`, `--entry-name`, `--profile-time`, `--wasm-info`,
  `--nyr-verify`, ownership/effect policy switches, and profiling controls.
  `--entry-name NAME` now accepts both separated and equals forms; the bare ELF
  entry-name regression covers both. `--prof=timing,stats,gprof,compile,peak-rss`
  is the consolidated profile list, with `--prof MODE` accepting separated
  values. `--opt=LIST` combines optimizer switches and `--check=LIST` selects
  type/effect/alias/ownership checks; both support `no-` items. `--warn=error`
  enables warnings-as-errors, and `--ownership-check=off|advisory|strict`
  provides named ownership diagnostic modes.
  Invalid `--dump-scope` values now use the concise exit-2 enum diagnostic.
  `--perf MODE` accepts a separated value;
  invalid modes use a
  one-line usage diagnostic and exit 2. Registry
  duplicate validation identifies the conflicting spelling. Gprof aliases
  and `-time` appear in the registry. `--timeout=SECONDS` and
  `-timeout=SECONDS` now work alongside
  separated timeout values; `-std=MODE` is also accepted. Output paths now
  reject a following option as a missing value rather than silently choosing a
  default, while `-` remains a valid path token.
- NYC now parses GNU switch case ranges and lowers `__builtin_clzl`,
  `__builtin_clz`, and `__builtin_ctzl`; the C frontend regression passes
  runtime boundary checks. GNU statement expressions with braced `if`
  statements now compile, link, and run in a reduced Kbuild-style fixture.
  Anonymous aggregate `sizeof` also honors no-argument `aligned`; the C
  frontend regression passes its compile-time 16-byte size assertion. The
  frontend also lowers empty-template `=r`/`"0"` tied pointer copies, with a
  compile-link-run check. Scalar `sizeof` expressions and void conditional arms
  are covered by frontend assertions/runtime tests. Direct Linux 7.3 `prepare`
  generates device-table offsets, passes modpost, and generates time constants.
  A static-function-pruning false reference from diagnostic strings was fixed;
  `typeof` member expressions, volatile member stores, GNU `void *` arithmetic,
  and runtime `__builtin_types_compatible_p` now have coverage. The latest
  direct Kbuild runs now pass `__builtin_object_size`, Linux's
  `__is_constexpr`/`sizeof(void)` pattern, and void GNU statement expressions,
  with frontend compile-link-run regressions for unknown object-size modes,
  external multidimensional row decay, and empty statement-expression bodies.
  Linux 6.9 `x86_64 defconfig` also generates; its direct `prepare` passes
  `__indirect_branch__`, asm-goto statement-expression labels, compatible
  repeated and multidimensional external aggregate-pointer arrays, and pointer
  arithmetic inside `typeof`, with runtime row-stride and type regressions.
  It now stops at `irqflags.h:28` on
  `native_save_fl`'s unlowered `=rm` output.
  The x86 global `rsp` `ASM_CALL_CONSTRAINT` `"+r"` form now compiles and
  passes a compile-link-run regression for a single direct call with no operand
  placeholders. NYC also lowers the `=rm` local-memory output form used by
  `native_save_fl`, runtime aggregate compound literals, `__builtin_choose_expr`
  branches, address-of one-byte aggregate array members, and
  `alternative_input`'s immediate-plus-`%P1` memory operands. Nested local
  aggregate initialization now copies parenthesized aggregate expressions
  into storage; the frontend regression passes. Both Linux 6.9 and 7.3
  `prepare` now advance past `thread_info.h`, `find.h`, and paravirt void
  statement expressions. The typedef-array pointer decay and level-zero
  `__builtin_return_address` now have compile-link-run coverage. Those
  intermediate runs had stopped in Linux 6.9 `asm-offsets.c` and Linux 7.3
  `asm-generic/qspinlock.h`; subsequent parser fixes resolve the Linux 7.3
  `READ_ONCE`/`typeof` path, preserve flexible-array identity in type
  compatibility checks, infer arithmetic `typeof` operands containing
  `sizeof`, support static scalar-array designators and aggregate member
  assignment, and lower constant-level-zero `__builtin_frame_address(0)` with
  a frame pointer; other levels fail compilation.
  Compile-link-run regressions cover flexible-array type checks, array
  designators, aggregate assignment, and frame-address correctness. A fresh
  clean-output direct Linux 7.3 and Linux 6.9 `prepare` runs now complete
  through their generated offsets and host `objtool`.
  SQLite 3.46.0's core amalgamation and shell both compile with NYC, link with
  Clang's compiler-rt, and pass a transaction/savepoint/rollback plus
  `integrity_check` CLI smoke test (`1|3`, `ok`).
- `core.os.file_read_lines` now propagates file-read errors without inferring the
  error arm as the function's string result. The ABI fixture checks trimming,
  blank-line preservation, and error propagation; the focused replay of all
  reported failing fixtures passed 14 selected cases across their routes.
- NYR lowering now preserves an already-f64 value when an integer-to-f64
  conversion is requested, matching the LLVM and native backend semantics.
  Contextual integer subtraction inside `float(...)` had emitted a redundant
  conversion that corrupted VM text widths. The fixture-protocol regression
  passes all three fixture variants; the bounded monitor replay matches five
  frames and state across native, JIT, and VM.
- UI smoke fixtures now execute eight demos independently on native, JIT, and
  VM with five scripted frames, resize/key/close events, state markers, and
  framebuffer hashes. The cold UI suite passes 24/24; synth VM completes in
  138.164 seconds with bounded mock text rendering and audio-source setup
  disabled when the mock backend has no audio device. This proves each route
  executes. At that point, the explicit parity harness matched monitor, input,
  pong, and synth; editor and terminal native/JIT frame hashes still differed,
  leaving full cross-route parity to a later run.
- All eight UI demos now match per-frame state, event, and framebuffer-hash
  markers across native, LLVM JIT, and VM (24 route runs). The engine replay
  loads the `ui-triangle` scene, selects and translates it, applies resize/key/
  close events, and verifies camera/matrix state and changed pixels.
- NYC now implements `-fomit-frame-pointer` with RSP-relative frames for leaf
  and call-containing functions. Stack arguments, temporary call-stack
  adjustments, spills, aggregate homes, and callee-saved state are covered by
  compile-link-run regression. Dynamic `alloca` and variadic-list functions
  conservatively retain RBP frames; the existing variadic translation-unit
  runtime fixture passes with the option enabled.
- NYC whole-unit x86-64 objects now support `-fPIC` and `-fPIE`, lowering
  symbol addresses through GOTPCREL and direct calls through PLT32. Runtime
  regressions compile and link both modes as shared objects and executables,
  covering external/local data addresses and external calls. The object check
  also requires `R_X86_64_64` relocations for file-scope pointer initializers.
  A current-source comparison compiled the same fixture with NYC, GCC, and
  Clang under both flags; all six objects linked and ran against the same
  oracle. Local-data relocation forms differ (NYC GOTPCREL, GCC/Clang
  section-relative), with matching runtime behavior.
- NYC resolves `typeof`/`__typeof__` of global register declarations and reads
  integer- or pointer-typed x86-64 `rsp` bindings as volatile register values.
  Compile-link-run regressions check both type spellings, the emitted
  `mov %rsp` instruction, runtime stack alignment, and a pointer-typed read.
  Taking the variable's address is rejected; register writes and bindings to
  other registers remain unsupported.
- NYC now accepts `-S` and `-fverbose-asm` and emits GNU x86-64 assembly for
  simple whole-unit C sources. The C frontend regression assembles, links, and
  runs the emitted output. It emits file-scope data/relocations and
  local/sectioned functions. Input-only `"i"` operands with constant
  expressions are substituted into inline-asm templates for `-S`; this
  supports Linux's `DEFINE` offset macro.
- NYC prunes unreferenced static functions and defers unsupported operand asm
  or `asm goto` inside them. Reachable unsupported asm remains a hard error;
  regressions cover both paths, including non-inline static functions. Direct
  Linux 7.3 Kbuild generates and assembles `scripts/mod/devicetable-offsets.s`,
  passes modpost, and generates `include/generated/timeconst.h` with the
  cached `bc` package on `PATH`. GNU switch case ranges now lower with runtime
  boundary coverage. Kbuild `prepare` advances through time-constant
  generation, then stops compiling `kernel/bounds.s` at
  `alternative.h:114` because the frontend treats `if` as an undeclared call
  target in that path.
- NYC accepts no-op GNU attributes on supported block-scope declarators,
  including an `__unused__` attribute on a secondary aggregate-pointer
  declarator. A compile-link-run frontend regression covers the case.
- NYC infers scalar and pointer types for initialized block-scope
  `__auto_type` declarations; a compile-link-run regression checks an
  aggregate-pointer initializer.
- LLVM-mode CMake now declares both generated core artifacts (`core.ny` and
  `core_symbols.h`) as command outputs, so Make can build targets that depend
  directly on the generated symbol header. The `ny-fmt-cloc` parity test also
  runs from the repository root, as required by formatter root discovery. The
  shared-core cache checker now compiles a root entry wrapper instead of
  treating its library module as an executable entry point.
- NYC now preserves floating-point element types when indexing pointer fields
  in aggregates. The frontend regression checks a `double *` field read, and
  a freshly rebuilt SQLite 3.46.0 fixture passes `fts5eb.test` (284/284),
  including the BM25 scoring cases that previously returned integer
  conversions of double bit patterns.
- NYC now emits 32-bit loads for `u32` elements indexed through aggregate
  pointer fields instead of reading 64 bits. A Linux guard-page regression
  covers the four-byte boundary; Valgrind on the rebuilt SQLite fixture
  reports zero errors.
- NYC now preserves the pointee type when forming an address through an
  aggregate pointer field, including 16-bit elements. The C frontend regression
  verifies that taking the address of an indexed `unsigned short *` field
  returns the correct typed element address. Relaxed 16-bit atomic builtins
  now use width-specific memory operations; a Linux guard-page regression
  covers atomic load/store at the end of a mapped page. Fresh SQLite
  `incrvacuum3.test` (53/53) and `fts3auto.test` (922/922) pass with no leaks.
- NYC now preserves aggregate metadata when dereferencing pointer-to-pointer
  values, so `(*cursor)++` advances by the aggregate size rather than 8 bytes.
  The 40-byte aggregate compile-link-run regression passes, and the rebuilt
  SQLite fixture passes `fts3auto.test` (922/922) with no memory leaks.
- NYC now classifies signed-character double pointers as arithmetic pointers.
  This fixes `char **` predecrement using a one-byte step instead of the
  pointer-sized step; whole-unit prefix and postfix compile-link-run
  regressions pass.
  A freshly compiled SQLite fixture passes `tableapi.test` (157/157, no leaks).
- NYC now accepts unreachable statements after a non-void `return`. This fixes
  configured production SQLite core compilation, where `UNUSED_PARAMETER`
  follows a return in an FTS5 initialization function. A whole-unit
  compile-link-run regression covers the return path.
- NYC now sign-extends integer results from indirect calls according to the
  declared return type. A runtime regression covers a function pointer
  returning `-1`; this fixes SQLite's Unix VFS unlink-error handling. The
  matched SQLite 3.46.0 `veryquick.test` run passes 388,779 tests with zero
  errors and no leaks. A fresh 8 MiB-stack run on the spill-colored fixture
  passes 389,203 tests with zero errors and no leaks.
- The spill-colored SQLite fixture also passes the compact 8 MiB-stack
  regression set: `json101.test` (278), `boundary2.test` (3022), and
  `boundary4.test` (55), with zero errors and no memory leaks.
- NYC evaluates runtime `__builtin_offsetof` expressions in constant-
  expression contexts. The translation-unit runtime regression checks the
  field offset returned from a function.
- NYC sign-extends signed 32-bit values loaded through pointer dereferences,
  including casts from unsigned storage. The translation-unit regression
  exercises the `INT32_MIN` alias/load boundary; a freshly rebuilt
  feature-complete SQLite fixture passes `boundary2.test` (3022/3022) and
  `boundary4.test` (55/55) at O2.
- NYC accepts Linux Kbuild's `-Os` flag and compiles through its conservative
  NYR `-O2` pipeline; the C frontend regression links and runs the output.
  NYC does not yet provide a distinct size-optimized pipeline.
- NYC now emits valid empty x86-64 relocatable objects, including for Kbuild's
  `-Werror` retpoline capability probe. This lets Linux Kbuild pass its
  retpoline-compiler check. Whole-unit x86-64 ELF `.s` inputs now assemble
  through the host GNU assembler; a compiled object linked to a C caller
  returns the expected value. NYC's `-fconserve-stack` selects its NYR `-O1`
  pipeline even when a higher `-O` level is also specified; compiling SQLite's
  feature-complete amalgamation with `-O2 -fconserve-stack` reduces
  `jsonTranslateTextToBlob`'s frame to 0x210 bytes. A generated
  `-O3 -fconserve-stack` C fixture also links and executes with the expected
  result. A fresh Linux 6.9 Kbuild now passes retpoline setup and the empty C
  object, then stops at `scripts/mod/devicetable-offsets.s`: the driver rejects
  Kbuild's `-fverbose-asm` argument on its C-to-assembly (`-S`) command. A
  fresh Linux 7.3 `defconfig` build reaches the same blocker.
- NYC's x86-64 object backend now reuses stack homes for spilled values with
  disjoint, register-allocation-safe live intervals. The direct object-encoder
  regression executes two high-pressure, non-overlapping value groups and
  checks their combined result. A fresh production SQLite shell built with
  NYC passes `json101.test` (278/278, no leaks) at the default 8 MiB stack,
  including `json101-11.0`;
  `jsonTranslateTextToBlob` reserves a 0x500-byte frame instead of 0x20f0.
- Refreshed the SQLite compiler comparison from current sources. GCC and NYC
  O0/O2 builds, links, SQL correctness cases, and compile-link-run features
  pass. NYR O2 median runtimes remain slower than GCC O2: 9.24 s vs 6.23 s for
  the speed workload and 5.67 s vs 1.61 s for the memory workload; runtime
  performance remains report-only.
- The `nytrix-embed-api` CTest now runs from the repository root, where the
  embedded compiler resolves its standard-library modules. Running from the
  build directory exhausted comptime fuel while compiling those modules; the
  full CTest suite passes 51/51 with the corrected working directory.
- Memory-SSA forwarding now treats self-edges as backedges and keeps block
  ranges correct when inserting join PHIs. Dedicated optimizer regressions
  pass, and a clean NYC `-O1` compile of SQLite's feature-complete amalgamation
  succeeds after the fix. Equal constants stored on separate branch edges now
  retain a join PHI instead of forwarding one branch-local definition into the
  merge block, preserving SSA dominance. Inserting a PHI at an empty block
  boundary now extends the target block and shifts the following block start,
  so branches to its label cannot bypass the PHI.
- Constant `__builtin_offsetof` evaluation now follows nested aggregate member
  designators. A compile regression covers the Linux Hyper-V trailing-overlap
  `_Static_assert` and its exact byte offset.
- NYC now reports pointer-width `sizeof` for subscripting pointer-to-pointer
  aggregate fields, rather than the pointed-to aggregate size. The regression
  uses a 56-byte record so the incorrect 56-byte result cannot equal the
  correct 8-byte pointer size.
- Aggregate parsing now accepts `__attribute__((packed))` between `struct` or
  `union` and its tag; a regression checks the resulting packed layout.
- Global integer constant expressions now evaluate `__builtin_choose_expr`;
  true and false branch-selection regressions pass in the C frontend suite.
  The C frontend also determines `sizeof` for dereferenced pointer conditional
  expressions, covering Linux's `__is_constexpr` probe. Unsigned 64-bit
  constant arithmetic now wraps correctly; regressions cover the Linux
  `GENMASK_ULL` assertions. Unsigned-int complement also preserves its width
  for cast operands.
- NYC lowers signed 64-bit and unsigned 64-bit `__builtin_add_overflow`,
  `__builtin_sub_overflow`, and `__builtin_mul_overflow`. The unsigned path
  computes the high product half without a runtime helper; boundary
  compile-link-run tests cover wrap and non-wrap cases.
- NYC's supported GNU inline-asm subset now accepts named operand labels and
  concatenates adjacent string literals in constraints and clobbers, matching
  C string-literal concatenation. The pointer-identity asm regression uses
  named operands and a split `"=" "r"` constraint; compile-link-run passes.
- NYC now lowers indirect calls through external `__x86_indirect_thunk_r11`
  retpoline symbols or an inline retpoline sequence. Both modes have
  compile-link-run coverage; the inline sequence's return path preserves the
  caller's stack return address. Linux 7.3's x86 Makefile uses the matching
  external and VDSO-inline modes when `CONFIG_CC_IS_NYC` is selected. The
  current kernel build passes the retpoline gate, generates offset headers and
  initramfs data, and compiles with PIC and `-Os` through the x86 boot startup
  source. Kbuild now passes the `GENMASK_ULL` assertions and unsigned
  `size_add`/`size_mul` builtins, then stops in `asm/preempt.h` on its `"m"`
  input constraint. `init/main.c` has another unsupported asm output
  constraint, and `init/do_mounts.c` reaches the `typeof`-array declaration in
  Linux's kfifo header. Full Kbuild remains open.
  NYC supports whole-unit `-fPIC`/`-fPIE` objects with GOTPCREL and PLT32
  relocations.
- Extended NYC C parsing/lowering for trailing commas in automatic array
  initializers, switch labels inside parenthesized expressions such as
  `_Generic`, implicit float conversions in local initializers and direct-call
  arguments, F32 aggregate-field assignments, typed nested pointer indexing
  and assignments, general `for` initializer/step expressions, and pointer
  compound assignment. Member access and assignment through aggregate pointer
  arrays and same-named function-local aggregate tags with distinct layouts
  now have regression coverage.
  `nyr-c-frontend` passes after these changes, including a compile-link-run
  regression that checks selected pointer identity and null-pointer behavior
  for a ternary-controlled `_Generic` expression.
- The configured preprocessed SQLite amalgamation now compiles successfully
  with NYC: 3,901 functions and 3,268 data symbols. The generic-selection
  type-probe path now isolates emitted NYIR, fixing duplicate labels caused
  by ternary control expressions. This is compile evidence only; upstream
  SQLite test-suite compatibility remains open. A fresh object also linked
  with the SQLite FTS3 test helper and Tcl dependency; API smokes passed
  windowed queries, recursive CTEs, JSON extraction, blob reads, rollback,
  integrity checks, and a disk database close/reopen.
- Fixed file-scope linkage detection so `static` declarations inside a
  preceding function no longer mark later globals local. A cross-translation-
  unit compile-link-run regression covers this boundary. The rebuilt SQLite
  object now exports the FTS3 test globals needed to link an isolated upstream
  `testfixture`. Its `alter.test` run still fails: the temp-table schema
  assertion differs, then `alter-1.3` aborts with `malloc(): invalid size
  (unsorted)`. Valgrind reproduces invalid reads and writes in `whereLoopXfer`
  while copying WhereLoop term storage; the first invalid read is one byte past
  a 1,072-byte allocation. The fresh `autoindex1.test` run also segfaults in
  `autoindex1-299`, with Valgrind reporting an invalid read in `sqlite3Update`.
  GDB stops at the faulting `pChanges->a[i]` load: `pChanges->nExpr` is 1,
  but the generated loop index in `%r14` is pointer-sized garbage. The fresh
  `ext/intck/intck1.test` run likewise segfaults at `intck1-1.3`.
  SQLite suite acceptance remains open.

### Verification — 2026-10-28

- The C frontend regression now models SQLite's `JsonCache` layout, including
  indexed pointer insertion from the `nUsed` field, count update, and indexed
  loads. The focused `nyr-c-frontend` compile-link-run test passes. This checks
  the aggregate layout/access pattern but does not resolve the upstream JSON
  cache invalid-free failure.
- Global C object emission now applies natural scalar or aggregate alignment,
  including function-local static aggregate arrays. A whole-unit compile-link-
  run regression verifies file-scope and function-scope static aggregate
  addresses, and `nyr-c-frontend` passes. A freshly compiled SQLite core also
  passes API smoke, 512 concurrent in-memory open/close operations, and 2,000
  repeated JSON extraction calls; the full SQLite test fixture still needs a
  correctly configured rebuild.

### Verification — 2026-10-27

- Fixed x86-64 native DWARF source mapping: machine encoding had freed the
  instruction-offset table before the object writer emitted line rows, leaving
  Nytrix breakpoints pending in GDB DAP. The offset map now survives through
  line-table generation. The DAP smoke passes launch, line-9 breakpoint stop,
  source-mapped stack trace, and step-over; its embedded NShape case also
  verifies a source-mapped breakpoint and stack frame.
- Fixed LLVM debug locations so generated locations use a function-local
  scope rather than the compile unit. This prevented an LLVM optimizer crash
  during `ny --emit-ir ... -g -emit-only`; the debug IR smoke verifies the
  emitted compile unit and LLVM's verifier accepts the module. Nytrix and
  embedded NShape hover/definition protocol smoke passes.
- The DAP bridge now parses JSON integer fields through the token delimiter, preserving request sequence numbers in initialize and disconnect responses. The protocol smoke verifies both request/response sequence pairs.
- `decompile()` now lifts its decoded rows once and builds the raw pseudocode
  and CFG view directly, instead of invoking the clean pseudocode renderer and
  full `structured()` analysis on every call. Full SSA/VSA/SMT and type
  analysis remain available through `function_model()`; clean rendering
  remains available through `ny_pseudocode()`. The generated C ELF fixture
  passes at `-O0` and `-O2`, including behavior checks, lifted LEA/arithmetic
  effects, signed branch-predicate rendering, typed compare effects,
  symbols/relocations, and unsupported-instruction diagnostics. On this host
  the fixture took 57.9 s with execution caches disabled; a cold one-function
  decompile smoke took 38.8 s total (38.1 s JIT compilation, 0.058 s
  execution). Cold compilation remains the dominant cost; these measurements
  describe only this fixture and sample.
- C aggregate-local lowering now handles mixed declarator lists with a pointer
  declarator before an aggregate object, fixing SQLite's uninitialized struct
  copy in `autoindex1-900`. A focused C compile-link-run regression and the
  rebuilt SQLite `autoindex1.test` suite pass (37/37). Host-checked member
  access regressions cover interleaved pointer/integer fields and a
  `JsonParse`-shaped record through its final pointer field. The kernel model
  regression also verifies that in-range RIP-relative data links at kernel
  addresses and the final linker rejects an out-of-range relocation.

### Verification — 2026-10-26

- Native lowering and the runtime ABI table now classify `rt_atoi` as raw
  `i64`, matching its runtime return and the `int` result of `str.atoi`; source
  calls were rendered through the tagged-value formatter and returned nil for
  `"32"`/`"42"` or 15 for `"31"`. The focused regression covers the implicit
  module context, signs, digit termination, and empty input across native
  O0/O2 and LLVM MCJIT.
- Implicit boolean returns now box the joined value when a trailing `case`
  statement supplies the function result. The focused regression covers true
  and false match arms plus loop-carried boolean state on native O0/O2 and
  LLVM MCJIT. The context-sensitive `core_bc` regression was traced to the
  missing `rt_atoi` raw-integer ABI entry; the same focused `atoi` and vterm
  probes now pass with source-stocked and compiled-core paths.
- Fixture verdicts: `etc/tests/code/probes/cli/terminal.nshape` and
  `etc/tests/code/intermediate/ny/vm-disasm-artifact-diff.nshape` pass the real
  single-fixture runner with gate-parity flags. vterm passes with caches off
  against both compiled-core (18.6s) and source-stocked (21.7s) routes. vterm's
  root cause was `str.atoi` miscompiling per source-stream context, collapsing
  every SGR parameter to a reset; `_tcsi_args` in
  `lib/os/ui/render/viewer/vterm.ny` now parses its arguments with a pure
  byte-loop number parser and all SGR flow routes through
  `_tcsihandle_fast -> _tsetattr_fast`, restoring background- and
  multi-argument SGR. The independent `str.atoi` raw-`i64` ABI fix and
  compiled-core regression are recorded above.
- Bench verdict integrity, harness side (`src/cmd/test/init.c`): default
  3-sample bench rows never computed the instability verdict (it was gated on
  `count >= 5`), so one fixture published contradictory samples
  (`call-chain` NYR 1.012ms / 230.6ms / 2645.3ms) as a stable 230.6ms row,
  tripping its 30x `native_c_budget` as a fake REGRESS (814x published). The
  verdict now runs at any sample count via `bench_samples_conflicting`, using
  scaled-MAD noise plus a range/median >= 10x poison signature that captures
  cases where two divergent samples agree with each other and outvote one
  fast sample; small counts judge on a 3x-widened limit so µs-floor kernels
  do not flag, and sub-1µs rows stay exempt. The sampling loop also spends its
  spare retry slots on a conflict trap that adds two samples so unpoisoned
  runs can outvote transient garble. Full-suite evidence:
  `build/bench-reports/bench-20261026-221627-2829576.json` — under session
  churn the new gate marked `list`/`dgemm`/`dict-oracle` NOISY exactly where
  contradictory samples existed; on quiet conditions all four rows passed.
- Benchmark noise classification now relies on scaled MAD rather than a
  range/median tripwire. A single scheduler pause remains visible in the
  reported min/max and p95 but no longer discards an otherwise stable median
  or its ratios. When samples remain unstable, the runner now spends its
  existing bounded retry budget two samples at a time and reevaluates before
  returning `NOISY`; persistent spread still yields `NOISY`.
- Bench std stability, make side: `NYTRIX_TEST_USE_STD_BC=auto` is the new
  bench default — `./make bench` pins a per-run `core.bc` snapshot into the
  run's trace dir and passes `--core-bc` to every sample, so per-sample std
  stocking can no longer compile against a stdlib that changes mid-suite. A
  missing bitcode keeps the previous source-stocking fallback; `=1` still
  builds `core_bc` first. Scoped-run evidence:
  `build/bench-reports/bench-20261026-222237-2892760.json` shows the pinned
  argv and a `call-chain` row at 0.5% dispersion / 0.1% noise. The fixture was
  never at fault: ~0.93-1.1ms native against 283µs C, checksum-stable across
  40+ runs, three core snapshots, cold stencil cache, and emit storms.


- NYC now seeds `__STDC__` and parses GNU `malloc` deallocator, `access`,
  `cold`, and `hot` attributes as no-op metadata; the C frontend regression
  covers the supported spellings.
- Linux ELF64 x86-64/AArch64 bare runtime coverage now includes integer, float,
  and `any` formatting, float bits/boxing and value tags, C-string conversion,
  concatenation, comparison, replacement and UTF-8 slicing, plus builder
  operations. Static typed-buffer clones preserve their headers; the x86-64
  writer retains those header bytes, and typed-list pop returns the removed
  element. `generate.py --check`, both selected-target payload harnesses
  (1,000,000 x86-64 and 100,000 AArch64/QEMU float differentials), and all four
  cold fixtures in `etc/tests/code/intermediate/ny/bare` pass. The backend IR
  acceptance CTest passes as well. Other OS/architecture pairs remain
  unsupported.
- `core.os.file_read` now performs its first raw read before testing for EOF; the cold `file-roundtrip-abi.nshape` regression passes all three routes, including with shared stdlib bitcode disabled. The related `semantic-effect-artifact` fixture remains open; current failure details are tracked in [TODO](TODO.md#current-check-failures).
- Standard-library `core.bc` generation now skips its standalone LLVM optimization pass; consumers optimize after linking. Three cold runs improved from a 170.333 s median to 134.776 s (20.9%) on the measured snapshot. The bitcode grew from 17.43 MB to 30.97 MB; cache identity and generated-flow consumer checks passed. Exact identities, commands, warm-hit timing, and input invalidation behavior are recorded in [measurements](learn/measurements.md#standard-library-core-bitcode-generation).

- Docs: the portal built from 523 modules and 21,238 symbols. All 50 learn/spec pages passed frontmatter, code-fence, callout, navigation, mini-TOC, link, and anchor audits. All 123 runnable Ny snippets compiled; 35 grammar or unimplemented forms are explicitly labeled. Light/dark code and callout surfaces were reviewed.
- Tests: an isolated `BUILD_DIR=/tmp/nytrix-phase-a-full-check ./make check --jobs 4` completed hygiene, format, parse, audit, backend scaling, and semantic-artifact checks. Its cold fixture run passed all 2,449/2,449 fixtures across Runtime (641), Native (923), Interop (73), Repl (19), Probe (131), Error (591), and Benchmark (71); the shapes audit found zero failing fixtures. The explicit 71-fixture benchmark parity/performance run also passed every gate. The overall check still reports `INVALIDATED: sources changed during the gate`, so this is full passing test evidence but not a clean source-stability verdict. All fixtures in the earlier failure report, including the comptime round-trip, bare assembly, AVR constants, and ELF32 cases, passed. VM listing/artifact parity now passes for source and serialized NYIR at `-O0` and `-O3`, with a tested first-divergent-opcode diagnostic.
- Benchmarks: the full cold parity/performance run passed all 71 fixtures with three measured samples plus a warm-up and unchanged thresholds. Fixture measurements run serially by default; `--jobs N` and `NYTRIX_BENCH_JOBS=N` opt into throughput.
- Runtime-operation symbol classification now lives in `src/code/backend/runtime_ops.c`, shared by the Ny compiler and standalone Nyr backend. The runtime alias and argument-ABI tables are lexically ordered; `runtime_call_contracts` and the cold bare `asm-artifacts.nshape` replay pass.
- Bare C-string equality now has freestanding x86-64 emission as well as the existing AArch64 path. Equality covers identical pointers, null pointers, and bytewise comparison through the terminating NUL; both architecture paths classify the helper through the shared runtime-operation identity.
- Bare C-string comparison now has x86-64 and AArch64 runtime adapters, with signed byte-difference results for lexical order and null-pointer cases. The bare assembly-artifact fixture exercises both paths.
- Bare x86-64 printing no longer routes boolean comparison results through the raw-integer printer. The comparison fixture now checks ordering predicates and null behavior on both native targets.
- Bare runtime-operation and syscall-adapter preflight checks pass; operation classification is shared, while each architecture still emits its own instruction sequence.
- The bounded internal x86-64/AArch64 decoder inverse now covers the audited in-tree encoder forms, including scalar/vector arithmetic and moves, packed operations, VEX, floating-point conversions, AArch64 SIMD shifts and scaled floating-point memory access, bare syscall stubs, relocations, and ELF labels. The x86 encoder oracle, `native_vec4_f64`, and architecture decoder regressions pass; unsupported encodings remain explicit without Capstone. Generated ELF original/decompiled behavior checks pass for `x+1`, `x-5`, and `x ^ 7` at `-O0`/`-O2`; relocation types, resolved helper targets, aliases/local labels, structured LEA effective addresses, and function-named unsupported diagnostics are covered. With `NYTRIX_NO_CAPSTONE=1`, all 162 fixtures in `etc/tests/code/intermediate/ny` and `etc/tests/code/probes/tools` pass. EVEX, broad vendor-extension parity, and arbitrary real-binary behavior remain outside the bounded in-tree encoder inverse.
- Disassembly target parsing now slices decimal and hexadecimal operands through the standard string API instead of an undefined helper; this keeps the source-built decompiler path executable.

- Bare x86-64 and AArch64 assembly-artifact coverage now checks source-level byte reads with positive, negative, and out-of-range indices; direct x86-64 and QEMU AArch64 runs produce matching values. Freestanding float formatting and broader platform support remain open.
- Raw Linux syscall forwarding now preserves the dynamic syscall-number value expected by the runtime ABI. The syscall-filter and file-read fixtures pass all routes cold after rebuilding `core.bc`.
- Fixture children now default `NYTRIX_TEST_MODE=1`, so direct-entry `__main()` checks exercise the same compile-time and runtime contract without changing the test scheduler. `lang/modules/use/guard.nshape` passes all four cold routes.
- NYC now lowers GNU scalar statement expressions and treats `__builtin_expect` as an identity builtin while evaluating both operands; local-static symbol relocations and function-address expressions have linked runtime coverage. The C frontend suite passes. Linux 7.3 Kbuild now passes the prior `init/main.c` statement-expression, builtin, and static-address cases, then stops at `arch/x86/include/asm/bug.h:160` on the `=r` output constraint for a local-label `lea`; general inline assembly remains unsupported.
- NYC now lowers the named Linux bug-entry asm shape: `[addr] "=r"` outputs, `%[addr]` register placeholders, `%c[name]` string/numeric immediates, numbered local labels, RIP-relative bug/file fields, and size-based `.org`. A compile-link-run regression checks the returned table pointer, relative pointers, and metadata.
- NYC now lowers a named `"+m"` read-write memory operand with one named register input and optional `memory`/`cc` clobbers; a compile-link-run regression verifies the updated memory value.
- NYC's `typeof` inference now retains aggregate array element types, and GNU `__builtin_offsetof` lowers variable array indices. Function-pointer/null comparisons, parenthesized for-loop declarators, void statement expressions ending in control statements, zero-size aggregate symbols, trailing inline-asm clobber commas, numbered `%1` memory inputs in immediate-plus-memory asm, Linux `"+m"` plus `=@cce` condition-code output, and local GNU `cleanup` attributes now have compile-link-run coverage. Cleanup functions run in reverse declaration order on compound-scope exit, return, break, and continue; the x86-64 backend emits local functions with local linkage. Linux 6.9 x86-64 `prepare` completes with NYC at the kernel's conservative `-O1` mode.
- NYC's x86-64 global-register contract now matches the Linux use of `current_stack_pointer`: volatile integer/pointer reads and the direct-call `ASM_CALL_CONSTRAINT` `"+r"` form work; writes, address-taking, and non-RSP bindings are explicitly rejected. Runtime and negative regressions cover the boundary.
- NYC's Linux Kbuild path now accepts `memory`/`cc` clobber lists, block-scope `_Static_assert`, broader arithmetic `typeof`, qualified `_Generic` pointer associations, direct `&*pointer`, and chained subscripting through global pointer variables. Global scalar-array arguments decay to typed pointers; typedef-defined aggregate array rows retain stride and decay to aggregate pointers. Parenthesized nested aggregate-member address expressions now have compile-link-run coverage, and switch cases accept enum constants and constant-expression GNU range bounds. Empty GNU aggregate `sizeof(*pointer)` accepts complete zero-size types, typedef-defined pointer-array rows retain their inner extent and pointer element type, typedef-defined aggregate-array fields decay to element pointers, scalar `const T[]` compound literals lower to function-local storage, undeclared `__builtin_memcpy`/`__builtin_memmove` calls lower to the standard memory-copy symbols, undeclared `__builtin_strlen` calls lower to `strlen`, local `_Bool` initialization and function returns normalize pointers to zero/one values, and `typeof(__builtin_choose_expr(...))` infers the selected branch type. Compile-link-run regressions cover these behaviors. Linux 7.3 `prepare` now completes `asm-offsets.c` and passes the `cpumask_empty(policy->cpus)` typedef-array decay in `rq-offsets.c`, then stops at `include/linux/cpufreq.h:264` in `cpufreq_get_pressure` with `reference 'const' must name a function parameter or declared local` while lowering the `READ_ONCE` expansion. Linux 6.9 `prepare` passes.
- NYC now parses function-pointer-return declarations, supports nested signed `char **` indexing and stores, `sizeof(pointer[index])`, local character-array string initializers, and block-scope enum declarations with branch scope restoration. It models additional hosted declarations used by SQLite's shell. Addressing a file-scope signed-char array element now keeps the element-pointer stride rather than scaling by pointer size; a frontend runtime regression covers indexing and dereferencing that address. File-scope `static` objects now receive local ELF binding, with a multi-translation-unit link/runtime regression for colliding private functions, data, and literals. NYC compiles and links SQLite 3.46.0's shell and core; the shell reports version `3.46.0` and successfully creates a table, inserts a row, and selects `42`. The upstream SQLite test suite remains unverified. NYC rejects `-fomit-frame-pointer` because non-leaf omission is not implemented.
- NYC now accepts unnamed integer bit-fields used for ABI padding and preserves following field offsets; the frontend regression suite compares those offsets with host C. Regressions cover continued macro comments, incomplete and later-completed enum tags, repeated external declarations, pre-name section/alignment attributes, enum ternaries, constant `offsetof`/`__alignof__`, unsigned-int complement width, `__builtin_clzll`/`__ilog2_u64` array bounds, and aggregate pointer arrays. Compatible `extern` aggregate declarations now complete into definitions, and file-scope designated initializers resolve promoted anonymous struct/union members; the compound-initializer runtime fixture covers both. File-scope assembly supports a bounded subset of section, alignment, and PC-relative `.long` directives; a linked runtime regression exercises the emitted relocation. Linux 7.3 Kbuild passes preparation and `checksyscalls.sh`, compiles `scripts/mod/empty.o`, and reaches `init/main.o`; the former `NR_VMEMMAP_TAILS` constant-bound and tracepoint aggregate-initializer failures now pass. At this checkpoint, Kbuild stopped at `.pushsection .static_call.text, "ax"` in file-scope asm generated by `include/trace/events/initcall.h:10`; a later run passed that and the earlier `rip_rel_ptr`, `WARN`, `__builtin_expect`, and local static-address cases, then stopped at `arch/x86/include/asm/bug.h:160` on an operand-bearing asm output constraint. GCC compatibility macros identify version 12.0.0 for guarded headers, not full GCC compatibility. General function-scope inline assembly, global-register semantics, `-fPIC`, and complete kernel compatibility remain unverified.
- NYC now lowers relational object-pointer comparisons as unsigned addresses; this fixes SQLite's `sqlite3Utf8CharLen(..., -1)` sentinel loop, with a compile-link-run regression. Block-scope scalar `extern` references and compound assignments to external globals also have translation-unit runtime coverage. Address-taken `float`/`double` locals now reload and store through their stack homes after address escape; a runtime regression covers a callee-written `double` passed alongside a pointer in the SysV floating-point argument register. The C frontend CTest passes; enum checks pass 64/64, include checks pass 60/60, assert checks pass 22/22, POSIX-header checks pass at O0/O2, preprocessing passes 86/86, and the sanitizer-backed stress suite passes 39/39. `build-cfe-asan.py` now snapshots the current split frontend sources instead of removed `nyc_frontend.c`/`c_lex.c` inputs. Full configured CTest passes 50/51; its sole failure remains `core-bc-cache-selector`, whose changed-core source hash checker exits 0 with a mismatch. NYC forwards Kbuild's exact `-Wa,--version` probe to the system assembler: Linux 7.3 `defconfig` passes, while `vmlinux` remains blocked by the enabled retpoline compiler gate. The current NYC SQLite core compiles and links into a non-PIE upstream fixture; `ext/intck/intck1.test` passes 70/70. GCC passes `alter.test` 120/120, while NYC segfaults at `alter-3.1.1` in `sqlite3VdbeExec` at both O0 and O2. GDB shows a heap-end store; measured `Mem` and `Vdbe` layouts match GCC, so the pointer calculation still needs diagnosis. Full SQLite acceptance remains open.
- Current SQLite build/check/features and three-sample measurement runs pass all benchmark correctness checks at GCC/NYR O0/O2. Median speed-test time is 10.208 s on NYR O2 versus 4.337 s on GCC O2 (2.35×); memory-test time is 6.870 s versus 1.462 s (4.70×). The single-sample register-switch feature check passes, but NYR O2 takes 0.468 s versus 0.0260 s on GCC O2 (18.0×). These workloads are slower with NYR; measurements are not acceptable-performance certification because no threshold is defined.
- The module-cache equivalence probe now clears inherited test mode before setting each child compiler's explicit environment, keeping its unset, empty, and enabled cache-key cases distinct. The focused cold probe passes.
- `filter_map` now tests dynamic results with `is_nil` before retaining them; the shared collection and iterator assertions pass cold on all fixture routes.
- The documentation site now uses theme tokens for code, callouts, cards, and navigation, with a light palette selected by `prefers-color-scheme`; browser previews confirmed readable code and callout surfaces in both palettes. Fixed the quick-reference link to the syntax bindings section.
- Language-reference pages now mark grammar fragments and surrounding-program placeholders instead of presenting them as standalone code. Expression-based `#if(arch())` is identified as unsupported; platform guards remain the supported selection form.
- Type inference now treats caught panic payloads as dynamic Ny values inside a scoped handler environment; `abi-conformance.nshape` passes all five cold routes, including the caught division and length errors.
- Native comparisons now preserve the raw integer ABI of literal-initialized top-level globals when compared with proven scalar list reads; the cold append-built list ABI fixture passes all four backend variants.
- Native match-expression conditions now normalize boolean-valued arm joins before branching. Result error-pattern bindings preserve dynamic string and integer payloads; generated reflected enum matching passes all four cold routes, and the Ny desugar budget probe reports its intended diagnostic.
- AVR portable emission now lowers boxed `rt_flt_nan`/`rt_flt_inf` results directly to IEEE-754 constants when every use is an `rt_any_to_f64` conversion. The AVR float-runtime fixture splits comparison/conversion groups to fit the ATmega328P flash and passes its serial QEMU route.
- AST signature and contract queries now narrow node-kind values to strings before classifying them, and typed statement/declaration constructors reject invalid quote metadata. The AST code-query fixture passes both variants.
- Remote socket retries now recognize tube dictionaries explicitly, and socket reads detect received bytes by length rather than string comparison with the empty literal. Cold loopback socket and tube ping-pong probes pass.
- The `--vm-disasm[=<name-filter>]` option prints sorted NYR instruction listings with available source and ABI annotations, including the bare entry function. Focused format fixtures pass; this listing is not native machine-code disassembly or a runtime trace.
- VM listing fixtures now pin single-instruction formatting at `-O0` and `-O3` and compare the executed source listing byte-for-byte with the same program loaded from an NYIR bundle.
- VM listing parity now compares direct source execution with execution from serialized NYIR at `-O0` and `-O3`. A failing comparison reports the first differing opcode line; the artifact-diff fixture passes cold.
- MT19937 SMT recovery now narrows constraint-kind values from dynamic dictionaries to strings before matching `eq` and `hi` constraints; its cold solver fixture passes.
- The bounded-JSON nesting probe now checks the decoder's boolean result directly, so its at-limit and over-limit cases assert the actual contract.
- Standalone benchmarks now run fixture measurements serially by default; `--jobs` and `NYTRIX_BENCH_JOBS` remain explicit throughput overrides. The SOR Ny workload uses the same alternating source/destination-buffer loop as its C oracle, preserving its checksum and four-route fixture coverage.
- Documentation Phase A now has verified navigation, frontmatter, callouts, fenced-language labels, spec mini-TOCs, and local links across all 50 learn/spec pages. All 123 runnable Ny blocks passed parse/type/trait/flow/ABI checks; 35 grammar or unimplemented forms are explicitly labeled. The portal parsed 523 modules and 21,238 symbols, built successfully, and its light/dark code and callout surfaces were reviewed.
- Clean `./make check` passed 2,447/2,447 fixtures; its integrated benchmark gates passed. Standalone `./make bench` passed all 71 fixtures with the new serial-by-default measurement mode and unchanged thresholds.
- Native x86 list-pop lowering recomputes the header address at its update store, avoiding an uninitialized branch-local stack home; the cold `result-list-mutate-x86-64` native oracle passes.
- The `parallel_map` guides now use a runnable example; `parallel-map-jit-parity.nshape` passes all four JIT, native, and threaded routes.
- Native `str(any)` formatting now checks the emitted tagged-value class before
  applying sequence-specific formatting. This preserves strings returned by
  dynamic lookups; the cold `semantic-effect-artifact.nshape` consumer passes
  after rebuilding `core.bc`.
- `core.os.io.recv_all` now treats invalid-stream integer sentinels as empty
  input instead of reading `.len` from an integer. Bare x86-64 and AArch64
  fixtures also cover owned C-string conversion, `str.chr`, and `str.repeat`.
- Native code: register allocation, signed arithmetic, target ABIs, and object
  linking.
- Runtime implementation sources and runtime API headers now live under
  `src/code/backend/runtime/`; language builtin effect contracts remain under
  `src/code/lang/`. CMake, compiler build/cache dependencies, embedded-runtime
  tests, and documented source paths use the new ownership split.
- Native AOT: x86-64 bare integer `eprint`, byte `rt_memset`, and page-rounded
  mmap-backed `rt_malloc_i64`/`rt_free` work without a host assembler,
  including multi-page writes, successful `free`, null `free`, and negative-size
  allocation regressions. Runtime C-string concatenation and explicit
  `rt_cstr_to_str` conversion now emit page-backed, tagged-length strings in
  bare AArch64 ELF; x86-64 also supports concatenation. AArch64 directly
  compares C-string contents, including equal and unequal null operands, and
  computes C-string lengths. Bare AArch64 boolean formatting covers computed
  true/false values, not only constants. The
  cold fixture checks empty/nonempty operands, nested concatenation, `.len`,
  and exact 4 KiB output on both targets. Freestanding binary64/`any`
  formatting, replacement, UTF-8 slicing, and builders now pass on both
  selected targets. x86-64 ELF object emission now fails closed through the
  in-process writer rather than invoking an assembler fallback; all 60 tests in
  `etc/tests/code/formats/elf64` pass cold under `CC=/nonexistent`.
- Native x86-64 optimization retains range-proven 32-bit arithmetic, safe COPY
  home coalescing, and non-overlapping stack-home reuse. The compiler defaults
  to `-O2` with size optimization enabled; a bare arithmetic/output probe
  measured 4,473 bytes at `-O0` and 4,423 bytes at `-O2`, with both printing
  `81`. Focused `narrow` and machine-home semantic tests pass.
- Native x86-64 no longer writes a canonical stack home for a colored value
  whose proven final use is a same-block return; the return consumes the value
  directly from its assigned register. The encoder oracle covers ordinary and
  signed-wrap boundary inputs; the focused function shrank from 48 to 41 bytes.
- NYC C frontend: whole-unit C objects now accept fixed-size zero-initialized
  file-scope arrays of aggregate pointers, and modeled hosted headers expose
  the common declarations needed to compile Linux 7.3 `fixdep.c`. Its NYR
  object now runs with output matching GCC. The standalone driver supports
  forced includes and Kbuild dependency files, with regression coverage for
  both. Anonymous aggregate members now have promoted access and a layout
  regression. Forced includes also work for standard-input sources. The
  preprocessor seeds target-width and x86-64 platform macros and supports
  variadic comma elision, conditional ternary/unsigned preprocessor expressions,
  File-scope `_Static_assert`, `_Bool`, `__signed__`, aligned aggregate fields
  (including attributes between a type and field name), `warn_unused_result`,
  `__extension__`, parenthesized function typedefs, fixed array parameters,
  allocation-related attributes, external aggregate-pointer globals, and
  multiple external integer-array declarators are supported. Preprocessing
  emits requested dependency files, including stdin input without a dependency
  target crash; nested token-paste macro dispatch and comments within continued
  macro replacements have regression coverage.
  Array bounds support shifts, comparisons, conditional expressions, `sizeof`,
  and `__alignof__`. A fresh local-adapter Linux 7.3 Kbuild run passes
  preparation and `checksyscalls.sh`, compiles `scripts/mod/empty.o`, and
  reaches NYC compilation of `init/main.o`; it fails at
  `include/linux/seqlock.h:174` on the type near `seq = smp_load_acquire(...)`
  while expanding `SEQCOUNT_LOCKNAME`. Direct compilation of the preprocessed
  `init/main.c` reaches the same expansion and fails at
  `build/cache/kernel-7.3/build/init/main-fresh.i:21841` near `seq=({ ... })`.
  The no-SSE scalar-integer path is tested to emit
  no SIMD registers and rejects floating-point/SIMD code. NYC seeds
  `__GNUC__` 12.0.0 for GCC-guarded Linux declarations. `-mcmodel=kernel` and
  `-mcmodel=small` are accepted using RIP-relative addressing, but full kernel
  range constraints are unverified. General C inline assembly and global
  register-variable semantics remain unsupported; `-fomit-frame-pointer` and
  `-falign-functions=16` are accepted no-ops. Full kernel-build compatibility
  is not established. The SQLite library/API comparison against GCC is
  verified, but shell compilation and the upstream test suite are not.
- Runtime fixtures: `lang/execution/defer.nshape` passes all five cold routes
  on 2026-10-25; the stale MCJIT/ORC warning was removed from the control-flow
  guide.
- Runtime fixture: `lang/language/prolog.nshape` passes its cold routes on
  2026-10-25.
- Runtime fixture: the `lang/types/inference/type.nshape` alias/group prefix
  passes explicit numeric alias and seeded group checks. Correct native assert
  truthiness now exposes a later `lowercase type-first poly_ring lhs` failure;
  the full fixture remains open.
- Runtime fixture: `lang/comptime/reflection/reflect-metadata.nshape` passes all
  four cold variants on 2026-10-25.
- Documentation search ranking now uses its existing ASCII-folded byte matcher
  for exact full-name, name, and leaf comparisons, plus bytewise lexical
  comparisons for deterministic kind and full-name tie ordering. The `doc-rank`
  fixture passes all four cold variants.
- Documentation metadata collection uses bytewise route ordering, and its
  fixture uses a monotonic suffix to avoid stale temporary-directory collisions;
  all four cold routes pass.
- The generated layout field-store regression now passes all four cold routes;
  the earlier boolean writer mismatch did not reproduce on the current build.
- SVG rasterization now keeps tag names typed and excludes `line`/`polyline`
  fills through explicit byte matching. The pixel fixture verifies opaque
  rectangles, icon coverage, and open-path behavior on all three cold routes.
- Typed AST code envelopes now serialize the sequence marker as integer `0/1`,
  matching the validator's required representation. The `ast-code-queries`
  fixture advances past declaration-category validation; a separate qualified
  `codeof` panic remains open.
- Native assert lowering now converts tagged and dynamic conditions to raw
  predicates before calling the C runtime, so tagged `false` fails as intended.
  A new subprocess regression passes all four cold routes. The now-visible
  TrueType face-load failure was later traced to FFI library-handle checks.
- The font text-codepoint probe now selects the mock renderer to avoid requiring
  a Vulkan device. Fixing pointer/integer zero checks in `core.os.ffi` lets
  `dlopen_checked` retain a valid FreeType handle and resolve symbol pointers;
  the codepoint and TrueType metrics probes pass all three cold routes. A new
  checked-open fixture also passes native O0, MCJIT, ORC, and native-only routes.
- The cold `ffi-pointer-handles` fixture reaches a separate odd-scalar conversion
  mismatch (`__ffi_untag_ptr(83)` is observed as `41`); it remains open in TODO.
- The expanded list-argument ABI fixture no longer reproduces its stale
  receiver/index mismatch and passes all eight cold routes with source caching
  disabled.
- The captured-callback divergent effect-row fixture no longer reproduces its
  earlier timeout; all five cold cases pass across native, MCJIT, ORC, and
  native-only routes.
- The generated-runtime ownership shadow differential no longer reproduces its
  prior high-RSS timeout; all three cold routes pass in 18.2 seconds at 392 MiB
  peak fixture RSS.
- The semantic-effect artifact probe no longer reproduces its high-RSS timeout:
  it emits the artifact in 8.5 seconds, then the native consumer reports an
  unsupported typed semantic fact shape. That consumer gap remains open in TODO.
- XML parsing now initializes the root node by checking its dict representation
  instead of comparing a dynamic `any` root with integer zero; the cold
  `xml-attr-probe` passes.
- Process tubes now classify `kind`, `offline`, and `closed` through typed
  checks, fixing process read/send routing; `interact-process` passes its cold
  fixture. The related argv-spacing probe now advances to its spaced-argument
  assertion.
- WAV resource dispatch now normalizes the extension into a typed string so
  `.wav` selects libsndfile; the bundled cold `sound-wav-resource` fixture
  passes.
- The cold virtual-terminal cell-output probe now passes all declared routes.
- Request preparation now checks optional dynamic sentinels by type and keeps
  integer query values during URL encoding; the strengthened GF2/import and
  request-query fixture passes all bundled cold routes.
- Process argv normalization now compares the first argument as a typed string;
  the bundled cold process-recvuntil fixture passes its spaced-argument case.
- UI event validation now checks the dynamic event tag as a typed string, and
  per-window event drains use the queue's integer key; all bundled cold
  ui-native-event-queue variants pass.
- The X11 client-message close probe also passes its three cold variants with
  the corrected event tag check.
- Native calls: preserve floating-point argument classes when lowering calls
  through runtime ABI aliases.
- SysV AMD64 native object calls now set `%al` to the number of vector argument
  registers before external calls, including variadic calls. The NYC regression
  passes a `double` through a variadic C function and checks it with `va_arg`.
- Native assembly: Linux x86-64 `__syscall_raw` calls emit the `syscall`
  instruction directly instead of calling the C runtime helper.
- Native backend tests: deterministic x86-64 encoder checks execute direct
  object code and clang-assembled machine output against expected float
  comparisons, immediate boundaries, and signed power-of-two division.
- Bare AArch64: `rt_syscall_raw` lowers to Linux `svc #0` with AAPCS argument
  registers translated to the Linux syscall ABI; unsupported OS targets remain
  fail-closed.
- Shared runtime-operation classification now identifies Linux syscalls once;
  x86-64 and AArch64 platform lowering consume that classification instead of
  duplicating the runtime-helper name check.
- The shared classifier also covers bare print/conversion, clock, memory, and
  C-string helpers; AArch64 bare lowering and fail-closed dependency checks use
  those operation categories rather than repeating helper-name comparisons.
- Native assembly: pooled strings use escaped `.ascii` data plus a terminator,
  preserving embedded NUL and arbitrary byte values without one `.byte`
  directive per character.
- Native booleans: negate both raw predicates and canonical tagged boolean
  values correctly.
- Native environment lookup: `core.os.env` reads inherited variables through
  the native-safe runtime getter.
- Language tools: bounded refinements, callback effects, hygienic templates,
  and typed reflection.
- Applications: UI rendering, audio playback, terminal behavior, and runtime
  representation fixes.
- Documentation: reordered learning paths, clearer language references, and
  theme-aware semantic callouts.
- Documentation: public contributor guides, source-highlighted examples, and
  explicit implementation limits.
- Documentation rendering: normalize indented line continuations in inline
  text so list items do not retain source indentation as visible spaces.

### Documentation onboarding — 2026-10-24

- The native guide now includes a runnable minimal C `sqrt` call. The testing
  guide points readers who only need to run a program to the start guide, and
  the contributor guide is included in manual navigation metadata.
- The cookbook now labels its `parallel_map` snippet as unverified and points
  to the concurrency guide's runtime warning.

### Runtime fixture recovery — 2026-10-24

- Cold native runs pass the path-normalization and JSON-state fixtures across
  all declared routes; the extensible-syntax and indexing fixtures pass their
  default route. Syntax form-head checks now distinguish the integer zero
  sentinel from string heads and read string heads through a typed list
  accessor. The proof-policy fixture passes all four cold routes. Other Phase A
  failures remain open in `docs/TODO.md`.
- AArch64 bare object emission now reserves integer/boolean formatting scratch
  before the value and local slots, keeping its frame offset within the
  immediate range even in large functions; the scalar-classes AArch64
  and runtime-numeric-results AArch64 object-link-run fixtures pass. Native
  global bool stores now normalize raw predicates and tagged values to the
  canonical tagged cell representation; the ABI conformance fixture and the
  general scalar-classes fixture pass all five routes cold.
- Additional cold debug-failures checks pass for the ABI disassembly fixture,
  bare entry-name and x86 typed-list-length fixtures, raw dynamic indexing,
  logical loop branching, nine AST metadata/serialization/rewrite fixtures,
  generated SOA representation and all three WASM32 runtime variants. The
  SOR native-only O0 route now passes after the x64 backend keeps
  stack-home floating-point memory LEAs materialized and reloads the canonical
  address instead of rematerializing it through the scratch-index path. The
  one-sample cold correctness benchmark reports NYR 3.70ms, LLVM 716µs, and C
  1.03ms; ratios remain advisory for this single sample. The
  x86-64 result-list oracle still fails after the rebuild with harness status
  139; a `-vvvvvv` GDB repro localizes it to a null stack-home store in the
  generated append/pop path, and it remains open. The remaining AST query failure is an out-of-range
  `index_read`; generated store-fields fails its unknown-field assertion. The
  SOR benchmark reports correct output, though its native-only O0 route returns
  an incorrect checksum (863729793 instead of 1725461952). A one-iteration
  reduction and standalone O0 typed-buffer fill/boundary probes narrow the issue
  to the nested stencil loop path.
- AVR cold checks also pass for divmod, scalar byte/global memory, scalar
  calls, the complete stack-argument fixture (including both rejection cases
  and QEMU execution), and the panic fixture through emission, assembly, link,
  QEMU trap execution, rejection, and cleanup. The panic fixture exposed a
  raw-bool runtime alias classified as tagged; call lowering now preserves the
  runtime ABI result class through a source-level bool declaration. AVR float
  comparison return branches now use absolute `jmp` instructions to the shared
  epilogue, clearing the former `R_AVR_13_PCREL` link overflow; the exact QEMU
  fixture now reaches execution but emits neither expected UART byte before its
  timeout, so it remains open.
  The BPF assembly fixtures now import `env` explicitly from `core.os`; both
  portable-rotation and signed-opcode assembly checks pass cold. The ny-desugar
  budget diagnostic and parity fixtures also pass (eight parity variants).
  File-roundtrip ABI, all fixture-protocol cases, and the term quit-key probe
  pass cold. Typing quit-key arguments as `int` avoids false results from
  generic `any` comparisons for key codes. The GF(2) identity solve now
  survives dynamic nested-list reads through explicit integer conversions;
  its combined probe advances to the request query separator assertion, which
  remains open. The documentation
  markdown probe passes all four variants, while metadata collection and
  ranking still fail their route-sorting and exact-name assertions.
- The sound-source ABI probe passes all four cold variants. The WAV-resource,
  XML-root-name, and TrueType face-metrics assertions still fail and remain in
  the TODO.

### Bare native output

- Simple Linux x86-64 bare `print("...")` output now lowers string and newline
  helpers to inline `write` syscalls in the ELF file. This path links and
  runs with `CC=/nonexistent`, without libc, an interpreter, or a host
  assembler. Other x86-64 runtime-backed bare programs still use the assembled
  runtime shim.
- Bare Linux x86-64 `__syscall_raw` calls encode directly into the ELF, so
  syscall-only native programs need no `rt_syscall_raw` import or libc. A
  direct `exit` syscall probe linked sectionlessly and exited with its requested
  status.
- Bare native output now applies `--entry-name=<symbol>` to both the emitted
  entry function and the ELF linker entry; the assembler fallback validates
  the symbol as a C-compatible identifier before inserting it into its startup
  stub.
- Bare x86-64 raw integer `print` now formats signed 64-bit values in a stack
  buffer and writes directly to stdout. Zero, positive/negative values, and
  `INT64_MIN` run through the internal ELF linker without a host assembler.
- Bare Linux x86-64 `rt_monotonic_ns_raw` now uses the `clock_gettime` syscall
  and returns plain nanoseconds. Dynamic raw/tagged booleans convert to
  embedded `true`/`false` C strings, and bare C-string output now branches to
  the `write` syscall after finding the terminator instead of skipping output.
- Bare x86-64 now handles static literal `.len` through direct C-string length
  scanning and keeps the C-string bridge as a borrowed pointer; no managed
  string allocation or libc call is introduced for this supported case.
- Bare x86-64 now lowers raw `memcpy` and `memset` byte loops directly in the
  in-process ELF encoder. Raw `memcmp` compares unsigned bytes and sign-extends
  the first-byte difference. Stack-backed copy/fill/compare probes run without
  libc or a host assembler.
- The x86-64 bare typed-buffer shim now keeps logical length separate from its
  minimum allocation capacity. Bare typed-list length reads return the
  requested element count; the executable remains static and has no dynamic
  section.
- Linux AArch64 bare objects now inline raw byte-copy and byte-compare helpers,
  preserving the destination return from `memcpy` and zero-length `memcmp`
  behavior without libc. Mismatch results are sign-extended to the 64-bit
  runtime ABI; bare fixtures now check equal, empty, and both mismatch signs.
- AArch64 bare `memset` raw byte fills are exercised alongside copy and compare
  in the assembler-free memory fixture.
- AArch64 bare `eprint` now formats string, integer, and boolean arguments and
  writes them to stderr through direct C-string scanning and Linux syscalls.
  The regression compiles with `CC=/nonexistent` and checks stderr under QEMU.
- AArch64 bare function output can borrow a static C-string literal through the
  direct writer without allocating or requiring a managed string runtime.
  AArch64 bare AOT now clones compiler-emitted numeric-list literals into
  anonymous mappings and supports length and positive/negative index reads
  without libc. Native-list length and raw `pop` now lower to shared NYIR
  memory/control-flow operations instead of target-specific helper dispatch,
  including lengths of local typed lists; pop updates the count and returns
  the removed value. A direct QEMU smoke confirmed local and static-list
  lengths, returned and retained values, and nonzero termination when reading
  the removed index. Bare lowering also
  omits managed static-handle registration for compiler-emitted arrays, so
  this hosted-runtime bookkeeping no longer needs an AArch64 backend shim.
- Corrected the AArch64 NYIR bounds-check success branch to skip its trap
  instruction. QEMU now confirms an in-range read after a list pop returns
  the retained element; an out-of-range read still traps.

### C frontend

- Added native preprocessing with `-E`, optional line markers, standard input,
  include search paths, ordered `-D`/`-U` object macros, and `-o` output.
  Local headers now support nested includes, include guards, `#pragma once`,
  missing-header diagnostics, and file-local conditional balancing.
- Fixed replacement-text lifetime across macro redefinition and `#undef`.
  Earlier tokens no longer reference freed buffers. Conditional preprocessing
  now expands function macros and late-bound aliases with correct precedence,
  protects `defined` operands, short-circuits logical operators, and skips
  already-taken `#elif` branches.
- Added truthful NYC identity/version macros and a build-version query.
  NYC does not impersonate GCC or Clang. Object modes accept explicit NYR
  optimization levels; external character arrays can be declared repeatedly
  and completed by a compatible definition.
- Unified modeled and filesystem headers under the same include pipeline.
  Added Linux x86-64 password, directory, file-time, ioctl and resource-usage
  declarations, checked against GCC layouts and real libc calls.
- Added file-scope enum definitions, typedefs, integer constants, array extents
  and enum function signatures. Empty definitions and duplicate enumerators
  are diagnosed; enum constants currently must fit `int`.
- Removed helper calls from disabled assertions and allowed `<assert.h>` to
  be included again after changing `NDEBUG`. Preserved void-expression typing
  and operand side effects.
- Corrected function-pointer call precedence under unary operators and casts,
  including calls whose result is discarded with `(void)`.
- Lowered runtime expressions in automatic aggregate brace initializers,
  including nested aggregates, arrays, floating-point conversions, pointers,
  and bitfields. Preserved omitted-element zeroing and literal-only templates.
- Added string-literal initialization for character-pointer globals and kept
  mutable scalar globals as lvalues for prefix/postfix updates. Distinguished
  pointer constness from pointee constness.
- Fixed ownership of aggregate lookahead tags, exported relocation arrays,
  borrowed call metadata, and runtime-helper symbols. Whole-unit data export
  now uses one table sized to the objects produced by all functions.
- Released incoming-edge arrays when eliminating NYR PHIs. Copy and constant
  rewrites now release the replaced instruction's owned operand metadata.
- Corrected argument prescan in nested function-like macros while preserving
  raw arguments for `#` and `##`. Kept expanded `__LINE__` text alive through
  translation-unit parsing instead of reusing a temporary buffer.
- Removed translation-unit-sized local-table clearing from each C function;
  parameters and local entries are fully initialized when inserted.
- Added the variadic `printf` declaration to the modeled `<stdio.h>` header,
  allowing ordinary C programs to call it without redeclaring the function.
- Reserved fixed-size C automatic storage at function entry while preserving
  declaration-time initialization and distinct allocation roots. Loops with
  local arrays or aggregate temporaries no longer exhaust the stack.
- Compiled the unmodified SQLite 3.46.0 amalgamation with `nyc --all` and ran
  an NYR-compiled API driver. In-memory SQL and multi-page disk database
  comparisons, including rollback, `VACUUM`, integrity checks, and reopening,
  agree with GCC; this is not a full upstream SQLite test-suite result.
- Corrected x86-64 aggregate arguments in the final integer registers, struct
  copies with register-resident addresses, and transfers between integer and
  floating-point register homes.
- Corrected bitfield layout, sign extension, constant initialization, and
  read-modify-write updates without changing neighboring fields.
- Preserved signed-byte and signed-short values across calls, fields,
  pointers, and narrowing. Two-byte compound lvalue stores no longer overwrite
  adjacent fields.
- Reloaded narrow C locals at their declared width after pointer writes, so
  out-parameters cannot retain stale upper bytes from an eight-byte NYR slot.
- Emitted scalar static locals as persistent data instead of automatic stack
  slots, preserving their address and mutations across calls.
- Corrected aggregate pointer-table indexing and parenthesized member
  addresses. Taking an address no longer evaluates a load from that address.
- Preserved full pointer width in aggregate arrays of byte pointers, including
  their element stride and pointee type.
- Evaluated constant `sizeof` from resolved types and aggregate layout instead
  of guessing from token names; static method-table descriptors now retain the
  actual size of their file objects.
- Passed a whole automatic array's storage address to byte-oriented C APIs,
  rather than the address of its internal pointer slot.
- Kept C lexical scope separate from NYR logical slot identity so unrelated
  floating-point and pointer locals cannot acquire each other's ABI class.
- Preserved allocation initialization across SSA merges and pointer selects;
  dead-store elimination must not discard headers returned through an
  interior pointer.
- Added a distinct entry predecessor when a local-memory function starts at a
  loop header. SSA parameter initialization no longer resets an updated
  parameter on every backedge.
- The standalone C frontend now handles signed 32-bit `int` values, including
  function ABI boundaries, narrowing from `long`, local updates, and typedef
  aliases.
- The standalone C frontend now tracks active `#define` / `#undef` names and
  integer-valued macros for bounded conditional-expression evaluation,
  including arithmetic, shift and bitwise operators, character constants,
  line continuations, multiline comments, and compiler feature probes.
  General file inclusion remains unsupported; conditional selection has
  compile-link-run coverage.
- The standalone C frontend expands recursively chained empty and multi-token
  object-like macros in C source.
- The standalone C frontend supports `char *` parameters, pointer arithmetic,
  byte indexing, byte loads, and byte stores in its SysV x86-64 object path.
  Byte loads zero-extend; signed `char` values with the high bit set remain
  unsupported. File-scope character arrays initialized from string literals
  are emitted as writable data symbols and can be indexed by compiled code.
- The standalone C frontend supports `unsigned long long` ABI values and
  arithmetic that preserves their 64-bit representation, unsigned relational
  comparisons, and 64-bit integer literals. Division, remainder, and right
  shift remain explicitly unsupported.
- The standalone C frontend now preserves opaque and function-pointer values
  through function signatures, supports pointer-to-pointer parameters, and
  lays out signed `int` fields in tagged aggregates with native field loads
  and stores. It also supports signed `int *` parameters, dereferences,
  indexed loads, and stores. Variadic declarations parse, but indirect calls,
  variadic definitions, and variadic calls remain unsupported.
- The standalone C frontend now supports scalar `unsigned char` and `unsigned
  int` values. Unsigned-int arithmetic wraps at 32 bits and relational
  comparisons use unsigned ordering; division, remainder, and right shift
  remain unsupported.
- Whole-unit C objects now support numeric scalar and pointer globals,
  including reads and simple writes; `extern` scalar data references resolve
  to linker-provided C symbols. The translation-unit test links and runs
  signed/unsigned integer, byte, pointer, double, and external-global cases.
  Scalar `char` parameters, locals, and results use zero-extending byte
  semantics; high-bit values do not match signed `char` on x86-64.
- Whole-unit C objects also emit file-scope aggregates and initialized static
  aggregate locals. The compile-link-run fixture verifies persistent field
  updates through both storage forms.
- SysV x86-64 C calls may now pass scalar arguments to variadic declarations.
  The caller performs default promotions and sets `%al` for floating-point
  register arguments; the frontend still cannot read variadic arguments.

### Documentation and contributor workflow

- Moved fixture diagnostics, fuzz scratch, optimizer-test files, and example
  output under `build/`. Repository discovery no longer requires a local
  scratch directory; default model lookup uses the managed asset cache.
  Public measurements retain findings and comparison limits, not private
  filenames or session instructions.
- Condensed the measurement archive into workload results, retained mechanisms,
  rejected experiments, and verification limits; contributor procedures remain
  in the public guides.
- Replaced numeric AST-kind output with `--dump-ast` source trees and unified
  typed/filter options. Grouped dump controls in compiler help; trees include
  source positions, child roles, defaults, fields, and available semantic facts
  without execution. Terminal syntax colors honor normal color detection.
- Removed landing-page slogans, repetitive preambles, and decorative emphasis.
  Tightened reference prose and separated historical investigation notes from
  current acceptance work without changing source examples or measurements.
- Replaced the linker roadmap with the implemented request fields, tool
  selection, entry conventions, and ELF limits.
- Bare Linux x86-64 AOT now provides zero-filled `malloc`/`free` using anonymous
  `mmap`/`munmap`; an end-to-end fixture checks allocation, memory access,
  release, and absence of dynamic linkage.
- Bare Linux x86-64 monotonic-clock encoding now uses the unsigned 32-bit
  immediate writer for its syscall number and nanoseconds multiplier. A
  `-no-std --native` `ticks()` smoke run returns a timestamp.
- The in-process ELF64 linker now emits AArch64 executables from NyR-produced
  ELF objects, applies AArch64 call relocations, and injects a Linux AArch64
  entry stub. The public acceptance test links and runs a cross-function
  result-returning executable under QEMU when available.
- Dependency-free Linux x86-64 bare programs now use the in-process ELF linker
  and encoded startup instead of invoking the host assembler. The internal
  startup stub provides the same no-op AOT argument hook as the existing bare
  runtime shim.
- Constant-output Linux AArch64 bare programs now emit direct `write` syscalls
  and a sectionless ELF image without invoking the host assembler.
- AArch64 bare NYR now lowers runtime-backed signed integer printing and
  constant-boolean formatting and newline output to direct `write` syscalls.
  Decimal formatting handles zero, negative values, and `INT64_MIN` using
  per-function stack scratch space; monotonic nanosecond ticks use the
  `clock_gettime` syscall. QEMU coverage runs without a host compiler or
  assembler.
- AArch64 bare `malloc`/`free` now use anonymous `mmap`/`munmap` syscalls,
  preserve the runtime allocation header, and reject size overflow. A
  multi-page QEMU fixture verifies memory access and release without libc.
- AArch64 bare typed-buffer allocation now uses `mmap`, writes the native
  typed-buffer header, rejects multiplication/size overflow, and supports the
  zero-filled `i64buf_new` load/store path without external runtime symbols.

- Added native `ny-make` global and command-specific help (`help <command>`).
  The wrapper uses it when built; the clean-checkout Python bootstrap now
  prints only a compact overview and directs users to build `ny-make` for
  detailed help. Help follows shared color and glyph settings; verbose builds
  show underlying commands. Section rules align to rendered icon widths and
  stop inheriting heading emphasis.
- Routed `./make --version` through native `ny-make` when available; the
  bootstrap retains the same short fallback for a clean checkout.
- Consolidated execution/adoption guidance in the status guide, syntax
  extensions in the compile-time guide, vectors in the numbers guide, and
  effect-row coverage in the function reference.
- Published debugging, profiling, graphics verification, and open acceptance
  work through `CONTRIBUTE.md`. Guides and measurements live under `learn/`;
  detailed development records stay with their release here.
- Replaced overlapping optimizer inspection spellings with
  `--inspect[=program|runtime]`, and bounded undefined-name edit-distance
  checks to the supported two-character suggestion threshold.
- Sparse CFG labels now use a sorted lookup index instead of rescanning the
  function for each branch; dense labels keep direct lookup.
- Clarified deep verbosity: `-vvvvv` omits imported-library internals;
  `-vvvvvv` includes them.
- Kept deep trace configuration summaries to tag counts by default, with full
  names available through `NYTRIX_TRACE_CONFIG_VERBOSE=1`; removed the
  duplicate `--native-dump-ir` spelling in favor of `--nyr-dump`.
- Consolidated NYR dump modifiers into `--nyr-dump=raw,cfg,stats,passes`;
  `--nyr-dump-to=PATH` now names text output files. Binary bundles use
  `--dump-nyr[=PATH]`.
- Standardized active dump switches on `--dump-*` names. Removed the duplicate
  `--track-value` spelling and parser-accepted no-op dump flags; NYR pass stats
  remain available through `--nyr-dump=stats`. Help groups source, NYR,
  backend, and failure-artifact inspection. Token dumps exclude preloaded
  library text; inspection flags bypass the tiny inline evaluator for `-c`.
- Value-taking options no longer consume a following `--flag` as their value.
  Missing values now produce a concise error; use `--option=VALUE` for
  option-looking values.
- Compacted symbol resolution to one key-value path/result record per lookup;
  `NYTRIX_TRACE_RESOLVE_VERBOSE=1` enables candidate, alias, and cache details.
- Clarified that package discovery has no default repository or implicit public
  fallback. Users add the source repositories they trust.
- Corrected numeric literals, atomic return documentation, function syntax,
  and Texinfo resource binding. Recorded the dictionary-return, dynamic-nil,
  deferred-capture, and atomic-update findings; subsequent compiler fixes are
  recorded below.
- Fixed static paragraphs, wrapped inline code, escaped table pipes, ordered
  list continuations, indented closing fences, emphasis, disclosure blocks,
  heading anchors, metadata comments, explicit index links, and repository-source
  navigation.
  The renderer regression passed native AOT/JIT and LLVM MCJIT/ORC with library
  caches disabled.
- Added prerequisites and short next-step routes across learn pages, reordered
  the guides and specification by learning path, and added contents navigation
  to long references. Markdown blockquotes now render as semantic callouts;
  the generated page stylesheet defines dark and light callout/code colors.
- Assigned topic-specific icons to learning and specification navigation,
  cards, and page headers. Added history, checklist, debugging, performance,
  contribution, workflow, and linking symbols; aligned the package/add-package
  geometry and replaced the mathematical glyph with a continuous summation mark.
- Removed the redundant document-title bar and generic manual-page caption.
  Long contents panels scroll within a bounded height; outline and direct
  heading navigation reveal containing disclosures.
- Removed Proofs from the overview's curated cards while retaining its guide
  in the manual tree, search, and ordinary documentation links.
- Separated sidebar folders, pages, and API symbols with tree guides and
  consistent indentation. Native disclosure buttons expose their expanded
  state and support Enter/Space; namespace and module links navigate separately
  from their arrows. Keep the selected page visible and prevent type-to-search
  from stealing keys from focused controls.
- Re-verified the duplicate-scan preserved forms in source and dropped the
  resolved note from `docs/TODO.md`: per-assertion selftest messages, existing
  macros, explicit `NY_NATIVE_ENTRY`/`NY_NATIVE_ENTRY_FN` ABI spellings, direct
  `&f->data[i]` optimizer iteration, and typed per-arity callback calls all
  stay as written.

### Compiler gates and optimization evidence

- `str.repeat` now uses a managed runtime helper that allocates the exact
  result size and copies the source with geometric doubling, replacing the
  byte-at-a-time Ny loop. The full benchmark run passed the `repeat`,
  `word-frequency`, and `word-frequency-scan` checksum fixtures. A fresh
  compiler smoke also passed positive, zero, negative, and empty-string
  repetition. The run still fails the string-class performance gate:
  `word-frequency-scan` measured
  52.0 ms on NYR versus 2.59 ms on LLVM (20.06x, against a 5.00x limit); no
  benchmark limits or scanning semantics were changed.
- Dynamic callback adapters share closure-safe callable preparation. The
  arity-specific call paths remain typed, avoiding a generic argument-array
  copy across ABI boundaries. Cold dynamic-callback VM and raw-result fixtures
  completed without failures:

  ```bash
  NYTRIX_TEST_COLD=1 ./make test \
    --pattern etc/tests/lang/execution/dynamic-callback-vm.nshape --failures-only
  NYTRIX_TEST_COLD=1 ./make test \
    --pattern etc/tests/lang/execution/dynamic-callback-raw-result.nshape --failures-only
  ```

- Split `ny-test` help rendering from argument handling and separated
  benchmark fixture discovery and worker scheduling from the suite driver.
  `./make ny test --help` retained the complete help surface; a one-run
  correctness benchmark selected only the exact `repeat` fixture and passed.
- Failure replay now treats `--native --run` as an explicit execution route,
  matching the test runner instead of rewriting it to `--native-only`. The
  renderer-color fixture checks its output marker across the configured route
  matrix.
- Test and benchmark gates hash fixture, standard-library, and generated
  `core.ny` inputs at gate start and recheck before reporting a verdict. A live
  source mutation now reports `INVALIDATED` and exits 2 rather than presenting
  a pass or failure.
- Loop-vectorizer attempt and rejection counts survive runs that make no IR
  change. The vector unroll/tail bound now preserves the power-of-two step
  required by its mask-based scalar epilogue; a nested dynamic-buffer regression
  checks both vector accesses and every updated element. The cold dgemm bench
  now matches NYR, LLVM, and C checksums and reports 31 attempts, 30 rejects,
  and one vectorized loop. This single sample is correctness/counter evidence,
  not a performance claim. Scalar epilogues remain in use; benchmark loop
  counters do not count SLP groups, and the focused `slp_debug` test checks
  source-location retention only. Full commands and results are in
  [measurements](learn/measurements.md#selected-workload-results).
- x64 stack-home reuse stays disabled in call-bearing blocks and for
  floating-comparison predicate results, where block-local intervals can
  miscompile live values. Cold spectral, nested-borrow, and optimized list-fill
  fixtures pass on their affected routes.
- The bounded x64 loop-GPR allocator now also handles vector-bearing functions
  with indirect memory; scalar indirect-memory functions retain the
  conservative stack-home path. A 9+3 SOR run matched NYR, LLVM, and C
  checksums and measured 3.52 ms, 762 µs, and 1.48 ms respectively
  (NYR/LLVM 4.62×, below the 5.00× fixture limit).
- Deferred closures now keep read-only local captures as registration-time
  snapshots and capture mutable bindings by address only when the deferred
  body assigns them. This makes updates visible after LIFO scope cleanup
  without changing read-only loop captures. The diagnostic VM now drains
  deferred closures through its own resolver when a panic resumes a catch, and
  treats null callbacks as no-ops. Cold `defer.nshape` and
  `defer-loop-capture.nshape` matrices passed on native JIT, native-only, LLVM
  MCJIT, LLVM ORC, and the diagnostic VM. They cover nested string updates,
  raw-int updates, f64/f32 slots, panic unwinding, null callbacks, and
  registration-time loop snapshots:

  ```bash
  NYTRIX_NO_PROGRESS=1 NYTRIX_TEST_COLD=1 ./make test \
    --pattern etc/tests/lang/execution/defer.nshape --failures-only
  NYTRIX_NO_PROGRESS=1 NYTRIX_TEST_COLD=1 ./make test \
    --pattern etc/tests/lang/execution/defer-loop-capture.nshape --failures-only
  ```

- The pipeline reference now maps the shared source-to-NYIR lowerer to the
  LLVM, owned-native, VM, and REPL consumers. It distinguishes active
  `codegen_prepare()` metadata work from legacy AST-body emitters that are not
  CLI or REPL execution routes. The defer matrices above exercise the live
  backend routes.

### C frontend reliability

- Added `nyc --all SOURCE.c OUTPUT.o` to emit all supported function definitions
  in one source unit as a single x86-64 ELF object with their C symbol names.
  The mode remains limited to the frontend's supported C subset and does not
  yet handle general file-scope data.
- The standalone frontend now skips inactive literal `#if 0` branches and
  selects literal `#if` / `#elif` branches. It rejects macro-dependent
  conditional compilation instead of compiling both paths as active.
- The standalone `nyc` frontend resolves file-scope typedef chains for its
  supported signed-`long` and `double` types, preserving `const` qualification.
  The host-C differential suite covers parameters, calls, locals, loops, and
  rejection of writes through a const alias.
- The lexer and integer parser accept C23 numeric digit separators and standard
  `uUlLzZ` suffixes, including `1'000`, `0x1'0000`, `0b1'0101`, `42UL`, and
  `1'000ULL`. Punctuators use longest-match selection, following
  `stb_c_lexer.h`; its public-domain/Unlicense or MIT reuse rights were checked.
- Four large `ny_parser_t` lookahead copies in `skip_unary_expr`,
  `skip_cast_expr_check`, aggregate lookahead, and `parse_array_extent_primary`
  use heap storage instead of roughly 11.7 MB of stack each. Recorded parser
  checks through `ny_parse_decl` cover `int x[1]`, `int arr[1000]`, and
  `int arr[1'000]`.

### Runtime, library, and build integration

- `str.word_frequency` counts directly without an intermediate token list;
  a follow-up corrects the count for words appearing once.
- Native `??` lowering evaluates the left operand once, preserves non-nil
  false/zero/empty values, and handles optional-chain fallback. Separate
  initialization fixes avoid double-tagging nil and confusing dynamic `false`
  with nil.
- Static LLVM strings are distinguished from tagged runtime strings at
  C-string boundaries. The sorted builtin alias table again resolves
  `core.set_mod.len`.
- Native UI startup discards the trace logger's floating-point result rather
  than returning it from a side-effect-only function. Large UI bundles use a
  larger bounded relocation capacity.
- Windows formatter/audit workers use portable processor discovery; Darwin
  test tools include their signal declarations; BigInt CPU-feature detection
  works across the supported Clang versions.
- Test path filters accept either directory separator across platforms.
  POSIX peak-memory reports use host-specific units, while audit
  workers share sorted function metadata instead of duplicating analysis.
- Windows native execution uses the host calling convention, reserves
  sufficient compiler stack, and exports internal runtime helpers for JIT
  lookup, preserving strict public FFI pointer boundaries.
- Windows failure replay retains compiler/debugger logs. Benchmark names and
  JSON reports handle native paths and escaped text across platforms.
- `./make check` streams cold standard-library, failure-replay, and benchmark
  checks and opts into auxiliary preflight drivers and CTests. CI also has an
  explicit build/source-check mode and manual controls for native platforms,
  filtered fixtures, replay, and serial benchmarks, with retained logs and
  summaries.
- Building `ny-test` also builds the standalone CLI tools used by command
  parity and documentation-discovery fixtures.

### Native x86-64 backend

- CFG construction solves immediate dominators by reverse-postorder ancestor
  intersection, then builds the dominance bitsets once. Unreachable blocks
  retain self-only dominance and do not affect reachable merges.
- The opt-in loop-phi carry policy now applies to scalar FPR values as well as
  GPR values. Its allocator regression and native float loop fixture pass with
  carry enabled, including calls, CFG joins, and conversion scratch use; the
  default policy is unchanged.
- NYIR v2 bundles preserve native strings, arrays, constants, and global
  symbols. `--native-from-nyr=PATH -o FILE` builds a native executable
  directly from saved NYIR; v1 bundles remain VM-runnable but lack this data.
- Signed remainder by a known power-of-two divisor now uses shift-one
  bit/remainder lowering instead of `idiv`; negative odd operands still
  produce `-1`, matching truncating signed remainder. The `collatz` benchmark
  checksum matched NYR, LLVM, and C (`178`). With 25 runs and 5 warm-ups,
  NYR measured 250 µs versus 259–262 µs before the change; one paired
  p95 comparison moved from 277 µs to 261 µs. Disassembly confirms the
  hot `% 2` path no longer contains `idiv`. `binary` stayed at 3.23 ms
  (5 runs, 2 warm-ups).
- The internal ELF64 linker now copies section contents directly into the
  zero-filled output image, avoiding temporary segment buffers and a second
  segment-to-image copy. Multi-object and archive link acceptance passes.

### Project demos

- `etc/projects/donut.ny` is now the runnable donut-shaped source itself, not a
  comments-only preview followed by a separate readable program. The private
  Gist-derived shaper removes common left padding and picks the smallest
  configured mask that fits the executable source. The renderer uses a 16-bit
  depth buffer, a 0.02-radian ring step, and 12 surface-normal light levels
  mapped across ANSI blue, magenta, cyan, and white with restrained bright
  highlights. A shared viewport scale uses the drawable width and height to
  keep the torus proportional and avoid clipping across sampled rotations and
  terminal sizes. Terminal mode helpers coerce TTY status to integers before
  testing success, so a successful restore does not fall through to the
  sane-mode fallback and alter saved termios flags. The source was redirected
  directly into `donut.ny` and is 3,276 bytes, 451 bytes smaller than the
  pre-dedent output. `./make ny etc/projects/donut.ny` rendered without
  type-fallback warnings. A PTY smoke
  passed 80x24 → 40x16 → 100x32 resize, four ANSI colors, `q` exit,
  alternate-screen restoration, and exact terminal-attribute restoration.
- Native dictionary-key lowering now tags proven raw integer carriers even
  when an imported expression lacks source-level representation facts.
  This keeps variable and literal key lookups in the same domain: X11 W
  remains key 87/scancode 25 instead of colliding with scancode 51.
  Native reachability retains the source bodies of `window.key_down` and
  string replacement helpers instead of assuming every call is inlined.
  The cold `x11-logical-key-map.nshape` regression passed; six focused
  dictionary, nil, and keyboard fixtures also passed with execution caches
  disabled.
- `etc/projects/ui/pong.ny` now keeps its physics helpers and mutable game
  state explicitly `f64`. It converts `core.math.abs` and container-returned
  values at the numeric boundary, preventing `any` from leaking into steering,
  swept collision, wall reflection, and ball motion. A strict-types mock run
  completed 120 frames and emitted `NYTRIX_FIXTURE_READY pong`.
- UI demo verification now uses a private Xvfb smoke that captures the generic
  `w` key while it is held, rather than inferring app input from the X server's
  keymap or a screenshot taken after release. GUI window collapse state is
  normalized to `bool` and only applied when both titlebar and collapsibility
  are enabled; the UI engine passes the current tiled editor/inspector bounds
  instead of stale fixed rectangles. Headless OpenGL smokes showed visible
  synth and editor output after their slow software-rendered first frames,
  retained output after resize, and a visible Pong response while `w` was
  held. A renderer-probe resize capture taken after its own five-second draw
  lifetime was correctly identified as invalid; a run resized before that
  deadline retained its colored primitives. The synth no longer forces
  Vulkan, so the configured backend can be selected. Vulkan presentation
  remains unverified in Xvfb: Mesa reports that DRI3 is unavailable and
  logical-device initialization fails before the window maps.
- Terminal grid resize now keeps the old column stride as an `int` before
  computing source-row addresses. The dynamic dictionary value had corrupted
  copied cells when widening the grid. Raw row copies now use
  `__copy_mem_raw`; the focused `vterm-cell-output.nshape` regression verifies
  preserved rows and blank newly allocated rows after resize.
- Linux joystick initialization and the fallback rescan timestamp now use
  dedicated module state rather than dictionary fields. This keeps initialization
  one-shot and preserves the five-second rescan throttle when inotify is
  unavailable. The private Xvfb input smoke rendered the keyboard, registered
  the synthetic `W` key, and retained visible keyboard content after resizing.

### UI renderer

- Vulkan frame statistics now update in the owning state module, so static,
  indexed, and dynamic draw submissions are reflected in the captured frame
  counters. The focused `ui_replay_engine_native` fixture passed, and a live
  Vulkan capture reported 45 draws (43 dynamic, 2 static, 1 indexed).
- Dynamic string classification now validates the complete small-string header
  once, and address readability checks no longer query the map oracle twice
  for known mappings. Two matched six-second headless input-renderer profiles
  reduced cycles by about 8.5% and instructions by about 11% versus the
  pre-change sample; keyboard input and resize captures remained visible.
- The OpenGL draw module now imports `VERTEX_STRIDE` as a named constant.
  Rounded rectangles, circles, and rings therefore allocate and address the
  expected vertex bytes instead of treating the module-qualified constant as
  zero. Packed integer circle/ring colors use the shared color-component
  decoder, and ring validation uses a non-mutating normalized inner radius.
  Scratch-buffer growth preserves the existing allocation if `realloc` fails
  and returns no buffer if its fallback allocation also fails. A private
  640×480 Xvfb capture showed colored rounded-rectangle, circle, SDF-rectangle,
  and ring output; the focused `render-color.nshape` passed all three variants.
- GL text quads now use the typed vertex writer, retaining NaN/infinity
  fallback and f32 range checks without routing every text vertex through
  dynamic `any` conversion. The synth demo applies FX controls only when their
  values change and scales its coordinate space to preserve the layout and
  pointer mapping after resize. The focused renderer fixture passed all three
  variants, and private Xvfb captures showed colored text/primitives and a
  fully drawn synth at 720×480.

### Test runner

- Parallel-runner wait heartbeats now use a compact active/oldest/elapsed-time
  line and emit every 45 seconds instead of every 30 seconds.
- The Ny desugar worker uses a compact unique temporary basename so its
  generated path literals retain their string lengths when passed through the
  VM. The cold `ny-desugar-parity.nshape` fixture passed all eight variants.
- The `.nshape` runner now supplies the CMake executable and sibling
  `ny-fmt`/`ny-doc` paths when the caller has not provided explicit values.
  This makes the migrated CTest fixtures run through `./make test` as well as
  CTest; the 12 previously failing environment-dependent fixtures pass in a
  focused run. The full 14-fixture failure set from the reported cold run also
  passed together through `ny-test` with execution caches disabled and shared
  core bytecode supplied.
- CTest compiler/documentation checks now use the same `.nshape` runner as
  focused tests. The legacy derive expansion fixture retains its AST digest
  assertion; documentation discovery checks standalone/embedded output parity,
  current ranking order, and the no-compiler fallback. Module-cache,
  standard-source-cache, and AOT-cache identity checks now express their file,
  process, environment, and cache-hit assertions in executable `.nshape`
  fixtures. Formatter case-dispatch parity also runs as a shape fixture with
  the standalone formatter supplied by CTest; CLI alias/option parity runs
  directly through the same compiler fixture runner. The operator-surface
  integration now exercises NYIR, native, LLVM, emitted executable, and Wasm
  paths from one executable fixture. Formatter operator migration now checks
  legacy, conservative, mixed, and embedded-shape source through the fixture;
  formatter CLI rejection and repeated-literal audit parity now run both tool
  executables from `.nshape`. Removed duplicate helper scripts and the legacy
  derive helper source; CMake still registers target-dependent checks and owns
  build setup. The cold-start compile budget now runs from `.nshape`, where it
  controls the cache-bypass environment, creates an isolated source, validates
  execution output, and enforces the configurable wall-time limit. AOT-cache
  identity now uses a per-run source pathname so shared system-temp cache
  entries cannot turn its expected initial miss into a stale hit. Formatter
  and native-image cache scripts that require separate stdout/stderr capture,
  PTY-backed REPL tests, and target/toolchain-owned C tests remain CMake-driven.

### Native backend performance

- The x86-64 AVX2 `vbroadcastsd` encoder now emits the required `W=0` VEX
  form. The old form trapped with `SIGILL` in `dgemm` and `sor`; the native
  `native_vec4_f64` machine-emitter check now executes successfully, and both
  fixtures return their expected checksums.
- Native assembly no longer emits empty `.rodata` or `.data` sections when a
  program has no corresponding literals. String-table data still follows code
  without an explicit alignment directive and stays in non-executable
  `.rodata`.
- Linux x86-64 bare AOT now folds checked integer constant expressions in
  `print`/`eprint` arguments, including unary signs, parentheses, and
  `+`/`-`/`*`/`/`/`%` precedence; calls also accept multiple constant
  arguments separated by spaces in output, including `true`, `false`, and
  `nil`. Top-level typed/untyped integer `def` and `mut` bindings and checked
  constant assignments are folded as well. Overflow and division by zero
  reject the tiny path instead of emitting incorrect output. The bare artifact
  regression executes a nine-write program and checks that its ELF has no
  sections or dynamic linker metadata.
- Bare AOT now falls back to the native x86-64 backend for programs outside
  the constant-output subset, links with a syscall-only `_start`, no CRT or
  libc, and section garbage collection. Syscall shims cover integer/boolean
  conversion, string/integer/boolean `print`/`eprint`, and monotonic nanosecond
  ticks from native functions; other unresolved runtime/external dependencies
  still fail closed. Regressions execute function/control-flow programs,
  verify no dynamic section, and exercise compiled functions with runtime
  arguments.
- Bare native AOT can allocate zeroed typed buffers without libc through the
  Linux anonymous `mmap` syscall. The fixture checks typed-buffer header
  behavior, indexed stores/loads, zero-filled untouched elements, and absence
  of a dynamic section. Bare ELF executables now use Nytrix's in-process ELF
  linker; the minimal startup/runtime shim object is still assembled by the
  configured host C tool.
- x86-64 NYIR parameter metadata now carries signed and unsigned byte widths
  through binary serialization, machine lowering, and argument prologues.
  `u8`/`bool` values zero-extend and `i8` values sign-extend from the first
  three SysV registers or stack slots. The
  `byte-argument-abi.nshape` linked-runtime regression covers byte identity,
  mixed three-argument arithmetic, and signed/unsigned stack arguments.
- The x86-64 NYIR assembly string pool no longer aligns byte-addressed literals
  to four-byte boundaries. String references remain RIP-relative and the pool
  remains in non-executable `.rodata`.
- Eligible x86-64 integer leaves now consume immediate-copy operands directly
  in the frameless register path. Compile-only assembly for `x + 1` contains
  `movq %rdi, %rax; incq %rax; ret`; the linked ABI fixture exercises that
  function alongside signed/unsigned byte arguments.

- Reused CFG topology and definition positions within peephole range queries,
  with invalidation after instruction insertion, value-count changes, or storage
  moves. Skipped singleton refinement and rejected ineligible parity definitions
  before scanning their uses. A validated editor profile pair fell from 69.7 to
  23.7 minutes while preserving the 33,223,632-byte object and all recorded inline
  decisions; see the [measurement record](learn/measurements.md#validated-editor-compile).

- Fast glyph texture ids now decode the packed 32-bit sentinel as signed before
  validity checks. GL text no longer treats `0xffffffff` (the unavailable
  texture marker) as a usable texture; zero-ink fast glyphs are reported as
  present only for the ordinary and non-breaking space characters. If the GL
  flat-color text batch cannot draw any glyphs, it now falls back to the normal
  text path instead of silently dropping the run. A live GL renderer-probe
  smoke showed both the white label and yellow `FAST TEXT RUN`; the unavailable
  glyph now reports `A=false, texture=-1` rather than the invalid unsigned id.
  The GL built-in bitmap fallback also batches horizontal lit-pixel spans into
  one vertex submission per string instead of issuing a rectangle draw for each
  lit pixel; the renderer-probe screenshot retained both labels after this
  change.
- Native branches now decode mutable typed-bool globals from their canonical
  tagged storage before testing them. Previously, stored `false` was the
  nonzero false-tag and incorrectly selected the true branch, which caused
  native UI startup flags to enable unrelated paths. The new
  `global-bool-storage.nshape` regression passes native O0/O3, native-only,
  and LLVM JIT variants.
- Native lowering now preserves the pointer representation when reloading a
  mutable top-level typed-buffer handle. The inferred-buffer loop no longer
  emits `rt_any_to_i64` conversions for those loads; its `rt_main` static-island
  counts changed from 2 dynamic operations, 2 box/unbox conversions, and 10
  runtime calls to 0, 0, and 8. A native-result-oracle smoke passed, and the
  1,000,000-element typed-byte sieve returned the expected 78,498 primes.
- The inferred-pointer reload classifier now requires a pointer type name
  instead of treating every pointer-shaped semantic representation as a raw
  pointer. This keeps typed-buffer globals on the raw path without corrupting
  native execution of the legacy derive-expansion fixture.
- OpenSSL EVP/HMAC imports and cached digest/context values now use `ptr`,
  matching the C pointer ABI instead of tagging them as language handles.
  This fixes optimized NYIR verification of `EVP_MD_CTX_free`; the focused
  legacy derive-expansion fixture passes both configured routes.
- Private-list scalar replacement now forwards a slot load only when one
  matching i64 store dominates it in the control-flow graph, including stores
  before a branch. Bounds elimination uses the actual eight-byte access width
  against the buffer's logical capacity. The observable recursive-list
  reproducer's `count` static-island facts changed from one heap allocation,
  two runtime calls, and two bounds checks to zero for all three; its
  `--native-oracle-per-pass` checksum is 4,057. The focused legacy
  derive-expansion fixture also passes after the optimizer change.
- The synthesized `rt_main` now keeps private F64 globals in entry-local slots
  when no user function references them. The n-body workload improved from
  5.22 ms to 4.67 ms NYR time; its matched 5-run/2-warm-up LLVM comparison
  measured 1.98x NYR/LLVM, down from 2.23x.
- Native GPR loop-phi coalescing is opt-in with `NYTRIX_GPR_LOOP_CARRY=1`.
  The guarded-division regression showed that default carried-register
  residency could lose a loop accumulator update; normal builds now retain
  canonical homes until the encoder's edge proof covers that case. The
  opt-in previously improved the focused binary benchmark from 3.93 ms to
  3.67 ms over matched 5-run/2-warm-up samples, with matching checksums.
- The generic indexed setter now tags only values whose emitted NYIR
  representation is raw. An already-tagged integer key was being tagged a
  second time, so dictionary lookup used a different key than assignment.
  The focused X11 logical-key-map probe now passes.
- Loop-phi edge coalescing now rejects caller-saved colors clobbered anywhere
  in the live source segment, not only at its terminator. A call in the
  preheader previously allowed the timer call to clobber a carried loop value,
  causing checksum divergence in `call-chain`, `hash`, `inline`, and `licm`.
  A register-allocation regression covers the preheader-call case. The focused
  benchmark checksum gates now pass for all four fixtures.

### Audio playback

- Sound streams now retain the backend-opened handle and backend ID, and the
  sound facade normalizes its dynamic looping argument before dispatch. Pulse
  playback with `init(false)` therefore stays synchronous instead of starting
  a mixer thread that cannot access the open stream. Synchronous playback now
  reports write failures instead of returning a successful instance, and
  backend diagnostics report the selected backend. The backend initializes its
  master gain before first playback, and the S16/F32 fast mixer preserves its
  floating-point cursor across output blocks; this prevents silence from an
  unset gain and broken samples after the first period. The remaining severe
  distortion came from deriving frame bytes through dynamic integer division:
  S16 stereo was calculated as 10 bytes instead of 4, so each mixer write read
  beyond its 1,024-byte block. Frame sizing now maps the declared S16/F32
  format directly to 2/4 bytes per channel; the backend I/O self-test covers
  mono and stereo for both formats. The sound source fixture verifies
  consecutive mixer blocks. The sound project writes a 48 kHz, 440 Hz sine
  WAV with a smooth 20 ms edge fade, loads and plays that WAV synchronously
  through the selected backend at unity gain, then cleans up. The source stays
  at 4% full scale; the mixer does not reduce the WAV's amplitude.

### Terminal projects

- Matrix rain now follows cmatrix's alternating-column layout, with independently
  paced streams, varied lengths and gaps, and mostly alphanumeric glyphs with
  occasional symbols. White heads fade through cyan into green; incremental
  cell updates avoid repainting blank canvas cells. The resize-aware
  `fixture.canvas_loop` still owns quit handling and terminal restoration.

### NYR public API

- Consolidated the single- and multi-object ELF linker into
  `src/code/backend/object/linker.c`. Added `nyr_link` requests for executables,
  shared libraries, and static archives: native Linux x86-64 executable links
  keep the in-process ELF path, while other target formats dispatch directly
  to clang/LLD, wasm-ld, or llvm-ar. The public API minor version is now 8.
  A local linker smoke linked sample COFF/PE, Mach-O, and Wasm objects and
  verified each output format; the existing NYR ELF acceptance executable
  passed. Cross-target use still depends on the selected linker and sysroot.
- `nyr_optimize` now runs the optimization pipeline on a deep copy and only
  replaces the caller's function after success. A failed pipeline leaves the
  original NYIR intact, at the cost of temporary memory proportional to the
  function size. The public API reference documents this failure guarantee.

### Compiler and test reliability

- The test runner's `Peak fixture RSS` summary is a `RUSAGE_CHILDREN.ru_maxrss`
  high-water mark across reaped child processes; it is not summed RSS or a
  per-fixture attribution. The investigated `loop-call-current-argument`
  reports used different compiler executables before and after rebuild, so
  their 237.0/243.0/247.7 MiB values are not a matched-binary memory
  comparison. No memory regression was established.
- The 2,185.3 MiB peak reported by `run-960027` is reproducible in the focused
  `kernel-list-alias-cow.nshape` fixture: its cold, serial 2-check run passed
  and reported 2,194.9 MiB. The fixture sweeps list sizes 1–513 while
  interleaving indexed writes through an alias and appends. A matching native
  `-O0` program run returned checksum `26812284435` and reported 2,140.0 MiB
  with `--report-peak-rss`; a no-op native control reported 238.4 MiB. Massif
  attributed the growth to runtime `rt_tbuf_append_i64_raw` and
  `rt_tbuf_clone_raw` allocations. The long differential fixtures listed
  first in `run-960027` each peaked around 346–356 MiB when run alone, and
  `legacy-derive-expansion.nshape` passed cold at 348.8 MiB. Thus the large
  peak is a fixture's runtime allocation workload, not summed worker RSS or
  compiler startup variance. No suite rerun or change to fixture coverage was
  needed.
- Native `int(BigInt)` lowering now passes already-raw i64 values through
  unchanged instead of treating their bits as a tagged BigInt handle. This
  prevents `INT64_MIN` from being dereferenced as a pointer in the native
  result-oracle path. The runtime-bound-wrap oracle fixture passes, and the
  current cold full suite passed 2,352/2,352 fixtures. A cache-enabled
  full-suite run also reported 2,352/2,352; the separate cold run and focused
  cache-disabled executions provide the execution evidence.
- NYIR inlining now stops the outer round loop when splice growth exhausts its
  budget; previously it stopped only the current round and retried until
  excessive IR growth or a timeout. Cloned return copies also inherit the
  call's scalar ABI class, preserving aggregate results across inlining.
  `inline_general` optimizer checks and the focused `inlined-guarded-division`,
  crypto-import, and aggregate-argument ABI fixtures pass.
- PRE may now consider loop-local loads when its existing memory-stability
  proof succeeds; other loop-block expressions remain excluded. The regression
  fixture verifies the redundant load is removed without crossing a store.
  Signed division by a power-of-two constant remains a signed divide unless
  nonnegativity is proven; the machine lowerer still represents that constant
  as an immediate.
- Wasm internal calls now push every argument onto the operand stack instead
  of limiting argument loading to the native four-register count. AVR function
  setup reserves the return-address space below locals before calls. Focused
  Wasm executable-runtime and AVR bounds-check fixtures pass.
- Typed-buffer store helpers now return their declared buffer pointer instead
  of the stored scalar. Native `i64buf_store` and `f64buf_store` lowering now
  preserves that same contract. This fixes pointer loss when a store result is
  assigned and prevents subsequent stores from dereferencing the scalar as an
  address. The focused all-width store smoke and 1M-element u8 sieve completed
  with the expected checksum; the single `knucleotide` benchmark also passed.
- x86-64 object emission now keeps the canonical home for an address base
  forwarded into a nested ADD→load SIB sequence. The SIB emitter loads the
  index first, so it must not clobber an unspilled forwarded base; the
  ownership regression fixture now passes in native execution.
- x86-64 immediate multiplies by 3, 5, or 9 now encode `%rbp`/`%r13` as a
  SIB base with a zero displacement. The previous base encoding selected the
  no-base form and corrupted following instructions; the proven-integer branch
  probe now executes successfully.
- POSIX subprocess capture now uses `posix_spawnp` file actions and
  close-on-exec pipes kept above the standard descriptors. Concurrent
  compiler subprocesses no longer fork from a multithreaded runtime, inherit
  unrelated workers' pipe ends, or close a redirected child stream when a
  parent standard descriptor is unavailable.
- The Ny-side desugar worker now isolates inherited standard-library bundle
  overrides while honoring its explicit minimal-stdlib mode. Test-runner
  prebuilt paths no longer make the nested compiler exhaust its memory limit.
- When shared core bytecode is unavailable, `./make test` now resolves standard
  modules from the checkout instead of forcing the monolithic generated source
  bundle. The n-body native-JIT fixture now passes its previously unresolved
  standard-library string symbol.
- The unfiltered cold suite (`./make test --cold --with-stdlib --no-bench
  --jobs=5`) passed 2,352/2,352 runnable fixtures with zero failures or
  timeouts. All 17 fixture paths in the separate cache-enabled failure profile
  `run-4079221` also passed in this cold run. The runner skipped 199 fixtures
  by policy or host capability: 137 optional system stdlib modules, 36
  CI-skip fixtures, 22 browser-only fixtures, 2 architecture-specific native
  fixtures, and 2 CMake-only sources.


### Example projects and focused regressions

- Rule 110 now renders a mirrored half-state symmetrically and applies the
  correct asymmetric transition table. Inclusive range endpoints are bounded
  to the actual cell counts, preventing an extra terminal column and wrapped
  rows. It stops exactly at the horizontal edge when width limits generations,
  reserves space for the heading and prompt when they fit, and emits no
  trailing blank-line padding. It uses double-width blocks whenever the
  terminal can fit them; on odd column counts the incomplete final column stays
  unused rather than distorting the cell grid. It never clears existing
  contents.
  The 45-line simulator reuses two state buffers and one pre-sized output
  builder.
- Fixed the native terminal-size bridge to store descriptor-list integer
  payloads untagged. `get_terminal_size()` now reports physical rows and
  columns instead of the tagged `2*n+1` representation.
- Fixed SHA-1 fallback hex output to append digest words through a
  managed string builder. Chained temporary-string concatenation could return
  an invalid value and crash callers that read the digest length.
- Fixed global mutable-list method effects so project functions such as
  `draw` keep known effect summaries. Function reachability now resolves
  `utf8_slice` by semantic declaration identity, avoiding a zero-address
  indirect call in the donut renderer.
- The HTTP server now parses decimal `Content-Length`, preserves the success
  flag in response dictionaries, and exits successfully after a bounded
  successful run. Loopback smoke testing returned HTTP 200 with the expected
  `/health` JSON body. Its example project now builds heterogeneous JSON and
  server-option dictionaries with explicit `dict_set` calls and handles
  `/favicon.ico` with an empty 204 response.
- Matrix rain now stores its single-width ASCII glyph table as a string rather
  than rebuilding a 93-entry string list at each indexed glyph lookup. In
  matched 80×24 native `-O3` PTY runs, 40 no-delay frames fell from a 0.700 s
  median to 0.071 s. A timed 40-frame resize smoke completed in 0.898 s and
  restored both alternate-screen and cursor state without adding a newline.
  A current 40-frame PTY profile completed in 0.049 s with no `mincore` calls;
  20 ms pacing completed in 0.875 s. Resizing from 80×24 to 100×32 cleared
  and redrew the canvas, then restored alternate-screen and cursor state.
  `rt_canvas_put_cell` retains its x/y bounds checks.
- Sound resource loading keeps decoded sources dynamically typed instead of
  narrowing them to the initial integer sentinel. Native libsndfile decoding
  opens the normalized file path directly and reads its 64-bit frame count as
  a tagged Nytrix integer. A generated-WAV round-trip regression verifies the
  loaded source type, frame count, rate, and channel count.
- Added explicit scalar types to UI engine loop parameters that were
  incorrectly treated as callback candidates during effect inference.
- Fixed x86-64 call-boundary register tracking: ABI argument placement could
  re-seed a caller-clobbered colored value after its home was flushed, and
  later uses then read the clobbered physical register. Calls now invalidate
  caller-clobbered color residency after argument setup; the existing typed
  list-index ABI regression passes all four execution/optimization variants,
  including the previously failing O2 native route. An unseeded argument now
  loads directly from its canonical home into its ABI destination, avoiding a
  reload into (and clobber of) an earlier argument's register; the native SoA
  codegen fixture passes all four variants.

- All 20 `etc/projects` programs compile. Bounded matrix and donut runs rendered
  frames. The focused project regressions and the two reported suite failures
  (`legacy-derive-expansion` and `realloc-null-stub`) pass cold (6/6).
- Conway's Life now converts dynamic row-offset values to integers before
  indexing, so its first source compilation no longer emits W2101 fallbacks.
  Its non-interactive frame remains bounded to one draw; interactive runs
  retain the 600-frame cap. Random seeds now start at 20% density instead of
  half-filled noise, and the doubled-width cells leave the terminal's final
  column unused to avoid edge wrapping. Canvas frames use the active TUI's
  configured delay, preventing tight-loop redraws.
- `core.os.process.run` now supplies `argv[0]` when callers pass only command
  options, so option-leading commands such as `stty -F /dev/tty ...` receive
  their arguments correctly. Raw terminal mode disables `ISIG`, so Ctrl-C is
  read by the TUI and runs its deferred screen/cursor cleanup instead of
  terminating the process mid-frame. Compiler diagnostics coordinate with the
  live progress renderer, keeping warning and hint lines separate from the bar.
- Matrix rain now uses integer lists for its drop state and builds replacement
  lists locally before publishing them to global state. This avoids dynamic
  arithmetic fallbacks and the NYR invalid-free failure during one-frame runs.
  Its glyph set is single-column, its fading colors avoid per-cell randomized
  palettes, and the terminal-frame scan uses nested row/column counters rather
  than division and modulo for every cell. Frame-stamped trails avoid rewriting
  each live cell every tick. Interactive canvas refreshes repaint changed runs
  instead of transmitting the full screen each frame. Fixture canvas cleanup
  now releases the current resized canvas exactly once, and its incremental
  renderer reuses one output buffer instead of allocating and freeing it on
  every frame. The runtime address
  oracle now returns authoritative positive mappings directly instead of
  issuing redundant `mincore` probes: a matched 40-frame headless Matrix run
  fell from 13.242 s to 2.726 s, and a 10-frame trace fell from 1,614,476
  `mincore` calls to 16.
- Conway now evolves rows with the allocated grid width as its stride. The
  previous extra halving updated only part of each row and indexed later rows
  incorrectly; initial seeding is also sparser to keep the display legible.
  The project compiled and completed 40 PTY-driven frames across a resize,
  restoring the alternate screen and cursor.
- TUI teardown now restores the saved screen and cursor without writing a
  trailing newline, avoiding a blank line before the shell prompt. Rebuilt the
  standard library and verified bounded exit after resize and Ctrl-C through a
  PTY; both exited cleanly, restored the alternate screen/cursor, and emitted
  no trailing newline.
- Donut's palette index and the heredoc example's numeric interpolation now
  keep integer types through their arithmetic, removing their first-compile
  W2101 fallback warnings.
- The audio example's unavailable-backend branch now tests optional sound and
  instance handles as booleans; its compile smoke exits cleanly without W2101.
  The HTTP server explicitly converts request method/path and error payloads
  before using them as strings. Its heterogeneous JSON and server-option
  dictionaries use incremental `dict_set` calls to avoid W2101 fallback
  warnings.

### Benchmark output

- Benchmark invalidation now fingerprints benchmark fixtures, not unrelated
  code-test fixtures, while retaining stdlib invalidation. Graph BFS has a
  workload-specific LLVM/native outlier ceiling of 5.5x, calibrated above its
  recorded 4.62x baseline with modest headroom.
- The Ny heapsort benchmark now stores its array in a typed i64 buffer rather
  than a generic list. Its isolated checksum-checked benchmark measured 7.80 ms
  NYR versus 19.3 ms before the change; the C reference and algorithm are
  unchanged.
- The n-queens benchmark now uses first-row mirror symmetry in both Ny and C.
  The checksum-checked N=13 run fell from 95.8 ms to 52.1 ms in NYR and from
  30.4 ms to 16.8 ms in C; both retain the 73,712-solution result.
- Benchmark fixture execution now defaults to one worker so concurrent timed
  fixtures cannot distort per-row runtime ratios. `--jobs N` and
  `NYTRIX_BENCH_JOBS` still opt into bounded parallel runs. A full warm
  65-fixture run (three samples, one warm-up) passed all fixture budgets and
  geomean gates; `binary` measured NYR/LLVM at 4.64x and `dict-set` at 1.04x.
- Benchmark summaries now render NYR consistently in console and Markdown
  output, and use one ratio formatter across fixture rows, hotspot callouts,
  geomeans, and gates. Ratios below 0.01x display as `<0.01x` rather than the
  misleading `0.00x`; the one-fixture string smoke showed aligned columns and
  preserved advisory status for a partial run.

- Native string concatenation now recognizes an immutable local initialized
  directly from a string literal, allowing looped `result = result + suffix`
  to lower to `rt_cstr_builder_repeat_known` instead of repeated append calls.
  Local shadowing is resolved before top-level literals, and the shadowing
  fixture now checks the exact repeated string as well as its length. The
  focused fixture passes cold. A full 66-fixture benchmark completed with all
  checksum and geomean gates passing; its append-scaling row measured 9.06 ms
  native, 9.00 ms LLVM, and 12.8 ms C.
- Benchmark fixture rows now stream when each fixture finishes, showing
  per-route timings and comparable ratios without worker-ID chatter. Two
  complete 66-fixture runs exited successfully; observed wall times were
  25.841 s and 12.431 s with warm execution caches. These runs verify output
  and warm throughput only; they do not establish cold `core.bc` build
  throughput, which remains open in `docs/TODO.md`.
- The focused `native-image-cache`, `std-source-cache-reuse`, and
  `core-bc-cache-selector` CTest regressions pass together (3/3). The
  native-image case checks warm reuse of
  relocation-bearing code with mutable data, source and optimization-key
  changes, truncated-cache recovery, explicit-assertion bypass, inline LLVM
  reuse, and the `fannkuch` checksum on cold and cached runs. Reuse remains
  specific to matching source/configuration keys; this does not establish
  cross-program native core reuse.
- Shared `core.bc` identity now uses the assembled core and compiler
  source/configuration fingerprints rather than the full compiler executable
  hash, so ordinary relinks can reuse the artifact. A Ny AOT checker validates
  the signature and exact source manifest before test runners select it.
  `NYTRIX_TEST_USE_STD_BC=1` opts focused tests and benchmarks into reuse;
  they skip the shared bitcode build by default.
- The test memory guard now budgets 3.4 GiB per worker, including 0.2 GiB of
  headroom over the observed ~3.2 GiB native-codegen peak, and retains an 8 GiB
  host reserve. A cold full-suite run exercised six workers without OOM
  (2,338 tests; 2,335 passed; 2,239.3 MiB peak fixture RSS). This confirms the
  guarded configuration is viable on the measured host; it is not a matched
  five-worker speedup measurement. The three target failures from that
  historical run were later cleared by focused cold target/replay runs.

### Test and standard-library build throughput

- Cold source-build profiling led to two compiler-path reductions: user
  function resolution now uses the existing exact-name hash buckets before
  falling back to the ordered scan, and NYIR type-map setup skips its
  union-find workspace when a function contains no typed float/vector values.
  With identical cold stdlib-emission flags, elapsed time fell from 195.99 s
  to 167.45 s (14.6%); emitted bitcode SHA-256 was identical
  (`84d0ebd0d30109df166d03809355c433c1ce9df1612f135ca8ae3da4d8d74175`).
  A current low-frequency CPU profile collected 8,138 samples with no lost
  samples; the remaining largest costs are call lowering and function
  resolution.
- A fresh cold-build profile collected 16,748 CPU samples with no lost samples;
  function resolution remained the dominant lowering cost. Expanded the
  target-aware tree-fallback cache from 4,096 entries/four probes to 16,384
  entries/16 probes, and indexed dynamic-list fact lookups. With identical
  cold stdlib-emission flags, per-function lowering fell from 122.742 s to
  114.012 s (7.1%), NYIR build time from 154.786 s to 143.074 s, and wall time
  from 168.376 s to 156.297 s. Both emitted bitcode files were 30,051,776
  bytes with SHA-256
  `84d0ebd0d30109df166d03809355c433c1ce9df1612f135ca8ae3da4d8d74175`.
  The current cold suite with the stdlib sweep passed 2,481/2,489 cases. Its
  eight failures were replayed against the saved pre-change compiler and
  reproduced there as well.
- Full benchmark validation passed all 65 fixtures with nine samples and
  three warm-ups, pinned to CPU 24 for a serial, affinity-controlled run.
  All repeated samples passed the existing noise check and checksum
  comparisons; `binary` measured NYR/LLVM at 4.86x, below its unchanged 5.00x
  fixture limit. No benchmark threshold or workload was changed.
- Fixed POSIX subprocess capture to merge child stderr into stdout, matching
  `core.os.subprocess.run_capture`'s contract and restoring generated
  negative-diagnostic assertions. The callback-alias differential passed all
  130 compiler invocations cold; changing its two workers to four reduced
  fixture time from 46.134 s to 26.424 s and 26.315 s in repeated runs.
- Generated ownership/refinement fixtures now invoke the compiler directly
  through argv instead of starting a shell for every generated case. The
  focused cold batch of eight generated differential fixtures plus the AVR
  float fixture passed 9/9 in 39.563 s with five workers. The legacy derive
  probe also passed separately cold (1/1, 10.527 s). These are focused
  correctness results, not a full-suite throughput comparison.
- Consolidated the AVR float fixture from 15 compiler/QEMU invocations to
  eight precision-specific checks, retaining its comparison, ABI, arithmetic,
  signed-conversion, widening, and narrowing assertions. Two focused cold runs
  passed in 24.338 s and 24.459 s; splitting arithmetic checks preserves AVR
  branch range and the two-second emulator window.
- Fixed arithmetic operator tails being lowered as heterogeneous positional
  lists, whose second slot was read incorrectly by the LLVM JIT route. Tails
  now use named `operator` and `operand` fields. A cold
  `arithmetic-parser-kit` run passed all four backend/runner variants, including
  shared `core.bc` and LLVM `mcjit`/`orc` routes.
- Added opt-in NYIR construction stage timings to the existing
  `NYTRIX_TRACE_NYIR_EMIT_TIMING` trace. A measured cold stdlib emission spent
  180.264 s in per-function lowering, 28.828 s on reachable-function analysis,
  and 2.310 s in dynamic-list prewarming; NYIR build totaled 211.853 s and LLVM
  emission 3.658 s. This instrumentation is disabled unless requested. It
  localizes the cost but does not yet demonstrate a compile-time improvement.
- The latest cold focused rerun of the six slow generated differential
  fixtures passed all six with 332.095 s aggregate fixture time. The callback
  alias fixture invokes the compiler 130 times serially; reducing that repeated
  work remains open because any batching must preserve each independent
  positive and negative assertion.

### Cross-target backends

- Wasm32 `extern "c"` imports now classify multi-field aggregates as memory
  class even when they occupy only eight bytes; Wasmtime fixtures check that
  small integer-pair and two-float-pair arguments cross the boundary as
  pointers. One-piece four-byte INTEGER and SSE aggregate arguments and returns
  map to `i32` and `f32`. Eight-byte INTEGER and SSE aggregate arguments map to
  `i64` and `f64`, with direct `i64`/`f64` returns. The portable-Wasm NYIR
  test passes; Wasmtime SSE/INTEGER argument/return and memory-class
  argument/return fixtures pass (3/3). Eight-byte multi-field returns also use the Wasm32 sret
  pointer and pass Wasmtime. Nonstandard multivalue return metadata is outside
  the C ABI support scope.
- The AVR floating-constant fixture now checks `f32`/`f64` stack arguments,
  exact tagged return bytes, all six comparison predicates, signed zero,
  unordered NaN comparisons, and infinity comparisons in two QEMU programs.
  Predicate checks use direct branch assertions rather than integer-weighted
  boolean sums, avoiding a generated 64-bit multiply loop in the test itself.
  The legacy derive expansion probe now checks record fields and generated
  declaration names instead of pinning the entire serialized AST hash. Both
  affected fixtures pass together in a cold focused run.
- AVR external C calls now marshal `f32` arguments to their register and stack
  locations, including after two raw `i64` register arguments, and capture
  `f32` return values. A QEMU fixture checks IEEE payloads and exact caller
  stack cleanup; unsupported `f64` arguments remain fail-closed. Existing
  3/4/9-argument stack calls still pass.
- ARM32 stack homes now keep i64 high words separate from adjacent 32-bit
  values. Its NYIR-verified QEMU smoke executes carry/borrow add/subtract,
  low-64 multiplication, bitwise operations, signed i64 comparisons, and
  variable left/arithmetic-right shifts at word-boundary and high counts,
  alongside full-width signed division/remainder, byte-swap, and AAPCS raw
  returns. ARM32 also materializes exact `f32`/`f64` constants into both
  words. Soft-float `f64` arguments follow AAPCS register-pair alignment and
  stack spilling; `f64` results preserve `r1:r0`. Soft-float `f32` arguments
  use their core-register or stack-word locations. NYIR-verified QEMU
  roundtrips check trailing `f64` and `f32` arguments after zero through eight
  integer arguments, including register-pair alignment and stack spill offsets.
  Mixed-order `f32`/integer/`f64` calls also pass exact-result QEMU checks. The
  expanded ARM runtime fixture passes cold under QEMU.
  Division and remainder use a restoring 64-bit path. QEMU checks signed
  results crossing the 32-bit boundary and the existing zero-divisor trap.
  ARM panic-spin labels
  now include their function identity, preventing duplicate symbols when one
  function contains multiple checked divisions. The focused ARM runtime fixture
  and the six D1 regressions from the reported replay pass cold (7/7).
  Removed the stale COFF64 aggregate-return rejection fixture: Win64 aggregate
  return and forwarding are already exercised by the passing
  `aggregate-composed-call.nshape` runtime test.
  The new AVR fail-closed matrix passes cold and verifies that representative
  unsupported square-root and transcendental operations fail without leaving
  partial assembly.
  AVR aggregate-parameter rejection now covers one-word, two-word,
  two-`f32`, and three-word layouts; its focused cold fixture passes all four
  cases.
  The new portable-AVR NYIR selftest enumerates all current vector and floating
  operations and verifies explicit rejection before partial assembly.
  The Wasm C ABI classifier emits no multi-register aggregate class; all
  direct scalar-class and memory-class aggregate import/return fixtures pass.
- AVR now lowers `f32` and `f64` addition, subtraction, multiplication, and
  division through the AVR libgcc soft-float helpers. Integer/float conversion
  and width-conversion helpers are included; the QEMU fixture links with
  `-mdouble=64` and exercises signed truncation, precision rounding, and both
  float widths. Unsupported square-root, transcendental, and vector operations
  remain explicitly fail-closed. The full focused AVR target fixture set
  passes cold (11/11); the three cross-target replay crashes reproduce neither
  serially (3/3).
- The x86-64 object encoder now homes colored values before every ABI call,
  including ordinary internal calls. Loop-phi residency is disabled only for
  loop regions containing calls, where the current home/residency contract
  cannot safely carry a value through the call boundary. A permanent allocator
  regression checks both loop-phi carry without calls and home-backed values
  with calls. The previous focused cold replay of the 31 reported D1 failures
  passed 34/34 variants. A later broad replay reported crashes in
  `gltf-index-modes`, `legacy-derive-expansion`, and `ffi-gen`; their serial
  cold rerun, together with the reported `f64-scalar-abi` and COFF aggregate
  fixtures, passed 5/5. A focused cold batch including those fixtures, the AVR
  scalar-call runtime, and both Wasm memory-class aggregate import fixtures
  passes 8/8. The crash signals did not reproduce serially.
- Current focused cold verification of the named AVR target fixtures, Wasm
  register-class and memory-class aggregate import/return fixtures, ARM/i386
  portable runtimes, reported probe/FFI regressions, and scalar ABI fixtures
  passes 20/20; the AVR NYIR unsupported-operation matrix passes independently.
- Focused cross-target regressions pass for the AVR, BPF, ARM, AArch64, i386,
  RISC-V64, MIPS64, and PowerPC64 paths (47/47 fixture variants), portable
  Wasm runtime and fail-closed import checks (6/6), and ELF32/ELF64 scalar ABI
  calls (50/50). Cross-target stack-bounds runtime passes, and the generic
  architecture fixture passes all seven target variants. The privileged BPF
  kernel load/test-run smoke returns 33.
- Portable `ror32.i64` now lowers on ARM with register-count `ror`, on i386
  with explicit zero-extension of the 32-bit result, and on RISC-V64 with
  operand and count masking. `bswap32.i64` now uses `rev` on ARM, `bswap` and
  explicit result zero-extension on i386, a portable shift/mask sequence on
  RISC-V64, and word rotate/mask instructions on PowerPC64. NYIR-verified
  QEMU smokes compare both operation results while checking the struct-copy
  payload on AVR, ARM, AArch64, i386, RISC-V64, MIPS64, and PowerPC64. The six
  D1 runtime/ABI/probe fixtures, including the AVR scalar-call
  runtime, pass cold (6/6).
  `bswap.i64` now has NYIR-verified QEMU checks on ARM32, i386, RISC-V64,
  MIPS64, PowerPC64, and AArch64. ARM32 constants initialize both 32-bit
  words; its raw i64 return path uses the AAPCS `r1:r0` register pair, checked
  against the full swapped value in QEMU.
  i386 `NYIR_CONST_I64`, `NYIR_LOAD_I64`, and `NYIR_STORE_I64` operate on both
  halves. All six `NYIR_CMP_I64` predicates compare signed high words and
  unsigned low words. `ADD`, `SUB`, `MUL`, `AND`, `OR`, `XOR`, variable
  `SHL`, and arithmetic `SAR` now operate on both halves; shift counts are
  masked to 0–63. i386 parameter entry sign-extends signed 32-bit `i64` inputs
  and local loads/stores preserve both words, so noinline callees receive the
  correct signed values. QEMU checks cover counts on either side of the 32-bit
  crossover, at 32, and at 63, plus full-width constants, memory operations,
  arithmetic carry/borrow and multiplication, comparisons, bitwise operations,
  signed boundary ordering, and raw i64 returns through the SysV `EDX:EAX` pair.
  `--ir-opt-test=portable_wasm` passes with the cross-target runtime checks.
- The portable WebAssembly backend now emits `env` imports for external
  scalar `i64`, Wasm32 pointer (`i32`), `f32`, and `f64` functions. Pointer
  arguments and results convert between the NYIR `i64` carrier and Wasm32
  `i32`; float arguments and results use their raw NYIR carriers. A Wasmtime
  preload fixture exercises these signatures, including a 64-argument import
  at the NYIR limit and a high-bit pointer round trip; an eight-argument
  internal call also executes. Wasm calls no longer inherit the host-register
  argument limit. Unsupported runtime helpers and unsupported aggregate
  signatures remain fail-closed; import validation rejects aggregate-return
  metadata instead of treating it as a void result. Wasm32 memory-class
  aggregate arguments now cross the import boundary as pointers, checked by a
  compiled, assembled, and
  executed provider fixture; register-class aggregate marshalling remains
  unsupported. WAT-emission negatives assert the unsupported runtime-helper
  diagnostic, and the focused portable-Wasm NYIR test rejects vector and
  unsupported aggregate argument/return import signatures. Its explicit bounds
  check is emitted and exercised for negative offsets and out-of-range limits.
  Wasm `select.i64` now uses the typed `select` instruction, and `ror32.i64`
  wraps to the 32-bit domain before `i32.rotr`; the native-only Wasmtime
  runtime fixture checks both select arms and a low-32-bit rotate. The verified
  NYIR contains both `select` and `ror32` in the exercised functions.
- The portable Wasm emitter now lowers `NYIR_COPY_STRUCT` through
  `memory.copy`. Both source and destination use the checked Wasm32 address
  conversion, and sizes above `INT32_MAX` fail emission instead of truncating.
  The focused Wasmtime NYIR smoke copies and reloads an eight-byte payload,
  confirms a high-bit pointer traps, and exercises oversized-size rejection.
- The ARM, i386, AArch64, RISC-V64, MIPS64, and PowerPC64 portable emitters
  lower `NYIR_COPY_STRUCT` to byte-copy loops using caller-saved temporaries.
  The focused backend NYIR smoke emits the load after this side-effecting
  operation and executes the copied-value check under each QEMU target. ARM and
  i386 reject sizes outside their 32-bit address spaces; all six native
  byte-loop emitters reject negative sizes. The existing MIPS64 and PowerPC64
  portable runtime fixtures still pass cold (2/2).
- The AVR portable emitter lowers `NYIR_COPY_STRUCT` to a bounded byte loop,
  rejects sizes outside its 16-bit address space, and traps if either pointer
  has nonzero upper bits. The backend NYIR smoke assembles and runs an
  eight-byte copy under QEMU and verifies oversized-size rejection.
- WebAssembly now lowers the classified `rt_malloc_i64` allocator helper to
  the module's linear-memory allocator, with checked i64-to-Wasm32 size bounds.
  Repeated zero-size allocations return distinct pointers. A cold NYIR-verified
  native-only Wasmtime fixture allocates memory, stores and reloads an i64,
  checks the tagged result, and confirms a negative size and a size above
  `UINT32_MAX` trap rather than wrapping or truncating; unsupported
  `rt_realloc` remains a diagnostic failure.
- The AArch64 portable-text backend now emits scalar `f32`/`f64` arithmetic,
  signed truncating `f32`/`f64`-to-`i64` conversions, `i64`-to-float and
  `f32`/`f64` width conversions, comparisons, and `f64` square root. Its QEMU
  runtime fixtures exercise float-to-integer conversion with noinline functions
  as well as the other scalar operations through runtime-dependent branches,
  including all ordered predicates and IEEE unordered/NaN comparisons. The
  portable text emitter also lowers variable `ror.i64`, exercised by a
  noinline QEMU runtime check at counts 1, 8, and 63, plus `ror32.i64` using
  the AArch64 W-register form and a constant-count QEMU runtime check. Float
  calls now marshal `f32`/`f64` arguments and results through the AAPCS64
  `s`/`d` registers, including mixed integer
  and floating argument banks. Scalar parameters and calls beyond the first
  eight registers per bank use the aligned AAPCS64 stack area; QEMU coverage
  includes mixed overflow banks, ninth-argument integer and floating calls,
  and 64-argument integer and floating calls. Floating power, sine, cosine,
  and remainder link to the production math runtime and execute; the
  no-runtime negative still rejects unresolved helpers. A fresh cold replay of
  `portable-runtime.nshape` and `f32-to-i64.nshape` passed 2/2.
- AVR portable text now has QEMU runtime checks for `bswap.i32` and `bswap.i64`,
  verifying byte order, zero-extension, and tagged return encoding. The
  scalar-call fixture checks a variable shift whose result spans the high
  32 bits, in addition to smaller shift boundaries, and executes a mutable
  branch merge carried through NYIR PHI values and all signed i64 comparison
  predicates at negative, equal, and positive operands. It also confirms
  unsupported dynamic panic formatting fails closed. Internal AVR scalar calls
  now pass arguments beyond the two register slots on the caller stack; a
  NYIR-verified QEMU fixture checks three-, four-, and nine-argument calls,
  including full-width and negative stack values, parameter order, and caller
  cleanup. External C calls now use the AVR stack convention for classified
  raw-i64 arguments; QEMU links and executes three- and four-argument C
  callees. AVR `f32` and `f64` constants now materialize their IEEE
  representations through the portable emitter; a NYIR-verified QEMU fixture
  checks the tagged result bytes for both widths. Internal `f32` and `f64`
  parameters pass through AVR's eight-byte scalar slots, with QEMU checks for
  fifth-argument stack loads. AVR `f32`/`f64` comparisons now implement all six
  predicates; noinline QEMU cases cover true/false results, signed-zero
  equality/inequality, and all unordered `f64` NaN outcomes. AVR lowers the
  canonical `rt_flt_nan` and `rt_flt_inf` helpers to quiet-NaN and
  positive-infinity constants, with QEMU comparison coverage for infinity.
  The cold comparison fixture passes. All six named D1 target regressions
  (`f64-scalar-abi`, COFF aggregate-return rejection, `gltf-index-modes`,
  `legacy-derive-expansion`, `ffi-gen`, and AVR `scalar-call-runtime`) pass in
  a six-job cold focused replay (6/6).
  All eight AVR target fixtures pass in the focused cold run (10/10 variants).
  External float stack arguments and aggregate
  parameters remain rejected. Focused AVR stack-argument fixtures pass 3/3
  cold, and the float-constant/fifth-argument fixture passes 1/1 cold. The
  scalar-call QEMU fixture passes 1/1 cold. The focused AVR,
  WebAssembly, and AArch64 target regressions pass cold (12/12). The privileged
  BPF kernel load/test-run smoke also passes in its container route (result 33);
  the focused BPF rotate and signed-operation fixtures pass cold (2/2).
  The AVR scalar-call fixture passes cold with the PHI and comparison cases;
  the focused WebAssembly runtime/rejection fixtures pass 5/5, and
  `--ir-opt-test=portable_wasm` passes.
- WebAssembly now supports C-ABI memory-class aggregate returns from `env`
  imports by passing the caller-allocated result buffer as a Wasm32 `i32`
  pointer and treating the imported call as `void`. The new Wasmtime regression
  compiles and assembles a 24-byte aggregate return, checks that the host
  receives a non-null pointer, and executes `rt_main`. Wasm runtime/helper and
  sret import fixtures pass cold (5/5); `--ir-opt-test=portable_wasm` passes.
- The metadata-aware x86-64 COFF bundle writer now places CodeView data before
  the symbol table, matching the symbol-table offset in the COFF header. A
  MinGW/Wine smoke links and executes a `ny-full`-generated Win64 object whose
  Ny function passes a C aggregate factory result by value to an imported C
  function and receives a by-value aggregate from the C host; the previous
  object was rejected by `ld` because its symbol table pointed into the
  CodeView filename string. Aggregate-returning Win64 functions remain
  fail-closed.
- Portable `ror32.i64` lowering now covers BPF, MIPS64, and PowerPC64 in
  addition to the AArch64 W-register form. MIPS64 and PowerPC64 QEMU runtime
  fixtures check the low-32-bit rotate result; the privileged BPF kernel
  load/test-run smoke checks the same result and returns 33. The corresponding
  NYIR-verification paths pass, including target assembly checks for BPF,
  MIPS64, and PowerPC64.

### Benchmark harness

- Benchmark fixtures now print one aligned, colored metric row as each
  completes, with layouts for narrow terminals. Warm execution caching is
  enabled by default; the runner still executes every timed sample and checks
  its checksum. `--bench-cache cold` retains repeated compilation timing.
- Report-only compiles for code-size and static metrics are optional during
  the ordinary console run. `--bench-code-metrics`, structured exports,
  counters, and static fixture budgets request the necessary report compiles.
- Geomean gates include all comparable fixture ratios. Sample instability is
  evaluated within each fixture; large gaps between different workloads are
  retained instead of being treated as scheduling outliers.
- Per-class geomean gate output sizes every column from its header and longest
  row value, keeping the startup and long-fixture rows aligned without
  truncating cells to a fixed width.
- Default compiler assertions no longer disable every JIT cache lookup.
  Explicit assertion requests still bypass compiled artifacts. Native x86-64
  images now retain unrelocated code and named relocations, which are resolved
  in the current process on warm execution. Cache writes are atomic and
  malformed images fall back to compilation.
- Inline LLVM execution promotes validated bitcode to a native image by
  default. Core content fingerprints are reused while source identity, size,
  permissions, and nanosecond modification/change times match; strict content
  hashing remains available with `NYTRIX_CACHE_STRICT_CONTENT=1`.
- AVX YMM SIB memory operands now encode the inverted base/index extension
  bits correctly. The previous swap could address the stack frame in place
  of a high index register and crash the vectorized `fannkuch` kernel.
- The `startup` LLVM/C geomean ceiling was recalibrated from an unreachable
  0.05x to 0.30x. The old value predated the current C>=1ms startup subset
  (`escape-loop`, whose C reference performs 1M malloc/free, plus `inline`);
  those fixtures stably geomean to ~0.17x LLVM/C, so the gate failed
  deterministically whenever `escape-loop`'s C time crossed the 1ms inclusion
  threshold. 0.30x restores the documented 1.6x-baseline relationship.

### Performance

- Typed-buffer length queries now read validated headers directly when the
  epoch-checked handle cache proves allocator provenance. Other representations
  retain the existing resolver. Seven interleaved runs of identically compiled
  heapsort executables reduced median NYR time from 98.330 ms to 15.220 ms and
  LLVM time from 85.664 ms to 4.032 ms, with unchanged checksums. Focused tests
  cover cached lengths, growth, aliases, scalar widths, and pointer ABI paths.
- Mapping-range registration checks its most recent deduplication entry before
  scanning the remaining entries. An allocation-heavy bigint executable
  retained its checksum while reducing retired instructions from 440.42 M to
  430.70 M; eleven interleaved runs measured medians of 130.53 ms before and
  127.00 ms after. The larger bigint arithmetic gap remains unresolved.
- Cold full-stdlib bytecode emission now caches the package-surface module list
  using the same module/block traversal and declaration order as the prior
  lookup. Two cache-disabled, opposite-order comparisons emitted byte-identical
  bitcode and reduced wall time from 254.894 s to 236.276 s and from 261.473 s
  to 228.422 s (7.3% and 12.6%). NYIR function lowering fell by 8.0% and
  15.5%; LLVM emission remained 3.6–3.7 s.
- The direct-mapped enum-member lookup cache grew from 4,096 to 16,384 slots
  after a cold full-stdlib compile measured 81,680 recursive scans. The larger
  cache reduced scans to 72,446 and raised its hit rate from 77.2% to 79.8%.
  An opposite-order cold bytecode pair produced identical output and reduced
  external wall time from 233.08 s to 226.45 s (2.8%); NYIR build time fell
  from 218.984 s to 212.457 s. A no-loss profile after the change measured enum
  tree scanning at 7.13% self samples; top-level variable and extern-declaration
  scans are the next measured lookup costs. Cold core/full-suite throughput
  remains open in TODO.
- The shared program-query cache grew from 8,192 to 32,768 slots after a
  cache-traced full-stdlib compile recorded 98,147 misses and 89,955 evictions.
  The larger cache recorded 66,211 misses and 33,550 evictions; NYIR build time
  fell from 222.358 s to 199.495 s and external wall time from 236.27 s to
  213.26 s. Both cold builds emitted byte-identical bitcode. The cold import
  fixture passes 10/10 with the larger cache.
- Native compilation now builds a per-emission symbol-to-section index for LLVM
  origin metadata and a sorted source-path index for module import resolution.
  Reachability discovery also reuses source-aware function resolutions and
  avoids repeating a qualified function lookup when import-alias resolution
  fails. These indexes are scoped to the compilation and retain the existing
  lookup order and fallback behavior.
- Re-exported constant lookup now memoizes positive and negative module/member
  resolutions for the current compilation snapshot, avoiding repeated walks
  through the same import graph. The AArch64 encoder initializes instruction
  offsets with one allocation and fill instead of zeroing them before
  overwriting every entry with the invalid-offset sentinel. Its floating-point
  allocator also skips the floating-point preflight for integer-only functions.
  Peak-live calculation now keeps small-function difference counters on the
  stack and reserves heap allocation for larger machine functions.
- Typed-list indexed stores now use the thread-local handle cache when its
  registry generation is current and the canonical tbuf header is valid. Cache
  misses, forwarded handles, malformed headers, and sanitizer-poisoned headers
  continue through the checked runtime path.
- Native string-builder lowering folds loops with a statically known inclusive
  range and immutable literal suffix into one reserve and a bulk repeated-copy
  operation. Dynamic bounds and suffixes keep the general loop lowering.
- Native list assignment now recognizes when a returned handle came from the
  canonical stdlib `append` intrinsic, even though semantic resolution records
  the stdlib function as a user function. It reuses that local's companion
  length, which the append lowering updates on both fast and growth paths,
  instead of emitting `rt_len` after the join PHI.

- The x64 object encoder now omits a literal's canonical home only when every
  machine use is an explicitly supported immediate right operand. Data reads,
  call arguments, returns, and cross-block uses keep their homes.

- Extended exact SMT translation validation to the two-lane integer broadcast
  and reduction operations. Both CFG path exploration and straightline
  evaluation now model packed lanes as bit-vectors and reduction as wrapping
  i64 addition. `tv_smt_semantics` proves equivalence to scalar addition and
  rejects a changed lane in both encodings; the bounded interpreter fallback
  also agrees on the CFG case.

- Added LLVM ORC JIT perf-map entries for materialized defined functions. The
  ORC object-transform layer now captures object symbol sizes, which are
  paired with loaded LLJIT addresses; symbols without size metadata are
  omitted rather than assigned guessed ranges.

- Bounds-check elimination now reuses one CFG analysis for dominating-guard
  and fresh-buffer proofs. Because BCE rewrites only non-terminators, its block
  and dominator maps remain valid for the full scan, avoiding repeated analysis
  construction for checked accesses.

- E-graph dominated-reuse lookup now indexes earlier pure scalar expressions by
  opcode and operands before checking CFG dominance. This avoids comparing each
  definition with unrelated prior instructions while retaining instruction
  order and the same first-dominating-definition rule.

- Indexed register-allocation copy hints by destination virtual register and
  source live segments by virtual register/start position. Color selection now
  binary-searches candidate copies and source segments instead of rescanning
  every instruction in each segment and linearly searching all segments. Copy
  affinity remains advisory; the existing conflict/spill rules are unchanged.

- Register allocation now indexes fixed-register clobber sites once per
  register color and advances monotone cursors across the index as live
  segments are visited. This removes instruction-by-instruction scans across
  long live ranges and avoids a binary search for each segment. Common
  register sets use a 64-bit occupancy mask instead of allocating and clearing
  per-color arrays. The existing monolithic and multi-block backend scaling
  cases exercise both paths.

- Phi pruning now builds CFG topology only. Its predecessor and block-membership
  queries do not need dominators or dominance frontiers, so generated join
  graphs avoid computing those unused analyses during dead-code elimination.

- NYIR phi-edge validation, CFG structure checks, and ownership-flow checks now
  request topology-only CFG data. They inspect block ranges, reachability, and
  predecessor edges; dominance analysis remains in the verifier paths that
  check value ordering and dominance.

- Dead-code elimination skips CFG construction for straight-line functions
  without PHIs, branches, labels, or instructions after a return. Its
  reachability check scans each instruction once, including functions with
  repeated returns.

- Parallel work dispatch now applies its minimum grain size per active worker,
  avoiding machine-wide thread teams for workloads that are small per task.

- If-conversion builds a sorted CFG label index once per pass, replacing
  repeated full-block scans for branch and join labels while retaining the
  existing dominance and natural-loop checks.

- Native `ctpop.i64` lowering now uses the typed popcount runtime bridge, so
  host builds can emit the target's population-count instruction instead of
  expanding the operation into a SWAR sequence.

- Hardened the rectangular loop-interchange legality gate against induction
  values escaping through non-induction exit PHIs or expanded call arguments.
  The transform now swaps only the loop guards' constant operands, preserving
  shared constant definitions and their range metadata.

- Generic `@effects(e)` callback aliases now retain outer-local updates across
  block scopes and join `if` branches, `match` arms, `while` bodies, and
  C-style `for` bodies by union. A match without `else` and loops include the
  incoming row as a possible path. Exception and deferred callback
  reassignment remain fail-closed.

- Qualified module function values passed through local callback aliases now
  preserve their pure or IO row when invoked through a generic `@effects(e)`
  helper.

- A named module re-export of an IO callback retains its IO row through a
  local alias and generic callback invocation.

- Returned lambdas that capture scalar factory parameters now get a known row
  from their body summary.

- A direct returned-lambda factory now defers its callback argument row to the
  returned closure and substitutes known captured `fnptr` arguments into the
  lambda summary.

- Callback rows now survive capture through immutable dictionary literals and
  `get("key")` lookup, and through immutable list literals with constant-index
  reads. Facts are copied through immutable local container aliases.

- Wired the 10k+ instruction monolithic and multi-block native backend scaling
  driver into the regular POSIX `./make test` path. It now checks machine
  lowering, register allocation, and object emission separately instead of
  leaving the stress guard as an uninvoked source file. Its CFG case now
  covers 2,500 blocks and at least 10,000 machine instructions. These
  measurements validate the existing budgets; they are not a runtime speedup
  claim.

- Fixed list length queries for registered native typed buffers. `rt_len`
  previously tested the C-string heuristic before the typed-buffer header, so
  a list beginning with zero appeared empty despite valid indexed elements.

- The `float-cmp-oracle` O3 native hang was fixed: after a fixed-register
  operation loads an operand, native lowering now spills any live values that
  operand loading re-seeded in the operation's scratch registers before
  clobbering them.

- Fixed a live-range corruption in x64 fixed-register arithmetic. Loading a
  dividend can re-seed its assigned physical register after the initial
  clobber flush; the quotient/remainder lowering now commits those newly
  resident values to their canonical homes before clobbering scratch
  registers. Reciprocal remainder remains enabled.

- Reduced register-allocation loop-edge discovery from a full machine-block
  scan per loop header to scanning that header's already-built predecessor
  list. The predecessor list preserves layout order; duplicate branch edges
  are deduplicated and the existing 256-edge limit and eligibility checks are
  unchanged.

- The overflow result is pinned to `INT64_MIN`; bounded recursion specializes,
  while unbounded recursive, effectful, and malformed calls remain out of
  line. This adds evaluator-boundary coverage, not general recursion
  specialization or effect-summary support.

- Closed the straightline SMT translation-validation gap for float operations:
  `nyir_smt_eval_straightline` now models f64/f32 arithmetic, sqrt, sin/cos
  (shared uninterpreted bit-level functions), f64/f32 comparisons, and all
  int↔float conversions with exactly the encodings the CFG path already uses,
  so straightline i64-returning rewrites with float intermediates
  (scaling-then-truncate idioms) reach the solver instead of the bounded
  interpreter. The straightline path also gains the same returned-float
  NaN-payload guard the CFG path uses.

- Removed a duplicated nested `ok && !nyir_pass_stopped` guard around the O3
  SSA loop-unroll pass. It did not change runtime behavior; it obscured the
  pass pipeline and made the guard structure misleading.

- CFG translation validation now models `NYIR_SIN_F64` and `NYIR_COS_F64` as
  shared uninterpreted functions over complete input/output bit patterns. This
  soundly admits congruence proofs without inventing transcendental identities
  or constraining NaN payloads.

- Native inliner diagnostics now distinguish a cost-model-approved candidate
  from a clone that was actually applied, including inserted instruction count
  and result value. This confirms the remaining gap is downstream loop
  specialization/vectorization and machine code quality, not a missing inline.

- Centralized invalidation of NYIR range and alias facts after rewrites that
  change an instruction's operation or operands. Pipelined-loop clones, rotate
  idiom recognition, parity comparison rewriting, fast-math reciprocal
  factoring, and scalar-to-SIMD rewrites now share this helper. Reciprocal,
  rotate, parity, and vector-reduction contracts seed stale range and alias
  metadata and confirm both are cleared; strict IEEE mode remains unchanged.

- Expanded optimizer pass-order validation from 120 orderings over three
  arithmetic-only shapes to 120 orderings over six shapes, adding alias-aware
  memory forwarding, mixed f64/i64 conversion, and loop-carried CFG cases.

- Enabled `NYIR_SQRT_F64` in the pure-IR TV eligibility gate, matching its
  existing exact SMT encoding instead of silently treating sqrt-containing
  CFGs as out of scope.

- CFG SMT translation validation now models f32/f64-to-i64 with truncation
  toward zero and tracks the valid finite signed-i64 input range separately
  from the solver's total bit-vector result.

- Native scalar-list append now carries its proven post-append length through
  the fast/slow result merge into the mutated local's companion slot.
  Reassigning `a = append(a, value)` reuses that count instead of calling
  `rt_len` on every loop iteration. Indexed-setter length validation remains.

### Language

- Added a strict-types negative case for an `any` binding named `std` that
  shadows the imported module; unresolved dynamic member access remains an
  error under lexical shadowing.

### Performance and sound

- Safe-mode raw-memory validation no longer treats `assert_compile_range` as
  an unchecked assumption. It verifies exact ordered bounds and proves the
  current value range is contained before using the assertion as a raw-index
  fact; unknown or false claims fail closed.

- Added a shared pre-backend check for `assert_compile_range` inside `while`
  loops. It derives an interval only from an exact start, exact `<`/`<=`
  bound, positive constant step, a single direct index write, and a
  side-effect-safe body; list-literal `.len` bounds are recognized. False
  claims and unprovable loop starts now fail before backend selection. A
  direct positive `xs.len` loop prints `60` on both routes.

- Fixed an unsound x86-64 scalar-emitter RAX-forwarding case where a
  single-use producer was followed by a coalesced same-home COPY that emits no
  machine instruction. The forwarder had treated the COPY as consuming the
  transient `%rax` value, skipped the producer's canonical home store, then a
  later consumer reloaded an uninitialized slot. It now retains the store for
  that exact no-op-copy shape.

- Fixed an O2/O3 dynamic-index miscompile where synthetic ICP profile steps
  triggered block layout despite zero per-block heat, causing a meaningless
  CFG reorder. `nyir_block_layout` now leaves zero-heat functions unchanged.

- Fixed three x86-64 f64 oracle crashes: the F64 indirect-load emitter ignored
  a forwarded address held in `%rax` and reloaded an uninitialized stack home.
  It now uses the shared cached-address helper.

- Re-enabled profile-guided NYIR block layout behind its existing CFG ordering
  checks and candidate verifier.

- The x64 encoder no longer rescans scalar register homes after a block
  terminator, and its pre-branch materialization skips flags-fused compare
  results that have no stack consumer.

- Refined GVN's memory barriers so observable clock/IO effects and fresh
  allocation do not automatically invalidate proven stable reads. Added exact
  runtime summaries for `rt_monotonic_ns_raw` and `rt_len_strict`, including
  the latter's read, allocation-on-error, and trap effects, plus optimizer
  checks that retain calls while forwarding a stable symbol read across them.
  Allocation still blocks forwarding through unknown pointer roots, whose
  heap-object lifetime is not established.

- Bounds-check elimination now removes the canonical negative-index
  normalization select when every store to a non-address-taken source local is
  a nonnegative constant, SCEV proves the value's interval is nonnegative, or
  a dominating signed-comparison edge proves nonnegativity at the select. The
  edge proof requires the guard target block to have that branch as its sole
  predecessor, preventing an alternate path from bypassing the comparison. In
  `cmov-sort`, four normalization selects disappear from the hot sort path.

- GVN-PRE now checks memory stability across loop re-entry before reusing a
  dominating load.

- Software pipelining now clears inherited range and alias facts from each
  cloned stage-A instruction after its operands are remapped. In particular,
  the look-ahead clone may evaluate at `iv + 1` after the original loop's last
  iteration, outside the source expression's proven range.

- SLP now checks the immediately following store when searching for adjacent
  store groups; the prior index increment skipped that candidate. Packed
  vector memory and arithmetic operations retain a representative source
  location.

- Reduced x86-64 carried-register lookup work by searching each virtual
  register's contiguous allocation segment range rather than scanning the
  whole function's segments.

- Expanded the compile-only backend stress guard from a single 10k-instruction
  block to also cover 10.3k machine instructions across 128 control-flow
  blocks.

- Extended verifier-backed SSA full unrolling from unit strides to exact
  positive strides for loops with known trip counts up to 16.

- Fixed native `remove`/`delete` lowering for dynamic collection receivers:
  native lowering now specializes only when the receiver is proven to be a set
  or dictionary, and otherwise uses a checked runtime layout dispatch. The
  helper preserves receiver aliasing, handles native and managed dictionaries,
  and records its memory effects and borrow contract.

- Fixed the sound-source compiler RSS blowup in `_poly_blep` by declaring its
  private float-only inputs as `f64`, avoiding dynamic-value arithmetic.
  Native lowering also trusts a concrete NYIR float producer over a stale
  BigInt AST classification when selecting a comparison ABI. Added opt-in
  statement-level resource checkpoints, restricted by
  `NY_TRACE_NATIVE_BUILD_FILTER`, to localize future pathological function
  lowering without flooding normal builds.

### Diagnostics

- The runtime ownership ABI now explicitly marks OS/resource release arguments
  as consuming for close, socket, websocket, directory, dynamic library,
  kqueue, canvas, mutex, and Windows change-notification helpers;
  `rt_zfree_raw` also consumes its allocation pointer. Raw memory copy,
  compare, fill, and file read/write bridges now mark pointer operands as
  borrowed while leaving scalar fd/length operands unknown; raw file open and
  file-exists borrow their path. C-string concatenation, equality, length,
  replacement, conversion, and indexed-access helpers borrow their input
  pointers. Other explicit borrows cover runtime string formatting/printing,
  bytes/range/typed-buffer reads, dictionary lookups, canvas text, and
  in-place string/list helpers. Result-aliasing getters declare
  `returns_borrow` for both the receiver and a fallback that may be returned;
  byte/canvas mutations do so for their receiver. Canvas text borrow metadata
  is recorded at formal argument 3, with intervening scalar arguments left
  unknown. `rt_any_to_cstr`, `rt_to_str`, and `rt_cstr_to_str` also mark their
  input as `returns_borrow` because already- string inputs pass through
  unchanged. Raw pointer arithmetic, memory loads/stores, and copy/fill
  helpers now record borrowed addresses and returned aliases; store helpers
  that return their value preserve that borrow relation. List item reads now
  declare a conditional result alias to the list; item stores borrow the
  receiver and declare the returned stored value as an alias of argument 2.
  Direct BigInt ABI inputs now declare borrows for arithmetic, comparison,
  formatting, conversion, bit count, and number-theory helpers;
  `rt_bigint_submul` records the conditional returned alias of its first
  argument.

- Extended the runtime argument ownership table to complex arithmetic and
  accessors, membership and sequence length, generic indexed lookup, value
  inspection, vector reads, and vector fallback results. Clear/clone/set
  conversion now declare conditional receiver aliases.

- Dynamic `rt_any_*` conversions, comparison, length, and arithmetic now
  declare borrowed inputs. Result constructors borrow payloads, while unwrap
  helpers distinguish a possible payload/fallback alias from raw scalar
  fallback arguments.

- Set insertion/removal helpers now declare their mutable receiver as a
  possible returned alias and their keys as borrowed; generic dynamic addition
  records its in-place set receiver alias.

- Public FFI adapters now reject captured Ny closures as well as marked Ny
  function pointers before C dispatch.

- Added opt-in `NY_TRACE_NATIVE_BUILD` checkpoints for function parameter
  setup, body lowering, and finalization. The hook is disabled by default and
  was used to replace an incomplete per-function memory diagnosis with
  whole-harness evidence.

- NYIR calls now keep actual argument ownership separate from formal
  `borrows`, `consumes`, and `returns_borrow` dispositions. Native lowering
  records the resolved source-function disposition on each logical argument's
  first ABI slot using two bits per argument; a returned borrow includes the
  borrow relation. Binary v18 serializes it, and verification, semantic
  fingerprints, and CSE keys preserve/check it. Runtime value ABI declarations
  now populate formal borrow metadata for the flat native dictionary bridges:
  borrowed inputs are marked `borrows`, and an aliased result marks its
  borrowed receiver as `returns_borrow`. Other synthetic/runtime call
  constructors remain unknown pending explicit contracts. The canonical
  runtime argument ABI now declares borrowed formals for audited dictionary
  and typed-buffer helpers, and source calls plus compiler-generated 3-, 5-,
  and argv-call constructors propagate them. A verbose `list.len` compile
  shows `rt_tbuf_len_raw`'s borrow contract in NYIR before and after
  optimization; a dictionary lookup trace shows borrowed receiver/key slots on
  `rt_dict_get_raw` after optimization. String-builder calls now declare
  append as a borrowed receiver result with a borrowed input, conversion as a
  receiver borrow, and free as consuming its builder. The native C-string
  self-concatenation builder now records borrowed initial text, borrowed
  append suffix, returned-borrow receiver, and consuming finalize contracts.
  Managed and raw memory free helpers now consume their pointer argument;
  `rt_init_str` returns a borrow of its pointer. RC retain/count helpers now
  borrow their pointer, release/drop consume it, and drop-owned-slot borrows
  the slot address while consuming its stored value. A full source search
  audited all eight NYIR CALL constructors: ordinary source calls and shared
  runtime call emitters propagate declarations; zero-argument generated
  entry/print calls have no argument lanes; the optimizer-generated memset and
  prefetch calls retain unknown ownership on raw-address operands because no
  source ownership fact is available.

- RC runtime calls now have explicit optimizer effect summaries: retain reads
  and writes the synchronized RC registry and may allocate; count reads the
  registry; release/drop read and write ownership state and may trap while
  freeing; drop-owned-slot also writes its addressed slot.

- CFG translation validation now lowers f32/f64 arithmetic, comparisons,
  square root, integer-to-float conversions, and f32/f64 width conversions
  through Z3's IEEE floating-point theory. Branch-sensitive f64 comparison and
  f32/f64 arithmetic counterexamples verify the path-sensitive model. Returned
  float bit patterns and float-to-integer conversions remain on the
  conservative bounded evaluator path because NaN payloads and invalid integer
  conversions need explicit semantics.

- GVN now refreshes a same-key candidate when its prior definition fails the
  dominance or memory-stability proof. Later loads can be checked against the
  newer definition instead of repeatedly missing against a stale one. GVN now
  consumes summarized read-only user-call effects for commoning, blocks reuse
  across a writing call, and includes call result/effect contracts in the
  value key.

- GVN now tracks explicit pointer provenance through COPY and pointer
  arithmetic when proving whether a direct local-slot store can invalidate a
  memory read. A store to a local slot no longer blocks reuse of a read
  through a proven symbol/allocation address; local-address reads retain
  exact-slot barriers, and unknown/load-derived pointers remain conservative.
  `gvn_pre` covers a proven non-local read with unrelated local writes and
  address-taken local reads.

- GVN's opt-in `NY_TRACE_GVN_MISSES=1` diagnostics now include canonical
  byte-load addresses and value-number lookup results, alongside per-load
  misses. This makes it possible to distinguish a mismatched expression key
  from a later dominance or memory-stability rejection. The compact `GVN_PRE`
  summary remains available in verbose traces; filtered trace-mode checks
  print identical program output with or without the diagnostic override.

  The larger JSON parser trace has been refreshed after correcting a lowerer
  validation regression; see the following entry.

- Fixed missing braces in native lowerer argument guards. Valid `__env_get`,
  `argv`, environment-count, float-memory, tty, callback, panic, and defaulted
  call paths had returned failure unconditionally after the validation `if`.
  The fresh O3 trace again shows 41 GVN replacements, 6 dominance misses, and
  4 memory-proof misses; repeated byte loads remain an open optimization task.

- DSE's allocation escape analysis now includes expanded CALL stack arguments,
  so an allocation passed after the six inline argument slots keeps its
  stores. A malformed nonempty argument vector conservatively keeps all
  allocation stores.

- Redundant-load elimination now uses the `COPY_STRUCT` destination's alias
  class to invalidate only potentially overwritten cached addresses. Its
  source operand remains read-only; unknown destinations flush conservatively.
  It also keeps a load's address class separate from the loaded value's
  provenance: a pointer loaded from memory remains unknown for subsequent
  alias decisions unless an explicit value fact is available. The other direct
  aggregate-copy consumers were audited: DSE records source reads, ADCE
  preserves the operation, loop reordering rejects it, and CFG simplification
  treats it as observable.

- Alias escape analysis now follows function-local pointer provenance through
  `COPY`, `ADD_I64`, and `SUB_I64` before checking call arguments. A local
  address passed directly or through those derivations to any inline or
  expanded call argument is conservatively escaped. Provenance is propagated
  in one reverse SSA scan. Returned local pointers and pointer values stored
  into memory are escape roots as well. `alias_contracts.c` covers direct,
  copied, and offset-derived pointers at inline/expanded call arguments,
  returns, and memory stores; alias, redundant-load, DSE, and effects drivers
  pass.

- Central NYIR rewrite helpers now clear derived alias classes when replacing
  an instruction with `COPY` or an integer/float constant, while preserving
  semantic class, ownership, mutability, source location, and applicable range
  facts. Previously a folded pointer-derived scalar could retain the pointer's
  nonzero alias class and mislead later memory passes.

- Private-entry-global promotion now invalidates address alias classes when
  rewriting global loads/stores to local-slot operations. Its removed
  `ADDR_SYMBOL` becomes a fully reset `NOP` while retaining only the source
  location.

- SROA's pointer-to-local promotion clears address-derived alias metadata when
  rewriting memory loads/stores to local-slot operations. Removed instructions
  use the full NYIR discard helper so side metadata is reset.

- Redundant-load forwarding now rebuilds `LOAD_LOCAL` as a `COPY` through the
  shared rewrite helper and assigns only the source value's proven provenance.
  Previously it retained the old slot-access alias class, which could make a
  scalarized pointer value look like the local slot.

- GVN-PRE duplicate-load forwarding now uses the shared COPY rewrite contract
  and carries the dominating value's alias provenance.

- `alias_contracts` seeds those facts and verifies the replacement constant;
  `alias_contracts`, `gvn_pre`, and `pass_order` pass.

- Counted-loop idiom reduction now clears alias metadata when replacing
  `MUL_I64` with `SHL_I64`, matching the alias contract that only COPY and
  pointer ADD/SUB propagate address provenance.

- E-graph extraction now rebuilds constants through the shared rewrite helper
  and clears alias metadata on other rewritten expressions pending fresh alias
  analysis.

- Peephole rotate recognition now clears alias provenance when replacing
  scalar shift/OR expressions with `ROR32_I64` or `ROR_I64`.

- Loop vectorization now clears scalar alias provenance when it widens an
  arithmetic result or COPY into vector values, including reduction updates
  rewritten as vector reductions.

- Common-divisor floating-point factoring is now gated by explicit fast-math:
  reciprocal multiplication can change strict IEEE rounding and exceptional
  behavior. Rewritten divisions also clear stale alias metadata and refresh
  opcode effects. `peephole_reciprocal_contract` checks strict-mode retention,
  fast-math factoring, verifier validity, and alias invalidation.

- The shared f32 constant rewrite now retains source debug location as well as
  scalar representation and ownership metadata, and explicitly clears every
  unused operand.

- PHI elimination now resets alias metadata when replacing a value-producing
  PHI with a local-slot load, and preserves its range, semantic ownership,
  mutability, and debug location.

- Strength reduction now preserves scalar class, semantic ownership,
  mutability, range, and source location on the replacement result while
  clearing the original operation's alias provenance. This applies to
  multiply/add-to-shift rewrites and signed/nonnegative divide or modulo
  rewrites.

- Constant folding and correlated value propagation now route constant
  rewrites through the shared constructors, so alias classes are invalidated
  and semantic/range facts are preserved consistently. The CVP regression also
  exposed a lost-fact bug: unioning a COPY destination with its source changed
  the representative before reading the source's affine constant. CVP now
  snapshots that fact before union and propagates it to the merged root.
  `alias_contracts` verifies the constant result, cleared alias class, and
  retained metadata; `pass_order` still passes.

- Corrected the packed call-contract setter to use thirty-two two-bit entries
  per word. The old `/16` word index misplaced arguments 16–31 and wrote
  beyond the contract array for arguments 32–63. `effects.c` round-trips all
  64 slots. The native `rt_tag` helper is now classified as a pure scalar
  call, and ADCE drops unused effect-free calls while retaining unknown and
  trapping calls. This removes a dead `rt_tag` call from each Fibonacci
  recurrence iteration; the focused result is noisy and no stable speedup is
  claimed yet.

- The NYIR runtime-call effect table now marks `rt_native_is_int` as a memory
  reader because its representation checks inspect heap registries and
  candidate object headers.

- Raw memory builtins and their NYIR runtime helpers now report memory reads,
  writes, and possible traps accurately. GVN-PRE reuses equal raw loads before
  a write and invalidates them after it; the effects and GVN-PRE C drivers
  cover those contracts.

- Its value numbering follows COPY chains, including copies of PHIs, when
  matching equivalent operands. The focused `json-parser` measurement did not
  improve, so this is a correctness and missed-CSE fix without a claimed
  runtime gain.

- Every pass boundary is verified and evaluated, then repeated until the
  semantic fingerprint reaches a fixed point.

- Native register-allocation loop-edge coalescing now resolves candidate
  segments through the existing block index instead of rescanning each vreg's
  full segment list.

- `NY_DUMP_REGALLOC=1` now reports segment live-in/live-out state and named
  spill reasons; `NY_DUMP_REGALLOC_CONFLICTS=1` shows loop-edge color
  evictions and the retained segment.

- The x86-64 scalar machine emitter now handles immediate integer divisors
  directly instead of reading a nonexistent virtual-register home. This
  removed a wrong-result/SIGFPE case in optimized control flow. Signed
  power-of-two division uses sign bias plus `sar`; remainder reconstructs `x -
  trunc(x / d) * d`, preserving the dividend's sign.

- NYIR translation validation skips solver work when a pass leaves the
  semantic IR fingerprint unchanged. The CFG SMT evaluator also simplifies
  expressions and prunes branches whose conditions become constant.

- The induction-variable eliminator now folds an even constant-trip reduction
  that selects between two constants using `(iv + invariant) & 1`, when the IV
  step is odd. Each adjacent pair contributes both select arms, independent of
  starting parity, so the rewrite uses wrapping i64 arithmetic and keeps
  odd-trip, even-step, and dynamic-bound loops unchanged.

- The rotate peephole now matches shift operands through intervening NYIR
  copies using same-value analysis, so SHA `rotr` expressions become `ROR32`
  instead of remaining two shifts and an OR.

- The range narrowing pass now accepts proven `uint32` intermediates and
  recognizes exact `& 0xFFFFFFFF` boundaries. It propagates low-word codegen
  through add/sub/mul/bitwise expression chains only when every use stays in
  the low-word region, leaving values with a wide-use escape unchanged. The
  `optimizer/narrow.c` driver guards unsigned boundaries, overflow rejection,
  masked unknown values, and escaping uses. This is a small result, not a
  parity claim; both remain far slower than C.

- Peephole now removes canonical zero/one normalization around boolean
  comparisons. It reduces `cmp != 0`, boolean `& 1`, and comparisons against
  zero/one back to the original or inverse i64 predicate, while leaving float
  complements that need unordered predicates untouched. This removes redundant
  compare/and instructions in the `json-parser` digit loop.

- Kept redundant-load forwarding block-local after an O3 `heapsort` regression
  showed that ignoring CFG backedges can reuse a load across loop iterations
  where a PHI changes the address.

- The x86-64 scalar machine-form gate now accepts immediate operands for
  `SHL`, `SAR`, `ROR`, and `ROR32`; the encoder materializes the masked count
  in `RCX` before the variable-count instruction. Immediate shifts had been
  incorrectly rejected, sending integer kernels to the older NYIR emitter.
  `NY_DUMP_MACH=1` now reports the first ineligible machine instruction when
  that fallback occurs.

- The x86-64 scalar machine-form gate accepts `i64 = ptr + i64` for typed
  buffer byte-address calculation; the emitted operation is ordinary 64-bit
  addition. Without this case, runtime-guard and affine typed-buffer vector
  loops were rejected by the machine backend and then failed in the NYIR
  fallback.

- The native i64 if-converter now follows up to four empty forward branch
  trampolines on diamond edges and accepts arms with up to eight safe scalar
  operations, including a fallthrough arm. A proven raw-i64 truthy condition
  may become an explicit `!= 0` comparison before SELECT, but only when the
  two arms together contain at most two instructions; larger speculation is
  rejected after the `loop-unswitch` kernel measured slower with both arms
  evaluated. Other conditions still require an existing boolean proof. The
  existing last-write loop reduction continues to reduce the benchmark's 100
  iterations to one, while retaining its predictable branch under this cost
  gate.

- Native JIT compilation now appends generated function address/size/name
  records to the Linux perf map when `--jit-perf-map` or profiler mode is
  enabled.

- `NYTRIX_JIT_CODE_DUMP=1` now saves the exact relocated JIT image and a
  sidecar mapping function addresses, sizes, image offsets, and names. A
  scaled `binary` perf capture used it to map the hottest sample to the
  signed-division sign-mask temporary. Image I/O stays opt-in and best-effort.

- `NY_DUMP_FN` also prints selected functions' final NYIR after cross-function
  inlining, PHI elimination, and scalar-class reconciliation, immediately
  before machine lowering. This distinguished a correct solver key operand
  from the backend's stale register/home write; normal output is unchanged.

- Mem2reg now rejects nested-join shapes with unproven downstream parity
  before cloning or inserting labels/zeros, preserving the input fingerprint
  exactly and tracing the downstream parity reason.

- Requested allocation counters are emitted once on explicit cleanup or
  process exit.

- Native-only compilation now honors `--emit-asm` before returning from its
  early route, including `-emit-only`, execution, and cross-target diagnostic
  requests. Assembly write failures return a nonzero status.

- `NY_DUMP_FN=rt_main` now prints actual synthesized-entry NYIR snapshots
  around inlining and after the final post-inline optimizer; the leaf-wrapper
  snapshot is named explicitly. `--nyr-dump` remains the optimizer-IR
  diagnostic view.

- The `loop-unswitch` benchmark's C reference no longer uses `volatile` for a
  local value whose intermediate writes are unobservable in Nytrix. Added the
  O2/O3 `loop_last_overwrite` NYIR pass: for a pure, constant-trip loop with
  one canonical induction variable and carried outputs overwritten solely from
  that IV and invariants, it seeds the IV at the proven final iteration and
  executes the observable body once. It rejects unknown/zero trips,
  reductions, trapping/effectful bodies, unsupported exits, and
  unrepresentable final IVs.

- The list benchmark's C reference now performs geometric dynamic growth with
  `realloc`, matching Nytrix's repeated `append` rather than preallocating the
  final array.

### Correctness

- The bigint classifier no longer treats every `any`-returning call as a
  BigInt handle.

- Bigint comparison is register-class coherent. The compare bridge now detects
  a float producer on either side and resolves the contradiction as a
  machine-f64 comparison — decoding tagged dynamics, widening f32, or
  promoting raw ints as needed — mirroring the arithmetic bridge's existing
  rule.

- The NYIR runtime-call effect table now describes `rt_cstr_to_str` as a
  checked read of caller-provided string bytes plus allocation. It no longer
  falls through to `CALL|FFI|UNKNOWN_SIDE_EFFECT`; allocation, memory-read,
  and trap effects remain explicit so optimizers keep their existing safety
  barriers.

- Dynamic-arithmetic producer classes are now coherent with float slot and
  argument contracts across the native backend. When operator lowering kept a
  dynamic operand in the canonical tagged domain (the `rt_any_mul` bridge and
  siblings), four boundaries previously consumed the tagged handle word as a
  number: the binary f64 promotion SIToFP-converted it, the dynamic-bridge
  boxing and the untyped-return box took IEEE bits of the handle, and the call
  funnel's f64-argument promotion treated it as a raw int. Each boundary now
  decodes via `rt_any_to_f64` when the register class says TAGGED, so `mut s =
  list.get(0, 0.0) * sin(1.0)` and `s +=...` produce identical, correct
  results on the tree-walking VM, the NYIR VM, and native x86-64 (previously
  the three backends disagreed by up to 1%).

- A violated `requires` contract now fails closed on the native path with the
  real `E2001` diagnostic instead of corrupt NYIR. The obligation failure
  returned `ny_native_nir_fail`'s bool result, and `false` doubles as NYIR
  value id 0, so the caller lowered `to_str(<violation>)` into a
  self-referential call (`call rt_i64_to_cstr_raw v0`) that the verifier
  rejected with an unrelated message. The site now propagates the `-1` failure
  sentinel.

- The NYIR VM no longer double-invokes unmarked callables at callback arities
  5-15. The VM's callback switch executed the resolved target for arities 5-15
  and then fell through to the generic dlsym path, which invoked
  `rt_call_anyN` a second time and overwrote the result — doubling side
  effects and keeping the second return.

- Intrinsic policy is declaration-driven on the active native path.

- The numeric and callback bridges are reconciled across backends. The LLVM
  NYIR emitter implements `NYIR_F64_TO_I64` via `FPToSI` (previously any NIR
  containing the opcode failed emission while the VM and machine paths
  executed it), and the JIT binds `rt_call_any3..15` alongside any1/any2. The
  complex family now has complete ABI rows (arithmetic, predicates, and
  `rt_complex_new`'s boxed-flt tagged arguments), `complex` classifies as the
  object-handle representation it is in the shared semantic layer, and the
  `__complex_real`/`__complex_imag` aliases document why the raw-f64 mapping
  matches the stdlib's declared `f64` returns. The `.long` receiver
  classification is one shared helper (the memcall copy was missing the bigint
  exclusion the member copy had), the stale "tagged small int" contract
  comment is corrected, and the two undeclared simmd helpers carry ABI rows
  matching their C signatures.

- The native backend now derives its wide-int, extended-float, alias,
  nullable, list, and call-boundary ABI decisions from the shared semantic
  fact layer instead of native type-name fallbacks. `ny_type_is_bigint` is the
  shared wide-int predicate (both `bigint` spellings, nullable-stripped,
  qualified-leaf aware) consumed by the bigint classifier, param/return slot
  decisions, and call-boundary boxing. The native float predicates consult
  `ny_semantic_rep_for_type` before the spelling check, so `number` and
  `numeric` annotations own the machine-f64 ABI everywhere instead of falling
  to the raw-int path. Function results, callback raw-int markers, lambda
  callback ABI, and the direct-call result class prefer a resolved and
  coherent `return_semantic` over the declared name, with coherence guarding
  stale facts; i128/u128 keep their non-raw-i64 exclusion.
  `ny_native_nir_primitive_layout` requires a container boundary character
  after `list`/`dict`/`set` prefixes so user types whose names merely start
  with those letters (for example `listx`) no longer get pointer-sized
  primitive layouts. Lambda and unresolved-call semantic stamps derive from
  `ny_semantic_rep_for_type`, so aggregate, pointer, and typed-buffer results
  stop being stamped raw-int.

- Machine-form home assignment no longer treats a PHI edge destination as a
  pure SSA alias. After `mach_lower_phi_edges` a phi result carries one COPY
  per incoming edge, so it is multi-defined and each edge aliases a different
  producer. The cross-block copy-chain walk now stops at multi-definition
  vregs, which keep private homes, and constant rematerialization refuses
  multi-def values. The plain-NIR x86-64 emitter applies the same rule to its
  COPY chain homes and const fusion.

- Native x86-64 `SELECT` emission now preserves its predicate while
  materializing the false arm. The old order tested the condition before
  loading that arm; zero rematerialization can use `xor`, which overwrites
  flags and selected the wrong value. The encoder now keeps the predicate in
  reserved R10 and tests it immediately before `cmovne`.

- Verbose heuristic output reports candidate trip count, rejection reason, or
  the applied final IV.

- `loop_last_overwrite` now validates every header-PHI preheader/backedge pair
  before its first edit. A rejected candidate therefore leaves its IR and
  fingerprint untouched; the former late rejection branch could only occur
  after partially rewriting the CFG and also referenced freed analysis
  buffers. DCE removes the loop test and induction PHI after the
  single-iteration rewrite; the live TODO tracks the remaining native/LLVM gap
  and general invariant-branch cloning.

- Runtime-bound induction-variable reduction rewrites now mark generated
  integer division/remainder operations with their required trap effects. This
  lets the verifier accept the closed-form formula after inlining instead of
  retaining the hot loop. Historical focused measurements remain as earlier
  snapshots, not the current acceptance result.

- Integer division and remainder now report `MAY_TRAP`; ADCE keeps these
  operations even when their results are dead.

- The effects, ADCE, CSE, GVN-PRE, and redundant-load drivers pass.

- Alias analysis now assigns address classes to all scalar and vector
  single-address load/store opcodes, including vec4/vec8 i64 accesses.
  `optimizer/alias_contracts.c` guards same/different global roots, unknown
  addresses, each memory opcode, all tested call positions, and reanalysis
  after rewriting a pointer producer to a scalar. Reanalysis clears the prior
  generation before assigning fresh facts.

- Unknown facts remain conservative. `pass_order` now includes ADCE in its
  reordered cleanup and alias/memory pipelines.

- NYIR CALL now carries packed per-argument ownership facts. Ordinary native
  calls record resolved source ownership; binary format v17 preserves it, the
  semantic fingerprint and CSE key include it, and verification rejects
  conflicts with known value producers while accepting unknown producers.

- Formal borrow contracts from canonical runtime callback and FFI ABI
  declarations now survive all three native call-construction paths, including
  calls whose expanded argument list uses NYIR's extra-argument storage.

- Runtime calls that return the same logical container now mark the receiver
  formal as `returns_borrow`: typed-buffer append, clear, set, and swap, and
  dictionary clear and delete.

- Verbose NYIR dumps now print known per-argument ABI classes and formal
  ownership dispositions beside each call, making expanded call contracts
  inspectable in the same compact instruction row.

- A missing-chain guard in attached-method suffix lookup prevents native
  `--nyr-dump=stats` diagnostics from following a
  zero-initialized function index as if it were a built snapshot.

- Debug function filters now preserve nested context push/pop even when a
  context line is filtered, and native raw O3/pass-stat dumps honor
  function-only selection. An impossible function filter emitted no compiler
  trace records.

- This establishes a repeated-read opportunity while leaving the bounds and
  memory-stability proof requirements explicit; no runtime gain is claimed.

- Native local inference now lets a concrete f32/f64 producer ABI clear stale
  list-shape metadata and its companion length slot before storing the result.
  This handles stale inferred list types even when the local's current
  `is_list` classification is already false, preventing a float call result
  from receiving a typed-buffer length query.

- Native call-effect lookup now uses a sorted symbol index instead of scanning
  every reachable function for each call. The use-alias result cache grows
  instead of silently missing recursive-resolution results after its fixed
  probe window fills.

- Native reachability now collects the concrete receiver-owner method as well
  as a general `any` candidate, so typed `set.add` calls include the wrapper
  body emitted by native lowering.

- General NYIR inlining now recomputes caller-local alias classes after a
  successful splice. Callee alias IDs are scoped to their original function
  and could otherwise collide with caller allocations, misleading later memory
  optimizations.

- Common-subexpression elimination now reuses calls only when the IR marks
  their effect summary trusted and the summary contains only local reads,
  control, traps, and the call itself. Memory reads, mutation, allocation,
  I/O, FFI, threading, and unknown effects remain excluded; writes clear the
  block-local table. CSE rewrites preserve result scalar ABI, ownership,
  mutability, alias, source location, and range facts.
- Within a basic block, CSE also resolves representation-preserving raw-i64
  copies when their scalar class, ownership, mutability, and alias metadata
  match. Tagged, pointer, aggregate, and contract-changing copies remain
  distinct.
- On the O3 binary-trees fixture, this merged the two identical recursive
  calls into one while preserving the 4057 checksum. A serial 3+1 run measured
  NYR 218 us versus its 237 us baseline; the sample ranges did not overlap.

- COPY/PHI producer consistency is now covered separately; stores, loads, and
  calls still need explicit ownership contracts.

- NYIR verification now rejects a known ownership annotation on a COPY or PHI
  when it contradicts the known producer/incoming ownership. Unknown ownership
  remains a valid conservative result.

- x86-64 immediate rematerialization now respects fixed-register clobber
  boundaries in both arithmetic and shift-immediate selection. Also bounded
  two standalone encoder-test relocation arrays: those fixtures have no
  external relocations, and the production maximum had overflowed their
  stacks.

- Explicit native f64/f32 local declarations now reassert their storage
  carrier after expression-semantic joins and immediately before the store.
  This preserves the widened f64 ABI for f32-returning calls and the declared
  f32 ABI symmetrically.

- The nested-join mem2reg gate remains enabled pending dedicated downstream
  SSA coverage.

- Native indexed stores now honor an explicitly declared top-level `list<int>`
  element ABI even when its initializer is the generic `list(size)` allocator.
  This keeps writes at 8-byte integer stride and prevents later
  descriptor-slot promotion from corrupting direct typed-list accesses in
  `sift_down`.

- Native vector arithmetic dispatch now checks the lowered operand class
  before calling vector helpers. This prevents scalar integer multiplication
  in loop bounds (`d * d <= n`) from becoming a vector dot product and hanging
  the prime benchmark.

- Member `.get` lowering preserves the tagged receiver ABI for dynamic
  producers, including nested sequence indices, and selects the checked
  accessor rather than assuming a dictionary pointer. This fixes the
  `_has_header` receiver-class verification failure and wrong nested-list
  lookups.

- The previously recorded typed-int marker failure was an invalid ABI
  combination: `tagged_args` deliberately supplies tagged values, while typed
  `int` parameters consume raw integers. Captured native closure
  identity/environment, typed closure scalar returns, parallel-module JIT
  startup, and resolved Result payload representation now have cross-route
  parity regressions (2026-10-01).

- Explicit raw-integer callback results are boxed by contract, without
  interpreting their bits as pointer, nil, or boolean encodings. Typed
  closures previously returned `21` instead of `42` after this ambiguity
  skipped boxing and the caller decoded again. Canonical `any` list callbacks
  in the marker fixture now use the tagged-result marker, retaining their
  structural list assertions rather than claiming an integer result ABI.

- Global dynamic loads preserve their tagged storage class instead of
  advertising raw integers to comparison lowering. Dynamic equality also tags
  proven raw integer peers, including unary negatives and computed values,
  before calling the runtime comparator.

- Native block-entry register residency now follows the same eligibility rule
  as operand reads. Values read from canonical stack homes are no longer
  falsely marked resident by a carried segment; a subsequent call flush
  previously replaced a solver string key with the unrelated tag `121`.

- The x86-64 machine encoder reserves its r8-r11 scratch registers from
  allocation and uses only callee-saved GPR colors in functions with calls.
  Win64 leaf functions also exclude rsi/rdi until their nonvolatile saves are
  represented. Register metrics use the same ABI-specific pool as emission.

- Mem2reg now captures incoming parameters before branch-local stores and PHI
  placement. An untouched predecessor previously supplied zero when the first
  parameter load occurred at a join.

- Replaced mem2reg's reverse-postorder reaching-definition table with an
  iterative dominator-tree traversal that restores value stacks on subtree
  exit and fills predecessor-specific PHI operands.

- Removed its stale loop-carried register-allocation TODO.

- Native IV elimination follows the CFG's real loop-exit edge through
  branch-only glue and preserves invariant local bounds when replacing a loop.
  Runtime affine reductions now keep the induction step separate from the
  accumulator's scale when computing trip counts.

- Runtime affine reductions with an even accumulator step absorb the division
  by two into the constant coefficient, preserving modulo-2^64 arithmetic.

- Typed closure parameters no longer double-untag through dynamic dispatch;
  typed local list annotations, single-element list initializers, and
  recursive `mapcat` preserve element values across print, loops, and
  f-strings.

- Match range arms untag dynamic subjects before the ordered compare; bitwise
  operators no longer divert into the c-string path on stale string
  representations; list-repeat counts unbox dynamic values.

- Boxed-bool truthiness normalizes between raw 0/1 and canonical boxed words
  in call arguments, list literals, and `&&` operands (Rule 110 now renders
  bit-exact).

- Integer-returning `case`/match expressions decode a conservative dynamic
  join once at the function ABI; Result-pattern bindings keep the tagged
  payload ABI; try/catch joins normalize scalar arm results.

- String slices preserve the full requested range (ASCII and UTF-8, including
  open bounds); X11 UTF-8 decoding accepts the native string ABI and advances
  by encoded bytes.

- BigInt boundary fixes preserve identity, comparison, negation, and
  absolute-value handles: `Z(4).sqrt_mod(17).powmod(2, 17) == Z(4)`, large
  import/re-export compilation completes, and `.unhex`/`.long` round-trips are
  restored.

- Native x64 constant folding now follows constants through uniquely-defined,
  type-preserving machine copies in any block. The earlier int32 C version
  vectorized a narrower computation. The sparse O(vregs) block-use summary
  preserves the old cross-block home-safety query without repeated CFG scans.
  OpenMP worker teams are capped at each region's task count, avoiding idle
  workers on short regions.

- The standard-library workaround inventory is closed. `core.common`'s
  functions are genuine cross-module utilities (not compiler workarounds):
  every one is used by dozens of files, and none exists to paper over a
  compiler deficiency. `core`'s polymorphic `len`/`get`/`set`/
  `contains`/`append` functions are the language's dynamic-dispatch surface,
  not re-exports. The remaining categories (dynamic-callback markers in async,
  simmd pointer helpers, camera contract, env scans) were all resolved or
  shown to be load-bearing in prior rounds.

- Register allocation derives block intervals from values touched or live in
  that block, avoiding full virtual-register scans for every block, and the
  machine encoder uses the active block for instruction lookup. The inline
  callee registry sizes to the actual function set. Constant signed modulo
  uses the magic-divisor path, with live values in its fixed R8-R10 scratch
  registers synchronized before the sequence.

- The general inliner leaves directly recursive callees out of line. A
  separate bounded scalar evaluator folds verified direct self-recursion with
  a depth limit of 32 and shared 100k-instruction budget, declining effectful
  bodies, non-self calls, and work beyond budget.

- Redundant-load elimination now clears local forwarding facts at call
  boundaries.

- Unified SROA now ends local store/load forwarding at every call boundary.
  Escape analysis proves whether an address is retained, not whether a call
  temporarily writes through it; retaining a cached local definition across
  that boundary crashed optimized native `fannkuch`.

- CSE no longer pools constants by erasing instructions while iterating stale
  CFG ranges, and it no longer merges calls without argument-location mod/ref
  summaries. Constant pooling remains disabled until it can rewrite in place
  while retaining semantic metadata.

- Native NYIR preserves the tagged ABI when an untyped lambda returns a
  dynamic ternary: raw branches are boxed once and the joined value keeps its
  class through the implicit return. Concrete local declarations override
  stale dynamic expression facts when selecting arithmetic.

- Index lowering now carries a post-proof representation refinement into the
  NYIR semantic stamp only when the emitted SSA scalar class matches it. This
  keeps append-built typed integer reads from retaining their earlier tagged
  source stamp without propagating unrelated call-context refinements.

- A typed `Result<f64, int>` regression and `unwrap(os.file_read(path))` both
  keep their resolved payload carriers: string and handle words are stamped
  with their semantic class instead of being boxed as integers.

- Explicit `int` locals for `old_cap` and `new_cap` in
  `core.dict_mod._dict_resize` keep capacity arithmetic raw, and the
  dict-clone list-result classification no longer overrides the resolved
  dictionary type.

### Native ABI and callbacks

- The untyped scalar ABI is driven by callee semantics end to end: raw
  producers box exactly once at `any` boundaries, raw consumers decode at
  their store boundary, raw-result bridges (`rt_len`, `rt_cstr_len`, builder
  handles) stop classifying as `any`, and spawn/index/descriptor paths keep
  loop counters, integer literals, and callback pointers in their canonical
  words.

- `rt_call_any3`/`rt_call_any4` trampolines carry three- and four-argument
  dynamic callbacks; indirect dispatch selects all-raw shapes for
  statically-known callees.

- Extern/runtime bridge declarations carry per-argument f64 ABI facts; f64
  runtime-bridge declarations are no longer retyped to i64 by later bit-typed
  call sites.

- Dictionary fixes: native `.len` reads the native table count first,
  string-key reads stop double-decoding integer fallbacks, and top-level
  mutable bindings stay in the entry frame (10,000-key construction works).

- Dynamic callable IDs are treated as closure-like values in descriptor
  buffers: returned callables work through direct `fnptr` calls, `any`
  formals, and list store/read; callable arguments keep the callable's
  recorded ABI at `fnptr` calls and skip scalar unboxing for closure formals;
  integer return boundaries follow the emitted tagged scalar class when the
  callee surface is qualified.

- High-arity indirect calls share the dynamic argument preparation used by
  lower arities, so raw integer words no longer enter tagged callback formals.
  The NYIR VM resolves unmarked captured closures before they reach the C
  callback trampoline, and reads bool, tagged-argument, and raw-result ABI
  flags from the closure metadata registry, matching native routes.

- The x64 object emitter stages integer call operands before placing them in
  ABI registers. Previously an early argument move could overwrite a later
  argument's source register, corrupting four- and six-argument callbacks,
  ordinary typed calls, and chained self-return calls.

- The raw SIMD scalar runtime bridges now declare their raw integer result
  class as well as their no-boxing argument contract. Without the result
  class, leading-zero counts returned raw 63 but NYIR treated that word as a
  tagged integer and decoded it as 31.

- File open retry loops keep their descriptor and counters in explicit raw
  integer locals across backedges. The cold file roundtrip preserves odd byte
  counts and binary payloads on NYIR, MCJIT, and ORC.

- Native-only lowering now preserves proven f64 results from sine/cosine and
  f64 arithmetic, plus the raw integer result from float-to-int conversion.

- Legacy LLVM intrinsic shortcuts (sqrt, sin, cos, abs, `get`, f64
  conversions) now require the registered `core.math`/`core` module identity
  or resolved return types instead of recognizing leaf names, so user modules
  owning those spellings keep their own implementations.

### Compiler performance

- IRCE now uses its affine induction-variable interval analysis to remove
  fixed-bound checks when every natural-loop byte offset is nonnegative and
  strictly below the exclusive limit.

- Root lowering now keeps indexed reads from a mutable homogeneous
  integer-list literal in the raw i64 ABI when the consumer does not request
  `any`. The previous path read an odd raw payload such as `5`, then called
  `rt_any_to_i64` and changed it to `2`, which made native insertion-style
  loops skip comparisons. This narrow fix does not close the broader dynamic
  list specialization gap.

- The AVR floating-point runtime fixture combines its constant/stack-argument
  checks and groups comparison predicates into 11 QEMU executions instead of
  38, while retaining the checked payloads and comparison cases. Its focused
  cold run passed in 14.121 s with shared bytecode; the earlier focused run
  took 59.9 s.

- The legacy derive-expansion probe now passes the test runner's shared
  `core.bc` to its nested compiler invocation, keeping expansion on the same
  standard-library bytecode as the parent test process.

- Focused `make test` runs now reuse an existing `core.bc` automatically.
  They still avoid building one solely for a focused run when the artifact is
  absent; `NYTRIX_TEST_USE_STD_BC=1` continues to force that build.
  A cold allowance-flow fixture timed out at 180 s on the source fallback and
  passed in 78.964 s with shared bytecode (peak fixture RSS 842.7 MiB).
  A cold seven-fixture slow-flow selection passed all cases in 127.9 s wall
  time with 873.3 MiB peak fixture RSS.

- The `core.math.big` multiplication, division, and modulo wrappers now keep a
  tagged integer operand immediate instead of allocating a temporary BigInt;
  the runtime's existing small-limb arithmetic accepts that representation.
  Addition and subtraction now preserve tagged integer operands too, and
  `pidigits` passes its scalar factors directly. The spread is large and the
  native/C gap remains substantial.
- Focused, checksum-passing `bigint-oracle` and `pidigits` runs confirm the
  scalar-operand specialization remains effective: NYR measured 41.5 ms versus
  LLVM 39.8 ms on `bigint-oracle`, and 339 µs versus 470 µs on `pidigits`
  (3 runs plus one warm-up each). The remaining NYR/C gap is shared with LLVM
  on `bigint-oracle`; the measured native path is not its current bottleneck.
- Cached x86 BMI2/ADX detection after its first query, avoiding repeated CPUID
  instructions in BigInt multiplication and division. The checksum-matched
  `bigint-oracle` run measured 35.3 ms NYR versus the supplied 38.9 ms
  checkpoint; the full benchmark gate passed.
- Signed power-of-two division by two now uses the shared strength-reduction
  path. A checksum-identical five-run native A/B improved the `binary` fixture
  from 8.48 ms to 6.70 ms; keep this general reduction and focus remaining
  typed-loop work on the separate right-shift case.

- Explicit `./make bench --jobs N` values remain exact.

- GVN load reuse no longer treats direct local-slot assignments as arbitrary
  memory writes when the function has no `ADDR_LOCAL` instruction. Functions
  that materialize a local address retain the conservative barrier because it
  may alias a subsequent memory read.

- GVN now records the canonical source as soon as it rewrites an instruction
  into a COPY, instead of waiting for a later scan to encounter that COPY.

- GVN-PRE value keys now preserve scalar-class metadata for every opcode.
  Previously, equal immediate bits with raw and tagged representations could
  share a value number; a replacement COPY then violated a downstream call's
  argument ABI. Previously, later expressions in the same pass could not see
  the new alias; identical address arithmetic therefore prevented a safe
  repeated-load match.

- GVN can now number `LOAD_LOCAL` results and reuse a dominating read of the
  same slot across CFG edges. A write to that slot, a call, or a possibly
  aliasing write to escaped local storage blocks reuse; writes to other local
  slots do not.

- `set_idx` on a top-level integer list proven to remain an elem-8 tbuf now
  emits a guarded direct store when the normalized index is within the live
  length. Negative indexing shares the runtime's wrap rule; null, capacity
  growth, and out-of-range writes still call the runtime setter.

- Native statement lowering now recognizes a bounded overwrite-loop pattern: a
  positive constant-trip `<` loop with unit induction and a pure two-arm
  assignment to one independent scalar can evaluate its final iteration once.
  The guard requires a straight-line dominating constant initializer, raw-i64
  operands, and a trip count no larger than 1024; zero-trip and other shapes
  keep their normal CFG.

- Native lowering now uses the existing exact-size multi-fill path for
  inferred empty lists when whole-program append analysis proves an integer
  element representation. Its zero-length proof recognizes the generated
  `rt_tbuf_new_raw(0, 8)`/`rt_tbuf_len_raw` pair; the `iter` optimized NYIR
  replaces its repeated append helpers with sized buffers and direct stores.

- SysV register allocation now uses caller-saved `rsi`/`rdi` for GPR segments
  only when the segment is local, has no spill/reload boundary, and belongs to
  a block without calls. Linear and loop-edge coalescing enforce the same
  color limit, so later coalescing cannot carry those registers across a call
  or block boundary. This suggests a modest native gain, but the LLVM and C
  variation means it is not a parity claim.

- Machine lowering now shares canonical homes for same-type cross-block SSA
  COPY chains as well as coloring block-local intervals.

- The `binary` benchmark's C reference now uses explicit unsigned wrapping for
  the recurrence and bit-preserving signed division, matching Nytrix's i64
  semantics without C signed-overflow undefined behavior.

- Native machine lowering now colors stack homes for virtual registers whose
  definitions and uses are confined to one basic block, and coalesces dying
  same-type copies only when source and destination share a home safely.
  Cross-block and loop-carried values keep private homes. This reduces frame
  footprint.

- The scalar x86 text emitter now lowers signed division and remainder by a
  compile-time non-power-of-two constant with the same exact magic-reciprocal
  sequence as the NYIR and object emitters. It resolves literal divisors
  through COPY chains and keeps runtime `idiv` for unknown or exceptional
  divisors.

- Added a compile-scoped cache for native user-function name lookup. Exact
  string keys preserve the recursive resolver as a fallback for long names or
  allocation failure.

- Native enum-member lookup now memoizes positive and negative results by
  interned query name for the compile-scoped expanded program. The original
  source-order tree walk remains the fallback if cache allocation or name
  interning fails. `NYTRIX_TRACE_LOOKUPS=1` reports cache effectiveness.


- x86-64 machine lowering now recognizes the verified signed power-of-two
  division sequence and fuses it to the existing immediate-divisor machine
  form. The encoder spills only its RAX/RCX scratch colors for this sequence,
  preserving unrelated loop-carried registers. Linked counters fell from
  31.04M to 29.61M cycles and 79.00M to 76.12M instructions; generated code
  shrank 48 bytes, with 28 fewer static spill segments and 9 fewer reload
  segments.

- A compile-scoped negative import-alias prefilter indexes explicit aliases,
  plain-use leaves, and module leaves as a conservative superset. Definite
  non-alias names skip local-shadow lookup and full use-tree traversal; actual
  alias resolution retains the existing source and module precedence. The
  opt-in `NYTRIX_TRACE_IMPORTS=1` summary measured 407,709 early rejects and
  reduced full scans from 21,857 to 1,106 on `vterm-key-state-open`. This
  generated- code variation remains under investigation.

- LICM moves signed division and remainder only when value/SCEV ranges prove
  zero-divisor and `INT64_MIN / -1` traps impossible. The fallback for
  incomplete call-alias summaries is now scoped to each loop that combines
  calls with raw memory, so unrelated calls after a loop do not suppress its
  LICM opportunities. Such mixed loops remain conservative. The `120000 / d`
  condition itself remains in the inner loop because the compiler must
  preserve its explicit zero-divisor panic path; the earlier claim that this
  division was hoisted was inaccurate.

- Native COFF, Mach-O, and single-function ELF object writers now allocate
  relocation symbol/name tables in proportion to actual relocations. This
  removes multi-megabyte fixed stack frames (the ELF path reserved roughly 68
  MiB) and avoids zeroing the global relocation limit for small objects.

- Fixed 24 hanging `core.os.ui` modules with operator-resolution and re-export
  memos, a global top-level-value index, O(1) module lookups, and cross-module
  constant-alias folding that replaces dangling raw-storage symbols.

- Small String Optimization plus SIMD SwissTable probing for native
  dictionaries; constant-key record literals fold reads, `.get`, `.has`, and
  `.len` to fields without hash-table traversal.

- Proven 8-byte typed-buffer reads stay on raw loads through the SHA/ PBKDF2
  schedule paths; raw-length list finalization stops decoding odd lengths as
  tagged.

- Counted-loop unrolling now locates a candidate header across its guard's
  loads and comparison instead of requiring the label immediately before the
  branch. PHI legality is checked in the candidate loop, so unrelated
  post-loop PHIs no longer disable this transform for the whole function.
  `loop_unroll.c` verifies that case unrolls with unchanged evaluation and
  that a loop-header PHI remains untouched pending edge-aware cloning.

### Language, proofs, and effects

- Audited all `static int` functions in native NYIR lowering sources for
  direct `return ny_native_nir_fail(...)` paths. These returned boolean
  `false` as value ID 0, which is valid NYIR; all 218 such paths now preserve
  the diagnostic and return the `-1` failure sentinel. Boolean lowerers retain
  their boolean failure contract.

- Native call lowering now fails closed when a `requires` obligation is false
  or unknown. It can fold a direct immutable module literal into the
  obligation; mutable values, computed globals, and unconstrained parameters
  stay unknown until shared solver discharge is available.

- `@total` now triggers call-graph recursion analysis and is validated after
  that graph is known. Recursive claims fail closed, including claims with
  `decreases(...)`, because that syntax annotation is not a well-foundedness
  proof.

- `@allowance` grade-aware ownership tracks parameter use over `ℕ ∪ {omega}`
  with structured diagnostics.

- `comptime{}` is enforced as closed code: capturing outer runtime `mut` state
  is a structured diagnostic.

- Intentionally unknown CPS/query and captured-return factory callbacks remain
  rejected rather than guessed.

### REPL

- Native NYIR lowering now marks `ticks()`'s `rt_monotonic_ns_raw` result as
  raw `i64`, matching the helper ABI before mixed integer/`f64` arithmetic or
  dynamic-value conversion. A REPL regression checks nanoseconds-to-seconds
  multiplication and confirms a fractional `f64` prints as a float.

- `core.str`'s `_env_present` no longer walks the process environment
  manually: it resolves the entry with the native `__env_get` builtin in one
  call, fixing prefix-colliding name lookups (e.g. `NY_H` matching `NY_HOME`)
  in the string-layer debug gate.

- Interactive input no longer fails with `undefined symbol 'repl_show'`: the
  echo helper is emitted into the snippet itself and uses only
  runtime-provided conversion builtins, assignments are not wrapped as display
  expressions (a wrapped assignment parsed the left side as a named argument),
  and the wrapper is skipped for non-value inputs.

### Compiler infrastructure

- `rt_fail(kind, msg)` now decodes both native C strings and short tagged
  Nytrix strings before composing its panic message; it also terminates the
  bounded output buffer explicitly. The previous heap-only conversion dropped
  short strings and emitted `PanicError::`. Indexed-store bounds errors and
  invalid integer `get`/`contains` receivers now route through `err.index` and
  `err.type`; no runtime source calls `rt_panic` directly outside `rt_fail`.

- The nested-join SSA promotion gate is now runnable per invocation via
  `NYTRIX_NESTED_MEM2REG=1` (previously a rebuild-only switch). The
  `set-effect-contracts` native-only row is now fixed by collecting its
  concrete attached-method wrapper; see the compiler infrastructure closure
  above.

### Diagnostics and tooling

- The `-vvvvv`/`-vvvvvv` introspection flood is compacted at its biggest
  sources.

- C5 normalized-failure infrastructure: `rt_fail(kind, msg)` joins the runtime
  API, building the panic message as `"<kind>: <message>"` using the
  lib/error.ny ERR_* kind vocabulary. Division-by-zero, modulo-by-zero, all
  index-out-of-range sites, and typed-buffer-handle failures now route through
  it. The machine-form bounds-check trap block calls `rt_bounds_trap` instead
  of raising bare SIGILL.

- Native register allocation no longer splits every long live range at
  arbitrary 16-instruction intervals or rescans each interval for fixed
  register clobbers. A 10,000-instruction, single-block stress driver keeps
  5,000 values live across the reduction and enforces a 1.5-second allocator
  budget.

- Large straight-line machine blocks no longer enter the post-RA scheduler's
  dense O(n²) dependency builder. The scheduler skips only safe regions over
  512 instructions, leaving their original valid order intact; normal sized
  regions retain latency scheduling.

- CFG translation validation now models `SELECT_I64` directly in both the
  straight-line and path-sensitive Z3 evaluators. Previously this valid pure
  operation forced CFG proofs into 2,048 interpreter trials; a 516-sample CPU
  profile attributed 96.7% of runtime to that fallback.

- Z3 translation evaluation now models `ROR_I64` and `ROR32_I64` with the same
  6-bit/5-bit count masks as the NYIR VM and x86 hardware. `SHL_I64` and
  `SAR_I64` likewise mask counts to six bits instead of using Z3's
  out-of-range shift result. `tv_smt_semantics.c` proves each unmasked form
  equivalent to its explicit masked-count form over all bit-vector inputs.

- Z3 translation validation now tracks whether signed `DIV_I64` and `MOD_I64`
  are defined, including zero divisors and `INT64_MIN / -1`. SMT bit-vector
  arithmetic defines results for those cases while NYIR traps; equivalence now
  requires both sides to agree on definedness as well as returned values.
  `tv_smt_semantics.c` confirms that rewrites to wrapping negation or zero
  cannot erase those traps. CFG validation also rejects a proof when its
  bounded path exploration is incomplete. The bounded differential fallback
  now enumerates directed signed boundary values, covering those same traps
  without Z3.

- Native lowering now forward-declares its f64-to-f32 emitter before the
  return-ABI reconciliation call sites, keeping the strict C build valid.

- Crash forensics: fatal signals write the last compiler decision records to
  `$NYTRIX_CRASH_DIR/nytrix-crash-<pid>.log` with consecutive duplicates
  folded, working with and without debug verbosity.

- `docs/learn/api-matrix.md` was renamed to `docs/learn/routes.md`.

- The native function-index collector evaluates platform guards
  (#macos/#linux/#windows), socket sockaddr variants are unified, and variadic
  default expansion no longer materializes a NULL rest argument.

- Hardware byte-swap and rotate are first-class operations across NYIR, x86-64
  (`bswap`), AArch64 (`rev`), LLVM, and the debug VM.

- Strict-warning builds are clean in native lowering/IR/parser; stdlib
  bundling no longer emits malformed parse forms in TOML/YAML helpers.

- Local development tooling re-applied the lowering fixes and saved
  working-tree snapshots.

### Standard library and legacy lowering

- `_split_impl` takes typed string parameters, `cstr_to_str` takes a typed
  integer offset, and `_list_push_reserved` remains the documented native
  append contract.

- Loop element lowering uses the inferred element type instead of recognizing
  function names: a user `range` returning a string list preserves string
  elements. Legacy `core.math` argument checks require the registered module
  identity before applying name-based numeric rules, and float-like inference
  trusts reserved intrinsics or resolved return types rather than arbitrary
  `sqrt`/`sin` spellings.

### Platform

- Native JIT allocation cleanup releases memory through the matching platform
  allocator, removing a Windows-only failure-path `munmap` reference.

### Undated updates

- Fixed x86 native list-pop lowering to recompute the header address at its
  update store, avoiding an uninitialized branch-local stack home. The cold
  `result-list-mutate-x86-64` native oracle passes.

- Signed division by a positive power-of-two now uses a compact bias-and-shift
  sequence: negative values receive `(divisor - 1)` before arithmetic shift,
  which preserves truncation toward zero, including at `INT64_MIN`, without a
  remainder test or multiply.

- Constant folding preserves the scalar-class/ownership/metadata trio when
  rewriting arithmetic into constants, matching copy propagation and keeping
  CSE from treating the folded value as a different entity.

- The NYIR verifier now rejects control-flow and label instructions that carry
  a semantic ownership tag (a stale stamp from a buggy rewrite), and the
  mem2reg/SROA rewrites preserve ownership, mutability, alias, and range facts
  across their PHI-to-load and load/store rewrites instead of dropping them.

- A user function owning the `range` spelling decides its ABI from its
  declared return type before the stdlib range spelling check, and the memcall
  form consults the user method first — user `range` functions and `.range()`
  methods no longer feed ~20 downstream ABI sites a wrong raw range
  classification. The `c.sqrt` hardware-sqrt fold no longer hijacks a Nytrix
  local or module named `c`.

- Documented that insertion is validated at its actual statement position,
  avoiding a redundant parser-only emit-context restriction.

- Recorded an explicit effect-row design in `docs/learn/effects.md`: inferred
  rows, upper-bound annotations, qualified effect identity, latent
  higher-order effects, and alpha-stable interface fingerprints. This is a
  design milestone; the existing effect mask remains the implemented contract.

### Earlier updates (date unverified)

- AVR portable-call validation now admits runtime entries only when their ABI
  metadata marks an untagged, non-f64 raw pointer result with no dynamic
  boxing; the check no longer special-cases the allocator symbol.

- Callback effect analysis now preserves custom rows when a factory returns
  matching immutable callback tuples from simple if-return branches and the
  caller extracts a dynamic index. Tuple allocation is included in the
  declared rows.

- The verifier still accepts the separately bounded nonlinear cases only when
  interval evidence proves descent and a finite lower bound.

- The independent proof checker now splits finite variable intervals when
  direct nonlinear bounds overlap, capped at depth 8 and 255 partitions. The
  general proof search remains bounded and fail-closed.

- Structural measures now support a same-enum component after an unchanged
  integer/boolean tuple prefix.

- Increased the bounded proof constraint budget to 128 and aligned the
  independent checker's DNF walk depth with its declared depth budget. Solver
  DNF branch lists now grow in bounded heap storage instead of placing a full
  branch matrix on every recursive C stack frame. Division and remainder now
  admit symbolic divisors only when the active path proves nonzero and
  excludes the `INT64_MIN / -1` trap. Remainder intervals use C-signed bounds
  for positive and negative divisors; Z3 remainder claims and floor-division
  claims pass only when the independent checker validates C semantics.

- Effect-row deserialization now has an explicit regression for import
  isolation: decoding the same alpha-normalized open row twice creates
  distinct fresh tail variables.

- The x64 integer arithmetic fast path now seeds an assigned cross-block
  source from its canonical home before checking register residency. This lets
  the remaining consumers in that block reuse the seeded register instead of
  loading the same home repeatedly. `machine_home_reuse` executes a dynamic
  cross-block value, verifies the result, and asserts no stack-home offset is
  loaded more than once; `regalloc_cfg_edge_carry` remains passing.

- The main object's SHA call remains an external `ny_fn_sha256` relocation, so
  the textual helper frame/load counts are not verified for the final linked
  helper/runtime body.

- Inspected the actual SOR AOT `main` disassembly beside the matched LLVM
  assembly. The native inner loop reloads row bases and the column index from
  stack homes and forms each neighbor address separately; LLVM keeps pointer
  and byte-offset recurrences in registers and uses constant load offsets.
  Loop-carried register residency remains the next native SOR target.

### 2026-10-23

- Landed the fmt/lsp shared context-scanner batch from the trim backlog
  (smallest, low-risk item). Added `ny_scan_ctx_step` to
  `src/base/util.h` and replaced 33 byte-exact scanner blocks in
  `cmd/fmt/core.c` plus 1 in `cmd/tools/lsp/main.c`
  (306 duplicate lines, zero behavior change; the exact 47x/36x
  duplicate classes are gone — rerun scan now 9,117 classes / 563,605
  tree lines vs the 9,355 / 566,004 baseline, with other trim batches
  landing concurrently on the same tree). Validation: `./make build`
  clean; all 23 `etc/tests/code/probes/tools/fmt-*.nshape` parity
  fixtures pass (`ny-test --with-stdlib`, 23 total · 23 passed);
  full `./make check` 2432/2433 with the sole failure the pre-existing
  author-WIP `code/intermediate/ny/run-bin.nshape` (failure trace ends
  `returned=yes result=58`, verified unrelated); bench `startup` class
  ok at 53µs (0.93x, in bounds — no compile-speed trade) with the
  `dgemm` checksum FAIL proving ambient (passes standalone,
  1 total · 1 passed). Residue recorded: ~20 structurally different
  `if (esc)` sites remain in `cmd/fmt/core.c` (braced variants and
  differently nested contexts) — they were never part of an exact
  duplicate class and stay for a later batch.

- Final verification pass over the audit and sweep documentation. Probed
  against the release binary: the reserved-spelling hints (`void` →
  `none`, `printf` → `print`, `var` → `mut`/`def`), the trace master
  switches (`NYTRIX_TRACE=all`, `NYTRIX_TRACE_DEEP`), all seven optimizer
  profile names (unknown `--profile` names are rejected with
  `invalid optimization profile`; unknown `NYTRIX_OPT_PROFILE` values
  degrade to custom), both bigfloat precision clamp bounds, and the
  `__is_nil`/`__is_int`/`__cstr_*` input contracts — which turned out to
  be value-level ABI twins with narrower inputs than the managed
  predicates, so [Values](spec/values.md) was rescoped (the C-string
  helpers take raw FFI pointers; programs keep the unprefixed predicates)
  and [Functions](spec/functions.md) gained the exact implied set
  (`@pure`/`@consteval` imply `@readnone`/`@nounwind`/`@willreturn`/
  `@mustprogress`). All 13 fixture citations were confirmed to exist.

- Second docgap sweep (knob and spelling layer). Documented the
  `NYTRIX_TRACE=all` and `NYTRIX_TRACE_DEEP` master switches in
  [Debugging](learn/debugging.md) (probe-verified: trace output appears
  only with the switch), the optimizer profile set
  (`--profile=MODE` with `default`/`speed`/`peak`/`balanced`/`compile`/
  `none`/`size` and its `NYTRIX_OPT_PROFILE` subprocess inheritance) in
  [Performance](learn/performance.md), the bigfloat precision rules
  (default 256 bits, 32..1048576 clamp, `NYTRIX_BIGFLOAT_PRECISION`
  override cached on first use, `core.math.big`'s pinned `BF_PRECISION`,
  and `bf_with_precision` for per-value precision) in
  [Numbers](learn/numbers.md), and the remaining runtime-substrate
  framing (`__str_*`/`__list_*`/`__dict_*`/`__gc_*` behind `lib/` APIs;
  `__is_nil`/`__is_int` as the lib-facing predicate spellings) in
  [Values](spec/values.md). Every claim is grounded in the source site
  that reads the knob (`base/trace.c`, `base/options.c`,
  `runtime/bigfloat.c`); the `AUTO_*` LLVM-legacy knobs were
  deliberately left out as non-live-route internals.

- Closed the language-surface audit batch. The three open items all landed:
  the `__flt_*`/`__bigint_*`/`__cstr_*` boxed-value ABI layer gained a
  paragraph in [Values](spec/values.md) with fixture citations; the
  reserved-and-future-forms item closed empirically in
  [Syntax](spec/syntax.md) (`impl`/`operator`/`with` were already
  implemented and documented in Traits/Control flow — the audit's "no
  operator form" premise was wrong — `macro` is retired, and no future
  forms are advertised); and [Architecture](learn/architecture.md) gained
  contributor-facing Substrate guides for the e-graph, ISLE selection,
  tier planning, purity/effect analysis, null narrowing, the proof
  substrate (`proof.c`/`policy.c`/`lib/proof.ny`), the fuzz harness, and
  the SIMMD runtime, each grounded in file headers and pass wiring. With
  every item documented, probe-verified, and its fixture citations
  confirmed, the audit section was removed from TODO.md under the
  close-on rule; this day's changelog entries keep the full record.

- Completed a symbol-level audit of the language surface (93 token kinds,
  keyword tables, `@`-attribute usage across fixtures, cross-checked
  against the ebnf grammar and the full docs corpus), then documented the
  previously unexplained pieces in the house style: tag/handle
  introspection (`__tagof`, `__runtime_tag`, `__handle_valid`) under the
  Native layout helpers; defer-queue diagnostics, `__jmpbuf_*`, and
  `__get_backtrace` frames under Control flow; the open attribute
  namespace plus the first-class memory and termination contract
  attributes (`@readnone`, `@readonly`, `@writeonly`, `@nounwind`,
  `@argmemonly`, `@mustprogress`) and two Quirks rows (reserved near-miss
  spellings, open attribute namespace). Three initial audit claims were
  corrected during
  cross-checking: the `del` binding semantics, the contextual `quote`
  rule, and the `dict.set` in-place/chaining contract were already
  documented; the audit section in TODO.md now records which claims were
  corrected, which items were closed by this pass, and which remain open
  (remaining `__` conversion families, the reserved/future forms decision
  page, and the contributor internals pages).

- Closed the nil-literal ambiguity class rather than only the `is_nil`
  symptom: the proof evaluator (`nyr_eval_proof_i64_env`,
  `src/code/lang/lower.c`) now refuses `NY_T_NIL` literals where an `i64`
  constant is required, so no integer fast path can fold `nil` into a
  proven `0` again. All proof-eval consumers are integer-domain (index,
  bounds, lemma, and return-value proofs, plus the `__is_nil` catch-all
  that already intercepts `nil` earlier), so refusal is a no-op for provable
  integers and only removes silent nil-to-zero folds. The invariant is
  documented in [Values](spec/values.md) (`nil` literals parse with
  `NY_T_NIL`; producers re-derive nil-ness from the token kind), and the
  spec contract examples moved from out-of-tree probes to gate fixtures:
  `etc/tests/lang/values/nil-dynamic-identity.nshape` (nil identity, `any`
  boxed nil, dictionary nil keys) and
  `etc/tests/lang/memory/atomic-cell-contract.nshape` (tagged atomic cell),
  and the deferred-capture example from
  [Control flow](spec/control-flow.md) was added verbatim to the existing
  `etc/tests/lang/control-flow/control.nshape` defer fixture, so every
  formerly failing spec example is now gate-covered and the spec pages name
  their fixtures. Evidence, std caches disabled: both new fixtures pass cold
  (`./build/release/ny-test --with-stdlib <fixture>…`, 2/2, three variants
  each) and on `./build/release/ny-full` with and without `--native-only`
  (`cc_values ok`, `cc_atomic ok`); the existing `lang/values/nil.nshape`
  suite still passes both routes after the evaluator change, and
  `lang/control-flow/control.nshape` passes with the added contract
  (`deferred_order() == "ba"`, three variants).

- Bench JSON rows now record the exact copy-pasteable Nytrix command lines
  behind each result: `ny_argv` for the timed route and `ny_stats_argv` for
  the counter/tier-report compile whose `facts` row feeds
  `ny_vector_attempted`. Verified on a focused `startup/dce.nshape` bench:
  `ny_argv` is `… -O3 --native-only --native-backend x86_64 -time -run <fixture>`
  and `ny_stats_argv` carries the `--native-tier-report` variant. This makes
  vectorizer-reach reports reproducible from a single artifact. The default
  bench driver now also writes a dated JSON artifact under
  `build/bench-reports/` unless `--bench-out-json` is given.

- Diagnosed the dgemm vectorizer reach on the current build: the candidate
  scan is reached (`facts vector_attempted=32`; `rt_main` attempts 14 loops),
  and `NY_TRACE_VECTORIZE=1` enumerates the rejects — non-canonical loop
  guards ×191, missing canonical +1 induction PHIs ×167, unsupported
  loop-carried f64 PHIs ×22 (the `s += load*load` accumulator), plus small
  counts of non-affine addresses, effect blocks, and unmodeled traps. The
  earlier "zero attempts / silent trace" reading was a stale-binary artifact;
  the TODO item now tracks `ny_vectorized > 0`.

- Fixed `atomic_add`/`atomic_sub` corrupting `atomic_i64` cells. Atomic cells
  hold tagged language words — the constructor, `atomic_store`, `exchange`,
  and CAS all pass tagged words through unchanged (store(5) leaves `11`,
  which reads back as 5) — but add/sub fetched with the raw untagged
  magnitude, so `atomic_i64(0)` + `atomic_add(c,1)` left word `2` (the false
  immediate) instead of tagged `1`, and every read printed `false`.
  `rt_atomic_i64_delta` (`src/code/lang/runtime/memory.c`) now doubles the
  untagged magnitude so the tag bit survives: `(2n+1) + 2k == 2(n+k)+1`.
  Evidence on `./make`-built `build/release/ny-full`, std caches disabled:
  the spec contract example passes (`assert(atomic_load(c) == 1)`), and the
  extended probe covered load/store, add/sub with negative deltas,
  exchange, CAS hit, and zero-store round-trips. No prior atomic coverage
  existed in `etc/tests`; temporary probes first exposed the failures.

- Fixed `is_nil` on top-level `def any boxed = nil` returning false while
  the value had correct nil identity (`type` reported `nil`, calls and
  compares agreed the word was nil). Root cause in the NYIR lowerer's
  `__is_nil`/`is_nil` fold (`src/code/lang/lower/call.h`): nil literals
  parse as `NY_LIT_INT` zero, so the proof evaluator folds a top-level
  `def any` local mirror to its declaration constant — a proven `0` — and
  the catch-all misread that proven zero as "raw integer zero" (the
  raw-int ABI reserved in this comment block) and emitted `NY_IMM_FALSE`.
  Proven zero under the tagged dynamic ABI is the nil immediate, not an
  integer payload. The fold now routes tagged-dynamic arguments (any-
  typed locals, `NY_SEM_REP_TAGGED_DYNAMIC`) to a literal nil compare of
  their runtime word instead; raw-integer arguments keep the existing
  false fold. Evidence: the `spec/values.md` example passes
  (`is_nil(boxed) && type(boxed) == "nil"`), plus def/mut locals, param
  views, and call-result shapes on the current build, std caches
  disabled. Residual (pre-existing, separate store ABI): a top-level
  `mut any` local reassigned with `m = nil` still reads `is_nil(m)`
  false while `m == nil` agrees true; recorded for a follow-up store-side
  fix.

- Refreshed the 2026-10-22 documentation smoke limitations: the dict
  setter-reassignment row passes on the current build (`s1 ok`), the atomic
  row is fixed above, and the `is_nil` row is fixed above. Deferred mutable
  capture was later fixed and verified across native and LLVM routes; see
  [Compiler gates and optimization evidence](#compiler-gates-and-optimization-evidence).

### 2026-10-13

- AVR native-only now lowers signed i64 division and remainder with a
  restoring binary long-division routine. Panic-trap labels include the
  function name, preventing collisions when multiple functions in one module
  contain checked operations.

- AVR native-only now lowers bounds checks by rejecting negative signed
  offsets and bounds, then comparing the nonnegative values.

- The AArch64 portable text backend now materializes arbitrary 64-bit integer
  constants with `movz`/`movk` rather than emitting invalid `mov` immediates.
  It lowers `bswap.i32` with `rev w0,w0` and `bswap.i64` with `rev x0,x0`.

- AVR now accepts the statically recognized `rt_cstr_to_str` →
  `rt_alloc_string` → `rt_panic` path for literal messages and lowers the
  panic to `break` followed by a non-returning loop.

- Exclusive-place analysis now recognizes bounded `@returns_borrow(param)`
  functions whose exhaustive `match` arms return `borrow_mut(param)`. The
  selector, patterns, and guards must be side-effect-free, there must be an
  unguarded wildcard or explicit default, and every arm must return the same
  parameter borrow. Other match shapes remain conservative.

- The portable WebAssembly panic path now exports its linear memory, emits
  NUL-terminated static string data, and writes literal panic and checked
  division-by-zero messages through WASI `fd_write` before trapping. General
  dynamic-string construction and unrelated runtime imports remain
  unsupported.

- RISC-V64 checked division now links without the unavailable runtime
  allocator and panic helpers: only a static `rt_alloc_string` immediately
  consumed by `rt_panic` is elided, and the panic call lowers to `ebreak` plus
  a non-returning loop. This trap path does not preserve a panic message;
  other runtime panic calls remain unsupported.

- Exclusive-place analysis now follows a `@returns_borrow(param)` call to its
  actual source argument. It classifies the returned pointer as exclusive only
  when the callee directly returns `borrow_mut(param)`, returns a bounded
  straight-line chain of local aliases rooted at that call, every arm of a
  side-effect-free `if` directly returns `borrow_mut(param)`, or a bounded
  chain of up to 15 side-effect-free early-return `if` statements falls
  through to the same borrow. Other return shapes remain conservative.

- Unverified `@returns_borrow` bodies now fail closed in strict ownership
  analysis: while the returned borrow is live, possible accesses are treated
  as overlapping an unknown source. Direct returns, local aliases, and
  side-effect-free `if` branches returning `borrow_mut` retain precise
  disjoint-place checking.

- Effect callback invalidation now tracks list and dictionary callback facts
  by bounded local alias groups. Mutating one alias invalidates facts for that
  group without discarding independent same-kind containers. The
  returned-callback differential also includes a generated dictionary
  alias-mutation positive/negative pair.

- Match selectors, patterns, and guards remain required to be side-effect-free
  and exhaustive.

- Dynamic dictionary-key places are represented as wildcard indices for
  exclusive-borrow overlap checks, so neither an identical dynamic key nor a
  different variable key is assumed disjoint.

- Fixed-index list refinements using integer `<`, `<=`, `>`, `>=`, `==`, or
  `!=` now survive a write only when the index is disjoint or the stored
  signed-i64 constant proves the predicate against an immutable top-level
  signed-i64 constant bound. Checked arithmetic is supported in those constant
  expressions; overflow, unknown values, and local shadows fail closed. String
  and boolean `==`/`!=` use resolved literal value equality, not identifier
  spelling. Appending preserves nonnegative fixed-index predicates; a `-1`
  predicate survives only when the appended value proves it. Every conjunct
  must remain proven; overloaded comparisons remain fail-closed. When a
  fixed-index comparison's other operand also reads the list, a store is
  accepted as disjoint only when those reads cannot alias the destination;
  mixed-sign fixed indices remain potentially overlapping.

- A fixed-index equality refinement also survives storing the exact expression
  from its opposite operand, provided the expression is side-effect-free and
  does not read the written index. This proves the equality by construction;
  different values, unsupported expressions, overloaded equality, and reads
  that may alias the destination remain invalidated.

- Content-dependent store preservation now composes across bounded `&&`/`||`
  predicates: each atom must remain unchanged or be proven true by the
  predicate, so every conjunct stays valid without relying on short-circuit
  branch assumptions. For append, `items[-2] == items[-1]` survives when the
  appended expression is the exact old `items[-1]`; equality and inequality
  both survive when appending the exact old `items[-2]`, which swaps the pair.
  Appending the old last element to an inequality is rejected because it makes
  the new last two elements equal. Unsupported negative-index expressions fail
  closed. Copied-tail proofs compose with bounded conjunctions and disjunctions
  only when every other atom is independently append-stable. Generated
  positive/negative cases cover same-tail invalidation, pair-swap preservation,
  conjunction transport, and a nonnegative indexed value whose negative-index
  bound changes after append.

- A fixed `items[-1]` comparison against an immutable scalar bound now
  survives append when the appended value is that exact bound and the relation
  is reflexive (`==`, `>=`, or `<=`). Appending another value expires the
  fact.

- Append now also preserves monotone comparisons between a fixed-index value
  and `items.len` (`items[i] < items.len`, `items[i] <= items.len`,
  `items.len > items[i]`, or `items.len >= items[i]`) when every indexed
  operand is stable across append. Nonmonotone comparisons and shifted negative
  indices still expire the refinement. Generated positive and negative modes
  cover both operand orders and a relation invalidated by length growth.

- Content append transport checks stability of the comparison's bound, not
  only its indexed left operand. A negative-index bound that shifts on append
  invalidates the proof; copying the exact old last element preserves reads
  of `items[-1]`. The generated differential catches the previously accepted
  invalid `items[0] < items[-1]` append and its preserving counterpart.

- Exact opposite-expression stores now prove reflexive `<=` and `>=` content
  predicates as well as equality, provided the expression is side-effect-free
  and cannot read the written index. The generated differential accepts an
  exact bound reconstruction and rejects a value beyond that bound.

- The generated mutable-flow differential now also covers exact immutable
  arithmetic-expression stores under symbolic equality, paired with a
  violating store.

- Transparent callback-container forwarding now includes argument evaluation
  in its safety check. A nested forwarding call with a computed argument is
  not treated as an unchanged-container return: argument evaluation may mutate
  or escape that container. A generated negative case mutates a dictionary
  callback while evaluating another argument and rejects an underdeclared
  caller; the returned-callback differential passes cold.

- Callback effect provenance now follows a bounded chain of immutable local
  aliases across interprocedural forwarding functions. Every body in the chain
  must contain only immutable identifier aliases and a direct return or a
  nested forwarding call; computed arguments, mutation, and unknown bodies
  remain conservative. Generated positive and underdeclared negative cases
  cover an aliased inner return followed by a second wrapper.

- Returned callback-container summaries now recurse through bounded
  interprocedural calls when a forwarding function receives another function's
  returned list or dictionary. The nested result must itself have a supported
  returned-container flow; unknown results retain conservative callback
  effects. Generated positive/negative modes cover a locally mutated callback
  list returned through an identity wrapper.

- Generated effect coverage now follows a returned closure that captures a
  mutable callback alias. Replacing the callback after closure creation is
  reflected when the escaped closure is invoked; the paired underdeclared
  effect contract is rejected. A second pair verifies callback effects survive
  forwarding the closure through an identity function.

- Returned callback effects now propagate through bounded chains of direct
  callback-returning calls. Each forwarding function must return its nested
  callback call directly; computed or side-effecting prefixes remain
  conservative. Generated positive and underdeclared cases cover two
  forwarding layers around closures that capture a callback replaced after
  closure creation.

- The five reported generated D2 regressions—noalias index overlap,
  returned-callback aliases, length-bound flow, allowance flow, and mutable
  aggregate flow—pass together cold and serially after these changes.

- Closed the D2 tracking item after the focused generated flow suite passed
  5/5. The supported refinements and alias/effect transfers are listed above;
  unknown aliases, dynamic dictionary-key overlap, unverified borrow-return
  bodies, mutable symbolic bounds, and paths beyond analysis limits remain
  conservative and fail closed.

- The noalias differential now also verifies that an unrecognized
  `@returns_borrow` body shape fails closed: a borrow returned after an
  unrelated local declaration is treated as unverified, so even a disjoint
  write is rejected. The focused differential passes cold.

- Effect callback facts now pass through a callee that returns one of its
  typed list/dictionary parameters unchanged. The callee must consist solely
  of `return parameter`; facts are preserved only for the returned container
  alias. Generated modes 91–94 cover dictionary/list forwarding and reject an
  underdeclared caller.

- Returned callback-container provenance also survives bounded exhaustive
  `if`/`match` bodies when every arm returns the same container parameter,
  including up to 16 consecutive immutable local aliases of that parameter.
  Pure single-return forwarding chains of up to 16 calls also preserve the
  parameter identity.

- Effect callback facts also pass through a callee that binds one typed
  list/dictionary parameter to a single local and immediately returns that
  local. No intervening mutation or escape is accepted. Generated modes 95–98
  cover both container kinds and reject underdeclared callers.

- Fixed-index list refinements with an immutable scalar comparison bound now
  survive disjoint same-sign fixed-index stores and append at nonnegative
  guarded indices. A store to the guarded index preserves the fact only when
  it stores the exact bound identifier and the comparison is `==`, `>=`, or
  `<=`. Other mutable bounds and unsupported relations still fail closed.

- The AVR portable emitter now lowers optimized 64-bit rotate-right with a
  masked variable count, byte swaps, and all six signed i64 comparisons.

- AVR now emits `ror32.i64` as a zero-extending 32-bit rotate.

- BPF `rt_main` now receives its `socket` section before the function symbol
  and label are emitted, and its ELF size is closed while still in that
  section. This fixes the zero-sized `.text` program that libbpf rejected. A
  minimal object with a noinline internal call loaded on the host kernel with
  `CAP_BPF` and `CAP_PERFMON`, passed `BPF_PROG_TEST_RUN` on a 64-byte packet,
  and returned the expected tagged value, 85.

- Portable function prologues now spill only declared parameters, not every
  local slot up to the function's maximum slot index. The old behavior read
  nonexistent BPF argument registers in one-argument callees, so the kernel
  verifier rejected a function using local memory. A noinline BPF program with
  local byte/i64 accesses and an internal call now loads and executes through
  `BPF_PROG_TEST_RUN`, returning 171.

- Bounded disjunctions of fixed-index integer comparisons now retain list
  refinements across stores and append only when every comparison is proved
  preserved.

- Bounded returned-callback effect inference accepts side-effect-free boolean
  conditions composed from identifiers, literals, `!`, `&&`, and `||`.

- Dynamic-key replacements in local callback lists and dictionaries now retain
  a complete effect summary by joining the pre-mutation container summary with
  the stored callback. Restoration requires a known summary and stable key and
  callback expressions; unknown callback rows still invalidate the summary,
  and same-kind aliases remain invalidated. Generated accepted and
  underdeclared cases cover builtin and custom capability rows for list
  replacement, dictionary `set`, and indexed dictionary assignment.

- Generated builtin/custom-row positives and underdeclared negatives cover
  callback escape through returned containers, mutation joins, and returned
  aliases; unsupported factory shapes remain conservative.

- The BPF, MIPS64, and PowerPC64 portable emitters now materialize local-slot
  addresses and lower byte- and word-width memory operations.

- MIPS64 and PowerPC64 now lower NYIR bounds checks to signed-negative and
  upper-bound trap sequences.

- PowerPC64 NYIR constants now materialize all 64 bits instead of relying on
  the signed-16-bit `li` instruction for arbitrary values.

- The direct WebAssembly text emitter now lowers byte and i64 memory loads and
  stores using linear-memory instructions. Separate byte load and word store
  probes trap at the end of a one-page memory.

- The direct WebAssembly emitter now lowers scalar f32/f64 constants,
  arithmetic, comparisons, square root, and integer/float conversions.

- AVR now lowers a bounded scalar NYIR slice: i64 constants, local spills,
  addition/subtraction/multiplication, bitwise operations, variable-count
  left/arithmetic-right shifts, direct internal calls with at most two i64
  arguments, conditional branches, and `select.i64`. AVR-GCC argument/result
  and callee-save conventions are respected. Spill frames use Y-relative
  accesses, with Z-based addressing beyond displacement.

- BPF portable output now places `rt_main` in a `socket` program section and
  emits a GPL `license` section.

- The shared ARM/AArch64/i386/RISC-V stack emitter now enforces
  `NYIR_BOUNDS_CHECK` instead of skipping it. Target-width comparisons reject
  negative offsets and offsets at or beyond the bound, using each ISA's trap
  instruction.

- The BPF text emitter now lowers 64-bit rotate-right to a masked right shift
  and complementary masked left shift. Runtime loading remains unavailable in
  this environment: unprivileged BPF is disabled and no effective `CAP_BPF` or
  `bpftool` is present.

- The portable WebAssembly emitter now enforces `NYIR_BOUNDS_CHECK` with
  signed-negative and upper-bound traps.

- Mutable list refinement transport now recognizes concatenation with an
  in-scope list-typed local suffix, not only a literal list suffix. Facts
  about the unchanged nonnegative prefix survive; negative-index facts still
  expire.

- Dereferencing a direct `borrow`/`borrow_mut` expression now resolves the
  exclusive borrow to its syntactic source place. Other unresolved pointer
  provenance conservatively overlaps every access while live.

- Returned callback summaries now have generated coverage for a branch-joined
  callback alias captured by two nested local lambda wrappers.

- Bounded early-return callback factories now join up to fifteen
  identifier/literal-guarded `if` returns plus one direct fallthrough return.
  Direct-return `match` arms with an `else` return or a direct fallthrough
  return are joined too. Leading local callback bindings are preserved through
  the bounded return analysis, so captured lambda aliases in a branch join
  retain their inferred rows; generated positive and underdeclared cases cover
  this path. Fully returning nested `if` trees are joined within the same
  branch-count and depth limits, with generated positive and underdeclared
  cases.

- Reassigning a mutable callback binding now refreshes the builtin-effect
  summary of local lambdas that capture it, including wrappers that capture
  another dependent lambda. Exact dictionary callback facts for captured
  lambdas are refreshed too; the affected dynamic-container summary is
  invalidated. Custom effect rows affected by such a capture change remain
  unknown and fail closed. Generated modes 31–46 cover both reassignment
  directions, nested wrappers, underdeclared builtin rows, exact dictionary
  and list lookup through aliases, dynamic summary invalidation, and a
  custom-row underdeclaration.

- Exact literal-key `dict.set` calls now update local callback provenance
  after invalidating the dictionary's previous callback facts. Generated
  positive and underdeclared cases verify the replacement callback's builtin
  IO row.

- The direct portable WebAssembly text emitter now has an executable scalar
  parity fixture: native-only NYIR output assembles with `wat2wasm` and
  Wasmtime invokes an internal i64 function call with the expected tagged
  result. Checked division now uses a local identity/trap helper pair only for
  the compiler-generated message-allocation followed immediately by panic:
  nonzero division returns the expected tagged result, zero divisor traps, and
  unrelated external imports still fail closed. The trap preserves failure but
  not the runtime panic message.

- Both emitters lower all six signed i64 comparison predicates instead of
  producing a comment/no-op; PowerPC register syntax and per-function local
  labels are valid for the assembler. Optimized `ror.i64` and `select.i64` now
  have executable MIPS64/PowerPC64 lowering. WAT `select.i64` converts its i64
  condition to the required i32 selector; both Wasmtime paths verify optimized
  compare/select control flow. Panic-message output and unrelated runtime
  helper imports remain unsupported.

- The shared stack emitter now lowers `select.i64` on RISC-V and gives NYIR
  branch labels function-specific names, preventing duplicate symbols when
  multiple functions contain control flow. Checked division and explicit
  literal panic now link and execute: static-message allocation immediately
  consumed by `rt_panic` is elided, and panic lowers to `ebreak` plus a
  non-returning loop.

- The shared stack text emitter lowers `select.i64` on ARM, AArch64, i386, and
  RISC-V. ARM leaf functions now return through `lr` instead of popping an
  unsaved return address. The generic seven-backend matrix now compiles
  optimized select flow.

- The BPF portable emitter now rejects optimized 64-bit rotate-right instead
  of silently emitting an arithmetic shift. Arithmetic right shift, divmod,
  and ordering comparisons now emit their signed eBPF operations. Comparison
  labels are unique across functions. Symbol addresses used by dynamic
  checked-divmod guards now use eBPF `lddw` syntax. The emitted BPF frame
  comment also uses assembler-valid syntax. This verifies object encoding
  only; loadable program sections and panic runtime integration remain
  unimplemented.

- Dynamic panic-message output and operations outside the exercised AVR subset
  remain unsupported.

- AVR now has QEMU runtime coverage for scalar calls, branches/select, integer
  arithmetic and shifts, local spills, and byte/word memory operations.

- Canonical list `append` results now retain known fixed-index callback facts
  from their source list. When the source-list summary and appended callback
  row are both complete, dynamic-index calls use their union; a callback
  parameter's known open owner row is preserved as a latent row, while unknown
  summaries remain fail-closed. Discarding the result preserves source facts,
  since append returns a new list. Other list mutations remain conservative.

- Returned-callback factories now strip builtin effect labels from each branch
  row before joining custom capability rows. This avoids losing a complete
  summary when three returned callbacks repeat a builtin effect label.

- Exclusive-borrow place analysis now distinguishes exact literal string keys
  in dictionary indexing and canonical indexed writes. Different keys are
  disjoint; same-key access remains overlapping.

- Callback-summary invalidation now uses a known container's kind: mutating a
  dictionary no longer erases list callback summaries, and vice versa.
  Same-kind aliases remain conservatively invalidated; unknown container kinds
  still invalidate both.

- Allowance analysis now handles a reassignment in a trailing `defer` at the
  lexical block's scope exit when its body is within the nonthrowing
  assignment fragment. This preserves uses after defer registration in the
  pre-exit epoch and validates the new epoch separately.

- Allowance analysis now recursively unrolls nested fixed counted loops and
  literal ranges when every loop is bounded and the nested body remains in the
  nonthrowing supported fragment. The existing path cap still fails closed.

- A fact survives only when pattern and guard evaluation is read-only and each
  arm's mutation is independently proved preserving.

- Mutable-list length lower-bound transport now also recognizes comparisons in
  reversed operand order: `N < items.len` and `N <= items.len`.

- Mutable-list refinement flow now recognizes assignment from concatenation
  with a literal list suffix. Direct length lower bounds and nonnegative
  fixed-index/content facts survive because the original prefix is unchanged;
  negative-index facts expire. Overloaded addition and computed suffixes
  remain conservative.

- Literal-list prefix concatenation preserves direct list length lower bounds,
  but invalidates fixed-index/content facts because prepended values shift the
  original elements. Overloaded addition remains conservative.

- Callable effect factories join rows from up to three leading `if` returns
  followed by a direct fallthrough return. For example:

  ```ny
  if choose { return callback_a }
  return callback_b
  ```

  This is a function-body fragment. Both returned callback rows contribute,
  including their custom capability rows.

- Bounded allowance loops now model parameter reassignment independently in
  each match arm, using the existing per-branch path state.

- Mutable literal callback lists and dictionaries now retain per-slot effect
  summaries. Exact nonnegative-index or string-key writes through canonical
  `core.set_idx` refresh the written slot. For local lists, only that
  binding's callback facts are invalidated because lowering detaches a shared
  list before the write; the source COW alias retains its original callback
  facts. Dictionaries keep global copied-alias invalidation. A branch-joined
  local callback alias stored into an exact slot retains the joined row.

- Mutable-list refinement transport recognizes direct `.len > N` and
  `.len >= N` predicates for folded nonnegative integer bounds. Those facts survive
  fixed-element stores and builtin append; a `pop` still invalidates them.

- Mutable-list refinements now also preserve bounded `&&` facts combining
  fixed-index integer comparisons with direct length lower bounds. Fixed-index
  stores and append retain the conjunction only when every content atom is
  stable and every length atom is an append-preserved lower bound;
  exact-length conjunctions still expire after append.

- Mutable-list refinement transport now preserves a bounded disjunction of a
  direct length lower bound and a fixed nonnegative content predicate through
  append only when each alternative remains true.

- Mutable callback-effect tracking now invalidates only the list snapshot
  changed by a direct indexed assignment. Copy-on-write aliases retain their
  independent callback rows; the replaced slot receives the assigned
  callback's row.

- Assigning the result of builtin list append now carries forward exact source
  slot facts and combines a complete known callback summary with the appended
  callback's effect mask and custom row. If either summary is unknown, the
  destination's dynamic-index summary remains unknown.

- Callable factory effect inference now follows returned local callback
  aliases, including aliases of callback parameters and aliases selected by
  ternary, `if`, and `match` joins. It preserves latent builtin and custom
  effect rows. Factories with an earlier return remain fail-closed rather than
  using an incomplete summary.

- The `[0,2]` case is accepted and `[1,2]` is rejected for the missing minimum
  use under strict native-only NYIR verification.

- An unguarded wildcard no longer creates a false zero-use path; guarded
  wildcard fallthrough remains represented.

- Effect analysis now carries the union of builtin and custom effect rows when
  a local callback is selected by a ternary. Lowering also preserves the
  callable ABI marker when a ternary selects named functions, including nested
  literal ternaries.

- Fixed-index integer refinement transport now folds bounded signed constant
  expressions in indices, comparison bounds, and stored values. Addition,
  subtraction, multiplication, division, remainder, and unary negation are
  accepted only when every intermediate is representable and division is
  defined; overflow and division by zero remain unknown.

- Mutable list refinement transport now preserves arbitrary content predicates
  across an exact indexed self-store. Refinement names also retain their base
  list indexing interface inside the guarded region. The generated refinement
  differential includes both flow cases.

- Ownership allowance analysis now splits layout-guard success and fallback
  paths, closing the pre-reassignment epoch only on the fallback path. Loop,
  potentially throwing `try`, and `defer` reassignment still fail closed.

- Allowance epochs now split inside try bodies proven non-throwing by the
  bounded literal/identifier assignment checker.

- Ownership allowance analysis now unrolls literal `for` ranges of up to 64
  iterations when the body uses supported nonthrowing control flow.

- Ownership allowance analysis also unrolls canonical counted `for` and
  `while` loops with literal initialization/bounds and unit increment or
  decrement when they execute at most 64 times. Strict and inclusive ascending
  or descending conditions split each bounded body epoch; empty loops preserve
  the current epoch, while dynamic or unsupported headers and induction-
  variable writes fail closed.

- Parser-generated indexed assignments now carry an explicit AST marker.
  Borrow-path analysis and mutable-refinement transport apply `set_idx` rules
  only to those synthetic stores; a source function with that name follows
  ordinary call handling.

- Exclusive-borrow place analysis now carries dynamic list indices as
  conservative wildcards while preserving member suffixes. Different fields
  remain disjoint even when indices may alias; same-field accesses remain
  overlapping.

- Exclusive-borrow analysis now folds checked literal signed-integer
  arithmetic in list indices. Computed disjoint indices retain a noalias
  proof, while two expressions resolving to the same index remain rejected.
  Indexed path comparison now interprets bracket components before matching
  their contents, so mixed-sign pairs such as `[-1]` and `[2]` conservatively
  overlap.

- Mutable list content refinements now survive fixed-length stores at distinct
  fixed indices when all predicate and store indices have the same sign.
  Append also preserves fixed `-1` integer comparison atoms when a constant
  appended expression proves them; other negative-index predicates still
  expire when their referents shift.

- Content predicates whose reads use fixed nonnegative indices now also
  survive appending to the list. A fixed `-1` integer comparison also survives
  when the appended constant satisfies the comparison, including conjunctions
  with unchanged nonnegative-index atoms. Other negative-index predicates
  still expire.

### 2026-10-12

- Exclusive-borrow place analysis now proves distinct constant indices
  disjoint when both are nonnegative or both are negative, while retaining
  overlap for mixed-sign indices whose aliasing depends on list length.

- Ownership allowance analysis now keeps separate bounded path states through
  `if` and `match`, closing an epoch on each path at the point it reassigns
  the parameter. One-sided joins and early exits no longer merge pre- and
  post-reassignment use counts. Reassignment in loops, `try`, `guard`, and
  `defer` fails closed as inconclusive.

- Mutable list refinements now transport bounded conjunctions of fixed-index
  integer comparisons across element stores only when every atom remains true.
  A write to an unrelated nonnegative index is proven disjoint; a write to a
  guarded index must prove the stored literal satisfies its comparison. Append
  preserves these facts only when all indices are nonnegative. Negative-index
  aliasing and append cases fail closed.

- Exact indexed stores now preserve `==`, `<=`, and `>=` refinements when the
  stored expression reconstructs the guarded side-effect-free arithmetic bound
  from immutable identifiers. Generated positive, violating-store, and
  mutable-bound invalidation cases pass in the mutable-flow differential.

- Append now transports a `items[-1] <= bound`-style refinement when the
  appended value reconstructs the same side-effect-free scalar expression
  from immutable identifiers. The generated differential verifies the exact
  preserving append and a one-step violating append.

- Ownership now verifies bounded `@returns_borrow` forwarding chains by
  tracing the callee's annotated source parameter through each body to
  `borrow_mut`. A two-wrapper positive disjoint-store case and overlapping
  negative case pass the generated noalias differential; unsupported bodies
  remain unverified.

- Returned callback-container effects now compose across two bounded identity
  wrappers around a locally mutated returned list. Generated positive and
  underdeclared negative cases pass.

- Backend acceptance covers all five portable emitters' rejection paths and
  WebAssembly `ROR32_I64` count boundaries (0, 1, 8, 31, 32, and -1).

- Machine lowering now accepts `F64_TO_I64` and `F32_TO_I64`. The AArch64
  object encoder also handles the single-precision conversion.

- Ownership allowance analysis now marks depth-limit results inconclusive and
  reports a compile-time error instead of counting an unanalyzed subtree as
  zero uses. The source borrow checker also reports its bounded traversal
  instead of silently skipping deeply nested statements and expressions.

- ELF64 `object_link_run` dispatch now reads `e_machine` instead of routing
  every ELF64 object through the AArch64 runner.

- Expanded ownership closure-capture traversal through nested statement
  bodies, match expressions, f-string interpolations, assembly operands, and
  additional expression wrappers.

- Added AArch64 ELF emission to the public Nyr object API for supported NYIR
  operations. The standalone C frontend now carries AAPCS64 aggregate
  parameter and return metadata through the AArch64 machine encoder; aggregate
  parameter homes preserve ascending memory order despite descending frame
  slots, and aggregate-signature entry functions receive the platform ABI
  spills. Unsupported AArch64/Windows aggregate targets still fail closed.
  Closed D5.

- Scoped the SOR grid buffers to the local `run_sor` helper, allowing the O3
  vectorizer to widen the inner column loop to four lanes. This is a measured
  SOR improvement; it does not establish native/LLVM parity.

- Refinement flow now preserves a checked `list.len > 0` fact across indexed
  element stores and append-to-self assignments inside the guarded region.
  Whole-list rebinding, mutating calls that may empty the list, and
  content-dependent predicates remain fail-closed. Alias-return calls before
  the guard and closure captures also remain fail-closed.

- `@allowance` now treats direct parameter reassignment as an epoch boundary.

- The standalone C frontend's completed AAPCS64 and Win64 cross-language
  matrix is recorded in the D5 entry above.

- Win64 COFF object emission now rejects aggregate ABI metadata before either
  the machine or NYIR encoder can emit a mismatched calling convention.

- Mach-O x86-64 object emission now has a verifier-enabled C aggregate matrix
  for integer, mixed, SSE, packed memory-class, aligned, and three-word
  layouts.

- The restored scalar, aggregate, and function bindings remain usable and
  mutable.

- Implemented `@async_effects` lowering: eligible direct calls produce
  stackless handles with positional/default argument expansion. Fixed async
  argument packing to use byte offsets, preserving multi-argument calls.

- Closed D4: typed `Code<T>` values, helper transport into template splices,
  source-text parsing into generated declarations, category diagnostics, and
  parser depth/size limits have focused coverage.

- A type-rejected assignment leaves earlier mutable state usable by later
  submissions, and `:trace off` reconstruction replays accepted updates while
  retaining earlier functions. Closed D6.

- Documented the proof/refinement acceptance matrix against paired positive,
  negative, and inconclusive fixtures. The documented calculus now names the
  exact tests for closed propositions, bounded nonlinear arithmetic, solver
  limits, refinement transport, mutation, and backend parity.

- Clarified that handles preserve aggregate runtime values without converting
  them and do not own host memory referenced by raw pointers.

- NYIR match lowering now keeps tagged dynamic subjects out of the raw-integer
  dense-dispatch optimization, so integer literal patterns compare against
  correctly tagged values. Consumer-specific LLVM cache-link and post-link
  verification failures no longer unlink the validated shared core bytecode.

- Expression lowering now preserves the proven borrowed ownership on exclusive
  borrow-call results instead of overwriting it with the generic unknown
  ownership stamp.

- BigInt type probing now calls a value from the dynamic globals registry only
  when its runtime tag identifies a function pointer.

- The ownership runtime-shadow and generated allowance differentials pass with
  the snapshot and current shared core bytecode.

- Effect-summary propagation now passes the appended-list item expression at
  both local declaration and reassignment sites, matching the helper's
  four-argument contract.

- Byte-load producers carry the raw integer ABI before local-store and call
  reconciliation, preserving odd byte values in shared-core string copies.
  Documentation-tree collection remains a separate failing acceptance check.

- Native dictionary getters keep numeric values tagged until the actual
  consumer requests a scalar or string; a string-returning function no longer
  turns unrelated numeric fields into text.

- Runtime locals shadow module constants during qualified integer folding.
  Dynamic multiplication boxes raw integer operands before entering the tagged
  arithmetic ABI, preserving dictionary-derived dimensions and row offsets.

- Byte, word, dword, qword, and typed-buffer memory indices use the existing
  integer-value normalizer before address arithmetic.

### 2026-10-11

- Boolean-formal normalization now stamps its emitted predicate as raw i64
  before integer ABI reconciliation, preventing `true` from being decoded as
  tagged integer zero. The source-loaded NYIR CLOC smoke counts CRLF source,
  multiline comments, and inline comments as `3`, `1`, and `1`.

- Inferred handle locals discard a fallback `any` type spelling when their
  emitted producer establishes a concrete carrier. Initializer scalar decoding
  also requires an actually tagged producer. The source-loaded AST smoke
  preserves a typed declaration sequence and its `declarations` category.

- Conditional locals retain their uniform tagged carrier through later
  raw-scalar classification and comparisons.

- Documentation metadata scans leading header lines instead of splitting
  entire file bodies before discarding them. Header parsing preserves CRLF
  handling, early termination, and a final line without a newline. Search
  scoring uses allocation-free ASCII-folded substring scans and cached byte
  lengths.

- Inferred dictionary locals now refine dynamic mutation results to the object
  carrier at the store boundary, preserving pointer bits and keeping later
  call arguments consistent with store-class resolution.

- NYIR call resolution uses a bounded per-evaluation symbol cache, including
  unresolved source-function lookups before runtime dispatch. Exact names
  still take precedence over suffix aliases, and ambiguous aliases remain
  unresolved.

- Native layout field loads carry their declared scalar class, and inferred
  locals retain resolved field/callee carriers instead of being overwritten by
  a stale dynamic expression stamp. A rebuilt shared-core smoke preserves the
  nested resource field `53` through a local and integer return.

- Getter comparison normalization records decoded scalar operands before
  dictionary-read handling, preventing overlapping source classifications from
  decoding an odd success flag twice. The rebuilt shared-core parser smoke
  returns integer `8080` and sequence `["port=", 8080, ";"]` at offset `10`;
  CPS returns `42`.

- Private append-fill induction counters and empty integer lists can now be
  localized before AST loop construction, using collected callee references,
  increment-only counter updates, and whole-program list escape checks. Other
  scalar globals retain post-lowering promotion. This exposes reserve-once
  append filling to real compiled entry points, rather than only source-only
  IR dumps. Function-observed globals, address-taken storage, closure
  captures, and standard-library module storage keep their existing cells.

- Managed-object type and length queries now use the live allocation registry
  before foreign-pointer heuristics. GMP result export writes directly into
  the final BigInt payload instead of allocating and copying an intermediate
  limb buffer.

- Parallel benchmark completion notices are also emitted in captured logs
  instead of remaining TTY-only.

- Integer-shape assignment-site queries now detect active identifier cycles
  without allocating a second dependency graph. Local scalar recurrences no
  longer restart whole-program scans indefinitely.

- Native scalar shift lowering explicitly masks variable and out-of-range
  counts to six bits before inlining and verification. BigInt shifts retain
  their separate runtime path.

- Embedded `ny fmt --audit` keeps Ny batching for `.ny` inputs and uses the
  existing C collector for `.c`/`.h` inputs.

- The LLVM and NYIR runtime resolvers register the raw-i64 allocation helpers
  emitted by compiled core code.

- Documentation metadata collection now normalizes native path/string values
  before dynamic container storage and uses byte-safe substring extraction for
  routes, titles, and keywords.

- Native result-oracle assembly now emits each reachable function symbol once,
  preventing duplicate assembler definitions when the function pool contains
  repeated declarations.

- Safe-mode alias policies now recognize declarations parsed from the
  synthetic `<stdlib>` source as standard-library code, even when a token
  lacks the standard-library feature bit.

- Native vector arithmetic now uses the vector marker's dict dimension before
  testing the dynamic list predicate.

- Runtime allocation registries skip repeated safe pointer `mincore` probes
  for known handles while retaining validation for unknown pointers and ASan
  redzones. Native lowering uses amortized-growth string builders for
  loop-carried append and a raw integer tbuf helper.

- HM now recognizes `load_layout` as a known metadata-resolved intrinsic
  inside generic codegen templates.

- Standard-library lemma declarations are checked with the stable `auto` proof
  policy, independent of the consuming program's `--proof-solver` mode. This
  keeps `none` and `presburger` modes from rejecting unrelated imported
  standard lemmas.

- Certificate-index persistence now uses a raw-path `__rename_raw` runtime
  entry point, matching `__open_raw`'s native string and status ABI. The
  general dynamic-value rename wrapper had formatted native path pointers as
  `"<ptr...>"`, so `save` could report success while leaving only its
  temporary file. A direct rename smoke test now moves the source and leaves
  only the destination.

- Proof constant evaluation now resolves unqualified lemma calls through the
  current module's imports before scanning the flattened program. This avoids
  rejecting valid witnesses when unrelated standard-library modules contain
  same-named lemma declarations.

- Fixed native `file_read("/dev/null")` failures across NYIR, LLVM MCJIT, LLVM
  ORC, and native-only routes. OS raw-open callers now pass typed Ny strings
  directly to `__open_raw`; routing them through `__any_to_cstr` converted
  normalized native strings to pointer descriptions instead of C paths.
  `path.normalize` also returns already-normalized components without the
  miscompiled dynamic-list iteration path.

- `make test` supplies the freshly built standard-library bundle to every
  fixture child and prefers it over rebuilding the library from sources for
  each temporary fixture tree.

- Native BigInt classification no longer lets a same-named module global
  override an `any` parameter in a user callback. Global-initializer recovery
  is limited to the `rt_main` builder, where root globals are represented in
  its local table.

- Strict ownership return checking now follows local borrow aliases: returning
  `ref` after `def ref = &param` requires a matching `@returns_borrow(param)`.

- Native dictionaries now register their runtime-created payload handles.

- The 2M explicit dynamic-list append case now uses the raw integer tbuf
  helper when lowering proves the appended value is an integer.

- Consolidated strict static borrow checks into `lang/types/ownership.c` and
  its existing public header; removed the parallel versioned module, CLI mode,
  and codegen/cache plumbing. The unified checker currently rejects borrow
  values stored in collections and enforces `@returns_borrow(param)` for
  borrow returns. Allowances now accept `[lo,hi]` intervals with branch joins
  and unbounded loop upper bounds, while integer limits retain exact-count
  meaning.

- Standard-library source cache keys no longer depend on the requesting source
  path. Automatic fixture workers scale from available RAM with a 2 GiB
  per-worker budget; `ny-test` applies a matching memory guard.

- Generalized native loop-carried string-builder promotion from literal
  suffixes to a distinct string-local suffix.

- Integer-key lookups in the managed dictionary path now compare immediate
  values directly and avoid the generic string/pointer key comparator. This is
  a regression baseline; the absolute native set cost remains several seconds.

### 2026-10-10

- Fixed native lowering of the modular/finite-field polynomial coefficient
  helpers: `gfe` and `zmod` values are dictionaries, so `_poly_coeff_value`
  now reads their `"value"` field by key instead of ambiguous `.value`
  property dispatch. The property path had pulled an unrelated imported
  `#main` initializer into the helper and left `json_decode` unresolved in
  native-only JIT builds.

- Fixed `ny-test --bench --jobs N` so an explicit worker count controls the
  benchmark fixture pool; previously the benchmark pool ignored `--jobs` and
  used its CPU-based default unless `NYTRIX_BENCH_JOBS` was set. This restores
  isolated serial measurements with `--jobs 1`.

- Native proof folding now evaluates effect-free user functions over known
  scalar and zero-field enum arguments when their bodies reduce through
  returns, matches, and conditionals.

- The 2026-10-11 entry records the replacement resource-based policy and
  uncached failure-batch result.

- Unknown-type append calls now use the checked runtime append boundary
  instead of bypassing validation with raw typed-buffer writes.

- Reorganized the library tree into `lib/`, `lib/math/`, `lib/os/`, and
  `lib/compiler/{syntax,tools}/`; removed the empty `lib/core/` level and
  updated Ny module declarations, imports, aliases, compiler/loader mappings,
  examples, and fixtures to use `core.*` namespaces without legacy import
  shims. Regenerated `std.ny`.

- Added summary and keyword metadata to every one of the 530 library modules.
  `./make docs` parses 530 modules and 21,441 symbols and generates
  `build/docs/index.html`.

- Fixed repeated literal-key scanning for both single- and double-quoted
  dictionary keys by removing the crashing `chr()` call from the Ny scanner.

- REPL module loading no longer treats an active-use list as proof that a
  module's source is loaded; this preserves explicit imports after a `core`
  root import. Crypto's public `hex_encode` wrapper calls its exported binary
  helper rather than relying on an attached extension that is unavailable in
  that module context.

- `dict_clone` now reads the legacy dictionary's stored length directly on its
  non-native fallback, avoiding an unresolved inline-only `dict_len` symbol in
  JIT routes. Call-result container indexing now preserves the accessor's
  tagged physical representation when static typing knows a scalar element,
  preventing a second unbox before comparison.

- Dynamic operands now override an inferred BigInt arithmetic result in native
  lowering.

- Native proof lowering now evaluates a lemma invocation by substituting its
  compile-time positional arguments into the lemma proposition and folding the
  supported integer/Boolean fragment. Unknown arguments and ambiguous lemma
  names still fail closed.

- Async task constructors now have audited fixed `io|alloc` effect summaries
  and compose the callback's open effect row at the call site.

- Native ABI type lookup now recognizes top-level unit enums directly from the
  program AST when enum signature metadata has not yet been materialized.

- Made `^` bitwise XOR and `**` exponentiation throughout Ny parsing,
  lowering, and runtime paths; `^^`/`^^=` now alias power. BigInt,
  finite-field, and ring operators are declared in Ny. C-to-Ny and
  Python-to-Ny conversions, pure-expression evaluation, disassembly, and
  decompiler output use the new spellings; decompiler rendering canonicalizes
  `^^` to `**`.

- Replaced operator quirks documentation with the normal XOR/power contract.

- Counted sieve survivors eight at a time from the existing byte flags using a
  64-bit byte-lane reduction.

- Checked odd/even Euclid parity before Euler 9's perimeter division in both
  reference implementations.

- Factored the common `depth` addition out of both arms of the Ny and C
  `binary` benchmark recurrence.

- Reworked both Ny and C heapsort `sift_down` implementations to retain the
  root value and move children into the hole rather than swapping at every
  level.

- Fixed raw-buffer deallocation lowering: `free_raw` and `__free_raw` now
  resolve to `rt_free_raw`, preserving unboxed pointer ABI in compiler/runtime
  probes.

- Fixed copy propagation to refresh each rewritten call argument's ownership
  metadata from its resulting producer while preserving the independent formal
  borrow/consume contract.

- Restricted Ny extern-group analysis to `.ny` inputs. A C block comment with
  lines resembling `extern fn` declarations no longer produces a rejected Ny
  record or aborts embedded C audit collection; C full-JSON parity covers it.

- Kept formatter-analysis coverage on native-only, LLVM MCJIT, and LLVM ORC
  routes; the NYIR interpreter exceeded the fixture timeout on this scanner.
  Tightened fast-path guards in the analyzer, corrected the function-metadata
  column expectation, and removed invalid `--color=never`/incompatible sound
  fixture routes.

- Fixed two native benchmark correctness defects. Added the qualified
  `core.dict_mod.list` runtime alias so native heapsort resolves the list
  constructor.

- C still validates the bounded wire protocol and applies safe file edits.

- Moved unified `ny` command-token selection and alias classification into
  `core.compiler.tools.cli`.

- Migrated trivial `#main` wrapper detection (`NYAUD4902`) into the Ny audit
  batch and suppressed its embedded C duplicate.

- Removed the embedded C bug collector invocation after its Ny and C-family
  rules moved to the batched Ny scanner. Fixed dispatch-score decoding so
  boxed batch values do not become pointer-sized counts; independent branches
  no longer trigger false `NYAUD3002` findings, and `collect.ny` trim JSON
  matches the standalone output.

- Moved adjacent extern-group (`NYAUD5001`) collection into the same Ny batch
  and excluded the embedded C duplicate.

- Moved repeated dictionary-access layout findings (`NYAUD6001`) for C-family
  files into the Ny audit batch, then disabled the embedded C duplicate.

- Moved battery findings (`NYAUD2001`–`NYAUD2006`) for C-family sources into
  the Ny batch and disabled the embedded C collector.

- Moved C-family zero-argument property-method findings (`NYAUD8101`) into the
  Ny batch and excluded the embedded C duplicate.

- Enabled colored physical-register arithmetic in the x86-64 object encoder
  for destination vregs whose source has no usable register: the source moves
  through its trusted load channel (immediate rematerialization, sticky rax,
  seeded preg, or canonical home) straight into the destination register and
  the ALU applies in place, skipping the def-side register publish. Copy
  propagation now also folds scalar-class-preserving `COPY` contracts (the
  aliased source's definition already carries the same class), which removes
  the per-copy register publish + home store pairs that dominated hot loops;
  this benefits the Ny LLVM backend equally.

- Replaced Fannkuch's three dynamically indexed integer lists with typed `i64`
  buffers.

- Finished moving Fannkuch's permutation, flip, and rotation operations onto
  typed-buffer loads/stores, removing the remaining dynamic-list indexing.

- Marked `i64buf_load` and `i64buf_store` inline so hot typed-buffer clients
  can avoid wrapper calls.

- The optimized NYIR had cloned the static list at each round-constant access.

- Prepared PBKDF2's normalized HMAC key and inner/outer pads once per
  derivation instead of rebuilding them for each U value.

- Replaced Knight's per-recursion indexed move-list literals with arithmetic
  that derives the same eight `(dx, dy)` pairs from the move index.

- Replaced Euler 9's remaining `(a,b)` search with Euclid's primitive
  Pythagorean-triple enumeration, scaling only coprime opposite-parity `(m,n)`
  pairs whose perimeter divides 1000.

- Samples concentrate in `rt_main`; the disassembly shows repeated stack-home
  traffic in its hot loop. The current evidence does not justify a safe
  allocator/codegen change: the prior matched if-conversion comparison found
  only a noisy ~1.3% median difference and did not isolate this diamond. No
  unmeasured performance optimization was retained. Large native-versus-C gaps
  remain in several benchmark rows.

- CLI parity passes with the expanded parser protocol.

- Repeated literal-key dictionary reads (`NYAUD3005`) now cross the embedded
  boundary as complete Ny issue records.

- Repeated if/elif selector dispatch (`NYAUD3002`) now crosses the embedded
  boundary as a complete Ny issue record.

- File-pressure findings now cross the embedded boundary as complete Ny issue
  records: trim targets (`NYAUD3001`), hot files (`NYAUD1001`), and hot
  functions (`NYAUD1002`). Ny mirrors the C score inputs and repeated-line
  threshold; bounded full-JSON trim and bloat parity covers both trim
  triggers, score metrics, and hot-file/function counts.

- C and header sources now use Ny-computed pressure scores for hot-file
  (`NYAUD1001`), hot-function (`NYAUD1002`), trim-pressure (`NYAUD3001`), and
  append-loop builder (`NYAUD3004`) records.

- Fixed embedded analysis parity for adjacent declaration grouping by
  normalizing indentation on the following source line.

- Layout-boundary records (`NYAUD4101`–`NYAUD4105`), resource cleanup
  suggestions (`NYAUD4201`–`NYAUD4203`), table/key-map findings
  (`NYAUD4301`–`NYAUD4302`), repeated extern advice (`NYAUD4401`), and C
  diagnostic-rule suggestions (`NYAUD4501`–`NYAUD4503`), platform-module
  advice (`NYAUD4601`), Ny and C sibling-function families (`NYAUD4701`),
  derive-block candidates (`NYAUD4801`), and verbose direct-main guards
  (`NYAUD4901`) now come from the Ny audit batch. Embedded mode skips the old
  C metaprogram collector for Ny, C, and header files; standalone `ny-fmt`
  keeps it as the oracle.

- Adjacent variadic-`free` findings (`NYAUD4204`) now cross the embedded
  boundary as complete Ny issue records, including their original source
  column, count-bearing message, and cleanup suggestion.

- REPL `:doc` ambiguity lists and missing-name messages now render through
  `core.compiler.tools.doc.meta`, with caller-provided terminal styles. Source
  snippets now remove leading docstrings and tokenize/style their Ny syntax
  through `core.compiler.tools.doc.source`; C reports renderer failures
  without recreating these presentations, while interactive line editing keeps
  its C highlighter.

- Ny's audit batch now returns complete issue records for branch hazards,
  typed-nil returns, unreachable statements, stalled loop counters, literal
  arithmetic hazards, duplicate dictionary keys and case arms, overwritten
  assignments, frozen-dictionary mutation, discarded list updates, and all six
  battery heuristics.

- Ny now returns complete audit issue records for backend contract hints
  (`NYAUD6101`), dense literal tables (`NYAUD3006`), append-loop builders
  (`NYAUD3004`), and preallocated-list builders (`NYAUD3009`).

- The Ny audit batch now emits complete records for unused private helpers
  (`NYAUD7001`), adjacent extern groups (`NYAUD5001`), and internal profile
  helpers (`NYAUD7201`). Removed the duplicate C-side parser and message
  construction for these records and the migrated Ny bug rules.

- Loop guard findings (`NYAUD3007` and `NYAUD3008`) now travel as complete Ny
  issue records for both Ny and C/H inputs.

- Repeated dictionary-access layout findings (`NYAUD6001`) now include their
  final severity and text in the Ny audit record.

- Unused top-level constants (`NYAUD7301`) and dead key constants
  (`NYAUD7302`) now arrive as complete Ny issue records.

- Corrected `core.str.builder_append_byte` to append the requested raw byte
  instead of UTF-8 encoding it as a Unicode code point. This keeps bytewise Ny
  source formatting from corrupting non-ASCII literals.

- Moved embedded `ny fmt --check` structure-diagnostic rendering and semicolon
  fix-offset selection into the Ny analyzer batch.

- Ny now returns complete issue records for C-family constant branches
  (`NYAUD1102`), empty bodies (`NYAUD1103`), unsafe string helpers
  (`NYAUD1104`), duplicate conditions and operands (`NYAUD1108`–`NYAUD1112`),
  literal zero divisors (`NYAUD1115`), and negative allocation sizes
  (`NYAUD1118`).

- Moved the embedded C/H `NYAUD1103` empty-control-body finding into Ny's
  batched audit scanner. The standalone audit now carries C comment state
  across lines for this scan.

- Extended the Ny C/H scan to constant branches (`NYAUD1102`), repeated branch
  conditions (`NYAUD1108`), duplicate logical operands (`NYAUD1109`),
  self-comparisons (`NYAUD1110`), contradictory literal conditions
  (`NYAUD1111`/`NYAUD1112`), literal zero divisors (`NYAUD1115`), negative
  allocation sizes (`NYAUD1118`), and unsafe string helpers (`NYAUD1104`).

- Moved adjacent simple `free(...)` grouping (`NYAUD4204`) into Ny's batched
  audit scanner. It retains argument/column reporting, nested-call and list
  arguments, and ignores comma-separated, qualified, and quoted lookalikes.

- Moved duplicate dictionary-key (`NYAUD1113`), duplicate case-arm
  (`NYAUD1114`), adjacent overwritten-assignment (`NYAUD1117`), frozen empty
  dictionary mutation (`NYAUD1119`), and discarded list update (`NYAUD1120`)
  findings for Ny sources into the same scanner.

- Moved Ny literal zero-divisor (`NYAUD1115`), negative-index (`NYAUD1116`),
  and negative allocation-size (`NYAUD1118`) audit findings into the batched
  scanner. The C implementation remains the oracle for standalone `ny-fmt` and
  for C/H sources.

- Moved Ny branch assignment (`NYAUD1101`), constant branch (`NYAUD1102`), and
  repeated `elif` condition (`NYAUD1108`), duplicate logical operand
  (`NYAUD1109`), self-comparison (`NYAUD1110`), and contradictory literal
  conditions (`NYAUD1111`/`NYAUD1112`), typed-nil returns (`NYAUD1105`), and
  unreachable statements (`NYAUD1106`) and stalled while counters
  (`NYAUD1107`) findings into the batched scanner for Ny sources.

- Moved embedded C/H `NYAUD1104` unsafe string-helper findings into the Ny
  audit batch. Its lexical scan ignores quoted text, comments, and member-call
  lookalikes; the standalone C oracle now ignores inline comments for this
  rule.

- Moved embedded `ny fmt --analyze` C-family function range collection into
  `core.compiler.tools.fmt.analyze`. The Ny scanner skips prototypes and
  ignores declarations inside comments and quoted literals; C still backs
  standalone `ny-fmt` as the differential oracle. Embedded output now retains
  C duplicate name findings as well as long-function findings.

- Extended Ny-owned loop-shape audit findings (`NYAUD3007` and `NYAUD3008`) to
  `.c` and `.h` inputs. The embedded C collector now leaves these findings to
  the Ny batch for every scanned source type; standalone `ny-fmt` retains the
  C implementation as oracle.

- Moved both method-syntax audit findings (`NYAUD8101` and `NYAUD8102`) for Ny
  files into the batched `core.compiler.tools.fmt.analyze` scanner. It skips
  quoted literals and semicolon comments, recognizes qualified `bin.*`
  helpers, and keeps method-only and legacy-only modes distinct. The same Ny
  batch now collects `NYAUD3003` helper-indexed-write and `NYAUD3009`
  preallocated-list append findings for trim audits, plus
  `NYAUD3007`/`NYAUD3008` loop guard findings. The specialization scanner now
  owns all five `NYAUD3101`–`NYAUD3105` rules for Ny files, including
  compile-time branch/case analysis and multi-line literal-case thresholds. Ny
  also emits the per-file `NYAUD9001` type summary alongside its existing
  per-function type findings, and supplies the source-level legacy helper and
  receiver-call totals used by methods and legacy audits.

- Moved Ny `NYAUD6001` repeated-dictionary-access layout findings into the
  same batch scanner.

- Moved Ny `NYAUD7301` and `NYAUD7302` dead top-level constant findings into
  the batch scanner, including platform API exclusions and key-constant
  thresholds.

- Moved the `NYAUD6101` backend contract hint into the Ny batch for backend
  source paths.

- Moved Ny `NYAUD3006` dense-literal-table scoring into the batch scanner,
  preserving its bounded code-line scan, score threshold, first declaration,
  severity, and JSON summary counter.

- Moved Ny `NYAUD3004` append-heavy function checks into the batched scanner.

- Moved Ny `NYAUD7001` unused private-helper findings into the batch scanner.

- Moved Ny `NYAUD5001` adjacent extern declaration findings into the batch
  scanner, preserving blank/include/link continuation, bounded symbol samples,
  thresholds, and summary counters.

- Moved Ny `NYAUD7201` unused `internal(...)` helper findings into the batch
  scanner.

- Moved the six `NYAUD2001`–`NYAUD2006` battery findings for Ny sources into
  the batched scanner. The checks preserve neighboring-line windows and C
  string/comment recognition, while C retains the rules for non-Ny files.

- Completed full LLVM stdlib bitcode generation. The target now passes a
  whitespace-only source argument and keeps `--emit-module=std` so the bundle
  contains stdlib definitions without a synthetic `_ny_top_entry`. A
  module-pipeline compilation using `--std-bc=build/release/std.bc` succeeds
  and emits valid consumer bitcode (27,654,896 bytes).

- Moved embedded `ny fmt` argument parsing into
  `core.compiler.tools.fmt.cli.parse_args_batch`; the C parser remains the
  standalone `ny-fmt` oracle.

- Ported the standalone `.c` source formatter into
  `core.compiler.tools.fmt.format.format_c` and routed embedded `ny fmt --fix`
  batches for Ny and C files through one Ny session. `ny-fmt-format-parity`
  compares both CLIs on decorated and ordinary block comments, blank spacing,
  tabs, and CRLF.

- Ported the C-family header comment tidier into
  `core.compiler.tools.fmt.format.format_c_tidy`.

- Ported embedded `ny fmt --check` brace-balance and semicolon-swallow
  analysis into `scan_structure_batch`. Ny returns bounded diagnostics and
  exact semicolon offsets; the C shell applies those edits only after the full
  batch response validates. Structure parity also covers balanced braces
  inside string/comment text and an unmatched closing brace.

- Moved `ny fmt --analyze`'s unbounded C string-helper finding into the
  batched Ny analyzer. It checks `.c` and `.h` lines while ignoring strings
  and `//` comments, returns the `unsafe_c` count with Ny's other analysis
  stats, and retains byte-for-byte output parity with standalone `ny-fmt`.

- Bounded Ny formatter long-function analysis to the 121 lines needed to
  decide the existing 120-line threshold.

- Moved Ny function, public-function, missing-doc, duplicate-name, and
  long-function summary counts into the batched Ny analyzer. Embedded `ny fmt
  --analyze` no longer reparses Ny functions in C; C continues to scan
  C-family functions and remains the standalone comparison implementation.

- Moved audit's Ny function signature scan into
  `scan_function_metadata_batch`. A bounded single-threaded Ny prepass emits
  function ranges, visibility, docs, parameter/return metadata, and inference
  directives before the audit workers start. Ny now also emits typed parameter
  and return findings from those rows; the standalone C implementation remains
  the parity oracle.

- Native lowering now derives a top-level scalar global's storage ABI from its
  declaration, not the current assignment's RHS.

- Inferred mutable globals initialized with signed integer literals now keep
  the raw integer ABI on reads as well as writes.

- Native NYIR call resolution now strips the generated `ny_fn_` prefix before
  consulting runtime aliases, and the qualified dictionary-length wrapper has
  the same raw runtime mapping as its unqualified form.

- Native call lowering now keeps a same-source user function ahead of an
  imported same-name stdlib function.

- Mutable top-level lists initialized from literals no longer qualify for the
  immutable raw-index equality shortcut.

- Mutable list append calls now copy aliased local buffers before updating the
  receiver slot.

- Native scalar comparisons now preserve raw integer payloads from immutable
  homogeneous tuple indexes and byte indexes.

- Socket async operations now use the runtime's `__errno()` builtin.

- The shape test runner now ignores braces in quoted metadata, comments, and
  embedded source heredocs while finding fixture boundaries.

- Shape source-tree materialization now skips sibling fixtures without an
  embedded Ny block instead of failing the entire tree.

- Native lowering preserves the BigInt representation when a wide literal is
  aliased through another binding.

- Dynamic-list member `get` lowering now uses the raw-index tbuf accessor
  instead of decoding raw loop indices as tagged integers. A reduced async
  accumulation now reads `1, 2, 3, 4` and sums to `10`.

- Mutable globals initialized from dynamic-returning calls now retain their
  tagged ABI across reassignment, including negative integer results.

- Guarded native caller-module resolution so a name-only fallback cannot
  replace a semantically resolved function with a declaration that rejects the
  call arity.

- Documented Nytrix embedding and the standalone Nyr frontend API directly in
  `docs/spec/nytrix.md` and `docs/spec/nyr.md` with raw C blocks, including a
  small Lisp-to-NYIR frontend. Removed standalone demo sources and targets;
  retained API and GC-handle regression tests. Test fixtures are grouped under
  `etc/tests/code`, `etc/tests/lang`, and `etc/tests/bench`; backend code
  fixtures sit directly in `code/`; NYIR and source-lowering fixtures use
  `code/intermediate/`; REPL, stress, cache, and tools probes use
  `code/probes/`. Derive fixtures are grouped by behavior. Removed redundant
  cache subdirectories and the standalone lowering folder, updating fixture
  routing, CMake references, direct paths, and documentation.

- Moved `ny fmt --cloc` collection, source-line counting, Git diff
  aggregation, ranking, and JSON/text rendering into
  `core.compiler.tools.fmt.cloc`; `ny-full` calls the Ny entry point through
  `nytrix_embed_run_source`, without a compiler subprocess. The standalone
  `ny-fmt` C implementation remains the differential oracle and is excluded
  from `ny-full`. Fixed `nytrix_embed_run_source` to enable JIT execution; the
  embedded example now exercises a Ny tool function rather than merely
  compiling it.

- Routed REPL `:doc` exact-name, dotted-suffix, basename, and ambiguity
  resolution through `core.compiler.tools.doc.meta.resolve`. Ny
  `doc.meta.render_entry` now composes the entry heading, signature,
  documentation body, colors, and missing-doc fallback; C retains
  ambiguity/miss messages and optional source syntax highlighting.

- Routed unified `ny doc search` through the Ny ranking library in-process. A
  live `ny-doc` versus `ny doc` search for `parser` produced byte-identical
  output during the cutover check.

- Routed Markdown section extraction and production documentation search-index
  row construction through `core.compiler.tools.doc.index`. C keeps the
  filesystem walk and a cheap heading-candidate scan; Ny validates headings,
  computes section spans, and emits the rows consumed by the C index adapter.

- The embedded portal collects Markdown bodies and renders them in one Ny
  batch, keeping compilation out of the per-page loop; standalone `ny-doc`
  retains the C renderer as the comparison path. Replaced per-line UTF-8-wide
  slicing with byte-range slices, and replaced per-byte HTML hex output with a
  length-prefixed raw batch result. The docs bundle parser now advances a
  cached lexical brace-depth cursor instead of rescanning each source prefix
  for every declaration. A UTF-8 and many-line regression joins the existing
  HTML/route fixture.

- Routed embedded `ny fmt --fix` and `--tidy` rewriting of Ny files through
  `core.compiler.tools.fmt.format.format_ny` in one in-process batch. The
  bridge hex-encodes source text through bounded temporary payloads, validates
  every result before applying edits, and leaves C-family formatting on the
  existing path.

- Routed human issue-row rendering in `ny fmt --analyze` and `--audit`, plus
  `ny fmt --analyze --json`, through `core.compiler.tools.fmt.report`. C
  continues to collect, filter, and enrich issue records, while Ny composes
  locations, severity colors, audit metadata, notes, JSON schema, and limit
  summaries. `ny fmt --analyze` and `--audit` summary rows also use Ny's
  aligned, wrapped key/value renderer, and audit JSON uses the Ny JSON
  serializer. C continues to collect and enrich audit records.

- Moved the embedded analyzer's shared trailing-whitespace, tab, and
  TODO/FIXME scan into `core.compiler.tools.fmt.analyze`, then moved
  visual-width counting, compact-line exemptions, import-style findings, and
  compact `return(...)`, control-block spacing, repeated-append chains, and
  repeated `mut` markers; reversed fields in `layout` blocks now use the same
  bounded batch, as do adjacent declaration and assignment grouping
  suggestions. Ny also detects oversized function bodies, duplicate public
  names within a file, and missing public stdlib docstrings; it recognizes
  preceding documentation comments, body docstrings, and simple forwarding
  wrappers. Ny now supplies function metadata to embedded audits and aggregate
  counts.

- Routed Ny-source `ny fmt --optimize --diff/--apply` through the embedded Ny
  formatter batch used by `--fix` and `--tidy`.

- Removed the lexer’s whole-prefix scan for semicolon comments at ordinary
  parenthesis depths. Tracked delimiter state preserves `for`/`if`/`while`
  initializer separators; nesting beyond the inline tracking depth retains the
  previous source scan. A capped 25-second stdlib probe now reaches lowering;
  the pre-fix profile spent 59% of its samples in `lexer_next`.

- Closed the P2 aggregate-ABI gap in the self-contained C frontend. `nyc` now
  lowers struct/union values end to end: by-value parameters and arguments
  (register-class, MEMORY-class, and mixed INTEGER/SSE with the shared SysV
  register counters), register-class returns through the two-eightbyte RET
  pieces, MEMORY-class returns through the hidden caller buffer, struct locals
  and arrays with zero-filled storage and member stores, and aggregate
  assignment through `NYIR_COPY_STRUCT`. Function-entry ABI travels as
  `NYIR_PARAM_AGGREGATE` plus packed `param_abi` words and
  `return_aggregate_size`/`return_aggregate_classes`, serialized in
  `NYIR_BINARY_VERSION` 19 and validated by the NYIR verifier.

- Fixed two optimizer/codegen defects uncovered by that work, both reachable
  from ordinary O2 compiles. `nyir_func_replace` zeroed the replacement's
  `param_count` whenever `param_abi` was NULL, so later passes treated
  parameter slots as uninitialized locals and folded parameter loads to zero
  (loops collapsed under `iv_elim`); and the x86-64 object writer lowered
  `NYIR_COPY` for float values through the integer spill path even when the
  value was register-resident, computing a zero frame displacement and
  returning garbage.

- Its temporary input path is unique per invocation, and the parent only
  accepts the rewritten source after successful completion.

- Extended comptime caching per the roadmap: closed immutable-global name
  references (keyed by binding identity), `len`/`range`/`range2`/`Z` calls,
  and member `.get` forms are memoized with shadowing checks; target queries,
  environment reads, and unresolved calls stay uncached.

- Finished read-only compiler reflection. Dynamic dictionary index callbacks
  now carry their joined builtin and custom effect rows through local aliases,
  matching existing list/tuple index and `.get` flows; mutation still
  invalidates stale container facts. The live-field Ny derive templates and Ny
  compound-assignment rewrite both have C-reference differential fixtures.

- Cut production search ranking over to the Ny `core.compiler.doc_rank`
  library and verified ordered-result parity across a ten-query matrix over
  docs/lib corpus, including alternatives, private-name penalties, and
  zero-match queries.

- Completed the ranking cutover: removed the legacy C scorer and
  `NYTRIX_DOC_C_RANK` switch.

- Reworked `ffi.gen` scanning to classify bytes and slice whole token,
  declaration, and parameter spans instead of allocating a string per
  character.

- Replaced the standalone Python derive AST parity checker and its six
  script-only `.ny` inputs with `derive-templates-parity.nshape`. Removed the
  Python CTest hook. Removed the unused `doc-rank-parity.cmake` harness;
  `doc-rank.nshape` remains the direct test for the Ny ranking library.

- Completed the bounded comptime expression memo's semantic keying. Immutable
  globals resolve to binding identities; closed receiver expressions and
  positional arguments participate structurally; pure user functions include
  their resolved declaration identity and evaluate through the bounded
  interpreter. Target queries, `__main`, and registered refinements stay
  uncached because they depend on compiler or typing context; random,
  effectful, and unresolved calls are uncached as well. Each parallel module
  worker owns an independent memo.

- Fixed intrinsic dispatch when standard-library `range2` is present and
  preserved user shadowing for `range2`. The comptime-caching TODO item is
  closed, and the cache's supported scope and exclusions are documented in the
  comptime spec.

- Comptime string `.get(index[, default])` now resolves closed string literals
  in the LLVM fast evaluator, counting UTF-8 code points and supporting
  positive and negative indexes.

- Trimmed the open comptime-caching TODO to its remaining acceptance gaps;
  landed closed-form evaluation and its regression coverage remain documented
  here.

### 2026-10-09

- Specified the accepted `requires`/`decreases` proof calculus and closed its
  bounded acceptance item. Unsupported or inconclusive formulas, calls,
  predicates, and measures remain `unknown` and fail closed; the contract does
  not claim completeness for nonlinear arithmetic or arbitrary termination.

- Plain `parse_ast` records now include declaration attributes, so compiler
  reflection sees the same attribute names as versioned snapshots. GPU
  platform checks use the exported `platform.is_windows()` and
  `platform.is_macos()` APIs. NYIR local stores now materialize an explicit
  object-class copy when an aggregate-class producer enters an object-handle
  slot; this keeps imported nominal values consistent through method and
  operator calls.

- `sound.get_backend_name()` now reports `"none"` whenever initialization has
  no active context, including an empty stored name.

- `swapped` now uses the existing append-based copy path for empty and
  single-element lists.

- Explicit `int` initialization from `.long` now avoids decoding values
  already emitted as raw integers, including comptime constants.

- Dynamic ternary joins now box raw scalar arms whenever either arm's
  arithmetic depends on `any` inputs.

- Arithmetic consuming `list.get` now follows the lowered result's scalar ABI:
  a tagged dynamic result is decoded even when its receiver is statically
  declared as a list.

- Native dictionary `merge` calls now preserve the receiver's scalar ABI
  because the runtime returns that same receiver. The focused Pollard module
  emits valid LLVM bitcode after this correction.

- Added versioned effect-row module interfaces with exact imported-signature
  matching and per-call substitution of known closed callback rows for
  imported open rows. Shared signatures remain unchanged; unsupported callback
  provenance fails closed. Relocated the derive parity Python test out of
  `etc/tests/language/compiler/` and removed that directory's bytecode cache.

- Consolidated NYIR backend tests under `src/code/backend/test/`: optimizer
  drivers are in `opt/`, and the public IR acceptance test is in `ir/`.
  Updated CMake, the generated registry include, command includes, and testing
  documentation. Merged the public evaluator API wrappers into `eval.c` and
  removed `eval_api.c`. The full optimizer registry currently fails in
  `runtime_call_contracts` with `runtime lookup mismatch for
  rt_f64_from_bits`.

- A completed full-bundle attempt then exposed a NYIR call-class mismatch in
  `core.math.gf.solve_gf2`: `rt_len` received a tagged indexed-list value
  while the call retained an aggregate source-formal class. The runtime-call
  emitter now records the physical carrier class for polymorphic `rt_len` and
  `rt_len_strict` calls.

- Refinement guard typing now asks the bounded overflow-aware contract solver
  to prove supported pure relational implications, including stronger lower
  bounds and conjunctions. Candidate specificity now instantiates both
  predicates on the guarded subject before solver comparison, selecting
  `Positive` over its implied weaker `Nat` predicate instead of rejecting the
  guard as ambiguous. Formal matching and solver substitution preserve each
  predicate's syntax context. Versioned refinement interfaces now serialize
  qualified identities, base/formal names, and supported canonical predicates;
  imports reconstruct predicate ASTs into the compiler arena and reject
  malformed, incompatible, duplicate, or unsupported records. Canonical AST
  roundtrips cover relational/logical, float, and escaped-string predicates.

- Pure predicate inlining for `requires` now composes multiple sequential
  no-`else` early-return guards into one exact formula.

- Pure predicate expansion now follows immutable local callable aliases and
  specializes a callback formal only when the actual target resolves to a pure
  function. Finite `match` predicates also lower closed integer-literal range
  patterns with inclusive endpoints.

- Pure predicate expansion now supports a single immutable `if` initializer,
  substituting its value into the guard and both branch results.

- Pure predicate expansion now lowers closed match expressions nested in
  supported boolean expressions.

- Closed integer-range patterns also lower in statement-form pure matches.

- Sound backend startup now passes the connected backend context directly into
  stream selection instead of relying on its module-global copy, and device
  list reads reject non-list values before taking their length. On the hosted
  PulseAudio workstation, `sound.init(false)`, generated sine-source
  `sound.play()`, and shutdown completed with real PCM output from an imported
  synthesized WAV resource. The facade reports the active backend and resets
  that name on shutdown. ALSA PCM handles are now explicitly unboxed at the C
  boundary, and ALSA's silent `null` candidate was removed so a missing device
  fails instead of claiming playback success. With an invalid PulseAudio
  server, initialization now returns false without crashing in
  `snd_pcm_set_params`.

- Attached-method re-export coverage now includes a nested consumer module
  with a same-named free function, local method-name value, imported-module
  alias, and nominal type name; receiver dispatch remains owner-qualified on
  all four routes.

- Completed backend API isolation: LLVM emission/JIT adapters, source-program
  entry points, runtime-aware NYIR evaluation, and the AST x86-64 fallback are
  language-owned; runtime source manifests and runtime build orchestration
  moved to the language pipeline, with process spawning in the base layer.
  Backend JIT symbol lookup, mapping registration, VM evaluation, and profile
  collection use injected runtime services. The neutral x86-64 NYIR encoder is
  included in the backend-only `nyr` library; CMake rejects direct frontend
  includes in that source set. Shared native buffer layout constants live in a
  neutral public header.

- Affine loop interchange now supports write-only accesses to external pointer
  roots when every stored address retains the frontend's in-bounds proof and
  affine analysis proves an injective iteration map. Different external roots
  remain potentially aliased and are rejected; memory reads and unproved
  addresses remain barriers. `loop_interchange.c` now checks loop behavior,
  including external buffer contents, aliasing, bounds traps, and overflow
  outcomes, without IR-length or bound-layout assertions.

- Native x86-64 code generation now keeps canonical stack homes for functions
  with carried scalar machine values, avoiding O3 loop-state corruption.

- Centralized built-in vector alias recognition in the language type layer and
  routed LLVM and native lowering type checks through its semantic dimension
  query. The backend no longer carries its own `vecN`/`VectorN` name tables.

- The chosen operator target remains stable on NYIR, MCJIT, ORC, and
  native-only (4/4).

- The LLVM NYIR emitter now receives a neutral backend module and an
  `ny_llvm_context_t` with injected source-origin, debug-scope, and global
  symbol resolvers. Program lowering and the codegen-to-backend context
  adapter moved to `src/code/lang/pipeline/native.c`; the compiler pipeline
  and REPL call the language adapter. The neutral NYIR lowering declaration is
  exposed from `src/code/lang/lower.h`. The LLVM emitter, language adapter,
  pipeline, and REPL translation units compile; a release `ny` build
  subsequently linked successfully after the source-program API move.

- Refinement guard matching now canonicalizes operand-reversed integer
  comparisons, including their complements in the opposite branch.

- Mutable refinement subjects may now carry a guard fact when both the branch
  and the enclosing function are proven read-only by the AST checker. The
  function result is computed once when entering HM inference. It rejects
  assignments, indirect memory operations, closures, calls, deferred bodies,
  and unknown syntax; mutable transport is limited to scalar representations.
  Writes, captured mutation, and aggregate/object aliases remain fail-closed.

- Extended comptime expression memoization to structurally closed `.get` calls
  whose receiver is a dictionary literal and whose one or two positional
  arguments are closed expressions. The call key includes the dictionary,
  method, and argument structure; named arguments and nonliteral receivers
  remain excluded.

- Added opaque `void *` function parameter and return support to the
  standalone C frontend.

- Added interleaved seven-integer/nine-double definitions and direct calls to
  exercise independent integer-register and XMM-register exhaustion. The
  generated objects matched host-C references for both entry arguments and
  forwarded calls.

- The closed-expression comptime memo now evaluates and owns list/tuple slices
  with omitted or explicit starts/stops, negative bounds, and positive or
  negative steps; step zero remains unsupported.

- Native reachability now follows the canonical operator declaration selected
  by shared typing. This retains module-scoped operator targets that are not
  nested in `impl` blocks. Native lowering resolves the selected module target
  from the function index and carries its canonical declaration into the call.

- The nested chained re-export fixture additionally combines method-name,
  free-function, module-alias, and nominal-type shadows.

- Added public opaque `nytrix_value_handle_t` roots for raw Ny words. Root
  slots survive moving collections; reads synchronize with the collector and
  writes use its barrier. `nytrix-embed-gc-handles` forces a major collection
  and verifies the rooted object remains live.

- Moved source/file embedding orchestration and its process-wide compile guard
  from the NYIR interpreter implementation to
  `src/code/lang/pipeline/embed.c`; `src/code/backend/ir/embedding.c` now owns
  the backend interpreter entry point.

- Renamed the internal backend target header to `src/code/backend/target.h` and
  updated the embedding example include sites.

- Refinement guard transport now recognizes a declared predicate that exactly
  matches either operand of a positive `&&` guard, and an exact complemented
  predicate in either operand of a false `||` arm. Logical `!` carries the
  selected guard polarity through the same exact matcher. It does not infer
  facts from disjunction true arms or conjunction false arms.

- Exact relational refinement transport now searches registered refinement
  declarations from loaded modules as well as the importing source tree.

- Loop unswitching now handles an invariant one-sided branch through an
  unlabelled branch-only exit trampoline when the loop has a proven positive
  trip count and its header and condition-block prefix are pure control. The
  optimizer dispatches to the existing loop header or the resolved exit and
  preserves the selected internal body edge. `loop_unswitch` verifies both
  branch outcomes and the zero-trip boundary.

- Refinement flow transports a source refinement from an exact alpha-matched,
  side-effect-free literal/formal relational predicate on an immutable local,
  in `if`, `while`, and C-style `for` true bodies. Positive conjunctions may
  contribute either exact operand; false disjunction arms may contribute
  either exact complemented operand. Facts are restored at branch/body
  boundaries. The matcher rejects calls, unrelated identifiers, unsupported
  logical implication, and ambiguous refinement matches. Mutable bindings
  remain unsupported until alias and write invalidation are tracked.

- Attached-method coherence now rejects repeated method names on one owner
  even when declarations have different arities. The compiler does not yet
  implement arity-based attached overload dispatch, and previously such
  declarations could survive validation only to resolve by signature-table
  order.

- Moved the embedding examples and their CMake registration to `src/examples`,
  keeping repository demos outside compiler implementation. Updated all build,
  status, spec, and changelog paths.

- Split callgraph input into a borrowed NYIR module view and neutral callgraph
  options, and moved target selection/layout records into `nyr/target.h`.
  Backend target discovery now consumes `nyr_target_request_t`; source options
  translate into that record at the boundary. Program-to-IR callgraph
  orchestration now lives in the language pipeline, leaving the backend report
  entry IR-only.

- Completed the broader CFG translation-validation acceptance. Also repaired a
  work-tree regression in `mach_build_imm_fold`, which returned false on
  success and pushed every constant-bearing function to the legacy NYIR object
  writer.

- Landed safe f64 row/column vectorization for the nested tbuf address shape
  (`payload + row_bytes + column * 8`). The vectorizer's address decomposition
  now peels nested `ADD_I64` row bases outside the candidate loop, keeps the
  dynamic bounds proof (guarded min/max over the full scalar induction range,
  which bounds every widened lane because the vector body stops at `vec_end -
  VF` and a scalar tail covers the remainder), and keeps the dependence rules
  intact: a dynamic-offset write still rejects every other access on its root,
  and cross-root independence still requires the swap-pair proof. The x64
  encoder gained the missing halves this needed: vector live ranges now
  allocate (V256 types were missing from the vector class filter, so every
  vector value took the scratch path), vector colors moved to xmm8-xmm13 with
  the scalar float allocator clamped below xmm8 so both XMM populations
  coexist, fully-resident SIB addressing serves widened loads and stores
  (`vmovdqu ymm, [base+idx*8]`), and V256 copies broadcast immediates and
  scalar SET1 sources instead of copying adjacent stack bytes into the upper
  lanes. A partial-residency SIB variant (colored base, RAX/R10 scratch index)
  miscompiled stencil loops and was backed out; it is documented as a TODO
  constraint in `docs/TODO.md`.

- A chained module re-export regression exposed imported nominal types
  retaining their short alias while method signatures carried the declaration
  identity. Type-name resolution now follows the import/re-export alias chain
  for known layouts. An earlier module-owned enum-constructor attempt exposed
  an unrelated native NYIR lowering limitation and was not retained.

- The LLVM comptime fast evaluator's bounded structural memo now owns and
  clones cached values, including allocated strings and nested dictionary
  values, and frees them with the codegen context. Closed dictionary literals
  support comptime `.get(key[, default])` lookup, including last-write
  behavior for duplicate keys and explicit or default-zero missing-key
  fallbacks; the receiver literal is structurally memoized. Closed UTF-8
  string indexing counts code points; list/tuple indexing supports negative
  sequence indexes, and dictionary indexing evaluates keys structurally.
  Slices and invalid indexes use the existing fallback. Closed equivalent
  expressions in distinct AST nodes reuse results at the same evaluator depth.

- Fixed runtime C compilation after the parser AST began including the public
  Nyr IR header: runtime compile commands now include `src/include`, and the
  runtime object cache tracks `src/include/nyr/ir.h` with a bumped cache
  revision.

- Closed the Ny-owned reflection/codegen cutover item for the existing
  compile-stage generators. Live compiler reflection and rewrites over live
  compiler state remain under the read-only reflection item.

- Native attached-method lowering now trusts the canonical declaration
  selected by shared type validation. The direct-call AST fallback and the
  second receiver/member lookup are removed, so an unvalidated member no
  longer silently lowers as an attached method.

- The C lexer, parser, and Clang bridge now live under `src/code/lang/c`, and
  the obsolete `src/code/lang/ffi` directory is gone. Backend example
  verification is blocked by an unrelated compile error in
  `src/code/backend/object/x64.c:4422` (`mach_vector_types_present`
  undeclared).

- Extended the public Nyr API to emit x86-64 COFF and Mach-O objects for an
  explicit target triple; `nyc --target=...` exposes the same target selection
  without requiring the `ny` CLI or LLVM.

- Kept two QS accumulators in the raw-integer lane: the target-prime-bound
  growth step and byte-sieve maximum score now use typed integer intermediates
  and explicit comparisons instead of generic `min`/`max` results.

- The source compilation pipeline driver and its cache, stage, and tiny-path
  fragments now live in `src/code/lang/pipeline`; its entry header is
  `src/code/lang/pipeline.h`. The pipeline coordinates parsing, semantic/type
  stages, and backend invocation, so it belongs with the language compiler.
  The NYIR verifier and two NYIR optimizer passes also dropped unused parser
  AST includes.

- Pure user-function call sites now retain the required `CALL` effect bit when
  the inferred transitive effect summary is empty. The missing tag prevented
  bounded recursive constant evaluation from recognizing `gcbench`'s
  allocation-free optimized `tree_count` body.

- VRP now models `rt_argc`'s tagged result as `(argc << 1) | 1` for C's
  nonnegative `int argc` range. This proves `argc()`-derived positive divisors
  nontrapping, so LICM hoists `licm-math`'s invariant signed division from its
  million-iteration loop.

- `core.compiler.tools.doc.rank.rank` now computes each matching entry's fuzzy
  score once and carries it through stable insertion sorting. Sorting no
  longer reruns fuzzy matching for every comparison; score, kind, full-name,
  and source-order behavior are unchanged.

- `bigint-oracle` now has a GMP C reference for its add/subtract/multiply/
  divide/sqrt/power workload. Parallel benchmark workers no longer print
  out-of-order completion rows by default; `NYTRIX_BENCH_PROGRESS=1` opts back
  in.

- Fixed the `@thread` argument-count transport at the spawn boundary: the
  attribute-call lowering emitted the ABI argument count tagged (2n+1) while
  `rt_thread_prepare_call_args` validates it as a raw scalar, so every call
  with eight or more arguments failed in `pthread_create` preparation and the
  value-position join crashed on the -1 handle; counts of seven or fewer were
  dispatched with doubled-plus-one arguments and read past the caller's
  argument buffer. The attribute path now emits the raw count, matching the
  list-form `thread_spawn_call` lowering and the declared runtime ABI, and
  `rt_thread_join` rejects non-positive handles as the declared invalid-handle
  error instead of invoking `pthread_join` on them.

- Fixed the executable entry contract in `nyr_build_rt_main`: the call to a
  user `main` was emitted only when no top-level statement had left a tail
  value, so any trailing module-level statement (including a plain `def`)
  suppressed `main` entirely and — for a trailing `def` — returned the
  initializer's value as the process status. The entry now always calls a
  user-defined `main` after top-level statements and returns its value; a
  module-only unit without `main` keeps its no-op entry.

- Direct immutable refinement guards (`if Refinement(value)`) now establish
  the nominal refinement only in the true arm. The fact follows same-typed
  local aliases and is discarded after the arm; mutable bindings are not
  narrowed. Refinement predicates are classified as pure by effect analysis,
  so a guard does not make an otherwise effect-known function unknown.

- Added `core.math.checked` with checked signed-64-bit add, subtract,
  multiply, divide, and negate operations plus saturating
  add/subtract/multiply. Checked operations return `Result` values with
  `err.overflow`, `err.div_zero`, or `err.type`; exact BigInt intermediates
  avoid relying on wrapping machine arithmetic.

- Refinement returns now fail closed in shared HM typing when the returned
  value is neither already of the declared refinement type nor proved by the
  compile-time predicate evaluator.

- Shared operator analysis now attaches the uniquely selected operator
  declaration to the expression, and native lowering consumes that identity
  before its compatibility AST walk.

- Clarified heap-policy tradeoffs using Baker's *Lively Linear Lisp*: tracing
  GC remains optional because the paper's no-tracing result depends on
  single-use values, no visible sharing/cycles, and explicit copying; its
  optimized shared heap still uses reference counts and hash-consing.
  Documented Nytrix's manual, RAII, RC, and GC policies and corrected the
  status summary.

- Grouped compiler-facing libraries under `lib/compiler/`: typed syntax and
  quote APIs moved from `core.ast` to `core.compiler.ast`, and Ny code
  generators moved from `core.codegen` to `core.compiler.codegen`. Updated
  imports, fixtures, tooling references, and guides. Fixed the formatter
  collector self-check's stale source paths.

- Added `core.compiler.tools.doc.rank`, a pure Ny implementation of the
  current documentation fuzzy scorer, field weights, query alternatives, and
  stable score/kind/full-name ordering. C still owns production ranking;
  differential scoring over the real search index remains required before
  switching the CLI.

- Matched C's private-function scoring when an entry has an unqualified
  `full_name`: the private-name penalty uses the function's `name`, as the C
  search does when no qualified leaf exists.

- Made C documentation search ordering deterministic for entries tied on
  score, kind, and full name by using index insertion order as the last
  tie-break.

- Closed the `@thread` default-argument ABI hole: NYIR now rejects calls whose
  expanded ABI exceeds 15 arguments with the callee-specific diagnostic and
  source location, before spawn lowering; the LLVM path rejects oversized
  signatures before substituting defaults.

- Preserved integer-index ABI for dynamic `.get` calls when the surrounding
  numeric context widens an integer-only index expression to f64.

- Kept dynamically extracted decompiler jump-table entries on the boxed `any`
  ABI when passing them through state-case helpers; those values are lists at
  runtime but are not statically typed NY lists.

- Canonicalized f32 values to the descriptor-backed list's f64 payload ABI
  instead of sending f32 bits through the integer tag bridge.

#### Standalone Nyr C compiler path

- Split the Nyr VM-facing wrappers and static-array lookup into separate
  translation units. The Linux x86-64 `nyc` executable can now link the static
  Nyr library without pulling the optional runtime handle-registration symbol
  or LLVM/libclang libraries; VM evaluation returns no static-array address
  when that runtime hook is unavailable.

- Normal compiler builds retain that profiling.

- Added function/data sections and section garbage collection to the
  standalone static compiler link so unused optional API paths do not add
  runtime dependencies.

- `ldd` on that `nyc` showed only libc, libm, and libgomp (no LLVM, libclang,
  or Ny runtime).

- The C frontend still lacks aggregate support, broader grammar/ABI coverage,
  and CFG-aware native backedge liveness; this is not a full-C or LLVM
  replacement claim.

### 2026-10-08

- Non-LLVM CMake builds now omit the LLVM language and backend implementation
  sources from `nytrix_compiler`. The standalone Nyr library / C compiler
  target is still not available in this configuration.

- The Nyr-backed C frontend now accepts C empty statements (`;`) in compound
  blocks and as control-flow bodies.

- C block declarations may now contain multiple declarators with independent
  initializers, including declaration-before-assignment locals. Executable
  host-C parity covers initialized and uninitialized declarators in one
  declaration; `for` initializer declarations support multiple loop-scoped
  declarators as well.

- Organized the compiler-interfacing Ny libraries under `lib/compiler/`:
  `fmt_collect`, `test_collect`, `doc_meta`, and `doc_rank` moved there with
  module paths `core.compiler.*` (matching the existing
  `core.reflect.semantic` subdirectory convention), and the five fixtures plus
  the fmt/web/test CLI Ny-program strings were updated to the new paths.

- Completed the frontend rename `ny_native_nir_*` → `nyr_*` (388 symbols,
  including the `NY_NATIVE_NIR_*` fact macros → `NYR_*`), unifying with the
  public `nyr_` builder API the concurrent migration had already introduced
  (`nyr_builder_t`, `nyr_fail`); no collisions — both targets were the same
  typedef/helper bodies.

- Merged `src/code/backend/llvm/string_init.c` into `strings.c` (its only
  external symbol `codegen_emit_string_init` is declared in `priv.h`; the rest
  were file-local), completing the backend/llvm consolidation.

- Fixed documentation header parsing and collection: title extraction now
  survives keyword normalization, and keyword scanning continues across
  leading `;;` title comments.

- Preserved the enclosing effect contract while summarizing closure bodies.

- Pure predicate proof expansion now follows immutable, unshadowed global
  function aliases. Four-route fixtures prove the positive caller and continue
  rejecting a caller without evidence for the predicate's precondition.

- Native runtime ABI metadata now preserves typed-buffer result classes for
  append, extend, reserve, clear, set, swap, and concat operations.

- Dictionary lookup ABI metadata now boxes dynamic fallback arguments while
  preserving typed-buffer fallback values. Nested float defaults and integer
  keys pass across NYIR, native, and both LLVM JIT engines.

- GLTF component and animation readers now widen binary32 loads to f64
  explicitly.

- CVP beam states now keep costs and coefficient vectors in parallel lists,
  avoiding an aggregate-to-typed-buffer call mismatch while retaining aligned
  beam candidates.

- Disassembler target parsing accepts an explicit row base for operand
  strings, preserving RISC-V relative target calculation without passing
  heterogeneous row aggregates through dynamic call boundaries.

- Effect-row v1 decoding now reconstructs closed nested generic applications,
  arrows, rows, numeric or symbolic `Fin` payloads, pointer/nullable and
  `struct`/`enum`/`layout` spellings, and non-atom concrete names through the
  `concrete#` hex form. The design note keeps effect rows separate from
  authority; they describe effects and grant no permissions.

- Fixed a native-lowering compile error by keeping the resolved callable value
  in scope for both local and global lambda lookup paths.

- Renamed selected native lowerer operation, constant-folding, failure,
  comparison-emitter, fact, and loop-frame symbols to the compact `nyr_`
  namespace. This is an internal symbol cleanup; emitted NYIR and runtime
  behavior are unchanged.

- Extended the public Nyr C API to version 1.1 with `nyr_eval_with_calls`,
  exposing synchronous host-call resolution over raw NYIR words while keeping
  `nyr_eval` as the no-resolver entry point.

- Expanded `nyc` to compile `void`-returning functions, explicit `return;`,
  implicit fallthrough returns, and direct function calls used as statements.

- Closed an affine loop-interchange dependence hole: individually injective
  stores into the same allocation can still overlap across iterations with
  different affine maps. A transposed 2×3 case changed the observed result
  from 10 to 1 before the fix; interchange now rejects that candidate and
  permits distinct same-allocation stores only when their complete byte ranges
  are disjoint.

- Widened loop vectorization for four-lane f64 elementwise memory/arithmetic
  bodies. SOR still does not vectorize: its nested row-base-plus-column
  `ADD_I64` address chain hides the typed-buffer root from the current direct
  root matcher. The vector trace now prints the address and addend opcodes;
  SOR reports `add.i64` / `shl.i64` with `root=-1`.

- Pure predicate inlining now lowers exhaustive boolean matches without a
  default arm, but only for a statically typed boolean subject with unguarded
  `true` and `false` patterns, in both expression and statement forms. Partial
  or guarded pattern sets remain unproved.

- Preserved closure-owned callback ABI metadata when generic call-site markers
  revisit an already marked closure. A native call through a returned untyped
  closure had overwritten its tagged result contract with the caller's
  inferred raw-integer contract; `rt_dynamic_callable_with_flags` now leaves
  an existing closure ABI intact.

- Fixed local shadowing in native container-literal callable classification. A
  parameter named `type_name` could be mistaken for the imported
  `core.type_name` function and stored as a callback marker instead of a
  string.

- Applied the same lexical-precedence rule to native callable-result ABI
  inference and callback lookup: a non-callable local no longer falls through
  to a same-named top-level function, while recorded callable aliases still
  resolve through their local metadata.

- Fixed compile-time existential witness memoization: each quantified integer
  candidate now has an immutable AST identity, so the pointer-keyed comptime
  memo cannot reuse the first candidate's value for later witnesses.

- Retired the comptime LLVM-module snapshot fallback. Unsupported comptime
  forms fail at their source with a diagnostic. The snapshot escape hatch and
  its bitcode/JIT implementation have now been deleted; the interpreter is the
  sole comptime evaluation boundary.

- Advanced `std.bc` emission from broken to two named defects, with the first
  class of fixes landed: untyped list locals in
  `lib/math/crypto/factorization/classical/gf2.ny` (vectors, vectors_packed,
  images, image_la, deps, solution) and `qs.ny` (`relations`, `poly_reports`)
  now carry explicit `list`/`dict`/`int`/`f64` declarations, which stabilizes
  their storage carriers under the call ABI verifier. The remaining blocker is
  a newly identified frontend defect, recorded in the TODO with an exact
  reproducer: in `_mpqs_best_adiv_mask` the implicit function-value handle for
  `_dict_with` is allocated to the same local slot as the f64 local
  `best_score`, so the mark-dynamic-callable funnel receives an f64-classified
  handle and the post-optimization ABI verifier rejects `std.bc` emission.

- Documented `bigint-oracle` as deliberately non-comparative: the harness
  C-reference path links libc/libm only and a fair bigint reference would need
  GMP, a non-hermetic dependency. Merged `prepare_links.h` into `prepare.c` as
  part of the llvm/ organization pass.

- Restricted the per-codegen comptime scalar memo to closed literal/operator
  expression trees. Identifiers, calls, member access, and nested comptime
  blocks are excluded so AST-address reuse cannot cross evaluation
  environments.

- The fast comptime evaluator now memoizes up to 128 successful scalar AST
  results per `codegen_t`, keyed by node identity within that immutable
  program context. Cache hits consume the shared comptime fuel budget;
  sequence and heap-backed results are not cached, and entries never cross
  compile sessions. Fuel exhaustion is diagnosed before the unsupported-form
  diagnostic.

- Dynamic dictionary callback storage now preserves boolean result ABI
  metadata across returned factory dictionaries, including both `true` and
  `false` results.

- Dynamic tuple and list storage now preserve named NYIR callback handles and
  their raw-versus-tagged argument conventions and integer/boolean result ABIs
  across factory returns. Dictionary storage uses the same typed marker
  selection. Indirect-call lowering recognizes direct sequence-index
  expressions as well as sequence `.get` calls.

- Comptime LLVM snapshot entry names now derive from the source file and token
  location and resolve collisions within the snapshot module. They no longer
  use a process-global incrementing counter, so concurrent evaluations do not
  race on generated names.

- Removed the formatter CLI's duplicate C recursive file collectors. The Ny
  collector also fails closed when a root or discovered path disappears or
  cannot be listed.

- Reorganized `src/code/lang/llvm/` into category folders with one-word names
  and retired the `legacy/` quarantine: `comptime_fast/if/interp.c` became
  `comptime/{fast,if,interp}.c`; `legacy/gencall/` became `calls/`;
  `legacy/expr/` became `expr/`;
  `legacy/{binary,core,debug,func,module,stmt,syms}.c` and `genintrinsics.c`
  moved to the `llvm/` top level under their own names; `gencalltypes.c` and
  `expr_profile.c` became `calltypes.c` and `profile.c`; the `core/` fragment
  headers stay beside `core.c`. The CMake globs and amalgamation
  `OBJECT_DEPENDS` tracking moved with the files (the `calls/init.c` and
  `expr/init.c` units still textually include their sibling fragments, and
  only those two units compile standalone), and all comment/doc path
  references were updated — the active LLVM emitter is no longer named
  "legacy". The remaining true legacy boundary (`gen_comptime_eval` module
  snapshots, interpreter parity, `std.bc` completeness) stays open in the
  renamed TODO item.

- Fixed compiler source-list initialization for the LLVM call and expression
  amalgamations.

- Callback effect inference now joins both outcomes of a direct ternary-return
  factory when each branch returns a complete immutable dictionary, list, or
  tuple with a matching shape. Dictionary keys must match in order; mismatched
  shapes remain unknown. It also joins a single top-level if-return and its
  fallback return, plus up to 32 explicit `match` arms when an `else` or
  unguarded wildcard arm makes the match exhaustive and all containers match.
  The simple if-return form accepts up to 32 immutable local prefix bindings.

- The same returned-container recognizer now includes guarded `match` arms
  when each guarded arm and the required default directly return matching
  containers. Guard expressions continue through the ordinary effect visitor.

- Dictionary callback values now retain callable identity through NYIR dynamic
  storage. Dynamic dictionary `.get` calls recover a matching callback result
  ABI from immutable literals or immutable factory returns, including dynamic
  keys when every candidate has the same result ABI. The existing
  dynamic-index callback matrix also remains green.

- A new negative compile shape pins the documented `@thread` f64-return
  diagnostic. Probing the ABI boundary found one real defect, recorded as the
  item's remaining work: an `@thread` call with eight or more source arguments
  under default-argument sugar bypasses the lowered-argument gate and crashes
  at runtime in the spawn path (non-default calls receive a clean diagnostic;
  the runtime dispatch itself handles 15 ABI arguments). `@async_effects`
  keeps its documented effect-analysis hint.

- Trimmed `docs/TODO.md` from 626 to 205 lines: every item now states its
  remaining acceptance criteria first, with landed sub-case narratives
  delegated to this changelog.

- Added `core.os.fs.list_dir_checked`, which returns `nil` when a directory
  cannot be opened while preserving an empty list for a valid empty directory.
  Documentation metadata collection uses the checked API and now fails closed
  on failed subtree opens instead of silently omitting them.

- The x64 object encoder now elides an adjacent, single-use scalar `ADD` when
  its address result feeds a scalar load. At the ADD, it captures the
  still-live base and either the raw or already-scaled index in reserved
  RAX/R10; the load consumes that pending SIB address. Shared, non-adjacent,
  vector, and store consumers retain the ordinary ADD.

- Callback effect analysis now joins rows from every callable entry in a
  complete immutable dictionary for dynamic `dict.get(key)` and `dict[key]`
  calls, and composes a dynamic `dict.get(key, fallback)` with the fallback's
  custom capability row. Direct-return factories preserve callback rows for
  returned dictionary/list/tuple literals and bounded chains of immutable
  local aliases, including rows supplied through callback parameters;
  factories with control flow or mutation stay unknown. Captured aliases
  preserve the summary; mutable dictionaries stay unknown, including after
  mutation.

- Website head links, RSS, static-page discovery, and `ny-doc search` doc
  enumeration now use the Ny documentation library exclusively; duplicate C
  tree walks are removed. Collection and RSS write errors propagate as
  site-build or search failures. The full `ny-doc` output builds, and a
  pre/post cutover comparison found identical file sets. Only the
  changelog-derived page, feed, index, and manual text differed because the
  docs changed between builds. An unavailable Ny collector now makes search
  fail instead of silently falling back to C.

- `core.doc_meta.collect` now fails on unreadable supported documentation
  files instead of silently omitting them.

- `core.doc_meta.resolve` now implements the REPL documentation lookup order
  in Ny: exact name, unique dotted suffix, then basename. REPL rendering
  remains in the C driver.

- `core.reflect.semantic_diagnostics` now exposes immutable HM diagnostic
  records, and `semantic_find_diagnostic` looks them up by stable code and
  optional source location.

- Two defects: `mach_vextract_i128` set the VEX R/B extension bits inverted,
  so every extract from ymm8-15 silently read an unextended register (the V256
  i64 reduce summed garbage lanes), and `mach_sib_lea_mem` folded single-use
  indexed LEAs under vector load/store consumers although only the scalar
  memory paths emit the folded SIB addressing — eliding the address
  computation and leaving the consumer's canonical home unwritten. f64 loop
  vectorization stays rejected inside the vectorizer: its lane math still
  models two lanes per VEC4_F64 op and needs dedicated widening plus SOR f64
  parity verification.

- Comptime templates can splice a reflected layout's ordered field sequence
  into a generated function signature with `comptime fields(Layout)`;
  `comptime fields(Layout, required)` omits defaults for fixed-arity APIs.
  Function cloning expands these parameters through the shared parameter
  expander, preserving field names, types, defaults, origins, and the template
  splice's syntax context.

- `core.codegen.new_fields_for` and `store_for` now generate positional
  constructors and fixed-arity typed layout stores from reflected fields.
  Reflected `${field.name}` values splice as identifiers in expression slots,
  allowing generated bodies to refer to the matching parameter hygienically.

- C `derive(store)` now emits typed per-field writes and returns safely when
  its destination is null.

- `core.codegen.default_for` now emits a no-argument constructor using
  reflected explicit and implicit field defaults. C-derived default
  constructors now return safely on allocation failure; the pinned legacy
  derive AST digest was refreshed.

- `core.codegen.default_ctor_for` now matches C `derive(default)`'s typed,
  defaulted parameter list. Field parameter splices preserve explicit defaults
  and synthesize implicit scalar defaults; `comptime fields(Layout, required)`
  keeps store values mandatory.

- C `derive(zero)` now returns null on allocation failure before calling
  `memset`, matching Ny `zero_for` safety.

- `core.codegen.load_fields_for(Layout)` now emits typed per-field
  `Layout_load_field` functions as well as the ordered dictionary reader.

- The C debug-string derive now uses the same ordered accumulation as Ny, with
  identical output and null handling.

- `core.doc_meta.collect` now rejects an existing file as its root; the
  collector requires a directory before bounded traversal starts.

- Unknown fallback rows remain unknown.

- The NYIR VM now preserves opaque user-function handles returned by dynamic
  `.get(index, fallback)` reads, including fallback misses, and marks
  statically known fallback functions with their raw scalar result ABI.

- Native module-member resolution now checks module-qualified top-level values
  before treating an identifier as a namespace.

- NYIR now lowers direct callback invocations from a one-index list/tuple
  `.get` expression as indirect calls.

- NYIR dynamic callback lists now retain the callable identity through indexed
  reads. The VM tags recognized opaque function handles as closures when
  building dynamic list descriptors and resolves zero-argument indirect calls
  through its function table instead of jumping to the handle as a C address.

- Four-lane f64 is now native end to end. The type map classifies the
  `VEC4_F64` family into a new 32-byte `V256_F64` machine representation,
  `ny_mach_lower_nir` accepts all ten opcodes, and the x86-64
  machine/object/JIT encoders select VEX.256 float forms: vaddpd/vsubpd/
  vmulpd/vdivpd with an explicit VEX.vvvv accumulator, vbroadcastsd SET1,
  vfmadd213pd fused multiply-add (admitted only on targets with a detected
  fused-FMA capability — a two-rounding mul+add substitute is refused),
  vextractf128 half extraction, general four-lane shuffles assembled from lane
  broadcasts plus cross shufpd on the home path, and reductions in the
  evaluator's sequential lane order. Several pre-existing latent encoder
  defects surfaced and were fixed along the way: the ymm register helpers
  silently dropped the VEX R/B extension bits for registers 8-15, the slow
  V256 store treated its value operand as an address, vector pointer operands
  read possibly-elided homes instead of the preg-aware path, and `UNPCKHPD`'s
  lane order had been misread. The AArch64 object fallback now emits all ten
  opcodes as paired two-lane NEON sequences over 32-byte homes (fused `fmla`,
  TBL-based shuffles, pairwise reduce), while its machine form fails closed by
  design so the from-NYIR caller selects the fallback.

- Native lowering now distinguishes zero-field enum variants from module
  namespace paths before resolving attached methods. Static variant receivers
  are preserved when lowering calls such as `Shape.Empty.method_area()`.

- Native indexed reads now prioritize the whole-program proof for append-built
  integer lists over the conservative dynamic-sequence accessor.

- Dynamic `any.get` now converts its already-tagged fallback to the raw
  fallback expected by the typed-buffer bridge, preventing a missing nested
  sequence lookup from tagging an integer default twice.

- Syntax registry membership checks now treat an absent raw-map entry as
  false, so builtin and user attribute names are retained in registration
  order.

- Native compile-time proof handling now validates tactic tokens before
  accepting a witness, so unknown `by_` strategies fail closed.

- Prolog substitution now stops on an absent raw dictionary binding instead of
  treating it as a present variable link.

- Comptime-generated recursive functions now retain direct-call identity
  through effect analysis, contract/decreases proof checking, and native call
  lowering. Generated `@total` countdown contracts pass through NYIR,
  native-only, LLVM MCJIT, and LLVM ORC.

- Native lowering now treats syntax-spliced identifiers as module namespace
  roots.

- Native `len` now checks registered typed-buffer handles before tagged-string
  recognition.

- The example builds/verifies NYIR, registers functions and a host callback,
  exercises nested execution plus callback re-entry into the same VM, and
  validates the returned result. It also verifies that duplicate registration
  fails without replacing the original function, registry changes are rejected
  inside callbacks, and a depth-limit failure unwinds cleanly for the next
  call. Function unregister, missing-call failure, and re-registration are
  verified. A host callback failure is also propagated through the VM API and
  verifies the VM accepts a subsequent call. Exact arity and per-call step
  budget failures also have recovery checks. The public header now states
  callback borrowing, reentrancy, registry-mutation, VM lifetime, and
  thread-safety constraints. Source loading, managed value handles, and
  runtime startup/shutdown remain unimplemented.

- `dict_pop` now routes membership, lookup, and deletion through the
  dictionary abstraction, which handles both managed and native storage.

- The self-contained C frontend now lowers `&&` and `||` with C precedence and
  short-circuit evaluation, returning normalized `0`/`1` values.

- The self-contained C frontend now supports prefix/postfix increment and
  decrement for mutable integer and double locals, including for-loop steps.

- Updated the Nyr API specification to describe the current C frontend subset,
  including short-circuit logic, conditional expressions, increment/decrement,
  compound assignments, dynamic declaration storage, and the 64-argument
  call-site limit.

- The C frontend now lowers `+=`, `-=`, `*=`, `/=` for integer/double locals
  and parameters, plus integer `%=` and bitwise compound assignments. Integer
  and floating local examples plus scalar/indexed pointer updates match host-C
  reference values and memory in `nyr-c-frontend`.

- The C frontend now lowers `do`/`while` loops.

- The C frontend now lowers signed integer shifts and `<<=`/`>>=`. C-reference
  cases exercise shift counts, signed negative right shifts, additive-versus-
  shift precedence, and compound shifts on defined inputs; `nyr-c-frontend`
  passes.

- The C frontend now lowers `pointer + integer`, `integer + pointer`, and
  `pointer - integer` for its 64-bit integer and double pointers, scaling the
  element offset before address arithmetic. Pointer differences and other
  pointer compound updates remain rejected.

- Const-qualified pointer parameters now distinguish pointee immutability from
  pointer-variable mutability: pointer offsets/increments and reassignment are
  allowed, while stores through const data pointers remain rejected. Host-C
  parity covers postfix increment, reassignment, and compound offset through a
  pointer-to-const parameter.

- Caller-side proof bindings now size to the formula instead of stopping at
  eight names; the solver and versioned certificate budget are aligned at 16
  variables and 64 constraints.

- Added `core.codegen.zero_for(Layout)`, a Ny compile-time generator for a
  layout-sized zero-initialized allocation.

- Added `core.codegen.load_fields_for(Layout)` to generate an ordinary Ny
  function that reads all layout fields through `load_layout`.

- Added `core.codegen.eq_for(Layout)` for null-safe, fieldwise equality.

- Added `core.codegen.hash_for(Layout)` with the legacy ordered field hash
  fold and null behavior.

- Added `core.codegen.debug_str_for(Layout)` with the legacy field order,
  formatting, and null marker.

- Added `core.codegen.defaults_for(Layout)` and reflected `field.default_expr`
  / `field.default_value`.

- Added `store_layout_field(dst, "Layout", "field", value)` with layout-schema
  type/offset validation and matching NYIR/LLVM stores.
  `store_fields_for(Layout)` emits a name-checked dynamic writer plus typed
  per-field writers, `store_dict_for(Layout)` emits a whole-layout writer that
  supplies defaults for omitted dictionary keys, and `new_for(Layout)`
  allocates an instance from named overrides and defaults. `new_for` also
  rejects invalid keys and releases its allocation.

- NYIR now boxes f64 values by their actual producer class when storing them
  in generic dictionaries.

- The shared NYIR list-append boundary now boxes f64/f32 values before generic
  tagged appends.

- `load_layout` now decodes stored f32 bits and boxes them for dynamic
  consumers, while preserving declared f32 and f64 return ABIs.

- Declaration-name splices now accept reflected identifier facts such as
  `${field.name}` and `${field.type}`. The cold four-route
  `comptime-reflection-name-splice` fixture generates and calls a helper for
  each layout field.

- `table_gen` also now has exact f64 cell formatting coverage (`1.5`, `2.25`).

- `core.reflect.semantic_find_function_at` now resolves duplicate function
  names by authored declaration location and fails closed when origins are
  ambiguous.

- Semantic source-location lookups now require correctly typed file, line, and
  column fields; malformed origins are rejected instead of coerced.

- The independent proof-policy interval checker now proves bounded signed
  division when the divisor's interval is strictly positive, using
  truncation-toward-zero bounds.

- `core.reflect.semantic` now exposes direct immutable accessors for an
  expression's resolved type, runtime representation, ownership, and versioned
  expression-effect record, custom expression and statement effect rows,
  mutability, and alias-class facts.

- `core.fmt_collect.collect` now enforces hard maxima of depth 256 and 100,000
  visited entries even when callers request larger limits, rejects empty or
  non-string roots before traversal, returns unique sorted paths, and fails
  closed when a root or discovered directory disappears or cannot be listed
  instead of silently formatting a partial tree.

- Semantic expression-effect aggregation now marks unhandled expression kinds
  unknown instead of treating them as effect-free.

- Custom callback capability rows now survive a returned closure that reads
  the callback from an immutable dictionary alias. The generated-origin
  consumer now identifies library-origin binary expressions instead of pinning
  a source line that moved as `codegen.ny` grew; emission location is still
  checked.

- The seven C layout derives were audited against Ny runtime generators. The
  audit initially found that template signatures could not expand typed field
  sequences. That gap was closed with `comptime fields(Layout)` parameter
  splicing, `new_fields_for`, `store_for`, and `default_ctor_for`; exact AST
  parity now covers all seven C-backed layout derives.

- Callback effect analysis now preserves callable rows through immutable tuple
  literals and bracketed constant-index reads, matching its existing list
  handling. List `get` reads preserve callback facts during analysis, while
  mutating and unknown list methods still invalidate them.

- Dynamic indexing into an immutable callback list or tuple now uses a summary
  only when every literal slot has a known callback and composable effect row.
  It unions builtin masks and custom rows across all slots and carries the
  summary through immutable aliases; mutation still invalidates it. Incomplete
  and mutable containers stay unknown.

- Match expressions now inherit the conservative union of their scrutinee,
  guards, and every arm's effects.

- Logical, ternary, index, member, dereference, and `try` expression rows now
  aggregate evaluated children. `sizeof` is resolved to an empty runtime row
  without visiting its unevaluated operand. HM now resolves the target type of
  `sizeof(expression)` without evaluating it, and NYIR plus LLVM lowering use
  that resolved type to emit the size constant.

- Runtime effect analysis now skips quoted code and proof-query syntax instead
  of attributing calls inside the stored syntax to the containing function.
  The comptime IO scan retains its conservative syntax traversal.

- Audited effect aggregation against the AST enums: all 29 current expression
  kinds and 32 statement kinds have explicit classifications, with new kinds
  remaining unknown through the fallback.

- Closed custom capability rows now survive aggregation through expressions
  and statements.

- Compile-time expression nodes now carry a resolved empty runtime-effect row
  even though the traversal skips their compile-time bodies.

- Fixed `core.report.format_error` and `format_warning` dropping the location
  prefix after constructing it.

- Environment cache hashing now binds each value to its variable name,
  distinguishes unset from empty values, and includes value lengths. The old
  value-only concatenation let different single-variable settings collide.
  JIT, std bitcode, AOT, std, and module cache identities were bumped. It also
  verifies unset and empty interpreter-only values use distinct entries.

- Whole-program AOT, standard-source, and standard-bitcode cache keys now bind
  `NYTRIX_COMPTIME_FUEL`, `NYTRIX_COMPTIME_INTERP_ONLY`, and
  `NYTRIX_TEST_MODE`. Module-bitcode keys bind the same inputs. The
  corresponding cache schema versions were advanced.

- Fixed `core.test.collect.collect` counting the root and every queued child
  against its visited-entry bound.

- Unqualified custom capability names declared inside a named module now
  receive that module's identity; explicitly dotted names stay unchanged. The
  capability contract matrix adds a matching qualified-owner call and rejects
  a same-named top-level capability that would previously collide. The
  row-algebra test round-trips the resulting qualified capability label. The
  cold/warm cache fixture now also derives `project.math.read` from
  unqualified `cap(read)` in its provider.

- Nested function effect facts now use the lexical leaf name used by local
  calls, rather than the module-qualified AST name. Nested function contracts
  are now installed in the nested-body effect context before summarizing calls
  to sibling module functions; this prevents the enclosing function's broader
  row from masking an underdeclared nested row.

- Module bitcode cache identities now include semantic/compiler options,
  relevant solver and comptime environment values, including
  `NYTRIX_COMPTIME_INTERP_ONLY`, standard-library paths, and link inputs.
  Module workers inherit strict-type, effect, alias, ownership, borrow,
  solver, safe-mode, standard-library mode, and heap settings.

- `core.reflect.semantic_obligations` now enumerates versioned proof
  obligation records attached to calls in a function. The ambiguity-safe
  `semantic_find_obligation` locates `requires` and `decreases` records by
  call origin and reuses the trusted call lookup rules.

- `core.reflect.semantic_statement_effects` now enumerates immutable statement
  effect records, and `semantic_find_statement_effect` provides ambiguity-safe
  lookup by kind and authored location.

- `core.reflect.semantic_signature`, `semantic_parameters`, `semantic_return`,
  and `semantic_signature_effects` expose checked function signature facts
  without requiring callers to traverse the generic snapshot.

- The independent bounded proof checker now tightens interval facts from
  overflow-safe multi-variable linear constraints, not only one-variable
  bounds.

- Added bounded `core.fmt_collect.collect` with deterministic `ny`, `fmt`,
  `code`, `c`, and `cloc` modes, extension filtering, and traversal limits.
  Formatter check, rewrite, optimize, C alignment, duplicate scan, and cloc
  paths now use its Ny-generated file list; analyze and audit now use its
  `code` mode as well. Existing C walks remain fail-safe paths. Formatter
  discovery resolves the sibling `ny-full` executable from `ny-fmt`; the
  replacement path is now correctly NUL-terminated.

- `core.reflect` now exposes `semantic_calls`, ambiguity-safe
  `semantic_find_call`, and `semantic_call_effects` over immutable semantic
  function records.

- Bounded-allocation preflight admits statically known loop trip counts
  through the eight-backedge walk limit; regressions prove three- and
  eight-trip cases, detect changed stores, and keep nine-trip loops
  inapplicable.

- `loop_interchange` now checks two individually injective affine stores whose
  shifted footprints overlap across iterations in the same private allocation.
  The pass verifies the function while preserving the original loop order and
  result.

- The semantic statement-effect visitor now includes the initializer subtree
  of `if` statements before aggregating the test and branches. The shared AST
  visitor and callback-alias effect walk now visit that initializer before the
  condition, making its bindings visible to branch analysis. The lexer now
  preserves semicolon separators in `if` and `while` headers as it already did
  for `for` headers.

- Template type substitution now recurses through nullable/pointer prefixes
  and nested generic arguments while preserving the surrounding type syntax
  and numeric layout arguments.

- The unified semantic reflection facade now forwards optional owners for
  function lookup and supports owner-qualified layout lookup.

- Template cloning now substitutes indexed-family enum index and payload
  types, plus layout/struct default-source expressions used by generated
  derive helpers. `#link` library names now use string interpolation rather
  than identifier validation, so generated library names may contain
  punctuation; explicit external symbol names use the same string
  interpolation.

- Compound assignment desugaring now retains the operator token on its
  synthesized binary expression, preserving source origins through template
  cloning and semantic reflection.

- Prefix increment/decrement and dereference compound-assignment desugaring
  now retain operator origins too; synthetic zero initializers use the binding
  declaration token.

- Typed `Code<T>` JSON restoration now rejects payloads above one MiB or
  deeper than 64 containers before exposing syntax.

- The deep-input matrix verifies the dedicated nesting-limit diagnostic
  instead of accepting a later envelope error.

- Added `json_try_decode_bounded` to the shared JSON parser so callers can
  reject excessive container nesting during parsing without a second source
  scan.

- Added immutable semantic function/layout list and name lookup helpers to the
  reflection facade.

- Added semantic expression-list access and ambiguity-safe lookup by
  expression kind and source location, with an optional file filter.

- Template AST cloning now explicitly covers every expression and statement
  kind. Interpolated f-string expressions and inline-assembly operands recurse
  through origin-aware cloning; embedded paths and include path/prefix/library
  fields receive template substitution.

- Native compile-time proof lowering now rejects source integer literals
  outside signed-i64 before evaluating their saturated parser payload.

- NYIR match arms that return through an `any`-valued function now box raw
  integer and f32/f64 arm results at the match join. Float arms use the
  runtime's canonical heap-float representation rather than integer tagging.

- Quoted compile-time emission now stores its expansion-origin token in the
  parser arena instead of retaining the address of `parse_emit_call_body`'s
  stack parameter. This prevents later semantic traversal from dereferencing a
  dangling origin when inspecting hygienic `#insert` declarations.

- Lemma regressions now bound positive operands before asserting a positive
  signed-i64 sum. The former unbounded claim was false under wrapping
  arithmetic at `INT64_MAX`; the independent checker correctly rejected it.

- The Ny typed-config DSL now supports nested table descriptors. Nested input
  is recursively validated for required, unknown, and mistyped fields, and
  errors carry qualified paths such as `service.port`. Nested schemas reject
  recursive validation deeper than 32 levels. Fixed TOML boolean decoding so
  the emitted `true`/`false` values round-trip with boolean type.

- Canonical TOML encoding now accepts and correctly quotes non-bare field keys
  inside nested dictionaries and nested table names, matching the existing
  `_key` escaping path. The cold four-route typed-config fixture round-trips a
  table name with spaces and a nested key containing a dot.

- TOML parsing now decodes basic-string escapes, validates Unicode scalar
  escapes, and splits arrays without treating commas inside strings or nested
  arrays as separators. Config encoding and parsing round-trip escaped quotes,
  backslashes, commas, and supported control bytes; NUL, raw DEL, malformed
  UTF-8, and surrogate code points remain rejected.

- Forged schemas fail with `E_CONFIG_SCHEMA`; required fields with defaults
  are rejected by schema construction. `schema(...)` also reports malformed
  raw descriptors instead of coercing names or flags.

- The arithmetic parser kit now consumes whitespace around operators and
  parentheses using byte-range predicates.

- Call expressions also carry version-1 `nytrix.semantic.obligations` records
  preserving instantiated `requires` formulas, caller premises, source
  origins, and proof outcomes, as well as recursive `decreases` strict-descent
  and `INT64_MIN` lower-bound obligations. Semantic signature slots, typed
  expression facts, obligations, and layouts preserve available template
  emission origins.

- Records use the `nytrix.semantic.call-effects` version-1 schema and keep
  unresolved effect rows explicitly unknown. It also verifies that a call
  through a statically known local callback alias preserves the IO row.

- Typed expression facts now include version-1
  `nytrix.semantic.expression-effects` records with inferred built-in rows
  aggregated from child expressions.

- Unsupported statement kinds now serialize as unknown instead of inheriting
  an empty, apparently complete row; declaration-only and control-only nodes
  are explicitly classified as effect-free.

- The trusted effect visitor composes rows through nested expressions and
  enclosing statements, including known higher-order callback substitutions;
  open tails and unresolved callbacks remain null.

- Runtime `parse_ast`, `parse_ast_snapshot`, and the `parse_code` declaration
  APIs reject inputs larger than one MiB before exposing parsed records.

- The generated-function case also covers loops, match, try/catch, defer, and
  nested expressions. Every serialized syntax node in those expansions must
  retain both its source definition and template emission origins.

- The self-contained C frontend now retains parameter names from a function
  definition when the same function was first declared with an unnamed
  prototype.

- C frontend local storage is sized from the tokenized translation unit rather
  than an arbitrary 32-local limit. A 33-local generated function is compared
  against the C reference executable.

- C frontend function storage now scales with the translation unit as well.

- C function signatures no longer stop at six parameters.

- The C frontend also lowers arguments after the first six of a direct call
  through NYIR's stack-argument payload. C block scopes now permit local
  shadowing but reject same-scope redeclarations; for-initializer names also
  leave outer locals visible after the loop. The generated objects and
  rejection diagnostic have focused host comparisons.

- Semantic-layout JSON emission now reads fields from the current
  `stmt_struct_t` AST union member, keeping the whole compiler build aligned
  with the parser.

- Effect analysis now resolves fully qualified calls into inline modules
  before checking custom rows.

- Template argument evaluation now follows typed `code_*_syntax` accessors
  through bounded pure helper functions.

- Bounded template helper evaluation now covers functions with the declared
  `Result<Code<CodeExpr>, str>` return type and functions that unwrap it to
  `Code<CodeExpr>`, including an immutable local holding the typed constructor
  result, forwarding the quoted AST through the typed constructor.

- Cold `comptime.nshape` coverage now also splices a `Code<CodeExpr>` created
  inside an ordinary zero-argument Ny function, then forwards that returned
  value through a second ordinary helper.

- Module workers now use the CLI's accepted `-no-std` spelling. Linking a
  cached worker drops only function bodies already defined in the entry
  module, preserving the entry definition while resolving declarations from
  the worker.

- The standalone `ny-doc` target now links `src/base/process.c`, which its
  metadata command requires for `ny_process_capture`.

- The docs renderer now consumes those Ny records for SEO links and schema
  entries, and RSS ordering uses their source paths and file modification
  times. The C collector remains a fallback if the Ny subprocess or record
  decoding fails. Recursive file discovery for the embedded manual's Markdown
  list and docs search also uses the Ny path collector; C still parses bodies
  and builds search entries. Static page generation now uses the same Ny path
  list while the C page renderer produces the output; the standalone render
  produced 53 documentation pages and completed successfully.

- Compile-time `code_category` now mirrors the Ny library query for scalar AST
  values and code sequences, while `code_kind` continues to report concrete
  singleton node kinds.

- Template arguments can resolve pure Ny helper functions with positional
  syntax parameters, immutable locals, and a final return expression. Typed
  syntax constructors and nested forwarding helpers preserve the returned
  quote AST; nested helper evaluation is capped at 64 and unsupported bodies
  fail closed.

- Parser-time shortcuts for `parse_code`, `codeof`, typed `code_*` helpers,
  compile-time code queries, `ident`, and typed-code `unwrap` now yield to
  same-named declarations in the current file or module. This keeps ordinary
  local name shadowing intact while preserving the standard-library syntax
  bridge.

- Template arguments now accept the typed `code_expr`, `code_stmt`,
  `code_decl`, and `code_ident` constructors over quoted syntax. The parser
  preserves the underlying AST and validates constructor and template
  parameter categories before expansion, including homogeneous sequence
  splicing.

- Parameters are marked borrowed according to the HM environment; returns
  remain unknown unless an owned or borrowed return contract establishes the
  state.

- Typed HM inference now retains final canonical type names separately from
  backend-directed `resolved_type_name`. Type-variable substitutions are
  finalized after inference before serialization, so the records do not freeze
  early variable spellings.

- The built-in row has its own known bit and uses canonical built-in effect
  identities; it does not claim that custom row labels were inferred.

- Effect analysis now resolves nested functions' closed `@effects` rows before
  their signatures enter the global function table. Capability rows and
  built-in effect names retain their identity at local calls; unsupported
  annotation forms still fail closed. Local callable aliases preserve the
  nested row as well.

- Imported source-module member calls now resolve the module-qualified
  signature before checking its custom effect row.

- Added `core.reflect.fields_with_origins_for(Layout)`, which emits an
  immutable version-1 field snapshot including source file, line, and column.
  The C comptime field descriptor reads the original field token; the existing
  `fields_for` row shape stays unchanged. Added the matching
  `variants_with_origins_for(Enum)` API using each variant and payload field's
  source token.

- Native lowering now treats an imported module-qualified function value as a
  function reference, not a method call with the module namespace passed as an
  argument. The local callable alias retains its canonical function identity,
  so callback invocation uses the correct target and arity.

- Zero-argument imported module calls now retain call semantics during native
  lowering instead of becoming function-value references.

- Inferred `any` locals now box an integer initializer only when the emitted
  NYIR value is actually raw. A dynamic-return user call already produces the
  tagged representation; applying `rt_tag` based on the call expression's
  stale raw-integer annotation double-tagged it and changed values such as
  `add(1, 2)` from `3` to `7`.

- `core.ast` now provides reusable `Code<T>` values for expression, statement,
  declaration, and identifier syntax. Constructors return typed results after
  category validation; identifier values require a marked AST identifier
  rather than a string. Canonical values use a versioned `nytrix.code.value`
  envelope. Typed accessors also reject forged or malformed envelopes whose
  canonical encoding, field types, schema, category, 0/1 sequence flag, or
  syntax category disagrees. The sequence flag is derived from the syntax
  shape when constructing or restoring `Code<T>`, avoiding duplicate mutable
  state.

- Added `core.ast.parse_code_typed`, which combines isolated declaration-only
  parsing with validated `Code<CodeDecl>` construction.

- Added `core.reflect.layout_for(Layout)`, a versioned immutable layout
  snapshot with the compiler-resolved size and alignment plus ordered field
  facts.

- `core.ast.code_signature` now returns a versioned immutable tuple for
  function, extern, and anonymous `fn` syntax, including ordered parameter
  names and types, canonical default expressions, parameter origins, variadic
  state, and declaration attributes. `code_contract` exposes canonical
  `requires` and `decreases` syntax.

- Added `comptime attributes(Declaration) as attribute` and
  `core.reflect.attributes_for(Declaration)`. Ordered declaration attributes
  expose their names; the library packages them in an immutable version-1
  tuple.

- `core.ast.code_attributes` now returns source-ordered declaration attributes
  as deeply immutable `nytrix.code.attributes` version-1 tuples. Each row
  contains the attribute name, canonical argument JSON, and canonical source
  and expansion origins.

- Moved `ny doc meta` header parsing and keyword normalization into
  `core.doc_meta`; the C entry point is now a thin Ny subprocess driver.

- Empty code sequences remain valid for expression, statement, and declaration
  sequence contracts, but no longer satisfy scalar `code<ident>`.

- Spliced call argument lists now have explicit cold arity controls for too
  few and too many expanded arguments.

- Canonical serialization now has a cold quote snapshot round-trip covering
  schema version, source origin, and the distinct syntax marks on quote-local
  and spliced identifiers.

- Versioned quote AST records now carry a `nytrix.code` version-1 category
  record.

- Versioned AST snapshots now preserve source and template-emission origins
  for each selected `use` import name and alias, and for individual module
  export names.

- Directly emitted `quote{...}` syntax now passes through the origin-aware
  clone path, preserving source locations and attaching the `comptime emit`
  site as the expansion origin for declaration, statement, and expression
  quotes.

- Template reflection now attaches emission origins to synthesized field-load
  callees and arguments, nested enum payload tuple rows and leaves, and layout
  offset call trees. Literal and identifier substitutions retain their
  original source tokens; identifier splices also retain their syntax mark
  while carrying the template emission origin on both the splice node and
  inserted identifier.

- Top-level assignments are no longer registered as `codeof` declarations.
  Compile-time declaration queries reject assignment-shaped `NY_S_VAR` nodes,
  so mutating a declared variable does not make `codeof(variable)` ambiguous.

- Fixed declaration-only `parse_code` validation for `NY_S_VAR`, which also
  represents assignment statements. Template-time parsing checks the
  declaration flag before accepting a variable node; runtime `parse_code`,
  `code_kind`, `code_name`, and sequence classification use the versioned
  snapshot's `is_decl` fact to distinguish declarations from assignments.

- Comptime template parameters can now declare `code<expr>`, `code<stmt>`,
  `code<decl>`, and `code<ident>` splice contracts. Category checking handles
  scalar quotes, homogeneous quoted sequences, empty sequences, and assignment
  statements before AST cloning; declaration-as-expression is rejected with a
  diagnostic that reports the expected and received categories.

- Cold REPL imports now resolve stdlib functions in batch and interactive
  sessions, including later prompts after imports. Fixed imported-signature
  ownership across engine rebuilds by deep-copying LLVM names, by-value layout
  strings, and effect allowances; guarded runtime string recognition by
  checking heap ownership before probing arbitrary C-string addresses.

- Payload binders are scoped per arm. Structural `decreases` now accepts a
  recursive call whose measure is a directly bound payload of the same enum
  type in a matching constructor arm, and composes descent through nested
  matches on that payload; rebinding that payload name anywhere in the arm
  invalidates the proof, and a non-descending call on the original value
  remains rejected. Native call lowering now expands chained defaults by
  substituting earlier normalized arguments before lowering later defaults.
  This prevents a formal such as `upper` from being rebound to a same-named
  imported function when compiling at the call site; builder-owned scratch
  storage bounds the cloned expression lifetime.

- Native NYIR lowering now supports range-valued `for` iterables without
  materializing a list: it reads the range length and indexed values through
  the runtime range ABI. Literal list and tuple destructuring lowers each
  initializer element once through the regular local-binding path. The runtime
  ABI declaration table now keeps the lexical ordering required by its
  binary-search lookup; the misplaced `rt_f64_to_i64` row had caused
  floating-point arguments to be classified as unknown.

- Native lowering now preserves dynamic results for list `get` calls,
  including named `key`/`default` arguments, while keeping typed scalar
  results on the raw path. Float comparisons against dynamically indexed
  sequence elements now retain the dynamic float representation.

- Native local initialization now trusts the lowered scalar class, not a stale
  source-level raw-integer annotation, when storing indexed values. Tagged
  values read from an untyped literal list are decoded before entering
  inferred integer locals; equality uses the raw comparison only for proven
  scalar-list reads.

- Added Ny-owned `core.reflect.fields_for(Layout)` and `exports_for(Module)`
  compile-time generators. They copy layout field facts and ordered export
  names into deeply immutable, version-1 tuple snapshots with schema
  identifiers; no compiler-owned descriptor escapes. Added
  `variants_for(Enum)` snapshots with ordered discriminants and typed payload
  field pairs. The enum variant compiler descriptor now exposes immutable
  `payload_fields` tuples to template code. Native call lowering now preserves
  the exact declaration recorded by type inference for unqualified module
  calls, preventing duplicate helper names in sibling modules from binding to
  the wrong function.

- Native lowering now treats a splice's recorded syntax mark as exact during
  identifier lookup.

- Compile-time `ident(...)` now preserves the syntax mark of its source symbol
  through template binding and reconstruction. Identifier code is kept inside
  the exact-context splice wrapper, including identifier elements expanded
  from expression lists, so quote-local same-named bindings cannot capture the
  caller reference.

- `many(parser, 0)` now succeeds with an empty result and no input
  consumption; negative repetition bounds fail with a structured error.
  `choice` now tries alternatives iteratively, so the number of alternatives
  does not consume grammar nesting depth; a 141-branch choice passes with a
  depth limit of 8 and a bounded step budget.

- Compile-time `parse_code` now commits its syntax-mark and deterministic
  gensym counters only after the isolated input parses completely as
  declarations. Malformed, empty, or executable input leaves caller parser
  state unchanged.

- Runtime `core.ast.parse_code` now returns declaration nodes from the
  versioned syntax snapshot, preserving decorator attributes and source
  origins that the legacy AST shape omits.

- `core.ast.code_kind` now classifies homogeneous declaration, statement, and
  expression sequences, and reports empty, mixed, invalid, and unknown
  sequences explicitly. Single-node queries now return `invalid` for malformed
  records and `unknown` for unrecognized node kinds; `code_name` returns names
  only for declarations and rejects malformed declaration-name fields rather
  than coercing arbitrary values to strings. The category/name helpers accept
  both list and tuple code sequences. The AST self-test now explicitly
  distinguishes unknown future node kinds from malformed records for both
  single nodes and sequences. `code_str` now accepts tuple code sequences,
  matching `code_kind` and `code_name`; tuple serialization round-trips
  through canonical JSON.

- Added `core.ast.code_has_attribute` for JSON AST declarations and as a
  compile-time query over `codeof`, `parse_code`, and quoted declarations.
  Template-time `code_kind`, `code_name`, and `code_has_attribute` now match
  library neutral results for scalar values, identifier code,
  non-declarations, and multi-declaration sequences; malformed query calls and
  nonliteral attribute names remain diagnostics.

- Compile-time `code_kind([...])` now derives the sequence category from every
  quoted item instead of labeling every list as declarations. Homogeneous
  expression and statement sequences, mixed sequences, and empty sequences
  preserve the same categories as the `core.ast` query; parsed declaration
  lists report `declarations`. Identifier code values classify as expressions
  and can occupy expression-list splices with their exact syntax marks. Scalar
  quoted expressions now report their concrete AST kind (`literal`, `ident`,
  and so on), matching `core.ast.code_kind` instead of returning the generic
  string `expr`.

- Every serialized record carrying a syntax `type` category in those fixtures
  has `source_origin` and survives canonical serialization.

- `ny test` now gets default and explicitly selected directory path lists,
  `--list-meta`, `--list-bench`, and benchmark fixture enumeration through
  `core.test.collect`; the duplicated C directory walker is removed. Its
  driver retains shape parsing/expansion, selection, scheduling, and
  reporting.

- Fixed the runtime ABI metadata for raw filesystem results: existence and
  directory predicates are raw integers, and directory-open returns a raw
  pointer.

- Renamed the semantic-reflection consumer source to `.ny.in`.

- Completed template AST cloning for generated links, labels, gotos, defines,
  nested modules, and macro bodies/arguments. Identifier-bearing fields are
  substituted in the caller's expansion context, and nested module bodies are
  recursively cloned.

- `core.ast.code_str` now rejects sets, non-finite floats, unsupported scalar
  objects, and non-string object keys instead of producing unstable or invalid
  JSON.

- JSON float encoding now emits 17-digit round-trip values, preserves negative
  zero and integral-float identity, and emits `null` for non-finite values.

- Added `<nytrix/nytrix.h>`, an embeddable NYIR VM API with registered
  functions, raw-word host callbacks, and bounded execution/call depth.
  Lisp/Lua hosts can lower their own forms to NYIR; source-text parsing and
  host-object marshalling remain the embedding application's responsibility.

- Moved their shared support header into `test/support.h` and the type/effect
  row test into `test/typing.c`; no standalone test Makefile is needed.

- Separated typed AST-to-NYIR language lowering into `src/code/lang/lower.c`
  from backend execution, optimization, LLVM integration, wire/build stages,
  machine encoding, and object writing under `src/code/backend/`. Moved
  optimizer implementation tests and their runner under
  `src/code/backend/ir/opt/test/`. Host JIT memory/runtime management lives
  beside NYIR emission in `src/code/backend/ir/jit.c`.

- Restored the language-lowering fragments beside `src/code/lang/lower.c`,
  made the builder typedef available before those fragments, and aligned dump
  exports and the shared function-pool limit with the Nyr backend names. CMake
  now compiles the legacy LLVM expression initializer, not its included
  `core.c` and `f64.c` fragments as independent translation units.

- Moved the embedding API CLI test to `src/cmd/ny/test.c`; its declaration now
  lives alongside the command in `src/cmd/ny/test.h`.

- Made `src/include/nyr/` the source of truth for the installed Nyr API: moved
  the public NYIR declarations to `nyr/ir.h`, installed the API header
  directory directly, and removed the generated-header copy and internal
  forwarding header. Private IR implementation headers remain in
  `src/code/backend/ir/`; compiler and optimizer consumers now include the
  public contract explicitly.

- Replaced libclang parsing in `nyc` with an in-tree lexer/parser that lowers
  the currently supported C subset through the public Nyr API.

- Extended the supported C subset with initialized and mutable signed-integer
  and double locals, parameter updates, direct signed 64-bit pointer stores,
  and direct integer/double function calls across separately emitted objects;
  generated native objects match C references across 135 deterministic
  argument and mutation cases.

- Added direct and indexed `double *` loads and stores, plus C's equivalent
  unsized one-dimensional array parameter spelling for integer and double
  pointers and direct pointer-argument calls, to the Nyr-backed C frontend.
  Const-pointee writes are rejected; generated x86-64 ELF objects match the
  host C reference.

- Consolidated the Nyr-backed C frontend's indexed pointer address calculation
  for reads and writes, centralized scalar-type lookahead, removed unused
  parser state, and clarified that the FFI header declaration parser and
  standalone function-body parser share one lexer but serve separate
  contracts.

- Removed `nyc`'s redundant post-optimization verification call; Nyr object
  emission verifies the optimized function before code generation.

- Restored the native array-table interner's shared declaration, definition,
  and lowering callsite after the internal symbol rename; this fixes runtime
  loading of `nytrixrt` for the standalone `nyc` compiler.

- Separated private native-target and program-object names from the public Nyr
  target descriptor and single-function object emitter. Updated recovered
  lowerer fragments to the current target initialization and IR-build APIs.

- Tightened the public Nyr surface: `nyr_func_t`/`nyr_inst_t` aliases, named
  O0–O3 levels, fallible emission with separate value IDs and diagnostics, and
  Nyr-specific capability bits for evaluation and x86-64 ELF output. Installed
  IR declarations now use `<nyr/ir.h>` rather than an internal-looking detail
  path. Object capability is omitted on hosts this API cannot emit for.

- Track `const` on scalar locals and pointer parameters, reject writes through
  const-qualified data, reject volatile objects and unsupported pointer
  arithmetic/comparisons, and diagnose out-of-range integer literals instead
  of silently wrapping them. Statement-lowering now propagates semantic errors
  raised after consuming a complete statement, preventing invalid pointer
  stores from being silently omitted and accepted.

- Both `while` and restricted `for` bodies can now return directly; the loop
  exit remains available when the condition initially fails.

- A `for` declaration is scoped to the loop; use after the loop is rejected.

- Add scaled signed-`long` pointer subscripting for loads and stores, with
  reference-compiler checks across five array positions and const-store
  rejection.

- Restored the statement-parser amalgamation's static helper declarations
  after its core/comptime split; the compiler and `nyc` targets now build from
  the split sources without implicit declarations.

- Reorganized typing pipeline implementation into
  `src/code/lang/types/pipeline/{core,hm,emit,init}.c`, the independent
  certificate checker into `src/code/lang/types/policy/check.c`, and compiler
  pipe amalgamation units into
  `src/code/backend/wire/pipe/{cache,stage,tiny,init}.c`. The `*_init.c`
  textual inclusion model and incremental object dependencies remain explicit
  in CMake.

- Caller-side `requires` inlining now handles pure predicate functions that
  explicitly return or finish with a supported finite literal `match` with a
  default arm.

- Moved statement-parser components into
  `src/code/lang/parse/stmt/{core,comptime,dispatch,init}.c` and retained
  their textual amalgamation through `parse/shared.c`.

- Legacy nested `@effects(cap(...))` annotations now reject capability names
  longer than the 63-byte identity limit instead of silently omitting that
  row.

- Callback-row analysis now snapshots and joins local aliases across
  `try`/`catch` normal and handler paths when the try body does not call or
  mutate tracked callback aliases. A body call or alias write makes the
  summary unknown because a catch may observe an intermediate exceptional
  state.

- Dynamic native integer stores now resolve the indexed destination before
  selecting a distinct source register. The encoder now avoids address
  registers, spills any resident value occupying its chosen scratch register,
  and loads the store source only after address resolution. These performance
  gaps are not fixed by the correctness repair.

- The x86-64 object encoder folds a same-block, single-use scaled `LEA`
  directly into an indexed load/store SIB operand. It caches the index in R10
  across eligible floating-point instructions; the definition map no longer
  mistakes `NY_MACH_STORE.dst` (an address use) for a value definition.
  `machine_home_reuse` checks the emitted indexed accesses, index-register
  reuse, and generated-code results. The object dump confirms direct indexed
  loads and store.

- The f64 loop-vectorizer now fails closed until all backends agree on
  `VEC4_F64` width: NYIR operations have four-lane semantics, but native
  machine lowering still represents them as two lanes. `loop_vector_metadata`
  now checks f64 memory reduction semantics remain unchanged and the unsafe
  vector load is not formed.

- Named closed effect rows now participate in effect-contract checking.
  Positive custom-row joins remain unsupported. Nested functions with
  unresolved custom effect annotations fail closed when called rather than
  silently dropping the annotation. Capability authority remains a separate,
  unimplemented interface.

- Recursive `@total` checking now has four-route coverage for bounded affine
  measures with a nonzero constant offset (`2 * n + 3`), in addition to the
  existing zero-offset affine countdowns.

- The independent proof checker now validates bounded nonlinear measures used
  by `@total` when it can prove both strict descent and a finite lower bound
  without signed overflow.

- The bounded interval proof path now handles products of distinct variables
  as well as squares.

- Caller-side `requires` checking now transports bounds from inclusive `for
  item in start..end` loops when both endpoints are simple integer identifiers
  or literals, the upper endpoint is proved below `INT64_MAX` (including by a
  `Fin<N>` bound), and the body does not mutate either endpoint.

- Block-scoped effect analysis now preserves callback-row facts when
  committing local updates to an enclosing scope. Unresolved and incompatible
  open-row joins remain fail-closed.

- Parser `gensym` now preserves full prefixes instead of silently truncating
  names at 127 bytes. Counter exhaustion rejects generation before wraparound;
  parser-local numbering remains unchanged. Named modules now have independent
  counters and reversible hexadecimal namespaces, preserving names when module
  declaration order changes. F-string and isolated declaration parsing share
  the enclosing name-generation state.

- Template cloning now expands deferred cleanup bodies, try bodies/handlers,
  and while-loop invariant expressions. Versioned loop snapshots retain
  invariant syntax and its source/emission origins; substitution and canonical
  round trips are covered without changing legacy loop record shape.

- Template expansion now clones external signatures and lemma declarations,
  including parameter provenance, substituted declaration names, and lemma
  propositions. Statement decorators and their argument ASTs are cloned per
  emission instead of shared with the template.

- Template string substitution and string-valued AST splices now preserve
  counted bytes, including NUL in the original literal, after the literal
  splice site, and inside the bound string argument.

- Template cloning now substitutes refinement names/base types/predicates,
  trait declarations and their methods, and implementation trait names/laws.
  Existential predicates also clone their bodies rather than sharing an
  unexpanded formula.

- Template cloning now expands function `requires`, `decreases`, and `@proves`
  expressions instead of sharing their unexpanded template AST.

- Parameter parsing now retains declaration tokens and binding scope marks.
  Versioned parameter records expose those marks and source origins; template
  cloning preserves definition metadata and adds emission origins for
  function, lambda, and layout parameters.

- AST string literals now use counted JSON escaping for every control byte,
  including embedded NUL. Formatted-string parts use the same counted writer;
  assembly strings and link/include paths use JSON escaping. The Ny builder
  byte API preserves NUL using an explicitly counted one-byte string.

- Versioned statements now retain binding scope marks, and variable records
  retain mutability, declaration/destructuring flags, and type annotations.
  Oversized AST integer literals now expose normalized exact source text with
  an explicit out-of-range flag and null numeric payload, rather than
  presenting the saturated internal int64 payload as their value.

- Versioned layout snapshots now retain deftype parameters, layout flavor,
  methods, field defaults with their source spelling, and array-extent syntax
  with expression origins. Struct snapshots retain the same field metadata and
  methods.

- AST serialization now represents refinement and trait declarations and
  existential predicates instead of emitting unknown-node placeholders. This
  lets `core.ast.parse_code` accept those valid declaration kinds. Versioned
  enum records retain generic parameters, indexed-family metadata, and typed
  payload fields; implementation records retain their trait and law syntax.

- Versioned AST snapshots now expose ordered statement attributes with parsed
  argument nodes and per-attribute source origins. Generated decorators carry
  emission origins. Functions also retain `requires`, `decreases`, and their
  documentation string.

- AST literal JSON now emits 17 significant digits for binary64 values and
  uses portable signed 64-bit integer formatting. Precise fractions, small
  exponent values, and `INT64_MAX` have a cold serialization regression. JSON
  number decoding now keeps its floating result in an explicit `f64` local and
  extracts number text through the string slicing API. JSON null decodes as
  nil, independently of integer zero.

- Added `core.ast.code_str` for canonical compact JSON serialization of AST
  records and versioned snapshots. Key ordering now uses a Ny merge sort with
  lexical string comparisons. JSON encoding distinguishes integer zero from
  nil instead of testing equality with zero.

- Added `core.ast.code_kind` and `code_name` for AST record inspection.

- Added `comptime variants(Enum)` reflection for declaration-order variant
  names, indices, payload presence, and payload arity.
  `core.codegen.match_gen` emits a typed variant-name matcher for unit and
  payload enums using the pattern-only `variant.pattern` descriptor. Template
  emissions now retain definition origins and expose their emission site as
  `expansion_origin` in syntax snapshots; generated declarations are validated
  in the calling module.

- Expanded cold acceptance to native-only for root `parse_code` insertion,
  generated-module insertion, and `table_gen(Layout)`.

- Returned callback factories now analyze their local prefix statements with
  the existing effect-flow visitor instead of rejecting every non-declaration
  prefix.

- Nested closures that capture and forward a callback alias now preserve its
  latent effect row.

- Lexicographic `@total` measures now lower boolean components as boolean
  formulas instead of embedding integer literals as proposition nodes.

- The x86-64 object encoder can omit a dead indexed address home for a
  load-address consumer, float store, or narrow-immediate store when
  immediate-use and liveness checks prove the RAX value can be forwarded.
  Dynamic integer stores retain the home. Fresh native-tier-opt assembly still
  contains load-address home saves and an output-store address store/reload.

- Added deferred layout reflection inside compile-time templates and three
  Ny-owned generators, `core.codegen.json_for(Layout)`, `soa_for(Layout)`, and
  `table_gen(Layout)`. They emit a field dictionary, a structure-of-arrays
  projection; generated declarations pass through ordinary typing and code
  generation. A dedicated AST marker distinguishes deferred queries from
  user-written calls. Template cloning now also recursively expands lambdas,
  including reflected member splices inside callback bodies.

- Consolidated the documented metaprogramming syntax around `comptime
  template`, `comptime emit`, `quote {... }`, and `${value}`. The `$`
  interpolation spelling now has one documented role across template
  arguments, quoted AST insertion, and template-string interpolation;
  hash-prefixed spellings remain compatibility aliases.

- Added `codeof(name)` as a template argument intrinsic for previously parsed
  named file- and module-level declarations. It preserves the declaration AST
  for category-checked insertion, resolves qualified names, and rejects
  missing, ambiguous, or expression-slot uses.

- A literal `parse_code("...")` template argument now parses declaration-only
  source in an isolated parser and supplies its AST nodes as a `${...}` splice
  list. Parser registries are copied before parsing so new source declarations
  cannot mutate the caller's `codeof` lookup state. Generated output is
  checked by the ordinary pipeline. The four `parse_code` negative controls
  pass.

- `table_gen(Layout)` now emits ordered headers and string rows as a pure
  library adapter.

- A polymorphic helper's open effect variable now receives the union of rows
  from all direct named callback arguments instead of being unified repeatedly
  with each callback independently. Row composition preserves a single open
  tail and composes distinct open tails by binding each to the other's
  label-residual plus a fresh shared tail.

- Lexicographic recursive measures accept finite boolean components before or
  after bounded integer components.

- Passing bounded proof and callback-row subsets do not close the general
  proof-obligation or effect-row TODOs.

- Caller-side relational proofs now cover bounded numeric disjunctions in both
  `while` and C-style `for` guards when every arm implies the called
  function's `requires` condition. Unsupported disjuncts and facts invalidated
  by mutation remain fail-closed.

- Caller-side contract substitution now maps mixed positional/named
  arguments—including positional arguments after named ones—for direct calls
  and attached methods, matching call normalization. Cold four-route
  regressions prove both forms under symbolic caller preconditions.

- Pure-predicate expansion uses the same next-unassigned-formal mapping for
  mixed positional/named arguments in both orders.

- Caller-side predicate expansion now resolves direct qualified module
  functions as well as local identifiers.

- Symbolic `requires` expansion now handles a uniquely identified attached
  method when its signature is pure and effect-free. Receiver actuals map to
  the implicit first parameter; ambiguous and unresolved methods remain
  fail-closed.

- Recursive `@total` proofs now accept `Fin<N>` measures using the resolved
  type-level `[0, N)` bound and still require strict descent at every
  recursive edge, including lexicographic tuples that mix `Fin<N>` and signed
  integers.

- Caller-side `requires` proofs now consume a resolved `Fin<N>` argument's
  `[0, N)` range.

- Added `core.parser`, a versioned Ny parser-combinator toolkit with
  sequence/choice, mapping, literals, byte predicates, integers,
  optional/repeated parsers, full-input and prefix runners, bounded parser
  steps/depth/input, recursive forward references, and structured byte-span
  diagnostics. Repetition rejects zero-width success. Its callback-bearing
  execution APIs conservatively declare `all` effects. The typed configuration
  API and recursive arithmetic parser/emitter complete the acceptance for
  roadmap item 10; these conservative callback effects keep parser execution
  sound until the type system can express stored callback rows.

- Added `core.math.parse.arithmetic`, a recursive integer-expression parser
  and canonical fully-parenthesized emitter built from `core.parser`.

- Added `core.config` for versioned TOML schemas, typed validation,
  required/optional/defaulted fields, unknown-key rejection, structured parse
  errors, and canonical TOML output. Scalar schema fields retain schema order;
  nested table keys sort deterministically, with scalar keys before child
  tables. Fixed nested TOML section updates to rebuild each dictionary path
  with `dict_set`, and added a nested encode/parse regression. Together with
  the recursive arithmetic parser/emitter matrix, this completes the
  parser-kit, configuration, validation, and emitter acceptance in roadmap
  item 10.

- Template expansion now has a cold positive proving an attached `@inline`
  attribute survives generated declaration expansion.

- Template name interpolation now requires an identifier-valued argument. Bare
  symbols remain identifiers; strings and scalars need explicit
  `ident(value)`, whose fragment alphabet is checked and whose completed name
  must be a valid identifier. Non-identifier values in type and symbol slots
  fail closed.

- `${value}` inside template string literals now interpolates string,
  identifier, integer, and boolean arguments. This fixes layout-derived helper
  names and layout-name strings when the layout itself is emitted by a
  template. The standard-library bundle passes after the fix.

- Quote syntax now has one braced opener, `quote {... }`. The former `quote $
  {... }` and `quote ${... }` openers fail with a targeted syntax diagnostic,
  leaving `${...}` available for splicing inside the quoted body. `quote` is
  now recognized as syntax only before a braced body, so `quote(...)` remains
  an ordinary identifier call.

- `NYTRIX_BENCH_JOBS` controls fixture workers (routes remain sequential
  inside each fixture); set it to `1` for isolated performance samples. It no
  longer inherits `NYTRIX_TEST_JOBS`.

### 2026-10-07

- Audited user-facing limitation statements in the REPL, status, errors,
  cookbook, networking, and project-fit pages against the open TODO list. Cold
  REPL stdlib imports now have one explicit acceptance item linked from
  affected feature pages. A bare `find("a", "a")` in `--repl` reports an
  undefined symbol; `use std` followed by the same call segfaults, while
  `ny-full -c` returns `0`. Removed the stale `TODO.md#current-verification`
  links and suite-count snapshot from the status and project-fit pages. Other
  documented feature gaps without an existing backlog item are recorded in the
  P2 section; none is marked complete.

- The runtime `parse_ast` inspection bridge now parses quietly and returns nil
  for malformed source instead of serializing a recovered partial AST. Its
  diagnostic limit cannot terminate the host program, and diagnostic
  de-duplication no longer hides parser failure state.

- `core.ast.parse_code` now returns a complete list of parsed declaration
  records, or nil for empty input, executable top-level statements, or any
  parser error. It builds on `parse_ast`, which discards recovered trees when
  parsing fails, so callers never receive partial declarations.

- `core.ast.parse_ast_snapshot` adds a versioned syntax view with the
  `nytrix.ast` schema identifier, version `1`, and a per-node `source_origin`
  filename/line/column. The legacy `parse_ast` list shape is unchanged. The
  snapshot is still JSON data and does not yet contain compiler type, effect,
  ownership, layout, or proof records.

- `core.ast.code_origin` returns source and expansion locations in a versioned
  `nytrix.origin` record, keeping consumers independent of raw AST field
  names.

- Versioned AST snapshots now include complete `use` import descriptors
  (locality, wildcard state, and ordered imported names and aliases).

- Versioned layout and struct field records now retain the field-name source
  token and comptime emission origin.

- Generated scalar expressions inserted into statement sequences now mark the
  synthesized statement wrapper with its definition and emission locations.

- Versioned enum variant and payload-field records now expose definition and
  template-emission origins.

- Versioned enum snapshots now include generic-parameter records with source
  and template-emission origins while retaining the existing `type_params`
  strings for compatibility.

- Template interpolation and quote splicing now share `${...}`. A code list
  expands in expression lists, positional call arguments, and statement
  sequences; scalar quoted code inserts one expression. Quote-body splices
  remain AST nodes inside template parsing, category errors retain the splice
  source location, and identifier suffix interpolation is unchanged.

- Quote syntax now has one braced opener, `quote {... }`. The former `quote $
  {... }` and `quote ${... }` openers fail with a targeted syntax diagnostic,
  leaving `${...}` consistently available for splicing inside the quoted body.

- Quote parsing now reports an error when nesting exceeds its 64-entry scope
  mark stack, and restores the exact saved caller mark during unwinding.

- LLVM NYIR lowering now uses `<4 x double>` for every `NYIR_VEC4_*_F64`
  operation, including memory, shuffle, and reduction. Vector FMA calls LLVM's
  fused `llvm.fma` intrinsic. The x86-64 NIR assembly emitter stores four
  scalar lanes, implements four-lane arithmetic/shuffle/reduction, and calls
  `fma` per lane for fused results. Its machine-scalar shortcut is bypassed
  for these functions because that representation still has two f64 lanes.

- Caller-side `requires` transport now accepts typed boolean parameters and
  boolean formulas in addition to signed integer formulas. Fixed-size
  lexicographic `decreases` tuples retain signed-integer components (with the
  `INT64_MIN` lower bound), and a standalone boolean measure uses the finite
  order `false < true`.

- Caller-side `requires` transport now inlines a direct call to a bounded,
  pure/effect-free predicate before checking the symbolic implication. A
  predicate body may be one value expression/return, sequential immutable
  single-name locals followed by a value, or nested `if` statements whose arms
  provide values and may introduce immutable single-name locals. A top-level
  boolean early-return guard with a fallthrough value is also supported.
  Recursive, indirect, mixed-argument-style, and other multi-statement
  predicate bodies remain unsupported and fail closed. Positional and complete
  named predicate calls are supported. Parameter substitution uses
  collision-free placeholders, so swapped actuals cannot capture one another.
  Multi-local, nested-branch, and early-return predicates preserve positive
  proofs and weaker-premise rejection on all four routes. The independent
  certificate checker now validates nonlinear integer comparisons when bounded
  interval evaluation decides them without overflow. It handles addition,
  subtraction, multiplication, and negation over explicit obligation ranges or
  bounds derived from overflow-safe linear premises; missing ranges,
  unsupported operators, and overlapping intervals remain undecided. The
  policy path can use this checker as a bounded range fallback. Signed
  division reasoning now accepts strictly negative as well as positive divisor
  intervals under truncation toward zero.

- The compile-time fallback can interpret direct identifier calls to pure,
  effect-free functions when argument substitution makes a caller-side
  `requires` predicate exact. The bounded interpreter's fuel limit applies;
  qualified, indirect, and impure calls are not interpreted, and false/unknown
  results remain unproved. Positive, false-predicate, and impure-predicate
  controls pass through NYIR, LLVM MCJIT, LLVM ORC, and native-only.

- `@effects(read|read_memory)` and `@effects(write|write_memory)` now map to
  their canonical memory-effect rows instead of being treated as capability
  names. Core and runtime `memcmp`, `memcpy`, and `memset` aliases have
  matching summaries, so an underdeclared callback that carries a memory row
  is rejected.

- HM effect types no longer use `NY_TYPE_EFFECT`: built-in effect masks
  translate to canonical `std.effects.*` row labels, while capabilities have
  their own type kind. Effect-row serialization is versioned,
  order-independent, duplicate-normalizing, and alpha-stable for open tails.

- Multiple names inside `@effects(cap(...))` now become distinct
  `ny.capability.<name>` row labels instead of duplicate `cap` labels.

- Polymorphic `@effects(e)` contracts now check known concrete body effects
  against the annotation's fixed builtin portion instead of skipping the
  contract check. Direct IO outside `e` is rejected on all four backends, both
  with and without a callback invocation; `@effects(io, e)` remains valid for
  a wrapper that performs IO and invokes an effect-polymorphic callback.

- Effect variables allocated for function signatures now receive unique IDs
  outside each HM substitution table's local index range. Previously, two
  independent signature variables both used ID zero, allowing `ny_subst_apply`
  to copy one callback's row into another signature.

- Closed the comptime mutable-capture check across module globals and locals
  from an enclosing function, with the same stable diagnostic. A positive
  control confirms mutation of a binding created inside comptime is allowed.

- The comptime IO call-graph scan now rejects incomplete analyses instead of
  treating an unvisited helper tail or unknown callback as IO-free. Its depth
  and step bounds produce the same dedicated diagnostic as unresolved callback
  effects. A 130-helper chain ending in IO and an isolated unknown callback
  both fail on NYIR, LLVM MCJIT, LLVM ORC, and native-only when unknown
  summaries are allowed.

- AST reflection serializes the origin, and template cloning preserves it for
  nested compile-time expressions.

- AST reflection now recursively serializes quoted bodies and expressions, and
  identifier records include their syntax context.

- Recursive `@total` edges now need both a strict measure decrease and proof
  that the caller's measure is greater than `INT64_MIN`. This rules out an
  indefinitely decreasing signed measure while allowing bounded negative
  domains. This remains a restricted integer proof, not general termination
  verification.

- Fixed the recursive lower-bound builder to return its constructed predicate
  rather than a boolean null value.

- Recursive `decreases` now accepts fixed tuples of signed integer terms and
  proves lexicographic descent plus a finite lower bound on each recursive
  edge. Tuple/list expression substitution now preserves formal-to-actual
  substitution inside measures.

- Caller-side `requires` checking now walks predicates through calls and
  member expressions, substitutes named and positional actuals, and asks the
  fast evaluator to prove predicates outside the bounded integer fragment when
  they become compile-time exact.

- Effect type unification now treats `NY_FX_ALL` as the exact set of built-in
  effects, not a wildcard that can unify with `pure` or another narrower set.
  When an effect carries a capability identity, unification also checks its
  mask and exact capability name, so a named capability cannot unify with a
  builtin mask or a different named capability.

- Loop unswitching now conservatively declines loops with multiple distinct
  exit blocks until cloning remaps each exit and its PHIs independently.

- Native equality now follows the emitted producer class when source inference
  misclassifies an immediately invoked untyped lambda result. Its tagged
  return is compared through the tagged-value path rather than against a raw
  integer literal.

- Integer proof evidence now distinguishes an exact `int64` literal from a
  saturated parser payload. Out-of-range source spelling remains distinct in
  canonical propositions; contract, range, and compile-time evaluators do not
  treat the saturated value as exact evidence. A violated callee precondition
  behind an `INT64_MAX + 1` literal guard previously compiled and aborted at
  runtime. It now rejects before execution. A false constant equality between
  `INT64_MAX` and that wider literal likewise rejects on NYIR, LLVM MCJIT,
  LLVM ORC, and native-only.

- Row unification now keeps a closed tail closed, uses the same variable
  storage for ordinary and effect variables, and allocates fresh residual
  variables in the input row's arena. Distinct open tails retain their
  respective missing labels, and matching a label no longer removes it from a
  shared input row. These checks do not close the remaining P1
  language/interface work.

- Equality on a promoted `list<int>` element was reaching `rt_any_eq` after
  sequence classification, even though the loaded element and scalar peer use
  raw i64 ABI; the tagged comparator can mistake odd raw payloads for tagged
  immediates.

- Type validation rejects unsupported `comptime` bodies that have proven IO,
  preventing the LLVM snapshot or native NYIR fallback from leaving that IO
  executable at runtime. A four case negative matrix passes NYIR, LLVM MCJIT,
  LLVM ORC, and native-only.

- Loop interchange now accepts invariant stack reads when several direct
  pre-loop stores target the same nonescaped allocation and the final store
  dominates every reordered read, including when an earlier store is
  conditional and the dominating overwrite is unconditional.

- Loop interchange now admits multiple distinct, private, in-bounds, injective
  affine stack stores in one reordered nest. This expands legality only; no
  performance gain was measured.

- Affine-nest optimization now splits a scaled inner-loop address into an
  invariant row base and a scaled induction value before LICM.

- Effect-row inference now follows straight-line local alias chains captured
  by a factory's returned lambda. Open callback rows inside an `@effects(e)`
  helper also survive a two-link local alias chain and forwarding the alias to
  another row-polymorphic helper. Returned aliased wrappers are checked for
  their effect rows but not invoked at runtime.

- The x86-64 NYIR emitter now uses memory-source operands for dynamic i64
  add/subtract/multiply/and/or/xor and signed comparisons when the right-hand
  value has a stack home. Dynamic shifts load the count byte directly into
  `%cl` from its home instead of staging it through `%r10`. These remove
  temporary RHS register moves while preserving immediate, fused-constant, and
  fallback paths. Packed f64 add/subtract/multiply/divide likewise use a
  memory-source operand instead of loading the right vector into `%xmm1`. A
  neighboring sample had nearly identical counters but different wall time, so
  - The x86-64 NYIR emitter now stores packed f64 values in a contiguous
  two-word home and uses one unaligned 128-bit move to reload/store both
  lanes. Previously those vector operations stored only the low lane, then
  reloaded 128 bits for packed arithmetic. Assembly shows packed stack
  `movupd` transfers with `addpd` and `mulpd`.

- The proof-parser bridge now includes `base/util.h` directly for its
  `ny_strdup` dependency; this restored release builds after the parser
  amalgamation began compiling that source.

- NYIR now classifies `rt_any_to_i64` as a read-only runtime call. Captured
  AOT assembly no longer calls either helper.

- Its observable global-writing loop callback also weights each input by its
  call sequence, pinning both call count and order.

- LLVM constant-folded the fixed ARX kernel to its pinned return value while
  NYIR retained the loop; both kept the guarded signed divide, list mutation
  and alias runtime operations, and the effectful loop's state updates (LLVM
  inlined its callback).

- Expanded `effects-row-polymorphism.nshape` from native NYIR to all four
  effect-policy routes (NYIR, LLVM MCJIT, LLVM ORC, native-only), using
  `--effect-require-known` on each.

- A matching `||`-guard negative ensures the proof does not extract one side
  of a disjunction.

- User-defined `@effects(e)` helpers now substitute effects from directly
  invoked `fnptr` parameters at positional and named call sites, inline
  lambdas, local aliases of direct function values, and zero-argument returned
  callbacks. The callback row contributes to the caller's inferred effects,
  and an `@effects(none)` caller is rejected when the callback performs IO.
  Straight-line reassignments update the current callback row; direct local
  callback reassignment in `if`, loop, and `match` control flow also joins
  rows. Four-route underdeclared-caller cases for each flow shape diagnose the
  inferred IO row.

- Caller-side `requires` transport now drops its premise when the function
  body reassigns a premise parameter.

- Caller-side `requires` transport now substitutes omitted integer defaults
  when the default is a plain signed integer literal, an immutable integer
  global resolved in the callee's module, or a supported arithmetic expression
  whose terms resolve to signed integers. The evaluator reads resolved global
  bindings by canonical name, including module-qualified bindings. Resolution
  declines if the caller declares a same-name local; unknown, mutable, or
  noninteger defaults remain fail-closed. HM now resolves bare global names
  against an enclosing module owner when that module exists, so the compound
  module default passes strict type checking. Chained omitted defaults now
  substitute each earlier bound parameter during proof transport and during
  LLVM and native NYIR call lowering, so a later default such as `limit =
  upper + 1` uses the actual `upper` value instead of resolving it as a module
  global. Negatives reject forwarding without a caller-side range proof and
  ensure a caller local with the same name cannot be mistaken for the callee
  global; another negative confirms a mutable module global is never treated
  as a proof constant; another negative confirms the `value + 1` default is
  not proved from a positive-only premise that allows signed overflow.

- Shared `requires` transport now retains supported parameter-only facts from
  `if` and `while` guards when other conjunction terms refer to local values.
  It still rejects an unsupported disjunction branch, since that branch may
  satisfy the guard without establishing the retained fact.

- Extended shared contract transport to guarded symbolic integer arithmetic.
  Each intermediate needs a checker-validated i64 overflow proof before its
  mathematical value can discharge a precondition; short-circuit premises are
  validated before reuse. Both independent linear kernels retain checked
  128-bit constants for endpoint differences, while coefficients stay bounded
  i64. The certificate parser now accepts `INT64_MIN` without accepting an
  out-of-range positive magnitude. Existing overflow, division-by-zero,
  custom-operator, rebinding, shadowing, and argument-substitution negatives
  remain rejected.

- Inlining now uses canonical function identity rather than ambiguous leaf
  names, and persisted call targets retain interned names after typing arenas
  are freed. Dynamic call results and container loads preserve their actual
  scalar carrier instead of decoding or boxing twice. List setters detach
  shared receivers before crossing the dynamic runtime bridge; a mutable
  binding's initial list literal no longer proves its current length. Comptime
  functions retain an implicit final expression, and mixed-list formatting
  quotes string elements.

- Added a symbolic XOR translation-validation case: `NYIR_XOR_I64` is proved
  equivalent to `(a | b) - (a & b)`, and an OR-only result is rejected.

- Restored strict symbol ordering in both runtime ABI declaration tables by
  placing `rt_proof_policy_digest_hex` before `rt_proof_policy_solve`. The
  full optimizer suite remains unrun.

- Closed the interprocedural optimization TODO at its tested bounded scope.
  Unsupported memory/effect shapes retain conservative call barriers.

- Moved resolved binary representation and custom-operator return facts ahead
  of recursive native string classification. The `is_cstr` classifier now
  relies on those frontend facts and the structural string-`+` rule instead of
  starting overload lookup itself. Release rebuild passes; a fresh full
  `std.bc` profile no longer shows operator/type recursion among its leading
  costs.

- Replaced repeated module-body scans for imported functions with an ordered
  lookup through the compilation function index, filtered by module path and
  source file.

- Corrected native function lookup so namespace-only `use module as alias`
  bindings do not leak functions into bare-name resolution. This does not
  verify full-bundle completeness or reuse.

- Fixed strict `.long` lowering in two paths: raw comptime i64 constants now
  bypass a redundant BigInt conversion, and `Result<T, E>` match bindings keep
  the instantiated payload type across `rt_result_unwrap_raw`. This prevents
  aggregate payloads such as `list<int>` from being mistaken for raw integers.

- Constant global folding now evaluates a simple `comptime { return __main()
  }` initializer using the expression token's source-file identity.

- Evaluator label-PC cache signatures now include each label's instruction
  offset, so in-place layout changes cannot reuse targets computed from an
  earlier layout with the same label IDs and function size.

- Loop interchange now accepts only a narrow invariant stack-memory read: the
  load must directly use an 8-byte-or-larger `ALLOCA` address, one dominating
  `STORE_I64` must initialize it, and every address use must be that store or
  a read in the candidate loops.

- Added comptime interpreter support for list/tuple `get(key, default)` calls,
  including named arguments, negative indices, default zero, and explicit
  out-of-range defaults. Dynamic negative-index results and string-keyed
  dictionary reads now cross into the raw integer ABI through an explicit
  decode when their scalar type is proven. This fixes nested-list accumulation
  and typed dictionary arithmetic. Added primitive f-string formatting to the
  AST interpreter, with owned string results so evaluation does not depend on
  an LLVM arena. Nil formatting follows runtime stringification. The comptime
  list-rendering assertion now matches the runtime's quoted string
  representation. Fixed NYIR result classification so comptime static-list
  `get` values remain boxed at dynamic consumers.

- Extended comptime interpretation to resolve user functions without an LLVM
  codegen context and to evaluate dictionary literals, nested lookup,
  defaults, and duplicate-key last-write behavior. NYIR now honors
  `NYTRIX_COMPTIME_INTERP_ONLY=1` and reports unsupported forms at their
  source location instead of lowering them as runtime expressions.

- Expanded `tv_smt_semantics --scalar-opcodes` with independent constant
  references for wrapping i64 addition, subtraction, and multiplication, plus
  defined signed division/remainder boundaries. The long full TV/optimizer
  gates remain unrun as requested.

- Added a process-start measurement recipe that separates compilation from
  direct executable launch and distinguishes launch-to-exit wall time from an
  internal steady-state timer. `file` and `readelf` confirm ELF64 x86-64
  executable output plus native and LLVM relocatable objects. These are
  launch-to-exit samples, not pure startup or speedup claims. This is a
  separation sample, not a throughput claim.

- Kept the growing matrix and insertion-sort buffers local to their benchmark
  functions so BCE can prove the allocation lineage instead of treating a
  module-global symbol as an unknown alias. `cmov-sort` also uses a typed
  `list<int>` local to preserve element representation. Focused O3 traces
  prove all four sort indices and the flattened matrix index range `[0,9999]`
  safe under the successful length assertion.

- The pass keeps the write-capable call and pre-call load, forwards the
  immediately following load only for a matching actual address, and leaves an
  unrelated-address load and a load after an unknown mutator in place.
  Distinct SSA arguments with equal pointer values alias in the bounded store
  map. A read-only callee and a mismatched effect summary remain conservative.

- `nyir_ipa_constprop` now refuses to erase a call unless its verified
  call-site effect summary is exactly `NYIR_EFFECT_CALL`. Other memory-effect
  shapes remain conservative.

- IPA now specializes the scalar return of a bounded callee that stores
  directly through an unchanged formal pointer. It requires an exact verified
  observable-effect summary and retains the original write/trap call; DCE also
  preserves the call. The bounded exact-address load forwarding is described
  above and does not remove or relax that call barrier.

- `inline_general` now covers a known write-through-pointer callee. This
  verifies effectful clone preservation, not memory-model-based call
  elimination.

- Both NYIR inliners now retain a call when a verified call-site summary has
  effects not represented by the cloned body. A focused case previously lost
  `WRITE_MEMORY` while inlining an apparently pure scalar callee; it now keeps
  the call through both `nyir_inline_small` and `nyir_inline_general`.

- A successful `rt_assert_cstr` is now treated as a true-edge lower-bound
  guard for a typed-buffer length only when the checked value traces to that
  buffer and the length remains stable through the bounds check.

- Retried `std_bc` with a process memory limit. This now gets past the earlier
  OOM point and exposed additional GF(2) NYIR ABI mismatches. The full stdlib
  compile currently fails optimized verification because a tagged `vectors`
  local is passed to a typed-list formal.

- Expanded the comptime AST interpreter to evaluate ordinary non-variadic
  function calls with positional/named arguments and defaults, preserve
  lexical block scopes, and execute bounded iterator-style and C-style `for`
  loops under the shared fuel budget. Hardened comptime integer evaluation
  against signed division/modulo overflow and C signed-shift undefined
  behavior.

- Fixed the no-LLVM `ny-dap` link by adding the shared debug logger and
  tool-only NYIR stubs used by the other small tools.

- Expanded the proposed effect-row contract with canonical library-owned
  identities, callback-row substitution, versioned serialization, provenance
  diagnostics, and backend acceptance requirements. This is a design target;
  implementation still uses built-in masks and incomplete callback summaries.

- Quote splices now retain the caller syntax context; native lowering and HM
  binding, identifier, call, and member lookups use exact-context identity,
  preventing an introduced local from capturing a spliced caller identifier or
  its type. The comptime specification now distinguishes quote-based
  generation from the missing first-class runtime code value; `type(quote { 1
  })` currently reports `int`.

- `value.(field)` now resolves under a compile-time `#fields(Layout)`
  descriptor and expands to `load_layout(value, "Layout", "field-name")`;
  unscoped dynamic member access emits a source-located error instead of
  reaching codegen.

- `core.proof` documents the v1 obligation/certificate API and its bounded
  checker. The docs landing page links to the proof guide.

- Integrated `nyir_devirtualize` at the start of the O1–O3 pipelines, before
  verifier checkpoints and inlining. The pass now reads the canonical call
  argument vector and preserves per-argument scalar, ownership, contract, and
  aggregate-size metadata when moving a resolved function target into the
  closure ABI's first argument slot.

- Entry-global localization now accepts pointer-class loads only when the
  global is independently proven to contain an initialized typed-buffer
  handle. This lets the native O3 SOR loop reach the vectorizer and emit
  packed `movupd`/`addpd`/`mulpd` instructions.

- BCE tbuf-root provenance is memoized per function, with scratch storage
  allocated only when a dynamic-length check reaches the guarded-proof path.
  Length forwarding scans for mutations only up to the header load; later
  mutations cannot alter the already-loaded SSA value used by the guard. No
  indexed-read proof or runtime speedup is claimed for `bce-matrix` or
  `cmov-sort`.

- BCE local-root resolution now ignores stores in CFG blocks that cannot reach
  the queried load, while retaining an over-approximation for reachable
  blocks. Append and returned `rt_tbuf_set_i64_raw` values preserve only
  logical may-alias lineage; their mutation still blocks length forwarding.
  Trusted calls whose effects exclude writes, unknown effects, FFI, threads,
  and volatility are treated as read-only observers even when they may trap.
  The indexed-read checks remain, and no runtime speedup is claimed.

- Runtime tbuf bounds checks can also use a path-isolated comparison against
  `rt_tbuf_len_raw` when the queried index range fits the resulting minimum
  length and intervening mutations cannot shorten the buffer. A false `len ==
  minimum` edge is explicitly rejected: inequality alone does not establish a
  lower bound.

- Emitted production assembly uses sign-bias plus arithmetic-shift lowering
  for signed power-of-two division and remainder reconstruction; LLVM uses its
  reciprocal-multiply sequence, while dynamic division retains `idiv` on both
  paths (LLVM also emits a narrow-divisor fast path).

- Moved incremental compilation sources under
  `src/code/backend/wire/incremental/` and updated the CMake source glob and
  both callers. The relocated wire amalgamation now includes `<elf.h>` for its
  Linux x86-64 tiny-ELF emitter.

#### Affine loop interchange and multi-exit unswitch

- Extended interchange to permit one store per inner-body iteration into a
  nonescaping stack allocation when its byte address is affine in both
  induction variables, its full-width access stays in bounds without signed
  intermediate overflow, and the two-dimensional address map is proven
  injective. Unsupported effects, aliases, recurrences, and unproved address
  ranges remain rejected.

- Broadened natural-loop unswitching from one shared exit to multiple resolved
  exits. Cloned exits retain their own targets, fallthrough bridges select the
  matching target, and each exit PHI receives cloned predecessor/value inputs.
  Added a two-exit loop whose selected exit PHI consumes a loop-carried IV;
  both invariant outcomes preserve their results after cloning.

- Extended unswitching to natural loops with multiple direct latches when all
  induction-PHI backedges prove the same constant step, even when each latch
  computes a distinct SSA update. The pass unions the latch-specific natural-
  loop members, clones/remaps all latch labels and PHI inputs, and rejects
  mismatched steps or noncanonical latch branches.

- The unselected call stays unexecuted; when selected, its trap and one-call
  count are preserved before and after cloning.

- Added a fallthrough exit without an explicit label; unswitching fails closed
  and preserves its evaluated result and original IR length.

- General indirect-exit cloning is still unsupported.

- Rebuilt with `./make ny`.

- Wrapped the late O3 loop-vectorize call in the default-off
  `NYIR_ENABLE_LATE_LOOP_VECTORIZE` build gate. O2 vectorization remains
  enabled.

- The post-gate native assembly uses a 608-byte frame and scalar
  `movsd`/`addsd`/`mulsd` operations with stack-home traffic; the `cc` loop is
  no longer vectorized in this O3 path. The performance loss is accepted
  containment, not a parity fix.

- The timer spread reinforces that these samples are correctness smokes only;
  neither the historical PBKDF2 regression nor the SOR native gap is fixed.

- Restored and rebuilt the default-off gate.

- Closed-form IV elimination now fails closed when a counted loop has an
  additional exit besides its induction guard.

- The direct ELF writer now keeps functions containing any backedge in their
  canonical NYIR value homes instead of assigning registers from textual
  intervals. Loop bounds and values live across one iteration could otherwise
  be reused for comparison results; this also broke `continue` in a `for`
  loop. Reference-checked nested-loop sums and `for`/`while` loop-control
  cases pass with the conservative fallback.

- The self-contained C frontend lowers `continue` to the `while` header or
  `for` step block and `break` to the current loop exit. Both forms are
  reference-tested across empty, short, and larger bounds. A nested-loop
  reference fixture for each loop kind proves `break` and `continue` affect
  only the inner loop. Using either statement outside a loop remains a located
  diagnostic.

- Linux automatic window backend selection now reports unavailable when both
  `DISPLAY` and `WAYLAND_DISPLAY` are absent, instead of claiming successful
  availability or initialization for the `none` backend. Window creation
  returns false promptly; gamepad enumeration returns an empty list. An
  explicit `PLATFORM_NULL` hint or `NY_UI_BACKEND=none` remains an intentional
  separate backend selection.

- The Linux joystick module now uses `core.os.fs.watch_init`, `watch_add`, and
  `watch_rm` for inotify.

- Effect-row composition now structurally unifies payload types when the same
  label appears in both rows.

- Mutable list refinement transport no longer treats every function spelled
  `append` as the standard list operation. The generated mutable-flow
  differential now includes the shadowed-call boundary.

- Structural `decreases` proofs now follow immutable single-name local aliases
  of a same-enum constructor payload, including alias chains.

- Structural `decreases` now tracks every same-enum payload binder in a
  constructor arm, not just the first one.

- Structural descent can lead a tuple `decreases` measure.

- Pure contract expansion now handles finite literal statement-form matches
  with a default arm and branch returns.

### 2026-10-06

- Corrected pointer access to the caller-owned SMT environment in the
  Presburger and Z3 proof paths.

- Fixed pointer status reads in the independent proof-certificate checker's
  Boolean branch expansion. The C policy API still has no production
  proof-site callsites.

- Explicit polymorphic effect contracts such as `@effects(e)` now count as
  known symbolic effects under the require-known policy; truly unannotated
  unknown callback effects remain rejected. The native-nyir fixture remains
  blocked on safe callback-row substitution; see the current status in
  [effect-row reference](spec/functions.md#effect-rows).

- `ipa_constprop` now tests its aggregate interpreter work budget across two
  call sites: each callee requires 50,003 instructions, the first folds, and
  the second stays out of line after the shared 100,000-instruction budget is
  exhausted. It also folds a bounded four-trip CFG loop with two header PHIs
  to 6. The bounded scalar interpreter handles `NYIR_SELECT_I64` in a callee,
  and caller constant collection propagates a known `SELECT_I64` argument.

- Loop interchange now admits only a plain `LOAD_LOCAL` effect when the nested
  bodies contain no writes, traps, calls, or memory accesses. Another case
  gives a body call the `CALL|FFI|UNKNOWN_SIDE_EFFECT` summary and likewise
  checks that its loop schedule does not change. A third case adds an ordinary
  carried PHI to the inner loop and checks the result, bounds, and instruction
  count stay unchanged. A nonvolatile, potentially trapping load from a local
  allocation also stays ordered. Interchange also requires both SCEV trip
  counts to be known finite and non-overflowing.

- Each of 256 cases uses a reproducible seed and 24 generated steps; original
  and rewritten expressions are compared at eight inputs, including signed
  boundaries.

- Scalar calls with exactly known `NYIR_EFFECT_CALL`, `CALL | READ_MEMORY`,
  `CALL | WRITE_MEMORY`, or `CALL | READ_MEMORY | WRITE_MEMORY` effects use
  shared uninterpreted results. Read results key on the input byte array;
  writing calls also produce a shared next-memory array from arguments and
  prior bytes. Captured secondary returns remain congruent for supported
  non-writing calls. Exact `CALL | MAY_TRAP` calls now carry shared
  definedness keyed by callee and scalar arguments; repeat calls are congruent
  and erasing a potentially trapping call is rejected on straightline and CFG
  paths. Unknown, volatile, conflicting, and other unsupported summaries
  remain inapplicable rather than being assumed pure. CFG allocation sites are
  admitted only when SCEV proves every enclosing counted loop executes at most
  twice, keeping path exploration bounded. Unknown-trip and larger cycles
  remain inapplicable. Fixed-size `NYIR_COPY_STRUCT` is checked against scalar
  loads/stores in straightline and CFG paths, including changed-word
  rejection. All eight exact `CALL` summaries formed by `READ_MEMORY`,
  `WRITE_MEMORY`, and `MAY_TRAP` are now accepted. Write-capable trapping
  calls share deterministic results and definedness and produce memory
  transitions keyed by arguments and incoming bytes; targeted read/write
  memory proofs pass. Unknown, volatile, conflicting, and other unsupported
  effects remain inapplicable. The interpreter and SMT model now process
  four-lane f64 loads, stores, arithmetic, FMA, shuffle, and reduction in
  straightline and CFG functions. The `--scalar-opcodes` group proves
  `SELECT_I64` equivalent to conditional control flow and both byte-swap
  opcodes against independent bytewise bit-vector constructions in
  straightline and CFG forms. Allocation shape matching permits differing
  function lengths when each positive-size allocation remains at the same
  instruction index; the bounded write-capable trap selector covers this case.

- Native C imports now box signed 64-bit scalar returns at dynamic boundaries,
  and `store64_i` avoids unboxing an out-of-range integer literal twice when
  `rt_any_to_i64` already produced raw i64 bits.

- The native runtime ABI table now classifies `rt_flt_box_val` results as
  tagged values. An inferred f64 local initialized through the boxed
  `float(any)` wrapper therefore unboxes at the local's f64 boundary instead
  of bitcasting a boxed pointer to `double`.

- HM call-target persistence now prefers a non-stdlib declaration owned by the
  current module for an unqualified same-module call.

- Declared user-defined value-indexed types now erase through their declared
  base ABI in type and native ABI resolution. HM call checking preserves index
  identity while accepting the erased base value, and compile-time integer
  arguments are checked against literal or immutable symbolic bounds.

- Explicit `i64` variable declarations now accept constant integer
  initializers throughout the signed 64-bit range, even when unhinted literal
  inference classifies their magnitude as BigInt. The change is limited to
  type unification for checked constant initializers; BigInt materialization
  and arithmetic overflow promotion remain unchanged.

- An inclusive `row <= 6` guard proves the same final strided row and
  preserves result 232; its wrapping counterpart remains scalar and preserves
  result 52. An unknown-effect call in the inner loop remains scalar and
  executes 16 times before and after the pass. A two-latch outer loop now
  vectorizes when both edges carry the same `+1` update, preserving result
  136; it also vectors under an inclusive `row <= 3` guard, and uniform `+2`
  latch edges vectorize under `row <= 6`. Differing `+1`/`+2` edge updates
  remain scalar and preserve result 94. A shifted same-buffer store
  overlapping later loads also remains scalar and preserves result 84.

- Loop interchange now rejects loop bodies with trap, volatile, I/O, thread,
  FFI, local-state, allocation, or unknown-side-effect effects. Its optimizer
  regression keeps a volatile/trapping read loop's iteration order unchanged.

- `nyir_loop_unswitch` now clones a restricted single-latch natural loop with
  an identified induction PHI, additional header recurrences, an invariant
  in-loop branch, and one shared exit. Cloned value IDs, labels, and internal
  and exit-PHI predecessor labels are remapped; external fallthrough is
  redirected to the shared exit. A false-arm divide-by-zero stays unexecuted
  when the invariant condition selects the true arm, and returning an
  induction value after the loop keeps the transform disabled.

- Dynamic append-length propagation remains unresolved.

- SCEV now derives in-body bounds for a unit-step nested induction variable
  whose initializer is affine in a dominating outer induction variable. A
  check on the guard's false edge also remains. Stack-backed accesses preserve
  results 10 and 1 before and after elimination for the ordinary and
  negative-initializer cases. No speedup or production bounds-check removal is
  claimed.

- Translation validation now models acyclic CFG allocation sites and rejects
  allocation sites inside backward-edge cycles.

- The SMT TV model now evaluates positive-size `NYIR_COPY_STRUCT` as a byte
  copy in straightline and path-sensitive CFG functions. It requires valid
  source/destination ranges that do not overlap, matching the copy's `memcpy`
  contract. `tv_smt_semantics` proves a 16-byte copy equivalent to scalar
  loads/stores over disjoint stack allocations and rejects a changed copied
  word. The same SMT paths now map `NYIR_ADDR_SYMBOL` to a consistent symbolic
  address; same-symbol loads prove equivalent, while changing the symbol is
  rejected. Scalar calls with exact known-pure or read-only effects use shared
  uninterpreted results; read-only calls are keyed by arguments and the
  current memory array. `tv_smt_semantics` proves duplicate-call congruence
  and detects different results when writes move across a read-only call.
  `NYIR_CAPTURE_RET` uses a shared selector-indexed result in both evaluators
  for supported calls; repeat captures prove equivalent, while changed
  selectors or arguments produce counterexamples. Calls with the four exact
  pure/read/write effect summaries carry their modeled memory transition
  through straightline and CFG evaluation; writing calls cannot be captured as
  secondary returns yet. Exact `CALL | MAY_TRAP` calls share argument-keyed
  definedness in both evaluators. Additional memory effects, unknown,
  volatile, conflicting, aggregate, and sret calls remain inapplicable.

- `NYIR_ADDR_LOCAL` now maps address-taken local slots into the shared
  symbolic byte memory in straightline and CFG evaluation. Direct local loads
  and stores agree with indirect memory operations; an eight-byte out-of-range
  load is rejected.

- The restricted `@total` countdown proof recognizes equivalent spellings for
  the `n <= 0` base (`0 >= n`, `n < 1`, `1 > n`) and the additional valid `n <
  0`/`0 > n` base guards; recursive calls may use positive integer-literal
  decrement steps. Overloaded comparisons/decrements, zero-step recursion,
  increasing recursion, and unmeasured recursion remain rejected.

- Failures included the `compiler/optimizer/vector-f64.nshape` result-oracle
  mismatch. The native list-shape proof did not recognize method `.append` and
  treated `.pop()` as an unsupported mutation, leaving a raw slot and tagged
  pop result mixed at `rt_any_add`. The standalone benchmark phase was
  skipped.

- Its direct native smoke prints `20 22` and `42`, confirming the indexed raw
  integer is correctly combined with the value returned by `pop()`.

- The native float-cast lowering now checks the raw-int ABI proof first and
  emits numeric i64-to-f64 conversion; dynamic tagged values still use
  `rt_any_to_f64`.

- Fixed the native-only qualified `core.len([1, 2, 3])` path: semantic
  completion had canonicalized it to `core.any.len` and included the `core`
  module expression as an implicit receiver. Native lowering now removes only
  a verified stdlib module-prefix receiver before checking the method's source
  arity. The build and clangd diagnostics for `lower.c` pass.

### 2026-10-05

- All execute to the known result 136 before and after the pass. A step-two
  induction wraps from `INT64_MAX - 1` to `INT64_MIN`, safely processes the
  next row, returns 52, and remains scalar. No `sor` performance or broader
  loop-transformation completion is claimed.

- Audited NYIR mutations that change alias/range dependencies. ISLE strength
  rewrites, memory-SSA forwarding, mem2reg, PHI pruning, double-negation, and
  private-entry localization now use shared metadata-preserving constructors
  or clear derived facts.

- Defined NYIR call memory effects as a conservative top set: `READ_MEMORY`
  and `WRITE_MEMORY` include all existing memory possibly reachable through
  arguments or globals, and carry no per-object alias set or bounded extent.
  Audited GVN/PRE, CSE, redundant-load elimination, LICM, and DSE against that
  contract; calls with unknown provenance or extent remain barriers. No memory
  barrier was relaxed by that audit; the later bounded GVN exception is
  recorded below.

- Straightline SMT now gives fixed-size `ALLOCA` instructions
  pairwise-disjoint symbolic regions; constant derived-pointer offsets are
  checked against each accessed width, while unsupported allocation
  shapes/provenance remain inapplicable. External pointers may still alias
  those regions and retain unconstrained validity.

- Configured OMP to use the project-built `ny-lsp` for `.ny` files. Fixed
  positional JSON integer parsing and NY function-signature hover formatting;
  rename now emits valid workspace edits directly instead of reconstructing
  nested ranges.

- Corrected the existing HM row unifier so an empty closed row no longer
  unifies with a non-empty row, and repeated labels normalize only when their
  field types unify.

- Removed the unused NYIR raw-call write-argument mask: at that stage no
  optimization consumer had a bounded write-extent proof, so the mask could
  not justify weakening memory barriers. The bounded GVN exception is recorded
  below.

- GVN now uses constant `rt_memcpy_raw`/`rt_memset_raw` destination ranges
  against provenance-rooted eight-byte loads. It forwards reads across
  disjoint ranges and zero-length calls, while overlapping, dynamic-length,
  and unknown-provenance calls remain barriers. Added negative-length (`-1`,
  `INT64_MIN`) fill/copy cases to pin the runtime no-op boundary.

- Fixed BigInt-to-`int` conversion at the dynamic ABI boundary, where
  heap-backed BigInts had been mistaken for pointer values. Thread callback
  helpers now match their LLVM/native call signatures, preserve tagged
  callback results, reconstruct tagged values from descriptor buffers, and
  carry mixed dynamic/raw parameter layouts for heterogeneous callbacks.

- The x64 object encoder now keeps values in the same physical register across
  a regalloc-proven unique CFG edge when the only external read is its edge
  copy. It omits the redundant canonical-home store but retains homes for
  returns, call arguments, and other cross-block reads; allocating safepoints
  flush resident GPR values to canonical homes. The proof is cached per live
  segment.

- `@total` proves one guarded direct integer countdown with `decreases(n)` and
  built-in subtraction, both with a following recursive expression and an
  explicit `else` return; increasing, overloaded-decrement, and unmeasured
  recursion fail closed. HM now preserves the canonical declaration for
  resolved named calls; shared typing proves that restricted implication
  before native lowering instead of relying on backend-local inference. The
  shared proof shortcut is disabled when the caller reassigns a formal or
  introduces a binding that shadows its name, including local declarations,
  iterator variables, and lambda parameters. The caller's entry precondition
  cannot justify a call using a changed or shadowed binding. The call-site
  contract solver now rejects variable-containing or overloaded integer
  arithmetic as unknown and still accepts constant arithmetic when its
  intermediate results fit `int64`. Dynamic method dispatch still fails
  closed. The proof fragment intentionally fails closed outside integer terms,
  arithmetic/logical formulas, positional or complete named calls, and
  built-in operators.

- Comptime AST interpretation now consumes the same compilation-wide fuel
  budget as the fast evaluator. Removed the independent 500,000-step limit and
  100,000-iteration while guard; fuel exhaustion now fails compilation with a
  source-located diagnostic instead of returning a runtime value.

- Continued the staged LLVM-legacy retirement without claiming completion.
  Preparation/link handling is in `prepare.c`/`prepare_links.h`; comptime-if
  dispatch, fast comptime expression evaluation, string initialization,
  layout/signature collection, attached call lookup, and ownership mutation
  analysis have been moved to `comptime_if.c`, `comptime_fast.c`,
  `string_init.c`, `layout.c`, `sigs.c`, `gencalltypes.c`, and
  `typing/ownership.c`. `comptime_fast.c` now also owns platform/main-mode
  queries, fuel accounting, unary/binary fast evaluation, binding constant
  lookup, bigint/long-property conversion, sequence/range construction,
  fast-value ownership, and tagged conversion. Expression-kind profiling
  state/reporting moved to `expr_profile.c`. The fallback comptime interpreter
  now lives in `comptime_interp.c`; LLVM expression lowering and
  `gen_comptime_eval` remain in `legacy/expr/core.c`, which is still called by
  `comptime_if.c`. The legacy expression unit is not yet deletable. The
  legacy-emission directory remains. The thread wrapper is now resolved as a
  stdlib function before the direct-call intrinsic path; this advances
  `std_bc` past its earlier `thread_spawn_call` lowering error. The latest
  automatic and serial `std_bc` precompile attempts were killed while
  compiling `std.bc`; neither produced bitcode. The last detailed compiler
  diagnostic before those attempts was NYIR verification in
  `core.math.crypto.lattice.lll._lll_rel_diff` (an f64 value stored with a
  non-float local ABI class). The no-LLVM Ninja compiler object target builds,
  but this configuration provides no full compiler executable.

- MCJIT REPL perf maps now reset when the engine is rebuilt and append symbols
  from each submitted module instead of retaining only the initial top-level
  entry. Supported object formats remain unvalidated.

- Native local stores now stamp known ownership from the stored value's
  producer. If a mutable local receives a different known ownership than its
  earlier value, the local fact becomes unknown so later loads do not claim
  one ownership across mixed paths.

- Fixed native verification and execution for unparameterized list elements
  passed to raw-int formals. An `elem_size == 8` guard proves slot width, not
  that a generic element is a raw integer; both arms now retain the tagged
  class at their temporary-slot join unless the source contract proves
  `list<int>`.

- Reviewed the related native lowering, x64 allocator/emitter, and runtime
  pointer bridge changes; removed superseded task-local probes and captured
  dumps. The individual fixes remain documented with their owning changes
  below and in earlier dated entries.

- Removed `--legacy-llvm-dump` (the first step of the legacy-retirement Stage
  2): the option field, help entry, and parse branch are gone, the two
  JIT-cache conditions in `wire/pipe/init.c` no longer consult it, and the
  legacy `codegen_emit` branch in the LLVM pipe is deleted — leaving the
  unified NYIR-to-LLVM path as the normal program emitter. The comptime legacy
  LLVM expression path remains active. This orphans
  `codegen_emit`/`codegen_emit_script` (their last external caller), the
  designed precondition for deleting the legacy emission TUs.

- The removed P0 items stay removed. Their temporary reproducers are not part
  of the public source tree.

- A temporary COW alias/append probe still showed a route difference after the
  list rework: native returned 314384 and the VM returned 182928. This is a
  historical observation, not a current route check.

- Fast-math reciprocal factoring now marks its synthesized constant and
  reciprocal result as F64 and attaches the originating division location.
  Strict IEEE mode remains unchanged.

- Self-recursive tail-call conversion now carries the original call-site
  source location onto generated parameter stores and its loop-back branch.
  The pass now discards both unreachable originals, including the moved call's
  owned metadata.

- PHI elimination now attaches the PHI's source location to every generated
  predecessor-edge store, preserving attribution through the temporary-local
  lowering as well as on the replacement load.

- If-conversion now transfers the replaced PHI's scalar class, ownership,
  mutability, range, and source location to its `SELECT`; removed bridge
  branches retain their own location on the no-op. `if_convert` seeds all PHI
  facts and verifies the converted instruction preserves them with an
  unchanged result.

- CFG simplification now preserves the source location when removing a
  constant-false `BR_IF`.

- The unconditional branch retains its target, source location, `dst=-1`, and
  required control effect; the false branch retains its source location.
  `null_check_elim` verifies both rewritten shapes and results.

- Induction-variable elimination now rewrites an exit PHI to a copy through
  `nir_make_copy`, preserving its debug location, scalar class, ownership, and
  mutability while dropping facts tied to the removed incoming edges. Rebuilt
  `build/release/ny`; the `pass_order` and `loop_idiom_metadata` drivers pass.

- Redundant-load forwarding now uses the shared NYIR copy constructor,
  clearing stale load range/effect metadata while preserving value provenance.

- NYIR debug fingerprints now include source file, line, and column.

- Generic NYIR `nir_make_copy` rewrites now clear both range and alias facts;
  the helper cannot assume facts from the old producer apply to a different
  source value. GVN explicitly restores the replaced range only after its
  expression-equality and memory-stability proof. Extended `alias_contracts`
  with an incompatible seeded-range case. This closes that helper contract,
  not the audit of all direct IR mutations.

- GVN's fixed-width scalar load/store alias proof now retains checked constant
  offsets from a shared symbol, local, or alloca root. It reuses a load across
  an eight-byte-adjacent store, retains the load for overlapping four-byte
  separation, and stays conservative when the modular distance can overlap at
  address wraparound. Offset-adjusted accesses rooted at different objects
  remain conservative because no object-bounds proof exists.

- Correct rewrites prove equivalent, an omitted-lane mutation is rejected, and
  removing an otherwise-unused load is rejected when it changes whether the
  program is defined.

- GVN now keeps raw `rt_memset_raw` and `rt_memcpy_raw` as memory barriers:
  their explicit arg0 write-base summary does not bound the runtime write
  extent, so distinct symbolic bases alone cannot prove non-aliasing. The
  address-root proof also stops stripping nonzero pointer offsets.

- Its inventory covers all 229 runtime argument declarations and reports the
  sole unknown, `rt_syscall_raw`, whose raw machine-word operands
  intentionally have no ownership promise. This protects declaration
  integrity.

- Added `machine_copy_scaling`, a compile-only 36,002-instruction workload
  with 12,000 COPY pairs. The allocator keeps its conservative conflict rule;
  machine-home semantics need an explicit model before this affinity can
  safely alter physical assignment.

- Capped automatic benchmark fixture concurrency at four workers and now
  applies the same 8 GiB system reserve / 12 GiB per worker budget as the main
  fixture runner. Explicit `NYTRIX_TEST_JOBS` still overrides the automatic
  choice. This prevents large logical CPU counts from launching 24+ competing
  compiler/runtime fixtures by default.

#### Rechecked reduced havlak native parity

- Rebuilt the in-tree release compiler; `./make --color=never ny
  --no-progress` completed with Ninja reporting no work.

- The preserved `N=10` stateful traversal reproducer now returns identical
  `cycles=2`, `pass_id=6`, `next`, and `vis` values on native O0, native O3,
  LLVM O3, and optimized NYIR VM routes. The old native code-generation
  mismatch no longer reproduces on this checkout.

#### Restore proof-pipeline build after context rename

- Synchronized the requires-proof visitor's save/restore state with the
  existing `current_fn_params_rebound` fields and `tp_function_rebinds_param`
  helper in `typing/pipeline/emit.c`; this resolves the compile errors from
  stale `_mutated` names without changing the proof invalidation condition.

#### Follow returned-borrow contracts in BCE provenance

- BCE root tracing now follows a unique formal `returns-borrow` argument.

- Length forwarding accepts returned-borrow calls only when their trusted
  effect summary excludes writes and unknown side effects. Mutating append
  calls are not treated as identity-preserving because they can replace the
  allocation.

- Removed the unimplemented `nyir_loop_memset` pass hook and declaration after
  the release link failed on the missing definition.

#### Add conservative NYIR zero-fill loop recognition

- `nyir_loop_memset` replaces counted zero-store loops over sufficiently large
  fixed `NYIR_ALLOCA` objects with `rt_memset_raw` at O2/O3. It requires a
  zero-initialized unit-step induction variable, a store before the increment,
  no bounds check or escaping loop value, and an induction slot used only for
  initialization, loads, and increment.
- Unknown pointer provenance, small allocations, nonzero fills, and observable
  final induction values remain unchanged. The cross-backend fixture's
  bounds-checked loops are deliberate non-candidates. No speedup was claimed;
  broader high-level loop work remained open at this checkpoint.

#### Carry allocated values across non-adjacent CFG layout

- Same-value live segments can coalesce across a unique CFG edge despite
  non-adjacent instruction layout. Carry requires exact block-boundary
  segments, compatible value classes, and one runtime incoming edge.
- Conflicting target segments can be evicted only with a valid canonical
  home; carried values and division/remainder results stay protected. In the
  scaled `binary` kernel, v66 carries B11→B21 without a spill/reload while
  conflicting, non-carried v39 uses its home.

#### Optimizer call-contract value numbering

- GVN/PRE call keys now hash and compare the packed per-argument actual
  ownership and formal borrow/consume contracts. This prevents two pure calls
  with identical operands but different ownership behavior from being merged.

#### Exact packed integer-vector validation

- Added exact per-lane bit-vector translation for two-lane packed i64 add,
  subtract, and/or/xor, and masked left/arithmetic-right shifts. The model
  matches NYIR's wrapping arithmetic and shift-count semantics.

#### Four-lane integer memory translation validation

- Extended the Z3 model to interpret NYIR `VEC8_I64` symbolic loads as four
  consecutive 64-bit reads, including per-lane address validity, then reduce
  all four lanes with wrapping i64 addition.

- Added straightline and branch-CFG solver comparisons against four scalar
  loads, plus negative cases omitting the final lane.

- Added four-lane packed add, subtract, bitwise-and, bitwise-or, and
  bitwise-xor proofs against four scalar load/op pairs; these exercise
  lane-wise SMT arithmetic in addition to load/reduction semantics.

- Modeled the currently implemented two-lane `VEC4_F64` load, set, arithmetic,
  and reduction operations with IEEE-754 round-to-nearest semantics. A
  memory-backed reduction matches two scalar f64 loads and rejects a missing
  lane after conversion to a defined i64 result. Broadcast/add/reduce also
  matches scalar f64 arithmetic and rejects a missing lane.

- At this stage of the work, memory was still read-only; the following update
  records the scalar-write extension.

#### Path-local scalar memory in translation validation

- Replaced the translation validator's immutable read function with one shared
  symbolic byte array.

- Modeled little-endian 64-bit stores and byte stores, including overlapping
  loads and separate byte/full-width validity predicates.

- Vector stores and concrete allocated-object validity remain unsupported; the
  abstraction does not claim that an address is allocated or writable.

#### Integer vector-store translation validation

- Added path-local stores for the two-lane `VEC4_STORE_I64` and four-lane
  `VEC8_STORE_I64` operations. Every lane uses the shared byte-array memory
  state, and each store carries validity for its complete byte range.

- Added SMT comparisons against scalar load/store sequences for both widths;
  removing the final scalar lane store is rejected. Repeated this comparison
  across branch CFGs, exercising forked path-local memory state through the
  join; omitted-lane cases are rejected there too.

#### Floating vector-store translation validation

- Modeled the existing two-lane `VEC4_STORE_F64` operation against the shared
  symbolic byte array, including validity for both 8-byte lanes and path-local
  state across CFG branches.

- Added straightline and branch-CFG equivalence checks against scalar
  load/store sequences. Both reject a rewrite that omits the second-lane
  store, including when source and destination addresses may alias.

- `VEC4_F64` remains two lanes in the current NYIR/LLVM contract, so this does
  not close the separate true-four-lane-f64 or concrete
  allocated-object-validity work.

#### Fused floating-vector operation in translation validation

- Added exact round-to-nearest `VEC4_FMA_F64` semantics to the SMT evaluator.
  The solver proves its two-lane result against scalar multiply/add for exact
  operands, rejects an altered addend, and distinguishes fused rounding from
  separate multiply/add rounding at a halfway product.

- Added fused two-lane `VEC4_FMA_F64` evaluation to `nyir_eval`. The bounded
  F32 vector subset is documented in the following section.

#### Four-lane float32 translation validation

- Modeled the existing four-lane `VEC8_F32` load/store, splat, and packed
  add/subtract/multiply/divide operations with shared byte memory and
  round-to-nearest IEEE-754 semantics.

- `nyir_eval` now supports every existing four-lane f32 vector opcode:
  load/store, splat, add/subtract/multiply/divide, fused FMA via `fmaf`, and
  shuffle. The solver support gate explicitly admits the modeled opcodes on
  its SMT path. Arithmetic cases have straightline and CFG
  scalar-reference/rejection checks.

- A distinct-lane shuffle is checked against a scalar byte-store reference,
  and an identity shuffle is rejected. An adversarial product/addend pair
  distinguishes fused from separately rounded multiply-plus-add.

- The bounded shuffle path reverses distinct input lanes and rejects
  identity-shuffle substitution.

#### Removed unreachable constant-branch rewrites

- Deleted two constant-branch mutation blocks in `const_fold.c` that were
  guarded by `false`; active constant-edge simplification remains in the
  SCCP/CFG cleanup path.

- `alias_contracts` and `cfg_simplify` passed after the cleanup.

#### Raw memory-call effect contracts

- Classified `rt_memcmp_raw` as read-only and potentially trapping in NYIR;
  GVN reuses a stable load across it, while CSE retains identical raw
  comparison calls. The shared builtin-effect registry now reports `read` for
  raw memcmp, `read|write` for raw memcpy, and `write` for raw memset.

- Corrected the tagged `__memcmp` and `__memcpy` builtin rows: comparison
  reads argument memory, and copying reads its source as well as writing its
  destination. The public `core.memcmp` wrapper retains the inferred read-only
  effect.

- Added negative checks for omitted tagged `__memcmp` read, `__memcpy`
  read/write, and `__memset` write effects.

#### CFG path-local memory validation

- The two-arm rewrite proves equivalent to full-width stores on both arms; a
  one-byte partial write is rejected.

### 2026-10-04

- Tightened guard-derived induction ranges in the native loop vectorizer: a
  two-input PHI is now refined only when its incoming CFG edges are exactly
  one explicit backedge and one non-backedge predecessor, the header has no
  additional predecessors, and the update is defined in the latch block. This
  closes an ambiguity in dominance-based edge classification; it does not
  broaden the proof to multi-latch or irreducible loops.

- Recorded guarded-rotate output contains ten immediate `rorl` instructions.

- Shared compile-range validation now proves descending canonical `while`
  induction intervals for `i > bound` and `i >= bound` with direct constant
  decrements. The validator requires one induction-variable write, an
  effect-safe body, a known start, and a decrement that cannot overflow.

- Fixed inferred integer locals initialized by `__flt_to_int`: the intrinsic
  emits raw i64 in native code, but expression classification had inherited
  the public runtime entry's tagged return class. This mismatch boxed the
  value at the local and tagged it again after the branch join (`480` became
  `961`).

- Extended SMT translation validation to model `NYIR_BOUNDS_CHECK` as a trap
  unless its signed byte offset is nonnegative and below the static or dynamic
  byte length.

- Closed the guarded-division-after-inlining regression with a cold
  eight-route optimizer fixture. The stale P0 entry was removed from
  `docs/TODO.md`.

- Made the SSA unroller's multi-block rejection actionable at high verbosity:
  it reports the proven trip count, interior-block count, first block, and
  that CFG cloning is unavailable.

- Fixed native typed-list indexing when an element is passed to an `int`
  formal. A runtime descriptor read now keeps the proven raw `list<int>`
  element class instead of labeling an elem-8 payload as tagged and decoding
  it again at the call boundary. The corresponding P0 entry was removed from
  `docs/TODO.md`.

#### Preserve dynamic BigInt arithmetic at raw-integer boundaries

- Native mixed-add uses raw `i64` only with explicit raw-read provenance.
  Other tagged operands use `rt_any_add`, preserving overflow-promoted BigInts
  through a following multiply rather than truncating them into heap pointers.
- Native function tails reject implicit `int` returns of proven BigInts before
  conversion, matching HM's diagnostic. Local-store normalization respects a
  proven BigInt carrier even when HM names it `int`; wide decimal literals
  bypass `rt_any_to_i64`.

### 2026-10-31

- `core.pow` now checks runtime numeric tags directly, preserving float
  operands across native calls. Added fractional/zero-base power regression
  coverage; Vulkan sRGB conversion and camera smoothing use the float-power
  intrinsic instead of dynamic dispatch.

### 2026-11-01

- Loop-vector unrolling now clones only bodies with at most four vector
  operations. A focused DGEMM control/candidate reduced spills and reloads
  while preserving checksum `1`; runtime timing remains noisy and the
  NYR/LLVM/C gap is still open.
- Address-of indexing for function-static pointers now loads the static
  pointer value through its global storage. The C frontend regression covers
  the address of a function-static pointer's indexed element.
- Compare-fused selects keep their hidden comparison operands live through the
  select, with C frontend regression coverage.
- A file-scope aggregate-pointer array definition now replaces a compatible
  prior `extern` declaration, including initialized definitions. The
  translation-unit regression checks the resulting relocated pointer. Global
  aggregate pointers also support indexed member paths such as
  `pointer[index].array[index]` in reads and stores.
- The C frontend accepts the kernel's GNU `__builtin_has_attribute` constant
  check for unannotated `nonstring` fields, recognizes
  `__builtin_counted_by_ref` as a `void *` in `_Generic`, and skips static
  type inference for unselected generic arms. It also accepts `__noclone__`,
  `__no_profile_instrument_function__`, and field-level `__nonstring__`, and
  lowers `__builtin_isdigit` with runtime coverage. Linux `READ_ONCE`/`WRITE_ONCE`
  64-bit memory loads and 32-bit register stores lower as volatile accesses;
  an inline-assembly runtime regression covers the store. Jump-label lowering
  preserves constant field offsets on global key addresses, with an
  asm-goto regression using a nonzero offset; Linux's `uncore.o` now compiles
  through Kbuild and GNU `as`. NYC also lowers generic 64-bit `bsr`, x86
  `lock addl`/`subl` and `addq`/`subq` memory operations with sign-flag
  outputs, legacy write-only memory outputs paired with read constraints, and
  the named read-write `xaddq` form while preserving its unlocked instruction.
  The general inliner leaves calls with a following `capture.ret` intact so
  secondary ABI return registers remain adjacent to their call; an IR
  regression covers the chain. NYC also lowers volatile `movb`/`movw` memory
  reads and `movb`/`movw`/`movq` memory writes used by Linux I/O helpers, with
  runtime coverage. The saved mailbox and `pcc.c` inputs now compile and
  assemble.
  Automatic local arrays now accept alignment attributes through the backend's
  16-byte stack guarantee, with a runtime alignment regression; this advances
  saved `efi_64.c` to its unsupported alternative-instruction asm block.
  NYC also lowers fault-table `asm goto` stores for 8-, 16-, 32-, and 64-bit
  memory operands while retaining the exception target through optimization.
  Focused runtime coverage and frontend regressions pass; isolated Linux 7.3
  `cred.c`, `file.c`, `signal_32.c`, `spinlock.c`, and `percpu-rwsem.c`
  compiles plus GNU assembly pass. A frontend regression also covers pointer
  preservation in `typeof(*&member)`. Sparse designated file-scope integer
  arrays now preserve zero-filled gaps, including designated pointer-array
  relocations. Local GNU range initializers and local multidimensional array
  types in `_Static_assert` have runtime and compile-time coverage. Integer
  type specifiers can put `long` after `int`. Local arrays with an explicit
  8-byte alignment also pass a runtime address-alignment check.
  File-scope aggregate redeclarations preserve an earlier initialized
  definition. Constant conditional evaluation skips an unselected operand,
  covering the Linux `_IOC` invalid-size fallback while still rejecting an
  invalid selected operand. File-scope `_Generic` controls now apply array
  decay, allowing saved kernel initializers to compile. Runtime regressions
  cover file-scope generic array decay, nested scalar-pointer stores, and
  indexed pointer-array member addresses. The saved `hidraw.c` and `pcc.c`
  inputs compile and assemble with `-fplan9-extensions`; `hid-sony.c`,
  `hiddev.c`,
  `dm-raid1.c`, `dm-ioctl.c`, `dm-kcopyd.c`, `dm-rq.c`, `dm-sysfs.c`, and
  `dm-stats.c` also compile and assemble. The saved `wmi-bmof.c` input compiles
  and assembles with the same extension flag.
- The NYC-built SQLite 3.46.0 `testfixture` completed all 10,379 jobs in the
  upstream `testrunner.tcl all` matrix at `-O1`, with zero failed or omitted
  jobs. The JSON, cache, lookaside, and API smoke fixtures passed under
  Valgrind with no invalid accesses, invalid frees, or definite leaks.
- NYC now lowers Linux's x86 `ip_fast_csum` inline-assembly sequence. The C
  frontend regression executes header lengths 5 through 16 and compares the
  assembly result with an independent checksum implementation.
  `net/ipv4/{af_inet,raw,ipconfig,igmp}.o` and
  `drivers/net/netconsole.o` now compile with NYC and GNU `as`. `objtool`
  reports missing `panic()` `__noreturn` metadata in `inet_init` and `raw_init`,
  plus one unreachable instruction in `igmp.o`; review remains open.
  File-scope aggregate arrays now retain `aligned` attributes placed between
  the type and declarator; a runtime regression checks every element's
  alignment, and `net/ipv6/syncookies.o` compiles with NYC and GNU `as`.
  Function-type typedef redeclarations retain their parameter signatures, and
  `net/ipv6/esp6.o` compiles with NYC and GNU `as`, with two unreachable-
  instruction warnings still under review. GNU `\e` character constants are
  accepted, `-fno-builtin` disables implicit builtin lookup, and aggregate
  values can pass through variadic calls. A host `va_arg` runtime regression
  passes. Single-integer-register aggregate `va_arg` also passes a runtime
  test, and `net/9p/{client,protocol}.o` compile with NYC and GNU `as`;
  `protocol.o` reports one unreachable instruction. The per-CPU pointer type
  fix also lets `net/sunrpc/svc.o` compile; `objtool` reports one unreachable
  instruction in `svc_pool_map_init_pernode`. The complete C frontend test
  passes with these regressions.
  `net/ipv4/syncookies.o`, `net/netfilter/nf_log_syslog.o`, and
  `net/sunrpc/{stats,svcsock}.o` also compile with NYC and GNU `as`;
  `svcsock.o` reports three unreachable instructions.
  Unsized local aggregate-pointer arrays now infer their extent from the
  initializer; the frontend regression passes and
  `drivers/dma-buf/sync_file.o` plus `drivers/gpu/drm/drm_syncobj.o` compile.
  Aggregate `_Bool` fields retain their boolean type for pointer-to-bool
  assignments; the runtime regression and `net/mac80211/parse.o` compile pass.
  Typed scalar pointer-to-pointer fields retain their pointee type through
  member access and `typeof`; a nested per-CPU pointer increment regression
  passes at runtime.

</details>

## [0.9] - 2026-08-21

- C header imports, subsystem reorganization, and native/runtime reliability.

<details>
<summary>Details</summary>

- Development period: 2026-07-21–2026-08-21.

### Added

- In-tree C header frontend replaces libclang for supported arrays, unions,
  packed structs, bitfields, extended numbers, function pointers, typedefs,
  macros, and variadics. Untrusted headers have parse deadlines, recursion
  limits, and bounded tables.
- Optional GMP with native BigInt fallbacks; `-Oz`, compile-time `#assert`,
  formatted `core.report` diagnostics, managed-heap `--zero-init`, and
  `core.os.rev` decompilation/symbolic-execution modules.
- Linux/x86-64 syscall restrictions on process creation and executable-memory
  remapping through `mprotect`/`clone`/`fork`/`vfork`.

### Changed

- Explicit subsystem boundaries under `src/base`, `src/cmd/ny/repl`, and
  `src/code/*`. Optional LLVM emission/JIT joins the native backend; direct
  AST-to-LLVM remains behind a semantic-parity gate.
- Native/self-build links default to the system linker; fast alternatives
  require explicit selection.
- Expanded diagnostics and measurements: register allocation, allocation
  sampling, native/C and native/LLVM budgets, emitted bytes, peak RSS,
  vectorization profitability, static-island quality, and inlining decisions
  including monomorphization rejection.
- Tagged-int bitwise operations, proven-safe shifts, and ordered comparisons
  use fast paths under `--profile=speed`.

### Fixed

- SSA: mem2reg parameter promotion, escape-SROA slots, and native allocation
  and bounds facts for `f64buf`/`i64buf`.
- Sockets: decode ephemeral ports from sockaddr bytes; return the raw
  descriptor from `socket_accept`.
- Unified loop analysis; repaired vector-tail preheaders, PHIs, and scalar
  induction remapping, syscall/`rdrand` assembly guards, AArch64 conditions,
  and BigInt heap corruption.

</details>

## [0.8] - 2026-07-13

- Proof witnesses, filesystem watching, cache integrity, and platform fixes.

<details>
<summary>Details</summary>

### Added

- Standalone `wasm-emscripten` output with `_ny_top_entry` and explicit
  `env.*` namespacing through aliases; existing declarations retain precedence.
- `prove(condition[, message]) -> proof` witnesses reject false/dynamic
  obligations and ordinary values in proof parameters. `core.math.logic`
  adds evaluation, simplification, certificates, bounded solvers, rewriting,
  and Prolog-style unification/backtracking.
- `core.os.fs.watch_*` and hot reload use inotify, kqueue, or Windows change
  notifications, with an mtime fallback.

### Changed

- NYIR coalesces copy/local chains, allocates scalar registers, selects
  immediates, indexes DCE labels once, and preserves floating types across
  collapsed equivalence classes.
- Precomputed x86-64 call boundaries and immediates reduce the measured call
  body and frame-relative accesses.
- Library sweeps stop after optimized IR rather than materializing MCJIT.
  Cache formats reject mixed library/user entries and sanitizer-tainted objects.
- `ny-fmt --cloc` reports tracked additions/deletions and per-file totals.
- Hot reload blocks on native events rather than busy polling.
- The opt-in JIT shared-object tier checks its bitcode-provenance sidecar;
  stale/interrupted entries return to normal compilation.

### Fixed

- REPL v2 snapshots bind source to a fingerprint and byte length; `:load`
  rejects corrupt payloads before rebuilding state. Older source-only snapshots
  remain accepted.
- One C lexical scanner serves formatter function ranges, rankings, and
  duplicate detection, excluding strings, comments, multiline macros,
  conditions, and call continuations.
- `ny-fmt --bugs` diagnoses discarded value-typed `list.append`/`extend`
  results (`NYAUD1120`) and `set()` on frozen `{}` dictionaries (`NYAUD1119`).
- Internal x86-64 JIT/object bundles support 1,024 relocations, up from 256.
  Live requests support 128 lowered user functions, up from 64, and reject
  larger requests before lowering; portable NYIP retains its separate
  4,096-function VM boundary.
- macOS libc imports materialize named aggregate parameter/return layouts
  without registering anonymous carriers as scalars. System headers recover
  supported declarations; project headers remain strict.
- Apple-arm64 compile-time MCJIT finalizes indirect callees through managed
  invocation. Native-only link discovery matches JIT and deduplicates annotations.
- Preserved x86-64 floating constants and typed f32/f64 locals, removing
  nondeterministic ELF results.
- Hardened sanitizer AOT output, cache isolation, cleanup, and UBSan handling.
- Repaired Windows JIT, target-width libc fixtures, variadic C imports,
  trace/debug progress suppression, ELF32 returns, watcher lifetime, parser
  recovery, and dictionary helper ambiguity.

</details>

## [0.7] - 2026-06-30

- LLVM-free native backends, object emission, and C interoperability.

<details>
<summary>Details</summary>

### Added

- Native emitters for x86-64 (default/primary), i386, ARM, AArch64, and
  RISC-V, with debug-scoped WASM, BPF, PowerPC, MIPS, and AVR support.
- In-process ELF64, ELF32, COFF, and Mach-O object writers with relocations
  and multi-function aggregation.
- Narrow internal ELF executable linker with runtime stubs for
  `malloc`/`free`/`realloc`/`calloc`, `memset`/`memcpy`/`memmove`/
  `memcmp`/`memchr`, and `strlen`/`strcmp`/`strchr`.
- `*p` dereference expressions parse as `NY_E_DEREF`, type-check as the
  pointed-to type, lower to `NYIR_LOAD_I64`, and support `*p += value`.
- `addr_of(local)` lowers to `NYIR_ADDR_LOCAL`, executes in the debug VM, and
  emits frame-relative `lea` on x86-64/i386; address-taken local facts are
  invalidated after raw pointer writes so later reads observe mutations.
- The wasm-emscripten web target emits standalone Wasm with `_ny_top_entry`;
  explicit aliases opt into namespacing while existing Nytrix declarations
  keep precedence.
- C frontend hardening: internal aggregate imports decline unsupported nested
  or by-value layouts without poisoning the fallback; strict no-libclang
  aggregate-return imports are covered through `load_layout`; non-positive
  array extents are rejected; unsupported C declarations produce recoverable
  diagnostics instead of hard aborts.
- Parser diagnostics no longer suggest C-style `for (;;)` and point to Nytrix
  iterator syntax instead.

</details>

## [0.6] - 2026-06-30

- Fuzzing, cryptography/math modules, rendering, and platform tooling.

<details>
<summary>Details</summary>

### Added

- Published fuzzer and tooling for local benchmarking and error-shape
  discovery.
- Radix helpers, stream/block ciphers, public-key helpers, and
  lattice/factorization modules.

### Changed

- SVG/UI rendering: 4x4 supersampling, stroke linecap/linejoin,
  gradient/`<use>` support, and terminal 256-color output.
- `--borrow-check` decoupled from `--ownership-strict`; Z3 enabled by default;
  proven-nonzero `f64` division checks elided.
- glTF hot paths moved from `src/code/lang/runtime/gltf.c` into Ny code.
- CMake dependency probing hardened for LLVM, libclang, Z3, Windows
  UCRT/MSYS2.

### Fixed

- Canvas UTF-8 buffer types, terminal rendering, lowercase type-first local
  bindings, and semicolon-comment diagnostics.
- Windows builds, joystick axes, SDK/toolchain probing, and zlib decompression
  capacity handling.

</details>

## [0.5] - 2026-06-05

- Editor/viewer framework, renderer layers, and initial WebAssembly support.

<details>
<summary>Details</summary>

### Added

- Editor and engine viewer (`core.os.ui.render.viewer`): asset browser,
  hierarchy, inspector, gizmos, transform tools, and runtime bootstrap.
- OpenGL, WebGL, and Vulkan renderer paths for the viewer.
- WebAssembly compiler backend foundation.
- RSS feed, Discord, and Mastodon integration.

### Changed

- Renderer/viewer split into distinct `render` and `viewer` layers.
- Function syntax moved from `fn foo(type: arg): ret` to
  `fn foo(type arg) ret`.
- Module self-checks moved into `#main` blocks.

### Fixed

- Vulkan UI mesh caching and startup text-fitting crashes; animated glTF
  index-buffer retention and texture reuse; GLSL syntax and redraw stability.

</details>

## [0.4] - 2026-05-30

- Ownership checks, typed compilation, and a unified CLI.

<details>
<summary>Details</summary>

### Added

- Cross-platform windowing/input: Win32, Cocoa, X11, Wayland, Vulkan.
- Typed compiler pipeline: Hindley-Milner inference, lambda/nested-collection
  inference, and monomorphic specialization.
- `&expr` shorthand for `borrow(expr)`, ownership contracts, `--safe-mode`.
- `handle`, `fnptr`, `seq` types; layout records/guards with compile-time
  reflection.
- Unified CLI: `ny fmt`, `ny test`, `ny doc`, `ny perf`, `ny make`, `ny pkg`,
  `ny new`.

### Changed

- Compiler/runtime/Vulkan internals standardized on raw integer
  representations.
- `-O2` became the default native optimization level.
- Bootstrap and dependency discovery reworked for cross-platform setup.

### Fixed

- Emit-only hangs in recursive raw-integer fast paths and macOS arm64
  compile-time evaluation of immutable collections.
- FFI header collisions, ownership diagnostics for returned values, and mutable
  closure captures across repeated calls.

</details>

## [0.3] - 2026-04-13

- Graphics, media assets, networking/audio, and platform expansion.

<details>
<summary>Details</summary>

### Added

- glTF loading, Meshopt integration, mesh/glTF parsers, and an image parser
  stack.
- Vulkan rendering, scene graph, sky/SDF shaders, and split Vulkan/GUI
  renderer paths.
- Terminal renderer integrated into `core.os.ui`; Win32 window backend added.
- IO and networking modules; JACK audio backend.
- Public fonts, dictionaries, website assets, and renderer shaders.
- Maintained sample programs, REPL import scenarios, and an updated learning
  guide.

### Changed

- Ordinary compilation now defaults to `-O2` with size-oriented native code
  generation, including assembly emission and normal JIT/AOT routes. Explicit
  `-O0` remains unoptimized, speed/peak profiles disable size tuning, and the
  REPL retains its unoptimized startup behavior.
- `--release` now selects size-oriented native code generation while retaining
  optimized LLVM passes, runtime frame-pointer and unwind-table omission,
  section garbage collection, no build ID, and stripping. Size-optimized ELF
  links also remove residual unwind and compiler-ident sections. `-flto` now
  applies host LTO to runtime compilation and final linking on non-Windows
  hosts.
- Linux x86-64 `--mode=bare` AOT accepts multiple independent top-level
  `print`, `eprint`, and literal no-op statements from inline or file input as
  a freestanding syscall executable. General functions and control flow remain
  unsupported and are rejected rather than silently linking the regular
  runtime. The tiny syscall writer clears `%eax` before each syscall-number
  load through `%al`; the multi-statement write/exit code shrank by four
  bytes for three writes, with a regression asserting the resulting 218-byte
  ELF and its output.
- Linux x86-64 bare machine lowering now implements C-string, signed-integer,
  and newline output with direct `write` syscalls, and lowers typed-buffer
  allocation, length, bounds-checked signed index reads, and typed pop without
  the host assembler. The typed-list fixture exercises positive and negative
  indexes, length mutation, and absence of dynamic ELF sections.
- Successful native/LLVM artifact messages now report the emitted ELF/EXE,
  object, bitcode, or assembly size in KB/MB when applicable, with the exact
  byte count included in every report.
- The x86-64 assembly emitter now uses the sign-extended 32-bit immediate
  encoding for signed-32-bit scalar constants, and uses 32-bit zero-extending
  moves for nonnegative `uint32` constants instead of needlessly emitting a
  64-bit `movabs`.
- The x86-64 machine assembly emitter gives exact two-argument `i64` binary
  leaves (add, subtract, multiply, and/or/xor) frameless bodies using the ABI
  argument registers. One-argument identity and constant-return leaves also
  omit the frame. Straight-line `i64` leaves, including one-argument
  immediate arithmetic and two-or-more-argument chains, keep intermediate
  results in `%rax` and use ABI argument registers without frame homes. General
  NYIR leaf emission uses `%rsp`-relative homes without a `%rbp` prologue or
  epilogue when stack homes are needed; an `immediate_remat` regression checks
  that path. Generic machine-leaf frame elimination remains open.
  One-argument increment/decrement leaves
  use `inc`/`dec` instead of immediate arithmetic and have linked disassembly
  checks for both compact opcodes. Frameless constant-return leaves with
  nonzero values through 255 clear `%eax` and load `%al`, with a disassembly
  regression for the 42 return value.
- Keep canonical stack homes current for dedicated loop GPRs, including
  non-resident CFG-exit consumers.
- Loop GPR homes are enabled by default; `NYTRIX_DISABLE_LOOP_GPR=1` disables
  them for backend diagnosis. Calls within loops remain conservative.
- Platform APIs moved into `core.os`; window backends moved into
  `core.os.ui.window`.
- Legacy native window backend path removed in favor of the new backend split.
- Cache management, bigint support, and shader generation improved for
  graphics workloads.

### Fixed

- Asset paths and shader generation during scene-coverage expansion; runtime
  fixture mismatches from moving platform code into `core.os`.

</details>

## [0.2] - 2026-03-09

- Compiler/runtime bootstrap, semantic analysis, REPL, library, and manuals.

<details>
<summary>Details</summary>

### Added

- Parser, lowering pipeline, AST node definitions, and visitor/function
  lowering.
- Semantic analysis, diagnostics, and statement/call/FFI lowering.
- JIT lowering state, module/JIT integration, and native value-runtime bridge.
- Interactive reader, REPL completion, and build/web launcher with LSP
  commands.
- Enums, packed layouts, `sizeof`, pointer dereference, `try`, reflection
  operators, effects, and the `#main {... }` entry guard.
- Core text, numeric, and cache modules; core IO and string helpers.
- Early UI facade, Vulkan renderer core, and native window/input backends.
- Network/audio backends; block cipher, factorization, RSA lattice, ECC/DLP,
  hash/PRNG, and public-key crypto helpers.
- Specification manuals, release notes, and initial benchmark/regression
  baselines.

### Changed

- Standard library moved to `lib/`, reducing prelude coupling.
- Parser, Vulkan renderer core, and UI renderer split into focused modules.
- Std module layout reorganized; numeric modules moved into `core.math`.
- Python build/bundle tooling replaced with native tools.
- Benchmark parity: fannkuch's C reference now uses 64-bit arrays like Ny;
  thread-ring reports launch time separately from per-hop time. Quiet 7+2
  runs passed, with thread-ring's measured NYR/C ratio falling from 5.06x to
  1.59x after launch cost was excluded.

### Fixed

- Standard-library import coupling and module-path drift.

</details>

## [0.1] - 2025-12-24

- Initial buildable launcher/runtime tree and smoke fixtures.

<details>
<summary>Details</summary>

### Added

- Launcher skeleton, build script, and CMake scaffold (`make`,
  `CMakeLists.txt`, `src/cmd/ny/main.c`).
- Runtime placeholders and smoke fixtures for a first compilable, testable
  tree.

</details>
