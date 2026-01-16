use std.io
use std.net.requests
use std.net.http
use std.core
use std.core.test
use std.core.reflect
use std.collections.dict

print("Testing net.requests...")

fn test_parse_url(){
	def url = "http://example.com:8080/foo/bar"
	def parts = http_parse_url(url)
	; host
	assert(eq(get(parts, 0), "example.com"), "host")
	; port
	assert(get(parts, 1) == 8080, "port")
	; path
	assert(eq(get(parts, 2), "/foo/bar"), "path")
	def url2 = "example.org"
	def parts2 = http_parse_url(url2)
	assert(eq(get(parts2, 0), "example.org"), "host no scheme")
	assert(get(parts2, 1) == 80, "default port")
	assert(eq(get(parts2, 2), "/"), "default path")
}

fn test_parse_query(){
	def q = "a=1&b=hello&c"
	def d = http_parse_query(q)
	assert(eq(getitem(d, "a"), "1"), "query a")
	assert(eq(getitem(d, "b"), "hello"), "query b")
	assert(getitem(d, "c") == 1, "query c flag") ; defaults to 1 if no value? Implementation sets 1.
}

test_parse_url()
test_parse_query()

print("✓ std.net.requests tests passed")
