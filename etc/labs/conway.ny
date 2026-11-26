#!/usr/bin/env ny

;; Keywords: ui gui automata conway life simulation cellular example
;; Interactive Conway's Game of Life with presets, painting, pan, zoom and native rendering.

use core
use core.common as common
use core.math (min,max,clamp,floor,ceil,exp,abs)
use core.math.random as rng
use core.os as os
use core.os.time as time
use core.os.ui.render as gfx
use core.os.ui.window as win
use core.os.ui.window.consts as k
use core.os.ui.assets.viewer as assets

def N = 256
def S = N+2
def TOTAL = S*S
def RECT_CAP = N*N+2*N+8
def PALETTES = ["Jade","Glacier","Ember","Orchid","Mono"]
def DEFAULT_PALETTE = 4
def LIVE = [[0.38,0.94,0.71,1.0],[0.42,0.79,1.0,1.0],[1.0,0.64,0.30,1.0],[0.83,0.61,1.0,1.0],[0.96,0.96,0.96,1.0]]
def BOARD = [[0.035,0.091,0.080,1.0],[0.036,0.074,0.12,1.0],[0.115,0.064,0.043,1.0],[0.085,0.052,0.13,1.0],[0.0,0.0,0.0,1.0]]
def GRID_COLOR = [[0.082,0.16,0.14,1.0],[0.083,0.15,0.22,1.0],[0.20,0.12,0.075,1.0],[0.16,0.105,0.23,1.0],[0.075,0.075,0.075,1.0]]
mut ptr rects = 0
mut int rect_count = 0
mut bool board_dirty = true
mut f64 cached_x = 0.0 mut f64 cached_y = 0.0 mut f64 cached_zoom = 0.0
mut f64 cached_w = 0.0 mut f64 cached_h = 0.0
mut int cached_theme = -1 mut bool cached_grid = false
mut int board_builds = 0
def names = ["Random","Glider","Blinker","Block","R-pentomino","Acorn","Galaxy","Gosper gun","LWSS"]
def shapes = [[],[".O.","..O","OOO"],["OOO"],["OO","OO"],[".OO","OO.",".O."],[".O.....","...O...","OO..OOO"],["OOOOOO.OO","OOOOOO.OO",".......OO","OO.....OO","OO.....OO","OO.....OO","OO.......","OO.OOOOOO","OO.OOOOOO"],["........................O...........","......................O.O...........","............OO......OO............OO","...........O...O....OO............OO","OO........O.....O...OO..............","OO........O...O.OO....O.O...........","..........O.....O.......O...........","...........O...O....................","............OO......................"],[".O..O","O....","O...O","OOOO."]]
mut ptr cells = 0
mut ptr next = 0
mut int generation = 0
mut int population = 0
mut int births = 0
mut int deaths = 0
mut int selected = 0
mut int density = 15
mut int rate = 20
mut int brush = 1
mut bool running = false
mut bool wrap = false
mut bool grid = true
mut bool panel = true
mut int gesture = 0
mut int held = -1
mut bool pointer_inside = false
mut bool stroke_inside = false
mut bool ui_draw = true
mut f64 panel_scroll = 0.0
mut f64 panel_max = 0.0
mut f64 press_x = 0.0
mut f64 press_y = 0.0
mut int ink = 1
mut f64 cx = 128.0
mut f64 cy = 128.0
mut f64 zoom = 6.0
mut f64 mx = 0.0
mut f64 my = 0.0
mut f64 vw = 720.0
mut f64 vh = 600.0
mut f64 acc = 0.0
mut f64 step_ms = 0.0
mut int theme = DEFAULT_PALETTE
mut bool fit_pending = true
mut bool clicked = false
mut bool input_trace = false
mut bool edited_frame = false
mut f64 click_x = 0.0
mut f64 click_y = 0.0
mut int font = 0
mut any arrow_cursor = 0
mut bool cursor_ready = false
fn release() any {
   if bool(cells) { free_raw(cells) cells = 0 }
   if bool(next) { free_raw(next) next = 0 }
   if bool(rects) { free_raw(rects) rects = 0 }
}
fn at(int x,int y) int { (y+1)*S+x+1 }
fn put(int x,int y,int v) any {
   if x < 0 || y < 0 || x >= N || y >= N { return 0 }
   def int i = at(x,y)
   def int old = load8(cells,i)
   if old != v { store8(cells,v,i) population += v-old board_dirty = true }
}
fn clear() any {
   memset(cells,0,TOTAL) memset(next,0,TOTAL)
   generation = 0 population = 0 births = 0 deaths = 0 acc = 0.0 board_dirty = true
}
fn halo(ptr p,bool torus) any {
   mut int i = 1
   while i <= N {
      store8(p,torus ? load8(p,N*S+i) : 0,i)
      store8(p,torus ? load8(p,S+i) : 0,(N+1)*S+i)
      store8(p,torus ? load8(p,i*S+N) : 0,i*S)
      store8(p,torus ? load8(p,i*S+1) : 0,i*S+N+1)
      i += 1
   }
   store8(p,torus ? load8(p,N*S+N) : 0,0)
   store8(p,torus ? load8(p,N*S+1) : 0,N+1)
   store8(p,torus ? load8(p,S+N) : 0,(N+1)*S)
   store8(p,torus ? load8(p,S+1) : 0,TOTAL-1)
}
fn evolve() any {
   def int start = os.monotonic_ns()
   halo(cells,wrap)
   mut int b = 0 mut int d = 0 mut int y = 1
   while y <= N {
      mut int i = y*S+1
      def int end = i+N
      mut int left = load8(cells,i-S-1)+load8(cells,i-1)+load8(cells,i+S-1)
      mut int mid = load8(cells,i-S)+load8(cells,i)+load8(cells,i+S)
      while i < end {
         def int right = load8(cells,i-S+1)+load8(cells,i+1)+load8(cells,i+S+1)
         def int old = load8(cells,i)
         def int n = left+mid+right-old
         def int live = n == 3 || (old == 1 && n == 2) ? 1 : 0
         store8(next,live,i) b += live & (1-old) d += old & (1-live)
         left = mid mid = right i += 1
      }
      y += 1
   }
   def ptr tmp = cells cells = next next = tmp
   population += b-d births = b deaths = d generation += 1
   if b+d > 0 { board_dirty = true }
   step_ms = float(os.monotonic_ns()-start)/1000000.0
}
fn preset(int p) any {
   clear() selected = p
   if p == 0 {
      mut int y = 0
      while y < N { mut int x = 0 while x < N { if rng.randint(0,99) < density { put(x,y,1) } x += 1 } y += 1 }
   } else {
      def list shape = shapes.get(p,[])
      def str first = to_str(shape.get(0,""))
      def int x0 = N/2-first.len/2 def int y0 = N/2-shape.len/2
      mut int y = 0
      while y < shape.len {
         def str row = to_str(shape.get(y,"")) mut int x = 0
         while x < row.len { if to_str(slice(row,x,x+1)) == "O" { put(x+x0,y+y0,1) } x += 1 }
         y += 1
      }
   }
   fit_pending = true
}
fn fit() any {
   cx = 128.0 cy = 128.0
   zoom = selected == 0 ? float(min(vw,vh))/float(N) : float(clamp(float(min(vw,vh))/(selected == 7 ? 64.0 : 32.0),1.0,28.0))
   fit_pending = false
}
fn camera() any {
   cx = float(clamp(cx,-vw*0.35/zoom,float(N)+vw*0.35/zoom))
   cy = float(clamp(cy,-vh*0.35/zoom,float(N)+vh*0.35/zoom))
}
fn zoom_at(f64 f,f64 x,f64 y) any {
   def f64 old = zoom zoom = float(clamp(zoom*f,0.25,96.0))
   cx += (x-vw*0.5)*(1.0/old-1.0/zoom) cy += (y-vh*0.5)*(1.0/old-1.0/zoom)
}
fn wheel_zoom(f64 dy,f64 x,f64 y) any { zoom_at(exp(float(clamp(dy,-6.0,6.0))*0.18),x,y) }
fn cell_x(f64 x) int { floor(cx+(x-vw*0.5)/zoom) }
fn cell_y(f64 y) int { floor(cy+(y-vh*0.5)/zoom) }
fn inside_world(f64 x,f64 y) bool { x >= 0.0 && y >= 0.0 && x < vw && y < vh }
fn valid_cell(int x,int y) bool { x >= 0 && y >= 0 && x < N && y < N }
fn stamp(int x,int y) any {
   if !valid_cell(x,y) { return 0 }
   def int r = brush-1 mut int j = -r
   while j <= r { mut int i = -r while i <= r { if i*i+j*j <= r*r { put(x+i,y+j,ink) } i += 1 } j += 1 }
}
fn stroke(f64 x,f64 y,f64 nx,f64 ny) any {
   mut int ax = cell_x(x) mut int ay = cell_y(y)
   def int bx = cell_x(nx) def int by = cell_y(ny)
   if !valid_cell(ax,ay) || !valid_cell(bx,by) { stamp(bx,by) return 0 }
   def int dx = bx >= ax ? bx-ax : ax-bx def int dy = by >= ay ? by-ay : ay-by
   def int sx = bx >= ax ? 1 : -1 def int sy = by >= ay ? 1 : -1
   mut int err = dx-dy
   mut int remaining = int(max(dx,dy))+1
   while remaining > 0 {
      remaining -= 1
      stamp(ax,ay)
      if ax == bx && ay == by { break }
      def int e = err*2
      if e > -dy { err -= dy ax += sx }
      if e < dx { err += dx ay += sy }
   }
}
fn sim_active() bool { running && gesture != 1 }
fn tick_sim(f64 dt) any {
   if !sim_active() || edited_frame { acc = 0.0 return 0 }
   acc += float(clamp(dt,0.0,0.1))
   def f64 interval = 1.0/float(rate)
   def int deadline = os.monotonic_ns()+8000000
   mut int steps = 0
   while acc >= interval && steps < 4 && os.monotonic_ns() < deadline { evolve() acc -= interval steps += 1 }
   if acc >= interval { acc = 0.0 }
}
fn cancel_gesture() any { if gesture == 1 { acc = 0.0 edited_frame = true } gesture = 0 held = -1 stroke_inside = false clicked = false }
fn update_view(f64 w,f64 h) any {
   def f64 width = panel && w >= 580.0 ? w-278.0 : w
   if width != vw || h != vh { stroke_inside = false vw = width vh = h }
   panel_max = float(max(0.0,649.0-h))
   panel_scroll = float(clamp(panel_scroll,0.0,panel_max))
   if fit_pending { fit() }
}
fn pointer_event(int typ,f64 x,f64 y,int btn,f64 dy,int mods,f64 w,f64 h) any {
   def bool inside = inside_world(x,y)
   pointer_inside = x >= 0.0 && y >= 0.0 && x < w && y < h
   if typ == k.EVENT_MOUSE_SCROLL && gesture == 0 {
      if inside { wheel_zoom(dy,x,y) }
      elif pointer_inside && vw < w { panel_scroll = float(clamp(panel_scroll-dy*42.0,0.0,panel_max)) }
   } elif typ == k.EVENT_MOUSE_BUTTON_PRESSED && gesture == 0 {
      if inside {
         if btn == 2 || (btn == 0 && (mods & k.MOD_SHIFT) != 0) { gesture = 2 held = btn }
         elif btn == 0 || btn == 1 {
            gesture = 1 held = btn acc = 0.0 edited_frame = true ink = btn == 0 ? 1 : 0
            stroke_inside = valid_cell(cell_x(x),cell_y(y))
            if stroke_inside { stamp(cell_x(x),cell_y(y)) }
         }
      } elif pointer_inside && btn == 0 && vw < w { gesture = 3 held = btn press_x = x press_y = y }
   } elif typ == k.EVENT_MOUSE_BUTTON_RELEASED && btn == held {
      if gesture == 1 && inside {
         if stroke_inside { stroke(mx,my,x,y) } else { stamp(cell_x(x),cell_y(y)) }
      } elif gesture == 3 && !inside && pointer_inside {
         clicked = true click_x = x click_y = y ui_draw = false
         controls(w,h)
         ui_draw = true clicked = false
      }
      cancel_gesture()
   } elif typ == k.EVENT_MOUSE_POS_CHANGED {
      if gesture == 2 { cx -= (x-mx)/zoom cy -= (y-my)/zoom camera() }
      elif gesture == 1 {
         def bool good = inside && valid_cell(cell_x(x),cell_y(y))
         if !inside { cancel_gesture() }
         elif good { if stroke_inside { stroke(mx,my,x,y) } else { stamp(cell_x(x),cell_y(y)) } }
         stroke_inside = good && gesture == 1
      }
   }
   mx = x my = y
}
fn input(any window,f64 w,f64 h) any {
   def list sz = win.size(window)
   def f64 sx = w/float(max(1.0,float(sz.get(0,w)))) def f64 sy = h/float(max(1.0,float(sz.get(1,h))))
   def int deadline = os.monotonic_ns()+4000000
   mut int budget = 64
   while budget > 0 {
      budget -= 1
      if os.monotonic_ns() >= deadline { break }
      def any e = win.check_event(window)
      if is_nil(e) || (is_int(e) && int(e) == 0) { break }
      def int typ = win.event_type(e) def any d = win.event_data(e)
      if typ == k.EVENT_KEY_PRESSED {
         def int key = int(d.get("key",0)) def bool fresh = int(d.get("action",1)) != 2
         if gesture == 1 && (key == k.KEY_EQUAL || key == k.KEY_KP_ADD || key == k.KEY_X || key == k.KEY_MINUS || key == k.KEY_KP_SUBTRACT || key == k.KEY_Z || key == k.KEY_LEFT || key == k.KEY_RIGHT || key == k.KEY_UP || key == k.KEY_DOWN) { continue }
         if key == k.KEY_ESCAPE { win.set_should_close(window,true) }
         elif key == k.KEY_EQUAL || key == k.KEY_KP_ADD || key == k.KEY_X { zoom_at(1.2,vw*0.5,vh*0.5) }
         elif key == k.KEY_MINUS || key == k.KEY_KP_SUBTRACT || key == k.KEY_Z { zoom_at(1.0/1.2,vw*0.5,vh*0.5) }
         elif key == k.KEY_LEFT { cx -= 24.0/zoom camera() }
         elif key == k.KEY_RIGHT { cx += 24.0/zoom camera() }
         elif key == k.KEY_UP { cy -= 24.0/zoom camera() }
         elif key == k.KEY_DOWN { cy += 24.0/zoom camera() }
         elif fresh {
            if key == k.KEY_SPACE { running = !running acc = 0.0 }
            elif key == k.KEY_PERIOD { if gesture != 1 { running = false evolve() } }
            elif key == k.KEY_C { cancel_gesture() clear() running = false }
            elif key == k.KEY_R { cancel_gesture() preset(selected) }
            elif key == k.KEY_F { cancel_gesture() fit_pending = true }
            elif key == k.KEY_G { grid = !grid }
            elif key == k.KEY_P { theme = (theme+1)%PALETTES.len }
            elif key == k.KEY_PAGE_UP { rate = int(min(120,rate+5)) }
            elif key == k.KEY_PAGE_DOWN { rate = int(max(1,rate-5)) }
            elif key == k.KEY_TAB { cancel_gesture() panel = !panel update_view(w,h) }
            elif key >= k.KEY_1 && key <= k.KEY_9 { cancel_gesture() preset(key-k.KEY_1) }
         }
         if fit_pending { fit() }
      }
      if typ == k.EVENT_MOUSE_ENTER || (!cursor_ready && (typ == k.EVENT_MOUSE_POS_CHANGED || typ == k.EVENT_MOUSE_BUTTON_PRESSED)) {
         if arrow_cursor { win.set_cursor(window,arrow_cursor) cursor_ready = true }
      }
      if typ == k.EVENT_MOUSE_SCROLL || typ == k.EVENT_MOUSE_POS_CHANGED || typ == k.EVENT_MOUSE_BUTTON_PRESSED || typ == k.EVENT_MOUSE_BUTTON_RELEASED {
         def f64 x = float(d.get("x",mx/sx))*sx def f64 y = float(d.get("y",my/sy))*sy
         pointer_event(typ,x,y,int(d.get("button",-1)),float(d.get("dy",0.0)),int(d.get("mod",0)),w,h)
         if fit_pending { fit() }
      }
      if typ == k.EVENT_FOCUS_OUT || typ == k.EVENT_MOUSE_LEAVE || typ == k.EVENT_WINDOW_MINIMIZED { cancel_gesture() pointer_inside = false cursor_ready = false }
      elif typ == k.EVENT_WINDOW_RESIZED || typ == k.EVENT_SCALE_UPDATED { stroke_inside = false }
      if input_trace && (typ == k.EVENT_MOUSE_BUTTON_PRESSED || typ == k.EVENT_MOUSE_BUTTON_RELEASED || typ == k.EVENT_MOUSE_POS_CHANGED || typ == k.EVENT_MOUSE_LEAVE || typ == k.EVENT_FOCUS_OUT) { eprint(f"LIFE_INPUT type={typ} data={to_str(d)} gesture={gesture} held={held} generation={generation} population={population} active={sim_active()}") }
   }
}
fn packed(list c) int { gfx.color_pack(float(c.get(0,0.0)),float(c.get(1,0.0)),float(c.get(2,0.0)),float(c.get(3,1.0))) }
fn emit_rect(f64 x,f64 y,f64 w,f64 h,int color) any {
   if w <= 0.0 || h <= 0.0 || rect_count >= RECT_CAP { return 0 }
   def int off = rect_count*20
   store32_f32(rects,x,off) store32_f32(rects,y,off+4)
   store32_f32(rects,w,off+8) store32_f32(rects,h,off+12) store32(rects,color,off+16)
   rect_count += 1
}
fn build_world(f64 ox,f64 oy,f64 l,f64 t,f64 r,f64 b) any {
   if !board_dirty && cx == cached_x && cy == cached_y && zoom == cached_zoom && vw == cached_w && vh == cached_h && theme == cached_theme && grid == cached_grid { return 0 }
   cached_x = cx cached_y = cy cached_zoom = zoom cached_w = vw cached_h = vh cached_theme = theme cached_grid = grid
   board_dirty = false rect_count = 0 board_builds += 1
   if l >= r || t >= b { return 0 }
   def int x0 = int(clamp(floor(-ox/zoom),0,N)) def int x1 = int(clamp(ceil((vw-ox)/zoom),0,N))
   def int y0 = int(clamp(floor(-oy/zoom),0,N)) def int y1 = int(clamp(ceil((vh-oy)/zoom),0,N))
   def int color = packed(LIVE.get(theme,[])) def int line = packed(GRID_COLOR.get(theme,[]))
   emit_rect(l,t,r-l,b-t,packed(BOARD.get(theme,[])))
   def f64 gap = grid && zoom >= 9.0 ? 1.0 : 0.0
   if gap > 0.0 {
      mut int x = x0 while x <= x1 { def f64 px = ox+float(x)*zoom if px >= l && px < r { emit_rect(px,t,float(min(1.0,r-px)),b-t,line) } x += 1 }
      mut int y = y0 while y <= y1 { def f64 py = oy+float(y)*zoom if py >= t && py < b { emit_rect(l,py,r-l,float(min(1.0,b-py)),line) } y += 1 }
   }
   mut int y = y0
   while y < y1 {
      mut int x = x0
      def int row = at(0,y)
      def f64 u = float(max(t,oy+float(y)*zoom+gap)) def f64 v = float(min(b,oy+float(y+1)*zoom))
      while x < x1 {
         if load8(cells,row+x) == 0 { x += 1 continue }
         def int start = x x += 1
         while gap == 0.0 && x < x1 && load8(cells,row+x) != 0 { x += 1 }
         def f64 a = float(max(l,ox+float(start)*zoom+gap)) def f64 c = float(min(r,ox+float(x)*zoom))
         emit_rect(a,u,c-a,v-u,color)
      }
      y += 1
   }
}
fn draw_world() any {
   def f64 ox = vw*0.5-cx*zoom def f64 oy = vh*0.5-cy*zoom
   def f64 l = float(max(0.0,ox)) def f64 t = float(max(0.0,oy)) def f64 r = float(min(vw,ox+float(N)*zoom)) def f64 b = float(min(vh,oy+float(N)*zoom))
   build_world(ox,oy,l,t,r,b)
   if l >= r || t >= b { return 0 }
   mut int off = 0
   while off < rect_count {
      def int n = int(min(512,rect_count-off))
      if gfx.draw_rects_fast_ptr(rects+off*20,n) != n { break }
      off += n
   }
   gfx.draw_rect_outline_fast(l,t,r-l,b-t,packed(GRID_COLOR.get(theme,[])))
   if pointer_inside && inside_world(mx,my) && gesture != 2 && zoom >= 2.0 && valid_cell(cell_x(mx),cell_y(my)) {
      def f64 px = ox+float(cell_x(mx))*zoom def f64 py = oy+float(cell_y(my))*zoom
      def f64 rad = float(brush-1)*zoom
      def f64 a = float(max(l,px-rad)) def f64 u = float(max(t,py-rad))
      def f64 c = float(min(r,px+zoom+rad)) def f64 v = float(min(b,py+zoom+rad))
      if c > a && v > u { gfx.draw_rect_outline_fast(a,u,c-a,v-u,gfx.color_pack(1.0,1.0,1.0,1.0)) }
   }
}
fn hit(f64 px,f64 py,f64 x,f64 y,f64 w,f64 h) bool { px >= x && px < x+w && py >= y && py < y+h && py >= 0.0 && py < vh }
fn button(str label,f64 x,f64 y,f64 w,f64 h,bool active=false) bool {
   if y+h <= 0.0 || y >= vh { return false }
   def bool hover = pointer_inside && hit(mx,my,x,y,w,h)
   if ui_draw {
      def list accent = LIVE.get(theme,[])
      def list fill = active ? [float(accent.get(0,0.0))*0.12,float(accent.get(1,0.0))*0.12,float(accent.get(2,0.0))*0.12,1.0] : hover ? [0.12,0.12,0.12,1.0] : [0.045,0.045,0.045,1.0]
      gfx.draw_rect(x,y,w,h,fill)
      if active { gfx.draw_rect(x,y+3.0,2.0,h-6.0,accent) }
      gfx.draw_text(font,label,x+8.0,y+(h-16.0)*0.5,active ? accent : [0.86,0.86,0.86,1.0])
   }
   !ui_draw && clicked && hit(press_x,press_y,x,y,w,h) && hit(click_x,click_y,x,y,w,h)
}
fn label(str txt,f64 x,f64 y,bool dim=false) any {
   if ui_draw && y+18.0 > 0.0 && y < vh { gfx.draw_text(font,txt,x,y,dim ? [0.61,0.61,0.61,1.0] : [0.92,0.92,0.92,1.0]) }
}
fn controls(f64 w,f64 h) any {
   def f64 x = vw+14.0 def f64 bw = w-vw-28.0 def f64 half = (bw-6.0)*0.5
   def f64 q = 1.0 def f64 rh = 28.0 def f64 gap = 5.0
   if ui_draw { gfx.draw_rect(vw,0.0,w-vw,h,[0.0,0.0,0.0,1.0]) gfx.draw_rect(vw,0.0,1.0,h,[0.10,0.10,0.10,1.0]) gfx.scissor_push(vw,0.0,w-vw,h) }
   mut f64 y = 14.0-panel_scroll
   label("CONWAY / LIFE",x,y)
   if ui_draw { gfx.draw_rect(x+bw-22.0,y+5.0,20.0,3.0,LIVE.get(theme,[])) }
   y += 25.0*q
   if ui_draw && y+19.0 > 0.0 && y < vh {
      gfx.draw_rect(x,y+5.0,7.0,7.0,gesture == 1 ? (theme == DEFAULT_PALETTE ? [0.78,0.78,0.78,1.0] : [1.0,0.69,0.30,1.0]) : running ? LIVE.get(theme,[]) : [0.43,0.43,0.43,1.0])
   }
   label(gesture == 1 ? "EDITING" : running ? "RUNNING" : "PAUSED",x+14.0,y,true) y += 23.0*q
   label(f"Generation  {generation}",x,y) y += 21.0*q
   label(f"Population  {population}",x,y) y += 21.0*q
   label(f"+{births} / -{deaths}   {round(step_ms,2)} ms",x,y,true) y += 27.0*q
   if button(running ? "Pause" : "Run",x,y,half,rh,running) { running = !running acc = 0.0 }
   if button("Step",x+half+6.0,y,half,rh) { running = false evolve() }
   y += rh+gap
   label(f"Speed {rate}/s",x,y+5.0*q)
   if button("-",x+bw-70.0,y,32.0,rh) { rate = int(max(1,rate-5)) }
   if button("+",x+bw-32.0,y,32.0,rh) { rate = int(min(120,rate+5)) }
   y += rh+12.0*q
   label("PATTERNS",x,y,true) y += 20.0*q
   mut int i = 0
   while i < names.len {
      if button(to_str(names.get(i,"")),x+float(i%2)*(half+6.0),y+float(i>>1)*(rh+gap),half,rh,i == selected) { preset(i) }
      i += 1
   }
   y += 5.0*(rh+gap)
   if button("Reseed",x,y,half,rh) { preset(selected) }
   if button("Clear",x+half+6.0,y,half,rh) { clear() running = false }
   y += rh+gap
   label(f"Density {density} pct",x,y+5.0*q)
   if button("-",x+bw-70.0,y,32.0,rh) { density = int(max(1,density-5)) }
   if button("+",x+bw-32.0,y,32.0,rh) { density = int(min(60,density+5)) }
   y += rh+gap
   if button(wrap ? "Wrap: on" : "Wrap: off",x,y,half,rh,wrap) { wrap = !wrap }
   if button(grid ? "Grid: on" : "Grid: off",x+half+6.0,y,half,rh,grid) { grid = !grid }
   y += rh+gap
   label(f"Brush {brush}",x,y+5.0*q)
   if button("-",x+bw-70.0,y,32.0,rh) { brush = int(max(1,brush-1)) }
   if button("+",x+bw-32.0,y,32.0,rh) { brush = int(min(8,brush+1)) }
   y += rh+gap
   if button("Fit view",x,y,half,rh) { fit_pending = true }
   if button(to_str(PALETTES.get(theme,"Palette")),x+half+6.0,y,half,rh,true) { theme = (theme+1)%PALETTES.len }
   y += rh+gap
   label("LMB draw / RMB erase",x,y,true) y += 19.0
   label("MMB or Shift+drag: pan",x,y,true) y += 19.0
   label("Wheel zoom / Tab sidebar",x,y,true) y += 19.0
   label("Release to resume / F fit",x,y,true) y += 19.0
   if ui_draw {
      gfx.scissor_pop()
      if panel_max > 0.0 { def f64 sh = h*h/(h+panel_max) gfx.draw_rect(w-4.0,(h-sh)*panel_scroll/panel_max,3.0,sh,[0.28,0.28,0.28,1.0]) }
   }
}
fn input_probe() any {
   vw = 682.0 vh = 680.0 zoom = 6.0 cx = 128.0 cy = 128.0
   def f64 x = 400.0 def f64 y = 300.0
   def f64 wx = cx+(x-vw*0.5)/zoom def f64 wy = cy+(y-vh*0.5)/zoom
   wheel_zoom(1.0,x,y) assert(zoom > 6.0,"wheel up")
   assert(float(abs(cx+(x-vw*0.5)/zoom-wx)) < 0.000001 && float(abs(cy+(y-vh*0.5)/zoom-wy)) < 0.000001,"zoom anchor")
   wheel_zoom(-1.0,x,y) assert(float(abs(zoom-6.0)) < 0.000001,"wheel down")
   zoom = 6.0 cx = 128.0 cy = 128.0
   clear() running = true brush = 1
   pointer_event(k.EVENT_MOUSE_BUTTON_PRESSED,344.0,343.0,0,0.0,0,960.0,680.0)
   assert(!sim_active() && running && gesture == 1,"hold pauses")
   pointer_event(k.EVENT_MOUSE_POS_CHANGED,368.0,343.0,-1,0.0,0,960.0,680.0)
   pointer_event(k.EVENT_MOUSE_BUTTON_RELEASED,368.0,343.0,1,0.0,0,960.0,680.0) assert(gesture == 1,"unrelated release")
   pointer_event(k.EVENT_MOUSE_BUTTON_RELEASED,368.0,343.0,0,0.0,0,960.0,680.0) assert(gesture == 0 && population == 5 && sim_active(),"release resumes")
   pointer_event(k.EVENT_MOUSE_BUTTON_PRESSED,344.0,343.0,0,0.0,k.MOD_SHIFT,960.0,680.0)
   def f64 old = cx
   pointer_event(k.EVENT_MOUSE_POS_CHANGED,368.0,343.0,-1,0.0,0,960.0,680.0) assert(cx < old && population == 5,"pan no paint")
   cancel_gesture() assert(gesture == 0 && held == -1,"cancel")
   panel_scroll = 0.0
   pointer_event(k.EVENT_MOUSE_BUTTON_PRESSED,710.0,150.0,0,0.0,0,960.0,680.0)
   pointer_event(k.EVENT_MOUSE_BUTTON_RELEASED,710.0,150.0,0,0.0,0,960.0,680.0) assert(!running,"sidebar action")
   pointer_event(k.EVENT_MOUSE_BUTTON_PRESSED,710.0,150.0,0,0.0,0,960.0,680.0)
   pointer_event(k.EVENT_MOUSE_BUTTON_RELEASED,400.0,150.0,0,0.0,0,960.0,680.0) assert(!running,"sidebar action")
   pointer_event(k.EVENT_MOUSE_BUTTON_PRESSED,344.0,343.0,1,0.0,0,960.0,680.0)
   assert(!sim_active() && !running && gesture == 1,"manual pause during erase")
   pointer_event(k.EVENT_MOUSE_BUTTON_RELEASED,344.0,343.0,1,0.0,0,960.0,680.0)
   assert(!sim_active() && !running && gesture == 0,"manual pause preserved")
   running = true
   pointer_event(k.EVENT_MOUSE_BUTTON_PRESSED,344.0,343.0,0,0.0,0,960.0,680.0)
   assert(!sim_active(),"second hold")
   pointer_event(k.EVENT_MOUSE_POS_CHANGED,710.0,343.0,-1,0.0,0,960.0,680.0)
   assert(sim_active() && gesture == 0,"leave canvas resumes")
   pointer_event(k.EVENT_MOUSE_BUTTON_PRESSED,344.0,343.0,0,0.0,0,960.0,680.0)
   cancel_gesture()
   assert(sim_active() && held == -1,"focus loss resumes")
   clear() running = true wrap = false zoom = 6.0 cx = 128.0 cy = 128.0 brush = 1
   pointer_event(k.EVENT_MOUSE_BUTTON_PRESSED,344.0,343.0,0,0.0,0,960.0,680.0)
   mut int frame = 0
   while frame < 30 {
      edited_frame = false tick_sim(0.1)
      assert(generation == 0 && gesture == 1 && !sim_active(),"hold across frames")
      if frame == 10 { pointer_event(k.EVENT_MOUSE_POS_CHANGED,368.0,343.0,-1,0.0,0,960.0,680.0) }
      frame += 1
   }
   assert(population == 5,"continuous brush")
   update_view(961.0,680.0) assert(gesture == 1 && !sim_active(),"resize does not release brush")
   pointer_event(k.EVENT_MOUSE_BUTTON_RELEASED,368.0,343.0,0,0.0,0,961.0,680.0)
   tick_sim(0.1) assert(generation == 0,"release frame preserves drawing")
   edited_frame = false tick_sim(0.1) assert(generation > 0 && sim_active(),"simulation resumes")
   clear() running = true
   pointer_event(k.EVENT_MOUSE_BUTTON_PRESSED,344.0,343.0,1,0.0,0,961.0,680.0)
   edited_frame = false tick_sim(0.1) assert(generation == 0 && gesture == 1,"eraser freezes simulation")
   cancel_gesture()
   print("LIFE_INPUT_OK")
}
fn geometry_probe() any {
   clear() vw = 2048.0 vh = 2048.0 cx = 128.0 cy = 128.0 zoom = 8.0 grid = false
   mut int y = 0
   while y < N { mut int x = 0 while x < N { if ((x+y)&1) == 0 { put(x,y,1) } x += 1 } y += 1 }
   build_world(0.0,0.0,0.0,0.0,2048.0,2048.0)
   assert(rect_count == N*N/2+1,"checkerboard batch")
   def int cached = board_builds
   build_world(0.0,0.0,0.0,0.0,2048.0,2048.0)
   assert(board_builds == cached,"geometry cache")
   put(1,0,1) build_world(0.0,0.0,0.0,0.0,2048.0,2048.0)
   assert(board_builds == cached+1,"paint invalidates cache")
   theme = (theme+1)%PALETTES.len build_world(0.0,0.0,0.0,0.0,2048.0,2048.0)
   assert(board_builds == cached+2,"palette invalidates cache")
   clear() brush = 1 ink = 1
   stroke(0.5,0.5,2047.5,2047.5)
   assert(population == N,"bounded diagonal stroke")
   mut int i = 0
   while i < N { assert(load8(cells,at(i,i)) == 1,"diagonal follows pointer") i += 1 }
   mut int octant = 0
   while octant < 8 {
      clear()
      def int sx = (octant & 1) == 0 ? 1 : -1
      def int sy = (octant & 2) == 0 ? 1 : -1
      def bool steep = (octant & 4) != 0
      def int dx = steep ? 4 : 8 def int dy = steep ? 8 : 4
      stroke(804.0,804.0,804.0+float(sx*dx)*8.0,804.0+float(sy*dy)*8.0)
      assert(population == 9,"eight-octant stroke length")
      mut int j = 0
      while j <= 8 {
         def int a = steep ? int(j/2) : j def int b = steep ? j : int(j/2)
         assert(load8(cells,at(100+sx*a,100+sy*b)) == 1,"eight-octant stroke position")
         j += 1
      }
      octant += 1
   }
   clear()
   stroke(804.0,804.0,804.0,884.0)
   assert(population == 11,"vertical stroke length")
   i = 100 while i <= 110 { assert(load8(cells,at(100,i)) == 1,"vertical stroke position") i += 1 }
   ink = 0 stroke(804.0,884.0,804.0,804.0) assert(population == 0,"reverse vertical erase")
   ink = 1 brush = 3 stroke(804.0,804.0,884.0,884.0)
   assert(load8(cells,at(105,105)) == 1 && load8(cells,at(105,106)) == 1 && load8(cells,at(105,100)) == 0,"round brush follows diagonal")
   ink = 0 stroke(804.0,804.0,884.0,884.0) assert(population == 0,"round brush erase")
   brush = 1 ink = 1
   print("LIFE_STROKE_OK")
   print("LIFE_GEOMETRY_OK")
}
fn probe() int {
   input_probe() geometry_probe()
   clear() wrap = false put(10,10,1) put(11,10,1) put(12,10,1) evolve()
   assert(population == 3 && births == 2 && deaths == 2 && load8(cells,at(11,9)) == 1 && load8(cells,at(11,11)) == 1)
   evolve() assert(load8(cells,at(10,10)) == 1 && load8(cells,at(12,10)) == 1)
   clear() wrap = true put(N-1,0,1) put(0,0,1) put(1,0,1) evolve()
   assert(population == 3 && load8(cells,at(0,N-1)) == 1 && load8(cells,at(0,1)) == 1)
   clear() wrap = false put(N-1,0,1) put(0,0,1) put(1,0,1) evolve() assert(population == 0)
   rng.seed(12345) preset(0) def int start = os.monotonic_ns()
   mut int i = 0 while i < 120 { evolve() i += 1 }
   mut int actual = 0 mut int y = 0
   while y < N { mut int x = 0 while x < N { actual += load8(cells,at(x,y)) x += 1 } y += 1 }
   assert(actual == population) print(f"LIFE_OK generations={generation} population={population} mean_ms={float(os.monotonic_ns()-start)/120000000.0}")
   0
}
fn main() int {
   cells = malloc_raw(TOTAL) next = malloc_raw(TOTAL) rects = malloc_raw(RECT_CAP*20)
   if !cells || !next || !rects { release() eprint("conway: allocation failed") return 1 }
   if common.env_truthy("NY_LIFE_PROBE") { assert(theme == DEFAULT_PALETTE && packed(BOARD.get(theme,[])) == gfx.color_pack(0.0,0.0,0.0,1.0),"AMOLED default") def int rc = probe() release() return rc }
   def bool trace = common.env_truthy("NY_LIFE_TRACE")
   input_trace = common.env_truthy("NY_LIFE_INPUT_TRACE")
   if trace { eprint("LIFE_STAGE init") }
   gfx.apply_backend_env() gfx.apply_backend_argv()
   def window = gfx.init_window(960,680,"Conway | Game of Life",0,false,false,1)
   if !window { release() eprint("conway: "+gfx.window_error()) return 1 }
   if gfx.window_error().len > 0 { eprint("conway: "+gfx.window_error()) gfx.close_window() release() return 1 }
   win.set_cursor_mode(window,win.CURSOR_NORMAL)
   arrow_cursor = win.create_standard_cursor(win.ARROW_CURSOR)
   if arrow_cursor && win.backend() != "wayland" { win.set_cursor(window,arrow_cursor) cursor_ready = true }
   if trace { eprint("LIFE_STAGE seed / " + gfx.get_active_backend_name()) }
   rng.seed(time.ticks()) preset(common.env_int_clamped("NY_LIFE_PATTERN",0,0,8))
   if trace { eprint("LIFE_STAGE font") }
   def list fonts = extend(["etc/assets/fonts/maplemono.ttf","etc/assets/fonts/jetbrains.ttf","etc/assets/fonts/monocraft.ttf"],assets.UI_FONT_CANDIDATES)
   font = gfx.font_load_first(fonts,16)
   if font <= 0 { eprint("conway: no usable UI font") if arrow_cursor { win.destroy_cursor(arrow_cursor) } gfx.close_window() release() return 1 }
   theme = common.env_int_clamped("NY_LIFE_PALETTE",DEFAULT_PALETTE,0,PALETTES.len-1)
   def int limit = common.env_int_clamped("NY_LIFE_FRAMES",0,0,10000)
   def str snapshot = to_str(os.env("NY_LIFE_SNAPSHOT"))
   mut int frames = 0 mut int failed = 0 mut int surface_since = 0 running = !common.env_truthy("NY_LIFE_PAUSED")
   mut int last = os.monotonic_ns()
   while true {
      if trace { eprint(f"LIFE_STAGE frame={frames} poll") }
      if gfx.window_should_close(window) { break }
      def int frame_start = os.monotonic_ns()
      def list fb = gfx.framebuffer_size_f64(960.0,680.0)
      def f64 w = float(fb.get(0,960.0)) def f64 h = float(fb.get(1,680.0))
      if w < 1.0 || h < 1.0 || win.is_window_minimized(window) {
         input(window,vw,vh) cancel_gesture() last = os.monotonic_ns() acc = 0.0 surface_since = 0
         os.msleep(32) continue
      }
      update_view(w,h)
      if trace { eprint(f"LIFE_STAGE frame={frames} input") }
      edited_frame = false
      input(window,w,h)
      if win.should_close(window) { break }
      def int now = os.monotonic_ns() def f64 dt = float(now-last)/1000000000.0 last = now
      tick_sim(dt)
      if trace { eprint(f"LIFE_STAGE frame={frames} begin") }
      if !gfx.begin_frame_clear(gfx.BLACK) {
         if surface_since == 0 { surface_since = os.monotonic_ns() }
         if os.monotonic_ns()-surface_since >= 3000000000 { failed = 120 eprint("conway: surface unavailable") break }
         os.msleep(16) continue
      }
      surface_since = 0
      failed = 0 gfx.set_ortho_2d(0.0,w,h,0.0)
      if trace { eprint(f"LIFE_STAGE frame={frames} board") }
      draw_world()
      if vw < w { controls(w,h) }
      clicked = false
      if trace { eprint(f"LIFE_STAGE frame={frames} present rects={rect_count}") }
      if !gfx.end_frame() { failed = 120 eprint("conway: presentation failed") break }
      frames += 1
      if trace { eprint(f"LIFE_STAGE frame={frames} OK") }
      if frames == 1 && snapshot.len > 0 && snapshot != "nil" { if !gfx.snapshot(snapshot) { eprint("conway: snapshot failed") } }
      if limit > 0 && frames >= limit { break }
      def int target = sim_active() || gesture != 0 ? 16666667 : 33333333
      def int sleep_ms = int((target-(os.monotonic_ns()-frame_start))/1000000)
      if sleep_ms > 0 { os.msleep(sleep_ms) }
   }
   if arrow_cursor { win.destroy_cursor(arrow_cursor) arrow_cursor = 0 }
   gfx.close_window() release()
   if failed >= 120 { return 1 }
   0
}
