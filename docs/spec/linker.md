<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"spec","order":46,"summary":"Current linker support, design boundaries, and requirements for broader target coverage."} -->
# Linking

This page distinguishes the implemented linker API from the proposed
general-purpose design. The implementation section describes what callers can
use today. The remaining sections describe design goals and acceptance criteria;
they are not promises of current support.

## Current implementation

The public `nyr_link_elf64_executable` and
`nyr_link_elf64_executable_objects` APIs provide an initial Linux x86-64
backend slice. They link ELF objects and lazily extract static archive members
into a static executable, resolving global and weak definitions across selected
objects. Selected members are parsed directly from the loaded archive buffer,
without temporary object files. Unresolved strong symbols and unsupported
relocations are rejected.

This slice does not implement shared libraries or the shared cross-format link
graph, and it does not replace the compiler driver's normal application link.
Keep the platform linker as the default for general builds until runtime
objects, shared libraries, startup rules, and user link options work through the
in-process path and pass the cross-target conformance suite.

## API and architecture goals

`nyr_emit_object_for_target` remains a relocatable-object API. Linking has a
separate request API so a frontend can select an executable, shared library,
static library, or relocatable output and receive structured errors.

Keep link policy independent from object formats. A shared layer should own
input discovery, symbol precedence, archive extraction, dead-strip reachability,
diagnostics, deterministic ordering, and output transactions. Format backends
should own object parsing, relocation semantics, section layout, and final-file
serialization. Target ABI rules must be explicit target data rather than
inferred from the host running the linker.

The request model should describe inputs and intent rather than expose a raw
linker command line. Inputs include relocatable objects, static archives,
shared-library interfaces, and runtime objects. Options include entry points,
export visibility, search paths, whole-archive groups, dead stripping, debug
data, PIE/static mode, runtime paths, deployment targets, and target-specific
ABI choices. Unsupported options must produce explicit diagnostics; they must
never be silently ignored.

The shared symbol graph should model local, global, and weak binding; visibility;
common symbols; archive extraction; duplicate-definition precedence;
undefined-symbol reporting; and versioned dynamic symbols where supported.
Output policy must account for TLS, unwind and exception metadata,
constructors/destructors, stack protection, build identifiers, and debug
sections. Each format backend must declare the features it implements and
rejects.

ELF, Mach-O, COFF/PE, and Wasm can share graph and policy code, but their
relocation, loader, and metadata rules remain format-specific. Diagnostics for
unsupported features should identify the input path, symbol or relocation, and
target.

## Implementation sequence

1. Turn the ELF slice into a reusable link graph for multiple objects, with
   deterministic strong/weak resolution and clear unresolved/duplicate-symbol
   diagnostics.
2. Add archive indexing and lazy extraction, then link Nytrix runtime objects
   and ordinary C objects through the same graph.
3. Add ELF executable and shared-library policies: dynamic symbols and versions,
   PLT/GOT, TLS, PIE, startup objects, unwind data, constructors, and debug
   information. Add static archive and relocatable output modes.
4. Add independent Mach-O, COFF/PE, and Wasm backends with relocation tests and
   native execution or format validation.
5. Integrate the driver through capability checks. Select the in-process path
   only when it implements the full request; retain the compatibility path for
   other requests until the conformance corpus is complete.
6. Profile representative small and large links. Parallelize measured hot
   phases, then add incremental reuse after output determinism and cache
   invalidation are tested.

## Performance

Measure cold and warm links across small programs, large object sets, large
archives, debug-heavy outputs, and programs with many shared libraries. Track
wall time, peak RSS, bytes read and written, and worker utilization. Compare
equivalent outputs and options with the platform linker. No speed claim is
valid without a reproducible corpus and correctness checks.

Use memory-mapped or otherwise low-copy inputs, compact string and symbol
tables, and per-link allocation arenas where measurements support them. Parallel
parsing, archive indexing, independent relocation, and output writing must
preserve deterministic symbol selection and diagnostics. Incremental linking is
a later layer: cache validated input identities and dependency edges so
unchanged inputs can skip discovery and relocation.

## Acceptance criteria

Do not describe the linker as general-purpose or select it unconditionally
until each advertised target and output mode meets these criteria:

- A shared request and diagnostic API with explicit target triples and no
  host-target inference.
- Relocatable objects, static archives, static and dynamic executables, and
  shared libraries, including normal startup objects and compiler runtimes.
- The platform's symbol rules, relocation set, TLS model, unwind and exception
  metadata, initialization arrays, PIE rules, and dynamic-loader metadata.
  Unsupported constructs are rejected before output is written.
- Archive groups, whole-archive behavior, library search, weak/common symbols,
  symbol visibility/versioning, and deterministic duplicate handling.
- Atomic output replacement, bounded resource use, malformed-input fuzzing,
  conformance tests, and executable tests on each target.
- Cold/warm performance and peak-RSS measurements on a published corpus.
  Memory mapping and parallel work are used where profiles show a benefit, with
  a serial path for small links where worker setup costs more.

Acceptance is per capability: ELF x86-64 executable support does not imply ELF
shared-library support or support for Mach-O, COFF/PE, and Wasm. The driver
should select the in-process linker only when the entire request is inside the
tested capability set.
