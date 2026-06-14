;; Keywords: io input output stream os
;; Process I/O operations with send, receive, line reads, and close semantics.
;; References:
;; - std.os
module std.os.io(spawn, send, sendline, recv, recv_line, recv_all, shutdown_send, close)
use std.core
use std.os.process
use std.os.sys
use std.core.str
use std.core.io
use std.core.str (chr)

fn _pfd(any p, any key) int {
   if !is_dict(p) { return -1 }
   p.get(key, -1)
}

fn spawn(any path, any args) any {
   "Starts `path` with stdin/stdout pipes and returns a process object."
   if !is_str(path) { return 0 }
   if !is_list(args) { return 0 }
   def res = popen(path, args)
   if res == 0 { return 0 }
   mut p = dict(4)
   p["pid"] = res.get(0)
   p["in"] = res.get(1)
   p["out"] = res.get(2)
   p["alive"] = 1
   p
}

fn send(any p, any data) any {
   "Sends `data` to process `p`'s stdin."
   if !is_dict(p) { return err("not a process") }
   def fd = _pfd(p, "in")
   if fd < 0 { return err("invalid fd") }
   if !is_str(data) { data = to_str(data) }
   def n = data.len
   if n <= 0 { return ok(0) }
   mut off = 0
   while off < n {
      match sys_write(fd, to_int(data) + off, n - off) {
         ok(w) -> {
            if w <= 0 { return err("short write") }
            off += w
         }
         err(e) -> { return err(e) }
      }
   }
   ok(off)
}

fn sendline(any p, any data) any {
   "Sends `data` followed by newline to process `p`'s stdin."
   if !is_str(data) { data = to_str(data) }
   match send(p, data) {
      ok(w0) -> {
         match send(p, "\n") {
            ok(w1) -> { return ok(w0 + w1) }
            err(e1) -> { return err(e1) }
         }
      }
      err(e0) -> { return err(e0) }
   }
}

fn recv(any p, any n=1024) any {
   "Receives up to `n` bytes from process `p`'s stdout."
   def fd = _pfd(p, "out")
   if fd < 0 { return 0 }
   if !is_int(n) || n <= 0 { n = 1024 }
   if n > 8 * 1024 * 1024 { n = 8 * 1024 * 1024 }
   mut buf = malloc(n + 1)
   if buf == 0 { return "" }
   match sys_read(fd, buf, n) {
      ok(r) -> {
         if r <= 0 {
            free(buf)
            return ""
         }
         init_str(buf, r)
         buf
      }
      err(ignorederr) -> { ignorederr
         free(buf)
         ""
      }
   }
}

fn recv_line(any p) str {
   "Receives a single line from process `p`'s stdout."
   def fd = _pfd(p, "out")
   if fd < 0 { return "" }
   mut c_buf = malloc(2)
   if c_buf == 0 { return "" }
   defer { free(c_buf) }
   init_str(c_buf, 1)
   mut b = Builder(128)
   defer { builder_free(b) }
   while 1 {
      def got = sys_read(fd, c_buf, 1)
      mut res = 0
      match got {
         ok(r) -> { res = r }
         err(ignorederr) -> { ignorederr  res = -1 }
      }
      if res <= 0 { break }
      def c = chr(load8(c_buf, 0))
      b = builder_append(b, c)
      if c == "\n" { break }
   }
   builder_to_str(b)
}

fn recv_all(any p, any n=1024) str {
   "Receives all available data from process `p`'s stdout."
   if !is_int(n) || n <= 0 { n = 1024 }
   if n > 8 * 1024 * 1024 { n = 8 * 1024 * 1024 }
   mut b = Builder(4096)
   defer { builder_free(b) }
   while 1 {
      def chunk = recv(p, n)
      if chunk.len == 0 { break }
      b = builder_append(b, chunk)
   }
   builder_to_str(b)
}

fn shutdown_send(any p) any {
   "Closes process `p`'s stdin."
   if !is_dict(p) { return err("not a process") }
   def fd = _pfd(p, "in")
   sys_close_quiet(fd)
   p["in"] = -1
   ok(0)
}

fn close(any p) any {
   "Closes all process pipes and waits for completion."
   if !is_dict(p) { return err("not a process") }
   sys_close_quiet(_pfd(p, "in"))
   sys_close_quiet(_pfd(p, "out"))
   p["in"] = -1
   p["out"] = -1
   def pid = _pfd(p, "pid")
   if pid < 0 { return err("invalid pid") }
   def status = waitpid(pid, 0)
   if status < 0 { return err(status) }
   ok(status)
}

#main {
   mut cat_cmd = "/bin/cat"
   mut cat_args = ["/bin/cat"]
   mut sh_cmd = "/bin/sh"
   mut sh_args = ["/bin/sh", "-c", "printf abc"]
   mut expected = "abc"
   #windows {
      cat_cmd = "python"
      cat_args = ["-c", "import sys; print(sys.stdin.read(), end='', flush=True)"]
      sh_cmd = "cmd"
      sh_args = ["cmd", "/c", "echo abc"]
      expected = "abc\r\n"
   } #endif
   def cat = spawn(cat_cmd, cat_args)
   assert(is_dict(cat), "io spawn cat")
   assert(is_ok(send(cat, "hello")) && is_ok(sendline(cat, " world")) && is_ok(shutdown_send(cat)), "io send cat")
   assert(strip(recv_line(cat)) == "hello world" && is_ok(close(cat)), "io recv cat")
   def sh = spawn(sh_cmd, sh_args)
   assert(is_dict(sh) && recv_all(sh, 64) == expected && is_ok(close(sh)), "io shell recv")
   assert(spawn(123, ["/bin/cat"]) == 0 && spawn("/bin/cat", 123) == 0 && is_err(send(0, "x")) && recv(0, 1) == 0, "io validation")
   print("✓ std.os.io self-test passed")
}
