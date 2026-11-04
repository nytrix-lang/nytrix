<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"learn","order":150,"summary":"Compiler diagnostics, pass isolation, runtime failures, and filtered instrumentation."} -->
# Compiler debugging

Start with one failing source file or fixture and retain its command, route,
flags, expected output, and first useful diagnostic. A trace explains a
compiler decision; it does not replace an executed correctness check.

Prerequisites: an in-tree compiler and a minimal reproducer.

## Command lookup

Run these from the repository root with the in-tree compiler. `case.ny` denotes
the reduced reproducer, not an included example file.

| Question | Command or control |
| --- | --- |
| What source shape was parsed? | `ny --dump-ast case.ny` |
| What tokens came from the source file? | `ny --dump-tokens case.ny` |
| What types were established? | `ny --dump-ast=typed case.ny` |
| Can the tree be narrowed to one declaration? | `--dump-ast=typed,filter=my_function` |
| Why did typing or trait selection fail? | `ny -vvvvvv --unify-trace --trait-resolve case.ny` |
| Which pass grew or changed IR? | `ny --native-only --inspect case.ny` |
| How do I inspect optimized IR? | `ny --inspect[=program|runtime] case.ny` |
| Where does IR first become invalid? | `--native-only --nyr-check=verify,rollback` |
| Does native execution disagree with the VM? | `--native-only --nyr-check=oracle` |
| Which pass introduces a native disagreement? | `--native-only --nyr-check=oracle-per-pass` |
| Which pass should be isolated? | `--nyr-pass=disable:NAME`, `--nyr-pass=stop-after:NAME`, or `--nyr-check=bisect` |
| Can I keep NYR bytecode and inspect LLVM IR from one compile? | `--native-only --emit=ir=build/program.ll,nyr=build/program.nyr --run=none case.ny` |
| Where are register pressure and frame costs? | `--native-only --abi --stack-frame --regalloc-heat` |
| What object metadata was emitted? | `--native-only --elf-dump` on an ELF target |
| How much work does the compiler do? | `-time`, `--nyr-dump=stats`, and a compile-only profile |
| Where does generated code spend time? | `--perf=compact`, `--perf=verbose`, or a validated execution-only profile |

`--native-only` runs shared semantic checks before native lowering; it does
not bypass type or trait validation. Its supported diagnostics and ABI path
still differ from LLVM. Use the failing route first and compare routes only
when investigating a disagreement. The [pipeline reference](../spec/pipeline.md)
describes stage ordering and cache behavior.

`--inspect` prints optimized program NYIR and pass statistics;
`--inspect=runtime` includes the runtime. The old overlapping `--dump` and
`--inspect-runtime` spellings are no longer accepted.

`--emit=ir=PATH` lowers the in-memory NYIR module to LLVM IR. Pair it with
`--emit=nyr=PATH` to retain the versioned NYR bundle from the same
compile. `--nyr-run-bin=PATH` loads that bundle into the diagnostic NYR VM;
it does not convert a saved bundle back to LLVM IR. The bundle contains
function IR, not a complete serialized LLVM module with source global data.

At deep verbosity, symbol resolution emits one compact path/result record per
lookup. Set `NYTRIX_TRACE_RESOLVE_VERBOSE=1` to include candidate attempts,
alias decisions, and cache details.

## AST inspection

`--dump-ast` prints the expanded/desugared source tree and stops before type
checking, code generation, or execution. Nodes show readable kinds, source
file/line/column, names, operators, literal values, and child roles. Terminal
colors follow Nytrix color detection and `--color`; redirected output stays
plain by default. Kinds are cyan, names green, types blue, operators yellow,
literals magenta, and source positions muted. Parameters, defaults, fields,
enum payloads, attributes, and function-expression bodies appear beneath their
owners.
Generated nodes include expansion origins when available. Strings are escaped,
and addresses are omitted so repeated dumps are comparable.

`--dump-ast=typed` runs shared type/trait validation first, then prints available
inferred types, resolved representations, and callees. It also stops without
executing the program, on both ordinary and `--native-only` routes. A failed
check returns nonzero instead of printing a successful typed tree.

`filter=NAME` selects source declarations by case-sensitive name substring,
including members of modules, layouts, structs, implementations, and traits.
Use it alone or after `typed`; keep it last. A missing match produces only the
program heading. Dumps inspect the requested source, not every imported library.
`--dump-tokens` excludes compiler-preloaded library source.
These inspection options also apply to inline `-c` input, for example
`ny --dump-ast -c 'print("hello")'`; the AST is printed and the code is not run.

```bash
ny --dump-ast case.ny
ny --dump-ast=typed,filter=my_function case.ny
ny --native-only --dump-ast=typed case.ny
```

Use `--expand` for specialization/desugaring explanations,
`--expand-json` for its raw AST appendix, or `--stop-after=STAGE` with
`--emit-artifact=PATH` for versioned machine-readable stage artifacts.
AST inspection is not a timing run.

## Verbosity and filtering

| Flag | Additional information |
| --- | --- |
| `-v` | Phase and pass summaries |
| `-vv` | IR changes, CFG/value summaries, effects, verification, rollback |
| `-vvv` | Instruction decisions, parsing, lowering, ABI, escape information |
| `-vvvv` | Full IR snapshots, detailed typing, allocation, object/frame data |
| `-vvvvv` | Deep optimizer detail for source code; imported-library internals stay hidden |
| `-vvvvvv` | Includes imported-library debug, phase timing, process resources, and failure artifacts |

Level five keeps imported-library internals out of the trace. Level six
includes them and can produce very large output; filter the relevant function
or category before capturing a large program:

```bash
build/release/ny-full -vvvvvv --dbg-filter=func:my_function case.ny
build/release/ny-full -vvvvvv --dbg-filter=cat:PASS,LOWER case.ny
build/release/ny-full -vvvvvv --delta-only case.ny
build/release/ny-full -vvvvvv --nyr-track-value=42 case.ny
build/release/ny-full -vvvvvv --dbg-format=json case.ny
```

Structured diagnostic records go to stderr; program output remains on stdout.
`--delta-only` suppresses unchanged IR. `--nyr-track-value=N` follows one SSA
value. `--callgraph=DEPTH` changes the bounded graph depth; the deep profile
caps its displayed edges. Process CPU time and peak RSS describe the compiler
process, including in-process execution, not individual function costs.
The deep trace reports enabled/disabled trace-tag counts by default. Set
`NYTRIX_TRACE_CONFIG_VERBOSE=1` to include the tag names.
Symbol-resolution records use compact key-value fields for lookup steps,
cache state, aliases, and final results.

Explicit `NY_TRACE_<FEATURE>=0` settings can disable a centralized trace.
A master switch bypasses per-category defaults: `NYTRIX_TRACE=all` enables
every known trace category, and `NYTRIX_TRACE_DEEP=1` (or `NY_TRACE_DEEP=1`)
forces all traces on regardless of the default heuristics.
The high-volume import alias trace remains opt-in with
`NYTRIX_TRACE_IMPORTS=1`. `NYTRIX_TRACE_FILTER` narrows trace categories.
The in-tree executable's `--help` is the authority for build-specific options.

## Miscompilation and verifier failures

1. Reproduce the result with the original optimization and backend settings.
2. Enable verification and inspect the earliest invalid or behavior-changing
   pass, not just final IR.
3. Compare the pass input, accepted/rejected candidates, output, and verifier
   result. Use pass-disable or stop-after controls to isolate the candidate.
4. Exercise the corrected source on the same route. Include nearby boundary,
   alias, trap, effect, and control-flow cases relevant to the transform.

A rollback preserves the last valid IR for diagnosis; it does not establish
that the rejected transform was semantically correct. `--nyr-fingerprint` and
`--nyr-flaky-check` help investigate unexpected or nondeterministic changes.

## Types, traits, and effects

Read the first source-located failure. Follow its concrete constraints,
substitutions, selected implementation, and containing function's effect
summary. A value becoming unknown earlier in the pipeline can explain a later
conversion or ABI failure. Use `--explain=types`, `--explain=signatures`,
`--explain=fallbacks`, or `--explain=any` for a focused view.

An inconclusive proof, callback row, or ownership shape is not established
safety. See [Types](../spec/types.md), [Functions](../spec/functions.md), and
[Ownership](../spec/ownership.md) for their distinct contracts.

## Native lowering and register allocation

Follow one value through NYIR representation, machine selection, ABI argument
classification, allocated registers/stack homes, and final relocations. Typed
integers and dynamic tagged values are different representations; a pointer
must not accidentally cross an integer tagging boundary.

Select controls for the failing stage:

- `NY_DUMP_OBJ_NYIR=1`: object-path NYIR.
- `NY_DUMP_MACH=1`: machine lowering and form.
- `NY_KEEP_ASM=1`: preserve temporary assembly.
- `NY_DUMP_REGALLOC=FUNCTION`: segment and frame/home information for a selected
  function; verify the exact supported filter spelling in the current build.
- `NY_DUMP_REGALLOC_CONFLICTS=1`: loop-edge retained/evicted segments, blocks,
  physical colors, and reasons.
- `NY_TRACE_X64_SIB_FILTER=FUNCTION`: x86-64 indexed-address folding decisions,
  used with deep tracing and a matching function filter.

Disassemble the object or executable used by the measurement. The readable
text-assembly emitter and linked object encoder are distinct paths; a frame
or load in one is not automatically present in the other.

For editor debugging, compile with `-g` and use the same target and flags as
the failing run. Native x86-64 source breakpoints depend on instruction
offsets surviving machine encoding into the DWARF line table; a GDB DAP
breakpoint reported as pending means the requested source line has no
executable row in that table. To inspect LLVM debug locations independently,
emit IR with `ny --emit=ir=build/program.ll -g --run=none case.ny`, then run
`opt -passes=verify -disable-output build/program.ll`.

## Crashes, hangs, and runtime failures

Separate compile time from execution time before classifying a timeout.
Retain exit status, stderr, source stack, and output. For a runtime failure,
use `-trace`; for a compiler crash, run `ny-full` under the debugger so the
launcher does not obscure the failing process:

```bash
gdb -q -batch -ex run -ex 'bt 20' --args \
  build/release/ny-full --no-progress case.ny
```

Failure bundles from `--dump-on-error` and the pipeline's deeper dump options
contain only artifacts produced before failure. A parse failure cannot supply
valid post-optimization IR. For memory faults, use the separate sanitizer builds
in [Contributing](../CONTRIBUTE.md#sanitizers-and-memory-tools).

If file execution and the REPL differ, compare parsing, expansion, typing,
resolution, and execution before changing the REPL display layer. For terminal
failures, retain raw output and terminal state; for graphics, see
[UI verification](ui.md#verification).

## Adding useful diagnostics

Add a trace at the decision you need to inspect. Use `ny_dbg_*`
or the centralized `ny_trace_*` gate and include inputs, selected/rejected
operation, reason, function/pass, and source or IR position where available.
Test the gate before formatting strings, walking IR, allocating, or collecting
expensive statistics. Deduplicate repeated events and summarize suppression.
Keep text and JSON fields equivalent and escape source text through the logger.

Build and run the smallest reproducer with the trace enabled and disabled.
The enabled record must answer the missing question; the disabled path must
remain quiet. Remove one-off print probes. Record the operands and rule that
ran; a plausible explanation or profile percentage is not sufficient.

Next:

- [Testing](testing.md) — reproduce a behavior with a focused fixture.
- [Pipeline](../spec/pipeline.md) — map a trace to its compiler stage.
- [Architecture](architecture.md) — find the owning subsystem.
