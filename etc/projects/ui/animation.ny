#!/usr/bin/env ny

;; Nytrix boot-logo animation

use core
use core.math as math
use core.str as text
use core.os (exit)
use core.os.fixture as fixture
use core.os.ui.debug as ui_debug
use core.os.ui.render as gfx
use core.os.ui.render.dump as ui_profile
use core.os.ui.window as window
use core.os.ui.window.consts as key

def WIN_W, WIN_H = 800, 450
def f64 VIEW_W, VIEW_H = 800.0, 450.0
def f64 CELL, TAG_CELL, WORD_CELL = 5.0, 2.0, 3.0
def f64 LOGO_X0, LOGO_Y0 = 279.0, 84.0
def f64 TAG_X0, TAG_Y0 = 347.0, 334.0
def f64 WORD_X0, WORD_Y0 = 332.0, 355.0
def f64 SHADOW_X, SHADOW_Y = 2.0, 3.0
def f64 POP_DURATION, SHADOW_WINDOW = 340.0, 260.0
def f64 TAG_INTERVAL, WORD_INTERVAL = 70.0, 85.0
def f64 ASSEMBLE_END, TAG_START, WORD_START = 926.003412959, 1080.0, 1720.0
def f64 HOLD_END, FADE_DURATION, CYCLE = 4580.0, 620.0, 5200.0
def f64 SHADOW_ALPHA, POP_VISIBLE, POP_ALPHA_RAMP = 0.13, 0.995, 1.6
def f64 LOGO_GHOST_ALPHA = 0.38

def BG_COLOR = gfx.color_hex("#000000")
def int SHADOW_COLOR = gfx.color_pack(49.0 / 255.0, 46.0 / 255.0, 129.0 / 255.0)
def int TAG_COLOR = gfx.color_pack(0.659, 0.620, 0.965)
def int WORD_COLOR = gfx.color_pack(244.0 / 255.0, 242.0 / 255.0, 1.0)
def int LOGO_C0 = gfx.color_pack(1.0, 1.0, 1.0)
def int LOGO_C1 = gfx.color_pack(238.0 / 255.0, 241.0 / 255.0, 1.0)
def int LOGO_C2 = gfx.color_pack(226.0 / 255.0, 232.0 / 255.0, 1.0)
def int LOGO_C3 = gfx.color_pack(248.0 / 255.0, 249.0 / 255.0, 1.0)
def int LOGO_C4 = gfx.color_pack(99.0 / 255.0, 102.0 / 255.0, 241.0 / 255.0)
def int LOGO_C5 = gfx.color_pack(111.0 / 255.0, 98.0 / 255.0, 242.0 / 255.0)
def int LOGO_C6 = gfx.color_pack(123.0 / 255.0, 95.0 / 255.0, 243.0 / 255.0)
def int LOGO_C7 = gfx.color_pack(137.0 / 255.0, 91.0 / 255.0, 244.0 / 255.0)
def int LOGO_C8 = gfx.color_pack(152.0 / 255.0, 88.0 / 255.0, 246.0 / 255.0)
def int LOGO_C9 = gfx.color_pack(168.0 / 255.0, 85.0 / 255.0, 247.0 / 255.0)

;; Each digit selects the original logo palette; dots are transparent.
def LOGO_ROWS = [
   "............0......................1............",
   "..........0000.....................233..........",
   "........0000000...................423333........",
   "......0000000000.................442333333......",
   "....0000000000000...............554233333333....",
   "..0000000000000000.............555523333333333..",
   "0000000000000000003...........555552333333333333",
   "0000000000000000033..........5555552333333333333",
   "00000000000000003333........55555552333333333333",
   "000000000000000333333.......55555552333333333333",
   "0000000000000033333333.....555555552333333333333",
   "00000000000003333333333...5555555552333333333333",
   "000000000000333333333333.66655555552333333333333",
   "000000000003333333333333766666655552333333333333",
   "000000000033333333333338666666666552333333333333",
   "000000000333333333333396666666666662333333333333",
   "000000003333333333333966666666666662333333333331",
   "000000033333333333332666666666666662333333333311",
   "000000333333333333327666666666666662333333333111",
   "000003333333333333176666666666666662333333331111",
   "000033333333333333877766666666666662333333311111",
   "000333333333333338777777666666666662333333111111",
   "0033333333333333977777777766666666.3333331111111",
   "0333333333333.39777777777777666668.3333311111111",
   "3333333333333.97777777777777776683.3333111111111",
   "3333333333333.7777777777777777781333331111111111",
   "333333333333177777777777777777713333311111111111",
   "333333333333277777777777777777233333111111111111",
   "333333333333287777777777777772333331111111111111",
   "333333333333288877777777777793333311111111111111",
   "333333333333288888777777777933333111111111111111",
   "333333333333288888887777779333331111111111111111",
   "333333333333288888888877781333311111111111111111",
   "333333333333288888888888813333111111111111111111",
   "333333333333288888888888233331111111111111111111",
   "33333333333328888888888.333311111111111111111112",
   "3333333333332888888888...33111111111111111111122",
   "333333333333288888888.....1111111111111111111222",
   "333333333333288888888......111111111111111112222",
   "33333333333328888888........11111111111111122222",
   "3333333333332888888..........1111111111111222222",
   "333333333333288888...........1111111111112222222",
   "..333333333329988.............1111111111222222..",
   "....333333332999...............1111111122222....",
   "......333333299.................1111112222......",
   "........3333299..................1111222........",
   "..........3329....................1122..........",
   "............2......................2............"
]

def LETTER_ROWS = [
   "0...0...1...1...22222...3333....44444...5...5",
   "00..0...1...1.....2.....3...3.....4.....5...5",
   "0.0.0....1.1......2.....3...3.....4......5.5",
   "0..00.....1.......2.....3333......4.......5",
   "0...0.....1.......2.....3.3.......4......5.5",
   "0...0.....1.......2.....3..3......4.....5...5",
   "0...0.....1.......2.....3...3...44444...5...5"
]

def TAG_ROWS = [
   "0...0..111..2222..33333.......5...5.66666.77777.8...8",
   "00.00.1...1.2...2.3...........5...5...6.....7...8...8",
   "0.0.0.1...1.2...2.3...........5...5...6.....7...8...8",
   "0.0.0.11111.2...2.3333........5.5.5...6.....7...88888",
   "0...0.1...1.2...2.3...........5.5.5...6.....7...8...8",
   "0...0.1...1.2...2.3...........55.55...6.....7...8...8",
   "0...0.1...1.2222..33333.......5...5.66666...7...8...8"
]

fn ease_out_back(f64 x) f64 {
   def f64 c1 = 1.70158
   def f64 c3 = c1 + 1.0
   def f64 t = x - 1.0
   1.0 + c3 * t * t * t + c1 * t * t
}

fn logo_color(int shade) int {
   if shade == 48 { return LOGO_C0 }
   if shade == 49 { return LOGO_C1 }
   if shade == 50 { return LOGO_C2 }
   if shade == 51 { return LOGO_C3 }
   if shade == 52 { return LOGO_C4 }
   if shade == 53 { return LOGO_C5 }
   if shade == 54 { return LOGO_C6 }
   if shade == 55 { return LOGO_C7 }
   if shade == 56 { return LOGO_C8 }
   LOGO_C9
}

fn pixel_rect(f64 x, f64 y, f64 w, f64 h, int color, f64 alpha) int {
   if alpha <= 0.0 { return 0 }
   def int alpha8 = int(math.clamp01(alpha) * 255.0)
   def int packed = (color & 0x00FFFFFF) | (alpha8 << 24)
   gfx.draw_rect_fast(math.round(x), math.round(y), math.ceil(w), math.ceil(h), packed)
   0
}

fn draw_logo_pass(f64 alpha, bool shadow) int {
   mut int y = 0
   while y < 48 {
      def str row = LOGO_ROWS[y]
      mut int x = 0
      while x < 48 {
         def int shade = text.byte_at(row, x)
         if shade != 46 {
            def int first = x
            x += 1
            while x < 48 {
               def int next_shade = text.byte_at(row, x)
               if next_shade == 46 || (!shadow && next_shade != shade) { break }
               x += 1
            }
            def f64 px = LOGO_X0 + float(first) * CELL
            def f64 py = LOGO_Y0 + float(y) * CELL
            def f64 width = float(x - first) * CELL
            if shadow {
               pixel_rect(px + SHADOW_X, py + SHADOW_Y, width, CELL, SHADOW_COLOR, SHADOW_ALPHA * alpha)
            } else {
               pixel_rect(px, py, width, CELL, logo_color(shade), alpha)
            }
         } else {
            x += 1
         }
      }
      y += 1
   }
   0
}

fn draw_static_logo(f64 alpha) int {
   draw_logo_pass(alpha, true)
   draw_logo_pass(alpha, false)
   0
}

fn draw_spawn_logo(f64 elapsed, f64 alpha) int {
   ;; Keep a soft silhouette visible while the colored pixels assemble.
   draw_logo_pass(LOGO_GHOST_ALPHA * alpha, false)
   def f64 shadow_alpha = math.clamp01((elapsed - (ASSEMBLE_END - SHADOW_WINDOW)) / SHADOW_WINDOW) * alpha
   if shadow_alpha > 0.0 { draw_logo_pass(shadow_alpha, true) }

   mut int y = 0
   while y < 48 {
      def str row = LOGO_ROWS[y]
      mut int x = 0
      while x < 48 {
         def int shade = text.byte_at(row, x)
         if shade != 46 {
            def f64 dx = float(x) * CELL - 117.5
            def f64 dy = float(y) * CELL - 117.5
            def f64 delay = math.sqrt(dx * dx + dy * dy) * 4.0
            def f64 raw = math.clamp01((elapsed - delay) / POP_DURATION)
            if raw > 0.0 {
               def int color = logo_color(shade)
               def f64 px = LOGO_X0 + float(x) * CELL
               def f64 py = LOGO_Y0 + float(y) * CELL
               if raw >= POP_VISIBLE {
                  pixel_rect(px, py, CELL, CELL, color, alpha)
               } else {
                  def f64 size = math.max(1.0, math.round(CELL * ease_out_back(raw)))
                  pixel_rect(px + (CELL - size) * 0.5, py + (CELL - size) * 0.5,
                     size, size, color, math.clamp01(raw * POP_ALPHA_RAMP) * alpha)
               }
            }
         }
         x += 1
      }
      y += 1
   }
   0
}

fn draw_label(list rows, int width, int height, int visible,
   f64 ox, f64 oy, f64 cell, int color, f64 alpha) int {
   mut int y = 0
   while y < height {
      def str row = rows[y]
      mut int x = 0
      while x < width {
         def int shade = text.byte_at(row, x)
         if shade >= 48 && shade - 48 < visible {
            def int first = x
            x += 1
            while x < width {
               def int next_shade = text.byte_at(row, x)
               if next_shade < 48 || next_shade - 48 >= visible { break }
               x += 1
            }
            pixel_rect(ox + float(first) * cell, oy + float(y) * cell,
               float(x - first) * cell, cell, color, alpha)
         } else {
            x += 1
         }
      }
      y += 1
   }
   0
}

gfx.apply_backend_env()
;; This is a 2D pixel animation; OpenGL is the faster default on software-backed
;; desktops. Honor an explicit NY_UI_RENDER_BACKEND or --vk/--auto selection.
if !ui_profile.env_present_cached("NY_UI_RENDER_BACKEND") {
   gfx.apply_backend_name("gl")
}
gfx.apply_backend_argv()

mut win = gfx.init_window(WIN_W, WIN_H, "Made with Nytrix", key.WINDOW_CENTER, true, false, 1)
if !win { panic("could not create the Nytrix splash window") }
defer { gfx.close_window() }

mut f64 timeline = 0.0
mut int fps_frames = 0
mut int fps_started = ui_debug.now()
def bool fps_log = ui_debug.env_truthy_cached("NY_UI_FPS_LOG")
fixture.ready("animation")
def mock_backend = is_dict(win) && !!win.get("_mock", false)
def fx = fixture.start_ui(0, 0, mock_backend)
window.set_exit_key(win, key.KEY_NULL)

;; Let the window manager apply its mapped size before the first Vulkan frame;
;; queue the resulting framebuffer extent before creating that frame's target.
if !mock_backend && gfx.get_active_backend_name() == "vulkan" {
   window.show(win)
   mut int framebuffer_w, framebuffer_h = 0, 0
   mut int stable_size_reads, size_polls = 0, 0
   while size_polls < 12 && stable_size_reads < 3 {
      window.poll_events()
      def fb_size = gfx.get_framebuffer_size()
      def int next_w, next_h = int(fb_size.get(0, 0)), int(fb_size.get(1, 0))
      if next_w > 0 && next_h > 0 {
         if next_w == framebuffer_w && next_h == framebuffer_h {
            stable_size_reads += 1
         } else {
            framebuffer_w, framebuffer_h = next_w, next_h
            stable_size_reads = 0
         }
      }
      size_polls += 1
      if size_polls < 12 && stable_size_reads < 3 { msleep(16) }
   }
}

while !window.should_close(win) && fixture.tick_ui(fx) {
   if fixture.quit_key(fixture.next_ui_key(fx)) { break }
   window.poll_events()
   if window.key_pressed(win, key.KEY_ESCAPE) { break }

   def frame_fit = gfx.begin_frame_layout(BG_COLOR, VIEW_W, VIEW_H)
   if !frame_fit { continue }

   def f64 dt = float(gfx.get_delta_time())
   if fps_log {
      fps_frames += 1
      def int fps_now = ui_debug.now()
      def f64 fps_elapsed = float(fps_now - fps_started) / 1000000000.0
      if fps_elapsed >= 1.0 {
         ui_debug.print_text("[fps] animation=" + to_str(float(fps_frames) / fps_elapsed) +
            " frames=" + to_str(fps_frames) + " window=" + to_str(fps_elapsed))
         fps_frames = 0
         fps_started = fps_now
      }
   }
   def f64 frame_ms = float(math.clamp(dt, 0.0, 0.50)) * 1000.0
   timeline = float(math.fmod(timeline + frame_ms, CYCLE))
   mut f64 alpha = 1.0
   if timeline > HOLD_END { alpha = 1.0 - math.clamp01((timeline - HOLD_END) / FADE_DURATION) }

   if timeline < ASSEMBLE_END { draw_spawn_logo(timeline, alpha) }
   else { draw_static_logo(alpha) }

   if timeline >= TAG_START {
      def int visible = int(math.min(9.0, math.floor((timeline - TAG_START) / TAG_INTERVAL) + 1.0))
      draw_label(TAG_ROWS, 53, 7, visible, TAG_X0, TAG_Y0, TAG_CELL, TAG_COLOR, 0.90 * alpha)
   }
   if timeline >= WORD_START {
      def int visible = int(math.min(6.0, math.floor((timeline - WORD_START) / WORD_INTERVAL) + 1.0))
      draw_label(LETTER_ROWS, 45, 7, visible, WORD_X0, WORD_Y0, WORD_CELL, WORD_COLOR, alpha)
   }
   gfx.end_frame()
}

exit(fixture.exit_status(fx))
