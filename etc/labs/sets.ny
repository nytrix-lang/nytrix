#!/usr/bin/env ny

;; Keywords: ui render shader fractal mandelbrot julia complex math gpu
;; Interactive Mandelbrot and Julia sets with adaptive FP32/FP64 Vulkan shading,
;; precision-aware zoom and framebuffer PNG export.
;; Keys: M/J switch sets, 1..6 presets, scroll zoom, drag pan, R reset,
;; Space animate Julia, Tab/F1 toggle controls, Enter export, Esc quit.

use core
use core.common as common
use core.math (log, exp, cos, sin, min, max, clamp)
use core.os.ui.render.vk as vk
use core.os.ui.render.vk.vulkan as vulkan
use core.os.ui.render.vk.state as vks
use core.os.ui.render.vk.pipeline as vkp
use core.os as os
use core.os.fs as fs
use core.math.parse.img as img
use core.os.ui.render as gfx
use core.os.ui.window as win
use core.os.ui.window.consts as k
use core.os.ui.render.viewer.gui as gui
use core.os.ui.assets.viewer as assets

def MANDEL = [[-1.76826775,-0.00422996283,0.00005275],[0.322004497,-0.0357099883,0.00002655],[-0.748880744,-0.0562955774,0.00016238],[-1.78385007,-0.0156200649,0.00010274],[-0.0985441282,-0.924688697,0.00005712],[0.317785531,-0.0322612226,0.00005120],[-0.743643887037,0.131825904205,0.012],[-0.408333333333,-0.618333333333,0.0833333333],[-0.596666666667,-0.656666666667,0.1666666667]]
def JULIA = [[-0.348827,0.607167],[-0.786268,0.169728],[-0.8,0.156],[0.285,0.0],[-0.835,-0.2321],[-0.70176,-0.3842]]
def VS = """#version 450
layout(location=1) in vec2 uv;
layout(location=0) out vec2 p;
layout(push_constant) uniform Params { layout(offset=108) float width_ratio; } pc;
void main(){
    p=uv;
    gl_Position=vec4(2.0*uv.x*pc.width_ratio-1.0,1.0-2.0*uv.y,0.0,1.0);
}
"""
def FS = """
layout(location=0) in vec2 p;
layout(location=0) out vec4 color;
layout(push_constant) uniform Params { layout(offset=64) REAL cx; layout(offset=72) REAL cy; layout(offset=80) REAL span; layout(offset=88) REAL jr; layout(offset=96) REAL ji; layout(offset=104) float aspect; layout(offset=108) float width_ratio; layout(offset=112) ivec4 settings; } pc;
void main(){
    VEC point=VEC(pc.cx,pc.cy)+(VEC(p)-REAL(0.5))*VEC(REAL(pc.aspect),REAL(1.0))*pc.span;
    bool julia=pc.settings.x==1;
    VEC c=julia?VEC(pc.jr,pc.ji):point;
    VEC z=julia?point:VEC(0.0);
    REAL qx=c.x-REAL(0.25), q=qx*qx+c.y*c.y;
    bool inside=!julia && (q*(q+qx)<=0.25*c.y*c.y || dot(c+VEC(1,0),c+VEC(1,0))<=0.0625);
    REAL r2=dot(z,z);
    VEC checkpoint=z;
    int block=8,next=8;
    int i=0;
    if(!inside) for(;i<pc.settings.y;i++){
        z=VEC(z.x*z.x-z.y*z.y,REAL(2.0)*z.x*z.y)+c;
        r2=dot(z,z);
        if(r2>256.0){i++;break;}
        if(all(equal(z,checkpoint))){i=pc.settings.y;break;}
        if(i+1==next){checkpoint=z;block*=2;next+=block;}
    }
    if(inside || i>=pc.settings.y && r2<=256.0){color=vec4(0.027,0.039,0.075,1.0);return;}
    float n=max(0.0,float(i)+1.0-log2(log(max(float(r2),4.0))*0.5));
    float t=log(1.0+n)*2.1;
    vec3 rgb=vec3(130,130,150)+vec3(120,120,105)*cos(t+vec3(2,.2,-.7));
    if(pc.settings.z==1) rgb=vec3(140,105,80)+vec3(115,100,75)*cos(t+vec3(0,.9,2));
    if(pc.settings.z==2) rgb=vec3(clamp(n/float(pc.settings.y)*255.0,0.0,255.0));
    color=vec4(clamp(rgb/255.0,0.0,1.0),1.0);
}
"""
mut int mode = 0
mut f64 jr = -0.348827
mut f64 ji = 0.607167
mut bool animate = false
mut f64 phase = 0.0
mut f64 speed = 0.5
mut any pipe = 0
mut any pipe64 = 0
mut ptr pc = 0
mut bool auto_iterations = true
mut bool precision_limit = false
mut f64 zoom_delta = 0.0
mut f64 zoom_x = 0.0
mut f64 zoom_y = 0.0
mut f64 cx = -0.5
mut f64 cy = 0.0
mut f64 span = 3.0
mut int iterations = 256
mut int palette = 0
mut bool panel = true
mut bool dragging = false
mut bool save_pending = false
mut f64 last_change = 0.0
mut f64 mx = 0.0
mut f64 my = 0.0
mut f64 vw = 640.0
mut f64 vh = 480.0
mut str status = "Ready"

fn invalidate() any {
   precision_limit = false
   last_change = gfx.get_time()
}
fn move_center(f64 dx, f64 dy) any { cx += dx cy += dy }
fn reset() any { zoom_delta = 0.0 cx = mode == 0 ? -0.5 : 0.0 cy = 0.0 span = 3.0 invalidate() }
fn switch_set(int m) any {
   if mode != m { mode = m animate = false reset() }
}
fn preset(int i) any {
   zoom_delta = 0.0
   animate = false
   if mode == 0 {

      def list v = MANDEL.get(i)
      cx = float(v.get(0)) cy = float(v.get(1)) span = float(v.get(2))
   } else {
      def list v = JULIA.get(i)
      jr = float(v.get(0)) ji = float(v.get(1)) reset()
   }
   invalidate()
   enforce_precision()
}
fn magnitude() f64 {
   float(max(0.001,max(cx < 0.0 ? -cx : cx,cy < 0.0 ? -cy : cy)))
}
fn pixel_floor() f64 {
   magnitude()*(pipe64 ? 8.881784197001252e-16 : 4.76837158203125e-7)*float(max(1.0,vh))
}
fn use_fp64() bool {
   bool(pipe64) && (!pipe || span/float(max(1.0,vh)) < magnitude()*0.000002)
}
fn enforce_precision() any {
   def f64 floor = pixel_floor()
   if span < floor { span = floor precision_limit = true zoom_delta = 0.0 }
}
fn effective_limit() int {
   if !auto_iterations { return iterations }
   def f64 levels = float(max(0.0,log(3.0/span)/0.6931471805599453))
   int(clamp(float(iterations)+levels*48.0,64.0,8192.0))
}
fn live_limit() int {
   def int n = effective_limit()
   if dragging || animate || gfx.get_time()-last_change < 0.18 { return int(min(n,512)) }
   n
}
fn fp64_supported() bool {
   if !vks._physical_device { return false }
   def ptr f = malloc_raw(256)
   if !f { return false }
   memset(f,0,256)
   store32(f,1000059000,0)
   vulkan.get_physical_device_features2(vks._physical_device,f)
   def bool ok = load32(f,172) != 0
   free_raw(f)
   ok
}
fn build_pipeline(any v, bool wide) any {
   def str prefix = wide ? "#version 450\n#extension GL_ARB_gpu_shader_fp64 : require\n#define REAL double\n#define VEC dvec2\n" : "#version 450\n#define REAL float\n#define VEC vec2\n"
   def f = gfx.compile_shader(prefix+FS,"frag")
   if !f { return 0 }
   def result = gfx.create_pipeline(v,f,3,0,0,0)
   vulkan.destroy_shader_module(vk._get_device(),f,0)
   result
}
fn init_shader() bool {
   if !gfx.vk_available() || vkp.shader_pc_bytes() < 128 { return false }
   def v = gfx.compile_shader(VS,"vert")
   if !v { return false }
   pipe = build_pipeline(v,false)
   if fp64_supported() { pipe64 = build_pipeline(v,true) }
   vulkan.destroy_shader_module(vk._get_device(),v,0)
   if !pipe && !pipe64 { return false }
   pc = malloc_raw(64)
   bool(pc)
}
fn draw_gpu(f64 w, bool full=false) bool {
   def bool wide = use_fp64()
   memset(pc,0,64)
   if wide {
      store64_f64(pc,cx,0) store64_f64(pc,cy,8) store64_f64(pc,span,16)
      store64_f64(pc,jr,24) store64_f64(pc,ji,32)
   } else {
      store32_f32(pc,cx,0) store32_f32(pc,cy,8) store32_f32(pc,span,16)
      store32_f32(pc,jr,24) store32_f32(pc,ji,32)
   }
   store32_f32(pc,vw/vh,40) store32_f32(pc,vw/w,44)
   store32(pc,mode,48) store32(pc,full ? effective_limit() : live_limit(),52) store32(pc,palette,56) store32(pc,0,60)
   gfx.draw_shader_rect(0.0,0.0,vw,vh,wide ? pipe64 : pipe,pc,64,64)
}
fn zoom_now(f64 factor, f64 x, f64 y) any {
   def f64 next = float(clamp(span*factor,pixel_floor(),20.0))
   if next == span { precision_limit = factor < 1.0 return 0 }
   move_center((x-vw*0.5)*(span-next)/vh,(y-vh*0.5)*(span-next)/vh)
   span = next
   invalidate()
   enforce_precision()
   if factor < 1.0 && span <= pixel_floor() { precision_limit = true }
}
fn zoom(f64 factor, f64 x, f64 y) any {
   if factor <= 0.0 { return 0 }
   if dragging { zoom_now(factor,x,y) return 0 }
   zoom_delta = float(clamp(zoom_delta+log(factor),-12.0,12.0))
   zoom_x = x zoom_y = y
}
fn tick_zoom() any {
   if zoom_delta == 0.0 { return 0 }
   def f64 dt = float(clamp(gfx.get_frame_time(),0.005,0.05))
   def f64 t = 1.0-exp(-dt*24.0)
   def f64 step = zoom_delta > -0.002 && zoom_delta < 0.002 ? zoom_delta : zoom_delta*t
   zoom_delta -= step
   zoom_now(exp(step),zoom_x,zoom_y)
   if precision_limit { zoom_delta = 0.0 }
}

fn save_image() any {
   animate = false
   zoom_delta = 0.0
   save_pending = true
   status = "Export queued"
}
fn export_frame(int width, int height, str path="") bool {
   def fb = gfx.read_framebuffer()
   if !is_dict(fb) { status = "Framebuffer capture failed" return false }
   def ptr src = fb.get("data",0)
   def int w = int(fb.get("width",0))
   def int h = int(fb.get("height",0))
   if !src { status = "Framebuffer capture failed" return false }
   if width <= 0 || height <= 0 || width > w || height != h || width*height > 134217728 {
      free(src)
      status = "Framebuffer resized; retry export"
      return false
   }
   def ptr rgba = malloc_raw(width*height*4)
   if !rgba { free(src) status = "Export allocation failed" return false }
   mut int y = 0
   while y < height {
      memcpy(rgba+y*width*4,src+y*w*4,width*4)
      y += 1
   }
   free(src)
   mut str target = path
   if target.len == 0 {
      if !os.is_dir("sets") && !is_ok(fs.make_dir("sets")) { free_raw(rgba) status = "Cannot create sets directory" return false }
      target = "sets/set-"+to_str(os.now_ms())+".png"
   }
   def data = img.encode({"width":width,"height":height,"channels":4,"data":rgba},"png")
   free_raw(rgba)
   def bool ok = bool(data) && is_ok(os.file_write(target,data))
   status = ok ? "Saved "+target : "PNG export failed"
   print(status)
   ok
}
fn release_shader() any {
   if pipe || pipe64 { gfx.renderer_wait_idle() }
   if pipe { vulkan.destroy_pipeline(vk._get_device(),pipe,0) pipe = 0 }
   if pipe64 { vulkan.destroy_pipeline(vk._get_device(),pipe64,0) pipe64 = 0 }
   if bool(pc) { free_raw(pc) pc = 0 }
}

fn controls(f64 w, f64 h) any {
   if !panel { return 0 }
   gui.apply_window_rect("sets",[vw+6.0,6.0,w-vw-12.0,h-12.0])
   if gui.begin_window("sets", "Fractal sets", vw+6.0, 6.0, w-vw-12.0, h-12.0, {"closable":false,"movable":false,"resizable":false,"collapsible":false}) {
      def int m = gui.combo_box("mode","Set",["Mandelbrot","Julia"],mode)
      switch_set(m)
      gui.separator()
      if gui.button("zin","Zoom +",100.0) { zoom(0.8,vw*0.5,vh*0.5) }
      gui.same_line(6.0)
      if gui.button("zout","Zoom -",100.0) { zoom(1.25,vw*0.5,vh*0.5) }
      if gui.button("reset","Reset view",gui.remaining_w()) { reset() }
      def bool adaptive = gui.checkbox("adaptive","Adaptive iterations",auto_iterations)
      if adaptive != auto_iterations { auto_iterations = adaptive invalidate() }
      def int n = gui.slider_int("iterations","Base iterations",iterations,64,2048)
      if n != iterations { iterations = n invalidate() }
      def int p = gui.combo_box("palette","Palette",["Spectrum","Ember","Grayscale"],palette)
      if p != palette { palette = p invalidate() }
      gui.separator()
      gui.text_colored("PRESETS  /  1 - 6", "text_dim")
      mut int i = 0
      while i < 6 {
         if gui.button("preset"+to_str(i),to_str(i+1),64.0) { preset(i) }
         if i%3 != 2 { gui.same_line(6.0) }
         i += 1
      }
      if mode == 0 {
         if gui.button("seahorse","Seahorse valley",gui.remaining_w()) { preset(6) }
         if gui.button("original1","Original view I",gui.remaining_w()) { preset(7) }
         if gui.button("original2","Original view II",gui.remaining_w()) { preset(8) }
      } else {
         def f64 x = gui.slider_float("jr","Real c",jr,-1.5,1.5)
         def f64 y = gui.slider_float("ji","Imaginary c",ji,-1.5,1.5)
         if x != jr || y != ji { jr = x ji = y animate = false invalidate() }
         animate = gui.checkbox("animate","Animate Julia",animate)
         speed = gui.slider_float("speed","Animation speed",speed,0.05,2.0)
      }
      gui.separator()
      gui.text_colored(use_fp64() ? "VULKAN / FP64" : "VULKAN / FP32", "text_dim")
      gui.text("Iterations: "+to_str(effective_limit()))
      if precision_limit { gui.text_colored("Precision limit reached", "text_dim") }
      gui.text("X: "+to_str(cx))
      gui.text("Y: "+to_str(cy))
      gui.text("Span: "+to_str(span))
      gui.text("Zoom: "+to_str(3.0/span)+"x")
      gui.text(status)
      if gui.button("save","Export PNG",gui.remaining_w()) { save_image() }
      gui.separator()
      gui.text_colored("Scroll / Z X: zoom", "text_dim")
      gui.text_colored("Drag / arrows: pan", "text_dim")
      gui.text_colored("M / J: set   R: reset", "text_dim")
      gui.text_colored("Tab / F1: panel   Esc: quit", "text_dim")
   }
   gui.end_window()
}

fn input(any window, f64 w, f64 h) any {
   def list sz = win.size(window)
   def f64 sx = w/float(max(1.0,float(sz.get(0,w))))
   def f64 sy = h/float(max(1.0,float(sz.get(1,h))))
   mut int count = 0
   mut any e = win.check_event(window)
   while count < 128 && !is_nil(e) && !(is_int(e) && int(e) == 0) {
      def int typ = win.event_type(e)
      def any d = win.event_data(e)
      gui.feed_event(typ,d)
      if typ == k.EVENT_KEY_PRESSED {
         def int key = int(d.get("key",0))
         if key == k.KEY_ESCAPE { win.set_should_close(window,true) }
         elif key == k.KEY_X || key == k.KEY_EQUAL { zoom(0.8,vw*0.5,vh*0.5) }
         elif key == k.KEY_Z || key == k.KEY_MINUS { zoom(1.25,vw*0.5,vh*0.5) }
         elif key == k.KEY_R { reset() }
         elif key == k.KEY_TAB || key == k.KEY_F1 { panel = !panel }
         elif key == k.KEY_M { switch_set(0) }
         elif key == k.KEY_J { switch_set(1) }
         elif key >= k.KEY_1 && key <= k.KEY_6 { preset(key-k.KEY_1) }
         elif key == k.KEY_SPACE && mode == 1 { animate = !animate }
         elif key == k.KEY_ENTER { save_image() }
         elif key == k.KEY_LEFT { move_center(-span*0.08,0.0) invalidate() }
         elif key == k.KEY_RIGHT { move_center(span*0.08,0.0) invalidate() }
         elif key == k.KEY_UP { move_center(0.0,-span*0.08) invalidate() }
         elif key == k.KEY_DOWN { move_center(0.0,span*0.08) invalidate() }
      }
      if typ == k.EVENT_MOUSE_SCROLL || typ == k.EVENT_MOUSE_POS_CHANGED || typ == k.EVENT_MOUSE_BUTTON_PRESSED || typ == k.EVENT_MOUSE_BUTTON_RELEASED {
         def f64 x = float(d.get("x",mx/sx))*sx
         def f64 y = float(d.get("y",my/sy))*sy
         if typ == k.EVENT_MOUSE_SCROLL && x >= 0.0 && x < vw && y >= 0.0 && y < vh { zoom(exp(-float(clamp(float(d.get("dy",0.0)),-8.0,8.0))*0.22),x,y) }
         elif typ == k.EVENT_MOUSE_BUTTON_PRESSED && int(d.get("button",-1)) == 0 { dragging = x >= 0.0 && x < vw && y >= 0.0 && y < vh if dragging { zoom_delta = 0.0 } }
         elif typ == k.EVENT_MOUSE_BUTTON_RELEASED && int(d.get("button",-1)) == 0 { dragging = false }
         elif typ == k.EVENT_MOUSE_POS_CHANGED && dragging { move_center(-(x-mx)*span/vh,-(y-my)*span/vh) invalidate() }
         mx = x
         my = y
      }
      if typ == k.EVENT_FOCUS_OUT { dragging = false }
      count += 1
      if count >= 128 { break }
      e = win.check_event(window)
   }
}

fn main() int {
   gfx.set_backend_type(gfx.BACKEND_VK)
   def window = gfx.init_window(1000,700,"Sets | Fractal Explorer",0,false,false,1)
   if !window { eprint("sets: "+gfx.window_error()) return 1 }
   if gfx.get_active_backend() != int(gfx.BACKEND_VK) || !init_shader() {
      eprint("sets: Vulkan fractal shader unavailable")
      release_shader()
      gfx.close_window()
      return 1
   }
   win.set_cursor_mode(window,win.CURSOR_NORMAL)
   print(pipe64 ? "sets: Vulkan FP32/FP64" : "sets: Vulkan FP32")
   if common.env_truthy("NY_SETS_JULIA") { switch_set(1) }
   if common.env_truthy("NY_SETS_DEEP") && mode == 0 { cx = -0.743643887037151 cy = 0.131825904205330 span = 1e-9 invalidate() }
   gui.set_enabled(true)
   def list fonts = extend(["/usr/share/fonts/TTF/Inter-Regular.ttf","/usr/share/fonts/truetype/lato/Lato-Regular.ttf","/usr/share/fonts/TTF/DejaVuSans.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"],assets.UI_FONT_CANDIDATES)
   def int font = gfx.font_load_first(fonts,16)
   gui.set_fonts(font,font,font)
   mut int frames = 0
   def int max_frames = common.env_int_clamped("NY_SETS_FRAMES",0,0,10000)
   mut int failed = 0
   while !gfx.window_should_close(window) {
      def list fb = gfx.framebuffer_size_f64(1000.0,700.0)
      def f64 w = float(fb.get(0,1000.0))
      def f64 h = float(fb.get(1,700.0))
      if w < 1.0 || h < 1.0 { os.msleep(16) continue }
      def f64 next = panel && w >= 560.0 ? w-274.0 : w
      if next != vw || h != vh { vw = next vh = h invalidate() }
      input(window,w,h)
      enforce_precision()
      tick_zoom()
      if animate && mode == 1 {
         phase += float(clamp(gfx.get_frame_time(),0.0,0.05))*speed
         jr = -0.65+0.22*cos(phase) ji = 0.38+0.20*sin(phase)
         invalidate()
      }
      if !gfx.begin_frame_clear([0.035,0.045,0.075,1.0]) {
         failed += 1
         if failed >= 120 { eprint("sets: surface unavailable") break }
         os.msleep(16)
         continue
      }
      failed = 0
      gfx.set_ortho_2d(0.0,w,h,0.0)
      def bool capture = save_pending
      if !draw_gpu(w,capture) {
         eprint("sets: Vulkan shader draw failed")
         gfx.end_frame()
         failed = 120
         break
      }
      if capture { gfx.request_frame_capture() }
      gui.prepare_input(window,w,h)
      gui.begin_frame(0,font,w,h)
      if vw < w { controls(w,h) }
      gui.end_frame()
      if !gfx.end_frame() { eprint("sets: presentation failed") failed = 120 break }
      if capture { save_pending = false export_frame(int(vw),int(vh)) }
      frames += 1
      if max_frames > 0 && frames >= max_frames { break }
      os.msleep(8)
   }
   release_shader()
   if font > 0 { gfx.font_destroy(font) }
   gfx.close_window()
   if failed >= 120 { return 1 }
   0
}
