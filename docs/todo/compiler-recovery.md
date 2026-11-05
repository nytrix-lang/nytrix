<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":3,"summary":"Confirmed compiler failures and their acceptance criteria."} -->
# Compiler recovery (P0)

The completed 2,494-case run passed 2,482 cases and failed 12. Keep only the
confirmed unresolved failures below; closed observations are recorded as
passed and must not be reopened without new evidence.
Paths are relative to `etc/tests/`; reproduce each in its real backend and
execution configuration; never rerun the entire suite to rediscover them, and never
skip, weaken, delete, fake, or silently bypass a failing test. Keep language
semantics valid while fixing: `;` comments, proof soundness, ABI behavior,
and optimizer correctness.

### Confirmed failures

- [ ] **UI engine replay:** `code/probes/ui/ui-smoke.nshape:ui_replay_engine`.
  Full-run artifact says engine/JIT and engine/VM each exited 1 after 25.850 s;
  fixture assertion is `engine native, JIT, and VM replay pass`. Earlier cold
  evidence reported a JIT startup segfault before frame markers. The completed
  run's JIT and VM logs now localize the failure before replay: NYIR verification
  rejects `_draw_frame` return instruction 403 in block `L19` because its
  producer is `f64` but the inferred return ABI is non-float/unknown. This is
  the same return-class family as `vector-f64.nshape`; fix shared return ABI
  inference/merge, then re-run the replay. Native route log has only dynamic
  type fallback warnings in the captured prefix, so inspect its final status
  separately. Require all routes to exit zero and produce identical expected
  state/frame streams. Preserve per-route deadlines and marker comparisons.

- [ ] **x86-64 `-O0`/`-O1` f64 buffer update miscompile:** with the native
  backend at `-O0` or `-O1`,

  ```ny
  use core
  use core.tbuf
  def B = f64buf_new(8)
  def C = f64buf_new(8)
  mut int k = 0
  while k < 8 {
     f64buf_store(B, k, float(k))
     f64buf_store(C, k, 0.0)
     k += 1
  }
  k = 0
  while k < 8 {
     f64buf_store(C, k, f64buf_load(C, k) + 2.0 * f64buf_load(B, k))
     k += 1
  }
  print(f64buf_load(C, 3))
  ```

  prints garbage (for example `6.36e-314`) instead of `6`; `-O2`/`-O3`, LLVM,
  and `NYTRIX_FORCE_STACK_FPR=1` are correct. The constant `2.0` is
  materialized in `xmm2` before the bounds-check `jge`, but its segment after
  the branch (`[48,55]`, `reload=1`) reloads `[rbp-0x178]`, which no path
  stores. Its definition never reaches `mach_float_end_segments` or the
  branch flush with a matching segment. Reproduces with the `object.c` from
  before the FPR loop-home replay, so it is pre-existing. Fix where the
  constant's definition segment and its post-branch segment disagree, then
  add `-O0`/`-O1` routes back to
  `code/intermediate/opt/loop-tail-passes.nshape`.

### Failures in the 2026-11-04 late full run

A cold `ny-test --with-stdlib` run (2,652 cases) failed 51. They reproduce in
isolation, also with the statement-lowering performance restore reverted and
with `NYTRIX_NO_LOOP_SUB_IV=1 NYTRIX_STRENGTH_EXPAND_MUL59=1`, so they are not
caused by the optimizer work; most coincide with concurrent edits to parsing,
lowering, and the bare backend. Each entry gives the first failing assertion.
Reproduce one fixture at a time with `build/release/ny-test --bin
build/release/ny --cold --with-stdlib etc/tests/<path>`.

#### Bool carriers and scalar ABI

- [ ] `code/abi/abi-conformance.nshape` — normalized true predicates retain the bool formal carrier
- [ ] `code/abi/calls/scalar-classes.nshape` — bool global store
- [ ] `code/intermediate/ny/logical-loop-branch.nshape` — assertion failed (no message)
- [ ] `code/probes/numeric/comma-float-carriers.nshape` — float coordinates survive erased list reads and reassignments

#### AST queries, quoting and serialization

- [ ] `code/ast/ast-binding-metadata.nshape` — quoted bindings retain their scope mark separately from outer bindings
- [ ] `code/ast/ast-code-queries.nshape` — runtime parse_code still accepts variable declarations
- [ ] `code/ast/ast-generated-origins.nshape` — identifier splice source origin points to its template argument
- [ ] `code/ast/ast-loop-invariant.nshape` — loop conditions and invariant syntax both substitute template arguments
- [ ] `code/ast/ast-numeric-serialization.nshape` — PanicError: err.type: get expects a string, bytes, list, tuple, dict, range, or vector, got int
- [ ] `code/ast/ast-string-serialization.nshape` — escaped literal and formatted-string AST records remain valid JSON
- [ ] `code/derive/dispatch/codegen-match.nshape` — SegmentationFault: signal 11
- [ ] `code/derive/representation/codegen-store-fields.nshape` — Ny field writer reports success only for known fields and pointers
- [ ] `lang/comptime/comptime-counted-strings.nshape` — template cloning preserves an untouched counted literal
- [ ] `lang/comptime/comptime.nshape` — parse_code accepts multiple complete declarations
- [ ] `lang/comptime/reflection/semantic-effect-artifact.nshape` — compiler reports a failed semantic artifact write

#### Bare AOT backend and CLI

- [ ] `code/intermediate/ny/bare/asm-artifacts.nshape` — bare AOT reports the output path and exact small-file size
- [ ] `code/intermediate/ny/bare/entry-name.nshape` — bare --entry-name controls the sectionless ELF entry and raw syscalls stay libc-free
- [ ] `code/intermediate/ny/bare/runtime-ops.nshape` — x86_64 runtime operations match VM output
- [ ] `code/intermediate/ny/bare/x86-typed-list-length.nshape` — bare x86-64 typed-list indexing and length work without libc or assembler; compile=0 execute=127 stdout=`` stderr=`` dynamic=
- [ ] `code/probes/cli/cli-argv-boundaries.nshape` — AOT inline arguments retain exact boundaries
- [ ] `code/probes/cli/cli-argv-hints.nshape` — warn-all reports a compiler-looking post-script option once
- [ ] `code/probes/cli/cli-bare-inspection.nshape` — --emit-asm=- must be honored or rejected by bare AOT (code=0, output=	.file	"nytrix"
- [ ] `code/probes/tools/decompiler-generated-c-elf.nshape` — retain function/data relocation symbols and types

#### Desugar tool

- [ ] `code/probes/tools/ny-desugar-budget.nshape` — missing diagnostic '64-container budget'; output: ny-desugar: 93942992071296
- [ ] `code/probes/tools/ny-desugar-parity.nshape` — error: --ny-desugar=ny pass failed for ny-desugar-parity.ny (exit 2)

#### UI, audio and graphics probes

- [ ] `code/probes/os/audio.nshape:sound_source_abi` — mono S16 mixer preserves samples in both output channels
- [ ] `code/probes/ui/fonts.nshape:font-text-codepoints` — ASCII and UTF-8 codepoints resolve
- [ ] `code/probes/ui/gltf.nshape:gltf_embedded_buffer` — a node without visibility overrides is visible
- [ ] `code/probes/ui/gltf.nshape:gltf_triangle_bounds` — scene minimum bounds match the triangle
- [ ] `code/probes/ui/ui-render-pixels.nshape` — software circle reaches framebuffer pixels
- [ ] `code/probes/ui/ui-smoke.nshape:ui_replay_animation` — animation native, JIT, and VM replay pass
- [ ] `code/probes/ui/ui-smoke.nshape:ui_replay_editor` — editor native, JIT, and VM replay pass
- [ ] `code/probes/ui/ui-smoke.nshape:ui_replay_engine` — engine native, JIT, and VM replay pass
- [ ] `code/probes/ui/ui-smoke.nshape:ui_replay_input` — input native, JIT, and VM replay pass
- [ ] `code/probes/ui/ui-smoke.nshape:ui_replay_monitor` — monitor native, JIT, and VM replay pass
- [ ] `code/probes/ui/ui-smoke.nshape:ui_replay_pong` — pong native, JIT, and VM replay pass
- [ ] `code/probes/ui/ui-smoke.nshape:ui_replay_synth` — synth native, JIT, and VM replay pass
- [ ] `code/probes/ui/ui-smoke.nshape:ui_replay_term` — term native, JIT, and VM replay pass
- [ ] `code/probes/ui/ui-snapshot-ownership.nshape` — later frames preserve an outstanding readback (frame 0: red=0, green=0)
- [ ] `code/probes/ui/window-headless-init.nshape:runtime_window_explicit_null_backend_init` — explicit null backend is available

#### Language semantics

- [ ] `lang/control-flow/control.nshape` — defers run LIFO at scope exit
- [ ] `lang/execution/defer.nshape` — deferred writes reach the enclosing local
- [ ] `lang/language/sugar.nshape` — while-def entered
- [ ] `lang/modules/attached-owner-identity.nshape` — right module keeps its attached method owner: 101
- [ ] `lang/modules/use/guard.nshape` — runtime __main() is true in direct file
- [ ] `lang/ownership/generated-runtime-shadow-differential.nshape` — generated inferred resource move 4 runtime shadow and strict checker disagree (code 0, accepted=true, expected=false): <inlin
- [ ] `lang/types/inference/type.nshape` — HM generic dict<str, int> parameter
- [ ] `lang/types/traits/enum-method-abi.nshape` — SegmentationFault: signal 11
- [ ] `lang/values/calloc-raw.nshape` — calloc-raw.ny:9:1:  [E2001] error: effect policy violation in 'allocate_dynamic': effects are unknown
- [ ] `lang/values/result.nshape` — typed Result error payload keeps its string type

#### F64 return ABI

- [ ] `code/intermediate/opt/vector-f64.nshape` — x86-64 `-O2` native-result-oracle route fails at emit/verify: `rt_main` returns an f64 loop result through an `i64-raw` return ABI
