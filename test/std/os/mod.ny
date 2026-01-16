use std.io
use std.os
use std.core.test
use std.core
use std.strings.str

print("Testing OS Mod...")

def p = pid()
print("PID:")
print(p)
assert(p > 0, "pid > 0")

def pp = ppid()
print("PPID:", pp)
assert(pp > 0, "ppid > 0")

def u = uid()
print("UID:", u)
assert(u >= 0, "uid >= 0")

def g = gid()
print("GID:", g)
assert(g >= 0, "gid >= 0")

; env
def path = env("PATH")
print("PATH:", path)
if(path != 0){
	def path_len = str_len(path)
	print("PATH found len: ", path_len)
	assert(path_len > 0, "env PATH len")
} else {
	print("PATH not found")
	assert(0, "env PATH")
}

def e = environ()
assert(type(e) == "list", "environ list")
assert(len(e) > 0, "environ len")

print("✓ std.os.mod passed")
