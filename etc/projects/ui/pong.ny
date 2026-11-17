#!/usr/bin/env ny

;; Keywords: ui pong game example
;; Single-player arcade Pong

use core
use core.os (exit)
use core.os.fixture as fixture
use core.common as common
use core.math as math
use core.os.ui.render as gfx
use core.os.ui.render.dump as ui_dump
use core.os.ui.window as window
use core.os.ui.window.consts (KEY_W, KEY_S, KEY_ESCAPE, KEY_SPACE, KEY_R, WINDOW_MAXIMIZE)

def int startup_started = ticks()
def bool frame_times = common.env_truthy("NYTRIX_PONG_FRAME_TIMES")

def WIN_W, WIN_H = 1280, 720
def f64 VIEW_W, VIEW_H = 1280.0, 720.0
def f64 BALL_SIZE, BALL_SPEED = 20.0, 400.0
def f64 PADDLE_W, PADDLE_H, PADDLE_MARGIN = 20.0, 120.0, 80.0
def f64 PADDLE_SPEED, OPPONENT_SPEED, OPPONENT_ERROR, SERVE_TIME = 480.0, 320.0, 18.0, 0.85
def f64 MAX_BOUNCE_ANGLE, SPEEDUP_PER_HIT, MAX_SPEEDUP = 0.9, 0.03, 1.6
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
   ;; Serve toward the side that just conceded; speed restarts at its base value.
   hits = 0
   ball_x = (w - size) * 0.5
   ball_y = (h - size) * 0.5
   ball_vx = serve_dir * BALL_SPEED * scale
   ball_vy = BALL_SPEED * scale * (rally % 4 < 2 ? 0.23 : -0.23)
}

;; Reflect the ball off a paddle: the exit angle follows where it struck the
;; paddle, and the speed (not just vx) is kept constant, growing slightly with
;; every return in the rally.
fn bounce_off_paddle(f64 dir, f64 paddle_y, f64 paddle_h, f64 ball) {
   def f64 half = paddle_h * 0.5 + ball * 0.5
   def f64 offset = lim(((ball_y + ball * 0.5) - (paddle_y + paddle_h * 0.5)) / half, -1.0, 1.0)
   def f64 angle = offset * MAX_BOUNCE_ANGLE
   hits += 1
   def f64 speedup = float(math.min(1.0 + SPEEDUP_PER_HIT * float(hits), MAX_SPEEDUP))
   def f64 spd = BALL_SPEED * scale * speedup
   ball_vx = dir * spd * float(math.cos(angle))
   ball_vy = spd * float(math.sin(angle))
   rally += 1
}

gfx.apply_backend_env()
gfx.apply_backend_argv()

mut win = gfx.init_window(WIN_W, WIN_H, "Ping Pong - Nytrix", WINDOW_MAXIMIZE, true, false, 1)
if !win { panic("could not create the Pong window") }
if is_dict(win) && !!win.get("_mock", false) && !common.env_truthy("NYTRIX_UI_REPLAY") {
   if !gfx.begin_frame_clear(BG) { panic("Pong renderer could not begin a frame") }
   if !gfx.end_frame() { panic("Pong renderer could not present a frame") }
   gfx.close_window()
   exit(0)
}

fixture.ready("pong")
def headless = gfx.get_active_backend_name() == "mock"
def int legacy_frames = common.env_int_clamped("NYTRIX_PONG_MAX_FRAMES", 0, 0, 1000000)
def int dump_delay = ui_dump.auto_dump_delay_frames(8)
def int legacy_timeout = common.env_int_clamped("NYTRIX_PONG_TIMEOUT_MS", headless ? 1000 : 0, 0, 3600000)
def fx = fixture.start_ui(legacy_frames, legacy_timeout, headless && !common.env_present("NYTRIX_PONG_MAX_FRAMES"))
def bool replay = common.env_truthy("NYTRIX_UI_REPLAY")
def bool trace = common.env_truthy("NYTRIX_PONG_TRACE")
def int trace_every = common.env_int_clamped("NYTRIX_PONG_TRACE_EVERY", 30, 1, 600)
mut int font = 0
mut int font_small = 0
mut int score_font_size = 0
mut int label_font_size = 0

mut int player_score = 0
mut int opponent_score = 0
mut f64 scale = 1.0
mut f64 ball_x = 0.0
mut f64 ball_y = 0.0
mut f64 ball_vx = 0.0
mut f64 ball_vy = 0.0
mut f64 serve_dir = 1.0
mut int hits = 0
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
mut int replay_frame = 0
mut bool dump_done = false
;; The court net is static between resizes: build its packed vertices once.
def int NET_RECTS = (NET_COUNT + 1) / 2
def ptr net_rects = malloc(NET_RECTS * 20)
if !net_rects { panic("could not allocate Pong net geometry") }
def int net_color = gfx.color_pack(128.0 / 255.0, 125.0 / 255.0, 117.0 / 255.0, 1.0)
mut f64 net_w = -1.0
mut f64 net_h = -1.0
mut f64 previous_w = 0.0
mut f64 previous_h = 0.0

while !gfx.window_should_close(win) && fixture.tick_ui(fx) {
   def int frame_started = ticks()
   window.poll_events()
   if fixture.quit_key(fixture.next_ui_key(fx)) { break }
   ;; Read the live native framebuffer, not the GL renderer's cached extent;
   ;; Wayland configure events can resize the window between frames.
   def fb = window.get_framebuffer_size(win)
   def f64 w = float(fb.get(0, WIN_W))
   def f64 h = float(fb.get(1, WIN_H))
   def f64 previous_scale = scale
   scale = float(math.min(w / VIEW_W, h / VIEW_H))
   if scale <= 0.0 { scale = 1.0 }
   if initialized && previous_w > 0.0 && previous_h > 0.0 &&
      (w != previous_w || h != previous_h) {
      ;; Preserve relative object centers and speed when framebuffer size or
      ;; DPI changes. Retaining old pixel positions can strand the ball outside
      ;; the resized court, especially during the serve countdown.
      def f64 old_ball = BALL_SIZE * previous_scale
      def f64 new_ball = BALL_SIZE * scale
      ball_x = (float(ball_x) + float(old_ball) * 0.5) * w / previous_w - float(new_ball) * 0.5
      ball_y = (float(ball_y) + float(old_ball) * 0.5) * h / previous_h - float(new_ball) * 0.5
      ball_x = lim(ball_x, 0.0, w - new_ball)
      ball_y = lim(ball_y, 0.0, h - new_ball)
      player_y = (player_y + PADDLE_H * previous_scale * 0.5) * h / previous_h - PADDLE_H * scale * 0.5
      opponent_y = (opponent_y + PADDLE_H * previous_scale * 0.5) * h / previous_h - PADDLE_H * scale * 0.5
      ball_vx *= scale / previous_scale
      ball_vy *= scale / previous_scale
   }
   previous_w, previous_h = w, h
   def int next_score_size = int(math.max(16.0, math.floor(48.0 * scale + 0.5)))
   def int next_label_size = int(math.max(10.0, math.floor(20.0 * scale + 0.5)))
   if next_score_size != score_font_size {
      def next_font = gfx.font_load_first(["etc/assets/fonts/monocraft.ttf"], next_score_size, gfx.FONT_FILTER_DEFAULT, "0123456789PAUSED")
      if next_font > 0 {
         if font > 0 { gfx.font_destroy(font) }
         font = next_font
         gfx.font_prepare(font, "0123456789PAUSED")
         score_font_size = next_score_size
      }
   }
   if next_label_size != label_font_size {
      def next_font = gfx.font_load_first(["etc/assets/fonts/monocraft.ttf"], next_label_size, gfx.FONT_FILTER_DEFAULT, "YOU CPU W/S MOVE - SPACE PAUSE - R RESET")
      if next_font > 0 {
         if font_small > 0 { gfx.font_destroy(font_small) }
         font_small = next_font
         gfx.font_prepare(font_small, "YOU CPU W/S MOVE - SPACE PAUSE - R RESET")
         label_font_size = next_label_size
      }
   }
   if !gfx.begin_frame_clear(BG) { continue }
   def f64 ball = BALL_SIZE * scale
   def f64 paddle_w = PADDLE_W * scale
   def f64 paddle_h = PADDLE_H * scale
   ;; Keep a useful side inset on very wide Wayland allocations while still
   ;; scaling the court proportionally in narrow and portrait windows.
   def f64 margin = lim(PADDLE_MARGIN * scale, w * 0.05, w * 0.12)
   def f64 speed = PADDLE_SPEED * scale
   def f64 player_x = margin
   def f64 opponent_x = w - margin - paddle_w
   def f64 dt = float(math.clamp(gfx.get_delta_time(), 0.0, 0.05))

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
      serve_dir = 1.0
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

         def f64 prev_x = ball_x
         def f64 prev_y = ball_y
         ball_x += ball_vx * dt
         ball_y += ball_vy * dt

         if ball_y < 0.0 { ball_y = 0.0 ball_vy = float(math.abs(ball_vy)) }
         if ball_y + ball > h { ball_y = h - ball ball_vy = -float(math.abs(ball_vy)) }

         ;; Paddle hits: either the ball overlaps the paddle in front of its centre, or
         ;; it crossed the paddle face between two frames (no tunnelling at low fps).
         if ball_vx < 0.0 {
            def f64 face = player_x + paddle_w
            mut f64 cross_y = ball_y
            if prev_x > ball_x && prev_x >= face && ball_x <= face {
               cross_y = prev_y + (ball_y - prev_y) * ((prev_x - face) / (prev_x - ball_x))
            }
            def bool crossed = prev_x >= face && ball_x <= face
            def bool overlap = ball_x <= face && ball_x + ball >= player_x &&
               ball_x + ball * 0.5 >= player_x + paddle_w * 0.5
            if (crossed || overlap) && cross_y + ball >= player_y && cross_y <= player_y + paddle_h {
               ball_x = face
               ball_y = lim(cross_y, 0.0, h - ball)
               bounce_off_paddle(1.0, player_y, paddle_h, ball)
            }
         } elif ball_vx > 0.0 {
            def f64 face = opponent_x - ball
            mut f64 cross_y = ball_y
            if prev_x < ball_x && prev_x <= face && ball_x >= face {
               cross_y = prev_y + (ball_y - prev_y) * ((face - prev_x) / (ball_x - prev_x))
            }
            def bool crossed = prev_x <= face && ball_x >= face
            def bool overlap = ball_x + ball >= opponent_x && ball_x <= opponent_x + paddle_w &&
               ball_x + ball * 0.5 <= opponent_x + paddle_w * 0.5
            if (crossed || overlap) && cross_y + ball >= opponent_y && cross_y <= opponent_y + paddle_h {
               ball_x = face
               ball_y = lim(cross_y, 0.0, h - ball)
               bounce_off_paddle(-1.0, opponent_y, paddle_h, ball)
            }
         }

         if ball_x + ball < 0.0 {
            opponent_score += 1
            rally += 1
            serve_dir = -1.0
            place_ball(w, h, ball, rally)
            serve_t = SERVE_TIME
         } elif ball_x > w {
            player_score += 1
            rally += 1
            serve_dir = 1.0
            place_ball(w, h, ball, rally)
            serve_t = SERVE_TIME
         }
      }
   }

   def f64 dash_h = h / float(NET_COUNT)
   if w != net_w || h != net_h {
      mut int dash = 0
      while dash < NET_RECTS {
         def int off = dash * 20
         store32_f32(net_rects, w * 0.5 - 1.5 * scale, off)
         store32_f32(net_rects, float(dash * 2) * dash_h, off + 4)
         store32_f32(net_rects, 3.0 * scale, off + 8)
         store32_f32(net_rects, dash_h * 0.56, off + 12)
         store32(net_rects, net_color, off + 16)
         dash += 1
      }
      net_w, net_h = w, h
   }
   gfx.draw_rects_fast_ptr(net_rects, NET_RECTS)

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

   if replay || (trace && replay_frame % trace_every == 0) {
      def live_window_size = window.size(win)
      def live_framebuffer_size = window.get_framebuffer_size(win)
      print("NYTRIX_UI_APP_STATE demo=pong frame=" + to_str(replay_frame + 1) +
         " scale=" + to_str(scale) + " ball=" + to_str(ball_x) + "," + to_str(ball_y) +
         " velocity=" + to_str(ball_vx) + "," + to_str(ball_vy) +
         " paddles=" + to_str(player_y) + "," + to_str(opponent_y) +
         " score=" + to_str(player_score) + ":" + to_str(opponent_score) +
         " serve=" + to_str(serve_t) + " rally=" + to_str(rally) +
         " fonts=" + to_str(score_font_size) + "," + to_str(label_font_size) +
         " dash=" + to_str(dash_h) + " size=" + to_str(w) + "x" + to_str(h) +
         " api=" + to_str(live_window_size.get(0, 0)) + "x" + to_str(live_window_size.get(1, 0)) +
         " fb=" + to_str(live_framebuffer_size.get(0, 0)) + "x" + to_str(live_framebuffer_size.get(1, 0)) +
         " paused=" + to_str(paused) + " w=" + to_str(window.key_down(win, KEY_W)) +
         " s=" + to_str(window.key_down(win, KEY_S)))
   }
   ui_dump.auto_dump_pre_frame(dump_done, replay_frame, dump_delay)
   if !gfx.end_frame() { panic("Pong renderer could not present a frame") }
   replay_frame += 1
   dump_done = ui_dump.auto_dump_post_frame(win, dump_done, replay_frame, dump_delay, "build/cache/fb/ui/pong.png")
   if first_frame {
      if !window.show(win) { panic("Pong window could not be shown") }
      first_frame = false
   }
   if frame_times {
      print("PONG_FRAME_TIME frame=" + to_str(replay_frame) +
         " elapsed_ns=" + to_str(ticks() - startup_started))
   }
   ;; Vsync is advisory on some window routes. Bound CPU use when presentation
   ;; returns immediately, using real elapsed time rather than advancing the
   ;; simulation by an artificial minimum delta.
   if !replay {
      def int remaining = 8333333 - (ticks() - frame_started)
      if remaining > 0 { msleep((remaining + 999999) / 1000000) }
   }
}

free(net_rects)
if font > 0 { gfx.font_destroy(font) }
if font_small > 0 { gfx.font_destroy(font_small) }
gfx.close_window()
exit(fixture.exit_status(fx))
