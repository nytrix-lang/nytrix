;; Keywords: image svg vector parse
;; Native SVG image loader for Nytrix using librsvg + cairo.
;; References:
;; - std.parse.img
;; - std.parse
module std.parse.img.svg(decode, load_path, available, last_error, backend_name)
use std.core
use std.core.dict_mod as dict_mod
use std.core.str as str
use std.math
use std.os as os

def SVG_REF_PATH_DATA = "https://www.w3.org/TR/SVG/paths.html"
def SVG_REF_SHAPES = "https://www.w3.org/TR/SVG/shapes.html"
def SVG_REF_ARCS = "https://www.w3.org/TR/SVG/implnote.html#ArcImplementationNotes"
mut _svg_last_error = ""

fn _svg_set_error(any msg) bool {
   _svg_last_error = to_str(msg)
   false
}

fn last_error() str {
   "Returns the last native SVG decoder error."
   _svg_last_error
}

fn backend_name() str {
   "Returns the active SVG backend."
   "native-ny-svg"
}

fn available() bool {
   "Returns whether the native Ny SVG backend is available."
   true
}

fn _looks_like_svg(any data) bool {
   if !is_str(data) || data.len < 4 { return false }
   mut i = 0
   def limit = min(data.len - 3, 768)
   while i < limit {
      if load8(data, i) == 60 {
         def c1 = load8(data, i + 1) | 32
         def c2 = load8(data, i + 2) | 32
         def c3 = load8(data, i + 3) | 32
         if c1 == 115 && c2 == 118 && c3 == 103 { return true }
      }
      i += 1
   }
   false
}

fn _svg_node(str name, any attrs=0) dict {
   mut n = dict(4)
   n["name"] = name
   n["attr"] = is_dict(attrs) ? attrs : dict()
   n["children"] = []
   n
}

fn _is_space(int c) bool { c <= 32 }

fn _is_digit(int c) bool { c >= 48 && c <= 57 }

fn _is_alpha(int c) bool {
   (c >= 65 && c <= 90) || (c >= 97 && c <= 122)
}

fn _is_path_cmd(int c) bool {
   case c {
      65, 67, 72, 76, 77, 81, 83, 84, 86, 90,
      97, 99, 104, 108, 109, 113, 115, 116, 118, 122 -> true
      _ -> false
   }
}

fn _skip_sep(str s, int p) int {
   while p < s.len {
      def c = load8(s, p)
      if c <= 32 || c == 44 { p += 1 }
      else { break }
   }
   p
}

fn _skip_ws(str s, int p) int {
   while p < s.len && _is_space(load8(s, p)) { p += 1 }
   p
}

fn _attr_key_stop(int c) bool {
   c <= 32 || c == 47 || c == 61 || c == 62
}

fn _parse_tag_attrs(str s, int p) list {
   mut attrs = dict(64)
   while p < s.len {
      p = _skip_ws(s, p)
      if p >= s.len || load8(s, p) == 47 || load8(s, p) == 62 { break }
      mut kb = Builder(32)
      while p < s.len {
         def c = load8(s, p)
         if _attr_key_stop(c) { break }
         kb = builder_append(kb, chr(c))
         p += 1
      }
      def key = builder_to_str(kb)
      builder_free(kb)
      p = _skip_ws(s, p)
      if p < s.len && load8(s, p) == 61 {
         p += 1
         p = _skip_ws(s, p)
         def quote = load8(s, p)
         mut vb = Builder(64)
         if quote == 34 || quote == 39 {
            p += 1
            while p < s.len && load8(s, p) != quote {
               vb = builder_append(vb, chr(load8(s, p)))
               p += 1
            }
            if p < s.len { p += 1 }
         } else {
            while p < s.len {
               def c = load8(s, p)
               if c <= 32 || c == 47 || c == 62 { break }
               vb = builder_append(vb, chr(c))
               p += 1
            }
         }
         def val = builder_to_str(vb)
         builder_free(vb)
         if key.len > 0 { attrs[key] = val }
      } else {
         if key.len > 0 { attrs[key] = true }
      }
   }
   [attrs, p]
}

fn _parse_svg_tree(str data) any {
   mut p = 0
   mut root = 0
   mut stack = []
   while p < data.len {
      if load8(data, p) != 60 {
         p += 1
         continue
      }
      p += 1
      if p >= data.len { break }
      def c0 = load8(data, p)
      if c0 == 33 || c0 == 63 {
         while p < data.len && load8(data, p) != 62 { p += 1 }
         if p < data.len { p += 1 }
         continue
      }
      if c0 == 47 {
         while p < data.len && load8(data, p) != 62 { p += 1 }
         if p < data.len { p += 1 }
         if stack.len > 0 { stack.pop() }
         continue
      }
      mut nb = Builder(24)
      while p < data.len {
         def c = load8(data, p)
         if c <= 32 || c == 47 || c == 62 { break }
         nb = builder_append(nb, chr(c))
         p += 1
      }
      def name = builder_to_str(nb)
      builder_free(nb)
      def ar = _parse_tag_attrs(data, p)
      def attrs = ar[0]
      p = int(ar[1])
      mut self_closing = false
      p = _skip_ws(data, p)
      if p < data.len && load8(data, p) == 47 {
         self_closing = true
         p += 1
      }
      if p < data.len && load8(data, p) == 62 { p += 1 }
      if name.len == 0 { continue }
      def node = _svg_node(name, attrs)
      if !root { root = node }
      if stack.len > 0 {
         def parent = stack[stack.len - 1]
         mut children = parent.get("children", [])
         children = children.append(node)
         parent["children"] = children
      }
      if !self_closing { stack = stack.append(node) }
   }
   root
}

fn _read_num(str s, int p) list {
   p = _skip_sep(s, p)
   def start = p
   if p < s.len && (load8(s, p) == 45 || load8(s, p) == 43) { p += 1 }
   mut any_digit = false
   while p < s.len && _is_digit(load8(s, p)) { any_digit = true p += 1 }
   if p < s.len && load8(s, p) == 46 {
      p += 1
      while p < s.len && _is_digit(load8(s, p)) { any_digit = true p += 1 }
   }
   if any_digit && p < s.len && ((load8(s, p) | 32) == 101) {
      def ep = p
      p += 1
      if p < s.len && (load8(s, p) == 45 || load8(s, p) == 43) { p += 1 }
      mut ed = false
      while p < s.len && _is_digit(load8(s, p)) { ed = true p += 1 }
      if !ed { p = ep }
   }
   if !any_digit { return [false, 0.0, start] }
   [true, str.atof(str.str_slice(s, start, p)), p]
}

fn _number_list(any raw) list {
   def s = to_str(raw)
   mut xs = []
   mut p = 0
   while p < s.len {
      def r = _read_num(s, p)
      if r.get(0, false) {
         xs = xs.append(float(r.get(1, 0.0)))
         p = int(r.get(2, p + 1))
      } else {
         p += 1
      }
   }
   xs
}

fn _num(any raw, f64 fallback=0.0) f64 {
   def xs = _number_list(raw)
   xs.len > 0 ? float(xs.get(0, fallback)) : fallback
}

fn _clamp01(f64 v) f64 {
   if v < 0.0 { return 0.0 }
   if v > 1.0 { return 1.0 }
   v
}

fn _opacity(any raw, f64 fallback=1.0) f64 {
   if !raw { return fallback }
   def s = str.strip(to_str(raw))
   if s.len == 0 { return fallback }
   def v = _num(s, fallback)
   _clamp01(str.endswith(s, "%") ? (v / 100.0) : v)
}

fn _rgba(int r, int g, int b, f64 a=1.0, any is_none=false) dict {
   mut d = dict(5)
   d["r"] = max(0, min(255, r))
   d["g"] = max(0, min(255, g))
   d["b"] = max(0, min(255, b))
   d["a"] = _clamp01(a)
   d["none"] = is_none
   d
}

fn _rgba_none() dict { _rgba(0, 0, 0, 0.0, true) }

fn _hex_byte(str s, int off) int {
   def hi = str.hex_val(load8(s, off))
   def lo = str.hex_val(load8(s, off + 1))
   if hi < 0 || lo < 0 { return 0 }
   (hi << 4) | lo
}

fn _parse_color(any raw, any inherited=0) dict {
   if !raw { return is_dict(inherited) ? inherited : _rgba(0, 0, 0, 1.0) }
   mut s = str.lower(str.strip(to_str(raw)))
   if s.len == 0 { return is_dict(inherited) ? inherited : _rgba(0, 0, 0, 1.0) }
   if s == "none" { return _rgba_none() }
   if s == "transparent" { return _rgba(0, 0, 0, 0.0) }
   if str.startswith(s, "url(") { return is_dict(inherited) ? inherited : _rgba(242, 242, 242, 1.0) }
   if str.startswith(s, "#") {
      s = str.str_slice(s, 1, s.len)
      if s.len == 3 {
         def r = str.hex_val(load8(s, 0))
         def g = str.hex_val(load8(s, 1))
         def b = str.hex_val(load8(s, 2))
         return _rgba((r << 4) | r, (g << 4) | g, (b << 4) | b, 1.0)
      }
      if s.len >= 6 { return _rgba(_hex_byte(s, 0), _hex_byte(s, 2), _hex_byte(s, 4), 1.0) }
   }
   if str.startswith(s, "rgb") {
      def xs = _number_list(s)
      if xs.len >= 3 { return _rgba(int(xs[0]), int(xs[1]), int(xs[2]), 1.0) }
   }
   case s {
      "white" -> _rgba(255, 255, 255, 1.0)
      "black" -> _rgba(0, 0, 0, 1.0)
      "red" -> _rgba(255, 0, 0, 1.0)
      "green" -> _rgba(0, 128, 0, 1.0)
      "blue" -> _rgba(0, 0, 255, 1.0)
      _ -> is_dict(inherited) ? inherited : _rgba(0, 0, 0, 1.0)
   }
}

fn _color_alpha(dict c, f64 opacity=1.0) f64 {
   if bool(c.get("none", false)) { return 0.0 }
   _clamp01(float(c.get("a", 1.0)) * opacity)
}

fn _style_lookup(str style, str key) any {
   if style.len == 0 { return 0 }
   def parts = str.split(style, ";")
   mut i = 0
   while i < parts.len {
      def part = to_str(parts[i])
      def colon = str.find(part, ":")
      if colon >= 0 {
         def k = str.lower(str.strip(str.str_slice(part, 0, colon)))
         if k == key { return str.strip(str.str_slice(part, colon + 1, part.len)) }
      }
      i += 1
   }
   0
}

fn _attr(any attrs, str key, any fallback=0) any {
   if !is_dict(attrs) { return fallback }
   attrs.get(key, fallback)
}

fn _style_attr(any attrs, str key, any fallback=0) any {
   if !is_dict(attrs) { return fallback }
   def style_val = _style_lookup(to_str(attrs.get("style", "")), key)
   if style_val { return style_val }
   attrs.get(key, fallback)
}

fn _mat_identity() list { [1.0, 0.0, 0.0, 1.0, 0.0, 0.0] }

fn _mat_mul(list a, list b) list {
   [
      float(a[0]) * float(b[0]) + float(a[2]) * float(b[1]),
      float(a[1]) * float(b[0]) + float(a[3]) * float(b[1]),
      float(a[0]) * float(b[2]) + float(a[2]) * float(b[3]),
      float(a[1]) * float(b[2]) + float(a[3]) * float(b[3]),
      float(a[0]) * float(b[4]) + float(a[2]) * float(b[5]) + float(a[4]),
      float(a[1]) * float(b[4]) + float(a[3]) * float(b[5]) + float(a[5])
   ]
}

fn _mat_apply(list m, f64 x, f64 y) list {
   [float(m[0]) * x + float(m[2]) * y + float(m[4]), float(m[1]) * x + float(m[3]) * y + float(m[5])]
}

fn _mat_translate(f64 x, f64 y) list { [1.0, 0.0, 0.0, 1.0, x, y] }

fn _mat_scale(f64 x, f64 y) list { [x, 0.0, 0.0, y, 0.0, 0.0] }

fn _mat_rotate(f64 deg) list {
   def r = deg * math.PI / 180.0
   def c = math.cos(r)
   def s = math.sin(r)
   [c, s, -s, c, 0.0, 0.0]
}

fn _transform_matrix(any raw) list {
   def s = to_str(raw)
   mut p = 0
   mut out = _mat_identity()
   while p < s.len {
      while p < s.len && !_is_alpha(load8(s, p)) { p += 1 }
      def start = p
      while p < s.len && (_is_alpha(load8(s, p)) || load8(s, p) == 45) { p += 1 }
      if start >= p { break }
      def name = str.lower(str.str_slice(s, start, p))
      while p < s.len && load8(s, p) != 40 { p += 1 }
      if p >= s.len { break }
      p += 1
      def arg_start = p
      mut depth = 1
      while p < s.len && depth > 0 {
         def c = load8(s, p)
         if c == 40 { depth += 1 }
         elif c == 41 { depth -= 1 }
         if depth > 0 { p += 1 }
      }
      def args = _number_list(str.str_slice(s, arg_start, p))
      if p < s.len && load8(s, p) == 41 { p += 1 }
      mut local = _mat_identity()
      if name == "translate" && args.len >= 1 {
         local = _mat_translate(float(args[0]), args.len > 1 ? float(args[1]) : 0.0)
      } elif name == "scale" && args.len >= 1 {
         local = _mat_scale(float(args[0]), args.len > 1 ? float(args[1]) : float(args[0]))
      } elif name == "matrix" && args.len >= 6 {
         local = [float(args[0]), float(args[1]), float(args[2]), float(args[3]), float(args[4]), float(args[5])]
      } elif name == "rotate" && args.len >= 1 {
         local = _mat_rotate(float(args[0]))
         if args.len >= 3 {
            local = _mat_mul(_mat_translate(float(args[1]), float(args[2])), _mat_mul(local, _mat_translate(0.0 - float(args[1]), 0.0 - float(args[2]))))
         }
      }
      out = _mat_mul(out, local)
   }
   out
}

fn _state_root(list transform) dict {
   mut s = dict(8)
   s["fill"] = _rgba(0, 0, 0, 1.0)
   s["stroke"] = _rgba_none()
   s["stroke_width"] = 1.0
   s["opacity"] = 1.0
   s["fill_rule"] = "nonzero"
   s["transform"] = transform
   s
}

fn _state_child(dict parent, any attrs) dict {
   mut s = dict_mod.dict_clone(parent)
   def opacity = float(parent.get("opacity", 1.0)) * _opacity(_style_attr(attrs, "opacity", 1.0), 1.0)
   s["opacity"] = opacity
   def fill_raw = _style_attr(attrs, "fill", 0)
   if fill_raw { s["fill"] = _parse_color(fill_raw, parent.get("fill", 0)) }
   def stroke_raw = _style_attr(attrs, "stroke", 0)
   if stroke_raw { s["stroke"] = _parse_color(stroke_raw, parent.get("stroke", 0)) }
   def fill_op = _style_attr(attrs, "fill-opacity", 0)
   if fill_op && is_dict(s["fill"]) {
      def fc = dict_mod.dict_clone(s["fill"])
      fc["a"] = _clamp01(float(fc.get("a", 1.0)) * _opacity(fill_op, 1.0))
      s["fill"] = fc
   }
   def stroke_op = _style_attr(attrs, "stroke-opacity", 0)
   if stroke_op && is_dict(s["stroke"]) {
      def sc = dict_mod.dict_clone(s["stroke"])
      sc["a"] = _clamp01(float(sc.get("a", 1.0)) * _opacity(stroke_op, 1.0))
      s["stroke"] = sc
   }
   def sw = _style_attr(attrs, "stroke-width", 0)
   if sw { s["stroke_width"] = max(0.0, _num(sw, float(s.get("stroke_width", 1.0)))) }
   def fr = _style_attr(attrs, "fill-rule", 0)
   if fr { s["fill_rule"] = str.lower(str.strip(to_str(fr))) }
   def tf = _style_attr(attrs, "transform", 0)
   if tf { s["transform"] = _mat_mul(s.get("transform", _mat_identity()), _transform_matrix(tf)) }
   s
}

fn _pt(f64 x, f64 y) list { [x, y] }

fn _apply_path(list paths, list m) list {
   mut out = []
   mut i = 0
   while i < paths.len {
      def path = paths[i]
      mut p2 = []
      mut j = 0
      while j < path.len {
         def p = path[j]
         p2 = p2.append(_mat_apply(m, float(p[0]), float(p[1])))
         j += 1
      }
      if p2.len > 0 { out = out.append(p2) }
      i += 1
   }
   out
}

fn _append_cubic(list cur, f64 x0, f64 y0, f64 x1, f64 y1, f64 x2, f64 y2, f64 x3, f64 y3) list {
   def rough = max(max(math.abs(x1 - x0) + math.abs(y1 - y0), math.abs(x2 - x1) + math.abs(y2 - y1)), math.abs(x3 - x2) + math.abs(y3 - y2))
   mut seg = int(max(8.0, min(32.0, rough * 2.0)))
   mut i = 1
   while i <= seg {
      def t = float(i) / float(seg)
      def mt = 1.0 - t
      def x = mt * mt * mt * x0 + 3.0 * mt * mt * t * x1 + 3.0 * mt * t * t * x2 + t * t * t * x3
      def y = mt * mt * mt * y0 + 3.0 * mt * mt * t * y1 + 3.0 * mt * t * t * y2 + t * t * t * y3
      cur = cur.append(_pt(x, y))
      i += 1
   }
   cur
}

fn _append_quad(list cur, f64 x0, f64 y0, f64 x1, f64 y1, f64 x2, f64 y2) list {
   _append_cubic(cur, x0, y0, x0 + (2.0 / 3.0) * (x1 - x0), y0 + (2.0 / 3.0) * (y1 - y0), x2 + (2.0 / 3.0) * (x1 - x2), y2 + (2.0 / 3.0) * (y1 - y2), x2, y2)
}

fn _angle_between(f64 ux, f64 uy, f64 vx, f64 vy) f64 {
   def dot = ux * vx + uy * vy
   def det = ux * vy - uy * vx
   math.atan2(det, dot)
}

fn _append_arc(list cur, f64 x0, f64 y0, f64 rx0, f64 ry0, f64 rot_deg, int large_arc, int sweep, f64 x, f64 y) list {
   mut rx = math.abs(rx0)
   mut ry = math.abs(ry0)
   if rx <= 0.000001 || ry <= 0.000001 || (math.abs(x - x0) < 0.000001 && math.abs(y - y0) < 0.000001) {
      return cur.append(_pt(x, y))
   }
   def phi = rot_deg * math.PI / 180.0
   def cos_phi = math.cos(phi)
   def sin_phi = math.sin(phi)
   def dx = (x0 - x) * 0.5
   def dy = (y0 - y) * 0.5
   def x1p = cos_phi * dx + sin_phi * dy
   def y1p = 0.0 - sin_phi * dx + cos_phi * dy
   def lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
   if lam > 1.0 {
      def s = math.sqrt(lam)
      rx *= s
      ry *= s
   }
   def rx2 = rx * rx
   def ry2 = ry * ry
   def x1p2 = x1p * x1p
   def y1p2 = y1p * y1p
   mut rad = (rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2) / max(0.000001, rx2 * y1p2 + ry2 * x1p2)
   if rad < 0.0 { rad = 0.0 }
   def sign = (large_arc == sweep) ? -1.0 : 1.0
   def coef = sign * math.sqrt(rad)
   def cxp = coef * (rx * y1p / ry)
   def cyp = coef * (0.0 - ry * x1p / rx)
   def cx = cos_phi * cxp - sin_phi * cyp + (x0 + x) * 0.5
   def cy = sin_phi * cxp + cos_phi * cyp + (y0 + y) * 0.5
   def ux = (x1p - cxp) / rx
   def uy = (y1p - cyp) / ry
   def vx = (0.0 - x1p - cxp) / rx
   def vy = (0.0 - y1p - cyp) / ry
   mut theta = _angle_between(1.0, 0.0, ux, uy)
   mut delta = _angle_between(ux, uy, vx, vy)
   if sweep == 0 && delta > 0.0 { delta -= 2.0 * math.PI }
   if sweep != 0 && delta < 0.0 { delta += 2.0 * math.PI }
   def seg = int(max(4.0, min(48.0, math.ceil(math.abs(delta) / (math.PI / 8.0)))))
   mut i = 1
   while i <= seg {
      def a = theta + delta * float(i) / float(seg)
      def ca = math.cos(a)
      def sa = math.sin(a)
      cur = cur.append(_pt(cx + cos_phi * rx * ca - sin_phi * ry * sa, cy + sin_phi * rx * ca + cos_phi * ry * sa))
      i += 1
   }
   cur
}

fn _path_finish(list paths, list cur) list {
   cur.len > 0 ? paths.append(cur) : paths
}

fn _path_flatten(any raw) list {
   def d = to_str(raw)
   mut paths = []
   mut cur = []
   mut p = 0
   mut cmd = 0
   mut x = 0.0
   mut y = 0.0
   mut sx = 0.0
   mut sy = 0.0
   mut last_cx = 0.0
   mut last_cy = 0.0
   mut last_qx = 0.0
   mut last_qy = 0.0
   mut last_cmd = 0
   while p < d.len {
      p = _skip_sep(d, p)
      if p >= d.len { break }
      if _is_path_cmd(load8(d, p)) {
         cmd = load8(d, p)
         p += 1
      } elif cmd == 0 {
         p += 1
         continue
      }
      def rel = cmd >= 97 && cmd <= 122
      def uc = rel ? (cmd - 32) : cmd
      if uc == 90 {
         if cur.len > 0 { cur = cur.append(_pt(sx, sy)) paths = paths.append(cur) cur = [] x = sx y = sy }
         last_cmd = uc
         continue
      }
      if uc == 77 {
         mut first = true
         while true {
            def r1 = _read_num(d, p)
            if !r1[0] { break }
            def r2 = _read_num(d, int(r1[2]))
            if !r2[0] { break }
            p = int(r2[2])
            mut nx = float(r1[1])
            mut ny = float(r2[1])
            if rel { nx += x ny += y }
            if first {
               paths = _path_finish(paths, cur)
               cur = [_pt(nx, ny)]
               sx = nx
               sy = ny
               first = false
            } else {
               cur = cur.append(_pt(nx, ny))
            }
            x = nx
            y = ny
         }
         last_cmd = 77
      } elif uc == 76 {
         while true {
            def r1 = _read_num(d, p)
            if !r1[0] { break }
            def r2 = _read_num(d, int(r1[2]))
            if !r2[0] { break }
            p = int(r2[2])
            x = (rel ? x : 0.0) + float(r1[1])
            y = (rel ? y : 0.0) + float(r2[1])
            cur = cur.append(_pt(x, y))
         }
         last_cmd = 76
      } elif uc == 72 {
         while true {
            def r = _read_num(d, p)
            if !r[0] { break }
            p = int(r[2])
            x = rel ? x + float(r[1]) : float(r[1])
            cur = cur.append(_pt(x, y))
         }
         last_cmd = 72
      } elif uc == 86 {
         while true {
            def r = _read_num(d, p)
            if !r[0] { break }
            p = int(r[2])
            y = rel ? y + float(r[1]) : float(r[1])
            cur = cur.append(_pt(x, y))
         }
         last_cmd = 86
      } elif uc == 67 {
         while true {
            def r1 = _read_num(d, p) if !r1[0] { break }
            def r2 = _read_num(d, int(r1[2])) if !r2[0] { break }
            def r3 = _read_num(d, int(r2[2])) if !r3[0] { break }
            def r4 = _read_num(d, int(r3[2])) if !r4[0] { break }
            def r5 = _read_num(d, int(r4[2])) if !r5[0] { break }
            def r6 = _read_num(d, int(r5[2])) if !r6[0] { break }
            p = int(r6[2])
            def x1 = (rel ? x : 0.0) + float(r1[1])
            def y1 = (rel ? y : 0.0) + float(r2[1])
            def x2 = (rel ? x : 0.0) + float(r3[1])
            def y2 = (rel ? y : 0.0) + float(r4[1])
            def x3 = (rel ? x : 0.0) + float(r5[1])
            def y3 = (rel ? y : 0.0) + float(r6[1])
            cur = _append_cubic(cur, x, y, x1, y1, x2, y2, x3, y3)
            x = x3
            y = y3
            last_cx = x2
            last_cy = y2
         }
         last_cmd = 67
      } elif uc == 83 {
         while true {
            def r1 = _read_num(d, p) if !r1[0] { break }
            def r2 = _read_num(d, int(r1[2])) if !r2[0] { break }
            def r3 = _read_num(d, int(r2[2])) if !r3[0] { break }
            def r4 = _read_num(d, int(r3[2])) if !r4[0] { break }
            p = int(r4[2])
            def x1 = (last_cmd == 67 || last_cmd == 83) ? (2.0 * x - last_cx) : x
            def y1 = (last_cmd == 67 || last_cmd == 83) ? (2.0 * y - last_cy) : y
            def x2 = (rel ? x : 0.0) + float(r1[1])
            def y2 = (rel ? y : 0.0) + float(r2[1])
            def x3 = (rel ? x : 0.0) + float(r3[1])
            def y3 = (rel ? y : 0.0) + float(r4[1])
            cur = _append_cubic(cur, x, y, x1, y1, x2, y2, x3, y3)
            x = x3
            y = y3
            last_cx = x2
            last_cy = y2
         }
         last_cmd = 83
      } elif uc == 81 {
         while true {
            def r1 = _read_num(d, p) if !r1[0] { break }
            def r2 = _read_num(d, int(r1[2])) if !r2[0] { break }
            def r3 = _read_num(d, int(r2[2])) if !r3[0] { break }
            def r4 = _read_num(d, int(r3[2])) if !r4[0] { break }
            p = int(r4[2])
            def x1 = (rel ? x : 0.0) + float(r1[1])
            def y1 = (rel ? y : 0.0) + float(r2[1])
            def x2 = (rel ? x : 0.0) + float(r3[1])
            def y2 = (rel ? y : 0.0) + float(r4[1])
            cur = _append_quad(cur, x, y, x1, y1, x2, y2)
            x = x2
            y = y2
            last_qx = x1
            last_qy = y1
         }
         last_cmd = 81
      } elif uc == 84 {
         while true {
            def r1 = _read_num(d, p) if !r1[0] { break }
            def r2 = _read_num(d, int(r1[2])) if !r2[0] { break }
            p = int(r2[2])
            def x1 = (last_cmd == 81 || last_cmd == 84) ? (2.0 * x - last_qx) : x
            def y1 = (last_cmd == 81 || last_cmd == 84) ? (2.0 * y - last_qy) : y
            def x2 = (rel ? x : 0.0) + float(r1[1])
            def y2 = (rel ? y : 0.0) + float(r2[1])
            cur = _append_quad(cur, x, y, x1, y1, x2, y2)
            x = x2
            y = y2
            last_qx = x1
            last_qy = y1
         }
         last_cmd = 84
      } elif uc == 65 {
         while true {
            def r1 = _read_num(d, p) if !r1[0] { break }
            def r2 = _read_num(d, int(r1[2])) if !r2[0] { break }
            def r3 = _read_num(d, int(r2[2])) if !r3[0] { break }
            def r4 = _read_num(d, int(r3[2])) if !r4[0] { break }
            def r5 = _read_num(d, int(r4[2])) if !r5[0] { break }
            def r6 = _read_num(d, int(r5[2])) if !r6[0] { break }
            def r7 = _read_num(d, int(r6[2])) if !r7[0] { break }
            p = int(r7[2])
            def nx = (rel ? x : 0.0) + float(r6[1])
            def ny = (rel ? y : 0.0) + float(r7[1])
            cur = _append_arc(cur, x, y, float(r1[1]), float(r2[1]), float(r3[1]), int(r4[1]) != 0 ? 1 : 0, int(r5[1]) != 0 ? 1 : 0, nx, ny)
            x = nx
            y = ny
         }
         last_cmd = 65
      } else {
         p += 1
      }
   }
   _path_finish(paths, cur)
}

fn _rect_path(f64 x, f64 y, f64 w, f64 h, f64 rx=0.0, f64 ry=0.0) list {
   if w <= 0.0 || h <= 0.0 { return [] }
   rx = min(math.abs(rx), w * 0.5)
   ry = min(math.abs(ry), h * 0.5)
   if rx <= 0.0 || ry <= 0.0 { return [[_pt(x, y), _pt(x + w, y), _pt(x + w, y + h), _pt(x, y + h), _pt(x, y)]] }
   mut p = []
   mut i = 0
   while i <= 6 {
      def a = -math.PI * 0.5 + float(i) * (math.PI * 0.5 / 6.0)
      p = p.append(_pt(x + w - rx + math.cos(a) * rx, y + ry + math.sin(a) * ry))
      i += 1
   }
   i = 0
   while i <= 6 {
      def a = float(i) * (math.PI * 0.5 / 6.0)
      p = p.append(_pt(x + w - rx + math.cos(a) * rx, y + h - ry + math.sin(a) * ry))
      i += 1
   }
   i = 0
   while i <= 6 {
      def a = math.PI * 0.5 + float(i) * (math.PI * 0.5 / 6.0)
      p = p.append(_pt(x + rx + math.cos(a) * rx, y + h - ry + math.sin(a) * ry))
      i += 1
   }
   i = 0
   while i <= 6 {
      def a = math.PI + float(i) * (math.PI * 0.5 / 6.0)
      p = p.append(_pt(x + rx + math.cos(a) * rx, y + ry + math.sin(a) * ry))
      i += 1
   }
   [p.append(p[0])]
}

fn _ellipse_path(f64 cx, f64 cy, f64 rx, f64 ry) list {
   if rx <= 0.0 || ry <= 0.0 { return [] }
   mut p = []
   mut i = 0
   def n = 40
   while i <= n {
      def a = float(i) * (2.0 * math.PI / float(n))
      p = p.append(_pt(cx + math.cos(a) * rx, cy + math.sin(a) * ry))
      i += 1
   }
   [p]
}

fn _points_path(any raw, bool close=false) list {
   def xs = _number_list(raw)
   mut p = []
   mut i = 0
   while i + 1 < xs.len {
      p = p.append(_pt(float(xs[i]), float(xs[i + 1])))
      i += 2
   }
   if close && p.len > 0 { p = p.append(p[0]) }
   p.len > 0 ? [p] : []
}

fn _line_path(f64 x1, f64 y1, f64 x2, f64 y2) list { [[_pt(x1, y1), _pt(x2, y2)]] }

fn _winding_at(list paths, f64 x, f64 y, bool evenodd=false) int {
   mut winding = 0
   mut i = 0
   while i < paths.len {
      def path = paths[i]
      mut j = 0
      while j + 1 < path.len {
         def p0 = path[j]
         def p1 = path[j + 1]
         def x0 = float(p0[0])
         def y0 = float(p0[1])
         def x1 = float(p1[0])
         def y1 = float(p1[1])
         if y0 <= y {
            if y1 > y && ((x1 - x0) * (y - y0) - (x - x0) * (y1 - y0)) > 0.0 { winding += 1 }
         } elif y1 <= y && ((x1 - x0) * (y - y0) - (x - x0) * (y1 - y0)) < 0.0 {
            winding -= 1
         }
         j += 1
      }
      i += 1
   }
   evenodd ? (math.abs(winding) % 2) : winding
}

fn _seg_dist2(f64 px, f64 py, f64 x0, f64 y0, f64 x1, f64 y1) f64 {
   def dx = x1 - x0
   def dy = y1 - y0
   def len2 = dx * dx + dy * dy
   mut t = len2 > 0.000001 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.0
   t = _clamp01(t)
   def qx = x0 + dx * t
   def qy = y0 + dy * t
   def ex = px - qx
   def ey = py - qy
   ex * ex + ey * ey
}

fn _stroke_hit(list paths, f64 x, f64 y, f64 width) bool {
   def r2 = (width * 0.5) * (width * 0.5)
   mut i = 0
   while i < paths.len {
      def path = paths[i]
      mut j = 0
      while j + 1 < path.len {
         def p0 = path[j]
         def p1 = path[j + 1]
         if _seg_dist2(x, y, float(p0[0]), float(p0[1]), float(p1[0]), float(p1[1])) <= r2 { return true }
         j += 1
      }
      i += 1
   }
   false
}

fn _path_bounds(list paths, f64 pad, int w, int h) list {
   mut minx = 1000000000.0
   mut miny = 1000000000.0
   mut maxx = -1000000000.0
   mut maxy = -1000000000.0
   mut any_pt = false
   mut i = 0
   while i < paths.len {
      def path = paths[i]
      mut j = 0
      while j < path.len {
         def p = path[j]
         def x = float(p[0])
         def y = float(p[1])
         if x < minx { minx = x }
         if y < miny { miny = y }
         if x > maxx { maxx = x }
         if y > maxy { maxy = y }
         any_pt = true
         j += 1
      }
      i += 1
   }
   if !any_pt { return [0, 0, -1, -1] }
   def x0 = max(0, int(math.floor(minx - pad)))
   def y0 = max(0, int(math.floor(miny - pad)))
   def x1 = min(w - 1, int(math.ceil(maxx + pad)))
   def y1 = min(h - 1, int(math.ceil(maxy + pad)))
   [x0, y0, x1, y1]
}

fn _blend(any out, int off, dict color, f64 alpha) any {
   alpha = _clamp01(alpha)
   if alpha <= 0.0 { return 0 }
   def sr = float(color.get("r", 0))
   def sg = float(color.get("g", 0))
   def sb = float(color.get("b", 0))
   def dr = float(load8(out, off) & 255)
   def dg = float(load8(out, off + 1) & 255)
   def db = float(load8(out, off + 2) & 255)
   def da = float(load8(out, off + 3) & 255) / 255.0
   def oa = alpha + da * (1.0 - alpha)
   if oa <= 0.0 { return 0 }
   def r = (sr * alpha + dr * da * (1.0 - alpha)) / oa
   def g = (sg * alpha + dg * da * (1.0 - alpha)) / oa
   def b = (sb * alpha + db * da * (1.0 - alpha)) / oa
   store8(out, max(0, min(255, int(r + 0.5))), off)
   store8(out, max(0, min(255, int(g + 0.5))), off + 1)
   store8(out, max(0, min(255, int(b + 0.5))), off + 2)
   store8(out, max(0, min(255, int(oa * 255.0 + 0.5))), off + 3)
   0
}

fn _draw_paths(any out, int w, int h, list paths, dict color, f64 opacity, str mode, f64 stroke_width=1.0, str fill_rule="nonzero") any {
   def ca = _color_alpha(color, opacity)
   if ca <= 0.0 || paths.len == 0 { return 0 }
   def evenodd = fill_rule == "evenodd"
   def pad = mode == "stroke" ? stroke_width * 0.5 + 1.0 : 1.0
   def b = _path_bounds(paths, pad, w, h)
   if int(b[2]) < int(b[0]) || int(b[3]) < int(b[1]) { return 0 }
   mut y = int(b[1])
   while y <= int(b[3]) {
      mut x = int(b[0])
      while x <= int(b[2]) {
         mut cov = 0
         mut sy = 0
         while sy < 2 {
            mut sx = 0
            while sx < 2 {
               def px = float(x) + (float(sx) + 0.5) * 0.5
               def py = float(y) + (float(sy) + 0.5) * 0.5
               if mode == "fill" {
                  if _winding_at(paths, px, py, evenodd) != 0 { cov += 1 }
               } elif _stroke_hit(paths, px, py, stroke_width) {
                  cov += 1
               }
               sx += 1
            }
            sy += 1
         }
         if cov > 0 { _blend(out, (y * w + x) * 4, color, ca * float(cov) * 0.25) }
         x += 1
      }
      y += 1
   }
   0
}

fn _collect_defs(any node, dict defs) any {
   if !is_dict(node) { return 0 }
   def attrs = node.get("attr", dict())
   def id = to_str(attrs.get("id", ""))
   if id.len > 0 { defs[id] = node }
   def children = node.get("children", [])
   mut i = 0
   while i < children.len {
      _collect_defs(children[i], defs)
      i += 1
   }
   0
}

fn _node_paths(str name, any attrs) list {
   if name == "path" { return _path_flatten(_attr(attrs, "d", "")) }
   if name == "rect" {
      def x = _num(_attr(attrs, "x", 0.0), 0.0)
      def y = _num(_attr(attrs, "y", 0.0), 0.0)
      def w = _num(_attr(attrs, "width", 0.0), 0.0)
      def h = _num(_attr(attrs, "height", 0.0), 0.0)
      mut rx = _num(_attr(attrs, "rx", 0.0), 0.0)
      mut ry = _num(_attr(attrs, "ry", rx), rx)
      if rx == 0.0 && ry > 0.0 { rx = ry }
      return _rect_path(x, y, w, h, rx, ry)
   }
   if name == "circle" { return _ellipse_path(_num(_attr(attrs, "cx", 0.0), 0.0), _num(_attr(attrs, "cy", 0.0), 0.0), _num(_attr(attrs, "r", 0.0), 0.0), _num(_attr(attrs, "r", 0.0), 0.0)) }
   if name == "ellipse" { return _ellipse_path(_num(_attr(attrs, "cx", 0.0), 0.0), _num(_attr(attrs, "cy", 0.0), 0.0), _num(_attr(attrs, "rx", 0.0), 0.0), _num(_attr(attrs, "ry", 0.0), 0.0)) }
   if name == "polygon" { return _points_path(_attr(attrs, "points", ""), true) }
   if name == "polyline" { return _points_path(_attr(attrs, "points", ""), false) }
   if name == "line" { return _line_path(_num(_attr(attrs, "x1", 0.0), 0.0), _num(_attr(attrs, "y1", 0.0), 0.0), _num(_attr(attrs, "x2", 0.0), 0.0), _num(_attr(attrs, "y2", 0.0), 0.0)) }
   []
}

fn _draw_node(any out, int w, int h, any node, dict state, dict defs, int depth=0) any {
   if !is_dict(node) || depth > 16 { return 0 }
   def name = str.lower(to_str(node.get("name", "")))
   if name == "defs" || name == "lineargradient" || name == "radialgradient" || name == "clippath" || name == "mask" || name == "style" { return 0 }
   def attrs = node.get("attr", dict())
   if to_str(_style_attr(attrs, "display", "")) == "none" { return 0 }
   def st = _state_child(state, attrs)
   if name == "use" {
      mut href = to_str(attrs.get("xlink:href", attrs.get("href", "")))
      if str.startswith(href, "#") { href = str.str_slice(href, 1, href.len) }
      if defs.contains(href) {
         mut use_state = dict_mod.dict_clone(st)
         def tx = _num(attrs.get("x", 0.0), 0.0)
         def ty = _num(attrs.get("y", 0.0), 0.0)
         if tx != 0.0 || ty != 0.0 { use_state["transform"] = _mat_mul(use_state.get("transform", _mat_identity()), _mat_translate(tx, ty)) }
         _draw_node(out, w, h, defs[href], use_state, defs, depth + 1)
      }
      return 0
   }
   def raw_paths = _node_paths(name, attrs)
   if raw_paths.len > 0 {
      def paths = _apply_path(raw_paths, st.get("transform", _mat_identity()))
      def fill = st.get("fill", _rgba_none())
      if _color_alpha(fill, float(st.get("opacity", 1.0))) > 0.0 && name != "line" && name != "polyline" {
         _draw_paths(out, w, h, paths, fill, float(st.get("opacity", 1.0)), "fill", 1.0, to_str(st.get("fill_rule", "nonzero")))
      }
      def stroke = st.get("stroke", _rgba_none())
      if _color_alpha(stroke, float(st.get("opacity", 1.0))) > 0.0 {
         def m = st.get("transform", _mat_identity())
         def scale = max(math.sqrt(float(m[0]) * float(m[0]) + float(m[1]) * float(m[1])), math.sqrt(float(m[2]) * float(m[2]) + float(m[3]) * float(m[3])))
         _draw_paths(out, w, h, paths, stroke, float(st.get("opacity", 1.0)), "stroke", max(0.5, float(st.get("stroke_width", 1.0)) * scale), "nonzero")
      }
   }
   def children = node.get("children", [])
   mut i = 0
   while i < children.len {
      _draw_node(out, w, h, children[i], st, defs, depth + 1)
      i += 1
   }
   0
}

fn _svg_min_raster() int {
   mut target = 32.0
   def raw = os.env("NY_SVG_MIN_RASTER")
   if raw {
      target = _num(raw, target)
      if target < 16.0 { target = 16.0 } elif target > 512.0 { target = 512.0 }
   }
   int(target + 0.5)
}

fn _viewport(any root) list {
   def attrs = root.get("attr", dict())
   def width = max(1.0, _num(attrs.get("width", 64.0), 64.0))
   def height = max(1.0, _num(attrs.get("height", width), width))
   def vb_raw = attrs.get("viewBox", attrs.get("viewbox", ""))
   def vb = _number_list(vb_raw)
   if vb.len >= 4 { return [float(vb[0]), float(vb[1]), max(1.0, float(vb[2])), max(1.0, float(vb[3])), width, height] }
   [0.0, 0.0, width, height, width, height]
}

fn _decode_root(any root) any {
   if !is_dict(root) || str.lower(to_str(root.get("name", ""))) != "svg" {
      _svg_set_error("missing <svg> root")
      return 0
   }
   def vp = _viewport(root)
   def min_raster = _svg_min_raster()
   def src_w = float(vp[4])
   def src_h = float(vp[5])
   def scale = float(min_raster) / max(src_w, src_h)
   def out_w = max(1, int(src_w * scale + 0.5))
   def out_h = max(1, int(src_h * scale + 0.5))
   def ptr = malloc(out_w * out_h * 4 + 32)
   if !ptr {
      _svg_set_error("out of memory allocating SVG raster")
      return 0
   }
   memset(ptr, 0, out_w * out_h * 4)
   mut defs = dict(32)
   _collect_defs(root, defs)
   def base = _mat_mul(_mat_scale(float(out_w) / float(vp[2]), float(out_h) / float(vp[3])), _mat_translate(0.0 - float(vp[0]), 0.0 - float(vp[1])))
   _draw_node(ptr, out_w, out_h, root, _state_root(base), defs)
   def rgba = init_str(ptr, out_w * out_h * 4)
   mut out = dict(4)
   out = dict_mod.dict_write(out, "data", rgba)
   out = dict_mod.dict_write(out, "width", out_w)
   out = dict_mod.dict_write(out, "height", out_h)
   out = dict_mod.dict_write(out, "channels", 4)
   _svg_last_error = ""
   out
}

fn decode(any data, any _source_path="") any {
   "Decodes SVG bytes through the native Ny rasterizer."
   if !_looks_like_svg(data) {
      _svg_set_error("input does not look like SVG")
      return 0
   }
   def root = _parse_svg_tree(data)
   if !root {
      _svg_set_error("SVG XML parse failed")
      return 0
   }
   _decode_root(root)
}

fn load_path(any path) any {
   "Loads and decodes an SVG file through the native Ny rasterizer."
   if !is_str(path) || path.len == 0 || !os.file_exists(path) {
      _svg_set_error("SVG path not found: " + to_str(path))
      return 0
   }
   def r = os.file_read(path)
   if !is_ok(r) {
      _svg_set_error("failed to read SVG path: " + to_str(path))
      return 0
   }
   decode(unwrap(r), path)
}
