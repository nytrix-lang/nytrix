;; Keywords: platform window backend linux x11 common shared keymap os ui input
;; X11 native window backend for windows, events, monitors, clipboard, and Vulkan surfaces.
;; References:
;; - std.os.ui.window.platform.linux
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.platform.linux.x11(available, get_backend_name, InputOutput, AllocNone, CWBackPixel, CWBorderPixel, CWColormap, CWEventMask, ShiftMask, LockMask, ControlMask, Mod1Mask, Mod2Mask, Mod4Mask, KeyPressMask, KeyReleaseMask, ButtonPressMask, ButtonReleaseMask, EnterWindowMask, LeaveWindowMask, PointerMotionMask, ExposureMask, FocusChangeMask, StructureNotifyMask, PropertyChangeMask, PropertyNewValue, WithdrawnState, NormalState, IconicState, IsViewable, XA_ATOM, XA_CARDINAL, PropModeReplace, PropModeAppend, MWM_HINTS_DECORATIONS, MWM_DECOR_ALL, PMinSize, PMaxSize, PAspect, SubstructureNotifyMask, SubstructureRedirectMask, NET_WM_STATE_REMOVE, NET_WM_STATE_ADD, NET_WM_STATE_TOGGLE, RRScreenChangeNotifyMask, RRCrtcChangeNotifyMask, RROutputChangeNotifyMask, Button1, Button2, Button3, Button4, Button5, Button6, Button7, NotifyGrab, NotifyUngrab, ClientMessage, ReparentNotify, ConfigureNotify, PropertyNotify, DestroyNotify, Expose, FocusIn, FocusOut, KeyPress, KeyRelease, ButtonPress, ButtonRelease, MotionNotify, VisibilityNotify, SelectionClear, SelectionRequest, SelectionNotify, EnterNotify, LeaveNotify, translate_keysym, translate_scancode, get_window_property, get_window_state, is_window_iconified, get_cardinal_value, get_window_frame_size, get_window_size, property_has_atom, is_window_visible, is_window_maximized, is_window_floating, is_window_fullscreen, append_atom_property, remove_atom_property, send_client_message, send_wm_state_event, iconify_window, show_window, hide_window, update_normal_hints, set_window_size, set_size, set_window_size_limits, set_window_aspect_ratio, maximize_window, restore_window, set_window_floating, set_window_fullscreen, set_window_decorated, request_window_attention, set_clipboard, get_clipboard, set_primary_selection, get_primary_selection, get_monitors, get_primary_monitor, get_monitor_pos, get_monitor_workarea, get_monitor_physical_size, get_monitor_content_scale, get_monitor_name, get_x11_monitor, get_x11_adapter, get_video_mode, get_video_modes, get_window_monitor, set_window_monitor, get_key_state, get_mouse_button_state, get_cursor_pos, get_key_name, get_key_scancode, get_size, set_size, get_pos, set_pos, set_input_mode, get_input_mode, create_cursor, create_standard_cursor, destroy_cursor, set_cursor, translate_event, poll_window_events, poll_display_events, set_window_opacity, get_window_opacity, get_window_content_scale, set_window_resizable, post_empty_event, set_window_icon, focus_window, set_cursor_pos, ARROW_CURSOR, IBEAM_CURSOR, CROSSHAIR_CURSOR, POINTING_HAND_CURSOR, RESIZE_EW_CURSOR, RESIZE_NS_CURSOR, RESIZE_NWSE_CURSOR, RESIZE_NESW_CURSOR, RESIZE_ALL_CURSOR, NOT_ALLOWED_CURSOR, INPUT_MODE_CURSOR, INPUT_MODE_STICKY_KEYS, INPUT_MODE_STICKY_MOUSE_BUTTONS, INPUT_MODE_LOCK_KEY_MODS, INPUT_MODE_RAW_MOUSE, CURSOR_MODE_NORMAL, CURSOR_MODE_HIDDEN, CURSOR_MODE_DISABLED, CURSOR_MODE_CAPTURED, wait_events, wait_for_visibility_notify, translate_state, create_basic_window, destroy_basic_window, set_title, set_window_icon, get_window_attrib, open_display, close_display, default_screen, root_window, default_visual, default_depth, intern_atom, create_colormap, create_window_raw, destroy_window_raw, map_window, unmap_window, next_event, pending, select_input, flush, sync, put_pixels, store_name, move_window, resize_window, set_wm_protocols, create_surface, get_gamma_ramp, set_gamma_ramp, vulkan_supported, vulkan_required_extensions, vulkan_get_surface_capabilities, xdnd_begin_drag, _handle_xdnd_status, _handle_xdnd_finished, set_video_mode, restore_video_mode, INVALID_CODEPOINT)
use std.core
use std.core.mem
use std.math (abs)
use std.os.prim
use std.os.time
use std.core.str as str
use std.os.ui.render.vk.vulkan (vk_create_xcb_surface_khr, vk_create_xlib_surface_khr, vkGetPhysicalDeviceSurfaceCapabilitiesKHR, vkGetPhysicalDeviceSurfaceSupportKHR)
use std.os.ui.window.consts
use std.os.ui.window.event as ui_event
use std.os.ui.window.platform.api
use std.os.ui.window.platform.state as platform_state
use std.os.ui.window.platform.linux.x11.common as x11_common
use std.os.ui.window.platform.linux.x11.keymap as x11_keymap
use std.core.common as common
use std.os.ui.render.dump as ui_profile

fn _get_x11_val(str key, any default=0) any {
   def p = platform_state._get_platform_val("platform", 0)
   if !is_dict(p) { return default }
   def x11 = p.get("x11", 0)
   if !is_dict(x11) { return default }
   x11.get(key, default)
}

fn _set_x11_val(str key, any val) any {
   mut p = platform_state._get_platform_val("platform", 0)
   if !is_dict(p) { p = dict(8) }
   mut x11 = p.get("x11", 0)
   if !is_dict(x11) { x11 = dict(8) }
   x11[key] = val
   p["x11"] = x11
   platform_state._set_platform_val("platform", p)
   val
}

fn _is_debug() bool {
   ui_profile.debug_enabled()
}

fn _dbg_tagged(str tag, any msg, str env_gate="") bool {
   if env_gate.len > 0 && !ui_profile.env_truthy_cached(env_gate) { return false }
   ui_profile.eprint_text("[" + tag + "] " + to_str(msg))
}

fn _dbg(any msg) bool {
   if !_is_debug() { return false }
   _dbg_tagged("ui:x11", msg)
}

fn _dbg_v(any msg) bool {
   if !ui_profile.debug_verbose_enabled() { return false }
   _dbg_tagged("ui:x11", msg)
}

fn _dbg_err(any msg) bool { _dbg_tagged("ui:x11:error", msg) }

fn _input_debug_enabled() bool {
   ;; Do not let generic -v/--verbose enable per-event mouse/key logs.
   ;; During captured camera look and gizmo drags those logs can dominate the
   ;; frame and make glTF animation look frozen.  Use NY_UI_INPUT_TRACE=1 when
   ;; the actual input stream is needed.
   ui_profile.env_truthy_cached("NY_UI_INPUT_TRACE") || ui_profile.event_trace_enabled()
}

fn _dbg_input(any msg) bool {
   if !_input_debug_enabled() { return false }
   _dbg_tagged("ui:x11:input", msg)
}

fn _dbg_key_name(any win, int key, int scancode=0) str {
   if key < 0 { return "none" }
   def name = get_key_name(win, key, scancode)
   if name && is_str(name) && name.len > 0 { return name + "(" + to_str(key) + ")" }
   if key >= 32 && key <= 126 { return str.chr(key) + "(" + to_str(key) + ")" }
   to_str(key)
}

def InputOutput = 1
def AllocNone = 0
def CWBackPixel = (1 << 1)
def CWBorderPixel = (1 << 3)
def CWColormap = (1 << 13)
def CWEventMask = (1 << 11)
def CWOverrideRedirect = (1 << 9)
def ShiftMask = (1 << 0)
def LockMask = (1 << 1)
def ControlMask = (1 << 2)
def Mod1Mask = (1 << 3)
def Mod2Mask = (1 << 4)
def Mod4Mask = (1 << 6)
def Button1Mask = (1 << 8)
def Button2Mask = (1 << 9)
def Button3Mask = (1 << 10)
def Button4Mask = (1 << 11)
def Button5Mask = (1 << 12)
def KeyPressMask = (1 << 0)
def KeyReleaseMask = (1 << 1)
def ButtonPressMask = (1 << 2)
def ButtonReleaseMask = (1 << 3)
def EnterWindowMask = (1 << 4)
def LeaveWindowMask = (1 << 5)
def PointerMotionMask = (1 << 6)
def ExposureMask = (1 << 15)
def VisibilityChangeMask = (1 << 16)
def StructureNotifyMask = (1 << 17)
def FocusChangeMask = (1 << 21)
def PropertyChangeMask = (1 << 22)
def SubstructureNotifyMask = (1 << 19)
def SubstructureRedirectMask = (1 << 20)
def PropertyNewValue = 0
def WithdrawnState = 0
def NormalState = 1
def IconicState = 3
def IsViewable = 2
def XA_ATOM = 4
def XA_CARDINAL = 6
def XA_STRING = 31
def AnyPropertyType = 0
def PropModeReplace = 0
def PropModeAppend = 2
def NoAtom = 0
def NoEventMask = 0
def CurrentTime = 0
def RevertToParent = 2
def RevertToPointerRoot = 1
def GrabModeAsync = 1
def GrabSuccess = 0
def XBufferOverflow = -1
def XLookupChars = 2
def XLookupBoth = 4
def XIMPreeditNothing = 0x0008
def XIMStatusNothing = 0x0400
def Success = 0
def XkbUseCoreKbd = 0x0100
def XkbEventCode = 0
def XkbStateNotify = 2
def XkbGroupStateMask = (1 << 4)
def XI_2_Major = 2
def XI_2_Minor = 0
def XIAllMasterDevices = 1
def XI_RawMotion = 17
def MWM_HINTS_DECORATIONS = 2
def MWM_DECOR_ALL = 1
def InputHint = (1 << 0)
def StateHint = (1 << 1)
def PPosition = (1 << 2)
def PMinSize = (1 << 4)
def PMaxSize = (1 << 5)
def PAspect = (1 << 7)
def PWinGravity = (1 << 9)
def StaticGravity = 10
def XC_left_ptr = 68
def XC_xterm = 152
def XC_crosshair = 34
def XC_hand2 = 60
def XC_sb_h_double_arrow = 108
def XC_sb_v_double_arrow = 116
def XC_fleur = 52

comptime table X11StandardCursorShape {
   ARROW_CURSOR, IBEAM_CURSOR, CROSSHAIR_CURSOR, POINTING_HAND_CURSOR,
   RESIZE_EW_CURSOR, RESIZE_NS_CURSOR, RESIZE_NWSE_CURSOR, RESIZE_NESW_CURSOR,
   RESIZE_ALL_CURSOR, NOT_ALLOWED_CURSOR -> true
}

comptime table X11CursorThemeName {
   ARROW_CURSOR -> "default"
   IBEAM_CURSOR -> "text"
   CROSSHAIR_CURSOR -> "crosshair"
   POINTING_HAND_CURSOR -> "pointer"
   RESIZE_EW_CURSOR -> "ew-resize"
   RESIZE_NS_CURSOR -> "ns-resize"
   RESIZE_NWSE_CURSOR -> "nwse-resize"
   RESIZE_NESW_CURSOR -> "nesw-resize"
   RESIZE_ALL_CURSOR -> "all-scroll"
   NOT_ALLOWED_CURSOR -> "not-allowed"
}

comptime table X11CursorFontShape {
   ARROW_CURSOR -> XC_left_ptr
   IBEAM_CURSOR -> XC_xterm
   CROSSHAIR_CURSOR -> XC_crosshair
   POINTING_HAND_CURSOR -> XC_hand2
   RESIZE_EW_CURSOR -> XC_sb_h_double_arrow
   RESIZE_NS_CURSOR -> XC_sb_v_double_arrow
   RESIZE_ALL_CURSOR -> XC_fleur
}

def NET_WM_STATE_REMOVE = 0
def NET_WM_STATE_ADD = 1
def NET_WM_STATE_TOGGLE = 2
def RRScreenChangeNotify = 0
def RRNotify = 1
def RRNotify_CrtcChange = 0
def RRNotify_OutputChange = 1
def RRNotify_ResourceChange = 5
def RR_Rotate_0 = 1
def RR_Rotate_90 = 2
def RR_Rotate_270 = 8
def RR_Interlace = 0x10
def RRScreenChangeNotifyMask = (1 << 0)
def RRCrtcChangeNotifyMask = (1 << 1)
def RROutputChangeNotifyMask = (1 << 2)
def RR_Connected = 0
def XDND_VERSION = 5
def Button1 = 1
def Button2 = 2
def Button3 = 3
def Button4 = 4
def Button5 = 5
def Button6 = 6
def Button7 = 7
def NotifyGrab = 1
def NotifyUngrab = 2
def KeyPress = 2
def KeyRelease = 3
def ButtonPress = 4
def ButtonRelease = 5
def MotionNotify = 6
def EnterNotify = 7
def LeaveNotify = 8
def FocusIn = 9
def FocusOut = 10
def Expose = 12
def VisibilityNotify = 15
def DestroyNotify = 17
def ReparentNotify = 21
def ConfigureNotify = 22
def SelectionClear = 29
def SelectionRequest = 30
def SelectionNotify = 31
def PropertyNotify = 28
def ClientMessage = 33
def GenericEvent = 35

fn available() bool {
   "Returns true if the X11 backend is available on Linux with a set DISPLAY."
   #linux {
      if common.env_trim("DISPLAY").len == 0 { return false }
      return true
   } #else {
      return false
   } #endif
}

fn get_backend_name() str {
   "Identifies this low-level backend."
   "x11"
}

fn _cursor_get(int id) any { _get_x11_val("cursor_specs", dict(8)).get(id, 0) }

fn _cursor_put(int id, any spec) any {
   mut specs = _get_x11_val("cursor_specs", dict(8))
   specs[id] = spec
   _set_x11_val("cursor_specs", specs)
}

fn _next_cursor_id() int {
   def nid = int(_get_x11_val("cursor_next_id", 1))
   _set_x11_val("cursor_next_id", nid + 1)
   nid
}

#linux {
   #link "libX11.so"
   #link "libX11.so.6"
   #link "libX11-xcb.so.1"
   #include <X11/Xlib.h>
   #include <X11/Xutil.h>
   #include <X11/Xatom.h>
   #include <X11/XKBlib.h>
   #include <X11/Xlib-xcb.h>
   #link "libXi.so"
   #include <X11/extensions/XInput2.h>
   #link "libXfixes.so"
   #include <X11/extensions/Xfixes.h>
   #link "libXext.so"
   #include <X11/extensions/shape.h>
   #link "libXcursor.so"
   #include <X11/Xcursor/Xcursor.h>
   #link "libXrandr.so"
   #include <X11/extensions/Xrandr.h>
   #link "libvulkan.so"
   #include <vulkan/vulkan_xcb.h>
   #include <vulkan/vulkan_xlib.h>
   extern "X11" {
      fn _c_xset_locale_modifiers(ptr modifiers) ptr as "XSetLocaleModifiers"
      fn _c_xopenim(ptr display, ptr rdb, ptr resource_name, ptr resource_class) ptr as "XOpenIM"
      fn _c_xcloseim(ptr im) i32 as "XCloseIM"
      fn _c_xcreateic(
         ptr im,
         ptr name_input_style,
         u64 input_style,
         ptr name_client_window,
         u64 client_window,
         ptr name_focus_window,
         u64 focus_window,
         ptr terminator,
      ) ptr as "XCreateIC"
      fn _c_xdestroyic(ptr ic) as "XDestroyIC"
      fn _c_xseticfocus(ptr ic) as "XSetICFocus"
      fn _c_xunseticfocus(ptr ic) as "XUnsetICFocus"
      fn _c_xutf8_lookup_string(
         ptr ic,
         ptr event,
         ptr buffer,
         i32 buffer_cap,
         ptr keysym,
         ptr status,
      ) i32 as "Xutf8LookupString"
      fn _c_xset_error_handler(fnptr handler) fnptr as "XSetErrorHandler"
      fn _c_xgetxcbconnection(ptr display) ptr as "XGetXCBConnection"
      fn _c_xkb_keycode_to_keysym(ptr display, u32 keycode, u32 group, u32 level) u64 as "XkbKeycodeToKeysym"
      fn _c_xkeysym_to_keycode(ptr display, u64 keysym) u32 as "XKeysymToKeycode"
      fn _c_xkeysym_to_string(u64 keysym) ptr as "XKeysymToString"
   }
   extern "Xcursor" {
      fn XcursorImageCreate(i32 width, i32 height) ptr
      fn XcursorImageDestroy(ptr image) any
      fn XcursorImageLoadCursor(ptr display, ptr image) u64
      fn XcursorLibraryLoadImage(ptr file, ptr theme, i32 size) ptr
      fn XcursorGetDefaultSize(ptr display) i32
      fn XcursorGetTheme(ptr display) ptr
   }
   extern "Xfixes" {
      fn XFixesHideCursor(ptr display, u64 win) any
      fn XFixesShowCursor(ptr display, u64 win) any
   }
   extern "Xext" {
      fn XShapeCombineRectangles(ptr display, u64 dest, i32 dest_kind, i32 x_off, i32 y_off, ptr rects, i32 n_rects, i32 op, i32 ordering) any
   }
   extern "Xi" {
      fn XIQueryVersion(ptr display, ptr major_version_inout, ptr minor_version_inout) i32
      fn XISelectEvents(ptr display, u64 win, ptr masks, i32 num_masks) i32
   }
   extern "Xrandr" {
      fn XRRUpdateConfiguration(ptr event) i32
      fn XRRGetScreenResourcesCurrent(ptr display, u64 window) ptr
      fn XRRFreeScreenResources(ptr resources) any
      fn XRRGetOutputPrimary(ptr display, u64 window) u64
      fn XRRGetOutputInfo(ptr display, ptr resources, u64 output) ptr
      fn XRRFreeOutputInfo(ptr output_info) any
      fn XRRGetCrtcInfo(ptr display, ptr resources, u64 crtc) ptr
      fn XRRFreeCrtcInfo(ptr crtc_info) any
      fn XRRSetCrtcConfig(ptr display, ptr resources, u64 crtc, u64 timestamp, i32 x, i32 y, i32 mode, i32 rotation, ptr outputs, i32 noutputs) i32
      fn XRRGetCrtcGammaSize(ptr display, u64 crtc) i32
      fn XRRAllocGamma(i32 size) ptr
      fn XRRFreeGamma(ptr gamma) any
      fn XRRGetCrtcGamma(ptr display, u64 crtc) ptr
      fn XRRSetCrtcGamma(ptr display, u64 crtc, ptr gamma) any
   }
} #else {
   fn _c_xset_locale_modifiers(..._args) any { 0 }
   fn _c_xopenim(..._args) any { 0 }
   fn _c_xcloseim(..._args) any { 0 }
   fn _c_xcreateic(..._args) any { 0 }
   fn _c_xdestroyic(..._args) any { 0 }
   fn _c_xseticfocus(..._args) any { 0 }
   fn _c_xunseticfocus(..._args) any { 0 }
   fn _c_xutf8_lookup_string(..._args) any { 0 }
   fn _c_xset_error_handler(..._args) any { 0 }
   fn _c_xgetxcbconnection(..._args) any { 0 }
   fn _c_xkb_keycode_to_keysym(..._args) any { 0 }
   fn _c_xkeysym_to_keycode(..._args) any { 0 }
   fn _c_xkeysym_to_string(..._args) any { 0 }
   fn XAllocClassHint(..._args) any {
      "Runs the XAllocClassHint operation."
      0
   }
   fn XAllocSizeHints(..._args) any {
      "Runs the XAllocSizeHints operation."
      0
   }
   fn XAllocWMHints(..._args) any {
      "Runs the XAllocWMHints operation."
      0
   }
   fn XChangeProperty(..._args) any {
      "Runs the XChangeProperty operation."
      0
   }
   fn XChangeWindowAttributes(..._args) any {
      "Runs the XChangeWindowAttributes operation."
      0
   }
   fn XCheckTypedWindowEvent(..._args) any {
      "Runs the XCheckTypedWindowEvent operation."
      0
   }
   fn XCloseDisplay(..._args) any {
      "Runs the XCloseDisplay operation."
      0
   }
   fn XConvertSelection(..._args) any {
      "Runs the XConvertSelection operation."
      0
   }
   fn XCreateColormap(..._args) any {
      "Runs the XCreateColormap operation."
      0
   }
   fn XCreateFontCursor(..._args) any {
      "Runs the XCreateFontCursor operation."
      0
   }
   fn XCreateGC(..._args) any {
      "Runs the XCreateGC operation."
      0
   }
   fn XCreateImage(..._args) any {
      "Runs the XCreateImage operation."
      0
   }
   fn XCreateWindow(..._args) any {
      "Runs the XCreateWindow operation."
      0
   }
   fn XcursorGetDefaultSize(..._args) any {
      "Runs the XcursorGetDefaultSize operation."
      0
   }
   fn XcursorGetTheme(..._args) any {
      "Runs the XcursorGetTheme operation."
      0
   }
   fn XcursorImageCreate(..._args) any {
      "Runs the XcursorImageCreate operation."
      0
   }
   fn XcursorImageDestroy(..._args) any {
      "Runs the XcursorImageDestroy operation."
      0
   }
   fn XcursorImageLoadCursor(..._args) any {
      "Runs the XcursorImageLoadCursor operation."
      0
   }
   fn XcursorLibraryLoadImage(..._args) any {
      "Runs the XcursorLibraryLoadImage operation."
      0
   }
   fn XDefaultDepth(..._args) any {
      "Runs the XDefaultDepth operation."
      0
   }
   fn XDefaultScreen(..._args) any {
      "Runs the XDefaultScreen operation."
      0
   }
   fn XDefaultVisual(..._args) any {
      "Runs the XDefaultVisual operation."
      0
   }
   fn XDefineCursor(..._args) any {
      "Runs the XDefineCursor operation."
      0
   }
   fn XDeleteProperty(..._args) any {
      "Runs the XDeleteProperty operation."
      0
   }
   fn XDestroyWindow(..._args) any {
      "Runs the XDestroyWindow operation."
      0
   }
   fn XDisplayHeight(..._args) any {
      "Runs the XDisplayHeight operation."
      0
   }
   fn XDisplayKeycodes(..._args) any {
      "Runs the XDisplayKeycodes operation."
      0
   }
   fn XDisplayWidth(..._args) any {
      "Runs the XDisplayWidth operation."
      0
   }
   fn XEventsQueued(..._args) any {
      "Runs the XEventsQueued operation."
      0
   }
   fn XFilterEvent(..._args) any {
      "Runs the XFilterEvent operation."
      0
   }
   fn XFixesHideCursor(..._args) any {
      "Runs the XFixesHideCursor operation."
      0
   }
   fn XFixesShowCursor(..._args) any {
      "Runs the XFixesShowCursor operation."
      0
   }
   fn XFlush(..._args) any {
      "Runs the XFlush operation."
      0
   }
   fn XFree(..._args) any {
      "Runs the XFree operation."
      0
   }
   fn XFreeColormap(..._args) any {
      "Runs the XFreeColormap operation."
      0
   }
   fn XFreeCursor(..._args) any {
      "Runs the XFreeCursor operation."
      0
   }
   fn XFreeEventData(..._args) any {
      "Runs the XFreeEventData operation."
      0
   }
   fn XFreeGC(..._args) any {
      "Runs the XFreeGC operation."
      0
   }
   fn XGetEventData(..._args) any {
      "Runs the XGetEventData operation."
      0
   }
   fn XGetInputFocus(..._args) any {
      "Runs the XGetInputFocus operation."
      0
   }
   fn XGetKeyboardMapping(..._args) any {
      "Runs the XGetKeyboardMapping operation."
      0
   }
   fn XGetSelectionOwner(..._args) any {
      "Runs the XGetSelectionOwner operation."
      0
   }
   fn XGetVisualInfo(..._args) any {
      "Runs the XGetVisualInfo operation."
      0
   }
   fn XGetWindowAttributes(..._args) any {
      "Runs the XGetWindowAttributes operation."
      0
   }
   fn XGetWindowProperty(..._args) any {
      "Runs the XGetWindowProperty operation."
      0
   }
   fn XGetWMNormalHints(..._args) any {
      "Runs the XGetWMNormalHints operation."
      0
   }
   fn XGrabPointer(..._args) any {
      "Runs the XGrabPointer operation."
      0
   }
   fn XIconifyWindow(..._args) any {
      "Runs the XIconifyWindow operation."
      0
   }
   fn XInitThreads(..._args) any {
      "Runs the XInitThreads operation."
      0
   }
   fn XInternAtom(..._args) any {
      "Runs the XInternAtom operation."
      0
   }
   fn XIQueryVersion(..._args) any {
      "Runs the XIQueryVersion operation."
      0
   }
   fn XISelectEvents(..._args) any {
      "Runs the XISelectEvents operation."
      0
   }
   fn XkbQueryExtension(..._args) any {
      "Runs the XkbQueryExtension operation."
      0
   }
   fn XkbSetDetectableAutoRepeat(..._args) any {
      "Runs the XkbSetDetectableAutoRepeat operation."
      0
   }
   fn XKeysymToKeycode(..._args) any {
      "Runs the XKeysymToKeycode operation."
      0
   }
   fn XLookupString(..._args) any {
      "Runs the XLookupString operation."
      0
   }
   fn XMapRaised(..._args) any {
      "Runs the XMapRaised operation."
      0
   }
   fn XMapWindow(..._args) any {
      "Runs the XMapWindow operation."
      0
   }
   fn XMoveWindow(..._args) any {
      "Runs the XMoveWindow operation."
      0
   }
   fn XNextEvent(..._args) any {
      "Runs the XNextEvent operation."
      0
   }
   fn XOpenDisplay(..._args) any {
      "Runs the XOpenDisplay operation."
      0
   }
   fn XPeekEvent(..._args) any {
      "Runs the XPeekEvent operation."
      0
   }
   fn XPending(..._args) any {
      "Runs the XPending operation."
      0
   }
   fn XPutImage(..._args) any {
      "Runs the XPutImage operation."
      0
   }
   fn XQueryExtension(..._args) any {
      "Runs the XQueryExtension operation."
      0
   }
   fn XQueryKeymap(..._args) any {
      "Runs the XQueryKeymap operation."
      0
   }
   fn XQueryPointer(..._args) any {
      "Runs the XQueryPointer operation."
      0
   }
   fn XRaiseWindow(..._args) any {
      "Runs the XRaiseWindow operation."
      0
   }
   fn XResizeWindow(..._args) any {
      "Runs the XResizeWindow operation."
      0
   }
   fn XrmInitialize(..._args) any {
      "Runs the XrmInitialize operation."
      0
   }
   fn XRootWindow(..._args) any {
      "Runs the XRootWindow operation."
      0
   }
   fn XRRAllocGamma(..._args) any {
      "Runs the XRRAllocGamma operation."
      0
   }
   fn XRRFreeCrtcInfo(..._args) any {
      "Runs the XRRFreeCrtcInfo operation."
      0
   }
   fn XRRFreeGamma(..._args) any {
      "Runs the XRRFreeGamma operation."
      0
   }
   fn XRRFreeOutputInfo(..._args) any {
      "Runs the XRRFreeOutputInfo operation."
      0
   }
   fn XRRFreeScreenResources(..._args) any {
      "Runs the XRRFreeScreenResources operation."
      0
   }
   fn XRRGetCrtcGamma(..._args) any {
      "Runs the XRRGetCrtcGamma operation."
      0
   }
   fn XRRGetCrtcGammaSize(..._args) any {
      "Runs the XRRGetCrtcGammaSize operation."
      0
   }
   fn XRRGetCrtcInfo(..._args) any {
      "Runs the XRRGetCrtcInfo operation."
      0
   }
   fn XRRGetOutputInfo(..._args) any {
      "Runs the XRRGetOutputInfo operation."
      0
   }
   fn XRRGetOutputPrimary(..._args) any {
      "Runs the XRRGetOutputPrimary operation."
      0
   }
   fn XRRGetScreenResourcesCurrent(..._args) any {
      "Runs the XRRGetScreenResourcesCurrent operation."
      0
   }
   fn XRRSetCrtcConfig(..._args) any {
      "Runs the XRRSetCrtcConfig operation."
      0
   }
   fn XRRSetCrtcGamma(..._args) any {
      "Runs the XRRSetCrtcGamma operation."
      0
   }
   fn XRRUpdateConfiguration(..._args) any {
      "Runs the XRRUpdateConfiguration operation."
      0
   }
   fn XSelectInput(..._args) any {
      "Runs the XSelectInput operation."
      0
   }
   fn XSendEvent(..._args) any {
      "Runs the XSendEvent operation."
      0
   }
   fn XSetClassHint(..._args) any {
      "Runs the XSetClassHint operation."
      0
   }
   fn XSetInputFocus(..._args) any {
      "Runs the XSetInputFocus operation."
      0
   }
   fn XSetSelectionOwner(..._args) any {
      "Runs the XSetSelectionOwner operation."
      0
   }
   fn XSetWMHints(..._args) any {
      "Runs the XSetWMHints operation."
      0
   }
   fn XSetWMNormalHints(..._args) any {
      "Runs the XSetWMNormalHints operation."
      0
   }
   fn XSetWMProtocols(..._args) any {
      "Runs the XSetWMProtocols operation."
      0
   }
   fn XShapeCombineRectangles(..._args) any {
      "Runs the XShapeCombineRectangles operation."
      0
   }
   fn XStoreName(..._args) any {
      "Runs the XStoreName operation."
      0
   }
   fn XSync(..._args) any {
      "Runs the XSync operation."
      0
   }
   fn XTranslateCoordinates(..._args) any {
      "Runs the XTranslateCoordinates operation."
      0
   }
   fn XUndefineCursor(..._args) any {
      "Runs the XUndefineCursor operation."
      0
   }
   fn XUngrabPointer(..._args) any {
      "Runs the XUngrabPointer operation."
      0
   }
   fn XUnmapWindow(..._args) any {
      "Runs the XUnmapWindow operation."
      0
   }
   fn XWarpPointer(..._args) any {
      "Runs the XWarpPointer operation."
      0
   }
} #endif

fn _ensure_x11_connected() bool {
   if _get_x11_val("x11_connected", 0) { return true }
   XInitThreads()
   XrmInitialize()
   _setup_x11_error_handler()
   _set_x11_val("x11_connected", 1)
   true
}

fn open_display(str name="") any {
   "Opens a connection to the X server."
   _ensure_x11_connected()
   mut display = 0
   if name.len > 0 {
      display = XOpenDisplay(cstr(name))
      if !display { display = XOpenDisplay(0) }
   } else {
      display = XOpenDisplay(0)
   }
   if !display {
      _dbg_err("open_display: XOpenDisplay() failed!")
      return 0
   }
   _set_x11_val("display", display)
   display
}

fn close_display(any display) int {
   "Closes an X display connection."
   if !display { return 0 }
   XCloseDisplay(display)
}

fn _open_input_method(any display) any {
   if !display { return 0 }
   _c_xset_locale_modifiers(cstr(""))
   mut im = _c_xopenim(display, 0, 0, 0)
   if !im {
      _c_xset_locale_modifiers(cstr("C"))
      im = _c_xopenim(display, 0, 0, 0)
   }
   if !im {
      _c_xset_locale_modifiers(cstr("POSIX"))
      im = _c_xopenim(display, 0, 0, 0)
   }
   if im && ui_profile.debug_verbose_enabled() { _dbg("_open_input_method: XIM opened successfully") }
   im
}

fn _create_input_context(any im, any window_handle) any {
   if !im || !window_handle { return 0 }
   def ic = _c_xcreateic(im,
      cstr("inputStyle"), XIMPreeditNothing | XIMStatusNothing,
      cstr("clientWindow"), window_handle,
      cstr("focusWindow"), window_handle,
   0)
   if !ic { _dbg_input("XCreateIC failed; continuing without text input context") }
   ic
}

fn _ensure_input_context(any win) any {
   if !win || !is_dict(win) { return win }
   if win.get("ic", 0) { return win }
   def im = win.get("im", 0)
   def handle = win.get("handle", 0)
   if !im || !handle { return win }
   def ic = _create_input_context(im, handle)
   if ic {
      win["ic"] = ic
      _sync_window_state(win)
   }
   win
}

fn _emit_utf8_chars(list events, dict win, ptr buffer, int count, int mods, bool plain) list {
   if !buffer || count <= 0 { return events }
   mut i = 0
   while i < count {
      def b0 = load8(buffer, i)
      mut codepoint = -1
      mut next = i + 1
      if b0 < 0x80 { codepoint = b0 } elif band(b0, 0xe0) == 0xc0 && i + 1 < count {
         codepoint = bor(bshl(band(b0, 0x1f), 6), band(load8(buffer, i + 1), 0x3f))
         next = i + 2
      } elif band(b0, 0xf0) == 0xe0 && i + 2 < count {
         codepoint = bor(
            bshl(band(b0, 0x0f), 12),
            bor(bshl(band(load8(buffer, i + 1), 0x3f), 6), band(load8(buffer, i + 2), 0x3f))
         )
         next = i + 3
      } elif band(b0, 0xf8) == 0xf0 && i + 3 < count {
         codepoint = bor(
            bshl(band(b0, 0x07), 18),
            bor(
               bshl(band(load8(buffer, i + 1), 0x3f), 12),
               bor(bshl(band(load8(buffer, i + 2), 0x3f), 6), band(load8(buffer, i + 3), 0x3f))
            )
         )
         next = i + 4
      }
      if codepoint >= 0 {
         mut char_data = dict(8)
         char_data["char"] = codepoint
         char_data["mod"] = mods
         char_data["mods"] = mods
         char_data["plain"] = plain
         _dbg_input("char utf8 codepoint=" + to_str(codepoint) +
            " mods=0x" + str.to_hex(mods) +
         " plain=" + to_str(plain))
         events = _push_translated_event(events, 3, win, char_data)
      }
      if next > i { i = next }
      else { i += 1 }
   }
   events
}

fn _emit_ic_chars(list events, dict win, ptr event_ptr, int mods, bool plain) list {
   def ic = win.get("ic", 0)
   if !ic { return events }
   def status_ptr = malloc(4)
   def keysym_ptr = malloc(8)
   mut buffer = malloc(128)
   mut buffer_cap = 127
   if !status_ptr || !keysym_ptr || !buffer {
      if status_ptr { free(status_ptr) }
      if keysym_ptr { free(keysym_ptr) }
      if buffer { free(buffer) }
      return events
   }
   store32(status_ptr, 0, 0)
   store64_h(keysym_ptr, 0, 0)
   mut count = _c_xutf8_lookup_string(ic, event_ptr, buffer, buffer_cap, keysym_ptr, status_ptr)
   mut status = load32(status_ptr, 0)
   if ui_profile.env_truthy_cached("NY_UI_INPUT_TRACE") {
      _dbg_input("xim lookup count=" + to_str(count) +
         " status=" + to_str(status) +
      " keysym=0x" + str.to_hex(load64_h(keysym_ptr, 0)))
   }
   if status == XBufferOverflow && count > 0 {
      free(buffer)
      buffer = malloc(count + 1)
      if !buffer {
         free(status_ptr, keysym_ptr)
         return events
      }
      buffer_cap = count
      store32(status_ptr, 0, 0)
      store64_h(keysym_ptr, 0, 0)
      count = _c_xutf8_lookup_string(ic, event_ptr, buffer, buffer_cap, keysym_ptr, status_ptr)
      status = load32(status_ptr, 0)
      if ui_profile.env_truthy_cached("NY_UI_INPUT_TRACE") {
         _dbg_input("xim lookup resize count=" + to_str(count) +
            " status=" + to_str(status) +
         " keysym=0x" + str.to_hex(load64_h(keysym_ptr, 0)))
      }
   }
   if count > 0 && (status == XLookupChars || status == XLookupBoth) {
      store8(buffer, 0, count)
      events = _emit_utf8_chars(events, win, buffer, count, mods, plain)
   }
   free(status_ptr, keysym_ptr, buffer)
   events
}

comptime template _x11_wrap1(name, doc, call_fn){
   fn ${name}(any a) any {
      doc
      call_fn(a)
   }
}

comptime template _x11_wrap2(name, doc, call_fn){
   fn ${name}(any a, any b) any {
      doc
      call_fn(a, b)
   }
}

comptime emit _x11_wrap1(default_screen, "Returns the default X11 screen index.", XDefaultScreen)
comptime emit _x11_wrap2(root_window, "Returns the root window for `screen_number`.", XRootWindow)
comptime emit _x11_wrap2(default_visual, "Returns the default visual pointer for `screen_number`.", XDefaultVisual)
comptime emit _x11_wrap2(default_depth, "Returns the default depth for `screen_number`.", XDefaultDepth)

fn intern_atom(any display, str atom_name, bool only_if_exists=false) any {
   "Interns an X11 atom and returns its id."
   XInternAtom(display, cstr(atom_name), only_if_exists ? 1 : 0)
}

fn create_colormap(any display, any win, any visual, int alloc=AllocNone) any {
   "Creates a colormap for a native X11 window."
   XCreateColormap(display, win, visual, alloc)
}

fn free_colormap(any display, any colormap) any {
   "Frees a colormap created for a native X11 window."
   if display && colormap { XFreeColormap(display, colormap) }
}

fn create_window_raw(
   any display,
   any parent,
   int x,
   int y,
   int width,
   int height,
   int border_width,
   int depth,
   int klass,
   any visual,
   int value_mask,
   any attributes
) int {
   "Creates a raw X11 window."
   XCreateWindow(
      display, parent, x, y, width, height,
      border_width, depth, klass, visual, value_mask, attributes,
   )
}

comptime emit _x11_wrap2(destroy_window_raw, "Destroys a raw X11 window.", XDestroyWindow)
comptime emit _x11_wrap2(map_window, "Maps a raw X11 window.", XMapWindow)
comptime emit _x11_wrap2(unmap_window, "Unmaps a raw X11 window.", XUnmapWindow)
comptime emit _x11_wrap1(pending, "Returns the number of queued X11 events.", XPending)

fn get_window_property(any display, any win, any property, any typ, int long_length=4096) any {
   "Read a native X11 window property into Ny-managed storage."
   if !display || !win || !property { return false }
   def actual_type = malloc(8)
   def actual_format = malloc(4)
   def nitems = malloc(8)
   def bytes_after = malloc(8)
   def prop = malloc(8)
   if !actual_type || !actual_format || !nitems || !bytes_after || !prop {
      if actual_type { free(actual_type) }
      if actual_format { free(actual_format) }
      if nitems { free(nitems) }
      if bytes_after { free(bytes_after) }
      if prop { free(prop) }
      return false
   }
   store64_h(prop, 0, 0)
   def status = XGetWindowProperty(display, win, property, 0, long_length, 0, typ,
   actual_type, actual_format, nitems, bytes_after, prop)
   if status != Success {
      free(actual_type, actual_format, nitems, bytes_after, prop)
      return false
   }
   def data_ptr = load64(prop, 0)
   def out = {
      "data": data_ptr, "data_ptr": data_ptr, "type": load64_h(actual_type, 0),
      "format": load32(actual_format, 0), "count": load64_h(nitems, 0),
      "bytes_after": load64_h(bytes_after, 0)
   }
   free(actual_type, actual_format, nitems, bytes_after, prop)
   out
}

fn _prop_data_ptr(any prop) any {
   if !prop || !is_dict(prop) { return 0 }
   prop.get("data_ptr", prop.get("data", 0))
}

fn get_window_state(any display, any win, any wm_state_atom) int {
   "Read EWMH window-state atoms for a native X11 window."
   if !display || !win || !wm_state_atom { return WithdrawnState }
   def prop = get_window_property(display, win, wm_state_atom, wm_state_atom)
   if !prop || !is_dict(prop) { return WithdrawnState }
   def data = _prop_data_ptr(prop)
   def count = prop.get("count", 0)
   mut result = WithdrawnState
   if data && count >= 2 { result = load32(data, 0) }
   if data { XFree(data) }
   result
}

fn is_window_iconified(any display, any win, any wm_state_atom) bool {
   "Returns true when the window is in X11 `IconicState`."
   get_window_state(display, win, wm_state_atom) == IconicState
}

fn get_cardinal_value(any display, any win, any property, int index=0) any {
   "Returns the CARDINAL value at `index` for `property`, or false if missing."
   if !display || !win || !property || index < 0 { return false }
   def prop = get_window_property(display, win, property, XA_CARDINAL)
   if !prop || !is_dict(prop) { return false }
   def data = _prop_data_ptr(prop)
   def count = prop.get("count", 0)
   mut value = 0
   if data && index < count { value = load64_h(data, index * 8) }
   if data { XFree(data) }
   value
}

fn property_has_atom(any display, any win, any property, any atom) bool {
   "Returns true when `property` on `win` contains `atom`."
   if !display || !win || !property || !atom { return false }
   def prop = get_window_property(display, win, property, XA_ATOM)
   if !prop || !is_dict(prop) { return false }
   def data = _prop_data_ptr(prop)
   def count = prop.get("count", 0)
   mut found = false
   if data {
      mut i = 0
      while i < count {
         if load32(data, i * 8) == atom {
            found = true
            break
         }
         i += 1
      }
      XFree(data)
   }
   found
}

fn is_window_maximized(any display, any win, any net_wm_state_atom, any max_vert_atom, any max_horz_atom) bool {
   "Returns true when both maximized state atoms are present."
   property_has_atom(display, win, net_wm_state_atom, max_vert_atom) &&
   property_has_atom(display, win, net_wm_state_atom, max_horz_atom)
}

fn is_window_visible(any display, any win) bool {
   "Returns true when the X11 window is currently viewable."
   if !display || !win { return false }
   def attrs = zalloc(256)
   if !attrs { return false }
   def ok = XGetWindowAttributes(display, win, attrs) != 0
   mut visible = false
   if ok {
      visible = load32(attrs, 92) == IsViewable
   }
   free(attrs)
   visible
}

fn get_window_size(any display, any win) any {
   "Returns `{ width, height }` based on `XGetWindowAttributes`."
   if !display || !win { return false }
   def attrs = zalloc(256)
   if !attrs { return false }
   def ok = XGetWindowAttributes(display, win, attrs) != 0
   if !ok {
      free(attrs)
      return false
   }
   mut out = dict(8)
   out["width"] = load32(attrs, 8)
   out["height"] = load32(attrs, 12)
   free(attrs)
   out
}

fn is_window_floating(any display, any win, any net_wm_state_atom, any above_atom) bool {
   "Returns true when `_NET_WM_STATE_ABOVE` is present."
   property_has_atom(display, win, net_wm_state_atom, above_atom)
}

fn is_window_fullscreen(any display, any win, any net_wm_state_atom, any fullscreen_atom) bool {
   "Returns true when `_NET_WM_STATE_FULLSCREEN` is present."
   property_has_atom(display, win, net_wm_state_atom, fullscreen_atom)
}

fn append_atom_property(any display, any win, any property, any atom) bool {
   "Appends `atom` to an X11 atom-list property if it is not already present."
   if !display || !win || !property || !atom { return false }
   if property_has_atom(display, win, property, atom) { return true }
   def value = zalloc(8)
   if !value { return false }
   store32(value, atom, 0)
   def ok = XChangeProperty(display, win, property, XA_ATOM, 32, PropModeAppend, value, 1) == 0
   free(value)
   ok
}

fn remove_atom_property(any display, any win, any property, any atom) bool {
   "Removes `atom` from an X11 atom-list property."
   if !display || !win || !property || !atom { return false }
   def prop = get_window_property(display, win, property, XA_ATOM)
   if !prop || !is_dict(prop) { return false }
   def data = _prop_data_ptr(prop)
   def count = prop.get("count", 0)
   if !data || count <= 0 {
      if data { XFree(data) }
      return false
   }
   mut found = -1
   mut i = 0
   while i < count {
      if load32(data, i * 8) == atom {
         found = i
         break
      }
      i += 1
   }
   if found < 0 {
      XFree(data)
      return false
   }
   if count == 1 {
      def ok_empty = XChangeProperty(display, win, property, XA_ATOM, 32, PropModeReplace, 0, 0) == 0
      XFree(data)
      return ok_empty
   }
   store32(data, load32(data, (count - 1) * 8), found * 8)
   def ok = XChangeProperty(display, win, property, XA_ATOM, 32, PropModeReplace, data, count - 1) == 0
   XFree(data)
   ok
}

fn send_client_message(any display, any root, any win, any atom, int d0, int d1=0, int d2=0, int d3=0, int d4=0) bool {
   "Sends a 32-bit X11 ClientMessage event."
   if !display || !root || !win || !atom { return false }
   def ev = malloc(96)
   if !ev { return false }
   memset(ev, 0, 96)
   store32(ev, ClientMessage, 0)
   store64_h(ev, win, 32)
   store64_h(ev, atom, 40)
   store32(ev, 32, 48)
   store64_h(ev, d0, 56)
   store64_h(ev, d1, 64)
   store64_h(ev, d2, 72)
   store64_h(ev, d3, 80)
   store64_h(ev, d4, 88)
   def mask = bor(SubstructureNotifyMask, SubstructureRedirectMask)
   def ok = XSendEvent(display, root, 0, mask, ev) != 0
   free(ev)
   ok
}

fn send_wm_state_event(
   any display, any root, any win, any net_wm_state_atom, int action,
   any first_atom, any second_atom=0, int source_indication=1
) bool {
   "Sends an EWMH `_NET_WM_STATE` client message."
   send_client_message(display, root, win, net_wm_state_atom,
   action, first_atom, second_atom, source_indication, 0)
}

fn _iconify_window_raw(any display, any window_handle, int screen_number) bool {
   def ok = XIconifyWindow(display, window_handle, screen_number) != 0
   flush(display)
   ok
}

fn iconify_window(any win) bool {
   "Iconify a native X11 window."
   def dh = _win_display_handle(win)
   if !dh { return false }
   def display = dh.get(0)
   def handle = dh.get(1)
   def screen_number = win.get("screen", 0)
   def override_redirect = win.get("override_redirect", false)
   if override_redirect { return false }
   _iconify_window_raw(display, handle, screen_number)
}

fn _show_window_raw(any display, any window_handle, bool floating=false, any net_wm_state_atom=0, any above_atom=0) bool {
   def trace = ui_profile.env_truthy_cached("NY_UI_STARTUP_TRACE") || ui_profile.env_truthy_cached("NY_UI_DEBUG_WINDOW")
   if floating && net_wm_state_atom && above_atom {
      if trace { ui_profile.print_text("[x11:show] floating.before") }
      append_atom_property(display, window_handle, net_wm_state_atom, above_atom)
      if trace { ui_profile.print_text("[x11:show] floating.after") }
   }
   if trace { ui_profile.print_text("[x11:show] map.before") }
   XMapRaised(display, window_handle)
   if trace { ui_profile.print_text("[x11:show] map.after") }
   if trace { ui_profile.print_text("[x11:show] flush.before") }
   flush(display)
   if trace { ui_profile.print_text("[x11:show] flush.after") }
   true
}

fn show_window(any win) bool {
   "Show and map a native X11 window."
   def dh = _win_display_handle(win)
   if !dh { return false }
   def display = dh.get(0)
   def handle = dh.get(1)
   def floating = win.get("floating", false)
   def net_wm_state_atom = win.get("net_wm_state", 0)
   def above_atom = win.get("net_wm_state_above", 0)
   _show_window_raw(display, handle, floating, net_wm_state_atom, above_atom)
}

fn _hide_window_raw(any display, any window_handle) bool {
   XUnmapWindow(display, window_handle)
   flush(display)
   true
}

fn hide_window(any win) bool {
   "Hide and unmap a native X11 window."
   def dh = _win_display_handle(win)
   dh ? _hide_window_raw(dh.get(0), dh.get(1)) : false
}

fn _update_normal_hints_raw(
   any display, any win, int width, int height,
   bool resizable=true, bool monitor=false,
   int minwidth=-1, int minheight=-1,
   int maxwidth=-1, int maxheight=-1,
   int numer=-1, int denom=-1
) bool {
   if !display || !win { return false }
   def hints = XAllocSizeHints()
   if !hints { return false }
   def supplied = zalloc(8)
   if !supplied {
      XFree(hints)
      return false
   }
   XGetWMNormalHints(display, win, hints, supplied)
   store64_h(hints, band(load64_h(hints, 0), bnot(bor(bor(PMinSize, PMaxSize), PAspect))), 0)
   if !monitor {
      if resizable {
         if minwidth >= 0 && minheight >= 0 {
            store64_h(hints, bor(load64_h(hints, 0), PMinSize), 0)
            store32(hints, minwidth, 24)
            store32(hints, minheight, 28)
         }
         if maxwidth >= 0 && maxheight >= 0 {
            store64_h(hints, bor(load64_h(hints, 0), PMaxSize), 0)
            store32(hints, maxwidth, 32)
            store32(hints, maxheight, 36)
         }
         if numer >= 0 && denom >= 0 {
            store64_h(hints, bor(load64_h(hints, 0), PAspect), 0)
            store32(hints, numer, 48)
            store32(hints, denom, 52)
            store32(hints, numer, 56)
            store32(hints, denom, 60)
         }
      } else {
         store64_h(hints, bor(load64_h(hints, 0), bor(PMinSize, PMaxSize)), 0)
         store32(hints, width, 24)
         store32(hints, width, 32)
         store32(hints, height, 28)
         store32(hints, height, 36)
      }
   }
   XSetWMNormalHints(display, win, hints)
   free(supplied)
   XFree(hints)
   true
}

fn update_normal_hints(
   any display,
   any win,
   int width,
   int height,
   bool resizable=true,
   bool monitor=false,
   int minwidth=-1,
   int minheight=-1,
   int maxwidth=-1,
   int maxheight=-1,
   int numer=-1,
   int denom=-1
) bool {
   "Updates ICCCM WM_NORMAL_HINTS for size limits and aspect ratio."
   _update_normal_hints_raw(
      display, win, width, height,
      resizable, monitor,
      minwidth, minheight,
      maxwidth, maxheight,
      numer, denom,
   )
}

fn _set_window_manager_hints(any display, any win) bool {
   if !display || !win { return false }
   def hints = XAllocWMHints()
   if !hints { return false }
   store64_h(hints, InputHint | StateHint, 0)
   store32(hints, 1, 8)
   store32(hints, NormalState, 12)
   XSetWMHints(display, win, hints)
   XFree(hints)
   true
}

fn _set_initial_normal_hints(
   any display, any win, int width, int height,
   bool resizable=true, int xpos=0, int ypos=0,
   bool honor_position=false,
) bool {
   "Initial X11 normal hints for native window creation."
   if !display || !win { return false }
   def hints = XAllocSizeHints()
   if !hints { return false }
   store64_h(hints, 0, 0)
   if !resizable {
      store64_h(hints, bor(load64_h(hints, 0), bor(PMinSize, PMaxSize)), 0)
      store32(hints, width, 24)
      store32(hints, height, 28)
      store32(hints, width, 32)
      store32(hints, height, 36)
   }
   if honor_position {
      store64_h(hints, bor(load64_h(hints, 0), PPosition), 0)
      store32(hints, 0, 8)
      store32(hints, 0, 12)
   }
   store64_h(hints, bor(load64_h(hints, 0), PWinGravity), 0)
   store32(hints, StaticGravity, 72)
   XSetWMNormalHints(display, win, hints)
   XFree(hints)
   true
}

fn _set_u32_property(any display, any win, any property_atom, any type_atom, any value) bool {
   if !display || !win || !property_atom || !type_atom { return false }
   def data = zalloc(8)
   if !data { return false }
   store32(data, value, 0)
   XChangeProperty(display, win, property_atom, type_atom, 32, PropModeReplace, data, 1)
   free(data)
   true
}

fn _set_cardinal_property(any display, any win, any property_atom, any value) bool { _set_u32_property(display, win, property_atom, XA_CARDINAL, value) }

fn _set_atom_property(any display, any win, any property_atom, any value_atom) bool {
   if !value_atom { return false }
   _set_u32_property(display, win, property_atom, XA_ATOM, value_atom)
}

fn _set_window_pid(any display, any win, any net_wm_pid_atom) bool {
   if !display || !win || !net_wm_pid_atom { return false }
   _set_cardinal_property(display, win, net_wm_pid_atom, pid())
}

fn _set_window_type_normal(any display, any win, any net_wm_window_type_atom, any net_wm_window_type_normal_atom) bool {
   if !display || !win || !net_wm_window_type_atom || !net_wm_window_type_normal_atom { return false }
   _set_atom_property(display, win, net_wm_window_type_atom, net_wm_window_type_normal_atom)
}

fn _set_compositor_bypass(any display, any win, any bypass_atom, bool enabled) bool {
   if !display || !win || !bypass_atom { return false }
   if enabled { return _set_cardinal_property(display, win, bypass_atom, 1) }
   XDeleteProperty(display, win, bypass_atom)
   flush(display)
   true
}

fn _set_fullscreen_monitors(any display, any root, any win, any atom, any monitor) bool {
   if !display || !root || !win || !atom { return false }
   if !monitor || !is_dict(monitor) {
      XDeleteProperty(display, win, atom)
      flush(display)
      return true
   }
   def index = int(monitor.get("index", -1))
   if index < 0 { return false }
   def ok = send_client_message(display, root, win, atom, index, index, index, index, 0)
   flush(display)
   ok
}

fn _set_class_hint(any display, any win, str res_name, str res_class) bool {
   if !display || !win { return false }
   def hint = XAllocClassHint()
   if !hint { return false }
   store64_h(hint, cstr(res_name), 0)
   store64_h(hint, cstr(res_class), 8)
   XSetClassHint(display, win, hint)
   XFree(hint)
   true
}

fn _reply_wm_ping(any display, any root, any event_ptr) bool {
   if !display || !root || !event_ptr { return false }
   def reply = zalloc(96)
   if !reply { return false }
   memcpy(reply, event_ptr, 96)
   store64_h(reply, root, 32)
   XSendEvent(display, root, 0,
      SubstructureNotifyMask | SubstructureRedirectMask,
   reply)
   free(reply)
   true
}

fn _set_override_redirect(any display, any win, bool enabled) bool {
   if !display || !win { return false }
   def attrs = zalloc(112)
   if !attrs { return false }
   store32(attrs, enabled ? 1 : 0, 88)
   def ok = XChangeWindowAttributes(display, win, CWOverrideRedirect, attrs) == 0
   free(attrs)
   flush(display)
   ok
}

fn _win_display_handle(any win) any {
   if !win || !is_dict(win) { return 0 }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   (display && handle) ? [display, handle] : 0
}

comptime template _x11_wrap_win_dh_xy(name, doc, call_fn){
   fn name(any win, any x, any y) any {
      doc
      def dh = _win_display_handle(win)
      dh ? call_fn(dh.get(0), dh.get(1), x, y) : false
   }
}

fn set_size(any win, int w, int h) bool {
   "Sets the X11 window size."
   def dh = _win_display_handle(win)
   dh ? set_window_size(dh.get(0), dh.get(1), w, h) : false
}

fn set_window_size(any display, any win, int width, int height, bool resizable=true, bool monitor=false) bool {
   "Set the native X11 window size."
   if !display || !win { return false }
   width = max(1, width)
   height = max(1, height)
   if !monitor {
      if !resizable { update_normal_hints(display, win, width, height, false, false) }
      resize_window(display, win, width, height)
   }
   flush(display)
   true
}

fn _update_normal_hints_for_size(
   any display, any win, bool resizable, bool monitor=false,
   int minwidth=-1, int minheight=-1, int maxwidth=-1, int maxheight=-1,
   int numer=-1, int denom=-1
) bool {
   if !display || !win { return false }
   def size = get_window_size(display, win)
   if !size || !is_dict(size) { return false }
   def ok = update_normal_hints(display, win, size.get("width", 0), size.get("height", 0),
   resizable, monitor, minwidth, minheight, maxwidth, maxheight, numer, denom)
   flush(display)
   ok
}

fn _set_window_size_limits_raw(
   any display, any win, int minwidth=-1, int minheight=-1,
   int maxwidth=-1, int maxheight=-1, bool resizable=true,
   bool monitor=false, int numer=-1, int denom=-1
) bool {
   "Set native X11 window size limits."
   _update_normal_hints_for_size(display, win, resizable, monitor,
   minwidth, minheight, maxwidth, maxheight, numer, denom)
}

fn _set_window_aspect_ratio_raw(
   any display, any win, int numer, int denom, bool resizable=true,
   bool monitor=false, int minwidth=-1, int minheight=-1,
   int maxwidth=-1, int maxheight=-1
) bool {
   "Set native X11 window aspect-ratio hints."
   _update_normal_hints_for_size(display, win, resizable, monitor,
   minwidth, minheight, maxwidth, maxheight, numer, denom)
}

fn _maximize_window_raw(any display, any root, any window_handle, any net_wm_state_atom, any max_vert_atom, any max_horz_atom) bool {
   if is_window_visible(display, window_handle) {
      def ok_visible = send_wm_state_event(display, root, window_handle, net_wm_state_atom,
      NET_WM_STATE_ADD, max_vert_atom, max_horz_atom)
      flush(display)
      return ok_visible
   }
   def ok_vert = append_atom_property(display, window_handle, net_wm_state_atom, max_vert_atom)
   def ok_horz = append_atom_property(display, window_handle, net_wm_state_atom, max_horz_atom)
   flush(display)
   ok_vert && ok_horz
}

fn maximize_window(any win) bool {
   "Maximize a native X11 window."
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   def root = win.get("root", 0)
   def net_wm_state_atom = win.get("net_wm_state", 0)
   def max_vert_atom = win.get("net_wm_state_maximized_vert", 0)
   def max_horz_atom = win.get("net_wm_state_maximized_horz", 0)
   if !display || !root || !handle || !net_wm_state_atom || !max_vert_atom || !max_horz_atom { return false }
   _maximize_window_raw(display, root, handle, net_wm_state_atom, max_vert_atom, max_horz_atom)
}

fn _restore_window_raw(any display, any root, any window_handle, any wm_state_atom, any net_wm_state_atom, any max_vert_atom=0, any max_horz_atom=0) bool {
   if wm_state_atom && is_window_iconified(display, window_handle, wm_state_atom) {
      XMapWindow(display, window_handle)
      flush(display)
      return true
   }
   if root && is_window_visible(display, window_handle) && net_wm_state_atom && max_vert_atom && max_horz_atom {
      def ok = send_wm_state_event(display, root, window_handle, net_wm_state_atom,
      NET_WM_STATE_REMOVE, max_vert_atom, max_horz_atom)
      flush(display)
      return ok
   }
   flush(display)
   true
}

fn restore_window(any win) bool {
   "Restore a native X11 window from maximized or minimized state."
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   def root = win.get("root", 0)
   def wm_state_atom = win.get("wm_state", 0)
   def net_wm_state_atom = win.get("net_wm_state", 0)
   def max_vert_atom = win.get("net_wm_state_maximized_vert", 0)
   def max_horz_atom = win.get("net_wm_state_maximized_horz", 0)
   def override_redirect = win.get("override_redirect", false)
   if !display || !handle { return false }
   if override_redirect { return false }
   _restore_window_raw(display, root, handle, wm_state_atom, net_wm_state_atom, max_vert_atom, max_horz_atom)
}

fn _set_window_floating_raw(any display, any root, any win, any net_wm_state_atom, any above_atom, bool enabled) bool {
   if !display || !win || !net_wm_state_atom || !above_atom { return false }
   if is_window_visible(display, win) {
      if !root { return false }
      def action = enabled ? NET_WM_STATE_ADD : NET_WM_STATE_REMOVE
      def ok = send_wm_state_event(display, root, win, net_wm_state_atom, action, above_atom)
      flush(display)
      return ok
   }
   if enabled {
      flush(display)
      return true
   }
   def ok_remove = remove_atom_property(display, win, net_wm_state_atom, above_atom)
   flush(display)
   ok_remove
}

fn set_window_fullscreen(any display, any root, any win, any net_wm_state_atom, any fullscreen_atom, bool enabled) bool {
   "Sets `_NET_WM_STATE_FULLSCREEN` via EWMH client messages."
   if !display || !root || !win || !net_wm_state_atom || !fullscreen_atom { return false }
   def action = enabled ? NET_WM_STATE_ADD : NET_WM_STATE_REMOVE
   def ok = send_wm_state_event(display, root, win, net_wm_state_atom, action, fullscreen_atom)
   flush(display)
   ok
}

fn _set_window_decorated_raw(any display, any win, any motif_wm_hints_atom, bool enabled) bool {
   if !display || !win || !motif_wm_hints_atom { return false }
   def hints = zalloc(40)
   if !hints { return false }
   store32(hints, MWM_HINTS_DECORATIONS, 0)
   store32(hints, enabled ? MWM_DECOR_ALL : 0, 16)
   def ok = XChangeProperty(display, win, motif_wm_hints_atom, motif_wm_hints_atom, 32,
   PropModeReplace, hints, 5) == 0
   free(hints)
   ok
}

fn set_window_resizable(any win, bool enabled) bool {
   "Toggles the resizable state of an X11 window."
   _update_window_hints(win, enabled)
}

fn _update_window_hints(
   any win, bool resizable, int min_w=-1, int min_h=-1,
   int max_w=-1, int max_h=-1, int numer=-1, int denom=-1
) bool {
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if !display || !handle { return false }
   def sz = get_size(win)
   update_normal_hints(display, handle, sz.get(0), sz.get(1), resizable, false,
   min_w, min_h, max_w, max_h, numer, denom)
   XFlush(display)
   true
}

fn set_window_decorated(any win, bool enabled) bool {
   "Toggles window decorations for an X11 window."
   def dh = _win_display_handle(win)
   if !dh { return false }
   def display = dh.get(0)
   def handle = dh.get(1)
   def motif = win.get("motif_wm_hints_atom", 0)
   if !motif { return false }
   def ok = _set_window_decorated_raw(display, handle, motif, enabled)
   XFlush(display)
   ok
}

fn set_window_floating(any win, bool enabled) bool {
   "Toggles the always-on-top state of an X11 window."
   def dh = _win_display_handle(win)
   if !dh { return false }
   def display = dh.get(0)
   def handle = dh.get(1)
   def root = win.get("root", 0)
   def state = win.get("net_wm_state_atom", 0)
   def above = win.get("net_wm_state_above_atom", 0)
   if !state || !above { return false }
   def ok = _set_window_floating_raw(display, root, handle, state, above, enabled)
   XFlush(display)
   ok
}

fn set_window_mouse_passthrough(any win, bool enabled) bool {
   "Sets mouse passthrough mode for an X11 window(click-through)."
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if !display || !handle { return false }
   if enabled {
      XShapeCombineRectangles(display, handle, 2, 0, 0, 0, 0, 0, 0)
   } else {
      XShapeCombineRectangles(display, handle, 2, 0, 0, 0, 0, 1, 0)
   }
   XFlush(display)
   win["mouse_passthrough"] = enabled
   true
}

fn set_window_size_limits(any win, int min_w, int min_h, int max_w, int max_h) bool {
   "Sets size limits for an X11 window via WM_NORMAL_HINTS."
   def dh = _win_display_handle(win)
   if !dh { return false }
   _set_window_size_limits_raw(
      dh.get(0), dh.get(1),
      min_w >= 0 ? min_w : -1, min_h >= 0 ? min_h : -1,
      max_w >= 0 ? max_w : -1, max_h >= 0 ? max_h : -1,
      true, false, -1, -1,
   )
}

fn set_window_aspect_ratio(any win, int numer, int denom) bool {
   "Sets aspect ratio for an X11 window via WM_NORMAL_HINTS."
   def dh = _win_display_handle(win)
   if !dh { return false }
   _set_window_aspect_ratio_raw(
      dh.get(0), dh.get(1),
      numer >= 0 ? numer : -1, denom >= 0 ? denom : -1,
      true, false, -1, -1, -1, -1,
   )
}

fn set_window_opacity(any win, f64 opacity) bool {
   "Sets `_NET_WM_WINDOW_OPACITY` when supported by the window manager."
   def dh = _win_display_handle(win)
   if !dh { return false }
   def display = dh.get(0)
   def handle = dh.get(1)
   def net_wm_window_opacity_atom = win.get("net_wm_window_opacity", 0)
   if !net_wm_window_opacity_atom { return false }
   if opacity < 0.0 { opacity = 0.0 }
   if opacity > 1.0 { opacity = 1.0 }
   def value = zalloc(8)
   if !value { return false }
   def scaled = int(opacity * 4294967295.0 + 0.5)
   store32(value, scaled, 0)
   def ok = XChangeProperty(display, handle, net_wm_window_opacity_atom, XA_CARDINAL, 32,
   PropModeReplace, value, 1) == 0
   free(value)
   flush(display)
   ok
}

fn get_window_opacity(any win) f64 {
   "Returns the window opacity from _NET_WM_WINDOW_OPACITY(1.0 if not set)."
   def dh = _win_display_handle(win)
   if !dh { return 1.0 }
   def display = dh.get(0)
   def handle = dh.get(1)
   def opacity_atom = win.get("net_wm_window_opacity", 0)
   if !opacity_atom { return 1.0 }
   def prop = get_window_property(display, handle, opacity_atom, XA_CARDINAL)
   if !prop || !is_dict(prop) { return 1.0 }
   def data_ptr = _prop_data_ptr(prop)
   def count = prop.get("count", 0)
   if !data_ptr || count < 1 {
      if data_ptr { XFree(data_ptr) }
      return 1.0
   }
   def raw = load32(data_ptr, 0)
   XFree(data_ptr)
   float(raw) / 4294967295.0
}

fn get_window_content_scale(any win) list {
   "Returns [xscale, yscale] from the cached scale values."
   if !win || !is_dict(win) { return [1.0, 1.0] }
   [win.get("scale_x", 1.0), win.get("scale_y", 1.0)]
}

fn get_content_scale(any win) list {
   "Alias for `get_window_content_scale`."
   get_window_content_scale(win)
}

fn _focus_window_raw(any display, any win) bool {
   XRaiseWindow(display, win)
   XSetInputFocus(display, win, RevertToParent, CurrentTime)
   flush(display)
   true
}

fn focus_window(any win) bool {
   "Raise and focus an X11 toplevel window, preferring _NET_ACTIVE_WINDOW."
   win = _ensure_input_context(win)
   def dh = _win_display_handle(win)
   if !dh { return false }
   def display = dh.get(0)
   def handle = dh.get(1)
   def ic = win.get("ic", 0)
   if ic { _c_xseticfocus(ic) }
   def root = win.get("root", 0)
   def net_active_window = win.get("net_active_window", 0)
   if net_active_window && root {
      send_client_message(display, root, handle, net_active_window, 1, 0, 0, 0, 0)
      flush(display)
      XSync(display, 0)
      if _window_focused(display, handle)== 0 && is_window_visible(display, handle) { _focus_window_raw(display, handle) }
      return true
   }
   if is_window_visible(display, handle) { _focus_window_raw(display, handle) }
   true
}

fn _set_cursor_pos_raw(any display, any win, any x, any y) bool {
   XWarpPointer(display, 0, win, 0, 0, 0, 0, int(x), int(y))
   flush(display)
   true
}

fn _alloc_query_pointer_args(bool zeroed=false) any {
   def alloc4, alloc8 = zeroed ? zalloc : malloc, zeroed ? zalloc : malloc
   def root = alloc8(8)
   def child = alloc8(8)
   def root_x = alloc4(4)
   def root_y = alloc4(4)
   def win_x = alloc4(4)
   def win_y = alloc4(4)
   def mask = alloc4(4)
   if !root || !child || !root_x || !root_y || !win_x || !win_y || !mask {
      if root { free(root) }
      if child { free(child) }
      if root_x { free(root_x) }
      if root_y { free(root_y) }
      if win_x { free(win_x) }
      if win_y { free(win_y) }
      if mask { free(mask) }
      return false
   }
   {
      "root": root, "child": child,
      "root_x": root_x, "root_y": root_y,
      "win_x": win_x, "win_y": win_y,
      "mask": mask,
   }
}

fn _free_query_pointer_args(any args) bool {
   if !is_dict(args) { return false }
   def root = args.get("root", 0)
   def child = args.get("child", 0)
   def root_x = args.get("root_x", 0)
   def root_y = args.get("root_y", 0)
   def win_x = args.get("win_x", 0)
   def win_y = args.get("win_y", 0)
   def mask = args.get("mask", 0)
   if root { free(root) }
   if child { free(child) }
   if root_x { free(root_x) }
   if root_y { free(root_y) }
   if win_x { free(win_x) }
   if win_y { free(win_y) }
   if mask { free(mask) }
   true
}

fn set_cursor_pos(dict win, any x, any y) dict {
   "Warps the cursor to window-local coordinates."
   if !win || !is_dict(win) { return win }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if !display || !handle { return win }
   def ix, iy = int(x), int(y)
   _set_cursor_pos_raw(display, handle, ix, iy)
   if win.get("cursor_mode", CURSOR_MODE_NORMAL) == CURSOR_MODE_DISABLED {
      win["warp_cursor_x"] = ix
      win["warp_cursor_y"] = iy
      win["ignore_warp_motion"] = true
      win["mouse_x"] = ix
      win["mouse_y"] = iy
   }
   win
}

comptime emit _x11_wrap_win_dh_xy(set_pos, "Moves the X11 window to [x, y].", move_window)

fn get_cursor_pos(dict win) list {
   "Queries the current pointer position relative to the X11 window."
   if !win || !is_dict(win) { return [0.0, 0.0] }
   if win.get("cursor_mode", CURSOR_MODE_NORMAL) == CURSOR_MODE_DISABLED {
      def mx, my = win.get("mouse_x", 0), win.get("mouse_y", 0)
      return [
         float(win.get("virtual_cursor_x", mx)),
         float(win.get("virtual_cursor_y", my))
      ]
   }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if !display || !handle { return [0.0, 0.0] }
   def qargs = _alloc_query_pointer_args(false)
   if !qargs { return [0.0, 0.0] }
   def root = qargs.get("root", 0)
   def child = qargs.get("child", 0)
   def root_x = qargs.get("root_x", 0)
   def root_y = qargs.get("root_y", 0)
   def win_x = qargs.get("win_x", 0)
   def win_y = qargs.get("win_y", 0)
   def mask = qargs.get("mask", 0)
   mut out = [
      float(win.get("mouse_x", 0)),
      float(win.get("mouse_y", 0))
   ]
   if XQueryPointer(display, handle, root, child, root_x, root_y, win_x, win_y, mask)!= 0 { out = [float(load32(win_x, 0)), float(load32(win_y, 0))] }
   _free_query_pointer_args(qargs)
   out
}

fn _capture_cursor(any display, any win, any cursor=0) bool {
   if !display || !win { return false }
   if common.env_truthy("NY_UI_HEADLESS") { return true }
   XGrabPointer(display, win, 1,
      ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
   GrabModeAsync, GrabModeAsync, win, cursor, CurrentTime) == GrabSuccess
}

fn _release_cursor(any display) bool {
   if !display { return false }
   XUngrabPointer(display, CurrentTime)
   flush(display)
   true
}

fn _hidden_cursor_handle(any display) any {
   if !display { return 0 }
   def key = "hidden_cursor_" + to_str(display)
   def cached = _get_x11_val(key, 0)
   if cached { return cached }
   def image = XcursorImageCreate(1, 1)
   if !image { return 0 }
   store32(image, 0, 16)
   store32(image, 0, 20)
   def pixels = load64_h(image, 32)
   if pixels { store32(pixels, 0, 0) }
   def cursor = XcursorImageLoadCursor(display, image)
   XcursorImageDestroy(image)
   if cursor { _set_x11_val(key, cursor) }
   cursor
}

fn _set_cursor_visibility(any display, any win, bool visible) bool {
   if !display || !win { return false }
   if !visible && common.env_truthy("NY_UI_HEADLESS") { return true }
   _suppress_x11_errors_temp(true)
   if visible {
      XFixesShowCursor(display, win)
   } else {
      XFixesHideCursor(display, win)
      def hidden = _hidden_cursor_handle(display)
      if hidden { XDefineCursor(display, win, hidden) }
   }
   _suppress_x11_errors_temp(false)
   flush(display)
   true
}

fn get_key_state(dict win, int key) int {
   "Returns live key state for the native X11 window, falling back to cached events."
   if !win || !is_dict(win) { return 0 }
   def display = win.get("display", 0)
   if display {
      mut code = int(win.get("x11_scancodes", dict(8)).get(key, 0))
      if code <= 0 {
         def keysym = x11_keymap.keysym_from_key(int(key))
         if keysym != 0 { code = int(XKeysymToKeycode(display, keysym)) }
      }
      if code > 0 && code < 256 {
         def keys = zalloc(32)
         if keys {
            XQueryKeymap(display, keys)
            def byte = load8(keys, int(code / 8))
            def down = band(byte, 1 << (code % 8)) != 0
            free(keys)
            return down ? 1 : 0
         }
      }
   }
   win.get("key_states", dict(8)).get(key, false) ? 1 : 0
}

fn _x11_mouse_button_mask_for_ny_button(int button) int {
   case button {
      0 -> Button1Mask
      1 -> Button3Mask
      2 -> Button2Mask
      _ -> 0
   }
}

fn get_mouse_button_state(any win, int button) int {
   "Returns the live X11 mouse button state for the cached native window."
   if !win || !is_dict(win) { return 0 }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if display && handle {
      def query_window = win.get("root", 0) ? win.get("root", 0) : handle
      def root = zalloc(8)
      def child = zalloc(8)
      def root_x = zalloc(4)
      def root_y = zalloc(4)
      def win_x = zalloc(4)
      def win_y = zalloc(4)
      def mask = zalloc(4)
      if root && child && root_x && root_y && win_x && win_y && mask {
         if XQueryPointer(display, query_window, root, child, root_x, root_y, win_x, win_y, mask) != 0 {
            def state = load32(mask, 0)
            mut mb = win.get("mouse_buttons", 0)
            if !is_dict(mb) { mb = dict(8) }
            mb[0] = band(state, Button1Mask) != 0
            mb[1] = band(state, Button3Mask) != 0
            mb[2] = band(state, Button2Mask) != 0
            win["mouse_buttons"] = mb
            free(root) free(child) free(root_x) free(root_y) free(win_x) free(win_y) free(mask)
            def btn_mask = _x11_mouse_button_mask_for_ny_button(button)
            return btn_mask ? (band(state, btn_mask) ? 1 : 0) : 0
         }
      }
      if root { free(root) }
      if child { free(child) }
      if root_x { free(root_x) }
      if root_y { free(root_y) }
      if win_x { free(win_x) }
      if win_y { free(win_y) }
      if mask { free(mask) }
   }
   win.get("mouse_buttons", dict(8)).get(button, false) ? 1 : 0
}

fn _x11_clear_mouse_buttons(any win) any {
   if !is_dict(win) { return win }
   win["mouse_buttons"] = dict(8)
   win
}

fn _clear_sticky_input_state(any state) any {
   if !is_dict(state) { state = dict(8) }
   def keys = dict_keys(state)
   mut i = 0
   while i < keys.len {
      def key = keys[i]
      if state.get(key, 0) == 3 {
         state[key] = 0
      }
      i += 1
   }
   state
}

fn _set_cursor_capture_flags(any win, bool captured, bool disabled) any {
   win["captured_cursor"] = captured
   win["disabled_cursor"] = disabled
   win["ignore_warp_motion"] = false
   win
}

fn _x11_auto_raw_lock_allowed() bool {
   if ui_profile.env_truthy_cached("NY_UI_X11_DISABLE_RAW_LOCK") {
      return false
   }
   if ui_profile.env_present_cached("NY_UI_RAW_MOUSE") && !ui_profile.env_truthy_cached("NY_UI_RAW_MOUSE") {
      return false
   }
   true
}

fn _x11_leave_disabled_raw(any win, any display, any root, bool xi_available, bool raw_mouse_motion, bool disabled_cursor) any {
   if xi_available && disabled_cursor && (raw_mouse_motion || win.get("raw_mouse_lock_auto", false)) {
      _xi_set_raw_motion_enabled(display, root, false)
   }
   if win.get("raw_mouse_lock_auto", false) {
      win["raw_mouse_motion"] = false
      win["raw_mouse_lock_auto"] = false
   }
   win["raw_motion_nonzero_seen"] = false
   win
}

fn set_input_mode(dict win, int mode, int value) dict {
   "Apply cursor and raw-mouse modes to the native X11 window state."
   if !win || !is_dict(win) { return win }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if !display || !handle { return win }
   def root = win.get("root", 0)
   def xi_available = win.get("xi_available", false)
   def raw_mouse_motion = win.get("raw_mouse_motion", false)
   def cursor_mode = win.get("cursor_mode", CURSOR_MODE_NORMAL)
   def disabled_cursor = win.get("disabled_cursor", false)
   def mouse_x = win.get("mouse_x", 0)
   def mouse_y = win.get("mouse_y", 0)
   def win_w = win.get("w", 1)
   def win_h = win.get("h", 1)
   if mode == INPUT_MODE_RAW_MOUSE {
      mut enabled = value != 0
      if enabled && !xi_available { enabled = false }
      win["raw_mouse_motion"] = enabled
      win["raw_mouse_lock_auto"] = false
      if !enabled { win["raw_motion_nonzero_seen"] = false }
      if cursor_mode == CURSOR_MODE_DISABLED && xi_available {
         if !_xi_set_raw_motion_enabled(display, root, enabled) { win["raw_mouse_motion"] = false }
      }
      _dbg_input("raw mode request=" + to_str(value) +
         " xi=" + to_str(xi_available) +
         " cursor=" + to_str(cursor_mode) +
      " enabled=" + to_str(win.get("raw_mouse_motion", false)))
      return win
   }
   if mode == INPUT_MODE_STICKY_KEYS {
      win["sticky_keys"] = value != 0
      if value == 0 { win["key_states"] = _clear_sticky_input_state(win.get("key_states", 0)) }
      return win
   }
   if mode == INPUT_MODE_STICKY_MOUSE_BUTTONS {
      win["sticky_mouse_buttons"] = value != 0
      if value == 0 { win["mouse_buttons"] = _clear_sticky_input_state(win.get("mouse_buttons", 0)) }
      return win
   }
   if mode == INPUT_MODE_LOCK_KEY_MODS {
      win["lock_key_mods"] = value != 0
      return win
   }
   if mode != INPUT_MODE_CURSOR { return win }
   def previous = cursor_mode
   if previous == CURSOR_MODE_DISABLED && value == CURSOR_MODE_DISABLED {
      if xi_available && raw_mouse_motion {
         _xi_set_raw_motion_enabled(display, root, true)
      }
      return win
   }
   if value == CURSOR_MODE_NORMAL {
      win["cursor_mode"] = value
      win = _x11_leave_disabled_raw(win, display, root, xi_available, raw_mouse_motion, disabled_cursor)
      _release_cursor(display)
      _set_cursor_visibility(display, handle, true)
      win = _apply_window_cursor(win)
      if previous == CURSOR_MODE_DISABLED {
         def rx, ry = win.get("restore_cursor_x", mouse_x), win.get("restore_cursor_y", mouse_y)
         _set_cursor_pos_raw(display, handle, rx, ry)
         win["mouse_x"] = rx
         win["mouse_y"] = ry
         win["virtual_cursor_x"] = rx
         win["virtual_cursor_y"] = ry
      }
      return _set_cursor_capture_flags(win, false, false)
   }
   if value == CURSOR_MODE_HIDDEN {
      win["cursor_mode"] = value
      win = _x11_leave_disabled_raw(win, display, root, xi_available, raw_mouse_motion, disabled_cursor)
      _release_cursor(display)
      _set_cursor_visibility(display, handle, false)
      return _set_cursor_capture_flags(win, false, false)
   }
   if value == CURSOR_MODE_CAPTURED {
      win["cursor_mode"] = value
      win = _x11_leave_disabled_raw(win, display, root, xi_available, raw_mouse_motion, disabled_cursor)
      _set_cursor_visibility(display, handle, true)
      win = _apply_window_cursor(win)
      _capture_cursor(display, handle)
      return _set_cursor_capture_flags(win, true, false)
   }
   if value == CURSOR_MODE_DISABLED {
      mut pos = get_cursor_pos(win)
      if !is_list(pos) || pos.len < 2 { pos = [float(mouse_x), float(mouse_y)] }
      win["cursor_mode"] = value
      def center_x, center_y = int(win_w / 2), int(win_h / 2)
      win["restore_cursor_x"] = int(pos[0])
      win["restore_cursor_y"] = int(pos[1])
      _set_cursor_visibility(display, handle, false)
      _capture_cursor(display, handle, _hidden_cursor_handle(display))
      def auto_raw = xi_available && !raw_mouse_motion && _x11_auto_raw_lock_allowed()
      def lock_raw = xi_available && (raw_mouse_motion || auto_raw)
      if lock_raw {
         if _xi_set_raw_motion_enabled(display, root, true) {
            win["raw_mouse_motion"] = true
            win["raw_mouse_lock_auto"] = auto_raw
         } else {
            win["raw_mouse_motion"] = false
            win["raw_mouse_lock_auto"] = false
         }
      }
      _set_cursor_pos_raw(display, handle, center_x, center_y)
      win["warp_cursor_x"] = center_x
      win["warp_cursor_y"] = center_y
      win["ignore_warp_motion"] = true
      win["raw_motion_nonzero_seen"] = false
      win["mouse_x"] = center_x
      win["mouse_y"] = center_y
      win["virtual_cursor_x"] = win.get("restore_cursor_x", center_x)
      win["virtual_cursor_y"] = win.get("restore_cursor_y", center_y)
      win["captured_cursor"] = true
      win["disabled_cursor"] = true
      _dbg_input("cursor disabled xi=" + to_str(xi_available) +
         " raw=" + to_str(win.get("raw_mouse_motion", false)) +
         " auto_raw=" + to_str(win.get("raw_mouse_lock_auto", false)) +
         " center=(" + to_str(center_x) + "," + to_str(center_y) + ")" +
         " restore=(" + to_str(win.get("restore_cursor_x", center_x)) +
      "," + to_str(win.get("restore_cursor_y", center_y)) + ")")
      return win
   }
   win
}

fn get_input_mode(any win, int mode) int {
   "Queries the current input mode for the given native X11 window."
   if !win || !is_dict(win) { return 0 }
   if mode == 0x00033005 {
      return win.get("raw_mouse_motion", false) ? 1 : 0
   }
   if mode == 0x00033001 {
      return win.get("cursor_mode", 0x00034001) ;; CURSOR_MODE_NORMAL
   }
   if mode == 0x00033002 {
      return win.get("sticky_keys", false) ? 1 : 0
   }
   if mode == 0x00033003 {
      return win.get("sticky_mouse_buttons", false) ? 1 : 0
   }
   if mode == 0x00033004 {
      return win.get("lock_key_mods", false) ? 1 : 0
   }
   0
}

fn get_key_scancode(any win, int key) int {
   "Returns the backend scancode for a logical key, or -1 when unavailable."
   if !win || !is_dict(win) { return -1 }
   int(win.get("x11_scancodes", dict(8)).get(key, -1))
}

fn _request_window_attention_raw(any display, any root, any win, any net_wm_state_atom, any demands_attention_atom) bool {
   if !display || !root || !win || !net_wm_state_atom || !demands_attention_atom { return false }
   def ok = send_wm_state_event(display, root, win, net_wm_state_atom,
   NET_WM_STATE_ADD, demands_attention_atom)
   flush(display)
   ok
}

fn request_window_attention(any win) bool {
   "Requests user attention for the window."
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def root = win.get("root", 0)
   def handle = win.get("handle", 0)
   def net_wm_state = win.get("net_wm_state", 0)
   def demands_attention = win.get("net_wm_state_demands_attention", 0)
   _request_window_attention_raw(display, root, handle, net_wm_state, demands_attention)
}

fn _get_window_frame_extents_property(any display, any win, any net_frame_extents_atom) any {
   def prop = get_window_property(display, win, net_frame_extents_atom, XA_CARDINAL)
   if !prop || !is_dict(prop) { return false }
   def data = _prop_data_ptr(prop)
   def count = prop.get("count", 0)
   if !data || count != 4 {
      if data { XFree(data) }
      return false
   }
   def out = {
      "left": load64_h(data, 0),
      "right": load64_h(data, 8),
      "top": load64_h(data, 16),
      "bottom": load64_h(data, 24),
   }
   XFree(data)
   out
}

fn _get_window_frame_extents_raw(
   any display,
   any root,
   any win,
   any net_frame_extents_atom,
   any net_request_frame_extents_atom=0,
   int timeout_ms=500
) any {
   "Compute native X11 window frame extents."
   if !display || !win || !net_frame_extents_atom { return false }
   def existing = _get_window_frame_extents_property(display, win, net_frame_extents_atom)
   if existing { return existing }
   if !is_window_visible(display, win) && root && net_request_frame_extents_atom {
      send_client_message(display, root, win, net_request_frame_extents_atom, 0, 0, 0, 0, 0)
      flush(display)
      mut waited = 0
      while waited < timeout_ms {
         def extents = _get_window_frame_extents_property(display, win, net_frame_extents_atom)
         if extents { return extents }
         wait_events(display, 1)
         waited += 1
      }
   }
   false
}

fn get_window_frame_size(any win) list {
   "Returns the window frame size [left, top, right, bottom]."
   if !win || !is_dict(win) { return [0, 0, 0, 0] }
   def display = win.get("display", 0)
   def root = win.get("root", 0)
   def handle = win.get("handle", 0)
   def net_frame_extents = win.get("net_frame_extents", 0)
   def net_request_frame_extents = win.get("net_request_frame_extents", 0)
   def extents = _get_window_frame_extents_raw(display, root, handle, net_frame_extents, net_request_frame_extents)
   if !extents { return [0, 0, 0, 0] }
   [extents.get("left", 0), extents.get("top", 0), extents.get("right", 0), extents.get("bottom", 0)]
}

fn wait_events(any display, int timeout_ms=100) bool {
   "Waits until at least one X11 event is pending or the timeout expires."
   if !display { return false }
   if timeout_ms <= 0 { return pending(display) > 0 }
   mut waited = 0
   while waited < timeout_ms {
      if pending(display) > 0 { return true }
      msleep(1)
      waited += 1
   }
   pending(display) > 0
}

fn wait_for_visibility_notify(any display, any win, int timeout_ms=100) bool {
   "Wait for a `VisibilityNotify` event for a native X11 window."
   if !display || !win { return false }
   def event_buf = malloc(256)
   if !event_buf { return false }
   mut ok = false
   mut remaining = timeout_ms
   while remaining >= 0 {
      if XCheckTypedWindowEvent(display, win, VisibilityNotify, event_buf) != 0 {
         ok = true
         break
      }
      if remaining == 0 { break }
      if wait_events(display, 1) {
      }
      remaining -= 1
   }
   free(event_buf)
   ok
}

fn _actual_event_window(any event_ptr, any typ) int {
   load32(event_ptr, 32)
}

fn _send_selection_notify(any display, any requestor, any selection, any target, any property, any time) bool {
   if !display || !requestor { return false }
   def ev = zalloc(96)
   if !ev { return false }
   store32(ev, SelectionNotify, 0)
   store64_h(ev, requestor, 32)
   store64_h(ev, selection, 40)
   store64_h(ev, target, 48)
   store64_h(ev, property, 56)
   store64_h(ev, time, 64)
   def ok = XSendEvent(display, requestor, 0, 0, ev) != 0
   free(ev)
   flush(display)
   ok
}

fn _selection_text_for_request(any win, any selection_atom) str {
   if !win || !is_dict(win) { return "" }
   if selection_atom && selection_atom == win.get("primary_atom", 0) { return win.get("primary_selection_string", "") }
   win.get("clipboard_string", "")
}

fn _set_utf8_text_property(any display, any win, any property, any utf8_string_atom, any text) bool {
   if !display || !win || !property || !utf8_string_atom { return false }
   if !is_str(text) { text = to_str(text) }
   if text.len == 0 {
      XDeleteProperty(display, win, property)
      return true
   }
   XChangeProperty(display, win, property, utf8_string_atom, 8, PropModeReplace, text, text.len)
   true
}

comptime template _x11_icon_dim_getter(name, key0, key1){
   fn ${name}(any image) int {
      if !is_dict(image) { return 0 }
      int(image.get(key0, image.get(key1, 0)))
   }
}

comptime emit _x11_icon_dim_getter(_icon_image_width, "width", "w")
comptime emit _x11_icon_dim_getter(_icon_image_height, "height", "h")

fn _icon_image_pixels(any image) any {
   if !is_dict(image) { return 0 }
   image.get("pixels_ptr",
      image.get("pixels",
   image.get("data", 0)))
}

fn _icon_pixel_source_len(any pixels) int {
   if is_str(pixels) || is_bytes(pixels) || is_list(pixels) || is_tuple(pixels) { return pixels.len }
   if is_ptr(pixels) { return -1 }
   0
}

fn _icon_pixel_byte(any pixels, int index) int {
   if is_ptr(pixels) || is_str(pixels) || is_bytes(pixels) { return load8(pixels, index) }
   if is_list(pixels) || is_tuple(pixels) { return pixels.get(index, 0) }
   0
}

fn _is_standard_cursor_shape(any shape) bool { comptime match X11StandardCursorShape(shape, false) }

fn _cursor_theme_name(any shape) str { comptime match X11CursorThemeName(shape, "") }

fn _cursor_font_shape(any shape) int { comptime match X11CursorFontShape(shape, 0) }

fn _create_native_cursor_handle(any display, any image, int xhot=0, int yhot=0) any {
   if !display { return 0 }
   def width = _icon_image_width(image)
   def height = _icon_image_height(image)
   def pixels = _icon_image_pixels(image)
   def bytes = width * height * 4
   def have = _icon_pixel_source_len(pixels)
   if width <= 0 || height <= 0 || !pixels || (have >= 0 && have < bytes) { return 0 }
   def native = XcursorImageCreate(width, height)
   if !native { return 0 }
   store32(native, int(xhot), 16)
   store32(native, int(yhot), 20)
   def target = load64_h(native, 32)
   if !target {
      XcursorImageDestroy(native)
      return 0
   }
   mut i = 0
   while i < width * height {
      def base = i * 4
      def r = _icon_pixel_byte(pixels, base + 0) & 255
      def g = _icon_pixel_byte(pixels, base + 1) & 255
      def b = _icon_pixel_byte(pixels, base + 2) & 255
      def a = _icon_pixel_byte(pixels, base + 3) & 255
      store32(target, (a << 24) | (((r * a) / 255) << 16) | (((g * a) / 255) << 8) | ((b * a) / 255), i * 4)
      i += 1
   }
   def cursor_handle = XcursorImageLoadCursor(display, native)
   XcursorImageDestroy(native)
   cursor_handle
}

fn _create_standard_cursor_handle(any display, any shape) any {
   if !display || !_is_standard_cursor_shape(shape) { return 0 }
   mut cursor_handle = 0
   def theme_name = _cursor_theme_name(shape)
   if theme_name != "" {
      def theme = XcursorGetTheme(display)
      if theme {
         def size = XcursorGetDefaultSize(display)
         def image = XcursorLibraryLoadImage(cstr(theme_name), cstr(theme), size)
         if image {
            cursor_handle = XcursorImageLoadCursor(display, image)
            XcursorImageDestroy(image)
         }
      }
   }
   if cursor_handle { return cursor_handle }
   def fallback = _cursor_font_shape(shape)
   if !fallback { return 0 }
   XCreateFontCursor(display, fallback)
}

fn create_cursor(any image, int xhot=0, int yhot=0) int {
   "Creates a backend cursor object from a Ny RGBA8 image dictionary."
   def width = _icon_image_width(image)
   def height = _icon_image_height(image)
   def pixels = _icon_image_pixels(image)
   def bytes = width * height * 4
   def have = _icon_pixel_source_len(pixels)
   if width <= 0 || height <= 0 || !pixels || (have >= 0 && have < bytes) { return 0 }
   def cursor = _next_cursor_id()
   def spec = {"kind": "image", "image": image, "xhot": int(xhot), "yhot": int(yhot), "display": 0, "handle": 0}
   _cursor_put(cursor, spec)
   cursor
}

fn create_standard_cursor(any shape) int {
   "Create a backend cursor object for a standard cursor shape."
   if !_is_standard_cursor_shape(shape) { return 0 }
   def cursor = _next_cursor_id()
   def spec = {"kind": "standard", "shape": shape, "display": 0, "handle": 0}
   _cursor_put(cursor, spec)
   cursor
}

fn _realize_cursor_handle(any display, any cursor) any {
   if !display || !is_int(cursor) || cursor <= 0 { return 0 }
   mut spec = _cursor_get(cursor)
   if !spec || !is_dict(spec) { return 0 }
   def cached_display = spec.get("display", 0)
   def cached_handle = spec.get("handle", 0)
   if cached_display == display && cached_handle { return cached_handle }
   if cached_display && cached_handle { XFreeCursor(cached_display, cached_handle) }
   mut handle = 0
   if spec.get("kind", "") == "standard" {
      handle = _create_standard_cursor_handle(display, spec.get("shape", 0))
   } else {
      handle = _create_native_cursor_handle(display,
         spec.get("image", 0),
         spec.get("xhot", 0),
      spec.get("yhot", 0))
   }
   spec["display"] = display
   spec["handle"] = handle
   _cursor_put(cursor, spec)
   handle
}

fn destroy_cursor(any cursor) bool {
   "Destroys a previously created X11 cursor object."
   if !is_int(cursor) || cursor <= 0 { return false }
   def spec = _cursor_get(cursor)
   if !spec || !is_dict(spec) { return false }
   def display = spec.get("display", 0)
   def handle = spec.get("handle", 0)
   if display && handle { XFreeCursor(display, handle) }
   _cursor_put(cursor, 0)
   true
}

fn _apply_cursor_handle(any display, any window_handle, any cursor_handle) bool {
   if !display || !window_handle { return false }
   if cursor_handle { XDefineCursor(display, window_handle, cursor_handle) }
   else { XUndefineCursor(display, window_handle) }
   flush(display)
   true
}

fn _apply_window_cursor(any win) any {
   if !win || !is_dict(win) { return win }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if !display || !handle { return win }
   def mode = win.get("cursor_mode", CURSOR_MODE_NORMAL)
   if mode != CURSOR_MODE_NORMAL && mode != CURSOR_MODE_CAPTURED { return win }
   def cursor = win.get("cursor", 0)
   def cursor_handle = _realize_cursor_handle(display, cursor)
   _apply_cursor_handle(display, handle, cursor_handle)
   win["cursor_handle"] = cursor_handle
   win
}

fn set_cursor(any win, any cursor) any {
   "Applies a cursor object to an X11 window, or clears it when cursor is zero."
   if !win || !is_dict(win) { return win }
   win["cursor"] = cursor
   _apply_window_cursor(win)
}

fn set_window_icon(dict win, any images) bool {
   "Publishes `_NET_WM_ICON` using packed ARGB32 data."
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   def net_wm_icon_atom = win.get("net_wm_icon_atom", 0)
   if !display || !handle || !net_wm_icon_atom { return false }
   if !images || !is_list(images) || images.len == 0 {
      XDeleteProperty(display, handle, net_wm_icon_atom)
      flush(display)
      return true
   }
   def images_n = images.len
   mut long_count = 0
   mut i = 0
   while i < images_n {
      def image = images[i]
      def width = _icon_image_width(image)
      def height = _icon_image_height(image)
      def pixels = _icon_image_pixels(image)
      if width <= 0 || height <= 0 || !pixels { return false }
      def need = width * height * 4
      def have = _icon_pixel_source_len(pixels)
      if have >= 0 && have < need { return false }
      long_count += 2 + width * height
      i += 1
   }
   def icon = zalloc(long_count * 8)
   if !icon { return false }
   mut offset = 0
   i = 0
   while i < images_n {
      def image = images[i]
      def width = _icon_image_width(image)
      def height = _icon_image_height(image)
      def pixels = _icon_image_pixels(image)
      store64_h(icon, width, offset * 8)
      offset += 1
      store64_h(icon, height, offset * 8)
      offset += 1
      mut p = 0
      while p < width * height {
         def base = p * 4
         def r = _icon_pixel_byte(pixels, base + 0) & 255
         def g = _icon_pixel_byte(pixels, base + 1) & 255
         def b = _icon_pixel_byte(pixels, base + 2) & 255
         def a = _icon_pixel_byte(pixels, base + 3) & 255
         store64_h(icon, (r << 16) | (g << 8) | b | (a << 24), offset * 8)
         offset += 1
         p += 1
      }
      i += 1
   }
   def ok = XChangeProperty(display, handle, net_wm_icon_atom, XA_CARDINAL, 32,
   PropModeReplace, icon, long_count) == 0
   free(icon)
   flush(display)
   ok
}

fn _xevent_client_l(any event_ptr, int index) any { load64_h(event_ptr, 56 + index * 8) }

fn _send_xdnd_status(any display, any source, any target, bool accept, any action_copy_atom=0) bool {
   _send_xdnd_client_message(display, source, target.get("xdnd_status", 0),
   target.get("handle", 0), accept ? 1 : 0, 0, 0, accept ? action_copy_atom : 0)
}

fn _send_xdnd_finished(any display, any source, any target, bool accepted, any action_copy_atom=0) bool {
   _send_xdnd_client_message(display, source, target.get("xdnd_finished", 0),
   target.get("handle", 0), accepted ? 1 : 0, accepted ? action_copy_atom : 0)
}

fn _send_xdnd_client_message(any display, any source, any message, any window_handle, any l0=0, any l1=0, any l2=0, any l3=0) bool {
   if !display || !source || !message || !window_handle { return false }
   def ev = zalloc(96)
   if !ev { return false }
   store32(ev, ClientMessage, 0)
   store64_h(ev, source, 32)
   store64_h(ev, message, 40)
   store32(ev, 32, 48)
   store64_h(ev, window_handle, 56)
   store64_h(ev, l0, 64)
   store64_h(ev, l1, 72)
   store64_h(ev, l2, 80)
   store64_h(ev, l3, 88)
   def ok = XSendEvent(display, source, 0, NoEventMask, ev) != 0
   free(ev)
   flush(display)
   ok
}

fn _clear_xdnd_state(any win) any {
   if !win || !is_dict(win) { return win }
   win["xdnd_source"] = 0
   win["xdnd_version"] = 0
   win["xdnd_format"] = 0
   win
}

fn _translate_root_to_window(any display, any root, any win, int xabs, int yabs) list {
   def out_x, out_y = malloc(4), malloc(4)
   def child = malloc(8)
   if !out_x || !out_y || !child {
      if out_x { free(out_x) }
      if out_y { free(out_y) }
      if child { free(child) }
      return [0, 0]
   }
   XTranslateCoordinates(display, root, win, xabs, yabs, out_x, out_y, child)
   def res = [load32(out_x, 0), load32(out_y, 0)]
   free(out_x, out_y, child)
   res
}

fn _xdnd_pick_format(any display, any source, any offered_list, any text_uri_atom, any xdnd_type_list_atom) any {
   if !display || !source || !text_uri_atom { return 0 }
   if is_list(offered_list) {
      def offered_n = offered_list.len
      mut i = 0
      while i < offered_n {
         if offered_list[i] == text_uri_atom { return text_uri_atom }
         i += 1
      }
   }
   if !xdnd_type_list_atom { return 0 }
   def prop = get_window_property(display, source, xdnd_type_list_atom, XA_ATOM)
   if !prop || !is_dict(prop) { return 0 }
   def data = _prop_data_ptr(prop)
   def count = prop.get("count", 0)
   mut chosen = 0
   if data {
      mut i = 0
      while i < count {
         if load64_h(data, i * 8) == text_uri_atom {
            chosen = text_uri_atom
            break
         }
         i += 1
      }
      XFree(data)
   }
   chosen
}

fn _compute_scale(int width, int height, int mm_width, int mm_height) list {
   mut sx, sy = 1.0, 1.0
   if width > 0 && mm_width > 0 { sx = (float(width) * 25.4) / (float(mm_width) * 96.0) }
   if height > 0 && mm_height > 0 { sy = (float(height) * 25.4) / (float(mm_height) * 96.0) }
   [sx, sy]
}

fn _resolve_monitor_context(any display=0, any root=0) any {
   mut owned = false
   mut resolved = display
   mut screen = 0
   if !resolved {
      resolved = open_display()
      if !resolved { return false }
      owned = true
   }
   screen = default_screen(resolved)
   if !root { root = root_window(resolved, screen) }
   def ctx = {"display": resolved, "root": root, "screen": screen, "owned": owned}
   ctx
}

fn _release_monitor_context(any ctx) bool {
   if !ctx || !is_dict(ctx) { return false }
   if ctx.get("owned", false) { close_display(ctx.get("display", 0)) }
   true
}

fn _split_bpp(int depth) list {
   match depth {
      30 -> { return [10, 10, 10] }
      24 -> { return [8, 8, 8] }
      16 -> { return [5, 6, 5] }
      15 -> { return [5, 5, 5] }
      _ -> {
         def c = max(1, int(depth / 3))
         return [c, c, c]
      }
   }
}

fn _get_mode_info(any resources, any mode_id) any {
   if !resources || !mode_id { return 0 }
   def count = load32(resources, 48)
   def modes = load64_h(resources, 56)
   mut i = 0
   while i < count {
      def mode_ptr = modes + i * 80
      if load64_h(mode_ptr, 0) == mode_id { return mode_ptr }
      i += 1
   }
   0
}

fn _refresh_from_mode_info(any mode_ptr) int {
   if !mode_ptr { return 0 }
   def dot_clock = load64_h(mode_ptr, 16)
   def h_total = load32(mode_ptr, 32)
   def v_total = load32(mode_ptr, 48)
   if !dot_clock || !h_total || !v_total { return 0 }
   int(float(dot_clock) / (float(h_total) * float(v_total)) + 0.5)
}

fn _mode_size_from_info(any mode_ptr, int rotation=RR_Rotate_0) list {
   if !mode_ptr { return [0, 0] }
   mut width = load32(mode_ptr, 8)
   mut height = load32(mode_ptr, 12)
   if rotation == RR_Rotate_90 || rotation == RR_Rotate_270 {
      def tmp = width
      width = height
      height = tmp
   }
   [width, height]
}

fn _monitor_from_output(any display, any resources, any output, any primary_output=0) any {
   if !display || !resources || !output { return false }
   def info = XRRGetOutputInfo(display, resources, output)
   if !info { return false }
   def connection = load16(info, 48)
   def crtc = load64_h(info, 8)
   if connection != RR_Connected || !crtc {
      XRRFreeOutputInfo(info)
      return false
   }
   def crtc_info = XRRGetCrtcInfo(display, resources, crtc)
   if !crtc_info {
      XRRFreeOutputInfo(info)
      return false
   }
   def rotation = load16(crtc_info, 32)
   def mode_id = load64_h(crtc_info, 24)
   def mode_ptr = _get_mode_info(resources, mode_id)
   def size = _mode_size_from_info(mode_ptr, rotation)
   mut width = size.get(0, load32(crtc_info, 16))
   mut height = size.get(1, load32(crtc_info, 20))
   if width <= 0 { width = load32(crtc_info, 16) }
   if height <= 0 { height = load32(crtc_info, 20) }
   mut width_mm = load32(info, 32)
   mut height_mm = load32(info, 40)
   if rotation == RR_Rotate_90 || rotation == RR_Rotate_270 {
      def tmp_mm = width_mm
      width_mm = height_mm
      height_mm = tmp_mm
   }
   if width_mm <= 0 || height_mm <= 0 {
      width_mm = int(float(width) * 25.4 / 96.0 + 0.5)
      height_mm = int(float(height) * 25.4 / 96.0 + 0.5)
   }
   def scale = _compute_scale(width, height, width_mm, height_mm)
   def rgb = _split_bpp(default_depth(display, default_screen(display)))
   def monitor = {
      "output": output, "crtc": crtc, "primary": output == primary_output, "name": str.cstr_to_str(load64_h(info, 16)),
      "x": load32(crtc_info, 8), "y": load32(crtc_info, 12), "width": width, "height": height,
      "width_mm": width_mm, "height_mm": height_mm, "scale_x": scale.get(0, 1.0), "scale_y": scale.get(1, 1.0),
      "refresh_rate": _refresh_from_mode_info(mode_ptr), "mode_id": mode_id, "rotation": rotation,
      "red_bits": rgb.get(0, 8), "green_bits": rgb.get(1, 8), "blue_bits": rgb.get(2, 8)
   }
   XRRFreeCrtcInfo(crtc_info)
   XRRFreeOutputInfo(info)
   monitor
}

fn get_monitors(any display=0, any root=0) list {
   "Enumerates connected X11/RandR monitors as monitor dicts."
   def ctx = _resolve_monitor_context(display, root)
   if !ctx { return [] }
   display, root = ctx.get("display", 0), ctx.get("root", 0)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if !resources {
      _release_monitor_context(ctx)
      return []
   }
   def count = load32(resources, 32)
   def outputs = load64_h(resources, 40)
   def primary = XRRGetOutputPrimary(display, root)
   mut primary_list = []
   mut others = []
   mut i = 0
   while i < count {
      def monitor = _monitor_from_output(display, resources, load64_h(outputs, i * 8), primary)
      if monitor {
         if monitor.get("primary", false) { primary_list = primary_list.append(monitor) }
         else { others = others.append(monitor) }
      }
      i += 1
   }
   XRRFreeScreenResources(resources)
   _release_monitor_context(ctx)
   mut out = []
   i = 0
   while i < primary_list.len {
      def monitor = primary_list[i]
      if monitor && is_dict(monitor) {
         monitor["index"] = out.len
         out = out.append(monitor)
      }
      i += 1
   }
   i = 0
   while i < others.len {
      def monitor = others[i]
      if monitor && is_dict(monitor) {
         monitor["index"] = out.len
         out = out.append(monitor)
      }
      i += 1
   }
   out
}

fn get_primary_monitor(any display=0, any root=0) any {
   "Returns the primary monitor dict, or false if none are connected."
   def monitors = get_monitors(display, root)
   if monitors.len == 0 { return false }
   monitors[0]
}

comptime template _x11_monitor_get2(name, doc, k0, d0, k1, d1){
   fn ${name}(any monitor) list {
      doc
      if !monitor || !is_dict(monitor) { return [d0, d1] }
      [monitor.get(k0, d0), monitor.get(k1, d1)]
   }
}

comptime template _x11_monitor_get1(name, doc, key, defv){
   fn ${name}(any monitor) any {
      doc
      if !monitor || !is_dict(monitor) { return defv }
      monitor.get(key, defv)
   }
}

def _X11_MONITOR_SCALE_DEF = 1.0
comptime emit _x11_monitor_get2(get_monitor_pos, "Returns `[x, y]` for a monitor dict.", "x", 0, "y", 0)
comptime emit _x11_monitor_get2(get_monitor_physical_size, "Returns `[width_mm, height_mm]` for a monitor dict.", "width_mm", 0, "height_mm", 0)
comptime emit _x11_monitor_get2(get_monitor_content_scale, "Returns `[xscale, yscale]` for a monitor dict.", "scale_x", _X11_MONITOR_SCALE_DEF, "scale_y", _X11_MONITOR_SCALE_DEF)
comptime emit _x11_monitor_get1(get_monitor_name, "Returns the UTF-8 display name for a monitor dict.", "name", "")

fn get_monitor_workarea(dict monitor, any display=0, any root=0, any net_workarea_atom=0, any net_current_desktop_atom=0) list {
   "Returns `[x, y, width, height]` clipped to `_NET_WORKAREA` when available."
   if !monitor || !is_dict(monitor) { return [0, 0, 0, 0] }
   def ctx = _resolve_monitor_context(display, root)
   if !ctx {
      return [
         monitor.get("x", 0),
         monitor.get("y", 0),
         monitor.get("width", 0),
         monitor.get("height", 0)
      ]
   }
   display, root = ctx.get("display", 0), ctx.get("root", 0)
   if !net_workarea_atom { net_workarea_atom = intern_atom(display, "_NET_WORKAREA") }
   if !net_current_desktop_atom { net_current_desktop_atom = intern_atom(display, "_NET_CURRENT_DESKTOP") }
   mut area_x, area_y = monitor.get("x", 0), monitor.get("y", 0)
   mut area_w, area_h = monitor.get("width", 0), monitor.get("height", 0)
   if net_workarea_atom && net_current_desktop_atom {
      def extents = get_window_property(display, root, net_workarea_atom, XA_CARDINAL)
      def desktop = get_window_property(display, root, net_current_desktop_atom, XA_CARDINAL)
      if extents && desktop {
         def extent_data = _prop_data_ptr(extents)
         def extent_count = extents.get("count", 0)
         def desk_data = _prop_data_ptr(desktop)
         if extent_data && desk_data && extent_count >= 4 {
            def index = int(load64_h(desk_data, 0))
            if index >= 0 && index < int(extent_count / 4) {
               def base = index * 32
               def global_x = load64_h(extent_data, base + 0)
               def global_y = load64_h(extent_data, base + 8)
               def global_w = load64_h(extent_data, base + 16)
               def global_h = load64_h(extent_data, base + 24)
               if area_x < global_x {
                  area_w -= global_x - area_x
                  area_x = global_x
               }
               if area_y < global_y {
                  area_h -= global_y - area_y
                  area_y = global_y
               }
               if area_x + area_w > global_x + global_w { area_w = global_x - area_x + global_w }
               if area_y + area_h > global_y + global_h { area_h = global_y - area_y + global_h }
            }
         }
         def extent_ptr = _prop_data_ptr(extents)
         def desk_ptr = _prop_data_ptr(desktop)
         if extent_ptr { XFree(extent_ptr) }
         if desk_ptr { XFree(desk_ptr) }
      }
   }
   _release_monitor_context(ctx)
   [area_x, area_y, area_w, area_h]
}

fn get_video_mode(any monitor, any display=0, any root=0) any {
   "Returns the current window video mode dict for a monitor."
   if !monitor || !is_dict(monitor) { return false }
   def ctx = _resolve_monitor_context(display, root)
   if !ctx { return false }
   display, root = ctx.get("display", 0), ctx.get("root", 0)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if !resources {
      _release_monitor_context(ctx)
      return false
   }
   def mode_ptr = _get_mode_info(resources, monitor.get("mode_id", 0))
   if !mode_ptr {
      XRRFreeScreenResources(resources)
      _release_monitor_context(ctx)
      return false
   }
   def size = _mode_size_from_info(mode_ptr, monitor.get("rotation", RR_Rotate_0))
   def out = {
      "width": size.get(0, monitor.get("width", 0)), "height": size.get(1, monitor.get("height", 0)),
      "red_bits": monitor.get("red_bits", 8), "green_bits": monitor.get("green_bits", 8),
      "blue_bits": monitor.get("blue_bits", 8), "refresh_rate": _refresh_from_mode_info(mode_ptr)
   }
   XRRFreeScreenResources(resources)
   _release_monitor_context(ctx)
   out
}

fn get_video_modes(any monitor, any display=0, any root=0) list {
   "Returns distinct window video modes for a monitor."
   if !monitor || !is_dict(monitor) { return [] }
   def ctx = _resolve_monitor_context(display, root)
   if !ctx { return [] }
   display, root = ctx.get("display", 0), ctx.get("root", 0)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if !resources {
      _release_monitor_context(ctx)
      return []
   }
   def info = XRRGetOutputInfo(display, resources, monitor.get("output", 0))
   def crtc_info = XRRGetCrtcInfo(display, resources, monitor.get("crtc", 0))
   if !info || !crtc_info {
      if info { XRRFreeOutputInfo(info) }
      if crtc_info { XRRFreeCrtcInfo(crtc_info) }
      XRRFreeScreenResources(resources)
      _release_monitor_context(ctx)
      return []
   }
   def mode_ids = load64_h(info, 88)
   def count = load32(info, 80)
   def rotation = load16(crtc_info, 32)
   mut modes = []
   mut seen = dict(8)
   mut i = 0
   while i < count {
      def mode_ptr = _get_mode_info(resources, load64_h(mode_ids, i * 8))
      if mode_ptr && !band(load64_h(mode_ptr, 72), RR_Interlace) {
         def size = _mode_size_from_info(mode_ptr, rotation)
         def refresh = _refresh_from_mode_info(mode_ptr)
         def key_str = to_str(size.get(0, 0)) + "x" + to_str(size.get(1, 0)) + "@" + to_str(refresh)
         if !seen.get(key_str, false) {
            seen[key_str] = true
            def mode = {
               "width": size.get(0, 0), "height": size.get(1, 0),
               "red_bits": monitor.get("red_bits", 8), "green_bits": monitor.get("green_bits", 8),
               "blue_bits": monitor.get("blue_bits", 8), "refresh_rate": refresh
            }
            modes = modes.append(mode)
         }
      }
      i += 1
   }
   XRRFreeOutputInfo(info)
   XRRFreeCrtcInfo(crtc_info)
   XRRFreeScreenResources(resources)
   _release_monitor_context(ctx)
   modes
}

fn _choose_video_mode(any resources, any output_info, int rotation, int width, int height, int refresh_rate=0) any {
   if !resources || !output_info { return 0 }
   def count = load32(output_info, 80)
   def mode_ids = load64_h(output_info, 88)
   mut best_mode = 0
   mut best_score = 1 << 30
   mut i = 0
   while i < count {
      def mode_ptr = _get_mode_info(resources, load64_h(mode_ids, i * 8))
      if mode_ptr && !band(load64_h(mode_ptr, 72), RR_Interlace) {
         def size = _mode_size_from_info(mode_ptr, rotation)
         def mw = size.get(0, 0)
         def mh = size.get(1, 0)
         def refresh = _refresh_from_mode_info(mode_ptr)
         mut score = abs(mw - width) * 10000 + abs(mh - height) * 100 + abs(refresh - max(0, refresh_rate))
         if refresh_rate <= 0 { score = abs(mw - width) * 10000 + abs(mh - height) * 100 }
         if score < best_score {
            best_score = score
            best_mode = load64_h(mode_ptr, 0)
         }
      }
      i += 1
   }
   best_mode
}

fn get_window_monitor(any win) any {
   "Returns the monitor dict currently associated with a native X11 window."
   if !win || !is_dict(win) { return false }
   win.get("monitor", false)
}

fn set_window_monitor(dict win, any monitor, int xpos, int ypos, int width, int height, int refresh_rate=0) dict {
   "Switch a native X11 window between fullscreen and windowed monitor modes."
   if !win || !is_dict(win) { return win }
   def display = win.get("display", 0)
   def root = win.get("root", 0)
   def handle = win.get("handle", 0)
   if !display || !root || !handle { return win }
   def ewmh_fullscreen = win.get("net_wm_state", 0) && win.get("net_wm_state_fullscreen", 0)
   def was_monitor = win.get("monitor", false)
   if monitor {
      if !was_monitor {
         win["windowed_x"] = win.get("x", xpos)
         win["windowed_y"] = win.get("y", ypos)
         win["windowed_w"] = win.get("w", width)
         win["windowed_h"] = win.get("h", height)
      }
      def resources = XRRGetScreenResourcesCurrent(display, root)
      if resources {
         def crtc = monitor.get("crtc", 0)
         def output = monitor.get("output", 0)
         def crtc_info = XRRGetCrtcInfo(display, resources, crtc)
         def output_info = XRRGetOutputInfo(display, resources, output)
         if crtc_info && output_info {
            def current_mode = load64_h(crtc_info, 24)
            def rotation = load16(crtc_info, 32)
            def chosen_mode = _choose_video_mode(resources, output_info, rotation, width, height, refresh_rate)
            if chosen_mode && chosen_mode != current_mode {
               XRRSetCrtcConfig(display, resources, crtc, CurrentTime,
                  load32(crtc_info, 8), load32(crtc_info, 12), chosen_mode,
               rotation, load64(crtc_info, 40), load32(crtc_info, 36))
               win["monitor_old_mode"] = current_mode
            }
            XRRFreeOutputInfo(output_info)
            XRRFreeCrtcInfo(crtc_info)
         } else {
            if output_info { XRRFreeOutputInfo(output_info) }
            if crtc_info { XRRFreeCrtcInfo(crtc_info) }
         }
         XRRFreeScreenResources(resources)
      }
      if !is_window_visible(display, handle) {
         XMapRaised(display, handle)
         flush(display)
         wait_for_visibility_notify(display, handle, 100)
      }
      update_normal_hints(display, handle, width, height, win.get("resizable", true), true)
      _set_fullscreen_monitors(display, root, handle,
      win.get("net_wm_fullscreen_monitors", 0), monitor)
      if ewmh_fullscreen {
         set_window_fullscreen(display, root, handle,
            win.get("net_wm_state", 0),
         win.get("net_wm_state_fullscreen", 0), true)
      } else {
         _set_override_redirect(display, handle, true)
      }
      if win.get("net_wm_bypass_compositor", 0) &&
      !band(int(win.get("flags", 0)), WINDOW_TRANSPARENT){
         _set_compositor_bypass(display, handle, win.get("net_wm_bypass_compositor", 0), true)
      }
      move_window(display, handle, monitor.get("x", xpos), monitor.get("y", ypos))
      resize_window(display, handle, width, height)
      flush(display)
      win["monitor"] = monitor
      win["fullscreen"] = true
      win["x"] = monitor.get("x", xpos)
      win["y"] = monitor.get("y", ypos)
      win["w"] = width
      win["h"] = height
      win["override_redirect"] = !ewmh_fullscreen
      return _sync_window_state(win)
   }
   def previous = win.get("monitor", false)
   if previous {
      def resources = XRRGetScreenResourcesCurrent(display, root)
      if resources {
         def crtc = previous.get("crtc", 0)
         def crtc_info = XRRGetCrtcInfo(display, resources, crtc)
         def old_mode = win.get("monitor_old_mode", 0)
         if crtc_info && old_mode {
            XRRSetCrtcConfig(display, resources, crtc, CurrentTime,
               load32(crtc_info, 8), load32(crtc_info, 12), old_mode,
            load16(crtc_info, 32), load64(crtc_info, 40), load32(crtc_info, 36))
         }
         if crtc_info { XRRFreeCrtcInfo(crtc_info) }
         XRRFreeScreenResources(resources)
      }
   }
   _set_fullscreen_monitors(display, root, handle,
   win.get("net_wm_fullscreen_monitors", 0), false)
   if ewmh_fullscreen {
      set_window_fullscreen(display, root, handle,
         win.get("net_wm_state", 0),
      win.get("net_wm_state_fullscreen", 0), false)
   } else {
      _set_override_redirect(display, handle, false)
   }
   if win.get("net_wm_bypass_compositor", 0) { _set_compositor_bypass(display, handle, win.get("net_wm_bypass_compositor", 0), false) }
   update_normal_hints(display, handle, width, height, win.get("resizable", true), false)
   move_window(display, handle, xpos, ypos)
   resize_window(display, handle, width, height)
   flush(display)
   win["monitor"] = false
   win["fullscreen"] = false
   win["x"] = xpos
   win["y"] = ypos
   win["w"] = width
   win["h"] = height
   win["monitor_old_mode"] = 0
   win["override_redirect"] = false
   _sync_window_state(win)
}

fn x11_error_handler(any display, ptr error_ptr) int {
   "Installs as Xlib error callback; stores latest error details in platform state."
   if !error_ptr { return 0 }
   def error_code = load8(error_ptr, 32)
   def request_code = load8(error_ptr, 33)
   def minor_code = load8(error_ptr, 34)
   def resource_id = load64_h(error_ptr, 16)
   _set_x11_val("error_code", error_code)
   _set_x11_val("error_request", request_code)
   _set_x11_val("error_minor", minor_code)
   _set_x11_val("error_resource", resource_id)
   if error_code != 11 && error_code != 17 { return 0 }
   def suppress = _get_x11_val("suppress_errors", 0)
   if suppress != 0 { return 0 }
   if _is_debug() {
      ui_profile.print_text("[x11:ERROR] X11 error occurred:")
      ui_profile.print_text("[x11:ERROR]   error_code: " + to_str(error_code))
      ui_profile.print_text("[x11:ERROR]   request_code: " + to_str(request_code))
      ui_profile.print_text("[x11:ERROR]   minor_code: " + to_str(minor_code))
      ui_profile.print_text("[x11:ERROR]   resource_id: 0x" + str.to_hex(resource_id))
   }
   0
}

fn _suppress_x11_errors_temp(any suppress) any { _set_x11_val("suppress_errors", suppress ? 1 : 0) }

fn _setup_x11_error_handler() bool {
   #linux {
      _c_xset_error_handler(x11_error_handler)
   } #endif
   true
}

fn _xrandr_connected_outputs(any display, any root) dict {
   if !display || !root { return dict(8) }
   def prev_handler = _c_xset_error_handler(x11_error_handler)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if !resources { _c_xset_error_handler(prev_handler) return dict(8) }
   mut outputs = dict(8)
   def count = load32(resources, 32)
   def output_ptr = load64_h(resources, 40)
   mut i = 0
   while i < count {
      def output = load64_h(output_ptr, i * 8)
      def info = XRRGetOutputInfo(display, resources, output)
      if info {
         def connection = load16(info, 48)
         def crtc = load64_h(info, 8)
         if connection == RR_Connected && crtc { outputs[output] = true }
         XRRFreeOutputInfo(info)
      }
      i += 1
   }
   XRRFreeScreenResources(resources)
   XSync(display, 0)
   _c_xset_error_handler(prev_handler)
   outputs
}

fn _xrandr_event_scale(any event_ptr) list {
   def width = load32(event_ptr, 72)
   def height = load32(event_ptr, 76)
   def mm_width = load32(event_ptr, 80)
   def mm_height = load32(event_ptr, 84)
   _compute_scale(width, height, mm_width, mm_height)
}

fn _push_randr_output_diff(list events, any win, dict before_outputs, dict after_outputs) list {
   mut keys = dict_keys(after_outputs)
   mut keys_n = keys.len
   mut i = 0
   while i < keys_n {
      def output = keys[i]
      if !before_outputs.get(output, false) {
         def data = {"output": output}
         events = _push_translated_event(events, 22, win, data)
      }
      i += 1
   }
   keys = dict_keys(before_outputs)
   keys_n = keys.len
   i = 0
   while i < keys_n {
      def output = keys[i]
      if !after_outputs.get(output, false) {
         def data = {"output": output}
         events = _push_translated_event(events, 23, win, data)
      }
      i += 1
   }
   events
}

fn _xrandr_poll_outputs(any win, list events) any {
   def display = win.get("display", 0)
   def root = win.get("root", 0)
   def before_outputs = win.get("randr_outputs", dict(8))
   def after_outputs = _xrandr_connected_outputs(display, root)
   win["randr_outputs"] = after_outputs
   _push_randr_output_diff(events, win, before_outputs, after_outputs)
   win
}

fn _write_selection_to_property(any display,
   any requestor,
   any property,
   any target,
   any text,
   any utf8_string_atom,
   any targets_atom,
   any multiple_atom=0,
   any atom_pair_atom=0,
   any save_targets_atom=0) any {
   "Writes clipboard data for a SelectionRequest and returns the reply property."
   if !display || !requestor || !property || !target { return NoAtom }
   if !is_str(text) { text = to_str(text) }
   if target == targets_atom {
      def target_count = multiple_atom ? 4 : 3
      def targets = zalloc(target_count * 8)
      if !targets { return NoAtom }
      store64_h(targets, targets_atom, 0)
      if multiple_atom { store64_h(targets, multiple_atom, 8) }
      store64_h(targets, common.value_or(utf8_string_atom, XA_STRING), multiple_atom ? 16 : 8)
      store64_h(targets, XA_STRING, multiple_atom ? 24 : 16)
      XChangeProperty(display, requestor, property, XA_ATOM, 32, PropModeReplace, targets, target_count)
      free(targets)
      return property
   }
   if multiple_atom && atom_pair_atom && target == multiple_atom {
      def prop = get_window_property(display, requestor, property, atom_pair_atom)
      if !prop || !is_dict(prop) { return NoAtom }
      def pairs = _prop_data_ptr(prop)
      def count = prop.get("count", 0)
      if !pairs || count <= 0 {
         if pairs { XFree(pairs) }
         return NoAtom
      }
      mut i = 0
      while i + 1 < count {
         def pair_target = load64_h(pairs, i * 8)
         def pair_property = load64_h(pairs, (i + 1) * 8)
         if pair_target == utf8_string_atom || pair_target == XA_STRING {
            XChangeProperty(display, requestor, pair_property, pair_target, 8, PropModeReplace, text, text.len)
         } elif save_targets_atom && pair_target == save_targets_atom {
            XChangeProperty(display, requestor, pair_property, NoAtom, 32, PropModeReplace, 0, 0)
         } else {
            store64_h(pairs, NoAtom, (i + 1) * 8)
         }
         i += 2
      }
      XChangeProperty(display, requestor, property, atom_pair_atom, 32, PropModeReplace, pairs, count)
      XFree(pairs)
      return property
   }
   if save_targets_atom && target == save_targets_atom {
      XChangeProperty(display, requestor, property, NoAtom, 32, PropModeReplace, 0, 0)
      return property
   }
   if target == utf8_string_atom || target == XA_STRING {
      mut payload = text
      if target == XA_STRING {
         payload = text
      }
      XChangeProperty(display, requestor, property, target, 8, PropModeReplace, payload, payload.len)
      return property
   }
   NoAtom
}

fn _read_selection_property_text(any display, any win, any selection_property_atom, any target, any incr_atom=0, int timeout_ms=500) str {
   if !display || !win || !selection_property_atom { return "" }
   def prop = get_window_property(display, win, selection_property_atom, AnyPropertyType)
   if !prop || !is_dict(prop) { return "" }
   def data = _prop_data_ptr(prop)
   def actual_type = prop.get("type", 0)
   if !data { return "" }
   if incr_atom && actual_type == incr_atom {
      XFree(data)
      XDeleteProperty(display, win, selection_property_atom)
      flush(display)
      def event_buf = zalloc(96)
      if !event_buf { return "" }
      mut out = ""
      mut waited = 0
      while waited < timeout_ms {
         if pending(display) <= 0 {
            wait_events(display, 1)
            waited += 1
            continue
         }
         next_event(display, event_buf)
         if load32(event_buf, 0) != PropertyNotify { continue }
         if load64_h(event_buf, 32) != win
         || load64_h(event_buf, 40) != selection_property_atom
         || load32(event_buf, 56) != PropertyNewValue{
            continue
         }
         def chunk = get_window_property(display, win, selection_property_atom, AnyPropertyType)
         if !chunk || !is_dict(chunk) { continue }
         def chunk_data = _prop_data_ptr(chunk)
         def chunk_type = chunk.get("type", 0)
         def chunk_count = chunk.get("count", 0)
         if chunk_data && chunk_count > 0 {
            if chunk_type == XA_STRING {
               out = out + x11_common.convertLatin1toUTF8(chunk_data)
            } else if chunk_type == target || target == 0 {
               out = out + x11_common.dup_string(chunk_data)
            }
            XFree(chunk_data)
            XDeleteProperty(display, win, selection_property_atom)
            flush(display)
            waited = 0
            continue
         }
         if chunk_data { XFree(chunk_data) }
         break
      }
      free(event_buf)
      return out
   }
   mut text = ""
   if actual_type == XA_STRING {
      text = x11_common.convertLatin1toUTF8(data)
   } elif actual_type == target || target == 0 {
      text = x11_common.dup_string(data)
   }
   XFree(data)
   XDeleteProperty(display, win, selection_property_atom)
   flush(display)
   text
}

fn set_clipboard(any win, any text) bool {
   "Claims the X11 clipboard selection for `win` and stores `text` locally."
   if !win || !is_dict(win) { return false }
   win["clipboard_string"] = to_str(text)
   def ok = _set_selection_owner(win, "clipboard_atom")
   win["clipboard_owned"] = ok
   ok
}

fn set_primary_selection(any win, any text) bool {
   "Claims the X11 PRIMARY selection for `win` and stores `text` locally."
   if !win || !is_dict(win) { return false }
   win["primary_selection_string"] = to_str(text)
   def ok = _set_selection_owner(win, "primary_atom")
   win["primary_owned"] = ok
   ok
}

fn _set_selection_owner(any win, str selection_key) bool {
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   def selection = win.get(selection_key, 0)
   if !display || !handle || !selection { return false }
   XSetSelectionOwner(display, selection, handle, CurrentTime)
   flush(display)
   XGetSelectionOwner(display, selection) == handle
}

fn get_clipboard(any win) str {
   "Fetches clipboard text using the X11 selection conversion path."
   _get_selection_text(win, "clipboard_atom", "clipboard_string")
}

fn get_primary_selection(any win) str {
   "Fetches PRIMARY selection text using the X11 selection conversion path."
   _get_selection_text(win, "primary_atom", "primary_selection_string")
}

fn _get_selection_text(any win, str selection_key, str local_key) str {
   if !win || !is_dict(win) { return "" }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   def selection = win.get(selection_key, 0)
   def utf8_string_atom = win.get("utf8_string", 0)
   def selection_property_atom = win.get("selection_property", 0)
   def local_text = win.get(local_key, "")
   def timeout_ms = 500
   def incr_atom = win.get("incr_atom", 0)
   if !display || !handle || !selection || !selection_property_atom { return "" }
   if XGetSelectionOwner(display, selection) == handle { return local_text }
   mut i = 0
   mut target = utf8_string_atom
   while i < 2 {
      if i == 1 { target = XA_STRING }
      XConvertSelection(display, selection, target, selection_property_atom, handle, CurrentTime)
      flush(display)
      def notification = zalloc(96)
      if !notification { return "" }
      mut waited = 0
      mut got = false
      while waited < timeout_ms {
         if XCheckTypedWindowEvent(display, handle, SelectionNotify, notification) != 0 {
            got = true
            break
         }
         wait_events(display, 1)
         waited += 1
      }
      if got {
         def property = load64_h(notification, 56)
         free(notification)
         if property != NoAtom {
            def text = _read_selection_property_text(display,
               handle,
               selection_property_atom,
               target,
               incr_atom,
            timeout_ms)
            if text { return text }
         }
      } else {
         free(notification)
      }
      i += 1
   }
   ""
}

fn destroy_basic_window(any win) bool {
   "Destroys an X11 window and its associated resources."
   close_basic_window(win)
}

fn set_title(any win, str title) bool {
   "Unified setter for the X11 window title."
   if !win || !is_dict(win) { return false }
   store_name(win.get("display", 0), win.get("handle", 0), title,
      win.get("net_wm_name", 0),
      win.get("net_wm_icon_name", 0),
   win.get("utf8_string", 0))
}

fn get_window_attrib(any win, int attrib) int {
   "Unified getter for X11 window attributes matching Nytrix constants."
   if !win || !is_dict(win) { return 0 }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   mut result = 0
   match attrib {
      RESIZABLE -> { result = win.get("resizable", true) ? 1 : 0 }
      VISIBLE -> { result = is_window_visible(display, handle) ? 1 : 0 }
      DECORATED -> { result = win.get("decorated", true) ? 1 : 0 }
      FOCUSED -> { result = _window_focused(display, handle) }
      ICONIFIED -> { result = is_window_iconified(display, handle, win.get("wm_state", 0)) ? 1 : 0 }
      MAXIMIZED -> {
         result = is_window_maximized(display, handle,
            win.get("net_wm_state", 0),
            win.get("net_wm_state_maximized_vert", 0),
         win.get("net_wm_state_maximized_horz", 0)) ? 1 : 0
      }
      TRANSPARENT_FRAMEBUFFER -> { result = win.get("transparent", false) ? 1 : 0 }
      FLOATING -> {
         result = is_window_floating(display, handle,
            win.get("net_wm_state", 0),
         win.get("net_wm_state_above", 0)) ? 1 : 0
      }
      HOVERED -> {
         result = _window_hovered(display, handle) ? 1 : 0
      }
      MOUSE_PASSTHROUGH -> { result = win.get("mouse_passthrough", false) ? 1 : 0 }
      AUTO_ICONIFY -> { result = win.get("auto_iconify", true) ? 1 : 0 }
      FOCUS_ON_SHOW -> { result = win.get("focus_on_show", true) ? 1 : 0 }
      _ -> { result = 0 }
   }
   result
}

fn _window_hovered(any display, any window_handle) int {
   if !display || !window_handle { return 0 }
   def qargs = _alloc_query_pointer_args(true)
   if !qargs { return 0 }
   def root = qargs.get("root", 0)
   def child = qargs.get("child", 0)
   def root_x = qargs.get("root_x", 0)
   def root_y = qargs.get("root_y", 0)
   def child_x = qargs.get("win_x", 0)
   def child_y = qargs.get("win_y", 0)
   def mask = qargs.get("mask", 0)
   def root_handle = root_window(display, default_screen(display))
   mut probe = root_handle
   mut hovered = 0
   while probe {
      def ok = XQueryPointer(display, probe, root, child, root_x, root_y, child_x, child_y, mask)
      if !ok {
         hovered = 0
         break
      }
      probe = load64_h(child, 0)
      if probe == window_handle {
         hovered = 1
         break
      }
   }
   _free_query_pointer_args(qargs)
   hovered
}

fn _window_focused(any display, any window_handle) int {
   if !display || !window_handle { return 0 }
   def focused = zalloc(8)
   def state = zalloc(4)
   if !focused || !state {
      if focused { free(focused) }
      if state { free(state) }
      return 0
   }
   XGetInputFocus(display, focused, state)
   def out = load64_h(focused, 0) == window_handle ? 1 : 0
   free(focused, state)
   out
}

fn _sync_window_state(any win) any {
   if !win || !is_dict(win) { return win }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if !display || !handle { return win }
   def size = get_window_size(display, handle)
   if size && is_dict(size) {
      win["w"] = size.get("width", win.get("w", 0))
      win["h"] = size.get("height", win.get("h", 0))
   }
   def pos = get_pos(win)
   win["x"] = pos.get(0, win.get("x", 0))
   win["y"] = pos.get(1, win.get("y", 0))
   def visible = is_window_visible(display, handle)
   win["visible"] = visible
   win["mapped"] = visible
   win["focused"] = _window_focused(display, handle) != 0
   win
}

fn _push_selection_to_manager(any win, int timeout_ms=250) bool {
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   def clipboard_manager = win.get("clipboard_manager", 0)
   def save_targets = win.get("save_targets", 0)
   if !display || !handle || !clipboard_manager || !save_targets { return false }
   XConvertSelection(display, clipboard_manager, save_targets, NoAtom, handle, CurrentTime)
   flush(display)
   def event_buf = zalloc(192)
   if !event_buf { return false }
   mut waited = 0
   while waited < timeout_ms {
      while pending(display) > 0 {
         next_event(display, event_buf)
         def typ = load32(event_buf, 0)
         if typ == SelectionRequest {
            def requestor = load64_h(event_buf, 40)
            def selection = load64_h(event_buf, 48)
            def target = load64_h(event_buf, 56)
            def property = load64_h(event_buf, 64)
            def time = load64_h(event_buf, 72)
            def reply_property = _write_selection_to_property(display, requestor, property, target,
               _selection_text_for_request(win, selection),
               win.get("utf8_string", 0),
               win.get("targets_atom", 0),
               win.get("multiple_atom", 0),
               win.get("atom_pair_atom", 0),
            save_targets)
            _send_selection_notify(display, requestor, selection, target, reply_property, time)
         } elif typ == SelectionNotify && load64_h(event_buf, 56) == save_targets {
            free(event_buf)
            return true
         }
      }
      wait_events(display, 1)
      waited += 1
   }
   free(event_buf)
   false
}

fn _push_translated_event(list events, int typ, any win, any data=0) list {
   def h = win.get("handle", 0)
   def e = ui_event.make_event(typ, win, h, data)
   events.append(e)
}

fn _xkb_setup(any display) dict {
   mut out = {"available": false, "event_base": -1, "error_base": -1, "group": 0, "detectable_repeat": false}
   if !display { return out }
   def opcode = malloc(4)
   def event_base = malloc(4)
   def error_base = malloc(4)
   def major = malloc(4)
   def minor = malloc(4)
   def detectable = malloc(4)
   if !opcode || !event_base || !error_base || !major || !minor || !detectable {
      if opcode { free(opcode) }
      if event_base { free(event_base) }
      if error_base { free(error_base) }
      if major { free(major) }
      if minor { free(minor) }
      if detectable { free(detectable) }
      return out
   }
   store32(major, 1, 0)
   store32(minor, 0, 0)
   def available = XkbQueryExtension(display, opcode, event_base, error_base, major, minor) != 0
   mut repeat_ok = false
   if available {
      store32(detectable, 0, 0)
      repeat_ok = XkbSetDetectableAutoRepeat(display, 1, detectable) != 0 && load32(detectable, 0) != 0
      out = {
         "available": true,
         "event_base": load32(event_base, 0),
         "error_base": load32(error_base, 0),
         "group": 0,
         "detectable_repeat": repeat_ok,
      }
   }
   free(opcode, event_base, error_base, major, minor, detectable)
   out
}

fn _xi_setup(any display) dict {
   mut out = {
      "available": false, "major_opcode": -1, "event_base": -1,
      "error_base": -1, "major": XI_2_Major, "minor": XI_2_Minor
   }
   if !display { return out }
   def major_opcode = malloc(4)
   def event_base = malloc(4)
   def error_base = malloc(4)
   def major = malloc(4)
   def minor = malloc(4)
   if !major_opcode || !event_base || !error_base || !major || !minor {
      if major_opcode { free(major_opcode) }
      if event_base { free(event_base) }
      if error_base { free(error_base) }
      if major { free(major) }
      if minor { free(minor) }
      return out
   }
   store32(major, XI_2_Major, 0)
   store32(minor, XI_2_Minor, 0)
   mut available = XQueryExtension(display, cstr("XInputExtension"), major_opcode, event_base, error_base) != 0
   if available {
      available = XIQueryVersion(display, major, minor) == Success
   }
   out = {
      "available": available, "major_opcode": available ? load32(major_opcode, 0) : -1,
      "event_base": available ? load32(event_base, 0) : -1, "error_base": available ? load32(error_base, 0) : -1,
      "major": available ? load32(major, 0) : XI_2_Major, "minor": available ? load32(minor, 0) : XI_2_Minor
   }
   free(major_opcode, event_base, error_base, major, minor)
   out
}

fn _xi_set_raw_motion_enabled(any display, any root, bool enabled) bool {
   if !display || !root { return false }
   def mask_len = (XI_RawMotion >> 3) + 1
   def mask = zalloc(mask_len)
   if !mask { return false }
   if enabled {
      store8(mask, XI_RawMotion >> 3, bor(load8(mask, XI_RawMotion >> 3), 1 << band(XI_RawMotion, 7)))
   }
   def evmask = zalloc(16)
   if !evmask {
      free(mask)
      return false
   }
   store32(evmask, XIAllMasterDevices, 0)
   store32(evmask, mask_len, 4)
   store64_h(evmask, mask, 8)
   def ok = XISelectEvents(display, root, evmask, 1) == Success
   flush(display)
   free(evmask)
   free(mask)
   ok
}

fn _ximask_is_set(any mask, int bit) bool {
   if !mask || bit < 0 { return false }
   band(load8(mask, bit >> 3), (1 << band(bit, 7))) != 0
}

fn _translate_raw_motion_event(any win, any event_ptr, list events) list {
   if !win || !is_dict(win) || !event_ptr { return [win, events] }
   def display = win.get("display", 0)
   if !display { return [win, events] }
   def cookie = event_ptr
   if XGetEventData(display, cookie) == 0 { return [win, events] }
   def evtype = load32(cookie, 36)
   def data = load64_h(cookie, 48)
   if evtype != XI_RawMotion || !data {
      XFreeEventData(display, cookie)
      return [win, events]
   }
   def deviceid = load32(data, 48)
   def sourceid = load32(data, 52)
   def mask_len = load32(data, 64)
   def mask = load64_h(data, 72)
   def raw_values = load64_h(data, 88)
   mut dx, dy = 0.0, 0.0
   mut debug_vals = ""
   if mask_len > 0 && mask && raw_values {
      mut value_idx = 0
      mut axis = 0
      while axis < mask_len * 8 {
         if _ximask_is_set(mask, axis) {
            def v = load64_f64(raw_values, value_idx * 8)
            if debug_vals.len < 192 {
               if debug_vals.len > 0 { debug_vals = debug_vals + " " }
               debug_vals = debug_vals + to_str(axis) + ":" + to_str(v)
            }
            if axis == 0 { dx = v }
            elif axis == 1 { dy = v }
            value_idx += 1
         }
         axis += 1
      }
   }
   XFreeEventData(display, cookie)
   def raw_nonzero = dx != 0.0 || dy != 0.0
   def raw_dbg_count = int(win.get("raw_motion_debug_count", 0))
   def raw_zero_dbg_count = int(win.get("raw_motion_zero_debug_count", 0))
   if raw_nonzero && raw_dbg_count < 16 {
      win["raw_motion_debug_count"] = raw_dbg_count + 1
      _dbg_input("raw motion dev=" + to_str(deviceid) +
         " src=" + to_str(sourceid) +
         " mask_len=" + to_str(mask_len) +
         " vals=[" + debug_vals + "]" +
         " dx=" + to_str(dx) +
      " dy=" + to_str(dy))
   } elif !raw_nonzero && raw_zero_dbg_count < 4 {
      win["raw_motion_zero_debug_count"] = raw_zero_dbg_count + 1
      _dbg_input("raw motion zero dev=" + to_str(deviceid) +
         " src=" + to_str(sourceid) +
         " mask_len=" + to_str(mask_len) +
      " vals=[" + debug_vals + "]")
   }
   if raw_nonzero {
      win["raw_motion_nonzero_seen"] = true
      def last_x = float(win.get("virtual_cursor_x", win.get("mouse_x", 0)))
      def last_y = float(win.get("virtual_cursor_y", win.get("mouse_y", 0)))
      def next_x = last_x + dx
      def next_y = last_y + dy
      win["virtual_cursor_x"] = next_x
      win["virtual_cursor_y"] = next_y
      win["mouse_x"] = next_x
      win["mouse_y"] = next_y
      def data_ev = {"x": next_x, "y": next_y, "dx": dx, "dy": dy, "moved": true, "relative": true, "raw": true, "mod": win.get("modifiers", 0)}
      data_ev["mods"] = win.get("modifiers", 0)
      events = _push_translated_event(events, 7, win, data_ev)
   }
   [win, events]
}

fn _x11_button_to_ny(int button) int {
   match button {
      Button1 -> { return 0 }
      Button2 -> { return 2 }
      Button3 -> { return 1 }
      _ -> {
         if button > Button7 { return button - Button1 - 4 }
         return button
      }
   }
}

fn _store_key_table_pair(dict keycodes, dict scancodes, int scancode, int key) list {
   if scancode <= 0 || key < 0 { return [keycodes, scancodes] }
   keycodes[scancode] = key
   if key > 0 && !scancodes.contains(key) { scancodes[key] = scancode }
   [keycodes, scancodes]
}

fn _key_table_result(dict keycodes, dict scancodes) dict {
   def out = {"keycodes": keycodes, "scancodes": scancodes}
   out
}

fn _create_key_tables(any display) dict {
   mut keycodes = dict(256)
   mut scancodes = dict(256)
   if !display { return _key_table_result(keycodes, scancodes) }
   def min_ptr = malloc(4)
   def max_ptr = malloc(4)
   def width_ptr = malloc(4)
   if !min_ptr || !max_ptr || !width_ptr {
      if min_ptr { free(min_ptr) }
      if max_ptr { free(max_ptr) }
      if width_ptr { free(width_ptr) }
      return _key_table_result(keycodes, scancodes)
   }
   XDisplayKeycodes(display, min_ptr, max_ptr)
   def scancode_min = load32(min_ptr, 0)
   def scancode_max = load32(max_ptr, 0)
   free(min_ptr, max_ptr)
   if scancode_min <= 0 || scancode_max < scancode_min {
      free(width_ptr)
      return _key_table_result(keycodes, scancodes)
   }
   def keysyms = XGetKeyboardMapping(display,
      scancode_min,
      scancode_max - scancode_min + 1,
   width_ptr)
   def width = load32(width_ptr, 0)
   free(width_ptr)
   mut scancode = scancode_min
   while scancode <= scancode_max {
      mut key = translate_scancode(scancode)
      if keysyms && width > 0 {
         def base = (scancode - scancode_min) * width * 8
         def primary = load64_h(keysyms, base)
         def secondary = width > 1 ? load64_h(keysyms, base + 8) : 0
         def translated = translate_keysym(primary, secondary, width > 1 ? 2 : 1)
         if key < 0 && translated >= 0 { key = translated }
      }
      def stored = _store_key_table_pair(keycodes, scancodes, scancode, key)
      keycodes = stored[0]
      scancodes = stored[1]
      scancode += 1
   }
   if keysyms { XFree(keysyms) }
   _key_table_result(keycodes, scancodes)
}

fn _translate_keycode(any win, int keycode) int {
   if keycode <= 0 || keycode > 255 { return -1 }
   if win && is_dict(win) {
      def cached_keycodes = win.get("x11_keycodes", 0)
      if is_dict(cached_keycodes) && cached_keycodes.contains(keycode) {
         def cached = cached_keycodes.get(keycode, -1)
         if cached >= 0 { return cached }
      }
   }
   def scancode_key = translate_scancode(keycode)
   if scancode_key >= 0 { return scancode_key }
   if !win || !is_dict(win) { return scancode_key }
   def display = win.get("display", _get_x11_val("display", 0))
   if !display { return scancode_key }
   def group = win.get("xkb_group", _get_x11_val("xkb_group", 0))
   def primary = _c_xkb_keycode_to_keysym(display, int(keycode), int(group), 0)
   def secondary = _c_xkb_keycode_to_keysym(display, int(keycode), int(group), 1)
   def width = (secondary != 0 && secondary != primary) ? 2 : 1
   def translated = x11_keymap.translate_keysym(primary, secondary, width)
   if translated >= 0 { return translated }
   scancode_key
}

fn _modifier_bit_for_key_event(int key, int scancode) int {
   case key {
      KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT -> { return MOD_SHIFT }
      KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL -> { return MOD_CONTROL }
      KEY_LEFT_ALT, KEY_RIGHT_ALT -> { return MOD_ALT }
      KEY_LEFT_SUPER, KEY_RIGHT_SUPER -> { return MOD_SUPER }
      _ -> {}
   }
   case int(scancode){
      50, 62 -> { return MOD_SHIFT }
      37, 105 -> { return MOD_CONTROL }
      64, 108 -> { return MOD_ALT }
      133, 134 -> { return MOD_SUPER }
      _ -> {}
   }
   0
}

fn _mods_for_key_event(int mods, int key, int scancode, bool down) int {
   def bit = _modifier_bit_for_key_event(key, scancode)
   if bit == 0 { return mods }
   down ? bor(mods, bit) : band(mods, bnot(bit))
}

fn _translate_key_press_event(any win, any event_ptr, list events, any key_states, any key_press_times, any ic, bool was_filtered, int repeat_sc) list {
   def scancode = load32(event_ptr, 84)
   def key = _translate_keycode(win, scancode)
   def mods = _mods_for_key_event(translate_state(load32(event_ptr, 80)), key, scancode, true)
   def plain = !band(mods, bor(MOD_CONTROL, MOD_ALT))
   def timestamp = load64_h(event_ptr, 56)
   mut _ks = key_states
   if !is_dict(_ks) { _ks = dict(64) }
   _ks[key] = true
   def is_repeat = repeat_sc == scancode
   if is_repeat { win["repeat_scancode"] = -1 }
   def data = {
      "raw_key": scancode,
      "key": key,
      "scancode": scancode,
      "action": is_repeat ? ACTION_REPEAT : ACTION_PRESS,
      "mod": mods,
      "mods": mods,
   }
   win["key_states"] = _ks
   win["modifiers"] = mods
   if _input_debug_enabled() {
      _dbg_input("key press key=" + _dbg_key_name(win, key, scancode) +
         " scancode=" + to_str(scancode) +
         " mods=0x" + str.to_hex(mods) +
      " repeat=" + to_str(is_repeat))
   }
   events = _push_translated_event(events, 1, win, data)
   if ic && !was_filtered {
      events = _emit_ic_chars(events, win, event_ptr, mods, plain)
   } elif !ic {
      def keysym_ptr = zalloc(8)
      if keysym_ptr {
         XLookupString(event_ptr, 0, 0, keysym_ptr, 0)
         def codepoint = x11_keymap.keysym_to_unicode(load64_h(keysym_ptr, 0))
         if codepoint > 31 && codepoint != 127 && codepoint != INVALID_CODEPOINT {
            def char_data = {"char": codepoint, "mod": mods, "mods": mods, "plain": plain}
            _dbg_input("char codepoint=" + to_str(codepoint) + " plain=" + to_str(plain))
            events = _push_translated_event(events, 3, win, char_data)
         }
         free(keysym_ptr)
      }
   }
   mut _kpt = key_press_times
   if !is_dict(_kpt) { _kpt = dict(64) }
   _kpt[scancode] = timestamp
   win["key_press_times"] = _kpt
   [win, events]
}

fn _translate_key_release_event(any win, any event_ptr, list events, any key_states, any display) list {
   def scancode = load32(event_ptr, 84)
   def time = load64_h(event_ptr, 56)
   def key = _translate_keycode(win, scancode)
   def mods = _mods_for_key_event(translate_state(load32(event_ptr, 80)), key, scancode, false)
   mut is_repeat = false
   def QueuedAfterReading = 2
   if XEventsQueued(display, QueuedAfterReading) > 0 {
      def peek_buf = zalloc(192)
      if peek_buf {
         XPeekEvent(display, peek_buf)
         def peek_typ = load32(peek_buf, 0)
         def peek_sc = load32(peek_buf, 84)
         def peek_time = load64_h(peek_buf, 56)
         if peek_typ == KeyPress && peek_sc == scancode && peek_time == time {
            is_repeat = true
            win["repeat_scancode"] = scancode
         }
         free(peek_buf)
      }
   }
   if !is_repeat {
      mut _ks = key_states
      if !is_dict(_ks) { _ks = dict(64) }
      _ks[key] = false
      def data = {"raw_key": scancode, "key": key, "scancode": scancode, "action": 0, "mod": mods, "mods": mods}
      win["key_states"] = _ks
      win["modifiers"] = mods
      if _input_debug_enabled() {
         _dbg_input("key release key=" + _dbg_key_name(win, key, scancode) +
            " scancode=" + to_str(scancode) +
         " mods=0x" + str.to_hex(mods))
      }
      events = _push_translated_event(events, 2, win, data)
   }
   [win, events]
}

fn _translate_button_press_event(any win, any event_ptr, list events, any mouse_buttons) list {
   def button = load32(event_ptr, 84)
   def raw_state = load32(event_ptr, 80)
   def mods = translate_state(raw_state)
   if button == Button4 || button == Button5 || button == Button6 || button == Button7 {
      def x, y = load32(event_ptr, 64), load32(event_ptr, 68)
      mut sdx, sdy = 0.0, 0.0
      if button == Button4 { sdy = 1.0 }
      elif button == Button5 { sdy = -1.0 }
      elif button == Button6 { sdx = -1.0 }
      else { sdx = 1.0 }
      def data = {"dx": sdx, "dy": sdy, "x": x, "y": y, "scrolling": true, "mod": mods, "mods": mods}
      win["modifiers"] = mods
      _dbg_input("mouse scroll dx=" + to_str(data.get("dx", 0.0)) +
         " dy=" + to_str(data.get("dy", 0.0)) +
      " mods=0x" + str.to_hex(mods))
      events = _push_translated_event(events, 6, win, data)
   } else {
      def x, y = load32(event_ptr, 64), load32(event_ptr, 68)
      mut _mb = mouse_buttons
      if !is_dict(_mb) { _mb = dict(8) }
      def ny_button = _x11_button_to_ny(button)
      _mb[ny_button] = true
      def data = {
         "button": ny_button, "x": x, "y": y, "mod": mods, "mods": mods,
         "left_down": ny_button == 0 || band(raw_state, Button1Mask) != 0,
         "middle_down": ny_button == 2 || band(raw_state, Button2Mask) != 0,
         "right_down": ny_button == 1 || band(raw_state, Button3Mask) != 0,
      }
      win["mouse_buttons"] = _mb
      win["modifiers"] = mods
      win["mouse_x"] = x
      win["mouse_y"] = y
      _dbg_input("mouse press button=" + to_str(ny_button) +
         " x=" + to_str(x) + " y=" + to_str(y) +
      " mods=0x" + str.to_hex(mods))
      events = _push_translated_event(events, 4, win, data)
   }
   [win, events]
}

fn _translate_button_release_event(any win, any event_ptr, list events, any mouse_buttons) list {
   def button = load32(event_ptr, 84)
   if button <= Button3 || button > Button7 {
      def x, y = load32(event_ptr, 64), load32(event_ptr, 68)
      def raw_state = load32(event_ptr, 80)
      def mods = translate_state(raw_state)
      mut _mb = mouse_buttons
      if !is_dict(_mb) { _mb = dict(8) }
      def ny_button = _x11_button_to_ny(button)
      _mb[ny_button] = false
      def data = {
         "button": ny_button, "x": x, "y": y, "mod": mods, "mods": mods,
         "left_down": ny_button != 0 && band(raw_state, Button1Mask) != 0,
         "middle_down": ny_button != 2 && band(raw_state, Button2Mask) != 0,
         "right_down": ny_button != 1 && band(raw_state, Button3Mask) != 0,
      }
      win["mouse_buttons"] = _mb
      win["modifiers"] = mods
      win["mouse_x"] = x
      win["mouse_y"] = y
      _dbg_input("mouse release button=" + to_str(ny_button) +
         " x=" + to_str(x) + " y=" + to_str(y) +
      " mods=0x" + str.to_hex(mods))
      events = _push_translated_event(events, 5, win, data)
   }
   [win, events]
}

fn _x11_sync_mouse_buttons_from_state(any win, int state) any {
   if !is_dict(win) { return win }
   mut mb = win.get("mouse_buttons", 0)
   if !is_dict(mb) { mb = dict(8) }
   mb[0] = band(state, Button1Mask) != 0
   mb[1] = band(state, Button3Mask) != 0
   mb[2] = band(state, Button2Mask) != 0
   win["mouse_buttons"] = mb
   win
}

fn _translate_motion_event(
   any win, any event_ptr, list events, int cursor_mode, bool xi_available, bool raw_mouse_motion,
   bool ignore_warp, int warp_x, int warp_y, int win_w, int win_h, any mouse_x_prev,
   any mouse_y_prev, any display, any window_handle,
) list {
   def x, y = load32(event_ptr, 64), load32(event_ptr, 68)
   if cursor_mode == CURSOR_MODE_DISABLED {
      if xi_available && raw_mouse_motion && win.get("raw_motion_nonzero_seen", false) {
         if ignore_warp {
            win["ignore_warp_motion"] = false
            win["mouse_x"] = float(warp_x)
            win["mouse_y"] = float(warp_y)
         }
         return [win, events]
      }
      if ignore_warp {
         win["ignore_warp_motion"] = false
         win["mouse_x"] = float(warp_x)
         win["mouse_y"] = float(warp_y)
         return [win, events]
      }
      def center_x, center_y = int(win_w / 2), int(win_h / 2)
      def dx, dy = x - center_x, y - center_y
      if abs(float(dx)) <= 1.0 && abs(float(dy)) <= 1.0 {
         win["mouse_x"] = float(center_x)
         win["mouse_y"] = float(center_y)
         win["warp_cursor_x"] = center_x
         win["warp_cursor_y"] = center_y
         if dx != 0 || dy != 0 {
            win["ignore_warp_motion"] = true
            _set_cursor_pos_raw(display, window_handle, center_x, center_y)
         }
         return [win, events]
      }
      if dx != 0 || dy != 0 {
         def fb_dbg_count = int(win.get("motion_fallback_debug_count", 0))
         if fb_dbg_count < 8 {
            win["motion_fallback_debug_count"] = fb_dbg_count + 1
            _dbg_input("motion fallback xi=" + to_str(xi_available) +
               " raw=" + to_str(raw_mouse_motion) +
               " ignore=" + to_str(ignore_warp) +
               " pos=(" + to_str(x) + "," + to_str(y) + ")" +
               " center=(" + to_str(center_x) + "," + to_str(center_y) + ")" +
               " delta=(" + to_str(dx) + "," + to_str(dy) + ")" +
            " size=(" + to_str(win_w) + "," + to_str(win_h) + ")")
         }
         def virt_x = float(win.get("virtual_cursor_x", center_x)) + float(dx)
         def virt_y = float(win.get("virtual_cursor_y", center_y)) + float(dy)
         def raw_state = load32(event_ptr, 80)
         win = _x11_sync_mouse_buttons_from_state(win, raw_state)
         def mods = translate_state(raw_state)
         def data = {
            "x": virt_x, "y": virt_y, "dx": dx, "dy": dy, "moved": true, "relative": true, "raw": false, "mod": mods, "mods": mods,
            "left_down": band(raw_state, Button1Mask) != 0,
            "middle_down": band(raw_state, Button2Mask) != 0,
            "right_down": band(raw_state, Button3Mask) != 0,
         }
         win["modifiers"] = mods
         win["virtual_cursor_x"] = virt_x
         win["virtual_cursor_y"] = virt_y
         win["mouse_x"] = float(center_x)
         win["mouse_y"] = float(center_y)
         win["warp_cursor_x"] = center_x
         win["warp_cursor_y"] = center_y
         win["ignore_warp_motion"] = true
         _set_cursor_pos_raw(display, window_handle, center_x, center_y)
         events = _push_translated_event(events, 7, win, data)
      }
      return [win, events]
   }
   def raw_state = load32(event_ptr, 80)
   win = _x11_sync_mouse_buttons_from_state(win, raw_state)
   def mods = translate_state(raw_state)
   win["modifiers"] = mods
   def prev_x = int(mouse_x_prev)
   def prev_y = int(mouse_y_prev)
   if x == prev_x && y == prev_y {
      return [win, events]
   }
   win["mouse_x"] = float(x)
   win["mouse_y"] = float(y)
   def data = {
      "x": float(x),
      "y": float(y),
      "dx": float(x - prev_x),
      "dy": float(y - prev_y),
      "moved": true,
      "relative": false,
      "raw": false,
      "mod": mods,
      "mods": mods,
      "left_down": band(raw_state, Button1Mask) != 0,
      "middle_down": band(raw_state, Button2Mask) != 0,
      "right_down": band(raw_state, Button3Mask) != 0,
   }
   events = _push_translated_event(events, 7, win, data)
   [win, events]
}

fn _translate_configure_event(any win, any event_ptr, list events, any display, any window_handle, any root, int win_w, int win_h) list {
   def width = load32(event_ptr, 56)
   def height = load32(event_ptr, 60)
   mut xpos, ypos = load32(event_ptr, 48), load32(event_ptr, 52)
   def parent = win.get("parent", root)
   if parent && root && parent != root {
      def out_x, out_y = zalloc(4), zalloc(4)
      def child = zalloc(8)
      if out_x && out_y && child { if XTranslateCoordinates(display, parent, root, xpos, ypos, out_x, out_y, child)!= 0 { xpos, ypos = load32(out_x, 0), load32(out_y, 0) } }
      if out_x { free(out_x) }
      if out_y { free(out_y) }
      if child { free(child) }
   }
   if ui_profile.debug_verbose_enabled() { _dbg("ConfigureNotify: win=0x" + str.to_hex(window_handle)) }
   if width != win_w || height != win_h {
      win["w"] = width
      win["h"] = height
      def data = {"w": width, "h": height}
      events = _push_translated_event(events, 9, win, data)
   }
   def win_x_prev, win_y_prev = win.get("x", xpos), win.get("y", ypos)
   if xpos != win_x_prev || ypos != win_y_prev {
      win["x"] = xpos
      win["y"] = ypos
      def data = {"x": xpos, "y": ypos}
      events = _push_translated_event(events, 8, win, data)
   }
   [win, events]
}

fn _translate_client_message_event(
   any win, any event_ptr, list events, any display, any window_handle, any root,
   any wm_protocols, any wm_delete, any net_wm_ping, any xdnd_enter_at, any xdnd_position_at, any xdnd_drop_at,
) list {
   def message_type = load64_h(event_ptr, 40)
   def protocol = load64_h(event_ptr, 56)
   if message_type == wm_protocols && protocol == wm_delete {
      win["should_close"] = true
      events = _push_translated_event(events, 15, win, 0)
      return [win, events]
   }
   if message_type == wm_protocols && protocol == net_wm_ping {
      _reply_wm_ping(display, root, event_ptr)
      return [win, events]
   }
   if message_type == xdnd_enter_at {
      def source = _xevent_client_l(event_ptr, 0)
      def version = bshr(_xevent_client_l(event_ptr, 1), 24)
      def use_list = band(_xevent_client_l(event_ptr, 1), 1) != 0
      if version > XDND_VERSION {
         win = _clear_xdnd_state(win)
         return [win, events]
      }
      mut offered = []
      if !use_list {
         offered = offered.append(_xevent_client_l(event_ptr, 2))
         offered = offered.append(_xevent_client_l(event_ptr, 3))
         offered = offered.append(_xevent_client_l(event_ptr, 4))
      }
      def text_uri_list = win.get("text_uri_list", 0)
      def xdnd_type_list = use_list ? win.get("xdnd_type_list", 0) : 0
      def format = _xdnd_pick_format(display, source, offered, text_uri_list, xdnd_type_list)
      win["xdnd_source"] = source
      win["xdnd_version"] = version
      win["xdnd_format"] = format
      return [win, events]
   }
   if message_type == xdnd_position_at {
      def source = _xevent_client_l(event_ptr, 0)
      def xdnd_src = win.get("xdnd_source", 0)
      if source == xdnd_src {
         def xdnd_ver = win.get("xdnd_version", 0)
         if xdnd_ver > XDND_VERSION { return [win, events] }
         def packed = _xevent_client_l(event_ptr, 2)
         def xabs = band(bshr(packed, 16), 0xffff)
         def yabs = band(packed, 0xffff)
         def pos = _translate_root_to_window(display, root, window_handle, xabs, yabs)
         def x = pos.get(0, 0)
         def y = pos.get(1, 0)
         def xdnd_fmt = win.get("xdnd_format", 0)
         win["mouse_x"] = x
         win["mouse_y"] = y
         def data = {"x": x, "y": y, "accept": !!xdnd_fmt, "format": xdnd_fmt}
         events = _push_translated_event(events, 17, win, data)
         def xdnd_action_copy = win.get("xdnd_action_copy", 0)
         _send_xdnd_status(display, source, win, !!xdnd_fmt, xdnd_ver >= 2 ? xdnd_action_copy : 0)
      }
      return [win, events]
   }
   if message_type == xdnd_drop_at {
      def source = _xevent_client_l(event_ptr, 0)
      def xdnd_src = win.get("xdnd_source", 0)
      if source == xdnd_src {
         def xdnd_ver = win.get("xdnd_version", 0)
         if xdnd_ver > XDND_VERSION { return [win, events] }
         def xdnd_fmt = win.get("xdnd_format", 0)
         if xdnd_fmt {
            mut time = CurrentTime
            if xdnd_ver >= 1 { time = _xevent_client_l(event_ptr, 2) }
            def xdnd_sel = win.get("xdnd_selection", 0)
            XConvertSelection(display, xdnd_sel, xdnd_fmt, xdnd_sel, window_handle, time)
            flush(display)
         } else {
            if xdnd_ver >= 2 { _send_xdnd_finished(display, source, win, false, 0) }
            win = _clear_xdnd_state(win)
         }
      }
   }
   [win, events]
}

fn _translate_selection_notify_event(any win, any event_ptr, list events, any display, any window_handle) list {
   def xdnd_selection = win.get("xdnd_selection", 0)
   if load64_h(event_ptr, 56) != xdnd_selection { return [win, events] }
   def property = load64_h(event_ptr, 56)
   def target = load64_h(event_ptr, 48)
   mut accepted = false
   if property {
      def prop = get_window_property(display, window_handle, property, common.value_or(target, AnyPropertyType))
      if prop && is_dict(prop) {
         def data_ptr = _prop_data_ptr(prop)
         if data_ptr {
            def raw = x11_common.dup_string(data_ptr)
            def paths = x11_common.parseUriList(raw)
            def data = {"paths": paths}
            events = _push_translated_event(events, 16, win, data)
            XFree(data_ptr)
            accepted = is_list(paths) && paths.len > 0
         }
      }
      XDeleteProperty(display, window_handle, property)
      flush(display)
   }
   def xdnd_ver = win.get("xdnd_version", 0)
   if xdnd_ver >= 2 {
      def xdnd_src = win.get("xdnd_source", 0)
      def xdnd_action_copy = win.get("xdnd_action_copy", 0)
      _send_xdnd_finished(display, xdnd_src, win, accepted, xdnd_action_copy)
   }
   win = _clear_xdnd_state(win)
   [win, events]
}

fn _translate_focus_in_event(any win, any event_ptr, list events, any ic, int cursor_mode, bool raw_mouse_motion) list {
   def mode = load32(event_ptr, 40)
   if mode == NotifyGrab || mode == NotifyUngrab { return [win, events] }
   mut active_ic = ic
   if !active_ic {
      win = _ensure_input_context(win)
      active_ic = is_dict(win) ? win.get("ic", 0) : 0
   }
   if active_ic { _c_xseticfocus(active_ic) }
   def raw_win = set_input_mode(win, INPUT_MODE_RAW_MOUSE, raw_mouse_motion ? 1 : 0)
   if is_dict(raw_win) { win = raw_win }
   def cursor_win = set_input_mode(win, INPUT_MODE_CURSOR, cursor_mode)
   if is_dict(cursor_win) { win = cursor_win }
   if !is_dict(win) { return [win, events] }
   win["focused"] = true
   events = _push_translated_event(events, 10, win, 0)
   [win, events]
}

fn _translate_focus_out_event(
   any win, any event_ptr, list events, any display, any window_handle, any root, any ic, int cursor_mode,
   bool disabled_cursor, bool xi_available, bool raw_mouse_motion,
) list {
   def mode = load32(event_ptr, 40)
   if mode == NotifyGrab || mode == NotifyUngrab { return [win, events] }
   if ic { _c_xunseticfocus(ic) }
   if xi_available && raw_mouse_motion { _xi_set_raw_motion_enabled(display, root, false) }
   def captured_cursor = win.get("captured_cursor", false)
   if cursor_mode != CURSOR_MODE_NORMAL || captured_cursor || disabled_cursor {
      _release_cursor(display)
      _set_cursor_visibility(display, window_handle, true)
      win = _apply_window_cursor(win)
      win["captured_cursor"] = false
      win["disabled_cursor"] = false
      win["ignore_warp_motion"] = false
   }
   def monitor = win.get("monitor", false)
   def auto_iconify = win.get("auto_iconify", true)
   if monitor && auto_iconify {
      def screen = win.get("screen", 0)
      _iconify_window_raw(display, window_handle, screen)
   }
   win["key_states"] = dict(64)
   win["scancode_states"] = dict(64)
   win["mouse_buttons"] = dict(8)
   win["modifiers"] = 0
   win["repeat_scancode"] = -1
   win["focused"] = false
   events = _push_translated_event(events, 11, win, 0)
   [win, events]
}

fn _translate_property_notify_event(any win, any event_ptr, list events, any display, any window_handle) list {
   if load32(event_ptr, 56) != PropertyNewValue { return [win, events] }
   def atom = load64_h(event_ptr, 40)
   def wm_state = win.get("wm_state", 0)
   if atom == wm_state {
      def state = get_window_state(display, window_handle, atom)
      if state == IconicState || state == NormalState {
         def iconified = state == IconicState
         def was_iconified = win.get("iconified", false)
         if was_iconified != iconified {
            win["iconified"] = iconified
            events = _push_translated_event(
               events, iconified ? EVENT_WINDOW_MINIMIZED : EVENT_WINDOW_RESTORED, win, 0,
            )
         }
      }
      return [win, events]
   }
   def net_wm_state = win.get("net_wm_state", 0)
   if atom != net_wm_state { return [win, events] }
   def net_wm_state_maximized_vert = win.get("net_wm_state_maximized_vert", 0)
   def net_wm_state_maximized_horz = win.get("net_wm_state_maximized_horz", 0)
   def maximized = is_window_maximized(display, window_handle, atom, net_wm_state_maximized_vert, net_wm_state_maximized_horz)
   def was_maximized = win.get("maximized", false)
   if was_maximized != maximized {
      win["maximized"] = maximized
      events = _push_translated_event(
         events, maximized ? EVENT_WINDOW_MAXIMIZED : EVENT_WINDOW_RESTORED, win, 0,
      )
   }
   [win, events]
}

fn _translate_enter_event(any win, any event_ptr, list events) list {
   def x, y = load32(event_ptr, 64), load32(event_ptr, 68)
   def prev_x = int(win.get("mouse_x", x))
   def prev_y = int(win.get("mouse_y", y))
   win["mouse_x"] = x
   win["mouse_y"] = y
   def enter_data = {"x": x, "y": y}
   events = _push_translated_event(events, 12, win, enter_data)
   if x != prev_x || y != prev_y {
      def pos_data = {"x": x, "y": y, "dx": x - prev_x, "dy": y - prev_y, "moved": true}
      events = _push_translated_event(events, 7, win, pos_data)
   }
   [win, events]
}

fn _translate_selection_clear_event(any win, any event_ptr) any {
   def selection = load64_h(event_ptr, 40)
   def clipboard_atom = win.get("clipboard_atom", 0)
   def primary_atom = win.get("primary_atom", 0)
   if selection == clipboard_atom { win["clipboard_owned"] = false }
   elif selection == primary_atom { win["primary_owned"] = false }
   win
}

fn _translate_selection_request_event(any win, any event_ptr, any display) bool {
   def requestor = load64_h(event_ptr, 40)
   def selection = load64_h(event_ptr, 48)
   def target = load64_h(event_ptr, 56)
   def property = load64_h(event_ptr, 64)
   def time = load64_h(event_ptr, 72)
   def utf8_string = win.get("utf8_string", 0)
   def targets_atom = win.get("targets_atom", 0)
   def multiple_atom = win.get("multiple_atom", 0)
   def atom_pair_atom = win.get("atom_pair_atom", 0)
   def save_targets = win.get("save_targets", 0)
   def reply_property = _write_selection_to_property(
      display, requestor, property, target, _selection_text_for_request(win, selection),
      utf8_string, targets_atom, multiple_atom, atom_pair_atom, save_targets,
   )
   _send_selection_notify(display, requestor, selection, target, reply_property, time)
   true
}

fn _translate_expose_event(any win, list events) list {
   win["mapped"] = true
   [win, _push_translated_event(events, 14, win, 0)]
}

fn _translate_destroy_event(any win, list events) list {
   win["should_close"] = true
   [win, _push_translated_event(events, 15, win, 0)]
}

fn _translate_event_server_events(
   any win, any event_ptr, list events, int typ, int randr_base, int xkb_base,
   bool xi_available, bool disabled_cursor, bool raw_mouse_motion, int xi_major_opcode,
   any scale_x, any scale_y,
) list {
   if randr_base >= 0 {
      if typ == randr_base + RRScreenChangeNotify {
         XRRUpdateConfiguration(event_ptr)
         win = _xrandr_poll_outputs(win, events)
         def scale = _xrandr_event_scale(event_ptr)
         def sx = scale.get(0, 1.0)
         def sy = scale.get(1, 1.0)
         if sx != scale_x || sy != scale_y {
            win["scale_x"] = sx
            win["scale_y"] = sy
            def data = {"xscale": sx, "yscale": sy}
            events = _push_translated_event(events, 21, win, data)
         }
         return [true, win, events]
      }
      if typ == randr_base + RRNotify {
         XRRUpdateConfiguration(event_ptr)
         win = _xrandr_poll_outputs(win, events)
         return [true, win, events]
      }
   }
   if xkb_base >= 0 {
      if typ == xkb_base + XkbEventCode {
         if load32(event_ptr, 40) == XkbStateNotify &&
         band(load32(event_ptr, 48), XkbGroupStateMask){
            win["xkb_group"] = load32(event_ptr, 52)
         }
         return [true, win, events]
      }
   }
   if typ == GenericEvent {
      if xi_available && disabled_cursor && raw_mouse_motion &&
      load32(event_ptr, 32) == xi_major_opcode{
         def out = _translate_raw_motion_event(win, event_ptr, events)
         return [true, out.get(0, win), out.get(1, events)]
      }
      return [true, win, events]
   }
   [false, win, events]
}

fn _translate_event_target_discard(any event_window, any window_handle, any root) bool {
   if event_window && window_handle && event_window != window_handle { return true }
   event_window && event_window != window_handle && event_window != root
}

fn _translate_event_input_dispatch(
   any win, any event_ptr, list events, int typ, any display, any window_handle, any root,
   any key_states, any mouse_buttons, any ic, any key_press_times, int repeat_sc,
   bool was_filtered, int cursor_mode, bool xi_available, bool raw_mouse_motion,
   bool ignore_warp, int warp_x, int warp_y, int win_w, int win_h, any mouse_x_prev, any mouse_y_prev,
) list {
   match typ {
      ReparentNotify -> {
         win["parent"] = load64_h(event_ptr, 32)
      }
      KeyPress -> {
         def out = _translate_key_press_event(
            win, event_ptr, events, key_states, key_press_times, ic, was_filtered, repeat_sc,
         )
         win, events = out.get(0, win), out.get(1, events)
      }
      KeyRelease -> {
         def out = _translate_key_release_event(win, event_ptr, events, key_states, display)
         win, events = out.get(0, win), out.get(1, events)
      }
      ButtonPress -> {
         def out = _translate_button_press_event(win, event_ptr, events, mouse_buttons)
         win, events = out.get(0, win), out.get(1, events)
      }
      ButtonRelease -> {
         def out = _translate_button_release_event(win, event_ptr, events, mouse_buttons)
         win, events = out.get(0, win), out.get(1, events)
      }
      EnterNotify -> {
         def out = _translate_enter_event(win, event_ptr, events)
         win, events = out.get(0, win), out.get(1, events)
      }
      LeaveNotify -> {
         win = _x11_clear_mouse_buttons(win)
         events = _push_translated_event(events, 13, win, 0)
      }
      MotionNotify -> {
         def out = _translate_motion_event(
            win, event_ptr, events, cursor_mode, xi_available, raw_mouse_motion, ignore_warp, warp_x, warp_y,
            win_w, win_h, mouse_x_prev, mouse_y_prev, display, window_handle,
         )
         win, events = out.get(0, win), out.get(1, events)
      }
      ConfigureNotify -> {
         def out = _translate_configure_event(win, event_ptr, events, display, window_handle, root, win_w, win_h)
         win, events = out.get(0, win), out.get(1, events)
      }
      _ -> { return [false, win, events] }
   }
   [true, win, events]
}

fn _translate_event_system_dispatch(
   any win, any event_ptr, list events, int typ, any display, any window_handle, any root,
   any ic, int cursor_mode, bool raw_mouse_motion, bool disabled_cursor, bool xi_available,
   any wm_protocols, any wm_delete, any net_wm_ping, any xdnd_enter_at, any xdnd_position_at, any xdnd_drop_at,
) list {
   match typ {
      ClientMessage -> {
         def out = _translate_client_message_event(
            win, event_ptr, events, display, window_handle, root,
            wm_protocols, wm_delete, net_wm_ping, xdnd_enter_at, xdnd_position_at, xdnd_drop_at,
         )
         win, events = out.get(0, win), out.get(1, events)
      }
      SelectionClear -> {
         win = _translate_selection_clear_event(win, event_ptr)
      }
      SelectionRequest -> {
         _translate_selection_request_event(win, event_ptr, display)
      }
      SelectionNotify -> {
         def out = _translate_selection_notify_event(win, event_ptr, events, display, window_handle)
         win, events = out.get(0, win), out.get(1, events)
      }
      FocusIn -> {
         def out = _translate_focus_in_event(win, event_ptr, events, ic, cursor_mode, raw_mouse_motion)
         win, events = out.get(0, win), out.get(1, events)
      }
      FocusOut -> {
         def out = _translate_focus_out_event(
            win, event_ptr, events, display, window_handle, root, ic, cursor_mode,
            disabled_cursor, xi_available, raw_mouse_motion,
         )
         win, events = out.get(0, win), out.get(1, events)
      }
      Expose -> {
         def out = _translate_expose_event(win, events)
         win, events = out.get(0, win), out.get(1, events)
      }
      PropertyNotify -> {
         def out = _translate_property_notify_event(win, event_ptr, events, display, window_handle)
         win, events = out.get(0, win), out.get(1, events)
      }
      DestroyNotify -> {
         def out = _translate_destroy_event(win, events)
         win, events = out.get(0, win), out.get(1, events)
      }
      _ -> { return [false, win, events] }
   }
   [true, win, events]
}

fn _translated_event_result(any win, any events) list {
   if !is_list(events) { return [win, []] }
   mut stable = []
   mut i = 0
   def n = events.len
   while i < n {
      def e = events.get(i)
      stable = stable.append(is_list(e) ? clone(e) : e)
      i += 1
   }
   [win, stable]
}

fn translate_event(any win, any event_ptr) list {
   "Translates one raw `XEvent` into zero or more std.os.ui events."
   if !win || !is_dict(win) || !event_ptr { return [win, []] }
   def display        = win.get("display", 0)
   def handle         = win.get("handle", 0)
   def root           = win.get("root", 0)
   def randr_base     = win.get("randr_event_base", -1)
   def xkb_base       = win.get("xkb_event_base", -1)
   def xi_major_opcode= win.get("xi_major_opcode", -1)
   def xi_available   = win.get("xi_available", false)
   def cursor_mode    = win.get("cursor_mode", CURSOR_MODE_NORMAL)
   def disabled_cursor= win.get("disabled_cursor", false)
   def raw_mouse_motion= win.get("raw_mouse_motion", false)
   def ignore_warp    = win.get("ignore_warp_motion", false)
   def warp_x         = win.get("warp_cursor_x", 0)
   def warp_y         = win.get("warp_cursor_y", 0)
   def win_w          = win.get("w", 1)
   def win_h          = win.get("h", 1)
   def mouse_x_prev   = win.get("mouse_x", 0)
   def mouse_y_prev   = win.get("mouse_y", 0)
   def scale_x        = win.get("scale_x", 1.0)
   def scale_y        = win.get("scale_y", 1.0)
   def wm_protocols   = win.get("wm_protocols", 0)
   def wm_delete      = win.get("wm_delete", 0)
   def net_wm_ping    = win.get("net_wm_ping", 0)
   def xdnd_enter_at  = win.get("xdnd_enter", 0)
   def xdnd_position_at= win.get("xdnd_position", 0)
   def xdnd_drop_at   = win.get("xdnd_drop", 0)
   def was_filtered   = win.get("last_event_was_filtered", false)
   def key_states     = win.get("key_states", 0)
   def mouse_buttons  = win.get("mouse_buttons", 0)
   def ic             = win.get("ic", 0)
   def repeat_sc      = win.get("repeat_scancode", -1)
   def key_press_times= win.get("key_press_times", 0)
   def typ = load32(event_ptr, 0)
   def event_window = _actual_event_window(event_ptr, typ)
   mut events = []
   def pre = _translate_event_server_events(
      win, event_ptr, events, typ, randr_base, xkb_base,
      xi_available, disabled_cursor, raw_mouse_motion, xi_major_opcode, scale_x, scale_y,
   )
   if pre.get(0, false) { return _translated_event_result(pre.get(1, win), pre.get(2, events)) }
   if _translate_event_target_discard(event_window, handle, root) { return _translated_event_result(win, events) }
   def input = _translate_event_input_dispatch(
      win, event_ptr, events, typ, display, handle, root, key_states, mouse_buttons, ic,
      key_press_times, repeat_sc, was_filtered, cursor_mode, xi_available, raw_mouse_motion,
      ignore_warp, warp_x, warp_y, win_w, win_h, mouse_x_prev, mouse_y_prev,
   )
   if input.get(0, false) { return _translated_event_result(input.get(1, win), input.get(2, events)) }
   def state = _translate_event_system_dispatch(
      win, event_ptr, events, typ, display, handle, root, ic, cursor_mode, raw_mouse_motion,
      disabled_cursor, xi_available, wm_protocols, wm_delete, net_wm_ping,
      xdnd_enter_at, xdnd_position_at, xdnd_drop_at,
   )
   if state.get(0, false) { return _translated_event_result(state.get(1, win), state.get(2, events)) }
   _translated_event_result(win, events)
}

fn _x11_append_event_coalesced(any out, any ev) list {
   if !is_list(out) { out = [] }
   if ui_event.is_event(ev) && ui_event.event_type(ev) == EVENT_MOUSE_POS_CHANGED && out.len > 0 {
      ;; Keep the fallback list-shaped so the compiler does not infer `last` as int.
      ;; `ui_event.event_type` intentionally accepts a concrete event list.
      def last = out.get(out.len - 1, [])
      if ui_event.is_event(last) && ui_event.event_type(last) == EVENT_MOUSE_POS_CHANGED {
         out[out.len - 1] = ev
         return out
      }
   }
   out.append(ev)
}

fn _x11_extend_events_coalesced(any out, any events) list {
   if !is_list(out) { out = [] }
   if !is_list(events) { return out }
   mut i = 0
   def n = events.len
   while i < n {
      out = _x11_append_event_coalesced(out, events.get(i))
      i += 1
   }
   out
}

fn poll_window_events(any win, int max_events=64) list {
   "Polls queued X11 events for `win` and returns `[updated_win, events]`."
   if !win || !is_dict(win) { return [win, []] }
   def fallback_win = win
   def display = win.get("display", 0)
   if !display { return [win, []] }
   mut p = pending(display)
   if p == 0 { return [win, []] }
   def event_buf = zalloc(192)
   if !event_buf { return [win, []] }
   mut out = []
   mut count = 0
   while p > 0 && count < max_events {
      next_event(display, event_buf)
      def typ = load32(event_buf, 0)
      def target = _actual_event_window(event_buf, typ)
      def filtered = XFilterEvent(event_buf, target) != 0
      if !is_dict(win) { win = fallback_win }
      if is_dict(win) { win["last_event_was_filtered"] = filtered }
      def translated = translate_event(win, event_buf)
      def next_win = translated.get(0, win)
      if is_dict(next_win) { win = next_win }
      def events = translated.get(1, [])
      if is_list(events) && events.len > 0 { out = _x11_extend_events_coalesced(out, events) }
      count += 1
      p = pending(display)
   }
   free(event_buf)
   _dbg_v("poll_window_events: " + to_str(count) + " raw events, " + to_str(out.len) + " dispatched")
   [win, out]
}

fn poll_display_events(list windows, int max_events=256) list {
   "Polls queued X11 events once per display and routes them to the owning window."
   if !is_list(windows) || windows.len == 0 { return [dict(8), []] }
   def first = windows.get(0)
   if !first || !is_dict(first) { return [dict(8), []] }
   def display = first.get("display", 0)
   if !display { return [dict(8), []] }
   def pending_count = pending(display)
   if pending_count == 0 { return [dict(8), []] }
   mut window_map = dict(8)
   mut focused_win = 0
   def windows_n = windows.len
   mut wi = 0
   while wi < windows_n {
      def cand = windows.get(wi)
      if is_dict(cand) {
         def handle = cand.get("handle", 0)
         if handle { window_map[handle] = cand }
         if !focused_win && cand.get("focused", false) { focused_win = cand }
      }
      wi += 1
   }
   def event_buf = malloc(192)
   if !event_buf { return [dict(8), []] }
   mut updated = dict(8)
   mut out = []
   mut count = 0
   mut p = pending_count
   while p > 0 && count < max_events {
      next_event(display, event_buf)
      def typ = load32(event_buf, 0)
      count += 1
      p = pending(display)
      def target = _actual_event_window(event_buf, typ)
      mut win = 0
      mut win_from_list = false
      if target { win = updated.get(target, 0) }
      if !win {
         win = window_map.get(target, 0)
         if win {
            updated[target] = win
            win_from_list = true
         }
      }
      if !win {
         win = focused_win
         if win {
            updated[win.get("handle", 0)] = win
            win_from_list = true
         }
      }
      if !win {
         win = first
         win_from_list = true
         updated[win.get("handle", 0)] = win
      }
      def filtered = XFilterEvent(event_buf, target) != 0
      if is_dict(win) { win["last_event_was_filtered"] = filtered }
      def translated = translate_event(win, event_buf)
      def next_win = translated.get(0, win)
      def evs = translated.get(1, [])
      if is_dict(next_win) {
         def handle = next_win.get("handle", 0)
         if handle { updated[handle] = next_win }
      }
      if is_list(evs) && evs.len > 0 { out = _x11_extend_events_coalesced(out, evs) }
   }
   free(event_buf)
   [updated, out]
}

fn translate_state(int state) int {
   "Translates an X11 modifier mask to Ny window modifier flags."
   mut mods = 0
   if band(state, ShiftMask) { mods = bor(mods, MOD_SHIFT) }
   if band(state, ControlMask) { mods = bor(mods, MOD_CONTROL) }
   if band(state, Mod1Mask) { mods = bor(mods, MOD_ALT) }
   if band(state, Mod4Mask) { mods = bor(mods, MOD_SUPER) }
   mods
}

def INVALID_CODEPOINT = x11_keymap.INVALID_CODEPOINT

fn translate_keysym(any primary, any secondary=0, int width=1) int {
   "Translate X11 keysyms using the backend fallback table."
   x11_keymap.translate_keysym(primary, secondary, width)
}

fn translate_scancode(int scancode) int {
   "Translates common X11 hardware scancodes to Ny key codes."
   x11_keymap.translate_scancode(scancode)
}

fn next_event(any display, any event_ptr) any {
   "Fetches the next X11 event from the display queue."
   XNextEvent(display, event_ptr)
}

fn post_empty_event(any win) bool {
   "Posts a dummy ClientMessage event to unblock event waiting."
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if !display || !handle { return false }
   def ev = zalloc(192)
   if !ev { return false }
   store32(ev, ClientMessage, 0)
   store64_h(ev, handle, 32)
   store64_h(ev, win.get("wm_protocols", 0), 40)
   store32(ev, 32, 48)
   XSendEvent(display, handle, 0, 0, ev)
   XFlush(display)
   free(ev)
   true
}

fn select_input(any display, any win, any event_mask) any {
   "Selects the X11 event mask for a window."
   XSelectInput(display, win, event_mask)
}

fn flush(any display) any {
   "Flushes pending X11 requests."
   XFlush(display)
}

fn put_pixels(any display, any win_ref, ptr buf, int w, int h) bool {
   "Blits an RGBA pixel buffer to an X11 window using XPutImage."
   if !display || !win_ref || !buf || w <= 0 || h <= 0 { return false }
   mut win_handle = win_ref
   if is_dict(win_ref) { win_handle = win_ref.get("handle", 0) }
   if !win_handle { return false }
   def screen = default_screen(display)
   mut vis = default_visual(display, screen)
   mut depth = default_depth(display, screen)
   if is_dict(win_ref) {
      vis = win_ref.get("visual", vis)
      depth = int(win_ref.get("depth", depth))
   }
   def n = w * h
   mut cvt_buf = _get_x11_val("sw_cvt_buf", 0)
   def cvt_n = _get_x11_val("sw_cvt_n", 0)
   if cvt_n != n {
      if cvt_buf { free(cvt_buf) }
      cvt_buf = malloc(n * 4)
      _set_x11_val("sw_cvt_buf", cvt_buf)
      _set_x11_val("sw_cvt_n", n)
   }
   if !cvt_buf { return false }
   mut i = 0
   while i < n {
      def o = i * 4
      def src = load32(buf, o)
      store32(cvt_buf, depth >= 32 ? src : band(src, 0x00FFFFFF), o)
      i += 1
   }
   mut sw_gc = _get_x11_val("sw_gc", 0)
   def gc_dpy = _get_x11_val("sw_gc_dpy", 0)
   def gc_win = _get_x11_val("sw_gc_win", 0)
   if sw_gc == 0 || gc_dpy != display || gc_win != win_handle {
      if sw_gc && gc_dpy == display { XFreeGC(display, sw_gc) }
      sw_gc = XCreateGC(display, win_handle, 0, 0)
      _set_x11_val("sw_gc", sw_gc)
      _set_x11_val("sw_gc_dpy", display)
      _set_x11_val("sw_gc_win", win_handle)
   }
   def xi = XCreateImage(display, vis, depth, 2, 0, cvt_buf, w, h, 32, 0)
   if xi && sw_gc {
      XPutImage(display, win_handle, sw_gc, xi, 0, 0, 0, 0, w, h)
      store64_h(xi, 0, 16)
      XFree(xi)
   }
   XFlush(display)
   true
}

fn sync(any display, bool discard=false) any {
   "Synchronizes with the X server."
   XSync(display, discard ? 1 : 0)
}

fn store_name(any display, any win, any window_name, any net_wm_name_atom=0, any net_wm_icon_name_atom=0, any utf8_string_atom=0) bool {
   "Sets the X11 window title via both ICCCM and EWMH UTF-8 properties."
   if !display || !win { return false }
   if !is_str(window_name) { window_name = to_str(window_name) }
   XStoreName(display, win, cstr(window_name))
   if !net_wm_name_atom { net_wm_name_atom = intern_atom(display, "_NET_WM_NAME") }
   if !net_wm_icon_name_atom { net_wm_icon_name_atom = intern_atom(display, "_NET_WM_ICON_NAME") }
   if !utf8_string_atom { utf8_string_atom = intern_atom(display, "UTF8_STRING") }
   if net_wm_name_atom && utf8_string_atom {
      _set_utf8_text_property(
         display, win, net_wm_name_atom, utf8_string_atom, window_name,
      )
   }
   if net_wm_icon_name_atom && utf8_string_atom {
      _set_utf8_text_property(
         display, win, net_wm_icon_name_atom, utf8_string_atom, window_name,
      )
   }
   flush(display)
   true
}

fn get_pos(any win) list {
   "Returns the X11 window position as [x, y] via XTranslateCoordinates."
   if !win || !is_dict(win) { return [0, 0] }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   def root = win.get("root", 0)
   if !display || !handle || !root { return [win.get("x", 0), win.get("y", 0)] }
   def xp, yp = malloc(4), malloc(4)
   def child = malloc(8)
   if !xp || !yp || !child {
      if xp { free(xp) } if yp { free(yp) } if child { free(child) }
      return [win.get("x", 0), win.get("y", 0)]
   }
   store32(xp, 0, 0) store32(yp, 0, 0) store64_h(child, 0, 0)
   XTranslateCoordinates(display, handle, root, 0, 0, xp, yp, child)
   def rx, ry = load32(xp, 0), load32(yp, 0)
   free(xp) free(yp) free(child)
   [rx, ry]
}

fn move_window(any display, any win, int x, int y) any {
   "Moves a raw X11 window."
   XMoveWindow(display, win, x, y)
}

fn _get_visual_depth_fallback(any display, int screen, any visual) int {
   if !display || !visual { return 24 }
   def vptr = zalloc(8)
   def n = zalloc(4)
   if !vptr || !n {
      if vptr { free(vptr) }
      if n { free(n) }
      return 24
   }
   def vinfo = XGetVisualInfo(display, 0, vptr, n)
   def count = load32(n, 0)
   mut depth = 24
   if vinfo && count > 0 {
      mut i = 0 while i < count {
         def vi = vinfo + i * 64
         if load64_h(vi, 0) == visual {
            depth = load32(vi, 20)
            break
         }
         i += 1
      }
      XFree(vinfo)
   }
   free(vptr) free(n)
   depth
}

fn _choose_window_visual(any display, int screen, bool want_argb32, any provided_visual=0, int provided_depth=0) list {
   if provided_visual {
      def visual_depth = _get_visual_depth_fallback(display, screen, provided_visual)
      def depth = (visual_depth > 0) ? visual_depth : provided_depth
      return [provided_visual, depth]
   }
   mut fallback_visual = default_visual(display, screen)
   mut fallback_depth = default_depth(display, screen)
   if !want_argb32 { return [fallback_visual, fallback_depth] }
   def tpl = zalloc(64)
   def n = zalloc(4)
   if !tpl || !n {
      if tpl { free(tpl) }
      if n { free(n) }
      return [fallback_visual, fallback_depth]
   }
   store32(tpl, screen, 16)
   def infos = XGetVisualInfo(display, 2, tpl, n)
   def count = load32(n, 0)
   if infos && count > 0 {
      mut i = 0
      while i < count {
         def vi = infos + i * 64
         def depth = load32(vi, 20)
         def klass = load32(vi, 24)
         if depth == 32 && klass == 4 {
            fallback_visual = load64_h(vi, 0)
            fallback_depth = 32
            break
         }
         i += 1
      }
      XFree(infos)
   }
   free(tpl, n)
   [fallback_visual, fallback_depth]
}

fn resize_window(any display, any win, int width, int height) any {
   "Resizes a raw X11 window."
   XResizeWindow(display, win, width, height)
}

fn _canonical_window_event_mask() int {
   KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
   EnterWindowMask | LeaveWindowMask | PointerMotionMask | ExposureMask |
   VisibilityChangeMask | FocusChangeMask | StructureNotifyMask |
   PropertyChangeMask
}

fn _default_create_event_mask(int event_mask) int {
   if event_mask != 0 { return bor(_canonical_window_event_mask(), event_mask) }
   _canonical_window_event_mask()
}

fn _adjust_window_create_geometry(any display, int screen, int flags, int x, int y, int width, int height) list {
   mut out_x, out_y = x, y
   mut out_w, out_h = width, height
   if band(flags, WINDOW_FULLSCREEN) {
      def sw, sh = XDisplayWidth(display, screen), XDisplayHeight(display, screen)
      if sw > 0 && sh > 0 {
         out_x, out_y = 0, 0
         out_w, out_h = sw, sh
      }
   }
   if band(flags, WINDOW_CENTER) {
      def sw, sh = XDisplayWidth(display, screen), XDisplayHeight(display, screen)
      if sw > 0 && sh > 0 { out_x, out_y = (sw - out_w) / 2, (sh - out_h) / 2 }
   }
   [out_x, out_y, out_w, out_h]
}

fn _intern_named_atoms(any display, list names) dict {
   if !display || !names || !is_list(names) { return dict(8) }
   mut out = dict(64)
   mut i = 0
   while i < names.len {
      def name = names[i]
      out[name] = intern_atom(display, name)
      i += 1
   }
   out
}

fn _x11_window_atom_state(any display) dict {
   def raw = _intern_named_atoms(display, [
         "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_PING", "WM_STATE",
         "_NET_ACTIVE_WINDOW", "_NET_WM_PID", "_NET_WM_FULLSCREEN_MONITORS",
         "_NET_WM_STATE", "_NET_WM_BYPASS_COMPOSITOR", "_NET_WM_WINDOW_TYPE",
         "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_STATE_ABOVE",
         "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_MAXIMIZED_VERT",
         "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_DEMANDS_ATTENTION",
         "_MOTIF_WM_HINTS", "_NET_WM_WINDOW_OPACITY", "_NET_FRAME_EXTENTS",
         "_NET_REQUEST_FRAME_EXTENTS", "CLIPBOARD", "CLIPBOARD_MANAGER",
         "PRIMARY", "TARGETS", "MULTIPLE", "ATOM_PAIR", "INCR", "SAVE_TARGETS",
         "UTF8_STRING", "_NET_WM_NAME", "_NET_WM_ICON", "_NET_WM_ICON_NAME",
         "NYTRIX_SELECTION", "XdndAware", "XdndEnter", "XdndPosition",
         "XdndStatus", "XdndActionCopy", "XdndDrop", "XdndFinished",
         "XdndSelection", "XdndTypeList", "text/uri-list",
   ])
   {
      "wm_protocols": raw.get("WM_PROTOCOLS", 0),
      "wm_delete": raw.get("WM_DELETE_WINDOW", 0),
      "net_wm_ping": raw.get("_NET_WM_PING", 0),
      "wm_state": raw.get("WM_STATE", 0),
      "net_active_window": raw.get("_NET_ACTIVE_WINDOW", 0),
      "net_wm_pid": raw.get("_NET_WM_PID", 0),
      "net_wm_fullscreen_monitors": raw.get("_NET_WM_FULLSCREEN_MONITORS", 0),
      "net_wm_state": raw.get("_NET_WM_STATE", 0),
      "net_wm_bypass_compositor": raw.get("_NET_WM_BYPASS_COMPOSITOR", 0),
      "net_wm_window_type": raw.get("_NET_WM_WINDOW_TYPE", 0),
      "net_wm_window_type_normal": raw.get("_NET_WM_WINDOW_TYPE_NORMAL", 0),
      "net_wm_state_above": raw.get("_NET_WM_STATE_ABOVE", 0),
      "net_wm_state_fullscreen": raw.get("_NET_WM_STATE_FULLSCREEN", 0),
      "net_wm_state_maximized_vert": raw.get("_NET_WM_STATE_MAXIMIZED_VERT", 0),
      "net_wm_state_maximized_horz": raw.get("_NET_WM_STATE_MAXIMIZED_HORZ", 0),
      "net_wm_state_demands_attention": raw.get("_NET_WM_STATE_DEMANDS_ATTENTION", 0),
      "motif_wm_hints": raw.get("_MOTIF_WM_HINTS", 0),
      "net_wm_window_opacity": raw.get("_NET_WM_WINDOW_OPACITY", 0),
      "net_frame_extents": raw.get("_NET_FRAME_EXTENTS", 0),
      "net_request_frame_extents": raw.get("_NET_REQUEST_FRAME_EXTENTS", 0),
      "clipboard_atom": raw.get("CLIPBOARD", 0),
      "clipboard_manager": raw.get("CLIPBOARD_MANAGER", 0),
      "primary_atom": raw.get("PRIMARY", 0),
      "targets_atom": raw.get("TARGETS", 0),
      "multiple_atom": raw.get("MULTIPLE", 0),
      "atom_pair_atom": raw.get("ATOM_PAIR", 0),
      "incr_atom": raw.get("INCR", 0),
      "save_targets": raw.get("SAVE_TARGETS", 0),
      "utf8_string": raw.get("UTF8_STRING", 0),
      "net_wm_name": raw.get("_NET_WM_NAME", 0),
      "net_wm_icon": raw.get("_NET_WM_ICON", 0),
      "net_wm_icon_name": raw.get("_NET_WM_ICON_NAME", 0),
      "selection_property": raw.get("NYTRIX_SELECTION", 0),
      "xdnd_aware": raw.get("XdndAware", 0),
      "xdnd_enter": raw.get("XdndEnter", 0),
      "xdnd_position": raw.get("XdndPosition", 0),
      "xdnd_status": raw.get("XdndStatus", 0),
      "xdnd_action_copy": raw.get("XdndActionCopy", 0),
      "xdnd_drop": raw.get("XdndDrop", 0),
      "xdnd_finished": raw.get("XdndFinished", 0),
      "xdnd_selection": raw.get("XdndSelection", 0),
      "xdnd_type_list": raw.get("XdndTypeList", 0),
      "text_uri_list": raw.get("text/uri-list", 0),
   }
}

fn _x11_close_create_resources(any display, any im, any colormap=0) bool {
   if colormap { free_colormap(display, colormap) }
   if im { _c_xcloseim(im) }
   if display { close_display(display) }
   false
}

fn _x11_create_window_handle(any display, any root, int x, int y, int width, int height, int depth, any visual, int event_mask, any colormap) any {
   def attrs = zalloc(112)
   if !attrs { return 0 }
   store64_h(attrs, 0, 0)
   store64_h(attrs, 0, 8)
   store64_h(attrs, 0, 24)
   store64_h(attrs, colormap, 96)
   mut value_mask = CWBackPixel | CWBorderPixel | CWColormap
   if event_mask != 0 {
      store64_h(attrs, event_mask, 72)
      value_mask = bor(value_mask, CWEventMask)
   }
   def handle = create_window_raw(display, root, x, y, width, height, 0, depth, InputOutput, visual, value_mask, attrs)
   free(attrs)
   XSync(display, 0)
   handle
}

fn _x11_set_wm_protocols(any display, any window_handle, any wm_protocols, any wm_delete, any net_wm_ping) bool {
   if !(wm_protocols && wm_delete) { return false }
   def protocol_count = net_wm_ping ? 2 : 1
   def protocol_atoms = zalloc(protocol_count * 8)
   if !protocol_atoms { return false }
   store32(protocol_atoms, wm_delete, 0)
   if net_wm_ping { store32(protocol_atoms, net_wm_ping, 8) }
   XSetWMProtocols(display, window_handle, protocol_atoms, protocol_count)
   free(protocol_atoms)
   true
}

fn _x11_enable_xdnd(any display, any window_handle, any xdnd_aware) bool {
   if !xdnd_aware { return false }
   def xdnd_ver = zalloc(8)
   if !xdnd_ver { return false }
   store32(xdnd_ver, XDND_VERSION, 0)
   XChangeProperty(display, window_handle, xdnd_aware, XA_ATOM, 32, PropModeReplace, xdnd_ver, 1)
   free(xdnd_ver)
   true
}

fn _x11_apply_initial_window_presentation(
   any display, any root, any window_handle, int screen, int flags, bool hidden, bool fullscreen,
   bool floating, bool maximized, bool minimized, any net_wm_state, any net_wm_state_fullscreen,
   any net_wm_state_above, any net_wm_state_maximized_vert, any net_wm_state_maximized_horz,
   any net_wm_bypass_compositor,
) bool {
   if hidden { return false }
   if fullscreen && net_wm_state && net_wm_state_fullscreen { append_atom_property(display, window_handle, net_wm_state, net_wm_state_fullscreen) }
   _show_window_raw(display, window_handle, floating, net_wm_state, net_wm_state_above)
   if maximized {
      _maximize_window_raw(
         display, root, window_handle, net_wm_state,
         net_wm_state_maximized_vert, net_wm_state_maximized_horz,
      )
   }
   if fullscreen {
      if net_wm_state && net_wm_state_fullscreen {
         set_window_fullscreen(display, root, window_handle, net_wm_state, net_wm_state_fullscreen, true)
      } else {
         _set_override_redirect(display, window_handle, true)
      }
      if net_wm_bypass_compositor && !band(flags, WINDOW_TRANSPARENT) { _set_compositor_bypass(display, window_handle, net_wm_bypass_compositor, true) }
   }
   if minimized { _iconify_window_raw(display, window_handle, screen) }
   true
}

fn _x11_has_compositor(any display, int screen, int flags) bool {
   if !band(flags, WINDOW_TRANSPARENT) { return false }
   def cm_name = f"_NET_WM_CM_S{screen}"
   def cm_atom = intern_atom(display, cm_name)
   cm_atom && XGetSelectionOwner(display, cm_atom) != 0
}

fn _x11_build_window_state(
   any display, any im, any ic, dict xkb, dict xi, dict key_tables, int screen, any root,
   any colormap, any visual, int depth, any window_handle, dict atoms, any title, int x, int y,
   int width, int height, int flags, int event_mask, bool resizable, bool decorated, bool floating,
   bool fullscreen, bool hidden, bool focus_on_show, bool minimized, bool maximized,
   bool want_transparent, bool has_compositor, bool randr_available, int randr_event,
   int randr_error, int cursor_mode,
) dict {
   def net_wm_state = atoms.get("net_wm_state", 0)
   def net_wm_state_fullscreen = atoms.get("net_wm_state_fullscreen", 0)
   mut win = {
      "display": display, "im": im, "ic": ic,
      "xkb_available": xkb.get("available", false), "xkb_event_base": xkb.get("event_base", -1),
      "xkb_error_base": xkb.get("error_base", -1), "xkb_group": xkb.get("group", 0),
      "xkb_detectable_repeat": xkb.get("detectable_repeat", false),
      "x11_keycodes": key_tables.get("keycodes", dict(256)), "x11_scancodes": key_tables.get("scancodes", dict(256)),
      "xi_available": xi.get("available", false), "xi_major_opcode": xi.get("major_opcode", -1),
      "xi_event_base": xi.get("event_base", -1), "xi_error_base": xi.get("error_base", -1),
      "screen": screen, "root": root, "colormap": colormap, "visual": visual, "depth": depth, "handle": window_handle,
      "event_mask": event_mask,
      "xdnd_source": 0, "xdnd_version": 0, "xdnd_format": 0,
      "title": title ? to_str(title) : "", "clipboard_string": "", "clipboard_owned": false,
      "primary_selection_string": "", "primary_owned": false,
      "key_states": dict(64), "key_press_times": dict(64), "mouse_buttons": dict(8),
      "x": x, "y": y, "w": width, "h": height,
      "resizable": resizable, "decorated": decorated, "floating": floating, "fullscreen": fullscreen,
      "override_redirect": fullscreen && !(net_wm_state && net_wm_state_fullscreen),
      "visible": !hidden, "mapped": !hidden, "focused": focus_on_show, "iconified": minimized, "maximized": maximized,
      "transparent": want_transparent && has_compositor && depth >= 32, "flags": flags,
      "cursor": 0, "cursor_handle": 0, "randr_available": randr_available,
      "randr_event_base": randr_event, "randr_error_base": randr_error,
      "raw_mouse_motion": band(flags, WINDOW_RAW_MOUSE), "raw_mouse_lock_auto": false, "cursor_mode": cursor_mode,
      "captured_cursor": false, "disabled_cursor": false,
      "restore_cursor_x": 0, "restore_cursor_y": 0, "virtual_cursor_x": 0.0, "virtual_cursor_y": 0.0,
      "warp_cursor_x": 0, "warp_cursor_y": 0, "ignore_warp_motion": false,
      "randr_outputs": dict(8), "scale_x": 1.0, "scale_y": 1.0
   }
   dict_merge(win, atoms)
}

fn create_basic_window(
   str title,
   int width,
   int height,
   int x=0,
   int y=0,
   int flags=0,
   int event_mask=0,
   str class_name="Nytrix",
   str instance_name="nytrix",
   any provided_visual=0,
   int provided_depth=0,
   any provided_display=0
) any {
   "Creates and maps a basic X11 top-level window using Ny-side logic."
   if !available() { return false }
   if ui_profile.env_truthy_cached("NY_UI_INPUT_TRACE") {
      _dbg_input("create.entry title=" + title +
         " flags=0x" + str.to_hex(flags) +
         " event_arg=0x" + str.to_hex(event_mask) +
         " provided_visual=0x" + str.to_hex(provided_visual) +
      " provided_depth=" + to_str(provided_depth))
   }
   def display = provided_display ? provided_display : open_display()
   if !display { return false }
   mut im, ic = _open_input_method(display), 0
   def xkb = _xkb_setup(display)
   def xi = _xi_setup(display)
   def key_tables = _create_key_tables(display)
   def screen = default_screen(display)
   def root = root_window(display, screen)
   def want_transparent = band(flags, WINDOW_TRANSPARENT)
   def want_argb32 = want_transparent
   def visual_pair = _choose_window_visual(display, screen, want_argb32, provided_visual, provided_depth)
   mut visual = visual_pair.get(0, default_visual(display, screen))
   mut depth = visual_pair.get(1, default_depth(display, screen))
   def fullscreen = band(flags, WINDOW_FULLSCREEN)
   def randr_available = false
   def randr_event_base = 0
   def randr_error_base = 0
   event_mask = _default_create_event_mask(event_mask)
   def geom = _adjust_window_create_geometry(display, screen, flags, x, y, width, height)
   x, y = int(geom.get(0, x)), int(geom.get(1, y))
   width = int(geom.get(2, width))
   height = int(geom.get(3, height))
   def colormap = create_colormap(display, root, visual, AllocNone)
   def handle = _x11_create_window_handle(display, root, x, y, width, height, depth, visual, event_mask, colormap)
   if !handle { return _x11_close_create_resources(display, im, colormap) }
   if event_mask != 0 { select_input(display, handle, event_mask) }
   def atoms = _x11_window_atom_state(display)
   def wm_protocols = atoms.get("wm_protocols", 0)
   def wm_delete = atoms.get("wm_delete", 0)
   def net_wm_ping = atoms.get("net_wm_ping", 0)
   def net_wm_name = atoms.get("net_wm_name", 0)
   def net_wm_icon_name = atoms.get("net_wm_icon_name", 0)
   def utf8_string = atoms.get("utf8_string", 0)
   def net_wm_pid = atoms.get("net_wm_pid", 0)
   def net_wm_window_type = atoms.get("net_wm_window_type", 0)
   def net_wm_window_type_normal = atoms.get("net_wm_window_type_normal", 0)
   def net_wm_state = atoms.get("net_wm_state", 0)
   def net_wm_state_fullscreen = atoms.get("net_wm_state_fullscreen", 0)
   def net_wm_state_above = atoms.get("net_wm_state_above", 0)
   def net_wm_state_maximized_vert = atoms.get("net_wm_state_maximized_vert", 0)
   def net_wm_state_maximized_horz = atoms.get("net_wm_state_maximized_horz", 0)
   def motif_wm_hints = atoms.get("motif_wm_hints", 0)
   def net_wm_bypass_compositor = atoms.get("net_wm_bypass_compositor", 0)
   def xdnd_aware = atoms.get("xdnd_aware", 0)
   _x11_set_wm_protocols(display, handle, wm_protocols, wm_delete, net_wm_ping)
   if title { store_name(display, handle, title, net_wm_name, net_wm_icon_name, utf8_string) }
   _set_window_pid(display, handle, net_wm_pid)
   _set_window_type_normal(display, handle, net_wm_window_type, net_wm_window_type_normal)
   _set_window_manager_hints(display, handle)
   _set_initial_normal_hints(display, handle, width, height, !band(flags, WINDOW_NO_RESIZE),
   x, y, x != 0 || y != 0)
   _set_class_hint(display, handle, instance_name, class_name)
   if im { ic = _create_input_context(im, handle) }
   _x11_enable_xdnd(display, handle, xdnd_aware)
   def resizable = !band(flags, WINDOW_NO_RESIZE)
   def decorated = !band(flags, WINDOW_NO_BORDER)
   def floating = band(flags, WINDOW_FLOATING)
   def maximized = band(flags, WINDOW_MAXIMIZE)
   def hidden = band(flags, WINDOW_HIDE)
   def minimized = band(flags, WINDOW_MINIMIZE)
   def focus_on_show = band(flags, WINDOW_FOCUS) || band(flags, WINDOW_FOCUS_ON_SHOW)
   if !decorated && motif_wm_hints { _set_window_decorated_raw(display, handle, motif_wm_hints, false) }
   if !resizable { update_normal_hints(display, handle, width, height, false, false) }
   _x11_apply_initial_window_presentation(
      display, root, handle, screen, flags, hidden, fullscreen, floating, maximized, minimized,
      net_wm_state, net_wm_state_fullscreen, net_wm_state_above,
      net_wm_state_maximized_vert, net_wm_state_maximized_horz, net_wm_bypass_compositor,
   )
   def has_compositor = _x11_has_compositor(display, screen, flags)
   def randr_event = randr_event_base ? load32(randr_event_base, 0) : -1
   def randr_error = randr_error_base ? load32(randr_error_base, 0) : -1
   if randr_event_base { free(randr_event_base) }
   if randr_error_base { free(randr_error_base) }
   def cursor_mode = band(flags, WINDOW_CAPTURE_MOUSE) ? CURSOR_MODE_CAPTURED : (band(flags,
   WINDOW_HIDE_MOUSE) ? CURSOR_MODE_HIDDEN : CURSOR_MODE_NORMAL)
   mut win = _x11_build_window_state(
      display, im, ic, xkb, xi, key_tables, screen, root, colormap, visual, depth, handle, atoms,
      title, x, y, width, height, flags, event_mask, resizable, decorated, floating, fullscreen, hidden,
      focus_on_show, minimized, maximized, want_transparent, has_compositor,
      randr_available, randr_event, randr_error, cursor_mode,
   )
   if !hidden {
      if ic && focus_on_show { _c_xseticfocus(ic) }
      win = set_input_mode(win, INPUT_MODE_RAW_MOUSE,
      win.get("raw_mouse_motion", false) ? 1 : 0)
      def initial_cursor_mode = win.get("cursor_mode", CURSOR_MODE_NORMAL)
      if initial_cursor_mode != CURSOR_MODE_NORMAL { win = set_input_mode(win, INPUT_MODE_CURSOR, initial_cursor_mode) }
      if focus_on_show {
         focus_window(win)
         win["focused"] = _window_focused(display, handle) != 0
      }
   }
   _sync_window_state(win)
}

fn get_key_name(any win, int key, int scancode) str {
   "Returns the layout-specific name of the specified printable key."
   if !win || !is_dict(win) { return "" }
   def display = win.get("display", 0)
   if !display { return "" }
   mut code = int(scancode)
   if code == 0 {
      def scancodes = win.get("x11_scancodes", 0)
      if is_dict(scancodes) { code = int(scancodes.get(key, 0)) }
      if code == 0 {
         def keysym = x11_keymap.keysym_from_key(key)
         if keysym == 0 { return "" }
         code = int(_c_xkeysym_to_keycode(display, keysym))
      }
      if code == 0 { return "" }
   }
   if code < 0 || code > 255 { return "" }
   def keysym = _c_xkb_keycode_to_keysym(display, code, 0, 0)
   if keysym == 0 { return "" }
   def name_ptr = _c_xkeysym_to_string(keysym)
   name_ptr ? str.cstr_to_str(name_ptr) : ""
}

fn get_size(any win) list {
   "Returns the X11 window size as [width, height]."
   if !win || !is_dict(win) { return [0, 0] }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   def sz = get_window_size(display, handle)
   if !sz || !is_dict(sz) { return [win.get("w", 0), win.get("h", 0)] }
   [sz.get("width", 0), sz.get("height", 0)]
}

fn close_basic_window(any win) bool {
   "Destroys a basic Ny-created X11 window and closes its display connection."
   if !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def im = win.get("im", 0)
   def ic = win.get("ic", 0)
   def handle = win.get("handle", 0)
   def colormap = win.get("colormap", 0)
   if win.get("clipboard_owned", false) { _push_selection_to_manager(win) }
   if ic { _c_xdestroyic(ic) }
   if colormap && display { free_colormap(display, colormap) }
   if handle && display { destroy_window_raw(display, handle) }
   if im { _c_xcloseim(im) }
   if display { close_display(display) }
   true
}

fn _vk_surface_preference() str {
   def pref = common.value_or(common.env_lower("NY_UI_X11_VK_SURFACE"), "auto")
   if pref == "xcb" || pref == "xlib" { return pref }
   "auto"
}

fn _vk_xcb_surface_create_info(any connection, any handle) any {
   def info = zalloc(40)
   if !info { return 0 }
   store32(info, 1000005000, 0)
   store64_h(info, 0, 8)
   store32(info, 0, 16)
   store64_h(info, connection, 24)
   store32(info, handle, 32)
   info
}

fn _finish_vk_surface_create(any surface, int res, str label) int {
   if res != 0 {
      store64(surface, 0, 0)
      return -1
   }
   def handle_val = load64(surface, 0)
   if handle_val == 0 || handle_val == 0x8000000000 {
      if ui_profile.debug_verbose_enabled() {
         print(
            "[x11] " + label +
            " invalid handle=0x" + str.to_hex(handle_val) +
            " (forcing fallback)",
         )
      }
      store64(surface, 0, 0)
      return -1
   }
   0
}

fn _create_xcb_surface(any instance, any display, any handle, any surface) int {
   def connection = _c_xgetxcbconnection(display)
   if !connection {
      if ui_profile.debug_verbose_enabled() { print("[x11] XCB surface unavailable: XGetXCBConnection returned null") }
      return -1
   }
   def info = _vk_xcb_surface_create_info(connection, handle)
   if !info {
      if ui_profile.debug_verbose_enabled() { print("[x11] XCB surface unavailable: create-info allocation failed") }
      return -1
   }
   store64(surface, 0, 0)
   if ui_profile.debug_verbose_enabled() {
      print(
         "[x11] XCB info ptr=" + to_str(info) +
         " stype=" + to_str(load32(info, 0)) +
         " pnext=0x" + str.to_hex(load64(info, 8)) +
         " flags=" + to_str(load32(info, 16)) +
         " conn=0x" + str.to_hex(load64(info, 24)) +
         " win=" + to_str(load32(info, 32)),
      )
   }
   def res = vk_create_xcb_surface_khr(instance, info, 0, surface)
   if ui_profile.debug_verbose_enabled() {
      print(
         "[x11] vkCreateXcbSurfaceKHR res=" + to_str(res) +
         " surf=0x" + str.to_hex(load64(surface, 0)) +
         " conn=" + to_str(connection) +
         " win=" + to_str(handle),
      )
   }
   free(info)
   _finish_vk_surface_create(surface, res, "vkCreateXcbSurfaceKHR")
}

fn _create_xlib_surface(any instance, any display, any handle, any surface) int {
   def info = zalloc(48)
   if !info {
      store64(surface, 0, 0)
      return -1
   }
   store32(info, 1000004000, 0)
   store64_h(info, 0, 8)
   store32(info, 0, 16)
   store64_h(info, display, 24)
   store64_h(info, handle, 32)
   XSync(display, 0)
   if ui_profile.debug_verbose_enabled() {
      print(
         "[x11] BEFORE vkCreateXlibSurfaceKHR surf_ptr=" + to_str(surface) +
         " surf_val=0x" + str.to_hex(load64(surface, 0)) +
         " info=" + to_str(info) +
         " stype=" + to_str(load32(info, 0)) +
         " pnext=0x" + str.to_hex(load64(info, 8)) +
         " flags=" + to_str(load32(info, 16)) +
         " dpy=0x" + str.to_hex(load64(info, 24)) +
         " win=" + to_str(load64(info, 32)),
      )
   }
   def res = vk_create_xlib_surface_khr(instance, info, 0, surface)
   if ui_profile.debug_verbose_enabled() {
      print(
         "[x11] vkCreateXlibSurfaceKHR res=" + to_str(res) +
         " surf=0x" + str.to_hex(load64(surface, 0)) +
         " dpy=" + to_str(display) +
         " win=" + to_str(handle),
      )
   }
   XSync(display, 0)
   free(info)
   _finish_vk_surface_create(surface, res, "vkCreateXlibSurfaceKHR")
}

fn _restore_window_event_mask(any win, any display, any handle) bool {
   if !display || !handle { return false }
   def mask = _default_create_event_mask(0)
   select_input(display, handle, mask)
   XSync(display, 0)
   if is_dict(win) {
      win["event_mask"] = mask
      _sync_window_state(win)
   }
   if ui_profile.env_truthy_cached("NY_UI_INPUT_TRACE") {
      _dbg_input("restore event_mask=0x" + str.to_hex(mask) + " win=0x" + str.to_hex(handle))
   }
   true
}

fn create_surface(any instance, any win, any allocator, any surface) int {
   "Creates a Vulkan X11 surface for the given backend window."
   if !is_dict(win) { return -1 }
   mut display = win.get("display", 0)
   mut handle = win.get("handle", 0)
   if is_dict(handle) {
      if !display { display = handle.get("display", 0) }
      handle = handle.get("handle", 0)
   }
   if !display || !handle { return -1 }
   def pref = _vk_surface_preference()
   mut res = -1
   if pref == "xcb" {
      res = _create_xcb_surface(instance, display, handle, surface)
      if res != 0 { res = _create_xlib_surface(instance, display, handle, surface) }
      if res == 0 { _restore_window_event_mask(win, display, handle) }
      return res
   }
   if pref == "xlib" {
      res = _create_xlib_surface(instance, display, handle, surface)
      if res != 0 { res = _create_xcb_surface(instance, display, handle, surface) }
      if res == 0 { _restore_window_event_mask(win, display, handle) }
      return res
   }
   res = _create_xlib_surface(instance, display, handle, surface)
   if res != 0 { res = _create_xcb_surface(instance, display, handle, surface) }
   if res == 0 { _restore_window_event_mask(win, display, handle) }
   res
}

fn vulkan_get_surface_capabilities(any phys, any surf, any caps) int {
   "Thin wrapper over `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`."
   def res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, caps)
   res
}

fn vulkan_get_surface_support(any phys, int queue_family, any surf, any supported_ptr) int {
   "Thin wrapper over `vkGetPhysicalDeviceSurfaceSupportKHR`."
   def res = vkGetPhysicalDeviceSurfaceSupportKHR(phys, queue_family, surf, supported_ptr)
   res
}

fn get_gamma_ramp(any monitor) any {
   "Returns the XRandR gamma ramp for the given monitor."
   if !is_dict(monitor) { return 0 }
   def display = monitor.get("display", 0)
   def crtc = monitor.get("crtc", 0)
   if !display || !crtc { return 0 }
   def size = XRRGetCrtcGammaSize(display, crtc)
   if size <= 0 { return 0 }
   def gamma = XRRGetCrtcGamma(display, crtc)
   if !gamma { return 0 }
   def red_ptr = load64_h(gamma, 0)
   def green_ptr = load64_h(gamma, 8)
   def blue_ptr = load64_h(gamma, 16)
   mut red, green, blue = [], [], []
   mut i = 0 while i < size {
      red = red.append(load16(red_ptr, i * 2))
      green = green.append(load16(green_ptr, i * 2))
      blue = blue.append(load16(blue_ptr, i * 2))
      i += 1
   }
   XRRFreeGamma(gamma)
   def res = {"size": size, "red": red, "green": green, "blue": blue}
   res
}

fn set_gamma_ramp(any monitor, any ramp) bool {
   "Applies an XRandR gamma ramp to the given monitor."
   if !is_dict(monitor) || !is_dict(ramp) { return false }
   def display = monitor.get("display", 0)
   def crtc = monitor.get("crtc", 0)
   if !display || !crtc { return false }
   def size = ramp.get("size", 0)
   if size <= 0 { return false }
   def red = ramp.get("red", [])
   def green = ramp.get("green", [])
   def blue = ramp.get("blue", [])
   if red.len < size || green.len < size || blue.len < size { return false }
   def gamma = XRRAllocGamma(size)
   if !gamma { return false }
   def red_ptr = load64_h(gamma, 0)
   def green_ptr = load64_h(gamma, 8)
   def blue_ptr = load64_h(gamma, 16)
   mut i = 0 while i < size {
      store16(red_ptr, red[i], i * 2)
      store16(green_ptr, green[i], i * 2)
      store16(blue_ptr, blue[i], i * 2)
      i += 1
   }
   XRRSetCrtcGamma(display, crtc, gamma)
   XRRFreeGamma(gamma)
   true
}

fn vulkan_supported() bool {
   "Returns true if the X11 backend supports Vulkan(currently always true for Linux builds)."
   true
}

fn _vulkan_extension_ptrs(any surface, any xcb, any xlib, str pref, bool use_xcb) list {
   mut count = 2
   if pref != "xcb" && pref != "xlib" && use_xcb { count = 3 }
   def arr = zalloc(count * 8)
   if !arr { return [0, 0] }
   store64_h(arr, surface, 0)
   if pref == "xcb" && use_xcb { store64_h(arr, xcb, 8) } elif pref == "xlib" || !use_xcb {
      store64_h(arr, xlib, 8)
   } else {
      store64_h(arr, xcb, 8)
      store64_h(arr, xlib, 16)
   }
   [count, arr]
}

fn vulkan_required_extensions() any {
   "Returns the Vulkan instance extensions required for X11 surfaces."
   mut ptrs = _get_x11_val("vk_ext_ptrs", 0)
   if !ptrs {
      def surface = cstr("VK_KHR_surface")
      def xcb = cstr("VK_KHR_xcb_surface")
      def xlib = cstr("VK_KHR_xlib_surface")
      _set_x11_val("vk_ext_surface", surface)
      _set_x11_val("vk_ext_xcb", xcb)
      _set_x11_val("vk_ext_xlib", xlib)
      def _pref = _vk_surface_preference()
      def _use_xcb = true
      ptrs = _vulkan_extension_ptrs(surface, xcb, xlib, _pref, _use_xcb)
      _set_x11_val("vk_ext_ptrs", ptrs)
   }
   ptrs
}

fn xdnd_begin_drag(any win, any data, str mime_type="text/uri-list") bool {
   "Initiates an Xdnd drag from win as source. Returns false if setup fails."
   if !available() || !win || !is_dict(win) { return false }
   def display = win.get("display", 0)
   def handle = win.get("handle", 0)
   if !display || !handle { return false }
   def xdnd_type_list = intern_atom(display, "XdndTypeList")
   def nytrix_dnd_data = intern_atom(display, "_NYTRIX_DND_DATA")
   def xdnd_selection = intern_atom(display, "XdndSelection")
   def mime_atom = intern_atom(display, mime_type)
   if !xdnd_type_list || !mime_atom { return false }
   def type_buf = zalloc(8)
   if !type_buf { return false }
   store32(type_buf, mime_atom, 0)
   XChangeProperty(display, handle, xdnd_type_list, XA_ATOM, 32, PropModeReplace, type_buf, 1)
   free(type_buf)
   if data && is_str(data) && xdnd_selection {
      def cs = cstr(data)
      def utf8 = intern_atom(display, "UTF8_STRING")
      if utf8 { XChangeProperty(display, handle, nytrix_dnd_data, utf8, 8, PropModeReplace, cs, data.len) }
      XSetSelectionOwner(display, xdnd_selection, handle, CurrentTime)
   }
   XGrabPointer(display, handle, 1,
      bor(ButtonPressMask, bor(ButtonReleaseMask, PointerMotionMask)),
   GrabModeAsync, GrabModeAsync, 0, 0, CurrentTime)
   flush(display)
   true
}

fn _handle_xdnd_status(any win, any display, any event_ptr) any {
   if !win || !display || !event_ptr { return win }
   def accepted = band(load64_h(event_ptr, 64), 1) != 0
   win["xdnd_drag_accepted"] = accepted
   win
}

fn _handle_xdnd_finished(any win, any display, any event_ptr) any {
   if !win || !display || !event_ptr { return win }
   def success = band(load64_h(event_ptr, 64), 1) != 0
   win["xdnd_drag_finished"] = success
   XUngrabPointer(display, CurrentTime)
   flush(display)
   win
}

fn set_video_mode(any monitor, int width, int height, int refresh_rate=0, any display=0, any root=0) bool {
   "Sets the video mode for a monitor using XRandR. Returns true on success."
   if !available() || !monitor || !is_dict(monitor) { return false }
   def ctx = _resolve_monitor_context(display, root)
   if !ctx { return false }
   display, root = ctx.get("display", 0), ctx.get("root", 0)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if !resources {
      _release_monitor_context(ctx)
      return false
   }
   def crtc = monitor.get("crtc", 0)
   def output = monitor.get("output", 0)
   if !crtc || !output {
      XRRFreeScreenResources(resources)
      _release_monitor_context(ctx)
      return false
   }
   def crtc_info = XRRGetCrtcInfo(display, resources, crtc)
   if !crtc_info {
      XRRFreeScreenResources(resources)
      _release_monitor_context(ctx)
      return false
   }
   def rotation = load16(crtc_info, 32)
   def cx = load32(crtc_info, 8)
   def cy = load32(crtc_info, 12)
   def noutputs = load32(crtc_info, 36)
   def outputs_ptr = load64_h(crtc_info, 40)
   def mode_count = load32(resources, 48)
   def modes_ptr = load64_h(resources, 56)
   mut best_mode = 0
   mut best_score = -1
   mut mi = 0
   while mi < mode_count {
      def mp, mw = modes_ptr + mi * 80, load32(mp, 8)
      def mh, mr = load32(mp, 12), _refresh_from_mode_info(mp)
      if mw == width && mh == height {
         def score = abs(mr - refresh_rate)
         if best_mode == 0 || score < best_score {
            best_mode = load64_h(mp, 0)
            best_score = score
         }
      }
      mi += 1
   }
   def ok = best_mode != 0
   if ok {
      XRRSetCrtcConfig(display, resources, crtc, 0,
      cx, cy, best_mode, rotation, outputs_ptr, noutputs)
      XRRFreeCrtcInfo(crtc_info)
      flush(display)
   } else {
      XRRFreeCrtcInfo(crtc_info)
   }
   XRRFreeScreenResources(resources)
   _release_monitor_context(ctx)
   ok
}

fn restore_video_mode(any monitor, any display=0, any root=0) bool {
   "Restores the original video mode for a monitor using XRandR."
   if !available() || !monitor || !is_dict(monitor) { return false }
   def orig_mode = monitor.get("mode_id", 0)
   if !orig_mode { return false }
   def w, h = monitor.get("width", 0), monitor.get("height", 0)
   def refresh = monitor.get("refresh_rate", 0)
   set_video_mode(monitor, w, h, refresh, display, root)
}

fn get_x11_monitor(any mon) any {
   "Returns the native X11 output handle from a monitor dict."
   if is_dict(mon) { mon.get("handle", 0) } else { 0 }
}

fn get_x11_adapter(any mon) any {
   "Returns the native X11 CRTC handle from a monitor dict."
   if is_dict(mon) { mon.get("crtc", 0) } else { 0 }
}
