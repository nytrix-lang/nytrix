;; Keywords: str json
;; JSON parser and generator.

module std.str.json (
    json_decode, json_encode
)
use std.core *
use std.str as str

;; Decoder State: [string, length, position]
fn _json_skip_ws(st){
    "Internal: Skips ASCII whitespace in the JSON source."
    def s = get(st, 0)
    def n = get(st, 1)
    mut pos = get(st, 2)
    while(pos < n){
        def c = load8(s, pos)
        if(c == 32 || c == 9 || c == 10 || c == 13){
            pos = pos + 1
        } else { break }
    }
    set_idx(st, 2, pos)
}

fn _json_parse_val(st){
    "Internal: Main recursive descent parser for JSON values."
    _json_skip_ws(st)
    def s = get(st, 0)
    def n = get(st, 1)
    def pos = get(st, 2)
    if(pos >= n){ return 0 }
    def c = load8(s, pos)
    if(c == 123){ return _json_parse_obj(st) } ;; '{'
    if(c == 91){ return _json_parse_arr(st) }  ;; '['
    if(c == 34){ return _json_parse_str(st) }  ;; '"'
    if(c == 116){ ;; 't' (true)
        set_idx(st, 2, pos + 4)
        return 1
    }
    if(c == 102){ ;; 'f' (false)
        set_idx(st, 2, pos + 5)
        return 0
    }
    if(c == 110){ ;; 'n' (null)
        set_idx(st, 2, pos + 4)
        return 0
    }
    if(c == 45 || (c >= 48 && c <= 57)){ return _json_parse_num(st) }
    return 0
}

fn _json_parse_obj(st){
    "Internal: Parses a JSON object into a Nytrix dictionary."
    set_idx(st, 2, get(st, 2) + 1) ;; skip '{'
    def d = dict(8)
    def s = get(st, 0)
    def n = get(st, 1)
    while(1){
        _json_skip_ws(st)
        mut pos = get(st, 2)
        if(pos >= n || load8(s, pos) == 125){ ;; '}'
            if(pos < n){ set_idx(st, 2, pos + 1) }
            return d
        }
        def key = _json_parse_str(st)
        _json_skip_ws(st)
        set_idx(st, 2, get(st, 2) + 1) ;; skip ':'
        def val = _json_parse_val(st)
        dict_set(d, key, val)
        _json_skip_ws(st)
        pos = get(st, 2)
        if(pos < n && load8(s, pos) == 44){ ;; ','
            set_idx(st, 2, pos + 1)
        }
    }
}

fn _json_parse_arr(st){
    "Internal: Parses a JSON array into a Nytrix list."
    set_idx(st, 2, get(st, 2) + 1) ;; skip '['
    def l = list(8)
    def s = get(st, 0)
    def n = get(st, 1)
    while(1){
        _json_skip_ws(st)
        mut pos = get(st, 2)
        if(pos >= n || load8(s, pos) == 93){ ;; ']'
            if(pos < n){ set_idx(st, 2, pos + 1) }
            return l
        }
        append(l, _json_parse_val(st))
        _json_skip_ws(st)
        pos = get(st, 2)
        if(pos < n && load8(s, pos) == 44){ ;; ','
            set_idx(st, 2, pos + 1)
        }
    }
}

fn _json_parse_str(st){
    "Internal: Parses a JSON string literal."
    mut pos = get(st, 2)
    def s = get(st, 0)
    def n = get(st, 1)
    pos = pos + 1 ;; skip '"'
    mut start = pos
    while(pos < n && load8(s, pos) != 34){
        if(load8(s, pos) == 92){ pos = pos + 1 } ;; skip escape
        pos = pos + 1
    }
    def len = pos - start
    def res = malloc(len + 1)
    __copy_mem(res, s + start, len)
    store8(res, 0, len)
    init_str(res, len)
    set_idx(st, 2, pos + 1) ;; skip ending '"'
    return res
}

fn _json_parse_num(st){
    "Internal: Parses a JSON number literal."
    mut pos = get(st, 2)
    def s = get(st, 0)
    def n = get(st, 1)
    mut start = pos
    if(load8(s, pos) == 45){ pos = pos + 1 }
    while(pos < n && load8(s, pos) >= 48 && load8(s, pos) <= 57){ pos = pos + 1 }
    if(pos < n && load8(s, pos) == 46){ ;; '.'
        pos = pos + 1
        while(pos < n && load8(s, pos) >= 48 && load8(s, pos) <= 57){ pos = pos + 1 }
    }
    def len = pos - start
    def tmp = malloc(len + 1)
    __copy_mem(tmp, s + start, len)
    store8(tmp, 0, len)
    init_str(tmp, len)
    def res = str.atoi(tmp)
    free(tmp)
    set_idx(st, 2, pos)
    return res
}

fn json_decode(s){
    "Decodes a JSON string into Nytrix objects (dictionaries, lists, strings, and numbers)."
    if(!is_str(s)){ return 0 }
    def st = [s, str.len(s), 0]
    _json_parse_val(st)
}

fn json_encode(obj){
    "Encodes Nytrix objects into their JSON string representation."
    if(obj == 0){ return "null" }
    if(is_int(obj)){ 
        if(obj == 2){ return "true" }
        if(obj == 4){ return "false" }
        return to_str(obj) 
    }
    if(is_str(obj)){ return str.str_add(str.str_add("\"", obj), "\"") }
    if(is_list(obj)){
        mut res = "["
        def n = len(obj)
        mut i = 0
        while(i < n){
            res = str.str_add(res, json_encode(get(obj, i)))
            if(i < n - 1){ res = str.str_add(res, ",") }
            i = i + 1
        }
        return str.str_add(res, "]")
    }
    if(is_dict(obj)){
        mut res = "{"
        ; For now, handle empty object; full dictionary encoding may need iteration over items
        return str.str_add(res, "}")
    }
    return "null"
}
