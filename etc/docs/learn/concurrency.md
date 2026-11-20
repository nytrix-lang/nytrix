<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":95,"summary":"Run independent work in parallel, share results through channels, and protect shared state."} -->
# Concurrency

Concurrency is easiest to reason about when independent tasks return values
that one caller merges. Use channels or queues for handoff; protect shared
mutable state explicitly.

Prerequisites: basic [Nytrix syntax](start.md#tour); thread APIs also require a
thread-capable target.

## Choose the abstraction

| Need | Use |
| --- | --- |
| Map independent list items | `parallel_map` |
| One background task | `async` + `await` |
| Own a raw thread with a handle | `thread_spawn` + `thread_join` |
| Pass values between tasks | `channel` + `chan_send` / `chan_recv` |
| Protect shared mutable state | `mutex_new` / `mutex_lock` / `mutex_unlock` |
| Atomic counters and flags | `atomic_i64` family in `core.os.atomic` |

## Parallel map

`core.os.parallel` maps a typed callback over a list with a bounded worker set:

```ny
use core
use core.os.parallel as parallel

fn twice(int x) int { x * 2 }
assert(parallel.parallel_map([1, 2, 3], twice) == [2, 4, 6], "parallel map")
```

> **Note:** `core.os.parallel.parallel_map([1, 2, 3], twice)` returns `[2, 4, 6]`
> in input order. The matching `lang/concurrency/parallel-map-jit-parity.nshape`
> fixture passes all four route variants (MCJIT, ORC, native-only, and
> `--parallel=threads --threads=2 --parallel-min-work=1`).

## Async tasks

`core.os.async` starts and joins units of work:

```ny
use core
use core.os.async as a

def t1 = a.async(fn() { 21 + 21 })
def t2 = a.async(fn() { 40 + 2 })
def done = a.await_all([t1, t2])
assert_eq(done, [42, 42], "await all")
```

`await` joins one task; `yield_now` and `sleep_ms` yield the current unit.

## Threads

Own a thread with a handle and join for its result:

```ny
use core
use core.os.thread as thread

fn worker(any arg) any { arg }
def handle = thread.thread_spawn(worker, "ok")
assert_eq(thread.thread_join(handle), "ok", "thread result")
```

`thread_spawn_call` takes a callable object. Join every handle you spawn.

## Channels

A channel passes values between producers and consumers. Capacity `0` is
unbounded:

```ny
use core

def ch = channel(4)
assert(chan_send(ch, "ny"), "send")
assert_eq(chan_recv(ch), "ny", "recv")
```

Use `chan_try_send` and `chan_try_recv` for nonblocking forms. `chan_close`
signals shutdown and `chan_closed` reports it. `chan_len` reports the pending
count.

## Queues

`queue`, `queue_push`, `queue_pop`, `queue_try_pop`, `queue_peek`, `queue_len`,
and `queue_empty` cover FIFO handoff in `core`. Use a queue when items are
produced and consumed in order.

## Protect shared state

Use the same mutex around every access to shared mutable state. Join workers
before freeing the lock:

```ny
use core
use core.os.thread as thread

def lock = thread.mutex_new()
mut shared = 0
fn increment(any arg) any {
   thread.mutex_lock(lock)
   shared += 1
   thread.mutex_unlock(lock)
   0
}
def first = thread.thread_spawn(increment, 0)
def second = thread.thread_spawn(increment, 0)
thread.thread_join(first)
thread.thread_join(second)
assert_eq(shared, 2, "mutex protects shared counter")
thread.mutex_free(lock)
```

The updates cannot overlap because both workers hold `lock` while changing the
counter. Prefer channels and merged results when a mutex is unnecessary.

## Atomic values

`core.os.atomic` exports `atomic_i64` with `load`, `store`, `add`, `sub`,
`exchange`, and `compare_exchange`, plus `free`. Use an atomic for a single
counter or flag that many threads touch.

## When not to parallelize

Keep the scalar path when the unit of work is smaller than the scheduling
overhead, when the result is order-dependent, or when the target does not
support threads. Measure the parallel and scalar paths before keeping a
rewrite (see [Performance](performance.md)).

Next:

- [Runtime: concurrency](../spec/runtime.md#concurrency)
- [Effects](effect.md)
- [Cookbook](cookbook.md)
