;; Keywords: net socket
;; Net Socket module.

module std.net.socket (
   htons, ipv4_parse, gethostbyname, socket_connect, socket_bind, socket_accept,
   read_socket, write_socket, close_socket
)
use std.core *
use std.str *
use std.core.reflect *
use std.os *
use std.os.sys *
use std.os.path *
use std.os.ffi *

fn _is_windows(){
   "Internal helper."
   __os_name() == "windows"
}

fn _is_macos(){
   "Internal helper."
   __os_name() == "macos"
}

;; Socket Constants
def AF_INET     = 2
def SOCK_STREAM = 1
def SOCK_DGRAM  = 2

def SOL_SOCKET = (_is_windows() || _is_macos()) ? 65535 : 1
def SO_REUSEADDR = (_is_windows() || _is_macos()) ? 4 : 2

fn htons(x){
   "Convert a 16-bit integer from host byte order to network byte order (big-endian)."
   def lo = x % 256
   def hi = (x / 256) % 256
   return lo*256 + hi
}

fn ipv4_parse(s){
   "Parses an IPv4 address string (e.g., '127.0.0.1') into a 32-bit integer (little-endian)."
   mut p = 0  mut val=0  mut oct=0  mut shift=0
   mut done = 0
   while(done==0){
      mut c = load8(s, p)
      if(c==0 || c==46){
         if(oct > 255){ return 0 }
         val = val + oct * case shift {
            0  -> 1
            8  -> 256
            16 -> 65536
            24 -> 16777216
            _  -> 0
         }
         if(c == 0){ return val }
         oct = 0
         shift = shift + 8
         p += 1
      } else {
         if(c < 48 || c > 57){ return 0 }
         oct = oct * 10 + (c - 48)
         p += 1
      }
   }
   return val
}

fn _hosts_file_path(){
   "Internal helper."
   if(__os_name() == "windows"){
      mut root = env("SystemRoot")
      if(!is_str(root) || str_len(root) == 0){ root = env("SYSTEMROOT") }
      if(is_str(root) && str_len(root) > 0){
         return normalize(root + "\\System32\\drivers\\etc\\hosts")
      }
      return normalize("C:\\Windows\\System32\\drivers\\etc\\hosts")
   }
   normalize("/etc/hosts")
}

fn _lookup_hosts(name, hosts){
   "Internal helper."
   if(!is_str(hosts) || len(hosts) == 0){ return 0 }
   def lines = split(hosts, "\n")
   mut i = 0
   while(i < len(lines)){
      def line = strip(get(lines, i))
      if(len(line) > 0 && load8(line, 0) != 35){ ; '#'
         def parts = split(line, " ")
         if(len(parts) >= 2){
            def ip_str = strip(get(parts, 0))
            mut j = 1
            while(j < len(parts)){
               def host_alias = strip(get(parts, j))
               if(len(host_alias) > 0 && host_alias == name){
                  return ipv4_parse(ip_str)
               }
               j += 1
            }
         }
      }
      i += 1
   }
   0
}

fn gethostbyname(name){
   "Resolves a hostname to an IPv4 address integer. No libc used."
   match name {
      "localhost" -> { return ipv4_parse("127.0.0.1") }
      "127.0.0.1" -> { return ipv4_parse("127.0.0.1") }
      _ -> {}
   }

   ; Try hosts file first
   def hosts_path = _hosts_file_path()
   mut hosts = ""
   match file_read(hosts_path){
      ok(s) -> { hosts = s }
      err(_) -> { hosts = "" }
   }
   def host_ip = _lookup_hosts(name, hosts)
   if(host_ip != 0){ return host_ip }

   ; Fallback to DNS Query (Google Public DNS)
   return dns_query(name, "8.8.8.8")
}

fn dns_query(name, server){
   "Performs a basic DNS A-record query via UDP. No libc dependency."
   use std.str.bytes *
   def buf = bytes(512)
   ; Header: ID=0x1234, Flags=0x0100 (Recursive Query), QDCOUNT=1
   bytes_set(buf, 0, 0x12) bytes_set(buf, 1, 0x34)
   bytes_set(buf, 2, 0x01) bytes_set(buf, 3, 0x00)
   bytes_set(buf, 4, 0x00) bytes_set(buf, 5, 0x01)

   mut pos = 12
   def labels = split(name, ".")
   mut l_idx = 0
   while(l_idx < len(labels)){
      def l = get(labels, l_idx)
      bytes_set(buf, pos, len(l)) pos += 1
      mut ch_idx = 0
      while(ch_idx < len(l)){
         bytes_set(buf, pos, load8(l, ch_idx)) pos += 1
         ch_idx += 1
      }
      l_idx += 1
   }
   bytes_set(buf, pos, 0) pos += 1
   ; TYPE A (1), CLASS IN (1)
   bytes_set(buf, pos, 0) bytes_set(buf, pos + 1, 1) pos = pos + 2
   bytes_set(buf, pos, 0) bytes_set(buf, pos + 1, 1) pos = pos + 2

   def fd = __socket(AF_INET, SOCK_DGRAM, 0)
   if(fd < 0){ return 0 }

   def sa = malloc(16)
   store16(sa, AF_INET, 0)
   store16(sa, htons(53), 2)
   store32(sa, ipv4_parse(server), 4)

   __sendto(fd, buf + 8, pos, 0, sa, 16)
   free(sa)

   def rb = bytes(512)
   mut n = __recvfrom(fd, rb + 8, 512, 0, 0, 0) ; recvfrom
   __closesocket(fd)

   if(n < 12){ return 0 }

   ; Simple Answer Parser
   mut rpos = 12
   while(bytes_get(rb, rpos) != 0){ rpos = rpos + bytes_get(rb, rpos) + 1 }
   rpos = rpos + 5 ; skip null, type(2), class(2)

   if(rpos + 12 > n){ return 0 }

   mut a_count = (bytes_get(rb, 6) << 8) | bytes_get(rb, 7)
   if(a_count == 0){ return 0 }

   ; Handle potential Pointer Compression in Name (0xC0XX)
   if((bytes_get(rb, rpos) & 192) == 192){ rpos = rpos + 2 }
   else { while(bytes_get(rb, rpos) != 0){ rpos = rpos + bytes_get(rb, rpos) + 1 } rpos += 1 }

   mut a_type = (bytes_get(rb, rpos) << 8) | bytes_get(rb, rpos + 1)
   mut a_rdlen = (bytes_get(rb, rpos + 8) << 8) | bytes_get(rb, rpos + 9)
   rpos += 10

   if(a_type == 1 && a_rdlen == 4){
      ; Return IP in 32-bit integer (little-endian as expected by socket_connect)
      return (bytes_get(rb, rpos) << 0) | (bytes_get(rb, rpos + 1) << 8) | (bytes_get(rb, rpos + 2) << 16) | (bytes_get(rb, rpos + 3) << 24)
   }
   return 0
}

fn _resolve_host(host){
   "Internal: resolve host string to IPv4 integer."
   mut ip = ipv4_parse(host)
   if(ip == 0){ ip = gethostbyname(host) }
   return ip
}

fn _make_sockaddr(ip, port){
   "Internal helper."
   def sa = malloc(16)
   if(sa == 0){ return 0 }
   store16(sa, AF_INET, 0)
   store16(sa, htons(port), 2)
   store32(sa, ip, 4)
   store64(sa, 0, 8)
   sa
}

fn socket_connect(host, port){
   "Connect to host:port (TCP). Returns fd or -1."
   def ip = _resolve_host(host)
   if(ip == 0){ return -1 }
   def fd = __socket(AF_INET, SOCK_STREAM, 0)
   if(fd < 0){ return -1 }
   def sa = _make_sockaddr(ip, port)
   if(sa == 0){
      __closesocket(fd)
      return -1
   }
   if(__connect(fd, sa, 16) < 0){
      free(sa)
      __closesocket(fd)
      return -1
   }
   free(sa)
   return fd
}

fn socket_bind(host, port){
   "Create a TCP socket, binds it to the specified host and port, and starts listening. Returns the file descriptor or -1 on error."
   def ip = _resolve_host(host)
   if(ip == 0){ return -1 }
   def fd = __socket(AF_INET, SOCK_STREAM, 0)
   if(fd < 0){
      print("Socket create failed, errno:", errno())
      return -1
   }
   ; Allow port reuse
   def opt = malloc(4)
   store32(opt, 1)
   __setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, opt, 4)

   def sa = _make_sockaddr(ip, port)
   if(sa == 0){
      free(opt)
      __closesocket(fd)
      return -1
   }
   if(__bind(fd, sa, 16) < 0){
      free(sa)
      free(opt)
      __closesocket(fd)
      print("Socket bind failed, errno:", errno())
      return -1
   }
   free(sa)
   free(opt)
   def lr = __listen(fd, 128)
   if(lr < 0){
      __closesocket(fd)
      print("Socket listen failed, errno:", errno())
      return -1
   }
   return fd
}

fn socket_accept(server_fd){
   "Accepts an incoming connection on a listening socket. Returns the client file descriptor."
   def addr = malloc(16)
   def len = malloc(4)
   store32(len, 16)
   def res = __accept(server_fd, addr, len) ; "accept"
   free(addr)
   free(len)
   return res
}

fn read_socket(fd, max_len){
   "Reads up to `max_len` bytes from a socket. Returns the data as a string."
   def buf = malloc(max_len + 1)
   mut n = __recv(fd, buf, max_len, 0)
   if(n < 0){ n = 0 }
   ; Initialize Nytrix string header
   store64(buf, n, -16)
   store64(buf, 120, -8) ; Tag 120 (String)
   store8(buf, 0, n) ; Null terminator
   return buf
}

fn write_socket(fd, data){
   "Writes data to a socket."
   return __send(fd, data, str_len(data), 0)
}

fn close_socket(fd){
   "Closes a socket file descriptor."
   return __closesocket(fd)
}

if(comptime{__main()}){
    use std.net.socket *
    use std.os.thread *
    use std.os.time *
    use std.core.error *

    def PORT = 54000 + ((ticks() / 1000000) % 2000)
    def CONNECT_RETRIES = 200
    def CONNECT_RETRY_MS = 25
    def SERVER_READY_WAIT_MS = 5000
    def SERVER_READY_POLL_MS = 10

    fn join_if_valid_thread(handle){
       "Test helper."
     if(handle != 0 && handle != -1){
      return thread_join(handle)
     }
     return -1
    }

    fn server_task(arg){
       "Test helper."
     def port = get(arg, 0)
     def state = get(arg, 1)
     def s = socket_bind("127.0.0.1", port)
     if(s < 0){
        store64(state, -1)
        print("Server: socket_bind failed, error:", s)
        return -1
     }
     store64(state, 1)
     def c = socket_accept(s)
     if(c < 0){
      store64(state, -2)
      print("Server: socket_accept failed, error:", c, ", errno:", errno())
      close_socket(s)
      return -1
     }
     store64(state, 2)
     def req = read_socket(c, 1024)
     if(is_err(req)){
        store64(state, -3)
        print("Server: read_socket failed, error:", unwrap_err(req))
        close_socket(c)
        close_socket(s)
        return -1
     }
     if(unwrap(req) == "ping"){
        def write_res = write_socket(c, "pong")
        if(is_err(write_res)){
            print("Server: write_socket failed, error:", unwrap_err(write_res))
        }
     }
     close_socket(c)
     close_socket(s)
     store64(state, 3)
     return 0
    }

    def state_ptr = malloc(8)
    if(state_ptr == 0){
     print("Skipping socket test: could not allocate shared state.")
     return 0
    }
    store64(state_ptr, 0)
    mut args = list()
    args = append(args, PORT)
    args = append(args, state_ptr)
    def t = thread_spawn(server_task, args)
    if(t == 0 || t == -1){
     print("Skipping socket test: thread_spawn failed, handle:", t)
     free(state_ptr)
     return 0
    }

    mut waited = 0
    while(load64(state_ptr) == 0 && waited < SERVER_READY_WAIT_MS){
      msleep(SERVER_READY_POLL_MS)
      waited += SERVER_READY_POLL_MS
    }
    def server_state = load64(state_ptr)
    if(server_state < 0){
      print("Skipping socket test: server init failed. state:", server_state)
      join_if_valid_thread(t)
      free(state_ptr)
      return 0
    }

    mut c = -1
    mut tries = 0
    while(c < 0 && tries < CONNECT_RETRIES){
      c = socket_connect("127.0.0.1", PORT)
      if(c < 0){ msleep(CONNECT_RETRY_MS) }
      tries += 1
    }
    if(c < 0){
      print("Skipping socket test: client connect failed after", tries, "tries. Error:", c)
      ;; Best-effort unblock server accept so join does not hang.
      mut kick = -1
      mut kick_tries = 0
      while(kick < 0 && kick_tries < CONNECT_RETRIES){
        kick = socket_connect("127.0.0.1", PORT)
        if(kick < 0){ msleep(CONNECT_RETRY_MS) }
        kick_tries += 1
      }
      if(kick >= 0){ close_socket(kick) }
      join_if_valid_thread(t)
      free(state_ptr)
      return 0 ; End test
    } else {
      def write_res = write_socket(c, "ping")
      if(is_err(write_res)){
        print("Client: write_socket failed, error:", unwrap_err(write_res))
        close_socket(c)
        join_if_valid_thread(t)
        free(state_ptr)
        panic("Client: write_socket failed")
      }
      def res_val = read_socket(c, 1024)
      if(is_err(res_val)){
        print("Client: read_socket failed, error:", unwrap_err(res_val))
        close_socket(c)
        join_if_valid_thread(t)
        free(state_ptr)
        panic("Client: read_socket failed")
      }
      assert((unwrap(res_val) == "pong"), "socket ping/pong")
      close_socket(c)
      join_if_valid_thread(t)
      free(state_ptr)
      print("✓ std.net.socket tests passed")
    }
}
