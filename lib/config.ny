;; Keywords: config schema toml validation
;; Typed configuration schemas with validation and canonical TOML output.
;; References:
;; - core
;; - core.math.parse.data.toml
module core.config(field, required, optional, defaulted, table, schema,
   validate, parse, encode)
use core
use core.str as str
use core.math.parse.data.toml as toml
use core.math.parse.data.json as json

def _SCHEMA = "nytrix.config"

fn field(str name, str type_name, bool required=true,
   bool has_default=false, any default=nil) dict {
   "Declares one typed configuration field."
   {"name":name, "type":type_name, "required":required,
      "has_default":has_default, "default":default}
}

fn required(str name, str type_name) dict {
   "Declares a required field without a default."
   field(name, type_name, true, false)
}

fn optional(str name, str type_name) dict {
   "Declares an optional field without a default."
   field(name, type_name, false, false)
}

fn defaulted(str name, str type_name, any value) dict {
   "Declares a field whose default value is used when the key is absent."
   field(name, type_name, false, true, value)
}

fn _error(str code, str path, str message) dict {
   {"code":code, "path":path, "message":message}
}

fn _valid_type(str type_name) bool {
   type_name == "any" || type_name == "bool" || type_name == "int" ||
   type_name == "float" || type_name == "str" || type_name == "list" ||
   type_name == "dict"
}

fn _matches_type(str expected, any value) bool {
   if expected == "any" {
      return type(value) == "bool" || is_int(value) || is_float(value) ||
      is_str(value) || is_list(value) || is_dict(value)
   }
   if expected == "bool" { return type(value) == "bool" }
   if expected == "int" { return is_int(value) && type(value) != "bool" }
   if expected == "float" { return is_float(value) }
   if expected == "str" { return is_str(value) }
   if expected == "list" { return is_list(value) }
   if expected == "dict" { return is_dict(value) }
   false
}

fn _is_flag(any value) bool {
   type(value) == "bool" ||
   (is_int(value) && type(value) != "bool" && (value == 0 || value == 1))
}

fn _toml_text_supported(str value) bool {
   mut i = 0
   while i < value.len {
      def first = load8(value, i)
      if first < 128 {
         if first == 0 || first == 127 {
            return false
         }
         i += 1
      } elif first >= 194 && first <= 223 {
         if i + 1 >= value.len { return false }
         def second = load8(value, i + 1)
         if second < 128 || second > 191 { return false }
         i += 2
      } elif first == 224 {
         if i + 2 >= value.len { return false }
         def second = load8(value, i + 1)
         def third = load8(value, i + 2)
         if second < 160 || second > 191 || third < 128 || third > 191 {
            return false
         }
         i += 3
      } elif (first >= 225 && first <= 236) ||
      (first >= 238 && first <= 239) {
         if i + 2 >= value.len { return false }
         def second = load8(value, i + 1)
         def third = load8(value, i + 2)
         if second < 128 || second > 191 || third < 128 || third > 191 {
            return false
         }
         i += 3
      } elif first == 237 {
         if i + 2 >= value.len { return false }
         def second = load8(value, i + 1)
         def third = load8(value, i + 2)
         if second < 128 || second > 159 || third < 128 || third > 191 {
            return false
         }
         i += 3
      } elif first == 240 {
         if i + 3 >= value.len { return false }
         def second = load8(value, i + 1)
         def third = load8(value, i + 2)
         def fourth = load8(value, i + 3)
         if second < 144 || second > 191 || third < 128 || third > 191 ||
         fourth < 128 || fourth > 191 {
            return false
         }
         i += 4
      } elif first >= 241 && first <= 243 {
         if i + 3 >= value.len { return false }
         def second = load8(value, i + 1)
         def third = load8(value, i + 2)
         def fourth = load8(value, i + 3)
         if second < 128 || second > 191 || third < 128 || third > 191 ||
         fourth < 128 || fourth > 191 {
            return false
         }
         i += 4
      } elif first == 244 {
         if i + 3 >= value.len { return false }
         def second = load8(value, i + 1)
         def third = load8(value, i + 2)
         def fourth = load8(value, i + 3)
         if second < 128 || second > 143 || third < 128 || third > 191 ||
         fourth < 128 || fourth > 191 {
            return false
         }
         i += 4
      } else {
         return false
      }
   }
   true
}

fn schema(list fields, bool reject_unknown=true) dict {
   "Creates a versioned schema from field descriptors."
   mut errors = []
   mut names = dict(8)
   mut i = 0
   while i < fields.len {
      def f = fields.get(i)
      if !is_dict(f) {
         errors = errors.append(_error("E_CONFIG_SCHEMA", "",
               "field descriptor must be a dict"))
         i += 1
         continue
      }
      def raw_name = f.get("name")
      def raw_kind = f.get("type")
      def name = is_str(raw_name) ? raw_name : ""
      def kind = is_str(raw_kind) ? raw_kind : ""
      if !is_str(raw_name) {
         errors = errors.append(_error("E_CONFIG_SCHEMA", "",
               "field name must be a string"))
      }
      if name.len == 0 {
         errors = errors.append(_error("E_CONFIG_SCHEMA", "",
               "field name must not be empty"))
      } else if names.contains(name) {
         errors = errors.append(_error("E_CONFIG_SCHEMA", name,
               "duplicate schema field"))
      } else {
         names[name] = true
      }
      if !_valid_type(kind) {
         errors = errors.append(_error("E_CONFIG_SCHEMA", name,
               "unsupported field type '" + kind + "'"))
      }
      if !is_str(raw_kind) {
         errors = errors.append(_error("E_CONFIG_SCHEMA", name,
               "field type must be a string"))
      }
      def valid_flags = _is_flag(f.get("required")) &&
      _is_flag(f.get("has_default"))
      if !valid_flags {
         errors = errors.append(_error(
               "E_CONFIG_SCHEMA", name,
               "required and has_default flags must be booleans"))
      }
      def has_default = valid_flags && int(f.get("has_default")) != 0
      if has_default {
         if !_matches_type(kind, f.get("default")) {
            errors = errors.append(_error(
                  "E_CONFIG_SCHEMA", name,
                  "default does not match field type '" + kind + "'"))
         }
         if int(f.get("required")) != 0 {
            errors = errors.append(_error(
                  "E_CONFIG_SCHEMA", name,
                  "a defaulted field cannot also be required"))
         }
      }
      if f.contains("nested_schema") &&
      (kind != "dict" || !_schema_valid(f.get("nested_schema"))) {
         errors = errors.append(_error(
               "E_CONFIG_SCHEMA", name,
               "nested schema requires a valid dict field"))
      }
      i += 1
   }
   mut result = dict(5)
   result["schema"] = _SCHEMA
   result["version"] = 1
   result["fields"] = fields
   result["reject_unknown"] = reject_unknown
   result["errors"] = errors
   result
}

fn table(str name, list fields, bool required=true) dict {
   "Declares a table field checked against a nested schema."
   mut f = field(name, "dict", required, false)
   f["nested_schema"] = schema(fields)
   f
}

fn _schema_valid_depth(any spec, int depth) bool {
   if depth > 32 || !is_dict(spec) || spec.get("schema", "") != _SCHEMA ||
   !_matches_type("int", spec.get("version")) ||
   int(spec.get("version")) != 1 ||
   !is_list(spec.get("fields", [])) ||
   !is_list(spec.get("errors", [])) || spec.get("errors", []).len != 0 ||
   !_is_flag(spec.get("reject_unknown")) {
      return false
   }
   def fields = spec.get("fields")
   mut names = dict(max(fields.len * 2, 8))
   mut i = 0
   while i < fields.len {
      def f = fields.get(i)
      if !is_dict(f) || !is_str(f.get("name")) ||
      !is_str(f.get("type")) ||
      str(f.get("name")) == "" ||
      names.contains(str(f.get("name"))) ||
      !_valid_type(str(f.get("type"))) ||
      !_is_flag(f.get("required")) ||
      !_is_flag(f.get("has_default")) {
         return false
      }
      def name = str(f.get("name"))
      def kind = str(f.get("type"))
      names[name] = true
      def has_default = int(f.get("has_default")) != 0
      if has_default &&
      (int(f.get("required")) != 0 ||
         !_matches_type(kind, f.get("default"))) {
         return false
      }
      if f.contains("nested_schema") &&
      (kind != "dict" ||
         !_schema_valid_depth(f.get("nested_schema"), depth + 1)) {
         return false
      }
      i += 1
   }
   true
}

fn _schema_valid(any spec) bool {
   _schema_valid_depth(spec, 0)
}

fn _has_field(list fields, str name) bool {
   mut i = 0
   while i < fields.len {
      if str(fields.get(i).get("name", "")) == name { return true }
      i += 1
   }
   false
}

fn validate(any values, any spec) dict {
   "Validates decoded values and applies defaults without partial success."
   if !_schema_valid(spec) {
      mut invalid = [_error("E_CONFIG_SCHEMA", "", "invalid configuration schema")]
      if is_dict(spec) && is_list(spec.get("errors", [])) &&
      spec.get("errors", []).len > 0 {
         invalid = spec.get("errors")
      }
      return {"ok":false, "value":nil, "errors":invalid}
   }
   if !is_dict(values) {
      return {"ok":false, "value":nil,
         "errors":[_error("E_CONFIG_ROOT", "",
               "configuration root must be a table") ]}
   }
   def fields = spec.get("fields")
   mut output = dict(max(fields.len * 2, 8))
   mut errors = []
   mut i = 0
   while i < fields.len {
      def f = fields.get(i)
      def name = str(f.get("name", ""))
      def kind = str(f.get("type", ""))
      mut present = values.contains(name)
      mut value = nil
      if present {
         value = values.get(name)
      } else if int(f.get("has_default", 0)) != 0 {
         value = f.get("default")
         present = true
      } else if int(f.get("required", 1)) == 0 {
         present = false
      } else {
         errors = errors.append(_error("E_CONFIG_MISSING", name,
               "missing required field '" + name + "'"))
      }
      if present {
         if f.contains("nested_schema") && _matches_type("dict", value) {
            def nested = validate(value, f.get("nested_schema"))
            if nested.get("ok", false) {
               output[name] = nested.get("value")
            } else {
               def nested_errors = nested.get("errors", [])
               mut e = 0
               while e < nested_errors.len {
                  def err = nested_errors.get(e)
                  def path = str(err.get("path", ""))
                  err["path"] = path.len > 0 ? name + "." + path : name
                  errors = errors.append(err)
                  e += 1
               }
            }
         } elif _matches_type(kind, value) {
            output[name] = value
         } else {
            errors = errors.append(_error(
                  "E_CONFIG_TYPE", name,
                  "field '" + name + "' expects " + kind + ", got " + type(value)))
         }
      }
      i += 1
   }
   if int(spec.get("reject_unknown", 1)) != 0 {
      def keys = sorted(dict_keys(values))
      mut k = 0
      while k < keys.len {
         def name = str(keys.get(k))
         if !_has_field(fields, name) {
            errors = errors.append(_error("E_CONFIG_UNKNOWN", name,
                  "unknown configuration field '" + name + "'"))
         }
         k += 1
      }
   }
   if errors.len > 0 { return {"ok":false, "value":nil, "errors":errors} }
   {"ok":true, "value":output, "errors":[]}
}

fn _line_start(str source, int line) int {
   if line <= 1 { return 0 }
   mut current = 1
   mut i = 0
   while i < source.len && current < line {
      if load8(source, i) == 10 { current += 1 }
      i += 1
   }
   i
}

fn _line_end(str source, int start) int {
   mut i = start
   while i < source.len && load8(source, i) != 10 && load8(source, i) != 13 {
      i += 1
   }
   i
}

fn parse(str source, any spec, int max_bytes=1048576) dict {
   "Parses TOML, validates it against a schema, and applies schema defaults."
   if source.len > max_bytes {
      def limit = _error("E_CONFIG_LIMIT", "", "configuration byte limit exceeded")
      limit["span"] = {"start":0, "end":0}
      limit["line"] = 1
      limit["column"] = 1
      return {"ok":false, "value":nil, "errors":[limit]}
   }
   def decoded = toml.toml_try_decode(source)
   if !decoded.get("ok", false) {
      mut line = int(decoded.get("line", 1))
      if line < 1 { line = 1 }
      def start = _line_start(source, line)
      def end = _line_end(source, start)
      def syntax = _error("E_CONFIG_SYNTAX", "", str(decoded.get("error", "invalid TOML")))
      syntax["span"] = {"start":start, "end":end}
      syntax["line"] = line
      syntax["column"] = 1
      return {"ok":false, "value":nil, "errors":[syntax]}
   }
   validate(decoded.get("value"), spec)
}

fn _bare_key(str name) bool {
   if name.len == 0 { return false }
   mut i = 0
   while i < name.len {
      def c = load8(name, i)
      if !((c >= 48 && c <= 57) || (c >= 65 && c <= 90) ||
         (c >= 97 && c <= 122) || c == 95 || c == 45) {
         return false
      }
      i += 1
   }
   true
}

fn _key(str name) str {
   if _bare_key(name) { return name }
   json.json_encode(name)
}

fn _value_supported(any value, int depth, bool in_array=false) bool {
   if depth > 32 { return false }
   if type(value) == "bool" || is_int(value) || is_float(value) {
      return true
   }
   if is_str(value) { return _toml_text_supported(value) }
   if is_dict(value) {
      if in_array { return false }
      def keys = sorted(dict_keys(value))
      mut i = 0
      while i < keys.len {
         def key = keys.get(i)
         if !is_str(key) || !_toml_text_supported(str(key)) ||
         !_value_supported(value.get(key), depth + 1) {
            return false
         }
         i += 1
      }
      return true
   }
   if is_list(value) {
      mut i = 0
      while i < value.len {
         if !_value_supported(value.get(i), depth + 1, true) { return false }
         i += 1
      }
      return true
   }
   false
}

fn _value_text(any value) str {
   if type(value) == "bool" {
      if value { return "true" }
      return "false"
   }
   if is_int(value) || is_float(value) { return to_str(value) }
   if is_str(value) { return json.json_encode(value) }
   if is_list(value) {
      mut out = Builder(32)
      out = builder_append(out, "[")
      mut i = 0
      while i < value.len {
         if i > 0 { out = builder_append(out, ", ") }
         out = builder_append(out, _value_text(value.get(i)))
         i += 1
      }
      out = builder_append(out, "]")
      def text = builder_to_str(out)
      builder_free(out)
      return text
   }
   ""
}

fn _section_text(dict table, str path) str {
   def keys = sorted(dict_keys(table))
   mut out = Builder(128)
   out = builder_append(out, "\n[" + path + "]\n")
   mut i = 0
   while i < keys.len {
      def key = str(keys.get(i))
      def value = table.get(key)
      if !is_dict(value) {
         out = builder_append(out,
            _key(key) + " = " + _value_text(value) + "\n")
      }
      i += 1
   }
   i = 0
   while i < keys.len {
      def key = str(keys.get(i))
      def value = table.get(key)
      if is_dict(value) {
         out = builder_append(out,
            _section_text(value, path + "." + _key(key)))
      }
      i += 1
   }
   def text = builder_to_str(out)
   builder_free(out)
   text
}

fn encode(any values, any spec) dict {
   "Validates values and emits canonical TOML with schema field order."
   def result = validate(values, spec)
   if !result.get("ok", false) {
      return {"ok":false, "text":nil, "errors":result.get("errors", [])}
   }
   def normalized = result.get("value")
   def fields = spec.get("fields")
   mut i = 0
   while i < fields.len {
      def f = fields.get(i)
      def name = str(f.get("name", ""))
      def value = normalized.get(name)
      if !is_nil(value) &&
      (!_toml_text_supported(name) || !_value_supported(value, 0)) {
         return {"ok":false, "text":nil,
            "errors":[_error("E_CONFIG_VALUE", name,
                  "value cannot be represented as TOML") ]}
      }
      i += 1
   }
   mut out = Builder(128)
   i = 0
   while i < fields.len {
      def f = fields.get(i)
      def name = str(f.get("name", ""))
      def value = normalized.get(name)
      if !is_nil(value) && !is_dict(value) {
         out = builder_append(out,
            _key(name) + " = " + _value_text(value) + "\n")
      }
      i += 1
   }
   i = 0
   while i < fields.len {
      def f = fields.get(i)
      def name = str(f.get("name", ""))
      def value = normalized.get(name)
      if is_dict(value) {
         out = builder_append(out, _section_text(value, _key(name)))
      }
      i += 1
   }
   def text = builder_to_str(out)
   builder_free(out)
   return {"ok":true, "text":text, "errors":[]}
}
