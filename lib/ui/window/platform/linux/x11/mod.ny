;; Keywords: ui window x11
;; Low-level X11 bridge for the in-progress native window backend.

module std.ui.window.platform.linux.x11 (
   available, get_backend_name,
   InputOutput, AllocNone,
   CWBackPixel, CWBorderPixel, CWColormap, CWEventMask,
   ShiftMask, LockMask, ControlMask, Mod1Mask, Mod2Mask, Mod4Mask,
   KeyPressMask, KeyReleaseMask, ButtonPressMask, ButtonReleaseMask,
   EnterWindowMask, LeaveWindowMask, PointerMotionMask,
   ExposureMask, FocusChangeMask, StructureNotifyMask, PropertyChangeMask,
   PropertyNewValue, WithdrawnState, NormalState, IconicState, IsViewable,
   XA_ATOM, XA_CARDINAL, PropModeReplace, PropModeAppend,
   MWM_HINTS_DECORATIONS, MWM_DECOR_ALL,
   PMinSize, PMaxSize, PAspect,
   SubstructureNotifyMask, SubstructureRedirectMask,
   NET_WM_STATE_REMOVE, NET_WM_STATE_ADD, NET_WM_STATE_TOGGLE,
   RRScreenChangeNotifyMask, RRCrtcChangeNotifyMask, RROutputChangeNotifyMask,
   Button1, Button2, Button3, Button4, Button5, Button6, Button7,
   NotifyGrab, NotifyUngrab,
   ClientMessage, ReparentNotify, ConfigureNotify, PropertyNotify, DestroyNotify, Expose, FocusIn, FocusOut,
   KeyPress, KeyRelease, ButtonPress, ButtonRelease, MotionNotify, VisibilityNotify,
   SelectionClear, SelectionRequest, SelectionNotify,
   EnterNotify, LeaveNotify,
   translate_keysym, translate_scancode,
   get_window_property, get_window_state, is_window_iconified,
   get_cardinal_value, get_window_frame_size,
   get_window_size,
   property_has_atom, is_window_visible, is_window_maximized,
   is_window_floating, is_window_fullscreen,
   append_atom_property, remove_atom_property,
   send_client_message, send_wm_state_event,
   iconify_window, show_window, hide_window,
   update_normal_hints, set_window_size, set_size, set_window_size_limits, set_window_aspect_ratio,
   maximize_window, restore_window, set_window_floating, set_window_fullscreen,
   set_window_decorated,
   request_window_attention,
   set_clipboard, get_clipboard,
   set_primary_selection, get_primary_selection,
   get_monitors, get_primary_monitor,
   get_monitor_pos, get_monitor_workarea,
   get_monitor_physical_size, get_monitor_content_scale, get_monitor_name,
   get_x11_monitor, get_x11_adapter,
   get_video_mode, get_video_modes,
   get_window_monitor, set_window_monitor,
   get_key_state, get_mouse_button_state, get_cursor_pos, get_key_name, get_key_scancode, get_size, set_size, get_pos, set_pos, set_input_mode, get_input_mode,
   create_cursor, create_standard_cursor, destroy_cursor, set_cursor,
   translate_event, poll_window_events,
   set_window_opacity, get_window_opacity, get_window_content_scale, set_window_resizable, post_empty_event,
   set_window_icon, focus_window, set_cursor_pos,
   ARROW_CURSOR, IBEAM_CURSOR, CROSSHAIR_CURSOR, POINTING_HAND_CURSOR,
   RESIZE_EW_CURSOR, RESIZE_NS_CURSOR, RESIZE_NWSE_CURSOR, RESIZE_NESW_CURSOR,
   RESIZE_ALL_CURSOR, NOT_ALLOWED_CURSOR,
   wait_events, wait_for_visibility_notify, translate_state,
   create_basic_window, destroy_basic_window,
   set_title, set_window_icon, get_window_attrib,
   open_display, close_display, default_screen, root_window,
   default_visual, default_depth,
   intern_atom, create_colormap, create_window_raw, destroy_window_raw,
   map_window, unmap_window, next_event, pending, select_input,
   flush, sync, store_name, move_window, resize_window, set_wm_protocols,
   create_surface,
   get_gamma_ramp, set_gamma_ramp,
   vulkan_supported, vulkan_required_extensions,
   vulkan_get_surface_capabilities,
   xdnd_begin_drag, _handle_xdnd_status, _handle_xdnd_finished,
   set_video_mode, restore_video_mode
)

use std.core *
use std.core.mem *
use std.os.prim *
use std.os.time *
use std.str as str
use std.ui.window.consts *
use std.ui.window.event as ui_event
use std.ui.window.platform.api *
use std.ui.window.platform.linux.x11.common as glfw_common
use std.ui.window.platform.linux.x11.keymap as x11_keymap
use std.util.common as common

mut _debug = -1
mut _debug_init_done = false

fn _is_debug(){
   if(!_debug_init_done){
      _debug = common.cached_env_truthy(_debug, "NY_UI_DEBUG")
      _debug_init_done = true
      if(_debug){
         print("[x11] NY_UI_DEBUG enabled - verbose X11 logging active")
      }
   }
   _debug
}

fn _dbg(msg){ if(_is_debug()){ print("[x11] " + msg) } }
fn _dbgu(msg){ if(_is_debug()){ print("[x11:v] " + msg) } }
fn _dbg_win(win, msg){ if(_is_debug()){ def h = dict_get(win, "handle", 0) print("[x11] win=0x" + to_hex(h) + " " + msg) } }
fn _dbg_err(msg){ if(_is_debug()){ print("[x11:ERROR] " + msg) } }
fn _dbg_warn(msg){ if(_is_debug()){ print("[x11:WARN] " + msg) } }
fn _dump_window(win){
   if(!_is_debug()){ return }
   def handle = dict_get(win, "handle", 0)
   def display = dict_get(win, "display", 0)
   def screen = dict_get(win, "screen", 0)
   def decorated = dict_get(win, "decorated", true)
   def resizable = dict_get(win, "resizable", true)
   def focused = dict_get(win, "focused", false)
   def visible = dict_get(win, "visible", true)
   def transparent = dict_get(win, "transparent", false)
   def mapped = dict_get(win, "mapped", false)
   def cursor_mode = dict_get(win, "cursor_mode", 0x00034001)

   print("[x11:state] === X11 Window State Dump (0x" + to_hex(handle) + ") ===")
   print("[x11:state]   display: 0x" + to_hex(display))
   print("[x11:state]   screen: " + to_str(screen))
   print("[x11:state]   decorated: " + to_str(decorated))
   print("[x11:state]   resizable: " + to_str(resizable))
   print("[x11:state]   focused: " + to_str(focused))
   print("[x11:state]   visible: " + to_str(visible))
   print("[x11:state]   transparent: " + to_str(transparent))
   print("[x11:state]   mapped: " + to_str(mapped))
   print("[x11:state]   cursor_mode: 0x" + to_hex(cursor_mode))
   print("[x11:state] ===========================================")
}

def InputOutput = 1
def AllocNone = 0

def CWBackPixel = (1 << 1)
def CWBorderPixel = (1 << 3)
def VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR = 1000004000
def CWColormap = (1 << 13)
def CWEventMask = (1 << 11)
def CWOverrideRedirect = (1 << 9)

def ShiftMask = (1 << 0)
def LockMask = (1 << 1)
def ControlMask = (1 << 2)
def Mod1Mask = (1 << 3)
def Mod2Mask = (1 << 4)
def Mod4Mask = (1 << 6)

def KeyPressMask = (1 << 0)
def KeyReleaseMask = (1 << 1)
def ButtonPressMask = (1 << 2)
def ButtonReleaseMask = (1 << 3)
def EnterWindowMask = (1 << 4)
def LeaveWindowMask = (1 << 5)
def PointerMotionMask = (1 << 6)
def ExposureMask = (1 << 15)
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
def StateHint = (1 << 1)
def PPosition = (1 << 2)
def PMinSize = (1 << 4)
def PMaxSize = (1 << 5)
def PAspect = (1 << 7)
def PWinGravity = (1 << 9)
def StaticGravity = 10

def GLFW_CURSOR_MODE = 0x00033001
def GLFW_STICKY_KEYS = 0x00033002
def GLFW_STICKY_MOUSE_BUTTONS = 0x00033003
def GLFW_LOCK_KEY_MODS = 0x00033004
def GLFW_RAW_MOUSE_MOTION = 0x00033005
def GLFW_CURSOR_NORMAL = 0x00034001
def GLFW_CURSOR_HIDDEN = 0x00034002
def GLFW_CURSOR_DISABLED = 0x00034003
def GLFW_CURSOR_CAPTURED = 0x00034004

def ARROW_CURSOR = 0x00036001
def IBEAM_CURSOR = 0x00036002
def CROSSHAIR_CURSOR = 0x00036003
def POINTING_HAND_CURSOR = 0x00036004
def RESIZE_EW_CURSOR = 0x00036005
def RESIZE_NS_CURSOR = 0x00036006
def RESIZE_NWSE_CURSOR = 0x00036007
def RESIZE_NESW_CURSOR = 0x00036008
def RESIZE_ALL_CURSOR = 0x00036009
def NOT_ALLOWED_CURSOR = 0x0003600a

def XC_left_ptr = 68
def XC_xterm = 152
def XC_crosshair = 34
def XC_hand2 = 60
def XC_sb_h_double_arrow = 108
def XC_sb_v_double_arrow = 116
def XC_fleur = 52

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

fn available(){
   "Returns true if the X11 backend is available on Linux with a set DISPLAY."
   if(comptime{ __os_name() != "linux" }){ return false }
   if(env("DISPLAY") == ""){ return false }
   true
}

fn get_backend_name(){
   "Identifies this low-level backend."
   "x11"
}

mut _cursor_specs = dict(32)
mut _cursor_next_id = 1

if(comptime{ __os_name() == "linux" }){
   #link "libX11.so"
   #include <X11/Xlib.h>
   #include <X11/Xutil.h>
   #include <X11/Xatom.h>
   #include <X11/XKBlib.h>
   #link "libXi.so"
   #include <X11/extensions/XInput2.h>
   #link "libXfixes.so"
   #include <X11/extensions/Xfixes.h>
   #link "libXcursor.so"
   #include <X11/Xcursor/Xcursor.h>
   #link "libXrandr.so"
   #include <X11/extensions/Xrandr.h>
   #link "libvulkan.so"
   #include <vulkan/vulkan_xlib.h>
}

mut _x11_threads_init = 0
fn open_display(name=0): ptr {
   if(!available()){
      _dbg_err("open_display: X11 not available (DISPLAY not set or not Linux)")
      return 0
   }
   if(!_x11_threads_init){
      XInitThreads()
      _x11_threads_init = 1
      _dbg("open_display: XInitThreads() called")
   }

   _setup_x11_error_handler()

   def display = name ? XOpenDisplay(name) : XOpenDisplay(0)
   if(!display){
      _dbg_err("open_display: XOpenDisplay() failed!")
      _dbg_err("  DISPLAY=" + env("DISPLAY"))
      return 0
   }

   _dbg("open_display: X display opened successfully: 0x" + to_hex(display))
   return display
}

fn close_display(display){
   "Closes an X display connection."
   if(!display){ return 0 }
   XCloseDisplay(display)
}
fn _open_input_method(display){
   if(!display){ return 0 }
   XSetLocaleModifiers(0)
   XOpenIM(display, 0, 0, 0)
}
fn _create_input_context(im, handle){
   "Creates a UTF-8 capable input context for a specific X11 window."
   if(!im || !handle){ return 0 }
   XCreateIC(im,
      cstr("inputStyle"), XIMPreeditNothing | XIMStatusNothing,
      cstr("clientWindow"), handle,
      cstr("focusWindow"), handle,
      0)
}

fn _emit_utf8_chars(events, win, buffer, count, mods, plain){
   "Decodes a UTF-8 byte buffer and emits EVENT_KEY_CHAR events."
   if(!buffer || count <= 0){ return events }
   mut i = 0
   while(i < count){
      def b0 = load8(buffer, i)
      mut codepoint = -1
      mut next = i + 1
      if(b0 < 0x80){
         codepoint = b0
      } elif(band(b0, 0xe0) == 0xc0 && i + 1 < count){
         codepoint = bor(bshl(band(b0, 0x1f), 6), band(load8(buffer, i + 1), 0x3f))
         next = i + 2
      } elif(band(b0, 0xf0) == 0xe0 && i + 2 < count){
         codepoint = bor(
         bshl(band(b0, 0x0f), 12),
         bor(bshl(band(load8(buffer, i + 1), 0x3f), 6), band(load8(buffer, i + 2), 0x3f))
         )
         next = i + 3
      } elif(band(b0, 0xf8) == 0xf0 && i + 3 < count){
         codepoint = bor(
         bshl(band(b0, 0x07), 18),
         bor(
               bshl(band(load8(buffer, i + 1), 0x3f), 12),
               bor(bshl(band(load8(buffer, i + 2), 0x3f), 6), band(load8(buffer, i + 3), 0x3f))
         )
         )
         next = i + 4
      }

      if(codepoint >= 0){
         mut char_data = dict()
         char_data = dict_set(char_data, "char", codepoint)
         char_data = dict_set(char_data, "mod", mods)
         char_data = dict_set(char_data, "plain", plain)
         _push_translated_event(events, EVENT_KEY_CHAR, win, char_data)
      }
      if(next > i){ i = next }
      else { i += 1 }
   }
   events
}

fn _emit_ic_chars(events, win, event_ptr, mods, plain){
   "Uses Xutf8LookupString to translate the current keypress into UTF-8 characters."
   def ic = dict_get(win, "ic", 0)
   if(!ic){ return events }

   def status_ptr = malloc(4)
   def keysym_ptr = malloc(8)
   mut buffer = malloc(128)
   mut buffer_cap = 127
   if(!status_ptr || !keysym_ptr || !buffer){
      if(status_ptr){ free(status_ptr) }
      if(keysym_ptr){ free(keysym_ptr) }
      if(buffer){ free(buffer) }
      return events
   }

   store32(status_ptr, 0, 0)
   store64_h(keysym_ptr, 0, 0)
   mut count = Xutf8LookupString(ic, event_ptr, buffer, buffer_cap, keysym_ptr, status_ptr)
   mut status = load32(status_ptr, 0)

   if(status == XBufferOverflow && count > 0){
      free(buffer)
      buffer = malloc(count + 1)
      if(!buffer){
         free(status_ptr)
         free(keysym_ptr)
         return events
      }
      buffer_cap = count
      store32(status_ptr, 0, 0)
      store64_h(keysym_ptr, 0, 0)
      count = Xutf8LookupString(ic, event_ptr, buffer, buffer_cap, keysym_ptr, status_ptr)
      status = load32(status_ptr, 0)
   }

   if(count > 0 && (status == XLookupChars || status == XLookupBoth)){
      store8(buffer, 0, count)
      events = _emit_utf8_chars(events, win, buffer, count, mods, plain)
   }

   free(status_ptr)
   free(keysym_ptr)
   free(buffer)
   events
}

fn default_screen(display){
   "Returns the default X11 screen index."
   XDefaultScreen(display)
}

fn root_window(display, screen_number){
   "Returns the root window for `screen_number`."
   XRootWindow(display, screen_number)
}

fn default_visual(display, screen_number){
   "Returns the default visual pointer for `screen_number`."
   XDefaultVisual(display, screen_number)
}

fn default_depth(display, screen_number){
   "Returns the default depth for `screen_number`."
   XDefaultDepth(display, screen_number)
}

fn intern_atom(display, atom_name, only_if_exists=false){
   "Interns an X11 atom and returns its id."
   XInternAtom(display, cstr(atom_name), only_if_exists ? 1 : 0)
}

fn create_colormap(display, win, visual, alloc=AllocNone){
   "Creates a colormap for a native X11 window."
   XCreateColormap(display, win, visual, alloc)
}

fn create_window_raw(display, parent, x, y, width, height, border_width, depth, klass, visual, value_mask, attributes){
   "Creates a raw X11 window."
   XCreateWindow(display, parent, x, y, width, height, border_width, depth, klass, visual, value_mask, attributes)
}

fn destroy_window_raw(display, win){
   "Destroys a raw X11 window."
   XDestroyWindow(display, win)
}

fn map_window(display, win){
   "Maps a raw X11 window."
   XMapWindow(display, win)
}

fn unmap_window(display, win){
   "Unmaps a raw X11 window."
   XUnmapWindow(display, win)
}

fn pending(display){
   "Returns the number of queued X11 events."
   XPending(display)
}

fn get_window_property(display, win, property, typ, long_length=4096){
   "Direct Ny adaptation of GLFW `_glfwGetWindowPropertyX11`."
   if(!display || !win || !property){ return false }

   def actual_type = malloc(8)
   def actual_format = malloc(4)
   def nitems = malloc(8)
   def bytes_after = malloc(8)
   def prop = malloc(8)
   if(!actual_type || !actual_format || !nitems || !bytes_after || !prop){
      if(actual_type){ free(actual_type) }
      if(actual_format){ free(actual_format) }
      if(nitems){ free(nitems) }
      if(bytes_after){ free(bytes_after) }
      if(prop){ free(prop) }
      return false
   }

   store64_h(prop, 0, 0)
   XGetWindowProperty(display, win, property, 0, long_length, 0, typ,
      actual_type, actual_format, nitems, bytes_after, prop)

   mut out = dict()
   out = dict_set(out, "data", load64_h(prop, 0))
   out = dict_set(out, "type", load32(actual_type, 0))
   out = dict_set(out, "format", load32(actual_format, 0))
   out = dict_set(out, "count", load32(nitems, 0))
   out = dict_set(out, "bytes_after", load32(bytes_after, 0))

   free(actual_type)
   free(actual_format)
   free(nitems)
   free(bytes_after)
   free(prop)
   out
}

fn get_window_state(display, win, wm_state_atom){
   "Direct Ny port of GLFW `getWindowState` helper."
   if(!display || !win || !wm_state_atom){ return WithdrawnState }
   def prop = get_window_property(display, win, wm_state_atom, wm_state_atom)
   if(!prop || !is_dict(prop)){ return WithdrawnState }

   def data = dict_get(prop, "data", 0)
   def count = dict_get(prop, "count", 0)
   mut result = WithdrawnState
   if(data && count >= 2){
      result = load32(data, 0)
   }
   if(data){ XFree(data) }
   result
}

fn is_window_iconified(display, win, wm_state_atom){
   "Returns true when the window is in X11 `IconicState`."
   get_window_state(display, win, wm_state_atom) == IconicState
}

fn get_cardinal_value(display, win, property, index=0){
   "Returns the CARDINAL value at `index` for `property`, or false if missing."
   if(!display || !win || !property || index < 0){ return false }
   def prop = get_window_property(display, win, property, XA_CARDINAL)
   if(!prop || !is_dict(prop)){ return false }

   def data = dict_get(prop, "data", 0)
   def count = dict_get(prop, "count", 0)
   mut value = false
   if(data && index < count){
      value = load64_h(data, index * 8)
   }
   if(data){ XFree(data) }
   value
}

fn property_has_atom(display, win, property, atom){
   "Returns true when `property` on `win` contains `atom`."
   if(!display || !win || !property || !atom){ return false }
   def prop = get_window_property(display, win, property, XA_ATOM)
   if(!prop || !is_dict(prop)){ return false }

   def data = dict_get(prop, "data", 0)
   def count = dict_get(prop, "count", 0)
   mut found = false
   if(data){
      mut i = 0
      while(i < count){
         if(load32(data, i * 8) == atom){
         found = true
         break
         }
         i += 1
      }
      XFree(data)
   }
   found
}

fn is_window_maximized(display, win, net_wm_state_atom, max_vert_atom, max_horz_atom){
   "Returns true when both maximized state atoms are present."
   property_has_atom(display, win, net_wm_state_atom, max_vert_atom) &&
      property_has_atom(display, win, net_wm_state_atom, max_horz_atom)
}

fn is_window_visible(display, win){
   "Returns true when the X11 window is currently viewable."
   if(!display || !win){ return false }
   def attrs = calloc(1, 256)
   if(!attrs){ return false }
   def ok = XGetWindowAttributes(display, win, attrs) != 0
   mut visible = false
   if(ok){
      ;; XWindowAttributes.map_state
      visible = load32(attrs, 136) == IsViewable
   }
   free(attrs)
   visible
}

fn get_window_size(display, win){
   "Returns `{ width, height }` based on `XGetWindowAttributes`."
   if(!display || !win){ return false }
   def attrs = calloc(1, 256)
   if(!attrs){ return false }
   def ok = XGetWindowAttributes(display, win, attrs) != 0
   if(!ok){
      free(attrs)
      return false
   }

   mut out = dict()
   ;; XWindowAttributes: int x@0, y@4, width@8, height@12
   out = dict_set(out, "width", load32(attrs, 8))
   out = dict_set(out, "height", load32(attrs, 12))
   free(attrs)
   out
}

fn is_window_floating(display, win, net_wm_state_atom, above_atom){
   "Returns true when `_NET_WM_STATE_ABOVE` is present."
   property_has_atom(display, win, net_wm_state_atom, above_atom)
}

fn is_window_fullscreen(display, win, net_wm_state_atom, fullscreen_atom){
   "Returns true when `_NET_WM_STATE_FULLSCREEN` is present."
   property_has_atom(display, win, net_wm_state_atom, fullscreen_atom)
}

fn append_atom_property(display, win, property, atom){
   "Appends `atom` to an X11 atom-list property if it is not already present."
   if(!display || !win || !property || !atom){ return false }
   if(property_has_atom(display, win, property, atom)){ return true }
   def value = calloc(1, 8)
   if(!value){ return false }
   store32(value, atom, 0)
   def ok = XChangeProperty(display, win, property, XA_ATOM, 32, PropModeAppend, value, 1) == 0
   free(value)
   ok
}

fn remove_atom_property(display, win, property, atom){
   "Removes `atom` from an X11 atom-list property."
   if(!display || !win || !property || !atom){ return false }
   def prop = get_window_property(display, win, property, XA_ATOM)
   if(!prop || !is_dict(prop)){ return false }

   def data = dict_get(prop, "data", 0)
   def count = dict_get(prop, "count", 0)
   if(!data || count <= 0){
      if(data){ XFree(data) }
      return false
   }

   mut found = -1
   mut i = 0
   while(i < count){
      if(load32(data, i * 8) == atom){
         found = i
         break
      }
      i += 1
   }

   if(found < 0){
      XFree(data)
      return false
   }

   if(count == 1){
      def ok_empty = XChangeProperty(display, win, property, XA_ATOM, 32, PropModeReplace, 0, 0) == 0
      XFree(data)
      return ok_empty
   }

   store32(data, load32(data, (count - 1) * 8), found * 8)
   def ok = XChangeProperty(display, win, property, XA_ATOM, 32, PropModeReplace, data, count - 1) == 0
   XFree(data)
   ok
}

fn send_client_message(display, root, win, atom, d0, d1=0, d2=0, d3=0, d4=0){
   "Sends a 32-bit X11 ClientMessage event."
   if(!display || !root || !win || !atom){ return false }
   def ev = malloc(96)
   if(!ev){ return false }
   memset(ev, 0, 96)

   store32(ev, ClientMessage, 0)
   store32(ev, 32, 28)
   store64_h(ev, win, 32)
   store64_h(ev, atom, 40)
   store64_h(ev, d0, 48)
   store64_h(ev, d1, 56)
   store64_h(ev, d2, 64)
   store64_h(ev, d3, 72)
   store64_h(ev, d4, 80)

   def mask = bor(SubstructureNotifyMask, SubstructureRedirectMask)
   def ok = XSendEvent(display, root, 0, mask, ev) != 0
   free(ev)
   ok
}

fn send_wm_state_event(display, root, win, net_wm_state_atom, action, first_atom, second_atom=0, source_indication=1){
   "Sends an EWMH `_NET_WM_STATE` client message."
   send_client_message(display, root, win, net_wm_state_atom,
      action, first_atom, second_atom, source_indication, 0)
}

fn _iconify_window_raw(display, handle, screen_number){
   def ok = XIconifyWindow(display, handle, screen_number) != 0
   flush(display)
   ok
}

fn iconify_window(win){
   "Direct Ny port of GLFW `_glfwIconifyWindowX11` core action."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def screen_number = dict_get(win, "screen", 0)
   def override_redirect = dict_get(win, "override_redirect", false)
   if(!display || !handle){ return false }
   if(override_redirect){ return false }
   _iconify_window_raw(display, handle, screen_number)
}

fn _show_window_raw(display, handle, floating=false, net_wm_state_atom=0, above_atom=0){
   if(is_window_visible(display, handle)){ return true }
   if(floating && net_wm_state_atom && above_atom){
      append_atom_property(display, handle, net_wm_state_atom, above_atom)
   }
   XMapWindow(display, handle)
   flush(display)
   wait_for_visibility_notify(display, handle, 100)
}

fn show_window(win){
   "Direct Ny port of GLFW `_glfwShowWindowX11` core behavior."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def floating = dict_get(win, "floating", false)
   def net_wm_state_atom = dict_get(win, "net_wm_state", 0)
   def above_atom = dict_get(win, "net_wm_state_above", 0)
   if(!display || !handle){ return false }
   _show_window_raw(display, handle, floating, net_wm_state_atom, above_atom)
}

fn _hide_window_raw(display, handle){
   XUnmapWindow(display, handle)
   flush(display)
   true
}

fn hide_window(win){
   "Direct Ny port of GLFW `_glfwHideWindowX11`."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return false }
   _hide_window_raw(display, handle)
}

fn update_normal_hints(display, win, width, height, resizable=true, monitor=false,
   minwidth=-1, minheight=-1, maxwidth=-1, maxheight=-1, numer=-1, denom=-1){
   "Direct Ny port of GLFW `updateNormalHints` core behavior."
   if(!display || !win){ return false }
   def hints = XAllocSizeHints()
   if(!hints){ return false }

   def supplied = calloc(1, 8)
   if(!supplied){
      XFree(hints)
      return false
   }

   XGetWMNormalHints(display, win, hints, supplied)
   store64_h(hints, 0, band(load64_h(hints, 0), bnot(bor(bor(PMinSize, PMaxSize), PAspect))))

   if(!monitor){
      if(resizable){
         if(minwidth >= 0 && minheight >= 0){
         store64_h(hints, 0, bor(load64_h(hints, 0), PMinSize))
         store32(hints, minwidth, 24)
         store32(hints, minheight, 28)
         }

         if(maxwidth >= 0 && maxheight >= 0){
         store64_h(hints, 0, bor(load64_h(hints, 0), PMaxSize))
         store32(hints, maxwidth, 32)
         store32(hints, maxheight, 36)
         }

         if(numer >= 0 && denom >= 0){
         store64_h(hints, 0, bor(load64_h(hints, 0), PAspect))
         store32(hints, numer, 48)
         store32(hints, denom, 52)
         store32(hints, numer, 56)
         store32(hints, denom, 60)
         }
      } else {
         store64_h(hints, 0, bor(load64_h(hints, 0), bor(PMinSize, PMaxSize)))
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

fn _set_window_manager_hints(display, win){
   "Sets the ICCCM WM_HINTS state for a newly created top-level window."
   if(!display || !win){ return false }
   def hints = XAllocWMHints()
   if(!hints){ return false }
   store32(hints, StateHint, 0)
   store32(hints, NormalState, 12)
   XSetWMHints(display, win, hints)
   XFree(hints)
   true
}

fn _set_initial_normal_hints(display, win, width, height, resizable=true, xpos=0, ypos=0, honor_position=false){
   "Initial X11 normal hints matching GLFW createNativeWindow setup."
   if(!display || !win){ return false }
   def hints = XAllocSizeHints()
   if(!hints){ return false }
   store32(hints, 0, 0)
   if(!resizable){
      store32(hints, bor(load32(hints, 0), bor(PMinSize, PMaxSize)), 0)
      store32(hints, width, 24)
      store32(hints, height, 28)
      store32(hints, width, 32)
      store32(hints, height, 36)
   }
   if(honor_position){
      store32(hints, bor(load32(hints, 0), PPosition), 0)
      store32(hints, 0, 8)
      store32(hints, 0, 12)
   }
   store32(hints, bor(load32(hints, 0), PWinGravity), 0)
   store32(hints, StaticGravity, 72)
   XSetWMNormalHints(display, win, hints)
   XFree(hints)
   true
}

fn _set_cardinal_property(display, win, property_atom, value){
   "Writes a 32-bit CARDINAL property."
   if(!display || !win || !property_atom){ return false }
   def data = calloc(1, 8)
   if(!data){ return false }
   store32(data, value, 0)
   XChangeProperty(display, win, property_atom, XA_CARDINAL, 32, PropModeReplace, data, 1)
   free(data)
   true
}

fn _set_atom_property(display, win, property_atom, value_atom){
   "Writes a single ATOM-valued property."
   if(!display || !win || !property_atom || !value_atom){ return false }
      def data = calloc(1, 8)
   if(!data){ return false }
   store32(data, value_atom, 0)
   XChangeProperty(display, win, property_atom, XA_ATOM, 32, PropModeReplace, data, 1)
   free(data)
   true
}

fn _set_window_pid(display, win, net_wm_pid_atom){
   "Publishes the current process id via _NET_WM_PID."
   if(!display || !win || !net_wm_pid_atom){ return false }
   _set_cardinal_property(display, win, net_wm_pid_atom, pid())
}

fn _set_window_type_normal(display, win, net_wm_window_type_atom, net_wm_window_type_normal_atom){
   "Marks the window as a normal EWMH toplevel."
   if(!display || !win || !net_wm_window_type_atom || !net_wm_window_type_normal_atom){ return false }
   _set_atom_property(display, win, net_wm_window_type_atom, net_wm_window_type_normal_atom)
}

fn _set_compositor_bypass(display, win, bypass_atom, enabled){
   "Sets or clears `_NET_WM_BYPASS_COMPOSITOR`."
   if(!display || !win || !bypass_atom){ return false }
   if(enabled){
      return _set_cardinal_property(display, win, bypass_atom, 1)
   }
   XDeleteProperty(display, win, bypass_atom)
   flush(display)
   true
}

fn _set_fullscreen_monitors(display, root, win, atom, monitor){
   "Publishes `_NET_WM_FULLSCREEN_MONITORS` for a monitor-backed fullscreen window."
   if(!display || !root || !win || !atom){ return false }
   if(!monitor || !is_dict(monitor)){
      XDeleteProperty(display, win, atom)
      flush(display)
      return true
   }
   def index = int(dict_get(monitor, "index", -1))
   if(index < 0){ return false }
   def ok = send_client_message(display, root, win, atom, index, index, index, index, 0)
   flush(display)
   ok
}

fn _set_class_hint(display, win, res_name, res_class){
   "Sets ICCCM WM_CLASS following GLFW rules."
   if(!display || !win){ return false }
   def hint = XAllocClassHint()
   if(!hint){ return false }
   store64_h(hint, cstr(res_name), 0)
   store64_h(hint, cstr(res_class), 8)
   XSetClassHint(display, win, hint)
   XFree(hint)
   true
}

fn _reply_wm_ping(display, root, event_ptr){
   "Replies to an EWMH _NET_WM_PING request."
   if(!display || !root || !event_ptr){ return false }
   def reply = calloc(1, 96)
   if(!reply){ return false }
   memcpy(reply, event_ptr, 96)
   store64_h(reply, root, 32)
   XSendEvent(display, root, 0,
      SubstructureNotifyMask | SubstructureRedirectMask,
      reply)
   free(reply)
   true
}

fn _set_override_redirect(display, win, enabled){
   "Toggles X11 override-redirect for fullscreen fallback when EWMH is unavailable."
   if(!display || !win){ return false }
   def attrs = calloc(1, 112)
   if(!attrs){ return false }
   store32(attrs, enabled ? 1 : 0, 88)
   def ok = XChangeWindowAttributes(display, win, CWOverrideRedirect, attrs) == 0
   free(attrs)
   flush(display)
   ok
}

fn set_size(win, w, h){
   "Sets the X11 window size."
   if(!win || !is_dict(win)){ return false }
   set_window_size(dict_get(win, "display", 0), dict_get(win, "handle", 0), w, h)
}

fn set_window_size(display, win, width, height, resizable=true, monitor=false){
   "Direct Ny port of GLFW `_glfwSetWindowSizeX11` core behavior."
   if(!display || !win){ return false }
   width = max(1, width)
   height = max(1, height)

   if(!monitor){
      if(!resizable){
         update_normal_hints(display, win, width, height, false, false)
      }
      resize_window(display, win, width, height)
   }

   flush(display)
   true
}

fn set_window_size_limits(display, win, minwidth=-1, minheight=-1, maxwidth=-1, maxheight=-1,
   resizable=true, monitor=false, numer=-1, denom=-1){
   "Direct Ny port of GLFW `_glfwSetWindowSizeLimitsX11` core behavior."
   if(!display || !win){ return false }
   def size = get_window_size(display, win)
   if(!size || !is_dict(size)){ return false }
   def width = dict_get(size, "width", 0)
   def height = dict_get(size, "height", 0)
   def ok = update_normal_hints(display, win, width, height, resizable, monitor,
      minwidth, minheight, maxwidth, maxheight, numer, denom)
   flush(display)
   ok
}

fn set_window_aspect_ratio(display, win, numer, denom, resizable=true, monitor=false,
   minwidth=-1, minheight=-1, maxwidth=-1, maxheight=-1){
   "Direct Ny port of GLFW `_glfwSetWindowAspectRatioX11` core behavior."
   if(!display || !win){ return false }
   def size = get_window_size(display, win)
   if(!size || !is_dict(size)){ return false }
   def width = dict_get(size, "width", 0)
   def height = dict_get(size, "height", 0)
   def ok = update_normal_hints(display, win, width, height, resizable, monitor,
      minwidth, minheight, maxwidth, maxheight, numer, denom)
   flush(display)
   ok
}

fn _maximize_window_raw(display, root, handle, net_wm_state_atom, max_vert_atom, max_horz_atom){
   if(is_window_visible(display, handle)){
      def ok_visible = send_wm_state_event(display, root, handle, net_wm_state_atom,
         NET_WM_STATE_ADD, max_vert_atom, max_horz_atom)
      flush(display)
      return ok_visible
   }
   def ok_vert = append_atom_property(display, handle, net_wm_state_atom, max_vert_atom)
   def ok_horz = append_atom_property(display, handle, net_wm_state_atom, max_horz_atom)
   flush(display)
   ok_vert && ok_horz
}

fn maximize_window(win){
   "Direct Ny port of GLFW `_glfwMaximizeWindowX11` core behavior."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def root = dict_get(win, "root", 0)
   def net_wm_state_atom = dict_get(win, "net_wm_state", 0)
   def max_vert_atom = dict_get(win, "net_wm_state_maximized_vert", 0)
   def max_horz_atom = dict_get(win, "net_wm_state_maximized_horz", 0)
   if(!display || !root || !handle || !net_wm_state_atom || !max_vert_atom || !max_horz_atom){ return false }
   _maximize_window_raw(display, root, handle, net_wm_state_atom, max_vert_atom, max_horz_atom)
}

fn _restore_window_raw(display, root, handle, wm_state_atom, net_wm_state_atom, max_vert_atom=0, max_horz_atom=0){
   if(wm_state_atom && is_window_iconified(display, handle, wm_state_atom)){
      XMapWindow(display, handle)
      flush(display)
      return true
   }
   if(root && is_window_visible(display, handle) && net_wm_state_atom && max_vert_atom && max_horz_atom){
      def ok = send_wm_state_event(display, root, handle, net_wm_state_atom,
         NET_WM_STATE_REMOVE, max_vert_atom, max_horz_atom)
      flush(display)
      return ok
   }
   flush(display)
   true
}

fn restore_window(win){
   "Direct Ny port of GLFW `_glfwRestoreWindowX11` core behavior."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def root = dict_get(win, "root", 0)
   def wm_state_atom = dict_get(win, "wm_state", 0)
   def net_wm_state_atom = dict_get(win, "net_wm_state", 0)
   def max_vert_atom = dict_get(win, "net_wm_state_maximized_vert", 0)
   def max_horz_atom = dict_get(win, "net_wm_state_maximized_horz", 0)
   def override_redirect = dict_get(win, "override_redirect", false)
   if(!display || !handle){ return false }
   if(override_redirect){ return false }
   _restore_window_raw(display, root, handle, wm_state_atom, net_wm_state_atom, max_vert_atom, max_horz_atom)
}

fn _set_window_floating_raw(display, root, win, net_wm_state_atom, above_atom, enabled){
   "Direct Ny port of GLFW `_glfwSetWindowFloatingX11` core behavior."
   if(!display || !win || !net_wm_state_atom || !above_atom){ return false }
   if(is_window_visible(display, win)){
      if(!root){ return false }
      def action = enabled ? NET_WM_STATE_ADD : NET_WM_STATE_REMOVE
      def ok = send_wm_state_event(display, root, win, net_wm_state_atom, action, above_atom)
      flush(display)
      return ok
   }

   ;; GLFW defers adding ABOVE until show-time for unmapped windows.
   if(enabled){
      flush(display)
      return true
   }

   def ok_remove = remove_atom_property(display, win, net_wm_state_atom, above_atom)
   flush(display)
   ok_remove
}

fn set_window_fullscreen(display, root, win, net_wm_state_atom, fullscreen_atom, enabled){
   "Sets `_NET_WM_STATE_FULLSCREEN` via EWMH client messages."
   if(!display || !root || !win || !net_wm_state_atom || !fullscreen_atom){ return false }
   def action = enabled ? NET_WM_STATE_ADD : NET_WM_STATE_REMOVE
   def ok = send_wm_state_event(display, root, win, net_wm_state_atom, action, fullscreen_atom)
   flush(display)
   ok
}

fn _set_window_decorated_raw(display, win, motif_wm_hints_atom, enabled){
   "Direct Ny port of GLFW `_glfwSetWindowDecoratedX11`."
   if(!display || !win || !motif_wm_hints_atom){ return false }
   def hints = calloc(5, 8)
   if(!hints){ return false }

   store32(hints, MWM_HINTS_DECORATIONS, 0)
   store32(hints, enabled ? MWM_DECOR_ALL : 0, 16)

   def ok = XChangeProperty(display, win, motif_wm_hints_atom, motif_wm_hints_atom, 32,
      PropModeReplace, hints, 5) == 0
   free(hints)
   ok
}

fn set_window_resizable(win, enabled){
   "Toggles the resizable state of an X11 window."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return false }
   def sz = get_size(win)
   update_normal_hints(display, handle, get(sz, 0), get(sz, 1), enabled, false)
   XFlush(display)
   true
}

fn set_window_decorated(win, enabled){
   "Toggles window decorations for an X11 window."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def motif = dict_get(win, "motif_wm_hints_atom", 0)
   if(!display || !handle || !motif){ return false }
   def ok = _set_window_decorated_raw(display, handle, motif, enabled)
   XFlush(display)
   ok
}

fn set_window_floating(win, enabled){
   "Toggles the always-on-top state of an X11 window."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def root = dict_get(win, "root", 0)
   def state = dict_get(win, "net_wm_state_atom", 0)
   def above = dict_get(win, "net_wm_state_above_atom", 0)
   if(!display || !handle || !state || !above){ return false }
   def ok = _set_window_floating_raw(display, root, handle, state, above, enabled)
   XFlush(display)
   ok
}

fn set_window_mouse_passthrough(win, enabled){
   "Sets mouse passthrough mode for an X11 window (click-through)."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return false }

   ;; ShapeInput=2, ShapeSet=0, Unsorted=0
   if(enabled){
      ;; Create empty region - no mouse input
      XShapeCombineRectangles(display, handle, 2, 0, 0, 0, 0, 0, 0, 0)
   } else {
      ;; Reset to full window - normal input
      XShapeCombineRectangles(display, handle, 2, 0, 0, 0, 0, 1, 0, 0)
   }
   XFlush(display)

   win = dict_set(win, "mouse_passthrough", enabled)
   true
}

fn set_window_size_limits(win, min_w, min_h, max_w, max_h){
   "Sets size limits for an X11 window via WM_NORMAL_HINTS."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return false }

   def sz = get_size(win)
   def w = get(sz, 0)
   def h = get(sz, 1)

   update_normal_hints(display, handle, w, h, true, false,
      min_w >= 0 ? min_w : -1, min_h >= 0 ? min_h : -1,
      max_w >= 0 ? max_w : -1, max_h >= 0 ? max_h : -1)
   XFlush(display)
   true
}

fn set_window_aspect_ratio(win, numer, denom){
   "Sets aspect ratio for an X11 window via WM_NORMAL_HINTS."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return false }

   def sz = get_size(win)
   def w = get(sz, 0)
   def h = get(sz, 1)

   update_normal_hints(display, handle, w, h, true, false,
      -1, -1, -1, -1,
      numer >= 0 ? numer : -1, denom >= 0 ? denom : -1)
   XFlush(display)
   true
}

fn set_window_opacity(win, opacity){
   "Sets `_NET_WM_WINDOW_OPACITY` when supported by the window manager."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def net_wm_window_opacity_atom = dict_get(win, "net_wm_window_opacity", 0)
   if(!display || !handle || !net_wm_window_opacity_atom){ return false }
   if(opacity < 0.0){ opacity = 0.0 }
   if(opacity > 1.0){ opacity = 1.0 }

   def value = calloc(1, 8)
   if(!value){ return false }

   def scaled = int(opacity * 4294967295.0 + 0.5)
   store32(value, scaled, 0)
   def ok = XChangeProperty(display, handle, net_wm_window_opacity_atom, XA_CARDINAL, 32,
       PropModeReplace, value, 1) == 0
   free(value)
   flush(display)
   ok
}

fn get_window_opacity(win){
   "Returns the window opacity from _NET_WM_WINDOW_OPACITY (1.0 if not set)."
   if(!win || !is_dict(win)){ return 1.0 }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def opacity_atom = dict_get(win, "net_wm_window_opacity", 0)
   if(!display || !handle || !opacity_atom){ return 1.0 }
   def prop = get_window_property(display, handle, opacity_atom, XA_CARDINAL)
   if(!prop || !is_dict(prop)){ return 1.0 }
   def data_ptr = dict_get(prop, "data", 0)
   def nitems = dict_get(prop, "nitems", 0)
   if(!data_ptr || nitems < 1){
      if(data_ptr){ XFree(data_ptr) }
      return 1.0
   }
   def raw = load32(data_ptr, 0)
   XFree(data_ptr)
   float(raw) / 4294967295.0
}

fn get_window_content_scale(win){
   "Returns [xscale, yscale] from the cached scale values."
   if(!win || !is_dict(win)){ return [1.0, 1.0] }
   [dict_get(win, "scale_x", 1.0), dict_get(win, "scale_y", 1.0)]
}

fn get_content_scale(win){ get_window_content_scale(win) }

fn _focus_window_raw(display, win){
   XRaiseWindow(display, win)
   XSetInputFocus(display, win, RevertToParent, CurrentTime)
   flush(display)
}

fn focus_window(win){
   "Raises and focuses an X11 toplevel window (only if mapped)."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return false }
   if(!dict_get(win, "mapped", false)){ return false }
   _focus_window_raw(display, handle)
   true
}

fn _set_cursor_pos_raw(display, win, x, y){
   XWarpPointer(display, 0, win, 0, 0, 0, 0, int(x), int(y))
   flush(display)
}

fn set_cursor_pos(win, x, y){
   "Warps the cursor to window-local coordinates."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return false }
   _set_cursor_pos_raw(display, handle, x, y)
   true
}

fn get_cursor_pos(win){
   "Queries the current pointer position relative to the X11 window."
   if(!win || !is_dict(win)){ return [0.0, 0.0] }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return [0.0, 0.0] }
   def root = malloc(8)
   def child = malloc(8)
   def root_x = malloc(4)
   def root_y = malloc(4)
   def win_x = malloc(4)
   def win_y = malloc(4)
   def mask = malloc(4)
   if(!root || !child || !root_x || !root_y || !win_x || !win_y || !mask){
      if(root){ free(root) }
      if(child){ free(child) }
      if(root_x){ free(root_x) }
      if(root_y){ free(root_y) }
      if(win_x){ free(win_x) }
      if(win_y){ free(win_y) }
      if(mask){ free(mask) }
      return [0.0, 0.0]
   }

   mut out = [0.0, 0.0]
   if(XQueryPointer(display, handle, root, child, root_x, root_y, win_x, win_y, mask) != 0){
      out = [float(load32(win_x, 0)), float(load32(win_y, 0))]
   }
   free(root) free(child) free(root_x) free(root_y) free(win_x) free(win_y) free(mask)
   out
}

fn _capture_cursor(display, win){
   "Confines the pointer to `win`, following GLFW X11 capture path."
   if(!display || !win){ return false }
   XGrabPointer(display, win, 1,
      ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
      GrabModeAsync, GrabModeAsync, win, 0, CurrentTime) == GrabSuccess
}

fn _release_cursor(display){
   "Releases any X11 pointer grab owned by this process."
   if(!display){ return false }
   XUngrabPointer(display, CurrentTime)
   flush(display)
   true
}

fn _set_cursor_visibility(display, win, visible){
   "Shows or hides the X11 cursor for `win` using XFixes."
   if(!display || !win){ return false }
   if(visible){ XFixesShowCursor(display, win) }
   else { XFixesHideCursor(display, win) }
   flush(display)
   true
}

fn get_key_state(win, key){
   "Returns GLFW-style key state for the cached native window state."
   if(!win || !is_dict(win)){ return 0 }
   dict_get(dict_get(win, "key_states", 0), key, false) ? 1 : 0
}

fn get_mouse_button_state(win, button){
   "Returns GLFW-style mouse button state for the cached native window state."
   if(!win || !is_dict(win)){ return 0 }
   dict_get(dict_get(win, "mouse_buttons", 0), button, false) ? 1 : 0
}

fn set_input_mode(win, mode, value){
   "Applies GLFW-compatible cursor/raw-mouse modes to the native X11 window state."
   if(!win || !is_dict(win)){ return win }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return win }

   if(mode == GLFW_RAW_MOUSE_MOTION){
      win = dict_set(win, "raw_mouse_motion", value != 0)
      if(dict_get(win, "disabled_cursor", false) && dict_get(win, "xi_available", false)){
         _xi_set_raw_motion_enabled(display, dict_get(win, "root", 0), value != 0)
      }
      return win
   }
   if(mode == GLFW_STICKY_KEYS){
      win = dict_set(win, "sticky_keys", value != 0)
      if(value == 0){
         ;; Clear sticky key states when disabling
         mut key_states = dict_get(win, "key_states", dict())
         def keys = dict_keys(key_states)
         mut i = 0
         while(i < len(keys)){
         def key = get(keys, i)
         def state = dict_get(key_states, key, 0)
         if(state == 3){ ;; _GLFW_STICK
               key_states = dict_set(key_states, key, 0) ;; GLFW_RELEASE
         }
         i += 1
         }
         win = dict_set(win, "key_states", key_states)
      }
      return win
   }
   if(mode == GLFW_STICKY_MOUSE_BUTTONS){
      win = dict_set(win, "sticky_mouse_buttons", value != 0)
      if(value == 0){
         ;; Clear sticky button states when disabling
         mut btn_states = dict_get(win, "mouse_buttons", dict())
         def btns = dict_keys(btn_states)
         mut i = 0
         while(i < len(btns)){
         def btn = get(btns, i)
         def state = dict_get(btn_states, btn, 0)
         if(state == 3){ ;; _GLFW_STICK
               btn_states = dict_set(btn_states, btn, 0) ;; GLFW_RELEASE
         }
         i += 1
         }
         win = dict_set(win, "mouse_buttons", btn_states)
      }
      return win
   }
   if(mode == GLFW_LOCK_KEY_MODS){
      win = dict_set(win, "lock_key_mods", value != 0)
      return win
   }
   if(mode != GLFW_CURSOR_MODE){ return win }

   def previous = dict_get(win, "cursor_mode", GLFW_CURSOR_NORMAL)
   win = dict_set(win, "cursor_mode", value)

   if(value == GLFW_CURSOR_NORMAL){
      if(dict_get(win, "xi_available", false) && dict_get(win, "raw_mouse_motion", false)){
         _xi_set_raw_motion_enabled(display, dict_get(win, "root", 0), false)
      }
      _release_cursor(display)
      _set_cursor_visibility(display, handle, true)
      win = _apply_window_cursor(win)
      if(previous == GLFW_CURSOR_DISABLED){
         def rx = dict_get(win, "restore_cursor_x", dict_get(win, "mouse_x", 0))
         def ry = dict_get(win, "restore_cursor_y", dict_get(win, "mouse_y", 0))
         _set_cursor_pos_raw(display, handle, rx, ry)
         win = dict_set(win, "mouse_x", rx)
         win = dict_set(win, "mouse_y", ry)
      }
      win = dict_set(win, "captured_cursor", false)
      win = dict_set(win, "disabled_cursor", false)
      win = dict_set(win, "ignore_warp_motion", false)
      return win
   }

   if(value == GLFW_CURSOR_HIDDEN){
      if(dict_get(win, "xi_available", false) && dict_get(win, "raw_mouse_motion", false)){
         _xi_set_raw_motion_enabled(display, dict_get(win, "root", 0), false)
      }
      _release_cursor(display)
      _set_cursor_visibility(display, handle, false)
      win = dict_set(win, "captured_cursor", false)
      win = dict_set(win, "disabled_cursor", false)
      win = dict_set(win, "ignore_warp_motion", false)
      return win
   }

   if(value == GLFW_CURSOR_CAPTURED){
      if(dict_get(win, "xi_available", false) && dict_get(win, "raw_mouse_motion", false)){
         _xi_set_raw_motion_enabled(display, dict_get(win, "root", 0), false)
      }
      _set_cursor_visibility(display, handle, true)
      win = _apply_window_cursor(win)
      _capture_cursor(display, handle)
      win = dict_set(win, "captured_cursor", true)
      win = dict_set(win, "disabled_cursor", false)
      win = dict_set(win, "ignore_warp_motion", false)
      return win
   }

   if(value == GLFW_CURSOR_DISABLED){
      def pos = get_cursor_pos(win)
      def center_x = int(dict_get(win, "w", 1) / 2)
      def center_y = int(dict_get(win, "h", 1) / 2)
      win = dict_set(win, "restore_cursor_x", int(get(pos, 0, 0.0)))
      win = dict_set(win, "restore_cursor_y", int(get(pos, 1, 0.0)))
      _set_cursor_visibility(display, handle, false)
      _capture_cursor(display, handle)
      if(dict_get(win, "xi_available", false) && dict_get(win, "raw_mouse_motion", false)){
         _xi_set_raw_motion_enabled(display, dict_get(win, "root", 0), true)
      }
      _set_cursor_pos_raw(display, handle, center_x, center_y)
      win = dict_set(win, "warp_cursor_x", center_x)
      win = dict_set(win, "warp_cursor_y", center_y)
      win = dict_set(win, "ignore_warp_motion", true)
      win = dict_set(win, "mouse_x", center_x)
      win = dict_set(win, "mouse_y", center_y)
      win = dict_set(win, "captured_cursor", true)
      win = dict_set(win, "disabled_cursor", true)
      return win
   }

   win
}

fn get_input_mode(win, mode){
   "Queries the current input mode for the given native X11 window."
   if(!win || !is_dict(win)){ return 0 }
   if(mode == 0x00033005){ ;; GLFW_RAW_MOUSE_MOTION
      return dict_get(win, "raw_mouse_motion", false) ? 1 : 0
   }
   if(mode == 0x00033001){ ;; GLFW_CURSOR_MODE
      return dict_get(win, "cursor_mode", 0x00034001) ;; GLFW_CURSOR_NORMAL
   }
   if(mode == 0x00033002){ ;; GLFW_STICKY_KEYS
      return dict_get(win, "sticky_keys", false) ? 1 : 0
   }
   if(mode == 0x00033003){ ;; GLFW_STICKY_MOUSE_BUTTONS
      return dict_get(win, "sticky_mouse_buttons", false) ? 1 : 0
   }
   if(mode == 0x00033004){ ;; GLFW_LOCK_KEY_MODS
      return dict_get(win, "lock_key_mods", false) ? 1 : 0
   }
   0
}

fn get_key_scancode(win, key){
   -1
}

fn _request_window_attention_raw(display, root, win, net_wm_state_atom, demands_attention_atom){
   "Direct Ny port of GLFW `_glfwRequestWindowAttentionX11`."
   if(!display || !root || !win || !net_wm_state_atom || !demands_attention_atom){ return false }
   def ok = send_wm_state_event(display, root, win, net_wm_state_atom,
      NET_WM_STATE_ADD, demands_attention_atom)
   flush(display)
   ok
}

fn request_window_attention(win){
   "Requests user attention for the window."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def root = dict_get(win, "root", 0)
   def handle = dict_get(win, "handle", 0)
   def net_wm_state = dict_get(win, "net_wm_state", 0)
   def demands_attention = dict_get(win, "net_wm_state_demands_attention", 0)
   _request_window_attention_raw(display, root, handle, net_wm_state, demands_attention)
}

fn _get_window_frame_extents_property(display, win, net_frame_extents_atom){
   "Reads `_NET_FRAME_EXTENTS` and returns `{ left, right, top, bottom }`."
   def prop = get_window_property(display, win, net_frame_extents_atom, XA_CARDINAL)
   if(!prop || !is_dict(prop)){ return false }
   def data = dict_get(prop, "data", 0)
   def count = dict_get(prop, "count", 0)
   if(!data || count != 4){
      if(data){ XFree(data) }
      return false
   }

   mut out = dict()
   out = dict_set(out, "left", load64_h(data, 0))
   out = dict_set(out, "right", load64_h(data, 8))
   out = dict_set(out, "top", load64_h(data, 16))
   out = dict_set(out, "bottom", load64_h(data, 24))
   XFree(data)
   out
}

fn _get_window_frame_extents_raw(display, root, win, net_frame_extents_atom, net_request_frame_extents_atom=0, timeout_ms=500){
   "Direct Ny port of GLFW `_glfwGetWindowFrameSizeX11` core logic."
   if(!display || !win || !net_frame_extents_atom){ return false }

   def existing = _get_window_frame_extents_property(display, win, net_frame_extents_atom)
   if(existing){ return existing }

   if(!is_window_visible(display, win) && root && net_request_frame_extents_atom){
      send_client_message(display, root, win, net_request_frame_extents_atom, 0, 0, 0, 0, 0)
      flush(display)

      mut waited = 0
      while(waited < timeout_ms){
         def extents = _get_window_frame_extents_property(display, win, net_frame_extents_atom)
         if(extents){ return extents }
         wait_events(display, 1)
         waited += 1
      }
   }

   false
}

fn get_window_frame_size(win){
   "Returns the window frame size [left, top, right, bottom]."
   if(!win || !is_dict(win)){ return [0, 0, 0, 0] }
   def display = dict_get(win, "display", 0)
   def root = dict_get(win, "root", 0)
   def handle = dict_get(win, "handle", 0)
   def net_frame_extents = dict_get(win, "net_frame_extents", 0)
   def net_request_frame_extents = dict_get(win, "net_request_frame_extents", 0)
   def extents = _get_window_frame_extents_raw(display, root, handle, net_frame_extents, net_request_frame_extents)
   if(!extents){ return [0, 0, 0, 0] }
   [dict_get(extents, "left", 0), dict_get(extents, "top", 0), dict_get(extents, "right", 0), dict_get(extents, "bottom", 0)]
}

fn wait_events(display, timeout_ms=100){
   "Waits until at least one X11 event is pending or the timeout expires."
   if(!display){ return false }
   if(timeout_ms <= 0){ return pending(display) > 0 }
   mut waited = 0
   while(waited < timeout_ms){
      if(pending(display) > 0){ return true }
      msleep(1)
      waited += 1
   }
   pending(display) > 0
}

fn wait_for_visibility_notify(display, win, timeout_ms=100){
   "Waits for a `VisibilityNotify` event for `win`, based on GLFW X11 startup path."
   if(!display || !win){ return false }
   def event_buf = malloc(256)
   if(!event_buf){ return false }

   mut ok = false
   mut remaining = timeout_ms
   while(remaining >= 0){
      if(XCheckTypedWindowEvent(display, win, VisibilityNotify, event_buf) != 0){
         ok = true
         break
      }
      if(remaining == 0){ break }
      if(wait_events(display, 1)){
         ;; Let XCheckTypedWindowEvent observe the newly queued event next loop.
      }
      remaining -= 1
   }

   free(event_buf)
   ok
}

fn _actual_event_window(event_ptr, typ){
   "Returns the X11 window handle targeted by the event union."
   match typ {
      ReparentNotify -> { return load64_h(event_ptr, 40) }
      ConfigureNotify -> { return load64_h(event_ptr, 40) }
      DestroyNotify -> { return load64_h(event_ptr, 40) }
      KeyPress -> { return load64_h(event_ptr, 32) }
      KeyRelease -> { return load64_h(event_ptr, 32) }
      ButtonPress -> { return load64_h(event_ptr, 32) }
      ButtonRelease -> { return load64_h(event_ptr, 32) }
      MotionNotify -> { return load64_h(event_ptr, 32) }
      EnterNotify -> { return load64_h(event_ptr, 32) }
      LeaveNotify -> { return load64_h(event_ptr, 32) }
      FocusIn -> { return load64_h(event_ptr, 32) }
      FocusOut -> { return load64_h(event_ptr, 32) }
      Expose -> { return load64_h(event_ptr, 32) }
      SelectionClear -> { return load64_h(event_ptr, 32) }
      SelectionRequest -> { return load64_h(event_ptr, 32) }
      SelectionNotify -> { return load64_h(event_ptr, 32) }
      PropertyNotify -> { return load64_h(event_ptr, 32) }
      ClientMessage -> { return load64_h(event_ptr, 32) }
      _ -> { return load64_h(event_ptr, 32) }
   }
}

fn _send_selection_notify(display, requestor, selection, target, property, time){
   "Sends a SelectionNotify event reply."
   if(!display || !requestor){ return false }
   def ev = calloc(1, 96)
   if(!ev){ return false }
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

fn _selection_text_for_request(win, selection_atom){
   "Returns the locally owned selection string for a selection target."
   if(!win || !is_dict(win)){ return "" }
   if(selection_atom && selection_atom == dict_get(win, "primary_atom", 0)){
      return dict_get(win, "primary_selection_string", "")
   }
   dict_get(win, "clipboard_string", "")
}

fn _set_utf8_text_property(display, win, property, utf8_string_atom, text){
   "Writes a UTF-8 X11 text property, deleting it when the string is empty."
   if(!display || !win || !property || !utf8_string_atom){ return false }
   if(!is_str(text)){ text = to_str(text) }
   if(len(text) == 0){
      XDeleteProperty(display, win, property)
      return true
   }
   XChangeProperty(display, win, property, utf8_string_atom, 8, PropModeReplace, text, len(text))
   true
}

fn _icon_image_width(image){
   "Reads the width field from a Ny window-icon image dictionary."
   if(!is_dict(image)){ return 0 }
   int(dict_get(image, "width", dict_get(image, "w", 0)))
}

fn _icon_image_height(image){
   "Reads the height field from a Ny window-icon image dictionary."
   if(!is_dict(image)){ return 0 }
   int(dict_get(image, "height", dict_get(image, "h", 0)))
}

fn _icon_image_pixels(image){
   "Returns the RGBA8 pixel source for a Ny window-icon image dictionary."
   if(!is_dict(image)){ return 0 }
   dict_get(image, "pixels_ptr",
      dict_get(image, "pixels",
      dict_get(image, "data", 0)))
}

fn _icon_pixel_source_len(pixels){
   "Returns the byte length for list/string/bytes icon sources, or -1 for raw pointers."
   if(is_str(pixels) || is_bytes(pixels) || is_list(pixels) || is_tuple(pixels)){ return len(pixels) }
   if(is_ptr(pixels)){ return -1 }
   0
}

fn _icon_pixel_byte(pixels, index){
   "Loads one RGBA byte from a supported icon pixel source."
   if(is_ptr(pixels) || is_str(pixels) || is_bytes(pixels)){ return load8(pixels, index) }
   if(is_list(pixels) || is_tuple(pixels)){ return get(pixels, index, 0) }
   0
}

fn _next_cursor_id(){
   def id = _cursor_next_id
   _cursor_next_id += 1
   id
}

fn _cursor_get(cursor){
   if(!is_int(cursor) || cursor <= 0){ return 0 }
   dict_get(_cursor_specs, cursor, 0)
}

fn _cursor_put(cursor, spec){
   if(is_int(cursor) && cursor > 0){
      _cursor_specs = dict_set(_cursor_specs, cursor, spec)
   }
   spec
}

fn _is_standard_cursor_shape(shape){
   shape == ARROW_CURSOR ||
   shape == IBEAM_CURSOR ||
   shape == CROSSHAIR_CURSOR ||
   shape == POINTING_HAND_CURSOR ||
   shape == RESIZE_EW_CURSOR ||
   shape == RESIZE_NS_CURSOR ||
   shape == RESIZE_NWSE_CURSOR ||
   shape == RESIZE_NESW_CURSOR ||
   shape == RESIZE_ALL_CURSOR ||
   shape == NOT_ALLOWED_CURSOR
}

fn _cursor_theme_name(shape){
   case shape {
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
      _ -> ""
   }
}

fn _cursor_font_shape(shape){
   case shape {
      ARROW_CURSOR -> XC_left_ptr
      IBEAM_CURSOR -> XC_xterm
      CROSSHAIR_CURSOR -> XC_crosshair
      POINTING_HAND_CURSOR -> XC_hand2
      RESIZE_EW_CURSOR -> XC_sb_h_double_arrow
      RESIZE_NS_CURSOR -> XC_sb_v_double_arrow
      RESIZE_ALL_CURSOR -> XC_fleur
      _ -> 0
   }
}

fn _create_native_cursor_handle(display, image, xhot=0, yhot=0){
   "Creates an Xcursor handle from a Ny RGBA8 image dictionary."
   if(!display){ return 0 }
   def width = _icon_image_width(image)
   def height = _icon_image_height(image)
   def pixels = _icon_image_pixels(image)
   def bytes = width * height * 4
   def have = _icon_pixel_source_len(pixels)
   if(width <= 0 || height <= 0 || !pixels || (have >= 0 && have < bytes)){ return 0 }

   def native = XcursorImageCreate(width, height)
   if(!native){ return 0 }
   store32(native, int(xhot), 16)
   store32(native, int(yhot), 20)
   def target = load64_h(native, 32)
   if(!target){
      XcursorImageDestroy(native)
      return 0
   }

   mut i = 0
   while(i < width * height){
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

fn _create_standard_cursor_handle(display, shape){
   "Creates a standard X11 cursor using Xcursor themes first, then font-cursor fallback."
   if(!display || !_is_standard_cursor_shape(shape)){ return 0 }

   mut cursor_handle = 0
   def theme_name = _cursor_theme_name(shape)
   if(theme_name != ""){
      def theme = XcursorGetTheme(display)
      if(theme){
         def size = XcursorGetDefaultSize(display)
         def image = XcursorLibraryLoadImage(cstr(theme_name), theme, size)
         if(image){
         cursor_handle = XcursorImageLoadCursor(display, image)
         XcursorImageDestroy(image)
         }
      }
   }

   if(cursor_handle){ return cursor_handle }
   def fallback = _cursor_font_shape(shape)
   if(!fallback){ return 0 }
   XCreateFontCursor(display, fallback)
}

fn create_cursor(image, xhot=0, yhot=0){
   "Creates a backend cursor object from a Ny RGBA8 image dictionary."
   def width = _icon_image_width(image)
   def height = _icon_image_height(image)
   def pixels = _icon_image_pixels(image)
   def bytes = width * height * 4
   def have = _icon_pixel_source_len(pixels)
   if(width <= 0 || height <= 0 || !pixels || (have >= 0 && have < bytes)){ return 0 }
   def cursor = _next_cursor_id()
   mut spec = dict()
   spec = dict_set(spec, "kind", "image")
   spec = dict_set(spec, "image", image)
   spec = dict_set(spec, "xhot", int(xhot))
   spec = dict_set(spec, "yhot", int(yhot))
   spec = dict_set(spec, "display", 0)
   spec = dict_set(spec, "handle", 0)
   _cursor_put(cursor, spec)
   cursor
}

fn create_standard_cursor(shape){
   "Creates a backend cursor object for one of GLFW standard cursor shapes."
   if(!_is_standard_cursor_shape(shape)){ return 0 }
   def cursor = _next_cursor_id()
   mut spec = dict()
   spec = dict_set(spec, "kind", "standard")
   spec = dict_set(spec, "shape", shape)
   spec = dict_set(spec, "display", 0)
   spec = dict_set(spec, "handle", 0)
   _cursor_put(cursor, spec)
   cursor
}

fn _realize_cursor_handle(display, cursor){
   if(!display || !is_int(cursor) || cursor <= 0){ return 0 }
   mut spec = _cursor_get(cursor)
   if(!spec || !is_dict(spec)){ return 0 }

   def cached_display = dict_get(spec, "display", 0)
   def cached_handle = dict_get(spec, "handle", 0)
   if(cached_display == display && cached_handle){ return cached_handle }
   if(cached_display && cached_handle){ XFreeCursor(cached_display, cached_handle) }

   mut handle = 0
   if(dict_get(spec, "kind", "") == "standard"){
      handle = _create_standard_cursor_handle(display, dict_get(spec, "shape", 0))
   } else {
      handle = _create_native_cursor_handle(display,
         dict_get(spec, "image", 0),
         dict_get(spec, "xhot", 0),
         dict_get(spec, "yhot", 0))
   }

   spec = dict_set(spec, "display", display)
   spec = dict_set(spec, "handle", handle)
   _cursor_put(cursor, spec)
   handle
}

fn destroy_cursor(cursor){
   "Destroys a previously created X11 cursor object."
   if(!is_int(cursor) || cursor <= 0){ return false }
   def spec = _cursor_get(cursor)
   if(!spec || !is_dict(spec)){ return false }
   def display = dict_get(spec, "display", 0)
   def handle = dict_get(spec, "handle", 0)
   if(display && handle){ XFreeCursor(display, handle) }
   _cursor_specs = dict_set(_cursor_specs, cursor, 0)
   true
}

fn _apply_cursor_handle(display, win, cursor_handle){
   if(!display || !win){ return false }
   if(cursor_handle){ XDefineCursor(display, win, cursor_handle) }
   else { XUndefineCursor(display, win) }
   flush(display)
   true
}

fn _apply_window_cursor(win){
   if(!win || !is_dict(win)){ return win }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return win }
   def mode = dict_get(win, "cursor_mode", GLFW_CURSOR_NORMAL)
   if(mode != GLFW_CURSOR_NORMAL && mode != GLFW_CURSOR_CAPTURED){ return win }

   def cursor = dict_get(win, "cursor", 0)
   def cursor_handle = _realize_cursor_handle(display, cursor)
   _apply_cursor_handle(display, handle, cursor_handle)
   win = dict_set(win, "cursor_handle", cursor_handle)
   win
}

fn set_cursor(win, cursor){
   "Applies a cursor object to an X11 window, or clears it when cursor is zero."
   if(!win || !is_dict(win)){ return win }
   win = dict_set(win, "cursor", cursor)
   _apply_window_cursor(win)
}
fn set_window_icon(win, images){
   "Publishes `_NET_WM_ICON` using GLFW-compatible packed ARGB32 data."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def net_wm_icon_atom = dict_get(win, "net_wm_icon_atom", 0)
   if(!display || !handle || !net_wm_icon_atom){ return false }

   if(!images || !is_list(images) || len(images) == 0){
      XDeleteProperty(display, handle, net_wm_icon_atom)
      flush(display)
      return true
   }

   mut long_count = 0
   mut i = 0
   while(i < len(images)){
      def image = get(images, i, 0)
      def width = _icon_image_width(image)
      def height = _icon_image_height(image)
      def pixels = _icon_image_pixels(image)
      if(width <= 0 || height <= 0 || !pixels){ return false }
      def need = width * height * 4
      def have = _icon_pixel_source_len(pixels)
      if(have >= 0 && have < need){ return false }
      long_count += 2 + width * height
      i += 1
   }

   def icon = calloc(long_count, 8)
   if(!icon){ return false }

   mut offset = 0
   i = 0
   while(i < len(images)){
      def image = get(images, i, 0)
      def width = _icon_image_width(image)
      def height = _icon_image_height(image)
      def pixels = _icon_image_pixels(image)
      store64_h(icon, width, offset * 8)
      offset += 1
      store64_h(icon, height, offset * 8)
      offset += 1

      mut p = 0
      while(p < width * height){
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

fn _xevent_client_l(event_ptr, index){
   "Reads xclient.data.l[index] from an XEvent."
   load64_h(event_ptr, 56 + index * 8)
}

fn _send_xdnd_status(display, source, target, accept, action_copy_atom=0){
   "Sends an XdndStatus reply to the source window."
   if(!display || !source || !target){ return false }
   def ev = calloc(1, 96)
   if(!ev){ return false }
   store32(ev, ClientMessage, 0)
   store64_h(ev, source, 32)
   store64_h(ev, dict_get(target, "xdnd_status", 0), 40)
   store32(ev, 32, 48)
   store64_h(ev, dict_get(target, "handle", 0), 56)
   store64_h(ev, accept ? 1 : 0, 64)
   store64_h(ev, 0, 72)
   store64_h(ev, 0, 80)
   store64_h(ev, accept ? action_copy_atom : 0, 88)
   def ok = XSendEvent(display, source, 0, NoEventMask, ev) != 0
   free(ev)
   flush(display)
   ok
}

fn _send_xdnd_finished(display, source, target, accepted, action_copy_atom=0){
   "Sends an XdndFinished reply after a drop has been handled."
   if(!display || !source || !target){ return false }
   def ev = calloc(1, 96)
   if(!ev){ return false }
   store32(ev, ClientMessage, 0)
   store64_h(ev, source, 32)
   store64_h(ev, dict_get(target, "xdnd_finished", 0), 40)
   store32(ev, 32, 48)
   store64_h(ev, dict_get(target, "handle", 0), 56)
   store64_h(ev, accepted ? 1 : 0, 64)
   store64_h(ev, accepted ? action_copy_atom : 0, 72)
   def ok = XSendEvent(display, source, 0, NoEventMask, ev) != 0
   free(ev)
   flush(display)
   ok
}

fn _clear_xdnd_state(win){
   "Resets cached Xdnd source/version/format state after a drag sequence ends."
   if(!win || !is_dict(win)){ return win }
   win = dict_set(win, "xdnd_source", 0)
   win = dict_set(win, "xdnd_version", 0)
   win = dict_set(win, "xdnd_format", 0)
   win
}

fn _translate_root_to_window(display, root, win, xabs, yabs){
   "Translates root coordinates to window-local coordinates."
   def out_x = malloc(4)
   def out_y = malloc(4)
   def child = malloc(8)
   if(!out_x || !out_y || !child){
      if(out_x){ free(out_x) }
      if(out_y){ free(out_y) }
      if(child){ free(child) }
      return [0, 0]
   }
   XTranslateCoordinates(display, root, win, xabs, yabs, out_x, out_y, child)
   def res = [load32(out_x, 0), load32(out_y, 0)]
   free(out_x)
   free(out_y)
   free(child)
   res
}

fn _xdnd_pick_format(display, source, offered_list, text_uri_atom, xdnd_type_list_atom){
   "Chooses a usable Xdnd data format, preferring `text/uri-list`."
   if(!display || !source || !text_uri_atom){ return 0 }
   if(is_list(offered_list)){
      mut i = 0
      while(i < len(offered_list)){
         if(get(offered_list, i) == text_uri_atom){ return text_uri_atom }
         i += 1
      }
   }

   if(!xdnd_type_list_atom){ return 0 }
   def prop = get_window_property(display, source, xdnd_type_list_atom, XA_ATOM)
   if(!prop || !is_dict(prop)){ return 0 }
   def data = dict_get(prop, "data", 0)
   def count = dict_get(prop, "count", 0)
   mut chosen = 0
   if(data){
      mut i = 0
      while(i < count){
         if(load64_h(data, i * 8) == text_uri_atom){
         chosen = text_uri_atom
         break
         }
         i += 1
      }
      XFree(data)
   }
   chosen
}

fn _compute_scale(width, height, mm_width, mm_height){
   "Computes approximate monitor content scale relative to 96 DPI."
   mut sx = 1.0
   mut sy = 1.0
   if(width > 0 && mm_width > 0){
      sx = (float(width) * 25.4) / (float(mm_width) * 96.0)
   }
   if(height > 0 && mm_height > 0){
      sy = (float(height) * 25.4) / (float(mm_height) * 96.0)
   }
   [sx, sy]
}

fn _resolve_monitor_context(display=0, root=0){
   "Resolves an X11 display/root pair, opening a temporary display if needed."
   mut owned = false
   mut resolved = display
   mut screen = 0
   if(!resolved){
      resolved = open_display()
      if(!resolved){ return false }
      owned = true
   }
   screen = default_screen(resolved)
   if(!root){ root = root_window(resolved, screen) }
   mut ctx = dict()
   ctx = dict_set(ctx, "display", resolved)
   ctx = dict_set(ctx, "root", root)
   ctx = dict_set(ctx, "screen", screen)
   ctx = dict_set(ctx, "owned", owned)
   ctx
}

fn _release_monitor_context(ctx){
   "Closes a temporary X11 display opened by `_resolve_monitor_context`."
   if(!ctx || !is_dict(ctx)){ return }
   if(dict_get(ctx, "owned", false)){
      close_display(dict_get(ctx, "display", 0))
   }
}

fn _split_bpp(depth){
   "Splits common X11 framebuffer depths into RGB channel bit counts."
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

fn _get_mode_info(resources, mode_id){
   "Looks up an `XRRModeInfo` inside a screen-resources snapshot."
   if(!resources || !mode_id){ return 0 }
   def count = load32(resources, 48)
   def modes = load64_h(resources, 56)
   mut i = 0
   while(i < count){
      def mode_ptr = modes + i * 80
      if(load64_h(mode_ptr, 0) == mode_id){ return mode_ptr }
      i += 1
   }
   0
}

fn _refresh_from_mode_info(mode_ptr){
   "Computes refresh rate in Hz from an `XRRModeInfo`."
   if(!mode_ptr){ return 0 }
   def dot_clock = load64_h(mode_ptr, 16)
   def h_total = load32(mode_ptr, 32)
   def v_total = load32(mode_ptr, 48)
   if(!dot_clock || !h_total || !v_total){ return 0 }
   int(float(dot_clock) / (float(h_total) * float(v_total)) + 0.5)
}

fn _mode_size_from_info(mode_ptr, rotation=RR_Rotate_0){
   "Returns `[width, height]` for a RandR mode, accounting for rotation."
   if(!mode_ptr){ return [0, 0] }
   mut width = load32(mode_ptr, 8)
   mut height = load32(mode_ptr, 12)
   if(rotation == RR_Rotate_90 || rotation == RR_Rotate_270){
      def tmp = width
      width = height
      height = tmp
   }
   [width, height]
}

fn _monitor_from_output(display, resources, output, primary_output=0){
   "Builds a GLFW-style monitor description dict from one RandR output."
   if(!display || !resources || !output){ return false }
   def info = XRRGetOutputInfo(display, resources, output)
   if(!info){ return false }
   def connection = load16(info, 48)
   def crtc = load64_h(info, 8)
   if(connection != RR_Connected || !crtc){
      XRRFreeOutputInfo(info)
      return false
   }

   def crtc_info = XRRGetCrtcInfo(display, resources, crtc)
   if(!crtc_info){
      XRRFreeOutputInfo(info)
      return false
   }

   def rotation = load16(crtc_info, 32)
   def mode_id = load64_h(crtc_info, 24)
   def mode_ptr = _get_mode_info(resources, mode_id)
   def size = _mode_size_from_info(mode_ptr, rotation)
   mut width = get(size, 0, load32(crtc_info, 16))
   mut height = get(size, 1, load32(crtc_info, 20))
   if(width <= 0){ width = load32(crtc_info, 16) }
   if(height <= 0){ height = load32(crtc_info, 20) }

   mut width_mm = load32(info, 32)
   mut height_mm = load32(info, 40)
   if(rotation == RR_Rotate_90 || rotation == RR_Rotate_270){
      def tmp_mm = width_mm
      width_mm = height_mm
      height_mm = tmp_mm
   }
   if(width_mm <= 0 || height_mm <= 0){
      width_mm = int(float(width) * 25.4 / 96.0 + 0.5)
      height_mm = int(float(height) * 25.4 / 96.0 + 0.5)
   }

   def scale = _compute_scale(width, height, width_mm, height_mm)
   def rgb = _split_bpp(default_depth(display, default_screen(display)))
   mut monitor = dict()
   monitor = dict_set(monitor, "output", output)
   monitor = dict_set(monitor, "crtc", crtc)
   monitor = dict_set(monitor, "primary", output == primary_output)
   monitor = dict_set(monitor, "name", cstr_to_str(load64_h(info, 16)))
   monitor = dict_set(monitor, "x", load32(crtc_info, 8))
   monitor = dict_set(monitor, "y", load32(crtc_info, 12))
   monitor = dict_set(monitor, "width", width)
   monitor = dict_set(monitor, "height", height)
   monitor = dict_set(monitor, "width_mm", width_mm)
   monitor = dict_set(monitor, "height_mm", height_mm)
   monitor = dict_set(monitor, "scale_x", get(scale, 0, 1.0))
   monitor = dict_set(monitor, "scale_y", get(scale, 1, 1.0))
   monitor = dict_set(monitor, "refresh_rate", _refresh_from_mode_info(mode_ptr))
   monitor = dict_set(monitor, "mode_id", mode_id)
   monitor = dict_set(monitor, "rotation", rotation)
   monitor = dict_set(monitor, "red_bits", get(rgb, 0, 8))
   monitor = dict_set(monitor, "green_bits", get(rgb, 1, 8))
   monitor = dict_set(monitor, "blue_bits", get(rgb, 2, 8))

   XRRFreeCrtcInfo(crtc_info)
   XRRFreeOutputInfo(info)
   monitor
}

fn get_monitors(display=0, root=0){
   "Enumerates connected X11/RandR monitors as monitor dicts."
   def ctx = _resolve_monitor_context(display, root)
   if(!ctx){ return [] }
   display = dict_get(ctx, "display", 0)
   root = dict_get(ctx, "root", 0)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if(!resources){
      _release_monitor_context(ctx)
      return []
   }

   def count = load32(resources, 32)
   def outputs = load64_h(resources, 40)
   def primary = XRRGetOutputPrimary(display, root)
   mut primary_list = []
   mut others = []
   mut i = 0
   while(i < count){
      def monitor = _monitor_from_output(display, resources, load64_h(outputs, i * 8), primary)
      if(monitor){
         if(dict_get(monitor, "primary", false)){ primary_list = append(primary_list, monitor) }
         else { others = append(others, monitor) }
      }
      i += 1
   }

   XRRFreeScreenResources(resources)
   _release_monitor_context(ctx)
   mut out = extend(primary_list, others)
   i = 0
   while(i < len(out)){
      def monitor = get(out, i)
      if(monitor && is_dict(monitor)){
         out = set(out, i, dict_set(monitor, "index", i))
      }
      i += 1
   }
   out
}

fn get_primary_monitor(display=0, root=0){
   "Returns the primary monitor dict, or false if none are connected."
   def monitors = get_monitors(display, root)
   if(len(monitors) == 0){ return false }
   get(monitors, 0)
}

fn get_monitor_pos(monitor){
   "Returns `[x, y]` for a monitor dict."
   if(!monitor || !is_dict(monitor)){ return [0, 0] }
   [dict_get(monitor, "x", 0), dict_get(monitor, "y", 0)]
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

fn get_monitor_workarea(monitor, display=0, root=0, net_workarea_atom=0, net_current_desktop_atom=0){
   "Returns `[x, y, width, height]` clipped to `_NET_WORKAREA` when available."
   if(!monitor || !is_dict(monitor)){ return [0, 0, 0, 0] }
   def ctx = _resolve_monitor_context(display, root)
   if(!ctx){
      return [
         dict_get(monitor, "x", 0),
         dict_get(monitor, "y", 0),
         dict_get(monitor, "width", 0),
         dict_get(monitor, "height", 0)
      ]
   }

   display = dict_get(ctx, "display", 0)
   root = dict_get(ctx, "root", 0)
   if(!net_workarea_atom){ net_workarea_atom = intern_atom(display, "_NET_WORKAREA") }
   if(!net_current_desktop_atom){ net_current_desktop_atom = intern_atom(display, "_NET_CURRENT_DESKTOP") }

   mut area_x = dict_get(monitor, "x", 0)
   mut area_y = dict_get(monitor, "y", 0)
   mut area_w = dict_get(monitor, "width", 0)
   mut area_h = dict_get(monitor, "height", 0)

   if(net_workarea_atom && net_current_desktop_atom){
      def extents = get_window_property(display, root, net_workarea_atom, XA_CARDINAL)
      def desktop = get_window_property(display, root, net_current_desktop_atom, XA_CARDINAL)
      if(extents && desktop){
         def extent_data = dict_get(extents, "data", 0)
         def extent_count = dict_get(extents, "count", 0)
         def desk_data = dict_get(desktop, "data", 0)
         if(extent_data && desk_data && extent_count >= 4){
         def index = int(load64_h(desk_data, 0))
         if(index >= 0 && index < int(extent_count / 4)){
               def base = index * 32
               def global_x = load64_h(extent_data, base + 0)
               def global_y = load64_h(extent_data, base + 8)
               def global_w = load64_h(extent_data, base + 16)
               def global_h = load64_h(extent_data, base + 24)
               if(area_x < global_x){
                  area_w -= global_x - area_x
                  area_x = global_x
               }
               if(area_y < global_y){
                  area_h -= global_y - area_y
                  area_y = global_y
               }
               if(area_x + area_w > global_x + global_w){
                  area_w = global_x - area_x + global_w
               }
               if(area_y + area_h > global_y + global_h){
                  area_h = global_y - area_y + global_h
               }
         }
         }
         def extent_ptr = dict_get(extents, "data", 0)
         def desk_ptr = dict_get(desktop, "data", 0)
         if(extent_ptr){ XFree(extent_ptr) }
         if(desk_ptr){ XFree(desk_ptr) }
      }
   }

   _release_monitor_context(ctx)
   [area_x, area_y, area_w, area_h]
}

fn get_video_mode(monitor, display=0, root=0){
   "Returns the current GLFW-style video mode dict for a monitor."
   if(!monitor || !is_dict(monitor)){ return false }
   def ctx = _resolve_monitor_context(display, root)
   if(!ctx){ return false }
   display = dict_get(ctx, "display", 0)
   root = dict_get(ctx, "root", 0)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if(!resources){
      _release_monitor_context(ctx)
      return false
   }

   def mode_ptr = _get_mode_info(resources, dict_get(monitor, "mode_id", 0))
   if(!mode_ptr){
      XRRFreeScreenResources(resources)
      _release_monitor_context(ctx)
      return false
   }

   def size = _mode_size_from_info(mode_ptr, dict_get(monitor, "rotation", RR_Rotate_0))
   mut out = dict()
   out = dict_set(out, "width", get(size, 0, dict_get(monitor, "width", 0)))
   out = dict_set(out, "height", get(size, 1, dict_get(monitor, "height", 0)))
   out = dict_set(out, "red_bits", dict_get(monitor, "red_bits", 8))
   out = dict_set(out, "green_bits", dict_get(monitor, "green_bits", 8))
   out = dict_set(out, "blue_bits", dict_get(monitor, "blue_bits", 8))
   out = dict_set(out, "refresh_rate", _refresh_from_mode_info(mode_ptr))

   XRRFreeScreenResources(resources)
   _release_monitor_context(ctx)
   out
}

fn get_video_modes(monitor, display=0, root=0){
   "Returns distinct GLFW-style video modes for a monitor."
   if(!monitor || !is_dict(monitor)){ return [] }
   def ctx = _resolve_monitor_context(display, root)
   if(!ctx){ return [] }
   display = dict_get(ctx, "display", 0)
   root = dict_get(ctx, "root", 0)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if(!resources){
      _release_monitor_context(ctx)
      return []
   }

   def info = XRRGetOutputInfo(display, resources, dict_get(monitor, "output", 0))
   def crtc_info = XRRGetCrtcInfo(display, resources, dict_get(monitor, "crtc", 0))
   if(!info || !crtc_info){
      if(info){ XRRFreeOutputInfo(info) }
      if(crtc_info){ XRRFreeCrtcInfo(crtc_info) }
      XRRFreeScreenResources(resources)
      _release_monitor_context(ctx)
      return []
   }

   def mode_ids = load64_h(info, 88)
   def count = load32(info, 80)
   def rotation = load16(crtc_info, 32)
   mut modes = []
   mut seen = dict()
   mut i = 0
   while(i < count){
      def mode_ptr = _get_mode_info(resources, load64_h(mode_ids, i * 8))
      if(mode_ptr && !band(load64_h(mode_ptr, 72), RR_Interlace)){
         def size = _mode_size_from_info(mode_ptr, rotation)
         def refresh = _refresh_from_mode_info(mode_ptr)
         def key_str = to_str(get(size, 0, 0)) + "x" + to_str(get(size, 1, 0)) + "@" + to_str(refresh)
         if(!dict_get(seen, key_str, false)){
         seen = dict_set(seen, key_str, true)
         mut mode = dict()
         mode = dict_set(mode, "width", get(size, 0, 0))
         mode = dict_set(mode, "height", get(size, 1, 0))
         mode = dict_set(mode, "red_bits", dict_get(monitor, "red_bits", 8))
         mode = dict_set(mode, "green_bits", dict_get(monitor, "green_bits", 8))
         mode = dict_set(mode, "blue_bits", dict_get(monitor, "blue_bits", 8))
         mode = dict_set(mode, "refresh_rate", refresh)
         modes = append(modes, mode)
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

fn _choose_video_mode(resources, output_info, rotation, width, height, refresh_rate=0){
   "Chooses the best RandR mode id matching the desired size and refresh."
   if(!resources || !output_info){ return 0 }
   def count = load32(output_info, 80)
   def mode_ids = load64_h(output_info, 88)
   mut best_mode = 0
   mut best_score = 1 << 30
   mut i = 0
   while(i < count){
      def mode_ptr = _get_mode_info(resources, load64_h(mode_ids, i * 8))
      if(mode_ptr && !band(load64_h(mode_ptr, 72), RR_Interlace)){
         def size = _mode_size_from_info(mode_ptr, rotation)
         def mw = get(size, 0, 0)
         def mh = get(size, 1, 0)
         def refresh = _refresh_from_mode_info(mode_ptr)
         mut score = abs(mw - width) * 10000 + abs(mh - height) * 100 + abs(refresh - max(0, refresh_rate))
         if(refresh_rate <= 0){
         score = abs(mw - width) * 10000 + abs(mh - height) * 100
         }
         if(score < best_score){
         best_score = score
         best_mode = load64_h(mode_ptr, 0)
         }
      }
      i += 1
   }
   best_mode
}

fn get_window_monitor(win){
   "Returns the monitor dict currently associated with a native X11 window."
   if(!win || !is_dict(win)){ return false }
   dict_get(win, "monitor", false)
}

fn set_window_monitor(win, monitor, xpos, ypos, width, height, refresh_rate=0){
   "GLFW-style fullscreen/windowed monitor transition for the native X11 backend."
   if(!win || !is_dict(win)){ return win }
   def display = dict_get(win, "display", 0)
   def root = dict_get(win, "root", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !root || !handle){ return win }
   def ewmh_fullscreen = dict_get(win, "net_wm_state", 0) && dict_get(win, "net_wm_state_fullscreen", 0)

   def was_monitor = dict_get(win, "monitor", false)
   if(monitor){
      if(!was_monitor){
         win = dict_set(win, "windowed_x", dict_get(win, "x", xpos))
         win = dict_set(win, "windowed_y", dict_get(win, "y", ypos))
         win = dict_set(win, "windowed_w", dict_get(win, "w", width))
         win = dict_set(win, "windowed_h", dict_get(win, "h", height))
      }

      def resources = XRRGetScreenResourcesCurrent(display, root)
      if(resources){
         def crtc = dict_get(monitor, "crtc", 0)
         def output = dict_get(monitor, "output", 0)
         def crtc_info = XRRGetCrtcInfo(display, resources, crtc)
         def output_info = XRRGetOutputInfo(display, resources, output)
         if(crtc_info && output_info){
         def current_mode = load64_h(crtc_info, 24)
         def rotation = load16(crtc_info, 32)
         def chosen_mode = _choose_video_mode(resources, output_info, rotation, width, height, refresh_rate)
         if(chosen_mode && chosen_mode != current_mode){
               XRRSetCrtcConfig(display, resources, crtc, CurrentTime,
                  load32(crtc_info, 8), load32(crtc_info, 12), chosen_mode,
                  rotation, load64_h(crtc_info, 40), load32(crtc_info, 36))
               win = dict_set(win, "monitor_old_mode", current_mode)
         }
         XRRFreeOutputInfo(output_info)
         XRRFreeCrtcInfo(crtc_info)
         } else {
         if(output_info){ XRRFreeOutputInfo(output_info) }
         if(crtc_info){ XRRFreeCrtcInfo(crtc_info) }
         }
         XRRFreeScreenResources(resources)
      }

      if(!is_window_visible(display, handle)){
         map_window(display, handle)
         true
      }
      update_normal_hints(display, handle, width, height, dict_get(win, "resizable", true), true)
      _set_fullscreen_monitors(display, root, handle,
         dict_get(win, "net_wm_fullscreen_monitors", 0), monitor)
      if(ewmh_fullscreen){
         set_window_fullscreen(display, root, handle,
         dict_get(win, "net_wm_state", 0),
         dict_get(win, "net_wm_state_fullscreen", 0), true)
      } else {
         _set_override_redirect(display, handle, true)
      }
      if(dict_get(win, "net_wm_bypass_compositor", 0) &&
         !band(int(dict_get(win, "flags", 0)), WINDOW_TRANSPARENT)){
         _set_compositor_bypass(display, handle, dict_get(win, "net_wm_bypass_compositor", 0), true)
      }
      move_window(display, handle, dict_get(monitor, "x", xpos), dict_get(monitor, "y", ypos))
      resize_window(display, handle, width, height)
      flush(display)
      win = dict_set(win, "monitor", monitor)
      win = dict_set(win, "fullscreen", true)
      win = dict_set(win, "x", dict_get(monitor, "x", xpos))
      win = dict_set(win, "y", dict_get(monitor, "y", ypos))
      win = dict_set(win, "w", width)
      win = dict_set(win, "h", height)
      win = dict_set(win, "override_redirect", !ewmh_fullscreen)
      return win
   }

   def previous = dict_get(win, "monitor", false)
   if(previous){
      def resources = XRRGetScreenResourcesCurrent(display, root)
      if(resources){
         def crtc = dict_get(previous, "crtc", 0)
         def crtc_info = XRRGetCrtcInfo(display, resources, crtc)
         def old_mode = dict_get(win, "monitor_old_mode", 0)
         if(crtc_info && old_mode){
         XRRSetCrtcConfig(display, resources, crtc, CurrentTime,
               load32(crtc_info, 8), load32(crtc_info, 12), old_mode,
               load16(crtc_info, 32), load64_h(crtc_info, 40), load32(crtc_info, 36))
         }
         if(crtc_info){ XRRFreeCrtcInfo(crtc_info) }
         XRRFreeScreenResources(resources)
      }
   }

   _set_fullscreen_monitors(display, root, handle,
      dict_get(win, "net_wm_fullscreen_monitors", 0), false)
   if(ewmh_fullscreen){
      set_window_fullscreen(display, root, handle,
         dict_get(win, "net_wm_state", 0),
         dict_get(win, "net_wm_state_fullscreen", 0), false)
   } else {
      _set_override_redirect(display, handle, false)
   }
   if(dict_get(win, "net_wm_bypass_compositor", 0)){
      _set_compositor_bypass(display, handle, dict_get(win, "net_wm_bypass_compositor", 0), false)
   }
   update_normal_hints(display, handle, width, height, dict_get(win, "resizable", true), false)
   move_window(display, handle, xpos, ypos)
   resize_window(display, handle, width, height)
   flush(display)
   win = dict_set(win, "monitor", false)
   win = dict_set(win, "fullscreen", false)
   win = dict_set(win, "x", xpos)
   win = dict_set(win, "y", ypos)
   win = dict_set(win, "w", width)
   win = dict_set(win, "h", height)
   win = dict_set(win, "monitor_old_mode", 0)
   win = dict_set(win, "override_redirect", false)
   win
}

mut _x11_error_code = 0
mut _x11_error_request = 0
mut _x11_error_minor = 0
mut _x11_error_resource = 0

fn _x11_error_handler(display, error_ptr: ptr){
   "X11 error handler that logs error details for debugging."
   if(!error_ptr){ return 0 }

   def error_code = load8(error_ptr, 0)
   def request_code = load8(error_ptr, 1)
   def minor_code = load8(error_ptr, 2)
   def resource_id = load32(error_ptr, 4)

   _x11_error_code = error_code
   _x11_error_request = request_code
   _x11_error_minor = minor_code
   _x11_error_resource = resource_id

   if(_is_debug()){
      print("[x11:ERROR] X11 error occurred:")
      print("[x11:ERROR]   error_code: " + to_str(error_code))
      print("[x11:ERROR]   request_code: " + to_str(request_code))
      print("[x11:ERROR]   minor_code: " + to_str(minor_code))
      print("[x11:ERROR]   resource_id: 0x" + to_hex(resource_id))

      ;; Decode common error codes
      match error_code {
         1 -> { print("[x11:ERROR]   type: BadValue (integer parameter out of range)") }
         2 -> { print("[x11:ERROR]   type: BadWindow (invalid Window parameter)") }
         3 -> { print("[x11:ERROR]   type: BadPixmap (invalid Pixmap parameter)") }
         4 -> { print("[x11:ERROR]   type: BadAtom (invalid Atom parameter)") }
         5 -> { print("[x11:ERROR]   type: BadCursor (invalid Cursor parameter)") }
         6 -> { print("[x11:ERROR]   type: BadFont (invalid Font/GC parameter)") }
         7 -> { print("[x11:ERROR]   type: BadMatch (invalid parameter combination)") }
         8 -> { print("[x11:ERROR]   type: BadDrawable (invalid Pixmap/Window)") }
         9 -> { print("[x11:ERROR]   type: BadAccess (resource access conflict)") }
         10 -> { print("[x11:ERROR]   type: BadAlloc (memory allocation failed)") }
         11 -> { print("[x11:ERROR]   type: BadColor (invalid Colormap parameter)") }
         12 -> { print("[x11:ERROR]   type: BadGC (invalid GC parameter)") }
         13 -> { print("[x11:ERROR]   type: BadIDChoice (invalid resource ID)") }
         14 -> { print("[x11:ERROR]   type: BadName (invalid Font/Atom name)") }
         15 -> { print("[x11:ERROR]   type: BadLength (poly request too large)") }
         16 -> { print("[x11:ERROR]   type: BadImplementation (server doesn't implement)") }
         _ -> { print("[x11:ERROR]   type: Unknown error code") }
      }
   }

   0
}

fn _setup_x11_error_handler(){
   "Sets up the X11 error handler for debugging."
   if(comptime{ __os_name() == "linux" }){
      XSetErrorHandler(_x11_error_handler)
      if(_is_debug()){
         print("[x11] X11 error handler installed")
      }
   }
}

fn _get_last_x11_error(){
   "Returns the last X11 error details as a dict."
   mut error = dict()
   error = dict_set(error, "code", _x11_error_code)
   error = dict_set(error, "request", _x11_error_request)
   error = dict_set(error, "minor", _x11_error_minor)
   error = dict_set(error, "resource", _x11_error_resource)
   error
}

fn _clear_x11_error(){
   "Clears the last X11 error."
   _x11_error_code = 0
   _x11_error_request = 0
   _x11_error_minor = 0
   _x11_error_resource = 0
}

fn _xrandr_connected_outputs(display, root){
   "Snapshots active RandR outputs as a dict keyed by output id."
   if(!display || !root){ return dict() }
   def prev_handler = XSetErrorHandler(0)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if(!resources){ XSetErrorHandler(prev_handler) return dict() }

   mut outputs = dict()
   def count = load32(resources, 32)
   def output_ptr = load64_h(resources, 40)
   mut i = 0
   while(i < count){
      def output = load64_h(output_ptr, i * 8)
      def info = XRRGetOutputInfo(display, resources, output)
      if(info){
         def connection = load16(info, 48)
         def crtc = load64_h(info, 8)
         if(connection == RR_Connected && crtc){
         outputs = dict_set(outputs, output, true)
         }
         XRRFreeOutputInfo(info)
      }
      i += 1
   }

   XRRFreeScreenResources(resources)
   XSync(display, 0)
   XSetErrorHandler(prev_handler)
   outputs
}

fn _xrandr_event_scale(event_ptr){
   "Reads scale data from an XRandR screen-change event."
   def width = load32(event_ptr, 72)
   def height = load32(event_ptr, 76)
   def mm_width = load32(event_ptr, 80)
   def mm_height = load32(event_ptr, 84)
   _compute_scale(width, height, mm_width, mm_height)
}

fn _push_randr_output_diff(events, win, before_outputs, after_outputs){
   "Appends monitor connect/disconnect events derived from two snapshots."
   mut keys = dict_keys(after_outputs)
   mut i = 0
   while(i < len(keys)){
      def output = get(keys, i)
      if(!dict_get(before_outputs, output, false)){
         mut data = dict()
         data = dict_set(data, "output", output)
         _push_translated_event(events, EVENT_MONITOR_CONNECTED, win, data)
      }
      i += 1
   }

   keys = dict_keys(before_outputs)
   i = 0
   while(i < len(keys)){
      def output = get(keys, i)
      if(!dict_get(after_outputs, output, false)){
         mut data = dict()
         data = dict_set(data, "output", output)
         _push_translated_event(events, EVENT_MONITOR_DISCONNECTED, win, data)
      }
      i += 1
   }
}

fn _xrandr_poll_outputs(win, events){
   "Polls active RandR outputs and emits monitor diff events."
   def display = dict_get(win, "display", 0)
   def root = dict_get(win, "root", 0)
   def before_outputs = dict_get(win, "randr_outputs", dict())
   def after_outputs = _xrandr_connected_outputs(display, root)
   win = dict_set(win, "randr_outputs", after_outputs)
   _push_randr_output_diff(events, win, before_outputs, after_outputs)
   win
}

fn _write_selection_to_property(display, requestor, property, target, text, utf8_string_atom, targets_atom, multiple_atom=0, atom_pair_atom=0, save_targets_atom=0){
   "Writes clipboard data for a SelectionRequest and returns the reply property."
   if(!display || !requestor || !property || !target){ return NoAtom }
   if(!is_str(text)){ text = to_str(text) }

   if(target == targets_atom){
      def target_count = multiple_atom ? 4 : 3
      def targets = calloc(target_count, 8)
      if(!targets){ return NoAtom }
      store64_h(targets, targets_atom, 0)
      if(multiple_atom){ store64_h(targets, multiple_atom, 8) }
      store64_h(targets, utf8_string_atom ? utf8_string_atom : XA_STRING, multiple_atom ? 16 : 8)
      store64_h(targets, XA_STRING, multiple_atom ? 24 : 16)
      XChangeProperty(display, requestor, property, XA_ATOM, 32, PropModeReplace, targets, target_count)
      free(targets)
      return property
   }

   if(multiple_atom && atom_pair_atom && target == multiple_atom){
      def prop = get_window_property(display, requestor, property, atom_pair_atom)
      if(!prop || !is_dict(prop)){ return NoAtom }
      def pairs = dict_get(prop, "data", 0)
      def count = dict_get(prop, "count", 0)
      if(!pairs || count <= 0){
         if(pairs){ XFree(pairs) }
         return NoAtom
      }

      mut i = 0
      while(i + 1 < count){
         def pair_target = load64_h(pairs, i * 8)
         def pair_property = load64_h(pairs, (i + 1) * 8)
         if(pair_target == utf8_string_atom || pair_target == XA_STRING){
         XChangeProperty(display, requestor, pair_property, pair_target, 8, PropModeReplace, text, len(text))
         } elif(save_targets_atom && pair_target == save_targets_atom){
         XChangeProperty(display, requestor, pair_property, NoAtom, 32, PropModeReplace, 0, 0)
         } else {
         store64_h(pairs, (i + 1) * 8, NoAtom)
         }
         i += 2
      }

      XChangeProperty(display, requestor, property, atom_pair_atom, 32, PropModeReplace, pairs, count)
      XFree(pairs)
      return property
   }

   if(save_targets_atom && target == save_targets_atom){
      XChangeProperty(display, requestor, property, NoAtom, 32, PropModeReplace, 0, 0)
      return property
   }

   if(target == utf8_string_atom || target == XA_STRING){
      mut payload = text
      if(target == XA_STRING){
         ;; GLFW converts XA_STRING reads to UTF-8, but writes raw bytes.
         ;; Keep the current UTF-8 string bytes here for simplicity.
         payload = text
      }
      XChangeProperty(display, requestor, property, target, 8, PropModeReplace, payload, len(payload))
      return property
   }

   NoAtom
}

fn _read_selection_property_text(display, win, selection_property_atom, target, incr_atom=0, timeout_ms=500){
   "Reads a converted X11 selection property, including incremental INCR transfers."
   if(!display || !win || !selection_property_atom){ return "" }
   def prop = get_window_property(display, win, selection_property_atom, AnyPropertyType)
   if(!prop || !is_dict(prop)){ return "" }

   def data = dict_get(prop, "data", 0)
   def actual_type = dict_get(prop, "type", 0)
   ;; def item_count = dict_get(prop, "count", 0)
   if(!data){ return "" }

   if(incr_atom && actual_type == incr_atom){
      XFree(data)
      XDeleteProperty(display, win, selection_property_atom)
      flush(display)

      def event_buf = calloc(1, 96)
      if(!event_buf){ return "" }

      mut out = ""
      mut waited = 0
      while(waited < timeout_ms){
         if(pending(display) <= 0){
         wait_events(display, 1)
         waited += 1
         continue
         }

         next_event(display, event_buf)
         if(load32(event_buf, 0) != PropertyNotify){
         continue
         }
         if(load64_h(event_buf, 32) != win || load64_h(event_buf, 40) != selection_property_atom || load32(event_buf, 56) != PropertyNewValue){
         continue
         }

         def chunk = get_window_property(display, win, selection_property_atom, AnyPropertyType)
         if(!chunk || !is_dict(chunk)){ continue }
         def chunk_data = dict_get(chunk, "data", 0)
         def chunk_type = dict_get(chunk, "type", 0)
         def chunk_count = dict_get(chunk, "count", 0)
         if(chunk_data && chunk_count > 0){
         if(chunk_type == XA_STRING){
               out = out + glfw_common.convertLatin1toUTF8(chunk_data)
         } else if(chunk_type == target || target == 0){
               out = out + glfw_common._glfw_strdup(chunk_data)
         }
         XFree(chunk_data)
         XDeleteProperty(display, win, selection_property_atom)
         flush(display)
         waited = 0
         continue
         }
         if(chunk_data){ XFree(chunk_data) }
         break
      }

      free(event_buf)
      return out
   }

   mut text = ""
   if(actual_type == XA_STRING){
      text = glfw_common.convertLatin1toUTF8(data)
   } elif(actual_type == target || target == 0){
      text = glfw_common._glfw_strdup(data)
   }
   XFree(data)
   XDeleteProperty(display, win, selection_property_atom)
   flush(display)
   text
}

fn set_clipboard(win, text){
   "Claims the X11 clipboard selection for `win` and stores `text` locally."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def clipboard_atom = dict_get(win, "clipboard_atom", 0)
   if(!display || !handle || !clipboard_atom){ return false }
   XSetSelectionOwner(display, clipboard_atom, handle, CurrentTime)
   flush(display)
   XGetSelectionOwner(display, clipboard_atom) == handle
}

fn set_primary_selection(win, text){
   "Claims the X11 PRIMARY selection for `win` and stores `text` locally."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def primary_atom = dict_get(win, "primary_atom", 0)
   if(!display || !handle || !primary_atom){ return false }
   XSetSelectionOwner(display, primary_atom, handle, CurrentTime)
   flush(display)
   XGetSelectionOwner(display, primary_atom) == handle
}

fn get_clipboard(win){
   "Fetches clipboard text using the X11 selection conversion path."
   if(!win || !is_dict(win)){ return "" }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def clipboard_atom = dict_get(win, "clipboard_atom", 0)
   def utf8_string_atom = dict_get(win, "utf8_string", 0)
   def selection_property_atom = dict_get(win, "selection_property", 0)
   def local_text = dict_get(win, "clipboard_string", "")
   def timeout_ms = 500
   def incr_atom = dict_get(win, "incr_atom", 0)
   if(!display || !handle || !clipboard_atom || !selection_property_atom){ return "" }
   if(XGetSelectionOwner(display, clipboard_atom) == handle){
      return local_text
   }

   mut i = 0
   mut target = utf8_string_atom
   while(i < 2){
      if(i == 1){ target = XA_STRING }
      XConvertSelection(display, clipboard_atom, target, selection_property_atom, handle, CurrentTime)
      flush(display)

      def notification = calloc(1, 96)
      if(!notification){ return "" }
      mut waited = 0
      mut got = false
      while(waited < timeout_ms){
         if(XCheckTypedWindowEvent(display, handle, SelectionNotify, notification) != 0){
         got = true
         break
         }
         wait_events(display, 1)
         waited += 1
      }

      if(got){
         def property = load64_h(notification, 56)
         free(notification)
         if(property != NoAtom){
         def text = _read_selection_property_text(display, handle, selection_property_atom, target, incr_atom, timeout_ms)
         if(text){ return text }
         }
      } else {
         free(notification)
      }

      i += 1
   }

   ""
}

fn get_primary_selection(win){
   "Fetches PRIMARY selection text using the X11 selection conversion path."
   if(!win || !is_dict(win)){ return "" }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def primary_atom = dict_get(win, "primary_atom", 0)
   def utf8_string_atom = dict_get(win, "utf8_string", 0)
   def selection_property_atom = dict_get(win, "selection_property", 0)
   def local_text = dict_get(win, "primary_selection_string", "")
   def timeout_ms = 500
   def incr_atom = dict_get(win, "incr_atom", 0)
   if(!display || !handle || !primary_atom || !selection_property_atom){ return "" }
   if(XGetSelectionOwner(display, primary_atom) == handle){
      return local_text
   }

   mut i = 0
   mut target = utf8_string_atom
   while(i < 2){
      if(i == 1){ target = XA_STRING }
      XConvertSelection(display, primary_atom, target, selection_property_atom, handle, CurrentTime)
      flush(display)

      def notification = calloc(1, 96)
      if(!notification){ return "" }
      mut waited = 0
      mut got = false
      while(waited < timeout_ms){
         if(XCheckTypedWindowEvent(display, handle, SelectionNotify, notification) != 0){
         got = true
         break
         }
         wait_events(display, 1)
         waited += 1
      }

      if(got){
         def property = load64_h(notification, 56)
         free(notification)
         if(property != NoAtom){
         def text = _read_selection_property_text(display, handle, selection_property_atom, target, incr_atom, timeout_ms)
         if(text){ return text }
         }
      } else {
         free(notification)
      }

      i += 1
   }

   ""
}

fn destroy_basic_window(win){
   "Destroys an X11 window and its associated resources."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def colormap = dict_get(win, "colormap", 0)
   if(!handle || !display){ return false }
   if(colormap){ free_colormap(display, colormap) }
   destroy_window_raw(display, handle)
   true
}

fn set_title(win, title){
   "Unified setter for the X11 window title."
   if(!win || !is_dict(win)){ return false }
   store_name(dict_get(win, "display", 0), dict_get(win, "handle", 0), title,
      dict_get(win, "net_wm_name", 0),
      dict_get(win, "net_wm_icon_name", 0),
      dict_get(win, "utf8_string", 0))
}

fn get_window_attrib(win, attrib){
   "Unified getter for X11 window attributes matching Nytrix constants."
   if(!win || !is_dict(win)){ return 0 }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   mut result = 0
   match attrib {
      RESIZABLE -> { result = dict_get(win, "resizable", true) ? 1 : 0 }
      VISIBLE -> { result = is_window_visible(display, handle) ? 1 : 0 }
      DECORATED -> { result = dict_get(win, "decorated", true) ? 1 : 0 }
      FOCUSED -> { result = dict_get(win, "focused", false) ? 1 : 0 }
      ICONIFIED -> { result = is_window_iconified(display, handle, dict_get(win, "wm_state", 0)) ? 1 : 0 }
      MAXIMIZED -> {
         result = is_window_maximized(display, handle,
         dict_get(win, "net_wm_state", 0),
         dict_get(win, "net_wm_state_maximized_vert", 0),
         dict_get(win, "net_wm_state_maximized_horz", 0)) ? 1 : 0
      }
      TRANSPARENT_FRAMEBUFFER -> { result = dict_get(win, "transparent", false) ? 1 : 0 }
      FLOATING -> {
         result = is_window_floating(display, handle,
         dict_get(win, "net_wm_state", 0),
         dict_get(win, "net_wm_state_above", 0)) ? 1 : 0
      }
      HOVERED -> {
         ;; Live query using XQueryPointer to check if cursor is over window
         result = _window_hovered(display, handle) ? 1 : 0
      }
      MOUSE_PASSTHROUGH -> { result = dict_get(win, "mouse_passthrough", false) ? 1 : 0 }
      AUTO_ICONIFY -> { result = dict_get(win, "auto_iconify", true) ? 1 : 0 }
      FOCUS_ON_SHOW -> { result = dict_get(win, "focus_on_show", true) ? 1 : 0 }
      _ -> { result = 0 }
   }
   _dbgu("get_window_attrib: win=0x" + to_hex(handle) + " attrib=0x" + to_hex(attrib) + " => " + to_str(result))
   result
}

fn _window_hovered(display, handle){
   "GLFW: _glfwWindowHoveredX11 - XQueryPointer to find window under cursor."
   if(!display || !handle){ return 0 }
   def root = calloc(1, 8)
   def child = calloc(1, 8)
   def unused = calloc(1, 8)
   ;; XQueryPointer returns child window under cursor
   def ok = XQueryPointer(display, handle, root, child, unused, unused, unused, unused, unused)
   mut hovered = 0
   if(ok){
      ;; If child is 0, cursor is directly on our window
      def child_win = load64_h(child, 0)
      if(child_win == 0 || child_win == handle){
         hovered = 1
      }
   }
   free(root)
   free(child)
   free(unused)
   hovered
}

fn _push_selection_to_manager(win, timeout_ms=250){
   "Transfers clipboard ownership to the clipboard manager before shutdown."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def clipboard_manager = dict_get(win, "clipboard_manager", 0)
   def save_targets = dict_get(win, "save_targets", 0)
   if(!display || !handle || !clipboard_manager || !save_targets){ return false }

   XConvertSelection(display, clipboard_manager, save_targets, NoAtom, handle, CurrentTime)
   flush(display)

   def event_buf = calloc(1, 192)
   if(!event_buf){ return false }
   mut waited = 0
   while(waited < timeout_ms){
      while(pending(display) > 0){
         next_event(display, event_buf)
         def typ = load32(event_buf, 0)
         if(typ == SelectionRequest){
         def requestor = load64_h(event_buf, 40)
         def selection = load64_h(event_buf, 48)
         def target = load64_h(event_buf, 56)
         def property = load64_h(event_buf, 64)
         def time = load64_h(event_buf, 72)
         def reply_property = _write_selection_to_property(display, requestor, property, target,
               _selection_text_for_request(win, selection),
               dict_get(win, "utf8_string", 0),
               dict_get(win, "targets_atom", 0),
               dict_get(win, "multiple_atom", 0),
               dict_get(win, "atom_pair_atom", 0),
               save_targets)
         _send_selection_notify(display, requestor, selection, target, reply_property, time)
         } elif(typ == SelectionNotify && load64_h(event_buf, 56) == save_targets){
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

fn _push_translated_event(events, typ, win, data=0){
   "Appends a translated std.ui event."
   append(events, ui_event.make_event(typ, win, dict_get(win, "handle", 0), data))
}

fn _xkb_setup(display){
   "Bootstraps the Xkb extension state that GLFW tracks globally."
   mut out = dict()
   if(!display){ return out }

   def major_opcode = malloc(4)
   def event_base = malloc(4)
   def error_base = malloc(4)
   def major = malloc(4)
   def minor = malloc(4)
   def supported = malloc(4)
   def state = calloc(1, 16)
   if(!major_opcode || !event_base || !error_base || !major || !minor || !supported || !state){
      if(major_opcode){ free(major_opcode) }
      if(event_base){ free(event_base) }
      if(error_base){ free(error_base) }
      if(major){ free(major) }
      if(minor){ free(minor) }
      if(supported){ free(supported) }
      if(state){ free(state) }
      return out
   }

   store32(major, 1, 0)
   store32(minor, 0, 0)
   mut available = _safe_XkbQueryExtension(display, major_opcode, event_base, error_base, major, minor) != 0
   mut detectable = false
   mut group = 0
   if(available){
      if(XkbSetDetectableAutoRepeat(display, 1, supported) != 0 && load32(supported, 0) != 0){
         detectable = true
      }
      if(XkbGetState(display, XkbUseCoreKbd, state) == Success){
         group = load8(state, 0)
      }
      XkbSelectEventDetails(display, XkbUseCoreKbd, XkbStateNotify, XkbGroupStateMask, XkbGroupStateMask)
   }

   out = dict_set(out, "available", available)
   out = dict_set(out, "event_base", available ? load32(event_base, 0) : -1)
   out = dict_set(out, "error_base", available ? load32(error_base, 0) : -1)
   out = dict_set(out, "group", group)
   out = dict_set(out, "detectable_repeat", detectable)

   free(major_opcode)
   free(event_base)
   free(error_base)
   free(major)
   free(minor)
   free(supported)
   free(state)
   out
}

fn _xi_setup(display){
   "Bootstraps the XI2 extension state that GLFW uses for raw mouse motion."
   mut out = dict()
   if(!display){ return out }

   def major_opcode = malloc(4)
   def event_base = malloc(4)
   def error_base = malloc(4)
   def major = malloc(4)
   def minor = malloc(4)
   if(!major_opcode || !event_base || !error_base || !major || !minor){
      if(major_opcode){ free(major_opcode) }
      if(event_base){ free(event_base) }
      if(error_base){ free(error_base) }
      if(major){ free(major) }
      if(minor){ free(minor) }
      return out
   }

   mut available = false
   if(_safe_XQueryExtension(display, "XInputExtension", major_opcode, event_base, error_base) != 0){
      store32(major, XI_2_Major, 0)
      store32(minor, XI_2_Minor, 0)
      available = XIQueryVersion(display, major, minor) == Success
   }

   out = dict_set(out, "available", available)
   out = dict_set(out, "major_opcode", available ? load32(major_opcode, 0) : -1)
   out = dict_set(out, "event_base", available ? load32(event_base, 0) : -1)
   out = dict_set(out, "error_base", available ? load32(error_base, 0) : -1)
   out = dict_set(out, "major", available ? load32(major, 0) : XI_2_Major)
   out = dict_set(out, "minor", available ? load32(minor, 0) : XI_2_Minor)

   free(major_opcode)
   free(event_base)
   free(error_base)
   free(major)
   free(minor)
   out
}

fn _xi_set_raw_motion_enabled(display, root, enabled){
   "Enables or disables XI2 raw motion events on the X11 root window."
   if(!display || !root){ return false }
   def mask_len = enabled ? 3 : 1
   def mask = calloc(mask_len, 1)
   def event_mask = calloc(1, 16)
   if(!mask || !event_mask){
      if(mask){ free(mask) }
      if(event_mask){ free(event_mask) }
      return false
   }

   if(enabled){
      store8(mask, (1 << (XI_RawMotion & 7)), XI_RawMotion >> 3)
   }
   store32(event_mask, XIAllMasterDevices, 0)
   store32(event_mask, mask_len, 4)
   store64_h(event_mask, mask, 8)
   def ok = XISelectEvents(display, root, event_mask, 1) == Success
   free(mask)
   free(event_mask)
   flush(display)
   ok
}

fn _ximask_is_set(mask, bit){
   "Checks whether a valuator bit is present in an XI2 mask."
   if(!mask || bit < 0){ return false }
   band(load8(mask, bit >> 3), (1 << band(bit, 7))) != 0
}

fn _translate_raw_motion_event(win, event_ptr, events){
   "Consumes one XI2 raw-motion GenericEvent and appends Ny mouse delta events."
   if(!win || !is_dict(win) || !event_ptr){ return [win, events] }
   def display = dict_get(win, "display", 0)
   if(!display){ return [win, events] }

   def cookie = ptr_add(event_ptr, 32)
   if(XGetEventData(display, cookie) == 0){
      return [win, events]
   }

   def evtype = load32(cookie, 4)
   def data = load64_h(cookie, 16)
   if(evtype != XI_RawMotion || !data){
      XFreeEventData(display, cookie)
      return [win, events]
   }

   def mask_len = load32(data, 64)
   def mask = load64_h(data, 72)
   def raw_values = load64_h(data, 88)
   mut dx = 0.0
   mut dy = 0.0
   if(mask_len > 0 && mask && raw_values){
      mut value_idx = 0
      mut axis = 0
      while(axis < mask_len * 8){
         if(_ximask_is_set(mask, axis)){
         def v = load64_f64(raw_values, value_idx * 8)
         if(axis == 0){ dx = v }
         elif(axis == 1){ dy = v }
         value_idx += 1
         }
         axis += 1
      }
   }
   XFreeEventData(display, cookie)

   if(dx != 0.0 || dy != 0.0){
      def last_x = float(dict_get(win, "mouse_x", 0))
      def last_y = float(dict_get(win, "mouse_y", 0))
      def next_x = last_x + dx
      def next_y = last_y + dy
      win = dict_set(win, "mouse_x", next_x)
      win = dict_set(win, "mouse_y", next_y)
      mut data_ev = dict()
      data_ev = dict_set(data_ev, "x", next_x)
      data_ev = dict_set(data_ev, "y", next_y)
      data_ev = dict_set(data_ev, "dx", dx)
      data_ev = dict_set(data_ev, "dy", dy)
      data_ev = dict_set(data_ev, "moved", true)
      _push_translated_event(events, EVENT_MOUSE_POS_CHANGED, win, data_ev)
   }

   [win, events]
}

fn _x11_button_to_ny(button){
   "Maps X11 button ids to GLFW-compatible button indices."
   match button {
      Button1 -> { return 0 }
      Button2 -> { return 2 }
      Button3 -> { return 1 }
      _ -> {
         if(button > Button7){ return button - Button1 - 4 }
         return button
      }
   }
}

fn translate_event(win, event_ptr){
   "Translates one raw `XEvent` into zero or more std.ui events."
   if(!win || !is_dict(win) || !event_ptr){ return [win, []] }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def root = dict_get(win, "root", 0)
   def typ = load32(event_ptr, 0)
   def event_window = _actual_event_window(event_ptr, typ)
   mut events = []
   def randr_base = dict_get(win, "randr_event_base", -1)
   def xkb_base = dict_get(win, "xkb_event_base", -1)
   def xi_major_opcode = dict_get(win, "xi_major_opcode", -1)

   if(randr_base >= 0){
      if(typ == randr_base + RRScreenChangeNotify){
         XRRUpdateConfiguration(event_ptr)
         win = _xrandr_poll_outputs(win, events)
         def scale = _xrandr_event_scale(event_ptr)
         def sx = get(scale, 0, 1.0)
         def sy = get(scale, 1, 1.0)
         if(sx != dict_get(win, "scale_x", 1.0) || sy != dict_get(win, "scale_y", 1.0)){
         win = dict_set(win, "scale_x", sx)
         win = dict_set(win, "scale_y", sy)
         mut data = dict()
         data = dict_set(data, "xscale", sx)
         data = dict_set(data, "yscale", sy)
         _push_translated_event(events, EVENT_SCALE_UPDATED, win, data)
         }
         return [win, events]
      }
      if(typ == randr_base + RRNotify){
         XRRUpdateConfiguration(event_ptr)
         win = _xrandr_poll_outputs(win, events)
         return [win, events]
      }
   }

   if(xkb_base >= 0){
      if(typ == xkb_base + XkbEventCode){
         if(load32(event_ptr, 40) == XkbStateNotify && band(load32(event_ptr, 48), XkbGroupStateMask)){
         win = dict_set(win, "xkb_group", load32(event_ptr, 52))
         }
         return [win, events]
      }
   }

   if(typ == GenericEvent){
      if(dict_get(win, "xi_available", false) &&
         dict_get(win, "disabled_cursor", false) &&
         dict_get(win, "raw_mouse_motion", false) &&
         load32(event_ptr, 64) == xi_major_opcode){
         return _translate_raw_motion_event(win, event_ptr, events)
      }
      return [win, events]
   }

   if(event_window && handle && event_window != handle){
      return [win, events]
   }

   def filtered = XFilterEvent(event_ptr, 0) != 0
   if(filtered && typ != KeyPress){
      return [win, events]
   }

   match typ {
      ReparentNotify -> {
         win = dict_set(win, "parent", load64_h(event_ptr, 32))
      }
      KeyPress -> {
         def scancode = load32(event_ptr, 84)
         def key = translate_scancode(scancode)
         def mods = translate_state(load32(event_ptr, 80))
         def plain = !band(mods, bor(MOD_CONTROL, MOD_ALT))
         def timestamp = load64_h(event_ptr, 56)
         mut key_states = dict_get(win, "key_states", dict(64))
         key_states = dict_set(key_states, key, true)
         def is_repeat = dict_get(win, "repeat_scancode", -1) == scancode
         if(is_repeat){ win = dict_set(win, "repeat_scancode", -1) }
         mut data = dict()
         data = dict_set(data, "raw_key", scancode)
         data = dict_set(data, "key", key)
         data = dict_set(data, "scancode", scancode)
         data = dict_set(data, "action", is_repeat ? ACTION_REPEAT : ACTION_PRESS)
         data = dict_set(data, "mod", mods)
         win = dict_set(win, "key_states", key_states)
         win = dict_set(win, "modifiers", mods)
         mut key_press_times = dict_get(win, "key_press_times", dict(64))
         def last_press = dict_get(key_press_times, scancode, 0)
         if(!dict_get(win, "ic", 0) || last_press != timestamp){
         _push_translated_event(events, EVENT_KEY_PRESSED, win, data)
         key_press_times = dict_set(key_press_times, scancode, timestamp)
         win = dict_set(win, "key_press_times", key_press_times)
         }

         if(dict_get(win, "ic", 0)){
         if(!filtered){
               events = _emit_ic_chars(events, win, event_ptr, mods, plain)
         }
         } else {
         def keysym_ptr = malloc(8)
         if(keysym_ptr){
               store64_h(keysym_ptr, 0, 0)
               XLookupString(event_ptr, 0, 0, keysym_ptr, 0)
               def codepoint = x11_keymap.keysym_to_unicode(load64_h(keysym_ptr, 0))
               if(codepoint >= 0){
                  mut char_data = dict()
                  char_data = dict_set(char_data, "char", codepoint)
                  char_data = dict_set(char_data, "mod", mods)
                  char_data = dict_set(char_data, "plain", plain)
                  _push_translated_event(events, EVENT_KEY_CHAR, win, char_data)
               }
               free(keysym_ptr)
         }
         }
      }
      KeyRelease -> {
         def scancode = load32(event_ptr, 84)
         def time = load64_h(event_ptr, 56)
         def mods = translate_state(load32(event_ptr, 80))
         ;; Check for X11 auto-repeat: KeyRelease immediately followed by KeyPress with same scancode+time
         mut is_repeat = false
         def QueuedAfterReading = 2
         if(XEventsQueued(display, QueuedAfterReading) > 0){
         def peek_buf = calloc(1, 192)
         if(peek_buf){
               XPeekEvent(display, peek_buf)
               def peek_typ = load32(peek_buf, 0)
               def peek_sc = load32(peek_buf, 84)
               def peek_time = load64_h(peek_buf, 56)
               if(peek_typ == KeyPress && peek_sc == scancode && peek_time == time){
                  is_repeat = true
                  win = dict_set(win, "repeat_scancode", scancode)
               }
               free(peek_buf)
         }
         }
         if(!is_repeat){
         mut key_states = dict_get(win, "key_states", dict(64))
         key_states = dict_set(key_states, translate_scancode(scancode), false)
         mut data = dict()
         data = dict_set(data, "raw_key", scancode)
         data = dict_set(data, "key", translate_scancode(scancode))
         data = dict_set(data, "scancode", scancode)
         data = dict_set(data, "action", 0)
         data = dict_set(data, "mod", mods)
         win = dict_set(win, "key_states", key_states)
         win = dict_set(win, "modifiers", mods)
         _push_translated_event(events, EVENT_KEY_RELEASED, win, data)
         }
      }
      ButtonPress -> {
         def button = load32(event_ptr, 84)
         def mods = translate_state(load32(event_ptr, 80))
         if(button == Button4 || button == Button5 || button == Button6 || button == Button7){
         mut data = dict()
         if(button == Button4){
               data = dict_set(data, "dx", 0.0)
               data = dict_set(data, "dy", 1.0)
         } elif(button == Button5){
               data = dict_set(data, "dx", 0.0)
               data = dict_set(data, "dy", -1.0)
         } elif(button == Button6){
               data = dict_set(data, "dx", 1.0)
               data = dict_set(data, "dy", 0.0)
         } else {
               data = dict_set(data, "dx", -1.0)
               data = dict_set(data, "dy", 0.0)
         }
         data = dict_set(data, "scrolling", true)
         data = dict_set(data, "mod", mods)
         win = dict_set(win, "modifiers", mods)
         _push_translated_event(events, EVENT_MOUSE_SCROLL, win, data)
         } else {
         def x = load32(event_ptr, 64)
         def y = load32(event_ptr, 68)
         mut mouse_buttons = dict_get(win, "mouse_buttons", dict(8))
         mouse_buttons = dict_set(mouse_buttons, _x11_button_to_ny(button), true)
         mut data = dict()
         data = dict_set(data, "button", _x11_button_to_ny(button))
         data = dict_set(data, "x", x)
         data = dict_set(data, "y", y)
         data = dict_set(data, "mod", mods)
         win = dict_set(win, "mouse_buttons", mouse_buttons)
         win = dict_set(win, "modifiers", mods)
         win = dict_set(win, "mouse_x", x)
         win = dict_set(win, "mouse_y", y)
         _push_translated_event(events, EVENT_MOUSE_BUTTON_PRESSED, win, data)
         }
      }
      ButtonRelease -> {
         def button = load32(event_ptr, 84)
         if(button <= Button3 || button > Button7){
         def x = load32(event_ptr, 64)
         def y = load32(event_ptr, 68)
         def mods = translate_state(load32(event_ptr, 80))
         mut mouse_buttons = dict_get(win, "mouse_buttons", dict(8))
         mouse_buttons = dict_set(mouse_buttons, _x11_button_to_ny(button), false)
         mut data = dict()
         data = dict_set(data, "button", _x11_button_to_ny(button))
         data = dict_set(data, "x", x)
         data = dict_set(data, "y", y)
         data = dict_set(data, "mod", mods)
         win = dict_set(win, "mouse_buttons", mouse_buttons)
         win = dict_set(win, "modifiers", mods)
         win = dict_set(win, "mouse_x", x)
         win = dict_set(win, "mouse_y", y)
         _push_translated_event(events, EVENT_MOUSE_BUTTON_RELEASED, win, data)
         }
      }
      EnterNotify -> {
         def x = load32(event_ptr, 64)
         def y = load32(event_ptr, 68)
         win = dict_set(win, "mouse_x", x)
         win = dict_set(win, "mouse_y", y)
         mut enter_data = dict()
         enter_data = dict_set(enter_data, "x", x)
         enter_data = dict_set(enter_data, "y", y)
         _push_translated_event(events, EVENT_MOUSE_ENTER, win, enter_data)
         mut pos_data = dict()
         pos_data = dict_set(pos_data, "x", x)
         pos_data = dict_set(pos_data, "y", y)
         pos_data = dict_set(pos_data, "dx", 0)
         pos_data = dict_set(pos_data, "dy", 0)
         pos_data = dict_set(pos_data, "moved", false)
         _push_translated_event(events, EVENT_MOUSE_POS_CHANGED, win, pos_data)
      }
      LeaveNotify -> {
         _push_translated_event(events, EVENT_MOUSE_LEAVE, win, 0)
      }
      MotionNotify -> {
         def x = load32(event_ptr, 64)
         def y = load32(event_ptr, 68)
         if(dict_get(win, "cursor_mode", GLFW_CURSOR_NORMAL) == GLFW_CURSOR_DISABLED){
         if(dict_get(win, "xi_available", false) && dict_get(win, "raw_mouse_motion", false)){
               if(dict_get(win, "ignore_warp_motion", false) &&
                  x == dict_get(win, "warp_cursor_x", x) &&
                  y == dict_get(win, "warp_cursor_y", y)){
                  win = dict_set(win, "ignore_warp_motion", false)
               }
               return [win, events]
         }
         if(dict_get(win, "ignore_warp_motion", false) &&
               x == dict_get(win, "warp_cursor_x", x) &&
               y == dict_get(win, "warp_cursor_y", y)){
               win = dict_set(win, "ignore_warp_motion", false)
               return [win, events]
         }

         def center_x = int(dict_get(win, "w", 1) / 2)
         def center_y = int(dict_get(win, "h", 1) / 2)
         def dx = x - center_x
         def dy = y - center_y
         if(dx != 0 || dy != 0){
               mut data = dict()
               data = dict_set(data, "x", center_x)
               data = dict_set(data, "y", center_y)
               data = dict_set(data, "dx", dx)
               data = dict_set(data, "dy", dy)
               data = dict_set(data, "moved", true)
               win = dict_set(win, "mouse_x", center_x)
               win = dict_set(win, "mouse_y", center_y)
               win = dict_set(win, "warp_cursor_x", center_x)
               win = dict_set(win, "warp_cursor_y", center_y)
               win = dict_set(win, "ignore_warp_motion", true)
               _set_cursor_pos_raw(display, handle, center_x, center_y)
               _push_translated_event(events, EVENT_MOUSE_POS_CHANGED, win, data)
         }
         return [win, events]
         }

         def last_x = dict_get(win, "mouse_x", x)
         def last_y = dict_get(win, "mouse_y", y)
         win = dict_set(win, "mouse_x", x)
         win = dict_set(win, "mouse_y", y)
         mut data = dict()
         data = dict_set(data, "x", x)
         data = dict_set(data, "y", y)
         data = dict_set(data, "dx", x - last_x)
         data = dict_set(data, "dy", y - last_y)
         data = dict_set(data, "moved", x != last_x || y != last_y)
         _push_translated_event(events, EVENT_MOUSE_POS_CHANGED, win, data)
      }
      ConfigureNotify -> {
         def width = load32(event_ptr, 56)
         def height = load32(event_ptr, 60)
         def xpos = load32(event_ptr, 48)
         def ypos = load32(event_ptr, 52)
         if(width != dict_get(win, "w", width) || height != dict_get(win, "h", height)){
         win = dict_set(win, "w", width)
         win = dict_set(win, "h", height)
         mut data = dict()
         data = dict_set(data, "w", width)
         data = dict_set(data, "h", height)
         _push_translated_event(events, EVENT_WINDOW_RESIZED, win, data)
         }
         if(xpos != dict_get(win, "x", xpos) || ypos != dict_get(win, "y", ypos)){
         win = dict_set(win, "x", xpos)
         win = dict_set(win, "y", ypos)
         mut data = dict()
         data = dict_set(data, "x", xpos)
         data = dict_set(data, "y", ypos)
         _push_translated_event(events, EVENT_WINDOW_MOVED, win, data)
         }
      }
      ClientMessage -> {
         def message_type = load64_h(event_ptr, 40)
         def protocol = load64_h(event_ptr, 56)
         if(message_type == dict_get(win, "wm_protocols", 0) && protocol == dict_get(win, "wm_delete", 0)){
         win = dict_set(win, "should_close", true)
         _push_translated_event(events, EVENT_QUIT, win, 0)
         } elif(message_type == dict_get(win, "wm_protocols", 0) && protocol == dict_get(win, "net_wm_ping", 0)){
         _reply_wm_ping(display, root, event_ptr)
         } elif(message_type == dict_get(win, "xdnd_enter", 0)){
         def source = _xevent_client_l(event_ptr, 0)
         def version = bshr(_xevent_client_l(event_ptr, 1), 24)
         def use_list = band(_xevent_client_l(event_ptr, 1), 1) != 0
         if(version > XDND_VERSION){
               win = _clear_xdnd_state(win)
               return [win, events]
         }
         mut offered = []
         if(!use_list){
               offered = append(offered, _xevent_client_l(event_ptr, 2))
               offered = append(offered, _xevent_client_l(event_ptr, 3))
               offered = append(offered, _xevent_client_l(event_ptr, 4))
         }
         def format = _xdnd_pick_format(display, source, offered,
         dict_get(win, "text_uri_list", 0),
         use_list ? dict_get(win, "xdnd_type_list", 0) : 0)
         win = dict_set(win, "xdnd_source", source)
         win = dict_set(win, "xdnd_version", version)
         win = dict_set(win, "xdnd_format", format)
         } elif(message_type == dict_get(win, "xdnd_position", 0)){
         def source = _xevent_client_l(event_ptr, 0)
         if(source == dict_get(win, "xdnd_source", 0)){
               if(dict_get(win, "xdnd_version", 0) > XDND_VERSION){
                  return [win, events]
               }
               def packed = _xevent_client_l(event_ptr, 2)
               def xabs = band(bshr(packed, 16), 0xffff)
               def yabs = band(packed, 0xffff)
               def pos = _translate_root_to_window(display, root, handle, xabs, yabs)
               def x = get(pos, 0, 0)
               def y = get(pos, 1, 0)
               win = dict_set(win, "mouse_x", x)
               win = dict_set(win, "mouse_y", y)
               mut data = dict()
               data = dict_set(data, "x", x)
               data = dict_set(data, "y", y)
               data = dict_set(data, "accept", !!dict_get(win, "xdnd_format", 0))
               data = dict_set(data, "format", dict_get(win, "xdnd_format", 0))
               _push_translated_event(events, EVENT_DATA_DRAG, win, data)
               _send_xdnd_status(display, source, win,
                  !!dict_get(win, "xdnd_format", 0),
                  dict_get(win, "xdnd_version", 0) >= 2 ? dict_get(win, "xdnd_action_copy", 0) : 0)
         }
         } elif(message_type == dict_get(win, "xdnd_drop", 0)){
         def source = _xevent_client_l(event_ptr, 0)
         if(source == dict_get(win, "xdnd_source", 0)){
               if(dict_get(win, "xdnd_version", 0) > XDND_VERSION){
                  return [win, events]
               }
               if(dict_get(win, "xdnd_format", 0)){
                  mut time = CurrentTime
                  if(dict_get(win, "xdnd_version", 0) >= 1){
                     time = _xevent_client_l(event_ptr, 2)
                  }
                  XConvertSelection(display,
                     dict_get(win, "xdnd_selection", 0),
                     dict_get(win, "xdnd_format", 0),
                     dict_get(win, "xdnd_selection", 0),
                     handle, time)
                  flush(display)
               } else {
                  if(dict_get(win, "xdnd_version", 0) >= 2){
                     _send_xdnd_finished(display, source, win, false, 0)
                  }
                  win = _clear_xdnd_state(win)
               }
         }
         }
      }
      SelectionClear -> {
         def selection = load64_h(event_ptr, 40)
         if(selection == dict_get(win, "clipboard_atom", 0)){
         win = dict_set(win, "clipboard_owned", false)
         } elif(selection == dict_get(win, "primary_atom", 0)){
         win = dict_set(win, "primary_owned", false)
         }
      }
      SelectionRequest -> {
         def requestor = load64_h(event_ptr, 40)
         def selection = load64_h(event_ptr, 48)
         def target = load64_h(event_ptr, 56)
         def property = load64_h(event_ptr, 64)
         def time = load64_h(event_ptr, 72)
         def reply_property = _write_selection_to_property(display, requestor, property, target,
         _selection_text_for_request(win, selection),
         dict_get(win, "utf8_string", 0),
         dict_get(win, "targets_atom", 0),
         dict_get(win, "multiple_atom", 0),
         dict_get(win, "atom_pair_atom", 0),
         dict_get(win, "save_targets", 0))
         _send_selection_notify(display, requestor, selection, target, reply_property, time)
      }
      SelectionNotify -> {
         if(load64_h(event_ptr, 56) == dict_get(win, "xdnd_selection", 0)){
         def property = load64_h(event_ptr, 56)
         def target = load64_h(event_ptr, 48)
         mut accepted = false
         if(property){
               def prop = get_window_property(display, handle, property,
                  target ? target : AnyPropertyType)
               if(prop && is_dict(prop)){
               def data_ptr = dict_get(prop, "data", 0)
               if(data_ptr){
                  def raw = glfw_common._glfw_strdup(data_ptr)
                  def paths = glfw_common._glfwParseUriList(raw)
                  mut data = dict()
                  data = dict_set(data, "paths", paths)
                  _push_translated_event(events, EVENT_DATA_DROP, win, data)
                  XFree(data_ptr)
                  accepted = is_list(paths) && len(paths) > 0
               }
               }
               XDeleteProperty(display, handle, property)
               flush(display)
         }
         if(dict_get(win, "xdnd_version", 0) >= 2){
               _send_xdnd_finished(display, dict_get(win, "xdnd_source", 0), win,
                  accepted, dict_get(win, "xdnd_action_copy", 0))
         }
         win = _clear_xdnd_state(win)
         }
      }
      FocusIn -> {
         def mode = load32(event_ptr, 40)
         if(mode != NotifyGrab && mode != NotifyUngrab){
         def ic = dict_get(win, "ic", 0)
         if(ic){ XSetICFocus(ic) }
         win = set_input_mode(win, GLFW_RAW_MOUSE_MOTION,
         dict_get(win, "raw_mouse_motion", false) ? 1 : 0)
         win = set_input_mode(win, GLFW_CURSOR_MODE,
         dict_get(win, "cursor_mode", GLFW_CURSOR_NORMAL))
         win = dict_set(win, "focused", true)
         _push_translated_event(events, EVENT_FOCUS_IN, win, 0)
         }
      }
      FocusOut -> {
         def mode = load32(event_ptr, 40)
         if(mode != NotifyGrab && mode != NotifyUngrab){
         def ic = dict_get(win, "ic", 0)
         if(ic){ XUnsetICFocus(ic) }
         if(dict_get(win, "captured_cursor", false) || dict_get(win, "disabled_cursor", false)){
               _release_cursor(display)
               _set_cursor_visibility(display, handle, true)
               win = dict_set(win, "captured_cursor", false)
               win = dict_set(win, "disabled_cursor", false)
               win = dict_set(win, "ignore_warp_motion", false)
         }
         win = dict_set(win, "focused", false)
         _push_translated_event(events, EVENT_FOCUS_OUT, win, 0)
         }
      }
      Expose -> {
         win = dict_set(win, "mapped", true)
         _push_translated_event(events, EVENT_WINDOW_REFRESH, win, 0)
      }
      PropertyNotify -> {
         if(load32(event_ptr, 56) == PropertyNewValue){
         def atom = load64_h(event_ptr, 40)
         if(atom == dict_get(win, "wm_state", 0)){
               def state = get_window_state(display, handle, atom)
               if(state == IconicState || state == NormalState){
                  def iconified = state == IconicState
                  if(dict_get(win, "iconified", false) != iconified){
                     win = dict_set(win, "iconified", iconified)
                     _push_translated_event(events, iconified ? EVENT_WINDOW_MINIMIZED : EVENT_WINDOW_RESTORED, win, 0)
                  }
               }
         } elif(atom == dict_get(win, "net_wm_state", 0)){
               def maximized = is_window_maximized(display, handle, atom,
                  dict_get(win, "net_wm_state_maximized_vert", 0),
                  dict_get(win, "net_wm_state_maximized_horz", 0))
               if(dict_get(win, "maximized", false) != maximized){
                  win = dict_set(win, "maximized", maximized)
                  _push_translated_event(events, maximized ? EVENT_WINDOW_MAXIMIZED : EVENT_WINDOW_RESTORED, win, 0)
               }
         }
         }
      }
      DestroyNotify -> {
         win = dict_set(win, "should_close", true)
         _push_translated_event(events, EVENT_QUIT, win, 0)
      }
   }

   [win, events]
}

fn poll_window_events(win, max_events=64){
   "Polls queued X11 events for `win` and returns `[updated_win, events]`."
   if(!win || !is_dict(win)){ return [win, []] }
   def display = dict_get(win, "display", 0)
   if(!display){ return [win, []] }

   def event_buf = calloc(1, 192)
   if(!event_buf){ return [win, []] }

   mut out = []
   mut count = 0
   while(pending(display) > 0 && count < max_events){
      next_event(display, event_buf)
      def typ = load32(event_buf, 0)
      _dbgu("x11 event type=" + to_str(typ) + " pending=" + to_str(pending(display)))
      def translated = translate_event(win, event_buf)
      win = get(translated, 0)
      def events = get(translated, 1, [])
      if(is_list(events) && len(events) > 0){
         _dbgu("x11 dispatched " + to_str(len(events)) + " events from type=" + to_str(typ))
         out = extend(out, events)
      }
      count += 1
   }

   free(event_buf)
   _dbgu("poll_window_events: " + to_str(count) + " raw events, " + to_str(len(out)) + " dispatched")
   [win, out]
}

fn translate_state(state){
   "Translates an X11 modifier mask to Ny window modifier flags."
   mut mods = 0
   if(band(state, ShiftMask)){ mods = bor(mods, MOD_SHIFT) }
   if(band(state, ControlMask)){ mods = bor(mods, MOD_CONTROL) }
   if(band(state, Mod1Mask)){ mods = bor(mods, MOD_ALT) }
   if(band(state, Mod4Mask)){ mods = bor(mods, MOD_SUPER) }
   mods
}

fn translate_keysym(primary, secondary=0, width=1){
   "Translates X11 keysyms using the direct Ny port of GLFW fallback logic."
   x11_keymap.translate_keysym(primary, secondary, width)
}

fn translate_scancode(scancode){
   "Translates common X11 hardware scancodes to Ny key codes."
   x11_keymap.translate_scancode(scancode)
}

fn next_event(display, event_ptr){
   "Reads the next X11 event into `event_ptr`."
   XNextEvent(display, event_ptr)
}

fn post_empty_event(win){
   "Posts a dummy ClientMessage event to unblock event waiting."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return false }
   def ev = calloc(1, 192)
   if(!ev){ return false }
   store32(ev, ClientMessage, 0)
   store64_h(ev, handle, 32)
   store64_h(ev, dict_get(win, "wm_protocols", 0), 40)
   store32(ev, 32, 48)
   XSendEvent(display, handle, 0, 0, ev)
   flush(display)
   free(ev)
   true
}

fn select_input(display, win, event_mask){
   "Selects the X11 event mask for a window."
   XSelectInput(display, win, event_mask)
}

fn flush(display){
   "Flushes pending X11 requests."
   XFlush(display)
}

fn sync(display, discard=false){
   "Synchronizes with the X server."
   XSync(display, discard ? 1 : 0)
}

fn store_name(display, win, window_name, net_wm_name_atom=0, net_wm_icon_name_atom=0, utf8_string_atom=0){
   "Sets the X11 window title via both ICCCM and EWMH UTF-8 properties."
   if(!display || !win){ return false }
   if(!is_str(window_name)){ window_name = to_str(window_name) }
   XStoreName(display, win, cstr(window_name))
   if(!net_wm_name_atom){ net_wm_name_atom = intern_atom(display, "_NET_WM_NAME") }
   if(!net_wm_icon_name_atom){ net_wm_icon_name_atom = intern_atom(display, "_NET_WM_ICON_NAME") }
   if(!utf8_string_atom){ utf8_string_atom = intern_atom(display, "UTF8_STRING") }
   if(net_wm_name_atom && utf8_string_atom){
      _set_utf8_text_property(display, win, net_wm_name_atom, utf8_string_atom, window_name)
   }
   if(net_wm_icon_name_atom && utf8_string_atom){
      _set_utf8_text_property(display, win, net_wm_icon_name_atom, utf8_string_atom, window_name)
   }
   flush(display)
   true
}

fn get_pos(win){
   "Returns the X11 window position as [x, y] via XTranslateCoordinates."
   if(!win || !is_dict(win)){ return [0, 0] }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def root = dict_get(win, "root", 0)
   if(!display || !handle || !root){ return [dict_get(win, "x", 0), dict_get(win, "y", 0)] }
   def xp = malloc(4)
   def yp = malloc(4)
   def child = malloc(8)
   if(!xp || !yp || !child){
      if(xp){ free(xp) } if(yp){ free(yp) } if(child){ free(child) }
      return [dict_get(win, "x", 0), dict_get(win, "y", 0)]
   }
   store32(xp, 0, 0) store32(yp, 0, 0) store64_h(child, 0, 0)
   XTranslateCoordinates(display, handle, root, 0, 0, xp, yp, child)
   def rx = load32(xp, 0)
   def ry = load32(yp, 0)
   free(xp) free(yp) free(child)
   [rx, ry]
}

fn set_pos(win, x, y){
   "Moves the X11 window to [x, y]."
   if(!win || !is_dict(win)){ return false }
   move_window(dict_get(win, "display", 0), dict_get(win, "handle", 0), x, y)
}

fn move_window(display, win, x, y){
   "Moves a raw X11 window."
   XMoveWindow(display, win, x, y)
}

fn resize_window(display, win, width, height){
   "Resizes a raw X11 window."
   XResizeWindow(display, win, width, height)
}
fn create_basic_window(title, width, height, x=0, y=0, flags=0, event_mask=0, class_name="Nytrix", instance_name="nytrix"){
   "Creates and maps a basic X11 top-level window using Ny-side logic."
   _dbg("create_basic_window: title='" + title + "' pos=" + to_str(x) + "," + to_str(y) + " size=" + to_str(width) + "x" + to_str(height) + " flags=0x" + to_hex(flags))
   if(!available()){ _dbg_err("X11 not available") return false }
   def display = open_display()
   _dbg("  open_display=0x" + to_hex(display))
   if(!display){ _dbg_err("failed to open X11 display") return false }
   def im = _open_input_method(display)
   def xkb = dict()
   def xi = dict()
   def screen = default_screen(display)
   def root = root_window(display, screen)
   _dbg("  screen=" + to_str(screen) + " root=0x" + to_hex(root))
   def visual = default_visual(display, screen)
   def depth = default_depth(display, screen)
   def randr_available = false
   def randr_event_base = 0
   def randr_error_base = 0
   if(event_mask == 0){
      event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
         EnterWindowMask | LeaveWindowMask | PointerMotionMask | ExposureMask |
         FocusChangeMask | StructureNotifyMask | PropertyChangeMask
   }
   if(band(flags, WINDOW_CENTER)){
      def sw = XDisplayWidth(display, screen)
      def sh = XDisplayHeight(display, screen)
      if(sw > 0 && sh > 0){ x = (sw - width) / 2 y = (sh - height) / 2 }
   }
   def handle = create_window_raw(display, root, x, y, width, height, 0, depth, InputOutput, visual, 0, 0)
   XSync(display, 0)
   if(!handle){
      if(im){ XCloseIM(im) }
      close_display(display)
      return false
   }
   def ic = _create_input_context(im, handle)
   select_input(display, handle, event_mask)
   def wm_protocols = intern_atom(display, "WM_PROTOCOLS")
   def wm_delete = intern_atom(display, "WM_DELETE_WINDOW")
   def net_wm_ping = intern_atom(display, "_NET_WM_PING")
   if(wm_protocols && wm_delete){
      def protocol_count = net_wm_ping ? 2 : 1
      def atoms = calloc(protocol_count, 8)
      if(atoms){
         store32(atoms, wm_delete, 0)
         if(net_wm_ping){ store32(atoms, net_wm_ping, 8) }
         XSetWMProtocols(display, handle, atoms, protocol_count)
         free(atoms)
      }
   }

   def wm_state = intern_atom(display, "WM_STATE")
   def net_wm_pid = intern_atom(display, "_NET_WM_PID")
   def net_wm_fullscreen_monitors = intern_atom(display, "_NET_WM_FULLSCREEN_MONITORS")
   def net_wm_state = intern_atom(display, "_NET_WM_STATE")
   def net_wm_bypass_compositor = intern_atom(display, "_NET_WM_BYPASS_COMPOSITOR")
   def net_wm_window_type = intern_atom(display, "_NET_WM_WINDOW_TYPE")
   def net_wm_window_type_normal = intern_atom(display, "_NET_WM_WINDOW_TYPE_NORMAL")
   def net_wm_state_above = intern_atom(display, "_NET_WM_STATE_ABOVE")
   def net_wm_state_fullscreen = intern_atom(display, "_NET_WM_STATE_FULLSCREEN")
   def net_wm_state_maximized_vert = intern_atom(display, "_NET_WM_STATE_MAXIMIZED_VERT")
   def net_wm_state_maximized_horz = intern_atom(display, "_NET_WM_STATE_MAXIMIZED_HORZ")
   def motif_wm_hints = intern_atom(display, "_MOTIF_WM_HINTS")
   def net_wm_window_opacity = intern_atom(display, "_NET_WM_WINDOW_OPACITY")
   def clipboard_atom = intern_atom(display, "CLIPBOARD")
   def clipboard_manager = intern_atom(display, "CLIPBOARD_MANAGER")
   def primary_atom = intern_atom(display, "PRIMARY")
   def targets_atom = intern_atom(display, "TARGETS")
   def multiple_atom = intern_atom(display, "MULTIPLE")
   def atom_pair_atom = intern_atom(display, "ATOM_PAIR")
   def incr_atom = intern_atom(display, "INCR")
   def save_targets = intern_atom(display, "SAVE_TARGETS")
   def utf8_string = intern_atom(display, "UTF8_STRING")
   def net_wm_name = intern_atom(display, "_NET_WM_NAME")
   def net_wm_icon = intern_atom(display, "_NET_WM_ICON")
   def net_wm_icon_name = intern_atom(display, "_NET_WM_ICON_NAME")
   def selection_property = intern_atom(display, "NYTRIX_SELECTION")
   def xdnd_aware = intern_atom(display, "XdndAware")
   def xdnd_enter = intern_atom(display, "XdndEnter")
   def xdnd_position = intern_atom(display, "XdndPosition")
   def xdnd_status = intern_atom(display, "XdndStatus")
   def xdnd_action_copy = intern_atom(display, "XdndActionCopy")
   def xdnd_drop = intern_atom(display, "XdndDrop")
   def xdnd_finished = intern_atom(display, "XdndFinished")
   def xdnd_selection = intern_atom(display, "XdndSelection")
   def xdnd_type_list = intern_atom(display, "XdndTypeList")
   def text_uri_list = intern_atom(display, "text/uri-list")

   if(title){ store_name(display, handle, title, net_wm_name, net_wm_icon_name, utf8_string) }
   _set_window_pid(display, handle, net_wm_pid)
   _set_window_type_normal(display, handle, net_wm_window_type, net_wm_window_type_normal)
   _set_window_manager_hints(display, handle)
   _set_initial_normal_hints(display, handle, width, height, !band(flags, WINDOW_NO_RESIZE),
      x, y, x != 0 || y != 0)
   _set_class_hint(display, handle, instance_name, class_name)

   if(xdnd_aware){
      def xdnd_ver = calloc(1, 8)
      if(xdnd_ver){
         store32(xdnd_ver, XDND_VERSION, 0)
         XChangeProperty(display, handle, xdnd_aware, XA_ATOM, 32,
         PropModeReplace, xdnd_ver, 1)
         free(xdnd_ver)
      }
   }

   def resizable = !band(flags, WINDOW_NO_RESIZE)
   def decorated = !(band(flags, WINDOW_NO_BORDER) || band(flags, WINDOW_TRANSPARENT))
   def floating = band(flags, WINDOW_FLOATING)
   def maximized = band(flags, WINDOW_MAXIMIZE)
   def fullscreen = band(flags, WINDOW_FULLSCREEN)
   def hidden = band(flags, WINDOW_HIDE)
   def minimized = band(flags, WINDOW_MINIMIZE)
   def focus_on_show = band(flags, WINDOW_FOCUS) || band(flags, WINDOW_FOCUS_ON_SHOW)

   if(!decorated && motif_wm_hints){
      _set_window_decorated_raw(display, handle, motif_wm_hints, false)
   }
   if(!resizable){
      update_normal_hints(display, handle, width, height, false, false)
   }

   if(!hidden){
      _show_window_raw(display, handle, floating, net_wm_state, net_wm_state_above)
      if(maximized){
         _maximize_window_raw(display, root, handle, net_wm_state,
         net_wm_state_maximized_vert, net_wm_state_maximized_horz)
      }
      if(fullscreen){
         if(net_wm_state && net_wm_state_fullscreen){
         set_window_fullscreen(display, root, handle, net_wm_state, net_wm_state_fullscreen, true)
         } else {
         _set_override_redirect(display, handle, true)
         }
         if(net_wm_bypass_compositor && !band(flags, WINDOW_TRANSPARENT)){
         _set_compositor_bypass(display, handle, net_wm_bypass_compositor, true)
         }
      }
      if(minimized){
         _iconify_window_raw(display, handle, screen)
      }
      if(focus_on_show){
         _focus_window_raw(display, handle)
      }
   }

   mut win = dict()
   win = dict_set(win, "display", display)
   win = dict_set(win, "im", im)
   win = dict_set(win, "ic", ic)
   win = dict_set(win, "xkb_available", dict_get(xkb, "available", false))
   win = dict_set(win, "xkb_event_base", dict_get(xkb, "event_base", -1))
   win = dict_set(win, "xkb_error_base", dict_get(xkb, "error_base", -1))
   win = dict_set(win, "xkb_group", dict_get(xkb, "group", 0))
   win = dict_set(win, "xkb_detectable_repeat", dict_get(xkb, "detectable_repeat", false))
   win = dict_set(win, "xi_available", dict_get(xi, "available", false))
   win = dict_set(win, "xi_major_opcode", dict_get(xi, "major_opcode", -1))
   win = dict_set(win, "xi_event_base", dict_get(xi, "event_base", -1))
   win = dict_set(win, "xi_error_base", dict_get(xi, "error_base", -1))
   win = dict_set(win, "screen", screen)
   win = dict_set(win, "root", root)
   win = dict_set(win, "visual", visual)
   win = dict_set(win, "depth", depth)
   win = dict_set(win, "handle", handle)
   win = dict_set(win, "wm_protocols", wm_protocols)
   win = dict_set(win, "wm_delete", wm_delete)
   win = dict_set(win, "net_wm_ping", net_wm_ping)
   win = dict_set(win, "wm_state", wm_state)
   win = dict_set(win, "net_wm_pid", net_wm_pid)
   win = dict_set(win, "net_wm_fullscreen_monitors", net_wm_fullscreen_monitors)
   win = dict_set(win, "net_wm_state", net_wm_state)
   win = dict_set(win, "net_wm_bypass_compositor", net_wm_bypass_compositor)
   win = dict_set(win, "net_wm_window_type", net_wm_window_type)
   win = dict_set(win, "net_wm_window_type_normal", net_wm_window_type_normal)
   win = dict_set(win, "net_wm_state_above", net_wm_state_above)
   win = dict_set(win, "net_wm_state_fullscreen", net_wm_state_fullscreen)
   win = dict_set(win, "net_wm_state_maximized_vert", net_wm_state_maximized_vert)
   win = dict_set(win, "net_wm_state_maximized_horz", net_wm_state_maximized_horz)
   win = dict_set(win, "motif_wm_hints", motif_wm_hints)
   win = dict_set(win, "net_wm_window_opacity", net_wm_window_opacity)
   win = dict_set(win, "clipboard_atom", clipboard_atom)
   win = dict_set(win, "clipboard_manager", clipboard_manager)
   win = dict_set(win, "primary_atom", primary_atom)
   win = dict_set(win, "targets_atom", targets_atom)
   win = dict_set(win, "multiple_atom", multiple_atom)
   win = dict_set(win, "atom_pair_atom", atom_pair_atom)
   win = dict_set(win, "incr_atom", incr_atom)
   win = dict_set(win, "save_targets", save_targets)
   win = dict_set(win, "utf8_string", utf8_string)
   win = dict_set(win, "net_wm_name", net_wm_name)
   win = dict_set(win, "net_wm_icon", net_wm_icon)
   win = dict_set(win, "net_wm_icon_name", net_wm_icon_name)
   win = dict_set(win, "selection_property", selection_property)
   win = dict_set(win, "xdnd_aware", xdnd_aware)
   win = dict_set(win, "xdnd_enter", xdnd_enter)
   win = dict_set(win, "xdnd_position", xdnd_position)
   win = dict_set(win, "xdnd_status", xdnd_status)
   win = dict_set(win, "xdnd_action_copy", xdnd_action_copy)
   win = dict_set(win, "xdnd_drop", xdnd_drop)
   win = dict_set(win, "xdnd_finished", xdnd_finished)
   win = dict_set(win, "xdnd_selection", xdnd_selection)
   win = dict_set(win, "xdnd_type_list", xdnd_type_list)
   win = dict_set(win, "text_uri_list", text_uri_list)
   win = dict_set(win, "xdnd_source", 0)
   win = dict_set(win, "xdnd_version", 0)
   win = dict_set(win, "xdnd_format", 0)
   win = dict_set(win, "title", title ? to_str(title) : "")
   win = dict_set(win, "clipboard_string", "")
   win = dict_set(win, "clipboard_owned", false)
   win = dict_set(win, "primary_selection_string", "")
   win = dict_set(win, "primary_owned", false)
   win = dict_set(win, "key_states", dict(64))
   win = dict_set(win, "key_press_times", dict(64))
   win = dict_set(win, "mouse_buttons", dict(8))
   win = dict_set(win, "x", x)
   win = dict_set(win, "y", y)
   win = dict_set(win, "w", width)
   win = dict_set(win, "h", height)
   win = dict_set(win, "resizable", resizable)
   win = dict_set(win, "decorated", decorated)
   win = dict_set(win, "floating", floating)
   win = dict_set(win, "fullscreen", fullscreen)
   win = dict_set(win, "override_redirect", fullscreen && !(net_wm_state && net_wm_state_fullscreen))
   win = dict_set(win, "visible", !hidden)
   win = dict_set(win, "mapped", !hidden)
   win = dict_set(win, "focused", focus_on_show)
   win = dict_set(win, "iconified", minimized)
   win = dict_set(win, "maximized", maximized)
   mut has_compositor = false
   if(band(flags, WINDOW_TRANSPARENT)){
      def cm_name = f"_NET_WM_CM_S{screen}"
      def cm_atom = intern_atom(display, cm_name)
      if(cm_atom){ has_compositor = XGetSelectionOwner(display, cm_atom) != 0 }
   }
   win = dict_set(win, "transparent", band(flags, WINDOW_TRANSPARENT) && has_compositor)
   win = dict_set(win, "flags", flags)
   _dbg("transparent=" + to_str(band(flags, WINDOW_TRANSPARENT) && has_compositor) + " compositor=" + to_str(has_compositor) + " decorated=" + to_str(decorated) + " resizable=" + to_str(resizable))
   win = dict_set(win, "cursor", 0)
   win = dict_set(win, "cursor_handle", 0)
   win = dict_set(win, "randr_available", randr_available)
   win = dict_set(win, "randr_event_base", randr_event_base ? load32(randr_event_base, 0) : -1)
   win = dict_set(win, "randr_error_base", randr_error_base ? load32(randr_error_base, 0) : -1)
   if(randr_event_base){ free(randr_event_base) }
   if(randr_error_base){ free(randr_error_base) }
   win = dict_set(win, "raw_mouse_motion", band(flags, WINDOW_RAW_MOUSE))
   win = dict_set(win, "cursor_mode",
      band(flags, WINDOW_CAPTURE_MOUSE) ? GLFW_CURSOR_CAPTURED :
      (band(flags, WINDOW_HIDE_MOUSE) ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL))
   win = dict_set(win, "captured_cursor", false)
   win = dict_set(win, "disabled_cursor", false)
   win = dict_set(win, "restore_cursor_x", 0)
   win = dict_set(win, "restore_cursor_y", 0)
   win = dict_set(win, "warp_cursor_x", 0)
   win = dict_set(win, "warp_cursor_y", 0)
   win = dict_set(win, "ignore_warp_motion", false)
   win = dict_set(win, "randr_outputs", dict())
   win = dict_set(win, "scale_x", 1.0)
   win = dict_set(win, "scale_y", 1.0)

   if(!hidden){
      if(ic && focus_on_show){ XSetICFocus(ic) }
      win = set_input_mode(win, GLFW_RAW_MOUSE_MOTION,
         dict_get(win, "raw_mouse_motion", false) ? 1 : 0)
      def initial_cursor_mode = dict_get(win, "cursor_mode", GLFW_CURSOR_NORMAL)
      if(initial_cursor_mode != GLFW_CURSOR_NORMAL){
         win = set_input_mode(win, GLFW_CURSOR_MODE, initial_cursor_mode)
      }
   }
   win
}

fn get_key_name(win, key, scancode){
   "Returns the layout-specific name of the specified printable key."
   if(!win || !is_dict(win)){ return "" }
   def display = dict_get(win, "display", 0)
   if(!display){ return "" }
   mut code = int(scancode)
   if(code == 0){
      def keysym = x11_keymap.keysym_from_key(key)
      if(keysym == 0){ return "" }
      code = int(XKeysymToKeycode(display, keysym))
      if(code == 0){ return "" }
   }
   def keysym = XkbKeycodeToKeysym(display, code, 0, 0)
   if(keysym == 0){ return "" }
   def name_ptr = XKeysymToString(keysym)
   name_ptr ? str.cstr_to_str(name_ptr) : ""
}

fn get_size(win){
   "Returns the X11 window size as [width, height]."
   if(!win || !is_dict(win)){ return [0, 0] }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   def sz = get_window_size(display, handle)
   if(!sz || !is_dict(sz)){ return [dict_get(win, "w", 0), dict_get(win, "h", 0)] }
   [dict_get(sz, "width", 0), dict_get(sz, "height", 0)]
}

fn close_basic_window(win){
   "Destroys a basic Ny-created X11 window and closes its display connection."
   if(!win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def im = dict_get(win, "im", 0)
   def ic = dict_get(win, "ic", 0)
   def handle = dict_get(win, "handle", 0)
   if(dict_get(win, "clipboard_owned", false)){
      _push_selection_to_manager(win)
   }
   if(ic){ XDestroyIC(ic) }
   if(handle && display){ destroy_window_raw(display, handle) }
   if(im){ XCloseIM(im) }
   if(display){ close_display(display) }
   true
}
;; Platform-specific Vulkan Surface Setup

fn create_surface(instance, win, allocator, surface){
   "Creates a Vulkan Xlib surface for the given backend window."
   if(!is_dict(win)){
      _dbg_err("create_surface: win is not a dict")
      return -1
   }
   mut display = dict_get(win, "display", 0)
   mut handle = dict_get(win, "handle", 0)
   if(is_dict(handle)){
       if(!display){ display = dict_get(handle, "display", 0) }
       handle = dict_get(handle, "handle", 0)
   }
   if(!display || !handle){
      _dbg_err("create_surface: missing display/handle display=0x" + to_hex(display) + " handle=0x" + to_hex(handle))
      return -1
   }

   def info = malloc(48)
   memset(info, 0, 48)
   store32(info, 1000004000, 0) ;; VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR
   store64_h(info, display, 24)
   store64_h(info, handle, 32)
   if(_is_debug()){ print("[x11] vkCreateXlibSurfaceKHR calling with surface_ptr=" + to_str(surface)) }
   def res = vkCreateXlibSurfaceKHR(instance, info, allocator, surface)
   if(_is_debug()){ print("[x11] vkCreateXlibSurfaceKHR returned " + to_str(res) + " surface_val=" + to_str(load64_h(surface, 0))) }
   free(info)
   if(res != 0){
      _dbg_err("create_surface: vkCreateXlibSurfaceKHR failed with " + to_str(res))
   }
   res
}

fn vulkan_get_surface_capabilities(phys, surf, caps){
   def res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, caps)
   if(res != 0 && _is_debug()){
      _dbg_err("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed with " + to_str(res))
   }
   res
}

fn vulkan_get_surface_support(phys, queue_family, surf, supported_ptr){
   def res = vkGetPhysicalDeviceSurfaceSupportKHR(phys, queue_family, surf, supported_ptr)
   if(_is_debug()){
      _dbg("surface_support family=" + to_str(queue_family) + " res=" + to_str(res) + " supported=" + to_str(load32(supported_ptr, 0)))
   }
   res
}
fn get_gamma_ramp(monitor){
   "Returns the XRandR gamma ramp for the given monitor."
   if(!is_dict(monitor)){ return 0 }
   def display = dict_get(monitor, "display", 0)
   def crtc = dict_get(monitor, "crtc", 0)
   if(!display || !crtc){ return 0 }
   def size = XRRGetCrtcGammaSize(display, crtc)
   if(size <= 0){ return 0 }
   def gamma = XRRGetCrtcGamma(display, crtc)
   if(!gamma){ return 0 }
   def red_ptr = load64_h(gamma, 0)
   def green_ptr = load64_h(gamma, 8)
   def blue_ptr = load64_h(gamma, 16)
   mut red = [] mut green = [] mut blue = []
   mut i = 0 while(i < size){
      red = append(red, load16(red_ptr, i * 2))
      green = append(green, load16(green_ptr, i * 2))
      blue = append(blue, load16(blue_ptr, i * 2))
      i += 1
   }
   XRRFreeCrtcGamma(gamma)
   mut res = dict()
   res = dict_set(res, "size", size)
   res = dict_set(res, "red", red)
   res = dict_set(res, "green", green)
   res = dict_set(res, "blue", blue)
   res
}

fn set_gamma_ramp(monitor, ramp){
   "Applies an XRandR gamma ramp to the given monitor."
   if(!is_dict(monitor) || !is_dict(ramp)){ return false }
   def display = dict_get(monitor, "display", 0)
   def crtc = dict_get(monitor, "crtc", 0)
   if(!display || !crtc){ return false }
   def size = dict_get(ramp, "size", 0)
   if(size <= 0){ return false }
   def red = dict_get(ramp, "red", [])
   def green = dict_get(ramp, "green", [])
   def blue = dict_get(ramp, "blue", [])
   if(len(red) < size || len(green) < size || len(blue) < size){ return false }
   def gamma = XRRAllocCrtcGamma(size)
   if(!gamma){ return false }
   def red_ptr = load64_h(gamma, 0)
   def green_ptr = load64_h(gamma, 8)
   def blue_ptr = load64_h(gamma, 16)
   mut i = 0 while(i < size){
      store16(red_ptr, get(red, i), i * 2)
      store16(green_ptr, get(green, i), i * 2)
      store16(blue_ptr, get(blue, i), i * 2)
      i += 1
   }
   XRRSetCrtcGamma(display, crtc, gamma)
   XRRFreeCrtcGamma(gamma)
   true
}

mut _x11_vk_ext_ptrs = 0
mut _x11_vk_ext_surface = 0
mut _x11_vk_ext_xlib = 0

fn vulkan_supported(){
   "Returns true if the X11 backend supports Vulkan (currently always true for Linux builds)."
   true
}

fn vulkan_required_extensions(){
   "Returns the Vulkan instance extensions required for X11 surfaces."
   if(!_x11_vk_ext_ptrs){
      _x11_vk_ext_surface = cstr("VK_KHR_surface")
      _x11_vk_ext_xlib = cstr("VK_KHR_xlib_surface")
      def arr = malloc(16)
      store64_h(arr, _x11_vk_ext_surface, 0)
      store64_h(arr, _x11_vk_ext_xlib, 8)
      _x11_vk_ext_ptrs = [2, arr]
   }
   _x11_vk_ext_ptrs
}

fn xdnd_begin_drag(win, data, mime_type="text/uri-list"){
   "Initiates an Xdnd drag from win as source. Returns false if setup fails."
   if(!available() || !win || !is_dict(win)){ return false }
   def display = dict_get(win, "display", 0)
   def handle = dict_get(win, "handle", 0)
   if(!display || !handle){ return false }
   def xdnd_type_list = intern_atom(display, "XdndTypeList")
   def nytrix_dnd_data = intern_atom(display, "_NYTRIX_DND_DATA")
   def xdnd_selection = intern_atom(display, "XdndSelection")
   def mime_atom = intern_atom(display, mime_type)
   if(!xdnd_type_list || !mime_atom){ return false }
   def type_buf = calloc(1, 8)
   if(!type_buf){ return false }
   store32(type_buf, mime_atom, 0)
   XChangeProperty(display, handle, xdnd_type_list, XA_ATOM, 32, PropModeReplace, type_buf, 1)
   free(type_buf)
   if(data && is_str(data) && xdnd_selection){
      def cs = cstr(data)
      def utf8 = intern_atom(display, "UTF8_STRING")
      if(utf8){
         XChangeProperty(display, handle, nytrix_dnd_data, utf8, 8, PropModeReplace, cs, str.len(data))
      }
      XSetSelectionOwner(display, xdnd_selection, handle, CurrentTime)
   }
   XGrabPointer(display, handle, 1,
      bor(ButtonPressMask, bor(ButtonReleaseMask, PointerMotionMask)),
      GrabModeAsync, GrabModeAsync, 0, 0, CurrentTime)
   flush(display)
   true
}

fn _handle_xdnd_status(win, display, event_ptr){
   "Handles incoming XdndStatus (target accepted/rejected) for a drag source."
   if(!win || !display || !event_ptr){ return win }
   def accepted = band(load64_h(event_ptr, 64), 1) != 0
   win = dict_set(win, "xdnd_drag_accepted", accepted)
   win
}

fn _handle_xdnd_finished(win, display, event_ptr){
   "Handles incoming XdndFinished (drop completed) for a drag source."
   if(!win || !display || !event_ptr){ return win }
   def success = band(load64_h(event_ptr, 64), 1) != 0
   win = dict_set(win, "xdnd_drag_finished", success)
   XUngrabPointer(display, CurrentTime)
   flush(display)
   win
}

fn set_video_mode(monitor, width, height, refresh_rate=0, display=0, root=0){
   "Sets the video mode for a monitor using XRandR. Returns true on success."
   if(!available() || !monitor || !is_dict(monitor)){ return false }
   def ctx = _resolve_monitor_context(display, root)
   if(!ctx){ return false }
   display = dict_get(ctx, "display", 0)
   root = dict_get(ctx, "root", 0)
   def resources = XRRGetScreenResourcesCurrent(display, root)
   if(!resources){
      _release_monitor_context(ctx)
      return false
   }
   def crtc = dict_get(monitor, "crtc", 0)
   def output = dict_get(monitor, "output", 0)
   if(!crtc || !output){
      XRRFreeScreenResources(resources)
      _release_monitor_context(ctx)
      return false
   }
   def crtc_info = XRRGetCrtcInfo(display, resources, crtc)
   if(!crtc_info){
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
   while(mi < mode_count){
      def mp = modes_ptr + mi * 80
      def mw = load32(mp, 8)
      def mh = load32(mp, 12)
      def mr = _refresh_from_mode_info(mp)
      if(mw == width && mh == height){
         def score = abs(mr - refresh_rate)
         if(best_mode == 0 || score < best_score){
         best_mode = load64_h(mp, 0)
         best_score = score
         }
      }
      mi += 1
   }
   def ok = best_mode != 0
   if(ok){
      def ts = malloc(8)
      if(ts){ store64_h(ts, 0, 0) }
      XRRSetCrtcConfig(display, resources, crtc, ts ? load64_h(ts, 0) : 0,
         cx, cy, best_mode, rotation, outputs_ptr, noutputs)
      if(ts){ free(ts) }
      XRRFreeCrtcInfo(crtc_info)
      flush(display)
   } else {
      XRRFreeCrtcInfo(crtc_info)
   }
   XRRFreeScreenResources(resources)
   _release_monitor_context(ctx)
   ok
}

fn restore_video_mode(monitor, display=0, root=0){
   "Restores the original video mode for a monitor using XRandR."
   if(!available() || !monitor || !is_dict(monitor)){ return false }
   def orig_mode = dict_get(monitor, "mode_id", 0)
   if(!orig_mode){ return false }
   def w = dict_get(monitor, "width", 0)
   def h = dict_get(monitor, "height", 0)
   def refresh = dict_get(monitor, "refresh_rate", 0)
   set_video_mode(monitor, w, h, refresh, display, root)
}

fn get_x11_monitor(mon){ dict_get(mon, "handle", 0) }
fn get_x11_adapter(mon){ if(is_dict(mon)){ dict_get(mon, "crtc", 0) } else { 0 } }
