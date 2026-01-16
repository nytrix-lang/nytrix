use std.process
use std.core.test
use std.io.fs
use std.io
use std.strings.str

fn test_process(){
   print("Testing Process...")
   ; Test run
   def res = run("/usr/bin/echo", ["echo", "hello"])
   assert(res == 0, "run echo")
   ; Test popen
   print("Testing popen...")
   def p = popen("/bin/cat", ["cat"])
   def pid = get(p, 0)
   def stdin = get(p, 1)
   def stdout = get(p, 2)
   ; Write to stdin
   def msg = "hello pipe"
   __syscall(1, stdin, msg, str_len(msg), 0,0,0)
   __syscall(3, stdin, 0,0,0,0,0) ; Close stdin to EOF
   def buf = __malloc(100)
   def nr = __syscall(0, stdout, buf, 100, 0,0,0)
   if(nr < 0){ nr = 0 }
   store64(buf, 120, -8) ; Tag as string
   __store8_idx(buf, nr, 0)
   assert(eq(buf, msg), "pipe echo match")
   ; Wait for child
   waitpid(pid, 0)
   __syscall(3, stdout, 0,0,0,0,0)
   print("✓ std.process.mod passed")
}

test_process()
