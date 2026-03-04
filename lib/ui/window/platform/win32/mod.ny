;; Keywords: ui window win32
;; Low-level Win32 entry helpers for the in-progress native backend.

module std.ui.window.platform.win32 (
   available, get_backend_name,
   module_handle, primary_screen_size,
   init_dpi_awareness, init_timer, get_timer_value, get_timer_frequency,
   utf16_from_utf8, utf8_from_utf16,
   translate_vk_key, translate_message, translate_messages,
   get_key_state, get_mouse_button_state, get_key_name,
   get_window_style, get_window_ex_style,
   ensure_window_class, create_basic_window, destroy_basic_window,
   show_window, hide_window, set_title, set_window_icon,
   get_pos, set_pos, get_size, get_framebuffer_size, set_size,
   iconify_window, restore_window, maximize_window,
   focus_window, request_window_attention,
   get_window_attrib,
   is_window_focused, is_window_iconified, is_window_visible, is_window_maximized, is_window_hovered,
   get_window_opacity, set_window_opacity,
   set_window_resizable, set_window_decorated, set_window_floating,
   set_clipboard, get_clipboard,
   get_cursor_pos, set_cursor_pos,
   create_cursor, create_standard_cursor, destroy_cursor, set_cursor,
   get_monitors, get_primary_monitor,
   get_monitor_pos, get_monitor_workarea,
   get_monitor_physical_size, get_monitor_content_scale, get_monitor_name,
   get_video_mode, get_video_modes, get_gamma_ramp, set_gamma_ramp,
   get_window_monitor, set_window_monitor,
   set_input_mode,
   set_window_size_limits,
   poll_messages, pump_window_messages, poll_window_events,
   vulkan_supported, vulkan_required_extensions, create_surface,
   set_video_mode, restore_video_mode,
   get_win32_window, get_win32_monitor, get_win32_adapter
)

use std.core *
use std.ui.window.platform.api as backend_api
use std.ui.window.consts *
use std.ui.window.event as ui_event

def SM_CXSCREEN = 0
def SM_CYSCREEN = 1
def SM_CXICON = 11
def SM_CYICON = 12
def SM_CXSMICON = 49
def SM_CYSMICON = 50

def CP_UTF8 = 65001
def CF_UNICODETEXT = 13
def GMEM_MOVEABLE = 0x0002

def WS_OVERLAPPED = 0x00000000
def WS_POPUP = 0x80000000
def WS_CAPTION = 0x00c00000
def WS_SYSMENU = 0x00080000
def WS_THICKFRAME = 0x00040000
def WS_MINIMIZEBOX = 0x00020000
def WS_MAXIMIZEBOX = 0x00010000
def WS_CLIPCHILDREN = 0x02000000
def WS_CLIPSIBLINGS = 0x04000000
def WS_MAXIMIZE = 0x01000000

def WS_EX_APPWINDOW = 0x00040000
def WS_EX_LAYERED = 0x00080000
def WS_EX_TOPMOST = 0x00000008
def WS_EX_TRANSPARENT = 0x00000020

def CS_VREDRAW = 0x0001
def CS_HREDRAW = 0x0002
def CS_OWNDC = 0x0020

def CW_USEDEFAULT = -2147483648

def SW_HIDE = 0
def SW_SHOWNORMAL = 1
def SW_SHOWMINIMIZED = 2
def SW_SHOWMAXIMIZED = 3
def SW_SHOWNOACTIVATE = 4
def SW_SHOW = 5
def SW_MINIMIZE = 6
def SW_SHOWMINNOACTIVE = 7
def SW_SHOWNA = 8
def SW_RESTORE = 9

def PM_NOREMOVE = 0x0000
def PM_REMOVE = 0x0001
def MSGFLT_ALLOW = 1

def SWP_NOSIZE = 0x0001
def SWP_NOMOVE = 0x0002
def SWP_NOZORDER = 0x0004
def SWP_NOACTIVATE = 0x0010
def SWP_FRAMECHANGED = 0x0020
def SWP_SHOWWINDOW = 0x0040
def SWP_NOCOPYBITS = 0x0100
def SWP_NOOWNERZORDER = 0x0200

def GWL_STYLE = -16
def GWL_EXSTYLE = -20
def GCLP_HICON = -14
def GCLP_HICONSM = -34

def LWA_ALPHA = 0x00000002
def BI_BITFIELDS = 3
def DIB_RGB_COLORS = 0
def DWM_BB_ENABLE = 0x00000001
def DWM_BB_BLURREGION = 0x00000002

def IMAGE_ICON = 1
def IMAGE_CURSOR = 2
def LR_DEFAULTSIZE = 0x00000040
def LR_SHARED = 0x00008000
def IDC_ARROW = 32512
def IDC_IBEAM = 32513
def IDC_CROSS = 32515
def IDC_SIZEALL = 32646
def IDC_SIZENWSE = 32642
def IDC_SIZENESW = 32643
def IDC_SIZEWE = 32644
def IDC_SIZENS = 32645
def IDC_HAND = 32649
def IDC_NO = 32648
def IDI_APPLICATION = 32512

def VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR = 1000009000
def ICON_SMALL = 0
def ICON_BIG = 1

def WM_CLOSE = 0x0010
def WM_QUIT = 0x0012
def WM_COPYGLOBALDATA = 0x0049
def WM_COPYDATA = 0x004A
def WM_SETICON = 0x0080
def WM_DROPFILES = 0x0233
def WM_INPUT = 0x00FF
def WM_KEYDOWN = 0x0100
def WM_KEYUP = 0x0101
def WM_CHAR = 0x0102
def WM_SYSKEYDOWN = 0x0104
def WM_SYSKEYUP = 0x0105
def WM_SYSCHAR = 0x0106
def WM_IME_STARTCOMPOSITION = 0x010d
def WM_IME_ENDCOMPOSITION = 0x010e
def WM_IME_COMPOSITION = 0x010f
def WM_IME_SETCONTEXT = 0x0281
def WM_IME_NOTIFY = 0x0282
def WM_GETMINMAXINFO = 0x0024
def WM_INPUTLANGCHANGE = 0x0051
def WM_UNICHAR = 0x0109
def WM_MOUSEMOVE = 0x0200
def WM_LBUTTONDOWN = 0x0201
def WM_LBUTTONUP = 0x0202
def WM_RBUTTONDOWN = 0x0204
def WM_RBUTTONUP = 0x0205
def WM_MBUTTONDOWN = 0x0207
def WM_MBUTTONUP = 0x0208
def WM_MOUSEWHEEL = 0x020A
def WM_XBUTTONDOWN = 0x020B
def WM_XBUTTONUP = 0x020C
def WM_MOUSEHWHEEL = 0x020E
def WM_MOUSELEAVE = 0x02A3
def WM_SETFOCUS = 0x0007
def WM_KILLFOCUS = 0x0008
def WM_SIZE = 0x0005
def WM_MOVE = 0x0003
def WM_PAINT = 0x000F
def WM_ENTERSIZEMOVE = 0x0231
def WM_EXITSIZEMOVE = 0x0232
def WM_DPICHANGED = 0x02E0
def WM_DISPLAYCHANGE = 0x007E

def SIZE_RESTORED = 0
def SIZE_MINIMIZED = 1
def SIZE_MAXIMIZED = 2

def KF_EXTENDED = 0x0100
def KF_REPEAT = 0x4000
def KF_UP = 0x8000
def WHEEL_DELTA = 120
def RID_INPUT = 0x10000003
def RIDEV_REMOVE = 0x00000001
def RIM_TYPEMOUSE = 0
def MOUSE_MOVE_ABSOLUTE = 0x0001
def MOUSE_VIRTUAL_DESKTOP = 0x0002

def VK_SHIFT = 0x10
def VK_CONTROL = 0x11
def VK_MENU = 0x12
def VK_LSHIFT = 0xA0
def VK_RSHIFT = 0xA1
def VK_LCONTROL = 0xA2
def VK_RCONTROL = 0xA3
def VK_LMENU = 0xA4
def VK_RMENU = 0xA5
def VK_LWIN = 0x5B
def VK_RWIN = 0x5C
def VK_APPS = 0x5D
def VK_PROCESSKEY = 0xE5
def VK_ESCAPE = 0x1B
def VK_RETURN = 0x0D
def VK_TAB = 0x09
def VK_BACK = 0x08
def VK_INSERT = 0x2D
def VK_DELETE = 0x2E
def VK_RIGHT = 0x27
def VK_LEFT = 0x25
def VK_DOWN = 0x28
def VK_UP = 0x26
def VK_PRIOR = 0x21
def VK_NEXT = 0x22
def VK_HOME = 0x24
def VK_END = 0x23
def VK_CAPITAL = 0x14
def VK_SCROLL = 0x91
def VK_NUMLOCK = 0x90
def VK_SNAPSHOT = 0x2C
def VK_PAUSE = 0x13
def VK_SPACE = 0x20
def VK_F1 = 0x70
def VK_F2 = 0x71
def VK_F3 = 0x72
def VK_F4 = 0x73
def VK_F5 = 0x74
def VK_F6 = 0x75
def VK_F7 = 0x76
def VK_F8 = 0x77
def VK_F9 = 0x78
def VK_F10 = 0x79
def VK_F11 = 0x7A
def VK_F12 = 0x7B
def VK_F13 = 0x7C
def VK_F14 = 0x7D
def VK_F15 = 0x7E
def VK_F16 = 0x7F
def VK_F17 = 0x80
def VK_F18 = 0x81
def VK_F19 = 0x82
def VK_F20 = 0x83
def VK_F21 = 0x84
def VK_F22 = 0x85
def VK_F23 = 0x86
def VK_F24 = 0x87
def VK_NUMPAD0 = 0x60
def VK_NUMPAD1 = 0x61
def VK_NUMPAD2 = 0x62
def VK_NUMPAD3 = 0x63
def VK_NUMPAD4 = 0x64
def VK_NUMPAD5 = 0x65
def VK_NUMPAD6 = 0x66
def VK_NUMPAD7 = 0x67
def VK_NUMPAD8 = 0x68
def VK_NUMPAD9 = 0x69
def VK_MULTIPLY = 0x6A
def VK_ADD = 0x6B
def VK_SEPARATOR = 0x6C
def VK_SUBTRACT = 0x6D
def VK_DECIMAL = 0x6E
def VK_DIVIDE = 0x6F
def XBUTTON1 = 0x0001
def XBUTTON2 = 0x0002
def UNICODE_NOCHAR = 0xffff
def MAPVK_VK_TO_VSC = 0
def GCS_COMPSTR = 0x0008
def GCS_RESULTSTR = 0x0800

def SM_XVIRTUALSCREEN = 76
def SM_YVIRTUALSCREEN = 77
def SM_CXVIRTUALSCREEN = 78
def SM_CYVIRTUALSCREEN = 79
def LOGPIXELSX = 88
def LOGPIXELSY = 90
def HORZSIZE = 4
def VERTSIZE = 6

def HWND_TOP = 0
def HWND_TOPMOST = -1
def HWND_NOTOPMOST = -2

def DISPLAY_DEVICE_ACTIVE = 0x00000001
def DISPLAY_DEVICE_PRIMARY_DEVICE = 0x00000004
def MONITORINFOF_PRIMARY = 0x00000001
def MONITOR_DEFAULTTONEAREST = 0x00000002
def ENUM_CURRENT_SETTINGS = 0xffffffff
def USER_DEFAULT_SCREEN_DPI = 96
def MDT_EFFECTIVE_DPI = 0
def DM_BITSPERPEL = 0x00040000
def DM_PELSWIDTH = 0x00080000
def DM_PELSHEIGHT = 0x00100000
def DM_DISPLAYFREQUENCY = 0x00400000
def CDS_FULLSCREEN = 0x00000004
def DISP_CHANGE_SUCCESSFUL = 0
def PROCESS_PER_MONITOR_DPI_AWARE = 2
def DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4

mut _registered_classes = dict(4)
mut _windows = dict()
mut _timer_frequency = 0
mut _dpi_awareness_done = false
mut _known_monitor_names = []
mut _monitors_initialized = false

if(comptime{ __os_name() == "windows" }){
   #include <windows.h>
   #include <dwmapi.h>
   #include <shellapi.h>
   #include <shcore.h>
   #include <imm.h>
   #include <vulkan/vulkan_win32.h>
}

fn available(){
   "Returns true when the process is running on Win32."
   comptime{ __os_name() == "windows" }
}

fn get_backend_name(){
   "Identifies this backend entry module."
   "win32"
}

fn init_dpi_awareness(){
   "Direct Ny port of GLFW's Win32 process DPI-awareness bootstrap."
   if(!available()){ return false }
   if(_dpi_awareness_done){ return true }
   if(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0){
      _dpi_awareness_done = true
      return true
   }
   if(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE) == 0){
      _dpi_awareness_done = true
      return true
   }
   if(SetProcessDPIAware() != 0){
      _dpi_awareness_done = true
      return true
   }
   false
}

fn init_timer(){
   "Initializes the high-resolution Win32 timer frequency."
   if(!available()){ return false }
   if(_timer_frequency > 0){ return true }
   def value = calloc(1, 8)
   if(!value){ return false }
   def ok = QueryPerformanceFrequency(value) != 0
   if(ok){ _timer_frequency = load64_h(value, 0) }
   free(value)
   ok
}

fn get_timer_value(){
   "Returns the current Win32 performance counter value."
   if(!available()){ return 0 }
   def value = calloc(1, 8)
   if(!value){ return 0 }
   mut out = 0
   if(QueryPerformanceCounter(value) != 0){ out = load64_h(value, 0) }
   free(value)
   out
}

fn get_timer_frequency(){
   "Returns the Win32 performance counter frequency."
   if(!available()){ return 0 }
   if(_timer_frequency <= 0){ init_timer() }
   _timer_frequency
}

fn module_handle(module_name=0){
   "Returns the Win32 HMODULE for the current process or the named module."
   if(!available()){ return 0 }
   if(module_name && is_str(module_name) && len(module_name) > 0){
      return GetModuleHandleA(cstr(module_name))
   }
   GetModuleHandleA(0)
}

fn _default_window_proc(hwnd: ptr, msg: u32, wparam: u64, lparam: i64): i64 {
   "Default Win32 window procedure."
   if(!available()){ return 0 }
   if(msg == WM_GETMINMAXINFO){
      def win = dict_get(_windows, hwnd, 0)
      if(win){
         def min_w = dict_get(win, "min_w", -1)
         def min_h = dict_get(win, "min_h", -1)
         def max_w = dict_get(win, "max_w", -1)
         def max_h = dict_get(win, "max_h", -1)
         if(min_w >= 0){ store32(lparam, min_w, 24) } ;; ptMinTrackSize.x
         if(min_h >= 0){ store32(lparam, min_h, 28) } ;; ptMinTrackSize.y
         if(max_w >= 0){ store32(lparam, max_w, 16) } ;; ptMaxTrackSize.x
         if(max_h >= 0){ store32(lparam, max_h, 20) } ;; ptMaxTrackSize.y
         return 0
      }
   }
   DefWindowProcW(hwnd, msg, wparam, lparam)
}

fn utf16_from_utf8(s){
   "Returns a heap-allocated UTF-16LE string buffer for a UTF-8 Ny string."
   if(!available()){ return 0 }
   if(!is_str(s)){ s = to_str(s) }
   def count = MultiByteToWideChar(CP_UTF8, 0, cstr(s), -1, 0, 0)
   if(count <= 0){ return 0 }
   def out = calloc(count, 2)
   if(!out){ return 0 }
   if(MultiByteToWideChar(CP_UTF8, 0, cstr(s), -1, out, count) == 0){
      free(out)
      return 0
   }
   out
}

fn utf8_from_utf16(wide){
   "Returns a UTF-8 Ny string converted from a NUL-terminated UTF-16 buffer."
   if(!available() || !wide){ return "" }
   def size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, 0, 0, 0, 0)
   if(size <= 0){ return "" }
   def out = calloc(size, 1)
   if(!out){ return "" }
   if(WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, size, 0, 0) == 0){
      free(out)
      return ""
   }
   def s = cstr_to_str(out)
   free(out)
   s
}

fn get_window_style(flags=0, monitor=false){
   "Direct Ny port of GLFW's Win32 window style selection."
   mut style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN
   if(monitor || band(flags, WINDOW_FULLSCREEN)){
      style = bor(style, WS_POPUP)
   } else {
      style = bor(style, WS_SYSMENU | WS_MINIMIZEBOX)
      if(!band(flags, WINDOW_NO_BORDER)){
         style = bor(style, WS_CAPTION)
         if(!band(flags, WINDOW_NO_RESIZE)){
         style = bor(style, WS_MAXIMIZEBOX | WS_THICKFRAME)
         }
      } else {
         style = bor(style, WS_POPUP)
      }
   }
   if(band(flags, WINDOW_MAXIMIZE)){ style = bor(style, WS_MAXIMIZE) }
   style
}

fn get_window_ex_style(flags=0, monitor=false){
   "Direct Ny port of GLFW's Win32 extended window style selection."
   mut style = WS_EX_APPWINDOW
   if(monitor || band(flags, WINDOW_FULLSCREEN) || band(flags, WINDOW_FLOATING)){
      style = bor(style, WS_EX_TOPMOST)
   }
   style
}

fn _rect_width(rect){
   load32(rect, 8) - load32(rect, 0)
}

fn _rect_height(rect){
   load32(rect, 12) - load32(rect, 4)
}

fn _window_dpi(hwnd=0){
   if(!available()){ return USER_DEFAULT_SCREEN_DPI }
   if(hwnd){
      def dpi = int(GetDpiForWindow(hwnd))
      if(dpi > 0){ return dpi }
   }
   USER_DEFAULT_SCREEN_DPI
}

fn _adjust_window_rect(rect, style, ex_style, hwnd=0){
   if(!rect){ return false }
   def dpi = _window_dpi(hwnd)
   if(AdjustWindowRectExForDpi(rect, style, FALSE, ex_style, dpi) != 0){ return true }
   AdjustWindowRectEx(rect, style, FALSE, ex_style) != 0
}

fn _system_metric(index, dpi=0){
   if(!available()){ return 0 }
   def actual_dpi = dpi > 0 ? dpi : USER_DEFAULT_SCREEN_DPI
   def value = GetSystemMetricsForDpi(index, actual_dpi)
   if(value > 0){ return value }
   GetSystemMetrics(index)
}

fn _update_framebuffer_transparency(win){
   if(!available() || !win || !is_dict(win) || !dict_get(win, "transparent", false)){ return win }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return win }

   def composition = calloc(1, 4)
   if(!composition){ return win }
   def enabled = DwmIsCompositionEnabled(composition) == 0 && load32(composition, 0) != 0
   free(composition)
   if(!enabled){ return win }

   def color = calloc(1, 4)
   def opaque = calloc(1, 4)
   def got_color = color && opaque && DwmGetColorizationColor(color, opaque) == 0
   def use_blur_region = !got_color || load32(opaque, 0) == 0

   def blur = calloc(1, 24)
   def region = use_blur_region ? CreateRectRgn(0, 0, -1, -1) : 0
   if(blur){
      if(region && use_blur_region){
         store32(blur, DWM_BB_ENABLE | DWM_BB_BLURREGION, 0)
         store32(blur, 1, 4)
         store64_h(blur, region, 8)
      } else {
         store32(blur, DWM_BB_ENABLE, 0)
      }
      DwmEnableBlurBehindWindow(hwnd, blur)
      free(blur)
   }

   if(region){ DeleteObject(region) }
   if(color){ free(color) }
   if(opaque){ free(opaque) }
   win
}

fn _make_int_resource(v){
   "Converts a small integer resource id to a Win32 MAKEINTRESOURCE pointer."
   v
}

fn _signed16(v){
   def x = int(v) & 0xffff
   if(x >= 0x8000){ x - 0x10000 } else { x }
}

fn _signed32(v){
   def x = int(v) & 0xffffffff
   if(x >= 0x80000000){ x - 0x100000000 } else { x }
}

fn _icon_image_width(image){
   if(!is_dict(image)){ return 0 }
   int(dict_get(image, "width", dict_get(image, "w", 0)))
}

fn _icon_image_height(image){
   if(!is_dict(image)){ return 0 }
   int(dict_get(image, "height", dict_get(image, "h", 0)))
}

fn _icon_image_pixels(image){
   if(!is_dict(image)){ return 0 }
   dict_get(image, "pixels_ptr",
      dict_get(image, "pixels",
      dict_get(image, "data", 0)))
}

fn _icon_pixel_source_len(pixels){
   if(is_str(pixels) || is_bytes(pixels) || is_list(pixels) || is_tuple(pixels)){ return len(pixels) }
   if(is_ptr(pixels)){ return -1 }
   0
}

fn _icon_pixel_byte(pixels, index){
   if(is_ptr(pixels) || is_str(pixels) || is_bytes(pixels)){ return load8(pixels, index) }
   if(is_list(pixels) || is_tuple(pixels)){ return get(pixels, index, 0) }
   0
}

fn _choose_icon_image(images, wanted_w, wanted_h){
   if(is_dict(images)){ return images }
   if(!is_list(images) && !is_tuple(images)){ return false }
   mut best = false
   mut best_score = 1 << 30
   mut i = 0
   while(i < len(images)){
      def image = get(images, i, 0)
      def iw = _icon_image_width(image)
      def ih = _icon_image_height(image)
      if(iw > 0 && ih > 0){
         def score = abs(iw - wanted_w) * 1000 + abs(ih - wanted_h)
         if(!best || score < best_score){
         best = image
         best_score = score
         }
      }
      i += 1
   }
   best
}

fn _wide_field_utf8(base, offset){
   if(!base){ return "" }
   utf8_from_utf16(base + offset)
}

fn _alloc_display_device(){
   def dd = calloc(1, 840)
   if(!dd){ return 0 }
   store32(dd, 840, 0)
   dd
}

fn _alloc_devmode(){
   def dm = calloc(1, 220)
   if(!dm){ return 0 }
   store16(dm, 220, 68)
   dm
}

fn _split_bpp(bpp){
   mut depth = int(bpp)
   if(depth == 32){ depth = 24 }
   mut red = int(depth / 3)
   mut green = red
   mut blue = red
   def delta = depth - red * 3
   if(delta >= 1){ green += 1 }
   if(delta == 2){ red += 1 }
   [red, green, blue]
}

fn _devmode_to_vidmode(dm){
   if(!dm){ return false }
   def rgb = _split_bpp(load32(dm, 168))
   mut mode = dict()
   mode = dict_set(mode, "width", load32(dm, 172))
   mode = dict_set(mode, "height", load32(dm, 176))
   mode = dict_set(mode, "red_bits", get(rgb, 0, 8))
   mode = dict_set(mode, "green_bits", get(rgb, 1, 8))
   mode = dict_set(mode, "blue_bits", get(rgb, 2, 8))
   mode = dict_set(mode, "refresh_rate", load32(dm, 184))
   mode
}

fn _vidmode_bpp(mode){
   int(dict_get(mode, "red_bits", 8)) +
   int(dict_get(mode, "green_bits", 8)) +
   int(dict_get(mode, "blue_bits", 8))
}

fn _choose_best_video_mode(monitor, width, height, refresh_rate=0){
   def modes = get_video_modes(monitor)
   if(len(modes) == 0){ return get_video_mode(monitor) }
   mut best = false
   mut best_score = 1 << 30
   mut i = 0
   while(i < len(modes)){
      def mode = get(modes, i, false)
      if(mode && is_dict(mode)){
         def mw = dict_get(mode, "width", 0)
         def mh = dict_get(mode, "height", 0)
         def mr = dict_get(mode, "refresh_rate", 0)
         mut score = abs(mw - width) * 10000 + abs(mh - height) * 100
         if(refresh_rate > 0){ score += abs(mr - refresh_rate) }
         if(!best || score < best_score){
         best = mode
         best_score = score
         }
      }
      i += 1
   }
   best
}

fn _set_video_mode(monitor, width, height, refresh_rate=0){
   if(!available() || !monitor || !is_dict(monitor)){ return false }
   def adapter = dict_get(monitor, "adapter_name", "")
   if(len(adapter) == 0){ return false }
   def current = get_video_mode(monitor)
   def best = _choose_best_video_mode(monitor, width, height, refresh_rate)
   if(!best || !is_dict(best)){ return false }
   if(current &&
      dict_get(current, "width", 0) == dict_get(best, "width", 0) &&
      dict_get(current, "height", 0) == dict_get(best, "height", 0) &&
      dict_get(current, "refresh_rate", 0) == dict_get(best, "refresh_rate", 0) &&
      _vidmode_bpp(current) == _vidmode_bpp(best)){
      return false
   }

   def wide = utf16_from_utf8(adapter)
   def dm = _alloc_devmode()
   if(!wide || !dm){
      if(wide){ free(wide) }
      if(dm){ free(dm) }
      return false
   }
   store32(dm, DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY, 72)
   store32(dm, dict_get(best, "width", width), 172)
   store32(dm, dict_get(best, "height", height), 176)
   mut bpp = _vidmode_bpp(best)
   if(bpp < 15 || bpp >= 24){ bpp = 32 }
   store32(dm, bpp, 168)
   store32(dm, dict_get(best, "refresh_rate", refresh_rate), 184)
   def result = ChangeDisplaySettingsExW(wide, dm, 0, CDS_FULLSCREEN, 0)
   free(wide)
   free(dm)
   result == DISP_CHANGE_SUCCESSFUL
}

fn _restore_video_mode(adapter_name){
   if(!available() || len(adapter_name) == 0){ return false }
   def wide = utf16_from_utf8(adapter_name)
   if(!wide){ return false }
   def result = ChangeDisplaySettingsExW(wide, 0, 0, CDS_FULLSCREEN, 0)
   free(wide)
   result == DISP_CHANGE_SUCCESSFUL
}

fn _driver_display_wide(){
   utf16_from_utf8("DISPLAY")
}

fn _physical_size_from_adapter(adapter_wide, width, height){
   if(!adapter_wide){ return [0, 0] }
   def driver = _driver_display_wide()
   def dc = CreateDCW(driver, adapter_wide, 0, 0)
   if(driver){ free(driver) }
   if(!dc){ return [0, 0] }
   mut width_mm = GetDeviceCaps(dc, HORZSIZE)
   mut height_mm = GetDeviceCaps(dc, VERTSIZE)
   if(width_mm <= 0 || height_mm <= 0){
      def log_x = max(1, GetDeviceCaps(dc, LOGPIXELSX))
      def log_y = max(1, GetDeviceCaps(dc, LOGPIXELSY))
      width_mm = int((float(width) * 25.4 / float(log_x)) + 0.5)
      height_mm = int((float(height) * 25.4 / float(log_y)) + 0.5)
   }
   DeleteDC(dc)
   [width_mm, height_mm]
}

fn _content_scale_from_monitor(handle, adapter_wide){
   mut xdpi = 0
   mut ydpi = 0
   def xp = calloc(1, 4)
   def yp = calloc(1, 4)
   if(handle && xp && yp && GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, xp, yp) == 0){
      xdpi = load32(xp, 0)
      ydpi = load32(yp, 0)
   }
   if(xp){ free(xp) }
   if(yp){ free(yp) }
   if(xdpi <= 0 || ydpi <= 0){
      def driver = _driver_display_wide()
      def dc = CreateDCW(driver, adapter_wide, 0, 0)
      if(driver){ free(driver) }
      if(dc){
         xdpi = GetDeviceCaps(dc, LOGPIXELSX)
         ydpi = GetDeviceCaps(dc, LOGPIXELSY)
         DeleteDC(dc)
      }
   }
   if(xdpi <= 0){ xdpi = USER_DEFAULT_SCREEN_DPI }
   if(ydpi <= 0){ ydpi = USER_DEFAULT_SCREEN_DPI }
   [float(xdpi) / float(USER_DEFAULT_SCREEN_DPI), float(ydpi) / float(USER_DEFAULT_SCREEN_DPI)]
}

fn _create_monitor_from_devices(adapter, display=0){
   if(!available() || !adapter){ return false }
   def adapter_name = _wide_field_utf8(adapter, 4)
   if(len(adapter_name) == 0){ return false }
   def adapter_wide = utf16_from_utf8(adapter_name)
   if(!adapter_wide){ return false }

   def dm = _alloc_devmode()
   if(!dm){
      free(adapter_wide)
      return false
   }
   if(EnumDisplaySettingsW(adapter_wide, ENUM_CURRENT_SETTINGS, dm) == 0){
      free(dm)
      free(adapter_wide)
      return false
   }

   def x = _signed32(load32(dm, 76))
   def y = _signed32(load32(dm, 80))
   def width = load32(dm, 172)
   def height = load32(dm, 176)
   def rect = calloc(1, 16)
   if(!rect){
      free(dm)
      free(adapter_wide)
      return false
   }
   store32(rect, x, 0)
   store32(rect, y, 4)
   store32(rect, x + width, 8)
   store32(rect, y + height, 12)
   def handle = MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST)
   free(rect)

   def mi = calloc(1, 104)
   if(!mi){
      free(dm)
      free(adapter_wide)
      return false
   }
   store32(mi, 104, 0)
   mut work_x = x
   mut work_y = y
   mut work_w = width
   mut work_h = height
   mut primary = band(load32(adapter, 324), DISPLAY_DEVICE_PRIMARY_DEVICE)
   if(handle && GetMonitorInfoW(handle, mi) != 0){
      work_x = _signed32(load32(mi, 20))
      work_y = _signed32(load32(mi, 24))
      work_w = _signed32(load32(mi, 28)) - work_x
      work_h = _signed32(load32(mi, 32)) - work_y
      primary = primary || band(load32(mi, 36), MONITORINFOF_PRIMARY)
   }
   free(mi)

   def mm = _physical_size_from_adapter(adapter_wide, width, height)
   def scale = _content_scale_from_monitor(handle, adapter_wide)
   def mode = _devmode_to_vidmode(dm)
   def name = display ? _wide_field_utf8(display, 68) : _wide_field_utf8(adapter, 68)
   def display_name = display ? _wide_field_utf8(display, 4) : ""
   free(dm)
   free(adapter_wide)

   mut monitor = dict()
   monitor = dict_set(monitor, "handle", handle)
   monitor = dict_set(monitor, "name", name)
   monitor = dict_set(monitor, "adapter_name", adapter_name)
   monitor = dict_set(monitor, "display_name", display_name)
   monitor = dict_set(monitor, "x", x)
   monitor = dict_set(monitor, "y", y)
   monitor = dict_set(monitor, "width", width)
   monitor = dict_set(monitor, "height", height)
   monitor = dict_set(monitor, "work_x", work_x)
   monitor = dict_set(monitor, "work_y", work_y)
   monitor = dict_set(monitor, "work_w", work_w)
   monitor = dict_set(monitor, "work_h", work_h)
   monitor = dict_set(monitor, "width_mm", get(mm, 0, 0))
   monitor = dict_set(monitor, "height_mm", get(mm, 1, 0))
   monitor = dict_set(monitor, "scale_x", get(scale, 0, 1.0))
   monitor = dict_set(monitor, "scale_y", get(scale, 1, 1.0))
   monitor = dict_set(monitor, "red_bits", dict_get(mode, "red_bits", 8))
   monitor = dict_set(monitor, "green_bits", dict_get(mode, "green_bits", 8))
   monitor = dict_set(monitor, "blue_bits", dict_get(mode, "blue_bits", 8))
   monitor = dict_set(monitor, "refresh_rate", dict_get(mode, "refresh_rate", 60))
   monitor = dict_set(monitor, "primary", !!primary)
   monitor
}

fn _copy_gamma_channel(source, dest, offset, size){
   "Copies raw gamma values into a Win32-compatible WORD buffer."
   if(!source || !dest || size <= 0){ return false }
   mut i = 0
   while(i < size){
      store16(dest, _clamp_gamma_word(get(source, i, 0)), offset + (i * 2))
      i += 1
   }
   true
}

fn _gamma_ramp_from_buffer(values, size=256){
   "Converts raw Win32 gamma buffer back into a Nytrix dictionary ramp."
   if(!values || size <= 0){ return false }
   mut red = []
   mut green = []
   mut blue = []
   mut i = 0
   while(i < size){
      red = append(red, int(load16(values, i * 2)))
      green = append(green, int(load16(values, 512 + (i * 2))))
      blue = append(blue, int(load16(values, 1024 + (i * 2))))
      i += 1
   }
   mut ramp = dict()
   ramp = dict_set(ramp, "size", size)
   ramp = dict_set(ramp, "red", red)
   ramp = dict_set(ramp, "green", green)
   ramp = dict_set(ramp, "blue", blue)
   ramp
}

fn _copy_gamma_channel(src, dst, off, size){
   "Internal helper to copy gamma values into Win32 WORD array."
   if(!is_list(src) || len(src) < size){ return false }
   mut i = 0
   while(i < size){
      store16(dst, _clamp_gamma_word(get(src, i, 0)), off + i * 2)
      i += 1
   }
   true
}

fn _get_x_lparam(lp){
   _signed16(lp)
}

fn _get_y_lparam(lp){
   _signed16(bshr(lp, 16))
}

fn _hiword(v){
   int(band(bshr(v, 16), 0xffff))
}

fn _loword(v){
   int(band(v, 0xffff))
}

fn _key_state_bit(win, key){
   if(!win || !is_dict(win)){ return false }
   dict_get(dict_get(win, "key_states", dict()), key, false)
}

fn _cursor_handle(cursor){
   if(!cursor){ return 0 }
   if(is_dict(cursor)){ return dict_get(cursor, "handle", 0) }
   cursor
}

fn _show_cursor(visible){
   if(!available()){ return false }
   mut n = 0
   if(visible){
      while(n < 32 && ShowCursor(1) < 0){ n += 1 }
   } else {
      while(n < 32 && ShowCursor(0) >= 0){ n += 1 }
   }
   true
}

fn _client_clip_rect(hwnd){
   if(!available() || !hwnd){ return 0 }
   def rect = calloc(1, 16)
   if(!rect){ return 0 }
   if(GetClientRect(hwnd, rect) == 0){
      free(rect)
      return 0
   }
   if(ClientToScreen(hwnd, rect) == 0 || ClientToScreen(hwnd, rect + 8) == 0){
      free(rect)
      return 0
   }
   rect
}

fn _capture_cursor(hwnd){
   if(!available() || !hwnd){ return false }
   def rect = _client_clip_rect(hwnd)
   if(!rect){ return false }
   def ok = ClipCursor(rect) != 0
   free(rect)
   ok
}

fn _release_cursor(){
   if(!available()){ return false }
   ClipCursor(0) != 0
}

fn _effective_flags(win){
   if(!win || !is_dict(win)){ return 0 }
   mut flags = int(dict_get(win, "flags", 0))
   if(dict_get(win, "decorated", !band(flags, WINDOW_NO_BORDER))){
      flags = band(flags, bnot(WINDOW_NO_BORDER))
   } else {
      flags = bor(flags, WINDOW_NO_BORDER)
   }
   if(dict_get(win, "resizable", !band(flags, WINDOW_NO_RESIZE))){
      flags = band(flags, bnot(WINDOW_NO_RESIZE))
   } else {
      flags = bor(flags, WINDOW_NO_RESIZE)
   }
   if(dict_get(win, "floating", band(flags, WINDOW_FLOATING))){
      flags = bor(flags, WINDOW_FLOATING)
   } else {
      flags = band(flags, bnot(WINDOW_FLOATING))
   }
   if(dict_get(win, "fullscreen", band(flags, WINDOW_FULLSCREEN))){
      flags = bor(flags, WINDOW_FULLSCREEN)
   } else {
      flags = band(flags, bnot(WINDOW_FULLSCREEN))
   }
   flags
}

fn _update_window_styles(win){
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return win }
   def flags = _effective_flags(win)
   def style = get_window_style(flags, dict_get(win, "fullscreen", false))
   def ex_style = get_window_ex_style(flags, dict_get(win, "fullscreen", false))
   SetWindowLongW(hwnd, GWL_STYLE, style)
   SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style)
   def sz = get_size(win)
   def rect = calloc(1, 16)
   if(rect){
      store32(rect, 0, 0)
      store32(rect, 0, 4)
      store32(rect, int(get(sz, 0, 0)), 8)
      store32(rect, int(get(sz, 1, 0)), 12)
      _adjust_window_rect(rect, style, ex_style, hwnd)
      SetWindowPos(hwnd, HWND_TOP,
         dict_get(win, "x", CW_USEDEFAULT), dict_get(win, "y", CW_USEDEFAULT),
         _rect_width(rect), _rect_height(rect),
         SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER)
      free(rect)
   }
   win
}

fn _clipboard_hwnd(win){
   if(is_dict(win)){ return dict_get(win, "handle", 0) }
   win
}

fn _open_clipboard_with_retry(hwnd, tries=3){
   mut n = 0
   while(n < tries){
      if(OpenClipboard(hwnd) != 0){ return true }
      Sleep(1)
      n += 1
   }
   false
}

fn _create_rgba_cursor_handle(image, xhot=0, yhot=0, icon=false){
   if(!available() || !image || !is_dict(image)){ return 0 }
   def width = _icon_image_width(image)
   def height = _icon_image_height(image)
   def pixels = _icon_image_pixels(image)
   def bytes = width * height * 4
   def have = _icon_pixel_source_len(pixels)
   if(width <= 0 || height <= 0 || !pixels || (have >= 0 && have < bytes)){ return 0 }

   def bi = calloc(1, 124)
   def bits = calloc(1, 8)
   if(!bi || !bits){
      if(bi){ free(bi) }
      if(bits){ free(bits) }
      return 0
   }
   store32(bi, 124, 0)
   store32(bi, width, 4)
   store32(bi, -height, 8)
   store16(bi, 1, 12)
   store16(bi, 32, 14)
   store32(bi, BI_BITFIELDS, 16)
   store32(bi, 0x00ff0000, 40)
   store32(bi, 0x0000ff00, 44)
   store32(bi, 0x000000ff, 48)
   store32(bi, 0xff000000, 52)

   def dc = GetDC(0)
   def color = CreateDIBSection(dc, bi, DIB_RGB_COLORS, bits, 0, 0)
   if(dc){ ReleaseDC(0, dc) }
   free(bi)
   if(!color){
      free(bits)
      return 0
   }

   def mask = CreateBitmap(width, height, 1, 1, 0)
   if(!mask){
      DeleteObject(color)
      free(bits)
      return 0
   }

   def target = load64_h(bits, 0)
   free(bits)
   if(!target){
      DeleteObject(color)
      DeleteObject(mask)
      return 0
   }

   mut i = 0
   while(i < width * height){
      def base = i * 4
      store8(target, _icon_pixel_byte(pixels, base + 2), base + 0)
      store8(target, _icon_pixel_byte(pixels, base + 1), base + 1)
      store8(target, _icon_pixel_byte(pixels, base + 0), base + 2)
      store8(target, _icon_pixel_byte(pixels, base + 3), base + 3)
      i += 1
   }

   def ii = calloc(1, 32)
   if(!ii){
      DeleteObject(color)
      DeleteObject(mask)
      return 0
   }
   store32(ii, icon ? 1 : 0, 0)
   store32(ii, xhot, 4)
   store32(ii, yhot, 8)
   store64_h(ii, mask, 16)
   store64_h(ii, color, 24)
   def handle = CreateIconIndirect(ii)
   free(ii)
   DeleteObject(color)
   DeleteObject(mask)
   handle
}

fn _enable_raw_mouse_motion(hwnd){
   if(!available() || !hwnd){ return false }
   def rid = calloc(1, 16)
   if(!rid){ return false }
   store16(rid, 0x01, 0)
   store16(rid, 0x02, 2)
   store32(rid, 0, 4)
   store64_h(rid, hwnd, 8)
   def ok = RegisterRawInputDevices(rid, 1, 16) != 0
   free(rid)
   ok
}

fn _disable_raw_mouse_motion(){
   if(!available()){ return false }
   def rid = calloc(1, 16)
   if(!rid){ return false }
   store16(rid, 0x01, 0)
   store16(rid, 0x02, 2)
   store32(rid, RIDEV_REMOVE, 4)
   store64_h(rid, 0, 8)
   def ok = RegisterRawInputDevices(rid, 1, 16) != 0
   free(rid)
   ok
}

fn _apply_cursor_handle(win){
   if(!win || !is_dict(win)){ return win }
   def mode = dict_get(win, "cursor_mode", backend_api.CURSOR_NORMAL)
   if(mode != backend_api.CURSOR_NORMAL && mode != backend_api.CURSOR_CAPTURED){ return win }
   def handle = _cursor_handle(dict_get(win, "cursor", 0))
   SetCursor(handle ? handle : LoadCursorW(0, _make_int_resource(IDC_ARROW)))
   win = dict_set(win, "cursor_handle", handle)
   win
}

fn _poll_monitor_changes(events, win){
   "Diffs current monitor list against known list; emits CONNECTED/DISCONNECTED events."
   if(!_monitors_initialized){
      ;; First call: snapshot current state without emitting events
      def monitors0 = get_monitors()
      mut names0 = []
      mut k = 0
      while(k < len(monitors0)){
         def aname = dict_get(get(monitors0, k), "adapter_name", "")
         if(is_str(aname) && aname != ""){ names0 = append(names0, aname) }
         k += 1
      }
      _known_monitor_names = names0
      _monitors_initialized = true
      return [events, win]
   }
   def monitors = get_monitors()
   mut current_names = dict()
   mut i = 0
   while(i < len(monitors)){
      def m = get(monitors, i)
      def aname = dict_get(m, "adapter_name", "")
      if(is_str(aname) && aname != ""){ current_names = dict_set(current_names, aname, m) }
      i += 1
   }
   ;; Detect new monitors
   def cur_keys = dict_keys(current_names)
   i = 0
   while(i < len(cur_keys)){
      def aname = get(cur_keys, i)
      mut was_known = false
      mut j = 0
      while(j < len(_known_monitor_names)){
         if(get(_known_monitor_names, j) == aname){ was_known = true break }
         j += 1
      }
      if(!was_known){
         mut data = dict()
         data = dict_set(data, "monitor", dict_get(current_names, aname, 0))
         _push_event(events, EVENT_MONITOR_CONNECTED, win, data)
      }
      i += 1
   }
   ;; Detect removed monitors
   i = 0
   while(i < len(_known_monitor_names)){
      def aname = get(_known_monitor_names, i)
      if(!dict_has(current_names, aname)){
         mut data = dict()
         data = dict_set(data, "adapter_name", aname)
         _push_event(events, EVENT_MONITOR_DISCONNECTED, win, data)
      }
      i += 1
   }
   _known_monitor_names = cur_keys
   [events, win]
}

fn _mods_from_win_state(win){
   mut mods = 0
   if(_key_state_bit(win, backend_api.KEY_LEFT_SHIFT) || _key_state_bit(win, backend_api.KEY_RIGHT_SHIFT)){ mods = bor(mods, MOD_SHIFT) }
   if(_key_state_bit(win, backend_api.KEY_LEFT_CONTROL) || _key_state_bit(win, backend_api.KEY_RIGHT_CONTROL)){ mods = bor(mods, MOD_CONTROL) }
   if(_key_state_bit(win, backend_api.KEY_LEFT_ALT) || _key_state_bit(win, backend_api.KEY_RIGHT_ALT)){ mods = bor(mods, MOD_ALT) }
   if(_key_state_bit(win, backend_api.KEY_LEFT_SUPER) || _key_state_bit(win, backend_api.KEY_RIGHT_SUPER)){ mods = bor(mods, MOD_SUPER) }
   if(_key_state_bit(win, backend_api.KEY_CAPS_LOCK)){ mods = bor(mods, MOD_CAPS_LOCK) }
   if(_key_state_bit(win, backend_api.KEY_NUM_LOCK)){ mods = bor(mods, MOD_NUM_LOCK) }
   mods
}

fn _mods_for_key_event(win, key, pressed){
   mut mods = _mods_from_win_state(win)
   match key {
      backend_api.KEY_LEFT_SHIFT, backend_api.KEY_RIGHT_SHIFT -> { mods = pressed ? bor(mods, MOD_SHIFT) : band(mods, bnot(MOD_SHIFT)) }
      backend_api.KEY_LEFT_CONTROL, backend_api.KEY_RIGHT_CONTROL -> { mods = pressed ? bor(mods, MOD_CONTROL) : band(mods, bnot(MOD_CONTROL)) }
      backend_api.KEY_LEFT_ALT, backend_api.KEY_RIGHT_ALT -> { mods = pressed ? bor(mods, MOD_ALT) : band(mods, bnot(MOD_ALT)) }
      backend_api.KEY_LEFT_SUPER, backend_api.KEY_RIGHT_SUPER -> { mods = pressed ? bor(mods, MOD_SUPER) : band(mods, bnot(MOD_SUPER)) }
      _ -> {}
   }
   mods
}

fn translate_vk_key(vk, scancode=0, extended=false){
   "Translates Win32 virtual keys to GLFW-compatible backend key ids."
   if(scancode == 0x056){ return backend_api.KEY_WORLD_2 }
   if(scancode == 0x059){ return backend_api.KEY_KP_EQUAL }
   if(scancode == 0x11c){ return backend_api.KEY_KP_ENTER }
   if(vk >= 48 && vk <= 57){ return 48 + (vk - 48) }
   if(vk >= 65 && vk <= 90){ return 65 + (vk - 65) }
   if(vk >= VK_F1 && vk <= VK_F12){ return backend_api.KEY_F1 + (vk - VK_F1) }
   if(vk >= VK_F13 && vk <= VK_F24){ return backend_api.KEY_F13 + (vk - VK_F13) }
   match vk {
      VK_ESCAPE -> backend_api.KEY_ESCAPE
      VK_RETURN -> backend_api.KEY_ENTER
      VK_TAB -> backend_api.KEY_TAB
      VK_BACK -> backend_api.KEY_BACKSPACE
      VK_INSERT -> backend_api.KEY_INSERT
      VK_DELETE -> backend_api.KEY_DELETE
      VK_RIGHT -> backend_api.KEY_RIGHT
      VK_LEFT -> backend_api.KEY_LEFT
      VK_DOWN -> backend_api.KEY_DOWN
      VK_UP -> backend_api.KEY_UP
      VK_PRIOR -> backend_api.KEY_PAGE_UP
      VK_NEXT -> backend_api.KEY_PAGE_DOWN
      VK_HOME -> backend_api.KEY_HOME
      VK_END -> backend_api.KEY_END
      VK_CAPITAL -> backend_api.KEY_CAPS_LOCK
      VK_SCROLL -> backend_api.KEY_SCROLL_LOCK
      VK_NUMLOCK -> backend_api.KEY_NUM_LOCK
      VK_SNAPSHOT -> backend_api.KEY_PRINT_SCREEN
      VK_PAUSE -> backend_api.KEY_PAUSE
      VK_SPACE -> backend_api.KEY_SPACE
      VK_NUMPAD0 -> backend_api.KEY_KP_0
      VK_NUMPAD1 -> backend_api.KEY_KP_1
      VK_NUMPAD2 -> backend_api.KEY_KP_2
      VK_NUMPAD3 -> backend_api.KEY_KP_3
      VK_NUMPAD4 -> backend_api.KEY_KP_4
      VK_NUMPAD5 -> backend_api.KEY_KP_5
      VK_NUMPAD6 -> backend_api.KEY_KP_6
      VK_NUMPAD7 -> backend_api.KEY_KP_7
      VK_NUMPAD8 -> backend_api.KEY_KP_8
      VK_NUMPAD9 -> backend_api.KEY_KP_9
      VK_DECIMAL, VK_SEPARATOR -> backend_api.KEY_KP_DECIMAL
      VK_DIVIDE -> backend_api.KEY_KP_DIVIDE
      VK_MULTIPLY -> backend_api.KEY_KP_MULTIPLY
      VK_SUBTRACT -> backend_api.KEY_KP_SUBTRACT
      VK_ADD -> backend_api.KEY_KP_ADD
      VK_LSHIFT -> backend_api.KEY_LEFT_SHIFT
      VK_RSHIFT -> backend_api.KEY_RIGHT_SHIFT
      VK_SHIFT -> scancode == 0x36 ? backend_api.KEY_RIGHT_SHIFT : backend_api.KEY_LEFT_SHIFT
      VK_LCONTROL -> backend_api.KEY_LEFT_CONTROL
      VK_RCONTROL -> backend_api.KEY_RIGHT_CONTROL
      VK_CONTROL -> extended ? backend_api.KEY_RIGHT_CONTROL : backend_api.KEY_LEFT_CONTROL
      VK_LMENU -> backend_api.KEY_LEFT_ALT
      VK_RMENU -> backend_api.KEY_RIGHT_ALT
      VK_MENU -> extended ? backend_api.KEY_RIGHT_ALT : backend_api.KEY_LEFT_ALT
      VK_LWIN -> backend_api.KEY_LEFT_SUPER
      VK_RWIN -> backend_api.KEY_RIGHT_SUPER
      VK_APPS -> backend_api.KEY_MENU
      0xBA -> backend_api.KEY_SEMICOLON
      0xBB -> backend_api.KEY_EQUAL
      0xBC -> backend_api.KEY_COMMA
      0xBD -> backend_api.KEY_MINUS
      0xBE -> backend_api.KEY_PERIOD
      0xBF -> backend_api.KEY_SLASH
      0xC0 -> backend_api.KEY_GRAVE_ACCENT
      0xDB -> backend_api.KEY_LEFT_BRACKET
      0xDC -> backend_api.KEY_BACKSLASH
      0xDD -> backend_api.KEY_RIGHT_BRACKET
      0xDE -> backend_api.KEY_APOSTROPHE
      _ -> -1
   }
}

fn _push_event(events, typ, win, data=0){
   append(events, ui_event.make_event(typ, win, dict_get(win, "handle", 0), data))
}

fn _emit_char_event(events, win, codepoint, mods=0){
   if(codepoint <= 0){ return events }
   mut data = dict()
   data = dict_set(data, "codepoint", int(codepoint))
   data = dict_set(data, "mods", mods)
   _push_event(events, EVENT_KEY_CHAR, win, data)
}

fn _emit_utf16_char_events(events, win, wide, units, mods=0){
   if(!wide || units <= 0){ return events }
   mut out = events
   mut pending_high = 0
   mut i = 0
   while(i < units){
      def ch = int(load16(wide, i * 2))
      if(ch >= 0xd800 && ch <= 0xdbff){
         pending_high = ch
      } elif(ch >= 0xdc00 && ch <= 0xdfff){
         if(pending_high >= 0xd800 && pending_high <= 0xdbff){
         def codepoint = ((pending_high - 0xd800) << 10) + (ch - 0xdc00) + 0x10000
         _emit_char_event(out, win, codepoint, mods)
         }
         pending_high = 0
      } else {
         pending_high = 0
         _emit_char_event(out, win, ch, mods)
      }
      i += 1
   }
   out
}

fn _emit_drop_event(events, win, drop){
   if(!drop){ return [win, events] }
   mut out = events
   def point = calloc(1, 8)
   if(point){
      if(DragQueryPoint(drop, point) != 0){
         def x = load32(point, 0)
         def y = load32(point, 4)
         win = dict_set(win, "mouse_x", x)
         win = dict_set(win, "mouse_y", y)
         mut move_data = dict()
         move_data = dict_set(move_data, "x", x)
         move_data = dict_set(move_data, "y", y)
         _push_event(out, EVENT_MOUSE_POS_CHANGED, win, move_data)
      }
      free(point)
   }

   def count = int(DragQueryFileW(drop, 0xffffffff, 0, 0))
   mut paths = []
   mut i = 0
   while(i < count){
      def length = int(DragQueryFileW(drop, i, 0, 0))
      if(length >= 0){
         def wide = calloc(length + 1, 2)
         if(wide){
         if(DragQueryFileW(drop, i, wide, length + 1) > 0){
               def path = utf8_from_utf16(wide)
               if(path && len(path) > 0){ paths = append(paths, path) }
         }
         free(wide)
         }
      }
      i += 1
   }

   if(len(paths) > 0){
      mut data = dict()
      data = dict_set(data, "paths", paths)
      _push_event(out, EVENT_DATA_DROP, win, data)
   }
   DragFinish(drop)
   [win, out]
}

fn _emit_ime_result_events(events, win){
   if(!available() || !win || !is_dict(win)){ return [win, events] }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return [win, events] }
   def himc = ImmGetContext(hwnd)
   if(!himc){ return [win, events] }

   mut out = events
   def bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, 0, 0)
   if(bytes > 0){
      def buffer = calloc(bytes + 2, 1)
      if(buffer){
         def got = ImmGetCompositionStringW(himc, GCS_RESULTSTR, buffer, bytes)
         if(got > 0){
         win = dict_set(win, "high_surrogate", 0)
         out = _emit_utf16_char_events(out, win, buffer, got / 2, _mods_from_win_state(win))
         }
         free(buffer)
      }
   }
   ImmReleaseContext(hwnd, himc)
   [win, out]
}

fn translate_message(win, msg){
   "Translates a raw Win32 message dict into Ny window events and updated state."
   if(!win || !is_dict(win) || !msg || !is_dict(msg)){ return [win, []] }
   mut events = []
   def typ = dict_get(msg, "message", 0)
   def wparam = dict_get(msg, "wparam", 0)
   mut lparam = dict_get(msg, "lparam", 0)

   match typ {
      WM_CLOSE, WM_QUIT -> {
         win = dict_set(win, "should_close", true)
         _push_event(events, EVENT_QUIT, win, 0)
      }
      WM_INPUTLANGCHANGE -> {
         win = dict_set(win, "input_lang_changed", true)
      }
      WM_IME_STARTCOMPOSITION -> {
         win = dict_set(win, "ime_composing", true)
      }
      WM_IME_ENDCOMPOSITION -> {
         win = dict_set(win, "ime_composing", false)
      }
      WM_IME_COMPOSITION -> {
         if(band(int(lparam), GCS_RESULTSTR) != 0){
         def res = _emit_ime_result_events(events, win)
         win = get(res, 0, win)
         events = get(res, 1, events)
         } elif(band(int(lparam), GCS_COMPSTR) != 0){
         win = dict_set(win, "ime_composing", true)
         }
      }
      WM_IME_SETCONTEXT -> {
         lparam = band(lparam, bnot(1)) ;; Prevent default IME UI from showing
      }
      WM_IME_NOTIFY -> {
         ;; GLFW handles this for composition window position, we'll just allow it for now
      }
      WM_SETFOCUS -> {
         win = dict_set(win, "focused", true)
         _push_event(events, EVENT_FOCUS_IN, win, 0)
      }
      WM_KILLFOCUS -> {
         win = dict_set(win, "focused", false)
         _push_event(events, EVENT_FOCUS_OUT, win, 0)
      }
      WM_CHAR, WM_SYSCHAR -> {
         def ch = int(wparam)
         if(ch >= 0xd800 && ch <= 0xdbff){
         win = dict_set(win, "high_surrogate", ch)
         } else {
         mut codepoint = 0
         if(ch >= 0xdc00 && ch <= 0xdfff){
               def high = int(dict_get(win, "high_surrogate", 0))
               if(high >= 0xd800 && high <= 0xdbff){
                  codepoint = ((high - 0xd800) << 10) + (ch - 0xdc00) + 0x10000
               }
         } else {
               codepoint = ch
         }
         win = dict_set(win, "high_surrogate", 0)
         _emit_char_event(events, win, codepoint, _mods_from_win_state(win))
         }
      }
      WM_UNICHAR -> {
         if(int(wparam) != UNICODE_NOCHAR){
         _emit_char_event(events, win, int(wparam), _mods_from_win_state(win))
         }
      }
      WM_DROPFILES -> {
         def res = _emit_drop_event(events, win, wparam)
         win = get(res, 0, win)
         events = get(res, 1, events)
      }
      WM_KEYDOWN, WM_SYSKEYDOWN, WM_KEYUP, WM_SYSKEYUP -> {
         def hi = _hiword(lparam)
         mut scancode = band(hi, bor(KF_EXTENDED, 0xff))
         def released = band(hi, KF_UP) != 0 || typ == WM_KEYUP || typ == WM_SYSKEYUP
         if(scancode == 0){
         scancode = MapVirtualKeyW(int(wparam), MAPVK_VK_TO_VSC)
         }
         if(scancode == 0x54){ scancode = 0x137 }
         if(scancode == 0x146){ scancode = 0x45 }
         if(scancode == 0x136){ scancode = 0x36 }
         if(int(wparam) == VK_PROCESSKEY){ return [win, events] }
         def key = translate_vk_key(int(wparam), scancode, band(hi, KF_EXTENDED) != 0)
         if(key >= 0){
         mut ks = dict_get(win, "key_states", dict(64))
         def mods = _mods_for_key_event(win, key, !released)
         if(released && int(wparam) == VK_SHIFT){
         ks = dict_set(ks, backend_api.KEY_LEFT_SHIFT, false)
         ks = dict_set(ks, backend_api.KEY_RIGHT_SHIFT, false)
         win = dict_set(win, "key_states", ks)
         mut left_data = dict()
         left_data = dict_set(left_data, "raw_key", int(wparam))
         left_data = dict_set(left_data, "key", backend_api.KEY_LEFT_SHIFT)
         left_data = dict_set(left_data, "scancode", scancode)
         left_data = dict_set(left_data, "action", backend_api.ACTION_RELEASE)
         left_data = dict_set(left_data, "mods", mods)
         _push_event(events, EVENT_KEY_RELEASED, win, left_data)
         mut right_data = dict()
         right_data = dict_set(right_data, "raw_key", int(wparam))
         right_data = dict_set(right_data, "key", backend_api.KEY_RIGHT_SHIFT)
         right_data = dict_set(right_data, "scancode", scancode)
         right_data = dict_set(right_data, "action", backend_api.ACTION_RELEASE)
         right_data = dict_set(right_data, "mods", mods)
         _push_event(events, EVENT_KEY_RELEASED, win, right_data)
         } elif(int(wparam) == VK_SNAPSHOT && released){
         ks = dict_set(ks, key, false)
         win = dict_set(win, "key_states", ks)
         mut press_data = dict()
         press_data = dict_set(press_data, "raw_key", int(wparam))
         press_data = dict_set(press_data, "key", key)
         press_data = dict_set(press_data, "scancode", scancode)
         press_data = dict_set(press_data, "action", backend_api.ACTION_PRESS)
         press_data = dict_set(press_data, "mods", mods)
         _push_event(events, EVENT_KEY_PRESSED, win, press_data)
         mut rel_data = dict()
         rel_data = dict_set(rel_data, "raw_key", int(wparam))
         rel_data = dict_set(rel_data, "key", key)
         rel_data = dict_set(rel_data, "scancode", scancode)
         rel_data = dict_set(rel_data, "action", backend_api.ACTION_RELEASE)
         rel_data = dict_set(rel_data, "mods", mods)
         _push_event(events, EVENT_KEY_RELEASED, win, rel_data)
         } else {
         ks = dict_set(ks, key, !released)
         win = dict_set(win, "key_states", ks)
         mut data = dict()
         data = dict_set(data, "raw_key", int(wparam))
         data = dict_set(data, "key", key)
         data = dict_set(data, "scancode", scancode)
         def is_repeat = !released && band(hi, KF_REPEAT) != 0
         data = dict_set(data, "action", released ? backend_api.ACTION_RELEASE : (is_repeat ? backend_api.ACTION_REPEAT : backend_api.ACTION_PRESS))
         data = dict_set(data, "mods", mods)
         _push_event(events, released ? EVENT_KEY_RELEASED : EVENT_KEY_PRESSED, win, data)
         }
         }
      }
      WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN,
      WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP -> {
         def pressed = typ == WM_LBUTTONDOWN || typ == WM_RBUTTONDOWN || typ == WM_MBUTTONDOWN || typ == WM_XBUTTONDOWN
         def button =
         (typ == WM_LBUTTONDOWN || typ == WM_LBUTTONUP) ? backend_api.MOUSE_BUTTON_LEFT :
         (typ == WM_RBUTTONDOWN || typ == WM_RBUTTONUP) ? backend_api.MOUSE_BUTTON_RIGHT :
         (typ == WM_MBUTTONDOWN || typ == WM_MBUTTONUP) ? backend_api.MOUSE_BUTTON_MIDDLE :
         (_hiword(wparam) == XBUTTON1 ? backend_api.MOUSE_BUTTON_4 : backend_api.MOUSE_BUTTON_5)
         mut buttons = dict_get(win, "mouse_buttons", dict(8))
         buttons = dict_set(buttons, button, pressed)
         win = dict_set(win, "mouse_buttons", buttons)
         mut data = dict()
         data = dict_set(data, "button", button)
         data = dict_set(data, "action", pressed ? backend_api.ACTION_PRESS : backend_api.ACTION_RELEASE)
         data = dict_set(data, "mods", _mods_from_win_state(win))
         _push_event(events, pressed ? EVENT_MOUSE_BUTTON_PRESSED : EVENT_MOUSE_BUTTON_RELEASED, win, data)
      }
      WM_MOUSEMOVE -> {
         def x = _get_x_lparam(lparam)
         def y = _get_y_lparam(lparam)
         def entered = !dict_get(win, "cursor_tracked", false)
         win = dict_set(win, "cursor_tracked", true)
         win = dict_set(win, "last_cursor_client_x", x)
         win = dict_set(win, "last_cursor_client_y", y)
         win = dict_set(win, "mouse_x", x)
         win = dict_set(win, "mouse_y", y)
         if(entered){ _push_event(events, EVENT_MOUSE_ENTER, win, 0) }
         mut data = dict()
         data = dict_set(data, "x", x)
         data = dict_set(data, "y", y)
         _push_event(events, EVENT_MOUSE_POS_CHANGED, win, data)
      }
      WM_INPUT -> {
         if(dict_get(win, "disabled_cursor", false) && dict_get(win, "raw_mouse_motion", false)){
         def size_ptr = calloc(1, 4)
         if(size_ptr){
               GetRawInputData(lparam, RID_INPUT, 0, size_ptr, 24)
               def size = load32(size_ptr, 0)
               if(size >= 48){
                  def raw = calloc(1, size)
                  if(raw){
                     store32(size_ptr, size, 0)
                     if(GetRawInputData(lparam, RID_INPUT, raw, size_ptr, 24) != 0xffffffff){
                  if(load32(raw, 0) == RIM_TYPEMOUSE){
                           def flags = load16(raw, 24)
                           mut dx = 0
                           mut dy = 0
                           if(band(flags, MOUSE_MOVE_ABSOLUTE)){
                              def pt = calloc(1, 8)
                              if(pt){
                                 def width = band(flags, MOUSE_VIRTUAL_DESKTOP) ? GetSystemMetrics(SM_CXVIRTUALSCREEN) : GetSystemMetrics(SM_CXSCREEN)
                                 def height = band(flags, MOUSE_VIRTUAL_DESKTOP) ? GetSystemMetrics(SM_CYVIRTUALSCREEN) : GetSystemMetrics(SM_CYSCREEN)
                                 def base_x = band(flags, MOUSE_VIRTUAL_DESKTOP) ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0
                                 def base_y = band(flags, MOUSE_VIRTUAL_DESKTOP) ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0
                                 store32(pt, base_x + int((float(load32(raw, 36)) / 65535.0) * float(width)), 0)
                                 store32(pt, base_y + int((float(load32(raw, 40)) / 65535.0) * float(height)), 4)
                                 if(ScreenToClient(dict_get(win, "handle", 0), pt) != 0){
                           dx = load32(pt, 0) - dict_get(win, "mouse_x", 0)
                           dy = load32(pt, 4) - dict_get(win, "mouse_y", 0)
                                 }
                                 free(pt)
                              }
                           } else {
                              dx = _signed32(load32(raw, 36))
                              dy = _signed32(load32(raw, 40))
                           }
                           if(dx != 0 || dy != 0){
                              def next_x = dict_get(win, "mouse_x", 0) + dx
                              def next_y = dict_get(win, "mouse_y", 0) + dy
                              win = dict_set(win, "mouse_x", next_x)
                              win = dict_set(win, "mouse_y", next_y)
                              mut data = dict()
                              data = dict_set(data, "x", next_x)
                              data = dict_set(data, "y", next_y)
                              data = dict_set(data, "dx", dx)
                              data = dict_set(data, "dy", dy)
                              data = dict_set(data, "raw", true)
                              _push_event(events, EVENT_MOUSE_POS_CHANGED, win, data)
                           }
                  }
                     }
                     free(raw)
                  }
               }
               free(size_ptr)
         }
         }
      }
      WM_MOUSELEAVE -> {
         win = dict_set(win, "cursor_tracked", false)
         _push_event(events, EVENT_MOUSE_LEAVE, win, 0)
      }
      WM_MOUSEWHEEL, WM_MOUSEHWHEEL -> {
         mut data = dict()
         def delta = float(_signed16(_hiword(wparam))) / float(WHEEL_DELTA)
         if(typ == WM_MOUSEHWHEEL){
         data = dict_set(data, "x", delta)
         data = dict_set(data, "y", 0.0)
         } else {
         data = dict_set(data, "x", 0.0)
         data = dict_set(data, "y", delta)
         }
         _push_event(events, EVENT_MOUSE_SCROLL, win, data)
      }
      WM_MOVE -> {
         def x = _get_x_lparam(lparam)
         def y = _get_y_lparam(lparam)
         win = dict_set(win, "x", x)
         win = dict_set(win, "y", y)
         mut data = dict()
         data = dict_set(data, "x", x)
         data = dict_set(data, "y", y)
         _push_event(events, EVENT_WINDOW_MOVED, win, data)
      }
      WM_SIZE -> {
         def size_type = int(wparam)
         def w = _loword(lparam)
         def h = _hiword(lparam)
         win = dict_set(win, "w", w)
         win = dict_set(win, "h", h)
         mut data = dict()
         data = dict_set(data, "w", w)
         data = dict_set(data, "h", h)
         _push_event(events, EVENT_WINDOW_RESIZED, win, data)
         if(size_type == SIZE_MINIMIZED){
         win = dict_set(win, "iconified", true)
         _push_event(events, EVENT_WINDOW_MINIMIZED, win, 0)
         } elif(size_type == SIZE_MAXIMIZED){
         win = dict_set(win, "iconified", false)
         win = dict_set(win, "maximized", true)
         _push_event(events, EVENT_WINDOW_MAXIMIZED, win, 0)
         } elif(size_type == SIZE_RESTORED){
         def was_iconified = dict_get(win, "iconified", false)
         def was_maximized = dict_get(win, "maximized", false)
         win = dict_set(win, "iconified", false)
         win = dict_set(win, "maximized", false)
         if(was_iconified || was_maximized){
               _push_event(events, EVENT_WINDOW_RESTORED, win, 0)
         }
         }
      }
      WM_DPICHANGED -> {
         def scale = float(_hiword(wparam)) / 96.0
         win = dict_set(win, "scale_x", scale)
         win = dict_set(win, "scale_y", scale)
         mut data = dict()
         data = dict_set(data, "x", scale)
         data = dict_set(data, "y", scale)
         _push_event(events, EVENT_SCALE_UPDATED, win, data)
      }
      WM_DISPLAYCHANGE -> {
         def res = _poll_monitor_changes(events, win)
         events = get(res, 0, events)
         win = get(res, 1, win)
      }
      WM_PAINT -> {
         _push_event(events, EVENT_WINDOW_REFRESH, win, 0)
      }
      WM_ENTERSIZEMOVE -> {
         win = dict_set(win, "in_sizemove", true)
      }
      WM_EXITSIZEMOVE -> {
         win = dict_set(win, "in_sizemove", false)
      }
      _ -> {}
   }

   [win, events]
}

fn translate_messages(win, messages){
   "Translates a list of raw Win32 messages for one window."
   if(!win || !is_dict(win) || !is_list(messages)){ return [win, []] }
   mut out = []
   mut i = 0
   while(i < len(messages)){
      def msg = get(messages, i, 0)
      if(msg && is_dict(msg) &&
         (dict_get(msg, "message", 0) == WM_KEYDOWN || dict_get(msg, "message", 0) == WM_SYSKEYDOWN ||
          dict_get(msg, "message", 0) == WM_KEYUP || dict_get(msg, "message", 0) == WM_SYSKEYUP) &&
         int(dict_get(msg, "wparam", 0)) == VK_CONTROL &&
         !band(_hiword(dict_get(msg, "lparam", 0)), KF_EXTENDED) &&
         i + 1 < len(messages)){
         def next = get(messages, i + 1, 0)
         if(next && is_dict(next) &&
         (dict_get(next, "message", 0) == WM_KEYDOWN || dict_get(next, "message", 0) == WM_SYSKEYDOWN ||
             dict_get(next, "message", 0) == WM_KEYUP || dict_get(next, "message", 0) == WM_SYSKEYUP) &&
         int(dict_get(next, "wparam", 0)) == VK_MENU &&
         band(_hiword(dict_get(next, "lparam", 0)), KF_EXTENDED) &&
         dict_get(next, "time", -1) == dict_get(msg, "time", -2)){
         i += 1
         continue
         }
      }
      def res = translate_message(win, msg)
      win = get(res, 0, win)
      def evs = get(res, 1, [])
      if(is_list(evs) && len(evs) > 0){ out = extend(out, evs) }
      i += 1
   }
   [win, out]
}

fn _vk_from_key(key){
   "Maps a Nytrix key constant to a Win32 virtual key code."
   if(key >= 65 && key <= 90){ return key } ;; A-Z
   if(key >= 48 && key <= 57){ return key } ;; 0-9
   match key {
      KEY_SPACE -> { return 0x20 }
      KEY_APOSTROPHE -> { return 0xDE }
      KEY_COMMA -> { return 0xBC }
      KEY_MINUS -> { return 0xBD }
      KEY_PERIOD -> { return 0xBE }
      KEY_SLASH -> { return 0xBF }
      KEY_SEMICOLON -> { return 0xBA }
      KEY_EQUAL -> { return 0xBB }
      KEY_LEFT_BRACKET -> { return 0xDB }
      KEY_BACKSLASH -> { return 0xDC }
      KEY_RIGHT_BRACKET -> { return 0xDD }
      KEY_GRAVE_ACCENT -> { return 0xC0 }
   }
   0
}

fn get_key_name(win, key, scancode){
   "Returns the layout-specific name of the specified printable key."
   if(!available()){ return "" }
   mut code = int(scancode)
   if(code == 0){
      def vk = _vk_from_key(key)
      if(vk == 0){ return "" }
      code = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)
      if(code == 0){ return "" }
   }
   def lparam = bshl(code, 16)
   def buf = malloc(128)
   if(!buf){ return "" }
   def len = GetKeyNameTextW(lparam, buf, 64)
   mut name = ""
   if(len > 0){
      name = utf8_from_utf16(buf)
   }
   free(buf)
   name
}

fn get_key_state(win, key){
   "Returns GLFW-style key state from the cached native window dictionary."
   if(!win || !is_dict(win)){ return 0 }
   dict_get(dict_get(win, "key_states", dict()), key, false) ? 1 : 0
}

fn get_mouse_button_state(win, button){
   "Returns GLFW-style mouse button state from the cached native window dictionary."
   if(!win || !is_dict(win)){ return 0 }
   dict_get(dict_get(win, "mouse_buttons", dict()), button, false) ? 1 : 0
}

fn ensure_window_class(class_name="NytrixWindowClass", icon_res=0){
   "Registers a reusable Win32 window class and returns its atom."
   if(!available()){ return 0 }
   def key = to_str(class_name)
   def existing = dict_get(_registered_classes, key, 0)
   if(existing && is_dict(existing)){ return dict_get(existing, "atom", 0) }

   def wide_name = utf16_from_utf8(key)
   if(!wide_name){ return 0 }
   def instance = GetModuleHandleW(0)
   def wc = calloc(1, 80)
   if(!wc){
      free(wide_name)
      return 0
   }

   store32(wc, 80, 0)
   store32(wc, CS_HREDRAW | CS_VREDRAW | CS_OWNDC, 4)
   store64_h(wc, _default_window_proc, 8)
   store32(wc, 0, 16)
   store32(wc, 0, 20)
   store64_h(wc, instance, 24)

   mut icon = 0
   if(icon_res){
      icon = LoadImageW(instance, _make_int_resource(icon_res), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED)
   }
   if(!icon){
      icon = LoadImageW(0, _make_int_resource(IDI_APPLICATION), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED)
   }
   store64_h(wc, icon, 32)
   store64_h(wc, LoadCursorW(0, _make_int_resource(IDC_ARROW)), 40)
   store64_h(wc, 0, 48)
   store64_h(wc, 0, 56)
   store64_h(wc, wide_name, 64)
   store64_h(wc, icon, 72)

   def atom = RegisterClassExW(wc)
   free(wc)
   if(atom == 0){
      free(wide_name)
      return 0
   }

   mut rec = dict()
   rec = dict_set(rec, "atom", atom)
   rec = dict_set(rec, "instance", instance)
   rec = dict_set(rec, "wide_name", wide_name)
   _registered_classes = dict_set(_registered_classes, key, rec)
   atom
}

fn create_basic_window(title, width, height, x=CW_USEDEFAULT, y=CW_USEDEFAULT, flags=0, class_name="NytrixWindowClass"){
   "Creates a basic native Win32 window directly from Ny code."
   if(!available()){ return false }
   init_dpi_awareness()
   init_timer()
   def atom = ensure_window_class(class_name)
   if(atom == 0){ return false }

   def cls = dict_get(_registered_classes, to_str(class_name), 0)
   if(!cls || !is_dict(cls)){ return false }
   def instance = dict_get(cls, "instance", GetModuleHandleW(0))
   def wide_title = utf16_from_utf8(title)
   if(!wide_title){ return false }

   def style = get_window_style(flags, false)
   def ex_style = get_window_ex_style(flags, false)

   def rect = calloc(1, 16)
   if(!rect){
      free(wide_title)
      return false
   }
   store32(rect, 0, 0)
   store32(rect, 0, 4)
   store32(rect, width, 8)
   store32(rect, height, 12)
   _adjust_window_rect(rect, style, ex_style)
   def frame_w = _rect_width(rect)
   def frame_h = _rect_height(rect)
   free(rect)

   def hwnd = CreateWindowExW(ex_style, _make_int_resource(atom), wide_title,
      style, x, y, frame_w, frame_h, 0, 0, instance, 0)
   free(wide_title)
   if(!hwnd){ return false }

   def prop_name = utf16_from_utf8("nytrix")
   if(prop_name){
      SetPropW(hwnd, prop_name, hwnd)
      free(prop_name)
   }
   ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, 0)
   ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, 0)
   ChangeWindowMessageFilterEx(hwnd, WM_COPYGLOBALDATA, MSGFLT_ALLOW, 0)

   mut win = dict()
   win = dict_set(win, "handle", hwnd)
   win = dict_set(win, "instance", instance)
   win = dict_set(win, "class_atom", atom)
   win = dict_set(win, "class_name", to_str(class_name))
   win = dict_set(win, "title", to_str(title))
   win = dict_set(win, "x", x)
   win = dict_set(win, "y", y)
   win = dict_set(win, "w", width)
   win = dict_set(win, "h", height)
   win = dict_set(win, "flags", flags)
   win = dict_set(win, "resizable", !band(flags, WINDOW_NO_RESIZE))
   win = dict_set(win, "decorated", !(band(flags, WINDOW_NO_BORDER) || band(flags, WINDOW_TRANSPARENT)))
   win = dict_set(win, "floating", band(flags, WINDOW_FLOATING))
   win = dict_set(win, "fullscreen", band(flags, WINDOW_FULLSCREEN))
   win = dict_set(win, "transparent", band(flags, WINDOW_TRANSPARENT))
   win = dict_set(win, "opacity", 1.0)
   win = dict_set(win, "key_states", dict(64))
   win = dict_set(win, "mouse_buttons", dict(8))
   win = dict_set(win, "mouse_x", 0)
   win = dict_set(win, "mouse_y", 0)
   win = dict_set(win, "cursor", 0)
   win = dict_set(win, "cursor_handle", 0)
   win = dict_set(win, "cursor_tracked", false)
   win = dict_set(win, "last_cursor_client_x", 0)
   win = dict_set(win, "last_cursor_client_y", 0)
   win = dict_set(win, "raw_mouse_motion", band(flags, WINDOW_RAW_MOUSE))
   win = dict_set(win, "cursor_mode",
      band(flags, WINDOW_CAPTURE_MOUSE) ? backend_api.CURSOR_CAPTURED :
      (band(flags, WINDOW_HIDE_MOUSE) ? backend_api.CURSOR_HIDDEN : backend_api.CURSOR_NORMAL))
   win = dict_set(win, "cursor_visible", !band(flags, WINDOW_HIDE_MOUSE))
   win = dict_set(win, "captured_cursor", band(flags, WINDOW_CAPTURE_MOUSE))
   win = dict_set(win, "disabled_cursor", false)
   win = dict_set(win, "big_icon", 0)
   win = dict_set(win, "small_icon", 0)
   win = dict_set(win, "big_icon_owned", false)
   win = dict_set(win, "small_icon_owned", false)
   win = dict_set(win, "visible", !band(flags, WINDOW_HIDE))
   win = _update_framebuffer_transparency(win)
   DragAcceptFiles(hwnd, 1)
   if(!band(flags, WINDOW_HIDE)){
      ShowWindow(hwnd, band(flags, WINDOW_MAXIMIZE) ? SW_SHOWMAXIMIZED :
         (band(flags, WINDOW_MINIMIZE) ? SW_SHOWMINIMIZED : SW_SHOWNA))
   }
   win = dict_set(win, "min_w", -1)
   win = dict_set(win, "min_h", -1)
   win = dict_set(win, "max_w", -1)
   win = dict_set(win, "max_h", -1)
   _windows = dict_set(_windows, hwnd, win)
   win
}

fn destroy_basic_window(win){
   "Destroys a Win32 window created by `create_basic_window`."
   if(!available()){ return false }
   if(is_dict(win) && dict_get(win, "monitor_mode_changed", false)){
      _restore_video_mode(dict_get(win, "monitor_adapter_name", ""))
   }
   if(is_dict(win)){
      if(dict_get(win, "big_icon_owned", false) && dict_get(win, "big_icon", 0)){ DestroyIcon(dict_get(win, "big_icon", 0)) }
      if(dict_get(win, "small_icon_owned", false) && dict_get(win, "small_icon", 0)){ DestroyIcon(dict_get(win, "small_icon", 0)) }
   }
   def hwnd = is_dict(win) ? dict_get(win, "handle", 0) : win
   if(!hwnd){ return false }
   DragAcceptFiles(hwnd, 0)
   DestroyWindow(hwnd) != 0
}

fn show_window(win, cmd=SW_SHOWNA){
   "Shows a Win32 window."
   if(!available()){ return false }
   def hwnd = is_dict(win) ? dict_get(win, "handle", 0) : win
   if(!hwnd){ return false }
   ShowWindow(hwnd, cmd) != 0
}

fn hide_window(win){
   "Hides a Win32 window."
   show_window(win, SW_HIDE)
}

fn set_title(win, title){
   "Sets a Win32 window title using a UTF-16 conversion path."
   if(!available()){ return false }
   def hwnd = is_dict(win) ? dict_get(win, "handle", 0) : win
   if(!hwnd){ return false }
   def wide = utf16_from_utf8(title)
   if(!wide){ return false }
   def ok = SetWindowTextW(hwnd, wide) != 0
   free(wide)
   ok
}

fn set_window_icon(win, images){
   "Applies Win32 big/small icons from Ny RGBA image dictionaries."
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return win }

   mut big_icon = GetClassLongPtrW(hwnd, GCLP_HICON)
   mut small_icon = GetClassLongPtrW(hwnd, GCLP_HICONSM)
   mut big_owned = false
   mut small_owned = false

   if((is_list(images) || is_tuple(images)) && len(images) > 0){
      def dpi = _window_dpi(hwnd)
      def big_image = _choose_icon_image(images, _system_metric(SM_CXICON, dpi), _system_metric(SM_CYICON, dpi))
      def small_image = _choose_icon_image(images, _system_metric(SM_CXSMICON, dpi), _system_metric(SM_CYSMICON, dpi))
      if(big_image){
         big_icon = _create_rgba_cursor_handle(big_image, 0, 0, true)
         big_owned = !!big_icon
      }
      if(small_image){
         small_icon = _create_rgba_cursor_handle(small_image, 0, 0, true)
         small_owned = !!small_icon
      }
   } elif(is_dict(images)){
      big_icon = _create_rgba_cursor_handle(images, 0, 0, true)
      small_icon = _create_rgba_cursor_handle(images, 0, 0, true)
      big_owned = !!big_icon
      small_owned = !!small_icon
   }

   SendMessageW(hwnd, WM_SETICON, ICON_BIG, big_icon)
   SendMessageW(hwnd, WM_SETICON, ICON_SMALL, small_icon)

   if(dict_get(win, "big_icon_owned", false) && dict_get(win, "big_icon", 0)){ DestroyIcon(dict_get(win, "big_icon", 0)) }
   if(dict_get(win, "small_icon_owned", false) && dict_get(win, "small_icon", 0)){ DestroyIcon(dict_get(win, "small_icon", 0)) }

   win = dict_set(win, "big_icon", big_icon)
   win = dict_set(win, "small_icon", small_icon)
   win = dict_set(win, "big_icon_owned", big_owned)
   win = dict_set(win, "small_icon_owned", small_owned)
   win = dict_set(win, "icon_images", images)
   win
}

fn get_pos(win){
   "Returns the window client-area origin in screen coordinates."
   if(!available() || !win || !is_dict(win)){ return [0, 0] }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return [0, 0] }
   def pt = calloc(1, 8)
   if(!pt){ return [dict_get(win, "x", 0), dict_get(win, "y", 0)] }
   store32(pt, 0, 0)
   store32(pt, 0, 4)
   mut out = [dict_get(win, "x", 0), dict_get(win, "y", 0)]
   if(ClientToScreen(hwnd, pt) != 0){
      out = [load32(pt, 0), load32(pt, 4)]
   }
   free(pt)
   out
}

fn set_pos(win, x, y){
   "Sets the Win32 window client-area origin."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def rect = calloc(1, 16)
   if(!rect){ return false }
   store32(rect, x, 0)
   store32(rect, y, 4)
   store32(rect, x, 8)
   store32(rect, y, 12)
   _adjust_window_rect(rect,
      get_window_style(_effective_flags(win), dict_get(win, "fullscreen", false)),
      get_window_ex_style(_effective_flags(win), dict_get(win, "fullscreen", false)),
      hwnd)
   def ok = SetWindowPos(hwnd, 0, load32(rect, 0), load32(rect, 4), 0, 0,
      SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE) != 0
   free(rect)
   ok
}

fn get_size(win){
   "Returns the Win32 client-area size."
   if(!available() || !win || !is_dict(win)){ return [0, 0] }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return [0, 0] }
   def rect = calloc(1, 16)
   if(!rect){ return [dict_get(win, "w", 0), dict_get(win, "h", 0)] }
   mut out = [dict_get(win, "w", 0), dict_get(win, "h", 0)]
   if(GetClientRect(hwnd, rect) != 0){
      out = [_rect_width(rect), _rect_height(rect)]
   }
   free(rect)
   out
}

fn get_framebuffer_size(win){
   "Returns the framebuffer size, matching Win32 client size."
   get_size(win)
}

fn set_size(win, width, height){
   "Sets the Win32 client-area size."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def rect = calloc(1, 16)
   if(!rect){ return false }
   store32(rect, 0, 0)
   store32(rect, 0, 4)
   store32(rect, width, 8)
   store32(rect, height, 12)
   _adjust_window_rect(rect,
      get_window_style(_effective_flags(win), dict_get(win, "fullscreen", false)),
      get_window_ex_style(_effective_flags(win), dict_get(win, "fullscreen", false)),
      hwnd)
   def ok = SetWindowPos(hwnd, HWND_TOP, 0, 0, _rect_width(rect), _rect_height(rect),
      SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOZORDER) != 0
   free(rect)
   ok
}

fn iconify_window(win){
   "Iconifies a Win32 window."
   if(!available() || !win || !is_dict(win)){ return false }
   ShowWindow(dict_get(win, "handle", 0), SW_MINIMIZE) != 0
}

fn restore_window(win){
   "Restores a minimized or maximized Win32 window."
   if(!available() || !win || !is_dict(win)){ return false }
   ShowWindow(dict_get(win, "handle", 0), SW_RESTORE) != 0
}

fn maximize_window(win){
   "Maximizes a Win32 window."
   if(!available() || !win || !is_dict(win)){ return false }
   ShowWindow(dict_get(win, "handle", 0), SW_SHOWMAXIMIZED) != 0
}

fn request_window_attention(win){
   "Requests user attention for a Win32 window."
   if(!available() || !win || !is_dict(win)){ return false }
   FlashWindow(dict_get(win, "handle", 0), TRUE) != 0
}

fn focus_window(win){
   "Brings a Win32 window to the foreground and focuses it."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   BringWindowToTop(hwnd)
   SetForegroundWindow(hwnd)
   SetFocus(hwnd) != 0
}

fn get_window_attrib(win, attrib){
   "Unified getter for Win32 window attributes matching GLFW constants."
   if(!win || !is_dict(win)){ return 0 }
   match attrib {
      backend_api.RESIZABLE -> { return dict_get(win, "resizable", true) ? 1 : 0 }
      backend_api.VISIBLE -> { return is_window_visible(win) ? 1 : 0 }
      backend_api.DECORATED -> { return dict_get(win, "decorated", true) ? 1 : 0 }
      backend_api.FOCUSED -> { return is_window_focused(win) ? 1 : 0 }
      backend_api.FLOATING -> { return dict_get(win, "floating", false) ? 1 : 0 }
      backend_api.MAXIMIZED -> { return is_window_maximized(win) ? 1 : 0 }
      backend_api.TRANSPARENT_FRAMEBUFFER -> { return dict_get(win, "transparent", false) ? 1 : 0 }
      _ -> { return 0 }
   }
}

fn is_window_focused(win){
   "Returns true if the Win32 window is the active window."
   if(!available() || !win || !is_dict(win)){ return false }
   dict_get(win, "handle", 0) == GetActiveWindow()
}

fn is_window_iconified(win){
   "Returns true if the Win32 window is minimized."
   if(!available() || !win || !is_dict(win)){ return false }
   IsIconic(dict_get(win, "handle", 0)) != 0
}

fn is_window_visible(win){
   "Returns true if the Win32 window is visible."
   if(!available() || !win || !is_dict(win)){ return false }
   IsWindowVisible(dict_get(win, "handle", 0)) != 0
}

fn is_window_maximized(win){
   "Returns true if the Win32 window is maximized."
   if(!available() || !win || !is_dict(win)){ return false }
   IsZoomed(dict_get(win, "handle", 0)) != 0
}

fn is_window_hovered(win){
   "Returns true if the cursor is currently over the Win32 client area."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def pt = calloc(1, 8)
   if(!pt){ return false }
   def ok = GetCursorPos(pt) != 0 && WindowFromPoint(pt) == hwnd
   free(pt)
   ok
}

fn get_window_opacity(win){
   "Returns the current Win32 layered-window opacity."
   if(!available() || !win || !is_dict(win)){ return 1.0 }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return 1.0 }
   def ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE)
   if(!band(ex_style, WS_EX_LAYERED)){ return 1.0 }
   def alpha = calloc(1, 1)
   def flags = calloc(1, 4)
   if(!alpha || !flags){
      if(alpha){ free(alpha) }
      if(flags){ free(flags) }
      return 1.0
   }
   mut out = 1.0
   if(GetLayeredWindowAttributes(hwnd, 0, alpha, flags) != 0 && band(load32(flags, 0), LWA_ALPHA)){
      out = float(load8(alpha, 0) & 255) / 255.0
   }
   free(alpha)
   free(flags)
   out
}

fn set_window_opacity(win, opacity){
   "Applies whole-window opacity using layered window attributes."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   mut ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE)
   if(opacity < 1.0 || band(ex_style, WS_EX_TRANSPARENT)){
      ex_style = bor(ex_style, WS_EX_LAYERED)
      SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style)
      SetLayeredWindowAttributes(hwnd, 0, int(opacity * 255.0), LWA_ALPHA) != 0
   } else {
      ex_style = band(ex_style, bnot(WS_EX_LAYERED))
      SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style)
      true
   }
}

fn get_window_pos(win){
   "Returns the top-left screen coordinates [x, y] of a Win32 window's client area."
   if(!available() || !win || !is_dict(win)){ return [0, 0] }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return [0, 0] }
   def pt = calloc(1, 8)
   if(!pt){ return [0, 0] }
   ClientToScreen(hwnd, pt)
   def x = load32(pt, 0)
   def y = load32(pt, 4)
   free(pt)
   [x, y]
}

fn set_window_pos(win, x, y){
   "Moves a Win32 window so its client area starts at screen coordinates [x, y]."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def style = GetWindowLongW(hwnd, GWL_STYLE)
   def ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE)
   def rect = calloc(1, 16)
   if(!rect){ return false }
   store32(rect, x, 0)
   store32(rect, y, 4)
   store32(rect, x, 8)
   store32(rect, y, 12)
   _adjust_window_rect(rect, style, ex_style)
   def final_x = load32(rect, 0)
   def final_y = load32(rect, 4)
   free(rect)
   SetWindowPos(hwnd, 0, final_x, final_y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER) != 0
}

fn get_window_size(win){
   "Returns the pixel size [width, height] of a Win32 window's client area."
   if(!available() || !win || !is_dict(win)){ return [0, 0] }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return [0, 0] }
   def rect = calloc(1, 16)
   if(!rect){ return [0, 0] }
   GetClientRect(hwnd, rect)
   def w = _rect_width(rect)
   def h = _rect_height(rect)
   free(rect)
   [w, h]
}

fn set_window_size(win, w, h){
   "Resizes a Win32 window so its client area has width `w` and height `h`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def style = GetWindowLongW(hwnd, GWL_STYLE)
   def ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE)
   def rect = calloc(1, 16)
   if(!rect){ return false }
   store32(rect, 0, 0)
   store32(rect, 0, 4)
   store32(rect, w, 8)
   store32(rect, h, 12)
   _adjust_window_rect(rect, style, ex_style)
   def fw = _rect_width(rect)
   def fh = _rect_height(rect)
   free(rect)
   SetWindowPos(hwnd, 0, 0, 0, fw, fh, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER) != 0
}

fn get_framebuffer_size(win){
   "Returns the pixel size [width, height] of the Win32 window's client area."
   get_window_size(win)
}

fn set_window_size_limits(win, min_w, min_h, max_w, max_h){
   "Applies window size constraints tracked via WM_GETMINMAXINFO."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   win = dict_set(win, "min_w", min_w)
   win = dict_set(win, "min_h", min_h)
   win = dict_set(win, "max_w", max_w)
   win = dict_set(win, "max_h", max_h)
   _windows = dict_set(_windows, hwnd, win)
   true
}

fn set_window_resizable(win, enabled){
   "Updates the Win32 resizable style flag."
   if(!available() || !win || !is_dict(win)){ return win }
   win = dict_set(win, "resizable", !!enabled)
   _update_window_styles(win)
}

fn set_window_decorated(win, enabled){
   "Updates the Win32 decorated style flag."
   if(!available() || !win || !is_dict(win)){ return win }
   win = dict_set(win, "decorated", !!enabled)
   _update_window_styles(win)
}

fn set_window_floating(win, enabled){
   "Updates the Win32 topmost state."
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = dict_get(win, "handle", 0)
   if(hwnd){
      SetWindowPos(hwnd, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
         SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE)
   }
   dict_set(win, "floating", !!enabled)
}

fn set_clipboard(win, s){
   "Sets the Win32 clipboard using CF_UNICODETEXT."
   if(!available()){ return false }
   if(!is_str(s)){ s = to_str(s) }
   def count = MultiByteToWideChar(CP_UTF8, 0, cstr(s), -1, 0, 0)
   if(count <= 0){ return false }

   def object = GlobalAlloc(GMEM_MOVEABLE, count * 2)
   if(!object){ return false }
   def buffer = GlobalLock(object)
   if(!buffer){
      GlobalFree(object)
      return false
   }
   if(MultiByteToWideChar(CP_UTF8, 0, cstr(s), -1, buffer, count) == 0){
      GlobalUnlock(object)
      GlobalFree(object)
      return false
   }
   GlobalUnlock(object)

   if(!_open_clipboard_with_retry(_clipboard_hwnd(win))){
      GlobalFree(object)
      return false
   }

   EmptyClipboard()
   def ok = SetClipboardData(CF_UNICODETEXT, object) != 0
   CloseClipboard()
   if(!ok){
      GlobalFree(object)
      return false
   }
   true
}

fn get_clipboard(win){
   "Gets the Win32 clipboard as UTF-8 text."
   if(!available()){ return "" }
   if(!_open_clipboard_with_retry(_clipboard_hwnd(win))){ return "" }
   def object = GetClipboardData(CF_UNICODETEXT)
   if(!object){
      CloseClipboard()
      return ""
   }
   def buffer = GlobalLock(object)
   if(!buffer){
      CloseClipboard()
      return ""
   }
   def out = utf8_from_utf16(buffer)
   GlobalUnlock(object)
   CloseClipboard()
   out
}

fn get_cursor_pos(win){
   "Returns the current cursor position relative to the Win32 client area."
   if(!available() || !win || !is_dict(win)){ return [0.0, 0.0] }
   if(dict_get(win, "disabled_cursor", false)){
      return [float(dict_get(win, "mouse_x", 0)), float(dict_get(win, "mouse_y", 0))]
   }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return [0.0, 0.0] }
   def pt = calloc(1, 8)
   if(!pt){ return [0.0, 0.0] }
   mut out = [float(dict_get(win, "mouse_x", 0)), float(dict_get(win, "mouse_y", 0))]
   if(GetCursorPos(pt) != 0 && ScreenToClient(hwnd, pt) != 0){
      out = [float(load32(pt, 0)), float(load32(pt, 4))]
   }
   free(pt)
   out
}

fn set_cursor_pos(win, x, y){
   "Sets the Win32 cursor position in client-area coordinates."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def pt = calloc(1, 8)
   if(!pt){ return false }
   store32(pt, int(x), 0)
   store32(pt, int(y), 4)
   def ok = ClientToScreen(hwnd, pt) != 0 && SetCursorPos(load32(pt, 0), load32(pt, 4)) != 0
   free(pt)
   ok
}

fn create_standard_cursor(shape){
   "Creates a shared Win32 standard cursor mapped from GLFW cursor shapes."
   if(!available()){ return 0 }
   def rid = case shape {
      backend_api.ARROW_CURSOR -> IDC_ARROW
      backend_api.IBEAM_CURSOR -> IDC_IBEAM
      backend_api.CROSSHAIR_CURSOR -> IDC_CROSS
      backend_api.POINTING_HAND_CURSOR -> IDC_HAND
      backend_api.RESIZE_EW_CURSOR -> IDC_SIZEWE
      backend_api.RESIZE_NS_CURSOR -> IDC_SIZENS
      backend_api.RESIZE_NWSE_CURSOR -> IDC_SIZENWSE
      backend_api.RESIZE_NESW_CURSOR -> IDC_SIZENESW
      backend_api.RESIZE_ALL_CURSOR -> IDC_SIZEALL
      backend_api.NOT_ALLOWED_CURSOR -> IDC_NO
      _ -> 0
   }
   if(rid == 0){ return 0 }
   def handle = LoadImageW(0, _make_int_resource(rid), IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED)
   if(!handle){ return 0 }
   mut cursor = dict()
   cursor = dict_set(cursor, "handle", handle)
   cursor = dict_set(cursor, "shared", true)
   cursor = dict_set(cursor, "shape", shape)
   cursor
}

fn create_cursor(image, xhot=0, yhot=0){
   "Creates a Win32 cursor from a Ny RGBA8 image dictionary."
   if(!available()){ return 0 }
   def handle = _create_rgba_cursor_handle(image, xhot, yhot, false)
   if(!handle){ return 0 }
   mut cursor = dict()
   cursor = dict_set(cursor, "handle", handle)
   cursor = dict_set(cursor, "shared", false)
   cursor = dict_set(cursor, "image", image)
   cursor = dict_set(cursor, "xhot", xhot)
   cursor = dict_set(cursor, "yhot", yhot)
   cursor
}

fn destroy_cursor(cursor){
   "Destroys a Win32 cursor object when it owns native resources."
   if(!cursor || !is_dict(cursor)){ return true }
   if(!dict_get(cursor, "shared", false)){
      def handle = dict_get(cursor, "handle", 0)
      if(handle){ DestroyIcon(handle) }
   }
   true
}

fn set_cursor(win, cursor){
   "Applies a cursor object to a Win32 window dictionary."
   if(!win || !is_dict(win)){ return win }
   win = dict_set(win, "cursor", cursor)
   _apply_cursor_handle(win)
}

fn set_input_mode(win, mode, value){
   "Applies GLFW-style cursor and raw-mouse modes to a Win32 window dictionary."
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return win }

   if(mode == backend_api.RAW_MOUSE_MOTION){
      win = dict_set(win, "raw_mouse_motion", value != 0)
      if(dict_get(win, "disabled_cursor", false)){
         if(value != 0){ _enable_raw_mouse_motion(hwnd) }
         else { _disable_raw_mouse_motion() }
      }
      return win
   }
   if(mode != backend_api.CURSOR){ return win }

   def previous = dict_get(win, "cursor_mode", backend_api.CURSOR_NORMAL)
   if(previous == backend_api.CURSOR_DISABLED && dict_get(win, "raw_mouse_motion", false)){
      _disable_raw_mouse_motion()
   }

   if(value == backend_api.CURSOR_NORMAL){
      _release_cursor()
      _show_cursor(true)
      win = _apply_cursor_handle(win)
      if(previous == backend_api.CURSOR_DISABLED){
         set_cursor_pos(win,
         dict_get(win, "restore_cursor_x", dict_get(win, "mouse_x", 0)),
         dict_get(win, "restore_cursor_y", dict_get(win, "mouse_y", 0)))
      }
      win = dict_set(win, "captured_cursor", false)
      win = dict_set(win, "disabled_cursor", false)
      win = dict_set(win, "cursor_visible", true)
      return dict_set(win, "cursor_mode", value)
   }

   if(value == backend_api.CURSOR_HIDDEN){
      _release_cursor()
      _show_cursor(false)
      win = dict_set(win, "captured_cursor", false)
      win = dict_set(win, "disabled_cursor", false)
      win = dict_set(win, "cursor_visible", false)
      return dict_set(win, "cursor_mode", value)
   }

   if(value == backend_api.CURSOR_CAPTURED){
      _capture_cursor(hwnd)
      _show_cursor(true)
      win = _apply_cursor_handle(win)
      win = dict_set(win, "captured_cursor", true)
      win = dict_set(win, "disabled_cursor", false)
      win = dict_set(win, "cursor_visible", true)
      return dict_set(win, "cursor_mode", value)
   }

   if(value == backend_api.CURSOR_DISABLED){
      def pos = get_cursor_pos(win)
      def center_x = int(dict_get(win, "w", 1) / 2)
      def center_y = int(dict_get(win, "h", 1) / 2)
      win = dict_set(win, "restore_cursor_x", int(get(pos, 0, 0.0)))
      win = dict_set(win, "restore_cursor_y", int(get(pos, 1, 0.0)))
      win = dict_set(win, "mouse_x", int(get(pos, 0, 0.0)))
      win = dict_set(win, "mouse_y", int(get(pos, 1, 0.0)))
      _capture_cursor(hwnd)
      _show_cursor(false)
      if(dict_get(win, "raw_mouse_motion", false)){ _enable_raw_mouse_motion(hwnd) }
      set_cursor_pos(win, center_x, center_y)
      win = dict_set(win, "last_cursor_client_x", center_x)
      win = dict_set(win, "last_cursor_client_y", center_y)
      win = dict_set(win, "captured_cursor", true)
      win = dict_set(win, "disabled_cursor", true)
      win = dict_set(win, "cursor_visible", false)
      return dict_set(win, "cursor_mode", value)
   }

   win
}

fn _message_to_dict(msg){
   mut out = dict()
   out = dict_set(out, "hwnd", load64_h(msg, 0))
   out = dict_set(out, "message", load32(msg, 8))
   out = dict_set(out, "wparam", load64_h(msg, 16))
   out = dict_set(out, "lparam", load64_h(msg, 24))
   out = dict_set(out, "time", load32(msg, 32))
   out = dict_set(out, "x", load32(msg, 36))
   out = dict_set(out, "y", load32(msg, 40))
   out
}

fn _synthetic_key_release(win, events, vk, key){
   if(!win || !is_dict(win)){ return [win, events] }
   if(GetKeyState(vk) & 0x8000){ return [win, events] }
   mut ks = dict_get(win, "key_states", dict(64))
   if(!dict_get(ks, key, false)){ return [win, events] }
   ks = dict_set(ks, key, false)
   win = dict_set(win, "key_states", ks)
   mut data = dict()
   data = dict_set(data, "raw_key", vk)
   data = dict_set(data, "key", key)
   data = dict_set(data, "scancode", MapVirtualKeyW(vk, MAPVK_VK_TO_VSC))
   data = dict_set(data, "action", backend_api.ACTION_RELEASE)
   data = dict_set(data, "mods", _mods_for_key_event(win, key, false))
   _push_event(events, EVENT_KEY_RELEASED, win, data)
   [win, events]
}

fn _sync_special_key_releases(win, events){
   "Mirrors GLFW's Win32 poll cleanup for Shift/Super keys that Windows may fail to release."
   if(!win || !is_dict(win) || GetActiveWindow() != dict_get(win, "handle", 0)){ return [win, events] }
   def left_shift = _synthetic_key_release(win, events, VK_LSHIFT, backend_api.KEY_LEFT_SHIFT)
   win = get(left_shift, 0, win)
   events = get(left_shift, 1, events)
   def right_shift = _synthetic_key_release(win, events, VK_RSHIFT, backend_api.KEY_RIGHT_SHIFT)
   win = get(right_shift, 0, win)
   events = get(right_shift, 1, events)
   def left_super = _synthetic_key_release(win, events, VK_LWIN, backend_api.KEY_LEFT_SUPER)
   win = get(left_super, 0, win)
   events = get(left_super, 1, events)
   def right_super = _synthetic_key_release(win, events, VK_RWIN, backend_api.KEY_RIGHT_SUPER)
   win = get(right_super, 0, win)
   events = get(right_super, 1, events)
   [win, events]
}

fn _recenter_disabled_cursor(win){
   "Recenters the disabled cursor after queue drain, matching GLFW's Win32 poll loop."
   if(!win || !is_dict(win) || !dict_get(win, "disabled_cursor", false)){ return win }
   def center_x = int(max(1, dict_get(win, "w", 1)) / 2)
   def center_y = int(max(1, dict_get(win, "h", 1)) / 2)
   if(dict_get(win, "last_cursor_client_x", center_x) != center_x ||
      dict_get(win, "last_cursor_client_y", center_y) != center_y){
      set_cursor_pos(win, center_x, center_y)
      win = dict_set(win, "last_cursor_client_x", center_x)
      win = dict_set(win, "last_cursor_client_y", center_y)
   }
   win
}

fn wait_messages(timeout_ms=-1){
   "Blocks until a new message is posted to the thread's queue or timeout elapses."
   if(!available()){ return false }
   def ms = (timeout_ms < 0) ? 0xffffffff : int(timeout_ms)
   MsgWaitForMultipleObjects(0, 0, 0, ms, 0x04FF) ;; 0x04FF = QS_ALLINPUT
   true
}

fn poll_messages(hwnd=0, max_messages=64, remove=true){
   "Polls raw Win32 messages into a list of dicts."
   if(!available()){ return [] }
   def msg = calloc(1, 48)
   if(!msg){ return [] }
   mut out = []
   mut n = 0
   while(n < max_messages && PeekMessageW(msg, hwnd, 0, 0, remove ? PM_REMOVE : PM_NOREMOVE) != 0){
      append(out, _message_to_dict(msg))
      n += 1
      if(!remove){ break }
   }
   free(msg)
   out
}

fn pump_window_messages(hwnd=0, max_messages=64){
   "Polls and dispatches queued Win32 messages."
   if(!available()){ return [] }
   def msg = calloc(1, 48)
   if(!msg){ return [] }
   mut out = []
   mut n = 0
   while(n < max_messages && PeekMessageW(msg, hwnd, 0, 0, PM_REMOVE) != 0){
      append(out, _message_to_dict(msg))
      TranslateMessage(msg)
      DispatchMessageW(msg)
      n += 1
   }
   free(msg)
   out
}

fn poll_window_events(win, max_messages=64){
   "Polls and translates Win32 messages for a backend window."
   if(!available() || !win || !is_dict(win)){ return [win, []] }
   def messages = pump_window_messages(dict_get(win, "handle", 0), max_messages)
   def res = translate_messages(win, messages)
   win = get(res, 0, win)
   mut events = get(res, 1, [])
   def synced = _sync_special_key_releases(win, events)
   win = get(synced, 0, win)
   events = get(synced, 1, events)
   win = _recenter_disabled_cursor(win)
   [win, events]
}

fn primary_screen_size(){
   "Returns the primary screen size using GetSystemMetrics."
   if(!available()){ return [0, 0] }
   [GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)]
}

fn get_monitors(){
   "Enumerates active Win32 display devices as monitor dicts."
   if(!available()){ return [] }
   mut primary = []
   mut others = []
   mut adapter_index = 0
   while(true){
      def adapter = _alloc_display_device()
      if(!adapter){ break }
      if(EnumDisplayDevicesW(0, adapter_index, adapter, 0) == 0){
         free(adapter)
         break
      }
      adapter_index += 1
      if(!band(load32(adapter, 324), DISPLAY_DEVICE_ACTIVE)){
         free(adapter)
         continue
      }

      mut display_index = 0
      mut found_display = false
      while(true){
         def display = _alloc_display_device()
         if(!display){ break }
         if(EnumDisplayDevicesW(adapter + 4, display_index, display, 0) == 0){
         free(display)
         break
         }
         display_index += 1
         if(!band(load32(display, 324), DISPLAY_DEVICE_ACTIVE)){
         free(display)
         continue
         }
         found_display = true
         def monitor = _create_monitor_from_devices(adapter, display)
         free(display)
         if(!monitor){ continue }
         if(dict_get(monitor, "primary", false)){ primary = append(primary, monitor) }
         else { others = append(others, monitor) }
      }

      if(!found_display){
         def monitor = _create_monitor_from_devices(adapter, 0)
         if(monitor){
         if(dict_get(monitor, "primary", false)){ primary = append(primary, monitor) }
         else { others = append(others, monitor) }
         }
      }
      free(adapter)
   }

   mut out = extend(primary, others)
   mut i = 0
   while(i < len(out)){
      def monitor = get(out, i)
      if(monitor && is_dict(monitor)){
         out = set(out, i, dict_set(monitor, "index", i))
      }
      i += 1
   }
   out
}

fn get_primary_monitor(){
   "Returns the primary Win32 monitor dict."
   def monitors = get_monitors()
   if(len(monitors) == 0){ return false }
   get(monitors, 0)
}

fn get_monitor_pos(monitor){
   "Returns `[x, y]` for a monitor dict."
   if(!monitor || !is_dict(monitor)){ return [0, 0] }
   [dict_get(monitor, "x", 0), dict_get(monitor, "y", 0)]
}

fn get_monitor_workarea(monitor){
   "Returns `[x, y, width, height]` for a monitor work area."
   if(!monitor || !is_dict(monitor)){ return [0, 0, 0, 0] }
   [
      dict_get(monitor, "work_x", dict_get(monitor, "x", 0)),
      dict_get(monitor, "work_y", dict_get(monitor, "y", 0)),
      dict_get(monitor, "work_w", dict_get(monitor, "width", 0)),
      dict_get(monitor, "work_h", dict_get(monitor, "height", 0))
   ]
}

fn get_monitor_physical_size(monitor){
   "Returns `[width_mm, height_mm]` for a monitor dict."
   if(!monitor || !is_dict(monitor)){ return [0, 0] }
   [dict_get(monitor, "width_mm", 0), dict_get(monitor, "height_mm", 0)]
}

fn get_monitor_content_scale(monitor){
   "Returns `[xscale, yscale]` for a monitor dict."
   if(!monitor || !is_dict(monitor)){ return [1.0, 1.0] }
   [dict_get(monitor, "scale_x", 1.0), dict_get(monitor, "scale_y", 1.0)]
}

fn get_monitor_name(monitor){
   "Returns the UTF-8 display name for a monitor dict."
   if(!monitor || !is_dict(monitor)){ return "" }
   dict_get(monitor, "name", "")
}

fn get_video_mode(monitor){
   "Returns the current GLFW-style video mode dict for a monitor."
   if(!available() || !monitor || !is_dict(monitor)){ return false }
   def adapter = dict_get(monitor, "adapter_name", "")
   if(len(adapter) == 0){ return false }
   def wide = utf16_from_utf8(adapter)
   def dm = _alloc_devmode()
   if(!wide || !dm){
      if(wide){ free(wide) }
      if(dm){ free(dm) }
      return false
   }
   if(EnumDisplaySettingsW(wide, ENUM_CURRENT_SETTINGS, dm) == 0){
      free(wide)
      free(dm)
      return false
   }
   def mode = _devmode_to_vidmode(dm)
   free(wide)
   free(dm)
   mode
}

fn get_video_modes(monitor){
   "Returns distinct GLFW-style video modes for a monitor."
   if(!available() || !monitor || !is_dict(monitor)){ return [] }
   def adapter = dict_get(monitor, "adapter_name", "")
   if(len(adapter) == 0){ return [] }
   def wide = utf16_from_utf8(adapter)
   if(!wide){ return [] }
   mut out = []
   mut seen = dict()
   mut index = 0
   while(true){
      def dm = _alloc_devmode()
      if(!dm){ break }
      if(EnumDisplaySettingsW(wide, index, dm) == 0){
         free(dm)
         break
      }
      index += 1
      if(load32(dm, 168) < 15){
         free(dm)
         continue
      }
      def mode = _devmode_to_vidmode(dm)
      def mw = dict_get(mode, "width", 0)
      def mh = dict_get(mode, "height", 0)
      def mr = dict_get(mode, "refresh_rate", 0)
      def mred = dict_get(mode, "red_bits", 0)
      def mgreen = dict_get(mode, "green_bits", 0)
      def mblue = dict_get(mode, "blue_bits", 0)
      def key = f"{mw}x{mh}@{mr}:{mred}/{mgreen}/{mblue}"
      if(!dict_get(seen, key, false)){
         seen = dict_set(seen, key, true)
         out = append(out, mode)
      }
      free(dm)
   }
   free(wide)
   if(len(out) == 0){
      def current = get_video_mode(monitor)
      if(current){ out = append(out, current) }
   }
   out
}

fn get_gamma_ramp(monitor){
   "Returns the current Win32 gamma ramp as a dict with `size`, `red`, `green`, and `blue`."
   if(!available() || !monitor || !is_dict(monitor)){ return false }
   def adapter = dict_get(monitor, "adapter_name", "")
   if(len(adapter) == 0){ return false }
   def adapter_wide = utf16_from_utf8(adapter)
   def driver = _driver_display_wide()
   def values = calloc(1, 1536)
   if(!adapter_wide || !driver || !values){
      if(adapter_wide){ free(adapter_wide) }
      if(driver){ free(driver) }
      if(values){ free(values) }
      return false
   }
   def dc = CreateDCW(driver, adapter_wide, 0, 0)
   free(driver)
   free(adapter_wide)
   if(!dc){
      free(values)
      return false
   }
   def ok = GetDeviceGammaRamp(dc, values) != 0
   DeleteDC(dc)
   if(!ok){
      free(values)
      return false
   }
   def ramp = _gamma_ramp_from_buffer(values, 256)
   free(values)
   ramp
}

fn set_gamma_ramp(monitor, ramp){
   "Sets the Win32 gamma ramp from a dict containing `size`, `red`, `green`, and `blue` arrays."
   if(!available() || !monitor || !is_dict(monitor) || !ramp || !is_dict(ramp)){ return false }
   def size = dict_get(ramp, "size", 0)
   if(size != 256){ return false }
   def values = calloc(1, 1536)
   if(!values){ return false }
   def red = dict_get(ramp, "red", [])
   def green = dict_get(ramp, "green", [])
   def blue = dict_get(ramp, "blue", [])
   if(!_copy_gamma_channel(red, values, 0, size) ||
      !_copy_gamma_channel(green, values, 512, size) ||
      !_copy_gamma_channel(blue, values, 1024, size)){
      free(values)
      return false
   }
   def adapter = dict_get(monitor, "adapter_name", "")
   def adapter_wide = utf16_from_utf8(adapter)
   def driver = _driver_display_wide()
   if(!adapter_wide || !driver){
      if(adapter_wide){ free(adapter_wide) }
      if(driver){ free(driver) }
      free(values)
      return false
   }
   def dc = CreateDCW(driver, adapter_wide, 0, 0)
   free(driver)
   free(adapter_wide)
   if(!dc){
      free(values)
      return false
   }
   def ok = SetDeviceGammaRamp(dc, values) != 0
   DeleteDC(dc)
   free(values)
   ok
}

fn get_window_monitor(win){
   "Returns the monitor dict nearest the specified Win32 window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def handle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
   if(!handle){ return false }
   def monitors = get_monitors()
   mut i = 0
   while(i < len(monitors)){
      def monitor = get(monitors, i)
      if(monitor && is_dict(monitor) && dict_get(monitor, "handle", 0) == handle){ return monitor }
      i += 1
   }
   false
}

fn set_window_monitor(win, monitor, xpos, ypos, width, height, refresh_rate=0){
   "Applies a basic GLFW-style monitor/fullscreen transition for Win32."
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return win }

   if(dict_get(win, "monitor_mode_changed", false)){
      _restore_video_mode(dict_get(win, "monitor_adapter_name", ""))
      win = dict_set(win, "monitor_mode_changed", false)
   }

   if(monitor && is_dict(monitor)){
      if(!dict_get(win, "monitor", false)){
         win = dict_set(win, "windowed_x", dict_get(win, "x", xpos))
         win = dict_set(win, "windowed_y", dict_get(win, "y", ypos))
         win = dict_set(win, "windowed_w", dict_get(win, "w", width))
         win = dict_set(win, "windowed_h", dict_get(win, "h", height))
      }
      mut mode = get_video_mode(monitor)
      def mode_changed = _set_video_mode(monitor, width, height, refresh_rate)
      if(mode_changed){ mode = get_video_mode(monitor) }
      def mon_x = dict_get(monitor, "x", xpos)
      def mon_y = dict_get(monitor, "y", ypos)
      def mon_w = max(width, dict_get(mode, "width", dict_get(monitor, "width", width)))
      def mon_h = max(height, dict_get(mode, "height", dict_get(monitor, "height", height)))
      win = dict_set(win, "monitor", monitor)
      win = dict_set(win, "monitor_adapter_name", dict_get(monitor, "adapter_name", ""))
      win = dict_set(win, "monitor_mode_changed", mode_changed)
      win = dict_set(win, "fullscreen", true)
      win = dict_set(win, "flags", bor(int(dict_get(win, "flags", 0)), WINDOW_FULLSCREEN))
      win = _update_window_styles(win)
      SetWindowPos(hwnd, HWND_TOPMOST, mon_x, mon_y, mon_w, mon_h, SWP_FRAMECHANGED | SWP_SHOWWINDOW)
      win = dict_set(win, "x", mon_x)
      win = dict_set(win, "y", mon_y)
      win = dict_set(win, "w", mon_w)
      win = dict_set(win, "h", mon_h)
      return win
   }

   win = dict_set(win, "monitor", false)
   win = dict_set(win, "monitor_adapter_name", "")
   win = dict_set(win, "monitor_mode_changed", false)
   win = dict_set(win, "fullscreen", false)
   win = dict_set(win, "flags", band(int(dict_get(win, "flags", 0)), bnot(WINDOW_FULLSCREEN)))
   win = _update_window_styles(win)
   def rx = dict_get(win, "windowed_x", xpos)
   def ry = dict_get(win, "windowed_y", ypos)
   def rw = max(1, dict_get(win, "windowed_w", width))
   def rh = max(1, dict_get(win, "windowed_h", height))
   SetWindowPos(hwnd, HWND_NOTOPMOST, rx, ry, rw, rh, SWP_FRAMECHANGED | SWP_SHOWWINDOW)
   win = dict_set(win, "x", rx)
   win = dict_set(win, "y", ry)
   win = dict_set(win, "w", rw)
   win = dict_set(win, "h", rh)
   win
}

fn vulkan_supported(){
   "Returns true when the Win32 backend supports Vulkan."
   true
}

mut _win32_vk_ext_ptrs = 0
mut _win32_vk_ext_surface = 0
mut _win32_vk_ext_win32 = 0

fn vulkan_required_extensions(){
   "Returns the Vulkan instance extensions required for a Win32 surface."
   if(!_win32_vk_ext_ptrs){
      _win32_vk_ext_surface = cstr("VK_KHR_surface")
      _win32_vk_ext_win32 = cstr("VK_KHR_win32_surface")
      def arr = malloc(16)
      store64_h(arr, _win32_vk_ext_surface, 0)
      store64_h(arr, _win32_vk_ext_win32, 8)
      _win32_vk_ext_ptrs = [2, arr]
   }
   _win32_vk_ext_ptrs
}

fn create_surface(instance, win, allocator, surface){
   "Creates a Vulkan Win32 surface from a native Win32 window."
   if(!available()){ return -1 }
   def hwnd = is_dict(win) ? dict_get(win, "handle", 0) : win
   if(!instance || !hwnd || !surface){ return -1 }
   def info = calloc(1, 48)
   if(!info){ return -1 }
   store32(info, VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0)
   store64_h(info, is_dict(win) ? dict_get(win, "instance", GetModuleHandleW(0)) : GetModuleHandleW(0), 24)
   store64_h(info, hwnd, 32)
   def res = vkCreateWin32SurfaceKHR(instance, info, allocator, surface)
   free(info)
   res
}

fn set_video_mode(monitor, width, height, refresh_rate=0){
   "Public wrapper: sets video mode on a Win32 monitor via ChangeDisplaySettingsExW."
   _set_video_mode(monitor, width, height, refresh_rate)
}

fn restore_video_mode(monitor){
   "Public wrapper: restores the original video mode on a Win32 monitor."
   if(!monitor || !is_dict(monitor)){ return false }
   _restore_video_mode(dict_get(monitor, "adapter_name", ""))
}

fn get_win32_window(win){
   "Returns the Win32 HWND handle for the given window dict."
   if(is_dict(win)){ dict_get(win, "handle", 0) } else { win }
}

fn get_win32_monitor(mon){
   "Returns the Win32 HMONITOR handle for the given monitor dict."
   if(is_dict(mon)){ dict_get(mon, "handle", 0) } else { 0 }
}

mut _adapter_wide_cache = dict(4)
fn get_win32_adapter(mon){
   "Returns a wide-string pointer to the Win32 adapter device name for the given monitor."
   if(!is_dict(mon)){ return 0 }
   def name = dict_get(mon, "adapter_name", "")
   if(len(name) == 0){ return 0 }
   def cached = dict_get(_adapter_wide_cache, name, 0)
   if(cached){ return cached }
   def wide = utf16_from_utf8(name)
   _adapter_wide_cache = dict_set(_adapter_wide_cache, name, wide)
   wide
}
