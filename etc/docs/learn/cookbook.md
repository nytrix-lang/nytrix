<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":200,"summary":"Short patterns for common Nytrix programming tasks."} -->
# Cookbook

Use the smallest pattern that fits, then follow its linked contract before
extending it.

Prerequisites: basic [Nytrix syntax](start.md#tour).

| Goal | Combine | Read next |
| --- | --- | --- |
| Choose an ADT branch | `match` and exhaustive arms | [Patterns](../spec/patterns.md) |
| Choose a literal or range | `case` and guards | [Control flow](../spec/control-flow.md) |
| Precompute a table | `comptime table` and a typed lookup | [Comptime](comptime.md) |
| Check an index | `assert_compile_range` and `assert_compile_index` | [Proofs](proofs.md) |
| Return resource ownership | `@returns_owned` and a narrow owner | [Ownership](ownership.md) |
| Attach methods or traits | `impl`, `trait`, `law`, operators | [Traits and impl](../spec/traits.md) |
| Cross a C boundary | `layout`, `extern`, and a typed wrapper | [Native compilation](native.md) |
| Run independent work | async task plus `parallel_map` | [Effects](effect.md) |
| Draw a portable frame | window, frame, then render calls | [UI and rendering](ui.md) |
| Inspect a public API | `ny doc get` and source comments | [Tooling](tooling.md#find-api-documentation) |
| Measure a native change | reproducible binary and result assertion | [Performance](performance.md) |

## Match an algebraic value

**Problem:** Select a payload from an enum while handling every variant.

```ny
use core

enum DemoResult {
   Value(int value),
   Failure(str message)
}

def value = DemoResult.Value(7)
def answer = match value {
   DemoResult.Value(n) -> n
   DemoResult.Failure(_) -> 0
}
assert(answer == 7, "matched result")
```

**Why it works:** `match` binds the selected constructor's payload and checks
that every ADT variant is covered.

> **Tip:** Use `case` for literal and range dispatch; use `match` to inspect
> ADT variants.

## Use a compile-time table

**Problem:** Classify fixed opcode ranges without a runtime decision tree.

```ny
use core

comptime table Opcode {
   0x00 -> "halt"
   0x10..0x1f -> "load"
   0x20..0x2f -> "math"
   _ -> "data"
}

fn opcode_name(i32 raw) str = comptime match Opcode(raw, "data")
assert(opcode_name(0x10) == "load", "table lookup")
```

**Why it works:** The table is evaluated during compilation, and the function
uses the generated lookup.

## Classify with case

**Problem:** Map a value to ordered literal and range categories.

```ny
use core

fn bucket(int n) str {
   case n {
      0 -> "zero"
      1..9 -> "small"
      _ -> "large"
   }
}

assert(bucket(5) == "small", "range dispatch")
```

**Why it works:** `case` checks literal and inclusive-range arms in source
order.

## Check a callback result

**Problem:** Apply a typed function to each list item concurrently.

```ny
use core
use core.os.parallel as parallel

fn twice(int x) int { x * 2 }
assert(parallel.parallel_map([1, 2, 3], twice) == [2, 4, 6], "parallel map")
```

**Why it works:** The typed callback makes the input and output types explicit;
`parallel_map` joins chunks in their original range order, so worker completion
order does not reorder results.

> **Tip:** For a small workload, compare this with a plain `for` loop; thread
> scheduling can cost more than the work.

## Chain list transforms

**Problem:** Build mapped and filtered lists and a reduction in one pass.

```ny
use core

def xs = [1, 2, 3, 4]
mut doubled = []
mut evens = []
mut total = 0
for x in xs {
   doubled = doubled.append(x * 2)
   if (x % 2) == 0 { evens = evens.append(x) }
   total += x
}
assert(doubled == [2, 4, 6, 8], "map")
assert(evens == [2, 4], "filter")
assert(total == 10, "reduce")
```

**Why it works:** One loop makes the retained append results and the sum
explicit. See [sequence operations](../spec/values.md#sequence-operations) for
the library `map`, `filter`, and `reduce` APIs.

## Build a list

**Problem:** Reserve list capacity, append values, and retain the grown list.

```ny
use core

mut xs = list(3)
xs = xs.append(1)
xs = xs.append(2)
assert(xs.len == 2 && xs[1] == 2, "retained append")
```

**Why it works:** `list(n)` reserves capacity without adding elements, and
retaining `append` handles growth that returns a replacement list.

## Accumulate into a dict

**Problem:** Count repeated keys in a mutable dictionary.

```ny
use core

mut counts = dict()
counts.set("a", int(counts.get("a", 0)) + 1)
counts.set("a", int(counts.get("a", 0)) + 1)
assert_eq(counts.get("a", 0), 2, "counted twice")
```

**Why it works:** `dict()` creates a mutable accumulator, and `get` supplies
zero for a key that has not been inserted.

## Sort in place or as a copy

**Problem:** Choose whether sorting should change the original list.

```ny
use core

mut xs = [3, 1, 2]
sort(xs)
assert(xs == [1, 2, 3], "in-place sort")

def ys = [3, 1, 2]
def list<int> sorted_ys = sorted(ys)
assert(ys == [3, 1, 2] && sorted_ys == [1, 2, 3], "sorted copy")
```

**Why it works:** `sort` mutates and returns the same list; `sorted` returns a
copy.

> **Tip:** Use `sorted` when another caller still needs the original order.

## Parse JSON safely

**Problem:** Read one known field from decoded JSON without assuming it exists.

```ny
use core
use core.math.parse.data.json as json

def cfg = json.json_decode("{\"port\": 8080, \"host\": \"127.0.0.1\"}")
assert_eq(cfg.get("port", 0), 8080, "parsed port")
```

**Why it works:** Decoded values are dynamic; `get(key, default)` makes the
missing-field case explicit. Check the value's shape before relying on it.

## Read and write a file

**Problem:** Persist text and verify both file operations returned success.

```ny
use core.os (file_read, file_write)
use core

def path = "/tmp/cookbook.txt"
assert(is_ok(file_write(path, "ny")), "write")
def data = file_read(path)
assert(is_ok(data) && unwrap(data) == "ny", "read back")
```

**Why it works:** `file_read` returns `Result<str, int>` and `file_write`
returns `Result<int, int>`, so the code checks each result before using it.

## Read CLI arguments

**Problem:** Read the first positional argument only when one was supplied.

```ny
use core
use core.os.args as args

def pos = args.positionals()
if pos.len > 0 { assert(is_str(pos[0]), "first positional") }
```

**Why it works:** `positionals()` returns a list indexed from zero; the length
guard prevents reading an absent element.

## Run a background thread

**Problem:** Run a function on a worker thread and obtain its result.

```ny
use core
use core.os.thread as thread

fn worker(any arg) any { arg }

def handle = thread.thread_spawn(worker, "ok")
assert_eq(thread.thread_join(handle), "ok", "thread result")
```

**Why it works:** `thread_spawn` returns a handle, and `thread_join` waits for
the worker and returns its value. For shared state and mutex lifetimes, see
[Concurrency](concurrency.md).

## Pass values through a channel

**Problem:** Send a value through a channel and receive it on the other side.

```ny
use core

def ch = channel(4)
assert(chan_send(ch, "ny"), "send")
assert_eq(chan_recv(ch), "ny", "recv")
```

**Why it works:** `chan_send` and `chan_recv` use the same channel state.
`channel(capacity)` is unbounded at capacity `0`; use `chan_try_*` for
nonblocking forms.

## Return ownership explicitly

**Problem:** Return allocated memory and make the caller responsible for
releasing it.

```ny
use core

@returns_owned
fn make_buffer() ptr { malloc(16) }

def buf = make_buffer()
free(buf)
```

**Why it works:** `@returns_owned` documents the transfer, and the caller pairs
the returned pointer with `free`.

## Prove an index in bounds

**Problem:** Establish an index bound before reading a list element.

```ny
use core

def xs = [10, 20, 30]
def int i = 1
assert_compile_index(xs, i, "index bounds")
assert(xs[i] == 20, "indexed value")
```

**Why it works:** `assert_compile_index` requires a proven bound; it rejects an
unknown bound rather than assuming the assertion is true.

## Wrap a C call

**Problem:** Call a C library function through a declared ABI boundary.

```ny
use core

extern "c" {
   fn abs(int v) int
}

assert_eq(abs(-42), 42, "libc boundary")
```

**Why it works:** `extern` gives the compiler the symbol's argument and return
types. Keep the declaration next to the wrapper that owns the ABI shape.

## Round a number

**Problem:** Use the standard numeric helpers for rounding and absolute value.

```ny
use core

assert_eq(round(3.6), 4, "round up")
assert_eq(abs(-3), 3, "absolute value")
```

**Why it works:** `round` and `abs` are exported numeric helpers in `core`.

## Build a small state machine

**Problem:** Express a small transition table with explicit fallback behavior.

```ny
use core

fn step(str state, str input) str {
   case state + ":" + input {
      "idle:start" -> "running"
      "running:stop" -> "idle"
      _ -> state
   }
}

assert(step("idle", "start") == "running", "state machine")
```

**Why it works:** Each `case` arm represents one transition, and `_` preserves
the current state for unknown input.

## Optional member access

**Problem:** Read an optional member without failing when its receiver is nil.

```ny
use core

def maybe = nil
assert(maybe?.missing == nil, "optional chain on nil")
assert_eq("text".len, 4, "normal member on value")
```

**Why it works:** `value?.member` returns `nil` for a nil receiver and performs
the normal lookup otherwise.

Next:

- [Start](start.md)
- [Programs](programs.md)
- [Testing](testing.md)
