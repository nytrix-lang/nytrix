;; Keywords: ui vector graphics canvas path stroke fill gradient antialias nanovg render
;; Antialiased 2D vector graphics on the UI renderer: paths, fills, strokes,
;; gradients, image patterns, transforms, scissoring, and text on one canvas.
;; References:
;; - core.os.ui.render
;;
;; The API follows the NanoVG model. Build a path, then fill or stroke it:
;;
;;   vg.begin_frame(w, h)
;;   vg.begin_path()
;;   vg.rounded_rect(20.0, 20.0, 200.0, 80.0, 12.0)
;;   vg.fill_paint(vg.linear_gradient(20.0, 20.0, 20.0, 100.0, top, bottom))
;;   vg.fill()
;;   vg.stroke_color([1.0, 1.0, 1.0, 0.5])
;;   vg.stroke_width(2.0)
;;   vg.stroke()
;;   vg.end_frame()
;;
;; Feature matrix:
;;   paths        move_to, line_to, bezier_to, quad_to, arc_to, arc, close_path,
;;                rect, rounded_rect, rounded_rect_varying, ellipse, circle
;;   fills        convex fan, or scanline trapezoids with the nonzero rule for
;;                concave shapes and holes (path_winding(HOLE))
;;   strokes      width, miter / round / bevel joins with a miter limit,
;;                butt / round / square caps
;;   antialias    one-pixel alpha fringe on every fill and stroke edge
;;   paints       solid color, linear_gradient, radial_gradient, box_gradient,
;;                image_pattern (gradients are baked to small textures, so
;;                they are exact per pixel; the mock renderer shades per vertex)
;;   transforms   translate, rotate, scale, skew_x, skew_y, transform, with a
;;                save / restore stack
;;   scissor      scissor, intersect_scissor, reset_scissor (the bounding box
;;                of the transformed rectangle)
;;   text         font_face, text_align, text, text_bounds (glyphs follow the
;;                whole transform on Vulkan and GL; the software renderer
;;                keeps them upright at the transformed position)
;;   compositing  global_alpha, and the eleven Porter-Duff operators of
;;                global_composite_operation (fills and strokes; text stays
;;                source-over). Operators other than SOURCE_OVER draw
;;                premultiplied colors; an image pattern with translucent
;;                texels is treated as opaque-premultiplied there.
module core.os.ui.render.vg(
   begin_frame, end_frame, save, restore, reset,
   stroke_color, fill_color, stroke_paint, fill_paint, stroke_width, miter_limit, line_cap, line_join,
   global_alpha, shape_anti_alias, global_composite_operation,
   reset_transform, transform, translate, rotate, scale, skew_x, skew_y, current_transform,
   linear_gradient, radial_gradient, box_gradient, image_pattern,
   scissor, intersect_scissor, reset_scissor,
   begin_path, move_to, line_to, bezier_to, quad_to, catmull_rom, bspline, arc_to, close_path, path_winding,
   arc, rect, rounded_rect, rounded_rect_varying, ellipse, circle, fill, stroke,
   font_face, text_align, text, text_bounds,
   frame_stats, release,
   BUTT, ROUND, SQUARE, BEVEL, MITER, SOLID, HOLE, CCW, CW,
   ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT, ALIGN_TOP, ALIGN_MIDDLE, ALIGN_BOTTOM, ALIGN_BASELINE,
   SOURCE_OVER, SOURCE_IN, SOURCE_OUT, ATOP, DESTINATION_OVER, DESTINATION_IN, DESTINATION_OUT,
   DESTINATION_ATOP, LIGHTER, COPY, XOR
)

use core
use core.math (sqrt, sin, cos, tan, atan2, acos, abs, PI)
use core.str as str
use core.os.ui.render as gfx
use core.os.ui.render.shared as shared
use core.os.ui.render.matrix as mat

def BUTT = 0
def ROUND = 1
def SQUARE = 2
def BEVEL = 3
def MITER = 4
def SOLID = 1
def HOLE = 2
def CCW = 1
def CW = 2
def ALIGN_LEFT = 1
def ALIGN_CENTER = 2
def ALIGN_RIGHT = 4
def ALIGN_TOP = 8
def ALIGN_MIDDLE = 16
def ALIGN_BOTTOM = 32
def ALIGN_BASELINE = 64
def SOURCE_OVER = 0
def SOURCE_IN = 1
def SOURCE_OUT = 2
def ATOP = 3
def DESTINATION_OVER = 4
def DESTINATION_IN = 5
def DESTINATION_OUT = 6
def DESTINATION_ATOP = 7
def LIGHTER = 8
def COPY = 9
def XOR = 10

;; Current state. save() copies these into a list; restore() reads them back.
mut _xa, _xb, _xc, _xd, _xe, _xf = 1.0, 0.0, 0.0, 1.0, 0.0, 0.0

;; Model matrix reused for transformed text.
mut any _text_mat = nil
mut any _fill = nil
mut any _stroke = nil
mut _stroke_w, _miter = 1.0, 10.0
mut int _cap = 0
mut int _join = 4
mut _alpha = 1.0

;; Porter-Duff operator for fills and strokes (SOURCE_OVER .. XOR).
mut int _comp = 0
mut _aa = true

;; Diagnostic toggle for texture bakes; off unless a caller sets it.
mut _debug_tex = false
mut _sc_on = false
mut _sc_x, _sc_y, _sc_w, _sc_h = 0.0, 0.0, 0.0, 0.0
mut int _font = 0
mut int _align = 65
mut any _stack = nil

;; Path being built: flattened, already transformed points, and one record of
;; (first point, point count, closed, winding) per subpath.
mut any _px = nil
mut any _py = nil
mut any _subs = nil
mut _pen_x, _pen_y = 0.0, 0.0
mut _fringe = 1.0
mut _view_w, _view_h = 0.0, 0.0

;; Vertex stream for the fill or stroke being emitted.
mut _vb = 0
mut int _vb_cap = 0
mut int _vb_n = 0
mut any _paint = nil
mut any _paint_tex_cache = nil
mut int _stat_draws = 0
mut int _stat_verts = 0
mut int _stat_bakes = 0

fn _init() any {
   if !is_list(_stack) { _stack = [] }
   if !is_list(_px) { _px = [] }
   if !is_list(_py) { _py = [] }
   if !is_list(_subs) { _subs = [] }
   if !is_dict(_paint_tex_cache) { _paint_tex_cache = dict(64) }
   if !is_list(_fill) { _fill = _solid([1.0, 1.0, 1.0, 1.0]) }
   if !is_list(_stroke) { _stroke = _solid([0.0, 0.0, 0.0, 1.0]) }
}

;; ---- paints ---------------------------------------------------------------
;; A paint is a flat list:
;;   0 kind (0 solid, 1 linear, 2 radial, 3 box, 4 image)
;;   1..6 inverse of the paint-to-screen transform
;;   7, 8 extent; 9 radius or inner radius; 10 feather or outer radius
;;   11..14 inner RGBA; 15..18 outer RGBA; 19 image texture; 20 image alpha
fn _c(any col, int i, f64 fallback) f64 {
   if is_list(col) { return float(col.get(i, fallback)) }
   fallback
}

fn _solid(any col) list {
   [0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
      _c(col, 0, 1.0), _c(col, 1, 1.0), _c(col, 2, 1.0), _c(col, 3, 1.0),
      _c(col, 0, 1.0), _c(col, 1, 1.0), _c(col, 2, 1.0), _c(col, 3, 1.0), -1.0, 1.0]
}

fn _paint_make(f64 kind, f64 pa, f64 pb, f64 pc, f64 pd, f64 pe, f64 pf, f64 ex, f64 ey, f64 r, f64 f, any icol, any ocol, f64 tex, f64 alpha) list {
   ;; Compose with the current transform, then store the inverse so a screen
   ;; point maps straight into paint space.
   def f64 a = pa * _xa + pb * _xc
   def f64 b = pa * _xb + pb * _xd
   def f64 c = pc * _xa + pd * _xc
   def f64 d = pc * _xb + pd * _xd
   def f64 e = pe * _xa + pf * _xc + _xe
   def f64 g = pe * _xb + pf * _xd + _xf
   def f64 det = a * d - c * b
   mut f64 ia, ib, ic, id, ie, ig = 1.0, 0.0, 0.0, 1.0, 0.0, 0.0
   if abs(det) > 0.000000001 {
      def f64 inv = 1.0 / det
      ia, ib = d * inv, (0.0 - b) * inv
      ic, id = (0.0 - c) * inv, a * inv
      ie = (c * g - d * e) * inv
      ig = (b * e - a * g) * inv
   }
   [kind, ia, ib, ic, id, ie, ig, ex, ey, r, f,
      _c(icol, 0, 1.0), _c(icol, 1, 1.0), _c(icol, 2, 1.0), _c(icol, 3, 1.0),
      _c(ocol, 0, 1.0), _c(ocol, 1, 1.0), _c(ocol, 2, 1.0), _c(ocol, 3, 1.0), tex, alpha]
}

fn linear_gradient(f64 sx, f64 sy, f64 ex, f64 ey, any icol, any ocol) list {
   "Returns a paint that blends from `icol` at(sx, sy) to `ocol` at(ex, ey)."
   mut f64 dx, dy = ex - sx, ey - sy
   mut f64 len = float(sqrt(dx * dx + dy * dy))
   if len < 0.0001 {
      dx, dy = 0.0, 1.0
      len = 1.0
   } else {
      dx, dy = dx / len, dy / len
   }
   _paint_make(1.0, dx, dy, 0.0 - dy, dx, sx, sy, len, 0.0, 0.0, 0.0, icol, ocol, -1.0, 1.0)
}

fn radial_gradient(f64 cx, f64 cy, f64 inr, f64 outr, any icol, any ocol) list {
   "Returns a paint that is `icol` inside radius `inr` and blends to `ocol` at radius `outr` around(cx, cy)."
   def f64 r1 = outr > inr ? outr : (inr + 0.001)
   _paint_make(2.0, 1.0, 0.0, 0.0, 1.0, cx, cy, r1, r1, inr, r1, icol, ocol, -1.0, 1.0)
}

fn box_gradient(f64 x, f64 y, f64 w, f64 h, f64 r, f64 f, any icol, any ocol) list {
   "Returns a feathered rounded-rectangle paint: `icol` inside the box, `ocol` outside, blended over `f` pixels. Useful for drop shadows and inner glows."
   _paint_make(3.0, 1.0, 0.0, 0.0, 1.0, x + w * 0.5, y + h * 0.5, w * 0.5, h * 0.5, r, f < 1.0 ? 1.0 : f, icol, ocol, -1.0, 1.0)
}

fn image_pattern(f64 ox, f64 oy, f64 ex, f64 ey, f64 angle, int image, f64 alpha=1.0) list {
   "Returns a paint that maps texture `image` onto the rectangle at(ox, oy) of size(ex, ey), rotated by `angle` radians around its origin."
   def f64 cs, sn = float(cos(angle)), float(sin(angle))
   _paint_make(4.0, cs, sn, 0.0 - sn, cs, ox, oy, ex == 0.0 ? 1.0 : ex, ey == 0.0 ? 1.0 : ey, 0.0, 0.0, [1.0, 1.0, 1.0, 1.0], [1.0, 1.0, 1.0, 1.0], float(image), alpha)
}

fn _box_t(f64 lx, f64 ly, f64 ex, f64 ey, f64 r, f64 f) f64 {
   ;; Signed distance to a rounded rectangle, mapped across the feather.
   def f64 qx = abs(lx) - (ex - r)
   def f64 qy = abs(ly) - (ey - r)
   def f64 ox = qx > 0.0 ? qx : 0.0
   def f64 oy = qy > 0.0 ? qy : 0.0
   def f64 inner = qx > qy ? qx : qy
   def f64 d = (inner < 0.0 ? inner : 0.0) + float(sqrt(ox * ox + oy * oy)) - r
   def f64 t = (d + f * 0.5) / f
   t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t)
}

fn _paint_t(list p, f64 lx, f64 ly) f64 {
   def int kind = int(float(p.get(0, 0.0)))
   mut f64 t = 0.0
   if kind == 1 { t = lx / float(p.get(7, 1.0)) }
   elif kind == 2 {
      def f64 inr, outr = float(p.get(9, 0.0)), float(p.get(10, 1.0))
      t = (float(sqrt(lx * lx + ly * ly)) - inr) / (outr - inr)
   } elif kind == 3 {
      return _box_t(lx, ly, float(p.get(7, 1.0)), float(p.get(8, 1.0)), float(p.get(9, 0.0)), float(p.get(10, 1.0)))
   }
   t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t)
}

fn _paint_key(list p) str {
   def int kind = int(float(p.get(0, 0.0)))
   mut key = to_str(kind)
   mut i = 7
   while i < 19 {
      ;; Linear ramps do not depend on their length, only on their colors.
      if !(kind == 1 && i < 11) { key = key + ":" + to_str(int(float(p.get(i, 0.0)) * 255.0 + 0.5)) }
      i += 1
   }
   key
}

fn _bake_px(ptr buf, int off, list p, f64 t, bool premul=false) any {
   def f64 u = 1.0 - t
   def f64 a = float(p.get(14, 1.0)) * u + float(p.get(18, 1.0)) * t
   def f64 k = premul ? a : 1.0
   store8(buf, int((float(p.get(11, 1.0)) * u + float(p.get(15, 1.0)) * t) * k * 255.0 + 0.5), off)
   store8(buf, int((float(p.get(12, 1.0)) * u + float(p.get(16, 1.0)) * t) * k * 255.0 + 0.5), off + 1)
   store8(buf, int((float(p.get(13, 1.0)) * u + float(p.get(17, 1.0)) * t) * k * 255.0 + 0.5), off + 2)
   store8(buf, int(a * 255.0 + 0.5), off + 3)
}

fn _paint_texture(list p) int {
   "Returns the texture that holds a gradient paint, baking it on first use. Returns -1 for solid paints and on the mock renderer."
   def int kind = int(float(p.get(0, 0.0)))
   if kind == 0 { return -1 }
   if kind == 4 { return int(float(p.get(19, -1.0))) }
   if gfx.get_backend_type() == gfx.BACKEND_MOCK { return -1 }
   def bool premul = _comp != 0
   def key = _paint_key(p) + (premul ? ":p" : "")
   if _paint_tex_cache.contains(key) { return int(float(_paint_tex_cache.get(key, -1.0))) }
   ;; A bounded cache: animated gradients would otherwise leak textures.
   if _paint_tex_cache.len >= 96 { _release_paint_textures() }
   def int tw = kind == 1 ? 256 : 96
   def int th = kind == 1 ? 1 : 96
   def ptr buf = malloc(tw * th * 4)
   if !buf { return -1 }
   mut int y = 0
   while y < th {
      mut int x = 0
      while x < tw {
         mut f64 t = 0.0
         if kind == 1 { t = (float(x) + 0.5) / float(tw) }
         elif kind == 2 {
            def f64 outr = float(p.get(10, 1.0))
            def f64 lx = ((float(x) + 0.5) / float(tw) * 2.0 - 1.0) * outr
            def f64 ly = ((float(y) + 0.5) / float(th) * 2.0 - 1.0) * outr
            t = _paint_t(p, lx, ly)
         } else {
            def f64 ex, ey = float(p.get(7, 1.0)), float(p.get(8, 1.0))
            def f64 f = float(p.get(10, 1.0))
            def f64 lx = ((float(x) + 0.5) / float(tw) * 2.0 - 1.0) * (ex + f)
            def f64 ly = ((float(y) + 0.5) / float(th) * 2.0 - 1.0) * (ey + f)
            t = _paint_t(p, lx, ly)
         }
         def int off = (y * tw + x) * 4
         _bake_px(buf, off, p, t, premul)
         x += 1
      }
      y += 1
   }
   ;; 9729 = linear filter, 33071 = clamp to edge, so anything outside the
   ;; baked area takes the outer color.
   ;; The baked bytes are display (sRGB) values, like the vertex colors. Vulkan
   ;; needs that said in the format (43 = R8G8B8A8_SRGB); as plain UNORM (37)
   ;; the ramp came out brighter than the same paint on GL.
   def int fmt = gfx.get_backend_type() == gfx.BACKEND_VK ? 43 : 37
   def int tex = gfx.texture_create_rgba(tw, th, buf, fmt, 9729, 33071, 33071, false)
   free(buf)
   _paint_tex_cache[key] = float(tex)
   _stat_bakes += 1
   if _debug_tex { print("[vg] bake kind=" + to_str(kind) + " tex=" + to_str(tex) + " last=" + to_str(gfx.texture_last_created_id())) }
   tex
}

fn _release_paint_textures() any {
   def keys = dict_keys(_paint_tex_cache)
   mut i = 0
   def int n = is_list(keys) ? keys.len : 0
   while i < n {
      def int tex = int(float(_paint_tex_cache.get(keys.get(i, ""), -1.0)))
      if tex >= 0 { gfx.texture_destroy(tex) }
      i += 1
   }
   _paint_tex_cache = dict(64)
}

fn release() any {
   "Frees the gradient textures and the vertex buffer. Call before closing the window."
   _init()
   _release_paint_textures()
   if _vb { free(_vb) }
   _vb = 0
   _vb_cap, _vb_n = 0, 0
}

;; ---- vertex stream --------------------------------------------------------
;; The bound texture is kept as a float and converted where it is used: as an
;; integer module global it read back halved for odd ids, which bound the
;; wrong texture for every other gradient.
mut _cur_texf = -1.0
mut int _cur_kind = 0
mut _cur_ia, _cur_ib, _cur_ic, _cur_id, _cur_ie, _cur_ig = 1.0, 0.0, 0.0, 1.0, 0.0, 0.0
mut _cur_r, _cur_g, _cur_b, _cur_al = 1.0, 1.0, 1.0, 1.0
mut _cur_ex, _cur_ey, _cur_f = 1.0, 1.0, 1.0
mut _cur_a = 1.0
mut int _col_full = 0
mut int _col_edge = 0

fn _pc(f64 r, f64 g, f64 b, f64 a) int {
   ;; Operators other than source-over blend premultiplied colors.
   if _comp != 0 { return gfx.color_pack(r * a, g * a, b * a, a) }
   gfx.color_pack(r, g, b, a)
}

fn _stream_begin(list p, f64 alpha_scale) any {
   _paint = p
   _cur_kind = int(float(p.get(0, 0.0)))
   _cur_texf = float(_paint_texture(p))
   _cur_ia, _cur_ib = float(p.get(1, 1.0)), float(p.get(2, 0.0))
   _cur_ic, _cur_id = float(p.get(3, 0.0)), float(p.get(4, 1.0))
   _cur_ie, _cur_ig = float(p.get(5, 0.0)), float(p.get(6, 0.0))
   _cur_r, _cur_g, _cur_b = float(p.get(11, 1.0)), float(p.get(12, 1.0)), float(p.get(13, 1.0))
   _cur_al = _alpha * alpha_scale * (_cur_kind == 4 ? float(p.get(20, 1.0)) : 1.0)
   _cur_ex, _cur_ey, _cur_f = float(p.get(7, 1.0)), float(p.get(8, 1.0)), float(p.get(10, 1.0))
   _cur_a = float(p.get(14, 1.0))
   _col_full = _pc(_cur_r, _cur_g, _cur_b, _cur_a * _cur_al)
   _col_edge = _pc(_cur_r, _cur_g, _cur_b, 0.0)
   _vb_n = 0
}

fn _reserve(int extra) bool {
   def int need = _vb_n + extra
   if need <= _vb_cap { return true }
   mut int cap = _vb_cap < 1024 ? 1024 : _vb_cap
   while cap < need { cap = cap * 2 }
   def ptr next = malloc(cap * 64)
   if !next { return false }
   if _vb {
      if _vb_n > 0 { __copy_mem(next, _vb, _vb_n * 64) }
      free(_vb)
   }
   ;; Every vertex is written in full once, here. _v then only touches the
   ;; six fields a canvas vertex varies in, which is most of the per-vertex cost.
   mut int i = _vb_n
   while i < cap {
      shared.store_vertex64_typed(next, i, 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0)
      i += 1
   }
   _vb = next
   _vb_cap = cap
   true
}

fn _v(f64 x, f64 y, f64 cover) any {
   "Appends one vertex at a screen position; `cover` is the antialiasing coverage(1 inside, 0 at the fringe edge)."
   mut f64 u, v = 0.0, 0.0
   mut int col = 0
   if _cur_kind == 0 {
      ;; A solid paint has two colors in practice: full coverage and the
      ;; transparent fringe edge.
      if cover >= 1.0 { col = _col_full }
      elif cover <= 0.0 { col = _col_edge }
      else { col = _pc(_cur_r, _cur_g, _cur_b, _cur_a * _cur_al * cover) }
   } else {
      def f64 lx = x * _cur_ia + y * _cur_ic + _cur_ie
      def f64 ly = x * _cur_ib + y * _cur_id + _cur_ig
      if _cur_texf >= 0.0 {
         if _cur_kind == 1 {
            u, v = lx / _cur_ex, 0.5
         } elif _cur_kind == 2 {
            u, v = lx / (_cur_f * 2.0) + 0.5, ly / (_cur_f * 2.0) + 0.5
         } elif _cur_kind == 3 {
            u, v = lx / ((_cur_ex + _cur_f) * 2.0) + 0.5, ly / ((_cur_ey + _cur_f) * 2.0) + 0.5
         } else {
            u, v = lx / _cur_ex, ly / _cur_ey
         }
         col = _pc(1.0, 1.0, 1.0, _cur_al * cover)
      } else {
         ;; No texture (mock renderer): shade the gradient at the vertex.
         def f64 t = _paint_t(_paint, lx, ly)
         def f64 s = 1.0 - t
         col = _pc(
            float(_paint.get(11, 1.0)) * s + float(_paint.get(15, 1.0)) * t,
            float(_paint.get(12, 1.0)) * s + float(_paint.get(16, 1.0)) * t,
            float(_paint.get(13, 1.0)) * s + float(_paint.get(17, 1.0)) * t,
            (float(_paint.get(14, 1.0)) * s + float(_paint.get(18, 1.0)) * t) * _cur_al * cover)
      }
   }
   def int vtex = _cur_texf >= 0.0 ? int(_cur_texf) : 0
   def ptr at = _vb + _vb_n * 64
   store32_f32(at, x, shared.OFF_X)
   store32_f32(at, y, shared.OFF_Y)
   store32_f32(at, u, shared.OFF_U)
   store32_f32(at, v, shared.OFF_V)
   store32(at, col, shared.OFF_C)
   store32(at, vtex, shared.OFF_TEX)
   _vb_n += 1
}

fn _tri(f64 x0, f64 y0, f64 c0, f64 x1, f64 y1, f64 c1, f64 x2, f64 y2, f64 c2) any {
   if !_reserve(3) { return 0 }
   _v(x0, y0, c0)
   _v(x1, y1, c1)
   _v(x2, y2, c2)
}

fn _quad(f64 x0, f64 y0, f64 c0, f64 x1, f64 y1, f64 c1, f64 x2, f64 y2, f64 c2, f64 x3, f64 y3, f64 c3) any {
   if !_reserve(6) { return 0 }
   _v(x0, y0, c0)
   _v(x1, y1, c1)
   _v(x2, y2, c2)
   _v(x0, y0, c0)
   _v(x2, y2, c2)
   _v(x3, y3, c3)
}

fn _stream_end() bool {
   if _vb_n <= 0 { return false }
   if _sc_on { gfx.scissor_push(_sc_x, _sc_y, _sc_w, _sc_h) }
   def int tex = int(_cur_texf)
   if _comp != 0 { gfx.set_blend_mode(_comp) }
   def ok = gfx.draw_vertices(_vb, _vb_n, tex)
   if _comp != 0 { gfx.set_blend_mode(0) }
   if _sc_on { gfx.scissor_pop() }
   _stat_draws += 1
   _stat_verts += _vb_n
   _vb_n = 0
   ok
}

;; ---- frame and state ------------------------------------------------------
fn begin_frame(f64 w, f64 h, f64 pixel_ratio=1.0) any {
   "Starts a canvas frame for a `w` by `h` target and resets the state. `pixel_ratio` is the framebuffer scale: it sets the width of the antialiasing fringe."
   _init()
   _view_w, _view_h = w, h
   _fringe = 1.0 / (pixel_ratio > 0.0 ? pixel_ratio : 1.0)
   _stack = []
   reset()
   _px, _py, _subs = [], [], []
   _stat_draws, _stat_verts = 0, 0
}

fn end_frame() any { "Ends the canvas frame." _px, _py, _subs = [], [], [] }

fn frame_stats() list {
   "Returns [draw calls, vertices, gradient textures baked] since begin_frame."
   return [_stat_draws, _stat_verts, _stat_bakes]
}

fn reset() any {
   "Resets the current state to its defaults."
   _init()
   _xa, _xb, _xc, _xd, _xe, _xf = 1.0, 0.0, 0.0, 1.0, 0.0, 0.0
   _fill = _solid([1.0, 1.0, 1.0, 1.0])
   _stroke = _solid([0.0, 0.0, 0.0, 1.0])
   _stroke_w, _miter = 1.0, 10.0
   _cap, _join = BUTT, MITER
   _alpha, _aa = 1.0, true
   _comp = 0
   _sc_on = false
   _align = ALIGN_LEFT | ALIGN_BASELINE
}

fn save() any {
   "Pushes a copy of the current state."
   _init()
   _stack = _stack.append([_xa, _xb, _xc, _xd, _xe, _xf, _fill, _stroke, _stroke_w, _miter, float(_cap), float(_join), _alpha, _aa,
         _sc_on, _sc_x, _sc_y, _sc_w, _sc_h, float(_font), float(_align), float(_comp)])
}

fn restore() any {
   "Pops the state pushed by the matching save."
   def int n = _stack.len
   if n <= 0 { return 0 }
   def int last = n - 1
   def any s = _stack.get(last, 0)
   mut rest = []
   mut i = 0
   while i < last {
      rest = rest.append(_stack.get(i, 0))
      i += 1
   }
   _stack = rest
   _xa, _xb, _xc = float(s.get(0, 1.0)), float(s.get(1, 0.0)), float(s.get(2, 0.0))
   _xd, _xe, _xf = float(s.get(3, 1.0)), float(s.get(4, 0.0)), float(s.get(5, 0.0))
   _fill = s.get(6, 0)
   _stroke = s.get(7, 0)
   _stroke_w, _miter = float(s.get(8, 1.0)), float(s.get(9, 10.0))
   _cap, _join = int(float(s.get(10, 0.0))), int(float(s.get(11, 4.0)))
   _alpha, _aa = float(s.get(12, 1.0)), bool(s.get(13, true))
   _sc_on = bool(s.get(14, false))
   _sc_x, _sc_y = float(s.get(15, 0.0)), float(s.get(16, 0.0))
   _sc_w, _sc_h = float(s.get(17, 0.0)), float(s.get(18, 0.0))
   _font, _align = int(float(s.get(19, 0.0))), int(float(s.get(20, 65.0)))
   _comp = int(float(s.get(21, 0.0)))
}

fn stroke_color(any rgba) any { "Sets a solid stroke color." _init() _stroke = _solid(rgba) }

fn fill_color(any rgba) any { "Sets a solid fill color." _init() _fill = _solid(rgba) }

fn stroke_paint(list paint) any { "Strokes with a gradient or pattern paint." if paint.len >= 21 { _stroke = paint } }

fn fill_paint(list paint) any { "Fills with a gradient or pattern paint." if paint.len >= 21 { _fill = paint } }

fn stroke_width(f64 w) any { "Sets the stroke width in canvas units." _stroke_w = w < 0.0 ? 0.0 : w }

fn miter_limit(f64 limit) any { "Sets the ratio at which a miter join falls back to a bevel." _miter = limit < 1.0 ? 1.0 : limit }

fn line_cap(int cap) any { "Sets the cap of open strokes: BUTT, ROUND, or SQUARE." if cap == BUTT || cap == ROUND || cap == SQUARE { _cap = cap } }

fn line_join(int join) any { "Sets the join of stroke corners: MITER, ROUND, or BEVEL." if join == MITER || join == ROUND || join == BEVEL { _join = join } }

fn global_alpha(f64 a) any { "Scales the alpha of everything drawn afterwards." _alpha = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a) }

fn shape_anti_alias(bool on) any { "Turns the antialiasing fringe on or off." _aa = on }

fn global_composite_operation(int op) bool {
   "Selects the Porter-Duff operator for later fills and strokes: SOURCE_OVER, SOURCE_IN, SOURCE_OUT, ATOP, DESTINATION_OVER, DESTINATION_IN, DESTINATION_OUT, DESTINATION_ATOP, LIGHTER, COPY, or XOR. It applies where geometry is drawn(as in NanoVG). Returns false and keeps the current operator for anything else."
   if op < SOURCE_OVER || op > XOR { return false }
   _comp = op
   true
}

;; ---- transforms -----------------------------------------------------------
fn reset_transform() any { "Resets the transform to the identity." _xa, _xb, _xc, _xd, _xe, _xf = 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 }

fn transform(f64 a, f64 b, f64 c, f64 d, f64 e, f64 f) any {
   "Premultiplies the current transform by the affine matrix [a c e; b d f]."
   def f64 na, nb = a * _xa + b * _xc, a * _xb + b * _xd
   def f64 nc, nd = c * _xa + d * _xc, c * _xb + d * _xd
   def f64 ne, nf = e * _xa + f * _xc + _xe, e * _xb + f * _xd + _xf
   _xa, _xb, _xc, _xd, _xe, _xf = na, nb, nc, nd, ne, nf
}

fn translate(f64 x, f64 y) any { "Moves the origin." transform(1.0, 0.0, 0.0, 1.0, x, y) }

fn rotate(f64 angle) any {
   "Rotates by `angle` radians."
   def f64 cs, sn = float(cos(angle)), float(sin(angle))
   transform(cs, sn, 0.0 - sn, cs, 0.0, 0.0)
}

fn scale(f64 x, f64 y) any { "Scales the axes." transform(x, 0.0, 0.0, y, 0.0, 0.0) }

fn skew_x(f64 angle) any { "Skews along x by `angle` radians." transform(1.0, 0.0, float(tan(angle)), 1.0, 0.0, 0.0) }

fn skew_y(f64 angle) any { "Skews along y by `angle` radians." transform(1.0, float(tan(angle)), 0.0, 1.0, 0.0, 0.0) }

fn current_transform() list {
   "Returns the transform as [a, b, c, d, e, f]."
   return [_xa, _xb, _xc, _xd, _xe, _xf]
}

fn _avg_scale() f64 {
   def f64 sx = float(sqrt(_xa * _xa + _xc * _xc))
   def f64 sy = float(sqrt(_xb * _xb + _xd * _xd))
   (sx + sy) * 0.5
}

;; ---- scissor --------------------------------------------------------------
fn _scissor_box(f64 x, f64 y, f64 w, f64 h) list {
   ;; Bounding box of the transformed rectangle.
   mut f64 x0, y0 = x * _xa + y * _xc + _xe, x * _xb + y * _xd + _xf
   mut f64 x1, y1 = x0, y0
   mut k = 1
   while k < 4 {
      def f64 cx = (k == 1 || k == 2) ? (x + w) : x
      def f64 cy = k >= 2 ? (y + h) : y
      def f64 tx, ty = cx * _xa + cy * _xc + _xe, cx * _xb + cy * _xd + _xf
      if tx < x0 { x0 = tx }
      if tx > x1 { x1 = tx }
      if ty < y0 { y0 = ty }
      if ty > y1 { y1 = ty }
      k += 1
   }
   [x0, y0, x1 - x0, y1 - y0]
}

fn scissor(f64 x, f64 y, f64 w, f64 h) any {
   "Clips drawing to a rectangle given in the current transform."
   def b = _scissor_box(x, y, w, h)
   _sc_on = true
   _sc_x, _sc_y = float(b.get(0, 0.0)), float(b.get(1, 0.0))
   _sc_w, _sc_h = float(b.get(2, 0.0)), float(b.get(3, 0.0))
}

fn intersect_scissor(f64 x, f64 y, f64 w, f64 h) any {
   "Narrows the scissor to its intersection with another rectangle."
   if !_sc_on { return scissor(x, y, w, h) }
   def b = _scissor_box(x, y, w, h)
   def f64 bx, by = float(b.get(0, 0.0)), float(b.get(1, 0.0))
   def f64 x0, y0 = _sc_x > bx ? _sc_x : bx, _sc_y > by ? _sc_y : by
   def f64 r0, r1 = _sc_x + _sc_w, bx + float(b.get(2, 0.0))
   def f64 b0, b1 = _sc_y + _sc_h, by + float(b.get(3, 0.0))
   def f64 x1, y1 = r0 < r1 ? r0 : r1, b0 < b1 ? b0 : b1
   _sc_x, _sc_y = x0, y0
   _sc_w, _sc_h = x1 > x0 ? (x1 - x0) : 0.0, y1 > y0 ? (y1 - y0) : 0.0
}

fn reset_scissor() any { "Removes the scissor." _sc_on = false }

;; ---- path building --------------------------------------------------------
fn begin_path() any {
   "Clears the current path."
   _init()
   _px, _py, _subs = [], [], []
}

fn _add_point(f64 x, f64 y) any {
   ;; Points are stored already transformed, and a point equal to the last
   ;; one is dropped so no segment has zero length.
   def f64 tx, ty = x * _xa + y * _xc + _xe, x * _xb + y * _xd + _xf
   _pen_x, _pen_y = x, y
   def int n = _px.len
   def int sn = _subs.len
   if sn <= 0 { return 0 }
   def int cnt_i = sn - 3
   def int cnt = int(float(_subs.get(cnt_i, 0.0)))
   if cnt > 0 {
      def int last = n - 1
      def f64 ddx, ddy = float(_px.get(last, 0.0)) - tx, float(_py.get(last, 0.0)) - ty
      if ddx * ddx + ddy * ddy < 0.0001 { return 0 }
   }
   _px = _px.append(tx)
   _py = _py.append(ty)
   _subs[cnt_i] = float(cnt + 1)
}

fn move_to(f64 x, f64 y) any {
   "Starts a new subpath at(x, y)."
   _init()
   _subs = _subs.append(float(_px.len))
   _subs = _subs.append(0.0)
   _subs = _subs.append(0.0)
   _subs = _subs.append(float(SOLID))
   _add_point(x, y)
}

fn line_to(f64 x, f64 y) any {
   "Adds a line from the pen to(x, y)."
   if _subs.len <= 0 { return move_to(x, y) }
   _add_point(x, y)
}

fn bezier_to(f64 c1x, f64 c1y, f64 c2x, f64 c2y, f64 x, f64 y) any {
   "Adds a cubic bezier from the pen through two control points to(x, y)."
   if _subs.len <= 0 { move_to(c1x, c1y) }
   def f64 x0, y0 = _pen_x, _pen_y
   ;; Segment count from the control polygon length on screen.
   def f64 sc = _avg_scale()
   def f64 l1 = float(sqrt((c1x - x0) * (c1x - x0) + (c1y - y0) * (c1y - y0)))
   def f64 l2 = float(sqrt((c2x - c1x) * (c2x - c1x) + (c2y - c1y) * (c2y - c1y)))
   def f64 l3 = float(sqrt((x - c2x) * (x - c2x) + (y - c2y) * (y - c2y)))
   mut int n = int(float(sqrt((l1 + l2 + l3) * sc)) * 0.9) + 2
   if n > 64 { n = 64 }
   mut int i = 1
   while i <= n {
      def f64 t = float(i) / float(n)
      def f64 s = 1.0 - t
      def f64 b0, b1 = s * s * s, 3.0 * s * s * t
      def f64 b2, b3 = 3.0 * s * t * t, t * t * t
      _add_point(b0 * x0 + b1 * c1x + b2 * c2x + b3 * x, b0 * y0 + b1 * c1y + b2 * c2y + b3 * y)
      i += 1
   }
}

fn quad_to(f64 cx, f64 cy, f64 x, f64 y) any {
   "Adds a quadratic bezier from the pen through one control point to(x, y)."
   def f64 x0, y0 = _pen_x, _pen_y
   bezier_to(x0 + 2.0 / 3.0 * (cx - x0), y0 + 2.0 / 3.0 * (cy - y0), x + 2.0 / 3.0 * (cx - x), y + 2.0 / 3.0 * (cy - y), x, y)
}

fn catmull_rom(list points) any {
   "Starts a subpath through the interior of a uniform Catmull-Rom point list.
   Each point is [x, y] ; the first and last points control the end tangents.
   Fewer than four points leave the path unchanged."
   if points.len < 4 { return 0 }
   def p = points.get(1)
   move_to(float(p.get(0)), float(p.get(1)))
   mut int i = 0
   while i + 3 < points.len {
      def a, b, c, d = points.get(i), points.get(i + 1), points.get(i + 2), points.get(i + 3)
      def f64 bx, by = float(b.get(0)), float(b.get(1))
      def f64 cx, cy = float(c.get(0)), float(c.get(1))
      bezier_to(bx + (cx - float(a.get(0))) / 6.0, by + (cy - float(a.get(1))) / 6.0,
         cx - (float(d.get(0)) - bx) / 6.0, cy - (float(d.get(1)) - by) / 6.0, cx, cy)
      i += 1
   }
}

fn bspline(list points) any {
   "Starts a subpath for a uniform cubic B-spline through a control point list.
   Each point is [x, y] ; fewer than four points leave the path unchanged."
   if points.len < 4 { return 0 }
   def a, b, c = points.get(0), points.get(1), points.get(2)
   move_to((float(a.get(0)) + 4.0 * float(b.get(0)) + float(c.get(0))) / 6.0,
      (float(a.get(1)) + 4.0 * float(b.get(1)) + float(c.get(1))) / 6.0)
   mut int i = 0
   while i + 3 < points.len {
      def p, q, r = points.get(i + 1), points.get(i + 2), points.get(i + 3)
      def f64 px, py = float(p.get(0)), float(p.get(1))
      def f64 qx, qy = float(q.get(0)), float(q.get(1))
      bezier_to((2.0 * px + qx) / 3.0, (2.0 * py + qy) / 3.0,
         (px + 2.0 * qx) / 3.0, (py + 2.0 * qy) / 3.0,
         (px + 4.0 * qx + float(r.get(0))) / 6.0, (py + 4.0 * qy + float(r.get(1))) / 6.0)
      i += 1
   }
}

fn close_path() any {
   "Closes the current subpath."
   def int sn = _subs.len
   if sn <= 0 { return 0 }
   def int ci = sn - 2
   _subs[ci] = 1.0
}

fn path_winding(int dir) any {
   "Marks the current subpath as SOLID(the default) or as a HOLE cut out of the shapes around it."
   def int sn = _subs.len
   if sn <= 0 { return 0 }
   def int wi = sn - 1
   _subs[wi] = float(dir == HOLE ? HOLE : SOLID)
}

fn arc(f64 cx, f64 cy, f64 r, f64 a0, f64 a1, int dir=CW) any {
   "Adds a circular arc around(cx, cy) from angle a0 to a1(radians), clockwise(CW) or counter-clockwise(CCW). It connects to the pen with a line when a subpath is open."
   mut f64 da = a1 - a0
   if dir == CW {
      if abs(da) >= PI * 2.0 { da = PI * 2.0 }
      else { while da < 0.0 { da = da + PI * 2.0 } }
   } else {
      if abs(da) >= PI * 2.0 { da = 0.0 - PI * 2.0 }
      else { while da > 0.0 { da = da - PI * 2.0 } }
   }
   def f64 sr = r * _avg_scale()
   mut int n = int(abs(da) * float(sqrt(sr < 1.0 ? 1.0 : sr)) * 0.9) + 2
   if n > 96 { n = 96 }
   mut int i = 0
   while i <= n {
      def f64 a = a0 + da * (float(i) / float(n))
      def f64 x, y = cx + float(cos(a)) * r, cy + float(sin(a)) * r
      if i == 0 { line_to(x, y) }
      else { _add_point(x, y) }
      i += 1
   }
}

fn arc_to(f64 x1, f64 y1, f64 x2, f64 y2, f64 radius) any {
   "Adds an arc of `radius` tangent to the lines pen-(x1, y1) and(x1, y1)-(x2, y2), joined to the pen by a line."
   if _subs.len <= 0 { return move_to(x1, y1) }
   def f64 x0, y0 = _pen_x, _pen_y
   mut f64 dx0, dy0 = x0 - x1, y0 - y1
   mut f64 dx1, dy1 = x2 - x1, y2 - y1
   def f64 l0 = float(sqrt(dx0 * dx0 + dy0 * dy0))
   def f64 l1 = float(sqrt(dx1 * dx1 + dy1 * dy1))
   if l0 < 0.0001 || l1 < 0.0001 || radius < 0.0001 { return line_to(x1, y1) }
   dx0, dy0 = dx0 / l0, dy0 / l0
   dx1, dy1 = dx1 / l1, dy1 / l1
   mut f64 cosang = dx0 * dx1 + dy0 * dy1
   if cosang > 0.9999 || cosang < -0.9999 { return line_to(x1, y1) }
   def f64 ang = float(acos(cosang))
   def f64 d = radius / float(tan(ang * 0.5))
   def f64 cross = dx1 * dy0 - dx0 * dy1
   mut f64 cx, cy, a0, a1 = 0.0, 0.0, 0.0, 0.0
   mut int dir = CW
   if cross > 0.0 {
      cx, cy = x1 + dx0 * d + dy0 * radius, y1 + dy0 * d - dx0 * radius
      a0, a1 = float(atan2(dx0, 0.0 - dy0)), float(atan2(0.0 - dx1, dy1))
      dir = CW
   } else {
      cx, cy = x1 + dx0 * d - dy0 * radius, y1 + dy0 * d + dx0 * radius
      a0, a1 = float(atan2(0.0 - dx0, dy0)), float(atan2(dx1, 0.0 - dy1))
      dir = CCW
   }
   arc(cx, cy, radius, a0, a1, dir)
}

fn rect(f64 x, f64 y, f64 w, f64 h) any {
   "Adds a closed rectangle subpath."
   move_to(x, y)
   line_to(x, y + h)
   line_to(x + w, y + h)
   line_to(x + w, y)
   close_path()
}

fn rounded_rect_varying(f64 x, f64 y, f64 w, f64 h, f64 rtl, f64 rtr, f64 rbr, f64 rbl) any {
   "Adds a closed rounded rectangle with one radius per corner(top-left, top-right, bottom-right, bottom-left)."
   def f64 hw, hh = abs(w) * 0.5, abs(h) * 0.5
   def f64 lim = hw < hh ? hw : hh
   def f64 tl, tr = rtl < lim ? rtl : lim, rtr < lim ? rtr : lim
   def f64 br, bl = rbr < lim ? rbr : lim, rbl < lim ? rbl : lim
   if tl < 0.1 && tr < 0.1 && br < 0.1 && bl < 0.1 { return rect(x, y, w, h) }
   move_to(x, y + tl)
   line_to(x, y + h - bl)
   if bl >= 0.1 { arc(x + bl, y + h - bl, bl, PI, PI * 0.5, CCW) } else { line_to(x, y + h) }
   line_to(x + w - br, y + h)
   if br >= 0.1 { arc(x + w - br, y + h - br, br, PI * 0.5, 0.0, CCW) } else { line_to(x + w, y + h) }
   line_to(x + w, y + tr)
   if tr >= 0.1 { arc(x + w - tr, y + tr, tr, 0.0, 0.0 - PI * 0.5, CCW) } else { line_to(x + w, y) }
   line_to(x + tl, y)
   if tl >= 0.1 { arc(x + tl, y + tl, tl, 0.0 - PI * 0.5, 0.0 - PI, CCW) } else { line_to(x, y) }
   close_path()
}

fn rounded_rect(f64 x, f64 y, f64 w, f64 h, f64 r) any { "Adds a closed rounded rectangle." rounded_rect_varying(x, y, w, h, r, r, r, r) }

fn ellipse(f64 cx, f64 cy, f64 rx, f64 ry) any {
   "Adds a closed ellipse."
   def f64 sr = (rx > ry ? rx : ry) * _avg_scale()
   mut int n = int(float(sqrt(sr < 1.0 ? 1.0 : sr)) * 5.6) + 8
   if n > 128 { n = 128 }
   mut int i = 0
   while i < n {
      def f64 a = PI * 2.0 * (float(i) / float(n))
      def f64 x, y = cx + float(cos(a)) * rx, cy + float(sin(a)) * ry
      if i == 0 { move_to(x, y) }
      else { _add_point(x, y) }
      i += 1
   }
   close_path()
}

fn circle(f64 cx, f64 cy, f64 r) any { "Adds a closed circle." ellipse(cx, cy, r, r) }

;; ---- fill -----------------------------------------------------------------
fn _sub_area(int first, int cnt) f64 {
   mut f64 a = 0.0
   mut int i = 0
   while i < cnt {
      def int i0 = first + i
      def int i1 = first + ((i + 1) % cnt)
      a = a + float(_px.get(i0, 0.0)) * float(_py.get(i1, 0.0)) - float(_px.get(i1, 0.0)) * float(_py.get(i0, 0.0))
      i += 1
   }
   a * 0.5
}

fn _sub_convex(int first, int cnt) bool {
   if cnt < 3 { return false }
   mut int sign = 0
   mut int i = 0
   while i < cnt {
      def int i0 = first + i
      def int i1 = first + ((i + 1) % cnt)
      def int i2 = first + ((i + 2) % cnt)
      def f64 ax, ay = float(_px.get(i1, 0.0)) - float(_px.get(i0, 0.0)), float(_py.get(i1, 0.0)) - float(_py.get(i0, 0.0))
      def f64 bx, by = float(_px.get(i2, 0.0)) - float(_px.get(i1, 0.0)), float(_py.get(i2, 0.0)) - float(_py.get(i1, 0.0))
      def f64 cr = ax * by - ay * bx
      if cr > 0.000001 {
         if sign < 0 { return false }
         sign = 1
      } elif cr < -0.000001 {
         if sign > 0 { return false }
         sign = -1
      }
      i += 1
   }
   sign != 0
}

fn _fill_fringe(int first, int cnt, f64 orient) any {
   ;; A strip one fringe wide outside the contour, fading to transparent.
   ;; Each vertex is pushed along the mitered average of its two edge normals.
   mut f64 pox, poy, pix, piy = 0.0, 0.0, 0.0, 0.0
   mut f64 fox, foy, fix, fiy = 0.0, 0.0, 0.0, 0.0
   mut int i = 0
   while i <= cnt {
      def int k = i % cnt
      def int ip = first + ((k + cnt - 1) % cnt)
      def int ic = first + k
      def int inx = first + ((k + 1) % cnt)
      def f64 x, y = float(_px.get(ic, 0.0)), float(_py.get(ic, 0.0))
      mut f64 ax, ay = x - float(_px.get(ip, 0.0)), y - float(_py.get(ip, 0.0))
      mut f64 bx, by = float(_px.get(inx, 0.0)) - x, float(_py.get(inx, 0.0)) - y
      def f64 la = float(sqrt(ax * ax + ay * ay))
      def f64 lb = float(sqrt(bx * bx + by * by))
      if la > 0.000001 { ax, ay = ax / la, ay / la }
      if lb > 0.000001 { bx, by = bx / lb, by / lb }
      ;; Outward normal of an edge (dx, dy) is (dy, -dx) for positive area.
      mut f64 nx, ny = (ay + by) * 0.5 * orient, (0.0 - ax - bx) * 0.5 * orient
      def f64 nl2 = nx * nx + ny * ny
      mut f64 sc = nl2 > 0.000001 ? (1.0 / nl2) : 1.0
      if sc > 8.0 { sc = 8.0 }
      def f64 ox, oy = x + nx * sc * _fringe, y + ny * sc * _fringe
      if i == 0 {
         fox, foy, fix, fiy = ox, oy, x, y
      } else {
         _quad(pix, piy, 1.0, pox, poy, 0.0, ox, oy, 0.0, x, y, 1.0)
      }
      pox, poy, pix, piy = ox, oy, x, y
      i += 1
   }
}

fn _fill_scanline() any {
   ;; Nonzero-rule trapezoid decomposition: gather the edges, cut the plane
   ;; at every vertex y, and inside each slab walk the edges left to right.
   mut ex0 = []
   mut ey0 = []
   mut ex1 = []
   mut ey1 = []
   mut edir = []
   mut ys = []
   mut int s = 0
   def int sn = _subs.len
   while s + 3 < sn {
      def int s1, s3 = s + 1, s + 3
      def int first = int(float(_subs.get(s, 0.0)))
      def int cnt = int(float(_subs.get(s1, 0.0)))
      if cnt >= 3 {
         ;; A HOLE winds against the solids whatever order it was drawn in.
         def f64 area = _sub_area(first, cnt)
         def bool hole = int(float(_subs.get(s3, 1.0))) == HOLE
         def f64 flip = ((area < 0.0) != hole) ? -1.0 : 1.0
         mut int i = 0
         while i < cnt {
            def int i0 = first + i
            def int i1 = first + ((i + 1) % cnt)
            def f64 xa, ya = float(_px.get(i0, 0.0)), float(_py.get(i0, 0.0))
            def f64 xb, yb = float(_px.get(i1, 0.0)), float(_py.get(i1, 0.0))
            ys = ys.append(ya)
            if ya < yb {
               ex0 = ex0.append(xa) ey0 = ey0.append(ya) ex1 = ex1.append(xb) ey1 = ey1.append(yb) edir = edir.append(flip)
            } elif yb < ya {
               ex0 = ex0.append(xb) ey0 = ey0.append(yb) ex1 = ex1.append(xa) ey1 = ey1.append(ya) edir = edir.append(0.0 - flip)
            }
            i += 1
         }
      }
      s += 4
   }
   ;; Shell sort of the cut lines.
   def int yn = ys.len
   mut int gap = yn / 2
   while gap > 0 {
      mut int i = gap
      while i < yn {
         def f64 v = float(ys.get(i, 0.0))
         mut int j = i
         mut bool moving = true
         while j >= gap && moving {
            def int jg = j - gap
            if float(ys.get(jg, 0.0)) > v {
               ys[j] = float(ys.get(jg, 0.0))
               j = jg
            } else { moving = false }
         }
         ys[j] = v
         i += 1
      }
      gap = gap / 2
   }
   def int en = ex0.len
   mut int yi = 0
   while yi + 1 < yn {
      def int yi1 = yi + 1
      def f64 ya, yb = float(ys.get(yi, 0.0)), float(ys.get(yi1, 0.0))
      if yb - ya > 0.0001 {
         def f64 ym = (ya + yb) * 0.5
         ;; Active edges in this slab: x at the top, x at the bottom, winding.
         mut axa = []
         mut axb = []
         mut adir = []
         mut int e = 0
         while e < en {
            def f64 y0, y1 = float(ey0.get(e, 0.0)), float(ey1.get(e, 0.0))
            if y0 <= ym && ym < y1 {
               def f64 x0, x1 = float(ex0.get(e, 0.0)), float(ex1.get(e, 0.0))
               def f64 inv = 1.0 / (y1 - y0)
               axa = axa.append(x0 + (x1 - x0) * (ya - y0) * inv)
               axb = axb.append(x0 + (x1 - x0) * (yb - y0) * inv)
               adir = adir.append(float(edir.get(e, 1.0)))
            }
            e += 1
         }
         ;; Insertion sort by the x at the middle of the slab.
         def int an = axa.len
         mut int i = 1
         while i < an {
            def f64 va, vb = float(axa.get(i, 0.0)), float(axb.get(i, 0.0))
            def f64 vd = float(adir.get(i, 1.0))
            mut int j = i
            mut bool moving = true
            while j > 0 && moving {
               def int jp = j - 1
               if float(axa.get(jp, 0.0)) + float(axb.get(jp, 0.0)) > va + vb {
                  axa[j] = float(axa.get(jp, 0.0))
                  axb[j] = float(axb.get(jp, 0.0))
                  adir[j] = float(adir.get(jp, 1.0))
                  j = jp
               } else { moving = false }
            }
            axa[j] = va
            axb[j] = vb
            adir[j] = vd
            i += 1
         }
         mut f64 wind = 0.0
         i = 0
         while i + 1 < an {
            def int i1 = i + 1
            wind = wind + float(adir.get(i, 1.0))
            if wind != 0.0 {
               def f64 la, lb = float(axa.get(i, 0.0)), float(axb.get(i, 0.0))
               def f64 ra, rb = float(axa.get(i1, 0.0)), float(axb.get(i1, 0.0))
               _quad(la, ya, 1.0, ra, ya, 1.0, rb, yb, 1.0, lb, yb, 1.0)
            }
            i += 1
         }
      }
      yi += 1
   }
}

fn fill() bool {
   "Fills the current path with the fill paint. Subpaths are closed implicitly; overlapping solids merge and HOLE subpaths cut out(nonzero rule)."
   _init()
   def int sn = _subs.len
   if sn < 4 { return false }
   _stream_begin(_fill, 1.0)
   mut int solid_subs = 0
   mut int s = 0
   while s + 3 < sn {
      def int s1 = s + 1
      if int(float(_subs.get(s1, 0.0))) >= 3 { solid_subs += 1 }
      s += 4
   }
   def int first0 = int(float(_subs.get(0, 0.0)))
   def int cnt0 = int(float(_subs.get(1, 0.0)))
   if solid_subs == 1 && sn == 4 && _sub_convex(first0, cnt0) {
      def f64 x0, y0 = float(_px.get(first0, 0.0)), float(_py.get(first0, 0.0))
      mut int i = 1
      while i + 1 < cnt0 {
         def int i1 = first0 + i
         def int i2 = i1 + 1
         _tri(x0, y0, 1.0, float(_px.get(i1, 0.0)), float(_py.get(i1, 0.0)), 1.0, float(_px.get(i2, 0.0)), float(_py.get(i2, 0.0)), 1.0)
         i += 1
      }
   } else {
      _fill_scanline()
   }
   if _aa {
      s = 0
      while s + 3 < sn {
         def int s1, s3 = s + 1, s + 3
         def int first = int(float(_subs.get(s, 0.0)))
         def int cnt = int(float(_subs.get(s1, 0.0)))
         if cnt >= 3 {
            ;; The fringe grows away from the filled side: outward for a
            ;; solid, into the hole for a hole.
            def f64 area = _sub_area(first, cnt)
            def bool hole = int(float(_subs.get(s3, 1.0))) == HOLE
            mut f64 orient = area < 0.0 ? -1.0 : 1.0
            if hole { orient = 0.0 - orient }
            _fill_fringe(first, cnt, orient)
         }
         s += 4
      }
   }
   _stream_end()
}

;; ---- stroke ---------------------------------------------------------------
mut any _sx = nil
mut any _sy = nil
mut any _sk = nil

fn _stroke_chain(int n, bool closed, f64 side, f64 dist, int join) list {
   "Offsets the working polyline to one side. Every vertex contributes the number of points recorded in _sk, so two chains built with different distances or sides pair up point by point."
   mut out = []
   mut int i = 0
   while i < n {
      def f64 x, y = float(_sx.get(i, 0.0)), float(_sy.get(i, 0.0))
      def int ip = (i + n - 1) % n
      def int inx = (i + 1) % n
      def bool has_prev = closed || i > 0
      def bool has_next = closed || i < n - 1
      mut f64 ax, ay, bx, by = 0.0, 0.0, 0.0, 0.0
      if has_prev {
         ax, ay = x - float(_sx.get(ip, 0.0)), y - float(_sy.get(ip, 0.0))
         def f64 l = float(sqrt(ax * ax + ay * ay))
         if l > 0.000001 { ax, ay = ax / l, ay / l }
      }
      if has_next {
         bx, by = float(_sx.get(inx, 0.0)) - x, float(_sy.get(inx, 0.0)) - y
         def f64 l = float(sqrt(bx * bx + by * by))
         if l > 0.000001 { bx, by = bx / l, by / l }
      }
      if !has_prev { ax, ay = bx, by }
      if !has_next { bx, by = ax, ay }
      def f64 n0x, n0y = ay * side, (0.0 - ax) * side
      def f64 n1x, n1y = by * side, (0.0 - bx) * side
      def int k = int(float(_sk.get(i, 1.0)))
      mut f64 mx, my = (n0x + n1x) * 0.5, (n0y + n1y) * 0.5
      def f64 mr2 = mx * mx + my * my
      mut f64 sc = mr2 > 0.000001 ? (1.0 / mr2) : 1.0
      def f64 cross = bx * ay - ax * by
      ;; The side the path turns away from is the outside of the corner.
      def bool outer = (cross * side) < 0.0
      if k <= 1 || !outer {
         ;; A single mitered point; on the inside of a sharp corner it is
         ;; capped so thin, tight shapes do not fold over themselves.
         if sc > 4.0 && (k > 1 || !outer) { sc = 4.0 }
         mut int r = 0
         while r < k {
            out = out.append(x + mx * sc * dist)
            out = out.append(y + my * sc * dist)
            r += 1
         }
      } elif join == ROUND && k > 2 {
         def f64 a0 = float(atan2(n0y, n0x))
         mut f64 da = float(atan2(n1y, n1x)) - a0
         while da > PI { da = da - PI * 2.0 }
         while da < 0.0 - PI { da = da + PI * 2.0 }
         mut int r = 0
         while r < k {
            def f64 a = a0 + da * (float(r) / float(k - 1))
            out = out.append(x + float(cos(a)) * dist)
            out = out.append(y + float(sin(a)) * dist)
            r += 1
         }
      } else {
         ;; Bevel: the two edge offsets, repeated to fill the point count.
         mut int r = 0
         while r < k {
            if r * 2 < k {
               out = out.append(x + n0x * dist)
               out = out.append(y + n0y * dist)
            } else {
               out = out.append(x + n1x * dist)
               out = out.append(y + n1y * dist)
            }
            r += 1
         }
      }
      i += 1
   }
   out
}

fn _stroke_cap_round(f64 x, f64 y, f64 dx, f64 dy, f64 w, f64 fr) any {
   ;; Half disc around an end point, facing along (dx, dy), with its fringe.
   def f64 a0 = float(atan2(dy, dx)) - PI * 0.5
   mut int n = int(w * 0.9) + 6
   if n > 32 { n = 32 }
   mut int i = 0
   while i < n {
      def f64 a = a0 + PI * (float(i) / float(n))
      def f64 b = a0 + PI * (float(i + 1) / float(n))
      def f64 ca, sa = float(cos(a)), float(sin(a))
      def f64 cb, sb = float(cos(b)), float(sin(b))
      _tri(x, y, 1.0, x + ca * w, y + sa * w, 1.0, x + cb * w, y + sb * w, 1.0)
      if fr > 0.0 { _quad(x + ca * w, y + sa * w, 1.0, x + ca * (w + fr), y + sa * (w + fr), 0.0, x + cb * (w + fr), y + sb * (w + fr), 0.0, x + cb * w, y + sb * w, 1.0) }
      i += 1
   }
}

fn _stroke_sub(int first, int cnt, bool closed, f64 w, f64 fr) any {
   ;; Working copy of the subpath; square caps extend the two end points.
   _sx, _sy, _sk = [], [], []
   mut int i = 0
   while i < cnt {
      def int idx = first + i
      _sx = _sx.append(float(_px.get(idx, 0.0)))
      _sy = _sy.append(float(_py.get(idx, 0.0)))
      i += 1
   }
   mut int n = cnt
   if closed && n >= 2 {
      ;; A closing point equal to the first would make a zero-length edge.
      def int last = n - 1
      def f64 ddx, ddy = float(_sx.get(last, 0.0)) - float(_sx.get(0, 0.0)), float(_sy.get(last, 0.0)) - float(_sy.get(0, 0.0))
      if ddx * ddx + ddy * ddy < 0.0001 { n -= 1 }
   }
   if n < 2 { return 0 }
   def int last = n - 1
   def int prev_last = n - 2
   mut f64 sdx, sdy = float(_sx.get(1, 0.0)) - float(_sx.get(0, 0.0)), float(_sy.get(1, 0.0)) - float(_sy.get(0, 0.0))
   mut f64 edx, edy = float(_sx.get(last, 0.0)) - float(_sx.get(prev_last, 0.0)), float(_sy.get(last, 0.0)) - float(_sy.get(prev_last, 0.0))
   def f64 sl = float(sqrt(sdx * sdx + sdy * sdy))
   def f64 el = float(sqrt(edx * edx + edy * edy))
   if sl > 0.000001 { sdx, sdy = sdx / sl, sdy / sl }
   if el > 0.000001 { edx, edy = edx / el, edy / el }
   if !closed && _cap == SQUARE {
      _sx[0] = float(_sx.get(0, 0.0)) - sdx * w
      _sy[0] = float(_sy.get(0, 0.0)) - sdy * w
      _sx[last] = float(_sx.get(last, 0.0)) + edx * w
      _sy[last] = float(_sy.get(last, 0.0)) + edy * w
   }
   ;; How many chain points each vertex needs.
   i = 0
   while i < n {
      mut f64 k = 1.0
      if closed || (i > 0 && i < last) {
         def int ip = (i + n - 1) % n
         def int inx = (i + 1) % n
         mut f64 ax, ay = float(_sx.get(i, 0.0)) - float(_sx.get(ip, 0.0)), float(_sy.get(i, 0.0)) - float(_sy.get(ip, 0.0))
         mut f64 bx, by = float(_sx.get(inx, 0.0)) - float(_sx.get(i, 0.0)), float(_sy.get(inx, 0.0)) - float(_sy.get(i, 0.0))
         def f64 la = float(sqrt(ax * ax + ay * ay))
         def f64 lb = float(sqrt(bx * bx + by * by))
         if la > 0.000001 { ax, ay = ax / la, ay / la }
         if lb > 0.000001 { bx, by = bx / lb, by / lb }
         mut f64 dot = ax * bx + ay * by
         if dot > 1.0 { dot = 1.0 }
         if dot < -1.0 { dot = -1.0 }
         def f64 turn = float(acos(dot))
         if turn > 0.02 {
            def f64 mr2 = (1.0 + dot) * 0.5
            def f64 miter_len = mr2 > 0.000001 ? (1.0 / float(sqrt(mr2))) : 1000.0
            if _join == ROUND {
               mut int steps = int(turn / (PI / 8.0) * float(sqrt(w < 1.0 ? 1.0 : w)) * 0.5) + 2
               if steps > 24 { steps = 24 }
               k = float(steps)
            } elif _join == BEVEL || miter_len > _miter { k = 2.0 }
         }
      }
      _sk = _sk.append(k)
      i += 1
   }
   def l0 = _stroke_chain(n, closed, 1.0, w, _join)
   def r0 = _stroke_chain(n, closed, -1.0, w, _join)
   def int m = l0.len / 2
   def int segs = closed ? m : (m - 1)
   mut l1 = []
   mut r1 = []
   if fr > 0.0 {
      l1 = _stroke_chain(n, closed, 1.0, w + fr, _join)
      r1 = _stroke_chain(n, closed, -1.0, w + fr, _join)
   }
   mut int j = 0
   while j < segs {
      def int a = j * 2
      def int b = ((j + 1) % m) * 2
      def int a1, b1 = a + 1, b + 1
      def f64 lax, lay = float(l0.get(a, 0.0)), float(l0.get(a1, 0.0))
      def f64 lbx, lby = float(l0.get(b, 0.0)), float(l0.get(b1, 0.0))
      def f64 rax, ray = float(r0.get(a, 0.0)), float(r0.get(a1, 0.0))
      def f64 rbx, rby = float(r0.get(b, 0.0)), float(r0.get(b1, 0.0))
      _quad(lax, lay, 1.0, rax, ray, 1.0, rbx, rby, 1.0, lbx, lby, 1.0)
      if fr > 0.0 {
         _quad(lax, lay, 1.0, float(l1.get(a, 0.0)), float(l1.get(a1, 0.0)), 0.0, float(l1.get(b, 0.0)), float(l1.get(b1, 0.0)), 0.0, lbx, lby, 1.0)
         _quad(rax, ray, 1.0, float(r1.get(a, 0.0)), float(r1.get(a1, 0.0)), 0.0, float(r1.get(b, 0.0)), float(r1.get(b1, 0.0)), 0.0, rbx, rby, 1.0)
      }
      j += 1
   }
   if closed { return 0 }
   def f64 x0, y0 = float(_sx.get(0, 0.0)), float(_sy.get(0, 0.0))
   def f64 xe, ye = float(_sx.get(last, 0.0)), float(_sy.get(last, 0.0))
   if _cap == ROUND {
      _stroke_cap_round(x0, y0, 0.0 - sdx, 0.0 - sdy, w, fr)
      _stroke_cap_round(xe, ye, edx, edy, w, fr)
   } elif fr > 0.0 {
      ;; Flat ends still get a fringe so they are not hard-edged.
      def int e2 = (m - 1) * 2
      def int e3 = e2 + 1
      _quad(float(l0.get(0, 0.0)), float(l0.get(1, 0.0)), 1.0, float(r0.get(0, 0.0)), float(r0.get(1, 0.0)), 1.0,
         float(r0.get(0, 0.0)) - sdx * fr, float(r0.get(1, 0.0)) - sdy * fr, 0.0, float(l0.get(0, 0.0)) - sdx * fr, float(l0.get(1, 0.0)) - sdy * fr, 0.0)
      _quad(float(l0.get(e2, 0.0)), float(l0.get(e3, 0.0)), 1.0, float(r0.get(e2, 0.0)), float(r0.get(e3, 0.0)), 1.0,
         float(r0.get(e2, 0.0)) + edx * fr, float(r0.get(e3, 0.0)) + edy * fr, 0.0, float(l0.get(e2, 0.0)) + edx * fr, float(l0.get(e3, 0.0)) + edy * fr, 0.0)
   }
}

fn stroke() bool {
   "Strokes the current path with the stroke paint, width, joins, and caps."
   _init()
   def int sn = _subs.len
   if sn < 4 { return false }
   mut f64 w = _stroke_w * _avg_scale()
   mut f64 alpha_scale = 1.0
   def f64 fr = _aa ? _fringe : 0.0
   if w < _fringe {
      ;; Hairlines keep one fringe of width and fade instead of thinning.
      def f64 cover = w / _fringe
      alpha_scale = cover * cover
      w = _fringe
   }
   ;; With antialiasing the opaque core is inset so the fringe straddles the edge.
   mut f64 half = w * 0.5 - fr * 0.5
   if half < 0.0 { half = 0.0 }
   _stream_begin(_stroke, alpha_scale)
   mut int s = 0
   while s + 3 < sn {
      def int s1, s2 = s + 1, s + 2
      def int first = int(float(_subs.get(s, 0.0)))
      def int cnt = int(float(_subs.get(s1, 0.0)))
      if cnt >= 2 { _stroke_sub(first, cnt, float(_subs.get(s2, 0.0)) > 0.5, half, fr) }
      s += 4
   }
   _stream_end()
}

;; ---- text -----------------------------------------------------------------
fn font_face(int font) any { "Selects the font used by text and text_bounds." _font = font }

fn text_align(int align) any { "Sets text alignment from ALIGN_LEFT / CENTER / RIGHT and ALIGN_TOP / MIDDLE / BOTTOM / BASELINE flags." _align = align }

fn _text_origin(f64 x, f64 y, any s) list {
   def m = gfx.measure_text_fast(_font, s)
   def f64 tw = float(m.get(0, 0.0))
   def f64 line = float(gfx.font_line_height(_font))
   def f64 asc = float(gfx.font_ascent(_font))
   mut f64 ox, oy = x * _xa + y * _xc + _xe, x * _xb + y * _xd + _xf
   if (_align & ALIGN_CENTER) != 0 { ox = ox - tw * 0.5 }
   elif (_align & ALIGN_RIGHT) != 0 { ox = ox - tw }
   if (_align & ALIGN_MIDDLE) != 0 { oy = oy - line * 0.5 }
   elif (_align & ALIGN_BOTTOM) != 0 { oy = oy - line }
   elif (_align & ALIGN_BASELINE) != 0 { oy = oy - (asc > 0.0 ? asc : line * 0.8) }
   [ox, oy, tw, line]
}

fn _text_transformed() bool {
   "True when glyphs must be rotated, scaled, or skewed and the backend can do it: the GPU backends take the transform as a model matrix, the software renderer keeps upright glyphs at the transformed position."
   if _xa == 1.0 && _xb == 0.0 && _xc == 0.0 && _xd == 1.0 { return false }
   def int backend = gfx.get_backend_type()
   backend == gfx.BACKEND_VK || backend == gfx.BACKEND_GL
}

fn _text_local_origin(f64 x, f64 y, any s) list {
   ;; Alignment in the untransformed frame: [x, y, width, line height].
   def m = gfx.measure_text_fast(_font, s)
   def f64 tw = float(m.get(0, 0.0))
   def f64 line = float(gfx.font_line_height(_font))
   def f64 asc = float(gfx.font_ascent(_font))
   mut f64 ox = x
   mut f64 oy = y
   if (_align & ALIGN_CENTER) != 0 { ox = ox - tw * 0.5 }
   elif (_align & ALIGN_RIGHT) != 0 { ox = ox - tw }
   if (_align & ALIGN_MIDDLE) != 0 { oy = oy - line * 0.5 }
   elif (_align & ALIGN_BOTTOM) != 0 { oy = oy - line }
   elif (_align & ALIGN_BASELINE) != 0 { oy = oy - (asc > 0.0 ? asc : line * 0.8) }
   [ox, oy, tw, line]
}

fn text(f64 x, f64 y, any s) f64 {
   "Draws a line of text at(x, y) in the fill color and returns the x where it ends. Glyphs follow the whole transform(rotation, scale, skew) on the Vulkan and GL backends; the software renderer keeps them upright at the transformed position."
   _init()
   if _font <= 0 { return x }
   def int col = gfx.color_pack(float(_fill.get(11, 1.0)), float(_fill.get(12, 1.0)), float(_fill.get(13, 1.0)), float(_fill.get(14, 1.0)) * _alpha)
   if _text_transformed() {
      def lo = _text_local_origin(x, y, s)
      if !is_list(_text_mat) { _text_mat = mat.mat4_identity() }
      mat.mat4_identity_into(_text_mat)
      mat.mat4_set(_text_mat, 0, 0, _xa)
      mat.mat4_set(_text_mat, 1, 0, _xb)
      mat.mat4_set(_text_mat, 0, 1, _xc)
      mat.mat4_set(_text_mat, 1, 1, _xd)
      mat.mat4_set(_text_mat, 0, 3, _xe)
      mat.mat4_set(_text_mat, 1, 3, _xf)
      if _sc_on { gfx.scissor_push(_sc_x, _sc_y, _sc_w, _sc_h) }
      gfx.set_model_matrix(_text_mat)
      gfx.draw_text(_font, s, float(lo.get(0, 0.0)), float(lo.get(1, 0.0)), col)
      mat.mat4_identity_into(_text_mat)
      gfx.set_model_matrix(_text_mat)
      if _sc_on { gfx.scissor_pop() }
      return x + float(lo.get(2, 0.0))
   }
   def o = _text_origin(x, y, s)
   if _sc_on { gfx.scissor_push(_sc_x, _sc_y, _sc_w, _sc_h) }
   gfx.draw_text(_font, s, float(o.get(0, 0.0)), float(o.get(1, 0.0)), col)
   if _sc_on { gfx.scissor_pop() }
   x + float(o.get(2, 0.0))
}

fn text_bounds(f64 x, f64 y, any s) list {
   "Returns the screen rectangle [x, y, w, h] the text would cover: the bounding box of the transformed text block when glyphs follow the transform."
   _init()
   if _font <= 0 { return [x, y, 0.0, 0.0] }
   if !_text_transformed() { return _text_origin(x, y, s) }
   def lo = _text_local_origin(x, y, s)
   def f64 lx = float(lo.get(0, 0.0))
   def f64 ly = float(lo.get(1, 0.0))
   def f64 lw = float(lo.get(2, 0.0))
   def f64 lh = float(lo.get(3, 0.0))
   mut f64 min_x = 0.0
   mut f64 min_y = 0.0
   mut f64 max_x = 0.0
   mut f64 max_y = 0.0
   mut int k = 0
   while k < 4 {
      def f64 cx = (k == 1 || k == 2) ? (lx + lw) : lx
      def f64 cy = (k >= 2) ? (ly + lh) : ly
      def f64 sx = cx * _xa + cy * _xc + _xe
      def f64 sy = cx * _xb + cy * _xd + _xf
      if k == 0 || sx < min_x { min_x = sx }
      if k == 0 || sx > max_x { max_x = sx }
      if k == 0 || sy < min_y { min_y = sy }
      if k == 0 || sy > max_y { max_y = sy }
      k += 1
   }
   [min_x, min_y, max_x - min_x, max_y - min_y]
}
