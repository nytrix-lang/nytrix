use std.core

def xs = [1, 2, 3]
assert(xs.len == 3, "gc list length")
assert(xs.get(1) == 2, "gc list element")
def d = {"name": "ny", "ok": true}
assert(d.get("name") == "ny", "gc dict string")
assert(d.get("ok") == true, "gc dict bool")
def text = "ny" + "trix"
assert(text == "nytrix", "gc string concat")
