use std.io
use std.util.url
use std.collections.dict
use std.strings.str
use std.core.error

;; std.util.url (Test)
;; Tests URL parsing and query parsing only.
;; NOTE: urlencode / urldecode return C-strings and are not safe for eq()
;; and may not be available in AOT builds, so they are excluded here.

print("Testing URL...")

def u = url_parse("http://user:pass@example.com:8080/path?query=1#frag")
assert(eq(get(u, 0), "http"), "scheme")
assert(eq(get(u, 1), "user:pass"), "auth")
assert(eq(get(u, 2), "example.com"), "host")
assert(get(u, 3) == 8080, "port")
assert(eq(get(u, 4), "/path"), "path")
assert(eq(get(u, 5), "query=1"), "query")
assert(eq(get(u, 6), "frag"), "fragment")

def p = url_parse("http://user@host:8080/path?q=1#frag")
assert(len(p) == 7, "url_parse len")
assert(eq(get(p, 0), "http"), "scheme 2")
assert(eq(get(p, 1), "user"), "user")
assert(eq(get(p, 2), "host"), "host")
assert(get(p, 3) == 8080, "port 2")
assert(eq(get(p, 4), "/path"), "path 2")
assert(eq(get(p, 5), "q=1"), "query 2")
assert(eq(get(p, 6), "frag"), "fragment 2")

def q = parse_query("a=1&b=2&flag")
assert(eq(dict_get(q, "a", 0), "1"), "query a")
assert(eq(dict_get(q, "b", 0), "2"), "query b")
assert(dict_get(q, "flag", 0) == 1, "query flag")

print("✓ std.util.url tests passed")
