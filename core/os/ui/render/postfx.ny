;; Keywords: render ui post process screen effects vignette tint invert scanlines grain fade letterbox glow
;; Screen-space effects drawn over a finished 2D or 3D frame.
;;
;; Each effect is one or more full-screen passes built from vertex-colored
;; triangles and a blend state (set_blend_mode / set_blend_factors), so the
;; same code runs on Vulkan, GL (which has no programmable post pass here) and
;; the mock renderer. Call apply(w, h, t) after the scene and before the UI,
;; with a Y-down orthographic projection over the framebuffer:
;;
;;   gfx.set_ortho_2d(0.0, w, h, 0.0)
;;   postfx.apply(w, h, gfx.get_time())
;;
;; Effects, in the order they are drawn:
;;   tint       multiplies the frame by a color (color grading)
;;   glow       screen-blends a color: lifts the shadows toward it
;;   invert     replaces every color with its complement
;;   fade       mixes the frame toward a color by an amount
;;   grain      animated film grain
;;   scanlines  darkened rows every few pixels (CRT look)
;;   vignette   darkens toward the edges and corners
;;   letterbox  black bars to a wider aspect ratio
;;
;; Every effect is off until enabled and has one `amount` (set_amount); tint,
;; glow and fade also take a color (set_effect_color).
module core.os.ui.render.postfx(
   EFFECTS, effect_index, enable, toggle, enabled, enabled_mask, set_mask, disable_all,
   set_amount, amount, set_effect_color, effect_color, apply, last_passes
)

use core
use core.math (min, max, floor)
use core.os.ui.render as gfx
use core.os.ui.render.shared as shared

def EFFECTS = ["tint", "glow", "invert", "fade", "grain", "scanlines", "vignette", "letterbox"]
def int _N = 8

;; Per-effect amounts and colors live in raw f64 slots, the enable flags in an
;; int mask: no bools or floats stored in lists or dicts.
mut int _mask = 0
mut ptr _amounts = 0
mut ptr _colors = 0
mut ptr _vb = 0
mut int _vb_cap = 0
mut int _vb_n = 0
mut int _passes = 0

fn _init() bool {
   if _amounts { return true }
   _amounts = malloc(_N * 8)
   _colors = malloc(_N * 3 * 8)
   if !_amounts || !_colors { return false }
   ;; Defaults chosen to read clearly at amount 1 without hiding the frame.
   def list amts = [1.0, 0.35, 1.0, 0.45, 0.10, 0.28, 0.65, 2.35]
   def list cols = [[1.0, 0.86, 0.70], [0.20, 0.32, 0.55], [1.0, 1.0, 1.0], [0.02, 0.02, 0.03],
      [1.0, 1.0, 1.0], [0.0, 0.0, 0.0], [0.0, 0.0, 0.0], [0.0, 0.0, 0.0]]
   mut int i = 0
   while i < _N {
      store64_f64(_amounts, float(amts.get(i, 1.0)), i * 8)
      def list c = cols.get(i, [1.0, 1.0, 1.0])
      store64_f64(_colors, float(c.get(0, 1.0)), (i * 3) * 8)
      store64_f64(_colors, float(c.get(1, 1.0)), (i * 3 + 1) * 8)
      store64_f64(_colors, float(c.get(2, 1.0)), (i * 3 + 2) * 8)
      i += 1
   }
   true
}

fn effect_index(str name) int {
   "Returns the index of an effect name in EFFECTS, or -1."
   mut int i = 0
   while i < _N {
      if to_str(EFFECTS.get(i, "")) == name { return i }
      i += 1
   }
   -1
}

fn enable(str name, bool on=true) bool {
   "Turns an effect on or off. Returns false for an unknown name."
   def int i = effect_index(name)
   if i < 0 { return false }
   _init()
   if on { _mask = _mask | (1 << i) } else { _mask = _mask & ~(1 << i) }
   true
}

fn toggle(str name) bool {
   "Flips an effect and returns its new state(false for an unknown name)."
   def int i = effect_index(name)
   if i < 0 { return false }
   enable(name, !enabled(name))
   enabled(name)
}

fn enabled(str name) bool {
   "Returns whether an effect is on."
   def int i = effect_index(name)
   i >= 0 && ((_mask >> i) & 1) == 1
}

fn enabled_mask() int { "Returns the on/off state of all effects as a bit mask in EFFECTS order." _mask }

fn set_mask(int m) any {
   "Sets every effect at once from a bit mask in EFFECTS order."
   _init()
   _mask = m & ((1 << _N) - 1)
   0
}

fn disable_all() any { "Turns every effect off." _mask = 0 0 }

fn set_amount(str name, f64 v) bool {
   "Sets an effect's strength: tint and invert mix 0..1(1 = full effect), glow/fade/grain/scanlines/vignette opacity 0..1, letterbox the target aspect ratio(2.35 = cinemascope)."
   def int i = effect_index(name)
   if i < 0 || !_init() { return false }
   store64_f64(_amounts, v, i * 8)
   true
}

fn amount(str name) f64 {
   "Returns an effect's strength(0 for an unknown name)."
   def int i = effect_index(name)
   if i < 0 || !_init() { return 0.0 }
   def f64 v = load64_f64(_amounts, i * 8)
   v
}

fn set_effect_color(str name, f64 r, f64 g, f64 b) bool {
   "Sets the color of tint, glow or fade."
   def int i = effect_index(name)
   if i < 0 || !_init() { return false }
   store64_f64(_colors, r, (i * 3) * 8)
   store64_f64(_colors, g, (i * 3 + 1) * 8)
   store64_f64(_colors, b, (i * 3 + 2) * 8)
   true
}

fn effect_color(str name) list {
   "Returns an effect's color as [r, g, b]."
   def int i = effect_index(name)
   if i < 0 || !_init() { return [0.0, 0.0, 0.0] }
   def f64 r = load64_f64(_colors, (i * 3) * 8)
   def f64 g = load64_f64(_colors, (i * 3 + 1) * 8)
   def f64 b = load64_f64(_colors, (i * 3 + 2) * 8)
   [r, g, b]
}

fn last_passes() int { "Returns how many draw passes the last apply issued." _passes }

;; ---- vertex stream ------------------------------------------------------------
fn _reserve(int extra) bool {
   def int need = _vb_n + extra
   if need <= _vb_cap { return true }
   mut int cap = _vb_cap < 256 ? 256 : _vb_cap
   while cap < need { cap = cap * 2 }
   def ptr next = malloc(cap * shared.VERTEX_STRIDE)
   if !next { return false }
   if _vb {
      if _vb_n > 0 { __copy_mem(next, _vb, _vb_n * shared.VERTEX_STRIDE) }
      free(_vb)
   }
   mut int i = _vb_n
   while i < cap {
      shared.store_vertex64_typed(next, i, 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0)
      i += 1
   }
   _vb = next
   _vb_cap = cap
   true
}

fn _v(f64 x, f64 y, int col) any {
   def ptr at = _vb + _vb_n * shared.VERTEX_STRIDE
   store32_f32(at, x, shared.OFF_X)
   store32_f32(at, y, shared.OFF_Y)
   store32(at, col, shared.OFF_C)
   _vb_n += 1
}

fn _quad(f64 x0, f64 y0, f64 x1, f64 y1, int c00, int c10, int c11, int c01) any {
   ;; Corners: c00 top-left, c10 top-right, c11 bottom-right, c01 bottom-left.
   if !_reserve(6) { return 0 }
   _v(x0, y0, c00)
   _v(x1, y0, c10)
   _v(x1, y1, c11)
   _v(x0, y0, c00)
   _v(x1, y1, c11)
   _v(x0, y1, c01)
}

fn _flush() bool {
   if _vb_n <= 0 { return false }
   def bool ok = gfx.draw_vertices(_vb, _vb_n, -1)
   _vb_n = 0
   _passes += 1
   ok
}

fn _pack(f64 r, f64 g, f64 b, f64 a) int { gfx.color_pack(r, g, b, a) }

;; ---- effects -------------------------------------------------------------------
fn _amt(int i) f64 { load64_f64(_amounts, i * 8) }

fn _col(int i, int c) f64 { load64_f64(_colors, (i * 3 + c) * 8) }

fn _full(f64 w, f64 h, int col) any { _quad(0.0, 0.0, w, h, col, col, col, col) }

fn _tint(f64 w, f64 h) any {
   ;; dst * mix(1, color, amount): multiply blend, source factor = dst color.
   def f64 a = max(0.0, min(1.0, _amt(0)))
   def f64 r = 1.0 + (_col(0, 0) - 1.0) * a
   def f64 g = 1.0 + (_col(0, 1) - 1.0) * a
   def f64 b = 1.0 + (_col(0, 2) - 1.0) * a
   gfx.set_blend_factors(6, 0)
   _full(w, h, _pack(r, g, b, 1.0))
   _flush()
}

fn _glow(f64 w, f64 h) any {
   ;; Screen blend: src + dst * (1 - src).
   def f64 a = max(0.0, min(1.0, _amt(1)))
   gfx.set_blend_factors(1, 9)
   _full(w, h, _pack(_col(1, 0) * a, _col(1, 1) * a, _col(1, 2) * a, 0.0))
   _flush()
}

fn _invert(f64 w, f64 h) any {
   ;; out = mix(dst, 1 - dst, a). Below a half that is dst * (1 - 2a) + a;
   ;; above, the full complement d' = 1 - dst first, then d' * (2a - 1) + (1 - a).
   def f64 a = max(0.0, min(1.0, _amt(2)))
   if a <= 0.0 { return 0 }
   mut f64 scale = 1.0 - 2.0 * a
   mut f64 lift = a
   if a >= 0.5 {
      gfx.set_blend_factors(7, 0)
      _full(w, h, _pack(1.0, 1.0, 1.0, 1.0))
      _flush()
      scale = 2.0 * a - 1.0
      lift = 1.0 - a
   }
   if scale < 0.999 {
      gfx.set_blend_factors(6, 0)
      _full(w, h, _pack(scale, scale, scale, 1.0))
      _flush()
   }
   if lift > 0.001 {
      gfx.set_blend_factors(1, 1)
      _full(w, h, _pack(lift, lift, lift, 0.0))
      _flush()
   }
}

fn _fade(f64 w, f64 h) any {
   def f64 a = max(0.0, min(1.0, _amt(3)))
   gfx.set_blend_mode(0)
   _full(w, h, _pack(_col(3, 0), _col(3, 1), _col(3, 2), a))
   _flush()
}

fn _grain(f64 w, f64 h, f64 t) any {
   ;; Small light and dark specks, reseeded about 24 times a second.
   def f64 a = max(0.0, min(1.0, _amt(4)))
   if a <= 0.0 { return 0 }
   gfx.set_blend_mode(0)
   def f64 cell = 3.0
   def int cols = int(w / cell) + 1
   def int rows = int(h / cell) + 1
   def int n = min(cols * rows / 6, 24000)
   mut int seed = (int(floor(t * 24.0)) * 2654435761 + 12345) & 0x7fffffff
   def int light = _pack(1.0, 1.0, 1.0, a)
   def int dark = _pack(0.0, 0.0, 0.0, a * 1.4)
   mut int i = 0
   while i < n {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff
      def int cx = (seed >> 4) % cols
      seed = (seed * 1103515245 + 12345) & 0x7fffffff
      def int cy = (seed >> 4) % rows
      def f64 x = float(cx) * cell
      def f64 y = float(cy) * cell
      def int c = (seed & 1024) != 0 ? light : dark
      _quad(x, y, x + cell * 0.67, y + cell * 0.67, c, c, c, c)
      i += 1
   }
   _flush()
}

fn _scanlines(f64 w, f64 h) any {
   def f64 a = max(0.0, min(1.0, _amt(5)))
   gfx.set_blend_mode(0)
   def int c = _pack(0.0, 0.0, 0.0, a)
   mut f64 y = 0.0
   while y < h {
      _quad(0.0, y, w, y + 1.0, c, c, c, c)
      y = y + 3.0
   }
   _flush()
}

fn _vignette(f64 w, f64 h) any {
   ;; Four edge bands, opaque at the edge, clear at the inner rectangle; the
   ;; corners, covered by two bands, come out darker, as a lens's do.
   def f64 a = max(0.0, min(1.0, _amt(6)))
   gfx.set_blend_mode(0)
   def int edge = _pack(0.0, 0.0, 0.0, a)
   def int clear = _pack(0.0, 0.0, 0.0, 0.0)
   def f64 bx = w * 0.28
   def f64 by = h * 0.28
   _quad(0.0, 0.0, w, by, edge, edge, clear, clear)
   _quad(0.0, h - by, w, h, clear, clear, edge, edge)
   _quad(0.0, 0.0, bx, h, edge, clear, clear, edge)
   _quad(w - bx, 0.0, w, h, clear, edge, edge, clear)
   _flush()
}

fn _letterbox(f64 w, f64 h) any {
   def f64 ratio = max(1.0, _amt(7))
   def f64 inner = w / ratio
   if inner >= h { return 0 }
   def f64 bar = (h - inner) * 0.5
   gfx.set_blend_mode(0)
   def int c = _pack(0.0, 0.0, 0.0, 1.0)
   _quad(0.0, 0.0, w, bar, c, c, c, c)
   _quad(0.0, h - bar, w, h, c, c, c, c)
   _flush()
}

fn apply(f64 w, f64 h, f64 t=0.0) int {
   "Draws the enabled effects over the current frame(w by h, Y-down) and returns the number of passes drawn. Leaves the UI blend mode in effect."
   _passes = 0
   if _mask == 0 || w <= 0.0 || h <= 0.0 || !_init() { return 0 }
   if (_mask & 1) != 0 { _tint(w, h) }
   if (_mask & 2) != 0 { _glow(w, h) }
   if (_mask & 4) != 0 { _invert(w, h) }
   if (_mask & 8) != 0 { _fade(w, h) }
   if (_mask & 16) != 0 { _grain(w, h, t) }
   if (_mask & 32) != 0 { _scanlines(w, h) }
   if (_mask & 64) != 0 { _vignette(w, h) }
   if (_mask & 128) != 0 { _letterbox(w, h) }
   gfx.set_blend_mode(0)
   _passes
}
