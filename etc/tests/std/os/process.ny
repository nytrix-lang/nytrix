use std.os.process *
use std.os.fs *
use std.os.sys *
use std.str *
use std.core.error *

;; std.process.mod (Test)
;; Tests run and popen with basic IPC.

print("Testing Process...")

def res = run("/bin/echo", ["/bin/echo", "hello"])
assert(res == 0, "run echo")
assert(run("/bin/sh", ["/bin/sh", "-c", "exit 7"]) == 7, "run returns exit code")
assert(run("/bin/does-not-exist-nytrix", ["/bin/does-not-exist-nytrix"]) == 127,
       "run returns 127 when exec fails")

print("Testing popen...")

def p = popen("/bin/cat", ["/bin/cat"])
if(p == 0){
   print("popen failed, returned 0")
   assert(0, "popen returned 0")
}
def pid = get(p, 0, 0)
def stdin = get(p, 1, 0)
def stdout = get(p, 2, 0)

def msg = "hello pipe"
syscall(1, stdin, msg, str_len(msg), 0, 0, 0)
syscall(3, stdin, 0, 0, 0, 0, 0)

def buf = malloc(100)
mut nr = syscall(0, stdout, buf, 100, 0, 0, 0)
if(nr < 0){ nr = 0 }

init_str(buf, nr)

assert(eq(buf, msg), "pipe echo match")

def status = waitpid(pid, 0)
assert(status >= 0, "waitpid returns status")
syscall(3, stdout, 0, 0, 0, 0, 0)

print("✓ std.process.mod tests passed")
