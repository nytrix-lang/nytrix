;; Keywords: net requests
;; Net Requests module.

module std.net.requests (
   requests_get, requests_get_host,
   requests_get_parsed, requests_get_host_parsed,
   requests_post, requests_put, requests_delete,
   requests_post_raw, requests_put_raw, requests_delete_raw,
   requests_request, requests_request_raw,
   requests_parse_url, requests_parse_url_ex, requests_parse_query, requests_parse_response
)
use std.net.http *

fn _requests_raw(method, url, data=0, headers=0){
   "Internal helper."
   http_request_url(method, url, data, headers)
}

fn _requests_parsed(method, url, data=0, headers=0){
   "Internal helper."
   http_request_url_parsed(method, url, data, headers)
}

fn requests_get(url){
   "Performs a raw HTTP GET request by URL."
   _requests_raw("GET", url)
}

fn requests_get_host(host, port, path){
   "Performs a raw HTTP GET request using host/port/path."
   return http_get(host, port, path)
}

fn requests_get_parsed(url, headers=0){
   "Performs HTTP GET request by URL and returns parsed response."
   http_get_url_parsed(url, headers)
}

fn requests_get_host_parsed(host, port, path, headers=0){
   "Performs HTTP GET request using host/port/path and returns parsed response."
   http_get_parsed(host, port, path, headers)
}

fn requests_request_raw(method, url, data=0, headers=0){
   "Performs a raw HTTP request by URL."
   _requests_raw(method, url, data, headers)
}

fn requests_request(method, url, data=0, headers=0){
   "Performs an HTTP request by URL and returns parsed response."
   _requests_parsed(method, url, data, headers)
}

fn requests_post_raw(url, data, headers=0){
   "Performs a raw HTTP POST request by URL."
   _requests_raw("POST", url, data, headers)
}

fn requests_put_raw(url, data, headers=0){
   "Performs a raw HTTP PUT request by URL."
   _requests_raw("PUT", url, data, headers)
}

fn requests_delete_raw(url, headers=0){
   "Performs a raw HTTP DELETE request by URL."
   _requests_raw("DELETE", url, 0, headers)
}

fn requests_post(url, data, headers=0){
   "Performs an HTTP POST request by URL and returns parsed response."
   _requests_parsed("POST", url, data, headers)
}

fn requests_put(url, data, headers=0){
   "Performs an HTTP PUT request by URL and returns parsed response."
   _requests_parsed("PUT", url, data, headers)
}

fn requests_delete(url, headers=0){
   "Performs an HTTP DELETE request by URL and returns parsed response."
   _requests_parsed("DELETE", url, 0, headers)
}

fn requests_parse_url(url){
   "Parses URL string into `[host, port, target]`."
   http_parse_url(url)
}

fn requests_parse_url_ex(url){
   "Parses URL string into detailed response map."
   http_parse_url_ex(url)
}

fn requests_parse_query(query){
   "Parses URL query into a dictionary."
   http_parse_query(query)
}

fn requests_parse_response(raw){
   "Parses raw HTTP response into structured map."
   http_parse_response(raw)
}

if(comptime{__main()}){
    use std.net.requests *
    use std.core *
    use std.core.dict *
    use std.core.error *
    use std.str.io *

    print("Testing net.requests...")

    def url = "http://example.com:8080/foo/bar"
    def parts = requests_parse_url(url)
    assert((get(parts, 0) == "example.com"), "host")
    assert(get(parts, 1) == 8080, "port")
    assert((get(parts, 2) == "/foo/bar"), "path")

    def url2 = "example.org"
    def parts2 = requests_parse_url(url2)
    assert((get(parts2, 0) == "example.org"), "host no scheme")
    assert(get(parts2, 1) == 80, "default port")
    assert((get(parts2, 2) == "/"), "default path")

    def ex = requests_parse_url_ex("https://user:pw@example.org/a/b?x=1#f")
    assert(dict_get(ex, "ok", false), "url_ex ok")
    assert((dict_get(ex, "scheme", "") == "https"), "url_ex scheme")
    assert((dict_get(ex, "userinfo", "") == "user:pw"), "url_ex userinfo")
    assert((dict_get(ex, "host", "") == "example.org"), "url_ex host")
    assert(dict_get(ex, "port", 0) == 443, "url_ex default https port")
    assert((dict_get(ex, "target", "") == "/a/b?x=1"), "url_ex target")

    def q = "a=1&b=hello&c"
    def d = requests_parse_query(q)
    assert((dict_get(d, "a") == "1"), "query a")
    assert((dict_get(d, "b") == "hello"), "query b")
    assert(dict_get(d, "c") == 1, "query c flag")

    def raw = "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nbad"
    def parsed = requests_parse_response(raw)
    assert(dict_get(parsed, "status", 0) == 404, "parse response status")
    assert((dict_get(parsed, "reason", "") == "Not Found"), "parse response reason")
    assert((dict_get(parsed, "body", "") == "bad"), "parse response body")

    print("✓ std.net.requests tests passed")
}
