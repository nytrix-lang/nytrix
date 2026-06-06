;; Keywords: net network socket tcp udp http https server client requests curl remote tube process ssh tls transcript context os
;; Networking facade for HTTP requests, servers, sockets, tubes, remotes, and shared transport context.
;; References:
;; - std.os
module std.os.net(
   server,
   htons, ipv4_parse, ipv4_format, gethostbyname, socket_connect, socket_bind, socket_accept, socket_accept_info,
   socket_set_timeout_ms, socket_set_recv_timeout_ms, socket_set_send_timeout_ms,
   read_socket, write_socket, read_socket_exact, write_socket_part,
   write_socket_all, write_socket_line, read_socket_until,
   socket_connect_async, socket_accept_async, read_socket_async,
   write_socket_part_async, write_socket_all_async, read_socket_until_async,
   close_socket,
   remote, connect, remote_retry, connect_retry, process, proc, shell, ssh,
   ssh_process, ssh_shell, tube, tube_fd, tube_process, tube_fixture,
   fixture, tube_replay, replay, transcript, transcript_text, tube_kind, fileno, pid, connected,
   context, set_context, set_default_level, default_level, set_level, set_log_level, log_level,
   set_verbose, verbose, set_chunk_size, set_timeout,
   buffered, unrecv, clean, send, sendline, send_after, sendafter,
   sendline_after, sendlineafter, recv, recvn, recv_line, recvline,
   recv_until, recvuntil, recv_all, recvall, expect, expect_map, shutdown_send, close,
   interactive,
   http_request_raw, http_request, http_request_url, http_request_url_parsed,
   http_get, http_post, http_put, http_delete,
   http_get_parsed, http_post_parsed, http_put_parsed, http_delete_parsed,
   http_get_url, http_get_url_parsed, http_parse_url, http_parse_url_ex,
   http_parse_query, http_parse_response,
   request, request_raw, prepare_request, requests_prepare,
   requests_get, requests_get_host, requests_get_parsed, requests_get_host_parsed,
   requests_post, requests_put, requests_delete, requests_post_raw,
   requests_put_raw, requests_delete_raw, requests_request, requests_request_raw,
   requests, requests_parse_url, requests_parse_url_ex, requests_parse_query,
   requests_parse_response,
   curl_available, curl_request, curl_request_raw, curl_get, curl_fetch
)

use std.os.net.socket as sock
use std.os.net.context as netctx
use std.os.net.remote as rem
use std.os.net.http as http
use std.os.net.requests as req
use std.os.net.curl as curl
use std.core
use std.os.net.server as server

fn htons(int x) { return sock.htons(x) }

fn ipv4_parse(str s) { return sock.ipv4_parse(s) }

fn ipv4_format(int ip) { return sock.ipv4_format(ip) }

fn gethostbyname(str name) { return sock.gethostbyname(name) }

fn socket_connect(str host, int port) { return sock.socket_connect(host, port) }

fn socket_connect_async(str host, int port) { return sock.socket_connect_async(host, port) }

fn socket_bind(str host, int port) { return sock.socket_bind(host, port) }

fn socket_accept(int fd) { return sock.socket_accept(fd) }

fn socket_accept_info(int fd) { return sock.socket_accept_info(fd) }

fn socket_accept_async(int fd) { return sock.socket_accept_async(fd) }

fn socket_set_timeout_ms(int fd, int timeout_ms) { return sock.socket_set_timeout_ms(fd, timeout_ms) }

fn socket_set_recv_timeout_ms(int fd, int timeout_ms) { return sock.socket_set_recv_timeout_ms(fd, timeout_ms) }

fn socket_set_send_timeout_ms(int fd, int timeout_ms) { return sock.socket_set_send_timeout_ms(fd, timeout_ms) }

fn read_socket(int fd, int max_len) { return sock.read_socket(fd, max_len) }

fn read_socket_async(int fd, int max_len) { return sock.read_socket_async(fd, max_len) }

fn write_socket(int fd, any data) { return sock.write_socket(fd, data) }

fn read_socket_exact(int fd, int want_len) { return sock.read_socket_exact(fd, want_len) }

fn write_socket_part(int fd, any data, int off, int size=-1) { return sock.write_socket_part(fd, data, off, size) }

fn write_socket_part_async(int fd, any data, int off, int size=-1) { return sock.write_socket_part_async(fd, data, off, size) }

fn write_socket_all(int fd, any data) { return sock.write_socket_all(fd, data) }

fn write_socket_all_async(int fd, any data) { return sock.write_socket_all_async(fd, data) }

fn write_socket_line(int fd, any data) { return sock.write_socket_line(fd, data) }

fn read_socket_until(int fd, any needle, int max_bytes=65536) { return sock.read_socket_until(fd, needle, max_bytes) }

fn read_socket_until_async(int fd, any needle, int max_bytes=65536) { return sock.read_socket_until_async(fd, needle, max_bytes) }

fn close_socket(int fd) { return sock.close_socket(fd) }

fn remote(str host, int port, str level="", int timeout_ms=-1, int chunk_size=0) { return rem.remote(host, port, level, timeout_ms, chunk_size) }

fn connect(str host, int port, str level="", int timeout_ms=-1, int chunk_size=0) { return rem.connect(host, port, level, timeout_ms, chunk_size) }

fn remote_retry(str host, int port, int retries=20, int delay_ms=50, str level="", int timeout_ms=-1, int chunk_size=0) { return rem.remote_retry(host, port, retries, delay_ms, level, timeout_ms, chunk_size) }

fn connect_retry(str host, int port, int retries=20, int delay_ms=50, str level="", int timeout_ms=-1, int chunk_size=0) { return rem.connect_retry(host, port, retries, delay_ms, level, timeout_ms, chunk_size) }

fn process(str path, list args=[], str level="", int timeout_ms=-1, int chunk_size=0) { return rem.process(path, args, level, timeout_ms, chunk_size) }

fn proc(str path, list args=[], str level="", int timeout_ms=-1, int chunk_size=0) { return rem.proc(path, args, level, timeout_ms, chunk_size) }

fn shell(str command, str level="", int timeout_ms=-1, int chunk_size=0) { return rem.shell(command, level, timeout_ms, chunk_size) }

fn ssh(str host, str user="", int port=22, any command="", list options=[], str level="", int timeout_ms=-1, int chunk_size=0) { return rem.ssh(host, user, port, command, options, level, timeout_ms, chunk_size) }

fn ssh_process(str host, list command=[], str user="", int port=22, list options=[], str level="", int timeout_ms=-1, int chunk_size=0) { return rem.ssh_process(host, command, user, port, options, level, timeout_ms, chunk_size) }

fn ssh_shell(str host, str user="", int port=22, list options=[], str level="", int timeout_ms=-1, int chunk_size=0) { return rem.ssh_shell(host, user, port, options, level, timeout_ms, chunk_size) }

fn tube(int fd) { return rem.tube(fd) }

fn tube_fd(int fd, str host="", int port=0, str level="", int timeout_ms=-1, int chunk_size=0) { return rem.tube_fd(fd, host, port, level, timeout_ms, chunk_size) }

fn tube_process(any p, str name="process", str level="", int timeout_ms=-1, int chunk_size=0) { return rem.tube_process(p, name, level, timeout_ms, chunk_size) }

fn tube_fixture(any data="", str level="", int chunk_size=0) { return rem.tube_fixture(data, level, chunk_size) }

fn fixture(any data="", str level="", int chunk_size=0) { return rem.fixture(data, level, chunk_size) }

fn tube_replay(list rows, str level="", int chunk_size=0) { return rem.tube_replay(rows, level, chunk_size) }

fn replay(list rows, str level="", int chunk_size=0) { return rem.replay(rows, level, chunk_size) }

fn transcript(any io) { return rem.transcript(io) }

fn transcript_text(any io) { return rem.transcript_text(io) }

fn tube_kind(any io) { return rem.tube_kind(io) }

fn fileno(any io) { return rem.fileno(io) }

fn pid(any io) { return rem.pid(io) }

fn connected(any io) { return rem.connected(io) }

fn context(any log_level="") { return netctx.context(log_level) }

fn set_context(any options=0) { return netctx.set_context(options) }

fn set_default_level(any level="debug") { return netctx.set_default_level(level) }

fn default_level() { return netctx.default_level() }

fn set_level(any io, str level="debug") { return rem.set_level(io, level) }

fn set_log_level(any io, str level="debug") { return rem.set_log_level(io, level) }

fn log_level(any io) { return rem.log_level(io) }

fn set_verbose(any io, bool on=true) { return rem.set_verbose(io, on) }

fn verbose(any io, bool on=true) { return rem.verbose(io, on) }

fn set_chunk_size(any io, int n) { return rem.set_chunk_size(io, n) }

fn set_timeout(any io, int timeout_ms) { return rem.set_timeout(io, timeout_ms) }

fn buffered(any io) { return rem.buffered(io) }

fn unrecv(any io, any data) { return rem.unrecv(io, data) }

fn clean(any io) { return rem.clean(io) }

fn send(any io, any data) { return rem.send(io, data) }

fn sendline(any io, any data="") { return rem.sendline(io, data) }

fn send_after(any io, any needle, any data, int max_bytes=65536) { return rem.send_after(io, needle, data, max_bytes) }

fn sendafter(any io, any needle, any data, int max_bytes=65536) { return rem.sendafter(io, needle, data, max_bytes) }

fn sendline_after(any io, any needle, any data, int max_bytes=65536) { return rem.sendline_after(io, needle, data, max_bytes) }

fn sendlineafter(any io, any needle, any data, int max_bytes=65536) { return rem.sendlineafter(io, needle, data, max_bytes) }

fn recv(any io, int n=4096) { return rem.recv(io, n) }

fn recvn(any io, int n) { return rem.recvn(io, n) }

fn recv_line(any io, bool keepends=true, int max_bytes=65536) { return rem.recv_line(io, keepends, max_bytes) }

fn recvline(any io, bool keepends=true, int max_bytes=65536) { return rem.recvline(io, keepends, max_bytes) }

fn recv_until(any io, any needle, bool drop=false, int max_bytes=65536) { return rem.recv_until(io, needle, drop, max_bytes) }

fn recvuntil(any io, any needle, bool drop=false, int max_bytes=65536) { return rem.recvuntil(io, needle, drop, max_bytes) }

fn recv_all(any io, int max_bytes=65536) { return rem.recv_all(io, max_bytes) }

fn recvall(any io, int max_bytes=65536) { return rem.recvall(io, max_bytes) }

fn expect(any io, any needles, int max_bytes=65536) { return rem.expect(io, needles, max_bytes) }

fn expect_map(any io, any mapping, int max_bytes=65536) { return rem.expect_map(io, mapping, max_bytes) }

fn shutdown_send(any io) { return rem.shutdown_send(io) }

fn close(any io) { return rem.close(io) }

fn interactive(any io, int max_read=4096) { return rem.interactive(io, max_read) }

fn http_request_raw(str method, str host, int port, str path, any data=0, any headers=0) { return http.http_request_raw(method, host, port, path, data, headers) }

fn http_request(str method, str host, int port, str path, any data=0, any headers=0) { return http.http_request(method, host, port, path, data, headers) }

fn http_request_url(str method, str url, any data=0, any headers=0) { return http.http_request_url(method, url, data, headers) }

fn http_request_url_parsed(str method, str url, any data=0, any headers=0) { return http.http_request_url_parsed(method, url, data, headers) }

fn http_get(str host, int port, str path="/", any headers=0) { return http.http_get(host, port, path, headers) }

fn http_post(str host, int port, str path, any body="", any headers=0) { return http.http_post(host, port, path, body, headers) }

fn http_put(str host, int port, str path, any body="", any headers=0) { return http.http_put(host, port, path, body, headers) }

fn http_delete(str host, int port, str path, any headers=0) { return http.http_delete(host, port, path, headers) }

fn http_get_parsed(str host, int port, str path="/", any headers=0) { return http.http_get_parsed(host, port, path, headers) }

fn http_post_parsed(str host, int port, str path, any body="", any headers=0) { return http.http_post_parsed(host, port, path, body, headers) }

fn http_put_parsed(str host, int port, str path, any body="", any headers=0) { return http.http_put_parsed(host, port, path, body, headers) }

fn http_delete_parsed(str host, int port, str path, any headers=0) { return http.http_delete_parsed(host, port, path, headers) }

fn http_get_url(str url, any headers=0) { return http.http_get_url(url, headers) }

fn http_get_url_parsed(str url, any headers=0) { return http.http_get_url_parsed(url, headers) }

fn http_parse_url(str url) { return http.http_parse_url(url) }

fn http_parse_url_ex(str url) { return http.http_parse_url_ex(url) }

fn http_parse_query(str q) { return http.http_parse_query(q) }

fn http_parse_response(str raw) { return http.http_parse_response(raw) }

fn requests_get(str url) { return req.requests_get(url) }

fn requests_get_host(str host, int port, str path="/") { return req.requests_get_host(host, port, path) }

fn request(any method, any url=0, any data=0, any headers=0, any options=0) { return req.request(method, url, data, headers, options) }

fn request_raw(any method, any url=0, any data=0, any headers=0, any options=0) { return req.request_raw(method, url, data, headers, options) }

fn prepare_request(any method, any url=0, any data=0, any headers=0, any options=0) { return req.prepare_request(method, url, data, headers, options) }

fn requests_prepare(any method, any url=0, any data=0, any headers=0, any options=0) { return req.requests_prepare(method, url, data, headers, options) }

fn requests_get_parsed(str url, any headers=0, any options=0) { return req.requests_get_parsed(url, headers, options) }

fn requests_get_host_parsed(str host, int port, str path="/", any headers=0) { return req.requests_get_host_parsed(host, port, path, headers) }

fn requests_post(str url, any data="", any headers=0, any options=0) { return req.requests_post(url, data, headers, options) }

fn requests_put(str url, any data="", any headers=0, any options=0) { return req.requests_put(url, data, headers, options) }

fn requests_delete(str url, any headers=0, any options=0) { return req.requests_delete(url, headers, options) }

fn requests_post_raw(str url, any data="", any headers=0, any options=0) { return req.requests_post_raw(url, data, headers, options) }

fn requests_put_raw(str url, any data="", any headers=0, any options=0) { return req.requests_put_raw(url, data, headers, options) }

fn requests_delete_raw(str url, any headers=0, any options=0) { return req.requests_delete_raw(url, headers, options) }

fn requests_request(any method, any url=0, any data=0, any headers=0, any options=0) { return req.requests_request(method, url, data, headers, options) }

fn requests_request_raw(any method, any url=0, any data=0, any headers=0, any options=0) { return req.requests_request_raw(method, url, data, headers, options) }

fn requests(any url, any method="GET", any data=0, any headers=0, bool parsed=true, any options=0) { return req.requests(url, method, data, headers, parsed, options) }

fn requests_parse_url(str url) { return req.requests_parse_url(url) }

fn requests_parse_url_ex(str url) { return req.requests_parse_url_ex(url) }

fn requests_parse_query(str q) { return req.requests_parse_query(q) }

fn requests_parse_response(str raw) { return req.requests_parse_response(raw) }

fn curl_available() bool { return curl.curl_available() }

fn curl_request(str method, str url, any data=0, any headers=0, int timeout_sec=60, str user_agent="nytrix/1.0", any options=0) any {
   "Runs the curl request operation."
   return curl.curl_request(method, url, data, headers, timeout_sec, user_agent, options)
}

fn curl_request_raw(str method, str url, any data=0, any headers=0, int timeout_sec=60, str user_agent="nytrix/1.0", any options=0) str {
   "Runs the curl request raw operation."
   return curl.curl_request_raw(method, url, data, headers, timeout_sec, user_agent, options)
}

fn curl_get(str url, any headers=0, int timeout_sec=60, str user_agent="nytrix/1.0", any options=0) any {
   "Runs the curl get operation."
   return curl.curl_get(url, headers, timeout_sec, user_agent, options)
}

fn curl_fetch(str url, int timeout_sec=60, str user_agent="nytrix/1.0") any {
   "Runs the curl fetch operation."
   return curl.curl_fetch(url, timeout_sec, user_agent)
}

#main {
   assert(htons(80) == 20480 && ipv4_parse("127.0.0.1") == gethostbyname("localhost"), "net socket wrappers")
   def url = http_parse_url("http://example.com:8080/a?b=c")
   def req_url = requests_parse_url("https://example.com/x")
   assert(url.get(0) == "example.com" && url.get(1) == 8080 && url.get(2) == "/a?b=c", "net http facade")
   assert(req_url.get(0) == "example.com" && req_url.get(1) == 443 && req_url.get(2) == "/x", "net requests facade")
   def query = http_parse_query("a=1&b=two")
   assert(query.get("a") == "1" && query.get("b") == "two" && is_bool(curl_available()), "net parse/curl facade")
   print("✓ std.os.net self-test passed")
}
