#!/usr/bin/env ny

;; Keywords: ui breakout bricks game example
;; Brick-breaking arcade game written for Nytrix: steer the paddle, keep the
;; ball in play, and clear the wall. Original design in the breakout genre.
;;
;; Controls: A/D or Left/Right move, Space launches or pauses, R restarts,
;; Esc quits. `--selftest` checks the collision rules and exits.

use core
use core.os (exit)
use core.os.args as process_args
use core.os.fixture as fixture
use core.common as common
use core.math as math
use core.os.ui.render as gfx
use core.os.ui.window as window
use core.os.ui.window.consts (KEY_A, KEY_D, KEY_LEFT, KEY_RIGHT, KEY_SPACE, KEY_R, KEY_ESCAPE, WINDOW_MAXIMIZE)

;; The playfield is laid out in a fixed virtual space and scaled to the window.
def f64 VIEW_W, VIEW_H = 960.0, 640.0
def int COLS, ROWS = 12, 7
def f64 WALL_TOP, WALL_SIDE = 84.0, 36.0
def f64 BRICK_GAP, BRICK_H = 6.0, 22.0
def f64 PADDLE_W, PADDLE_H, PADDLE_Y, PADDLE_SPEED = 120.0, 14.0, 596.0, 640.0
def f64 BALL, BALL_SPEED, BALL_MAX_SPEED = 12.0, 360.0, 620.0
def f64 MAX_ANGLE = 1.05
def int START_LIVES = 3
def f64 ENTRY_DURATION, BURST_DURATION = 0.85, 0.28
def f64 WALL_INTRO_DURATION, SCORE_POPUP_DURATION, RESULT_FADE_DURATION = 0.82, 0.76, 0.34

def BG = gfx.color_hex("#10151C")
def PANEL = gfx.color_hex("#1A2230")
def FG = gfx.color_hex("#EDE6D6")
def MUTED = gfx.color_hex("#7D8796")
def BALL_COLOR = gfx.color_hex("#FFF4C2")
def ROW_COLORS = [gfx.color_hex("#E4572E"), gfx.color_hex("#F3A712"), gfx.color_hex("#E8D44D"),
                  gfx.color_hex("#76B041"), gfx.color_hex("#17BEBB"), gfx.color_hex("#4F6CDB"),
                  gfx.color_hex("#9A5FD1")]
def CRACKED = gfx.color_hex("#5B6474")

fn lim(f64 v, f64 lo, f64 hi) f64 { float(math.min(math.max(v, lo), hi)) }

fn brick_w() f64 { (VIEW_W - WALL_SIDE * 2.0 - BRICK_GAP * float(COLS - 1)) / float(COLS) }
fn brick_x(int col) f64 { WALL_SIDE + float(col) * (brick_w() + BRICK_GAP) }
fn brick_y(int row) f64 { WALL_TOP + float(row) * (BRICK_H + BRICK_GAP) }

;; The top two rows take two hits; the rest break on the first.
fn row_hp(int row) int { row < 2 ? 2 : 1 }
fn row_points(int row) int { (ROWS - row) * 10 }

fn overlaps(f64 ax, f64 ay, f64 aw, f64 ah, f64 bx, f64 by, f64 bw, f64 bh) bool {
   ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by
}

;; Exit angle from where the ball meets the paddle: centre goes straight up,
;; edges leave at MAX_ANGLE. Returns the offset in [-1, 1].
fn paddle_offset(f64 ball_center, f64 paddle_x) f64 {
   def f64 half = PADDLE_W * 0.5 + BALL * 0.5
   lim((ball_center - (paddle_x + PADDLE_W * 0.5)) / half, -1.0, 1.0)
}

;; Brick hit points live in a byte buffer (row-major); 0 means cleared.
def ptr bricks = malloc(COLS * ROWS)
if !bricks { panic("could not allocate the brick wall") }

fn hp_at(int col, int row) int { load8(bricks, row * COLS + col) }
fn set_hp(int col, int row, int hp) { store8(bricks, hp, row * COLS + col) }

fn reset_wall() int {
   mut int live = 0
   mut int r = 0
   while r < ROWS {
      mut int c = 0
      while c < COLS {
         set_hp(c, r, row_hp(r))
         live += 1
         c += 1
      }
      r += 1
   }
   live
}

;; Returns the index (row * COLS + col) of the first live brick overlapping the
;; ball box, or -1. Bricks are scanned in a fixed order so collisions are
;; deterministic.
fn brick_hit(f64 x, f64 y) int {
   if y > brick_y(ROWS) || y + BALL < WALL_TOP { return -1 }
   def f64 bw = brick_w()
   mut int r = 0
   while r < ROWS {
      def f64 by = brick_y(r)
      if y < by + BRICK_H && y + BALL > by {
         mut int c = 0
         while c < COLS {
            if hp_at(c, r) > 0 && overlaps(x, y, BALL, BALL, brick_x(c), by, bw, BRICK_H) {
               return r * COLS + c
            }
            c += 1
         }
      }
      r += 1
   }
   -1
}

mut f64 ball_x = 0.0
mut f64 ball_y = 0.0
mut f64 ball_vx = 0.0
mut f64 ball_vy = 0.0
mut f64 paddle_x = (VIEW_W - PADDLE_W) * 0.5
mut int score = 0
mut int lives = START_LIVES
mut int bricks_left = 0
;; Consecutive cleared bricks on one ball ramp the score up to 5x.
mut int chain = 0
;; One deterministic impact burst is reused for brick, paddle, and life hits.
mut f64 burst_x = 0.0
mut f64 burst_y = 0.0
mut f64 burst_time = 0.0
mut vec4 burst_color = BALL_COLOR
;; A serve enters from below the paddle rather than appearing instantly.
mut f64 entry_time = 0.0
mut f64 wall_intro_time = 0.0
mut int popup_score = 0
mut int popup_chain = 0
mut f64 popup_x = 0.0
mut f64 popup_y = 0.0
mut f64 popup_time = 0.0
mut f64 result_time = 0.0
;; 0 serve, 1 play, 2 paused, 3 won, 4 lost
mut int mode = 0

fn ball_speed() f64 { float(math.sqrt(ball_vx * ball_vx + ball_vy * ball_vy)) }

fn set_velocity(f64 offset, f64 speed) {
   def f64 angle = offset * MAX_ANGLE
   ball_vx = speed * float(math.sin(angle))
   ball_vy = -speed * float(math.cos(angle))
}

fn park_ball() {
   ball_x = paddle_x + PADDLE_W * 0.5 - BALL * 0.5
   ball_y = PADDLE_Y - BALL - 1.0
}

fn burst_at(f64 x, f64 y, vec4 color) {
   burst_x, burst_y, burst_color, burst_time = x, y, color, BURST_DURATION
}

fn new_game() {
   bricks_left = reset_wall()
   score = 0
   lives = START_LIVES
   chain = 0
   burst_time = 0.0
   entry_time = ENTRY_DURATION
   wall_intro_time = 0.0
   popup_score, popup_chain, popup_time = 0, 0, 0.0
   result_time = 0.0
   paddle_x = (VIEW_W - PADDLE_W) * 0.5
   mode = 0
   park_ball()
   burst_at(ball_x + BALL * 0.5, ball_y + BALL * 0.5, BALL_COLOR)
}

fn launch() {
   chain = 0
   entry_time = 0.0
   ;; A slight fixed lean so the first bounce is never a vertical loop.
   set_velocity(0.18, BALL_SPEED)
   mode = 1
}

fn handle_space() {
   if mode == 0 { launch() }
   elif mode == 1 { mode = 2 }
   elif mode == 2 { mode = 1 }
   elif mode >= 3 { new_game() }
}

fn damage(int index) {
   def int col = index % COLS
   def int row = index / COLS
   def int hp = hp_at(col, row) - 1
   set_hp(col, row, hp)
   burst_at(brick_x(col) + brick_w() * 0.5, brick_y(row) + BRICK_H * 0.5,
      hp > 0 ? CRACKED : ROW_COLORS.get(row, FG))
   if hp <= 0 {
      if chain < 5 { chain += 1 }
      def int award = row_points(row) * chain
      score += award
      popup_score, popup_chain = award, chain
      popup_x, popup_y, popup_time =
         brick_x(col) + brick_w() * 0.5, brick_y(row), SCORE_POPUP_DURATION
      bricks_left -= 1
      if bricks_left <= 0 {
         mode = 3
         result_time = 0.0
      }
   }
   ;; Each brick hit speeds the ball up a little, up to a ceiling.
   def f64 s = ball_speed()
   if s > 0.0 && s < BALL_MAX_SPEED {
      def f64 k = float(math.min(BALL_MAX_SPEED, s * 1.015)) / s
      ball_vx *= k
      ball_vy *= k
   }
}

;; One physics substep. Movement is applied per axis: a brick met while
;; moving horizontally reflects vx, one met vertically reflects vy. That keeps
;; corner and edge contacts well defined (a corner hit reflects only the axis
;; that entered the brick) and never removes two bricks in one axis move.
fn step(f64 dt) {
   ball_x += ball_vx * dt
   if ball_x < 0.0 {
      ball_x = 0.0
      ball_vx = float(math.abs(ball_vx))
      burst_at(0.0, ball_y + BALL * 0.5, MUTED)
   }
   if ball_x + BALL > VIEW_W {
      ball_x = VIEW_W - BALL
      ball_vx = -float(math.abs(ball_vx))
      burst_at(VIEW_W, ball_y + BALL * 0.5, MUTED)
   }
   def int hx = brick_hit(ball_x, ball_y)
   if hx >= 0 {
      ball_x -= ball_vx * dt
      ball_vx = -ball_vx
      damage(hx)
   }
   ball_y += ball_vy * dt
   if ball_y < WALL_TOP - 40.0 {
      ball_y = WALL_TOP - 40.0
      ball_vy = float(math.abs(ball_vy))
      burst_at(ball_x + BALL * 0.5, ball_y, MUTED)
   }
   def int hy = brick_hit(ball_x, ball_y)
   if hy >= 0 {
      ball_y -= ball_vy * dt
      ball_vy = -ball_vy
      damage(hy)
   }
   ;; Paddle: only while descending, so a ball already leaving cannot stick.
   if ball_vy > 0.0 && overlaps(ball_x, ball_y, BALL, BALL, paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H) {
      ball_y = PADDLE_Y - BALL
      chain = 0
      burst_at(ball_x + BALL * 0.5, PADDLE_Y, BALL_COLOR)
      set_velocity(paddle_offset(ball_x + BALL * 0.5, paddle_x), ball_speed())
   }
   if ball_y > VIEW_H {
      lives -= 1
      chain = 0
      burst_at(ball_x + BALL * 0.5, VIEW_H - 2.0, CRACKED)
      if lives <= 0 {
         mode = 4
         result_time = 0.0
      } else {
         mode = 0
      }
      park_ball()
      entry_time = lives > 0 ? ENTRY_DURATION : 0.0
   }
}

fn advance(f64 dt) {
   ;; Substeps keep each move under half a ball so nothing tunnels at low fps.
   def f64 travel = ball_speed() * dt
   mut int n = int(math.ceil(travel / (BALL * 0.5)))
   if n < 1 { n = 1 }
   if n > 64 { n = 64 }
   def f64 sub = dt / float(n)
   mut int i = 0
   while i < n && mode == 1 {
      step(sub)
      i += 1
   }
}

;; ---------------------------------------------------------------- self test

fn check(bool ok, str what) {
   if !ok {
      eprint("breakout selftest FAILED: " + what)
      exit(1)
   }
}

fn selftest() {
   new_game()
   check(bricks_left == COLS * ROWS, "wall starts full")
   check(entry_time == ENTRY_DURATION && burst_time == BURST_DURATION,
      "new serve starts with a visible entry effect")
   handle_space()
   check(mode == 1 && entry_time == 0.0, "space launches from the animated serve")
   new_game()
   ;; Straight up into the bottom face of a single-hit brick: vy flips, brick goes.
   def int row = ROWS - 1
   def int col = 3
   ball_x = brick_x(col) + brick_w() * 0.5 - BALL * 0.5
   ball_y = brick_y(row) + BRICK_H + 1.0
   ball_vx = 0.0
   ball_vy = -300.0
   mode = 1
   step(0.01)
   check(ball_vy > 0.0, "bottom face reflects vy")
   check(hp_at(col, row) == 0, "single-hit brick removed")
   check(bricks_left == COLS * ROWS - 1, "brick count drops by one")
   check(score == row_points(row), "score adds row points")
   check(chain == 1, "first clear starts a 1x chain")
   ;; Horizontal approach into a side face: vx flips, vy kept.
   def int col2 = 6
   set_hp(col2 - 1, row, 0) ; the approach lane must be empty
   bricks_left -= 1
   ball_x = brick_x(col2) - BALL - 1.0
   ball_y = brick_y(row) + 4.0
   ball_vx = 300.0
   ball_vy = 0.0
   step(0.01)
   check(ball_vx < 0.0, "side face reflects vx")
   check(ball_vy == 0.0, "side hit keeps vy")
   check(hp_at(col2, row) == 0, "side hit removes brick")
   ;; Consecutive clears build a bounded score chain.
   new_game()
   mut int chain_col = 0
   while chain_col < 7 {
      damage((ROWS - 1) * COLS + chain_col)
      chain_col += 1
   }
   check(chain == 5, "score chain caps at 5x")
   check(score == row_points(ROWS - 1) * 25, "chain multiplier scores consecutive clears")
   check(popup_score == row_points(ROWS - 1) * 5 && popup_chain == 5,
      "score popup captures the awarded chain")
   mode = 1
   handle_space()
   check(mode == 2, "space pauses active play")
   handle_space()
   check(mode == 1, "space resumes paused play")
   score = 123
   lives = 0
   mode = 4
   handle_space()
   check(mode == 0 && score == 0 && lives == START_LIVES,
      "space restarts a finished game")
   ;; Two-hit brick survives the first hit (column 0 below it cleared first).
   mut int below = 2
   while below < ROWS { set_hp(0, below, 0) below += 1 }
   ball_x = brick_x(0) + 4.0
   ball_y = brick_y(1) + BRICK_H + 1.0
   ball_vx = 0.0
   ball_vy = -300.0
   step(0.01)
   check(hp_at(0, 1) == 1, "two-hit brick cracks first")
   ;; The gap between two bricks lets a vertical ball through untouched.
   new_game()
   mode = 1
   ball_x = brick_x(4) + brick_w() + BRICK_GAP * 0.5 - BALL * 0.5
   check(BALL > BRICK_GAP, "ball is wider than the gap (edge case below)")
   ball_y = brick_y(ROWS) + 2.0
   ball_vx = 0.0
   ball_vy = -300.0
   step(0.04)
   check(ball_vy > 0.0, "a ball overlapping two bricks bounces once")
   check(bricks_left == COLS * ROWS - 1, "and breaks exactly one brick")
   ;; Paddle: centre hit goes straight up, right edge leans right.
   new_game()
   mode = 1
   ball_x = paddle_x + PADDLE_W * 0.5 - BALL * 0.5
   ball_y = PADDLE_Y - BALL + 2.0
   ball_vx = 0.0
   ball_vy = 300.0
   chain = 3
   step(0.001)
   check(ball_vy < 0.0, "paddle reflects upward")
   check(float(math.abs(ball_vx)) < 1.0, "centre hit is near vertical")
   check(chain == 0, "paddle return resets the clear chain")
   ball_x = paddle_x + PADDLE_W - 2.0
   ball_y = PADDLE_Y - BALL + 2.0
   ball_vx = 0.0
   ball_vy = 300.0
   step(0.001)
   check(ball_vx > 100.0, "right edge sends the ball right")
   ;; An ascending ball passing the paddle is not captured.
   ball_x = paddle_x + 10.0
   ball_y = PADDLE_Y + 2.0
   ball_vx = 0.0
   ball_vy = -300.0
   step(0.001)
   check(ball_vy < 0.0, "ascending ball ignores paddle")
   ;; Side walls and the ceiling reflect.
   ball_x = 1.0
   ball_y = 400.0
   ball_vx = -300.0
   ball_vy = -10.0
   step(0.01)
   check(ball_vx > 0.0, "left wall reflects")
   ball_x = VIEW_W - BALL - 1.0
   ball_vx = 300.0
   step(0.01)
   check(ball_vx < 0.0, "right wall reflects")
   ;; Fast ball at a low frame rate does not tunnel through a brick row.
   new_game()
   mode = 1
   ball_x = brick_x(8) + 10.0
   ball_y = brick_y(ROWS) + 30.0
   ball_vx = 0.0
   ball_vy = -BALL_MAX_SPEED
   advance(0.25)
   check(bricks_left < COLS * ROWS, "no tunnelling at 4 fps")
   ;; Missing the ball costs a life; the last one ends the game.
   new_game()
   mode = 1
   lives = 1
   ball_x = 20.0
   ball_y = VIEW_H - 2.0
   ball_vx = 0.0
   ball_vy = 300.0
   step(0.02)
   check(mode == 4 && lives == 0, "last miss ends the game")
   ;; Breaking the last brick wins.
   new_game()
   mode = 1
   mut int r = 0
   while r < ROWS {
      mut int c = 0
      while c < COLS { set_hp(c, r, 0) c += 1 }
      r += 1
   }
   set_hp(0, ROWS - 1, 1)
   bricks_left = 1
   ball_x = brick_x(0) + 4.0
   ball_y = brick_y(ROWS - 1) + BRICK_H + 1.0
   ball_vx = 0.0
   ball_vy = -300.0
   step(0.01)
   check(mode == 3, "clearing the wall wins")
   print("breakout selftest ok")
   exit(0)
}

if process_args.positionals().contains("--selftest") { selftest() }

;; ---------------------------------------------------------------- game loop

gfx.apply_backend_env()
gfx.apply_backend_argv()
;; Keep the initial client area inside a 720p output when maximize is ignored.
mut win = gfx.init_window(1200, 650, "Bricks - Nytrix", WINDOW_MAXIMIZE, true, false, 1)
if !win { panic("could not create the Bricks window") }
fixture.ready("breakout")
def headless = gfx.get_active_backend_name() == "mock"
def fx = fixture.start_ui(0, headless ? 1000 : 0, headless)
def bool replay = common.env_truthy("NYTRIX_UI_REPLAY")
;; In replay the paddle follows the ball so a scripted run exercises play.
def bool autopilot = replay || common.env_truthy("NYTRIX_BREAKOUT_AUTOPILOT")
mut int font = 0
mut int font_small = 0
mut int font_px = 0
mut bool prev_space = false
mut bool prev_r = false
mut bool first_frame = true
mut int frame = 0
new_game()

while !gfx.window_should_close(win) && fixture.tick_ui(fx) {
   def int frame_started = ticks()
   window.poll_events()
   if fixture.quit_key(fixture.next_ui_key(fx)) { break }
   def fb = window.get_framebuffer_size(win)
   def f64 w = float(fb.get(0, 1280))
   def f64 h = float(fb.get(1, 840))
   def f64 scale = float(math.max(0.1, math.min(w / VIEW_W, h / VIEW_H)))
   def f64 ox = (w - VIEW_W * scale) * 0.5
   def f64 oy = (h - VIEW_H * scale) * 0.5
   def int want_px = int(math.max(12.0, math.floor(28.0 * scale + 0.5)))
   if want_px != font_px {
      def nf = gfx.font_load_first(["etc/assets/fonts/monocraft.ttf"], want_px)
      def ns = gfx.font_load_first(["etc/assets/fonts/monocraft.ttf"], int(math.max(10.0, float(want_px) * 0.6)))
      if nf > 0 && ns > 0 {
         if font > 0 { gfx.font_destroy(font) }
         if font_small > 0 { gfx.font_destroy(font_small) }
         font, font_small, font_px = nf, ns, want_px
      }
   }
   if !gfx.begin_frame_clear(BG) { continue }
   gfx.set_ortho_2d(0.0, w, h, 0.0)
   def f64 dt = float(math.clamp(gfx.get_delta_time(), 0.0, 0.05))

   if window.key_down(win, KEY_ESCAPE) { window.set_should_close(win, true) }
   def bool space = window.key_down(win, KEY_SPACE)
   def bool rkey = window.key_down(win, KEY_R)
   if rkey && !prev_r { new_game() }
   if space && !prev_space { handle_space() }
   prev_space, prev_r = space, rkey

   if mode <= 1 {
      mut f64 dir = 0.0
      if window.key_down(win, KEY_A) || window.key_down(win, KEY_LEFT) { dir -= 1.0 }
      if window.key_down(win, KEY_D) || window.key_down(win, KEY_RIGHT) { dir += 1.0 }
      if autopilot {
         ;; Aim a little off-centre so the replay ball sweeps the wall.
         def f64 target = ball_x + BALL * 0.5 - PADDLE_W * 0.5 + (frame % 240 < 120 ? 18.0 : -18.0)
         def f64 d = target - paddle_x
         dir = d > 4.0 ? 1.0 : (d < -4.0 ? -1.0 : 0.0)
         if mode == 0 && frame % 30 == 15 { launch() }
      }
      paddle_x = lim(paddle_x + dir * PADDLE_SPEED * dt, 0.0, VIEW_W - PADDLE_W)
      if mode == 0 { park_ball() } else { advance(dt) }
   }

   if burst_time > 0.0 { burst_time = float(math.max(0.0, burst_time - dt)) }
   if popup_time > 0.0 { popup_time = float(math.max(0.0, popup_time - dt)) }
   if wall_intro_time < WALL_INTRO_DURATION {
      wall_intro_time = float(math.min(WALL_INTRO_DURATION, wall_intro_time + dt))
   }
   if mode >= 3 && result_time < RESULT_FADE_DURATION {
      result_time = float(math.min(RESULT_FADE_DURATION, result_time + dt))
   }
   if mode == 0 && entry_time > 0.0 {
      entry_time = float(math.max(0.0, entry_time - dt))
   }

   ;; Playfield frame.
   gfx.draw_rect(ox, oy, VIEW_W * scale, VIEW_H * scale, PANEL)
   def f64 bw = brick_w()
   mut int r = 0
   while r < ROWS {
      def f64 row_delay = float(r) * 0.045
      def f64 row_progress = lim(
         (wall_intro_time - row_delay) / (WALL_INTRO_DURATION - row_delay), 0.0, 1.0)
      def f64 row_y = brick_y(r) - (1.0 - row_progress) * 48.0
      mut int c = 0
      while c < COLS {
         def int hp = hp_at(c, r)
         if hp > 0 {
            def base_color = hp < row_hp(r) ? CRACKED : ROW_COLORS.get(r, FG)
            def color = gfx.color_alpha(base_color, 0.22 + 0.78 * row_progress)
            gfx.draw_rect(ox + brick_x(c) * scale, oy + row_y * scale,
               bw * scale, BRICK_H * scale, color)
         }
         c += 1
      }
      r += 1
   }
   gfx.draw_rect(ox + paddle_x * scale, oy + PADDLE_Y * scale, PADDLE_W * scale, PADDLE_H * scale, FG)
   if mode == 1 {
      mut int trail = 1
      while trail <= 3 {
         def f64 back = float(trail) * 0.045
         def f64 trail_x = ball_x - ball_vx * back
         def f64 trail_y = ball_y - ball_vy * back
         def color = gfx.color_alpha(BALL_COLOR, 0.20 / float(trail))
         if trail_x >= 0.0 && trail_x + BALL <= VIEW_W &&
            trail_y >= 0.0 && trail_y + BALL <= VIEW_H {
            gfx.draw_rect(ox + trail_x * scale, oy + trail_y * scale,
               BALL * scale, BALL * scale, color)
         }
         trail += 1
      }
   }
   def f64 entry_progress = lim(1.0 - entry_time / ENTRY_DURATION, 0.0, 1.0)
   def f64 entry_remaining = 1.0 - entry_progress
   def f64 entry_eased = 1.0 - entry_remaining * entry_remaining * entry_remaining
   def f64 ball_scale = mode == 0 && entry_time > 0.0 ? 0.45 + 0.55 * entry_eased : 1.0
   def f64 shown_ball = BALL * ball_scale
   def f64 shown_ball_y = mode == 0 && entry_time > 0.0 ?
      ball_y + (1.0 - entry_eased) * 72.0 : ball_y
   if mode == 0 && entry_time > 0.0 {
      def f64 glow = BALL * (1.5 + 0.8 * (1.0 - entry_eased))
      def color = gfx.color_alpha(BALL_COLOR, 0.24 * (1.0 - entry_eased))
      gfx.draw_rect(ox + (ball_x + BALL * 0.5 - glow * 0.5) * scale,
         oy + (shown_ball_y + shown_ball * 0.5 - glow * 0.5) * scale,
         glow * scale, glow * scale, color)
      mut int ghost = 1
      while ghost <= 3 {
         def f64 ghost_size = shown_ball * (0.86 - float(ghost) * 0.12)
         def f64 ghost_y = shown_ball_y + shown_ball * 0.5 + float(ghost) * 18.0 - ghost_size * 0.5
         def f64 alpha = (entry_time / ENTRY_DURATION) * (0.40 / float(ghost))
         gfx.draw_rect(ox + (ball_x + BALL * 0.5 - ghost_size * 0.5) * scale,
            oy + ghost_y * scale, ghost_size * scale, ghost_size * scale,
            gfx.color_alpha(BALL_COLOR, alpha))
         ghost += 1
      }
   }
   gfx.draw_rect(ox + (ball_x + (BALL - shown_ball) * 0.5) * scale,
      oy + (shown_ball_y + (BALL - shown_ball) * 0.5) * scale,
      shown_ball * scale, shown_ball * scale,
      gfx.color_alpha(BALL_COLOR, mode == 0 && entry_time > 0.0 ? 0.42 + 0.58 * entry_eased : 1.0))
   if burst_time > 0.0 {
      def f64 progress = 1.0 - burst_time / BURST_DURATION
      def f64 radius = (7.0 + 34.0 * float(progress)) * scale
      def f64 spark = (5.0 - 2.0 * float(progress)) * scale
      def color = gfx.color_alpha(burst_color, burst_time / BURST_DURATION)
      mut int i = 0
      while i < 8 {
         def f64 angle = float(i) * 0.7853981633974483
         def f64 distance = 7.0 + radius / scale * float(progress)
         def f64 sx = ox + (burst_x + float(math.cos(angle)) * distance) * scale - spark * 0.5
         def f64 sy = oy + (burst_y + float(math.sin(angle)) * distance) * scale - spark * 0.5
         gfx.draw_rect(sx, sy, spark, spark, color)
         i += 1
      }
   }
   if mode == 2 {
      gfx.draw_rect(ox, oy, VIEW_W * scale, VIEW_H * scale, gfx.color_alpha(BG, 0.72))
   } elif mode >= 3 {
      def f64 fade = lim(result_time / RESULT_FADE_DURATION, 0.0, 1.0)
      gfx.draw_rect(ox, oy, VIEW_W * scale, VIEW_H * scale, gfx.color_alpha(BG, 0.52 * fade))
   }
   gfx.draw_text(font_small, "SCORE " + to_str(score), ox + 16.0 * scale, oy + 18.0 * scale, FG)
   gfx.draw_text(font_small, "LIVES " + to_str(lives), ox + (VIEW_W - 150.0) * scale, oy + 18.0 * scale, FG)
   def f64 mid_x = ox + VIEW_W * 0.5 * scale
   def f64 mid_y = oy + VIEW_H * 0.62 * scale
   if chain > 1 {
      gfx.draw_text_centered(font_small, "STREAK x" + to_str(chain), mid_x, oy + 18.0 * scale, BALL_COLOR)
   }
   if popup_time > 0.0 {
      def f64 rise = (1.0 - popup_time / SCORE_POPUP_DURATION) * 26.0
      def f64 alpha = popup_time / SCORE_POPUP_DURATION
      def popup_label = "+" + to_str(popup_score) +
         (popup_chain > 1 ? "  x" + to_str(popup_chain) : "")
      gfx.draw_text_centered(font_small, popup_label, ox + popup_x * scale,
         oy + (popup_y - rise) * scale, gfx.color_alpha(BALL_COLOR, alpha))
   }
   def f64 result_pulse = 0.84 + 0.16 * float(math.sin(float(frame) * 0.08))
   def result_color = gfx.color_alpha(mode == 3 ? BALL_COLOR : FG, result_pulse)
   if mode == 0 { gfx.draw_text_centered(font_small, "SPACE TO LAUNCH  -  A/D MOVE", mid_x, mid_y, MUTED) }
   elif mode == 2 { gfx.draw_text_centered(font, "PAUSED", mid_x, mid_y, FG) }
   elif mode == 3 { gfx.draw_text_centered(font, "WALL CLEARED - SPACE TO PLAY AGAIN", mid_x, mid_y, result_color) }
   elif mode == 4 { gfx.draw_text_centered(font, "GAME OVER - SPACE TO RETRY", mid_x, mid_y, result_color) }

   if replay {
      print("NYTRIX_UI_APP_STATE demo=breakout frame=" + to_str(frame + 1) + " mode=" + to_str(mode) +
         " score=" + to_str(score) + " lives=" + to_str(lives) + " bricks=" + to_str(bricks_left) +
         " ball=" + to_str(ball_x) + "," + to_str(ball_y) + " paddle=" + to_str(paddle_x))
   }
   if !gfx.end_frame() { panic("Bricks renderer could not present a frame") }
   frame += 1
   if first_frame {
      window.show(win)
      first_frame = false
   }
   if !replay {
      def int remaining = 8333333 - (ticks() - frame_started)
      if remaining > 0 { msleep((remaining + 999999) / 1000000) }
   }
}

free(bricks)
if font > 0 { gfx.font_destroy(font) }
if font_small > 0 { gfx.font_destroy(font_small) }
gfx.close_window()
exit(fixture.exit_status(fx))
