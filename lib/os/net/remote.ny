;; Keywords: net socket remote tube process ssh
;; Buffered tube-style interaction with TCP sockets, local processes, SSH, and transcripts.
module std.os.net.remote(remote, connect, remote_retry, connect_retry, process, proc, shell, ssh, ssh_process, ssh_shell, tube, tube_fd, tube_process, tube_fixture, fixture, tube_replay, replay, transcript, transcript_text, tube_kind, fileno, pid, connected, context, set_context, set_default_level, default_level, set_level, set_log_level, log_level, set_verbose, verbose, set_chunk_size, set_timeout, buffered, unrecv, clean, send, sendline, send_after, sendafter, sendline_after, sendlineafter, recv, recvn, recv_line, recvline, recv_until, recvuntil, recv_all, recvall, expect, expect_map, shutdown_send, close, interactive)
use std.core
use std.core.str
use std.os.net.socket as sock
use std.os.net.context as netctx
use std.os.io as pio
use std.os.prim
use std.os.sys as sys
use std.os.time

fn set_default_level(any: level="debug"): str {
   "Sets the default logging level for newly created tubes."
   netctx.set_default_level(level)
}

fn default_level(): str { return netctx.default_level() }

fn set_context(any: options=0): dict {
   "Updates process-wide net context defaults for tubes and requests."
   netctx.set_context(options)
}

fn context(any: log_level=""): dict {
   "Returns or updates net context. Use `context(log_level=\"debug\")` or `context({\"log_level\":\"trace\"})`."
   netctx.context(log_level)
}

fn _ctx(dict: io): str {
   def host = io.get("host", "")
   def port = io.get("port", 0)
   def fd = io.get("fd", -1)
   def kind = io.get("kind", "tcp")
   mut out = "tube/" + to_str(kind)
   if(is_str(host) && host.len > 0){ out = out + " " + host + ":" + to_str(port) }
   if(fd >= 0){ return out + " fd=" + to_str(fd) }
   def p = io.get("pid", -1)
   if(p >= 0){ return out + " pid=" + to_str(p) }
   out
}

fn _log_enabled(any: io, str: want): bool {
   if(!is_dict(io)){ return false }
   netctx.level_value(io.get("level", "quiet")) >= netctx.level_value(want)
}

fn _level_enabled(any: level, str: want): bool { netctx.level_value(level) >= netctx.level_value(want) }

fn _log(any: io, str: want, str: msg): int {
   if(is_dict(io) && _log_enabled(io, want)){
      def lvl = netctx.level_name(want)
      def color = (lvl == "error") ? "red" : (lvl == "info" ? "green" : (lvl == "debug" ? "cyan" : "gray"))
      print(netctx.paint("[net " + lvl + "]", color, lvl == "info" ? 1 : 0) + " " + _ctx(io) + " " + msg)
   }
   0
}

fn _log_connect(str: host, int: port, any: level, str: msg): int {
   if(_level_enabled(level, "info")){ print(netctx.paint("[net info]", "green", 1) + " tube " + host + ":" + to_str(port) + " " + msg) }
   0
}

fn _is_printable_ascii(int: b): bool { b >= 32 && b <= 126 }

fn _hexdump_preview(str: s, int: max_bytes=96): str {
   def n = s.len
   def take = (n < max_bytes) ? n : max_bytes
   mut hex_b = Builder(take * 2 + 8)
   mut asc_b = Builder(take + 8)
   mut i = 0
   while(i < take){
      def b = load8(s, i)
      hex_b = builder_append(hex_b, to_hex(b, 2))
      asc_b = builder_append(asc_b, _is_printable_ascii(b) ? chr(b) : ".")
      i += 1
   }
   if(n > take){ asc_b = builder_append(asc_b, "...") }
   def hex = builder_to_str(hex_b)
   def asc = builder_to_str(asc_b)
   builder_free(hex_b)
   builder_free(asc_b)
   hex + " |" + asc + "|"
}

fn _trace(dict: io, str: dir, str: op, str: data): int {
   if(_log_enabled(io, "debug")){ print(netctx.paint("[net debug]", "cyan", 0) + " " + _ctx(io) + " " + netctx.paint(dir, dir == "[>]" ? "yellow" : "magenta", 1) + " " + op + " " + to_str(data.len) + "B " + _hexdump_preview(data)) }
   0
}

fn _record(dict: io, str: op, str: data): int {
   mut rows = io.get("transcript", 0)
   if(!is_list(rows)){ rows = list(8) }
   rows = rows.append({"op": op, "data": data, "size": data.len})
   io.set("transcript", rows)
   0
}

fn tube_fd(int: fd, any: host="", any: port=0, any: level="", int: timeout_ms=-1, int: chunk_size=0): dict {
   "Wraps an existing TCP socket fd in a buffered remote tube."
   mut io = dict(20)
   io = io.set("kind", "tcp")
   io = io.set("fd", fd)
   io = io.set("pid", -1)
   io = io.set("proc", 0)
   io = io.set("host", host)
   io = io.set("port", port)
   io = io.set("buf", "")
   io = io.set("chunk", 4096)
   io = io.set("closed", fd < 0)
   io = io.set("offline", false)
   io = io.set("script", "")
   io = io.set("sent", "")
   io = io.set("transcript", list(8))
   def lvl = (is_str(level) && strip(level).len > 0) ? netctx.level_name(level) : netctx.default_level()
   io = io.set("level", lvl)
   io = io.set("verbose", netctx.level_value(lvl) >= netctx.level_value("debug"))
   def timeout_eff = (timeout_ms >= 0) ? timeout_ms : netctx.timeout_ms(5000)
   io = io.set("timeout_ms", timeout_eff)
   def chunk_eff = (chunk_size > 0) ? chunk_size : netctx.chunk_size()
   if(chunk_eff > 0){ io = io.set("chunk", max(1, min(chunk_eff, 1048576))) }
   if(fd >= 0 && timeout_eff > 0){ sock.socket_set_timeout_ms(fd, timeout_eff) }
   _log(io, "trace", "created")
   return io
}

fn tube(int: fd): dict { return tube_fd(fd) }

fn tube_process(any: p, any: name="process", any: level="", int: timeout_ms=-1, int: chunk_size=0): dict {
   "Wraps a `std.os.io.spawn` process object as a buffered tube."
   if(!is_dict(p)){ return tube_fd(-1, name, 0, level, timeout_ms, chunk_size) }
   mut io = tube_fd(-1, name, 0, level, timeout_ms, chunk_size)
   io.set("kind", "process")
   io.set("closed", false)
   io.set("proc", p)
   io.set("pid", p.get("pid", -1))
   _log(io, "trace", "process tube created")
   io
}

fn tube_fixture(any: data="", any: level="", int: chunk_size=0): dict {
   "Creates an offline tube backed by scripted receive bytes."
   if(!is_str(data)){ data = to_str(data) }
   mut io = tube_fd(-1, "fixture", 0, level, 0, chunk_size)
   io.set("closed", false)
   io.set("offline", true)
   io.set("script", data)
   io.set("sent", "")
   io.set("transcript", list(8))
   _log(io, "trace", "fixture " + to_str(data.len) + "B")
   io
}

fn fixture(any: data="", any: level="", int: chunk_size=0): dict { return tube_fixture(data, level, chunk_size) }

fn tube_replay(any: rows, any: level="", int: chunk_size=0): dict {
   "Creates a fixture tube from transcript rows, replaying received bytes."
   if(is_str(rows)){ return tube_fixture(rows, level, chunk_size) }
   mut script = ""
   if(is_list(rows)){
      mut i = 0
      while(i < rows.len){
         def row = rows.get(i, 0)
         if(is_dict(row)){
            def op = row.get("op", row.get("dir", ""))
            def data = row.get("data", "")
            if(is_str(data) && (op == "recv" || op == "<" || op == "[<]")){ script = script + data }
         }
         i += 1
      }
   }
   tube_fixture(script, level, chunk_size)
}

fn replay(any: rows, any: level="", int: chunk_size=0): dict { return tube_replay(rows, level, chunk_size) }

fn process(str: path, list: args=[], any: level="", int: timeout_ms=-1, int: chunk_size=0): any {
   "Starts a local process and returns a buffered tube over its stdin/stdout."
   def p = pio.spawn(path, args)
   if(p == 0){ return 0 }
   tube_process(p, path, level, timeout_ms, chunk_size)
}

fn proc(str: path, list: args=[], any: level="", int: timeout_ms=-1, int: chunk_size=0): any {
   "Alias for `process`."
   process(path, args, level, timeout_ms, chunk_size)
}

fn shell(str: command, any: level="", int: timeout_ms=-1, int: chunk_size=0): any {
   "Starts the platform shell as a buffered process tube."
   #windows {
      return process("cmd", ["/c", command], level, timeout_ms, chunk_size)
   } #else {
      return process("/bin/sh", ["-c", command], level, timeout_ms, chunk_size)
   } #endif
}

fn _ssh_target(str: host, any: user=""): str {
   if(is_str(user) && strip(user).len > 0){ return strip(user) + "@" + host }
   host
}

fn _ssh_base_args(str: host, any: user="", int: port=22, list: options=[]): list {
   mut argv = []
   if(port > 0 && port != 22){
      argv = argv.append("-p")
      argv = argv.append(to_str(port))
   }
   mut i = 0
   while(i < options.len){
      argv = argv.append(to_str(options.get(i)))
      i += 1
   }
   argv = argv.append(_ssh_target(host, user))
   argv
}

fn _ssh_append_command(list: argv, any: command): list {
   if(command == nil || command == 0){ return argv }
   if(is_str(command)){
      if(command.len > 0){ argv = argv.append(command) }
      return argv
   }
   if(is_list(command)){
      mut i = 0
      while(i < command.len){
         argv = argv.append(to_str(command.get(i)))
         i += 1
      }
   }
   argv
}

fn ssh(str: host, any: user="", int: port=22, any: command="", list: options=[], any: level="", int: timeout_ms=-1, int: chunk_size=0): any {
   "Starts the local OpenSSH client as a tube. `command` may be a string or argv list."
   def argv = _ssh_append_command(_ssh_base_args(host, user, port, options), command)
   process("ssh", argv, level, timeout_ms, chunk_size)
}

fn ssh_process(str: host, list: command=[], any: user="", int: port=22, list: options=[], any: level="", int: timeout_ms=-1, int: chunk_size=0): any {
   "Starts an SSH command tube using a remote argv-style command list."
   ssh(host, user, port, command, options, level, timeout_ms, chunk_size)
}

fn ssh_shell(str: host, any: user="", int: port=22, list: options=[], any: level="", int: timeout_ms=-1, int: chunk_size=0): any {
   "Starts an interactive SSH shell tube using the local OpenSSH client."
   ssh(host, user, port, "", options, level, timeout_ms, chunk_size)
}

fn transcript(any: io): list {
   "Returns transcript rows captured by a tube."
   if(!is_dict(io)){ return [] }
   def rows = io.get("transcript", 0)
   is_list(rows) ? rows : []
}

fn transcript_text(any: io): str {
   "Returns a compact text rendering of the tube transcript."
   def rows = transcript(io)
   mut b, i = Builder(64), 0
   while(i < rows.len){
      def row = rows.get(i, 0)
      if(is_dict(row)){ b = builder_append(b, row.get("op", "?") + " " + repr(row.get("data", "")) + "\n") }
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn tube_kind(any: io): str {
   "Returns `tcp`, `process`, `fixture`, or `invalid`."
   if(!is_dict(io)){ return "invalid" }
   if(io.get("offline", false)){ return "fixture" }
   io.get("kind", "tcp")
}

fn remote(str: host, int: port, any: level="", int: timeout_ms=-1, int: chunk_size=0): any {
   "Connects to host:port and returns a buffered tube, or 0 on failure."
   def lvl = (is_str(level) && strip(level).len > 0) ? netctx.level_name(level) : netctx.default_level()
   _log_connect(host, port, lvl, "connect")
   def fd = sock.socket_connect(host, port)
   if(fd < 0){
      _log_connect(host, port, lvl, "connect failed")
      return 0
   }
   def io = tube_fd(fd, host, port, lvl, timeout_ms, chunk_size)
   _log(io, "info", "connected")
   return io
}

fn connect(str: host, int: port, any: level="", int: timeout_ms=-1, int: chunk_size=0): any { return remote(host, port, level, timeout_ms, chunk_size) }

fn remote_retry(str: host, int: port, int: retries=20, int: delay_ms=50, any: level="", int: timeout_ms=-1, int: chunk_size=0): any {
   "Connects with retries and returns a tube, or 0 on failure."
   if(retries < 1){ retries = 1 }
   if(delay_ms < 0){ delay_ms = 0 }
   def lvl = (is_str(level) && strip(level).len > 0) ? netctx.level_name(level) : netctx.default_level()
   mut i = 0
   while(i < retries){
      if(_level_enabled(lvl, "trace")){ _log_connect(host, port, lvl, "retry " + to_str(i + 1) + "/" + to_str(retries)) }
      def r = remote(host, port, lvl, timeout_ms, chunk_size)
      if(r != 0){ return r }
      if(delay_ms > 0){ msleep(delay_ms) }
      i += 1
   }
   return 0
}

fn connect_retry(str: host, int: port, int: retries=20, int: delay_ms=50, any: level="", int: timeout_ms=-1, int: chunk_size=0): any { return remote_retry(host, port, retries, delay_ms, level, timeout_ms, chunk_size) }

fn fileno(any: io): int {
   "Returns the underlying socket descriptor, or -1 for invalid/offline tubes."
   if(!is_dict(io)){ return -1 }
   return io.get("fd", -1)
}

fn pid(any: io): int {
   "Returns the process id for process/ssh/shell tubes, or -1 otherwise."
   if(!is_dict(io)){ return -1 }
   io.get("pid", -1)
}

fn connected(any: io): bool {
   "Returns true while the tube is usable."
   if(!is_dict(io)){ return false }
   if(io.get("offline", false)){ return !io.get("closed", false) }
   if(io.get("kind", "tcp") == "process"){ return is_dict(io.get("proc", 0)) && !io.get("closed", false) }
   return io.get("fd", -1) >= 0 && !io.get("closed", false)
}

fn set_level(any: io, any: level="debug"): any {
   "Sets the logging level on an existing tube."
   if(!is_dict(io)){ return io }
   def lvl = netctx.level_name(level)
   io.set("level", lvl)
   io.set("verbose", netctx.level_value(lvl) >= netctx.level_value("debug"))
   _log(io, "info", "level=" + lvl)
   return io
}

fn set_log_level(any: io, any: level="debug"): any { return set_level(io, level) }

fn log_level(any: io): str {
   "Returns the tube logging level."
   if(!is_dict(io)){ return "quiet" }
   io.get("level", "quiet")
}

fn set_verbose(any: io, bool: on=true): any {
   "Enables or disables debug-level tube logging."
   if(!is_dict(io)){ return io }
   return set_level(io, on ? "debug" : "quiet")
}

fn verbose(any: io, bool: on=true): any { return set_verbose(io, on) }

fn set_chunk_size(any: io, int: n): any {
   "Sets the maximum receive chunk size for a tube."
   if(!is_dict(io)){ return io }
   if(n < 1){ n = 1 }
   if(n > 1048576){ n = 1048576 }
   io.set("chunk", n)
   return io
}

fn set_timeout(any: io, int: timeout_ms): any {
   "Sets socket recv/send timeout for this tube in milliseconds."
   if(!is_dict(io)){ return io }
   if(timeout_ms < 0){ timeout_ms = 0 }
   io.set("timeout_ms", timeout_ms)
   def fd = fileno(io)
   if(fd >= 0){ sock.socket_set_timeout_ms(fd, timeout_ms) }
   return io
}

fn _buf(any: io): str {
   if(!is_dict(io)){ return "" }
   def b = io.get("buf", "")
   return is_str(b) ? b : ""
}

fn _set_buf(any: io, str: b): any {
   if(is_dict(io)){ io.set("buf", b) }
   return io
}

fn buffered(any: io): int { "Returns the number of buffered receive bytes." return _buf(io).len }

fn unrecv(any: io, any: data): int {
   "Pushes bytes back to the front of the receive buffer."
   if(!is_dict(io) || !is_str(data)){ return -1 }
   _set_buf(io, data + _buf(io))
   _log(io, "trace", "unrecv " + to_str(data.len) + "B buffered=" + to_str(_buf(io).len))
   return data.len
}

fn clean(any: io): str {
   "Returns and clears already-buffered bytes without blocking for more."
   def b = _buf(io)
   _set_buf(io, "")
   _log(io, "trace", "clean buffered=" + to_str(b.len) + "B")
   return b
}

fn _read_more(dict: io, int: want): str {
   if(!connected(io)){ return "" }
   if(want < 1){ want = io.get("chunk", 4096) }
   if(want > 1048576){ want = 1048576 }
   if(io.get("offline", false)){
      def script = io.get("script", "")
      if(!is_str(script) || script.len == 0){ return "" }
      def take = (script.len < want) ? script.len : want
      def got = slice(script, 0, take)
      io.set("script", slice(script, take, script.len))
      _record(io, "recv", got)
      _trace(io, "[<]", "recv", got)
      return got
   }
   if(io.get("kind", "tcp") == "process"){
      def p = io.get("proc", 0)
      if(!is_dict(p)){ return "" }
      def gotp = pio.recv(p, want)
      if(is_str(gotp) && gotp.len > 0){
         _record(io, "recv", gotp)
         _trace(io, "[<]", "recv", gotp)
         return gotp
      }
      return ""
   }
   mut tries = 0
   while(tries < 200){
      def got = sock.read_socket(fileno(io), want)
      if(is_int(got) || got == nil){
         tries += 1
         msleep(5)
         continue
      }
      if(is_str(got) && got.len > 0){
         _record(io, "recv", got)
         _trace(io, "[<]", "recv", got)
         return got
      }
      tries += 1
      msleep(5)
   }
   return ""
}

fn _send_all(any: io, any: data): int {
   "Sends all bytes in `data`."
   if(!is_dict(io) || !is_str(data) || !connected(io)){ return -1 }
   def _trace_ignore = (data.len > 0) ? _trace(io, "[>]", "send", data) : 0
   _record(io, "send", data)
   if(io.get("offline", false)){
      io.set("sent", io.get("sent", "") + data)
      return data.len
   }
   if(io.get("kind", "tcp") == "process"){
      def sent = pio.send(io.get("proc", 0), data)
      if(is_ok(sent)){ return unwrap(sent) }
      return -1
   }
   def fd = fileno(io)
   mut off = 0
   while(off < data.len){
      def wrote = sock.write_socket_part(fd, data, off, data.len - off)
      if(wrote <= 0){ return off == 0 ? -1 : off }
      off += wrote
   }
   return off
}

fn send(any: io, any: data): int { return _send_all(io, data) }

fn sendline(any: io, any: data=""): int {
   "Sends `data` followed by `\\n`."
   if(!is_str(data)){ data = to_str(data) }
   return _send_all(io, data + "\n")
}

fn _recv_take(any: io, any: n=4096): str {
   "Receives up to `n` bytes, using the tube buffer first."
   if(!is_dict(io)){ return "" }
   if(!is_int(n) || n <= 0){ n = 4096 }
   def b = _buf(io)
   if(b.len > 0){
      def take = (b.len < n) ? b.len : n
      def out = slice(b, 0, take)
      _set_buf(io, slice(b, take, b.len))
      return out
   }
   return _read_more(io, n)
}

fn recv(any: io, any: n=4096): str { return _recv_take(io, n) }

fn recvn(any: io, int: n): str {
   "Receives up to exactly `n` bytes, stopping early on EOF."
   if(!is_dict(io) || n <= 0){ return "" }
   mut out = Builder(n + 1)
   mut total = 0
   while(total < n){
      def chunk = _recv_take(io, n - total)
      if(chunk.len == 0){ break }
      out = builder_append(out, chunk)
      total += chunk.len
   }
   def s = builder_to_str(out)
   builder_free(out)
   return s
}

fn recv_until(any: io, any: needle, bool: drop=false, int: max_bytes=65536): str {
   "Receives until `needle` appears. Extra bytes after the needle remain buffered."
   if(!is_dict(io) || !is_str(needle)){ return "" }
   if(max_bytes <= 0){ max_bytes = 65536 }
   _log(io, "trace", "recvuntil needle=" + repr(needle) + " drop=" + to_str(drop) + " max=" + to_str(max_bytes))
   mut acc = clean(io)
   while(acc.len < max_bytes){
      def at = (needle.len == 0) ? 0 : find(acc, needle)
      if(at >= 0){
         def end = at + needle.len
         def out_end = drop ? at : end
         def out = slice(acc, 0, out_end)
         if(end < acc.len){ unrecv(io, slice(acc, end, acc.len)) }
         _log(io, "trace", "recvuntil hit len=" + to_str(out.len))
         return out
      }
      def left = max_bytes - acc.len
      def want = (left < io.get("chunk", 4096)) ? left : io.get("chunk", 4096)
      def chunk = _recv_take(io, want)
      if(chunk.len == 0){ break }
      acc = acc + chunk
   }
   _log(io, "trace", "recvuntil miss len=" + to_str(acc.len))
   return acc
}

fn recvuntil(any: io, any: needle, bool: drop=false, int: max_bytes=65536): str { return recv_until(io, needle, drop, max_bytes) }

fn recv_line(any: io, bool: keepends=true, int: max_bytes=65536): str { return recv_until(io, "\n", !keepends, max_bytes) }

fn recvline(any: io, bool: keepends=true, int: max_bytes=65536): str { return recv_line(io, keepends, max_bytes) }

fn recv_all(any: io, int: max_bytes=65536): str {
   "Receives until EOF or `max_bytes` is reached."
   if(!is_dict(io)){ return "" }
   mut b = Builder(max(16, max_bytes + 8))
   mut total = 0
   def pending = clean(io)
   if(pending.len > 0){
      b = builder_append(b, pending)
      total += pending.len
   }
   while(total < max_bytes && connected(io)){
      def left = max_bytes - total
      def want = (left < io.get("chunk", 4096)) ? left : io.get("chunk", 4096)
      def chunk = _read_more(io, want)
      if(chunk.len == 0){ break }
      b = builder_append(b, chunk)
      total += chunk.len
   }
   def out = builder_to_str(b)
   builder_free(b)
   return out
}

fn recvall(any: io, int: max_bytes=65536): str { return recv_all(io, max_bytes) }

fn expect(any: io, any: needles, int: max_bytes=65536): list {
   "Receives until any string in `needles` appears. Returns `[idx, buf]`."
   if(!is_list(needles) || needles.len == 0){ return [-1, ""] }
   mut buf = _buf(io)
   _set_buf(io, "")
   while(buf.len < max_bytes){
      mut i = 0
      while(i < needles.len){
         def nd = needles.get(i)
         if(is_str(nd)){
            def at = (nd.len == 0) ? 0 : find(buf, nd)
            if(at >= 0){
               def end = at + nd.len
               def out = slice(buf, 0, end)
               if(end < buf.len){
                  def rest = slice(buf, end, buf.len)
                  _set_buf(io, rest)
               } else {
                  _set_buf(io, "")
               }
               _log(io, "trace", "expect hit index=" + to_str(i) + " len=" + to_str(out.len))
               def res = [i, out]
               return res
            }
         }
         i += 1
      }
      def left = max_bytes - buf.len
      def want = (left < io.get("chunk", 4096)) ? left : io.get("chunk", 4096)
      def chunk = _read_more(io, want)
      if(chunk.len == 0){ break }
      buf = buf + chunk
   }
   _log(io, "trace", "expect miss len=" + to_str(buf.len))
   return [-1, buf]
}

fn expect_map(any: io, any: mapping, int: max_bytes=65536): list {
   "Like `expect`, but maps matched needle to a caller-provided tag."
   if(!is_dict(mapping)){ return [nil, ""] }
   def ks = mapping.keys()
   def res = expect(io, ks, max_bytes)
   def idx = res.get(0, -1)
   if(idx < 0){ return [nil, res.get(1, "")] }
   def key = ks.get(idx)
   return [mapping.get(key, nil), res.get(1, "")]
}

fn send_after(any: io, any: needle, any: data, int: max_bytes=65536): str {
   "Receives through `needle`, then sends `data`."
   def seen = recv_until(io, needle, false, max_bytes)
   _send_all(io, data)
   return seen
}

fn sendafter(any: io, any: needle, any: data, int: max_bytes=65536): str { return send_after(io, needle, data, max_bytes) }

fn sendline_after(any: io, any: needle, any: data, int: max_bytes=65536): str {
   "Receives through `needle`, then sends `data` plus a newline."
   def seen = recv_until(io, needle, false, max_bytes)
   if(!is_str(data)){ data = to_str(data) }
   _send_all(io, data + "\n")
   return seen
}

fn sendlineafter(any: io, any: needle, any: data, int: max_bytes=65536): str { return sendline_after(io, needle, data, max_bytes) }

fn shutdown_send(any: io): int {
   "Closes the sending side for process tubes. Socket tubes currently return 0."
   if(!is_dict(io)){ return -1 }
   if(io.get("kind", "tcp") == "process"){
      def res = pio.shutdown_send(io.get("proc", 0))
      if(is_ok(res)){ return unwrap(res) }
      return -1
   }
   return 0
}

fn close(any: io): int {
   "Closes a tube and returns the backend close status."
   if(!is_dict(io)){ return -1 }
   def fd = io.get("fd", -1)
   _log(io, "info", "close")
   io.set("fd", -1)
   io.set("closed", true)
   if(io.get("offline", false)){ return 0 }
   if(io.get("kind", "tcp") == "process"){
      def res = pio.close(io.get("proc", 0))
      if(is_ok(res)){ return unwrap(res) }
      return -1
   }
   if(fd >= 0){ return sock.close_socket(fd) }
   return 0
}

fn _write_fd(int: fd, any: data): int {
   if(!is_str(data)){ data = to_str(data) }
   mut off = 0
   while(off < data.len){
      match sys.sys_write(fd, to_int(data) + off, data.len - off){
         ok(w) -> {
            if(w <= 0){ return off }
            off += w
         }
         err(ignorederr) -> { ignorederr  return off }
      }
   }
   off
}

fn _read_stdin_byte(): str {
   def p = malloc(2)
   if(!p){ return "" }
   match sys.sys_read(0, p, 1){
      ok(n) -> {
         if(n <= 0){ free(p) return "" }
         init_str(p, 1)
         return p
      }
      err(ignorederr) -> { ignorederr  free(p) return "" }
   }
}

fn _interactive_opt_bool(any: options, str: key, bool: fallback): bool {
   if(!is_dict(options)){ return fallback }
   def v = options.get(key, fallback)
   if(is_int(v)){ return v != 0 }
   if(is_str(v)){
      def s = lower(strip(v))
      return !(s == "" || s == "0" || s == "false" || s == "off" || s == "no")
   }
   v ? true : false
}

fn interactive(any: io, int: max_read=4096, any: options=0): int {
   "Interactive tube bridge: forwards remote output to stdout and raw stdin to the tube. Ctrl-D detaches."
   if(!is_dict(io)){ return -1 }
   if(max_read <= 0){ max_read = 4096 }
   def raw = _interactive_opt_bool(options, "raw", true)
   def banner = _interactive_opt_bool(options, "banner", true)
   if(banner){ print(netctx.paint("[*]", "green", 1) + " interactive " + _ctx(io) + " " + netctx.paint("(Ctrl-D to detach)", "gray", 0)) }
   if(raw){ __tty_raw(1) }
   defer {
      if(raw){
         __tty_raw(0)
         __tty_sane_fd(0)
      }
      if(banner){ print(netctx.paint("[*]", "green", 1) + " interactive closed") }
   }
   while(connected(io)){
      def chunk = _recv_take(io, max_read)
      if(chunk.len > 0){ _write_fd(1, chunk) }
      mut did_input = false
      while(__tty_pending() > 0){
         def ch = _read_stdin_byte()
         if(ch.len == 0){ return 0 }
         if(load8(ch, 0) == 4){ return 0 }
         _send_all(io, ch)
         did_input = true
      }
      if(chunk.len == 0 && !did_input){ msleep(10) }
   }
   return 0
}
