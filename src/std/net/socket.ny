;;; socket.ny --- net socket module

;; Keywords: net socket

;;; Commentary:

;; Net Socket module.

use std.core
use std.strings.str
use std.core.reflect
use std.os.ffi
module std.net.socket (
	htons, ipv4_parse, gethostbyname, socket_connect, socket_bind, socket_accept,
	read_socket, write_socket, close_socket
)

fn htons(x){
	"Convert a 16-bit integer from host byte order to network byte order (big-endian)."
	def lo = x % 256
	def hi = (x / 256) % 256
	return lo*256 + hi
}

fn ipv4_parse(s){
	"Parses an IPv4 address string (e.g., '127.0.0.1') into a 32-bit integer (little-endian)."
	def p = 0  def val=0  def oct=0  def shift=0
	def done = 0
	while(done==0){
		def c = load8(s, p)
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
	"Resolves a hostname to an IPv4 address integer."
	def h = dlopen("libc.so.6", 2)
	if(h == 0){ return 0 }
	def f = dlsym(h, "gethostbyname")
	if(f == 0){ dlclose(h)
 return 0 }
	def res = call1(f, name)
	if(res == 0){ dlclose(h)
 return 0 }
 ; struct hostent: h_name, h_aliases, h_addrtype, h_length, h_addr_list
 ; h_addr_list is at offset 32 on x86_64
	def addr_list_ptr = load64(res, 32)
	if(addr_list_ptr == 0){ dlclose(h)
 return 0 }
	def first_addr_ptr = load64(addr_list_ptr)
	if(first_addr_ptr == 0){ dlclose(h)
 return 0 }
	def ip = load32(first_addr_ptr, 0)
	dlclose(h)
	return ip
}

fn socket_connect(host, port){
	"Connect to host:port (TCP). Returns fd or -1."
	def ip = ipv4_parse(host)
	if(ip == 0){
		ip = gethostbyname(host)
	}
	if(ip == 0){ return -1 }
	def fd = rt_syscall(41, 2, 1, 0, 0,0,0) ; "AF_INET, SOCK_STREAM"
	if(fd < 0){ return -1 }
	def sa = rt_malloc(16)
	store16(sa, 2, 0)
	store16(sa, htons(port), 2)
	store32(sa, ip, 4)
	store64(sa, 0, 8)
	if(rt_syscall(42, fd, sa, 16, 0,0,0) < 0){
		rt_syscall(3, fd, 0,0,0,0,0)
		return -1
	}
	return fd
}

fn socket_bind(host, port){
	"Create a TCP socket, binds it to the specified host and port, and starts listening. Returns the file descriptor or -1 on error."
	def ip = ipv4_parse(host)
	def fd = rt_syscall(41, 2, 1, 0, 0,0,0) "AF_INET, SOCK_STREAM"
	if(fd < 0){ return -1 }
 ; Allow port reuse
	def opt = rt_malloc(4)
	store32(opt, 1)
	rt_syscall(54, fd, 1, 2, opt, 4, 0) ; "SOL_SOCKET, SO_REUSEADDR"
	def sa = rt_malloc(16)
	store16(sa, 2, 0)
	store16(sa, htons(port), 2)
	store32(sa, ip, 4)
	store64(sa, 0, 8)
	if(rt_syscall(49, fd, sa, 16, 0,0,0) < 0){ return -1 } ; "bind"
	if(rt_syscall(50, fd, 128, 0, 0,0,0) < 0){ return -1 } ; "listen"
	return fd
}

fn socket_accept(server_fd){
	"Accepts an incoming connection on a listening socket. Returns the client file descriptor."
	return rt_syscall(43, server_fd, 0, 0, 0,0,0) ; "accept"
}

fn read_socket(fd, max_len){
	"Reads up to `max_len` bytes from a socket. Returns the data as a string."
	def buf = rt_malloc(max_len + 1)
	store64(buf - 8, 120)
	def n = rt_syscall(0, fd, buf, max_len, 0,0,0)
	if(n < 0){ n = 0 }
	store8(buf, 0, n)
	return buf
}

fn write_socket(fd, data){
	"Writes data to a socket."
	return rt_syscall(1, fd, data, str_len(data), 0,0,0)
}

fn close_socket(fd){
	"Closes a socket file descriptor."
	return rt_syscall(3, fd, 0,0,0,0,0)
}
