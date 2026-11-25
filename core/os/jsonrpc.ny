;; Keywords: os jsonrpc lsp dap rpc protocol process pipes json framing
;; Content-Length framed JSON messaging with a child process, the transport
;; shared by the Language Server Protocol and the Debug Adapter Protocol.
;; `start` spawns the server on pipes (stderr discarded so it cannot corrupt
;; the framing), `send`/`request`/`notify`/`respond` write framed messages,
;; and `poll` reads whatever has arrived without blocking and returns the
;; complete decoded messages. Suited to a UI loop: call `poll(c, 0)` once per
;; frame.
;; References:
;; - https://microsoft.github.io/language-server-protocol/specifications/base/0.9/specification/
;; - https://microsoft.github.io/debug-adapter-protocol/overview
module core.os.jsonrpc(start, alive, send, request, notify, respond, poll, stop, frame, take_frames)
use core
use core.str as st
use core.os.process as process
use core.os.sys as sys
use core.math.parse.data.json (json_encode, json_try_decode)

fn frame(any msg) str {
   "Returns `msg` encoded as JSON behind a Content-Length header."
   def body = json_encode(msg)
   "Content-Length: " + to_str(body.len) + "\r\n\r\n" + body
}

fn start(list argv, str cwd="") any {
   "Spawns `argv` with piped stdin/stdout and returns a connection dict, or
   an error string. `cwd` (optional) is the server's working directory."
   if argv.len <= 0 { return "empty command" }
   ;; sh keeps stderr off the protocol pipe and applies the working directory.
   mut args = ["sh", "-c", "cd \"$0\" 2>/dev/null; exec \"$@\" 2>/dev/null", cwd.len > 0 ? cwd : "."]
   mut i = 0
   while i < argv.len { args = args.append(to_str(argv.get(i, ""))) i += 1 }
   def p = process.popen("/bin/sh", args)
   if !is_list(p) || p.len < 3 { return "cannot start " + to_str(argv.get(0, "")) }
   def int pid = int(p.get(0, 0))
   if pid <= 0 { return "cannot start " + to_str(argv.get(0, "")) }
   {"pid": pid, "in": int(p.get(1, -1)), "out": int(p.get(2, -1)), "buf": "",
    "next_id": 1, "alive": true, "argv": argv, "error": ""}
}

fn alive(dict c) bool {
   "True while the child is running and its pipes are open."
   if !bool(c.get("alive", false)) { return false }
   if _reaped(int(c.get("pid", 0))) {
      c["alive"] = false
      c["error"] = "server exited"
   }
   bool(c.get("alive", false))
}

fn _reaped(int pid) bool {
   ;; wait4(pid, NULL, WNOHANG): the pid once it has exited, 0 while running.
   if pid <= 0 { return false }
   mut int r = 0
   #x86_64 { r = sys.syscall(61, pid, 0, 1, 0) } #endif
   #aarch64 { r = sys.syscall(260, pid, 0, 1, 0) } #endif
   r == pid
}

fn send(dict c, any msg) bool {
   "Writes one framed message; false (and the connection closed) on failure."
   if !bool(c.get("alive", false)) { return false }
   def data = frame(msg)
   def int fd = int(c.get("in", -1))
   mut int off = 0
   while off < data.len {
      def r = sys.sys_write(fd, ptr_add(data, off), data.len - off)
      if !is_ok(r) || unwrap(r) <= 0 {
         c["alive"] = false
         c["error"] = "write failed"
         return false
      }
      off += unwrap(r)
   }
   true
}

fn request(dict c, str method, any params=nil) int {
   "Sends a JSON-RPC request and returns its id (0 when it could not be sent)."
   def int id = int(c.get("next_id", 1))
   c["next_id"] = id + 1
   mut dict msg = {"jsonrpc": "2.0", "id": id, "method": method}
   if !is_nil(params) { msg["params"] = params }
   send(c, msg) ? id : 0
}

fn notify(dict c, str method, any params=nil) bool {
   "Sends a JSON-RPC notification."
   mut dict msg = {"jsonrpc": "2.0", "method": method}
   if !is_nil(params) { msg["params"] = params }
   send(c, msg)
}

fn respond(dict c, any id, any result=nil) bool {
   "Answers a request the server sent to the client."
   send(c, {"jsonrpc": "2.0", "id": id, "result": result})
}

fn _readable(int fd, int timeout_ms) bool {
   def ptr pfd = malloc(8)
   if !pfd { return false }
   defer { free(pfd) }
   store32(pfd, fd, 0)
   store32(pfd, 1, 4) ;; events = POLLIN, revents = 0
   mut int n = -1
   #x86_64 { n = sys.syscall(7, pfd, 1, timeout_ms) } #endif
   #aarch64 {
      def ptr ts = malloc(16)
      if !ts { return false }
      store64(ts, timeout_ms / 1000, 0)
      store64(ts, (timeout_ms % 1000) * 1000000, 8)
      n = sys.syscall(73, pfd, 1, ts, 0, 8)
      free(ts)
   } #endif
   ;; POLLIN, or POLLHUP/POLLERR so the read below sees end of file.
   n > 0 && (load32(pfd, 4) >> 16) != 0
}

fn take_frames(dict c) list {
   "Removes every complete message from the connection's buffer and returns
   them decoded (undecodable bodies are skipped)."
   mut out = []
   mut buf = to_str(c.get("buf", ""))
   while true {
      def h = st.find(buf, "\r\n\r\n")
      if h < 0 { break }
      def header = st.lower(st.str_slice(buf, 0, h))
      def k = st.find(header, "content-length:")
      if k < 0 {
         buf = st.str_slice(buf, h + 4, buf.len)
         continue
      }
      mut int p = k + 15
      while p < header.len && (load8(header, p) == 32 || load8(header, p) == 9) { p += 1 }
      mut int n = 0
      while p < header.len && load8(header, p) >= 48 && load8(header, p) <= 57 { n = n * 10 + load8(header, p) - 48 p += 1 }
      def int body = h + 4
      if buf.len < body + n { break }
      def res = json_try_decode(st.str_slice(buf, body, body + n))
      if is_dict(res) && bool(res.get("ok", false)) { out = out.append(res.get("value")) }
      buf = st.str_slice(buf, body + n, buf.len)
   }
   c["buf"] = buf
   out
}

fn poll(dict c, int timeout_ms=0) list {
   "Reads what has arrived (waiting at most `timeout_ms` for the first byte)
   and returns the complete messages, oldest first."
   if !bool(c.get("alive", false)) { return take_frames(c) }
   def int fd = int(c.get("out", -1))
   def ptr chunk = malloc(65536)
   if !chunk { return [] }
   defer { free(chunk) }
   mut int wait = timeout_ms
   mut int rounds = 0
   while rounds < 64 && _readable(fd, wait) {
      def r = sys.sys_read(fd, chunk, 65536)
      if !is_ok(r) || unwrap(r) <= 0 {
         c["alive"] = false
         c["error"] = "server closed its output"
         break
      }
      def int got = unwrap(r)
      def ptr owned = malloc(got + 1)
      if !owned { break }
      memcpy(owned, chunk, got)
      c["buf"] = to_str(c.get("buf", "")) + init_str(owned, got)
      wait = 0
      rounds += 1
   }
   take_frames(c)
}

fn stop(dict c) bool {
   "Closes the pipes and terminates the child."
   def int pid = int(c.get("pid", 0))
   if bool(c.get("alive", false)) {
      sys.sys_close_quiet(int(c.get("in", -1)))
      sys.sys_close_quiet(int(c.get("out", -1)))
   }
   if pid > 0 {
      #x86_64 { sys.syscall(62, pid, 15) } #endif
      #aarch64 { sys.syscall(129, pid, 15) } #endif
      if !_reaped(pid) { process.waitpid(pid, 0) }
   }
   c["alive"] = false
   true
}
