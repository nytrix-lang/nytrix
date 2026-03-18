;; Keywords: platform window backend linux wayland
;; Wayland native window backend for surfaces, input, monitors, clipboard, and Vulkan surfaces.
module std.os.ui.window.platform.linux.wayland(available, get_backend_name, connect_display, disconnect_display, get_registry, destroy_registry, flush, roundtrip, get_fd, dispatch_pending, dispatch, prepare_read, cancel_read, read_events, wait_events, wait_events_queue, create_event_queue, destroy_event_queue, prepare_read_queue, dispatch_queue_pending, create_proxy_wrapper, destroy_proxy_wrapper, set_proxy_queue, get_proxy_user_data, set_proxy_user_data, get_proxy_version, destroy_proxy, bootstrap_globals, destroy_globals, set_globals, vulkan_supported, vulkan_required_extensions, probe_display, presentation_supported, create_surface, create_wl_surface, create_xdg_surface, create_xdg_toplevel, xdg_toplevel_set_title, create_basic_window, destroy_basic_window, poll_window_events, show_window, hide_window, iconify_window, restore_window, maximize_window, get_window_attrib, set_window_opacity, set_window_resizable, set_title, get_size, set_size, get_cursor_pos, set_cursor_pos, set_window_icon, create_cursor, create_standard_cursor, destroy_cursor, set_cursor, get_key_state, get_mouse_button_state, get_key_name, get_window_monitor, set_window_monitor, set_clipboard, get_clipboard, get_gamma_ramp, set_gamma_ramp, get_monitors, get_primary_monitor, get_monitor_pos, get_monitor_workarea, get_monitor_physical_size, get_monitor_content_scale, get_monitor_name, get_wayland_monitor, get_video_mode, get_video_modes, set_input_mode, get_input_mode, get_key_name, get_key_scancode, text_input_enable, text_input_disable, get_window_content_scale, get_window_opacity, get_window_frame_size, request_window_attention, get_seat_capabilities, poll_joystick_events)
use std.core
use std.core.mem (cstr)
use std.core.str as str
use std.core.str (to_hex)
use std.os.ui.render.vk.vulkan (vk_create_wayland_surface_khr)
use std.os.ui.window.consts
use std.os.ui.window.event as ui_event
use std.os.ui.window.platform.api
use std.os.ui.window.platform.api as backend_api
use std.os.ui.window.platform.linux.x11.keymap as x11_keymap
use std.os.ui.window.platform.linux.joystick as linux_joystick
use std.core.common as common
use std.os.ui.profile as ui_profile
use std.os.ffi as ffi

#linux {
   #link "libwayland-client.so"
   #include <wayland-client.h>
   #link "libwayland-cursor.so"
   #include <wayland-cursor.h>
   #link "libxkbcommon.so"
   #include <xkbcommon/xkbcommon.h>
   #link "libvulkan.so"
   #include <vulkan/vulkan_wayland.h>
   #include <poll.h>
}

fn _is_debug(): bool { ui_profile.debug_enabled() }

fn _dbg(any: msg): any { if(_is_debug()){ ui_profile.print_text("[wayland] " + msg) } }

fn _dbgu(any: msg): any { if(_is_debug()){ ui_profile.print_text("[wayland:v] " + msg) } }

fn _dbg_err(any: msg): any { if(_is_debug()){ ui_profile.print_text("[wayland:ERROR] " + msg) } }
mut _wayland_globals = 0

fn set_globals(any: g): any {
   "Overrides the process-global Wayland state dictionary."
   _wayland_globals = g
}

def VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR = 1000006000
def POLLIN = 0x0001
def WL_SEAT_CAPABILITY_POINTER = 1
def WL_SEAT_CAPABILITY_KEYBOARD = 2
def WL_SEAT_CAPABILITY_TOUCH = 4
def WL_DISPLAY_GET_REGISTRY = 1
def WL_REGISTRY_BIND = 0
def WL_COMPOSITOR_CREATE_SURFACE = 0
def WL_SEAT_GET_POINTER = 0
def WL_SEAT_GET_KEYBOARD = 1
def WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE = 1
def WL_OUTPUT_MODE_CURRENT = 0x1
def WL_SURFACE_ATTACH = 1
def WL_SURFACE_COMMIT = 6
def XDG_WM_BASE_PONG = 3
def XDG_WM_BASE_GET_XDG_SURFACE = 2
def XDG_SURFACE_GET_TOPLEVEL = 1
def XDG_SURFACE_ACK_CONFIGURE = 4
def XDG_TOPLEVEL_SET_TITLE = 2
def XDG_TOPLEVEL_SET_APP_ID = 3
def XDG_TOPLEVEL_SET_MIN_SIZE = 8
def XDG_TOPLEVEL_SET_MAX_SIZE = 7
def XDG_TOPLEVEL_SET_MAXIMIZED = 9
def XDG_TOPLEVEL_UNSET_MAXIMIZED = 10
def XDG_TOPLEVEL_SET_MINIMIZED = 13
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
def _WG_SCROLL_AXIS_DX = 488
def _WG_SCROLL_AXIS_DY = 492
def _WG_SCROLL_VALUE120_DX = 496
def _WG_SCROLL_VALUE120_DY = 500
def _WG_SCROLL_DISCRETE_DX = 504
def _WG_SCROLL_DISCRETE_DY = 508
def _WG_SCROLL_HAS_AXIS = 512
def _WG_SCROLL_HAS_VALUE120 = 516
def _WG_SCROLL_HAS_DISCRETE = 520
def _WG_SCROLL_AXIS_SOURCE = 524
def _WG_PENDING_EVENTS = 528
def _WG_PENDING_SURFACE = 536
def _WG_PENDING_X = 544
def _WG_PENDING_Y = 548
def _WG_PENDING_BUTTON = 552
def _WG_PENDING_ACTION = 556
def _WG_SIZE = 560
def _WP_PENDING_SURFACE = 1
def _WP_PENDING_MOTION = 2
def _WP_PENDING_BUTTON = 4
def _WP_SET_CURSOR = 0
def _WP_RELEASE = 1
mut _windows = dict(8)
mut _pending_events = []
mut _clipboard_text = ""
mut _clipboard_source = 0
mut _wl_data_source_iface = 0
mut _cursor_theme = 0
mut _cursor_surface = 0
mut _wl_client_lib = 0

comptime table WaylandScancodeKey {
   1 -> KEY_ESCAPE
   2..10 -> KEY_1 + (raw - 2)
   11 -> KEY_0
   12 -> KEY_MINUS
   13 -> KEY_EQUAL
   14 -> KEY_BACKSPACE
   15 -> KEY_TAB
   16 -> KEY_Q
   17 -> KEY_W
   18 -> KEY_E
   19 -> KEY_R
   20 -> KEY_T
   21 -> KEY_Y
   22 -> KEY_U
   23 -> KEY_I
   24 -> KEY_O
   25 -> KEY_P
   26 -> KEY_LEFT_BRACKET
   27 -> KEY_RIGHT_BRACKET
   28 -> KEY_ENTER
   29 -> KEY_LEFT_CONTROL
   30 -> KEY_A
   31 -> KEY_S
   32 -> KEY_D
   33 -> KEY_F
   34 -> KEY_G
   35 -> KEY_H
   36 -> KEY_J
   37 -> KEY_K
   38 -> KEY_L
   39 -> KEY_SEMICOLON
   40 -> KEY_APOSTROPHE
   41 -> KEY_GRAVE_ACCENT
   42 -> KEY_LEFT_SHIFT
   43 -> KEY_BACKSLASH
   44 -> KEY_Z
   45 -> KEY_X
   46 -> KEY_C
   47 -> KEY_V
   48 -> KEY_B
   49 -> KEY_N
   50 -> KEY_M
   51 -> KEY_COMMA
   52 -> KEY_PERIOD
   53 -> KEY_SLASH
   54 -> KEY_RIGHT_SHIFT
   55 -> KEY_KP_MULTIPLY
   56 -> KEY_LEFT_ALT
   57 -> KEY_SPACE
   58 -> KEY_CAPS_LOCK
   59..68 -> KEY_F1 + (raw - 59)
   69 -> KEY_NUM_LOCK
   70 -> KEY_SCROLL_LOCK
   71..73 -> KEY_KP_7 + (raw - 71)
   74 -> KEY_KP_SUBTRACT
   75..77 -> KEY_KP_4 + (raw - 75)
   78 -> KEY_KP_ADD
   79..81 -> KEY_KP_1 + (raw - 79)
   82 -> KEY_KP_0
   83 -> KEY_KP_DECIMAL
   86 -> KEY_WORLD_2
   87 -> KEY_F11
   88 -> KEY_F12
   89 -> KEY_KP_EQUAL
   97 -> KEY_RIGHT_CONTROL
   98 -> KEY_KP_DIVIDE
   99 -> KEY_PRINT_SCREEN
   100 -> KEY_RIGHT_ALT
   102 -> KEY_HOME
   103 -> KEY_UP
   104 -> KEY_PAGE_UP
   105 -> KEY_LEFT
   106 -> KEY_RIGHT
   107 -> KEY_END
   108 -> KEY_DOWN
   109 -> KEY_PAGE_DOWN
   110 -> KEY_INSERT
   111 -> KEY_DELETE
   119 -> KEY_PAUSE
   125 -> KEY_LEFT_SUPER
   126 -> KEY_RIGHT_SUPER
   127 -> KEY_MENU
   183..194 -> KEY_F13 + (raw - 183)
   _ -> default
}

fn _translate_wayland_scancode(any: scancode): int { comptime match WaylandScancodeKey(int(scancode), 0) }

fn _push_event(any: win, int: typ, any: data=0): any {
   if(!win){ return 0 }
   _pending_events = _pending_events.append(ui_event.make_event(typ, win, win.get("handle", 0), data))
}

fn _broadcast_event(int: typ, any: data=0): any {
   def keys = dict_keys(_windows)
   mut i = 0
   def keys_n = keys.len
   while(i < keys_n){
      def win = _windows.get(keys.get(i), 0)
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
def _WL_IFACE_SZ = 40
def _WL_MSG_SZ = 24

fn _build_msgs(list: msgs): any {
   def n = msgs.len
   def ms = malloc(_WL_MSG_SZ * n)
   if(!ms){ return 0 }
   mut i = 0
   while(i < n){
      def m = msgs.get(i)
      def msg = ms + i * _WL_MSG_SZ
      store64_h(msg, cstr(m.get(0)), 0) ;; name
      store64_h(msg, cstr(m.get(1)), 8) ;; signature
      def types_list = m.get(2, 0)
      if(types_list && is_list(types_list)){
         def types_n = types_list.len
         def ts = malloc(8 * types_n)
         if(!ts){ store64_h(msg, 0, 16) } else {
            mut j = 0
            while(j < types_n){
               store64_h(ts, types_list.get(j, 0), j * 8)
               j += 1
            }
            store64_h(msg, ts, 16) ;; types
         }
      } else {
         store64_h(msg, 0, 16)
      }
      i += 1
   }
   ms
}

fn _create_interface(str: name, int: version, any: methods=0, any: events=0): any {
   def iface = malloc(_WL_IFACE_SZ)
   if(!iface){ return 0 }
   memset(iface, 0, _WL_IFACE_SZ)
   store64_h(iface, cstr(name), 0) ;; name
   store32(iface, int(version), 8) ;; version
   if(methods){
      store32(iface, methods.len, 12) ;; method_count
      store64_h(iface, _build_msgs(methods), 16) ;; methods
   }
   if(events){
      store32(iface, events.len, 24) ;; event_count
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
mut _xdg_positioner_interface = 0
mut _xdg_toplevel_interface = 0
mut _xdg_surface_interface = 0
mut _xdg_wm_base_interface = 0
mut _wl_registry_interface_local = 0
mut _wl_compositor_interface_local = 0
mut _wl_surface_interface_local = 0
mut _wl_region_interface_local = 0
mut _wl_shm_interface_local = 0
mut _wl_shm_pool_interface_local = 0
mut _wl_buffer_interface_local = 0
mut _wl_seat_interface_local = 0
mut _wl_pointer_interface_local = 0
mut _wl_keyboard_interface_local = 0
mut _wl_touch_interface_local = 0
mut _wl_output_interface_local = 0
mut _wl_data_device_manager_interface_local = 0
mut _wl_data_device_interface_local = 0
mut _wl_data_offer_interface_local = 0
mut _wl_data_source_interface_local = 0
mut _wl_callback_interface_local = 0
mut _wl_subcompositor_interface_local = 0
mut _wl_subsurface_interface_local = 0

fn _init_core_interfaces_base(): any {
   _wl_callback_interface_local = _create_interface(
      "wl_callback", 1, 0, [["done", "u", 0]],
   )
   _wl_buffer_interface_local = _create_interface(
      "wl_buffer", 1, [["destroy", "", 0]], [["release", "", 0]],
   )
   _wl_touch_interface_local = _create_interface("wl_touch", 1, 0, 0)
   _wl_region_interface_local = _create_interface(
      "wl_region", 7,
      [["destroy", "", 0], ["add", "iiii", 0], ["subtract", "iiii", 0]],
      0,
   )
   _wl_output_interface_local = _create_interface(
      "wl_output", 4, [["release", "", 0]],
      [
         ["geometry", "iiiiissi", 0], ["mode", "uiii", 0], ["done", "", 0],
         ["scale", "i", 0], ["name", "s", 0], ["description", "s", 0],
      ],
   )
   def wl_surface_attach_types = [_wl_buffer_interface_local, 0, 0]
   def wl_surface_frame_types = [_wl_callback_interface_local]
   def wl_surface_region_types = [_wl_region_interface_local]
   def wl_surface_output_types = [_wl_output_interface_local]
   _wl_surface_interface_local = _create_interface("wl_surface", 4,
      [
         ["destroy", "", 0], ["attach", "?oii", wl_surface_attach_types],
         ["damage", "iiii", 0], ["frame", "n", wl_surface_frame_types],
         ["set_opaque_region", "?o", wl_surface_region_types],
         ["set_input_region", "?o", wl_surface_region_types],
         ["commit", "", 0], ["set_buffer_transform", "i", 0],
         ["set_buffer_scale", "i", 0], ["damage_buffer", "iiii", 0],
      ],
      [["enter", "o", wl_surface_output_types], ["leave", "o", wl_surface_output_types]],
   )
   def wl_pointer_set_cursor_types = [0, _wl_surface_interface_local, 0, 0]
   def wl_pointer_enter_types = [0, _wl_surface_interface_local, 0, 0]
   def wl_pointer_leave_types = [0, _wl_surface_interface_local]
   _wl_pointer_interface_local = _create_interface("wl_pointer", 5,
      [["set_cursor", "u?oii", wl_pointer_set_cursor_types], ["release", "", 0]],
      [
         ["enter", "uoff", wl_pointer_enter_types], ["leave", "uo", wl_pointer_leave_types],
         ["motion", "uff", 0], ["button", "uuuu", 0], ["axis", "uuf", 0],
         ["frame", "", 0], ["axis_source", "u", 0], ["axis_stop", "uu", 0],
         ["axis_discrete", "ui", 0],
      ],
   )
   def wl_keyboard_surface_types = [0, _wl_surface_interface_local]
   def wl_keyboard_enter_types = [0, _wl_surface_interface_local, 0]
   _wl_keyboard_interface_local = _create_interface("wl_keyboard", 5,
      [["release", "", 0]],
      [
         ["keymap", "uhu", 0], ["enter", "uoa", wl_keyboard_enter_types],
         ["leave", "uo", wl_keyboard_surface_types], ["key", "uuuu", 0],
         ["modifiers", "uuuuu", 0], ["repeat_info", "ii", 0],
      ],
   )
   _wl_seat_interface_local = _create_interface("wl_seat", 5,
      [
         ["get_pointer", "n", [_wl_pointer_interface_local]],
         ["get_keyboard", "n", [_wl_keyboard_interface_local]],
         ["get_touch", "n", [_wl_touch_interface_local]],
         ["release", "", 0],
      ],
      [["capabilities", "u", 0], ["name", "s", 0]],
   )
}

fn _init_core_interfaces_data(): any {
   _wl_data_offer_interface_local = _create_interface("wl_data_offer", 3,
      [
         ["accept", "u?s", 0], ["receive", "sh", 0], ["destroy", "", 0],
         ["finish", "", 0], ["set_actions", "uu", 0],
      ],
      [["offer", "s", 0], ["source_actions", "u", 0], ["action", "u", 0]],
   )
   _wl_data_source_interface_local = _create_interface("wl_data_source", 3,
      [["offer", "s", 0], ["destroy", "", 0], ["set_actions", "u", 0]],
      [
         ["target", "?s", 0], ["send", "sh", 0], ["cancelled", "", 0],
         ["dnd_drop_performed", "", 0], ["dnd_finished", "", 0], ["action", "u", 0],
      ],
   )
   def wl_data_device_start_drag_types = [
      _wl_data_source_interface_local,
      _wl_surface_interface_local,
      _wl_surface_interface_local,
      0,
   ]
   def wl_data_device_enter_types = [
      0, _wl_surface_interface_local, 0, 0, _wl_data_offer_interface_local,
   ]
   _wl_data_device_interface_local = _create_interface("wl_data_device", 3,
      [
         ["start_drag", "?oo?ou", wl_data_device_start_drag_types],
         ["set_selection", "?ou", [_wl_data_source_interface_local, 0]],
         ["release", "", 0],
      ],
      [
         ["data_offer", "n", [_wl_data_offer_interface_local]],
         ["enter", "uoff?o", wl_data_device_enter_types],
         ["leave", "", 0], ["motion", "uff", 0], ["drop", "", 0],
         ["selection", "?o", [_wl_data_offer_interface_local]],
      ],
   )
   _wl_data_device_manager_interface_local = _create_interface("wl_data_device_manager", 3,
      [
         ["create_data_source", "n", [_wl_data_source_interface_local]],
         ["get_data_device", "no", [_wl_data_device_interface_local, _wl_seat_interface_local]],
      ],
      0,
   )
   _wl_shm_pool_interface_local = _create_interface(
      "wl_shm_pool", 1,
      [
         ["create_buffer", "niiiiu", [_wl_buffer_interface_local, 0, 0, 0, 0, 0]],
         ["destroy", "", 0], ["resize", "i", 0],
      ],
      0,
   )
   _wl_shm_interface_local = _create_interface(
      "wl_shm", 1,
      [["create_pool", "nhi", [_wl_shm_pool_interface_local, 0, 0]]],
      [["format", "u", 0]],
   )
}

fn _init_core_interfaces_shell(): any {
   _wl_subsurface_interface_local = _create_interface("wl_subsurface", 1,
      [
         ["destroy", "", 0], ["set_position", "ii", 0],
         ["place_above", "o", [_wl_surface_interface_local]],
         ["place_below", "o", [_wl_surface_interface_local]],
         ["set_sync", "", 0], ["set_desync", "", 0],
      ],
      0,
   )
   def wl_subcompositor_get_subsurface_types = [
      _wl_subsurface_interface_local, _wl_surface_interface_local, _wl_surface_interface_local,
   ]
   _wl_subcompositor_interface_local = _create_interface(
      "wl_subcompositor", 1,
      [["destroy", "", 0], ["get_subsurface", "noo", wl_subcompositor_get_subsurface_types]],
      0,
   )
   _wl_compositor_interface_local = _create_interface(
      "wl_compositor", 4,
      [["create_surface", "n", [_wl_surface_interface_local]], ["create_region", "n", [_wl_region_interface_local]]],
      0,
   )
   _wl_registry_interface_local = _create_interface(
      "wl_registry", 1,
      [["bind", "usun", [0, 0, 0, 0]]],
      [["global", "usu", 0], ["global_remove", "u", 0]],
   )
}

fn _init_core_interfaces(): any {
   if(_wl_registry_interface_local){ return 0 }
   _init_core_interfaces_base()
   _init_core_interfaces_data()
   _init_core_interfaces_shell()
}

fn _init_xdg_interfaces(): any {
   _init_core_interfaces()
   if(_xdg_wm_base_interface){ return 0 }
   def wl_seat_iface = _interface_symbol("wl_seat_interface")
   def wl_output_iface = _interface_symbol("wl_output_interface")
   def wl_surface_iface = _interface_symbol("wl_surface_interface")
   _xdg_positioner_interface = _create_interface("xdg_positioner", 1,
      [["destroy", "", 0],
         ["set_size", "ii", 0],
         ["set_anchor_rect", "iiii", 0],
         ["set_anchor", "u", 0],
         ["set_gravity", "u", 0],
         ["set_constraint_adjustment", "u", 0],
      ["set_offset", "ii", 0]],
   0)
   _xdg_toplevel_interface = _create_interface("xdg_toplevel", 1,
      [["destroy", "", 0],
         ["set_parent", "?o", [0]],
         ["set_title", "s", 0],
         ["set_app_id", "s", 0],
         ["show_window_menu", "ouii", [wl_seat_iface]],
         ["move", "ou", [wl_seat_iface]],
         ["resize", "ouu", [wl_seat_iface]],
         ["set_max_size", "ii", 0],
         ["set_min_size", "ii", 0],
         ["set_maximized", "", 0],
         ["unset_maximized", "", 0],
         ["set_fullscreen", "?o", [wl_output_iface]],
         ["unset_fullscreen", "", 0],
      ["set_minimized", "", 0]],
      [["configure", "iia", 0],
   ["close", "", 0]])
   _xdg_surface_interface = _create_interface("xdg_surface", 1,
      [["destroy", "", 0],
         ["get_toplevel", "n", [_xdg_toplevel_interface]],
         ["get_popup", "n?oo", [0, _xdg_surface_interface, _xdg_positioner_interface]],
         ["set_window_geometry", "iiii", 0],
      ["ack_configure", "u", 0]],
   [["configure", "u", 0]])
   _xdg_wm_base_interface = _create_interface("xdg_wm_base", 1,
      [["destroy", "", 0],
         ["create_positioner", "n", [_xdg_positioner_interface]],
         ["get_xdg_surface", "no", [_xdg_surface_interface, wl_surface_iface]],
      ["pong", "u", 0]],
   [["ping", "u", 0]])
}

fn _init_unstable_interfaces(): any {
   _init_core_interfaces()
   _init_xdg_interfaces()
   if(_wp_relative_pointer_manager_interface){ return 0 }
   def wl_pointer_iface = _interface_symbol("wl_pointer_interface")
   def wl_region_iface = _interface_symbol("wl_region_interface")
   def wl_surface_iface = _interface_symbol("wl_surface_interface")
   def wl_seat_iface = _interface_symbol("wl_seat_interface")
   _wp_relative_pointer_interface = _create_interface("zwp_relative_pointer_v1", 1,
      [["destroy", "n", 0]],
   [["relative_motion", "uuffff", 0]])
   _wp_relative_pointer_manager_interface = _create_interface("zwp_relative_pointer_manager_v1", 1,
      [["destroy", "n", 0],
   ["get_relative_pointer", "no", [_wp_relative_pointer_interface, wl_pointer_iface]]])
   _wp_locked_pointer_interface = _create_interface("zwp_locked_pointer_v1", 1,
      [["destroy", "n", 0],
         ["set_cursor_position_hint", "ff", 0],
      ["set_region", "?o", [wl_region_iface]]],
      [["locked", "", 0],
   ["unlocked", "", 0]])
   _wp_confined_pointer_interface = _create_interface("zwp_confined_pointer_v1", 1,
      [["destroy", "n", 0],
      ["set_region", "?o", [wl_region_iface]]],
      [["confined", "", 0],
   ["unconfined", "", 0]])
   def lock_pointer_types = [
      _wp_locked_pointer_interface,
      wl_surface_iface,
      wl_pointer_iface,
      wl_region_iface,
      0,
   ]
   def confine_pointer_types = [
      _wp_confined_pointer_interface,
      wl_surface_iface,
      wl_pointer_iface,
      wl_region_iface,
      0,
   ]
   _wp_pointer_constraints_interface = _create_interface("zwp_pointer_constraints_v1", 1,
      [["destroy", "n", 0],
         ["lock_pointer", "noo?ou", lock_pointer_types],
   ["confine_pointer", "noo?ou", confine_pointer_types]])
   _wp_toplevel_decoration_interface = _create_interface("zxdg_toplevel_decoration_v1", 1,
      [["destroy", "n", 0],
         ["set_mode", "u", 0],
      ["unset_mode", "n", 0]],
   [["configure", "u", 0]])
   _wp_decoration_manager_interface = _create_interface("zxdg_decoration_manager_v1", 1,
      [["destroy", "n", 0],
   ["get_toplevel_decoration", "no", [_wp_toplevel_decoration_interface, _xdg_toplevel_interface]]])
   _wp_text_input_interface = _create_interface("zwp_text_input_v3", 1,
      [["destroy", "n", 0],
         ["enable", "", 0],
         ["disable", "", 0],
         ["set_content_type", "uu", 0],
         ["set_cursor_rectangle", "iiii", 0],
         ["set_surrounding_text", "sii", 0],
      ["commit", "", 0]],
      [["enter", "o", [wl_surface_iface]],
         ["leave", "o", [wl_surface_iface]],
         ["preedit_string", "?si", 0],
         ["commit_string", "?s", 0],
         ["delete_surrounding_text", "uu", 0],
   ["done", "u", 0]])
   _wp_text_input_manager_interface = _create_interface("zwp_text_input_manager_v3", 1,
      [["destroy", "n", 0],
      ["get_text_input", "no", [_wp_text_input_interface, wl_seat_iface]]],
   0)
   _wp_viewport_interface = _create_interface("wp_viewport", 1,
      [["destroy", "n", 0],
         ["set_source", "ffff", 0],
      ["set_destination", "ii", 0]],
   0)
   _wp_viewporter_interface = _create_interface("wp_viewporter", 1,
      [["destroy", "n", 0],
      ["get_viewport", "no", [_wp_viewport_interface, wl_surface_iface]]],
   0)
   _wp_fractional_scale_interface = _create_interface("wp_fractional_scale_v1", 1,
      [["destroy", "n", 0]],
   [["preferred_scale", "u", 0]])
   _wp_fractional_scale_manager_interface = _create_interface("wp_fractional_scale_manager_v1", 1,
      [["destroy", "n", 0],
      ["get_fractional_scale", "no", [_wp_fractional_scale_interface, wl_surface_iface]]],
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

comptime template _wl_marshal_flags_wrap0(name){
   fn ${name}(any: proxy, int: opcode, any: interface, int: version, any: flags): any {
      #linux { return wl_proxy_marshal_flags(proxy, opcode, interface, version, flags) }
      #else { return 0 } #endif
   }
}

comptime template _wl_marshal_flags_wrap1(name){
   fn ${name}(any: proxy, int: opcode, any: interface, int: version, any: flags, any: arg0): any {
      #linux { return wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, arg0) }
      #else { return 0 } #endif
   }
}

comptime template _wl_marshal_flags_wrap2(name){
   fn ${name}(any: proxy, int: opcode, any: interface, int: version, any: flags, any: arg0, any: arg1): any {
      #linux { return wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, arg0, arg1) }
      #else { return 0 } #endif
   }
}

comptime template _wl_marshal_flags_wrap3(name){
   fn ${name}(any: proxy, int: opcode, any: interface, int: version, any: flags, any: arg0, any: arg1, any: arg2): any {
      #linux { return wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, arg0, arg1, arg2) }
      #else { return 0 } #endif
   }
}

comptime template _wl_marshal_flags_wrap4(name){
   fn ${name}(any: proxy, int: opcode, any: interface, int: version, any: flags, any: arg0, any: arg1, any: arg2, any: arg3): any {
      #linux { return wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, arg0, arg1, arg2, arg3) }
      #else { return 0 } #endif
   }
}

comptime template _wl_marshal_flags_wrap5(name){
   fn ${name}(any: proxy, int: opcode, any: interface, int: version, any: flags, any: arg0, any: arg1, any: arg2, any: arg3, any: arg4): any {
      #linux { return wl_proxy_marshal_flags(proxy, opcode, interface, version, flags, arg0, arg1, arg2, arg3, arg4) }
      #else { return 0 } #endif
   }
}

comptime emit _wl_marshal_flags_wrap1(wl_proxy_marshal_flags_ptr)
comptime emit _wl_marshal_flags_wrap2(wl_proxy_marshal_flags_ptr_ii)
comptime emit _wl_marshal_flags_wrap2(wl_proxy_marshal_flags_ptr_obj)
comptime emit _wl_marshal_flags_wrap4(wl_proxy_marshal_flags_cursor)
comptime emit _wl_marshal_flags_wrap2(wl_proxy_marshal_flags_rel_ptr)
comptime emit _wl_marshal_flags_wrap5(wl_proxy_marshal_flags_lock_ptr)
comptime emit _wl_marshal_flags_wrap3(wl_proxy_marshal_flags_obj_ii)
comptime emit _wl_marshal_flags_wrap1(wl_proxy_marshal_flags_str)
comptime emit _wl_marshal_flags_wrap2(wl_proxy_marshal_flags_obj_u)
comptime emit _wl_marshal_flags_wrap2(wl_proxy_marshal_flags_s_fd)
comptime emit _wl_marshal_flags_wrap0(wl_proxy_marshal_flags_void)
comptime emit _wl_marshal_flags_wrap1(wl_proxy_marshal_flags_u)
comptime emit _wl_marshal_flags_wrap4(wl_proxy_marshal_flags_registry_bind)

fn _wl_proxy_marshal_new_id0(any: proxy, int: opcode, any: interface): any {
   if(!proxy || !interface){ return 0 }
   wl_proxy_marshal_flags_ptr(proxy, opcode, interface, int(get_proxy_version(proxy)), 0, 0)
}

fn _wl_proxy_marshal_new_id1_obj(any: proxy, int: opcode, any: interface, any: arg0): any {
   if(!proxy || !interface){ return 0 }
   wl_proxy_marshal_flags_ptr_obj(proxy, opcode, interface, int(get_proxy_version(proxy)), 0, 0, arg0)
}

fn _wl_proxy_marshal_void(any: proxy, int: opcode): any {
   if(!proxy){ return 0 }
   wl_proxy_marshal_flags_void(proxy, opcode, 0, int(get_proxy_version(proxy)), 0)
}

fn _wl_proxy_marshal_str(any: proxy, int: opcode, any: s): any {
   if(!proxy){ return 0 }
   wl_proxy_marshal_flags_str(proxy, opcode, 0, int(get_proxy_version(proxy)), 0, s)
}

fn _wl_proxy_marshal_u(any: proxy, int: opcode, int: value): any {
   if(!proxy){ return 0 }
   wl_proxy_marshal_flags_u(proxy, opcode, 0, int(get_proxy_version(proxy)), 0, int(value))
}

fn _wl_display_get_registry(any: display): any {
   if(!display){ return 0 }
   def iface = _interface_symbol("wl_registry_interface")
   if(!iface){ return 0 }
   wl_proxy_marshal_flags_ptr(display, WL_DISPLAY_GET_REGISTRY, iface, int(get_proxy_version(display)), 0, 0)
}

fn _wl_registry_destroy(any: registry): bool {
   if(!registry){ return false }
   #linux { wl_proxy_destroy(registry) }
   true
}

fn _wl_registry_add_listener(any: registry, any: listener, any: data): int {
   if(!registry || !listener){ return -1 }
   #linux { return wl_proxy_add_listener(registry, listener, data) }
   #else { return -1 } #endif
}

fn _wl_registry_bind(any: registry, any: name, any: interface, any: version): any {
   if(!registry || !interface){ return 0 }
   def iface_name = load64_h(interface, 0)
   if(!iface_name){ return 0 }
   wl_proxy_marshal_flags_registry_bind(registry, WL_REGISTRY_BIND, interface, int(version), 0,
   int(name), iface_name, int(version), 0)
}

fn _wl_compositor_create_surface(any: compositor): any {
   if(!compositor){ return 0 }
   def iface = _interface_symbol("wl_surface_interface")
   if(!iface){ return 0 }
   _wl_proxy_marshal_new_id0(compositor, WL_COMPOSITOR_CREATE_SURFACE, iface)
}

fn available(): bool {
   "Returns true when the process appears to be running under Wayland."
   #linux {
      def wd = common.env_trim("WAYLAND_DISPLAY")
      if(wd.len > 0){
         _dbg("available: yes(WAYLAND_DISPLAY=" + wd + ")")
         return true
      }
      _dbg("available: no(WAYLAND_DISPLAY absent/empty)")
   }
   false
}

fn get_backend_name(): str {
   "Identifies this backend entry module."
   "wayland"
}

fn connect_display(any: name=0): any {
   "Connects to a Wayland display."
   if(!available()){ _dbg("connect_display: not available") return 0 }
   mut dpy = 0
   if(name && is_str(name) && name.len > 0){
      def str: display_name = name
      def any: display_name_s = cstr(display_name)
      def ptr: display_name_c = display_name_s
      dpy = wl_display_connect(display_name_c)
   } else {
      dpy = wl_display_connect(0)
   }
   _dbg("connect_display: dpy=" + to_hex(dpy))
   dpy
}

fn disconnect_display(any: display): bool {
   "Disconnects from a Wayland display."
   _dbg("disconnect_display: dpy=" + to_hex(display))
   if(display){ wl_display_disconnect(display) }
   true
}

fn get_registry(any: display): any {
   "Returns the Wayland registry for a connected display."
   if(!display){ return 0 }
   _wl_display_get_registry(display)
}

fn destroy_registry(any: registry): bool {
   "Destroys a Wayland registry object."
   if(!registry){ return false }
   _wl_registry_destroy(registry)
   true
}

comptime template _wl_wrap_display_i(name, doc, call_fn){
   fn ${name}(any: display): int {
      doc
      if(!display){ return -1 }
      call_fn(display)
   }
}

comptime template _wl_wrap_display_ptr(name, doc, call_fn){
   fn ${name}(any: display): any {
      doc
      if(!display){ return 0 }
      call_fn(display)
   }
}

comptime template _wl_wrap_display_bool(name, doc, call_fn){
   fn ${name}(any: display): bool {
      doc
      if(!display){ return false }
      call_fn(display)
      true
   }
}

comptime template _wl_wrap_display_queue_i(name, doc, call_fn){
   fn ${name}(any: display, any: queue): int {
      doc
      if(!display || !queue){ return -1 }
      call_fn(display, queue)
   }
}

comptime template _wl_wrap_proxy_ptr(name, doc, call_fn){
   fn ${name}(any: proxy): any {
      doc
      if(!proxy){ return 0 }
      call_fn(proxy)
   }
}

comptime template _wl_wrap_proxy_bool(name, doc, call_fn){
   fn ${name}(any: proxy): bool {
      doc
      if(!proxy){ return false }
      call_fn(proxy)
      true
   }
}

comptime emit _wl_wrap_display_i(flush, "Flushes pending client requests to the compositor.", wl_display_flush)
comptime emit _wl_wrap_display_i(roundtrip, "Performs a blocking Wayland roundtrip.", wl_display_roundtrip)
comptime emit _wl_wrap_display_i(get_fd, "Returns the Wayland display file descriptor.", wl_display_get_fd)
comptime emit _wl_wrap_display_i(dispatch_pending, "Dispatches already queued Wayland events.", wl_display_dispatch_pending)
comptime emit _wl_wrap_display_i(dispatch, "Dispatches the next available Wayland event.", wl_display_dispatch)
comptime emit _wl_wrap_display_i(prepare_read, "Begin a blocking Wayland read cycle.", wl_display_prepare_read)
comptime emit _wl_wrap_display_i(read_events, "Reads pending Wayland events after a successful prepare-read cycle.", wl_display_read_events)
comptime emit _wl_wrap_display_ptr(create_event_queue, "Creates a dedicated Wayland event queue.", wl_display_create_queue)
comptime emit _wl_wrap_display_bool(cancel_read, "Cancels a previously prepared blocking read.", wl_display_cancel_read)

fn wl_pointer_set_cursor(any: pointer, any: serial, any: surface, any: hotspot_x, any: hotspot_y): bool {
   "Sends wl_pointer.set_cursor(serial, surface, hotspot_x, hotspot_y)."
   if(!pointer){ return false }
   wl_proxy_marshal_flags_ptr_obj(pointer, _WP_SET_CURSOR, 0, wl_proxy_get_version(pointer), 0, surface, 0)
   true
}

fn destroy_event_queue(any: queue): bool {
   "Destroys a previously created Wayland event queue."
   if(!queue){ return false }
   wl_event_queue_destroy(queue)
   true
}

comptime emit _wl_wrap_display_queue_i(prepare_read_queue, "Begins a blocking read cycle for a specific Wayland event queue.", wl_display_prepare_read_queue)
comptime emit _wl_wrap_display_queue_i(dispatch_queue_pending, "Dispatches already queued events for a specific Wayland queue.", wl_display_dispatch_queue_pending)
comptime emit _wl_wrap_proxy_ptr(create_proxy_wrapper, "Creates a queue-local wrapper for a Wayland proxy.", wl_proxy_create_wrapper)
comptime emit _wl_wrap_proxy_ptr(get_proxy_user_data, "Returns the Wayland proxy user-data pointer.", wl_proxy_get_user_data)
comptime emit _wl_wrap_proxy_bool(destroy_proxy, "Destroys a generic Wayland proxy.", wl_proxy_destroy)
comptime emit _wl_wrap_proxy_bool(destroy_proxy_wrapper, "Destroys a Wayland proxy wrapper.", wl_proxy_wrapper_destroy)

comptime template _wl_wrap_proxy_bool2(name, doc, require_arg, call_fn){
   fn ${name}(any: proxy, any: arg0): bool {
      doc
      if(!proxy || (require_arg && !arg0)){ return false }
      call_fn(proxy, arg0)
      true
   }
}

comptime emit _wl_wrap_proxy_bool2(set_proxy_queue, "Assigns a Wayland proxy to an explicit event queue.", true, wl_proxy_set_queue)
comptime emit _wl_wrap_proxy_bool2(set_proxy_user_data, "Sets the Wayland proxy user-data pointer.", false, wl_proxy_set_user_data)

fn get_proxy_version(any: proxy): int {
   "Returns the protocol version advertised by a Wayland proxy."
   if(!proxy){ return 0 }
   int(wl_proxy_get_version(proxy))
}

fn _bind_version(int: advertised, int: max_supported): int {
   if(advertised < 1){ return 1 }
   advertised < max_supported ? advertised : max_supported
}

fn _interface_symbol(str: name): any {
   if(!name || !is_str(name) || name.len == 0){ return 0 }
   _init_core_interfaces()
   if(name == "wl_registry_interface"){ return _wl_registry_interface_local }
   if(name == "wl_compositor_interface"){ return _wl_compositor_interface_local }
   if(name == "wl_surface_interface"){ return _wl_surface_interface_local }
   if(name == "wl_region_interface"){ return _wl_region_interface_local }
   if(name == "wl_shm_interface"){ return _wl_shm_interface_local }
   if(name == "wl_shm_pool_interface"){ return _wl_shm_pool_interface_local }
   if(name == "wl_buffer_interface"){ return _wl_buffer_interface_local }
   if(name == "wl_seat_interface"){ return _wl_seat_interface_local }
   if(name == "wl_pointer_interface"){ return _wl_pointer_interface_local }
   if(name == "wl_keyboard_interface"){ return _wl_keyboard_interface_local }
   if(name == "wl_touch_interface"){ return _wl_touch_interface_local }
   if(name == "wl_output_interface"){ return _wl_output_interface_local }
   if(name == "wl_data_device_manager_interface"){ return _wl_data_device_manager_interface_local }
   if(name == "wl_data_device_interface"){ return _wl_data_device_interface_local }
   if(name == "wl_data_offer_interface"){ return _wl_data_offer_interface_local }
   if(name == "wl_data_source_interface"){ return _wl_data_source_interface_local }
   if(name == "wl_callback_interface"){ return _wl_callback_interface_local }
   if(name == "wl_subcompositor_interface"){ return _wl_subcompositor_interface_local }
   if(name == "wl_subsurface_interface"){ return _wl_subsurface_interface_local }
   if(!_wl_client_lib){
      _wl_client_lib = ffi.dlopen("libwayland-client.so.0", ffi.RTLD_NOW())
      if(!_wl_client_lib){ _wl_client_lib = ffi.dlopen("libwayland-client.so", ffi.RTLD_NOW()) }
   }
   _wl_client_lib ? ffi.dlsym(_wl_client_lib, name) : 0
}

fn _create_seat_pointer(any: seat): any {
   if(!seat){ return 0 }
   def iface = _interface_symbol("wl_pointer_interface")
   if(!iface){ return 0 }
   _wl_proxy_marshal_new_id0(seat, WL_SEAT_GET_POINTER, iface)
}

fn _create_seat_keyboard(any: seat): any {
   if(!seat){ return 0 }
   def iface = _interface_symbol("wl_keyboard_interface")
   if(!iface){ return 0 }
   _wl_proxy_marshal_new_id0(seat, WL_SEAT_GET_KEYBOARD, iface)
}

fn _create_data_device(any: manager, any: seat): any {
   if(!manager || !seat){ return 0 }
   def iface = _interface_symbol("wl_data_device_interface")
   if(!iface){ return 0 }
   _wl_proxy_marshal_new_id1_obj(manager, WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE, iface, seat)
}

fn create_wl_surface(any: compositor): any {
   "Creates a raw `wl_surface` from `wl_compositor`."
   if(!compositor){ return 0 }
   _wl_compositor_create_surface(compositor)
}

fn create_xdg_surface(any: wm_base, any: surface): any {
   "Creates an `xdg_surface` wrapper around a `wl_surface`."
   if(!wm_base || !surface){ return 0 }
   _init_xdg_interfaces()
   def iface = _xdg_surface_interface
   if(!iface){ return 0 }
   _wl_proxy_marshal_new_id1_obj(wm_base, XDG_WM_BASE_GET_XDG_SURFACE, iface, surface)
}

fn create_xdg_toplevel(any: xdg_surface): any {
   "Creates an `xdg_toplevel` object from an `xdg_surface`."
   if(!xdg_surface){ return 0 }
   _init_xdg_interfaces()
   def iface = _xdg_toplevel_interface
   if(!iface){ return 0 }
   _wl_proxy_marshal_new_id0(xdg_surface, XDG_SURFACE_GET_TOPLEVEL, iface)
}

fn xdg_toplevel_set_title(any: toplevel, any: title): any {
   "Sets the title of an `xdg_toplevel` window."
   if(!toplevel){ return 0 }
   def s = cstr(title)
   _wl_proxy_marshal_str(toplevel, XDG_TOPLEVEL_SET_TITLE, s)
}

fn xdg_toplevel_set_app_id(any: toplevel, any: app_id): any {
   "Sets the app_id of an `xdg_toplevel` window."
   if(!toplevel){ return 0 }
   def s = cstr(app_id)
   _wl_proxy_marshal_str(toplevel, XDG_TOPLEVEL_SET_APP_ID, s)
}

fn xdg_toplevel_set_minimized(any: toplevel): any {
   "Requests minimization for an `xdg_toplevel`."
   if(toplevel){ _wl_proxy_marshal_void(toplevel, XDG_TOPLEVEL_SET_MINIMIZED) }
}

fn xdg_toplevel_set_maximized(any: toplevel): any {
   "Requests maximization for an `xdg_toplevel`."
   if(toplevel){ _wl_proxy_marshal_void(toplevel, XDG_TOPLEVEL_SET_MAXIMIZED) }
}

fn xdg_toplevel_unset_maximized(any: toplevel): any {
   "Restores an `xdg_toplevel` from maximized state."
   if(toplevel){ _wl_proxy_marshal_void(toplevel, XDG_TOPLEVEL_UNSET_MAXIMIZED) }
}

fn _surface_handle_enter(any: data, any: surface, any: output): any {
   mut win = _windows.get(data, 0)
   if(!win || !is_dict(win)){ return 0 }
   def outputs = win.get("outputs", [])
   win = win.set("outputs", outputs.append(output))
   _windows = _windows.set(data, win)
}

fn _surface_handle_leave(any: data, any: surface, any: output): any {
   mut win = _windows.get(data, 0)
   if(!win || !is_dict(win)){ return 0 }
   def outputs = win.get("outputs", [])
   mut next_outputs = []
   mut i = 0
   def outputs_n = outputs.len
   while(i < outputs_n){
      def o = outputs.get(i)
      if(o != output){ next_outputs = next_outputs.append(o) }
      i += 1
   }
   win = win.set("outputs", next_outputs)
   _windows = _windows.set(data, win)
}

fn _xdg_surface_handle_configure(any: surface, any: xdg_surface, any: serial): any {
   if(!xdg_surface){ return 0 }
   _dbgu("xdg_surface.configure surface=0x" + to_hex(surface) + " serial=" + to_str(from_int(serial)))
   _wl_proxy_marshal_u(xdg_surface, XDG_SURFACE_ACK_CONFIGURE, int(from_int(serial)))
   mut win = _windows.get(surface, 0)
   if(win){
      def pw, ph = win.get("pending_w", 0), win.get("pending_h", 0)
      win = win.set("configured", true)
      if(pw > 0 && ph > 0 && (pw != win.get("w", 0) || ph != win.get("h", 0))){
         mut next_win = win.set("w", pw)
         next_win = next_win.set("h", ph)
         next_win = next_win.set("configured", true)
         _windows = _windows.set(surface, next_win)
         mut data = dict(8)
         data = data.set("w", pw)
         data = data.set("h", ph)
         _push_event(next_win, EVENT_WINDOW_RESIZED, data)
      } else {
         _windows = _windows.set(surface, win)
      }
   }
}

fn _xdg_toplevel_handle_configure(any: surface, any: toplevel, any: width, any: height, any: states): any {
   mut win = _windows.get(surface, 0)
   if(!win || !is_dict(win)){ return 0 }
   _dbgu("xdg_toplevel.configure surface=0x" + to_hex(surface) +
      " width=" + to_str(int(from_int(width))) +
   " height=" + to_str(int(from_int(height))))
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
   def was_maximized = win.get("maximized", false)
   if(next_maximized != was_maximized){
      win = win.set("maximized", next_maximized)
      _push_event(win, next_maximized ? EVENT_WINDOW_MAXIMIZED : EVENT_WINDOW_RESTORED, 0)
   }
   def was_fullscreen = win.get("fullscreen", false)
   if(next_fullscreen != was_fullscreen){ win = win.set("fullscreen", next_fullscreen) }
   def was_focused = win.get("focused", false)
   if(next_activated != was_focused){
      win = win.set("focused", next_activated)
      _push_event(win, next_activated ? EVENT_FOCUS_IN : EVENT_FOCUS_OUT, 0)
   }
   def cfg_w, cfg_h = int(from_int(width)), int(from_int(height))
   if(cfg_w > 0 && cfg_h > 0){
      win = win.set("pending_w", cfg_w)
      win = win.set("pending_h", cfg_h)
   }
   _windows = _windows.set(surface, win)
}

fn _xdg_toplevel_handle_close(any: win_obj, any: toplevel): any {
   def win = _windows.get(win_obj, 0)
   if(win){ _push_event(win, 15, 0) }
}

fn _decoration_handle_configure(any: win_obj, any: decoration, any: mode): any {
   mut win = _windows.get(win_obj, 0)
   if(win && is_dict(win)){
      win = win.set("decoration_mode", int(from_int(mode)))
      _windows = _windows.set(win_obj, win)
   }
}

fn _create_shell_objects(any: win): any {
   if(!win || !is_dict(win)){ return win }
   def handle = win.get("handle", 0)
   def globals = win.get("globals", 0)
   if(!handle || !globals){ return win }
   def wm_base = globals.get("wm_base", 0)
   if(!wm_base){ return win }
   def xdg_surface = create_xdg_surface(wm_base, handle)
   if(!xdg_surface){ return win }
   def toplevel = create_xdg_toplevel(xdg_surface)
   if(!toplevel){
      destroy_proxy(xdg_surface)
      return win
   }
   def xdg_surface_listener = zalloc(8)
   store64_h(xdg_surface_listener, _xdg_surface_handle_configure, 0)
   wl_proxy_add_listener(xdg_surface, xdg_surface_listener, handle)
   def toplevel_listener = zalloc(16)
   store64_h(toplevel_listener, _xdg_toplevel_handle_configure, 0)
   store64_h(toplevel_listener, _xdg_toplevel_handle_close, 8)
   wl_proxy_add_listener(toplevel, toplevel_listener, handle)
   xdg_toplevel_set_title(toplevel, win.get("title", "Untitled"))
   xdg_toplevel_set_app_id(toplevel, win.get("app_id", "nytrix"))
   def state = globals.get("listener_state", 0)
   def dec_mgr = state ? load64_h(state, _WG_DECORATION_MANAGER) : 0
   if(dec_mgr){
      _init_unstable_interfaces()
      def dec_iface = _wp_toplevel_decoration_interface
      if(dec_iface){
         def decoration = _wl_proxy_marshal_new_id1_obj(dec_mgr, 1, dec_iface, toplevel)
         if(decoration){
            def dec_listener = zalloc(8)
            if(dec_listener){
               store64_h(dec_listener, _decoration_handle_configure, 0)
               wl_proxy_add_listener(decoration, dec_listener, handle)
            }
            _wl_proxy_marshal_u(decoration, 1, 2)
         }
      }
   }
   mut next_win = win.set("xdg_surface", xdg_surface)
   next_win = next_win.set("xdg_toplevel", toplevel)
   next_win
}

fn _destroy_shell_objects(any: win): any {
   if(!win || !is_dict(win)){ return win }
   def xdg_surface = win.get("xdg_surface", 0)
   def toplevel = win.get("xdg_toplevel", 0)
   if(toplevel){ destroy_proxy(toplevel) }
   if(xdg_surface){ destroy_proxy(xdg_surface) }
   mut next_win = win.set("xdg_surface", 0)
   next_win = next_win.set("xdg_toplevel", 0)
   next_win
}

fn create_basic_window(any: globals, str: title, int: width, int: height, str: app_id="nytrix"): any {
   "High-level helper to create a Wayland window with all necessary surface/shell wrappers."
   _dbg("create_basic_window: title=" + title + " size=" + to_str(width) + "x" + to_str(height))
   if(!globals || !is_dict(globals)){ _dbg_err("no globals") return 0 }
   def comp = globals.get("compositor", 0)
   if(!comp){ _dbg_err("no compositor") return 0 }
   _dbg("  compositor=0x" + to_hex(comp))
   def surface = create_wl_surface(comp)
   if(!surface){ _dbg_err("failed to create surface") return 0 }
   _dbg("  surface=0x" + to_hex(surface))
   def wl_surface_listener = zalloc(16)
   store64_h(wl_surface_listener, _surface_handle_enter, 0)
   store64_h(wl_surface_listener, _surface_handle_leave, 8)
   wl_proxy_add_listener(surface, wl_surface_listener, surface)
   mut win = {
      "handle": surface,
      "globals": globals,
      "w": width,
      "h": height,
      "title": title,
      "app_id": app_id,
      "visible": false,
      "configured": false
   }
   win = _create_shell_objects(win)
   win = win.set("visible", true)
   def state_gi = globals.get("listener_state", 0)
   if(state_gi){
      def seat = load64_h(state_gi, _WG_SEAT)
      def ti = _create_text_input(state_gi, seat)
      if(ti){
         def ti_listener = zalloc(48)
         store64_h(ti_listener, _text_input_handle_enter, 0)
         store64_h(ti_listener, _text_input_handle_leave, 8)
         store64_h(ti_listener, _text_input_handle_preedit, 16)
         store64_h(ti_listener, _text_input_handle_commit, 24)
         store64_h(ti_listener, _text_input_handle_delete_surrounding, 32)
         store64_h(ti_listener, _text_input_handle_done, 40)
         wl_proxy_add_listener(ti, ti_listener, surface)
         win = win.set("text_input", ti)
         win = win.set("text_input_listener", ti_listener)
      }
      def fs = _create_fractional_scale(state_gi, surface)
      if(fs){
         def fs_listener = zalloc(8)
         store64_h(fs_listener, _fractional_scale_handle_preferred_scale, 0)
         wl_proxy_add_listener(fs, fs_listener, surface)
         win = win.set("fractional_scale", fs)
         win = win.set("fractional_scale_listener", fs_listener)
      }
   }
   _windows = _windows.set(surface, win)
   _wl_proxy_marshal_void(surface, WL_SURFACE_COMMIT)
   _dbgu("create_basic_window commit surface=0x" + to_hex(surface))
   def display = globals.get("handle", 0)
   if(display){
      _dbgu("create_basic_window roundtrip begin surface=0x" + to_hex(surface))
      wl_display_roundtrip(display)
      _dbgu("create_basic_window roundtrip end surface=0x" + to_hex(surface))
   }
   _windows.get(surface, win)
}

fn _wl_destroy_proxy_slot(dict: win, str: key_proxy, str: key_listener="", str: key_data=""): bool {
   if(!win || !is_dict(win)){ return false }
   def handle = win.get(key_proxy, 0)
   if(!handle){ return false }
   wl_proxy_marshal_flags_ptr(handle, 0, 0, 1, 1, 0) ;; destroy
   destroy_proxy(handle)
   if(key_listener){
      def listener = win.get(key_listener, 0)
      if(listener){ free(listener) }
   }
   if(key_data){
      def data = win.get(key_data, 0)
      if(data){ free(data) }
   }
   true
}

fn destroy_basic_window(any: win): bool {
   "Destroys a Wayland window and its associated surface proxies."
   if(!win || !is_dict(win)){ return false }
   def surface = win.get("handle", 0)
   def xdg_surface = win.get("xdg_surface", 0)
   def toplevel = win.get("xdg_toplevel", 0)
   _wl_destroy_proxy_slot(win, "locked_pointer", "lock_listener", "lock_data")
   _wl_destroy_proxy_slot(win, "relative_pointer", "rel_listener", "rel_data")
   if(toplevel){ destroy_proxy(toplevel) }
   if(xdg_surface){ destroy_proxy(xdg_surface) }
   if(surface){ destroy_proxy(surface) }
   _windows = _windows.set(surface, 0)
   true
}

fn get_size(any: win): list {
   "Returns the Wayland window size as [width, height]."
   if(!win || !is_dict(win)){ return [0, 0] }
   [win.get("w", 0), win.get("h", 0)]
}

fn set_size(any: win, int: w, int: h): bool {
   "Sets the Wayland window size(as a hint to the compositor)."
   if(!win || !is_dict(win)){ return false }
   true
}

fn get_cursor_pos(any: win): list {
   "Returns the current cursor position relative to the Wayland window."
   if(!win || !is_dict(win)){ return [0.0, 0.0] }
   [float(win.get("mouse_x", 0)), float(win.get("mouse_y", 0))]
}

fn get_key_name(any: win, int: key, int: scancode): str {
   "Returns the keyboard-layout specific name for a key using XKB if available."
   if(!win || !is_dict(win)){ return "" }
   def globals = win.get("globals", 0)
   if(globals){
      def state = globals.get("listener_state", 0)
      if(state){
         def xkb_state = load64(state, _WG_XKB_STATE)
         if(xkb_state){
            def sym = xkb_state_key_get_one_sym(xkb_state, int(scancode) + 8)
            if(sym){
               def cp = int(xkb_keysym_to_utf32(sym))
               if(cp > 32 && cp != 127){ return str.chr(cp) }
               def buf = malloc(64)
               if(!buf){ return "" }
               def res = xkb_keysym_get_name(sym, buf, 64)
               mut out = ""
               if(res > 0){ out = str.cstr_to_str(buf) }
               free(buf)
               return out
            }
         }
      }
   }
   if(key >= 32 && key <= 126){ return str.chr(key) }
   ""
}

fn set_window_icon(any: win, any: images): bool {
   "Wayland does not support setting window icons directly."
   false
}

fn get_key_state(any: win, int: key): int {
   "Returns the Wayland key state from the local dictionary."
   if(!win || !is_dict(win)){ return 0 }
   win.get("key_states", dict(8)).get(key, false) ? 1 : 0
}

fn get_mouse_button_state(any: win, int: btn): int {
   "Returns the Wayland mouse button state from the local dictionary."
   if(!win || !is_dict(win)){ return 0 }
   win.get("mouse_button_" + to_str(btn), 0)
}

fn set_cursor_pos(any: win, any: x, any: y): bool {
   "Wayland cursor warping: only available via pointer lock hint when CURSOR_DISABLED."
   if(!win || !is_dict(win)){ return false }
   def locked_ptr = win.get("locked_pointer", 0)
   if(!locked_ptr){ return false }
   wl_proxy_marshal_flags_ptr_ii(locked_ptr, 1, 0, int(get_proxy_version(locked_ptr)), 0,
   int(x * 256), int(y * 256))
   true
}

fn _ensure_cursor_theme(any: globals): any {
   if(_cursor_theme){ return _cursor_theme }
   if(!globals){ return 0 }
   def shm = globals.get("shm", 0)
   if(!shm){ return 0 }
   _cursor_theme = wl_cursor_theme_load(0, 24, shm)
   _cursor_theme
}

fn _ensure_cursor_surface(any: globals): any {
   if(_cursor_surface){ return _cursor_surface }
   if(!globals){ return 0 }
   def comp = globals.get("compositor", 0)
   if(!comp){ return 0 }
   _cursor_surface = _wl_compositor_create_surface(comp)
   _cursor_surface
}

comptime table WaylandCursorShapeNames {
   backend_api.ARROW_CURSOR -> ["default", "left_ptr", "arrow"]
   backend_api.IBEAM_CURSOR -> ["text", "xterm", "ibeam"]
   backend_api.CROSSHAIR_CURSOR -> ["crosshair", "cross"]
   backend_api.POINTING_HAND_CURSOR -> ["pointer", "hand2", "pointing_hand"]
   backend_api.RESIZE_EW_CURSOR -> ["ew-resize", "sb_h_double_arrow", "size_hor"]
   backend_api.RESIZE_NS_CURSOR -> ["ns-resize", "sb_v_double_arrow", "size_ver"]
   backend_api.RESIZE_NWSE_CURSOR -> ["nwse-resize", "top_left_corner", "size_fdiag"]
   backend_api.RESIZE_NESW_CURSOR -> ["nesw-resize", "top_right_corner", "size_bdiag"]
   backend_api.RESIZE_ALL_CURSOR -> ["all-scroll", "fleur", "size_all"]
   backend_api.NOT_ALLOWED_CURSOR -> ["not-allowed", "crossed_circle", "forbidden"]
}

fn _wl_cursor_for_shape(any: theme, int: shape): any {
   if(!theme){ return 0 }
   def names = comptime match WaylandCursorShapeNames(shape, ["default", "left_ptr", "arrow"])
   if(!names){ return 0 }
   mut i = 0
   def names_n = names.len
   while(i < names_n){
      def cur = wl_cursor_theme_get_cursor(theme, cstr(names.get(i)))
      if(cur){ return cur }
      i += 1
   }
   0
}

fn create_cursor(any: image, int: xhot=0, int: yhot=0): any {
   "Custom cursor from image data: stores as dict for deferred upload."
   if(!image){ return 0 }
   return {"type": "custom", "image": image, "xhot": xhot, "yhot": yhot}
}

fn create_standard_cursor(int: shape): dict {
   "Creates a Wayland standard cursor by shape index."
   return {"type": "standard", "shape": shape}
}

fn destroy_cursor(any: cursor): bool {
   "Destroys a cursor dict(theme-owned cursors are freed with the theme)."
   true
}

fn set_cursor(any: win, any: cursor): any {
   "Sets the cursor for a Wayland window using the cursor theme."
   if(!win || !is_dict(win)){ return win }
   def globals = win.get("globals", 0)
   if(!globals){ return win }
   def state = globals.get("listener_state", 0)
   if(!state){ return win }
   def pointer = load64_h(state, _WG_POINTER)
   if(!pointer){ return win }
   def serial = load64_h(state, _WG_POINTER_ENTER_SERIAL)
   if(!cursor){
      wl_proxy_marshal_flags_full_set_cursor(pointer, serial, 0, 0, 0)
      return win
   }
   def theme = _ensure_cursor_theme(globals)
   def surf  = _ensure_cursor_surface(globals)
   if(!theme || !surf){ return win }
   def cur_shape = cursor.get("shape", 0)
   def wl_cur = _wl_cursor_for_shape(theme, cur_shape)
   if(!wl_cur){ return win }
   def image_count = load32(wl_cur, 0)
   if(image_count == 0){ return win }
   def images_ptr = load64_h(wl_cur, 8) ;; ptr to array of wl_cursor_image*
   def img = load64_h(images_ptr, 0) ;; first image
   if(!img){ return win }
   def hx, hy = load32(img, 8), load32(img, 12)
   def buf = wl_cursor_image_get_buffer(img)
   if(!buf){ return win }
   wl_proxy_marshal_flags_obj_ii(surf, 1, 0, int(get_proxy_version(surf)), 0, buf, 0, 0)
   _wl_proxy_marshal_void(surf, WL_SURFACE_COMMIT)
   wl_proxy_marshal_flags_full_set_cursor(pointer, serial, surf, hx, hy)
   win
}

fn _ensure_data_source_iface(): any {
   if(!_wl_data_source_iface){
      _wl_data_source_iface = _create_interface("wl_data_source", 3,
         [["offer", "s", 0], ["destroy", "n", 0], ["set_actions", "u", 0]],
      [["target", "?s", 0], ["send", "sh", 0], ["cancelled", "", 0]])
   }
   _wl_data_source_iface
}

fn _data_source_send(any: data, any: source, any: mime_type, any: fd): any {
   if(!_clipboard_text || !fd){ close(int(fd)) return 0 }
   def n = _clipboard_text.len
   if(n > 0){ write(int(fd), cstr(_clipboard_text), int(n)) }
   close(int(fd))
}

fn _data_source_cancelled(any: data, any: source): any {
   if(source && source == _clipboard_source){
      destroy_proxy(source)
      _clipboard_source = 0
      _clipboard_text = ""
   }
}

fn _data_device_data_offer(any: data, any: device, any: offer): any {
}

fn _data_device_selection(any: data, any: device, any: offer): any {
   def prev = load64_h(data, _WG_CLIPBOARD_OFFER)
   if(prev && prev != offer){ destroy_proxy(prev) }
   store64_h(data, offer, _WG_CLIPBOARD_OFFER)
}

fn _data_device_enter(any: data, any: device, any: serial, any: surface, any: x, any: y, any: offer): any {
   if(!data){ return 0 }
   store64_h(data, offer, _WG_DND_OFFER)
   store64_h(data, surface, _WG_DND_SURFACE)
   def win = _windows.get(surface, 0)
   if(win && is_dict(win)){
      mut ev = dict(8)
      ev = ev.set("x", int(x))
      ev = ev.set("y", int(y))
      _push_event(win, EVENT_DATA_DROP, ev)
   }
}

fn _data_device_leave(any: data, any: device): any {
   if(!data){ return 0 }
   store64_h(data, 0, _WG_DND_OFFER)
   store64_h(data, 0, _WG_DND_SURFACE)
}

fn _data_device_motion(any: data, any: device, any: time, any: x, any: y): any {
   if(!data){ return 0 }
   def surface = load64_h(data, _WG_DND_SURFACE)
   if(!surface){ return 0 }
   def win = _windows.get(surface, 0)
   if(win && is_dict(win)){
      mut ev = dict(8)
      ev = ev.set("x", int(x))
      ev = ev.set("y", int(y))
      _push_event(win, EVENT_DATA_DROP, ev)
   }
}

fn _data_device_drop(any: data, any: device): any {
   if(!data){ return 0 }
   def offer = load64_h(data, _WG_DND_OFFER)
   def surface = load64_h(data, _WG_DND_SURFACE)
   if(!offer || !surface){ return 0 }
   def display = load64_h(data, _WG_DISPLAY)
   def win = _windows.get(surface, 0)
   if(!win || !is_dict(win)){ return 0 }
   def fds = malloc(8)
   if(!fds){ return 0 }
   memset(fds, 0, 8)
   if(pipe(fds) != 0){ free(fds) return 0 }
   def rfd, wfd = int(load32(fds, 0)), int(load32(fds, 4))
   free(fds)
   wl_proxy_marshal_flags_s_fd(offer, 1, 0, int(get_proxy_version(offer)), 0, cstr("text/uri-list"), wfd)
   close(wfd)
   if(display){ wl_display_flush(display) }
   def buf = malloc(4096)
   if(!buf){ close(rfd) return 0 }
   memset(buf, 0, 4096)
   def n = read(rfd, buf, 4095)
   close(rfd)
   if(n > 0){
      def text = to_str(buf)
      mut ev = dict(8)
      ev = ev.set("text", text)
      _push_event(win, EVENT_DATA_DROP, ev)
   }
   free(buf)
   wl_proxy_marshal_flags_ptr(offer, 3, 0, int(get_proxy_version(offer)), 0, 0)
   store64_h(data, 0, _WG_DND_OFFER)
}

fn _install_data_device_listener(any: state): bool {
   if(!state){ return false }
   def device = load64_h(state, _WG_DATA_DEVICE)
   if(!device){ return false }
   if(load64_h(state, _WG_DATA_DEVICE_LISTENER)){ return true }
   def listener = zalloc(48)
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

fn set_clipboard(any: win, any: text): bool {
   "Sets the Wayland clipboard via wl_data_source / wl_data_device."
   if(!win || !is_dict(win)){ return false }
   def globals = win.get("globals", 0)
   if(!globals){ return false }
   def state = globals.get("listener_state", 0)
   if(!state){ return false }
   def manager = load64_h(state, _WG_DATA_DEVICE_MANAGER)
   def device  = load64_h(state, _WG_DATA_DEVICE)
   if(!manager || !device){ return false }
   def display = globals.get("handle", 0)
   if(_clipboard_source){
      destroy_proxy(_clipboard_source)
      _clipboard_source = 0
   }
   _clipboard_text = to_str(text)
   def iface = _ensure_data_source_iface()
   def source = wl_proxy_marshal_flags_ptr(manager, 0, iface, int(get_proxy_version(manager)), 0, 0)
   if(!source){ return false }
   def src_listener = zalloc(24)
   if(!src_listener){ destroy_proxy(source) return false }
   store64_h(src_listener, 0, 0) ;; target (noop)
   store64_h(src_listener, _data_source_send, 8)
   store64_h(src_listener, _data_source_cancelled, 16)
   wl_proxy_add_listener(source, src_listener, 0)
   wl_proxy_marshal_flags_str(source, 0, 0, int(get_proxy_version(source)), 0, cstr("text/plain;charset=utf-8"))
   wl_proxy_marshal_flags_str(source, 0, 0, int(get_proxy_version(source)), 0, cstr("text/plain"))
   wl_proxy_marshal_flags_str(source, 0, 0, int(get_proxy_version(source)), 0, cstr("UTF8_STRING"))
   _install_data_device_listener(state)
   def btn_serial = load64_h(state, _WG_POINTER_BUTTON_SERIAL)
   wl_proxy_marshal_flags_obj_u(device, 1, 0, int(get_proxy_version(device)), 0, source, int(btn_serial))
   _clipboard_source = source
   if(display){ wl_display_flush(display) }
   true
}

fn get_clipboard(any: win): str {
   "Gets the Wayland clipboard via wl_data_offer pipe receive."
   if(_clipboard_source && _clipboard_text.len > 0){ return _clipboard_text }
   if(!win || !is_dict(win)){ return "" }
   def globals = win.get("globals", 0)
   if(!globals){ return "" }
   def state = globals.get("listener_state", 0)
   if(!state){ return "" }
   def display = globals.get("handle", 0)
   _install_data_device_listener(state)
   if(display){ wl_display_roundtrip(display) }
   def offer = load64_h(state, _WG_CLIPBOARD_OFFER)
   if(!offer){ return "" }
   def pipefd = malloc(8)
   if(!pipefd){ return "" }
   if(pipe(pipefd) != 0){ free(pipefd) return "" }
   def rfd, wfd = load32(pipefd, 0), load32(pipefd, 4)
   free(pipefd)
   wl_proxy_marshal_flags_s_fd(
      offer, 1, 0, int(get_proxy_version(offer)),
      0, cstr("text/plain;charset=utf-8"), int(wfd),
   )
   close(wfd)
   if(display){ wl_display_flush(display) }
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

fn get_window_monitor(any: win): any {
   "Returns the first associated output for a Wayland window."
   if(!is_dict(win)){ return 0 }
   def outputs = win.get("outputs", [])
   if(outputs.len == 0){ return 0 }
   def proxy = outputs.get(0)
   def monitors = get_monitors()
   mut i = 0
   def monitors_n = monitors.len
   while(i < monitors_n){
      def m = monitors.get(i)
      if(m.get("handle", 0) == proxy){ return m }
      i += 1
   }
   0
}

fn set_window_monitor(dict: win, any: monitor, int: xpos, int: ypos, int: width, int: height, int: refresh_rate=0): dict {
   "Stub for Wayland window-monitor association."
   win
}

fn show_window(any: win): bool {
   "Shows the Wayland window by creating shell objects if needed."
   _dbg("show_window: visible=" + to_str(win.get("visible", false)))
   if(!win || !is_dict(win)){ _dbg("show_window: invalid win") return false }
   if(win.get("visible", false)){ return true }
   def handle = win.get("handle", 0)
   if(!handle){ _dbg("show_window: no handle") return false }
   mut next_win = _create_shell_objects(win)
   next_win = next_win.set("visible", true)
   _windows = _windows.set(handle, next_win)
   _wl_proxy_marshal_void(handle, WL_SURFACE_COMMIT)
   true
}

fn hide_window(any: win): bool {
   "Hides the Wayland window by destroying shell objects and detaching surface buffer."
   if(!win || !is_dict(win)){ return false }
   if(!win.get("visible", false)){ return true }
   def handle = win.get("handle", 0)
   if(!handle){ return false }
   mut next_win = _destroy_shell_objects(win)
   next_win = next_win.set("visible", false)
   _windows = _windows.set(handle, next_win)
   wl_proxy_marshal_flags_ptr_obj(handle, WL_SURFACE_ATTACH, 0, int(get_proxy_version(handle)), 0, 0, 0)
   _wl_proxy_marshal_void(handle, WL_SURFACE_COMMIT)
   true
}

comptime template _wl_toplevel_action(name, doc, call_fn){
   fn name(any: win): bool {
      doc
      if(!win || !is_dict(win)){ return false }
      def toplevel = win.get("xdg_toplevel", 0)
      if(toplevel){ call_fn(toplevel) }
      true
   }
}

comptime emit _wl_toplevel_action(iconify_window, "Iconify a native Wayland toplevel.", xdg_toplevel_set_minimized)
comptime emit _wl_toplevel_action(maximize_window, "Maximize a native Wayland toplevel.", xdg_toplevel_set_maximized)
comptime emit _wl_toplevel_action(restore_window, "Restore a native Wayland toplevel.", xdg_toplevel_unset_maximized)

fn _set_toplevel_size_limits(any: toplevel, int: min_w, int: min_h, int: max_w, int: max_h): bool {
   if(!toplevel){ return false }
   def ver = int(get_proxy_version(toplevel))
   def minw = (min_w >= 0 && min_h >= 0) ? int(min_w) : 0
   def minh = (min_w >= 0 && min_h >= 0) ? int(min_h) : 0
   def maxw = (max_w >= 0 && max_h >= 0) ? int(max_w) : 0
   def maxh = (max_w >= 0 && max_h >= 0) ? int(max_h) : 0
   wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MIN_SIZE, 0, ver, 0, minw, minh)
   wl_proxy_marshal_flags_ptr_ii(toplevel, XDG_TOPLEVEL_SET_MAX_SIZE, 0, ver, 0, maxw, maxh)
   true
}

fn _window_toplevel(any: win): any {
   if(!win || !is_dict(win)){ return 0 }
   win.get("xdg_toplevel", 0)
}

fn set_title(any: win, str: title): bool {
   "Updates the Wayland platform window title."
   if(!title){ return false }
   def toplevel = _window_toplevel(win)
   if(!toplevel){ return false }
   xdg_toplevel_set_title(toplevel, title)
}

fn set_window_opacity(any: win, f64: opacity): bool {
   "Applies window opacity via Wayland protocols(currently a no-op)."
   false
}

fn set_window_resizable(any: win, bool: enabled): bool {
   "Toggles the resizable state of a Wayland window by setting min/max sizes."
   def toplevel = _window_toplevel(win)
   if(!toplevel){ return false }
   if(!enabled){
      def sz = get_size(win)
      return _set_toplevel_size_limits(toplevel, sz.get(0), sz.get(1), sz.get(0), sz.get(1))
   }
   _set_toplevel_size_limits(toplevel, -1, -1, -1, -1)
}

fn set_window_decorated(any: win, bool: enabled): bool {
   "Requests server-side or no-decoration mode via zxdg_decoration_manager_v1."
   if(!win || !is_dict(win)){ return false }
   def globals = win.get("globals", 0)
   def state = globals ? globals.get("listener_state", 0) : 0
   if(!state){ return false }
   def dec_mgr = load64_h(state, _WG_DECORATION_MANAGER)
   if(!dec_mgr){ return false }
   def toplevel = _window_toplevel(win)
   if(!toplevel){ return false }
   _init_unstable_interfaces()
   def dec_iface = _wp_toplevel_decoration_interface
   if(!dec_iface){ return false }
   def decoration = wl_proxy_marshal_flags_ptr(dec_mgr, 1, dec_iface, int(get_proxy_version(dec_mgr)), 0, toplevel)
   if(!decoration){ return false }
   wl_proxy_marshal_flags_ptr_ii(decoration, 1, 0, int(get_proxy_version(decoration)), 0, enabled ? 2 : 1, 0)
   true
}

fn set_window_floating(any: win, bool: enabled): bool {
   "Stub for Wayland floating state(not directly supported by xdg-shell)."
   false
}

fn set_window_size_limits(any: win, int: min_w, int: min_h, int: max_w, int: max_h): bool {
   "Sets size limits for a Wayland window via xdg_toplevel."
   def toplevel = _window_toplevel(win)
   if(!toplevel){ return false }
   _set_toplevel_size_limits(toplevel, min_w, min_h, max_w, max_h)
}

fn get_window_attrib(any: win, int: attrib): int {
   "Unified getter for Wayland window attributes matching Nytrix constants."
   if(!win || !is_dict(win)){ return 0 }
   match attrib {
      RESIZABLE -> { return win.get("resizable", true) ? 1 : 0 }
      VISIBLE -> { return 1 }
      FOCUSED -> { return win.get("focused", false) ? 1 : 0 }
      MAXIMIZED -> { return win.get("maximized", false) ? 1 : 0 }
      ICONIFIED -> { return 0 }
      FLOATING -> { return 0 }
      _ -> { return 0 }
   }
}

fn _free_cstr_ptr(any: p): any { if(p){ free(p) } }

fn _new_output_state(any: output, any: global_name): any {
   def state = zalloc(_WO_SIZE)
   if(!state){ return 0 }
   store64_h(state, output, _WO_PROXY)
   store64_h(state, global_name, _WO_GLOBAL_NAME)
   store64_h(state, 1, _WO_SCALE)
   state
}

fn _append_output_state(any: state, any: out_state): bool {
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

fn _output_count(any: state): int { state ? load64_h(state, _WG_OUTPUT_COUNT) : 0 }

fn _output_at(any: state, int: index): any {
   if(!state || index < 0 || index >= _output_count(state)){ return 0 }
   def arr = load64_h(state, _WG_OUTPUTS)
   if(!arr){ return 0 }
   load64_h(arr, index * 8)
}

fn _refresh_hz(any: refresh_mhz): int { refresh_mhz > 0 ? int((refresh_mhz + 500) / 1000) : 0 }

fn _store_output_name_if_empty(any: out_state, any: make, any: model): any {
   if(!out_state || load64_h(out_state, _WO_NAME)){ return 0 }
   def mk, md = make ? to_str(make) : "", model ? to_str(model) : ""
   def full = str.strip(mk + " " + md)
   if(full.len > 0){ store64_h(out_state, strdup(cstr(full)), _WO_NAME) }
}

fn _output_handle_geometry(any: data, any: wl_output, any: x, any: y, any: physical_width, any: physical_height, any: subpixel, any: make, any: model, any: transform): any {
   if(!data){ return 0 }
   store64_h(data, from_int(x), _WO_X)
   store64_h(data, from_int(y), _WO_Y)
   store64_h(data, from_int(physical_width), _WO_WIDTH_MM)
   store64_h(data, from_int(physical_height), _WO_HEIGHT_MM)
   _store_output_name_if_empty(data, make, model)
}

fn _output_handle_mode(any: data, any: wl_output, any: flags, any: width, any: height, any: refresh): any {
   if(!data){ return 0 }
   def mode_flags = from_int(flags)
   if(band(mode_flags, WL_OUTPUT_MODE_CURRENT)){
      store64_h(data, from_int(width), _WO_MODE_W)
      store64_h(data, from_int(height), _WO_MODE_H)
      store64_h(data, _refresh_hz(from_int(refresh)), _WO_REFRESH)
   }
}

fn _output_handle_done(any: data, any: wl_output): any {
   if(!data){ return 0 }
   if(load64_h(data, _WO_WIDTH_MM) <= 0 || load64_h(data, _WO_HEIGHT_MM) <= 0){
      def w, h = load64_h(data, _WO_MODE_W), load64_h(data, _WO_MODE_H)
      if(w > 0 && h > 0){
         store64_h(data, int(w * 25.4 / 96.0), _WO_WIDTH_MM)
         store64_h(data, int(h * 25.4 / 96.0), _WO_HEIGHT_MM)
      }
   }
   if(load64_h(data, _WO_ANNOUNCED) == 0){
      store64_h(data, 1, _WO_ANNOUNCED)
      mut ev_data = dict(8)
      ev_data = ev_data.set("output", wl_output)
      _broadcast_event(EVENT_MONITOR_CONNECTED, ev_data)
   }
}

fn _output_handle_scale(any: data, any: wl_output, any: factor): any {
   if(!data){ return 0 }
   def next_scale = int(from_int(factor))
   store64_h(data, next_scale, _WO_SCALE)
   mut keys = dict_keys(_windows)
   mut i = 0
   def keys_n = keys.len
   while(i < keys_n){
      def handle = keys.get(i)
      def win = _windows.get(handle, 0)
      if(is_dict(win)){
         def outputs = win.get("outputs", [])
         mut on_it = false
         mut j = 0
         def outputs_n = outputs.len
         while(j < outputs_n){
            if(outputs.get(j) == wl_output){ on_it = true break }
            j += 1
         }
         if(on_it){ _push_event(win, EVENT_SCALE_UPDATED, next_scale) }
      }
      i += 1
   }
}

fn _output_handle_name(any: data, any: wl_output, any: name): any {
   if(!data){ return 0 }
   _free_cstr_ptr(load64_h(data, _WO_NAME))
   store64_h(data, name ? strdup(name) : 0, _WO_NAME)
}

fn _output_handle_description(any: data, any: wl_output, any: description): any {
   if(!data){ return 0 }
   _free_cstr_ptr(load64_h(data, _WO_DESCRIPTION))
   store64_h(data, description ? strdup(description) : 0, _WO_DESCRIPTION)
}

fn _install_output_listener(any: out_state): bool {
   def any: state_mem = out_state
   if(!state_mem || load64_h(state_mem, _WO_LISTENER)){ return true }
   def listener = zalloc(48)
   if(!listener){ return false }
   store64_h(listener, _output_handle_geometry, 0)
   store64_h(listener, _output_handle_mode, 8)
   store64_h(listener, _output_handle_done, 16)
   store64_h(listener, _output_handle_scale, 24)
   store64_h(listener, _output_handle_name, 32)
   store64_h(listener, _output_handle_description, 40)
   def proxy = load64(state_mem, _WO_PROXY)
   if(wl_proxy_add_listener(proxy, listener, out_state) != 0){
      free(listener)
      return false
   }
   store64_h(state_mem, listener, _WO_LISTENER)
   true
}

fn _find_output_index_by_global_name(any: state, any: global_name): int {
   mut i = 0
   while(i < _output_count(state)){
      def out_state = _output_at(state, i)
      if(out_state && load64_h(out_state, _WO_GLOBAL_NAME) == global_name){ return i }
      i += 1
   }
   -1
}

fn _destroy_output_state(any: out_state): any {
   if(!out_state){ return 0 }
   def proxy = load64_h(out_state, _WO_PROXY)
   def listener = load64_h(out_state, _WO_LISTENER)
   if(proxy){ destroy_proxy(proxy) }
   if(listener){ free(listener) }
   _free_cstr_ptr(load64_h(out_state, _WO_NAME))
   _free_cstr_ptr(load64_h(out_state, _WO_DESCRIPTION))
   free(out_state)
}

fn _remove_output_state(any: state, int: index): bool {
   if(!state || index < 0 || index >= _output_count(state)){ return false }
   def arr = load64_h(state, _WG_OUTPUTS)
   if(!arr){ return false }
   def count = _output_count(state)
   def out_state = load64_h(arr, index * 8)
   if(out_state){
      if(load64_h(out_state, _WO_ANNOUNCED) != 0){
         mut ev_data = dict(8)
         ev_data = ev_data.set("output", load64_h(out_state, _WO_PROXY))
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

fn _output_state_to_dict(any: out_state): any {
   if(!out_state){ return false }
   mut out = dict(8)
   out = out.set("handle", load64_h(out_state, _WO_PROXY))
   out = out.set("global_name", load64_h(out_state, _WO_GLOBAL_NAME))
   out = out.set("name", to_str(load64_h(out_state, _WO_NAME)))
   out = out.set("description", to_str(load64_h(out_state, _WO_DESCRIPTION)))
   out = out.set("x", load64_h(out_state, _WO_X))
   out = out.set("y", load64_h(out_state, _WO_Y))
   out = out.set("width_mm", load64_h(out_state, _WO_WIDTH_MM))
   out = out.set("height_mm", load64_h(out_state, _WO_HEIGHT_MM))
   out = out.set("scale", load64_h(out_state, _WO_SCALE))
   out = out.set("mode_width", load64_h(out_state, _WO_MODE_W))
   out = out.set("mode_height", load64_h(out_state, _WO_MODE_H))
   out = out.set("refresh_rate", load64_h(out_state, _WO_REFRESH))
   out
}

fn _ensure_seat_objects(any: state): any {
   if(!state){ return 0 }
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

fn _pointer_pending(any: data): int {
   if(!data){ return 0 }
   load32(data, _WG_PENDING_EVENTS)
}

fn _pointer_set_pending(any: data, int: bits): any {
   if(!data){ return 0 }
   store32(data, bor(load32(data, _WG_PENDING_EVENTS), bits), _WG_PENDING_EVENTS)
}

fn _pointer_active_surface(any: data): any {
   if(!data){ return 0 }
   def pending = _pointer_pending(data)
   if(band(pending, _WP_PENDING_SURFACE)){
      def surf = load64_h(data, _WG_PENDING_SURFACE)
      if(surf){ return surf }
   }
   load64_h(data, _WG_POINTER_FOCUS)
}

fn _pointer_apply_cursor(any: data, any: pointer, any: serial, any: win): any {
   if(!data || !win || !is_dict(win)){ return 0 }
   def mode = win.get("mode_" + to_str(CURSOR), 0)
   if(mode == CURSOR_HIDDEN || mode == CURSOR_DISABLED){ wl_proxy_marshal_flags_full_set_cursor(pointer, serial, 0, 0, 0) }
}

fn _pointer_push_motion(any: surface, any: x, any: y): bool {
   mut win = _windows.get(surface, 0)
   if(!win){ return false }
   win = win.set("mouse_x", x)
   win = win.set("mouse_y", y)
   _windows = _windows.set(surface, win)
   def ev_data = {"x": x, "y": y, "mod": win.get("modifiers", 0)}
   _push_event(win, EVENT_MOUSE_POS_CHANGED, ev_data)
   true
}

fn _pointer_push_button(any: surface, int: btn, bool: action): bool {
   mut win = _windows.get(surface, 0)
   if(!win){ return false }
   win = win.set("mouse_button_" + to_str(btn), action ? 1 : 0)
   _windows = _windows.set(surface, win)
   def ev_data = {
      "button": btn,
      "x": win.get("mouse_x", 0),
      "y": win.get("mouse_y", 0),
      "mod": win.get("modifiers", 0)
   }
   def kind = action ? EVENT_MOUSE_BUTTON_PRESSED : EVENT_MOUSE_BUTTON_RELEASED
   _push_event(win, kind, ev_data)
   true
}

fn _pointer_handle_enter(any: data, any: pointer, any: serial, any: surface, any: sx, any: sy): any {
   if(!data){ return 0 }
   store64_h(data, from_int(serial), _WG_POINTER_ENTER_SERIAL)
   def win = _windows.get(surface, 0)
   if(!win){ return 0 }
   def fx, fy = float(from_int(sx)) / 256.0, float(from_int(sy)) / 256.0
   _pointer_set_pending(data, bor(_WP_PENDING_SURFACE, _WP_PENDING_MOTION))
   store64_h(data, surface, _WG_PENDING_SURFACE)
   store32_f32(data, fx, _WG_PENDING_X)
   store32_f32(data, fy, _WG_PENDING_Y)
}

fn _pointer_handle_leave(any: data, any: pointer, any: serial, any: surface): any {
   if(!data){ return 0 }
   _pointer_set_pending(data, _WP_PENDING_SURFACE)
   store64_h(data, 0, _WG_PENDING_SURFACE)
}

fn _pointer_handle_motion(any: data, any: pointer, any: time, any: sx, any: sy): any {
   if(!data){ return 0 }
   def surface = _pointer_active_surface(data)
   def win = _windows.get(surface, 0)
   if(!win){ return 0 }
   def fx = float(from_int(sx)) / 256.0 ;; Wayland fixed-point to float
   def fy = float(from_int(sy)) / 256.0
   _pointer_set_pending(data, _WP_PENDING_MOTION)
   store32_f32(data, fx, _WG_PENDING_X)
   store32_f32(data, fy, _WG_PENDING_Y)
}

fn _pointer_handle_button(any: data, any: pointer, any: serial, any: time, any: button, any: state): any {
   if(!data){ return 0 }
   def surface = _pointer_active_surface(data)
   def win = _windows.get(surface, 0)
   if(!win){ return 0 }
   store64_h(data, from_int(serial), _WG_POINTER_BUTTON_SERIAL)
   def btn = int(from_int(button)) - 0x110 ;; BTN_LEFT baseline.
   if(btn < 0){ return 0 }
   _pointer_set_pending(data, _WP_PENDING_BUTTON)
   store32(data, btn, _WG_PENDING_BUTTON)
   store32(data, (from_int(state) == 1) ? 1 : 0, _WG_PENDING_ACTION)
}

fn _pointer_reset_scroll(any: data): any {
   if(!data){ return 0 }
   store32_f32(data, 0.0, _WG_SCROLL_AXIS_DX)
   store32_f32(data, 0.0, _WG_SCROLL_AXIS_DY)
   store32_f32(data, 0.0, _WG_SCROLL_VALUE120_DX)
   store32_f32(data, 0.0, _WG_SCROLL_VALUE120_DY)
   store32_f32(data, 0.0, _WG_SCROLL_DISCRETE_DX)
   store32_f32(data, 0.0, _WG_SCROLL_DISCRETE_DY)
   store32(data, 0, _WG_SCROLL_HAS_AXIS)
   store32(data, 0, _WG_SCROLL_HAS_VALUE120)
   store32(data, 0, _WG_SCROLL_HAS_DISCRETE)
}

fn _pointer_reset_frame(any: data): any {
   if(!data){ return 0 }
   store32(data, 0, _WG_PENDING_EVENTS)
   store64_h(data, 0, _WG_PENDING_SURFACE)
   store32_f32(data, 0.0, _WG_PENDING_X)
   store32_f32(data, 0.0, _WG_PENDING_Y)
   store32(data, 0, _WG_PENDING_BUTTON)
   store32(data, 0, _WG_PENDING_ACTION)
   _pointer_reset_scroll(data)
}

fn _pointer_accum_scroll(any: data, f64: dx, f64: dy, int: dx_off, int: dy_off, int: flag_off): any {
   if(!data){ return 0 }
   store32_f32(data, load32_f32(data, dx_off) + dx, dx_off)
   store32_f32(data, load32_f32(data, dy_off) + dy, dy_off)
   store32(data, 1, flag_off)
}

fn _pointer_flush_scroll(any: data): any {
   if(!data){ return 0 }
   mut dx, dy = 0.0, 0.0
   if(load32(data, _WG_SCROLL_HAS_VALUE120)!= 0){ dx, dy = load32_f32(data, _WG_SCROLL_VALUE120_DX), load32_f32(data, _WG_SCROLL_VALUE120_DY) }
   if(dx == 0.0 && dy == 0.0 && load32(data, _WG_SCROLL_HAS_AXIS)!= 0){ dx, dy = load32_f32(data, _WG_SCROLL_AXIS_DX), load32_f32(data, _WG_SCROLL_AXIS_DY) }
   if(dx == 0.0 && dy == 0.0 && load32(data, _WG_SCROLL_HAS_DISCRETE)!= 0){ dx, dy = load32_f32(data, _WG_SCROLL_DISCRETE_DX), load32_f32(data, _WG_SCROLL_DISCRETE_DY) }
   _pointer_reset_scroll(data)
   if(dx == 0.0 && dy == 0.0){ return 0 }
   def surface = load64_h(data, _WG_POINTER_FOCUS)
   def win = _windows.get(surface, 0)
   if(!win){ return 0 }
   def ev_data = {
      "dx": dx,
      "dy": dy,
      "x": win.get("mouse_x", 0),
      "y": win.get("mouse_y", 0),
      "scrolling": true,
      "mod": win.get("modifiers", 0)
   }
   _push_event(win, EVENT_MOUSE_SCROLL, ev_data)
}

fn _pointer_flush_frame(any: data, any: pointer=0): any {
   if(!data){ return 0 }
   def pending = _pointer_pending(data)
   if(pending == 0 && load32(data, _WG_SCROLL_HAS_AXIS) == 0 &&
      load32(data, _WG_SCROLL_HAS_VALUE120) == 0 &&
      load32(data, _WG_SCROLL_HAS_DISCRETE) == 0){
      return 0
   }
   if(band(pending, _WP_PENDING_SURFACE)){
      def old_surface = load64_h(data, _WG_POINTER_FOCUS)
      def next_surface = load64_h(data, _WG_PENDING_SURFACE)
      if(old_surface && old_surface != next_surface){
         def old_win = _windows.get(old_surface, 0)
         if(old_win){ _push_event(old_win, EVENT_MOUSE_LEAVE, 0) }
      }
      store64_h(data, next_surface, _WG_POINTER_FOCUS)
      if(next_surface && old_surface != next_surface){
         def win = _windows.get(next_surface, 0)
         if(win){
            _pointer_apply_cursor(data, common.value_or(pointer, load64_h(data, _WG_POINTER)), load64_h(data,
               _WG_POINTER_ENTER_SERIAL),
            win)
            _push_event(win, EVENT_MOUSE_ENTER, 0)
         }
      }
   }
   def surface = load64_h(data, _WG_POINTER_FOCUS)
   if(!surface){
      _pointer_reset_frame(data)
      return 0
   }
   if(band(pending, _WP_PENDING_MOTION)){
      _pointer_push_motion(
         surface, load32_f32(data, _WG_PENDING_X), load32_f32(data, _WG_PENDING_Y),
      )
   }
   if(band(pending, _WP_PENDING_BUTTON)){
      _pointer_push_button(
         surface, load32(data, _WG_PENDING_BUTTON),
         load32(data, _WG_PENDING_ACTION) != 0,
      )
   }
   _pointer_flush_scroll(data)
   _pointer_reset_frame(data)
}

fn _pointer_handle_axis(any: data, any: pointer, any: time, any: axis, any: value): any {
   if(!data){ return 0 }
   def raw = float(from_int(value)) / 256.0
   def amag = raw < 0.0 ? (0.0 - raw) : raw
   def scroll_val = (amag > 2.0) ? (raw / 10.0) : raw
   mut dx, dy = 0.0, 0.0
   if(from_int(axis) == 0){ dy = -scroll_val } ;; Vertical axis
   else { dx = -scroll_val } ;; Horizontal axis
   _pointer_accum_scroll(data, dx, dy, _WG_SCROLL_AXIS_DX, _WG_SCROLL_AXIS_DY, _WG_SCROLL_HAS_AXIS)
   if(load64_h(data, _WG_SEAT_VER) <= 5){ _pointer_flush_scroll(data) }
}

fn _pointer_handle_frame(any: data, any: pointer): any { _pointer_flush_frame(data, pointer) }

fn _pointer_handle_axis_source(any: data, any: pointer, any: axis_source): any { if(data){ store32(data, int(from_int(axis_source)), _WG_SCROLL_AXIS_SOURCE) } }

fn _pointer_handle_axis_stop(any: data, any: pointer, any: time, any: axis): any {
}

fn _pointer_handle_axis_discrete(any: data, any: pointer, any: axis, any: discrete): any {
   if(!data){ return 0 }
   mut dx, dy = 0.0, 0.0
   if(from_int(axis) == 0){ dy = -float(from_int(discrete)) }
   else { dx = -float(from_int(discrete)) }
   _pointer_accum_scroll(data, dx, dy, _WG_SCROLL_DISCRETE_DX, _WG_SCROLL_DISCRETE_DY, _WG_SCROLL_HAS_DISCRETE)
   if(load64_h(data, _WG_SEAT_VER) <= 5){ _pointer_flush_scroll(data) }
}

fn _pointer_handle_axis_value120(any: data, any: pointer, any: axis, any: value120): any {
   if(!data){ return 0 }
   def scroll_val = float(from_int(value120)) / 120.0
   mut dx, dy = 0.0, 0.0
   if(from_int(axis) == 0){ dy = -scroll_val }
   else { dx = -scroll_val }
   _pointer_accum_scroll(data, dx, dy, _WG_SCROLL_VALUE120_DX, _WG_SCROLL_VALUE120_DY, _WG_SCROLL_HAS_VALUE120)
   if(load64_h(data, _WG_SEAT_VER) <= 5){ _pointer_flush_scroll(data) }
}

fn _pointer_handle_axis_relative_direction(any: data, any: pointer, any: axis, any: direction): any {
}

fn _relative_pointer_handle_motion(any: data, any: rel_ptr, any: utime_hi, any: utime_lo, any: dx, any: dy, any: dx_unaccel, any: dy_unaccel): any {
   if(!data){ return 0 }
   def surface = load64_h(data, 0)
   mut win = _windows.get(surface, 0)
   if(!win){ return 0 }
   def fdx, fdy = float(from_int(dx)) / 256.0, float(from_int(dy)) / 256.0
   mut mx, my = float(win.get("mouse_x", 0)), float(win.get("mouse_y", 0))
   mx += fdx
   my += fdy
   win = win.set("mouse_x", int(mx))
   win = win.set("mouse_y", int(my))
   _windows = _windows.set(surface, win)
   def ev_data = {
      "x": mx,
      "y": my,
      "dx": fdx,
      "dy": fdy,
      "moved": (fdx != 0.0) || (fdy != 0.0),
      "mod": win.get("modifiers", 0)
   }
   _push_event(win, EVENT_MOUSE_POS_CHANGED, ev_data)
}

fn _locked_pointer_handle_locked(any: data, any: locked_ptr): any {
}

fn _locked_pointer_handle_unlocked(any: data, any: locked_ptr): any {
}

fn _confined_pointer_handle_confined(any: data, any: confined_ptr): any {
}

fn _confined_pointer_handle_unconfined(any: data, any: confined_ptr): any {
}

fn _install_pointer_listener(any: state): bool {
   if(!state){ return false }
   def pointer = load64_h(state, _WG_POINTER)
   if(!pointer || load64_h(state, _WG_POINTER_LISTENER)){ return true }
   def listener = zalloc(88) ;; wl_pointer listener through axis_relative_direction
   if(!listener){ return false }
   store64_h(listener, _pointer_handle_enter, 0)
   store64_h(listener, _pointer_handle_leave, 8)
   store64_h(listener, _pointer_handle_motion, 16)
   store64_h(listener, _pointer_handle_button, 24)
   store64_h(listener, _pointer_handle_axis, 32)
   store64_h(listener, _pointer_handle_frame, 40)
   store64_h(listener, _pointer_handle_axis_source, 48)
   store64_h(listener, _pointer_handle_axis_stop, 56)
   store64_h(listener, _pointer_handle_axis_discrete, 64)
   store64_h(listener, _pointer_handle_axis_value120, 72)
   store64_h(listener, _pointer_handle_axis_relative_direction, 80)
   if(wl_proxy_add_listener(pointer, listener, state) != 0){
      free(listener)
      return false
   }
   store64_h(state, listener, _WG_POINTER_LISTENER)
   true
}

fn _get_xkb_mods(any: data): int {
   if(!data){ return 0 }
   def state = load64(data, _WG_XKB_STATE)
   if(!state){ return 0 }
   mut mods = 0
   def XKB_STATE_MODS_EFFECTIVE = 1
   if(xkb_state_mod_index_is_active(
         state, load32(data, _WG_XKB_MOD_SHIFT), XKB_STATE_MODS_EFFECTIVE,
   )){ mods = bor(mods, MOD_SHIFT) }
   if(xkb_state_mod_index_is_active(
         state, load32(data, _WG_XKB_MOD_CTRL), XKB_STATE_MODS_EFFECTIVE,
   )){ mods = bor(mods, MOD_CONTROL) }
   if(xkb_state_mod_index_is_active(
         state, load32(data, _WG_XKB_MOD_ALT), XKB_STATE_MODS_EFFECTIVE,
   )){ mods = bor(mods, MOD_ALT) }
   if(xkb_state_mod_index_is_active(
         state, load32(data, _WG_XKB_MOD_SUPER), XKB_STATE_MODS_EFFECTIVE,
   )){ mods = bor(mods, MOD_SUPER) }
   if(xkb_state_mod_index_is_active(
         state, load32(data, _WG_XKB_MOD_CAPS), XKB_STATE_MODS_EFFECTIVE,
   )){ mods = bor(mods, MOD_CAPS_LOCK) }
   if(xkb_state_mod_index_is_active(
         state, load32(data, _WG_XKB_MOD_NUM), XKB_STATE_MODS_EFFECTIVE,
   )){ mods = bor(mods, MOD_NUM_LOCK) }
   mods
}

fn _keyboard_handle_keymap(any: data, any: keyboard, any: keymap_format_raw, any: fd, any: size): any {
   def keymap_format = from_int(keymap_format_raw)
   def keymap_fd = int(from_int(fd))
   def keymap_size = int(from_int(size))
   if(!data){ if(keymap_fd >= 0){ close(keymap_fd) } return 0 }
   if(keymap_format != XKB_KEYMAP_FORMAT_TEXT_V1){
      close(keymap_fd)
      return 0
   }
   def PROT_READ = 1
   def MAP_PRIVATE = 2
   def map = mmap(0, keymap_size, PROT_READ, MAP_PRIVATE, keymap_fd, 0)
   if(int(map) == -1){ close(keymap_fd) return 0 }
   def ctx = load64(data, _WG_XKB_CONTEXT)
   if(!ctx){
      def next_ctx = xkb_context_new(0)
      if(next_ctx){ store64(data, next_ctx, _WG_XKB_CONTEXT) } else {
         munmap(map, keymap_size)
         close(keymap_fd)
         return 0
      }
   }
   def old_keymap = load64(data, _WG_XKB_KEYMAP)
   def old_state = load64(data, _WG_XKB_STATE)
   def next_keymap = xkb_keymap_new_from_string(load64(data, _WG_XKB_CONTEXT), map, XKB_KEYMAP_FORMAT_TEXT_V1, 0)
   if(next_keymap){
      def next_state = xkb_state_new(next_keymap)
      if(next_state){
         store64(data, next_keymap, _WG_XKB_KEYMAP)
         store64(data, next_state, _WG_XKB_STATE)
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
   munmap(map, keymap_size)
   close(keymap_fd)
}

fn _keyboard_handle_enter(any: data, any: keyboard, any: serial, any: surface, any: keys): any {
   if(!data){ return 0 }
   store64_h(data, surface, _WG_KEYBOARD_FOCUS)
   mut win = _windows.get(surface, 0)
   if(win && !win.get("focused", false)){
      win = win.set("focused", true)
      _windows = _windows.set(surface, win)
      _push_event(win, EVENT_FOCUS_IN, 0)
   }
}

fn _keyboard_handle_leave(any: data, any: keyboard, any: serial, any: surface): any {
   if(!data){ return 0 }
   store64_h(data, 0, _WG_KEYBOARD_FOCUS)
   mut win = _windows.get(surface, 0)
   if(win){
      def ks_old = win.get("key_states", 0)
      if(ks_old){ win = win.set("key_states", dict(64)) }
      if(win.get("focused", false)){
         win = win.set("focused", false)
         _windows = _windows.set(surface, win)
         _push_event(win, EVENT_FOCUS_OUT, 0)
      } else {
         _windows = _windows.set(surface, win)
      }
   }
}

fn _keyboard_handle_key(any: data, any: keyboard, any: serial, any: time, any: key, any: state): any {
   if(!data){ return 0 }
   def surface = load64_h(data, _WG_KEYBOARD_FOCUS)
   mut win = _windows.get(surface, 0)
   if(!win){ return 0 }
   def scancode = from_int(key)
   def key_state = from_int(state)
   def kind = (key_state == 1) ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED
   mut nk = _translate_wayland_scancode(int(scancode))
   def xkb_state = load64(data, _WG_XKB_STATE)
   if(nk == 0 && xkb_state){
      def sym = xkb_state_key_get_one_sym(xkb_state, int(scancode + 8))
      nk = x11_keymap.translate_keysym(sym)
   }
   mut ks = win.get("key_states", dict(64))
   if(nk != 0){ ks = ks.set(nk, (key_state == 1)) }
   win = win.set("key_states", ks)
   _windows = _windows.set(surface, win)
   def mods = _get_xkb_mods(data)
   def ev_data = {
      "key": nk,
      "scancode": int(scancode),
      "action": (key_state == 1) ? backend_api.ACTION_PRESS : backend_api.ACTION_RELEASE,
      "mod": mods,
      "mods": mods
   }
   _push_event(win, kind, ev_data)
   def rate = load64_h(data, _WG_REPEAT_RATE)
   if(rate > 0){
      if(key_state == 1){
         def ts = malloc(16)
         if(!ts){ return 0 }
         memset(ts, 0, 16)
         __clock_gettime(1, ts)
         def sec = load64_h(ts, 0)
         def nsec = load64_h(ts, 8)
         def now_ns = from_int(sec) * 1000000000 + from_int(nsec)
         free(ts)
         store64_h(data, nk, _WG_REPEAT_KEY)
         store64_h(data, scancode, _WG_REPEAT_SCANCODE)
         store64_h(data, now_ns, _WG_REPEAT_START_NS)
         store64_h(data, now_ns, _WG_REPEAT_LAST_NS)
      } elif(key_state == 0){
         if(load64_h(data, _WG_REPEAT_KEY) == nk){ store64_h(data, 0, _WG_REPEAT_KEY) }
      }
   }
   if(key_state == 1 && xkb_state){
      def utf8_buf = malloc(8)
      if(!utf8_buf){ return 0 }
      memset(utf8_buf, 0, 8)
      def nbytes = xkb_state_key_get_utf8(xkb_state, int(scancode + 8), utf8_buf, 8)
      if(nbytes > 0){
         def b0 = load8(utf8_buf, 0)
         if(b0 >= 0x20 && b0 != 0x7f){
            mut cp = 0
            if(b0 < 0x80){ cp = b0 }
            elif(band(b0, 0xe0) == 0xc0 && nbytes >= 2){
               cp = bor(
                  bshl(band(b0, 0x1f), 6),
                  band(load8(utf8_buf, 1), 0x3f),
               )
            } elif(band(b0, 0xf0) == 0xe0 && nbytes >= 3){
               cp = bor(bshl(band(b0, 0x0f), 12),
               bor(bshl(band(load8(utf8_buf, 1), 0x3f), 6), band(load8(utf8_buf, 2), 0x3f)))
            } elif(band(b0, 0xf8) == 0xf0 && nbytes >= 4){
               cp = bor(bshl(band(b0, 0x07), 18),
                  bor(bshl(band(load8(utf8_buf, 1), 0x3f), 12),
               bor(bshl(band(load8(utf8_buf, 2), 0x3f), 6), band(load8(utf8_buf, 3), 0x3f))))
            }
            if(cp > 0){
               mut char_data = dict(8)
               char_data = char_data.set("char", cp)
               def mods = _get_xkb_mods(data)
               char_data = char_data.set("mod", mods)
               char_data = char_data.set("mods", mods)
               _push_event(win, EVENT_KEY_CHAR, char_data)
            }
         }
      }
      free(utf8_buf)
   }
}

fn _synthesize_key_repeat(any: win, any: state): any {
   if(!state){ return 0 }
   def repeat_key = load64_h(state, _WG_REPEAT_KEY)
   if(!repeat_key){ return 0 }
   def rate = load64_h(state, _WG_REPEAT_RATE)
   if(!rate){ return 0 }
   def delay_ms = load64_h(state, _WG_REPEAT_DELAY)
   def ts = malloc(16)
   if(!ts){ return 0 }
   memset(ts, 0, 16)
   __clock_gettime(1, ts)
   def sec = load64_h(ts, 0)
   def nsec = load64_h(ts, 8)
   def now_ns = from_int(sec) * 1000000000 + from_int(nsec)
   free(ts)
   def start_ns = load64_h(state, _WG_REPEAT_START_NS)
   def last_ns = load64_h(state, _WG_REPEAT_LAST_NS)
   def elapsed_ms = (now_ns - start_ns) / 1000000
   if(elapsed_ms < from_int(delay_ms)){ return 0 }
   def interval_ns = 1000000000 / from_int(rate)
   mut fire_ns = last_ns + interval_ns
   def scancode = load64_h(state, _WG_REPEAT_SCANCODE)
   def mods = _get_xkb_mods(state)
   def ev_data = {
      "key": from_int(repeat_key),
      "scancode": int(scancode),
      "action": backend_api.ACTION_REPEAT,
      "mod": mods,
      "mods": mods
   }
   while(fire_ns <= now_ns){
      _push_event(win, EVENT_KEY_PRESSED, ev_data)
      store64_h(state, fire_ns, _WG_REPEAT_LAST_NS)
      fire_ns = fire_ns + interval_ns
   }
}

fn poll_window_events(any: win, int: max_events=64): list {
   "Drains pending events for the given window from the shared backend queue."
   if(!win || !is_dict(win)){ return [win, []] }
   win = poll_joystick_events(win)
   def handle = win.get("handle", 0)
   def globals = win.get("globals", 0)
   def state = globals ? globals.get("listener_state", 0) : 0
   if(state){
      _pointer_flush_frame(state)
      _synthesize_key_repeat(win, state)
   }
   mut out = []
   mut remaining = []
   mut i, n = 0, _pending_events.len
   mut count = 0
   while(i < n){
      def ev = _pending_events.get(i)
      def ev_win = ev.get("window", 0)
      def match_win = (ev_win == win) || (is_dict(ev_win) && ev_win.get("handle", 0) == handle)
      if(match_win && count < max_events){
         out = out.append(ev)
         count += 1
      } else {
         remaining = remaining.append(ev)
      }
      i += 1
   }
   _pending_events = remaining
   def latest_win = _windows.get(handle, win)
   _dbgu("poll_window_events: " + to_str(count) + " events dispatched, " + to_str(remaining.len) + " still pending")
   [latest_win, out]
}

fn _keyboard_handle_modifiers(any: data, any: keyboard, any: serial, any: mods_depres, any: mods_latched, any: mods_locked, any: group): any {
   if(!data){ return 0 }
   def depressed = from_int(mods_depres)
   def latched = from_int(mods_latched)
   def locked = from_int(mods_locked)
   def layout_group = from_int(group)
   store64_h(data, depressed, _WG_KEYBOARD_MODS)
   def state = load64(data, _WG_XKB_STATE)
   if(state){ xkb_state_update_mask(state, depressed, latched, locked, 0, 0, layout_group) }
}

fn _keyboard_handle_repeat_info(any: data, any: keyboard, any: rate, any: delay): any {
   if(!data){ return 0 }
   store64_h(data, from_int(rate), _WG_REPEAT_RATE)
   store64_h(data, from_int(delay), _WG_REPEAT_DELAY)
}

fn _install_keyboard_listener(any: state): bool {
   if(!state){ return false }
   def keyboard = load64_h(state, _WG_KEYBOARD)
   if(!keyboard || load64_h(state, _WG_KEYBOARD_LISTENER)){ return true }
   def listener = zalloc(48)
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

fn _seat_handle_capabilities(any: data, any: seat, any: capabilities): any {
   if(!data){ return 0 }
   def caps = from_int(capabilities)
   store64_h(data, caps, _WG_SEAT_CAPS)
   if(!band(caps, WL_SEAT_CAPABILITY_POINTER)){
      def pointer = load64_h(data, _WG_POINTER)
      if(pointer){
         destroy_proxy(pointer)
         store64_h(data, 0, _WG_POINTER)
      }
   }
   if(!band(caps, WL_SEAT_CAPABILITY_KEYBOARD)){
      def keyboard = load64_h(data, _WG_KEYBOARD)
      if(keyboard){
         destroy_proxy(keyboard)
         store64_h(data, 0, _WG_KEYBOARD)
      }
   }
   _ensure_seat_objects(data)
}

fn _seat_handle_name(any: data, any: seat, any: name): any {
   if(!data){ return 0 }
   def old = load64_h(data, _WG_SEAT_NAME)
   if(old){ free(old) }
   if(name){ store64_h(data, strdup(name), _WG_SEAT_NAME) } else { store64_h(data, 0, _WG_SEAT_NAME) }
}

fn _install_seat_listener(any: state): bool {
   if(!state){ return false }
   def seat = load64_h(state, _WG_SEAT)
   if(!seat){ return false }
   if(load64_h(state, _WG_SEAT_LISTENER)){ return true }
   def listener = zalloc(16)
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

fn _xdg_wm_base_handle_ping(any: data, any: wm_base, any: serial): any {
   if(!wm_base){ return 0 }
   _wl_proxy_marshal_u(wm_base, XDG_WM_BASE_PONG, int(from_int(serial)))
}

fn _install_wm_base_listener(any: state): bool {
   if(!state){ return false }
   def wm_base = load64_h(state, _WG_WM_BASE)
   if(!wm_base){ return false }
   if(load64_h(state, _WG_WM_BASE_LISTENER)){ return true }
   def listener = zalloc(8)
   if(!listener){ return false }
   store64_h(listener, _xdg_wm_base_handle_ping, 0)
   if(wl_proxy_add_listener(wm_base, listener, state) != 0){
      free(listener)
      return false
   }
   store64_h(state, listener, _WG_WM_BASE_LISTENER)
   true
}

fn _bind_registry_iface_once(
   any: data,
   any: registry,
   str: iface,
   str: want_iface,
   int: global_name,
   int: global_version,
   any: symbol,
   int: slot_off,
   int: ver_off=-1,
   int: max_supported=1,
): bool {
   if(iface != want_iface || load64_h(data, slot_off) != 0 || !symbol){ return false }
   def bind_ver = _bind_version(global_version, max_supported)
   def proxy = _wl_registry_bind(registry, global_name, symbol, bind_ver)
   if(proxy){
      store64_h(data, proxy, slot_off)
      if(ver_off >= 0){ store64_h(data, bind_ver, ver_off) }
   }
   true
}

fn _registry_handle_global(any: data, any: registry, any: name, any: iface_cstr, any: version): any {
   if(!data || !registry || !iface_cstr){ return 0 }
   def iface = str.cstr_to_str(iface_cstr)
   def global_name = from_int(name)
   def global_version = from_int(version)
   _dbgu("registry global name=" + to_str(global_name) + " iface=" + iface + " version=" + to_str(global_version))
   if(_bind_registry_iface_once(
         data, registry, iface, "wl_compositor",
         global_name, global_version, _interface_symbol("wl_compositor_interface"),
         _WG_COMPOSITOR, _WG_COMPOSITOR_VER, 4,
   )){ return 0 }
   if(_bind_registry_iface_once(
         data, registry, iface, "wl_subcompositor",
         global_name, global_version, _interface_symbol("wl_subcompositor_interface"),
         _WG_SUBCOMPOSITOR, _WG_SUBCOMPOSITOR_VER, 1,
   )){ return 0 }
   if(_bind_registry_iface_once(
         data, registry, iface, "wl_shm",
         global_name, global_version, _interface_symbol("wl_shm_interface"),
         _WG_SHM, _WG_SHM_VER, 1,
   )){ return 0 }
   if(_bind_registry_iface_once(
         data, registry, iface, "wl_seat",
         global_name, global_version, _interface_symbol("wl_seat_interface"),
         _WG_SEAT, _WG_SEAT_VER, 5,
      )){
      _install_seat_listener(data)
      return 0
   }
   if(_bind_registry_iface_once(
         data, registry, iface, "wl_data_device_manager",
         global_name, global_version, _interface_symbol("wl_data_device_manager_interface"),
         _WG_DATA_DEVICE_MANAGER, _WG_DATA_DEVICE_MANAGER_VER, 3,
      )){
      _ensure_seat_objects(data)
      return 0
   }
   if(iface == "wl_output"){
      def sym = _interface_symbol("wl_output_interface")
      if(!sym || global_version < 2){ return 0 }
      def bind_ver = _bind_version(global_version, 4)
      def proxy = _wl_registry_bind(registry, global_name, sym, bind_ver)
      if(!proxy){ return 0 }
      def out_state = _new_output_state(proxy, global_name)
      if(!out_state){
         destroy_proxy(proxy)
         return 0
      }
      if(!_install_output_listener(out_state) ||
         !_append_output_state(data, out_state)){
         _destroy_output_state(out_state)
      }
      return 0
   }
   if(iface == "xdg_wm_base" && load64_h(data, _WG_WM_BASE) == 0){
      _init_xdg_interfaces()
      if(_bind_registry_iface_once(
            data, registry, iface, "xdg_wm_base",
            global_name, global_version, _xdg_wm_base_interface,
            _WG_WM_BASE, _WG_WM_BASE_VER, 1,
      )){ _install_wm_base_listener(data) }
      return 0
   }
   if(iface == "zwp_relative_pointer_manager_v1" && load64_h(data, _WG_RELATIVE_POINTER_MANAGER) == 0){
      _init_unstable_interfaces()
      _bind_registry_iface_once(
         data, registry, iface, "zwp_relative_pointer_manager_v1",
         global_name, global_version, _wp_relative_pointer_manager_interface,
         _WG_RELATIVE_POINTER_MANAGER, _WG_RELATIVE_POINTER_MANAGER_VER, 1,
      )
      return 0
   }
   if(iface == "zwp_pointer_constraints_v1" && load64_h(data, _WG_POINTER_CONSTRAINTS) == 0){
      _init_unstable_interfaces()
      _bind_registry_iface_once(
         data, registry, iface, "zwp_pointer_constraints_v1",
         global_name, global_version, _wp_pointer_constraints_interface,
         _WG_POINTER_CONSTRAINTS, _WG_POINTER_CONSTRAINTS_VER, 1,
      )
      return 0
   }
   if(iface == "zxdg_decoration_manager_v1" && load64_h(data, _WG_DECORATION_MANAGER) == 0){
      _init_unstable_interfaces()
      _bind_registry_iface_once(
         data, registry, iface, "zxdg_decoration_manager_v1",
         global_name, global_version, _wp_decoration_manager_interface,
         _WG_DECORATION_MANAGER, _WG_DECORATION_MANAGER_VER, 1,
      )
      return 0
   }
   if(iface == "zwp_text_input_manager_v3" && load64_h(data, _WG_TEXT_INPUT_MANAGER) == 0){
      _init_unstable_interfaces()
      _bind_registry_iface_once(
         data, registry, iface, "zwp_text_input_manager_v3",
         global_name, global_version, _wp_text_input_manager_interface,
         _WG_TEXT_INPUT_MANAGER, -1, 1,
      )
      return 0
   }
   if(iface == "wp_viewporter" && load64_h(data, _WG_VIEWPORTER) == 0){
      _init_unstable_interfaces()
      _bind_registry_iface_once(
         data, registry, iface, "wp_viewporter",
         global_name, global_version, _wp_viewporter_interface,
         _WG_VIEWPORTER, -1, 1,
      )
      return 0
   }
   if(iface == "wp_fractional_scale_manager_v1" && load64_h(data, _WG_FRACTIONAL_SCALE_MANAGER) == 0){
      _init_unstable_interfaces()
      _bind_registry_iface_once(
         data, registry, iface, "wp_fractional_scale_manager_v1",
         global_name, global_version, _wp_fractional_scale_manager_interface,
         _WG_FRACTIONAL_SCALE_MANAGER, -1, 1,
      )
      return 0
   }
}

fn _registry_handle_global_remove(any: data, any: registry, any: name): any {
   if(!data){ return 0 }
   def index = _find_output_index_by_global_name(data, from_int(name))
   if(index >= 0){ _remove_output_state(data, index) }
}

fn bootstrap_globals(any: display): any {
   "Bootstrap core Wayland globals from the registry."
   _dbg("bootstrap_globals: display=" + to_hex(display))
   if(!display){ _dbg("bootstrap_globals: no display") return false }
   def registry = get_registry(display)
   if(!registry){ _dbg("bootstrap_globals: no registry") return false }
   def state = zalloc(_WG_SIZE)
   if(!state){
      _dbg("bootstrap_globals: calloc failed")
      destroy_registry(registry)
      return false
   }
   def listener = zalloc(16)
   if(!listener){
      free(state)
      destroy_registry(registry)
      return false
   }
   store64_h(state, display, _WG_DISPLAY)
   store64_h(listener, _registry_handle_global, 0)
   store64_h(listener, _registry_handle_global_remove, 8)
   if(_wl_registry_add_listener(registry, listener, state) != 0){
      free(listener, state)
      destroy_registry(registry)
      return false
   }
   def rr1, rr2 = roundtrip(display), roundtrip(display)
   mut out = dict(8)
   out = out.set("handle", display)
   out = out.set("registry", registry)
   out = out.set("listener", listener)
   out = out.set("listener_state", state)
   out = out.set("roundtrip_1", rr1)
   out = out.set("roundtrip_2", rr2)
   out = out.set("compositor", load64_h(state, _WG_COMPOSITOR))
   out = out.set("subcompositor", load64_h(state, _WG_SUBCOMPOSITOR))
   out = out.set("shm", load64_h(state, _WG_SHM))
   out = out.set("seat", load64_h(state, _WG_SEAT))
   out = out.set("pointer", load64_h(state, _WG_POINTER))
   out = out.set("keyboard", load64_h(state, _WG_KEYBOARD))
   out = out.set("data_device", load64_h(state, _WG_DATA_DEVICE))
   out = out.set("data_device_manager", load64_h(state, _WG_DATA_DEVICE_MANAGER))
   out = out.set("seat_caps", load64_h(state, _WG_SEAT_CAPS))
   out = out.set("seat_name_ptr", load64_h(state, _WG_SEAT_NAME))
   out = out.set("seat_name", to_str(load64_h(state, _WG_SEAT_NAME)))
   out = out.set("seat_listener", load64_h(state, _WG_SEAT_LISTENER))
   out = out.set("output_count", load64_h(state, _WG_OUTPUT_COUNT))
   mut outputs = []
   mut oi = 0
   while(oi < _output_count(state)){
      outputs = outputs.append(_output_state_to_dict(_output_at(state, oi)))
      oi += 1
   }
   out = out.set("outputs", outputs)
   out = out.set("wm_base", load64_h(state, _WG_WM_BASE))
   out = out.set("wm_base_version", load64_h(state, _WG_WM_BASE_VER))
   out = out.set("wm_base_listener", load64_h(state, _WG_WM_BASE_LISTENER))
   out = out.set("compositor_version", load64_h(state, _WG_COMPOSITOR_VER))
   out = out.set("subcompositor_version", load64_h(state, _WG_SUBCOMPOSITOR_VER))
   out = out.set("shm_version", load64_h(state, _WG_SHM_VER))
   out = out.set("seat_version", load64_h(state, _WG_SEAT_VER))
   out = out.set("data_device_manager_version", load64_h(state, _WG_DATA_DEVICE_MANAGER_VER))
   out = out.set("relative_pointer_manager", load64_h(state, _WG_RELATIVE_POINTER_MANAGER))
   out = out.set("pointer_constraints", load64_h(state, _WG_POINTER_CONSTRAINTS))
   out = out.set("decoration_manager", load64_h(state, _WG_DECORATION_MANAGER))
   out
}

fn destroy_globals(any: globals): bool {
   "Destroys the bootstrapped Wayland globals tracked by `bootstrap_globals`."
   if(!globals || !is_dict(globals)){ return false }
   def pointer = globals.get("pointer", 0)
   def keyboard = globals.get("keyboard", 0)
   def device = globals.get("data_device", 0)
   def ddm = globals.get("data_device_manager", 0)
   def seat = globals.get("seat", 0)
   def shm = globals.get("shm", 0)
   def sub = globals.get("subcompositor", 0)
   def comp = globals.get("compositor", 0)
   def wm_base = globals.get("wm_base", 0)
   def rel_ptr_mgr = globals.get("relative_pointer_manager", 0)
   def ptr_constraints = globals.get("pointer_constraints", 0)
   def registry = globals.get("registry", 0)
   def seat_name = globals.get("seat_name_ptr", 0)
   def listener = globals.get("listener", 0)
   def seat_listener = globals.get("seat_listener", 0)
   def wm_base_listener = globals.get("wm_base_listener", 0)
   def state = globals.get("listener_state", 0)
   if(state){
      while(_output_count(state) > 0){ _remove_output_state(state, _output_count(state) - 1) }
      def out_arr = load64_h(state, _WG_OUTPUTS)
      if(out_arr){ free(out_arr) }
      def xkb_state = load64(state, _WG_XKB_STATE)
      def xkb_keymap = load64(state, _WG_XKB_KEYMAP)
      def xkb_ctx = load64(state, _WG_XKB_CONTEXT)
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

fn wait_events(any: display, int: timeout_ms=-1): int {
   "Block until Wayland events arrive or timeout elapses."
   if(!display){ return -1 }
   def pending0 = dispatch_pending(display)
   if(pending0 > 0){ return pending0 }
   mut attempts = 0
   while(prepare_read(display) != 0){
      if(dispatch_pending(display) > 0){ return 1 }
      attempts += 1
      if(timeout_ms == 0 && attempts >= 2){ return 0 }
      if(attempts >= 64){ return 0 }
   }
   wl_display_flush(display)
   def fds = zalloc(8)
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

fn wait_events_queue(any: display, any: queue, int: timeout_ms=-1): int {
   "Block on a specific Wayland event queue."
   if(!display || !queue){ return -1 }
   def pending0 = dispatch_queue_pending(display, queue)
   if(pending0 > 0){ return pending0 }
   mut attempts = 0
   while(prepare_read_queue(display, queue) != 0){
      if(dispatch_queue_pending(display, queue) > 0){ return 1 }
      attempts += 1
      if(timeout_ms == 0 && attempts >= 2){ return 0 }
      if(attempts >= 64){ return 0 }
   }
   wl_display_flush(display)
   def fds = zalloc(8)
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

fn probe_display(any: name=0): bool {
   "Checks that a Wayland display can actually be opened."
   def display = connect_display(name)
   if(!display){ return false }
   wl_display_roundtrip(display)
   wl_display_disconnect(display)
   true
}

fn vulkan_supported(): bool {
   "Returns true if the Wayland backend supports Vulkan."
   true
}

mut _wayland_vk_ext_ptrs = 0
mut _wayland_vk_ext_surface = 0
mut _wayland_vk_ext_wayland = 0

fn vulkan_required_extensions(): list {
   "Returns the Vulkan instance extensions required for a Wayland surface."
   if(!_wayland_vk_ext_ptrs){
      _wayland_vk_ext_surface = cstr("VK_KHR_surface")
      _wayland_vk_ext_wayland = cstr("VK_KHR_wayland_surface")
      def arr = malloc(16)
      if(!arr){ return [0, 0] }
      store64_h(arr, _wayland_vk_ext_surface, 0)
      store64_h(arr, _wayland_vk_ext_wayland, 8)
      _wayland_vk_ext_ptrs = [2, arr]
   }
   _wayland_vk_ext_ptrs
}

fn vulkan_get_presentation_support(any: device, int: queuefamily, any: display): bool {
   "Returns true when the queue family can present to Wayland."
   vkGetPhysicalDeviceWaylandPresentationSupportKHR(
      device, queuefamily, display,
   ) != 0
}

fn create_surface(any: instance, any: win, any: allocator, any: surface_out): int {
   "Creates a Vulkan Wayland surface for the given backend window."
   if(!is_dict(win)){ return -1 }
   def globals = win.get("globals", _wayland_globals)
   def wl_display = globals.get("display", globals.get("handle", 0))
   def wl_surface = win.get("handle", 0)
   if(!wl_display || !wl_surface){ return -1 }
   def info = malloc(40)
   if(!info){ return -1 }
   memset(info, 0, 40)
   store32(info, VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, 0)
   store32(info, 0, 16)
   store64_h(info, wl_display, 24)
   store64_h(info, wl_surface, 32)
   store32(surface_out, 0, 0)
   store32(surface_out, 0, 4)
   def res = vk_create_wayland_surface_khr(instance, info, allocator, surface_out)
   free(info)
   if(res == 0 && load64(surface_out, 0) == 0){
      if(_is_debug()){
         _dbg_err(
            "create_surface: vkCreateWaylandSurfaceKHR returned success but surface handle is null",
         )
      }
      return -1
   }
   res
}

fn get_gamma_ramp(any: monitor): bool {
   "Stub for Wayland gamma support(currently unsupported)."
   false
}

fn set_gamma_ramp(any: monitor, any: ramp): bool {
   "Stub for Wayland gamma support(currently unsupported)."
   false
}

fn get_monitors(): list {
   "Returns list of Wayland output dictionaries as monitor handles."
   if(!_wayland_globals){ return [] }
   mut state = _wayland_globals
   if(is_dict(state)){ state = state.get("listener_state", 0) }
   if(!state){ return [] }
   def count = _output_count(state)
   mut out = []
   mut i = 0 while(i < count){
      def out_state = _output_at(state, i)
      if(out_state){
         def d = _output_state_to_dict(out_state)
         if(d){ out = out.append(d) }
      }
      i += 1
   }
   out
}

fn get_primary_monitor(): any {
   "Returns the first available Wayland output as the primary monitor."
   def all = get_monitors()
   if(all.len > 0){ return all.get(0) }
   0
}

comptime template _wl_monitor_get2(name, doc, k0, d0, k1, d1){
   fn name(any: monitor): list {
      doc
      if(!is_dict(monitor)){ return [d0, d1] }
      [monitor.get(k0, d0), monitor.get(k1, d1)]
   }
}

comptime template _wl_monitor_get4(name, doc, k0, d0, k1, d1, k2, d2, k3, d3){
   fn name(any: monitor): list {
      doc
      if(!is_dict(monitor)){ return [d0, d1, d2, d3] }
      [monitor.get(k0, d0), monitor.get(k1, d1), monitor.get(k2, d2), monitor.get(k3, d3)]
   }
}

comptime template _wl_monitor_get1(name, doc, key, defv, fallback){
   fn name(any: monitor): any {
      doc
      if(!is_dict(monitor)){ return fallback }
      monitor.get(key, defv)
   }
}

comptime emit _wl_monitor_get2(get_monitor_pos,
   "Returns [x, y] of a Wayland output.",
"x", 0, "y", 0)
comptime emit _wl_monitor_get4(get_monitor_workarea,
   "Returns [x, y, w, h] of a Wayland output.",
"x", 0, "y", 0, "w", 0, "h", 0)
comptime emit _wl_monitor_get2(get_monitor_physical_size,
   "Returns [width_mm, height_mm] of a Wayland output.",
"physical_w", 0, "physical_h", 0)

fn get_monitor_content_scale(any: monitor): list {
   "Returns [xscale, yscale] of a Wayland output."
   if(!is_dict(monitor)){ return [1.0, 1.0] }
   def scale = float(monitor.get("scale", 1))
   [scale, scale]
}

comptime emit _wl_monitor_get1(get_monitor_name,
   "Returns the human-readable name of a Wayland output.",
"name", "", "")

fn get_video_mode(any: monitor): any {
   "Returns current video mode(w, h, refresh) for a Wayland output."
   if(!is_dict(monitor)){ return 0 }
   return {"w": monitor.get("w", 0), "h": monitor.get("h", 0), "refresh": monitor.get("refresh", 0)}
}

fn get_video_modes(any: monitor): list {
   "Wayland currently reports only the current/active mode."
   def cur = get_video_mode(monitor)
   if(cur){ return [cur] }
   []
}

fn set_input_mode(any: win, int: mode, int: value): dict {
   "Updates the input mode for a Wayland window and applies it if possible."
   if(!available() || !win || !is_dict(win)){ return win }
   mut next_win = win
   if(mode == CURSOR){
      def globals = win.get("globals", 0)
      if(!globals){ return win }
      def surface = win.get("handle", 0)
      def pointer = globals.get("pointer", 0)
      if(value == CURSOR_CAPTURED){
         def constraints = globals.get("pointer_constraints", 0)
         if(constraints && pointer){
            _init_unstable_interfaces()
            def conf_listener = zalloc(16)
            store64_h(conf_listener, _confined_pointer_handle_confined, 0)
            store64_h(conf_listener, _confined_pointer_handle_unconfined, 8)
            def conf_data = zalloc(8)
            store64_h(conf_data, surface, 0)
            def confined_ptr = wl_proxy_marshal_flags_lock_ptr(constraints,
               2,
               _wp_confined_pointer_interface,
               1,
               1,
               0,
               surface,
               pointer,
               0,
            2)
            if(confined_ptr){
               wl_proxy_add_listener(confined_ptr, conf_listener, conf_data)
               next_win = next_win.set("confined_pointer", confined_ptr)
               next_win = next_win.set("conf_listener", conf_listener)
               next_win = next_win.set("conf_data", conf_data)
            }
         }
      } elif(value == CURSOR_DISABLED){
         def constraints = globals.get("pointer_constraints", 0)
         def rel_mgr = globals.get("relative_pointer_manager", 0)
         if(constraints && rel_mgr && pointer){
            _init_unstable_interfaces()
            def lock_listener = zalloc(16)
            store64_h(lock_listener, _locked_pointer_handle_locked, 0)
            store64_h(lock_listener, _locked_pointer_handle_unlocked, 8)
            def lock_data = zalloc(8)
            store64_h(lock_data, surface, 0)
            def locked_ptr = wl_proxy_marshal_flags_lock_ptr(constraints,
               1,
               _wp_locked_pointer_interface,
               1,
               1,
               0,
               surface,
               pointer,
               0,
            2)
            if(locked_ptr){
               wl_proxy_add_listener(locked_ptr, lock_listener, lock_data)
               next_win = next_win.set("locked_pointer", locked_ptr)
               next_win = next_win.set("lock_listener", lock_listener)
               next_win = next_win.set("lock_data", lock_data)
            }
            def rel_listener = zalloc(8)
            store64_h(rel_listener, _relative_pointer_handle_motion, 0)
            def rel_data = zalloc(8)
            store64_h(rel_data, surface, 0)
            def relative_ptr = wl_proxy_marshal_flags_rel_ptr(rel_mgr,
               1,
               _wp_relative_pointer_interface,
               1,
               1,
               0,
            pointer)
            if(relative_ptr){
               wl_proxy_add_listener(relative_ptr, rel_listener, rel_data)
               next_win = next_win.set("relative_pointer", relative_ptr)
               next_win = next_win.set("rel_listener", rel_listener)
               next_win = next_win.set("rel_data", rel_data)
            }
         }
         def state = globals.get("listener_state", 0)
         if(pointer && state){
            def serial = load64_h(state, _WG_POINTER_ENTER_SERIAL)
            wl_proxy_marshal_flags_ptr_obj(pointer, _WP_SET_CURSOR, 0, wl_proxy_get_version(pointer), 0, 0, 0)
            wl_proxy_marshal_flags_full_set_cursor(pointer, int(serial), 0, 0, 0)
         }
      } else {
         if(_wl_destroy_proxy_slot(win, "locked_pointer", "lock_listener", "lock_data")){ next_win = next_win.set("locked_pointer", 0) }
         if(_wl_destroy_proxy_slot(win, "confined_pointer", "conf_listener", "conf_data")){ next_win = next_win.set("confined_pointer", 0) }
         if(_wl_destroy_proxy_slot(win, "relative_pointer", "rel_listener", "rel_data")){ next_win = next_win.set("relative_pointer", 0) }
         def state = globals.get("listener_state", 0)
         if(pointer && state){
            if(value == CURSOR_HIDDEN){
               def serial = load64_h(state, _WG_POINTER_ENTER_SERIAL)
               wl_proxy_marshal_flags_ptr_obj(pointer, _WP_SET_CURSOR, 0, wl_proxy_get_version(pointer), 0, 0, 0)
               wl_proxy_marshal_flags_full_set_cursor(pointer, int(serial), 0, 0, 0)
            } else {
            }
         }
      }
   }
   next_win.set("mode_" + mode, value)
}

fn get_input_mode(any: win, int: mode): int {
   "Queries the current input mode for the given native Wayland window."
   if(!win || !is_dict(win)){ return 0 }
   win.get("mode_" + mode, 0)
}

fn get_key_scancode(any: win, int: key): int {
   "Wayland has no stable hardware scancode mapping; returns -1."
   -1
}

fn wl_proxy_marshal_flags_full_set_cursor(any: pointer, int: serial, any: surface, int: x, int: y): any {
   "Low-level cursor marshal helper for pointer enter/hidden-mode paths."
   if(!pointer){ return 0 }
   wl_proxy_marshal_flags_cursor(
      pointer, _WP_SET_CURSOR, 0, wl_proxy_get_version(pointer),
      0, serial, surface, int(x), int(y),
   )
}

fn _text_input_handle_commit(any: data, any: text_input, any: text): any {
   if(!data || !text){ return 0 }
   def win = _windows.get(data, 0)
   if(!win || !is_dict(win)){ return 0 }
   mut i = 0
   while(true){
      def b0 = load8(text, i)
      if(b0 == 0){ break }
      mut cp = -1
      mut next = i + 1
      if(b0 < 0x80){ cp = b0 } elif(band(b0, 0xe0) == 0xc0 && load8(text, i + 1) != 0){
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
         mut char_data = dict(8)
         char_data = char_data.set("char", cp)
         char_data = char_data.set("mod", 0)
         char_data = char_data.set("mods", 0)
         _push_event(win, EVENT_KEY_CHAR, char_data)
      }
      i = next
   }
}

fn _text_input_handle_preedit(any: data, any: text_input, any: text, any: cursor_begin, any: cursor_end): any {
}

fn _text_input_handle_delete_surrounding(any: data, any: text_input, any: before_length, any: after_length): any {
}

fn _text_input_handle_done(any: data, any: text_input, any: serial): any {
}

fn _text_input_handle_enter(any: data, any: text_input, any: surface): any {
}

fn _text_input_handle_leave(any: data, any: text_input, any: surface): any {
}

fn _fractional_scale_handle_preferred_scale(any: data, any: fs_obj, any: scale_120): any {
   if(!data){ return 0 }
   mut win = _windows.get(data, 0)
   if(!win || !is_dict(win)){ return 0 }
   def scale = float(from_int(scale_120)) / 120.0
   win = win.set("content_scale", scale)
   _windows = _windows.set(data, win)
}

fn _create_text_input(any: state, any: seat): any {
   def mgr = load64_h(state, _WG_TEXT_INPUT_MANAGER)
   if(!mgr){ return 0 }
   _init_unstable_interfaces()
   if(!_wp_text_input_interface){ return 0 }
   wl_proxy_marshal_flags_ptr_obj(mgr, 1, _wp_text_input_interface, int(get_proxy_version(mgr)), 0, 0, seat)
}

fn _create_fractional_scale(any: state, any: surface): any {
   def mgr = load64_h(state, _WG_FRACTIONAL_SCALE_MANAGER)
   if(!mgr){ return 0 }
   _init_unstable_interfaces()
   if(!_wp_fractional_scale_interface){ return 0 }
   wl_proxy_marshal_flags_ptr_obj(mgr, 1, _wp_fractional_scale_interface, int(get_proxy_version(mgr)), 0, 0, surface)
}

fn text_input_enable(any: win): bool {
   "Enables zwp_text_input_v3 for IME on the given Wayland window."
   if(!win || !is_dict(win)){ return false }
   def ti = win.get("text_input", 0)
   if(!ti){ return false }
   wl_proxy_marshal_flags_ptr(ti, 1, 0, int(get_proxy_version(ti)), 0, 0)
   wl_proxy_marshal_flags_ptr(ti, 6, 0, int(get_proxy_version(ti)), 0, 0)
   true
}

fn text_input_disable(any: win): bool {
   "Disables zwp_text_input_v3 for IME on the given Wayland window."
   if(!win || !is_dict(win)){ return false }
   def ti = win.get("text_input", 0)
   if(!ti){ return false }
   wl_proxy_marshal_flags_ptr(ti, 2, 0, int(get_proxy_version(ti)), 0, 0)
   wl_proxy_marshal_flags_ptr(ti, 6, 0, int(get_proxy_version(ti)), 0, 0)
   true
}

fn get_window_content_scale(any: win): list {
   "Returns [sx, sy] content scale for a Wayland window(from wp_fractional_scale_v1 or 1.0)."
   if(!win || !is_dict(win)){ return [1.0, 1.0] }
   def scale = float(win.get("content_scale", 1.0))
   [scale, scale]
}

fn get_window_opacity(any: win): f64 {
   "Wayland backend opacity querying is unsupported; returns 1.0."
   1.0
}

fn get_window_frame_size(any: win): list {
   "Wayland cannot reliably expose frame extents; returns zero margins."
   [0, 0, 0, 0]
}

fn request_window_attention(any: win): bool {
   "Best-effort attention request; currently a no-op that returns true."
   true
}

fn get_content_scale(any: win): list {
   "Compatibility alias for `get_window_content_scale`."
   get_window_content_scale(win)
}

fn get_seat_capabilities(any: win): dict {
   "Returns a dict with keys pointer/keyboard/touch indicating seat capabilities."
   if(!win || !is_dict(win)){ return dict(8) }
   def globals = win.get("globals", 0)
   if(!globals){ return dict(8) }
   def state = globals.get("listener_state", 0)
   def caps = state ? int(load64_h(state, _WG_SEAT_CAPS)) : 0
   return {
      "pointer": !!band(caps, WL_SEAT_CAPABILITY_POINTER),
      "keyboard": !!band(caps, WL_SEAT_CAPABILITY_KEYBOARD),
      "touch": !!band(caps, WL_SEAT_CAPABILITY_TOUCH)
   }
}

fn poll_joystick_events(any: win): any {
   "Polls evdev joystick events and returns updated window. Requires linux_joystick module."
   if(!win || !is_dict(win)){ return win }
   #linux {
      linux_joystick.poll_joysticks()
   }
   win
}

fn get_wayland_monitor(any: mon): any {
   "Returns the raw Wayland output handle from a monitor dict."
   mon.get("handle", 0)
}
