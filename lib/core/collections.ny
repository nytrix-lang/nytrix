;; Keywords: core collections list dict set data
;; Collection operations for queues, counters, grouping, filtering, and script-level data work.
;; References:
;; - std.core
module std.core.collections(Counter, counter, counter_add, counter_inc, counter_update, count_by, most_common, group_by, default_get, Queue, queue, queue_push, queue_pop, queue_try_pop, queue_peek, queue_len, queue_empty, queue_clear, Channel, channel, chan, chan_send, chan_try_send, chan_recv, chan_try_recv, chan_close, chan_closed, chan_len)
use std.core
use std.core.counter as ctr

fn _collection_missing(any xs) bool { xs == nil || xs == 0 }

@returns_owned
fn Counter(any xs=[]) dict {
   "Python-style Counter constructor. Returns a dict of item -> count."
   if _collection_missing(xs) { return dict(16) }
   ctr.counter(xs)
}

@returns_owned
fn counter(any xs=[]) dict {
   "Builds a frequency counter dictionary from `xs`."
   Counter(xs)
}

@returns_owned
@consumes(d)
fn counter_add(dict d, any key, int n=1) dict {
   "Adds `n` to `d[key]` and returns `d`."
   ctr.counter_add(d, key, n)
}

@returns_owned
@consumes(d)
fn counter_inc(dict d, any key) dict {
   "Increments `d[key]` and returns `d`."
   ctr.counter_add(d, key, 1)
}

@returns_owned
@consumes(d)
fn counter_update(dict d, any xs) dict {
   "Adds every item from `xs` into counter `d`."
   if _collection_missing(xs) { return d }
   mut i = 0
   while i < xs.len {
      d = counter_add(d, xs.get(i), 1)
      i += 1
   }
   d
}

@returns_owned
fn count_by(any xs, fnptr key_fn) dict {
   "Counts values by `key_fn(value)`, like `Counter(map(key_fn, xs))` without building an intermediate list."
   mut out = dict(16)
   if _collection_missing(xs) { return out }
   mut i = 0
   while i < xs.len {
      def k = key_fn(xs.get(i))
      out = counter_add(out, k, 1)
      i += 1
   }
   out
}

@returns_owned
fn most_common(dict d, int n=0) list {
   "Returns `[key, count]` pairs sorted by descending count. Optional `n` limits output."
   def all = ctr.most_common(d)
   if n <= 0 || n >= all.len { return all }
   slice(all, 0, n)
}

@returns_owned
fn group_by(any xs, fnptr key_fn) dict {
   "Groups values from `xs` by `key_fn(value)`."
   mut out = dict(16)
   if _collection_missing(xs) { return out }
   mut i = 0
   while i < xs.len {
      def v, k = xs.get(i), key_fn(v)
      mut bucket = out.get(k, [])
      bucket = bucket.append(v)
      out[k] = bucket
      i += 1
   }
   out
}

fn default_get(dict d, any key, any default) any {
   "Returns `d[key]`, installing `default` into `d` first when missing."
   if !d.contains(key) { d[key] = default }
   d.get(key, default)
}

@returns_owned
fn Queue(any xs=[]) dict {
   "Creates a FIFO queue. Queue operations mutate and return/use the queue dict."
   mut items = list()
   if !_collection_missing(xs) {
      mut i = 0
      while i < xs.len {
         items = items.append(xs.get(i))
         i += 1
      }
   }
   {"kind": "queue", "items": items, "head": 0}
}

@returns_owned
fn queue(any xs=[]) dict {
   "Alias for Queue(xs)."
   Queue(xs)
}

fn _queue_items(dict q) list { q.get("items", list()) }

fn _queue_apply(dict dst, dict src) dict {
   "Copies queue storage fields from `src` back into `dst` for APIs that must mutate in place."
   dst["items"] = src.get("items", list())
   dst["head"] = src.get("head", 0)
   dst
}

fn queue_len(dict q) int {
   "Returns the number of queued items."
   def items = _queue_items(q)
   def head = q.get("head", 0)
   def n = items.len - head
   if n < 0 { return 0 }
   n
}

fn queue_empty(dict q) bool {
   "Returns true when the queue has no items."
   queue_len(q) == 0
}

@returns_owned
@consumes(q)
fn _queue_compact(dict q) dict {
   def items = _queue_items(q)
   def head = q.get("head", 0)
   if head <= 64 || head * 2 < items.len { return q }
   mut out = list(items.len - head)
   mut i = head
   while i < items.len {
      out = out.append(items.get(i))
      i += 1
   }
   q = q.set("items", out)
   q = q.set("head", 0)
   q
}

@returns_owned
@consumes(q)
fn queue_push(dict q, any value) dict {
   "Pushes `value` onto the queue and returns the queue."
   mut items = _queue_items(q)
   items = items.append(value)
   q = q.set("items", items)
   if !q.contains("head") { q = q.set("head", 0) }
   q
}

fn queue_peek(dict q, any default=0) any {
   "Returns the next queued value without removing it."
   if queue_empty(q) { return default }
   _queue_items(q).get(q.get("head", 0), default)
}

fn queue_pop(dict q, any default=0) any {
   "Removes and returns the next queued value, or `default` when empty."
   if queue_empty(q) { return default }
   def items = _queue_items(q)
   def head = q.get("head", 0)
   def value = items.get(head, default)
   q["head"] = head + 1
   def updated = _queue_compact(q)
   _queue_apply(q, updated)
   value
}

@returns_owned
fn queue_try_pop(dict q) dict {
   "Returns `{ok, value, queue}` for a nonblocking queue pop."
   if queue_empty(q) { return {"ok": false, "value": 0, "queue": q} }
   def value = queue_pop(q)
   {"ok": true, "value": value, "queue": q}
}

@returns_owned
@consumes(q)
fn queue_clear(dict q) dict {
   "Removes all queued values and returns the queue."
   q = q.set("items", list())
   q = q.set("head", 0)
   q
}

@returns_owned
fn Channel(int capacity=0) dict {
   "Creates a cooperative channel. `capacity=0` means unbounded."
   {"kind": "chan", "items": list(), "head": 0, "closed": false, "capacity": capacity}
}

@returns_owned
fn channel(int capacity=0) dict {
   "Alias for Channel(capacity)."
   Channel(capacity)
}

@returns_owned
fn chan(int capacity=0) dict {
   "Short alias for Channel(capacity)."
   Channel(capacity)
}

fn chan_closed(dict ch) bool {
   "Returns true when a channel is closed."
   ch.get("closed", false)
}

fn chan_len(dict ch) int {
   "Returns queued messages in a channel."
   queue_len(ch)
}

fn chan_send(dict ch, any value) bool {
   "Sends `value` if the channel is open and capacity permits; returns success."
   if chan_closed(ch) { return false }
   def cap = ch.get("capacity", 0)
   if cap > 0 && chan_len(ch) >= cap { return false }
   def updated = queue_push(ch, value)
   _queue_apply(ch, updated)
   true
}

fn chan_try_send(dict ch, any value) bool {
   "Nonblocking send alias for `chan_send`."
   chan_send(ch, value)
}

fn chan_recv(dict ch, any default=0) any {
   "Receives the next value, or `default` when no message is queued."
   queue_pop(ch, default)
}

@returns_owned
fn chan_try_recv(dict ch) dict {
   "Returns `{ok, value, closed}` for a nonblocking receive."
   mut r = queue_try_pop(ch)
   r = r.set("closed", chan_closed(ch))
   r
}

@returns_owned
@consumes(ch)
fn chan_close(dict ch) dict {
   "Closes a channel; queued messages remain receiveable."
   ch.set("closed", true)
}

#main {
   fn _selftest_len(any x) any { x.len }
   def c = Counter(["a", "b", "a"])
   assert_eq(c.get("a", 0), 2, "collections Counter")
   mut d = Counter(["x", "x", "y"])
   d = counter_update(d, ["z", "z", "z"])
   assert(d.get("z", 0) == 3 && most_common(d, 1).get(0).get(0) == "z", "collections counter helpers")
   assert(group_by(["aa", "b", "cc"], _selftest_len).get(2).len == 2 && count_by(["aa", "b", "cc"], _selftest_len).get(2, 0) == 2, "collections grouping")
   mut q = Queue()
   q = queue_push(q, "a")
   q = queue_push(q, "b")
   assert(queue_len(q) == 2 && queue_pop(q) == "a" && queue_try_pop(q).get("value") == "b" && queue_empty(q), "collections queue")
   def ch = chan(1)
   assert(chan_send(ch, 42) && !chan_send(ch, 43) && chan_recv(ch) == 42, "collections channel send/recv")
   assert(chan_try_send(ch, "first") && !chan_try_send(ch, "second") && chan_recv(ch, "") == "first", "collections channel try send")
   chan_close(ch)
   assert(chan_closed(ch), "chan closed")
   print("✓ std.core.collections self-test passed")
}
