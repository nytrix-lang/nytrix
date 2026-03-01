;; Keywords: ui window win32
;; Windows Backend for std.ui.window (Preliminary)

module std.ui.backend.win32 (
   available, create_native_window, poll_events, swap_buffers, make_current, blit_buffer
)
use std.core *
use std.os.ffi *
use std.ui.consts *
use std.ui.event as ev

mut _user32 = 0
mut _kernel32 = 0
mut _gdi32 = 0
mut _DefWindowProcA = 0
mut _RegisterClassExA = 0
mut _CreateWindowExA = 0
mut _ShowWindow = 0
mut _GetMessageA = 0
mut _TranslateMessage = 0
mut _DispatchMessageA = 0
mut _PeekMessageA = 0
mut _GetModuleHandleA = 0
mut _GetKeyState = 0
mut _GetDC = 0
mut _ReleaseDC = 0
mut _SwapBuffers = 0

fn _touch(...args){
   "Internal helper to mark arguments as used."
   len(args)
}

fn _vk_is_down(vk){
   "Internal helper to check if a virtual key is currently pressed using GetKeyState."
   if(_GetKeyState == 0){ return false }
   (to_int(call1_i64(_GetKeyState, vk)) & 0x8000) != 0
}

fn _current_mods(){
   "Determines the current active modifier keys (Shift, Ctrl, Alt, Win)."
   mut mod = 0
   if(_vk_is_down(16)){ mod = mod | MOD_SHIFT }   ;; VK_SHIFT
   if(_vk_is_down(17)){ mod = mod | MOD_CONTROL } ;; VK_CONTROL
   if(_vk_is_down(18)){ mod = mod | MOD_ALT }     ;; VK_MENU (Alt)
   if(_vk_is_down(0x5B) || _vk_is_down(0x5C)){ mod = mod | MOD_SUPER } ;; VK_LWIN/VK_RWIN
   mod
}

fn _lo16(v){
   "Returns the low 16 bits of a value."
   v & 0xFFFF
}

fn _hi16(v){
   "Returns the high 16 bits of a value."
   (v >> 16) & 0xFFFF
}

fn _sign16(v){
   "Interprets a 16-bit value as a signed integer."
   if(v >= 32768){ return v - 65536 }
   v
}

fn available(){
   "Returns true if the Win32 user32, kernel32, and gdi32 libraries are available."
   if(_user32 != 0){ return true }
   _user32 = dlopen_any("user32", RTLD_NOW())
   _kernel32 = dlopen_any("kernel32", RTLD_NOW())
   _gdi32 = dlopen_any("gdi32", RTLD_NOW())
   if(_user32 == 0 || _kernel32 == 0 || _gdi32 == 0){ return false }
   _DefWindowProcA = bind(_user32, "DefWindowProcA")
   _RegisterClassExA = bind(_user32, "RegisterClassExA")
   _CreateWindowExA = bind(_user32, "CreateWindowExA")
   _ShowWindow = bind(_user32, "ShowWindow")
   _GetMessageA = bind(_user32, "GetMessageA")
   _TranslateMessage = bind(_user32, "TranslateMessage")
   _DispatchMessageA = bind(_user32, "DispatchMessageA")
   _PeekMessageA = bind(_user32, "PeekMessageA")
   _GetModuleHandleA = bind(_kernel32, "GetModuleHandleA")
   _GetKeyState = bind(_user32, "GetKeyState")
   _GetDC = bind(_user32, "GetDC")
   _ReleaseDC = bind(_user32, "ReleaseDC")
   _SwapBuffers = bind(_gdi32, "SwapBuffers")
   if(_CreateWindowExA == 0){ return false }
   return true
}

fn create_native_window(win){
   "Creates a native Win32 window for the given Nytrix window object."
   if(!available()){ return false }
   def h_instance = call1_i64(_GetModuleHandleA, 0)
   ;; WNDCLASSEX format (80 bytes)
   mut wcx = malloc(80)
   memset(wcx, 0, 80)
   store32(wcx, 80, 0) ;; cbSize
   store32(wcx, 3, 4)  ;; style CS_HREDRAW | CS_VREDRAW
   store64(wcx, _DefWindowProcA, 8) ;; lpfnWndProc
   store64(wcx, h_instance, 24) ;; hInstance
   def class_name = "NytrixUIWindow"
   store64(wcx, class_name, 64) ;; lpszClassName
   call1(_RegisterClassExA, wcx)
   free(wcx)
   def x = get(win, 3, 0)
   def y = get(win, 4, 0)
   def w = get(win, 5, 800)
   def h = get(win, 6, 600)
   def title = get(win, 2, "Window")
   ;; WS_OVERLAPPEDWINDOW = 0x00cf0000
   def window_ptr = call12(_CreateWindowExA, 0, class_name, title, 13565952, x, y, w, h, 0, 0, h_instance, 0)
   if(window_ptr == 0){ return false }
   ;; SW_SHOW = 5
   call2(_ShowWindow, window_ptr, 5)
   set_idx(win, 22, window_ptr)
   return true
}

fn poll_events(win){
   "Polls Win32 messages and converts them to Nytrix UI events."
   if(_user32 == 0 || _PeekMessageA == 0){ return 0 }
   mut msg = malloc(48)
   ;; PM_REMOVE = 1
   while(call5(_PeekMessageA, msg, 0, 0, 0, 1) > 0){
      call1(_TranslateMessage, msg)
      call1(_DispatchMessageA, msg)
      def message = load32(msg, 8)
      ;; MSG layout (win64): message@8, wParam@16, lParam@24
      def wparam = load32(msg, 16)
      def lparam = load32(msg, 24)
      ;; WM_CLOSE=0x0010, WM_QUIT=0x0012, WM_KEYDOWN/UP, WM_SYSKEYDOWN/UP
      if(message == 16 || message == 18){
         set_idx(win, 8, true)
         def q = get(win, 10, 0)
         set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_QUIT, win, get(win, 1), 0)))
      } elif(message == 3){ ;; WM_MOVE
         def x = _sign16(_lo16(lparam))
         def y = _sign16(_hi16(lparam))
         set_idx(win, 3, x)
         set_idx(win, 4, y)
         mut data = dict(4)
         data = dict_set(data, "x", x)
         data = dict_set(data, "y", y)
         def q = get(win, 10, 0)
         set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_WINDOW_MOVED, win, get(win, 1), data)))
      } elif(message == 5){ ;; WM_SIZE
         mut w = _lo16(lparam)
         mut h = _hi16(lparam)
         if(w < 1){ w = 1 }
         if(h < 1){ h = 1 }
         set_idx(win, 5, w)
         set_idx(win, 6, h)
         mut data = dict(8)
         data = dict_set(data, "x", get(win, 3, 0))
         data = dict_set(data, "y", get(win, 4, 0))
         data = dict_set(data, "w", w)
         data = dict_set(data, "h", h)
         def q = get(win, 10, 0)
         set_idx(win, 10, ev.queue_push(q, ev.make_event(EVENT_WINDOW_RESIZED, win, get(win, 1), data)))
      } elif(message == 256 || message == 257 || message == 260 || message == 261){
         mut typ = EVENT_KEY_PRESSED
         if(message == 257 || message == 261){ typ = EVENT_KEY_RELEASED }
         def repeat = ((lparam >> 30) & 1) != 0
         mut data = dict(8)
         data = dict_set(data, "key", wparam)
         data = dict_set(data, "mod", _current_mods())
         data = dict_set(data, "repeat", repeat)
         def q = get(win, 10, 0)
         set_idx(win, 10, ev.queue_push(q, ev.make_event(typ, win, get(win, 1), data)))
      }
   }
   free(msg)
   0
}

fn swap_buffers(win){
   "Win32 buffer swap implementation using WGL/GDI."
   def hwnd = get(win, 22)
   if(hwnd == 0 || _GetDC == 0 || _SwapBuffers == 0){ return 0 }

   def hdc = call1(_GetDC, hwnd)
   if(hdc == 0){ return 0 }

   call1(_SwapBuffers, hdc)

   if(_ReleaseDC != 0){
      call2(_ReleaseDC, hwnd, hdc)
   }
   1
}

fn make_current(win){
   "Makes the window the current rendering context."
   _touch(win)
}

fn blit_buffer(win, buf, w, h){
   "Blits a raw buffer to the Win32 window (placeholder)."
   _touch(win, buf, w, h)
   0
}
