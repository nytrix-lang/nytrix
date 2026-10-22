<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":30,"summary":"Ownership, cleanup, receivers, asynchronous behavior, and the runtime guarantees programs rely on."} -->
# Runtime

Runtime behavior covers execution modes, memory boundaries, ownership,
concurrency, cleanup, and effect metadata.

## Execution modes

| Mode | Command | Meaning |
| --- | --- | --- |
| REPL | `ny` | Interactive evaluation, or batch stdin when piped. |
| JIT | `ny file.ny` | Compile and run through the JIT path. |
| Inline | `ny -c 'code'` | Compile and run inline source. |
| Inline REPL | `ny -ic 'code'`, `ny -ci 'code'` | Compile inline source, then continue in the REPL. |
| REPL batch | `ny --repl < file.ny` | Compile stdin source once through the REPL batch path. |
| Native run | `ny -run file.ny` | Build a temporary executable and run it. |
| Native output | `ny -o app file.ny` | Emit a native executable. |
| Explicit REPL | `ny -i`, `ny --interactive`, `ny --plain-repl` | Interactive evaluation. |

## Safety profile

`--safe-mode` gives Nytrix its compile-time safety profile. It promotes
ownership diagnostics to errors and adds RC/RAII cleanup, strict effect/alias
policy, and stricter raw-memory diagnostics.

Ordinary compilation leaves ownership/provenance analysis disabled by default.
`--borrow-check` enables advisory diagnostics without changing heap policy or
inserting cleanup. `--ownership` enables ownership tracking and RAII cleanup;
`--ownership-strict` promotes supported ownership diagnostics to errors.
`--safe-mode` enables strict ownership checks and RC/RAII cleanup together
with strict effect/alias policy and tracked raw-memory diagnostics.

```bash
ny --safe-mode file.ny
ny --mode=safe file.ny
```

In this profile, code scopes owned raw allocations with `with ptr name` or
returns them through an ownership contract. Raw memory loads and stores against
a compiler-tracked allocation require a proven byte range:

```ny
use core

with ptr p = malloc(8){
   def int i = 3
   assert_compile_range(i, 0, 7, "byte index")
   store8(p, 65, i)
   assert(load8(p, i) == 65, "checked load")
}
```

If the compiler cannot prove the index range, or proves that `index + width`
exceeds the allocation size, compilation fails.

## Managed and native boundaries

The runtime manages ordinary values. Raw pointers, handles, layouts, and FFI
strings cross into native memory and native lifetime rules.

## Runtime global registry

`globals()` returns the runtime registry installed by `set_globals()`. It is
not a reflection of lexical scope, imported modules, or all compiled
functions. The registry is empty until a caller installs a dictionary, and
functions are not exported into it automatically. Code that looks up a
callable by name must handle a missing entry or install the entry explicitly.
`set_globals()` replaces the registry pointer; it does not alter name
resolution in Ny source.

## Runtime bridge representations

Runtime entry points have explicit tagged or raw variants where callers use
different machine representations. The C names below describe the boundary;
source-level intrinsics can be routed to a backend-specific variant during
lowering.

| Entry point | Arguments and result |
| --- | --- |
| `__list_len` / `rt_list_len` | Returns a tagged Ny integer. Native NYIR sequence operations use `rt_tbuf_len_raw` and receive a raw count. |
| `__read_off` / `__write_off` | Legacy tagged entry points decode the integer descriptor, byte count, and offset, resolve the buffer pointer, and return a tagged integer result (including errors). Native lowering may route typed calls to the corresponding `_raw` entry, which takes and returns raw integers. |
| `__syscall` / `rt_syscall` | Takes seven Ny dynamic values and returns a tagged Ny integer on every platform. Native NYIR lowers this call to `rt_syscall_raw`, which takes machine-word arguments, preserves addresses that are already readable, and decodes an encoded native pointer handle when the encoded word is unreadable but its payload is readable. Linux denies `mprotect`, `clone`, `fork`, and `vfork`; unsupported hosts return tagged `-1`. |
| `__execve` / `rt_execve` | Decodes the path's tagged/native-handle representation, validates each managed or native list/tuple vector entry as a string, and builds temporary null-terminated C vectors for the OS call. |
| `rt_call0_ptr` / `rt_call1_ptr` | Call native FFI functions with pointer return values and encode non-null results as native handles. These are pointer-return bridges; closure callbacks use the `rt_call*` callback family instead. |
| `__proof_cert_digest` / `__proof_cert_check` | Accept registered Ny strings as borrowed, read-only inputs; ownership stays with the caller. The checker requires tagged integer digest and budget arguments and returns a tagged boolean. |

Dynamic callback values are interned opaque IDs. Their bool, tagged-argument,
and raw-result flags occupy separate value bits; code-address bits are not
reused for callback metadata. The runtime does not own the callback code, so
its module must remain loaded while a callback value can be invoked. Captured
closures retain their separate heap closure and environment representation.

The proof checker validates the certificate digest before parsing, rejects
non-canonical encodings, and applies node, variable-metadata, depth, step, and
working-set limits. Its ABI is exercised by
`etc/tests/lang/types/proofs/proof-runtime-abi.nshape` on the NYIR VM,
native NYIR, and legacy LLVM paths.

The compatibility `rt_list_len` implementation can read both legacy managed
list headers and native tbuf headers, but its result stays tagged. Code that
stores a raw count must use a raw length bridge and must not infer the result
representation from the list layout.

## Heap policy

The default heap path uses the native runtime allocator for runtime-managed
Nytrix objects. Standard library constructors such as strings, lists, dicts,
sets, tuples, and ordinary result values return managed Nytrix objects. Raw
buffers, external handles, FFI pointers, and memory returned by native APIs
still follow the owning API's explicit native ownership rules.

Choose one reclamation policy for a compilation:

| Policy | Option | Reclamation model |
| --- | --- | --- |
| Manual | `--heap=manual` | The program or owning API is responsible for cleanup. |
| RAII | `--heap=raii` | Inserts cleanup at supported scope exits; recommended for resource handles. |
| Reference counting | `--heap=rc` | Tracked ownership uses reference-counted cleanup. |
| Tracing GC (optional) | `--heap=gc` | Reclaims supported managed object graphs. |

Tracing GC is optional and not recommended for resource management. Prefer
ownership contracts and RAII for resources. Cleanup support is partial; see
[implementation status](../learn/status.md#feature-boundaries).
`--safe-mode` enables strict ownership and RC/RAII cleanup without tracing GC.

Enable it with `-gc` or `--heap=gc`.

```bash
ny -gc file.ny
NYTRIX_GC_NURSERY_SIZE=64M NYTRIX_GC_TENURED_SIZE=512M ny --heap=gc file.ny
```

GC tuning variables are `NYTRIX_GC_NURSERY_SIZE`,
`NYTRIX_GC_TENURED_SIZE`, and `NYTRIX_GC_LOS_THRESHOLD` (sizes accept bytes,
`K`, `M`, or `G`).

### Collector mechanics

GC mode adds a 16-byte collector header before the ordinary prefix. It moves
supported managed objects; large objects are tracked without moving.

GC does not manage native handles or raw buffers. Use the owning API,
`with`, `release`, or `forget`; do not retain raw addresses across a GC point.

## Ownership

Declare ownership contracts with attributes. Compiler and runtime modes check
them. Resource APIs define whether a value is borrowed, owned, released, or
intentionally forgotten.

```text
def b = borrow(a)
def c = &a
def o = own(value)
release(o)
forget(o)
```

| Syntax | Meaning | Use |
| --- | --- | --- |
| `borrow(expr)` | Explicit borrow. | Named ownership boundary. |
| `&expr` | Equivalent borrow shorthand. | Compact local call. |
| `own(expr)` | Owned value. | Transfer or managed cleanup boundary. |

`release` consumes and drops an owned value. `forget` consumes an owned value
without dropping it.

Ownership diagnostics and cleanup are explicit options:

```bash
ny --borrow-check file.ny          # advisory diagnostics, no cleanup changes
ny --ownership-strict file.ny      # diagnostics become errors
ny --ownership file.ny             # RAII cleanup plus diagnostics
ny --safe-mode file.ny             # strict safety profile
```

`--borrow-check` enables the advisory pass. It reports
moves, releases, borrow escapes, use after move, and double release without
forcing RAII cleanup. `--ownership-strict` promotes these diagnostics to
errors and enables stricter tracking such as rejecting implicit borrows from
mutable slots. `--ownership` (alias for `--heap=raii`) enables automatic
runtime cleanup in addition to the analysis.

Use `--no-borrow-check` only for a legacy migration boundary. The compiler
still parses ownership attributes and keeps them as declaration metadata, but
does not issue advisory ownership diagnostics for that invocation.

Ownership function contracts are:

| Attribute | Meaning |
| --- | --- |
| `@borrows(x)` | The function may borrow parameter `x`. |
| `@returns_borrow(x)` | The return value is a borrow tied to parameter `x`. |
| `@returns_owned` | The return value transfers ownership to the caller. |
| `@consumes(x)` | The function consumes ownership of parameter `x`. |
| `@mutates(x)` | The function mutates parameter `x`. |
| `@releases(x)` | The function releases parameter `x`. |
| `@forgets(x)` | The function forgets parameter `x` without dropping it. |

## Receiver aliasing

When a function receives a dictionary and calls `set`, it changes the caller's
object. Choose one of two conventions for a public API and do not mix them:

- In-place mutator: mutate and return the same object.
- Functional mutator: clone, mutate the clone, and return it.

```ny
use core

; In-place: mutates caller's dict, returns it
fn add_name(any input, str name) any {
   input.set("name", name)
   input
}

; Functional: returns a new dict, caller's dict unchanged
fn with_name(any input, str name) any {
   mut out = clone(input)
   out.set("name", name)
   out
}
```

## Scoped cleanup

`defer` and `with` provide cleanup. `with` uses the resource spelling
`with Type name = value { ... }`.

```text
defer { cleanup() }
with Resource r = open_resource() { use(r) }
```

`defer` runs its body when the current scope exits, including early returns and
panic unwinding; multiple defers run in last-in-first-out order. `with` runs
cleanup when the body falls through, returns, or unwinds. Use `defer` for
several unrelated cleanups in one function and `with` for one resource whose
scope should be visibly bounded.

## Concurrency

The runtime and standard library include stackless async tasks, OS threads,
atomics, queues, channels, and network async helpers. Shared state uses
synchronization APIs from `core` and `core.os`.

### Async tasks

`async` starts a stackless task and `await` waits for its value:

```ny
use core

use core.os.async (async, await)

fn plus_one(x){ x + 1 }

def h = async plus_one(41)
assert(await h == 42, "async result")
```

The callable form is also supported: `async(fn_value, arg...)` and
`await(handle)`. `await_all(handles)` waits for a list of handles.
`future(fn_value, arg...)` and `Future(fn_value, arg...)` are compatibility
constructors for joinable async handles. `sleep_ms(ms)` and `yield_now()` are
async scheduling helpers in `core.os.async`.

Async socket helpers return awaitable handles for connect, accept, read, write,
and read-until operations.

On the browser target, `core.os.time.msleep(ms)` uses the runner's Asyncify
bridge to yield to the browser event loop and resume the suspended Wasm stack
after the delay. It does not block the browser thread. The fixture
`etc/tests/lang/interop/web/time.nshape:test_time_sleep` verifies this behavior.

### Threads and mutexes

`core.os.thread` spawns OS threads. `thread_spawn(fn_value, arg)` joins with
`thread_join(handle)`. `thread_spawn_call(fn_value, args)` passes a list of
arguments. Protect shared mutable state with `mutex_new`, `mutex_lock`, and
`mutex_unlock`; free the mutex with `mutex_free`.

Native thread and async callback entry points carry callback representation and
result representation as separate ABI arguments. Result kinds distinguish raw
integer scalars, already-tagged dynamic values, and raw booleans. The runtime
converts raw results to canonical dynamic values before `thread_join` or
`await` exposes them; callback argument counts contain only the argument count.
The callback ABI rejects non-integer flags, raw-function flags outside `0` or
`1`, and unknown result kinds before starting the callback.

```ny
use core
use core.os.thread

fn worker(any arg) any { arg }

def handle = thread_spawn(worker, "ok")
assert_eq(thread_join(handle), "ok", "thread")
```

`thread.spawn` is not exported.

### Channels

Cooperative channels move values between tasks. `channel(capacity)` creates a
channel; `capacity=0` means unbounded. `chan_send` and `chan_recv` are the
blocking forms, `chan_try_send` and `chan_try_recv` are nonblocking, and
`chan_close`, `chan_closed`, and `chan_len` manage lifecycle and capacity.

```ny
use core

def ch = channel(4)
assert(chan_send(ch, "ny"), "send")
assert_eq(chan_recv(ch), "ny", "recv")
```

### Queues

`queue(xs)` builds a FIFO queue value. `queue_push` appends and returns the
queue; `queue_pop` removes and returns the front (or a default); `queue_try_pop`
returns a result dict; `queue_peek`, `queue_len`, `queue_empty`, and
`queue_clear` cover inspection and reset.

### Shared-memory model

Nytrix uses a data-race-free shared-memory contract. Two threads must not access
the same mutable location concurrently when at least one access is a write unless
that location is protected by a mutex or accessed through the atomic API. A data
race is outside the portable language contract; optimizers may assume ordinary
non-atomic accesses are race-free.

Thread creation publishes values prepared before the spawn to the worker, and a
successful join makes the worker's completed writes visible to the joining
thread. `mutex_unlock` on a mutex synchronizes with a later successful
`mutex_lock` of the same mutex. These synchronization operations are compiler
barriers for ordinary memory effects; transformations must not move observable
loads/stores across them in a way that changes the happens-before relation.

Values crossing a thread boundary must remain valid for the worker lifetime. A
borrow may cross only when its owner is guaranteed to outlive the worker and no
conflicting mutation/release can occur; otherwise transfer an owned/managed value
or clone the data. Detached work must not capture a stack-local borrow whose
owner can leave scope before the worker completes.

### Atomics

`core.os.atomic` provides lock-free cells for simple shared counters. The current
`atomic_i64` operations are sequentially consistent: each atomic operation
participates in one global order consistent with program order. There is no
source-level relaxed/acquire/release ordering selector yet.

`atomic_i64(initial)` creates a cell; `atomic_load`, `atomic_store`,
`atomic_add`, `atomic_sub`, `atomic_exchange`, and `atomic_compare_exchange`
operate on it, with an optional byte `offset` into the cell. Free the cell with
`atomic_free`.

```ny
use core
use core.os.atomic

def counter = atomic_i64(0)
assert(atomic_add(counter, 1) == 1, "atomic increment")
atomic_free(counter)
```

## Attributes and effects

Attributes describe declaration metadata: linkage, codegen hints, purity,
effects, hot/cold markers, accelerator/vectorization hints, and ownership
contracts.

```text
@pure
@effects(none|io|alloc|ffi|thread|read|write|all)
@async_effects
```

`@pure` is shorthand for `@effects(none)`. `read`/`read_memory` and
`write`/`write_memory` denote memory effects.

## Terminal canvas contract

`core.term.canvas(width, height)` returns an opaque native handle. The
caller owns the handle and must call `canvas_free` exactly once after its last
use. The handle is outside the managed heap: it is not retained or reclaimed by
GC. Allocation failure returns `0`; canvas operations on an invalid or
out-of-bounds cell are no-ops. `canvas_clear` resets cells to spaces and clears
their color and bold attributes.

Each canvas coordinate is one **code-point cell**, not one grapheme cluster or
measured terminal column. `canvas_set` stores the first UTF-8 code point in
`ch`; `canvas_print` walks the input by code point and advances one canvas
column for each. Valid code points occupy one cell even when terminals display
them as two columns; combining marks also occupy their own cell. Invalid UTF-8
is replaced by U+FFFD. Keep canvas text to printable, single-column code points
when alignment matters. Newlines, tabs, and other control characters are not
expanded into rows or spaces.

The native cell buffer uses an eight-byte, zero-filled slot per cell. UTF-8
bytes occupy the start of that slot; foreground color and bold are stored in
separate per-cell arrays. `canvas_set` and `canvas_print` clamp color indices to
0–255 and store bold as false/true. `canvas_refresh` writes rows to stdout. On
an interactive terminal it homes the cursor, emits ANSI SGR when attributes
change, resets attributes at the end, and separates rows with CR/LF. In
non-interactive output it writes plain cell bytes with LF row separators and
omits color and bold sequences.

For interactive canvas loops, the draw callback borrows the canvas handle and
z-buffer passed to it. Use them only during the callback and do not free or
retain them: terminal resize replaces both, and loop exit frees the canvas. The
callback's return value is ignored. It may retain its own application state
outside those borrowed arguments.

The loop sets canvas height to terminal rows minus one, reserving the final
terminal row for application-owned status or legend output. `canvas_refresh`
homes the cursor and writes only the canvas rows; it does not clear or write the
reserved row. A resize clears the screen before the next callback, so redraw
application-owned row content after a resize.

Canvas color `0` selects the terminal default/reset. Nonzero indices map to the
basic ANSI foreground palette with `30 + (index % 8)`; indices therefore cycle
through black, red, green, yellow, blue, magenta, cyan, and white. Bold is SGR
attribute 1. This canvas index mapping is separate from `color_names()` and
`get_color_name()`, whose list also includes gray and starts at black index 0.

## Related

- [Control Flow](control-flow.md) for `defer` and `with`.
- [Native](native.md) for FFI boundary rules.
- [Concurrency](../learn/concurrency.md) for practical task and thread workflows.
- [Tooling](../learn/tooling.md) for run modes and diagnostics.
