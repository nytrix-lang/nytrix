;; Keywords: ui gui immediate-mode widgets layout input controls panels colorpicker os render viewer
;; Immediate-mode GUI widgets, layout state, input routing, and draw integration.
;; References:
;; - core.os.ui.render.viewer.batch
;; - core.os.ui.window
module core.os.ui.render.viewer.gui(paint_rect, paint_text, item_hovered, mouse_xy, remaining_w, set_enabled, enabled, set_scale, scale, set_fonts, set_accent, accent, theme_color, theme_color_alpha, set_theme_color, generate_palette, apply_palette, button_style_map, export_theme, save_theme_file, load_theme_file, watch_theme_file, unwatch_theme_file, poll_theme_file, theme_editor, set_debug_overlay, debug_overlay, rect, inset, center_rect, clamp_rect, split_cols, split_rows, apply_window_rect, apply_window_rect_if_visible, tile_editor_shell, tile_editor_shell_preset, prepare_input, feed_event, key_pressed, begin_frame, end_frame, reset_state, accessibility_tree, accessibility_action, animate_value, animation_busy, overlay_offset, list_view, dialog_open, dialog_close, dialog_is_open, message_dialog, text_input_dialog, file_dialog, icon_pack_load, icon_pack_save, icon_pack_clear, icon_pack_count, icon_pack_glyph, icon_pack_text, icon_pack_define, wants_mouse, wants_keyboard, hit_test, suppress_mouse_clicks, hovered_id, active_id, focused_id, mouse_down, mouse_press_seq, last_item_rect, focus_window, request_focus, clear_focus, show_window, window_visible, window_closed, set_window_pos, set_window_size, window_rect, reset_window_scroll, warm_text_pipeline, begin_window, end_window, begin_scroll_area, set_scroll_area_content_hint, scroll_area_ensure_visible, end_scroll_area, scroll_area_scroll_y, scroll_area_visible_h, push_clip_rect, pop_clip_rect, same_line, spacing, spacer_px, reset_cursor, layout_gap, remaining_h, text, label, text_colored, property_row, title_input_text, button, button_at, draw_flex_commands, measure_flex_text, small_button, image, icon_button, selectable, selectable_file, input_text, checkbox, toggle, radio_button, tab_strip, tab_strip_slot, combo_box, slider_float, slider_int, progress_bar, stat_card, plot_lines, grid_canvas, node_canvas, collapsing_header, color_edit4, color_picker4, set_headless_input, set_theme, theme, push_id, pop_id, indent, unindent, layout_row, begin_columns, next_column, end_columns, begin_disabled, end_disabled, item_status, tooltip, set_tooltip_delay, open_popup, close_popup, close_all_popups, popup_open, any_popup_open, begin_popup, end_popup, begin_menu_bar, end_menu_bar, begin_menu, end_menu, menu_item, menu_separator, begin_context_menu, tree_node, tree_pop, drag_float, drag_int, begin_table, table_next_row, table_next_column, table_text, table_row_clicked, table_sort_column, table_sort_descending, end_table, drag_source, drop_target, drag_active, last_item_id, dockspace, dock_window, undock_window, window_docked, dock_windows, dock_split_ratio, dock_layout_save, dock_layout_load, dock_spacer_rect, panel_state)
use core
use core.os.ui.anim as ui_anim
use core.math
use core.os.ui.window.consts
use core.os.ui.render
use core.os.ui.window as uiw
use core.os.ui.render.dump as ui_profile
use core.os.ui.render.viewer.batch as gui_batch
use core.str as str
use core.common as common
use core.cache as cache
use core.os (file_read, file_write)
use core.os.fs as fs
use core.os.path as ospath
use core.math.parse.data.json as json

mut bool _enabled = true
mut f64 _scale = 1.0
mut any _win = nil

;; Separate typed globals: as one multi-assigned declaration the font ids
;; were read back tagged in compiled executables (font 3 became 7, an
;; unknown font, so text fell back to the bitmap font).
mut int _font = 0
mut int _font_title = 0
mut int _font_small = 0
mut _fb_w, _fb_h = 0.0, 0.0
mut _mouse_x, _mouse_y = 0.0, 0.0
mut _mouse_down0, _mouse_down0_prev = false, false
mut _mouse_pressed0, _mouse_released0 = false, false
mut _mouse_press_x, _mouse_press_y = 0.0, 0.0
mut int _mouse_press_seq = 0
mut _mouse_scroll_dx, _mouse_scroll_dy = 0.0, 0.0
mut _last_scroll_x, _last_scroll_y = 0.0, 0.0
mut _event_mouse_has_pos, _event_mouse_down0_known = false, false

;; A press and its release fed before one prepare_input (a fast click inside
;; one compositor frame) would cancel out; the release is held for a frame.
mut bool _defer_release0 = false
mut bool _apply_release0 = false
mut _event_mouse_x, _event_mouse_y = 0.0, 0.0
mut _event_mouse_down0, _event_mouse_pressed0, _event_mouse_released0 = false, false, false
mut _suppress_mouse_frames, _suppress_mouse_until_up = 0, false
mut _event_scroll_dx, _event_scroll_dy = 0.0, 0.0
mut _key_events = []
mut _key_mask_0, _key_mask_1, _key_mask_2, _key_mask_3 = 0, 0, 0, 0
mut _key_mask_4, _key_mask_5, _key_mask_6, _key_mask_7 = 0, 0, 0, 0
mut _text_events = ""

;; Input-method composition for the focused text field (EVENT_TEXT_PREEDIT /
;; EVENT_TEXT_DELETE_SURROUNDING from the window backend). The preedit is
;; shown at the caret but is not part of the value until it is committed.
mut _ime_preedit = ""
mut _ime_preedit_cursor = -1
mut _ime_delete_before = 0
mut _ime_delete_after = 0
mut _ime_claimed = false
mut _ime_active = false
mut dict _windows = dict(32)
mut dict _widget_state = dict(64)
mut any _current_window = nil
mut _current_window_id_cache, _widget_id_prefix = "", ""
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
mut dict _window_closed_events = dict(32)
mut _popup_hit_active, _popup_next_hit_active = false, false
mut _popup_hit_window, _popup_next_hit_window = "", ""
mut _popup_hit_x, _popup_hit_y, _popup_hit_w, _popup_hit_h = 0.0, 0.0, 0.0, 0.0
mut _popup_next_hit_x, _popup_next_hit_y, _popup_next_hit_w, _popup_next_hit_h = 0.0, 0.0, 0.0, 0.0
mut _popup_combo_active = false
mut _popup_combo_key = ""
mut _popup_combo_x, _popup_combo_y, _popup_combo_w, _popup_combo_h = 0.0, 0.0, 0.0, 0.0
mut _popup_combo_row_h, _popup_combo_max_scroll, _popup_combo_scroll_y = 0.0, 0.0, 0.0
mut _popup_combo_rows, _popup_combo_first = 0, 0
mut any _popup_combo_out = nil
mut _popup_combo_labels = []
mut any _popup_combo_style = nil
mut _move_off_x, _move_off_y = 0.0, 0.0
mut _resize_pad_x, _resize_pad_y = 0.0, 0.0
mut _cursor_arrow, _cursor_ibeam, _cursor_hand = 0, 0, 0
mut _cursor_resize, _cursor_move, _cursor_active, _cursor_want = 0, 0, 0, 0
mut _wants_mouse, _wants_keyboard, _debug_overlay = false, false, false
mut _last_item = [0.0, 0.0, 0.0, 0.0]
mut _hot_rect = [0.0, 0.0, 0.0, 0.0]
mut _active_rect = [0.0, 0.0, 0.0, 0.0]
mut _iw_visible, _iw_hovered, _iw_held, _iw_clicked, _iw_released = false, false, false, false, false
mut _same_line_next, _same_line_gap = false, 0.0
mut _scroll_area_active, _scroll_area_id = false, ""

;; Declared `any` on its own: as an untyped slot of a multi-assignment the saved
;; parent no longer tested as a dict when the area ended, so every widget after
;; a scroll area was laid out at a zero rectangle and end_window was skipped.
mut any _scroll_area_parent = nil
mut _scroll_area_rect = [0.0, 0.0, 0.0, 0.0]
mut _scroll_area_start_y, _scroll_area_scroll_y = 0.0, 0.0
mut _scroll_area_clip_pushed, _scroll_area_capture_window = false, ""
mut _scroll_area_capture_rect = [0.0, 0.0, 0.0, 0.0]
mut _text_fit_cache = dict(512)
mut _accessibility_nodes = []
mut _accessibility_requests = dict(16)
mut int _accessibility_request_count = 0
def _A11Y_EMPTY = dict(0)
mut _ui_animator = ui_anim.animator()

;; Toolkit layer state: overlay command list, popups, layout scopes, drag and drop.
mut _ov_cmds = []
mut _ov_stack = []
mut _ov_pending = []
mut _ov_top = []
mut _win_layers = []
mut _dock_frame_active = false
mut any _dock_spaces = nil
mut _ov_depth, _ov_discard = 0, 0
mut _popup_stack = []
mut _popup_dismissed = []
mut _popup_depth = 0
mut _ctx_stack = []
mut _menu_measure_w = 0.0
mut _menubar_active, _menubar_open = false, ""
mut _menubar_x, _menubar_y, _menubar_h = 0.0, 0.0, 0.0
mut _headless_input = false

;; Button state as last reported by fed events. Once an event has reported the
;; primary button it is authoritative until focus is lost: polling the window
;; between a fed press and its release dropped scripted drags on real
;; backends, where injected events do not change the polled button state.
mut _fed_down0, _fed_down0_valid = false, false
mut _fed_down0_stale = 0
mut _mouse_pressed1, _event_mouse_pressed1 = false, false
mut _disabled_depth, _disabled_on = 0, 0
mut _dis_text_u32, _dis_dim_u32, _dis_accent_u32 = 0, 0, 0
mut _id_stack = []
mut _indent_stack = []
mut _row_cells = []
mut _row_n, _row_i = 0, 0
mut _row_y, _row_bottom = 0.0, 0.0
mut _col_stack = []
mut _tip_x, _tip_y = -1.0, -1.0
mut _tip_frames, _tip_seen, _tip_delay = 0, -1, 30
mut _tip_text = ""
mut _last_item_id = ""
mut _dnd_active, _dnd_type, _dnd_label, _dnd_source = false, "", "", ""
mut any _dnd_payload = nil
mut _tbl_id = ""
mut _tbl_style = nil
mut _tbl_cols = []
mut _tbl_n, _tbl_col, _tbl_row = 0, -1, -1
mut _tbl_x, _tbl_w, _tbl_row_y, _tbl_row_bottom, _tbl_row_h = 0.0, 0.0, 0.0, 0.0, 0.0
mut _tbl_base_x, _tbl_base_w = 0.0, 0.0
mut _tbl_row_hit = false
mut _theme_name = "dark"
mut _theme_button_styles = dict(0)
mut _theme_file_path = ""
mut _theme_file_snapshot = ""
mut int _theme_file_poll_frame = -1
mut _text_measure_cache = dict(1024)
mut _font_h_cache = dict(16)
mut _text_caches_ready = false
mut _gui_frame_id = 0
mut _overlay_state = nil
mut _overlay_offsets = nil
mut _gui_view_w = 0.0
mut _gui_view_h = 0.0
mut _dialogs = nil
mut _icon_pack = nil
mut _font_metric_frame = -1
mut _cached_body_h, _cached_title_h, _cached_small_h = 0.0, 0.0, 0.0

;; Queued text, already in the layout draw_text_runs_flat_colors takes:
;; `_text_run_queue` holds the runs (text, x, y, color) of the latest font,
;; `_text_run_font`; runs of earlier fonts wait in `_text_run_segments`
;; (font, list pairs) so the queue order is kept. Spare lists are reused
;; across frames. (Queuing five mixed values per run and re-packing them per
;; font at flush read and classified each value twice.)
mut _text_run_queue = []
mut int _text_run_font = -1
mut _text_run_segments = []
mut _text_run_spare = []
def int _TEXT_SELECT_ALL = -9001
def int _TEXT_COPY = -9002
def int _TEXT_CUT = -9003
def int _TEXT_PASTE = -9004
mut _theme_dirty, _theme_ready = true, false
mut _accent_rgba = [0.714, 0.627, 1.000, 1.00]
mut _press_rgba = [0.290, 0.243, 0.447, 1.00]
mut _window_bg_rgba = [0.020, 0.020, 0.024, 1.00]
mut _window_body_rgba = [0.031, 0.031, 0.039, 0.98]
mut _window_hdr_rgba = [0.055, 0.055, 0.067, 1.00]
mut _panel_rgba = [0.067, 0.067, 0.082, 1.00]
mut _panel_hover_rgba = [0.102, 0.102, 0.125, 1.00]
mut _panel_active_rgba = [0.141, 0.141, 0.173, 1.00]
mut _popup_rgba = [0.047, 0.047, 0.059, 1.00]

fn _ensure_text_caches() any {
   if _text_caches_ready { return 0 }
   if !is_dict(_text_fit_cache) { _text_fit_cache = dict(512) }
   if !is_dict(_text_measure_cache) { _text_measure_cache = dict(1024) }
   if !is_dict(_font_h_cache) { _font_h_cache = dict(16) }
   _text_caches_ready = true
}

fn _ensure_gui_state() any {
   if !is_dict(_windows) { _windows = dict(32) }
   if !is_dict(_widget_state) { _widget_state = dict(64) }
   if !is_dict(_window_closed_events) { _window_closed_events = dict(32) }
   if !is_list(_window_order) { _window_order = [] }
   if !is_dict(_dock_spaces) { _dock_spaces = dict(4) }
   if !is_dict(_overlay_state) { _overlay_state = dict(16) }
   if !is_dict(_overlay_offsets) { _overlay_offsets = dict(16) }
   if !is_dict(_dialogs) { _dialogs = dict(8) }
   if !is_dict(_icon_pack) { _icon_pack = dict(64) }
}

fn _ui_text(any txt) str {
   if is_str(txt) { return txt }
   if txt == nil { return "" }
   if is_int(txt) || is_float(txt) || is_bool(txt) { return to_str(txt) }
   if is_list(txt) { return "[" + to_str(txt.len) + "]" }
   if is_dict(txt) { return "{dict}" }
   def s = to_str(txt)
   is_str(s) ? s : ""
}

mut _border_rgba = [0.149, 0.149, 0.180, 1.00]
mut _text_rgba = [0.941, 0.941, 0.953, 1.00]
mut _text_dim_rgba = [0.643, 0.643, 0.682, 1.00]
mut _ok_rgba = [0.545, 0.863, 0.604, 1.00]
mut _warn_rgba = [0.949, 0.784, 0.475, 1.00]
mut _danger_rgba = [1.000, 0.478, 0.420, 1.00]
mut _text_faint_rgba = [0.435, 0.435, 0.478, 1.00]
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
mut _text_faint_u32 = 0
mut _select_u32 = 0
mut _focus_u32 = 0
mut _shadow_u32 = 0

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
def _M_GAP = 3.0
def _M_TINY_GAP = 2.0
def _M_TITLE_H = 26.0
def _M_ITEM_PAD_X = 8.0
def _M_ITEM_PAD_Y = 2.0
def _M_SLIDER_H = 32.0
def _M_TOGGLE_W = 38.0
def _M_TOGGLE_H = 18.0
def _M_RESIZE_H = 12.0
def _M_MIN_WIN_W = 220.0
def _M_MIN_WIN_H = 84.0
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
fn _snap(f64 v) f64 {
   "Rounds UI geometry to the nearest pixel without truncation drift."
   v >= 0.0 ? float(int(v + 0.5)) : float(int(v - 0.5))
}

fn _clear_pending_input_events() any {
   _event_mouse_has_pos, _event_mouse_down0_known, _event_mouse_down0 = false, false, false
   _event_mouse_pressed0, _event_mouse_released0 = false, false
   _event_scroll_dx, _event_scroll_dy = 0.0, 0.0
   _event_mouse_pressed1 = false
}

fn _clear_key_masks() any {
   _key_mask_0, _key_mask_1, _key_mask_2, _key_mask_3 = 0, 0, 0, 0
   _key_mask_4, _key_mask_5, _key_mask_6, _key_mask_7 = 0, 0, 0, 0
}

fn _key_mask_set(int key) any {
   if key < 0 || key >= 512 { return 0 }
   def bit = 1 << (key & 63)
   case key / 64 {
      0 -> { _key_mask_0 = _key_mask_0 | bit }
      1 -> { _key_mask_1 = _key_mask_1 | bit }
      2 -> { _key_mask_2 = _key_mask_2 | bit }
      3 -> { _key_mask_3 = _key_mask_3 | bit }
      4 -> { _key_mask_4 = _key_mask_4 | bit }
      5 -> { _key_mask_5 = _key_mask_5 | bit }
      6 -> { _key_mask_6 = _key_mask_6 | bit }
      7 -> { _key_mask_7 = _key_mask_7 | bit }
      _ -> 0
   }
}

fn _key_mask_has(int key) bool {
   if key < 0 || key >= 512 { return false }
   def bit = 1 << (key & 63)
   case key / 64 {
      0 -> (_key_mask_0 & bit) != 0
      1 -> (_key_mask_1 & bit) != 0
      2 -> (_key_mask_2 & bit) != 0
      3 -> (_key_mask_3 & bit) != 0
      4 -> (_key_mask_4 & bit) != 0
      5 -> (_key_mask_5 & bit) != 0
      6 -> (_key_mask_6 & bit) != 0
      7 -> (_key_mask_7 & bit) != 0
      _ -> false
   }
}

fn suppress_mouse_clicks(int frames=3, bool until_up=true) any {
   "Suppresses mouse click transitions for a few frames after focus or modal changes."
   mut n = int(frames)
   if n < 1 { n = 1 }
   if n > _suppress_mouse_frames { _suppress_mouse_frames = n }
   if until_up { _suppress_mouse_until_up = true }
   _event_mouse_pressed0, _event_mouse_released0 = false, false
   _event_mouse_down0_known = false
}

@inline
@jit
fn _pick_positive(any v, f64 fallback) f64 {
   def fv = float(v)
   if fv > 0.0 { return fv }
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
   if !_win { return 0 }
   if !_cursor_arrow { _cursor_arrow = uiw.create_standard_cursor(uiw.ARROW_CURSOR) }
   if !_cursor_ibeam { _cursor_ibeam = uiw.create_standard_cursor(uiw.IBEAM_CURSOR) }
   if !_cursor_hand { _cursor_hand = uiw.create_standard_cursor(uiw.POINTING_HAND_CURSOR) }
   if !_cursor_resize { _cursor_resize = uiw.create_standard_cursor(uiw.RESIZE_NWSE_CURSOR) }
   if !_cursor_move { _cursor_move = uiw.create_standard_cursor(uiw.RESIZE_ALL_CURSOR) }
}

fn _want_cursor(any cursor) any { _cursor_want = cursor }

fn _apply_cursor() any {
   if !_win { return 0 }
   _ensure_cursors()
   def want = common.value_or(_cursor_want, _cursor_arrow)
   if want != _cursor_active {
      uiw.set_cursor(_win, want)
      _cursor_active = want
   }
}

fn _theme_pack(list c) int { color_pack(float(c.get(0, 1.0)), float(c.get(1, 1.0)), float(c.get(2, 1.0)), float(c.get(3, 1.0))) }

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
   while i < weights_n {
      total += max(0.0, float(weights.get(i, 0.0)))
      i += 1
   }
   if total > 0.000001 { return total }
   1.0
}

fn _weight_or(list weights, int idx, f64 fallback) f64 {
   def value = float(weights.get(idx, fallback))
   if value > 0.000001 { return value }
   float(fallback)
}

fn _center_row_weights(f64 top_ratio, f64 mid_ratio, list weights) list {
   if weights.len >= 3 { return [_weight_or(weights, 0, 0.34), _weight_or(weights, 1, 0.24), _weight_or(weights, 2, 0.42)] }
   def top = max(0.12, float(top_ratio))
   def mid = max(0.12, float(mid_ratio))
   [top, mid, max(0.26, 1.0 - top - mid)]
}

fn _right_stack_weights(list weights) list {
   if weights.len >= 4 { return [_weight_or(weights, 0, 1.12), _weight_or(weights, 1, 0.92), _weight_or(weights, 2, 0.78), _weight_or(weights, 3, 0.88)] }
   if weights.len == 3 { return [_weight_or(weights, 0, 1.12), _weight_or(weights, 1, 0.92), _weight_or(weights, 2, 0.78), 0.88] }
   if weights.len == 2 { return [_weight_or(weights, 0, 1.12), _weight_or(weights, 1, 0.92), 0.78, 0.88] }
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
   while i < n {
      def wt = max(0.0, float(weights.get(i, 1.0)))
      mut part = inner * (wt / total)
      if i == n - 1 { part = max(0.0, (vertical ? y0 : x0) + full - pos) }
      out = out.append(vertical ? [x0, pos, ww, max(0.0, part)] : [pos, y0, max(0.0, part), hh])
      pos += part + g
      i += 1
   }
   out
}

fn split_cols(list r, list weights, f64 gap=0.0) list { "Splits `r` into weighted columns." _split_rect_axis(r, weights, gap, false) }

fn split_rows(list r, list weights, f64 gap=0.0) list { "Splits `r` into weighted rows." _split_rect_axis(r, weights, gap, true) }

fn apply_window_rect(any id, list r) any {
   "Applies a stored window rectangle by widget/window id without fighting active drag/resize."
   def key = to_str(id)
   if key.len == 0 { return 0 }
   mut st = _window_lookup(key)
   if !is_dict(st) {
      st = dict(12)
      st["id"] = key
      st["open"] = true
   }
   def nx, ny = _snap(float(r.get(0, 0.0))), _snap(float(r.get(1, 0.0)))
   def nw, nh = max(_snap(float(r.get(2, 0.0))), _min_win_w()), max(_snap(float(r.get(3, 0.0))), _min_win_h())
   def moved = abs(float(st.get("x", nx)) - nx) > 0.5 ||
   abs(float(st.get("y", ny)) - ny) > 0.5 ||
   abs(float(st.get("w", nw)) - nw) > 0.5 ||
   abs(float(st.get("h", nh)) - nh) > 0.5
   if _active_window_move == key || _active_window_resize == key {
      return 0
   }
   ;; A docked window's rectangle belongs to its dockspace.
   if to_str(st.get("dock", "")) != "" { return 0 }
   st["x"] = nx
   st["y"] = ny
   st["w"] = nw
   st["h"] = nh
   if moved {
      st["scroll_y"] = 0.0
      st["content_h"] = 0.0
   }
   st = _clamp_window_pos(st)
   _window_store(st)
}

fn apply_window_rect_if_visible(any id, bool visible, list r) any { "Applies `r` only when the window is visible." if visible { apply_window_rect(id, r) } }

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
   if compact_stack {
      def editor_min = compact_tiny ? 112.0 : (compact_short ? 144.0 : 190.0)
      def editor_max = compact_short ? 260.0 : 340.0
      def lower_min = compact_tiny ? 128.0 : (compact_short ? 190.0 : 280.0)
      mut editor_h = clamp(root_h * (compact_tiny ? 0.30 : (compact_short ? 0.32 : (compact_narrow ? 0.34 : 0.36))), editor_min, editor_max)
      if root_h - editor_h - g < lower_min { editor_h = max(editor_min, root_h - g - lower_min) }
      def editor = [root_x, root_y, root_w, editor_h]
      def lower = [root_x, root_y + editor_h + g, root_w, max(1.0, root_h - editor_h - g)]
      mut center = lower
      mut side = lower
      if compact_narrow || compact_short {
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
   if root_w < 980.0 { left_w = clamp(root_w * 0.28, 220.0, min(280.0, editor_left_max)) }
   if root_w >= 980.0 && left_w > editor_left_max { left_w = editor_left_max }
   def right_x = root_x + left_w + g
   mut right_w = max(1.0, root_w - left_w - g)
   def center_bias = _weight_or(bottom_weights, 0, 1.0)
   def side_bias = _weight_or(bottom_weights, 1, 0.86)
   def side_share = clamp(0.40 * (side_bias / max(0.05, center_bias)), compact ? 0.30 : 0.28, compact ? 0.46 : 0.42)
   mut sidebar_w = clamp(right_w * side_share, compact ? 340.0 : 390.0, compact ? 520.0 : 580.0)
   if right_w < 760.0 { sidebar_w = clamp(right_w * 0.40, 230.0, 300.0) }
   mut center_w = right_w - sidebar_w - g
   if center_w < 360.0 {
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

fn _ensure_theme_colors() any {
   if !is_list(_accent_rgba) { _accent_rgba = [0.714, 0.627, 1.000, 1.00] }
   if !is_list(_press_rgba) { _press_rgba = [0.290, 0.243, 0.447, 1.00] }
   if !is_list(_window_bg_rgba) { _window_bg_rgba = [0.020, 0.020, 0.024, 1.00] }
   if !is_list(_window_body_rgba) { _window_body_rgba = [0.031, 0.031, 0.039, 0.98] }
   if !is_list(_window_hdr_rgba) { _window_hdr_rgba = [0.055, 0.055, 0.067, 1.00] }
   if !is_list(_panel_rgba) { _panel_rgba = [0.067, 0.067, 0.082, 1.00] }
   if !is_list(_panel_hover_rgba) { _panel_hover_rgba = [0.102, 0.102, 0.125, 1.00] }
   if !is_list(_panel_active_rgba) { _panel_active_rgba = [0.141, 0.141, 0.173, 1.00] }
   if !is_list(_popup_rgba) { _popup_rgba = [0.047, 0.047, 0.059, 1.00] }
   if !is_list(_border_rgba) { _border_rgba = [0.149, 0.149, 0.180, 1.00] }
   if !is_list(_text_rgba) { _text_rgba = [0.941, 0.941, 0.953, 1.00] }
   if !is_list(_text_dim_rgba) { _text_dim_rgba = [0.643, 0.643, 0.682, 1.00] }
   if !is_list(_ok_rgba) { _ok_rgba = [0.545, 0.863, 0.604, 1.00] }
   if !is_list(_warn_rgba) { _warn_rgba = [0.949, 0.784, 0.475, 1.00] }
   if !is_list(_danger_rgba) { _danger_rgba = [1.000, 0.478, 0.420, 1.00] }
   if !is_list(_text_faint_rgba) { _text_faint_rgba = [0.435, 0.435, 0.478, 1.00] }
   0
}

fn _accent_alpha(f64 a) int {
   color_pack(float(_accent_rgba.get(0, 0.0)), float(_accent_rgba.get(1, 0.0)), float(_accent_rgba.get(2, 0.0)), a)
}

fn _palette_linear(f64 v) f64 {
   def f64 c = float(clamp(v, 0.0, 1.0))
   c <= 0.04045 ? c / 12.92 : float(pow((c + 0.055) / 1.055, 2.4))
}

fn _palette_luminance(list c) f64 {
   0.2126 * _palette_linear(float(c.get(0, 0.0))) + 0.7152 * _palette_linear(float(c.get(1, 0.0))) + 0.0722 * _palette_linear(float(c.get(2, 0.0)))
}

fn _palette_mix(list a, list b, f64 t) list {
   [float(a.get(0)) * (1.0 - t) + float(b.get(0)) * t,
      float(a.get(1)) * (1.0 - t) + float(b.get(1)) * t,
      float(a.get(2)) * (1.0 - t) + float(b.get(2)) * t,
      float(a.get(3, 1.0)) * (1.0 - t) + float(b.get(3, 1.0)) * t]
}

fn _palette_color(list c) list {
   [float(clamp(float(c.get(0)), 0.0, 1.0)), float(clamp(float(c.get(1)), 0.0, 1.0)),
      float(clamp(float(c.get(2)), 0.0, 1.0)), float(clamp(float(c.get(3, 1.0)), 0.0, 1.0))]
}

fn generate_palette(list background, list surface, list accent_color) any {
   "Generates semantic GUI colors from background, surface and accent RGB(A) lists.
   Text is chosen from black or white for contrast with the surface. Input colors
   are clamped ; return 0 if any list has fewer than three channels."
   if background.len < 3 || surface.len < 3 || accent_color.len < 3 { return 0 }
   def bg = _palette_color(background)
   def panel = _palette_color(surface)
   def accent = _palette_color(accent_color)
   def bool light = _palette_luminance(panel) > 0.179
   def ink = light ? [0.0, 0.0, 0.0, 1.0] : [1.0, 1.0, 1.0, 1.0]
   def ok = light ? [0.10, 0.52, 0.24, 1.0] : [0.545, 0.863, 0.604, 1.0]
   def warn = light ? [0.66, 0.43, 0.02, 1.0] : [0.949, 0.784, 0.475, 1.0]
   def danger = light ? [0.78, 0.16, 0.12, 1.0] : [1.0, 0.478, 0.42, 1.0]
   {"window": bg, "surface": panel, "accent": accent, "text": ink,
      "text_dim": _palette_mix(panel, ink, 0.70), "text_faint": _palette_mix(panel, ink, 0.48),
      "surface_hover": _palette_mix(panel, ink, 0.10), "surface_active": _palette_mix(panel, ink, 0.18),
      "surface_weak": _palette_mix(bg, panel, 0.45), "surface_strong": _palette_mix(panel, ink, 0.08),
      "accent_weak": _palette_mix(panel, accent, 0.16), "accent_strong": _palette_mix(accent, ink, 0.12),
      "accent_hover": _palette_mix(accent, ink, 0.08), "accent_pressed": _palette_mix(accent, ink, 0.18),
      "header": _palette_mix(bg, panel, 0.55), "popup": _palette_mix(bg, panel, 0.75),
      "border": _palette_mix(panel, ink, 0.25), "ok": ok, "warn": warn, "danger": danger,
      "press": _palette_mix(panel, accent, 0.45)}
}

fn apply_palette(any palette) bool {
   "Applies a complete palette from generate_palette. Invalid palettes leave the current theme unchanged."
   if !is_dict(palette) { return false }
   def keys = ["window", "surface", "accent", "text", "text_dim", "text_faint", "surface_hover", "surface_active", "header", "popup", "border", "ok", "warn", "danger", "press"]
   for key in keys {
      def c = palette.get(key, nil)
      if !is_list(c) || c.len < 3 { return false }
   }
   for key in keys { if key != "press" { set_theme_color(to_str(key), palette.get(key)) } }
   _press_rgba = _palette_color(palette.get("press"))
   _theme_button_styles = dict(0)
   _window_bg_rgba = _palette_color(palette.get("window"))
   _theme_name = "custom"
   _theme_dirty = true
   true
}

fn _theme_refresh() any {
   ;; Packs the semantic palette once per change. Widgets read only the packed
   ;; tokens below, so a theme is these fifteen RGBA lists and nothing else.
   if !_theme_dirty && _theme_ready { return 0 }
   _ensure_theme_colors()
   _accent_u32 = _theme_pack(_accent_rgba)
   _press_u32 = _theme_pack(_press_rgba)
   _press_soft_u32 = color_pack(float(_press_rgba.get(0, 0.0)), float(_press_rgba.get(1, 0.0)), float(_press_rgba.get(2, 0.0)), 0.58)
   _accent_soft_u32 = _accent_alpha(0.22)
   _select_u32 = _accent_alpha(0.30)
   _focus_u32 = _accent_alpha(0.85)
   ;; A light surface needs a much fainter drop edge than a dark one.
   _shadow_u32 = color_pack(0.0, 0.0, 0.0, _palette_luminance(_panel_rgba) > 0.179 ? 0.12 : 0.35)
   _window_bg_u32 = _theme_pack(_window_bg_rgba)
   _window_body_u32 = _theme_pack(_window_body_rgba)
   _window_hdr_u32 = _theme_pack(_window_hdr_rgba)
   _panel_u32 = _theme_pack(_panel_rgba)
   _panel_hover_u32 = _theme_pack(_panel_hover_rgba)
   _panel_active_u32 = _theme_pack(_panel_active_rgba)
   _popup_u32 = _theme_pack(_popup_rgba)
   _border_u32 = _theme_pack(_border_rgba)
   _scroll_track_u32 = color_pack(float(_panel_rgba.get(0, 0.0)), float(_panel_rgba.get(1, 0.0)), float(_panel_rgba.get(2, 0.0)), 0.60)
   _scroll_thumb_u32 = color_pack(float(_text_dim_rgba.get(0, 0.0)), float(_text_dim_rgba.get(1, 0.0)), float(_text_dim_rgba.get(2, 0.0)), 0.34)
   _scroll_thumb_hot_u32 = _accent_alpha(0.80)
   _grid_major_u32 = color_pack(float(_border_rgba.get(0, 0.0)), float(_border_rgba.get(1, 0.0)), float(_border_rgba.get(2, 0.0)), 0.55)
   _grid_minor_u32 = color_pack(float(_border_rgba.get(0, 0.0)), float(_border_rgba.get(1, 0.0)), float(_border_rgba.get(2, 0.0)), 0.25)
   _text_u32 = _theme_pack(_text_rgba)
   _text_dim_u32 = _theme_pack(_text_dim_rgba)
   _text_faint_u32 = _theme_pack(_text_faint_rgba)
   _ok_u32 = _theme_pack(_ok_rgba)
   _warn_u32 = _theme_pack(_warn_rgba)
   _danger_u32 = _theme_pack(_danger_rgba)
   _theme_dirty = false
   _theme_ready = true
}

fn _theme_token_index(str name) int {
   ;; One table for both the getter and the setter, aliases included.
   if name == "text" { return 0 }
   if name == "text_dim" || name == "muted" { return 1 }
   if name == "text_faint" || name == "faint" || name == "disabled" { return 2 }
   if name == "accent" || name == "info" { return 3 }
   if name == "ok" || name == "success" { return 4 }
   if name == "warn" || name == "warning" { return 5 }
   if name == "danger" || name == "error" { return 6 }
   if name == "surface" || name == "panel" { return 7 }
   if name == "surface_hover" { return 8 }
   if name == "surface_active" { return 9 }
   if name == "window" { return 10 }
   if name == "header" { return 11 }
   if name == "popup" { return 12 }
   if name == "border" { return 13 }
   if name == "press" { return 14 }
   -1
}

fn theme_color(str name) list {
   "Returns the RGBA list of a semantic theme token: text, text_dim, text_faint, accent, ok, warn, danger, surface, surface_hover, surface_active, window, header, popup, or border. Unknown names return the text color."
   _ensure_theme_colors()
   def idx = _theme_token_index(name)
   if idx == 1 { return _text_dim_rgba }
   if idx == 2 { return _text_faint_rgba }
   if idx == 3 { return _accent_rgba }
   if idx == 4 { return _ok_rgba }
   if idx == 5 { return _warn_rgba }
   if idx == 6 { return _danger_rgba }
   if idx == 7 { return _panel_rgba }
   if idx == 8 { return _panel_hover_rgba }
   if idx == 9 { return _panel_active_rgba }
   if idx == 10 { return _window_body_rgba }
   if idx == 11 { return _window_hdr_rgba }
   if idx == 12 { return _popup_rgba }
   if idx == 13 { return _border_rgba }
   if idx == 14 { return _press_rgba }
   _text_rgba
}

fn theme_color_alpha(str name, f64 alpha) list {
   "Returns a semantic theme token with its alpha replaced by `alpha` (for lines, overlays, and faded text)."
   def c = theme_color(name)
   [float(c.get(0, 0.0)), float(c.get(1, 0.0)), float(c.get(2, 0.0)), clamp(alpha, 0.0, 1.0)]
}

fn set_theme_color(str name, list rgba) bool {
   "Overrides one semantic theme token with an RGBA list. Returns false for an unknown token or a short list."
   _ensure_theme_colors()
   def idx = _theme_token_index(name)
   if idx < 0 || rgba.len < 3 { return false }
   def c = [
      clamp(float(rgba.get(0, 0.0)), 0.0, 1.0), clamp(float(rgba.get(1, 0.0)), 0.0, 1.0),
      clamp(float(rgba.get(2, 0.0)), 0.0, 1.0), clamp(float(rgba.get(3, 1.0)), 0.0, 1.0)
   ]
   if idx == 0 { _text_rgba = c }
   elif idx == 1 { _text_dim_rgba = c }
   elif idx == 2 { _text_faint_rgba = c }
   elif idx == 3 { _accent_rgba = c }
   elif idx == 4 { _ok_rgba = c }
   elif idx == 5 { _warn_rgba = c }
   elif idx == 6 { _danger_rgba = c }
   elif idx == 7 { _panel_rgba = c }
   elif idx == 8 { _panel_hover_rgba = c }
   elif idx == 9 { _panel_active_rgba = c }
   elif idx == 10 { _window_body_rgba = c }
   elif idx == 11 { _window_hdr_rgba = c }
   elif idx == 12 { _popup_rgba = c }
   elif idx == 13 { _border_rgba = c }
   else { _press_rgba = c }
   _theme_dirty = true
   true
}

fn set_enabled(bool v) any { "Enables or disables GUI input and rendering helpers." _enabled = !!v }

fn enabled() bool { "Returns true when GUI helpers are enabled." _enabled }

fn reset_state() any {
   "Clears transient widget, focus, and scroll-area state."
   _accessibility_nodes = []
   _accessibility_requests = dict(16)
   _accessibility_request_count = 0
   _ui_animator = ui_anim.animator()
   _widget_state = dict(64)
   _hot_id, _active_widget_id = "", ""
   _kbd_focus_id, _text_focus_id = "", ""
   _nav_focus_target, _nav_focus_window = "", ""
   _last_item, _hot_rect, _active_rect = [0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0]
   _same_line_next, _same_line_gap = false, 0.0
   _scroll_area_active, _scroll_area_id = false, ""
   _scroll_area_parent = nil
   _scroll_area_rect = [0.0, 0.0, 0.0, 0.0]
   _scroll_area_start_y, _scroll_area_scroll_y = 0.0, 0.0
   _scroll_area_clip_pushed, _scroll_area_capture_window = false, ""
   _scroll_area_capture_rect = [0.0, 0.0, 0.0, 0.0]
   _popup_close_from(0)
   _popup_dismissed, _ov_cmds, _ctx_stack = [], [], []
   _ov_top, _win_layers, _dock_frame_active = [], [], false
   _ov_depth, _ov_discard, _popup_depth = 0, 0, 0
   _id_stack, _indent_stack, _col_stack, _row_n, _tbl_id = [], [], [], 0, ""
   _menubar_active, _menubar_open = false, ""
   _tip_x, _tip_y, _tip_frames, _tip_seen, _tip_text = -1.0, -1.0, 0, -1, ""
   _dnd_active, _dnd_type, _dnd_label, _dnd_source = false, "", "", ""
   _dnd_payload = nil
   _last_item_id = ""
   _mouse_pressed1, _event_mouse_pressed1 = false, false
   _fed_down0, _fed_down0_valid = false, false
}

fn _reset_font_metric_caches() any {
   _text_fit_cache, _text_measure_cache, _font_h_cache = dict(512), dict(1024), dict(16)
   _text_caches_ready = true
   _font_metric_frame = -1
   _cached_body_h, _cached_title_h, _cached_small_h = 0.0, 0.0, 0.0
}

fn set_fonts(int body_font, int title_font=0, int small_font=0) any {
   "Sets body, title, and small font handles used by GUI text."
   def old_body, old_title, old_small = _font, _font_title, _font_small
   if is_int(body_font) && body_font > 0 { _font = int(body_font) }
   _font_title = _font
   if is_int(title_font) && title_font > 0 { _font_title = int(title_font) }
   _font_small = _font
   if is_int(small_font) && small_font > 0 { _font_small = int(small_font) }
   if old_body != _font || old_title != _font_title || old_small != _font_small { _reset_font_metric_caches() }
}

fn set_scale(f64 v) any {
   "Sets UI scale, clamped to a practical range."
   mut nv = float(v)
   if nv < 0.50 { nv = 0.50 } elif nv > 3.00 { nv = 3.00 }
   if abs(nv - _scale) > 0.000001 { _reset_font_metric_caches() }
   _scale = nv
}

fn scale() f64 { "Returns the current UI scale." _scale }

fn set_accent(list c) any {
   "Sets the accent color from an RGBA list."
   _ensure_theme_colors()
   if !is_list(c) { return 0 }
   if is_list(_accent_rgba) && _accent_rgba.len == 4 && c.len == 4 {
      if c.get(0) == _accent_rgba.get(0) && c.get(1) == _accent_rgba.get(1) &&
      c.get(2) == _accent_rgba.get(2) && c.get(3) == _accent_rgba.get(3) {
         return 0
      }
   }
   _accent_rgba = clone(c)
   _theme_dirty = true
}

fn accent() list { "Returns the current accent RGBA color." if !is_list(_accent_rgba) { _ensure_theme_colors() } _accent_rgba }

fn set_debug_overlay(bool v) any { "Enables or disables the GUI debug overlay." _debug_overlay = !!v }

fn debug_overlay() bool { "Returns true when the GUI debug overlay is enabled." _debug_overlay }

@inline
@jit
fn _font_body() int {
   if _font > 0 { return _font }
   _font_title
}

@inline
@jit
fn _font_title_id() int {
   if _font_title > 0 { return _font_title }
   _font_body()
}

@inline
@jit
fn _font_small_id() int {
   if _font_small > 0 { return _font_small }
   _font_body()
}

fn _measure_text_ui(int font_id, any txt) list {
   if !_text_caches_ready { _ensure_text_caches() }
   def fid = (font_id > 0) ? font_id : _font_body()
   def s = _ui_text(txt)
   def n = str.len(s)
   def use_cache = fid > 0 && n > 0 && n <= 256
   ;; One table per font, keyed by the text: building a "font:text" key
   ;; allocated a string for every label on every frame.
   mut any per_font = use_cache ? _text_measure_cache.get(fid, nil) : nil
   if use_cache && is_dict(per_font) {
      def any hit = per_font.get(s, nil)
      if is_list(hit) { return hit }
   }
   def m = measure_text_fast(fid, s)
   if use_cache {
      if !is_dict(per_font) || per_font.len >= 4096 {
         per_font = dict(1024)
         _text_measure_cache[fid] = per_font
      }
      per_font[s] = m
   }
   m
}

fn _font_h_for(int font_id) f64 {
   if !_text_caches_ready { _ensure_text_caches() }
   def fid = (font_id > 0) ? font_id : _font_body()
   if _font_metric_frame == _gui_frame_id {
      if fid == _font_body() && _cached_body_h > 0.0 { return _cached_body_h }
      if fid == _font_title_id() && _cached_title_h > 0.0 { return _cached_title_h }
      if fid == _font_small_id() && _cached_small_h > 0.0 { return _cached_small_h }
   } else {
      _font_metric_frame = _gui_frame_id
      _cached_body_h, _cached_title_h, _cached_small_h = 0.0, 0.0, 0.0
   }
   def key = to_str(fid)
   if _font_h_cache.contains(key) {
      def cached = float(_font_h_cache.get(key, _sx(15.0)))
      if fid == _font_body() { _cached_body_h = cached }
      if fid == _font_title_id() { _cached_title_h = cached }
      if fid == _font_small_id() { _cached_small_h = cached }
      return cached
   }
   mut h = float(font_line_height(fid))
   mut asc = float(font_ascent(fid))
   if asc > 0.0 {
      h = min(h, asc + _sx(3.0))
   }
   if h < _sx(10.0) {
      def m = _measure_text_ui(fid, "Ag")
      h = float(m.get(1, _sx(15.0)))
   }
   if h < _sx(10.0) { h = _sx(15.0) }
   _font_h_cache[key] = h
   if fid == _font_body() { _cached_body_h = h }
   if fid == _font_title_id() { _cached_title_h = h }
   if fid == _font_small_id() { _cached_small_h = h }
   h
}

fn _text_h() f64 { _font_h_for(_font_body()) }

fn _text_fit_cache_key(int font_id, any txt, f64 max_w) str {
   def s = _ui_text(txt)
   (str.len(s) <= 256 && float(max_w) > 0.0) ? (to_str(font_id) + ":" + to_str(int(float(max_w))) + ":" + s) : ""
}

fn _text_fit_cache_put(str key, str value) str {
   if !_text_caches_ready { _ensure_text_caches() }
   def k, v = to_str(key), to_str(value)
   if k.len > 0 { _text_fit_cache = cache.cache_put_reset(_text_fit_cache, k, v, 4096, 512) }
   v
}

fn _text_fit_width(int font_id, str s) f64 {
   def measured_m = _measure_text_ui(font_id, s)
   def measured = float(measured_m.get(0, 0.0))
   def n = str.len(s)
   if n <= 0 { return measured }
   max(measured, float(n) * _font_h_for(font_id) * 0.56)
}

fn _text_likely_fits_ascii(int font_id, str s, f64 max_w) bool {
   ;; A shortcut must never claim a fit the glyphs do not have: 0.64 of the
   ;; line height let wide strings ("WWWW") overflow unclipped. ASCII advances
   ;; stay under one line height, so only clearly short text skips measuring.
   def n = str.len(s)
   if float(max_w) <= 0.0 || n <= 0 || n > 64 { return false }
   if !str.ascii_only(s) { return false }
   float(n) * _font_h_for(font_id) <= float(max_w)
}

fn _text_fit(int font_id, any txt, f64 max_w=0.0) str {
   if !_text_caches_ready { _ensure_text_caches() }
   def s = _ui_text(txt)
   if float(max_w) <= 0.0 { return s }
   def fid = (font_id > 0) ? font_id : _font_body()
   ;; Fit against whole pixels: the cache key is the floored width, so every
   ;; caller sharing a key gets a result computed for that exact width, which
   ;; never exceeds its real space (rounding reused a 100.4px fit at 99.6px).
   def whole_w = float(int(float(max_w)))
   if whole_w != float(max_w) { return whole_w > 0.0 ? _text_fit(fid, s, whole_w) : "" }
   if _text_likely_fits_ascii(fid, s, max_w) { return s }
   def n = str.len(s)
   if n <= 48 && _text_fit_width(fid, s) <= float(max_w) { return s }
   def cache_key = _text_fit_cache_key(fid, s, max_w)
   def use_cache = is_str(cache_key) && str.len(cache_key) > 0
   if use_cache && _text_fit_cache.contains(cache_key) { return to_str(_text_fit_cache.get(cache_key, s)) }
   if _text_fit_width(fid, s) <= float(max_w) { return _text_fit_cache_put(cache_key, s) }
   def ell = "..."
   if _text_fit_width(fid, ell) > float(max_w) { return "" }
   def total_n = n
   mut lo = 0
   mut hi = total_n
   mut best = 0
   while lo <= hi {
      def mid_raw = (lo + hi) / 2
      def mid = _utf8_prev_index(s, mid_raw)
      def cand = str.str_slice(s, 0, mid) + ell
      if _text_fit_width(fid, cand) <= float(max_w) {
         best = mid
         lo = max(mid_raw + 1, mid + 1)
      } else {
         hi = mid_raw - 1
      }
   }
   if best > 0 { return _text_fit_cache_put(cache_key, str.str_slice(s, 0, best) + ell) }
   _text_fit_cache_put(cache_key, ell)
}

fn _text_color_u32(any color) int {
   if is_int(color) { return color }
   def col = is_list(color) ? color : (is_tuple(color) ? color : _text_rgba)
   color_pack(
      float(col.get(0, 1.0)),
      float(col.get(1, 1.0)),
      float(col.get(2, 1.0)),
      float(col.get(3, 1.0))
   )
}

fn _flush_rect_batch() int { gui_batch.flush() }

fn _ov_push(any kind, any a, any b, any c, any d, any e) bool {
   "Appends one overlay draw command; popups, menus, and tooltips replay these above every window at end_frame."
   if _ov_discard > 0 { return true }
   _ov_cmds = _ov_cmds.append(kind)
   _ov_cmds = _ov_cmds.append(a)
   _ov_cmds = _ov_cmds.append(b)
   _ov_cmds = _ov_cmds.append(c)
   _ov_cmds = _ov_cmds.append(d)
   _ov_cmds = _ov_cmds.append(e)
   true
}

fn _gui_rect_fast(f64 x, f64 y, f64 w, f64 h, int color) bool {
   if _ov_depth > 0 { return _ov_push(0, _snap(x), _snap(y), max(1.0, _snap(w)), max(1.0, _snap(h)), color) }
   gui_batch.queue_rect(_snap(x), _snap(y), max(1.0, _snap(w)), max(1.0, _snap(h)), color)
}

fn _gui_rect_outline_fast(f64 x, f64 y, f64 w, f64 h, int color) bool {
   if _ov_depth > 0 { return _ov_push(1, _snap(x), _snap(y), max(1.0, _snap(w)), max(1.0, _snap(h)), color) }
   gui_batch.queue_outline(_snap(x), _snap(y), max(1.0, _snap(w)), max(1.0, _snap(h)), color)
}

fn _gui_rect_tex_uv(
   f64 x, f64 y, f64 w, f64 h, int tex_id,
   f64 u1, f64 v1, f64 u2, f64 v2,
   f64 r, f64 g, f64 b, f64 a) bool {
   if _ov_depth > 0 { return _ov_push(5, [x, y, w, h, u1, v1, u2, v2, r, g, b, a], tex_id, 0, 0, 0) }
   gui_batch.draw_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a)
}

fn _tex_ref_id(any tex_ref) int {
   is_dict(tex_ref) ? int(tex_ref.get("tex", -1)) : int(tex_ref)
}

fn _tex_ref_uv(any tex_ref) list {
   if is_dict(tex_ref) {
      def uv = tex_ref.get("uv", 0)
      if is_list(uv) && uv.len >= 4 { return uv }
   }
   [0.0, 0.0, 1.0, 1.0]
}

fn _gui_tex_ref_uv(
   f64 x, f64 y, f64 w, f64 h, any tex_ref,
   f64 r=1.0, f64 g=1.0, f64 b=1.0, f64 a=1.0
) bool {
   def tex = _tex_ref_id(tex_ref)
   if tex < 0 { return false }
   def uv = _tex_ref_uv(tex_ref)
   _gui_rect_tex_uv(
      x, y, w, h, tex,
      float(uv.get(0, 0.0)), float(uv.get(1, 0.0)),
      float(uv.get(2, 1.0)), float(uv.get(3, 1.0)),
      r, g, b, a
   )
}

fn _draw_text_ui_now(int font_id, any txt, f64 x, f64 y, any color, f64 shadow_alpha=0.0, f64 max_w=0.0) any {
   def px, py = _snap(float(x)), _snap(float(y))
   def fid = (font_id > 0) ? font_id : _font_body()
   def fit_txt = _text_fit(fid, txt, max_w)
   def packed = _text_color_u32(color)
   if _ov_depth > 0 {
      if str.len(fit_txt) > 0 { _ov_push(2, fid, fit_txt, px, py, packed) }
      return 0
   }
   _flush_rect_batch()
   def a = float((packed >> 24) & 255) / 255.0
   def sh_a = clamp(float(shadow_alpha), 0.0, 1.0)
   if a > 0.001 && sh_a > 0.001 { draw_text(fid, fit_txt, px + 1.0, py + 1.0, color_pack(0.0, 0.0, 0.0, min(sh_a, a * sh_a))) }
   draw_text(fid, fit_txt, px, py, packed)
}

fn _queue_text_ui(int font_id, any txt, f64 x, f64 y, any color, f64 max_w=0.0) any {
   def fid = (font_id > 0) ? font_id : _font_body()
   def fit_txt = _text_fit(fid, txt, max_w)
   if str.len(fit_txt) <= 0 { return 0 }
   if _ov_depth > 0 {
      _ov_push(2, fid, fit_txt, _snap(float(x)), _snap(float(y)), _text_color_u32(color))
      return 0
   }
   if !is_list(_text_run_queue) { _text_run_queue = [] }
   if fid != _text_run_font {
      if _text_run_queue.len > 0 {
         _text_run_segments = _text_run_segments.append(_text_run_font)
         _text_run_segments = _text_run_segments.append(_text_run_queue)
         _text_run_queue = _text_run_spare.len > 0 ? _text_run_spare.pop() : []
      }
      _text_run_font = fid
   }
   _text_run_queue = _text_run_queue.append(fit_txt)
   _text_run_queue = _text_run_queue.append(_snap(float(x)))
   _text_run_queue = _text_run_queue.append(_snap(float(y)))
   _text_run_queue = _text_run_queue.append(_text_color_u32(color))
}

fn _text_runs_reset() any {
   mut i = 1
   while i < _text_run_segments.len {
      def seg = _text_run_segments.get(i, nil)
      if is_list(seg) {
         clear(seg)
         _text_run_spare = _text_run_spare.append(seg)
      }
      i += 2
   }
   clear(_text_run_segments)
   if is_list(_text_run_queue) { clear(_text_run_queue) } else { _text_run_queue = [] }
   _text_run_font = -1
}

fn _text_runs_queued() int {
   mut n = is_list(_text_run_queue) ? _text_run_queue.len / 4 : 0
   mut i = 1
   while i < _text_run_segments.len {
      def seg = _text_run_segments.get(i, nil)
      if is_list(seg) { n += seg.len / 4 }
      i += 2
   }
   n
}

fn _flush_text_runs() any {
   _flush_rect_batch()
   if !is_list(_text_run_queue) { _text_run_queue = [] }
   if _text_run_queue.len <= 0 && _text_run_segments.len <= 0 { return 0 }
   def prof = ui_profile.enabled()
   def t0 = prof ? ui_profile.now() : 0
   def runs = prof ? _text_runs_queued() : 0
   mut batches = 0
   mut i = 0
   while i + 1 < _text_run_segments.len {
      def seg = _text_run_segments.get(i + 1, nil)
      if is_list(seg) && seg.len > 0 {
         draw_text_runs_flat_colors(int(_text_run_segments.get(i, _font_body())), seg)
         batches += 1
      }
      i += 2
   }
   if _text_run_queue.len > 0 {
      draw_text_runs_flat_colors(_text_run_font, _text_run_queue)
      batches += 1
   }
   _text_runs_reset()
   if prof { ui_profile.mark("flush_text_internal", t0, " runs=" + to_str(runs) + " batches=" + to_str(batches)) }
}

fn warm_text_pipeline(int body_font=0, int title_font=0, int small_font=0) bool {
   "Warms GUI font atlases for interactive panels without submitting visible text."
   if !_enabled { return false }
   if body_font > 0 {
      set_fonts(body_font, title_font, small_font)
   }
   _theme_refresh()
   _text_runs_reset()
   def sample = "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789 [](){}<>:/._-+*=%,"
   if _font_body() > 0 { font_prepare(_font_body(), sample) }
   if _font_title_id() > 0 { font_prepare(_font_title_id(), sample) }
   if _font_small_id() > 0 { font_prepare(_font_small_id(), sample) }
   true
}

fn _draw_text_ui_ex(int font_id, any txt, f64 x, f64 y, any color, f64 shadow_alpha=0.0, f64 max_w=0.0) any {
   def px, py = _snap(float(x)), _snap(float(y))
   def fid = (font_id > 0) ? font_id : _font_body()
   def sh_a = clamp(float(shadow_alpha), 0.0, 1.0)
   if sh_a > 0.001 || ui_profile.text_immediate_enabled() {
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
   if is_list(clip_rect) && clip_rect.len >= 4 {
      def s = _ui_text(txt)
      def limit_w = float(max_w)
      def clip_x = float(clip_rect.get(0, 0.0))
      def clip_y = float(clip_rect.get(1, 0.0))
      def clip_w = max(0.0, float(clip_rect.get(2, 0.0)))
      def clip_h = max(0.0, float(clip_rect.get(3, 0.0)))
      def right = clip_x + clip_w
      def bottom = clip_y + clip_h
      def available_w = max(0.0, right - float(x))
      if clip_w <= 0.0 || clip_h <= 0.0 || available_w <= 0.0 { return 0 }
      def fit_w = limit_w > 0.0 ? min(limit_w, available_w) : available_w
      def fid = (font_id > 0) ? font_id : _font_body()
      def font_h = _font_h_for(fid)
      if float(y) >= bottom || float(y) + font_h <= clip_y { return 0 }
      def y_inside = float(y) >= clip_y && float(y) + font_h <= bottom
      if float(x) >= clip_x && y_inside && float(shadow_alpha) <= 0.001 {
         _draw_text_ui_ex(fid, s, x, y, color, shadow_alpha, fit_w)
         return 0
      }
      ;; Partial rows and left-overhanging glyphs need a real scissor even
      ;; when their text is short enough to fit horizontally.
      push_clip_rect(clip_rect)
      _draw_text_ui_now(fid, s, x, y, color, shadow_alpha, fit_w)
      pop_clip_rect()
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
   if _fb_w > _min_win_w() { w = min(w, max(_min_win_w(), _fb_w)) }
   if _fb_h > _min_win_h() { h = min(h, max(_min_win_h(), _fb_h)) }
   st["w"] = w
   st["h"] = h
   st
}

fn _clamp_window_pos(dict st) dict {
   st = _fit_window_size(st)
   mut x, y = float(st.get("x", 0.0)), float(st.get("y", 0.0))
   def w, h = max(float(st.get("w", _min_win_w())), _min_win_w()), max(float(st.get("h", _min_win_h())), _min_win_h())
   def max_x, max_y = (_fb_w > 0.0) ? max(0.0, _fb_w - w) : x, (_fb_h > 0.0) ? max(0.0, _fb_h - h) : y
   if x < 0.0 { x = 0.0 } elif x > max_x { x = max_x }
   if y < 0.0 { y = 0.0 } elif y > max_y { y = max_y }
   st["x"] = x
   st["y"] = y
   st
}

fn _window_store(dict st) any { _ensure_gui_state() _windows[to_str(st.get("id", ""))] = st }

fn _window_lookup(any id) any { _ensure_gui_state() _windows.get(to_str(id), 0) }

fn _current_window_state() any {
   if is_dict(_current_window) { return _current_window }
   if _current_window_id_cache.len <= 0 { return 0 }
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
   if is_list(override) && override.len >= 4 { return override }
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
   while i < items_n {
      if to_str(items.get(i, "")) == want { return i }
      i += 1
   }
   -1
}

fn _full_id_window_id(any full_id) str {
   def key = to_str(full_id)
   def sep = str.find(key, "::")
   if sep > 0 { return str.str_slice(key, 0, sep) }
   ""
}

fn _window_order_touch(any id, bool to_front=false) any {
   def key = to_str(id)
   if key.len == 0 { return 0 }
   if !is_list(_window_order) { _window_order = [] }
   def found = _list_find_text(_window_order, key) >= 0
   if found && !to_front { return 0 }
   mut out = []
   mut i = 0
   def order_n = _window_order.len
   while i < order_n {
      def cur = to_str(_window_order.get(i, ""))
      if cur != key { out = out.append(cur) }
      i += 1
   }
   _window_order = out.append(key)
}

fn _top_window_id_at(f64 px, f64 py) str {
   mut pi = _popup_stack.len - 1
   while pi >= 0 {
      def pid = to_str(_popup_stack.get(pi, ""))
      def pst = _windows.get(pid, 0)
      if is_dict(pst) {
         if _rect_hit(px, py, float(pst.get("x", 0.0)), float(pst.get("y", 0.0)), float(pst.get("w", 0.0)), float(pst.get("h", 0.0))) { return pid }
      }
      pi -= 1
   }
   if _popup_hit_active {
      if _rect_hit(px, py, _popup_hit_x, _popup_hit_y, _popup_hit_w, _popup_hit_h) {
         return _popup_hit_window
      }
   }
   ;; Floating windows sit above docked ones, and a docked window only counts
   ;; while it is the visible tab of its panel.
   mut pass = 0
   while pass < 2 {
      mut i = _window_order.len - 1
      while i >= 0 {
         def wid = to_str(_window_order.get(i, ""))
         def st = _windows.get(wid, 0)
         if is_dict(st) && st.get("open", true) {
            def docked = to_str(st.get("dock", "")) != ""
            def counts = pass == 0 ? !docked : (docked && bool(st.get("dock_show", true)))
            if counts && _rect_hit(px, py,
               float(st.get("x", 0.0)),
               float(st.get("y", 0.0)),
               float(st.get("w", 0.0)),
               float(st.get("h", 0.0))){
               return wid
            }
         }
         i -= 1
      }
      pass += 1
   }
   def chrome_id = _dock_chrome_hit(px, py)
   if chrome_id != "" { return chrome_id }
   if _window_order.len > 0 { return "" }
   mut i = 0
   mut ks = dict_keys(_windows)
   if !is_list(ks) { return "" }
   def ks_n = ks.len
   while i < ks_n {
      def st = _windows.get(ks.get(i), 0)
      if is_dict(st) && st.get("open", true) {
         if _rect_hit(px, py,
            float(st.get("x", 0.0)),
            float(st.get("y", 0.0)),
            float(st.get("w", 0.0)),
            float(st.get("h", 0.0))){
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
   if is_dict(st) { ids = st.get("focusables", []) }
   is_list(ids) ? ids : []
}

fn _pointer_window_id() str {
   if _active_window_move != "" { return _active_window_move }
   if _active_window_resize != "" { return _active_window_resize }
   if _mouse_down0 && _active_widget_id != "" {
      def wid = _full_id_window_id(_active_widget_id)
      if wid.len > 0 { return wid }
   }
   _window_hovered
}

fn _window_accepts_pointer(any window_id) bool {
   def wid = to_str(window_id)
   if wid.len == 0 { return false }
   _pointer_window_id() == wid
}

fn _window_pointer_hit(any window_id, f64 x, f64 y, f64 w, f64 h) bool { _window_accepts_pointer(window_id) && _rect_hit(_mouse_x, _mouse_y, x, y, w, h) }

fn _current_window_pointer_hit(f64 x, f64 y, f64 w, f64 h) bool {
   if _current_window_id_cache.len <= 0 { return false }
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
   if win_id.len == 0 { return 0 }
   def ids = _window_focusables(win_id)
   def n = ids.len
   if n <= 0 { return 0 }
   mut cur = _text_focus_id
   if cur.len == 0 { cur = _kbd_focus_id }
   if cur.len == 0 { cur = _active_widget_id }
   mut idx = _list_find_text(ids, cur)
   if dir >= 0 { idx = (idx < 0) ? 0 : (((idx + 1) >= n) ? 0 : (idx + 1)) } else { idx = (idx < 0) ? (n - 1) : (((idx - 1) < 0) ? (n - 1) : (idx - 1)) }
   _nav_focus_target = to_str(ids.get(idx, ""))
   _nav_focus_window = win_id
   _kbd_focus_id = _nav_focus_target
   _text_focus_id = ""
   _active_widget_id = ""
}

fn _focus_collect_for_window(any window_id) bool {
   def wid = to_str(window_id)
   if wid.len == 0 { return false }
   _focused_window == wid ||
   _nav_focus_window == wid ||
   _full_id_window_id(_kbd_focus_id) == wid ||
   _full_id_window_id(_text_focus_id) == wid ||
   _full_id_window_id(_active_widget_id) == wid
}

fn hit_test(f64 x, f64 y) bool {
   "Returns true when a visible GUI window covers point `(x, y)`."
   if !_enabled { return false }
   _top_window_id_at(x, y).len > 0
}

fn _apply_event_pointer_pos() any {
   if !_event_mouse_has_pos { return 0 }
   _mouse_x, _mouse_y = _event_mouse_x, _event_mouse_y
   _window_hovered = _top_window_id_at(_mouse_x, _mouse_y)
}

fn _pointer_view_xy(any x, any y) list {
   if !_win { return [float(x), float(y)] }
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
   if data.contains("x") || data.contains("y") {
      _event_mouse_has_pos = true
      def p = _pointer_view_xy(data.get("x", _event_mouse_x), data.get("y", _event_mouse_y))
      _event_mouse_x, _event_mouse_y = float(p.get(0, _event_mouse_x)), float(p.get(1, _event_mouse_y))
   } elif _win {
      def mp = uiw.cursor_pos(_win)
      def p = _pointer_view_xy(mp.get(0, _mouse_x), mp.get(1, _mouse_y))
      _event_mouse_has_pos = true
      _event_mouse_x, _event_mouse_y = float(p.get(0, _mouse_x)), float(p.get(1, _mouse_y))
   }
   _apply_event_pointer_pos()
}

fn _event_pointer_claimed() bool { _pointer_window_id() != "" || _active_widget_id != "" || _active_window_move != "" || _active_window_resize != "" }

fn _queue_key_event(int key) any {
   if !is_list(_key_events) { _key_events = [] }
   _key_events = _key_events.append(key)
   _key_mask_set(key)
}

fn prepare_input(any win, f64 ww=0.0, f64 wh=0.0) any {
   "Prepares per-frame input state for the GUI."
   _theme_refresh()
   _win = win
   if float(ww) > 0.0 { _fb_w = float(ww) }
   if float(wh) > 0.0 { _fb_h = float(wh) }
   if !_enabled || (!win && !_headless_input) {
      _window_hovered = ""
      _wants_mouse, _wants_keyboard = false, false
      _mouse_down0, _mouse_down0_prev = false, false
      _mouse_pressed0, _mouse_released0 = false, false
      _mouse_pressed1 = false
      _key_events = []
      _clear_key_masks()
      _clear_pending_input_events()
      return 0
   }
   if _apply_release0 {
      _apply_release0 = false
      if !_event_mouse_pressed0 {
         _event_mouse_down0_known, _event_mouse_down0 = true, false
         _event_mouse_released0 = true
         _fed_down0, _fed_down0_valid = false, true
      }
   }
   if _event_mouse_has_pos {
      _mouse_x, _mouse_y = _event_mouse_x, _event_mouse_y
   } elif win {
      def mp = uiw.cursor_pos(win)
      def vp = _pointer_view_xy(mp.get(0, 0.0), mp.get(1, 0.0))
      _mouse_x, _mouse_y = float(vp.get(0, 0.0)), float(vp.get(1, 0.0))
   }
   ;; Headless input (no window handle) is driven only by fed events, so the
   ;; pointer and the button keep their last fed state between frames.
   mut sampled_down0 = _mouse_down0_prev
   if _event_mouse_down0_known { sampled_down0 = _event_mouse_down0 }
   elif _fed_down0_valid {
      sampled_down0 = _fed_down0
      ;; Safety net for a release that was never fed (an app that withheld
      ;; the event): if the window has reported the button up for a long
      ;; stretch with no button event, believe the window again.
      if _fed_down0 && win {
         if uiw.mouse_down(win, 0) { _fed_down0_stale = 0 }
         else { _fed_down0_stale += 1 }
         if _fed_down0_stale > 240 {
            _fed_down0, _fed_down0_valid = false, false
            _fed_down0_stale = 0
            sampled_down0 = false
         }
      }
   } elif win { sampled_down0 = uiw.mouse_down(win, 0) }
   _mouse_pressed1 = _event_mouse_pressed1
   def sampled_pressed0 = (_event_mouse_pressed0 || (sampled_down0 && !_mouse_down0_prev)) && sampled_down0
   def sampled_released0 = _event_mouse_released0 || (_mouse_down0_prev && !sampled_down0)
   _mouse_down0, _mouse_pressed0 = sampled_down0, sampled_pressed0
   _mouse_released0 = sampled_released0 && (_active_widget_id != "" || _active_window_move != "" || _active_window_resize != "")
   mut suppress_mouse = false
   if _suppress_mouse_frames > 0 {
      suppress_mouse = true
      _suppress_mouse_frames -= 1
   }
   if _suppress_mouse_until_up {
      suppress_mouse = true
      if !_mouse_down0 { _suppress_mouse_until_up = false }
   }
   if suppress_mouse {
      _mouse_pressed0, _mouse_released0 = false, false
      _event_mouse_pressed0, _event_mouse_released0 = false, false
      _event_mouse_down0_known = false
      _mouse_down0, _mouse_down0_prev = false, false
      _active_widget_id, _active_window_move, _active_window_resize = "", "", ""
      _active_rect = [0.0, 0.0, 0.0, 0.0]
   }
   _mouse_down0_prev = _mouse_down0
   if _mouse_pressed0 {
      _mouse_press_x, _mouse_press_y = _mouse_x, _mouse_y
      _mouse_press_seq += 1
   }
   if abs(_event_scroll_dx) > 0.000001 || abs(_event_scroll_dy) > 0.000001 {
      _mouse_scroll_dx, _mouse_scroll_dy = _event_scroll_dx, _event_scroll_dy
      _last_scroll_x += _event_scroll_dx
      _last_scroll_y += _event_scroll_dy
   } elif win {
      def sp, sx = uiw.scroll_pos(win), float(sp.get(0, 0.0))
      def sy = float(sp.get(1, 0.0))
      _mouse_scroll_dx, _mouse_scroll_dy = sx - _last_scroll_x, sy - _last_scroll_y
      _last_scroll_x, _last_scroll_y = sx, sy
   } else {
      _mouse_scroll_dx, _mouse_scroll_dy = 0.0, 0.0
   }
   if (_mouse_pressed0 || _mouse_pressed1) && _popup_stack.len > 0 { _popup_dismiss_outside() }
   _window_hovered = _top_window_id_at(_mouse_x, _mouse_y)
   if _mouse_pressed0 && _window_hovered.len == 0 && _active_window_move == "" && _active_window_resize == "" {
      _focused_window, _text_focus_id, _kbd_focus_id, _active_widget_id = "", "", "", ""
   }
   _wants_mouse = _pointer_window_id() != "" || _active_widget_id != "" || _active_window_move != "" || _active_window_resize != ""
   _wants_keyboard = _text_focus_id != "" || _kbd_focus_id != "" || _focused_window != "" || _active_widget_id != ""
   _cursor_want = 0
   _clear_pending_input_events()
   if _defer_release0 {
      _defer_release0 = false
      _apply_release0 = true
   }
}

fn feed_event(int typ, dict data) bool {
   "Feeds a window/input event into the GUI and returns whether it was claimed."
   if !_enabled { return false }
   if typ == EVENT_MOUSE_POS_CHANGED {
      _capture_event_pointer_pos(data)
      return _event_pointer_claimed()
   }
   if typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED {
      def b = int(data.get("button", -1))
      _capture_event_pointer_pos(data)
      if b == 0 && typ == EVENT_MOUSE_BUTTON_RELEASED && _event_mouse_pressed0 && _event_mouse_down0_known && _event_mouse_down0 {
         _defer_release0 = true
         return _event_pointer_claimed()
      }
      if b == 0 {
         _fed_down0, _fed_down0_valid = typ == EVENT_MOUSE_BUTTON_PRESSED, true
         _fed_down0_stale = 0
         _event_mouse_down0_known = true
         _event_mouse_down0 = typ == EVENT_MOUSE_BUTTON_PRESSED
         if typ == EVENT_MOUSE_BUTTON_PRESSED { _event_mouse_pressed0 = true }
         else { _event_mouse_released0 = true }
      } elif b == 1 && typ == EVENT_MOUSE_BUTTON_PRESSED {
         _event_mouse_pressed1 = true
      }
      return _event_pointer_claimed()
   }
   if typ == EVENT_MOUSE_SCROLL {
      _capture_event_pointer_pos(data)
      _event_scroll_dx += float(data.get("dx", 0.0))
      _event_scroll_dy += float(data.get("dy", 0.0))
      return _pointer_window_id() != ""
   }
   if typ == EVENT_FOCUS_OUT {
      ;; A release may never arrive once focus is gone; fall back to polling.
      _fed_down0, _fed_down0_valid = false, false
      return false
   }
   if typ == EVENT_TEXT_PREEDIT {
      _ime_preedit = to_str(data.get("text", ""))
      ;; Byte offset into the composition, like the field's own caret.
      _ime_preedit_cursor = int(data.get("cursor", -1))
      return _text_focus_id != ""
   }
   if typ == EVENT_TEXT_DELETE_SURROUNDING {
      _ime_delete_before += int(data.get("before", 0))
      _ime_delete_after += int(data.get("after", 0))
      return _text_focus_id != ""
   }
   if typ == EVENT_KEY_CHAR || typ == EVENT_KEY_CHAR_MODS {
      def cp = int(data.get("char", data.get("codepoint", 0)))
      if cp > 31 && cp != 127 {
         _text_events = _text_events + str.chr(cp)
         return _text_focus_id != ""
      }
      return false
   }
   if typ == EVENT_KEY_PRESSED {
      def mods = int(data.get("mod", 0))
      def k = int(data.get("key", 0))
      def text_active = _text_focus_id != ""
      def ctrl = (mods & MOD_CONTROL) != 0
      ;; Escape closes the innermost popup before it clears any focus.
      if k == KEY_ESCAPE && !text_active && _popup_stack.len > 0 {
         _popup_close_from(_popup_stack.len - 1)
         return true
      }
      if text_active && (mods & (MOD_ALT | MOD_SUPER | MOD_META)) == 0 {
         if ctrl {
            if k == KEY_A { _queue_key_event(_TEXT_SELECT_ALL) }
            elif k == KEY_C { _queue_key_event(_TEXT_COPY) }
            elif k == KEY_X { _queue_key_event(_TEXT_CUT) }
            elif k == KEY_V { _queue_key_event(_TEXT_PASTE) }
            elif k == KEY_BACKSPACE || k == KEY_DELETE { _queue_key_event(_TEXT_SELECT_ALL) _queue_key_event(KEY_DELETE) }
            return true
         }
         if k == KEY_ESCAPE {
            _text_focus_id = ""
            _kbd_focus_id = ""
            _active_widget_id = ""
            return true
         }
         if k == KEY_BACKSPACE
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
         || k == KEY_SPACE{
            _queue_key_event(k)
            return true
         }
      }
      if (mods & (MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META)) != 0 { return false }
      if k == KEY_ESCAPE {
         def had_focus = _text_focus_id != "" || _kbd_focus_id != "" || _focused_window != ""
         _text_focus_id = ""
         _kbd_focus_id = ""
         _active_widget_id = ""
         _focused_window = ""
         return had_focus
      }
      if k == KEY_TAB {
         _request_focus_step(((mods & MOD_SHIFT) != 0) ? -1 : 1)
         _queue_key_event(k)
         return _focused_window != "" || _text_focus_id != ""
      }
      if k == KEY_BACKSPACE
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
      || k == KEY_SPACE{
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
   if i > n { i = n }
   if i <= 0 { return 0 }
   i -= 1
   while i > 0 {
      def b = load8(s, i)
      if (b & 0xC0) != 0x80 { break }
      i -= 1
   }
   i
}

fn _utf8_next_index(str s, int idx) int {
   mut i = int(idx)
   def n = str.len(s)
   if i < 0 { i = 0 }
   if i >= n { return n }
   i += 1
   while i < n {
      def b = load8(s, i)
      if (b & 0xC0) != 0x80 { break }
      i += 1
   }
   i
}

fn _text_cursor_from_x(int font_id, any s, f64 px, f64 text_x) int {
   def txt = _ui_text(s)
   def target = float(px) - float(text_x)
   if target <= 0.0 { return 0 }
   def n = str.len(txt)
   mut i = 0
   while i < n {
      def next_i = _utf8_next_index(txt, i)
      def before = str._substr(txt, 0, i)
      def thru = str._substr(txt, 0, next_i)
      def bw_m = _measure_text_ui(font_id, before)
      def bw = float(bw_m.get(0, 0.0))
      def nw_m = _measure_text_ui(font_id, thru)
      def nw = float(nw_m.get(0, 0.0))
      if target < (bw + nw) * 0.5 { return i }
      i = next_i
   }
   n
}

fn _text_state_cursor(str full_id, int fallback_len) int {
   def st = _widget_state.get(full_id, 0)
   mut cur = is_dict(st) ? int(st.get("cursor", fallback_len)) : int(fallback_len)
   def n = int(fallback_len)
   if cur < 0 { cur = 0 } elif cur > n { cur = n }
   cur
}

fn _text_state_set_cursor(str full_id, int cursor_idx) any {
   mut st = _widget_state.get(full_id, 0)
   if !is_dict(st) { st = dict(4) }
   st["cursor"] = int(cursor_idx)
   _widget_state[full_id] = st
}

fn begin_frame(any win=0, int font=0, f64 ww=0.0, f64 wh=0.0, f64 dt=0.016666667) any {
   "Starts a GUI frame and resets per-frame hover/layout state. `dt` is seconds and advances keyed UI animations."
   _ensure_gui_state()
   if ww > 0.0 { _gui_view_w = ww }
   if wh > 0.0 { _gui_view_h = wh }
   _gui_frame_id += 1
   _overlay_offsets = dict(16)
   _theme_hot_reload_tick()
   ui_anim.step(_ui_animator, dt)
   if is_list(_accessibility_nodes) { clear(_accessibility_nodes) } else { _accessibility_nodes = [] }
   if _accessibility_request_count > 0 {
      for request_id in _accessibility_requests.keys() {
         def request = _accessibility_requests.get(request_id)
         if float(request.get("expires", -1.0)) < float(_gui_frame_id) {
            _accessibility_requests = _accessibility_requests.delete(request_id)
            _accessibility_request_count -= 1
         }
      }
   }
   if !_text_caches_ready { _ensure_text_caches() }
   if win { prepare_input(win, ww, wh) }
   else { _theme_refresh() }
   if font {
      _font = font
      if !_font_title { _font_title = _font }
      if !_font_small { _font_small = _font }
   }
   _hot_id, _hot_rect = "", [0.0, 0.0, 0.0, 0.0]
   _same_line_next, _same_line_gap = false, _gap()
   _text_runs_reset()
   _popup_next_hit_active = false
   _popup_next_hit_window = ""
   _popup_combo_active = false
   _popup_combo_labels = []
   _popup_combo_style = nil
   _ov_cmds, _ov_stack, _ov_pending = [], [], []
   _ov_top, _win_layers, _dock_frame_active = [], [], false
   _ov_depth, _ov_discard, _popup_depth = 0, 0, 0
   _ctx_stack, _id_stack, _indent_stack, _col_stack = [], [], [], []
   _row_n, _tbl_id, _menubar_active = 0, "", false
   _tip_text, _last_item_id = "", ""
   if _disabled_on > 0 { _disabled_restore() }
   _disabled_depth = 0
   gui_batch.reset()
   _sync_current_layout_cache(0)
   _focus_collect_active = false
}

fn _flush_popup_draw_queue() any {
   if _popup_combo_active {
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
         _popup_combo_labels,
         _popup_combo_style
      )
   }
}

fn end_frame() any {
   "Finishes a GUI frame, draws debug overlays, and clears transient input."
   if !_enabled { return 0 }
   def prof = ui_profile.enabled()
   def t_flush = prof ? ui_profile.now() : 0
   def run_count = prof ? _text_runs_queued() : 0
   _flush_popup_draw_queue()
   _flush_text_runs()
   _overlay_finish()
   _ime_end_frame()
   if prof { ui_profile.mark("flush_text", t_flush, " runs=" + to_str(run_count)) }
   def t_tail = prof ? ui_profile.now() : 0
   if !_mouse_down0 {
      _active_window_move, _active_window_resize, _active_widget_id = "", "", ""
   }
   if _debug_overlay {
      if _hot_rect.len >= 4 && (_hot_id != "") {
         _gui_rect_outline_fast(float(_hot_rect.get(0, 0.0)), float(_hot_rect.get(1, 0.0)),
            float(_hot_rect.get(2, 0.0)), float(_hot_rect.get(3, 0.0)), _accent_u32)
      }
      if _active_rect.len >= 4 && (_active_widget_id != "") {
         _gui_rect_outline_fast(float(_active_rect.get(0, 0.0)), float(_active_rect.get(1, 0.0)),
            float(_active_rect.get(2, 0.0)), float(_active_rect.get(3, 0.0)), _danger_u32)
      }
      if _window_hovered != "" {
         def st = _window_lookup(_window_hovered)
         if is_dict(st) {
            _gui_rect_outline_fast(float(st.get("x", 0.0)), float(st.get("y", 0.0)),
               float(st.get("w", 0.0)), float(st.get("h", 0.0)), _ok_u32)
         }
      }
      _gui_rect_fast(_mouse_x - 6.0, _mouse_y, 13.0, 1.0, _warn_u32)
      _gui_rect_fast(_mouse_x, _mouse_y - 6.0, 1.0, 13.0, _warn_u32)
   }
   _flush_rect_batch()
   _apply_cursor()
   if prof { ui_profile.mark("end_frame_tail", t_tail) }
   _popup_hit_active = _popup_next_hit_active
   _popup_hit_window = _popup_next_hit_window
   _popup_hit_x, _popup_hit_y = _popup_next_hit_x, _popup_next_hit_y
   _popup_hit_w, _popup_hit_h = _popup_next_hit_w, _popup_next_hit_h
   _key_events = []
   _clear_key_masks()
   _text_events = ""
   _popup_dismissed = []
   _mouse_pressed1 = false
   if _dnd_active && !_mouse_down0 {
      _dnd_active, _dnd_type, _dnd_label, _dnd_source = false, "", "", ""
      _dnd_payload = nil
   }
}

fn wants_mouse() bool { "Returns true when GUI wants mouse input." _enabled && _wants_mouse }

fn wants_keyboard() bool { "Returns true when GUI wants keyboard input." _enabled && _wants_keyboard }

fn hovered_id() str { "Returns the currently hovered widget id." _hot_id }

fn active_id() str { "Returns the currently active widget id." _active_widget_id }

fn focused_id() str {
   "Returns the active text or keyboard focus id."
   if _text_focus_id != "" { return _text_focus_id }
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
   if key.len == 0 { return false }
   _focused_window = key
   _window_order_touch(key, true)
   true
}

fn animate_value(any id, f64 target, dict spring=dict(0)) f64 {
   "Returns a keyed spring value for this GUI frame. Call each frame with a moving target; begin_frame advances by its dt. First use snaps to target."
   ui_anim.value(_ui_animator, _wid("animation:" + to_str(id)), target, spring)
}

fn animation_busy() bool {
   "Returns whether any keyed GUI animation requested this frame still needs another frame."
   ui_anim.busy(_ui_animator)
}

fn overlay_offset(any id, bool open, f64 distance=12.0) f64 {
   "Returns this frame's offset for an overlay: `distance` while closed, then an eased slide to 0 over ten frames after it opens."
   _ensure_gui_state()
   def key = _wid("overlay:" + to_str(id))
   def slot = to_str(_gui_frame_id) + "|" + key
   def any cached = _overlay_offsets.get(slot, nil)
   if cached != nil { return float(cached) }
   mut f64 v = distance
   if !open {
      _overlay_state = _overlay_state.delete(key)
   } else {
      def state = _overlay_state.get(key, nil)
      if !is_dict(state) {
         _overlay_state[key] = {"age": 0, "frame": int(_gui_frame_id)}
      } else {
         mut int age = int(state.get("age", 0))
         if int(state.get("frame", -1)) != int(_gui_frame_id) {
            age += 1
            _overlay_state[key] = {"age": age, "frame": int(_gui_frame_id)}
         }
         v = distance * (1.0 - ui_anim.ease_out_cubic(min(1.0, float(age) / 10.0)))
      }
   }
   _overlay_offsets[slot] = v
   v
}

fn request_focus(any full_id) bool {
   "Requests keyboard/text focus for a full widget id."
   def fid = to_str(full_id)
   if fid.len == 0 { return false }
   _nav_focus_target, _kbd_focus_id, _text_focus_id = fid, fid, fid
   _active_widget_id = ""
   def sep = str.find(fid, "::")
   if sep > 0 {
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
   if !is_dict(st) {
      st = dict(8)
      st["id"] = key
   }
   st["open"] = !!visible
   _window_store(st)
   if !visible {
      if _focused_window == key { _focused_window = "" }
      if _window_hovered == key { _window_hovered = "" }
      if _active_window_move == key { _active_window_move = "" }
      if _active_window_resize == key { _active_window_resize = "" }
      if _nav_focus_window == key {
         _nav_focus_window, _nav_focus_target = "", ""
      }
      if _full_id_window_id(_kbd_focus_id) == key { _kbd_focus_id = "" }
      if _full_id_window_id(_text_focus_id) == key { _text_focus_id = "" }
      if _full_id_window_id(_active_widget_id) == key { _active_widget_id = "" }
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
   if key.len == 0 { return false }
   def hit = bool(_window_closed_events.get(key, false))
   if hit { _window_closed_events[key] = false }
   hit
}

fn reset_window_scroll(any id) bool {
   "Resets stored scroll state for a named GUI window."
   def key = to_str(id)
   if key.len == 0 { return false }
   mut st = _window_lookup(key)
   if !is_dict(st) { return false }
   st["scroll_y"] = 0.0
   st["content_h"] = 0.0
   _window_store(st)
   true
}

fn set_window_pos(any id, f64 x, f64 y) any {
   "Sets stored position for a named GUI window."
   mut st = _window_lookup(id)
   if !is_dict(st) { return 0 }
   if to_str(st.get("dock", "")) != "" { return 0 }
   st["x"] = float(x)
   st["y"] = float(y)
   st = _clamp_window_pos(st)
   _window_store(st)
}

fn set_window_size(any id, f64 w, f64 h) any {
   "Sets stored size for a named GUI window."
   mut st = _window_lookup(id)
   if !is_dict(st) { return 0 }
   if to_str(st.get("dock", "")) != "" { return 0 }
   st["w"] = max(float(w), _min_win_w())
   st["h"] = max(float(h), _min_win_h())
   st = _clamp_window_pos(st)
   _window_store(st)
}

fn window_rect(any id) list {
   "Returns the stored rectangle for a named GUI window."
   def st = _window_lookup(id)
   if !is_dict(st) { return [0.0, 0.0, 0.0, 0.0] }
   [
      float(st.get("x", 0.0)),
      float(st.get("y", 0.0)),
      float(st.get("w", 0.0)),
      float(st.get("h", 0.0))
   ]
}

fn _ensure_window(any id, any title, f64 x, f64 y, f64 w, f64 h, any opts=0) dict {
   _ensure_gui_state()
   def key = to_str(id)
   mut st = _windows.get(key, 0)
   if !is_dict(st) {
      st = dict(32)
      st["id"] = key
   }
   if !st.contains("id") { st["id"] = key }
   if !st.contains("title") { st["title"] = to_str(title) }
   if !st.contains("x") { st["x"] = _snap(x) }
   if !st.contains("y") { st["y"] = _snap(y) }
   if !st.contains("w") || float(st.get("w", 0.0)) <= 0.0 { st["w"] = max(_snap(w), _min_win_w()) }
   if !st.contains("h") || float(st.get("h", 0.0)) <= 0.0 { st["h"] = max(_snap(h), _min_win_h()) }
   def opts_is_dict = is_dict(opts)
   mut opt_open = true
   mut opt_collapsed = false
   mut opt_closable = true
   mut opt_movable = true
   mut opt_resizable = true
   mut opt_scrollable = true
   mut opt_collapsible = true
   mut opt_titlebar = true
   if opts_is_dict {
      opt_open, opt_collapsed = opts.get("open", true), opts.get("collapsed", false)
      opt_closable, opt_movable = opts.get("closable", true), opts.get("movable", true)
      opt_resizable, opt_scrollable = opts.get("resizable", true), opts.get("scrollable", true)
      opt_collapsible, opt_titlebar = opts.get("collapsible", true), opts.get("titlebar", true)
   }
   if !st.contains("open") { st["open"] = opt_open }
   if !st.contains("collapsed") { st["collapsed"] = opt_collapsed }
   if !st.contains("closable") { st["closable"] = opt_closable }
   if !st.contains("movable") { st["movable"] = opt_movable }
   if !st.contains("resizable") { st["resizable"] = opt_resizable }
   if !st.contains("scrollable") { st["scrollable"] = opt_scrollable }
   if !st.contains("collapsible") { st["collapsible"] = opt_collapsible }
   if !st.contains("titlebar") { st["titlebar"] = opt_titlebar }
   if !st.contains("scroll_y") { st["scroll_y"] = 0.0 }
   if !st.contains("content_h") { st["content_h"] = 0.0 }
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
   if opts_is_dict {
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
   if to_str(st.get("dock", "")) != "" {
      ;; The dockspace draws the tab and owns the rectangle.
      st["titlebar"] = false
      st["movable"] = false
      st["resizable"] = false
      st["collapsible"] = false
      st["collapsed"] = false
   } else {
      st = _clamp_window_pos(st)
   }
   _window_store(st)
   _window_order_touch(key, false)
   st
}

fn _register_focusable(any full_id) bool {
   mut win = _current_window_state()
   if !is_dict(win) { return false }
   def fid = to_str(full_id)
   if fid.len == 0 { return false }
   if _disabled_on > 0 {
      if _kbd_focus_id == fid { _kbd_focus_id = "" }
      return false
   }
   if !_focus_collect_active { return false }
   mut ids = win.get("focusables", [])
   if !is_list(ids) { ids = [] }
   mut seen = win.get("focusable_map", 0)
   if !is_dict(seen) {
      seen = dict(64)
      mut i = 0
      def ids_n = ids.len
      while i < ids_n {
         def existing = to_str(ids.get(i, ""))
         if existing.len > 0 { seen[existing] = true }
         i += 1
      }
   }
   if !seen.contains(fid) {
      if !is_list(ids) { ids = [] }
      ids = ids.append(fid)
      seen[fid] = true
      win["focusables"] = ids
      win["focusable_map"] = seen
      _window_store(win)
      _current_window = win
   }
   if _nav_focus_target == fid && (_nav_focus_window == "" || _nav_focus_window == _current_window_id_cache) {
      _kbd_focus_id = fid
      _focused_window = _current_window_id_cache
      _nav_focus_target = ""
      _nav_focus_window = ""
      return true
   }
   false
}

fn _key_event_present(int key) bool {
   if _key_mask_has(key) { return true }
   if !is_list(_key_events) { return false }
   mut i = 0
   def events_n = _key_events.len
   while i < events_n {
      if int(_key_events.get(i, 0)) == int(key) { return true }
      i += 1
   }
   false
}

fn _widget_key_activate(any full_id) bool { _disabled_on == 0 && (_kbd_focus_id == full_id) && (_key_event_present(KEY_ENTER) || _key_event_present(KEY_SPACE)) }

fn _accessibility_take(any full_id, str action) dict {
   if _accessibility_request_count <= 0 || _disabled_on > 0 { return _A11Y_EMPTY }
   def key = to_str(full_id)
   def request = _accessibility_requests.get(key, nil)
   if !is_dict(request) || to_str(request.get("action", "")) != action { return _A11Y_EMPTY }
   _accessibility_requests = _accessibility_requests.delete(key)
   _accessibility_request_count -= 1
   request
}

fn _text_selected_all(str full_id) bool { _state_get_num(full_id, "select_all", 0.0) > 0.5 }

fn _text_set_selected_all(str full_id, bool selected) any { _state_set_num(full_id, "select_all", selected ? 1.0 : 0.0) }

fn _begin_interact_widget(any full_id, f64 x, f64 y, f64 w, f64 h, bool visible=true) any {
   ;; Called for every widget each frame: keep the clip in scalars. Building
   ;; the widget and clip rects as lists leaked two lists per widget per frame.
   mut cx, cy, cw, ch = x, y, w, h
   def win = _current_window_state()
   if is_dict(win) {
      def override = win.get("clip_override", 0)
      if is_list(override) && override.len >= 4 {
         cx, cy = float(override.get(0, 0.0)), float(override.get(1, 0.0))
         cw, ch = float(override.get(2, 0.0)), float(override.get(3, 0.0))
      } else {
         def title_h = win.get("titlebar", true) ? _title_h() : 0.0
         cx = float(win.get("x", 0.0)) + _pad()
         cy = float(win.get("y", 0.0)) + title_h
         cw = max(1.0, float(win.get("w", _min_win_w())) - _pad() * 2.0)
         ch = max(1.0, float(win.get("h", _min_win_h())) - title_h)
      }
   }
   _iw_visible = !!visible && !(x + w <= cx || cx + cw <= x || y + h <= cy || cy + ch <= y)
   _last_item_id = full_id
   def focus_request = _accessibility_take(full_id, "focus")
   if focus_request.get("found", false) {
      _kbd_focus_id = full_id
      if _current_window_id_cache.len > 0 { _focused_window = _current_window_id_cache }
   }
   if _disabled_on > 0 {
      _iw_hovered, _iw_held, _iw_released, _iw_clicked = false, false, false, false
      if _active_widget_id == full_id { _active_widget_id = "" }
      return 0
   }
   _iw_hovered = _iw_visible && _rect_hit(_mouse_x, _mouse_y, cx, cy, cw, ch) && _current_window_pointer_hit(x, y, w, h)
   if _iw_hovered {
      _hot_id = full_id
      _hot_rect = [x, y, w, h]
      _ensure_cursors()
      _want_cursor(_cursor_hand)
   }
   if _iw_hovered && _mouse_pressed0 {
      if _text_focus_id != "" && _text_focus_id != full_id { _text_focus_id = "" }
      _active_widget_id = full_id
      _kbd_focus_id = full_id
      _active_rect = [x, y, w, h]
      if _current_window_id_cache.len > 0 { _focused_window = _current_window_id_cache }
   }
   _iw_held = (_active_widget_id == full_id) && _mouse_down0
   _iw_released = (_active_widget_id == full_id) && _mouse_released0
   _iw_clicked = _iw_released && _iw_hovered
   0
}

fn _accessibility_record(any full_id, str role, str name, any value, any bounds, bool checked=false, bool selected=false, bool expanded=false, bool disabled=false, any extra=nil) any {
   if !_iw_visible { return 0 }
   ;; Store a compact raw tuple on the hot widget path; materialize dicts and
   ;; action lists lazily only when accessibility_tree() / accessibility_action()
   ;; is actually queried.
   _accessibility_nodes = _accessibility_nodes.append([
         full_id, _current_window_id_cache, role, name, value, bounds,
         disabled || _disabled_on > 0, _kbd_focus_id == full_id, _iw_hovered,
         _iw_held, checked, selected, expanded, extra
      ])
   0
}

fn _accessibility_materialize(any raw) dict {
   if is_dict(raw) { return raw }
   if !is_list(raw) || raw.len < 14 { return dict(0) }
   def role = to_str(raw.get(2, ""))
   mut actions = []
   if role == "button" || role == "checkbox" || role == "radio" || role == "switch" || role == "option" || role == "tab" || role == "combobox" || role == "menuitem" || role == "treeitem" || role == "slider" || role == "spinbutton" || role == "textbox" {
      actions = actions.append("focus")
   }
   if role == "button" || role == "checkbox" || role == "radio" || role == "switch" || role == "option" || role == "tab" || role == "combobox" || role == "menuitem" || role == "treeitem" {
      actions = actions.append("activate")
   }
   if role == "slider" || role == "spinbutton" {
      actions = actions.append("set_value").append("increment").append("decrement")
   }
   if role == "textbox" { actions = actions.append("set_value") }
   mut dict node = {
      "id": to_str(raw.get(0, "")),
      "parent": "window:" + to_str(raw.get(1, "")),
      "role": role,
      "name": to_str(raw.get(3, "")),
      "value": raw.get(4, nil),
      "bounds": raw.get(5, []),
      "disabled": bool(raw.get(6, false)),
      "focused": bool(raw.get(7, false)),
      "hovered": bool(raw.get(8, false)),
      "pressed": bool(raw.get(9, false)),
      "checked": bool(raw.get(10, false)),
      "selected": bool(raw.get(11, false)),
      "expanded": bool(raw.get(12, false)),
      "actions": actions
   }
   def extra = raw.get(13, nil)
   if is_dict(extra) { for k in extra.keys() { node[k] = extra.get(k) } }
   node
}

fn accessibility_tree() dict {
   "Returns the current GUI frame's semantic window/widget tree for assistive technology and inspection."
   mut windows = []
   mut by_id = dict(16)
   mut roots = []
   for node in _accessibility_nodes {
      if !is_dict(node) { continue }
      def int index = windows.len
      windows = windows.append({"id": node.get("id", ""), "role": "window", "name": node.get("name", ""), "bounds": node.get("bounds", []), "disabled": node.get("disabled", false), "focused": node.get("focused", false), "children": []})
      by_id[to_str(node.get("id", ""))] = index
   }
   for raw in _accessibility_nodes {
      if !is_list(raw) { continue }
      def node = _accessibility_materialize(raw)
      def parent = to_str(node.get("parent", ""))
      if by_id.contains(parent) {
         def int index = int(by_id.get(parent, -1))
         def win = windows.get(index, dict(0))
         win["children"] = win.get("children", []).append(node)
         windows = windows.set(index, win)
      } else { roots = roots.append(node) }
   }
   {"role": "application", "children": windows, "orphans": roots}
}

fn accessibility_action(any id, any action, any value=nil) bool {
   "Queues an action advertised by the current accessibility tree for the next frame. Pass the node id from accessibility_tree; actions are consumed by that widget when it is submitted."
   def node_id, act = to_str(id), to_str(action)
   mut target = nil
   for raw in _accessibility_nodes {
      def rid = is_dict(raw) ? to_str(raw.get("id", "")) : (is_list(raw) && raw.len > 0 ? to_str(raw.get(0, "")) : "")
      if rid == node_id { target = _accessibility_materialize(raw) break }
   }
   if !is_dict(target) || target.get("disabled", false) { return false }
   def available = target.get("actions", [])
   if !is_list(available) || _list_find_text(available, act) < 0 { return false }
   if !_accessibility_requests.contains(node_id) { _accessibility_request_count += 1 }
   _accessibility_requests[node_id] = {"action": act, "value": value, "found": true, "expires": float(_gui_frame_id + 1)}
   true
}

fn _sync_current_layout_cache(any st) any {
   if !is_dict(st) {
      _current_window_id_cache = ""
      _widget_id_prefix = ""
      _current_body_x_cache = 0.0
      _current_body_y_cache = 0.0
      _current_body_w_cache = 0.0
      _current_body_h_cache = 0.0
      _current_cursor_y_cache = 0.0
      return 0
   }
   _current_window_id_cache = to_str(st.get("id", ""))
   _widget_id_prefix = _current_window_id_cache + "::"
   _current_body_x_cache = float(st.get("body_x", 0.0))
   _current_body_y_cache = float(st.get("body_y", 0.0))
   _current_body_w_cache = max(1.0, float(st.get("body_w", _sx(100.0))))
   _current_body_h_cache = max(1.0, float(st.get("body_h", 1.0)))
   _current_cursor_y_cache = float(st.get("cursor_y", _current_body_y_cache))
}

@inline
@jit
fn _wid(any id) str {
   _widget_id_prefix + (is_str(id) ? id : to_str(id))
}

@inline
@jit
fn _state_get_bool(any full_id, bool fallback=false) bool {
   def st = _widget_state.get(full_id, 0)
   if is_dict(st) { return st.get("value", fallback) }
   fallback
}

@inline
@jit
fn _state_set_bool(any full_id, bool v) any {
   mut st = _widget_state.get(full_id, 0)
   if !is_dict(st) { st = dict(4) }
   st["value"] = !!v
   _widget_state[full_id] = st
}

fn _debug_forced_combo_id() str {
   if _debug_force_combo_open == "\x00" { _debug_force_combo_open = ui_profile.env_trim_cached("NY_UI_GUI_FORCE_COMBO_OPEN") }
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
   if !is_dict(st) { st = dict(8) }
   st[key] = float(v)
   _widget_state[full_id] = st
}

@inline
@jit
fn _current_body_w() f64 {
   if _current_window_id_cache.len <= 0 { return _sx(100.0) }
   max(1.0, _current_body_w_cache)
}

fn remaining_w() f64 {
   "Returns the width of the current window body(the layout's available row width)."
   _current_body_w()
}

fn remaining_h(any reserve=0.0) f64 {
   "Returns remaining vertical room in the current window body."
   max(0.0,
      (_current_body_y_cache + _current_body_h_cache) -
      _current_cursor_y_cache - _gap() - float(reserve))
}

fn _current_clip() list {
   def win = _current_window_state()
   if !is_dict(win) { return [0.0, 0.0, _fb_w, _fb_h] }
   _window_active_clip(win)
}

fn push_clip_rect(any x, any y=0.0, any w=0.0, any h=0.0) any {
   "Pushes a scissor clip rectangle for subsequent GUI drawing."
   if _ov_depth > 0 {
      if is_list(x) { _ov_push(3, float(x.get(0, 0.0)), float(x.get(1, 0.0)), float(x.get(2, 0.0)), float(x.get(3, 0.0)), 0) }
      else { _ov_push(3, float(x), float(y), float(w), float(h), 0) }
      return 0
   }
   _flush_text_runs()
   if is_list(x) {
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
   if _ov_depth > 0 {
      _ov_push(4, 0, 0, 0, 0, 0)
      return 0
   }
   _flush_text_runs()
   scissor_pop()
}

@jit
fn _layout_rect(any w, any h) list {
   if _current_window_id_cache.len <= 0 { return [0.0, 0.0, 0.0, 0.0] }
   if _row_n > 0 { return _row_take(max(1.0, float(h))) }
   mut x, y = _current_body_x_cache, _current_cursor_y_cache
   def desired_w, desired_h = max(1.0, float(w)), max(1.0, float(h))
   def body_x, body_w = _current_body_x_cache, max(1.0, _current_body_w_cache)
   def body_right = body_x + body_w
   if _same_line_next && _last_item.len >= 4 {
      x, y = float(_last_item.get(0, x)) + float(_last_item.get(2, 0.0)) + _same_line_gap, float(_last_item.get(1, y))
      def remaining = body_right - x
      def min_inline = min(desired_w, max(_sx(44.0), body_w * 0.30))
      if remaining + 1.0 < min_inline { x, y = body_x, _current_cursor_y_cache }
   }
   if x < body_x { x = body_x }
   if x >= body_right { x = body_x }
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
   if _current_window_id_cache.len <= 0 { return 0 }
   _current_cursor_y_cache += float(amount) * _gap()
}

fn paint_rect(f64 x, f64 y, f64 w, f64 h, any color) bool {
   "Fills a rectangle in the current window's draw order(a packed RGBA int or an [r, g, b, a] list); for custom views such as byte grids."
   if !_enabled || w <= 0.0 || h <= 0.0 { return false }
   _gui_rect_fast(x, y, w, h, _text_color_u32(color))
}

fn paint_text(f64 x, f64 y, any txt, any color=0, bool small=false) any {
   "Draws text at(x, y) in the current window's draw order, in the body or small font."
   if !_enabled { return 0 }
   def col = is_int(color) && int(color) == 0 ? _text_u32 : _text_color_u32(color)
   _queue_text_ui(small ? _font_small_id() : _font_body(), txt, _snap(x), _snap(y), col)
}

fn item_hovered() bool {
   "True when the pointer is over the last submitted item inside the visible clip."
   def x, y = float(_last_item.get(0, 0.0)), float(_last_item.get(1, 0.0))
   def w, h = float(_last_item.get(2, 0.0)), float(_last_item.get(3, 0.0))
   if !_current_window_pointer_hit(x, y, w, h) { return false }
   def clip = _current_clip()
   _rect_hit(_mouse_x, _mouse_y, float(clip.get(0, 0.0)), float(clip.get(1, 0.0)), float(clip.get(2, 0.0)), float(clip.get(3, 0.0)))
}

fn mouse_xy() list { "Returns the pointer position GUI sees, [x, y]." [_mouse_x, _mouse_y] }

fn spacer_px(any px=0.0) any {
   "Adds vertical layout spacing measured in pixels."
   if _current_window_id_cache.len <= 0 { return 0 }
   def h = max(0.0, float(px))
   if h <= 0.0 { return 0 }
   def x, y = _current_body_x_cache, _current_cursor_y_cache
   def w = _current_body_w_cache
   _current_cursor_y_cache = y + h
   _last_item = [x, y, w, h]
   _same_line_next = false
}

fn reset_cursor() bool {
   "Resets the current window layout cursor to the body origin."
   if _current_window_id_cache.len <= 0 { return false }
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
   if _rect_intersects(rect, clip) { _gui_rect_fast(float(rect.get(0, 0.0)), float(rect.get(1, 0.0)), float(rect.get(2, 0.0)), 1.0, _border_u32) }
}

fn _text_line(any msg, any color) any {
   def s = _ui_text(msg)
   def rect = _layout_rect(_current_body_w(), _text_h())
   def clip = _current_clip()
   def hit = _rect_intersects(rect, clip)
   if hit { _draw_text_ui_clipped(s, float(rect.get(0, 0.0)), float(rect.get(1, 0.0)), color, float(rect.get(2, 0.0)), rect) }
}

fn text(any msg) any { _text_line(msg, _text_u32) }

fn label(any msg) any { text(msg) }

fn text_colored(any msg, any color) any {
   "Draws a single text line in an RGBA color or a semantic token name such as \"text_dim\" or \"warn\"."
   if is_str(color) {
      _text_line(msg, _text_color_u32(theme_color(to_str(color))))
      return 0
   }
   _text_line(msg, [
         clamp(float(color.get(0, 1.0)), 0.0, 1.0),
         clamp(float(color.get(1, 1.0)), 0.0, 1.0),
         clamp(float(color.get(2, 1.0)), 0.0, 1.0),
         clamp(float(color.get(3, 1.0)), 0.0, 1.0)
      ])
}

fn property_row(any id, any label, any value, any tone=0) list {
   "Draws a two-column property row and returns its rectangle. `tone` is an RGBA list or a theme token name."
   def lbl = _ui_text(label)
   def val = _ui_text(value)
   mut tone_col = _text_u32
   if is_list(tone) { tone_col = _text_color_u32(tone) }
   elif is_str(tone) { tone_col = _text_color_u32(theme_color(to_str(tone))) }
   def row_h = max(_small_item_h(), _sx(22.0))
   def rect = _layout_rect(_current_body_w(), row_h)
   def clip = _current_clip()
   if !_rect_intersects(rect, clip) { return rect }
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def w, h = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   ;; Rows sit directly on the window surface; only the hovered row is filled,
   ;; so a long property list reads as a table instead of a stack of boxes.
   if _current_window_pointer_hit(x, y, w, h) { _gui_rect_fast(x, y, w, h, _panel_u32) }
   def label_w = clamp(w * 0.46, _sx(104.0), _sx(200.0))
   def tx = x + _item_pad_x()
   def ty = _text_center_y_for(_font_small_id(), y, h)
   def row_clip = _clip_rect_inset(x, y, w, h, 0.0)
   _draw_text_ui_small_clipped(lbl, tx, ty, _text_dim_u32, max(1.0, label_w - _item_pad_x() * 2.0), row_clip)
   _draw_text_ui_small_clipped(val, x + label_w, ty, tone_col, max(1.0, w - label_w - _item_pad_x()), row_clip)
   rect
}

fn _label_h() f64 {
   "Height of the caption line above a labelled field."
   _font_h_for(_font_small_id()) + _sx(4.0)
}

fn _field_h() f64 {
   "Height of an editable field: one body text line plus vertical padding."
   max(_text_h() + _sx(8.0), _sx(26.0))
}

fn _labelled_h(str lbl) f64 {
   "Total layout height of a field with an optional caption above it."
   (str.len(lbl) > 0) ? (_label_h() + _field_h()) : _field_h()
}

fn _gui_diag(f64 x0, f64 y0, f64 x1, f64 y1, f64 t, int color) any {
   "Draws a pixel-stepped diagonal stroke, `t` pixels tall, from(x0,y0) to(x1,y1)."
   def steps = int(max(abs(x1 - x0), 1.0))
   mut i = 0
   while i <= steps {
      def k = float(i) / float(steps)
      _gui_rect_fast(x0 + (x1 - x0) * k, y0 + (y1 - y0) * k, 1.0, t, color)
      i += 1
   }
}

fn _gui_check_mark(f64 bx, f64 by, f64 box, int color) any {
   "Draws a check mark inside the square at(bx,by) with side `box`."
   def u = box / 16.0
   def t = max(2.0, _snap(2.0 * u))
   _gui_diag(bx + 4.0 * u, by + 8.0 * u, bx + 7.0 * u, by + 11.0 * u, t, color)
   _gui_diag(bx + 7.0 * u, by + 11.0 * u, bx + 12.0 * u, by + 4.0 * u, t, color)
}

fn _gui_chevron(f64 cx, f64 cy, f64 size, bool down, int color) any {
   "Draws a filled disclosure triangle centred on(cx,cy): pointing down when open, right when closed."
   def half = max(2.0, _snap(size * 0.5))
   def n = int(half)
   mut i = 0
   while i <= n {
      def span = float(n - i) * 2.0 + 1.0
      if down {
         _gui_rect_fast(cx - float(n - i), cy - half * 0.5 + float(i), span, 1.0, color)
      } else {
         _gui_rect_fast(cx - half * 0.5 + float(i), cy - float(n - i), 1.0, span, color)
      }
      i += 1
   }
}

fn _gui_focus_ring(f64 x, f64 y, f64 w, f64 h) any {
   "Draws the keyboard focus indicator: a two-pixel accent frame, so it reads on filled and unfilled widgets in both themes."
   _gui_rect_outline_fast(x, y, w, h, _focus_u32)
   if w > 4.0 && h > 4.0 { _gui_rect_outline_fast(x + 1.0, y + 1.0, w - 2.0, h - 2.0, _focus_u32) }
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

fn _button_style(any style, str state, int background, int border, int text_color) list {
   if !is_dict(style) { return [background, border, text_color] }
   def result = style.get(state, nil)
   if !is_dict(result) { return [background, border, text_color] }
   mut int bg = background
   mut int edge = border
   mut int fg = text_color
   def b = result.get("background", nil)
   def e = result.get("border", nil)
   def t = result.get("text", nil)
   def f = result.get("fill", nil)
   def k = result.get("knob", nil)
   if is_list(b) && b.len >= 3 { bg = _theme_pack(_rgba_or_default(b, [0.0, 0.0, 0.0, 1.0])) }
   if is_list(e) && e.len >= 3 { edge = _theme_pack(_rgba_or_default(e, [0.0, 0.0, 0.0, 1.0])) }
   if is_list(t) && t.len >= 3 { fg = _theme_pack(_rgba_or_default(t, [0.0, 0.0, 0.0, 1.0])) }
   def fill = is_list(f) && f.len >= 3 ? _theme_pack(_rgba_or_default(f, [0.0, 0.0, 0.0, 1.0])) : _accent_u32
   def knob = is_list(k) && k.len >= 3 ? _theme_pack(_rgba_or_default(k, [0.0, 0.0, 0.0, 1.0])) : fg
   [bg, edge, fg, fill, knob]
}

@effects(all)
fn button_style_map(fnptr style_fn) dict {
   "Evaluates a style function for active, hovered, pressed and disabled states and returns a style map accepted by styled GUI widgets. Each callback result is a dict of RGBA background, border and/or text colors."
   mut styles = dict(4)
   for state in ["active", "hovered", "pressed", "disabled"] {
      def any result = style_fn(state)
      if is_dict(result) { styles[to_str(state)] = result }
   }
   styles
}

fn button(any id, any label, any w=0.0, any h=0.0, any style=nil) bool {
   "Draws a button and returns true when activated. Optional style maps active,
   hovered, pressed and disabled states to dicts with background, border and/or
   text RGBA colors. Missing states and colors use the current theme."
   def txt = _ui_text(label)
   mut bw = float(w)
   if bw <= 0.0 { bw = min(_current_body_w(), max(_sx(84.0), _text_fit_width(_font_body(), txt) + _item_pad_x() * 2.0)) }
   mut bh = float(h)
   if bh <= 0.0 { bh = max(_item_h(), _sx(26.0)) }
   button_at(id, txt, _layout_rect(bw, bh), style)
}

fn button_at(any id, any label, list rect, any style=nil) bool {
   "Draws an interactive button at a solved [x, y, w, h] rectangle without moving the window layout cursor."
   if !_enabled || _current_window_id_cache.len == 0 || rect.len < 4 { return false }
   if float(rect.get(2)) <= 0.0 || float(rect.get(3)) <= 0.0 { return false }
   def txt = _ui_text(label)
   def full = _wid(id)
   _register_focusable(full)
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww, hh = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   _begin_interact_widget(full, x, y, ww, hh, true)
   def visible = _iw_visible
   def hovered = _iw_hovered
   def held = _iw_held
   def focused = _kbd_focus_id == full
   mut active_style = style
   if is_nil(active_style) && _theme_button_styles.keys().len > 0 { active_style = _theme_button_styles }
   def a11y_activate = _accessibility_take(full, "activate")
   def clicked = _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false)
   _accessibility_record(full, "button", txt, "", rect)
   if visible {
      mut int bg = held ? _press_u32 : (hovered ? _panel_active_u32 : _panel_hover_u32)
      mut int edge = held ? _accent_u32 : (hovered ? _text_faint_u32 : _border_u32)
      mut int fg = _text_u32
      if is_nil(style) && !held {
         def hover_mix = animate_value("button:" + to_str(id), (hovered || focused) ? 1.0 : 0.0)
         bg = _theme_pack(ui_anim.mix_color(_panel_hover_rgba, _panel_active_rgba, hover_mix))
      }
      if !is_nil(active_style) {
         def state = _disabled_on > 0 ? "disabled" : (held ? "pressed" : (hovered ? "hovered" : "active"))
         def colors = _button_style(active_style, state, bg, edge, fg)
         bg, edge, fg = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      }
      _gui_rect_fast(x, y, ww, hh, bg)
      _gui_rect_outline_fast(x, y, ww, hh, edge)
      if focused { _gui_focus_ring(x, y, ww, hh) }
      _draw_centered_text(txt, x, y, ww, hh, fg)
   }
   clicked
}

fn _flex_color(any color) any {
   if !is_list(color) { return color }
   [float(color.get(0, 255)) / 255.0, float(color.get(1, 255)) / 255.0,
      float(color.get(2, 255)) / 255.0, float(color.get(3, 255)) / 255.0]
}

fn _draw_flex_rounded(list shape, any color) any {
   if _ov_depth > 0 { return _ov_push(7, shape, color, 0, 0, 0) }
   _flush_text_runs()
   _flush_rect_batch()
   def x, y = float(shape.get(0)), float(shape.get(1))
   def w, h = float(shape.get(2)), float(shape.get(3))
   def radius, thickness = float(shape.get(4)), float(shape.get(5))
   if thickness <= 0.0 { return draw_rect_rounded(x, y, w, h, radius, color) }
   def inset = thickness * 0.5
   draw_rect_rounded_lines_ex([x + inset, y + inset, w - thickness, h - thickness], max(0.0, radius - inset), 24, thickness, color)
}

fn _flex_font(dict data) int {
   def selected = data.get("font", 0)
   def size = int(clamp(float(data.get("font_size", 16)), 0.0, 256.0))
   mut font = 0
   if is_str(selected) {
      font = str.atoi(selected)
      if font <= 0 && selected.len > 0 { font = font_load(to_str(selected), size > 0 ? size : 16) }
   } else { font = int(selected) }
   if font <= 0 { font = _font_body() }
   font_at_size(font, size)
}

fn measure_flex_text(str text, dict node) list {
   "Measures text with the font and letter spacing used by draw_flex_commands. Pass {\"styled\": gui.measure_flex_text} to flex.solve."
   measure_text(_flex_font(node), text, float(node.get("letter_spacing", 0.0)))
}

fn _draw_flex_text(int font, str text, f64 x, f64 y, any color, f64 letter_spacing) any {
   if text.len == 0 { return 0 }
   if letter_spacing == 0.0 { return _queue_text_ui(font, text, x, y, color) }
   def px, py = _snap(x), _snap(y)
   def packed = _text_color_u32(color)
   if _ov_depth > 0 { return _ov_push(8, font, text, px, py, [packed, letter_spacing]) }
   _flush_text_runs()
   draw_text(font, text, px, py, packed, letter_spacing)
}

fn _flex_menu_at(any id, str label, list items, list rect) list {
   "Draws a declarative menu button at a solved rectangle and returns chosen item actions."
   mut list chosen = []
   def full = _wid(id)
   def pid = _popup_key(to_str(id) + "#menu")
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def w, h = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   if w <= 0.0 || h <= 0.0 { return chosen }
   _register_focusable(full)
   _begin_interact_widget(full, x, y, w, h, true)
   def is_open = _popup_index(pid) >= 0
   def a11y_activate = _accessibility_take(full, "activate")
   if _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false) {
      if is_open { _popup_close_from(_popup_index(pid)) }
      elif _list_find_text(_popup_dismissed, pid) < 0 { _popup_open_at(pid, x, y + h, _popup_depth) }
   }
   _accessibility_record(full, "button", label, "", rect, false, false, is_open, false, {"has_popup": true, "menu": true})
   if _iw_visible {
      def lit = _popup_index(pid) >= 0
      def bg = lit || _iw_hovered || _iw_held ? _panel_active_u32 : _panel_hover_u32
      def fg = _text_u32
      _gui_rect_fast(x, y, w, h, bg)
      _gui_rect_outline_fast(x, y, w, h, _border_u32)
      if _kbd_focus_id == full { _gui_focus_ring(x, y, w, h) }
      def ty = _text_center_y_for(_font_small_id(), y, h)
      _draw_text_ui_small_clipped(label, x + _sx(10.0), ty, fg, max(1.0, w - _sx(30.0)), _clip_rect_inset(x, y, w, h, 0.0))
      _gui_chevron(x + w - _sx(12.0), y + h * 0.5, _sx(8.0), false, fg)
   }
   if _popup_begin(pid, 0.0, _sx(4.0)) {
      mut int index = 0
      for item in items {
         def item_key = to_str(item.get("id", ""))
         def item_id = item_key.len > 0 ? item_key : full + ":item:" + to_str(index)
         if menu_item(item_id, item.get("text", ""), item.get("shortcut", ""), bool(item.get("checked", false))) {
            chosen = chosen.append({"id": item_id, "action": item.get("action", ""), "value": item.get("checked", false)})
         }
         index += 1
      }
      _popup_end()
   }
   chosen
}

fn draw_flex_commands(list commands, f64 origin_x, f64 origin_y, dict values=dict(0)) list {
   "Draws flex commands at a screen origin and returns named actions. Stateful controls read values by id, default to the markup, and include the new value in change records."
   mut actions = []
   def window = _current_window_state()
   if !_enabled || !is_dict(window) { return actions }
   def saved_clip = window.get("clip_override", 0)
   def body = _current_clip()
   for command in commands {
      def kind = to_str(command.get("type", ""))
      if kind == "scissor_start" || kind == "scissor_end" { continue }
      def bounds = command.get("rect", [])
      if bounds.len < 4 { continue }
      def x = origin_x + float(bounds.get(0))
      def y = origin_y + float(bounds.get(1))
      def w = float(bounds.get(2))
      def h = float(bounds.get(3))
      def data = command.get("data", dict(0))
      def clip = command.get("clip", [])
      if clip.len >= 4 {
         def left = max(float(body.get(0)), origin_x + float(clip.get(0)))
         def top = max(float(body.get(1)), origin_y + float(clip.get(1)))
         def right = min(float(body.get(0)) + float(body.get(2)), origin_x + float(clip.get(0)) + float(clip.get(2)))
         def bottom = min(float(body.get(1)) + float(body.get(3)), origin_y + float(clip.get(1)) + float(clip.get(3)))
         if right <= left || bottom <= top { continue }
         def visible = [left, top, right - left, bottom - top]
         window["clip_override"] = visible
         push_clip_rect(visible)
      }
      if kind == "button" {
         def id = command.get("id", "")
         if button_at(id, data.get("text", ""), [x, y, w, h]) {
            actions = actions.append({"id": id, "action": data.get("action", "")})
         }
      } elif kind == "checkbox" && w > 0.0 && h > 0.0 {
         def id = to_str(command.get("id", ""))
         def checked = !!values.get(id, data.get("checked", false))
         def next = _choice_control_at(id, data.get("text", ""), checked, [x, y, w, h])
         if next != checked { actions = actions.append({"id": id, "action": data.get("action", ""), "value": next}) }
      } elif kind == "slider" && w > 0.0 && h > 0.0 {
         def id = to_str(command.get("id", ""))
         def value = float(values.get(id, data.get("value", 0.0)))
         def next = _slider_float_at(id, data.get("text", ""), value, float(data.get("min", 0.0)), float(data.get("max", 1.0)), [x, y, w, h], nil)
         if next != value { actions = actions.append({"id": id, "action": data.get("action", ""), "value": next}) }
      } elif kind == "input" && w > 0.0 && h > 0.0 {
         def id = to_str(command.get("id", ""))
         def value = to_str(values.get(id, data.get("value", "")))
         def next = _input_text_at(id, _ui_text(data.get("text", "")), value, data.get("placeholder", ""), [x, y, w, h], nil)
         if next != value { actions = actions.append({"id": id, "action": data.get("action", ""), "value": next}) }
      } elif kind == "spinbox" && w > 0.0 && h > 0.0 {
         def id = to_str(command.get("id", ""))
         def value = float(values.get(id, data.get("value", 0.0)))
         def next = _drag_float_at(id, _ui_text(data.get("text", "")), value, float(data.get("step", 1.0)), float(data.get("min", 0.0)), float(data.get("max", 0.0)), int(data.get("decimals", 0)), [x, y, w, h], nil)
         if next != value { actions = actions.append({"id": id, "action": data.get("action", ""), "value": next}) }
      } elif kind == "combo" && w > 0.0 && h > 0.0 {
         def id = to_str(command.get("id", ""))
         def selected = int(values.get(id, data.get("selected", 0)))
         def next = _combo_box_at(id, _ui_text(data.get("text", "")), data.get("options", []), selected, [x, y, w, h], int(data.get("max_visible", 6)), nil)
         if next != selected { actions = actions.append({"id": id, "action": data.get("action", ""), "value": next}) }
      } elif kind == "menu" && w > 0.0 && h > 0.0 {
         def menu_id = to_str(command.get("id", ""))
         for picked in _flex_menu_at(menu_id, _ui_text(data.get("text", "")), data.get("items", []), [x, y, w, h]) {
            actions = actions.append(picked)
         }
      } elif kind == "rect" {
         def radius = float(data.get("radius", 0.0))
         def color = _flex_color(data.get("color", 0))
         if radius > 0.0 { _draw_flex_rounded([x, y, w, h, radius, 0.0], color) }
         else { paint_rect(x, y, w, h, color) }
      } elif kind == "text" {
         def font = _flex_font(data)
         def tx = origin_x + float(data.get("x", bounds.get(0)))
         def ty = origin_y + float(data.get("y", bounds.get(1)))
         _draw_flex_text(font, data.get("text", ""), tx, ty, _flex_color(data.get("color", 0)), float(data.get("letter_spacing", 0.0)))
      } elif kind == "border" {
         def width = min(max(0.0, float(data.get("width", 1.0))), min(w, h) * 0.5)
         def color = _flex_color(data.get("color", 0))
         def radius = float(data.get("radius", 0.0))
         if width > 0.0 && radius > 0.0 {
            _draw_flex_rounded([x, y, w, h, min(radius, min(w, h) * 0.5), width], color)
         } else {
            paint_rect(x, y, w, width, color)
            paint_rect(x, y + h - width, w, width, color)
            paint_rect(x, y + width, width, h - width * 2.0, color)
            paint_rect(x + w - width, y + width, width, h - width * 2.0, color)
         }
      } elif kind == "image" {
         def texture = data.get("image", 0)
         if _tex_ref_id(texture) >= 0 {
            def tint = _rgba_or_default(_flex_color(data.get("tint", 0)), [1.0, 1.0, 1.0, 1.0])
            _gui_tex_ref_uv(x, y, w, h, texture, float(tint.get(0)), float(tint.get(1)), float(tint.get(2)), float(tint.get(3)))
         }
      }
      if clip.len >= 4 {
         pop_clip_rect()
         window["clip_override"] = saved_clip
      }
   }
   actions
}

fn small_button(any id, any label, any w=0.0, any h=0.0, any style=nil) bool {
   "Draws a compact button; optional style follows the button style contract."
   def txt = _ui_text(label)
   mut bw = float(w)
   if bw <= 0.0 { bw = _text_fit_width(_font_small_id(), txt) + _sx(14.0) }
   mut bh = float(h)
   if bh <= 0.0 { bh = _small_item_h() }
   def rect = _layout_rect(bw, bh)
   def full = _wid(id)
   _register_focusable(full)
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww, hh = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   _begin_interact_widget(full, x, y, ww, hh, true)
   def hovered = _iw_hovered
   def held = _iw_held
   def focused = _kbd_focus_id == full
   def a11y_activate = _accessibility_take(full, "activate")
   def clicked = _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false)
   _accessibility_record(full, "button", txt, "", rect)
   if _iw_visible {
      mut int bg = held ? _press_u32 : (hovered ? _panel_active_u32 : _panel_hover_u32)
      mut int edge = held ? _accent_u32 : (hovered ? _text_faint_u32 : _border_u32)
      mut int fg = _text_u32
      if !is_nil(style) {
         def state = _disabled_on > 0 ? "disabled" : (held ? "pressed" : (hovered ? "hovered" : "active"))
         def colors = _button_style(style, state, bg, edge, fg)
         bg, edge, fg = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      }
      _gui_rect_fast(x, y, ww, hh, bg)
      _gui_rect_outline_fast(x, y, ww, hh, edge)
      if focused { _gui_focus_ring(x, y, ww, hh) }
      _draw_centered_text_ex(_font_small_id(), txt, x, y, ww, hh, fg)
   }
   clicked
}

fn _rgba_or_default(any color, any fallback) list {
   if is_list(color) || is_tuple(color) {
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
   if !_rect_intersects(rect, _current_clip()) { return rect }
   def tex = _tex_ref_id(tex_id)
   if tex >= 0 {
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

fn icon_button(any id, any tex_id, any label="", f64 w=0.0, f64 h=0.0, bool selected=false, any style=nil) bool {
   "Draws an icon button with optional label, selected state, and state styles."
   def txt = _ui_text(label)
   def has_txt = str.len(txt) > 0
   mut bw = float(w)
   if bw <= 0.0 {
      def text_w = has_txt ? float(_measure_text_ui(_font_small_id(), txt).get(0, 0.0)) : 0.0
      bw = max(_sx(62.0), _sx(22.0) + _item_pad_x() * 2.0 + ((text_w > 0.0) ? (text_w + _tiny_gap()) : 0.0))
   }
   mut bh = float(h)
   if bh <= 0.0 { bh = max(_small_item_h() + _sx(4.0), _sx(28.0)) }
   def icon_sz = clamp(bh - _sx(12.0), _sx(18.0), _sx(24.0))
   def rect = _layout_rect(bw, bh)
   _last_item = rect
   def full = _wid(id)
   _register_focusable(full)
   _begin_interact_widget(full,
      float(rect.get(0,
            0.0)),
      float(rect.get(1,
            0.0)),
      float(rect.get(2,
            0.0)),
      float(rect.get(3,
            0.0)),
      true)
   def visible = _iw_visible
   def hovered = _iw_hovered
   def held = _iw_held
   def focused = _kbd_focus_id == full
   def a11y_activate = _accessibility_take(full, "activate")
   def clicked = _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false)
   if visible {
      def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      def rwf, rhf = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
      def state = _disabled_on > 0 ? "disabled" : (held ? "pressed" : (hovered ? "hovered" : "active"))
      def default_bg = selected ? _accent_soft_u32 : (held ? _press_u32 : (hovered ? _panel_hover_u32 : _panel_u32))
      def default_edge = held || selected || focused ? _accent_u32 : _border_u32
      def colors = _button_style(style, state, default_bg, default_edge, selected || focused || hovered || held ? _text_u32 : _text_dim_u32)
      def bg, edge, fg = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      _gui_rect_fast(x, y, rwf, rhf, bg)
      if selected || focused || hovered || held { _gui_rect_fast(x, y, rwf, held ? _sx(3.0) : _sx(2.0), _accent_u32) }
      _gui_rect_outline_fast(x, y, rwf, rhf, edge)
      def ix, iy = x + _item_pad_x(), y + max(0.0, (rhf - icon_sz) * 0.5)
      if _tex_ref_id(tex_id) >= 0 { _gui_tex_ref_uv(ix, iy, icon_sz, icon_sz, tex_id, 1.0, 1.0, 1.0, 0.96) } else {
         _gui_rect_fast(ix + icon_sz * 0.25,
            iy + icon_sz * 0.25,
            icon_sz * 0.50,
            icon_sz * 0.50,
            selected ? _accent_u32 : _text_dim_u32)
      }
      if has_txt {
         def text_x = ix + icon_sz + _tiny_gap()
         def text_y = _text_center_y_for(_font_small_id(), y, rhf)
         def text_w = max(1.0, rwf - icon_sz - _item_pad_x() * 2.0 - _tiny_gap())
         def text_clip = _clip_rect_inset(x, y, rwf, rhf, 1.0)
         _draw_text_ui_small_clipped(txt, text_x, text_y, fg, text_w, text_clip)
      }
   }
   clicked
}

fn selectable(any id, any label, bool selected=false, f64 w=0.0, f64 h=0.0, any detail="", any tex_id=-1, bool keyboard=true, any style=nil) bool {
   "Draws a selectable row with optional detail text and icon. Optional state styles use the button style-map contract."
   def txt = _ui_text(label)
   def d = _ui_text(detail)
   def rw = _pick_positive(w, _current_body_w())
   mut rh = float(h)
   if rh <= 0.0 {
      def m = _measure_text_ui(_font_body(), txt)
      rh = max(_small_item_h(), float(m.get(1, _small_item_h())) + ((d.len > 0) ? _text_h() : 0.0) + _sx(8.0))
   }
   def rect = _layout_rect(rw, rh)
   def full = _wid(id)
   _register_focusable(full)
   _begin_interact_widget(full,
      float(rect.get(0,
            0.0)),
      float(rect.get(1,
            0.0)),
      float(rect.get(2,
            0.0)),
      float(rect.get(3,
            0.0)),
      true)
   def visible = _iw_visible
   def hovered = _iw_hovered
   def held = _iw_held
   def focused = _kbd_focus_id == full
   def a11y_activate = _accessibility_take(full, "activate")
   def clicked = _iw_clicked || (keyboard && _widget_key_activate(full)) || a11y_activate.get("found", false)
   _accessibility_record(full, "option", txt, selected, rect, false, selected)
   if visible {
      def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      def rwf, rhf = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
      def clip = _clip_rect_inset(x, y, rwf, rhf, 1.0)
      def title_y = (d.len > 0) ? (y + _sx(5.0)) : _text_center_y_for(_font_body(), y, rhf)
      def icon_sz = (_tex_ref_id(tex_id) >= 0) ? min(_sx(22.0), max(_sx(14.0), rhf - _sx(10.0))) : 0.0
      def text_x = x + _item_pad_x() + _sx(4.0) + ((icon_sz > 0.0) ? (icon_sz + _sx(7.0)) : 0.0)
      def text_w = max(1.0, rw - (text_x - x) - _item_pad_x())
      def state = _disabled_on > 0 ? "disabled" : (held ? "pressed" : (hovered ? "hovered" : "active"))
      def defaults_bg = selected ? (held ? _select_u32 : _accent_soft_u32) : (held ? _panel_active_u32 : (hovered ? _panel_hover_u32 : 0))
      def colors = _button_style(style, state, defaults_bg, _border_u32, selected || hovered || focused ? _text_u32 : _text_dim_u32)
      def bg, edge, fg = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      ;; Unselected rows are transparent; hover, press and selection are the
      ;; only fills, and the selected row carries a leading accent bar.
      if !is_nil(style) || selected || held || hovered { _gui_rect_fast(x, y, rwf, rhf, bg) }
      def state_style = is_dict(style) ? style.get(state, nil) : nil
      if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(x, y, rwf, rhf, edge) }
      if selected { _gui_rect_fast(x, y, _sx(3.0), rhf, _accent_u32) }
      if focused { _gui_focus_ring(x, y, rwf, rhf) }
      if icon_sz > 0.0 {
         _gui_tex_ref_uv(x + _item_pad_x(), y + max(_sx(5.0), (rhf - icon_sz) * 0.5), icon_sz, icon_sz,
            tex_id, 1.0, 1.0, 1.0, selected ? 1.0 : 0.88)
      }
      _draw_text_ui_clipped(txt, text_x, title_y, fg, text_w, clip)
      if d.len > 0 { _draw_text_ui_small_clipped(d, text_x, y + _sx(5.0) + _text_h() + _tiny_gap(), _text_dim_u32, text_w, clip) }
   }
   clicked
}

fn selectable_file(any id, any label, bool selected=false, f64 w=0.0, f64 h=0.0, any tex_id=-1, any style=nil) bool {
   "Draws a lean selectable row for virtualized file lists, with optional state styles."
   def txt = _ui_text(label)
   def rw = _pick_positive(w, _current_body_w())
   def rh = _pick_positive(h, _small_item_h())
   def rect = _layout_rect(rw, rh)
   _last_item = rect
   def full = _wid(id)
   _register_focusable(full)
   _begin_interact_widget(full,
      float(rect.get(0,
            0.0)),
      float(rect.get(1,
            0.0)),
      float(rect.get(2,
            0.0)),
      float(rect.get(3,
            0.0)),
      true)
   _accessibility_record(full, "option", txt, selected, rect, false, selected)
   if _iw_visible {
      def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      def rwf, rhf = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
      def hovered = bool(_iw_hovered)
      def held = bool(_iw_held)
      def default_bg = selected ? _accent_soft_u32 : (held ? _panel_active_u32 : (hovered ? _panel_hover_u32 : _panel_u32))
      mut int bg = default_bg
      mut int edge = _border_u32
      mut int text_col = _text_u32
      def has_style = is_dict(style)
      def state = !has_style ? "" : (_disabled_on > 0 ? "disabled" : (held ? "pressed" : (hovered ? "hovered" : "active")))
      if has_style {
         def colors = _button_style(style, state, default_bg, _border_u32, _text_u32)
         bg = int(colors.get(0))
         edge = int(colors.get(1))
         text_col = int(colors.get(2))
      }
      def icon_sz = (_tex_ref_id(tex_id) >= 0) ? min(_sx(18.0), max(_sx(12.0), rhf - _sx(8.0))) : 0.0
      def text_x = x + _item_pad_x() + ((icon_sz > 0.0) ? (icon_sz + _sx(6.0)) : 0.0)
      def text_w = max(1.0, rwf - (text_x - x) - _item_pad_x())
      _gui_rect_fast(x, y, rwf, rhf, bg)
      if has_style {
         def state_style = style.get(state, nil)
         if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(x, y, rwf, rhf, edge) }
      }
      if selected || hovered { _gui_rect_fast(x, y, selected ? _sx(4.0) : _sx(2.0), rhf, selected ? _accent_u32 : _border_u32) }
      if icon_sz > 0.0 {
         _gui_tex_ref_uv(x + _item_pad_x(), y + max(_sx(4.0), (rhf - icon_sz) * 0.5), icon_sz, icon_sz,
            tex_id, 1.0, 1.0, 1.0, selected ? 1.0 : 0.82)
      }
      _draw_text_ui_ex(_font_body(), txt, text_x, _text_center_y_for(_font_body(), y, rhf), text_col, 0.0, text_w)
   }
   def a11y_activate = _accessibility_take(full, "activate")
   _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false)
}

fn _text_edit_apply_input(str full_id, str value, int cursor) list {
   mut str out = to_str(value)
   mut int cur = int(cursor)
   mut selected_all = _text_selected_all(full_id)
   if selected_all { cur = int(out.len) }
   mut ki = 0
   mut int key_events_n = 0
   if is_list(_key_events) { key_events_n = _key_events.len }
   while ki < key_events_n {
      def k = int(_key_events.get(ki, 0))
      if k == _TEXT_SELECT_ALL {
         selected_all = out.len > 0
         cur = out.len
      } elif k == _TEXT_COPY {
         if selected_all && out.len > 0 { uiw.set_clipboard(_win, out) }
      } elif k == _TEXT_CUT {
         if selected_all && out.len > 0 {
            uiw.set_clipboard(_win, out)
            out = ""
            cur = 0
            selected_all = false
         }
      } elif k == _TEXT_PASTE {
         def clip = uiw.get_clipboard(_win)
         if clip.len > 0 {
            if selected_all {
               out = clip
               cur = out.len
            } else {
               out = str._substr(out, 0, cur) + clip + str._substr(out, cur, out.len)
               cur += clip.len
            }
            selected_all = false
         }
      } elif k == KEY_BACKSPACE {
         if selected_all {
            out = ""
            cur = 0
            selected_all = false
         } elif cur > 0 {
            def prev = _utf8_prev_index(out, cur)
            out = str._substr(out, 0, prev) + str._substr(out, cur, out.len)
            cur = prev
         }
      } elif k == KEY_DELETE {
         if selected_all {
            out = ""
            cur = 0
            selected_all = false
         } elif cur < out.len {
            def nxt = _utf8_next_index(out, cur)
            out = str._substr(out, 0, cur) + str._substr(out, nxt, out.len)
         }
      } elif k == KEY_LEFT {
         selected_all = false
         cur = _utf8_prev_index(out, cur)
      } elif k == KEY_RIGHT {
         selected_all = false
         cur = _utf8_next_index(out, cur)
      } elif k == KEY_HOME {
         selected_all = false
         cur = 0
      } elif k == KEY_END {
         selected_all = false
         cur = out.len
      } elif k == KEY_ENTER {
         selected_all = false
         _text_focus_id = ""
         _kbd_focus_id = ""
      }
      ki += 1
   }
   if _text_events.len > 0 {
      if selected_all {
         out = _text_events
         cur = out.len
      } else {
         out = str._substr(out, 0, cur) + _text_events + str._substr(out, cur, out.len)
         cur += _text_events.len
      }
      selected_all = false
   }
   _text_set_selected_all(full_id, selected_all)
   [out, cur]
}

;; Removes UTF-8 bytes around the caret for an IME delete-surrounding request.
fn _ime_apply_delete(str value, int cursor) list {
   def n = value.len
   mut c = int(cursor)
   if c > n { c = n }
   if c < 0 { c = 0 }
   mut lo = c - _ime_delete_before
   mut hi = c + _ime_delete_after
   if lo < 0 { lo = 0 }
   if hi > n { hi = n }
   ;; The IME counts bytes; keep the cut on character boundaries.
   while lo > 0 && (load8(value, lo) & 0xC0) == 0x80 { lo -= 1 }
   while hi < n && (load8(value, hi) & 0xC0) == 0x80 { hi += 1 }
   _ime_delete_before = 0
   _ime_delete_after = 0
   [str._substr(value, 0, lo) + str._substr(value, hi, n), lo]
}

;; A focused text field asks the window for composition at its caret; the
;; rectangle is in window (surface) coordinates, the layout's are pixels.
fn _ime_claim(f64 cx, f64 cy, f64 ch) any {
   _ime_claimed = true
   if !_win { return 0 }
   mut sc = 1.0
   def scl = uiw.get_window_content_scale(_win)
   if is_list(scl) && scl.len > 0 { sc = max(0.01, float(scl.get(0, 1.0))) }
   uiw.text_input_start(_win, [int(cx / sc), int(cy / sc), 2, max(1, int(ch / sc))])
   _ime_active = true
}

fn _ime_end_frame() any {
   if _ime_active && !_ime_claimed {
      if _win { uiw.text_input_stop(_win) }
      _ime_active = false
      _ime_preedit = ""
      _ime_preedit_cursor = -1
   }
   _ime_claimed = false
}

fn ime_preedit() str {
   "Returns the input-method composition shown in the focused text field."
   _ime_preedit
}

fn list_view(any id, list rows, any w=0.0, any h=0.0, int selected=-1) int {
   "Draws a standalone scrollable list with click and Up/Down selection; returns the selected row index(-1 when none)."
   def rw = _pick_positive(w, _current_body_w())
   def rh = _pick_positive(h, _sx(220.0))
   def scroll_id = to_str(id) + "#scroll"
   mut int out = selected
   set_scroll_area_content_hint(scroll_id, float(rows.len) * _small_item_h() + _sx(8.0))
   if begin_scroll_area(scroll_id, rw, rh) {
      mut i = 0
      while i < rows.len {
         def row = rows.get(i)
         def label_text = is_dict(row) ? to_str(row.get("label", "")) : _ui_text(row)
         def detail_text = is_dict(row) ? to_str(row.get("detail", "")) : ""
         def int icon = is_dict(row) ? int(row.get("icon", -1)) : -1
         if selectable(to_str(id) + "#row" + to_str(i), label_text, i == out, 0.0, _small_item_h(), detail_text, icon) { out = i }
         i += 1
      }
      end_scroll_area()
   }
   def row_prefix = _wid(to_str(id) + "#row")
   if rows.len > 0 && str.startswith(_kbd_focus_id, row_prefix) {
      mut int next = out
      if _key_event_present(KEY_DOWN) { next = min(rows.len - 1, max(0, out) + 1) }
      elif _key_event_present(KEY_UP) { next = max(0, out - 1) }
      if next != out {
         out = next
         request_focus(_wid(to_str(id) + "#row" + to_str(next)))
      }
   }
   out
}

fn input_text(any id, any label, any value, any placeholder="", f64 w=0.0, any style=nil) str {
   "Draws an editable text field and returns the edited value. Optional state styles control field, border and text colors."
   def lbl = _ui_text(label)
   def rw = _pick_positive(w, _current_body_w())
   ;; The caption owns its own line above the field. Reserving a fixed height
   ;; for both let the field spill into the next widget's row.
   def rect = _layout_rect(rw, _labelled_h(lbl))
   _input_text_at(id, lbl, value, placeholder, rect, style)
}

fn _input_text_at(any id, str lbl, any value, any placeholder, list rect, any style) str {
   mut out = _ui_text(value)
   def full = _wid(id)
   def ph = _ui_text(placeholder)
   def f64 height = float(rect.get(3, 0.0))
   def f64 caption = lbl.len > 0 ? _label_h() : 0.0
   def f64 field_h = height - caption
   if field_h <= 0.0 { return out }
   _begin_interact_widget(full,
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
   def field_y = y + caption
   def tx = x + _item_pad_x()
   def field_hover = _current_window_pointer_hit(x, field_y, rwf, field_h)
   if field_hover {
      _ensure_cursors()
      _want_cursor(_cursor_ibeam)
   }
   def nav_focus = _register_focusable(full)
   def a11y_value = _accessibility_take(full, "set_value")
   if a11y_value.get("found", false) {
      out = _ui_text(a11y_value.get("value", ""))
      _kbd_focus_id = full
      _text_focus_id = full
      _text_state_set_cursor(full, out.len)
      _text_set_selected_all(full, false)
   }
   def mouse_focus = field_hover && _mouse_pressed0
   if nav_focus || mouse_focus || _iw_clicked {
      _kbd_focus_id = full
      _text_focus_id = full
      mut next_cursor = out.len
      if mouse_focus { next_cursor = _text_cursor_from_x(_font_body(), out, _mouse_x, tx) }
      _text_state_set_cursor(full, next_cursor)
      _text_set_selected_all(full, nav_focus && !mouse_focus && out.len > 0)
   }
   def active_text = _text_focus_id == full || _kbd_focus_id == full
   if active_text && _text_focus_id != full { _text_focus_id = full }
   mut cursor = _text_state_cursor(full, out.len)
   if active_text {
      if _ime_delete_before > 0 || _ime_delete_after > 0 {
         def ime_del = _ime_apply_delete(out, cursor)
         out, cursor = to_str(ime_del.get(0, out)), int(ime_del.get(1, cursor))
      }
      def edit = _text_edit_apply_input(full, out, cursor)
      out, cursor = to_str(edit.get(0, out)), int(edit.get(1, cursor))
      _text_state_set_cursor(full, cursor)
      def caret_before = _measure_text_ui(_font_body(), str._substr(out, 0, cursor))
      _ime_claim(tx + float(caret_before.get(0, 0.0)), field_y, field_h)
   }
   ;; The composition is drawn spliced in at the caret, underlined.
   def composing = active_text && _ime_preedit.len > 0
   if _iw_visible {
      def label_clip = _clip_rect_inset(x, y, rwf, _label_h(), 0.0)
      def field_clip = _clip_rect_inset(x, field_y, rwf, field_h, 1.0)
      def state = _disabled_on > 0 ? "disabled" : (active_text ? "pressed" : (field_hover ? "hovered" : "active"))
      def default_bg = active_text ? _window_bg_u32 : (field_hover ? _panel_hover_u32 : _panel_u32)
      def default_edge = active_text ? _accent_u32 : (field_hover ? _text_faint_u32 : _border_u32)
      def colors = _button_style(style, state, default_bg, default_edge, _text_u32)
      def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      _gui_rect_fast(x, field_y, rwf, field_h, bg)
      _gui_rect_outline_fast(x, field_y, rwf, field_h, edge)
      mut draw_s = (out.len > 0) ? out : ph
      mut draw_col = _text_faint_u32
      if out.len > 0 { draw_col = text_col }
      if composing {
         draw_s = str._substr(out, 0, cursor) + _ime_preedit + str._substr(out, cursor, str.len(out))
         draw_col = text_col
      }
      def ty = _text_center_y_for(_font_body(), field_y, field_h)
      if str.len(lbl) > 0 { _draw_text_ui_small_clipped(lbl, x, y, is_nil(style) ? _text_dim_u32 : text_col, rwf, label_clip) }
      if active_text && _text_selected_all(full) && out.len > 0 {
         def mw_sel = _measure_text_ui(_font_body(), out)
         _gui_rect_fast(tx - 1.0, field_y + _sx(4.0), min(rwf - _item_pad_x() * 2.0, float(mw_sel.get(0, 0.0)) + _sx(3.0)), max(_sx(8.0), field_h - _sx(8.0)), _select_u32)
      }
      _draw_text_ui_clipped(draw_s, tx, ty, draw_col, rwf - _item_pad_x() * 2.0, field_clip)
      mut caret_extra = ""
      if composing {
         def pre_x = tx + float(_measure_text_ui(_font_body(), str._substr(out, 0, cursor)).get(0, 0.0))
         def pre_w = float(_measure_text_ui(_font_body(), _ime_preedit).get(0, 0.0))
         _gui_rect_fast(pre_x, field_y + field_h - _sx(5.0), max(1.0, pre_w), 1.0, _accent_u32)
         if _ime_preedit_cursor >= 0 {
            caret_extra = str._substr(_ime_preedit, 0, _ime_preedit_cursor)
         } else {
            caret_extra = _ime_preedit
         }
      }
      if active_text && (composing || (int(get_time() * 2.0) % 2) == 0) {
         def before = str._substr(out, 0, cursor) + caret_extra
         def mw = _measure_text_ui(_font_body(), before)
         def cx = tx + float(mw.get(0, 0.0))
         _gui_rect_fast(cx, field_y + _sx(5.0), 1.0, max(_sx(8.0), field_h - _sx(10.0)), _accent_u32)
      }
   }
   _last_item = [x, field_y, rwf, field_h]
   _accessibility_record(full, "textbox", lbl.len > 0 ? lbl : "Text input", out, [x, field_y, rwf, field_h], false, false, false, false, {"placeholder": ph, "multiline": false})
   out
}

fn title_input_text(any id, any value, any placeholder="", any w=0.0) str {
   "Draws an editable text field inside the current window title bar."
   mut out = _ui_text(value)
   def win = _current_window_state()
   if !is_dict(win) { return out }
   def win_id = _current_window_id_cache
   def full = _wid(id)
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
   if hovered {
      _hot_id = full
      _hot_rect = [x, y, rw, field_h]
      _ensure_cursors()
      _want_cursor(_cursor_ibeam)
   }
   def nav_focus = _register_focusable(full)
   def mouse_focus = hovered && _mouse_pressed0
   if nav_focus || mouse_focus {
      _kbd_focus_id = full
      _text_focus_id = full
      _focused_window = win_id
      mut next_cursor = out.len
      if mouse_focus { next_cursor = _text_cursor_from_x(_font_small_id(), out, _mouse_x, tx) }
      _text_state_set_cursor(full, next_cursor)
      _text_set_selected_all(full, nav_focus && !mouse_focus && out.len > 0)
   }
   def active_text = _text_focus_id == full || _kbd_focus_id == full
   if active_text && _text_focus_id != full { _text_focus_id = full }
   mut cursor = _text_state_cursor(full, out.len)
   if active_text {
      def edit = _text_edit_apply_input(full, out, cursor)
      out, cursor = to_str(edit.get(0, out)), int(edit.get(1, cursor))
      _text_state_set_cursor(full, cursor)
   }
   def draw_s = (out.len > 0) ? out : _ui_text(placeholder)
   def draw_col = (out.len > 0) ? _text_u32 : _text_dim_u32
   def bg = active_text ? _panel_active_u32 : (hovered ? _panel_hover_u32 : _panel_u32)
   def restore_body_clip = bool(win.get("clip_active", false))
   if restore_body_clip { pop_clip_rect() }
   _gui_rect_fast(x, y, rw, field_h, bg)
   _gui_rect_outline_fast(x, y, rw, field_h, active_text ? _accent_u32 : _border_u32)
   if active_text && _text_selected_all(full) && out.len > 0 {
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
   if active_text && ((int(get_time() * 2.0) % 2) == 0) {
      def before = str._substr(out, 0, cursor)
      def mw = _measure_text_ui(_font_small_id(), before)
      def cx = min(x + rw - _sx(6.0), tx + float(mw.get(0, 0.0)))
      _gui_rect_fast(cx, y + _sx(4.0), 1.0, max(_sx(8.0), field_h - _sx(8.0)), _accent_u32)
   }
   if restore_body_clip { push_clip_rect(_window_body_clip(win)) }
   out
}

fn _draw_choice_label(any txt, any rect, any box, any bx, any by, int text_color=_text_u32) any {
   def ry, rh = float(rect.get(1, 0.0)), float(rect.get(3, 0.0))
   _draw_text_ui_clipped(
      txt, bx + box + _sx(8.0), _text_center_y_for(_font_body(), ry, rh), text_color,
      float(rect.get(2, 0.0)) - box - _sx(8.0),
      _clip_rect_inset(float(rect.get(0, 0.0)), ry, float(rect.get(2, 0.0)), rh, 0.0)
   )
}

fn _choice_control(any id, any label, any value, bool radio=false, any style=nil) bool {
   def rect = _layout_rect(_current_body_w(), max(_sx(22.0), _small_item_h()))
   _choice_control_at(id, label, value, rect, radio, style)
}

fn _choice_control_at(any id, any label, any value, list rect, bool radio=false, any style=nil) bool {
   def txt = _ui_text(label)
   def box = _sx(16.0)
   def full = _wid(id)
   _register_focusable(full)
   _begin_interact_widget(full, float(rect.get(0)), float(rect.get(1)), float(rect.get(2)), float(rect.get(3)), true)
   mut out = !!value
   def focused = _kbd_focus_id == full
   def a11y_activate = _accessibility_take(full, "activate")
   if _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false) { out = radio ? true : !out }
   _accessibility_record(full, radio ? "radio" : "checkbox", txt, out, rect, out)
   if _iw_visible {
      def bx = float(rect.get(0, 0.0)) + _sx(2.0)
      def by = _snap(float(rect.get(1, 0.0)) + (float(rect.get(3, 0.0)) - box) * 0.5)
      def state = _disabled_on > 0 ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered ? "hovered" : "active"))
      def defaults_bg = out ? _accent_u32 : (_iw_held ? _panel_active_u32 : (_iw_hovered ? _panel_hover_u32 : _panel_u32))
      def defaults_edge = out ? _accent_u32 : (_iw_hovered ? _text_faint_u32 : _border_u32)
      def colors = _button_style(style, state, defaults_bg, defaults_edge, _text_u32)
      def bg, edge, fg = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      ;; A set box is filled with the accent and carries a dark mark, so the
      ;; state reads at a glance and never depends on the mark's own colour.
      _gui_rect_fast(bx, by, box, box, bg)
      _gui_rect_outline_fast(bx, by, box, box, edge)
      if out {
         if radio { _gui_rect_fast(bx + _sx(5.0), by + _sx(5.0), box - _sx(10.0), box - _sx(10.0), fg) }
         else { _gui_check_mark(bx, by, box, fg) }
      }
      if focused { _gui_focus_ring(bx - _sx(2.0), by - _sx(2.0), box + _sx(4.0), box + _sx(4.0)) }
      _draw_choice_label(txt, rect, box, bx, by, fg)
   }
   out
}

fn checkbox(any id, any label, any value, any style=nil) bool { _choice_control(id, label, value, false, style) }

fn toggle(any id, any label, any value, any style=nil) bool {
   "Draws a toggle switch and returns the updated boolean value. Optional state styles use the button style-map contract."
   def txt = _ui_text(label)
   def tw = _toggle_w()
   def th = _toggle_h()
   def m = _measure_text_ui(_font_body(), txt)
   def rect = _layout_rect(min(_current_body_w(), _text_fit_width(_font_body(), txt) + _gap() + tw), max(float(m.get(1, th)), th))
   def full = _wid(id)
   _register_focusable(full)
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww, hh = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   _begin_interact_widget(full, x, y, ww, hh, true)
   mut out = !!value
   def focused = _kbd_focus_id == full
   def a11y_activate = _accessibility_take(full, "activate")
   if _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false) { out = !out }
   _accessibility_record(full, "switch", txt, out, rect, out)
   if _iw_visible {
      def tx, ty = x + ww - tw, y + max(0.0, (hh - th) * 0.5)
      def clip = _clip_rect_inset(x, y, ww, hh, 1.0)
      def state = _disabled_on > 0 ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered ? "hovered" : "active"))
      def defaults_bg = out ? _accent_soft_u32 : _panel_u32
      def defaults_edge = (_iw_hovered || focused) ? _accent_u32 : _border_u32
      def colors = _button_style(style, state, defaults_bg, defaults_edge, _text_u32)
      def track, edge, label_color = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      _draw_text_ui_clipped(txt,
         x,
         _text_center_y_for(_font_body(), y, hh),
         label_color,
         ww - tw - _gap(),
         clip)
      _gui_rect_fast(tx, ty, tw, th, track)
      _gui_rect_outline_fast(tx, ty, tw, th, edge)
      def knob = th - _sx(4.0)
      def kx = out ? (tx + tw - knob - _sx(2.0)) : (tx + _sx(2.0))
      _gui_rect_fast(kx, ty + _sx(2.0), knob, knob, out ? _accent_u32 : _text_dim_u32)
   }
   out
}

fn radio_button(any id, any label, any selected, any style=nil) bool { _choice_control(id, label, selected, true, style) }

fn tab_strip_slot(f64 x, f64 width, int count, int index) list {
   "Returns the non-overlapping [x,width] hit target for one tab slot."
   if width <= 0.0 || count <= 0 || index < 0 || index >= count { return [float(x), 0.0] }
   def slot_w = width / float(count)
   def slot_x = x + float(index) * slot_w
   [slot_x, (index == count - 1) ? (x + width - slot_x) : slot_w]
}

fn tab_strip(any id, list labels, int selected=0, f64 w=0.0, f64 h=0.0, any style=nil) int {
   "Draws a tab strip and returns the selected tab index. Optional state styles use the button style-map contract."
   ;; `labels` is used as passed. Routing a typed list through an
   ;; `is_list(x) ? x : []` guard reads it back shifted by one element, which
   ;; put the second label in the first slot and blanked the rest.
   def n = labels.len
   if n == 0 { return int(selected) }
   def id_s = to_str(id)
   def rw = _pick_positive(w, _current_body_w())
   def rh = _pick_positive(h, _item_h())
   def rect = _layout_rect(rw, rh)
   _last_item = rect
   def clip = _current_clip()
   mut out = int(selected)
   if !_rect_intersects(rect, clip) { return out }
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww = float(rect.get(2, 0.0))
   def hh = float(rect.get(3, 0.0))
   ;; A tab strip is a row of text buttons over one baseline rule. The active
   ;; tab is bright with an accent underline; nothing else is filled at rest.
   _gui_rect_fast(x, y + hh - 1.0, ww, 1.0, _border_u32)
   mut i = 0
   while i < n {
      def slot = tab_strip_slot(x, ww, n, i)
      def tx, tw = float(slot.get(0, x)), float(slot.get(1, 0.0))
      def full = _widget_id_prefix + id_s + "#tab" + to_str(i)
      _register_focusable(full)
      _begin_interact_widget(full, tx, y, tw, hh, true)
      def hovered = _iw_hovered
      def held = _iw_held
      def focused = _kbd_focus_id == full
      def a11y_activate = _accessibility_take(full, "activate")
      if _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false) { out = i }
      def active = out == i
      _accessibility_record(full, "tab", to_str(labels.get(i, "")), i, [tx, y, tw, hh], false, active, false, false, {"position": i + 1, "set_size": n})
      def state = held ? "pressed" : (hovered && !active ? "hovered" : "active")
      def defaults_bg = held ? _panel_active_u32 : (hovered && !active ? _panel_u32 : 0)
      def colors = _button_style(style, state, defaults_bg, _border_u32, active ? _text_u32 : _text_dim_u32)
      def bg, edge, fg = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      if !is_nil(style) { _gui_rect_fast(tx, y, tw, hh - 1.0, bg) }
      elif held { _gui_rect_fast(tx, y, tw, hh - 1.0, bg) }
      elif hovered && !active { _gui_rect_fast(tx, y, tw, hh - 1.0, bg) }
      def state_style = is_dict(style) ? style.get(state, nil) : nil
      if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(tx, y, tw, hh, edge) }
      if focused { _gui_focus_ring(tx, y, tw, hh) }
      if active { _gui_rect_fast(tx, y + hh - _sx(2.0), tw, _sx(2.0), _accent_u32) }
      _draw_centered_text(to_str(labels.get(i, "")), tx, y, tw, hh, fg)
      i += 1
   }
   out
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
   if first_i < 0 { first_i = 0 }
   if first_i >= values.len { first_i = max(0, values.len - 1) }
   [first_i, min(values.len, first_i + popup_rows + 2)]
}

fn _combo_popup_draw_items(str full, list values, int out, f64 x, f64 popup_y, f64 rwf, f64 popup_h, f64 row_h, int popup_rows, f64 scroll_y, bool draw_visual=true) list {
   mut next_out, next_open = int(out), true
   def span = _combo_popup_item_range(values, row_h, popup_rows, scroll_y)
   mut i = int(span.get(0, 0))
   def last_i = int(span.get(1, i))
   while i < last_i {
      def item_y = popup_y + float(i) * row_h - scroll_y
      if item_y + row_h >= popup_y && item_y <= popup_y + popup_h {
         def item_hover = _current_window_pointer_hit(x + 1.0, item_y, max(1.0, rwf - 2.0), row_h)
         if item_hover {
            _hot_id = full + "#item" + to_str(i)
            _hot_rect = [x + 1.0, item_y, max(1.0, rwf - 2.0), row_h]
         }
         if item_hover && _mouse_pressed0 {
            next_out = i
            next_open = false
            _kbd_focus_id = full
            _focused_window = _current_window_id_cache
         }
         if draw_visual {
            def active = next_out == i
            _gui_rect_fast(x + 1.0, item_y, max(1.0, rwf - 2.0), row_h, active ? _panel_active_u32 : (item_hover ? _panel_hover_u32 : _popup_u32))
            if active { _gui_rect_fast(x + 1.0, item_y, _sx(4.0), row_h, _accent_u32) }
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
   if max_scroll <= 1.0 { return next_scroll }
   def track_w, track_x = _sx(6.0), x + rwf - _sx(6.0) - _sx(2.0)
   def track_y, track_h = popup_y + 1.0, popup_h - 2.0
   def thumb_h = max(_sx(18.0), popup_h * (popup_h / (popup_h + max_scroll)))
   def usable = max(1.0, track_h - thumb_h)
   def scroll_id = popup_key + "#thumb"
   mut thumb_y = track_y + usable * ((max_scroll > 0.0) ? (next_scroll / max_scroll) : 0.0)
   def track_hover = _current_window_pointer_hit(track_x, track_y, track_w, track_h)
   def thumb_hover = _current_window_pointer_hit(track_x, thumb_y, track_w, thumb_h)
   if thumb_hover {
      _hot_id = scroll_id
      _hot_rect = [track_x, thumb_y, track_w, thumb_h]
      _ensure_cursors()
      _want_cursor(_cursor_hand)
   }
   if thumb_hover && _mouse_pressed0 {
      _active_widget_id = scroll_id
      _kbd_focus_id = full
      _focused_window = _current_window_id_cache
      _state_set_num(scroll_id, "drag_off_y", _mouse_y - thumb_y)
   } elif track_hover && _mouse_pressed0 {
      def want_thumb_y = clamp(_mouse_y - thumb_h * 0.5, track_y, track_y + usable)
      next_scroll = (max_scroll > 0.0) ? (max_scroll * ((want_thumb_y - track_y) / usable)) : 0.0
      thumb_y = track_y + usable * ((max_scroll > 0.0) ? (next_scroll / max_scroll) : 0.0)
   }
   if _active_widget_id == scroll_id {
      if _mouse_down0 {
         def want_thumb_y = clamp(_mouse_y - _state_get_num(scroll_id, "drag_off_y", 0.0), track_y, track_y + usable)
         next_scroll = (max_scroll > 0.0) ? (max_scroll * ((want_thumb_y - track_y) / usable)) : 0.0
         thumb_y = want_thumb_y
      } else {
         _active_widget_id = ""
      }
   }
   if draw_visual {
      _gui_rect_fast(track_x, track_y, track_w, track_h, _scroll_track_u32)
      _gui_rect_fast(track_x, thumb_y, track_w, thumb_h, (_active_widget_id == scroll_id || thumb_hover) ? _scroll_thumb_hot_u32 : _scroll_thumb_u32)
   }
   next_scroll
}

fn _combo_popup_reserve_layout(bool forced_open, f64 field_y, f64 x, f64 popup_y, f64 rwf, f64 popup_h) any {
   if !forced_open || popup_y < field_y { return 0 }
   def reserve_y = popup_y + popup_h + _gap()
   def cur_y = _current_cursor_y_cache
   if reserve_y > cur_y {
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
   if !next_open { return [next_out, next_open, next_scroll] }
   def item_state = _combo_popup_draw_items(full, values, next_out, x, popup_y, rwf, popup_h, row_h, popup_rows, next_scroll, false)
   next_out, next_open = int(item_state.get(0, next_out)), !!item_state.get(1, next_open)
   next_scroll = _combo_popup_scrollbar(popup_key, full, x, rwf, popup_y, popup_h, max_scroll, next_scroll, false)
   if next_open {
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
   while i < last_i {
      labels = labels.append(_ui_text(values.get(i, "")))
      i += 1
   }
   _popup_combo_labels = labels
}

fn _combo_popup_scrollbar_visual(str popup_key, f64 x, f64 rwf, f64 popup_y, f64 popup_h, f64 max_scroll, f64 scroll_y) any {
   if max_scroll <= 1.0 { return 0 }
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

fn _combo_popup_draw_labels(list labels, int first_i, int out, f64 x, f64 popup_y, f64 rwf, f64 popup_h, f64 row_h, f64 scroll_y, any style=nil) any {
   mut li = 0
   def n = labels.len
   while li < n {
      def item_i = first_i + li
      def item_y = popup_y + float(item_i) * row_h - scroll_y
      if item_y + row_h >= popup_y && item_y <= popup_y + popup_h {
         def item_hover = _rect_hit(_mouse_x, _mouse_y, x + 1.0, item_y, max(1.0, rwf - 2.0), row_h)
         def active = int(out) == item_i
         def state = item_hover ? "hovered" : "active"
         def default_bg = active ? _panel_active_u32 : (item_hover ? _panel_hover_u32 : _popup_u32)
         def colors = _button_style(style, state, default_bg, _border_u32, active ? _text_u32 : _text_dim_u32)
         def bg, edge, fg = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
         _gui_rect_fast(x + 1.0, item_y, max(1.0, rwf - 2.0), row_h, bg)
         def state_style = is_dict(style) ? style.get(state, nil) : nil
         if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(x + 1.0, item_y, max(1.0, rwf - 2.0), row_h, edge) }
         if active { _gui_rect_fast(x + 1.0, item_y, _sx(4.0), row_h, _accent_u32) }
         _draw_text_ui_clipped(_ui_text(labels.get(li, "")),
            x + _item_pad_x() + _sx(4.0),
            _text_center_y_for(_font_body(), item_y, row_h),
            fg,
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
   list labels,
   any style=nil) any {
   _draw_combo_popup_frame(x, popup_y, rwf, popup_h)
   push_clip_rect(x + 1.0, popup_y + 1.0, max(1.0, rwf - 2.0), max(1.0, popup_h - 2.0))
   _combo_popup_draw_labels(labels, first_i, out, x, popup_y, rwf, popup_h, row_h, scroll_y, style)
   pop_clip_rect()
   _combo_popup_scrollbar_visual(popup_key, x, rwf, popup_y, popup_h, max_scroll, scroll_y)
}

fn _combo_selected_index(list values, int selected) int {
   mut out = int(selected)
   if values.len <= 0 { return -1 }
   if out < 0 { return 0 }
   if out >= values.len { return values.len - 1 }
   out
}

fn _combo_apply_trigger(str full, bool open, bool forced_open, bool nav_focus) bool {
   mut next_open = forced_open ? true : !!open
   if nav_focus { _kbd_focus_id = full }
   def a11y_activate = _accessibility_take(full, "activate")
   if _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false) {
      next_open = !next_open
      _focused_window = _current_window_id_cache
   }
   next_open
}

fn _combo_apply_keyboard(str full, list values, int out, int max_visible) int {
   mut next_out = int(out)
   if _kbd_focus_id != full || values.len <= 0 { return next_out }
   if _key_event_present(KEY_UP) { next_out = (next_out <= 0) ? 0 : (next_out - 1) }
   if _key_event_present(KEY_DOWN) { next_out = (next_out >= values.len - 1) ? (values.len - 1) : (next_out + 1) }
   if _key_event_present(KEY_PAGE_UP) { next_out = max(0, next_out - max(1, int(max_visible))) }
   if _key_event_present(KEY_PAGE_DOWN) { next_out = min(values.len - 1, next_out + max(1, int(max_visible))) }
   if _key_event_present(KEY_HOME) { next_out = 0 }
   if _key_event_present(KEY_END) { next_out = values.len - 1 }
   next_out
}

fn _combo_popup_metrics(list values, f64 field_y, f64 field_h, int max_visible) list {
   def row_h = field_h
   def popup_rows = min(values.len, max(1, int(max_visible)))
   def popup_h = max(1.0, float(popup_rows) * row_h + 2.0)
   def clip_y, clip_h = 0.0, _fb_h
   mut popup_y = field_y + field_h + _tiny_gap()
   if popup_y + popup_h > clip_y + clip_h {
      def above_y = field_y - popup_h - _tiny_gap()
      if above_y >= clip_y { popup_y = above_y } else { popup_y = max(clip_y, clip_y + clip_h - popup_h) }
   }
   [row_h, popup_rows, popup_h, popup_y, max(0.0, float(values.len) * row_h - popup_h)]
}

fn _combo_scroll_state(bool open, list values, int out, str popup_key, bool popup_hover, f64 row_h, f64 popup_h, f64 max_scroll) f64 {
   mut scroll_y = clamp(_state_get_num(popup_key, "scroll_y", 0.0), 0.0, max_scroll)
   if open && (_iw_hovered|| popup_hover) && abs(_mouse_scroll_dy) > 0.000001 { scroll_y = clamp(scroll_y - _mouse_scroll_dy * _scroll_step(), 0.0, max_scroll) }
   if open && values.len > 0 && out >= 0 {
      def sel_top = float(out) * row_h
      def sel_bot = sel_top + row_h
      if sel_top < scroll_y { scroll_y = sel_top } elif sel_bot > scroll_y + popup_h { scroll_y = sel_bot - popup_h }
      scroll_y = clamp(scroll_y, 0.0, max_scroll)
   }
   scroll_y
}

fn _draw_combo_field(str full, str lbl, list values, int out, bool open, f64 x, f64 y, f64 rwf, f64 field_y, f64 field_h, any style=nil) any {
   if !_iw_visible { return 0 }
   def field_clip = _clip_rect_inset(x, field_y, rwf, field_h, 1.0)
   def focused = _kbd_focus_id == full
   def state = open ? "pressed" : (_iw_hovered ? "hovered" : "active")
   def default_bg = open ? _panel_active_u32 : (_iw_hovered ? _panel_hover_u32 : _panel_u32)
   def default_edge = open ? _accent_u32 : (_iw_hovered ? _text_faint_u32 : _border_u32)
   def colors = _button_style(style, state, default_bg, default_edge, _text_u32)
   def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
   _gui_rect_fast(x, field_y, rwf, field_h, bg)
   _gui_rect_outline_fast(x, field_y, rwf, field_h, edge)
   if focused && !open { _gui_focus_ring(x, field_y, rwf, field_h) }
   def show_txt = ((values.len > 0) && out >= 0 && out < values.len) ? _ui_text(values.get(out, "")) : "<empty>"
   def text_y = _text_center_y_for(_font_body(), field_y, field_h)
   def show_col = (values.len > 0) ? (is_nil(style) ? _text_u32 : text_col) : _text_faint_u32
   def label_col = is_nil(style) ? _text_dim_u32 : text_col
   if str.len(lbl) > 0 { _draw_text_ui_small_clipped(lbl, x, y, label_col, rwf, _clip_rect_inset(x, y, rwf, _label_h(), 0.0)) }
   _draw_text_ui_clipped(show_txt,
      x + _item_pad_x(),
      text_y,
      show_col,
      rwf - _item_pad_x() * 2.0 - _sx(16.0),
      field_clip)
   _gui_chevron(x + rwf - _item_pad_x() - _sx(7.0), field_y + field_h * 0.5, _sx(9.0), true, is_nil(style) ? ((open || _iw_hovered) ? _text_u32 : _text_dim_u32) : text_col)
}

fn combo_box(any id, any label, list items, int selected=0, f64 w=0.0, int max_visible=6, any style=nil) int {
   "Draws a combo box and returns the selected item index. Optional state styles apply to its field."
   def lbl = _ui_text(label)
   def rect = _layout_rect(_pick_positive(w, _current_body_w()), _labelled_h(lbl))
   _combo_box_at(id, lbl, items, selected, rect, max_visible, style)
}

fn _combo_box_at(any id, str lbl, list items, int selected, list rect, int max_visible, any style) int {
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def rwf = float(rect.get(2, 0.0))
   def cap_h = lbl.len > 0 ? _label_h() : 0.0
   def field_y, field_h = y + cap_h, float(rect.get(3, 0.0)) - cap_h
   mut out = _combo_selected_index(items, selected)
   if field_h <= 0.0 { return out }
   _popup_combo_style = style
   ;; See tab_strip: a typed list must not pass through an is_list guard.
   def values = items
   def full = _wid(id)
   def open_key = full + "#open"
   def popup_key = full + "#popup"
   def nav_focus = _register_focusable(full)
   _begin_interact_widget(full, x, field_y, rwf, field_h, true)
   def forced_open = _debug_combo_forced(full, id)
   mut open = _combo_apply_trigger(full, _state_get_bool(open_key, false), forced_open, nav_focus)
   out = _combo_apply_keyboard(full, values, out, max_visible)
   def metrics = _combo_popup_metrics(values, field_y, field_h, max_visible)
   def row_h, popup_h = float(metrics.get(0, field_h)), float(metrics.get(2, field_h))
   def popup_rows, popup_y = int(metrics.get(1, 1)), float(metrics.get(3, field_y + field_h))
   def max_scroll = float(metrics.get(4, 0.0))
   def popup_hover = open && _current_window_pointer_hit(x, popup_y, rwf, popup_h)
   if open && !forced_open && _mouse_pressed0 && !_iw_hovered && !popup_hover { open = false }
   mut scroll_y = _combo_scroll_state(open, values, out, popup_key, popup_hover, row_h, popup_h, max_scroll)
   _draw_combo_field(full, lbl, values, out, open, x, y, rwf, field_y, field_h, style)
   def popup_state = _combo_popup(full, popup_key, values, out, open, forced_open, x, field_y, rwf, popup_y, popup_h, row_h, popup_rows, max_scroll, scroll_y)
   out, open, scroll_y = int(popup_state.get(0, out)), !!popup_state.get(1, open), float(popup_state.get(2, scroll_y))
   _state_set_bool(open_key, open)
   _state_set_num(popup_key, "scroll_y", scroll_y)
   if !(forced_open && open) { _last_item = [x, field_y, rwf, field_h] }
   _accessibility_record(full, "combobox", lbl, values.get(out, ""), [x, field_y, rwf, field_h], false, false, open, false, {"selected_index": out, "options": values})
   out
}

fn slider_float(any id, any label, f64 value, f64 min_v, f64 max_v, f64 w=0.0, any style=nil) f64 {
   "Draws a floating-point slider and returns the adjusted value. Optional state styles support background, fill, knob, border and text RGBA colors."
   def rect = _layout_rect(_pick_positive(w, _current_body_w()), _label_h() + _sx(18.0))
   _slider_float_at(id, label, value, min_v, max_v, rect, style)
}

fn _slider_float_at(any id, any label, f64 value, f64 min_v, f64 max_v, list rect, any style) f64 {
   mut lo = float(min_v)
   mut hi = float(max_v)
   if hi < lo {
      def t = lo
      lo = hi
      hi = t
   }
   mut out = clamp(float(value), lo, hi)
   def f64 height = float(rect.get(3, 0.0))
   def f64 band_h = height - _label_h()
   if band_h <= 0.0 { return out }
   _last_item = rect
   def full = _wid(id)
   _register_focusable(full)
   _begin_interact_widget(full,
      float(rect.get(0,
            0.0)),
      float(rect.get(1,
            0.0)),
      float(rect.get(2,
            0.0)),
      float(rect.get(3,
            0.0)),
      true)
   def set_action = _accessibility_take(full, "set_value")
   if set_action.get("found", false) { out = clamp(float(set_action.get("value", out)), lo, hi) }
   def step = max((hi - lo) / 100.0, 0.0001)
   if _accessibility_take(full, "increment").get("found", false) { out = clamp(out + step, lo, hi) }
   if _accessibility_take(full, "decrement").get("found", false) { out = clamp(out - step, lo, hi) }
   if _kbd_focus_id == full {
      if _key_event_present(KEY_LEFT) || _key_event_present(KEY_DOWN) { out = clamp(out - step, lo, hi) }
      if _key_event_present(KEY_RIGHT) || _key_event_present(KEY_UP) { out = clamp(out + step, lo, hi) }
   }
   if _iw_held || (_iw_hovered && _mouse_pressed0) {
      def track_x = float(rect.get(0, 0.0)) + _item_pad_x()
      def track_w = max(_sx(24.0), float(rect.get(2, 0.0)) - _item_pad_x() * 2.0)
      def t = clamp((_mouse_x - track_x) / track_w, 0.0, 1.0)
      out = lo + (hi - lo) * t
   }
   if _iw_visible {
      def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      def rw = float(rect.get(2, 0.0))
      def tw = max(_sx(24.0), rw - _item_pad_x() * 2.0)
      def rx = x + _item_pad_x()
      ;; Caption and value share the first line; the track is centred in the
      ;; band below it with the knob straddling the track.
      def band_y = y + _label_h()
      def track_h = min(band_h, _sx(4.0))
      def ry = _snap(band_y + (band_h - track_h) * 0.5)
      def norm = (hi > lo) ? clamp((out - lo) / (hi - lo), 0.0, 1.0) : 0.0
      def clip = _clip_rect_inset(x, y, rw, float(rect.get(3, 0.0)), 0.0)
      def val_s = f"{out:.3f}"
      def val_w = float(_measure_text_ui(_font_small_id(), val_s).get(0, 0.0))
      def hot = _iw_hovered || _iw_held || _kbd_focus_id == full
      def state = _disabled_on > 0 ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered ? "hovered" : "active"))
      def colors = _button_style(style, state, _panel_active_u32, _window_bg_u32, _text_u32)
      def text_col, edge = int(colors.get(2, _text_u32)), int(colors.get(1, _window_bg_u32))
      def track, fill, knob = int(colors.get(0, _panel_active_u32)), int(colors.get(3, _accent_u32)), int(colors.get(4, hot ? _text_u32 : _text_dim_u32))
      _draw_text_ui_small_clipped(_ui_text(label), x, y, is_nil(style) ? _text_dim_u32 : text_col, max(1.0, rw - val_w - _sx(12.0)), clip)
      _draw_text_ui_small_clipped(val_s, x + rw - val_w - _item_pad_x(), y, text_col, val_w + _item_pad_x(), clip)
      _gui_rect_fast(rx, ry, tw, track_h, track)
      _gui_rect_fast(rx, ry, max(1.0, tw * norm), track_h, fill)
      def knob_w, knob_h = _sx(8.0), min(band_h, _sx(14.0))
      def knob_x = _snap(rx + tw * norm - knob_w * 0.5)
      def knob_y = _snap(ry + track_h * 0.5 - knob_h * 0.5)
      _gui_rect_fast(knob_x, knob_y, knob_w, knob_h, knob)
      _gui_rect_outline_fast(knob_x, knob_y, knob_w, knob_h, edge)
      if _kbd_focus_id == full { _gui_focus_ring(knob_x - _sx(2.0), knob_y - _sx(2.0), knob_w + _sx(4.0), knob_h + _sx(4.0)) }
   }
   _accessibility_record(full, "slider", _ui_text(label), out, rect, false, false, false, false, {"min": lo, "max": hi, "orientation": "horizontal"})
   out
}

fn slider_int(any id, any label, any value, any min_v, any max_v, any w=0.0, any style=nil) int {
   "Draws an integer slider and returns the rounded adjusted value. Optional state styles use slider_float's color contract."
   def fv = slider_float(id, label, float(value), float(min_v), float(max_v), w, style)
   int(fv + 0.5)
}

fn progress_bar(any id, any value, any label="", any w=0.0, any style=nil) any {
   "Draws a bounded progress bar with its caption and percentage on the line above; optional styles control its track, fill, border and text."
   def full_w = _pick_positive(w, _current_body_w())
   def rect = _layout_rect(full_w, _label_h() + _sx(10.0))
   _last_item = rect
   def p = clamp(float(value), 0.0, 1.0)
   def visible = _rect_intersects(rect, _current_clip())
   _iw_visible, _iw_hovered, _iw_held = visible, false, false
   _accessibility_record(_wid(id), "progressbar", _ui_text(label), p, rect, false, false, false, _disabled_on > 0, {"min": 0.0, "max": 1.0, "orientation": "horizontal"})
   if visible {
      def label_s = _ui_text(label)
      def x = float(rect.get(0, 0.0))
      def y = float(rect.get(1, 0.0))
      def rw = float(rect.get(2, 0.0))
      def clip = _clip_rect_inset(x, y, rw, float(rect.get(3, 0.0)), 0.0)
      def pct = f"{p * 100.0:.0f}%"
      def pct_w = float(_measure_text_ui(_font_small_id(), pct).get(0, 0.0))
      def state = _disabled_on > 0 ? "disabled" : "active"
      def colors = _button_style(style, state, _panel_active_u32, _border_u32, _text_u32)
      def track, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      def state_style = is_dict(style) ? style.get(state, nil) : nil
      def default_fill = (p >= 1.0) ? _ok_u32 : _accent_u32
      def styled_fill = is_dict(state_style) && is_list(state_style.get("fill", nil)) ? int(colors.get(3, default_fill)) : default_fill
      _draw_text_ui_small_clipped((label_s.len > 0) ? label_s : to_str(id), x, y, is_nil(style) ? _text_dim_u32 : text_col, max(1.0, rw - pct_w - _sx(8.0)), clip)
      _draw_text_ui_small_clipped(pct, x + rw - pct_w, y, text_col, pct_w, clip)
      def by, bh = y + _label_h() + _sx(2.0), _sx(4.0)
      _gui_rect_fast(x, by, rw, bh, track)
      if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(x, by, rw, bh, edge) }
      if p > 0.0 { _gui_rect_fast(x, by, max(_sx(2.0), rw * p), bh, styled_fill) }
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
   if !_rect_intersects(rect, _current_clip()) { return rect }
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww = float(rect.get(2, 0.0))
   def hh = float(rect.get(3, 0.0))
   def clip = _clip_rect_inset(x, y, ww, hh, 1.0)
   def accent_col = is_list(tone) ? tone : _accent_rgba
   _gui_rect_fast(x + _sx(2.0), y + _sx(3.0), ww, hh, color_pack(0.0, 0.0, 0.0, 0.14))
   _gui_rect_fast(x, y, ww, hh, _window_bg_u32)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), max(1.0, hh - 2.0), _window_body_u32)
   _gui_rect_fast(x + 1.0, y + _sx(18.0), max(1.0, ww - 2.0), max(1.0, hh - _sx(19.0)), _panel_u32)
   def hdr_h = _sx(24.0)
   def bar_h = _sx(3.0)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), hdr_h, _window_hdr_u32)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), bar_h, color_pack(float(accent_col.get(0,
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
   def label_y = y + 1.0 + bar_h + max(_sx(2.0), (hdr_h - bar_h - detail_h) * 0.5)
   mut value_y = y + hdr_h + _sx(7.0)
   mut detail_y = y + hh - detail_h - _sx(11.0)
   if detail_s.len > 0 && detail_y < value_y + value_h + _sx(5.0) {
      value_font = _font_body()
      value_h = _font_h_for(value_font)
      value_y = y + hdr_h + _sx(6.0)
      detail_y = y + hh - detail_h - _sx(10.0)
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
   if detail_s.len > 0 && detail_y >= value_y + value_h + _sx(4.0) {
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
   if !_rect_intersects(rect, _current_clip()) { return [rect, false, 0.0, 0.0, 0.0, 0.0] }
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def ww = float(rect.get(2, 0.0))
   def hh = float(rect.get(3, 0.0))
   if ww < 4.0 || hh < 4.0 { return [rect, false, x, y, ww, hh] }
   push_clip_rect(rect)
   [rect, true, x, y, ww, hh]
}

fn plot_lines(any id, any values, any label="", any w=0.0, any h=120.0, any color=0) list {
   "Draws a plot panel placeholder and returns its rectangle."
   def c = _begin_canvas_rect(w, h, 84.0)
   def rect = c.get(0)
   if !c.get(1, false) { return rect }
   def x, y, ww, hh = c.get(2), c.get(3), c.get(4), c.get(5)
   _gui_rect_fast(x, y, ww, hh, _window_bg_u32)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), max(1.0, hh - 2.0), _window_body_u32)
   _gui_rect_fast(x + 1.0, y + 1.0, max(1.0, ww - 2.0), _sx(18.0), _window_hdr_u32)
   _gui_rect_outline_fast(x, y, ww, hh, _border_u32)
   def text_clip = _clip_rect_inset(x, y, ww, hh, 1.0)
   def label_txt = _ui_text(label)
   if str.len(label_txt) > 0 {
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
   if !c.get(1, false) { return rect }
   def x, y, ww, hh = c.get(2), c.get(3), c.get(4), c.get(5)
   _gui_rect_fast(x, y, ww, hh, _window_bg_u32)
   def step = max(_sx(8.0), float(cell))
   def major_n = max(int(major_every), 1)
   mut gx = x
   mut idx = 0
   while gx <= x + ww {
      def major_col = ((idx % major_n) == 0) ? _grid_major_u32 : _grid_minor_u32
      _gui_rect_fast(gx, y, 1.0, hh, major_col)
      gx += step
      idx += 1
   }
   mut gy = y
   idx = 0
   while gy <= y + hh {
      def major_col = ((idx % major_n) == 0) ? _grid_major_u32 : _grid_minor_u32
      _gui_rect_fast(x, gy, ww, 1.0, major_col)
      gy += step
      idx += 1
   }
   _gui_rect_outline_fast(x, y, ww, hh, _border_u32)
   def label_txt = _ui_text(label)
   if str.len(label_txt) > 0 {
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
   while sj < out_n {
      mut other = out.get(sj, 0)
      if is_dict(other) {
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
   if header_hit && _mouse_pressed0 {
      _active_widget_id = header_key
      _state_set_num(header_key, "drag_off_x", _mouse_x - nx)
      _state_set_num(header_key, "drag_off_y", _mouse_y - ny)
   }
   if _active_widget_id == header_key && _mouse_down0 && _mouse_drag_ready() {
      next_x = clamp(_mouse_x - _state_get_num(header_key, "drag_off_x", 0.0), 0.0, max(0.0, ww - nw))
      next_y = clamp(_mouse_y - _state_get_num(header_key, "drag_off_y", 0.0), 0.0, max(0.0, hh - nh))
      next_node["x"] = next_x
      next_node["y"] = next_y
   } elif _active_widget_id == header_key && !_mouse_down0 {
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
   while ii < ins_n {
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
   while oi < outs_n {
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
   while ni < out_n {
      mut node = out.get(ni, 0)
      if !is_dict(node) { ni += 1 continue }
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
      if header_hit || (_active_widget_id == header_key && _mouse_down0) {
         _ensure_cursors()
         _want_cursor(_cursor_move)
      }
      if node_hit && _mouse_pressed0 {
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
   while li < links_n {
      def lk = links.get(li, 0)
      mut from_node, from_slot, to_node, to_slot = -1, -1, -1, -1
      if is_list(lk) {
         from_node = int(lk.get(0, -1))
         from_slot = int(lk.get(1, -1))
         to_node = int(lk.get(2, -1))
         to_slot = int(lk.get(3, -1))
      } elif is_dict(lk) {
         from_node = int(lk.get("from_node", -1))
         from_slot = int(lk.get("from_slot", -1))
         to_node = int(lk.get("to_node", -1))
         to_slot = int(lk.get("to_slot", -1))
      }
      def p0 = slot_pos.get("out:" + to_str(from_node) + ":" + to_str(from_slot), 0)
      def p1 = slot_pos.get("in:" + to_str(to_node) + ":" + to_str(to_slot), 0)
      if is_list(p0) && is_list(p1) {
         def x1, y1 = float(p0.get(0, 0.0)), float(p0.get(1, 0.0))
         def x2, y2 = float(p1.get(0, 0.0)), float(p1.get(1, 0.0))
         def mx = x1 + (x2 - x1) * 0.5
         _gui_line(x1, y1, mx, y1, _accent_rgba, 2.0)
         _gui_line(mx, y1, mx, y2, _accent_rgba, 2.0)
         _gui_line(mx, y2, x2, y2, _accent_rgba, 2.0)
      }
      li += 1
   }
}

fn node_canvas(any id, list nodes, list links, f64 w=0.0, f64 h=280.0, f64 cell=32.0) list {
   "Draws an editable node graph and returns the updated node list."
   def full = _wid(id)
   def rect = grid_canvas(full + ".grid", "Node Graph", w, h, cell, 4)
   if !_rect_intersects(rect, _current_clip()) { return nodes }
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

fn collapsing_header(any id, any label, bool default_open=true, any style=nil) bool {
   "Draws a collapsing header and returns its open state. Optional state styles use the button style-map contract."
   def full = _wid(id) + "#header"
   _register_focusable(full)
   mut open = _state_get_bool(full, default_open)
   def rect = _layout_rect(_current_body_w(), _item_h())
   _last_item = rect
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def w, h = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   _begin_interact_widget(full, x, y, w, h, true)
   def focused = _kbd_focus_id == full
   def a11y_activate = _accessibility_take(full, "activate")
   if _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false) {
      open = !open
      _state_set_bool(full, open)
   }
   _accessibility_record(full, "button", _ui_text(label), "", rect, false, false, open, _disabled_on > 0, {"disclosure": true})
   if _iw_visible {
      def state = _disabled_on > 0 ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered ? "hovered" : "active"))
      def default_bg = _iw_held ? _panel_active_u32 : (_iw_hovered ? _panel_hover_u32 : _window_hdr_u32)
      def colors = _button_style(style, state, default_bg, _border_u32, _text_u32)
      def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      _gui_rect_fast(x, y, w, h, bg)
      def state_style = is_dict(style) ? style.get(state, nil) : nil
      if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(x, y, w, h, edge) }
      if focused { _gui_focus_ring(x, y, w, h) }
      _gui_chevron(x + _item_pad_x() + _sx(6.0), y + h * 0.5, _sx(9.0), open, text_col)
      _draw_text_ui_clipped(_ui_text(label),
         x + _item_pad_x() + _sx(20.0),
         _text_center_y_for(_font_body(), y, h),
         text_col,
         w - _item_pad_x() * 2.0 - _sx(20.0),
         _clip_rect_inset(x, y, w, h, 0.0))
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
   if d > 0.000001 {
      if hi == r {
         h = (g - b) / d
         if h < 0.0 { h += 6.0 }
      } elif hi == g {
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
   if sector >= 6 { sector = 5 }
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

fn _color_preview_row(str full, any label, list rgba, any style=nil) any {
   def rect = _layout_rect(_current_body_w(), _sx(44.0))
   if !_rect_intersects(rect, _current_clip()) { return 0 }
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def h = float(rect.get(3, 0.0))
   def sw = min(_sx(64.0), max(_sx(42.0), w * 0.22))
   def clip = _clip_rect_inset(x, y, w, h, 1.0)
   def colors = _button_style(style, "active", _panel_u32, _border_u32, _text_u32)
   def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
   _draw_text_ui_clipped(_ui_text(label), x, y, text_col, max(_sx(40.0), w - sw - _gap()), clip)
   _draw_text_ui_small_clipped(_color_hex_rgb(rgba) + "  A " + f"{float(rgba.get(3, 1.0)):.2f}",
      x, y + _text_h() + _sx(3.0), is_nil(style) ? _text_dim_u32 : text_col, max(_sx(40.0), w - sw - _gap()), clip)
   def px = x + w - sw
   def py = y
   _gui_rect_fast(px, py, sw, h, bg)
   _gui_rect_fast(px + 1.0, py + 1.0, max(1.0, sw - 2.0), max(1.0, h - 2.0),
      color_pack(float(rgba.get(0, 1.0)), float(rgba.get(1, 1.0)), float(rgba.get(2, 1.0)), float(rgba.get(3, 1.0))))
   _gui_rect_outline_fast(px, py, sw, h, edge)
}

fn _color_sv_plane(str full, list rgba, f64 hue, f64 x, f64 y, f64 w, f64 h, any style=nil) list {
   mut out = rgba
   def hsv = _color_rgb_to_hsv(out)
   mut sat = float(hsv.get(1, 0.0))
   mut val = float(hsv.get(2, 1.0))
   _begin_interact_widget(full + ".sv", x, y, w, h, true)
   if _iw_held || (_iw_hovered && _mouse_pressed0) {
      sat = clamp((_mouse_x - x) / max(1.0, w), 0.0, 1.0)
      val = clamp(1.0 - ((_mouse_y - y) / max(1.0, h)), 0.0, 1.0)
      out = _color_set_rgb(out, _color_hsv_to_rgb(hue, sat, val))
   }
   if _iw_visible {
      def cols = 14
      def rows = 7
      def cw = max(1.0, w / float(cols))
      def ch = max(1.0, h / float(rows))
      mut row = 0
      while row < rows {
         mut col = 0
         while col < cols {
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
      def state = _disabled_on > 0 ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered ? "hovered" : "active"))
      def colors = _button_style(style, state, 0, _iw_hovered ? _accent_u32 : _border_u32, _text_u32)
      _gui_rect_outline_fast(cx - _sx(5.0), cy - _sx(5.0), _sx(10.0), _sx(10.0), _window_bg_u32)
      _gui_rect_outline_fast(cx - _sx(4.0), cy - _sx(4.0), _sx(8.0), _sx(8.0), int(colors.get(2, _text_u32)))
      _gui_rect_outline_fast(x, y, w, h, int(colors.get(1, _iw_hovered ? _accent_u32 : _border_u32)))
   }
   out
}

fn _color_hue_strip(str full, list rgba, f64 hue, f64 x, f64 y, f64 w, f64 h, any style=nil) list {
   mut out = rgba
   mut hval = clamp(hue, 0.0, 1.0)
   def hsv = _color_rgb_to_hsv(out)
   def sat = float(hsv.get(1, 0.0))
   def val = float(hsv.get(2, 1.0))
   _begin_interact_widget(full + ".hue", x, y, w, h, true)
   if _iw_held || (_iw_hovered && _mouse_pressed0) {
      hval = clamp((_mouse_x - x) / max(1.0, w), 0.0, 1.0)
      out = _color_set_rgb(out, _color_hsv_to_rgb(hval, sat, val))
      _state_set_num(full, "hue", hval)
   }
   if _iw_visible {
      def segs = 24
      def sw = max(1.0, w / float(segs))
      mut i = 0
      while i < segs {
         def rgb = _color_hsv_to_rgb(float(i) / float(max(1, segs - 1)), 1.0, 1.0)
         _gui_rect_fast(x + float(i) * sw, y, sw + 1.0, h,
            color_pack(float(rgb.get(0, 1.0)), float(rgb.get(1, 1.0)), float(rgb.get(2, 1.0)), 1.0))
         i += 1
      }
      def mx = x + hval * w
      def state = _disabled_on > 0 ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered ? "hovered" : "active"))
      def colors = _button_style(style, state, 0, _iw_hovered ? _accent_u32 : _border_u32, _text_u32)
      _gui_rect_fast(mx - _sx(2.0), y - _sx(2.0), _sx(4.0), h + _sx(4.0), _window_bg_u32)
      _gui_rect_fast(mx - _sx(1.0), y - _sx(1.0), _sx(2.0), h + _sx(2.0), int(colors.get(2, _text_u32)))
      _gui_rect_outline_fast(x, y, w, h, int(colors.get(1, _iw_hovered ? _accent_u32 : _border_u32)))
   }
   out
}

fn _color_swatch(str full, int idx, list color, list rgba, f64 x, f64 y, f64 size, any style=nil) list {
   mut out = rgba
   _begin_interact_widget(full + ".swatch" + to_str(idx), x, y, size, size, true)
   if _iw_hovered && _mouse_pressed0 {
      out = [
         clamp(float(color.get(0, 1.0)), 0.0, 1.0),
         clamp(float(color.get(1, 1.0)), 0.0, 1.0),
         clamp(float(color.get(2, 1.0)), 0.0, 1.0),
         clamp(float(rgba.get(3, 1.0)), 0.0, 1.0)
      ]
      def hsv = _color_rgb_to_hsv(out)
      _state_set_num(full, "hue", float(hsv.get(0, 0.0)))
   }
   if _iw_visible {
      def state = _disabled_on > 0 ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered ? "hovered" : "active"))
      def colors = _button_style(style, state, _theme_pack(color), _iw_hovered ? _accent_u32 : _border_u32, _text_u32)
      _gui_rect_fast(x, y, size, size,
         color_pack(float(color.get(0, 1.0)), float(color.get(1, 1.0)), float(color.get(2, 1.0)), 1.0))
      _gui_rect_outline_fast(x, y, size, size, int(colors.get(1, _iw_hovered ? _accent_u32 : _border_u32)))
   }
   out
}

fn _color_swatch_row(str full, list rgba, any style=nil) list {
   mut out = rgba
   def rect = _layout_rect(_current_body_w(), _sx(28.0))
   def presets = [
      [1.0, 1.0, 1.0], [0.0, 0.0, 0.0], [0.90, 0.10, 0.08], [1.0, 0.52, 0.12],
      [0.95, 0.86, 0.20], [0.18, 0.72, 0.28], [0.08, 0.54, 0.88], [0.42, 0.28, 0.86],
      [0.95, 0.40, 0.66], [0.50, 0.50, 0.50]
   ]
   if !_rect_intersects(rect, _current_clip()) { return out }
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def size = min(_sx(24.0), max(_sx(14.0), (w - _gap() * float(max(0, presets.len - 1))) / float(max(1, presets.len))))
   mut i = 0
   while i < presets.len {
      out = _color_swatch(full, i, presets.get(i, [1.0, 1.0, 1.0]), out, x + float(i) * (size + _gap()), y, size, style)
      i += 1
   }
   out
}

fn color_edit4(any id, any label, any rgba, any style=nil) list {
   "Draws RGBA sliders and returns the edited color. Optional styles are forwarded to the component sliders."
   mut out = _color_rgba4(rgba)
   def full = _wid(id) + "#color"
   def start_y = _current_cursor_y_cache
   _color_preview_row(full, label, out, style)
   out[0] = slider_float(to_str(id) + ".r", "R", out.get(0, 1.0), 0.0, 1.0, 0.0, style)
   out[1] = slider_float(to_str(id) + ".g", "G", out.get(1, 1.0), 0.0, 1.0, 0.0, style)
   out[2] = slider_float(to_str(id) + ".b", "B", out.get(2, 1.0), 0.0, 1.0, 0.0, style)
   out[3] = slider_float(to_str(id) + ".a", "A", out.get(3, 1.0), 0.0, 1.0, 0.0, style)
   def group_bounds = [_current_body_x_cache, start_y, _current_body_w_cache, max(0.0, _current_cursor_y_cache - start_y)]
   _iw_visible = _rect_intersects(group_bounds, _current_clip())
   _accessibility_record(full, "group", _ui_text(label), out, group_bounds, false, false, false, _disabled_on > 0, {"format": "RGBA", "components": ["R", "G", "B", "A"]})
   out
}

fn color_picker4(any id, any label, any rgba, any style=nil) list {
   "Draws an inline SV/hue color picker with swatches and RGBA sliders. Optional styles are forwarded to the component sliders."
   mut out = _color_rgba4(rgba)
   def full = _wid(id) + "#picker"
   def start_y = _current_cursor_y_cache
   def hsv = _color_rgb_to_hsv(out)
   mut hue = _state_get_num(full, "hue", float(hsv.get(0, 0.0)))
   if float(hsv.get(1, 0.0)) > 0.0005 { hue = float(hsv.get(0, 0.0)) }
   _state_set_num(full, "hue", hue)
   _color_preview_row(full, label, out, style)
   def plane_h = clamp(_current_body_w() * 0.40, _sx(86.0), _sx(126.0))
   def plane = _layout_rect(_current_body_w(), plane_h)
   out = _color_sv_plane(full, out, hue,
      float(plane.get(0, 0.0)), float(plane.get(1, 0.0)),
      float(plane.get(2, 0.0)), float(plane.get(3, 0.0)), style)
   def hue_rect = _layout_rect(_current_body_w(), _sx(18.0))
   def hsv2 = _color_rgb_to_hsv(out)
   if float(hsv2.get(1, 0.0)) > 0.0005 { hue = float(hsv2.get(0, hue)) }
   out = _color_hue_strip(full, out, hue,
      float(hue_rect.get(0, 0.0)), float(hue_rect.get(1, 0.0)),
      float(hue_rect.get(2, 0.0)), float(hue_rect.get(3, 0.0)), style)
   out = _color_swatch_row(full, out, style)
   out[0] = slider_float(to_str(id) + ".r", "R", out.get(0, 1.0), 0.0, 1.0, 0.0, style)
   out[1] = slider_float(to_str(id) + ".g", "G", out.get(1, 1.0), 0.0, 1.0, 0.0, style)
   out[2] = slider_float(to_str(id) + ".b", "B", out.get(2, 1.0), 0.0, 1.0, 0.0, style)
   out[3] = slider_float(to_str(id) + ".a", "A", out.get(3, 1.0), 0.0, 1.0, 0.0, style)
   def group_bounds = [_current_body_x_cache, start_y, _current_body_w_cache, max(0.0, _current_cursor_y_cache - start_y)]
   _iw_visible = _rect_intersects(group_bounds, _current_clip())
   _accessibility_record(full, "group", _ui_text(label), out, group_bounds, false, false, false, _disabled_on > 0, {"format": "RGBA", "components": ["R", "G", "B", "A"], "picker": true})
   def hsv3 = _color_rgb_to_hsv(out)
   if float(hsv3.get(1, 0.0)) > 0.0005 { _state_set_num(full, "hue", float(hsv3.get(0, hue))) }
   out
}

fn begin_scroll_area(any id, any w=0.0, any h=0.0) bool {
   "Begins a clipped scroll area inside the current window."
   mut win = _current_window_state()
   if !is_dict(win) || _scroll_area_active { return false }
   def win_id = _current_window_id_cache
   def full = _wid(id)
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
   if hovered && abs(_mouse_scroll_dy) > 0.000001 { scroll_y = clamp(scroll_y - _mouse_scroll_dy * _scroll_step(), 0.0, max_scroll) }
   mut area_st = _widget_state.get(full, 0)
   if !is_dict(area_st) { area_st = dict(12) }
   area_st["scroll_y"] = scroll_y
   _widget_state[full] = area_st
   _scroll_area_capture_window = win_id
   _scroll_area_capture_rect = rect
   if visible {
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
   if visible { push_clip_rect([x + 1.0, y + 1.0, max(1.0, rwf - 2.0), max(1.0, rhf - 2.0)]) }
   visible
}

fn set_scroll_area_content_hint(any id, any content_h) bool {
   "Sets the expected content height for the next scroll-area begin in the current window."
   if !is_dict(_current_window_state()) { return false }
   def full = _wid(id)
   _state_set_num(full, "content_h", max(0.0, float(content_h)))
   true
}

fn scroll_area_ensure_visible(any id, any item_top, any item_bottom, any visible_h) bool {
   "Adjusts a scroll area so the requested content span is visible on next begin."
   if !is_dict(_current_window_state()) { return false }
   def full = _wid(id)
   def content_h = max(0.0, _state_get_num(full, "content_h", 0.0))
   def vh = max(1.0, float(visible_h))
   def max_scroll = max(0.0, content_h - vh)
   mut scroll_y = clamp(_state_get_num(full, "scroll_y", 0.0), 0.0, max_scroll)
   def top = max(0.0, float(item_top))
   def bottom = max(top, float(item_bottom))
   if top < scroll_y { scroll_y = top }
   elif bottom > scroll_y + vh { scroll_y = bottom - vh }
   _state_set_num(full, "scroll_y", clamp(scroll_y, 0.0, max_scroll))
   true
}

fn scroll_area_scroll_y() f64 { "Returns the scroll offset of the active scroll area." _scroll_area_active ? _scroll_area_scroll_y : 0.0 }

fn scroll_area_visible_h() f64 {
   "Returns the visible height of the active scroll area."
   if !_scroll_area_active { return 0.0 }
   max(1.0, float(_scroll_area_rect.get(3, 0.0)) - _sx(8.0))
}

fn end_scroll_area() any {
   "Ends the active scroll area and stores its scroll state."
   if !_scroll_area_active { return 0 }
   mut child = _current_window_state()
   if !is_dict(child) { child = dict(8) }
   if _scroll_area_clip_pushed { pop_clip_rect() }
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
   if is_dict(parent) {
      if _focus_collect_active {
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
   if max_scroll > 1.0 {
      def track_w, track_x = _sx(6.0), x + rwf - track_w - _sx(2.0)
      def track_y, track_h = y + _sx(2.0), max(1.0, rhf - _sx(4.0))
      def thumb_h = max(_sx(18.0), track_h * (track_h / max(content_h, 1.0)))
      def usable = max(1.0, track_h - thumb_h)
      mut thumb_y = track_y + usable * ((max_scroll > 0.0) ? (scroll_y / max_scroll) : 0.0)
      def scroll_id = _scroll_area_id + "#scroll"
      def win_id = is_dict(parent) ? to_str(parent.get("id", "")) : ""
      def track_hover = _window_pointer_hit(win_id, track_x, track_y, track_w, track_h)
      def thumb_hover = _window_pointer_hit(win_id, track_x, thumb_y, track_w, thumb_h)
      if thumb_hover {
         _hot_id = scroll_id
         _hot_rect = [track_x, thumb_y, track_w, thumb_h]
         _ensure_cursors()
         _want_cursor(_cursor_hand)
      }
      if thumb_hover && _mouse_pressed0 {
         _active_widget_id = scroll_id
         _focused_window = win_id
         _state_set_num(scroll_id, "drag_off_y", _mouse_y - thumb_y)
      } elif track_hover && _mouse_pressed0 {
         def want_thumb_y = clamp(_mouse_y - thumb_h * 0.5, track_y, track_y + usable)
         scroll_y = (max_scroll > 0.0) ? (max_scroll * ((want_thumb_y - track_y) / usable)) : 0.0
         thumb_y = track_y + usable * ((max_scroll > 0.0) ? (scroll_y / max_scroll) : 0.0)
      }
      if _active_widget_id == scroll_id {
         if _mouse_down0 {
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
   _scroll_area_parent = nil
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
   def collapse_hover = titlebar && collapsible && _window_pointer_hit(id, collapse_x, sy + _sx(3.0), collapse_btn_w, title_h - _sx(6.0))
   def close_hover = titlebar && closable && _window_pointer_hit(id, close_x, sy + _sx(3.0), close_btn_w, title_h - _sx(6.0))
   def resize_hover = resizable && !collapsed && _window_pointer_hit(id, sx + sw - _resize_h(), sy + sh - _resize_h(), _resize_h(), _resize_h())
   def is_focused = _focused_window == to_str(id)
   def btn_y, btn_h = sy + _sx(3.0), title_h - _sx(6.0)
   ;; One soft drop edge, one border, and a header that brightens with focus.
   ;; Hover alone never recolours the frame: with tiled panels the pointer is
   ;; always over one of them, and a frame that lights up under it is noise.
   _gui_rect_fast(sx + _sx(2.0), sy + _sx(3.0), sw, sh, _shadow_u32)
   _gui_rect_fast(sx, sy, sw, sh, _window_bg_u32)
   if !collapsed {
      _gui_rect_fast(sx + 1.0, sy + title_h, max(1.0, sw - 2.0), max(1.0, sh - title_h - 1.0), _window_body_u32)
   }
   if titlebar {
      _gui_rect_fast(sx + 1.0, sy + 1.0, max(1.0, sw - 2.0), max(1.0, title_h - 1.0), is_focused ? _panel_hover_u32 : _window_hdr_u32)
      if !collapsed { _gui_rect_fast(sx + 1.0, sy + title_h - 1.0, max(1.0, sw - 2.0), 1.0, _border_u32) }
      if is_focused { _gui_rect_fast(sx + 1.0, sy + 1.0, _sx(3.0), max(1.0, title_h - 2.0), _accent_u32) }
      def title_w = max(1.0, button_left - (sx + _pad()) - _sx(10.0))
      def title_hh = _font_h_for(_font_title_id())
      def title_y = _snap(float(sy) + max(0.0, (title_h - title_hh) * 0.5))
      _draw_text_ui_title_clipped(to_str(title), sx + _pad() + _sx(4.0), title_y, is_focused ? _text_u32 : _text_dim_u32, title_w, [sx + 1.0, sy + 1.0, max(1.0, title_w), max(1.0, title_h - 2.0)])
   }
   _gui_rect_outline_fast(sx, sy, sw, sh, is_focused ? _text_faint_u32 : _border_u32)
   if titlebar && collapsible {
      if collapse_hover { _gui_rect_fast(collapse_x, btn_y, collapse_btn_w, btn_h, _panel_active_u32) }
      def mcol = collapse_hover ? _text_u32 : _text_dim_u32
      def mcx, mcy = _snap(collapse_x + collapse_btn_w * 0.5), _snap(btn_y + btn_h * 0.5)
      _gui_rect_fast(mcx - _sx(4.0), mcy, _sx(9.0), max(1.0, _sx(1.0)), mcol)
      if collapsed { _gui_rect_fast(mcx, mcy - _sx(4.0), max(1.0, _sx(1.0)), _sx(9.0), mcol) }
   }
   if titlebar && closable {
      if close_hover { _gui_rect_fast(close_x, btn_y, close_btn_w, btn_h, _danger_u32) }
      def xcol = close_hover ? _window_bg_u32 : _text_dim_u32
      def xcx, xcy = _snap(close_x + close_btn_w * 0.5), _snap(btn_y + btn_h * 0.5)
      def arm = _sx(4.0)
      _gui_diag(xcx - arm, xcy - arm, xcx + arm, xcy + arm, max(1.0, _sx(1.0)), xcol)
      _gui_diag(xcx - arm, xcy + arm, xcx + arm, xcy - arm, max(1.0, _sx(1.0)), xcol)
   }
   if !collapsed && resizable {
      ;; Three stepped strokes in the corner; the hit target stays the full square.
      def gcol = resize_hover ? _text_u32 : _text_faint_u32
      def gx, gy = sx + sw - _sx(3.0), sy + sh - _sx(3.0)
      mut gi = 0
      while gi < 3 {
         def off = _sx(3.0) * float(gi + 1)
         _gui_diag(gx - off, gy, gx, gy - off, max(1.0, _sx(1.0)), gcol)
         gi += 1
      }
   }
}

fn begin_window(any id, any title, f64 x, f64 y, f64 w, f64 h, any opts=0) bool {
   "Begins a movable GUI window and returns true when its body is visible."
   _theme_refresh()
   _current_window = 0
   _sync_current_layout_cache(0)
   if !_enabled {
      return false
   }
   mut st = _ensure_window(id, title, x, y, w, h, opts)
   if !st.get("open", true) {
      return false
   }
   def docked = to_str(st.get("dock", "")) != ""
   if docked && !bool(st.get("dock_show", true)) {
      ;; A background tab: nothing to draw until its tab is chosen.
      st["layered"] = false
      _window_store(st)
      return false
   }
   ;; Beside a dockspace, floating windows are recorded and replayed in
   ;; z-order at end_frame so they stay above the docked panels.
   def layered = !docked && _dock_frame_active
   st["layered"] = layered
   if layered { _layer_begin() }
   _focus_collect_active = _focus_collect_for_window(id)
   st["clip_override"] = 0
   mut sx, sy = _snap(float(st.get("x", x))), _snap(float(st.get("y", y)))
   mut sw, sh = max(_snap(float(st.get("w", w))), _min_win_w()), max(_snap(float(st.get("h", h))), _min_win_h())
   if docked { sw, sh = max(1.0, _snap(float(st.get("w", w)))), max(1.0, _snap(float(st.get("h", h)))) }
   def closable = st.get("closable", true)
   def movable = st.get("movable", true)
   def resizable = st.get("resizable", true)
   def scrollable = st.get("scrollable", true)
   def titlebar = st.get("titlebar", true)
   def title_h = titlebar ? _title_h() : 0.0
   def collapsible = st.get("collapsible", true)
   mut bool collapsed = false
   if bool(titlebar) && bool(collapsible) {
      collapsed = bool(st.get("collapsed", false))
   }
   def body_h = max(1.0, sh - title_h - _pad())
   def max_scroll = scrollable ? max(0.0, float(st.get("content_h", 0.0)) - body_h) : 0.0
   mut scroll_y = clamp(float(st.get("scroll_y", 0.0)), 0.0, max_scroll)
   def whole_hover = _window_pointer_hit(id, sx, sy, sw, collapsed ? title_h : sh)
   if whole_hover && _mouse_pressed0 {
      _focused_window = to_str(id)
      _window_order_touch(id, true)
      if _active_window_move != to_str(id) && _active_window_resize != to_str(id) {
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
   def collapse_hover = titlebar && collapsible && _window_pointer_hit(id, collapse_x, sy + _sx(3.0), collapse_btn_w, title_h - _sx(6.0))
   def close_hover = titlebar && closable && _window_pointer_hit(id, close_x, sy + _sx(3.0), close_btn_w, title_h - _sx(6.0))
   if movable && title_hover && !collapse_hover && !close_hover {
      _ensure_cursors()
      _want_cursor(_cursor_move)
   }
   if close_hover && _mouse_pressed0 {
      st["open"] = false
      _window_store(st)
      _window_closed_events[to_str(id)] = true
      if _focused_window == to_str(id) { _focused_window = "" }
      _active_widget_id = ""
      _active_window_move = ""
      _active_window_resize = ""
      _text_focus_id = ""
      _kbd_focus_id = ""
      return false
   }
   if collapse_hover && _mouse_pressed0 {
      collapsed = !collapsed
      st["collapsed"] = collapsed
      if collapsed { scroll_y = 0.0 }
   }
   if movable && title_hover && !_mouse_pressed0 && _active_window_move == to_str(id) && !_mouse_down0 { _active_window_move = "" }
   if movable && title_hover && _mouse_pressed0 && !collapse_hover && !close_hover {
      _active_window_move = to_str(id)
      _move_off_x = _mouse_x - sx
      _move_off_y = _mouse_y - sy
   }
   if _active_window_move == to_str(id) && _mouse_down0 && _mouse_drag_ready() { sx, sy = _snap(_mouse_x - _move_off_x), _snap(_mouse_y - _move_off_y) }
   def resize_x, resize_y = sx + sw - _resize_h(), sy + sh - _resize_h()
   def resize_hover = resizable && !collapsed && _window_pointer_hit(id, resize_x, resize_y, _resize_h(), _resize_h())
   if resize_hover || (_active_window_resize == to_str(id) && _mouse_down0) {
      _ensure_cursors()
      _want_cursor(_cursor_resize)
   }
   if resizable && resize_hover && _mouse_pressed0 {
      _active_window_resize = to_str(id)
      _resize_pad_x = sx + sw - _mouse_x
      _resize_pad_y = sy + sh - _mouse_y
   }
   if _active_window_resize == to_str(id) && !_mouse_down0 { _active_window_resize = "" }
   if _active_window_resize == to_str(id) && _mouse_down0 && _mouse_drag_ready() { sw, sh = max(_min_win_w(), _snap(_mouse_x - sx + _resize_pad_x)), max(_min_win_h(), _snap(_mouse_y - sy + _resize_pad_y)) }
   st["x"] = sx st["y"] = sy
   st["w"] = sw st["h"] = sh
   if !docked { st = _clamp_window_pos(st) }
   sx = float(st.get("x", sx))
   sy, sw = float(st.get("y", sy)), float(st.get("w", sw))
   sh = float(st.get("h", sh))
   if scrollable && whole_hover && abs(_mouse_scroll_dy) > 0.000001 && !collapsed && !_pointer_over_scroll_area(id) { scroll_y = clamp(scroll_y - _mouse_scroll_dy * _scroll_step(), 0.0, max_scroll) }
   if scrollable
   && !collapsed
   && _focused_window == to_str(id)
   && _text_focus_id == ""
   && _active_window_move != to_str(id)
   && _active_window_resize != to_str(id){
      def page_step = max(_sx(36.0), body_h * 0.90)
      if _key_event_present(KEY_PAGE_UP) { scroll_y = clamp(scroll_y - page_step, 0.0, max_scroll) }
      if _key_event_present(KEY_PAGE_DOWN) { scroll_y = clamp(scroll_y + page_step, 0.0, max_scroll) }
      if _key_event_present(KEY_HOME) { scroll_y = 0.0 }
      if _key_event_present(KEY_END) { scroll_y = max_scroll }
   }
   st["scroll_y"] = scroll_y
   _draw_window_frame(id, title, st, titlebar, collapsed, closable, resizable, collapsible)
   def origin = _window_content_origin(st)
   st["body_x"] = float(origin.get(0, sx + _pad()))
   st["body_y"] = float(origin.get(1, sy + title_h + _pad()))
   st["body_w"] = max(1.0, sw - _pad() * 2.0)
   st["body_h"] = body_h
   st["cursor_y"] = float(origin.get(1, sy + title_h + _pad()))
   if _focus_collect_active { st["focusables"] = [] st["focusable_map"] = dict(64) }
   ;; A window body is always clipped. Clipping only when the content
   ;; overflowed let an oversized widget (a wide button or a long row) paint
   ;; outside its panel whenever the panel happened to fit its content.
   def needs_clip = !collapsed
   st["clip_active"] = needs_clip
   _same_line_next = false
   _same_line_gap = _gap()
   _window_store(st)
   _current_window = st
   _sync_current_layout_cache(st)
   _last_item = [_current_body_x_cache, _current_cursor_y_cache, 0.0, 0.0]
   if needs_clip { push_clip_rect(_window_body_clip(st)) }
   _accessibility_nodes = _accessibility_nodes.append({"id": "window:" + to_str(id), "parent": "", "role": "window", "name": _ui_text(title), "bounds": [sx, sy, sw, collapsed ? title_h : sh], "disabled": false, "focused": _focused_window == to_str(id), "children": []})
   !collapsed
}

fn end_window() any {
   "Ends the current GUI window and stores layout metrics."
   mut st = _current_window_state()
   if !is_dict(st) { return 0 }
   def win_id = _current_window_id_cache
   if st.get("clip_active", false) { pop_clip_rect() }
   if st.get("collapsed", false) {
      _window_store(st)
      if st.get("layered", false) { _layer_end(win_id) }
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
   if content_h < 0.0 { content_h = 0.0 }
   st["content_h"] = content_h
   def scrollable = st.get("scrollable", true)
   def max_scroll = scrollable ? max(0.0, content_h - max(1.0, sh - _title_h() - _pad())) : 0.0
   mut scroll_y = clamp(float(st.get("scroll_y", 0.0)), 0.0, max_scroll)
   st["scroll_y"] = scroll_y
   if scrollable && max_scroll > 1.0 {
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
      if thumb_hover {
         _hot_id = scroll_id
         _hot_rect = [track_x, bar_y, track_w, bar_h]
         _ensure_cursors()
         _want_cursor(_cursor_hand)
      }
      if thumb_hover && _mouse_pressed0 {
         _active_widget_id = scroll_id
         _focused_window = win_id
         _state_set_num(scroll_id, "drag_off_y", _mouse_y - bar_y)
      } elif track_hover && _mouse_pressed0 {
         def want_bar_y = clamp(_mouse_y - bar_h * 0.5, track_y, track_y + avail)
         scroll_y = (max_scroll > 0.0) ? (max_scroll * ((want_bar_y - track_y) / avail)) : 0.0
         st["scroll_y"] = scroll_y
         bar_y = want_bar_y
      }
      if _active_widget_id == scroll_id {
         if _mouse_down0 {
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
   if st.get("layered", false) { _layer_end(win_id) }
   _current_window = 0
   _sync_current_layout_cache(0)
   _focus_collect_active = false
   _same_line_next = false
}

;; ---------------------------------------------------------------------------
;; Toolkit layer: overlay command list, popups and menus, tooltips, trees,
;; numeric drag fields, tables, drag and drop, layout scopes, themes.
;;
;; Widget contract (every entry has a mock probe in
;; etc/labs/tests/code/probes/ui/gui-toolkit-*.nshape):
;;
;;   windows            begin_window / end_window (move, resize, collapse, close, scroll)
;;   menu bar, menus    begin_menu_bar, begin_menu (nests as submenus), menu_item, menu_separator
;;   context menus      begin_context_menu (secondary button on the previous item)
;;   popups             open_popup, begin_popup, close_popup; Escape and outside press dismiss
;;   tabs               tab_strip
;;   trees              collapsing_header, tree_node / tree_pop
;;   buttons            button, small_button, icon_button, selectable
;;   choices            checkbox, toggle, radio_button, combo_box
;;   numbers            slider_float, slider_int, drag_float, drag_int (double click or Enter to type)
;;   text entry         input_text, title_input_text (clipboard, select-all, UTF-8 cursor)
;;   tooltips           tooltip (after an item, shown after a hover delay)
;;   scrolling          window scroll, begin_scroll_area
;;   tables, lists      begin_table (sortable, resizable columns), selectable_file
;;   drag and drop      drag_source / drop_target (typed payloads, inside one GUI)
;;   keyboard           Tab / Shift-Tab focus order, Enter / Space activate, arrows in menus
;;   layout             same_line, layout_row, begin_columns, indent, spacing, split_cols / split_rows
;;   identity           push_id / pop_id scope repeated widgets; ids are window-scoped by default
;;   state              begin_disabled, item_status ("warn" / "error" / "ok"),
;;                      panel_state ("empty" / "loading" / "error")
;;   themes             set_theme("dark" | "light"), set_theme_color, set_accent
;;   draw order         overlay command list (popups, menus, tooltips draw above all windows)
;;
;;   docking            dockspace, dock_window / undock_window, dock_layout_save / dock_layout_load
;;
;; Intentionally unsupported: modal popups, multi-line text
;; entry (the editor owns that), IME pre-edit display inside input_text (the
;; committed text arrives as character events), and drags between two OS
;; windows.
;; ---------------------------------------------------------------------------
fn set_headless_input(bool on=true) any {
   "Drives the GUI from fed events alone, with no window handle: prepare_input(0, w, h) then keeps the pointer and button state between frames. Used by probes and replays."
   _headless_input = !!on
}

fn last_item_id() str { "Returns the full id of the last interactive item." _last_item_id }

fn _overlay_replay(list cmds) any {
   def int n = cmds.len
   if n <= 0 { return 0 }
   mut int i = 0
   while i + 5 < n {
      def int i1, i2 = i + 1, i + 2
      def int i3, i4 = i + 3, i + 4
      def int i5 = i + 5
      def kind = int(cmds.get(i, 0))
      if kind == 0 {
         gui_batch.queue_rect(float(cmds.get(i1, 0.0)), float(cmds.get(i2, 0.0)), float(cmds.get(i3, 0.0)), float(cmds.get(i4, 0.0)), int(cmds.get(i5, 0)))
      } elif kind == 1 {
         gui_batch.queue_outline(float(cmds.get(i1, 0.0)), float(cmds.get(i2, 0.0)), float(cmds.get(i3, 0.0)), float(cmds.get(i4, 0.0)), int(cmds.get(i5, 0)))
      } elif kind == 2 {
         _flush_rect_batch()
         draw_text(int(cmds.get(i1, 0)), to_str(cmds.get(i2, "")), float(cmds.get(i3, 0.0)), float(cmds.get(i4, 0.0)), int(cmds.get(i5, 0)))
      } elif kind == 3 {
         _flush_rect_batch()
         scissor_push(float(cmds.get(i1, 0.0)), float(cmds.get(i2, 0.0)), float(cmds.get(i3, 0.0)), float(cmds.get(i4, 0.0)))
      } elif kind == 4 {
         _flush_rect_batch()
         scissor_pop()
      } elif kind == 7 {
         def shape = cmds.get(i1, [])
         if is_list(shape) && shape.len == 6 { _draw_flex_rounded(shape, cmds.get(i2, 0)) }
      } elif kind == 8 {
         _flush_rect_batch()
         def style = cmds.get(i5, [])
         draw_text(int(cmds.get(i1, 0)), to_str(cmds.get(i2, "")), float(cmds.get(i3, 0.0)), float(cmds.get(i4, 0.0)), int(style.get(0, 0)), float(style.get(1, 0.0)))
      } elif kind == 6 {
         _flush_rect_batch()
         def any ln = cmds.get(i1, 0)
         if is_list(ln) { draw_line_2d(float(ln.get(0, 0.0)), float(ln.get(1, 0.0)), float(ln.get(2, 0.0)), float(ln.get(3, 0.0)), cmds.get(i2, 0), float(ln.get(4, 1.0))) }
      } elif kind == 5 {
         _flush_rect_batch()
         def any t = cmds.get(i1, 0)
         if is_list(t) {
            gui_batch.draw_tex_uv(float(t.get(0, 0.0)), float(t.get(1, 0.0)), float(t.get(2, 0.0)), float(t.get(3, 0.0)), int(cmds.get(i2, -1)),
               float(t.get(4, 0.0)), float(t.get(5, 0.0)), float(t.get(6, 1.0)), float(t.get(7, 1.0)),
               float(t.get(8, 1.0)), float(t.get(9, 1.0)), float(t.get(10, 1.0)), float(t.get(11, 1.0)))
         }
      }
      i += 6
   }
   _flush_rect_batch()
}

fn _overlay_finish() any {
   "Queues the drag preview and the tooltip, then replays the overlay above the windows."
   if _dnd_active && _dnd_label.len > 0 { _overlay_tag(_dnd_label, _mouse_x + _sx(14.0), _mouse_y + _sx(10.0), _accent_u32) }
   if _tip_text.len > 0 {
      _overlay_tag(_tip_text, _mouse_x + _sx(12.0), _mouse_y + _sx(18.0), _border_u32)
      _tip_text = ""
   }
   ;; Floating window layers bottom to top, then popups, then the tooltip,
   ;; drag preview, and dock drop preview.
   def int layer_n = _win_layers.len
   if layer_n > 0 {
      mut oi = 0
      def int order_n = _window_order.len
      while oi < order_n {
         def wid = to_str(_window_order.get(oi, ""))
         mut li = 0
         while li + 1 < layer_n {
            if to_str(_win_layers.get(li, "")) == wid {
               def int seg_i = li + 1
               def any seg = _win_layers.get(seg_i, 0)
               if is_list(seg) { _overlay_replay(seg) }
            }
            li += 2
         }
         oi += 1
      }
      _win_layers = []
   }
   def top = _ov_top
   _ov_top = []
   _overlay_replay(top)
   def misc = _ov_cmds
   _ov_cmds = []
   _overlay_replay(misc)
}

fn _layer_begin() any {
   _ov_depth += 1
   _ov_stack = _ov_stack.append(_ov_cmds)
   _ov_cmds = []
}

fn _layer_end(str win_id) any {
   def seg = _ov_cmds
   def int sn = _ov_stack.len
   def any parent = sn > 0 ? _ov_stack.get(sn - 1, 0) : 0
   _ov_stack = _list_drop_last(_ov_stack)
   mut restored = []
   if is_list(parent) { restored = parent }
   _ov_cmds = restored
   _ov_depth -= 1
   _win_layers = _win_layers.append(win_id)
   _win_layers = _win_layers.append(seg)
}

fn _gui_line(f64 x1, f64 y1, f64 x2, f64 y2, any rgba, f64 thick) any {
   "Draws a 2D line through the window layer when one is being recorded."
   if _ov_depth > 0 {
      _ov_push(6, [x1, y1, x2, y2, thick], rgba, 0, 0, 0)
      return 0
   }
   draw_line_2d(x1, y1, x2, y2, rgba, thick)
}

fn _overlay_tag(str txt, f64 x, f64 y, int edge) any {
   def fid = _font_small_id()
   def tw = _text_fit_width(fid, txt)
   def w, h = tw + _sx(12.0), _font_h_for(fid) + _sx(8.0)
   mut px, py = x, y
   if _fb_w > 0.0 && px + w > _fb_w { px = max(0.0, _fb_w - w) }
   if _fb_h > 0.0 && py + h > _fb_h { py = max(0.0, _mouse_y - h - _sx(4.0)) }
   _ov_depth += 1
   _gui_rect_fast(px + _sx(2.0), py + _sx(2.0), w, h, _shadow_u32)
   _gui_rect_fast(px, py, w, h, _popup_u32)
   _gui_rect_outline_fast(px, py, w, h, edge)
   _queue_text_ui(fid, txt, px + _sx(6.0), py + _sx(4.0), _text_u32, 0.0)
   _ov_depth -= 1
}

;; ---- themes ---------------------------------------------------------------
fn set_theme(str name) bool {
   "Selects a built-in palette: \"dark\" (default) or \"light\". Token overrides made with set_theme_color are replaced."
   if name == "light" {
      _accent_rgba = [0.380, 0.270, 0.800, 1.00]
      _press_rgba = [0.760, 0.710, 0.950, 1.00]
      _window_bg_rgba = [0.930, 0.930, 0.950, 1.00]
      _window_body_rgba = [0.965, 0.965, 0.975, 0.98]
      _window_hdr_rgba = [0.900, 0.900, 0.925, 1.00]
      _panel_rgba = [0.915, 0.915, 0.935, 1.00]
      _panel_hover_rgba = [0.870, 0.870, 0.900, 1.00]
      _panel_active_rgba = [0.810, 0.810, 0.860, 1.00]
      _popup_rgba = [0.985, 0.985, 0.990, 1.00]
      _border_rgba = [0.740, 0.740, 0.790, 1.00]
      _text_rgba = [0.090, 0.090, 0.120, 1.00]
      _text_dim_rgba = [0.330, 0.330, 0.390, 1.00]
      _text_faint_rgba = [0.550, 0.550, 0.610, 1.00]
      _ok_rgba = [0.100, 0.520, 0.240, 1.00]
      _warn_rgba = [0.660, 0.430, 0.020, 1.00]
      _danger_rgba = [0.780, 0.160, 0.120, 1.00]
   } elif name == "dark" {
      _accent_rgba = [0.714, 0.627, 1.000, 1.00]
      _press_rgba = [0.290, 0.243, 0.447, 1.00]
      _window_bg_rgba = [0.020, 0.020, 0.024, 1.00]
      _window_body_rgba = [0.031, 0.031, 0.039, 0.98]
      _window_hdr_rgba = [0.055, 0.055, 0.067, 1.00]
      _panel_rgba = [0.067, 0.067, 0.082, 1.00]
      _panel_hover_rgba = [0.102, 0.102, 0.125, 1.00]
      _panel_active_rgba = [0.141, 0.141, 0.173, 1.00]
      _popup_rgba = [0.047, 0.047, 0.059, 1.00]
      _border_rgba = [0.149, 0.149, 0.180, 1.00]
      _text_rgba = [0.941, 0.941, 0.953, 1.00]
      _text_dim_rgba = [0.643, 0.643, 0.682, 1.00]
      _text_faint_rgba = [0.435, 0.435, 0.478, 1.00]
      _ok_rgba = [0.545, 0.863, 0.604, 1.00]
      _warn_rgba = [0.949, 0.784, 0.475, 1.00]
      _danger_rgba = [1.000, 0.478, 0.420, 1.00]
   } else {
      return false
   }
   _theme_name = name
   _theme_button_styles = dict(0)
   _theme_file_path = ""
   _theme_file_snapshot = ""
   _theme_dirty = true
   true
}

fn theme() str { "Returns the name of the active built-in palette." _theme_name }

fn _theme_file_color_valid(any value) bool {
   if !is_list(value) || value.len != 4 { return false }
   for channel in value {
      if !is_number(channel) || float(channel) < 0.0 || float(channel) > 1.0 { return false }
   }
   true
}

fn _theme_file_states_valid(any states) bool {
   if !is_dict(states) { return false }
   for component in states.keys() {
      if to_str(component) != "button" { return false }
      def button_states = states.get(component, nil)
      if !is_dict(button_states) { return false }
      for state in button_states.keys() {
         def state_name = to_str(state)
         if state_name != "active" && state_name != "hovered" && state_name != "pressed" && state_name != "disabled" { return false }
         def style = button_states.get(state, nil)
         if !is_dict(style) { return false }
         for part in style.keys() {
            def part_name = to_str(part)
            if part_name != "background" && part_name != "border" && part_name != "text" { return false }
            if !_theme_file_color_valid(style.get(part, nil)) { return false }
         }
      }
   }
   true
}

fn _theme_file_apply(str source) bool {
   def decoded = json.json_try_decode(source)
   if !decoded.get("ok", false) { return false }
   def doc = decoded.get("value", nil)
   if !is_dict(doc) || to_str(doc.get("format", "")) != "nytrix-ui-theme" || int(doc.get("version", 0)) != 1 { return false }
   def name = to_str(doc.get("name", ""))
   if name.len == 0 { return false }
   def parts = doc.get("parts", nil)
   if !is_dict(parts) { return false }
   def token_names = ["text", "text_dim", "text_faint", "accent", "ok", "warn", "danger", "surface", "surface_hover", "surface_active", "window", "header", "popup", "border", "press"]
   for token in token_names { if !_theme_file_color_valid(parts.get(to_str(token), nil)) { return false } }
   for key in parts.keys() { if !token_names.contains(to_str(key)) { return false } }
   mut state_styles = dict(8)
   def states = doc.get("states", dict(0))
   if !_theme_file_states_valid(states) { return false }
   if states.contains("button") { state_styles = states.get("button", dict(0)) }
   ;; All input is validated before mutating the live palette.
   for token in token_names { set_theme_color(to_str(token), parts.get(to_str(token))) }
   _theme_name = name
   _theme_button_styles = state_styles
   _theme_dirty = true
   true
}

fn export_theme() dict {
   "Returns the active theme as versioned parts plus button-state styles in the nytrix-ui-theme JSON schema."
   def token_names = ["text", "text_dim", "text_faint", "accent", "ok", "warn", "danger", "surface", "surface_hover", "surface_active", "window", "header", "popup", "border", "press"]
   mut parts = dict(20)
   for token in token_names { parts[to_str(token)] = theme_color(to_str(token)) }
   {"format": "nytrix-ui-theme", "version": 1, "name": _theme_name, "parts": parts, "states": {"button": _theme_button_styles}}
}

fn file_dialog(any id, str start_dir=".") any {
   "Draws an open stock file picker rooted at `start_dir`; folders navigate and files are chosen. Returns {done, ok, path}."
   def key = to_str(id)
   def d = _dialogs.get(key, nil)
   if !is_dict(d) || !bool(d.get("open", false)) { return {"done": false, "ok": false, "path": ""} }
   mut st = d
   if !st.contains("dir") { st["dir"] = start_dir.len > 0 ? start_dir : "." st["sel"] = -1 }
   def dir = to_str(st.get("dir", "."))
   if !_dialog_begin(key, "Open file", 380.0, 320.0) { return {"done": false, "ok": false, "path": ""} }
   text_colored(dir, "text_dim")
   mut rows = []
   mut paths = []
   mut kinds = []
   if dir != "/" {
      rows = rows.append({"label": "..", "detail": "folder"})
      paths = paths.append(ospath.dirname(dir))
      kinds = kinds.append(true)
   }
   for name in fs.list_dir(dir) {
      def full = ospath.join(dir, to_str(name))
      def isdir = fs.is_dir(full)
      rows = rows.append({"label": to_str(name) + (isdir ? "/" : ""), "detail": isdir ? "folder" : "file"})
      paths = paths.append(full)
      kinds = kinds.append(isdir)
   }
   mut int sel = int(st.get("sel", -1))
   sel = list_view("stockfile_list", rows, _current_body_w(), 180.0, sel)
   st["sel"] = sel
   _dialogs[key] = st
   spacing(1.0)
   def chosen = _dialog_buttons(["Open", "Cancel"])
   end_window()
   if chosen == 1 {
      _dialogs = _dialogs.delete(key)
      return {"done": true, "ok": false, "path": ""}
   }
   if chosen == 0 && sel >= 0 && sel < paths.len {
      def target = to_str(paths.get(sel))
      if bool(kinds.get(sel)) {
         st["dir"] = target
         st["sel"] = -1
         _dialogs[key] = st
         return {"done": false, "ok": false, "path": ""}
      }
      _dialogs = _dialogs.delete(key)
      return {"done": true, "ok": true, "path": target}
   }
   {"done": false, "ok": false, "path": ""}
}

;; ---- icon packs -----------------------------------------------------------
fn _icon_pack_dict() any {
   _ensure_gui_state()
   if !is_dict(_icon_pack) { _icon_pack = dict(64) }
   _icon_pack
}

fn _icon_parse_int(str raw) int {
   def t = str.strip(raw)
   if t.len <= 0 { return -1 }
   mut int i = 0
   mut int sign = 1
   if load8(t, 0) == 45 { sign = -1 i = 1 }
   mut bool hex = false
   if i + 1 < t.len && load8(t, i) == 48 && ((load8(t, i + 1) | 32) == 120) { hex = true i += 2 }
   mut int n = 0
   mut bool seen = false
   mut int digit = 0
   while i < t.len && digit >= 0 {
      def c = load8(t, i) & 255
      digit = -1
      if c >= 48 && c <= 57 { digit = c - 48 }
      elif hex && c >= 97 && c <= 102 { digit = c - 87 }
      elif hex && c >= 65 && c <= 70 { digit = c - 55 }
      if digit >= 0 {
         n = n * (hex ? 16 : 10) + digit
         seen = true
         i += 1
      }
   }
   seen ? n * sign : -1
}

fn _icon_hex(int n) str {
   if n == 0 { return "0" }
   def digits = "0123456789ABCDEF"
   mut out = ""
   mut int v = n
   while v > 0 {
      def d = v % 16
      out = str.str_slice(digits, d, d + 1) + out
      v = v / 16
   }
   out
}

fn _icon_pack_line(str line) list {
   mut l = str.strip(line)
   if l.len == 0 || str.startswith(l, "//") { return [] }
   def comment = str.find(l, "//")
   if comment >= 0 { l = str.strip(str.str_slice(l, 0, comment)) }
   if l.len == 0 { return [] }
   def parts = str.split(l, " ")
   mut toks = []
   for part in parts {
      def t = str.strip(part)
      if t.len > 0 { toks = toks.append(t) }
   }
   if toks.len >= 3 && to_str(toks.get(0)) == "#define" { return [to_str(toks.get(1)), to_str(toks.get(2))] }
   if toks.len >= 3 && (to_str(toks.get(1)) == "=" || to_str(toks.get(1)) == ":") { return [to_str(toks.get(0)), to_str(toks.get(2))] }
   if toks.len >= 2 { return [to_str(toks.get(0)), to_str(toks.get(1))] }
   []
}

fn icon_pack_load(str path) int {
   "Loads `NAME codepoint` lines or C-style `#define NAME 0x...` icon packs; returns the glyph count or -1."
   _icon_pack_dict()
   if path.len == 0 { return -1 }
   match file_read(path) {
      ok(source) -> {
         mut pack = dict(64)
         for line in str.split(to_str(source), "\n") {
            def pair = _icon_pack_line(to_str(line))
            if pair.len >= 2 {
               def cp = _icon_parse_int(to_str(pair.get(1)))
               if cp >= 0 { pack[to_str(pair.get(0))] = cp }
            }
         }
         _icon_pack = pack
         pack.len
      }
      err(_) -> -1
   }
}

fn icon_pack_save(str path) bool {
   "Writes the loaded icons as a `#define NAME 0x...` pack."
   def pack = _icon_pack_dict()
   if path.len == 0 { return false }
   mut out = ""
   for name in pack.keys() { out = out + "#define " + to_str(name) + " 0x" + _icon_hex(int(pack.get(name))) + "\n" }
   match file_write(path, out) {
      ok(_) -> true
      err(_) -> false
   }
}

fn icon_pack_define(any name, int codepoint) bool {
   "Adds or replaces one icon glyph in the active pack."
   if codepoint < 0 { return false }
   def pack = _icon_pack_dict()
   pack[to_str(name)] = codepoint
   _icon_pack = pack
   true
}

fn icon_pack_clear() any {
   "Empties the active icon pack."
   _icon_pack = dict(64)
   0
}

fn icon_pack_count() int {
   "Returns the number of glyphs in the active icon pack."
   def pack = _icon_pack_dict()
   int(pack.len)
}

fn icon_pack_glyph(any name) int {
   "Returns the codepoint for an icon name, or -1."
   def pack = _icon_pack_dict()
   int(pack.get(to_str(name), -1))
}

fn icon_pack_text(any name) str {
   "Returns the UTF-8 glyph for an icon name, or the empty string."
   def cp = icon_pack_glyph(name)
   cp < 0 ? "" : str.chr(cp)
}

fn save_theme_file(str path) bool {
   "Writes the current theme parts and state styles as JSON."
   if path.len == 0 { return false }
   match file_write(path, json.json_encode(export_theme())) {
      ok(_) -> true
      err(_) -> false
   }
}

fn load_theme_file(str path) bool {
   "Loads and validates a version 1 theme JSON file; invalid files leave the active theme unchanged."
   if path.len == 0 { return false }
   match file_read(path) {
      ok(source) -> {
         def content = to_str(source)
         if !_theme_file_apply(content) { return false }
         _theme_file_path = path
         _theme_file_snapshot = content
         _theme_file_poll_frame = int(_gui_frame_id)
         true
      }
      err(_) -> false
   }
}

fn watch_theme_file(str path) bool {
   "Loads a theme file and enables periodic hot reload while a GUI frame loop is running."
   load_theme_file(path)
}

fn unwatch_theme_file() any {
   "Stops hot-reloading the active theme file without changing the current palette."
   _theme_file_path = ""
   _theme_file_snapshot = ""
   _theme_file_poll_frame = -1
}

fn poll_theme_file() bool {
   "Reloads a watched theme only when its file contents change; invalid edits are ignored and retried on the next poll."
   if _theme_file_path.len == 0 { return false }
   match file_read(_theme_file_path) {
      ok(source) -> {
         def content = to_str(source)
         _theme_file_poll_frame = int(_gui_frame_id)
         if content == _theme_file_snapshot { return false }
         if !_theme_file_apply(content) { return false }
         _theme_file_snapshot = content
         true
      }
      err(_) -> false
   }
}

fn _theme_color_changed(list original, list edited) bool {
   mut channel = 0
   while channel < 4 {
      if float(original.get(channel, 1.0)) != float(edited.get(channel, 1.0)) { return true }
      channel += 1
   }
   false
}

fn theme_editor(any id, str path="") dict {
   "Draws an interactive theme editor for semantic colors and button state colors. Returns the edited path and whether the window remains open."
   mut file_path = path
   mut saved, watched = false, false
   def editor_id = _wid(id)
   def opened = begin_window(id, "Theme Editor", 24.0, 24.0, 410.0, 650.0)
   if !opened { return {"open": window_visible(id), "path": file_path, "saved": false, "watched": false} }
   text("Theme: " + _theme_name)
   def parts = ["text", "text_dim", "text_faint", "accent", "ok", "warn", "danger", "surface", "surface_hover", "surface_active", "window", "header", "popup", "border", "press"]
   mut part_idx = int(_state_get_num(editor_id, "part", 0.0))
   part_idx = combo_box(to_str(id) + ".part", "Semantic color", parts, part_idx)
   _state_set_num(editor_id, "part", float(part_idx))
   def part_name = to_str(parts.get(part_idx, "text"))
   def part_color = theme_color(part_name)
   def edited_part = color_picker4(to_str(id) + ".part-color." + part_name, part_name, part_color)
   if _theme_color_changed(part_color, edited_part) { set_theme_color(part_name, edited_part) }
   separator()
   text("Button state colors")
   def state_names = ["active", "hovered", "pressed", "disabled"]
   mut state_idx = int(_state_get_num(editor_id, "button-state", 0.0))
   state_idx = combo_box(to_str(id) + ".button-state", "Button state", state_names, state_idx)
   _state_set_num(editor_id, "button-state", float(state_idx))
   def state_name = to_str(state_names.get(state_idx, "active"))
   mut state_style = _theme_button_styles.get(state_name, dict(0))
   def components = ["background", "border", "text"]
   def component_idx = combo_box(to_str(id) + ".component", "Button color", components, int(_state_get_num(editor_id, "component", 0.0)))
   _state_set_num(editor_id, "component", float(component_idx))
   def component = to_str(components.get(component_idx, "background"))
   mut default_token = "text"
   if component == "background" {
      default_token = state_name == "pressed" ? "press" : (state_name == "hovered" ? "surface_active" : "surface_hover")
   } elif component == "border" {
      default_token = state_name == "pressed" ? "accent" : (state_name == "hovered" ? "text_faint" : "border")
   }
   def old_color = _color_rgba4(state_style.get(component, theme_color(default_token)))
   def edited = color_picker4(to_str(id) + ".button." + state_name + "." + component, component, old_color)
   if _theme_color_changed(old_color, edited) {
      state_style[component] = edited
      _theme_button_styles[state_name] = state_style
   }
   separator()
   file_path = input_text(to_str(id) + ".path", "Theme JSON file", file_path, "path/to/theme.json")
   if button(to_str(id) + ".save", "Save theme") { saved = save_theme_file(file_path) }
   same_line()
   if button(to_str(id) + ".watch", "Load and watch") { watched = watch_theme_file(file_path) }
   same_line()
   if button(to_str(id) + ".stop", "Stop watching") { unwatch_theme_file() }
   if _theme_file_path.len > 0 { label("Watching: " + _theme_file_path) }
   end_window()
   {"open": true, "path": file_path, "saved": saved, "watched": watched}
}

fn _theme_hot_reload_tick() any {
   if _theme_file_path.len > 0 && int(_gui_frame_id) - _theme_file_poll_frame >= 30 { poll_theme_file() }
   0
}

;; ---- identity and layout scopes -------------------------------------------
fn push_id(any id) any {
   "Scopes the ids of the following widgets under `id`, so repeated rows can reuse the same widget ids."
   _id_stack = _id_stack.append(_widget_id_prefix)
   _widget_id_prefix = _widget_id_prefix + to_str(id) + "/"
}

fn pop_id() any {
   "Leaves the innermost push_id scope."
   def n = _id_stack.len
   if n <= 0 { return 0 }
   _widget_id_prefix = to_str(_id_stack.get(n - 1, ""))
   _id_stack = _list_drop_last(_id_stack)
}

fn _list_drop_last(list xs) list {
   mut out = []
   mut i = 0
   def n = xs.len - 1
   while i < n {
      out = out.append(xs.get(i, 0))
      i += 1
   }
   out
}

fn indent(any px=0.0) any {
   "Shifts the layout origin right by `px` (default one tree level) until the matching unindent."
   def d = _pick_positive(px, _sx(14.0))
   def step = min(d, max(0.0, _current_body_w_cache - _sx(24.0)))
   _indent_stack = _indent_stack.append(step)
   _current_body_x_cache = _current_body_x_cache + step
   _current_body_w_cache = _current_body_w_cache - step
}

fn unindent() any {
   "Undoes the innermost indent."
   def n = _indent_stack.len
   if n <= 0 { return 0 }
   def step = float(_indent_stack.get(n - 1, 0.0))
   _indent_stack = _list_drop_last(_indent_stack)
   _current_body_x_cache = _current_body_x_cache - step
   _current_body_w_cache = _current_body_w_cache + step
}

fn layout_row(list widths, f64 gap=-1.0) int {
   "Lays the next `widths.len` widgets out on one row. A width above 1 is pixels, a width in(0,1] is a fraction of the row, and 0 shares what is left equally. Returns the cell count."
   def n = widths.len
   if n <= 0 || _current_window_id_cache.len <= 0 { return 0 }
   def g = gap >= 0.0 ? gap : _gap()
   def total = max(1.0, _current_body_w_cache - g * float(n - 1))
   mut fixed = 0.0
   mut flex = 0
   mut i = 0
   while i < n {
      def wv = float(widths.get(i, 0.0))
      if wv > 1.0 { fixed += wv }
      elif wv > 0.0 { fixed += total * wv }
      else { flex += 1 }
      i += 1
   }
   def share = flex > 0 ? max(1.0, (total - fixed) / float(flex)) : 0.0
   mut cells = []
   mut x = _current_body_x_cache
   def right = _current_body_x_cache + _current_body_w_cache
   i = 0
   while i < n {
      def wv = float(widths.get(i, 0.0))
      mut cw = share
      if wv > 1.0 { cw = wv }
      elif wv > 0.0 { cw = total * wv }
      if i == n - 1 && flex > 0 { cw = max(1.0, right - x) }
      cw = max(1.0, min(cw, max(1.0, right - x)))
      cells = cells.append(_snap(x))
      cells = cells.append(_snap(cw))
      x = x + cw + g
      i += 1
   }
   _row_cells = cells
   _row_n, _row_i = n, 0
   _row_y, _row_bottom = _current_cursor_y_cache, _current_cursor_y_cache
   _same_line_next = false
   n
}

fn _row_take(f64 h) list {
   def int cx_i, cw_i = _row_i * 2, _row_i * 2 + 1
   def x = float(_row_cells.get(cx_i, _current_body_x_cache))
   def w = float(_row_cells.get(cw_i, 1.0))
   def rect = [x, _snap(_row_y), w, _snap(h)]
   _row_bottom = max(_row_bottom, _row_y + h + _gap())
   _row_i += 1
   if _row_i >= _row_n {
      _row_n = 0
      _current_cursor_y_cache = max(_current_cursor_y_cache, _row_bottom)
   }
   _same_line_next = false
   _last_item = rect
   rect
}

fn begin_columns(any spec, f64 gap=-1.0) int {
   "Splits the body into columns and enters the first. `spec` is a column count or a list of weights. Returns the column count."
   if _current_window_id_cache.len <= 0 { return 0 }
   mut weights = []
   if is_list(spec) { weights = spec }
   else {
      mut k = 0
      def cnt = max(1, int(spec))
      while k < cnt {
         weights = weights.append(1.0)
         k += 1
      }
   }
   def n = weights.len
   if n <= 0 { return 0 }
   def g = gap >= 0.0 ? gap : _pad()
   def cols = split_cols([_current_body_x_cache, 0.0, _current_body_w_cache, 1.0], weights, g)
   mut xs = []
   mut i = 0
   while i < n {
      def any c = cols.get(i, 0)
      xs = xs.append(float(c.get(0, 0.0)))
      xs = xs.append(max(1.0, float(c.get(2, 1.0))))
      i += 1
   }
   ;; [x/w pairs, count, index, base_x, base_w, band top, band bottom]
   _col_stack = _col_stack.append([xs, n, 0, _current_body_x_cache, _current_body_w_cache, _current_cursor_y_cache, _current_cursor_y_cache])
   _current_body_x_cache = float(xs.get(0, 0.0))
   _current_body_w_cache = float(xs.get(1, 1.0))
   _same_line_next = false
   n
}

fn next_column() bool {
   "Moves to the next column; after the last one it wraps to the first, below the tallest column so far."
   def depth = _col_stack.len
   if depth <= 0 { return false }
   def any c = _col_stack.get(depth - 1, 0)
   def any xs = c.get(0, 0)
   def n = int(c.get(1, 1))
   mut idx = int(c.get(2, 0)) + 1
   mut top = float(c.get(5, 0.0))
   mut bottom = max(float(c.get(6, 0.0)), _current_cursor_y_cache)
   if idx >= n {
      idx = 0
      top = bottom
   }
   _col_stack = _list_drop_last(_col_stack).append([xs, n, idx, float(c.get(3, 0.0)), float(c.get(4, 1.0)), top, bottom])
   def int cx_i, cw_i = idx * 2, idx * 2 + 1
   _current_body_x_cache = float(xs.get(cx_i, 0.0))
   _current_body_w_cache = float(xs.get(cw_i, 1.0))
   _current_cursor_y_cache = top
   _same_line_next = false
   true
}

fn end_columns() any {
   "Leaves the column layout; the cursor continues below the tallest column."
   def depth = _col_stack.len
   if depth <= 0 { return 0 }
   def any c = _col_stack.get(depth - 1, 0)
   _col_stack = _list_drop_last(_col_stack)
   _current_body_x_cache = float(c.get(3, 0.0))
   _current_body_w_cache = float(c.get(4, 1.0))
   _current_cursor_y_cache = max(float(c.get(6, 0.0)), _current_cursor_y_cache)
   _same_line_next = false
}

;; ---- disabled scope and item status ---------------------------------------
fn _disabled_restore() any {
   _text_u32, _text_dim_u32, _accent_u32 = _dis_text_u32, _dis_dim_u32, _dis_accent_u32
   _disabled_on = 0
}

fn begin_disabled(bool disabled=true) any {
   "Starts a scope whose widgets draw faded and ignore pointer, keyboard, and focus when `disabled` is true. Scopes nest."
   _disabled_depth += 1
   if !disabled {
      return 0
   }
   if _disabled_on == 0 {
      _theme_refresh()
      _dis_text_u32, _dis_dim_u32, _dis_accent_u32 = _text_u32, _text_dim_u32, _accent_u32
      _text_u32, _text_dim_u32, _accent_u32 = _text_faint_u32, _text_faint_u32, _text_faint_u32
      _disabled_on = _disabled_depth
   }
}

fn end_disabled() any {
   "Ends the innermost begin_disabled scope."
   if _disabled_depth <= 0 { return 0 }
   if _disabled_on == _disabled_depth { _disabled_restore() }
   _disabled_depth -= 1
}

fn item_status(str kind, any message="") bool {
   "Marks the previous item as \"error\", \"warn\", or \"ok\": frames it in the matching token and shows `message` as its tooltip. Returns true for a known kind."
   mut col = 0
   if kind == "error" || kind == "danger" { col = _danger_u32 }
   elif kind == "warn" || kind == "warning" { col = _warn_u32 }
   elif kind == "ok" || kind == "success" { col = _ok_u32 }
   else { return false }
   def x, y = float(_last_item.get(0, 0.0)), float(_last_item.get(1, 0.0))
   def w, h = float(_last_item.get(2, 0.0)), float(_last_item.get(3, 0.0))
   if _rect_intersects(_last_item, _current_clip()) {
      _gui_rect_outline_fast(x, y, w, h, col)
      _gui_rect_fast(x, y + h - _sx(2.0), w, _sx(2.0), col)
   }
   def msg = _ui_text(message)
   if msg.len > 0 { tooltip(msg) }
   true
}

fn _panel_state_split(int fid, str txt, f64 max_w) list {
   ;; Breaks a detail message onto two lines at the last space that fits, so a
   ;; narrow panel shows the whole reason instead of an ellipsis.
   if float(_measure_text_ui(fid, txt).get(0, 0.0)) <= max_w { return [txt, ""] }
   def words = txt.split(" ")
   mut head = ""
   mut k = 0
   while k < words.len {
      def cand = head.len > 0 ? (head + " " + to_str(words[k])) : to_str(words[k])
      if head.len > 0 && float(_measure_text_ui(fid, cand).get(0, 0.0)) > max_w { break }
      head = cand
      k += 1
   }
   mut tail = ""
   while k < words.len {
      tail = tail.len > 0 ? (tail + " " + to_str(words[k])) : to_str(words[k])
      k += 1
   }
   [head, tail]
}

fn _panel_state_text(int fid, str txt, f64 x, f64 y, f64 w, any color, list clip) any {
   ;; Centred on `w` and clipped to the whole panel: a clip one line high cut
   ;; the ascenders of the title.
   def tw = float(_measure_text_ui(fid, txt).get(0, 0.0))
   _draw_text_ui_ex_clipped(fid, txt, _snap(x + max(0.0, (w - tw) * 0.5)), _snap(y), color, 0.0, max(1.0, w), clip)
}

fn panel_state(str kind, any title, any detail="", f64 h=0.0) list {
   "Fills the rest of a panel(or `h` pixels) with a centred state message and returns its rectangle. `kind` is \"empty\", \"loading\", or \"error\": empty is quiet, loading shows a moving bar, error uses the danger token. A detail wider than the panel continues on a second line."
   def small_id = _font_small_id()
   def line = _text_h()
   def small = _font_h_for(small_id)
   def detail_s = _ui_text(detail)
   def has_detail = detail_s.len > 0
   def body_w = _current_body_w()
   mut str detail_a = ""
   mut str detail_b = ""
   if has_detail {
      def list parts = _panel_state_split(small_id, detail_s, max(1.0, body_w - _sx(8.0)))
      detail_a = to_str(parts.get(0, ""))
      detail_b = to_str(parts.get(1, ""))
   }
   mut f64 detail_lines = 0.0
   if has_detail { detail_lines = 1.0 }
   if detail_b.len > 0 { detail_lines = 2.0 }
   def block = _sx(10.0) + line + detail_lines * (small + _sx(4.0))
   def want = h > 0.0 ? h : max(_sx(72.0), remaining_h(0.0))
   ;; A requested height is kept; only a wrapped detail may grow the row.
   def hh = detail_b.len > 0 ? max(want, block + _sx(12.0)) : want
   def rect = _layout_rect(body_w, hh)
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   if !_rect_intersects(rect, _current_clip()) { return rect }
   def top = _snap(y + max(0.0, (hh - block) * 0.5))
   mut tone = _text_dim_u32
   if kind == "error" { tone = _danger_u32 }
   elif kind == "loading" { tone = _text_u32 }
   ;; A short rule above the text carries the state: still for empty and
   ;; error, sweeping for loading.
   def bar_w = min(_sx(96.0), w * 0.5)
   def bar_x = _snap(x + (w - bar_w) * 0.5)
   _gui_rect_fast(bar_x, top, bar_w, _sx(2.0), kind == "error" ? _danger_u32 : _border_u32)
   if kind == "loading" {
      def phase = float(_gui_frame_id % 90) / 90.0
      def seg = bar_w * 0.3
      _gui_rect_fast(_snap(bar_x + (bar_w - seg) * phase), top, seg, _sx(2.0), _accent_u32)
   }
   def text_y = top + _sx(10.0)
   _panel_state_text(_font_body(), _ui_text(title), x, text_y, w, tone, rect)
   if has_detail {
      _panel_state_text(small_id, detail_a, x, text_y + line + _sx(4.0), w, _text_faint_u32, rect)
      if detail_b.len > 0 { _panel_state_text(small_id, detail_b, x, text_y + line + small + _sx(8.0), w, _text_faint_u32, rect) }
   }
   rect
}

;; ---- tooltips -------------------------------------------------------------
fn set_tooltip_delay(int frames) any {
   "Sets how many consecutive hovered frames pass before a tooltip appears."
   ;; Not `max(1, frames)`: on a typed int that returned the tagged word.
   _tip_delay = frames
   if _tip_delay < 1 { _tip_delay = 1 }
}

fn tooltip(any text) bool {
   "Shows `text` near the pointer once the previous item has been hovered for the tooltip delay. Returns true while the tooltip is shown."
   if !_enabled || _mouse_down0 || _dnd_active { return false }
   def x, y = float(_last_item.get(0, 0.0)), float(_last_item.get(1, 0.0))
   def w, h = float(_last_item.get(2, 0.0)), float(_last_item.get(3, 0.0))
   if !_current_window_pointer_hit(x, y, w, h) { return false }
   def clip = _current_clip()
   if !_rect_hit(_mouse_x, _mouse_y, float(clip.get(0, 0.0)), float(clip.get(1, 0.0)), float(clip.get(2, 0.0)), float(clip.get(3, 0.0))) { return false }
   if _tip_x == x && _tip_y == y && _tip_seen >= _gui_frame_id - 1 {
      if _tip_seen != _gui_frame_id { _tip_frames += 1 }
   } else {
      _tip_x, _tip_y = x, y
      _tip_frames = 1
   }
   _tip_seen = _gui_frame_id
   if _tip_frames < _tip_delay { return false }
   _tip_text = _ui_text(text)
   true
}

;; ---- popups ---------------------------------------------------------------
fn _popup_key(any id) str {
   ;; A popup is a window of its own, so its key must not contain the "::"
   ;; that separates a window id from a widget id.
   def full = _wid(id)
   def sep = str.find(full, "::")
   if sep < 0 { return "~" + full }
   "~" + str.str_slice(full, 0, sep) + "~" + str.str_slice(full, sep + 2, full.len)
}

fn _popup_index(str pid) int { _list_find_text(_popup_stack, pid) }

fn _popup_close_from(int idx) any {
   "Closes the popup at stack position `idx` and everything opened from it."
   def n = _popup_stack.len
   if idx < 0 || idx >= n { return 0 }
   mut keep = []
   mut i = 0
   while i < n {
      def pid = to_str(_popup_stack.get(i, ""))
      if i < idx { keep = keep.append(pid) }
      else {
         mut st = _windows.get(pid, 0)
         if is_dict(st) {
            st["open"] = false
            st["w"] = 0.0
            st["h"] = 0.0
            _windows[pid] = st
         }
         if _focused_window == pid { _focused_window = "" }
         if _full_id_window_id(_kbd_focus_id) == pid { _kbd_focus_id = "" }
         if _full_id_window_id(_text_focus_id) == pid { _text_focus_id = "" }
         if _full_id_window_id(_active_widget_id) == pid { _active_widget_id = "" }
         if _menubar_open == pid { _menubar_open = "" }
         _popup_dismissed = _popup_dismissed.append(pid)
      }
      i += 1
   }
   _popup_stack = keep
}

fn _popup_dismiss_outside() any {
   ;; A press closes every popup above the one it lands in; a press outside
   ;; all of them closes the whole stack and then reaches what is underneath.
   mut i = _popup_stack.len - 1
   while i >= 0 {
      def pst = _windows.get(to_str(_popup_stack.get(i, "")), 0)
      if is_dict(pst) {
         if _rect_hit(_mouse_x, _mouse_y, float(pst.get("x", 0.0)), float(pst.get("y", 0.0)), float(pst.get("w", 0.0)), float(pst.get("h", 0.0))) {
            _popup_close_from(i + 1)
            return 0
         }
      }
      i -= 1
   }
   _popup_close_from(0)
}

fn _popup_open_at(str pid, f64 x, f64 y, int parent_depth) any {
   if _popup_index(pid) >= 0 { return 0 }
   _popup_close_from(parent_depth)
   mut st = dict(24)
   st["id"] = pid
   st["popup"] = true
   st["open"] = true
   st["ax"] = x
   st["ay"] = y
   st["x"] = x
   st["y"] = y
   st["w"] = 0.0
   st["h"] = 0.0
   st["titlebar"] = false
   st["scroll_y"] = 0.0
   st["content_h"] = -1.0
   st["want_w"] = 0.0
   _windows[pid] = st
   _popup_stack = _popup_stack.append(pid)
   _focused_window = pid
   _kbd_focus_id, _text_focus_id = "", ""
}

fn _ctx_push() any {
   mut st = _current_window_state()
   if is_dict(st) {
      st["cursor_y"] = _current_cursor_y_cache
      _current_window = st
   }
   _ctx_stack = _ctx_stack.append([st, _widget_id_prefix, _same_line_next, _same_line_gap, _last_item,
         _focus_collect_active, _current_body_x_cache, _current_body_w_cache, _current_cursor_y_cache,
         _menu_measure_w, _menubar_active, _menubar_x])
}

fn _ctx_pop() any {
   def n = _ctx_stack.len
   if n <= 0 { return 0 }
   def any c = _ctx_stack.get(n - 1, 0)
   _ctx_stack = _list_drop_last(_ctx_stack)
   def any st = c.get(0, 0)
   if is_dict(st) { _current_window = st }
   else { _current_window = 0 }
   _sync_current_layout_cache(st)
   _widget_id_prefix = to_str(c.get(1, ""))
   _same_line_next = bool(c.get(2, false))
   _same_line_gap = float(c.get(3, 0.0))
   _last_item = c.get(4, [0.0, 0.0, 0.0, 0.0])
   _focus_collect_active = bool(c.get(5, false))
   _current_body_x_cache = float(c.get(6, 0.0))
   _current_body_w_cache = float(c.get(7, 1.0))
   _current_cursor_y_cache = float(c.get(8, 0.0))
   _menu_measure_w = float(c.get(9, 0.0))
   _menubar_active = bool(c.get(10, false))
   _menubar_x = float(c.get(11, 0.0))
}

fn _popup_begin(str pid, f64 want_w, f64 pad) bool {
   mut st = _windows.get(pid, 0)
   if !is_dict(st) { return false }
   if !st.get("open", false) { return false }
   if _popup_index(pid) < 0 { return false }
   ;; The first frame only measures: nothing is drawn and the popup has no
   ;; hit area until its content height is known.
   def measuring = float(st.get("content_h", -1.0)) < 0.0
   mut w = want_w
   if w <= 0.0 { w = max(_sx(140.0), float(st.get("want_w", 0.0))) }
   mut h = measuring ? 0.0 : (float(st.get("content_h", 0.0)) + pad * 2.0)
   if _fb_h > 0.0 { h = min(h, max(1.0, _fb_h - _sx(8.0))) }
   mut x, y = float(st.get("ax", 0.0)), float(st.get("ay", 0.0))
   if _fb_w > 0.0 { x = clamp(x, 0.0, max(0.0, _fb_w - w)) }
   if _fb_h > 0.0 && y + h > _fb_h { y = max(0.0, _fb_h - h) }
   x, y = _snap(x), _snap(y)
   st["x"] = x
   st["y"] = y
   st["w"] = measuring ? 0.0 : w
   st["h"] = h
   st["measuring"] = measuring
   ;; Arrow keys walk the innermost popup's rows, using the focus order the
   ;; previous frame collected (it is rebuilt below).
   if _popup_index(pid) == _popup_stack.len - 1 {
      if _key_event_present(KEY_DOWN) {
         _focused_window = pid
         _request_focus_step(1)
      } elif _key_event_present(KEY_UP) {
         _focused_window = pid
         _request_focus_step(-1)
      }
   }
   _ctx_push()
   _popup_depth += 1
   _ov_depth += 1
   ;; Each popup records into its own command list. A submenu is begun while
   ;; its parent's clip is still pushed, so its commands are replayed after
   ;; the parent's, not inside them.
   _ov_stack = _ov_stack.append(_ov_cmds)
   _ov_cmds = []
   if measuring { _ov_discard += 1 }
   _gui_rect_fast(x + _sx(3.0), y + _sx(4.0), w, h, _shadow_u32)
   _gui_rect_fast(x, y, w, h, _popup_u32)
   _gui_rect_outline_fast(x, y, w, h, _border_u32)
   def clip = [x + 1.0, y + 1.0, max(1.0, w - 2.0), measuring ? 100000.0 : max(1.0, h - 2.0)]
   st["body_x"] = x + pad
   st["body_y"] = y + pad
   st["body_w"] = max(1.0, w - pad * 2.0)
   st["body_h"] = max(1.0, h - pad * 2.0)
   st["cursor_y"] = y + pad
   st["clip_override"] = clip
   st["focusables"] = []
   st["focusable_map"] = dict(32)
   _windows[pid] = st
   _current_window = st
   _sync_current_layout_cache(st)
   _focus_collect_active = true
   _same_line_next, _same_line_gap = false, _gap()
   _last_item = [x + pad, y + pad, 0.0, 0.0]
   _menu_measure_w = 0.0
   _menubar_active = false
   push_clip_rect(clip)
   true
}

fn _popup_end() any {
   mut st = _current_window_state()
   if !is_dict(st) { return 0 }
   if !st.get("popup", false) { return 0 }
   pop_clip_rect()
   st["content_h"] = max(1.0, _current_cursor_y_cache - float(st.get("body_y", 0.0)))
   if _menu_measure_w > 0.0 { st["want_w"] = _menu_measure_w }
   def measuring = bool(st.get("measuring", false))
   _windows[to_str(st.get("id", ""))] = st
   _ov_depth -= 1
   if measuring { _ov_discard -= 1 }
   _popup_depth -= 1
   mut pending = [_ov_cmds]
   mut pi = 0
   while pi < _ov_pending.len {
      pending = pending.append(_ov_pending.get(pi, 0))
      pi += 1
   }
   def sn = _ov_stack.len
   def any parent = sn > 0 ? _ov_stack.get(sn - 1, 0) : 0
   _ov_stack = _list_drop_last(_ov_stack)
   mut restored = []
   if is_list(parent) { restored = parent }
   if _popup_depth <= 0 {
      ;; Popups draw above every window layer, whatever they were opened from.
      pi = 0
      while pi < pending.len {
         def any seg = pending.get(pi, 0)
         if is_list(seg) {
            mut si = 0
            def int seg_n = seg.len
            while si < seg_n {
               _ov_top = _ov_top.append(seg.get(si, 0))
               si += 1
            }
         }
         pi += 1
      }
      pending = []
   }
   _ov_cmds = restored
   _ov_pending = pending
   _ctx_pop()
}

fn open_popup(any id, any x=nil, any y=nil) bool {
   "Opens the popup `id` at the pointer, or at `(x, y)`. Call it from the scope that will call begin_popup with the same id."
   def px = x == nil ? _mouse_x : float(x)
   def py = y == nil ? _mouse_y : float(y)
   _popup_open_at(_popup_key(id), px, py, _popup_depth)
   true
}

fn popup_open(any id) bool { "Returns true while the popup `id` (in the current id scope) is open." _popup_index(_popup_key(id)) >= 0 }

fn any_popup_open() bool { "Returns true while any popup or menu is open." _popup_stack.len > 0 }

fn close_popup() any {
   "Closes the popup being built, or every popup when called outside one."
   if _popup_depth > 0 { _popup_close_from(_popup_depth - 1) }
   else { _popup_close_from(0) }
}

fn close_all_popups() any { "Closes every open popup and menu." _popup_close_from(0) }

fn begin_popup(any id, f64 w=0.0) bool {
   "Begins the popup `id` when it is open; pair with end_popup only when this returns true. The height follows the content."
   if !_enabled { return false }
   _popup_begin(_popup_key(id), w > 0.0 ? w : _sx(220.0), _pad())
}

fn end_popup() any { "Ends the popup started by begin_popup." _popup_end() }

;; ---- stock dialogs --------------------------------------------------------
fn dialog_open(any id, f64 w=340.0) bool {
   "Opens the stock dialog `id`; draw it with message_dialog or text_input_dialog."
   _ensure_gui_state()
   mut d = _dialogs.get(to_str(id), 0)
   if !is_dict(d) { d = dict(4) }
   d["open"] = true
   d["w"] = w
   _dialogs[to_str(id)] = d
   true
}

fn dialog_close(any id) any {
   "Closes the stock dialog `id` without a choice."
   _ensure_gui_state()
   _dialogs = _dialogs.delete(to_str(id))
   0
}

fn dialog_is_open(any id) bool {
   "Returns true while the stock dialog `id` is open."
   _ensure_gui_state()
   def d = _dialogs.get(to_str(id), nil)
   is_dict(d) && bool(d.get("open", false))
}

fn _dialog_begin(str key, any title, f64 w, f64 h) bool {
   def d = _dialogs.get(key, nil)
   if !is_dict(d) || !bool(d.get("open", false)) { return false }
   def dw = min(max(220.0, w), max(220.0, _gui_view_w - 32.0))
   def dh = max(1.0, h)
   def x = max(16.0, (_gui_view_w - dw) * 0.5)
   def y = max(16.0, (_gui_view_h - dh) * 0.5)
   begin_window("stockdlg:" + key, title, x, y, dw, dh, {"closable": false, "movable": false, "resizable": false, "collapsible": false})
}

fn _dialog_buttons(list labels) int {
   if labels.len <= 0 { return -1 }
   def gap = _sx(8.0)
   def bw = max(64.0, (_current_body_w() - gap * float(labels.len - 1)) / float(labels.len))
   mut widths = []
   mut i = 0
   while i < labels.len { widths = widths.append(bw) i += 1 }
   layout_row(widths, gap)
   mut chosen = -1
   i = 0
   while i < labels.len {
      push_id("dlgbtn" + to_str(i))
      if button("b", labels.get(i), bw, _sx(26.0)) { chosen = i }
      pop_id()
      i += 1
   }
   chosen
}

fn message_dialog(any id, any title, any text, any buttons=0, f64 w=340.0) int {
   "Draws an open stock message dialog; returns the clicked button index, or -1 while it is still open."
   def key = to_str(id)
   def d = _dialogs.get(key, nil)
   if !is_dict(d) || !bool(d.get("open", false)) { return -1 }
   if !_dialog_begin(key, title, w, 168.0) { return -1 }
   text_colored(text, "text")
   spacing(1.0)
   def labels = is_list(buttons) && buttons.len > 0 ? buttons : ["OK"]
   def chosen = _dialog_buttons(labels)
   end_window()
   if chosen >= 0 { _dialogs = _dialogs.delete(key) }
   chosen
}

fn text_input_dialog(any id, any title, any caption, any value, any buttons=0, f64 w=340.0) any {
   "Draws an open stock single-field dialog. Returns {done, ok, text}; the edited text persists across frames until a button is chosen."
   def key = to_str(id)
   def initial = _ui_text(value)
   def d = _dialogs.get(key, nil)
   if !is_dict(d) || !bool(d.get("open", false)) { return {"done": false, "ok": false, "text": initial} }
   mut st = d
   if !st.contains("text") { st["text"] = initial }
   if !_dialog_begin(key, title, w, 176.0) { return {"done": false, "ok": false, "text": to_str(st.get("text", initial))} }
   def edited = input_text("stockdlg_field", caption, to_str(st.get("text", initial)), "", 0.0)
   st["text"] = edited
   _dialogs[key] = st
   spacing(1.0)
   def labels = is_list(buttons) && buttons.len > 0 ? buttons : ["OK", "Cancel"]
   def chosen = _dialog_buttons(labels)
   end_window()
   def ok = chosen == 0
   if chosen >= 0 { _dialogs = _dialogs.delete(key) }
   {"done": chosen >= 0, "ok": ok, "text": edited}
}

;; ---- menus ----------------------------------------------------------------
fn begin_menu_bar() bool {
   "Reserves a menu bar row in the current window; follow with begin_menu calls and end_menu_bar."
   if !_enabled || _current_window_id_cache.len <= 0 { return false }
   def h = _small_item_h() + _sx(2.0)
   def rect = _layout_rect(_current_body_w(), h)
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   if _rect_intersects(rect, _current_clip()) {
      _gui_rect_fast(x, y, w, h, _window_hdr_u32)
      _gui_rect_fast(x, y + h - 1.0, w, 1.0, _border_u32)
   }
   _menubar_active = true
   _menubar_x, _menubar_y, _menubar_h = x, y, h
   true
}

fn end_menu_bar() any {
   "Ends the menu bar row."
   _menubar_active = false
   _last_item = [_current_body_x_cache, _menubar_y, _current_body_w_cache, _menubar_h]
}

fn begin_menu(any id, any label, bool enabled=true, any style=nil) bool {
   "Draws a menu header or submenu row with optional state styles and begins its popup when open; pair with end_menu only when this returns true."
   if !_enabled || _current_window_id_cache.len <= 0 { return false }
   def txt = _ui_text(label)
   def key = to_str(id) + "#menu"
   def full = _wid(key)
   def pid = _popup_key(key)
   def is_open = _popup_index(pid) >= 0
   def fid = _font_small_id()
   if _menubar_active {
      def w = _text_fit_width(fid, txt) + _sx(18.0)
      def x, y, h = _menubar_x, _menubar_y, _menubar_h
      _menubar_x = x + w
      if enabled { _register_focusable(full) }
      _begin_interact_widget(full, x, y, w, h, true)
      if !enabled { _iw_hovered = false }
      def pressed = _iw_hovered && _mouse_pressed0
      if enabled {
         def a11y_activate = _accessibility_take(full, "activate")
         if pressed || _widget_key_activate(full) || a11y_activate.get("found", false) {
            if is_open { _popup_close_from(_popup_index(pid)) }
            elif _list_find_text(_popup_dismissed, pid) < 0 {
               _popup_open_at(pid, x, y + h, 0)
               _menubar_open = pid
            }
         } elif _iw_hovered && !is_open && _menubar_open != "" && _popup_index(_menubar_open) >= 0 {
            _popup_open_at(pid, x, y + h, 0)
            _menubar_open = pid
         }
      }
      _accessibility_record(full, "button", txt, "", [x, y, w, h], false, false, _popup_index(pid) >= 0, !enabled, {"has_popup": true, "menu": true})
      if _iw_visible {
         def lit = _popup_index(pid) >= 0
         def state = !enabled ? "disabled" : (pressed || lit ? "pressed" : (_iw_hovered ? "hovered" : "active"))
         def default_bg = lit ? _panel_active_u32 : (_iw_hovered ? _panel_hover_u32 : 0)
         def colors = _button_style(style, state, default_bg, _border_u32, enabled ? _text_u32 : _text_faint_u32)
         def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
         if !is_nil(style) || lit || _iw_hovered { _gui_rect_fast(x, y, w, h - 1.0, bg) }
         def state_style = is_dict(style) ? style.get(state, nil) : nil
         if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(x, y, w, h, edge) }
         if _kbd_focus_id == full { _gui_focus_ring(x, y, w, h) }
         _draw_centered_text_ex(fid, txt, x, y, w, h, text_col)
      }
      _last_item = [x, y, w, h]
   } else {
      def h = _small_item_h() + _sx(2.0)
      def rect = _layout_rect(_current_body_w(), h)
      _current_cursor_y_cache = _current_cursor_y_cache - _gap()
      def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
      def w = float(rect.get(2, 0.0))
      _menu_measure_w = max(_menu_measure_w, _text_fit_width(fid, txt) + _sx(46.0) + _item_pad_x() * 2.0)
      if enabled { _register_focusable(full) }
      _begin_interact_widget(full, x, y, w, h, true)
      if !enabled { _iw_hovered = false }
      def focused = _kbd_focus_id == full
      if enabled && !is_open {
         def a11y_activate = _accessibility_take(full, "activate")
         if _iw_hovered || _widget_key_activate(full) || a11y_activate.get("found", false) || (focused && _key_event_present(KEY_RIGHT)) {
            _popup_open_at(pid, x + w - _sx(2.0), y - _sx(4.0), _popup_depth)
         }
      }
      _accessibility_record(full, "button", txt, "", rect, false, false, _popup_index(pid) >= 0, !enabled, {"has_popup": true, "menu": true})
      if _iw_visible {
         def lit = _popup_index(pid) >= 0
         def state = !enabled ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered || focused || lit ? "hovered" : "active"))
         def default_bg = lit || _iw_hovered || focused ? _panel_active_u32 : 0
         def colors = _button_style(style, state, default_bg, _border_u32, enabled ? _text_u32 : _text_faint_u32)
         def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
         if !is_nil(style) || lit || _iw_hovered || focused { _gui_rect_fast(x, y, w, h, bg) }
         def state_style = is_dict(style) ? style.get(state, nil) : nil
         if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(x, y, w, h, edge) }
         def ty = _text_center_y_for(fid, y, h)
         _draw_text_ui_small_clipped(txt, x + _sx(22.0), ty, text_col, max(1.0, w - _sx(40.0)), _clip_rect_inset(x, y, w, h, 0.0))
         _gui_chevron(x + w - _sx(10.0), y + h * 0.5, _sx(8.0), false, text_col)
      }
      _last_item = rect
   }
   _popup_begin(pid, 0.0, _sx(4.0))
}

fn end_menu() any { "Ends the menu started by begin_menu." _popup_end() }

fn menu_item(any id, any label, any shortcut="", bool checked=false, bool enabled=true, any style=nil) bool {
   "Draws a menu row with an optional shortcut hint and check mark. Optional state styles use the button style-map contract. Returns true when chosen."
   def txt, sc = _ui_text(label), _ui_text(shortcut)
   def fid = _font_small_id()
   def h = _small_item_h() + _sx(2.0)
   def rect = _layout_rect(_current_body_w(), h)
   _last_item = rect
   _current_cursor_y_cache = _current_cursor_y_cache - _gap()
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def full = _wid(id)
   def sw = sc.len > 0 ? (_text_fit_width(fid, sc) + _sx(24.0)) : 0.0
   _menu_measure_w = max(_menu_measure_w, _text_fit_width(fid, txt) + sw + _sx(30.0) + _item_pad_x() * 2.0)
   mut clicked = false
   if enabled { _register_focusable(full) }
   _begin_interact_widget(full, x, y, w, h, true)
   if enabled {
      def a11y_activate = _accessibility_take(full, "activate")
      clicked = _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false)
      if _iw_hovered && _popup_depth > 0 { _popup_close_from(_popup_depth) }
   } else {
      _iw_hovered, _iw_held = false, false
   }
   _accessibility_record(full, "menuitem", txt, checked, rect, checked, false, false, !enabled, {"shortcut": sc})
   if _iw_visible {
      def focused = _kbd_focus_id == full
      def state = !enabled ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered || focused ? "hovered" : "active"))
      def default_bg = _iw_held ? _press_u32 : ((_iw_hovered || focused) ? _panel_active_u32 : 0)
      def default_text = enabled ? _text_u32 : _text_faint_u32
      def colors = _button_style(style, state, default_bg, _border_u32, default_text)
      def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      if !is_nil(style) || _iw_held || _iw_hovered || focused { _gui_rect_fast(x, y, w, h, bg) }
      def state_style = is_dict(style) ? style.get(state, nil) : nil
      if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(x, y, w, h, edge) }
      def ty = _text_center_y_for(fid, y, h)
      def row_clip = _clip_rect_inset(x, y, w, h, 0.0)
      if checked { _gui_check_mark(x + _sx(4.0), y + (h - _sx(14.0)) * 0.5, _sx(14.0), enabled ? _accent_u32 : _text_faint_u32) }
      _draw_text_ui_small_clipped(txt, x + _sx(22.0), ty, text_col, max(1.0, w - sw - _sx(26.0)), row_clip)
      if sc.len > 0 { _draw_text_ui_small_clipped(sc, x + w - sw + _sx(12.0), ty, _text_faint_u32, sw, row_clip) }
   }
   if clicked { _popup_close_from(0) }
   clicked
}

fn menu_separator() any {
   "Draws a divider between menu rows."
   def rect = _layout_rect(_current_body_w(), _sx(7.0))
   _current_cursor_y_cache = _current_cursor_y_cache - _gap()
   _gui_rect_fast(float(rect.get(0, 0.0)) + _sx(6.0), float(rect.get(1, 0.0)) + _sx(3.0), max(1.0, float(rect.get(2, 0.0)) - _sx(12.0)), 1.0, _border_u32)
}

fn begin_context_menu(any id) bool {
   "Opens a menu when the secondary button is pressed on the previous item, and begins it while open; pair with end_menu only when this returns true."
   if !_enabled || _current_window_id_cache.len <= 0 { return false }
   def key = to_str(id) + "#ctx"
   def pid = _popup_key(key)
   if _mouse_pressed1 && _disabled_on == 0 {
      def x, y = float(_last_item.get(0, 0.0)), float(_last_item.get(1, 0.0))
      if _current_window_pointer_hit(x, y, float(_last_item.get(2, 0.0)), float(_last_item.get(3, 0.0))) {
         _popup_close_from(_popup_depth)
         _popup_open_at(pid, _mouse_x, _mouse_y, _popup_depth)
      }
   }
   _popup_begin(pid, 0.0, _sx(4.0))
}

;; ---- trees ----------------------------------------------------------------
fn tree_node(any id, any label, bool default_open=false, bool leaf=false, bool selected=false, any style=nil) bool {
   "Draws a tree row. Returns true when it is open and has children: draw them, then call tree_pop. Left and Right collapse and expand a focused row. Optional state styles follow the button style-map contract."
   def full = _wid(id) + "#tree"
   _register_focusable(full)
   mut open = leaf ? false : _state_get_bool(full, default_open)
   def h = _small_item_h()
   def rect = _layout_rect(_current_body_w(), h)
   _current_cursor_y_cache = _current_cursor_y_cache - _gap()
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   _last_item = rect
   _begin_interact_widget(full, x, y, w, h, true)
   def focused = _kbd_focus_id == full
   if !leaf {
      mut want = open
      def a11y_activate = _accessibility_take(full, "activate")
      if _iw_clicked || _widget_key_activate(full) || a11y_activate.get("found", false) { want = !open }
      if focused && _key_event_present(KEY_RIGHT) { want = true }
      if focused && _key_event_present(KEY_LEFT) { want = false }
      if want != open {
         open = want
         _state_set_bool(full, open)
      }
   }
   _accessibility_record(full, "treeitem", _ui_text(label), nil, rect, false, selected, open, false, {"leaf": leaf, "level": _indent_stack.len})
   if _iw_visible {
      def state = _disabled_on > 0 ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered || focused ? "hovered" : "active"))
      def default_bg = selected ? _accent_soft_u32 : (_iw_held ? _panel_active_u32 : (_iw_hovered ? _panel_hover_u32 : 0))
      def colors = _button_style(style, state, default_bg, _border_u32, _disabled_on > 0 ? _text_faint_u32 : _text_u32)
      def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      if !is_nil(style) || selected || _iw_held || _iw_hovered { _gui_rect_fast(x, y, w, h, bg) }
      def state_style = is_dict(style) ? style.get(state, nil) : nil
      if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(x, y, w, h, edge) }
      if focused { _gui_focus_ring(x, y, w, h) }
      def mark = _iw_hovered ? text_col : _text_dim_u32
      if leaf { _gui_rect_fast(x + _sx(6.0), y + h * 0.5 - _sx(1.0), _sx(3.0), _sx(3.0), _text_faint_u32) }
      else { _gui_chevron(x + _sx(8.0), y + h * 0.5, _sx(8.0), open, mark) }
      _draw_text_ui_small_clipped(_ui_text(label), x + _sx(18.0), _text_center_y_for(_font_small_id(), y, h), text_col, max(1.0, w - _sx(20.0)), _clip_rect_inset(x, y, w, h, 0.0))
   }
   if open { indent(_sx(14.0)) }
   open
}

fn tree_pop() any { "Closes the children of an open tree_node." unindent() }

;; ---- numeric drag fields --------------------------------------------------
fn _fmt_fixed(f64 v, int decimals) str {
   mut d = decimals
   if d < 0 { d = 0 }
   if d > 9 { d = 9 }
   mut scale = 1.0
   mut i = 0
   while i < d {
      scale = scale * 10.0
      i += 1
   }
   def neg = v < 0.0
   def a = neg ? (0.0 - v) : v
   def scaled = int(a * scale + 0.5)
   def ip = scaled / int(scale)
   def fp = scaled - ip * int(scale)
   mut out = to_str(ip)
   if d > 0 {
      mut frac = to_str(fp)
      while frac.len < d { frac = "0" + frac }
      out = out + "." + frac
   }
   (neg && scaled > 0) ? ("-" + out) : out
}

fn drag_float(any id, any label, f64 value, f64 speed=0.01, f64 min_v=0.0, f64 max_v=0.0, int decimals=3, f64 w=0.0, any style=nil) f64 {
   "Draws a numeric field edited by dragging horizontally or with the arrow keys; optional state styles control field, fill, border and text colors."
   def lbl = _ui_text(label)
   def rect = _layout_rect(_pick_positive(w, _current_body_w()), _labelled_h(lbl))
   _drag_float_at(id, lbl, value, speed, min_v, max_v, decimals, rect, style)
}

fn _drag_float_at(any id, str lbl, f64 value, f64 speed, f64 min_v, f64 max_v, int decimals, list rect, any style) f64 {
   mut out = value
   def bounded = max_v > min_v
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def rwf = float(rect.get(2, 0.0))
   def cap_h = lbl.len > 0 ? _label_h() : 0.0
   def fy, fh = y + cap_h, float(rect.get(3, 0.0)) - cap_h
   if fh <= 0.0 { return out }
   def full = _wid(id)
   _register_focusable(full)
   _begin_interact_widget(full, x, fy, rwf, fh, true)
   def set_action = _accessibility_take(full, "set_value")
   if set_action.get("found", false) { out = float(set_action.get("value", out)) }
   if _accessibility_take(full, "increment").get("found", false) { out = out + speed }
   if _accessibility_take(full, "decrement").get("found", false) { out = out - speed }
   mut wst = _widget_state.get(full, 0)
   if !is_dict(wst) { wst = dict(8) }
   mut editing = float(wst.get("edit", 0.0)) > 0.5
   mut buf = to_str(wst.get("buf", ""))
   if editing {
      if _text_focus_id != full {
         editing = false
      } else {
         def enter = _key_event_present(KEY_ENTER)
         def edit = _text_edit_apply_input(full, buf, _text_state_cursor(full, buf.len))
         buf = to_str(edit.get(0, buf))
         _text_state_set_cursor(full, int(edit.get(1, buf.len)))
         if enter {
            out = str.atof(buf)
            editing = false
            _kbd_focus_id = full
         }
      }
   } else {
      mut start = false
      if _iw_hovered && _mouse_pressed0 {
         if float(_gui_frame_id) - float(wst.get("press_frame", -1000.0)) <= 24.0 { start = true }
         wst["press_frame"] = float(_gui_frame_id)
         wst["drag_x"] = _mouse_x
         wst["drag_v"] = out
      }
      if _iw_held && _mouse_drag_ready() && !start {
         out = float(wst.get("drag_v", out)) + (_mouse_x - float(wst.get("drag_x", _mouse_x))) * speed
      }
      if _kbd_focus_id == full && _disabled_on == 0 {
         if _key_event_present(KEY_LEFT) || _key_event_present(KEY_DOWN) { out = out - speed }
         if _key_event_present(KEY_RIGHT) || _key_event_present(KEY_UP) { out = out + speed }
         if _key_event_present(KEY_ENTER) { start = true }
      }
      if start {
         editing = true
         buf = _fmt_fixed(out, decimals)
         _text_focus_id, _kbd_focus_id = full, full
         _active_widget_id = ""
         _text_state_set_cursor(full, buf.len)
         _text_set_selected_all(full, true)
         wst = _widget_state.get(full, wst)
      }
   }
   if bounded { out = clamp(out, min_v, max_v) }
   wst["edit"] = editing ? 1.0 : 0.0
   wst["buf"] = buf
   _widget_state[full] = wst
   if _iw_visible {
      def focused = _kbd_focus_id == full
      def state = _disabled_on > 0 ? "disabled" : (editing ? "pressed" : (_iw_hovered ? "hovered" : "active"))
      def default_bg = editing ? _window_bg_u32 : (_iw_hovered ? _panel_hover_u32 : _panel_u32)
      def default_edge = editing ? _accent_u32 : (_iw_hovered ? _text_faint_u32 : _border_u32)
      def colors = _button_style(style, state, default_bg, default_edge, _text_u32)
      def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
      def state_style = is_dict(style) ? style.get(state, nil) : nil
      def fill = is_dict(state_style) && is_list(state_style.get("fill", nil)) ? int(colors.get(3, _accent_u32)) : _accent_u32
      _gui_rect_fast(x, fy, rwf, fh, bg)
      if bounded && !editing {
         def t = clamp((out - min_v) / (max_v - min_v), 0.0, 1.0)
         if t > 0.0 { _gui_rect_fast(x + 1.0, fy + fh - _sx(3.0), max(1.0, (rwf - 2.0) * t), _sx(2.0), fill) }
      }
      _gui_rect_outline_fast(x, fy, rwf, fh, edge)
      if focused && !editing { _gui_focus_ring(x, fy, rwf, fh) }
      if lbl.len > 0 { _draw_text_ui_small_clipped(lbl, x, y, is_nil(style) ? _text_dim_u32 : text_col, rwf, _clip_rect_inset(x, y, rwf, _label_h(), 0.0)) }
      def shown = editing ? buf : _fmt_fixed(out, decimals)
      if editing {
         def tx = x + _item_pad_x()
         def ty = _text_center_y_for(_font_body(), fy, fh)
         if _text_selected_all(full) && buf.len > 0 {
            def measured_buf = _measure_text_ui(_font_body(), buf)
            _gui_rect_fast(tx - 1.0, fy + _sx(4.0), min(rwf - _item_pad_x() * 2.0, float(measured_buf.get(0, 0.0)) + _sx(3.0)), max(_sx(8.0), fh - _sx(8.0)), _select_u32)
         }
         _draw_text_ui_clipped(shown, tx, ty, text_col, rwf - _item_pad_x() * 2.0, _clip_rect_inset(x, fy, rwf, fh, 1.0))
      } else {
         _draw_centered_text(shown, x, fy, rwf, fh, text_col)
      }
      if !editing && _iw_hovered {
         _ensure_cursors()
         _want_cursor(_cursor_resize)
      }
   }
   _last_item = [x, fy, rwf, fh]
   if editing {
      _accessibility_record(full, "spinbutton", lbl, buf, [x, fy, rwf, fh], false, false, false, _disabled_on > 0, {"min": bounded ? min_v : nil, "max": bounded ? max_v : nil, "decimals": decimals})
   } else {
      _accessibility_record(full, "spinbutton", lbl, out, [x, fy, rwf, fh], false, false, false, _disabled_on > 0, {"min": bounded ? min_v : nil, "max": bounded ? max_v : nil, "decimals": decimals})
   }
   out
}

fn drag_int(any id, any label, int value, f64 speed=0.25, int min_v=0, int max_v=0, f64 w=0.0, any style=nil) int {
   "Integer form of drag_float with the same optional state style contract."
   def full = _wid(id)
   mut acc = _state_get_num(full, "acc", float(value))
   if int(acc + (acc < 0.0 ? -0.5 : 0.5)) != value { acc = float(value) }
   acc = drag_float(id, label, acc, speed, float(min_v), float(max_v), 0, w, style)
   _state_set_num(full, "acc", acc)
   int(acc + (acc < 0.0 ? -0.5 : 0.5))
}

;; ---- tables ---------------------------------------------------------------
fn _table_arrow(f64 cx, f64 cy, bool down, int color) any {
   def n = int(max(2.0, _sx(3.0)))
   mut i = 0
   while i <= n {
      def half = float(down ? (n - i) : i)
      _gui_rect_fast(cx - half, cy - float(n) * 0.5 + float(i), half * 2.0 + 1.0, 1.0, color)
      i += 1
   }
}

fn begin_table(any id, list labels, any weights=0, any style=nil) bool {
   "Begins a table with a header row. Clicking a header sorts by it(read with table_sort_column / table_sort_descending); dragging a header edge resizes the two columns beside it. Fill rows with table_next_row and table_next_column or table_text, then call end_table. Optional state styles follow the button style-map contract."
   if !_enabled || _current_window_id_cache.len <= 0 || _tbl_id != "" { return false }
   def n = labels.len
   if n <= 0 { return false }
   def full = _wid(id) + "#table"
   mut st = _widget_state.get(full, 0)
   if !is_dict(st) { st = dict(8) }
   mut any fr = st.get("fr", 0)
   mut have = false
   if is_list(fr) { have = fr.len == n }
   if !have {
      mut total = 0.0
      mut k = 0
      while k < n {
         total += is_list(weights) ? max(0.01, float(weights.get(k, 1.0))) : 1.0
         k += 1
      }
      fr = []
      k = 0
      while k < n {
         fr = fr.append((is_list(weights) ? max(0.01, float(weights.get(k, 1.0))) : 1.0) / total)
         k += 1
      }
   }
   mut sort_col = int(float(st.get("sort", -1.0)))
   mut desc = float(st.get("desc", 0.0)) > 0.5
   def hh = _small_item_h() + _sx(2.0)
   def rect = _layout_rect(_current_body_w(), hh)
   _current_cursor_y_cache = _current_cursor_y_cache - _gap()
   def x, y = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def vis = _rect_intersects(rect, _current_clip())
   if vis && is_nil(style) { _gui_rect_fast(x, y, w, hh, _window_hdr_u32) }
   mut cols = []
   mut cx = x
   mut i = 0
   while i < n {
      def cw = (i == n - 1) ? max(1.0, x + w - cx) : max(1.0, _snap(w * float(fr.get(i, 0.1))))
      def hid = full + "#h" + to_str(i)
      _register_focusable(hid)
      _begin_interact_widget(hid, cx, y, cw, hh, true)
      def focused = _kbd_focus_id == hid
      if _iw_clicked || _widget_key_activate(hid) {
         if sort_col == i { desc = !desc }
         else {
            sort_col = i
            desc = false
         }
      }
      if vis {
         def state = _disabled_on > 0 ? "disabled" : (_iw_held ? "pressed" : (_iw_hovered || focused ? "hovered" : "active"))
         def default_bg = _iw_held ? _panel_active_u32 : (_iw_hovered ? _panel_hover_u32 : (is_nil(style) ? _window_hdr_u32 : 0))
         def default_fg = _disabled_on > 0 ? _text_faint_u32 : (sort_col == i ? _text_u32 : _text_dim_u32)
         def colors = _button_style(style, state, default_bg, _border_u32, default_fg)
         def bg, edge, text_col = int(colors.get(0)), int(colors.get(1)), int(colors.get(2))
         if !is_nil(style) || _iw_held || _iw_hovered { _gui_rect_fast(cx, y, cw, hh, bg) }
         def state_style = is_dict(style) ? style.get(state, nil) : nil
         if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(cx, y, cw, hh, edge) }
         if focused { _gui_focus_ring(cx, y, cw, hh) }
         _draw_text_ui_small_clipped(_ui_text(labels.get(i, "")), cx + _item_pad_x(), _text_center_y_for(_font_small_id(), y, hh), text_col, max(1.0, cw - _item_pad_x() * 2.0 - (sort_col == i ? _sx(10.0) : 0.0)), _clip_rect_inset(cx, y, cw, hh, 0.0))
         if sort_col == i { _table_arrow(cx + cw - _sx(9.0), y + hh * 0.5, desc, _accent_u32) }
      }
      cols = cols.append(cx)
      cols = cols.append(cw)
      cx = cx + cw
      i += 1
   }
   ;; Resize handles are submitted after the header cells, so a press on a
   ;; column edge belongs to the handle and never toggles the sort.
   mut nfr = fr
   i = 0
   while i < n - 1 {
      def int cx_i, cw_i = i * 2, i * 2 + 1
      def int nx_i = i + 1
      def bx = float(cols.get(cx_i, 0.0)) + float(cols.get(cw_i, 0.0))
      def rid = full + "#r" + to_str(i)
      _begin_interact_widget(rid, bx - _sx(3.0), y, _sx(6.0), hh, true)
      if _iw_hovered || _iw_held {
         _ensure_cursors()
         _want_cursor(_cursor_resize)
      }
      if _iw_held && w > 1.0 {
         def pair = float(fr.get(i, 0.1)) + float(fr.get(nx_i, 0.1))
         def left = clamp((_mouse_x - float(cols.get(cx_i, 0.0))) / w, 0.04, max(0.04, pair - 0.04))
         mut out = []
         mut k = 0
         while k < n {
            if k == i { out = out.append(left) }
            elif k == nx_i { out = out.append(pair - left) }
            else { out = out.append(float(fr.get(k, 0.1))) }
            k += 1
         }
         nfr = out
      }
      if vis { _gui_rect_fast(bx, y + _sx(3.0), 1.0, hh - _sx(6.0), (_iw_hovered || _iw_held) ? _accent_u32 : _border_u32) }
      i += 1
   }
   if vis { _gui_rect_fast(x, y + hh - 1.0, w, 1.0, _border_u32) }
   st["fr"] = nfr
   st["sort"] = float(sort_col)
   st["desc"] = desc ? 1.0 : 0.0
   _widget_state[full] = st
   _tbl_id = full
   _tbl_style = style
   _tbl_cols = cols
   _tbl_n, _tbl_col, _tbl_row = n, -1, -1
   _tbl_x, _tbl_w = x, w
   _tbl_base_x, _tbl_base_w = _current_body_x_cache, _current_body_w_cache
   _tbl_row_y, _tbl_row_bottom = _current_cursor_y_cache, _current_cursor_y_cache
   _tbl_row_h = _small_item_h()
   _tbl_row_hit = false
   _same_line_next = false
   true
}

fn table_next_row(bool selected=false, any style=nil) bool {
   "Starts the next table row and returns whether it is inside the visible clip, so long tables can skip hidden rows' content."
   if _tbl_id == "" { return false }
   _tbl_row_bottom = max(_tbl_row_bottom, _current_cursor_y_cache)
   _tbl_row += 1
   _tbl_col = -1
   _tbl_row_y = _tbl_row_bottom
   _tbl_row_bottom = _tbl_row_y + _tbl_row_h
   _current_cursor_y_cache = _tbl_row_y
   _current_body_x_cache, _current_body_w_cache = _tbl_base_x, _tbl_base_w
   def vis = _rect_intersects([_tbl_x, _tbl_row_y, _tbl_w, _tbl_row_h], _current_clip())
   def hover = vis && _disabled_on == 0 && _current_window_pointer_hit(_tbl_x, _tbl_row_y, _tbl_w, _tbl_row_h)
   _tbl_row_hit = hover && _mouse_pressed0
   if vis {
      def state = _disabled_on > 0 ? "disabled" : (_mouse_down0 && hover ? "pressed" : (hover ? "hovered" : "active"))
      def default_bg = selected ? _accent_soft_u32 : (hover ? _panel_hover_u32 : ((_tbl_row % 2) == 1 ? _panel_u32 : 0))
      def row_style = is_nil(style) ? _tbl_style : style
      def colors = _button_style(row_style, state, default_bg, _border_u32, _disabled_on > 0 ? _text_faint_u32 : _text_u32)
      def bg, edge = int(colors.get(0)), int(colors.get(1))
      if !is_nil(row_style) || selected || hover || (_tbl_row % 2) == 1 { _gui_rect_fast(_tbl_x, _tbl_row_y, _tbl_w, _tbl_row_h, bg) }
      def state_style = is_dict(row_style) ? row_style.get(state, nil) : nil
      if is_dict(state_style) && is_list(state_style.get("border", nil)) { _gui_rect_outline_fast(_tbl_x, _tbl_row_y, _tbl_w, _tbl_row_h, edge) }
   }
   _last_item = [_tbl_x, _tbl_row_y, _tbl_w, _tbl_row_h]
   vis
}

fn table_row_clicked() bool { "Returns true when the pointer was pressed on the current table row this frame." _tbl_row_hit }

fn table_next_column() bool {
   "Moves layout into the next cell of the current row; widgets submitted afterwards fill that cell."
   if _tbl_id == "" || _tbl_row < 0 { return false }
   _tbl_row_bottom = max(_tbl_row_bottom, _current_cursor_y_cache)
   _tbl_col += 1
   if _tbl_col >= _tbl_n { return false }
   def padx = _sx(6.0)
   def int cx_i, cw_i = _tbl_col * 2, _tbl_col * 2 + 1
   _current_body_x_cache = float(_tbl_cols.get(cx_i, _tbl_x)) + padx
   _current_body_w_cache = max(1.0, float(_tbl_cols.get(cw_i, 1.0)) - padx * 2.0)
   _current_cursor_y_cache = _tbl_row_y
   _same_line_next = false
   true
}

fn table_text(any txt, any tone=0) bool {
   "Fills the next cell with one clipped line of text. `tone` is a theme token name or an RGBA list."
   if !table_next_column() { return false }
   mut col = _text_u32
   if is_str(tone) { col = _text_color_u32(theme_color(to_str(tone))) }
   elif is_list(tone) { col = _text_color_u32(tone) }
   def x, w = _current_body_x_cache, _current_body_w_cache
   def cell = [x, _tbl_row_y, w, _tbl_row_h]
   if _rect_intersects(cell, _current_clip()) {
      _draw_text_ui_small_clipped(_ui_text(txt), x, _text_center_y_for(_font_small_id(), _tbl_row_y, _tbl_row_h), col, w, cell)
   }
   _current_cursor_y_cache = _tbl_row_y + _tbl_row_h
   _last_item = cell
   true
}

fn table_sort_column(any id) int {
   "Returns the column a table is sorted by, or -1 before any header was chosen."
   int(_state_get_num(_wid(id) + "#table", "sort", -1.0))
}

fn table_sort_descending(any id) bool { "Returns true when the table's sort order is descending." _state_get_num(_wid(id) + "#table", "desc", 0.0) > 0.5 }

fn end_table() any {
   "Ends the table started by begin_table."
   if _tbl_id == "" { return 0 }
   _tbl_row_bottom = max(_tbl_row_bottom, _current_cursor_y_cache)
   _current_body_x_cache, _current_body_w_cache = _tbl_base_x, _tbl_base_w
   _gui_rect_fast(_tbl_x, _tbl_row_bottom, _tbl_w, 1.0, _border_u32)
   _current_cursor_y_cache = _tbl_row_bottom + 1.0 + _gap()
   _last_item = [_tbl_x, _tbl_row_bottom, _tbl_w, 1.0]
   _tbl_id = ""
   _tbl_style = nil
   _tbl_row, _tbl_col = -1, -1
   _same_line_next = false
}

;; ---- drag and drop --------------------------------------------------------
fn drag_source(any kind, any payload, any label="") bool {
   "Makes the previous item draggable: once it is dragged past the threshold, `payload` travels with the pointer under the type name `kind`. Returns true while this item is the active drag."
   if !_enabled || _last_item_id == "" { return false }
   if !_dnd_active && _active_widget_id == _last_item_id && _mouse_down0 && _mouse_drag_ready() {
      _dnd_active = true
      _dnd_type = to_str(kind)
      _dnd_payload = payload
      _dnd_label = _ui_text(label)
      _dnd_source = _last_item_id
   }
   _dnd_active && _dnd_source == _last_item_id
}

fn drop_target(any kind) any {
   "Makes the previous item accept drags of type `kind`. Returns the payload on the frame it is dropped here, otherwise nil."
   if !_enabled || !_dnd_active { return nil }
   if _dnd_type != to_str(kind) { return nil }
   def x, y = float(_last_item.get(0, 0.0)), float(_last_item.get(1, 0.0))
   def w, h = float(_last_item.get(2, 0.0)), float(_last_item.get(3, 0.0))
   if !_rect_hit(_mouse_x, _mouse_y, x, y, w, h) { return nil }
   ;; The dragged item keeps the pointer captured, so the window test asks
   ;; which window is under the pointer rather than which one owns it.
   if _top_window_id_at(_mouse_x, _mouse_y) != _current_window_id_cache { return nil }
   if _rect_intersects(_last_item, _current_clip()) {
      _gui_rect_outline_fast(x, y, w, h, _accent_u32)
      _gui_rect_outline_fast(x + 1.0, y + 1.0, max(1.0, w - 2.0), max(1.0, h - 2.0), _accent_u32)
   }
   if !_mouse_down0 { return _dnd_payload }
   nil
}

fn drag_active(any kind="") bool {
   "Returns true while a drag is in flight(of type `kind`, when given)."
   def k = to_str(kind)
   _dnd_active && (k.len == 0 || k == _dnd_type)
}

;; ---- docking --------------------------------------------------------------
;;
;; A dockspace owns a rectangle and a tree of panels. A leaf holds one or more
;; windows as tabs; a split divides its rectangle between two children. Call
;; dockspace() once per frame before the windows it may contain:
;;
;;   gui.dockspace("main", 0.0, 0.0, w, h)
;;   if gui.begin_window("scene", ...) { ... } gui.end_window()
;;
;; Dragging a floating window by its title bar over the dockspace shows drop
;; indicators: a cross on the panel under the pointer (centre adds a tab, an
;; arm splits that panel) and one on each rim (splits the whole dockspace).
;; Releasing anywhere else leaves the window floating. Dragging a tab out
;; of its bar undocks the window again; dragging a splitter resizes. The tree
;; is plain nested lists and every edit builds a new tree:
;;
;;   leaf   ["L", active_index, [window ids]]
;;   split  ["S", "h" | "v", ratio, first, second]    h: side by side, v: stacked
;;
;; A closed window keeps its place in the tree and returns to it when shown
;; again; undock_window (or pulling its tab out) removes it. An id starting
;; with "@" is a spacer: a panel-shaped hole with no tab bar where the app
;; draws its own content, read back with dock_spacer_rect.
;;
;; dock_layout_save / dock_layout_load turn the tree and the floating window
;; rectangles into one line of text, so an app can restore its layout.
fn _dk_is_leaf(any n) bool {
   if !is_list(n) { return false }
   to_str(n.get(0, "")) == "L"
}

fn _dk_is_split(any n) bool {
   if !is_list(n) { return false }
   to_str(n.get(0, "")) == "S"
}

fn _dk_contains(any n, str win) bool {
   if _dk_is_leaf(n) {
      def any ids = n.get(2, 0)
      if !is_list(ids) { return false }
      return _list_find_text(ids, win) >= 0
   }
   if _dk_is_split(n) { return _dk_contains(n.get(3, 0), win) || _dk_contains(n.get(4, 0), win) }
   false
}

fn _dk_windows_into(any n, list out) list {
   if _dk_is_leaf(n) {
      def any ids = n.get(2, 0)
      mut acc = out
      if is_list(ids) {
         mut i = 0
         def int cnt = ids.len
         while i < cnt {
            acc = acc.append(to_str(ids.get(i, "")))
            i += 1
         }
      }
      return acc
   }
   if _dk_is_split(n) { return _dk_windows_into(n.get(4, 0), _dk_windows_into(n.get(3, 0), out)) }
   out
}

fn _dk_remove(any n, str win) any {
   "Returns the tree without `win`; an emptied leaf disappears and its sibling takes the parent's place."
   if _dk_is_leaf(n) {
      def any ids = n.get(2, 0)
      mut keep = []
      mut i = 0
      def int cnt = is_list(ids) ? ids.len : 0
      mut removed_at = -1
      while i < cnt {
         def cur = to_str(ids.get(i, ""))
         if cur == win { removed_at = i }
         else { keep = keep.append(cur) }
         i += 1
      }
      if keep.len == 0 { return 0 }
      mut active = int(float(n.get(1, 0.0)))
      if removed_at >= 0 && active >= removed_at && active > 0 { active -= 1 }
      if active >= keep.len { active = keep.len - 1 }
      return ["L", float(active), keep]
   }
   if _dk_is_split(n) {
      def any a = _dk_remove(n.get(3, 0), win)
      def any b = _dk_remove(n.get(4, 0), win)
      if !is_list(a) { return b }
      if !is_list(b) { return a }
      return ["S", to_str(n.get(1, "h")), float(n.get(2, 0.5)), a, b]
   }
   0
}

fn _dk_wrap(any n, str win, str side) any {
   def any fresh = ["L", 0.0, [win]]
   if !is_list(n) { return fresh }
   if side == "left" { return ["S", "h", 0.5, fresh, n] }
   if side == "right" { return ["S", "h", 0.5, n, fresh] }
   if side == "top" { return ["S", "v", 0.5, fresh, n] }
   if side == "bottom" { return ["S", "v", 0.5, n, fresh] }
   n
}

fn _dk_insert(any n, str target, str win, str side) any {
   "Returns the tree with `win` docked beside(or tabbed into) the leaf that holds `target`."
   if _dk_is_leaf(n) {
      if !_dk_contains(n, target) { return n }
      if side == "center" {
         def any ids = n.get(2, 0)
         mut next = []
         mut i = 0
         def int cnt = is_list(ids) ? ids.len : 0
         while i < cnt {
            next = next.append(to_str(ids.get(i, "")))
            i += 1
         }
         next = next.append(win)
         return ["L", float(next.len - 1), next]
      }
      return _dk_wrap(n, win, side)
   }
   if _dk_is_split(n) {
      return ["S", to_str(n.get(1, "h")), float(n.get(2, 0.5)), _dk_insert(n.get(3, 0), target, win, side), _dk_insert(n.get(4, 0), target, win, side)]
   }
   n
}

fn _dk_set_active(any n, str win) any {
   if _dk_is_leaf(n) {
      def any ids = n.get(2, 0)
      def idx = is_list(ids) ? _list_find_text(ids, win) : -1
      if idx < 0 { return n }
      return ["L", float(idx), ids]
   }
   if _dk_is_split(n) {
      return ["S", to_str(n.get(1, "h")), float(n.get(2, 0.5)), _dk_set_active(n.get(3, 0), win), _dk_set_active(n.get(4, 0), win)]
   }
   n
}

fn _dk_set_ratio(any n, str path, int depth, f64 ratio) any {
   "Returns the tree with the split addressed by `path` (a string of a/b turns from the root) resized."
   if !_dk_is_split(n) { return n }
   if depth >= path.len { return ["S", to_str(n.get(1, "h")), clamp(ratio, 0.08, 0.92), n.get(3, 0), n.get(4, 0)] }
   def turn = str.str_slice(path, depth, depth + 1)
   def int next_depth = depth + 1
   if turn == "a" { return ["S", to_str(n.get(1, "h")), float(n.get(2, 0.5)), _dk_set_ratio(n.get(3, 0), path, next_depth, ratio), n.get(4, 0)] }
   ["S", to_str(n.get(1, "h")), float(n.get(2, 0.5)), n.get(3, 0), _dk_set_ratio(n.get(4, 0), path, next_depth, ratio)]
}

fn _dk_is_spacer_id(str id) bool { str.startswith(id, "@") }

fn _dk_filter(any n, str path) any {
   "Returns the part of the tree that is on screen: closed windows are left out(they keep their place in the stored tree and return to it when shown again), and each split remembers its path in the stored tree."
   if _dk_is_leaf(n) {
      def any ids = n.get(2, 0)
      def int cnt = is_list(ids) ? ids.len : 0
      def int active = int(float(n.get(1, 0.0)))
      mut keep = []
      mut act = 0
      mut i = 0
      while i < cnt {
         def id = to_str(ids.get(i, ""))
         mut shown = true
         if !_dk_is_spacer_id(id) {
            def st = _window_lookup(id)
            if is_dict(st) { shown = bool(st.get("open", true)) }
         }
         if shown {
            if i == active { act = keep.len }
            keep = keep.append(id)
         }
         i += 1
      }
      if keep.len == 0 { return 0 }
      return ["L", float(act), keep]
   }
   if _dk_is_split(n) {
      def any a = _dk_filter(n.get(3, 0), path + "a")
      def any b = _dk_filter(n.get(4, 0), path + "b")
      if !is_list(a) { return b }
      if !is_list(b) { return a }
      return ["S", to_str(n.get(1, "h")), float(n.get(2, 0.5)), a, b, path]
   }
   0
}

mut _dk_leaves = []
mut _dk_splits = []

fn _dk_layout(any n, f64 x, f64 y, f64 w, f64 h, str path) any {
   "Collects leaf rectangles(x, y, w, h, node, path) and splitter rectangles(x, y, w, h, dir, path, parent x, y, w, h)."
   if _dk_is_leaf(n) {
      _dk_leaves = _dk_leaves.append(x)
      _dk_leaves = _dk_leaves.append(y)
      _dk_leaves = _dk_leaves.append(w)
      _dk_leaves = _dk_leaves.append(h)
      _dk_leaves = _dk_leaves.append(n)
      _dk_leaves = _dk_leaves.append(path)
      return 0
   }
   if !_dk_is_split(n) { return 0 }
   def dir = to_str(n.get(1, "h"))
   def ratio = clamp(float(n.get(2, 0.5)), 0.08, 0.92)
   def bar = max(3.0, _sx(5.0))
   ;; A filtered tree carries each split's path in the stored tree.
   def split_path = n.len > 5 ? to_str(n.get(5, path)) : path
   mut sx, sy, sw, sh = 0.0, 0.0, 0.0, 0.0
   if dir == "h" {
      def aw = _snap(max(1.0, (w - bar) * ratio))
      sx, sy, sw, sh = x + aw, y, bar, h
      _dk_layout(n.get(3, 0), x, y, aw, h, path + "a")
      _dk_layout(n.get(4, 0), x + aw + bar, y, max(1.0, w - aw - bar), h, path + "b")
   } else {
      def ah = _snap(max(1.0, (h - bar) * ratio))
      sx, sy, sw, sh = x, y + ah, w, bar
      _dk_layout(n.get(3, 0), x, y, w, ah, path + "a")
      _dk_layout(n.get(4, 0), x, y + ah + bar, w, max(1.0, h - ah - bar), path + "b")
   }
   _dk_splits = _dk_splits.append(sx)
   _dk_splits = _dk_splits.append(sy)
   _dk_splits = _dk_splits.append(sw)
   _dk_splits = _dk_splits.append(sh)
   _dk_splits = _dk_splits.append(dir)
   _dk_splits = _dk_splits.append(split_path)
   _dk_splits = _dk_splits.append(x)
   _dk_splits = _dk_splits.append(y)
   _dk_splits = _dk_splits.append(w)
   _dk_splits = _dk_splits.append(h)
}

fn _dock_space(str key) dict {
   if !is_dict(_dock_spaces) { _dock_spaces = dict(4) }
   mut sp = _dock_spaces.get(key, 0)
   if !is_dict(sp) {
      sp = dict(12)
      sp["tree"] = 0
      sp["x"] = 0.0
      sp["y"] = 0.0
      sp["w"] = 0.0
      sp["h"] = 0.0
      sp["chrome"] = []
      _dock_spaces[key] = sp
   }
   sp
}

fn _dock_tab_h() f64 { _small_item_h() + _sx(4.0) }

fn _dock_window_state(str win) dict {
   mut st = _window_lookup(win)
   if !is_dict(st) {
      st = dict(32)
      st["id"] = win
      st["open"] = true
      st["title"] = win
   }
   st
}

fn _dock_mark(str key, str win) any {
   "Flags a window as docked and remembers its floating size."
   if _dk_is_spacer_id(win) { return 0 }
   mut st = _dock_window_state(win)
   if to_str(st.get("dock", "")) == "" {
      st["float_w"] = float(st.get("w", 420.0))
      st["float_h"] = float(st.get("h", 320.0))
   }
   st["dock"] = key
   st["collapsed"] = false
   _window_store(st)
}

fn _dock_unmark(str win, f64 x, f64 y) any {
   mut st = _window_lookup(win)
   if !is_dict(st) { return 0 }
   st["dock"] = ""
   st["dock_show"] = true
   st["x"] = x
   st["y"] = y
   st["w"] = max(_min_win_w(), float(st.get("float_w", 420.0)))
   st["h"] = max(_min_win_h(), float(st.get("float_h", 320.0)))
   st["titlebar"] = true
   st["movable"] = true
   st["resizable"] = true
   _window_store(st)
}

fn window_docked(any id) bool {
   "Returns true while the window is docked in a dockspace."
   def st = _window_lookup(to_str(id))
   if !is_dict(st) { return false }
   to_str(st.get("dock", "")) != ""
}

fn dock_window(any space, any id, any target="", str side="center") bool {
   "Docks window `id` into dockspace `space`: as a tab of the panel holding `target` (side \"center\"), split off one of its edges(\"left\", \"right\", \"top\", \"bottom\"), or, with an empty target, against the whole dockspace. Returns false for an unknown side or target."
   def key, win = to_str(space), to_str(id)
   def tgt = to_str(target)
   if win.len == 0 { return false }
   if side != "center" && side != "left" && side != "right" && side != "top" && side != "bottom" { return false }
   mut sp = _dock_space(key)
   mut any tree = _dk_remove(sp.get("tree", 0), win)
   if !is_list(tree) {
      tree = ["L", 0.0, [win]]
   } elif tgt.len == 0 {
      if side == "center" {
         def any first = _dk_windows_into(tree, [])
         tree = _dk_insert(tree, to_str(first.get(0, "")), win, "center")
      } else { tree = _dk_wrap(tree, win, side) }
   } else {
      if tgt == win || !_dk_contains(tree, tgt) { return false }
      tree = _dk_insert(tree, tgt, win, side)
   }
   sp["tree"] = tree
   _dock_spaces[key] = sp
   _dock_mark(key, win)
   true
}

fn undock_window(any id) bool {
   "Removes a window from its dockspace and floats it where its panel was."
   def win = to_str(id)
   def st = _window_lookup(win)
   if !is_dict(st) { return false }
   def key = to_str(st.get("dock", ""))
   if key.len == 0 { return false }
   mut sp = _dock_space(key)
   sp["tree"] = _dk_remove(sp.get("tree", 0), win)
   _dock_spaces[key] = sp
   _dock_unmark(win, float(st.get("x", 0.0)) + _sx(24.0), float(st.get("y", 0.0)) + _sx(24.0))
   true
}

fn dock_split_ratio(any space, str path, f64 ratio) bool {
   "Sets the ratio of the split addressed by `path` (a/b turns from the root; \"\" is the root split)."
   mut sp = _dock_space(to_str(space))
   sp["tree"] = _dk_set_ratio(sp.get("tree", 0), path, 0, ratio)
   _dock_spaces[to_str(space)] = sp
   true
}

fn dock_windows(any space) list {
   "Returns the ids of the windows docked in `space`, in tree order."
   _dk_windows_into(_dock_space(to_str(space)).get("tree", 0), [])
}

fn _dock_chrome_hit(f64 px, f64 py) str {
   "Returns the pseudo window id of the dockspace whose tab bar or splitter is under the point."
   if !is_dict(_dock_spaces) { return "" }
   def keys = dict_keys(_dock_spaces)
   if !is_list(keys) { return "" }
   mut k = 0
   def int keys_n = keys.len
   while k < keys_n {
      def key = to_str(keys.get(k, ""))
      def any sp = _dock_spaces.get(key, 0)
      if is_dict(sp) {
         def any chrome = sp.get("chrome", 0)
         if is_list(chrome) {
            mut i = 0
            def int cn = chrome.len
            while i + 3 < cn {
               def int i1, i2 = i + 1, i + 2
               def int i3 = i + 3
               if _rect_hit(px, py, float(chrome.get(i, 0.0)), float(chrome.get(i1, 0.0)), float(chrome.get(i2, 0.0)), float(chrome.get(i3, 0.0))) { return "#dock:" + key }
               i += 4
            }
         }
      }
      k += 1
   }
   ""
}

fn _dock_target_size() f64 { max(24.0, _sx(34.0)) }

fn _dock_target_rect(str zone, f64 x, f64 y, f64 w, f64 h) list {
   "Returns the indicator square of one drop zone: a cross of five squares at the centre of the panel."
   def s = _dock_target_size()
   def step = s + _sx(4.0)
   mut cx, cy = x + w * 0.5 - s * 0.5, y + h * 0.5 - s * 0.5
   if zone == "left" { cx = cx - step }
   elif zone == "right" { cx = cx + step }
   elif zone == "top" { cy = cy - step }
   elif zone == "bottom" { cy = cy + step }
   [_snap(cx), _snap(cy), s, s]
}

fn _dock_rim_rect(str zone, f64 x, f64 y, f64 w, f64 h) list {
   "Returns the indicator square on the dockspace rim that splits the whole dockspace."
   def s = _dock_target_size()
   def inset = _sx(8.0)
   if zone == "left" { return [_snap(x + inset), _snap(y + h * 0.5 - s * 0.5), s, s] }
   if zone == "right" { return [_snap(x + w - inset - s), _snap(y + h * 0.5 - s * 0.5), s, s] }
   if zone == "top" { return [_snap(x + w * 0.5 - s * 0.5), _snap(y + inset), s, s] }
   [_snap(x + w * 0.5 - s * 0.5), _snap(y + h - inset - s), s, s]
}

fn _dock_zone_name(int k) str {
   if k == 0 { return "center" }
   if k == 1 { return "left" }
   if k == 2 { return "right" }
   if k == 3 { return "top" }
   "bottom"
}

fn _dock_draw_target(list r, str zone, bool hot) any {
   def tx, ty = float(r.get(0, 0.0)), float(r.get(1, 0.0))
   def ts = float(r.get(2, 0.0))
   _gui_rect_fast(tx, ty, ts, ts, hot ? _accent_u32 : _popup_u32)
   _gui_rect_outline_fast(tx, ty, ts, ts, hot ? _text_u32 : _text_faint_u32)
   ;; A small pictogram of where the window will land.
   def p = _sx(6.0)
   def half = _dock_zone_rect(zone, tx + p, ty + p, ts - p * 2.0, ts - p * 2.0)
   _gui_rect_fast(float(half.get(0, 0.0)), float(half.get(1, 0.0)), float(half.get(2, 0.0)), float(half.get(3, 0.0)), hot ? _window_bg_u32 : _text_dim_u32)
}

fn _dock_zone_rect(str zone, f64 x, f64 y, f64 w, f64 h) list {
   if zone == "left" { return [x, y, w * 0.5, h] }
   if zone == "right" { return [x + w * 0.5, y, w * 0.5, h] }
   if zone == "top" { return [x, y, w, h * 0.5] }
   if zone == "bottom" { return [x, y + h * 0.5, w, h * 0.5] }
   [x, y, w, h]
}

fn dockspace(any id, f64 x, f64 y, f64 w, f64 h) bool {
   "Lays out, draws, and handles input for a dockspace. Call once per frame before the windows it may hold. Returns true when the dockspace holds at least one window."
   if !_enabled { return false }
   _ensure_gui_state()
   _theme_refresh()
   def key = to_str(id)
   def pseudo = "#dock:" + key
   _dock_frame_active = true
   mut sp = _dock_space(key)
   mut any tree = sp.get("tree", 0)
   sp["x"] = x
   sp["y"] = y
   sp["w"] = w
   sp["h"] = h
   ;; Lay out what is on screen; closed windows stay in the stored tree.
   mut i = 0
   mut int cnt = 0
   _dk_leaves, _dk_splits = [], []
   mut any shown_tree = _dk_filter(tree, "")
   if is_list(shown_tree) { _dk_layout(shown_tree, _snap(x), _snap(y), _snap(w), _snap(h), "") }
   def leaves, splits = _dk_leaves, _dk_splits
   def tab_h = _dock_tab_h()
   def on_chrome = _window_hovered == pseudo
   mut chrome = []
   ;; A floating window dragged by its title bar: preview the drop zone, and
   ;; dock on release.
   def moving = _active_window_move
   mut drop_done = false
   if moving != "" && !window_docked(moving) && _rect_hit(_mouse_x, _mouse_y, x, y, w, h) && (_mouse_drag_ready() || !_mouse_down0) {
      ;; Drop targets: a cross of indicators on the panel under the pointer
      ;; and one on each rim of the dockspace. Only a release on an indicator
      ;; docks, so a window can also be left floating above the panels.
      mut zone, target = "", ""
      mut zx, zy, zw, zh = x, y, w, h
      mut lx0, ly0, lw0, lh0 = x, y, w, h
      mut have_leaf = !is_list(shown_tree)
      mut over_spacer = false
      mut li = 0
      def int ln = leaves.len
      while li + 5 < ln {
         def int l1, l2 = li + 1, li + 2
         def int l3, l4 = li + 3, li + 4
         def lx, ly = float(leaves.get(li, 0.0)), float(leaves.get(l1, 0.0))
         def lw, lh = float(leaves.get(l2, 0.0)), float(leaves.get(l3, 0.0))
         if _rect_hit(_mouse_x, _mouse_y, lx, ly, lw, lh) {
            def any leaf = leaves.get(l4, 0)
            def any ids = leaf.get(2, 0)
            target = to_str(ids.get(0, ""))
            lx0, ly0, lw0, lh0 = lx, ly, lw, lh
            have_leaf = true
            over_spacer = _dk_is_spacer_id(target)
         }
         li += 6
      }
      _ov_depth += 1
      mut k = 0
      while k < 5 {
         def zn = _dock_zone_name(k)
         if have_leaf && (k == 0 || is_list(shown_tree)) && !(k == 0 && over_spacer) {
            def tr = _dock_target_rect(zn, lx0, ly0, lw0, lh0)
            def hot = _rect_hit(_mouse_x, _mouse_y, float(tr.get(0, 0.0)), float(tr.get(1, 0.0)), float(tr.get(2, 0.0)), float(tr.get(3, 0.0)))
            if hot {
               zone = zn
               zx, zy, zw, zh = lx0, ly0, lw0, lh0
            }
            if _mouse_down0 { _dock_draw_target(tr, zn, hot) }
         }
         if k > 0 && is_list(shown_tree) {
            def rr = _dock_rim_rect(zn, x, y, w, h)
            def hot = _rect_hit(_mouse_x, _mouse_y, float(rr.get(0, 0.0)), float(rr.get(1, 0.0)), float(rr.get(2, 0.0)), float(rr.get(3, 0.0)))
            if hot {
               zone = zn
               target = ""
               zx, zy, zw, zh = x, y, w, h
            }
            if _mouse_down0 { _dock_draw_target(rr, zn, hot) }
         }
         k += 1
      }
      if zone != "" && _mouse_down0 {
         def zr = _dock_zone_rect(zone, zx, zy, zw, zh)
         _gui_rect_fast(float(zr.get(0, 0.0)), float(zr.get(1, 0.0)), float(zr.get(2, 0.0)), float(zr.get(3, 0.0)), _accent_soft_u32)
         _gui_rect_outline_fast(float(zr.get(0, 0.0)), float(zr.get(1, 0.0)), float(zr.get(2, 0.0)), float(zr.get(3, 0.0)), _accent_u32)
      }
      _ov_depth -= 1
      if zone != "" && !_mouse_down0 {
         sp["tree"] = tree
         _dock_spaces[key] = sp
         dock_window(key, moving, target, zone)
         sp = _dock_space(key)
         tree = sp.get("tree", 0)
         _active_window_move = ""
         drop_done = true
      }
   }
   if drop_done {
      _dk_leaves, _dk_splits = [], []
      shown_tree = _dk_filter(tree, "")
      if is_list(shown_tree) { _dk_layout(shown_tree, _snap(x), _snap(y), _snap(w), _snap(h), "") }
   }
   mut leaves2, splits2 = leaves, splits
   if drop_done { leaves2, splits2 = _dk_leaves, _dk_splits }
   ;; Splitters.
   i = 0
   cnt = splits2.len
   while i + 9 < cnt {
      def int s1, s2 = i + 1, i + 2
      def int s3, s4 = i + 3, i + 4
      def int s5, s6 = i + 5, i + 6
      def int s7, s8 = i + 7, i + 8
      def int s9 = i + 9
      def sx, sy = float(splits2.get(i, 0.0)), float(splits2.get(s1, 0.0))
      def sw, sh = float(splits2.get(s2, 0.0)), float(splits2.get(s3, 0.0))
      def dir, path = to_str(splits2.get(s4, "h")), to_str(splits2.get(s5, ""))
      def sid = pseudo + "::split:" + path
      def grab = _sx(3.0)
      def hot = on_chrome && _active_widget_id == "" && _rect_hit(_mouse_x, _mouse_y, sx - grab, sy - grab, sw + grab * 2.0, sh + grab * 2.0)
      if hot && _mouse_pressed0 { _active_widget_id = sid }
      def held = _active_widget_id == sid && _mouse_down0
      if hot || held {
         _ensure_cursors()
         _want_cursor(_cursor_resize)
      }
      if held {
         def px0, py0 = float(splits2.get(s6, 0.0)), float(splits2.get(s7, 0.0))
         def pw, ph = float(splits2.get(s8, 1.0)), float(splits2.get(s9, 1.0))
         def r = dir == "h" ? ((_mouse_x - px0 - sw * 0.5) / max(1.0, pw - sw)) : ((_mouse_y - py0 - sh * 0.5) / max(1.0, ph - sh))
         tree = _dk_set_ratio(tree, path, 0, r)
      }
      _gui_rect_fast(sx, sy, sw, sh, held ? _accent_u32 : (hot ? _text_faint_u32 : _border_u32))
      chrome = chrome.append(sx - grab)
      chrome = chrome.append(sy - grab)
      chrome = chrome.append(sw + grab * 2.0)
      chrome = chrome.append(sh + grab * 2.0)
      i += 10
   }
   ;; Tab bars, and the rectangle each docked window gets below its bar.
   i = 0
   cnt = leaves2.len
   mut undock_win = ""
   mut close_win = ""
   mut spacers = []
   while i + 5 < cnt {
      def int l1, l2 = i + 1, i + 2
      def int l3, l4 = i + 3, i + 4
      def lx, ly = float(leaves2.get(i, 0.0)), float(leaves2.get(l1, 0.0))
      def lw, lh = float(leaves2.get(l2, 0.0)), float(leaves2.get(l3, 0.0))
      def any leaf = leaves2.get(l4, 0)
      def any ids = leaf.get(2, 0)
      def int n = ids.len
      mut active = int(float(leaf.get(1, 0.0)))
      if active < 0 || active >= n { active = 0 }
      if _dk_is_spacer_id(to_str(ids.get(0, ""))) {
         ;; A spacer is a hole in the dockspace: no tab bar, no window, and the
         ;; pointer passes through to whatever the app draws there.
         spacers = spacers.append(to_str(ids.get(0, "")))
         spacers = spacers.append([lx, ly, lw, lh])
         i += 6
         continue
      }
      _gui_rect_fast(lx, ly, lw, tab_h, _window_hdr_u32)
      _gui_rect_fast(lx, ly + tab_h - 1.0, lw, 1.0, _border_u32)
      chrome = chrome.append(lx)
      chrome = chrome.append(ly)
      chrome = chrome.append(lw)
      chrome = chrome.append(tab_h)
      ;; Tabs share the bar: up to 180 px each, and when there are more than
      ;; fit at a comfortable width they all shrink so none is cut off.
      mut tab_w = min(_sx(180.0), lw / float(n))
      if tab_w < _sx(18.0) { tab_w = _sx(18.0) }
      mut t = 0
      while t < n {
         def win = to_str(ids.get(t, ""))
         mut st = _dock_window_state(win)
         def tx = lx + float(t) * tab_w
         def tid = pseudo + "::tab:" + win
         def is_active = t == active
         def closable = bool(st.get("closable", true))
         def close_w = closable && is_active ? _sx(18.0) : 0.0
         def hot = on_chrome && _rect_hit(_mouse_x, _mouse_y, tx, ly, tab_w, tab_h) && tx + tab_w <= lx + lw + 0.5
         def close_hot = hot && close_w > 0.0 && _mouse_x >= tx + tab_w - close_w
         if hot && _mouse_pressed0 {
            if close_hot { close_win = win }
            else {
               _active_widget_id = tid
               _focused_window = win
               if !is_active { tree = _dk_set_active(tree, win) }
            }
         }
         def held = _active_widget_id == tid && _mouse_down0
         ;; Pulling a tab out of its bar undocks the window and hands the drag
         ;; over to the ordinary window move.
         if held && _mouse_drag_ready() && (_mouse_y < ly - _sx(6.0) || _mouse_y > ly + tab_h + _sx(6.0)) { undock_win = win }
         if tx + tab_w <= lx + lw + 0.5 {
            if is_active { _gui_rect_fast(tx, ly, tab_w, tab_h - 1.0, _focused_window == win ? _panel_hover_u32 : _panel_u32) }
            elif hot { _gui_rect_fast(tx, ly, tab_w, tab_h - 1.0, _panel_u32) }
            if is_active { _gui_rect_fast(tx, ly + tab_h - _sx(2.0), tab_w, _sx(2.0), _focused_window == win ? _accent_u32 : _text_faint_u32) }
            _draw_text_ui_small_clipped(to_str(st.get("title", win)), tx + _sx(8.0), _text_center_y_for(_font_small_id(), ly, tab_h), is_active ? _text_u32 : _text_dim_u32, max(1.0, tab_w - _sx(14.0) - close_w), [tx, ly, max(1.0, tab_w - close_w), tab_h])
            if close_w > 0.0 {
               def ccx, ccy = _snap(tx + tab_w - close_w * 0.5 - _sx(2.0)), _snap(ly + tab_h * 0.5)
               def arm = _sx(3.0)
               def xcol = close_hot ? _danger_u32 : _text_dim_u32
               _gui_diag(ccx - arm, ccy - arm, ccx + arm, ccy + arm, max(1.0, _sx(1.0)), xcol)
               _gui_diag(ccx - arm, ccy + arm, ccx + arm, ccy - arm, max(1.0, _sx(1.0)), xcol)
            }
            if t > 0 { _gui_rect_fast(tx, ly + _sx(4.0), 1.0, tab_h - _sx(8.0), _border_u32) }
         }
         st["dock"] = key
         st["dock_show"] = is_active
         st["x"] = lx
         st["y"] = ly + tab_h
         st["w"] = lw
         st["h"] = max(1.0, lh - tab_h)
         st["collapsed"] = false
         _window_store(st)
         t += 1
      }
      i += 6
   }
   if close_win != "" {
      ;; Closed, not undocked: the window keeps its place for show_window.
      mut cst = _dock_window_state(close_win)
      cst["open"] = false
      _window_store(cst)
      _window_closed_events[close_win] = true
   }
   if undock_win != "" {
      tree = _dk_remove(tree, undock_win)
      def fw = max(_min_win_w(), float(_dock_window_state(undock_win).get("float_w", 420.0)))
      _dock_unmark(undock_win, _mouse_x - min(fw * 0.5, _sx(80.0)), _mouse_y - _title_h() * 0.5)
      _active_widget_id = ""
      _active_window_move = undock_win
      _move_off_x, _move_off_y = min(fw * 0.5, _sx(80.0)), _title_h() * 0.5
      _mouse_press_x, _mouse_press_y = _mouse_x - _sx(100.0), _mouse_y
      _focused_window = undock_win
      _window_order_touch(undock_win, true)
   }
   sp["tree"] = tree
   sp["chrome"] = chrome
   sp["spacers"] = spacers
   _dock_spaces[key] = sp
   _flush_rect_batch()
   is_list(shown_tree)
}

fn dock_spacer_rect(any space, any name) list {
   "Returns the rectangle [x, y, w, h] the dockspace left free for spacer `name` (an id starting with \"@\", docked like a window) at its last layout, or [] when it has none. An app draws its own content there: a viewport, a canvas."
   def sp = _dock_space(to_str(space))
   def any spacers = sp.get("spacers", 0)
   if !is_list(spacers) { return [] }
   def want = to_str(name)
   mut i = 0
   def int n = spacers.len
   while i + 1 < n {
      if to_str(spacers.get(i, "")) == want {
         def int ri = i + 1
         def any r = spacers.get(ri, 0)
         if is_list(r) { return r }
      }
      i += 2
   }
   []
}

;; ---- layout persistence ---------------------------------------------------
fn _dk_save(any n) str {
   if _dk_is_leaf(n) {
      def any ids = n.get(2, 0)
      mut out = "{" + to_str(int(float(n.get(1, 0.0))))
      mut i = 0
      def int cnt = ids.len
      while i < cnt {
         out = out + " " + to_str(ids.get(i, ""))
         i += 1
      }
      return out + "}"
   }
   if _dk_is_split(n) {
      return "[" + to_str(n.get(1, "h")) + " " + _fmt_fixed(float(n.get(2, 0.5)), 4) + " " + _dk_save(n.get(3, 0)) + " " + _dk_save(n.get(4, 0)) + "]"
   }
   "-"
}

mut _dk_parse_pos = 0

fn _dk_token(str s) str {
   "Reads the next space-delimited word, stopping before a bracket."
   def int n = s.len
   while _dk_parse_pos < n && str.str_slice(s, _dk_parse_pos, _dk_parse_pos + 1) == " " { _dk_parse_pos += 1 }
   def int start = _dk_parse_pos
   while _dk_parse_pos < n {
      def c = str.str_slice(s, _dk_parse_pos, _dk_parse_pos + 1)
      if c == " " || c == "}" || c == "]" || c == "{" || c == "[" || c == "|" { break }
      _dk_parse_pos += 1
   }
   str.str_slice(s, start, _dk_parse_pos)
}

fn _dk_parse(str s) any {
   "Parses one node at _dk_parse_pos; returns 0 for an empty or malformed node."
   def int n = s.len
   while _dk_parse_pos < n && str.str_slice(s, _dk_parse_pos, _dk_parse_pos + 1) == " " { _dk_parse_pos += 1 }
   if _dk_parse_pos >= n { return 0 }
   def c = str.str_slice(s, _dk_parse_pos, _dk_parse_pos + 1)
   if c == "{" {
      _dk_parse_pos += 1
      def active = str.atof(_dk_token(s))
      mut ids = []
      while _dk_parse_pos < n {
         def w = _dk_token(s)
         if w.len == 0 { break }
         ids = ids.append(w)
      }
      if _dk_parse_pos < n && str.str_slice(s, _dk_parse_pos, _dk_parse_pos + 1) == "}" { _dk_parse_pos += 1 }
      if ids.len == 0 { return 0 }
      mut act = active
      if act < 0.0 || act >= float(ids.len) { act = 0.0 }
      return ["L", act, ids]
   }
   if c == "[" {
      _dk_parse_pos += 1
      def dir = _dk_token(s)
      def ratio = str.atof(_dk_token(s))
      def any a = _dk_parse(s)
      def any b = _dk_parse(s)
      while _dk_parse_pos < n && str.str_slice(s, _dk_parse_pos, _dk_parse_pos + 1) == " " { _dk_parse_pos += 1 }
      if _dk_parse_pos < n && str.str_slice(s, _dk_parse_pos, _dk_parse_pos + 1) == "]" { _dk_parse_pos += 1 }
      if !is_list(a) { return b }
      if !is_list(b) { return a }
      return ["S", dir == "v" ? "v" : "h", clamp(ratio, 0.08, 0.92), a, b]
   }
   if c == "-" { _dk_parse_pos += 1 }
   0
}

fn dock_layout_save(any space) str {
   "Returns the dockspace tree and every floating window rectangle as one line of text, for dock_layout_load."
   _ensure_gui_state()
   def key = to_str(space)
   mut out = "dock1 " + _dk_save(_dock_space(key).get("tree", 0))
   def keys = dict_keys(_windows)
   mut i = 0
   def int cnt = is_list(keys) ? keys.len : 0
   while i < cnt {
      def win = to_str(keys.get(i, ""))
      def st = _windows.get(win, 0)
      if is_dict(st) {
         if !st.get("popup", false) && !_dk_is_spacer_id(win) && str.find(win, " ") < 0 && str.find(win, "|") < 0 {
            def docked = to_str(st.get("dock", "")) != ""
            def fw = docked ? float(st.get("float_w", 420.0)) : float(st.get("w", 420.0))
            def fh = docked ? float(st.get("float_h", 320.0)) : float(st.get("h", 320.0))
            out = out + " |" + win + " " + to_str(int(float(st.get("x", 0.0)))) + " " + to_str(int(float(st.get("y", 0.0)))) + " " + to_str(int(fw)) + " " + to_str(int(fh)) + " " + (st.get("open", true) ? "1" : "0")
         }
      }
      i += 1
   }
   out
}

fn dock_layout_load(any space, str text) bool {
   "Restores a layout written by dock_layout_save. Returns false, changing nothing, when the text is not a saved layout."
   _ensure_gui_state()
   def key = to_str(space)
   if str.find(text, "dock1 ") != 0 { return false }
   _dk_parse_pos = 6
   def any tree = _dk_parse(text)
   ;; Undock everything first, then mark what the loaded tree holds.
   def any old = _dk_windows_into(_dock_space(key).get("tree", 0), [])
   mut i = 0
   mut int cnt = old.len
   while i < cnt {
      def oid = to_str(old.get(i, ""))
      if !_dk_is_spacer_id(oid) {
         mut ost = _dock_window_state(oid)
         ost["dock"] = ""
         ost["dock_show"] = true
         _window_store(ost)
      }
      i += 1
   }
   ;; Floating rectangles: "|id x y w h open".
   def int n = text.len
   while _dk_parse_pos < n {
      def c = str.str_slice(text, _dk_parse_pos, _dk_parse_pos + 1)
      if c != "|" {
         _dk_parse_pos += 1
         continue
      }
      _dk_parse_pos += 1
      def win = _dk_token(text)
      def fx, fy = str.atof(_dk_token(text)), str.atof(_dk_token(text))
      def fw, fh = str.atof(_dk_token(text)), str.atof(_dk_token(text))
      def open = _dk_token(text) != "0"
      if win.len > 0 && fw > 0.0 && fh > 0.0 {
         mut st = _dock_window_state(win)
         st["x"] = fx
         st["y"] = fy
         st["w"] = fw
         st["h"] = fh
         st["float_w"] = fw
         st["float_h"] = fh
         st["open"] = open
         _window_store(st)
      }
   }
   mut sp = _dock_space(key)
   sp["tree"] = tree
   _dock_spaces[key] = sp
   def any now = _dk_windows_into(tree, [])
   i = 0
   cnt = now.len
   while i < cnt {
      def nid = to_str(now.get(i, ""))
      if !_dk_is_spacer_id(nid) {
         mut nst = _dock_window_state(nid)
         nst["dock"] = key
         nst["collapsed"] = false
         _window_store(nst)
      }
      i += 1
   }
   true
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
   assert(_snap(10.6) == 11.0 && _snap(10.2) == 10.0, "gui snap nearest")
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
   assert(begin_window("probe", "Probe", 8.0, 8.0, 280.0, 210.0,
         {"closable": false, "resizable": false, "collapsed": true, "collapsible": false, "titlebar": false}), "gui begin window expands pinned panel")
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
   print("✓ core.os.ui.render.viewer.gui self-test passed")
}
