;; Keywords: net socket curl http-client os
;; libcurl-backed HTTP client transport for requests that need curl features.
;;
;; This lives in Ny stdlib (not the C runtime) to keep the runtime smaller while
;; still allowing HTTPS + redirects when libcurl is present.
;; References:
;; - std.os.net
;; - std.os
module std.os.net.curl(curl_available, curl_request, curl_request_raw, curl_get, curl_fetch)
use std.core
use std.core.error
use std.core.dict_mod as _d
use std.core.str
use std.os (temp_dir, file_read, file_remove)
use std.os.clock (ticks)
use std.os.path as ospath
use std.os.net.context as netctx
use std.os.net.http as http
use std.os.ffi (dlsym, call0_ptr, call1, call1_ptr, call2_ptr, call3, cstr, cptr)

#linux {
   #link "libcurl.so"
   #include <curl/curl.h> as "curl_"
} #elif macos {
   #link "libcurl.dylib"
   #include <curl/curl.h> as "curl_"
} #elif windows {
   #link "libcurl"
   #include <curl/curl.h> as "curl_"
} #endif
extern "" {
   fn _fopen(ptr path, ptr mode) ptr as "fopen"
   fn _fclose(ptr fp) i32 as "fclose"
}

def _CURLOPT_URL            = 10002
def _CURLOPT_WRITEDATA      = 10001
def _CURLOPT_HEADERDATA     = 10029
def _CURLOPT_FOLLOWLOCATION = 52
def _CURLOPT_USERAGENT      = 10018
def _CURLOPT_FAILONERROR    = 45
def _CURLOPT_TIMEOUT        = 13
def _CURLOPT_TIMEOUT_MS     = 155
def _CURLOPT_CONNECTTIMEOUT = 78
def _CURLOPT_CONNECTTIMEOUT_MS = 156
def _CURLOPT_CUSTOMREQUEST  = 10036
def _CURLOPT_POSTFIELDS     = 10015
def _CURLOPT_POSTFIELDSIZE  = 60
def _CURLOPT_HTTPHEADER     = 10023
def _CURLOPT_MAXREDIRS      = 68
def _CURLOPT_COOKIE         = 10022
def _CURLOPT_COOKIEFILE     = 10031
def _CURLOPT_COOKIEJAR      = 10082
def _CURLOPT_PROXY          = 10004
def _CURLOPT_REFERER        = 10016
def _CURLOPT_AUTOREFERER    = 58
def _CURLOPT_ACCEPT_ENCODING = 10102
def _CURLOPT_USERPWD        = 10005
def _CURLOPT_USERNAME       = 10173
def _CURLOPT_PASSWORD       = 10174
def _CURLOPT_HTTPAUTH       = 107
def _CURLOPT_PROXYUSERPWD   = 10006
def _CURLOPT_PROXYUSERNAME  = 10175
def _CURLOPT_PROXYPASSWORD  = 10176
def _CURLOPT_PROXYAUTH      = 111
def _CURLOPT_SSL_VERIFYPEER = 64
def _CURLOPT_SSL_VERIFYHOST = 81
def _CURLOPT_CAINFO         = 10065
def _CURLOPT_SSLCERT        = 10025
def _CURLOPT_SSLKEY         = 10087
def _CURLOPT_SSLCERTTYPE    = 10086
def _CURLOPT_SSLKEYTYPE     = 10088
def _CURLOPT_HTTP_VERSION   = 84
def _CURLOPT_NOBODY         = 44
def _CURLOPT_HTTPGET        = 80
def _CURLOPT_POST           = 47
def _CURLOPT_POSTREDIR      = 161
def _CURLOPT_IPRESOLVE      = 113
def _CURLOPT_INTERFACE      = 10062
def _CURLOPT_LOCALPORT      = 139
def _CURLOPT_LOCALPORTRANGE = 140
def _CURLOPT_RANGE          = 10007
def _CURLOPT_RESUME_FROM    = 21
def _CURLOPT_MAXFILESIZE    = 114
def _CURLOPT_LOW_SPEED_LIMIT = 19
def _CURLOPT_LOW_SPEED_TIME = 20
def _CURLOPT_TCP_KEEPALIVE  = 213
def _CURLOPT_UNIX_SOCKET_PATH = 10231
def _CURLOPT_RESOLVE        = 10203
def _CURLOPT_DNS_SERVERS    = 10211
def _CURLOPT_VERBOSE        = 41
def _CURLOPT_NOSIGNAL       = 99
def _CURL_GLOBAL_ALL = 3
mut _curl = 0
mut _p_global_init = 0
mut _p_easy_init = 0
mut _p_easy_setopt = 0
mut _p_easy_perform = 0
mut _p_easy_cleanup = 0
mut _p_easy_strerror = 0
mut _p_slist_append = 0
mut _p_slist_free_all = 0
mut _curl_inited = false
mut _tmp_seq = 0

fn _load() bool {
   if _curl { return true }
   _p_global_init = dlsym(0, "curl_global_init")
   _p_easy_init = dlsym(0, "curl_easy_init")
   _p_easy_setopt = dlsym(0, "curl_easy_setopt")
   _p_easy_perform = dlsym(0, "curl_easy_perform")
   _p_easy_cleanup = dlsym(0, "curl_easy_cleanup")
   _p_easy_strerror = dlsym(0, "curl_easy_strerror")
   _p_slist_append = dlsym(0, "curl_slist_append")
   _p_slist_free_all = dlsym(0, "curl_slist_free_all")
   _curl = (_p_easy_init && _p_easy_setopt && _p_easy_perform && _p_easy_cleanup) ? 1 : 0
   _curl != 0
}

fn curl_available() bool { _load() }

fn _tmp_path(str tag="curl") str {
   def t = temp_dir()
   def pid = __getpid()
   _tmp_seq += 1
   def stamp = ticks()
   ospath.join(t, "nycurl_" + tag + "_" + to_str(pid) + "_" + to_str(stamp) + "_" + to_str(_tmp_seq) + ".tmp")
}

fn _timeout_sec(any v) int {
   if !is_int(v) { return 60 }
   if v < 1 { return 1 }
   if v > 3600 { return 3600 }
   v
}

fn _has_ctl(any s) bool {
   if !is_str(s) { return true }
   mut i = 0
   def n = s.len
   while i < n {
      def c = load8(s, i)
      if c == 0 || c == 10 || c == 13 { return true }
      i += 1
   }
   false
}

fn _ua(any v) str {
   if !is_str(v) || v.len == 0 || _has_ctl(v) { return "nytrix/1.0" }
   v
}

fn _method(any v) str {
   mut m = upper(strip(to_str(v)))
   if m.len == 0 || _has_ctl(m) { return "GET" }
   m
}

fn _opt_bool(any options, str name, bool fallback) bool {
   if !is_dict(options) { return fallback }
   def v = options.get(name, fallback)
   if is_int(v) { return v != 0 }
   if is_str(v) {
      def s = lower(strip(v))
      return !(s == "" || s == "0" || s == "false" || s == "off" || s == "no")
   }
   v ? true : false
}

fn _opt_has(any options, str name) bool {
   is_dict(options) && options.get(name, nil) != nil
}

fn _opt_int(any options, str name, int fallback, int min_v, int max_v) int {
   if !is_dict(options) { return fallback }
   def v = options.get(name, fallback)
   mut out = is_int(v) ? v : int(v)
   if out < min_v { out = min_v }
   if out > max_v { out = max_v }
   out
}

fn _opt_int_any(any options, list names, int fallback, int min_v, int max_v) int {
   mut i = 0
   while i < names.len {
      def k = to_str(names.get(i))
      if _opt_has(options, k) { return _opt_int(options, k, fallback, min_v, max_v) }
      i += 1
   }
   fallback
}

fn _opt_str(any options, str name, str fallback="") str {
   if !is_dict(options) { return fallback }
   def v = options.get(name, fallback)
   if !is_str(v) || _has_ctl(v) { return fallback }
   v
}

fn _opt_str_any(any options, list names, str fallback="") str {
   mut i = 0
   while i < names.len {
      def k = to_str(names.get(i))
      if _opt_has(options, k) { return _opt_str(options, k, fallback) }
      i += 1
   }
   fallback
}

fn _curl_error(int code) str {
   if code == 0 { return "" }
   if _p_easy_strerror != 0 {
      def p = call1_ptr(_p_easy_strerror, code)
      if p != 0 { return cstr_to_str(p) }
   }
   "curl error " + to_str(code)
}

fn _curl_auth_mask(any v, int fallback=1) int {
   if is_int(v) { return v }
   def s = lower(strip(to_str(v)))
   case s {
      "", "basic" -> 1
      "digest" -> 2
      "negotiate", "gssapi", "spnego" -> 4
      "ntlm" -> 8
      "digest-ie", "digest_ie" -> 16
      "bearer" -> 64
      "any" -> 4294967279
      "anysafe", "safe" -> 4294967278
      _ -> fallback
   }
}

fn _curl_http_version(any v) int {
   if is_int(v) { return v }
   def s = lower(strip(to_str(v)))
   case s {
      "", "auto", "none" -> 0
      "1", "1.0", "http/1.0" -> 1
      "1.1", "http/1.1" -> 2
      "2", "2.0", "h2", "http/2" -> 3
      "2tls", "h2tls" -> 4
      "2-prior", "h2c", "prior" -> 5
      "3", "h3", "http/3" -> 30
      "3only", "h3only" -> 31
      _ -> 0
   }
}

fn _curl_ip_resolve(any v) int {
   if is_int(v) { return v }
   def s = lower(strip(to_str(v)))
   case s {
      "4", "v4", "ipv4" -> 1
      "6", "v6", "ipv6" -> 2
      _ -> 0
   }
}

fn _curl_postredir(any v) int {
   if is_int(v) { return v }
   if v ? false : true { return 0 }
   def s = lower(strip(to_str(v)))
   case s {
      "all", "true", "1", "yes", "on" -> 7
      "301" -> 1
      "302" -> 2
      "303" -> 4
      _ -> 0
   }
}

fn _log_enabled(any options, str want) bool {
   _opt_bool(options, "curl_log", true) && netctx.log_enabled(options, want)
}

fn _curl_log(any options, str want, str msg) int {
   if !_opt_bool(options, "curl_log", true) { return 0 }
   netctx.log_line("curl", want, msg, options)
}

fn _headers_slist(any headers) any {
   if _p_slist_append == 0 { return 0 }
   if is_list(headers) || is_tuple(headers) {
      mut sl0 = 0
      mut j = 0
      while j < headers.len {
         def line = to_str(headers.get(j))
         if line.len > 0 && !_has_ctl(line) { sl0 = call2_ptr(_p_slist_append, sl0, cstr(line)) }
         j += 1
      }
      return sl0
   }
   if !is_dict(headers) { return 0 }
   mut sl = 0
   def items = _d.dict_items(headers)
   mut i = 0
   while i < items.len {
      def pair = items.get(i)
      def k = pair.get(0)
      def v = pair.get(1)
      if is_str(k) && strip(k).len > 0 && !_has_ctl(k) {
         def vv = is_str(v) ? v : to_str(v)
         if !_has_ctl(vv) { sl = call2_ptr(_p_slist_append, sl, cstr(strip(k) + ": " + vv)) }
      }
      i += 1
   }
   sl
}

fn _string_slist(any values) any {
   if _p_slist_append == 0 || !(is_list(values) || is_tuple(values)) { return 0 }
   mut sl = 0
   mut i = 0
   while i < values.len {
      def v = to_str(values.get(i))
      if v.len > 0 && !_has_ctl(v) { sl = call2_ptr(_p_slist_append, sl, cstr(v)) }
      i += 1
   }
   sl
}

fn _count_header_blocks(str headers) int {
   mut count = 0
   if startswith(headers, "HTTP/") { count += 1 }
   mut pos = 0
   while true {
      def cr = find_from(headers, "\r\nHTTP/", pos)
      def lf = find_from(headers, "\nHTTP/", pos)
      mut hit = -1
      if cr >= 0 && (lf < 0 || cr <= lf) { hit = cr + 2 }
      elif lf >= 0 { hit = lf + 1 }
      if hit < 0 { break }
      count += 1
      pos = hit + 5
   }
   count
}

fn _last_headers(str headers) str {
   if headers.len == 0 { return "" }
   mut start = 0
   def cr = find_last(headers, "\r\nHTTP/")
   def lf = find_last(headers, "\nHTTP/")
   if cr >= 0 && cr >= lf { start = cr + 2 }
   elif lf >= 0 { start = lf + 1 }
   strip(slice(headers, start, headers.len))
}

fn _curl_result(str headers, str body, str method, any url, int curl_code) dict {
   def last_head = _last_headers(headers)
   def raw = last_head.len > 0 ? (last_head + "\r\n\r\n" + body) : body
   mut out = http.http_parse_response(raw)
   out = out.set("body", body)
   out = out.set("raw", raw)
   out = out.set("raw_all_headers", headers)
   out = out.set("method", method)
   out = out.set("url", is_str(url) ? url : "")
   out = out.set("transport", "curl")
   out = out.set("tls", is_str(url) && startswith(lower(url), "https://"))
   out = out.set("curl_code", curl_code)
   out = out.set("error", _curl_error(curl_code))
   out = out.set("redirects", max(0, _count_header_blocks(headers) - 1))
   def status = out.get("status", 0)
   out = out.set("ok", curl_code == 0 && status >= 200 && status < 300)
   out
}

fn curl_request(any method, any url, any data=0, any headers=0, any timeout_sec=60, any user_agent="nytrix/1.0", any options=0) dict {
   "Performs an HTTP/HTTPS request via libcurl and returns parsed status, headers, and body."
   def m = _method(method)
   _curl_log(options, "info", m + " " + (is_str(url) ? url : to_str(url)))
   if !_load() { return _curl_result("", "", m, url, 1) }
   if !_curl_inited && _p_global_init {
      call1(_p_global_init, _CURL_GLOBAL_ALL)
      _curl_inited = true
   }
   if !is_str(url) || url.len == 0 || _has_ctl(url) { return _curl_result("", "", m, url, 1) }
   def low = lower(url)
   if !(startswith(low, "http://") || startswith(low, "https://")) { return _curl_result("", "", m, url, 1) }
   def timeout_v = _timeout_sec(timeout_sec)
   def ua_v = _ua(user_agent)
   def body_path = _tmp_path("body")
   def header_path = _tmp_path("headers")
   if !is_str(body_path) || body_path.len == 0 || !is_str(header_path) || header_path.len == 0 { return _curl_result("", "", m, url, 1) }
   mut remove_tmp = true
   mut body_fp = 0
   mut header_fp = 0
   mut curl = 0
   mut slist = 0
   mut resolve_list = 0
   defer {
      if curl != 0 { call1(_p_easy_cleanup, curl) }
      if slist != 0 && _p_slist_free_all != 0 { call1(_p_slist_free_all, slist) }
      if resolve_list != 0 && _p_slist_free_all != 0 { call1(_p_slist_free_all, resolve_list) }
      if body_fp != 0 { _fclose(body_fp) }
      if header_fp != 0 { _fclose(header_fp) }
      if remove_tmp {
         match file_remove(body_path) { ok(ignoredok) -> { ignoredok } err(ignorederr) -> { ignorederr } }
         match file_remove(header_path) { ok(ignoredok) -> { ignoredok } err(ignorederr) -> { ignorederr } }
      }
   }
   body_fp = _fopen(cptr(body_path), cptr("wb"))
   header_fp = _fopen(cptr(header_path), cptr("wb"))
   if body_fp == 0 || header_fp == 0 { return _curl_result("", "", m, url, 1) }
   curl = call0_ptr(_p_easy_init)
   if curl == 0 { return _curl_result("", "", m, url, 1) }
   mut body = ""
   def has_body = !(data == nil || data == 0)
   if has_body { body = is_str(data) ? data : to_str(data) }
   def body_c = has_body ? cstr(body) : 0
   slist = _headers_slist(headers)
   def ok_url  = call3(_p_easy_setopt, curl, _CURLOPT_URL, cstr(url)) == 0
   def ok_wr   = call3(_p_easy_setopt, curl, _CURLOPT_WRITEDATA, body_fp) == 0
   def ok_hdr  = call3(_p_easy_setopt, curl, _CURLOPT_HEADERDATA, header_fp) == 0
   def ok_meth = call3(_p_easy_setopt, curl, _CURLOPT_CUSTOMREQUEST, cstr(m)) == 0
   def ok_foll = call3(_p_easy_setopt, curl, _CURLOPT_FOLLOWLOCATION, _opt_bool(options, "follow", true) ? 1 : 0) == 0
   def ok_maxr = call3(_p_easy_setopt, curl, _CURLOPT_MAXREDIRS, _opt_int(options, "max_redirects", 20, 0, 100)) == 0
   def ok_fail = call3(_p_easy_setopt, curl, _CURLOPT_FAILONERROR, _opt_bool(options, "fail_on_error", false) ? 1 : 0) == 0
   def ok_to   = call3(_p_easy_setopt, curl, _CURLOPT_TIMEOUT, timeout_v) == 0
   def ok_ua   = call3(_p_easy_setopt, curl, _CURLOPT_USERAGENT, cstr(ua_v)) == 0
   def ok_sig  = call3(_p_easy_setopt, curl, _CURLOPT_NOSIGNAL, 1) == 0
   mut ok_extra = true
   def timeout_ms = _opt_int_any(options, ["timeout_ms", "read_timeout_ms"], 0, 0, 2147483647)
   if timeout_ms > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_TIMEOUT_MS, timeout_ms) == 0 }
   def connect_timeout = _opt_int_any(options, ["connect_timeout", "connect_timeout_sec"], 0, 0, 3600)
   if connect_timeout > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_CONNECTTIMEOUT, connect_timeout) == 0 }
   def connect_timeout_ms = _opt_int(options, "connect_timeout_ms", 0, 0, 2147483647)
   if connect_timeout_ms > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms) == 0 }
   if m == "HEAD" || _opt_bool(options, "nobody", false) { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_NOBODY, 1) == 0 }
   if m == "GET" && _opt_bool(options, "force_get", false) { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_HTTPGET, 1) == 0 }
   if m == "POST" && _opt_bool(options, "force_post", false) { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_POST, 1) == 0 }
   def referer = _opt_str_any(options, ["referer", "referrer"], "")
   if referer.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_REFERER, cstr(referer)) == 0 }
   if _opt_bool(options, "auto_referer", false) { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_AUTOREFERER, 1) == 0 }
   if _opt_has(options, "accept_encoding") {
      ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_ACCEPT_ENCODING, cstr(_opt_str(options, "accept_encoding", ""))) == 0
   } elif _opt_bool(options, "decompress", false) {
      ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_ACCEPT_ENCODING, cstr("")) == 0
   }
   def userpwd = _opt_str(options, "userpwd", "")
   if userpwd.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_USERPWD, cstr(userpwd)) == 0 }
   def username = _opt_str_any(options, ["username", "user"], "")
   if username.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_USERNAME, cstr(username)) == 0 }
   def password = _opt_str(options, "password", "")
   if password.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_PASSWORD, cstr(password)) == 0 }
   if _opt_has(options, "auth") || _opt_has(options, "auth_type") || userpwd.len > 0 || username.len > 0 {
      ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_HTTPAUTH, _curl_auth_mask(options.get("auth_type", options.get("auth", "basic")), 1)) == 0
   }
   def proxy_userpwd = _opt_str(options, "proxy_userpwd", "")
   if proxy_userpwd.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_PROXYUSERPWD, cstr(proxy_userpwd)) == 0 }
   def proxy_username = _opt_str_any(options, ["proxy_username", "proxy_user"], "")
   if proxy_username.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_PROXYUSERNAME, cstr(proxy_username)) == 0 }
   def proxy_password = _opt_str(options, "proxy_password", "")
   if proxy_password.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_PROXYPASSWORD, cstr(proxy_password)) == 0 }
   if _opt_has(options, "proxy_auth") || proxy_userpwd.len > 0 || proxy_username.len > 0 {
      ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_PROXYAUTH, _curl_auth_mask(options.get("proxy_auth", "basic"), 1)) == 0
   }
   def verify = _opt_bool(options, "verify", true)
   ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_SSL_VERIFYPEER, verify ? 1 : 0) == 0
   ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_SSL_VERIFYHOST, verify ? 2 : 0) == 0
   def ca = _opt_str_any(options, ["ca", "ca_info", "cainfo"], "")
   if ca.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_CAINFO, cstr(ca)) == 0 }
   def cert = _opt_str_any(options, ["cert", "ssl_cert"], "")
   if cert.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_SSLCERT, cstr(cert)) == 0 }
   def key = _opt_str_any(options, ["key", "ssl_key"], "")
   if key.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_SSLKEY, cstr(key)) == 0 }
   def cert_type = _opt_str(options, "cert_type", "")
   if cert_type.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_SSLCERTTYPE, cstr(cert_type)) == 0 }
   def key_type = _opt_str(options, "key_type", "")
   if key_type.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_SSLKEYTYPE, cstr(key_type)) == 0 }
   if _opt_has(options, "http_version") {
      ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_HTTP_VERSION, _curl_http_version(options.get("http_version"))) == 0
   }
   if _opt_has(options, "ip_resolve") {
      ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_IPRESOLVE, _curl_ip_resolve(options.get("ip_resolve"))) == 0
   }
   def iface = _opt_str_any(options, ["interface", "bind_interface"], "")
   if iface.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_INTERFACE, cstr(iface)) == 0 }
   def local_port = _opt_int(options, "local_port", 0, 0, 65535)
   if local_port > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_LOCALPORT, local_port) == 0 }
   def local_range = _opt_int(options, "local_port_range", 0, 0, 65535)
   if local_range > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_LOCALPORTRANGE, local_range) == 0 }
   def range_v = _opt_str(options, "range", "")
   if range_v.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_RANGE, cstr(range_v)) == 0 }
   def resume_from = _opt_int(options, "resume_from", -1, -1, 2147483647)
   if resume_from >= 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_RESUME_FROM, resume_from) == 0 }
   def max_filesize = _opt_int(options, "max_filesize", -1, -1, 2147483647)
   if max_filesize >= 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_MAXFILESIZE, max_filesize) == 0 }
   def low_speed_limit = _opt_int(options, "low_speed_limit", 0, 0, 2147483647)
   if low_speed_limit > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_LOW_SPEED_LIMIT, low_speed_limit) == 0 }
   def low_speed_time = _opt_int(options, "low_speed_time", 0, 0, 2147483647)
   if low_speed_time > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_LOW_SPEED_TIME, low_speed_time) == 0 }
   if _opt_bool(options, "tcp_keepalive", false) { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_TCP_KEEPALIVE, 1) == 0 }
   def unix_socket = _opt_str_any(options, ["unix_socket", "unix_socket_path"], "")
   if unix_socket.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_UNIX_SOCKET_PATH, cstr(unix_socket)) == 0 }
   if _opt_has(options, "resolve") {
      resolve_list = _string_slist(options.get("resolve"))
      if resolve_list != 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_RESOLVE, resolve_list) == 0 }
   }
   def dns = _opt_str_any(options, ["dns_servers", "dns"], "")
   if dns.len > 0 { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_DNS_SERVERS, cstr(dns)) == 0 }
   if _opt_has(options, "post_redirects") {
      ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_POSTREDIR, _curl_postredir(options.get("post_redirects"))) == 0
   }
   if _opt_bool(options, "verbose", false) { ok_extra = ok_extra && call3(_p_easy_setopt, curl, _CURLOPT_VERBOSE, 1) == 0 }
   if _log_enabled(options, "debug") {
      _curl_log(options, "debug", "timeout=" + to_str(timeout_v) + "s connect_timeout_ms=" + to_str(connect_timeout_ms) + " follow=" + to_str(_opt_bool(options, "follow", true)) + " redirects=" + to_str(_opt_int(options, "max_redirects", 20, 0, 100)))
   }
   mut ok_body = true
   if has_body {
      ok_body = (call3(_p_easy_setopt, curl, _CURLOPT_POSTFIELDS, body_c) == 0) &&
      (call3(_p_easy_setopt, curl, _CURLOPT_POSTFIELDSIZE, body.len) == 0)
   }
   mut ok_head = true
   if slist != 0 { ok_head = call3(_p_easy_setopt, curl, _CURLOPT_HTTPHEADER, slist) == 0 }
   def cookie = _opt_str(options, "cookie", "")
   mut ok_cookie = true
   if cookie.len > 0 { ok_cookie = call3(_p_easy_setopt, curl, _CURLOPT_COOKIE, cstr(cookie)) == 0 }
   def cookie_file = _opt_str(options, "cookie_file", "")
   if cookie_file.len > 0 { ok_cookie = ok_cookie && call3(_p_easy_setopt, curl, _CURLOPT_COOKIEFILE, cstr(cookie_file)) == 0 }
   def cookie_jar = _opt_str(options, "cookie_jar", "")
   if cookie_jar.len > 0 { ok_cookie = ok_cookie && call3(_p_easy_setopt, curl, _CURLOPT_COOKIEJAR, cstr(cookie_jar)) == 0 }
   def proxy = _opt_str(options, "proxy", "")
   mut ok_proxy = true
   if proxy.len > 0 { ok_proxy = call3(_p_easy_setopt, curl, _CURLOPT_PROXY, cstr(proxy)) == 0 }
   if !(ok_url && ok_wr && ok_hdr && ok_meth && ok_foll && ok_maxr && ok_fail && ok_to && ok_ua && ok_sig && ok_extra && ok_body && ok_head && ok_cookie && ok_proxy) {
      return _curl_result("", "", m, url, 1)
   }
   def res = call1(_p_easy_perform, curl)
   _fclose(body_fp)
   body_fp = 0
   _fclose(header_fp)
   header_fp = 0
   def br = file_read(body_path)
   def hr = file_read(header_path)
   def out_body = is_ok(br) ? unwrap(br) : ""
   def out_headers = is_ok(hr) ? unwrap(hr) : ""
   if res != 0 {
      def er = _curl_result(out_headers, out_body, m, url, res)
      _curl_log(options, "error", m + " " + url + " -> curl " + to_str(res) + " " + er.get("error", ""))
      return er
   }
   def okr = _curl_result(out_headers, out_body, m, url, 0)
   _curl_log(options, okr.get("ok", false) ? "info" : "error", m + " " + url + " -> " + to_str(okr.get("status", 0)) + " " + okr.get("reason", "") + " " + to_str(out_body.len) + "B")
   okr
}

fn curl_request_raw(any method, any url, any data=0, any headers=0, any timeout_sec=60, any user_agent="nytrix/1.0", any options=0) str {
   "Performs a libcurl request and returns the final raw HTTP response."
   def r = curl_request(method, url, data, headers, timeout_sec, user_agent, options)
   if !is_dict(r) { return "" }
   r.get("raw", "")
}

fn curl_get(any url, any headers=0, any timeout_sec=60, any user_agent="nytrix/1.0", any options=0) dict {
   "Performs a libcurl GET request and returns parsed response metadata."
   curl_request("GET", url, 0, headers, timeout_sec, user_agent, options)
}

fn curl_fetch(any url, any timeout_sec=60, any user_agent="nytrix/1.0") any {
   "Fetches `url` via libcurl and returns the body string. Returns 0 on failure or non-2xx status."
   def r = curl_request("GET", url, 0, 0, timeout_sec, user_agent)
   if !is_dict(r) || !r.get("ok", false) { return 0 }
   r.get("body", "")
}
