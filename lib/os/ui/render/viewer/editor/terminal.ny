;; Keywords: editor terminal console output command os ui render viewer text
;; Embedded terminal and command output surface for editor workflows.
;; References:
;; - std.os.ui.render.viewer.editor.runner
module std.os.ui.render.viewer.editor.terminal(
   new, is_open, is_focused, title, mode, toggle_shell, toggle_repl,
   open_shell, open_repl, show_shell, show_repl, open_command, close, close_tab, select_tab, next_tab, prev_tab,
   tab_count, active_tab, tab_title, set_font, update, resize, draw, contains, handle_event,
   send_text, write_text, visible_cell_count, debug_text
)

use std.core
use std.core.common as common
use std.math (max, min)
use std.os (ticks)
use std.os.ui.render.viewer.editor.tools as tools
use std.os.ui.render.viewer.vterm as vterm
use std.os.ui.window.consts
use std.os.ui.render (font_load, FONT_FILTER_NEAREST, FONT_FILTER_LINEAR)

fn _fonts(any font) dict {
   def path = common.env_trim("NY_EDITOR_TERM_FONT")
   if path.len > 0 {
      def mode = common.env_trim("NY_EDITOR_TERM_FONT_FILTER")
      def filter = (mode == "nearest") ? FONT_FILTER_NEAREST : FONT_FILTER_LINEAR
      def f = font_load(path, 15, filter)
      if f { return {"regular": f, "bold": f, "italic": f, "emoji": f} }
   }
   {"regular": font, "bold": font, "italic": font, "emoji": font}
}

fn new(any font, int bg=0xff070707, int fg=0xfff0f0f0) dict {
   {
      "open": false, "focus": false, "mode": "shell", "font": font,
      "bg": bg, "fg": fg, "x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0,
      "cols": 80, "rows": 18, "active": 0, "update_tick": 0, "tabs": [],
      "vt": 0
   }
}

fn is_open(dict st) bool { bool(st.get("open", false)) }

fn is_focused(dict st) bool { bool(st.get("focus", false)) }

fn mode(dict st) str { to_str(st.get("mode", "shell")) }

fn _tabs(dict st) list {
   def tabs = st.get("tabs", [])
   is_list(tabs) ? tabs : []
}

fn _tab_at(list tabs, int idx) dict {
   if idx < 0 || idx >= tabs.len { return dict(0) }
   def tab = tabs.get(idx, dict(0))
   is_dict(tab) ? tab : dict(0)
}

fn tab_count(dict st) int { _tabs(st).len }

fn active_tab(dict st) int {
   def n = tab_count(st)
   if n <= 0 { return 0 }
   min(max(0, int(st.get("active", 0))), n - 1)
}

fn _blank_vt(dict st) any {
   _tune_vt(vterm.new(
         max(2, int(st.get("cols", 80))),
         max(2, int(st.get("rows", 18))),
         _fonts(st.get("font")),
         int(st.get("bg", 0xff070707)),
         int(st.get("fg", 0xfff0f0f0))
   ))
}

fn _ensure_vt(dict st) dict {
   if is_dict(st.get("vt")) { return st }
   st["vt"] = _blank_vt(st)
   mut tabs = _tabs(st)
   if tabs.len > 0 {
      def idx = active_tab(st)
      mut tab = _tab_at(tabs, idx)
      if !is_dict(tab.get("vt", 0)) {
         tab["vt"] = st.get("vt")
         tabs[idx] = tab
         st["tabs"] = tabs
      }
   }
   st
}

fn _tune_vt(any vt) any {
   if !is_dict(vt) { return vt }
   vt
   .set("parse_bytes", common.env_int_clamped("NY_EDITOR_TERM_PARSE_BYTES", 65536, 4096, 262144))
   .set("max_history", common.env_int_clamped("NY_EDITOR_TERM_HISTORY", 2000, 128, 20000))
}

fn _sync_active(dict st) dict {
   def tabs = _tabs(st)
   if tabs.len <= 0 {
      st["open"] = false
      st["active"] = 0
      st["mode"] = "terminal"
      return st
   }
   def idx = active_tab(st)
   mut tab = _tab_at(tabs, idx)
   st["active"] = idx
   st["vt"] = tab.get("vt", st.get("vt", 0))
   if !is_dict(st.get("vt")) {
      st["vt"] = _blank_vt(st)
      tab["vt"] = st.get("vt")
      tabs[idx] = tab
      st["tabs"] = tabs
   }
   st["mode"] = to_str(tab.get("mode", "terminal"))
   st["open"] = true
   st
}

fn _replace_active(dict st, any vt) dict {
   mut tabs = _tabs(st)
   if tabs.len <= 0 {
      st["vt"] = vt
      return st
   }
   def idx = active_tab(st)
   mut tab = _tab_at(tabs, idx)
   tab["vt"] = vt
   tabs[idx] = tab
   st["tabs"] = tabs
   _sync_active(st)
}

fn _update_tab_vt_budget(list tabs, int idx, int max_updates, int deadline_ns) list {
   if idx < 0 || idx >= tabs.len { return tabs }
   mut tab = _tab_at(tabs, idx)
   mut vt = tab.get("vt", 0)
   if !(is_dict(vt) && int(vt.get("master_fd", -1)) >= 0) { return tabs }
   mut updates = 0
   mut drained = 0
   while updates < max(1, max_updates) {
      vt = vterm.update(vt)
      def bytes = int(vt.get("last_update_bytes", 0))
      drained += max(0, bytes)
      updates += 1
      if bytes <= 0 { break }
      if deadline_ns > 0 && ticks() >= deadline_ns { break }
   }
   tab["vt"] = vt
   tab["last_update_bytes"] = drained
   tab["hot"] = drained > 0 || vterm.needs_visual_refresh(vt)
   tabs[idx] = tab
   tabs
}

fn _tab_label(str m, any vt) str {
   def t = is_dict(vt) ? to_str(vterm.get_title(vt)) : ""
   if t.len > 0 && t != "Terminal" { return t }
   if m == "repl" { return "ny repl" }
   if m == "debug" { return "debug" }
   m == "terminal" ? "shell" : m
}

fn tab_title(dict st, int idx=-1) str {
   def tabs = _tabs(st)
   if tabs.len <= 0 { return "" }
   if idx < 0 { idx = active_tab(st) }
   if idx < 0 || idx >= tabs.len { return "" }
   def tab = _tab_at(tabs, idx)
   _tab_label(to_str(tab.get("mode", "terminal")), tab.get("vt"))
}

fn title(dict st) str {
   if !is_open(st) { return "terminal" }
   tab_title(st)
}

fn _spawn(dict st, str m, str path, list args, str banner, list child_env=[]) dict {
   mut vt = _blank_vt(st)
   if child_env.len > 0 { vt = vt.set("child_env", child_env) }
   match vterm.open(vt, path, args) {
      ok(v) -> { vt = v }
      err(e) -> { vt = vterm.write(vt, "terminal start failed: " + to_str(e) + "\n") }
   }
   mut tabs = _tabs(st)
   tabs = tabs.append({"mode": m, "vt": vt})
   st["tabs"] = tabs
   st["active"] = tabs.len - 1
   st["open"] = true
   st["focus"] = true
   st = _sync_active(st)
   st = resize(st, float(st.get("x", 0.0)), float(st.get("y", 0.0)), float(st.get("w", 1.0)), float(st.get("h", 1.0)))
   banner.len > 0 ? write_text(st, banner) : st
}

fn open_shell(dict st) dict {
   _spawn(st, "terminal", vterm.default_shell_path(), vterm.default_shell_args(false), "")
}

fn open_repl(dict st) dict {
   _spawn(st, "repl", tools.ny_command(), [], "", [
         "NYTRIX_REPL_QUIET=1",
         "NYTRIX_REPL_PLAIN=1"
   ])
}

fn _find_tab_mode(dict st, str m) int {
   def tabs = _tabs(st)
   mut i = 0
   while i < tabs.len {
      def tab = _tab_at(tabs, i)
      if is_dict(tab) && to_str(tab.get("mode", "terminal")) == m { return i }
      i += 1
   }
   -1
}

fn _show_or_open(dict st, str m) dict {
   def idx = _find_tab_mode(st, m)
   if idx >= 0 { return select_tab(st, idx) }
   m == "repl" ? open_repl(st) : open_shell(st)
}

fn show_shell(dict st) dict { _show_or_open(st, "terminal") }

fn show_repl(dict st) dict { _show_or_open(st, "repl") }

fn open_command(dict st, str title, str path, list args, str banner="") dict {
   _spawn(st, title.len > 0 ? title : "command", path, args, banner)
}

fn select_tab(dict st, int idx) dict {
   if tab_count(st) <= 0 { return st }
   st["active"] = min(max(0, idx), tab_count(st) - 1)
   st["open"] = true
   st["focus"] = true
   st = _sync_active(st)
   resize(st, float(st.get("x", 0.0)), float(st.get("y", 0.0)), float(st.get("w", 1.0)), float(st.get("h", 1.0)))
}

fn next_tab(dict st) dict {
   def n = tab_count(st)
   n <= 1 ? st : select_tab(st, (active_tab(st) + 1) % n)
}

fn prev_tab(dict st) dict {
   def n = tab_count(st)
   n <= 1 ? st : select_tab(st, (active_tab(st) + n - 1) % n)
}

fn close_tab(dict st, int idx=-1) dict {
   mut tabs = _tabs(st)
   if tabs.len <= 0 { return close(st) }
   if idx < 0 { idx = active_tab(st) }
   if idx < 0 || idx >= tabs.len { return st }
   def old_active = active_tab(st)
   def tab = _tab_at(tabs, idx)
   def vt = tab.get("vt", 0)
   if is_dict(vt) { vterm.close(vt) }
   mut out = []
   mut i = 0
   while i < tabs.len {
      if i != idx { out = out.append(_tab_at(tabs, i)) }
      i += 1
   }
   st["tabs"] = out
   if out.len <= 0 {
      st["open"] = false
      st["focus"] = false
      st["active"] = 0
      st["mode"] = "terminal"
      st["vt"] = 0
      return st
   }
   st["active"] = idx < old_active ? old_active - 1 : old_active
   if int(st.get("active", 0)) >= out.len { st["active"] = out.len - 1 }
   _sync_active(st)
}

fn send_text(dict st, str text) dict {
   if is_open(st) {
      st = _ensure_vt(st)
      def vt = st.get("vt")
      if vterm.is_running(vt) { vterm.send_input(vt, text) }
      else { st = _replace_active(st, vterm.write(vt, text)) }
   }
   st
}

fn _display_text(str text) str {
   mut b = Builder(max(16, text.len + 8))
   mut i = 0
   while i < text.len {
      def c = load8(text, i) & 255
      if c == 10 && (i == 0 || (load8(text, i - 1) & 255) != 13) {
         b = builder_append_byte(b, 13)
      }
      b = builder_append_byte(b, c)
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn write_text(dict st, str text) dict {
   if is_open(st) {
      st = _ensure_vt(st)
      st = _replace_active(st, vterm.write(st.get("vt"), _display_text(text)))
   }
   st
}

fn _set_vt_font(any vt, any font) any {
   if !is_dict(vt) { return vt }
   _tune_vt(vterm.set_fonts(vt, _fonts(font)))
}

fn set_font(dict st, any font) dict {
   "Updates the shared terminal font and all existing tabs without clearing terminal contents."
   st["font"] = font
   mut tabs = _tabs(st)
   mut i = 0
   while i < tabs.len {
      mut tab = _tab_at(tabs, i)
      def vt = tab.get("vt", 0)
      if is_dict(vt) {
         tab["vt"] = _set_vt_font(vt, font)
         tabs[i] = tab
      }
      i += 1
   }
   st["tabs"] = tabs
   if tabs.len <= 0 && is_dict(st.get("vt", 0)) { st["vt"] = _set_vt_font(st.get("vt"), font) }
   st = _sync_active(st)
   resize(st, float(st.get("x", 0.0)), float(st.get("y", 0.0)), float(st.get("w", 1.0)), float(st.get("h", 1.0)))
}

fn _vt_visible_cells(any vt) int {
   if !is_dict(vt) { return 0 }
   def g = vt.get("grid", 0)
   if !g { return 0 }
   def co = max(0, int(vt.get("cols", 0)))
   def ro = max(0, int(vt.get("rows", 0)))
   mut n = 0
   mut i = 0
   while i < co * ro {
      def cp = load32(g, i * 16)
      if cp > 32 { n += 1 }
      i += 1
   }
   n
}

fn visible_cell_count(dict st) int {
   if !is_open(st) { return 0 }
   _vt_visible_cells(st.get("vt"))
}

fn debug_text(dict st, int max_rows=4, int max_cols=120) str {
   if !is_open(st) { return "" }
   def vt = st.get("vt")
   if !is_dict(vt) { return "" }
   def g = vt.get("grid", 0)
   if !g { return "" }
   def co = min(max(0, int(vt.get("cols", 0))), max_cols)
   def ro = min(max(0, int(vt.get("rows", 0))), max_rows)
   mut b = Builder(max(64, co * ro + ro))
   mut y = 0
   while y < ro {
      mut last = -1
      mut x = 0
      while x < co {
         def cp = load32(g, (y * int(vt.get("cols", 0)) + x) * 16)
         if cp > 32 { last = x }
         x += 1
      }
      x = 0
      while x <= last {
         def cp = load32(g, (y * int(vt.get("cols", 0)) + x) * 16)
         b = builder_append_byte(b, (cp >= 32 && cp < 127) ? cp : 63)
         x += 1
      }
      if y + 1 < ro { b = builder_append_byte(b, 10) }
      y += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn close(dict st) dict {
   def tabs = _tabs(st)
   mut i = 0
   while i < tabs.len {
      def tab = _tab_at(tabs, i)
      def vt = is_dict(tab) ? tab.get("vt", 0) : 0
      if is_dict(vt) { vterm.close(vt) }
      i += 1
   }
   if tabs.len <= 0 && is_dict(st.get("vt")) { vterm.close(st.get("vt")) }
   st["tabs"] = []
   st["open"] = false
   st["focus"] = false
   st["active"] = 0
   st["mode"] = "terminal"
   st["vt"] = 0
   st
}

fn toggle_shell(dict st) dict {
   if !is_open(st) { return open_shell(st) }
   mode(st) == "terminal" ? close_tab(st) : show_shell(st)
}

fn toggle_repl(dict st) dict {
   if !is_open(st) { return open_repl(st) }
   mode(st) == "repl" ? close_tab(st) : show_repl(st)
}

fn update(dict st) dict {
   if !is_open(st) { return st }
   mut tabs = _tabs(st)
   def n = tabs.len
   if n <= 0 { return _sync_active(st) }
   def active = active_tab(st)
   def tick = int(st.get("update_tick", 0))
   def active_max = common.env_int_clamped("NY_EDITOR_TERM_ACTIVE_UPDATE_MAX", 8, 1, 64)
   def inactive_every = common.env_int_clamped("NY_EDITOR_TERM_INACTIVE_EVERY", 8, 1, 120)
   def budget_ms = common.env_int_clamped("NY_EDITOR_TERM_UPDATE_BUDGET_MS", 3, 1, 16)
   def deadline = ticks() + budget_ms * 1000000
   tabs = _update_tab_vt_budget(tabs, active, active_max, deadline)
   def inactive = tick % n
   if n > 1 && inactive != active && (tick % inactive_every) == 0 && ticks() < deadline {
      tabs = _update_tab_vt_budget(tabs, inactive, 1, deadline)
   }
   mut hot = false
   mut bytes = 0
   mut i = 0
   while i < tabs.len {
      def tab = _tab_at(tabs, i)
      bytes += int(tab.get("last_update_bytes", 0))
      hot = hot || bool(tab.get("hot", false))
      i += 1
   }
   st["tabs"] = tabs
   st["update_tick"] = tick + 1
   st["last_update_bytes"] = bytes
   st["hot"] = hot
   _sync_active(st)
}

fn resize(dict st, f64 x, f64 y, f64 w, f64 h) dict {
   st["x"] = x
   st["y"] = y
   st["w"] = max(1.0, w)
   st["h"] = max(1.0, h)
   if !is_open(st) && _tabs(st).len <= 0 { return st }
   st = _ensure_vt(st)
   def vt = st.get("vt")
   def cw = max(1.0, float(vt.get("char_w", 9.0)))
   def ch = max(1.0, float(vt.get("char_h", 18.0)))
   def cols = max(2, int(max(1.0, w - 12.0) / cw))
   def rows = max(2, int(max(1.0, h - 10.0) / ch))
   def old_cols = int(st.get("cols", 0))
   def old_rows = int(st.get("rows", 0))
   st["cols"] = cols
   st["rows"] = rows
   if old_cols == cols && old_rows == rows && is_dict(st.get("vt", 0)) { return st }
   mut tabs = _tabs(st)
   mut i = 0
   while i < tabs.len {
      mut tab = _tab_at(tabs, i)
      def tvt = tab.get("vt", 0)
      if is_dict(tvt) {
         mut rvt = vterm.resize(tvt, cols, rows)
         def rcw = max(1.0, float(rvt.get("char_w", cw)))
         def rch = max(1.0, float(rvt.get("char_h", ch)))
         tab["vt"] = rvt.set("px_w", int(float(cols) * rcw)).set("px_h", int(float(rows) * rch))
         tabs[i] = tab
      }
      i += 1
   }
   st["tabs"] = tabs
   if tabs.len <= 0 {
      mut rvt = vterm.resize(vt, cols, rows)
      def rcw = max(1.0, float(rvt.get("char_w", cw)))
      def rch = max(1.0, float(rvt.get("char_h", ch)))
      st["vt"] = rvt.set("px_w", int(float(cols) * rcw)).set("px_h", int(float(rows) * rch))
   }
   _sync_active(st)
}

fn contains(dict st, f64 x, f64 y) bool {
   is_open(st) &&
   x >= float(st.get("x", 0.0)) && x <= float(st.get("x", 0.0)) + float(st.get("w", 0.0)) &&
   y >= float(st.get("y", 0.0)) && y <= float(st.get("y", 0.0)) + float(st.get("h", 0.0))
}

fn _event_data(dict st, any data) dict {
   mut d = is_dict(data) ? data : dict(8)
   d["mod"] = int(d.get("mod", d.get("mods", 0)))
   d["ww"] = float(st.get("w", 1.0))
   d["wh"] = float(st.get("h", 1.0))
   d
}

fn handle_event(dict st, int typ, any data) list {
   if !is_open(st) { return [st, false] }
   st = _ensure_vt(st)
   def d = _event_data(st, data)
   def mouse_event = typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED || typ == EVENT_MOUSE_POS_CHANGED || typ == EVENT_MOUSE_SCROLL
   if mouse_event {
      def inside = contains(st, float(d.get("x", -1.0)), float(d.get("y", -1.0)))
      if typ == EVENT_MOUSE_BUTTON_PRESSED { st["focus"] = inside }
      if !inside { return [st, false] }
   } elif !is_focused(st) {
      return [st, false]
   }
   st = _replace_active(st, vterm.handle_event(st.get("vt"), typ, d))
   [st, true]
}

fn draw(dict st) dict {
   if !is_open(st) { return st }
   st = _ensure_vt(st)
   vterm.set_viewport(float(st.get("x", 0.0)), float(st.get("y", 0.0)))
   st = _replace_active(st, vterm.draw(st.get("vt"), float(st.get("w", 1.0)), float(st.get("h", 1.0))))
   st
}

#main {
   mut st = new(0)
   st = resize(st, 10.0, 20.0, 400.0, 200.0)
   assert(contains(st.set("open", true), 12.0, 24.0), "editor terminal hit")
   st["tabs"] = [{"mode": "terminal", "vt": st.get("vt")}, {"mode": "repl", "vt": _blank_vt(st)}]
   st["open"] = true
   st = select_tab(st, 1)
   assert(tab_count(st) == 2 && active_tab(st) == 1 && mode(st) == "repl", "terminal tab select")
   st = set_font(st, 0)
   assert(tab_count(st) == 2 && is_dict(st.get("vt")), "terminal tab font update")
   st = show_shell(st)
   assert(tab_count(st) == 2 && active_tab(st) == 0 && mode(st) == "terminal", "terminal show shell reuses tab")
   st = show_repl(st)
   assert(tab_count(st) == 2 && active_tab(st) == 1 && mode(st) == "repl", "terminal show repl reuses tab")
   st = prev_tab(st)
   assert(active_tab(st) == 0 && tab_title(st, 0).len > 0, "terminal tab previous")
   st = close_tab(st, 0)
   assert(tab_count(st) == 1 && active_tab(st) == 0, "terminal tab close")
   st = close(st)
}
