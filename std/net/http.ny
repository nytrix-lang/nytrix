;; Keywords: net http
;; Net Http module.

module std.net.http (
   _http_parse_url,
   http_request_raw, http_request, http_request_url, http_request_url_parsed,
   http_get, http_post, http_put, http_delete,
   http_get_parsed, http_post_parsed, http_put_parsed, http_delete_parsed,
   http_get_url, http_get_url_parsed,
   http_parse_url, http_parse_url_ex, http_parse_query, http_parse_response
)
use std.net.socket *
use std.core as core
use std.core.dict as _d
use std.str *

fn _http_substr(s, a, b){
   "Internal helper."
   core.slice(s, a, b, 1)
}

fn _http_strip_cr(s){
   "Internal helper."
   if(!is_str(s)){ return "" }
   def n = str_len(s)
   if(n > 0 && core.load8(s, n - 1) == 13){
      return _http_substr(s, 0, n - 1)
   }
   s
}

fn _http_last_index_byte(s, want){
   "Internal helper."
   if(!is_str(s)){ return -1 }
   mut i = str_len(s) - 1
   while(i >= 0){
      if(core.load8(s, i) == want){ return i }
      i -= 1
   }
   -1
}

fn _http_is_digit(c){
   "Internal helper."
   c >= 48 && c <= 57
}

fn _http_all_digits(s){
   "Internal helper."
   if(!is_str(s)){ return false }
   def n = str_len(s)
   if(n == 0){ return false }
   mut i = 0
   while(i < n){
      if(!_http_is_digit(core.load8(s, i))){ return false }
      i += 1
   }
   true
}

fn _http_hex_val(c){
   "Internal helper."
   if(c >= 48 && c <= 57){ return c - 48 }
   if(c >= 65 && c <= 70){ return c - 55 }
   if(c >= 97 && c <= 102){ return c - 87 }
   -1
}

fn _http_atoi_hex(s){
   "Internal helper."
   if(!is_str(s)){ return 0 }
   def n = str_len(s)
   mut i = 0
   mut out = 0
   while(i < n){
      def v = _http_hex_val(core.load8(s, i))
      if(v < 0){ break }
      out = out * 16 + v
      i += 1
   }
   out
}

fn _http_default_port_for_scheme(scheme){
   "Internal helper."
   if(lower(scheme) == "https"){ return 443 }
   80
}

fn _http_url_decode_component(s){
   "Internal helper."
   if(!is_str(s)){ return "" }
   def n = str_len(s)
   mut out = ""
   mut i = 0
   while(i < n){
      def c = core.load8(s, i)
      if(c == 37 && i + 2 < n){
         def hi = _http_hex_val(core.load8(s, i + 1))
         def lo = _http_hex_val(core.load8(s, i + 2))
         if(hi >= 0 && lo >= 0){
            out = out + chr(hi * 16 + lo)
            i = i + 3
         } else {
            out = out + "%"
            i += 1
         }
      } elif(c == 43){
         out = out + " "
         i += 1
      } else {
         out = out + chr(c)
         i += 1
      }
   }
   out
}

fn _http_parse_url_ex(url){
   "Internal helper."
   mut out = _d.dict(16)
   out = _d.dict_set(out, "ok", false)
   out = _d.dict_set(out, "scheme", "http")
   out = _d.dict_set(out, "userinfo", "")
   out = _d.dict_set(out, "authority", "")
   out = _d.dict_set(out, "host", "")
   out = _d.dict_set(out, "port", 80)
   out = _d.dict_set(out, "path", "/")
   out = _d.dict_set(out, "query", "")
   out = _d.dict_set(out, "fragment", "")
   out = _d.dict_set(out, "target", "/")
   if(!is_str(url)){ return out }
   mut u = strip(url)
   if(str_len(u) == 0){ return out }

   def n = str_len(u)
   mut pos = 0
   mut scheme = "http"
   def sch_idx = find(u, "://")
   if(sch_idx >= 0){
      scheme = lower(_http_substr(u, 0, sch_idx))
      pos = sch_idx + 3
   }
   mut default_port = _http_default_port_for_scheme(scheme)
   mut auth_end = n
   mut i = pos
   while(i < n){
      def c = core.load8(u, i)
      if(c == 47 || c == 63 || c == 35){
         auth_end = i
         break
      }
      i += 1
   }

   mut authority = _http_substr(u, pos, auth_end)
   mut userinfo = ""
   mut host_port = authority
   def at_idx = _http_last_index_byte(authority, 64) ;; @
   if(at_idx >= 0){
      userinfo = _http_substr(authority, 0, at_idx)
      host_port = _http_substr(authority, at_idx + 1, str_len(authority))
   }

   mut host = host_port
   mut port = default_port
   if(str_len(host_port) > 0 && core.load8(host_port, 0) == 91){ ;; '['
      def rb = find(host_port, "]")
      if(rb >= 0){
         host = _http_substr(host_port, 1, rb)
         if(rb + 1 < str_len(host_port) && core.load8(host_port, rb + 1) == 58){ ;; :
            def pstr = _http_substr(host_port, rb + 2, str_len(host_port))
            if(_http_all_digits(pstr)){ port = atoi(pstr) }
         }
      }
   } else {
      def colon = _http_last_index_byte(host_port, 58)
      if(colon > 0 && find(host_port, ":") == colon){
         def pstr = _http_substr(host_port, colon + 1, str_len(host_port))
         if(_http_all_digits(pstr)){
            host = _http_substr(host_port, 0, colon)
            port = atoi(pstr)
         }
      }
   }
   if(port <= 0){ port = default_port }

   mut path = "/"
   mut query = ""
   mut fragment = ""
   if(auth_end < n){
      def c0 = core.load8(u, auth_end)
      if(c0 == 47){ ;; /
         mut j = auth_end
         while(j < n && core.load8(u, j) != 63 && core.load8(u, j) != 35){ j += 1 }
         path = _http_substr(u, auth_end, j)
         if(j < n && core.load8(u, j) == 63){
            def q0 = j + 1
            j = q0
            while(j < n && core.load8(u, j) != 35){ j += 1 }
            query = _http_substr(u, q0, j)
            if(j < n && core.load8(u, j) == 35){ fragment = _http_substr(u, j + 1, n) }
         } elif(j < n && core.load8(u, j) == 35){
            fragment = _http_substr(u, j + 1, n)
         }
      } elif(c0 == 63){ ;; ?
         def q0 = auth_end + 1
         mut j = q0
         while(j < n && core.load8(u, j) != 35){ j += 1 }
         query = _http_substr(u, q0, j)
         if(j < n && core.load8(u, j) == 35){ fragment = _http_substr(u, j + 1, n) }
      } elif(c0 == 35){ ;; #
         fragment = _http_substr(u, auth_end + 1, n)
      }
   }
   if(str_len(path) == 0){ path = "/" }
   mut target = path
   if(str_len(query) > 0){ target = f"{path}?{query}" }

   out = _d.dict_set(out, "ok", true)
   out = _d.dict_set(out, "scheme", scheme)
   out = _d.dict_set(out, "userinfo", userinfo)
   out = _d.dict_set(out, "authority", authority)
   out = _d.dict_set(out, "host", host)
   out = _d.dict_set(out, "port", port)
   out = _d.dict_set(out, "path", path)
   out = _d.dict_set(out, "query", query)
   out = _d.dict_set(out, "fragment", fragment)
   out = _d.dict_set(out, "target", target)
   out
}

fn _http_parse_url(url){
   "Parses URL into `[host, port, target]`."
   def u = _http_parse_url_ex(url)
   def host = _d.dict_get(u, "host", "")
   def port = _d.dict_get(u, "port", 80)
   def target = _d.dict_get(u, "target", "/")
   return [host, port, target]
}

fn _http_read_all(fd){
   "Read all socket data until EOF."
   mut res = ""
   while(1){
      def chunk = read_socket(fd, 4096)
      if(core.len(chunk) == 0){ break }
      res = f"{res}{chunk}"
   }
   return res
}

fn _http_has_header(headers, want_name){
   "Internal helper."
   if(!is_dict(headers)){ return false }
   def want = lower(want_name)
   def items = _d.dict_items(headers)
   mut i = 0
   while(i < core.len(items)){
      def pair = core.get(items, i)
      def k = core.get(pair, 0)
      if(is_str(k) && (lower(strip(k)) == want)){ return true }
      i += 1
   }
   false
}

fn _http_headers_to_lines(headers){
   "Internal helper."
   if(!is_dict(headers)){ return "" }
   def items = _d.dict_items(headers)
   mut i = 0
   mut out = ""
   while(i < core.len(items)){
      def pair = core.get(items, i)
      def k = core.get(pair, 0)
      def v = core.get(pair, 1)
      if(is_str(k) && str_len(strip(k)) > 0){
         def vv = is_str(v) ? v : f"{v}"
         out = f"{out}{k}: {vv}\r\n"
      }
      i += 1
   }
   out
}

fn _http_normalize_target(path){
   "Internal helper."
   mut target = path
   if(!is_str(target) || str_len(target) == 0){ return "/" }
   if(startswith(target, "http://") || startswith(target, "https://")){
      def u = _http_parse_url_ex(target)
      return _d.dict_get(u, "target", "/")
   }
   if(core.load8(target, 0) == 47){ return target } ;; /
   if(core.load8(target, 0) == 63){ return "/" + target }
   "/" + target
}

fn _http_request(method, host, port, path, data=0, headers=0){
   "Perform an HTTP request and return raw response."
   def fd = socket_connect(host, port)
   if(fd < 0){ return "" }

   mut body = ""
   if(data != 0){
      body = is_str(data) ? data : f"{data}"
   }
   mut target = _http_normalize_target(path)

   mut req = f"{method} {target} HTTP/1.1\r\n"
   mut host_hdr = host
   if(port != 80 && port != 443){ host_hdr = f"{host}:{port}" }
   if(!_http_has_header(headers, "host")){
      req = f"{req}Host: {host_hdr}\r\n"
   }
   if(!_http_has_header(headers, "user-agent")){
      req = req + "User-Agent: Nytrix/1.0\r\n"
   }
   if(!_http_has_header(headers, "accept")){
      req = req + "Accept: */*\r\n"
   }
   if(!_http_has_header(headers, "connection")){
      req = req + "Connection: close\r\n"
   }
   if(str_len(body) > 0 && !_http_has_header(headers, "content-length")){
      req = f"{req}Content-Length: {str_len(body)}\r\n"
   }
   if(str_len(body) > 0 && !_http_has_header(headers, "content-type")){
      req = req + "Content-Type: text/plain; charset=utf-8\r\n"
   }
   req = req + _http_headers_to_lines(headers) + "\r\n"
   if(str_len(body) > 0){ req = req + body }

   write_socket(fd, req)
   def res = _http_read_all(fd)
   close_socket(fd)
   return res
}

fn _http_decode_chunked(body){
   "Internal helper."
   if(!is_str(body)){ return "" }
   mut out = ""
   def n = str_len(body)
   mut i = 0
   while(i < n){
      mut line_end = -1
      mut j = i
      while(j < n){
         if(core.load8(body, j) == 10){ ;; \n
            line_end = j
            break
         }
         j += 1
      }
      if(line_end < 0){ break }
      mut line = _http_substr(body, i, line_end)
      line = _http_strip_cr(line)
      def semi = find(line, ";")
      if(semi >= 0){ line = _http_substr(line, 0, semi) }
      line = strip(line)
      def chunk_n = _http_atoi_hex(line)
      i = line_end + 1
      if(chunk_n <= 0){ break }
      if(i + chunk_n > n){ break }
      out = out + _http_substr(body, i, i + chunk_n)
      i = i + chunk_n
      if(i < n && core.load8(body, i) == 13){ i += 1 }
      if(i < n && core.load8(body, i) == 10){ i += 1 }
   }
   out
}

fn http_parse_response(raw){
   "Parses raw HTTP response into map with status, headers, and body."
   mut out = _d.dict(16)
   out = _d.dict_set(out, "ok", false)
   out = _d.dict_set(out, "protocol", "")
   out = _d.dict_set(out, "status", 0)
   out = _d.dict_set(out, "reason", "")
   out = _d.dict_set(out, "headers", _d.dict(16))
   out = _d.dict_set(out, "raw_headers", "")
   out = _d.dict_set(out, "body", "")
   out = _d.dict_set(out, "raw", raw)
   if(!is_str(raw)){ return out }

   mut split_idx = find(raw, "\r\n\r\n")
   mut split_len = 4
   if(split_idx < 0){
      split_idx = find(raw, "\n\n")
      split_len = 2
   }
   mut head = raw
   mut body = ""
   if(split_idx >= 0){
      head = _http_substr(raw, 0, split_idx)
      body = _http_substr(raw, split_idx + split_len, str_len(raw))
   }

   mut status_line = head
   mut header_lines = ""
   def lf = find(head, "\n")
   if(lf >= 0){
      status_line = _http_substr(head, 0, lf)
      header_lines = _http_substr(head, lf + 1, str_len(head))
   }
   status_line = _http_strip_cr(status_line)
   mut protocol = ""
   mut status = 0
   mut reason = ""
   def sp1 = find(status_line, " ")
   if(sp1 >= 0){
      protocol = _http_substr(status_line, 0, sp1)
      def rest = strip(_http_substr(status_line, sp1 + 1, str_len(status_line)))
      def sp2 = find(rest, " ")
      if(sp2 >= 0){
         status = atoi(_http_substr(rest, 0, sp2))
         reason = strip(_http_substr(rest, sp2 + 1, str_len(rest)))
      } else {
         status = atoi(rest)
      }
   } else {
      protocol = status_line
   }

   mut headers = _d.dict(16)
   if(str_len(header_lines) > 0){
      def lines = split(header_lines, "\n")
      mut i = 0
      while(i < core.len(lines)){
         mut line = _http_strip_cr(core.get(lines, i))
         line = strip(line)
         if(str_len(line) > 0){
            def cidx = find(line, ":")
            if(cidx > 0){
               def k = lower(strip(_http_substr(line, 0, cidx)))
               def v = strip(_http_substr(line, cidx + 1, str_len(line)))
               headers = _d.dict_set(headers, k, v)
            }
         }
         i += 1
      }
   }

   mut parsed_body = body
   def te = lower(_d.dict_get(headers, "transfer-encoding", ""))
   if(str_contains(te, "chunked")){
      parsed_body = _http_decode_chunked(body)
   } else {
      def cl = _d.dict_get(headers, "content-length", "")
      if(is_str(cl) && _http_all_digits(cl)){
         def want = atoi(cl)
         if(want >= 0 && want <= str_len(body)){
            parsed_body = _http_substr(body, 0, want)
         }
      }
   }

   out = _d.dict_set(out, "protocol", protocol)
   out = _d.dict_set(out, "status", status)
   out = _d.dict_set(out, "reason", reason)
   out = _d.dict_set(out, "headers", headers)
   out = _d.dict_set(out, "raw_headers", head)
   out = _d.dict_set(out, "body", parsed_body)
   out = _d.dict_set(out, "ok", status >= 200 && status < 300)
   out
}

fn http_request_raw(method, host, port, path, data=0, headers=0){
   "Performs raw HTTP request and returns raw response."
   _http_request(method, host, port, path, data, headers)
}

fn _http_request_parsed(method, host, port, path, data=0, headers=0){
   "Internal helper."
   http_parse_response(_http_request(method, host, port, path, data, headers))
}

fn _http_request_method(method, host, port, path, headers=0, data=0){
   "Internal helper."
   _http_request(method, host, port, path, data, headers)
}

fn _http_request_method_parsed(method, host, port, path, headers=0, data=0){
   "Internal helper."
   _http_request_parsed(method, host, port, path, data, headers)
}

fn _http_request_url_raw(method, url, data=0, headers=0){
   "Internal helper."
   def u = _http_parse_url_ex(url)
   _http_request(method, _d.dict_get(u, "host", ""), _d.dict_get(u, "port", 80), _d.dict_get(u, "target", "/"), data, headers)
}

fn _http_request_url_parsed(method, url, data=0, headers=0){
   "Internal helper."
   http_parse_response(_http_request_url_raw(method, url, data, headers))
}

fn http_request(method, host, port, path, data=0, headers=0){
   "Performs HTTP request and returns parsed response map."
   _http_request_parsed(method, host, port, path, data, headers)
}

fn http_request_url(method, url, data=0, headers=0){
   "Performs raw HTTP request against URL."
   _http_request_url_raw(method, url, data, headers)
}

fn http_request_url_parsed(method, url, data=0, headers=0){
   "Performs HTTP request against URL and returns parsed response."
   _http_request_url_parsed(method, url, data, headers)
}

fn http_get(host, port, path, headers=0){
   "Performs an HTTP GET request and returns raw response."
   _http_request_method("GET", host, port, path, headers)
}

fn http_get_parsed(host, port, path, headers=0){
   "Performs an HTTP GET request and returns parsed response."
   _http_request_method_parsed("GET", host, port, path, headers)
}

fn http_post(host, port, path, data, headers=0){
   "Performs an HTTP POST request and returns raw response."
   _http_request_method("POST", host, port, path, headers, data)
}

fn http_post_parsed(host, port, path, data, headers=0){
   "Performs an HTTP POST request and returns parsed response."
   _http_request_method_parsed("POST", host, port, path, headers, data)
}

fn http_put(host, port, path, data, headers=0){
   "Performs an HTTP PUT request and returns raw response."
   _http_request_method("PUT", host, port, path, headers, data)
}

fn http_put_parsed(host, port, path, data, headers=0){
   "Performs an HTTP PUT request and returns parsed response."
   _http_request_method_parsed("PUT", host, port, path, headers, data)
}

fn http_delete(host, port, path, headers=0){
   "Performs an HTTP DELETE request and returns raw response."
   _http_request_method("DELETE", host, port, path, headers)
}

fn http_delete_parsed(host, port, path, headers=0){
   "Performs an HTTP DELETE request and returns parsed response."
   _http_request_method_parsed("DELETE", host, port, path, headers)
}

fn http_get_url(url, headers=0){
   "Performs raw HTTP GET request to URL."
   _http_request_url_raw("GET", url, 0, headers)
}

fn http_get_url_parsed(url, headers=0){
   "Performs HTTP GET request to URL and returns parsed response."
   _http_request_url_parsed("GET", url, 0, headers)
}

fn http_parse_url(url){
   "Parses URL string into list `[host, port, target]`."
   _http_parse_url(url)
}

fn http_parse_url_ex(url){
   "Parses URL string into map `{scheme, host, port, path, query, fragment, target, ...}`."
   _http_parse_url_ex(url)
}

fn http_parse_query(q){
   "Parses URL query string into decoded dictionary."
   mut d = _d.dict(16)
   if(q == 0 || !is_str(q)){ return d }
   if(startswith(q, "?")){ q = _http_substr(q, 1, str_len(q)) }
   mut i = 0
   def n = str_len(q)
   while(i<n){
      mut j = i
      while(j < n && core.load8(q, j) != 38){ j += 1 } ;; &
      def part = _http_substr(q, i, j)
      def eqi = find(part, "=")
      if(eqi >= 0){
         def k = _http_url_decode_component(strip(_http_substr(part, 0, eqi)))
         def v = _http_url_decode_component(strip(_http_substr(part, eqi + 1, str_len(part))))
         d = _d.dict_set(d, k, v)
      } else {
         def k = _http_url_decode_component(strip(part))
         if(str_len(k) > 0){ d = _d.dict_set(d, k, 1) }
      }
      i = j + 1
   }
   d
}

if(comptime{__main()}){
    use std.net.http *
    use std.core *
    use std.core.list *
    use std.core.dict *
    use std.core.error *
    use std.str.io *

    print("Testing HTTP...")

    def part = http_parse_url("http://google.com/foo")
    assert((get(part, 0) == "google.com"), "parse host")
    assert(get(part, 1) == 80, "parse port")
    assert((get(part, 2) == "/foo"), "parse path")

    def part2 = http_parse_url("https://example.com/api/v1?q=1#x")
    assert((get(part2, 0) == "example.com"), "parse host https")
    assert(get(part2, 1) == 443, "parse https default port")
    assert((get(part2, 2) == "/api/v1?q=1"), "parse target with query")

    def ex = http_parse_url_ex("https://user:pw@[::1]:8443/path?a=1&b=2#frag")
    assert(dict_get(ex, "ok", false), "url_ex ok")
    assert((dict_get(ex, "scheme", "") == "https"), "url_ex scheme")
    assert((dict_get(ex, "userinfo", "") == "user:pw"), "url_ex userinfo")
    assert((dict_get(ex, "host", "") == "::1"), "url_ex host ipv6")
    assert(dict_get(ex, "port", 0) == 8443, "url_ex port")
    assert((dict_get(ex, "path", "") == "/path"), "url_ex path")
    assert((dict_get(ex, "query", "") == "a=1&b=2"), "url_ex query")
    assert((dict_get(ex, "fragment", "") == "frag"), "url_ex fragment")
    assert((dict_get(ex, "target", "") == "/path?a=1&b=2"), "url_ex target")

    def q = http_parse_query("a=1&b=hello%20world&c&d=one+two")
    assert((dict_get(q, "a") == "1"), "query a")
    assert((dict_get(q, "b") == "hello world"), "query decode percent")
    assert(dict_get(q, "c", 0) == 1, "query flag key")
    assert((dict_get(q, "d") == "one two"), "query decode plus")

    def raw = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhelloEXTRA"
    def parsed = http_parse_response(raw)
    assert(dict_get(parsed, "ok", false), "response ok")
    assert(dict_get(parsed, "status", 0) == 200, "response status")
    assert((dict_get(parsed, "reason", "") == "OK"), "response reason")
    def hdr = dict_get(parsed, "headers", dict(4))
    assert((dict_get(hdr, "content-type", "") == "text/plain"), "response content-type")
    assert((dict_get(parsed, "body", "") == "hello"), "response content-length body trim")

    def raw_chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"
    def parsed_chunked = http_parse_response(raw_chunked)
    assert(dict_get(parsed_chunked, "status", 0) == 200, "chunked status")
    assert((dict_get(parsed_chunked, "body", "") == "Wikipedia"), "chunked decode")

    print("✓ std.net.http tests passed")
}
