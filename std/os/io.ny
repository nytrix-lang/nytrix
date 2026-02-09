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
   p
}

fn send(p, data){
   "Sends `data` to process `p`'s stdin."
   if(!is_dict(p)){ return err("not a process") }
   def fd = dict_get(p, "in", -1)
   if(fd < 0){ return err("invalid fd") }
   def n = str_len(data)
   def res = syscall(1, fd, data, n, 0, 0, 0) ;; write
   if(res < 0){ return err(res) }
   ok(res)
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
   def res = syscall(0, fd, buf, n, 0, 0, 0) ;; read
   if(res <= 0){
      free(buf)
      return ""
   }
   init_str(buf, res)
   store8(buf, 0, res)
   buf
}

fn recv_line(p){
   "Receives a single line from process `p`'s stdout."
   if(!is_dict(p)){ return "" }
   def fd = dict_get(p, "out", -1)
   mut out = ""
   mut c_buf = malloc(2)
   init_str(c_buf, 1)
   while(1){
      def res = syscall(0, fd, c_buf, 1, 0, 0, 0)
      if(res <= 0){ break }
      def c = chr(load8(c_buf, 0))
      out = str_add(out, c)
      if(eq(c, "\n")){ break }
   }
   free(c_buf)
   out
}

fn recv_all(p, timeout=1024){
   "Receives all available data from process `p`'s stdout."
   mut out = ""
   while(1){
      def chunk = recv(p, 1024)
      if(eq(str_len(chunk), 0)){ break }
      out = str_add(out, chunk)
   }
   out
}

fn shutdown_send(p){
   "Closes process `p`'s stdin."
   if(!is_dict(p)){ return err("not a process") }
   def fd = dict_get(p, "in", -1)
   syscall(3, fd, 0,0,0,0,0) ;; close
   dict_set(p, "in", -1)
   ok(0)
}

fn close(p){
   "Closes all process pipes and waits for completion."
   if(!is_dict(p)){ return err("not a process") }
   def in_fd = dict_get(p, "in", -1)
   def out_fd = dict_get(p, "out", -1)
   if(in_fd >= 0){ syscall(3, in_fd, 0,0,0,0,0) }
   if(out_fd >= 0){ syscall(3, out_fd, 0,0,0,0,0) }
   def pid = dict_get(p, "pid", -1)
   def status = waitpid(pid, 0)
   ok(status)
}
