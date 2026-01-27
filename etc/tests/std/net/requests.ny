use std.net.requests *
use std.net.http *
use std.core.dict *
use std.core.error *

;; std.net.requests (Test)
;; Tests URL and query parsing.

print("Testing net.requests...")

def url = "http://example.com:8080/foo/bar"
def parts = http_parse_url(url)
assert(eq(get(parts, 0), "example.com"), "host")
assert(get(parts, 1) == 8080, "port")
assert(eq(get(parts, 2), "/foo/bar"), "path")

def url2 = "example.org"
def parts2 = http_parse_url(url2)
assert(eq(get(parts2, 0), "example.org"), "host no scheme")
assert(get(parts2, 1) == 80, "default port")
assert(eq(get(parts2, 2), "/"), "default path")

def q = "a=1&b=hello&c"
def d = http_parse_query(q)
assert(eq(dict_get(d, "a"), "1"), "query a")
assert(eq(dict_get(d, "b"), "hello"), "query b")
assert(dict_get(d, "c") == 1, "query c flag")

print("✓ std.net.requests tests passed")
