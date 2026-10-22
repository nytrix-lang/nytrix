;; Keywords: io input output stream os
;; Process I/O operations with send, receive, line reads, and close semantics.
;; References:
;; - core.os
module core.os.io(spawn, send, sendline, recv, recv_line, recv_all, shutdown_send, close)
use core
use core.os.process
use core.os.sys
use core.str
use core.io
use core.str (chr)

fn _pfd(any p, any key) int {
   if !is_dict(p) { return -1 }
   ;; Dict int slots round-trip raw machine words verbatim on every
   ;; route, so no tag/untag glue is needed around process fds.
   p.get(key, -1)
}

fn _ppid(any p) int {
   if !is_dict(p) { return -1 }
   p.get("pid", -1)
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
   p["alive"] = true
   p
}

fn send(any p, any data) Result<int, str> {
   "Sends `data` to process `p`'s stdin."
   if !is_dict(p) { return err("not a process") }
   def fd = _pfd(p, "in")
   if fd < 0 { return err("invalid fd") }
   if !is_str(data) { data = to_str(data) }
   def n = data.len
   if n <= 0 { return ok(0) }
   def ptr bytes = __any_to_cstr(data)
   mut off = 0
   while off < n {
      match sys_write(fd, ptr_add(bytes, off), n - off) {
         ok(w) -> {
            def wr = w
            if wr <= 0 { return err("short write") }
            off += wr
         }
         err(e) -> { return err(to_str(e)) }
      }
   }
   ok(off)
}

fn sendline(any p, any data) Result<int, str> {
   "Sends `data` followed by newline to process `p`'s stdin."
   if !is_str(data) { data = to_str(data) }
   match send(p, data) {
      ok(w0) -> {
         match send(p, "\n") {
            ok(w1) -> {
               return ok(w0 + w1)
            }
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
   mut int size = is_int(n) ? int(n) : 1024
   if size <= 0 { size = 1024 }
   if size > 8 * 1024 * 1024 { size = 8 * 1024 * 1024 }
   mut buf = malloc(size + 1)
   if buf == 0 { return "" }
   def int rr = __read_fd_raw(fd, buf, size)
   if rr <= 0 {
      free(buf)
      return ""
   }
   store8(buf, 0, rr)
   def str out = __init_str_raw(buf, rr)
   out
}

fn recv_line(any p) str {
   "Receives a single line from process `p`'s stdout."
   def fd = _pfd(p, "out")
   if fd < 0 { return "" }
   mut c_buf = malloc(2)
   if c_buf == 0 { return "" }
   mut b = Builder(128)
   while 1 {
      def int res = __read_fd_raw(fd, c_buf, 1)
      if res <= 0 { break }
      def c = chr(load8(c_buf, 0))
      b = builder_append(b, c)
      if c == "\n" { break }
   }
   def out = builder_to_str(b)
   builder_free(b)
   free(c_buf)
   out
}

fn recv_all(any p, any n=1024) str {
   "Receives all available data from process `p`'s stdout."
   if !is_int(n) || n <= 0 { n = 1024 }
   if n > 8 * 1024 * 1024 { n = 8 * 1024 * 1024 }
   mut b = Builder(4096)
   while 1 {
      def chunk = recv(p, n)
      if chunk.len == 0 { break }
      b = builder_append(b, chunk)
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
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
   def pid = _ppid(p)
   if pid < 0 { return err("invalid pid") }
   def int status = waitpid(pid, 0)
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
   print("✓ core.os.io self-test passed")
}
