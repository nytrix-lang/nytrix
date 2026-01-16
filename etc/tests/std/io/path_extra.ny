use std.io.path
use std.core.test
use std.strings.str

print("Testing std.io.path (extra)...")

assert(basename("/a/b/c.txt") == "c.txt", "basename file")
assert(basename("/a/b/") == "b", "basename trailing slash")
assert(dirname("/a/b/c.txt") == "/a/b", "dirname file")
assert(dirname("c.txt") == ".", "dirname relative")
assert(extname("/a/b/c.txt") == ".txt", "extname txt")
assert(extname("/a/b/c") == "", "extname none")

def n1 = normalize("/a/./b/../c/")
assert(n1 == "/a/c", "normalize dot dotdot")

def j = path_join("/a", "b", "c")
assert(j == "/a/b/c", "path_join")

print("✓ std.io.path extra passed")
