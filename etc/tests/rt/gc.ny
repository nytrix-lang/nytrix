;; flags: -gc
use std.core

def xs = [1, 2, 3]
assert(xs.len == 3, "gc list length")
assert(xs.get(1) == 2, "gc list element")
def d = {"name": "ny", "ok": true}
assert(d.get("name") == "ny", "gc dict string")
assert(d.get("ok") == true, "gc dict bool")
def text = "ny" + "trix"
assert(text == "nytrix", "gc string concat")
mut survivors = []
mut i = 0
while i < 256 {
   def row = [i, i + 1, "ny" + to_str(i)]
   survivors = survivors.append(row)
   i += 1
}

assert(survivors.len == 256, "gc pressure survivors")
assert(survivors[128][2] == "ny128", "gc pressure string survives")
print("✓ gc heap tests passed")
