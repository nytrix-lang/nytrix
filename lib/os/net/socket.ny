;; Keywords: net socket tcp udp os
;; Net Socket for Nytrix
;; References:
;; - std.os.net
;; - std.os
module std.os.net.socket(htons, ipv4_parse, ipv4_format, gethostbyname, _make_sockaddr, socket_connect, socket_bind, socket_accept, socket_accept_info, read_socket, write_socket, socket_connect_async, socket_accept_async, read_socket_async, write_socket_part_async, write_socket_all_async, read_socket_until_async, socket_set_timeout_ms, socket_set_recv_timeout_ms, socket_set_send_timeout_ms, read_socket_exact, write_socket_part, write_socket_all, write_socket_line, read_socket_until, close_socket)
use std.core
use std.core.str
use std.core.reflect
use std.os
use std.os.sys
use std.os.path
use std.os.path as ospath
use std.core.mem as mem

#windows {
   #link "ws2_32.lib"
   extern "" {
      fn _c_wsa_startup(u16 version, ptr data) i32 as "WSAStartup"
      fn _c_socket(i32 domain, i32 typ, i32 protocol) i64 as "socket"
      fn _c_connect(i64 fd, ptr addr, i32 addrlen) i32 as "connect"
      fn _c_bind(i64 fd, ptr addr, i32 addrlen) i32 as "bind"
      fn _c_listen(i64 fd, i32 backlog) i32 as "listen"
      fn _c_accept(i64 fd, ptr addr, ptr addrlen) i64 as "accept"
      fn _c_send(i64 fd, ptr buf, i32 len, i32 flags) i32 as "send"
      fn _c_recv(i64 fd, ptr buf, i32 len, i32 flags) i32 as "recv"
      fn _c_sendto(i64 fd, ptr buf, i32 len, i32 flags, ptr addr, i32 addrlen) i32 as "sendto"
      fn _c_recvfrom(i64 fd, ptr buf, i32 len, i32 flags, ptr addr, ptr addrlen) i32 as "recvfrom"
      fn _c_setsockopt(i64 fd, i32 level, i32 optname, ptr optval, i32 optlen) i32 as "setsockopt"
      fn _c_close_socket(i64 fd) i32 as "closesocket"
      fn _c_gethostbyname(ptr name) ptr as "gethostbyname"
   }
} #else {
   extern "" {
      fn _c_socket(i32 domain, i32 typ, i32 protocol) i32 as "socket"
      fn _c_connect(i32 fd, ptr addr, u32 addrlen) i32 as "connect"
      fn _c_bind(i32 fd, ptr addr, u32 addrlen) i32 as "bind"
      fn _c_listen(i32 fd, i32 backlog) i32 as "listen"
      fn _c_accept(i32 fd, ptr addr, ptr addrlen) i32 as "accept"
      fn _c_send(i32 fd, ptr buf, u64 len, i32 flags) i64 as "send"
      fn _c_recv(i32 fd, ptr buf, u64 len, i32 flags) i64 as "recv"
      fn _c_sendto(i32 fd, ptr buf, u64 len, i32 flags, ptr addr, u32 addrlen) i64 as "sendto"
      fn _c_recvfrom(i32 fd, ptr buf, u64 len, i32 flags, ptr addr, ptr addrlen) i64 as "recvfrom"
      fn _c_setsockopt(i32 fd, i32 level, i32 optname, ptr optval, u32 optlen) i32 as "setsockopt"
      fn _c_close_socket(i32 fd) i32 as "close"
      fn _c_gethostbyname(ptr name) ptr as "gethostbyname"
   }
} #endif
def AF_INET     = 2
def SOCK_STREAM = 1
#if(windows || macos){
   def SOL_SOCKET = 65535
   def SO_REUSEADDR = 4
   def SO_SNDTIMEO = 0x1005
   def SO_RCVTIMEO = 0x1006
} #else {
   def SOL_SOCKET = 1
   def SO_REUSEADDR = 2
   def SO_RCVTIMEO = 20
   def SO_SNDTIMEO = 21
} #endif
mut _hosts_cache_loaded = false
mut _hosts_cache_txt = ""
mut _net_ready_done = false

fn _cptr(any s) any { to_int(mem.cstr(s)) }

fn _net_ready() bool {
   #windows {
      if(_net_ready_done){ return true }
      def data = malloc(512)
      if(!data){ return false }
      def ok = _c_wsa_startup(0x0202, data) == 0
      free(data)
      _net_ready_done = ok
      return ok
   } #else {
      return true
   } #endif
}

fn _net_socket(int domain, int typ, int protocol) int {
   if(!_net_ready()){ return -1 }
   def fd = _c_socket(domain, typ, protocol)
   #windows { if(fd == -1){ return -1 } } #endif
   return fd
}

fn _net_close(int fd) int {
   if(fd < 0){ return -1 }
   return _c_close_socket(fd)
}

fn htons(int x) int {
   "Convert a 16-bit integer from host byte order to network byte order(big-endian)."
   def lo = x % 256
   def hi = (x / 256) % 256
   return lo*256 + hi
}

fn ipv4_parse(str s) int {
   "Parses an IPv4 address string(e.g., '127.0.0.1') into a 32-bit integer(little-endian)."
   mut p, val, oct, shift, parts, digits = 0, 0, 0, 0, 0, 0
   while(p <= s.len){
      mut c = (p < s.len) ? load8(s, p) : 0
      if(c==0 || c==46){
         if(digits == 0 || oct > 255 || parts >= 4){ return 0 }
         mut scale = 0
         if(shift == 0){ scale = 1 }
         elif(shift == 8){ scale = 256 }
         elif(shift == 16){ scale = 65536 }
         elif(shift == 24){ scale = 16777216 }
         else { return 0 }
         val = val + oct * scale
         parts += 1
         if(c == 0){ return(parts == 4) ? val : 0 }
         oct = 0
         digits = 0
         shift = shift + 8
         p += 1
      } else {
         if(c < 48 || c > 57){ return 0 }
         oct = oct * 10 + (c - 48)
         digits += 1
         p += 1
      }
   }
   return 0
}

fn ipv4_format(int ip) str {
   "Formats an IPv4 address integer returned by Ny socket helpers."
   def a = ip % 256
   def b = (ip / 256) % 256
   def c = (ip / 65536) % 256
   def d = (ip / 16777216) % 256
   to_str(a) + "." + to_str(b) + "." + to_str(c) + "." + to_str(d)
}

fn _hosts_file_path() str {
   #windows {
      mut root = env("SystemRoot")
      if(!is_str(root) || root.len == 0){ root = env("SYSTEMROOT") }
      if(is_str(root) && root.len > 0){ return ospath.normalize(root + "\\System32\\drivers\\etc\\hosts") }
      return ospath.normalize("C:\\Windows\\System32\\drivers\\etc\\hosts")
   } #else {
      return ospath.normalize("/etc/hosts")
   } #endif
}

fn _lookup_hosts(str name, any hosts) int {
   if(!is_str(hosts) || hosts.len == 0){ return 0 }
   def lines = split(hosts, "\n")
   mut i = 0
   while(i < lines.len){
      def line = strip(lines.get(i))
      if(line.len > 0 && load8(line, 0) != 35){
         def parts = split(line, " ")
         if(parts.len >= 2){
            def ip_str = strip(parts.get(0))
            mut j = 1
            while(j < parts.len){
               def host_alias = strip(parts.get(j))
               if(host_alias.len > 0 && host_alias == name){ return ipv4_parse(ip_str) }
               j += 1
            }
         }
      }
      i += 1
   }
   0
}

fn gethostbyname(any name) int {
   "Resolves a hostname to an IPv4 address integer using hosts plus the OS resolver."
   if(!is_str(name) || name.len == 0){ return 0 }
   def direct = ipv4_parse(name)
   if(direct != 0){ return direct }
   match name {
      "localhost" -> { return ipv4_parse("127.0.0.1") }
      "127.0.0.1" -> { return ipv4_parse("127.0.0.1") }
      _ -> {}
   }
   def hosts_path = _hosts_file_path()
   if(!_hosts_cache_loaded){
      match file_read(hosts_path){
         ok(s) -> { _hosts_cache_txt = s }
         err(ignorederr) -> { ignorederr  _hosts_cache_txt = "" }
      }
      _hosts_cache_loaded = true
   }
   def host_ip = _lookup_hosts(name, _hosts_cache_txt)
   if(host_ip != 0){ return host_ip }
   if(!_net_ready()){ return 0 }
   def h = _c_gethostbyname(_cptr(name))
   if(!h){ return 0 }
   def listp = load64(h, 24)
   if(!listp){ return 0 }
   def addrp = load64(listp, 0)
   if(!addrp){ return 0 }
   def ip = load32(addrp, 0)
   ip
}

fn _resolve_host(str host) int {
   mut ip = ipv4_parse(host)
   if(ip == 0){ ip = gethostbyname(host) }
   return ip
}

fn _resolve_bind_host(str host) int {
   if(host.len == 0 || host == "*" || host == "0.0.0.0"){ return 0 }
   _resolve_host(host)
}

#macos {
   fn _make_sockaddr(int ip, int port) any {
      def sa = malloc(16)
      if(sa == 0){ return 0 }
      store8(sa, 16, 0)
      store8(sa, AF_INET, 1)
      store16(sa, htons(port), 2)
      store32(sa, ip, 4)
      store64(sa, 0, 8)
      return sa
   }
} #else {
   fn _make_sockaddr(int ip, int port) any {
      def sa = malloc(16)
      if(sa == 0){ return 0 }
      store16(sa, AF_INET, 0)
      store16(sa, htons(port), 2)
      store32(sa, ip, 4)
      store64(sa, 0, 8)
      return sa
   }
} #endif

fn socket_connect(str host, int port) int {
   "Connect to host:port(TCP). Returns fd or -1."
   def ip = _resolve_host(host)
   if(ip == 0){ return -1 }
   def fd = _net_socket(AF_INET, SOCK_STREAM, 0)
   if(fd < 0){ return -1 }
   def sa = _make_sockaddr(ip, port)
   if(sa == 0){
      _net_close(fd)
      return -1
   }
   def rc = _c_connect(fd, sa, 16)
   free(sa)
   if(rc < 0){
      _net_close(fd)
      return -1
   }
   return fd
}

fn socket_connect_async(str host, int port) any {
   "Starts a non-blocking TCP connect task. Await it for fd or -1."
   def ip = _resolve_host(host)
   if(ip == 0){ return __async_value(-1) }
   def fd = _net_socket(AF_INET, SOCK_STREAM, 0)
   if(fd < 0){ return __async_value(-1) }
   def sa = _make_sockaddr(ip, port)
   if(sa == 0){
      _net_close(fd)
      return __async_value(-1)
   }
   def h = __async_connect(fd, sa, 16)
   free(sa)
   h
}

fn socket_bind(str host, int port) int {
   "Create a TCP socket, binds it to the specified host and port, and starts listening. Returns the file descriptor or -1 on error."
   def ip = _resolve_bind_host(host)
   if(ip == 0 && !(host.len == 0 || host == "*" || host == "0.0.0.0")){ return -1 }
   def fd = _net_socket(AF_INET, SOCK_STREAM, 0)
   if(fd < 0){
      print("Socket create failed, errno:", errno())
      return -1
   }
   def opt = malloc(4)
   if(opt == 0){
      print("Socket bind setup failed, errno:", errno())
      _net_close(fd)
      return -1
   }
   store32(opt, 1)
   _c_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, opt, 4)
   free(opt)
   def sa = _make_sockaddr(ip, port)
   if(sa == 0){
      _net_close(fd)
      return -1
   }
   def br = _c_bind(fd, sa, 16)
   free(sa)
   if(br < 0){
      print("Socket bind failed, errno:", errno())
      _net_close(fd)
      return -1
   }
   def lr = _c_listen(fd, 128)
   if(lr < 0){
      print("Socket listen failed, errno:", errno())
      _net_close(fd)
      return -1
   }
   return fd
}

fn socket_accept(int server_fd) int {
   "Accepts an incoming connection on a listening socket. Returns the client file descriptor."
   socket_accept_info(server_fd).get("fd", -1)
}

fn socket_accept_info(int server_fd) dict {
   "Accepts an incoming connection and returns fd plus peer IPv4 metadata."
   def addr = malloc(16)
   def len = malloc(4)
   if(addr == 0 || len == 0){
      if(addr != 0){ free(addr) }
      if(len != 0){ free(len) }
      return {"ok": false, "fd": -1, "host": "", "ip": 0, "port": 0, "addr": ""}
   }
   defer { free(addr) }
   defer { free(len) }
   store32(len, 16)
   def res = _c_accept(server_fd, addr, len)
   if(res < 0){ return {"ok": false, "fd": -1, "host": "", "ip": 0, "port": 0, "addr": ""} }
   def ip = load32(addr, 4)
   def port = htons(load16(addr, 2))
   def host = ipv4_format(ip)
   return {"ok": true, "fd": res, "host": host, "ip": ip, "port": port, "addr": host + ":" + to_str(port)}
}

fn socket_accept_async(int server_fd) any {
   "Starts an accept task. Await it for the accepted client fd or -1."
   __async_accept(server_fd)
}

fn _socket_set_timeval_opt(int fd, int optname, int timeout_ms) int {
   if(fd < 0){ return -1 }
   if(timeout_ms < 0){ timeout_ms = 0 }
   #windows {
      def opt = malloc(4)
      if(opt == 0){ return -1 }
      store32(opt, timeout_ms)
      def rc = _c_setsockopt(fd, SOL_SOCKET, optname, opt, 4)
      free(opt)
      return rc
   } #else {
      def tv = malloc(16)
      if(tv == 0){ return -1 }
      store64(tv, timeout_ms / 1000, 0)
      store64(tv, (timeout_ms % 1000) * 1000, 8)
      def rc = _c_setsockopt(fd, SOL_SOCKET, optname, tv, 16)
      free(tv)
      return rc
   } #endif
}

fn socket_set_recv_timeout_ms(int fd, int timeout_ms) int {
   "Sets socket receive timeout in milliseconds. `0` asks the OS for blocking/default behavior."
   return _socket_set_timeval_opt(fd, SO_RCVTIMEO, timeout_ms)
}

fn socket_set_send_timeout_ms(int fd, int timeout_ms) int {
   "Sets socket send timeout in milliseconds. `0` asks the OS for blocking/default behavior."
   return _socket_set_timeval_opt(fd, SO_SNDTIMEO, timeout_ms)
}

fn socket_set_timeout_ms(int fd, int timeout_ms) int {
   "Sets both receive and send socket timeouts in milliseconds."
   def r1, r2 = socket_set_recv_timeout_ms(fd, timeout_ms), socket_set_send_timeout_ms(fd, timeout_ms)
   (r1 < 0 || r2 < 0) ? -1 : 0
}

fn read_socket(int fd, any max_len) any {
   "Reads up to `max_len` bytes from a socket. Returns the data as a string."
   if(!is_int(max_len) || max_len <= 0){ return "" }
   if(max_len > 1048576){ max_len = 1048576 }
   def base = malloc(max_len + 17)
   if(base == 0){ return "" }
   def buf = base + 16
   def n = _c_recv(fd, buf, max_len, 0)
   if(n <= 0){
      free(base)
      return ""
   }
   store8(buf, 0, n)
   return init_str(buf, n)
}

fn read_socket_async(int fd, any max_len) any {
   "Starts a socket read task returning a string when awaited."
   if(!is_int(max_len) || max_len <= 0){ return __async_value("") }
   if(max_len > 1048576){ max_len = 1048576 }
   __async_read_socket(fd, max_len)
}

fn write_socket(int fd, any data) int {
   "Writes all bytes in `data` to a socket."
   return write_socket_all(fd, data)
}

fn read_socket_exact(int fd, any want_len) str {
   "Reads until `want_len` bytes are collected, EOF happens, or the peer stops sending."
   if(!is_int(want_len) || want_len <= 0){ return "" }
   if(want_len > 1048576){ want_len = 1048576 }
   mut b = Builder(want_len + 1)
   mut total = 0
   while(total < want_len){
      def chunk = read_socket(fd, want_len - total)
      if(chunk.len == 0){ break }
      b = builder_append(b, chunk)
      total += chunk.len
   }
   def out = builder_to_str(b)
   builder_free(b)
   return out
}

fn write_socket_part(int fd, any data, any off, any size=-1) int {
   "Writes a string slice `[off, off+size)` to socket without allocating substrings."
   if(!is_str(data) || !is_int(off)){ return -1 }
   def n = data.len
   if(off < 0){ off = 0 }
   if(off >= n){ return 0 }
   mut count = size
   if(!is_int(count) || count < 0 || off + count > n){ count = n - off }
   if(count <= 0){ return 0 }
   return _c_send(fd, to_int(data) + off, count, 0)
}

fn write_socket_part_async(int fd, any data, any off, any size=-1) any {
   "Starts a socket write task for a string slice. Await it for bytes written or -1."
   if(!is_str(data) || !is_int(off)){ return __async_value(-1) }
   def n = data.len
   if(off < 0){ off = 0 }
   if(off >= n){ return __async_value(0) }
   mut count = size
   if(!is_int(count) || count < 0 || off + count > n){ count = n - off }
   if(count <= 0){ return __async_value(0) }
   __async_write_socket_part(fd, data, off, count)
}

fn write_socket_all(int fd, any data) int {
   "Writes all bytes in `data` unless the peer closes or send fails. Returns bytes written, or -1."
   if(!is_str(data)){ return -1 }
   mut off = 0
   def n = data.len
   while(off < n){
      def wrote = write_socket_part(fd, data, off, n - off)
      if(wrote <= 0){ return off == 0 ? -1 : off }
      off += wrote
   }
   return off
}

fn write_socket_all_async(int fd, any data) any {
   "Starts a socket write task that sends the whole string. Await it for bytes written or -1."
   if(!is_str(data)){ return __async_value(-1) }
   __async_write_socket_all(fd, data)
}

fn write_socket_line(int fd, any data) int {
   "Writes `data` plus a trailing newline."
   if(!is_str(data)){ return -1 }
   return write_socket_all(fd, data + "\n")
}

fn read_socket_until(int fd, any needle, any max_bytes=65536) str {
   "Reads from socket until `needle` appears, EOF, or `max_bytes` is reached."
   if(!is_str(needle)){ return "" }
   if(!is_int(max_bytes) || max_bytes <= 0){ max_bytes = 65536 }
   mut buf = ""
   while(buf.len < max_bytes){
      def at = find(buf, needle)
      if(needle.len == 0 || at >= 0){
         def end = at + needle.len
         return slice(buf, 0, end)
      }
      def left = max_bytes - buf.len
      def want = (left < 4096) ? left : 4096
      def chunk = read_socket(fd, want)
      if(!is_str(chunk)){ break }
      if(chunk.len == 0){ break }
      buf = buf + chunk
   }
   return buf
}

fn read_socket_until_async(int fd, any needle, any max_bytes=65536) any {
   "Starts a task that reads until `needle`, EOF, or `max_bytes`."
   if(!is_str(needle)){ return __async_value("") }
   if(!is_int(max_bytes) || max_bytes <= 0){ max_bytes = 65536 }
   __async_read_socket_until(fd, needle, max_bytes)
}

fn close_socket(int fd) int {
   "Closes a socket file descriptor."
   return _net_close(fd)
}

#main {
   assert(htons(0x1234) == 0x3412, "socket htons")
   def loop = ipv4_parse("127.0.0.1")
   assert(loop != 0, "socket ipv4 parse")
   assert(ipv4_format(loop) == "127.0.0.1", "socket ipv4 format")
   assert(ipv4_parse("999.1.1.1") == 0, "socket invalid ipv4")
   assert(socket_set_timeout_ms(-1, 100) == -1, "socket invalid timeout")
   assert(close_socket(-1) == -1, "socket invalid close")
   print("✓ std.os.net.socket self-test passed")
}
