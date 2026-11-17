#!/usr/bin/env ny

;; Keywords: ui gui toolkit widgets gallery example menus tables tree popup
;; Gallery of the immediate-mode toolkit: every widget the editor and the
;; engine build on, in one window. The panels live in a dockspace: drag a
;; title bar or a tab to rearrange them, drag a splitter to resize.
;;
;;   NY_WIDGETS_THEME=light|dark    palette (F2 toggles at run time)
;;   NY_WIDGETS_SCALE=1.5           UI scale (F3 / F4 step it)
;;   NY_WIDGETS_SCRIPT="3:move:60:20;4:down;5:up;9:key:258"
;;       frame:op[:a[:b]] steps fed to the GUI as synthetic input, so a
;;       capture can show a chosen state. Ops: move x y, down, up, rdown,
;;       key code, char code.
;;   NY_WIDGETS_SIZE=700x900       initial window size
;;   NY_WIDGETS_TAB=0|1|2           initial tab of the Controls panel
;;   NY_WIDGETS_PANEL_STATE=empty|loading|error   state shown in the Assets panel
;;   NY_WIDGETS_LAYOUT=path         load the dock layout from this file at start
;;                                  and save it there on exit
;;   NY_WIDGETS_TILE=1              fixed tiled panels instead of the dockspace
;;   NY_WIDGETS_FRAMES=N            exit after N frames (0 = run until closed)
;;   NY_WIDGETS_SNAPSHOT=path.png   write the framebuffer on the last frame
use core
use core.common as common
use core.os (exit, file_read, file_write)
use core.str as str
use core.math (max, min)
use core.os.ui.window.consts
use core.os.ui.render
use core.os.ui.render.viewer
use core.os.ui.render.viewer.gui as gui
use core.os.ui.window

mut START_W = 1120
mut START_H = 720
def env_size = str.split(common.env_trim("NY_WIDGETS_SIZE"), "x")
if env_size.len == 2 {
   def ew, eh = int(str.atof(to_str(env_size.get(0, "0")))), int(str.atof(to_str(env_size.get(1, "0"))))
   if ew >= 320 && eh >= 240 {
      START_W = ew
      START_H = eh
   }
}
render.apply_backend_env()
render.apply_backend_argv()
mut win = render.init_window(START_W, START_H, "Nytrix Widgets", consts.WINDOW_CENTER | consts.WINDOW_FOCUS_ON_SHOW, false, true, 1)
if !win { panic("window init failed") }
window.set_exit_key(win, consts.KEY_NULL)

mut ui_scale = 1.0
def env_scale = str.atof(common.env_trim("NY_WIDGETS_SCALE"))
if env_scale >= 0.5 { ui_scale = env_scale }

;; Fonts are rasterised at the size they appear on screen (the UI scale times
;; the window's content scale, 2 on a HiDPI output), so text stays crisp and
;; fills its rows; F3 / F4 and scale changes reload them.
def fonts = viewer.FONT_CANDIDATES
mut font_body, font_title, font_small = 0, 0, 0
mut font_scale = 0.0
fn load_fonts(f64 s) any {
   if font_scale == s { return 0 }
   font_body = render.font_load_first(fonts, int(15.0 * s + 0.5))
   font_title = render.font_load_first(fonts, int(16.0 * s + 0.5))
   font_small = render.font_load_first(fonts, int(13.0 * s + 0.5))
   font_scale = s
}
load_fonts(ui_scale)
if common.env_trim("NY_WIDGETS_THEME") == "light" { gui.set_theme("light") }
def frame_limit = int(str.atof(common.env_trim("NY_WIDGETS_FRAMES")))
def snapshot_path = common.env_trim("NY_WIDGETS_SNAPSHOT")

;; Scripted input: "frame:op:a:b" steps, flattened to [frame, op, a, b, ...].
mut script = []
fn load_script() any {
   def steps = str.split(common.env_trim("NY_WIDGETS_SCRIPT"), ";")
   mut i = 0
   while i < steps.len {
      def parts = str.split(to_str(steps.get(i, "")), ":")
      if parts.len >= 2 {
         script = script.append(int(str.atof(to_str(parts.get(0, "0")))))
         script = script.append(to_str(parts.get(1, "")))
         script = script.append(parts.len > 2 ? str.atof(to_str(parts.get(2, "0"))) : 0.0)
         script = script.append(parts.len > 3 ? str.atof(to_str(parts.get(3, "0"))) : 0.0)
      }
      i += 1
   }
}
load_script()

mut script_x, script_y = 0.0, 0.0
mut script_has_pos = false
fn run_script(int frame) any {
   mut i = 0
   while i + 3 < script.len {
      def int at = int(script.get(i, -1))
      if at == frame {
         def int i1, i2 = i + 1, i + 2
         def int i3 = i + 3
         def op = to_str(script.get(i1, ""))
         def a, b = float(script.get(i2, 0.0)), float(script.get(i3, 0.0))
         if op == "move" {
            script_x, script_y = a, b
            script_has_pos = true
         } elif op == "down" { gui.feed_event(EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": script_x, "y": script_y}) }
         elif op == "up" { gui.feed_event(EVENT_MOUSE_BUTTON_RELEASED, {"button": 0, "x": script_x, "y": script_y}) }
         elif op == "rdown" { gui.feed_event(EVENT_MOUSE_BUTTON_PRESSED, {"button": 1, "x": script_x, "y": script_y}) }
         elif op == "key" { gui.feed_event(EVENT_KEY_PRESSED, {"key": int(a), "mod": 0}) }
         elif op == "char" { gui.feed_event(EVENT_KEY_CHAR, {"char": int(a)}) }
      }
      i += 4
   }
   ;; A scripted pointer stays where it was put until the script moves it.
   if script_has_pos { gui.feed_event(EVENT_MOUSE_POS_CHANGED, {"x": script_x, "y": script_y}) }
}

;; Demo state.
mut check_a, check_b, toggle_a = true, false, true
mut radio = 1
mut combo = 2
mut tab = 0
def env_tab = common.env_trim("NY_WIDGETS_TAB")
if env_tab == "1" { tab = 1 }
if env_tab == "2" { tab = 2 }
mut slider = 0.35
mut count = 12
mut gain = 0.5
mut ratio = 42
mut name = "cube.glb"
mut bad_name = "no such file"
mut picked = [0.71, 0.63, 1.0, 1.0]
mut log_line = "Ready"
mut selected_row = 1
mut tree_sel = "mesh"
mut slots = ["Albedo", "Normal", "Roughness"]
mut slot_map = ["", "", ""]
mut clicks = 0
mut panel_demo = common.env_trim("NY_WIDGETS_PANEL_STATE")

fn say(str msg) any {
   log_line = msg
   print("WIDGETS_EVENT " + msg)
}

fn draw_menu_bar() any {
   if !gui.begin_menu_bar() { return 0 }
   if gui.begin_menu("file", "File") {
      if gui.menu_item("new", "New Scene", "Ctrl+N") { say("file.new") }
      if gui.menu_item("open", "Open...", "Ctrl+O") { say("file.open") }
      if gui.begin_menu("recent", "Open Recent") {
         if gui.menu_item("r0", "sponza.glb") { say("recent.sponza") }
         if gui.menu_item("r1", "helmet.glb") { say("recent.helmet") }
         gui.end_menu()
      }
      gui.menu_separator()
      gui.menu_item("save", "Save", "Ctrl+S", false, false)
      if gui.menu_item("quit", "Quit", "Ctrl+Q") { window.set_should_close(win, true) }
      gui.end_menu()
   }
   if gui.begin_menu("view", "View") {
      if gui.menu_item("light", "Light Theme", "F2", gui.theme() == "light") { gui.set_theme(gui.theme() == "light" ? "dark" : "light") }
      if gui.menu_item("grid", "Show Grid", "", check_a) { check_a = !check_a }
      gui.end_menu()
   }
   if gui.begin_menu("help", "Help") {
      if gui.menu_item("about", "About") { gui.open_popup("about") }
      gui.end_menu()
   }
   gui.end_menu_bar()
}

fn draw_controls(f64 x, f64 y, f64 w, f64 h) any {
   if gui.begin_window("controls", "Controls", x, y, w, h, {"closable": false, "collapsible": false}) {
      draw_menu_bar()
      tab = gui.tab_strip("tabs", ["Basics", "Numbers", "State"], tab)
      if tab == 0 {
         gui.layout_row([0.0, 0.0, 0.0])
         if gui.button("run", "Run") {
            clicks += 1
            say("button.run")
         }
         gui.tooltip("Runs the selected scene")
         if gui.button("stop", "Stop") { say("button.stop") }
         if gui.begin_context_menu("stop_ctx") {
            if gui.menu_item("force", "Force Stop") { say("ctx.force") }
            if gui.menu_item("restart", "Restart") { say("ctx.restart") }
            gui.end_menu()
         }
         if gui.button("more", "More...") { gui.open_popup("more") }
         if gui.begin_popup("more", 220.0) {
            gui.text("Popup content")
            gain = gui.slider_float("pg", "Gain", gain, 0.0, 1.0)
            if gui.button("close", "Close") { gui.close_popup() }
            gui.end_popup()
         }
         check_a = gui.checkbox("ca", "Show grid", check_a)
         check_b = gui.checkbox("cb", "Wireframe", check_b)
         toggle_a = gui.toggle("ta", "Shadows", toggle_a)
         if gui.radio_button("r0", "Translate", radio == 0) { radio = 0 }
         if gui.radio_button("r1", "Rotate", radio == 1) { radio = 1 }
         if gui.radio_button("r2", "Scale", radio == 2) { radio = 2 }
         combo = gui.combo_box("cmb", "Tonemap", ["Linear", "Reinhard", "ACES", "AgX"], combo)
         name = gui.input_text("name", "Asset", name, "file name")
      } elif tab == 1 {
         slider = gui.slider_float("sl", "Exposure", slider, 0.0, 1.0)
         count = gui.slider_int("si", "Samples", count, 1, 64)
         gain = gui.drag_float("dg", "Gain (drag, or double click to type)", gain, 0.005, 0.0, 2.0, 3)
         ratio = gui.drag_int("dr", "Ratio", ratio, 0.25, 0, 100)
         gui.progress_bar("pb", slider, "Bake")
         picked = gui.color_edit4("col", "Tint", picked)
      } else {
         gui.begin_disabled(true)
         gui.button("dis_b", "Disabled button")
         gui.checkbox("dis_c", "Disabled choice", true)
         gui.slider_float("dis_s", "Disabled slider", 0.4, 0.0, 1.0)
         gui.end_disabled()
         bad_name = gui.input_text("bad", "Texture", bad_name, "")
         gui.item_status("error", "File not found")
         gui.button("warned", "Rebuild lighting")
         gui.item_status("warn", "Lighting is stale")
         gui.text_colored("3 assets loaded", "ok")
         gui.text_colored("1 asset missing", "danger")
         gui.text_colored("Shader cache is cold", "warn")
      }
      if gui.begin_popup("about", 260.0) {
         gui.text("Nytrix toolkit gallery")
         gui.text_colored("Escape or a press outside closes this.", "text_dim")
         gui.end_popup()
      }
   }
   gui.end_window()
}

fn draw_tree(f64 x, f64 y, f64 w, f64 h) any {
   if gui.begin_window("scene", "Scene", x, y, w, h, {"closable": false, "collapsible": false}) {
      if gui.tree_node("root", "World", true) {
         if gui.tree_node("cam", "Camera", false, true, tree_sel == "cam") { gui.tree_pop() }
         if gui.last_item_id() == gui.active_id() && gui.mouse_down() { tree_sel = "cam" }
         if gui.tree_node("mesh", "Meshes", true, false, tree_sel == "mesh") {
            mut i = 0
            while i < 3 {
               gui.push_id(i)
               gui.tree_node("m", "mesh_" + to_str(i), false, true)
               gui.drag_source("mesh", "mesh_" + to_str(i), "mesh_" + to_str(i))
               gui.pop_id()
               i += 1
            }
            gui.tree_pop()
         }
         if gui.tree_node("lights", "Lights") {
            gui.tree_node("sun", "Sun", false, true)
            gui.tree_pop()
         }
         gui.tree_pop()
      }
      gui.separator()
      gui.text_colored("Drag a mesh onto a slot:", "text_dim")
      mut s = 0
      while s < slots.len {
         gui.push_id("slot" + to_str(s))
         def cur = to_str(slot_map.get(s, ""))
         gui.button("b", to_str(slots.get(s, "")) + (cur.len > 0 ? (": " + cur) : ""), gui.remaining_w(), 0.0)
         def any got = gui.drop_target("mesh")
         if is_str(got) {
            mut next = []
            mut k = 0
            while k < slot_map.len {
               next = next.append(k == s ? to_str(got) : to_str(slot_map.get(k, "")))
               k += 1
            }
            slot_map = next
            say("drop." + to_str(slots.get(s, "")) + "=" + to_str(got))
         }
         gui.pop_id()
         s += 1
      }
   }
   gui.end_window()
}

fn draw_table(f64 x, f64 y, f64 w, f64 h) any {
   if gui.begin_window("assets", "Assets", x, y, w, h, {"closable": false, "collapsible": false}) {
      def names = ["sponza.glb", "helmet.glb", "sky_4k.exr", "noise.png", "ui.ttf", "bricks_n.png"]
      def sizes = [48211, 3770, 21504, 256, 148, 4096]
      def kinds = ["scene", "scene", "environment", "texture", "font", "texture"]
      if gui.begin_table("tbl", ["Name", "Size (KB)", "Kind", ""], [2.6, 1.7, 1.7, 1.0]) {
         def sort_col = gui.table_sort_column("tbl")
         def desc = gui.table_sort_descending("tbl")
         ;; Order by the chosen column with a small selection sort over indices.
         mut order = [0, 1, 2, 3, 4, 5]
         if sort_col >= 0 {
            mut a = 0
            while a < order.len {
               mut best = a
               mut b = a + 1
               while b < order.len {
                  def ib, ibest = int(order.get(b, 0)), int(order.get(best, 0))
                  mut less = false
                  if sort_col == 1 { less = int(sizes.get(ib, 0)) < int(sizes.get(ibest, 0)) }
                  elif sort_col == 2 { less = to_str(kinds.get(ib, "")) < to_str(kinds.get(ibest, "")) }
                  else { less = to_str(names.get(ib, "")) < to_str(names.get(ibest, "")) }
                  if desc { less = !less }
                  if less { best = b }
                  b += 1
               }
               if best != a {
                  def va, vb = int(order.get(a, 0)), int(order.get(best, 0))
                  mut next = []
                  mut k = 0
                  while k < order.len {
                     next = next.append(k == a ? vb : (k == best ? va : int(order.get(k, 0))))
                     k += 1
                  }
                  order = next
               }
               a += 1
            }
         }
         mut r = 0
         while r < order.len {
            def idx = int(order.get(r, 0))
            gui.table_next_row(idx == selected_row)
            if gui.table_row_clicked() { selected_row = idx }
            gui.table_text(names.get(idx, ""))
            gui.table_text(to_str(sizes.get(idx, 0)), "text_dim")
            gui.table_text(kinds.get(idx, ""), to_str(kinds.get(idx, "")) == "scene" ? "accent" : "text_dim")
            gui.table_next_column()
            gui.push_id(idx)
            if gui.small_button("open", "Open") { say("open." + to_str(names.get(idx, ""))) }
            gui.pop_id()
            r += 1
         }
         gui.end_table()
      }
      ;; The three panel states, switched by the demo buttons.
      gui.layout_row([0.0, 0.0, 0.0, 0.0])
      if gui.small_button("st0", "Ready") { panel_demo = "" }
      if gui.small_button("st1", "Empty") { panel_demo = "empty" }
      if gui.small_button("st2", "Loading") { panel_demo = "loading" }
      if gui.small_button("st3", "Error") { panel_demo = "error" }
      if panel_demo == "empty" { gui.panel_state("empty", "No assets match", "Clear the filter to see all six", 84.0) }
      elif panel_demo == "loading" { gui.panel_state("loading", "Loading sponza.glb", "48 MB, decoding textures", 84.0) }
      elif panel_demo == "error" { gui.panel_state("error", "Could not load helmet.glb", "File not found: models/helmet.glb", 84.0) }
      gui.begin_columns([1.0, 1.0])
      gui.property_row("p0", "Selected", names.get(selected_row, ""))
      gui.property_row("p1", "Kind", kinds.get(selected_row, ""))
      gui.next_column()
      gui.property_row("p2", "Clicks", to_str(clicks))
      gui.property_row("p3", "Theme", gui.theme())
      gui.end_columns()
      gui.separator()
      gui.text_colored(log_line, "text_dim")
   }
   gui.end_window()
}

def tiled = common.env_trim("NY_WIDGETS_TILE") == "1"
def layout_path = common.env_trim("NY_WIDGETS_LAYOUT")
mut layout_ready = false

fn draw_notes(f64 x, f64 y) any {
   if gui.begin_window("notes", "Notes", x, y, 300.0, 170.0, {"collapsible": false}) {
      gui.text("A floating window.")
      gui.text_colored("Drag my title bar onto an indicator to dock.", "text_dim")
      if gui.button("reset", "Reset layout") {
         layout_ready = false
         say("layout.reset")
      }
   }
   gui.end_window()
}

fn default_layout() any {
   gui.undock_window("controls")
   gui.undock_window("scene")
   gui.undock_window("assets")
   gui.undock_window("notes")
   gui.dock_window("main", "controls")
   gui.dock_window("main", "assets", "controls", "right")
   gui.dock_window("main", "scene", "assets", "left")
   gui.dock_split_ratio("main", "", 0.34)
   gui.dock_split_ratio("main", "b", 0.38)
}

mut frame = 0
while !render.window_should_close(win) {
   mut e = window.check_event(win)
   while e != 0 {
      def typ = window.event_type(e)
      def data = window.event_data(e)
      if typ == EVENT_KEY_PRESSED {
         def k = int(data.get("key", 0))
         if k == KEY_F2 { gui.set_theme(gui.theme() == "light" ? "dark" : "light") }
         elif k == KEY_F3 { ui_scale = max(0.75, ui_scale - 0.25) }
         elif k == KEY_F4 { ui_scale = min(3.0, ui_scale + 0.25) }
      }
      if !script_has_pos || (typ != EVENT_MOUSE_POS_CHANGED && typ != EVENT_MOUSE_BUTTON_PRESSED && typ != EVENT_MOUSE_BUTTON_RELEASED) {
         gui.feed_event(typ, data)
      }
      e = window.check_event(win)
   }
   run_script(frame)
   def fb = render.framebuffer_size_f64(START_W, START_H)
   def sw, sh = max(1.0, float(fb.get(0, START_W))), max(1.0, float(fb.get(1, START_H)))
   gui.set_headless_input(script_has_pos)
   gui.set_enabled(true)
   ;; Framebuffer pixels per window unit; layout is in framebuffer pixels.
   def f64 dpi = float(window.get_window_content_scale(win).get(0, 1.0))
   def f64 draw_scale = ui_scale * (dpi > 0.0 ? dpi : 1.0)
   load_fonts(draw_scale)
   gui.set_scale(draw_scale)
   gui.set_fonts(font_body, font_title, font_small)
   ;; Two calls rather than `script_has_pos ? 0 : win`: that conditional
   ;; yielded the window even while a script owned the pointer.
   if script_has_pos { gui.prepare_input(0, sw, sh) }
   else { gui.prepare_input(win, sw, sh) }
   def bg = gui.theme_color("window")
   if !render.begin_frame_clear([float(bg.get(0, 0.0)) * 0.8, float(bg.get(1, 0.0)) * 0.8, float(bg.get(2, 0.0)) * 0.8, 1.0]) { continue }
   render.set_ortho_2d(0.0, sw, sh, 0.0)
   gui.begin_frame(0, font_body, sw, sh)
   if tiled {
      ;; Three tiled panels; below 760 px they stack so nothing is cut off.
      def pad = 8.0
      if sw >= 760.0 {
         def cols = gui.split_cols([pad, pad, sw - pad * 2.0, sh - pad * 2.0], [1.15, 0.8, 1.35], pad)
         def any c0, c1 = cols.get(0, 0), cols.get(1, 0)
         def any c2 = cols.get(2, 0)
         gui.apply_window_rect("controls", c0)
         gui.apply_window_rect("scene", c1)
         gui.apply_window_rect("assets", c2)
         draw_controls(float(c0.get(0)), float(c0.get(1)), float(c0.get(2)), float(c0.get(3)))
         draw_tree(float(c1.get(0)), float(c1.get(1)), float(c1.get(2)), float(c1.get(3)))
         draw_table(float(c2.get(0)), float(c2.get(1)), float(c2.get(2)), float(c2.get(3)))
      } else {
         def rows = gui.split_rows([pad, pad, sw - pad * 2.0, sh - pad * 2.0], [1.3, 0.9, 1.0], pad)
         def any r0, r1 = rows.get(0, 0), rows.get(1, 0)
         def any r2 = rows.get(2, 0)
         gui.apply_window_rect("controls", r0)
         gui.apply_window_rect("scene", r1)
         gui.apply_window_rect("assets", r2)
         draw_controls(float(r0.get(0)), float(r0.get(1)), float(r0.get(2)), float(r0.get(3)))
         draw_tree(float(r1.get(0)), float(r1.get(1)), float(r1.get(2)), float(r1.get(3)))
         draw_table(float(r2.get(0)), float(r2.get(1)), float(r2.get(2)), float(r2.get(3)))
      }
   } else {
      if !layout_ready {
         layout_ready = true
         mut loaded = false
         if layout_path.len > 0 {
            def saved = file_read(layout_path)
            if is_ok(saved) { loaded = gui.dock_layout_load("main", str.strip(to_str(unwrap(saved)))) }
         }
         if !loaded { default_layout() }
      }
      gui.dockspace("main", 0.0, 0.0, sw, sh)
      draw_controls(8.0, 8.0, 380.0, 520.0)
      draw_tree(400.0, 8.0, 280.0, 520.0)
      draw_table(700.0, 8.0, 400.0, 520.0)
      draw_notes(sw * 0.5 - 150.0, sh * 0.55)
   }
   gui.end_frame()
   def last = frame_limit > 0 && frame + 1 >= frame_limit
   if last && snapshot_path.len > 0 { render.snapshot(snapshot_path) }
   render.end_frame()
   if (frame % 10) == 0 || last {
      print("WIDGETS_DOCK " + gui.dock_layout_save("main"))
      print("WIDGETS_STATE frame=" + to_str(frame) + " popup=" + to_str(gui.any_popup_open()) + " hot=" + gui.hovered_id() + " focus=" + gui.focused_id() + " theme=" + gui.theme() + " size=" + to_str(int(sw)) + "x" + to_str(int(sh)))
   }
   frame += 1
   if last { break }
}
if !tiled && layout_path.len > 0 { file_write(layout_path, gui.dock_layout_save("main") + "\n") }
render.close_window()
exit(0)
