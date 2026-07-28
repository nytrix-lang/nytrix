;; Collection semantics: dicts are reference-typed, lists are value-typed.
use std.core

fn update_dict(dict d) dict {
   d.set("value", 2)
   d
}

fn update_list(list xs) list {
   xs = xs.append(2)
   xs = xs.extend([3, 4])
   xs = xs.set(0, 9)
   xs
}

fn update_dict_free(dict d) dict {
   set(d, "free", 1)
   d
}

fn update_index(list xs) list {
   xs[0] = 6
   xs
}

mut empty = {}
empty.set("ready", true)
assert(empty.get("ready") == true, "mutable empty dict literal accepts writes")

mut d = {"value": 1}
mut d_alias = d
d.set("extra", 3)
assert(d.get("extra") == 3, "dict statement mutation writes back")
assert(d_alias.contains("extra"), "dict aliases share identity")

mut d2 = update_dict(d)
assert(d.get("value") == 2, "dict parameter mutates caller (reference-typed)")
assert(d2.get("value") == 2, "dict parameter returns same reference")
mut d3 = update_dict_free(d)
assert(d.contains("free") && d3.get("free") == 1, "free set call mutates shared reference")

mut xs = [1]
mut xs_alias = xs
xs = xs.append(2)
xs = xs.extend([3, 4])
xs = xs.set(0, 9)
assert(xs == [9, 2, 3, 4], "list statement mutators write back")
assert(xs_alias == [1], "list aliases are isolated")
xs[0] = 7
assert(xs[0] == 7 && xs_alias[0] == 1, "list index assignment is isolated")

mut indexed_dict = {"x": 1}
mut indexed_dict_alias = indexed_dict
indexed_dict["x"] = 5
assert(indexed_dict["x"] == 5 && indexed_dict_alias["x"] == 5,
       "dict index assignment mutates shared reference")

mut ys = update_list([1])
assert(ys == [9, 2, 3, 4], "list parameter threads returned copies")
mut indexed = [1]
mut indexed_out = update_index(indexed)
assert(indexed[0] == 6, "indexed parameter update mutates caller")
assert(indexed_out[0] == 6, "indexed parameter returns same reference")

mut merged = {"a": 1}
mut merged_alias = merged
merged.merge({"b": 2})
assert(merged.get("b") == 2 && merged_alias.contains("b"), "dict merge mutates shared reference")
merged.delete("a")
assert(!merged.contains("a") && !merged_alias.contains("a"), "dict delete mutates shared reference")
merged.clear()
assert(merged.len == 0 && merged_alias.len == 0, "dict clear mutates shared reference")

mut cleared = [1, 2]
mut cleared_alias = cleared
cleared = cleared.clear()
assert(cleared.len == 0 && cleared_alias.len == 0, "list clear mutates shared reference")

assert(nil != 0, "nil is distinct from integer zero")
assert(!nil && !0, "nil and integer zero remain falsy")
assert(is_nil(nil) && is_int(0), "nil and integer zero retain distinct types")

print("✓ semantics tests passed")
