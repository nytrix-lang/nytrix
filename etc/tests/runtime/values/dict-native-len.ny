fn typed_len(dict d) int { d.len }
fn dynamic_len(any d) int { d.len }

def d = dict_set(dict(4), "a", 1)
assert(d.len == 1, "native dictionary property length")
assert(typed_len(d) == 1, "typed native dictionary property length")
assert(dynamic_len(d) == 1, "dynamic native dictionary property length")
assert(dict_len(d) == 1, "dictionary helper length")
assert(dict(4).len == 0, "empty native dictionary property length")
