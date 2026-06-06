;; Keywords: engine loading progress assets async os ui render viewer scene
;; Loading state and progress UI helpers for model and asset changes.
;; References:
;; - std.os.ui.assets.batch
;; - std.os.ui.render.scene
module std.os.ui.render.viewer.engine.loading(draw_card, draw_startup_card)
use std.core
use std.math
use std.os.ui.render as gfx

fn _panel_rect(f64 w, f64 h, f64 min_w, f64 max_w, f64 frac, f64 panel_h) list {
   def pw = min(max_w, max(min_w, w * frac))
   [w * 0.5 - pw * 0.5, h * 0.5 - panel_h * 0.5, pw, panel_h]
}

fn _bar(f64 x, f64 y, f64 w, f64 phase) any {
   gfx.draw_rect_fast(x, y, w, 5.0, gfx.color_pack(0.000, 0.000, 0.000, 0.92))
   def sweep_w = max(54.0, w * 0.22)
   def pulse = 0.5 + 0.5 * sin(float(phase) * 5.0)
   gfx.draw_rect_fast(x + (w - sweep_w) * pulse, y, sweep_w, 5.0, gfx.color_pack(0.86, 0.86, 0.86, 0.86))
}

fn draw_card(f64 phase, f64 ww, f64 wh, int title_font, int ui_font, any label, any detail="Preparing asset off-thread; final GPU upload follows.") bool {
   "Draws the async asset loading overlay card."
   def r = _panel_rect(float(ww), float(wh), 340.0, 560.0, 0.46, 154.0)
   def x, y = float(r.get(0, 0.0)), float(r.get(1, 0.0))
   def w, h = float(r.get(2, 0.0)), float(r.get(3, 0.0))
   gfx.draw_rect_fast(0.0, 0.0, float(ww), float(wh), gfx.color_pack(0.000, 0.000, 0.000, 0.58))
   gfx.draw_rect_fast(x, y, w, h, gfx.color_pack(0.000, 0.000, 0.000, 0.84))
   gfx.draw_rect_fast(x, y, w, 2.0, gfx.color_pack(0.86, 0.86, 0.86, 0.72))
   gfx.draw_rect_fast(x, y + h - 1.0, w, 1.0, gfx.color_pack(0.22, 0.22, 0.22, 0.74))
   _bar(x + 20.0, y + 108.0, w - 40.0, phase)
   gfx.draw_text(title_font, "LOADING", x + 22.0, y + 24.0, [0.94, 0.94, 0.94, 0.98])
   gfx.draw_text(ui_font, to_str(label), x + 22.0, y + 62.0, [0.78, 0.78, 0.78, 0.96])
   gfx.draw_text(ui_font, to_str(detail), x + 22.0, y + 84.0, [0.58, 0.58, 0.58, 0.88])
}

fn draw_startup_card(f64 ww, f64 wh, int title_font, int ui_font, any label) bool {
   "Draws the compact synchronous loading card used during direct scene loads."
   def r = _panel_rect(float(ww), float(wh), 320.0, 520.0, 0.42, 116.0)
   def x, y = float(r.get(0, 0.0)), float(r.get(1, 0.0))
   def w, h = float(r.get(2, 0.0)), float(r.get(3, 0.0))
   gfx.draw_rect_fast(0.0, 0.0, float(ww), float(wh), gfx.color_pack(0.000, 0.000, 0.000, 1.0))
   gfx.draw_rect_fast(0.0, 0.0, float(ww), 56.0, gfx.color_pack(0.000, 0.000, 0.000, 0.92))
   gfx.draw_rect_fast(0.0, 56.0, float(ww), 1.0, gfx.color_pack(0.86, 0.86, 0.86, 0.42))
   gfx.draw_rect_fast(x, y, w, h, gfx.color_pack(0.000, 0.000, 0.000, 0.94))
   gfx.draw_rect_fast(x, y, w, 2.0, gfx.color_pack(0.86, 0.86, 0.86, 0.92))
   gfx.draw_rect_fast(x, y + h - 1.0, w, 1.0, gfx.color_pack(0.22, 0.22, 0.22, 0.84))
   gfx.draw_text(title_font, "NYTRIX", x + 22.0, y + 24.0, [0.94, 0.94, 0.94, 1.0])
   gfx.draw_text(ui_font, to_str(label), x + 22.0, y + 66.0, [0.74, 0.74, 0.74, 1.0])
   gfx.draw_rect_fast(x + 22.0, y + 92.0, w - 44.0, 4.0, gfx.color_pack(0.10, 0.10, 0.10, 1.0))
   gfx.draw_rect_fast(x + 22.0, y + 92.0, (w - 44.0) * 0.62, 4.0, gfx.color_pack(0.86, 0.86, 0.86, 0.92))
}

#main {
   assert(_panel_rect(800.0, 450.0, 320.0, 520.0, 0.42, 116.0).len == 4, "loading panel rect")
   print("✓ viewer loading self-test passed")
}
