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

`--safe-mode` gives Nytrix its compile-time safety profile. It keeps the
default type checks and adds ownership/borrow checking, RC/RAII cleanup, strict
effect/alias policy, and stricter raw-memory diagnostics.

```bash
ny --safe-mode file.ny
ny --mode=safe file.ny
```

In this profile, code scopes owned raw allocations with `with ptr` or returns
them through an ownership contract. Raw memory loads and stores against a
compiler-tracked allocation require a proven byte range:

```ny
with ptr: p = malloc(8){
   def int: i = 3
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

GC mode changes allocation for managed Nytrix objects. Native handles and raw
buffers still need cleanup. Pair native allocations that escape the managed
object model with the owning API's cleanup function, `with` scopes, or a
`release` / `forget` contract.

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

`&expr` is the borrow operator and is equivalent to `borrow(expr)`. `own`
marks a value as owned. `release` consumes and drops an owned value. `forget`
consumes an owned value without dropping it.

Enable compiler checks:

```bash
ny --borrow-check file.ny
ny --borrow-check --ownership-strict file.ny
```

In strict ownership mode the compiler reports moves, releases, mutations, and
reassignments while a borrow is live; use after move; double release; local
borrow escapes; and owned returns without `@returns_owned`.

With borrow checking disabled, the compiler still parses ownership attributes
and keeps them as declaration metadata. Run `ny --borrow-check` or
`ny --safe-mode` for files that rely on those contracts.

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

## Scoped cleanup

`defer` and `with` provide language-level cleanup. Library APIs can build
resource-safe wrappers on top.

```ny
defer { cleanup() }
with Resource: r = open_resource() { use(r) }
```

## Concurrency

The runtime and standard library include stackless async tasks, OS threads,
atomics, queues, channels, and network async helpers. Shared state uses
synchronization APIs from `std.core` and `std.os`.

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

- [control-flow.md](control-flow.md) for `defer` and `with`.
- [native.md](native.md) for FFI boundary rules.
- [tooling.md](../learn/tooling.md) for run modes and diagnostics.
