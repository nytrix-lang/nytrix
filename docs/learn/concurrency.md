<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":95,"summary":"Run independent work in parallel, share results through channels, and protect shared state."} -->
# Concurrency

Parallelism works best on independent work with a merged result. Communicate
through channels and queues instead of sharing mutable state between threads.

## Choose the abstraction

| Need | Use |
| --- | --- |
| Map independent list items | `parallel_map` |
| One background task | `async` + `await` |
| Own a raw thread with a handle | `thread_spawn` + `thread_join` |
| Pass values between tasks | `channel` + `chan_send` / `chan_recv` |
| Protect shared mutable state | `mutex_new` / `mutex_lock` / `mutex_unlock` |
| Atomic counters and flags | `atomic_i64` family in `std.os.atomic` |

## Parallel map

`std.os.parallel` maps a typed callback over a list with a bounded worker set:

```ny
use std.core
use std.os.parallel

fn twice(int x) int { x * 2 }
assert(parallel_map([1, 2, 3], twice) == [2, 4, 6], "parallel map")
```

Keep the callback side-effect free. `parallel_map_indexed` also passes the
index when the result depends on position.

## Async tasks

`std.os.async` starts and joins units of work:

```ny
use std.core
use std.os.async as a

def t1 = a.async(fn() { 21 + 21 })
def t2 = a.async(fn() { 40 + 2 })
def done = a.await_all([t1, t2])
```

`await` joins one task; `yield_now` and `sleep_ms` yield the current unit.

## Threads

Own a thread with a handle and join for its result:

```ny
use std.core
use std.os.thread

fn worker(any arg) any { arg }
def handle = thread_spawn(worker, "ok")
assert_eq(thread_join(handle), "ok", "thread result")
```

`thread_spawn_call` takes a callable object. Join every handle you spawn.

## Channels

A channel passes values between producers and consumers. Capacity `0` is
unbounded:

```ny
use std.core

def ch = channel(4)
assert(chan_send(ch, "ny"), "send")
assert_eq(chan_recv(ch), "ny", "recv")
```

Use `chan_try_send` and `chan_try_recv` for nonblocking forms. `chan_close`
signals shutdown and `chan_closed` reports it. `chan_len` reports the pending
count.

## Queues

`queue`, `queue_push`, `queue_pop`, `queue_try_pop`, `queue_peek`, `queue_len`,
and `queue_empty` cover FIFO handoff in `std.core`. Use a queue when items are
produced and consumed in order.

## Shared state

Protect shared mutable state with a mutex:

```ny
use std.core
use std.os.thread

mut m = thread.mutex_new()
thread.mutex_lock(m)
;; guarded section
thread.mutex_unlock(m)
```

Free mutexes with `thread.mutex_free(m)`. Prefer channels and merged results
over shared mutable state; the compiler rejects a release while a borrow of
the same slot is live.

## Atomic values

`std.os.atomic` exports `atomic_i64` with `load`, `store`, `add`, `sub`,
`exchange`, and `compare_exchange`, plus `free`. Use an atomic for a single
counter or flag that many threads touch.

## When not to parallelize

Keep the scalar path when the unit of work is smaller than the scheduling
overhead, when the result is order-dependent, or when the target does not
support threads. Measure the parallel and scalar paths before keeping a
rewrite (see [Performance](performance.md)).

## Related

- [Runtime: concurrency](../spec/runtime.md#concurrency)
- [Effects](effects.md)
- [Cookbook](cookbook.md)