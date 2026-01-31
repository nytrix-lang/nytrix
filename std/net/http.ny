;; Keywords: net http
;; Net Http module.

use std.net.socket *
use std.core as core
use std.core.dict as _d
use std.str *
module std.net.http (
   _http_parse_url, http_get, http_post, http_put, http_delete, http_get_url,
   http_parse_url, http_parse_query
)

fn _http_parse_url(url){
   "HTTP helpers (wraps requests.http_get)."
   mut u = url
   if(startswith(u, "http://")){ u = core.slice(u, 7, len(u), 1)  }
   mut host = u
   mut path = "/"
   def idx = find(u, "/")
   if(idx >= 0){
      host = core.slice(u, 0, idx, 1)
      path = core.slice(u, idx, len(u), 1)
   }
   mut port = 80
   def cidx = find(host, ":")
   if(cidx >= 0){
      port = atoi(core.slice(host, cidx+1, len(host), 1))
      host = core.slice(host, 0, cidx, 1)
   }
   return [host, port, path]
}

fn http_get(host, port, path){
   "Performs an HTTP GET request."
   def fd = socket_connect(host, port)
   if(fd < 0){ return "" }
   write_socket(fd, f"GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n")
   mut res = ""
   while(1){
      def chunk = read_socket(fd, 4096)
      if(len(chunk) == 0){ break }
      res = f"{res}{chunk}"
   }
   close_socket(fd)
   return res
}

fn http_post(host, port, path, data){
   "Performs an HTTP POST request."
   def fd = socket_connect(host, port)
   if(fd < 0){ return "" }
   write_socket(fd, f"POST {path} HTTP/1.1\r\nHost: {host}\r\nContent-Length: {len(data)}\r\nConnection: close\r\n\r\n{data}")
   mut res = ""
   while(1){
      def chunk = read_socket(fd, 4096)
      if(len(chunk) == 0){ break }
      res = f"{res}{chunk}"
   }
   close_socket(fd)
   return res
}

fn http_put(host, port, path, data){
   "Performs an HTTP PUT request."
   def fd = socket_connect(host, port)
   if(fd < 0){ return "" }
   write_socket(fd, f"PUT {path} HTTP/1.1\r\nHost: {host}\r\nContent-Length: {len(data)}\r\nConnection: close\r\n\r\n{data}")
   mut res = ""
   while(1){
      def chunk = read_socket(fd, 4096)
      if(len(chunk) == 0){ break }
      res = f"{res}{chunk}"
   }
   close_socket(fd)
   return res
}

fn http_delete(host, port, path){
   "Performs an HTTP DELETE request."
   def fd = socket_connect(host, port)
   if(fd < 0){ return "" }
   write_socket(fd, f"DELETE {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n")
   mut res = ""
   while(1){
      def chunk = read_socket(fd, 4096)
      if(len(chunk) == 0){ break }
      res = f"{res}{chunk}"
   }
   close_socket(fd)
   return res
}

fn http_get_url(url){
   "Performs an HTTP GET request to the specified URL. Returns the response body."
   def parts = _http_parse_url(url)
   return http_get(get(parts, 0), get(parts, 1), get(parts, 2))
}

fn http_parse_url(url){
   "Parses a URL string into a list `[host, port, path]`."
   return _http_parse_url(url)
}

fn http_parse_query(q){
   "Parses a URL query string (e.g., 'a=1&b=2') into a dictionary."
   def d = _d.dict(16)
   if(q==0){ return d  }
   mut i = 0
   def n = len(q)
   while(i<n){
      mut j =i
      while(j<n && core.load8(q, j)!=38){ j=j+1  }
      def part = core.slice(q, i, j, 1)
      def eqi = find(part, "=")
      if(eqi >= 0){
         def k = core.slice(part, 0, eqi, 1)
         def v = core.slice(part, eqi+1, len(part), 1)
         _d.dict_set(d, k, v)
      } else {
         _d.dict_set(d, part, 1)
      }
      i = j + 1
   }
   return d
}
