;; Keywords: ui font text layout fallback atlas align outline blur fontstash render
;; Text layout over the renderer's font atlas: named fonts, fallback chains,
;; alignment and metrics, letter spacing, outline and blur, and a state stack.
;; References:
;; - core.os.ui.render
;; - core.os.ui.font.truetype
;;
;; The model follows fontstash. Fonts are added by name; any font can list
;; fallbacks that are tried, in order, for code points it does not cover.
;; Drawing and measuring go through the current state:
;;
;;   def ui = fs.add_font("ui", "etc/assets/fonts/maplemono.ttf", 15)
;;   def sym = fs.add_font("symbols", "etc/assets/fonts/jetbrains.ttf", 15)
;;   fs.add_fallback(ui, sym)
;;   fs.set_font(ui)
;;   fs.set_align(fs.ALIGN_CENTER | fs.ALIGN_MIDDLE)
;;   fs.draw_text(cx, cy, "hello")
;;
;; Feature matrix:
;;   fonts        add_font, font_by_name, font_count; each is one rasterised size
;;   fallback     add_fallback chains; has_glyph, resolve, runs
;;   atlas        glyphs are rasterised on demand into the renderer's atlas,
;;                which grows as new glyphs arrive (prepare warms it)
;;   metrics      vert_metrics, text_width, text_bounds
;;   alignment    ALIGN_LEFT / CENTER / RIGHT, ALIGN_TOP / MIDDLE / BOTTOM / BASELINE
;;   style        set_color, set_spacing, set_outline, set_blur
;;   state        push_state, pop_state, clear_state
module core.os.ui.render.fontstash(
   add_font, font_by_name, font_count, add_fallback, has_glyph, resolve, runs, prepare,
   push_state, pop_state, clear_state, set_font, set_color, set_align, set_spacing, set_outline, set_blur,
   current_font, vert_metrics, text_width, text_bounds, draw_text, reset, blur_taps,
   ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT, ALIGN_TOP, ALIGN_MIDDLE, ALIGN_BOTTOM, ALIGN_BASELINE
)
use core
use core.str as str
use core.os (file_exists)
use core.os.ui.render as gfx
use core.math.scalar as fmath
use core.os.ui.font.truetype as ttf

def ALIGN_LEFT = 1
def ALIGN_CENTER = 2
def ALIGN_RIGHT = 4
def ALIGN_TOP = 8
def ALIGN_MIDDLE = 16
def ALIGN_BOTTOM = 32
def ALIGN_BASELINE = 64

;; One record per font: [name, renderer font id, parsed face, fallbacks].
mut any _fonts = nil
;; Current state: font, align, spacing, outline px, blur px, then the fill
;; and outline RGBA. Kept as floats so the stack is plain lists of numbers.
mut _st_font, _st_align = 0.0, 9.0
mut _st_spacing, _st_outline, _st_blur = 0.0, 0.0, 0.0
mut any _st_color = nil
mut any _st_outline_color = nil
mut any _stack = nil
;; Gaussian halo taps for the last blur radius: flat [dx, dy, weight, ...].
mut any _blur_taps = nil
mut _blur_taps_px = -1.0

fn _init() any {
   if !is_list(_fonts) { _fonts = [] }
   if !is_list(_stack) { _stack = [] }
   if !is_list(_st_color) { _st_color = [1.0, 1.0, 1.0, 1.0] }
   if !is_list(_st_outline_color) { _st_outline_color = [0.0, 0.0, 0.0, 1.0] }
}

fn reset() any {
   "Forgets every font and state. Renderer fonts already loaded stay loaded."
   _fonts, _stack = [], []
   clear_state()
}

fn font_count() int { "Returns how many fonts were added." _init() return _fonts.len }

fn add_font(str name, str path, int size) int {
   "Loads `path` at pixel size `size` under `name` and returns its handle, or -1 when the file cannot be loaded. Adding the same name again returns the existing handle."
   _init()
   def have = font_by_name(name)
   if have >= 0 { return have }
   if !file_exists(path) { return -1 }
   def int rid = gfx.font_load(path, size)
   if rid <= 0 { return -1 }
   ;; The parsed face answers coverage questions; it is not used to draw.
   def any face = ttf.load_path(path, 0)
   def any rec = [name, float(rid), face, []]
   _fonts = _fonts.append(rec)
   def int handle = _fonts.len - 1
   if _st_font < 0.5 && handle == 0 { _st_font = 0.0 }
   handle
}

fn font_by_name(str name) int {
   "Returns the handle of the font added as `name`, or -1."
   _init()
   mut i = 0
   def int n = _fonts.len
   while i < n {
      def any rec = _fonts.get(i, 0)
      if to_str(rec.get(0, "")) == name { return i }
      i += 1
   }
   -1
}

fn _rec(int font) any {
   _init()
   if font < 0 || font >= _fonts.len { return 0 }
   _fonts.get(font, 0)
}

fn _rid(int font) int {
   def any rec = _rec(font)
   if !is_list(rec) { return 0 }
   int(float(rec.get(1, 0.0)))
}

fn add_fallback(int base, int fallback) bool {
   "Makes `fallback` the next font tried for code points `base` does not cover. Returns false for unknown handles, a font falling back to itself, or a repeated entry."
   def any rec = _rec(base)
   if !is_list(rec) || !is_list(_rec(fallback)) || base == fallback { return false }
   def any chain = rec.get(3, 0)
   mut next = []
   mut i = 0
   def int n = is_list(chain) ? chain.len : 0
   while i < n {
      if int(float(chain.get(i, -1.0))) == fallback { return false }
      next = next.append(float(chain.get(i, -1.0)))
      i += 1
   }
   next = next.append(float(fallback))
   mut out = []
   i = 0
   def int total = _fonts.len
   while i < total {
      if i == base { out = out.append([to_str(rec.get(0, "")), float(rec.get(1, 0.0)), rec.get(2, 0), next]) }
      else { out = out.append(_fonts.get(i, 0)) }
      i += 1
   }
   _fonts = out
   true
}

fn has_glyph(int font, int cp) bool {
   "Returns true when the font file itself has a glyph for code point `cp`."
   def any rec = _rec(font)
   if !is_list(rec) { return false }
   def any face = rec.get(2, 0)
   if !face { return cp >= 32 && cp < 127 }
   ttf.get_glyph_index(face, cp) != 0
}

fn _resolve_depth(int font, int cp, int depth) int {
   if has_glyph(font, cp) { return font }
   if depth >= 8 { return -1 }
   def any rec = _rec(font)
   if !is_list(rec) { return -1 }
   def any chain = rec.get(3, 0)
   mut i = 0
   def int n = is_list(chain) ? chain.len : 0
   def int next_depth = depth + 1
   while i < n {
      def int hit = _resolve_depth(int(float(chain.get(i, -1.0))), cp, next_depth)
      if hit >= 0 { return hit }
      i += 1
   }
   -1
}

fn resolve(int font, int cp) int {
   "Returns the font that draws `cp` when asked through `font`: the font itself, or the first one in its fallback chain (searched depth first) that covers the code point. Falls back to `font` when none does."
   def int hit = _resolve_depth(font, cp, 0)
   hit >= 0 ? hit : font
}

fn _utf8_len(int lead) int {
   if lead < 128 { return 1 }
   if lead >= 240 { return 4 }
   if lead >= 224 { return 3 }
   if lead >= 192 { return 2 }
   1
}

fn _utf8_cp(str s, int i, int len) int {
   def int b0 = load8(s, i) & 255
   if len == 1 { return b0 }
   if len == 2 { return ((b0 & 31) << 6) | (load8(s, i + 1) & 63) }
   if len == 3 { return ((b0 & 15) << 12) | ((load8(s, i + 1) & 63) << 6) | (load8(s, i + 2) & 63) }
   ((b0 & 7) << 18) | ((load8(s, i + 1) & 63) << 12) | ((load8(s, i + 2) & 63) << 6) | (load8(s, i + 3) & 63)
}

fn runs(int font, any text) list {
   "Splits `text` into [font, substring, font, substring, ...]: maximal stretches drawn by one font once the fallback chain of `font` is applied."
   def s = to_str(text)
   def int n = s.len
   mut out = []
   ;; Bytes are walked to decode UTF-8; slices are taken by character index,
   ;; which is what str_slice counts in.
   mut int i = 0
   mut int ci = 0
   mut int run_start = 0
   mut int run_font = -1
   while i < n {
      mut int len = _utf8_len(load8(s, i) & 255)
      if i + len > n { len = n - i }
      def int cp = _utf8_cp(s, i, len)
      ;; White space stays with the run around it.
      mut int f = run_font
      if !(cp == 32 || cp == 9) || run_font < 0 { f = resolve(font, cp) }
      if run_font >= 0 && f != run_font {
         out = out.append(float(run_font))
         out = out.append(str.str_slice(s, run_start, ci))
         run_start = ci
      }
      run_font = f
      i += len
      ci += 1
   }
   if run_font >= 0 && run_start < ci {
      out = out.append(float(run_font))
      out = out.append(str.str_slice(s, run_start, ci))
   }
   out
}

fn prepare(int font, any text) int {
   "Rasterises the glyphs `text` needs into the atlas ahead of drawing, through the fallback chain. Returns how many runs were prepared."
   def any rs = runs(font, text)
   mut int i = 0
   mut int count = 0
   while i + 1 < rs.len {
      def int ri = i + 1
      def int rid = _rid(int(float(rs.get(i, 0.0))))
      if rid > 0 {
         gfx.font_prepare(rid, to_str(rs.get(ri, "")))
         count += 1
      }
      i += 2
   }
   count
}

;; ---- state ----------------------------------------------------------------

fn clear_state() any {
   "Resets the current state: first font, top-left alignment, white, no spacing, outline, or blur."
   _init()
   _st_font, _st_align = 0.0, float(ALIGN_LEFT | ALIGN_TOP)
   _st_spacing, _st_outline, _st_blur = 0.0, 0.0, 0.0
   _st_color = [1.0, 1.0, 1.0, 1.0]
   _st_outline_color = [0.0, 0.0, 0.0, 1.0]
}

fn push_state() any {
   "Saves the current state."
   _init()
   _stack = _stack.append([_st_font, _st_align, _st_spacing, _st_outline, _st_blur, _st_color, _st_outline_color])
}

fn pop_state() bool {
   "Restores the state saved by the matching push_state. Returns false when nothing was pushed."
   _init()
   def int n = _stack.len
   if n <= 0 { return false }
   def int last = n - 1
   def any s = _stack.get(last, 0)
   mut rest = []
   mut i = 0
   while i < last {
      rest = rest.append(_stack.get(i, 0))
      i += 1
   }
   _stack = rest
   _st_font, _st_align = float(s.get(0, 0.0)), float(s.get(1, 9.0))
   _st_spacing, _st_outline, _st_blur = float(s.get(2, 0.0)), float(s.get(3, 0.0)), float(s.get(4, 0.0))
   _st_color = s.get(5, 0)
   _st_outline_color = s.get(6, 0)
   true
}

fn set_font(int font) bool {
   "Selects the font for drawing and measuring. Returns false for an unknown handle."
   if !is_list(_rec(font)) { return false }
   _st_font = float(font)
   true
}

fn current_font() int { "Returns the selected font handle." int(_st_font) }

fn set_color(list rgba) any { "Sets the fill color." if rgba.len >= 3 { _st_color = rgba } }

fn set_align(int flags) any { "Sets alignment from the ALIGN_ flags: one horizontal and one vertical." _st_align = float(flags) }

fn set_spacing(f64 px) any { "Adds `px` pixels after every glyph (negative tightens)." _st_spacing = px }

fn set_outline(f64 px, any rgba=0) any {
   "Draws an outline `px` pixels wide behind the text, in `rgba` (black by default). Zero turns it off."
   _st_outline = px < 0.0 ? 0.0 : px
   if is_list(rgba) { _st_outline_color = rgba }
}

fn set_blur(f64 px) any { "Draws a Gaussian halo of radius `px` (sigma px/2) in the outline color behind the text. Zero turns it off." _st_blur = px < 0.0 ? 0.0 : px }

;; ---- metrics --------------------------------------------------------------

fn vert_metrics() list {
   "Returns [ascent, descent, line height] of the selected font in pixels; descent is negative."
   def int rid = _rid(int(_st_font))
   if rid <= 0 { return [0.0, 0.0, 0.0] }
   def f64 line = float(gfx.font_line_height(rid))
   mut f64 asc = float(gfx.font_ascent(rid))
   if asc <= 0.0 || asc > line { asc = line * 0.8 }
   [asc, asc - line, line]
}

fn _glyph_count(str s) int {
   mut int i = 0
   mut int count = 0
   def int n = s.len
   while i < n {
      i += _utf8_len(load8(s, i) & 255)
      count += 1
   }
   count
}

fn text_width(any text) f64 {
   "Returns the advance width of `text` in the current state, fallback fonts and letter spacing included."
   def s = to_str(text)
   def any rs = runs(int(_st_font), s)
   mut f64 w = 0.0
   mut int i = 0
   while i + 1 < rs.len {
      def int ri = i + 1
      def int rid = _rid(int(float(rs.get(i, 0.0))))
      if rid > 0 {
         def m = gfx.measure_text_fast(rid, to_str(rs.get(ri, "")))
         w = w + float(m.get(0, 0.0))
      }
      i += 2
   }
   def int glyphs = _glyph_count(s)
   if glyphs > 1 { w = w + _st_spacing * float(glyphs - 1) }
   w
}

fn _origin(f64 x, f64 y, f64 w) list {
   def int align = int(_st_align)
   def vm = vert_metrics()
   def f64 asc, line = float(vm.get(0, 0.0)), float(vm.get(2, 0.0))
   mut f64 ox, oy = x, y
   if (align & ALIGN_CENTER) != 0 { ox = x - w * 0.5 }
   elif (align & ALIGN_RIGHT) != 0 { ox = x - w }
   if (align & ALIGN_MIDDLE) != 0 { oy = y - line * 0.5 }
   elif (align & ALIGN_BOTTOM) != 0 { oy = y - line }
   elif (align & ALIGN_BASELINE) != 0 { oy = y - asc }
   [ox, oy, line]
}

fn text_bounds(f64 x, f64 y, any text) list {
   "Returns the rectangle [x, y, w, h] that draw_text(x, y, text) covers in the current state."
   def f64 w = text_width(text)
   def o = _origin(x, y, w)
   [float(o.get(0, 0.0)), float(o.get(1, 0.0)), w, float(o.get(2, 0.0))]
}

;; ---- drawing --------------------------------------------------------------

fn _draw_runs(any rs, f64 x, f64 y, any rgba, f64 alpha) f64 {
   "Draws resolved runs left to right from (x, y) and returns the pen x after the last one."
   def col = gfx.color_pack(float(rgba.get(0, 1.0)), float(rgba.get(1, 1.0)), float(rgba.get(2, 1.0)), float(rgba.get(3, 1.0)) * alpha)
   mut f64 pen = x
   mut int i = 0
   while i + 1 < rs.len {
      def int ri = i + 1
      def int rid = _rid(int(float(rs.get(i, 0.0))))
      def part = to_str(rs.get(ri, ""))
      if rid > 0 {
         if _st_spacing == 0.0 {
            gfx.draw_text(rid, part, pen, y, col)
            pen = pen + float(gfx.measure_text_fast(rid, part).get(0, 0.0))
         } else {
            ;; Letter spacing places each glyph on its own.
            mut int k = 0
            def int n = _glyph_count(part)
            while k < n {
               def int k1 = k + 1
               def ch = str.str_slice(part, k, k1)
               gfx.draw_text(rid, ch, pen, y, col)
               pen = pen + float(gfx.measure_text_fast(rid, ch).get(0, 0.0)) + _st_spacing
               k += 1
            }
         }
      }
      i += 2
   }
   pen
}

def f64 _BLUR_PEAK = 0.85
def int _BLUR_MAX_RING = 5

fn blur_taps(f64 px) list {
   "Returns the halo kernel for a blur of `px` pixels as flat [dx, dy, weight, ...]: a Gaussian with sigma px/2 sampled on a square grid, cut off at radius px, with weights that sum to 1. The grid step is one pixel up to px 5 and grows with larger radii."
   if is_list(_blur_taps) && _blur_taps_px == px { return _blur_taps }
   mut list out = []
   if px <= 0.0 {
      _blur_taps, _blur_taps_px = out, px
      return out
   }
   mut int ring = int(px + 0.999)
   if ring < 1 { ring = 1 }
   if ring > _BLUR_MAX_RING { ring = _BLUR_MAX_RING }
   def f64 step = px / float(ring)
   def f64 sigma = px * 0.5
   def f64 inv = 1.0 / (2.0 * sigma * sigma)
   def f64 cut = px * px + 0.0001
   mut f64 total = 0.0
   mut int j = 0 - ring
   while j <= ring {
      mut int i = 0 - ring
      while i <= ring {
         def f64 dx, dy = float(i) * step, float(j) * step
         def f64 d2 = dx * dx + dy * dy
         if d2 <= cut {
            def f64 w = fmath.exp(0.0 - d2 * inv)
            out = out.append(dx)
            out = out.append(dy)
            out = out.append(w)
            total = total + w
         }
         i += 1
      }
      j += 1
   }
   mut int k = 2
   while k < out.len {
      out[k] = float(out.get(k, 0.0)) / total
      k += 3
   }
   _blur_taps, _blur_taps_px = out, px
   out
}

fn draw_text(f64 x, f64 y, any text) f64 {
   "Draws `text` at (x, y) in the current state and returns the x where it ends."
   _init()
   def s = to_str(text)
   if s.len <= 0 || _rid(int(_st_font)) <= 0 { return x }
   def any rs = runs(int(_st_font), s)
   def f64 w = text_width(s)
   def o = _origin(x, y, w)
   def f64 ox, oy = float(o.get(0, 0.0)), float(o.get(1, 0.0))
   if _st_blur > 0.0 {
      ;; Each tap draws the text at one kernel offset. Source-over blending
      ;; multiplies what is left uncovered, so a tap of weight w gets alpha
      ;; 1 - (1 - peak)^w: fully covered pixels reach `peak` exactly and the
      ;; falloff follows the Gaussian-weighted coverage.
      def any taps = blur_taps(_st_blur)
      def f64 keep = fmath.log(1.0 - _BLUR_PEAK)
      mut int k = 0
      while k + 2 < taps.len {
         def int k1 = k + 1
         def int k2 = k + 2
         def f64 a = 1.0 - fmath.exp(keep * float(taps.get(k2, 0.0)))
         _draw_runs(rs, ox + float(taps.get(k, 0.0)), oy + float(taps.get(k1, 0.0)), _st_outline_color, a)
         k += 3
      }
   }
   if _st_outline > 0.0 {
      def f64 r = _st_outline
      mut int k = 0
      while k < 8 {
         def f64 dx = (k == 0 || k == 1 || k == 7) ? r : ((k == 3 || k == 4 || k == 5) ? (0.0 - r) : 0.0)
         def f64 dy = (k == 1 || k == 2 || k == 3) ? r : ((k == 5 || k == 6 || k == 7) ? (0.0 - r) : 0.0)
         _draw_runs(rs, ox + dx, oy + dy, _st_outline_color, 1.0)
         k += 1
      }
   }
   _draw_runs(rs, ox, oy, _st_color, 1.0)
   ox + w
}
