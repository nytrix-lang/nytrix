use std.net.http *
use std.core.list *
use std.core.error *

;; std.net.http (Test)
;; Tests URL and query parsing helpers.

print("Testing HTTP...")

def part = http_parse_url("http://google.com/foo")
assert(eq(get(part, 0), "google.com"), "parse host")
assert(get(part, 1) == 80, "parse port")
assert(eq(get(part, 2), "/foo"), "parse path")

def q = http_parse_query("a=1&b=2")
assert(eq(dict_get(q, "a"), "1"), "query a")
assert(eq(dict_get(q, "b"), "2"), "query b")

print("✓ std.net.http tests passed")
