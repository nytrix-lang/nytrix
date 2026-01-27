use std.io
use std.io.path
use std.core.error

;; std.io.path (Test)
;; Tests basename, dirname, extname, join, and normalize.

print("Testing basename...")
assert(eq(basename("/foo/bar.txt"), "bar.txt"), "basename file")
assert(eq(basename("/foo/bar/"), "bar"), "basename dir slash")
assert(eq(basename("bar"), "bar"), "basename plain")
assert(eq(basename("/"), "/"), "basename root")
assert(eq(basename(""), ""), "basename empty")

print("Testing dirname...")
assert(eq(dirname("/foo/bar.txt"), "/foo"), "dirname file")
assert(eq(dirname("/foo/bar/"), "/foo"), "dirname dir slash")
assert(eq(dirname("bar"), "."), "dirname plain")
assert(eq(dirname("/"), "/"), "dirname root")
assert(eq(dirname("/foo"), "/"), "dirname parent")

print("Testing extname...")
assert(eq(extname("foo.txt"), ".txt"), "ext .txt")
assert(eq(extname("/path/to/foo.tar.gz"), ".gz"), "ext .gz")
assert(eq(extname("file"), ""), "ext none")
assert(eq(extname(".hidden"), ".hidden"), "ext hidden")

print("Testing join...")
assert(eq(path_join("foo", "bar"), "foo/bar"), "join simple")
assert(eq(path_join("/foo", "bar"), "/foo/bar"), "join abs")
assert(eq(path_join("foo/", "bar"), "foo/bar"), "join trailing")
assert(eq(path_join("foo", "/bar"), "/bar"), "join abs right")

print("Testing extra cases...")
assert(basename("/a/b/c.txt") == "c.txt", "basename file")
assert(basename("/a/b/") == "b", "basename trailing")
assert(dirname("/a/b/c.txt") == "/a/b", "dirname file")
assert(dirname("c.txt") == ".", "dirname relative")
assert(extname("/a/b/c.txt") == ".txt", "ext txt")
assert(extname("/a/b/c") == "", "ext none")

def n1 = normalize("/a/./b/../c/")
assert(n1 == "/a/c", "normalize")

def j = path_join("/a", "b", "c")
assert(j == "/a/b/c", "join multi")

print("✓ std.io.path tests passed")
