use std.os.io *
use std.core *
use std.str *

;; std.os.io (Test)
;; pwntools-like process IO primitives.

def p = spawn("/bin/cat", ["/bin/cat"])
assert(p != 0, "spawn cat")

unwrap(send(p, "hello"))
unwrap(sendline(p, " world"))
unwrap(shutdown_send(p))

def line = recv_line(p)
assert(eq(line, "hello world\n"), "recv_line cat")
def code1 = close(p)
assert(is_ok(code1), "close cat")

def p2 = spawn("/bin/sh", ["/bin/sh", "-c", "printf abc"])
assert(p2 != 0, "spawn sh printf")
def out = recv_all(p2, 1024)
assert(eq(out, "abc"), "recv_all")
def code2 = close(p2)
assert(is_ok(code2), "close sh printf")

def bad = spawn(123, ["/bin/cat"])
assert(bad == 0, "spawn input validate")

print("✓ std.os.io tests passed")