;; Keywords: str json rfc8259
;; JSON parser and generator.
;; Reference: https://www.rfc-editor.org/rfc/rfc8259.html

module std.enc.json (
    json_decode, json_try_decode, json_last_error, json_encode
)
use std.core *
use std.core.dict *
use std.text as str
use std.math.float as f

mut _json_error = ""

;; Decoder state: [source, length, pos, error]

fn json_last_error(){
    "Returns the error from the last decode attempt (empty string on success)."
    _json_error
}

fn _json_set_error(st, msg){
    "Internal: Sets the error state message if one isn't already set."
    def cur = get(st, 3, "")
    if(!is_str(cur) || str_len(cur) == 0){
        set_idx(st, 3, msg)
    }
    0
}

fn _json_make_result(ok, value, err, pos){
    "Internal: Constructs the result dictionary for json_try_decode."
    mut r = dict(8)
    r = dict_set(r, "ok", ok)
    r = dict_set(r, "value", value)
    r = dict_set(r, "error", err)
    r = dict_set(r, "pos", pos)
    r
}

fn _json_peek(st){
    "Internal: Peeks at the current character without advancing."
    def pos = get(st, 2)
    def n = get(st, 1)
    if(pos < 0 || pos >= n){ return -1 }
    load8(get(st, 0), pos)
}

fn _json_skip_ws(st){
    "Internal: Skips ASCII whitespace (space, tab, newline, CR)."
    def s = get(st, 0)
    def n = get(st, 1)
    mut pos = get(st, 2)
    while(pos < n){
        def c = load8(s, pos)
        if(c == 32 || c == 9 || c == 10 || c == 13){
            pos += 1
        } else { break }
    }
    set_idx(st, 2, pos)
}

fn _json_expect(st, want, msg){
    "Internal: expects a specific character or sets an error."
    def c = _json_peek(st)
    if(c != want){ return _json_set_error(st, msg) }
    set_idx(st, 2, get(st, 2) + 1)
    true
}

fn _json_is_digit(c){
    "Internal: Returns true if char code is a digit 0-9."
    c >= 48 && c <= 57
}

fn _json_hex(c){
    "Internal: Converts a hex char code to integer value."
    if(c >= 48 && c <= 57){ return c - 48 }
    if(c >= 65 && c <= 70){ return c - 55 }
    if(c >= 97 && c <= 102){ return c - 87 }
    -1
}

fn _json_hex4(s, start){
    "Internal: Decodes 4 hex chars at `start` into integer; -1 on invalid."
    mut cp = 0
    mut k = 0
    while(k < 4){
        def hv = _json_hex(load8(s, start + k))
        if(hv < 0){ return -1 }
        cp = cp * 16 + hv
        k += 1
    }
    cp
}

fn _json_byte_to_str(c){
    "Internal: Creates a one-byte string from raw byte `c`."
    def tmp = malloc(2)
    if(!tmp){ return "" }
    init_str(tmp, 1)
    store8(tmp, c, 0)
    store8(tmp, 0, 1)
    tmp
}

fn _json_parse_literal(st, lit, value){
    "Internal: Parses specific literal tokens like true, false, null."
    def s = get(st, 0)
    def n = get(st, 1)
    def pos = get(st, 2)
    def m = str_len(lit)
    if(pos + m > n){ return _json_set_error(st, "unexpected end while parsing literal") }
    mut i = 0
    while(i < m){
        if(load8(s, pos + i) != load8(lit, i)){
            return _json_set_error(st, "invalid literal")
        }
        i += 1
    }
    set_idx(st, 2, pos + m)
    value
}

fn _json_parse_float_text(s){
    "Internal: Helper to parse float from string representation manually."
    def n = str_len(s)
    mut i = 0
    mut sign = 1
    if(i < n && load8(s, i) == 45){
        sign = -1
        i += 1
    }
    mut int_part = 0
    while(i < n && _json_is_digit(load8(s, i))){
        int_part = int_part * 10 + (load8(s, i) - 48)
        i += 1
    }
    mut frac_part = 0
    mut frac_scale = 1
    if(i < n && load8(s, i) == 46){
        i += 1
        while(i < n && _json_is_digit(load8(s, i))){
            frac_part = frac_part * 10 + (load8(s, i) - 48)
            frac_scale = frac_scale * 10
            i += 1
        }
    }
    mut out = f.float(int_part)
    if(frac_scale > 1){
        out = f.fadd(out, f.fdiv(f.float(frac_part), f.float(frac_scale)))
    }
    if(i < n && (load8(s, i) == 101 || load8(s, i) == 69)){
        i += 1
        mut exp_sign = 1
        if(i < n && load8(s, i) == 45){
            exp_sign = -1
            i += 1
        } elif(i < n && load8(s, i) == 43){
            i += 1
        }
        mut exp_val = 0
        while(i < n && _json_is_digit(load8(s, i))){
            exp_val = exp_val * 10 + (load8(s, i) - 48)
            i += 1
        }
        def ten = f.float(10)
        while(exp_val > 0){
            if(exp_sign > 0){ out = f.fmul(out, ten) }
            else { out = f.fdiv(out, ten) }
            exp_val -= 1
        }
    }
    if(sign < 0){ out = f.fsub(f.float(0), out) }
    out
}

fn _json_parse_val(st){
    "Internal: Recursive JSON value parser."
    _json_skip_ws(st)
    def c = _json_peek(st)
    if(c < 0){ return _json_set_error(st, "unexpected end of input") }
    if(c == 123){ return _json_parse_obj(st) } ;; '{'
    if(c == 91){ return _json_parse_arr(st) } ;; '['
    if(c == 34){ return _json_parse_str(st) } ;; '"'
    if(c == 116){ return _json_parse_literal(st, "true", true) } ;; true
    if(c == 102){ return _json_parse_literal(st, "false", false) } ;; false
    if(c == 110){ return _json_parse_literal(st, "null", 0) } ;; null
    if(c == 45 || (c >= 48 && c <= 57)){ return _json_parse_num(st) }
    _json_set_error(st, "unexpected token")
}
fn _json_parse_obj(st){
    "Internal: Parses a JSON object."
    if(!_json_expect(st, 123, "expected '{'")){ return 0 }
    mut d = dict(8)
    _json_skip_ws(st)
    if(_json_peek(st) == 125){
        set_idx(st, 2, get(st, 2) + 1)
        return d
    }
    while(1){
        _json_skip_ws(st)
        if(_json_peek(st) != 34){ return _json_set_error(st, "expected string key") }
        def key = _json_parse_str(st)
        if(str_len(get(st, 3, "")) > 0){ return 0 }
        _json_skip_ws(st)
        if(!_json_expect(st, 58, "expected ':' after object key")){ return 0 }
        def val = _json_parse_val(st)
        if(str_len(get(st, 3, "")) > 0){ return 0 }
        d = dict_set(d, key, val)
        _json_skip_ws(st)
        def c = _json_peek(st)
        if(c == 44){ ;; ','
            set_idx(st, 2, get(st, 2) + 1)
            continue
        }
        if(c == 125){ ;; '}'
            set_idx(st, 2, get(st, 2) + 1)
            return d
        }
        return _json_set_error(st, "expected ',' or '}' in object")
    }
}

fn _json_parse_arr(st){
    "Internal: Parses a JSON array."
    if(!_json_expect(st, 91, "expected '['")){ return 0 }
    mut l = list(8)
    _json_skip_ws(st)
    if(_json_peek(st) == 93){
        set_idx(st, 2, get(st, 2) + 1)
        return l
    }
    while(1){
        l = append(l, _json_parse_val(st))
        if(str_len(get(st, 3, "")) > 0){ return 0 }
        _json_skip_ws(st)
        def c = _json_peek(st)
        if(c == 44){
            set_idx(st, 2, get(st, 2) + 1)
            continue
        }
        if(c == 93){
            set_idx(st, 2, get(st, 2) + 1)
            return l
        }
        return _json_set_error(st, "expected ',' or ']' in array")
    }
}

fn _json_parse_str(st){
    "Internal: Parses a JSON string literal with escapes."
    mut pos = get(st, 2)
    def s = get(st, 0)
    def n = get(st, 1)
    if(pos >= n || load8(s, pos) != 34){ return _json_set_error(st, "expected string") }
    pos += 1
    mut out = ""
    while(pos < n){
        def c = load8(s, pos)
        if(c == 34){
            set_idx(st, 2, pos + 1)
            return out
        }
        if(c == 92){
            pos += 1
            if(pos >= n){ return _json_set_error(st, "unterminated escape sequence") }
            def esc = load8(s, pos)
            match esc {
                34 -> { out = out + "\"" } ;; "
                92 -> { out = out + "\\" } ;; \
                47 -> { out = out + "/" } ;; /
                98 -> { out = out + chr(8) } ;; \b
                102 -> { out = out + chr(12) } ;; \f
                110 -> { out = out + "\n" } ;; \n
                114 -> { out = out + "\r" } ;; \r
                116 -> { out = out + "\t" } ;; \t
                117 -> {
                    if(pos + 4 >= n){ return _json_set_error(st, "invalid unicode escape") }
                    mut cp1 = _json_hex4(s, pos + 1)
                    if(cp1 < 0){ return _json_set_error(st, "invalid unicode escape") }
                    pos = pos + 4
                    mut cp = cp1
                    ;; Handle UTF-16 surrogate pairs.
                    if(cp1 >= 55296 && cp1 <= 56319){
                        if(pos + 6 >= n){ return _json_set_error(st, "invalid unicode surrogate pair") }
                        if(load8(s, pos + 1) != 92 || load8(s, pos + 2) != 117){
                            return _json_set_error(st, "invalid unicode surrogate pair")
                        }
                        def cp2 = _json_hex4(s, pos + 3)
                        if(cp2 < 56320 || cp2 > 57343){
                            return _json_set_error(st, "invalid unicode surrogate pair")
                        }
                        cp = 65536 + ((cp1 - 55296) * 1024) + (cp2 - 56320)
                        pos = pos + 6
                    } elif(cp1 >= 56320 && cp1 <= 57343){
                        return _json_set_error(st, "invalid unicode surrogate pair")
                    }
                    out = out + chr(cp)
                }
                _ -> { return _json_set_error(st, "invalid escape sequence") }
            }
        } else {
            if(c < 32){ return _json_set_error(st, "invalid control character in string") }
            ;; Preserve original UTF-8 bytes from the source text.
            out = out + _json_byte_to_str(c)
        }
        pos += 1
    }
    _json_set_error(st, "unterminated string")
}

fn _json_parse_num(st){
    "Internal: Parses integer/float JSON number."
    mut pos = get(st, 2)
    def s = get(st, 0)
    def n = get(st, 1)
    mut start = pos
    if(pos < n && load8(s, pos) == 45){ pos += 1 }
    if(pos >= n){ return _json_set_error(st, "invalid number") }
    if(load8(s, pos) == 48){
        pos += 1
        if(pos < n && _json_is_digit(load8(s, pos))){
            return _json_set_error(st, "leading zero in number")
        }
    } elif(_json_is_digit(load8(s, pos))){
        while(pos < n && _json_is_digit(load8(s, pos))){ pos += 1 }
    } else {
        return _json_set_error(st, "invalid number")
    }
    mut has_frac = false
    mut has_exp = false
    if(pos < n && load8(s, pos) == 46){
        has_frac = true
        pos += 1
        if(pos >= n || !_json_is_digit(load8(s, pos))){
            return _json_set_error(st, "invalid fraction in number")
        }
        while(pos < n && _json_is_digit(load8(s, pos))){ pos += 1 }
    }
    if(pos < n && (load8(s, pos) == 101 || load8(s, pos) == 69)){
        has_exp = true
        pos += 1
        if(pos < n && (load8(s, pos) == 43 || load8(s, pos) == 45)){ pos += 1 }
        if(pos >= n || !_json_is_digit(load8(s, pos))){
            return _json_set_error(st, "invalid exponent in number")
        }
        while(pos < n && _json_is_digit(load8(s, pos))){ pos += 1 }
    }
    def len = pos - start
    def tmp = malloc(len + 1)
    __copy_mem(tmp, s + start, len)
    store8(tmp, 0, len)
    init_str(tmp, len)
    mut res = 0
    if(has_frac || has_exp){ res = _json_parse_float_text(tmp) }
    else { res = str.atoi(tmp) }
    set_idx(st, 2, pos)
    free(tmp)
    return res
}

fn json_try_decode(s){
    "Decodes JSON and returns `{ok, value, error, pos}`."
    if(!is_str(s)){
        _json_error = "json input must be a string"
        return _json_make_result(false, 0, _json_error, 0)
    }
    def st = [s, str.len(s), 0, ""]
    def val = _json_parse_val(st)
    _json_skip_ws(st)
    mut err = get(st, 3, "")
    if(str_len(err) == 0 && get(st, 2) != get(st, 1)){
        err = "trailing characters after JSON value"
        set_idx(st, 3, err)
    }
    _json_error = get(st, 3, "")
    if(str_len(_json_error) == 0){
        return _json_make_result(true, val, "", get(st, 2))
    }
    _json_make_result(false, 0, _json_error, get(st, 2))
}

fn json_decode(s){
    "Decodes JSON string and returns parsed value (`0` on error)."
    def res = json_try_decode(s)
    if(res != 0 && dict_get(res, "ok", false)){
        return dict_get(res, "value", 0)
    }
    0
}

fn _json_hex_digit(n){
    "Internal: Converts a nibble (0-15) to hex char."
    if(n < 10){ return chr(48 + n) }
    chr(87 + n)
}

fn _json_escape_string(s){
    "Internal: Escapes a string for JSON output, handling control chars and unicode."
    if(!is_str(s)){ return "\"\"" }
    def n = str_len(s)
    mut out = "\""
    mut i = 0
    while(i < n){
        def c = load8(s, i)
        match c {
            34 -> { out = out + "\\\"" }
            92 -> { out = out + "\\\\" }
            8 -> { out = out + "\\b" }
            12 -> { out = out + "\\f" }
            10 -> { out = out + "\\n" }
            13 -> { out = out + "\\r" }
            9 -> { out = out + "\\t" }
            _ -> {
                if(c < 32){
                    out = out + "\\u00" + _json_hex_digit((c / 16) % 16) + _json_hex_digit(c % 16)
                } else {
                    ;; Preserve original UTF-8 bytes from the source text.
                    out = out + _json_byte_to_str(c)
                }
            }
        }
        i += 1
    }
    out + "\""
}

fn _json_encode_seq(v){
    "Internal: Helper to encode sequence types (list, tuple, set)."
    mut out = "["
    def n = len(v)
    mut i = 0
    while(i < n){
        out = out + json_encode(get(v, i))
        if(i + 1 < n){ out = out + "," }
        i += 1
    }
    out + "]"
}

fn json_encode(obj){
    "Encodes Nytrix values into JSON string."
    if(type(obj) == "bool"){
        if obj{ return "true" }
        return "false"
    }
    if(obj == 0){ return "null" }
    if(is_int(obj)){
        return to_str(obj)
    }
    if(f.is_float(obj)){ return to_str(obj) }
    if(is_str(obj)){ return _json_escape_string(obj) }
    if(is_list(obj) || is_tuple(obj) || is_set(obj)){ return _json_encode_seq(obj) }
    if(is_dict(obj)){
        mut out = "{"
        def items = dict_items(obj)
        mut i = 0
        def n = len(items)
        while(i < n){
            def pair = get(items, i)
            def k = get(pair, 0)
            def v = get(pair, 1)
            mut key = ""
            if(is_str(k)){ key = k }
            else { key = to_str(k) }
            out = out + _json_escape_string(key) + ":" + json_encode(v)
            if(i + 1 < n){ out = out + "," }
            i += 1
        }
        return out + "}"
    }
    "null"
}

if(comptime{__main()}){
    use std.core *
    use std.enc.json *
    use std.text *
    use std.core.dict *

    def n = json_decode("123")
    assert(n == 123, "json number")

    def t = json_decode("true")
    assert(t == true, "json true")

    def f = json_decode("false")
    assert(f == false, "json false")

    def nul = json_decode("null")
    assert(nul == 0, "json null")

    def s = json_decode("\"hi\"")
    assert(is_str(s), "json string")
    assert(str_len(s) == 2, "json string len")

    def esc = json_decode("\"line\\n\\tquote:\\\"\"")
    assert((esc == "line\n\tquote:\""), "json escaped string")

    def euro = json_decode("\"\\u20AC\"")
    assert((euro == chr(8364)), "json unicode bmp")

    def raw_euro = json_decode("\"" + chr(8364) + "\"")
    assert((raw_euro == chr(8364)), "json raw utf8 char")

    def grin = json_decode("\"\\uD83D\\uDE00\"")
    assert((grin == chr(128512)), "json unicode surrogate pair")

    def bad_sur = json_try_decode("\"\\uD83D\"")
    assert(!dict_get(bad_sur, "ok", true), "json invalid surrogate pair")

    def arr = json_decode("[1,2,3]")
    assert(is_list(arr), "json array list")
    assert(len(arr) == 3, "json array len")
    assert(get(arr, 2) == 3, "json array value")

    def obj = json_decode("{\"a\":1,\"b\":[2,3],\"s\":\"x\"}")
    assert(is_dict(obj), "json object dict")
    assert(dict_get(obj, "a") == 1, "json object field int")
    def obj_b = dict_get(obj, "b", list(0))
    assert(is_list(obj_b), "json object nested list")
    assert(get(obj_b, 0) == 2, "json object nested list first")
    assert((dict_get(obj, "s", "") == "x"), "json object field str")

    def try_bad = json_try_decode("{\"a\":1")
    assert(!dict_get(try_bad, "ok", true), "json_try_decode invalid")
    assert(str_len(dict_get(try_bad, "error", "")) > 0, "json_try_decode error message")
    assert(str_len(json_last_error()) > 0, "json_last_error set")

    mut enc_obj = dict(8)
    enc_obj = dict_set(enc_obj, "n", 7)
    enc_obj = dict_set(enc_obj, "ok", true)
    enc_obj = dict_set(enc_obj, "name", "ny")
    enc_obj = dict_set(enc_obj, "arr", [1, 2])

    def enc = json_encode(enc_obj)
    def dec = json_decode(enc)
    assert(is_dict(dec), "json encode+decode dict")
    assert(dict_get(dec, "n") == 7, "json roundtrip int")
    assert(dict_get(dec, "ok") == true, "json roundtrip bool")
    assert((dict_get(dec, "name", "") == "ny"), "json roundtrip string")
    def dec_arr = dict_get(dec, "arr", list(0))
    assert(is_list(dec_arr), "json roundtrip list")
    assert(get(dec_arr, 1) == 2, "json roundtrip list value")

    def enc_s = json_encode("a\tb\nc\"d\\e")
    def dec_s = json_decode(enc_s)
    assert((dec_s == "a\tb\nc\"d\\e"), "json encode escaped string")

    def enc_uni = json_encode("price:" + chr(8364))
    def dec_uni = json_decode(enc_uni)
    assert((dec_uni == "price:" + chr(8364)), "json encode unicode utf8")

    ;; Error path tests
    def err_non_str = json_try_decode(123)
    assert(dict_get(err_non_str, "ok", true) == false, "json error non-string")
    assert(dict_get(err_non_str, "error", "") == "json input must be a string", "json error msg non-string")

    def err_trailing = json_try_decode("123 abc")
    assert(dict_get(err_trailing, "ok", true) == false, "json error trailing chars")
    assert(dict_get(err_trailing, "error", "") == "trailing characters after JSON value", "json error msg trailing chars")

    def err_empty = json_try_decode("")
    assert(dict_get(err_empty, "ok", true) == false, "json error empty")
    assert(dict_get(err_empty, "error", "") == "unexpected end of input", "json error msg empty")

    def err_token = json_try_decode("abc")
    assert(dict_get(err_token, "ok", true) == false, "json error unexpected token")
    assert(dict_get(err_token, "error", "") == "unexpected token", "json error msg unexpected token")

    def err_lit_trail = json_try_decode("truex")
    assert(dict_get(err_lit_trail, "ok", true) == false, "json error literal trailing")
    assert(dict_get(err_lit_trail, "error", "") == "trailing characters after JSON value", "json error msg literal trailing")

    def err_lit_trunc = json_try_decode("tru")
    assert(dict_get(err_lit_trunc, "ok", true) == false, "json error literal trunc")
    assert(dict_get(err_lit_trunc, "error", "") == "unexpected end while parsing literal", "json error msg literal trunc")

    def err_obj_key = json_try_decode("{:1}")
    assert(dict_get(err_obj_key, "ok", true) == false, "json error obj missing key")
    assert(dict_get(err_obj_key, "error", "") == "expected string key", "json error msg obj missing key")

    def err_obj_colon = json_try_decode("{\"a\" 1}")
    assert(dict_get(err_obj_colon, "ok", true) == false, "json error obj missing colon")
    assert(dict_get(err_obj_colon, "error", "") == "expected ':' after object key", "json error msg obj missing colon")

    def err_obj_comma = json_try_decode("{\"a\":1 \"b\":2}")
    assert(dict_get(err_obj_comma, "ok", true) == false, "json error obj missing comma")
    assert(dict_get(err_obj_comma, "error", "") == "expected ',' or '}' in object", "json error msg obj missing comma")

    def err_arr_comma = json_try_decode("[1 2]")
    assert(dict_get(err_arr_comma, "ok", true) == false, "json error arr missing comma")
    assert(dict_get(err_arr_comma, "error", "") == "expected ',' or ']' in array", "json error msg arr missing comma")

    def err_str_term = json_try_decode("\"abc")
    assert(dict_get(err_str_term, "ok", true) == false, "json error str unterminated")
    assert(dict_get(err_str_term, "error", "") == "unterminated string", "json error msg str unterminated")

    def err_str_esc = json_try_decode("\"\\x\"")
    assert(dict_get(err_str_esc, "ok", true) == false, "json error str invalid escape")
    assert(dict_get(err_str_esc, "error", "") == "invalid escape sequence", "json error msg str invalid escape")

    def err_str_uni = json_try_decode("\"\\u123\"")
    assert(dict_get(err_str_uni, "ok", true) == false, "json error str short unicode")
    assert(dict_get(err_str_uni, "error", "") == "invalid unicode escape", "json error msg str short unicode")

    def err_str_ctrl = json_try_decode("\"\n\"")
    assert(dict_get(err_str_ctrl, "ok", true) == false, "json error str control char")
    assert(dict_get(err_str_ctrl, "error", "") == "invalid control character in string", "json error msg str control char")

    def err_num_lead = json_try_decode("0123")
    assert(dict_get(err_num_lead, "ok", true) == false, "json error num leading zero")
    assert(dict_get(err_num_lead, "error", "") == "leading zero in number", "json error msg num leading zero")

    def err_num_frac = json_try_decode("1.")
    assert(dict_get(err_num_frac, "ok", true) == false, "json error num invalid fraction")
    assert(dict_get(err_num_frac, "error", "") == "invalid fraction in number", "json error msg num invalid fraction")

    def err_invalid_lit = json_try_decode("truu")
    assert(dict_get(err_invalid_lit, "ok", true) == false, "json error invalid literal truu")
    assert(dict_get(err_invalid_lit, "error", "") == "invalid literal", "json error msg invalid literal truu")

    def err_unterminated_esc = json_try_decode("\"\\")
    assert(dict_get(err_unterminated_esc, "ok", true) == false, "json error unterminated escape")
    assert(dict_get(err_unterminated_esc, "error", "") == "unterminated escape sequence", "json error msg unterminated escape")

    def err_invalid_surrogate = json_try_decode("\"\\uD83D\\x\"")
    assert(dict_get(err_invalid_surrogate, "ok", true) == false, "json error invalid surrogate \\x")
    assert(dict_get(err_invalid_surrogate, "error", "") == "invalid unicode surrogate pair", "json error msg invalid surrogate \\x")

    def err_invalid_surrogate_high = json_try_decode("\"\\uD83D\\uD83D\"")
    assert(dict_get(err_invalid_surrogate_high, "ok", true) == false, "json error invalid surrogate high high")
    assert(dict_get(err_invalid_surrogate_high, "error", "") == "invalid unicode surrogate pair", "json error msg invalid surrogate high high")

    def err_invalid_num_minus = json_try_decode("-")
    assert(dict_get(err_invalid_num_minus, "ok", true) == false, "json error invalid number minus")
    assert(dict_get(err_invalid_num_minus, "error", "") == "invalid number", "json error msg invalid number minus")

    def err_invalid_num_minus_char = json_try_decode("-a")
    assert(dict_get(err_invalid_num_minus_char, "ok", true) == false, "json error invalid number minus char")
    assert(dict_get(err_invalid_num_minus_char, "error", "") == "invalid number", "json error msg invalid number minus char")

    def err_invalid_exp = json_try_decode("1e")
    assert(dict_get(err_invalid_exp, "ok", true) == false, "json error invalid exponent")
    assert(dict_get(err_invalid_exp, "error", "") == "invalid exponent in number", "json error msg invalid exponent")

    def err_invalid_exp_plus = json_try_decode("1e+")
    assert(dict_get(err_invalid_exp_plus, "ok", true) == false, "json error invalid exponent plus")
    assert(dict_get(err_invalid_exp_plus, "error", "") == "invalid exponent in number", "json error msg invalid exponent plus")

    print("✓ std.enc.json tests passed")
}
