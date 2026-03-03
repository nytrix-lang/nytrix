;; Keywords: ui glfw
;; GLFW backend — static extern fn linking via #link.

module std.ui.glfw (
   init, terminate,
   create_window, destroy_window, should_close, set_should_close,
   set_title, get_size, get_framebuffer_size, set_size,
   poll_events, swap_buffers, swap_interval,
   get_key, get_mouse_button, get_cursor_pos,
   set_key_callback, set_mouse_button_callback,
   set_scroll_callback, set_cursor_pos_callback,
    set_window_size_callback, set_close_callback,
    set_input_mode,
   vulkan_supported, required_extensions, create_surface,
   apply_hints,
   GLFW_NO_API, GLFW_CLIENT_API, GLFW_RESIZABLE, GLFW_DECORATED,
   GLFW_VISIBLE, GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FOCUSED,
   GLFW_FLOATING, GLFW_MAXIMIZED, GLFW_FOCUS_ON_SHOW,
   GLFW_PRESS, GLFW_RELEASE, GLFW_REPEAT,
   KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_SPACE,
   KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
   KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
   KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
   MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE,
   MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER,
   get_backend_name
)

use std.core *
use std.core.mem *
use std.ui.consts *

mut _title_buf = 0
mut _title_cap = 0
fn _title_cstr(s){
    "Internal: Keeps a reusable NUL-terminated window-title buffer."
    s = cstr(s, "nytrix")
    def n = str_len(s)
    if(_title_cap < n + 1){
        def newp = realloc(_title_buf, n + 1)
        if(!newp){ return 0 }
        _title_buf = newp
        _title_cap = n + 1
    }
    strcpy(_title_buf, s)
    _title_buf
}

if(comptime{ __os_name() == "linux" }){
    #link "glfw"
} else if(comptime{ __os_name() == "macos" }){
    #link "glfw"
} else if(comptime{ __os_name() == "windows" }){
    #link "glfw3"
}

extern fn glfwInit(): i32 as "glfwInit"
extern fn glfwTerminate(): i32 as "glfwTerminate"
extern fn glfwCreateWindow(w: i32, h: i32, title: ptr, monitor: ptr, share: ptr): ptr as "glfwCreateWindow"
extern fn glfwDestroyWindow(win: ptr): i32 as "glfwDestroyWindow"
extern fn glfwWindowShouldClose(win: ptr): i32 as "glfwWindowShouldClose"
extern fn glfwSetWindowShouldClose(win: ptr, v: i32): i32 as "glfwSetWindowShouldClose"
extern fn glfwSetWindowTitle(win: ptr, title: ptr): i32 as "glfwSetWindowTitle"
extern fn glfwPollEvents(): i32 as "glfwPollEvents"
extern fn glfwSwapBuffers(win: ptr): i32 as "glfwSwapBuffers"
extern fn glfwSwapInterval(n: i32): i32 as "glfwSwapInterval"
extern fn glfwWindowHint(hint: i32, value: i32): i32 as "glfwWindowHint"
extern fn glfwGetWindowSize(win: ptr, w: ptr, h: ptr): i32 as "glfwGetWindowSize"
extern fn glfwGetFramebufferSize(win: ptr, w: ptr, h: ptr): i32 as "glfwGetFramebufferSize"
extern fn glfwSetWindowSize(win: ptr, w: i32, h: i32): i32 as "glfwSetWindowSize"
extern fn glfwGetKey(win: ptr, key: i32): i32 as "glfwGetKey"
extern fn glfwGetMouseButton(win: ptr, btn: i32): i32 as "glfwGetMouseButton"
extern fn glfwGetCursorPos(win: ptr, xpos: ptr, ypos: ptr): i32 as "glfwGetCursorPos"
extern fn glfwSetCursorPos(win: ptr, xpos: f64, ypos: f64) as "glfwSetCursorPos"
extern fn glfwSetKeyCallback(win: ptr, cb: ptr): ptr as "glfwSetKeyCallback"
extern fn glfwSetMouseButtonCallback(win: ptr, cb: ptr): ptr as "glfwSetMouseButtonCallback"
extern fn glfwSetScrollCallback(win: ptr, cb: ptr): ptr as "glfwSetScrollCallback"
extern fn glfwSetCursorPosCallback(win: ptr, cb: ptr): ptr as "glfwSetCursorPosCallback"
extern fn glfwSetWindowSizeCallback(win: ptr, cb: ptr): ptr as "glfwSetWindowSizeCallback"
extern fn glfwSetCloseCallback(win: ptr, cb: ptr): ptr as "glfwSetWindowCloseCallback"
extern fn glfwVulkanSupported(): i32 as "glfwVulkanSupported"
extern fn glfwGetRequiredInstanceExtensions(count: ptr): ptr as "glfwGetRequiredInstanceExtensions"

extern fn glfwCreateWindowSurface(instance: ptr, win: ptr, alloc: ptr, surface: ptr): i32 as "glfwCreateWindowSurface"

;; Window hint constants

def GLFW_CLIENT_API             = 0x00022001
def GLFW_NO_API                 = 0
def GLFW_OPENGL_API             = 0x00030001
def GLFW_RESIZABLE              = 0x00020003
def GLFW_VISIBLE                = 0x00020004
def GLFW_DECORATED              = 0x00020005
def GLFW_FOCUSED                = 0x00020001
def GLFW_AUTO_ICONIFY           = 0x00020006
def GLFW_FLOATING               = 0x00020007
def GLFW_MAXIMIZED              = 0x00020008
def GLFW_CENTER_CURSOR          = 0x00020009
def GLFW_TRANSPARENT_FRAMEBUFFER= 0x0002000A
def GLFW_FOCUS_ON_SHOW          = 0x0002000C
def GLFW_SCALE_TO_MONITOR       = 0x0002000D

def GLFW_PRESS   = 1
def GLFW_RELEASE = 0
def GLFW_REPEAT  = 2

def KEY_ESCAPE    = 256
def KEY_ENTER     = 257
def KEY_TAB       = 258
def KEY_BACKSPACE = 259
def KEY_RIGHT     = 262
def KEY_LEFT      = 263
def KEY_DOWN      = 264
def KEY_UP        = 265
def KEY_F1  = 290
def KEY_F2  = 291
def KEY_F3  = 292
def KEY_F4  = 293
def KEY_F5  = 294
def KEY_F6  = 295
def KEY_F7  = 296
def KEY_F8  = 297
def KEY_F9  = 298
def KEY_F10 = 299
def KEY_F11 = 300
def KEY_F12 = 301
def KEY_SPACE = 32

def MOUSE_BUTTON_LEFT   = 0
def MOUSE_BUTTON_RIGHT  = 1
def MOUSE_BUTTON_MIDDLE = 2

def MOD_SHIFT   = 0x0001
def MOD_CONTROL = 0x0002
def MOD_ALT     = 0x0004
def MOD_SUPER   = 0x0008

;; Lifecycle

mut _ready = false

fn init() {
   "Initializes GLFW once and returns whether setup succeeded."
    if(_ready){ return true }
    if(glfwInit() == 0){ return false }
    _ready = true
    true
}

fn terminate() {
   "Terminates GLFW when it has been initialized."
    if(_ready){ glfwTerminate() _ready = false }
}

;; Window management

fn apply_hints(flags) {
   "Applies Nytrix window flags to GLFW window hints."
    if((flags & WINDOW_TRANSPARENT) != 0){ glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, 1) }
    if((flags & WINDOW_NO_BORDER)   != 0){ glfwWindowHint(GLFW_DECORATED, 0) }
    if((flags & WINDOW_NO_RESIZE)   != 0){ glfwWindowHint(GLFW_RESIZABLE, 0) }
    if((flags & WINDOW_FLOATING)    != 0){ glfwWindowHint(GLFW_FLOATING, 1) }
    if((flags & WINDOW_MAXIMIZE)    != 0){ glfwWindowHint(GLFW_MAXIMIZED, 1) }
    if((flags & WINDOW_HIDE)        != 0){ glfwWindowHint(GLFW_VISIBLE, 0) }
}

fn create_window(title, w, h, flags=0) {
    "Creates a GLFW-backed window with Nytrix window flags applied."
    init()
    if(!title){ title = "nytrix" }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)
    apply_hints(flags)
    glfwCreateWindow(w, h, _title_cstr(title), 0, 0)
}

fn destroy_window(win){
   "Destroys a GLFW window when `win` is non-zero."
   if(win){ glfwDestroyWindow(win) }
}
fn should_close(win){
   "Returns whether GLFW has marked `win` for closing."
   glfwWindowShouldClose(win) != 0
}
fn set_should_close(win, v=1){
   "Marks GLFW window `win` as closing when `v` is truthy."
   glfwSetWindowShouldClose(win, v)
}
fn set_title(win, title){
   "Updates the title of GLFW window `win`."
   glfwSetWindowTitle(win, _title_cstr(title))
}
fn poll_events(){
   "Polls GLFW window and input events when GLFW is ready."
   if(_ready){ glfwPollEvents() }
}
fn swap_buffers(win){
   "Swaps front and back buffers for window `win`."
   glfwSwapBuffers(win)
}
fn swap_interval(n){
   "Sets the GLFW swap interval."
   glfwSwapInterval(n)
}

mut _size_wp = 0
mut _size_hp = 0

fn _ensure_size_bufs(){
   "Internal: allocates reusable buffers for GLFW size queries."
    if(!_size_wp){ _size_wp = malloc(4) _size_hp = malloc(4) }
}

fn _size_pair(win, framebuffer=false) {
   "Internal: reads a GLFW window or framebuffer size pair."
    _ensure_size_bufs()
    if(framebuffer){
        glfwGetFramebufferSize(win, _size_wp, _size_hp)
    } else {
        glfwGetWindowSize(win, _size_wp, _size_hp)
    }
    [load32(_size_wp, 0), load32(_size_hp, 0)]
}

fn get_size(win) {
   "Returns the current GLFW window size as `[width, height]`."
    _size_pair(win)
}

fn get_framebuffer_size(win) {
   "Returns the current framebuffer size as `[width, height]`."
    _size_pair(win, true)
}

fn set_size(win, w, h){
   "Sets the GLFW window size to `w` by `h`."
   glfwSetWindowSize(win, w, h)
}

;; Input

mut _cursor_xp = 0
mut _cursor_yp = 0

fn get_key(win, key){
   "Returns the GLFW key state for `key` on window `win`."
   glfwGetKey(win, key)
}
fn get_mouse_button(win, btn){
   "Returns the GLFW mouse-button state for `btn` on window `win`."
   glfwGetMouseButton(win, btn)
}

fn get_cursor_pos(win) {
   "Returns the cursor position for window `win` as `[x, y]`."
    if(!_cursor_xp){ _cursor_xp = malloc(8) _cursor_yp = malloc(8) }
    glfwGetCursorPos(win, _cursor_xp, _cursor_yp)
    [load64_f64(_cursor_xp, 0), load64_f64(_cursor_yp, 0)]
}

fn set_cursor_pos(win, x, y){
   "Moves the cursor for window `win` to `(x, y)`."
    glfwSetCursorPos(win, float(x), float(y))
}

;; Callbacks

fn set_key_callback(win, cb){
   "Registers a GLFW key callback for window `win`."
   glfwSetKeyCallback(win, cb)
}
fn set_mouse_button_callback(win, cb){
   "Registers a GLFW mouse-button callback for window `win`."
   glfwSetMouseButtonCallback(win, cb)
}
fn set_scroll_callback(win, cb){
   "Registers a GLFW scroll callback for window `win`."
   glfwSetScrollCallback(win, cb)
}
fn set_cursor_pos_callback(win, cb){
   "Registers a GLFW cursor-position callback for window `win`."
   glfwSetCursorPosCallback(win, cb)
}
fn set_window_size_callback(win, cb){
   "Registers a GLFW window-size callback for window `win`."
   glfwSetWindowSizeCallback(win, cb)
}
fn set_close_callback(win, cb){
   "Registers a GLFW close callback for window `win`."
   glfwSetCloseCallback(win, cb)
}

;; Vulkan integration

fn vulkan_supported(){
   "Returns whether GLFW reports Vulkan support on this host."
   glfwVulkanSupported() != 0
}

fn required_extensions() {
   "Returns `[count, ptr]` for the GLFW-required Vulkan instance extensions."
    _ensure_size_bufs()
    def exts = glfwGetRequiredInstanceExtensions(_size_wp)
    [load32(_size_wp, 0), exts]
}

fn create_surface(instance, win, allocator, surface) {
   "Creates a Vulkan surface for GLFW window `win`."
    glfwCreateWindowSurface(instance, win, allocator, surface)
}

fn get_backend_name() { "glfw" }

fn set_input_mode(win, mode, value){
   "Sets a GLFW input mode for window `win`."
    glfwSetInputMode(win, mode, value)
}
extern fn glfwSetInputMode(win: ptr, mode: i32, value: i32): i32 as "glfwSetInputMode"
