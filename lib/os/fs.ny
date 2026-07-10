;; Keywords: fs filesystem files os
;; Filesystem Management for Nytrix
;; References:
;; - std.os
module std.os.fs
use std.core
use std.core as core
use std.os
use std.os.path as ospath
use std.core.str

fn is_file(any path) bool {
   "Returns true if the given `path` exists and points to a regular file."
   if !is_str(path) { return false }
   def p = ospath.normalize(path)
   if !(file_exists(p)) { return false }
   if is_dir(p) { return false }
   return true
}

fn is_dir(any path) bool {
   "Returns true if the given `path` exists and points to a directory."
   if !is_str(path) { return false }
   if path == "." || path == ".." { return true }
   def p = ospath.normalize(path)
   __is_dir(p) == 1
}

fn list_dir(any path) list {
   "Returns a list of filenames contained within the directory at `path`. Excludes '.' and '..' entries."
   if !is_str(path) { return list(0) }
   def p, h = ospath.normalize(path), __dir_open(p)
   if !h { return list(0) }
   mut files = list(8)
   while 1 {
      def name = __dir_read(h)
      if !name { break }
      if name == "." || name == ".." { continue }
      files = files.append(name)
   }
   __dir_close(h)
   return files
}

fn walk(any root, any cb, int max_depth=-1, any visited=nil) int {
   "Recursively traverses `root`, calling `cb(path)` for each entry. Optional `max_depth` limits recursion."
   if !is_str(root) { return 0 }
   mut r = ospath.normalize(root)
   if r.len == 0 { r = "." }
   if !file_exists(r) { return 0 }
   if visited == nil || !is_dict(visited) { visited = dict(16) }
   if visited.contains(r) { return 0 }
   visited = visited.set(r, true)
   cb(r)
   if max_depth == 0 { return 0 }
   if is_dir(r) {
      def items = list_dir(r)
      def n = items.len
      mut i = 0
      while i < n {
         def name = items.get(i)
         def full_path = ospath.join(r, name)
         def next_depth = max_depth > 0 ? max_depth - 1 : -1
         walk(full_path, cb, next_depth, visited)
         i += 1
      }
   }
   0
}

fn rename(str old_path, str new_path) Result<int, int> {
   "Renames or moves `old_path` to `new_path`."
   file_rename(old_path, new_path)
}

mut _fs_selftest_walk_hits = 0

fn _fs_selftest_walk_hit(any _path) int {
   _fs_selftest_walk_hits += 1
   0
}

#main {
   def tmp = ospath.temp_dir()
   assert(is_dir(tmp), "fs temp dir")
   def fp = ospath.join(tmp, "nytrix_fs_selftest_" + to_str(pid()) + "_" + to_str(ticks()) + ".tmp")
   unwrap(file_write(fp, "ok"))
   assert(is_file(fp), "fs temp file")
   def fp2 = fp + ".renamed"
   unwrap(rename(fp, fp2))
   assert(!is_file(fp) && is_file(fp2), "fs rename")
   unwrap(rename(fp2, fp))
   def entries = list_dir(".")
   assert(entries.len > 0, "fs list_dir cwd")
   mut i = 0
   mut n = entries.len
   if n > 32 { n = 32 }
   while i < n {
      def name = entries.get(i)
      assert(name != "." && name != "..", "fs list_dir excludes dots")
      i += 1
   }
   _fs_selftest_walk_hits = 0
   walk(fp, _fs_selftest_walk_hit)
   assert(_fs_selftest_walk_hits == 1, "fs walk file once")
   _fs_selftest_walk_hits = 0
   walk(".", _fs_selftest_walk_hit, 0)
   assert(_fs_selftest_walk_hits == 1, "fs walk max_depth zero")
   unwrap(file_remove(fp))
   print("✓ std.os.fs self-test passed")
}

;; File watching via real platform watchers (inotify on linux; poll fallback elsewhere)
;; For language-level hot reloading of .so/.dylib/.dll modules.

def IN_ACCESS = 0x00000001
def IN_MODIFY = 0x00000002
def IN_ATTRIB = 0x00000004
def IN_CLOSE_WRITE = 0x00000008
def IN_CLOSE_NOWRITE = 0x00000010
def IN_OPEN = 0x00000020
def IN_MOVED_FROM = 0x00000040
def IN_MOVED_TO = 0x00000080
def IN_CREATE = 0x00000100
def IN_DELETE = 0x00000200
def IN_DELETE_SELF = 0x00000400
def IN_MOVE_SELF = 0x00000800
def IN_UNMOUNT = 0x00002000
def IN_Q_OVERFLOW = 0x00004000
def IN_IGNORED = 0x00008000
def IN_ONLYDIR = 0x01000000
def IN_DONT_FOLLOW = 0x02000000
def IN_EXCL_UNLINK = 0x04000000
def IN_MASK_ADD = 0x20000000
def IN_ISDIR = 0x40000000
def IN_ONESHOT = 0x80000000

def O_NONBLOCK = 0x800  ;; O_NONBLOCK for inotify_init1 (and fallbacks); passed through as logical value (runtime >>1 yields host flag)

def IN_ALL_EVENTS = bor(bor(bor(IN_MODIFY, IN_CREATE), bor(IN_DELETE, IN_MOVED_TO)), IN_CLOSE_WRITE)

#linux {
   #include <sys/inotify.h>
} #else {
   fn __inotify_init(any _flags) int { -1 }
   fn __inotify_add_watch(any _fd, any _path, any _mask) int { -1 }
   fn __inotify_rm_watch(any _fd, any _wd) int { -1 }
} #endif

fn watch_init(int flags=0) int {
   "Initialize an inotify instance (or -1 on unsupported platforms). Use nonblock via flags if supported."
   __inotify_init(flags)
}

fn watch_add(int fd, str path, int mask=IN_ALL_EVENTS) int {
   "Add a watch on `path` returning watch descriptor wd (>=0) or -1."
   if !is_str(path) || fd < 0 { return -1 }
   def p = ospath.normalize(path)
   __inotify_add_watch(fd, p, mask)
}

fn watch_rm(int fd, int wd) int {
   "Remove watch descriptor from fd."
   if fd < 0 || wd < 0 { return -1 }
   __inotify_rm_watch(fd, wd)
}

;; Simple event struct reader. Event layout (linux): wd:i32, mask:u32, cookie:u32, len:u32, name...
def IN_EVENT_SIZE = 16
def IN_EVENT_WD = 0
def IN_EVENT_MASK = 4
def IN_EVENT_COOKIE = 8
def IN_EVENT_LEN = 12
def IN_EVENT_NAME = 16

fn watch_read_events(int fd, any buf, int bufsz) list {
   "Read pending events from inotify fd into buf; returns list of {wd,mask,cookie,name} dicts. Non-blocking if fd set nonblock."
   if fd < 0 || !buf { return [] }
   def n = __read_off(fd, buf, bufsz, 0)
   if n <= 0 { return [] }
   mut evs = []
   mut off = 0
   while off + IN_EVENT_SIZE <= n {
      def wd = load32(buf, off + IN_EVENT_WD)
      def mask = load32(buf, off + IN_EVENT_MASK)
      def cookie = load32(buf, off + IN_EVENT_COOKIE)
      def name_len = load32(buf, off + IN_EVENT_LEN)
      mut name = ""
      if name_len > 0 && off + IN_EVENT_SIZE + name_len <= n {
         name = str.cstr_to_str(buf + off + IN_EVENT_NAME)
      }
      evs = evs.append({wd: wd, mask: mask, cookie: cookie, name: name})
      off += IN_EVENT_SIZE + name_len
   }
   evs
}

fn watch_has_change(any evs, int want_mask=IN_MODIFY) bool {
   "Returns true if any event in list has bits from want_mask set (or create/delete/move)."
   if !is_list(evs) { return false }
   mut i = 0
   def n = evs.len
   while i < n {
      def e = evs.get(i)
      def m = int(e.get("mask", 0))
      if (m & want_mask) != 0 { return true }
      if (m & bor(bor(IN_CREATE, IN_DELETE), IN_MOVED_TO)) != 0 { return true }
      i += 1
   }
   false
}

#main {
   ;; previous selftest already ran; add minimal watcher smoke if possible
   def wfd = watch_init(O_NONBLOCK)
   if wfd >= 0 {
      def wd = watch_add(wfd, ".", IN_MODIFY | IN_CREATE | IN_DELETE)
      if wd >= 0 {
         ;; non-destructive: just rm it
         watch_rm(wfd, wd)
      }
      __close(wfd)
      print("✓ std.os.fs.watch primitives self-test passed")
   }
}

;; ------------------------------------------------------------------
;; Portable watch facade with best-effort platform backends.
;; ------------------------------------------------------------------

use std.os.platform as platform

def WATCH_MODIFY = 0x00000002
def WATCH_CREATE = 0x00000100
def WATCH_DELETE = 0x00000200
def WATCH_ALL    = bor(bor(WATCH_MODIFY, WATCH_CREATE), WATCH_DELETE)

#macos {
   def NOTE_WRITE  = 0x00000002
   def NOTE_DELETE = 0x00000001
   def NOTE_RENAME = 0x00000020
   def NOTE_ATTRIB = 0x00000008
   def NOTE_EXTEND = 0x00000004
   def EV_ADD      = 0x0001
   def EV_CLEAR    = 0x0020

   fn _kqueue_vnode(str p) any {
      def fd = __watch_open_vnode(p)
      if fd <= 0 { return nil }
      def kq = __kqueue()
      if kq <= 0 { return nil }
      def mask = bor(bor(NOTE_WRITE, NOTE_DELETE), bor(NOTE_RENAME, NOTE_ATTRIB))
      __kevent(kq, fd, 0, bor(EV_ADD, EV_CLEAR), mask, 0, 0)
      mut d = dict(4)
      d = d.set("type", "kqueue")
      d = d.set("kq", kq)
      d = d.set("fd", fd)
      d = d.set("path", p)
      return d
   }

   fn _kqueue_read(any wh) list {
      if !is_dict(wh) { return [] }
      def kq = int(wh.get("kq", -1))
      if kq < 0 { return [] }
      def ev = __kevent(kq, 0, 0, 0, 0, 0, 0)
      if ev == 0 { return [] }
      mut e = dict(2)
      e = e.set("mask", int(ev))
      e = e.set("name", "")
      return [e]
   }
} #else {
   fn _kqueue_vnode(str _p) any { nil }
   fn _kqueue_read(any _w) list { [] }
} #endif

#windows {
   def FILE_NOTIFY_CHANGE_FILE_NAME  = 0x00000001
   def FILE_NOTIFY_CHANGE_DIR_NAME   = 0x00000002
   def FILE_NOTIFY_CHANGE_LAST_WRITE = 0x00000010

   fn _win32_watch(str p) any {
      def f = bor(FILE_NOTIFY_CHANGE_LAST_WRITE, FILE_NOTIFY_CHANGE_FILE_NAME)
      def h = __win32_find_first_change(p, 1, f)
      if h == 0 { return nil }
      mut d = dict(3)
      d = d.set("type", "win32")
      d = d.set("handle", h)
      d = d.set("path", p)
      return d
   }

   fn _win32_next(any h) bool {
      if !h || h == 0 { return false }
      return __win32_find_next_change(h) != 0
   }

   fn _win32_close(any h) int {
      if !h || h == 0 { return 0 }
      __win32_find_close_change(h)
      return 0
   }
} #else {
   fn _win32_watch(str _p) any { nil }
   fn _win32_next(any _h) bool { false }
   fn _win32_close(any _h) int { 0 }
} #endif

fn watch_create(str path) any {
   "Portable create (real when available)."
   if !is_str(path) { return nil }
   def p = ospath.normalize(path)
   if platform.is_linux() {
      def fd = watch_init(O_NONBLOCK)
      if fd > 0 {
         def m = bor(bor(IN_MODIFY, IN_CREATE), bor(IN_DELETE, IN_ATTRIB))
         def wd = watch_add(fd, p, m)
         if wd >= 0 {
            mut d = dict(4)
            d = d.set("type", "inotify")
            d = d.set("fd", fd)
            d = d.set("wd", wd)
            d = d.set("path", p)
            return d
         }
         __close(fd)
      }
   } elif platform.is_macos() {
      def h = _kqueue_vnode(p)
      if h != nil { return h }
   } elif platform.is_windows() {
      def h = _win32_watch(p)
      if h != nil { return h }
   }
   mut d = dict(3)
   d = d.set("type", "poll")
   d = d.set("path", p)
   d = d.set("last", 0)
   return d
}

fn watch_close(any h) int {
   if !is_dict(h) { return 0 }
   def ty = to_str(h.get("type", ""))
   if ty == "inotify" {
      def fd = int(h.get("fd", -1))
      def wd = int(h.get("wd", -1))
      if wd >= 0 { watch_rm(fd, wd) }
      if fd >= 0 { __close(fd) }
   } elif ty == "kqueue" {
      def kq = int(h.get("kq", -1))
      def vfd = int(h.get("fd", -1))
      if kq > 0 { __close(kq) }
      if vfd > 0 { __close(vfd) }
   } elif ty == "win32" {
      _win32_close(h.get("handle", 0))
   }
   0
}

fn watch_poll(any h) list {
   if !is_dict(h) { return [] }
   def ty = to_str(h.get("type", ""))
   if ty == "inotify" {
      def fd = int(h.get("fd", -1))
      def b = malloc(8192)
      def r = watch_read_events(fd, b, 8192)
      free(b)
      return r
   } elif ty == "kqueue" {
      return _kqueue_read(h)
   } elif ty == "win32" {
      if _win32_next(h.get("handle", 0)) {
         mut e = dict(2)
         e = e.set("mask", WATCH_MODIFY)
         e = e.set("name", "")
         return [e]
      }
      return []
   }
   []
}

fn watch_has_event(any h) bool { watch_poll(h).len > 0 }

#main {
   def w = watch_create(".")
   if w {
      watch_poll(w)
      watch_close(w)
      print("✓ std.os.fs.watch facade self-test passed")
   }
}
