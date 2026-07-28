;; Regression coverage for nil value semantics.
use std.core

fn identity(any v) any { v }
fn classify(any v) str {
  if is_nil(v) { "nil" }
  elif is_int(v) { "int" }
  else { "other" }
}

assert(is_nil(nil), "direct is_nil(nil)")
assert(!is_nil(0), "integer 0 is not nil")
assert(nil != 0, "nil is distinct from integer 0")
assert(!(nil == 0), "nil does not equal integer 0")
assert(0 != nil, "integer 0 is distinct from nil")
assert(!(0 == nil), "integer 0 does not equal nil")
def nil_alias = nil
assert(nil_alias != 0 && !(nil_alias == 0), "bound nil keeps identity")
assert(nil == nil, "nil equals nil")
assert(to_str(nil) == "nil", "to_str(nil)")
assert(type(nil) == "nil", "type(nil)")

if nil { assert(false, "nil must be falsy") }
if 0 { assert(false, "integer 0 may remain falsy") }
assert(nil ?? 99 == 99, "nil coalesces to fallback")

assert(classify(identity(nil)) == "nil", "nil survives any")
assert(classify(identity(0)) == "int", "integer 0 survives any")

mut values = {"_": 0}
values.set("missing", nil)
values.set("zero", 0)
assert(is_nil(values.get("missing")), "nil survives dictionary values")
assert(is_int(values.get("zero")), "integer 0 survives dictionary values")

mut keys = {"_": 0}
keys.delete("_")
keys.set(nil, "missing")
keys.set(0, "zero")
assert(keys.len == 2, "nil and integer 0 are distinct dictionary keys")
assert(keys.get(nil) == "missing", "nil dictionary key")
assert(keys.get(0) == "zero", "integer 0 dictionary key")
assert(hash(nil) != hash(0), "nil and integer 0 have distinct hashes")

fn return_nil() any { nil }
assert(is_nil(return_nil()), "returned nil stays nil")

print("✓ nil tests passed")
