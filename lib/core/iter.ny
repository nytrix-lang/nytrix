;; Keywords: iter
;; Iterator and sequence operations for map, filter, reduce, chunking, and traversal.
module std.core.iter(range, range2, enumerate, map, filter, repeat, take, drop, reverse, zip2, any, all, fold, reduce, each, count, count_if, first, last, find_if, find_index_if, chain, flatten, filter_map, compact, zip_with, cycle, partition, chunk, windowed, mapcat)
use std.core
use std.core.primitives as prim

@inline
@jit
fn _iter_is_seq(any: x): bool {
   is_list(x) || is_tuple(x) || is_str(x) || is_bytes(x) || is_range(x)
}

@inline
@jit
fn _iter_seq_len(seq: xs, str: fn_name): int {
   if(_iter_is_seq(xs)){ return xs.len }
   panic("expected sequence")
}

@inline
@jit
@returns_owned
fn _iter_finish_like(any: xs, list: out, int: n): any {
   _list_finish(out, n)
   if(is_tuple(xs)){ prim.list_as_tuple_raw(out) }
   out
}

@inline
@jit
@returns_owned
fn _iter_empty_like(any: xs): any {
   if(is_str(xs)){ return "" }
   mut out = list(0)
   if(is_tuple(xs)){ prim.list_as_tuple_raw(out) }
   out
}

fn any(seq: xs, fnptr: pred): bool {
   "Returns true when any item in `xs` matches `pred`."
   def n = _iter_seq_len(xs, "any")
   mut i = 0
   while(i < n){
      if(pred(xs.get(i))){ return true }
      i += 1
   }
   false
}

fn all(seq: xs, fnptr: pred): bool {
   "Returns true when every item in `xs` matches `pred`."
   def n = _iter_seq_len(xs, "all")
   mut i = 0
   while(i < n){
      if(!pred(xs.get(i))){ return false }
      i += 1
   }
   true
}

fn fold(seq: xs, any: init, fnptr: fn2): any {
   "Reduces `xs` left-to-right starting from `init`."
   mut acc = init
   def n = _iter_seq_len(xs, "fold")
   mut i = 0
   while(i < n){
      acc = fn2(acc, xs.get(i))
      i += 1
   }
   acc
}

fn find_if(seq: xs, fnptr: pred, any: default=0): any {
   "Returns the first value in `xs` that matches `pred`."
   def n = _iter_seq_len(xs, "find_if")
   mut i = 0
   while(i < n){
      def v = xs.get(i)
      if(pred(v)){ return v }
      i += 1
   }
   default
}

fn find_index_if(seq: xs, fnptr: pred): int {
   "Returns the index of the first item in `xs` that matches `pred`."
   def n = _iter_seq_len(xs, "find_index_if")
   mut i = 0
   while(i < n){
      if(pred(xs.get(i))){ return i }
      i += 1
   }
   -1
}

fn reduce(seq: xs, any: init, fnptr: fn2): any {
   "Alias for fold; reduces `xs` left-to-right starting from `init`."
   fold(xs, init, fn2)
}

fn each(seq: xs, fnptr: fn1): any {
   "Calls `fn1` for each value in `xs` and returns `xs`."
   def n = _iter_seq_len(xs, "each")
   mut i = 0
   while(i < n){
      fn1(xs.get(i))
      i += 1
   }
   xs
}

fn count(seq: xs): int {
   "Returns the number of items in a sequence."
   _iter_seq_len(xs, "count")
}

fn count_if(seq: xs, fnptr: pred): int {
   "Counts values matching `pred`."
   def n = _iter_seq_len(xs, "count_if")
   mut total = 0
   mut i = 0
   while(i < n){
      if(pred(xs.get(i))){ total += 1 }
      i += 1
   }
   total
}

fn first(seq: xs, any: default=0): any {
   "Returns the first item in `xs`, or `default` for an empty sequence."
   def n = _iter_seq_len(xs, "first")
   if(n <= 0){ return default }
   xs.get(0, default)
}

fn last(seq: xs, any: default=0): any {
   "Returns the last item in `xs`, or `default` for an empty sequence."
   def n = _iter_seq_len(xs, "last")
   if(n <= 0){ return default }
   xs.get(n - 1, default)
}

@returns_owned
fn chain(seq: xs, seq: ys): any {
   "Concatenates two sequences."
   if(is_str(xs) && is_str(ys)){ return xs + ys }
   def n, m = _iter_seq_len(xs, "chain"), _iter_seq_len(ys, "chain")
   def want_tuple = is_tuple(xs) && is_tuple(ys)
   if(n == 0){
      if(want_tuple){ return clone(ys) }
      if(is_list(ys)){ return clone(ys) }
   }
   if(m == 0){
      if(want_tuple){ return clone(xs) }
      if(is_list(xs)){ return clone(xs) }
   }
   mut out = list(n + m)
   mut i = 0
   while(i < n){
      _list_set(out, i, xs.get(i))
      i += 1
   }
   mut j = 0
   while(j < m){
      _list_set(out, n + j, ys.get(j))
      j += 1
   }
   _list_finish(out, n + m)
   if(want_tuple){ prim.list_as_tuple_raw(out) }
   out
}

@returns_owned
fn flatten(seq: xss): list {
   "Flattens one level of nested sequence values."
   mut total = 0
   def n = _iter_seq_len(xss, "flatten")
   mut i = 0
   while(i < n){
      def inner = xss.get(i)
      if(_iter_is_seq(inner)){ total += inner.len }
      else { total += 1 }
      i += 1
   }
   mut out = list(total)
   mut pos = 0
   i = 0
   while(i < n){
      def inner = xss.get(i)
      if(_iter_is_seq(inner)){
         def m = inner.len
         mut j = 0
         while(j < m){
            _list_set(out, pos, inner.get(j))
            pos += 1
            j += 1
         }
      } else {
         _list_set(out, pos, inner)
         pos += 1
      }
      i += 1
   }
   _list_finish(out, total)
}

@returns_owned
fn filter_map(seq: xs, fnptr: fn1): list {
   "Maps `xs` and keeps only truthy results."
   def n = _iter_seq_len(xs, "filter_map")
   mut out = list(n)
   mut i = 0
   mut pos = 0
   while(i < n){
      def res = fn1(xs.get(i))
      if(res){
         _list_set(out, pos, res)
         pos += 1
      }
      i += 1
   }
   _list_finish(out, pos)
}

@returns_owned
fn compact(seq: xs): list {
   "Returns truthy values from `xs`."
   def n = _iter_seq_len(xs, "compact")
   mut out = list(n)
   mut i = 0
   mut pos = 0
   while(i < n){
      def v = xs.get(i)
      if(is_truthy(v)){
         _list_set(out, pos, v)
         pos += 1
      }
      i += 1
   }
   _list_finish(out, pos)
}

@returns_owned
fn mapcat(fnptr: fn1, seq: xs): list {
   "Maps `xs` and concatenates sequence results."
   def n = _iter_seq_len(xs, "mapcat")
   mut mapped = list(n)
   mut total = 0
   mut i = 0
   while(i < n){
      def r = fn1(xs.get(i))
      _list_set(mapped, i, r)
      if(_iter_is_seq(r)){ total += r.len } else { total += 1 }
      i += 1
   }
   _list_finish(mapped, n)
   mut out = list(total)
   mut pos = 0
   i = 0
   while(i < n){
      def r = mapped.get(i)
      if(_iter_is_seq(r)){
         def m = r.len
         mut j = 0
         while(j < m){
            _list_set(out, pos, r.get(j))
            pos += 1
            j += 1
         }
      } else {
         _list_set(out, pos, r)
         pos += 1
      }
      i += 1
   }
   _list_finish(out, total)
}

@returns_owned
fn zip_with(seq: a, seq: b, fnptr: fn2): list {
   "Combines two sequences item-by-item with `fn2`."
   mut n = _iter_seq_len(a, "zip_with")
   def m = _iter_seq_len(b, "zip_with")
   if(m < n){ n = m }
   mut out = list(n)
   mut i = 0
   while(i < n){
      _list_set(out, i, fn2(a.get(i), b.get(i)))
      i += 1
   }
   _list_finish(out, n)
}

@returns_owned
fn chunk(seq: xs, int: size): list {
   "Splits `xs` into non-overlapping chunks of at most `size`."
   def n = _iter_seq_len(xs, "chunk")
   if(size <= 0 || n <= 0){ return list(0) }
   mut out = list((n + size - 1) / size)
   mut pos = 0
   mut i = 0
   while(i < n){
      mut stop = i + size
      if(stop > n){ stop = n }
      _list_set(out, pos, slice(xs, i, stop, 1))
      pos += 1
      i = stop
   }
   _list_finish(out, pos)
}

@returns_owned
fn windowed(seq: xs, int: size, int: step=1): list {
   "Returns sliding windows of length `size` from `xs`."
   def n = _iter_seq_len(xs, "windowed")
   if(size <= 0 || n <= 0){ return list(0) }
   if(step <= 0){ step = 1 }
   mut out = list(n)
   mut pos = 0
   mut i = 0
   while(i + size <= n){
      _list_set(out, pos, slice(xs, i, i + size, 1))
      pos += 1
      i += step
   }
   _list_finish(out, pos)
}

@returns_owned
fn cycle(seq: xs, int: count): list {
   "Repeats `xs` `count` times into a new list."
   def n = _iter_seq_len(xs, "cycle")
   if(count <= 0 || n == 0){ return list(0) }
   mut out = list(n * count)
   mut i = 0
   while(i < count){
      mut j = 0
      while(j < n){
         _list_set(out, i * n + j, xs.get(j))
         j += 1
      }
      i += 1
   }
   _list_finish(out, n * count)
}

@returns_owned
fn partition(seq: xs, fnptr: pred): list {
   "Splits `xs` into matching and non-matching values."
   def n = _iter_seq_len(xs, "partition")
   mut t, f = list(n), list(n)
   mut ti, fi = 0, 0
   mut i = 0
   while(i < n){
      def v = xs.get(i)
      if(pred(v)){
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
fn _list_set(list: out, int: idx, any: value): any { store64(out, value, 16 + idx * 8) }

@jit
@inline
@returns_owned
fn _list_finish(list: out, int: len): list {
   store64(out, len, 0)
   out
}

@returns_owned
fn range2(int: start, int: stop, int: step=1): range {
   "Returns a range object from `start` to `stop` using `step`."
   mut st = step
   if(st == 0){ st = 1 }
   mut obj = prim.range_new_raw(start, stop, st)
   if(!obj){ panic("range allocation failed") }
   obj
}

@returns_owned
fn range(...args): range {
   "Returns `range(0, stop)`, `range(start, stop)`, or `range(start, stop, step)`."
   def n = args.len
   if(n == 1){ return range2(0, args.get(0), 1) }
   if(n == 2){ return range2(args.get(0), args.get(1), 1) }
   if(n == 3){ return range2(args.get(0), args.get(1), args.get(2)) }
   panic("range expects 1, 2, or 3 argument(s)")
}

@returns_owned
fn enumerate(seq: xs, int: start=0): list {
   "Returns `[index, value]` pairs starting at `start`."
   def n = _iter_seq_len(xs, "enumerate")
   mut out = list(n)
   mut i = 0
   while(i < n){
      _list_set(out, i, [start + i, xs.get(i)])
      i += 1
   }
   _list_finish(out, n)
}

@returns_owned
fn map(seq: xs, fnptr: fn1): any {
   "Applies `fn1` to each item in `xs`."
   def n = _iter_seq_len(xs, "map")
   if(n == 0){ return _iter_empty_like(xs) }
   if(is_str(xs)){
      use std.core.str
      mut out = Builder(n * 2 + 8)
      mut i = 0
      while(i < n){
         out = builder_append(out, fn1(xs.get(i)))
         i += 1
      }
      def s = builder_to_str(out)
      builder_free(out)
      return s
   }
   mut out = list(n)
   mut i = 0
   while(i < n){
      _list_set(out, i, fn1(xs.get(i)))
      i += 1
   }
   _iter_finish_like(xs, out, n)
}

@returns_owned
fn filter(seq: xs, fnptr: pred): any {
   "Keeps the items in `xs` that match `pred`."
   def n = _iter_seq_len(xs, "filter")
   if(n == 0){ return _iter_empty_like(xs) }
   if(is_str(xs)){
      use std.core.str
      mut out = Builder(n + 8)
      mut i = 0
      while(i < n){
         def v = xs.get(i)
         if(pred(v)){ out = builder_append(out, v) }
         i += 1
      }
      def s = builder_to_str(out)
      builder_free(out)
      return s
   }
   mut out = list(n)
   mut i = 0
   mut idx = 0
   while(i < n){
      def v = xs.get(i)
      if(pred(v)){
         _list_set(out, idx, v)
         idx += 1
      }
      i += 1
   }
   _iter_finish_like(xs, out, idx)
}

@returns_owned
fn repeat(any: value, int: count): list {
   "Returns a list containing `value` repeated `count` times."
   if(count <= 0){ return list(0) }
   mut out = list(count)
   mut i = 0
   while(i < count){
      _list_set(out, i, value)
      i += 1
   }
   _list_finish(out, count)
}

@returns_owned
fn take(seq: xs, int: count): any {
   "Returns the first `count` items from `xs`."
   if(count <= 0){
      if(is_str(xs)){ return "" }
      return list(0)
   }
   def n = _iter_seq_len(xs, "take")
   mut lim = count
   if(lim > n){ lim = n }
   if(is_str(xs)){ return slice(xs, 0, lim) }
   mut out = list(lim)
   mut i = 0
   while(i < lim){
      _list_set(out, i, xs.get(i))
      i += 1
   }
   _iter_finish_like(xs, out, lim)
}

@returns_owned
fn drop(seq: xs, int: count): any {
   "Returns `xs` without its first `count` items."
   def n = _iter_seq_len(xs, "drop")
   if(count <= 0){
      if(is_str(xs)){ return xs }
      if(is_tuple(xs)){
         mut out = list(n)
         mut i = 0
         while(i < n){
            _list_set(out, i, xs.get(i))
            i += 1
         }
         return _iter_finish_like(xs, out, n)
      }
      if(is_list(xs)){
         mut out = list(n)
         mut i = 0
         while(i < n){
            _list_set(out, i, xs.get(i))
            i += 1
         }
         return _list_finish(out, n)
      }
      count = 0
   }
   if(count >= n){ return _iter_empty_like(xs) }
   if(is_str(xs)){ return slice(xs, count, n) }
   def out_n = n - count
   mut out = list(out_n)
   mut i = 0
   while(i < out_n){
      _list_set(out, i, xs.get(count + i))
      i += 1
   }
   _iter_finish_like(xs, out, out_n)
}

@returns_owned
fn reverse(seq: xs): any {
   "Returns a reversed copy of `xs`."
   def n = _iter_seq_len(xs, "reverse")
   if(n == 0){ return _iter_empty_like(xs) }
   if(n == 1){
      if(is_str(xs)){ return xs }
      return clone(xs)
   }
   if(is_str(xs)){
      use std.core.str
      mut out = Builder(n + 8)
      mut i = n - 1
      while(i >= 0){
         out = builder_append(out, xs.get(i))
         i -= 1
      }
      def s = builder_to_str(out)
      builder_free(out)
      return s
   }
   mut out = list(n)
   mut i = 0
   while(i < n){
      _list_set(out, i, xs.get(n - 1 - i))
      i += 1
   }
   _iter_finish_like(xs, out, n)
}

@returns_owned
fn zip2(seq: a, seq: b): list {
   "Returns `[a[i], b[i]]` pairs up to the shorter input."
   mut n = _iter_seq_len(a, "zip2")
   def m = _iter_seq_len(b, "zip2")
   if(m < n){ n = m }
   if(n <= 0){ return list(0) }
   mut out = list(n)
   mut i = 0
   while(i < n){
      _list_set(out, i, [a.get(i), b.get(i)])
      i += 1
   }
   _list_finish(out, n)
}
