;;; urllib.ny --- util urllib module

;; Keywords: util urllib

;;; Commentary:

;; Util Urllib module.

use std.core
use std.net.http
use std.util.url
use std.collections
module std.util.urllib (
	request, urlopen
)

fn request(method, url, data=0){
	"Performs an HTTP request."
	def parts = parse_url(url)
	def host = get(parts, 0)
	def port = get(parts, 1)
	def path = get(parts, 2)
	return case method {
		"GET"    -> http_get(host, port, path)
		"POST"   -> http_post(host, port, path, data)
		"PUT"    -> http_put(host, port, path, data)
		"DELETE" -> http_delete(host, port, path)
		_        -> 0
	}
}

fn urlopen(url){
	"Opens a URL and returns the response body."
	return request("GET", url, 0)
}
