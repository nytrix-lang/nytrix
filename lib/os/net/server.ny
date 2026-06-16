;; Keywords: net socket http web server router os
;; Blocking HTTP/1.1 server operations for route handlers and structured responses.
;; References:
;; - std.os.net
;; - std.os
module std.os.net.server(
   Server, listen, serve, serve_app, serve_cli, serve_config, serve_once, serve_once_fd, handle_client,
   read_request, parse_request, response, text, html, json_response, json,
   redirect, bad_request, not_found, method_not_allowed, route, router,
   send_response, status_text, mime_type, header, server_url, serve_banner
)

use std.core
use std.core.dict_mod as _d
use std.core.str
use std.os.args as cli
use std.os.net.context as netctx
use std.os.net.socket as sock
use std.os.net.http as http
use std.math.parse.data.json as jsonlib

fn status_text(int status) str {
   "Returns a standard HTTP reason phrase for common status codes."
   if status == 100 { return "Continue" }
   if status == 101 { return "Switching Protocols" }
   if status == 200 { return "OK" }
   if status == 201 { return "Created" }
   if status == 202 { return "Accepted" }
   if status == 204 { return "No Content" }
   if status == 301 { return "Moved Permanently" }
   if status == 302 { return "Found" }
   if status == 303 { return "See Other" }
   if status == 304 { return "Not Modified" }
   if status == 307 { return "Temporary Redirect" }
   if status == 308 { return "Permanent Redirect" }
   if status == 400 { return "Bad Request" }
   if status == 401 { return "Unauthorized" }
   if status == 403 { return "Forbidden" }
   if status == 404 { return "Not Found" }
   if status == 405 { return "Method Not Allowed" }
   if status == 409 { return "Conflict" }
   if status == 415 { return "Unsupported Media Type" }
   if status == 429 { return "Too Many Requests" }
   if status == 500 { return "Internal Server Error" }
   if status == 501 { return "Not Implemented" }
   if status == 503 { return "Service Unavailable" }
   "Status"
}

fn mime_type(any path) str {
   "Returns a small built-in MIME type guess for a path."
   if !is_str(path) { return "application/octet-stream" }
   def p = lower(path)
   if endswith(p, ".html") || endswith(p, ".htm") { return "text/html; charset=utf-8" }
   if endswith(p, ".css") { return "text/css; charset=utf-8" }
   if endswith(p, ".js") || endswith(p, ".mjs") { return "text/javascript; charset=utf-8" }
   if endswith(p, ".json") { return "application/json; charset=utf-8" }
   if endswith(p, ".txt") || endswith(p, ".log") { return "text/plain; charset=utf-8" }
   if endswith(p, ".svg") { return "image/svg+xml" }
   if endswith(p, ".png") { return "image/png" }
   if endswith(p, ".jpg") || endswith(p, ".jpeg") { return "image/jpeg" }
   if endswith(p, ".gif") { return "image/gif" }
   if endswith(p, ".webp") { return "image/webp" }
   if endswith(p, ".wasm") { return "application/wasm" }
   "application/octet-stream"
}

fn _has_header(any headers, str name) bool {
   if !is_dict(headers) { return false }
   def want = lower(name)
   def items = _d.dict_items(headers)
   mut i = 0
   while i < items.len {
      def k = items[i].get(0)
      if is_str(k) && lower(k) == want { return true }
      i += 1
   }
   false
}

fn header(any headers, str name, any fallback="") any {
   "Case-insensitive request/response header lookup."
   if !is_dict(headers) { return fallback }
   def want = lower(name)
   def direct = headers.get(want, nil)
   if direct != nil { return direct }
   def items = _d.dict_items(headers)
   mut i = 0
   while i < items.len {
      def pair = items[i]
      def k = pair.get(0)
      if is_str(k) && lower(k) == want { return pair.get(1) }
      i += 1
   }
   fallback
}

fn _opt_bool(any options, str key, bool fallback) bool {
   if !is_dict(options) { return fallback }
   def v = options.get(key, fallback)
   if is_int(v) { return v != 0 }
   if is_str(v) {
      def s = lower(strip(v))
      return !(s == "" || s == "0" || s == "false" || s == "off" || s == "no")
   }
   v ? true : false
}

fn _opt_int(any options, str key, int fallback) int {
   if !is_dict(options) { return fallback }
   def v = options.get(key, fallback)
   if is_int(v) { return v }
   atoi(to_str(v))
}

fn _opt_str(any options, str key, str fallback="") str {
   if !is_dict(options) { return fallback }
   def v = options.get(key, fallback)
   is_str(v) ? v : to_str(v)
}

fn _debug_enabled(any options=0) bool {
   _opt_bool(options, "debug", false) || cli.flag("--debug") || cli.flag("--verbose") || cli.flag("-v")
}

fn _clip(any value, int limit) str {
   def s = is_str(value) ? value : to_str(value)
   if limit <= 0 || s.len <= limit { return s }
   if limit <= 3 { return slice(s, 0, limit) }
   slice(s, 0, limit - 3) + "..."
}

fn _peer_label(any peer) str {
   if is_dict(peer) {
      def addr = peer.get("addr", "")
      if is_str(addr) && addr.len > 0 { return addr }
      def host = peer.get("host", "")
      def port = peer.get("port", 0)
      if is_str(host) && host.len > 0 {
         return port > 0 ? host + ":" + to_str(port) : host
      }
      return ""
   }
   is_str(peer) ? peer : ""
}

fn server_url(str host, int port) str {
   "Returns the primary URL shown for a server bind address."
   mut show = host
   if show == "" || show == "*" || show == "0.0.0.0" { show = "127.0.0.1" }
   "http://" + show + ":" + to_str(port) + "/"
}

fn serve_banner(str host, int port, any options=0) int {
   "Prints a formatted local-server banner."
   if !_opt_bool(options, "banner", true) { return 0 }
   def name = _opt_str(options, "name", "Ny HTTP")
   def url = server_url(host, port)
   print(netctx.paint("Serving HTTP", "green", 1, options) + " " + netctx.paint(name, "cyan", 1, options))
   print("  " + netctx.paint("url", "gray", 0, options) + "   " + netctx.paint(url, "white", 1, options))
   print("  " + netctx.paint("bind", "gray", 0, options) + "  " + host + ":" + to_str(port))
   print("  " + netctx.paint("stop", "gray", 0, options) + "  Ctrl-C")
   if _debug_enabled(options) { print("  " + netctx.paint("debug", "gray", 0, options) + " on") }
   0
}

fn _log_request(dict r, any options=0) int {
   if !_opt_bool(options, "log", true) { return 0 }
   def req = r.get("request", 0)
   def method = is_dict(req) ? req.get("method", "?") : "?"
   def path = is_dict(req) ? req.get("path", "/") : "/"
   def peer = is_dict(req) ? _peer_label(req.get("peer", r.get("peer", ""))) : _peer_label(r.get("peer", ""))
   def status = r.get("status", 0)
   def sc = status >= 500 ? "red" : (status >= 400 ? "yellow" : "green")
   def reason = status_text(status)
   def peer_part = peer.len > 0 ? netctx.paint(peer, "magenta", 0, options) + " " + netctx.paint("->", "gray", 0, options) + " " : ""
   print(netctx.paint(method, "cyan", 1, options) + " " + peer_part + path + " " + netctx.paint(to_str(status), sc, 1, options) + " " + reason + " " + to_str(r.get("bytes", 0)) + "B")
   if _debug_enabled(options) && is_dict(req) {
      def headers = req.get("headers", 0)
      mut line = "  " + netctx.paint("debug", "gray", 0, options) + " " + req.get("version", "HTTP/1.1")
      def host = header(headers, "host", "")
      if host.len > 0 { line = line + " host=" + netctx.paint(_clip(host, 80), "white", 0, options) }
      def query = req.get("query", "")
      if query.len > 0 { line = line + " query=" + netctx.paint(_clip(query, 120), "white", 0, options) }
      def ua = header(headers, "user-agent", "")
      if ua.len > 0 { line = line + " ua=" + netctx.paint(_clip(ua, 96), "white", 0, options) }
      print(line)
   }
   0
}

fn response(any body="", int status=200, any headers=0) dict {
   "Builds a response dictionary accepted by `send_response`."
   mut h = is_dict(headers) ? headers : _d.dict(8)
   if !_has_header(h, "content-type") { h["content-type"] = "text/plain; charset=utf-8" }
   return {"status": status, "reason": status_text(status), "headers": h, "body": is_str(body) ? body : to_str(body)}
}

fn text(any body="", int status=200, any headers=0) dict {
   "Builds a plain-text HTTP response."
   mut h = is_dict(headers) ? headers : _d.dict(4)
   h["content-type"] = "text/plain; charset=utf-8"
   response(body, status, h)
}

fn html(any body="", int status=200, any headers=0) dict {
   "Builds an HTML HTTP response."
   mut h = is_dict(headers) ? headers : _d.dict(4)
   h["content-type"] = "text/html; charset=utf-8"
   response(body, status, h)
}

fn json_response(any value, int status=200, any headers=0) dict {
   "Builds a JSON HTTP response."
   mut h = is_dict(headers) ? headers : _d.dict(4)
   h["content-type"] = "application/json; charset=utf-8"
   response(jsonlib.json_encode(value), status, h)
}

fn json(any value, int status=200, any headers=0) dict { json_response(value, status, headers) }

fn redirect(str location, int status=302) dict {
   "Builds a redirect response."
   response("", status, {"location": location, "content-type": "text/plain; charset=utf-8"})
}

fn bad_request(any body="Bad Request\n") dict { text(body, 400) }

fn not_found(any body="Not Found\n") dict { text(body, 404) }

fn method_not_allowed(any allow="GET, HEAD") dict { text("Method Not Allowed\n", 405, {"allow": allow}) }

fn _headers_wire(any headers) str {
   mut b = Builder(256)
   if is_dict(headers) {
      def items = _d.dict_items(headers)
      mut i = 0
      while i < items.len {
         def pair = items[i]
         def k = to_str(pair.get(0))
         def v = to_str(pair.get(1))
         if k.len > 0 && find(k, "\n") < 0 && find(k, "\r") < 0 && find(v, "\n") < 0 && find(v, "\r") < 0 {
            b = builder_append(b, k + ": " + v + "\r\n")
         }
         i += 1
      }
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _coerce_response(any res) dict {
   if is_dict(res) && res.get("status", 0) > 0 { return res }
   response(res, 200)
}

fn send_response(int fd, any res, str method="GET") int {
   "Sends a response dictionary or string to a socket."
   def r = _coerce_response(res)
   def body = r.get("body", "")
   def status = r.get("status", 200)
   mut h = r.get("headers", _d.dict(8))
   if !_has_header(h, "content-length") { h["content-length"] = to_str(body.len) }
   if !_has_header(h, "connection") { h["connection"] = "close" }
   def head = "HTTP/1.1 " + to_str(status) + " " + r.get("reason", status_text(status)) + "\r\n" + _headers_wire(h) + "\r\n"
   def wrote = sock.write_socket_all(fd, head)
   if upper(method) == "HEAD" { return wrote }
   if body.len == 0 { return wrote }
   def bw = sock.write_socket_all(fd, body)
   if wrote < 0 || bw < 0 { return -1 }
   wrote + bw
}

fn _split_head_body(str raw) list {
   mut idx = find(raw, "\r\n\r\n")
   mut sep = 4
   if idx < 0 {
      idx = find(raw, "\n\n")
      sep = 2
   }
   if idx < 0 { return [raw, ""] }
   [slice(raw, 0, idx), slice(raw, idx + sep, raw.len)]
}

fn parse_request(any raw, any peer="") dict {
   "Parses a raw HTTP request into a request dictionary."
   mut req = {"ok": false, "method": "", "target": "", "path": "", "query": "", "query_params": _d.dict(4), "version": "", "headers": _d.dict(16), "body": "", "raw": raw, "peer": peer}
   if !is_str(raw) || raw.len == 0 { return req }
   def hb = _split_head_body(raw)
   def head = hb[0]
   req["body"] = hb[1]
   def lines = split(head, "\n")
   if lines.len == 0 { return req }
   def first = strip(lines[0])
   def parts = split_words(first)
   if parts.len < 2 { return req }
   req["method"] = upper(parts[0])
   req["target"] = parts[1]
   req["version"] = (parts.len >= 3) ? parts[2] : "HTTP/1.0"
   mut target = parts[1]
   if startswith(lower(target), "http://") || startswith(lower(target), "https://") {
      def u = http.http_parse_url_ex(target)
      target = u.get("target", "/")
   }
   def qpos = find(target, "?")
   if qpos >= 0 {
      req["path"] = slice(target, 0, qpos)
      req["query"] = slice(target, qpos + 1, target.len)
   } else {
      req["path"] = target
      req["query"] = ""
   }
   req["query_params"] = http.http_parse_query(req["query"])
   mut h = _d.dict(16)
   mut i = 1
   while i < lines.len {
      def line = strip(lines[i])
      if line.len > 0 {
         def c = find(line, ":")
         if c > 0 { h[lower(strip(slice(line, 0, c)))] = strip(slice(line, c + 1, line.len)) }
      }
      i += 1
   }
   req["headers"] = h
   req["ok"] = req["method"].len > 0 && req["path"].len > 0
   req
}

fn _read_header_buffer(int fd, int max_header) str {
   mut raw = ""
   while raw.len < max_header {
      def left = max_header - raw.len
      def want = left < 4096 ? left : 4096
      def chunk = sock.read_socket(fd, want)
      if !is_str(chunk) || chunk.len == 0 { break }
      raw = raw + chunk
      if endswith(raw, "\r\n\r\n") || endswith(raw, "\n\n") { break }
      if find(raw, "\r\n\r\n") >= 0 || find(raw, "\n\n") >= 0 { break }
   }
   raw
}

fn read_request(int fd, int max_header=65536, int max_body=10485760, any peer=0) dict {
   "Reads and parses one HTTP request from a socket."
   if max_header < 1024 { max_header = 1024 }
   if max_body < 0 { max_body = 0 }
   def raw0 = _read_header_buffer(fd, max_header)
   if raw0.len == 0 { return parse_request("", peer) }
   def req0 = parse_request(raw0, peer)
   def cl = int(header(req0.get("headers", 0), "content-length", "0"))
   if cl > 0 {
      if cl > max_body { return {"ok": false, "error": "request body too large", "status": 413, "raw": raw0, "peer": peer} }
      mut body = req0.get("body", "")
      if body.len < cl { body = body + sock.read_socket_exact(fd, cl - body.len) }
      if body.len > cl { body = slice(body, 0, cl) }
      req0["body"] = body
      req0["raw"] = raw0
   }
   req0
}

fn handle_client(int fd, fnptr handler, any peer=0) dict {
   "Handles one accepted client by reading a request, calling `handler(req)`, and sending the response."
   def req = read_request(fd, 65536, 10485760, peer)
   if !req.get("ok", false) {
      send_response(fd, bad_request(), "GET")
      return {"ok": false, "request": req, "status": 400, "peer": peer}
   }
   def res = handler(req)
   def status = _coerce_response(res).get("status", 200)
   def wrote = send_response(fd, res, req.get("method", "GET"))
   return {"ok": wrote >= 0, "request": req, "status": status, "bytes": wrote, "peer": peer}
}

fn serve_once_fd(int server_fd, fnptr handler, int timeout_ms=0) dict {
   "Accepts and handles one client on an existing listening socket."
   if timeout_ms > 0 { sock.socket_set_timeout_ms(server_fd, timeout_ms) }
   def accepted = sock.socket_accept_info(server_fd)
   def c = accepted.get("fd", -1)
   if c < 0 { return {"ok": false, "error": "accept failed"} }
   defer { sock.close_socket(c) }
   handle_client(c, handler, accepted)
}

fn serve_once(str host, int port, fnptr handler, int timeout_ms=5000) dict {
   "Binds, accepts, and handles one HTTP request."
   def fd = sock.socket_bind(host, port)
   if fd < 0 { return {"ok": false, "error": "bind failed"} }
   defer { sock.close_socket(fd) }
   serve_once_fd(fd, handler, timeout_ms)
}

fn Server(str host="127.0.0.1", int port=8080, any handler=0, int max_requests=-1, int timeout_ms=0) dict {
   "Builds a server config for `serve_config`."
   return {"host": host, "port": port, "handler": handler, "max_requests": max_requests, "timeout_ms": timeout_ms}
}

fn serve_config(dict cfg) dict {
   "Serves HTTP using a config from `Server(...)`."
   serve(cfg.get("host", "127.0.0.1"), cfg.get("port", 8080), cfg.get("handler", 0), cfg.get("max_requests", -1), cfg.get("timeout_ms", 0))
}

fn _listen_impl(str host, int port, fnptr handler, int max_requests=-1, int timeout_ms=0, any options=0) dict {
   def fd = sock.socket_bind(host, port)
   if fd < 0 { return {"ok": false, "error": "bind failed", "served": 0} }
   defer { sock.close_socket(fd) }
   serve_banner(host, port, options)
   mut served = 0
   while true {
      if max_requests >= 0 && served >= max_requests { break }
      def r = serve_once_fd(fd, handler, timeout_ms)
      if !r.get("ok", false) {
         if max_requests >= 0 { return {"ok": false, "error": r.get("error", "client failed"), "served": served} }
         continue
      }
      _log_request(r, options)
      served += 1
   }
   return {"ok": true, "served": served}
}

fn listen(str host, int port, fnptr handler, int max_requests=-1, int timeout_ms=0) dict {
   "Starts a blocking HTTP server. Use `max_requests` for finite testable runs."
   _listen_impl(host, port, handler, max_requests, timeout_ms, {"banner": false, "log": false})
}

fn serve(str host, int port, fnptr handler, int max_requests=-1, int timeout_ms=0) dict {
   "Alias for `listen`."
   listen(host, port, handler, max_requests, timeout_ms)
}

fn serve_app(str host, int port, fnptr handler, int max_requests=-1, int timeout_ms=0, any options=0) dict {
   "Starts a formatted blocking HTTP server with banner and request logging."
   _listen_impl(host, port, handler, max_requests, timeout_ms, options)
}

fn _cli_port(any options) int {
   def explicit = cli.int_value("--port", 0)
   if explicit > 0 { return explicit }
   def ps = cli.positionals()
   if ps.len > 0 {
      def p = atoi(to_str(ps[0]))
      if p > 0 { return p }
   }
   _opt_int(options, "port", 8080)
}

fn serve_cli(fnptr handler, any options=0) dict {
   "Serves using CLI flags: `[port]`, `--port`, `--bind`/`--host`, `--once`, and `--requests`."
   mut host = cli.value("--bind", "")
   if host.len == 0 { host = cli.value("--host", _opt_str(options, "host", "127.0.0.1")) }
   def port = _cli_port(options)
   def fallback_requests = _opt_int(options, "max_requests", -1)
   def max_requests = cli.int_value("--requests", cli.flag("--once") ? 1 : fallback_requests)
   def timeout_ms = _opt_int(options, "timeout_ms", 0)
   serve_app(host, port, handler, max_requests, timeout_ms, options)
}

fn route(dict routes, dict req, any fallback=0) any {
   "Dispatches a request through a route map. Keys may be `/path` or `METHOD /path`."
   def method = upper(req.get("method", "GET"))
   def path = req.get("path", "/")
   def exact = method + " " + path
   mut h = routes.get(exact, nil)
   if h == nil { h = routes.get(path, nil) }
   if h != nil { return h(req) }
   if fallback != 0 { return fallback(req) }
   not_found()
}

fn router(dict routes, any fallback=0) fnptr {
   "Returns a handler function for a route map."
   return fn(req) { route(routes, req, fallback) }
}

#main {
   use std.os (ticks)
   use std.os.async (await)
   def parsed = parse_request("POST /submit?x=1 HTTP/1.1\r\nHost: local\r\nContent-Length: 3\r\n\r\nabc")
   assert_eq(parsed.get("ok", false), true, "request parses")
   assert_eq(parsed.get("method", ""), "POST", "request method")
   assert_eq(parsed.get("path", ""), "/submit", "request path")
   assert_eq(parsed.get("query_params", 0).get("x", ""), "1", "request query")
   assert_eq(header(parsed.get("headers", 0), "HOST", ""), "local", "case-insensitive header")
   def rr = route({
         "GET /ok": fn(r) { text("ok") },
         "/json": fn(r) { json({"ok": true}) }
   }, parse_request("GET /json HTTP/1.1\r\nHost: local\r\n\r\n"))
   assert_eq(rr.get("status", 0), 200, "route response")
   assert(str_contains(rr.get("body", ""), "\"ok\":true") || str_contains(rr.get("body", ""), "\"ok\": true"), "route json body")
   def port = 56000 + ((ticks() / 1000000) % 1000)
   def server = sock.socket_bind("127.0.0.1", port)
   if server >= 0 {
      def accept_h = sock.socket_accept_async(server)
      def client = sock.socket_connect("127.0.0.1", port)
      def peer = await accept_h
      if client >= 0 && peer >= 0 {
         def raw_req = "POST /echo?name=ny HTTP/1.1\r\nHost: local\r\nContent-Length: 5\r\n\r\nhello"
         assert(sock.write_socket_all(client, raw_req) == raw_req.len, "client writes request")
         def handled = handle_client(peer, fn(in_req) {
               assert_eq(in_req.get("method", ""), "POST", "server handler method")
               assert_eq(in_req.get("path", ""), "/echo", "server handler path")
               assert_eq(in_req.get("body", ""), "hello", "server handler body")
               text("echo:" + in_req.get("query_params", 0).get("name", ""))
         })
         assert_eq(handled.get("ok", false), true, "server handled client")
         def raw_res = sock.read_socket(client, 4096)
         assert(str_contains(raw_res, "HTTP/1.1 200 OK"), "server response status")
         assert(str_contains(raw_res, "echo:ny"), "server response body")
         sock.close_socket(client)
         sock.close_socket(peer)
      }
      sock.close_socket(server)
   }
   print("✓ std.os.net.server self-test passed")
}
