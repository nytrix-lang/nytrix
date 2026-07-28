;; Keywords: decompiler pseudocode rendering tokens renames text
;; Quote-aware token replacement and symbol normalization for rendered Ny text.
module std.os.rev.decomp.render_text *

use std.core
use std.core.str as str
use std.os.rev.decomp.annotations (_safe_name)
use std.os.rev.decomp.symbols (_symbol_is_name_char)

fn _clean_token_char(int c) bool { str.ascii_is_alnum(c) || c == 95 }

fn _clean_replace_token(str text, str needle, str repl) str {
   if needle.len == 0 || text.len < needle.len { return text }
   mut b = str.Builder(text.len + repl.len)
   mut i = 0
   while i < text.len {
      if i + needle.len <= text.len && slice(text, i, i + needle.len, 1) == needle {
         def before_ok = i == 0 || !_clean_token_char(load8(text, i - 1))
         def after_ok = i + needle.len >= text.len || !_clean_token_char(load8(text, i + needle.len))
         if before_ok && after_ok {
            b = str.builder_append(b, repl)
            i += needle.len
            continue
         }
      }
      b = str.builder_append(b, chr(load8(text, i)))
      i += 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn _clean_replace_token_code(str text, str needle, str repl) str {
   if needle.len == 0 || text.len < needle.len { return text }
   mut b = str.Builder(text.len + repl.len)
   mut quote = 0
   mut i = 0
   while i < text.len {
      def ch = load8(text, i)
      if quote != 0 {
         b = str.builder_append(b, slice(text, i, i + 1, 1))
         if ch == 92 && i + 1 < text.len {
            i += 1
            b = str.builder_append(b, slice(text, i, i + 1, 1))
         } elif ch == quote { quote = 0 }
         i += 1
         continue
      }
      if ch == 34 || ch == 39 {
         quote = ch
         b = str.builder_append(b, slice(text, i, i + 1, 1))
         i += 1
         continue
      }
      if i + needle.len <= text.len && slice(text, i, i + needle.len, 1) == needle {
         def before_ok = i == 0 || !_clean_token_char(load8(text, i - 1))
         def after_ok = i + needle.len >= text.len || !_clean_token_char(load8(text, i + needle.len))
         if before_ok && after_ok {
            b = str.builder_append(b, repl)
            i += needle.len
            continue
         }
      }
      b = str.builder_append(b, slice(text, i, i + 1, 1))
      i += 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn _clean_apply_render_renames(str text, any renames0) str {
   if !is_dict(renames0) || renames0.len == 0 { return text }
   mut out = text
   def keys = renames0.keys()
   mut i = 0
   while i < keys.len {
      def old = to_str(keys[i])
      def fresh = _safe_name(to_str(renames0.get(old, "")), "")
      if str.startswith(old, "local_") && fresh.len > 0 && fresh != old { out = _clean_replace_token_code(out, old, fresh) }
      i += 1
   }
   out
}

fn _clean_normalize_rip_relative_data_symbols(str text) str {
   if str.find(text, "[rip]") < 0 { return text }
   mut b = str.Builder(text.len)
   mut changed = false
   mut quote = 0
   mut i = 0
   while i < text.len {
      def ch = load8(text, i)
      if quote != 0 {
         b = str.builder_append(b, slice(text, i, i + 1, 1))
         if ch == 92 && i + 1 < text.len {
            i += 1
            b = str.builder_append(b, slice(text, i, i + 1, 1))
         } elif ch == quote { quote = 0 }
         i += 1
         continue
      }
      if ch == 34 || ch == 39 {
         quote = ch
         b = str.builder_append(b, slice(text, i, i + 1, 1))
         i += 1
         continue
      }
      def is_data = i + 5 <= text.len && slice(text, i, i + 5, 1) == "data_"
      def is_string = i + 4 <= text.len && slice(text, i, i + 4, 1) == "str_"
      if is_data || is_string {
         def start = i
         while i < text.len && _symbol_is_name_char(load8(text, i)) { i += 1 }
         def name = slice(text, start, i, 1)
         if i + 5 <= text.len && slice(text, i, i + 5, 1) == "[rip]" {
            b = str.builder_append(b, name)
            i += 5
            changed = true
            continue
         }
         b = str.builder_append(b, name)
         continue
      }
      b = str.builder_append(b, slice(text, i, i + 1, 1))
      i += 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   changed ? out : text
}

#main {
   assert(_clean_replace_token("rax + rax1", "rax", "value") == "value + rax1", "token boundary")
   assert(_clean_replace_token_code("rax + \"rax\"", "rax", "value") == "value + \"rax\"", "quoted token")
   assert(_clean_normalize_rip_relative_data_symbols("data_flag[rip] = str_ok[rip]") == "data_flag = str_ok", "rip symbols")
   print("✓ std.os.rev.decomp.render_text self-test passed")
}
