<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":113,"summary":"Catch use-after-free, out-of-bounds, double free, leaks, and allocation failure in your own programs."} -->
# Safety

Nytrix does not make every program memory-safe by default. This page shows the
checks you can turn on, what each one catches, and what it costs. Pair it with
[Ownership](ownership.md), which covers the opt-in static checks.

Prerequisites: [Tour](tour.md) for functions and bindings.

## The safe heap

`--heap=safe` puts the allocations your program makes itself (`malloc`,
`zalloc`, `realloc`, and the raw heap) on guard pages. It uses virtual-memory
protection on both POSIX and Windows:

- Each block is rounded to 16-byte alignment and ends at an inaccessible
  page. An access beyond that rounded payload traps.
- Compiling with `--heap=safe` or `--mode=safe` also instruments Nytrix
  `load8`/`store8` accesses against the requested payload size. This catches
  accesses into alignment padding, negative offsets, and out-of-range
  interior-pointer accesses. Other raw access widths and foreign C accesses
  do not receive those byte-intrinsic checks.
- `free` makes the whole block inaccessible and keeps it quarantined, so a
  later read or write traps instead of silently using recycled memory.
- Freeing the same block twice is reported.
- Blocks that are never freed are listed when the program exits.

```ny
fn run() int {
   mut p = malloc(16)
   store8(p, 1, 15)   // last byte: fine
   store8(p, 2, 16)   // one past the end
   0
}
run()
```

```text
$ ny --heap=safe overflow.ny
Nytrix safe-heap: heap-buffer-overflow at 0x7f...000 (block 0x7f...ff0, 16 byte(s))
```

The process exits with status 134. The other messages are
`use-after-free`, `double free`, and, at exit with status unchanged,
`leak: N allocation(s), M byte(s) never freed`.

The runtime's own objects (strings, lists, dictionaries) keep the normal
allocator, so only blocks the program allocates pay the cost: every allocation
maps its own pages (several microseconds and at least two pages of address
space) and quarantined blocks are kept until 512 MiB or 8191 blocks are held.
Instrumented byte accesses add metadata lookup and synchronization;
interior pointers can require a registry scan.
Use it for tests and debugging, not for production runs.

`--heap=safe` is a compile-time choice. It also applies to executables you save
with `--output`. A program that was built without it can still be checked by
running it with `NYTRIX_SAFE_HEAP=1` in its environment; do not set that
variable for the compiler itself.
Runtime opt-in enables the guard-page allocator but does not retroactively
insert requested-size byte checks; recompile for that instrumentation.

`NYTRIX_SAFE_HEAP_TRACE=1` adds the faulting instruction address to each trap.

## Dynamic indices into stack and foreign buffers

For memory outside the safe heap, pass its byte extent directly to
`load8(address, index, extent)` or `store8(address, value, index, extent)`:

```ny
fn stack_byte(int index) int {
   mut int cell = 73
   def address = addr_of(cell)
   load8(address, index, 8)
}
```

An explicit nonnegative extent checks `0 <= index < extent` before either
access, including zero-sized buffers. Extents below `-1` are rejected.
The default `-1` retains the existing raw-access contract; it is not a checked
extent. Range failures panic and run registered defers. These checks work
under the safe profile and independently of the safe-heap allocator.

`assert_compile_range` remains a compile-time proof operation, not a runtime
test. `checked_fin(index, extent)` is an alternative when a bounded index
must cross an API boundary; it requires a positive extent and retains its
`Fin<extent>` type. Scalar index parameters are values, not reference-alias
roots; taking a scalar parameter's address still makes it an alias-analysis root.

A `requires` contract can prove a numeric index range at a raw-memory function
boundary when the caller discharges that precondition. This proves only the
index expression; it does not establish that the supplied extent matches the
actual allocation or that the pointer is still live.

The supplied extent must match the actual buffer. A checked access does not
discover a foreign allocation's size, prove a pointer is still live, or make
an unsafe C binding safe. The checked foreign-buffer regression uses
foreign `calloc`, observes writes through C `memcmp`, and frees the buffer
on both normal and failing checks. Its native, LLVM MCJIT, and LLVM ORC
matrix covers checked-index and direct-extent forms, negative indices, both
valid boundary bytes, the first invalid byte, and empty/negative extents.

## Allocation failure

`malloc`, `malloc_raw`, `calloc_raw`, `zalloc`, and `realloc` return `0` when
they cannot allocate. Check the result before using it. On a failed `realloc`,
the original block remains valid and must still be freed or retained:

```ny
mut buffer = malloc(4096)
if !buffer {
   print("out of memory")
   return 1
}
```

To prove those paths run, set `NYTRIX_FAIL_ALLOC_AFTER=N`: the (N+1)th and later
`malloc`, `zalloc`, raw-heap, and raw `realloc` allocation requests return `0`.
A failed `realloc` keeps its input block valid.

```text
$ NYTRIX_FAIL_ALLOC_AFTER=3 ny program.ny
```

For allocation-failure testing of a saved program, compile first and inject
only when running the executable. This keeps compiler/tool allocations out of
the program's allocation budget:

```text
$ ny --native-backend=llvm --output=app program.ny
$ NYTRIX_FAIL_ALLOC_AFTER=3 ./app
```

The null-return contract belongs to raw allocation APIs, not every
constructor. Dictionary and set constructors/growth reject capacities above
134,217,728 with a capacity-overflow panic before allocating; allocation
failures report an out-of-memory panic. They do not return an invalid object
or silently discard an insertion. Panic executes registered `defer` cleanup,
including when the panic is uncaught. `core.alloc.bump_new` also panics if its
initial buffer allocation fails; `bump_alloc` and `ctx_alloc` return `0` on
exhaustion. `core.bytes` panics on allocation failure, while `zalloc` returns
`0` like `malloc` because it is a raw buffer helper.

Native dictionary and set growth participate in the injected allocation
budget. Runtime-owned dictionary initialization and stdlib bootstrap caches
do not consume that budget. Use `with ptr` for owned raw allocations that
must be released on fallthrough, return, break, continue, or panic; a
registered `defer` provides explicit cleanup for other resources.

Internal fixed-size scratch buffers that have no failure path use the
`core` conventions `malloc_or_panic(n, what)`, `zalloc_or_panic(n, what)`, and
`realloc_or_panic(p, n, what)`. They panic with `out of memory: <what>`
instead of returning `0`, so the allocation failure is still reported and
registered `defer` cleanup runs. A failed `realloc_or_panic` leaves its input
block valid, matching `realloc`. These helpers are for implementation buffers
that are written immediately (kernel structs, hash padding, FFI scratch
space, window-system listener tables and other fixed-size platform glue);
public APIs that can report failure keep the raw `malloc`/`zalloc`
null-return contract. Platform bridges that allocate command/event scratch use
them so an OOM panic is reported instead of a null dereference inside native
code.

Every allocation path in the standard library and compiler follows one of
these contracts:

| API category | Failure contract |
| --- | --- |
| Raw `malloc`, `malloc_raw`, `calloc_raw`, `zalloc`, `realloc` | Return `0`. The caller checks before writing through the pointer; a failed `realloc` leaves the input block valid. |
| Dictionary and set constructors/growth | Panic with `out of memory: ...`; capacities above 134,217,728 panic as a capacity overflow before allocating. No invalid container is returned. |
| `core.alloc` bump/context helpers | `bump_new` panics when its initial buffer fails; `bump_alloc`, `ctx_alloc`, `ctx_realloc`, and `ctx_zalloc` return `0` on exhaustion or a missing realloc. |
| Internal fixed-size scratch (`*_or_panic`) | Panic with `out of memory: <what>` so registered `defer` cleanup runs. |
| Checked high-level fallbacks | `core.str` padding, slicing, joining, and hex formatting return `""`; `str_replace` keeps the input; `core.general.uuid` returns `""`; the Base64/Base32/Base16 encoders return `""` when their checked capacity arithmetic overflows. |

Allocation sizes are checked arithmetic, not just the allocation result:
mul/sub/add extents are validated with `core.math.checked` (or equivalent
bounds) before multiplying them into a byte count, so an overflowing element
count reports a failure instead of requesting an undersized block. This
applies to the standard library's encoded output, string, image, mesh, scene
merge, atlas, and platform buffer paths.

## Choosing a safety profile

The default reclamation policy is `--heap=manual`, with ownership checks
disabled. This preserves existing programs and adds no safety-profile checks
or inserted cleanup.
`--borrow-check` and `--ownership-check=strict` add compile-time analysis;
`--heap=raii` and `--mode=safe` also insert cleanup for supported resources,
including implicit release of provably local raw allocations (see
[Allocation failure](#allocation-failure)). Those profiles add analysis and
cleanup work; their runtime cost depends on the program and has not been
quantified here.

`--mode=safe` also enables the guard-page heap for user allocations, while
retaining its RC/RAII cleanup policy. `--heap=safe` enables the guard-page heap
without selecting strict ownership analysis. Both map and quarantine user
allocations, with substantial per-allocation overhead and retained address
space as described above. Neither is a zero-cost production profile.

## AddressSanitizer

`ny --sanitize=address file.ny` instruments emitted user code through LLVM
and links an AddressSanitizer-instrumented runtime. The command builds and
runs an executable; `--output=app` saves that executable instead. ASan reports
invalid accesses with a stack trace and terminates the program. Its shadow
memory, redzones, and access checks add runtime and memory overhead.
The `sanitizer-user-access` regression covers heap, stack, and global
buffer overflows in emitted Nytrix code.

Sanitizer requests require the LLVM backend. Explicit requests for the owned
native encoder are rejected rather than silently producing an uninstrumented
program. Instrumentation failure is a compilation error.

## UndefinedBehaviorSanitizer

`ny --sanitize=ub file.ny` (also `--sanitize=undefined`) checks emitted signed
64-bit addition, subtraction, multiplication, division, remainder, unary
negation, and null memory accesses. Checked arithmetic calls the instrumented
C runtime; null checks precede LLVM optimization so a constant null access
cannot disappear before instrumentation. Reports name the failing operation
and include generated Nytrix frames. Unsigned wrapping arithmetic is not
treated as signed overflow.

The regression runs saved executables at `-O0` and `-O2`, checking defined
boundary operations and each overflow/null mutation. Like the other lanes,
this requires a compatible host sanitizer runtime. It does not make arbitrary
foreign dependencies instrumented.

## MemorySanitizer and ThreadSanitizer

`ny --sanitize=memory file.ny` instruments emitted LLVM code and the C runtime
with MemorySanitizer. It detects use of uninitialized raw allocation bytes;
the initialization regression checks both an unwritten read and a byte written
before use. The safe heap's `0xA5` fill is initialized memory, not a substitute
for this check.

`ny --sanitize=thread file.ny` instruments emitted code and the C runtime with
ThreadSanitizer. Its regression distinguishes unsynchronized byte writes in a
worker and parent from a parent read ordered after `thread_join`. The reported
race includes the generated worker and parent functions.

Both lanes link PIE executables for sanitizer shadow-memory compatibility.
They require a supported host sanitizer runtime and compatible dependencies;
uninstrumented foreign code can hide accesses. Instrumented execution is not
a proof of initialization or race freedom for every path.

## Seeded memory fuzzing

Run the user-memory lane independently:

```sh
./make fuzz sanitizers run --user-only
```

It builds saved executables for native safe heap, LLVM safe heap, and emitted
AddressSanitizer. Four deterministic seeds exercise changing allocation sizes,
byte writes/reads, C `memchr` observations, and normal release. Mutated extents
must produce a named heap-buffer-overflow report rather than merely a nonzero
exit. The valid runs must complete without safe-heap leaks or ASan errors.
Saved ASan executables run with leak detection and abort-on-error enabled.

The same command also runs emitted UBSan arithmetic/null mutations, MSan
initialized/uninitialized reads, and TSan join-ordered/racing writes. Each
lane includes valid executions and requires a named sanitizer report for its
invalid execution; a generic crash is not success.

Without `--user-only`, the same command also runs the compiler's ASan and
UBSan lanes. `--dry-run` records the selected commands without running them.
This is a bounded reproducible campaign, not exhaustive fuzz coverage.

## What this does not cover

- The safe-heap registry does not check pointers it did not allocate (FFI
  buffers, the stack, runtime objects). Such raw byte accesses need an
  explicit checked index and a correct extent, or emitted-code sanitizer
  coverage.
- The safe heap alone does not detect uninitialized reads; raw-heap blocks
  are filled with `0xA5`. Use MemorySanitizer for uninitialized-value checks.
- Strict ownership rejects raw-pointer use after a local free or an explicit
  `@releases` call, including copied and interior aliases in supported local
  flows. `@returns_owned` factory results retain an ownership identity, and
  `@consumes` invalidates the transferred raw owner's aliases. Call contracts
  apply to every annotated parameter, not just the first. This is not general
  interprocedural inference: unknown calls, dynamic aggregate provenance, and
  unsupported control-flow shapes do not receive a safety certificate.
- Implicit RAII covers raw owners, not arbitrary handle types.
  `--heap=raii` and `--mode=safe` release a local raw allocation at the end of
  the block that holds it (fallthrough, return, break, continue, and panic).
  A declaration qualifies only when it binds one name to
  `malloc`/`zalloc`/`calloc_raw`/`__malloc` and every later use in that block
  is a raw access through a known intrinsic (`load*`/`store*`, `memset`,
  `memcpy`, `__copy_mem`, `ptr_add`/`ptr_sub`), an index into the pointer, or
  a nil test. Returning the pointer, storing or passing it anywhere, aliasing
  it into another binding, capturing it in a closure, taking its address,
  exposing it to a `defer`, or freeing it explicitly all keep manual
  ownership, so an escaping owner is never released at scope exit. File,
  socket, and other handle types still need `with`, an explicit `close`/`free`,
  or a registered `defer`; stdlib modules keep their own explicit contracts.
- `requires`/`@proves` contracts are checked for their numeric or boolean
expression, not for allocation size. A `requires index < 8` on a raw-memory
function does not prove that the caller's extent matches the real allocation,
and no contract form proves release-once for an allocation. Strict ownership
tracking rejects supported use-after-free, double-free, and alias shapes, but
it is not an extent or lifetime theorem over unknown calls.
- Worker threads release their own small-string arena and intern table when
they exit; strings created on a worker are not a shared, thread-safe pool.
Copy a worker result into a caller-owned value before sharing it across
threads.
- `./make asan` sanitizes the compiler itself; it is distinct from
  `ny --sanitize=address`, which instruments the emitted program.

## Related

- [Runtime and heap policy](../spec/runtime.md)
- [Status and execution routes](status.md)
