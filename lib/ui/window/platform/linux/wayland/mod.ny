;; Keywords: ui window wayland
;; Low-level Wayland entry helpers for the in-progress native backend.

module std.ui.window.platform.linux.wayland (
   available, get_backend_name,
   connect_display, disconnect_display,
   get_registry, destroy_registry,
   flush, roundtrip, get_fd, dispatch_pending, dispatch,
   prepare_read, cancel_read, read_events,
   wait_events, wait_events_queue,
   create_event_queue, destroy_event_queue,
   prepare_read_queue, dispatch_queue_pending,
   create_proxy_wrapper, destroy_proxy_wrapper, set_proxy_queue,
   get_proxy_user_data, set_proxy_user_data, get_proxy_version, destroy_proxy,
   bootstrap_globals, destroy_globals, set_globals,
   vulkan_supported, vulkan_required_extensions,
   probe_display, presentation_supported, create_surface,
   create_wl_surface, create_xdg_surface, create_xdg_toplevel, xdg_toplevel_set_title,
   create_basic_window, destroy_basic_window, poll_window_events,
   show_window, hide_window, iconify_window, restore_window, maximize_window,
   get_window_attrib, set_window_opacity, set_window_resizable,
   set_title, get_size, set_size, get_cursor_pos, set_cursor_pos, set_window_icon,
   create_cursor, create_standard_cursor, destroy_cursor, set_cursor,
   get_key_state, get_mouse_button_state, get_key_name,
   get_window_monitor, set_window_monitor,
   set_clipboard, get_clipboard,
   get_gamma_ramp, set_gamma_ramp,
   get_monitors, get_primary_monitor,
   get_monitor_pos, get_monitor_workarea,
   get_monitor_physical_size, get_monitor_content_scale,
   get_monitor_name, get_wayland_monitor, get_video_mode, get_video_modes,
   set_input_mode, get_input_mode,
   get_key_name, get_key_scancode,
   text_input_enable, text_input_disable,
   get_window_content_scale, get_window_opacity, get_window_frame_size,
   request_window_attention,
   get_seat_capabilities,
   poll_joystick_events
)

use std.core *
use std.str as str
use std.ui.window.consts *
use std.ui.window.event as ui_event
use std.ui.window.platform.api *
use std.ui.window.platform.linux.x11.keymap as x11_keymap
use std.ui.window.platform.linux.joystick as linux_joystick
use std.util.common as common

;; Wayland FFI includes - only loaded when Wayland is actually available/used
;; We defer these to avoid loading libraries when running on X11
fn _wayland_ffi_init(){
   if(comptime{ __os_name() == "linux" }){
      #include <dlfcn.h>
      #link "libwayland-client.so"
      #include <wayland-client.h>
      #link "libwayland-cursor.so"
      #include <wayland-cursor.h>
      #link "libxkbcommon.so"
      #include <xkbcommon/xkbcommon.h>
      #link "libvulkan.so"
      #include <vulkan/vulkan_wayland.h>
   }
}

mut _debug = -1
fn _is_debug(){ if(_debug == -1){ _debug = common.cached_env_truthy(_debug, "NY_UI_DEBUG") } _debug }
fn _dbg(msg){ if(_is_debug()){ print("[wayland] " + msg) } }
fn _dbgu(msg){ if(_is_debug()){ print("[wayland:v] " + msg) } }
fn _dbg_win(win, msg){ if(_is_debug()){ print("[wayland] win=0x" + to_hex(dict_get(win, "handle", 0)) + " " + msg) } }
fn _dbg_err(msg){ if(_is_debug()){ print("[wayland:ERROR] " + msg) } }
fn _dump_window(win){
   if(!_is_debug()){ return }
   def handle = dict_get(win, "handle", 0)
   def display = dict_get(win, "display", 0)
   def surface = dict_get(win, "surface", 0)
   def xdg_surface = dict_get(win, "xdg_surface", 0)
   def xdg_toplevel = dict_get(win, "xdg_toplevel", 0)
   print("=== Wayland Window Dump ===")
   print("  handle: 0x" + to_hex(handle))
   print("  display: 0x" + to_hex(display))
   print("  surface: 0x" + to_hex(surface))
   print("  xdg_surface: 0x" + to_hex(xdg_surface))
   print("  xdg_toplevel: 0x" + to_hex(xdg_toplevel))
   print("  width: " + to_str(dict_get(win, "width", 0)))
   print("  height: " + to_str(dict_get(win, "height", 0)))
   print("  maximized: " + to_str(dict_get(win, "maximized", false)))
   print("  fullscreen: " + to_str(dict_get(win, "fullscreen", false)))
   print("=========================")
}
mut _wayland_globals = 0
fn set_globals(g){ _wayland_globals = g }

def VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR = 1000006000
def POLLIN = 0x0001
def WL_SEAT_CAPABILITY_POINTER = 1
def WL_SEAT_CAPABILITY_KEYBOARD = 2
def WL_SEAT_CAPABILITY_TOUCH = 4
def WL_SEAT_GET_POINTER = 0
def WL_SEAT_GET_KEYBOARD = 1
def WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE = 1
def WL_OUTPUT_MODE_CURRENT = 0x1
def XDG_WM_BASE_PONG = 3
def XDG_WM_BASE_GET_XDG_SURFACE = 2
def XDG_SURFACE_GET_TOPLEVEL = 1
def XDG_SURFACE_ACK_CONFIGURE = 4
def XDG_TOPLEVEL_SET_TITLE = 1
def XDG_TOPLEVEL_SET_APP_ID = 2
def XDG_TOPLEVEL_SET_MIN_SIZE = 8
def XDG_TOPLEVEL_SET_MAX_SIZE = 9

def XDG_TOPLEVEL_STATE_MAXIMIZED = 1
def XDG_TOPLEVEL_STATE_FULLSCREEN = 2
def XDG_TOPLEVEL_STATE_RESIZING = 3
def XDG_TOPLEVEL_STATE_ACTIVATED = 4

def _WG_COMPOSITOR = 0
def _WG_SUBCOMPOSITOR = 8
def _WG_SHM = 16
def _WG_SEAT = 24
def _WG_DATA_DEVICE_MANAGER = 32
def _WG_OUTPUT_COUNT = 40
def _WG_COMPOSITOR_VER = 48
def _WG_SUBCOMPOSITOR_VER = 56
def _WG_SHM_VER = 64
def _WG_SEAT_VER = 72
def _WG_DATA_DEVICE_MANAGER_VER = 80
def _WG_POINTER = 88
def _WG_KEYBOARD = 96
def _WG_DATA_DEVICE = 104
def _WG_SEAT_CAPS = 112
def _WG_SEAT_NAME = 120
def _WG_SEAT_LISTENER = 128
def _WG_OUTPUTS = 136
def _WG_OUTPUT_CAP = 144
def _WG_WM_BASE = 152
def _WG_WM_BASE_VER = 160
def _WG_WM_BASE_LISTENER = 168
def _WG_POINTER_LISTENER = 176
def _WG_KEYBOARD_LISTENER = 184
def _WG_POINTER_FOCUS = 192
def _WG_KEYBOARD_FOCUS = 200
def _WG_MODS_DEPRESSED = 208
def _WG_MODS_LATCHED = 216
def _WG_MODS_LOCKED = 224
def _WG_MODS_GROUP = 232
def _WG_KEYBOARD_MODS = 208
def _WG_POINTER_ENTER_SERIAL = 240
def _WG_POINTER_BUTTON_SERIAL = 248
def _WG_XKB_CONTEXT = 256
def _WG_XKB_KEYMAP = 264
def _WG_XKB_STATE = 272
def _WG_XKB_MOD_SHIFT = 280
def _WG_XKB_MOD_CTRL = 288
def _WG_XKB_MOD_ALT = 296
def _WG_XKB_MOD_SUPER = 304
def _WG_XKB_MOD_CAPS = 312
def _WG_XKB_MOD_NUM = 320
def _WG_RELATIVE_POINTER_MANAGER = 328
def _WG_RELATIVE_POINTER_MANAGER_VER = 336
def _WG_POINTER_CONSTRAINTS = 344
def _WG_POINTER_CONSTRAINTS_VER = 352
def _WG_DATA_DEVICE_LISTENER = 360
def _WG_CLIPBOARD_OFFER = 368
def _WG_REPEAT_RATE = 376
def _WG_REPEAT_DELAY = 384
def _WG_REPEAT_KEY = 392
def _WG_REPEAT_SCANCODE = 400
def _WG_REPEAT_START_NS = 408
def _WG_REPEAT_LAST_NS = 416
def _WG_DECORATION_MANAGER = 424
def _WG_DECORATION_MANAGER_VER = 432
def _WG_TEXT_INPUT_MANAGER = 440
def _WG_VIEWPORTER = 448
def _WG_FRACTIONAL_SCALE_MANAGER = 456
def _WG_DISPLAY = 464
def _WG_DND_OFFER = 472
def _WG_DND_SURFACE = 480
def _WG_SIZE = 488

def _WP_SET_CURSOR = 0
def _WP_RELEASE = 1

mut _windows = dict()
mut _pending_events = []
mut _clipboard_text = ""
mut _clipboard_source = 0
mut _wl_data_source_iface = 0
mut _cursor_theme = 0
mut _cursor_surface = 0

fn _push_event(win, typ, data=0){
   if(!win){ return }
   _pending_events = append(_pending_events, ui_event.make_event(typ, win, dict_get(win, "handle", 0), data))
}

fn _broadcast_event(typ, data=0){
   def keys = dict_keys(_windows)
   mut i = 0
   while(i < len(keys)){
      def win = dict_get(_windows, get(keys, i), 0)
      if(is_dict(win)){ _push_event(win, typ, data) }
      i += 1
   }
}

def _WO_PROXY = 0
def _WO_GLOBAL_NAME = 8
def _WO_LISTENER = 16
def _WO_NAME = 24
def _WO_DESCRIPTION = 32
def _WO_X = 40
def _WO_Y = 48
def _WO_WIDTH_MM = 56
def _WO_HEIGHT_MM = 64
def _WO_SCALE = 72
def _WO_MODE_W = 80
def _WO_MODE_H = 88
def _WO_REFRESH = 96
def _WO_ANNOUNCED = 104
def _WO_SIZE = 112

;; wl_interface layout (x86-64): name@0(8), version@8(4), method_count@12(4), methods@16(8), event_count@24(4), pad@28(4), events@32(8) = 40 bytes
;; wl_message layout: name@0(8), signature@8(8), types@16(8) = 24 bytes
def _WL_IFACE_SZ = 40
def _WL_MSG_SZ = 24

fn _build_msgs(list){
   "Allocates and fills a wl_message array from a Nytrix list of [name, sig, types?]."
   def n = len(list)
   def ms = malloc(_WL_MSG_SZ * n)
   mut i = 0
   while(i < n){
      def m = get(list, i)
      def msg = ms + i * _WL_MSG_SZ
      store64_h(msg, cstr(get(m, 0)), 0) ;; name
      store64_h(msg, cstr(get(m, 1)), 8) ;; signature
      def types_list = get(m, 2, 0)
      if(types_list && is_list(types_list)){
         def ts = malloc(8 * len(types_list))
         mut j = 0
         while(j < len(types_list)){
         store64_h(ts, get(types_list, j, 0), j * 8)
         j += 1
         }
         store64_h(msg, ts, 16) ;; types
      } else {
         store64_h(msg, 0, 16)
      }
      i += 1
   }
   ms
}

fn _create_interface(name, version, methods=0, events=0){
   def iface = malloc(_WL_IFACE_SZ)
   memset(iface, 0, _WL_IFACE_SZ)
   store64_h(iface, cstr(name), 0) ;; name
   store32(iface, int(version), 8) ;; version
   if(methods){
      store32(iface, len(methods), 12) ;; method_count
      store64_h(iface, _build_msgs(methods), 16) ;; methods
   }
   if(events){
      store32(iface, len(events), 24) ;; event_count
      store64_h(iface, _build_msgs(events), 32) ;; events
   }
   iface
}

mut _wp_relative_pointer_manager_interface = 0
mut _wp_relative_pointer_interface = 0
mut _wp_pointer_constraints_interface = 0
mut _wp_locked_pointer_interface = 0
mut _wp_confined_pointer_interface = 0
mut _wp_decoration_manager_interface = 0
mut _wp_toplevel_decoration_interface = 0
mut _wp_text_input_interface = 0
mut _wp_text_input_manager_interface = 0
mut _wp_viewporter_interface = 0
mut _wp_viewport_interface = 0
mut _wp_fractional_scale_interface = 0
mut _wp_fractional_scale_manager_interface = 0

fn _init_unstable_interfaces(){
   if(_wp_relative_pointer_manager_interface){ return }

   _wp_relative_pointer_interface = _create_interface("zwp_relative_pointer_v1", 1,
      [["destroy", "n", 0]],
      [["relative_motion", "uuffff", 0]])

   _wp_relative_pointer_manager_interface = _create_interface("zwp_relative_pointer_manager_v1", 1,
      [["destroy", "n", 0],
       ["get_relative_pointer", "no", [_wp_relative_pointer_interface, _interface_symbol("wl_pointer_interface")]]])

   _wp_locked_pointer_interface = _create_interface("zwp_locked_pointer_v1", 1,
      [["destroy", "n", 0],
       ["set_cursor_position_hint", "ff", 0],
       ["set_region", "?o", [_interface_symbol("wl_region_interface")]]],
      [["locked", "", 0],
       ["unlocked", "", 0]])

   _wp_confined_pointer_interface = _create_interface("zwp_confined_pointer_v1", 1,
      [["destroy", "n", 0],
       ["set_region", "?o", [_interface_symbol("wl_region_interface")]]],
      [["confined", "", 0],
       ["unconfined", "", 0]])

   _wp_pointer_constraints_interface = _create_interface("zwp_pointer_constraints_v1", 1,
      [["destroy", "n", 0],
       ["lock_pointer", "noo?ou", [_wp_locked_pointer_interface, _interface_symbol("wl_surface_interface"), _interface_symbol("wl_pointer_interface"), _interface_symbol("wl_region_interface"), 0]],
       ["confine_pointer", "noo?ou", [_wp_confined_pointer_interface, _interface_symbol("wl_surface_interface"), _interface_symbol("wl_pointer_interface"), _interface_symbol("wl_region_interface"), 0]]])

   _wp_toplevel_decoration_interface = _create_interface("zxdg_toplevel_decoration_v1", 1,
      [["destroy", "n", 0],
       ["set_mode", "u", 0],
       ["unset_mode", "n", 0]],
      [["configure", "u", 0]])

   _wp_decoration_manager_interface = _create_interface("zxdg_decoration_manager_v1", 1,
      [["destroy", "n", 0],
       ["get_toplevel_decoration", "no", [_wp_toplevel_decoration_interface, _interface_symbol("zxdg_toplevel_v6_interface")]]])

   _wp_text_input_interface = _create_interface("zwp_text_input_v3", 1,
      [["destroy", "n", 0],
       ["enable", "", 0],
       ["disable", "", 0],
       ["set_content_type", "uu", 0],
       ["set_cursor_rectangle", "iiii", 0],
       ["set_surrounding_text", "sii", 0],
       ["commit", "", 0]],
      [["enter", "o", [_interface_symbol("wl_surface_interface")]],
       ["leave", "o", [_interface_symbol("wl_surface_interface")]],
       ["preedit_string", "?si", 0],
       ["commit_string", "?s", 0],
       ["delete_surrounding_text", "uu", 0],
       ["done", "u", 0]])

   _wp_text_input_manager_interface = _create_interface("zwp_text_input_manager_v3", 1,
      [["destroy", "n", 0],
       ["get_text_input", "no", [_wp_text_input_interface, _interface_symbol("wl_seat_interface")]]],
      0)

   _wp_viewport_interface = _create_interface("wp_viewport", 1,
      [["destroy", "n", 0],
       ["set_source", "ffff", 0],
       ["set_destination", "ii", 0]],
      0)

   _wp_viewporter_interface = _create_interface("wp_viewporter", 1,
      [["destroy", "n", 0],
       ["get_viewport", "no", [_wp_viewport_interface, _interface_symbol("wl_surface_interface")]]],
      0)

   _wp_fractional_scale_interface = _create_interface("wp_fractional_scale_v1", 1,
      [["destroy", "n", 0]],
      [["preferred_scale", "u", 0]])

   _wp_fractional_scale_manager_interface = _create_interface("wp_fractional_scale_manager_v1", 1,
      [["destroy", "n", 0],
       ["get_fractional_scale", "no", [_wp_fractional_scale_interface, _interface_symbol("wl_surface_interface")]]],
      0)
}

#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

def XKB_KEYMAP_FORMAT_TEXT_V1 = 1
def XKB_STATE_MODS_DEPRESSED = 1
def XKB_STATE_MODS_LATCHED = 2
def XKB_STATE_MODS_LOCKED = 4
def XKB_STATE_LAYOUT_EFFECTIVE = 1
def PROT_READ = 0x1
def MAP_PRIVATE = 0x02

;; Wayland wrapper functions
fn wl_proxy_marshal_flags_ptr(proxy, opcode, interface, version, flags, arg){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, arg)
}
fn wl_proxy_marshal_flags_ptr_ii(proxy, opcode, interface, version, flags, arg1, arg2){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, arg1, arg2)
}
fn wl_proxy_marshal_flags_ptr_obj(proxy, opcode, interface, version, flags, nullarg, arg){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, nullarg, arg)
}
fn wl_proxy_marshal_flags_cursor(proxy, opcode, interface, version, flags, serial, surface, x, y){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, serial, surface, x, y)
}
fn wl_proxy_marshal_flags_rel_ptr(proxy, opcode, interface, version, flags, id_null, pointer){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, id_null, pointer)
}
fn wl_proxy_marshal_flags_lock_ptr(proxy, opcode, interface, version, flags, id_null, surface, pointer, region, lifetime){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, id_null, surface, pointer, region, lifetime)
}
fn wl_proxy_marshal_flags_obj_ii(proxy, opcode, interface, version, flags, obj, x, y){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, obj, x, y)
}
fn wl_proxy_marshal_flags_str(proxy, opcode, interface, version, flags, s){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, s)
}
fn wl_proxy_marshal_flags_obj_u(proxy, opcode, interface, version, flags, src, serial){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, src, serial)
}
fn wl_proxy_marshal_flags_s_fd(proxy, opcode, interface, version, flags, s, fd){
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, s, fd)
}

fn available(){
   "Returns true when the process appears to be running under Wayland."
   if(!comptime{ __os_name() == "linux" }){ return false }
   def wd = env("WAYLAND_DISPLAY")
   if(wd && is_str(wd) && str.len(wd) > 0){
      _dbg("available: yes (WAYLAND_DISPLAY=" + wd + ")")
      _wayland_ffi_init()
      return true
   }
   def xdg = env("XDG_SESSION_TYPE")
   def session = xdg && is_str(xdg) ? str.lower(xdg) : ""
   def ok = session == "wayland"
   _dbg("available: " + to_str(ok) + " (XDG_SESSION_TYPE=" + session + ")")
   if(ok){ _wayland_ffi_init() }
   ok
}

fn get_backend_name(){
   "Identifies this backend entry module."
   "wayland"
}

fn connect_display(name=0){
   "Connects to a Wayland display."
   if(!available()){ _dbg("connect_display: not available") return 0 }
   _wayland_ffi_init()
   mut dpy = 0
   if(name && is_str(name) && str.len(name) > 0){
      dpy = wl_display_connect(cstr(name))
   } else {
      dpy = wl_display_connect(0)
   }
   _dbg("connect_display: dpy=" + to_hex(dpy))
   dpy
}

fn disconnect_display(display){
   "Disconnects from a Wayland display."
   _dbg("disconnect_display: dpy=" + to_hex(display))
   if(display){ wl_display_disconnect(display) }
   true
}

fn get_registry(display){
   "Returns the Wayland registry for a connected display."
   if(!display){ return 0 }
   wl_display_get_registry(display)
}

fn destroy_registry(registry){
   "Destroys a Wayland registry object."
   if(!registry){ return false }
   wl_registry_destroy(registry)
   true
}

fn flush(display){
   "Flushes pending client requests to the compositor."
   if(!display){ return -1 }
   wl_display_flush(display)
}

fn roundtrip(display){
   "Performs a blocking Wayland roundtrip."
   if(!display){ return -1 }
   wl_display_roundtrip(display)
}
fn wl_pointer_set_cursor(pointer, serial, surface, hotspot_x, hotspot_y){
   if(!pointer){ return }
   wl_proxy_marshal_flags_ptr_obj(pointer, _WP_SET_CURSOR, 0, wl_proxy_get_version(pointer), 0, surface, 0)
   ;; Note: marshal_flags_ptr_obj is usually used for objects, but set_cursor takes (serial, surface, x, y).
   ;; We might need a specific variant for (u, o, i, i).
}

fn get_fd(display){
   "Returns the Wayland display file descriptor."
   if(!display){ return -1 }
   wl_display_get_fd(display)
}

fn dispatch_pending(display){
   "Dispatches already queued Wayland events."
   if(!display){ return -1 }
   wl_display_dispatch_pending(display)
}

fn dispatch(display){
   "Dispatches the next available Wayland event."
   if(!display){ return -1 }
   wl_display_dispatch(display)
}

fn prepare_read(display){
   "Begins a blocking read cycle as used by GLFW Wayland event loop."
   if(!display){ return -1 }
   wl_display_prepare_read(display)
}

fn cancel_read(display){
   "Cancels a previously prepared blocking read."
   if(!display){ return false }
   wl_display_cancel_read(display)
   true
}

fn read_events(display){
   "Reads pending Wayland events after a successful prepare-read cycle."
   if(!display){ return -1 }
   wl_display_read_events(display)
}

fn create_event_queue(display){
   "Creates a dedicated Wayland event queue."
   if(!display){ return 0 }
   wl_display_create_queue(display)
}

fn destroy_event_queue(queue){
   "Destroys a previously created Wayland event queue."
   if(!queue){ return false }
   wl_event_queue_destroy(queue)
   true
}

fn prepare_read_queue(display, queue){
   "Begins a blocking read cycle for a specific Wayland event queue."
   if(!display || !queue){ return -1 }
   wl_display_prepare_read_queue(display, queue)
}

fn dispatch_queue_pending(display, queue){
   "Dispatches already queued events for a specific Wayland queue."
   if(!display || !queue){ return -1 }
   wl_display_dispatch_queue_pending(display, queue)
}

fn create_proxy_wrapper(proxy){
   "Creates a queue-local wrapper for a Wayland proxy."
   if(!proxy){ return 0 }
   wl_proxy_create_wrapper(proxy)
}

fn destroy_proxy_wrapper(proxy_wrapper){
   "Destroys a Wayland proxy wrapper."
   if(!proxy_wrapper){ return false }
   wl_proxy_wrapper_destroy(proxy_wrapper)
   true
}

fn set_proxy_queue(proxy, queue){
   "Assigns a Wayland proxy to an explicit event queue."
   if(!proxy || !queue){ return false }
   wl_proxy_set_queue(proxy, queue)
   true
}

fn get_proxy_user_data(proxy){
   "Returns the Wayland proxy user-data pointer."
   if(!proxy){ return 0 }
   wl_proxy_get_user_data(proxy)
}

fn set_proxy_user_data(proxy, user_data){
   "Sets the Wayland proxy user-data pointer."
   if(!proxy){ return false }
   wl_proxy_set_user_data(proxy, user_data)
   true
}

fn get_proxy_version(proxy){
   "Returns the protocol version advertised by a Wayland proxy."
   if(!proxy){ return 0 }
   int(wl_proxy_get_version(proxy))
}

fn destroy_proxy(proxy){
   "Destroys a generic Wayland proxy."
   if(!proxy){ return false }
   wl_proxy_destroy(proxy)
   true
}

fn _bind_version(advertised, max_supported){
   if(advertised < 1){ return 1 }
   advertised < max_supported ? advertised : max_supported
}

fn _interface_symbol(name){
   if(!name || !is_str(name) || str.len(name) == 0){ return 0 }
   dlsym(0, name)
}

fn _create_seat_pointer(seat){
   if(!seat){ return 0 }
   def iface = _interface_symbol("wl_pointer_interface")
   if(!iface){ return 0 }
   wl_proxy_marshal_flags_ptr(seat, WL_SEAT_GET_POINTER, iface, int(get_proxy_version(seat)), 0, 0)
}

fn _create_seat_keyboard(seat){
   if(!seat){ return 0 }
   def iface = _interface_symbol("wl_keyboard_interface")
   if(!iface){ return 0 }
   wl_proxy_marshal_flags_ptr(seat, WL_SEAT_GET_KEYBOARD, iface, int(get_proxy_version(seat)), 0, 0)
}

fn _create_data_device(manager, seat){
   if(!manager || !seat){ return 0 }
   def iface = _interface_symbol("wl_data_device_interface")
   if(!iface){ return 0 }
   wl_proxy_marshal_flags_ptr_obj(manager, WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE, iface, int(get_proxy_version(manager)), 0, 0, seat)
}

fn create_wl_surface(compositor){
   "Creates a raw `wl_surface` from `wl_compositor`."
   if(!compositor){ return 0 }
   wl_compositor_create_surface(compositor)
}

fn create_xdg_surface(wm_base, surface){
   "Creates an `xdg_surface` wrapper around a `wl_surface`."
   if(!wm_base || !surface){ return 0 }
   def iface = _interface_symbol("xdg_surface_interface")
   if(!iface){ return 0 }
   wl_proxy_marshal_flags_ptr_obj(wm_base, XDG_WM_BASE_GET_XDG_SURFACE, iface, int(get_proxy_version(wm_base)), 0, 0, surface)
}

fn create_xdg_toplevel(xdg_surface){
   "Creates an `xdg_toplevel` object from an `xdg_surface`."
   if(!xdg_surface){ return 0 }
   def iface = _interface_symbol("xdg_toplevel_interface")
   if(!iface){ return 0 }
   wl_proxy_marshal_flags_ptr(xdg_surface, XDG_SURFACE_GET_TOPLEVEL, iface, int(get_proxy_version(xdg_surface)), 0, 0)
}

fn xdg_toplevel_set_title(toplevel, title){
   "Sets the title of an `xdg_toplevel` window."
   if(!toplevel){ return }
   def s = cstr(title)
   wl_proxy_marshal_flags(toplevel, XDG_TOPLEVEL_SET_TITLE, 0, wl_proxy_get_version(toplevel), 0, s)
}

fn xdg_toplevel_set_app_id(toplevel, app_id){
   if(!toplevel){ return }
   def s = cstr(app_id)
   wl_proxy_marshal_flags(toplevel, XDG_TOPLEVEL_SET_APP_ID, 0, wl_proxy_get_version(toplevel), 0, s)
}

fn _surface_handle_enter(data, surface, output){
   "Tracks which outputs a window is currently on."
   mut win = dict_get(_windows, data, 0)
   if(!win || !is_dict(win)){ return }
   def outputs = dict_get(win, "outputs", [])
   win = dict_set(win, "outputs", append(outputs, output))
   _windows = dict_set(_windows, data, win)
}

fn _surface_handle_leave(data, surface, output){
   "Removes an output from the window output list."
   mut win = dict_get(_windows, data, 0)
   if(!win || !is_dict(win)){ return }
   def outputs = dict_get(win, "outputs", [])
   mut next_outputs = []
   mut i = 0
   while(i < len(outputs)){
      def o = get(outputs, i)
      if(o != output){ next_outputs = append(next_outputs, o) }
      i += 1
   }
   win = dict_set(win, "outputs", next_outputs)
   _windows = dict_set(_windows, data, win)
}

fn _xdg_surface_handle_configure(surface, xdg_surface, serial){
   "Acknowledge the configuration serial to Wayland."
   if(!xdg_surface){ return }
   wl_proxy_marshal_flags_ptr(xdg_surface, XDG_SURFACE_ACK_CONFIGURE, 0, int(get_proxy_version(xdg_surface)), 0, int(serial))
   def win = dict_get(_windows, surface, 0)
   if(win){
      def pw = dict_get(win, "pending_w", 0)
      def ph = dict_get(win, "pending_h", 0)
      if(pw > 0 && ph > 0 && (pw != dict_get(win, "w", 0) || ph != dict_get(win, "h", 0))){
         mut next_win = dict_set(win, "w", pw)
         next_win = dict_set(next_win, "h", ph)
         _windows = dict_set(_windows, surface, next_win)
         mut data = dict()
         data = dict_set(data, "w", pw)
         data = dict_set(data, "h", ph)
         _push_event(next_win, EVENT_WINDOW_RESIZED, data)
      }
   }
}

fn _xdg_toplevel_handle_configure(surface, toplevel, width, height, states){
   "Handles window resize suggestions from the compositor."
   mut win = dict_get(_windows, surface, 0)
   if(!win || !is_dict(win)){ return }

   mut next_maximized = false
   mut next_fullscreen = false
   mut next_activated = false

   if(states){
      def size = load64_h(states, 0)
      def data = load64_h(states, 16)
      if(data && size > 0){
         mut i = 0
         while(i < size){
         def state = load32(data, i)
         if(state == XDG_TOPLEVEL_STATE_MAXIMIZED){ next_maximized = true }
         elif(state == XDG_TOPLEVEL_STATE_FULLSCREEN){ next_fullscreen = true }
         elif(state == XDG_TOPLEVEL_STATE_ACTIVATED){ next_activated = true }
         i += 4
         }
      }
   }

   def was_maximized = dict_get(win, "maximized", false)
   if(next_maximized != was_maximized){
      win = dict_set(win, "maximized", next_maximized)
      _push_event(win, next_maximized ? EVENT_WINDOW_MAXIMIZED : EVENT_WINDOW_RESTORED, 0)
   }

   def was_fullscreen = dict_get(win, "fullscreen", false)
   if(next_fullscreen != was_fullscreen){
      win = dict_set(win, "fullscreen", next_fullscreen)
   }

   def was_focused = dict_get(win, "focused", false)
   if(next_activated != was_focused){
      win = dict_set(win, "focused", next_activated)
      _push_event(win, next_activated ? EVENT_FOCUS_IN : EVENT_FOCUS_OUT, 0)
   }

   if(width > 0 && height > 0){
      win = dict_set(win, "pending_w", int(width))
      win = dict_set(win, "pending_h", int(height))
   }
   _windows = dict_set(_windows, surface, win)
}

fn _xdg_toplevel_handle_close(win_obj, toplevel){
   "Sets the should_close bit for a Wayland window."
   def win = dict_get(_windows, win_obj, 0)
   if(win){
      _push_event(win, EVENT_QUIT, 0)
   }
}

fn _decoration_handle_configure(win_obj, decoration, mode){
   "zxdg_toplevel_decoration_v1 configure callback — compositor chose CSD or SSD."
   ;; mode=1: CSD (client-side), mode=2: SSD (server-side)
   ;; No action needed; we accept whatever the compositor decided.
}

fn _create_shell_objects(win){
   "Creates XDG shell objects for the Wayland surface."
   if(!win || !is_dict(win)){ return win }
   def handle = dict_get(win, "handle", 0)
   def globals = dict_get(win, "globals", 0)
   if(!handle || !globals){ return win }
   def wm_base = dict_get(globals, "wm_base", 0)
   if(!wm_base){ return win }

   def xdg_surface = create_xdg_surface(wm_base, handle)
   if(!xdg_surface){ return win }
   def toplevel = create_xdg_toplevel(xdg_surface)
   if(!toplevel){
      destroy_proxy(xdg_surface)
      return win
   }

   ;; Register listeners
   def xdg_surface_listener = calloc(1, 8)
   store64_h(xdg_surface_listener, _xdg_surface_handle_configure, 0)
   wl_proxy_add_listener(xdg_surface, xdg_surface_listener, handle)

   def toplevel_listener = calloc(2, 8)
   store64_h(toplevel_listener, _xdg_toplevel_handle_configure, 0)
   store64_h(toplevel_listener, _xdg_toplevel_handle_close, 8)
   wl_proxy_add_listener(toplevel, toplevel_listener, handle)

   xdg_toplevel_set_title(toplevel, dict_get(win, "title", "Untitled"))
   xdg_toplevel_set_app_id(toplevel, dict_get(win, "app_id", "nytrix"))

   ;; Request server-side decorations if zxdg_decoration_manager_v1 is available
   def state = dict_get(globals, "listener_state", 0)
   def dec_mgr = state ? load64_h(state, _WG_DECORATION_MANAGER) : 0
   if(dec_mgr){
      _init_unstable_interfaces()
      def dec_iface = _wp_toplevel_decoration_interface
      if(dec_iface){
         def decoration = wl_proxy_marshal_flags_ptr(dec_mgr, 1, dec_iface, int(get_proxy_version(dec_mgr)), 0, toplevel)
         if(decoration){
         def dec_listener = calloc(1, 8)
         if(dec_listener){
               store64_h(dec_listener, _decoration_handle_configure, 0)
               wl_proxy_add_listener(decoration, dec_listener, handle)
         }
         ;; set_mode opcode=1, SSD=2 (mode arg is u32)
         wl_proxy_marshal_flags_ptr_ii(decoration, 1, 0, int(get_proxy_version(decoration)), 0, 2, 0)
         }
      }
   }

   mut next_win = dict_set(win, "xdg_surface", xdg_surface)
   next_win = dict_set(next_win, "xdg_toplevel", toplevel)
   next_win
}

fn _destroy_shell_objects(win){
   "Destroys XDG shell objects for the Wayland surface."
   if(!win || !is_dict(win)){ return win }
   def xdg_surface = dict_get(win, "xdg_surface", 0)
   def toplevel = dict_get(win, "xdg_toplevel", 0)
   if(toplevel){ destroy_proxy(toplevel) }
   if(xdg_surface){ destroy_proxy(xdg_surface) }
   mut next_win = dict_set(win, "xdg_surface", 0)
   next_win = dict_set(next_win, "xdg_toplevel", 0)
   next_win
}

fn create_basic_window(globals, title, width, height, app_id="nytrix"){
   "High-level helper to create a Wayland window with all necessary surface/shell wrappers."
   _dbg("create_basic_window: title=" + title + " size=" + to_str(width) + "x" + to_str(height))
   if(!globals || !is_dict(globals)){ _dbg_err("no globals") return 0 }
   def comp = dict_get(globals, "compositor", 0)
   if(!comp){ _dbg_err("no compositor") return 0 }
   _dbg("  compositor=0x" + to_hex(comp))

   def surface = create_wl_surface(comp)
   if(!surface){ _dbg_err("failed to create surface") return 0 }
   _dbg("  surface=0x" + to_hex(surface))

   ;; Register wl_surface listener
   def wl_surface_listener = calloc(2, 8)
   store64_h(wl_surface_listener, _surface_handle_enter, 0)
   store64_h(wl_surface_listener, _surface_handle_leave, 8)
   wl_proxy_add_listener(surface, wl_surface_listener, surface)

   mut win = dict()
   win = dict_set(win, "handle", surface)
   win = dict_set(win, "globals", globals)
   win = dict_set(win, "w", width)
   win = dict_set(win, "h", height)
   win = dict_set(win, "title", title)
   win = dict_set(win, "app_id", app_id)
   win = dict_set(win, "visible", false)

   ;; Standard start state: shown by default if visibility not explicitly disabled
   win = _create_shell_objects(win)
   win = dict_set(win, "visible", true)

   ;; Wire zwp_text_input_v3 if manager is available
   def state_gi = dict_get(globals, "listener_state", 0)
   if(state_gi){
      def seat = load64_h(state_gi, _WG_SEAT)
      def ti = _create_text_input(state_gi, seat)
      if(ti){
         def ti_listener = calloc(6, 8)
         store64_h(ti_listener, _text_input_handle_enter, 0)
         store64_h(ti_listener, _text_input_handle_leave, 8)
         store64_h(ti_listener, _text_input_handle_preedit, 16)
         store64_h(ti_listener, _text_input_handle_commit, 24)
         store64_h(ti_listener, _text_input_handle_delete_surrounding, 32)
         store64_h(ti_listener, _text_input_handle_done, 40)
         wl_proxy_add_listener(ti, ti_listener, surface)
         win = dict_set(win, "text_input", ti)
         win = dict_set(win, "text_input_listener", ti_listener)
      }
      def fs = _create_fractional_scale(state_gi, surface)
      if(fs){
         def fs_listener = calloc(1, 8)
         store64_h(fs_listener, _fractional_scale_handle_preferred_scale, 0)
         wl_proxy_add_listener(fs, fs_listener, surface)
         win = dict_set(win, "fractional_scale", fs)
         win = dict_set(win, "fractional_scale_listener", fs_listener)
      }
   }

   wl_proxy_marshal_flags_ptr(surface, 0, 0, int(get_proxy_version(surface)), 0, 0) ;; wl_surface.commit

   _windows = dict_set(_windows, surface, win)
   win
}

fn destroy_basic_window(win){
   "Destroys a Wayland window and its associated surface proxies."
   if(!win || !is_dict(win)){ return false }
   def surface = dict_get(win, "handle", 0)
   def xdg_surface = dict_get(win, "xdg_surface", 0)
   def toplevel = dict_get(win, "xdg_toplevel", 0)

   ;; Clean up unstable protocol proxies
   def locked_ptr = dict_get(win, "locked_pointer", 0)
   if(locked_ptr){
      wl_proxy_marshal_flags_ptr(locked_ptr, 0, 0, 1, 1, 0) ;; destroy
      destroy_proxy(locked_ptr)
      def ll = dict_get(win, "lock_listener", 0)
      if(ll){ free(ll) }
      def ld = dict_get(win, "lock_data", 0)
      if(ld){ free(ld) }
   }
   def relative_ptr = dict_get(win, "relative_pointer", 0)
   if(relative_ptr){
      wl_proxy_marshal_flags_ptr(relative_ptr, 0, 0, 1, 1, 0) ;; destroy
      destroy_proxy(relative_ptr)
      def rl = dict_get(win, "rel_listener", 0)
      if(rl){ free(rl) }
      def rd = dict_get(win, "rel_data", 0)
      if(rd){ free(rd) }
   }

   if(toplevel){ destroy_proxy(toplevel) }
   if(xdg_surface){ destroy_proxy(xdg_surface) }
   if(surface){ destroy_proxy(surface) }
   _windows = dict_set(_windows, surface, 0)
   true
}

fn get_size(win){
   "Returns the Wayland window size as [width, height]."
   if(!win || !is_dict(win)){ return [0, 0] }
   [dict_get(win, "w", 0), dict_get(win, "h", 0)]
}

fn set_size(win, w, h){
   "Sets the Wayland window size (as a hint to the compositor)."
   if(!win || !is_dict(win)){ return false }
   ;; Wayland does not support setting size directly from client side
   ;; but we track it in our state.
   true
}

fn get_cursor_pos(win){
   "Returns the current cursor position relative to the Wayland window."
   if(!win || !is_dict(win)){ return [0.0, 0.0] }
   [float(dict_get(win, "mouse_x", 0)), float(dict_get(win, "mouse_y", 0))]
}

fn get_key_name(win, key, scancode){
   "Returns the keyboard-layout specific name for a key using XKB if available."
   if(!win || !is_dict(win)){ return "" }
   def globals = dict_get(win, "globals", 0)
   if(globals){
      def state = dict_get(globals, "listener_state", 0)
      if(state){
         def xkb_state = load64_h(state, _WG_XKB_STATE)
         if(xkb_state){
         def sym = xkb_state_key_get_one_sym(xkb_state, int(scancode) + 8)
         if(sym){
               ;; Try printable codepoint first (GLFW-style: return single char)
               def cp = int(xkb_keysym_to_utf32(sym))
               if(cp > 32 && cp != 127){ return str.chr(cp) }
               ;; Fall back to keysym name
               def buf = malloc(64)
               if(buf){
                  def res = xkb_keysym_get_name(sym, buf, 64)
                  mut out = ""
                  if(res > 0){ out = str.cstr_to_str(buf) }
                  free(buf)
                  return out
               }
         }
         }
      }
   }
   if(key >= 32 && key <= 126){ return str.chr(key) }
   ""
}

fn set_window_icon(win, images){
   "Wayland does not support setting window icons directly."
   false
}

fn get_key_state(win, key){
   "Returns the Wayland key state from the local dictionary."
   if(!win || !is_dict(win)){ return 0 }
   dict_get(dict_get(win, "key_states", 0), key, false) ? 1 : 0
}

fn get_mouse_button_state(win, btn){
   "Returns the Wayland mouse button state from the local dictionary."
   if(!win || !is_dict(win)){ return 0 }
   dict_get(win, "mouse_button_" + to_str(btn), 0)
}

fn set_cursor_pos(win, x, y){
   "Wayland cursor warping: only available via pointer lock hint when CURSOR_DISABLED."
   if(!win || !is_dict(win)){ return false }
   def locked_ptr = dict_get(win, "locked_pointer", 0)
   if(!locked_ptr){ return false }
   ;; set_cursor_position_hint opcode=1, signature "ff"
   wl_proxy_marshal_flags_ptr_ii(locked_ptr, 1, 0, int(get_proxy_version(locked_ptr)), 0,
      int(x * 256), int(y * 256))
   true
}

fn _ensure_cursor_theme(globals){
   if(_cursor_theme){ return _cursor_theme }
   if(!globals){ return 0 }
   def shm = dict_get(globals, "shm", 0)
   if(!shm){ return 0 }
   _cursor_theme = wl_cursor_theme_load(0, 24, shm)
   _cursor_theme
}

fn _ensure_cursor_surface(globals){
   if(_cursor_surface){ return _cursor_surface }
   if(!globals){ return 0 }
   def comp = dict_get(globals, "compositor", 0)
   if(!comp){ return 0 }
   _cursor_surface = wl_compositor_create_surface(comp)
   _cursor_surface
}

mut _cursor_shape_dict = 0

fn _cursor_shape_names_dict(){
   if(_cursor_shape_dict){ return _cursor_shape_dict }
   mut d = dict()
   d = dict_set(d, backend_api.ARROW_CURSOR,        ["default", "left_ptr", "arrow"])
   d = dict_set(d, backend_api.IBEAM_CURSOR,        ["text", "xterm", "ibeam"])
   d = dict_set(d, backend_api.CROSSHAIR_CURSOR,    ["crosshair", "cross"])
   d = dict_set(d, backend_api.POINTING_HAND_CURSOR,["pointer", "hand2", "pointing_hand"])
   d = dict_set(d, backend_api.RESIZE_EW_CURSOR,    ["ew-resize", "sb_h_double_arrow", "size_hor"])
   d = dict_set(d, backend_api.RESIZE_NS_CURSOR,    ["ns-resize", "sb_v_double_arrow", "size_ver"])
   d = dict_set(d, backend_api.RESIZE_NWSE_CURSOR,  ["nwse-resize", "top_left_corner", "size_fdiag"])
   d = dict_set(d, backend_api.RESIZE_NESW_CURSOR,  ["nesw-resize", "top_right_corner", "size_bdiag"])
   d = dict_set(d, backend_api.RESIZE_ALL_CURSOR,   ["all-scroll", "fleur", "size_all"])
   d = dict_set(d, backend_api.NOT_ALLOWED_CURSOR,  ["not-allowed", "crossed_circle", "forbidden"])
   _cursor_shape_dict = d
   d
}

fn _wl_cursor_for_shape(theme, shape){
   if(!theme){ return 0 }
   def d = _cursor_shape_names_dict()
   mut names = dict_get(d, shape, 0)
   if(!names){ names = dict_get(d, backend_api.ARROW_CURSOR, 0) }
   if(!names){ return 0 }
   mut i = 0
   while(i < len(names)){
      def cur = wl_cursor_theme_get_cursor(theme, cstr(get(names, i)))
      if(cur){ return cur }
      i += 1
   }
   0
}

fn create_cursor(image, xhot=0, yhot=0){
   "Custom cursor from image data: stores as dict for deferred upload."
   if(!image){ return 0 }
   mut c = dict()
   c = dict_set(c, "type", "custom")
   c = dict_set(c, "image", image)
   c = dict_set(c, "xhot", xhot)
   c = dict_set(c, "yhot", yhot)
   c
}

fn create_standard_cursor(shape){
   "Creates a Wayland standard cursor by shape index."
   mut c = dict()
   c = dict_set(c, "type", "standard")
   c = dict_set(c, "shape", shape)
   c
}

fn destroy_cursor(cursor){
   "Destroys a cursor dict (theme-owned cursors are freed with the theme)."
   true
}

fn set_cursor(win, cursor){
   "Sets the cursor for a Wayland window using the cursor theme."
   if(!win || !is_dict(win)){ return win }
   def globals = dict_get(win, "globals", 0)
   if(!globals){ return win }
   def state = dict_get(globals, "listener_state", 0)
   if(!state){ return win }
   def pointer = load64_h(state, _WG_POINTER)
   if(!pointer){ return win }
   def serial = load64_h(state, _WG_POINTER_ENTER_SERIAL)

   if(!cursor){
      ;; Hide cursor
      wl_proxy_marshal_flags_full_set_cursor(pointer, serial, 0, 0, 0)
      return win
   }

   def theme = _ensure_cursor_theme(globals)
   def surf  = _ensure_cursor_surface(globals)
   if(!theme || !surf){ return win }

   def cur_shape = dict_get(cursor, "shape", 0)
   def wl_cur = _wl_cursor_for_shape(theme, cur_shape)
   if(!wl_cur){ return win }

   ;; wl_cursor layout: [image_count: u32, pad: u32, images: ptr, name: ptr]
   def image_count = load32(wl_cur, 0)
   if(image_count == 0){ return win }
   def images_ptr = load64_h(wl_cur, 8) ;; ptr to array of wl_cursor_image*
   def img = load64_h(images_ptr, 0) ;; first image
   if(!img){ return win }

   ;; wl_cursor_image layout: [width: u32, height: u32, hotspot_x: u32, hotspot_y: u32, delay: u32]
   def hx = load32(img, 8)
   def hy = load32(img, 12)

   ;; Get buffer from image (wl_buffer is at offset 20 in wl_cursor_image)
   ;; Actually use wl_cursor_image_get_buffer via a wrapper
   def buf = _wl_cursor_image_get_buffer(img)
   if(!buf){ return win }

   ;; wl_surface.attach(buffer, 0, 0) opcode=1, then commit opcode=6
   wl_proxy_marshal_flags_obj_ii(surf, 1, 0, int(get_proxy_version(surf)), 0, buf, 0, 0)
   wl_proxy_marshal_flags_ptr(surf, 6, 0, int(get_proxy_version(surf)), 0, 0)
   wl_proxy_marshal_flags_full_set_cursor(pointer, serial, surf, hx, hy)
   win
}

fn _ensure_data_source_iface(){
   if(!_wl_data_source_iface){
      _wl_data_source_iface = _create_interface("wl_data_source", 3,
         [["offer", "s", 0], ["destroy", "n", 0], ["set_actions", "u", 0]],
         [["target", "?s", 0], ["send", "sh", 0], ["cancelled", "", 0]])
   }
   _wl_data_source_iface
}

fn _data_source_send(data, source, mime_type, fd){
   if(!_clipboard_text || !fd){ close(int(fd)) return }
   def n = str.str_len(_clipboard_text)
   if(n > 0){ write(int(fd), cstr(_clipboard_text), int(n)) }
   close(int(fd))
}

fn _data_source_cancelled(data, source){
   if(source && source == _clipboard_source){
      destroy_proxy(source)
      _clipboard_source = 0
      _clipboard_text = ""
   }
}

fn _data_device_data_offer(data, device, offer){
   ;; Track pending offer; will be tied to clipboard in selection event
}

fn _data_device_selection(data, device, offer){
   def prev = load64_h(data, _WG_CLIPBOARD_OFFER)
   if(prev && prev != offer){ destroy_proxy(prev) }
   store64_h(data, offer, _WG_CLIPBOARD_OFFER)
}

fn _data_device_enter(data, device, serial, surface, x, y, offer){
   if(!data){ return }
   store64_h(data, offer, _WG_DND_OFFER)
   store64_h(data, surface, _WG_DND_SURFACE)
   def win = dict_get(_windows, surface, 0)
   if(win && is_dict(win)){
      mut ev = dict()
      ev = dict_set(ev, "x", int(x))
      ev = dict_set(ev, "y", int(y))
      _push_event(win, EVENT_DATA_DROP, ev)
   }
}

fn _data_device_leave(data, device){
   if(!data){ return }
   store64_h(data, 0, _WG_DND_OFFER)
   store64_h(data, 0, _WG_DND_SURFACE)
}

fn _data_device_motion(data, device, time, x, y){
   if(!data){ return }
   def surface = load64_h(data, _WG_DND_SURFACE)
   if(!surface){ return }
   def win = dict_get(_windows, surface, 0)
   if(win && is_dict(win)){
      mut ev = dict()
      ev = dict_set(ev, "x", int(x))
      ev = dict_set(ev, "y", int(y))
      _push_event(win, EVENT_DATA_DROP, ev)
   }
}

fn _data_device_drop(data, device){
   if(!data){ return }
   def offer = load64_h(data, _WG_DND_OFFER)
   def surface = load64_h(data, _WG_DND_SURFACE)
   if(!offer || !surface){ return }
   def display = load64_h(data, _WG_DISPLAY)
   def win = dict_get(_windows, surface, 0)
   if(!win || !is_dict(win)){ return }
   def fds = malloc(8)
   if(!fds){ return }
   memset(fds, 0, 8)
   if(pipe(fds) != 0){ free(fds) return }
   def rfd = int(load32(fds, 0))
   def wfd = int(load32(fds, 4))
   free(fds)
   ;; opcode 1 = receive(mime_type: s, fd: h)
   wl_proxy_marshal_flags_s_fd(offer, 1, 0, int(get_proxy_version(offer)), 0, cstr("text/uri-list"), wfd)
   close(wfd)
   if(display){ flush(display) }
   def buf = malloc(4096)
   if(!buf){ close(rfd) return }
   memset(buf, 0, 4096)
   def n = read(rfd, buf, 4095)
   close(rfd)
   if(n > 0){
      def text = to_str(buf)
      mut ev = dict()
      ev = dict_set(ev, "text", text)
      _push_event(win, EVENT_DATA_DROP, ev)
   }
   free(buf)
   ;; opcode 3 = finish()
   wl_proxy_marshal_flags_ptr(offer, 3, 0, int(get_proxy_version(offer)), 0, 0)
   store64_h(data, 0, _WG_DND_OFFER)
}

fn _install_data_device_listener(state){
   if(!state){ return false }
   def device = load64_h(state, _WG_DATA_DEVICE)
   if(!device){ return false }
   if(load64_h(state, _WG_DATA_DEVICE_LISTENER)){ return true }
   def listener = calloc(6, 8)
   if(!listener){ return false }
   store64_h(listener, _data_device_data_offer, 0)
   store64_h(listener, _data_device_enter,      8)
   store64_h(listener, _data_device_leave,      16)
   store64_h(listener, _data_device_motion,     24)
   store64_h(listener, _data_device_drop,       32)
   store64_h(listener, _data_device_selection,  40)
   if(wl_proxy_add_listener(device, listener, state) != 0){
      free(listener)
      return false
   }
   store64_h(state, listener, _WG_DATA_DEVICE_LISTENER)
   true
}

fn set_clipboard(win, text){
   "Sets the Wayland clipboard via wl_data_source / wl_data_device."
   if(!win || !is_dict(win)){ return false }
   def globals = dict_get(win, "globals", 0)
   if(!globals){ return false }
   def state = dict_get(globals, "listener_state", 0)
   if(!state){ return false }
   def manager = load64_h(state, _WG_DATA_DEVICE_MANAGER)
   def device  = load64_h(state, _WG_DATA_DEVICE)
   if(!manager || !device){ return false }
   def display = dict_get(globals, "handle", 0)

   ;; Destroy previous owned source
   if(_clipboard_source){
      destroy_proxy(_clipboard_source)
      _clipboard_source = 0
   }
   _clipboard_text = to_str(text)

   def iface = _ensure_data_source_iface()
   def source = wl_proxy_marshal_flags_ptr(manager, 0, iface, int(get_proxy_version(manager)), 0, 0)
   if(!source){ return false }

   ;; Install send/cancelled listener
   def src_listener = calloc(3, 8)
   if(!src_listener){ destroy_proxy(source) return false }
   store64_h(src_listener, 0, 0) ;; target (noop)
   store64_h(src_listener, _data_source_send, 8)
   store64_h(src_listener, _data_source_cancelled, 16)
   wl_proxy_add_listener(source, src_listener, 0)

   ;; Offer MIME types
   wl_proxy_marshal_flags_str(source, 0, 0, int(get_proxy_version(source)), 0, cstr("text/plain;charset=utf-8"))
   wl_proxy_marshal_flags_str(source, 0, 0, int(get_proxy_version(source)), 0, cstr("text/plain"))
   wl_proxy_marshal_flags_str(source, 0, 0, int(get_proxy_version(source)), 0, cstr("UTF8_STRING"))

   ;; Install data_device listener if needed
   _install_data_device_listener(state)

   ;; Set selection with keyboard enter serial
   def btn_serial = load64_h(state, _WG_POINTER_BUTTON_SERIAL)
   wl_proxy_marshal_flags_obj_u(device, 1, 0, int(get_proxy_version(device)), 0, source, int(btn_serial))
   _clipboard_source = source
   if(display){ wl_display_flush(display) }
   true
}

fn get_clipboard(win){
   "Gets the Wayland clipboard via wl_data_offer pipe receive."
   if(_clipboard_source && str.str_len(_clipboard_text) > 0){ return _clipboard_text }
   if(!win || !is_dict(win)){ return "" }
   def globals = dict_get(win, "globals", 0)
   if(!globals){ return "" }
   def state = dict_get(globals, "listener_state", 0)
   if(!state){ return "" }
   def display = dict_get(globals, "handle", 0)

   ;; Install data_device listener if not yet done
   _install_data_device_listener(state)
   if(display){ wl_display_roundtrip(display) }

   def offer = load64_h(state, _WG_CLIPBOARD_OFFER)
   if(!offer){ return "" }

   ;; Create a pipe: pipefd[0]=read end, pipefd[1]=write end
   def pipefd = malloc(8)
   if(!pipefd){ return "" }
   if(pipe(pipefd) != 0){ free(pipefd) return "" }
   def rfd = load32(pipefd, 0)
   def wfd = load32(pipefd, 4)
   free(pipefd)

   ;; Ask compositor to write clipboard into the write end
   wl_proxy_marshal_flags_s_fd(offer, 1, 0, int(get_proxy_version(offer)), 0, cstr("text/plain;charset=utf-8"), int(wfd))
   close(wfd)
   if(display){ wl_display_flush(display) }

   ;; Read from read end
   def buf = malloc(16384)
   if(!buf){ close(rfd) return "" }
   mut total = 0
   mut chunk = read(int(rfd), buf + total, 16384 - total)
   while(chunk > 0 && total < 16383){
      total += chunk
      chunk = read(int(rfd), buf + total, 16384 - total - 1)
   }
   close(rfd)
   store8(buf, 0, total)
   def result = to_str(buf)
   free(buf)
   result
}

fn get_window_monitor(win){
   "Returns the first associated output for a Wayland window."
   if(!is_dict(win)){ return 0 }
   def outputs = dict_get(win, "outputs", [])
   if(len(outputs) == 0){ return 0 }
   def proxy = get(outputs, 0)

   ;; Match proxy back to output state in globals
   def monitors = get_monitors()
   mut i = 0
   while(i < len(monitors)){
      def m = get(monitors, i)
      if(dict_get(m, "handle", 0) == proxy){ return m }
      i += 1
   }
   0
}

fn set_window_monitor(win, monitor, xpos, ypos, width, height, refresh_rate=0){
   "Stub for Wayland window-monitor association."
   win
}

fn show_window(win){
   "Shows the Wayland window by creating shell objects if needed."
   _dbg("show_window: visible=" + to_str(dict_get(win, "visible", false)))
   if(!win || !is_dict(win)){ _dbg("show_window: invalid win") return false }
   if(dict_get(win, "visible", false)){ return true }
   def handle = dict_get(win, "handle", 0)
   if(!handle){ _dbg("show_window: no handle") return false }

   mut next_win = _create_shell_objects(win)
   next_win = dict_set(next_win, "visible", true)
   _windows = dict_set(_windows, handle, next_win)

   wl_proxy_marshal_flags_ptr(handle, 0, 0, int(get_proxy_version(handle)), 0, 0) ;; commit
   true
}

fn hide_window(win){
   "Hides the Wayland window by destroying shell objects and detaching surface buffer."
   if(!win || !is_dict(win)){ return false }
   if(!dict_get(win, "visible", false)){ return true }
   def handle = dict_get(win, "handle", 0)
   if(!handle){ return false }

   mut next_win = _destroy_shell_objects(win)
   next_win = dict_set(next_win, "visible", false)
   _windows = dict_set(_windows, handle, next_win)

   ;; Detach buffer and commit hide
   def WL_SURFACE_ATTACH = 1
   wl_proxy_marshal_flags_ptr_obj(handle, WL_SURFACE_ATTACH, 0, int(get_proxy_version(handle)), 0, 0, 0)
   wl_proxy_marshal_flags_ptr(handle, 0, 0, int(get_proxy_version(handle)), 0, 0) ;; commit
   true
}

fn iconify_window(win){
   "Ny direct port of GLFW `_glfwIconifyWindowWayland`."
   if(!win || !is_dict(win)){ return false }
   def toplevel = dict_get(win, "xdg_toplevel", 0)
   if(toplevel){ xdg_toplevel_set_minimized(toplevel) }
   true
}

fn maximize_window(win){
   "Ny direct port of GLFW `_glfwMaximizeWindowWayland`."
   if(!win || !is_dict(win)){ return false }
   def toplevel = dict_get(win, "xdg_toplevel", 0)
   if(toplevel){ xdg_toplevel_set_maximized(toplevel) }
   true
}

fn restore_window(win){
   "Ny direct port of GLFW `_glfwRestoreWindowWayland`."
   if(!win || !is_dict(win)){ return false }
   def toplevel = dict_get(win, "xdg_toplevel", 0)
   if(toplevel){ xdg_toplevel_unset_maximized(toplevel) }
   true
}

fn set_title(win, title){
   "Updates the Wayland platform window title."
   if(!win || !is_dict(win) || !title){ return false }
   def toplevel = dict_get(win, "xdg_toplevel", 0)
   if(!toplevel){ return false }
   xdg_toplevel_set_title(toplevel, title)
}

fn set_window_opacity(win, opacity){
   "Applies window opacity via Wayland protocols (currently a no-op)."
   false
}

fn set_window_resizable(win, enabled){
   "Toggles the resizable state of a Wayland window by setting min/max sizes."
   if(!win || !is_dict(win)){ return false }
   def toplevel = dict_get(win, "toplevel", 0)
   if(!toplevel){ return false }
   if(!enabled){
      def sz = get_size(win)
      wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MIN_SIZE, 0, int(get_proxy_version(toplevel)), 0, int(get(sz, 0)), int(get(sz, 1)))
      wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MAX_SIZE, 0, int(get_proxy_version(toplevel)), 0, int(get(sz, 0)), int(get(sz, 1)))
   } else {
      wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MIN_SIZE, 0, int(get_proxy_version(toplevel)), 0, 0, 0)
      wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MAX_SIZE, 0, int(get_proxy_version(toplevel)), 0, 0, 0)
   }
   true
}

fn set_window_decorated(win, enabled){
   "Requests server-side or no-decoration mode via zxdg_decoration_manager_v1."
   if(!win || !is_dict(win)){ return false }
   def globals = dict_get(win, "globals", 0)
   def state = globals ? dict_get(globals, "listener_state", 0) : 0
   if(!state){ return false }
   def dec_mgr = load64_h(state, _WG_DECORATION_MANAGER)
   if(!dec_mgr){ return false }
   def toplevel = dict_get(win, "xdg_toplevel", 0)
   if(!toplevel){ return false }
   _init_unstable_interfaces()
   def dec_iface = _wp_toplevel_decoration_interface
   if(!dec_iface){ return false }
   def decoration = wl_proxy_marshal_flags_ptr(dec_mgr, 1, dec_iface, int(get_proxy_version(dec_mgr)), 0, toplevel)
   if(!decoration){ return false }
   ;; set_mode: SSD=2, CSD=1
   wl_proxy_marshal_flags_ptr_ii(decoration, 1, 0, int(get_proxy_version(decoration)), 0, enabled ? 2 : 1, 0)
   true
}

fn set_window_floating(win, enabled){
   "Stub for Wayland floating state (not directly supported by xdg-shell)."
   false
}

fn set_window_size_limits(win, min_w, min_h, max_w, max_h){
   "Sets size limits for a Wayland window via xdg_toplevel."
   if(!win || !is_dict(win)){ return false }
   def toplevel = dict_get(win, "xdg_toplevel", 0)
   if(!toplevel){ return false }

   if(min_w >= 0 && min_h >= 0){
      wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MIN_SIZE, 0,
         int(get_proxy_version(toplevel)), 0, int(min_w), int(min_h))
   } else {
      wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MIN_SIZE, 0,
         int(get_proxy_version(toplevel)), 0, 0, 0)
   }

   if(max_w >= 0 && max_h >= 0){
      wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MAX_SIZE, 0,
         int(get_proxy_version(toplevel)), 0, int(max_w), int(max_h))
   } else {
      wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MAX_SIZE, 0,
         int(get_proxy_version(toplevel)), 0, 0, 0)
   }
   true
}

fn get_window_attrib(win, attrib){
   "Unified getter for Wayland window attributes matching Nytrix constants."
   if(!win || !is_dict(win)){ return 0 }
   match attrib {
      RESIZABLE -> { return dict_get(win, "resizable", true) ? 1 : 0 }
      VISIBLE -> { return 1 }
      FOCUSED -> { return dict_get(win, "focused", false) ? 1 : 0 }
      MAXIMIZED -> { return dict_get(win, "maximized", false) ? 1 : 0 }
      ICONIFIED -> { return 0 }
      FLOATING -> { return 0 }
      _ -> { return 0 }
   }
}

fn _free_cstr_ptr(p){
   if(p){ free(p) }
}

fn _new_output_state(output, global_name){
   def state = calloc(1, _WO_SIZE)
   if(!state){ return 0 }
   store64_h(state, output, _WO_PROXY)
   store64_h(state, global_name, _WO_GLOBAL_NAME)
   store64_h(state, 1, _WO_SCALE)
   state
}

fn _append_output_state(state, out_state){
   if(!state || !out_state){ return false }
   mut cap = load64_h(state, _WG_OUTPUT_CAP)
   mut arr = load64_h(state, _WG_OUTPUTS)
   def count = load64_h(state, _WG_OUTPUT_COUNT)
   if(count >= cap){
      def new_cap = cap > 0 ? cap * 2 : 4
      def new_arr = arr ? realloc(arr, new_cap * 8) : malloc(new_cap * 8)
      if(!new_arr){ return false }
      if(!arr){ memset(new_arr, 0, new_cap * 8) }
      store64_h(state, new_arr, _WG_OUTPUTS)
      store64_h(state, new_cap, _WG_OUTPUT_CAP)
      arr = new_arr
      cap = new_cap
   }
   store64_h(arr, out_state, count * 8)
   store64_h(state, count + 1, _WG_OUTPUT_COUNT)
   true
}

fn _output_count(state){
   state ? load64_h(state, _WG_OUTPUT_COUNT) : 0
}

fn _output_at(state, index){
   if(!state || index < 0 || index >= _output_count(state)){ return 0 }
   def arr = load64_h(state, _WG_OUTPUTS)
   if(!arr){ return 0 }
   load64_h(arr, index * 8)
}

fn _refresh_hz(refresh_mhz){
   refresh_mhz > 0 ? int((refresh_mhz + 500) / 1000) : 0
}

fn _store_output_name_if_empty(out_state, make, model){
   if(!out_state || load64_h(out_state, _WO_NAME)){ return }
   def mk = make ? to_str(make) : ""
   def md = model ? to_str(model) : ""
   def full = str.strip(mk + " " + md)
   if(str.len(full) > 0){
      store64_h(out_state, strdup(cstr(full)), _WO_NAME)
   }
}

fn _output_handle_geometry(data, wl_output, x, y, physical_width, physical_height, subpixel, make, model, transform){
   if(!data){ return }
   store64_h(data, x, _WO_X)
   store64_h(data, y, _WO_Y)
   store64_h(data, physical_width, _WO_WIDTH_MM)
   store64_h(data, physical_height, _WO_HEIGHT_MM)
   _store_output_name_if_empty(data, make, model)
}

fn _output_handle_mode(data, wl_output, flags, width, height, refresh){
   if(!data){ return }
   if(band(flags, WL_OUTPUT_MODE_CURRENT)){
      store64_h(data, width, _WO_MODE_W)
      store64_h(data, height, _WO_MODE_H)
      store64_h(data, _refresh_hz(refresh), _WO_REFRESH)
   }
}

fn _output_handle_done(data, wl_output){
   if(!data){ return }
   if(load64_h(data, _WO_WIDTH_MM) <= 0 || load64_h(data, _WO_HEIGHT_MM) <= 0){
      def w = load64_h(data, _WO_MODE_W)
      def h = load64_h(data, _WO_MODE_H)
      if(w > 0 && h > 0){
         store64_h(data, int(w * 25.4 / 96.0), _WO_WIDTH_MM)
         store64_h(data, int(h * 25.4 / 96.0), _WO_HEIGHT_MM)
      }
   }
   if(load64_h(data, _WO_ANNOUNCED) == 0){
      store64_h(data, 1, _WO_ANNOUNCED)
      mut ev_data = dict()
      ev_data = dict_set(ev_data, "output", wl_output)
      _broadcast_event(EVENT_MONITOR_CONNECTED, ev_data)
   }
}

fn _output_handle_scale(data, wl_output, factor){
   "Updates output scale and notifies windows on that output."
   if(!data){ return }
   store64_h(data, int(factor), _WO_SCALE)

   ;; notify all windows on this output
   mut keys = dict_keys(_windows)
   mut i = 0
   while(i < len(keys)){
      def handle = get(keys, i)
      def win = dict_get(_windows, handle, 0)
      if(is_dict(win)){
         def outputs = dict_get(win, "outputs", [])
         mut on_it = false
         mut j = 0
         while(j < len(outputs)){
         if(get(outputs, j) == wl_output){ on_it = true break }
         j += 1
         }
         if(on_it){
         _push_event(win, EVENT_SCALE_UPDATED, int(factor))
         }
      }
      i += 1
   }
}

fn _output_handle_name(data, wl_output, name){
   if(!data){ return }
   _free_cstr_ptr(load64_h(data, _WO_NAME))
   store64_h(data, name ? strdup(name) : 0, _WO_NAME)
}

fn _output_handle_description(data, wl_output, description){
   if(!data){ return }
   _free_cstr_ptr(load64_h(data, _WO_DESCRIPTION))
   store64_h(data, description ? strdup(description) : 0, _WO_DESCRIPTION)
}

fn _install_output_listener(out_state){
   if(!out_state || load64_h(out_state, _WO_LISTENER)){ return true }
   def listener = calloc(6, 8)
   if(!listener){ return false }
   store64_h(listener, _output_handle_geometry, 0)
   store64_h(listener, _output_handle_mode, 8)
   store64_h(listener, _output_handle_done, 16)
   store64_h(listener, _output_handle_scale, 24)
   store64_h(listener, _output_handle_name, 32)
   store64_h(listener, _output_handle_description, 40)
   if(wl_proxy_add_listener(load64_h(out_state, _WO_PROXY), listener, out_state) != 0){
      free(listener)
      return false
   }
   store64_h(out_state, listener, _WO_LISTENER)
   true
}

fn _find_output_index_by_global_name(state, global_name){
   mut i = 0
   while(i < _output_count(state)){
      def out_state = _output_at(state, i)
      if(out_state && load64_h(out_state, _WO_GLOBAL_NAME) == global_name){ return i }
      i += 1
   }
   -1
}

fn _destroy_output_state(out_state){
   if(!out_state){ return }
   def proxy = load64_h(out_state, _WO_PROXY)
   def listener = load64_h(out_state, _WO_LISTENER)
   if(proxy){ destroy_proxy(proxy) }
   if(listener){ free(listener) }
   _free_cstr_ptr(load64_h(out_state, _WO_NAME))
   _free_cstr_ptr(load64_h(out_state, _WO_DESCRIPTION))
   free(out_state)
}

fn _remove_output_state(state, index){
   if(!state || index < 0 || index >= _output_count(state)){ return false }
   def arr = load64_h(state, _WG_OUTPUTS)
   if(!arr){ return false }
   def count = _output_count(state)
   def out_state = load64_h(arr, index * 8)
   if(out_state){
      if(load64_h(out_state, _WO_ANNOUNCED) != 0){
         mut ev_data = dict()
         ev_data = dict_set(ev_data, "output", load64_h(out_state, _WO_PROXY))
         _broadcast_event(EVENT_MONITOR_DISCONNECTED, ev_data)
      }
      _destroy_output_state(out_state)
   }
   mut i = index
   while(i + 1 < count){
      store64_h(arr, load64_h(arr, (i + 1) * 8), i * 8)
      i += 1
   }
   store64_h(arr, 0, (count - 1) * 8)
   store64_h(state, count - 1, _WG_OUTPUT_COUNT)
   true
}

fn _output_state_to_dict(out_state){
   if(!out_state){ return false }
   mut out = dict()
   out = dict_set(out, "handle", load64_h(out_state, _WO_PROXY))
   out = dict_set(out, "global_name", load64_h(out_state, _WO_GLOBAL_NAME))
   out = dict_set(out, "name", to_str(load64_h(out_state, _WO_NAME)))
   out = dict_set(out, "description", to_str(load64_h(out_state, _WO_DESCRIPTION)))
   out = dict_set(out, "x", load64_h(out_state, _WO_X))
   out = dict_set(out, "y", load64_h(out_state, _WO_Y))
   out = dict_set(out, "width_mm", load64_h(out_state, _WO_WIDTH_MM))
   out = dict_set(out, "height_mm", load64_h(out_state, _WO_HEIGHT_MM))
   out = dict_set(out, "scale", load64_h(out_state, _WO_SCALE))
   out = dict_set(out, "mode_width", load64_h(out_state, _WO_MODE_W))
   out = dict_set(out, "mode_height", load64_h(out_state, _WO_MODE_H))
   out = dict_set(out, "refresh_rate", load64_h(out_state, _WO_REFRESH))
   out
}

fn _ensure_seat_objects(state){
   if(!state){ return }
   def seat = load64_h(state, _WG_SEAT)
   def manager = load64_h(state, _WG_DATA_DEVICE_MANAGER)
   def caps = load64_h(state, _WG_SEAT_CAPS)
   if(seat && band(caps, WL_SEAT_CAPABILITY_POINTER) && load64_h(state, _WG_POINTER) == 0){
      def pointer = _create_seat_pointer(seat)
      if(pointer){
         store64_h(state, pointer, _WG_POINTER)
         _install_pointer_listener(state)
      }
   }
   if(seat && band(caps, WL_SEAT_CAPABILITY_KEYBOARD) && load64_h(state, _WG_KEYBOARD) == 0){
      def keyboard = _create_seat_keyboard(seat)
      if(keyboard){
         store64_h(state, keyboard, _WG_KEYBOARD)
         _install_keyboard_listener(state)
      }
   }
   if(manager && seat && load64_h(state, _WG_DATA_DEVICE) == 0){
      def device = _create_data_device(manager, seat)
      if(device){
         store64_h(state, device, _WG_DATA_DEVICE)
         _install_data_device_listener(state)
      }
   }
}

fn _pointer_handle_enter(data, pointer, serial, surface, sx, sy){
   if(!data){ return }
   store64_h(data, int(serial), _WG_POINTER_ENTER_SERIAL)
   store64_h(data, surface, _WG_POINTER_FOCUS)

   def win = dict_get(_windows, surface, 0)
   if(!win){ return }

   def fx = float(int(sx)) / 256.0
   def fy = float(int(sy)) / 256.0
   mut next_win = dict_set(win, "mouse_x", int(fx))
   next_win = dict_set(next_win, "mouse_y", int(fy))
   _windows = dict_set(_windows, surface, next_win)

   ;; Apply cursor mode
   def mode = dict_get(next_win, "mode_" + to_str(CURSOR), 0)
   if(mode == CURSOR_HIDDEN || mode == CURSOR_DISABLED){
      wl_proxy_marshal_flags_full_set_cursor(pointer, serial, 0, 0, 0)
   }

   _push_event(next_win, EVENT_MOUSE_POS_CHANGED, [fx, fy])
}

fn _pointer_handle_leave(data, pointer, serial, surface){
   if(!data){ return }
   store64_h(data, 0, _WG_POINTER_FOCUS)
}

fn _pointer_handle_motion(data, pointer, time, sx, sy){
   if(!data){ return }
   def surface = load64_h(data, _WG_POINTER_FOCUS)
   def win = dict_get(_windows, surface, 0)
   if(!win){ return }
   def fx = float(sx) / 256.0 ;; Wayland fixed-point to float
   def fy = float(sy) / 256.0
   _push_event(win, EVENT_MOUSE_POS_CHANGED, [fx, fy])
}

fn _pointer_handle_button(data, pointer, serial, time, button, state){
   if(!data){ return }
   def surface = load64_h(data, _WG_POINTER_FOCUS)
   def win = dict_get(_windows, surface, 0)
   if(!win){ return }
   def kind = (state == 1) ? EVENT_MOUSE_BUTTON_PRESSED : EVENT_MOUSE_BUTTON_RELEASED
   mut btn = 0 ;; Map button code
   if(button == 0x110){ btn = 0 } ;; BTN_LEFT
   elif(button == 0x111){ btn = 1 } ;; BTN_RIGHT
   elif(button == 0x112){ btn = 2 } ;; BTN_MIDDLE
   _push_event(win, kind, btn)
}

fn _pointer_handle_axis(data, pointer, time, axis, value){
   if(!data){ return }
   def surface = load64_h(data, _WG_POINTER_FOCUS)
   def win = dict_get(_windows, surface, 0)
   if(!win){ return }
   def scroll_val = float(value) / 256.0 ;; Wayland fixed-point to float
   mut dx = 0.0 mut dy = 0.0
   if(axis == 0){ dy = -scroll_val } ;; Vertical axis
   else { dx = scroll_val } ;; Horizontal axis
   _push_event(win, EVENT_MOUSE_SCROLL, [dx, dy])
}

fn _relative_pointer_handle_motion(data, rel_ptr, utime_hi, utime_lo, dx, dy, dx_unaccel, dy_unaccel){
   if(!data){ return }
   def surface = load64_h(data, 0)
   mut win = dict_get(_windows, surface, 0)
   if(!win){ return }

   def fdx = float(int(dx)) / 256.0
   def fdy = float(int(dy)) / 256.0

   mut mx = float(dict_get(win, "mouse_x", 0))
   mut my = float(dict_get(win, "mouse_y", 0))
   mx += fdx
   my += fdy

   win = dict_set(win, "mouse_x", int(mx))
   win = dict_set(win, "mouse_y", int(my))
   _windows = dict_set(_windows, surface, win)

   _push_event(win, EVENT_MOUSE_POS_CHANGED, [mx, my])
}

fn _locked_pointer_handle_locked(data, locked_ptr){
}

fn _locked_pointer_handle_unlocked(data, locked_ptr){
}

fn _confined_pointer_handle_confined(data, confined_ptr){
}

fn _confined_pointer_handle_unconfined(data, confined_ptr){
}

fn _install_pointer_listener(state){
   if(!state){ return false }
   def pointer = load64_h(state, _WG_POINTER)
   if(!pointer || load64_h(state, _WG_POINTER_LISTENER)){ return true }
   def listener = calloc(9, 8) ;; Wayland pointer has 9 events in current spec
   if(!listener){ return false }
   store64_h(listener, _pointer_handle_enter, 0)
   store64_h(listener, _pointer_handle_leave, 8)
   store64_h(listener, _pointer_handle_motion, 16)
   store64_h(listener, _pointer_handle_button, 24)
   store64_h(listener, _pointer_handle_axis, 32)
   if(wl_proxy_add_listener(pointer, listener, state) != 0){
      free(listener)
      return false
   }
   store64_h(state, listener, _WG_POINTER_LISTENER)
   true
}

fn _get_xkb_mods(data){
   if(!data){ return 0 }
   def state = load64_h(data, _WG_XKB_STATE)
   if(!state){ return 0 }
   mut mods = 0
   def XKB_STATE_MODS_EFFECTIVE = 1
   if(xkb_state_mod_index_is_active(state, load32(data, _WG_XKB_MOD_SHIFT), XKB_STATE_MODS_EFFECTIVE)){ mods = bor(mods, MOD_SHIFT) }
   if(xkb_state_mod_index_is_active(state, load32(data, _WG_XKB_MOD_CTRL), XKB_STATE_MODS_EFFECTIVE)){ mods = bor(mods, MOD_CONTROL) }
   if(xkb_state_mod_index_is_active(state, load32(data, _WG_XKB_MOD_ALT), XKB_STATE_MODS_EFFECTIVE)){ mods = bor(mods, MOD_ALT) }
   if(xkb_state_mod_index_is_active(state, load32(data, _WG_XKB_MOD_SUPER), XKB_STATE_MODS_EFFECTIVE)){ mods = bor(mods, MOD_SUPER) }
   if(xkb_state_mod_index_is_active(state, load32(data, _WG_XKB_MOD_CAPS), XKB_STATE_MODS_EFFECTIVE)){ mods = bor(mods, MOD_CAPS_LOCK) }
   if(xkb_state_mod_index_is_active(state, load32(data, _WG_XKB_MOD_NUM), XKB_STATE_MODS_EFFECTIVE)){ mods = bor(mods, MOD_NUM_LOCK) }
   mods
}

fn _keyboard_handle_keymap(data, keyboard, format, fd, size){
   if(!data){ if(fd){ close(fd) } return }
   if(int(format) != 0){ ;; NOT XKB_V1
      close(fd)
      return
   }

   def PROT_READ = 1
   def MAP_PRIVATE = 2
   def map = mmap(0, size, PROT_READ, MAP_PRIVATE, int(fd), 0)
   if(int(map) == -1){ close(fd) return }

   def ctx = load64_h(data, _WG_XKB_CONTEXT)
   if(!ctx){
      def next_ctx = xkb_context_new(0)
      if(next_ctx){
         store64_h(data, next_ctx, _WG_XKB_CONTEXT)
      } else {
         munmap(map, size)
         close(fd)
         return
      }
   }

   def old_keymap = load64_h(data, _WG_XKB_KEYMAP)
   def old_state = load64_h(data, _WG_XKB_STATE)

   def next_keymap = xkb_keymap_new_from_string(load64_h(data, _WG_XKB_CONTEXT), map, 0, 0)
   if(next_keymap){
      def next_state = xkb_state_new(next_keymap)
      if(next_state){
         store64_h(data, next_keymap, _WG_XKB_KEYMAP)
         store64_h(data, next_state, _WG_XKB_STATE)

         ;; Update mod indices
         store32(data, xkb_keymap_mod_get_index(next_keymap, cstr("Shift")), _WG_XKB_MOD_SHIFT)
         store32(data, xkb_keymap_mod_get_index(next_keymap, cstr("Control")), _WG_XKB_MOD_CTRL)
         store32(data, xkb_keymap_mod_get_index(next_keymap, cstr("Alt")), _WG_XKB_MOD_ALT)
         store32(data, xkb_keymap_mod_get_index(next_keymap, cstr("Super")), _WG_XKB_MOD_SUPER)
         store32(data, xkb_keymap_mod_get_index(next_keymap, cstr("Caps Lock")), _WG_XKB_MOD_CAPS)
         store32(data, xkb_keymap_mod_get_index(next_keymap, cstr("Num Lock")), _WG_XKB_MOD_NUM)

         if(old_state){ xkb_state_unref(old_state) }
         if(old_keymap){ xkb_keymap_unref(old_keymap) }
      } else {
         xkb_keymap_unref(next_keymap)
      }
   }

   munmap(map, size)
   close(int(fd))
}

fn _keyboard_handle_enter(data, keyboard, serial, surface, keys){
   if(!data){ return }
   store64_h(data, surface, _WG_KEYBOARD_FOCUS)
   mut win = dict_get(_windows, surface, 0)
   if(win && !dict_get(win, "focused", false)){
      win = dict_set(win, "focused", true)
      _windows = dict_set(_windows, surface, win)
      _push_event(win, EVENT_FOCUS_IN, 0)
   }
}

fn _keyboard_handle_leave(data, keyboard, serial, surface){
   if(!data){ return }
   store64_h(data, 0, _WG_KEYBOARD_FOCUS)
   mut win = dict_get(_windows, surface, 0)
   if(win){
      ;; Release all held keys
      def ks_old = dict_get(win, "key_states", 0)
      if(ks_old){
         win = dict_set(win, "key_states", dict(64))
      }
      if(dict_get(win, "focused", false)){
         win = dict_set(win, "focused", false)
         _windows = dict_set(_windows, surface, win)
         _push_event(win, EVENT_FOCUS_OUT, 0)
      } else {
         _windows = dict_set(_windows, surface, win)
      }
   }
}

fn _keyboard_handle_key(data, keyboard, serial, time, key, state){
   if(!data){ return }
   def surface = load64_h(data, _WG_KEYBOARD_FOCUS)
   mut win = dict_get(_windows, surface, 0)
   if(!win){ return }
   def kind = (state == 1) ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED

   ;; Use XKB if available, otherwise fallback to evdev+8 table
   mut nk = 0
   def xkb_state = load64_h(data, _WG_XKB_STATE)
   if(xkb_state){
      def sym = xkb_state_key_get_one_sym(xkb_state, int(key + 8))
      nk = x11_keymap.translate_keysym(sym)
   }

   if(nk == 0){
      nk = x11_keymap.translate_scancode(int(key) + 8)
   }

   mut ks = dict_get(win, "key_states", dict(64))
   ks = dict_set(ks, nk, (state == 1))
   win = dict_set(win, "key_states", ks)
   _windows = dict_set(_windows, surface, win)

   mut ev_data = dict()
   ev_data = dict_set(ev_data, "key", nk)
   ev_data = dict_set(ev_data, "scancode", int(key))
   ev_data = dict_set(ev_data, "action", (state == 1) ? backend_api.ACTION_PRESS : backend_api.ACTION_RELEASE)
   ev_data = dict_set(ev_data, "mods", _get_xkb_mods(data))
   _push_event(win, kind, ev_data)

   ;; Track key repeat state
   def rate = load64_h(data, _WG_REPEAT_RATE)
   if(rate > 0){
      if(state == 1){
         def ts = malloc(16)
         if(ts){
         memset(ts, 0, 16)
         __clock_gettime(1, ts)
         def sec = load64_h(ts, 0)
         def nsec = load64_h(ts, 8)
         def now_ns = from_int(sec) * 1000000000 + from_int(nsec)
         free(ts)
         store64_h(data, nk, _WG_REPEAT_KEY)
         store64_h(data, key, _WG_REPEAT_SCANCODE)
         store64_h(data, now_ns, _WG_REPEAT_START_NS)
         store64_h(data, now_ns, _WG_REPEAT_LAST_NS)
         }
      } elif(state == 0){
         if(load64_h(data, _WG_REPEAT_KEY) == nk){
         store64_h(data, 0, _WG_REPEAT_KEY)
         }
      }
   }

   ;; Emit EVENT_KEY_CHAR for printable characters on key press
   if(state == 1 && xkb_state){
      def utf8_buf = malloc(8)
      if(utf8_buf){
         memset(utf8_buf, 0, 8)
         def nbytes = xkb_state_key_get_utf8(xkb_state, int(key + 8), utf8_buf, 8)
         if(nbytes > 0){
         def b0 = load8(utf8_buf, 0)
         ;; Skip control characters (< 0x20) and DEL (0x7f)
         if(b0 >= 0x20 && b0 != 0x7f){
               ;; Decode the UTF-8 codepoint
               mut cp = 0
               if(b0 < 0x80){ cp = b0 }
               elif(band(b0, 0xe0) == 0xc0 && nbytes >= 2){
                  cp = bor(bshl(band(b0, 0x1f), 6), band(load8(utf8_buf, 1), 0x3f))
               } elif(band(b0, 0xf0) == 0xe0 && nbytes >= 3){
                  cp = bor(bshl(band(b0, 0x0f), 12),
                     bor(bshl(band(load8(utf8_buf, 1), 0x3f), 6), band(load8(utf8_buf, 2), 0x3f)))
               } elif(band(b0, 0xf8) == 0xf0 && nbytes >= 4){
                  cp = bor(bshl(band(b0, 0x07), 18),
                     bor(bshl(band(load8(utf8_buf, 1), 0x3f), 12),
                  bor(bshl(band(load8(utf8_buf, 2), 0x3f), 6), band(load8(utf8_buf, 3), 0x3f))))
               }
               if(cp > 0){
                  mut char_data = dict()
                  char_data = dict_set(char_data, "char", cp)
                  char_data = dict_set(char_data, "mod", _get_xkb_mods(data))
                  _push_event(win, EVENT_KEY_CHAR, char_data)
               }
         }
         }
         free(utf8_buf)
      }
   }
}

fn _synthesize_key_repeat(win, state){
   "Synthesizes wl_keyboard key-repeat events based on stored rate/delay."
   if(!state){ return }
   def repeat_key = load64_h(state, _WG_REPEAT_KEY)
   if(!repeat_key){ return }
   def rate = load64_h(state, _WG_REPEAT_RATE)
   if(!rate){ return }
   def delay_ms = load64_h(state, _WG_REPEAT_DELAY)
   def ts = malloc(16)
   if(!ts){ return }
   memset(ts, 0, 16)
   __clock_gettime(1, ts)
   def sec = load64_h(ts, 0)
   def nsec = load64_h(ts, 8)
   def now_ns = from_int(sec) * 1000000000 + from_int(nsec)
   free(ts)
   def start_ns = load64_h(state, _WG_REPEAT_START_NS)
   def last_ns = load64_h(state, _WG_REPEAT_LAST_NS)
   def elapsed_ms = (now_ns - start_ns) / 1000000
   if(elapsed_ms < from_int(delay_ms)){ return }
   def interval_ns = 1000000000 / from_int(rate)
   mut fire_ns = last_ns + interval_ns
   def scancode = load64_h(state, _WG_REPEAT_SCANCODE)
   mut ev_data = dict()
   ev_data = dict_set(ev_data, "key", from_int(repeat_key))
   ev_data = dict_set(ev_data, "scancode", int(scancode))
   ev_data = dict_set(ev_data, "action", backend_api.ACTION_REPEAT)
   ev_data = dict_set(ev_data, "mods", _get_xkb_mods(state))
   while(fire_ns <= now_ns){
      _push_event(win, EVENT_KEY_PRESSED, ev_data)
      store64_h(state, fire_ns, _WG_REPEAT_LAST_NS)
      fire_ns = fire_ns + interval_ns
   }
}

fn poll_window_events(win, max_events=64){
   "Drains pending events for the given window from the shared backend queue."
   if(!win || !is_dict(win)){ return [win, []] }
   win = poll_joystick_events(win)
   def handle = dict_get(win, "handle", 0)
   def globals = dict_get(win, "globals", 0)
   def state = globals ? dict_get(globals, "listener_state", 0) : 0
   if(state){ _synthesize_key_repeat(win, state) }
   mut out = []
   mut remaining = []
   mut i = 0 mut n = len(_pending_events)
   mut count = 0
   while(i < n){
      def ev = get(_pending_events, i)
      def ev_win = dict_get(ev, "window", 0)
      def match_win = (ev_win == win) || (is_dict(ev_win) && dict_get(ev_win, "handle", 0) == handle)
      if(match_win && count < max_events){
         out = append(out, ev)
         count += 1
      } else {
         remaining = append(remaining, ev)
      }
      i += 1
   }
   _pending_events = remaining
   _dbgu("poll_window_events: " + to_str(count) + " events dispatched, " + to_str(len(remaining)) + " still pending")
   [win, out]
}

fn _keyboard_handle_modifiers(data, keyboard, serial, mods_depres, mods_latched, mods_locked, group){
   if(!data){ return }
   store64_h(data, mods_depres, _WG_KEYBOARD_MODS)
   def state = load64_h(data, _WG_XKB_STATE)
   if(state){
      xkb_state_update_mask(state, mods_depres, mods_latched, mods_locked, 0, 0, group)
   }
}

fn _keyboard_handle_repeat_info(data, keyboard, rate, delay){
   "Stores key repeat rate and delay from the compositor."
   if(!data){ return }
   store64_h(data, rate, _WG_REPEAT_RATE)
   store64_h(data, delay, _WG_REPEAT_DELAY)
}

fn _install_keyboard_listener(state){
   if(!state){ return false }
   def keyboard = load64_h(state, _WG_KEYBOARD)
   if(!keyboard || load64_h(state, _WG_KEYBOARD_LISTENER)){ return true }
   def listener = calloc(6, 8)
   if(!listener){ return false }
   store64_h(listener, _keyboard_handle_keymap, 0)
   store64_h(listener, _keyboard_handle_enter, 8)
   store64_h(listener, _keyboard_handle_leave, 16)
   store64_h(listener, _keyboard_handle_key, 24)
   store64_h(listener, _keyboard_handle_modifiers, 32)
   store64_h(listener, _keyboard_handle_repeat_info, 40)
   if(wl_proxy_add_listener(keyboard, listener, state) != 0){
      free(listener)
      return false
   }
   store64_h(state, listener, _WG_KEYBOARD_LISTENER)
   true
}

fn _seat_handle_capabilities(data, seat, capabilities){
   if(!data){ return }
   store64_h(data, capabilities, _WG_SEAT_CAPS)
   if(!band(capabilities, WL_SEAT_CAPABILITY_POINTER)){
      def pointer = load64_h(data, _WG_POINTER)
      if(pointer){
         destroy_proxy(pointer)
         store64_h(data, 0, _WG_POINTER)
      }
   }
   if(!band(capabilities, WL_SEAT_CAPABILITY_KEYBOARD)){
      def keyboard = load64_h(data, _WG_KEYBOARD)
      if(keyboard){
         destroy_proxy(keyboard)
         store64_h(data, 0, _WG_KEYBOARD)
      }
   }
   _ensure_seat_objects(data)
}

fn _seat_handle_name(data, seat, name){
   if(!data){ return }
   def old = load64_h(data, _WG_SEAT_NAME)
   if(old){ free(old) }
   if(name){
      store64_h(data, strdup(name), _WG_SEAT_NAME)
   } else {
      store64_h(data, 0, _WG_SEAT_NAME)
   }
}

fn _install_seat_listener(state){
   if(!state){ return false }
   def seat = load64_h(state, _WG_SEAT)
   if(!seat){ return false }
   if(load64_h(state, _WG_SEAT_LISTENER)){ return true }
   def listener = calloc(2, 8)
   if(!listener){ return false }
   store64_h(listener, _seat_handle_capabilities, 0)
   store64_h(listener, _seat_handle_name, 8)
   if(wl_proxy_add_listener(seat, listener, state) != 0){
      free(listener)
      return false
   }
   store64_h(state, listener, _WG_SEAT_LISTENER)
   true
}

fn _xdg_wm_base_handle_ping(data, wm_base, serial){
   "Responds to Wayland xdg_wm_base ping events to keep the connection alive."
   if(!wm_base){ return }
   wl_proxy_marshal_flags_ptr(wm_base, XDG_WM_BASE_PONG, 0, int(get_proxy_version(wm_base)), 0, int(serial))
}

fn _install_wm_base_listener(state){
   if(!state){ return false }
   def wm_base = load64_h(state, _WG_WM_BASE)
   if(!wm_base){ return false }
   if(load64_h(state, _WG_WM_BASE_LISTENER)){ return true }
   def listener = calloc(1, 8)
   if(!listener){ return false }
   store64_h(listener, _xdg_wm_base_handle_ping, 0)
   if(wl_proxy_add_listener(wm_base, listener, state) != 0){
      free(listener)
      return false
   }
   store64_h(state, listener, _WG_WM_BASE_LISTENER)
   true
}

fn _registry_handle_global(data, registry, name, interface, version){
   if(!data || !registry || !interface){ return }
   def iface = to_str(interface)
   if(iface == "wl_compositor" && load64_h(data, _WG_COMPOSITOR) == 0){
      def sym = _interface_symbol("wl_compositor_interface")
      if(sym){
         def bind_ver = _bind_version(int(version), 4)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){
         store64_h(data, proxy, _WG_COMPOSITOR)
         store64_h(data, bind_ver, _WG_COMPOSITOR_VER)
         }
      }
      return
   }
   if(iface == "wl_subcompositor" && load64_h(data, _WG_SUBCOMPOSITOR) == 0){
      def sym = _interface_symbol("wl_subcompositor_interface")
      if(sym){
         def bind_ver = _bind_version(int(version), 1)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){
         store64_h(data, proxy, _WG_SUBCOMPOSITOR)
         store64_h(data, bind_ver, _WG_SUBCOMPOSITOR_VER)
         }
      }
      return
   }
   if(iface == "wl_shm" && load64_h(data, _WG_SHM) == 0){
      def sym = _interface_symbol("wl_shm_interface")
      if(sym){
         def bind_ver = _bind_version(int(version), 1)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){
         store64_h(data, proxy, _WG_SHM)
         store64_h(data, bind_ver, _WG_SHM_VER)
         }
      }
      return
   }
   if(iface == "wl_seat" && load64_h(data, _WG_SEAT) == 0){
      def sym = _interface_symbol("wl_seat_interface")
      if(sym){
         def bind_ver = _bind_version(int(version), 5)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){
         store64_h(data, proxy, _WG_SEAT)
         store64_h(data, bind_ver, _WG_SEAT_VER)
         _install_seat_listener(data)
         }
      }
      return
   }
   if(iface == "wl_data_device_manager" && load64_h(data, _WG_DATA_DEVICE_MANAGER) == 0){
      def sym = _interface_symbol("wl_data_device_manager_interface")
      if(sym){
         def bind_ver = _bind_version(int(version), 3)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){
         store64_h(data, proxy, _WG_DATA_DEVICE_MANAGER)
         store64_h(data, bind_ver, _WG_DATA_DEVICE_MANAGER_VER)
         _ensure_seat_objects(data)
         }
      }
      return
   }
   if(iface == "wl_output"){
      def sym = _interface_symbol("wl_output_interface")
      if(!sym || int(version) < 2){ return }
      def bind_ver = _bind_version(int(version), 4)
      def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
      if(!proxy){ return }
      def out_state = _new_output_state(proxy, name)
      if(!out_state){
         destroy_proxy(proxy)
         return
      }
      if(!_install_output_listener(out_state) || !_append_output_state(data, out_state)){
         _destroy_output_state(out_state)
      }
      return
   }
   if(iface == "xdg_wm_base" && load64_h(data, _WG_WM_BASE) == 0){
      def sym = _interface_symbol("xdg_wm_base_interface")
      if(sym){
         def bind_ver = _bind_version(int(version), 1)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){
         store64_h(data, proxy, _WG_WM_BASE)
         store64_h(data, bind_ver, _WG_WM_BASE_VER)
         _install_wm_base_listener(data)
         }
      }
      return
   }
   if(iface == "zwp_relative_pointer_manager_v1" && load64_h(data, _WG_RELATIVE_POINTER_MANAGER) == 0){
      _init_unstable_interfaces()
      def sym = _wp_relative_pointer_manager_interface
      if(sym){
         def bind_ver = _bind_version(int(version), 1)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){
         store64_h(data, proxy, _WG_RELATIVE_POINTER_MANAGER)
         store64_h(data, bind_ver, _WG_RELATIVE_POINTER_MANAGER_VER)
         }
      }
      return
   }
   if(iface == "zwp_pointer_constraints_v1" && load64_h(data, _WG_POINTER_CONSTRAINTS) == 0){
      _init_unstable_interfaces()
      def sym = _wp_pointer_constraints_interface
      if(sym){
         def bind_ver = _bind_version(int(version), 1)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){
         store64_h(data, proxy, _WG_POINTER_CONSTRAINTS)
         store64_h(data, bind_ver, _WG_POINTER_CONSTRAINTS_VER)
         }
      }
      return
   }
   if(iface == "zxdg_decoration_manager_v1" && load64_h(data, _WG_DECORATION_MANAGER) == 0){
      _init_unstable_interfaces()
      def sym = _wp_decoration_manager_interface
      if(sym){
         def bind_ver = _bind_version(int(version), 1)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){
         store64_h(data, proxy, _WG_DECORATION_MANAGER)
         store64_h(data, bind_ver, _WG_DECORATION_MANAGER_VER)
         }
      }
      return
   }
   if(iface == "zwp_text_input_manager_v3" && load64_h(data, _WG_TEXT_INPUT_MANAGER) == 0){
      _init_unstable_interfaces()
      def sym = _wp_text_input_manager_interface
      if(sym){
         def bind_ver = _bind_version(int(version), 1)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){ store64_h(data, proxy, _WG_TEXT_INPUT_MANAGER) }
      }
      return
   }
   if(iface == "wp_viewporter" && load64_h(data, _WG_VIEWPORTER) == 0){
      _init_unstable_interfaces()
      def sym = _wp_viewporter_interface
      if(sym){
         def bind_ver = _bind_version(int(version), 1)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){ store64_h(data, proxy, _WG_VIEWPORTER) }
      }
      return
   }
   if(iface == "wp_fractional_scale_manager_v1" && load64_h(data, _WG_FRACTIONAL_SCALE_MANAGER) == 0){
      _init_unstable_interfaces()
      def sym = _wp_fractional_scale_manager_interface
      if(sym){
         def bind_ver = _bind_version(int(version), 1)
         def proxy = wl_registry_bind(registry, int(name), sym, int(bind_ver))
         if(proxy){ store64_h(data, proxy, _WG_FRACTIONAL_SCALE_MANAGER) }
      }
      return
   }
}

fn _registry_handle_global_remove(data, registry, name){
   if(!data){ return }
   def index = _find_output_index_by_global_name(data, name)
   if(index >= 0){ _remove_output_state(data, index) }
}

fn bootstrap_globals(display){
   "Bootstraps core Wayland globals from the registry similarly to GLFW `wl_init.c`."
   _dbg("bootstrap_globals: display=" + to_hex(display))
   if(!display){ _dbg("bootstrap_globals: no display") return false }
   def registry = get_registry(display)
   if(!registry){ _dbg("bootstrap_globals: no registry") return false }
   def state = calloc(1, _WG_SIZE)
   if(!state){
      _dbg("bootstrap_globals: calloc failed")
      destroy_registry(registry)
      return false
   }
   def listener = calloc(2, 8)
   if(!listener){
      free(state)
      destroy_registry(registry)
      return false
   }
   store64_h(state, display, _WG_DISPLAY)
   store64_h(listener, _registry_handle_global, 0)
   store64_h(listener, _registry_handle_global_remove, 8)
   if(wl_registry_add_listener(registry, listener, state) != 0){
      free(listener)
      free(state)
      destroy_registry(registry)
      return false
   }
   def rr1 = roundtrip(display)
   def rr2 = roundtrip(display)
   mut out = dict()
   out = dict_set(out, "handle", display)
   out = dict_set(out, "registry", registry)
   out = dict_set(out, "listener", listener)
   out = dict_set(out, "listener_state", state)
   out = dict_set(out, "roundtrip_1", rr1)
   out = dict_set(out, "roundtrip_2", rr2)
   out = dict_set(out, "compositor", load64_h(state, _WG_COMPOSITOR))
   out = dict_set(out, "subcompositor", load64_h(state, _WG_SUBCOMPOSITOR))
   out = dict_set(out, "shm", load64_h(state, _WG_SHM))
   out = dict_set(out, "seat", load64_h(state, _WG_SEAT))
   out = dict_set(out, "pointer", load64_h(state, _WG_POINTER))
   out = dict_set(out, "keyboard", load64_h(state, _WG_KEYBOARD))
   out = dict_set(out, "data_device", load64_h(state, _WG_DATA_DEVICE))
   out = dict_set(out, "data_device_manager", load64_h(state, _WG_DATA_DEVICE_MANAGER))
   out = dict_set(out, "seat_caps", load64_h(state, _WG_SEAT_CAPS))
   out = dict_set(out, "seat_name_ptr", load64_h(state, _WG_SEAT_NAME))
   out = dict_set(out, "seat_name", to_str(load64_h(state, _WG_SEAT_NAME)))
   out = dict_set(out, "seat_listener", load64_h(state, _WG_SEAT_LISTENER))
   out = dict_set(out, "output_count", load64_h(state, _WG_OUTPUT_COUNT))
   mut outputs = []
   mut oi = 0
   while(oi < _output_count(state)){
      outputs = append(outputs, _output_state_to_dict(_output_at(state, oi)))
      oi += 1
   }
   out = dict_set(out, "outputs", outputs)
   out = dict_set(out, "wm_base", load64_h(state, _WG_WM_BASE))
   out = dict_set(out, "wm_base_version", load64_h(state, _WG_WM_BASE_VER))
   out = dict_set(out, "wm_base_listener", load64_h(state, _WG_WM_BASE_LISTENER))
   out = dict_set(out, "compositor_version", load64_h(state, _WG_COMPOSITOR_VER))
   out = dict_set(out, "subcompositor_version", load64_h(state, _WG_SUBCOMPOSITOR_VER))
   out = dict_set(out, "shm_version", load64_h(state, _WG_SHM_VER))
   out = dict_set(out, "seat_version", load64_h(state, _WG_SEAT_VER))
   out = dict_set(out, "data_device_manager_version", load64_h(state, _WG_DATA_DEVICE_MANAGER_VER))
   out = dict_set(out, "relative_pointer_manager", load64_h(state, _WG_RELATIVE_POINTER_MANAGER))
   out = dict_set(out, "pointer_constraints", load64_h(state, _WG_POINTER_CONSTRAINTS))
   out = dict_set(out, "decoration_manager", load64_h(state, _WG_DECORATION_MANAGER))
   out
}

fn destroy_globals(globals){
   "Destroys the bootstrapped Wayland globals tracked by `bootstrap_globals`."
   if(!globals || !is_dict(globals)){ return false }
   def pointer = dict_get(globals, "pointer", 0)
   def keyboard = dict_get(globals, "keyboard", 0)
   def device = dict_get(globals, "data_device", 0)
   def ddm = dict_get(globals, "data_device_manager", 0)
   def seat = dict_get(globals, "seat", 0)
   def shm = dict_get(globals, "shm", 0)
   def sub = dict_get(globals, "subcompositor", 0)
   def comp = dict_get(globals, "compositor", 0)
   def wm_base = dict_get(globals, "wm_base", 0)
   def rel_ptr_mgr = dict_get(globals, "relative_pointer_manager", 0)
   def ptr_constraints = dict_get(globals, "pointer_constraints", 0)
   def registry = dict_get(globals, "registry", 0)
   def seat_name = dict_get(globals, "seat_name_ptr", 0)
   def listener = dict_get(globals, "listener", 0)
   def seat_listener = dict_get(globals, "seat_listener", 0)
   def wm_base_listener = dict_get(globals, "wm_base_listener", 0)
   def state = dict_get(globals, "listener_state", 0)
   if(state){
      while(_output_count(state) > 0){
         _remove_output_state(state, _output_count(state) - 1)
      }
      def out_arr = load64_h(state, _WG_OUTPUTS)
      if(out_arr){ free(out_arr) }

      def xkb_state = load64_h(state, _WG_XKB_STATE)
      def xkb_keymap = load64_h(state, _WG_XKB_KEYMAP)
      def xkb_ctx = load64_h(state, _WG_XKB_CONTEXT)
      if(xkb_state){ xkb_state_unref(xkb_state) }
      if(xkb_keymap){ xkb_keymap_unref(xkb_keymap) }
      if(xkb_ctx){ xkb_context_unref(xkb_ctx) }
      def dd_listener = load64_h(state, _WG_DATA_DEVICE_LISTENER)
      if(dd_listener){ free(dd_listener) }
      def clip_offer = load64_h(state, _WG_CLIPBOARD_OFFER)
      if(clip_offer){ destroy_proxy(clip_offer) }
   }
   if(_clipboard_source){ destroy_proxy(_clipboard_source) _clipboard_source = 0 }
   if(_cursor_theme){ wl_cursor_theme_destroy(_cursor_theme) _cursor_theme = 0 }
   if(_cursor_surface){ destroy_proxy(_cursor_surface) _cursor_surface = 0 }
   if(device){ destroy_proxy(device) }
   if(keyboard){ destroy_proxy(keyboard) }
   if(pointer){ destroy_proxy(pointer) }
   if(wm_base){ destroy_proxy(wm_base) }
   if(rel_ptr_mgr){ destroy_proxy(rel_ptr_mgr) }
   if(ptr_constraints){ destroy_proxy(ptr_constraints) }
   if(ddm){ destroy_proxy(ddm) }
   if(seat){ destroy_proxy(seat) }
   if(shm){ destroy_proxy(shm) }
   if(sub){ destroy_proxy(sub) }
   if(comp){ destroy_proxy(comp) }
   if(registry){ destroy_registry(registry) }
   if(seat_name){ free(seat_name) }
   if(seat_listener){ free(seat_listener) }
   if(wm_base_listener){ free(wm_base_listener) }
   if(listener){ free(listener) }
   if(state){ free(state) }
   true
}

fn wait_events(display, timeout_ms=-1){
   "Blocks like GLFW Wayland event loop until events arrive or timeout elapses."
   if(!display){ return -1 }
   while(prepare_read(display) != 0){
      if(dispatch_pending(display) > 0){ return 1 }
   }
   flush(display)
   def fds = calloc(1, 8)
   if(!fds){
      cancel_read(display)
      return -1
   }
   store32(fds, get_fd(display), 0)
   store16(fds, POLLIN, 4)
   store16(fds, 0, 6)
   def rc = poll(fds, 1, int(timeout_ms))
   free(fds)
   if(rc <= 0){
      cancel_read(display)
      return rc
   }
   def rr = read_events(display)
   if(rr < 0){ return rr }
   dispatch_pending(display)
}

fn wait_events_queue(display, queue, timeout_ms=-1){
   "Blocks on a specific Wayland event queue like GLFW queue-based EGL path."
   if(!display || !queue){ return -1 }
   while(prepare_read_queue(display, queue) != 0){
      if(dispatch_queue_pending(display, queue) > 0){ return 1 }
   }
   flush(display)
   def fds = calloc(1, 8)
   if(!fds){
      cancel_read(display)
      return -1
   }
   store32(fds, get_fd(display), 0)
   store16(fds, POLLIN, 4)
   store16(fds, 0, 6)
   def rc = poll(fds, 1, int(timeout_ms))
   free(fds)
   if(rc <= 0){
      cancel_read(display)
      return rc
   }
   def rr = read_events(display)
   if(rr < 0){ return rr }
   dispatch_queue_pending(display, queue)
}

fn probe_display(name=0){
   "Checks that a Wayland display can actually be opened."
   def display = connect_display(name)
   if(!display){ return false }
   wl_display_roundtrip(display)
   wl_display_disconnect(display)
   true
}
fn vulkan_supported(){
   "Returns true if the Wayland backend supports Vulkan."
   true
}

mut _wayland_vk_ext_ptrs = 0
mut _wayland_vk_ext_surface = 0
mut _wayland_vk_ext_wayland = 0

fn vulkan_required_extensions(){
   "Returns the Vulkan instance extensions required for a Wayland surface."
   if(!_wayland_vk_ext_ptrs){
      _wayland_vk_ext_surface = cstr("VK_KHR_surface")
      _wayland_vk_ext_wayland = cstr("VK_KHR_wayland_surface")
      def arr = malloc(16)
      store64_h(arr, _wayland_vk_ext_surface, 0)
      store64_h(arr, _wayland_vk_ext_wayland, 8)
      _wayland_vk_ext_ptrs = [2, arr]
   }
   _wayland_vk_ext_ptrs
}

fn vulkan_get_presentation_support(device, queuefamily, display){
   vkGetPhysicalDeviceWaylandPresentationSupportKHR(device, queuefamily, display) != 0
}

fn create_surface(instance, win, allocator, surface){
   "Creates a Vulkan Wayland surface for the given backend window."
   if(!is_dict(win)){ return -1 }
   def globals = dict_get(win, "globals", _wayland_globals)
   def wl_display = dict_get(globals, "display", 0)
   def wl_surface = dict_get(win, "handle", 0)
   if(!wl_display || !wl_surface){ return -1 }
   def info = malloc(40)
   memset(info, 0, 40)
   store32(info, 1000006000, 0) ;; VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR
   store64_h(info, to_int(wl_display), 24)
   store64_h(info, to_int(wl_surface), 32)
   if(!info){ return -1 }
   def res = vkCreateWaylandSurfaceKHR(instance, info, allocator, surface)
   free(info)
   res
}
fn get_gamma_ramp(monitor){
   "Stub for Wayland gamma support (currently unsupported)."
   false
}

fn set_gamma_ramp(monitor, ramp){
   "Stub for Wayland gamma support (currently unsupported)."
   false
}

fn get_monitors(){
   "Returns list of Wayland output dictionaries as monitor handles."
   if(!_wayland_globals){ return [] }
   def count = _output_count(_wayland_globals)
   mut out = []
   mut i = 0 while(i < count){
      def out_state = _output_at(_wayland_globals, i)
      if(out_state){
         def d = _output_state_to_dict(out_state)
         if(d){ out = append(out, d) }
      }
      i += 1
   }
   out
}

fn get_primary_monitor(){
   "Returns the first available Wayland output as the primary monitor."
   def all = get_monitors()
   if(len(all) > 0){ return get(all, 0) }
   0
}

fn get_monitor_pos(monitor){
   "Returns [x, y] of a Wayland output."
   if(!is_dict(monitor)){ return [0, 0] }
   [dict_get(monitor, "x", 0), dict_get(monitor, "y", 0)]
}

fn get_monitor_workarea(monitor){
   "Returns [x, y, w, h] of a Wayland output."
   if(!is_dict(monitor)){ return [0, 0, 0, 0] }
   [dict_get(monitor, "x", 0), dict_get(monitor, "y", 0), dict_get(monitor, "w", 0), dict_get(monitor, "h", 0)]
}

fn get_monitor_physical_size(monitor){
   "Returns [width_mm, height_mm] of a Wayland output."
   if(!is_dict(monitor)){ return [0, 0] }
   [dict_get(monitor, "physical_w", 0), dict_get(monitor, "physical_h", 0)]
}

fn get_monitor_content_scale(monitor){
   "Returns [xscale, yscale] of a Wayland output."
   if(!is_dict(monitor)){ return [1.0, 1.0] }
   def scale = float(dict_get(monitor, "scale", 1))
   [scale, scale]
}

fn get_monitor_name(monitor){
   "Returns the human-readable name of a Wayland output."
   if(!is_dict(monitor)){ return "" }
   dict_get(monitor, "name", "")
}

fn get_video_mode(monitor){
   "Returns current video mode (w, h, refresh) for a Wayland output."
   if(!is_dict(monitor)){ return 0 }
   mut mode = dict()
   mode = dict_set(mode, "w", dict_get(monitor, "w", 0))
   mode = dict_set(mode, "h", dict_get(monitor, "h", 0))
   mode = dict_set(mode, "refresh", dict_get(monitor, "refresh", 0))
   mode
}

fn get_video_modes(monitor){
   "Wayland currently reports only the current/active mode."
   def cur = get_video_mode(monitor)
   if(cur){ return [cur] }
   []
}

fn set_input_mode(win, mode, value){
   "Updates the input mode for a Wayland window and applies it if possible."
   if(!available() || !win || !is_dict(win)){ return win }

   mut next_win = win
   if(mode == CURSOR){
      def globals = dict_get(win, "globals", 0)
      if(!globals){ return win }
      def surface = dict_get(win, "handle", 0)
      def pointer = dict_get(globals, "pointer", 0)

      if(value == CURSOR_CAPTURED){
         def constraints = dict_get(globals, "pointer_constraints", 0)
         if(constraints && pointer){
         _init_unstable_interfaces()
         def conf_listener = calloc(2, 8)
         store64_h(conf_listener, _confined_pointer_handle_confined, 0)
         store64_h(conf_listener, _confined_pointer_handle_unconfined, 8)
         def conf_data = calloc(1, 8)
         store64_h(conf_data, surface, 0)
         ;; confine_pointer: opcode 2, lifetime 2 = persistent
         def confined_ptr = wl_proxy_marshal_flags_lock_ptr(constraints, 2, _wp_confined_pointer_interface, 1, 1, 0, surface, pointer, 0, 2)
         if(confined_ptr){
               wl_proxy_add_listener(confined_ptr, conf_listener, conf_data)
               next_win = dict_set(next_win, "confined_pointer", confined_ptr)
               next_win = dict_set(next_win, "conf_listener", conf_listener)
               next_win = dict_set(next_win, "conf_data", conf_data)
         }
         }
      } elif(value == CURSOR_DISABLED){
         def constraints = dict_get(globals, "pointer_constraints", 0)
         def rel_mgr = dict_get(globals, "relative_pointer_manager", 0)

         if(constraints && rel_mgr && pointer){
         _init_unstable_interfaces()
         ;; Lock pointer
         def lock_listener = calloc(2, 8)
         store64_h(lock_listener, _locked_pointer_handle_locked, 0)
         store64_h(lock_listener, _locked_pointer_handle_unlocked, 8)
         def lock_data = calloc(1, 8)
         store64_h(lock_data, surface, 0)

         ;; lock_pointer(proxy, opcode, iface, ver, flags, id_null, surface, pointer, region, lifetime)
         ;; opcode 1 for lock_pointer, lifetime 2 = persistent
         def locked_ptr = wl_proxy_marshal_flags_lock_ptr(constraints, 1, _wp_locked_pointer_interface, 1, 1, 0, surface, pointer, 0, 2)
         if(locked_ptr){
               wl_proxy_add_listener(locked_ptr, lock_listener, lock_data)
               next_win = dict_set(next_win, "locked_pointer", locked_ptr)
               next_win = dict_set(next_win, "lock_listener", lock_listener)
               next_win = dict_set(next_win, "lock_data", lock_data)
         }

         ;; Get relative pointer
         def rel_listener = calloc(1, 8)
         store64_h(rel_listener, _relative_pointer_handle_motion, 0)
         def rel_data = calloc(1, 8)
         store64_h(rel_data, surface, 0)

         ;; get_relative_pointer(proxy, opcode, iface, ver, flags, id_null, pointer)
         ;; opcode 1 for get_relative_pointer
         def relative_ptr = wl_proxy_marshal_flags_rel_ptr(rel_mgr, 1, _wp_relative_pointer_interface, 1, 1, 0, pointer)
         if(relative_ptr){
               wl_proxy_add_listener(relative_ptr, rel_listener, rel_data)
               next_win = dict_set(next_win, "relative_pointer", relative_ptr)
               next_win = dict_set(next_win, "rel_listener", rel_listener)
               next_win = dict_set(next_win, "rel_data", rel_data)
         }
         }
         ;; Hide cursor by setting null surface
         def state = dict_get(globals, "listener_state", 0)
         if(pointer && state){
         def serial = load64_h(state, _WG_POINTER_ENTER_SERIAL)
         wl_proxy_marshal_flags_ptr_obj(pointer, _WP_SET_CURSOR, 0, wl_proxy_get_version(pointer), 0, 0, 0)
         wl_proxy_marshal_flags_full_set_cursor(pointer, int(serial), 0, 0, 0)
         }
      } else {
         ;; Normal or Hidden - unlock/unconfine if locked/confined
         def locked_ptr = dict_get(win, "locked_pointer", 0)
         if(locked_ptr){
         wl_proxy_marshal_flags_ptr(locked_ptr, 0, 0, 1, 1, 0) ;; destroy
         destroy_proxy(locked_ptr)
         next_win = dict_set(next_win, "locked_pointer", 0)
         free(dict_get(win, "lock_listener", 0))
         free(dict_get(win, "lock_data", 0))
         }
         def confined_ptr = dict_get(win, "confined_pointer", 0)
         if(confined_ptr){
         wl_proxy_marshal_flags_ptr(confined_ptr, 0, 0, 1, 1, 0) ;; destroy
         destroy_proxy(confined_ptr)
         next_win = dict_set(next_win, "confined_pointer", 0)
         free(dict_get(win, "conf_listener", 0))
         free(dict_get(win, "conf_data", 0))
         }
         def relative_ptr = dict_get(win, "relative_pointer", 0)
         if(relative_ptr){
         wl_proxy_marshal_flags_ptr(relative_ptr, 0, 0, 1, 1, 0) ;; destroy
         destroy_proxy(relative_ptr)
         next_win = dict_set(next_win, "relative_pointer", 0)
         free(dict_get(win, "rel_listener", 0))
         free(dict_get(win, "rel_data", 0))
         }

         def state = dict_get(globals, "listener_state", 0)
         if(pointer && state){
         if(value == CURSOR_HIDDEN){
               def serial = load64_h(state, _WG_POINTER_ENTER_SERIAL)
               wl_proxy_marshal_flags_ptr_obj(pointer, _WP_SET_CURSOR, 0, wl_proxy_get_version(pointer), 0, 0, 0)
               wl_proxy_marshal_flags_full_set_cursor(pointer, int(serial), 0, 0, 0)
         } else {
               ;; Normal - restore cursor (usually handled by next enter event or app)
         }
         }
      }
   }

   dict_set(next_win, "mode_" + mode, value)
}

fn get_input_mode(win, mode){
   "Queries the current input mode for the given native Wayland window."
   if(!win || !is_dict(win)){ return 0 }
   dict_get(win, "mode_" + mode, 0)
}

fn get_key_scancode(win, key){
   -1
}

fn wl_proxy_marshal_flags_full_set_cursor(pointer, serial, surface, x, y){
   if(!pointer){ return }
   wl_proxy_marshal_flags_cursor(pointer, _WP_SET_CURSOR, 0, wl_proxy_get_version(pointer), 0, serial, surface, int(x), int(y))
}

fn _text_input_handle_commit(data, text_input, text){
   "zwp_text_input_v3 commit_string event — push EVENT_KEY_CHAR for each codepoint."
   if(!data || !text){ return }
   def win = dict_get(_windows, data, 0)
   if(!win || !is_dict(win)){ return }
   ;; text is a raw UTF-8 C string pointer; decode it byte by byte
   mut i = 0
   while(true){
      def b0 = load8(text, i)
      if(b0 == 0){ break }
      mut cp = -1
      mut next = i + 1
      if(b0 < 0x80){
         cp = b0
      } elif(band(b0, 0xe0) == 0xc0 && load8(text, i + 1) != 0){
         cp = bor(bshl(band(b0, 0x1f), 6), band(load8(text, i + 1), 0x3f))
         next = i + 2
      } elif(band(b0, 0xf0) == 0xe0 && load8(text, i + 1) != 0 && load8(text, i + 2) != 0){
         cp = bor(bshl(band(b0, 0x0f), 12),
         bor(bshl(band(load8(text, i + 1), 0x3f), 6), band(load8(text, i + 2), 0x3f)))
         next = i + 3
      } elif(band(b0, 0xf8) == 0xf0 && load8(text, i + 1) != 0 && load8(text, i + 2) != 0 && load8(text, i + 3) != 0){
         cp = bor(bshl(band(b0, 0x07), 18),
         bor(bshl(band(load8(text, i + 1), 0x3f), 12),
               bor(bshl(band(load8(text, i + 2), 0x3f), 6), band(load8(text, i + 3), 0x3f))))
         next = i + 4
      }
      if(cp > 0){
         mut char_data = dict()
         char_data = dict_set(char_data, "char", cp)
         char_data = dict_set(char_data, "mod", 0)
         _push_event(win, EVENT_KEY_CHAR, char_data)
      }
      i = next
   }
}

fn _text_input_handle_preedit(data, text_input, text, cursor_begin, cursor_end){
   ;; preedit updates — no-op for now
}

fn _text_input_handle_delete_surrounding(data, text_input, before_length, after_length){
   ;; delete surrounding text — no-op for now
}

fn _text_input_handle_done(data, text_input, serial){
   ;; commit sequence done — no-op
}

fn _text_input_handle_enter(data, text_input, surface){
   ;; text input focus enter — no-op
}

fn _text_input_handle_leave(data, text_input, surface){
   ;; text input focus leave — no-op
}

fn _fractional_scale_handle_preferred_scale(data, fs_obj, scale_120){
   "wp_fractional_scale_v1 preferred_scale event — stores scale in win[content_scale]."
   if(!data){ return }
   mut win = dict_get(_windows, data, 0)
   if(!win || !is_dict(win)){ return }
   def scale = float(scale_120) / 120.0
   win = dict_set(win, "content_scale", scale)
   _windows = dict_set(_windows, data, win)
}

fn _create_text_input(state, seat){
   "Creates a zwp_text_input_v3 for a seat using the text input manager."
   def mgr = load64_h(state, _WG_TEXT_INPUT_MANAGER)
   if(!mgr){ return 0 }
   _init_unstable_interfaces()
   if(!_wp_text_input_interface){ return 0 }
   ;; opcode 1 = get_text_input(id, seat)
   wl_proxy_marshal_flags_ptr_obj(mgr, 1, _wp_text_input_interface, int(get_proxy_version(mgr)), 0, 0, seat)
}

fn _create_fractional_scale(state, surface){
   "Creates a wp_fractional_scale_v1 for a surface."
   def mgr = load64_h(state, _WG_FRACTIONAL_SCALE_MANAGER)
   if(!mgr){ return 0 }
   _init_unstable_interfaces()
   if(!_wp_fractional_scale_interface){ return 0 }
   ;; opcode 1 = get_fractional_scale(id, surface)
   wl_proxy_marshal_flags_ptr_obj(mgr, 1, _wp_fractional_scale_interface, int(get_proxy_version(mgr)), 0, 0, surface)
}

fn text_input_enable(win){
   "Enables zwp_text_input_v3 for IME on the given Wayland window."
   if(!win || !is_dict(win)){ return false }
   def ti = dict_get(win, "text_input", 0)
   if(!ti){ return false }
   ;; opcode 1 = enable
   wl_proxy_marshal_flags_ptr(ti, 1, 0, int(get_proxy_version(ti)), 0, 0)
   ;; opcode 6 = commit
   wl_proxy_marshal_flags_ptr(ti, 6, 0, int(get_proxy_version(ti)), 0, 0)
   true
}

fn text_input_disable(win){
   "Disables zwp_text_input_v3 for IME on the given Wayland window."
   if(!win || !is_dict(win)){ return false }
   def ti = dict_get(win, "text_input", 0)
   if(!ti){ return false }
   ;; opcode 2 = disable
   wl_proxy_marshal_flags_ptr(ti, 2, 0, int(get_proxy_version(ti)), 0, 0)
   ;; opcode 6 = commit
   wl_proxy_marshal_flags_ptr(ti, 6, 0, int(get_proxy_version(ti)), 0, 0)
   true
}

fn get_window_content_scale(win){
   "Returns [sx, sy] content scale for a Wayland window (from wp_fractional_scale_v1 or 1.0)."
   if(!win || !is_dict(win)){ return [1.0, 1.0] }
   def scale = float(dict_get(win, "content_scale", 1.0))
   [scale, scale]
}

fn get_window_opacity(win){ 1.0 }
fn get_window_frame_size(win){ [0, 0, 0, 0] }
fn request_window_attention(win){ true }

fn get_content_scale(win){ get_window_content_scale(win) }

fn get_seat_capabilities(win){
   "Returns a dict with keys pointer/keyboard/touch indicating seat capabilities."
   if(!win || !is_dict(win)){ return dict() }
   def globals = dict_get(win, "globals", 0)
   if(!globals){ return dict() }
   def state = dict_get(globals, "listener_state", 0)
   def caps = state ? int(load64_h(state, _WG_SEAT_CAPS)) : 0
   mut out = dict()
   out = dict_set(out, "pointer", !!band(caps, WL_SEAT_CAPABILITY_POINTER))
   out = dict_set(out, "keyboard", !!band(caps, WL_SEAT_CAPABILITY_KEYBOARD))
   out = dict_set(out, "touch", !!band(caps, WL_SEAT_CAPABILITY_TOUCH))
   out
}

fn poll_joystick_events(win){
   "Polls evdev joystick events and returns updated window. Requires linux_joystick module."
   if(!win || !is_dict(win)){ return win }
   if(comptime{ __os_name() == "linux" }){
      linux_joystick.poll_joysticks()
   }
   win
}

fn get_wayland_monitor(mon){ dict_get(mon, "handle", 0) }
