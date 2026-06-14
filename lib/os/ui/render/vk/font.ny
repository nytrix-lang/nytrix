;; Keywords: render vulkan gpu font truetype os ui
;; Vulkan font atlas upload, glyph cache, and text draw submission.
;; References:
;; - std.os.ui.render.vk
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.vk.font(draw_text_batch, draw_text_runs, draw_text_runs_ptr, draw_text_runs_flat_ptr, draw_text_runs_flat_color_ptr, draw_terminal_line_ptr, __vkr_draw_text, __vkr_draw_text_glyph, _vkr_glyph_get_off, _vkr_glyph_present)
use std.core
use std.core.mem
use std.math.float as fmath
use std.os.ui.font.truetype as lib_ttf
use std.os.ui.render.vk.state
use std.os.ui.render.vk.utils
use std.os.ui.render.vk.renderer (_check_flush, set_mask, set_unlit, set_ui_material)
use std.os.ui.render.vk.texture (bind_texture, bindless_sync_texture_slot)
use std.os.ui.render.vk.draw (_ensure_default_triangle_pipeline)
use std.os.ui.render (_font_get)

def _TEXT_METRIC_LIMIT = 8192.0
def _TEXT_COORD_LIMIT = 1048576.0

@inline
@jit
fn _text_safe_metric(any v, f64 fallback=0.0) f64 {
   def fv = fmath.float(v)
   if fmath.is_nan(fv) || fmath.is_inf(fv) { return fallback }
   if fv > _TEXT_METRIC_LIMIT { return _TEXT_METRIC_LIMIT }
   if fv < 0.0 - _TEXT_METRIC_LIMIT { return 0.0 - _TEXT_METRIC_LIMIT }
   fv
}

@inline
@jit
fn _text_safe_coord(any v, f64 fallback=0.0) f64 {
   def fv = fmath.float(v)
   if fmath.is_nan(fv) || fmath.is_inf(fv) { return fallback }
   if fv > _TEXT_COORD_LIMIT { return _TEXT_COORD_LIMIT }
   if fv < 0.0 - _TEXT_COORD_LIMIT { return 0.0 - _TEXT_COORD_LIMIT }
   fv
}

@inline
@jit
fn _text_safe_uv(any v, f64 fallback=0.0) f64 {
   def fv = fmath.float(v)
   if fmath.is_nan(fv) || fmath.is_inf(fv) { return fallback }
   if fv < 0.0 { return 0.0 }
   if fv > 1.0 { return 1.0 }
   fv
}

@inline
@jit
fn _text_baseline_y(any y, any ascent) f64 {
   floor(_text_safe_coord(y, 0.0) + _text_safe_metric(ascent, 0.0) + 0.5)
}

fn _begin_text_batch(int base_tex_id=-1) any {
   ;; Text batches must stay in per-vertex texture mode.  btop/htop style TUIs
   ;; mix box-drawing glyphs, ASCII and fallback pages; using one concrete
   ;; base texture per page forced a material flush on almost every page switch.
   ;;
   ;; vc_mode=12 => vertex texture index + vertex color multiply.
   ;; alpha=2 keeps atlas coverage/mask behavior.
   set_ui_material(-1, 2, 12)
   set_mask(0)
   set_unlit(true)
   _ensure_default_triangle_pipeline()
   0
}

fn _set_text_page(int tex_id) any {
   "Prepares an atlas page for vertex-indexed text without changing material.
   The glyph page is carried per vertex through vTexIndex, so changing pages
   must not flush the whole dynamic batch.  This keeps fullscreen terminal apps
   from producing hundreds of tiny Vulkan draws when they mix ASCII and Unicode
   box-drawing glyph pages."
   if tex_id < 0 { return 0 }
   if _bindless_ds { bindless_sync_texture_slot(tex_id) }
   else { bind_texture(tex_id) }
   _current_texture_id = tex_id
   _current_tex_index = tex_id
   if _vertex_offset == _last_flush_offset {
      _batch_texture_id = tex_id
      _batch_tex_index = tex_id
   }
   0
}

mut _glyph_page_frame = -1
mut _glyph_page_tex = -999999

fn _ensure_glyph_text_page(int tex_id) any {
   if tex_id < 0 { return 0 }
   ;; Keep one stable text material for the whole terminal foreground pass.
   ;; Only the per-vertex texture id changes between glyph pages.
   if _current_base_tex_id != -1 || _current_vc_mode != 12 || _current_alpha_u32 != 2 {
      _begin_text_batch(-1)
   } else {
      set_unlit(true)
      set_mask(0)
      _ensure_default_triangle_pipeline()
   }
   if _glyph_page_frame == _current_frame && _glyph_page_tex == tex_id &&
   _current_texture_id == tex_id && _current_tex_index == tex_id{
      return 0
   }
   _set_text_page(tex_id)
   _glyph_page_frame = _current_frame
   _glyph_page_tex = tex_id
   0
}

@inline
@jit
fn _store_text_glyph_vertex_full(any v, f64 x, f64 y, f64 u, f64 uv, int color_u32, int tex_id) any {
   store32_f32(v, x, _VKR_OFF_X)
   store32_f32(v, y, _VKR_OFF_Y)
   store32_f32(v, 0.0, _VKR_OFF_Z)
   store32_f32(v, u, _VKR_OFF_U)
   store32_f32(v, uv, _VKR_OFF_V)
   store32(v, color_u32, _VKR_OFF_C)
   store32_f32(v, 0.0, _VKR_OFF_NX)
   store32_f32(v, 0.0, _VKR_OFF_NY)
   store32_f32(v, 1.0, _VKR_OFF_NZ)
   store32_f32(v, 1.0, _VKR_OFF_TX)
   store32_f32(v, 0.0, _VKR_OFF_TY)
   store32_f32(v, 0.0, _VKR_OFF_TZ)
   store32_f32(v, 1.0, _VKR_OFF_TW)
   store32_f32(v, 0.0, _VKR_OFF_U2)
   store32_f32(v, 0.0, _VKR_OFF_V2)
   store32(v, tex_id, _VKR_OFF_TEX)
}

@inline
@jit
fn _push_text_glyph_rect_unchecked(f64 x,
   f64 y,
   f64 w,
   f64 h,
   f64 u1,
   f64 v1,
   f64 u2,
   f64 v2,
   int c) bool {
   def p = _local_vertex_map + _vertex_offset
   def x2 = x + w
   def y2 = y + h
   def tex_id = _current_tex_index
   if _quad_template {
      __copy_mem(p, _quad_template, _VKR_VERT_STRIDE * 6)
      mut bv = p
      store32_f32(bv, x, _VKR_OFF_X)
      store32_f32(bv, y, _VKR_OFF_Y)
      store32_f32(bv, u1, _VKR_OFF_U)
      store32_f32(bv, v1, _VKR_OFF_V)
      store32(bv, c, _VKR_OFF_C)
      store32(bv, tex_id, _VKR_OFF_TEX)
      bv += _VKR_VERT_STRIDE
      store32_f32(bv, x, _VKR_OFF_X)
      store32_f32(bv, y2, _VKR_OFF_Y)
      store32_f32(bv, u1, _VKR_OFF_U)
      store32_f32(bv, v2, _VKR_OFF_V)
      store32(bv, c, _VKR_OFF_C)
      store32(bv, tex_id, _VKR_OFF_TEX)
      bv += _VKR_VERT_STRIDE
      store32_f32(bv, x2, _VKR_OFF_X)
      store32_f32(bv, y2, _VKR_OFF_Y)
      store32_f32(bv, u2, _VKR_OFF_U)
      store32_f32(bv, v2, _VKR_OFF_V)
      store32(bv, c, _VKR_OFF_C)
      store32(bv, tex_id, _VKR_OFF_TEX)
      bv += _VKR_VERT_STRIDE
      store32_f32(bv, x2, _VKR_OFF_X)
      store32_f32(bv, y2, _VKR_OFF_Y)
      store32_f32(bv, u2, _VKR_OFF_U)
      store32_f32(bv, v2, _VKR_OFF_V)
      store32(bv, c, _VKR_OFF_C)
      store32(bv, tex_id, _VKR_OFF_TEX)
      bv += _VKR_VERT_STRIDE
      store32_f32(bv, x2, _VKR_OFF_X)
      store32_f32(bv, y, _VKR_OFF_Y)
      store32_f32(bv, u2, _VKR_OFF_U)
      store32_f32(bv, v1, _VKR_OFF_V)
      store32(bv, c, _VKR_OFF_C)
      store32(bv, tex_id, _VKR_OFF_TEX)
      bv += _VKR_VERT_STRIDE
      store32_f32(bv, x, _VKR_OFF_X)
      store32_f32(bv, y, _VKR_OFF_Y)
      store32_f32(bv, u1, _VKR_OFF_U)
      store32_f32(bv, v1, _VKR_OFF_V)
      store32(bv, c, _VKR_OFF_C)
      store32(bv, tex_id, _VKR_OFF_TEX)
   } else {
      _store_text_glyph_vertex_full(p + 0 * _VKR_VERT_STRIDE, x, y, u1, v1, c, tex_id)
      _store_text_glyph_vertex_full(p + 1 * _VKR_VERT_STRIDE, x, y2, u1, v2, c, tex_id)
      _store_text_glyph_vertex_full(p + 2 * _VKR_VERT_STRIDE, x2, y2, u2, v2, c, tex_id)
      _store_text_glyph_vertex_full(p + 3 * _VKR_VERT_STRIDE, x2, y2, u2, v2, c, tex_id)
      _store_text_glyph_vertex_full(p + 4 * _VKR_VERT_STRIDE, x2, y, u2, v1, c, tex_id)
      _store_text_glyph_vertex_full(p + 5 * _VKR_VERT_STRIDE, x, y, u1, v1, c, tex_id)
   }
   _vertex_offset += _VKR_VERT_STRIDE * 6
   _prim_rect_quads += 1
   true
}

@inline
fn _vkr_draw_text_fast_inner(str text, f64 x, f64 y, int color_u32, any glyphs_ptr, f64 line_h_f, any font_info=0) any {
   def n = text.len
   if n <= 0 { return 0 }
   _prim_text_calls += 1
   if !_check_flush(n * (_VKR_VERT_STRIDE * 6)) { return 0 }
   def page0 = load64(glyphs_ptr, 0)
   def pen_x0 = x
   mut pen_x, pen_y = x, y
   mut v_off = _vertex_offset
   mut page_tid_cur = -2
   mut prev_gi = -1
   mut glyph_count = 0
   mut i = 0
   while i < n {
      def b0 = load8(text, i) & 255
      mut cp, g_off = 0, 0
      if b0 < 128 {
         cp = b0
         i += 1
         if page0 { g_off = ptr_add(page0, cp * 48) }
      }
      else {
         if (b0 & 224) == 192 && i + 1 < n { cp = ((b0 & 31) << 6) | (load8(text, i + 1) & 63) i += 2 }
         elif (b0 & 240) == 224 && i + 2 < n { cp = ((b0 & 15) << 12) | ((load8(text, i + 1) & 63) << 6) | (load8(text, i + 2) & 63) i += 3 }
         elif (b0 & 248) == 240 && i + 3 < n { cp = ((b0 & 7) << 18) | ((load8(text, i + 1) & 63) << 12) | ((load8(text, i + 2) & 63) << 6) | (load8(text, i + 3) & 63) i += 4 }
         else { cp = 63 i += 1 }
         def page = load64(glyphs_ptr, ((cp >> 8) & 65535) * 8)
         if page { g_off = ptr_add(page, (cp & 255) * 48) }
      }
      if cp < 32 {
         if cp == 13 { prev_gi = -1 continue }
         if cp == 10 { pen_x = pen_x0 pen_y += line_h_f prev_gi = -1 continue }
         if cp == 9 { pen_x += line_h_f * 2.0 prev_gi = -1 }
         continue
      }
      mut gi = 0
      if font_info {
         gi = lib_ttf.get_glyph_index(font_info, cp)
         if gi == 0 && cp != 63 { gi = lib_ttf.get_glyph_index(font_info, 63) }
         if prev_gi >= 0 && gi > 0 { pen_x += float(lib_ttf.get_kern(font_info, prev_gi, gi, int(line_h_f))) }
      }
      if !g_off || load32(g_off, 40) == 0 {
         if page0 {
            g_off = ptr_add(page0, 63 * 48)
            if load32_f32(g_off, 12) <= 0.0 { g_off = ptr_add(page0, 35 * 48) }
            if load32_f32(g_off, 12) <= 0.0 { g_off = ptr_add(page0, 32 * 48) }
         }
      }
      if !g_off { pen_x += line_h_f * 0.5 prev_gi = gi continue }
      def bw_check = load32_f32(g_off, 12)
      if bw_check <= 0.0 { pen_x += load32_f32(g_off, 0) prev_gi = gi continue }
      def tid = load32(g_off, 36)
      if tid < 0 { pen_x += load32_f32(g_off, 0) prev_gi = gi continue }
      def is_color = load32(g_off, 44) != 0
      if tid != page_tid_cur {
         _vertex_offset = v_off
         _ensure_glyph_text_page(tid)
         v_off = _vertex_offset
         page_tid_cur = tid
      }
      def bw = load32_f32(g_off, 12)
      if bw > 0.0 {
         def gx = floor(pen_x + load32_f32(g_off, 4) + 0.5)
         def bh = load32_f32(g_off, 16)
         def gy = floor(pen_y - load32_f32(g_off, 8) + 0.5)
         def gx2 = gx + bw
         def gy2 = gy + bh
         def u1 = load32_f32(g_off, 20)
         def v1 = load32_f32(g_off, 24)
         def u2 = load32_f32(g_off, 28)
         def v2 = load32_f32(g_off, 32)
         def c = is_color ? 0xFFFFFFFF : color_u32
         _vertex_offset = v_off
         _push_text_glyph_rect_unchecked(gx, gy, gx2 - gx, gy2 - gy, u1, v1, u2, v2, c)
         v_off = _vertex_offset
         glyph_count += 1
      }
      pen_x += load32_f32(g_off, 0)
      prev_gi = gi
   }
   _vertex_offset = v_off
   _prim_text_glyphs += glyph_count
   0
}

@inline
@jit
fn _vkr_ascii_run_tid(str text, int n, any page0) int {
   if !page0 || n <= 0 { return -1 }
   mut i = 0
   mut tid = -1
   while i < n {
      def cp = load8(text, i) & 255
      if cp < 32 || cp >= 128 { return -1 }
      def g_off = ptr_add(page0, cp * 48)
      if load32(g_off, 40) == 0 { return -1 }
      def gt = load32(g_off, 36)
      if gt < 0 { return -1 }
      if tid < 0 { tid = gt }
      elif gt != tid { return -1 }
      i += 1
   }
   tid
}

@jit
fn _vkr_draw_text_ascii_run(str text, int n, f64 pen_x, f64 pen_y, int color_u32, any page0, int tex_id) int {
   if tex_id < 0 || !_check_flush(n * (_VKR_VERT_STRIDE * 6)) { return -1 }
   if _current_tex_index != tex_id { _ensure_glyph_text_page(tex_id) }
   mut glyph_count = 0
   mut i = 0
   while i < n {
      def cp = load8(text, i) & 255
      def g_off = ptr_add(page0, cp * 48)
      def bw = load32_f32(g_off, 12)
      if bw > 0.0 {
         def gx = floor(pen_x + load32_f32(g_off, 4) + 0.5)
         def bh = load32_f32(g_off, 16)
         def gy = floor(pen_y - load32_f32(g_off, 8) + 0.5)
         def c = load32(g_off, 44) != 0 ? 0xFFFFFFFF : color_u32
         _push_text_glyph_rect_unchecked(
            gx,
            gy,
            bw,
            bh,
            load32_f32(g_off, 20),
            load32_f32(g_off, 24),
            load32_f32(g_off, 28),
            load32_f32(g_off, 32),
            c
         )
         glyph_count += 1
      }
      pen_x += load32_f32(g_off, 0)
      i += 1
   }
   glyph_count
}

@jit
fn _vkr_draw_text_runs_flat_inner(list runs, int color_u32, any glyphs_ptr, any ascent, any line_h, int stride=3, int color_slot=-1) any {
   def n = runs.len
   if n < stride { return 0 }
   def asc = float(ascent)
   def line_h_f = float(line_h)
   def page0 = load64(glyphs_ptr, 0)
   mut page_tid_cur = -2
   mut glyph_total = 0
   mut ri = 0
   def need = stride - 1
   while ri + need < n {
      mut text = runs.get(ri, "")
      if !is_str(text) {
         ri += stride
         continue
      }
      def tn = text.len
      if tn > 0 {
         def run_color = (color_slot >= 0) ? int(runs.get(ri + color_slot, color_u32)) : int(color_u32)
         def run_x = float(runs.get(ri + 1, 0.0))
         def run_y = _text_baseline_y(runs.get(ri + 2, 0.0), asc)
         def ascii_tid = _vkr_ascii_run_tid(text, tn, page0)
         if ascii_tid >= 0 {
            _prim_text_calls += 1
            def added = _vkr_draw_text_ascii_run(
               text,
               tn,
               run_x,
               run_y,
               run_color,
               page0,
               ascii_tid
            )
            if added >= 0 {
               glyph_total += added
               ri += stride
               continue
            }
         }
         _prim_text_calls += 1
         if !_check_flush(tn * (_VKR_VERT_STRIDE * 6)) { return 0 }
         def pen_x0 = run_x
         mut pen_x, pen_y = pen_x0, run_y
         mut v_off = _vertex_offset
         mut j = 0
         while j < tn {
            def b0 = load8(text, j) & 255
            mut cp, g_off = 0, 0
            if b0 < 128 {
               cp = b0
               j += 1
               if page0 { g_off = ptr_add(page0, cp * 48) }
            } else {
               if (b0 & 224) == 192 && j + 1 < tn { cp = ((b0 & 31) << 6) | (load8(text, j + 1) & 63) j += 2 }
               elif (b0 & 240) == 224 && j + 2 < tn { cp = ((b0 & 15) << 12) | ((load8(text, j + 1) & 63) << 6) | (load8(text, j + 2) & 63) j += 3 }
               elif (b0 & 248) == 240 && j + 3 < tn { cp = ((b0 & 7) << 18) | ((load8(text, j + 1) & 63) << 12) | ((load8(text, j + 2) & 63) << 6) | (load8(text, j + 3) & 63) j += 4 }
               else { cp = 63 j += 1 }
               def page = load64(glyphs_ptr, ((cp >> 8) & 65535) * 8)
               if page { g_off = ptr_add(page, (cp & 255) * 48) }
            }
            if cp < 32 {
               if cp == 13 { continue }
               if cp == 10 { pen_x = pen_x0 pen_y += line_h_f continue }
               if cp == 9 { pen_x += line_h_f * 2.0 }
               continue
            }
            if !g_off || load32(g_off, 40) == 0 {
               if page0 {
                  g_off = ptr_add(page0, 63 * 48)
                  if load32_f32(g_off, 12) <= 0.0 { g_off = ptr_add(page0, 35 * 48) }
                  if load32_f32(g_off, 12) <= 0.0 { g_off = ptr_add(page0, 32 * 48) }
               }
            }
            if !g_off { pen_x += line_h_f * 0.5 continue }
            def bw_check = load32_f32(g_off, 12)
            if bw_check <= 0.0 { pen_x += load32_f32(g_off, 0) continue }
            def tid = load32(g_off, 36)
            if tid < 0 { pen_x += load32_f32(g_off, 0) continue }
            def is_color = load32(g_off, 44) != 0
            if tid != page_tid_cur {
               _vertex_offset = v_off
               _ensure_glyph_text_page(tid)
               v_off = _vertex_offset
               page_tid_cur = tid
            }
            def gx = floor(pen_x + load32_f32(g_off, 4) + 0.5)
            def bh = load32_f32(g_off, 16)
            def gy = floor(pen_y - load32_f32(g_off, 8) + 0.5)
            def u1 = load32_f32(g_off, 20)
            def v1 = load32_f32(g_off, 24)
            def u2 = load32_f32(g_off, 28)
            def v2 = load32_f32(g_off, 32)
            def c = is_color ? 0xFFFFFFFF : run_color
            _vertex_offset = v_off
            _push_text_glyph_rect_unchecked(gx, gy, bw_check, bh, u1, v1, u2, v2, c)
            v_off = _vertex_offset
            glyph_total += 1
            pen_x += load32_f32(g_off, 0)
         }
         _vertex_offset = v_off
      }
      ri += stride
   }
   _prim_text_glyphs += glyph_total
   0
}

@jit
fn draw_text_batch(int font_id, list lines, any x, any y, any spacing, int color_u32) any {
   "Draws multiple lines of text in a single Nytrix call to minimize interpreter overhead."
   if !_frame_open { return 0 }
   def f = _font_get(font_id)
   if !f { return 0 }
   def gptr = f.get("fast_glyphs", 0)
   if !gptr { return 0 }
   def ascent = float(f.get("ascent", 0.0))
   def line_h = float(f.get("line_height", f.get("size", 16.0)))
   _begin_text_batch()
   mut i = 0
   def n_lines = lines.len
   while i < n_lines {
      _vkr_draw_text_fast_inner(to_str(lines.get(i)), float(x), _text_baseline_y(float(y) + float(i) * float(spacing), ascent), color_u32, gptr, line_h, 0)
      i += 1
   }
   0
}

@jit
fn draw_text_runs(int font_id, list runs, int color_u32) any {
   "Draws arbitrary same-font/same-color text runs in one backend text batch."
   if !_frame_open || !is_list(runs) { return 0 }
   def n_runs = runs.len
   if n_runs <= 0 { return 0 }
   def f = _font_get(font_id)
   if !f { return 0 }
   def gptr = f.get("fast_glyphs", 0)
   if !gptr { return 0 }
   def ascent = float(f.get("ascent", 0.0))
   def line_h = float(f.get("line_height", f.get("size", 16.0)))
   _begin_text_batch()
   mut i = 0
   while i < n_runs {
      def run = runs.get(i, 0)
      if is_list(run) && run.len >= 3 { _vkr_draw_text_fast_inner(to_str(run.get(0, "")), float(run.get(1, 0.0)), _text_baseline_y(run.get(2, 0.0), ascent), color_u32, gptr, line_h, 0) }
      i += 1
   }
   0
}

@jit
fn draw_text_runs_ptr(int font_id, list runs, int color_u32, any glyphs_ptr, any ascent, any line_h) any {
   "Draws arbitrary same-font/same-color text runs using a pre-resolved glyph table."
   if !_frame_open || !is_list(runs) || !glyphs_ptr { return 0 }
   def n_runs = runs.len
   if n_runs <= 0 { return 0 }
   def asc = float(ascent)
   def line_h_f = float(line_h)
   _begin_text_batch()
   mut i = 0
   while i < n_runs {
      def run = runs.get(i, 0)
      if is_list(run) && run.len >= 3 {
         _vkr_draw_text_fast_inner(
            to_str(run.get(0, "")),
            float(run.get(1, 0.0)),
            _text_baseline_y(run.get(2, 0.0), asc),
            color_u32,
            glyphs_ptr,
            line_h_f,
            0
         )
      }
      i += 1
   }
   0
}

@jit
fn draw_text_runs_flat_ptr(int font_id, list runs, int color_u32, any glyphs_ptr, any ascent, any line_h) any {
   "Draws flat [text,x,y,...] runs using a pre-resolved glyph table."
   if !_frame_open || !is_list(runs) || !glyphs_ptr { return 0 }
   def n = runs.len
   if n < 3 { return 0 }
   _begin_text_batch()
   _vkr_draw_text_runs_flat_inner(runs, color_u32, glyphs_ptr, ascent, line_h)
   0
}

@jit
fn draw_text_runs_flat_color_ptr(int font_id, list runs, any glyphs_ptr, any ascent, any line_h) any {
   "Draws flat [text,x,y,color,...] runs using a pre-resolved glyph table."
   if !_frame_open || !is_list(runs) || !glyphs_ptr { return 0 }
   def n = runs.len
   if n < 4 { return 0 }
   _begin_text_batch()
   _vkr_draw_text_runs_flat_inner(runs, 0xffffffff, glyphs_ptr, ascent, line_h, 4, 3)
   0
}

@readonly
@jit
fn _vkr_glyph_get_off(any glyphs_ptr, int cp) any {
   if cp < 0 || cp >= 1114112 { return 0 }
   def page_idx = cp >> 8
   def page_ptr = load64(glyphs_ptr, page_idx * 8)
   if !page_ptr { return 0 }
   ptr_add(page_ptr, (cp & 255) * 48)
}

@readonly
@jit
fn _vkr_glyph_present(any glyphs_ptr, int cp) bool {
   def off = _vkr_glyph_get_off(glyphs_ptr, cp)
   if !off || load32(off, 40) == 0 { return false }
   int(load32(off, 36)) >= 0
}

@jit
fn __vkr_draw_text(int font_id, any text, any x, any y, any color, any glyphs_ptr, any ascent, any line_h, any out_info, bool begin_batch=true) any {
   if begin_batch { _begin_text_batch() }
   if !glyphs_ptr || !is_str(text) { return 0 }
   _prim_text_calls += 1
   def n = text.len
   def line_h_f = float(line_h)
   def c_text = _vkr_color_u32(color)
   def base_y = _text_baseline_y(y, ascent)
   if !out_info {
      _vkr_draw_text_fast_inner(text, float(x), base_y, c_text, glyphs_ptr, line_h_f, 0)
      return 0
   }
   if !_check_flush(n * (_VKR_VERT_STRIDE * 6)) { return 0 }
   def pen_x0 = float(x)
   mut pen_x, pen_y = pen_x0, base_y
   mut total_verts = 0
   def page0 = load64(glyphs_ptr, 0)
   def font_obj = _font_get(font_id)
   def font_info = font_obj ? font_obj.get("info", 0) : 0
   def f_size = font_obj ? int(font_obj.get("size", 16.0)) : 16
   mut page_tid_cur = -2
   mut prev_gi = -1
   mut i = 0
   while i < n {
      def b0 = load8(text, i) & 255
      mut cp, step = 0, 1
      mut g_off = 0
      if b0 < 128 {
         cp = b0 i += 1
         if page0 {
            g_off = ptr_add(page0, cp * 48)
            if load32(g_off, 40) == 0 { g_off = 0 }
         }
      } else {
         if (b0 & 224) == 192 && i + 1 < n {
            cp = ((b0 & 31) << 6) | (load8(text, i + 1) & 255 & 63)
            step = 2
         } elif (b0 & 240) == 224 && i + 2 < n {
            cp = ((b0 & 15) << 12) | ((load8(text, i + 1) & 255 & 63) << 6) | (load8(text, i + 2) & 255 & 63)
            step = 3
         } elif (b0 & 248) == 240 && i + 3 < n {
            cp = ((b0 & 7) << 18) | ((load8(text, i + 1) & 255 & 63) << 12) | ((load8(text, i + 2) & 255 & 63) << 6) | (load8(text, i + 3) & 255 & 63)
            step = 4
         } else {
            cp = 63
            step = 1
         }
         i += step
         def page = load64(glyphs_ptr, ((cp >> 8) & 65535) * 8)
         if page {
            g_off = ptr_add(page, (cp & 255) * 48)
            if load32(g_off, 40) == 0 { g_off = 0 }
         }
      }
      if cp == 13 { prev_gi = -1 continue }
      if cp == 10 { pen_x = pen_x0 pen_y = pen_y + line_h_f prev_gi = -1 continue }
      if cp == 9 { pen_x = pen_x + line_h_f * 2.0 prev_gi = -1 continue }
      mut gi = 0
      if font_info {
         gi = lib_ttf.get_glyph_index(font_info, cp)
         if gi == 0 && cp != 63 { gi = lib_ttf.get_glyph_index(font_info, 63) }
         if prev_gi >= 0 && gi > 0 { pen_x += float(lib_ttf.get_kern(font_info, prev_gi, gi, f_size)) }
      }
      if !g_off { if page0 { g_off = ptr_add(page0, 63 * 48) } else { continue } }
      def tex_id = load32(g_off, 36)
      if tex_id < 0 { pen_x = pen_x + load32_f32(g_off, 0) prev_gi = gi continue }
      def is_color = load32(g_off, 44) != 0
      if tex_id != page_tid_cur {
         _ensure_glyph_text_page(tex_id)
         page_tid_cur = tex_id
      }
      if font_info == 0 && !out_info {
         def adv_fast = load32_f32(g_off, 0)
         def bw_fast = load32_f32(g_off, 12)
         def bh_fast = load32_f32(g_off, 16)
         if bw_fast > 0.0 && bh_fast > 0.0 {
            def gx = floor(pen_x + load32_f32(g_off, 4) + 0.5)
            def gy = floor(pen_y - load32_f32(g_off, 8) + 0.5)
            def c = is_color ? 0xFFFFFFFF : c_text
            _push_text_glyph_rect_unchecked(
               gx,
               gy,
               bw_fast,
               bh_fast,
               load32_f32(g_off, 20),
               load32_f32(g_off, 24),
               load32_f32(g_off, 28),
               load32_f32(g_off, 32),
               c
            )
            total_verts += 6
         }
         pen_x = pen_x + adv_fast
         prev_gi = gi
         continue
      }
      def adv_raw = _text_safe_metric(load32_f32(g_off, 0))
      def bh_raw = _text_safe_metric(load32_f32(g_off, 16))
      mut gs = 1.0
      def bw, bh = _text_safe_metric(load32_f32(g_off, 12) * gs), bh_raw * gs
      def adv = adv_raw * gs
      if bw > 0.0 && bh > 0.0 {
         def gx = floor(_text_safe_coord(pen_x + _text_safe_metric(load32_f32(g_off, 4) * gs), pen_x) + 0.5)
         mut gy = floor(_text_safe_coord(pen_y - _text_safe_metric(load32_f32(g_off, 8) * gs), pen_y) + 0.5)
         def u1, v1 = _text_safe_uv(load32_f32(g_off, 20)), _text_safe_uv(load32_f32(g_off, 24))
         def u2, v2 = _text_safe_uv(load32_f32(g_off, 28)), _text_safe_uv(load32_f32(g_off, 32))
         def c = is_color ? 0xFFFFFFFF : c_text
         def gx2, gy2 = _text_safe_coord(gx + bw, gx), _text_safe_coord(gy + bh, gy)
         _push_text_glyph_rect_unchecked(gx, gy, gx2 - gx, gy2 - gy, u1, v1, u2, v2, c)
         total_verts += 6
      }
      pen_x = pen_x + adv
      prev_gi = gi
   }
   if out_info {
      store64_h(out_info, total_verts, 0)
      store64_h(out_info, _current_texture_id, 8)
   }
   _prim_text_glyphs += total_verts / 6
   0
}

fn __vkr_draw_text_glyph(any g_ptr, any v, any x, any y, int cp, any color, any tid) any {
   if !g_ptr || !_frame_open { return 0 }
   if !_check_flush(_VKR_VERT_STRIDE * 6) { return 0 }
   mut g_off = _vkr_glyph_get_off(g_ptr, cp)
   if !g_off || load32(g_off, 40) == 0 {
      def page0 = load64(g_ptr, 0)
      if page0 { g_off = ptr_add(page0, (63 & 255) * 48) } else { return 0 }
   }
   def tex_id = load32(g_off, 36)
   if tex_id < 0 { return 0 }
   _ensure_glyph_text_page(tex_id)
   def bw, bh = _text_safe_metric(load32_f32(g_off, 12)), _text_safe_metric(load32_f32(g_off, 16))
   if bw <= 0.0 || bh <= 0.0 { return 0 }
   def gx = floor(_text_safe_coord(x + _text_safe_metric(load32_f32(g_off, 4)), x) + 0.5)
   def gy = floor(_text_safe_coord(y - _text_safe_metric(load32_f32(g_off, 8)), y) + 0.5)
   def gx2 = _text_safe_coord(gx + bw, gx)
   def gy2 = _text_safe_coord(gy + bh, gy)
   def u1 = _text_safe_uv(load32_f32(g_off, 20)) def v1 = _text_safe_uv(load32_f32(g_off, 24))
   def u2 = _text_safe_uv(load32_f32(g_off, 28)) def v2 = _text_safe_uv(load32_f32(g_off, 32))
   _push_text_glyph_rect_unchecked(gx, gy, gx2 - gx, gy2 - gy, u1, v1, u2, v2, _vkr_color_u32(color))
   _prim_text_glyphs += 1
   0
}

@jit
fn draw_terminal_line_ptr(any line_ptr, int co, f64 px, f64 baseline_y, f64 cw, any glyphs_ptr, int skip_mask, int reverse_mask) any {
   "Draws draw terminal line ptr."
   if !line_ptr || !glyphs_ptr || !_frame_open || co <= 0 { return 0 }
   if !_check_flush(co * _VKR_VERT_STRIDE * 6) { return 0 }
   def page0 = load64(glyphs_ptr, 0)
   def missing_glyph = page0 ? ptr_add(page0, 63 * 48) : 0
   _begin_text_batch(-1)
   mut c = 0
   mut pen_x = px
   mut glyph_count = 0
   ;; Force first visible glyph in each line to validate the text material.
   ;; The caller may have only bound the texture object, while the shader still
   ;; needs baseTexIndex/vc_mode set for atlas sampling.
   mut page_tid_cur = -999999
   while c < co {
      def off = c * 16
      mut cp = load32(line_ptr, off)
      if cp > 32 {
         def md = load32(line_ptr, off + 12)
         if (md & skip_mask) == 0 {
            mut g_off = (cp < 256 && page0) ? ptr_add(page0, (cp & 255) * 48) : _vkr_glyph_get_off(glyphs_ptr, cp)
            ;; Do not draw a '?' box for missing terminal glyphs here.  The
            ;; vterm foreground pass falls back through draw_text(), which can
            ;; prime/upload the real glyph and avoids first-frame white blocks.
            if g_off && load32(g_off, 40) != 0 {
               def tex_id = load32(g_off, 36)
               if tex_id < 0 { c += 1 pen_x += cw continue }
               if tex_id != page_tid_cur {
                  _ensure_glyph_text_page(tex_id)
                  page_tid_cur = tex_id
               }
               def bw = load32_f32(g_off, 12)
               def bh = load32_f32(g_off, 16)
               if bw > 0.0 && bh > 0.0 {
                  def fg = ((md & reverse_mask) != 0) ? (load32(line_ptr, off + 8) | 0xFF000000) : (load32(line_ptr, off + 4) | 0xFF000000)
                  def gx = floor(pen_x + load32_f32(g_off, 4) + 0.5)
                  def gy = floor(baseline_y - load32_f32(g_off, 8) + 0.5)
                  def color = (load32(g_off, 44) != 0) ? 0xFFFFFFFF : fg
                  _push_text_glyph_rect_unchecked(
                     gx,
                     gy,
                     bw,
                     bh,
                     load32_f32(g_off, 20),
                     load32_f32(g_off, 24),
                     load32_f32(g_off, 28),
                     load32_f32(g_off, 32),
                     color
                  )
                  glyph_count += 1
               }
            }
         }
      }
      c += 1
      pen_x += cw
   }
   _prim_text_calls += 1
   _prim_text_glyphs += glyph_count
   0
}
