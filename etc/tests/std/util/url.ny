use std.io
use std.util.url
use std.core.reflect
use std.core.test
use std.strings.str
use std.collections

print("Testing URL...")
def u = url_parse("http://user:pass@example.com:8080/path?query=1#frag")
assert(eq(get(u, 0), "http"), "scheme")
assert(eq(get(u, 1), "user:pass"), "auth")

assert(eq(get(u, 2), "example.com"), "host")
assert(get(u, 3) == 8080, "port")
assert(eq(get(u, 4), "/path"), "path")
assert(eq(get(u, 5), "query=1"), "query")
assert(eq(get(u, 6), "frag"), "fragment")
print("✓ std.util.url passed")
