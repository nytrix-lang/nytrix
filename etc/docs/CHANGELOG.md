<!-- nytrix-doc: {"audience":"user","featured":true,"group":"project","order":530,"summary":"Release highlights and notable changes for each version."} -->
# Changelog

Each release starts with a short summary, followed by details when useful.
Entries describe notable changes in that release; measurements and local
artifacts, where mentioned, are not a fresh test of the current checkout.

## [0.10] - 2026-10-13

- **Native backend**: improved x86-64 code generation, register allocation,
  ABI handling, and runtime lowering across compiled programs and benchmarks.
- **Language and compiler**: expanded semantic checks, diagnostics, effects,
  templates, reflection, and compile-time capabilities with targeted correctness
  fixes.
- **Runtime and standard library**: reliability fixes across strings, lists,
  buffers, I/O, crypto, networking, audio, terminal UI, and platform bridges.
- **Cross-target support**: continued work on LLVM, NYR, NYC, and
  architecture-specific backends, with fail-closed behavior where a target does
  not support an operation.
- **Tooling and docs**: benchmark/reporting improvements, clearer contributor
  guides, and consolidated documentation.

<details>
<summary>Details</summary>

### NYC C compiler

- NYIR verifier accepts an integer select as another select's condition when
  both arms are boolean; four Linux translation units now compile through the
  verifier and emit native dependency files.
- SysV x86-64 `va_arg` reads supported two-eightbyte and memory-class
  aggregates; an aggregate that does not fit in the remaining registers comes
  entirely from the stack and leaves both register cursors unchanged.
- `_Bool *` arguments, assignments, and returns retain the pointer instead of
  converting to a boolean; scalar returns from `_Bool` functions and explicit
  casts to `_Bool` normalize nonzero values to one instead of truncating to a
  byte.
- Multidimensional array and aggregate pointer indexing uses the pointee
  stride when taking a subobject's address; file-scope integer arrays retain
  dimensions from array typedefs, and subtraction counts complete rows rather
  than bytes.
- Completed file-scope pointer arrays retain their bound and reserve
  zero-initialized storage after an earlier incomplete declaration.
- GNU section and alignment attributes before `__typeof__` and trailing or
  inherited alignment on file-scope records are preserved; automatic record and
  record-array compound literals zero omitted fields and elements on each
  evaluation; block-local aggregates declared with `typeof` receive record
  storage, including inside GNU statement expressions.
- SysV x86-64 returns that mix INTEGER and SSE eightbytes work in both class
  orders; integer-only `-mno-sse` variadic functions preserve the `va_list`
  calling convention without touching SSE registers; scalar inlining and value
  numbering preserve calls that feed aggregate return registers and keep results
  from separate calls distinct.
- x86-64 GNU assembly handles narrow memory accesses, unsigned division and
  shifts, and GOT-relative `-fPIC` data references; fused bit tests keep their
  input live until it is read; ELF relocations use the correct symbol index
  after sorting definitions, including in vDSO objects.
- Peephole range inference at function-entry loops no longer lets backedge
  facts erase the exit guard on the initial iteration.
- Aggregate pointer loads and stores through GNU FS/GS-qualified addresses keep
  the segment override; Linux accesses both the per-CPU IRQ stack pointer and
  saved register pointer in the correct segment.
- Scalar increment and decrement stores and x86-64 single-precision memory
  moves keep FS/GS, with the read and write using the same segment.
- Named FS/GS scalar objects retain their segment on reads, writes, and
  address-of dereferences.
- Named GNU inline-assembly memory outputs keep FS/GS, including bitwise
  updates, decrement-and-test, and compare-exchange.
- Const ELF objects use read-only storage, with relocatable read-only sections
  for pointer-containing data in PIE and shared links.
- `lgdt` and `lidt` accept packed descriptor operands, and explicit
  RIP-relative inline assembly operands are preserved under PIC; inlined x86
  memory-capable asm outputs keep their destination and cannot return a stale C
  value, including when the output overwrites a formal parameter; required
  static-branch inlining runs before optional stackless inlining; scoped
  cleanups run on C and asm-goto exits without running a guard's destructor on
  a jump to a macro-generated label inside that same guard.
- i386 real-mode code size drops with fewer frame loads, stores, and copies,
  including for four-byte stack calls and wide integer arithmetic; low-stack
  real-mode code uses shorter fixed-frame prologues and folds one-use symbol
  addresses into loads and stores with 32-bit relocations, bringing the Linux
  setup replay within its sector limit, while the separate setup memory limit
  still prevents a `bzImage`.

See the [NYC compiler guide](spec/nyc.md#verification-evidence) for the
build and runtime evidence and its limits.

### Compiler and backends

- Safe mode: guard-page user heap + RC/RAII; LLVM ASan/TSan passes: required attrs; sanitizer compile fail-closed on unsupported native encoders/failed instrumentation; user-memory fuzz: deterministic alloc/byte-access mutations under native/LLVM safe heap + emitted ASan; LLVM literal-array symbols: valid ASan COMDAT sigs; numeric native string conversions: managed storage, no leaked uncached formatting buffers.
- Strict ownership: raw alloc identities via copied/interior local aliases + explicit releasing callees; contracts on every annotated parameter; owned factory results keep caller-side provenance; consuming raw owner invalidates copied/interior aliases; uncaught panics run active defers; managed dict alloc checks failure before dereferencing new storage.
- Indexed resource extraction: conservative invalidation of collection owner, incl. resource-bearing layout fields; ownership-rule errors fail shared type validation, no print-and-return-success; layout fields: common generic/alias grammar; nominal dict values keep layout on field read.
- Native dict/set ctors: >134,217,728 capacity rejected pre-alloc; growth: overflow reject + alloc-failure report, no silent insertion loss; stdlib bootstrap dicts skip user alloc-injection budget; tests: scoped-pointer fallthrough/return/loop exits/panic, emitted-code ASan heap/stack/global bounds.
- Safe-heap byte accesses: requested size, not rounded alloc capacity, incl. padding/underflow/interior pointers; top-level lexical defers collect environments before helper emission; resource cleanup + deferred local mutation work outside functions.
- Alias summaries: scalar param values vs address-taken param storage; checked dynamic indices in safe-profile stack byte reads w/o weakening borrow-escape rejection; explicitly bounded FFI + stack byte accesses: boundary/panic-cleanup tests on native/LLVM MCJIT/ORC; byte load/store APIs: explicit dynamic byte extent, stack/FFI bounds before access, defers unwound on failure; omitted extents keep raw compatibility contract.
- Substitution-table growth: moved allocs preserved if later realloc fails, enabling retry + safe destruction; failure injection on each growth alloc + capacity overflow.
- `--sanitize=memory`: emitted code + runtime MSan-instrumented; acceptance tests: initialized/uninitialized byte reads; TSan checks generated parent/worker writes, distinguishes join-ordered read; both lanes link PIE for shadow-memory compatibility.
- `--sanitize=ub`/`undefined`: emitted signed 64-bit arithmetic + null accesses, null checks precede LLVM optimization; saved executable tests: defined ops + eight failure modes at `-O0`/`-O2`; fuzz cmd runs all four emitted sanitizer lanes, valid-op controls + named failure reports.
- Bump allocators: byte offsets w/o tagged-integer corruption; overflowing/exhausted requests rejected w/o cursor change; pointer arithmetic keeps result across dynamic bounds; native precondition/refinement substitutions use builder-owned arena.
- NYC stack canaries: real SysV x86-64 for stack-protector options, option order/predefines/GNU function opt-outs preserved; both output routes detect corrupt cookies via FS/GS TLS, linker-resolved offsets, global guards; no spillable guard pointers.
- Single MOVS: both tied pointer outputs at every scalar width; locked/unlocked CMPXCHG16B: expected-value fields w/ or w/o condition output; values live across fixed-register asm not kept in clobbered caller-saved regs.
- PadLock SHA256 + repeated port-I/O: explicit operand lowering, not speculative passthrough; SHA256 hardware execution unverified; strict kernel SHA256 + iomap objects compile; both native routes run byte/word/dword REP INS/OUTS in ring-zero QEMU guest, checking input data, bounds, pointer/count outputs, zero counts, byte output via debug console.
- NYC: prefix `section` attrs on aggregate/int-array/pointer-array/callback globals; ELF keeps exact section names w/o leading dot, matching GNU asm + linker-generated bounds; C `section` attrs concat adjacent literals, preserving kernel registration sections like `__governor_thermal_table`; function section attrs survive param-type parsing, preserving `__init` on funcs with params.
- NYC emits inline helpers for static callback tables; `-O1` post-inline slot promotion drops disabled short-circuit branches before retaining unavailable linker symbols; section-attributed static helpers eligible for heuristic inlining, out-of-line defs retained; empty feature-disabled helpers no longer keep undefined arg symbols alive, arg side effects preserved; unreferenced `static inline` helpers pruned even when section-attributed, avoiding dead refs to feature-disabled impls.
- GNU `typeof` aggregate aliases keep target, incl. Linux startup PIC aliases; asm + direct ELF bind data aliases to target's original storage/section; GNU aggregate-array aliases keep local binding, incl. static Linux device-table aliases; file-scope asm local labels C-addressable w/o `.globl`, trampoline data + local binding preserved.
- Dominating equality/inequality branches drop contradictory tests w/o facts through joins or changed memory; private integer stack fields keep values across unrelated calls, incl. byte/word/signed-dword loads; NYIR const folding carries facts across unconditional branch only to immediately following label w/ unique predecessor, unknown at joins; NYIR jump threading leaves branch-only blocks intact when PHI results escape, preserving SSA dominance; memory SSA: real join PHI for differing constants, not one predecessor's non-dominating def.
- Nonvolatile C lvalue cast to `void`: read dropped, address-expression side effects kept; volatile reads observable; discarded void exprs valid comma operands, incl. DRM connector macros; `noreturn` calls terminate lowering w/o synthetic continuation code; Linux-compatible conditionals with void false arm keep both arms' side effects, no value store for void arm.
- Late inline cleanup refreshes instruction effects before verification; GNU C label-address funcs not inlined across function-local label symbols; DCE roots those symbols for target blocks; unreachable C code resumes at enclosing control-flow stmts when labels nested in loops/switches, `break`/`continue` targets preserved, externally reachable labels still compiled.
- Direct ELF: byte/word/dword/qword memory exchanges, incl. function-pointer operands + signed narrow return values; unsupported asm no longer substitutes GS offset zero or exchanges with uninitialized operands; both output routes reject those templates; NYC preserves signed 32-bit absolute relocations in C asm + ELF, fixing x86-64 kernel trampoline relocations.
- C const objects: read-only section flags; `.note.*`: ELF note type in both output routes; Linux build notes no longer trigger conflicting section-type/attribute assembler warnings; file-scope char arrays keep ELF section/alias attrs, incl. Linux exported-table aliases, no stale section attr from preceding static addressable anchor, ELF section test covers symbol placement; file-scope aggregates keep declarator + type `section` attrs for kernel init/setup tables; zero-bound aggregates stay complete zero-size objects after incomplete `extern` array.
- File-scope static/weak bindings: one name index, not whole-TU rescans per exported object; repeated ordinary + typedef callback declarations denote one object; weak ELF data + defining-unit reads/writes honor strong interposition in both native routes, fixing duplicate kernel reboot callbacks; C weak attrs survive repeated declarations/definitions, incl. suffix-form `__weak` in kernel version object; NYC keeps local static pointer tables referenced by data relocations; vector PHI classifier traverses shared copy chains iteratively.
- C storage bindings follow declarators, not refs in weak funcs/prototype params/typedef spellings/initializers; pointer + suffix attrs don't leak; mixed scalar/pointer initializer lists keep each declarator's type, relocations, initialized storage; function-pointer objects via function-type typedefs keep tentative defs + callback signatures, incl. prior `extern`; declaration-binding lookahead resets alias state between declarators, preserving distinct GNU weak aliases in VDSO clock objects; 32/64-bit strict VDSO compile + alias runtime tests pass.
- Opaque UD2 asm no longer implies C `noreturn` (signal/kernel handlers may resume); tests resume plain traps in both output routes + labelled, section-annotated traps in asm output; direct encoder still rejects annotated form; native x86-64 text emission omits next-label branches, removing dead zero-offset jump after Linux fatal BUG trap w/o guessing trap semantics or suppressing `objtool`; file-scope asm function symbols stay functions in C exprs, incl. address-taking + function-pointer init.
- NYC frame-pointer options obey command-line order: final `-fno-omit-frame-pointer` cancels earlier omission, restores single-function route; test runs eight-argument calls w/ ± values; called static-inline jump-label helpers kept when out-of-line emission needed; whole-unit asm honors `-fcf-protection=branch` with ENDBR; unsupported control-flow modes + direct-object branch protection fail explicitly.
- Private nonescaping C funcs propagate scalar param constants only when all direct calls agree, w/o signature changes, call removal, noinline bypass, or address-taken formals assumed unchanged; strict kernel `swap_state.o` no longer retains disabled THP helper; constant helper results propagate even when call must remain for effects/possible nontermination; late cleanup needs no lower call count; truncate/device-mapper objects no longer ref disabled DAX mapping helpers or `dm_zone_endio`; callback-table reachability + THP specialization remain open link blockers.
- Unreferenced private file-scope data no longer roots dead callback defs; explicit used defs, named sections, public objects, aliases, asm refs, function-owned statics keep lifetimes; both output routes pass callback retention/runtime tests; strict device-mapper object no longer refs `dax_get_private`.
- Physical x86-64 asm + object prologues materialize SysV aggregate params as addresses of actual by-value storage, incl. mixed integer/SSE classes, register-bank rollback, stack args; host-compiler interop tests run calls through both output routes; C frontend suite passes.
- Branch-protected C asm: ENDBR at address-taken labels + function entries; forward/backward label addresses: runtime landing-byte checks at O2/O3; all 256 BPF interpreter data-relocation targets pass strict-object byte check; branch compare fusion honors adjacency proof, not operands reloaded from reused stack homes.
- CPUID asm: omitted input/clobber sections, read-write reg operands, reordered numeric ties; x86-64 asm/direct-object + freestanding i386 tests compare real results w/ host compiler; encoders preserve EBX/RBX; preservation checks keep real RBX live across calls + live compiler values across instruction; asm restores RBX before loading output addresses; direct encoder avoids stack-home reads while temporary pushes change RSP.
- Raw RDMSR/WRMSR asm: explicit low/high-word lowering for 32-bit integer outputs, incl. struct members + reordered inputs; ring-zero QEMU verifies read/write/readback via x86-64 asm, direct objects, i386 asm; i386 probe reads/restores/rereads CR0; control-register writes accept omitted trailing asm sections; physical hardware execution unverified.
- Bit-string set/reset/complement: locked/unlocked semantics + signed indexing across memory words; carry-output bit tests accept omitted clobber sections w/o extra C memory read; both native routes + i386 run boundary tests; direct-object disassembly distinguishes actual LOCK prefix; x87 init/status asm marks output locals address-taken, not reused constants; i386 + direct-object emitters do real 16-bit status/control writes, neighboring high bytes preserved; fresh strict real-mode/compressed CPU probes + boot printf compile.
- Repeated byte comparisons: tied source/destination cursors, remaining native-width counts, final flag, incl. null zero-count inputs + backwards traversal; asm/direct-object + i386 runtime tests cover these; i386 local loads no longer write high word into four-byte home, corrupting live neighbor; unsigned 64-by-32 division: tied accumulator inputs, member/indirect quotient/remainder outputs, no extra output reads; two-stage REP word/byte copies keep cursors, consume actual tail count, incl. backwards traversal; native asm/direct-object + all i386 regparm tests pass w/ frozen compiler; boot string + compressed string Kbuild commands compile.
- Fixed R9 asm inputs evaluate real C operand + write actual register; both native routes pass protected-page fault/resume probe, return loaded 64-bit value via R9, observe later memory changes; strict decompressor identity-map object compiles; explicit FS/GS asm: real byte/word/dword accesses w/o ordinary C read at unsegmented address; probes use distinct real segment bases via both x86-64 routes + all i386 regparm settings; plain selector writes no longer get invented exception fixups; i386 selector/memory reads respect narrow value-home allocation; A20 boot object compiles; general C address-space qualifiers remain a separate incomplete requirement.
- i386 variadic defs: pointer-shaped lists, copies, mixed-width extraction, aggregate args, hidden-result returns; GCC interop at all four regparm settings, nonzero-regparm result-pointer popping differs from Clang default; x87 conversion keeps full-width integers, restores rounding control word after truncation toward zero; C frontend suite + IR effects roundtrip pass.
- NYIR use-def/value replacement keep asm-goto targets in label namespace, incl. labels overlapping SSA value IDs or exceeding value count; static-feature cleanup no longer fails on those valid inline-asm branches; descriptor-table `sldt %0` keeps no-result contract; focused test guards against spurious NYIR destination.
- x86-64 C asm lowers CRC32B/W/L/Q w/ 32-bit accumulators + 8/16/32/64-bit inputs; logical exprs skip dead right operands when constant left short-circuits, keeping Linux `__builtin_constant_p` guards around jump-label asm; `__builtin_strcmp` folds adjacent literals; constant-`if` straight-line arms checked in scratch IR before omitting unselected arm, supporting Linux scheduler guards at `-O0`; O0 safe const folding + const-branch cleanup let always-inline bitfield helpers drop compile-time error calls; constant-if pruning keeps `used` static locals from dead arms, incl. static-call anchors in `.discard.addressable`.
- NYC `-O0`: SSA promotion + unsigned right-shift folding eliminate Linux `GENMASK` `__bad_mask` checks even across joins; late CFG simplification skipped at `-O0` post-SSA because it produced verifier-invalid copies on kernel control flow; focused kernel rebuilds validate; x86-64 NYIR encoder accepts up to 65,536 value slots, covering 46,507-value Intel VDSC function; O1 whole-unit compile considers discardable static helpers ≤8 NYIR instructions for small inliner, letting tiny `static inline` predicates fold disabled kernel paths w/o general O2 inliner; O1 inliner resolves C-frontend `extern`-flagged calls against same-TU defs, foreign calls stay out of line.
- C `always_inline` helpers cloned at every optimization level before codegen, preserving constant specialization + eliminating out-of-line calls required to disappear; NYC keeps `.discard.addressable` relocations owned by live functions after optimization/inlining, frontend pruning removes anchors from unreachable statics, static-local addresses emitted on use, no runtime refs into linker-discarded sections.
- NYR jump threading accepts zero-value CFGs w/o incorrectly failing const collection; NYIR typed-value maps allocate only observed classes, one zeroed region for absent classes instead of 14 full-size arrays; native reachability trusts frontend builtin-operator facts, avoiding module/impl scans for ordinary arithmetic; selected overloads + unresolved expressions keep target discovery; operator tests include overloaded call-result operands.
- Incremental dependency inventories: cold/forced builds schedule inputs, removed providers invalidate surviving consumers, bare module names match `.ny`, ambiguous basenames keep all candidate deps; graph reload no longer reads freed pathname; cache roots follow config; alloc/publication failures reported; HM unification snapshots type-detail/deep tracing once per pass, no per-constraint debug-policy calls, verbose constraint/substitution output preserved.
- Native compile-time cleanup: verifier defs indexed once per verification, PHI/select/local-store analyses skipped when absent, immediate module imports indexed w/o re-export precedence change; isolated three-pair Pong object-compile comparison: median 40.840 s → 39.788 s (2.6%), byte-identical objects; x86-64 lowering improved reg allocation, call boundaries, signed arithmetic, address/base forwarding, immediate multiply encoding, several miscompile/crash cases from focused tests.
- Refinement lookup indexes main + generated trees, traversal precedence preserved, no repeated full-AST scans for distinct names; exported-member checks scan declarations directly, no temporary dedup export-name lists; module alias resolution writes bounded path copies + joins, no formatted I/O in recursive lookup; module-member caches compare stored key hash before member strings, no collision string scans, exact module/member verification kept; four-way cache 16,384 → 65,536 entries after cold Pong profiling found repeated imported-member scans; cache-disabled trace: 7.4% fewer scans, no total compile-time speedup established.
- x86-64 home DSE skips reference-buffer alloc + byte scanning when no scalar homes or no home stores from probe; probe encoder reused, emitted bytes unchanged; native list append gives f32/f64 precedence over stale raw-integer ABI facts, blocking float values at raw-i64 append entry; terminal/editor native fixtures + integer-append tests pass.
- HM env lookup caches +/− results, checks appended bindings before reuse to preserve shadowing + syntax-context behavior across cloned envs; HM unification skips constraint string formatting/allocation when type-detail tracing off, but `-vvvv --stop-after=hm` still emits constraint/substitution details; HM scheme inference reuses temp env buffer across sequential schemes; env clones reserve final capacity once + bulk-copy entries.
- Loop-phi reg coalescing recolors preheader source only when it dies at copy + candidate-color constraints pass; call-crossing/non-dying values keep reg/home behavior; NYR/LLVM paths got focused correctness/perf work on typed-buffer/store behavior, inlining/splice budgets, block layout guards, range/memory facts, runtime fast paths where safe; NYC/C frontend expanded accepted C constructs, fixed parser/preprocessor/declarator/initializer/typeof/array/asm cases, target-dependent optimization + instruction selection still limited; cross-target backends (AVR, ARM/AArch64, i386, RISC-V64, MIPS64, PowerPC64, Wasm/BPF-related paths, object/ABI handling) got focused ABI/lowering/fail-closed fixes with targeted tests.

### Language, diagnostics, and analysis

- Added and refined compile-time checks, effect handling, proof/refinement
  behavior, template/macro diagnostics, and type/ownership-related validation.
- Diagnostics for unsupported operations, invalid compile-time claims, and
  backend/target limits now fail explicitly rather than silently miscompiling.
- Fixed semantic/lowering issues in partial application and currying paths,
  callback/effect inference, dynamic indexing, and ABI-sensitive call lowering.
- Fixed NYR lowering for integer conversion of dynamic-list elements, tagged
  floating-point builtin returns, raw-integer `append` results, and list
  copy-on-write aliases mutated in loops; affected regression fixtures pass
  their configured execution variants.
- DCE unreachable-instruction removal now erases in reverse index order,
  preserving original CFG-to-instruction mappings while compacting SSA.
- Vectorized loop reductions initialize unused NYIR operands to the sentinel
  value so SSA compaction does not treat zero as live.

### Runtime and standard library

- Fixed correctness issues in string/list/buffer operations, hashing/crypto
  helpers, subprocess/process handling, terminal and UI behavior, audio
  resource loading, HTTP handling, and platform-specific bridges.
- Legacy crypt hash scratch-buffer size arithmetic and allocation are checked;
  streamed dictionary scans close their opened file before reporting
  scratch-buffer allocation failure.
- BMP decoding checks pixel-buffer size arithmetic and allocation before
  initializing output.
- GIF decode and encode paths check temporary pixel, frame, and image buffers
  before writing through them.
- Linux filesystem watcher polling fails clearly if its event buffer cannot be
  allocated.
- Sound-file decoding checks metadata/sample allocation sizes and closes native
  handles on failure; mixer buffers preserve prior allocation when `realloc`
  fails.
- Loader vectors, zlib output growth, and LZ4 frame sizing check arithmetic
  before allocation; UI glyph-page allocation stops before writing through null.
- PNG decode scratch allocations, JPEG result buffers, and TGA encode buffers
  fail closed on allocation errors instead of writing through null storage.
- Wayland window and optional protocol listeners stop before dereferencing
  failed allocations, destroying the corresponding proxies on failure.
- DirectInput GUID and data-format setup retry only missing allocations after
  partial OOM instead of treating partial state as initialized.
- EGL display/config/context setup checks every scratch allocation before
  passing or writing through its storage.
- Mock-renderer CPU surfaces are freed when switching to a hardware renderer,
  on failed hardware initialization, and on close/reinitialization.
- Mock fast-fill skips are disabled during auto-dump, frame-hash, and UI replay
  capture so captures retain the clear color and rendered pixels.
- Terminal renderer readback buffers are released through a deferred,
  type-checked cleanup helper, including when ANSI conversion exits early.
- Editor headless mock exit uses the render facade's exported `close_window`
  API.
- Editor line limits derive from the current measured body-font advance instead
  of a fixed 7.5-pixel cell width.
- Fixed AOT UI font registration by explicitly typing the module-level font
  counter, with O2 coverage and Pong font load verified on the native Vulkan
  path; cached exact-pointer SSO readability probes with lifecycle
  invalidation, reducing repeated runtime address checks.
- Added pure-Ny AU ADPCM (G.721/G.722/G.723), native FLAC/Ogg FLAC, and MPEG
  Layers I–III decoding; compressed streams support frame-range reads and
  seeking without libsndfile for those formats.
- Preserved runtime contracts for typed-buffer store return values and native
  lowering behavior on affected paths.
- Reduced avoidable overhead in hot paths (hashing/hex formatting, string
  building, targeted runtime lookups/caches) while keeping behavior and
  checksums stable.

### Projects, benchmarks, and tooling

- Decompiler defaults to Ny-style lifted statements without disassembly
  comments or raw-instruction fallbacks; it handles sectionless bare ELF
  executable segments, common x86 `INC`/`DEC` and `REP MOVSB` forms, and
  trailing return padding, and fails explicitly on unknown semantic lifts.
- Added an original Breakout game with capped consecutive-clear scoring,
  animated wall and serve entrances, ball trails, floating score popups, impact
  sparks, fading win/loss states, and pause/restart; a keyboard playthrough
  runs through a headless Vulkan compositor.
- Improved example projects and interactive demos (terminal graphics, audio,
  HTTP, small games/utilities) with focused correctness and rendering fixes.
- Benchmark harness output and reporting now give clearer per-fixture results,
  comparable ratios, and optional metric collection; benchmark gates and fixture
  checksums remain the source of truth for comparisons.
- Fixed Test/CI and build tooling reliability for worker isolation, cache reuse,
  platform differences, and focused regression coverage.

### Documentation

- Consolidated contributor guides, measurements, and implementation notes;
  removed obsolete scratch/TODO material from active docs.
- Editor compile-time investigation is closed; the validated 2026-10-22 editor
  compile comparison remains in `etc/docs/spec/stats.md`. Future editor
  compile-time regressions require a fresh reduced reproducer and validated
  baseline/candidate evidence (same source graph, build, target, flags, cache
  state, hashes/BuildID, inline-decision evidence, and validated CPU samples).
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

### Fixed

- Standard-library import coupling and module-path drift.
- x86-64 NYIR stack allocation overlapping spill homes when loop-register
  assignment expands the frame.
- Direct `return` expressions that call a `noreturn` function in switch arms.

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
