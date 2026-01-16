;;; fmt.ny --- io fmt module

;; Keywords: io fmt

;;; Commentary:

;; Io Fmt module.

use std.core
use std.core.reflect
use std.strings.str
module std.io.fmt (
	format, printf
)

fn format(fmt, ...args){
	"Replace each '{}' in fmt with str(args[i])."
	def xs = args
	if(len(args) == 1){
		def first = get(args, 0)
		if(eq(type(first), "list")){ xs = first }
	}
	def out = list(8)
	def n = str_len(fmt)
	def i = 0
	def start = 0
	def argi = 0
	while(i < n){
		def c = load8(fmt, i)
		if(c == 123){
			def next_i = i + 1
			if(next_i < n){
				def next_c = load8(fmt, next_i)
				if(next_c == 125){
					if(i > start){ out = append(out, slice(fmt, start, i, 1)) }
					if(argi < len(xs)){
						def v = get(xs, argi)
						out = append(out, str(v))
						argi = argi + 1
					} else {
						out = append(out, "{}")
					}
					i = i + 2
					start = i
					continue
				}
			}
		}
		i = i + 1
	}
	if(i > start){ out = append(out, slice(fmt, start, i, 1)) }
	return join(out, "")
}

fn printf(fmt, ...args){
	"Print formatted string."
	def s = format(fmt, args)
	sys_write(1, s, str_len(s))
	sys_write(1, "\n", 1)
}
