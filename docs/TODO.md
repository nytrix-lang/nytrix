# TODO

Open work only. Re-read before editing: other agents may add findings here.
Remove a fixture only after a fresh, cache-disabled pass; retain regression tests.
A proposed diagnosis is not proof that a fix works. Do not weaken assertions,
hide checksum mismatches, or classify a timeout as solved by increasing its limit.

## Compiler and runtime

Keep this section limited to work that still has an observable failure or missing
implementation. Completed runtime-ABI renames and test-only investigations are
removed from this document; their regression fixtures remain in the suite.

- [ ] **One ASM representation for both spellings:** the parser now normalizes
  newline-delimited `asm { in ... out ... instruction ... }` blocks and compact
  `asm("...", "...", ...)` calls into the same `NY_E_ASM` node; the native
  regression covers both spellings, named operands, and clobbers. Finish
  backend-independent validation and diagnostics for register classes,
  malformed templates, and input/output arity, then extend parity coverage to
  LLVM. ASM result typing is now derived from the normalized output constraint
  (machine-word integer or floating-register `f64`) so both backends preserve
  the value ABI instead of treating compact calls as opaque `any`.
  Operand-only structured blocks are now rejected during parsing with the
  same diagnostic path used by both backends.

- [ ] Make the remaining shared NYIR lowering consume semantic representation
  and ABI facts rather than spelling-only fallbacks. Audit the remaining native
  lowering groups and validate specialization, bounds/tag elimination, alias
  analysis, and allocation on both backends. The native predicate family
  (`is_nil`, `is_int`, `is_ptr`, and container predicates) now dispatches from
  the canonical semantic callee, with surface spelling retained only for
  unresolved/synthesized nodes. Generic `repr`/`to_str` and `get`/`dict_get`
  dispatch now use the same semantic path; broader intrinsic groups still
  need the same migration. Common conversion and container mutation groups
  (`int`/`to_int`, `set`, `delete`/`remove`, `append`, and `len`) now also
  dispatch from that canonical callee. Collection and arithmetic helpers
  (`keys`, `values`, `items`, `add`, `contains`, `abs`, `min`, `max`,
  `clamp`, `lerp`, `sin`, `cos`, and `sqrt`) now follow it too.
  Thread spawn and callback-launch intrinsics now use the same semantic
  dispatch and preserve their existing ABI packing.
  Untyped user functions whose parameters are used by scalar operators now
  derive their raw integer call ABI from the lowered body, preventing dynamic
  metadata from being appended to raw calls (the comments regression passes).
  Aliases to those functions now carry the same raw parameter and scalar
  return facts, while direct dynamic functions retain their tagged ABI.
  Dictionary-index comparisons now retain the tagged-value boundary even when
  scalar inference reports a raw integer (pipeline/member and typed-merge
  regressions pass without caches).
  The `cttz.i64`/`ctlz.i64` intrinsic bridge now keeps raw bit operations raw
  and tags the returned count exactly once; both zero/defined-count contracts
  are preserved.

- [ ] Define one explicit dynamic-value/container ABI. A raw eight-byte slot
  cannot distinguish integers, nil/bools, float bits, pointers, and nested
  containers. Choose explicit element metadata or canonical dynamic storage and
  decode each value exactly once across calls, returns, captures, globals,
  dictionary access, callbacks, and list writes. Remaining failures are
  cross-call dynamic reads/callbacks, dictionary representation outside the
  scalar-boxed path, and vector/object returns across both engines. BigFloat
  handle detection now preserves the tagged formatter for generic
  `to_str(any)`. Free
  `get(...)` now selects the tag-aware accessor for descriptor-backed lists and
  the raw-index ABI for scalar lists. Opaque string-builder calls keep the raw
  builder handle while converting literal payloads to the tagged string ABI at
  the native boundary.
  Scalar element-8 reads no longer infer integer-ness from the low tag bit;
  small raw integers are boxed explicitly, avoiding collisions with native
  pointer tags during dynamic `get`.
  Mixed arithmetic now tags semantically raw scalar subexpressions (including
  computed indices such as `i % n`) exactly once at an `any` boundary; the
  dynamic integer-list checksum reaches the expected result.
  Native typed-buffer growth now updates the handle registry after `realloc`,
  preserving list length and metadata through `__list_reserve`.

  The LLVM proof-logic path now builds, but its bounded assignment-count
  assertion still fails in the full fixture; keep the raw-index/value ABI work
  open until that result is correct with caches disabled.

- [ ] Finish the remaining import-scope and probe families: vector/object
  return ABI, image decoder paths, and compile-heavy UI/Vulkan fixtures.
  The XML attribute probe now passes cold in 535 ms under the 20-second
  fixture ceiling. Device-dependent probes (`ci skip`) remain excluded from
  the default CPU suite and can be exercised with
  `NYTRIX_TEST_INCLUDE_CI=1`.

## Semantic model roadmap

Only additive semantic extensions remain below; the shipped foundation and
its regression coverage are intentionally omitted from this worklist.

- [ ] Generalize value-indexed types beyond `Fin<N>` to user-defined bounded
  constructors such as `Vec<T, N>`, dependent result normalization, and
  first-class `where` obligations. Preserve versioned mutation facts and emit
  solver counterexamples from every lowering path while extending the existing
  bounds machinery to user-defined constructors and dependent normalization.

- [ ] Add hygienic AST `quote { ... }` and typed `${...}` splicing so comptime
  functions can return declarations, types, expressions, tests, and
  serializers. Generated names must carry lexical scope and never expose
  compiler pointers; existing textual templates remain a compatibility layer.

- [ ] Add effect parameters/capability values and row-polymorphic `shape`
  composition for typestate APIs. Keep ABI-sensitive layouts nominal and
  protocol/schema implementations in ordinary libraries.

Cross-call specialization now has the bounded implementation required for the
current semantic model: deterministic type/list-length keys, `Fin<N>` bound
compatibility checks, per-function and global specialization caps, body-cost
limits, comptime fuel, and emitted specialization byte/function budgets. The
remaining semantic extension is to add proof/effect rows to those keys once
the dependent/effect forms above exist; do not duplicate a second budget
system.
