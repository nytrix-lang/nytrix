;; Keywords: ui backend
;; UI Backend Dispatcher for std.ui.window

module std.ui.backend (
   init, shutdown,
   available, create_native_window, poll_events, 
   swap_buffers, make_current, blit_buffer,
   get_backend_id, get_backend_name,
   x11_available, wayland_available, win32_available, cocoa_available, mock_available
)

use std.core *
use std.os *
use std.text *
use std.ui.backend.x11 as ui_x11
use std.ui.backend.win32 as ui_win32
use std.ui.backend.cocoa as ui_cocoa
use std.ui.backend.wayland as ui_wayland

mut _active_backend = 0 ;; 1=X11, 2=Win32, 3=Cocoa, 4=Wayland
mut _backend_name = "none"
mut _debug = -1

fn _env_enabled(name){
   "Returns true if the environment variable is set to a truthy string value."
   def v = env(name)
   if(!v){ return false }
   def n = lower(v)
   eq(n, "1") || eq(n, "true") || eq(n, "yes") || eq(n, "on") || eq(n, "y") || eq(n, "t")
}

fn _is_debug(){
   "Returns true if UI debugging is enabled."
   if(_debug == -1){
      _debug = _env_enabled("NY_UI_DEBUG") ? 1 : 0
   }
   _debug
}

fn _try_backend_by_name(name){
   "Attempts to activate an explicit backend by name. Returns true on success."
   if(!is_str(name)){ return false }
   def n = lower(name)
   if(eq(n, "x11") && ui_x11.available()){
      _active_backend = 1
      _backend_name = "x11"
      return true
   }
   if(eq(n, "wayland") && ui_wayland.available()){
      _active_backend = 4
      _backend_name = "wayland"
      return true
   }
   if((eq(n, "win32") || eq(n, "windows")) && ui_win32.available()){
      _active_backend = 2
      _backend_name = "win32"
      return true
   }
   if((eq(n, "cocoa") || eq(n, "macos")) && ui_cocoa.available()){
      _active_backend = 3
      _backend_name = "cocoa"
      return true
   }
   false
}

fn init(){
   "Initializes and selects the most appropriate native UI backend."
   if(_active_backend != 0){ return _active_backend }
   if(_is_debug()){ print("UI Backend: Initializing...") }
   if(_env_enabled("NY_HEADLESS")){
      ;; Headless now handled by falling back to NO display, but mock is removed.
   }
   def forced = env("NY_UI_BACKEND")
   if(forced && _try_backend_by_name(forced)){
      if(_is_debug()){ print(f"UI Backend: Forced '{_backend_name}' via NY_UI_BACKEND") }
      return _active_backend
   }
   def name = os()
   ;; Probe order based on OS
   if(eq(name, "windows")){
      if(ui_win32.available()){ _active_backend = 2 _backend_name = "win32" }
   } elif(eq(name, "macos")){
      if(ui_cocoa.available()){ _active_backend = 3 _backend_name = "cocoa" }
   }
   if(_active_backend == 0){
      ;; X11 is the most complete Linux backend currently (input + resize + Vulkan path).
      ;; Wayland remains experimental and can be preferred explicitly by env:
      ;; NY_UI_BACKEND=wayland or NY_UI_PREFER_WAYLAND=1.
      if(_env_enabled("NY_UI_PREFER_WAYLAND")){
         if(ui_wayland.available()){ _active_backend = 4 _backend_name = "wayland" }
         elif(ui_x11.available()){ _active_backend = 1 _backend_name = "x11" }
      } else {
         if(ui_x11.available()){ _active_backend = 1 _backend_name = "x11" }
         elif(ui_wayland.available()){ _active_backend = 4 _backend_name = "wayland" }
      }
   }
   if(_active_backend == 0){
      ;; Mock removed.
   }
   if(_is_debug()){ print(f"UI Backend: Selected '{_backend_name}' (ID: {_active_backend})") }
   _active_backend
}

fn shutdown(){
   "Shuts down the active UI backend."
   if(_is_debug()){ print(f"UI Backend: Shutting down '{_backend_name}'") }
   _active_backend = 0
   _backend_name = "none"
}

fn available(){
   "Returns true if any native UI backend can be successfully initialized."
   init() != 0
}

fn create_native_window(win){
   "Delegates native window creation to the active backend."
   def b = init()
   if(_is_debug()){ print(f"UI Backend: Creating window via '{_backend_name}'") }
   if(b == 1){ return ui_x11.create_native_window(win) }
   if(b == 2){ return ui_win32.create_native_window(win) }
   if(b == 3){ return ui_cocoa.create_native_window(win) }
   if(b == 4){ return ui_wayland.create_native_window(win) }
   false
}

fn poll_events(win){
   "Delegates event polling for a specific window to the active backend."
   def b = init()
   if(b == 1){ return ui_x11.poll_events(win) }
   if(b == 2){ return ui_win32.poll_events(win) }
   if(b == 3){ return ui_cocoa.poll_events(win) }
   if(b == 4){ return ui_wayland.poll_events(win) }
   0
}

fn swap_buffers(win){
   "Delegates buffer swapping (GPU) for a specific window to the active backend."
   def b = init()
   if(b == 1){ ui_x11.swap_buffers(win) }
   elif(b == 2){ ui_win32.swap_buffers(win) }
   elif(b == 3){ ui_cocoa.swap_buffers(win) }
   elif(b == 4){ ui_wayland.swap_buffers(win) }
}

fn make_current(win){
   "Delegates making a graphics context current to the active backend."
   def b = init()
   if(b == 1){ ui_x11.make_current(win) }
   elif(b == 2){ ui_win32.make_current(win) }
   elif(b == 3){ ui_cocoa.make_current(win) }
   elif(b == 4){ ui_wayland.make_current(win) }
}

fn blit_buffer(win, buf, w, h){
   "Delegates raw buffer blitting (CPU) to the active backend."
   def b = init()
   if(b == 1){ return ui_x11.blit_buffer(win, buf, w, h) }
   if(b == 2){ return ui_win32.blit_buffer(win, buf, w, h) }
   if(b == 3){ return ui_cocoa.blit_buffer(win, buf, w, h) }
   if(b == 4){ return ui_wayland.blit_buffer(win, buf, w, h) }
   0
}

fn get_backend_id(){
   "Returns the internal numeric ID of the active backend."
   _active_backend
}
fn get_backend_name(){
   "Returns the human-readable name of the active backend."
   _backend_name
}

fn x11_available(){
   "Checks if the X11 backend is available on this system."
   ui_x11.available()
}
fn wayland_available(){
   "Checks if the Wayland backend is available. Note: experimental."
   ui_wayland.available()
}
fn win32_available(){
   "Checks if the Win32 backend is available."
   ui_win32.available()
}
fn cocoa_available(){
   "Checks if the Cocoa (macOS) backend is available."
   ui_cocoa.available()
}

fn mock_available(){
   "Always returns false; mock backend is deprecated in favor of headless VK/GL."
   false
}

if(comptime{__main()}){
   use std.core.error *

   def b = init()
   assert(b != 0, "ui backend init")
   assert(get_backend_name() != "none", "ui backend name")
   print("✓ std.ui.backend tests passed")
}
