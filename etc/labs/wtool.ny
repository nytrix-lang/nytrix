#!/usr/bin/env ny
;; Keywords: wayland seat input ime driver cli
;; waydr - dependency-free Wayland seat driver for the compositor input probe.
;;
;; Speaks the Wayland wire protocol directly over AF_UNIX (no libwayland) and
;; binds a virtual keyboard (zwp_virtual_keyboard_v1), a virtual pointer
;; (zwlr_virtual_pointer_v1), and an input method (zwp_input_method_v2) that
;; sends preedit, commit and delete-surrounding to the focused text-input-v3
;; client. It replays a step script so the probe goes through the compositor
;; seat instead of the application's own listeners.
;;
;; usage: wtool.ny probe
;;        wtool.ny run SCRIPT
;;        wtool.ny selftest
;; `probe` exits 0 when the compositor offers all three protocols, 3 otherwise.
;; SCRIPT holds one step per line:
;;   wait_file PATH            wait until PATH exists (the app's ready marker)
;;   move X Y                  pointer to surface-of-output position X Y
;;   click X Y                 move there and click the left button
;;   type TEXT                 type ASCII through the virtual keyboard
;;   key CODE                  press and release one evdev key code
;;   wait_active               wait until the focused client enables text input
;;   preedit TEXT CURSOR       show a composition (CURSOR in bytes, -1 hides)
;;   commit TEXT               commit text (ends any composition)
;;   delete BEFORE AFTER       delete bytes around the caret
;;   cancel                    end the composition without committing
;;   sleep SECONDS
;;   log TEXT                  print a marker
;; Each IME step is followed by zwp_input_method_v2.commit with the serial of
;; the last `done`, as the protocol requires.
use core
use core.common as common
use core.str as str
use core.mem as mem
use core.os (file_read, file_exists)
use core.os.time (now_ms)
use core.os.subprocess (run_capture)

;; Linux libc entry points: plain sockets and fd passing, no libwayland.
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
   if ow.len > 0 { OUT_W = int(str.atof(ow)) }
   def oh = common.env_trim("NY_DRIVER_OUTPUT_H")
   if oh.len > 0 { OUT_H = int(str.atof(oh)) }
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
   int(str.atof(to_str(args.get(i, "0"))))
}

fn run_step(str line) {
   def parts = partition_space(line)
   def cmd = to_str(parts.get(0, ""))
   def rest = to_str(parts.get(1, ""))
   def args = rest.len > 0 ? str.split(rest, " ") : []
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
   } elif cmd == "sleep" {
      wl_pump(float(str.atof(rest)))
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
   def lines = str.split(to_str(unwrap(rd)), "\n")
   mut i = 0
   while i < lines.len {
      def line = str.strip(lines.get(i, ""))
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

;; ---------------------------------------------------------------------- main
mut MODE = ""
if argc() >= 2 { MODE = argv(1) }
if MODE == "probe" {
   exit(probe_main())
} elif MODE == "run" && argc() >= 3 {
   exit(run_main(argv(2)))
} elif MODE == "selftest" {
   exit(selftest())
} else {
   print("usage: wtool.ny probe | run SCRIPT | selftest")
   exit(2)
}
