#!/usr/bin/env ny

;; Keywords: ui breakout bricks arcade game animation example
;; Brick-breaking arcade game written for Nytrix: steer the paddle, keep the
;; ball in play, and clear the wall. Original design in the breakout genre.
;;
;; Controls: A/D or Left/Right move, Space/Enter launches or pauses, R restarts,
;; Esc quits. `--selftest` checks the collision rules and exits.

use core
use core.os (exit)
use core.os.args as process_args
use core.os.fixture as fixture
use core.common as common
use core.math as math
use core.os.ui.render as gfx
use core.os.ui.window as window
use core.os.ui.window.consts (KEY_A, KEY_D, KEY_LEFT, KEY_RIGHT, KEY_SPACE, KEY_ENTER, KEY_R, KEY_ESCAPE, WINDOW_MAXIMIZE)
def f64 VIEW_W, VIEW_H = 960.0, 640.0
def int COLS, ROWS = 12, 7
def f64 WALL_TOP, WALL_SIDE = 84.0, 36.0
def f64 BRICK_GAP = 6.0
def f64 BRICK_H = 22.0
def f64 PADDLE_W, PADDLE_H, PADDLE_Y, PADDLE_SPEED = 120.0, 14.0, 596.0, 640.0
def f64 BALL, BALL_SPEED, BALL_MAX_SPEED = 12.0, 360.0, 620.0
def f64 MAX_ANGLE = 1.05
def int START_LIVES = 3
def f64 ENTRY_DURATION, BURST_DURATION = 0.58, 0.32
def f64 WALL_INTRO_DURATION, SCORE_POPUP_DURATION, RESULT_FADE_DURATION = 0.68, 0.76, 0.38

def f64 DEATH_DURATION = 0.42
def int FX_CAP, FX_STRIDE, TRAIL_CAP = 32, 28, 12
def int PCAP, PS = 512, 32
def f64 FIXED = 1.0/240.0

def BG = gfx.color_hex("#000000")
def PANEL = BG
def FG = gfx.color_hex("#EDE6D6")
def MUTED = gfx.color_hex("#858585")
def BALL_COLOR = gfx.color_hex("#FFF4C2")
def ROW_COLORS = [gfx.color_hex("#E4572E"), gfx.color_hex("#F3A712"), gfx.color_hex("#E8D44D"),
                  gfx.color_hex("#76B041"), gfx.color_hex("#17BEBB"), gfx.color_hex("#4F6CDB"),
                  gfx.color_hex("#9A5FD1")]
def TRACK = gfx.color_hex("#252525")

fn lim(f64 v, f64 lo, f64 hi) f64 { float(math.min(math.max(v, lo), hi)) }

fn ease(f64 t) f64 { def f64 v = 1.0-lim(t,0.0,1.0) 1.0-v*v*v }
fn smooth(f64 t) f64 { def f64 v = lim(t,0.0,1.0) v*v*(3.0-2.0*v) }
fn brick_w() f64 { (VIEW_W - WALL_SIDE * 2.0 - BRICK_GAP * float(COLS - 1)) / float(COLS) }
fn brick_x(int col) f64 { WALL_SIDE + float(col) * (brick_w() + BRICK_GAP) }
fn brick_y(int row) f64 { WALL_TOP + float(row) * (BRICK_H + BRICK_GAP) }
fn row_hp(int row) int { row < 2 ? 2 : 1 }
fn row_points(int row) int { (ROWS - row) * 10 }

fn overlaps(f64 ax, f64 ay, f64 aw, f64 ah, f64 bx, f64 by, f64 bw, f64 bh) bool {
   ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by
}
fn paddle_offset(f64 ball_center, f64 paddle_x) f64 {
   def f64 half = PADDLE_W * 0.5 + BALL * 0.5
   lim((ball_center - (paddle_x + PADDLE_W * 0.5)) / half, -1.0, 1.0)
}
def ptr bricks = malloc(COLS * ROWS)
def ptr effects = malloc(FX_CAP*FX_STRIDE)
def ptr trail_buf = malloc(TRAIL_CAP*8)
def ptr particles = malloc(PCAP*PS)
def ptr flashes = malloc(COLS*ROWS*4)
if !particles || !flashes || !bricks || !effects || !trail_buf { panic("could not allocate Breakout state") }

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
mut f64 previous_x = 0.0
mut f64 previous_y = 0.0
mut f64 ball_vx = 0.0
mut f64 ball_vy = 0.0
mut f64 paddle_x = (VIEW_W - PADDLE_W) * 0.5
mut int score = 0
mut int lives = START_LIVES
mut int bricks_left = 0
mut int chain = 0
mut f64 entry_time = 0.0
mut f64 wall_intro_time = 0.0
mut f64 result_time = 0.0
mut int mode = 0
mut int fx_head = 0
mut int phead = 0
mut int serial = 0
mut f64 prev_paddle = (VIEW_W-PADDLE_W)*0.5
mut f64 acc = 0.0
mut f64 pause_fade = 0.0
mut int trail_head = 0
mut int trail_count = 0
mut f64 trail_time = 0.0
mut f64 death_time = 0.0
mut f64 paddle_v = 0.0
mut f64 paddle_hit = 0.0
mut bool launch_pending = false

fn release() { free(flashes) free(particles) free(effects) free(trail_buf) free(bricks) }
fn putq(ptr p,int o,f64 v) { store32(p,int(v*256.0)+1048576,o) }
fn getq(ptr p,int o) f64 { float(load32(p,o)-1048576)/256.0 }
fn clear_trail() { trail_head,trail_count,trail_time = 0,0,0.0 }
fn record_trail() {
   putq(trail_buf,trail_head*8,ball_x+BALL*0.5)
   putq(trail_buf,trail_head*8+4,ball_y+BALL*0.5)
   trail_head = (trail_head+1)%TRAIL_CAP
   if trail_count < TRAIL_CAP { trail_count += 1 }
}
fn rgb(int row,int kind) int {
   if row == 0 { return 0x2e57e4 }
   if row == 1 { return 0x12a7f3 }
   if row == 2 { return 0x4dd4e8 }
   if row == 3 { return 0x41b076 }
   if row == 4 { return 0xbbbe17 }
   if row == 5 { return 0xdb6c4f }
   if row == 6 { return 0xd15f9a }
   kind == 1 ? 0xa0a0a0 : 0xc2f4ff
}
fn rgba(int c,f64 alpha) int { (c & 0xffffff) | (int(lim(alpha,0.0,1.0)*255.0+0.5)<<24) }
fn scatter(f64 x,f64 y,int kind,int row) {
   def int count = kind == 5 ? 36 : (kind == 3 ? 22 : (kind == 2 ? 9 : 7))
   def int color = rgb(row,kind)
   mut int j = 0
   while j < count {
      def int o = phead*PS
      def int seed = serial*73+j*137+19
      def f64 a = float(j)*6.283185307179586/float(count)+float(seed%97)*0.006
      def f64 speed = (kind == 5 ? 190.0 : (kind == 3 ? 135.0 : 85.0))*(0.35+float(seed%71)/100.0)
      mut f64 vx = float(math.cos(a))*speed
      mut f64 vy = float(math.sin(a))*speed
      if kind == 5 || kind == 4 { vy = -float(math.abs(vy))-25.0 }
      if kind == 1 {
         if x < 2.0 { vx = float(math.abs(vx)) }
         elif x > VIEW_W-2.0 { vx = -float(math.abs(vx)) }
         else { vy = float(math.abs(vy)) }
      }
      putq(particles,o,x) putq(particles,o+4,y)
      putq(particles,o+8,vx) putq(particles,o+12,vy)
      store32(particles,0,o+16)
      store32(particles,(kind == 5 ? 96 : (kind == 3 ? 76 : 44))+seed%34,o+20)
      store32(particles,color,o+24)
      store32(particles,3+seed%5,o+28)
      phead = (phead+1)%PCAP
      j += 1
   }
   serial = (serial+1)%65536
}
fn emit(f64 x,f64 y,int kind,int row=-1,int award=0,int streak=0) {
   scatter(x,y,kind,row)
   def int o = fx_head*FX_STRIDE
   putq(effects,o,x) putq(effects,o+4,y) store32(effects,0,o+8)
   store32(effects,kind,o+12) store32(effects,row+1,o+16)
   store32(effects,award,o+20) store32(effects,streak,o+24)
   fx_head = (fx_head+1)%FX_CAP
}
fn age_effects(f64 dt) {
   def int n = int(dt*240.0+0.5)
   mut int i = 0
   while i < FX_CAP {
      def int o = i*FX_STRIDE
      if load32(effects,o+12) != 0 {
         def int t = load32(effects,o+8)+n
         if t >= 183 { store32(effects,0,o+12) }
         else { store32(effects,t,o+8) }
      }
      i += 1
   }
   i = 0
   while i < PCAP {
      def int o = i*PS
      def int life = load32(particles,o+20)
      if life > 0 {
         def int t = load32(particles,o+16)+n
         if t >= life { store32(particles,0,o+20) }
         else { store32(particles,t,o+16) }
      }
      i += 1
   }
   i = 0
   while i < COLS*ROWS {
      def int t = load32(flashes,i*4)
      if t > 0 { store32(flashes,t > n ? t-n : 0,i*4) }
      i += 1
   }
}

fn ball_speed() f64 { float(math.sqrt(ball_vx * ball_vx + ball_vy * ball_vy)) }

fn set_velocity(f64 offset, f64 speed) {
   def f64 angle = offset * MAX_ANGLE
   ball_vx = speed * float(math.sin(angle))
   ball_vy = -speed * float(math.cos(angle))
}

fn park_ball() {
   ball_x = paddle_x + PADDLE_W * 0.5 - BALL * 0.5
   ball_y = PADDLE_Y - BALL - 1.0
   previous_x,previous_y = ball_x,ball_y
}

fn burst_at(f64 x,f64 y,int kind=1) { emit(x,y,kind) }

fn new_game() {
   memset(effects,0,FX_CAP*FX_STRIDE)
   memset(particles,0,PCAP*PS)
   memset(flashes,0,COLS*ROWS*4)
   phead = 0
   serial = 0
   pause_fade = 0.0
   acc = 0.0
   fx_head = 0
   clear_trail()
   death_time,paddle_v,paddle_hit = 0.0,0.0,0.0
   launch_pending = false
   ball_vx,ball_vy = 0.0,0.0
   bricks_left = reset_wall()
   score = 0
   lives = START_LIVES
   chain = 0
   entry_time = ENTRY_DURATION
   wall_intro_time = 0.0
   result_time = 0.0
   paddle_x = (VIEW_W - PADDLE_W) * 0.5
   prev_paddle = paddle_x
   mode = 0
   park_ball()
}

fn launch() {
   launch_pending = false
   clear_trail()
   record_trail()
   chain = 0
   entry_time = 0.0
   set_velocity(0.18, BALL_SPEED)
   mode = 1
}

fn handle_space() {
   if mode == 0 {
      if entry_time > 0.0 || death_time > 0.0 || wall_intro_time < WALL_INTRO_DURATION { launch_pending = true }
      else { launch() }
   }
   elif mode == 1 { mode = 2 }
   elif mode == 2 { mode = 1 }
   elif mode >= 3 { new_game() }
}

fn damage(int index) int {
   def int col = index % COLS
   def int row = index / COLS
   if hp_at(col,row) <= 0 { return 0 }
   def int hp = hp_at(col, row) - 1
   set_hp(col, row, hp)
   store32(flashes,30,index*4)
   if hp > 0 { emit(brick_x(col)+brick_w()*0.5,brick_y(row)+BRICK_H*0.5,2,row) }
   if hp <= 0 {
      if chain < 5 { chain += 1 }
      def int award = row_points(row) * chain
      score += award
      emit(brick_x(col)+brick_w()*0.5,brick_y(row)+BRICK_H*0.5,3,row,award,chain)
      bricks_left -= 1
      if bricks_left <= 0 {
         mode = 3
         result_time = 0.0
      }
   }
   def f64 s = ball_speed()
   if s > 0.0 && s < BALL_MAX_SPEED {
      def f64 k = float(math.min(BALL_MAX_SPEED, s * 1.015)) / s
      ball_vx *= k
      ball_vy *= k
   }
   0
}
fn step(f64 dt) int {
   previous_x,previous_y = ball_x,ball_y
   def f64 prev_x = ball_x
   ball_x += ball_vx * dt
   if ball_x < 0.0 {
      ball_x = 0.0
      ball_vx = float(math.abs(ball_vx))
      burst_at(0.0,ball_y+BALL*0.5)
   }
   if ball_x + BALL > VIEW_W {
      ball_x = VIEW_W - BALL
      ball_vx = -float(math.abs(ball_vx))
      burst_at(VIEW_W,ball_y+BALL*0.5)
   }
   def int hx = ball_vx == 0.0 ? -1 : brick_hit(ball_x, ball_y)
   if hx >= 0 {
      ball_x = prev_x
      ball_vx = -ball_vx
      damage(hx)
      if mode == 3 { return 0 }
   }
   def f64 prev_y = ball_y
   ball_y += ball_vy * dt
   if ball_y < WALL_TOP - 40.0 {
      ball_y = WALL_TOP - 40.0
      ball_vy = float(math.abs(ball_vy))
      burst_at(ball_x+BALL*0.5,ball_y)
   }
   def int hy = ball_vy == 0.0 ? -1 : brick_hit(ball_x, ball_y)
   if hy >= 0 {
      ball_y = prev_y
      ball_vy = -ball_vy
      if hy != hx { damage(hy) }
      if mode == 3 { return 0 }
   }
   if ball_vy > 0.0 && overlaps(ball_x, ball_y, BALL, BALL, paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H) {
      ball_y = PADDLE_Y - BALL
      chain = 0
      burst_at(ball_x+BALL*0.5,PADDLE_Y,4)
      paddle_hit = 0.18
      set_velocity(paddle_offset(ball_x + BALL * 0.5, paddle_x), ball_speed())
   }
   if ball_y + BALL*0.5 >= VIEW_H {
      emit(ball_x+BALL*0.5,VIEW_H-30.0,5)
      death_time = DEATH_DURATION
      clear_trail()
      launch_pending = false
      lives -= 1
      chain = 0
      if lives <= 0 {
         mode = 4
         result_time = 0.0
      } else {
         mode = 0
      }
      ball_vx,ball_vy = 0.0,0.0
      park_ball()
      entry_time = lives > 0 ? ENTRY_DURATION : 0.0
      return 0
   }
   trail_time += dt
   if trail_time >= 1.0/120.0 { trail_time -= 1.0/120.0 record_trail() }
   0
}

fn advance(f64 dt) {
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

fn animate(f64 dt) int {
   pause_fade = lim(pause_fade+(mode == 2 ? dt : -dt)*8.0,0.0,1.0)
   if mode == 2 { return 0 }
   age_effects(dt)
   paddle_hit = lim(paddle_hit-dt,0.0,0.18)
   wall_intro_time = lim(wall_intro_time+dt,0.0,WALL_INTRO_DURATION)
   if mode >= 3 && death_time <= 0.0 { result_time = lim(result_time+dt,0.0,RESULT_FADE_DURATION) }
   if death_time > 0.0 { death_time = lim(death_time-dt,0.0,DEATH_DURATION) }
   elif mode == 0 { entry_time = lim(entry_time-dt,0.0,ENTRY_DURATION) }
   if mode == 0 && launch_pending && death_time == 0.0 && entry_time == 0.0 && wall_intro_time >= WALL_INTRO_DURATION { launch() }
   0
}

fn update(f64 dt,f64 dir) {
   acc = lim(acc+lim(dt,0.0,0.05),0.0,0.05)
   mut int count = 0
   while acc >= FIXED && count < 12 {
      acc -= FIXED
      animate(FIXED)
      if mode <= 1 {
         prev_paddle = paddle_x
         paddle_v += (dir*PADDLE_SPEED-paddle_v)*0.13
         paddle_x = lim(paddle_x+paddle_v*FIXED,0.0,VIEW_W-PADDLE_W)
         if paddle_x <= 0.0 || paddle_x >= VIEW_W-PADDLE_W { paddle_v = 0.0 }
         if mode == 0 { park_ball() }
         else { step(FIXED) }
      }
      count += 1
   }
}

fn clipped(f64 ox,f64 oy,f64 scale,f64 x,f64 y,f64 w,f64 h,int color) {
   def f64 l = lim(x,0.0,VIEW_W)
   def f64 t = lim(y,44.0,VIEW_H-26.0)
   def f64 r = lim(x+w,0.0,VIEW_W)
   def f64 b = lim(y+h,44.0,VIEW_H-26.0)
   if r > l && b > t { gfx.draw_rect_fast(ox+l*scale,oy+t*scale,(r-l)*scale,(b-t)*scale,color) }
}
fn draw_wall(f64 ox,f64 oy,f64 scale) {
   def f64 bw = brick_w()
   mut int r = 0
   while r < ROWS {
      mut int c = 0
      while c < COLS {
         def int hp = hp_at(c,r)
         if hp > 0 {
            def f64 delay = float(r)*0.035+float(c)*0.013
            def f64 t = ease((wall_intro_time-delay)/(WALL_INTRO_DURATION-delay))
            def f64 hit = float(load32(flashes,(r*COLS+c)*4))/30.0
            def f64 width = bw*t*(1.0-0.08*hit)
            def f64 height = BRICK_H*t*(1.0-0.18*hit)
            def f64 x = brick_x(c)+(bw-width)*0.5
            def f64 y = brick_y(r)+(BRICK_H-height)*0.5-24.0*(1.0-t)
            if t > 0.0 {
               clipped(ox,oy,scale,x,y,width,height,rgba(rgb(r,3),(hp < row_hp(r) ? 0.52 : 1.0)*t))
               if hit > 0.0 { clipped(ox,oy,scale,x,y,width,height,rgba(0xffffff,hit*0.55)) }
               if hp > 1 && width > 8.0 { clipped(ox,oy,scale,x+3.0,y+3.0,width-6.0,2.0,rgba(0xffffff,0.48*t)) }
            }
         }
         c += 1
      }
      r += 1
   }
}
fn draw_ball(f64 ox,f64 oy,f64 scale,f64 accumulator) {
   def f64 blend = mode <= 1 ? lim(accumulator/FIXED,0.0,1.0) : 1.0
   def f64 px = prev_paddle+(paddle_x-prev_paddle)*blend
   def f64 kick = paddle_hit/0.18
   def f64 pw = PADDLE_W*ease(wall_intro_time/0.36)*(1.0+0.08*kick)
   if pw > 0.0 { clipped(ox,oy,scale,px+(PADDLE_W-pw)*0.5,PADDLE_Y+2.0*kick,pw,PADDLE_H*(1.0-0.22*kick),0xffe6e6ed) }
   if mode == 1 || mode == 2 {
      mut int j = trail_count-1
      while j >= 0 {
         def int k = (trail_head-1-j+TRAIL_CAP)%TRAIL_CAP
         def f64 t = 1.0-float(j+1)/float(TRAIL_CAP+1)
         def f64 size = BALL*(0.18+0.65*t)
         clipped(ox,oy,scale,getq(trail_buf,k*8)-size*0.5,getq(trail_buf,k*8+4)-size*0.5,size,size,rgba(0xc2f4ff,0.32*t*t))
         j -= 1
      }
   }
   if death_time <= 0.0 && mode < 3 {
      def f64 t = mode == 0 ? ease(1.0-entry_time/ENTRY_DURATION) : 1.0
      def f64 size = BALL*(0.15+0.85*t)
      def f64 x = mode == 0 ? px+PADDLE_W*0.5 : previous_x+(ball_x-previous_x)*blend+BALL*0.5
      def f64 y = previous_y+(ball_y-previous_y)*blend+BALL*0.5+52.0*(1.0-t)
      if t > 0.0 {
         clipped(ox,oy,scale,x-size*0.5-3.0,y-size*0.5-3.0,size+6.0,size+6.0,rgba(0xc2f4ff,0.12*t))
         clipped(ox,oy,scale,x-size*0.5,y-size*0.5,size,size,rgba(0xc2f4ff,t))
      }
   }
}
fn draw_effects(f64 ox,f64 oy,f64 scale,int font_small) {
   mut int i = 0
   while i < PCAP {
      def int o = i*PS
      def int life = load32(particles,o+20)
      if life > 0 {
         def int age = load32(particles,o+16)
         def f64 t = float(age)/float(life)
         def f64 sec = float(age)/240.0
         def f64 travel = sec*(1.0-0.38*t)
         def f64 size = float(load32(particles,o+28))*(1.0-0.75*t)
         def f64 x = getq(particles,o)+getq(particles,o+8)*travel
         def f64 y = getq(particles,o+4)+getq(particles,o+12)*travel+80.0*sec*sec
         clipped(ox,oy,scale,x-size*0.5,y-size*0.5,size,size,rgba(load32(particles,o+24),(1.0-t)*(1.0-t)))
      }
      i += 1
   }
   i = 0
   while i < FX_CAP {
      def int o = i*FX_STRIDE
      def int kind = load32(effects,o+12)
      def int award = load32(effects,o+20)
      if kind != 0 && award > 0 {
         def f64 t = float(load32(effects,o+8))/183.0
         def int streak = load32(effects,o+24)
         gfx.draw_text_centered(font_small,"+"+to_str(award)+(streak > 1 ? " x"+to_str(streak) : ""),ox+getq(effects,o)*scale,oy+lim(getq(effects,o+4)-12.0-32.0*ease(t),48.0,VIEW_H-40.0)*scale,gfx.color_alpha(FG,(1.0-t)*smooth(t*12.0)))
      }
      i += 1
   }
}

fn draw_scene(f64 ox,f64 oy,f64 scale,int font,int font_small,f64 accumulator) {
   gfx.draw_rect(ox, oy, VIEW_W * scale, VIEW_H * scale, BG)
   gfx.draw_rect(ox,oy+42.0*scale,VIEW_W*scale,scale,TRACK)
   if bricks_left < COLS*ROWS { gfx.draw_rect(ox,oy+40.0*scale,VIEW_W*scale*float(COLS*ROWS-bricks_left)/float(COLS*ROWS),2.0*scale,BALL_COLOR) }
   draw_wall(ox,oy,scale)
   draw_ball(ox,oy,scale,accumulator)
   draw_effects(ox,oy,scale,font_small)
   if pause_fade > 0.0 {
      gfx.draw_rect_fast(ox,oy,VIEW_W*scale,VIEW_H*scale,rgba(0,0.72*smooth(pause_fade)))
   } elif mode >= 3 {
      def f64 fade = lim(result_time / RESULT_FADE_DURATION, 0.0, 1.0)
      gfx.draw_rect(ox, oy, VIEW_W * scale, VIEW_H * scale, gfx.color_alpha(BG, 0.52 * fade))
   }
   gfx.draw_text(font_small, "SCORE " + to_str(score), ox + 16.0 * scale, oy + 18.0 * scale, FG)
   mut int life = 0
   while life < START_LIVES {
      gfx.draw_rect(ox+(VIEW_W-42.0-float(life)*18.0)*scale,oy+17.0*scale,10.0*scale,10.0*scale,life < lives ? FG : TRACK)
      life += 1
   }
   def f64 mid_x = ox + VIEW_W * 0.5 * scale
   def f64 mid_y = oy + VIEW_H * 0.62 * scale
   gfx.draw_text_centered(font_small,chain > 1 ? "STREAK x"+to_str(chain) : "BRICKS "+to_str(bricks_left),mid_x,oy+18.0*scale,chain > 1 ? BALL_COLOR : MUTED)
   if mode == 0 && death_time <= 0.0 {
      def f64 t = smooth(1.0-entry_time/ENTRY_DURATION)
      gfx.draw_text_centered(font,"BRICKS",mid_x,mid_y-24.0*scale,gfx.color_alpha(FG,t))
      gfx.draw_text_centered(font_small,launch_pending ? "GET READY" : "SPACE / ENTER TO LAUNCH",mid_x,mid_y+20.0*scale,gfx.color_alpha(MUTED,t))
   } elif mode == 2 {
      gfx.draw_text_centered(font,"PAUSED",mid_x,mid_y-24.0*scale,FG)
      gfx.draw_text_centered(font_small,"SPACE / ENTER TO RESUME",mid_x,mid_y+20.0*scale,MUTED)
   } elif mode >= 3 {
      def f64 t = smooth(result_time/RESULT_FADE_DURATION)
      gfx.draw_text_centered(font,mode == 3 ? "WALL CLEARED" : "GAME OVER",mid_x,mid_y-24.0*scale+14.0*(1.0-t)*scale,gfx.color_alpha(FG,t))
      gfx.draw_text_centered(font_small,"SPACE / ENTER TO RETRY",mid_x,mid_y+20.0*scale,gfx.color_alpha(MUTED,t))
   }
   gfx.draw_text_centered(font_small,"A / D MOVE     SPACE PAUSE     R RESTART",mid_x,oy+628.0*scale,MUTED)
}

;; ---------------------------------------------------------------- self test

fn check(bool ok, str what) {
   if !ok {
      eprint("breakout selftest FAILED: " + what)
      release()
      exit(1)
   }
}

fn selftest() {
   new_game()
   check(bricks_left == COLS * ROWS, "wall starts full")
   check(entry_time == ENTRY_DURATION && mode == 0,
      "new serve starts with an entry animation")
   handle_space()
   check(launch_pending && mode == 0, "space queues launch during intro")
   mut int intro_steps = 0
   while intro_steps < 120 && mode == 0 {
      animate(0.01)
      intro_steps += 1
   }
   check(mode == 1 && entry_time == 0.0, "queued launch enters play after intro")
   new_game()
   entry_time = 0.0
   wall_intro_time = WALL_INTRO_DURATION
   handle_space()
   check(mode == 1 && entry_time == 0.0, "space launches immediately after intro")
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
   def int last_fx = ((fx_head + FX_CAP - 1) % FX_CAP) * FX_STRIDE
   check(load32(effects, last_fx + 20) == row_points(ROWS - 1) * 5 &&
      load32(effects, last_fx + 24) == 5,
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
   release()
   exit(0)
}

if process_args.positionals().contains("--selftest") { selftest() }

gfx.apply_backend_env()
gfx.apply_backend_argv()
mut win = gfx.init_window(960, 640, "Bricks - Nytrix", 0, true, false, 1)
if !win { release() panic("Bricks: "+gfx.window_error()) }
window.set_cursor_mode(win,window.CURSOR_NORMAL)
fixture.ready("breakout")
def headless = gfx.get_active_backend_name() == "mock"
def fx = fixture.start_ui(0, headless ? 1000 : 0, headless)
def bool replay = common.env_truthy("NYTRIX_UI_REPLAY")
def bool autopilot = replay || common.env_truthy("NYTRIX_BREAKOUT_AUTOPILOT")
mut int font = 0
mut int font_small = 0
mut int font_px = 0
mut bool prev_space = false
mut bool prev_r = false
mut bool first_frame = true
mut int frame = 0
mut int last_tick = ticks()
mut int surface_failures = 0
new_game()

while !gfx.window_should_close(win) && fixture.tick_ui(fx) {
   def int frame_started = ticks()
   window.poll_events()
   if fixture.quit_key(fixture.next_ui_key(fx)) { break }
   def fb = window.get_framebuffer_size(win)
   def f64 w = float(fb.get(0, 1280))
   def f64 h = float(fb.get(1, 840))
   if w <= 0.0 || h <= 0.0 { last_tick = ticks() msleep(16) continue }
   def f64 scale = float(math.min(w / VIEW_W, h / VIEW_H))
   def f64 ox = (w - VIEW_W * scale) * 0.5
   def f64 oy = (h - VIEW_H * scale) * 0.5
   def int want_px = int(math.max(1.0, math.floor(34.0 * scale + 0.5)))
   if want_px != font_px {
      def nf = gfx.font_load_first(["etc/assets/fonts/monocraft.ttf"], want_px)
      def ns = gfx.font_load_first(["etc/assets/fonts/maplemono.ttf","etc/assets/fonts/monocraft.ttf"], int(math.max(1.0, float(want_px) * 0.6)))
      if nf > 0 && ns > 0 {
         if font > 0 { gfx.font_destroy(font) }
         if font_small > 0 { gfx.font_destroy(font_small) }
         font, font_small, font_px = nf, ns, want_px
      } else {
         if nf > 0 { gfx.font_destroy(nf) }
         if ns > 0 { gfx.font_destroy(ns) }
         font_px = want_px
      }
   }
   if !gfx.begin_frame_clear(BG) {
      surface_failures += 1
      if surface_failures >= 120 { eprint("Bricks: surface unavailable") break }
      last_tick = ticks() msleep(16) continue
   }
   surface_failures = 0
   gfx.set_ortho_2d(0.0, w, h, 0.0)
   def int now_tick = ticks()
   def f64 dt = replay ? 1.0/120.0 : lim(float(now_tick-last_tick)/1000000000.0,0.0,0.05)
   last_tick = now_tick
   if window.key_down(win,KEY_ESCAPE) { window.set_should_close(win,true) }
   def bool space = window.key_down(win,KEY_SPACE) || window.key_down(win,KEY_ENTER)
   def bool rkey = window.key_down(win,KEY_R)
   if rkey && !prev_r { new_game() }
   elif space && !prev_space { handle_space() }
   prev_space,prev_r = space,rkey
   mut f64 dir = 0.0
   if window.key_down(win,KEY_A) || window.key_down(win,KEY_LEFT) { dir -= 1.0 }
   if window.key_down(win,KEY_D) || window.key_down(win,KEY_RIGHT) { dir += 1.0 }
   if autopilot {
      def f64 target = ball_x+BALL*0.5-PADDLE_W*0.5+(frame%240 < 120 ? 18.0 : -18.0)
      def f64 d = target-paddle_x
      dir = d > 4.0 ? 1.0 : (d < -4.0 ? -1.0 : 0.0)
      if mode == 0 { launch_pending = true }
   }
   update(dt,dir)
   draw_scene(ox,oy,scale,font,font_small,acc)

   if replay {
      print("NYTRIX_UI_APP_STATE demo=breakout frame=" + to_str(frame + 1) + " mode=" + to_str(mode) +
         " score=" + to_str(score) + " lives=" + to_str(lives) + " bricks=" + to_str(bricks_left) +
         " ball=" + to_str(ball_x) + "," + to_str(ball_y) + " paddle=" + to_str(paddle_x))
   }
   if !gfx.end_frame() { eprint("Bricks: presentation failed") surface_failures = 120 break }
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

release()
if font > 0 { gfx.font_destroy(font) }
if font_small > 0 { gfx.font_destroy(font_small) }
gfx.close_window()
exit(surface_failures >= 120 ? 1 : fixture.exit_status(fx))
