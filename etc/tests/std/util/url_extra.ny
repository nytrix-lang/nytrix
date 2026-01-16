use std.io
use std.util.url
use std.collections.dict
use std.core.test
use std.strings.str
use std.core

print("Testing std.util.url (extra)...")

fn cstr_eq(cptr, s){
   def n = str_len(s)
   def i = 0
   while(i < n){
      if(__load8_idx(cptr, i) != __load8_idx(s, i)){ return false }
      i = i + 1
   }
   return __load8_idx(cptr, n) == 0
}

def enc = urlencode("a b+c")
def dec = urldecode("a%20b%2bc")
assert(cstr_eq(enc, "a%20b%2bc"), "urlencode output")
assert(cstr_eq(dec, "a b+c"), "urldecode output")

def p = url_parse("http://user@host:8080/path?q=1#frag")
assert(len(p) == 7, "url_parse len")
assert(get(p, 0) == "http", "url_parse scheme")
assert(get(p, 1) == "user", "url_parse user")
assert(get(p, 2) == "host", "url_parse host")
assert(get(p, 3) == 8080, "url_parse port")
assert(get(p, 4) == "/path", "url_parse path")
assert(get(p, 5) == "q=1", "url_parse query")
assert(get(p, 6) == "frag", "url_parse fragment")

def q = parse_query("a=1&b=2&flag")
def qa = dict_get(q, "a", 0)
def qb = dict_get(q, "b", 0)
assert(eq(qa, "1"), "parse_query a")
assert(eq(qb, "2"), "parse_query b")
assert(dict_get(q, "flag", 0) == 1, "parse_query flag")

print("✓ std.util.url extra passed")
