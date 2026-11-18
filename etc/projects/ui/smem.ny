#!/usr/bin/env ny

;; Keywords: ui memory scanner process proc hex viewer tool
;; smem: process memory scanner and visualizer for Linux /proc, a port of
;; x3ric/smem. The scanner is core.os.memscan; this file is the UI.
;;
;;   ny etc/projects/ui/smem.ny [pid]
;;   ny etc/projects/ui/smem.ny <command> ...   (headless, no window)
;;
;; Commands (candidate files hold one "0xADDR TYPE VALUE" line per result):
;;   regions <pid> [--writable] [--shared]      list the scannable mappings
;;   scan <pid> <value> [--type T] [--region NAME] [--writable] [--shared] [--out FILE]
;;   next <pid> <value|changed|unchanged|increased|decreased> --in FILE [--out FILE]
;;   list FILE                                   print a candidate file
;;   read <pid> <addr> [--type T]                print one value
;;   write <pid> <addr> <value> [--type T]       write one value
;;   freeze <pid> <addr> <value> [--type T] [--every MS] [--for MS]
;;   shell [pid]   (or -i) interactive scanmem-style prompt; `help` lists its commands
;; T is a type name (Int32 by default for read/write/freeze, Auto for scan).
;;
;; Attaching to another process needs ptrace access: run as root, or set
;; kernel.yama.ptrace_scope=0, or start the target from smem's shell.
;;
;; Keys (letters only while no text field has focus):
;;   S toggle the scan bar          R toggle the region panel
;;   F1 attach / detach             F2 scan (first or next)
;;   F3 previous scan               F4 reset the scan
;;   F5 focus the address typed in the value field
;;   F7 lock / unlock the results at the value
;;   F8 / F9 / F10 scan Changed / Increased / Decreased
;;   F11 / F12 zoom out / in
;;   W writable mappings only       H shared mappings only (memfd, shm)
;;   Ctrl+S save the results to $XDG_CACHE_HOME/smem/<pid>.txt
;;   Ctrl+= / Ctrl+- / Ctrl+0 make the whole UI larger / smaller / default
;; The UI follows the window's content scale (HiDPI); NY_UI_SCALE sets an
;; extra factor at start.
;; Right-click an address row or a byte to copy its address; hover for the
;; decoded value.

use core
use core.common as common
use core.str as str
use core.math (max, min, clamp, floor, ceil)
use core.os (file_read, file_write, list_dir, ticks, msleep, cache_dir, path_join, is_dir, run_capture)
use core.os.args as args
use core.os.memscan as ms
use core.os.sys as sys
use core.os.ui.window as window
use core.os.ui.window.consts as keyc
use core.os.ui.render as gfx
use core.os.ui.render.viewer.gui as gui
use core.os.ui.render.viewer.icons as icons
use core.os.ui.assets.viewer as assets

def TYPES = ["Auto", "Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32", "UInt64",
   "Float32", "Float64", "Pointer", "Size", "Bool"]

;; ---- state ------------------------------------------------------------------
;; Scalars live in typed globals; lists and dicts only hold strings and lists.

mut any _win = 0
mut any _groups = []
mut str _status = ""
mut str _error = ""
mut f64 _zoom = 1.0
mut str _scan_value = "0"
mut int _mode_idx = 0
mut int _type_idx = 0
mut str _selected_region = ""
mut str _pid_query = ""
mut any _pid_items = []
mut int _pid_selected = 0
mut str _proc_name = ""
mut bool _show_scan = true
mut bool _show_regions = true
mut bool _only_writable = false
mut bool _only_shared = false
mut int _focus_addr = -1
mut int _focus_pending = 0
mut any _expanded = nil
;; UI scale: the window's content scale times the user's factor. Every
;; size below goes through px(), and the fonts are reloaded at the scaled
;; size when it changes.
mut f64 _ui = 1.0
mut f64 _ui_user = 1.0
mut f64 _font_scale = 0.0
mut int _font = 0
mut int _font_small = 0

fn px(f64 v) f64 { float(floor(v * _ui + 0.5)) }

fn update_scale() any {
   def list cs = window.get_window_content_scale(_win)
   def f64 content = max(1.0, float(cs.get(0, 1.0)))
   _ui = float(clamp(content * _ui_user, 0.75, 3.0))
   gui.set_scale(_ui)
   if _ui != _font_scale {
      _font = gfx.font_load_first(assets.MONO_FONT_CANDIDATES, int(16.0 * _ui + 0.5))
      _font_small = gfx.font_load_first(assets.MONO_FONT_CANDIDATES, int(13.0 * _ui + 0.5))
      gui.set_fonts(_font, _font, _font_small)
      _font_scale = _ui
   }
   0
}

;; Clicks of this frame, from the window event queue.
mut bool _lclick = false
mut bool _rclick = false

fn _set_error(str e) any {
   _error = e
   0
}

fn _comm(int p) str {
   def res = file_read("/proc/" + to_str(p) + "/comm")
   if is_err(res) { return "" }
   str.strip(to_str(unwrap(res)))
}

fn refresh_pids() any {
   mut items = []
   def entries = __list_dir_safe("/proc")
   mut int i = 0
   while i < entries.len {
      def str name = to_str(entries.get(i, ""))
      i += 1
      if name.len == 0 || name.len > 10 { continue }
      mut bool digits = true
      mut int j = 0
      while j < name.len {
         def int c = load8(name, j)
         if c < 48 || c > 57 { digits = false }
         j += 1
      }
      if !digits { continue }
      def str comm = _comm(int(str.atoi(name)))
      if comm.len > 0 { items = items.append(name + "  " + comm) }
   }
   _pid_items = sorted(items)
   0
}

fn __list_dir_safe(str path) list {
   def any d = list_dir(path)
   if is_list(d) { return d }
   []
}

fn _parse_pid(str s) int {
   def str t = str.strip(s)
   if t.len == 0 { return 0 }
   mut int v = 0
   mut int i = 0
   while i < t.len {
      def int c = load8(t, i)
      if c < 48 || c > 57 { return 0 }
      v = v * 10 + (c - 48)
      i += 1
   }
   v
}

fn do_attach(int p) bool {
   _set_error("")
   if !ms.attach(p) {
      _set_error(ms.last_error())
      return false
   }
   _groups = ms.load_maps(p)
   _expanded = dict(64)
   _selected_region = ""
   _proc_name = _comm(p)
   _status = "attached to " + to_str(p) + " " + _proc_name + ", " + to_str(_groups.len) + " mapping groups"
   true
}

fn do_detach() any {
   ms.detach_process()
   _groups = []
   _status = "detached"
   0
}

fn do_attach_query() any {
   mut int p = _parse_pid(_pid_query)
   if p == 0 {
      ;; A name: take the first process whose name contains it.
      def str q = str.lower(str.strip(_pid_query))
      mut int i = 0
      while i < _pid_items.len && q.len > 0 {
         def str it = to_str(_pid_items.get(i, ""))
         if str.find(str.lower(it), q) >= 0 {
            p = _parse_pid(str.str_slice(it, 0, str.find(it, " ")))
            break
         }
         i += 1
      }
   }
   if p > 0 { do_attach(p) } else { _set_error("no process matches '" + _pid_query + "'") }
   0
}

fn _regions() list {
   mut out = []
   mut int gi = 0
   while gi < _groups.len {
      def any g = _groups.get(gi, 0)
      gi += 1
      if !bool(g.get("enabled", true)) { continue }
      def str name = to_str(g.get("name", ""))
      if _selected_region.len > 0 && name != _selected_region { continue }
      def any regs = g.get("regions", [])
      mut int ri = 0
      while ri < regs.len {
         def any r = regs.get(ri, 0)
         ri += 1
         if _only_writable && !ms.region_writable(r) { continue }
         if _only_shared && !ms.region_shared(r) { continue }
         out = out.append(r)
      }
   }
   out
}

fn do_scan(str mode) any {
   if !ms.attached() { _set_error("attach to a process first (F1)") return 0 }
   def str tname = to_str(TYPES.get(_type_idx, "Auto"))
   def list regs = _regions()
   if ms.depth() == 0 && regs.len == 0 { _set_error("no mapping passes the region filters (W/H, selected group)") return 0 }
   def int t0 = ticks_ms()
   if !ms.scan(regs, _scan_value, mode, tname) {
      _set_error(ms.last_error())
      return 0
   }
   _set_error("")
   def int ms_taken = ticks_ms() - t0
   if ms.is_snapshot() {
      _status = "baseline: " + to_str(ms.snapshot_bytes() / 1024) + " KiB snapshot (" + to_str(ms_taken) + " ms); scan again with a comparison"
   } else {
      _status = to_str(ms.count()) + " results" + (ms.truncated() ? " (stopped at the limit)" : "") + ", level " + to_str(ms.depth()) + " (" + to_str(ms_taken) + " ms)"
   }
   0
}

fn ticks_ms() int { ticks() / 1000000 }

fn do_previous() any {
   if ms.previous() { _status = to_str(ms.count()) + " results, level " + to_str(ms.depth()) }
   0
}

fn do_reset() any {
   ms.lock_stop()
   ms.reset()
   _status = "scan reset"
   0
}

fn do_lock_toggle() any {
   if ms.lock_active() {
      ms.lock_stop()
      _status = "lock off"
      return 0
   }
   if ms.lock_start(_scan_value) { _status = "locked " + to_str(ms.count()) + " results at " + _scan_value } else { _set_error(ms.last_error()) }
   0
}

fn do_set() any {
   if ms.set_all(_scan_value) { _status = "wrote " + _scan_value + " to " + to_str(ms.count()) + " results" } else { _set_error(ms.last_error()) }
   0
}

fn do_focus() any {
   ;; F5: the value field holds an address (hex with or without 0x, or decimal).
   mut str t = str.strip(_scan_value)
   if str.startswith(t, "0x") || str.startswith(t, "0X") || str.find(t, ":") < 0 {
      if !ms.parse_value("hex:" + (str.startswith(t, "0x") || str.startswith(t, "0X") ? str.str_slice(t, 2, t.len) : t)) {
         _set_error("F5 needs an address in the value field")
         return 0
      }
      _focus_addr = ms.parsed_int()
      _focus_pending = 1
      _status = "focus 0x" + str.to_hex(_focus_addr, 0)
   }
   0
}

fn _filter_note() str {
   mut str f = ""
   if _only_writable { f = f + " writable" }
   if _only_shared { f = f + " shared" }
   f.len > 0 ? "region filter:" + f : "region filter off"
}

fn do_save() any {
   if !ms.attached() || ms.count() == 0 { _set_error("no results to save") return 0 }
   def str dir = path_join(cache_dir(), "smem")
   if !is_dir(dir) { run_capture("mkdir -p '" + dir + "'") }
   def str file = path_join(dir, to_str(ms.pid()) + ".txt")
   def str text = _cli_results_text()
   def res = file_write(file, text)
   if is_err(res) { _set_error("cannot write " + file) return 0 }
   _set_error("")
   _status = "saved " + to_str(ms.count()) + " results to " + file
   0
}

fn copy_addr(int a) any {
   window.set_clipboard(_win, "0x" + str.to_hex(a, 0))
   _status = "copied 0x" + str.to_hex(a, 0)
   0
}

;; Byte classes, each its own hue so a block reads at a glance:
;; 00 near black, 01-1F control (teal), 20-7E printable text (blue),
;; 7F-FE high bytes (amber), FF (red). Brightness grows with the value.
fn _mix(f64 a, f64 b, f64 t) f64 { a + (b - a) * t }

fn _byte_color(int b) list {
   if b == 0 { return [0.075, 0.075, 0.09, 1.0] }
   if b == 255 { return [0.90, 0.30, 0.32, 1.0] }
   if b < 32 {
      def f64 t = float(b) / 31.0
      return [_mix(0.08, 0.16, t), _mix(0.28, 0.58, t), _mix(0.30, 0.56, t), 1.0]
   }
   if b < 127 {
      def f64 t = float(b - 32) / 94.0
      return [_mix(0.18, 0.46, t), _mix(0.30, 0.64, t), _mix(0.60, 0.98, t), 1.0]
   }
   def f64 t = float(b - 127) / 127.0
   [_mix(0.42, 0.98, t), _mix(0.26, 0.66, t), _mix(0.08, 0.22, t), 1.0]
}

fn _ink_for(int b) list {
   ;; Dark text on the bright cells, light text on the dark ones.
   if b == 0 { return [0.42, 0.42, 0.48, 1.0] }
   if b >= 200 || (b >= 90 && b < 127) { return [0.04, 0.04, 0.06, 1.0] }
   [0.93, 0.94, 0.97, 1.0]
}

fn _char_text(int b) str {
   if b >= 32 && b < 127 { return "'" + chr(b) + "'" }
   "."
}


;; ---- top bar -------------------------------------------------------------------

fn ibtn(str id, str icon, str tip, bool selected=false) bool {
   ;; Square SVG icon button; the tooltip names it and its key.
   def bool hit = gui.icon_button(id, icons.icon_tex(icon, 48), "", px(34.0), px(30.0), selected)
   gui.tooltip(tip)
   hit
}

fn draw_topbar(f64 ww) f64 {
   ;; One row: who is attached, the scan fields and every action as an icon,
   ;; then the status text in what is left of the row.
   def f64 h = px(46.0)
   def f64 gap = px(5.0)
   def f64 ib = px(34.0)
   gui.begin_window("smem_top", "smem", 0.0, 0.0, ww, h,
      {"titlebar": false, "resizable": false, "movable": false, "collapsible": false, "closable": false})
   gui.set_window_pos("smem_top", 0.0, 0.0)
   gui.set_window_size("smem_top", ww, h)
   if ms.attached() {
      def f64 who = min(px(190.0), ww * 0.16)
      if _show_scan {
         gui.layout_row([who, ib, ib, px(110.0), px(160.0), max(px(120.0), ww * 0.16), ib, ib, ib, ib, ib, ib, px(62.0), ib], gap)
      } else {
         gui.layout_row([who, ib, ib], gap)
      }
      gui.text_colored(" ", "ok")
      gui.tooltip("Attached process: PID and name")
      def list wr = gui.last_item_rect()
      ;; Centred on the 30 px icon row the label shares.
      gui.paint_rect(float(wr.get(0, 0.0)), float(wr.get(1, 0.0)) + px(11.0), px(8.0), px(8.0), [0.35, 0.85, 0.45, 1.0])
      gui.paint_text(float(wr.get(0, 0.0)) + px(14.0), float(wr.get(1, 0.0)) + px(6.0),
         str.str_slice(to_str(ms.pid()) + " " + _proc_name, 0, int((who - px(16.0)) / (9.4 * _ui))), [0.55, 0.92, 0.62, 1.0])
      if ibtn("detach", "unlinked", "Detach (F1)") { do_detach() }
      if ibtn("reload", "reload", "Reload the memory map") {
         _groups = ms.load_maps(ms.pid())
         _status = to_str(_groups.len) + " mapping groups"
      }
      if _show_scan {
         scan_fields()
         if ibtn("scan", "search", "Scan (F2): first scan, then narrow") { do_scan(to_str(ms.MODES.get(_mode_idx, "Exact"))) }
         if ibtn("prev", "history", "Previous scan (F3)") { do_previous() }
         if ibtn("reset", "clear", "Reset the scan (F4)") { do_reset() }
         if ibtn("set", "edit", "Write the value to every result") { do_set() }
         mut str lock_icon = "unlock"
         mut str lock_tip = "Lock every result at the value (F7)"
         if ms.lock_active() {
            lock_icon = "lock"
            lock_tip = "Unlock (F7)"
         }
         if ibtn("lock", lock_icon, lock_tip, ms.lock_active()) { do_lock_toggle() }
         if ibtn("zoom_out", "zoom_less", "Smaller cells (F11)") { _zoom = float(clamp(_zoom / 1.25, 0.5, 16.0)) }
         if gui.button("zoom_reset", "x" + str.to_fixed(_zoom, 1), 0.0, px(30.0)) { _zoom = 1.0 }
         gui.tooltip("Cell zoom; click resets to x1")
         if ibtn("zoom_in", "zoom_more", "Larger cells (F12)") { _zoom = float(clamp(_zoom * 1.25, 0.5, 16.0)) }
      }
   } else {
      gui.layout_row([max(px(160.0), ww * 0.22), ib, ib, max(px(220.0), ww * 0.30), ib], gap)
      _pid_query = gui.input_text("pid_query", "", _pid_query, "process name or PID", 0.0)
      gui.tooltip("Type a PID or part of a name, then press F1 or the link button")
      if ibtn("attach_q", "search", "Attach to the typed PID or name (F1)") { do_attach_query() }
      if ibtn("refresh", "reload", "Refresh the process list") { refresh_pids() }
      mut labels = ["select a process"]
      mut int cur = 0
      def str q = str.lower(str.strip(_pid_query))
      mut int i = 0
      while i < _pid_items.len && labels.len < 200 {
         def str it = to_str(_pid_items.get(i, ""))
         i += 1
         if q.len == 0 || str.find(str.lower(it), q) >= 0 {
            labels = labels.append(it)
            if _parse_pid(str.str_slice(it, 0, max(0, str.find(it, " ")))) == _pid_selected { cur = labels.len - 1 }
         }
      }
      def int pick = gui.combo_box("pid_pick", "", labels, cur, 0.0, 14)
      if pick > 0 {
         def str it = to_str(labels.get(pick, ""))
         _pid_selected = _parse_pid(str.str_slice(it, 0, max(0, str.find(it, " "))))
      }
      if ibtn("attach", "link", "Attach to the selected process") && _pid_selected > 0 { do_attach(_pid_selected) }
   }
   def list rr = gui.last_item_rect()
   draw_status(float(rr.get(0, 0.0)) + float(rr.get(2, 0.0)) + px(12.0), float(rr.get(1, 0.0)) + (float(rr.get(3, 0.0)) - px(15.0)) * 0.5, ww)
   gui.end_window()
   h
}

fn scan_fields() any {
   ;; Captionless; tooltips name them.
   _type_idx = gui.combo_box("type", "", TYPES, _type_idx, 0.0, 14)
   gui.tooltip("Value type (Auto tries every size)")
   _mode_idx = gui.combo_box("mode", "", ms.MODES, _mode_idx, 0.0, 9)
   gui.tooltip("Scan mode; Changed/Increased/... compare with the last scan")
   _scan_value = gui.input_text("value", "", _scan_value, "value or address", 0.0)
   gui.tooltip("Value to scan for, set, or lock; F5 jumps to it as an address")
   0
}

;; ---- status line ----------------------------------------------------------------
;; Painted at the right of the top bar's first row: the last message or error,
;; the key hints when there is none, and a badge while a lock is writing.

fn draw_status(f64 x, f64 y, f64 ww) any {
   ;; The rest of the row: the last message (red for errors) or the key hints.
   mut list ink = [0.62, 0.62, 0.68, 1.0]
   mut str msg = _status
   if _error.len > 0 {
      ink = [1.0, 0.45, 0.42, 1.0]
      msg = _error
   }
   if msg.len == 0 { msg = "F1 attach  F2 scan  F3 prev  F4 reset  F5 go to  F7 lock  F8-F10 quick  S/R panels  W/H filter  Ctrl+S save  Ctrl+/- size" }
   def int room = int((ww - px(12.0) - x) / (7.8 * _ui))
   if room < 8 { return 0 }
   if msg.len > room { msg = str.str_slice(msg, 0, room - 3) + "..." }
   gui.paint_text(x, y, msg, ink, true)
   0
}

;; ---- region panel ----------------------------------------------------------------

fn draw_regions(f64 x, f64 y, f64 w, f64 h) any {
   gui.begin_window("smem_regions", "Regions", x, y, w, h,
      {"titlebar": false, "resizable": false, "movable": false, "collapsible": false, "closable": false})
   gui.set_window_pos("smem_regions", x, y)
   gui.set_window_size("smem_regions", w, h)
   gui.text_colored("Regions", "text_dim")
   if gui.selectable("all", "All enabled", _selected_region.len == 0, 0.0, 0.0) { _selected_region = "" }
   gui.begin_scroll_area("regions_scroll", w - 16.0, max(40.0, h - 70.0))
   mut int gi = 0
   while gi < _groups.len {
      mut any g = _groups.get(gi, 0)
      def str name = to_str(g.get("name", ""))
      gui.push_id("g" + to_str(gi))
      ;; Keep the tail of long paths, as much as the panel width holds.
      def int fit = max(8, int((w - px(90.0)) / (8.6 * _ui)))
      mut str short = name
      if name.len > fit { short = "..." + str.str_slice(name, name.len - fit + 3, name.len) }
      gui.layout_row([px(28.0), 0.0], px(2.0))
      def bool en = gui.checkbox("en", "", bool(g.get("enabled", true)))
      g["enabled"] = en
      ;; Clicking a name scans only that group (smem's selected region).
      if gui.selectable("pick", short + "  (" + to_str(g.get("regions", []).len) + ")", _selected_region == name, 0.0, 0.0) {
         if _selected_region == name { _selected_region = "" } else { _selected_region = name }
      }
      gui.tooltip(name)
      gui.pop_id()
      gi += 1
   }
   gui.end_scroll_area()
   gui.end_window()
   0
}

;; ---- results view -------------------------------------------------------------

fn _type_ink(int t) list {
   ;; Integers green, unsigned teal, floats violet, pointers/sizes amber.
   def str n = ms.type_name(t)
   if str.startswith(n, "Float") { return [0.74, 0.60, 1.0, 1.0] }
   if str.startswith(n, "UInt") { return [0.45, 0.85, 0.85, 1.0] }
   if n == "Pointer" || n == "Size" { return [0.98, 0.72, 0.35, 1.0] }
   if n == "Bool" { return [0.95, 0.55, 0.65, 1.0] }
   [0.55, 0.88, 0.55, 1.0]
}

fn draw_results(f64 x, f64 y, f64 w, f64 h) any {
   ;; A table: address | size | type | value | first bytes, one aligned column
   ;; each, with a fixed header and zebra rows.
   gui.begin_window("smem_results", "Results", x, y, w, h,
      {"titlebar": false, "resizable": false, "movable": false, "collapsible": false, "closable": false})
   gui.set_window_pos("smem_results", x, y)
   gui.set_window_size("smem_results", w, h)
   def int n = ms.count()
   def f64 row_h = max(px(20.0), min(px(32.0), floor((4.0 * _zoom + 16.0) * _ui)))
   def f64 c_addr = px(10.0)
   def f64 c_size = px(196.0)
   def f64 c_type = px(250.0)
   def f64 c_val = px(340.0)
   def f64 c_bytes = max(c_val + px(220.0), w * 0.62)
   gui.spacer_px(px(24.0))
   def list hr = gui.last_item_rect()
   def f64 hx = float(hr.get(0, 0.0))
   def f64 hy = float(hr.get(1, 0.0))
   def list head_ink = [0.60, 0.60, 0.68, 1.0]
   gui.paint_rect(hx, hy, w - px(32.0), px(22.0), [0.12, 0.12, 0.15, 1.0])
   gui.paint_text(hx + c_addr, hy + px(4.0), "Address", head_ink, true)
   gui.paint_text(hx + c_size, hy + px(4.0), "Size", head_ink, true)
   gui.paint_text(hx + c_type, hy + px(4.0), "Type", head_ink, true)
   gui.paint_text(hx + c_val, hy + px(4.0), "Value", head_ink, true)
   gui.paint_text(hx + c_bytes, hy + px(4.0), "Bytes", head_ink, true)
   gui.set_scroll_area_content_hint("res_scroll", float(n) * row_h)
   gui.begin_scroll_area("res_scroll", w - px(16.0), h - px(48.0))
   def list origin = gui.last_item_rect()
   def f64 ox = float(origin.get(0, 0.0))
   def f64 oy = float(origin.get(1, 0.0))
   def f64 sy = gui.scroll_area_scroll_y()
   def f64 vh = gui.scroll_area_visible_h()
   def f64 bw = w - px(40.0)
   def list mp = gui.mouse_xy()
   def f64 mx = float(mp.get(0, -1.0))
   def f64 my = float(mp.get(1, -1.0))
   def bool rclick = _rclick
   mut int first = int(floor(sy / row_h))
   if first < 0 { first = 0 }
   def int last = min(n, first + int(vh / row_h) + 2)
   def f64 text_dy = (row_h - px(15.0)) * 0.5
   def f64 sw = px(10.0)
   mut int hover = -1
   mut int i = first
   while i < last {
      def f64 ry = oy + float(i) * row_h
      def int a = ms.addr_at(i)
      def int t = ms.type_at(i)
      def int sz = ms.type_size(t)
      def ptr b = ms.read_bytes(a, sz)
      def bool hot = mx >= ox && mx < ox + bw && my >= ry && my < ry + row_h
      if hot { hover = i }
      mut list bg = [0.085, 0.085, 0.10, 1.0]
      if i % 2 == 1 { bg = [0.105, 0.105, 0.125, 1.0] }
      if hot { bg = [0.19, 0.16, 0.28, 1.0] }
      gui.paint_rect(ox, ry, bw, row_h - 1.0, bg)
      if a == _focus_addr { gui.paint_rect(ox, ry, px(3.0), row_h - 1.0, [1.0, 0.75, 0.2, 1.0]) }
      gui.paint_text(ox + c_addr, ry + text_dy, "0x" + str.to_hex(a, 12), [0.70, 0.72, 0.80, 1.0], true)
      gui.paint_text(ox + c_size, ry + text_dy, to_str(sz) + " B", [0.55, 0.55, 0.62, 1.0], true)
      gui.paint_text(ox + c_type, ry + text_dy, ms.type_name(t), _type_ink(t), true)
      if b {
         gui.paint_text(ox + c_val, ry + text_dy - px(1.0), ms.read_value_text(a, t), [0.97, 0.97, 1.0, 1.0])
         ;; The value's bytes as small colored cells, low address first.
         mut int k = 0
         while k < sz && k < 8 {
            def int bv = load8(b, k) & 255
            def f64 cx = ox + c_bytes + float(k) * (sw * 2.4)
            gui.paint_rect(cx, ry + (row_h - sw * 1.6) * 0.5, sw * 2.2, sw * 1.6, _byte_color(bv))
            gui.paint_text(cx + px(2.0), ry + (row_h - sw * 1.6) * 0.5 + px(1.0), str.to_hex(bv, 2), _ink_for(bv), true)
            k += 1
         }
         free(b)
      } else {
         gui.paint_text(ox + c_val, ry + text_dy, "unreadable", [0.9, 0.4, 0.4, 1.0], true)
      }
      i += 1
   }
   gui.spacer_px(float(n) * row_h)
   if hover >= 0 {
      def int a = ms.addr_at(hover)
      if rclick { copy_addr(a) }
      _tooltip(mx, my, _value_lines(a, ms.type_name(ms.type_at(hover)) + " " + ms.value_text(hover)))
   }
   gui.end_scroll_area()
   gui.end_window()
   0
}

fn _value_lines(int a, str head) list {
   ;; The same bytes read as every type, for the hover card.
   mut list lines = ["0x" + str.to_hex(a, 16), head]
   def ptr b = ms.read_bytes(a, 1)
   if b {
      def int bv = load8(b, 0) & 255
      free(b)
      lines = lines.append("byte 0x" + str.to_hex(bv, 2) + "  " + to_str(bv) + "  " + _char_text(bv))
   }
   lines = lines.append("i16 " + ms.read_value_text(a, 1) + "   i32 " + ms.read_value_text(a, 2))
   lines = lines.append("i64 " + ms.read_value_text(a, 3))
   lines = lines.append("f32 " + ms.read_value_text(a, 8) + "   f64 " + ms.read_value_text(a, 9))
   lines = lines.append("right-click copies the address")
   lines
}

mut any _tip_lines = []
mut f64 _tip_x = 0.0
mut f64 _tip_y = 0.0

fn _tooltip(f64 mx, f64 my, list lines) any {
   ;; Recorded here, drawn by draw_tooltip as the frame's last window, so no
   ;; view's text shows through it.
   _tip_lines = lines
   _tip_x = mx
   _tip_y = my
   0
}

fn draw_tooltip(f64 ww, f64 wh) any {
   if !is_list(_tip_lines) || _tip_lines.len == 0 { return 0 }
   def f64 tw = px(430.0)
   def f64 th = max(px(84.0), px(16.0) + px(20.0) * float(_tip_lines.len))
   mut f64 x = _tip_x + px(18.0)
   mut f64 y = _tip_y + px(18.0)
   if x + tw > ww - px(4.0) { x = _tip_x - tw - px(12.0) }
   if y + th > wh - px(4.0) { y = wh - th - px(4.0) }
   gui.begin_window("smem_tip", "", x, y, tw, th,
      {"titlebar": false, "resizable": false, "movable": false, "collapsible": false, "closable": false})
   gui.set_window_pos("smem_tip", x, y)
   gui.set_window_size("smem_tip", tw, th)
   mut int i = 0
   while i < _tip_lines.len {
      mut str role = "text"
      if i == 0 { role = "accent" }
      if i == _tip_lines.len - 1 { role = "text_dim" }
      gui.text_colored(to_str(_tip_lines.get(i, "")), role)
      i += 1
   }
   gui.end_window()
   _tip_lines = []
   0
}

;; ---- maps view -----------------------------------------------------------------
;; Per enabled group: a header row; when expanded, per region a label row, an
;; offset ruler and ceil(size / bytes_per_row) rows of bsz-pixel cells. Each
;; row starts with its address in a gutter; rows hold a multiple of 8 bytes
;; so columns line up. Only rows inside the visible band are read.

fn _is_expanded(str name) bool { is_dict(_expanded) && to_str(_expanded.get(name, "")) == "1" }

fn GROUP_H() f64 { px(26.0) }
fn REGION_H() f64 { px(36.0) }

fn _cell_size() f64 { max(3.0, min(px(34.0), floor(6.0 * _zoom * _ui))) }

fn _gutter(f64 bsz) f64 {
   if bsz >= px(10.0) { return px(132.0) }
   px(8.0)
}

fn _bytes_per_row(f64 aw, f64 bsz) int {
   def int fit = int(floor((aw - _gutter(bsz)) / bsz))
   max(8, (fit / 8) * 8)
}

fn _legend(f64 x, f64 y) any {
   def list items = [[0, "00"], [10, "01-1F ctrl"], [80, "20-7E text"], [200, "7F-FE high"], [255, "FF"]]
   mut f64 cx = x
   mut int i = 0
   while i < items.len {
      def list it = items.get(i, [0, ""])
      gui.paint_rect(cx, y + px(3.0), px(14.0), px(14.0), _byte_color(int(it.get(0, 0))))
      gui.paint_text(cx + px(19.0), y + px(3.0), to_str(it.get(1, "")), [0.66, 0.66, 0.72, 1.0], true)
      cx = cx + px(30.0) + px(8.2) * float(to_str(it.get(1, "")).len)
      i += 1
   }
   gui.paint_text(cx + px(10.0), y + px(3.0), "F11/F12 cell size   click a header to open it", [0.45, 0.45, 0.52, 1.0], true)
   0
}

fn draw_maps(f64 x, f64 y, f64 w, f64 h) any {
   gui.begin_window("smem_maps", "Maps", x, y, w, h,
      {"titlebar": false, "resizable": false, "movable": false, "collapsible": false, "closable": false})
   gui.set_window_pos("smem_maps", x, y)
   gui.set_window_size("smem_maps", w, h)
   if !ms.attached() {
      gui.panel_state("empty", "Not attached", "Pick a process above, or press F1 to attach to the PID typed there")
      gui.end_window()
      return 0
   }
   def f64 bsz = _cell_size()
   def f64 aw = w - px(40.0)
   def f64 gut = _gutter(bsz)
   def int bpr = _bytes_per_row(aw, bsz)
   def bool show_hex = bsz >= px(20.0)
   gui.spacer_px(px(22.0))
   def list lr = gui.last_item_rect()
   _legend(float(lr.get(0, 0.0)), float(lr.get(1, 0.0)))
   ;; Content height.
   mut f64 total = 0.0
   mut int gi = 0
   while gi < _groups.len {
      def any g = _groups.get(gi, 0)
      gi += 1
      if !bool(g.get("enabled", true)) { continue }
      total = total + GROUP_H()
      if _is_expanded(to_str(g.get("name", ""))) {
         def any regs = g.get("regions", [])
         mut int ri = 0
         while ri < regs.len {
            def any r = regs.get(ri, 0)
            def int size = int(r.get(1, 0)) - int(r.get(0, 0))
            total = total + REGION_H() + float((size + bpr - 1) / bpr) * bsz
            ri += 1
         }
      }
   }
   gui.set_scroll_area_content_hint("maps_scroll", total)
   ;; F5: open the group holding the focus address and scroll to it.
   if _focus_pending == 1 { _focus_scroll(bpr, bsz, h - px(46.0)) }
   gui.begin_scroll_area("maps_scroll", w - px(16.0), h - px(46.0))
   def list origin = gui.last_item_rect()
   def f64 ox = float(origin.get(0, 0.0))
   def f64 oy = float(origin.get(1, 0.0))
   def f64 gx = ox + gut
   def f64 sy = gui.scroll_area_scroll_y()
   def f64 vh = gui.scroll_area_visible_h()
   def f64 top = oy + sy
   def f64 bottom = top + vh
   def list mp = gui.mouse_xy()
   def f64 mx = float(mp.get(0, -1.0))
   def f64 my = float(mp.get(1, -1.0))
   def bool lclick = _lclick
   def bool rclick = _rclick
   mut int hover_addr = -1
   mut f64 hover_x = 0.0
   mut f64 hover_y = 0.0
   mut f64 cy = oy
   gi = 0
   while gi < _groups.len {
      def any g = _groups.get(gi, 0)
      gi += 1
      if !bool(g.get("enabled", true)) { continue }
      def str name = to_str(g.get("name", ""))
      def bool open = _is_expanded(name)
      if cy + GROUP_H() >= top && cy <= bottom {
         def bool hot = mx >= ox && mx < ox + aw && my >= cy && my < cy + GROUP_H()
         mut list head_col = [0.12, 0.12, 0.15, 1.0]
         if hot { head_col = [0.20, 0.17, 0.29, 1.0] }
         gui.paint_rect(ox, cy + px(1.0), aw, GROUP_H() - px(3.0), head_col)
         mut str arrow = ">"
         if open { arrow = "v" }
         gui.paint_text(ox + px(10.0), cy + px(5.0), arrow + "  " + name, [0.90, 0.90, 0.95, 1.0])
         gui.paint_text(ox + aw - px(110.0), cy + px(6.0), to_str(g.get("regions", []).len) + " regions", [0.55, 0.55, 0.62, 1.0], true)
         if hot && lclick {
            mut str nv = "1"
            if open { nv = "" }
            _expanded[name] = nv
         }
      }
      cy = cy + GROUP_H()
      if !open { continue }
      def any regs = g.get("regions", [])
      mut int ri = 0
      while ri < regs.len {
         def any r = regs.get(ri, 0)
         ri += 1
         def int start = int(r.get(0, 0))
         def int size = int(r.get(1, 0)) - start
         def int rows = (size + bpr - 1) / bpr
         if cy + REGION_H() >= top && cy <= bottom {
            gui.paint_text(ox + px(6.0), cy + px(3.0), "0x" + str.to_hex(start, 0) + " - 0x" + str.to_hex(start + size, 0) + "    " + to_str(size / 1024) + " KiB", [0.62, 0.62, 0.70, 1.0], true)
            ;; Offset ruler: every column when the cells hold text, else every 8.
            mut int c = 0
            while c < bpr {
               if show_hex || c % 8 == 0 {
                  gui.paint_text(gx + float(c) * bsz + px(2.0), cy + px(20.0), "+" + str.to_hex(c, 0), [0.42, 0.42, 0.50, 1.0], true)
               }
               c += 1
            }
         }
         cy = cy + REGION_H()
         def f64 block_h = float(rows) * bsz
         if cy + block_h >= top && cy <= bottom {
            mut int r0 = int(floor((top - cy) / bsz))
            if r0 < 0 { r0 = 0 }
            mut int r1 = int(floor((bottom - cy) / bsz)) + 1
            if r1 > rows { r1 = rows }
            def int a0 = start + r0 * bpr
            def int len = min(size - r0 * bpr, (r1 - r0) * bpr)
            mut ptr b = 0
            if len > 0 { b = ms.read_bytes(a0, len) }
            ;; Row addresses: every row when they fit, otherwise every few rows.
            def int label_every = max(1, int(ceil(px(15.0) / bsz)))
            mut int row = r0
            while row < r1 {
               def f64 ry = cy + float(row) * bsz
               def int rstart = row * bpr
               def int rlen = int(min(bpr, size - rstart))
               if gut > px(20.0) && row % label_every == 0 {
                  def f64 label_dy = float(max(0.0, (bsz - px(15.0)) * 0.5))
                  gui.paint_text(ox + px(6.0), ry + label_dy, str.to_hex(start + rstart, 12), [0.50, 0.52, 0.60, 1.0], true)
               }
               mut int k = 0
               while k < rlen {
                  def f64 cx = gx + float(k) * bsz
                  if b {
                     def int bv = load8(b, (row - r0) * bpr + k) & 255
                     gui.paint_rect(cx, ry, bsz - 1.0, bsz - 1.0, _byte_color(bv))
                     if show_hex { gui.paint_text(cx + (bsz - px(16.0)) * 0.5, ry + (bsz - px(15.0)) * 0.5, str.to_hex(bv, 2), _ink_for(bv), true) }
                  } else {
                     gui.paint_rect(cx, ry, bsz - 1.0, bsz - 1.0, [0.22, 0.06, 0.06, 1.0])
                  }
                  k += 1
               }
               if my >= ry && my < ry + bsz && mx >= gx && mx < gx + float(rlen) * bsz {
                  def int col = int(floor((mx - gx) / bsz))
                  hover_addr = start + rstart + col
                  hover_x = gx + float(col) * bsz
                  hover_y = ry
               }
               if _focus_addr >= start + rstart && _focus_addr < start + rstart + rlen {
                  def f64 fx = gx + float(_focus_addr - start - rstart) * bsz
                  _outline(fx, ry, bsz - 1.0, [1.0, 0.75, 0.2, 1.0])
               }
               row += 1
            }
            if b { free(b) }
         }
         cy = cy + block_h
      }
   }
   gui.spacer_px(total)
   if hover_addr >= 0 {
      _outline(hover_x, hover_y, bsz - 1.0, [1.0, 1.0, 1.0, 1.0])
      if rclick { copy_addr(hover_addr) }
      _tooltip(mx, my, _value_lines(hover_addr, "region bytes"))
   }
   gui.end_scroll_area()
   gui.end_window()
   0
}

fn _outline(f64 x, f64 y, f64 s, list col) any {
   def f64 t = max(1.0, px(1.5))
   gui.paint_rect(x - t, y - t, s + t * 2.0, t, col)
   gui.paint_rect(x - t, y + s, s + t * 2.0, t, col)
   gui.paint_rect(x - t, y, t, s, col)
   gui.paint_rect(x + s, y, t, s, col)
   0
}

fn _focus_scroll(int bpr, f64 bsz, f64 vh) any {
   _focus_pending = 0
   mut f64 cy = 0.0
   mut int gi = 0
   while gi < _groups.len {
      def any g = _groups.get(gi, 0)
      gi += 1
      if !bool(g.get("enabled", true)) { continue }
      def str name = to_str(g.get("name", ""))
      def any regs = g.get("regions", [])
      mut int hit = -1
      mut int ri = 0
      while ri < regs.len {
         def any r = regs.get(ri, 0)
         if _focus_addr >= int(r.get(0, 0)) && _focus_addr < int(r.get(1, 0)) { hit = ri }
         ri += 1
      }
      cy = cy + GROUP_H()
      if hit >= 0 {
         _expanded[name] = "1"
         ri = 0
         while ri < hit {
            def any r = regs.get(ri, 0)
            cy = cy + REGION_H() + float((int(r.get(1, 0)) - int(r.get(0, 0)) + bpr - 1) / bpr) * bsz
            ri += 1
         }
         def any r = regs.get(hit, 0)
         def int row = (_focus_addr - int(r.get(0, 0))) / bpr
         def f64 at = cy + REGION_H() + float(row) * bsz
         gui.scroll_area_ensure_visible("maps_scroll", at - vh * 0.4, at + bsz, vh)
         return 0
      }
      if _is_expanded(name) {
         ri = 0
         while ri < regs.len {
            def any r = regs.get(ri, 0)
            cy = cy + REGION_H() + float((int(r.get(1, 0)) - int(r.get(0, 0)) + bpr - 1) / bpr) * bsz
            ri += 1
         }
      }
   }
   _set_error("0x" + str.to_hex(_focus_addr, 0) + " is not in an enabled mapping")
   0
}

;; ---- keys ----------------------------------------------------------------------

fn on_key(int k, int mods=0) any {
   def bool typing = gui.wants_keyboard()
   if (mods & keyc.MOD_CONTROL) != 0 {
      if k == keyc.KEY_S { do_save() return 0 }
      if k == keyc.KEY_EQUAL || k == keyc.KEY_KP_ADD { _ui_user = float(clamp(_ui_user * 1.15, 0.75, 3.0)) }
      if k == keyc.KEY_MINUS || k == keyc.KEY_KP_SUBTRACT { _ui_user = float(clamp(_ui_user / 1.15, 0.75, 3.0)) }
      if k == keyc.KEY_0 { _ui_user = 1.0 }
      return 0
   }
   if !typing && k == keyc.KEY_S { _show_scan = !_show_scan }
   if !typing && k == keyc.KEY_R { _show_regions = !_show_regions }
   if !typing && k == keyc.KEY_W { _only_writable = !_only_writable _status = _filter_note() }
   if !typing && k == keyc.KEY_H { _only_shared = !_only_shared _status = _filter_note() }
   if k == keyc.KEY_F1 {
      if ms.attached() { do_detach() } else { do_attach_query() }
   }
   if k == keyc.KEY_F2 { do_scan(to_str(ms.MODES.get(_mode_idx, "Exact"))) }
   if k == keyc.KEY_F3 { do_previous() }
   if k == keyc.KEY_F4 { do_reset() }
   if k == keyc.KEY_F5 { do_focus() }
   if k == keyc.KEY_F7 { do_lock_toggle() }
   if k == keyc.KEY_F8 { do_scan("Changed") }
   if k == keyc.KEY_F9 { do_scan("Increased") }
   if k == keyc.KEY_F10 { do_scan("Decreased") }
   if k == keyc.KEY_F11 { _zoom = clamp(_zoom / 1.25, 0.5, 16.0) }
   if k == keyc.KEY_F12 { _zoom = clamp(_zoom * 1.25, 0.5, 16.0) }
   0
}

fn pump_events() any {
   ;; Keys and clicks drive smem; every event also goes to the toolkit.
   _lclick = false
   _rclick = false
   mut any e = window.check_event(_win)
   while !is_nil(e) && !(is_int(e) && int(e) == 0) {
      def int typ = window.event_type(e)
      def any data = window.event_data(e)
      if typ == keyc.EVENT_KEY_PRESSED { on_key(int(data.get("key", 0)), int(data.get("mod", data.get("mods", 0)))) }
      if typ == keyc.EVENT_MOUSE_BUTTON_PRESSED {
         def int b = int(data.get("button", 0))
         if b == 0 { _lclick = true }
         if b == 1 { _rclick = true }
      }
      gui.feed_event(typ, data)
      e = window.check_event(_win)
   }
   0
}

;; ---- headless commands (EdenScan-style) --------------------------------------

def CLI_COMMANDS = ["regions", "scan", "next", "list", "read", "write", "freeze", "shell", "-i"]

fn _cli_results_text() str {
   ;; One "0xADDR TYPE VALUE" line per current result.
   def int n = ms.count()
   mut out = str.Builder(max(64, n * 24))
   mut int i = 0
   while i < n {
      out = str.builder_append(out, "0x" + str.to_hex(ms.addr_at(i), 0) + " " + ms.type_name(ms.type_at(i)) + " " + ms.value_text(i) + "\n")
      i += 1
   }
   def str text = str.builder_to_str(out)
   str.builder_free(out)
   text
}

fn _cli_opt(list av, int from, str name, str dflt) str {
   ;; Value after --name, or dflt.
   mut int i = from
   while i + 1 < av.len {
      if to_str(av.get(i, "")) == name { return to_str(av.get(i + 1, dflt)) }
      i += 1
   }
   dflt
}

fn _cli_flag(list av, int from, str name) bool {
   mut int i = from
   while i < av.len {
      if to_str(av.get(i, "")) == name { return true }
      i += 1
   }
   false
}

fn _cli_pos(list av, int from, int k) str {
   ;; k-th positional argument after `from`, skipping --options and their values.
   mut int i = from
   mut int seen = 0
   while i < av.len {
      def str a = to_str(av.get(i, ""))
      if str.startswith(a, "--") {
         i += (a == "--writable" || a == "--shared") ? 1 : 2
         continue
      }
      if seen == k { return a }
      seen += 1
      i += 1
   }
   ""
}

fn _cli_addr(str t) int {
   ;; Hex with or without 0x.
   def str h = (str.startswith(t, "0x") || str.startswith(t, "0X")) ? str.str_slice(t, 2, t.len) : t
   if h.len == 0 || !ms.parse_value("hex:" + h) { return -1 }
   ms.parsed_int()
}

fn _cli_type(list av, int from, str dflt) int {
   def str tn = _cli_opt(av, from, "--type", dflt)
   def int t = ms.type_index(tn)
   if t < 0 { print("smem: unknown type " + tn + " (one of " + str.join(ms.TYPE_NAMES, ", ") + ")") }
   t
}

fn _cli_attach(str ps) bool {
   def int p = _parse_pid(ps)
   if p <= 0 { print("smem: bad pid '" + ps + "'") return false }
   if !ms.attach(p) { print("smem: " + ms.last_error()) return false }
   true
}

fn _cli_regions(int p, bool writable, bool shared, str only) list {
   mut out = []
   def any groups = ms.load_maps(p)
   mut int gi = 0
   while gi < groups.len {
      def any g = groups.get(gi, 0)
      gi += 1
      if only.len > 0 && str.find(to_str(g.get("name", "")), only) < 0 { continue }
      def any regs = g.get("regions", [])
      mut int ri = 0
      while ri < regs.len {
         def any r = regs.get(ri, 0)
         ri += 1
         if writable && !ms.region_writable(r) { continue }
         if shared && !ms.region_shared(r) { continue }
         out = out.append(r)
      }
   }
   out
}

fn _cli_num(str text, bool fl) f64 {
   ;; Numeric value of a formatted value (int or float); NaN-free: 0 when unparsable.
   if !ms.parse_value(text) { return 0.0 }
   def bool pf = str.startswith(ms.type_name(ms.parsed_type()), "Float")
   pf ? float(ms.parsed_float()) : float(ms.parsed_int())
}

fn _cli_same(f64 a, f64 b, bool fl) bool {
   if !fl { return a == b }
   def f64 d = a - b
   def f64 m = max(1.0, max(a < 0.0 ? -a : a, b < 0.0 ? -b : b))
   (d < 0.0 ? -d : d) <= 1.0e-6 * m
}

fn _cli_print_some(str text, int n) any {
   ;; Print the lines when there are few, as EdenScan does.
   if n > 0 && n <= 20 { print(str.strip(text)) }
   0
}

fn _cli_save(str file, str text) bool {
   if file.len == 0 { return true }
   if is_err(file_write(file, text)) { print("smem: cannot write " + file) return false }
   true
}

;; ---- interactive shell (scanmem-style) ----------------------------------------

def SHELL_HELP = "  pid N              attach to process N
  lregions           list scannable regions with ids
  dregion [!]ID,...  disable regions (! re-enables); dregion all / !all
  option type T      value type for scans: Auto Int8..UInt64 Float32 Float64 ...
  option filter F    regions: all | writable | shared
  N                  exact scan for N (first scan or narrow the results)
  = [N]  != [N]      unchanged / changed since the last scan (= N is exact)
  > [N]  < [N]       increased / decreased (with N: greater / less or equal steps)
  + N    - N         increased by N / decreased by N
  snapshot           record every byte, then narrow with = != > <
  undo  reset        go back one scan level / drop all levels
  list [N]           show the first N results (default 20)
  count              number of results
  set [ID] V         write V to every result, or to result ID
  read ADDR [T]      read one value (default Int32)
  write ADDR V [T]   write one value
  dump ADDR LEN      hex dump
  freeze ADDR V [MS] [T]  rewrite V every MS ms until 100 writes or Ctrl+C
  save FILE          write the results as 0xADDR TYPE VALUE lines
  help  exit"

mut str _sh_type = "Auto"
mut str _sh_filter = "all"
mut any _sh_off = nil

fn _cli_readline() str {
   ;; One line from stdin without the newline; "\x04" at end of input.
   def ptr b = malloc(1)
   mut out = str.Builder(64)
   mut bool got = false
   while true {
      def res = sys.sys_read(0, b, 1)
      if is_err(res) || unwrap(res) <= 0 { break }
      got = true
      def int c = load8(b, 0) & 255
      if c == 10 { break }
      if c != 13 { out = str.builder_append(out, chr(c)) }
   }
   free(b)
   def str line = str.builder_to_str(out)
   str.builder_free(out)
   (!got && line.len == 0) ? "\x04" : line
}

fn _sh_regions() list {
   def list all = _cli_regions(ms.pid(), _sh_filter == "writable", _sh_filter == "shared", "")
   mut out = []
   mut int i = 0
   while i < all.len {
      if !(is_dict(_sh_off) && int(_sh_off.get(to_str(i), 0)) == 1) { out = out.append(all.get(i, 0)) }
      i += 1
   }
   out
}

fn _sh_scan(str mode, str value) any {
   if !ms.attached() { print("attach first: pid N") return 0 }
   def list regs = ms.depth() == 0 ? _sh_regions() : []
   if ms.depth() == 0 && regs.len == 0 { print("no enabled region") return 0 }
   def int t0 = ticks_ms()
   if !ms.scan(regs, value, mode, _sh_type) { print("error: " + ms.last_error()) return 0 }
   if ms.is_snapshot() { print("snapshot: " + to_str(ms.snapshot_bytes() / 1024) + " KiB (" + to_str(ticks_ms() - t0) + " ms)") }
   else { print("info: " + to_str(ms.count()) + " matches" + (ms.truncated() ? " (limit reached)" : "") + " (" + to_str(ticks_ms() - t0) + " ms)") }
   0
}

fn _sh_list(int n) any {
   def int c = ms.count()
   mut int i = 0
   while i < c && i < n {
      print("[" + to_str(i) + "] 0x" + str.to_hex(ms.addr_at(i), 0) + " " + ms.type_name(ms.type_at(i)) + " " + ms.value_text(i))
      i += 1
   }
   if c > n { print("... " + to_str(c - n) + " more") }
   0
}

fn _sh_dump(int addr, int len) any {
   mut int n = len
   if n < 1 { n = 1 }
   if n > 65536 { n = 65536 }
   def ptr b = ms.read_bytes(addr, n)
   if !b { print("error: cannot read 0x" + str.to_hex(addr, 0)) return 0 }
   mut int o = 0
   while o < n {
      mut str hex = ""
      mut str asc = ""
      mut int k = 0
      while k < 16 && o + k < n {
         def int v = load8(b, o + k) & 255
         hex = hex + (v < 16 ? "0" : "") + str.to_hex(v, 0) + " "
         asc = asc + ((v >= 32 && v < 127) ? chr(v) : ".")
         k += 1
      }
      while k < 16 { hex = hex + "   " k += 1 }
      print("0x" + str.to_hex(addr + o, 0) + "  " + hex + " " + asc)
      o += 16
   }
   free(b)
   0
}

fn _sh_type_of(str tn, str dflt) int {
   def int t = ms.type_index(tn.len > 0 ? tn : dflt)
   if t < 0 { print("unknown type " + tn) }
   t
}

fn cli_shell(int start_pid) int {
   _sh_off = dict(16)
   if start_pid > 0 && !ms.attach(start_pid) { print("error: " + ms.last_error()) }
   print("smem shell: `help` lists commands")
   while true {
      def str prompt = to_str(ms.attached() ? ms.count() : 0) + "> "
      sys.sys_write(1, cptr(prompt), prompt.len)
      def str line = str.strip(_cli_readline())
      if line == "\x04" { print("") break }
      if line.len == 0 { continue }
      def list w = line.split(" ")
      def str c = to_str(w.get(0, ""))
      def str a1 = to_str(w.get(1, ""))
      def str a2 = to_str(w.get(2, ""))
      def str a3 = to_str(w.get(3, ""))
      if c == "exit" || c == "quit" || c == "q" { break }
      elif c == "help" || c == "?" { print(SHELL_HELP) }
      elif c == "pid" {
         def int p = _parse_pid(a1)
         ms.lock_stop() ms.reset() _sh_off = dict(16)
         if p > 0 && ms.attach(p) { print("attached to " + to_str(p) + " " + _comm(p)) } else { print("error: " + ms.last_error()) }
      }
      elif !ms.attached() { print("attach first: pid N") }
      elif c == "lregions" {
         def list all = _cli_regions(ms.pid(), _sh_filter == "writable", _sh_filter == "shared", "")
         mut int i = 0
         while i < all.len {
            def any r = all.get(i, 0)
            def int lo, hi = int(r.get(0, 0)), int(r.get(1, 0))
            def str off = int(_sh_off.get(to_str(i), 0)) == 1 ? " (disabled)" : ""
            print("[" + to_str(i) + "] 0x" + str.to_hex(lo, 0) + " " + to_str((hi - lo) / 1024) + " KiB " + to_str(r.get(2, "")) + off)
            i += 1
         }
      }
      elif c == "dregion" {
         mut str spec = a1
         def bool on = str.startswith(spec, "!")
         if on { spec = str.str_slice(spec, 1, spec.len) }
         if spec == "all" {
            def int n = _cli_regions(ms.pid(), _sh_filter == "writable", _sh_filter == "shared", "").len
            mut int i = 0
            while i < n { _sh_off[to_str(i)] = on ? 0 : 1 i += 1 }
         } else {
            def list ids = spec.split(",")
            mut int i = 0
            while i < ids.len { _sh_off[to_str(_parse_pid(to_str(ids.get(i, ""))))] = on ? 0 : 1 i += 1 }
         }
         print(on ? "enabled" : "disabled")
      }
      elif c == "option" {
         if a1 == "type" && ms.type_index(a2) >= 0 || a1 == "type" && a2 == "Auto" { _sh_type = a2 print("type " + a2) }
         elif a1 == "filter" && (a2 == "all" || a2 == "writable" || a2 == "shared") { _sh_filter = a2 print("filter " + a2 + " (applies to the next first scan)") }
         else { print("option type <" + str.join(ms.TYPE_NAMES, "|") + "|Auto> | option filter <all|writable|shared>") }
      }
      elif c == "snapshot" { if ms.depth() > 0 { print("reset first") } else { _sh_scan("Changed", "") } }
      elif c == "=" { _sh_scan(a1.len > 0 ? "Exact" : "Unchanged", a1) }
      elif c == "!=" { _sh_scan("Changed", a1) }
      elif c == ">" { _sh_scan(a1.len > 0 ? "Increased or Greater" : "Increased", a1) }
      elif c == "<" { _sh_scan(a1.len > 0 ? "Decreased or Less" : "Decreased", a1) }
      elif c == "+" { _sh_scan("Increased by", a1) }
      elif c == "-" { _sh_scan("Decreased by", a1) }
      elif c == "undo" { if ms.previous() { print(to_str(ms.count()) + " results") } else { print("nothing to undo") } }
      elif c == "reset" { ms.lock_stop() ms.reset() print("reset") }
      elif c == "count" { print(to_str(ms.count())) }
      elif c == "list" { _sh_list(a1.len > 0 ? _parse_pid(a1) : 20) }
      elif c == "set" {
         def bool one = a2.len > 0
         def bool ok = one ? ms.set_one(_parse_pid(a1), a2) : ms.set_all(a1)
         print(ok ? "ok" : "error: " + ms.last_error())
      }
      elif c == "read" {
         def int t = _sh_type_of(a2, "Int32")
         def int ad = _cli_addr(a1)
         if t >= 0 && ad >= 0 { print("0x" + str.to_hex(ad, 0) + " " + ms.type_name(t) + " " + ms.read_value(ad, t)) }
      }
      elif c == "write" {
         def int t = _sh_type_of(a3, "Int32")
         def int ad = _cli_addr(a1)
         if t >= 0 && ad >= 0 { print(ms.write_value(ad, a2, t) ? "ok" : "error: " + ms.last_error()) }
      }
      elif c == "dump" { _sh_dump(_cli_addr(a1), a2.len > 0 ? _parse_pid(a2) : 64) }
      elif c == "freeze" {
         def int t = _sh_type_of(to_str(w.get(4, "")), "Int32")
         def int ad = _cli_addr(a1)
         def int every = a3.len > 0 ? max(1, _parse_pid(a3)) : 100
         mut int k = 0
         while t >= 0 && ad >= 0 && k < 100 {
            if !ms.write_value(ad, a2, t) { print("error: " + ms.last_error()) break }
            msleep(every)
            k += 1
         }
         print("freeze done (" + to_str(k) + " writes)")
      }
      elif c == "save" { print(_cli_save(a1, _cli_results_text()) ? "saved " + to_str(ms.count()) + " results" : "save failed") }
      elif _cli_addr(c) >= 0 && ms.parse_value(c) { _sh_scan("Exact", c) }
      else { print("unknown command '" + c + "' (help)") }
   }
   ms.detach_process()
   0
}

fn cli_main(list av) int {
   ;; Returns -1 when av holds no command (the window runs instead).
   mut int ci = -1
   mut int i = 1
   while i < av.len {
      def str w0 = to_str(av.get(i, ""))
      mut int c = 0
      while c < CLI_COMMANDS.len {
         if to_str(CLI_COMMANDS.get(c, "")) == w0 { ci = i }
         c += 1
      }
      if ci >= 0 { break }
      i += 1
   }
   if ci < 0 { return -1 }
   def str cmd = to_str(av.get(ci, ""))
   def int a = ci + 1
   if cmd == "shell" || cmd == "-i" { return cli_shell(_parse_pid(_cli_pos(av, a, 0))) }
   if cmd == "list" {
      def res = file_read(_cli_pos(av, a, 0))
      if is_err(res) { print("smem: cannot read " + _cli_pos(av, a, 0)) return 1 }
      print(str.strip(to_str(unwrap(res))))
      return 0
   }
   if !_cli_attach(_cli_pos(av, a, 0)) { return 1 }
   def int p = ms.pid()
   def bool w = _cli_flag(av, a, "--writable")
   def bool sh = _cli_flag(av, a, "--shared")
   if cmd == "regions" {
      def list regs = _cli_regions(p, w, sh, _cli_opt(av, a, "--region", ""))
      mut int total = 0
      mut int k = 0
      while k < regs.len {
         def any r = regs.get(k, 0)
         def int lo, hi = int(r.get(0, 0)), int(r.get(1, 0))
         total += hi - lo
         print("0x" + str.to_hex(lo, 0) + "-0x" + str.to_hex(hi, 0) + " " + to_str(r.get(2, "")) + " " + to_str((hi - lo) / 1024) + " KiB")
         k += 1
      }
      print(to_str(regs.len) + " regions, " + to_str(total / 1024) + " KiB")
      return 0
   }
   if cmd == "scan" {
      def int t = _cli_type(av, a, "Auto")
      if t < 0 && _cli_opt(av, a, "--type", "Auto") != "Auto" { return 2 }
      def list regs = _cli_regions(p, w, sh, _cli_opt(av, a, "--region", ""))
      if regs.len == 0 { print("smem: no region matches the filters") return 1 }
      if !ms.scan(regs, _cli_pos(av, a, 1), "Exact", _cli_opt(av, a, "--type", "Auto")) { print("smem: " + ms.last_error()) return 1 }
      def str text = _cli_results_text()
      print(to_str(ms.count()) + " candidates" + (ms.truncated() ? " (stopped at the limit)" : ""))
      _cli_print_some(text, ms.count())
      return _cli_save(_cli_opt(av, a, "--out", ""), text) ? 0 : 1
   }
   if cmd == "next" {
      def str inf = _cli_opt(av, a, "--in", "")
      def res = file_read(inf)
      if is_err(res) { print("smem: next needs --in FILE from scan") return 1 }
      def str how = str.lower(_cli_pos(av, a, 1))
      def bool rel = how == "changed" || how == "unchanged" || how == "increased" || how == "decreased"
      mut out = str.Builder(4096)
      mut int kept = 0
      def lines = to_str(unwrap(res)).split("\n")
      mut int li = 0
      while li < lines.len {
         def parts = str.strip(to_str(lines.get(li, ""))).split(" ")
         li += 1
         if parts.len < 2 { continue }
         def int addr = _cli_addr(to_str(parts.get(0, "")))
         def int t = ms.type_index(to_str(parts.get(1, "")))
         if addr < 0 || t < 0 { continue }
         def str cur = ms.read_value(addr, t)
         if cur == "??" { continue }
         def bool fl = str.startswith(ms.type_name(t), "Float")
         def f64 now = _cli_num(cur, fl)
         mut bool keep = false
         if rel {
            def f64 was = _cli_num(to_str(parts.get(2, "")), fl)
            if how == "changed" { keep = !_cli_same(now, was, fl) }
            elif how == "unchanged" { keep = _cli_same(now, was, fl) }
            elif how == "increased" { keep = now > was && !_cli_same(now, was, fl) }
            else { keep = now < was && !_cli_same(now, was, fl) }
         } else {
            keep = _cli_same(now, _cli_num(how, fl), fl)
         }
         if keep {
            out = str.builder_append(out, to_str(parts.get(0, "")) + " " + to_str(parts.get(1, "")) + " " + cur + "\n")
            kept += 1
         }
      }
      def str text = str.builder_to_str(out)
      str.builder_free(out)
      print(to_str(kept) + " candidates")
      _cli_print_some(text, kept)
      return _cli_save(_cli_opt(av, a, "--out", inf), text) ? 0 : 1
   }
   def int addr = _cli_addr(_cli_pos(av, a, 1))
   if addr < 0 { print("smem: bad address '" + _cli_pos(av, a, 1) + "'") return 2 }
   def int t = _cli_type(av, a, "Int32")
   if t < 0 { return 2 }
   if cmd == "read" {
      def str v = ms.read_value(addr, t)
      print("0x" + str.to_hex(addr, 0) + " " + ms.type_name(t) + " " + v)
      return v == "??" ? 1 : 0
   }
   def str value = _cli_pos(av, a, 2)
   if !ms.write_value(addr, value, t) { print("smem: " + ms.last_error()) return 1 }
   if cmd == "write" { print("wrote " + value + " at 0x" + str.to_hex(addr, 0)) return 0 }
   ;; freeze: rewrite every --every ms; --for MS stops (otherwise until killed).
   def int every = max(1, _parse_pid(_cli_opt(av, a, "--every", "100")))
   def int dur = _parse_pid(_cli_opt(av, a, "--for", "0"))
   print("freezing 0x" + str.to_hex(addr, 0) + " at " + value + " every " + to_str(every) + " ms" + (dur > 0 ? " for " + to_str(dur) + " ms" : " (Ctrl+C stops)"))
   def int t0 = ticks_ms()
   while dur <= 0 || ticks_ms() - t0 < dur {
      if !ms.write_value(addr, value, t) { print("smem: " + ms.last_error()) return 1 }
      msleep(every)
   }
   0
}

;; ---- main ----------------------------------------------------------------------

fn main() int {
   mut int start_pid = 0
   def av = args.args()
   mut int i = av.len - 1
   while i >= 1 {
      def int p = _parse_pid(to_str(av.get(i, "")))
      if p > 0 {
         start_pid = p
         break
      }
      i -= 1
   }
   def int cli_rc = cli_main(av)
   if cli_rc >= 0 { return cli_rc }
   if common.env_truthy("NY_UI_HEADLESS") { gfx.set_backend_type(gfx.BACKEND_MOCK) }
   else {
      gfx.apply_backend_env()
      gfx.apply_backend_argv()
   }
   _win = gfx.init_window(1180, 760, "smem", keyc.WINDOW_CENTER | keyc.WINDOW_FOCUS_ON_SHOW, false, true, 1)
   if !_win {
      print("smem: no window: " + gfx.window_error())
      return 1
   }
   window.set_exit_key(_win, keyc.KEY_NULL)
   gui.set_enabled(true)
   def str scale_env = common.env_trim("NY_UI_SCALE")
   if scale_env.len > 0 { _ui_user = float(clamp(float(str.atof(scale_env)), 0.75, 3.0)) }
   update_scale()
   refresh_pids()
   _expanded = dict(64)
   if start_pid > 0 {
      _pid_query = to_str(start_pid)
      do_attach(start_pid)
   }
   ;; Headless runs (CI) draw a couple of frames unless a replay drives them.
   def int backend = gfx.get_backend_type()
   def int mock = gfx.BACKEND_MOCK
   def bool headless = backend == mock && !common.env_truthy("NYTRIX_UI_REPLAY")
   mut int frames = 0
   while !gfx.window_should_close(_win) {
      if headless && frames >= 2 { break }
      pump_events()
      update_scale()
      ms.lock_tick()
      def fb = gfx.framebuffer_size_f64(1180, 760)
      def f64 ww = max(480.0, float(fb.get(0, 1180)))
      def f64 wh = max(320.0, float(fb.get(1, 760)))
      if !gfx.begin_frame_clear(gfx.color_hex("#060607")) { continue }
      gui.prepare_input(_win, ww, wh)
      gfx.set_ortho_2d(0.0, ww, wh, 0.0)
      gui.begin_frame(0, _font, ww, wh)
      def f64 top = draw_topbar(ww)
      mut f64 left = 0.0
      if _show_regions && ms.attached() { left = float(min(px(280.0), ww * 0.34)) }
      def f64 body_h = wh - top
      if left > 0.0 { draw_regions(0.0, top, left, body_h) }
      if ms.attached() && ms.count() > 0 { draw_results(left, top, ww - left, body_h) }
      else { draw_maps(left, top, ww - left, body_h) }
      draw_tooltip(ww, wh)
      gui.end_frame()
      gfx.end_frame()
      if common.env_truthy("NYTRIX_UI_REPLAY") { print("NYTRIX_UI_STATE frame=" + to_str(frames) + " attached=" + to_str(ms.attached()) + " results=" + to_str(ms.count()) + " focus=" + gui.focused_id() + " value=" + _scan_value + " mouse=" + to_str(gui.mouse_xy())) }
      frames += 1
   }
   ms.detach_process()
   gfx.close_window()
   0
}

;; The runtime calls main(); calling it here as well runs the app twice.
