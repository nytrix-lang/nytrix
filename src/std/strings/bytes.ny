;;; bytes.ny --- strings bytes module

;; Keywords: strings bytes

;;; Commentary:

;; Strings Bytes module.

use std.core
use std.core.mem
module std.strings.bytes (
	bytes, bytes_from_str, bytes_len, bytes_get, bytes_set, bytes_eq, bytes_slice, bytes_concat, bytes_to_str,
	hex_encode, _hex_val, hex_decode, bget, bset
)

; define BYTES_TAG = 122

fn bytes(n){
	"Create zeroed byte buffer."
	def p = rt_malloc(n + 8) ; length + data (tag at -8)
	store64(p, 122, -8)
	store64(p, n, 0)
	memset(p + 8, 0, n)
	return p
}

fn bytes_from_str(s){
	"Copy string to bytes buffer."
	def n = str_len(s)
	def buf = rt_malloc(n + 8)
	store64(buf, 122, -8)
	store64(buf, n, 0)
	memcpy(buf + 8, s, n)
	return buf
}

fn bytes_len(b){
	"Return length of bytes."
	if(b==0){ return 0  }
	if(rt_load64_idx(b, -8) != 122){ return 0  }
	return rt_load64_idx(b, 0)
}

fn bytes_get(b, i){
	if(i < 0 || i >= bytes_len(b)){ 0 } else { rt_load8_idx(b, 8 + i) }
}
fn bget(b, i){ bytes_get(b, i) }

fn bytes_set(b, i, v){
	rt_store8_idx(b, 8 + i, v)
	b
}
fn bset(b, i, v){ bytes_set(b, i, v) }

fn bytes_eq(a, b){
	def la = bytes_len(a)  def lb = bytes_len(b)
	if(la != lb){ false }
	else {
		def i = 0
		while(i < la){
			if(rt_load8_idx(a, i) != rt_load8_idx(b, i)){ return false }
			i = i + 1
		}
		true
	}
}
fn beq(a, b){ bytes_eq(a, b) }

fn bytes_slice(b, start, stop){
	def n = bytes_len(b)
	if(start < 0){ start = 0 }
	if(stop < 0 || stop > n){ stop = n }
	if(stop < start){ stop = start }
	def len = stop - start
	def out = bytes(len)
	def i = 0
	while(i < len){
		rt_store8_idx(out, 8 + i, rt_load8_idx(b, 8 + start + i))
		i = i + 1
	}
	out
}
fn bslice(b, start, stop){ bytes_slice(b, start, stop) }

fn bytes_concat(a, b){
	def la = bytes_len(a)  def lb = bytes_len(b)
	def out = bytes(la + lb)
	memcpy(out + 8, a + 8, la)
	memcpy(out + 8 + la, b + 8, lb)
	out
}
fn bconcat(a, b){ bytes_concat(a, b) }

fn bytes_to_str(b){
	"Convert bytes to string."
	def n = bytes_len(b)
	def s = rt_malloc(n + 1)
	rt_store64_idx(s, -8, 120) ; TAG_STR
	memcpy(s, b + 8, n)
	store8(s + n, 0)
	return s
}

fn hex_encode(b){
	"Hex encode bytes."
	def hex = "0123456789abcdef"
	def n = bytes_len(b)
	def out = rt_malloc(n*2 + 8)
	store64(out, 122, -8)
	store64(out, n*2, 0)
	def i=0  def o=0
	while(i<n){
		def v = rt_load8_idx(b, 8 + i)
		rt_store8_idx(out, 8 + o, rt_load8_idx(hex, ((v >> 4) & 15)))  o=o+1
		rt_store8_idx(out, 8 + o, rt_load8_idx(hex, (v & 15)))  o=o+1
		i=i+1
	}
	return out
}

fn _hex_val(c){
	"Internal: convert hex digit to value, or -1 if invalid."
	if(c>=48 && c<=57){ return c-48  }
	if(c>=97 && c<=102){ return 10 + (c-97)  }
	if(c>=65 && c<=70){ return 10 + (c-65)  }
	return -1
}

fn hex_decode(s){
	"Hex decode string to bytes (ignores invalid, stops on odd length)."
	def n = str_len(s)
	def len_out = n/2
	def out = rt_malloc(len_out + 8)
	store64(out, 122, -8)
	store64(out, len_out, 0)
	def i=0  def o=0
	while(i+1<n){
		def a = _hex_val(rt_load8_idx(s, i))
		def b = _hex_val(rt_load8_idx(s, i+1))
		if(a<0 || b<0){ break  }
		rt_store8_idx(out, 8 + o, (a<<4) + b)
		o=o+1  i=i+2
	}
	return out
}
