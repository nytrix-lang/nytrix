<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":125,"summary":"Small, focused combinations of Nytrix language has for everyday programs."} -->
# Cookbook

Use one pattern at a time. Each pattern names the owning language rule and the
guide that explains its workflow.

| Goal | Combine | Read next |
| --- | --- | --- |
| Choose an ADT branch | `match` and exhaustive arms | [Patterns](../spec/patterns.md) |
| Choose a literal or range | `case` and guards | [Control flow](../spec/control-flow.md) |
| Precompute a table | `comptime table` and a typed lookup | [Comptime](comptime.md) |
| Check an index | `assert_compile_range` and `assert_compile_index` | [Proofs](proofs.md) |
| Return resource ownership | `@returns_owned` and a narrow owner | [Ownership](ownership.md) |
| Cross a C boundary | `layout`, `extern`, and a typed wrapper | [Native compilation](native.md) |
| Run independent work | async task plus `parallel_map` | [Effects](effects.md) |
| Draw a portable frame | window, frame, then render calls | [UI and rendering](ui.md) |
| Inspect a public API | `ny doc get` and source comments | [Library](library.md) |
| Measure a native change | reproducible binary and result assertion | [Performance](performance.md) |

## Match an algebraic value

```ny
use std.core

def value = ok(7)
def answer = match value {
   ok(n) -> n
   err(_) -> 0
}
assert(answer == 7, "matched result")
```

Match arms are exhaustive for ADT values. Add `err(_)` or a wildcard before
the compiler rejects the match.

## Use a compile-time table

```ny
use std.core

comptime table Opcode {
   0x00 -> "halt"
   0x10..0x1f -> "load"
   0x20..0x2f -> "math"
   _ -> "data"
}

fn opcode_name(i32 raw) str = comptime match Opcode(raw, "data")
assert(opcode_name(0x10) == "load", "table lookup")
```

Tables keep a static classification beside the code that consumes it.

## Classify with case

```ny
use std.core

fn bucket(int n) str {
   case n {
      0 -> "zero"
      1..9 -> "small"
      _ -> "large"
   }
}

assert(bucket(5) == "small", "range dispatch")
```

`case` handles literals, literal sets, and inclusive ranges in source order.

## Check a callback result

```ny
use std.os.parallel

fn twice(int x) int { x * 2 }
assert(parallel_map([1, 2, 3], twice) == [2, 4, 6], "parallel map")
```

Use a typed callback directly. The standard-library callback adapter preserves
the typed boundary.

## Chain list transforms

```ny
use std.core

def xs = [1, 2, 3, 4]
def doubled = xs.map(fn(x){ x * 2 })
def evens = xs.filter(fn(x){ (x % 2) == 0 })
def total = xs.reduce(0, fn(acc, x){ acc + x })
assert(doubled == [2, 4, 6, 8], "map")
assert(evens == [2, 4], "filter")
assert(total == 10, "reduce")
```

`map`, `filter`, and `reduce` return new values; assign the result back.

## Grow a list safely

```ny
use std.core

mut xs = []
xs = xs.append(1)
xs = xs.extend([2, 3])
assert(xs == [1, 2, 3], "append then extend")
```

`list(n)` reserves capacity but starts with zero elements. Always reassign the
result of `append`/`extend`.

## Accumulate into a dict

```ny
use std.core

mut counts = dict()
counts.set("a", counts.get("a", 0) + 1)
counts.set("a", counts.get("a", 0) + 1)
assert_eq(counts.get("a", 0), 2, "counted twice")
```

Use `dict()` for a mutable accumulator and `get(key, default)` for the missing
key case.

## Sort in place or as a copy

```ny
use std.core

mut xs = [3, 1, 2]
sort(xs)
assert(xs == [1, 2, 3], "in-place sort")

def ys = [3, 1, 2]
def sorted_ys = sorted(ys)
assert(ys == [3, 1, 2] && sorted_ys == [1, 2, 3], "sorted copy")
```

`sort` mutates and returns the same list; `sorted` returns a copy.

## Parse JSON safely

```ny
use std.core
use std.math.parse.data.json as json

def cfg = json.json_decode("{\"port\": 8080, \"host\": \"127.0.0.1\"}")
assert_eq(cfg.get("port", 0), 8080, "parsed port")
```

Treat decode results as dynamic values: use `get(key, default)` and shape
checks before relying on a field.

## Read and write a file

```ny
use std.os (file_read, file_write)
use std.core

def path = "/tmp/cookbook.txt"
assert(is_ok(file_write(path, "ny")), "write")
def data = file_read(path)
assert(is_ok(data) && unwrap(data) == "ny", "read back")
```

`file_read` returns `Result<str, int>` and `file_write` returns
`Result<int, int>`; inspect the result instead of assuming success.

## Read CLI arguments

```ny
use std.core
use std.os.args as args

def pos = args.positionals()
if pos.len > 0 { assert(is_str(pos[0]), "first positional") }
```

`positionals()` returns the positional argument list, indexed from zero.

## Run a background thread

```ny
use std.core
use std.os.thread

fn worker(any arg) any { arg }

def handle = thread_spawn(worker, "ok")
assert_eq(thread_join(handle), "ok", "thread result")
```

Protect shared mutable state with `mutex_new`/`mutex_lock`/`mutex_unlock`.

## Pass values through a channel

```ny
use std.core

def ch = channel(4)
assert(chan_send(ch, "ny"), "send")
assert_eq(chan_recv(ch), "ny", "recv")
```

`channel(capacity)` is unbounded when capacity is `0`. Use `chan_try_*` for
nonblocking forms.

## Return ownership explicitly

```ny
use std.core

@returns_owned
fn make_buffer() ptr { malloc(16) }

def buf = make_buffer()
free(buf)
```

`@returns_owned` states who frees the result. Pair owned returns with the
matching cleanup.

## Prove an index in bounds

```ny
use std.core

def xs = [10, 20, 30]
def int i = 1
assert_compile_index(xs, i, "index bounds")
assert(xs[i] == 20, "indexed value")
```

Compile-time proofs turn a range or index fact into a checked boundary before
the native store happens.

## Wrap a C call

```ny
use std.core

extern "c" {
   fn abs(int v) int
}

assert_eq(abs(-42), 42, "libc boundary")
```

Keep the extern block next to the wrapper that owns the ABI shape.

## Round a number

```ny
use std.core

assert_eq(round(3.6), 4, "round up")
assert_eq(abs(-3), 3, "absolute value")
```

`round` and `abs` are exported numeric helpers in `std.core`.

## Build a small state machine

```ny
use std.core

fn step(str state, str input) str {
   case state + ":" + input {
      "idle:start" -> "running"
      "running:stop" -> "idle"
      _ -> state
   }
}

assert(step("idle", "start") == "running", "state machine")
```

Keep one transition per `case` arm and a wildcard fallback for unknown input.

## Optional member access

```ny
use std.core

def maybe = nil
assert(maybe?.missing == nil, "optional chain on nil")
assert_eq("text".len, 4, "normal member on value")
```

`value?.member` returns `nil` for a nil receiver and performs the normal lookup
otherwise.

## Related

- [Start](start.md)
- [Programs](programs.md)
- [Testing](testing.md)
- [Concurrency](concurrency.md)