;; Keywords: collections lists symbols deduplication
;; Small immutable-list helpers shared by decompiler analysis passes.
module std.os.rev.decomp.collections(_list_has, _append_unique, _append_all_unique, _symbols_intersect)
use std.core

fn _list_has(list xs, any value) bool {
   mut i = 0
   while i < xs.len {
      if xs[i] == value { return true }
      i += 1
   }
   false
}

fn _append_unique(list xs, any value) list {
   if is_str(value) && value.len == 0 { return xs }
   _list_has(xs, value) ? xs : xs.append(value)
}

fn _append_all_unique(list xs, list values) list {
   mut out = xs
   mut i = 0
   while i < values.len { out = _append_unique(out, values[i]) i += 1 }
   out
}

fn _symbols_intersect(list a, list b) bool {
   mut i = 0
   while i < a.len {
      if _list_has(b, a[i]) { return true }
      i += 1
   }
   false
}

#main {
   assert(_append_all_unique(["a"], ["a", "b"]).len == 2 && _symbols_intersect(["x"], ["x", "y"]), "collection helpers")
   print("✓ std.os.rev.decomp.collections self-test passed")
}
