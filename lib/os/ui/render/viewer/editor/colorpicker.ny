;; Keywords: editor colorpicker rgba hex swatch os ui render viewer text
;; RGBA and hex color picker helpers for editor and inspector color controls.
;; References:
;; - std.os.ui.render.viewer.gui
module std.os.ui.render.viewer.editor.colorpicker(
   state, open, close, is_open,
   hex, line, start, end, rgb, rgb_label,
   normalize, scan_hex_literal, at_cursor, swatches_visible,
   rgba, rgba_hex, same_rgba, changed, edit4, picker4
)

use std.core
use std.core.str as str
use std.math (clamp, max, min)
use std.os.ui.render.viewer.gui as gui

fn state() dict {
   {"open": false, "hex": "", "line": 0, "start": 0, "end": 0}
}

fn open(dict st, dict hit) dict {
   st = hit
   st["open"] = true
   st
}

fn close(dict st) dict { st["open"] = false st }

fn is_open(dict st) bool { bool(st.get("open", false)) }

fn hex(dict st) str { to_str(st.get("hex", "")) }

fn line(dict st) int { int(st.get("line", 0)) }

fn start(dict st) int { int(st.get("start", 0)) }

fn end(dict st) int { int(st.get("end", 0)) }

fn _hex_digit_value(int c) int {
   if(c >= 48 && c <= 57){ return c - 48 }
   if(c >= 65 && c <= 70){ return c - 55 }
   if(c >= 97 && c <= 102){ return c - 87 }
   -1
}

fn _is_hex_digit(int c) bool { _hex_digit_value(c) >= 0 }

fn normalize(str text) str {
   if(text.len <= 0 || load8(text, 0) != 35){ return "" }
   def digits = text.len - 1
   if(digits == 3){
      if(!_is_hex_digit(load8(text, 1)) || !_is_hex_digit(load8(text, 2)) || !_is_hex_digit(load8(text, 3))){ return "" }
      def r = str.str_slice(text, 1, 2)
      def g = str.str_slice(text, 2, 3)
      def b = str.str_slice(text, 3, 4)
      return "#" + r + r + g + g + b + b
   }
   if(digits == 6 || digits == 8){
      mut i = 1
      while(i < text.len){
         if(!_is_hex_digit(load8(text, i))){ return "" }
         i += 1
      }
      return str.str_slice(text, 0, 7)
   }
   ""
}

fn _hit(str line_text, int i, int j, int row) dict {
   def hx = normalize(str.str_slice(line_text, i, j))
   hx.len <= 0 ? dict(0) : {"hex": hx, "line": row, "start": i, "end": j}
}

fn scan_hex_literal(str line_text, int pos, int row=0) dict {
   mut i = 0
   while(i < line_text.len){
      if(load8(line_text, i) != 35){ i += 1 continue }
      mut j = i + 1
      while(j < line_text.len && _is_hex_digit(load8(line_text, j))){ j += 1 }
      def digits = j - i - 1
      if((digits == 3 || digits == 6 || digits == 8) && pos >= i && pos <= j){
         return _hit(line_text, i, j, row)
      }
      i = max(i + 1, j)
   }
   dict(0)
}

fn at_cursor(list lines, int row, int col) dict {
   if(lines.len <= 0){ return dict(0) }
   def r = min(max(row, 0), lines.len - 1)
   def line_text = to_str(lines.get(r, ""))
   scan_hex_literal(line_text, min(max(col, 0), line_text.len), r)
}

fn _clip_line(str line_text, int limit) str {
   if(line_text.len <= limit){ return line_text }
   str.str_slice(line_text, 0, max(0, limit - 3)) + "..."
}

fn _append_line_swatches(list out, str line_text, int row) list {
   mut i = 0
   while(i < line_text.len){
      if(load8(line_text, i) != 35){ i += 1 continue }
      mut j = i + 1
      while(j < line_text.len && _is_hex_digit(load8(line_text, j))){ j += 1 }
      def digits = j - i - 1
      if(digits == 3 || digits == 6 || digits == 8){
         def hit = _hit(line_text, i, j, row)
         if(hit.len > 0){ out = out.append(hit) }
      }
      i = max(i + 1, j)
   }
   out
}

fn swatches_visible(list lines, int scroll, int rows, int draw_limit) list {
   mut out = []
   mut row = max(0, scroll)
   def last = min(lines.len, max(0, scroll) + max(0, rows))
   while(row < last){
      out = _append_line_swatches(out, _clip_line(to_str(lines.get(row, "")), draw_limit), row)
      row += 1
   }
   out
}

fn _hex_byte(str hx, int off) int {
   _hex_digit_value(load8(hx, off)) * 16 + _hex_digit_value(load8(hx, off + 1))
}

fn rgb(str hx) list {
   hx = normalize(hx)
   hx.len < 7 ? [0, 0, 0] : [_hex_byte(hx, 1), _hex_byte(hx, 3), _hex_byte(hx, 5)]
}

fn rgb_label(str hx) str {
   def c = rgb(hx)
   "rgb(" + to_str(int(c.get(0, 0))) + ", " + to_str(int(c.get(1, 0))) + ", " + to_str(int(c.get(2, 0))) + ")"
}

fn _rgba_default_at(any fallback, int idx, f64 def_val) f64 {
   (is_list(fallback) || is_tuple(fallback)) ? clamp(float(fallback.get(idx, def_val)), 0.0, 1.0) : def_val
}

fn _rgba_fallback(any fallback) list {
   [
      _rgba_default_at(fallback, 0, 1.0),
      _rgba_default_at(fallback, 1, 1.0),
      _rgba_default_at(fallback, 2, 1.0),
      _rgba_default_at(fallback, 3, 1.0)
   ]
}

fn _byte_unit(int v) f64 { float(max(0, min(255, v))) / 255.0 }

fn _hex_pair_byte(str text, int off, int fallback) int {
   if(off + 1 >= text.len){ return fallback }
   def hi = _hex_digit_value(load8(text, off))
   def lo = _hex_digit_value(load8(text, off + 1))
   (hi < 0 || lo < 0) ? fallback : hi * 16 + lo
}

fn _hex_nibble_byte(str text, int off, int fallback) int {
   if(off >= text.len){ return fallback }
   def v = _hex_digit_value(load8(text, off))
   v < 0 ? fallback : v * 17
}

fn _rgba_from_hex(str text, any fallback) list {
   def fb = _rgba_fallback(fallback)
   if(text.len <= 1 || load8(text, 0) != 35){ return fb }
   def digits = text.len - 1
   if(digits == 3 || digits == 4){
      return [
         _byte_unit(_hex_nibble_byte(text, 1, int(float(fb.get(0, 1.0)) * 255.0 + 0.5))),
         _byte_unit(_hex_nibble_byte(text, 2, int(float(fb.get(1, 1.0)) * 255.0 + 0.5))),
         _byte_unit(_hex_nibble_byte(text, 3, int(float(fb.get(2, 1.0)) * 255.0 + 0.5))),
         digits == 4 ? _byte_unit(_hex_nibble_byte(text, 4, int(float(fb.get(3, 1.0)) * 255.0 + 0.5))) : float(fb.get(3, 1.0))
      ]
   }
   if(digits == 6 || digits == 8){
      return [
         _byte_unit(_hex_pair_byte(text, 1, int(float(fb.get(0, 1.0)) * 255.0 + 0.5))),
         _byte_unit(_hex_pair_byte(text, 3, int(float(fb.get(1, 1.0)) * 255.0 + 0.5))),
         _byte_unit(_hex_pair_byte(text, 5, int(float(fb.get(2, 1.0)) * 255.0 + 0.5))),
         digits == 8 ? _byte_unit(_hex_pair_byte(text, 7, int(float(fb.get(3, 1.0)) * 255.0 + 0.5))) : float(fb.get(3, 1.0))
      ]
   }
   fb
}

fn rgba(any color, any fallback=[1.0, 1.0, 1.0, 1.0]) list {
   "Normalizes list/tuple or hex colors to clamped RGBA floats."
   def fb = _rgba_fallback(fallback)
   if(is_str(color)){ return _rgba_from_hex(to_str(color), fb) }
   if(is_list(color) || is_tuple(color)){
      return [
         clamp(float(color.get(0, fb.get(0, 1.0))), 0.0, 1.0),
         clamp(float(color.get(1, fb.get(1, 1.0))), 0.0, 1.0),
         clamp(float(color.get(2, fb.get(2, 1.0))), 0.0, 1.0),
         clamp(float(color.get(3, fb.get(3, 1.0))), 0.0, 1.0)
      ]
   }
   fb
}

fn _unit_to_byte(any v) int {
   max(0, min(255, int(clamp(float(v), 0.0, 1.0) * 255.0 + 0.5)))
}

fn rgba_hex(any color) str {
   def c = rgba(color, [0.0, 0.0, 0.0, 1.0])
   "#" + str.to_hex(_unit_to_byte(c.get(0, 0.0)), 2) +
   str.to_hex(_unit_to_byte(c.get(1, 0.0)), 2) +
   str.to_hex(_unit_to_byte(c.get(2, 0.0)), 2)
}

fn _absf(f64 v) f64 { v < 0.0 ? -v : v }

fn same_rgba(any a, any b, f64 eps=0.0005) bool {
   def ca = rgba(a, [1.0, 1.0, 1.0, 1.0])
   def cb = rgba(b, ca)
   _absf(float(ca.get(0, 1.0)) - float(cb.get(0, 1.0))) <= eps &&
   _absf(float(ca.get(1, 1.0)) - float(cb.get(1, 1.0))) <= eps &&
   _absf(float(ca.get(2, 1.0)) - float(cb.get(2, 1.0))) <= eps &&
   _absf(float(ca.get(3, 1.0)) - float(cb.get(3, 1.0))) <= eps
}

fn changed(any before, any after, f64 eps=0.0005) bool { !same_rgba(before, after, eps) }

fn edit4(any id, any label, any color, any fallback=[1.0, 1.0, 1.0, 1.0]) list {
   rgba(gui.color_edit4(id, label, rgba(color, fallback)), fallback)
}

fn picker4(any id, any label, any color, any fallback=[1.0, 1.0, 1.0, 1.0]) list {
   rgba(gui.color_picker4(id, label, rgba(color, fallback)), fallback)
}

#main {
   assert(normalize("#acf") == "#aaccff", "short hex normalization")
   assert(normalize("#b6a0ff") == "#b6a0ff", "long hex normalization")
   assert(scan_hex_literal("bg: #b6a0ff;", 6).get("hex", "") == "#b6a0ff", "scan literal")
   assert(at_cursor(["x", "fg #acf"], 1, 5).get("hex", "") == "#aaccff", "cursor color")
   assert(swatches_visible(["#000 #ffffff", "none"], 0, 2, 120).len == 2, "visible swatches")
   assert(rgb_label("#b6a0ff") == "rgb(182, 160, 255)", "rgb label")
   assert(rgba("#80402080").get(3, 0.0) > 0.50 && rgba_hex([1, 0.5, 0]) == "#ff8000", "rgba helpers")
   assert(same_rgba("#ffffff", [1, 1, 1, 1]) && changed("#ffffff", "#000000"), "rgba compare")
   print("✓ viewer editor colorpicker test passed")
}
