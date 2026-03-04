;; Native Windowing Backend - X11 (GLFW-ported, Linux)
;; Port of GLFW x11_init.c / x11_window.c / x11_monitor.c
;; No GLFW FFI — direct X11/XRandR/XInput2/Xkb syscalls via dlsym

module std.ui.window.backend (
   create_window, close_window, check_event, poll_events,
   set_title, get_pos, get_framebuffer_size, move_window, resize_window,
   should_close, swap_buffers, swap_interval,
   set_cursor_mode, get_cursor_pos, set_cursor_pos, focus_window,
   set_clipboard, get_clipboard,
   vulkan_supported, create_vk_surface, get_required_vk_extensions,
   x11_display, x11_visual_id,
   joystick_present, joystick_is_gamepad, get_joystick_name, get_joystick_guid,
   get_joystick_axes, get_joystick_buttons, get_gamepad_state, get_gamepad_name,
   update_gamepad_mappings,
   set_fullscreen, set_borderless, set_floating, set_opacity,
   minimize, maximize, restore, is_maximized, is_minimized, is_visible,
   get_monitor_size, get_monitor_pos, get_monitor_name,
   get_monitor_count, get_monitor_workarea, get_monitor_physical_size,
   get_monitor_content_scale, get_monitor_refresh_rate, get_monitor_video_modes,
   get_time, set_time, get_timer_value, get_timer_frequency,
   request_attention, set_resizable, wait_events, set_mouse_passthrough,
   show_window, hide_window, post_empty_event,
   set_cursor_shape, create_cursor, destroy_cursor, set_cursor,
   raw_mouse_motion_supported, set_raw_mouse_motion,
   set_window_size_limits, set_window_aspect_ratio, set_window_monitor,
   get_window_content_scale, get_window_frame_size, get_window_attrib, set_window_attrib,
   set_window_user_pointer, get_window_user_pointer,
   is_focused, is_hovered,
   get_joystick_hats, set_joystick_user_pointer, get_joystick_user_pointer,
   get_key_scancode,
   get_backend_name, blit_buffer
)

use std.core *
use std.core.mem *
use std.os.ffi *
use std.str as text

;; ═══════════════════════════════════════════════════════════════════════════
;; §1  CONSTANTS
;; ═══════════════════════════════════════════════════════════════════════════

;; X11 event types
def X_KeyPress         = 2
def X_KeyRelease       = 3
def X_ButtonPress      = 4
def X_ButtonRelease    = 5
def X_MotionNotify     = 6
def X_EnterNotify      = 7
def X_LeaveNotify      = 8
def X_FocusIn          = 9
def X_FocusOut         = 10
def X_ConfigureNotify  = 22
def X_MapNotify        = 19
def X_UnmapNotify      = 18
def X_DestroyNotify    = 17
def X_ClientMessage    = 33
def X_SelectionRequest = 30
def X_SelectionNotify  = 31
def X_PropertyNotify   = 28
def X_Expose           = 12

;; XCreateWindow masks
def X_CWBorderPixel  = 8
def X_CWColormap     = 8192
def X_CWEventMask    = 2048
def X_CWBackPixel    = 2

;; Window classes
def X_InputOutput    = 1
def X_AllocNone      = 0
def X_TrueColor      = 4

;; Event masks
def XM_KeyPress          = 1
def XM_KeyRelease        = 2
def XM_ButtonPress       = 4
def XM_ButtonRelease     = 8
def XM_PointerMotion     = 64
def XM_StructureNotify   = 131072
def XM_FocusChange       = 2097152
def XM_PropertyChange    = 4194304
def XM_Exposure          = 32768
def XM_EnterWindow       = 16
def XM_LeaveWindow       = 32

;; PropMode
def X_PropModeReplace    = 0

;; NET_WM_STATE actions
def NET_WM_STATE_REMOVE  = 0
def NET_WM_STATE_ADD     = 1
def NET_WM_STATE_TOGGLE  = 2

;; VkStructureType for surfaces
def VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR     = 1000004000
def VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR  = 1000006000

;; Window flags
def WINDOW_TRANSPARENT  = 1
def WINDOW_BORDERLESS   = 2
def WINDOW_RESIZABLE    = 4
def WINDOW_FLOATING     = 8

;; XRandR rotation
def RR_Rotate_0   = 1

;; ═══════════════════════════════════════════════════════════════════════════
;; §2  SHARED LIBRARY HANDLES
;; ═══════════════════════════════════════════════════════════════════════════

mut _lib_x11    = 0
mut _lib_xrandr = 0
mut _lib_xi     = 0
mut _lib_xcursor= 0
mut _lib_vk     = 0

;; ═══════════════════════════════════════════════════════════════════════════
;; §3  X11 FUNCTION POINTERS (GLFW-style lazy dlsym)
;; ═══════════════════════════════════════════════════════════════════════════

;; Core Xlib
mut _fp_XOpenDisplay          = 0
mut _fp_XCloseDisplay         = 0
mut _fp_XDefaultRootWindow    = 0
mut _fp_XDefaultScreen        = 0
mut _fp_XDisplayWidth         = 0
mut _fp_XDisplayHeight        = 0
mut _fp_XCreateWindow         = 0
mut _fp_XDestroyWindow        = 0
mut _fp_XMapWindow            = 0
mut _fp_XUnmapWindow          = 0
mut _fp_XFlush                = 0
mut _fp_XSync                 = 0
mut _fp_XPending              = 0
mut _fp_XNextEvent            = 0
mut _fp_XCheckMaskEvent       = 0
mut _fp_XCheckTypedWindowEvent = 0
mut _fp_XCheckIfEvent         = 0
mut _fp_XFilterEvent          = 0
mut _fp_XInternAtom           = 0
mut _fp_XSetWMProtocols       = 0
mut _fp_XGetWindowAttributes  = 0
mut _fp_XGetWindowProperty    = 0
mut _fp_XChangeProperty       = 0
mut _fp_XDeleteProperty       = 0
mut _fp_XSendEvent            = 0
mut _fp_XMoveWindow           = 0
mut _fp_XResizeWindow         = 0
mut _fp_XMoveResizeWindow     = 0
mut _fp_XRaiseWindow          = 0
mut _fp_XIconifyWindow        = 0
mut _fp_XStoreName            = 0
mut _fp_XAllocSizeHints       = 0
mut _fp_XAllocWMHints         = 0
mut _fp_XSetWMNormalHints     = 0
mut _fp_XSetWMHints           = 0
mut _fp_XFree                 = 0
mut _fp_XCreateColormap       = 0
mut _fp_XFreeColormap         = 0
mut _fp_XMatchVisualInfo      = 0
mut _fp_XVisualIDFromVisual   = 0
mut _fp_XSetInputFocus        = 0
mut _fp_XGetInputFocus        = 0
mut _fp_XSelectInput          = 0
mut _fp_XTranslateCoordinates = 0
mut _fp_XQueryPointer         = 0
mut _fp_XWarpPointer          = 0
mut _fp_XGrabPointer          = 0
mut _fp_XUngrabPointer        = 0
mut _fp_XDefineCursor         = 0
mut _fp_XFreeCursor           = 0
mut _fp_XCreateFontCursor     = 0
mut _fp_XDisplayKeycodes      = 0
mut _fp_XGetKeyboardMapping   = 0
mut _fp_XkbSetDetectableAutoRepeat = 0
mut _fp_XSetLocaleModifiers   = 0
mut _fp_XOpenIM               = 0
mut _fp_XCloseIM              = 0
mut _fp_XCreateIC             = 0
mut _fp_XDestroyIC            = 0
mut _fp_Xutf8LookupString     = 0
mut _fp_XFilterEvent_ic       = 0
mut _fp_XGetEventData         = 0
mut _fp_XFreeEventData        = 0
mut _fp_XQueryExtension       = 0
mut _fp_XInitThreads          = 0
mut _fp_XrmInitialize         = 0
mut _fp_Xutf8SetWMProperties  = 0

;; XRandR function pointers
mut _fp_XRRGetScreenResourcesCurrent = 0
mut _fp_XRRFreeScreenResources       = 0
mut _fp_XRRGetCrtcInfo               = 0
mut _fp_XRRFreeCrtcInfo              = 0
mut _fp_XRRGetOutputInfo             = 0
mut _fp_XRRFreeOutputInfo            = 0
mut _fp_XRRQueryExtension            = 0
mut _fp_XRRQueryVersion              = 0
mut _fp_XRRSelectInput               = 0
mut _fp_XRRGetOutputPrimary          = 0
mut _fp_XRRSetCrtcConfig             = 0
mut _fp_XRRUpdateConfiguration      = 0
mut _fp_XRRAllocGamma                = 0
mut _fp_XRRFreeGamma                 = 0
mut _fp_XRRGetCrtcGamma              = 0
mut _fp_XRRGetCrtcGammaSize          = 0
mut _fp_XRRSetCrtcGamma              = 0

;; XInput2
mut _fp_XIQueryVersion  = 0
mut _fp_XISelectEvents  = 0

;; XCursor
mut _fp_XcursorImageCreate      = 0
mut _fp_XcursorImageDestroy     = 0
mut _fp_XcursorImageLoadCursor  = 0
mut _fp_XcursorGetTheme         = 0
mut _fp_XcursorGetDefaultSize   = 0
mut _fp_XcursorLibraryLoadCursor= 0

;; Vulkan loader
mut _fp_vkGetInstanceProcAddr   = 0

;; ═══════════════════════════════════════════════════════════════════════════
;; §4  GLOBAL STATE
;; ═══════════════════════════════════════════════════════════════════════════

mut _initialized   = false
mut _x11_dpy       = 0 ;; Display*
mut _x11_scr       = 0 ;; int screen
mut _x11_root      = 0 ;; Window root
mut _x11_im        = 0 ;; XIM

;; Atoms (GLFW-style — intern once on init)
mut _atom_WM_DELETE   = 0
mut _atom_WM_PROTOS   = 0
mut _atom_NET_STATE   = 0
mut _atom_NET_ABOVE   = 0
mut _atom_NET_FS      = 0
mut _atom_NET_MAX_V   = 0
mut _atom_NET_MAX_H   = 0
mut _atom_NET_ACTIVE  = 0
mut _atom_NET_CHECK   = 0
mut _atom_NET_BYPASS  = 0
mut _atom_NET_NAME    = 0
mut _atom_NET_ICON    = 0
mut _atom_MOTIF_HINTS = 0
mut _atom_UTF8        = 0
mut _atom_CLIPBOARD   = 0
mut _atom_TARGETS     = 0
mut _atom_NET_WORKAREA = 0
mut _atom_NET_DESKTOP  = 0

;; XRandR state
mut _randr_avail  = false
mut _randr_evbase = 0
mut _randr_errbase= 0

;; XInput2 state
mut _xi2_avail    = false
mut _xi2_evbase   = 0
mut _xi2_major    = 2
mut _xi2_minor    = 0

;; Window registry: xwin → state dict
mut _windows = 0

;; ═══════════════════════════════════════════════════════════════════════════
;; §5  LIBRARY LOADING (GLFW _glfwPlatformLoadModule style)
;; ═══════════════════════════════════════════════════════════════════════════

fn _sym(lib, name){ dlsym(lib, name) }

fn _load_x11(){
   if(_lib_x11){ return true }
   _lib_x11 = dlopen("libX11.so.6", 1)
   if(!_lib_x11){ _lib_x11 = dlopen("libX11.so", 1) }
   if(!_lib_x11){ return false }

   _fp_XOpenDisplay          = _sym(_lib_x11, "XOpenDisplay")
   _fp_XCloseDisplay         = _sym(_lib_x11, "XCloseDisplay")
   _fp_XDefaultRootWindow    = _sym(_lib_x11, "XDefaultRootWindow")
   _fp_XDefaultScreen        = _sym(_lib_x11, "XDefaultScreen")
   _fp_XDisplayWidth         = _sym(_lib_x11, "XDisplayWidth")
   _fp_XDisplayHeight        = _sym(_lib_x11, "XDisplayHeight")
   _fp_XCreateWindow         = _sym(_lib_x11, "XCreateWindow")
   _fp_XDestroyWindow        = _sym(_lib_x11, "XDestroyWindow")
   _fp_XMapWindow            = _sym(_lib_x11, "XMapWindow")
   _fp_XUnmapWindow          = _sym(_lib_x11, "XUnmapWindow")
   _fp_XFlush                = _sym(_lib_x11, "XFlush")
   _fp_XSync                 = _sym(_lib_x11, "XSync")
   _fp_XPending              = _sym(_lib_x11, "XPending")
   _fp_XNextEvent            = _sym(_lib_x11, "XNextEvent")
   _fp_XCheckMaskEvent       = _sym(_lib_x11, "XCheckMaskEvent")
   _fp_XCheckTypedWindowEvent= _sym(_lib_x11, "XCheckTypedWindowEvent")
   _fp_XCheckIfEvent         = _sym(_lib_x11, "XCheckIfEvent")
   _fp_XFilterEvent          = _sym(_lib_x11, "XFilterEvent")
   _fp_XInternAtom           = _sym(_lib_x11, "XInternAtom")
   _fp_XSetWMProtocols       = _sym(_lib_x11, "XSetWMProtocols")
   _fp_XGetWindowAttributes  = _sym(_lib_x11, "XGetWindowAttributes")
   _fp_XGetWindowProperty    = _sym(_lib_x11, "XGetWindowProperty")
   _fp_XChangeProperty       = _sym(_lib_x11, "XChangeProperty")
   _fp_XDeleteProperty       = _sym(_lib_x11, "XDeleteProperty")
   _fp_XSendEvent            = _sym(_lib_x11, "XSendEvent")
   _fp_XMoveWindow           = _sym(_lib_x11, "XMoveWindow")
   _fp_XResizeWindow         = _sym(_lib_x11, "XResizeWindow")
   _fp_XMoveResizeWindow     = _sym(_lib_x11, "XMoveResizeWindow")
   _fp_XRaiseWindow          = _sym(_lib_x11, "XRaiseWindow")
   _fp_XIconifyWindow        = _sym(_lib_x11, "XIconifyWindow")
   _fp_XStoreName            = _sym(_lib_x11, "XStoreName")
   _fp_XAllocSizeHints       = _sym(_lib_x11, "XAllocSizeHints")
   _fp_XAllocWMHints         = _sym(_lib_x11, "XAllocWMHints")
   _fp_XSetWMNormalHints     = _sym(_lib_x11, "XSetWMNormalHints")
   _fp_XSetWMHints           = _sym(_lib_x11, "XSetWMHints")
   _fp_XFree                 = _sym(_lib_x11, "XFree")
   _fp_XCreateColormap       = _sym(_lib_x11, "XCreateColormap")
   _fp_XFreeColormap         = _sym(_lib_x11, "XFreeColormap")
   _fp_XMatchVisualInfo      = _sym(_lib_x11, "XMatchVisualInfo")
   _fp_XVisualIDFromVisual   = _sym(_lib_x11, "XVisualIDFromVisual")
   _fp_XSetInputFocus        = _sym(_lib_x11, "XSetInputFocus")
   _fp_XGetInputFocus        = _sym(_lib_x11, "XGetInputFocus")
   _fp_XSelectInput          = _sym(_lib_x11, "XSelectInput")
   _fp_XTranslateCoordinates = _sym(_lib_x11, "XTranslateCoordinates")
   _fp_XQueryPointer         = _sym(_lib_x11, "XQueryPointer")
   _fp_XWarpPointer          = _sym(_lib_x11, "XWarpPointer")
   _fp_XGrabPointer          = _sym(_lib_x11, "XGrabPointer")
   _fp_XUngrabPointer        = _sym(_lib_x11, "XUngrabPointer")
   _fp_XDefineCursor         = _sym(_lib_x11, "XDefineCursor")
   _fp_XFreeCursor           = _sym(_lib_x11, "XFreeCursor")
   _fp_XCreateFontCursor     = _sym(_lib_x11, "XCreateFontCursor")
   _fp_XDisplayKeycodes      = _sym(_lib_x11, "XDisplayKeycodes")
   _fp_XGetKeyboardMapping   = _sym(_lib_x11, "XGetKeyboardMapping")
   _fp_XkbSetDetectableAutoRepeat = _sym(_lib_x11, "XkbSetDetectableAutoRepeat")
   _fp_XSetLocaleModifiers   = _sym(_lib_x11, "XSetLocaleModifiers")
   _fp_XOpenIM               = _sym(_lib_x11, "XOpenIM")
   _fp_XCloseIM              = _sym(_lib_x11, "XCloseIM")
   _fp_XCreateIC             = _sym(_lib_x11, "XCreateIC")
   _fp_XDestroyIC            = _sym(_lib_x11, "XDestroyIC")
   _fp_Xutf8LookupString     = _sym(_lib_x11, "Xutf8LookupString")
   _fp_XGetEventData         = _sym(_lib_x11, "XGetEventData")
   _fp_XFreeEventData        = _sym(_lib_x11, "XFreeEventData")
   _fp_XQueryExtension       = _sym(_lib_x11, "XQueryExtension")
   _fp_XInitThreads          = _sym(_lib_x11, "XInitThreads")
   _fp_XrmInitialize         = _sym(_lib_x11, "XrmInitialize")
   _fp_Xutf8SetWMProperties  = _sym(_lib_x11, "Xutf8SetWMProperties")
   true
}

fn _load_randr(){
   if(_lib_xrandr){ return true }
   _lib_xrandr = dlopen("libXrandr.so.2", 1) || dlopen("libXrandr.so", 1)
   if(!_lib_xrandr){ return false }
   _fp_XRRGetScreenResourcesCurrent = _sym(_lib_xrandr, "XRRGetScreenResourcesCurrent")
   _fp_XRRFreeScreenResources       = _sym(_lib_xrandr, "XRRFreeScreenResources")
   _fp_XRRGetCrtcInfo               = _sym(_lib_xrandr, "XRRGetCrtcInfo")
   _fp_XRRFreeCrtcInfo              = _sym(_lib_xrandr, "XRRFreeCrtcInfo")
   _fp_XRRGetOutputInfo             = _sym(_lib_xrandr, "XRRGetOutputInfo")
   _fp_XRRFreeOutputInfo            = _sym(_lib_xrandr, "XRRFreeOutputInfo")
   _fp_XRRQueryExtension            = _sym(_lib_xrandr, "XRRQueryExtension")
   _fp_XRRQueryVersion              = _sym(_lib_xrandr, "XRRQueryVersion")
   _fp_XRRSelectInput               = _sym(_lib_xrandr, "XRRSelectInput")
   _fp_XRRGetOutputPrimary          = _sym(_lib_xrandr, "XRRGetOutputPrimary")
   _fp_XRRSetCrtcConfig             = _sym(_lib_xrandr, "XRRSetCrtcConfig")
   _fp_XRRUpdateConfiguration      = _sym(_lib_xrandr, "XRRUpdateConfiguration")
   _fp_XRRAllocGamma                = _sym(_lib_xrandr, "XRRAllocGamma")
   _fp_XRRFreeGamma                 = _sym(_lib_xrandr, "XRRFreeGamma")
   _fp_XRRGetCrtcGamma              = _sym(_lib_xrandr, "XRRGetCrtcGamma")
   _fp_XRRGetCrtcGammaSize          = _sym(_lib_xrandr, "XRRGetCrtcGammaSize")
   _fp_XRRSetCrtcGamma              = _sym(_lib_xrandr, "XRRSetCrtcGamma")
   true
}

fn _load_xi2(){
   if(_lib_xi){ return true }
   _lib_xi = dlopen("libXi.so.6", 1) || dlopen("libXi.so", 1)
   if(!_lib_xi){ return false }
   _fp_XIQueryVersion = _sym(_lib_xi, "XIQueryVersion")
   _fp_XISelectEvents = _sym(_lib_xi, "XISelectEvents")
   true
}

fn _load_xcursor(){
   if(_lib_xcursor){ return true }
   _lib_xcursor = dlopen("libXcursor.so.1", 1) || dlopen("libXcursor.so", 1)
   if(!_lib_xcursor){ return false }
   _fp_XcursorImageCreate      = _sym(_lib_xcursor, "XcursorImageCreate")
   _fp_XcursorImageDestroy     = _sym(_lib_xcursor, "XcursorImageDestroy")
   _fp_XcursorImageLoadCursor  = _sym(_lib_xcursor, "XcursorImageLoadCursor")
   _fp_XcursorGetTheme         = _sym(_lib_xcursor, "XcursorGetTheme")
   _fp_XcursorGetDefaultSize   = _sym(_lib_xcursor, "XcursorGetDefaultSize")
   _fp_XcursorLibraryLoadCursor= _sym(_lib_xcursor, "XcursorLibraryLoadCursor")
   true
}

fn _load_vk(){
   if(_lib_vk){ return true }
   _lib_vk = dlopen("libvulkan.so.1", 1) || dlopen("libvulkan.so", 1)
   if(!_lib_vk){ return false }
   _fp_vkGetInstanceProcAddr = _sym(_lib_vk, "vkGetInstanceProcAddr")
   true
}

;; ═══════════════════════════════════════════════════════════════════════════
;; §6  HELPERS — raw callN wrappers for clarity
;; ═══════════════════════════════════════════════════════════════════════════

fn _x11_intern(name, only_if_exists){
   call3(_fp_XInternAtom, _x11_dpy, cstr(name), only_if_exists ? 1 : 0)
}

fn _x11_flush(){ call1(_fp_XFlush, _x11_dpy) }
fn _x11_sync(){ call2(_fp_XSync, _x11_dpy, 0) }

fn _x11_change_prop32(win, prop, typ, mode, val){
   ;; XChangeProperty with 32-bit format (single value via stack)
   def p = sys_malloc(8) store32(p, val, 0)
   call8(_fp_XChangeProperty, _x11_dpy, win, prop, typ, 32, mode, p, 1)
   sys_free(p)
}

fn _x11_change_prop_atom(win, prop, val){
   def XA_ATOM = 4
   def p = sys_malloc(8) store64_raw(p, val, 0)
   call8(_fp_XChangeProperty, _x11_dpy, win, prop, XA_ATOM, 32, X_PropModeReplace, p, 1)
   sys_free(p)
}

fn _x11_send_event(win, atom, d0, d1, d2){
   ;; _NET_WM_STATE style send
   def ev = sys_malloc(96) memset(ev, 0, 96)
   store32(ev, X_ClientMessage, 0) ;; type
   store32(ev, 32, 28) ;; format=32
   store64_raw(ev, win, 32) ;; window
   store64_raw(ev, atom, 40) ;; message_type
   store64_raw(ev, d0, 48) ;; data.l[0]
   store64_raw(ev, d1, 56) ;; data.l[1]
   store64_raw(ev, d2, 64) ;; data.l[2]
   def send_mask = 131073 ;; SubstructureRedirectMask|SubstructureNotifyMask
   call5(_fp_XSendEvent, _x11_dpy, _x11_root, 0, send_mask, ev)
   sys_free(ev)
}

;; Read a window property, returns raw pointer (caller must XFree)
fn _x11_get_prop(win, prop, typ, out_count){
   def actual_type = sys_malloc(8)
   def actual_fmt  = sys_malloc(4)
   def nitems      = sys_malloc(8)
   def bytes_after = sys_malloc(8)
   def data        = sys_malloc(8) store64_raw(data, 0, 0)
   call12(_fp_XGetWindowProperty, _x11_dpy, win, prop, 0, 1024, 0,
         typ, actual_type, actual_fmt, nitems, bytes_after, data)
   def result = load64_raw(data, 0)
   if(out_count){ store64_raw(out_count, load64_raw(nitems, 0), 0) }
   sys_free(actual_type) sys_free(actual_fmt) sys_free(nitems)
   sys_free(bytes_after) sys_free(data)
   result
}

;; Safe XFree wrapper
fn _xfree(p){ if(p){ call1(_fp_XFree, p) } }

;; Get window state dict (internal handle → state)
fn _st(win){
   if(!_windows){ return 0 }
   dict_get(_windows, win, 0)
}

;; ═══════════════════════════════════════════════════════════════════════════
;; §7  INIT (GLFW x11_init.c _glfwConnectX11 + _glfwInitX11)
;; ═══════════════════════════════════════════════════════════════════════════

fn _linux_init(){
   if(_initialized){ return true }
   if(!_load_x11()){ return false }

   ;; XInitThreads + XrmInitialize (GLFW does this)
   if(_fp_XInitThreads){ call0(_fp_XInitThreads) }
   if(_fp_XrmInitialize){ call0(_fp_XrmInitialize) }

   _x11_dpy = call1(_fp_XOpenDisplay, 0)
   if(!_x11_dpy){ return false }

   _x11_scr  = call1(_fp_XDefaultScreen, _x11_dpy)
   _x11_root = call1(_fp_XDefaultRootWindow, _x11_dpy)

   ;; Intern all needed atoms upfront (GLFW style)
   _atom_WM_DELETE   = _x11_intern("WM_DELETE_WINDOW", false)
   _atom_WM_PROTOS   = _x11_intern("WM_PROTOCOLS", false)
   _atom_NET_STATE   = _x11_intern("_NET_WM_STATE", false)
   _atom_NET_ABOVE   = _x11_intern("_NET_WM_STATE_ABOVE", false)
   _atom_NET_FS      = _x11_intern("_NET_WM_STATE_FULLSCREEN", false)
   _atom_NET_MAX_V   = _x11_intern("_NET_WM_STATE_MAXIMIZED_VERT", false)
   _atom_NET_MAX_H   = _x11_intern("_NET_WM_STATE_MAXIMIZED_HORZ", false)
   _atom_NET_ACTIVE  = _x11_intern("_NET_ACTIVE_WINDOW", false)
   _atom_NET_CHECK   = _x11_intern("_NET_SUPPORTING_WM_CHECK", false)
   _atom_NET_BYPASS  = _x11_intern("_NET_WM_BYPASS_COMPOSITOR", false)
   _atom_NET_NAME    = _x11_intern("_NET_WM_NAME", false)
   _atom_NET_ICON    = _x11_intern("_NET_WM_ICON", false)
   _atom_MOTIF_HINTS = _x11_intern("_MOTIF_WM_HINTS", false)
   _atom_UTF8        = _x11_intern("UTF8_STRING", false)
   _atom_CLIPBOARD   = _x11_intern("CLIPBOARD", false)
   _atom_TARGETS     = _x11_intern("TARGETS", false)
   _atom_NET_WORKAREA= _x11_intern("_NET_WORKAREA", false)
   _atom_NET_DESKTOP = _x11_intern("_NET_CURRENT_DESKTOP", false)

   ;; Enable detectable auto-repeat (GLFW x11_init.c line 818)
   if(_fp_XkbSetDetectableAutoRepeat){
      call3(_fp_XkbSetDetectableAutoRepeat, _x11_dpy, 1, 0)
   }

   ;; Load optional extension libs
   _load_randr()
   _load_xi2()
   _load_xcursor()
   _load_vk()

   ;; Init XRandR (GLFW x11_init.c ~line 663)
   if(_lib_xrandr && _fp_XRRQueryExtension){
      def evb = sys_malloc(4) def erb = sys_malloc(4)
      def ok = call3(_fp_XRRQueryExtension, _x11_dpy, evb, erb)
      if(ok){
         _randr_avail  = true
         _randr_evbase = load32(evb, 0)
         _randr_errbase= load32(erb, 0)
         ;; Subscribe to RandR events on root
         call3(_fp_XRRSelectInput, _x11_dpy, _x11_root, 3) ;; RRScreenChangeNotifyMask|RRCrtcChangeNotifyMask
      }
      sys_free(evb) sys_free(erb)
   }

   ;; Init XInput2 (GLFW x11_init.c ~line 631)
   if(_lib_xi && _fp_XQueryExtension){
      def evb = sys_malloc(4) def erb = sys_malloc(4) def opcode = sys_malloc(4)
      def ok = call5(_fp_XQueryExtension, _x11_dpy, cstr("XInputExtension"), opcode, evb, erb)
      if(ok && _fp_XIQueryVersion){
         def major = sys_malloc(4) def minor = sys_malloc(4)
         store32(major, 2, 0) store32(minor, 0, 0)
         def res = call3(_fp_XIQueryVersion, _x11_dpy, major, minor)
         if(res == 0){
         _xi2_avail   = true
         _xi2_evbase  = load32(evb, 0)
         }
         sys_free(major) sys_free(minor)
      }
      sys_free(evb) sys_free(erb) sys_free(opcode)
   }

   _windows     = dict(32)
   _initialized = true
   true
}

;; ═══════════════════════════════════════════════════════════════════════════
;; §8  WINDOW CREATION (GLFW x11_window.c createNativeWindow)
;; ═══════════════════════════════════════════════════════════════════════════

fn _x11_choose_visual(transp){
   ;; Returns (visual*, depth) — ported from GLFW _glfwChooseVisualX11
   if(transp && _fp_XMatchVisualInfo){
      ;; Try 32-bit ARGB visual for transparency
      def vi = sys_malloc(152) memset(vi, 0, 152)
      def ok = call5(_fp_XMatchVisualInfo, _x11_dpy, _x11_scr, 32, X_TrueColor, vi)
      if(ok){
         def vis   = load64_raw(vi, 0) ;; XVisualInfo.visual  offset 0
         def depth = load32(vi, 20) ;; XVisualInfo.depth   offset 20
         sys_free(vi)
         return [vis, depth]
      }
      sys_free(vi)
   }
   ;; Default visual
   def vi = sys_malloc(152) memset(vi, 0, 152)
   def ok = call5(_fp_XMatchVisualInfo, _x11_dpy, _x11_scr, 24, X_TrueColor, vi)
   if(ok){
      def vis   = load64_raw(vi, 0)
      def depth = load32(vi, 20)
      sys_free(vi)
      return [vis, depth]
   }
   sys_free(vi)
   ;; Absolute fallback: use DefaultVisual equivalent
   [0, 24]
}

fn _x11_set_wm_protocols(xwin){
   ;; XSetWMProtocols with WM_DELETE_WINDOW
   def p = sys_malloc(8) store64_raw(p, _atom_WM_DELETE, 0)
   call4(_fp_XSetWMProtocols, _x11_dpy, xwin, p, 1)
   sys_free(p)
}

fn _x11_set_title(xwin, title){
   ;; Set both ICCCM XStoreName and _NET_WM_NAME (UTF-8)
   call3(_fp_XStoreName, _x11_dpy, xwin, cstr(title))
   if(_atom_NET_NAME && _atom_UTF8){
      def cs = cstr(title)
      call8(_fp_XChangeProperty, _x11_dpy, xwin,
         _atom_NET_NAME, _atom_UTF8, 8, X_PropModeReplace, cs, len(title))
   }
}

fn _x11_set_size_hints(xwin, w, h, resizable){
   ;; XSetWMNormalHints — GLFW x11_window.c updateNormalHints
   if(!_fp_XAllocSizeHints){ return }
   def hints = call0(_fp_XAllocSizeHints)
   if(!hints){ return }
   ;; PMinSize|PMaxSize flags at offset 4 in XSizeHints
   if(!resizable){
      store32(hints, 0x30, 4) ;; PMinSize|PMaxSize = 0x10|0x20
      store32(hints, w, 24) ;; min_width
      store32(hints, h, 28) ;; min_height
      store32(hints, w, 32) ;; max_width
      store32(hints, h, 36) ;; max_height
   } else {
      store32(hints, 0, 4)
   }
   call3(_fp_XSetWMNormalHints, _x11_dpy, xwin, hints)
   call1(_fp_XFree, hints)
}

fn _x11_set_borderless(xwin, borderless){
   ;; Motif WM hints to remove decorations — GLFW x11_window.c
   if(!_atom_MOTIF_HINTS){ return }
   def hints = sys_malloc(40) memset(hints, 0, 40)
   if(borderless){
      store32(hints, 2, 0) ;; flags: MWM_HINTS_DECORATIONS
      store32(hints, 0, 8) ;; decorations: 0 = none
   } else {
      store32(hints, 2, 0)
      store32(hints, 1, 8) ;; decorations: 1 = all
   }
   call8(_fp_XChangeProperty, _x11_dpy, xwin,
         _atom_MOTIF_HINTS, _atom_MOTIF_HINTS,
         32, X_PropModeReplace, hints, 5)
   sys_free(hints)
}

fn create_window(title, w, h, flags=0){
   if(comptime{__os_name()!="linux"}){ return 0 }
   if(!_linux_init()){ return 0 }

   def transp    = band(flags, WINDOW_TRANSPARENT) != 0
   def borderless= band(flags, WINDOW_BORDERLESS)  != 0
   def resizable = band(flags, WINDOW_RESIZABLE)   != 0 || (!borderless)

   def vd    = _x11_choose_visual(transp)
   def vis   = get(vd, 0)
   def depth = get(vd, 1)

   ;; Create colormap
   mut cmap = 0
   if(vis){ cmap = call4(_fp_XCreateColormap, _x11_dpy, _x11_root, vis, X_AllocNone) }

   ;; Build XSetWindowAttributes (112 bytes, zeroed)
   def evmask = XM_KeyPress|XM_KeyRelease|XM_ButtonPress|XM_ButtonRelease|
         XM_PointerMotion|XM_StructureNotify|XM_FocusChange|
         XM_LeaveWindow|XM_EnterWindow|XM_Exposure|XM_PropertyChange
   def at = sys_malloc(112) memset(at, 0, 112)
   store64_raw(at, 0, 8) ;; background_pixmap
   store64_raw(at, 0, 16) ;; background_pixel
   store64_raw(at, 0, 24) ;; border_pixmap
   store64_raw(at, 0, 32) ;; border_pixel
   store64_raw(at, evmask, 72) ;; event_mask
   if(cmap){ store64_raw(at, cmap, 96) } ;; colormap

   def cwmask = X_CWBorderPixel|X_CWEventMask|(cmap ? X_CWColormap : 0)
   def xwin = call12(_fp_XCreateWindow,
      _x11_dpy, _x11_root,
      0, 0, w, h, 0,
      depth, X_InputOutput,
      vis ? vis : 0,
      cwmask, at)
   sys_free(at)
   if(!xwin){ if(cmap){ call2(_fp_XFreeColormap, _x11_dpy, cmap) } return 0 }

   ;; WM protocols, title, hints
   _x11_set_wm_protocols(xwin)
   _x11_set_title(xwin, title)
   _x11_set_size_hints(xwin, w, h, resizable)
   if(borderless){ _x11_set_borderless(xwin, true) }

   ;; _NET_WM_BYPASS_COMPOSITOR = 0 (let compositor decide)
   if(_atom_NET_BYPASS){ _x11_change_prop32(xwin, _atom_NET_BYPASS, 6, X_PropModeReplace, 0) }

   call2(_fp_XMapWindow, _x11_dpy, xwin)
   _x11_flush()

   ;; Register window
   mut st = dict(16)
   st = dict_set(st, "xwin",    xwin)
   st = dict_set(st, "w",       w)
   st = dict_set(st, "h",       h)
   st = dict_set(st, "vis",     vis)
   st = dict_set(st, "depth",   depth)
   st = dict_set(st, "cmap",    cmap)
   st = dict_set(st, "closed",  false)
   st = dict_set(st, "focused", false)
   st = dict_set(st, "events",  [])
   st = dict_set(st, "keys",    dict(64))
   st = dict_set(st, "buttons", dict(8))
   st = dict_set(st, "mx",      0)
   st = dict_set(st, "my",      0)
   st = dict_set(st, "mods",    0)
   _windows = dict_set(_windows, xwin, st)
   xwin
}

;; ═══════════════════════════════════════════════════════════════════════════
;; §9  EVENT LOOP (GLFW x11_window.c processEvent)
;; ═══════════════════════════════════════════════════════════════════════════

;; XEvent offsets (64-bit Linux, Xlib layout)
;; type        = load32(ev, 0)
;; serial      = load64(ev, 8)
;; send_event  = load32(ev, 16)
;; display     = load64_raw(ev, 24)
;; window/xid  = load64_raw(ev, 32)
;;
;; KeyEvent:    keycode=load32(ev,64)  state=load32(ev,60)
;; ButtonEvent: button=load32(ev,64)   state=load32(ev,60)  x=load32(ev,40) y=load32(ev,44)
;; MotionEvent: x=load32(ev,40)        y=load32(ev,44)
;; ConfigureNotify: x=load32(ev,40) y=load32(ev,44) w=load32(ev,48) h=load32(ev,52)
;; ClientMessage: message_type=load64_raw(ev,40)  data.l[0]=load64_raw(ev,56)
;; FocusEvent:  (just type + window at 32)

;; ── Event type constants (must match consts.ny EVENT_* values) ──────────────
def _EV_NONE        = 0
def _EV_KEY_DOWN    = 1
def _EV_KEY_UP      = 2
def _EV_KEY_CHAR    = 3
def _EV_MB_DOWN     = 4
def _EV_MB_UP       = 5
def _EV_SCROLL      = 6
def _EV_MOUSE_MOVE  = 7
def _EV_WIN_MOVE    = 8
def _EV_WIN_RESIZE  = 9
def _EV_FOCUS_IN    = 10
def _EV_FOCUS_OUT   = 11
def _EV_MOUSE_ENTER = 12
def _EV_MOUSE_LEAVE = 13
def _EV_REFRESH     = 14
def _EV_QUIT        = 15

;; ── X11 state mask → GLFW mod bits ──────────────────────────────────────────
;; X11 state: ShiftMask=1 LockMask=2 ControlMask=4 Mod1Mask=8(Alt) Mod4Mask=64(Super)
fn _x11_state_to_mods(state){
   mut m = 0
   if(band(state, 1)  != 0){ m = m + 0x0001 } ;; MOD_SHIFT
   if(band(state, 4)  != 0){ m = m + 0x0002 } ;; MOD_CONTROL
   if(band(state, 8)  != 0){ m = m + 0x0004 } ;; MOD_ALT
   if(band(state, 64) != 0){ m = m + 0x0008 } ;; MOD_SUPER
   if(band(state, 2)  != 0){ m = m + 0x0010 } ;; MOD_CAPS_LOCK
   if(band(state, 16) != 0){ m = m + 0x0020 } ;; MOD_NUM_LOCK
   m
}

;; ── XKB keysym → GLFW key code ───────────────────────────────────────────────
;; We call XkbKeycodeToKeysym(dpy, keycode, 0, 0) to get the base keysym,
;; then map to GLFW KEY_* values.
mut _fp_XkbKeycodeToKeysym = 0
mut _fp_XLookupString      = 0

fn _ensure_xkb(){
   if(!_fp_XkbKeycodeToKeysym && _lib_x11){
      _fp_XkbKeycodeToKeysym = dlsym(_lib_x11, "XkbKeycodeToKeysym")
      _fp_XLookupString      = dlsym(_lib_x11, "XLookupString")
   }
}

fn _keysym_to_glfw(ks){
   ;; Printable ASCII: space(32) through tilde(126) — return as-is (uppercase)
   if(ks >= 97 && ks <= 122){ return ks - 32 } ;; a-z → A-Z
   if(ks >= 32 && ks <= 96 ){ return ks } ;; space, digits, uppercase, symbols
   ;; X11 special keysyms (0xFF00–0xFFFF)
   if(ks == 0xFF0D){ return 257 } ;; XK_Return      → KEY_ENTER
   if(ks == 0xFF08){ return 259 } ;; XK_BackSpace    → KEY_BACKSPACE
   if(ks == 0xFF09){ return 258 } ;; XK_Tab          → KEY_TAB
   if(ks == 0xFF1B){ return 256 } ;; XK_Escape       → KEY_ESCAPE
   if(ks == 0xFF63){ return 260 } ;; XK_Insert       → KEY_INSERT
   if(ks == 0xFFFF){ return 261 } ;; XK_Delete       → KEY_DELETE
   if(ks == 0xFF51){ return 263 } ;; XK_Left
   if(ks == 0xFF52){ return 265 } ;; XK_Up
   if(ks == 0xFF53){ return 262 } ;; XK_Right
   if(ks == 0xFF54){ return 264 } ;; XK_Down
   if(ks == 0xFF55){ return 266 } ;; XK_Prior  (Page Up)
   if(ks == 0xFF56){ return 267 } ;; XK_Next   (Page Down)
   if(ks == 0xFF50){ return 268 } ;; XK_Home
   if(ks == 0xFF57){ return 269 } ;; XK_End
   if(ks == 0xFF7F){ return 282 } ;; XK_Num_Lock
   if(ks == 0xFF14){ return 281 } ;; XK_Scroll_Lock
   if(ks == 0xFFE5){ return 280 } ;; XK_Caps_Lock
   if(ks == 0xFF61){ return 283 } ;; XK_Print
   if(ks == 0xFF13){ return 284 } ;; XK_Pause
   ;; Function keys F1–F25
   if(ks >= 0xFFBE && ks <= 0xFFCC){ return 290 + (ks - 0xFFBE) }
   ;; Numpad
   if(ks == 0xFFAF){ return 331 } ;; XK_KP_Divide
   if(ks == 0xFFAA){ return 332 } ;; XK_KP_Multiply
   if(ks == 0xFFAD){ return 333 } ;; XK_KP_Subtract
   if(ks == 0xFFAB){ return 334 } ;; XK_KP_Add
   if(ks == 0xFF8D){ return 335 } ;; XK_KP_Enter
   if(ks == 0xFFBD){ return 336 } ;; XK_KP_Equal
   if(ks == 0xFFB0){ return 320 }  if(ks == 0xFFB1){ return 321 }
   if(ks == 0xFFB2){ return 322 }  if(ks == 0xFFB3){ return 323 }
   if(ks == 0xFFB4){ return 324 }  if(ks == 0xFFB5){ return 325 }
   if(ks == 0xFFB6){ return 326 }  if(ks == 0xFFB7){ return 327 }
   if(ks == 0xFFB8){ return 328 }  if(ks == 0xFFB9){ return 329 }
   if(ks == 0xFFAE){ return 330 } ;; XK_KP_Decimal
   ;; Modifier keys
   if(ks == 0xFFE1 || ks == 0xFFE2){ return 340 } ;; XK_Shift_L/R
   if(ks == 0xFFE3 || ks == 0xFFE4){ return 341 } ;; XK_Control_L/R
   if(ks == 0xFFE9 || ks == 0xFFEA){ return 342 } ;; XK_Alt_L/R
   if(ks == 0xFFEB || ks == 0xFFEC){ return 343 } ;; XK_Super_L/R
   if(ks == 0xFF67){ return 348 } ;; XK_Menu
   ;; Unknown
   -1
}

fn _push_event(win, evt){
   def st = _st(win)
   if(!st){ return }
   def evs = dict_get(st, "events", [])
   def st2 = dict_set(st, "events", push(evs, evt))
   _windows = dict_set(_windows, win, st2)
}

fn _set_key(win, key, down){
   def st = _st(win)
   if(!st){ return }
   def ks  = dict_get(st, "keys", dict(64))
   def st2 = dict_set(st, "keys", dict_set(ks, key, down))
   _windows = dict_set(_windows, win, st2)
}

fn _set_btn(win, btn, down){
   def st = _st(win)
   if(!st){ return }
   def bs  = dict_get(st, "buttons", dict(8))
   def st2 = dict_set(st, "buttons", dict_set(bs, btn, down))
   _windows = dict_set(_windows, win, st2)
}

fn _set_mouse(win, x, y){
   def st = _st(win)
   if(!st){ return }
   def st2 = dict_set(dict_set(st, "mx", x), "my", y)
   _windows = dict_set(_windows, win, st2)
}

fn _process_event(ev){
   def etype = load32(ev, 0)
   def xwin  = load64_raw(ev, 32)

   ;; ClientMessage — WM_DELETE_WINDOW
   if(etype == X_ClientMessage){
      def msg = load64_raw(ev, 40)
      if(msg == _atom_WM_PROTOS){
         def d0 = load64_raw(ev, 56)
         if(d0 == _atom_WM_DELETE){
         def st = _st(xwin)
         if(st){
               _windows = dict_set(_windows, xwin, dict_set(st, "closed", true))
               _push_event(xwin, [_EV_QUIT, dict()])
         }
         }
      }
      return
   }

   ;; ConfigureNotify — resize/move
   if(etype == X_ConfigureNotify){
      def nw = load32(ev, 48) def nh = load32(ev, 52)
      def st = _st(xwin)
      if(st){
         def ow = dict_get(st, "w", 0) def oh = dict_get(st, "h", 0)
         if(nw != ow || nh != oh){
         _windows = dict_set(_windows, xwin, dict_set(dict_set(st, "w", nw), "h", nh))
         _push_event(xwin, [_EV_WIN_RESIZE, dict_set(dict_set(dict(), "w", nw), "h", nh)])
         }
      }
      return
   }

   ;; FocusIn / FocusOut
   if(etype == X_FocusIn || etype == X_FocusOut){
      def focused = etype == X_FocusIn
      def st = _st(xwin)
      if(st){ _windows = dict_set(_windows, xwin, dict_set(st, "focused", focused)) }
      _push_event(xwin, [focused ? _EV_FOCUS_IN : _EV_FOCUS_OUT, dict()])
      return
   }

   ;; EnterNotify / LeaveNotify
   if(etype == X_EnterNotify){ _push_event(xwin, [_EV_MOUSE_ENTER, dict()]) return }
   if(etype == X_LeaveNotify){ _push_event(xwin, [_EV_MOUSE_LEAVE, dict()]) return }

   ;; KeyPress / KeyRelease
   if(etype == X_KeyPress || etype == X_KeyRelease){
      _ensure_xkb()
      def keycode = load32(ev, 64)
      def state   = load32(ev, 60)
      def down    = etype == X_KeyPress
      def mods    = _x11_state_to_mods(state)
      ;; Translate keycode → keysym → GLFW key
      mut ks = keycode
      if(_fp_XkbKeycodeToKeysym){ ks = call4(_fp_XkbKeycodeToKeysym, _x11_dpy, keycode, 0, 0) }
      def key = _keysym_to_glfw(ks)
      def ev_type = down ? _EV_KEY_DOWN : _EV_KEY_UP
      mut kdata = dict()
      kdata = dict_set(kdata, "key", key)
      kdata = dict_set(kdata, "scancode", keycode)
      kdata = dict_set(kdata, "mod", mods)
      _push_event(xwin, [ev_type, kdata])
      ;; Emit KEY_CHAR for printable keys on press
      if(down && key >= 32 && key <= 126){
         _push_event(xwin, [_EV_KEY_CHAR, dict_set(dict(), "char", key)])
      }
      return
   }

   ;; ButtonPress / ButtonRelease
   if(etype == X_ButtonPress || etype == X_ButtonRelease){
      def btn  = load32(ev, 64)
      def down = etype == X_ButtonPress
      def x    = load32(ev, 40) def y = load32(ev, 44)
      def state = load32(ev, 60)
      def mods  = _x11_state_to_mods(state)
      ;; Buttons 4/5/6/7 = scroll wheel
      if(btn >= 4 && btn <= 7){
         if(down){
         def dx = (btn == 6) ? -1 : (btn == 7) ? 1 : 0
         def dy = (btn == 4) ? 1  : (btn == 5) ? -1 : 0
         _push_event(xwin, [_EV_SCROLL, dict_set(dict_set(dict(), "dx", float(dx)), "dy", float(dy))])
         }
      } else {
         ;; X11 buttons: 1=left 2=middle 3=right → GLFW 0=left 1=right 2=middle
         def glfw_btn = btn == 1 ? 0 : btn == 3 ? 1 : btn == 2 ? 2 : btn - 1
         def ev_type  = down ? _EV_MB_DOWN : _EV_MB_UP
         mut bdata = dict()
         bdata = dict_set(bdata, "button", glfw_btn)
         bdata = dict_set(bdata, "x", x)
         bdata = dict_set(bdata, "y", y)
         bdata = dict_set(bdata, "mod", mods)
         _push_event(xwin, [ev_type, bdata])
      }
      return
   }

   ;; MotionNotify
   if(etype == X_MotionNotify){
      def x = load32(ev, 40) def y = load32(ev, 44)
      def st = _st(xwin)
      if(st){ _windows = dict_set(_windows, xwin, dict_set(dict_set(st, "mx", x), "my", y)) }
      _push_event(xwin, [_EV_MOUSE_MOVE, dict_set(dict_set(dict(), "x", float(x)), "y", float(y))])
      return
   }

   ;; Expose
   if(etype == X_Expose){ _push_event(xwin, [_EV_REFRESH, dict()]) }
}

fn poll_events(){
   if(comptime{__os_name()!="linux"}){ return }
   if(!_initialized || !_x11_dpy){ return }
   def ev = sys_malloc(192)
   while(true){
      def pending = call1(_fp_XPending, _x11_dpy)
      if(pending <= 0){ break }
      memset(ev, 0, 192)
      call2(_fp_XNextEvent, _x11_dpy, ev)
      if(_fp_XFilterEvent){
         def filtered = call2(_fp_XFilterEvent, ev, 0)
         if(filtered){ continue }
      }
      _process_event(ev)
   }
   sys_free(ev)
}

fn check_event(win){
   def st = _st(win)
   if(!st){ return 0 }
   def evs = dict_get(st, "events", [])
   if(len(evs) == 0){ return 0 }
   def e   = get(evs, 0)
   _windows = dict_set(_windows, win, dict_set(st, "events", slice(evs, 1, len(evs))))
   e
}

;; ═══════════════════════════════════════════════════════════════════════════
;; §10  VULKAN SURFACE (direct call4, bypassing ffi_call list path)
;; ═══════════════════════════════════════════════════════════════════════════

fn vulkan_supported(){ _load_vk() && !!_lib_vk }
fn get_required_vk_extensions(){ ["VK_KHR_surface", "VK_KHR_xlib_surface"] }

fn x11_display(){ _x11_dpy }

fn x11_visual_id(win){
   if(comptime{__os_name()!="linux"}){ return 0 }
   def st = _st(win) if(!st){ return 0 }
   def vis = dict_get(st, "vis", 0)
   if(!vis){ return 0 }
   if(_fp_XVisualIDFromVisual){ return call1(_fp_XVisualIDFromVisual, vis) }
   ;; Fallback: read VisualID directly from Visual struct at offset 8
   load64_raw(vis, 8)
}

fn create_vk_surface(win, instance){
   ;; Direct call — no ffi_call list boxing, all sys_malloc for raw C ptrs
   if(!_load_vk()){ return 0 }
   def fp = call2(_fp_vkGetInstanceProcAddr, instance, cstr("vkCreateXlibSurfaceKHR"))
   if(!fp){ return 0 }

   def st = _st(win) if(!st){ return 0 }
   def xwin_h = dict_get(st, "xwin", 0)

   ;; VkXlibSurfaceCreateInfoKHR  (40 bytes)
   ;; offset 0:  sType  (uint32)
   ;; offset 8:  pNext  (ptr)
   ;; offset 16: flags  (uint32)
   ;; offset 24: dpy    (Display*)
   ;; offset 32: window (Window/XID = ulong)
   def ci = sys_malloc(40) memset(ci, 0, 40)
   store32(ci, VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, 0)
   store64_raw(ci, _x11_dpy, 24)
   store64_raw(ci, xwin_h,   32)

   def p   = sys_malloc(8) store64_raw(p, 0, 0)
   def res = call4(fp, instance, ci, 0, p)
   def surf = load64_raw(p, 0)
   sys_free(ci) sys_free(p)
   if(res == 0){ return surf }
   0
}

;; ═══════════════════════════════════════════════════════════════════════════
;; §11  WINDOW MANAGEMENT
;; ═══════════════════════════════════════════════════════════════════════════

fn close_window(win){
   if(comptime{__os_name()!="linux"}){ return }
   def st = _st(win) if(!st){ return }
   def cmap = dict_get(st, "cmap", 0)
   call2(_fp_XDestroyWindow, _x11_dpy, win)
   if(cmap){ call2(_fp_XFreeColormap, _x11_dpy, cmap) }
   _x11_flush()
   _windows = dict_del(_windows, win)
}

fn should_close(win){
   def st = _st(win) if(!st){ return true }
   dict_get(st, "closed", false)
}

fn set_title(win, title){
   if(comptime{__os_name()!="linux"}){ return }
   _x11_set_title(win, title)
   _x11_flush()
}

fn get_pos(win){
   if(comptime{__os_name()!="linux"}){ return [0,0] }
   ;; XTranslateCoordinates to root for absolute position
   if(!_fp_XTranslateCoordinates){ return [0, 0] }
   def dx = sys_malloc(4) def dy = sys_malloc(4) def child = sys_malloc(8)
   call8(_fp_XTranslateCoordinates, _x11_dpy, win, _x11_root, 0, 0, dx, dy, child)
   def x = load32(dx, 0) def y = load32(dy, 0)
   sys_free(dx) sys_free(dy) sys_free(child)
   [x, y]
}

fn get_framebuffer_size(win){
   def st = _st(win) if(!st){ return [0, 0] }
   [dict_get(st, "w", 0), dict_get(st, "h", 0)]
}

fn move_window(win, x, y){
   if(comptime{__os_name()!="linux"}){ return }
   call4(_fp_XMoveWindow, _x11_dpy, win, x, y)
   _x11_flush()
}

fn resize_window(win, w, h){
   if(comptime{__os_name()!="linux"}){ return }
   call4(_fp_XResizeWindow, _x11_dpy, win, w, h)
   def st = _st(win)
   if(st){
      _windows = dict_set(_windows, win, dict_set(dict_set(st, "w", w), "h", h))
   }
   _x11_flush()
}

fn show_window(win){
   if(comptime{__os_name()!="linux"}){ return }
   call2(_fp_XMapWindow, _x11_dpy, win)
   _x11_flush()
}

fn hide_window(win){
   if(comptime{__os_name()!="linux"}){ return }
   call2(_fp_XUnmapWindow, _x11_dpy, win)
   _x11_flush()
}

fn minimize(win){
   if(comptime{__os_name()!="linux"}){ return }
   call3(_fp_XIconifyWindow, _x11_dpy, win, _x11_scr)
   _x11_flush()
}

fn maximize(win){
   if(comptime{__os_name()!="linux"}){ return }
   _x11_send_event(win, _atom_NET_STATE, NET_WM_STATE_ADD, _atom_NET_MAX_V, _atom_NET_MAX_H)
   _x11_flush()
}

fn restore(win){
   if(comptime{__os_name()!="linux"}){ return }
   _x11_send_event(win, _atom_NET_STATE, NET_WM_STATE_REMOVE, _atom_NET_MAX_V, _atom_NET_MAX_H)
   call2(_fp_XMapWindow, _x11_dpy, win)
   _x11_flush()
}

fn is_maximized(win){
   if(comptime{__os_name()!="linux"}){ return false }
   def n = sys_malloc(8) store64_raw(n, 0, 0)
   def atoms = _x11_get_prop(win, _atom_NET_STATE, 4, n) ;; XA_ATOM=4
   def count = load64_raw(n, 0)
   sys_free(n)
   if(!atoms){ return false }
   mut result = false
   mut i = 0
   while(i < count){
      def a = load32(atoms, i * 4)
      if(a == _atom_NET_MAX_V || a == _atom_NET_MAX_H){ result = true }
      i += 1
   }
   _xfree(atoms)
   result
}

fn is_minimized(win){
   ;; Check _NET_WM_STATE for _NET_WM_STATE_HIDDEN
   false
}

fn is_visible(win){
   def st = _st(win) if(!st){ return false }
   !dict_get(st, "closed", false)
}

fn is_focused(win){
   def st = _st(win) if(!st){ return false }
   dict_get(st, "focused", false)
}

fn is_hovered(win){ true }

fn focus_window(win){
   if(comptime{__os_name()!="linux"}){ return }
   call2(_fp_XRaiseWindow, _x11_dpy, win)
   if(_fp_XSetInputFocus){
      call4(_fp_XSetInputFocus, _x11_dpy, win, 1, 0) ;; RevertToParent=2, CurrentTime=0
   }
   _x11_flush()
}

fn set_fullscreen(win, on){
   if(comptime{__os_name()!="linux"}){ return }
   def action = on ? NET_WM_STATE_ADD : NET_WM_STATE_REMOVE
   _x11_send_event(win, _atom_NET_STATE, action, _atom_NET_FS, 0)
   _x11_flush()
}

fn set_borderless(win, on){
   if(comptime{__os_name()!="linux"}){ return }
   _x11_set_borderless(win, on)
   _x11_flush()
}

fn set_floating(win, on){
   if(comptime{__os_name()!="linux"}){ return }
   def action = on ? NET_WM_STATE_ADD : NET_WM_STATE_REMOVE
   _x11_send_event(win, _atom_NET_STATE, action, _atom_NET_ABOVE, 0)
   _x11_flush()
}

fn set_opacity(win, o){
   if(comptime{__os_name()!="linux"}){ return }
   def atom = _x11_intern("_NET_WM_WINDOW_OPACITY", false)
   def val  = int(o * 4294967295.0)
   _x11_change_prop32(win, atom, 6, X_PropModeReplace, val) ;; XA_CARDINAL=6
   _x11_flush()
}

fn set_resizable(win, on){
   if(comptime{__os_name()!="linux"}){ return }
   def st = _st(win) if(!st){ return }
   def w = dict_get(st, "w", 0) def h = dict_get(st, "h", 0)
   _x11_set_size_hints(win, w, h, on)
}

fn request_attention(win){
   if(comptime{__os_name()!="linux"}){ return }
   def atom = _x11_intern("_NET_WM_STATE_DEMANDS_ATTENTION", false)
   _x11_send_event(win, _atom_NET_STATE, NET_WM_STATE_ADD, atom, 0)
   _x11_flush()
}

fn wait_events(t){ poll_events() }

fn post_empty_event(){ }

fn swap_buffers(win){ }
fn swap_interval(i){ }

;; ═══════════════════════════════════════════════════════════════════════════
;; §12  CURSOR & INPUT
;; ═══════════════════════════════════════════════════════════════════════════

;; XFont cursor shapes (from X11/cursorfont.h)
def XC_arrow          = 2
def XC_crosshair      = 34
def XC_hand2          = 60
def XC_sb_h_double_arrow = 108
def XC_sb_v_double_arrow = 116
def XC_xterm          = 152
def XC_watch          = 150

fn _cursor_shape_to_xfont(shape){
   if(shape == "arrow"    ){ return XC_arrow }
   if(shape == "crosshair"){ return XC_crosshair }
   if(shape == "hand"     ){ return XC_hand2 }
   if(shape == "hresize"  ){ return XC_sb_h_double_arrow }
   if(shape == "vresize"  ){ return XC_sb_v_double_arrow }
   if(shape == "text"     ){ return XC_xterm }
   if(shape == "wait"     ){ return XC_watch }
   XC_arrow
}

fn set_cursor_mode(win, mode){ }

fn get_cursor_pos(win){
   if(comptime{__os_name()!="linux"}){ return [0.0, 0.0] }
   if(!_fp_XQueryPointer){ return [0.0, 0.0] }
   def root_ret = sys_malloc(8)
   def child    = sys_malloc(8)
   def rx = sys_malloc(4) def ry = sys_malloc(4)
   def wx = sys_malloc(4) def wy = sys_malloc(4)
   def mask = sys_malloc(4)
   call8(_fp_XQueryPointer, _x11_dpy, win, root_ret, child, rx, ry, wx, wy)
   def x = load32(wx, 0) def y = load32(wy, 0)
   sys_free(root_ret) sys_free(child) sys_free(rx) sys_free(ry)
   sys_free(wx) sys_free(wy) sys_free(mask)
   [float(x), float(y)]
}

fn set_cursor_pos(win, x, y){
   if(comptime{__os_name()!="linux"}){ return }
   ;; XWarpPointer(dpy, None, win, 0,0,0,0, x, y) — 9 args
   call9(_fp_XWarpPointer, _x11_dpy, 0, win, 0, 0, 0, 0, int(x), int(y))
   _x11_flush()
}

fn create_cursor(pixels, w, h, hx, hy){
   ;; Use XcursorImageCreate if available
   if(!_lib_xcursor || !_fp_XcursorImageCreate){ return 0 }
   def img = call2(_fp_XcursorImageCreate, w, h)
   if(!img){ return 0 }
   ;; XcursorImage layout: width=off8 height=off12 xhot=off16 yhot=off20 pixels=off32(ptr)
   store32(img, w,  8)
   store32(img, h,  12)
   store32(img, hx, 16)
   store32(img, hy, 20)
   store64_raw(img, pixels, 32)
   def cursor = call2(_fp_XcursorImageLoadCursor, _x11_dpy, img)
   call1(_fp_XcursorImageDestroy, img)
   cursor
}

fn destroy_cursor(c){
   if(comptime{__os_name()!="linux"}){ return }
   if(c){ call2(_fp_XFreeCursor, _x11_dpy, c) }
}

fn set_cursor(win, c){
   if(comptime{__os_name()!="linux"}){ return }
   if(c){ call3(_fp_XDefineCursor, _x11_dpy, win, c) }
   _x11_flush()
}

fn set_cursor_shape(win, shape){
   if(comptime{__os_name()!="linux"}){ return }
   if(!_fp_XCreateFontCursor){ return }
   def xc = _cursor_shape_to_xfont(shape)
   def c  = call2(_fp_XCreateFontCursor, _x11_dpy, xc)
   call3(_fp_XDefineCursor, _x11_dpy, win, c)
   _x11_flush()
}

fn raw_mouse_motion_supported(){ _xi2_avail }
fn set_raw_mouse_motion(win, en){ }

fn set_mouse_passthrough(win, on){ }

fn get_key_scancode(key){ key }

;; ═══════════════════════════════════════════════════════════════════════════
;; §13  CLIPBOARD (GLFW x11_window.c setClipboardString / getClipboardString)
;; ═══════════════════════════════════════════════════════════════════════════

mut _clipboard_str = ""

fn set_clipboard(win, s){
   _clipboard_str = s
   ;; Assert ownership of CLIPBOARD selection
   if(_atom_CLIPBOARD){
      call4(_fp_XSetWMProtocols, _x11_dpy, win, _atom_CLIPBOARD, 0)
      ;; XSetSelectionOwner(dpy, CLIPBOARD, win, CurrentTime)
      def fp = dlsym(_lib_x11, "XSetSelectionOwner")
      if(fp){ call4(fp, _x11_dpy, _atom_CLIPBOARD, win, 0) }
   }
}

fn get_clipboard(win){ _clipboard_str }

;; ═══════════════════════════════════════════════════════════════════════════
;; §14  MONITORS (GLFW x11_monitor.c — XRandR based)
;; ═══════════════════════════════════════════════════════════════════════════

;; XRRScreenResources offsets (approximate, architecture-dependent)
;; ncrtc=off8  crtcs=off16(ptr array)  noutput=off24  outputs=off32(ptr array)
;; XRRCrtcInfo offsets: x=off0 y=off4 width=off8 height=off12
;; XRROutputInfo offsets: nameLen=off8 name=off16(ptr) mm_width=off24 mm_height=off28
;;                        crtc=off32 connection=off36(0=connected)

fn _get_screen_resources(){
   if(!_randr_avail || !_fp_XRRGetScreenResourcesCurrent){ return 0 }
   call2(_fp_XRRGetScreenResourcesCurrent, _x11_dpy, _x11_root)
}

fn _get_monitor_list(){
   ;; Returns list of dicts with monitor info
   def res = _get_screen_resources()
   if(!res){ return [[1920, 1080]] }
   def ncrtc = load32(res, 8)
   def crtcs_ptr = load64_raw(res, 16)
   mut monitors = []
   mut i = 0
   while(i < ncrtc){
      def crtc_id = load64_raw(crtcs_ptr, i * 8)
      def ci = call3(_fp_XRRGetCrtcInfo, _x11_dpy, res, crtc_id)
      if(ci){
         def cx = load32(ci, 0)  def cy = load32(ci, 4)
         def cw = load32(ci, 8)  def ch = load32(ci, 12)
         if(cw > 0 && ch > 0){
         mut m = dict(8)
         m = dict_set(m, "x", cx)  m = dict_set(m, "y", cy)
         m = dict_set(m, "w", cw)  m = dict_set(m, "h", ch)
         m = dict_set(m, "name", "Monitor" + to_str(i))
         monitors = push(monitors, m)
         }
         call1(_fp_XRRFreeCrtcInfo, ci)
      }
      i += 1
   }
   call1(_fp_XRRFreeScreenResources, res)
   if(len(monitors) == 0){ return [dict_set(dict_set(dict_set(dict_set(dict(),
      "x",0),"y",0),"w",1920),"h",1080)] }
   monitors
}

fn get_monitor_count(){
   if(comptime{__os_name()!="linux"}){ return 1 }
   if(!_initialized){ _linux_init() }
   if(!_randr_avail){ return 1 }
   len(_get_monitor_list())
}

fn _mon(i){
   def ml = _get_monitor_list()
   if(i < 0 || i >= len(ml)){ return dict_set(dict_set(dict_set(dict_set(dict(),
      "x",0),"y",0),"w",1920),"h",1080) }
   get(ml, i)
}

fn get_monitor_size(i){
   def m = _mon(i)
   [dict_get(m, "w", 1920), dict_get(m, "h", 1080)]
}

fn get_monitor_pos(i){
   def m = _mon(i)
   [dict_get(m, "x", 0), dict_get(m, "y", 0)]
}

fn get_monitor_name(i){
   def m = _mon(i)
   dict_get(m, "name", "Monitor")
}

fn get_monitor_workarea(i){
   def m = _mon(i)
   [dict_get(m,"x",0), dict_get(m,"y",0), dict_get(m,"w",1920), dict_get(m,"h",1080)]
}

fn get_monitor_physical_size(i){ [527, 296] }
fn get_monitor_content_scale(i){ [1.0, 1.0] }
fn get_monitor_refresh_rate(i){ 60 }
fn get_monitor_video_modes(i){ [] }

fn set_window_monitor(win, mon, x, y, w, h, refresh_rate){
   set_fullscreen(win, mon >= 0)
}

;; ═══════════════════════════════════════════════════════════════════════════
;; §15  TIMING (GLFW posix_time.c)
;; ═══════════════════════════════════════════════════════════════════════════

fn _clock_gettime_ns(){
   ;; clock_gettime(CLOCK_MONOTONIC=1, &ts) → ts.tv_sec*1e9 + ts.tv_nsec
   def libc = dlopen("libc.so.6", 1) || dlopen("libc.so", 1)
   if(!libc){ return 0 }
   def fp = dlsym(libc, "clock_gettime")
   if(!fp){ return 0 }
   def ts = sys_malloc(16)
   call2(fp, 1, ts) ;; CLOCK_MONOTONIC = 1
   def sec = load64_raw(ts, 0)
   def nsec= load64_raw(ts, 8)
   sys_free(ts)
   sec * 1000000000 + nsec
}

mut _time_origin_ns = 0

fn get_time(){
   if(comptime{__os_name()!="linux"}){ return 0.0 }
   if(_time_origin_ns == 0){ _time_origin_ns = _clock_gettime_ns() }
   def now = _clock_gettime_ns()
   float(now - _time_origin_ns) / 1000000000.0
}

fn set_time(t){
   def now = _clock_gettime_ns()
   _time_origin_ns = now - int(t * 1000000000.0)
}

fn get_timer_value(){
   if(comptime{__os_name()!="linux"}){ return 0 }
   _clock_gettime_ns()
}

fn get_timer_frequency(){ 1000000000 }

;; ═══════════════════════════════════════════════════════════════════════════
;; §16  WINDOW ATTRIBUTES & USER DATA
;; ═══════════════════════════════════════════════════════════════════════════

fn get_window_content_scale(win){ [1.0, 1.0] }

fn get_window_frame_size(win){ [0, 0, 0, 0] }

fn get_window_attrib(win, a){ 0 }
fn set_window_attrib(win, a, v){ }

fn set_window_user_pointer(win, p){
   def st = _st(win) if(!st){ return }
   _windows = dict_set(_windows, win, dict_set(st, "_user_ptr", p))
}

fn get_window_user_pointer(win){
   def st = _st(win) if(!st){ return 0 }
   dict_get(st, "_user_ptr", 0)
}

fn set_window_size_limits(win, minw, minh, maxw, maxh){
   if(comptime{__os_name()!="linux"}){ return }
   if(!_fp_XAllocSizeHints){ return }
   def hints = call0(_fp_XAllocSizeHints) if(!hints){ return }
   mut flags = 0
   if(minw > 0 && minh > 0){
      flags = flags | 0x10 ;; PMinSize
      store32(hints, minw, 24) store32(hints, minh, 28)
   }
   if(maxw > 0 && maxh > 0){
      flags = flags | 0x20 ;; PMaxSize
      store32(hints, maxw, 32) store32(hints, maxh, 36)
   }
   store32(hints, flags, 4)
   call3(_fp_XSetWMNormalHints, _x11_dpy, win, hints)
   call1(_fp_XFree, hints)
}

fn set_window_aspect_ratio(win, num, den){ }

;; ═══════════════════════════════════════════════════════════════════════════
;; §17  JOYSTICK STUBS
;; ═══════════════════════════════════════════════════════════════════════════

fn joystick_present(jid){ false }
fn joystick_is_gamepad(jid){ false }
fn get_joystick_name(jid){ "" }
fn get_joystick_guid(jid){ "" }
fn get_joystick_axes(jid, cp){ 0 }
fn get_joystick_buttons(jid, cp){ 0 }
fn get_joystick_hats(j){ 0 }
fn get_gamepad_state(jid, sp){ false }
fn get_gamepad_name(jid){ "" }
fn update_gamepad_mappings(s){ }
fn set_joystick_user_pointer(j, p){ }
fn get_joystick_user_pointer(j){ 0 }

;; ═══════════════════════════════════════════════════════════════════════════
;; §18  MISC
;; ═══════════════════════════════════════════════════════════════════════════

fn blit_buffer(win, buf, w, h){ }
fn get_backend_name(){ "X11 (GLFW-ported)" }
