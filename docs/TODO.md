# TODO

Open work only. Keep regression fixtures after fixes. Do not weaken assertions
or classify a timeout as solved by extending its limit. Fixed work is recorded
in `docs/CHANGELOG.md`, not here.

## Compiler ABI items (`etc/projects/os` demos) — status 2026-09-18 (late)

All reproducers run standalone with `NYTRIX_STD_CACHE=0` and no extra flags.

RESOLVED this round (mechanics in docs/CHANGELOG.md):
- [x] extern C calls with variable f64 arguments (ffi.ny passes all asserts).
- [x] platform conditionals selecting the wrong branch (server.ny serves).
- [x] boxed-bool truthiness corrupting automata (110.ny renders bit-exact).
- [x] `rt_any_mul` boxed floats multiply correctly; any-value repr prints
      TAG_FLOAT boxes as doubles.
- [x] typed closure parameters no longer double-untag: the lambda callback
      mark treats typed-int params as tagged (generated lambda prologues
      untag them), so `[159].map(fn(i32 op) ...)` receives 159; herodoc.ny
      is back on its typed callback.
- [x] typed local list annotations read correctly everywhere:
      `def list<int>`/`def list<str>` index reads (multi-arg print, print
      loops, f-string interpolation) return elements, not pointer words
      (index reads from an annotated `list<str>` local classify as C
      strings).

- [x] RESOLVED — typed local list annotations read correctly everywhere:
  `def list<int>`/`def list<str>` index reads return elements across
  multi-arg print, loops, and f-string interpolation (annotated `list<str>`
  index reads classify as C strings).

Still open:
- [ ] dynamic-callback env-slot convention (donut): a plain top-level fn
  value invoked through the any-trampolines is entered WITHOUT the env slot
  while closure-prologue bodies expect env+triples — tui_canvas_loop's
  draw(c,z,w,h) reads w=nil, h=-1 from the one-word shift.  Raw-marked
  plain fns now route through raw rt_call3/4 shapes and rt_call_any3/4
  enters raw-marked callables with the plain raw shape; what remains is
  the env-slot convention for bodies compiled with the closure prologue
  (or compiling fn values uniformly with a leading env).
- [ ] server.ny intermittent empty first `recv` under system I/O stress:
  `_read_header_buffer` occasionally gets a 0-byte first chunk while the
  request bytes are still buffered (and execve intermittently fails with
  EIO on the same box — `env: python3: Input/output error`).  Suspect host
  I/O instability (flaky external mount + btrfs pressure) rather than the
  socket code; retest on a fresh boot before touching socket.ny.

- [ ] dynamic-float ELEMENT round-trip via `float(load32_f32(...))`: the
  scalar value itself is 0/garbage — root cause: f64 register class is lost
  through i64 LLVM local slots (rt_print_f64_raw reads XMM0 while bits went
  GP).  A coordinated fix needs type-map-driven f64 alloca slots in
  src/code/native/llvm/emitter.c plus the ARG-fact ladder; two quick
  attempts destabilized other paths and were reverted.  ffi.ny works via
  raw f64 locals.

## Slow / Heavy JIT Modules (was: timeout problems) — RESOLVED 2026-09-18

The exponential compile-time blowups are FIXED (operator/re-export/member/
top-stmt/imported-value/refine/is_f64/attached-method memos, O(1) program
module index, global top-level-value index, cross-module alias constant
folding). All 24 previously-hanging `lib/os/ui` fixtures compile to
completion and PASS — verified serially (`--stop-after=opt --parallel=off`,
~20-120s each) and via the `ny-test --with-stdlib` harness sweep;
325 runtime/error/shape/interop fixtures stay green.  `build/release/std.bc`
(6.3MB) now builds cleanly with the fixed compiler, so `./make ny`-style
cached runs start in well under a second (no more per-run stdlib
recompiles).

Remaining (linear, not exponential): the heaviest modules (mod.ny,
input.ny, hierarchy.ny, tools.ny) still take 40-80s of single-core lowering
on this machine — genuine code volume, no algorithmic cliff found in the
latest samples. Next lever if needed is parallel per-function lowering or
caching lowered NYIR per module hash, not more micro-memos.

- [ ] Optional: parallelize per-function NYIR lowering or add a lowered-NYIR
  per-module cache keyed on source hash.

## Static-analysis scan results (2026-09-17)

Scanner: CodeQL-compatible dataflow/security scan over `$NYTRIX_ROOT`.
Durable category/total summary; the full 2697-item listing lives in the
scanner output.

Totals: 2697 issues  [ 2 HIGH | 1034 MEDIUM | 1661 LOW ]  (Ignored 0 / Open 2697)

### HIGH (2)
- [x] RESOLVED — Path Traversal at src/cmd/test/init.c bench capture CREATE:
  replaced the placeholder substring guards with a single lexical
  normalizer (`ny_test_capture_path_ok`) that resolves relative paths
  against cwd, folds `.`/`..` component-wise, and rejects any climb past
  the floor (cwd prefix for relative inputs, the root slash for absolute
  ones — `..`, `../sibling`, `/tmp/../..`, and `/..` are all rejected;
  nested legit `a/../b` forms pass). Verified against a 13-case table
  including the not-yet-existing-file case the old realpath guard broke on.

### Category breakdown (open, MEDIUM)
- Buffer Overflow — src/cmd/fuzz/compare/tools.c, src/cmd/web/init.c,
  src/cmd/ny/repl/repl.c, src/cmd/test/pkg.c, src/base/loader.c
- Command/Code Injection — perf.c; pkg.c taint into dlopen/execvp/execv
- Double Free — src/cmd/test/elf.c (x7), repl/repl.c
- Integer Overflow — runtime/pipe/init.c; fmt pkg.c
- Missing Release of Memory — loader.c, format.c, hm.c, egraph.c, binary.c
- Path Traversal (MEDIUM/LOW) — loader.c (x174)
- Use After Free — egraph.c, jit.c, binary.c, ssa.c, format.c

Remediation is a follow-up for the compiler backend (native/IR + typing
pipeline) — NOT part of the LSP decouple item.

## Session continuity note

The working tree was reset several times during review, silently dropping
uncommitted compiler fixes. Recovery tooling lives in the repo:
- `tmp/reapply_fixes.py` + `tmp/reapply_fixes2.py` — idempotent re-application
  of the full native-lowering fix set (run from repo root).
- `tmp/fix-backup-YYYYMMDD-HHMM.patch` — full working-tree diff snapshots.
Commit the working tree before running git operations that touch it.

Repo hygiene: docs/TODO.md is the single source of truth; there is NO root
TODO.md (docs-only; root copy removed by prior instruction).
