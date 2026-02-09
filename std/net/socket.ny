;; Keywords: net socket
;; Net Socket module.

module std.net.socket (
   htons, ipv4_parse, gethostbyname, socket_connect, socket_bind, socket_accept,
   read_socket, write_socket, close_socket
)
use std.core *
use std.str *
use std.core.reflect *
use std.os.ffi *
use std.core *
use std.os.sys *

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
         p = p + 1
      } else {
         if(c < 48 || c > 57){ return 0 }
         oct = oct * 10 + (c - 48)
         p = p + 1
      }
   }
   return val
}

fn gethostbyname(name){
   "Resolves a hostname to an IPv4 address integer. No libc used."
   if (name == "localhost") { return ipv4_parse("127.0.0.1") }
   if (name == "127.0.0.1") { return ipv4_parse("127.0.0.1") }

   ; Try /etc/hosts first
   def hosts = file_read("/etc/hosts")
   if (len(hosts) > 0) {
      def lines = split(hosts, "\n")
      mut i = 0
      while (i < len(lines)) {
         def line = strip(get(lines, i))
         if (len(line) > 0 && load8(line, 0) != 35) { ; '#'
            def parts = split(line, " ")
            if (len(parts) >= 2) {
               def ip_str = strip(get(parts, 0))
               mut j = 1
               while (j < len(parts)) {
                  mut host_alias = strip(get(parts, j))
                  if (len(host_alias) > 0 && host_alias == name) {
                     return ipv4_parse(ip_str)
                  }
                  j += 1
               }
            }
         }
         i += 1
      }
   }

   ; Fallback to DNS Query (Google Public DNS)
   return dns_query(name, "8.8.8.8")
}

fn dns_query(name, server) {
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
   while (l_idx < len(labels)) {
      def l = get(labels, l_idx)
      bytes_set(buf, pos, len(l)) pos += 1
      mut ch_idx = 0
      while (ch_idx < len(l)) {
         bytes_set(buf, pos, load8(l, ch_idx)) pos += 1
         ch_idx += 1
      }
      l_idx += 1
   }
   bytes_set(buf, pos, 0) pos += 1
   ; TYPE A (1), CLASS IN (1)
   bytes_set(buf, pos, 0) bytes_set(buf, pos + 1, 1) pos += 2
   bytes_set(buf, pos, 0) bytes_set(buf, pos + 1, 1) pos += 2

   def fd = syscall(41, 2, 2, 0, 0,0,0) ; AF_INET, SOCK_DGRAM
   if (fd < 0) { return 0 }

   def sa = malloc(16)
   store16(sa, 2, 0) ; AF_INET
   store16(sa, htons(53), 2)
   store32(sa, ipv4_parse(server), 4)

   syscall(44, fd, buf + 8, pos, 0, sa, 16) ; sendto

   def rb = bytes(512)
   mut n = syscall(45, fd, rb + 8, 512, 0, 0, 0) ; recvfrom
   syscall(3, fd, 0,0,0,0,0)

   if (n < 12) { return 0 }

   ; Simple Answer Parser
   mut rpos = 12
   while (bytes_get(rb, rpos) != 0) { rpos += bytes_get(rb, rpos) + 1 }
   rpos += 5 ; skip null, type(2), class(2)

   if (rpos + 12 > n) { return 0 }

   mut a_count = (bytes_get(rb, 6) << 8) | bytes_get(rb, 7)
   if (a_count == 0) { return 0 }

   ; Handle potential Pointer Compression in Name (0xC0XX)
   if ((bytes_get(rb, rpos) & 192) == 192) { rpos += 2 }
   else { while (bytes_get(rb, rpos) != 0) { rpos += bytes_get(rb, rpos) + 1 } rpos += 1 }

   mut a_type = (bytes_get(rb, rpos) << 8) | bytes_get(rb, rpos + 1)
   mut a_rdlen = (bytes_get(rb, rpos + 8) << 8) | bytes_get(rb, rpos + 9)
   rpos += 10

   if (a_type == 1 && a_rdlen == 4) {
      ; Return IP in 32-bit integer (little-endian as expected by socket_connect)
      return (bytes_get(rb, rpos) << 0) | (bytes_get(rb, rpos + 1) << 8) | (bytes_get(rb, rpos + 2) << 16) | (bytes_get(rb, rpos + 3) << 24)
   }
   return 0
}

fn socket_connect(host, port){
   "Connect to host:port (TCP). Returns fd or -1."
   mut ip = ipv4_parse(host)
   if(ip == 0){
      ip = gethostbyname(host)
   }
   if(ip == 0){ return -1 }
   def fd = syscall(41, 2, 1, 0, 0,0,0) ; "AF_INET, SOCK_STREAM"
   if(fd < 0){ return -1 }
   def sa = malloc(16)
   store16(sa, 2, 0)
   store16(sa, htons(port), 2)
   store32(sa, ip, 4)
   store64(sa, 0, 8)
   if(syscall(42, fd, sa, 16, 0,0,0) < 0){
      syscall(3, fd, 0,0,0,0,0)
      return -1
   }
   return fd
}

fn socket_bind(host, port){
   "Create a TCP socket, binds it to the specified host and port, and starts listening. Returns the file descriptor or -1 on error."
   mut ip = ipv4_parse(host)
   if(ip == 0){
      ip = gethostbyname(host)
   }
   def fd = syscall(41, 2, 1, 0, 0,0,0) "AF_INET, SOCK_STREAM"
   if(fd < 0){ return -1 }
   ; Allow port reuse
   def opt = malloc(4)
   store32(opt, 1)
   syscall(54, fd, 1, 2, opt, 4, 0) ; "SOL_SOCKET, SO_REUSEADDR"
   def sa = malloc(16)
   store16(sa, 2, 0)
   store16(sa, htons(port), 2)
   store32(sa, ip, 4)
   store64(sa, 0, 8)
   if(syscall(49, fd, sa, 16, 0,0,0) < 0){ return -1 } ; "bind"
   if(syscall(50, fd, 128, 0, 0,0,0) < 0){ return -1 } ; "listen"
   return fd
}

fn socket_accept(server_fd){
   "Accepts an incoming connection on a listening socket. Returns the client file descriptor."
   return syscall(43, server_fd, 0, 0, 0,0,0) ; "accept"
}

fn read_socket(fd, max_len){
   "Reads up to `max_len` bytes from a socket. Returns the data as a string."
   def buf = malloc(max_len + 1)
   mut n = syscall(0, fd, buf, max_len, 0,0,0)
   if(n < 0){ n = 0 }
   ; Initialize Nytrix string header
   store64(buf, (n << 1) | 1, -16) ; Tagged length
   store64(buf, 120, -8)           ; Tag 120 (String)
   store8(buf, 0, n)               ; Null terminator
   return buf
}

fn write_socket(fd, data){
   "Writes data to a socket."
   return syscall(1, fd, data, str_len(data), 0,0,0)
}

fn close_socket(fd){
   "Closes a socket file descriptor."
   return syscall(3, fd, 0,0,0,0,0)
}
