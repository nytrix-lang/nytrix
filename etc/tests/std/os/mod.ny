use std.io
use std.os
use std.core.error
use std.strings.str

;; std.os.mod (Test)
;; Tests process info and environment access.

print("Testing OS Mod...")

def p = pid()
assert(p > 0, "pid > 0")

def pp = ppid()
assert(pp > 0, "ppid > 0")

def u = uid()
assert(u >= 0, "uid >= 0")

def g = gid()
assert(g >= 0, "gid >= 0")

def path = env("PATH")
if(path != 0){
 assert(str_len(path) > 0, "env PATH len")
} else {
 assert(0, "env PATH missing")
}

def e = environ()
assert(type(e) == "list", "environ list")
assert(len(e) > 0, "environ len")

print("✓ std.os.mod tests passed")
