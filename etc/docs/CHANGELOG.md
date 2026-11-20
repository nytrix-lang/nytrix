<!-- nytrix-doc: {"audience":"user","featured":true,"group":"project","order":10,"summary":"Release highlights and notable changes for each version."} -->
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

- SysV x86-64 `va_arg` now reads supported two-eightbyte and memory-class
  aggregates. An aggregate that does not fit in the remaining registers
  comes entirely from the stack, leaving both register cursors unchanged.
- Corrected `_Bool *` arguments, assignments, and returns so they retain the
  pointer rather than being converted to a boolean. Scalar returns from
  `_Bool` functions and explicit casts to `_Bool` now normalize nonzero
  values to one instead of truncating to a byte.
- Multidimensional array and aggregate pointer indexing uses the pointee
  stride when taking a subobject's address. File-scope integer arrays retain
  dimensions from array typedefs, and subtraction counts complete rows rather
  than bytes.
- Completed file-scope pointer arrays retain their bound and reserve
  zero-initialized storage after an earlier incomplete declaration.
- Preserved GNU section and alignment attributes before `__typeof__` and
  trailing or inherited alignment on file-scope records. Automatic record
  and record-array compound literals zero omitted fields and elements on each
  evaluation. Block-local aggregates declared with `typeof` now receive
  record storage, including inside GNU statement expressions.
- Fixed SysV x86-64 returns that mix INTEGER and SSE eightbytes, in both
  class orders. Integer-only `-mno-sse` variadic functions preserve the
  `va_list` calling convention without touching SSE registers. Scalar
  inlining and value numbering preserve calls that feed aggregate return
  registers and keep results from separate calls distinct.
- Fixed x86-64 GNU assembly for narrow memory accesses, unsigned division and
  shifts, and GOT-relative `-fPIC` data references. Fused bit tests now keep
  their input live until it is read. ELF relocations use the correct symbol
  index after sorting definitions, including in vDSO objects.
- Fixed peephole range inference at function-entry loops: facts from a
  loop backedge no longer erase its exit guard on the initial iteration.
- Preserved the segment override when loading or storing an aggregate pointer
  through a GNU FS/GS-qualified address. Linux now accesses both the per-CPU
  IRQ stack pointer and saved register pointer in the correct segment.
- Preserved FS/GS on scalar increment and decrement stores and on x86-64
  single-precision memory moves. The read and write now use the same segment.
- Named FS/GS scalar objects retain their segment on reads, writes, and
  address-of dereferences.
- Preserved FS/GS on named GNU inline-assembly memory outputs, including
  bitwise updates, decrement-and-test, and compare-exchange.
- Placed const ELF objects in read-only storage, using relocatable read-only
  sections for pointer-containing data in PIE and shared links.
- Fixed packed descriptor operands for `lgdt` and `lidt`, and preserved
  explicit RIP-relative inline assembly operands under PIC. Inlined x86
  memory-capable asm outputs keep their destination and cannot return a stale
  C value, including when the output overwrites a formal parameter. Required
  static-branch inlining now runs before optional stackless inlining; scoped
  cleanups run on C and asm-goto exits without running a guard's destructor
  on a jump to a macro-generated label inside that same guard.
- Reduced i386 real-mode code size by emitting fewer frame loads, stores,
  and copies, including for four-byte stack calls and wide integer arithmetic.
  Low-stack real-mode code uses shorter fixed-frame prologues and folds
  one-use symbol addresses into loads and stores with 32-bit relocations. These
  changes bring the Linux setup replay within its sector limit. The separate
  setup memory limit still prevents a `bzImage`.

See the [NYC compiler guide](spec/nyc.md#verification-evidence) for the
build and runtime evidence and its limits.

### Compiler and backends

- Safe mode also arms the guard-page user heap while retaining RC/RAII
  cleanup. LLVM AddressSanitizer and ThreadSanitizer passes receive the
  required function attributes; sanitizer compilation fails closed on
  unsupported native encoders or failed instrumentation.
- Strict ownership tracks raw allocation identities through copied and
  interior local aliases and explicit releasing callees. Uncaught panics run
  active defers; managed dictionary allocation paths check failures before
  dereferencing newly allocated storage.
- Ownership call contracts apply to every annotated parameter. Owned factory
  results preserve caller-side provenance, and consuming a raw owner also
  invalidates its copied/interior aliases.
- Indexed resource extraction conservatively invalidates its collection
  owner, including resource-bearing layout fields. Ownership-rule errors now
  fail shared type validation instead of printing an error and returning
  success. Layout fields use the common generic/alias type grammar; nominal
  dictionary values retain their layout when reading a field.
- Native dictionary and set constructors reject capacities above 134,217,728
  before allocating; growth rejects capacity overflow and reports allocation
  failure without silently losing insertions. Stdlib bootstrap dictionaries do
  not consume the user allocation-injection budget.
  Regression coverage includes scoped-pointer fallthrough, return, loop
  exits, and panic, plus emitted-code ASan heap/stack/global boundaries.
- Compiled safe-heap byte accesses check requested sizes rather than rounded
  allocation capacity, including padding, underflow, and interior pointers.
  Top-level lexical defers collect their environments before helper emission;
  resource cleanup and deferred local mutation work outside functions too.
- Alias summaries distinguish scalar parameter values from address-taken
  parameter storage. Checked dynamic indices now work in safe-profile stack
  byte reads without weakening borrow-escape rejection. Stack and explicitly
  bounded FFI byte accesses have boundary/panic-cleanup coverage on native,
  LLVM MCJIT, and ORC. Byte load/store APIs also accept an explicit dynamic
  byte extent, checking stack and FFI boundaries before access and unwinding
  defers on failure; omitted extents retain the raw compatibility contract.
- User-memory fuzzing runs deterministic allocation/byte-access mutations under
  native/LLVM safe heap and emitted ASan. LLVM literal-array backing symbols
  remain valid ASan COMDAT signatures. Numeric native string conversions use
  managed storage rather than leaking uncached formatting buffers.
- Substitution-table growth preserves every moved allocation if a later
  realloc fails, allowing retry and safe destruction. Failure injection covers
  each growth allocation and capacity overflow.
- `--sanitize=memory` instruments emitted code and the runtime with MSan;
  initialized and uninitialized byte reads have acceptance coverage. TSan
  checks generated parent/worker writes and distinguishes a join-ordered read.
  Both lanes link PIE executables for shadow-memory compatibility.
- `--sanitize=ub`/`undefined` checks emitted signed 64-bit arithmetic and null
  accesses. Null checks precede LLVM optimization; saved executable coverage
  includes defined operations and eight failure modes at `-O0` and `-O2`.
  The sanitizer fuzz command runs all four emitted sanitizer lanes, with
  valid-operation controls and named failure reports.
- Bump allocators retain byte offsets without tagged-integer corruption and
  reject overflowing/exhausted requests without changing their cursor.
  Pointer arithmetic preserves its pointer result across dynamic boundaries.
  Native precondition/refinement substitutions use the builder-owned arena.
- NYC emits real SysV x86-64 stack canaries for stack-protector options,
  preserving option order, predefines, and GNU function opt-outs. Both output
  routes detect corrupted frame cookies with FS/GS TLS, linker-resolved
  offsets, and global guards; checks do not use spillable guard pointers.
- Single MOVS instructions preserve both tied pointer outputs at every
  scalar width. Locked and unlocked CMPXCHG16B preserve expected-value fields
  with or without a condition output. Values live across fixed-register
  assembly are no longer kept in clobbered caller-saved registers.
- PadLock SHA256 and repeated port-I/O templates now have explicit operand
  lowering rather than speculative passthrough. SHA256 hardware execution
  remains unverified; the strict kernel SHA256 and iomap objects compile.
  Both native output routes execute byte/word/dword REP INS/OUTS in a
  ring-zero QEMU guest, checking input data, boundaries, pointer/count
  outputs, zero counts, and byte output through the debug console.
- NYC retains prefix `section` attributes on aggregate, integer-array,
  pointer-array, and callback globals. ELF objects preserve exact section
  names without inserting a leading dot, matching GNU assembly and enabling
  linker-generated section bounds.
- NYC emits inline helpers referenced by static callback tables, and
  post-inline slot promotion at `-O1` removes disabled short-circuit feature
  branches before they can retain unavailable linker symbols.
- Aggregate object aliases declared with GNU `typeof` retain their target,
  including Linux startup PIC aliases. Both assembly and direct ELF output
  bind data aliases to the target's original storage and section.
- Named local labels in file-scope assembly remain addressable from C without
  requiring `.globl`, preserving trampoline data and local symbol binding.
- Section-attributed static helpers remain eligible for heuristic inlining
  while their out-of-line definitions are retained. Empty feature-disabled
  helpers no longer keep undefined argument symbols alive; argument side
  effects are preserved.
- Dominating equality and inequality branches now eliminate contradictory
  tests without carrying facts through joins or changed memory. Private
  integer stack fields retain their values across unrelated calls, including
  their byte, word, and signed-dword load semantics.
- Casting a nonvolatile C lvalue to `void` discards its read while preserving
  address-expression side effects; volatile reads remain observable.
- GNU aggregate-array aliases preserve local binding, including static Linux
  device-table aliases. Late inline cleanup refreshes instruction effects
  before verification.
- Direct ELF output encodes byte, word, dword, and qword memory exchanges,
  including function-pointer operands and signed narrow return values.
  Unsupported assembly no longer substitutes GS offset zero or exchanges
  with uninitialized operands; both output routes reject those templates.
- C constant objects retain read-only section flags, and `.note.*` sections
  use ELF note type in both output routes. Linux build notes no longer
  trigger conflicting section-type or section-attribute assembler warnings.
- File-scope static and weak bindings are collected through one name index
  instead of rescanning the whole translation unit for each exported object.
- Repeated ordinary and typedef callback declarations now denote one object.
  Weak ELF data and defining-unit reads and writes honor strong interposition
  in both native output routes, fixing duplicate kernel reboot callbacks.
- C storage bindings follow actual declarators, not references inside weak
  functions, prototype parameters, typedef spellings, or initializers.
  Pointer and suffix attributes do not leak to neighboring declarators.
  Mixed scalar/pointer initializer lists retain each declarator's type,
  relocations, and previously initialized storage.
- Opaque UD2 inline assembly no longer implies C `noreturn`: signal and
  kernel handlers may resume it. Runtime regressions resume plain traps in
  both output routes and labelled, section-annotated traps in assembly output.
  The direct encoder still rejects the unsupported annotated form.
- Native x86-64 text emission omits branches to the next label, removing a
  dead zero-offset jump after Linux's fatal BUG trap without guessing trap
  semantics or suppressing `objtool` diagnostics.
- NYC frame-pointer options obey their command-line order: a final
  `-fno-omit-frame-pointer` cancels earlier omission and restores the supported
  single-function route. The option-order regression executes eight-argument
  calls with positive and negative values.
- Private nonescaping C functions propagate scalar parameter constants only
  when every direct call agrees, without changing signatures, removing calls,
  bypassing noinline or assuming address-taken formals remain unchanged. A
  strict kernel `swap_state.o` no longer retains the disabled THP helper.
- Physical x86-64 assembly and object prologues materialize SysV aggregate
  parameters as addresses of their actual by-value storage, including mixed
  integer/SSE register classes, register-bank rollback and stack arguments.
  Interoperability regressions execute calls from the host compiler through
  both output routes; the C frontend suite passes.
- Branch-protected C assembly places ENDBR at address-taken labels as well
  as function entries. Forward and backward label addresses have runtime
  landing-byte checks at O2/O3; all 256 BPF interpreter data-relocation targets
  pass a strict-object byte check. Branch compare fusion honors its earlier
  adjacency proof rather than reloading operands from reused stack homes.
- CPUID assembly accepts omitted input/clobber sections, read-write register
  operands and reordered numeric ties. x86-64 assembly/direct-object and
  freestanding i386 regressions compare real instruction results with the
  host compiler; the encoders preserve EBX/RBX.
- CPUID preservation checks now keep a real RBX value live across calls and
  exercise live compiler values across the instruction. Assembly restores RBX
  before loading output addresses; the direct encoder also avoids reading
  stack homes while temporary pushes change RSP.
- Raw RDMSR/WRMSR assembly has explicit low/high-word operand lowering for
  32-bit integer outputs, including structure members and reordered inputs.
  Ring-zero QEMU execution verifies read/write/readback through x86-64 assembly
  and direct objects and i386 assembly. The i386 probe also reads, restores and
  rereads CR0; control-register writes accept omitted trailing asm sections.
  Physical hardware execution is not verified.
- Bit-string set/reset/complement operations retain their locked or unlocked
  instruction semantics and signed indexing across memory words. Carry-output
  bit tests accept omitted clobber sections without an extra C memory read.
  Both native routes and i386 execute the boundary regressions; direct-object
  disassembly distinguishes the actual LOCK prefix.
- x87 initialization/status assembly marks output locals address-taken rather
  than reusing their initial constants. i386 and direct-object emitters perform
  real 16-bit status/control writes, preserving neighboring high bytes. Fresh
  strict real-mode/compressed CPU probes and boot printf now compile.
- Repeated byte comparisons preserve tied source/destination cursors, remaining
  native-width counts and the final comparison flag, including null zero-count
  inputs and backwards traversal. Assembly/direct-object and i386 runtime
  regressions cover these cases. i386 local loads no longer write a high word
  into a four-byte value home and corrupt its live neighbor.
- Unsigned 64-by-32 inline division accepts tied accumulator inputs and
  member/indirect quotient and remainder outputs without extra output reads.
  Two-stage REP word/byte copies preserve both cursors and consume the actual
  tail count, including backwards traversal. Native assembly/direct-object and
  all i386 regparm regressions pass with a frozen compiler; the actual boot
  string and compressed string Kbuild commands now compile.
- Fixed R9 assembly inputs evaluate the real C operand and write the actual
  register. Both native output routes pass a protected-page fault/resume probe,
  return the loaded 64-bit value through R9 and observe subsequent memory
  changes. The strict decompressor identity-map object now compiles.
- Explicit FS/GS assembly performs real byte, word and dword accesses without
  an ordinary C memory read at the unsegmented address. Runtime probes use
  distinct real segment bases through both x86-64 output routes and all i386
  regparm settings. Plain selector writes no longer acquire invented exception
  fixups; i386 selector/memory reads respect narrow value-home allocation.
  The actual A20 boot object now compiles. General C address-space qualifiers
  remain a separate incomplete requirement.
- i386 variadic definitions use pointer-shaped lists and support copies,
  mixed-width extraction, aggregate arguments and hidden-result returns.
  GCC interoperability covers all four regparm settings; its nonzero-regparm
  variadic result-pointer popping differs from Clang's default. x87 conversion
  preserves full-width integers and restores the rounding control word after
  truncating toward zero. The C frontend suite and IR effects roundtrip pass.
- Declaration-binding lookahead resets alias state between declarators,
  preserving distinct GNU weak function aliases in the VDSO clock objects.
  Both 32-bit and 64-bit strict VDSO compilation and alias runtime regressions
  pass.
- Constant C helper results propagate even when the call must remain for its
  observable effects or possible nontermination. The late cleanup recognizes
  these changes without requiring a lower call count. Focused truncate and
  device-mapper objects no longer reference disabled DAX mapping helpers or
  `dm_zone_endio`; callback-table reachability and THP specialization remain
  separate open link blockers.
- Unreferenced private file-scope data no longer roots dead callback
  definitions. Explicit used definitions, named sections, public objects,
  aliases, assembly references, and function-owned statics retain their
  existing lifetimes. Both output routes pass callback retention/runtime
  regressions; the strict device-mapper object no longer references
  `dax_get_private`.
- NYIR use-def analysis and value replacement keep asm-goto targets in the
  label namespace, including labels whose numbers overlap SSA value IDs or
  exceed the value count. Static-feature cleanup no longer fails on those
  valid inline-assembly branches.
- Discarded C void expressions remain valid operands of comma expressions,
  including the connector iteration macros used by DRM.
- NYC preserves local static pointer tables referenced by other data relocations;
  the vector PHI classifier now traverses shared copy chains iteratively.
- GNU C label-address functions are not inlined across function-local label
  symbols, and DCE recognizes those symbols as roots for their target blocks.
- C `section` attributes concatenate adjacent string literals, preserving
  kernel registration sections such as `__governor_thermal_table`.
- Function section attributes survive parameter-type parsing, preserving
  kernel placements such as `__init` on functions with parameters.
- NYC prunes unreferenced `static inline` helpers even when section-attributed,
  avoiding dead references to feature-disabled implementations.
- C calls to `noreturn` functions now terminate their lowering path without
  synthetic continuation code, preserving the declared control-flow contract.
- NYC preserves signed 32-bit absolute relocations in C assembly and ELF object
  output, fixing x86-64 kernel trampoline address relocations.
- C file-scope character-array declarations retain ELF section and alias
  attributes, including Linux exported-table aliases.
- C file-scope aggregate definitions retain declarator and type `section`
  attributes, preserving kernel init/setup tables in their requested sections.
- C zero-bound aggregate definitions remain complete zero-size objects when
  following an incomplete `extern` array declaration.
- Function symbols emitted from file-scope assembly remain functions in C
  expressions, including address-taking and function-pointer initialization.
- C weak data attributes are preserved across repeated declarations and
  definitions, including suffix-form `__weak` used by the kernel's version
  object.
- NYIR constant folding carries local facts across an unconditional branch only
  when it targets the immediately following label with a unique predecessor;
  facts remain unknown at control-flow joins.
- NYIR jump threading leaves branch-only blocks intact when their PHI results
  are used beyond the block, preserving SSA dominance across retargeted edges.
- Memory SSA keeps differing incoming constants in a real join PHI instead of
  reusing one predecessor's non-dominating definition.
- NYC's `-O0` pipeline promotes scalar locals through SSA and folds unsigned
  right-shift constants, eliminating Linux `GENMASK` runtime `__bad_mask`
  checks even when the mask crosses a control-flow join. Late CFG simplification
  is skipped at `-O0` after SSA promotion because it produced verifier-invalid
  copies on kernel control flow; focused kernel rebuilds validate the route.
- Linux-kernel-compatible C conditional expressions with a void-valued false
  arm now preserve both arms' side effects without emitting a value store for
  the void arm. The x86-64 NYIR encoder also accepts functions with up to
  65,536 value slots, covering the 46,507-value Intel VDSC function.
- x86-64 C inline assembly lowers the CRC32B/W/L/Q forms with 32-bit
  accumulators and 8/16/32/64-bit integer inputs.
- C logical expressions skip dead right operands when a constant left operand
  short-circuits, preserving Linux `__builtin_constant_p` guards around
  jump-label assembly.
- C `__builtin_strcmp` folds adjacent string literals, and straight-line arms
  of constant `if` conditions are checked in scratch IR before omitting the
  unselected arm; this supports Linux scheduler compile-time operator guards at
  `-O0`.
- Unreachable C code resumes at enclosing control-flow statements when labels
  are nested in loops or switches, preserving the correct `break`/`continue`
  targets while still compiling externally reachable labels.
- Constant-if pruning retains `used` static locals from dead arms, including
  Linux static-call addressability anchors in `.discard.addressable`.
- O0 now runs safe constant folding and constant-branch cleanup, allowing
  always-inline bitfield helpers to eliminate compile-time error calls.
- NYC keeps `.discard.addressable` relocations owned by live C functions after
  optimization and inlining, while frontend pruning still removes anchors from
  unreachable static functions. Static-local addresses are now emitted on use,
  avoiding runtime references into linker-discarded anchor sections.
- O1 whole-unit C compilation considers discardable static helpers of at most
  eight NYIR instructions for the small inliner, allowing tiny ordinary
  `static inline` predicates to fold compile-time-disabled kernel paths without
  enabling the general O2 inliner.
- C `always_inline` helpers are explicitly cloned at every optimization level
  before code generation, preserving constant specialization and eliminating
  out-of-line calls required to disappear by the attribute.
- File-scope character arrays no longer inherit a stale section attribute from
  a preceding static addressable anchor; an ELF section regression covers the
  emitted symbol placement.
- C file-scope function-pointer objects declared through function-type typedefs
  retain tentative definitions and their callback signatures, including prior
  `extern` declarations.
- The O1 small inliner resolves C-frontend `extern`-flagged calls against
  registered same-translation-unit definitions; foreign calls outside a C
  unit remain out of line.
- NYR jump threading now accepts zero-value control-flow graphs without
  incorrectly failing constant collection.
- NYIR typed-value maps allocate regions only for classes observed in a
  function, sharing one zeroed region among absent classes instead of
  allocating fourteen full-size arrays.
- The descriptor-table `sldt %0` inline-assembly form retains its no-result
  contract; a focused regression guards against assigning it a spurious NYIR
  destination.
- Native reachability now trusts the frontend's resolved builtin-operator
  facts, avoiding repeated module/impl scans for ordinary arithmetic.
  Explicitly selected overloads and unresolved expressions retain target
  discovery; operator regressions include overloaded call-result operands.
- Fixed incremental dependency inventories: cold/forced builds now schedule
  inputs, removed providers invalidate surviving consumers, bare module names
  match `.ny` files, and ambiguous basenames retain all candidate dependencies.
  Graph reload no longer reads a freed pathname; cache roots follow the current
  configuration, and allocation/publication failures are reported.
- HM unification snapshots type-detail/deep tracing policy once per pass,
  avoiding repeated debug-policy calls for each constraint while preserving
  verbose constraint and substitution output.
- Native compile-time cleanup indexes verifier definitions once per verification,
  avoids PHI/select/local-store analyses when their instructions are absent, and
  indexes immediate module imports without changing re-export precedence.
  An isolated three-pair Pong object-compilation comparison reduced median wall
  time from 40.840 s to 39.788 s (2.6%) with byte-identical objects.
- Native x86-64 lowering improved register allocation, call-boundary handling,
  signed arithmetic, address/base forwarding, immediate multiply encoding, and
  several miscompilation/crash cases found in focused regressions.
- Refinement lookup now indexes the main and generated program trees while
  preserving traversal precedence, avoiding repeated full-AST scans for
  distinct refinement names.
- Exported-member checks scan module declarations directly instead of
  allocating and deduplicating a temporary export-name list.
- Module alias resolution writes bounded path copies and joins directly,
  avoiding formatted-I/O calls in its recursive lookup path.
- Native module-member lookup caches compare the stored key hash before
  comparing member strings, avoiding string scans on set collisions while
  retaining exact module/member verification.
- Expanded the four-way module-member lookup cache from 16,384 to 65,536
  entries after cold Pong profiling showed repeated imported-member tree scans;
  the cache-disabled trace recorded 7.4% fewer such scans, but no total
  compile-time speedup is established.
- x86-64 home DSE now skips reference-buffer allocation and byte scanning when
  a function has no scalar homes or the probe emits no home stores; the probe
  encoder is still reused, preserving emitted bytes.
- Native list append dispatch now gives f32/f64 classification precedence over
  stale raw-integer ABI facts, preventing floating values from reaching the
  raw-i64 append entry. Terminal/editor native fixtures and integer-append
  regressions pass.
- HM environment lookup caches positive and negative results, checking
  appended bindings before reusing either result to preserve shadowing and
  syntax-context behavior across cloned environments.
- HM unification now skips constraint type-string formatting and allocation
  when type-detail tracing is disabled; `-vvvv --stop-after=hm` still emits
  constraint and substitution details.
- HM function-scheme inference reuses its temporary environment buffer across
  sequential schemes instead of allocating and freeing a buffer per function.
- HM environment clones reserve their final capacity once and copy entries in
  bulk instead of growing the vector one binding at a time.
- Loop-phi register coalescing now recolors a preheader source only when it
  dies at the copy and candidate-color constraints pass; call-crossing and
  non-dying values retain their existing register/home behavior.
- NYR and LLVM execution paths received targeted correctness and performance
  work, including typed-buffer/store behavior, inlining/splice budget handling,
  block layout guards, range/memory facts, and runtime fast paths where safe.
- NYC/C frontend work expanded accepted C constructs and fixed targeted parser,
  preprocessor, declarator, initializer, typeof, array, and assembly-related
  cases. Target-dependent optimization and instruction selection remain limited.
- NYC retains called static-inline jump-label helpers when they need out-of-line
  emission, and its whole-unit assembly path honors `-fcf-protection=branch`
  with ENDBR entries; unsupported control-flow modes and direct-object branch
  protection fail explicitly.
- Cross-target backends (AVR, ARM/AArch64, i386, RISC-V64, MIPS64, PowerPC64,
  Wasm/BPF-related paths, and object/ABI handling) received focused ABI,
  lowering, and fail-closed fixes covered by targeted tests.

### Language, diagnostics, and analysis

- Added and refined compile-time checks, effect handling, proof/refinement
  behavior, template/macro diagnostics, and type/ownership-related validation.
- Improved diagnostics for unsupported operations, invalid compile-time claims,
  and backend/target limitations so failures are explicit rather than silently
  miscompiled.
- Fixed targeted semantic/lowering issues involving partial application and
  currying paths, callback/effect inference, dynamic indexing cases, and
  ABI-sensitive call lowering.
- Fixed NYR lowering for integer conversion of dynamic-list elements, tagged
  floating-point builtin returns, raw-integer `append` results, and list
  copy-on-write aliases mutated inside loops; the affected regression fixtures
  pass their configured execution variants.
- Fixed DCE's unreachable-instruction removal to erase in reverse index order,
  preserving original CFG-to-instruction mappings while compacting SSA.
- Vectorized loop reductions now initialize unused NYIR operands to the
  sentinel value, so later SSA compaction does not treat zero as a live value.

### Runtime and standard library

- Fixed correctness issues in string/list/buffer operations, hashing/crypto
  helpers, subprocess/process handling, terminal and UI behavior, audio
  resource loading, HTTP handling, and platform-specific bridges.
- Checked the legacy crypt hash scratch-buffer size arithmetic and allocation,
  and made streamed dictionary scans close their opened file before reporting
  scratch-buffer allocation failure.
- BMP decoding now checks pixel-buffer size arithmetic and allocation before
  initializing the output.
- GIF decode and encode paths now check temporary pixel, frame, and image
  buffers before writing through them.
- Linux filesystem watcher polling now fails clearly if its event buffer cannot
  be allocated.
- Sound-file decoding checks metadata/sample allocation sizes and closes native
  handles on failure; mixer buffers preserve their prior allocation when
  `realloc` fails.
- Loader vectors, zlib output growth, and LZ4 frame sizing now check arithmetic
  before allocation; UI glyph-page allocation stops before writing through null.
- PNG decode scratch allocations plus JPEG result buffers and TGA encode buffers
  now fail closed on allocation errors instead of writing through null storage.
- Wayland window and optional protocol listeners now stop before dereferencing
  failed allocations, destroying the corresponding proxies on failure.
- DirectInput GUID and data-format setup now retries only missing allocations
  after partial OOM instead of treating a partial state as initialized.
- EGL display/config/context setup now checks every scratch allocation before
  passing or writing through its storage.
- Mock-renderer CPU surfaces are now freed when switching to a hardware
  renderer, on failed hardware initialization, and on close/reinitialization.
- Mock fast-fill skips are disabled during auto-dump, frame-hash, and UI replay
  capture so those captures retain the clear color and rendered pixels.
- Terminal renderer readback buffers are released through a deferred,
  type-checked cleanup helper, including when ANSI conversion exits early.
- The editor's headless mock exit now uses the render facade's exported
  `close_window` API.
- Editor line limits now derive from the current measured body-font advance
  instead of a fixed 7.5-pixel cell width.
- Fixed AOT UI font registration by keeping the module-level font counter
  explicitly typed; added O2 coverage and verified the Pong font load on the
  native Vulkan path. Cached exact-pointer SSO readability probes with
  lifecycle invalidation, reducing repeated runtime address checks.
- Added pure-Ny AU ADPCM (G.721/G.722/G.723), native FLAC/Ogg FLAC, and MPEG
  Layers I–III decoding. Compressed streams support frame-range reads and
  seeking without requiring libsndfile for those formats.
- Preserved runtime contracts in focused fixes, including typed-buffer store
  return values and native lowering behavior for affected paths.
- Reduced avoidable overhead in selected hot paths (for example, hashing/hex
  formatting, string building, and targeted runtime lookups/caches) while
  keeping behavior and checksums stable.

### Projects, benchmarks, and tooling

- Decompiler output now defaults to Ny-style lifted statements without
  disassembly comments or raw-instruction fallbacks. Sectionless bare ELF
  executable segments, common x86 `INC`/`DEC` and `REP MOVSB` forms, and
  trailing return padding are handled; unknown semantic lifts fail explicitly.
- Added an original Breakout game with capped consecutive-clear scoring,
  animated wall and serve entrances, ball trails, floating score popups, impact
  sparks, fading win/loss states, pause/restart, and a keyboard playthrough
  through a headless Vulkan compositor.
- Improved example projects and interactive demos (terminal graphics, audio,
  HTTP, and small games/utilities) with focused correctness and rendering fixes.
- Benchmark harness output and reporting were refined for clearer per-fixture
  results, comparable ratios, and optional metric collection; benchmark gates
  and fixture checksums remain the source of truth for comparisons.
- Test/CI and build tooling received reliability fixes for worker isolation,
  cache reuse, platform differences, and focused regression coverage.

### Documentation

- Consolidated contributor guides, measurements, and implementation notes;
  removed obsolete scratch/TODO material from active docs.
- Editor compile-time investigation is closed. The validated 2026-10-22 editor
  compile comparison remains in `etc/docs/spec/stats.md`. Future editor
  compile-time regressions should be reported with a fresh reduced reproducer
  and validated baseline/candidate evidence (same source graph, build, target,
  flags, cache state, hashes/BuildID, inline-decision evidence, and validated
  CPU samples).

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
