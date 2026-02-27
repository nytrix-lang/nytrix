;; Keywords: ui window x11
;; X11 Backend for std.ui.window

module std.ui.backend.x11 (
   available, create_native_window, poll_events, swap_buffers, make_current, blit_buffer, native_display
)
use std.core *
use std.os *
use std.os.ffi *
use std.ui.consts *
use std.ui.event as ev

mut _x11_handle = 0
mut _disp = 0

;; Native symbol pointers. Avoid bind() wrappers for X11/GLX pointer-heavy calls.
mut _XOpenDisplay_sym = 0
mut _XMapWindow_sym = 0
mut _XStoreName_sym = 0
mut _XSelectInput_sym = 0
mut _XNextEvent_sym = 0
mut _XPending_sym = 0
mut _XFlush_sym = 0
mut _XCreateWindow_sym = 0
mut _XCreateColormap_sym = 0
mut _XDefaultColormap_sym = 0
mut _XDefaultScreen_sym = 0
mut _XRootWindow_sym = 0
mut _XSetWMProtocols_sym = 0
mut _XLookupKeysym_sym = 0
mut _XInternAtom_sym = 0
mut _XFree_sym = 0
mut _XDefaultVisual_sym = 0
mut _XDefaultDepth_sym = 0
mut _XCreateImage_sym = 0
mut _XPutImage_sym = 0
mut _XDefaultGC_sym = 0
mut _XDestroyImage_sym = 0
mut _XSync_sym = 0

;; GL import removed

;; Constants
def KeyPressMask = 1
def KeyReleaseMask = 2
def ButtonPressMask = 4
def ButtonReleaseMask = 8
def EnterWindowMask = 16
def LeaveWindowMask = 32
def StructureNotifyMask = 131072
def PointerMotionMask = 64
def ExposureMask = 32768
def FocusChangeMask = 2097152

def ShiftMask = 1
def ControlMask = 4
def Mod1Mask = 8
def Mod4Mask = 64

mut _WM_DELETE_WINDOW = 0
mut _WM_PROTOCOLS = 0

mut _debug = -1

fn _is_debug(){
   "Auto-generated docstring: _is_debug."
   if(_debug == -1){
      def v = env("NY_UI_DEBUG")
      _debug = (v && (eq(v, "1") || eq(v, "true"))) ? 1 : 0
   }
   _debug
}

fn _x11_state_to_mod(state){
   "Auto-generated docstring: _x11_state_to_mod."
   mut mod = 0
   if((state & ShiftMask) != 0){ mod = mod | MOD_SHIFT }
   if((state & ControlMask) != 0){ mod = mod | MOD_CONTROL }
   if((state & Mod1Mask) != 0){ mod = mod | MOD_ALT }
   if((state & Mod4Mask) != 0){ mod = mod | MOD_SUPER }
   mod
}

fn available(){
   "Auto-generated docstring: available."
   if(_disp != 0){ return true }
   if(_x11_handle == 0){
      if(_is_debug()){ print("X11: Loading libX11...") }
      _x11_handle = dlopen_any("X11", RTLD_NOW())
      if(_x11_handle == 0){ 
         if(_is_debug()){ print("X11: Failed to load libX11.") }
         return false 
      }
      _XOpenDisplay_sym = dlsym(_x11_handle, "XOpenDisplay")
      _XMapWindow_sym = dlsym(_x11_handle, "XMapWindow")
      _XFlush_sym = dlsym(_x11_handle, "XFlush")
      _XStoreName_sym = dlsym(_x11_handle, "XStoreName")
      _XSelectInput_sym = dlsym(_x11_handle, "XSelectInput")
      _XNextEvent_sym = dlsym(_x11_handle, "XNextEvent")
      _XPending_sym = dlsym(_x11_handle, "XPending")
      _XCreateWindow_sym = dlsym(_x11_handle, "XCreateWindow")
      _XCreateColormap_sym = dlsym(_x11_handle, "XCreateColormap")
      _XDefaultColormap_sym = dlsym(_x11_handle, "XDefaultColormap")
      _XDefaultScreen_sym = dlsym(_x11_handle, "XDefaultScreen")
      _XRootWindow_sym = dlsym(_x11_handle, "XRootWindow")
      _XSetWMProtocols_sym = dlsym(_x11_handle, "XSetWMProtocols")
      _XLookupKeysym_sym = dlsym(_x11_handle, "XLookupKeysym")
      _XInternAtom_sym = dlsym(_x11_handle, "XInternAtom")
      _XFree_sym = dlsym(_x11_handle, "XFree")
      _XDefaultVisual_sym = dlsym(_x11_handle, "XDefaultVisual")
      _XDefaultDepth_sym = dlsym(_x11_handle, "XDefaultDepth")
      _XCreateImage_sym = dlsym(_x11_handle, "XCreateImage")
      _XPutImage_sym = dlsym(_x11_handle, "XPutImage")
      _XDefaultGC_sym = dlsym(_x11_handle, "XDefaultGC")
      _XDestroyImage_sym = dlsym(_x11_handle, "XDestroyImage")
      _XSync_sym = dlsym(_x11_handle, "XSync")
      if(_XOpenDisplay_sym == 0){ return false }
   }
   _disp = call1_i64(_XOpenDisplay_sym, 0)
   if(_disp == 0){
      if(_is_debug()){ print("X11: Could not open display.") }
      return false 
   }
   if(_is_debug()){ print(f"X11: Display opened at {cat(_disp)}") }
   if(_XInternAtom_sym != 0){
      ;; Atom ids are raw XIDs. Convert FFI-tagged returns to raw ids for struct writes/comparisons.
      _WM_DELETE_WINDOW = to_int(call3(_XInternAtom_sym, _disp, "WM_DELETE_WINDOW", 0))
      _WM_PROTOCOLS = to_int(call3(_XInternAtom_sym, _disp, "WM_PROTOCOLS", 0))
   }
   true
}

fn create_native_window(win){
   "Auto-generated docstring: create_native_window."
   if(!available()){ return false }
   def scr = call1_i64(_XDefaultScreen_sym, _disp)
   def root = call2(_XRootWindow_sym, _disp, scr)
   def flags = get(win, 7, 0)
   def x = get(win, 3, 0)
   def y = get(win, 4, 0)
   def w = get(win, 5, 800)
   def h = get(win, 6, 600)
   mut window_ptr = 0
   mut visual = call2(_XDefaultVisual_sym, _disp, scr)
   mut depth = call2(_XDefaultDepth_sym, _disp, scr)
   mut cmap = 0
   def input_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                    EnterWindowMask | LeaveWindowMask | FocusChangeMask |
                    StructureNotifyMask | PointerMotionMask | ExposureMask
   if(cmap == 0 && _XDefaultColormap_sym != 0){
      ;; Use default colormap if no GL visual or error
      cmap = call2(_XDefaultColormap_sym, _disp, scr)
   }
   ;; valuemask: CWBackPixel (2) | CWEventMask (2048) = 2050
   mut xwa = malloc(160)
   memset(xwa, 0, 160)
   store64(xwa, 0, 8)                  ;; background_pixel = Black
   store64(xwa, to_int(input_mask), 72) 
   window_ptr = call12(_XCreateWindow_sym, _disp, root, x, y, w, h, 0, depth, 1, visual, 2050, xwa)
   free(xwa)
   if(window_ptr == 0){ return false }
   if(_XSetWMProtocols_sym != 0 && _WM_DELETE_WINDOW != 0){
      mut protocols_ptr = malloc(8)
      store64(protocols_ptr, _WM_DELETE_WINDOW)
      call4(_XSetWMProtocols_sym, _disp, window_ptr, protocols_ptr, 1)
      free(protocols_ptr)
   }
   if(_XStoreName_sym != 0){ call3(_XStoreName_sym, _disp, window_ptr, get(win, 2, "Window")) }
   if(_XMapWindow_sym != 0 && (flags & WINDOW_HIDE) == 0){ 
      call2(_XMapWindow_sym, _disp, window_ptr) 
   }
   set_idx(win, 22, window_ptr)
   if(_is_debug()){ print(f"X11: Window {cat(window_ptr)} initialized") }
   if(_XFlush_sym != 0){ call1(_XFlush_sym, _disp) }
   return true
}

fn native_display(){
   "Auto-generated docstring: native_display."
   _disp
}

fn poll_events(win){
   "Auto-generated docstring: poll_events."
   if(_disp == 0 || _XPending_sym == 0 || _XNextEvent_sym == 0){ return 0 }
   while(to_int(call1_i64(_XPending_sym, _disp)) > 0){
      mut ev_buf = malloc(256) 
      call2(_XNextEvent_sym, _disp, ev_buf)
      def ev_type = load32(ev_buf, 0)
      def q = get(win, 10, 0) ;; _W_EVENTS
      if(ev_type == 2 || ev_type == 3){
         mut keysym = 0
         if(_XLookupKeysym_sym != 0){ keysym = to_int(call2(_XLookupKeysym_sym, ev_buf, 0)) }
         def x_keycode = load32(ev_buf, 84)
         if(_is_debug()){ print(cat("X11 Input: keysym=", hex(keysym), " x_keycode=", str(x_keycode))) }
         ;; Normalization fallback
         if(keysym == 0 && x_keycode == 9){ keysym = 0xFF1B }
         def keycode = keysym
         def state = _x11_state_to_mod(load32(ev_buf, 80))
         def x = load32(ev_buf, 64)
         def y = load32(ev_buf, 68)
         mut ev_kind = EVENT_KEY_PRESSED
         if(ev_type == 3){ ev_kind = EVENT_KEY_RELEASED }
         mut data = dict(4)
         data = dict_set(data, "key", keycode)
         data = dict_set(data, "mod", state)
         data = dict_set(data, "x", x)
         data = dict_set(data, "y", y)
         set_idx(win, 10, ev.queue_push(q, ev.make_event(ev_kind, win, get(win, 1), data)))
      } elif(ev_type == 4 || ev_type == 5){
         def button = load32(ev_buf, 84)
         def state = _x11_state_to_mod(load32(ev_buf, 80))
         def x = load32(ev_buf, 64)
         def y = load32(ev_buf, 68)
         mut ev_kind = EVENT_MOUSE_BUTTON_PRESSED
         if(ev_type == 5){ ev_kind = EVENT_MOUSE_BUTTON_RELEASED }
         mut data = dict(8)
         data = dict_set(data, "button", button)
         data = dict_set(data, "mod", state)
         data = dict_set(data, "x", x)
         data = dict_set(data, "y", y)
         set_idx(win, 10, ev.queue_push(q, ev.make_event(ev_kind, win, get(win, 1), data)))
      } elif(ev_type == 6){
         def state = _x11_state_to_mod(load32(ev_buf, 80))
         def x = load32(ev_buf, 64)
         def y = load32(ev_buf, 68)
         mut data = dict(8)
         data = dict_set(data, "mod", state)
         data = dict_set(data, "x", x)
         data = dict_set(data, "y", y)
         set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_MOUSE_POS_CHANGED, win, get(win, 1), data)))
      } elif(ev_type == 7){
         set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_MOUSE_ENTER, win, get(win, 1), 0)))
      } elif(ev_type == 8){
         set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_MOUSE_LEAVE, win, get(win, 1), 0)))
      } elif(ev_type == 9){
         set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_FOCUS_IN, win, get(win, 1), 0)))
      } elif(ev_type == 10){
         set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_FOCUS_OUT, win, get(win, 1), 0)))
      } elif(ev_type == 12){
         set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_WINDOW_REFRESH, win, get(win, 1), 0)))
      } elif(ev_type == 22){
         def x = load32(ev_buf, 48)
         def y = load32(ev_buf, 52)
         def w = load32(ev_buf, 56)
         def h = load32(ev_buf, 60)
         if(_is_debug()){ print(cat("X11 ConfigureNotify: x=", str(x), " y=", str(y), " w=", str(w), " h=", str(h))) }
         def old_x = get(win, 3, x)
         def old_y = get(win, 4, y)
         def old_w = get(win, 5, w)
         def old_h = get(win, 6, h)
         ;; Keep window metadata coherent for software rendering and layout.
         set_idx(win, 3, x)
         set_idx(win, 4, y)
         set_idx(win, 5, w)
         set_idx(win, 6, h)
         if(x != old_x || y != old_y){
            mut mv = dict(4)
            mv = dict_set(mv, "x", x)
            mv = dict_set(mv, "y", y)
            set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_WINDOW_MOVED, win, get(win, 1), mv)))
         }
         if(w != old_w || h != old_h){
            mut r = dict(8)
            r = dict_set(r, "x", x)
            r = dict_set(r, "y", y)
            r = dict_set(r, "w", w)
            r = dict_set(r, "h", h)
            set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_WINDOW_RESIZED, win, get(win, 1), r)))
         }
      } elif(ev_type == 33){
         def msg_type = load64(ev_buf, 40)
         def data0 = load64(ev_buf, 56)
         if(msg_type == _WM_PROTOCOLS && data0 == _WM_DELETE_WINDOW){
            set_idx(win, 8, true) 
            set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_QUIT, win, get(win, 1), 0)))
         }
      }
      free(ev_buf)
   }
   if(_XFlush_sym != 0){ call1(_XFlush_sym, _disp) }
   0
}

fn swap_buffers(win){
   "Auto-generated docstring: swap_buffers."
   _touch(win)
   if(_XFlush_sym != 0){ call1(_XFlush_sym, _disp) }
}
fn make_current(win){
   "Auto-generated docstring: make_current."
   _touch(win)
}

fn blit_buffer(win, buf, w, h){
   "Auto-generated docstring: blit_buffer."
   if(_disp == 0 || _XCreateImage_sym == 0){ return 0 }
   def scr = call1_i64(_XDefaultScreen_sym, _disp)
   def visual = call2(_XDefaultVisual_sym, _disp, scr)
   def dpy_depth = call2(_XDefaultDepth_sym, _disp, scr)
   def window_ptr = get(win, 22)
   ;; Create a GC specifically for this window if we don't have one
   mut gc = get(win, 23, 0)
   if(to_int(gc) == 0){
      def XCreateGC_sym = dlsym(_x11_handle, "XCreateGC")
      if(XCreateGC_sym != 0){ gc = call4(XCreateGC_sym, _disp, window_ptr, 0, 0) }
      else { gc = call2(_XDefaultGC_sym, _disp, scr) }
      set_idx(win, 23, gc)
   }
   ;; format=2 (ZPixmap), offset=0, bitmap_pad=32, bytes_per_line=0 (auto)
   def img = call10(_XCreateImage_sym, _disp, visual, dpy_depth, 2, 0, buf, w, h, 32, 0)
   if(to_int(img) == 0){ return 0 }
   call10_void(_XPutImage_sym, _disp, window_ptr, gc, img, 0, 0, 0, 0, w, h)
   ;; Avoid XDestroyImage if it's crashing. Use XFree on the struct only.
   if(_XFree_sym != 0){ call1_void(_XFree_sym, img) }
   ;; Ensure the server processes the request
   if(_XSync_sym != 0){ call2_void(_XSync_sym, _disp, 0) }
   1
}
mut i = 0
