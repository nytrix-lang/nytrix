;; Keywords: os io process
;; Process IO module (pwntools-like).

module std.os.io (
   spawn, send, sendline, recv, recv_line, recv_all, shutdown_send, close
)
use std.core *
use std.os.process *
use std.os.sys *
use std.str *
use std.str.io *
use std.str.chr

fn spawn(path, args){
   "Starts `path` with stdin/stdout pipes and returns a process object."
   if(!is_str(path)){ return 0 }
   def res = popen(path, args)
   if(res == 0){ return 0 }
   ;; res = [pid, child_stdin, child_stdout]
   mut p = dict(4)
   dict_set(p, "pid", get(res, 0))
   dict_set(p, "in", get(res, 1))
   dict_set(p, "out", get(res, 2))
   dict_set(p, "alive", 1)
   p
}

fn send(p, data){
   "Sends `data` to process `p`'s stdin."
   if(!is_dict(p)){ return err("not a process") }
   def fd = dict_get(p, "in", -1)
   if(fd < 0){ return err("invalid fd") }
   def n = str_len(data)
   match sys_write(fd, data, n){
      ok(r) -> { ok(r) }
      err(e) -> { err(e) }
   }
}

fn sendline(p, data){
   "Sends `data` followed by newline to process `p`'s stdin."
   send(p, str_add(data, "\n"))
}

fn recv(p, n=1024){
   "Receives up to `n` bytes from process `p`'s stdout."
   if(!is_dict(p)){ return 0 }
   def fd = dict_get(p, "out", -1)
   if(fd < 0){ return 0 }
   mut buf = malloc(n + 1)
   match sys_read(fd, buf, n){
      ok(r) -> {
         if(r <= 0){
            free(buf)
            return ""
         }
         init_str(buf, r)
         store8(buf, 0, r)
         buf
      }
      err(_) -> {
         free(buf)
         ""
      }
   }
}

fn recv_line(p){
   "Receives a single line from process `p`'s stdout."
   if(!is_dict(p)){ return "" }
   def fd = dict_get(p, "out", -1)
   mut out = ""
   mut c_buf = malloc(2)
   init_str(c_buf, 1)
   while(1){
      def got = sys_read(fd, c_buf, 1)
      mut res = 0
      match got {
         ok(r) -> { res = r }
         err(_) -> { res = -1 }
      }
      if(res <= 0){ break }
      def c = chr(load8(c_buf, 0))
      out = str_add(out, c)
      if(c == "\n"){ break }
   }
   free(c_buf)
   out
}

fn recv_all(p, n=1024){
   "Receives all available data from process `p`'s stdout."
   mut out = ""
   while(1){
      def chunk = recv(p, n)
      if(str_len(chunk) == 0){ break }
      out = str_add(out, chunk)
   }
   out
}

fn shutdown_send(p){
   "Closes process `p`'s stdin."
   if(!is_dict(p)){ return err("not a process") }
   def fd = dict_get(p, "in", -1)
   if(fd >= 0){ unwrap(sys_close(fd)) }
   dict_set(p, "in", -1)
   ok(0)
}

fn close(p){
   "Closes all process pipes and waits for completion."
   if(!is_dict(p)){ return err("not a process") }
   def in_fd = dict_get(p, "in", -1)
   def out_fd = dict_get(p, "out", -1)
   if(in_fd >= 0){ unwrap(sys_close(in_fd)) }
   if(out_fd >= 0){ unwrap(sys_close(out_fd)) }
   def pid = dict_get(p, "pid", -1)
   def status = waitpid(pid, 0)
   ok(status)
}

if(comptime{__main()}){
    use std.os.io *
    use std.core *
    use std.str *
    use std.os.sys *

    ;; pwntools-like process IO primitives.

    def osn = os()
    mut cat_cmd = "/bin/cat"
    mut cat_args = ["/bin/cat"]
    mut sh_cmd = "/bin/sh"
    mut sh_args = ["/bin/sh", "-c", "printf abc"]
    mut expected_sh_output = "abc"

    if(eq(osn, "windows")){
        cat_cmd = "python"
        cat_args = ["-c", "import sys; print(sys.stdin.read(), end='', flush=True)"]
        sh_cmd = "cmd"
        sh_args = ["cmd", "/c", "echo abc"]
        expected_sh_output = "abc\r\n"
    }

    def p = spawn(cat_cmd, cat_args)
    assert(p != 0, "spawn cat")

    unwrap(send(p, "hello"))
    unwrap(sendline(p, " world"))
    unwrap(shutdown_send(p))

    def line = recv_line(p)
    def line_clean = strip(line)
    assert(eq(line_clean, "hello world"), "recv_line cat")
    def code1 = close(p)
    assert(is_ok(code1), "close cat")

    def p2 = spawn(sh_cmd, sh_args)
    assert(p2 != 0, "spawn sh printf")
    def out = recv_all(p2, 1024)
    assert(eq(out, expected_sh_output), "recv_all")
    def code2 = close(p2)
    assert(is_ok(code2), "close sh printf")

    def bad = spawn(123, ["/bin/cat"])
    assert(bad == 0, "spawn input validate")

    print("✓ std.os.io tests passed")
}
