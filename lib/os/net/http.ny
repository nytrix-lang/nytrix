;; Keywords: net socket http web os
;; Hypertext Transfer Protocol (HTTP) Client for Nytrix
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc9110.html
;; References:
;; - std.os.net
;; - std.os
module std.os.net.http(_http_parse_url, http_request_raw, http_request, http_request_url, http_request_url_parsed, http_get, http_post, http_put, http_delete, http_get_parsed, http_post_parsed, http_put_parsed, http_delete_parsed, http_get_url, http_get_url_parsed, http_parse_url, http_parse_url_ex, http_parse_query, http_parse_response)
use std.os.net.socket
use std.core
use std.core.dict_mod as _d
use std.core.str
use std.core.common as common

def _HTTP_MAX_RESPONSE_BYTES = 64 * 1024 * 1024

fn _http_substr(str s, int a, int b) str { slice(s, a, b, 1) }

fn _http_strip_cr(any s) str {
   if !is_str(s) { return "" }
   def n = s.len
   if n > 0 && load8(s, n - 1) == 13 { return _http_substr(s, 0, n - 1) }
   s
}

fn _http_is_digit(int c) bool { c >= 48 && c <= 57 }

fn _http_all_digits(any s) bool {
   if !is_str(s) { return false }
   def n = s.len
   if n == 0 { return false }
   mut i = 0
   while i < n {
      if !_http_is_digit(load8(s, i)) { return false }
      i += 1
   }
   true
}

fn _http_atoi_hex(any s) int {
   if !is_str(s) { return 0 }
   def n = s.len
   mut i = 0
   mut out = 0
   while i < n {
      def v = hex_val(load8(s, i))
      if v < 0 { break }
      out = out * 16 + v
      i += 1
   }
   out
}

fn _http_default_port_for_scheme(any scheme) int {
   if lower(scheme) == "https" { return 443 }
   80
}

fn _http_parse_port(any pstr, int default_port) int {
   if !_http_all_digits(pstr) { return default_port }
   def n = pstr.len
   if n == 0 || n > 5 { return default_port }
   def p = atoi(pstr)
   if p < 1 || p > 65535 { return default_port }
   p
}

fn _http_has_ctl(any s) bool {
   if !is_str(s) { return true }
   mut i = 0
   def n = s.len
   while i < n {
      if case load8(s, i) { 0, 10, 13 -> true _ -> false }{ return true }
      i += 1
   }
   false
}

fn _http_is_tchar(int c) bool {
   ((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) ||
      c == 33 || c == 35 || c == 36 || c == 37 || c == 38 || c == 39 ||
      c == 42 || c == 43 || c == 45 || c == 46 || c == 94 || c == 95 ||
   c == 96 || c == 124 || c == 126)
}

fn _http_valid_token(any s) bool {
   if !is_str(s) { return false }
   def n = s.len
   if n == 0 { return false }
   mut i = 0
   while i < n {
      if !_http_is_tchar(load8(s, i)) { return false }
      i += 1
   }
   true
}

fn _http_parse_dec_bounded(any s, int max_v) int {
   if !_http_all_digits(s) { return -1 }
   def n = s.len
   if n == 0 || n > 10 { return -1 }
   def v = atoi(s)
   if v < 0 || v > max_v { return -1 }
   v
}

fn _http_url_decode_component(any s) str {
   if !is_str(s) { return "" }
   def n = s.len
   mut b, i = Builder(n + 8), 0
   while i < n {
      def c = load8(s, i)
      if c == 37 && i + 2 < n {
         def hi = hex_val(load8(s, i + 1))
         def lo = hex_val(load8(s, i + 2))
         if hi >= 0 && lo >= 0 {
            b, i = builder_append(b, chr(hi * 16 + lo)), i + 3
         } else {
            b = builder_append(b, "%")
            i += 1
         }
      } elif c == 43 {
         b = builder_append(b, " ")
         i += 1
      } else {
         b = builder_append(b, chr(c))
         i += 1
      }
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _http_url_fields() dict {
   {
      "ok": false,
      "scheme": "http",
      "userinfo": "",
      "authority": "",
      "host": "",
      "port": 80,
      "path": "/",
      "query": "",
      "fragment": "",
      "target": "/"
   }
}

fn _http_response_fields(any raw) dict {
   {
      "ok": false,
      "protocol": "",
      "status": 0,
      "reason": "",
      "headers": _d.dict(16),
      "raw_headers": "",
      "body": "",
      "raw": raw
   }
}

fn _http_authority_parts(str authority, int default_port) list {
   mut userinfo = ""
   mut host_port = authority
   def at_idx = common.last_index_byte(authority, 64)
   if at_idx >= 0 {
      userinfo = _http_substr(authority, 0, at_idx)
      host_port = _http_substr(authority, at_idx + 1, authority.len)
   }
   mut host = host_port
   mut port = default_port
   if host_port.len > 0 && load8(host_port, 0) == 91 {
      def rb = find(host_port, "]")
      if rb >= 0 {
         host = _http_substr(host_port, 1, rb)
         if rb + 1 < host_port.len && load8(host_port, rb + 1) == 58 {
            port = _http_parse_port(_http_substr(host_port, rb + 2, host_port.len), default_port)
         }
      }
   } else {
      def colon = common.last_index_byte(host_port, 58)
      if colon > 0 && find(host_port, ":") == colon {
         def pstr = _http_substr(host_port, colon + 1, host_port.len)
         if _http_all_digits(pstr) {
            host = _http_substr(host_port, 0, colon)
            port = _http_parse_port(pstr, default_port)
         }
      }
   }
   if port <= 0 { port = default_port }
   [userinfo, host, port]
}

fn _http_parse_url_ex(any url) dict {
   mut out = _http_url_fields()
   if !is_str(url) { return out }
   mut u = strip(url)
   if u.len == 0 { return out }
   def n = u.len
   mut pos = 0
   mut scheme = "http"
   def sch_idx = find(u, "://")
   if sch_idx >= 0 {
      scheme = lower(_http_substr(u, 0, sch_idx))
      pos = sch_idx + 3
   }
   mut default_port = _http_default_port_for_scheme(scheme)
   mut auth_end = n
   mut i = pos
   while i < n {
      def c = load8(u, i)
      if c == 47 || c == 63 || c == 35 {
         auth_end = i
         break
      }
      i += 1
   }
   def authority = _http_substr(u, pos, auth_end)
   def auth = _http_authority_parts(authority, default_port)
   def userinfo, host, port = auth[0], auth[1], auth[2]
   mut path = "/"
   mut query = ""
   mut fragment = ""
   if auth_end < n {
      def c0 = load8(u, auth_end)
      if c0 == 47 {
         mut j = auth_end
         while j < n && load8(u, j) != 63 && load8(u, j) != 35 { j += 1 }
         path = _http_substr(u, auth_end, j)
         if j < n && load8(u, j) == 63 {
            def q0 = j + 1
            j = q0
            while j < n && load8(u, j) != 35 { j += 1 }
            query = _http_substr(u, q0, j)
            if j < n && load8(u, j) == 35 { fragment = _http_substr(u, j + 1, n) }
         } elif j < n && load8(u, j) == 35 {
            fragment = _http_substr(u, j + 1, n)
         }
      } elif c0 == 63 {
         def q0 = auth_end + 1
         mut j = q0
         while j < n && load8(u, j) != 35 { j += 1 }
         query = _http_substr(u, q0, j)
         if j < n && load8(u, j) == 35 { fragment = _http_substr(u, j + 1, n) }
      } elif c0 == 35 {
         fragment = _http_substr(u, auth_end + 1, n)
      }
   }
   if path.len == 0 { path = "/" }
   mut target = path
   if query.len > 0 { target = f"{path}?{query}" }
   out = out.set("ok", true)
   out = out.set("scheme", scheme)
   out = out.set("userinfo", userinfo)
   out = out.set("authority", authority)
   out = out.set("host", host)
   out = out.set("port", port)
   out = out.set("path", path)
   out = out.set("query", query)
   out = out.set("fragment", fragment)
   out = out.set("target", target)
   out
}

fn _http_parse_url(any url) list {
   def u = _http_parse_url_ex(url)
   def host = u.get("host", "")
   def port = u.get("port", 80)
   def target = u.get("target", "/")
   return [host, port, target]
}

fn _http_read_all(int fd, int max_bytes=_HTTP_MAX_RESPONSE_BYTES) str {
   mut b = Builder(4096)
   mut total = 0
   while 1 {
      def left = max_bytes - total
      if left <= 0 { break }
      def want = (left < 4096) ? left : 4096
      def chunk = read_socket(fd, want)
      if chunk.len == 0 { break }
      b = builder_append(b, chunk)
      total += chunk.len
   }
   def res = builder_to_str(b)
   builder_free(b)
   res
}

fn _http_has_header(any headers, str want_name) bool {
   if !is_dict(headers) { return false }
   def want = lower(want_name)
   def items = _d.dict_items(headers)
   mut i = 0
   while i < items.len {
      def pair = items.get(i)
      def k = pair.get(0)
      if is_str(k) && (lower(strip(k)) == want) { return true }
      i += 1
   }
   false
}

fn _http_headers_to_lines(any headers) str {
   if !is_dict(headers) { return "" }
   def items = _d.dict_items(headers)
   mut i, b = 0, Builder(256)
   while i < items.len {
      def pair = items.get(i)
      def k = pair.get(0)
      def v = pair.get(1)
      if is_str(k) && len(strip(k)) > 0 {
         def vv = is_str(v) ? v : f"{v}"
         if !_http_has_ctl(k) && !_http_has_ctl(vv) { b = builder_append(b, f"{k}: {vv}\r\n") }
      }
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _http_send_all(int fd, any s) bool {
   if !is_str(s) { return false }
   mut off = 0
   def n = s.len
   while off < n {
      def wrote = write_socket_part(fd, s, off, n - off)
      if wrote <= 0 { return false }
      off += wrote
   }
   true
}

fn _http_normalize_target(any path) str {
   mut target = path
   if !is_str(target) || target.len == 0 { return "/" }
   if startswith(target, "http://") || startswith(target, "https://") {
      def u = _http_parse_url_ex(target)
      return u.get("target", "/")
   }
   if load8(target, 0) == 47 { return target }
   if load8(target, 0) == 63 { return "/" + target }
   "/" + target
}

fn _http_request(any method, any host, int port, any path, any data=0, any headers=0) str {
   if !_http_valid_token(method) { return "" }
   if !is_str(host) || host.len == 0 || _http_has_ctl(host) { return "" }
   def fd = socket_connect(host, port)
   if fd < 0 { return "" }
   defer { close_socket(fd) }
   mut body = ""
   if data != 0 { body = is_str(data) ? data : f"{data}" }
   mut target = _http_normalize_target(path)
   if _http_has_ctl(target) { return "" }
   mut b = Builder(512)
   b = builder_append(b, f"{method} {target} HTTP/1.1\r\n")
   mut host_hdr = host
   if port != 80 && port != 443 { host_hdr = f"{host}:{port}" }
   if !_http_has_header(headers, "host") { b = builder_append(b, f"Host: {host_hdr}\r\n") }
   if !_http_has_header(headers, "user-agent") { b = builder_append(b, "User-Agent: Nytrix/1.0\r\n") }
   if !_http_has_header(headers, "accept") { b = builder_append(b, "Accept: */*\r\n") }
   if !_http_has_header(headers, "connection") { b = builder_append(b, "Connection: close\r\n") }
   if body.len > 0 && !_http_has_header(headers, "content-length") { b = builder_append(b, f"Content-Length: {body.len}\r\n") }
   if body.len > 0 && !_http_has_header(headers, "content-type") { b = builder_append(b, "Content-Type: text/plain; charset=utf-8\r\n") }
   b = builder_append(b, _http_headers_to_lines(headers))
   b = builder_append(b, "\r\n")
   if body.len > 0 { b = builder_append(b, body) }
   def req = builder_to_str(b)
   builder_free(b)
   if !_http_send_all(fd, req) { return "" }
   _http_read_all(fd, _HTTP_MAX_RESPONSE_BYTES)
}

fn _http_decode_chunked(any body) str {
   if !is_str(body) { return "" }
   mut b = Builder(256)
   mut out_n = 0
   def n = body.len
   mut i = 0
   while i < n {
      mut line_end = -1
      mut j = i
      while j < n {
         if load8(body, j) == 10 {
            line_end = j
            break
         }
         j += 1
      }
      if line_end < 0 { break }
      mut line = _http_substr(body, i, line_end)
      line = _http_strip_cr(line)
      def semi = find(line, ";")
      if semi >= 0 { line = _http_substr(line, 0, semi) }
      line = strip(line)
      def chunk_n = _http_atoi_hex(line)
      i = line_end + 1
      if chunk_n <= 0 { break }
      if i + chunk_n > n { break }
      if out_n + chunk_n > _HTTP_MAX_RESPONSE_BYTES { break }
      b = builder_append(b, _http_substr(body, i, i + chunk_n))
      out_n += chunk_n
      i = i + chunk_n
      if i < n && load8(body, i) == 13 { i += 1 }
      if i < n && load8(body, i) == 10 { i += 1 }
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn http_parse_response(any raw) dict {
   "Parses raw HTTP response into map with status, headers, and body."
   mut out = _http_response_fields(raw)
   if !is_str(raw) { return out }
   mut split_idx = find(raw, "\r\n\r\n")
   mut split_len = 4
   if split_idx < 0 {
      split_idx = find(raw, "\n\n")
      split_len = 2
   }
   mut head = raw
   mut body = ""
   if split_idx >= 0 {
      head = _http_substr(raw, 0, split_idx)
      body = _http_substr(raw, split_idx + split_len, raw.len)
   }
   mut status_line = head
   mut header_lines = ""
   def lf = find(head, "\n")
   if lf >= 0 {
      status_line = _http_substr(head, 0, lf)
      header_lines = _http_substr(head, lf + 1, head.len)
   }
   status_line = _http_strip_cr(status_line)
   mut protocol = ""
   mut status = 0
   mut reason = ""
   def sp1 = find(status_line, " ")
   if sp1 >= 0 {
      protocol = _http_substr(status_line, 0, sp1)
      def rest = strip(_http_substr(status_line, sp1 + 1, status_line.len))
      def sp2 = find(rest, " ")
      if sp2 >= 0 {
         status = atoi(_http_substr(rest, 0, sp2))
         reason = strip(_http_substr(rest, sp2 + 1, rest.len))
      } else {
         status = atoi(rest)
      }
   } else {
      protocol = status_line
   }
   mut headers = _d.dict(16)
   if header_lines.len > 0 {
      def lines = split(header_lines, "\n")
      mut i = 0
      while i < lines.len {
         mut line = _http_strip_cr(lines.get(i))
         line = strip(line)
         if line.len > 0 {
            def cidx = find(line, ":")
            if cidx > 0 {
               def k, v = lower(strip(_http_substr(line, 0, cidx))), strip(_http_substr(line, cidx + 1, line.len))
               headers = headers.set(k, v)
            }
         }
         i += 1
      }
   }
   mut parsed_body = body
   def te = lower(headers.get("transfer-encoding", ""))
   if str_contains(te, "chunked") { parsed_body = _http_decode_chunked(body) } else {
      def cl = headers.get("content-length", "")
      if is_str(cl) {
         def want = _http_parse_dec_bounded(cl, body.len)
         if want >= 0 { parsed_body = _http_substr(body, 0, want) }
      }
   }
   out = out.set("protocol", protocol)
   out = out.set("status", status)
   out = out.set("reason", reason)
   out = out.set("headers", headers)
   out = out.set("raw_headers", head)
   out = out.set("body", parsed_body)
   out = out.set("ok", status >= 200 && status < 300)
   out
}

fn http_request_raw(any method, any host, int port, any path, any data=0, any headers=0) str {
   "Performs raw HTTP request and returns raw response."
   _http_request(method, host, port, path, data, headers)
}

fn _http_request_host(any method, any host, int port, any path, any data=0, any headers=0, bool parsed=false) any {
   def raw = _http_request(method, host, port, path, data, headers)
   if parsed { return http_parse_response(raw) }
   raw
}

fn _http_request_url(any method, any url, any data=0, any headers=0, bool parsed=false) any {
   def u = _http_parse_url_ex(url)
   _http_request_host(method, u.get("host", ""), u.get("port", 80), u.get("target", "/"), data, headers, parsed)
}

fn http_request(any method, any host, int port, any path, any data=0, any headers=0) dict {
   "Performs HTTP request and returns parsed response map."
   _http_request_host(method, host, port, path, data, headers, true)
}

fn http_request_url(any method, any url, any data=0, any headers=0) str {
   "Performs raw HTTP request against URL."
   _http_request_url(method, url, data, headers)
}

fn http_request_url_parsed(any method, any url, any data=0, any headers=0) dict {
   "Performs HTTP request against URL and returns parsed response."
   _http_request_url(method, url, data, headers, true)
}

fn http_get(any host, int port, any path, any headers=0) str {
   "Performs an HTTP GET request and returns raw response."
   _http_request_host("GET", host, port, path, 0, headers)
}

fn http_get_parsed(any host, int port, any path, any headers=0) dict {
   "Performs an HTTP GET request and returns parsed response."
   _http_request_host("GET", host, port, path, 0, headers, true)
}

fn http_post(any host, int port, any path, any data, any headers=0) str {
   "Performs an HTTP POST request and returns raw response."
   _http_request_host("POST", host, port, path, data, headers)
}

fn http_post_parsed(any host, int port, any path, any data, any headers=0) dict {
   "Performs an HTTP POST request and returns parsed response."
   _http_request_host("POST", host, port, path, data, headers, true)
}

fn http_put(any host, int port, any path, any data, any headers=0) str {
   "Performs an HTTP PUT request and returns raw response."
   _http_request_host("PUT", host, port, path, data, headers)
}

fn http_put_parsed(any host, int port, any path, any data, any headers=0) dict {
   "Performs an HTTP PUT request and returns parsed response."
   _http_request_host("PUT", host, port, path, data, headers, true)
}

fn http_delete(any host, int port, any path, any headers=0) str {
   "Performs an HTTP DELETE request and returns raw response."
   _http_request_host("DELETE", host, port, path, 0, headers)
}

fn http_delete_parsed(any host, int port, any path, any headers=0) dict {
   "Performs an HTTP DELETE request and returns parsed response."
   _http_request_host("DELETE", host, port, path, 0, headers, true)
}

fn http_get_url(any url, any headers=0) str {
   "Performs raw HTTP GET request to URL."
   _http_request_url("GET", url, 0, headers)
}

fn http_get_url_parsed(any url, any headers=0) dict {
   "Performs HTTP GET request to URL and returns parsed response."
   _http_request_url("GET", url, 0, headers, true)
}

fn http_parse_url(any url) list {
   "Parses URL string into list `[host, port, target]`."
   _http_parse_url(url)
}

fn http_parse_url_ex(any url) dict {
   "Parses URL string into map `{scheme, host, port, path, query, fragment, target, ...}`."
   _http_parse_url_ex(url)
}

fn http_parse_query(any q) dict {
   "Parses URL query string into decoded dictionary."
   mut d = _d.dict(16)
   if q == 0 || !is_str(q) { return d }
   if startswith(q, "?") { q = _http_substr(q, 1, q.len) }
   mut i = 0
   def n = q.len
   while i<n {
      mut j = i
      while j < n && load8(q, j) != 38 { j += 1 }
      def part = _http_substr(q, i, j)
      def eqi = find(part, "=")
      if eqi >= 0 {
         def k = _http_url_decode_component(strip(_http_substr(part, 0, eqi)))
         def v = _http_url_decode_component(strip(_http_substr(part, eqi + 1, part.len)))
         d = d.set(k, v)
      } else {
         def k = _http_url_decode_component(strip(part))
         if k.len > 0 { d = d.set(k, 1) }
      }
      i = j + 1
   }
   d
}

#main {
   def p = http_parse_url("http://google.com/foo")
   assert(p.get(0) == "google.com" && p.get(1) == 80 && p.get(2) == "/foo", "http parse basic url")
   def p2 = http_parse_url("https://example.com/api/v1?q=1#x")
   assert(p2.get(0) == "example.com" && p2.get(1) == 443 && p2.get(2) == "/api/v1?q=1", "http parse https url")
   def ex = http_parse_url_ex("https://user:pw@[::1]:8443/path?a=1&b=2#frag")
   assert(ex.get("ok", false) && ex.get("scheme", "") == "https" && ex.get("userinfo", "") == "user:pw" && ex.get("host", "") == "::1", "http url ex authority")
   assert(ex.get("port", 0) == 8443 && ex.get("path", "") == "/path" && ex.get("query", "") == "a=1&b=2" && ex.get("fragment", "") == "frag" && ex.get("target", "") == "/path?a=1&b=2", "http url ex target")
   def q = http_parse_query("a=1&b=hello%20world&c&d=one+two")
   assert(q.get("a") == "1" && q.get("b") == "hello world" && q.get("c", 0) == 1 && q.get("d") == "one two", "http query parse")
   def raw = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhelloEXTRA"
   def parsed = http_parse_response(raw)
   assert(parsed.get("ok", false) && parsed.get("status", 0) == 200 && parsed.get("reason", "") == "OK", "http response status")
   assert(parsed.get("headers", dict(4)).get("content-type", "") == "text/plain" && parsed.get("body", "") == "hello", "http response body")
   def raw_chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"
   assert(http_parse_response(raw_chunked).get("body", "") == "Wikipedia", "http chunked response")
   print("✓ std.os.net.http self-test passed")
}
