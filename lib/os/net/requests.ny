;; Keywords: net socket requests http-client request
;; High-level HTTP requests for Nytrix.
module std.os.net.requests(
   request, request_raw, prepare_request, requests_prepare,
   requests_get, requests_get_host, requests_get_parsed, requests_get_host_parsed,
   requests_post, requests_put, requests_delete, requests_post_raw,
   requests_put_raw, requests_delete_raw, requests_request, requests_request_raw,
   requests, requests_parse_url, requests_parse_url_ex, requests_parse_query, requests_parse_response
)

use std.os.net.http
use std.os.net.curl as curl
use std.os.net.context as netctx
use std.core
use std.core.dict_mod as _d
use std.core.str
use std.parse.data.json as jsonlib

fn _requests_empty_response(any: url, any: method, str: transport="requests", str: error=""): dict {
   mut out = _d.dict(12)
   out["ok"] = false
   out["status"] = 0
   out["protocol"] = ""
   out["reason"] = ""
   out["headers"] = _d.dict(4)
   out["raw_headers"] = ""
   out["body"] = ""
   out["raw"] = ""
   out["method"] = upper(to_str(method))
   out["url"] = is_str(url) ? url : ""
   out["transport"] = transport
   out["error"] = error
   out
}

fn _has(any: d, str: key): bool { is_dict(d) && d.get(key, nil) != nil }

fn _truthy(any: v, bool: fallback=false): bool {
   if(v == nil){ return fallback }
   if(is_int(v)){ return v != 0 }
   if(is_str(v)){
      def s = lower(strip(v))
      return !(s == "" || s == "0" || s == "false" || s == "off" || s == "no")
   }
   v ? true : false
}

fn _int_opt(any: d, str: key, int: fallback): int {
   if(!_has(d, key)){ return fallback }
   def v = d.get(key)
   is_int(v) ? v : atoi(to_str(v))
}

fn _log_enabled(any: opts, str: want): bool { netctx.log_enabled(opts, want) }

fn _req_log(any: opts, str: want, str: msg): int { netctx.log_line("http", want, msg, opts) }

fn _str_opt(any: d, str: key, str: fallback=""): str {
   if(!_has(d, key)){ return fallback }
   def v = d.get(key)
   is_str(v) ? v : to_str(v)
}

fn _header_name_eq(any: a, str: b): bool { is_str(a) && lower(strip(a)) == lower(strip(b)) }

fn _header_has(any: headers, str: name): bool {
   if(!is_dict(headers)){ return false }
   def items = _d.dict_items(headers)
   mut i = 0
   while(i < items.len){
      if(_header_name_eq(items.get(i).get(0), name)){ return true }
      i += 1
   }
   false
}

fn _header_get(any: headers, str: name, str: fallback=""): str {
   if(!is_dict(headers)){ return fallback }
   def items = _d.dict_items(headers)
   mut i = 0
   while(i < items.len){
      def pair = items.get(i)
      if(_header_name_eq(pair.get(0), name)){ return to_str(pair.get(1)) }
      i += 1
   }
   fallback
}

fn _header_set_default(dict: headers, str: name, any: value): dict {
   if(!_header_has(headers, name)){ headers[name] = is_str(value) ? value : to_str(value) }
   headers
}

fn _merge_headers(any: a, any: b): dict {
   mut out = _d.dict(16)
   if(is_dict(a)){ out = _d.dict_merge(out, a) }
   if(is_dict(b)){ out = _d.dict_merge(out, b) }
   out
}

fn _hex2(int: c): str {
   def h = "0123456789ABCDEF"
   slice(h, c / 16, c / 16 + 1) + slice(h, c % 16, c % 16 + 1)
}

fn _quote(any: value, bool: plus=false): str {
   def s = is_str(value) ? value : to_str(value)
   mut b = Builder(s.len + 8)
   mut i = 0
   while(i < s.len){
      def c = load8(s, i)
      def ok = (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 45 || c == 46 || c == 95 || c == 126
      if(ok){ b = builder_append(b, chr(c)) }
      elif(plus && c == 32){ b = builder_append(b, "+") }
      else { b = builder_append(b, "%" + _hex2(c)) }
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _append_pair(any: builder, any: key, any: value, bool: plus): any {
   if(builder[1] > 0){ builder = builder_append(builder, "&") }
   builder = builder_append(builder, _quote(key, plus) + "=" + _quote(value, plus))
   builder
}

fn _encode_pairs(any: params, bool: plus=true): str {
   if(params == nil || params == 0){ return "" }
   if(is_str(params)){
      def p = strip(params)
      startswith(p, "?") ? slice(p, 1, p.len) : p
   } elif(is_dict(params)){
      mut b = Builder(128)
      def items = _d.dict_items(params)
      mut i = 0
      while(i < items.len){
         def pair = items.get(i)
         def k = pair.get(0)
         def v = pair.get(1)
         if(is_list(v) || is_tuple(v)){
            mut j = 0
            while(j < v.len){
               b = _append_pair(b, k, v.get(j), plus)
               j += 1
            }
         } else {
            b = _append_pair(b, k, v, plus)
         }
         i += 1
      }
      def out = builder_to_str(b)
      builder_free(b)
      out
   } elif(is_list(params) || is_tuple(params)){
      mut b = Builder(128)
      mut i = 0
      while(i < params.len){
         def p = params.get(i)
         if((is_list(p) || is_tuple(p)) && p.len >= 2){ b = _append_pair(b, p.get(0), p.get(1), plus) }
         i += 1
      }
      def out = builder_to_str(b)
      builder_free(b)
      out
   } else {
      ""
   }
}

fn _append_query(str: url, any: params): str {
   def q = _encode_pairs(params, true)
   if(q.len == 0){ return url }
   def hash_i = find(url, "#")
   mut base = url
   mut frag = ""
   if(hash_i >= 0){
      base = slice(url, 0, hash_i)
      frag = slice(url, hash_i, url.len)
   }
   def sep = find(base, "?") >= 0 ? (endswith(base, "?") || endswith(base, "&") ? "" : "&") : "?"
   base + sep + q + frag
}

fn _cookie_string(any: cookies): str {
   if(cookies == nil || cookies == 0){ return "" }
   if(is_str(cookies)){ return cookies }
   if(!is_dict(cookies)){ return "" }
   mut b = Builder(128)
   def items = _d.dict_items(cookies)
   mut i = 0
   while(i < items.len){
      def pair = items.get(i)
      if(b[1] > 0){ b = builder_append(b, "; ") }
      b = builder_append(b, _quote(pair.get(0), false) + "=" + _quote(pair.get(1), false))
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _b64(str: s): str {
   def alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
   mut b = Builder(((s.len + 2) / 3) * 4 + 4)
   mut i = 0
   while(i < s.len){
      def b0 = load8(s, i)
      def have1 = i + 1 < s.len
      def have2 = i + 2 < s.len
      def b1 = have1 ? load8(s, i + 1) : 0
      def b2 = have2 ? load8(s, i + 2) : 0
      def n = b0 * 65536 + b1 * 256 + b2
      b = builder_append(b, slice(alphabet, (n / 262144) % 64, (n / 262144) % 64 + 1))
      b = builder_append(b, slice(alphabet, (n / 4096) % 64, (n / 4096) % 64 + 1))
      b = builder_append(b, have1 ? slice(alphabet, (n / 64) % 64, (n / 64) % 64 + 1) : "=")
      b = builder_append(b, have2 ? slice(alphabet, n % 64, n % 64 + 1) : "=")
      i += 3
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _multipart_body(any: fields, any: files, str: boundary): str {
   mut b = Builder(1024)
   if(is_dict(fields)){
      def items = _d.dict_items(fields)
      mut i = 0
      while(i < items.len){
         def pair = items.get(i)
         b = builder_append(b, "--" + boundary + "\r\n")
         b = builder_append(b, "Content-Disposition: form-data; name=\"" + to_str(pair.get(0)) + "\"\r\n\r\n")
         b = builder_append(b, to_str(pair.get(1)) + "\r\n")
         i += 1
      }
   }
   if(is_dict(files)){
      def items = _d.dict_items(files)
      mut i = 0
      while(i < items.len){
         def pair = items.get(i)
         def name = to_str(pair.get(0))
         def fv = pair.get(1)
         mut filename = name
         mut ctype = "application/octet-stream"
         mut content = ""
         if(is_dict(fv)){
            filename = _str_opt(fv, "filename", filename)
            ctype = _str_opt(fv, "content_type", _str_opt(fv, "type", ctype))
            content = _str_opt(fv, "content", "")
         } else {
            content = is_str(fv) ? fv : to_str(fv)
         }
         b = builder_append(b, "--" + boundary + "\r\n")
         b = builder_append(b, "Content-Disposition: form-data; name=\"" + name + "\"; filename=\"" + filename + "\"\r\n")
         b = builder_append(b, "Content-Type: " + ctype + "\r\n\r\n")
         b = builder_append(b, content + "\r\n")
         i += 1
      }
   }
   b = builder_append(b, "--" + boundary + "--\r\n")
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _apply_auth(dict: spec, dict: headers, dict: curl_opts): int {
   def bearer = _str_opt(spec, "bearer", _str_opt(spec, "token", ""))
   if(bearer.len > 0){
      _header_set_default(headers, "Authorization", "Bearer " + bearer)
      curl_opts["auth_type"] = "bearer"
      return 0
   }
   def auth = spec.get("auth", nil)
   if(auth == nil){ return 0 }
   mut typ = "basic"
   mut user = ""
   mut password = ""
   if(is_dict(auth)){
      typ = lower(_str_opt(auth, "type", typ))
      user = _str_opt(auth, "username", _str_opt(auth, "user", ""))
      password = _str_opt(auth, "password", _str_opt(auth, "pass", ""))
      def token = _str_opt(auth, "token", "")
      if(token.len > 0){
         _header_set_default(headers, "Authorization", "Bearer " + token)
         curl_opts["auth_type"] = "bearer"
         return 0
      }
   } elif(is_list(auth) || is_tuple(auth)){
      if(auth.len >= 1){ user = to_str(auth.get(0)) }
      if(auth.len >= 2){ password = to_str(auth.get(1)) }
   } elif(is_str(auth)){
      user = auth
      def c = find(auth, ":")
      if(c >= 0){
         user = slice(auth, 0, c)
         password = slice(auth, c + 1, auth.len)
      }
   }
   if(user.len > 0 || password.len > 0){
      curl_opts["username"] = user
      curl_opts["password"] = password
      curl_opts["auth_type"] = typ
      if(typ == "basic"){ _header_set_default(headers, "Authorization", "Basic " + _b64(user + ":" + password)) }
   }
   0
}

fn prepare_request(any: method, any: url=0, any: data=0, any: headers=0, any: options=0): dict {
   "Normalizes request options into `{method,url,body,headers,options}` without sending it."
   def ctx = netctx.context()
   mut spec = _d.dict(32)
   if(is_dict(method) && (url == nil || url == 0)){
      spec = _d.dict_clone(method)
      if(is_dict(options)){ spec = _d.dict_merge(spec, options) }
   } else {
      if(is_dict(options)){ spec = _d.dict_clone(options) }
      spec["method"] = method
      spec["url"] = url
      if(!(data == nil || data == 0)){ spec["data"] = data }
      if(is_dict(headers)){ spec["headers"] = headers }
   }
   mut req_url = _str_opt(spec, "url", _str_opt(spec, "uri", ""))
   req_url = _append_query(req_url, spec.get("params", spec.get("query", 0)))
   mut out_headers = _merge_headers(spec.get("headers", 0), spec.get("extra_headers", 0))
   if(_has(spec, "user_agent")){ _header_set_default(out_headers, "User-Agent", spec.get("user_agent")) }
   if(_has(spec, "accept")){ _header_set_default(out_headers, "Accept", spec.get("accept")) }
   if(_has(spec, "referer")){ _header_set_default(out_headers, "Referer", spec.get("referer")) }
   if(_has(spec, "referrer")){ _header_set_default(out_headers, "Referer", spec.get("referrer")) }
   mut body = ""
   mut has_body = false
   if(_has(spec, "json")){
      body = jsonlib.json_encode(spec.get("json"))
      has_body = true
      _header_set_default(out_headers, "Content-Type", "application/json")
   } elif(_has(spec, "form")){
      body = _encode_pairs(spec.get("form"), true)
      has_body = true
      _header_set_default(out_headers, "Content-Type", "application/x-www-form-urlencoded")
   } elif(_has(spec, "multipart") || _has(spec, "files")){
      def boundary = "----nytrix-" + to_str(__getpid()) + "-" + to_str(body.len + req_url.len)
      body = _multipart_body(spec.get("multipart", spec.get("fields", 0)), spec.get("files", 0), boundary)
      has_body = true
      _header_set_default(out_headers, "Content-Type", "multipart/form-data; boundary=" + boundary)
   } elif(_has(spec, "body")){
      body = to_str(spec.get("body"))
      has_body = true
   } elif(_has(spec, "data")){
      def d = spec.get("data")
      if(is_dict(d) || is_list(d) || is_tuple(d)){
         body = _encode_pairs(d, true)
         _header_set_default(out_headers, "Content-Type", "application/x-www-form-urlencoded")
      } else {
         body = is_str(d) ? d : to_str(d)
      }
      has_body = true
   }
   if(_has(spec, "content_type")){ _header_set_default(out_headers, "Content-Type", spec.get("content_type")) }
   def cookie = _cookie_string(spec.get("cookies", spec.get("cookie", 0)))
   if(cookie.len > 0){
      _header_set_default(out_headers, "Cookie", cookie)
      spec["cookie"] = cookie
   }
   mut curl_opts = _d.dict_clone(spec)
   _apply_auth(spec, out_headers, curl_opts)
   if(!_has(curl_opts, "log_level")){ curl_opts["log_level"] = ctx.get("log_level", "quiet") }
   if(!_has(curl_opts, "color")){ curl_opts["color"] = ctx.get("color", true) }
   if(!_has(curl_opts, "curl_log")){ curl_opts["curl_log"] = false }
   mut m = upper(strip(_str_opt(spec, "method", "")))
   if(m.len == 0){ m = has_body ? "POST" : "GET" }
   def ua = _header_get(out_headers, "User-Agent", _str_opt(spec, "user_agent", "nytrix/1.0"))
   def ctx_timeout = ctx.get("timeout_ms", -1)
   def timeout_fallback = ctx_timeout > 0 ? max(1, ctx_timeout / 1000) : 60
   def timeout_sec = _int_opt(spec, "timeout_sec", _int_opt(spec, "timeout", timeout_fallback))
   mut parsed = _truthy(spec.get("parsed", true), true)
   if(_truthy(spec.get("raw", false), false)){ parsed = false }
   return {
      "method": m, "url": req_url, "body": has_body ? body : 0,
      "headers": out_headers, "options": curl_opts, "timeout_sec": timeout_sec,
      "user_agent": ua, "parsed": parsed, "has_body": has_body
   }
}

fn requests_prepare(any: method, any: url=0, any: data=0, any: headers=0, any: options=0): dict {
   prepare_request(method, url, data, headers, options)
}

fn _requests_is_https(any: url): bool {
   is_str(url) && startswith(lower(strip(url)), "https://")
}

fn _requests_is_http_url(any: url): bool {
   if(!is_str(url)){ return false }
   def u = lower(strip(url))
   startswith(u, "http://") || startswith(u, "https://")
}

fn _finish(dict: r, dict: p, str: transport): dict {
   r["method"] = p.get("method", "GET")
   r["url"] = p.get("url", "")
   if(r.get("transport", "").len == 0){ r["transport"] = transport }
   r["request"] = {"method": p.get("method", "GET"), "url": p.get("url", ""), "headers": p.get("headers", 0), "body_len": p.get("has_body", false) ? to_str(p.get("body", "")).len : 0}
   r
}

fn _send_prepared(dict: p): any {
   def method = p.get("method", "GET")
   def url = p.get("url", "")
   def body = p.get("body", 0)
   def headers = p.get("headers", 0)
   def opts = p.get("options", 0)
   def parsed = p.get("parsed", true)
   def prefer_curl = _truthy(opts.get("curl", true), true) && lower(to_str(opts.get("transport", ""))) != "socket"
   _req_log(opts, "info", method + " " + url)
   if(_log_enabled(opts, "debug")){
      def blen = p.get("has_body", false) ? to_str(body).len : 0
      _req_log(opts, "debug", "headers=" + to_str(headers.len) + " body=" + to_str(blen) + "B timeout=" + to_str(p.get("timeout_sec", 60)) + "s transport=" + (prefer_curl ? "curl" : "socket"))
   }
   if(_requests_is_http_url(url) && prefer_curl && curl.curl_available()){
      if(parsed){
         def r = _finish(curl.curl_request(method, url, body, headers, p.get("timeout_sec", 60), p.get("user_agent", "nytrix/1.0"), opts), p, "curl")
         _req_log(opts, r.get("ok", false) ? "info" : "error", method + " " + url + " -> " + to_str(r.get("status", 0)) + " " + r.get("reason", "") + " " + to_str(r.get("body", "").len) + "B")
         return r
      }
      def raw = curl.curl_request_raw(method, url, body, headers, p.get("timeout_sec", 60), p.get("user_agent", "nytrix/1.0"), opts)
      _req_log(opts, "info", method + " " + url + " -> raw " + to_str(raw.len) + "B")
      return raw
   }
   if(_requests_is_https(url)){
      return parsed ? _requests_empty_response(url, method, "curl-unavailable", "HTTPS requires libcurl") : ""
   }
   if(parsed){
      def r = _finish(http_request_url_parsed(method, url, body, headers), p, "socket")
      _req_log(opts, r.get("ok", false) ? "info" : "error", method + " " + url + " -> " + to_str(r.get("status", 0)) + " " + r.get("reason", "") + " " + to_str(r.get("body", "").len) + "B")
      return r
   }
   def raw = http_request_url(method, url, body, headers)
   _req_log(opts, "info", method + " " + url + " -> raw " + to_str(raw.len) + "B")
   raw
}

fn requests_request(any: method, any: url=0, any: data=0, any: headers=0, any: options=0): dict {
   "Performs an HTTP request and returns a parsed response. `method` may also be a full options dict."
   def p = prepare_request(method, url, data, headers, options)
   p["parsed"] = true
   _send_prepared(p)
}

fn requests_request_raw(any: method, any: url=0, any: data=0, any: headers=0, any: options=0): str {
   "Performs an HTTP request and returns the final raw response string."
   def p = prepare_request(method, url, data, headers, options)
   p["parsed"] = false
   _send_prepared(p)
}

fn request(any: method, any: url=0, any: data=0, any: headers=0, any: options=0): dict {
   "Alias for `requests_request`; accepts positional args or one options dict."
   requests_request(method, url, data, headers, options)
}

fn request_raw(any: method, any: url=0, any: data=0, any: headers=0, any: options=0): str {
   "Alias for `requests_request_raw`; accepts positional args or one options dict."
   requests_request_raw(method, url, data, headers, options)
}

fn requests(any: url, any: method="GET", any: data=0, any: headers=0, bool: parsed=true, any: options=0): any {
   "Unified requests entrypoint. Pass either `(url, method, data, headers, parsed, options)` or one dict."
   if(is_dict(url)){
      def p = prepare_request(url)
      p["parsed"] = _truthy(url.get("parsed", parsed), parsed) && !_truthy(url.get("raw", false), false)
      return _send_prepared(p)
   }
   def p = prepare_request(method, url, data, headers, options)
   p["parsed"] = parsed
   _send_prepared(p)
}

fn _requests_host(any: method, any: host, int: port, any: path, any: headers=0, any: data=0, bool: parsed=false): any {
   if(parsed){ return http_request(method, host, port, path, data, headers) }
   http_request_raw(method, host, port, path, data, headers)
}

fn requests_get(any: url): str {
   "Performs a raw HTTP GET request by URL."
   requests_request_raw("GET", url)
}

fn requests_get_host(any: host, int: port, any: path): str {
   "Performs a raw HTTP GET request using host/port/path."
   _requests_host("GET", host, port, path)
}

fn requests_get_parsed(any: url, any: headers=0, any: options=0): dict {
   "Performs HTTP GET request by URL and returns parsed response."
   requests_request("GET", url, 0, headers, options)
}

fn requests_get_host_parsed(any: host, int: port, any: path, any: headers=0): dict {
   "Performs HTTP GET request using host/port/path and returns parsed response."
   _requests_host("GET", host, port, path, headers, 0, true)
}

fn requests_post_raw(any: url, any: data, any: headers=0, any: options=0): str {
   "Performs a raw HTTP POST request by URL."
   requests_request_raw("POST", url, data, headers, options)
}

fn requests_put_raw(any: url, any: data, any: headers=0, any: options=0): str {
   "Performs a raw HTTP PUT request by URL."
   requests_request_raw("PUT", url, data, headers, options)
}

fn requests_delete_raw(any: url, any: headers=0, any: options=0): str {
   "Performs a raw HTTP DELETE request by URL."
   requests_request_raw("DELETE", url, 0, headers, options)
}

fn requests_post(any: url, any: data, any: headers=0, any: options=0): dict {
   "Performs an HTTP POST request by URL and returns parsed response."
   requests_request("POST", url, data, headers, options)
}

fn requests_put(any: url, any: data, any: headers=0, any: options=0): dict {
   "Performs an HTTP PUT request by URL and returns parsed response."
   requests_request("PUT", url, data, headers, options)
}

fn requests_delete(any: url, any: headers=0, any: options=0): dict {
   "Performs an HTTP DELETE request by URL and returns parsed response."
   requests_request("DELETE", url, 0, headers, options)
}

fn requests_parse_url(any: url): list {
   "Parses URL string into `[host, port, target]`."
   http_parse_url(url)
}

fn requests_parse_url_ex(any: url): dict {
   "Parses URL string into detailed response map."
   http_parse_url_ex(url)
}

fn requests_parse_query(any: query): dict {
   "Parses URL query into a dictionary."
   http_parse_query(query)
}

fn requests_parse_response(any: raw): dict {
   "Parses raw HTTP response into structured map."
   http_parse_response(raw)
}

if(comptime{ __main() }){
   def p = prepare_request({
         "url": "http://example.test/path",
         "method": "POST",
         "params": {"q": "ny request", "page": 2},
         "json": {"ok": true, "n": 7},
         "headers": {"X-Test": "yes"},
         "cookies": {"sid": "abc 123"},
         "auth": ["u", "p"],
         "timeout": 5,
         "follow": false,
         "proxy": "http://127.0.0.1:9"
   })
   assert_eq(p.get("method", ""), "POST", "prepared method")
   assert(str_contains(p.get("url", ""), "q=ny+request"), "prepared query")
   assert_eq(_header_get(p.get("headers", 0), "content-type", ""), "application/json", "json content type")
   assert(str_contains(_header_get(p.get("headers", 0), "cookie", ""), "sid=abc%20123"), "cookie header")
   assert(startswith(_header_get(p.get("headers", 0), "authorization", ""), "Basic "), "basic auth header")
   assert_eq(p.get("timeout_sec", 0), 5, "timeout prepared")
   assert_eq(p.get("options", 0).get("follow", true), false, "curl options preserved")
   def f = prepare_request({"url": "http://example.test/upload", "form": {"a": "b c"}})
   assert_eq(f.get("body", ""), "a=b+c", "form body")
   print("requests self-test ok")
}
