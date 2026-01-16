;;; msgpack.ny --- util msgpack module
;; Keywords: util msgpack
;; Commentary:
;; Util Msgpack module.

use std.core
use std.core.error
use std.collections.dict
use std.strings.str
use std.collections
module std.util.msgpack (
	_be_bytes, _append, _append_many, msgpack_encode, _mp_enc, msgpack_decode,
	msgpack_decode_from, msgpack_stream_decode, _mp_dec
)

fn _be_bytes(n, count){
	def b = list(8)
	def i = (count - 1) * 8
	while(i >= 0){
		b = append(b, (n >> i) & 255)
		i = i - 8
	}
	return b
}

fn _append(lst, b){
	return append(lst, b & 255)
}

fn _append_many(lst, src){
	def i = 0
	def n = list_len(src)
	while(i < n){
		lst = append(lst, get(src, i))
		i = i + 1
	}
	return lst
}

fn msgpack_encode(v){
	def out = list(8)
	return _mp_enc(v, out)
}

fn _mp_enc(v, out){
	def t = type(v)
	if(t == "dict"){
		def n = list_len(v)
		if(n < 16){
			out = _append(out, 0x80 | n)
		} else {
			out = _append(out, 0xde)
			out = _append(out, n >> 8)
			out = _append(out, n & 255)
		}
		def pairs = items(v)
		def i = 0
		while(i < n){
			def p = get(pairs, i)
			out = _mp_enc(get(p, 0), out)
			out = _mp_enc(get(p, 1), out)
			i = i + 1
		}
	}
	elif(t == "list" || t == "tuple"){
		def n = list_len(v)
		if(n < 16){
			out = _append(out, 0x90 | n)
		} else {
			out = _append(out, 0xdc)
			out = _append(out, n >> 8)
			out = _append(out, n & 255)
		}
		def i = 0
		while(i < n){
			out = _mp_enc(get(v, i), out)
			i = i + 1
		}
	}
	elif(t == "str"){
		def l = str_len(v)
		if(l < 32){
			out = _append(out, 0xa0 | l)
		} elif(l < 256){
			out = _append(out, 0xd9)
			out = _append(out, l)
		} elif(l < 65536){
			out = _append(out, 0xda)
			out = _append(out, l >> 8)
			out = _append(out, l & 255)
		} else {
			out = _append(out, 0xdb)
			out = _append(out, (l >> 24) & 255)
			out = _append(out, (l >> 16) & 255)
			out = _append(out, (l >> 8) & 255)
			out = _append(out, l & 255)
		}
		def i = 0
		while(i < l){
			out = _append(out, load8(v, i))
			i = i + 1
		}
	}
	elif(t == "int"){
		if(v >= 0){
			if(v < 128){
				out = _append(out, v)
			} elif(v < 256){
				out = _append(out, 0xcc)
				out = _append(out, v)
			} elif(v < 65536){
				out = _append(out, 0xcd)
				out = _append_many(out, _be_bytes(v, 2))
			} elif(v < 4294967296){
				out = _append(out, 0xce)
				out = _append_many(out, _be_bytes(v, 4))
			} else {
				out = _append(out, 0xcf)
				out = _append_many(out, _be_bytes(v, 8))
			}
		} else {
			if(v >= -32){
				out = _append(out, v)
			} elif(v >= -128){
				out = _append(out, 0xd0)
				out = _append(out, v)
			} elif(v >= -32768){
				out = _append(out, 0xd1)
				out = _append_many(out, _be_bytes(v, 2))
			} elif(v >= -2147483648){
				out = _append(out, 0xd2)
				out = _append_many(out, _be_bytes(v, 4))
			} else {
				out = _append(out, 0xd3)
				out = _append_many(out, _be_bytes(v, 8))
			}
		}
	}
	elif(t == "none"){
		out = _append(out, 0xc0)
	}
	return out
}

fn msgpack_decode(bytes){
	def p = _mp_dec(bytes, 0)
	return get(p, 0)
}

fn msgpack_decode_from(bytes, idx){
	return _mp_dec(bytes, idx)
}

fn msgpack_stream_decode(bytes){
	def out = list(8)
	def i = 0
	def n = list_len(bytes)
	while(i < n){
		def p = _mp_dec(bytes, i)
		out = append(out, get(p, 0))
		i = get(p, 1)
	}
	return out
}

fn _mp_dec(bytes, i){
	def b = get(bytes, i)
	i = i + 1
	if (b < 128) { return [b, i] }
	if (b >= 224) { return [b - 256, i] }
	if ((b & 240) == 128) {
		def n = b & 15
		def m = dict(16)
		def j = 0
		while(j < n){
			def pk = _mp_dec(bytes, i)
			def k = get(pk, 0)
			i = get(pk, 1)
			def pv = _mp_dec(bytes, i)
			def v = get(pv, 0)
			i = get(pv, 1)
			m = setitem(m, k, v)
			j = j + 1
		}
		return [m, i]
	}
	if ((b & 240) == 144) {
		def n = b & 15
		def lst = list(8)
		def j = 0
		while(j < n){
			def pv = _mp_dec(bytes, i)
			def v = get(pv, 0)
			i = get(pv, 1)
			lst = append(lst, v)
			j = j + 1
		}
		return [lst, i]
	}
	if ((b & 224) == 160) {
		def l = b & 31
		def s = rt_malloc(l + 1)
		rt_init_str(s, l)
		def j = 0
		while(j < l){
			rt_store8_idx(s, j, get(bytes, i + j))
			j = j + 1
		}
		rt_store8_idx(s, l, 0)
		return [s, i + l]
	}
	case b {
		192 -> { return [0, i] }
		217 -> {
			def l = get(bytes, i)
			i = i + 1
			def s = rt_malloc(l + 1)
			rt_init_str(s, l)
			def j = 0
			while(j < l){
				rt_store8_idx(s, j, get(bytes, i + j))
				j = j + 1
			}
			rt_store8_idx(s, l, 0)
			return [s, i + l]
		}
		218 -> {
			def l = (get(bytes, i) << 8) | get(bytes, i + 1)
			i = i + 2
			def s = rt_malloc(l + 1)
			rt_init_str(s, l)
			def j = 0
			while(j < l){
				rt_store8_idx(s, j, get(bytes, i + j))
				j = j + 1
			}
			rt_store8_idx(s, l, 0)
			return [s, i + l]
		}
		219 -> {
			def l = (get(bytes, i) << 24) | (get(bytes, i + 1) << 16) | (get(bytes, i + 2) << 8) | get(bytes, i + 3)
			i = i + 4
			def s = rt_malloc(l + 1)
			rt_init_str(s, l)
			def j = 0
			while(j < l){
				rt_store8_idx(s, j, get(bytes, i + j))
				j = j + 1
			}
			rt_store8_idx(s, l, 0)
			return [s, i + l]
		}
		220 -> {
			def l = (get(bytes, i) << 8) | get(bytes, i + 1)
			i = i + 2
			def lst = list(8)
			def j = 0
			while(j < l){
				def pv = _mp_dec(bytes, i)
				lst = append(lst, get(pv, 0))
				i = get(pv, 1)
				j = j + 1
			}
			return [lst, i]
		}
		222 -> {
			def l = (get(bytes, i) << 8) | get(bytes, i + 1)
			i = i + 2
			def m = dict(16)
			def j = 0
			while(j < l){
				def pk = _mp_dec(bytes, i)
				def k = get(pk, 0)
				i = get(pk, 1)
				def pv = _mp_dec(bytes, i)
				def v = get(pv, 0)
				i = get(pv, 1)
				m = setitem(m, k, v)
				j = j + 1
			}
			return [m, i]
		}
		208 -> {
			def v = get(bytes, i)
			if(v > 127) { v = v - 256 }
			return [v, i + 1]
		}
		209 -> {
			def v = (get(bytes, i) << 8) | get(bytes, i + 1)
			if(v > 32767) { v = v - 65536 }
			return [v, i + 2]
		}
		210 -> {
			def v = (get(bytes, i) << 24) | (get(bytes, i + 1) << 16) | (get(bytes, i + 2) << 8) | get(bytes, i + 3)
			if(v > 2147483647) { v = v - 4294967296 }
			return [v, i + 4]
		}
		204 -> { return [get(bytes, i), i + 1] }
		205 -> { return [(get(bytes, i) << 8) | get(bytes, i + 1), i + 2] }
		206 -> { return [(get(bytes, i) << 24) | (get(bytes, i + 1) << 16) | (get(bytes, i + 2) << 8) | get(bytes, i + 3), i + 4] }
		207 -> {
			def val = 0
			def j = 0
			while(j < 8){
				val = (val << 8) | get(bytes, i + j)
				j = j + 1
			}
			return [val, i + 8]
		}
		211 -> {
			def val = 0
			def j = 0
			while(j < 8){
				val = (val << 8) | get(bytes, i + j)
				j = j + 1
			}
			return [val, i + 8]
		}
		_ -> { panic("msgpack: unsupported type") }
	}
}
