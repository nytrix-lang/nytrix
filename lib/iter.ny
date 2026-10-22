;; Keywords: core iter iteration sequence functional
;; Iterator and sequence operations for map, filter, reduce, chunking, and traversal.
;; References:
;; - core
module core.iter(range, range2, enumerate, map, filter, repeat, take, drop, reverse, zip2, any, all, fold, reduce, sum, each, count, count_if, first, last, find_if, find_index_if, chain, flatten, filter_map, compact, zip_with, cycle, partition, chunk, windowed, mapcat)
use core
use core.primitives as core_prim
use core.str as strmod

;; Keep the string path local to this module.  Calling the attached `str.*`
;; wrappers from a module that is also imported through `core` can resolve
;; back to their adapter symbol, which is recursive for dynamic string args.
@inline
fn _iter_utf8_seq_len(any s, int i, int n) int {
   if i < 0 || i >= n { return -1 }
   def b0 = load8(s, i) & 255
   if b0 < 128 { return 1 }
   if b0 >= 194 && b0 <= 223 {
      if i + 1 >= n { return -1 }
      def b1 = load8(s, i + 1) & 255
      if b1 < 128 || b1 > 191 { return -1 }
      return 2
   }
   if b0 >= 224 && b0 <= 239 {
      if i + 2 >= n { return -1 }
      def b1, b2 = load8(s, i + 1) & 255, load8(s, i + 2) & 255
      if b1 < 128 || b1 > 191 || b2 < 128 || b2 > 191 { return -1 }
      if b0 == 224 && b1 < 160 { return -1 }
      if b0 == 237 && b1 >= 160 { return -1 }
      return 3
   }
   if b0 >= 240 && b0 <= 244 {
      if i + 3 >= n { return -1 }
      def b1, b2 = load8(s, i + 1) & 255, load8(s, i + 2) & 255
      def b3 = load8(s, i + 3) & 255
      if b1 < 128 || b1 > 191 || b2 < 128 || b2 > 191 || b3 < 128 || b3 > 191 { return -1 }
      if b0 == 240 && b1 < 144 { return -1 }
      if b0 == 244 && b1 > 143 { return -1 }
      return 4
   }
   -1
}

@inline
fn _iter_utf8_len(any s) int {
   if !is_str(s) { return 0 }
   def n = s.len
   mut i = 0
   mut count = 0
   while i < n {
      def w = _iter_utf8_seq_len(s, i, n)
      if w < 1 { i += 1 } else { i += w }
      count += 1
   }
   count
}

@inline
fn _iter_ord_at(any s, int idx) int {
   if !is_str(s) || idx < 0 { return 0 }
   def n = s.len
   mut i = 0
   mut pos = 0
   while i < n {
      def w = _iter_utf8_seq_len(s, i, n)
      if pos == idx {
         def b0 = load8(s, i) & 255
         if w == 1 || w < 1 { return b0 }
         if w == 2 { return ((b0 & 31) << 6) | (load8(s, i + 1) & 63) }
         if w == 3 {
            return ((b0 & 15) << 12) | ((load8(s, i + 1) & 63) << 6) |
            (load8(s, i + 2) & 63)
         }
         return ((b0 & 7) << 18) | ((load8(s, i + 1) & 63) << 12) |
         ((load8(s, i + 2) & 63) << 6) | (load8(s, i + 3) & 63)
      }
      if w < 1 { i += 1 } else { i += w }
      pos += 1
   }
   0
}

@inline
@returns_owned
fn _iter_utf8_slice(any s, int start, int stop, int step=1) any {
   ; Keep iterator slicing on the local UTF-8 path.  Calling the attached
   ; `str.utf8_slice` adapter here makes a dynamic string cross the same
   ; source/native wrapper boundary twice, which can recurse or reinterpret
   ; the string handle after an earlier callback.
   if !is_str(s) { return "" }
   def n = _iter_utf8_len(s)
   if step == 0 { step = 1 }
   if start < 0 { start = n + start }
   if stop < 0 { stop = n + stop }
   if start < 0 { start = 0 }
   if stop < 0 { stop = 0 }
   if start > n { start = n }
   if stop > n { stop = n }
   if step < 0 || start >= stop { return "" }
   mut out = strmod.Builder((stop - start) * 2 + 8)
   mut i = start
   while i < stop {
      out = strmod.builder_append(out, strmod.chr(_iter_ord_at(s, i)))
      i += step
   }
   def result = strmod.builder_to_str(out)
   strmod.builder_free(out)
   result
}

@inline
@jit
fn _iter_is_seq(any x) bool {
   is_list(x) || is_tuple(x) || is_str(x) || is_bytes(x) || is_range(x)
}

@inline
@jit
fn _iter_seq_len(seq xs, str fn_name) int {
   ; `seq` is already the checked sequence boundary. Re-running the dynamic
   ; tag probe here forced the native backend through an erased `any` call
   ; while the sequence ABI carries its length in a separate slot. Reading
   ; the length from that ABI directly keeps native and LLVM iteration in
   ; agreement and avoids rebuilding the dynamic shape check for every helper.
   xs.len
}

@inline
@jit
@returns_owned
fn _iter_finish_like(any xs, list out, int n) any {
   _list_finish(out, n)
   if is_tuple(xs) { core_prim.list_as_tuple_raw(out) }
   out
}

@inline
@jit
@returns_owned
fn _iter_empty_like(any xs) any {
   if is_str(xs) { return "" }
   mut out = list(0)
   if is_tuple(xs) { core_prim.list_as_tuple_raw(out) }
   out
}

fn any(seq xs, fnptr pred) bool {
   "Returns true when any item in `xs` matches `pred`."
   def iter_predicate = __mark_dynamic_callable_tagged_args(pred)
   def n = _iter_seq_len(xs, "any")
   mut i = 0
   while i < n {
      if iter_predicate(__tbuf_index_any_raw(xs, i)) { return true }
      i += 1
   }
   false
}

fn all(seq xs, fnptr pred) bool {
   "Returns true when every item in `xs` matches `pred`."
   def iter_predicate = __mark_dynamic_callable_tagged_args(pred)
   def n = _iter_seq_len(xs, "all")
   mut i = 0
   while i < n {
      if !iter_predicate(__tbuf_index_any_raw(xs, i)) { return false }
      i += 1
   }
   true
}

fn fold(seq xs, any init, fnptr fn2) any {
   "Reduces `xs` left-to-right starting from `init`."
   mut acc = init
   def n = _iter_seq_len(xs, "fold")
   mut i = 0
   while i < n {
      acc = fn2(acc, __tbuf_index_any_raw(xs, i))
      i += 1
   }
   acc
}

fn find_if(seq xs, fnptr pred, any default=0) any {
   "Returns the first value in `xs` that matches `pred`."
   def n = _iter_seq_len(xs, "find_if")
   mut i = 0
   while i < n {
      ; Predicates are dynamic callables.  Decode native sequence slots at
      ; this boundary so odd scalar payloads and nested handles keep their
      ; canonical any representation.
      def v = __tbuf_index_any_raw(xs, i)
      if pred(v) { return v }
      i += 1
   }
   default
}

fn find_index_if(seq xs, fnptr pred) int {
   "Returns the index of the first item in `xs` that matches `pred`."
   def n = _iter_seq_len(xs, "find_index_if")
   mut i = 0
   while i < n {
      if pred(__tbuf_index_any_raw(xs, i)) { return i }
      i += 1
   }
   -1
}

fn reduce(seq xs, any init, fnptr fn2) any {
   "Alias for fold; reduces `xs` left-to-right starting from `init`."
   fold(xs, init, fn2)
}

fn sum(seq xs, any start=0) any {
   "Returns the sum of all elements in `xs`, optionally starting with `start`."
   mut total = start
   def n = _iter_seq_len(xs, "sum")
   mut i = 0
   while i < n {
      ; Read a canonical any value so odd raw integer slots are tagged exactly
      ; once before dynamic addition. This direct loop has no callback ABI.
      total = total + __tbuf_index_any(xs, i)
      i += 1
   }
   total
}

fn each(seq xs, fnptr fn1) any {
   "Calls `fn1` for each value in `xs` and returns `xs`."
   def n = _iter_seq_len(xs, "each")
   mut i = 0
   while i < n {
      fn1(__tbuf_index_any_raw(xs, i))
      i += 1
   }
   xs
}

fn count(seq xs) int {
   "Returns the number of items in a sequence."
   _iter_seq_len(xs, "count")
}

fn count_if(seq xs, fnptr pred) int {
   "Counts values matching `pred`."
   def n = _iter_seq_len(xs, "count_if")
   mut total = 0
   mut i = 0
   while i < n {
      if pred(__tbuf_index_any_raw(xs, i)) { total += 1 }
      i += 1
   }
   total
}

fn first(seq xs, any default=0) any {
   "Returns the first item in `xs`, or `default` for an empty sequence."
   def n = _iter_seq_len(xs, "first")
   if n <= 0 { return default }
   __tbuf_index_any_raw(xs, 0)
}

fn last(seq xs, any default=0) any {
   "Returns the last item in `xs`, or `default` for an empty sequence."
   def n = _iter_seq_len(xs, "last")
   if n <= 0 { return default }
   __tbuf_index_any_raw(xs, n - 1)
}

@returns_owned
fn chain(seq xs, seq ys) any {
   "Concatenates two sequences."
   if is_str(xs) && is_str(ys) { return xs + ys }
   def n, m = _iter_seq_len(xs, "chain"), _iter_seq_len(ys, "chain")
   def want_tuple = is_tuple(xs) && is_tuple(ys)
   if n == 0 {
      if want_tuple { return clone(ys) }
      if is_list(ys) { return clone(ys) }
   }
   if m == 0 {
      if want_tuple { return clone(xs) }
      if is_list(xs) { return clone(xs) }
   }
   ; Build with append, not preallocated raw stores: raw indexed stores
   ; combined with runtime-call element reads miscompile when the loop
   ; index and the read value are simultaneously live (wrong slots and a
   ; corrupted count).  Append performs the canonical value normalization
   ; per element, which also keeps heterogeneous inputs correct.
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(__tbuf_index_any_raw(xs, i))
      i += 1
   }
   mut j = 0
   while j < m {
      out = out.append(__tbuf_index_any_raw(ys, j))
      j += 1
   }
   if want_tuple { core_prim.list_as_tuple_raw(out) }
   out
}

@returns_owned
fn flatten(seq xss) list {
   "Flattens one level of nested sequence values."
   mut total = 0
   def n = _iter_seq_len(xss, "flatten")
   mut i = 0
   while i < n {
      def inner = __tbuf_index_any_raw(xss, i)
      if _iter_is_seq(inner) { total += inner.len }
      else { total += 1 }
      i += 1
   }
   ; Values read from heterogeneous sequence families may be either raw
   ; element slots or tagged dynamic values. Append performs the canonical
   ; value normalization; a preallocated raw store cannot distinguish 1 from
   ; tagged zero without carrying an element descriptor.
   mut out = []
   mut pos = 0
   i = 0
   while i < n {
      def inner = __tbuf_index_any_raw(xss, i)
      if _iter_is_seq(inner) {
         def m = inner.len
         mut j = 0
         while j < m {
            out = out.append(__tbuf_index_any_raw(inner, j))
            j += 1
         }
      } else {
         out = out.append(inner)
      }
      i += 1
   }
   out
}

@returns_owned
fn filter_map(seq xs, fnptr fn1) list {
   "Maps `xs` and keeps every non-nil result. Return nil from the mapper to skip."
   def n = _iter_seq_len(xs, "filter_map")
   mut out = list(n)
   mut i = 0
   mut pos = 0
   while i < n {
      def res = fn1(__tbuf_index_any_raw(xs, i))
      if res != nil {
         _list_set(out, pos, res)
         pos += 1
      }
      i += 1
   }
   _list_finish(out, pos)
}

@returns_owned
fn compact(seq xs) list {
   "Returns truthy values from `xs`."
   def n = _iter_seq_len(xs, "compact")
   mut out = []
   mut i = 0
   while i < n {
      ; Canonicalize the raw element slot before applying ordinary truthiness.
      def v = __load_item_any(xs, i)
      if is_truthy(v) { out = out.append(v) }
      i += 1
   }
   out
}

@returns_owned
fn mapcat(fnptr fn1, seq xs) list {
   "Maps `xs` and concatenates sequence results."
   def n = _iter_seq_len(xs, "mapcat")
   mut mapped = []
   mut total = 0
   mut i = 0
   while i < n {
      def r = fn1(__tbuf_index_any_raw(xs, i))
      mapped = mapped.append(r)
      if _iter_is_seq(r) { total += r.len } else { total += 1 }
      i += 1
   }
   mut out = []
   i = 0
   while i < n {
      def r = __tbuf_index_any_raw(mapped, i)
      if _iter_is_seq(r) {
         def m = r.len
         mut j = 0
         while j < m {
            ; Keep the canonical dynamic element returned by the typed-buffer
            ; bridge.  mapcat is intentionally heterogeneous: an element may
            ; be an integer, string, list, or another handle.  Decoding every
            ; element as i64 corrupts odd tagged integers and treats handles as
            ; addresses, which only becomes visible with nested flattening.
            out = out.append(__tbuf_index_any_raw(r, j))
            j += 1
         }
      } else {
         out = out.append(r)
      }
      i += 1
   }
   out
}

@returns_owned
fn zip_with(seq a, seq b, fnptr fn2) list {
   "Combines two sequences item-by-item with `fn2`."
   mut n = _iter_seq_len(a, "zip_with")
   def m = _iter_seq_len(b, "zip_with")
   if m < n { n = m }
   mut out = list(n)
   mut i = 0
   while i < n {
      _list_set(out, i, fn2(__tbuf_index_any_raw(a, i),
            __tbuf_index_any_raw(b, i)))
      i += 1
   }
   _list_finish(out, n)
}

@returns_owned
fn chunk(seq xs, int size) list {
   "Splits `xs` into non-overlapping chunks of at most `size`."
   def n = _iter_seq_len(xs, "chunk")
   if size <= 0 || n <= 0 { return list(0) }
   mut out = []
   mut i = 0
   if is_str(xs) {
      while i < n {
         mut stop = i + size
         if stop > n { stop = n }
         mut part = ""
         mut j = i
         while j < stop {
            part = part + __tbuf_index_any_raw(xs, j)
            j += 1
         }
         out = out.append(part)
         i = stop
      }
      return out
   }
   while i < n {
      mut stop = i + size
      if stop > n { stop = n }
      ; Keep bounds in the native integer ABI: an odd raw stop crossing the
      ; generic `any` slice boundary can look tagged (stop=5 becomes 2).
      ; Materialize the chunk directly and preserve nested values.
      mut part = list(stop - i)
      mut j = i
      while j < stop {
         ; `__tbuf_index_any_raw` takes the native index directly.  The old
         ; `__load_item_any(..., j * 2 + 1)` spelling mixed the tagged public
         ; ABI with this raw loop index, so chunking read the next element and
         ; eventually probed past the end of the source list.
         _list_set(part, j - i, __tbuf_index_any_raw(xs, j))
         j += 1
      }
      _list_finish(part, stop - i)
      out = out.append(part)
      i = stop
   }
   out
}

@returns_owned
fn windowed(seq xs, int size, int step=1) list {
   "Returns sliding windows of length `size` from `xs`."
   def n = _iter_seq_len(xs, "windowed")
   if size <= 0 || n <= 0 { return list(0) }
   if step <= 0 { step = 1 }
   mut out = []
   mut i = 0
   if is_str(xs) {
      while i + size <= n {
         mut part = ""
         mut j = 0
         while j < size {
            part = part + __tbuf_index_any_raw(xs, i + j)
            j += 1
         }
         out = out.append(part)
         i += step
      }
      return out
   }
   while i + size <= n {
      mut part = []
      mut j = 0
      while j < size {
         part = part.append(__tbuf_index_any_raw(xs, i + j))
         j += 1
      }
      out = out.append(part)
      i += step
   }
   out
}

@returns_owned
fn cycle(seq xs, int count) list {
   "Repeats `xs` `count` times into a new list."
   def n = _iter_seq_len(xs, "cycle")
   if count <= 0 || n == 0 { return list(0) }
   mut out = list(n * count)
   mut i = 0
   while i < count {
      mut j = 0
      while j < n {
         _list_set(out, i * n + j, __tbuf_index_any_raw(xs, j))
         j += 1
      }
      i += 1
   }
   _list_finish(out, n * count)
}

@returns_owned
fn partition(seq xs, fnptr pred) any {
   "Splits `xs` into matching and non-matching values."
   def n = _iter_seq_len(xs, "partition")
   mut t, f = list(n), list(n)
   mut ti, fi = 0, 0
   mut i = 0
   while i < n {
      ; Predicates are dynamic callables. Decode native sequence slots at
      ; this boundary so odd scalar payloads and nested handles keep their
      ; canonical any representation.
      def v = __tbuf_index_any_raw(xs, i)
      if pred(v) {
         _list_set(t, ti, v)
         ti += 1
      } else {
         _list_set(f, fi, v)
         fi += 1
      }
      i += 1
   }
   [ _list_finish(t, ti), _list_finish(f, fi) ]
}

@jit
@inline
fn _list_set(list out, int idx, any value) any {
   ; Keep list writes on the same raw-index/value bridge as `__store_item`.
   ; The low-level store64 spelling has no dynamic-slot normalization and
   ; corrupts odd integers and nested values in JIT/native lowering.
   ; Scalar callback results are canonical tagged values; the raw list store
   ; needs their payload, while containers/strings pass through unchanged.
   __store_item(out, idx, __any_to_i64(value))
}

@jit
@inline
@returns_owned
fn _list_finish(list out, int len) list {
   __list_set_len(out, len)
   out
}

@returns_owned
fn range2(int start, int stop, int step=1) range {
   "Returns a range object from `start` to `stop` using `step`."
   mut st = step
   if st == 0 { st = 1 }
   mut obj = primitives.range_new_raw(start, stop, st)
   if !obj { panic("range allocation failed") }
   obj
}

@returns_owned
fn range(...args) range {
   "Returns `range(0, stop)`, `range(start, stop)`, or `range(start, stop, step)`."
   def n = args.len
   if n == 1 { return range2(0, args[0], 1) }
   if n == 2 { return range2(args[0], args[1], 1) }
   if n == 3 { return range2(args[0], args[1], args[2]) }
   panic("range expects 1, 2, or 3 argument(s)")
}

@returns_owned
fn enumerate(seq xs, int start=0) list {
   "Returns `[index, value]` pairs starting at `start`."
   def n = _iter_seq_len(xs, "enumerate")
   ;; Build nested pairs through append so native buffers preserve nested
   ;; value descriptors.
   mut out = list(0)
   mut i = 0
   while i < n {
      out = append(out, [start + i, __tbuf_index_any_raw(xs, i)])
      i += 1
   }
   out
}

@returns_owned
fn map(seq xs, fnptr fn1) any {
   "Applies `fn1` to each item in `xs`."
   def n = _iter_seq_len(xs, "map")
   if n == 0 { return _iter_empty_like(xs) }
   if is_str(xs) {
      def cp_n = _iter_utf8_len(xs)
      mut out = strmod.Builder(n * 2 + 8)
      mut i = 0
      while i < cp_n {
         def mapped = fn1(strmod.chr(_iter_ord_at(xs, i)))
         ; builder_append already formats non-string callback results.  Keep
         ; managed strings on the canonical path instead of routing them
         ; through the raw C-string compatibility conversion.
         out = strmod.builder_append(out, mapped)
         i += 1
      }
      def s = strmod.builder_to_str(out)
      strmod.builder_free(out)
      return s
   }
   mut out = list(n)
   mut i = 0
   while i < n {
      ; Dynamic callbacks receive canonical values; ranges store raw indices.
      _list_set(out, i, fn1(__tbuf_index_any_raw(xs, i)))
      i += 1
   }
   _iter_finish_like(xs, out, n)
}

@returns_owned
fn filter(seq xs, fnptr pred) any {
   "Keeps the items in `xs` that match `pred`."
   def n = _iter_seq_len(xs, "filter")
   if n == 0 { return _iter_empty_like(xs) }
   if is_str(xs) {
      def cp_n = _iter_utf8_len(xs)
      mut out = strmod.Builder(n + 8)
      mut i = 0
      while i < cp_n {
         def v = strmod.chr(_iter_ord_at(xs, i))
         if pred(v) { out = strmod.builder_append(out, v) }
         i += 1
      }
      def s = strmod.builder_to_str(out)
      strmod.builder_free(out)
      return s
   }
   mut out = list(n)
   mut i = 0
   mut idx = 0
   while i < n {
      ; Cross the sequence boundary through the canonical decoder. Direct
      ; raw loads can turn an odd native container handle into an integer when
      ; it is passed through an indirect callback.
      def v = __tbuf_index_any_raw(xs, i)
      if pred(v) {
         _list_set(out, idx, v)
         idx += 1
      }
      i += 1
   }
   _iter_finish_like(xs, out, idx)
}

@returns_owned
fn repeat(any value, int count) list {
   "Returns a list containing `value` repeated `count` times."
   if count <= 0 { return list(0) }
   mut out = list(count)
   mut i = 0
   while i < count {
      _list_set(out, i, value)
      i += 1
   }
   _list_finish(out, count)
}

@returns_owned
fn take(seq xs, int count) any {
   "Returns the first `count` items from `xs`."
   if count <= 0 {
      if is_str(xs) { return "" }
      return list(0)
   }
   def n = _iter_seq_len(xs, "take")
   mut lim = count
   if lim > n { lim = n }
   if is_str(xs) { return _iter_utf8_slice(xs, 0, lim, 1) }
   mut out = list(lim)
   mut i = 0
   while i < lim {
      _list_set(out, i, __tbuf_index_any_raw(xs, i))
      i += 1
   }
   _iter_finish_like(xs, out, lim)
}

@returns_owned
fn drop(seq xs, int count) any {
   "Returns `xs` without its first `count` items."
   def n = _iter_seq_len(xs, "drop")
   if count <= 0 {
      return clone(xs)
   }
   if count >= n { return _iter_empty_like(xs) }
   if is_str(xs) { return _iter_utf8_slice(xs, count, n, 1) }
   def out_n = n - count
   mut out = list(out_n)
   mut i = 0
   while i < out_n {
      _list_set(out, i, __tbuf_index_any_raw(xs, count + i))
      i += 1
   }
   _iter_finish_like(xs, out, out_n)
}

@returns_owned
fn reverse(seq xs) any {
   "Returns a reversed copy of `xs`."
   def n = _iter_seq_len(xs, "reverse")
   if n == 0 { return _iter_empty_like(xs) }
   if n == 1 {
      if is_str(xs) { return xs }
      return clone(xs)
   }
   if is_str(xs) {
      def cp_n = _iter_utf8_len(xs)
      mut out = strmod.Builder(n + 8)
      mut i = cp_n - 1
      while i >= 0 {
         out = strmod.builder_append(out, strmod.chr(_iter_ord_at(xs, i)))
         i -= 1
      }
      def s = strmod.builder_to_str(out)
      strmod.builder_free(out)
      return s
   }
   mut out = list(n)
   mut i = 0
   while i < n {
      _list_set(out, i, __tbuf_index_any_raw(xs, n - 1 - i))
      i += 1
   }
   _iter_finish_like(xs, out, n)
}

@returns_owned
fn zip2(seq a, seq b) list {
   "Returns `[a[i], b[i]]` pairs up to the shorter input."
   mut n = _iter_seq_len(a, "zip2")
   def m = _iter_seq_len(b, "zip2")
   if m < n { n = m }
   if n <= 0 { return list(0) }
   mut out = list(n)
   mut i = 0
   while i < n {
      _list_set(out, i, [__tbuf_index_any_raw(a, i),
            __tbuf_index_any_raw(b, i)])
      i += 1
   }
   _list_finish(out, n)
}

#main {
   assert(range(5) == [0, 1, 2, 3, 4], "iter range stop")
   assert(range(2, 6) == [2, 3, 4, 5], "iter range start stop")
   assert(range(6, 2, -2) == [6, 4], "iter range negative")
   assert(enumerate(["a", "b"], 10) == [[10, "a"], [11, "b"]], "iter enumerate")
   assert(map([1, 2, 3], fn(v) { v * v }) == [1, 4, 9], "iter map list")
   assert(map("ab", fn(v) { v + "!" }) == "a!b!", "iter map str")
   assert(map((1, 2, 3), fn(v) { v + 1 }) == (2, 3, 4), "iter map tuple")
   assert(filter([1, 2, 3, 4], fn(v) { (v % 2) == 0 }) == [2, 4], "iter filter list")
   assert(filter((1, 2, 3, 4), fn(v) { (v % 2) == 0 }) == (2, 4), "iter filter tuple")
   assert(take("abcd", 2) == "ab", "iter take str")
   assert(drop([9, 8, 7], 1) == [8, 7], "iter drop list")
   assert(reverse((1, 2, 3)) == (3, 2, 1), "iter reverse tuple")
   assert(zip2(["a", "b", "c"], [1, 2]) == [["a", 1], ["b", 2]], "iter zip2")
   assert(any([1, 2, 3], fn(v) { v > 2 }), "iter any")
   assert(!all([4, 5, 6], fn(v) { v > 5 }), "iter all false")
   assert(fold([1, 2, 3, 4], 0, fn(a, v) { a + v }) == 10, "iter fold")
   assert(reduce([1, 2, 3], 1, fn(a, v) { a * v }) == 6, "iter reduce")
   assert(sum([1, 2, 3, 4]) == 10, "iter sum")
   assert(sum([1, 2], 5) == 8, "iter sum start")
   assert(find_if([10, 20, 30], fn(v) { v > 15 }) == 20, "iter find_if")
   assert(chain("ab", "cd") == "abcd", "iter chain str")
   assert(flatten([[1, 2], 3, [4]]) == [1, 2, 3, 4], "iter flatten")
   assert(filter_map([1, 2, 3, 4], fn(v) any { if (v % 2) == 0 { return v * 10 } nil }) == [20, 40], "iter filter_map")
   assert(reverse("éa") == "aé", "iter reverse utf8")
   assert(compact([0, 1, "", "x", nil, 4]) == [1, "x", 4], "iter compact")
   assert(zip_with([1, 2], [10, 20], fn(a, b) { a + b }) == [11, 22], "iter zip_with")
   assert(cycle([1, 2], 2) == [1, 2, 1, 2], "iter cycle")
   assert(chunk("abcde", 2) == ["ab", "cd", "e"], "iter chunk")
   assert(windowed([1, 2, 3, 4], 3) == [[1, 2, 3], [2, 3, 4]], "iter windowed")
   def p = partition([1, 2, 3, 4], fn(v) { v > 2 })
   assert(p.get(0) == [3, 4] && p.get(1) == [1, 2], "iter partition")
   print("✓ core.iter self-test passed")
}
