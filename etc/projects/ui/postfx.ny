#!/usr/bin/env ny

;; Keywords: ui render post process screen effects demo vignette tint invert scanlines grain
;; Screen effects demo for core.os.ui.render.postfx: a test scene with the
;; effects drawn over it and a panel to switch and tune them at run time.
;;
;;   ny etc/projects/ui/postfx.ny            (NY_UI_RENDER=vk|gl picks the backend)
;;
;; Keys: 1..8 toggle tint, glow, invert, fade, grain, scanlines, vignette,
;; letterbox; 0 turns all off; Tab hides the panel. NY_POSTFX_MASK=<bits>
;; starts with a set of effects on (bit 0 = tint, in EFFECTS order).

use core
use core.common as common
use core.math (sin, cos, max, min)
use core.os.ui.window as window
use core.os.ui.window.consts as keyc
use core.os.ui.render as gfx
use core.os.ui.render.postfx as fx
use core.os.ui.render.viewer.gui as gui
use core.os.ui.assets.viewer as assets

mut any _win = 0
mut bool _panel = true

fn scene(f64 w, f64 h, f64 t) any {
   ;; Sky, sun, hills, a row of saturated swatches, and a grey ramp: enough
   ;; color and contrast to judge every effect.
   gfx.draw_rect_gradient_v(0, 0, int(w), int(h * 0.62), [0.16, 0.36, 0.72, 1.0], [0.95, 0.62, 0.42, 1.0])
   gfx.draw_rect_gradient_v(0, int(h * 0.62), int(w), int(h * 0.38) + 1, [0.18, 0.42, 0.22, 1.0], [0.06, 0.16, 0.08, 1.0])
   def f64 sx = w * 0.70 + cos(t * 0.3) * w * 0.05
   def f64 sy = h * 0.34 + sin(t * 0.3) * h * 0.03
   gfx.draw_circle(sx, sy, h * 0.11, [1.0, 0.93, 0.62, 1.0])
   gfx.draw_circle(sx, sy, h * 0.15, [1.0, 0.86, 0.50, 0.25])
   mut int i = 0
   while i < 7 {
      def f64 fi = float(i)
      def f64 x = w * 0.06 + fi * w * 0.075
      def list cols = [[0.95, 0.20, 0.25, 1.0], [0.98, 0.58, 0.12, 1.0], [0.98, 0.88, 0.18, 1.0], [0.25, 0.80, 0.35, 1.0],
         [0.18, 0.62, 0.95, 1.0], [0.48, 0.32, 0.90, 1.0], [0.92, 0.38, 0.72, 1.0]]
      gfx.draw_rect(x, h * 0.70, w * 0.06, h * 0.10, cols.get(i, [1.0, 1.0, 1.0, 1.0]))
      i += 1
   }
   i = 0
   while i < 16 {
      def f64 g = float(i) / 15.0
      gfx.draw_rect(w * 0.06 + float(i) * w * 0.033, h * 0.84, w * 0.033, h * 0.06, [g, g, g, 1.0])
      i += 1
   }
   0
}

fn panel(f64 w, f64 h) any {
   if !_panel { return 0 }
   def f64 pw = 300.0
   def f64 ph = min(h - 24.0, 520.0)
   gui.begin_window("fx_panel", "Screen effects", w - pw - 12.0, 12.0, pw, ph, {"closable": false})
   gui.text_colored("1-8 toggle  0 off  Tab hide", "text_dim")
   gui.separator()
   ;; Bound to a typed local: `.len` read straight off the imported list
   ;; global is wrong on --native-only (docs/todo/ui.md, compiler defects).
   def list effects = fx.EFFECTS
   mut int i = 0
   while i < effects.len {
      def str name = to_str(effects.get(i, ""))
      gui.push_id("e" + to_str(i))
      def bool was = fx.enabled(name)
      def bool on = gui.toggle("on", to_str(i + 1) + "  " + name, was)
      if on != was { fx.enable(name, on) }
      if on {
         mut f64 lo = 0.0
         mut f64 hi = 1.0
         if name == "letterbox" {
            lo = 1.2
            hi = 3.0
         }
         def f64 cur = fx.amount(name)
         def f64 next = gui.slider_float("amt", "amount", cur, lo, hi, 0.0)
         if next != cur { fx.set_amount(name, next) }
      }
      gui.pop_id()
      i += 1
   }
   gui.separator()
   gui.text_colored(to_str(fx.last_passes()) + " passes on " + gfx.get_active_backend_name(), "text_dim")
   gui.end_window()
   0
}

fn on_key(int k) any {
   def list effects = fx.EFFECTS
   if k >= keyc.KEY_1 && k <= keyc.KEY_8 { fx.toggle(to_str(effects.get(k - keyc.KEY_1, ""))) }
   if k == keyc.KEY_0 { fx.disable_all() }
   if k == keyc.KEY_TAB { _panel = !_panel }
   0
}

fn main() int {
   if common.env_truthy("NY_UI_HEADLESS") { gfx.set_backend_type(gfx.BACKEND_MOCK) }
   else {
      gfx.apply_backend_env()
      gfx.apply_backend_argv()
   }
   _win = gfx.init_window(1100, 680, "postfx", keyc.WINDOW_CENTER | keyc.WINDOW_FOCUS_ON_SHOW, false, true, 1)
   if !_win {
      print("postfx: no window: " + gfx.window_error())
      return 1
   }
   def int mask = common.env_int_clamped("NY_POSTFX_MASK", 64, 0, 255)
   fx.set_mask(mask)
   gui.set_enabled(true)
   def int font = gfx.font_load_first(assets.MONO_FONT_CANDIDATES, 16)
   def int font_small = gfx.font_load_first(assets.MONO_FONT_CANDIDATES, 13)
   gui.set_fonts(font, font, font_small)
   def int backend = gfx.get_backend_type()
   def int mock = gfx.BACKEND_MOCK
   def bool headless = backend == mock && !common.env_truthy("NYTRIX_UI_REPLAY")
   mut int frames = 0
   while !gfx.window_should_close(_win) {
      if headless && frames >= 2 { break }
      mut any e = window.check_event(_win)
      while !is_nil(e) && !(is_int(e) && int(e) == 0) {
         def int typ = window.event_type(e)
         def any data = window.event_data(e)
         if typ == keyc.EVENT_KEY_PRESSED { on_key(int(data.get("key", 0))) }
         gui.feed_event(typ, data)
         e = window.check_event(_win)
      }
      def fb = gfx.framebuffer_size_f64(1100, 680)
      def f64 w = max(320.0, float(fb.get(0, 1100)))
      def f64 h = max(240.0, float(fb.get(1, 680)))
      if !gfx.begin_frame_clear([0.0, 0.0, 0.0, 1.0]) { continue }
      gfx.set_ortho_2d(0.0, w, h, 0.0)
      scene(w, h, gfx.get_time())
      fx.apply(w, h, gfx.get_time())
      gui.prepare_input(_win, w, h)
      gfx.set_ortho_2d(0.0, w, h, 0.0)
      gui.begin_frame(0, font, w, h)
      panel(w, h)
      gui.end_frame()
      gfx.end_frame()
      if common.env_truthy("NYTRIX_UI_REPLAY") { print("NYTRIX_UI_STATE frame=" + to_str(frames) + " postfx mask=" + to_str(fx.enabled_mask()) + " passes=" + to_str(fx.last_passes())) }
      frames += 1
   }
   gfx.close_window()
   0
}

;; The runtime calls main(); calling it here as well ran the app twice.
