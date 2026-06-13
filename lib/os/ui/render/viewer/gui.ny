;; Keywords: ui gui immediate-mode widgets layout input controls panels colorpicker os render viewer
;; Immediate-mode GUI widgets, layout state, input routing, and draw integration.
;; References:
;; - std.os.ui.render.viewer.batch
;; - std.os.ui.window
module std.os.ui.render.viewer.gui(set_enabled, enabled, set_scale, scale, set_fonts, set_accent, accent, set_debug_overlay, debug_overlay, rect, inset, center_rect, clamp_rect, split_cols, split_rows, apply_window_rect, apply_window_rect_if_visible, tile_editor_shell, tile_editor_shell_preset, prepare_input, feed_event, key_pressed, begin_frame, end_frame, reset_state, wants_mouse, wants_keyboard, hit_test, suppress_mouse_clicks, hovered_id, active_id, focused_id, mouse_down, mouse_press_seq, last_item_rect, focus_window, request_focus, clear_focus, show_window, window_visible, window_closed, set_window_pos, set_window_size, window_rect, reset_window_scroll, warm_text_pipeline, begin_window, end_window, begin_scroll_area, set_scroll_area_content_hint, scroll_area_ensure_visible, end_scroll_area, scroll_area_scroll_y, scroll_area_visible_h, push_clip_rect, pop_clip_rect, same_line, spacing, spacer_px, reset_cursor, layout_gap, remaining_h, separator, text, label, text_colored, property_row, title_input_text, button, small_button, image, icon_button, selectable, selectable_file, input_text, checkbox, toggle, radio_button, tab_strip, combo_box, slider_float, slider_int, progress_bar, stat_card, plot_lines, grid_canvas, node_canvas, collapsing_header, color_edit4, color_picker4)
use std.core
use std.math
use std.os.ui.window.consts
use std.os.ui.render
use std.os.ui.window as uiw
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.viewer.batch as gui_batch
use std.core.str as str
use std.core.common as common
use std.core.cache as cache

mut _enabled, _scale, _win = true, 1.0, 0
mut _font, _font_title, _font_small = 0, 0, 0
mut _fb_w, _fb_h = 0.0, 0.0
mut _mouse_x, _mouse_y = 0.0, 0.0
mut _mouse_down0, _mouse_down0_prev = false, false
mut _mouse_pressed0, _mouse_released0 = false, false
mut _mouse_press_x, _mouse_press_y = 0.0, 0.0
mut int: _mouse_press_seq = 0
mut _mouse_scroll_dx, _mouse_scroll_dy = 0.0, 0.0
mut _last_scroll_x, _last_scroll_y = 0.0, 0.0
mut _event_mouse_has_pos, _event_mouse_down0_known = false, false
mut _event_mouse_x, _event_mouse_y = 0.0, 0.0
mut _event_mouse_down0, _event_mouse_pressed0, _event_mouse_released0 = false, false, false
mut _suppress_mouse_frames, _suppress_mouse_until_up = 0, false
mut _event_scroll_dx, _event_scroll_dy = 0.0, 0.0
mut _key_events = []
mut _text_events = ""
mut _windows = dict(32)
mut _widget_state = dict(64)
mut _current_window, _current_window_id_cache = 0, ""
mut _current_body_x_cache, _current_body_y_cache = 0.0, 0.0
mut _current_body_w_cache, _current_body_h_cache = 0.0, 0.0
mut _current_cursor_y_cache, _focus_collect_active = 0.0, false
mut _window_hovered, _focused_window = "", ""
mut _hot_id, _active_widget_id = "", ""
mut _kbd_focus_id, _text_focus_id = "", ""
mut _active_window_move, _active_window_resize = "", ""
mut _nav_focus_target, _nav_focus_window = "", ""
mut _debug_force_combo_open = "\x00"
mut _window_order = []
mut _window_closed_events = dict(32)
mut _popup_hit_active, _popup_next_hit_active = false, false
mut _popup_hit_window, _popup_next_hit_window = "", ""
mut _popup_hit_x, _popup_hit_y, _popup_hit_w, _popup_hit_h = 0.0, 0.0, 0.0, 0.0
mut _popup_next_hit_x, _popup_next_hit_y, _popup_next_hit_w, _popup_next_hit_h = 0.0, 0.0, 0.0, 0.0
mut _popup_combo_active = false
mut _popup_combo_key = ""
mut _popup_combo_x, _popup_combo_y, _popup_combo_w, _popup_combo_h = 0.0, 0.0, 0.0, 0.0
mut _popup_combo_row_h, _popup_combo_max_scroll, _popup_combo_scroll_y = 0.0, 0.0, 0.0
mut _popup_combo_rows, _popup_combo_out, _popup_combo_first = 0, 0, 0
mut _popup_combo_labels = []
mut _move_off_x, _move_off_y = 0.0, 0.0
mut _resize_pad_x, _resize_pad_y = 0.0, 0.0
mut _cursor_arrow, _cursor_ibeam, _cursor_hand = 0, 0, 0
mut _cursor_resize, _cursor_move, _cursor_active, _cursor_want = 0, 0, 0, 0
mut _wants_mouse, _wants_keyboard, _debug_overlay = false, false, false
mut _last_item = [0.0, 0.0, 0.0, 0.0]
mut _hot_rect = [0.0, 0.0, 0.0, 0.0]
mut _active_rect = [0.0, 0.0, 0.0, 0.0]
mut _same_line_next, _same_line_gap = false, 0.0
mut _scroll_area_active, _scroll_area_parent, _scroll_area_id = false, 0, ""
mut _scroll_area_rect = [0.0, 0.0, 0.0, 0.0]
mut _scroll_area_start_y, _scroll_area_scroll_y = 0.0, 0.0
mut _scroll_area_clip_pushed, _scroll_area_capture_window = false, ""
mut _scroll_area_capture_rect = [0.0, 0.0, 0.0, 0.0]
mut _text_fit_cache = dict(512)
mut _text_measure_cache = dict(1024)
mut _font_h_cache = dict(16)
mut _text_run_queue = []
def int: _TEXT_SELECT_ALL = -9001
def int: _TEXT_COPY = -9002
def int: _TEXT_CUT = -9003
def int: _TEXT_PASTE = -9004
mut _theme_dirty, _theme_ready = true, false
mut _accent_rgba = [0.624, 0.525, 0.851, 0.96]
mut _press_rgba = [0.200, 0.137, 0.278, 0.96]
mut _window_bg_rgba = [0.000, 0.000, 0.000, 0.92]
mut _window_body_rgba = [0.031, 0.031, 0.031, 0.90]
mut _window_hdr_rgba = [0.075, 0.075, 0.094, 0.96]
mut _panel_rgba = [0.031, 0.031, 0.039, 0.84]
mut _panel_hover_rgba = [0.106, 0.086, 0.141, 0.90]
mut _panel_active_rgba = [0.161, 0.118, 0.220, 0.94]
mut _popup_rgba = [0.000, 0.000, 0.000, 1.00]

fn _ensure_text_caches() any {
   if(!is_dict(_text_fit_cache)){ _text_fit_cache = dict(512) }
   if(!is_dict(_text_measure_cache)){ _text_measure_cache = dict(1024) }
   if(!is_dict(_font_h_cache)){ _font_h_cache = dict(16) }
}

fn _ui_text(any txt) str {
   if(is_str(txt)){ return txt }
   if(txt == nil){ return "" }
   if(is_int(txt) || is_float(txt) || is_bool(txt)){ return to_str(txt) }
   if(is_list(txt)){ return "[" + to_str(txt.len) + "]" }
   if(is_dict(txt)){ return "{dict}" }
   def s = to_str(txt)
   is_str(s) ? s : ""
}

mut _border_rgba = [0.196, 0.169, 0.247, 0.66]
mut _text_rgba = [0.961, 0.961, 0.965, 0.98]
mut _text_dim_rgba = [0.776, 0.776, 0.792, 0.90]
mut _ok_rgba = [0.624, 0.525, 0.851, 0.96]
mut _warn_rgba = [0.776, 0.776, 0.792, 0.96]
mut _danger_rgba = [0.200, 0.137, 0.278, 0.96]
mut _accent_u32 = 0
mut _accent_soft_u32 = 0
mut _press_u32 = 0
mut _press_soft_u32 = 0
mut _window_bg_u32 = 0
mut _window_body_u32 = 0
mut _window_hdr_u32 = 0
mut _panel_u32 = 0
mut _panel_hover_u32 = 0
mut _panel_active_u32 = 0
mut _popup_u32 = 0
mut _border_u32 = 0
mut _scroll_track_u32 = 0
mut _scroll_thumb_u32 = 0
mut _scroll_thumb_hot_u32 = 0
mut _grid_major_u32 = 0
mut _grid_minor_u32 = 0
mut _text_u32 = 0
mut _text_dim_u32 = 0
mut _ok_u32 = 0
mut _warn_u32 = 0
mut _danger_u32 = 0

@inline
@jit
fn _sx(f64 v) f64 { float(v) * _scale }

comptime template _scaled_metric(name, base){
   @inline
   @jit
   fn ${name}() f64 { _sx(base) }
}

comptime template _scaled_metric_triplet(n1, b1, n2, b2, n3, b3){
   comptime emit _scaled_metric(n1, b1)
   comptime emit _scaled_metric(n2, b2)
   comptime emit _scaled_metric(n3, b3)
}

def _M_PAD = 8.0
def _M_GAP = 5.0
def _M_TINY_GAP = 2.0
def _M_TITLE_H = 24.0
def _M_ITEM_PAD_X = 7.0
def _M_ITEM_PAD_Y = 3.0
def _M_SLIDER_H = 38.0
def _M_TOGGLE_W = 44.0
def _M_TOGGLE_H = 22.0
def _M_RESIZE_H = 16.0
def _M_MIN_WIN_W = 240.0
def _M_MIN_WIN_H = 96.0
comptime emit _scaled_metric_triplet(_pad, _M_PAD, _gap, _M_GAP, _tiny_gap, _M_TINY_GAP)
comptime emit _scaled_metric_triplet(_title_h, _M_TITLE_H, _item_pad_x, _M_ITEM_PAD_X, _item_pad_y, _M_ITEM_PAD_Y)

@inline
@jit
fn _item_h() f64 { max(_text_h() + _item_pad_y() * 2.0, _sx(26.0)) }

@inline
@jit
fn _small_item_h() f64 { max(_text_h() + _sx(5.0), _sx(22.0)) }
comptime emit _scaled_metric_triplet(_slider_h, _M_SLIDER_H, _toggle_w, _M_TOGGLE_W, _toggle_h, _M_TOGGLE_H)
comptime emit _scaled_metric_triplet(_resize_h, _M_RESIZE_H, _min_win_w, _M_MIN_WIN_W, _min_win_h, _M_MIN_WIN_H)

@inline
@jit
fn _snap(f64 v) f64 { float(int(v)) }

fn _clear_pending_input_events() any {
   _event_mouse_has_pos, _event_mouse_down0_known, _event_mouse_down0 = false, false, false
   _event_mouse_pressed0, _event_mouse_released0 = false, false
   _event_scroll_dx, _event_scroll_dy = 0.0, 0.0
}

fn suppress_mouse_clicks(int frames=3, bool until_up=true) any {
   "Suppresses mouse click transitions for a few frames after focus or modal changes."
   mut n = int(frames)
   if(n < 1){ n = 1 }
   if(n > _suppress_mouse_frames){ _suppress_mouse_frames = n }
   if(until_up){ _suppress_mouse_until_up = true }
   _event_mouse_pressed0, _event_mouse_released0 = false, false
   _event_mouse_down0_known = false
}

@inline
@jit
fn _pick_positive(any v, f64 fallback) f64 {
   def fv = float(v)
   if(fv > 0.0){ return fv }
   float(fallback)
}

@inline
@jit
fn _drag_threshold() f64 { max(2.0, _sx(4.0)) }

@inline
@jit
fn _scroll_step() f64 { max(_sx(18.0), _small_item_h() * 1.35) }

fn _mouse_drag_ready() bool {
   def dx, dy = _mouse_x - _mouse_press_x, _mouse_y - _mouse_press_y
   (dx * dx + dy * dy) >= (_drag_threshold() * _drag_threshold())
}

fn _ensure_cursors() any {
   if(!_win){ return 0 }
   if(!_cursor_arrow){ _cursor_arrow = uiw.create_standard_cursor(uiw.ARROW_CURSOR) }
   if(!_cursor_ibeam){ _cursor_ibeam = uiw.create_standard_cursor(uiw.IBEAM_CURSOR) }
   if(!_cursor_hand){ _cursor_hand = uiw.create_standard_cursor(uiw.POINTING_HAND_CURSOR) }
   if(!_cursor_resize){ _cursor_resize = uiw.create_standard_cursor(uiw.RESIZE_NWSE_CURSOR) }
   if(!_cursor_move){ _cursor_move = uiw.create_standard_cursor(uiw.RESIZE_ALL_CURSOR) }
}

fn _want_cursor(any cursor) any { _cursor_want = cursor }

fn _apply_cursor() any {
   if(!_win){ return 0 }
   _ensure_cursors()
   def want = common.value_or(_cursor_want, _cursor_arrow)
   if(want != _cursor_active){
      uiw.set_cursor(_win, want)
      _cursor_active = want
   }
}

fn _theme_pack(any c) int { color_pack(float(c.get(0, 1.0)), float(c.get(1, 1.0)), float(c.get(2, 1.0)), float(c.get(3, 1.0))) }

fn rect(f64 x, f64 y, f64 w, f64 h) list {
   "Builds a normalized [x, y, w, h] rectangle."
   [float(x), float(y), max(0.0, float(w)), max(0.0, float(h))]
}

fn inset(list r, f64 pad) list {
   "Returns `r` inset by `pad` on all sides."
   def p, x = max(0.0, float(pad)), float(r.get(0, 0.0)) + p
   def y, w = float(r.get(1, 0.0)) + p, max(0.0, float(r.get(2, 0.0)) - p * 2.0)
   def h = max(0.0, float(r.get(3, 0.0)) - p * 2.0)
   [x, y, w, h]
}

fn center_rect(list bounds, f64 w, f64 h) list {
   "Returns a rectangle of size `w` by `h` centered inside `bounds`."
   def bx, by = float(bounds.get(0, 0.0)), float(bounds.get(1, 0.0))
   def bw, bh = max(0.0, float(bounds.get(2, 0.0))), max(0.0, float(bounds.get(3, 0.0)))
   def rw, rh = min(max(0.0, float(w)), bw), min(max(0.0, float(h)), bh)
   [bx + max(0.0, (bw - rw) * 0.5), by + max(0.0, (bh - rh) * 0.5), rw, rh]
}

fn clamp_rect(list r, list bounds, f64 min_w=1.0, f64 min_h=1.0) list {
   "Clamps rectangle `r` inside `bounds` with minimum dimensions."
   def bx, by = float(bounds.get(0, 0.0)), float(bounds.get(1, 0.0))
   def bw, bh = max(0.0, float(bounds.get(2, 0.0))), max(0.0, float(bounds.get(3, 0.0)))
   def rw, rh = min(max(float(min_w), float(r.get(2, 0.0))), bw), min(max(float(min_h), float(r.get(3, 0.0))), bh)
   def max_x, max_y = bx + max(0.0, bw - rw), by + max(0.0, bh - rh)
   [clamp(float(r.get(0, bx)), bx, max_x), clamp(float(r.get(1, by)), by, max_y), rw, rh]
}

fn _weights_total(list weights) f64 {
   mut total = 0.0
   mut i = 0
   def weights_n = weights.len
   while(i < weights_n){
      total += max(0.0, float(weights.get(i, 0.0)))
      i += 1
   }
   if(total > 0.000001){ return total }
   1.0
}

fn _weight_or(list weights, int idx, f64 fallback) f64 {
   def value = float(weights.get(idx, fallback))
   if(value > 0.000001){ return value }
   float(fallback)
}

fn _center_row_weights(f64 top_ratio, f64 mid_ratio, list weights) list {
   if(weights.len >= 3){ return [_weight_or(weights, 0, 0.34), _weight_or(weights, 1, 0.24), _weight_or(weights, 2, 0.42)] }
   def top = max(0.12, float(top_ratio))
   def mid = max(0.12, float(mid_ratio))
   [top, mid, max(0.26, 1.0 - top - mid)]
}

fn _right_stack_weights(list weights) list {
   if(weights.len >= 4){ return [_weight_or(weights, 0, 1.12), _weight_or(weights, 1, 0.92), _weight_or(weights, 2, 0.78), _weight_or(weights, 3, 0.88)] }
   if(weights.len == 3){ return [_weight_or(weights, 0, 1.12), _weight_or(weights, 1, 0.92), _weight_or(weights, 2, 0.78), 0.88] }
   if(weights.len == 2){ return [_weight_or(weights, 0, 1.12), _weight_or(weights, 1, 0.92), 0.78, 0.88] }
   [1.12, 0.92, 0.78, 0.88]
}

fn _split_rect_axis(list r, list weights, f64 gap, bool vertical) list {
   def x0, y0 = float(r.get(0, 0.0)), float(r.get(1, 0.0))
   def ww = max(0.0, float(r.get(2, 0.0)))
   def hh = max(0.0, float(r.get(3, 0.0)))
   def n = max(weights.len, 1)
   def g = max(0.0, float(gap))
   def total = _weights_total(weights)
   def full = vertical ? hh : ww
   def inner = max(0.0, full - g * float(max(n - 1, 0)))
   mut out = []
   mut pos = vertical ? y0 : x0
   mut i = 0
   while(i < n){
      def wt = max(0.0, float(weights.get(i, 1.0)))
      mut part = inner * (wt / total)
      if(i == n - 1){ part = max(0.0, (vertical ? y0 : x0) + full - pos) }
      out = out.append(vertical ? [x0, pos, ww, max(0.0, part)] : [pos, y0, max(0.0, part), hh])
      pos += part + g
      i += 1
   }
   out
}

fn split_cols(list r, list weights, f64 gap=0.0) list { "Splits `r` into weighted columns." _split_rect_axis(r, weights, gap, false) }

fn split_rows(list r, list weights, f64 gap=0.0) list { "Splits `r` into weighted rows." _split_rect_axis(r, weights, gap, true) }

fn apply_window_rect(any id, list r) any {
   "Applies a stored window rectangle by widget/window id."
   def key = to_str(id)
   if(key.len == 0){ return 0 }
   mut st = _window_lookup(key)
   if(!is_dict(st)){
      st = dict(12)
      st["id"] = key
      st["open"] = true
   }
   def nx, ny = float(r.get(0, 0.0)), float(r.get(1, 0.0))
   def nw, nh = max(float(r.get(2, 0.0)), _min_win_w()), max(float(r.get(3, 0.0)), _min_win_h())
   def moved = abs(float(st.get("x", nx)) - nx) > 0.5 ||
   abs(float(st.get("y", ny)) - ny) > 0.5 ||
   abs(float(st.get("w", nw)) - nw) > 0.5 ||
   abs(float(st.get("h", nh)) - nh) > 0.5
   st["x"] = nx
   st["y"] = ny
   st["w"] = nw
   st["h"] = nh
   if(moved){
      st["scroll_y"] = 0.0
      st["content_h"] = 0.0
   }
   st = _clamp_window_pos(st)
   _window_store(st)
}

fn apply_window_rect_if_visible(any id, bool visible, list r) any { "Applies `r` only when the window is visible." if(visible){ apply_window_rect(id, r) } }

fn _tile_editor_shell_map(list editor, list center, list side, list center_rows, list side_rows) dict {
   {
      "editor_main": editor, "center_main": center, "side_main": side,
      "node_graph": center_rows.get(0, center),
      "workspace_grid": center_rows.get(1, center),
      "asset_browser": center_rows.get(2, center),
      "inspector": side_rows.get(0, side),
      "profiler": side_rows.get(1, side),
      "widget_probe": side_rows.get(2, side),
      "widget_gallery": side_rows.get(3, side)
   }
}

fn _tile_editor_shell_cfg(f64 ww, f64 wh, f64 gap, f64 left_ratio, f64 top_ratio, f64 mid_ratio, list top_weights, list bottom_weights, list right_stack_weights) dict {
   def g = max(0.0, float(gap))
   def root = rect(0.0, 0.0, max(0.0, float(ww)), max(0.0, float(wh)))
   def root_x = float(root.get(0, 0.0))
   def root_y = float(root.get(1, 0.0))
   def root_w = float(root.get(2, 0.0))
   def root_h = float(root.get(3, 0.0))
   def compact_stack = root_w < 780.0 || root_h < 420.0
   def compact_narrow = root_w < 820.0
   def compact_short = root_h < 620.0
   def compact_tiny = root_w < 720.0 || root_h < 500.0
   def compact = root_w < 1360.0
   if(compact_stack){
      def editor_min = compact_tiny ? 112.0 : (compact_short ? 144.0 : 190.0)
      def editor_max = compact_short ? 260.0 : 340.0
      def lower_min = compact_tiny ? 128.0 : (compact_short ? 190.0 : 280.0)
      mut editor_h = clamp(root_h * (compact_tiny ? 0.30 : (compact_short ? 0.32 : (compact_narrow ? 0.34 : 0.36))), editor_min, editor_max)
      if(root_h - editor_h - g < lower_min){ editor_h = max(editor_min, root_h - g - lower_min) }
      def editor = [root_x, root_y, root_w, editor_h]
      def lower = [root_x, root_y + editor_h + g, root_w, max(1.0, root_h - editor_h - g)]
      mut center = lower
      mut side = lower
      if(compact_narrow || compact_short){
         def lower_rows = split_rows(lower, compact_tiny ? [1.0, 0.72] : [1.08, 0.82], g)
         center, side = lower_rows.get(0, lower), lower_rows.get(1, lower)
      } else {
         def lower_cols = split_cols(lower, [1.18, 0.82], g)
         center, side = lower_cols.get(0, lower), lower_cols.get(1, lower)
      }
      def center_rows = split_rows(center, _center_row_weights(top_ratio, mid_ratio, top_weights), g)
      def side_rows = split_rows(side, _right_stack_weights(right_stack_weights), g)
      return _tile_editor_shell_map(editor, center, side, center_rows, side_rows)
   }
   def editor_left_max = float(ui_profile.env_int_cached("NY_EDITOR_LEFT_MAX_W", 360, 260, 520))
   mut left_w = clamp(root_w * float(left_ratio), compact ? 250.0 : 280.0, compact ? min(360.0, editor_left_max) : min(400.0, editor_left_max))
   if(root_w < 980.0){ left_w = clamp(root_w * 0.28, 220.0, min(280.0, editor_left_max)) }
   if(root_w >= 980.0 && left_w > editor_left_max){ left_w = editor_left_max }
   def right_x = root_x + left_w + g
   mut right_w = max(1.0, root_w - left_w - g)
   def center_bias = _weight_or(bottom_weights, 0, 1.0)
   def side_bias = _weight_or(bottom_weights, 1, 0.86)
   def side_share = clamp(0.36 * (side_bias / max(0.05, center_bias)), compact ? 0.26 : 0.24, compact ? 0.42 : 0.38)
   mut sidebar_w = clamp(right_w * side_share, compact ? 290.0 : 340.0, compact ? 430.0 : 500.0)
   if(right_w < 760.0){ sidebar_w = clamp(right_w * 0.40, 230.0, 300.0) }
   mut center_w = right_w - sidebar_w - g
   if(center_w < 360.0){
      center_w = max(1.0, right_w * 0.54)
      sidebar_w = max(1.0, right_w - center_w - g)
   }
   def center_x = right_x
   def sidebar_x = center_x + center_w + g
   def editor = [root_x, root_y, left_w, root_h]
   def center = [center_x, root_y, center_w, root_h]
   def side = [sidebar_x, root_y, sidebar_w, root_h]
   def center_rows = split_rows(center, _center_row_weights(top_ratio, mid_ratio, top_weights), g)
   def side_rows = split_rows(side, _right_stack_weights(right_stack_weights), g)
   _tile_editor_shell_map(editor, center, side, center_rows, side_rows)
}

fn tile_editor_shell(f64 ww, f64 wh, f64 gap=10.0, f64 left_ratio=0.27, f64 top_ratio=0.33, f64 mid_ratio=0.24) dict {
   "Returns the default editor shell layout rectangles."
   _tile_editor_shell_cfg(ww, wh, gap, left_ratio, top_ratio, mid_ratio, [], [1.0, 0.86], [])
}

fn tile_editor_shell_preset(str name, f64 ww, f64 wh, f64 gap=10.0) dict {
   "Returns named editor shell layout presets."
   def preset = str.lower(str.strip(to_str(name)))
   case preset {
      "probe", "validate", "validation" -> _tile_editor_shell_cfg(ww, wh, gap, 0.30, 0.30, 0.22,
      [0.30, 0.22, 0.48], [1.02, 1.04], [1.10, 0.88, 0.82, 0.88])
      "graph", "nodes" -> _tile_editor_shell_cfg(ww, wh, gap, 0.24, 0.42, 0.18,
      [0.48, 0.18, 0.34], [1.16, 0.78], [0.92, 0.84, 0.70, 0.74])
      "compact", "dense" -> _tile_editor_shell_cfg(ww, wh, max(0.0, gap - 2.0), 0.22, 0.24, 0.20,
      [0.24, 0.20, 0.56], [0.92, 1.12], [1.10, 0.90, 0.82, 0.92])
      _ -> tile_editor_shell(ww, wh, gap, 0.27, 0.33, 0.24)
   }
}

fn _theme_refresh() any {
   if(!_theme_dirty && _theme_ready){ return 0 }
   _accent_u32 = _theme_pack(_accent_rgba)
   _press_u32 = _theme_pack(_press_rgba)
   _press_soft_u32 = color_pack(float(_press_rgba.get(0, 0.0)), float(_press_rgba.get(1, 0.0)), float(_press_rgba.get(2, 0.0)), 0.58)
   _accent_soft_u32 = color_pack(float(_accent_rgba.get(0,
      0.0)),
      float(_accent_rgba.get(1,
      0.0)),
      float(_accent_rgba.get(2,
      0.0)),
   0.24)
   _window_bg_u32 = _theme_pack(_window_bg_rgba)
   _window_body_u32 = _theme_pack(_window_body_rgba)
   _window_hdr_u32 = _theme_pack(_window_hdr_rgba)
   _panel_u32 = _theme_pack(_panel_rgba)
   _panel_hover_u32 = _theme_pack(_panel_hover_rgba)
   _panel_active_u32 = _theme_pack(_panel_active_rgba)
   _popup_u32 = _theme_pack(_popup_rgba)
   _border_u32 = _theme_pack(_border_rgba)
   _scroll_track_u32 = color_pack(0.031, 0.031, 0.031, 0.58)
   _scroll_thumb_u32 = color_pack(float(_accent_rgba.get(0,
      0.0)),
      float(_accent_rgba.get(1,
      0.0)),
      float(_accent_rgba.get(2,
      0.0)),
   0.46)
   _scroll_thumb_hot_u32 = color_pack(float(_accent_rgba.get(0,
      0.0)),
      float(_accent_rgba.get(1,
      0.0)),
      float(_accent_rgba.get(2,
      0.0)),
   0.62)
   _grid_major_u32 = color_pack(0.298, 0.298, 0.322, 0.22)
   _grid_minor_u32 = color_pack(0.114, 0.114, 0.141, 0.10)
   _text_u32 = _theme_pack(_text_rgba)
   _text_dim_u32 = _theme_pack(_text_dim_rgba)
   _ok_u32 = _theme_pack(_ok_rgba)
   _warn_u32 = _theme_pack(_warn_rgba)
   _danger_u32 = _theme_pack(_danger_rgba)
   _theme_dirty = false
   _theme_ready = true
}

fn set_enabled(bool v) any { "Enables or disables GUI input and rendering helpers." _enabled = !!v }

fn enabled() bool { "Returns true when GUI helpers are enabled." _enabled }

fn reset_state() any {
   "Clears transient widget, focus, and scroll-area state."
   _widget_state = dict(64)
   _hot_id, _active_widget_id = "", ""
   _kbd_focus_id, _text_focus_id = "", ""
   _nav_focus_target, _nav_focus_window = "", ""
   _last_item, _hot_rect, _active_rect = [0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0]
   _same_line_next, _same_line_gap = false, 0.0
   _scroll_area_active, _scroll_area_parent, _scroll_area_id = false, 0, ""
   _scroll_area_rect = [0.0, 0.0, 0.0, 0.0]
   _scroll_area_start_y, _scroll_area_scroll_y = 0.0, 0.0
   _scroll_area_clip_pushed, _scroll_area_capture_window = false, ""
   _scroll_area_capture_rect = [0.0, 0.0, 0.0, 0.0]
}

fn _reset_font_metric_caches() any {
   _text_fit_cache, _text_measure_cache, _font_h_cache = dict(512), dict(1024), dict(16)
}

fn set_fonts(int body_font, int title_font=0, int small_font=0) any {
   "Sets body, title, and small font handles used by GUI text."
   def old_body, old_title, old_small = _font, _font_title, _font_small
   if(is_int(body_font) && body_font > 0){ _font = int(body_font) }
   _font_title = _font
   if(is_int(title_font) && title_font > 0){ _font_title = int(title_font) }
   _font_small = _font
   if(is_int(small_font) && small_font > 0){ _font_small = int(small_font) }
   if(old_body != _font || old_title != _font_title || old_small != _font_small){ _reset_font_metric_caches() }
}

fn set_scale(f64 v) any {
   "Sets UI scale, clamped to a practical range."
   mut nv = float(v)
   if(nv < 0.50){ nv = 0.50 } elif(nv > 3.00){ nv = 3.00 }
   if(abs(nv - _scale) > 0.000001){ _reset_font_metric_caches() }
   _scale = nv
}

fn scale() f64 { "Returns the current UI scale." _scale }

fn set_accent(any c) any {
   "Sets the accent color from an RGBA list."
   def r, g = clamp(float(c.get(0, 0.72)), 0.0, 1.0), clamp(float(c.get(1, 0.58)), 0.0, 1.0)
   def b, a = clamp(float(c.get(2, 0.96)), 0.0, 1.0), clamp(float(c.get(3, 0.96)), 0.0, 1.0)
   if(abs(r - float(_accent_rgba.get(0, 0.72))) <= 0.000001 &&
      abs(g - float(_accent_rgba.get(1, 0.58))) <= 0.000001 &&
      abs(b - float(_accent_rgba.get(2, 0.96))) <= 0.000001 &&
      abs(a - float(_accent_rgba.get(3, 0.96))) <= 0.000001){
      return 0
   }
   _accent_rgba = [r, g, b, a]
   _theme_dirty = true
}

fn accent() list { "Returns the current accent RGBA color." _accent_rgba }

fn set_debug_overlay(bool v) any { "Enables or disables the GUI debug overlay." _debug_overlay = !!v }

fn debug_overlay() bool { "Returns true when the GUI debug overlay is enabled." _debug_overlay }

@inline
@jit
fn _font_body() int { (_font > 0) ? _font : _font_title }

@inline
@jit
fn _font_title_id() int { (_font_title > 0) ? _font_title : _font_body() }

@inline
@jit
fn _font_small_id() int { (_font_small > 0) ? _font_small : _font_body() }

fn _measure_text_ui(int font_id, any txt) list {
   _ensure_text_caches()
   def fid = (font_id > 0) ? font_id : _font_body()
   def s = _ui_text(txt)
   def n = str.len(s)
   def key = (fid > 0 && n <= 256) ? (to_str(fid) + ":" + s) : ""
   def use_cache = is_str(key) && str.len(key) > 0
   if(use_cache && _text_measure_cache.contains(key)){ return _text_measure_cache.get(key, [0.0, 0.0]) }
   def m = measure_text_fast(fid, s)
   if(use_cache){ _text_measure_cache = cache.cache_put_reset(_text_measure_cache, key, m, 4096, 1024) }
   m
}

fn _font_h_for(int font_id) f64 {
   _ensure_text_caches()
   def fid = (font_id > 0) ? font_id : _font_body()
   def key = to_str(fid)
   if(_font_h_cache.contains(key)){ return float(_font_h_cache.get(key, _sx(15.0))) }
   mut h = float(font_line_height(fid))
   mut asc = float(font_ascent(fid))
   if(asc > 0.0){
      h = min(h, asc + _sx(3.0))
   }
   if(h < _sx(10.0)){
      def m = _measure_text_ui(fid, "Ag")
      h = float(m.get(1, _sx(15.0)))
   }
   if(h < _sx(10.0)){ h = _sx(15.0) }
   _font_h_cache[key] = h
   h
}

fn _text_h() f64 { _font_h_for(_font_body()) }

fn _text_fit_cache_key(int font_id, any txt, f64 max_w) str {
   def s = _ui_text(txt)
   (str.len(s) <= 256 && float(max_w) > 0.0) ? (to_str(font_id) + ":" + to_str(int(float(max_w) + 0.5)) + ":" + s) : ""
}

fn _text_fit_cache_put(str key, str value) str {
   _ensure_text_caches()
   def k, v = to_str(key), to_str(value)
   if(k.len > 0){ _text_fit_cache = cache.cache_put_reset(_text_fit_cache, k, v, 4096, 512) }
   v
}

fn _text_fit_width(int font_id, str s) f64 {
   def measured = float(_measure_text_ui(font_id, s).get(0, 0.0))
   def n = str.len(s)
   if(n <= 0){ return measured }
   max(measured, float(n) * _font_h_for(font_id) * 0.56)
}

fn _text_likely_fits_ascii(int font_id, str s, f64 max_w) bool {
   def n = str.len(s)
   if(float(max_w) <= 0.0 || n <= 0 || n > 64){ return false }
   if(!str.ascii_only(s)){ return false }
   float(n) * _font_h_for(font_id) * 0.64 <= float(max_w)
}

fn _text_fit(int font_id, any txt, f64 max_w=0.0) str {
   _ensure_text_caches()
   def s = _ui_text(txt)
   if(float(max_w) <= 0.0){ return s }
   def fid = (font_id > 0) ? font_id : _font_body()
   if(_text_likely_fits_ascii(fid, s, max_w)){ return s }
   def n = str.len(s)
   if(n <= 48 && _text_fit_width(fid, s) <= float(max_w)){ return s }
   def cache_key = _text_fit_cache_key(fid, s, max_w)
   def use_cache = is_str(cache_key) && str.len(cache_key) > 0
   if(use_cache && _text_fit_cache.contains(cache_key)){ return to_str(_text_fit_cache.get(cache_key, s)) }
   if(_text_fit_width(fid, s) <= float(max_w)){ return _text_fit_cache_put(cache_key, s) }
   def ell = "..."
   def total_n = n
   mut lo = 0
   mut hi = total_n
   mut best = 0
   while(lo <= hi){
      def mid_raw = (lo + hi) / 2
      def mid = _utf8_prev_index(s, mid_raw)
      def cand = str.str_slice(s, 0, mid) + ell
      if(_text_fit_width(fid, cand) <= float(max_w)){
         best = mid
         lo = max(mid_raw + 1, mid + 1)
      } else {
         hi = mid_raw - 1
      }
   }
   if(best > 0){ return _text_fit_cache_put(cache_key, str.str_slice(s, 0, best) + ell) }
   _text_fit_cache_put(cache_key, ell)
}

fn _text_color_u32(any color) int {
   if(is_int(color)){ return color }
   def col = is_list(color) ? color : (is_tuple(color) ? color : _text_rgba)
   color_pack(
      float(col.get(0, 1.0)),
      float(col.get(1, 1.0)),
      float(col.get(2, 1.0)),
      float(col.get(3, 1.0))
   )
}

fn _flush_rect_batch() int { gui_batch.flush() }

fn _gui_rect_fast(f64 x, f64 y, f64 w, f64 h, int color) bool {
   gui_batch.queue_rect(_snap(x), _snap(y), max(1.0, _snap(w)), max(1.0, _snap(h)), color)
}

fn _gui_rect_outline_fast(f64 x, f64 y, f64 w, f64 h, int color) bool {
   gui_batch.queue_outline(_snap(x), _snap(y), max(1.0, _snap(w)), max(1.0, _snap(h)), color)
}

fn _gui_rect_tex_uv(
   f64 x, f64 y, f64 w, f64 h, int tex_id,
   f64 u1, f64 v1, f64 u2, f64 v2,
   f64 r, f64 g, f64 b, f64 a) bool {
   gui_batch.draw_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a)
}

fn _tex_ref_id(any tex_ref) int {
   is_dict(tex_ref) ? int(tex_ref.get("tex", -1)) : int(tex_ref)
}

fn _tex_ref_uv(any tex_ref) list {
   if(is_dict(tex_ref)){
      def uv = tex_ref.get("uv", 0)
      if(is_list(uv) && uv.len >= 4){ return uv }
   }
   [0.0, 0.0, 1.0, 1.0]
}

fn _gui_tex_ref_uv(
   f64 x, f64 y, f64 w, f64 h, any tex_ref,
   f64 r=1.0, f64 g=1.0, f64 b=1.0, f64 a=1.0
) bool {
   def tex = _tex_ref_id(tex_ref)
   if(tex < 0){ return false }
   def uv = _tex_ref_uv(tex_ref)
   _gui_rect_tex_uv(
      x, y, w, h, tex,
      float(uv.get(0, 0.0)), float(uv.get(1, 0.0)),
      float(uv.get(2, 1.0)), float(uv.get(3, 1.0)),
      r, g, b, a
   )
}

fn _draw_text_ui_now(int font_id, any txt, f64 x, f64 y, any color, f64 shadow_alpha=0.0, f64 max_w=0.0) any {
   _flush_rect_batch()
   def px, py = _snap(float(x)), _snap(float(y))
   def fid = (font_id > 0) ? font_id : _font_body()
   def fit_txt = _text_fit(fid, txt, max_w)
   def packed = _text_color_u32(color)
   def a = float((packed >> 24) & 255) / 255.0
   def sh_a = clamp(float(shadow_alpha), 0.0, 1.0)
   if(a > 0.001 && sh_a > 0.001){ draw_text(fid, fit_txt, px + 1.0, py + 1.0, color_pack(0.0, 0.0, 0.0, min(sh_a, a * sh_a))) }
   draw_text(fid, fit_txt, px, py, packed)
}

fn _queue_text_ui(int font_id, any txt, f64 x, f64 y, any color, f64 max_w=0.0) any {
   def fid = (font_id > 0) ? font_id : _font_body()
   def fit_txt = _text_fit(fid, txt, max_w)
   if(str.len(fit_txt) <= 0){ return 0 }
   if(!is_list(_text_run_queue)){ _text_run_queue = [] }
   _text_run_queue = _text_run_queue.append(fid)
   _text_run_queue = _text_run_queue.append(fit_txt)
   _text_run_queue = _text_run_queue.append(_snap(float(x)))
   _text_run_queue = _text_run_queue.append(_snap(float(y)))
   _text_run_queue = _text_run_queue.append(_text_color_u32(color))
}

fn _flush_text_runs() any {
   _flush_rect_batch()
   if(!is_list(_text_run_queue)){ _text_run_queue = [] }
   def qn = _text_run_queue.len
   if(qn <= 0){ return 0 }
   def prof = ui_profile.enabled()
   def t0 = prof ? ui_profile.now() : 0
   mut batches = 0
   mut batch_font = -1
   mut batch = []
   mut i = 0
   while(i + 4 < qn){
      def fid, txt = int(_text_run_queue.get(i, _font_body())), _text_run_queue.get(i + 1, "")
      def x, y = float(_text_run_queue.get(i + 2, 0.0)), float(_text_run_queue.get(i + 3, 0.0))
      def col = int(_text_run_queue.get(i + 4, 0xffffffff))
      if(is_str(txt) && str.len(txt) > 0){
         if(batch.len > 0 && fid != batch_font){
            draw_text_runs_flat_colors(batch_font, batch)
            batch = []
            batches += 1
         }
         batch_font = fid
         batch = batch.append(txt)
         batch = batch.append(x)
         batch = batch.append(y)
         batch = batch.append(col)
      }
      i += 5
   }
   if(batch.len > 0){
      draw_text_runs_flat_colors(batch_font, batch)
      batches += 1
   }
   _text_run_queue = []
   if(prof){ ui_profile.mark("flush_text_internal", t0, " runs=" + to_str(qn / 5) + " batches=" + to_str(batches)) }
}

fn warm_text_pipeline(int body_font=0, int title_font=0, int small_font=0) bool {
   "Warms GUI font atlases for interactive panels without submitting visible text."
   if(!_enabled){ return false }
   if(body_font > 0){
      set_fonts(body_font, title_font, small_font)
   }
   _theme_refresh()
   _text_run_queue = []
   def sample = "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789 [](){}<>:/._-+*=%,"
   if(_font_body() > 0){ font_prepare(_font_body(), sample) }
   if(_font_title_id() > 0){ font_prepare(_font_title_id(), sample) }
   if(_font_small_id() > 0){ font_prepare(_font_small_id(), sample) }
   true
}

fn _draw_text_ui_ex(int font_id, any txt, f64 x, f64 y, any color, f64 shadow_alpha=0.0, f64 max_w=0.0) any {
   def px, py = _snap(float(x)), _snap(float(y))
   def fid = (font_id > 0) ? font_id : _font_body()
   def sh_a = clamp(float(shadow_alpha), 0.0, 1.0)
   if(sh_a > 0.001 || ui_profile.text_immediate_enabled()){
      _draw_text_ui_now(fid, txt, px, py, color, sh_a, max_w)
      return 0
   }
   _queue_text_ui(fid, txt, px, py, color, max_w)
}

fn _draw_text_ui_ex_clipped(int font_id,
   any txt,
   f64 x,
   f64 y,
   any color,
   f64 shadow_alpha=0.0,
   f64 max_w=0.0,
   any clip_rect=0) any {
   if(is_list(clip_rect) && clip_rect.len >= 4){
      def s = _ui_text(txt)
      def limit_w = float(max_w)
      def clip_x = float(clip_rect.get(0, 0.0))
      def clip_y = float(clip_rect.get(1, 0.0))
      def clip_w = max(0.0, float(clip_rect.get(2, 0.0)))
      def clip_h = max(0.0, float(clip_rect.get(3, 0.0)))
      def fit_w = (limit_w > 0.0 && clip_w > 0.0) ? min(limit_w, clip_w) : max(limit_w, clip_w)
      def fid = (font_id > 0) ? font_id : _font_body()
      def font_h = _font_h_for(fid)
      def y_inside = float(y) >= clip_y - 0.5 && (float(y) + font_h) <= (clip_y + clip_h + 0.5)
      if(fit_w > 0.0 && float(x) >= clip_x - 0.5 && y_inside){
         _draw_text_ui_ex(fid, s, x, y, color, shadow_alpha, fit_w)
         return 0
      }
      def likely_fits = float(x) >= clip_x - 0.5 && _text_likely_fits_ascii(fid, s, fit_w + _sx(2.0))
      def needs_hard_clip = fit_w <= 0.0 || (!likely_fits && (float(x) < clip_x - 0.5 || _text_fit_width(fid, s) > fit_w + _sx(2.0)))
      if(needs_hard_clip){
         push_clip_rect(clip_rect)
         _draw_text_ui_now(fid, s, x, y, color, shadow_alpha, fit_w)
         pop_clip_rect()
      } else {
         _draw_text_ui_ex(fid, s, x, y, color, shadow_alpha, fit_w)
      }
      return 0
   }
   _draw_text_ui_ex(font_id, txt, x, y, color, shadow_alpha, max_w)
}

fn _draw_text_ui_clipped(any txt, f64 x, f64 y, any color, f64 max_w=0.0, any clip_rect=0) any { _draw_text_ui_ex_clipped(_font_body(), txt, x, y, color, 0.0, max_w, clip_rect) }

fn _draw_text_ui_small_clipped(any txt, f64 x, f64 y, any color, f64 max_w=0.0, any clip_rect=0) any { _draw_text_ui_ex_clipped(_font_small_id(), txt, x, y, color, 0.0, max_w, clip_rect) }

fn _draw_text_ui_title_clipped(any txt, f64 x, f64 y, any color, f64 max_w=0.0, any clip_rect=0) any { _draw_text_ui_ex_clipped(_font_title_id(), txt, x, y, color, 0.0, max_w, clip_rect) }

@inline
@jit
fn _rect_hit(f64 px, f64 py, f64 x, f64 y, f64 w, f64 h) bool { float(px) >= float(x) && float(py) >= float(y) && float(px) < float(x + w) && float(py) < float(y + h) }

@inline
@jit
fn _rect_intersects(list a, list b) bool {
   def ax, ay = float(a.get(0, 0.0)), float(a.get(1, 0.0))
   def aw, ah = float(a.get(2, 0.0)), float(a.get(3, 0.0))
   def bx, by = float(b.get(0, 0.0)), float(b.get(1, 0.0))
   def bw, bh = float(b.get(2, 0.0)), float(b.get(3, 0.0))
   !(ax + aw <= bx || bx + bw <= ax || ay + ah <= by || by + bh <= ay)
}

fn _fit_window_size(dict st) dict {
   mut w, h = max(float(st.get("w", _min_win_w())), _min_win_w()), max(float(st.get("h", _min_win_h())), _min_win_h())
   if(_fb_w > _min_win_w()){ w = min(w, max(_min_win_w(), _fb_w)) }
   if(_fb_h > _min_win_h()){ h = min(h, max(_min_win_h(), _fb_h)) }
   st["w"] = w
   st["h"] = h
   st
}

fn _clamp_window_pos(dict st) dict {
   st = _fit_window_size(st)
   mut x, y = float(st.get("x", 0.0)), float(st.get("y", 0.0))
   def w, h = max(float(st.get("w", _min_win_w())), _min_win_w()), max(float(st.get("h", _min_win_h())), _min_win_h())
   def max_x, max_y = (_fb_w > 0.0) ? max(0.0, _fb_w - w) : x, (_fb_h > 0.0) ? max(0.0, _fb_h - h) : y
   if(x < 0.0){ x = 0.0 } elif(x > max_x){ x = max_x }
   if(y < 0.0){ y = 0.0 } elif(y > max_y){ y = max_y }
   st["x"] = x
   st["y"] = y
   st
}

fn _window_store(dict st) any { _windows[to_str(st.get("id", ""))] = st }

fn _window_lookup(any id) any { _windows.get(to_str(id), 0) }

fn _current_window_state() any {
   if(is_dict(_current_window)){ return _current_window }
   if(_current_window_id_cache.len <= 0){ return 0 }
   _window_lookup(_current_window_id_cache)
}

fn _window_body_clip(dict st) list {
   def title_h = st.get("titlebar", true) ? _title_h() : 0.0
   [
      float(st.get("x", 0.0)) + _pad(),
      float(st.get("y", 0.0)) + title_h,
      max(1.0, float(st.get("w", _min_win_w())) - _pad() * 2.0),
      max(1.0, float(st.get("h", _min_win_h())) - title_h)
   ]
}

fn _window_active_clip(dict st) list {
   def override = st.get("clip_override", 0)
   if(is_list(override) && override.len >= 4){ return override }
   _window_body_clip(st)
}

fn _window_content_origin(dict st) list {
   def title_h = st.get("titlebar", true) ? _title_h() : 0.0
   [
      float(st.get("x", 0.0)) + _pad(),
      float(st.get("y", 0.0)) + title_h + _pad() - float(st.get("scroll_y", 0.0))
   ]
}

fn _list_find_text(list items, any value) int {
   def want = to_str(value)
   mut i = 0
   def items_n = items.len
   while(i < items_n){
      if(to_str(items.get(i, "")) == want){ return i }
      i += 1
   }
   -1
}

fn _full_id_window_id(any full_id) str {
   def key = to_str(full_id)
   def sep = str.find(key, "::")
   if(sep > 0){ return str.str_slice(key, 0, sep) }
   ""
}

fn _window_order_touch(any id, bool to_front=false) any {
   def key = to_str(id)
   if(key.len == 0){ return 0 }
   if(!is_list(_window_order)){ _window_order = [] }
   def found = _list_find_text(_window_order, key) >= 0
   if(found && !to_front){ return 0 }
   mut out = []
   mut i = 0
   def order_n = _window_order.len
   while(i < order_n){
      def cur = to_str(_window_order.get(i, ""))
      if(cur != key){ out = out.append(cur) }
      i += 1
   }
   _window_order = out.append(key)
}

fn _top_window_id_at(f64 px, f64 py) str {
   if(_popup_hit_active){
      if(_rect_hit(px, py, _popup_hit_x, _popup_hit_y, _popup_hit_w, _popup_hit_h)){
         return _popup_hit_window
      }
   }
   mut i = _window_order.len - 1
   while(i >= 0){
      def wid = to_str(_window_order.get(i, ""))
      def st = _windows.get(wid, 0)
      if(is_dict(st) && st.get("open", true)){
         if(_rect_hit(px, py,
               float(st.get("x", 0.0)),
               float(st.get("y", 0.0)),
               float(st.get("w", 0.0)),
            float(st.get("h", 0.0)))){
            return wid
         }
      }
      i -= 1
   }
   if(_window_order.len > 0){ return "" }
   mut ks = dict_keys(_windows)
   if(!is_list(ks)){ return "" }
   i = 0
   def ks_n = ks.len
   while(i < ks_n){
      def st = _windows.get(ks.get(i), 0)
      if(is_dict(st) && st.get("open", true)){
         if(_rect_hit(px, py,
               float(st.get("x", 0.0)),
               float(st.get("y", 0.0)),
               float(st.get("w", 0.0)),
            float(st.get("h", 0.0)))){
            return to_str(st.get("id", ""))
         }
      }
      i += 1
   }
   ""
}

fn _window_focusables(any window_id) list {
   def st = _window_lookup(window_id)
   mut ids = []
   if(is_dict(st)){ ids = st.get("focusables", []) }
   is_list(ids) ? ids : []
}

fn _pointer_window_id() str {
   if(_active_window_move != ""){ return _active_window_move }
   if(_active_window_resize != ""){ return _active_window_resize }
   if(_mouse_down0 && _active_widget_id != ""){
      def wid = _full_id_window_id(_active_widget_id)
      if(wid.len > 0){ return wid }
   }
   _window_hovered
}

fn _window_accepts_pointer(any window_id) bool {
   def wid = to_str(window_id)
   if(wid.len == 0){ return false }
   _pointer_window_id() == wid
}

fn _window_pointer_hit(any window_id, f64 x, f64 y, f64 w, f64 h) bool { _window_accepts_pointer(window_id) && _rect_hit(_mouse_x, _mouse_y, x, y, w, h) }

fn _current_window_pointer_hit(f64 x, f64 y, f64 w, f64 h) bool {
   if(_current_window_id_cache.len <= 0){ return false }
   _window_pointer_hit(_current_window_id_cache, x, y, w, h)
}

fn _pointer_over_scroll_area(any window_id) bool {
   to_str(window_id) == _scroll_area_capture_window &&
   _rect_hit(_mouse_x, _mouse_y,
      float(_scroll_area_capture_rect.get(0, 0.0)),
      float(_scroll_area_capture_rect.get(1, 0.0)),
      float(_scroll_area_capture_rect.get(2, 0.0)),
   float(_scroll_area_capture_rect.get(3, 0.0)))
}

fn _request_focus_step(int dir) any {
   def win_id = to_str(_focused_window)
   if(win_id.len == 0){ return 0 }
   def ids = _window_focusables(win_id)
   def n = ids.len
   if(n <= 0){ return 0 }
   mut cur = _text_focus_id
   if(cur.len == 0){ cur = _kbd_focus_id }
   if(cur.len == 0){ cur = _active_widget_id }
   mut idx = _list_find_text(ids, cur)
   if(dir >= 0){ idx = (idx < 0) ? 0 : (((idx + 1) >= n) ? 0 : (idx + 1)) } else { idx = (idx < 0) ? (n - 1) : (((idx - 1) < 0) ? (n - 1) : (idx - 1)) }
   _nav_focus_target = to_str(ids.get(idx, ""))
   _nav_focus_window = win_id
   _kbd_focus_id = _nav_focus_target
   _text_focus_id = ""
   _active_widget_id = ""
}

fn _focus_collect_for_window(any window_id) bool {
   def wid = to_str(window_id)
   if(wid.len == 0){ return false }
   _focused_window == wid ||
   _nav_focus_window == wid ||
   _full_id_window_id(_kbd_focus_id) == wid ||
   _full_id_window_id(_text_focus_id) == wid ||
   _full_id_window_id(_active_widget_id) == wid
}

fn hit_test(f64 x, f64 y) bool {
   "Returns true when a visible GUI window covers point `(x, y)`."
   if(!_enabled){ return false }
   _top_window_id_at(x, y).len > 0
}

fn _apply_event_pointer_pos() any {
   if(!_event_mouse_has_pos){ return 0 }
   _mouse_x, _mouse_y = _event_mouse_x, _event_mouse_y
   _window_hovered = _top_window_id_at(_mouse_x, _mouse_y)
}

fn _pointer_view_xy(any x, any y) list {
   if(!_win){ return [float(x), float(y)] }
   def sz = uiw.size(_win)
   def ww = max(1.0, float(sz.get(0, _fb_w > 0.0 ? _fb_w : 1.0)))
   def wh = max(1.0, float(sz.get(1, _fb_h > 0.0 ? _fb_h : 1.0)))
   def vw = max(1.0, _fb_w > 0.0 ? _fb_w : ww)
   def vh = max(1.0, _fb_h > 0.0 ? _fb_h : wh)
   [
      clamp(float(x) * vw / ww, 0.0, vw),
      clamp(float(y) * vh / wh, 0.0, vh)
   ]
}

fn _capture_event_pointer_pos(dict data) any {
   if(data.contains("x") || data.contains("y")){
      _event_mouse_has_pos = true
      def p = _pointer_view_xy(data.get("x", _event_mouse_x), data.get("y", _event_mouse_y))
      _event_mouse_x, _event_mouse_y = float(p.get(0, _event_mouse_x)), float(p.get(1, _event_mouse_y))
   } elif(_win){
      def mp = uiw.cursor_pos(_win)
      def p = _pointer_view_xy(mp.get(0, _mouse_x), mp.get(1, _mouse_y))
      _event_mouse_has_pos = true
      _event_mouse_x, _event_mouse_y = float(p.get(0, _mouse_x)), float(p.get(1, _mouse_y))
   }
   _apply_event_pointer_pos()
}

fn _event_pointer_claimed() bool { _pointer_window_id() != "" || _active_widget_id != "" || _active_window_move != "" || _active_window_resize != "" }

fn _queue_key_event(int key) any {
   if(!is_list(_key_events)){ _key_events = [] }
   _key_events = _key_events.append(key)
}

fn prepare_input(any win, f64 ww=0.0, f64 wh=0.0) any {
   "Prepares per-frame input state for the GUI."
   _theme_refresh()
   _win = win
   if(float(ww) > 0.0){ _fb_w = float(ww) }
   if(float(wh) > 0.0){ _fb_h = float(wh) }
   if(!_enabled || !win){
      _window_hovered = ""
      _wants_mouse, _wants_keyboard = false, false
      _mouse_down0, _mouse_down0_prev = false, false
      _mouse_pressed0, _mouse_released0 = false, false
      _clear_pending_input_events()
      return 0
   }
   def mp = uiw.cursor_pos(win)
   def vp = _pointer_view_xy(mp.get(0, 0.0), mp.get(1, 0.0))
   _mouse_x, _mouse_y = float(vp.get(0, 0.0)), float(vp.get(1, 0.0))
   if(_event_mouse_has_pos){ _mouse_x, _mouse_y = _event_mouse_x, _event_mouse_y }
   def sampled_down0 = _event_mouse_down0_known ? _event_mouse_down0 : uiw.mouse_down(win, 0)
   def sampled_pressed0 = (_event_mouse_pressed0 || (sampled_down0 && !_mouse_down0_prev)) && sampled_down0
   def sampled_released0 = _event_mouse_released0 || (_mouse_down0_prev && !sampled_down0)
   _mouse_down0, _mouse_pressed0 = sampled_down0, sampled_pressed0
   _mouse_released0 = sampled_released0 && (_active_widget_id != "" || _active_window_move != "" || _active_window_resize != "")
   mut suppress_mouse = false
   if(_suppress_mouse_frames > 0){
      suppress_mouse = true
      _suppress_mouse_frames -= 1
   }
   if(_suppress_mouse_until_up){
      suppress_mouse = true
      if(!_mouse_down0){ _suppress_mouse_until_up = false }
   }
   if(suppress_mouse){
      _mouse_pressed0, _mouse_released0 = false, false
      _event_mouse_pressed0, _event_mouse_released0 = false, false
      _event_mouse_down0_known = false
      _mouse_down0, _mouse_down0_prev = false, false
      _active_widget_id, _active_window_move, _active_window_resize = "", "", ""
      _active_rect = [0.0, 0.0, 0.0, 0.0]
   }
   _mouse_down0_prev = _mouse_down0
   if(_mouse_pressed0){
      _mouse_press_x, _mouse_press_y = _mouse_x, _mouse_y
      _mouse_press_seq += 1
   }
   def sp, sx = uiw.scroll_pos(win), float(sp.get(0, 0.0))
   def sy = float(sp.get(1, 0.0))
   if(abs(_event_scroll_dx) > 0.000001 || abs(_event_scroll_dy) > 0.000001){
      _mouse_scroll_dx, _mouse_scroll_dy = _event_scroll_dx, _event_scroll_dy
   } else {
      _mouse_scroll_dx, _mouse_scroll_dy = sx - _last_scroll_x, sy - _last_scroll_y
   }
   _last_scroll_x, _last_scroll_y = sx, sy
   _window_hovered = _top_window_id_at(_mouse_x, _mouse_y)
   if(_mouse_pressed0 && _window_hovered.len == 0 && _active_window_move == "" && _active_window_resize == ""){
      _focused_window, _text_focus_id, _kbd_focus_id, _active_widget_id = "", "", "", ""
   }
   _wants_mouse = _pointer_window_id() != "" || _active_widget_id != "" || _active_window_move != "" || _active_window_resize != ""
   _wants_keyboard = _text_focus_id != "" || _kbd_focus_id != "" || _focused_window != "" || _active_widget_id != ""
   _cursor_want = 0
   _clear_pending_input_events()
}

fn feed_event(int typ, dict data) bool {
   "Feeds a window/input event into the GUI and returns whether it was claimed."
   if(!_enabled){ return false }
   if(typ == EVENT_MOUSE_POS_CHANGED){
      _capture_event_pointer_pos(data)
      return _event_pointer_claimed()
   }
   if(typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED){
      def b = int(data.get("button", -1))
      _capture_event_pointer_pos(data)
      if(b == 0){
         _event_mouse_down0_known = true
         _event_mouse_down0 = typ == EVENT_MOUSE_BUTTON_PRESSED
         if(typ == EVENT_MOUSE_BUTTON_PRESSED){ _event_mouse_pressed0 = true }
         else { _event_mouse_released0 = true }
      }
      return _event_pointer_claimed()
   }
   if(typ == EVENT_MOUSE_SCROLL){
      _capture_event_pointer_pos(data)
      _event_scroll_dx += float(data.get("dx", 0.0))
      _event_scroll_dy += float(data.get("dy", 0.0))
      return _pointer_window_id() != ""
   }
   if(typ == EVENT_KEY_CHAR || typ == EVENT_KEY_CHAR_MODS){
      def cp = int(data.get("char", data.get("codepoint", 0)))
      if(cp > 31 && cp != 127){
         _text_events = _text_events + str.chr(cp)
         return _text_focus_id != ""
      }
      return false
   }
   if(typ == EVENT_KEY_PRESSED){
      def mods = int(data.get("mod", 0))
      def k = int(data.get("key", 0))
      def text_active = _text_focus_id != ""
      def ctrl = (mods & MOD_CONTROL) != 0
      if(text_active && (mods & (MOD_ALT | MOD_SUPER | MOD_META)) == 0){
         if(ctrl){
            if(k == KEY_A){ _queue_key_event(_TEXT_SELECT_ALL) }
            elif(k == KEY_C){ _queue_key_event(_TEXT_COPY) }
            elif(k == KEY_X){ _queue_key_event(_TEXT_CUT) }
            elif(k == KEY_V){ _queue_key_event(_TEXT_PASTE) }
            elif(k == KEY_BACKSPACE || k == KEY_DELETE){ _queue_key_event(_TEXT_SELECT_ALL) _queue_key_event(KEY_DELETE) }
            return true
         }
         if(k == KEY_ESCAPE){
            _text_focus_id = ""
            _kbd_focus_id = ""
            _active_widget_id = ""
            return true
         }
         if(k == KEY_BACKSPACE
            || k == KEY_DELETE
            || k == KEY_LEFT
            || k == KEY_RIGHT
            || k == KEY_UP
            || k == KEY_DOWN
            || k == KEY_HOME
            || k == KEY_END
            || k == KEY_PAGE_UP
            || k == KEY_PAGE_DOWN
            || k == KEY_ENTER
            || k == KEY_SPACE){
            _queue_key_event(k)
            return true
         }
      }
      if((mods & (MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META)) != 0){ return false }
      if(k == KEY_ESCAPE){
         def had_focus = _text_focus_id != "" || _kbd_focus_id != "" || _focused_window != ""
         _text_focus_id = ""
         _kbd_focus_id = ""
         _active_widget_id = ""
         _focused_window = ""
         return had_focus
      }
      if(k == KEY_TAB){
         _request_focus_step(((mods & MOD_SHIFT) != 0) ? -1 : 1)
         _queue_key_event(k)
         return _focused_window != "" || _text_focus_id != ""
      }
      if(k == KEY_BACKSPACE
         || k == KEY_DELETE
         || k == KEY_LEFT
         || k == KEY_RIGHT
         || k == KEY_UP
         || k == KEY_DOWN
         || k == KEY_HOME
         || k == KEY_END
         || k == KEY_PAGE_UP
         || k == KEY_PAGE_DOWN
         || k == KEY_ENTER
         || k == KEY_SPACE){
         _queue_key_event(k)
         return _text_focus_id != "" || _kbd_focus_id != "" || _focused_window != ""
      }
   }
   false
}

fn key_pressed(any key) bool {
   "Returns true when the GUI has a queued key press for this frame."
   _enabled && _key_event_present(int(key))
}

fn _utf8_prev_index(str s, int idx) int {
   mut i = int(idx)
   def n = str.len(s)
   if(i > n){ i = n }
   if(i <= 0){ return 0 }
   i -= 1
   while(i > 0){
      def b = load8(s, i)
      if((b & 0xC0) != 0x80){ break }
      i -= 1
   }
   i
}

fn _utf8_next_index(str s, int idx) int {
   mut i = int(idx)
   def n = str.len(s)
   if(i < 0){ i = 0 }
   if(i >= n){ return n }
   i += 1
   while(i < n){
      def b = load8(s, i)
      if((b & 0xC0) != 0x80){ break }
      i += 1
   }
   i
}

fn _text_cursor_from_x(int font_id, any s, f64 px, f64 text_x) int {
   def txt = _ui_text(s)
   def target = float(px) - float(text_x)
   if(target <= 0.0){ return 0 }
   def n = str.len(txt)
   mut i = 0
   while(i < n){
      def next_i = _utf8_next_index(txt, i)
      def before = str.str_slice(txt, 0, i)
      def thru = str.str_slice(txt, 0, next_i)
      def bw = float(_measure_text_ui(font_id, before).get(0, 0.0))
      def nw = float(_measure_text_ui(font_id, thru).get(0, 0.0))
      if(target < (bw + nw) * 0.5){ return i }
      i = next_i
   }
   n
}

fn _text_state_cursor(any full_id, int fallback_len) int {
   def st = _widget_state.get(full_id, 0)
   mut cur = is_dict(st) ? int(st.get("cursor", fallback_len)) : int(fallback_len)
   def n = int(fallback_len)
   if(cur < 0){ cur = 0 } elif(cur > n){ cur = n }
   cur
}

fn _text_state_set_cursor(any full_id, int cursor_idx) any {
   mut st = _widget_state.get(full_id, 0)
   if(!is_dict(st)){ st = dict(4) }
   st["cursor"] = int(cursor_idx)
   _widget_state[full_id] = st
}

fn begin_frame(any win=0, int font=0, f64 ww=0.0, f64 wh=0.0) any {
   "Starts a GUI frame and resets per-frame hover/layout state."
   if(win){ prepare_input(win, ww, wh) }
   else { _theme_refresh() }
   if(font){
      _font = font
      if(!_font_title){ _font_title = _font }
      if(!_font_small){ _font_small = _font }
   }
   _hot_id, _hot_rect = "", [0.0, 0.0, 0.0, 0.0]
   _same_line_next, _same_line_gap = false, _gap()
   _text_run_queue = []
   _popup_next_hit_active = false
   _popup_next_hit_window = ""
   _popup_combo_active = false
   _popup_combo_labels = []
   gui_batch.reset()
   _sync_current_layout_cache(0)
   _focus_collect_active = false
}

fn _flush_popup_draw_queue() any {
   if(_popup_combo_active){
      _combo_popup_draw_flat(
         _popup_combo_key,
         _popup_combo_x,
         _popup_combo_y,
         _popup_combo_w,
         _popup_combo_h,
         _popup_combo_row_h,
         _popup_combo_rows,
         _popup_combo_max_scroll,
         _popup_combo_scroll_y,
         _popup_combo_out,
         _popup_combo_first,
         _popup_combo_labels
      )
   }
}

fn end_frame() any {
   "Finishes a GUI frame, draws debug overlays, and clears transient input."
   if(!_enabled){ return 0 }
   def prof = ui_profile.enabled()
   def t_flush = prof ? ui_profile.now() : 0
   def run_count = (prof && is_list(_text_run_queue)) ? _text_run_queue.len : 0
   _flush_popup_draw_queue()
   _flush_text_runs()
   if(prof){ ui_profile.mark("flush_text", t_flush, " runs=" + to_str(run_count)) }
   def t_tail = prof ? ui_profile.now() : 0
   if(!_mouse_down0){
      _active_window_move, _active_window_resize, _active_widget_id = "", "", ""
   }
   if(_debug_overlay){
      if(_hot_rect.len >= 4 && (_hot_id != "")){
         _gui_rect_outline_fast(float(_hot_rect.get(0, 0.0)), float(_hot_rect.get(1, 0.0)),
         float(_hot_rect.get(2, 0.0)), float(_hot_rect.get(3, 0.0)), _accent_u32)
      }
      if(_active_rect.len >= 4 && (_active_widget_id != "")){
         _gui_rect_outline_fast(float(_active_rect.get(0, 0.0)), float(_active_rect.get(1, 0.0)),
         float(_active_rect.get(2, 0.0)), float(_active_rect.get(3, 0.0)), _danger_u32)
      }
      if(_window_hovered != ""){
         def st = _window_lookup(_window_hovered)
         if(is_dict(st)){
            _gui_rect_outline_fast(float(st.get("x", 0.0)), float(st.get("y", 0.0)),
            float(st.get("w", 0.0)), float(st.get("h", 0.0)), _ok_u32)
         }
      }
      _gui_rect_fast(_mouse_x - 6.0, _mouse_y, 13.0, 1.0, _warn_u32)
      _gui_rect_fast(_mouse_x, _mouse_y - 6.0, 1.0, 13.0, _warn_u32)
   }
   _flush_rect_batch()
   _apply_cursor()
   if(prof){ ui_profile.mark("end_frame_tail", t_tail) }
   _popup_hit_active = _popup_next_hit_active
   _popup_hit_window = _popup_next_hit_window
   _popup_hit_x, _popup_hit_y = _popup_next_hit_x, _popup_next_hit_y
   _popup_hit_w, _popup_hit_h = _popup_next_hit_w, _popup_next_hit_h
   _key_events = []
   _text_events = ""
}

fn wants_mouse() bool { "Returns true when GUI wants mouse input." _enabled && _wants_mouse }

fn wants_keyboard() bool { "Returns true when GUI wants keyboard input." _enabled && _wants_keyboard }

fn hovered_id() str { "Returns the currently hovered widget id." _hot_id }

fn active_id() str { "Returns the currently active widget id." _active_widget_id }

fn focused_id() str {
   "Returns the active text or keyboard focus id."
   if(_text_focus_id != ""){ return _text_focus_id }
   _kbd_focus_id
}

fn mouse_down() bool { "Returns whether GUI sees the primary mouse button held." _mouse_down0 }

fn mouse_press_seq() int { "Returns the GUI mouse press sequence number." _mouse_press_seq }

fn last_item_rect() list { "Returns the last submitted item rectangle." _last_item }

fn clear_focus() any {
   "Clears active widget, keyboard, and text focus."
   _active_widget_id, _kbd_focus_id, _text_focus_id = "", "", ""
}

fn focus_window(any id) bool {
   "Moves a window id to focused/front state."
   def key = to_str(id)
   if(key.len == 0){ return false }
   _focused_window = key
   _window_order_touch(key, true)
   true
}

fn request_focus(any full_id) bool {
   "Requests keyboard/text focus for a full widget id."
   def fid = to_str(full_id)
   if(fid.len == 0){ return false }
   _nav_focus_target, _kbd_focus_id, _text_focus_id = fid, fid, fid
   _active_widget_id = ""
   def sep = str.find(fid, "::")
   if(sep > 0){
      def wid = str.str_slice(fid, 0, sep)
      _nav_focus_window, _focused_window = wid, wid
      _window_order_touch(wid, true)
   }
   true
}

fn show_window(any id, bool visible=true) any {
   "Shows or hides a named GUI window."
   def key = to_str(id)
   mut st = _window_lookup(key)
   if(!is_dict(st)){
      st = dict(8)
      st["id"] = key
   }
   st["open"] = !!visible
   _window_store(st)
   if(!visible){
      if(_focused_window == key){ _focused_window = "" }
      if(_window_hovered == key){ _window_hovered = "" }
      if(_active_window_move == key){ _active_window_move = "" }
      if(_active_window_resize == key){ _active_window_resize = "" }
      if(_nav_focus_window == key){
         _nav_focus_window, _nav_focus_target = "", ""
      }
      if(_full_id_window_id(_kbd_focus_id) == key){ _kbd_focus_id = "" }
      if(_full_id_window_id(_text_focus_id) == key){ _text_focus_id = "" }
      if(_full_id_window_id(_active_widget_id) == key){ _active_widget_id = "" }
   }
}

fn window_visible(any id) bool {
   "Returns true when a named GUI window is open."
   def st = _window_lookup(id)
   is_dict(st) && st.get("open", true)
}

fn window_closed(any id) bool {
   "Consumes and returns the close event flag for a named GUI window."
   def key = to_str(id)
   if(key.len == 0){ return false }
   def hit = bool(_window_closed_events.get(key, false))
   if(hit){ _window_closed_events[key] = false }
   hit
}

fn reset_window_scroll(any id) bool {
   "Resets stored scroll state for a named GUI window."
   def key = to_str(id)
   if(key.len == 0){ return false }
   mut st = _window_lookup(key)
   if(!is_dict(st)){ return false }
   st["scroll_y"] = 0.0
   st["content_h"] = 0.0
   _window_store(st)
   true
}

fn set_window_pos(any id, f64 x, f64 y) any {
   "Sets stored position for a named GUI window."
   mut st = _window_lookup(id)
   if(!is_dict(st)){ return 0 }
   st["x"] = float(x)
   st["y"] = float(y)
   st = _clamp_window_pos(st)
   _window_store(st)
}

fn set_window_size(any id, f64 w, f64 h) any {
   "Sets stored size for a named GUI window."
   mut st = _window_lookup(id)
   if(!is_dict(st)){ return 0 }
   st["w"] = max(float(w), _min_win_w())
   st["h"] = max(float(h), _min_win_h())
   st = _clamp_window_pos(st)
   _window_store(st)
}

fn window_rect(any id) list {
   "Returns the stored rectangle for a named GUI window."
   def st = _window_lookup(id)
   if(!is_dict(st)){ return [0.0, 0.0, 0.0, 0.0] }
   [
      float(st.get("x", 0.0)),
      float(st.get("y", 0.0)),
      float(st.get("w", 0.0)),
      float(st.get("h", 0.0))
   ]
}

fn _ensure_window(any id, any title, f64 x, f64 y, f64 w, f64 h, any opts=0) dict {
   def key = to_str(id)
   mut st = _windows.get(key, 0)
   if(!is_dict(st)){
      st = dict(32)
      st["id"] = key
   }
   if(!st.contains("id")){ st["id"] = key }
   if(!st.contains("title")){ st["title"] = to_str(title) }
   if(!st.contains("x")){ st["x"] = _snap(x) }
   if(!st.contains("y")){ st["y"] = _snap(y) }
   if(!st.contains("w") || float(st.get("w", 0.0)) <= 0.0){ st["w"] = max(_snap(w), _min_win_w()) }
   if(!st.contains("h") || float(st.get("h", 0.0)) <= 0.0){ st["h"] = max(_snap(h), _min_win_h()) }
   def opts_is_dict = is_dict(opts)
   mut opt_open = true
   mut opt_collapsed = false
   mut opt_closable = true
   mut opt_movable = true
   mut opt_resizable = true
   mut opt_scrollable = true
   mut opt_collapsible = true
   mut opt_titlebar = true
   if(opts_is_dict){
      opt_open, opt_collapsed = opts.get("open", true), opts.get("collapsed", false)
      opt_closable, opt_movable = opts.get("closable", true), opts.get("movable", true)
      opt_resizable, opt_scrollable = opts.get("resizable", true), opts.get("scrollable", true)
      opt_collapsible, opt_titlebar = opts.get("collapsible", true), opts.get("titlebar", true)
   }
   if(!st.contains("open")){ st["open"] = opt_open }
   if(!st.contains("collapsed")){ st["collapsed"] = opt_collapsed }
   if(!st.contains("closable")){ st["closable"] = opt_closable }
   if(!st.contains("movable")){ st["movable"] = opt_movable }
   if(!st.contains("resizable")){ st["resizable"] = opt_resizable }
   if(!st.contains("scrollable")){ st["scrollable"] = opt_scrollable }
   if(!st.contains("collapsible")){ st["collapsible"] = opt_collapsible }
   if(!st.contains("titlebar")){ st["titlebar"] = opt_titlebar }
   if(!st.contains("scroll_y")){ st["scroll_y"] = 0.0 }
   if(!st.contains("content_h")){ st["content_h"] = 0.0 }
   st["title"] = to_str(title)
   def old_closable = st.get("closable", true)
   def old_movable = st.get("movable", true)
   def old_resizable = st.get("resizable", true)
   def old_scrollable = st.get("scrollable", true)
   def old_collapsible = st.get("collapsible", true)
   def old_titlebar = st.get("titlebar", true)
   mut next_closable = old_closable
   mut next_movable = old_movable
   mut next_resizable = old_resizable
   mut next_scrollable = old_scrollable
   mut next_collapsible = old_collapsible
   mut next_titlebar = old_titlebar
   if(opts_is_dict){
      next_closable, next_movable = opts.get("closable", old_closable), opts.get("movable", old_movable)
      next_resizable, next_scrollable = opts.get("resizable", old_resizable), opts.get("scrollable", old_scrollable)
      next_collapsible, next_titlebar = opts.get("collapsible", old_collapsible), opts.get("titlebar", old_titlebar)
   }
   st["closable"] = next_closable
   st["movable"] = next_movable
   st["resizable"] = next_resizable
   st["scrollable"] = next_scrollable
   st["collapsible"] = next_collapsible
   st["titlebar"] = next_titlebar
   st = _clamp_window_pos(st)
   _window_store(st)
   _window_order_touch(key, false)
   st
}

fn _register_focusable(any full_id) bool {
   mut win = _current_window_state()
   if(!is_dict(win)){ return false }
   def fid = to_str(full_id)
   if(fid.len == 0){ return false }
   if(!_focus_collect_active){ return false }
   mut ids = win.get("focusables", [])
   if(!is_list(ids)){ ids = [] }
   mut seen = win.get("focusable_map", 0)
   if(!is_dict(seen)){
      seen = dict(64)
      mut i = 0
      def ids_n = ids.len
      while(i < ids_n){
         def existing = to_str(ids.get(i, ""))
         if(existing.len > 0){ seen[existing] = true }
         i += 1
      }
   }
   if(!seen.contains(fid)){
      if(!is_list(ids)){ ids = [] }
      ids = ids.append(fid)
      seen[fid] = true
      win["focusables"] = ids
      win["focusable_map"] = seen
      _window_store(win)
      _current_window = win
   }
   if(_nav_focus_target == fid && (_nav_focus_window == "" || _nav_focus_window == _current_window_id_cache)){
      _kbd_focus_id = fid
      _focused_window = _current_window_id_cache
      _nav_focus_target = ""
      _nav_focus_window = ""
      return true
   }
   false
}

fn _key_event_present(int key) bool {
   if(!is_list(_key_events)){ return false }
   mut i = 0
   def events_n = _key_events.len
   while(i < events_n){
      if(int(_key_events.get(i, 0)) == int(key)){ return true }
      i += 1
   }
   false
}

fn _widget_key_activate(any full_id) bool { (_kbd_focus_id == full_id) && (_key_event_present(KEY_ENTER) || _key_event_present(KEY_SPACE)) }

fn _text_selected_all(any full_id) bool { _state_get_num(full_id, "select_all", 0.0) > 0.5 }

fn _text_set_selected_all(any full_id, bool selected) any { _state_set_num(full_id, "select_all", selected ? 1.0 : 0.0) }

fn _begin_interact_widget(any full_id, f64 x, f64 y, f64 w, f64 h, bool visible=true) list {
   def rect = [x, y, w, h]
   mut clip = rect
   def win = _current_window_state()
   if(is_dict(win)){ clip = _window_active_clip(win) }
   def draw_ok = !!visible && _rect_intersects(rect, clip)
   def hovered = draw_ok && _rect_hit(_mouse_x, _mouse_y,
      float(clip.get(0, 0.0)), float(clip.get(1, 0.0)),
   float(clip.get(2, 0.0)), float(clip.get(3, 0.0))) && _current_window_pointer_hit(x, y, w, h)
   if(hovered){
      _hot_id = full_id
      _hot_rect = rect
      _ensure_cursors()
      _want_cursor(_cursor_hand)
   }
   if(hovered && _mouse_pressed0){
      if(_text_focus_id != "" && _text_focus_id != full_id){ _text_focus_id = "" }
      _active_widget_id = full_id
      _kbd_focus_id = full_id
      _active_rect = rect
      if(_current_window_id_cache.len > 0){ _focused_window = _current_window_id_cache }
   }
   def held = (_active_widget_id == full_id) && _mouse_down0
   def released = (_active_widget_id == full_id) && _mouse_released0
   def clicked = released && hovered
   [draw_ok, hovered, held, clicked, released]
}

fn _sync_current_layout_cache(any st) any {
   if(!is_dict(st)){
      _current_window_id_cache = ""
      _current_body_x_cache = 0.0
      _current_body_y_cache = 0.0
      _current_body_w_cache = 0.0
      _current_body_h_cache = 0.0
      _current_cursor_y_cache = 0.0
      return 0
   }
   _current_window_id_cache = to_str(st.get("id", ""))
   _current_body_x_cache = float(st.get("body_x", 0.0))
   _current_body_y_cache = float(st.get("body_y", 0.0))
   _current_body_w_cache = max(1.0, float(st.get("body_w", _sx(100.0))))
   _current_body_h_cache = max(1.0, float(st.get("body_h", 1.0)))
   _current_cursor_y_cache = float(st.get("cursor_y", _current_body_y_cache))
}

@inline
@jit
fn _state_get_bool(any full_id, bool fallback=false) bool {
   def st = _widget_state.get(full_id, 0)
   if(is_dict(st)){ return st.get("value", fallback) }
   fallback
}

@inline
@jit
fn _state_set_bool(any full_id, bool v) any {
   mut st = _widget_state.get(full_id, 0)
   if(!is_dict(st)){ st = dict(4) }
   st["value"] = !!v
   _widget_state[full_id] = st
}

fn _debug_forced_combo_id() str {
   if(_debug_force_combo_open == "\x00"){ _debug_force_combo_open = ui_profile.env_trim_cached("NY_UI_GUI_FORCE_COMBO_OPEN") }
   _debug_force_combo_open
}

fn debug_force_combo_open(any id) bool {
   "Forces a combo box open for debug capture and visual probes."
   _debug_force_combo_open = to_str(id)
   true
}

@inline
@jit
fn _debug_combo_forced(any full_id, any short_id) bool {
   def want = _debug_forced_combo_id()
   want.len > 0 && (want == to_str(short_id) || want == to_str(full_id) || str.find(to_str(full_id), want) >= 0)
}

@inline
@jit
fn _state_get_num(any full_id, any key, f64 fallback=0.0) f64 {
   def st = _widget_state.get(full_id, 0)
   is_dict(st) ? float(st.get(key, fallback)) : float(fallback)
}

@inline
@jit
fn _state_set_num(any full_id, any key, f64 v) any {
   mut st = _widget_state.get(full_id, 0)
   if(!is_dict(st)){ st = dict(8) }
   st[key] = float(v)
   _widget_state[full_id] = st
}

@inline
@jit
fn _current_body_w() f64 {
   if(_current_window_id_cache.len <= 0){ return _sx(100.0) }
   max(1.0, _current_body_w_cache)
}

fn remaining_h(any reserve=0.0) f64 {
   "Returns remaining vertical room in the current window body."
   max(0.0,
      (_current_body_y_cache + _current_body_h_cache) -
   _current_cursor_y_cache - _gap() - float(reserve))
}

fn _current_clip() list {
   def win = _current_window_state()
   if(!is_dict(win)){ return [0.0, 0.0, _fb_w, _fb_h] }
   _window_active_clip(win)
}

fn push_clip_rect(any x, any y=0.0, any w=0.0, any h=0.0) any {
   "Pushes a scissor clip rectangle for subsequent GUI drawing."
   _flush_text_runs()
   if(is_list(x)){
      scissor_push(
         float(x.get(0, 0.0)),
         float(x.get(1, 0.0)),
         float(x.get(2, 0.0)),
         float(x.get(3, 0.0))
      )
      return 0
   }
   scissor_push(float(x), float(y), float(w), float(h))
}

fn pop_clip_rect() any {
   "Pops the current GUI scissor clip rectangle."
   _flush_text_runs()
   scissor_pop()
}

@jit
fn _layout_rect(any w, any h) list {
   if(_current_window_id_cache.len <= 0){ return [0.0, 0.0, 0.0, 0.0] }
   mut x, y = _current_body_x_cache, _current_cursor_y_cache
   def desired_w, desired_h = max(1.0, float(w)), max(1.0, float(h))
   def body_x, body_w = _current_body_x_cache, max(1.0, _current_body_w_cache)
   def body_right = body_x + body_w
   if(_same_line_next && _last_item.len >= 4){
      x, y = float(_last_item.get(0, x)) + float(_last_item.get(2, 0.0)) + _same_line_gap, float(_last_item.get(1, y))
      def remaining = body_right - x
      def min_inline = min(desired_w, max(_sx(44.0), body_w * 0.30))
      if(remaining + 1.0 < min_inline){ x, y = body_x, _current_cursor_y_cache }
   }
   if(x < body_x){ x = body_x }
   if(x >= body_right){ x = body_x }
   def avail_w = max(1.0, body_right - x)
   def rw = min(desired_w, avail_w)
   def rh = desired_h
   def rect = [_snap(x), _snap(y), _snap(rw), _snap(rh)]
   _current_cursor_y_cache = max(_current_cursor_y_cache, float(rect.get(1, y)) + float(rect.get(3, rh)) + _gap())
   _same_line_next = false
   _same_line_gap = _gap()
   _last_item = rect
   rect
}

@inline
@jit
fn same_line(any spacing=0.0) any {
   "Places the next widget on the same row when space allows."
   _same_line_next = true
   _same_line_gap = _pick_positive(spacing, _gap())
}

fn spacing(any amount=1.0) any {
   "Adds vertical layout spacing measured in GUI gaps."
   if(_current_window_id_cache.len <= 0){ return 0 }
   _current_cursor_y_cache += float(amount) * _gap()
}

fn spacer_px(any px=0.0) any {
   "Adds vertical layout spacing measured in pixels."
   if(_current_window_id_cache.len <= 0){ return 0 }
   def h = max(0.0, float(px))
   if(h <= 0.0){ return 0 }
   def x, y = _current_body_x_cache, _current_cursor_y_cache
   def w = _current_body_w_cache
   _current_cursor_y_cache = y + h
   _last_item = [x, y, w, h]
   _same_line_next = false
}

fn reset_cursor() bool {
   "Resets the current window layout cursor to the body origin."
   if(_current_window_id_cache.len <= 0){ return false }
   def x, y = _current_body_x_cache, _current_body_y_cache
   def w = _current_body_w_cache
   _current_cursor_y_cache = y
   _last_item = [x, y, w, 0.0]
   _same_line_next = false
   _same_line_gap = _gap()
   true
}

fn layout_gap() f64 { _gap() }

fn separator() any {
   "Draws a one-pixel separator across the current body width."
   def rect = _layout_rect(_current_body_w(), 1.0)
   def clip = _current_clip()
   if(_rect_intersects(rect, clip)){ _gui_rect_fast(float(rect.get(0, 0.0)), float(rect.get(1, 0.0)), float(rect.get(2, 0.0)), 1.0, _border_u32) }
}

fn _text_line(any msg, any color) any {
   def s = _ui_text(msg)
   def rect = _layout_rect(_current_body_w(), _text_h())
   def clip = _current_clip()
   def hit = _rect_intersects(rect, clip)
   if(hit){ _draw_text_ui_clipped(s, float(rect.get(0, 0.0)), float(rect.get(1, 0.0)), color, float(rect.get(2, 0.0)), rect) }
}

fn text(any msg) any { _text_line(msg, _text_u32) }

fn label(any msg) any { text(msg) }

fn text_colored(any msg, any color) any {
   "Draws a single text line with an RGBA color."
   _text_line(msg, [
         clamp(float(color.get(0, 1.0)), 0.0, 1.0),
         clamp(float(color.get(1, 1.0)), 0.0, 1.0),
         clamp(float(color.get(2, 1.0)), 0.0, 1.0),
         clamp(float(color.get(3, 1.0)), 0.0, 1.0)
   ])
}

fn property_row(any id, any label, any value, any tone=0) list {
   "Draws a two-column property row and returns its rectangle."
   def lbl = _ui_text(label)
   def val = _ui_text(value)
   def tone_col = is_list(tone) ? tone : _text_u32
   def row_h = max(_small_item_h(), _sx(24.0))
   def rect = _layout_rect(_current_body_w(), row_h)
   def clip = _current_clip()
   if(!_rect_intersects(rect, clip)){ return rect }
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def w, h = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   def hot = _current_window_pointer_hit(x, y, w, h)
   _gui_rect_fast(x, y, w, h, hot ? _panel_hover_u32 : _panel_u32)
   _gui_rect_fast(x, y, _sx(2.0), h, _border_u32)
   def label_w = clamp(w * 0.38, _sx(96.0), _sx(168.0))
   def tx = x + _item_pad_x()
   def ty = _text_center_y_for(_font_small_id(), y, h)
   _draw_text_ui_small_clipped(lbl, tx, ty, _text_dim_u32, max(1.0, label_w - _item_pad_x()), _clip_rect_inset(x,
         y,
         w,
         h,
   1.0))
   _draw_text_ui_small_clipped(val, x + label_w, ty, tone_col, max(1.0, w - label_w - _item_pad_x()), _clip_rect_inset(x,
         y,
         w,
         h,
   1.0))
   rect
}

fn _draw_centered_text(any txt, any x, any y, any w, any h, any color) any { _draw_centered_text_ex(_font_body(), txt, x, y, w, h, color) }

fn _draw_centered_text_ex(any font_id, any txt, any x, any y, any w, any h, any color) any {
   def fid = (int(font_id) > 0) ? int(font_id) : _font_body()
   def m = _measure_text_ui(fid, txt)
   def tw = float(m.get(0, 0.0))
   def th = max(_font_h_for(fid), float(m.get(1, _font_h_for(fid))))
   def tx = _snap(x + max(0.0, (w - tw) * 0.5))
   def ty = _snap(y + max(0.0, (h - th) * 0.5))
   _draw_text_ui_ex_clipped(fid, txt, tx, ty, color, 0.0, max(1.0, float(w)), _clip_rect_inset(x, y, w, h, 1.0))
}

@inline
@jit
fn _clip_rect_inset(any x, any y, any w, any h, any inset=0.0) list {
   [
      float(x) + float(inset),
      float(y) + float(inset),
      max(1.0, float(w) - float(inset) * 2.0),
      max(1.0, float(h) - float(inset) * 2.0)
   ]
}

@inline
@jit
fn _text_center_y_for(any font_id, any y, any h) f64 {
   def fid = (int(font_id) > 0) ? int(font_id) : _font_body()
   def fh = _font_h_for(fid)
   _snap(float(y) + max(0.0, (float(h) - fh) * 0.5))
}

fn button(any id, any label, any w=0.0, any h=0.0) bool {
   "Draws a button and returns true when activated."
   def txt = _ui_text(label)
   mut bw = float(w)
   if(bw <= 0.0){ bw = min(_current_body_w(), max(_sx(84.0), _text_fit_width(_font_body(), txt) + _item_pad_x() * 2.0)) }
   mut bh = float(h)
   if(bh <= 0.0){ bh = max(_item_h(), _sx(30.0)) }
   def rect = _layout_rect(bw, bh)
   def full = _current_window_id_cache + "::" + to_str(id)
   _register_focusable(full)
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww, hh = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   def inter = _begin_interact_widget(full, x, y, ww, hh, true)
   def visible = inter.get(0, false)
   def hovered = inter.get(1, false)
   def held = inter.get(2, false)
   def focused = _kbd_focus_id == full
   def clicked = inter.get(3, false) || _widget_key_activate(full)
   if(visible){
      _gui_rect_fast(x, y, ww, hh, held ? _press_u32 : (hovered ? _panel_hover_u32 : _panel_u32))
      if(focused || hovered || held){ _gui_rect_fast(x, y, ww, held ? _sx(3.0) : _sx(2.0), _accent_u32) }
      _gui_rect_outline_fast(x, y, ww, hh, held ? _accent_u32 : ((hovered || focused) ? _accent_u32 : _border_u32))
      _draw_centered_text(txt, x, y, ww, hh, _text_u32)
   }
   clicked
}

fn small_button(any id, any label, any w=0.0, any h=0.0) bool {
   "Draws a compact button sized for toolbars and dense rows."
   def txt = _ui_text(label)
   mut bw = float(w)
   if(bw <= 0.0){ bw = _text_fit_width(_font_small_id(), txt) + _sx(14.0) }
   mut bh = float(h)
   if(bh <= 0.0){ bh = _small_item_h() }
   def rect = _layout_rect(bw, bh)
   def full = _current_window_id_cache + "::" + to_str(id)
   _register_focusable(full)
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww, hh = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   def inter = _begin_interact_widget(full, x, y, ww, hh, true)
   def hovered = inter.get(1, false)
   def held = inter.get(2, false)
   def focused = _kbd_focus_id == full
   def clicked = inter.get(3, false) || _widget_key_activate(full)
   if(inter.get(0, false)){
      _gui_rect_fast(x, y, ww, hh, held ? _press_u32 : (hovered ? _panel_hover_u32 : _panel_u32))
      if(focused || hovered || held){ _gui_rect_fast(x, y, ww, held ? _sx(3.0) : _sx(2.0), _accent_u32) }
      _gui_rect_outline_fast(x, y, ww, hh, held ? _accent_u32 : ((hovered || focused) ? _accent_u32 : _border_u32))
      _draw_centered_text_ex(_font_small_id(), txt, x, y, ww, hh, _text_u32)
   }
   clicked
}

fn _rgba_or_default(any color, any fallback) list {
   if(is_list(color) || is_tuple(color)){
      return [
         clamp(float(color.get(0, fallback.get(0, 1.0))), 0.0, 1.0),
         clamp(float(color.get(1, fallback.get(1, 1.0))), 0.0, 1.0),
         clamp(float(color.get(2, fallback.get(2, 1.0))), 0.0, 1.0),
         clamp(float(color.get(3, fallback.get(3, 1.0))), 0.0, 1.0)
      ]
   }
   fallback
}

fn image(any id, any tex_id, any w, any h, any tint=0) list {
   "Draws a texture region in the current layout and returns its rectangle."
   def rw, rh = _pick_positive(w, _small_item_h()), _pick_positive(h, rw)
   def rect = _layout_rect(rw, rh)
   if(!_rect_intersects(rect, _current_clip())){ return rect }
   def tex = _tex_ref_id(tex_id)
   if(tex >= 0){
      def c = _rgba_or_default(tint, [1.0, 1.0, 1.0, 1.0])
      _gui_tex_ref_uv(
         float(rect.get(0, 0.0)), float(rect.get(1, 0.0)),
         float(rect.get(2, 0.0)), float(rect.get(3, 0.0)),
         tex_id,
         float(c.get(0, 1.0)), float(c.get(1, 1.0)), float(c.get(2, 1.0)), float(c.get(3, 1.0))
      )
   }
   rect
}

fn icon_button(any id, any tex_id, any label="", f64 w=0.0, f64 h=0.0, bool selected=false) bool {
   "Draws an icon button with optional label and selected state."
   def txt = _ui_text(label)
   def has_txt = str.len(txt) > 0
   mut bw = float(w)
   if(bw <= 0.0){
      def text_w = has_txt ? float(_measure_text_ui(_font_small_id(), txt).get(0, 0.0)) : 0.0
      bw = max(_sx(62.0), _sx(22.0) + _item_pad_x() * 2.0 + ((text_w > 0.0) ? (text_w + _tiny_gap()) : 0.0))
   }
   mut bh = float(h)
   if(bh <= 0.0){ bh = max(_small_item_h() + _sx(6.0), _sx(34.0)) }
   def icon_sz = clamp(bh - _sx(12.0), _sx(18.0), _sx(24.0))
   def rect = _layout_rect(bw, bh)
   def full = _current_window_id_cache + "::" + to_str(id)
   _register_focusable(full)
   def inter = _begin_interact_widget(full,
      float(rect.get(0,
      0.0)),
      float(rect.get(1,
      0.0)),
      float(rect.get(2,
      0.0)),
      float(rect.get(3,
      0.0)),
   true)
   def visible = inter.get(0, false)
   def hovered = inter.get(1, false)
   def held = inter.get(2, false)
   def focused = _kbd_focus_id == full
   def clicked = inter.get(3, false) || _widget_key_activate(full)
   if(visible){
      def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      def rwf, rhf = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
      _gui_rect_fast(x,
         y,
         rwf,
         rhf,
      selected ? _accent_soft_u32 : (held ? _press_u32 : (hovered ? _panel_hover_u32 : _panel_u32)))
      if(selected || focused || hovered || held){ _gui_rect_fast(x, y, rwf, held ? _sx(3.0) : _sx(2.0), _accent_u32) }
      _gui_rect_outline_fast(x, y, rwf, rhf, held ? _accent_u32 : ((selected || focused) ? _accent_u32 : _border_u32))
      def ix, iy = x + _item_pad_x(), y + max(0.0, (rhf - icon_sz) * 0.5)
      if(_tex_ref_id(tex_id) >= 0){ _gui_tex_ref_uv(ix, iy, icon_sz, icon_sz, tex_id, 1.0, 1.0, 1.0, 0.96) } else {
         _gui_rect_fast(ix + icon_sz * 0.25,
            iy + icon_sz * 0.25,
            icon_sz * 0.50,
            icon_sz * 0.50,
         selected ? _accent_u32 : _text_dim_u32)
      }
      if(has_txt){
         def text_col = held ? _text_u32 : ((selected || focused || hovered) ? _text_u32 : _text_dim_u32)
         def text_x = ix + icon_sz + _tiny_gap()
         def text_y = _text_center_y_for(_font_small_id(), y, rhf)
         def text_w = max(1.0, rwf - icon_sz - _item_pad_x() * 2.0 - _tiny_gap())
         def text_clip = _clip_rect_inset(x, y, rwf, rhf, 1.0)
         _draw_text_ui_small_clipped(txt, text_x, text_y, text_col, text_w, text_clip)
      }
   }
   clicked
}

fn selectable(any id, any label, bool selected=false, f64 w=0.0, f64 h=0.0, any detail="", any tex_id=-1, bool keyboard=true) bool {
   "Draws a selectable row with optional detail text and icon."
   def txt = _ui_text(label)
   def d = _ui_text(detail)
   def rw = _pick_positive(w, _current_body_w())
   mut rh = float(h)
   if(rh <= 0.0){
      def m = _measure_text_ui(_font_body(), txt)
      rh = max(_small_item_h(), float(m.get(1, _small_item_h())) + ((d.len > 0) ? _text_h() : 0.0) + _sx(8.0))
   }
   def rect = _layout_rect(rw, rh)
   def full = _current_window_id_cache + "::" + to_str(id)
   _register_focusable(full)
   def inter = _begin_interact_widget(full,
      float(rect.get(0,
      0.0)),
      float(rect.get(1,
      0.0)),
      float(rect.get(2,
      0.0)),
      float(rect.get(3,
      0.0)),
   true)
   def visible = inter.get(0, false)
   def hovered = inter.get(1, false)
   def held = inter.get(2, false)
   def focused = _kbd_focus_id == full
   def clicked = inter.get(3, false) || (keyboard && _widget_key_activate(full))
   if(visible){
      def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      def rwf, rhf = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
      def clip = _clip_rect_inset(x, y, rwf, rhf, 1.0)
      def bg = selected ? _accent_soft_u32 : (held ? _panel_active_u32 : (hovered ? _panel_hover_u32 : _panel_u32))
      def title_y = (d.len > 0) ? (y + _sx(5.0)) : _text_center_y_for(_font_body(), y, rhf)
      def icon_sz = (_tex_ref_id(tex_id) >= 0) ? min(_sx(22.0), max(_sx(14.0), rhf - _sx(10.0))) : 0.0
      def text_x = x + _item_pad_x() + ((icon_sz > 0.0) ? (icon_sz + _sx(7.0)) : 0.0)
      def text_w = max(1.0, rw - (text_x - x) - _item_pad_x())
      _gui_rect_fast(x, y, rwf, rhf, bg)
      if(selected || focused || hovered){ _gui_rect_outline_fast(x, y, rwf, rhf, (selected || focused) ? _accent_u32 : _border_u32) }
      if(icon_sz > 0.0){
         _gui_tex_ref_uv(x + _item_pad_x(), y + max(_sx(5.0), (rhf - icon_sz) * 0.5), icon_sz, icon_sz,
         tex_id, 1.0, 1.0, 1.0, selected ? 1.0 : 0.88)
      }
      _draw_text_ui_clipped(txt, text_x, title_y, _text_u32, text_w, clip)
      if(d.len > 0){ _draw_text_ui_small_clipped(d, text_x, y + _sx(5.0) + _text_h() + _tiny_gap(), _text_dim_u32, text_w, clip) }
      if(selected){ _gui_rect_fast(x + rwf - _sx(5.0), y, _sx(5.0), rhf, _accent_u32) }
   }
   clicked
}

fn selectable_file(any id, any label, bool selected=false, f64 w=0.0, f64 h=0.0, any tex_id=-1) bool {
   "Draws a lean selectable row for virtualized file lists."
   def txt = _ui_text(label)
   def rw = _pick_positive(w, _current_body_w())
   def rh = _pick_positive(h, _small_item_h())
   def rect = _layout_rect(rw, rh)
   def full = _current_window_id_cache + "::" + to_str(id)
   def inter = _begin_interact_widget(full,
      float(rect.get(0,
      0.0)),
      float(rect.get(1,
      0.0)),
      float(rect.get(2,
      0.0)),
      float(rect.get(3,
      0.0)),
   true)
   if(inter.get(0, false)){
      def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      def rwf, rhf = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
      def hovered = bool(inter.get(1, false))
      def held = bool(inter.get(2, false))
      def bg = selected ? _accent_soft_u32 : (held ? _panel_active_u32 : (hovered ? _panel_hover_u32 : _panel_u32))
      def icon_sz = (_tex_ref_id(tex_id) >= 0) ? min(_sx(18.0), max(_sx(12.0), rhf - _sx(8.0))) : 0.0
      def text_x = x + _item_pad_x() + ((icon_sz > 0.0) ? (icon_sz + _sx(6.0)) : 0.0)
      def text_w = max(1.0, rwf - (text_x - x) - _item_pad_x())
      _gui_rect_fast(x, y, rwf, rhf, bg)
      if(selected || hovered){ _gui_rect_fast(x, y, selected ? _sx(4.0) : _sx(2.0), rhf, selected ? _accent_u32 : _border_u32) }
      if(icon_sz > 0.0){
         _gui_tex_ref_uv(x + _item_pad_x(), y + max(_sx(4.0), (rhf - icon_sz) * 0.5), icon_sz, icon_sz,
         tex_id, 1.0, 1.0, 1.0, selected ? 1.0 : 0.82)
      }
      _draw_text_ui_ex(_font_body(), txt, text_x, _text_center_y_for(_font_body(), y, rhf), _text_u32, 0.0, text_w)
   }
   bool(inter.get(3, false))
}

fn _text_edit_apply_input(any full_id, str value, int cursor) list {
   mut out = to_str(value)
   mut cur = int(cursor)
   mut selected_all = _text_selected_all(full_id)
   if(selected_all){ cur = out.len }
   mut ki = 0
   def key_events_n = is_list(_key_events) ? _key_events.len : 0
   while(ki < key_events_n){
      def k = int(_key_events.get(ki, 0))
      if(k == _TEXT_SELECT_ALL){
         selected_all = out.len > 0
         cur = out.len
      } elif(k == _TEXT_COPY){
         if(selected_all && out.len > 0){ uiw.set_clipboard(_win, out) }
      } elif(k == _TEXT_CUT){
         if(selected_all && out.len > 0){
            uiw.set_clipboard(_win, out)
            out = ""
            cur = 0
            selected_all = false
         }
      } elif(k == _TEXT_PASTE){
         def clip = uiw.get_clipboard(_win)
         if(clip.len > 0){
            if(selected_all){
               out = clip
               cur = out.len
            } else {
               out = str.str_slice(out, 0, cur) + clip + str.str_slice(out, cur, out.len)
               cur += clip.len
            }
            selected_all = false
         }
      } elif(k == KEY_BACKSPACE){
         if(selected_all){
            out = ""
            cur = 0
            selected_all = false
         } elif(cur > 0){
            def prev = _utf8_prev_index(out, cur)
            out = str.str_slice(out, 0, prev) + str.str_slice(out, cur, out.len)
            cur = prev
         }
      } elif(k == KEY_DELETE){
         if(selected_all){
            out = ""
            cur = 0
            selected_all = false
         } elif(cur < out.len){
            def nxt = _utf8_next_index(out, cur)
            out = str.str_slice(out, 0, cur) + str.str_slice(out, nxt, out.len)
         }
      } elif(k == KEY_LEFT){
         selected_all = false
         cur = _utf8_prev_index(out, cur)
      } elif(k == KEY_RIGHT){
         selected_all = false
         cur = _utf8_next_index(out, cur)
      } elif(k == KEY_HOME){
         selected_all = false
         cur = 0
      } elif(k == KEY_END){
         selected_all = false
         cur = out.len
      } elif(k == KEY_ENTER){
         selected_all = false
         _text_focus_id = ""
         _kbd_focus_id = ""
      }
      ki += 1
   }
   if(_text_events.len > 0){
      if(selected_all){
         out = _text_events
         cur = out.len
      } else {
         out = str.str_slice(out, 0, cur) + _text_events + str.str_slice(out, cur, out.len)
         cur += _text_events.len
      }
      selected_all = false
   }
   _text_set_selected_all(full_id, selected_all)
   [out, cur]
}

fn input_text(any id, any label, any value, any placeholder="", f64 w=0.0) str {
   "Draws an editable text field and returns the edited value."
   mut out = _ui_text(value)
   def full = _current_window_id_cache + "::" + to_str(id)
   def lbl = _ui_text(label)
   def ph = _ui_text(placeholder)
   def rw = _pick_positive(w, _current_body_w())
   def rect = _layout_rect(rw, _slider_h())
   def inter = _begin_interact_widget(full,
      float(rect.get(0,
      0.0)),
      float(rect.get(1,
      0.0)),
      float(rect.get(2,
      0.0)),
      float(rect.get(3,
      0.0)),
   true)
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def rwf = float(rect.get(2, 0.0))
   def field_y = y + _text_h() + _tiny_gap()
   def field_h = max(_small_item_h(), float(rect.get(3, 0.0)) - _text_h() - _tiny_gap())
   def tx = x + _item_pad_x()
   def field_hover = _current_window_pointer_hit(x, field_y, rwf, field_h)
   if(field_hover){
      _ensure_cursors()
      _want_cursor(_cursor_ibeam)
   }
   def nav_focus = _register_focusable(full)
   def mouse_focus = field_hover && _mouse_pressed0
   if(nav_focus || mouse_focus || inter.get(3, false)){
      _kbd_focus_id = full
      _text_focus_id = full
      mut next_cursor = out.len
      if(mouse_focus){ next_cursor = _text_cursor_from_x(_font_body(), out, _mouse_x, tx) }
      _text_state_set_cursor(full, next_cursor)
      _text_set_selected_all(full, nav_focus && !mouse_focus && out.len > 0)
   }
   def active_text = _text_focus_id == full || _kbd_focus_id == full
   if(active_text && _text_focus_id != full){ _text_focus_id = full }
   mut cursor = _text_state_cursor(full, out.len)
   if(active_text){
      def edit = _text_edit_apply_input(full, out, cursor)
      out, cursor = to_str(edit.get(0, out)), int(edit.get(1, cursor))
      _text_state_set_cursor(full, cursor)
   }
   if(inter.get(0, false)){
      def label_clip = _clip_rect_inset(x, y, rwf, _text_h(), 0.0)
      def field_clip = _clip_rect_inset(x, field_y, rwf, field_h, 1.0)
      _gui_rect_fast(x,
         field_y,
         rwf,
         field_h,
         active_text ? _panel_active_u32 : (inter.get(1,
      false) ? _panel_hover_u32 : _panel_u32))
      _gui_rect_outline_fast(x, field_y, rwf, field_h, active_text ? _accent_u32 : _border_u32)
      def draw_s = (out.len > 0) ? out : ph
      mut draw_col = _text_dim_u32
      if(out.len > 0){ draw_col = _text_u32 }
      def ty = _text_center_y_for(_font_body(), field_y, field_h)
      _draw_text_ui_small_clipped(lbl, x, y, _text_u32, rwf, label_clip)
      if(active_text && _text_selected_all(full) && out.len > 0){
         def mw_sel = _measure_text_ui(_font_body(), out)
         _gui_rect_fast(tx, field_y + _sx(4.0), min(rwf - _item_pad_x() * 2.0, float(mw_sel.get(0, 0.0)) + _sx(3.0)), max(_sx(8.0), field_h - _sx(8.0)), _accent_soft_u32)
      }
      _draw_text_ui_clipped(draw_s, tx, ty, draw_col, rwf - _item_pad_x() * 2.0, field_clip)
      if(active_text && ((int(get_time() * 2.0) % 2) == 0)){
         def before = str.str_slice(out, 0, cursor)
         def mw = _measure_text_ui(_font_body(), before)
         def cx = tx + float(mw.get(0, 0.0))
         _gui_rect_fast(cx, field_y + _sx(5.0), 1.0, max(_sx(8.0), field_h - _sx(10.0)), _accent_u32)
      }
   }
   _last_item = [x, field_y, rwf, field_h]
   out
}

fn title_input_text(any id, any value, any placeholder="", any w=0.0) str {
   "Draws an editable text field inside the current window title bar."
   mut out = _ui_text(value)
   def win = _current_window_state()
   if(!is_dict(win)){ return out }
   def win_id = _current_window_id_cache
   def full = win_id + "::" + to_str(id)
   def sx = float(win.get("x", 0.0))
   def sy = float(win.get("y", 0.0))
   def sw = float(win.get("w", 0.0))
   def th = _title_h()
   def right_pad = _sx(62.0)
   def x = sx + clamp(sw * 0.19, _sx(150.0), _sx(260.0))
   def max_w = max(_sx(120.0), sw - (x - sx) - right_pad)
   def rw = min(max_w, _pick_positive(w, min(max_w, max(_sx(220.0), sw * 0.38))))
   def field_h = max(_sx(18.0), th - _sx(10.0))
   def y = sy + _sx(5.0)
   def tx = x + _sx(8.0)
   def hovered = _window_accepts_pointer(win_id) && _rect_hit(_mouse_x, _mouse_y, x, y, rw, field_h)
   if(hovered){
      _hot_id = full
      _hot_rect = [x, y, rw, field_h]
      _ensure_cursors()
      _want_cursor(_cursor_ibeam)
   }
   def nav_focus = _register_focusable(full)
   def mouse_focus = hovered && _mouse_pressed0
   if(nav_focus || mouse_focus){
      _kbd_focus_id = full
      _text_focus_id = full
      _focused_window = win_id
      mut next_cursor = out.len
      if(mouse_focus){ next_cursor = _text_cursor_from_x(_font_small_id(), out, _mouse_x, tx) }
      _text_state_set_cursor(full, next_cursor)
      _text_set_selected_all(full, nav_focus && !mouse_focus && out.len > 0)
   }
   def active_text = _text_focus_id == full || _kbd_focus_id == full
   if(active_text && _text_focus_id != full){ _text_focus_id = full }
   mut cursor = _text_state_cursor(full, out.len)
   if(active_text){
      def edit = _text_edit_apply_input(full, out, cursor)
      out, cursor = to_str(edit.get(0, out)), int(edit.get(1, cursor))
      _text_state_set_cursor(full, cursor)
   }
   def draw_s = (out.len > 0) ? out : _ui_text(placeholder)
   def draw_col = (out.len > 0) ? _text_u32 : _text_dim_u32
   def bg = active_text ? _panel_active_u32 : (hovered ? _panel_hover_u32 : _panel_u32)
   def restore_body_clip = bool(win.get("clip_active", false))
   if(restore_body_clip){ pop_clip_rect() }
   _gui_rect_fast(x, y, rw, field_h, bg)
   _gui_rect_outline_fast(x, y, rw, field_h, active_text ? _accent_u32 : _border_u32)
   if(active_text && _text_selected_all(full) && out.len > 0){
      def mw_sel = _measure_text_ui(_font_small_id(), out)
      _gui_rect_fast(tx, y + _sx(4.0), min(rw - _sx(16.0), float(mw_sel.get(0, 0.0)) + _sx(3.0)), max(_sx(8.0), field_h - _sx(8.0)), _accent_soft_u32)
   }
   _draw_text_ui_small_clipped(draw_s,
      tx,
      _text_center_y_for(_font_small_id(),
         y,
      field_h),
      draw_col,
      max(1.0,
      rw - _sx(16.0)),
      [x + 1.0,
         y + 1.0,
         max(1.0,
         rw - 2.0),
         max(1.0,
   field_h - 2.0)])
   if(active_text && ((int(get_time() * 2.0) % 2) == 0)){
      def before = str.str_slice(out, 0, cursor)
      def mw = _measure_text_ui(_font_small_id(), before)
      def cx = min(x + rw - _sx(6.0), tx + float(mw.get(0, 0.0)))
      _gui_rect_fast(cx, y + _sx(4.0), 1.0, max(_sx(8.0), field_h - _sx(8.0)), _accent_u32)
   }
   if(restore_body_clip){ push_clip_rect(_window_body_clip(win)) }
   out
}

fn _choice_box_interact(any id, any txt, any box) list {
   def m = _measure_text_ui(_font_body(), txt)
   def rect = _layout_rect(_current_body_w(), max(box, float(m.get(1, box))))
   def full = _current_window_id_cache + "::" + to_str(id)
   _register_focusable(full)
   def inter = _begin_interact_widget(full,
      float(rect.get(0,
      0.0)),
      float(rect.get(1,
      0.0)),
      float(rect.get(2,
      0.0)),
      float(rect.get(3,
      0.0)),
   true)
   [rect, full, inter, _kbd_focus_id == full]
}

fn _draw_choice_label(any txt, any rect, any box, any bx, any by) any {
   _draw_text_ui_clipped(
      txt, bx + box + _gap(), _text_center_y_for(_font_body(), by, box), _text_u32,
      float(rect.get(2, 0.0)) - box - _gap(),
      _clip_rect_inset(bx, by, float(rect.get(2, 0.0)), float(rect.get(3, 0.0)), 1.0)
   )
}

fn _choice_control(any id, any label, any value, bool radio=false) bool {
   def txt = _ui_text(label)
   def box = _small_item_h()
   def hit = _choice_box_interact(id, txt, box)
   def rect = hit.get(0, [0.0, 0.0, 0.0, 0.0])
   def full = hit.get(1, "")
   def inter = hit.get(2, [])
   mut out = !!value
   def focused = hit.get(3, false)
   if(inter.get(3, false) || _widget_key_activate(full)){ out = radio ? true : !out }
   if(inter.get(0, false)){
      def bx, by = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      _gui_rect_fast(bx, by, box, box, out ? _accent_soft_u32 : _panel_u32)
      _gui_rect_outline_fast(bx, by, box, box, (inter.get(1, false) || focused) ? _accent_u32 : _border_u32)
      if(out){
         if(radio){ _gui_rect_fast(bx + _sx(5.0), by + _sx(5.0), box - _sx(10.0), box - _sx(10.0), _accent_u32) } else {
            _gui_rect_fast(bx + _sx(5.0), by + _sx(9.0), _sx(5.0), _sx(2.0), _ok_u32)
            _gui_rect_fast(bx + _sx(9.0), by + _sx(5.0), _sx(8.0), _sx(2.0), _ok_u32)
         }
      }
      _draw_choice_label(txt, rect, box, bx, by)
   }
   out
}

fn checkbox(any id, any label, any value) bool { _choice_control(id, label, value, false) }

fn toggle(any id, any label, any value) bool {
   "Draws a toggle switch and returns the updated boolean value."
   def txt = _ui_text(label)
   def tw = _toggle_w()
   def th = _toggle_h()
   def m = _measure_text_ui(_font_body(), txt)
   def rect = _layout_rect(min(_current_body_w(), _text_fit_width(_font_body(), txt) + _gap() + tw), max(float(m.get(1, th)), th))
   def full = _current_window_id_cache + "::" + to_str(id)
   _register_focusable(full)
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww, hh = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   def inter = _begin_interact_widget(full, x, y, ww, hh, true)
   mut out = !!value
   def focused = _kbd_focus_id == full
   if(inter.get(3, false) || _widget_key_activate(full)){ out = !out }
   if(inter.get(0, false)){
      def tx, ty = x + ww - tw, y + max(0.0, (hh - th) * 0.5)
      def clip = _clip_rect_inset(x, y, ww, hh, 1.0)
      _draw_text_ui_clipped(txt,
         x,
         _text_center_y_for(_font_body(), y, hh),
         _text_u32,
         ww - tw - _gap(),
      clip)
      _gui_rect_fast(tx, ty, tw, th, out ? _accent_soft_u32 : _panel_u32)
      _gui_rect_outline_fast(tx, ty, tw, th, (inter.get(1, false) || focused) ? _accent_u32 : _border_u32)
      def knob = th - _sx(4.0)
      def kx = out ? (tx + tw - knob - _sx(2.0)) : (tx + _sx(2.0))
      _gui_rect_fast(kx, ty + _sx(2.0), knob, knob, out ? _accent_u32 : _text_dim_u32)
   }
   out
}

fn radio_button(any id, any label, any selected) bool { _choice_control(id, label, selected, true) }

fn tab_strip(any id, list labels, int selected=0, f64 w=0.0, f64 h=0.0) int {
   "Draws a tab strip and returns the selected tab index."
   def items = is_list(labels) ? labels : []
   if(items.len == 0){ return int(selected) }
   def id_s = to_str(id)
   def rw = _pick_positive(w, _current_body_w())
   def rh = _pick_positive(h, _item_h())
   def rect = _layout_rect(rw, rh)
   def clip = _current_clip()
   mut out = int(selected)
   if(!_rect_intersects(rect, clip)){ return out }
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww = float(rect.get(2, 0.0))
   def hh = float(rect.get(3, 0.0))
   def n = items.len
   def tab_w = max(_sx(52.0), ww / float(max(n, 1)))
   _gui_rect_fast(x, y, ww, hh, _window_bg_u32)
   _gui_rect_outline_fast(x, y, ww, hh, _border_u32)
   mut i = 0
   while(i < n){
      mut tx, tw = x + float(i) * tab_w, tab_w
      if(i == n - 1){ tx, tw = x + ww - tab_w, max(_sx(52.0), x + ww - tx) }
      def full = _current_window_id_cache + "::" + id_s + "#tab" + to_str(i)
      _register_focusable(full)
      def inter = _begin_interact_widget(full, tx, y, tw, hh, true)
      def hovered = inter.get(1, false)
      def held = inter.get(2, false)
      def focused = _kbd_focus_id == full
      if(inter.get(3, false) || _widget_key_activate(full)){ out = i }
      def active = out == i
      _gui_rect_fast(tx,
         y,
         tw,
         hh,
      active ? _accent_soft_u32 : (held ? _panel_active_u32 : (hovered ? _panel_hover_u32 : _panel_u32)))
      if(focused && !active){ _gui_rect_outline_fast(tx, y, tw, hh, _accent_u32) }
      if(i > 0){ _gui_rect_fast(tx, y + _sx(5.0), 1.0, hh - _sx(10.0), _border_u32) }
      if(active){ _gui_rect_fast(tx + _sx(6.0), y + hh - _sx(3.0), max(_sx(12.0), tw - _sx(12.0)), _sx(3.0), _accent_u32) }
      _draw_centered_text(to_str(items.get(i, "")), tx, y, tw, hh, active ? _text_u32 : _text_dim_u32)
      i += 1
   }
   out
}

fn _combo_popup_unclip_window() list {
   mut restore_window_clip, restore_window_rect = false, []
   def win = _current_window_state()
   if(is_dict(win) && win.get("clip_active", false)){
      restore_window_rect = _window_body_clip(win)
      pop_clip_rect()
      restore_window_clip = true
   }
   [restore_window_clip, restore_window_rect]
}

fn _draw_combo_popup_frame(f64 x, f64 popup_y, f64 rwf, f64 popup_h) any {
   _gui_rect_fast(x + _sx(5.0), popup_y + _sx(7.0), rwf, popup_h, color_pack(0.0, 0.0, 0.0, 0.44))
   _gui_rect_fast(x + _sx(2.0), popup_y + _sx(3.0), rwf, popup_h, color_pack(0.0, 0.0, 0.0, 0.30))
   _gui_rect_fast(x, popup_y, rwf, popup_h, _popup_u32)
   _gui_rect_fast(x + 1.0, popup_y + 1.0, max(1.0, rwf - 2.0), _sx(2.0), _accent_u32)
   _gui_rect_outline_fast(x, popup_y, rwf, popup_h, _accent_u32)
}

fn _combo_popup_item_range(list values, f64 row_h, int popup_rows, f64 scroll_y) list {
   mut first_i = int(scroll_y / row_h)
   if(first_i < 0){ first_i = 0 }
   if(first_i >= values.len){ first_i = max(0, values.len - 1) }
   [first_i, min(values.len, first_i + popup_rows + 2)]
}

fn _combo_popup_draw_items(str full, list values, int out, f64 x, f64 popup_y, f64 rwf, f64 popup_h, f64 row_h, int popup_rows, f64 scroll_y, bool draw_visual=true) list {
   mut next_out, next_open = int(out), true
   def span = _combo_popup_item_range(values, row_h, popup_rows, scroll_y)
   mut i = int(span.get(0, 0))
   def last_i = int(span.get(1, i))
   while(i < last_i){
      def item_y = popup_y + float(i) * row_h - scroll_y
      if(item_y + row_h >= popup_y && item_y <= popup_y + popup_h){
         def item_hover = _current_window_pointer_hit(x + 1.0, item_y, max(1.0, rwf - 2.0), row_h)
         if(item_hover){
            _hot_id = full + "#item" + to_str(i)
            _hot_rect = [x + 1.0, item_y, max(1.0, rwf - 2.0), row_h]
         }
         if(item_hover && _mouse_pressed0){
            next_out = i
            next_open = false
            _kbd_focus_id = full
            _focused_window = _current_window_id_cache
         }
         if(draw_visual){
            def active = next_out == i
            _gui_rect_fast(x + 1.0, item_y, max(1.0, rwf - 2.0), row_h, active ? _panel_active_u32 : (item_hover ? _panel_hover_u32 : _popup_u32))
            if(active){ _gui_rect_fast(x + 1.0, item_y, _sx(4.0), row_h, _accent_u32) }
            _draw_text_ui_clipped(to_str(values.get(i, "")),
               x + _item_pad_x() + _sx(4.0),
               _text_center_y_for(_font_body(), item_y, row_h),
               active ? _text_u32 : _text_dim_u32,
               rwf - _item_pad_x() * 2.0 - _sx(8.0),
            [x + 1.0, item_y, max(1.0, rwf - 2.0), row_h])
         }
      }
      i += 1
   }
   [next_out, next_open]
}

fn _combo_popup_scrollbar(str popup_key, str full, f64 x, f64 rwf, f64 popup_y, f64 popup_h, f64 max_scroll, f64 scroll_y, bool draw_visual=true) f64 {
   mut next_scroll = float(scroll_y)
   if(max_scroll <= 1.0){ return next_scroll }
   def track_w, track_x = _sx(6.0), x + rwf - _sx(6.0) - _sx(2.0)
   def track_y, track_h = popup_y + 1.0, popup_h - 2.0
   def thumb_h = max(_sx(18.0), popup_h * (popup_h / (popup_h + max_scroll)))
   def usable = max(1.0, track_h - thumb_h)
   def scroll_id = popup_key + "#thumb"
   mut thumb_y = track_y + usable * ((max_scroll > 0.0) ? (next_scroll / max_scroll) : 0.0)
   def track_hover = _current_window_pointer_hit(track_x, track_y, track_w, track_h)
   def thumb_hover = _current_window_pointer_hit(track_x, thumb_y, track_w, thumb_h)
   if(thumb_hover){
      _hot_id = scroll_id
      _hot_rect = [track_x, thumb_y, track_w, thumb_h]
      _ensure_cursors()
      _want_cursor(_cursor_hand)
   }
   if(thumb_hover && _mouse_pressed0){
      _active_widget_id = scroll_id
      _kbd_focus_id = full
      _focused_window = _current_window_id_cache
      _state_set_num(scroll_id, "drag_off_y", _mouse_y - thumb_y)
   } elif(track_hover && _mouse_pressed0){
      def want_thumb_y = clamp(_mouse_y - thumb_h * 0.5, track_y, track_y + usable)
      next_scroll = (max_scroll > 0.0) ? (max_scroll * ((want_thumb_y - track_y) / usable)) : 0.0
      thumb_y = track_y + usable * ((max_scroll > 0.0) ? (next_scroll / max_scroll) : 0.0)
   }
   if(_active_widget_id == scroll_id){
      if(_mouse_down0){
         def want_thumb_y = clamp(_mouse_y - _state_get_num(scroll_id, "drag_off_y", 0.0), track_y, track_y + usable)
         next_scroll = (max_scroll > 0.0) ? (max_scroll * ((want_thumb_y - track_y) / usable)) : 0.0
         thumb_y = want_thumb_y
      } else {
         _active_widget_id = ""
      }
   }
   if(draw_visual){
      _gui_rect_fast(track_x, track_y, track_w, track_h, _scroll_track_u32)
      _gui_rect_fast(track_x, thumb_y, track_w, thumb_h, (_active_widget_id == scroll_id || thumb_hover) ? _scroll_thumb_hot_u32 : _scroll_thumb_u32)
   }
   next_scroll
}

fn _combo_popup_reserve_layout(bool forced_open, f64 field_y, f64 x, f64 popup_y, f64 rwf, f64 popup_h) any {
   if(!forced_open || popup_y < field_y){ return 0 }
   def reserve_y = popup_y + popup_h + _gap()
   def cur_y = _current_cursor_y_cache
   if(reserve_y > cur_y){
      _current_cursor_y_cache = reserve_y
      _last_item = [x, popup_y, rwf, popup_h]
      _same_line_next = false
   }
}

fn _combo_popup(str full,
   str popup_key,
   list values,
   int out,
   bool open,
   bool forced_open,
   f64 x,
   f64 field_y,
   f64 rwf,
   f64 popup_y,
   f64 popup_h,
   f64 row_h,
   int popup_rows,
   f64 max_scroll,
   f64 scroll_y) list {
   mut next_out, next_open, next_scroll = int(out), !!open, float(scroll_y)
   if(!next_open){ return [next_out, next_open, next_scroll] }
   def item_state = _combo_popup_draw_items(full, values, next_out, x, popup_y, rwf, popup_h, row_h, popup_rows, next_scroll, false)
   next_out, next_open = int(item_state.get(0, next_out)), !!item_state.get(1, next_open)
   next_scroll = _combo_popup_scrollbar(popup_key, full, x, rwf, popup_y, popup_h, max_scroll, next_scroll, false)
   if(next_open){
      _combo_popup_queue_draw(popup_key, values, next_out, x, popup_y, rwf, popup_h, row_h, popup_rows, max_scroll, next_scroll)
      _popup_next_hit_active = true
      _popup_next_hit_window = _current_window_id_cache
      _popup_next_hit_x, _popup_next_hit_y = x, popup_y
      _popup_next_hit_w, _popup_next_hit_h = rwf, popup_h
   }
   _combo_popup_reserve_layout(forced_open, field_y, x, popup_y, rwf, popup_h)
   [next_out, next_open, next_scroll]
}

fn _combo_popup_queue_draw(str popup_key,
   list values,
   int out,
   f64 x,
   f64 popup_y,
   f64 rwf,
   f64 popup_h,
   f64 row_h,
   int popup_rows,
   f64 max_scroll,
   f64 scroll_y) any {
   def span = _combo_popup_item_range(values, row_h, popup_rows, scroll_y)
   def first_i = int(span.get(0, 0))
   def last_i = int(span.get(1, first_i))
   _popup_combo_active = true
   _popup_combo_key = popup_key
   _popup_combo_x, _popup_combo_y = x, popup_y
   _popup_combo_w, _popup_combo_h = rwf, popup_h
   _popup_combo_row_h = row_h
   _popup_combo_rows = popup_rows
   _popup_combo_max_scroll = max_scroll
   _popup_combo_scroll_y = scroll_y
   _popup_combo_out = out
   _popup_combo_first = first_i
   mut labels = []
   mut i = first_i
   while(i < last_i){
      labels = labels.append(_ui_text(values.get(i, "")))
      i += 1
   }
   _popup_combo_labels = labels
}

fn _combo_popup_scrollbar_visual(str popup_key, f64 x, f64 rwf, f64 popup_y, f64 popup_h, f64 max_scroll, f64 scroll_y) any {
   if(max_scroll <= 1.0){ return 0 }
   def track_w, track_x = _sx(6.0), x + rwf - _sx(6.0) - _sx(2.0)
   def track_y, track_h = popup_y + 1.0, popup_h - 2.0
   def thumb_h = max(_sx(18.0), popup_h * (popup_h / (popup_h + max_scroll)))
   def usable = max(1.0, track_h - thumb_h)
   def scroll_id = popup_key + "#thumb"
   def thumb_y = track_y + usable * ((max_scroll > 0.0) ? (scroll_y / max_scroll) : 0.0)
   def thumb_hover = _rect_hit(_mouse_x, _mouse_y, track_x, thumb_y, track_w, thumb_h)
   _gui_rect_fast(track_x, track_y, track_w, track_h, _scroll_track_u32)
   _gui_rect_fast(track_x, thumb_y, track_w, thumb_h, (_active_widget_id == scroll_id || thumb_hover) ? _scroll_thumb_hot_u32 : _scroll_thumb_u32)
}

fn _combo_popup_draw_labels(list labels, int first_i, int out, f64 x, f64 popup_y, f64 rwf, f64 popup_h, f64 row_h, f64 scroll_y) any {
   mut li = 0
   def n = labels.len
   while(li < n){
      def item_i = first_i + li
      def item_y = popup_y + float(item_i) * row_h - scroll_y
      if(item_y + row_h >= popup_y && item_y <= popup_y + popup_h){
         def item_hover = _rect_hit(_mouse_x, _mouse_y, x + 1.0, item_y, max(1.0, rwf - 2.0), row_h)
         def active = int(out) == item_i
         _gui_rect_fast(x + 1.0, item_y, max(1.0, rwf - 2.0), row_h, active ? _panel_active_u32 : (item_hover ? _panel_hover_u32 : _popup_u32))
         if(active){ _gui_rect_fast(x + 1.0, item_y, _sx(4.0), row_h, _accent_u32) }
         _draw_text_ui_clipped(_ui_text(labels.get(li, "")),
            x + _item_pad_x() + _sx(4.0),
            _text_center_y_for(_font_body(), item_y, row_h),
            active ? _text_u32 : _text_dim_u32,
            rwf - _item_pad_x() * 2.0 - _sx(8.0),
         [x + 1.0, item_y, max(1.0, rwf - 2.0), row_h])
      }
      li += 1
   }
}

fn _combo_popup_draw_flat(str popup_key,
   f64 x,
   f64 popup_y,
   f64 rwf,
   f64 popup_h,
   f64 row_h,
   int popup_rows,
   f64 max_scroll,
   f64 scroll_y,
   int out,
   int first_i,
   list labels) any {
   _draw_combo_popup_frame(x, popup_y, rwf, popup_h)
   push_clip_rect(x + 1.0, popup_y + 1.0, max(1.0, rwf - 2.0), max(1.0, popup_h - 2.0))
   _combo_popup_draw_labels(labels, first_i, out, x, popup_y, rwf, popup_h, row_h, scroll_y)
   pop_clip_rect()
   _combo_popup_scrollbar_visual(popup_key, x, rwf, popup_y, popup_h, max_scroll, scroll_y)
}

fn _combo_selected_index(list values, int selected) int {
   mut out = int(selected)
   if(values.len <= 0){ return -1 }
   if(out < 0){ return 0 }
   if(out >= values.len){ return values.len - 1 }
   out
}

fn _combo_layout(f64 w) list {
   def rw = _pick_positive(w, _current_body_w())
   def field_h = _small_item_h()
   def rect = _layout_rect(rw, _text_h() + _tiny_gap() + field_h)
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def rwf = float(rect.get(2, 0.0))
   [rect, x, y, rwf, y + _text_h() + _tiny_gap(), field_h]
}

fn _combo_apply_trigger(str full, bool open, bool forced_open, bool nav_focus, list inter) bool {
   mut next_open = forced_open ? true : !!open
   if(nav_focus){ _kbd_focus_id = full }
   if(inter.get(3, false) || _widget_key_activate(full)){
      next_open = !next_open
      _focused_window = _current_window_id_cache
   }
   next_open
}

fn _combo_apply_keyboard(str full, list values, int out, int max_visible) int {
   mut next_out = int(out)
   if(_kbd_focus_id != full || values.len <= 0){ return next_out }
   if(_key_event_present(KEY_UP)){ next_out = (next_out <= 0) ? 0 : (next_out - 1) }
   if(_key_event_present(KEY_DOWN)){ next_out = (next_out >= values.len - 1) ? (values.len - 1) : (next_out + 1) }
   if(_key_event_present(KEY_PAGE_UP)){ next_out = max(0, next_out - max(1, int(max_visible))) }
   if(_key_event_present(KEY_PAGE_DOWN)){ next_out = min(values.len - 1, next_out + max(1, int(max_visible))) }
   if(_key_event_present(KEY_HOME)){ next_out = 0 }
   if(_key_event_present(KEY_END)){ next_out = values.len - 1 }
   next_out
}

fn _combo_popup_metrics(list values, f64 field_y, f64 field_h, int max_visible) list {
   def row_h = field_h
   def popup_rows = min(values.len, max(1, int(max_visible)))
   def popup_h = max(1.0, float(popup_rows) * row_h + 2.0)
   def clip_y, clip_h = 0.0, _fb_h
   mut popup_y = field_y + field_h + _tiny_gap()
   if(popup_y + popup_h > clip_y + clip_h){
      def above_y = field_y - popup_h - _tiny_gap()
      if(above_y >= clip_y){ popup_y = above_y } else { popup_y = max(clip_y, clip_y + clip_h - popup_h) }
   }
   [row_h, popup_rows, popup_h, popup_y, max(0.0, float(values.len) * row_h - popup_h)]
}

fn _combo_scroll_state(bool open, list values, int out, str popup_key, list inter, bool popup_hover, f64 row_h, f64 popup_h, f64 max_scroll) f64 {
   mut scroll_y = clamp(_state_get_num(popup_key, "scroll_y", 0.0), 0.0, max_scroll)
   if(open && (inter.get(1, false)|| popup_hover) && abs(_mouse_scroll_dy) > 0.000001){ scroll_y = clamp(scroll_y - _mouse_scroll_dy * _scroll_step(), 0.0, max_scroll) }
   if(open && values.len > 0 && out >= 0){
      def sel_top = float(out) * row_h
      def sel_bot = sel_top + row_h
      if(sel_top < scroll_y){ scroll_y = sel_top } elif(sel_bot > scroll_y + popup_h){ scroll_y = sel_bot - popup_h }
      scroll_y = clamp(scroll_y, 0.0, max_scroll)
   }
   scroll_y
}

fn _draw_combo_field(str full, str lbl, list values, int out, bool open, list inter, f64 x, f64 y, f64 rwf, f64 field_y, f64 field_h) any {
   if(!inter.get(0, false)){ return 0 }
   def label_clip = _clip_rect_inset(x, y, rwf, _text_h(), 0.0)
   def field_clip = _clip_rect_inset(x, field_y, rwf, field_h, 1.0)
   _gui_rect_fast(x,
      field_y,
      rwf,
      field_h,
      open ? _panel_active_u32 : (inter.get(1,
   false) ? _panel_hover_u32 : _panel_u32))
   _gui_rect_outline_fast(x, field_y, rwf, field_h, (_kbd_focus_id == full || open) ? _accent_u32 : _border_u32)
   def show_txt = ((values.len > 0) && out >= 0 && out < values.len) ? _ui_text(values.get(out, "")) : "<empty>"
   def text_y = _text_center_y_for(_font_body(), field_y, field_h)
   def show_col = (values.len > 0) ? _text_u32 : _text_dim_u32
   _draw_text_ui_clipped(lbl, x, y, _text_u32, rwf, label_clip)
   _draw_text_ui_clipped(show_txt,
      x + _item_pad_x(),
      text_y,
      show_col,
      rwf - _item_pad_x() * 2.0 - _sx(14.0),
   field_clip)
   _draw_text_ui_small_clipped(open ? "^" : "v",
      x + rwf - _item_pad_x() - _sx(8.0),
      text_y,
      _text_dim_u32,
      _sx(10.0),
   field_clip)
}

fn combo_box(any id, any label, list items, int selected=0, f64 w=0.0, int max_visible=6) int {
   "Draws a combo box and returns the selected item index."
   def values = is_list(items) ? items : []
   mut out = _combo_selected_index(values, selected)
   def full = _current_window_id_cache + "::" + to_str(id)
   def open_key = full + "#open"
   def popup_key = full + "#popup"
   def lbl = _ui_text(label)
   def geom = _combo_layout(w)
   def x, y = float(geom.get(1, 0.0)), float(geom.get(2, 0.0))
   def rwf, field_y = float(geom.get(3, 0.0)), float(geom.get(4, 0.0))
   def field_h = float(geom.get(5, _small_item_h()))
   def nav_focus = _register_focusable(full)
   def inter = _begin_interact_widget(full, x, field_y, rwf, field_h, true)
   def forced_open = _debug_combo_forced(full, id)
   mut open = _combo_apply_trigger(full, _state_get_bool(open_key, false), forced_open, nav_focus, inter)
   out = _combo_apply_keyboard(full, values, out, max_visible)
   def metrics = _combo_popup_metrics(values, field_y, field_h, max_visible)
   def row_h, popup_h = float(metrics.get(0, field_h)), float(metrics.get(2, field_h))
   def popup_rows, popup_y = int(metrics.get(1, 1)), float(metrics.get(3, field_y + field_h))
   def max_scroll = float(metrics.get(4, 0.0))
   def popup_hover = open && _current_window_pointer_hit(x, popup_y, rwf, popup_h)
   if(open && !forced_open && _mouse_pressed0 && !inter.get(1, false) && !popup_hover){ open = false }
   mut scroll_y = _combo_scroll_state(open, values, out, popup_key, inter, popup_hover, row_h, popup_h, max_scroll)
   _draw_combo_field(full, lbl, values, out, open, inter, x, y, rwf, field_y, field_h)
   def popup_state = _combo_popup(full, popup_key, values, out, open, forced_open, x, field_y, rwf, popup_y, popup_h, row_h, popup_rows, max_scroll, scroll_y)
   out, open, scroll_y = int(popup_state.get(0, out)), !!popup_state.get(1, open), float(popup_state.get(2, scroll_y))
   _state_set_bool(open_key, open)
   _state_set_num(popup_key, "scroll_y", scroll_y)
   if(!(forced_open && open)){ _last_item = [x, field_y, rwf, field_h] }
   out
}

fn slider_float(any id, any label, f64 value, f64 min_v, f64 max_v, f64 w=0.0) f64 {
   "Draws a floating-point slider and returns the adjusted value."
   mut lo = float(min_v)
   mut hi = float(max_v)
   if(hi < lo){
      def t = lo
      lo = hi
      hi = t
   }
   mut out = clamp(float(value), lo, hi)
   def full_w = _pick_positive(w, _current_body_w())
   def rect = _layout_rect(full_w, _slider_h())
   def full = _current_window_id_cache + "::" + to_str(id)
   _register_focusable(full)
   def inter = _begin_interact_widget(full,
      float(rect.get(0,
      0.0)),
      float(rect.get(1,
      0.0)),
      float(rect.get(2,
      0.0)),
      float(rect.get(3,
      0.0)),
   true)
   if(_kbd_focus_id == full){
      def step = max((hi - lo) / 100.0, 0.0001)
      if(_key_event_present(KEY_LEFT) || _key_event_present(KEY_DOWN)){ out = clamp(out - step, lo, hi) }
      if(_key_event_present(KEY_RIGHT) || _key_event_present(KEY_UP)){ out = clamp(out + step, lo, hi) }
   }
   if(inter.get(2, false) || (inter.get(1, false) && _mouse_pressed0)){
      def track_x = float(rect.get(0, 0.0)) + _item_pad_x()
      def track_w = max(_sx(24.0), float(rect.get(2, 0.0)) - _item_pad_x() * 2.0)
      def t = clamp((_mouse_x - track_x) / track_w, 0.0, 1.0)
      out = lo + (hi - lo) * t
   }
   if(inter.get(0, false)){
      def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      def rw, ry = float(rect.get(2, 0.0)), y + _text_h() + _sx(10.0)
      def rh, rx = _sx(8.0), x + _item_pad_x()
      def tw = max(_sx(24.0), rw - _item_pad_x() * 2.0)
      def norm = (hi > lo) ? clamp((out - lo) / (hi - lo), 0.0, 1.0) : 0.0
      def fill_w = max(_sx(6.0), tw * norm)
      def clip = _clip_rect_inset(x, y, rw, float(rect.get(3, 0.0)), 1.0)
      _draw_text_ui_clipped(_ui_text(label), x, y, _text_u32, rw - _sx(90.0), clip)
      _draw_text_ui_small_clipped(f"{out:.3f}", x + rw - _sx(84.0), y + _sx(1.0), _text_dim_u32, _sx(80.0), clip)
      _gui_rect_fast(rx, ry, tw, rh, _panel_u32)
      _gui_rect_fast(rx, ry, fill_w, rh, _accent_u32)
      _gui_rect_outline_fast(rx,
         ry,
         tw,
         rh,
         (inter.get(1,
      false) || _kbd_focus_id == full) ? _accent_u32 : _border_u32)
      def knob_x = rx + tw * norm - _sx(5.0)
      _gui_rect_fast(knob_x,
         ry - _sx(4.0),
         _sx(10.0),
         _sx(16.0),
         inter.get(2,
      false) ? _panel_active_u32 : _panel_hover_u32)
   }
   out
}

fn slider_int(any id, any label, any value, any min_v, any max_v, any w=0.0) int {
   "Draws an integer slider and returns the rounded adjusted value."
   def fv = slider_float(id, label, float(value), float(min_v), float(max_v), w)
   int(fv + 0.5)
}

fn progress_bar(any id, any value, any label="", any w=0.0) any {
   "Draws a bounded progress bar."
   def full_w = _pick_positive(w, _current_body_w())
   def rect = _layout_rect(full_w, _small_item_h() + _sx(10.0))
   def p = clamp(float(value), 0.0, 1.0)
   if(_rect_intersects(rect, _current_clip())){
      def label_s = _ui_text(label)
      def x = float(rect.get(0, 0.0))
      def y = float(rect.get(1, 0.0))
      def rw = float(rect.get(2, 0.0))
      def rh = float(rect.get(3, 0.0))
      def inset = _sx(2.0)
      _gui_rect_fast(x, y, rw, rh, _panel_u32)
      _gui_rect_fast(x + inset, y + inset, max(1.0, rw - inset * 2.0), max(1.0, rh - inset * 2.0), _window_bg_u32)
      _gui_rect_fast(x + inset,
         y + inset,
         max(_sx(2.0),
         (rw - inset * 2.0) * p),
         max(1.0,
         rh - inset * 2.0),
      _accent_u32)
      _gui_rect_outline_fast(x, y, rw, rh, _border_u32)
      def pct = f"{p * 100.0:.0f}%"
      def txt = (label_s.len > 0) ? (label_s + "  " + pct) : (to_str(id) + "  " + pct)
      _draw_centered_text_ex(_font_small_id(), txt, x, y, rw, rh, _text_u32)
   }
}

fn stat_card(any id, any label, any value, any detail="", f64 w=0.0, f64 h=0.0, any tone=0) list {
   "Draws a compact statistic card and returns its rectangle."
   def label_s = _ui_text(label)
   def value_s = _ui_text(value)
   def detail_s = _ui_text(detail)
   def rw = _pick_positive(w, _current_body_w())
   def rh = _pick_positive(h, _sx(84.0))
   def rect = _layout_rect(rw, rh)
   if(!_rect_intersects(rect, _current_clip())){ return rect }
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww = float(rect.get(2, 0.0))
   def hh = float(rect.get(3, 0.0))
   def clip = _clip_rect_inset(x, y, ww, hh, 1.0)
   def accent_col = is_list(tone) ? tone : _accent_rgba
   _gui_rect_fast(x + _sx(2.0), y + _sx(3.0), ww, hh, color_pack(0.0, 0.0, 0.0, 0.14))
   _gui_rect_fast(x, y, ww, hh, _window_bg_u32)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), max(1.0, hh - 2.0), _window_body_u32)
   _gui_rect_fast(x + 1.0, y + _sx(18.0), max(1.0, ww - 2.0), max(1.0, hh - _sx(19.0)), _panel_u32)
   def hdr_h = _sx(20.0)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), hdr_h, _window_hdr_u32)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), _sx(3.0), color_pack(float(accent_col.get(0,
         0.31)),
         float(accent_col.get(1,
         0.71)),
         float(accent_col.get(2,
         1.0)),
   0.92))
   _gui_rect_outline_fast(x, y, ww, hh, _border_u32)
   mut value_font = ((hh >= _sx(88.0)) ? _font_title_id() : _font_body())
   def detail_font = _font_small_id()
   mut value_h = _font_h_for(value_font)
   def detail_h = _font_h_for(detail_font)
   def label_y = y + _sx(4.0)
   mut value_y = y + hdr_h + _sx(9.0)
   mut detail_y = y + hh - detail_h - _sx(9.0)
   if(detail_s.len > 0 && detail_y < value_y + value_h + _sx(6.0)){
      value_font = _font_body()
      value_h = _font_h_for(value_font)
      value_y = y + hdr_h + _sx(7.0)
      detail_y = y + hh - detail_h - _sx(7.0)
   }
   _draw_text_ui_small_clipped(label_s, x + _item_pad_x(), label_y, _text_dim_u32, ww - _item_pad_x() * 2.0, clip)
   _draw_text_ui_ex_clipped(value_font,
      value_s,
      x + _item_pad_x(),
      value_y,
      _text_u32,
      0.0,
      ww - _item_pad_x() * 2.0,
   clip)
   if(detail_s.len > 0 && detail_y >= value_y + value_h + _sx(4.0)){
      _draw_text_ui_small_clipped(detail_s,
         x + _item_pad_x(),
         detail_y,
         _text_dim_u32,
         ww - _item_pad_x() * 2.0,
      clip)
   }
   rect
}

fn _begin_canvas_rect(any w, any h, any min_h) list {
   def rect = _layout_rect(_pick_positive(w, _current_body_w()), max(_sx(min_h), float(h)))
   if(!_rect_intersects(rect, _current_clip())){ return [rect, false, 0.0, 0.0, 0.0, 0.0] }
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww = float(rect.get(2, 0.0))
   def hh = float(rect.get(3, 0.0))
   if(ww < 4.0 || hh < 4.0){ return [rect, false, x, y, ww, hh] }
   push_clip_rect(rect)
   [rect, true, x, y, ww, hh]
}

fn plot_lines(any id, any values, any label="", any w=0.0, any h=120.0, any color=0) list {
   "Draws a plot panel placeholder and returns its rectangle."
   def c = _begin_canvas_rect(w, h, 84.0)
   def rect = c.get(0)
   if(!c.get(1, false)){ return rect }
   def x, y, ww, hh = c.get(2), c.get(3), c.get(4), c.get(5)
   _gui_rect_fast(x, y, ww, hh, _window_bg_u32)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), max(1.0, hh - 2.0), _window_body_u32)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), _sx(18.0), _window_hdr_u32)
   _gui_rect_outline_fast(x, y, ww, hh, _border_u32)
   def text_clip = _clip_rect_inset(x, y, ww, hh, 1.0)
   def label_txt = _ui_text(label)
   if(str.len(label_txt) > 0){
      _draw_text_ui_small_clipped(label_txt,
         x + _item_pad_x(),
         y + _sx(4.0),
         _text_u32,
         max(1.0,
         ww - _sx(70.0)),
      text_clip)
   }
   _draw_text_ui_small_clipped("plot disabled",
      x + _item_pad_x(),
      y + _sx(28.0),
      _text_dim_u32,
      max(1.0,
      ww - _sx(16.0)),
   text_clip)
   pop_clip_rect()
   rect
}

fn grid_canvas(any id, any label="", f64 w=0.0, f64 h=220.0, f64 cell=32.0, int major_every=4) list {
   "Draws a grid canvas and returns its rectangle."
   def c = _begin_canvas_rect(w, h, 96.0)
   def rect = c.get(0)
   if(!c.get(1, false)){ return rect }
   def x, y, ww, hh = c.get(2), c.get(3), c.get(4), c.get(5)
   _gui_rect_fast(x, y, ww, hh, _window_bg_u32)
   def step = max(_sx(8.0), float(cell))
   def major_n = max(int(major_every), 1)
   mut gx = x
   mut idx = 0
   while(gx <= x + ww){
      def major_col = ((idx % major_n) == 0) ? _grid_major_u32 : _grid_minor_u32
      _gui_rect_fast(gx, y, 1.0, hh, major_col)
      gx += step
      idx += 1
   }
   mut gy = y
   idx = 0
   while(gy <= y + hh){
      def major_col = ((idx % major_n) == 0) ? _grid_major_u32 : _grid_minor_u32
      _gui_rect_fast(x, gy, ww, 1.0, major_col)
      gy += step
      idx += 1
   }
   _gui_rect_outline_fast(x, y, ww, hh, _border_u32)
   def label_txt = _ui_text(label)
   if(str.len(label_txt) > 0){
      _draw_text_ui_small_clipped(label_txt, x + _item_pad_x(), y + _sx(4.0), _text_dim_u32, ww - _item_pad_x() * 2.0, _clip_rect_inset(x,
            y,
            ww,
            hh,
      1.0))
   }
   pop_clip_rect()
   rect
}

fn _node_canvas_select(list out, int ni, int out_n) list {
   mut sj = 0
   while(sj < out_n){
      mut other = out.get(sj, 0)
      if(is_dict(other)){
         other["selected"] = sj == ni
         out[sj] = other
      }
      sj += 1
   }
   out
}

fn _node_canvas_drag_node(str header_key, bool header_hit, dict node, f64 nx, f64 ny, f64 nw, f64 nh, f64 ww, f64 hh) list {
   mut next_node = node
   mut next_x, next_y = nx, ny
   if(header_hit && _mouse_pressed0){
      _active_widget_id = header_key
      _state_set_num(header_key, "drag_off_x", _mouse_x - nx)
      _state_set_num(header_key, "drag_off_y", _mouse_y - ny)
   }
   if(_active_widget_id == header_key && _mouse_down0 && _mouse_drag_ready()){
      next_x = clamp(_mouse_x - _state_get_num(header_key, "drag_off_x", 0.0), 0.0, max(0.0, ww - nw))
      next_y = clamp(_mouse_y - _state_get_num(header_key, "drag_off_y", 0.0), 0.0, max(0.0, hh - nh))
      next_node["x"] = next_x
      next_node["y"] = next_y
   } elif(_active_widget_id == header_key && !_mouse_down0){
      _active_widget_id = ""
   }
   [next_node, next_x, next_y]
}

fn _node_canvas_draw_shell(dict node, bool selected, f64 hx, f64 hy, f64 nw, f64 nh, f64 header_h, f64 pad) any {
   _gui_rect_fast(hx, hy, nw, nh, selected ? _panel_active_u32 : _window_body_u32)
   _gui_rect_fast(hx, hy, nw, header_h, selected ? _accent_u32 : _window_hdr_u32)
   _gui_rect_outline_fast(hx, hy, nw, nh, selected ? _accent_u32 : _border_u32)
   _draw_text_ui_title_clipped(_ui_text(node.get("title", "Node")), hx + pad, hy + _sx(3.0), _text_u32, nw - pad * 2.0, _clip_rect_inset(hx,
         hy,
         nw,
         nh,
   1.0))
}

fn _node_canvas_draw_input_slots(dict slot_pos, int ni, list ins, f64 hx, f64 hy, f64 nw, f64 nh, f64 header_h, f64 row_h, f64 slot_w, f64 pad) dict {
   def clip = _clip_rect_inset(hx, hy, nw, nh, 1.0)
   mut ii = 0
   def ins_n = ins.len
   while(ii < ins_n){
      def sy = hy + header_h + pad + float(ii) * row_h + _sx(6.0)
      _gui_rect_fast(hx + _sx(6.0), sy, slot_w, slot_w, _warn_u32)
      _draw_text_ui_small_clipped(_ui_text(ins.get(ii, "")), hx + _sx(22.0), sy - _sx(4.0), _text_dim_u32, nw - _sx(44.0), clip)
      slot_pos["in:" + to_str(ni) + ":" + to_str(ii)] = [hx + _sx(11.0), sy + slot_w * 0.5]
      ii += 1
   }
   slot_pos
}

fn _node_canvas_draw_output_slots(dict slot_pos, int ni, list outs, f64 hx, f64 hy, f64 nw, f64 nh, f64 header_h, f64 row_h, f64 slot_w, f64 pad) dict {
   def clip = _clip_rect_inset(hx, hy, nw, nh, 1.0)
   mut oi = 0
   def outs_n = outs.len
   while(oi < outs_n){
      def sy = hy + header_h + pad + float(oi) * row_h + _sx(6.0)
      _gui_rect_fast(hx + nw - _sx(16.0), sy, slot_w, slot_w, _ok_u32)
      def txt = _ui_text(outs.get(oi, ""))
      def mw = _measure_text_ui(_font_small_id(), txt)
      _draw_text_ui_small_clipped(txt, hx + nw - _sx(22.0) - float(mw.get(0, 0.0)), sy - _sx(4.0), _text_dim_u32, nw - _sx(44.0), clip)
      slot_pos["out:" + to_str(ni) + ":" + to_str(oi)] = [hx + nw - _sx(11.0), sy + slot_w * 0.5]
      oi += 1
   }
   slot_pos
}

fn _node_canvas_draw_nodes(str full, list nodes, f64 x0, f64 y0, f64 ww, f64 hh, f64 row_h, f64 header_h, f64 slot_w, f64 pad) list {
   mut slot_pos = dict(64)
   mut out = nodes
   mut ni = 0
   def out_n = out.len
   while(ni < out_n){
      mut node = out.get(ni, 0)
      if(!is_dict(node)){ ni += 1 continue }
      mut nx, ny = float(node.get("x", 20.0)), float(node.get("y", 20.0))
      mut nw = max(_sx(140.0), float(node.get("w", 180.0)))
      def ins = node.get("inputs", [])
      def outs = node.get("outputs", [])
      def rows = max(ins.len, outs.len)
      def nh = max(_sx(70.0), header_h + pad + float(rows) * row_h + pad)
      def hx = x0 + nx
      def hy = y0 + ny
      def header_key = full + "::node:" + to_str(ni)
      def header_hit = _current_window_pointer_hit(hx, hy, nw, header_h)
      def node_hit = _current_window_pointer_hit(hx, hy, nw, nh)
      if(header_hit || (_active_widget_id == header_key && _mouse_down0)){
         _ensure_cursors()
         _want_cursor(_cursor_move)
      }
      if(node_hit && _mouse_pressed0){
         out = _node_canvas_select(out, ni, out_n)
         node = out.get(ni, node)
      }
      def drag_state = _node_canvas_drag_node(header_key, header_hit, node, nx, ny, nw, nh, ww, hh)
      node, nx, ny = drag_state.get(0, node), float(drag_state.get(1, nx)), float(drag_state.get(2, ny))
      out[ni] = node
      def selected = !!out.get(ni, node).get("selected", false)
      _node_canvas_draw_shell(node, selected, hx, hy, nw, nh, header_h, pad)
      slot_pos = _node_canvas_draw_input_slots(slot_pos, ni, ins, hx, hy, nw, nh, header_h, row_h, slot_w, pad)
      slot_pos = _node_canvas_draw_output_slots(slot_pos, ni, outs, hx, hy, nw, nh, header_h, row_h, slot_w, pad)
      ni += 1
   }
   [out, slot_pos]
}

fn _node_canvas_draw_links(any links, any slot_pos) any {
   mut li = 0
   def links_n = links.len
   while(li < links_n){
      def lk = links.get(li, 0)
      mut from_node, from_slot, to_node, to_slot = -1, -1, -1, -1
      if(is_list(lk)){
         from_node = int(lk.get(0, -1))
         from_slot = int(lk.get(1, -1))
         to_node = int(lk.get(2, -1))
         to_slot = int(lk.get(3, -1))
      } elif(is_dict(lk)){
         from_node = int(lk.get("from_node", -1))
         from_slot = int(lk.get("from_slot", -1))
         to_node = int(lk.get("to_node", -1))
         to_slot = int(lk.get("to_slot", -1))
      }
      def p0 = slot_pos.get("out:" + to_str(from_node) + ":" + to_str(from_slot), 0)
      def p1 = slot_pos.get("in:" + to_str(to_node) + ":" + to_str(to_slot), 0)
      if(is_list(p0) && is_list(p1)){
         def x1, y1 = float(p0.get(0, 0.0)), float(p0.get(1, 0.0))
         def x2, y2 = float(p1.get(0, 0.0)), float(p1.get(1, 0.0))
         def mx = x1 + (x2 - x1) * 0.5
         draw_line_2d(x1, y1, mx, y1, _accent_rgba, 2.0)
         draw_line_2d(mx, y1, mx, y2, _accent_rgba, 2.0)
         draw_line_2d(mx, y2, x2, y2, _accent_rgba, 2.0)
      }
      li += 1
   }
}

fn node_canvas(any id, list nodes, list links, f64 w=0.0, f64 h=280.0, f64 cell=32.0) list {
   "Draws an editable node graph and returns the updated node list."
   def full = _current_window_id_cache + "::" + to_str(id)
   def rect = grid_canvas(full + ".grid", "Node Graph", w, h, cell, 4)
   if(!_rect_intersects(rect, _current_clip())){ return nodes }
   push_clip_rect(rect)
   def x0, y0 = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww, hh = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   def row_h, header_h, slot_w, pad = _sx(20.0), _sx(26.0), _sx(10.0), _sx(10.0)
   def node_state = _node_canvas_draw_nodes(full, nodes, x0, y0, ww, hh, row_h, header_h, slot_w, pad)
   def out = node_state.get(0, nodes)
   _node_canvas_draw_links(links, node_state.get(1, dict(0)))
   pop_clip_rect()
   out
}

fn collapsing_header(any id, any label, bool default_open=true) bool {
   "Draws a collapsing header and returns its open state."
   def full = _current_window_id_cache + "::" + to_str(id) + "#header"
   _register_focusable(full)
   mut open = _state_get_bool(full, default_open)
   def rect = _layout_rect(_current_body_w(), _item_h())
   def inter = _begin_interact_widget(full,
      float(rect.get(0,
      0.0)),
      float(rect.get(1,
      0.0)),
      float(rect.get(2,
      0.0)),
      float(rect.get(3,
      0.0)),
   true)
   def focused = _kbd_focus_id == full
   if(inter.get(3, false) || _widget_key_activate(full)){
      open = !open
      _state_set_bool(full, open)
   }
   if(inter.get(0, false)){
      _gui_rect_fast(float(rect.get(0, 0.0)), float(rect.get(1, 0.0)), float(rect.get(2, 0.0)), float(rect.get(3, 0.0)),
      inter.get(2, false) ? _panel_active_u32 : (inter.get(1, false) ? _panel_hover_u32 : _panel_u32))
      _gui_rect_outline_fast(float(rect.get(0,
         0.0)),
         float(rect.get(1,
         0.0)),
         float(rect.get(2,
         0.0)),
         float(rect.get(3,
         0.0)),
      focused ? _accent_u32 : _border_u32)
      def clip = _clip_rect_inset(float(rect.get(0,
         0.0)),
         float(rect.get(1,
         0.0)),
         float(rect.get(2,
         0.0)),
         float(rect.get(3,
         0.0)),
      1.0)
      def text_y = _text_center_y_for(_font_body(), float(rect.get(1, 0.0)), float(rect.get(3, 0.0)))
      _draw_text_ui_clipped(open ? "[-]" : "[+]",
         float(rect.get(0,
         0.0)) + _item_pad_x(),
         text_y,
         _accent_u32,
         _sx(42.0),
      clip)
      _draw_text_ui_clipped(_ui_text(label),
         float(rect.get(0,
         0.0)) + _item_pad_x() + _sx(48.0),
         text_y,
         _text_u32,
         float(rect.get(2,
         0.0)) - _sx(54.0),
      clip)
   }
   open
}

fn _color_rgba4(any rgba) list {
   [
      clamp(float(rgba.get(0, 1.0)), 0.0, 1.0),
      clamp(float(rgba.get(1, 1.0)), 0.0, 1.0),
      clamp(float(rgba.get(2, 1.0)), 0.0, 1.0),
      clamp(float(rgba.get(3, 1.0)), 0.0, 1.0)
   ]
}

fn _color_byte(any v) int { max(0, min(255, int(clamp(float(v), 0.0, 1.0) * 255.0 + 0.5))) }

fn _color_hex_rgb(list rgba) str {
   "#" + str.to_hex(_color_byte(rgba.get(0, 1.0)), 2) +
   str.to_hex(_color_byte(rgba.get(1, 1.0)), 2) +
   str.to_hex(_color_byte(rgba.get(2, 1.0)), 2)
}

fn _color_rgb_to_hsv(list rgba) list {
   def r = clamp(float(rgba.get(0, 1.0)), 0.0, 1.0)
   def g = clamp(float(rgba.get(1, 1.0)), 0.0, 1.0)
   def b = clamp(float(rgba.get(2, 1.0)), 0.0, 1.0)
   def hi = max(r, max(g, b))
   def lo = min(r, min(g, b))
   def d = hi - lo
   mut h = 0.0
   if(d > 0.000001){
      if(hi == r){
         h = (g - b) / d
         if(h < 0.0){ h += 6.0 }
      } elif(hi == g){
         h = ((b - r) / d) + 2.0
      } else {
         h = ((r - g) / d) + 4.0
      }
      h = clamp(h / 6.0, 0.0, 1.0)
   }
   [h, hi <= 0.000001 ? 0.0 : clamp(d / hi, 0.0, 1.0), hi]
}

fn _color_hsv_to_rgb(f64 h, f64 s, f64 v) list {
   h, s, v = clamp(h, 0.0, 1.0), clamp(s, 0.0, 1.0), clamp(v, 0.0, 1.0)
   mut sector = int(h * 6.0)
   if(sector >= 6){ sector = 5 }
   def f = h * 6.0 - float(sector)
   def p = v * (1.0 - s)
   def q = v * (1.0 - s * f)
   def t = v * (1.0 - s * (1.0 - f))
   case sector {
      0 -> { return [v, t, p] }
      1 -> { return [q, v, p] }
      2 -> { return [p, v, t] }
      3 -> { return [p, q, v] }
      4 -> { return [t, p, v] }
      _ -> { return [v, p, q] }
   }
}

fn _color_set_rgb(list rgba, list rgb) list {
   [
      clamp(float(rgb.get(0, rgba.get(0, 1.0))), 0.0, 1.0),
      clamp(float(rgb.get(1, rgba.get(1, 1.0))), 0.0, 1.0),
      clamp(float(rgb.get(2, rgba.get(2, 1.0))), 0.0, 1.0),
      clamp(float(rgba.get(3, 1.0)), 0.0, 1.0)
   ]
}

fn _color_preview_row(str full, any label, list rgba) any {
   def rect = _layout_rect(_current_body_w(), _sx(44.0))
   if(!_rect_intersects(rect, _current_clip())){ return 0 }
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def h = float(rect.get(3, 0.0))
   def sw = min(_sx(64.0), max(_sx(42.0), w * 0.22))
   def clip = _clip_rect_inset(x, y, w, h, 1.0)
   _draw_text_ui_clipped(_ui_text(label), x, y, _text_u32, max(_sx(40.0), w - sw - _gap()), clip)
   _draw_text_ui_small_clipped(_color_hex_rgb(rgba) + "  A " + f"{float(rgba.get(3, 1.0)):.2f}",
   x, y + _text_h() + _sx(3.0), _text_dim_u32, max(_sx(40.0), w - sw - _gap()), clip)
   def px = x + w - sw
   def py = y
   _gui_rect_fast(px, py, sw, h, _panel_u32)
   _gui_rect_fast(px + 1.0, py + 1.0, max(1.0, sw - 2.0), max(1.0, h - 2.0),
   color_pack(float(rgba.get(0, 1.0)), float(rgba.get(1, 1.0)), float(rgba.get(2, 1.0)), float(rgba.get(3, 1.0))))
   _gui_rect_outline_fast(px, py, sw, h, _border_u32)
}

fn _color_sv_plane(str full, list rgba, f64 hue, f64 x, f64 y, f64 w, f64 h) list {
   mut out = rgba
   def hsv = _color_rgb_to_hsv(out)
   mut sat = float(hsv.get(1, 0.0))
   mut val = float(hsv.get(2, 1.0))
   def inter = _begin_interact_widget(full + ".sv", x, y, w, h, true)
   if(inter.get(2, false) || (inter.get(1, false) && _mouse_pressed0)){
      sat = clamp((_mouse_x - x) / max(1.0, w), 0.0, 1.0)
      val = clamp(1.0 - ((_mouse_y - y) / max(1.0, h)), 0.0, 1.0)
      out = _color_set_rgb(out, _color_hsv_to_rgb(hue, sat, val))
   }
   if(inter.get(0, false)){
      def cols = 14
      def rows = 7
      def cw = max(1.0, w / float(cols))
      def ch = max(1.0, h / float(rows))
      mut row = 0
      while(row < rows){
         mut col = 0
         while(col < cols){
            def cs = clamp((float(col) + 0.5) / float(cols), 0.0, 1.0)
            def cv = clamp(1.0 - ((float(row) + 0.5) / float(rows)), 0.0, 1.0)
            def rgb = _color_hsv_to_rgb(hue, cs, cv)
            _gui_rect_fast(x + float(col) * cw, y + float(row) * ch, cw + 1.0, ch + 1.0,
            color_pack(float(rgb.get(0, 1.0)), float(rgb.get(1, 1.0)), float(rgb.get(2, 1.0)), 1.0))
            col += 1
         }
         row += 1
      }
      def cx = x + sat * w
      def cy = y + (1.0 - val) * h
      _gui_rect_outline_fast(cx - _sx(5.0), cy - _sx(5.0), _sx(10.0), _sx(10.0), _window_bg_u32)
      _gui_rect_outline_fast(cx - _sx(4.0), cy - _sx(4.0), _sx(8.0), _sx(8.0), _text_u32)
      _gui_rect_outline_fast(x, y, w, h, inter.get(1, false) ? _accent_u32 : _border_u32)
   }
   out
}

fn _color_hue_strip(str full, list rgba, f64 hue, f64 x, f64 y, f64 w, f64 h) list {
   mut out = rgba
   mut hval = clamp(hue, 0.0, 1.0)
   def hsv = _color_rgb_to_hsv(out)
   def sat = float(hsv.get(1, 0.0))
   def val = float(hsv.get(2, 1.0))
   def inter = _begin_interact_widget(full + ".hue", x, y, w, h, true)
   if(inter.get(2, false) || (inter.get(1, false) && _mouse_pressed0)){
      hval = clamp((_mouse_x - x) / max(1.0, w), 0.0, 1.0)
      out = _color_set_rgb(out, _color_hsv_to_rgb(hval, sat, val))
      _state_set_num(full, "hue", hval)
   }
   if(inter.get(0, false)){
      def segs = 24
      def sw = max(1.0, w / float(segs))
      mut i = 0
      while(i < segs){
         def rgb = _color_hsv_to_rgb(float(i) / float(max(1, segs - 1)), 1.0, 1.0)
         _gui_rect_fast(x + float(i) * sw, y, sw + 1.0, h,
         color_pack(float(rgb.get(0, 1.0)), float(rgb.get(1, 1.0)), float(rgb.get(2, 1.0)), 1.0))
         i += 1
      }
      def mx = x + hval * w
      _gui_rect_fast(mx - _sx(2.0), y - _sx(2.0), _sx(4.0), h + _sx(4.0), _window_bg_u32)
      _gui_rect_fast(mx - _sx(1.0), y - _sx(1.0), _sx(2.0), h + _sx(2.0), _text_u32)
      _gui_rect_outline_fast(x, y, w, h, inter.get(1, false) ? _accent_u32 : _border_u32)
   }
   out
}

fn _color_swatch(str full, int idx, list color, list rgba, f64 x, f64 y, f64 size) list {
   mut out = rgba
   def inter = _begin_interact_widget(full + ".swatch" + to_str(idx), x, y, size, size, true)
   if(inter.get(1, false) && _mouse_pressed0){
      out = [
         clamp(float(color.get(0, 1.0)), 0.0, 1.0),
         clamp(float(color.get(1, 1.0)), 0.0, 1.0),
         clamp(float(color.get(2, 1.0)), 0.0, 1.0),
         clamp(float(rgba.get(3, 1.0)), 0.0, 1.0)
      ]
      def hsv = _color_rgb_to_hsv(out)
      _state_set_num(full, "hue", float(hsv.get(0, 0.0)))
   }
   if(inter.get(0, false)){
      _gui_rect_fast(x, y, size, size,
      color_pack(float(color.get(0, 1.0)), float(color.get(1, 1.0)), float(color.get(2, 1.0)), 1.0))
      _gui_rect_outline_fast(x, y, size, size, inter.get(1, false) ? _accent_u32 : _border_u32)
   }
   out
}

fn _color_swatch_row(str full, list rgba) list {
   mut out = rgba
   def rect = _layout_rect(_current_body_w(), _sx(28.0))
   def presets = [
      [1.0, 1.0, 1.0], [0.0, 0.0, 0.0], [0.90, 0.10, 0.08], [1.0, 0.52, 0.12],
      [0.95, 0.86, 0.20], [0.18, 0.72, 0.28], [0.08, 0.54, 0.88], [0.42, 0.28, 0.86],
      [0.95, 0.40, 0.66], [0.50, 0.50, 0.50]
   ]
   if(!_rect_intersects(rect, _current_clip())){ return out }
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def size = min(_sx(24.0), max(_sx(14.0), (w - _gap() * float(max(0, presets.len - 1))) / float(max(1, presets.len))))
   mut i = 0
   while(i < presets.len){
      out = _color_swatch(full, i, presets.get(i, [1.0, 1.0, 1.0]), out, x + float(i) * (size + _gap()), y, size)
      i += 1
   }
   out
}

fn color_edit4(any id, any label, any rgba) list {
   "Draws RGBA sliders and returns the edited color."
   mut out = _color_rgba4(rgba)
   def full = _current_window_id_cache + "::" + to_str(id) + "#color"
   _color_preview_row(full, label, out)
   out[0] = slider_float(to_str(id) + ".r", "R", out.get(0, 1.0), 0.0, 1.0)
   out[1] = slider_float(to_str(id) + ".g", "G", out.get(1, 1.0), 0.0, 1.0)
   out[2] = slider_float(to_str(id) + ".b", "B", out.get(2, 1.0), 0.0, 1.0)
   out[3] = slider_float(to_str(id) + ".a", "A", out.get(3, 1.0), 0.0, 1.0)
   out
}

fn color_picker4(any id, any label, any rgba) list {
   "Draws an inline SV/hue color picker with swatches and RGBA sliders."
   mut out = _color_rgba4(rgba)
   def full = _current_window_id_cache + "::" + to_str(id) + "#picker"
   def hsv = _color_rgb_to_hsv(out)
   mut hue = _state_get_num(full, "hue", float(hsv.get(0, 0.0)))
   if(float(hsv.get(1, 0.0)) > 0.0005){ hue = float(hsv.get(0, 0.0)) }
   _state_set_num(full, "hue", hue)
   _color_preview_row(full, label, out)
   def plane_h = clamp(_current_body_w() * 0.40, _sx(86.0), _sx(126.0))
   def plane = _layout_rect(_current_body_w(), plane_h)
   out = _color_sv_plane(full, out, hue,
      float(plane.get(0, 0.0)), float(plane.get(1, 0.0)),
   float(plane.get(2, 0.0)), float(plane.get(3, 0.0)))
   def hue_rect = _layout_rect(_current_body_w(), _sx(18.0))
   def hsv2 = _color_rgb_to_hsv(out)
   if(float(hsv2.get(1, 0.0)) > 0.0005){ hue = float(hsv2.get(0, hue)) }
   out = _color_hue_strip(full, out, hue,
      float(hue_rect.get(0, 0.0)), float(hue_rect.get(1, 0.0)),
   float(hue_rect.get(2, 0.0)), float(hue_rect.get(3, 0.0)))
   out = _color_swatch_row(full, out)
   out[0] = slider_float(to_str(id) + ".r", "R", out.get(0, 1.0), 0.0, 1.0)
   out[1] = slider_float(to_str(id) + ".g", "G", out.get(1, 1.0), 0.0, 1.0)
   out[2] = slider_float(to_str(id) + ".b", "B", out.get(2, 1.0), 0.0, 1.0)
   out[3] = slider_float(to_str(id) + ".a", "A", out.get(3, 1.0), 0.0, 1.0)
   def hsv3 = _color_rgb_to_hsv(out)
   if(float(hsv3.get(1, 0.0)) > 0.0005){ _state_set_num(full, "hue", float(hsv3.get(0, hue))) }
   out
}

fn begin_scroll_area(any id, any w=0.0, any h=0.0) bool {
   "Begins a clipped scroll area inside the current window."
   mut win = _current_window_state()
   if(!is_dict(win) || _scroll_area_active){ return false }
   def win_id = _current_window_id_cache
   def full = win_id + "::" + to_str(id)
   def rw = _pick_positive(w, _current_body_w())
   def rh = _pick_positive(h, _sx(220.0))
   def rect = _layout_rect(rw, rh)
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def rwf = float(rect.get(2, 0.0))
   def rhf = float(rect.get(3, 0.0))
   def visible = _rect_intersects(rect, _current_clip())
   def content_h = max(0.0, _state_get_num(full, "content_h", 0.0))
   def inner_h = max(1.0, rhf - _sx(8.0))
   def max_scroll = max(0.0, content_h - inner_h)
   mut scroll_y = clamp(_state_get_num(full, "scroll_y", 0.0), 0.0, max_scroll)
   def hovered = visible && _current_window_pointer_hit(x, y, rwf, rhf)
   if(hovered && abs(_mouse_scroll_dy) > 0.000001){ scroll_y = clamp(scroll_y - _mouse_scroll_dy * _scroll_step(), 0.0, max_scroll) }
   mut area_st = _widget_state.get(full, 0)
   if(!is_dict(area_st)){ area_st = dict(12) }
   area_st["scroll_y"] = scroll_y
   _widget_state[full] = area_st
   _scroll_area_capture_window = win_id
   _scroll_area_capture_rect = rect
   if(visible){
      _gui_rect_fast(x, y, rwf, rhf, _panel_u32)
      _gui_rect_outline_fast(x, y, rwf, rhf, hovered ? _accent_u32 : _border_u32)
   }
   def pad = _sx(8.0)
   def track_w = (max_scroll > 1.0) ? _sx(8.0) : 0.0
   _scroll_area_active = true
   win["cursor_y"] = _current_cursor_y_cache
   _window_store(win)
   _current_window = win
   _scroll_area_parent = clone(win)
   _scroll_area_id = full
   _scroll_area_rect = rect
   _scroll_area_scroll_y = scroll_y
   _scroll_area_start_y = y + pad - scroll_y
   _scroll_area_clip_pushed = visible
   mut child = clone(win)
   child["body_x"] = x + pad
   child["body_y"] = _scroll_area_start_y
   child["body_w"] = max(1.0, rwf - pad * 2.0 - track_w)
   child["body_h"] = inner_h
   child["cursor_y"] = _scroll_area_start_y
   child["clip_override"] = [x + 1.0, y + 1.0, max(1.0, rwf - 2.0), max(1.0, rhf - 2.0)]
   _current_window = child
   _sync_current_layout_cache(child)
   _same_line_next = false
   _same_line_gap = _gap()
   _last_item = [x + pad, _scroll_area_start_y, 0.0, 0.0]
   if(visible){ push_clip_rect([x + 1.0, y + 1.0, max(1.0, rwf - 2.0), max(1.0, rhf - 2.0)]) }
   visible
}

fn set_scroll_area_content_hint(any id, any content_h) bool {
   "Sets the expected content height for the next scroll-area begin in the current window."
   if(!is_dict(_current_window_state())){ return false }
   def full = _current_window_id_cache + "::" + to_str(id)
   _state_set_num(full, "content_h", max(0.0, float(content_h)))
   true
}

fn scroll_area_ensure_visible(any id, any item_top, any item_bottom, any visible_h) bool {
   "Adjusts a scroll area so the requested content span is visible on next begin."
   if(!is_dict(_current_window_state())){ return false }
   def full = _current_window_id_cache + "::" + to_str(id)
   def content_h = max(0.0, _state_get_num(full, "content_h", 0.0))
   def vh = max(1.0, float(visible_h))
   def max_scroll = max(0.0, content_h - vh)
   mut scroll_y = clamp(_state_get_num(full, "scroll_y", 0.0), 0.0, max_scroll)
   def top = max(0.0, float(item_top))
   def bottom = max(top, float(item_bottom))
   if(top < scroll_y){ scroll_y = top }
   elif(bottom > scroll_y + vh){ scroll_y = bottom - vh }
   _state_set_num(full, "scroll_y", clamp(scroll_y, 0.0, max_scroll))
   true
}

fn scroll_area_scroll_y() f64 { "Returns the scroll offset of the active scroll area." _scroll_area_active ? _scroll_area_scroll_y : 0.0 }

fn scroll_area_visible_h() f64 {
   "Returns the visible height of the active scroll area."
   if(!_scroll_area_active){ return 0.0 }
   max(1.0, float(_scroll_area_rect.get(3, 0.0)) - _sx(8.0))
}

fn end_scroll_area() any {
   "Ends the active scroll area and stores its scroll state."
   if(!_scroll_area_active){ return 0 }
   mut child = _current_window_state()
   if(!is_dict(child)){ child = dict(8) }
   if(_scroll_area_clip_pushed){ pop_clip_rect() }
   def rect = _scroll_area_rect
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def rwf = float(rect.get(2, 0.0))
   def rhf = float(rect.get(3, 0.0))
   child["cursor_y"] = _current_cursor_y_cache
   def content_h = max(0.0, _current_cursor_y_cache - _scroll_area_start_y)
   def inner_h = max(1.0, rhf - _sx(8.0))
   def max_scroll = max(0.0, content_h - inner_h)
   mut scroll_y = clamp(_scroll_area_scroll_y, 0.0, max_scroll)
   _state_set_num(_scroll_area_id, "content_h", content_h)
   mut parent = _scroll_area_parent
   if(is_dict(parent)){
      if(_focus_collect_active){
         parent["focusables"] = child.get("focusables", parent.get("focusables", []))
         parent["focusable_map"] = child.get("focusable_map", parent.get("focusable_map", dict(64)))
      }
      _current_window = parent
      _sync_current_layout_cache(parent)
      _same_line_next = false
      _same_line_gap = _gap()
      _last_item = [_current_body_x_cache, _current_cursor_y_cache, 0.0, 0.0]
   } else {
      _current_window = 0
      _sync_current_layout_cache(0)
      _same_line_next = false
      _same_line_gap = _gap()
      _last_item = [0.0, 0.0, 0.0, 0.0]
   }
   if(max_scroll > 1.0){
      def track_w, track_x = _sx(6.0), x + rwf - track_w - _sx(2.0)
      def track_y, track_h = y + _sx(2.0), max(1.0, rhf - _sx(4.0))
      def thumb_h = max(_sx(18.0), track_h * (track_h / max(content_h, 1.0)))
      def usable = max(1.0, track_h - thumb_h)
      mut thumb_y = track_y + usable * ((max_scroll > 0.0) ? (scroll_y / max_scroll) : 0.0)
      def scroll_id = _scroll_area_id + "#scroll"
      def win_id = is_dict(parent) ? to_str(parent.get("id", "")) : ""
      def track_hover = _window_pointer_hit(win_id, track_x, track_y, track_w, track_h)
      def thumb_hover = _window_pointer_hit(win_id, track_x, thumb_y, track_w, thumb_h)
      if(thumb_hover){
         _hot_id = scroll_id
         _hot_rect = [track_x, thumb_y, track_w, thumb_h]
         _ensure_cursors()
         _want_cursor(_cursor_hand)
      }
      if(thumb_hover && _mouse_pressed0){
         _active_widget_id = scroll_id
         _focused_window = win_id
         _state_set_num(scroll_id, "drag_off_y", _mouse_y - thumb_y)
      } elif(track_hover && _mouse_pressed0){
         def want_thumb_y = clamp(_mouse_y - thumb_h * 0.5, track_y, track_y + usable)
         scroll_y = (max_scroll > 0.0) ? (max_scroll * ((want_thumb_y - track_y) / usable)) : 0.0
         thumb_y = track_y + usable * ((max_scroll > 0.0) ? (scroll_y / max_scroll) : 0.0)
      }
      if(_active_widget_id == scroll_id){
         if(_mouse_down0){
            def want_thumb_y = clamp(_mouse_y - _state_get_num(scroll_id, "drag_off_y", 0.0), track_y, track_y + usable)
            scroll_y = (max_scroll > 0.0) ? (max_scroll * ((want_thumb_y - track_y) / usable)) : 0.0
            thumb_y = want_thumb_y
         } else {
            _active_widget_id = ""
         }
      }
      _gui_rect_fast(track_x, track_y, track_w, track_h, _scroll_track_u32)
      _gui_rect_outline_fast(track_x, track_y, track_w, track_h, _border_u32)
      _gui_rect_fast(track_x,
         thumb_y,
         track_w,
         thumb_h,
      (_active_widget_id == scroll_id || thumb_hover) ? _scroll_thumb_hot_u32 : _scroll_thumb_u32)
   }
   _state_set_num(_scroll_area_id, "scroll_y", scroll_y)
   _scroll_area_active = false
   _scroll_area_parent = 0
   _scroll_area_id = ""
   _scroll_area_rect = [0.0, 0.0, 0.0, 0.0]
   _scroll_area_start_y = 0.0
   _scroll_area_scroll_y = 0.0
   _scroll_area_clip_pushed = false
}

fn _draw_window_frame(any id, any title, dict st, bool titlebar, bool collapsed, bool closable, bool resizable, bool collapsible) any {
   def title_h = titlebar ? _title_h() : 0.0
   def sx, sy = float(st.get("x", 0.0)), float(st.get("y", 0.0))
   def sw = float(st.get("w", 0.0))
   def raw_h = float(st.get("h", 0.0))
   def sh = (titlebar && collapsed) ? title_h : max(title_h + 1.0, raw_h)
   def collapse_btn_w = (titlebar && collapsible) ? _sx(22.0) : 0.0
   def close_btn_w = (titlebar && closable) ? _sx(22.0) : 0.0
   def button_left = sx + sw - close_btn_w - collapse_btn_w - _sx(10.0)
   def collapse_x = button_left
   def close_x = sx + sw - close_btn_w - _sx(10.0)
   def collapse_hover = titlebar && collapsible && _window_pointer_hit(id, collapse_x, sy + _sx(5.0), collapse_btn_w, title_h - _sx(10.0))
   def close_hover = titlebar && closable && _window_pointer_hit(id, close_x, sy + _sx(5.0), close_btn_w, title_h - _sx(10.0))
   def resize_hover = resizable && !collapsed && _window_pointer_hit(id, sx + sw - _resize_h(), sy + sh - _resize_h(), _resize_h(), _resize_h())
   _gui_rect_fast(sx + _sx(1.0), sy + _sx(2.0), sw, sh, color_pack(0.0, 0.0, 0.0, 0.20))
   _gui_rect_fast(sx, sy, sw, sh, _window_bg_u32)
   if(!collapsed){
      _gui_rect_fast(sx + 1.0, sy + title_h, max(1.0, sw - 2.0), max(1.0, sh - title_h - 1.0), _window_body_u32)
   }
   _gui_rect_outline_fast(sx, sy, sw, sh, (_window_hovered == to_str(id) || _focused_window == to_str(id)) ? _accent_u32 : _border_u32)
   if(titlebar){
      _gui_rect_fast(sx, sy, sw, title_h, (_focused_window == to_str(id)) ? _accent_soft_u32 : _window_hdr_u32)
      _gui_rect_fast(sx, sy, sw, _sx(2.0), (_focused_window == to_str(id)) ? _accent_u32 : _border_u32)
      def title_w = max(1.0, button_left - (sx + _pad()) - _sx(10.0))
      def title_hh = _font_h_for(_font_title_id())
      def title_y = _snap(float(sy) + max(0.0, (title_h - title_hh) * 0.5))
      _draw_text_ui_title_clipped(to_str(title), sx + _pad(), title_y, _text_u32, title_w, [sx + 1.0, sy + 1.0, max(1.0, title_w), max(1.0, title_h - 2.0)])
   }
   if(titlebar && collapsible){
      _gui_rect_fast(collapse_x, sy + _sx(5.0), collapse_btn_w, title_h - _sx(10.0), collapse_hover ? _panel_hover_u32 : _panel_u32)
      _gui_rect_outline_fast(collapse_x, sy + _sx(5.0), collapse_btn_w, title_h - _sx(10.0), _border_u32)
      _draw_centered_text(collapsed ? "+" : "-", collapse_x, sy + _sx(5.0), collapse_btn_w, title_h - _sx(10.0), _text_u32)
   }
   if(titlebar && closable){
      _gui_rect_fast(close_x, sy + _sx(5.0), close_btn_w, title_h - _sx(10.0), close_hover ? _danger_u32 : _panel_u32)
      _gui_rect_outline_fast(close_x, sy + _sx(5.0), close_btn_w, title_h - _sx(10.0), _border_u32)
      _draw_centered_text("x", close_x, sy + _sx(5.0), close_btn_w, title_h - _sx(10.0), _text_u32)
   }
   if(!collapsed && resizable){
      _gui_rect_fast(sx + sw - _resize_h(), sy + sh - _resize_h(), _resize_h(), _resize_h(), resize_hover ? _panel_hover_u32 : _panel_u32)
      _gui_rect_outline_fast(sx + sw - _resize_h(), sy + sh - _resize_h(), _resize_h(), _resize_h(), _border_u32)
   }
}

fn begin_window(any id, any title, f64 x, f64 y, f64 w, f64 h, any opts=0) bool {
   "Begins a movable GUI window and returns true when its body is visible."
   _theme_refresh()
   _current_window = 0
   _sync_current_layout_cache(0)
   if(!_enabled){ return false }
   mut st = _ensure_window(id, title, x, y, w, h, opts)
   if(!st.get("open", true)){ return false }
   _focus_collect_active = _focus_collect_for_window(id)
   st["clip_override"] = 0
   mut sx, sy = _snap(float(st.get("x", x))), _snap(float(st.get("y", y)))
   mut sw, sh = max(_snap(float(st.get("w", w))), _min_win_w()), max(_snap(float(st.get("h", h))), _min_win_h())
   def closable = st.get("closable", true)
   def movable = st.get("movable", true)
   def resizable = st.get("resizable", true)
   def scrollable = st.get("scrollable", true)
   def titlebar = st.get("titlebar", true)
   def title_h = titlebar ? _title_h() : 0.0
   def collapsible = st.get("collapsible", true)
   mut collapsed = st.get("collapsed", false)
   if(!titlebar || !collapsible){ collapsed = false }
   def body_h = max(1.0, sh - title_h - _pad())
   def max_scroll = scrollable ? max(0.0, float(st.get("content_h", 0.0)) - body_h) : 0.0
   mut scroll_y = clamp(float(st.get("scroll_y", 0.0)), 0.0, max_scroll)
   def whole_hover = _window_pointer_hit(id, sx, sy, sw, collapsed ? title_h : sh)
   if(whole_hover && _mouse_pressed0){
      _focused_window = to_str(id)
      _window_order_touch(id, true)
      if(_active_window_move != to_str(id) && _active_window_resize != to_str(id)){
         _text_focus_id = ""
         _active_widget_id = ""
      }
   }
   def collapse_btn_w = (titlebar && collapsible) ? _sx(22.0) : 0.0
   def close_btn_w = (titlebar && closable) ? _sx(22.0) : 0.0
   def button_left = sx + sw - close_btn_w - collapse_btn_w - _sx(10.0)
   def collapse_x = button_left
   def close_x = sx + sw - close_btn_w - _sx(10.0)
   def title_hover = titlebar && _window_pointer_hit(id, sx, sy, sw, title_h)
   def collapse_hover = titlebar && collapsible && _window_pointer_hit(id, collapse_x, sy + _sx(5.0), collapse_btn_w, title_h - _sx(10.0))
   def close_hover = titlebar && closable && _window_pointer_hit(id, close_x, sy + _sx(5.0), close_btn_w, title_h - _sx(10.0))
   if(movable && title_hover && !collapse_hover && !close_hover){
      _ensure_cursors()
      _want_cursor(_cursor_move)
   }
   if(close_hover && _mouse_pressed0){
      st["open"] = false
      _window_store(st)
      _window_closed_events[to_str(id)] = true
      if(_focused_window == to_str(id)){ _focused_window = "" }
      _active_widget_id = ""
      _active_window_move = ""
      _active_window_resize = ""
      _text_focus_id = ""
      _kbd_focus_id = ""
      return false
   }
   if(collapse_hover && _mouse_pressed0){
      collapsed = !collapsed
      st["collapsed"] = collapsed
      if(collapsed){ scroll_y = 0.0 }
   }
   if(movable && title_hover && !_mouse_pressed0 && _active_window_move == to_str(id) && !_mouse_down0){ _active_window_move = "" }
   if(movable && title_hover && _mouse_pressed0 && !collapse_hover && !close_hover){
      _active_window_move = to_str(id)
      _move_off_x = _mouse_x - sx
      _move_off_y = _mouse_y - sy
   }
   if(_active_window_move == to_str(id)&& _mouse_down0 && _mouse_drag_ready()){ sx, sy = _snap(_mouse_x - _move_off_x), _snap(_mouse_y - _move_off_y) }
   def resize_x, resize_y = sx + sw - _resize_h(), sy + sh - _resize_h()
   def resize_hover = resizable && !collapsed && _window_pointer_hit(id, resize_x, resize_y, _resize_h(), _resize_h())
   if(resize_hover || (_active_window_resize == to_str(id) && _mouse_down0)){
      _ensure_cursors()
      _want_cursor(_cursor_resize)
   }
   if(resizable && resize_hover && _mouse_pressed0){
      _active_window_resize = to_str(id)
      _resize_pad_x = sx + sw - _mouse_x
      _resize_pad_y = sy + sh - _mouse_y
   }
   if(_active_window_resize == to_str(id) && !_mouse_down0){ _active_window_resize = "" }
   if(_active_window_resize == to_str(id)&& _mouse_down0 && _mouse_drag_ready()){ sw, sh = max(_min_win_w(), _snap(_mouse_x - sx + _resize_pad_x)), max(_min_win_h(), _snap(_mouse_y - sy + _resize_pad_y)) }
   st["x"] = sx st["y"] = sy
   st["w"] = sw st["h"] = sh
   st, sx = _clamp_window_pos(st), float(st.get("x", sx))
   sy, sw = float(st.get("y", sy)), float(st.get("w", sw))
   sh = float(st.get("h", sh))
   if(scrollable && whole_hover && abs(_mouse_scroll_dy) > 0.000001 && !collapsed && !_pointer_over_scroll_area(id)){ scroll_y = clamp(scroll_y - _mouse_scroll_dy * _scroll_step(), 0.0, max_scroll) }
   if(scrollable
      && !collapsed
      && _focused_window == to_str(id)
      && _text_focus_id == ""
      && _active_window_move != to_str(id)
      && _active_window_resize != to_str(id)){
      def page_step = max(_sx(36.0), body_h * 0.90)
      if(_key_event_present(KEY_PAGE_UP)){ scroll_y = clamp(scroll_y - page_step, 0.0, max_scroll) }
      if(_key_event_present(KEY_PAGE_DOWN)){ scroll_y = clamp(scroll_y + page_step, 0.0, max_scroll) }
      if(_key_event_present(KEY_HOME)){ scroll_y = 0.0 }
      if(_key_event_present(KEY_END)){ scroll_y = max_scroll }
   }
   st["scroll_y"] = scroll_y
   _draw_window_frame(id, title, st, titlebar, collapsed, closable, resizable, collapsible)
   def origin = _window_content_origin(st)
   st["body_x"] = float(origin.get(0, sx + _pad()))
   st["body_y"] = float(origin.get(1, sy + title_h + _pad()))
   st["body_w"] = max(1.0, sw - _pad() * 2.0)
   st["body_h"] = body_h
   st["cursor_y"] = float(origin.get(1, sy + title_h + _pad()))
   if(_focus_collect_active){ st["focusables"] = [] st["focusable_map"] = dict(64) }
   def needs_clip = !collapsed && (!scrollable || max_scroll > 0.5 || scroll_y > 0.0)
   st["clip_active"] = needs_clip
   _same_line_next = false
   _same_line_gap = _gap()
   _window_store(st)
   _current_window = st
   _sync_current_layout_cache(st)
   _last_item = [_current_body_x_cache, _current_cursor_y_cache, 0.0, 0.0]
   if(needs_clip){ push_clip_rect(_window_body_clip(st)) }
   !collapsed
}

fn end_window() any {
   "Ends the current GUI window and stores layout metrics."
   mut st = _current_window_state()
   if(!is_dict(st)){ return 0 }
   def win_id = _current_window_id_cache
   if(st.get("clip_active", false)){ pop_clip_rect() }
   if(st.get("collapsed", false)){
      _window_store(st)
      _current_window = 0
      _sync_current_layout_cache(0)
      _focus_collect_active = false
      _same_line_next = false
      return 0
   }
   def sy, sh = float(st.get("y", 0.0)), float(st.get("h", 0.0))
   def start_y = sy + _title_h() + _pad()
   st["cursor_y"] = _current_cursor_y_cache
   mut content_h = _current_cursor_y_cache - start_y + float(st.get("scroll_y", 0.0))
   if(content_h < 0.0){ content_h = 0.0 }
   st["content_h"] = content_h
   def scrollable = st.get("scrollable", true)
   def max_scroll = scrollable ? max(0.0, content_h - max(1.0, sh - _title_h() - _pad())) : 0.0
   mut scroll_y = clamp(float(st.get("scroll_y", 0.0)), 0.0, max_scroll)
   st["scroll_y"] = scroll_y
   if(scrollable && max_scroll > 1.0){
      def track_w = _sx(6.0)
      def bar_h = max(_sx(18.0), (sh - _title_h()) * ((sh - _title_h()) / max(content_h, 1.0)))
      def track_x = float(st.get("x", 0.0)) + float(st.get("w", 0.0)) - track_w - _sx(2.0)
      def track_y = sy + _title_h()
      def track_h = sh - _title_h()
      def avail = max(1.0, track_h - bar_h)
      def t = (max_scroll > 0.0) ? (scroll_y / max_scroll) : 0.0
      mut bar_y = track_y + avail * t
      def scroll_id = win_id + "::scroll"
      def track_hover = _window_pointer_hit(win_id, track_x, track_y, track_w, track_h)
      def thumb_hover = _window_pointer_hit(win_id, track_x, bar_y, track_w, bar_h)
      if(thumb_hover){
         _hot_id = scroll_id
         _hot_rect = [track_x, bar_y, track_w, bar_h]
         _ensure_cursors()
         _want_cursor(_cursor_hand)
      }
      if(thumb_hover && _mouse_pressed0){
         _active_widget_id = scroll_id
         _focused_window = win_id
         _state_set_num(scroll_id, "drag_off_y", _mouse_y - bar_y)
      } elif(track_hover && _mouse_pressed0){
         def want_bar_y = clamp(_mouse_y - bar_h * 0.5, track_y, track_y + avail)
         scroll_y = (max_scroll > 0.0) ? (max_scroll * ((want_bar_y - track_y) / avail)) : 0.0
         st["scroll_y"] = scroll_y
         bar_y = want_bar_y
      }
      if(_active_widget_id == scroll_id){
         if(_mouse_down0){
            def want_bar_y = clamp(_mouse_y - _state_get_num(scroll_id, "drag_off_y", 0.0), track_y, track_y + avail)
            scroll_y = (max_scroll > 0.0) ? (max_scroll * ((want_bar_y - track_y) / avail)) : 0.0
            st["scroll_y"] = scroll_y
            bar_y = want_bar_y
         } else {
            _active_widget_id = ""
         }
      }
      _gui_rect_fast(track_x, track_y, track_w, track_h, _scroll_track_u32)
      _gui_rect_fast(track_x,
         bar_y,
         track_w,
         bar_h,
      (_active_widget_id == scroll_id || thumb_hover) ? _scroll_thumb_hot_u32 : _scroll_thumb_u32)
   }
   _window_store(st)
   _current_window = 0
   _sync_current_layout_cache(0)
   _focus_collect_active = false
   _same_line_next = false
}

#main {
   set_backend_type(BACKEND_MOCK)
   assert(is_dict(init_mock_surface(320, 240)), "gui mock surface")
   reset_state()
   assert(enabled(), "gui enabled default")
   set_enabled(true)
   set_scale(1.25)
   assert(scale() == 1.25, "gui scale")
   set_accent([0.20, 0.40, 0.60, 0.80])
   assert(accent().get(2, 0.0) == 0.60, "gui accent")
   set_debug_overlay(true)
   assert(debug_overlay(), "gui debug overlay on")
   set_debug_overlay(false)
   assert(!debug_overlay(), "gui debug overlay off")
   assert(rect(1.0, 2.0, 3.0, 4.0) == [1.0, 2.0, 3.0, 4.0], "gui rect")
   assert(inset([0.0, 0.0, 10.0, 10.0], 2.0) == [2.0, 2.0, 6.0, 6.0], "gui inset")
   assert(split_cols([0.0, 0.0, 100.0, 20.0], [1.0, 1.0], 4.0).len == 2, "gui split cols")
   assert(split_rows([0.0, 0.0, 20.0, 100.0], [1.0, 1.0], 4.0).len == 2, "gui split rows")
   apply_window_rect("probe", [10.0, 12.0, 260.0, 120.0])
   assert(window_visible("probe"), "gui window visible")
   set_window_pos("probe", 14.0, 18.0)
   set_window_size("probe", 250.0, 130.0)
   def stored = window_rect("probe")
   assert(stored.get(0, 0.0) == 14.0 && stored.get(1, 0.0) == 18.0 && stored.get(2, 0.0) >= 240.0, "gui stored window rect")
   show_window("probe", false)
   assert(!window_visible("probe"), "gui hide window")
   show_window("probe", true)
   assert(focus_window("probe") && request_focus("probe::icon"), "gui focus request")
   clear_focus()
   assert(focused_id() == "", "gui clear focus")
   prepare_input(0, 320.0, 240.0)
   begin_frame()
   assert(begin_window("probe", "Probe", 8.0, 8.0, 280.0, 210.0, {"closable": false, "resizable": false}), "gui begin window")
   assert(remaining_h() > 0.0, "gui remaining height")
   assert(!icon_button("icon", -1, "Run", 72.0, 32.0, false), "gui icon button idle")
   assert(!selectable("row", "Row", false, 180.0, 34.0, "Detail", -1), "gui selectable idle")
   assert(tab_strip("tabs", ["One", "Two"], 1, 180.0, 30.0) == 1, "gui tab strip")
   assert(slider_float("gain", "Gain", 0.40, 0.0, 1.0, 180.0) == 0.40, "gui slider")
   def grid = grid_canvas("grid", "Grid", 180.0, 84.0, 16.0, 2)
   assert(grid.get(2, 0.0) > 0.0 && grid.get(3, 0.0) > 0.0, "gui grid canvas")
   def nodes = [
      {"title": "Source", "x": 12.0, "y": 14.0, "inputs": [], "outputs": ["out"]},
      {"title": "Sink", "x": 120.0, "y": 34.0, "inputs": ["in"], "outputs": []}
   ]
   def next_nodes = node_canvas("nodes", nodes, [[0, 0, 1, 0]], 190.0, 110.0, 16.0)
   assert(is_list(next_nodes) && next_nodes.len == 2, "gui node canvas")
   end_window()
   end_frame()
   close_window()
   print("✓ std.os.ui.render.viewer.gui self-test passed")
}
