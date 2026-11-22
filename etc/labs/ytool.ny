#!/usr/bin/env ny
;; Keywords: wayland x11 desktop input automation mouse keyboard windows cli
;; ytool - one input-automation utility tool for Wayland/X11.
;;
;; Combines the X11 window automation command set with a dependency-free
;; Wayland seat driver: virtual keyboard, virtual pointer and input method over the
;; Wayland wire protocol, plus XTest/Xlib window automation on X11.
;;
;;   ytool probe | env | selftest | run SCRIPT
;;   ytool type TEXT | key SEQ | move X Y | click X Y   (auto backend)
;;   ytool wayland <cmd>   force Wayland
;;   ytool x11 <cmd>       force the X11 command set (xdotool-style)

#link "libX11.so"
#link "libX11.so.6"
#link "libXtst.so"
#link "libc.so"

#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XTest.h>

use core
use core.os (msleep, waitpid, file_read, file_exists, file_write, temp_dir)
use core.common as common
use core.str as sx
use core.mem as mem
use core.os.time (now_ms)
use core.os.subprocess (run_capture)

extern "c" {
   fn fork() i32
   fn posix_open(ptr path, i32 flags) i32 as "open"
   fn posix_dup2(i32 oldfd, i32 newfd) i32 as "dup2"
   fn posix_close(i32 fd) i32 as "close"
   fn c_system(ptr command) i32 as "system"
   fn xt_xintern_atom(ptr display, ptr name, i32 only_if_exists) u64 as "XInternAtom"
   fn xt_xget_window_property(ptr display, u64 window, u64 property,
                              i64 offset, i64 length, i32 delete,
                              u64 requested_type, ptr actual_type,
                              ptr actual_format, ptr item_count,
                              ptr bytes_after, ptr data_out) i32 as "XGetWindowProperty"
   fn xt_xfree(ptr data) i32 as "XFree"
   fn xt_xquery_tree(ptr display, u64 window, ptr root_out, ptr parent_out,
                     ptr children_out, ptr child_count_out) i32 as "XQueryTree"
   fn xt_xdefault_screen(ptr display) i32 as "XDefaultScreen"
   fn xt_xroot_window(ptr display, i32 screen) u64 as "XRootWindow"
   fn xt_xfetch_name(ptr display, u64 window, ptr name_out) i32 as "XFetchName"
   fn xt_xget_window_attributes(ptr display, u64 window, ptr attributes) i32 as "XGetWindowAttributes"
   fn xt_xtranslate_coordinates(ptr display, u64 source, u64 destination,
                                i32 source_x, i32 source_y,
                                ptr destination_x, ptr destination_y,
                                ptr child_out) i32 as "XTranslateCoordinates"
   fn xt_xget_class_hint(ptr display, u64 window, ptr hint) i32 as "XGetClassHint"
   fn xt_xget_atom_name(ptr display, u64 atom) ptr as "XGetAtomName"
   fn xt_xsend_event(ptr display, u64 window, i32 propagate, i64 event_mask,
                     ptr event) i32 as "XSendEvent"
   fn xt_xset_input_focus(ptr display, u64 focus, i32 revert_to, u64 timestamp) i32 as "XSetInputFocus"
   fn xt_xkill_client(ptr display, u64 resource) i32 as "XKillClient"
   fn xt_xmove_window(ptr display, u64 window, i32 x, i32 y) i32 as "XMoveWindow"
   fn xt_xresize_window(ptr display, u64 window, i32 width, i32 height) i32 as "XResizeWindow"
   fn xt_xmap_window(ptr display, u64 window) i32 as "XMapWindow"
   fn xt_xunmap_window(ptr display, u64 window) i32 as "XUnmapWindow"
   fn xt_xraise_window(ptr display, u64 window) i32 as "XRaiseWindow"
   fn xt_xlower_window(ptr display, u64 window) i32 as "XLowerWindow"
   fn xt_xiconify_window(ptr display, u64 window, i32 screen) i32 as "XIconifyWindow"
   fn xt_strnlen(ptr text, u64 maxlen) u64 as "strnlen"
}

mut display = nil
mut root = nil

fn parse_int(str s) int {
   mut start = 0
   mut negative = false
   if(s.len > 0 && ord(s.get(0)) == 45) {
      start = 1
      negative = true
   }
   mut base = 10
   if(s.len > start + 1 && ord(s.get(start)) == 48 &&
      (ord(s.get(start + 1)) == 120 || ord(s.get(start + 1)) == 88)) {
      base = 16
      start += 2
   }
   mut value = 0
   mut i = start
   while(i < s.len) {
      def c = ord(s.get(i))
      mut digit = 0
      if(c >= 48 && c <= 57) { digit = c - 48 }
      elif(c >= 65 && c <= 70) { digit = c - 55 }
      elif(c >= 97 && c <= 102) { digit = c - 87 }
      value = value * base + digit
      i += 1
   }
   negative ? 0 - value : value
}

fn to_hex(int n) str {
   if(n == 0) { return "0" }
   def hex_chars = "0123456789abcdef"
   mut val = n
   mut out = ""
   while(val > 0) {
      out = str(hex_chars.get(val & 15)) + out
      val = val >> 4
   }
   out
}

fn atom_val(str name) u64 {
   xt_xintern_atom(display, cstr(name), 0)
}

fn raw_window_id(any value) int {
   int(value)
}

fn has_wm() bool {
   def a = atom_val("_NET_SUPPORTING_WM_CHECK")
   def prop = zalloc(8)
   def act = zalloc(8)
   def fmt = zalloc(4)
   def nitems = zalloc(8)
   def after = zalloc(8)
   def ok = xt_xget_window_property(display, root, a, 0, 1, 0, 33, act, fmt, nitems, after, prop)
   mut found = false
   if(ok == 0) {
      def p = load64(prop, 0)
      if(p) {
         def win = load64(p, 0)
         if(win != 0) {
            def tree = get_top_windows()
            mut in_tree = false
            mut j = 0
            while(j < tree.len) {
               if(int(tree.get(j, 0)) == int(win)) { in_tree = true }
               if(in_tree) { break }
               j += 1
            }
            found = in_tree
         }
         xt_xfree(p)
      }
   }
   free(prop, act, fmt, nitems, after)
   found
}

fn mouse_pos() list {
   def root_ret = zalloc(8)
   def child_ret = zalloc(8)
   def root_x = zalloc(4)
   def root_y = zalloc(4)
   def win_x = zalloc(4)
   def win_y = zalloc(4)
   def mask = zalloc(4)
   XQueryPointer(display, root, root_ret, child_ret, root_x, root_y, win_x, win_y, mask)
   def x = load32(root_x, 0)
   def y = load32(root_y, 0)
   free(root_ret, child_ret, root_x, root_y, win_x, win_y, mask)
   [x, y]
}

fn active_window_ptr() int {
   def prop = zalloc(8)
   def act = zalloc(8)
   def fmt = zalloc(4)
   def nitems = zalloc(8)
   def after = zalloc(8)
   def a = atom_val("_NET_ACTIVE_WINDOW")
   def ok = xt_xget_window_property(display, root, a, 0, 1, 0, 33, act, fmt, nitems, after, prop)
   def win = load64(prop, 0)
   if(ok == 0 && win) {
      def val = load64(win, 0)
      xt_xfree(win)
      free(prop, act, fmt, nitems, after)
      return val
   }
   free(prop, act, fmt, nitems, after)
   0
}

fn read_cstr(any ptr, int maxlen) str {
   def len = int(xt_strnlen(int(ptr), maxlen))
   if(len == 0) { return "Unknown" }
   def buf = malloc(len + 1)
   if(!buf) { return "Unknown" }
   memcpy(buf, int(ptr), len)
   store8(buf, 0, len)
   def s = init_str(buf, len)
   s
}

fn load_i32(any ptr, int offset) int {
   def value = int(load32(ptr, offset))
   if(value >= 2147483648) { return value - 4294967296 }
   value
}

fn win_title(int win) str {
   def net_wm = atom_val("_NET_WM_NAME")
   def utf8 = atom_val("UTF8_STRING")
   def prop = zalloc(8)
   def act = zalloc(8)
   def fmt = zalloc(4)
   def nitems = zalloc(8)
   def after = zalloc(8)
   def ok = xt_xget_window_property(display, win, net_wm, 0, 1000, 0, utf8, act, fmt, nitems, after, prop)
   def data = load64(prop, 0)
   mut title = ""
   if(ok == 0 && data) {
      def len = load64(nitems, 0)
      if(len > 0) {
         title = read_cstr(data, len > 1024 ? 1024 : len)
      }
      xt_xfree(data)
   }
   free(prop, act, fmt, nitems, after)
   if(title.len > 0) {
      return title
   }
   def name = zalloc(8)
   if(xt_xfetch_name(display, win, name)) {
      def p = load64(name, 0)
      if(p) {
         title = read_cstr(p, 256)
         xt_xfree(p)
      }
   }
   free(name)
   if(title.len == 0) {
      return "Unknown"
   }
   title
}

fn btn_event(int button, bool press) {
   XTestFakeButtonEvent(display, button, press ? 1 : 0, 0)
   XFlush(display)
}

fn key_event(int keycode, bool press) {
   XTestFakeKeyEvent(display, keycode, press ? 1 : 0, 0)
   XFlush(display)
}

fn read_card_prop(str atom_name, any target) int {
   def a = atom_val(atom_name)
   def prop = zalloc(8)
   def act = zalloc(8)
   def fmt = zalloc(4)
   def nitems = zalloc(8)
   def after = zalloc(8)
   def ok = xt_xget_window_property(display, target, a, 0, 1, 0, 6, act, fmt, nitems, after, prop)
   mut val = 0
   if(ok == 0) {
      def p = load64(prop, 0)
      def n = load64(nitems, 0)
      def f = load32(fmt, 0)
      if(p && n > 0 && f == 32) {
         val = load32(p, 0)
      }
      if(p) { xt_xfree(p) }
   }
   free(prop, act, fmt, nitems, after)
   val
}

fn send_client_message(any target, str atom_str, int val0, int val1) {
   def a = atom_val(atom_str)
   def ev = zalloc(192)
   store32(ev, 33, 0)
   store64(ev, target, 32)
   store64(ev, a, 40)
   store32(ev, 32, 48)
   store32(ev, val0, 56)
   store32(ev, val1, 60)
   xt_xsend_event(display, root, 0, 557056, ev)
   XFlush(display)
   free(ev)
}

fn window_geom(int win) list {
   def attr = zalloc(136)
   def ok = xt_xget_window_attributes(display, win, attr)
   if(!ok) { free(attr)
      return [0, 0, 0, 0] }
   def w = load32(attr, 8)
   def h = load32(attr, 12)
   def bdr = load32(attr, 16)
   free(attr)
   def dest_x = zalloc(4)
   def dest_y = zalloc(4)
   def child_ret = zalloc(8)
   xt_xtranslate_coordinates(display, win, root, -int(bdr), -int(bdr), dest_x, dest_y, child_ret)
   def x = load_i32(dest_x, 0)
   def y = load_i32(dest_y, 0)
   free(dest_x, dest_y, child_ret)
   [int(x), int(y), int(w), int(h)]
}

fn wm_state_names(any win) list {
   def a = atom_val("_NET_WM_STATE")
   def prop = zalloc(8)
   def act = zalloc(8)
   def fmt = zalloc(4)
   def nitems = zalloc(8)
   def after = zalloc(8)
   def ok = xt_xget_window_property(display, win, a, 0, 1024, 0, 4, act, fmt, nitems, after, prop)
   mut names = []
   if(ok == 0) {
      def p = load64(prop, 0)
      def n = load64(nitems, 0)
      if(p && n > 0) {
         mut i = 0
         while(i < n) {
            def atom = load64(p, i * 8)
            def raw = xt_xget_atom_name(display, atom)
            if(raw) {
               def name = read_cstr(raw, 256)
               if(name != "Unknown") { names = names + [name] }
               xt_xfree(raw)
            }
            i += 1
         }
      }
      if(p) { xt_xfree(p) }
   }
   free(prop, act, fmt, nitems, after)
   names
}

fn set_wm_state(any win, str state1, str state2, int action) {
   def a = atom_val("_NET_WM_STATE")
   def a1 = atom_val(state1)
   def a2 = state2.len > 0 ? atom_val(state2) : 0
   def ev = zalloc(192)
   store32(ev, 33, 0)
   store64(ev, win, 32)
   store64(ev, a, 40)
   store32(ev, 32, 48)
   store32(ev, action, 56)
   store32(ev, int(a1), 60)
   store32(ev, int(a2), 64)
   store32(ev, 1, 68)
   xt_xsend_event(display, root, 0, 557056, ev)
   XFlush(display)
   free(ev)
}

fn get_top_windows() list {
   def root_ret = zalloc(8)
   def parent_ret = zalloc(8)
   def children = zalloc(8)
   def nchildren = zalloc(4)
   xt_xquery_tree(display, root, root_ret, parent_ret, children, nchildren)
   mut result = []
   def count = load32(nchildren, 0)
   def child_list = load64(children, 0)
   if(child_list && count > 0) {
      mut i = 0
      while(i < count) {
         result = result + [__tag(load64(child_list, i * 8))]
         i += 1
      }
      xt_xfree(child_list)
   }
   free(root_ret, parent_ret, children, nchildren)
   result
}

fn safe_window_exists(int win) bool {
   def pid = fork()
   if(int(pid) == 0) {
      def fd = posix_open(cstr("/dev/null"), 1)
      posix_dup2(fd, 2)
      posix_close(fd)
      def cd = XOpenDisplay(0)
      mut name_ok = false
      if(cd) {
         def attr = zalloc(136)
         name_ok = xt_xget_window_attributes(cd, win, attr) != 0
         free(attr)
         XCloseDisplay(cd)
      }
      __exit(name_ok ? 0 : 1)
   }
   waitpid(int(pid), 0) == 0
}

fn get_client_windows() list {
   def a = atom_val("_NET_CLIENT_LIST")
   def prop = zalloc(8)
   def act = zalloc(8)
   def fmt = zalloc(4)
   def nitems = zalloc(8)
   def after = zalloc(8)
   def ok = xt_xget_window_property(display, root, a, 0, 1000000, 0, 33, act, fmt, nitems, after, prop)
   if(ok == 0) {
      def p = load64(prop, 0)
      if(p) {
         def n = load64(nitems, 0)
         if(n > 0) {
            mut valid = []
            mut i = 0
            while(i < n) {
               def win = load64(p, i * 8)
               if(safe_window_exists(win)) {
                  valid = valid + [__tag(win)]
               }
               i += 1
            }
            xt_xfree(p)
            free(prop, act, fmt, nitems, after)
            if(valid.len > 0) {
               return valid
            }
         } else {
            xt_xfree(p)
         }
      }
   }
   free(prop, act, fmt, nitems, after)

   ; Fallback: use XQueryTree with fork-verified entries
   def tree = get_top_windows()
   mut valid = []
   mut i = 0
   while(i < tree.len) {
      def encoded_win = tree.get(i, 0)
      def win = raw_window_id(encoded_win)
      def w = int(win)
      if(w != 2097152 && w != 3145728 && w != 3145729 && w != 5242880) {
         if(safe_window_exists(win)) {
            valid = valid + [encoded_win]
         }
      }
      i += 1
   }
   valid
}

fn keysym_from_name(str name) any {
   def buf = zalloc(256)
   mut i = 0
   while(i < name.len) {
      store8(buf, ord(name.get(i)), i)
      i += 1
   }
   store8(buf, 0, i)
   def ks = XStringToKeysym(buf)
   free(buf)
   ks
}

fn ieq(str a, str b) bool {
   if(a.len != b.len) { return false }
   mut i = 0
   while(i < a.len) {
      mut ca = ord(a.get(i))
      mut cb = ord(b.get(i))
      if(ca >= 65 && ca <= 90) { ca += 32 }
      if(cb >= 65 && cb <= 90) { cb += 32 }
      if(ca != cb) { return false }
      i += 1
   }
   true
}

fn mod_aliases(str name) any {
   if(ieq(name, "alt") || ieq(name, "meta")) { return XStringToKeysym(cstr("Alt_L")) }
   if(ieq(name, "ctrl") || ieq(name, "control")) { return XStringToKeysym(cstr("Control_L")) }
   if(ieq(name, "shift")) { return XStringToKeysym(cstr("Shift_L")) }
   if(ieq(name, "super") || ieq(name, "win") || ieq(name, "windows")) { return XStringToKeysym(cstr("Super_L")) }
   XStringToKeysym(cstr(name))
}

fn parse_combo(str spec) list {
   mut shift = false
   mut ctrl = false
   mut alt = false
   mut key_name = ""
   def parts = spec.split("+")
   mut i = 0
   while(i < parts.len) {
      def part = parts.get(i, "")
      if(ieq(part, "shift")) { shift = true }
      elif(ieq(part, "ctrl") || ieq(part, "control")) { ctrl = true }
      elif(ieq(part, "alt") || ieq(part, "meta")) { alt = true }
      elif(!ieq(part, "super") && !ieq(part, "win") && !ieq(part, "windows")) { key_name = part }
      i += 1
   }
   [key_name, shift, ctrl, alt]
}

fn send_keysequence(str seq, bool press) {
   def plus = seq.find("+")
   mut mods = []
   mut key_part = seq
   if(plus >= 0) {
      def parts = seq.split("+")
      key_part = parts.get(parts.len - 1, "")
      mut i = 0
      while(i < parts.len - 1) {
         mods = mods + [parts.get(i, "")]
         i += 1
      }
   }
   if(press) {
      mut i = 0
      while(i < mods.len) {
         def mod = mod_aliases(mods.get(i, ""))
         if(mod) {
            def kc = XKeysymToKeycode(display, mod)
            if(int(kc)) { key_event(int(kc), true) }
         }
         i += 1
      }
   }
   def keysym = keysym_from_name(key_part)
   if(keysym) {
      def kc = XKeysymToKeycode(display, keysym)
      if(int(kc)) { key_event(int(kc), press) }
   }
   if(!press) {
      mut i = 0
      while(i < mods.len) {
         def mod = mod_aliases(mods.get(i, ""))
         if(mod) {
            def kc = XKeysymToKeycode(display, mod)
            if(int(kc)) { key_event(int(kc), false) }
         }
         i += 1
      }
   }
}

fn send_combo_key(str key_name, bool shift, bool ctrl, bool alt) {
   mut mods = []
   if(shift) {
      def kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Shift_L")))
      mods = mods + [int(kc)]
   }
   if(ctrl) {
      def kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Control_L")))
      mods = mods + [int(kc)]
   }
   if(alt) {
      def kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Alt_L")))
      mods = mods + [int(kc)]
   }
   mut i = 0
   while(i < mods.len) {
      key_event(mods.get(i, 0), true)
      i += 1
   }
   def keysym = keysym_from_name(key_name)
   if(keysym) {
      def kc = XKeysymToKeycode(display, keysym)
      if(int(kc)) {
         key_event(int(kc), true)
         msleep(20)
         key_event(int(kc), false)
      }
   }
   i = mods.len
   while(i > 0) {
      i -= 1
      key_event(mods.get(i, 0), false)
   }
}

fn type_string(str s, int delay_ms) {
   def shift_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Shift_L")))
   mut i = 0
   while(i < s.len) {
      mut c = ord(s.get(i))
      mut name = ""
      mut shift = false
      if(c >= 97 && c <= 122) {
         name = s.slice(i, i + 1)
      } elif(c >= 65 && c <= 90) {
         def letters = "abcdefghijklmnopqrstuvwxyz"
         name = letters.slice(c - 65, c - 64)
         shift = true
      } elif(c >= 48 && c <= 57) {
         name = s.slice(i, i + 1)
      } elif(c == 32) {
         name = "space"
      } elif(c == 10) {
         name = "Return"
      } elif(c == 9) {
         name = "Tab"
      } elif(c == 47) { name = "slash"
      } elif(c == 63) { name = "slash"
         shift = true
      } elif(c == 46) { name = "period"
      } elif(c == 62) { name = "period"
         shift = true
      } elif(c == 44) { name = "comma"
      } elif(c == 60) { name = "comma"
         shift = true
      } elif(c == 58) { name = "semicolon"
         shift = true
      } elif(c == 59) { name = "semicolon"
      } elif(c == 40) { name = "9"
         shift = true
      } elif(c == 41) { name = "0"
         shift = true
      } elif(c == 34) { name = "apostrophe"
         shift = true
      } elif(c == 39) { name = "apostrophe"
      } elif(c == 95) { name = "minus"
         shift = true
      } elif(c == 45) { name = "minus"
      } elif(c == 61) { name = "equal"
      } elif(c == 43) { name = "equal"
         shift = true
      } elif(c == 91) { name = "bracketleft"
      } elif(c == 123) { name = "bracketleft"
         shift = true
      } elif(c == 93) { name = "bracketright"
      } elif(c == 125) { name = "bracketright"
         shift = true
      } elif(c == 92) { name = "backslash"
      } elif(c == 124) { name = "backslash"
         shift = true
      } elif(c == 96) { name = "grave"
      } elif(c == 126) { name = "grave"
         shift = true
      } elif(c == 33) { name = "1"
         shift = true
      } elif(c == 64) { name = "2"
         shift = true
      } elif(c == 35) { name = "3"
         shift = true
      } elif(c == 36) { name = "4"
         shift = true
      } elif(c == 37) { name = "5"
         shift = true
      } elif(c == 94) { name = "6"
         shift = true
      } elif(c == 38) { name = "7"
         shift = true
      } elif(c == 42) { name = "8"
         shift = true
      } else {
         eprint("type: unsupported character '" + str(s.slice(i, i + 1)) + "'")
         i += 1
         continue
      }
      if(name.len > 0) {
         def ks = XStringToKeysym(cstr(name))
         if(ks) {
            def kc = XKeysymToKeycode(display, ks)
            if(kc) {
               if(shift) { key_event(int(shift_kc), true) }
               key_event(int(kc), true)
               msleep(delay_ms)
               key_event(int(kc), false)
               if(shift) { key_event(int(shift_kc), false) }
               msleep(delay_ms)
            }
         }
      }
      i += 1
   }
}

fn mouse_move_to(int tx, int ty, int sx, int sy, int steps) {
   mut i = 0
   while(i <= steps) {
      def t = float(i) / float(steps)
      def p = t * t * (3.0 - 2.0 * t)
      def nx = sx + int(p * float(tx - sx))
      def ny = sy + int(p * float(ty - sy))
      XTestFakeMotionEvent(display, 0, nx, ny, 0)
      XFlush(display)
      msleep(5)
      i += 1
   }
}

fn list_windows() {
   def wins = get_client_windows()
   if(wins.len == 0) {
      eprint("No windows found")
      return 1
   }
   print("Window list:")
   mut i = 0
   while(i < wins.len) {
      def win = raw_window_id(wins.get(i, 0))
      print("  ID: " + to_str(int(win)) + "  Title: " + win_title(win))
      i += 1
   }
   0
}

fn search_windows(str needle) {
   def wins = get_client_windows()
   if(wins.len == 0) {
      print("No windows found")
      return
   }
   mut found = false
   mut i = 0
   while(i < wins.len) {
      def win = raw_window_id(wins.get(i, 0))
      def title = win_title(win)
      if(title.contains(needle)) {
         print(to_str(int(win)) + "  " + title)
         found = true
      }
      i += 1
   }
   if(!found) {
      print("No matching windows found")
   }
}

fn selectwindow() int {
   def cursor = XCreateFontCursor(display, 128)
   def ret = XGrabPointer(display, root, 0, 8, 1, 0, 0, cursor, 0)
   if(ret != 0) {
     XFreeCursor(display, cursor)
      eprint("Failed to grab pointer")
      return 0
   }
   def event = zalloc(192)
   mut win = 0
   while(true) {
      XAllowEvents(display, 0, 0)
      store32(event, 0, 0)
      XNextEvent(display, event)
      if(load32(event, 0) == 5) {
         def sub = load64(event, 48)
         win = sub ? sub : load64(event, 32)
         break
      }
   }
   free(event)
   XUngrabPointer(display, 0)
   XFlush(display)
   XFreeCursor(display, cursor)
   win
}

fn find_window(str needle, bool prefer_largest) int {
   def wins = get_client_windows()
   mut best = 0
   mut best_area = -1
   mut i = 0
   while(i < wins.len) {
      def encoded_win = wins.get(i, 0)
      def win = raw_window_id(encoded_win)
      def title = win_title(win)
      if(title.contains(needle)) {
         if(prefer_largest) {
            def g = window_geom(win)
            def area = int(g.get(2, 0)) * int(g.get(3, 0))
            if(int(area) > best_area) {
               best = win
               best_area = int(area)
            }
         } else {
            best = win
            break
         }
      }
      i += 1
   }
   best
}

fn wait_window(str needle, int timeout_ms) int {
   mut remaining = timeout_ms
   while(remaining > 0) {
      def win = find_window(needle, true)
      if(win) { return win }
      msleep(200)
      remaining -= 200
   }
   0
}

fn print_window_geom(any win, bool shell_mode, str prefix) {
   def g = window_geom(win)
   def x = g.get(0, 0)
   def y = g.get(1, 0)
   def w = g.get(2, 0)
   def h = g.get(3, 0)
   if(shell_mode) {
      print(prefix + "WINDOW=" + to_str(int(win)))
      print(prefix + "X=" + to_str(int(x)))
      print(prefix + "Y=" + to_str(int(y)))
      print(prefix + "WIDTH=" + to_str(int(w)))
      print(prefix + "HEIGHT=" + to_str(int(h)))
      print(prefix + "SCREEN=0")
   } else {
      print("Window " + to_str(int(win)))
      print("  Position: " + to_str(int(x)) + "," + to_str(int(y)) + " (screen: 0)")
      print("  Geometry: " + to_str(int(w)) + "x" + to_str(int(h)))
   }
}

fn getwindowname(any win) {
   print(win_title(win))
}

fn get_window_class(any win) str {
   def hint = zalloc(16)
   def ok = xt_xget_class_hint(display, win, hint)
   mut cls = ""
   if(ok) {
      def res_class = load64(hint, 8)
      if(res_class) {
         cls = read_cstr(res_class, 256)
         xt_xfree(res_class)
      }
      def res_name = load64(hint, 0)
      if(res_name) { xt_xfree(res_name) }
   }
   free(hint)
   if(cls.len == 0) { cls = "Unknown" }
   cls
}

fn get_window_focus() int {
   def focus = zalloc(8)
   def revert = zalloc(4)
   XGetInputFocus(display, focus, revert)
   def w = load64(focus, 0)
   free(focus, revert)
   w
}

fn inputscan_loop() {
   def gret = XGrabKeyboard(display, root, 0, 1, 1, 0)
   if(gret != 0) {
      eprint("Failed to grab keyboard (already grabbed?)")
      return
   }
   def event = zalloc(192)
   mut pressed = list(256)
   mut i = 0
   while(i < 256) {
      pressed = pressed.append(false)
      i += 1
   }
   print("Listening for key events. Press Escape or Ctrl+C to quit.")
   while(true) {
      store32(event, 0, 0)
      XNextEvent(display, event)
      def etype = load32(event, 0)
      if(etype == 2 || etype == 3) {
         def is_press = etype == 2
         def keycode = load32(event, 84)
         if(keycode < 256) {
            if(is_press) {
               if(pressed.get(keycode)) {
                  continue
               }
               pressed.set(keycode, true)
            } else {
               mut is_autorepeat = false
               if(XEventsQueued(display, 1) > 0) {
                  def nev = zalloc(192)
                  XPeekEvent(display, nev)
                  if(load32(nev, 0) == 2 && load32(nev, 84) == keycode && load64(nev, 56) == load64(event, 56)) {
                     is_autorepeat = true
                  }
                  free(nev)
               }
               if(is_autorepeat) {
                  continue
               }
               pressed.set(keycode, false)
            }
         }
         def keysym = XKeycodeToKeysym(display, keycode, 0)
         def state = load32(event, 80)
         mut mods = ""
         if(state & 1) { mods = mods + "Shift+" }
         if(state & 4) { mods = mods + "Ctrl+" }
         if(state & 8) { mods = mods + "Alt+" }
         if(state & 64) { mods = mods + "Super+" }
         mut name = ""
         def ks_ptr = XKeysymToString(keysym)
         if(ks_ptr) {
            name = ks_ptr
            def end = name.find("\0")
            if(end >= 0) { name = name.slice(0, end) }
         }
         if(name.len == 0) {
            name = "0x" + to_hex(int(keysym))
         }
         def ev_name = is_press ? "KeyPress" : "KeyRelease"
         print(ev_name + ": " + mods + name + " (keycode=" + to_str(keycode) + ")")
         if(is_press && (name == "Escape" || (name == "c" && (state & 4) != 0))) {
            break
         }
      }
   }
   XUngrabKeyboard(display, 0)
   free(event)
}

fn btn_tap(btn) {
   btn_event(btn, true)
   msleep(8)
   btn_event(btn, false)
}

fn fakemouse_loop(use_wasd) {
   def q_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("q")))
   def esc_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Escape")))
   def w_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("w")))
   def a_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("a")))
   def s_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("s")))
   def d_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("d")))
   def up_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Up")))
   def down_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Down")))
   def left_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Left")))
   def right_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Right")))
   def kp8_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("KP_8")))
   def kp2_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("KP_2")))
   def kp4_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("KP_4")))
   def kp6_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("KP_6")))
   def kp7_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("KP_7")))
   def kp9_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("KP_9")))
   def kp1_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("KP_1")))
   def kp3_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("KP_3")))
   def kp5_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("KP_5")))
   def sp_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("space")))
   def r_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("r")))
   def m_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("m")))
   def pgup_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Page_Up")))
   def pgdn_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Page_Down")))
   def shift_l_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Shift_L")))
   def shift_r_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("Shift_R")))
   def k1_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("1")))
   def k2_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("2")))
   def k3_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("3")))
   def k4_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("4")))
   def k5_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("5")))
   def k6_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("6")))
   def k7_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("7")))
   def k8_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("8")))
   def k9_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("9")))
   def k0_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("0")))
   def plus_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("plus")))
   def minus_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("minus")))
   def comma_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("comma")))
   def period_kc = XKeysymToKeycode(display, XStringToKeysym(cstr("period")))
   def scr_w = XDisplayWidth(display, 0)
   def scr_h = XDisplayHeight(display, 0)
   mut event = zalloc(192)
   mut held_up = false
   mut held_down = false
   mut held_left = false
   mut held_right = false
   mut dragging = false
   mut hold_left = false
   mut hold_right = false
   mut hold_middle = false
    mut speed = 8

    mut km_buf = zalloc(32)
   def gret = XGrabKeyboard(display, root, 0, 1, 1, 0)
   if(gret != 0) {
      eprint("Failed to grab keyboard (already grabbed?)")
      free(event)
      return
   }
   print("arrows/numpad=move" + (use_wasd ? "/wasd" : "") + "  space=hold  r=hold3  m=hold2  kp5=click")
    print("pgup/uparrow=scrollUp  pgdn/downarrow=scrollDown  shift=drag")
    print("1-9=speed  0=pixel  +/-=adjust  q=quit")
    print("speed=" + to_str(int(speed)))
    while(true) {
       def px = mouse_pos().get(0, 0)
       def py = mouse_pos().get(1, 0)
       store32(event, 0, 0)
       mut ev_found = XCheckTypedEvent(display, 2, event)
       if(!ev_found) {
          store32(event, 0, 0)
          ev_found = XCheckTypedEvent(display, 3, event)
       }
       if(ev_found) {
          def etype = load32(event, 0)
          def kc = load32(event, 84)
         if(kc == q_kc || kc == esc_kc) {
            if(dragging) {
               btn_event(1, false)
               key_event(shift_l_kc, false)
               dragging = false
            }
            if(hold_left) {
               btn_event(1, false)
               hold_left = false
            }
            if(hold_right) {
               btn_event(3, false)
               hold_right = false
            }
            if(hold_middle) {
               btn_event(2, false)
               hold_middle = false
            }
             XUngrabKeyboard(display, 0)
             XFlush(display)
             free(event, km_buf)
             return
         }
          mut is_bound = false
          def pressed = etype == 2
          if(kc == up_kc || kc == kp8_kc || kc == kp7_kc || kc == kp9_kc) {
             held_up = pressed
             is_bound = true
          }
          if(kc == down_kc || kc == kp2_kc || kc == kp1_kc || kc == kp3_kc) {
             held_down = pressed
             is_bound = true
          }
          if(kc == left_kc || kc == kp4_kc || kc == kp7_kc || kc == kp1_kc) {
             held_left = pressed
             is_bound = true
          }
          if(kc == right_kc || kc == kp6_kc || kc == kp9_kc || kc == kp3_kc) {
             held_right = pressed
             is_bound = true
          }
          if(use_wasd) {
             if(kc == w_kc) {
                held_up = pressed
                is_bound = true
             }
             if(kc == s_kc) {
                held_down = pressed
                is_bound = true
             }
             if(kc == a_kc) {
                held_left = pressed
                is_bound = true
             }
             if(kc == d_kc) {
                held_right = pressed
                is_bound = true
             }
          }
          if(kc == sp_kc) {
             is_bound = true
             if(pressed) {
                if(hold_left) {
                   btn_event(1, false)
                   hold_left = false
                   print("left up")
                } else {
                   btn_event(1, true)
                   hold_left = true
                   print("left down")
                }
             }
          }
          if(kc == r_kc) {
             is_bound = true
             if(pressed) {
                if(hold_right) {
                   btn_event(3, false)
                   hold_right = false
                   print("right up")
                } else {
                   btn_event(3, true)
                   hold_right = true
                   print("right down")
                }
             }
          }
          if(kc == m_kc) {
             is_bound = true
             if(pressed) {
                if(hold_middle) {
                   btn_event(2, false)
                   hold_middle = false
                   print("middle up")
                } else {
                   btn_event(2, true)
                   hold_middle = true
                   print("middle down")
                }
             }
          }
          if(kc == kp5_kc) {
             is_bound = true
             if(pressed) { btn_tap(1) }
          }
          if(kc == period_kc) {
             is_bound = true
             if(pressed) { btn_tap(3) }
          }
          if(kc == comma_kc) {
             is_bound = true
             if(pressed) { btn_tap(2) }
          }
          if(kc == pgup_kc) {
             is_bound = true
             if(pressed) { btn_tap(4) }
          }
          if(kc == pgdn_kc) {
             is_bound = true
             if(pressed) { btn_tap(5) }
          }
          if(kc == shift_l_kc || kc == shift_r_kc) {
             is_bound = true
             if(pressed && !dragging) {
                key_event(kc, true)
                btn_event(1, true)
                dragging = true
             }
             if(!pressed && dragging) {
                btn_event(1, false)
                key_event(kc, false)
                dragging = false
             }
          }
          if(kc == k0_kc || kc == k1_kc || kc == k2_kc || kc == k3_kc || kc == k4_kc) {
             is_bound = true
             if(pressed) {
                if(kc == k0_kc) { speed = 1 }
                if(kc == k1_kc) { speed = 4 }
                if(kc == k2_kc) { speed = 8 }
                if(kc == k3_kc) { speed = 12 }
                if(kc == k4_kc) { speed = 16 }
                print("speed=" + to_str(int(speed)))
             }
          }
          if(kc == k5_kc || kc == k6_kc || kc == k7_kc || kc == k8_kc || kc == k9_kc) {
             is_bound = true
             if(pressed) {
                if(kc == k5_kc) { speed = 20 }
                if(kc == k6_kc) { speed = 24 }
                if(kc == k7_kc) { speed = 28 }
                if(kc == k8_kc) { speed = 32 }
                if(kc == k9_kc) { speed = 36 }
                print("speed=" + to_str(int(speed)))
             }
          }
          if(kc == plus_kc || kc == minus_kc) {
             is_bound = true
             if(pressed) {
                if(kc == plus_kc) {
                   speed = speed + 2 > 50 ? speed : speed + 2
                } else {
                   speed = speed - 2 < 1 ? speed : speed - 2
                }
                print("speed=" + to_str(int(speed)))
             }
           }
        }
        mut dx = 0
       mut dy = 0
       if(held_up) { dy = -speed }
       if(held_down) { dy = speed }
      if(held_left) { dx = -speed }
      if(held_right) { dx = speed }
      if(dx != 0 || dy != 0) {
         mut nx = int(px) + dx
         mut ny = int(py) + dy
         if(ny < 0) { ny = 0 }
         if(ny >= scr_h) { ny = scr_h - 1 }
         if(nx < 0) { nx = 0 }
         if(nx >= scr_w) { nx = scr_w - 1 }
         XTestFakeMotionEvent(display, 0, nx, ny, 0)
         XFlush(display)
      }
      msleep(15)
   }
}

fn print_usage() {
   print("ytool 0.1 - Wayland + X11 input tool")
   print("Usage: ytool [--] <command> [args...]")
   print("")
   print("Mouse commands:")
   print("   mousemove x y                  Move mouse to absolute position")
   print("   mousemove_smooth x y           Move mouse absolute with smooth animation")
   print("   mousemove_relative x y         Move mouse relative to current position")
   print("   mousemove_relative_smooth x y  Move mouse relative with smooth animation")
   print("   click [button]                 Click mouse button (1=left, 2=middle, 3=right)")
   print("   mousedown [button]             Press mouse button")
   print("   mouseup [button]               Release mouse button")
   print("   getmouselocation               Print current mouse position")
   print("")
   print("Keyboard commands:")
   print("   key <seq>                      Send keystroke (e.g. Return, alt+Tab)")
   print("   keydown <seq>                  Press key(s)")
   print("   keyup <seq>                    Release key(s)")
   print("   combo <seq>                    Send combo (e.g. ctrl+alt+F1)")
   print("   type <text>                    Type a string of text")
   print("   repeat-key <n> <key> [--delay] Repeat key N times")
   print("")
   print("Window commands:")
   print("   listwindows                    List all client windows")
   print("   search <text>                  Search windows by title")
   print("   find-window [pattern]          Find window by name (largest match)")
   print("   wait-window <sec> [pattern]    Wait for window to appear")
   print("   getactivewindow                Print active window ID")
   print("   active-window-id               Print active window ID (decimal)")
   print("   getwindowfocus                 Print focused window ID")
   print("   getwindowgeometry <id>         Get window position and size")
   print("   getwindowname <id>             Get window title")
   print("   getwindowclassname <id>        Get window class name")
   print("   getwindowpid <id>              Get window PID")
   print("   window-geom <id>               Get window geometry: x y w h")
   print("   windowfocus <id>               Focus a window")
   print("   windowactivate <id>            Activate a window (switches desktop)")
   print("   windowclose <id>               Close a window")
   print("   windowkill <id>                Kill a window's client")
   print("   windowsize <id> <w> <h>        Resize a window")
   print("   windowmove <id> <x> <y>        Move a window")
   print("   windowmap <id>                 Show a window")
   print("   windowunmap <id>               Hide a window")
   print("   windowminimize <id>            Minimize a window")
   print("   windowraise <id>               Raise a window")
   print("   windowlower <id>               Lower a window")
   print("   move-window <id> <rx> <ry>     Warp pointer to window-relative")
   print("   click-window <id> <rx> <ry>    Warp+click relative to window")
   print("   wm-state [id]                  List WM_STATE atoms of window")
   print("   fullscreen-window <id>         Set window fullscreen")
   print("   maximize-window <id>           Maximize window")
   print("")
   print("Desktop commands:")
   print("   get_num_desktops               Print number of desktops")
   print("   set_num_desktops <n>           Set number of desktops")
   print("   get_desktop                    Print current desktop number")
   print("   set_desktop <n>                Switch to desktop n")
   print("   get_desktop_for_window <id>    Print desktop containing window")
   print("   set_desktop_for_window <id> <n>  Move window to desktop n")
   print("   get_desktop_viewport           Print desktop viewport x y")
   print("   set_desktop_viewport <x> <y>   Set desktop viewport")
   print("   getdisplaygeometry             Print screen dimensions")
   print("")
   print("Monitoring:")
   print("   inputscan                      Listen for key events")
   print("   fakemouse [--wasd]             Control mouse with keyboard [add wasd]")
   print("")
   print("Other:")
   print("   sleep <sec>                    Sleep for N seconds")
   print("   exec <cmd...>                  Execute a command")
   print("   windowquit <id>                Send WM_DELETE_WINDOW (close request)")
   print("   help                           Show this help")
   print("   version                        Print version")
   print("")
   print("Examples:")
   print("   ytool mousemove 100 100")
   print("   ytool mousemove_relative -10 -10")
   print("   ytool click 1")
   print("   ytool key Return")
   print("   ytool key alt+Tab")
   print("   ytool type \"Hello World\"")
   print("   ytool selectwindow")
   print("   ytool getmouselocation")
   print("   ytool getdisplaygeometry")
   print("   ytool getwindowgeometry 0x1e00003")
   print("   ytool getwindowname 0x1e00003")
   print("   ytool getwindowclassname 0x1e00003")
   print("   ytool search firefox")
   print("   ytool listwindows")
   print("   ytool windowactivate 0x1e00003")
   print("   ytool windowsize 0x1e00003 800 600")
   print("   ytool windowmove 0x1e00003 0 0")
   print("   ytool inputscan")
   print("   ytool fakemouse")
}


extern "" {
   fn c_socket(i32 domain, i32 typ, i32 protocol) i32 as "socket"
   fn c_connect(i32 fd, ptr addr, u32 len) i32 as "connect"
   fn c_send(i32 fd, ptr buf, u64 len, i32 flags) i64 as "send"
   fn c_sendmsg(i32 fd, ptr msg, i32 flags) i64 as "sendmsg"
   fn c_recvmsg(i32 fd, ptr msg, i32 flags) i64 as "recvmsg"
   fn c_poll(ptr fds, u64 nfds, i32 timeout) i32 as "poll"
   fn c_memfd_create(ptr name, u32 flags) i32 as "memfd_create"
   fn c_write(i32 fd, ptr buf, u64 n) i64 as "write"
   fn c_close(i32 fd) i32 as "close"
   fn c_open(ptr path, i32 flags, i32 mode) i32 as "open"
   fn c_unlink(ptr path) i32 as "unlink"
}

def AF_UNIX = 1
def SOCK_STREAM = 1
def SOL_SOCKET = 1
def SCM_RIGHTS = 1
def POLLIN = 1
def DISPLAY_ID = 1
def RX_CAP = 1 << 20
def KEYMAP_OPEN_FLAGS = 194 ;; O_RDWR | O_CREAT | O_EXCL
def KEYMAP_OPEN_MODE = 384  ;; 0600

def NEED = ["zwp_input_method_manager_v2", "zwp_virtual_keyboard_manager_v1",
            "zwlr_virtual_pointer_manager_v1", "wl_seat"]
def KNOWN = ["wl_seat", "zwp_virtual_keyboard_manager_v1",
             "zwlr_virtual_pointer_manager_v1", "zwp_input_method_manager_v2"]

;; A conservative US layout; used only when xkbcli is unavailable. xkbcommon
;; resolves the includes from its system data directory.
def KM_FALLBACK = "xkb_keymap {\n xkb_keycodes { include \"evdev+aliases(qwerty)\" };\n xkb_types { include \"complete\" };\n xkb_compat { include \"complete\" };\n xkb_symbols { include \"pc+us+inet(evdev)\" };\n};\n"

mut SOCK = -1
mut NEXT_ID = 2
mut REG = 0
mut SEAT = 0
mut KBD = 0
mut PTR = 0
mut IM = 0
mut G_NAME = {}
mut G_VER = {}
mut RX = 0
mut RX_LEN = 0
mut RX_OFF = 0
mut WAIT_ID = 0
mut WAIT_DONE = 0
mut IM_PENDING = 0
mut IM_PENDING_SET = false
mut IM_ACTIVE = false
mut IM_SERIAL = 0
mut T0 = 0
mut OUT_W = 1280
mut OUT_H = 720
mut KEYMAP_SIZE = 0
mut KEYS = {}
mut KEYS_SHIFTED = {}

fn die(str msg) any {
   print("Error: " + msg)
   exit(1)
}

;; ------------------------------------------------------------ wire encoding
fn b_u32(list b, int v) list {
   b = b.append(v & 255)
   b = b.append((v >> 8) & 255)
   b = b.append((v >> 16) & 255)
   b = b.append((v >> 24) & 255)
   b
}

fn b_str(list b, str s) list {
   b = b_u32(b, s.len + 1)
   mut i = 0
   while i < s.len {
      b = b.append(load8(s, i))
      i += 1
   }
   b = b.append(0)
   while b.len % 4 != 0 { b = b.append(0) }
   b
}

fn pack(list b) ptr {
   def n = b.len
   def p = malloc(n > 0 ? n : 1)
   mut i = 0
   while i < n {
      store8(p, b.get(i, 0), i)
      i += 1
   }
   p
}

fn msg_bytes(int obj, int op, list payload) list {
   def total = 8 + payload.len
   mut m = b_u32([], obj)
   m = b_u32(m, (total << 16) | (op & 0xFFFF))
   mut i = 0
   while i < payload.len {
      m = m.append(payload.get(i, 0))
      i += 1
   }
   m
}

fn send_all(ptr p, int n) bool {
   mut off = 0
   while off < n {
      def w = c_send(SOCK, p + off, n - off, 0)
      if w <= 0 { return false }
      off += int(w)
   }
   true
}

fn wl_send(int obj, int op, list payload) {
   def m = msg_bytes(obj, op, payload)
   def p = pack(m)
   def ok = send_all(p, m.len)
   free(p)
   if !ok { die("could not send to the compositor") }
}

;; One message plus a single SCM_RIGHTS fd in the ancillary data.
fn wl_send_fd(int obj, int op, list payload, int fd) {
   def m = msg_bytes(obj, op, payload)
   def p = pack(m)
   def iov = malloc(16)
   store64_h(iov, p, 0)
   store64(iov, m.len, 8)
   def ctrl = malloc(24)
   mem.memset(ctrl, 0, 24)
   store64(ctrl, 20, 0)          ;; cmsg_len = CMSG_LEN(sizeof(int))
   store32(ctrl, SOL_SOCKET, 8)
   store32(ctrl, SCM_RIGHTS, 12)
   store32(ctrl, fd, 16)
   def mh = malloc(56)
   mem.memset(mh, 0, 56)
   store64_h(mh, iov, 16)        ;; msg_iov
   store64(mh, 1, 24)            ;; msg_iovlen
   store64_h(mh, ctrl, 32)       ;; msg_control
   store64(mh, 24, 40)           ;; msg_controllen
   def sent = c_sendmsg(SOCK, mh, 0)
   free(mh)
   free(ctrl)
   free(iov)
   free(p)
   if sent <= 0 { die("could not send a keymap to the compositor") }
}

;; ------------------------------------------------------------- wire reading
fn rx_init() {
   RX = malloc(RX_CAP)
   RX_LEN = 0
   RX_OFF = 0
}

fn rx_append(ptr src, int n) {
   if RX_LEN + n > RX_CAP { die("wayland event buffer overflow") }
   if RX_OFF > 0 {
      mem.memcpy(RX, RX + RX_OFF, RX_LEN)
      RX_OFF = 0
   }
   mem.memcpy(RX + RX_LEN, src, n)
   RX_LEN += n
}

fn rx_str_is(int off, str s) bool {
   def n = load32(RX, off)
   if n != s.len + 1 { return false }
   mut i = 0
   while i < s.len {
      if load8(RX, off + 4 + i) != load8(s, i) { return false }
      i += 1
   }
   load8(RX, off + 4 + s.len) == 0
}

fn wl_recv(int wait_ms) int {
   def pfd = malloc(8)
   mem.memset(pfd, 0, 8)
   store32(pfd, SOCK, 0)
   store16(pfd, POLLIN, 4)
   def ready = c_poll(pfd, 1, wait_ms)
   free(pfd)
   if ready <= 0 { return 0 }
   def buf = malloc(65536)
   def iov = malloc(16)
   store64_h(iov, buf, 0)
   store64(iov, 65536, 8)
   def mh = malloc(56)
   mem.memset(mh, 0, 56)
   store64_h(mh, iov, 16)
   store64(mh, 1, 24)
   def n = c_recvmsg(SOCK, mh, 0)
   free(mh)
   free(iov)
   if n <= 0 {
      free(buf)
      if n == 0 { die("compositor closed the connection") }
      return 0
   }
   rx_append(buf, int(n))
   free(buf)
   1
}

fn note_global(int off) {
   def name = load32(RX, off)
   def nlen = load32(RX, off + 4)
   def voff = off + 8 + ((nlen + 3) & (~3))
   def ver = load32(RX, voff)
   mut i = 0
   while i < KNOWN.len {
      def iface = to_str(KNOWN.get(i, ""))
      if rx_str_is(off + 4, iface) {
         G_NAME[iface] = name
         G_VER[iface] = ver
      }
      i += 1
   }
}

fn handle_event(int obj, int op, int off, int plen) {
   if obj == DISPLAY_ID && op == 0 {
      def oid = load32(RX, off)
      def code = load32(RX, off + 4)
      die("protocol error on object " + to_str(oid) + " code " + to_str(code))
   }
   if obj == REG && op == 0 {
      note_global(off)
      return
   }
   if WAIT_ID != 0 && obj == WAIT_ID && op == 0 {
      WAIT_DONE = 1
      return
   }
   if IM != 0 && obj == IM {
      if op == 0 {
         IM_PENDING = 1
         IM_PENDING_SET = true
      } elif op == 1 {
         IM_PENDING = 0
         IM_PENDING_SET = true
      } elif op == 5 {
         IM_SERIAL = load32(RX, off)
         if IM_PENDING_SET { IM_ACTIVE = IM_PENDING == 1 }
      } elif op == 6 {
         die("input method unavailable (another one is bound)")
      }
   }
}

fn wl_dispatch(int wait_ms) bool {
   if wl_recv(wait_ms) == 0 { return false }
   while true {
      if RX_LEN < 8 { break }
      def obj = load32(RX, RX_OFF)
      def word = load32(RX, RX_OFF + 4)
      def size = (word >> 16) & 0xFFFF
      if size < 8 || RX_LEN < size { break }
      def op = word & 0xFFFF
      handle_event(obj, op, RX_OFF + 8, size - 8)
      RX_OFF += size
      RX_LEN -= size
   }
   if RX_LEN == 0 { RX_OFF = 0 }
   true
}

fn wl_pump(f64 seconds) {
   def end = now_ms() + int(seconds * 1000.0)
   while true {
      def left = end - now_ms()
      if left <= 0 { break }
      wl_dispatch(int(left > 50 ? 50 : left))
   }
}

fn now32() int {
   (now_ms() - T0) & 0xFFFFFFFF
}

;; -------------------------------------------------------------- connection
fn wl_new_id() int {
   def i = NEXT_ID
   NEXT_ID += 1
   i
}

fn runtime_path() str {
   def rt = common.env_trim("XDG_RUNTIME_DIR")
   def dn = common.env_trim("WAYLAND_DISPLAY")
   if rt.len == 0 || dn.len == 0 { return "" }
   if load8(dn, 0) == 47 { return dn }
   rt + "/" + dn
}

fn wl_roundtrip() {
   WAIT_ID = wl_new_id()
   WAIT_DONE = 0
   wl_send(DISPLAY_ID, 0, b_u32([], WAIT_ID))
   def end = now_ms() + 5000
   while WAIT_DONE == 0 {
      wl_dispatch(200)
      if now_ms() > end { die("roundtrip timed out") }
   }
   WAIT_ID = 0
}

fn wl_open(str path) bool {
   SOCK = c_socket(AF_UNIX, SOCK_STREAM, 0)
   if SOCK < 0 { return false }
   def sa = malloc(110)
   mem.memset(sa, 0, 110)
   store16(sa, AF_UNIX, 0)
   mut i = 0
   while i < path.len && i < 106 {
      store8(sa, load8(path, i), 2 + i)
      i += 1
   }
   def rc = c_connect(SOCK, sa, 2 + i + 1)
   free(sa)
   if rc != 0 {
      c_close(SOCK)
      SOCK = -1
      return false
   }
   T0 = now_ms()
   NEXT_ID = 2
   G_NAME = {}
   G_VER = {}
   rx_init()
   REG = wl_new_id()
   wl_send(DISPLAY_ID, 1, b_u32([], REG))
   wl_roundtrip()
   true
}

fn has_global(str iface) bool {
   int(G_NAME.get(iface, -1)) >= 0
}

fn wl_bind(str iface, int version) int {
   if !has_global(iface) { return 0 }
   def id = wl_new_id()
   mut b = b_u32([], G_NAME.get(iface, 0))
   b = b_str(b, iface)
   b = b_u32(b, min(version, G_VER.get(iface, 1)))
   b = b_u32(b, id)
   wl_send(REG, 0, b)
   id
}

;; ---------------------------------------------------------------- keymap/fd
fn keymap_source() str {
   def r = run_capture(["xkbcli", "compile-keymap", "--layout", "us"], [], nil, false)
   mut km = ""
   if int(r.get("code", 1)) == 0 { km = to_str(r.get("stdout", "")) }
   if km.len == 0 { km = KM_FALLBACK }
   km
}

fn make_keymap_fd() int {
   def km = keymap_source()
   KEYMAP_SIZE = km.len + 1
   mut fd = c_memfd_create(mem.cstr("ny-keymap"), 0)
   if fd < 0 {
      def dir = common.env_trim("TMPDIR")
      def base = dir.len > 0 ? dir : "/tmp"
      def path = base + "/ny-keymap-" + to_str(now_ms())
      fd = c_open(mem.cstr(path), KEYMAP_OPEN_FLAGS, KEYMAP_OPEN_MODE)
      if fd >= 0 {
         def ignored = c_unlink(mem.cstr(path))
         ignored
      }
   }
   if fd < 0 { die("could not create the keymap file descriptor") }
   def kc = mem.cstr(km)
   def wrote = c_write(fd, kc, km.len)
   if wrote < 0 { die("could not write the keymap") }
   def z = malloc(1)
   store8(z, 0, 0)
   def zn = c_write(fd, z, 1)
   free(z)
   if zn < 0 { die("could not terminate the keymap") }
   fd
}

;; --------------------------------------------------------------------- seat
fn seat_init() {
   SEAT = wl_bind("wl_seat", 1)
   if SEAT == 0 { die("compositor has no wl_seat") }
   def vkm = wl_bind("zwp_virtual_keyboard_manager_v1", 1)
   if vkm == 0 { die("compositor has no zwp_virtual_keyboard_manager_v1") }
   KBD = wl_new_id()
   wl_send(vkm, 0, b_u32(b_u32([], SEAT), KBD))
   def kmfd = make_keymap_fd()
   wl_send_fd(KBD, 0, b_u32(b_u32([], 1), KEYMAP_SIZE), kmfd)
   def closed = c_close(kmfd)
   closed
   def vpm = wl_bind("zwlr_virtual_pointer_manager_v1", 1)
   if vpm == 0 { die("compositor has no zwlr_virtual_pointer_manager_v1") }
   PTR = wl_new_id()
   wl_send(vpm, 0, b_u32(b_u32([], SEAT), PTR))
   def imm = wl_bind("zwp_input_method_manager_v2", 1)
   if imm == 0 { die("compositor has no zwp_input_method_manager_v2") }
   IM = wl_new_id()
   wl_send(imm, 0, b_u32(b_u32([], SEAT), IM))
   wl_roundtrip()
   def ow = common.env_trim("NY_DRIVER_OUTPUT_W")
   if ow.len > 0 { OUT_W = int(sx.atof(ow)) }
   def oh = common.env_trim("NY_DRIVER_OUTPUT_H")
   if oh.len > 0 { OUT_H = int(sx.atof(oh)) }
}

fn keys_init() {
   mut k = {}
   mut i = 0
   while i < 10 {
      k[slice("1234567890", i, i + 1)] = 2 + i
      i += 1
   }
   def rows = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
   mut r = 0
   while r < 3 {
      def row = to_str(rows.get(r, ""))
      def start = 16 + r * 14
      mut j = 0
      while j < row.len {
         k[slice(row, j, j + 1)] = start + j
         j += 1
      }
      r += 1
   }
   k[" "] = 57
   k["-"] = 12
   k["="] = 13
   k[","] = 51
   k["."] = 52
   k["/"] = 53
   k[";"] = 39
   k["'"] = 40
   KEYS = k
   mut sh = {}
   sh["!"] = "1"
   sh["@"] = "2"
   sh["#"] = "3"
   sh["$"] = "4"
   sh["%"] = "5"
   sh["^"] = "6"
   sh["&"] = "7"
   sh["*"] = "8"
   sh["("] = "9"
   sh[")"] = "0"
   sh["_"] = "-"
   sh["+"] = "="
   sh["<"] = ","
   sh[">"] = "."
   sh["?"] = "/"
   sh[":"] = ";"
   sh["\""] = "'"
   KEYS_SHIFTED = sh
}

fn seat_key(int code, int mods) {
   if mods != 0 {
      wl_send(KBD, 2, b_u32(b_u32(b_u32(b_u32([], mods), 0), 0), 0))
   }
   wl_send(KBD, 1, b_u32(b_u32(b_u32([], now32()), code), 1))
   wl_pump(0.03)
   wl_send(KBD, 1, b_u32(b_u32(b_u32([], now32()), code), 0))
   if mods != 0 {
      wl_send(KBD, 2, b_u32(b_u32(b_u32(b_u32([], 0), 0), 0), 0))
   }
   wl_pump(0.03)
}

fn lower_ascii(str c) str {
   def b = load8(c, 0)
   if b >= 65 && b <= 90 { return slice("abcdefghijklmnopqrstuvwxyz", b - 65, b - 64) }
   c
}

fn seat_key_state(int code, int down) {
   wl_send(KBD, 1, b_u32(b_u32(b_u32([], now32()), code), down))
   wl_pump(0.02)
}

fn seat_type(str text) {
   mut i = 0
   while i < text.len {
      def str c = to_str(slice(text, i, i + 1))
      def b = load8(c, 0)
      def upper = b >= 65 && b <= 90
      def mapped = to_str(KEYS_SHIFTED.get(c, ""))
      def base = mapped.len > 0 ? mapped : lower_ascii(c)
      def shift = upper || mapped.len > 0
      def code = int(KEYS.get(base, 0))
      if code == 0 { die("no key for '" + c + "'") }
      if shift { wl_send(KBD, 1, b_u32(b_u32(b_u32([], now32()), 42), 1)) }
      seat_key(code, shift ? 1 : 0)
      if shift { wl_send(KBD, 1, b_u32(b_u32(b_u32([], now32()), 42), 0)) }
      i += 1
   }
}

fn seat_move(int x, int y) {
   wl_send(PTR, 1, b_u32(b_u32(b_u32(b_u32(b_u32([], now32()), x), y), OUT_W), OUT_H))
   wl_send(PTR, 4, [])
   wl_pump(0.05)
}

fn seat_click(int x, int y) {
   seat_move(x, y)
   wl_send(PTR, 2, b_u32(b_u32(b_u32([], now32()), 0x110), 1))
   wl_send(PTR, 4, [])
   wl_pump(0.05)
   wl_send(PTR, 2, b_u32(b_u32(b_u32([], now32()), 0x110), 0))
   wl_send(PTR, 4, [])
   wl_pump(0.1)
}

;; ---------------------------------------------------------------------- ime
fn ime_finish() {
   wl_send(IM, 3, b_u32([], IM_SERIAL))
   wl_pump(0.1)
}

fn ime_preedit(str text, int cursor) {
   mut b = b_str([], text)
   b = b_u32(b, cursor)
   b = b_u32(b, cursor)
   wl_send(IM, 1, b)
   ime_finish()
}

fn ime_commit(str text) {
   wl_send(IM, 0, b_str([], text))
   ime_finish()
}

fn ime_delete(int before, int after) {
   wl_send(IM, 2, b_u32(b_u32([], before), after))
   ime_finish()
}

;; --------------------------------------------------------------- step script
fn partition_space(str s) list {
   mut i = 0
   while i < s.len {
      if load8(s, i) == 32 { return [slice(s, 0, i), slice(s, i + 1, s.len)] }
      i += 1
   }
   [s, ""]
}

fn step_num(list args, int i) int {
   int(sx.atof(to_str(args.get(i, "0"))))
}

fn run_step(str line) {
   def parts = partition_space(line)
   def cmd = to_str(parts.get(0, ""))
   def rest = to_str(parts.get(1, ""))
   def args = rest.len > 0 ? sx.split(rest, " ") : []
   if cmd == "wait_file" {
      def end = now_ms() + 120000
      while !file_exists(rest) && now_ms() < end { wl_pump(0.1) }
      if !file_exists(rest) { die("timed out waiting for " + rest) }
   } elif cmd == "move" {
      seat_move(step_num(args, 0), step_num(args, 1))
   } elif cmd == "click" {
      seat_click(step_num(args, 0), step_num(args, 1))
   } elif cmd == "type" {
      seat_type(rest)
   } elif cmd == "key" {
      seat_key(step_num(args, 0), 0)
   } elif cmd == "wait_active" {
      def end = now_ms() + 30000
      while !IM_ACTIVE && now_ms() < end { wl_pump(0.1) }
      if !IM_ACTIVE { die("the focused client never enabled text input") }
   } elif cmd == "preedit" {
      def cursor = step_num(args, args.len - 1)
      mut text = ""
      mut k = 0
      while k < args.len - 1 {
         text = k == 0 ? to_str(args.get(0, "")) : text + " " + to_str(args.get(k, ""))
         k += 1
      }
      ime_preedit(text, cursor)
   } elif cmd == "commit" {
      ime_commit(rest)
   } elif cmd == "delete" {
      ime_delete(step_num(args, 0), step_num(args, 1))
   } elif cmd == "cancel" {
      ime_preedit("", 0)
   } elif cmd == "keydown" {
      seat_key_state(step_num(args, 0), 1)
   } elif cmd == "keyup" {
      seat_key_state(step_num(args, 0), 0)
   } elif cmd == "combo" {
      mut k = 0
      while k < args.len { seat_key_state(step_num(args, k), 1) k += 1 }
      wl_pump(0.03)
      k = args.len - 1
      while k >= 0 { seat_key_state(step_num(args, k), 0) k -= 1 }
      wl_pump(0.05)
   } elif cmd == "sleep" {
      wl_pump(float(sx.atof(rest)))
   } elif cmd == "log" {
      print(rest)
   } else {
      die("unknown step: " + line)
   }
}

;; --------------------------------------------------------------------- modes
fn run_main(str script) int {
   def path = runtime_path()
   if path == "" { die("WAYLAND_DISPLAY is not set") }
   if !wl_open(path) { die("cannot connect to " + path) }
   keys_init()
   seat_init()
   def rd = file_read(script)
   if !is_ok(rd) { die("cannot read script " + script) }
   def lines = sx.split(to_str(unwrap(rd)), "\n")
   mut i = 0
   while i < lines.len {
      def line = sx.strip(lines.get(i, ""))
      if line.len > 0 && load8(line, 0) != 35 { run_step(line) }
      i += 1
   }
   wl_pump(0.3)
   print("DRIVER-DONE")
   0
}

fn probe_main() int {
   def path = runtime_path()
   if path == "" { return 3 }
   if !wl_open(path) { return 3 }
   mut missing = []
   mut i = 0
   while i < NEED.len {
      def iface = to_str(NEED.get(i, ""))
      if !has_global(iface) { missing = missing.append(iface) }
      i += 1
   }
   if missing.len == 0 {
      print("seat driver: ok")
      return 0
   }
   mut text = "missing: "
   mut j = 0
   while j < missing.len {
      text = text + (j == 0 ? "" : ",") + to_str(missing.get(j, ""))
      j += 1
   }
   print(text)
   3
}

;; Exercises the framing, string padding and key tables without a compositor.
fn selftest() int {
   def header = msg_bytes(5, 3, [0xAA, 0xBB])
   def hp = pack(header)
   if load32(hp, 0) != 5 { die("selftest: object id") }
   if load32(hp, 4) != ((10 << 16) | 3) { die("selftest: header word") }
   if load8(hp, 8) != 0xAA || load8(hp, 9) != 0xBB { die("selftest: payload") }
   free(hp)
   def framed = b_str([], "ab")
   def fp = pack(framed)
   if framed.len != 8 { die("selftest: string padding") }
   if load32(fp, 0) != 3 { die("selftest: string length") }
   if load8(fp, 4) != 97 || load8(fp, 5) != 98 || load8(fp, 6) != 0 { die("selftest: string bytes") }
   free(fp)
   RX = malloc(64)
   mem.memset(RX, 0, 64)
   store32(RX, 4, 0)
   store8(RX, 119, 4)
   store8(RX, 108, 5)
   store8(RX, 108, 6)
   store8(RX, 0, 7)
   if !rx_str_is(0, "wll") { die("selftest: registry string compare") }
   if rx_str_is(0, "wl_seat") { die("selftest: registry length compare") }
   free(RX)
   RX = 0
   keys_init()
   if int(KEYS.get("q", 0)) != 16 { die("selftest: key q") }
   if int(KEYS.get("m", 0)) != 50 { die("selftest: key m") }
   if int(KEYS.get(" ", 0)) != 57 { die("selftest: key space") }
   if to_str(KEYS_SHIFTED.get("!", "")) != "1" { die("selftest: shifted key") }
   if lower_ascii("Q") != "q" || lower_ascii("7") != "7" { die("selftest: ascii lower") }
   print("seat driver: selftest ok")
   0
}

;; ------------------------------------------------------------------- ytool
;; One front door for both display servers: the X11 command set below and the
;; Wayland virtual-seat driver. `ytool probe` reports both; input commands use
;; X11 when a display is reachable and otherwise replay through the seat.
mut YNAMED = {}

fn yt_lower(str s) str {
   mut out = ""
   mut i = 0
   while i < s.len {
      out = out + lower_ascii(to_str(slice(s, i, i + 1)))
      i += 1
   }
   out
}

fn yt_keys() {
   keys_init()
   if YNAMED.len > 0 { return }
   YNAMED["return"] = 28
   YNAMED["enter"] = 28
   YNAMED["tab"] = 15
   YNAMED["escape"] = 1
   YNAMED["esc"] = 1
   YNAMED["backspace"] = 14
   YNAMED["space"] = 57
   YNAMED["delete"] = 111
   YNAMED["insert"] = 110
   YNAMED["home"] = 102
   YNAMED["end"] = 107
   YNAMED["left"] = 105
   YNAMED["right"] = 106
   YNAMED["up"] = 103
   YNAMED["down"] = 108
   YNAMED["pageup"] = 104
   YNAMED["pagedown"] = 109
   YNAMED["ctrl"] = 29
   YNAMED["control"] = 29
   YNAMED["shift"] = 42
   YNAMED["alt"] = 56
   YNAMED["super"] = 125
   YNAMED["meta"] = 125
   YNAMED["win"] = 125
}

fn yt_evdev(str name) int {
   def low = yt_lower(name)
   yt_keys()
   def named = int(YNAMED.get(low, 0))
   if named != 0 { return named }
   if low.len == 1 {
      def mapped = to_str(KEYS_SHIFTED.get(low, ""))
      def base = mapped.len > 0 ? mapped : low
      return int(KEYS.get(base, 0))
   }
   0
}

fn yt_combo_line(str seq) str {
   def parts = sx.split(seq, "+")
   mut codes = []
   mut shift = false
   mut need_shift = false
   mut i = 0
   while i < parts.len {
      def one = to_str(parts.get(i, ""))
      def low = yt_lower(one)
      if low == "shift" { shift = true }
      if one.len == 1 && to_str(KEYS_SHIFTED.get(low, "")).len > 0 { need_shift = true }
      def code = yt_evdev(one)
      if code == 0 { die("unknown key: " + one) }
      codes = codes.append(code)
      i += 1
   }
   mut all = []
   if need_shift && !shift { all = all.append(42) }
   i = 0
   while i < codes.len { all = all.append(int(codes.get(i, 0))) i += 1 }
   mut out = "combo"
   i = 0
   while i < all.len { out = out + " " + to_str(int(all.get(i, 0))) i += 1 }
   out
}

fn yt_has_wayland() bool { runtime_path().len > 0 }

fn yt_x11_ok() bool {
   def d = XOpenDisplay(0)
   if !d { return false }
   XCloseDisplay(d)
   true
}

fn yt_script(str body) int {
   def path = temp_dir() + "/ytool-steps-" + to_str(now_ms()) + ".txt"
   def wr = file_write(path, body + "\n")
   if !is_ok(wr) { die("cannot write the step script") }
   run_main(path)
}

fn yt_usage() {
   print("ytool 0.1 - Wayland + X11 input tool")
   print("")
   print("Backends:")
   print("   probe                      Report Wayland protocols and X11")
   print("   env                        Show session backends")
   print("   selftest                   Check the seat driver tables")
   print("   run SCRIPT                 Replay a Wayland step script")
   print("   wayland <cmd>              Force the Wayland backend")
   print("   x11 <cmd>                  Force the X11 command set")
   print("")
   print("Wayland input (also used automatically without X11):")
   print("   type TEXT                  Type text through the virtual keyboard")
   print("   key SEQ                    Press a combo, e.g. alt+Tab or ctrl+l")
   print("   move X Y                   Move the virtual pointer")
   print("   click X Y                  Move and click")
   print("")
   print("X11 commands: run `ytool x11 help`.")
}

fn yt_probe() int {
   def xok = yt_x11_ok()
   print("x11: " + (xok ? "ok" : "unavailable"))
   def wl = probe_main()
   if wl == 0 || xok { return 0 }
   3
}

fn yt_selftest() int {
   def rc = selftest()
   print("x11: " + (yt_x11_ok() ? "ok" : "unavailable"))
   rc
}

fn yt_env() int {
   def wd = common.env_trim("WAYLAND_DISPLAY")
   def xd = common.env_trim("XDG_RUNTIME_DIR")
   def dd = common.env_trim("DISPLAY")
   print("WAYLAND_DISPLAY: " + (wd.len > 0 ? wd : "(unset)"))
   print("XDG_RUNTIME_DIR: " + (xd.len > 0 ? xd : "(unset)"))
   print("DISPLAY:         " + (dd.len > 0 ? dd : "(unset)"))
   print("wayland: " + (yt_has_wayland() ? "socket ready" : "unavailable"))
   print("x11:     " + (yt_x11_ok() ? "ok" : "unavailable"))
   0
}

fn yt_wayland_dispatch(int at) int {
   if at >= argc() {
      eprint("wayland: expected a command (probe|selftest|run|type|key|move|click)")
      return 2
   }
   def sub = str(argv(at))
   if sub == "probe" { return yt_probe() }
   if sub == "selftest" { return yt_selftest() }
   if sub == "run" {
      if at + 1 >= argc() { eprint("run requires SCRIPT") return 2 }
      return run_main(str(argv(at + 1)))
   }
   if sub == "type" {
      mut text = ""
      mut i = at + 1
      while i < argc() {
         text = text + (i == at + 1 ? "" : " ") + str(argv(i))
         i += 1
      }
      if text.len == 0 { eprint("type requires TEXT") return 2 }
      return yt_script("type " + text)
   }
   if sub == "key" {
      if at + 1 >= argc() { eprint("key requires SEQ") return 2 }
      return yt_script(yt_combo_line(str(argv(at + 1))))
   }
   if sub == "move" || sub == "mousemove" {
      if at + 2 >= argc() { eprint("move requires X Y") return 2 }
      return yt_script("move " + str(argv(at + 1)) + " " + str(argv(at + 2)))
   }
   if sub == "click" {
      if at + 2 >= argc() { eprint("click requires X Y") return 2 }
      return yt_script("click " + str(argv(at + 1)) + " " + str(argv(at + 2)))
   }
   if sub == "sleep" {
      if at + 1 >= argc() { eprint("sleep requires SECONDS") return 2 }
      return yt_script("sleep " + str(argv(at + 1)))
   }
   eprint("wayland: unknown command " + sub)
   2
}

mut arg_offset = 0
if argc() > 1 && (str(argv(1)) == "x11" || str(argv(1)) == "x") { arg_offset = 1 }
if argc() > 1 && xt_arg(1) == "--" { arg_offset = 1 }
fn xt_arg(int index) str { str(argv(index + arg_offset)) }
def nargs = argc() - arg_offset
mut cmd = nargs > 1 ? xt_arg(1) : ""

;; --------------------------------------------------------------- ytool door
if cmd == "probe" { exit(yt_probe()) }
if cmd == "selftest" { exit(yt_selftest()) }
if cmd == "env" || cmd == "doctor" { exit(yt_env()) }
if cmd == "run" {
   if nargs < 3 { eprint("run requires SCRIPT") exit(2) }
   exit(run_main(xt_arg(2)))
}
if cmd == "wayland" || cmd == "wl" { exit(yt_wayland_dispatch(2 + arg_offset)) }
;; No reachable X server: route pointer/keyboard commands to the seat driver.
if cmd == "type" || cmd == "key" || cmd == "mousemove" || cmd == "click" {
   if !yt_x11_ok() {
      if !yt_has_wayland() {
         eprint("no X11 display and WAYLAND_DISPLAY is not set")
         exit(1)
      }
      exit(yt_wayland_dispatch(1 + arg_offset))
   }
}

if(cmd == "version" || cmd == "--version") {
   print("ytool 0.1")
   exit(0)
}

if(nargs == 1 || cmd == "help" || cmd == "--help") {
   print_usage()
   exit(0)
}

; Command aliases
if(cmd == "ls" || cmd == "winlist") { cmd = "listwindows" }
if(cmd == "mv" || cmd == "mm") { cmd = "mousemove" }
if(cmd == "mvrel" || cmd == "mmrel") { cmd = "mousemove_relative" }
if(cmd == "mvsm" || cmd == "mmsm") { cmd = "mousemove_smooth" }
if(cmd == "mvre" || cmd == "mmre") { cmd = "mousemove_relative_smooth" }
if(cmd == "loc" || cmd == "where") { cmd = "getmouselocation" }
if(cmd == "aw" || cmd == "active") { cmd = "getactivewindow" }
if(cmd == "gf" || cmd == "focus") { cmd = "getwindowfocus" }
if(cmd == "gw" || cmd == "geom") { cmd = "getwindowgeometry" }
if(cmd == "wn" || cmd == "name") { cmd = "getwindowname" }
if(cmd == "cls") { cmd = "getwindowclassname" }
if(cmd == "sel") { cmd = "selectwindow" }
if(cmd == "wf" || cmd == "focusw") { cmd = "windowfocus" }
if(cmd == "wa" || cmd == "activate") { cmd = "windowactivate" }
if(cmd == "ws") { cmd = "windowsize" }
if(cmd == "wm" || cmd == "movew") { cmd = "windowmove" }
if(cmd == "map") { cmd = "windowmap" }
if(cmd == "unmap") { cmd = "windowunmap" }
if(cmd == "min") { cmd = "windowminimize" }
if(cmd == "raise") { cmd = "windowraise" }
if(cmd == "lower") { cmd = "windowlower" }
if(cmd == "close") { cmd = "windowclose" }
if(cmd == "kill") { cmd = "windowkill" }
if(cmd == "sc") { cmd = "search" }
if(cmd == "dg" || cmd == "disp" || cmd == "screen") { cmd = "getdisplaygeometry" }
if(cmd == "nd" || cmd == "desks") { cmd = "get_num_desktops" }
if(cmd == "cd" || cmd == "desk") { cmd = "get_desktop" }
if(cmd == "sd") { cmd = "set_desktop" }
if(cmd == "gdw") { cmd = "get_desktop_for_window" }
if(cmd == "sdw") { cmd = "set_desktop_for_window" }
if(cmd == "fw" || cmd == "find") { cmd = "find-window" }
if(cmd == "ww") { cmd = "wait-window" }
if(cmd == "awid" || cmd == "activeid") { cmd = "active-window-id" }
if(cmd == "rk" || cmd == "repeat") { cmd = "repeat-key" }
if(cmd == "mw" || cmd == "movewin") { cmd = "move-window" }
if(cmd == "cw") { cmd = "click-window" }
if(cmd == "wg") { cmd = "window-geom" }
if(cmd == "ws" || cmd == "wstate") { cmd = "wm-state" }
if(cmd == "fs" || cmd == "fullscreen") { cmd = "fullscreen-window" }
if(cmd == "mx" || cmd == "maximize") { cmd = "maximize-window" }
if(cmd == "sl") { cmd = "sleep" }
if(cmd == "wq" || cmd == "close") { cmd = "windowquit" }
if(cmd == "snd" || cmd == "numdesktops") { cmd = "set_num_desktops" }
if(cmd == "gdv" || cmd == "getviewport") { cmd = "get_desktop_viewport" }
if(cmd == "sdv" || cmd == "setviewport") { cmd = "set_desktop_viewport" }

display = XOpenDisplay(0)
if(!display) {
   eprint("Cannot open display. Is X11 running?")
   exit(1)
}
root = xt_xroot_window(display, xt_xdefault_screen(display))
XSync(display, 0)
XFlush(display)

mut code = 0

if(cmd == "mousemove" || cmd == "mousemove_relative") {
   if(nargs < 4) {
      eprint(cmd + " requires x y")
      code = 1
   } else {
      def x = parse_int(xt_arg(2))
      def y = parse_int(xt_arg(3))
      def pos = mouse_pos()
      def cx = int(pos.get(0, 0))
      def cy = int(pos.get(1, 0))
      if(cmd == "mousemove") {
         XTestFakeMotionEvent(display, 0, x, y, 0)
      } else {
         XTestFakeMotionEvent(display, 0, cx + x, cy + y, 0)
      }
      XFlush(display)
   }
} elif(cmd == "mousemove_smooth" || cmd == "mousemove_relative_smooth") {
   if(nargs < 4) {
      eprint(cmd + " requires x y")
      code = 1
   } else {
      def x = parse_int(xt_arg(2))
      def y = parse_int(xt_arg(3))
      def pos = mouse_pos()
      def cx = int(pos.get(0, 0))
      def cy = int(pos.get(1, 0))
      def abs_cmd = !cmd.contains("relative")
      def tx = abs_cmd ? x : cx + x
      def ty = abs_cmd ? y : cy + y
      mouse_move_to(tx, ty, cx, cy, 100)
   }
} elif(cmd == "click" || cmd == "mousedown" || cmd == "mouseup") {
   mut arg_idx = 2
   mut win = 0
   while(arg_idx < nargs && xt_arg(arg_idx) == "--window") {
      if(arg_idx + 1 >= nargs) {
         eprint("--window requires a window ID")
         code = 1
         break
      }
      win = parse_int(xt_arg(arg_idx + 1))
      arg_idx += 2
   }
   if(win && code == 0) {
      def g = window_geom(win)
      def cx = int(g.get(0, 0)) + int(g.get(2, 0)) / 2
      def cy = int(g.get(1, 0)) + int(g.get(3, 0)) / 2
      XTestFakeMotionEvent(display, 0, cx, cy, 0)
      XFlush(display)
   }
   if(code == 0) {
      def btn = nargs > arg_idx ? parse_int(xt_arg(arg_idx)) : 1
      if(cmd == "click") {
         btn_event(btn, true)
         msleep(10)
         btn_event(btn, false)
      } elif(cmd == "mousedown") {
         btn_event(btn, true)
      } else {
         btn_event(btn, false)
      }
   }
} elif(cmd == "key" || cmd == "keydown" || cmd == "keyup") {
   mut arg_idx = 2
   mut win = 0
   while(arg_idx < nargs && xt_arg(arg_idx) == "--window") {
      if(arg_idx + 1 >= nargs) {
         eprint("--window requires a window ID")
         code = 1
         break
      }
      win = parse_int(xt_arg(arg_idx + 1))
      arg_idx += 2
   }
   if(arg_idx >= nargs && code == 0) {
      eprint(cmd + " requires a key sequence")
      code = 1
   }
   if(win && code == 0) {
      def g = window_geom(win)
      def cx = int(g.get(0, 0)) + int(g.get(2, 0)) / 2
      def cy = int(g.get(1, 0)) + int(g.get(3, 0)) / 2
      XTestFakeMotionEvent(display, 0, cx, cy, 0)
      XFlush(display)
   }
   if(code == 0) {
      def is_press = cmd != "keyup"
      def do_release = cmd == "key"
      send_keysequence(xt_arg(arg_idx), is_press)
      if(do_release) {
         msleep(20)
         send_keysequence(xt_arg(arg_idx), false)
      }
   }
} elif(cmd == "combo") {
   if(nargs < 3) {
      eprint("combo requires a key sequence")
      code = 1
   } else {
      def c = parse_combo(xt_arg(2))
      send_combo_key(c.get(0, ""), c.get(1, false), c.get(2, false), c.get(3, false))
   }
} elif(cmd == "type") {
   mut delay = 10
   mut win = 0
   mut arg_idx = 2
   while(arg_idx < nargs && (xt_arg(arg_idx) == "--delay" || xt_arg(arg_idx) == "--window")) {
      if(xt_arg(arg_idx) == "--delay") {
         if(arg_idx + 1 >= nargs) {
            eprint("--delay requires a value")
            code = 1
            break
         }
         delay = parse_int(xt_arg(arg_idx + 1))
         arg_idx += 2
      } elif(xt_arg(arg_idx) == "--window") {
         if(arg_idx + 1 >= nargs) {
            eprint("--window requires a window ID")
            code = 1
            break
         }
         win = parse_int(xt_arg(arg_idx + 1))
         arg_idx += 2
      }
   }
   if(win && code == 0) {
      def g = window_geom(win)
      def cx = int(g.get(0, 0)) + int(g.get(2, 0)) / 2
      def cy = int(g.get(1, 0)) + int(g.get(3, 0)) / 2
      XTestFakeMotionEvent(display, 0, cx, cy, 0)
      XFlush(display)
   }
   if(arg_idx < nargs && code == 0) {
      type_string(xt_arg(arg_idx), delay)
   } elif(code == 0) {
      eprint("type requires a string")
      code = 1
   }
} elif(cmd == "getmouselocation") {
   def root_ret = zalloc(8)
   def child_ret = zalloc(8)
   def rx = zalloc(4)
   def ry = zalloc(4)
   def wx = zalloc(4)
   def wy = zalloc(4)
   def mask = zalloc(4)
   XQueryPointer(display, root, root_ret, child_ret, rx, ry, wx, wy, mask)
   def x = load32(rx, 0)
   def y = load32(ry, 0)
   def child = load64(child_ret, 0)
   free(root_ret, child_ret, rx, ry, wx, wy, mask)
   mut shell_mode = false
   mut prefix = ""
   mut arg_idx = 2
   while(arg_idx < nargs) {
      if(xt_arg(arg_idx) == "--shell") {
         shell_mode = true
         arg_idx += 1
         continue
      }
      if(xt_arg(arg_idx) == "--prefix") {
         if(arg_idx + 1 >= nargs) {
            eprint("--prefix requires a value")
            code = 1
            break
         }
         shell_mode = true
         prefix = xt_arg(arg_idx + 1)
         arg_idx += 2
         continue
      }
      arg_idx += 1
   }
   if(shell_mode) {
      print(prefix + "X=" + to_str(int(x)))
      print(prefix + "Y=" + to_str(int(y)))
      print(prefix + "SCREEN=0")
      print(prefix + "WINDOW=" + to_str(int(child)))
   } else {
      print("x:" + to_str(int(x)) + " y:" + to_str(int(y)) + " screen:0 window:" + to_str(int(child)))
   }
} elif(cmd == "getactivewindow") {
   def win = active_window_ptr()
   print(to_str(int(win)))
} elif(cmd == "getwindowfocus") {
   def win = get_window_focus()
   print(to_str(int(win)))
} elif(cmd == "getwindowgeometry") {
   mut shell_mode = false
   mut prefix = ""
   mut arg_idx = 2
   while(arg_idx < nargs && xt_arg(arg_idx) == "--shell") {
      shell_mode = true
      arg_idx += 1
   }
   while(arg_idx < nargs && xt_arg(arg_idx) == "--prefix") {
      if(arg_idx + 1 >= nargs) {
         eprint("--prefix requires a value")
         code = 1
         break
      }
      prefix = xt_arg(arg_idx + 1)
      arg_idx += 2
   }
   if(arg_idx >= nargs && code == 0) {
      eprint("getwindowgeometry requires a window ID")
      code = 1
   } elif(code == 0) {
      def win = parse_int(xt_arg(arg_idx))
      print_window_geom(win, shell_mode, prefix)
   }
} elif(cmd == "getwindowname") {
   if(nargs < 3) {
      eprint("getwindowname requires a window ID")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      getwindowname(win)
   }
} elif(cmd == "getwindowclassname") {
   if(nargs < 3) {
      eprint("getwindowclassname requires a window ID")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      print(get_window_class(win))
   }
} elif(cmd == "getwindowpid") {
   if(nargs < 3) {
      eprint("getwindowpid requires a window ID")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      def pid = read_card_prop("_NET_WM_PID", win)
      if(pid) {
         print(to_str(pid))
      } else {
         eprint("No PID found for window")
         code = 1
      }
   }
} elif(cmd == "getdisplaygeometry") {
   def w = XDisplayWidth(display, 0)
   def h = XDisplayHeight(display, 0)
   mut shell_mode = false
   mut prefix = ""
   mut arg_idx = 2
   while(arg_idx < nargs) {
      if(xt_arg(arg_idx) == "--shell") {
         shell_mode = true
         arg_idx += 1
         continue
      }
      if(xt_arg(arg_idx) == "--prefix") {
         if(arg_idx + 1 >= nargs) {
            eprint("--prefix requires a value")
            code = 1
            break
         }
         shell_mode = true
         prefix = xt_arg(arg_idx + 1)
         arg_idx += 2
         continue
      }
      arg_idx += 1
   }
   if(shell_mode) {
      print(prefix + "WIDTH=" + to_str(int(w)))
      print(prefix + "HEIGHT=" + to_str(int(h)))
   } else {
      print(to_str(int(w)) + " " + to_str(int(h)))
   }
} elif(cmd == "get_num_desktops") {
   def n = read_card_prop("_NET_NUMBER_OF_DESKTOPS", root)
   print(to_str(n > 0 ? n : 1))
} elif(cmd == "get_desktop") {
   def d = read_card_prop("_NET_CURRENT_DESKTOP", root)
   print(to_str(d))
} elif(cmd == "set_desktop") {
   if(nargs < 3) {
      eprint("set_desktop requires a desktop number")
      code = 1
   } else {
      def num = parse_int(xt_arg(2))
      send_client_message(root, "_NET_CURRENT_DESKTOP", num, 0)
   }
} elif(cmd == "get_desktop_for_window") {
   if(nargs < 3) {
      eprint("get_desktop_for_window requires a window ID")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      def d = read_card_prop("_NET_WM_DESKTOP", win)
      print(to_str(d))
   }
} elif(cmd == "set_desktop_for_window") {
   if(nargs < 4) {
      eprint("set_desktop_for_window requires window ID and desktop number")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      def num = parse_int(xt_arg(3))
      send_client_message(win, "_NET_WM_DESKTOP", num, 1)
   }
} elif(cmd == "selectwindow") {
   def win = selectwindow()
   if(win) {
      print(to_str(int(win)))
   } else {
      eprint("No window selected")
      code = 1
   }
} elif(cmd == "windowfocus" || cmd == "windowactivate") {
   if(nargs < 3) {
      eprint(cmd + " requires window ID")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      if(cmd == "windowactivate") {
         def wm_desk = read_card_prop("_NET_WM_DESKTOP", win)
         def cur_desk = read_card_prop("_NET_CURRENT_DESKTOP", root)
         if(wm_desk > 0 && int(wm_desk) != int(cur_desk)) {
            send_client_message(root, "_NET_CURRENT_DESKTOP", int(wm_desk), 0)
            msleep(50)
         }
         def a = atom_val("_NET_ACTIVE_WINDOW")
         def ev = zalloc(192)
         store32(ev, 33, 0)
         store64(ev, win, 32)
         store64(ev, a, 40)
         store32(ev, 32, 48)
         store64(ev, 2, 56)
   xt_xsend_event(display, root, 0, 557056, ev)
         XFlush(display)
         free(ev)
      }
      xt_xset_input_focus(display, win, 0, 0)
      xt_xraise_window(display, win)
      XFlush(display)
   }
} elif(cmd == "windowsize") {
   if(nargs < 5) {
      eprint("windowsize requires window ID, width, height")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      def w = parse_int(xt_arg(3))
      def h = parse_int(xt_arg(4))
      xt_xresize_window(display, win, w, h)
      XFlush(display)
   }
} elif(cmd == "windowmove") {
   if(nargs < 5) {
      eprint("windowmove requires window ID, x, y")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      def x = parse_int(xt_arg(3))
      def y = parse_int(xt_arg(4))
      xt_xmove_window(display, win, x, y)
      XFlush(display)
   }
} elif(cmd == "windowmap") {
   if(nargs < 3) {
      eprint("windowmap requires window ID")
      code = 1
   } else {
      xt_xmap_window(display, parse_int(xt_arg(2)))
      XFlush(display)
   }
} elif(cmd == "windowunmap") {
   if(nargs < 3) {
      eprint("windowunmap requires window ID")
      code = 1
   } else {
      xt_xunmap_window(display, parse_int(xt_arg(2)))
      XFlush(display)
   }
} elif(cmd == "windowminimize") {
   if(nargs < 3) {
      eprint("windowminimize requires window ID")
      code = 1
   } else {
      xt_xiconify_window(display, parse_int(xt_arg(2)), 0)
      XFlush(display)
   }
} elif(cmd == "windowraise") {
   if(nargs < 3) {
      eprint("windowraise requires window ID")
      code = 1
   } else {
      xt_xraise_window(display, parse_int(xt_arg(2)))
      XFlush(display)
   }
} elif(cmd == "windowlower") {
   if(nargs < 3) {
      eprint("windowlower requires window ID")
      code = 1
   } else {
      xt_xlower_window(display, parse_int(xt_arg(2)))
      XFlush(display)
   }
} elif(cmd == "windowclose") {
   if(nargs < 3) {
      eprint("windowclose requires window ID")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      def a = atom_val("_NET_CLOSE_WINDOW")
      def ev = zalloc(192)
      store32(ev, 33, 0)
      store64(ev, win, 32)
      store64(ev, a, 40)
      store32(ev, 32, 48)
      xt_xsend_event(display, win, 0, 0, ev)
      XFlush(display)
      free(ev)
   }
} elif(cmd == "windowkill") {
   if(nargs < 3) {
      eprint("windowkill requires window ID")
      code = 1
   } else {
      xt_xkill_client(display, parse_int(xt_arg(2)))
      XFlush(display)
   }
} elif(cmd == "search") {
   if(nargs < 3) {
      eprint("search requires a string")
      code = 1
   } else {
      search_windows(xt_arg(2))
   }
} elif(cmd == "listwindows") {
   if(list_windows() != 0) { code = 1 }
} elif(cmd == "inputscan") {
   inputscan_loop()
} elif(cmd == "fakemouse") {
   mut use_wasd = false
   mut arg_idx = 2
   while(arg_idx < nargs) {
      if(xt_arg(arg_idx) == "--wasd" || xt_arg(arg_idx) == "-w") {
         use_wasd = true
      }
      arg_idx += 1
   }
   fakemouse_loop(use_wasd)
} elif(cmd == "find-window") {
   def needle = nargs > 2 ? xt_arg(2) : "Visual Studio Code"
   def win = find_window(needle, true)
   if(win) {
      print(to_str(int(win)))
   } else {
      eprint("No matching window found")
      code = 1
   }
} elif(cmd == "wait-window") {
   if(nargs < 3) {
      eprint("wait-window requires timeout (seconds)")
      code = 1
   } else {
      def timeout_secs = parse_int(xt_arg(2))
      def needle = nargs > 3 ? xt_arg(3) : "Visual Studio Code"
      def win = wait_window(needle, timeout_secs * 1000)
      if(win) {
         print(to_str(int(win)))
      } else {
         eprint("Window not found within timeout")
         code = 1
      }
   }
} elif(cmd == "active-window-id") {
   def win = active_window_ptr()
   print(to_str(int(win)))
} elif(cmd == "repeat-key") {
   if(nargs < 4) {
      eprint("repeat-key requires count and key name")
      code = 1
   } else {
      def count = parse_int(xt_arg(2))
      def key_name = xt_arg(3)
      mut delay = 20
      mut idx = 4
      while(idx < nargs && xt_arg(idx) == "--delay") {
         if(idx + 1 >= nargs) {
            eprint("--delay requires a value")
            code = 1
            break
         }
         delay = parse_int(xt_arg(idx + 1))
         idx += 2
      }
      if(code == 0) {
         mut j = 0
         while(j < count) {
            send_keysequence(key_name, true)
            msleep(delay)
            send_keysequence(key_name, false)
            if(j < count - 1) { msleep(delay) }
            j += 1
         }
      }
   }
} elif(cmd == "move-window" || cmd == "click-window") {
   if(nargs < 5) {
      eprint(cmd + " requires window ID, rel_x, rel_y")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      def rx = parse_int(xt_arg(3))
      def ry = parse_int(xt_arg(4))
      def g = window_geom(win)
      def ax = int(g.get(0, 0)) + rx
      def ay = int(g.get(1, 0)) + ry
      XTestFakeMotionEvent(display, 0, ax, ay, 0)
      XFlush(display)
      if(cmd == "click-window") {
         def btn = nargs > 5 ? parse_int(xt_arg(5)) : 1
         btn_event(btn, true)
         msleep(10)
         btn_event(btn, false)
      }
   }
} elif(cmd == "window-geom") {
   if(nargs < 3) {
      eprint("window-geom requires a window ID")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      def g = window_geom(win)
      print(to_str(g.get(0, 0)) + " " + to_str(g.get(1, 0)) + " " + to_str(g.get(2, 0)) + " " + to_str(g.get(3, 0)))
   }
} elif(cmd == "wm-state") {
   def win = nargs > 2 ? parse_int(xt_arg(2)) : active_window_ptr()
   if(!win) {
      eprint("No window specified and no active window")
      code = 1
   }
   else {
      def states = wm_state_names(win)
      mut i = 0
      while(i < states.len) {
         print(states.get(i, ""))
         i += 1
      }
   }
} elif(cmd == "fullscreen-window") {
   if(nargs < 3) {
      eprint("fullscreen-window requires a window ID")
      code = 1
   } else {
      set_wm_state(parse_int(xt_arg(2)), "_NET_WM_STATE_FULLSCREEN", "", 1)
   }
} elif(cmd == "maximize-window") {
   if(nargs < 3) {
      eprint("maximize-window requires a window ID")
      code = 1
   } else {
      set_wm_state(parse_int(xt_arg(2)), "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ", 1)
   }
} elif(cmd == "sleep") {
   if(nargs < 3) {
      eprint("sleep requires a duration in seconds")
      code = 1
   } else {
      msleep(parse_int(xt_arg(2)) * 1000)
   }
} elif(cmd == "exec") {
   if(nargs < 3) {
      eprint("exec requires a command")
      code = 1
   } else {
      mut cmd_str = xt_arg(2)
      mut i = 3
      while(i < nargs) {
         cmd_str = cmd_str + " " + xt_arg(i)
         i += 1
      }
      def ret = c_system(cstr(cmd_str))
      if(ret != 0) { code = 1 }
   }
} elif(cmd == "windowquit") {
   if(nargs < 3) {
      eprint("windowquit requires a window ID")
      code = 1
   } else {
      def win = parse_int(xt_arg(2))
      def a = atom_val("WM_PROTOCOLS")
      def del_atom = atom_val("WM_DELETE_WINDOW")
      def ev = zalloc(192)
      store32(ev, 33, 0)
      store64(ev, win, 32)
      store64(ev, a, 40)
      store32(ev, 32, 48)
      store64(ev, del_atom, 56)
      xt_xsend_event(display, win, 0, 0, ev)
      XFlush(display)
      free(ev)
   }
} elif(cmd == "set_num_desktops") {
   if(nargs < 3) {
      eprint("set_num_desktops requires a number")
      code = 1
   } else {
      send_client_message(root, "_NET_NUMBER_OF_DESKTOPS", parse_int(xt_arg(2)), 0)
   }
} elif(cmd == "get_desktop_viewport") {
   def a = atom_val("_NET_DESKTOP_VIEWPORT")
   def prop = zalloc(8)
   def act = zalloc(8)
   def fmt = zalloc(4)
   def nitems = zalloc(8)
   def after = zalloc(8)
   def ok = xt_xget_window_property(display, root, a, 0, 2, 0, 6, act, fmt, nitems, after, prop)
   if(ok == 0) {
      def p = load64(prop, 0)
      def n = load64(nitems, 0)
      def f = load32(fmt, 0)
      if(p && n > 0 && f == 32) {
         def vx = load32(p, 0)
         def vy = load32(p, 8)
         mut shell_mode = false
         mut prefix = ""
         mut arg_idx = 2
         while(arg_idx < nargs) {
            if(xt_arg(arg_idx) == "--shell") {
               shell_mode = true
               arg_idx += 1
            } elif(xt_arg(arg_idx) == "--prefix") {
               if(arg_idx + 1 >= nargs) {
                  eprint("--prefix requires a value")
                  code = 1
                  break
               }
               shell_mode = true
               prefix = xt_arg(arg_idx + 1)
               arg_idx += 2
            } else {
               arg_idx += 1
            }
         }
         if(shell_mode) {
            print(prefix + "X=" + to_str(int(vx)))
            print(prefix + "Y=" + to_str(int(vy)))
         } else {
            print(to_str(int(vx)) + " " + to_str(int(vy)))
         }
         xt_xfree(p)
      } else {
         print("0 0")
      }
   } else {
      print("0 0")
   }
   free(prop, act, fmt, nitems, after)
} elif(cmd == "set_desktop_viewport") {
   if(nargs < 4) {
      eprint("set_desktop_viewport requires x and y")
      code = 1
   } else {
      def vx = parse_int(xt_arg(2))
      def vy = parse_int(xt_arg(3))
      send_client_message(root, "_NET_DESKTOP_VIEWPORT", vx, vy)
   }
} else {
   eprint("Unknown command: " + cmd)
   print_usage()
   code = 1
}

XSync(display, 0)
if(display) {
   XCloseDisplay(display)
   display = nil
}
exit(code)
