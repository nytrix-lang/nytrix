#!/usr/bin/env ny

;; Keywords: ui pong game example
;; Single-player arcade Pong with an AI opponent, independently implemented for Nytrix.
;; Visual inspiration: https://github.com/tsoding/ping-pong-love-remastered

use core
use core.os (exit)
use core.os.fixture as fixture
use core.common as common
use core.math as math
use core.os.ui.render as gfx
use core.os.ui.window as window
use core.os.ui.window.consts (KEY_W, KEY_S, KEY_ESCAPE, KEY_SPACE, KEY_R)

def WIN_W, WIN_H = 1280, 720
def f64 VIEW_W, VIEW_H = 1280.0, 720.0
def f64 BALL_SIZE, BALL_SPEED = 20.0, 400.0
def f64 PADDLE_W, PADDLE_H, PADDLE_MARGIN = 20.0, 120.0, 80.0
def f64 PADDLE_SPEED, OPPONENT_SPEED, OPPONENT_ERROR, SERVE_TIME = 480.0, 320.0, 18.0, 0.85
def int NET_COUNT = 51

def BG = gfx.color_hex("#201E18")
def FG = gfx.color_hex("#F4F1E8")
def MUTED = gfx.color_hex("#807D75")

fn lim(f64 value, f64 lower, f64 upper) f64 {
   float(math.min(math.max(value, lower), upper))
}

fn opponent_goal(f64 bx, f64 by, f64 vx, f64 vy, f64 ball, f64 h,
                 f64 paddle_h, f64 plane, int rally) f64 {
   def f64 center = by + ball * 0.5
   if vx <= 0.0 { return (h - paddle_h) * 0.5 }
   def f64 t = float(math.max((plane - bx) / vx, 0.0))
   def f64 span = h - ball
   if span <= 0.0 { return 0.0 }
   def f64 period = span * 2.0
   mut f64 folded = (center + vy * t - ball * 0.5) % period
   if folded < 0.0 { folded += period }
   if folded > span { folded = period - folded }
   ;; Keep the predictive opponent fallible and beatable without making it erratic.
   def f64 variation = float(rally % 5 - 2) * OPPONENT_ERROR
   lim(ball * 0.5 + folded + variation - paddle_h * 0.5, 0.0, h - paddle_h)
}

fn place_ball(f64 w, f64 h, f64 size, int rally) {
   ball_x = (w - size) * 0.5
   ball_y = (h - size) * 0.5
   def f64 direction = rally % 2 == 0 ? 1.0 : -1.0
   ball_vx = direction * BALL_SPEED * scale
   ball_vy = BALL_SPEED * scale * (rally % 4 < 2 ? 0.23 : -0.23)
}

if common.env_present("NY_UI_RENDER_BACKEND") {
   gfx.apply_backend_env()
} else {
   gfx.set_backend_type(gfx.BACKEND_GL)
}
gfx.apply_backend_argv()

mut win = gfx.init_window(WIN_W, WIN_H, "Ping Pong - Nytrix", 0, true, false, 1)
if !win { panic("could not create the Pong window") }
if is_dict(win) && !!win.get("_mock", false) {
   if !gfx.begin_frame_clear(BG) { panic("Pong renderer could not begin a frame") }
   if !gfx.end_frame() { panic("Pong renderer could not present a frame") }
   gfx.close_window()
   exit(0)
}

fixture.ready("pong")
def headless = gfx.get_active_backend_name() == "mock"
def int legacy_frames = common.env_int_clamped("NYTRIX_PONG_MAX_FRAMES", 0, 0, 1000000)
def int legacy_timeout = common.env_int_clamped("NYTRIX_PONG_TIMEOUT_MS", headless ? 1000 : 0, 0, 3600000)
def fx = fixture.start_ui(legacy_frames, legacy_timeout, headless && !common.env_present("NYTRIX_PONG_MAX_FRAMES"))
def int font = gfx.font_load_first(["etc/assets/fonts/monocraft.ttf"], 48) ?? 0
def int font_small = gfx.font_load_first(["etc/assets/fonts/monocraft.ttf"], 20) ?? 0

mut int player_score = 0
mut int opponent_score = 0
mut f64 scale = 1.0
mut f64 ball_x = 0.0
mut f64 ball_y = 0.0
mut f64 ball_vx = 0.0
mut f64 ball_vy = 0.0
mut f64 player_y = 0.0
mut f64 opponent_y = 0.0
mut f64 serve_t = SERVE_TIME
mut bool paused = false
mut bool previous_pause = false
mut bool previous_reset = false
mut bool previous_escape = false
mut bool first_frame = true
mut bool initialized = false
mut int rally = 0

while !gfx.window_should_close(win) && fixture.tick_ui(fx) {
   if fixture.quit_key(fixture.next_ui_key(fx)) { break }
   if !gfx.begin_frame_clear(BG) { continue }

   def fb = gfx.framebuffer_size_f64()
   def f64 w = float(fb[0])
   def f64 h = float(fb[1])
   scale = float(math.min(w / VIEW_W, h / VIEW_H))
   if scale <= 0.0 { scale = 1.0 }
   def f64 ball = BALL_SIZE * scale
   def f64 paddle_w = PADDLE_W * scale
   def f64 paddle_h = PADDLE_H * scale
   def f64 margin = PADDLE_MARGIN * scale
   def f64 speed = PADDLE_SPEED * scale
   def f64 player_x = margin
   def f64 opponent_x = w - margin - paddle_w
   def f64 dt = float(math.clamp(gfx.get_delta_time(), 1.0 / 240.0, 0.05))

   gfx.set_ortho_2d(0.0, w, h, 0.0)
   if !initialized {
      player_y = (h - paddle_h) * 0.5
      opponent_y = player_y
      place_ball(w, h, ball, rally)
      initialized = true
   }
   def pause_down = window.key_down(win, KEY_SPACE)
   def reset_down = window.key_down(win, KEY_R)
   def escape_down = window.key_down(win, KEY_ESCAPE)
   if escape_down && !previous_escape { window.set_should_close(win, true) }
   previous_escape = escape_down
   if pause_down && !previous_pause { paused = !paused }
   previous_pause = pause_down
   if reset_down && !previous_reset {
      player_score, opponent_score = 0, 0
      rally = 0
      paused = false
      serve_t = SERVE_TIME
      place_ball(w, h, ball, rally)
   }
   previous_reset = reset_down

   player_y = lim(player_y, 0.0, h - paddle_h)
   opponent_y = lim(opponent_y, 0.0, h - paddle_h)
   if window.key_down(win, KEY_W) { player_y -= speed * dt }
   if window.key_down(win, KEY_S) { player_y += speed * dt }
   player_y = lim(player_y, 0.0, h - paddle_h)

   if !paused {
      if serve_t > 0.0 {
         serve_t = float(math.max(0.0, serve_t - dt))
      } else {
         def f64 goal = opponent_goal(
            ball_x, ball_y, ball_vx, ball_vy, ball, h, paddle_h,
            opponent_x - ball, rally
         )
         def f64 delta = goal - opponent_y
         def f64 step = float(math.min(float(math.abs(delta)), OPPONENT_SPEED * scale * dt))
         if delta > 0.0 { opponent_y += step }
         elif delta < 0.0 { opponent_y -= step }
         opponent_y = lim(opponent_y, 0.0, h - paddle_h)

         ball_x += ball_vx * dt
         ball_y += ball_vy * dt

         if ball_y < 0.0 { ball_y = 0.0 ball_vy = float(math.abs(ball_vy)) }
         if ball_y + ball > h { ball_y = h - ball ball_vy = -float(math.abs(ball_vy)) }

         if ball_vx < 0.0 && ball_x <= player_x + paddle_w && ball_x + ball >= player_x &&
            ball_y + ball >= player_y && ball_y <= player_y + paddle_h {
            ball_x = player_x + paddle_w
            ball_vx = float(math.abs(ball_vx))
            def f64 offset = ((ball_y + ball * 0.5) - (player_y + paddle_h * 0.5)) / (paddle_h * 0.5)
            ball_vy = lim(offset * BALL_SPEED * scale, -BALL_SPEED * scale, BALL_SPEED * scale)
            rally += 1
         }
         if ball_vx > 0.0 && ball_x + ball >= opponent_x && ball_x <= opponent_x + paddle_w &&
            ball_y + ball >= opponent_y && ball_y <= opponent_y + paddle_h {
            ball_x = opponent_x - ball
            ball_vx = -float(math.abs(ball_vx))
            def f64 offset = ((ball_y + ball * 0.5) - (opponent_y + paddle_h * 0.5)) / (paddle_h * 0.5)
            ball_vy = lim(offset * BALL_SPEED * scale, -BALL_SPEED * scale, BALL_SPEED * scale)
            rally += 1
         }

         if ball_x + ball < 0.0 {
            opponent_score += 1
            rally += 1
            place_ball(w, h, ball, rally)
            serve_t = SERVE_TIME
         } elif ball_x > w {
            player_score += 1
            rally += 1
            place_ball(w, h, ball, rally)
            serve_t = SERVE_TIME
         }
      }
   }

   def f64 dash_h = h / float(NET_COUNT)
   mut int dash = 0
   while dash < NET_COUNT {
      if dash % 2 == 0 {
         gfx.draw_rect(w * 0.5 - 1.5 * scale, float(dash) * dash_h, 3.0 * scale, dash_h * 0.56, MUTED)
      }
      dash += 1
   }

   gfx.draw_rect(player_x, player_y, paddle_w, paddle_h, FG)
   gfx.draw_rect(opponent_x, opponent_y, paddle_w, paddle_h, FG)
   gfx.draw_rect(ball_x, ball_y, ball, ball, FG)
   def f64 score_y = 44.0 * scale
   def f64 label_y = 12.0 * scale
   gfx.draw_text_centered(font_small, "YOU", w * 0.5 - 100.0 * scale, label_y, MUTED)
   gfx.draw_text_centered(font_small, "CPU", w * 0.5 + 100.0 * scale, label_y, MUTED)
   gfx.draw_text_centered(font, to_str(player_score), w * 0.5 - 100.0 * scale, score_y, FG)
   gfx.draw_text_centered(font, to_str(opponent_score), w * 0.5 + 100.0 * scale, score_y, FG)

   if paused { gfx.draw_text_centered(font, "PAUSED", w * 0.5, h * 0.5 - 24.0 * scale, FG) }
   elif serve_t > 0.0 { gfx.draw_text_centered(font_small, "W/S MOVE  -  SPACE PAUSE", w * 0.5, h - 28.0 * scale, MUTED) }
   else { gfx.draw_text_centered(font_small, "W/S MOVE  -  SPACE PAUSE  -  R RESET", w * 0.5, h - 28.0 * scale, MUTED) }

   if !gfx.end_frame() { panic("Pong renderer could not present a frame") }
   if first_frame {
      if !window.show(win) { panic("Pong window could not be shown") }
      first_frame = false
   }
}

if font > 0 { gfx.font_destroy(font) }
if font_small > 0 { gfx.font_destroy(font_small) }
gfx.close_window()
exit(fixture.exit_status(fx))
