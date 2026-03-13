;; Keywords: data serialization yaml yml
;; YAML subset parser and generator for practical configuration files.
module std.parse.data.yaml(yaml_decode, yaml_try_decode, yaml_last_error, yaml_encode, decode, encode)
use std.core
use std.core.str as str
use std.parse.data.json as json

mut _yaml_error = ""

fn yaml_last_error(): str {
   "Returns the error from the last YAML decode attempt."
   _yaml_error
}

fn _yaml_result(any: ok, any: value, any: error, any: line): dict {
   {"ok": ok, "value": value, "error": error, "line": line}
}

fn _yaml_set_error(any: msg): int {
   if(_yaml_error.len == 0){ _yaml_error = msg }
   0
}

fn _yaml_strip_comment(str: line): str {
   mut quote = 0
   mut i = 0
   while(i < line.len){
      def c = load8(line, i)
      if(quote != 0){
         if(c == quote){ quote = 0 }
      } elif(c == 34 || c == 39){
         quote = c
      } elif(c == 35){
         return str.str_slice(line, 0, i)
      }
      i += 1
   }
   line
}

fn _yaml_unquote(str: v): str {
   if(v.len >= 2){
      def a, b = load8(v, 0), load8(v, v.len - 1)
      if((a == 34 && b == 34) || (a == 39 && b == 39)){ return str.str_slice(v, 1, v.len - 1) }
   }
   v
}

fn _yaml_numeric_like(str: v): bool {
   if(v.len == 0){ return false }
   mut i = 0
   if(load8(v, 0) == 45){ i = 1 }
   if(i >= v.len){ return false }
   mut saw_digit = false
   mut dots = 0
   while(i < v.len){
      def c = load8(v, i)
      if(c >= 48 && c <= 57){ saw_digit = true } elif(c == 46){
         dots += 1
         if(dots > 1){ return false }
      } else {
         return false
      }
      i += 1
   }
   saw_digit
}

fn _yaml_scalar(any: raw): any {
   mut v = str.strip(raw)
   if(v.len == 0){ return "" }
   def lo = str.lower(v)
   if(lo == "true" || lo == "yes" || lo == "on"){ return true }
   if(lo == "false" || lo == "no" || lo == "off"){ return false }
   if(lo == "null" || lo == "~"){ return 0 }
   if(load8(v, 0) == 91 && load8(v, v.len - 1) == 93){
      def inner = str.str_slice(v, 1, v.len - 1)
      def parts = str.split(inner, ",")
      mut out = list(parts.len)
      mut i = 0
      while(i < parts.len){
         out = out.append(_yaml_scalar(parts.get(i)))
         i += 1
      }
      return out
   }
   if(_yaml_numeric_like(v)){
      if(str.find(v, ".") >= 0){ return str.atof(v) }
      return str.atoi(v)
   }
   _yaml_unquote(v)
}

fn yaml_try_decode(any: src): dict {
   "Decodes a practical YAML subset: mappings, scalar values, flat lists, and `[a, b]` arrays."
   _yaml_error = ""
   if(!is_str(src)){ return _yaml_result(false, 0, "yaml input must be a string", 0) }
   def lines = str.split(src, "\n")
   mut root = dict(16)
   mut root_list = list()
   mut list_mode = false
   mut current_key = ""
   mut line_no = 0
   mut i = 0
   while(i < lines.len){
      line_no = i + 1
      mut raw = _yaml_strip_comment(lines.get(i))
      if(str.strip(raw).len == 0){ i += 1 continue }
      mut leading = 0
      while(leading < raw.len && load8(raw, leading) == 32){ leading += 1 }
      def line = str.strip(raw)
      if(str.startswith(line, "- ")){
         def item = _yaml_scalar(str.str_slice(line, 2, line.len))
         if(current_key.len > 0 && !list_mode){
            mut xs = root.get(current_key, list())
            xs = xs.append(item)
            root[current_key] = xs
         } else {
            root_list = root_list.append(item)
            list_mode = true
         }
         i += 1
         continue
      }
      def colon = str.find(line, ":")
      if(colon < 0){
         _yaml_set_error("expected key: value")
         return _yaml_result(false, 0, _yaml_error, line_no)
      }
      def key = _yaml_unquote(str.strip(str.str_slice(line, 0, colon)))
      def val_text = str.strip(str.str_slice(line, colon + 1, line.len))
      if(key.len == 0){
         _yaml_set_error("empty mapping key")
         return _yaml_result(false, 0, _yaml_error, line_no)
      }
      if(val_text.len == 0){
         root[key] = list()
         current_key = key
         list_mode = false
      } else {
         root[key] = _yaml_scalar(val_text)
         current_key = ""
         list_mode = false
      }
      i += 1
   }
   if(list_mode && root.len == 0){ return _yaml_result(true, root_list, "", 0) }
   _yaml_result(true, root, "", 0)
}

fn yaml_decode(any: src): any {
   "Decodes YAML and returns 0 on error; inspect yaml_last_error() for details."
   def res = yaml_try_decode(src)
   _yaml_error = res.get("error", "")
   if(!res.get("ok", false)){ return 0 }
   res.get("value")
}

fn _yaml_encode_scalar(any: v): str {
   if(type(v) == "bool"){
      if(v){ return "true" }
      return "false"
   }
   if(is_int(v) || is_float(v)){ return to_str(v) }
   if(v == 0){ return "null" }
   if(is_list(v)){
      mut out = Builder(32)
      out = builder_append(out, "[")
      mut i = 0
      while(i < v.len){
         out = builder_append(out, _yaml_encode_scalar(v.get(i)))
         if(i + 1 < v.len){ out = builder_append(out, ", ") }
         i += 1
      }
      out = builder_append(out, "]")
      def s = builder_to_str(out)
      builder_free(out)
      return s
   }
   json.json_encode(to_str(v))
}

fn yaml_encode(any: value): str {
   "Encodes dicts/lists/scalars as a readable YAML subset."
   if(is_dict(value)){
      mut out = Builder(128)
      def items = dict_items(value)
      mut i = 0
      while(i < items.len){
         def p, k = items.get(i), to_str(p.get(0))
         def v = p.get(1)
         if(is_list(v)){
            out = builder_append(out, k + ":\n")
            mut j = 0
            while(j < v.len){
               out = builder_append(out, "  - " + _yaml_encode_scalar(v.get(j)) + "\n")
               j += 1
            }
         } else {
            out = builder_append(out, k + ": " + _yaml_encode_scalar(v) + "\n")
         }
         i += 1
      }
      def s = builder_to_str(out)
      builder_free(out)
      return s
   }
   if(is_list(value)){
      mut out = Builder(128)
      mut i = 0
      while(i < value.len){
         out = builder_append(out, "- " + _yaml_encode_scalar(value.get(i)) + "\n")
         i += 1
      }
      def s = builder_to_str(out)
      builder_free(out)
      return s
   }
   _yaml_encode_scalar(value) + "\n"
}

fn decode(any: src): any { yaml_decode(src) }

fn encode(any: value): str { yaml_encode(value) }
