;;; path.ny --- io path module

;; Keywords: io path

;;; Commentary:

;; Io Path module.

use std.core
use std.strings.str
use std.io
module std.io.path (
	path_join_list, abspath, basename, dirname, extname, normalize, path_join
)

fn path_join_list(xs){
	"Join list of path segments with '/'."
	if(list_len(xs) == 0){ return "" }
	def res = get(xs, 0)
	def i = 1
	while(i < list_len(xs)){
		def part = get(xs, i)
		if(startswith(part, "/")){
			res = part
		} else {
			if(endswith(res, "/")){
				res = f"{res}{part}"
			} else {
				res = f"{res}/{part}"
			}
		}
		i = i + 1
	}
	return res
}

fn abspath(p){
	"Absolute path."
	if(startswith(p, "/")){ return p }
	return path_join_list([cwd(), p])
}

fn basename(p){
	"Return the last component of a path."
	def n = str_len(p)
	if(n == 0){ return "" }
	; Trim trailing slashes
	while(n > 0){
		def off = n - 1
		if(load8(p, off) != 47){ break }
		n = n - 1
	}
	if(n == 0){ return "/" }
	def i = n - 1
	while(i >= 0){
		if(load8(p, i) == 47){ return slice(p, i + 1, n, 1) }
		i = i - 1
	}
	return slice(p, 0, n, 1)
}

fn dirname(p){
	"Return the directory component of a path."
	def n = str_len(p)
	if(n == 0){ return "." }
	; Trim trailing slashes
	while(n > 0){
		def off = n - 1
		if(load8(p, off) != 47){ break }
		n = n - 1
	}
	if(n == 0){ return "/" }
	def i = n - 1
	while(i >= 0){
		if(load8(p, i) == 47){
			if(i == 0){ return "/" }
			return slice(p, 0, i, 1)
		}
		i = i - 1
	}
	return "."
}

; Extension including dot, or \"\"

fn extname(p){
	"Return extension including dot, or empty string."
	def b = basename(p)
	def n = str_len(b)
	def i = n-1
	while(i>=0){
		def c = load8(b, i)
		if(c==46){ return slice(b, i, n, 1)  }
		if(c==47){ return "" }
		i=i-1
	}
	return ""
}

fn normalize(p){
	"Normalize: handle . and .. and trailing slash."
	def parts = split(p, "/")
	def res = list(8)
	def i = 0
	while(i < list_len(parts)){
		def part = get(parts, i)
		case part {
			".."    -> { if(list_len(res) > 0){ pop(res) } }
			".", "" -> {}
			_       -> { res = append(res, part) }
		}
		i = i + 1
	}
	def out = join(res, "/")
	if(startswith(p, "/")){ f"/{out}" } else { out }
}

fn path_join(a, b=0, c=0, d=0){
	"Join multiple paths as arguments."
	def l = list(8)
	l = append(l, a)
	if(b != 0){ l = append(l, b) }
	if(c != 0){ l = append(l, c) }
	if(d != 0){ l = append(l, d) }
	return path_join_list(l)
}
