;; Keywords: platform window backend win32 windows joystick
;; Win32 native window backend for Windows windows, events, monitors, clipboard, and Vulkan surfaces.
module std.os.ui.window.platform.win32(available, get_backend_name, module_handle, primary_screen_size, init_dpi_awareness, init_timer, get_timer_value, get_timer_frequency, utf16_from_utf8, utf8_from_utf16, translate_vk_key, translate_message, translate_messages, get_key_state, get_mouse_button_state, get_key_name, get_window_style, get_window_ex_style, ensure_window_class, create_basic_window, destroy_basic_window, show_window, hide_window, set_title, set_window_icon, get_pos, set_pos, get_size, get_framebuffer_size, set_size, iconify_window, restore_window, maximize_window, focus_window, request_window_attention, get_window_attrib, is_window_focused, is_window_iconified, is_window_visible, is_window_maximized, is_window_hovered, get_window_opacity, set_window_opacity, set_window_resizable, set_window_decorated, set_window_floating, set_clipboard, get_clipboard, get_cursor_pos, set_cursor_pos, create_cursor, create_standard_cursor, destroy_cursor, set_cursor, get_monitors, get_primary_monitor, get_monitor_pos, get_monitor_workarea, get_monitor_physical_size, get_monitor_content_scale, get_monitor_name, get_video_mode, get_video_modes, get_gamma_ramp, set_gamma_ramp, get_window_monitor, set_window_monitor, set_input_mode, set_window_size_limits, poll_messages, pump_window_messages, poll_window_events, vulkan_supported, vulkan_required_extensions, create_surface, set_video_mode, restore_video_mode, get_win32_window, get_win32_monitor, get_win32_adapter)
use std.core
use std.core.mem (cstr)
use std.core.str as str
use std.os.ui.window.platform.api as backend_api
use std.os.ui.consts as ui_consts
use std.os.ui.window.event as ui_event
use std.core.common as common
use std.math (abs)

fn _handle_from(any: v): any {
   if(is_dict(v)){ return v.get("handle", 0) }
   v
}

fn _instance_from_window(any: win): any {
   if(is_dict(win)){ return win.get("instance", GetModuleHandleW(0)) }
   GetModuleHandleW(0)
}

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
mut _windows = dict(8)
mut _timer_frequency = 0
mut _dpi_awareness_done = false
mut _known_monitor_names = []
mut _monitors_initialized = false
#windows {
   #include <windows.h>
   #include <dwmapi.h>
   #include <shellapi.h>
   #include <shcore.h>
   #include <imm.h>
   #include <vulkan/vulkan_win32.h>
} #else {
   fn AdjustWindowRectEx(any: _rect, any: _style, any: _menu, any: _ex_style): any { 0 }
   fn AdjustWindowRectExForDpi(any: _rect, any: _style, any: _menu, any: _ex_style, any: _dpi): any { 0 }
   fn BringWindowToTop(any: _hwnd): any { 0 }
   fn ChangeDisplaySettingsExW(any: _device, any: _mode, any: _hwnd, any: _flags, any: _param): any { -1 }
   fn ChangeWindowMessageFilterEx(any: _hwnd, any: _msg, any: _action, any: _change): any { 0 }
   fn ClientToScreen(any: _hwnd, any: _pt): any { 0 }
   fn ClipCursor(any: _rect): any { 0 }
   fn CloseClipboard(): any { 0 }
   fn CreateBitmap(any: _w, any: _h, any: _planes, any: _bits, any: _data): any { 0 }
   fn CreateDCW(any: _driver, any: _device, any: _output, any: _init): any { 0 }
   fn CreateDIBSection(any: _dc, any: _bmi, any: _usage, any: _bits, any: _section, any: _offset): any { 0 }
   fn CreateIconIndirect(any: _icon_info): any { 0 }
   fn CreateRectRgn(any: _left, any: _top, any: _right, any: _bottom): any { 0 }
   fn CreateWindowExW(any: _ex, any: _class, any: _title, any: _style, any: _x, any: _y, any: _w, any: _h, any: _parent, any: _menu, any: _instance, any: _param): any { 0 }
   fn DefWindowProcW(any: _hwnd, any: _msg, any: _wparam, any: _lparam): any { 0 }
   fn DeleteDC(any: _dc): any { 0 }
   fn DeleteObject(any: _obj): any { 0 }
   fn DestroyIcon(any: _icon): any { 0 }
   fn DestroyWindow(any: _hwnd): any { 0 }
   fn DispatchMessageW(any: _msg): any { 0 }
   fn DragAcceptFiles(any: _hwnd, any: _accept): any { 0 }
   fn DragFinish(any: _drop): any { 0 }
   fn DragQueryFileW(any: _drop, any: _index, any: _buf, any: _len): any { 0 }
   fn DragQueryPoint(any: _drop, any: _pt): any { 0 }
   fn DwmEnableBlurBehindWindow(any: _hwnd, any: _blur): any { 0 }
   fn DwmGetColorizationColor(any: _color, any: _opaque): any { 1 }
   fn DwmIsCompositionEnabled(any: _enabled): any { 1 }
   fn EmptyClipboard(): any { 0 }
   fn EnumDisplayDevicesW(any: _device, any: _index, any: _adapter, any: _flags): any { 0 }
   fn EnumDisplaySettingsW(any: _device, any: _mode, any: _devmode): any { 0 }
   fn FlashWindow(any: _hwnd, any: _invert): any { 0 }
   fn GetActiveWindow(): any { 0 }
   fn GetClassLongPtrW(any: _hwnd, any: _index): any { 0 }
   fn GetClientRect(any: _hwnd, any: _rect): any { 0 }
   fn GetClipboardData(any: _format): any { 0 }
   fn GetCursorPos(any: _pt): any { 0 }
   fn GetDC(any: _hwnd): any { 0 }
   fn GetDeviceCaps(any: _dc, any: _index): any { 0 }
   fn GetDeviceGammaRamp(any: _dc, any: _ramp): any { 0 }
   fn GetDpiForMonitor(any: _monitor, any: _typ, any: _x, any: _y): any { 1 }
   fn GetDpiForWindow(any: _hwnd): any { USER_DEFAULT_SCREEN_DPI }
   fn GetKeyNameTextW(any: _lparam, any: _buf, any: _len): any { 0 }
   fn GetKeyState(any: _key): any { 0 }
   fn GetLayeredWindowAttributes(any: _hwnd, any: _key, any: _alpha, any: _flags): any { 0 }
   fn GetModuleHandleA(any: _name=0): any { 0 }
   fn GetModuleHandleW(any: _name=0): any { 0 }
   fn GetMonitorInfoW(any: _monitor, any: _info): any { 0 }
   fn GetRawInputData(any: _raw, any: _cmd, any: _data, any: _size, any: _header): any { 0xffffffff }
   fn GetSystemMetrics(any: _index): any { 0 }
   fn GetSystemMetricsForDpi(any: _index, any: _dpi): any { 0 }
   fn GetWindowLongW(any: _hwnd, any: _index): any { 0 }
   fn GlobalAlloc(any: _flags, any: _bytes): any { 0 }
   fn GlobalFree(any: _obj): any { 0 }
   fn GlobalLock(any: _obj): any { 0 }
   fn GlobalUnlock(any: _obj): any { 0 }
   fn ImmGetCompositionStringW(any: _ctx, any: _index, any: _buf, any: _len): any { 0 }
   fn ImmGetContext(any: _hwnd): any { 0 }
   fn ImmReleaseContext(any: _hwnd, any: _ctx): any { 0 }
   fn IsIconic(any: _hwnd): any { 0 }
   fn IsWindowVisible(any: _hwnd): any { 0 }
   fn IsZoomed(any: _hwnd): any { 0 }
   fn LoadCursorW(any: _inst, any: _name): any { 0 }
   fn LoadImageW(any: _inst, any: _name, any: _typ, any: _cx, any: _cy, any: _flags): any { 0 }
   fn MapVirtualKeyW(any: _code, any: _map_type): any { 0 }
   fn MonitorFromRect(any: _rect, any: _flags): any { 0 }
   fn MonitorFromWindow(any: _hwnd, any: _flags): any { 0 }
   fn MsgWaitForMultipleObjects(any: _count, any: _handles, any: _wait_all, any: _ms, any: _mask): any { 0 }
   fn MultiByteToWideChar(any: _cp, any: _flags, any: _src, any: _src_len, any: _dst, any: _dst_len): any { 0 }
   fn OpenClipboard(any: _hwnd): any { 0 }
   fn PeekMessageW(any: _msg, any: _hwnd, any: _min, any: _max, any: _remove): any { 0 }
   fn QueryPerformanceCounter(any: _value): any { 0 }
   fn QueryPerformanceFrequency(any: _value): any { 0 }
   fn RegisterClassExW(any: _wc): any { 0 }
   fn RegisterRawInputDevices(any: _rid, any: _count, any: _size): any { 0 }
   fn ReleaseDC(any: _hwnd, any: _dc): any { 0 }
   fn ScreenToClient(any: _hwnd, any: _pt): any { 0 }
   fn SendMessageW(any: _hwnd, any: _msg, any: _wparam, any: _lparam): any { 0 }
   fn SetClipboardData(any: _format, any: _obj): any { 0 }
   fn SetCursor(any: _cursor): any { 0 }
   fn SetCursorPos(any: _x, any: _y): any { 0 }
   fn SetDeviceGammaRamp(any: _dc, any: _ramp): any { 0 }
   fn SetFocus(any: _hwnd): any { 0 }
   fn SetForegroundWindow(any: _hwnd): any { 0 }
   fn SetLayeredWindowAttributes(any: _hwnd, any: _key, any: _alpha, any: _flags): any { 0 }
   fn SetProcessDPIAware(): any { 0 }
   fn SetProcessDpiAwareness(any: _value): any { 1 }
   fn SetProcessDpiAwarenessContext(any: _value): any { 0 }
   fn SetPropW(any: _hwnd, any: _name, any: _value): any { 0 }
   fn SetWindowLongW(any: _hwnd, any: _index, any: _value): any { 0 }
   fn SetWindowPos(any: _hwnd, any: _after, any: _x, any: _y, any: _w, any: _h, any: _flags): any { 0 }
   fn SetWindowTextW(any: _hwnd, any: _text): any { 0 }
   fn ShowCursor(any: _show): any { 0 }
   fn ShowWindow(any: _hwnd, any: _cmd): any { 0 }
   fn Sleep(any: _ms): any { 0 }
   fn TranslateMessage(any: _msg): any { 0 }
   fn vkCreateWin32SurfaceKHR(any: _instance, any: _info, any: _allocator, any: _surface): any { -1 }
   fn WideCharToMultiByte(any: _cp, any: _flags, any: _src, any: _src_len, any: _dst, any: _dst_len, any: _def, any: _used): any { 0 }
   fn WindowFromPoint(any: _pt): any { 0 }
}

fn available(): bool {
   "Returns true when the process is running on Win32."
   #windows { return true }
   false
}

fn get_backend_name(): str {
   "Identifies this backend entry module."
   "win32"
}

fn init_dpi_awareness(): bool {
   "Initialize Win32 process DPI-awareness."
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

fn init_timer(): bool {
   "Initializes the high-resolution Win32 timer frequency."
   if(!available()){ return false }
   if(_timer_frequency > 0){ return _timer_frequency > 0 }
   def value = zalloc(8)
   if(!value){ return false }
   def ok = QueryPerformanceFrequency(value) != 0
   if(ok){ _timer_frequency = load64_h(value, 0) }
   free(value)
   ok
}

fn get_timer_value(): int {
   "Returns the current Win32 performance counter value."
   if(!available()){ return 0 }
   def value = zalloc(8)
   if(!value){ return 0 }
   mut out = 0
   if(QueryPerformanceCounter(value) != 0){ out = load64_h(value, 0) }
   free(value)
   out
}

fn get_timer_frequency(): int {
   "Returns the Win32 performance counter frequency."
   if(!available()){ return 0 }
   if(_timer_frequency <= 0){ init_timer() }
   _timer_frequency
}

fn module_handle(any: module_name=0): any {
   "Returns the Win32 HMODULE for the current process or the named module."
   if(!available()){ return 0 }
   if(module_name && is_str(module_name) && module_name.len > 0){ return GetModuleHandleA(cstr(module_name)) }
   GetModuleHandleA(0)
}

fn _default_window_proc(ptr: hwnd, u32: msg, u64: wparam, i64: lparam): i64 {
   if(!available()){ return 0 }
   if(msg == WM_GETMINMAXINFO){
      def win = _windows.get(hwnd, 0)
      if(win){
         def min_w, min_h = win.get("min_w", -1), win.get("min_h", -1)
         def max_w, max_h = win.get("max_w", -1), win.get("max_h", -1)
         if(min_w >= 0){ store32(lparam, min_w, 24) } ;; ptMinTrackSize.x
         if(min_h >= 0){ store32(lparam, min_h, 28) } ;; ptMinTrackSize.y
         if(max_w >= 0){ store32(lparam, max_w, 16) } ;; ptMaxTrackSize.x
         if(max_h >= 0){ store32(lparam, max_h, 20) } ;; ptMaxTrackSize.y
         return 0
      }
   }
   DefWindowProcW(hwnd, msg, wparam, lparam)
}

fn utf16_from_utf8(any: s): any {
   "Returns a heap-allocated UTF-16LE string buffer for a UTF-8 Ny string."
   if(!available()){ return 0 }
   if(!is_str(s)){ s = to_str(s) }
   def count = MultiByteToWideChar(CP_UTF8, 0, cstr(s), -1, 0, 0)
   if(count <= 0){ return 0 }
   def out = zalloc(count * 2)
   if(!out){ return 0 }
   if(MultiByteToWideChar(CP_UTF8, 0, cstr(s), -1, out, count) == 0){
      free(out)
      return 0
   }
   out
}

fn utf8_from_utf16(any: wide): str {
   "Returns a UTF-8 Ny string converted from a NUL-terminated UTF-16 buffer."
   if(!available() || !wide){ return "" }
   def size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, 0, 0, 0, 0)
   if(size <= 0){ return "" }
   def out = zalloc(size)
   if(!out){ return "" }
   if(WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, size, 0, 0) == 0){
      free(out)
      return ""
   }
   def s = str.cstr_to_str(out)
   free(out)
   s
}

fn get_window_style(int: flags=0, bool: monitor=false): int {
   "Select the Win32 window style for the requested backend hints."
   mut style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN
   if(monitor || band(flags, ui_consts.WINDOW_FULLSCREEN)){ style = bor(style, WS_POPUP) } else {
      style = bor(style, WS_SYSMENU | WS_MINIMIZEBOX)
      if(!band(flags, ui_consts.WINDOW_NO_BORDER)){
         style = bor(style, WS_CAPTION)
         if(!band(flags, ui_consts.WINDOW_NO_RESIZE)){ style = bor(style, WS_MAXIMIZEBOX | WS_THICKFRAME) }
      } else {
         style = bor(style, WS_POPUP)
      }
   }
   if(band(flags, ui_consts.WINDOW_MAXIMIZE)){ style = bor(style, WS_MAXIMIZE) }
   style
}

fn get_window_ex_style(int: flags=0, bool: monitor=false): int {
   "Select the extended Win32 window style for the requested backend hints."
   mut style = WS_EX_APPWINDOW
   if(monitor || band(flags, ui_consts.WINDOW_FULLSCREEN) || band(flags, ui_consts.WINDOW_FLOATING)){ style = bor(style, WS_EX_TOPMOST) }
   style
}

fn _rect_width(any: rect): int { load32(rect, 8) - load32(rect, 0) }

fn _rect_height(any: rect): int { load32(rect, 12) - load32(rect, 4) }

fn _window_dpi(any: hwnd=0): int {
   if(!available()){ return USER_DEFAULT_SCREEN_DPI }
   if(hwnd){
      def dpi = int(GetDpiForWindow(hwnd))
      if(dpi > 0){ return dpi }
   }
   USER_DEFAULT_SCREEN_DPI
}

fn _adjust_window_rect(any: rect, any: style, any: ex_style, any: hwnd=0): bool {
   if(!rect){ return false }
   def dpi = _window_dpi(hwnd)
   if(AdjustWindowRectExForDpi(rect, style, backend_api.FALSE, ex_style, dpi) != 0){ return true }
   AdjustWindowRectEx(rect, style, backend_api.FALSE, ex_style) != 0
}

fn _system_metric(int: index, int: dpi=0): int {
   if(!available()){ return 0 }
   def actual_dpi = dpi > 0 ? dpi : USER_DEFAULT_SCREEN_DPI
   def value = GetSystemMetricsForDpi(index, actual_dpi)
   if(value > 0){ return value }
   GetSystemMetrics(index)
}

fn _update_framebuffer_transparency(any: win): any {
   if(!available() || !win || !is_dict(win) || !win.get("transparent", false)){ return win }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return win }
   def composition = zalloc(4)
   if(!composition){ return win }
   def enabled = DwmIsCompositionEnabled(composition) == 0 && load32(composition, 0) != 0
   free(composition)
   if(!enabled){ return win }
   def color = zalloc(4)
   def opaque = zalloc(4)
   def got_color = color && opaque && DwmGetColorizationColor(color, opaque) == 0
   def use_blur_region = !got_color || load32(opaque, 0) == 0
   def blur = zalloc(24)
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

fn _make_int_resource(any: v): any { v }

fn _signed16(any: v): int {
   def x = int(v) & 0xffff
   if(x >= 0x8000){ x - 0x10000 } else { x }
}

fn _signed32(any: v): int {
   def x = int(v) & 0xffffffff
   if(x >= 0x80000000){ x - 0x100000000 } else { x }
}

fn _icon_image_width(any: image): int {
   if(!is_dict(image)){ return 0 }
   int(image.get("width", image.get("w", 0)))
}

fn _icon_image_height(any: image): int {
   if(!is_dict(image)){ return 0 }
   int(image.get("height", image.get("h", 0)))
}

fn _icon_image_pixels(any: image): any {
   if(!is_dict(image)){ return 0 }
   image.get("pixels_ptr",
      image.get("pixels",
   image.get("data", 0)))
}

fn _icon_pixel_source_len(any: pixels): int {
   if(is_str(pixels) || is_bytes(pixels) || is_list(pixels) || is_tuple(pixels)){ return pixels.len }
   if(is_ptr(pixels)){ return -1 }
   0
}

fn _icon_pixel_byte(any: pixels, int: index): int {
   if(is_ptr(pixels) || is_str(pixels) || is_bytes(pixels)){ return load8(pixels, index) }
   if(is_list(pixels) || is_tuple(pixels)){ return pixels.get(index, 0) }
   0
}

fn _choose_icon_image(any: images, int: wanted_w, int: wanted_h): any {
   if(is_dict(images)){ return images }
   if(!is_list(images) && !is_tuple(images)){ return false }
   mut best = false
   mut best_score = 1 << 30
   mut i = 0
   def images_n = images.len
   while(i < images_n){
      def image = images.get(i, 0)
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

fn _wide_field_utf8(any: base, int: offset): str {
   if(!base){ return "" }
   utf8_from_utf16(base + offset)
}

fn _alloc_display_device(): any {
   def dd = zalloc(840)
   if(!dd){ return 0 }
   store32(dd, 840, 0)
   dd
}

fn _alloc_devmode(): any {
   def dm = zalloc(220)
   if(!dm){ return 0 }
   store16(dm, 220, 68)
   dm
}

fn _split_bpp(any: bpp): list {
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

fn _devmode_to_vidmode(any: dm): any {
   if(!dm){ return false }
   def rgb = _split_bpp(load32(dm, 168))
   mut mode = dict(8)
   mode = mode.set("width", load32(dm, 172))
   mode = mode.set("height", load32(dm, 176))
   mode = mode.set("red_bits", rgb.get(0, 8))
   mode = mode.set("green_bits", rgb.get(1, 8))
   mode = mode.set("blue_bits", rgb.get(2, 8))
   mode = mode.set("refresh_rate", load32(dm, 184))
   mode
}

fn _vidmode_bpp(any: mode): int {
   int(mode.get("red_bits", 8)) +
   int(mode.get("green_bits", 8)) +
   int(mode.get("blue_bits", 8))
}

fn _choose_best_video_mode(any: monitor, int: width, int: height, int: refresh_rate=0): any {
   def modes = get_video_modes(monitor)
   if(modes.len == 0){ return get_video_mode(monitor) }
   mut best = false
   mut best_score = 1 << 30
   mut i = 0
   def modes_n = modes.len
   while(i < modes_n){
      def mode = modes.get(i, false)
      if(mode && is_dict(mode)){
         def mw, mh = mode.get("width", 0), mode.get("height", 0)
         def mr = mode.get("refresh_rate", 0)
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

fn _set_video_mode(any: monitor, int: width, int: height, int: refresh_rate=0): bool {
   if(!available() || !monitor || !is_dict(monitor)){ return false }
   def adapter = monitor.get("adapter_name", "")
   if(adapter.len == 0){ return false }
   def current = get_video_mode(monitor)
   def best = _choose_best_video_mode(monitor, width, height, refresh_rate)
   if(!best || !is_dict(best)){ return false }
   if(current &&
      current.get("width", 0) == best.get("width", 0) &&
      current.get("height", 0) == best.get("height", 0) &&
      current.get("refresh_rate", 0) == best.get("refresh_rate", 0) &&
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
   store32(dm, best.get("width", width), 172)
   store32(dm, best.get("height", height), 176)
   mut bpp = _vidmode_bpp(best)
   if(bpp < 15 || bpp >= 24){ bpp = 32 }
   store32(dm, bpp, 168)
   store32(dm, best.get("refresh_rate", refresh_rate), 184)
   def result = ChangeDisplaySettingsExW(wide, dm, 0, CDS_FULLSCREEN, 0)
   free(wide, dm)
   result == DISP_CHANGE_SUCCESSFUL
}

fn _restore_video_mode(str: adapter_name): bool {
   if(!available() || adapter_name.len == 0){ return false }
   def wide = utf16_from_utf8(adapter_name)
   if(!wide){ return false }
   def result = ChangeDisplaySettingsExW(wide, 0, 0, CDS_FULLSCREEN, 0)
   free(wide)
   result == DISP_CHANGE_SUCCESSFUL
}

fn _driver_display_wide(): any { utf16_from_utf8("DISPLAY") }

fn _physical_size_from_adapter(any: adapter_wide, int: width, int: height): list {
   if(!adapter_wide){ return [0, 0] }
   def driver = _driver_display_wide()
   def dc = CreateDCW(driver, adapter_wide, 0, 0)
   if(driver){ free(driver) }
   if(!dc){ return [0, 0] }
   mut width_mm = GetDeviceCaps(dc, HORZSIZE)
   mut height_mm = GetDeviceCaps(dc, VERTSIZE)
   if(width_mm <= 0 || height_mm <= 0){
      def log_x, log_y = max(1, GetDeviceCaps(dc, LOGPIXELSX)), max(1, GetDeviceCaps(dc, LOGPIXELSY))
      width_mm = int((float(width) * 25.4 / float(log_x)) + 0.5)
      height_mm = int((float(height) * 25.4 / float(log_y)) + 0.5)
   }
   DeleteDC(dc)
   [width_mm, height_mm]
}

fn _content_scale_from_monitor(any: monitor_handle, any: adapter_wide): list {
   mut xdpi, ydpi = 0, 0
   def xp, yp = zalloc(4), zalloc(4)
   if(monitor_handle && xp && yp && GetDpiForMonitor(monitor_handle, MDT_EFFECTIVE_DPI, xp, yp) == 0){ xdpi, ydpi = load32(xp, 0), load32(yp, 0) }
   if(xp){ free(xp) }
   if(yp){ free(yp) }
   if(xdpi <= 0 || ydpi <= 0){
      def driver = _driver_display_wide()
      def dc = CreateDCW(driver, adapter_wide, 0, 0)
      if(driver){ free(driver) }
      if(dc){
         xdpi, ydpi = GetDeviceCaps(dc, LOGPIXELSX), GetDeviceCaps(dc, LOGPIXELSY)
         DeleteDC(dc)
      }
   }
   if(xdpi <= 0){ xdpi = USER_DEFAULT_SCREEN_DPI }
   if(ydpi <= 0){ ydpi = USER_DEFAULT_SCREEN_DPI }
   [float(xdpi) / float(USER_DEFAULT_SCREEN_DPI), float(ydpi) / float(USER_DEFAULT_SCREEN_DPI)]
}

fn _create_monitor_from_devices(any: adapter, any: display=0): any {
   if(!available() || !adapter){ return false }
   def adapter_name = _wide_field_utf8(adapter, 4)
   if(adapter_name.len == 0){ return false }
   def adapter_wide = utf16_from_utf8(adapter_name)
   if(!adapter_wide){ return false }
   def dm = _alloc_devmode()
   if(!dm){
      free(adapter_wide)
      return false
   }
   if(EnumDisplaySettingsW(adapter_wide, ENUM_CURRENT_SETTINGS, dm) == 0){
      free(dm, adapter_wide)
      return false
   }
   def x, y = _signed32(load32(dm, 76)), _signed32(load32(dm, 80))
   def width = load32(dm, 172)
   def height = load32(dm, 176)
   def rect = zalloc(16)
   if(!rect){
      free(dm, adapter_wide)
      return false
   }
   store32(rect, x, 0)
   store32(rect, y, 4)
   store32(rect, x + width, 8)
   store32(rect, y + height, 12)
   def handle = MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST)
   free(rect)
   def mi = zalloc(104)
   if(!mi){
      free(dm, adapter_wide)
      return false
   }
   store32(mi, 104, 0)
   mut work_x, work_y = x, y
   mut work_w, work_h = width, height
   mut primary = band(load32(adapter, 324), DISPLAY_DEVICE_PRIMARY_DEVICE)
   if(handle && GetMonitorInfoW(handle, mi) != 0){
      work_x, work_y = _signed32(load32(mi, 20)), _signed32(load32(mi, 24))
      work_w, work_h = _signed32(load32(mi, 28)) - work_x, _signed32(load32(mi, 32)) - work_y
      primary = primary || band(load32(mi, 36), MONITORINFOF_PRIMARY)
   }
   free(mi)
   def mm = _physical_size_from_adapter(adapter_wide, width, height)
   def scale = _content_scale_from_monitor(handle, adapter_wide)
   def mode = _devmode_to_vidmode(dm)
   def name = display ? _wide_field_utf8(display, 68) : _wide_field_utf8(adapter, 68)
   def display_name = display ? _wide_field_utf8(display, 4) : ""
   free(dm, adapter_wide)
   mut monitor = dict(8)
   monitor = monitor.set("handle", handle)
   monitor = monitor.set("name", name)
   monitor = monitor.set("adapter_name", adapter_name)
   monitor = monitor.set("display_name", display_name)
   monitor = monitor.set("x", x)
   monitor = monitor.set("y", y)
   monitor = monitor.set("width", width)
   monitor = monitor.set("height", height)
   monitor = monitor.set("work_x", work_x)
   monitor = monitor.set("work_y", work_y)
   monitor = monitor.set("work_w", work_w)
   monitor = monitor.set("work_h", work_h)
   monitor = monitor.set("width_mm", mm.get(0, 0))
   monitor = monitor.set("height_mm", mm.get(1, 0))
   monitor = monitor.set("scale_x", scale.get(0, 1.0))
   monitor = monitor.set("scale_y", scale.get(1, 1.0))
   monitor = monitor.set("red_bits", mode.get("red_bits", 8))
   monitor = monitor.set("green_bits", mode.get("green_bits", 8))
   monitor = monitor.set("blue_bits", mode.get("blue_bits", 8))
   monitor = monitor.set("refresh_rate", mode.get("refresh_rate", 60))
   monitor = monitor.set("primary", !!primary)
   monitor
}

fn _gamma_ramp_from_buffer(any: values, int: size=256): any {
   if(!values || size <= 0){ return false }
   mut red = []
   mut green = []
   mut blue = []
   mut i = 0
   while(i < size){
      red = red.append(int(load16(values, i * 2)))
      green = green.append(int(load16(values, 512 + (i * 2))))
      blue = blue.append(int(load16(values, 1024 + (i * 2))))
      i += 1
   }
   mut ramp = dict(8)
   ramp = ramp.set("size", size)
   ramp = ramp.set("red", red)
   ramp = ramp.set("green", green)
   ramp = ramp.set("blue", blue)
   ramp
}

fn _clamp_gamma_word(any: v): int {
   def x = int(v)
   if(x < 0){ return 0 }
   if(x > 65535){ return 65535 }
   x
}

fn _copy_gamma_channel(any: src, any: dst, int: off, int: size): bool {
   if(!src || !dst || size <= 0){ return false }
   if(!is_list(src) || src.len < size){ return false }
   mut i = 0
   while(i < size){
      store16(dst, _clamp_gamma_word(src.get(i, 0)), off + i * 2)
      i += 1
   }
   true
}

fn _get_x_lparam(any: lp): int { _signed16(lp) }

fn _get_y_lparam(any: lp): int { _signed16(bshr(lp, 16)) }

fn _hiword(any: v): int { int(band(bshr(v, 16), 0xffff)) }

fn _loword(any: v): int { int(band(v, 0xffff)) }

fn _key_state_bit(any: win, int: key): bool {
   if(!win || !is_dict(win)){ return false }
   win.get("key_states", dict(8)).get(key, false)
}

fn _cursor_handle(any: cursor): any {
   if(!cursor){ return 0 }
   if(is_dict(cursor)){ return cursor.get("handle", 0) }
   cursor
}

fn _show_cursor(bool: visible): bool {
   if(!available()){ return false }
   mut n = 0
   if(visible){ while(n < 32 && ShowCursor(1) < 0){ n += 1 } } else { while(n < 32 && ShowCursor(0) >= 0){ n += 1 } }
   true
}

fn _client_clip_rect(any: hwnd): any {
   if(!available() || !hwnd){ return 0 }
   def rect = zalloc(16)
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

fn _capture_cursor(any: hwnd): bool {
   if(!available() || !hwnd){ return false }
   def rect = _client_clip_rect(hwnd)
   if(!rect){ return false }
   def ok = ClipCursor(rect) != 0
   free(rect)
   ok
}

fn _release_cursor(): bool {
   if(!available()){ return false }
   ClipCursor(0) != 0
}

fn _effective_flags(any: win): int {
   if(!win || !is_dict(win)){ return 0 }
   mut flags = int(win.get("flags", 0))
   if(win.get("decorated", !band(flags, ui_consts.WINDOW_NO_BORDER))){ flags = band(flags, bnot(ui_consts.WINDOW_NO_BORDER)) } else { flags = bor(flags, ui_consts.WINDOW_NO_BORDER) }
   if(win.get("resizable", !band(flags, ui_consts.WINDOW_NO_RESIZE))){ flags = band(flags, bnot(ui_consts.WINDOW_NO_RESIZE)) } else { flags = bor(flags, ui_consts.WINDOW_NO_RESIZE) }
   if(win.get("floating", band(flags, ui_consts.WINDOW_FLOATING))){ flags = bor(flags, ui_consts.WINDOW_FLOATING) } else { flags = band(flags, bnot(ui_consts.WINDOW_FLOATING)) }
   if(win.get("fullscreen", band(flags, ui_consts.WINDOW_FULLSCREEN))){ flags = bor(flags, ui_consts.WINDOW_FULLSCREEN) } else { flags = band(flags, bnot(ui_consts.WINDOW_FULLSCREEN)) }
   flags
}

fn _update_window_styles(any: win): any {
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return win }
   def flags = _effective_flags(win)
   def style = get_window_style(flags, win.get("fullscreen", false))
   def ex_style = get_window_ex_style(flags, win.get("fullscreen", false))
   SetWindowLongW(hwnd, GWL_STYLE, style)
   SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style)
   def sz = get_size(win)
   def rect = zalloc(16)
   if(rect){
      store32(rect, 0, 0)
      store32(rect, 0, 4)
      store32(rect, int(sz.get(0, 0)), 8)
      store32(rect, int(sz.get(1, 0)), 12)
      _adjust_window_rect(rect, style, ex_style, hwnd)
      SetWindowPos(hwnd, HWND_TOP,
         win.get("x", CW_USEDEFAULT), win.get("y", CW_USEDEFAULT),
         _rect_width(rect), _rect_height(rect),
      SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER)
      free(rect)
   }
   win
}

fn _clipboard_hwnd(any: win): any {
   if(is_dict(win)){ return win.get("handle", 0) }
   win
}

fn _open_clipboard_with_retry(any: hwnd, int: tries=3): bool {
   mut n = 0
   while(n < tries){
      if(OpenClipboard(hwnd) != 0){ return true }
      Sleep(1)
      n += 1
   }
   false
}

fn _create_rgba_cursor_handle(any: image, int: xhot=0, int: yhot=0, bool: icon=false): any {
   if(!available() || !image || !is_dict(image)){ return 0 }
   def width = _icon_image_width(image)
   def height = _icon_image_height(image)
   def pixels = _icon_image_pixels(image)
   def bytes = width * height * 4
   def have = _icon_pixel_source_len(pixels)
   if(width <= 0 || height <= 0 || !pixels || (have >= 0 && have < bytes)){ return 0 }
   def bi = zalloc(124)
   def bits = zalloc(8)
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
   def ii = zalloc(32)
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

fn _enable_raw_mouse_motion(any: hwnd): bool {
   if(!available() || !hwnd){ return false }
   def rid = zalloc(16)
   if(!rid){ return false }
   store16(rid, 0x01, 0)
   store16(rid, 0x02, 2)
   store32(rid, 0, 4)
   store64_h(rid, hwnd, 8)
   def ok = RegisterRawInputDevices(rid, 1, 16) != 0
   free(rid)
   ok
}

fn _disable_raw_mouse_motion(): bool {
   if(!available()){ return false }
   def rid = zalloc(16)
   if(!rid){ return false }
   store16(rid, 0x01, 0)
   store16(rid, 0x02, 2)
   store32(rid, RIDEV_REMOVE, 4)
   store64_h(rid, 0, 8)
   def ok = RegisterRawInputDevices(rid, 1, 16) != 0
   free(rid)
   ok
}

fn _apply_cursor_handle(any: win): any {
   if(!win || !is_dict(win)){ return win }
   def mode = win.get("cursor_mode", backend_api.CURSOR_NORMAL)
   if(mode != backend_api.CURSOR_NORMAL && mode != backend_api.CURSOR_CAPTURED){ return win }
   def handle = _cursor_handle(win.get("cursor", 0))
   SetCursor(common.value_or(handle, LoadCursorW(0, _make_int_resource(IDC_ARROW))))
   win = win.set("cursor_handle", handle)
   win
}

fn _poll_monitor_changes(list: events, any: win): list {
   if(!_monitors_initialized){
      def monitors0 = get_monitors()
      mut names0 = []
      mut k = 0
      def monitors0_n = monitors0.len
      while(k < monitors0_n){
         def aname = monitors0.get(k).get("adapter_name", "")
         if(is_str(aname) && aname != ""){ names0 = names0.append(aname) }
         k += 1
      }
      _known_monitor_names = names0
      _monitors_initialized = true
      return [events, win]
   }
   def monitors = get_monitors()
   mut current_names = dict(8)
   mut i = 0
   def monitors_n = monitors.len
   while(i < monitors_n){
      def m = monitors.get(i)
      def aname = m.get("adapter_name", "")
      if(is_str(aname) && aname != ""){ current_names = current_names.set(aname, m) }
      i += 1
   }
   def cur_keys = dict_keys(current_names)
   i = 0
   def cur_keys_n = cur_keys.len
   def known_monitor_names_n = _known_monitor_names.len
   while(i < cur_keys_n){
      def aname = cur_keys.get(i)
      mut was_known = false
      mut j = 0
      while(j < known_monitor_names_n){
         if(_known_monitor_names.get(j) == aname){ was_known = true break }
         j += 1
      }
      if(!was_known){
         mut data = dict(8)
         data = data.set("monitor", current_names.get(aname, 0))
         _push_event(events, ui_consts.EVENT_MONITOR_CONNECTED, win, data)
      }
      i += 1
   }
   i = 0
   while(i < known_monitor_names_n){
      def aname = _known_monitor_names.get(i)
      if(!current_names.contains(aname)){
         mut data = dict(8)
         data = data.set("adapter_name", aname)
         _push_event(events, ui_consts.EVENT_MONITOR_DISCONNECTED, win, data)
      }
      i += 1
   }
   _known_monitor_names = cur_keys
   [events, win]
}

fn _mods_from_win_state(any: win): int {
   mut mods = 0
   if(_key_state_bit(win, backend_api.KEY_LEFT_SHIFT) ||
      _key_state_bit(win, backend_api.KEY_RIGHT_SHIFT)){
      mods = bor(mods, backend_api.MOD_SHIFT)
   }
   if(_key_state_bit(win, backend_api.KEY_LEFT_CONTROL) ||
      _key_state_bit(win, backend_api.KEY_RIGHT_CONTROL)){
      mods = bor(mods, backend_api.MOD_CONTROL)
   }
   if(_key_state_bit(win, backend_api.KEY_LEFT_ALT) ||
      _key_state_bit(win, backend_api.KEY_RIGHT_ALT)){
      mods = bor(mods, backend_api.MOD_ALT)
   }
   if(_key_state_bit(win, backend_api.KEY_LEFT_SUPER) ||
      _key_state_bit(win, backend_api.KEY_RIGHT_SUPER)){
      mods = bor(mods, backend_api.MOD_SUPER)
   }
   if(_key_state_bit(win, backend_api.KEY_CAPS_LOCK)){ mods = bor(mods, backend_api.MOD_CAPS_LOCK) }
   if(_key_state_bit(win, backend_api.KEY_NUM_LOCK)){ mods = bor(mods, backend_api.MOD_NUM_LOCK) }
   mods
}

fn _mods_for_key_event(any: win, int: key, bool: pressed): int {
   mut mods = _mods_from_win_state(win)
   match key {
      backend_api.KEY_LEFT_SHIFT, backend_api.KEY_RIGHT_SHIFT -> {
         mods = pressed ? bor(mods, backend_api.MOD_SHIFT) : band(mods, bnot(backend_api.MOD_SHIFT))
      }
      backend_api.KEY_LEFT_CONTROL, backend_api.KEY_RIGHT_CONTROL -> {
         mods = pressed ? bor(mods, backend_api.MOD_CONTROL) : band(mods, bnot(backend_api.MOD_CONTROL))
      }
      backend_api.KEY_LEFT_ALT, backend_api.KEY_RIGHT_ALT -> {
         mods = pressed ? bor(mods, backend_api.MOD_ALT) : band(mods, bnot(backend_api.MOD_ALT))
      }
      backend_api.KEY_LEFT_SUPER, backend_api.KEY_RIGHT_SUPER -> {
         mods = pressed ? bor(mods, backend_api.MOD_SUPER) : band(mods, bnot(backend_api.MOD_SUPER))
      }
      _ -> {}
   }
   mods
}

comptime table Win32ScancodeKey {
   0x00B -> backend_api.KEY_0
   0x002..0x00A -> backend_api.KEY_1 + (raw - 0x002)
   0x01E -> backend_api.KEY_A
   0x030 -> backend_api.KEY_B
   0x02E -> backend_api.KEY_C
   0x020 -> backend_api.KEY_D
   0x012 -> backend_api.KEY_E
   0x021 -> backend_api.KEY_F
   0x022 -> backend_api.KEY_G
   0x023 -> backend_api.KEY_H
   0x017 -> backend_api.KEY_I
   0x024 -> backend_api.KEY_J
   0x025 -> backend_api.KEY_K
   0x026 -> backend_api.KEY_L
   0x032 -> backend_api.KEY_M
   0x031 -> backend_api.KEY_N
   0x018 -> backend_api.KEY_O
   0x019 -> backend_api.KEY_P
   0x010 -> backend_api.KEY_Q
   0x013 -> backend_api.KEY_R
   0x01F -> backend_api.KEY_S
   0x014 -> backend_api.KEY_T
   0x016 -> backend_api.KEY_U
   0x02F -> backend_api.KEY_V
   0x011 -> backend_api.KEY_W
   0x02D -> backend_api.KEY_X
   0x015 -> backend_api.KEY_Y
   0x02C -> backend_api.KEY_Z
   0x028 -> backend_api.KEY_APOSTROPHE
   0x02B -> backend_api.KEY_BACKSLASH
   0x033 -> backend_api.KEY_COMMA
   0x00D -> backend_api.KEY_EQUAL
   0x029 -> backend_api.KEY_GRAVE_ACCENT
   0x01A -> backend_api.KEY_LEFT_BRACKET
   0x00C -> backend_api.KEY_MINUS
   0x034 -> backend_api.KEY_PERIOD
   0x01B -> backend_api.KEY_RIGHT_BRACKET
   0x027 -> backend_api.KEY_SEMICOLON
   0x035 -> backend_api.KEY_SLASH
   0x056 -> backend_api.KEY_WORLD_2
   0x00E -> backend_api.KEY_BACKSPACE
   0x153 -> backend_api.KEY_DELETE
   0x14F -> backend_api.KEY_END
   0x01C -> backend_api.KEY_ENTER
   0x001 -> backend_api.KEY_ESCAPE
   0x147 -> backend_api.KEY_HOME
   0x152 -> backend_api.KEY_INSERT
   0x15D -> backend_api.KEY_MENU
   0x151 -> backend_api.KEY_PAGE_DOWN
   0x149 -> backend_api.KEY_PAGE_UP
   0x045 -> backend_api.KEY_PAUSE
   0x039 -> backend_api.KEY_SPACE
   0x00F -> backend_api.KEY_TAB
   0x03A -> backend_api.KEY_CAPS_LOCK
   0x145 -> backend_api.KEY_NUM_LOCK
   0x046 -> backend_api.KEY_SCROLL_LOCK
   0x03B..0x044 -> backend_api.KEY_F1 + (raw - 0x03B)
   0x057 -> backend_api.KEY_F11
   0x058 -> backend_api.KEY_F12
   0x064..0x06E -> backend_api.KEY_F13 + (raw - 0x064)
   0x076 -> backend_api.KEY_F24
   0x038 -> backend_api.KEY_LEFT_ALT
   0x01D -> backend_api.KEY_LEFT_CONTROL
   0x02A -> backend_api.KEY_LEFT_SHIFT
   0x15B -> backend_api.KEY_LEFT_SUPER
   0x137 -> backend_api.KEY_PRINT_SCREEN
   0x138 -> backend_api.KEY_RIGHT_ALT
   0x11D -> backend_api.KEY_RIGHT_CONTROL
   0x036 -> backend_api.KEY_RIGHT_SHIFT
   0x15C -> backend_api.KEY_RIGHT_SUPER
   0x150 -> backend_api.KEY_DOWN
   0x14B -> backend_api.KEY_LEFT
   0x14D -> backend_api.KEY_RIGHT
   0x148 -> backend_api.KEY_UP
   0x052 -> backend_api.KEY_KP_0
   0x04F..0x051 -> backend_api.KEY_KP_1 + (raw - 0x04F)
   0x04B..0x04D -> backend_api.KEY_KP_4 + (raw - 0x04B)
   0x047..0x049 -> backend_api.KEY_KP_7 + (raw - 0x047)
   0x04E -> backend_api.KEY_KP_ADD
   0x053 -> backend_api.KEY_KP_DECIMAL
   0x135 -> backend_api.KEY_KP_DIVIDE
   0x11C -> backend_api.KEY_KP_ENTER
   0x059 -> backend_api.KEY_KP_EQUAL
   0x037 -> backend_api.KEY_KP_MULTIPLY
   0x04A -> backend_api.KEY_KP_SUBTRACT
   _ -> default
}

fn _translate_win32_scancode(any: scancode): int { comptime match Win32ScancodeKey(int(scancode), -1) }

comptime table Win32VirtualKeyFallback {
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
   VK_NUMPAD0..VK_NUMPAD9 -> backend_api.KEY_KP_0 + (raw - VK_NUMPAD0)
   VK_DECIMAL, VK_SEPARATOR -> backend_api.KEY_KP_DECIMAL
   VK_DIVIDE -> backend_api.KEY_KP_DIVIDE
   VK_MULTIPLY -> backend_api.KEY_KP_MULTIPLY
   VK_SUBTRACT -> backend_api.KEY_KP_SUBTRACT
   VK_ADD -> backend_api.KEY_KP_ADD
   VK_LSHIFT -> backend_api.KEY_LEFT_SHIFT
   VK_RSHIFT -> backend_api.KEY_RIGHT_SHIFT
   VK_LCONTROL -> backend_api.KEY_LEFT_CONTROL
   VK_RCONTROL -> backend_api.KEY_RIGHT_CONTROL
   VK_LMENU -> backend_api.KEY_LEFT_ALT
   VK_RMENU -> backend_api.KEY_RIGHT_ALT
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
   _ -> default
}

fn translate_vk_key(int: vk, int: scancode=0, bool: extended=false): int {
   "Translates Win32 virtual keys to backend key ids."
   def key = _translate_win32_scancode(scancode)
   if(key >= 0){ return key }
   if(vk == VK_SHIFT){ return scancode == 0x36 ? backend_api.KEY_RIGHT_SHIFT : backend_api.KEY_LEFT_SHIFT }
   if(vk == VK_CONTROL){ return extended ? backend_api.KEY_RIGHT_CONTROL : backend_api.KEY_LEFT_CONTROL }
   if(vk == VK_MENU){ return extended ? backend_api.KEY_RIGHT_ALT : backend_api.KEY_LEFT_ALT }
   comptime match Win32VirtualKeyFallback(vk, -1)
}

fn _push_event(list: events, int: typ, any: win, any: data=0): list { events.append(ui_event.make_event(typ, win, win.get("handle", 0), data)) }

fn _emit_char_event(list: events, any: win, int: codepoint, int: mods=0): list {
   if(codepoint <= 0){ return events }
   mut data = dict(8)
   data = data.set("char", int(codepoint))
   data = data.set("codepoint", int(codepoint))
   data = data.set("mod", mods)
   data = data.set("mods", mods)
   _push_event(events, ui_consts.EVENT_KEY_CHAR, win, data)
}

fn _emit_utf16_char_events(list: events, any: win, any: wide, int: units, int: mods=0): list {
   if(!wide || units <= 0){ return events }
   mut out = events
   mut pending_high = 0
   mut i = 0
   while(i < units){
      def ch = int(load16(wide, i * 2))
      if(ch >= 0xd800 && ch <= 0xdbff){ pending_high = ch } elif(ch >= 0xdc00 && ch <= 0xdfff){
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

fn _emit_drop_event(list: events, any: win, any: drop): list {
   if(!drop){ return [win, events] }
   mut out = events
   def point = zalloc(8)
   if(point){
      if(DragQueryPoint(drop, point) != 0){
         def x, y = load32(point, 0), load32(point, 4)
         win = win.set("mouse_x", x)
         win = win.set("mouse_y", y)
         mut move_data = dict(8)
         move_data = move_data.set("x", x)
         move_data = move_data.set("y", y)
         _push_event(out, ui_consts.EVENT_MOUSE_POS_CHANGED, win, move_data)
      }
      free(point)
   }
   def count = int(DragQueryFileW(drop, 0xffffffff, 0, 0))
   mut paths = []
   mut i = 0
   while(i < count){
      def length = int(DragQueryFileW(drop, i, 0, 0))
      if(length >= 0){
         def wide = zalloc((length + 1) * 2)
         if(wide){
            if(DragQueryFileW(drop, i, wide, length + 1) > 0){
               def path = utf8_from_utf16(wide)
               if(path && path.len > 0){ paths = paths.append(path) }
            }
            free(wide)
         }
      }
      i += 1
   }
   if(paths.len > 0){
      mut data = dict(8)
      data = data.set("paths", paths)
      _push_event(out, ui_consts.EVENT_DATA_DROP, win, data)
   }
   DragFinish(drop)
   [win, out]
}

fn _emit_ime_result_events(list: events, any: win): list {
   if(!available() || !win || !is_dict(win)){ return [win, events] }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return [win, events] }
   def himc = ImmGetContext(hwnd)
   if(!himc){ return [win, events] }
   mut out = events
   def bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, 0, 0)
   if(bytes > 0){
      def buffer = zalloc(bytes + 2)
      if(buffer){
         def got = ImmGetCompositionStringW(himc, GCS_RESULTSTR, buffer, bytes)
         if(got > 0){
            win = win.set("high_surrogate", 0)
            out = _emit_utf16_char_events(out, win, buffer, got / 2, _mods_from_win_state(win))
         }
         free(buffer)
      }
   }
   ImmReleaseContext(hwnd, himc)
   [win, out]
}

fn _translate_message_general(any: win, list: events, int: typ, any: wparam, any: lparam): list {
   match typ {
      WM_CLOSE, WM_QUIT -> {
         win = win.set("should_close", true)
         _push_event(events, ui_consts.EVENT_QUIT, win, 0)
      }
      WM_INPUTLANGCHANGE -> {
         win = win.set("input_lang_changed", true)
      }
      WM_IME_STARTCOMPOSITION -> {
         win = win.set("ime_composing", true)
      }
      WM_IME_ENDCOMPOSITION -> {
         win = win.set("ime_composing", false)
      }
      WM_IME_COMPOSITION -> {
         if(band(int(lparam), GCS_RESULTSTR) != 0){
            def res = _emit_ime_result_events(events, win)
            win, events = res.get(0, win), res.get(1, events)
         } elif(band(int(lparam), GCS_COMPSTR) != 0){
            win = win.set("ime_composing", true)
         }
      }
      WM_IME_SETCONTEXT -> {
      }
      WM_IME_NOTIFY -> {
      }
      WM_SETFOCUS -> {
         win = win.set("focused", true)
         _push_event(events, ui_consts.EVENT_FOCUS_IN, win, 0)
      }
      WM_KILLFOCUS -> {
         win = win.set("focused", false)
         _push_event(events, ui_consts.EVENT_FOCUS_OUT, win, 0)
      }
      WM_CHAR, WM_SYSCHAR -> {
         def ch = int(wparam)
         if(ch >= 0xd800 && ch <= 0xdbff){
            win = win.set("high_surrogate", ch)
         } else {
            mut codepoint = 0
            if(ch >= 0xdc00 && ch <= 0xdfff){
               def high = int(win.get("high_surrogate", 0))
               if(high >= 0xd800 && high <= 0xdbff){ codepoint = ((high - 0xd800) << 10) + (ch - 0xdc00) + 0x10000 }
            } else {
               codepoint = ch
            }
            win = win.set("high_surrogate", 0)
            _emit_char_event(events, win, codepoint, _mods_from_win_state(win))
         }
      }
      WM_UNICHAR -> {
         if(int(wparam) != UNICODE_NOCHAR){ _emit_char_event(events, win, int(wparam), _mods_from_win_state(win)) }
      }
      WM_DROPFILES -> {
         def res = _emit_drop_event(events, win, wparam)
         win, events = res.get(0, win), res.get(1, events)
      }
      _ -> { return [false, win, events] }
   }
   [true, win, events]
}

fn _translate_message_key(any: win, list: events, int: typ, any: wparam, any: lparam): list {
   if(!(typ == WM_KEYDOWN || typ == WM_SYSKEYDOWN || typ == WM_KEYUP || typ == WM_SYSKEYUP)){ return [false, win, events] }
   def hi = _hiword(lparam)
   mut scancode = band(hi, bor(KF_EXTENDED, 0xff))
   def released = band(hi, KF_UP) != 0 || typ == WM_KEYUP || typ == WM_SYSKEYUP
   if(scancode == 0){ scancode = MapVirtualKeyW(int(wparam), MAPVK_VK_TO_VSC) }
   if(scancode == 0x54){ scancode = 0x137 }
   if(scancode == 0x146){ scancode = 0x45 }
   if(scancode == 0x136){ scancode = 0x36 }
   if(int(wparam) == VK_PROCESSKEY){ return [true, win, events] }
   def key = translate_vk_key(int(wparam), scancode, band(hi, KF_EXTENDED) != 0)
   if(key >= 0){
      mut ks = win.get("key_states", dict(64))
      def mods = _mods_for_key_event(win, key, !released)
      if(released && int(wparam) == VK_SHIFT){
         ks = ks.set(backend_api.KEY_LEFT_SHIFT, false)
         ks = ks.set(backend_api.KEY_RIGHT_SHIFT, false)
         win = win.set("key_states", ks)
         mut left_data = dict(8)
         left_data = left_data.set("raw_key", int(wparam))
         left_data = left_data.set("key", backend_api.KEY_LEFT_SHIFT)
         left_data = left_data.set("scancode", scancode)
         left_data = left_data.set("action", backend_api.ACTION_RELEASE)
         left_data = left_data.set("mod", mods)
         left_data = left_data.set("mods", mods)
         _push_event(events, ui_consts.EVENT_KEY_RELEASED, win, left_data)
         mut right_data = dict(8)
         right_data = right_data.set("raw_key", int(wparam))
         right_data = right_data.set("key", backend_api.KEY_RIGHT_SHIFT)
         right_data = right_data.set("scancode", scancode)
         right_data = right_data.set("action", backend_api.ACTION_RELEASE)
         right_data = right_data.set("mod", mods)
         right_data = right_data.set("mods", mods)
         _push_event(events, ui_consts.EVENT_KEY_RELEASED, win, right_data)
      } elif(int(wparam) == VK_SNAPSHOT && released){
         ks = ks.set(key, false)
         win = win.set("key_states", ks)
         mut press_data = dict(8)
         press_data = press_data.set("raw_key", int(wparam))
         press_data = press_data.set("key", key)
         press_data = press_data.set("scancode", scancode)
         press_data = press_data.set("action", backend_api.ACTION_PRESS)
         press_data = press_data.set("mod", mods)
         press_data = press_data.set("mods", mods)
         _push_event(events, ui_consts.EVENT_KEY_PRESSED, win, press_data)
         mut rel_data = dict(8)
         rel_data = rel_data.set("raw_key", int(wparam))
         rel_data = rel_data.set("key", key)
         rel_data = rel_data.set("scancode", scancode)
         rel_data = rel_data.set("action", backend_api.ACTION_RELEASE)
         rel_data = rel_data.set("mod", mods)
         rel_data = rel_data.set("mods", mods)
         _push_event(events, ui_consts.EVENT_KEY_RELEASED, win, rel_data)
      } else {
         ks = ks.set(key, !released)
         win = win.set("key_states", ks)
         mut data = dict(8)
         data = data.set("raw_key", int(wparam))
         data = data.set("key", key)
         data = data.set("scancode", scancode)
         def is_repeat = !released && band(hi, KF_REPEAT) != 0
         def action = released ? backend_api.ACTION_RELEASE :
         (is_repeat ? backend_api.ACTION_REPEAT : backend_api.ACTION_PRESS)
         data = data.set("action", action)
         data = data.set("mod", mods)
         data = data.set("mods", mods)
         _push_event(events, released ? ui_consts.EVENT_KEY_RELEASED : ui_consts.EVENT_KEY_PRESSED, win, data)
      }
   }
   [true, win, events]
}

fn _translate_message_pointer(any: win, list: events, int: typ, any: wparam, any: lparam): list {
   match typ {
      WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN,
      WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP -> {
         def pressed = typ == WM_LBUTTONDOWN || typ == WM_RBUTTONDOWN || typ == WM_MBUTTONDOWN || typ == WM_XBUTTONDOWN
         def button =
         (typ == WM_LBUTTONDOWN || typ == WM_LBUTTONUP) ? backend_api.MOUSE_BUTTON_LEFT :
         (typ == WM_RBUTTONDOWN || typ == WM_RBUTTONUP) ? backend_api.MOUSE_BUTTON_RIGHT :
         (typ == WM_MBUTTONDOWN || typ == WM_MBUTTONUP) ? backend_api.MOUSE_BUTTON_MIDDLE :
         (_hiword(wparam) == XBUTTON1 ? backend_api.MOUSE_BUTTON_4 : backend_api.MOUSE_BUTTON_5)
         mut buttons = win.get("mouse_buttons", dict(8))
         buttons = buttons.set(button, pressed)
         win = win.set("mouse_buttons", buttons)
         mut data = dict(8)
         data = data.set("button", button)
         data = data.set("action", pressed ? backend_api.ACTION_PRESS : backend_api.ACTION_RELEASE)
         data = data.set("mods", _mods_from_win_state(win))
         _push_event(events, pressed ? ui_consts.EVENT_MOUSE_BUTTON_PRESSED : ui_consts.EVENT_MOUSE_BUTTON_RELEASED, win, data)
      }
      WM_MOUSEMOVE -> {
         def x, y = _get_x_lparam(lparam), _get_y_lparam(lparam)
         def entered = !win.get("cursor_tracked", false)
         win = win.set("cursor_tracked", true)
         win = win.set("last_cursor_client_x", x)
         win = win.set("last_cursor_client_y", y)
         win = win.set("mouse_x", x)
         win = win.set("mouse_y", y)
         if(entered){ _push_event(events, ui_consts.EVENT_MOUSE_ENTER, win, 0) }
         mut data = dict(8)
         data = data.set("x", x)
         data = data.set("y", y)
         _push_event(events, ui_consts.EVENT_MOUSE_POS_CHANGED, win, data)
      }
      WM_INPUT -> {
         if(win.get("disabled_cursor", false) && win.get("raw_mouse_motion", false)){
            def size_ptr = zalloc(4)
            if(size_ptr){
               GetRawInputData(lparam, RID_INPUT, 0, size_ptr, 24)
               def size = load32(size_ptr, 0)
               if(size >= 48){
                  def raw = zalloc(size)
                  if(raw){
                     store32(size_ptr, size, 0)
                     if(GetRawInputData(lparam, RID_INPUT, raw, size_ptr, 24) != 0xffffffff){
                        if(load32(raw, 0) == RIM_TYPEMOUSE){
                           def flags = load16(raw, 24)
                           mut dx, dy = 0, 0
                           if(band(flags, MOUSE_MOVE_ABSOLUTE)){
                              def pt = zalloc(8)
                              if(pt){
                                 def virtual_desktop = band(flags, MOUSE_VIRTUAL_DESKTOP)
                                 def width = virtual_desktop ? GetSystemMetrics(SM_CXVIRTUALSCREEN) :
                                 GetSystemMetrics(SM_CXSCREEN)
                                 def height = virtual_desktop ? GetSystemMetrics(SM_CYVIRTUALSCREEN) :
                                 GetSystemMetrics(SM_CYSCREEN)
                                 def base_x = virtual_desktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0
                                 def base_y = virtual_desktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0
                                 store32(pt, base_x + int((float(load32(raw, 36)) / 65535.0) * float(width)), 0)
                                 store32(pt, base_y + int((float(load32(raw, 40)) / 65535.0) * float(height)), 4)
                                 if(ScreenToClient(win.get("handle", 0), pt) != 0){ dx, dy = load32(pt, 0) - win.get("mouse_x", 0), load32(pt, 4) - win.get("mouse_y", 0) }
                                 free(pt)
                              }
                           } else {
                              dx, dy = _signed32(load32(raw, 36)), _signed32(load32(raw, 40))
                           }
                           if(dx != 0 || dy != 0){
                              def next_x, next_y = win.get("mouse_x", 0) + dx, win.get("mouse_y", 0) + dy
                              win = win.set("mouse_x", next_x)
                              win = win.set("mouse_y", next_y)
                              mut data = dict(8)
                              data = data.set("x", next_x)
                              data = data.set("y", next_y)
                              data = data.set("dx", dx)
                              data = data.set("dy", dy)
                              data = data.set("raw", true)
                              _push_event(events, ui_consts.EVENT_MOUSE_POS_CHANGED, win, data)
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
         win = win.set("cursor_tracked", false)
         _push_event(events, ui_consts.EVENT_MOUSE_LEAVE, win, 0)
      }
      WM_MOUSEWHEEL, WM_MOUSEHWHEEL -> {
         mut data = dict(8)
         def delta = float(_signed16(_hiword(wparam))) / float(WHEEL_DELTA)
         if(typ == WM_MOUSEHWHEEL){
            data = data.set("x", delta)
            data = data.set("y", 0.0)
         } else {
            data = data.set("x", 0.0)
            data = data.set("y", delta)
         }
         _push_event(events, ui_consts.EVENT_MOUSE_SCROLL, win, data)
      }
      _ -> { return [false, win, events] }
   }
   [true, win, events]
}

fn _translate_message_window_state(any: win, list: events, int: typ, any: wparam, any: lparam): list {
   match typ {
      WM_MOVE -> {
         def x, y = _get_x_lparam(lparam), _get_y_lparam(lparam)
         win = win.set("x", x)
         win = win.set("y", y)
         mut data = dict(8)
         data = data.set("x", x)
         data = data.set("y", y)
         _push_event(events, ui_consts.EVENT_WINDOW_MOVED, win, data)
      }
      WM_SIZE -> {
         def size_type = int(wparam)
         def w = _loword(lparam)
         def h = _hiword(lparam)
         win = win.set("w", w)
         win = win.set("h", h)
         mut data = dict(8)
         data = data.set("w", w)
         data = data.set("h", h)
         _push_event(events, ui_consts.EVENT_WINDOW_RESIZED, win, data)
         if(size_type == SIZE_MINIMIZED){
            win = win.set("iconified", true)
            _push_event(events, ui_consts.EVENT_WINDOW_MINIMIZED, win, 0)
         } elif(size_type == SIZE_MAXIMIZED){
            win = win.set("iconified", false)
            win = win.set("maximized", true)
            _push_event(events, ui_consts.EVENT_WINDOW_MAXIMIZED, win, 0)
         } elif(size_type == SIZE_RESTORED){
            def was_iconified = win.get("iconified", false)
            def was_maximized = win.get("maximized", false)
            win = win.set("iconified", false)
            win = win.set("maximized", false)
            if(was_iconified || was_maximized){ _push_event(events, ui_consts.EVENT_WINDOW_RESTORED, win, 0) }
         }
      }
      WM_DPICHANGED -> {
         def scale = float(_hiword(wparam)) / 96.0
         win = win.set("scale_x", scale)
         win = win.set("scale_y", scale)
         mut data = dict(8)
         data = data.set("x", scale)
         data = data.set("y", scale)
         _push_event(events, ui_consts.EVENT_SCALE_UPDATED, win, data)
      }
      WM_DISPLAYCHANGE -> {
         def res = _poll_monitor_changes(events, win)
         events, win = res.get(0, events), res.get(1, win)
      }
      WM_PAINT -> {
         _push_event(events, ui_consts.EVENT_WINDOW_REFRESH, win, 0)
      }
      WM_ENTERSIZEMOVE -> {
         win = win.set("in_sizemove", true)
      }
      WM_EXITSIZEMOVE -> {
         win = win.set("in_sizemove", false)
      }
      _ -> { return [false, win, events] }
   }
   [true, win, events]
}

fn translate_message(any: win, any: msg): list {
   "Translates a raw Win32 message dict into Ny window events and updated state."
   if(!win || !is_dict(win) || !msg || !is_dict(msg)){ return [win, []] }
   mut events = []
   def typ = msg.get("message", 0)
   def wparam = msg.get("wparam", 0)
   def lparam = msg.get("lparam", 0)
   def general = _translate_message_general(win, events, typ, wparam, lparam)
   if(general.get(0, false)){ return [general.get(1, win), general.get(2, events)] }
   def key = _translate_message_key(win, events, typ, wparam, lparam)
   if(key.get(0, false)){ return [key.get(1, win), key.get(2, events)] }
   def ptr = _translate_message_pointer(win, events, typ, wparam, lparam)
   if(ptr.get(0, false)){ return [ptr.get(1, win), ptr.get(2, events)] }
   def wnd = _translate_message_window_state(win, events, typ, wparam, lparam)
   if(wnd.get(0, false)){ return [wnd.get(1, win), wnd.get(2, events)] }
   [win, events]
}

fn translate_messages(any: win, any: messages): list {
   "Translates a list of raw Win32 messages for one window."
   if(!win || !is_dict(win) || !is_list(messages)){ return [win, []] }
   mut out = []
   mut i = 0
   def messages_n = messages.len
   while(i < messages_n){
      def msg = messages.get(i, 0)
      if(msg && is_dict(msg) &&
         (msg.get("message", 0) == WM_KEYDOWN || msg.get("message", 0) == WM_SYSKEYDOWN ||
         msg.get("message", 0) == WM_KEYUP || msg.get("message", 0) == WM_SYSKEYUP) &&
         int(msg.get("wparam", 0)) == VK_CONTROL &&
         !band(_hiword(msg.get("lparam", 0)), KF_EXTENDED) &&
         i + 1 < messages_n){
         def next = messages.get(i + 1, 0)
         if(next && is_dict(next) &&
            (next.get("message", 0) == WM_KEYDOWN || next.get("message", 0) == WM_SYSKEYDOWN ||
            next.get("message", 0) == WM_KEYUP || next.get("message", 0) == WM_SYSKEYUP) &&
            int(next.get("wparam", 0)) == VK_MENU &&
            band(_hiword(next.get("lparam", 0)), KF_EXTENDED) &&
            next.get("time", -1) == msg.get("time", -2)){
            i += 1
            continue
         }
      }
      def res = translate_message(win, msg)
      win = res.get(0, win)
      def evs = res.get(1, [])
      if(is_list(evs) && evs.len > 0){ out = out.extend(evs) }
      i += 1
   }
   [win, out]
}

fn _vk_from_key(int: key): int {
   if(key >= 65 && key <= 90){ return key } ;; A-Z
   if(key >= 48 && key <= 57){ return key } ;; 0-9
   match key {
      backend_api.KEY_SPACE -> { return 0x20 }
      backend_api.KEY_APOSTROPHE -> { return 0xDE }
      backend_api.KEY_COMMA -> { return 0xBC }
      backend_api.KEY_MINUS -> { return 0xBD }
      backend_api.KEY_PERIOD -> { return 0xBE }
      backend_api.KEY_SLASH -> { return 0xBF }
      backend_api.KEY_SEMICOLON -> { return 0xBA }
      backend_api.KEY_EQUAL -> { return 0xBB }
      backend_api.KEY_LEFT_BRACKET -> { return 0xDB }
      backend_api.KEY_BACKSLASH -> { return 0xDC }
      backend_api.KEY_RIGHT_BRACKET -> { return 0xDD }
      backend_api.KEY_GRAVE_ACCENT -> { return 0xC0 }
   }
   0
}

fn get_key_name(any: win, int: key, int: scancode): str {
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
   if(len > 0){ name = utf8_from_utf16(buf) }
   free(buf)
   name
}

fn get_key_state(any: win, int: key): int {
   "Returns cached key state from the cached native window dictionary."
   if(!win || !is_dict(win)){ return 0 }
   win.get("key_states", dict(8)).get(key, false) ? 1 : 0
}

fn get_mouse_button_state(any: win, int: button): int {
   "Returns cached mouse button state from the cached native window dictionary."
   if(!win || !is_dict(win)){ return 0 }
   win.get("mouse_buttons", dict(8)).get(button, false) ? 1 : 0
}

fn ensure_window_class(any: class_name="NytrixWindowClass", any: icon_res=0): any {
   "Registers a reusable Win32 window class and returns its atom."
   if(!available()){ return 0 }
   def key = to_str(class_name)
   def existing = _registered_classes.get(key, 0)
   if(existing && is_dict(existing)){ return existing.get("atom", 0) }
   def wide_name = utf16_from_utf8(key)
   if(!wide_name){ return 0 }
   def instance = GetModuleHandleW(0)
   def wc = zalloc(80)
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
      icon = LoadImageW(
         instance, _make_int_resource(icon_res), IMAGE_ICON,
         0, 0, LR_DEFAULTSIZE | LR_SHARED,
      )
   }
   if(!icon){ icon = LoadImageW(0, _make_int_resource(IDI_APPLICATION), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED) }
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
   mut rec = dict(8)
   rec = rec.set("atom", atom)
   rec = rec.set("instance", instance)
   rec = rec.set("wide_name", wide_name)
   _registered_classes = _registered_classes.set(key, rec)
   atom
}

fn create_basic_window(any: title, int: width, int: height, int: x=CW_USEDEFAULT, int: y=CW_USEDEFAULT, int: flags=0, any: class_name="NytrixWindowClass"): any {
   "Creates a basic native Win32 window directly from Ny code."
   if(!available()){ return false }
   init_dpi_awareness()
   init_timer()
   def atom = ensure_window_class(class_name)
   if(atom == 0){ return false }
   def cls = _registered_classes.get(to_str(class_name), 0)
   if(!cls || !is_dict(cls)){ return false }
   def instance = cls.get("instance", GetModuleHandleW(0))
   def wide_title = utf16_from_utf8(title)
   if(!wide_title){ return false }
   def style = get_window_style(flags, false)
   def ex_style = get_window_ex_style(flags, false)
   def rect = zalloc(16)
   if(!rect){
      free(wide_title)
      return false
   }
   store32(rect, 0, 0)
   store32(rect, 0, 4)
   store32(rect, width, 8)
   store32(rect, height, 12)
   _adjust_window_rect(rect, style, ex_style)
   def frame_w, frame_h = _rect_width(rect), _rect_height(rect)
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
   mut win = dict(8)
   win = win.set("handle", hwnd)
   win = win.set("instance", instance)
   win = win.set("class_atom", atom)
   win = win.set("class_name", to_str(class_name))
   win = win.set("title", to_str(title))
   win = win.set("x", x)
   win = win.set("y", y)
   win = win.set("w", width)
   win = win.set("h", height)
   win = win.set("flags", flags)
   win = win.set("resizable", !band(flags, ui_consts.WINDOW_NO_RESIZE))
   win = win.set("decorated", !(band(flags, ui_consts.WINDOW_NO_BORDER) || band(flags, ui_consts.WINDOW_TRANSPARENT)))
   win = win.set("floating", band(flags, ui_consts.WINDOW_FLOATING))
   win = win.set("fullscreen", band(flags, ui_consts.WINDOW_FULLSCREEN))
   win = win.set("transparent", band(flags, ui_consts.WINDOW_TRANSPARENT))
   win = win.set("opacity", 1.0)
   win = win.set("key_states", dict(64))
   win = win.set("mouse_buttons", dict(8))
   win = win.set("mouse_x", 0)
   win = win.set("mouse_y", 0)
   win = win.set("cursor", 0)
   win = win.set("cursor_handle", 0)
   win = win.set("cursor_tracked", false)
   win = win.set("last_cursor_client_x", 0)
   win = win.set("last_cursor_client_y", 0)
   win = win.set("raw_mouse_motion", band(flags, ui_consts.WINDOW_RAW_MOUSE))
   win = win.set("cursor_mode",
      band(flags, ui_consts.WINDOW_CAPTURE_MOUSE) ? backend_api.CURSOR_CAPTURED :
   (band(flags, ui_consts.WINDOW_HIDE_MOUSE) ? backend_api.CURSOR_HIDDEN : backend_api.CURSOR_NORMAL))
   win = win.set("cursor_visible", !band(flags, ui_consts.WINDOW_HIDE_MOUSE))
   win = win.set("captured_cursor", band(flags, ui_consts.WINDOW_CAPTURE_MOUSE))
   win = win.set("disabled_cursor", false)
   win = win.set("big_icon", 0)
   win = win.set("small_icon", 0)
   win = win.set("big_icon_owned", false)
   win = win.set("small_icon_owned", false)
   win = win.set("visible", !band(flags, ui_consts.WINDOW_HIDE))
   win = _update_framebuffer_transparency(win)
   DragAcceptFiles(hwnd, 1)
   if(!band(flags, ui_consts.WINDOW_HIDE)){
      ShowWindow(hwnd, band(flags, ui_consts.WINDOW_MAXIMIZE) ? SW_SHOWMAXIMIZED :
      (band(flags, ui_consts.WINDOW_MINIMIZE) ? SW_SHOWMINIMIZED : SW_SHOWNA))
   }
   win = win.set("min_w", -1)
   win = win.set("min_h", -1)
   win = win.set("max_w", -1)
   win = win.set("max_h", -1)
   _windows = _windows.set(hwnd, win)
   win
}

fn destroy_basic_window(any: win): bool {
   "Destroys a Win32 window created by `create_basic_window`."
   if(!available()){ return false }
   if(is_dict(win) && win.get("monitor_mode_changed", false)){ _restore_video_mode(win.get("monitor_adapter_name", "")) }
   if(is_dict(win)){
      if(win.get("big_icon_owned", false) && win.get("big_icon", 0)){ DestroyIcon(win.get("big_icon", 0)) }
      if(win.get("small_icon_owned", false) && win.get("small_icon", 0)){ DestroyIcon(win.get("small_icon", 0)) }
   }
   def hwnd = _handle_from(win)
   if(!hwnd){ return false }
   DragAcceptFiles(hwnd, 0)
   DestroyWindow(hwnd) != 0
}

fn show_window(any: win, int: cmd=SW_SHOWNA): bool {
   "Shows a Win32 window."
   if(!available()){ return false }
   def hwnd = _handle_from(win)
   if(!hwnd){ return false }
   ShowWindow(hwnd, cmd) != 0
}

fn hide_window(any: win): bool {
   "Hides a Win32 window."
   show_window(win, SW_HIDE)
}

fn set_title(any: win, any: title): bool {
   "Sets a Win32 window title using a UTF-16 conversion path."
   if(!available()){ return false }
   def hwnd = _handle_from(win)
   if(!hwnd){ return false }
   def wide = utf16_from_utf8(title)
   if(!wide){ return false }
   def ok = SetWindowTextW(hwnd, wide) != 0
   free(wide)
   ok
}

fn set_window_icon(any: win, any: images): any {
   "Applies Win32 big/small icons from Ny RGBA image dictionaries."
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return win }
   mut big_icon = GetClassLongPtrW(hwnd, GCLP_HICON)
   mut small_icon = GetClassLongPtrW(hwnd, GCLP_HICONSM)
   mut big_owned = false
   mut small_owned = false
   if((is_list(images) || is_tuple(images)) && images.len > 0){
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
   if(win.get("big_icon_owned", false) && win.get("big_icon", 0)){ DestroyIcon(win.get("big_icon", 0)) }
   if(win.get("small_icon_owned", false) && win.get("small_icon", 0)){ DestroyIcon(win.get("small_icon", 0)) }
   win = win.set("big_icon", big_icon)
   win = win.set("small_icon", small_icon)
   win = win.set("big_icon_owned", big_owned)
   win = win.set("small_icon_owned", small_owned)
   win = win.set("icon_images", images)
   win
}

fn _window_client_pos(any: win, bool: fallback_cached=false): list {
   if(!available() || !win || !is_dict(win)){ return [0, 0] }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return [0, 0] }
   def pt = zalloc(8)
   if(!pt){
      if(fallback_cached){ return [win.get("x", 0), win.get("y", 0)] }
      return [0, 0]
   }
   store32(pt, 0, 0)
   store32(pt, 0, 4)
   mut out = fallback_cached ? [win.get("x", 0), win.get("y", 0)] : [0, 0]
   if(ClientToScreen(hwnd, pt) != 0){ out = [load32(pt, 0), load32(pt, 4)] }
   free(pt)
   out
}

fn _window_client_size(any: win, bool: fallback_cached=false): list {
   if(!available() || !win || !is_dict(win)){ return [0, 0] }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return [0, 0] }
   def rect = zalloc(16)
   if(!rect){
      if(fallback_cached){ return [win.get("w", 0), win.get("h", 0)] }
      return [0, 0]
   }
   mut out = fallback_cached ? [win.get("w", 0), win.get("h", 0)] : [0, 0]
   if(GetClientRect(hwnd, rect) != 0){ out = [_rect_width(rect), _rect_height(rect)] }
   free(rect)
   out
}

fn get_pos(any: win): list {
   "Returns the window client-area origin in screen coordinates."
   _window_client_pos(win, true)
}

fn set_pos(any: win, int: x, int: y): bool {
   "Sets the Win32 window client-area origin."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def rect = zalloc(16)
   if(!rect){ return false }
   store32(rect, x, 0)
   store32(rect, y, 4)
   store32(rect, x, 8)
   store32(rect, y, 12)
   _adjust_window_rect(rect,
      get_window_style(_effective_flags(win), win.get("fullscreen", false)),
      get_window_ex_style(_effective_flags(win), win.get("fullscreen", false)),
   hwnd)
   def ok = SetWindowPos(hwnd, 0, load32(rect, 0), load32(rect, 4), 0, 0,
   SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE) != 0
   free(rect)
   ok
}

fn get_size(any: win): list {
   "Returns the Win32 client-area size."
   _window_client_size(win, true)
}

fn get_framebuffer_size(any: win): list {
   "Returns the framebuffer size, matching Win32 client size."
   get_size(win)
}

fn set_size(any: win, int: width, int: height): bool {
   "Sets the Win32 client-area size."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def rect = zalloc(16)
   if(!rect){ return false }
   store32(rect, 0, 0)
   store32(rect, 0, 4)
   store32(rect, width, 8)
   store32(rect, height, 12)
   _adjust_window_rect(rect,
      get_window_style(_effective_flags(win), win.get("fullscreen", false)),
      get_window_ex_style(_effective_flags(win), win.get("fullscreen", false)),
   hwnd)
   def ok = SetWindowPos(hwnd, HWND_TOP, 0, 0, _rect_width(rect), _rect_height(rect),
   SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOZORDER) != 0
   free(rect)
   ok
}

comptime template _win32_showwindow_action(name, doc, cmd){
   fn ${name}(any: win): bool {
      doc
      if(!available() || !win || !is_dict(win)){ return false }
      ShowWindow(win.get("handle", 0), cmd) != 0
   }
}

comptime emit _win32_showwindow_action(iconify_window, "Iconifies a Win32 window.", SW_MINIMIZE)
comptime emit _win32_showwindow_action(restore_window, "Restores a minimized or maximized Win32 window.", SW_RESTORE)
comptime emit _win32_showwindow_action(maximize_window, "Maximizes a Win32 window.", SW_SHOWMAXIMIZED)

fn request_window_attention(any: win): bool {
   "Requests user attention for a Win32 window."
   if(!available() || !win || !is_dict(win)){ return false }
   FlashWindow(win.get("handle", 0), backend_api.TRUE) != 0
}

fn focus_window(any: win): bool {
   "Brings a Win32 window to the foreground and focuses it."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   BringWindowToTop(hwnd)
   SetForegroundWindow(hwnd)
   SetFocus(hwnd) != 0
}

comptime template _win32_hwnd_bool(name, doc, call_fn){
   fn ${name}(any: win): bool {
      doc
      if(!available() || !win || !is_dict(win)){ return false }
      call_fn(win.get("handle", 0)) != 0
   }
}

fn get_window_attrib(any: win, int: attrib): int {
   "Unified getter for Win32 window attributes."
   if(!win || !is_dict(win)){ return 0 }
   match attrib {
      backend_api.RESIZABLE -> { return win.get("resizable", true) ? 1 : 0 }
      backend_api.VISIBLE -> { return is_window_visible(win) ? 1 : 0 }
      backend_api.DECORATED -> { return win.get("decorated", true) ? 1 : 0 }
      backend_api.FOCUSED -> { return is_window_focused(win) ? 1 : 0 }
      backend_api.FLOATING -> { return win.get("floating", false) ? 1 : 0 }
      backend_api.MAXIMIZED -> { return is_window_maximized(win) ? 1 : 0 }
      backend_api.TRANSPARENT_FRAMEBUFFER -> { return win.get("transparent", false) ? 1 : 0 }
      _ -> { return 0 }
   }
}

fn is_window_focused(any: win): bool {
   "Returns true if the Win32 window is the active window."
   if(!available() || !win || !is_dict(win)){ return false }
   win.get("handle", 0) == GetActiveWindow()
}

comptime emit _win32_hwnd_bool(is_window_iconified, "Returns true if the Win32 window is minimized.", IsIconic)
comptime emit _win32_hwnd_bool(is_window_visible, "Returns true if the Win32 window is visible.", IsWindowVisible)
comptime emit _win32_hwnd_bool(is_window_maximized, "Returns true if the Win32 window is maximized.", IsZoomed)

fn is_window_hovered(any: win): bool {
   "Returns true if the cursor is currently over the Win32 client area."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def pt = zalloc(8)
   if(!pt){ return false }
   def ok = GetCursorPos(pt) != 0 && WindowFromPoint(pt) == hwnd
   free(pt)
   ok
}

fn get_window_opacity(any: win): f64 {
   "Returns the current Win32 layered-window opacity."
   if(!available() || !win || !is_dict(win)){ return 1.0 }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return 1.0 }
   def ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE)
   if(!band(ex_style, WS_EX_LAYERED)){ return 1.0 }
   def alpha = zalloc(1)
   def flags = zalloc(4)
   if(!alpha || !flags){
      if(alpha){ free(alpha) }
      if(flags){ free(flags) }
      return 1.0
   }
   mut out = 1.0
   if(GetLayeredWindowAttributes(hwnd, 0, alpha, flags) != 0 &&
      band(load32(flags, 0), LWA_ALPHA)){
      out = float(load8(alpha, 0) & 255) / 255.0
   }
   free(alpha, flags)
   out
}

fn set_window_opacity(any: win, f64: opacity): bool {
   "Applies whole-window opacity using layered window attributes."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
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

fn get_window_pos(any: win): list {
   "Returns the top-left screen coordinates [x, y] of a Win32 window's client area."
   _window_client_pos(win, false)
}

fn set_window_pos(any: win, int: x, int: y): bool {
   "Moves a Win32 window so its client area starts at screen coordinates [x, y]."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def style = GetWindowLongW(hwnd, GWL_STYLE)
   def ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE)
   def rect = zalloc(16)
   if(!rect){ return false }
   store32(rect, x, 0)
   store32(rect, y, 4)
   store32(rect, x, 8)
   store32(rect, y, 12)
   _adjust_window_rect(rect, style, ex_style)
   def final_x, final_y = load32(rect, 0), load32(rect, 4)
   free(rect)
   SetWindowPos(hwnd, 0, final_x, final_y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER) != 0
}

fn get_window_size(any: win): list {
   "Returns the pixel size [width, height] of a Win32 window's client area."
   _window_client_size(win, false)
}

fn set_window_size(any: win, int: w, int: h): bool {
   "Resizes a Win32 window so its client area has width `w` and height `h`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def style = GetWindowLongW(hwnd, GWL_STYLE)
   def ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE)
   def rect = zalloc(16)
   if(!rect){ return false }
   store32(rect, 0, 0)
   store32(rect, 0, 4)
   store32(rect, w, 8)
   store32(rect, h, 12)
   _adjust_window_rect(rect, style, ex_style)
   def fw, fh = _rect_width(rect), _rect_height(rect)
   free(rect)
   SetWindowPos(hwnd, 0, 0, 0, fw, fh, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER) != 0
}

fn set_window_size_limits(any: win, int: min_w, int: min_h, int: max_w, int: max_h): bool {
   "Applies window size constraints tracked via WM_GETMINMAXINFO."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   _windows = _windows.set(hwnd,
   win.set("min_w", min_w).set("min_h", min_h).set("max_w", max_w).set("max_h", max_h))
   true
}

fn _set_style_flag(any: win, str: key, bool: enabled): any {
   if(!available() || !win || !is_dict(win)){ return win }
   _update_window_styles(win.set(key, !!enabled))
}

fn set_window_resizable(any: win, bool: enabled): any {
   "Updates the Win32 resizable style flag."
   _set_style_flag(win, "resizable", enabled)
}

fn set_window_decorated(any: win, bool: enabled): any {
   "Updates the Win32 decorated style flag."
   _set_style_flag(win, "decorated", enabled)
}

fn set_window_floating(any: win, bool: enabled): any {
   "Updates the Win32 topmost state."
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = win.get("handle", 0)
   if(hwnd){
      SetWindowPos(hwnd, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
      SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE)
   }
   win.set("floating", !!enabled)
}

fn set_clipboard(any: win, any: s): bool {
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

fn get_clipboard(any: win): str {
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

fn get_cursor_pos(any: win): list {
   "Returns the current cursor position relative to the Win32 client area."
   if(!available() || !win || !is_dict(win)){ return [0.0, 0.0] }
   if(win.get("disabled_cursor", false)){ return [float(win.get("mouse_x", 0)), float(win.get("mouse_y", 0))] }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return [0.0, 0.0] }
   def pt = zalloc(8)
   if(!pt){ return [0.0, 0.0] }
   mut out = [float(win.get("mouse_x", 0)), float(win.get("mouse_y", 0))]
   if(GetCursorPos(pt) != 0 && ScreenToClient(hwnd, pt) != 0){ out = [float(load32(pt, 0)), float(load32(pt, 4))] }
   free(pt)
   out
}

fn set_cursor_pos(any: win, any: x, any: y): bool {
   "Sets the Win32 cursor position in client-area coordinates."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def pt = zalloc(8)
   if(!pt){ return false }
   store32(pt, int(x), 0)
   store32(pt, int(y), 4)
   def ok = ClientToScreen(hwnd, pt) != 0 && SetCursorPos(load32(pt, 0), load32(pt, 4)) != 0
   free(pt)
   ok
}

fn create_standard_cursor(int: shape): any {
   "Create a shared Win32 standard cursor for a backend cursor shape."
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
   mut cursor = dict(8)
   cursor = cursor.set("handle", handle)
   cursor = cursor.set("shared", true)
   cursor = cursor.set("shape", shape)
   cursor
}

fn create_cursor(any: image, int: xhot=0, int: yhot=0): any {
   "Creates a Win32 cursor from a Ny RGBA8 image dictionary."
   if(!available()){ return 0 }
   def handle = _create_rgba_cursor_handle(image, xhot, yhot, false)
   if(!handle){ return 0 }
   mut cursor = dict(8)
   cursor = cursor.set("handle", handle)
   cursor = cursor.set("shared", false)
   cursor = cursor.set("image", image)
   cursor = cursor.set("xhot", xhot)
   cursor = cursor.set("yhot", yhot)
   cursor
}

fn destroy_cursor(any: cursor): bool {
   "Destroys a Win32 cursor object when it owns native resources."
   if(!cursor || !is_dict(cursor)){ return true }
   if(!cursor.get("shared", false)){
      def handle = cursor.get("handle", 0)
      if(handle){ DestroyIcon(handle) }
   }
   true
}

fn set_cursor(any: win, any: cursor): any {
   "Applies a cursor object to a Win32 window dictionary."
   if(!win || !is_dict(win)){ return win }
   win = win.set("cursor", cursor)
   _apply_cursor_handle(win)
}

fn set_input_mode(any: win, int: mode, any: value): any {
   "Applies cursor and raw-mouse modes to a Win32 window dictionary."
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return win }
   if(mode == backend_api.RAW_MOUSE_MOTION){
      win = win.set("raw_mouse_motion", value != 0)
      if(win.get("disabled_cursor", false)){
         if(value != 0){ _enable_raw_mouse_motion(hwnd) }
         else { _disable_raw_mouse_motion() }
      }
      return win
   }
   if(mode != backend_api.CURSOR){ return win }
   def previous = win.get("cursor_mode", backend_api.CURSOR_NORMAL)
   if(previous == backend_api.CURSOR_DISABLED && win.get("raw_mouse_motion", false)){ _disable_raw_mouse_motion() }
   if(value == backend_api.CURSOR_NORMAL){
      _release_cursor()
      _show_cursor(true)
      win = _apply_cursor_handle(win)
      if(previous == backend_api.CURSOR_DISABLED){
         set_cursor_pos(win,
            win.get("restore_cursor_x", win.get("mouse_x", 0)),
         win.get("restore_cursor_y", win.get("mouse_y", 0)))
      }
      win = win.set("captured_cursor", false)
      win = win.set("disabled_cursor", false)
      win = win.set("cursor_visible", true)
      return win.set("cursor_mode", value)
   }
   if(value == backend_api.CURSOR_HIDDEN){
      _release_cursor()
      _show_cursor(false)
      win = win.set("captured_cursor", false)
      win = win.set("disabled_cursor", false)
      win = win.set("cursor_visible", false)
      return win.set("cursor_mode", value)
   }
   if(value == backend_api.CURSOR_CAPTURED){
      _capture_cursor(hwnd)
      _show_cursor(true)
      win = _apply_cursor_handle(win)
      win = win.set("captured_cursor", true)
      win = win.set("disabled_cursor", false)
      win = win.set("cursor_visible", true)
      return win.set("cursor_mode", value)
   }
   if(value == backend_api.CURSOR_DISABLED){
      def pos = get_cursor_pos(win)
      def center_x = int(win.get("w", 1) / 2)
      def center_y = int(win.get("h", 1) / 2)
      win = win.set("restore_cursor_x", int(pos.get(0, 0.0)))
      win = win.set("restore_cursor_y", int(pos.get(1, 0.0)))
      win = win.set("mouse_x", int(pos.get(0, 0.0)))
      win = win.set("mouse_y", int(pos.get(1, 0.0)))
      _capture_cursor(hwnd)
      _show_cursor(false)
      if(win.get("raw_mouse_motion", false)){ _enable_raw_mouse_motion(hwnd) }
      set_cursor_pos(win, center_x, center_y)
      win = win.set("last_cursor_client_x", center_x)
      win = win.set("last_cursor_client_y", center_y)
      win = win.set("captured_cursor", true)
      win = win.set("disabled_cursor", true)
      win = win.set("cursor_visible", false)
      return win.set("cursor_mode", value)
   }
   win
}

fn _message_to_dict(any: msg): dict {
   mut out = dict(8)
   out = out.set("hwnd", load64_h(msg, 0))
   out = out.set("message", load32(msg, 8))
   out = out.set("wparam", load64_h(msg, 16))
   out = out.set("lparam", load64_h(msg, 24))
   out = out.set("time", load32(msg, 32))
   out = out.set("x", load32(msg, 36))
   out = out.set("y", load32(msg, 40))
   out
}

fn _synthetic_key_release(any: win, list: events, int: vk, int: key): list {
   if(!win || !is_dict(win)){ return [win, events] }
   if(GetKeyState(vk) & 0x8000){ return [win, events] }
   mut ks = win.get("key_states", dict(64))
   if(!ks.get(key, false)){ return [win, events] }
   ks = ks.set(key, false)
   win = win.set("key_states", ks)
   mut data = dict(8)
   data = data.set("raw_key", vk)
   data = data.set("key", key)
   data = data.set("scancode", MapVirtualKeyW(vk, MAPVK_VK_TO_VSC))
   data = data.set("action", backend_api.ACTION_RELEASE)
   data = data.set("mods", _mods_for_key_event(win, key, false))
   _push_event(events, ui_consts.EVENT_KEY_RELEASED, win, data)
   [win, events]
}

fn _sync_special_key_releases(any: win, list: events): list {
   if(!win || !is_dict(win) || GetActiveWindow() != win.get("handle", 0)){ return [win, events] }
   def left_shift = _synthetic_key_release(win, events, VK_LSHIFT, backend_api.KEY_LEFT_SHIFT)
   win, events = left_shift.get(0, win), left_shift.get(1, events)
   def right_shift = _synthetic_key_release(win, events, VK_RSHIFT, backend_api.KEY_RIGHT_SHIFT)
   win, events = right_shift.get(0, win), right_shift.get(1, events)
   def left_super = _synthetic_key_release(win, events, VK_LWIN, backend_api.KEY_LEFT_SUPER)
   win, events = left_super.get(0, win), left_super.get(1, events)
   def right_super = _synthetic_key_release(win, events, VK_RWIN, backend_api.KEY_RIGHT_SUPER)
   win, events = right_super.get(0, win), right_super.get(1, events)
   [win, events]
}

fn _recenter_disabled_cursor(any: win): any {
   if(!win || !is_dict(win) || !win.get("disabled_cursor", false)){ return win }
   def center_x, center_y = int(max(1, win.get("w", 1)) / 2), int(max(1, win.get("h", 1)) / 2)
   if(win.get("last_cursor_client_x", center_x) != center_x ||
      win.get("last_cursor_client_y", center_y) != center_y){
      set_cursor_pos(win, center_x, center_y)
      win = win.set("last_cursor_client_x", center_x)
      win = win.set("last_cursor_client_y", center_y)
   }
   win
}

fn wait_messages(int: timeout_ms=-1): bool {
   "Blocks until a new message is posted to the thread's queue or timeout elapses."
   if(!available()){ return false }
   def ms = (timeout_ms < 0) ? 0xffffffff : int(timeout_ms)
   MsgWaitForMultipleObjects(0, 0, 0, ms, 0x04FF) ;; 0x04FF = QS_ALLINPUT
   true
}

fn poll_messages(any: hwnd=0, int: max_messages=64, bool: remove=true): list {
   "Polls raw Win32 messages into a list of dicts."
   if(!available()){ return [] }
   def msg = zalloc(48)
   if(!msg){ return [] }
   mut out = []
   mut n = 0
   while(n < max_messages && PeekMessageW(msg, hwnd, 0, 0, remove ? PM_REMOVE : PM_NOREMOVE) != 0){
      out.append(_message_to_dict(msg))
      n += 1
      if(!remove){ break }
   }
   free(msg)
   out
}

fn pump_window_messages(any: hwnd=0, int: max_messages=64): list {
   "Polls and dispatches queued Win32 messages."
   if(!available()){ return [] }
   def msg = zalloc(48)
   if(!msg){ return [] }
   mut out = []
   mut n = 0
   while(n < max_messages && PeekMessageW(msg, hwnd, 0, 0, PM_REMOVE) != 0){
      out.append(_message_to_dict(msg))
      TranslateMessage(msg)
      DispatchMessageW(msg)
      n += 1
   }
   free(msg)
   out
}

fn poll_window_events(any: win, int: max_messages=64): list {
   "Polls and translates Win32 messages for a backend window."
   if(!available() || !win || !is_dict(win)){ return [win, []] }
   def messages = pump_window_messages(win.get("handle", 0), max_messages)
   def res = translate_messages(win, messages)
   win = res.get(0, win)
   mut events = res.get(1, [])
   def synced = _sync_special_key_releases(win, events)
   win, events = synced.get(0, win), synced.get(1, events)
   win = _recenter_disabled_cursor(win)
   [win, events]
}

fn primary_screen_size(): list {
   "Returns the primary screen size using GetSystemMetrics."
   if(!available()){ return [0, 0] }
   [GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)]
}

fn get_monitors(): list {
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
         if(monitor.get("primary", false)){ primary = primary.append(monitor) }
         else { others = others.append(monitor) }
      }
      if(!found_display){
         def monitor = _create_monitor_from_devices(adapter, 0)
         if(monitor){
            if(monitor.get("primary", false)){ primary = primary.append(monitor) }
            else { others = others.append(monitor) }
         }
      }
      free(adapter)
   }
   mut out = primary.extend(others)
   mut i = 0
   def out_n = out.len
   while(i < out_n){
      def monitor = out.get(i)
      if(monitor && is_dict(monitor)){ out = out.set(i, monitor.set("index", i)) }
      i += 1
   }
   out
}

fn get_primary_monitor(): any {
   "Returns the primary Win32 monitor dict."
   def monitors = get_monitors()
   if(monitors.len == 0){ return false }
   monitors.get(0)
}

fn get_monitor_pos(any: monitor): list {
   "Returns `[x, y]` for a monitor dict."
   if(!monitor || !is_dict(monitor)){ return [0, 0] }
   [monitor.get("x", 0), monitor.get("y", 0)]
}

fn get_monitor_workarea(any: monitor): list {
   "Returns `[x, y, width, height]` for a monitor work area."
   if(!monitor || !is_dict(monitor)){ return [0, 0, 0, 0] }
   [
      monitor.get("work_x", monitor.get("x", 0)),
      monitor.get("work_y", monitor.get("y", 0)),
      monitor.get("work_w", monitor.get("width", 0)),
      monitor.get("work_h", monitor.get("height", 0))
   ]
}

fn get_monitor_physical_size(any: monitor): list {
   "Returns `[width_mm, height_mm]` for a monitor dict."
   if(!monitor || !is_dict(monitor)){ return [0, 0] }
   [monitor.get("width_mm", 0), monitor.get("height_mm", 0)]
}

fn get_monitor_content_scale(any: monitor): list {
   "Returns `[xscale, yscale]` for a monitor dict."
   if(!monitor || !is_dict(monitor)){ return [1.0, 1.0] }
   [monitor.get("scale_x", 1.0), monitor.get("scale_y", 1.0)]
}

fn get_monitor_name(any: monitor): str {
   "Returns the UTF-8 display name for a monitor dict."
   if(!monitor || !is_dict(monitor)){ return "" }
   monitor.get("name", "")
}

fn get_video_mode(any: monitor): any {
   "Returns the current window video mode dict for a monitor."
   if(!available() || !monitor || !is_dict(monitor)){ return false }
   def adapter = monitor.get("adapter_name", "")
   if(adapter.len == 0){ return false }
   def wide = utf16_from_utf8(adapter)
   def dm = _alloc_devmode()
   if(!wide || !dm){
      if(wide){ free(wide) }
      if(dm){ free(dm) }
      return false
   }
   if(EnumDisplaySettingsW(wide, ENUM_CURRENT_SETTINGS, dm) == 0){
      free(wide, dm)
      return false
   }
   def mode = _devmode_to_vidmode(dm)
   free(wide, dm)
   mode
}

fn get_video_modes(any: monitor): list {
   "Returns distinct window video modes for a monitor."
   if(!available() || !monitor || !is_dict(monitor)){ return [] }
   def adapter = monitor.get("adapter_name", "")
   if(adapter.len == 0){ return [] }
   def wide = utf16_from_utf8(adapter)
   if(!wide){ return [] }
   mut out = []
   mut seen = dict(8)
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
      def mw = mode.get("width", 0)
      def mh = mode.get("height", 0)
      def mr = mode.get("refresh_rate", 0)
      def mred = mode.get("red_bits", 0)
      def mgreen = mode.get("green_bits", 0)
      def mblue = mode.get("blue_bits", 0)
      def key = f"{mw}x{mh}@{mr}:{mred}/{mgreen}/{mblue}"
      if(!seen.get(key, false)){
         seen = seen.set(key, true)
         out = out.append(mode)
      }
      free(dm)
   }
   free(wide)
   if(out.len == 0){
      def current = get_video_mode(monitor)
      if(current){ out = out.append(current) }
   }
   out
}

fn get_gamma_ramp(any: monitor): any {
   "Returns the current Win32 gamma ramp as a dict with `size`, `red`, `green`, and `blue`."
   if(!available() || !monitor || !is_dict(monitor)){ return false }
   def adapter = monitor.get("adapter_name", "")
   if(adapter.len == 0){ return false }
   def adapter_wide = utf16_from_utf8(adapter)
   def driver = _driver_display_wide()
   def values = zalloc(1536)
   if(!adapter_wide || !driver || !values){
      if(adapter_wide){ free(adapter_wide) }
      if(driver){ free(driver) }
      if(values){ free(values) }
      return false
   }
   def dc = CreateDCW(driver, adapter_wide, 0, 0)
   free(driver, adapter_wide)
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

fn set_gamma_ramp(any: monitor, any: ramp): bool {
   "Sets the Win32 gamma ramp from a dict containing `size`, `red`, `green`, and `blue` arrays."
   if(!available() || !monitor || !is_dict(monitor) || !ramp || !is_dict(ramp)){ return false }
   def size = ramp.get("size", 0)
   if(size != 256){ return false }
   def values = zalloc(1536)
   if(!values){ return false }
   def red = ramp.get("red", [])
   def green = ramp.get("green", [])
   def blue = ramp.get("blue", [])
   if(!_copy_gamma_channel(red, values, 0, size) ||
      !_copy_gamma_channel(green, values, 512, size) ||
      !_copy_gamma_channel(blue, values, 1024, size)){
      free(values)
      return false
   }
   def adapter = monitor.get("adapter_name", "")
   def adapter_wide = utf16_from_utf8(adapter)
   def driver = _driver_display_wide()
   if(!adapter_wide || !driver){
      if(adapter_wide){ free(adapter_wide) }
      if(driver){ free(driver) }
      free(values)
      return false
   }
   def dc = CreateDCW(driver, adapter_wide, 0, 0)
   free(driver, adapter_wide)
   if(!dc){
      free(values)
      return false
   }
   def ok = SetDeviceGammaRamp(dc, values) != 0
   DeleteDC(dc)
   free(values)
   ok
}

fn get_window_monitor(any: win): any {
   "Returns the monitor dict nearest the specified Win32 window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def handle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
   if(!handle){ return false }
   def monitors = get_monitors()
   mut i = 0
   def monitors_n = monitors.len
   while(i < monitors_n){
      def monitor = monitors.get(i)
      if(monitor && is_dict(monitor) && monitor.get("handle", 0) == handle){ return monitor }
      i += 1
   }
   false
}

fn set_window_monitor(any: win, any: monitor, int: xpos, int: ypos, int: width, int: height, int: refresh_rate=0): any {
   "Applies a basic basic monitor/fullscreen transition for Win32."
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return win }
   if(win.get("monitor_mode_changed", false)){
      _restore_video_mode(win.get("monitor_adapter_name", ""))
      win = win.set("monitor_mode_changed", false)
   }
   if(monitor && is_dict(monitor)){
      if(!win.get("monitor", false)){
         win = win.set("windowed_x", win.get("x", xpos))
         win = win.set("windowed_y", win.get("y", ypos))
         win = win.set("windowed_w", win.get("w", width))
         win = win.set("windowed_h", win.get("h", height))
      }
      mut mode = get_video_mode(monitor)
      def mode_changed = _set_video_mode(monitor, width, height, refresh_rate)
      if(mode_changed){ mode = get_video_mode(monitor) }
      def mon_x, mon_y = monitor.get("x", xpos), monitor.get("y", ypos)
      def mon_w = max(width, mode.get("width", monitor.get("width", width)))
      def mon_h = max(height, mode.get("height", monitor.get("height", height)))
      win = win.set("monitor", monitor)
      win = win.set("monitor_adapter_name", monitor.get("adapter_name", ""))
      win = win.set("monitor_mode_changed", mode_changed)
      win = win.set("fullscreen", true)
      win = win.set("flags", bor(int(win.get("flags", 0)), ui_consts.WINDOW_FULLSCREEN))
      win = _update_window_styles(win)
      SetWindowPos(hwnd, HWND_TOPMOST, mon_x, mon_y, mon_w, mon_h, SWP_FRAMECHANGED | SWP_SHOWWINDOW)
      win = win.set("x", mon_x)
      win = win.set("y", mon_y)
      win = win.set("w", mon_w)
      win = win.set("h", mon_h)
      return win
   }
   win = win.set("monitor", false)
   win = win.set("monitor_adapter_name", "")
   win = win.set("monitor_mode_changed", false)
   win = win.set("fullscreen", false)
   win = win.set("flags", band(int(win.get("flags", 0)), bnot(ui_consts.WINDOW_FULLSCREEN)))
   win = _update_window_styles(win)
   def rx, ry = win.get("windowed_x", xpos), win.get("windowed_y", ypos)
   def rw, rh = max(1, win.get("windowed_w", width)), max(1, win.get("windowed_h", height))
   SetWindowPos(hwnd, HWND_NOTOPMOST, rx, ry, rw, rh, SWP_FRAMECHANGED | SWP_SHOWWINDOW)
   win = win.set("x", rx)
   win = win.set("y", ry)
   win = win.set("w", rw)
   win = win.set("h", rh)
   win
}

fn vulkan_supported(): bool {
   "Returns true when the Win32 backend supports Vulkan."
   true
}

mut _win32_vk_ext_ptrs = 0
mut _win32_vk_ext_surface = 0
mut _win32_vk_ext_win32 = 0

fn vulkan_required_extensions(): list {
   "Returns the Vulkan instance extensions required for a Win32 surface."
   if(!_win32_vk_ext_ptrs){
      _win32_vk_ext_surface = cstr("VK_KHR_surface")
      _win32_vk_ext_win32 = cstr("VK_KHR_win32_surface")
      def arr = malloc(16)
      if(!arr){ return [0, 0] }
      store64_h(arr, _win32_vk_ext_surface, 0)
      store64_h(arr, _win32_vk_ext_win32, 8)
      _win32_vk_ext_ptrs = [2, arr]
   }
   _win32_vk_ext_ptrs
}

fn create_surface(any: instance, any: win, any: allocator, any: surface_out): int {
   "Creates a Vulkan Win32 surface from a native Win32 window."
   if(!available()){ return -1 }
   def hwnd = _handle_from(win)
   if(!instance || !hwnd || !surface_out){ return -1 }
   def info = zalloc(48)
   if(!info){ return -1 }
   store32(info, VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0)
   store64_h(info, _instance_from_window(win), 24)
   store64_h(info, hwnd, 32)
   def res = vkCreateWin32SurfaceKHR(instance, info, allocator, surface_out)
   free(info)
   res
}

fn set_video_mode(any: monitor, int: width, int: height, int: refresh_rate=0): bool {
   "Public wrapper: sets video mode on a Win32 monitor via ChangeDisplaySettingsExW."
   _set_video_mode(monitor, width, height, refresh_rate)
}

fn restore_video_mode(any: monitor): bool {
   "Public wrapper: restores the original video mode on a Win32 monitor."
   if(!monitor || !is_dict(monitor)){ return false }
   _restore_video_mode(monitor.get("adapter_name", ""))
}

fn get_win32_window(any: win): any {
   "Returns the Win32 HWND handle for the given window dict."
   if(is_dict(win)){ win.get("handle", 0) } else { win }
}

fn get_win32_monitor(any: mon): any {
   "Returns the Win32 HMONITOR handle for the given monitor dict."
   if(is_dict(mon)){ mon.get("handle", 0) } else { 0 }
}

mut _adapter_wide_cache = dict(4)

fn get_win32_adapter(any: mon): any {
   "Returns a wide-string pointer to the Win32 adapter device name for the given monitor."
   if(!is_dict(mon)){ return 0 }
   def name = mon.get("adapter_name", "")
   if(name.len == 0){ return 0 }
   def cached = _adapter_wide_cache.get(name, 0)
   if(cached){ return cached }
   def wide = utf16_from_utf8(name)
   _adapter_wide_cache = _adapter_wide_cache.set(name, wide)
   wide
}
