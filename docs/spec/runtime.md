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

Ordinary compilation runs the ownership/provenance pass in advisory mode by
default. It warns about tracked use-after-move, double release, releasing or
mutating an observed value, and escaping borrows, but it does not change the
heap policy or insert cleanup. This makes common lifetime mistakes visible
without turning small scripts into an annotation exercise.

```bash
ny --safe-mode file.ny
ny --mode=safe file.ny
```

In this profile, code scopes owned raw allocations with `with ptr name` or
returns them through an ownership contract. Raw memory loads and stores against
a compiler-tracked allocation require a proven byte range:

```ny
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

## Heap policy

The default heap path uses the native runtime allocator for runtime-managed
Nytrix objects. Standard library constructors such as strings, lists, dicts,
sets, tuples, and ordinary result values return managed Nytrix objects. Raw
buffers, external handles, FFI pointers, and memory returned by native APIs
still follow the owning API's explicit native ownership rules.

Enable the nursery/tenured GC with `-gc` or `--heap=gc`; the CLI sets the
runtime `NYTRIX_GC` switch from that policy.

```bash
ny -gc file.ny
NYTRIX_GC_NURSERY_SIZE=64M NYTRIX_GC_TENURED_SIZE=512M ny --heap=gc file.ny
```

With GC disabled, the collector reserves no nursery or tenured spaces. With GC
enabled, configure the nursery, tenured space, and large-object threshold with
`NYTRIX_GC_NURSERY_SIZE`, `NYTRIX_GC_TENURED_SIZE`, and
`NYTRIX_GC_LOS_THRESHOLD`. Size values accept bytes, `K`, `M`, or `G`.

### Collector mechanics

In GC mode the collector adds a 16-byte collector header before the ordinary
runtime prefix and moves supported heap objects between the nursery and the
tenured space. Managed Nytrix objects are scanned and relocated; the collector
keeps its own header so the ordinary prefix and object layout stay unchanged.
Large objects above `NYTRIX_GC_LOS_THRESHOLD` are not moved; they are tracked
directly.

GC mode changes allocation for managed Nytrix objects. Native handles and raw
buffers still need cleanup. Pair native allocations that escape the managed
object model with the owning API's cleanup function, `with` scopes, or a
`release` / `forget` contract. A raw buffer or handle is not traced and is not
moved; do not cache its address across a GC boundary.

## Ownership

Declare ownership contracts with attributes. Compiler and runtime modes check
them. Resource APIs define whether a value is borrowed, owned, released, or
intentionally forgotten.

```ny
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

Ownership diagnostics run by default:

```bash
ny file.ny                         # advisory diagnostics, no cleanup changes
ny --ownership-strict file.ny      # diagnostics become errors
ny --ownership file.ny             # RAII cleanup plus diagnostics
ny --safe-mode file.ny             # strict safety profile
```

`--borrow-check` explicitly enables the default advisory pass. It reports
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

```ny
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
synchronization APIs from `std.core` and `std.os`.

### Async tasks

`async` starts a stackless task and `await` waits for its value:

```ny
use std.os.async (async, await)

fn plus_one(x){ x + 1 }

def h = async plus_one(41)
assert(await h == 42, "async result")
```

The callable form is also supported: `async(fn_value, arg...)` and
`await(handle)`. `await_all(handles)` waits for a list of handles.
`future(fn_value, arg...)` and `Future(fn_value, arg...)` are compatibility
constructors for joinable async handles. `sleep_ms(ms)` and `yield_now()` are
async scheduling helpers in `std.os.async`.

Async socket helpers return awaitable handles for connect, accept, read, write,
and read-until operations.

On the browser target, `std.os.time.msleep(ms)` uses the runner's Asyncify
bridge to yield to the browser event loop and resume the suspended Wasm stack
after the delay. It does not block the browser thread. The fixture
`etc/tests/native/web/time-sleep.ny` verifies this behavior.

### Threads and mutexes

`std.os.thread` spawns OS threads. `thread_spawn(fn_value, arg)` joins with
`thread_join(handle)`. `thread_spawn_call(fn_value, args)` passes a list of
arguments. Protect shared mutable state with `mutex_new`, `mutex_lock`, and
`mutex_unlock`; free the mutex with `mutex_free`.

```ny
use std.core
use std.os.thread

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
use std.core

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

`std.os.atomic` provides lock-free cells for simple shared counters. The current
`atomic_i64` operations are sequentially consistent: each atomic operation
participates in one global order consistent with program order. There is no
source-level relaxed/acquire/release ordering selector yet.

`atomic_i64(initial)` creates a cell; `atomic_load`, `atomic_store`,
`atomic_add`, `atomic_sub`, `atomic_exchange`, and `atomic_compare_exchange`
operate on it, with an optional byte `offset` into the cell. Free the cell with
`atomic_free`.

```ny
use std.os.atomic

def counter = atomic_i64(0)
assert(atomic_add(counter, 1) == 1, "atomic increment")
atomic_free(counter)
```

## Attributes and effects

Attributes describe declaration metadata: linkage, codegen hints, purity,
effects, hot/cold markers, accelerator/vectorization hints, and ownership
contracts.

```ny
@pure
@effects(none|io|alloc|ffi|thread|all)
@async_effects
```

`@pure` is shorthand for `@effects(none)`.

## Related

- [Control Flow](control-flow.md) for `defer` and `with`.
- [Native](native.md) for FFI boundary rules.
- [Concurrency](../learn/concurrency.md) for practical task and thread workflows.
- [Tooling](../learn/tooling.md) for run modes and diagnostics.