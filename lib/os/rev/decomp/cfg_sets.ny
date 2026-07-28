;; Keywords: sets lists intersection difference dominance
;; Set-shaped list operations used by CFG dominance analysis.
module std.os.rev.decomp.cfg_sets(_set_intersection, _same_set, _list_without, _set_difference)

use std.core
use std.os.rev.decomp.collections (_list_has, _append_unique)

fn _set_intersection(list a, list b) list {
   mut out = []
   mut i = 0
   while i < a.len {
      if _list_has(b, a[i]) { out = _append_unique(out, a[i]) }
      i += 1
   }
   out
}

fn _same_set(list a, list b) bool {
   if a.len != b.len { return false }
   mut i = 0
   while i < a.len {
      if !_list_has(b, a[i]) { return false }
      i += 1
   }
   true
}

fn _list_without(list xs, any value) list {
   mut out = []
   mut i = 0
   while i < xs.len {
      if xs[i] != value { out = out.append(xs[i]) }
      i += 1
   }
   out
}

fn _set_difference(list a, list b) list {
   mut out = []
   mut i = 0
   while i < a.len {
      if !_list_has(b, a[i]) { out = _append_unique(out, a[i]) }
      i += 1
   }
   out
}

#main {
   assert(_same_set(_set_intersection([1, 2, 2], [2, 3]), [2]), "set intersection")
   assert(_set_difference([1, 2], [2]).get(0) == 1, "set difference")
   print("✓ std.os.rev.decomp.cfg_sets self-test passed")
}
