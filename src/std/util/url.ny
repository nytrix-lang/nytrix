;;; url.ny --- util url module

;; Keywords: util url

;;; Commentary:

;; Util Url module.

use std.core
use std.strings.str
module std.util.url (
	_is_alnum, _url_hex_val, _url_hex_char, urlencode, urldecode, url_parse, parse_url,
	parse_query
)

fn _is_alnum(c){
	"URL encoding/decoding."
	if(c>=48 && c<=57){ return 1  }
	if(c>=65 && c<=90){ return 1  }
	if(c>=97 && c<=122){ return 1  }
	return 0
}

fn _url_hex_val(c){
	"Internal: convert hex digit to value, or -1 if invalid."
	if(c>=48 && c<=57){ return c-48  }
	if(c>=65 && c<=70){ return 10 + (c-65)  }
	if(c>=97 && c<=102){ return 10 + (c-97)  }
	return -1
}

fn _url_hex_char(v){
	"Internal: convert 0..15 to lowercase hex digit."
	if(v<10){ return 48 + v  }
	return 87 + v
}

fn urlencode(s){
	"Percent-encode string."
	def n = str_len(s)
	def out = rt_malloc(n*3 + 1)
	def i =0  o=0
	while(i<n){
		def c = load8(s, i)
		if(_is_alnum(c)==1 || c==45 || c==95 || c==46 || c==126){
			store8(out, c, o)  o=o+1
		} else if(c==32){
			store8(out, 37, o)  store8(out, 50, o+1)  store8(out, 48, o+2)
			o=o+3
		} else {
			store8(out, 37, o)
			store8(out, _url_hex_char((c >> 4) & 15), o+1)
			store8(out, _url_hex_char(c & 15), o+2)
			o=o+3
		}
		i=i+1
	}
	store8(out, 0, o)
	return out
}

fn urldecode(s){
	"Percent-decode string (accepts + as space)."
	def n = str_len(s)
	def out = rt_malloc(n + 1)
	def i =0  o=0
	while(i<n){
		def c = load8(s, i)
		if(c==37 && i+2<n){
			def a = _url_hex_val(load8(s, i+1))
			def b = _url_hex_val(load8(s, i+2))
			if(a>=0 && b>=0){
				store8(out, (a<<4) + b, o)
				o=o+1  i=i+3  continue
			}
		}
		if(c==43){ c=32  }
		store8(out, c, o)  o=o+1  i=i+1
	}
	store8(out, 0, o)
	return out
}

fn url_parse(url){
	"Parse URL into components: {scheme, host, port, path, query, fragment}. Example: url_parse('http://example.com:8080/path?q=1 frag')."
	if(url == 0){ return list(8)  }
	def result = list(8)
	def len = str_len(url)
	def i = 0
	;  Extract scheme (protocol)
	def scheme_end = find(url, "://")
	if(scheme_end >= 0){
		def scheme = slice(url, 0, scheme_end, 1)
		result = append(result, scheme)
		i = scheme_end + 3
	} else {
		result = append(result, "")
	}
	;  Find end of authority (host:port)
	def authority_end = i
	while(authority_end < len){
		def c = load8(url, authority_end)
		if(c == 47 || c == 63 || c == 35){ break  } ; "/, ?,  //"
		authority_end = authority_end + 1
	}
	;  Extract host and port
	def authority = slice(url, i, authority_end, 1)
	def at_pos = find(authority, "@")
	if(at_pos >= 0){
		def auth = slice(authority, 0, at_pos, 1)
		result = append(result, auth)
		authority = slice(authority, at_pos + 1, str_len(authority), 1)
	} else {
		result = append(result, "")
	}
	def port_sep = find(authority, ":")
	if(port_sep >= 0){
		def host = slice(authority, 0, port_sep, 1)
		def port_str = slice(authority, port_sep + 1, str_len(authority), 1)
		result = append(result, host)
		result = append(result, atoi(port_str))
	} else {
		result = append(result, authority)
		result = append(result, 80)
	}
	i = authority_end
	;  Extract path
	def path_end = i
	while(path_end < len && load8(url, path_end) != 63 && load8(url, path_end) != 35){ ; " "
		path_end = path_end + 1
	}
	if(path_end > i){
		def path = slice(url, i, path_end, 1)
		result = append(result, path)
	} else {
		result = append(result, "/")
	}
	i = path_end
	;  Extract query
	if(i < len && load8(url, i) == 63){ ; "?"
		i = i + 1
		def query_end = i
		while(query_end < len){
			if(load8(url, query_end) == 35){ break  } ; " "
			query_end = query_end + 1
		}
		def query = slice(url, i, query_end, 1)
		result = append(result, query)
		i = query_end
	} else {
		result = append(result, "")
	}
	;  Extract fragment
	if(i < len && load8(url, i) == 35){ ; " "
		def fragment = slice(url, i + 1, len, 1)
		result = append(result, fragment)
	} else {
		result = append(result, "")
	}
	return result
}

fn parse_url(url){
	"Compatible parse_url returning [host, port, path]."
	def p = url_parse(url)
	if(len(p) < 7){ return ["", 80, "/"] }
	def host = get(p, 2)
	def port = get(p, 3)
	if(port == 0){ port = 80 }
	def path = get(p, 4)
	return [host, port, path]
}

fn parse_query(query){
	"Parse query string into a dictionary. Example: parse_query('a=1&b=2') returns {'a':'1','b':'2'}."
	def result = dict(16)
	if(query == 0 || str_len(query) == 0){ return result }
	def len = str_len(query)
	def i = 0
	while(i < len){
		;  Find next & or end
		def pair_end = i
		while(pair_end < len && load8(query, pair_end) != 38){ ; "&"
			pair_end = pair_end + 1
		}
		;  Extract key=value
		def pair = slice(query, i, pair_end, 1)
		def eq_pos = find(pair, "=")
		if(eq_pos >= 0){
			def key = slice(pair, 0, eq_pos, 1)
			def value = slice(pair, eq_pos + 1, str_len(pair), 1)
			def key_dec = urldecode(key)
			def value_dec = urldecode(value)
			setitem(result, cstr_to_str(key_dec), cstr_to_str(value_dec))
		} else if(str_len(pair) > 0){
			def key_dec = urldecode(pair)
			setitem(result, cstr_to_str(key_dec), 1)
		}
		i = pair_end + 1
	}
	return result
}
