#!/usr/bin/env ny

;; Keywords: ui tictactoe game example
;; Tic-tac-toe with optimal AI, local multiplayer, animated marks and round scoring.

use core
use core.os as os
use core.os.args as args
use core.common as common
use core.math as math
use core.os.ui.render as gfx
use core.os.ui.window as win
use core.os.ui.window.consts as k

def f64 W,H = 600.0,720.0
def f64 CELL,BX,BY = 144.0,84.0,176.0
fn mask(int i) int {
   if i == 0 { return 7 }
   if i == 1 { return 56 }
   if i == 2 { return 448 }
   if i == 3 { return 73 }
   if i == 4 { return 146 }
   if i == 5 { return 292 }
   if i == 6 { return 273 }
   84
}
fn first(int i) int { i < 3 ? i*3 : i < 6 ? i-3 : i == 6 ? 0 : 2 }
fn final_cell(int i) int { i < 3 ? i*3+2 : i < 6 ? i+3 : i == 6 ? 8 : 6 }
def BLACK = gfx.color_hex("#000000")
def WHITE = gfx.color_hex("#F3F4F5")
def MUTED = gfx.color_hex("#929298")
def XC = gfx.color_hex("#76E5C0")
def OC = gfx.color_hex("#B5A4FF")
mut bool ai = true
mut f64 think = 0.0
mut int xb = 0
mut int ob = 0
mut int turn = 1
mut int outcome = 0
mut int winning = -1
mut int count = 0
mut int xs = 0
mut int oscore = 0
mut int draws = 0
mut list moves = []
mut list ages = [0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0]
mut f64 intro = 0.0
mut f64 result = 0.0
mut int hover = -1
mut int pressed = -1
mut f64 mx = -1.0
mut f64 my = -1.0
mut f64 scale = 1.0
mut f64 ox = 0.0
mut f64 oy = 0.0

fn lim(f64 x,f64 a,f64 b) f64 { float(math.min(math.max(x,a),b)) }
fn ease(f64 t) f64 { def f64 v = 1.0-lim(t,0.0,1.0) 1.0-v*v*v }
fn cx(int i) f64 { BX+(float(i%3)+0.5)*CELL }
fn cy(int i) f64 { BY+(float(int(i/3))+0.5)*CELL }
fn mark(int i) int { if (xb & (1<<i)) != 0 { return 1 } if (ob & (1<<i)) != 0 { return 2 } 0 }
fn winner(int bits) int {
   mut int i = 0
   while i < 8 { def int m = mask(i) if (bits&m) == m { return i } i += 1 }
   -1
}
fn round_reset() {
   xb = 0 ob = 0 turn = 1 outcome = 0 winning = -1 count = 0
   moves = [] ages = [0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0]
   intro = 0.0 result = 0.0 pressed = -1 think = 0.0
}
fn match_reset() { xs = 0 oscore = 0 draws = 0 round_reset() }
fn play(int i) bool {
   if i < 0 || i >= 9 || outcome != 0 { return false }
   if mark(i) != 0 { return false }
   if turn == 1 { xb = xb | (1<<i) } else { ob = ob | (1<<i) }
   moves = moves.append(i) ages = ages.set(i,0.0) count += 1
   winning = winner(turn == 1 ? xb : ob)
   if winning >= 0 {
      outcome = turn
      if turn == 1 { xs += 1 } else { oscore += 1 }
   } elif count == 9 { outcome = 3 draws += 1 }
   else { turn = 3-turn think = 0.0 }
   result = 0.0
   true
}
fn undo() bool {
   if count == 0 { return false }
   if outcome == 1 { xs -= 1 } elif outcome == 2 { oscore -= 1 } elif outcome == 3 { draws -= 1 }
   def int i = int(moves[count-1])
   turn = mark(i)
   if turn == 1 { xb = xb & ~(1<<i) } else { ob = ob & ~(1<<i) }
   count -= 1 moves = moves.slice(0,count) ages = ages.set(i,0.0)
   outcome = 0 winning = -1 result = 0.0
   true
}
fn priority(int i) int {
   if i == 0 { return 4 }
   if i == 1 { return 0 }
   if i == 2 { return 2 }
   if i == 3 { return 6 }
   if i == 4 { return 8 }
   if i == 5 { return 1 }
   if i == 6 { return 3 }
   if i == 7 { return 5 }
   7
}
fn negamax(int a,int b,int depth,int alpha,int beta) int {
   if winner(b) >= 0 { return depth-10 }
   def int occupied = a|b
   if occupied == 511 { return 0 }
   mut int best = -11
   mut int lo = alpha
   mut int i = 0
   while i < 9 {
      def int bit = 1<<priority(i)
      if (occupied&bit) == 0 {
         def int v = 0-negamax(b,a|bit,depth+1,0-beta,0-lo)
         if v > best { best = v }
         if v > lo { lo = v }
         if lo >= beta { return best }
      }
      i += 1
   }
   best
}
fn choose(int a,int b) int {
   if winner(a) >= 0 || winner(b) >= 0 || (a|b) == 511 { return -1 }
   mut int best = -11
   mut int cell = -1
   mut int i = 0
   while i < 9 {
      def int n = priority(i)
      def int bit = 1<<n
      if ((a|b)&bit) == 0 {
         def int v = 0-negamax(b,a|bit,1,-11,11)
         if v > best { best = v cell = n }
      }
      i += 1
   }
   cell
}
fn human_turn() bool { outcome == 0 && (!ai || turn == 1) }
fn human_play(int i) { if human_turn() { play(i) } }
fn undo_turn() {
   if undo() && ai && turn == 2 { undo() }
   think = 0.0
}
fn toggle_ai() { ai = !ai match_reset() }
fn animate(f64 dt) {
   if ai && turn == 2 && outcome == 0 {
      think += dt
      if think >= 0.36 { play(choose(ob,xb)) think = 0.0 }
   }
   intro = lim(intro+dt,0.0,0.55)
   if outcome != 0 { result = lim(result+dt,0.0,1.0) }
   mut int i = 0
   while i < 9 { if mark(i) != 0 && float(ages[i]) < 0.32 { ages = ages.set(i,lim(float(ages[i])+dt,0.0,0.32)) } i += 1 }
}
fn fit_view(f64 w,f64 h) { scale = float(math.min(w/W,h/H)) ox = (w-W*scale)*0.5 oy = (h-H*scale)*0.5 }
fn inside(f64 x,f64 y,f64 l,f64 t,f64 w,f64 h) bool { x >= l && x < l+w && y >= t && y < t+h }
fn target(f64 x,f64 y) int {
   if inside(x,y,BX,BY,CELL*3.0,CELL*3.0) { return int((y-BY)/CELL)*3+int((x-BX)/CELL) }
   if inside(x,y,84.0,632.0,132.0,48.0) { return 9 }
   if inside(x,y,228.0,632.0,132.0,48.0) { return 10 }
   if inside(x,y,372.0,632.0,144.0,48.0) { return 11 }
   if inside(x,y,426.0,25.0,90.0,36.0) { return 12 }
   -1
}
fn action(int i) { if i == 9 { undo_turn() } elif i == 10 { round_reset() } elif i == 11 { toggle_ai() } elif i == 12 { match_reset() } elif i >= 0 && i < 9 { human_play(i) } }
fn input(any window,f64 w,f64 h,any cursor) {
   def list sz = win.size(window)
   def f64 sx = w/float(max(1.0,float(sz.get(0,w))))
   def f64 sy = h/float(max(1.0,float(sz.get(1,h))))
   mut int budget = 64
   while budget > 0 {
      budget -= 1
      def any e = win.check_event(window)
      if is_nil(e) || (is_int(e) && int(e) == 0) { break }
      def int typ = win.event_type(e)
      def any d = win.event_data(e)
      if typ == k.EVENT_KEY_PRESSED && int(d.get("action",1)) != 2 {
         def int key = int(d.get("key",0))
         if key == k.KEY_ESCAPE { win.set_should_close(window,true) }
         elif key == k.KEY_R { round_reset() }
         elif key == k.KEY_N { match_reset() }
         elif key == k.KEY_U || key == k.KEY_BACKSPACE { undo_turn() }
         elif key == k.KEY_M { toggle_ai() }
         elif key == k.KEY_ENTER || key == k.KEY_SPACE { if outcome != 0 { round_reset() } }
         elif key >= k.KEY_1 && key <= k.KEY_9 { human_play(key-k.KEY_1) }
      }
      if typ == k.EVENT_MOUSE_POS_CHANGED || typ == k.EVENT_MOUSE_BUTTON_PRESSED || typ == k.EVENT_MOUSE_BUTTON_RELEASED {
         mx = float(d.get("x",mx)) my = float(d.get("y",my))
         def f64 x = (mx*sx-ox)/scale
         def f64 y = (my*sy-oy)/scale
         hover = target(x,y)
         if int(d.get("button",-1)) == 0 {
            if typ == k.EVENT_MOUSE_BUTTON_PRESSED { pressed = hover }
            elif typ == k.EVENT_MOUSE_BUTTON_RELEASED { if pressed == hover { action(hover) } pressed = -1 }
         }
      }
      if typ == k.EVENT_MOUSE_ENTER && cursor { win.set_cursor(window,cursor) }
      if typ == k.EVENT_MOUSE_LEAVE || typ == k.EVENT_FOCUS_OUT || typ == k.EVENT_WINDOW_RESIZED || typ == k.EVENT_SCALE_UPDATED { hover = -1 pressed = -1 }
   }
}
mut int tex_x = 0
mut int tex_o = 0
fn capsule(f64 x,f64 y,f64 d) f64 {
   def f64 t = lim((x+y*d)/60.0,-1.0,1.0)
   def f64 u = x-30.0*t
   def f64 v = y-30.0*t*d
   float(math.sqrt(u*u+v*v))-6.0
}
fn mark_texture(int m) int {
   def int n = 384
   def ptr pixels = malloc(n*n*4)
   if !pixels { return -1 }
   mut int y = 0
   while y < n {
      mut int x = 0
      def f64 v = (float(y)+0.5)*100.0/float(n)-50.0
      while x < n {
         def f64 u = (float(x)+0.5)*100.0/float(n)-50.0
         def f64 d = m == 1 ? float(math.min(capsule(u,v,1.0),capsule(u,v,-1.0))) : float(math.abs(float(math.sqrt(u*u+v*v))-40.0))-6.0
         def int a = int(lim(0.5-d/1.0,0.0,1.0)*255.0+0.5)
         store32(pixels,(m == 1 ? 0xc0e576 : 0xffa4b5)|(a<<24),(y*n+x)*4)
         x += 1
      }
      y += 1
   }
   def int tex = gfx.texture_create_rgba(n,n,pixels,37,1,33071,33071,true)
   free(pixels)
   tex
}
fn card(f64 x,f64 y,f64 w,f64 h,f64 radius,vec4 color) {
   gfx.draw_rect_rounded_sdf(ox+x*scale,oy+y*scale,w*scale,h*scale,radius*scale,color)
}
fn stroke(f64 x,f64 y,f64 u,f64 v,f64 width,f64 t,f64 alpha) {
   if t <= 0.001 { return }
   def f64 ex = x+(u-x)*t
   def f64 ey = y+(v-y)*t
   gfx.draw_line_fast(ox+x*scale,oy+y*scale,ox+ex*scale,oy+ey*scale,width*scale,0xffffff|(int(lim(alpha,0.0,1.0)*255.0+0.5)<<24))
}
fn text_at(int font,str s,f64 x,f64 y,any color) { gfx.draw_text_centered(font,s,ox+x*scale,oy+y*scale,color) }
fn symbol(int i,int m,f64 progress,f64 alpha) {
   def f64 t = ease(progress)
   if t <= 0.001 { return }
   def f64 size = 100.0*(0.68+0.32*t)*scale
   gfx.draw_rect_tex(ox+cx(i)*scale-size*0.5,oy+cy(i)*scale-size*0.5,size,size,m == 1 ? tex_x : tex_o,1.0,1.0,1.0,alpha*t)
}
fn button(int id,f64 x,f64 w,str label,int font,bool enabled,bool primary=false) {
   def color = !enabled ? gfx.color_hex("#141619") : pressed == id ? gfx.color_hex("#373E43") : hover == id ? gfx.color_hex("#2A3337") : primary ? gfx.color_hex("#76E5C0") : gfx.color_hex("#1C2227")
   card(x,632.0,w,48.0,5.0,color)
   text_at(font,label,x+w*0.5,656.0,!enabled ? MUTED : primary && hover != id && pressed != id ? BLACK : WHITE)
}
fn draw(int title,int font,int small) {
   gfx.draw_text(title,"Tic tac toe",ox+84.0*scale,oy+27.0*scale,WHITE)
   card(426.0,25.0,90.0,36.0,4.0,hover == 12 ? gfx.color_hex("#30373C") : gfx.color_hex("#171C21"))
   text_at(small,"Reset",471.0,43.0,MUTED)
   card(84.0,76.0,136.0,64.0,5.0,gfx.color_hex("#10241F"))
   card(232.0,76.0,136.0,64.0,5.0,gfx.color_hex("#16191E"))
   card(380.0,76.0,136.0,64.0,5.0,gfx.color_hex("#201B30"))
   text_at(small,ai ? "You" : "Player X",152.0,92.0,XC)
   text_at(small,"Draws",300.0,92.0,MUTED)
   text_at(small,ai ? "Computer" : "Player O",448.0,92.0,OC)
   text_at(font,to_str(xs),152.0,117.0,WHITE)
   text_at(font,to_str(draws),300.0,117.0,WHITE)
   text_at(font,to_str(oscore),448.0,117.0,WHITE)
   def str status = outcome == 0 ? (turn == 1 ? (ai ? "Your turn" : "Player X's turn") : (ai ? "Computer is thinking..." : "Player O's turn")) : outcome == 1 ? (ai ? "You win!" : "Player X wins!") : outcome == 2 ? (ai ? "Computer wins" : "Player O wins!") : "It's a draw"
   text_at(small,status,300.0,158.0,outcome == 3 ? WHITE : (outcome == 0 ? turn : outcome) == 1 ? XC : OC)
   mut int i = 0
   while i < 9 {
      def f64 x = cx(i) def f64 y = cy(i)
      def int m = mark(i)
      def f64 reveal = ease((intro-float(i)*0.022)/0.35)
      def f64 s = (CELL-10.0)*reveal
      def bool active = hover == i && human_turn() && m == 0
      mut bool won = false
      if winning >= 0 { won = (mask(winning)&(1<<i)) != 0 }
      def color = won ? (outcome == 1 ? gfx.color_hex("#17352B") : gfx.color_hex("#2D2544")) : active ? (pressed == i ? gfx.color_hex("#2A3837") : gfx.color_hex("#20282B")) : gfx.color_hex("#111519")
      if s > 0.0 { card(x-s*0.5,y-s*0.5,s,s,6.0*reveal,color) }
      if m != 0 { symbol(i,m,float(ages[i])/0.32,outcome != 0 && winning >= 0 && !won ? 0.36 : 1.0) }
      elif active { symbol(i,turn,1.0,pressed == i ? 0.5 : 0.22) }
      i += 1
   }
   if winning >= 0 {
      def int a = first(winning) def int b = final_cell(winning)
      def f64 p = ease((result-0.12)/0.45)
      stroke(cx(a),cy(a),cx(b),cy(b),5.0,p,0.85*p)
   }
   button(9,84.0,132.0,"Undo",small,count > 0)
   button(10,228.0,132.0,"New round",small,true,true)
   button(11,372.0,144.0,ai ? "vs Computer" : "2 players",small,true)
   text_at(small,ai ? "Single player  /  Perfect AI" : "Local multiplayer",300.0,701.0,MUTED)
}
fn verify_ai(int x,int o) int {
   assert(winner(x) < 0,"human cannot win against AI")
   if winner(o) >= 0 || (x|o) == 511 { return 1 }
   mut int total = 0
   mut int i = 0
   while i < 9 {
      def int bit = 1<<i
      if ((x|o)&bit) == 0 {
         def int nx = x|bit
         assert(winner(nx) < 0,"AI allowed winning move")
         if (nx|o) == 511 { total += 1 }
         else {
            def int j = choose(o,nx)
            assert(j >= 0 && j < 9 && ((nx|o)&(1<<j)) == 0,"AI legal move")
            total += verify_ai(nx,o|(1<<j))
         }
      }
      i += 1
   }
   total
}
fn selftest() {
   ai = false
   match_reset()
   mut int i = 0
   while i < 8 { assert(winner(mask(i)) == i,"test 1") i += 1 }
   assert(!play(-1) && !play(9),"test 2")
   assert(play(0) && !play(0) && turn == 2,"test 3")
   assert(play(3) && play(1) && play(4) && play(2),"test 4")
   assert(outcome == 1 && winning == 0 && xs == 1 && !play(8),"test 5")
   assert(undo() && outcome == 0 && xs == 0 && turn == 1 && mark(2) == 0,"test 6")
   assert(play(2) && xs == 1,"test 7")
   round_reset() assert(xs == 1 && count == 0,"test 8")
   assert(play(0) && play(1) && play(2) && play(4) && play(3) && play(7),"test 9")
   assert(outcome == 2 && oscore == 1,"test 10")
   round_reset()
   assert(play(0) && play(1) && play(2) && play(4) && play(3) && play(5) && play(7) && play(6) && play(8),"test 11")
   assert(outcome == 3 && draws == 1 && undo() && draws == 0 && turn == 1,"test 12")
   assert(target(BX+CELL*2.5,BY+CELL*2.5) == 8 && target(BX-1.0,BY) == -1,"test 13")
   fit_view(1000.0,500.0) assert(scale > 0.0 && ox > 0.0,"test 14")
   fit_view(300.0,900.0) assert(oy > 0.0 && ox == 0.0,"test 15")
   assert(choose(3,24) == 2,"AI wins immediately")
   assert(choose(16,3) == 2,"AI blocks immediate win")
   assert(choose(0,0) == 4,"AI chooses center")
   ai = true match_reset()
   human_play(0) human_play(1)
   assert(count == 1 && turn == 2,"AI owns O turn")
   animate(0.4)
   assert(count == 2 && turn == 1,"AI responds")
   undo_turn()
   assert(count == 0 && turn == 1,"AI undo pair")
   print("TICTACTOE_OK AI terminal paths",verify_ai(0,0))
}
if args.positionals().contains("--selftest") { selftest() os.exit(0) }
gfx.apply_backend_env() gfx.apply_backend_argv()
if gfx.get_backend_type() != int(gfx.BACKEND_VK) && gfx.get_backend_type() != int(gfx.BACKEND_GL) { gfx.set_backend_type(gfx.BACKEND_GL) }
def any window = gfx.init_window(600,720,"Tic Tac Toe - Nytrix",0,false,true,4)
if !window { eprint("Tic tac toe requires a graphical display: "+gfx.window_error()) os.exit(1) }
if gfx.get_active_backend_name() == "mock" || gfx.get_active_backend_name() == "tty" { gfx.close_window() eprint("Tic tac toe: no graphical renderer available") os.exit(1) }
tex_x = mark_texture(1)
tex_o = mark_texture(2)
if tex_x <= 0 || tex_o <= 0 { gfx.close_window() eprint("Tic tac toe: mark texture allocation failed") os.exit(1) }
win.set_cursor_mode(window,win.CURSOR_NORMAL)
def any cursor = win.create_standard_cursor(win.ARROW_CURSOR)
if cursor && win.backend() != "wayland" { win.set_cursor(window,cursor) }
def list fonts = ["etc/assets/fonts/maplemono.ttf","etc/assets/fonts/jetbrains.ttf","etc/assets/fonts/monocraft.ttf"]
mut int title = 0 mut int font = 0 mut int small = 0 mut int font_px = 0
mut int last = os.monotonic_ns() mut int frames = 0 mut int failures = 0
def int limit = common.env_int_clamped("NY_TTT_FRAMES",0,0,10000)
def bool trace = common.env_truthy("NY_TTT_TRACE")
ai = true match_reset()
win.show(window)
eprint("tictactoe: "+gfx.get_active_backend_name())
while !gfx.window_should_close(window) {
   def int start = os.monotonic_ns()
   def list fb = gfx.framebuffer_size_f64(600.0,720.0)
   def f64 w = float(fb.get(0,600.0)) def f64 h = float(fb.get(1,720.0))
   if w <= 0.0 || h <= 0.0 || win.is_window_minimized(window) { input(window,600.0,720.0,cursor) last = os.monotonic_ns() os.msleep(32) continue }
   fit_view(w,h) input(window,w,h,cursor)
   if win.should_close(window) { break }
   def int px = int(max(1.0,math.floor(24.0*scale+0.5)))
   if px != font_px {
      def int nf = gfx.font_load_first(fonts,px)
      def int nt = gfx.font_load_first(fonts,int(max(1.0,float(px)*1.4)))
      def int ns = gfx.font_load_first(fonts,int(max(1.0,float(px)*0.70)))
      if nf > 0 && nt > 0 && ns > 0 {
         if font > 0 { gfx.font_destroy(font) gfx.font_destroy(title) gfx.font_destroy(small) }
         font = nf title = nt small = ns font_px = px
      } else {
         if nf > 0 { gfx.font_destroy(nf) } if nt > 0 { gfx.font_destroy(nt) } if ns > 0 { gfx.font_destroy(ns) }
         if font == 0 { eprint("tictactoe: font unavailable") failures = 120 break }
      }
   }
   def int now = os.monotonic_ns()
   animate(lim(float(now-last)/1000000000.0,0.0,0.05)) last = now
   if !gfx.begin_frame_clear(BLACK) { failures += 1 if failures >= 120 { break } os.msleep(16) continue }
   gfx.set_ortho_2d(0.0,w,h,0.0)
   draw(title,font,small)
   if !gfx.end_frame() { failures = 120 break }
   failures = 0 frames += 1
   if trace { print(f"TTT frame={frames} x={xb} o={ob} turn={turn} result={outcome} hover={hover}") }
   if limit > 0 && frames >= limit { break }
   def int wait = int((16666667-(os.monotonic_ns()-start))/1000000)
   if wait > 0 { os.msleep(wait) }
}
if font > 0 { gfx.font_destroy(font) gfx.font_destroy(title) gfx.font_destroy(small) }
if cursor { win.destroy_cursor(cursor) }
gfx.texture_destroy(tex_x)
gfx.texture_destroy(tex_o)
gfx.close_window()
os.exit(failures >= 120 ? 1 : 0)
