;; Keywords: memory scanner process proc mem value search cheat debug linux
;; Process memory scanner for Linux /proc/<pid>/mem: typed value parsing,
;; first/next scans with comparison modes, scan history, writes and value
;; locks. Used by etc/projects/ui/smem.ny (a port of x3ric/smem).
;;
;; One process is attached at a time. Results live in raw memory (address,
;; type and previous value per entry) so a scan over hundreds of megabytes
;; stays bounded; a first scan without a known value keeps a byte snapshot
;; of each region instead of one entry per address.
;;
;;   memscan.attach(pid)                       ; open /proc/<pid>/mem
;;   memscan.scan(regions, "100", "Exact", "Auto")
;;   memscan.scan(regions, "0", "Decreased", "Auto")   ; next scan
;;   memscan.count() / addr_at(i) / type_at(i) / value_text(i)
;;   memscan.set_all("999")                    ; write every result
;;
;; Types (index): 0 Int8, 1 Int16, 2 Int32, 3 Int64, 4 UInt8, 5 UInt16,
;; 6 UInt32, 7 UInt64, 8 Float32, 9 Float64, 10 Pointer, 11 Size, 12 Bool.
;; Values typed in the UI follow smem: `int32:5`, `float:3.5`, `hex:ff`,
;; `bool:true`, `byte:7`, `ptr:0x1000`, or a bare number. A bare number is
;; compared numerically in every scanned type (42 matches an Int16 or Int32
;; cell holding 42), not only in the type it parsed as.
module core.os.memscan(
   TYPE_NAMES, MODES, MAX_RESULTS,
   type_index, type_name, type_size,
   parse_value, parsed_type, parsed_int, parsed_float,
   attach, detach_process, attached, pid, last_error,
   load_maps, read_bytes, write_bytes,
   scan, previous, reset, depth, count, truncated, is_snapshot,
   snapshot_bytes, addr_at, type_at, read_value_text, value_text, decode_text,
   set_all, set_one, lock_start, lock_stop, lock_active, lock_tick,
   write_value, read_value, region_writable, region_shared
)

use core
use core.str as str
use core.os (file_read, ticks)

def TYPE_NAMES = ["Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32", "UInt64",
   "Float32", "Float64", "Pointer", "Size", "Bool"]
def MODES = ["Exact", "Changed", "Unchanged", "Increased", "Increased or Greater", "Increased by",
   "Decreased", "Decreased or Less", "Decreased by"]

;; Result entries kept per scan; a scan that finds more stops there and
;; reports truncated().
def int MAX_RESULTS = 4000000

;; Bytes a snapshot (first scan without a value) may hold in total.
def int MAX_SNAPSHOT = 536870912

;; Scan levels kept for previous().
def int MAX_DEPTH = 32

;; ---- types ----------------------------------------------------------------
fn type_index(str name) int {
   "Returns the index of a type name, or -1(also for \"Auto\")."
   mut int i = 0
   while i < 13 {
      if to_str(TYPE_NAMES.get(i, "")) == name { return i }
      i += 1
   }
   -1
}

fn type_name(int t) str {
   "Returns the name of type index `t`."
   (t >= 0 && t < 13) ? to_str(TYPE_NAMES.get(t, "")) : "?"
}

fn type_size(int t) int {
   "Returns the byte size of type index `t`."
   if t == 0 || t == 4 || t == 12 { return 1 }
   if t == 1 || t == 5 { return 2 }
   if t == 2 || t == 6 || t == 8 { return 4 }
   if t >= 0 && t < 13 { return 8 }
   0
}

;; 0 signed, 1 unsigned, 2 float32, 3 float64, 4 bool
fn _cls(int t) int {
   if t <= 3 { return 0 }
   if t <= 7 || t == 10 || t == 11 { return 1 }
   if t == 8 { return 2 }
   if t == 9 { return 3 }
   4
}

;; Reads a value of type `t` at p+off as raw bits, zero-extended.
fn _bits(ptr p, int off, int t) int {
   def int n = type_size(t)
   if n == 8 { return load64(p, off) }
   if n == 4 { return load32(p, off) & 0xffffffff }
   if n == 2 { return load16(p, off) & 0xffff }
   load8(p, off) & 255
}

fn _signed(int bits, int t) int {
   def int n = type_size(t)
   if n == 1 && bits >= 128 { return bits - 256 }
   if n == 2 && bits >= 32768 { return bits - 65536 }
   if n == 4 && bits >= 2147483648 { return bits - 4294967296 }
   bits
}

fn _f32_of_bits(int bits) f64 {
   def ptr tmp = _scratch()
   store32(tmp, bits, 0)
   float(load32_f32(tmp, 0))
}

fn _f64_of_bits(int bits) f64 {
   def ptr tmp = _scratch()
   store64(tmp, bits, 0)
   float(load64_f64(tmp, 0))
}

fn _bits_of_f32(f64 v) int {
   def ptr tmp = _scratch()
   store32_f32(tmp, v, 0)
   load32(tmp, 0) & 0xffffffff
}

fn _bits_of_f64(f64 v) int {
   def ptr tmp = _scratch()
   store64_f64(tmp, v, 0)
   load64(tmp, 0)
}

mut int _scratch_p = 0

fn _scratch() ptr {
   if _scratch_p == 0 { _scratch_p = int(malloc(16)) }
   ptr_add(0, _scratch_p)
}

;; Decoded numeric value of bits of type t as f64 (for comparisons).
fn _num(int bits, int t) f64 {
   def int c = _cls(t)
   if c == 2 { return _f32_of_bits(bits) }
   if c == 3 { return _f64_of_bits(bits) }
   if c == 0 { return float(_signed(bits, t)) }
   if c == 4 { return bits != 0 ? 1.0 : 0.0 }
   float(bits)
}

fn decode_text(int bits, int t) str {
   "Formats raw bits of type index `t` the way smem shows a value."
   def int c = _cls(t)
   if c == 2 { return to_str(_f32_of_bits(bits)) }
   if c == 3 { return to_str(_f64_of_bits(bits)) }
   if c == 4 { return bits != 0 ? "true" : "false" }
   if c == 0 { return to_str(_signed(bits, t)) }
   if t == 10 { return "0x" + str.to_hex(bits, 0) }
   to_str(bits)
}

;; ---- parsing --------------------------------------------------------------
mut int _pv_ok = 0
mut int _pv_type = -1
mut int _pv_int = 0
mut f64 _pv_float = 0.0
mut int _pv_is_float = 0

fn parsed_type() int { "Returns the type index of the last parse_value." _pv_type }

fn parsed_int() int { "Returns the integer of the last parse_value." _pv_int }

fn parsed_float() f64 { "Returns the number of the last parse_value as a float." _pv_float }

fn _digits_only(str t, int from) bool {
   if from >= t.len { return false }
   mut int i = from
   while i < t.len {
      def int c = load8(t, i)
      if c < 48 || c > 57 { return false }
      i += 1
   }
   true
}

fn _parse_dec(str s) bool {
   def str t = str.strip(s)
   if t.len == 0 { return false }
   mut int i = 0
   mut bool neg = false
   if load8(t, 0) == 45 { neg = true i = 1 }
   elif load8(t, 0) == 43 { i = 1 }
   if !_digits_only(t, i) { return false }
   mut int v = 0
   while i < t.len {
      v = v * 10 + (load8(t, i) - 48)
      i += 1
   }
   _pv_int = neg ? 0 - v : v
   _pv_float = float(_pv_int)
   _pv_is_float = 0
   true
}

fn _parse_hex(str s) bool {
   mut str t = str.strip(s)
   if str.startswith(t, "0x") || str.startswith(t, "0X") { t = str.str_slice(t, 2, t.len) }
   if t.len == 0 || t.len > 16 { return false }
   mut int v = 0
   mut int i = 0
   while i < t.len {
      def int c = load8(t, i)
      mut int d = -1
      if c >= 48 && c <= 57 { d = c - 48 }
      elif c >= 65 && c <= 70 { d = c - 55 }
      elif c >= 97 && c <= 102 { d = c - 87 }
      if d < 0 { return false }
      v = (v << 4) | d
      i += 1
   }
   _pv_int = v
   _pv_float = float(v)
   _pv_is_float = 0
   true
}

fn _parse_float(str s) bool {
   def str t = str.strip(s)
   if t.len == 0 { return false }
   mut int digits = 0
   mut int i = 0
   while i < t.len {
      def int c = load8(t, i)
      if c >= 48 && c <= 57 { digits += 1 }
      elif !(c == 46 || c == 101 || c == 69 || c == 43 || c == 45) { return false }
      i += 1
   }
   if digits == 0 { return false }
   _pv_float = float(str.atof(t))
   _pv_int = int(_pv_float)
   _pv_is_float = 1
   true
}

fn parse_value(str input) bool {
   "Parses a smem value(`type:value` or a bare number). Returns false when it does not parse; parsed_type/parsed_int/parsed_float hold the result."
   _pv_ok = 0
   _pv_type = -1
   def str t = str.strip(input)
   if t.len == 0 { return false }
   def int cpos = str.find(t, ":")
   if cpos > 0 {
      def str pfx = str.lower(str.str_slice(t, 0, cpos))
      def str v = str.str_slice(t, cpos + 1, t.len)
      if pfx == "bool" || pfx == "boolean" {
         def str b = str.lower(str.strip(v))
         if b == "true" || b == "1" { _pv_int = 1 }
         elif b == "false" || b == "0" { _pv_int = 0 }
         else { return false }
         _pv_float = float(_pv_int)
         _pv_type = 12
      } elif pfx == "byte" || pfx == "b" {
         if !_parse_dec(v) || _pv_int < 0 || _pv_int > 255 { return false }
         _pv_type = 4
      } elif pfx == "hex" || pfx == "h" {
         if !_parse_hex(v) { return false }
         if _pv_int >= 0 && _pv_int <= 255 { _pv_type = 4 }
         elif _pv_int >= 0 && _pv_int <= 65535 { _pv_type = 5 }
         elif _pv_int >= 0 && _pv_int <= 4294967295 { _pv_type = 6 }
         else { _pv_type = 7 }
      } elif pfx == "int8" || pfx == "i8" || pfx == "char" {
         if !_parse_dec(v) || _pv_int < -128 || _pv_int > 127 { return false }
         _pv_type = 0
      } elif pfx == "int16" || pfx == "i16" || pfx == "short" {
         if !_parse_dec(v) || _pv_int < -32768 || _pv_int > 32767 { return false }
         _pv_type = 1
      } elif pfx == "int32" || pfx == "i32" || pfx == "int" {
         if !_parse_dec(v) || _pv_int < -2147483648 || _pv_int > 2147483647 { return false }
         _pv_type = 2
      } elif pfx == "int64" || pfx == "i64" || pfx == "long" {
         if !_parse_dec(v) { return false }
         _pv_type = 3
      } elif pfx == "float32" || pfx == "f32" || pfx == "float" {
         if !_parse_float(v) { return false }
         _pv_type = 8
      } elif pfx == "float64" || pfx == "f64" || pfx == "double" {
         if !_parse_float(v) { return false }
         _pv_type = 9
      } elif pfx == "size" || pfx == "s" {
         if !_parse_dec(v) || _pv_int < 0 { return false }
         _pv_type = 11
      } elif pfx == "ptr" || pfx == "pointer" {
         if !_parse_dec(v) && !_parse_hex(v) { return false }
         _pv_type = 10
      } else {
         return false
      }
   } elif _parse_dec(t) {
      if _pv_int >= -128 && _pv_int <= 127 { _pv_type = 0 }
      elif _pv_int >= -32768 && _pv_int <= 32767 { _pv_type = 1 }
      elif _pv_int >= -2147483648 && _pv_int <= 2147483647 { _pv_type = 2 }
      else { _pv_type = 3 }
   } elif _parse_float(t) {
      _pv_type = (_pv_float <= 3.4028234663852886e38 && _pv_float >= -3.4028234663852886e38) ? 8 : 9
   } else {
      return false
   }
   _pv_ok = 1
   true
}

;; Types an Auto scan searches for a parsed value, as smem does.
fn _auto_types(int t) list {
   if t == 0 { return [0, 1, 2] }
   if t == 1 { return [1, 0, 2] }
   if t == 2 { return [2, 3, 1] }
   if t == 3 { return [3, 2] }
   if t == 4 { return [4, 5, 6] }
   if t == 5 { return [5, 4, 6] }
   if t == 6 { return [6, 7, 5] }
   if t == 7 { return [7, 6] }
   if t == 8 { return [8, 9] }
   if t == 9 { return [9, 8] }
   if t == 11 { return [11, 7, 3] }
   if t == 10 { return [10, 7] }
   if t == 12 { return [12, 4] }
   [2]
}

;; Bits of the parsed value in type t, or -1 in _enc_ok when it does not fit.
mut int _enc_ok = 0

fn _encode(int t) int {
   _enc_ok = 1
   def int c = _cls(t)
   if c == 2 { return _bits_of_f32(_pv_float) }
   if c == 3 { return _bits_of_f64(_pv_float) }
   if c == 4 { return (_pv_int != 0 || _pv_float != 0.0) ? 1 : 0 }
   ;; Integer types need an integral input in range.
   if _pv_is_float == 1 && float(int(_pv_float)) != _pv_float {
      _enc_ok = 0
      return 0
   }
   def int v = _pv_int
   def int n = type_size(t)
   if c == 0 {
      if n == 1 && (v < -128 || v > 127) { _enc_ok = 0 return 0 }
      if n == 2 && (v < -32768 || v > 32767) { _enc_ok = 0 return 0 }
      if n == 4 && (v < -2147483648 || v > 2147483647) { _enc_ok = 0 return 0 }
   } else {
      if v < 0 && n < 8 { _enc_ok = 0 return 0 }
      if n == 1 && v > 255 { _enc_ok = 0 return 0 }
      if n == 2 && v > 65535 { _enc_ok = 0 return 0 }
      if n == 4 && v > 4294967295 { _enc_ok = 0 return 0 }
   }
   if n == 1 { return v & 255 }
   if n == 2 { return v & 0xffff }
   if n == 4 { return v & 0xffffffff }
   v
}

fn _feq(f64 a, f64 b, int t) bool {
   if a == b { return true }
   def f64 d = a > b ? a - b : b - a
   def f64 m = (a > 0.0 ? a : 0.0 - a) + (b > 0.0 ? b : 0.0 - b)
   ;; Float32 cells hold a rounded copy of the input; compare at that precision.
   def f64 rel = t == 8 ? 0.000001 : 0.000000000001
   d <= rel * m || d < 0.0000000001
}

;; Does new (bits nb) of type t satisfy `mode` against old bits ob and the parsed value?
fn _match(int mode, int t, int ob, int nb) bool {
   def int c = _cls(t)
   if mode == 0 {
      def int want = _encode(t)
      if _enc_ok == 0 { return false }
      if c == 2 || c == 3 { return _feq(_num(nb, t), _num(want, t), t) }
      if c == 4 { return (nb != 0 ? 1 : 0) == want && nb <= 1 }
      return nb == want
   }
   def f64 o = _num(ob, t)
   def f64 n = _num(nb, t)
   def bool same = (c == 2 || c == 3) ? _feq(n, o, t) : nb == ob
   if mode == 1 { return !same }
   if mode == 2 { return same }
   if mode == 3 { return !same && n > o }
   if mode == 4 { return same || n > o }
   if mode == 6 { return !same && n < o }
   if mode == 7 { return same || n < o }
   ;; By an amount: compare in the cell's type, wrapping integers.
   def f64 delta = _pv_float
   if mode == 5 || mode == 8 {
      def f64 want = mode == 5 ? o + delta : o - delta
      if c == 2 || c == 3 { return _feq(n, want, t) }
      if _pv_is_float == 1 && float(int(delta)) != delta { return false }
      def int step = mode == 5 ? _pv_int : 0 - _pv_int
      def int nbits = type_size(t) * 8
      mut int w = ob + step
      if nbits < 64 { w = w & ((1 << nbits) - 1) }
      return nb == w
   }
   false
}

fn _mode_index(str mode) int {
   mut int i = 0
   while i < MODES.len {
      if to_str(MODES.get(i, "")) == mode { return i }
      i += 1
   }
   -1
}

;; ---- process access -------------------------------------------------------
mut int _fd = -1
mut int _pid = 0
mut str _err = ""

fn last_error() str { "Returns the last error message, or \"\"." _err }

fn attached() bool { "True while a process is attached." _fd >= 0 }

fn pid() int { "Returns the attached process id, or 0." _pid }

fn attach(int p) bool {
   "Opens /proc/<p>/mem for reading and writing. Returns false and sets last_error when it cannot(permissions, no such process)."
   detach_process()
   _err = ""
   if p <= 0 { _err = "invalid pid" return false }
   mut int fd = __open_raw("/proc/" + to_str(p) + "/mem", 2, 0)
   if fd < 0 { fd = __open_raw("/proc/" + to_str(p) + "/mem", 0, 0) }
   if fd < 0 {
      _err = "cannot open /proc/" + to_str(p) + "/mem(needs ptrace access: run as root or set kernel.yama.ptrace_scope=0)"
      return false
   }
   _fd = fd
   _pid = p
   true
}

fn detach_process() any {
   "Closes the process and drops every scan level and lock."
   lock_stop()
   reset()
   if _fd >= 0 { __close(_fd) }
   _fd = -1
   _pid = 0
   0
}

fn _sys_pread_nr() int {
   #aarch64 {
      return 67
   } #endif
   17
}

fn _sys_pwrite_nr() int {
   #aarch64 {
      return 68
   } #endif
   18
}

;; Positioned reads and writes on /proc/<pid>/mem: the file offset is the
;; address. (__read_off_raw's offset is into the buffer, not the file.)
fn _pread(ptr buf, int len, int addr) int { __syscall_raw(_sys_pread_nr(), _fd, int(buf), len, addr, 0, 0) }

fn _pwrite(ptr buf, int len, int addr) int { __syscall_raw(_sys_pwrite_nr(), _fd, int(buf), len, addr, 0, 0) }

fn read_bytes(int addr, int len) ptr {
   "Reads `len` bytes at `addr` into a fresh buffer(the caller frees it), or returns 0 when any byte is unreadable."
   if _fd < 0 || len <= 0 { return 0 }
   def ptr buf = malloc(len)
   if !buf { return 0 }
   mut int off = 0
   while off < len {
      def int n = _pread(ptr_add(buf, off), len - off, addr + off)
      if n <= 0 {
         free(buf)
         return 0
      }
      off += n
   }
   buf
}

fn _read_into(ptr buf, int addr, int len) int {
   ;; Returns the readable prefix length (reads stop at an unmapped page).
   mut int off = 0
   while off < len {
      def int n = _pread(ptr_add(buf, off), len - off, addr + off)
      if n <= 0 { return off }
      off += n
   }
   off
}

fn write_bytes(int addr, ptr src, int len) bool {
   "Writes `len` bytes from `src` at `addr` in the attached process."
   if _fd < 0 || len <= 0 { return false }
   mut int off = 0
   while off < len {
      def int n = _pwrite(ptr_add(src, off), len - off, addr + off)
      if n <= 0 { return false }
      off += n
   }
   true
}

fn _write_bits(int addr, int bits, int t) bool {
   def ptr tmp = _scratch()
   store64(tmp, bits, 0)
   write_bytes(addr, tmp, type_size(t))
}

fn load_maps(int p) list {
   "Returns the readable mappings of process `p` grouped by name, smem's order: [Anonymous], [heap], other [..], then files. Each group is {name, enabled, regions: [[start, end, perms], ...]}; perms is the maps column(`rw-p`)."
   if p <= 0 { return [] }
   def res = file_read("/proc/" + to_str(p) + "/maps")
   if is_err(res) { return [] }
   def str text = to_str(unwrap(res))
   mut names = []
   mut regs = []
   def lines = text.split("\n")
   mut int i = 0
   while i < lines.len {
      def str line = to_str(lines.get(i, ""))
      i += 1
      def parts = _fields(line)
      if parts.len < 2 { continue }
      def str perms = to_str(parts.get(1, ""))
      if str.find(perms, "r") < 0 { continue }
      def str range = to_str(parts.get(0, ""))
      def int dash = str.find(range, "-")
      if dash <= 0 { continue }
      if !_parse_hex(str.str_slice(range, 0, dash)) { continue }
      def int a0 = _pv_int
      if !_parse_hex(str.str_slice(range, dash + 1, range.len)) { continue }
      def int a1 = _pv_int
      if a1 <= a0 { continue }
      mut str name = "[Anonymous]"
      if parts.len >= 6 {
         mut str nm = ""
         mut int j = 5
         while j < parts.len {
            if j > 5 { nm = nm + " " }
            nm = nm + to_str(parts.get(j, ""))
            j += 1
         }
         name = nm
      }
      mut int gi = -1
      mut int k = 0
      while k < names.len {
         if to_str(names.get(k, "")) == name { gi = k break }
         k += 1
      }
      if gi < 0 {
         names = names.append(name)
         regs = regs.append([])
         gi = names.len - 1
      }
      mut any rl = regs.get(gi, [])
      rl = rl.append([a0, a1, perms])
      regs[gi] = rl
   }
   ;; Order by rank then name: repeatedly move the first remaining group.
   mut any pending = []
   mut int k2 = 0
   while k2 < names.len {
      pending = pending.append({"name": to_str(names.get(k2, "")), "enabled": true, "regions": regs.get(k2, [])})
      k2 += 1
   }
   mut out = []
   while pending.len > 0 {
      mut int best = 0
      mut int c = 1
      while c < pending.len {
         def any gc = pending.get(c, 0)
         def any gb = pending.get(best, 0)
         if _group_before(to_str(gc.get("name", "")), to_str(gb.get("name", ""))) { best = c }
         c += 1
      }
      out = out.append(pending.get(best, 0))
      mut any rest = []
      mut int r2 = 0
      while r2 < pending.len {
         if r2 != best { rest = rest.append(pending.get(r2, 0)) }
         r2 += 1
      }
      pending = rest
   }
   out
}

fn _rank(str n) int {
   if n == "[Anonymous]" { return 0 }
   if n == "[heap]" { return 1 }
   if str.startswith(n, "[") { return 2 }
   3
}

fn _group_before(str a, str b) bool {
   def int ra = _rank(a)
   def int rb = _rank(b)
   if ra != rb { return ra < rb }
   a < b
}

fn _fields(str line) list {
   mut out = []
   def parts = line.split(" ")
   mut int i = 0
   while i < parts.len {
      def str p = to_str(parts.get(i, ""))
      if p.len > 0 { out = out.append(p) }
      i += 1
   }
   out
}

;; ---- scan levels ----------------------------------------------------------
;; Level record (8 words in _lv): kind (1 list, 2 snapshot), n, addrs, types,
;; vals, snapshot regions, snapshot region count, truncated flag.
;; A list stores per entry: address (i64), type (u8) and bits (i64).
;; A snapshot stores per region: start, size, buffer, plus the scan types.
mut int _lv = 0
mut int _depth = 0

fn _lvp() ptr {
   if _lv == 0 { _lv = int(zalloc(MAX_DEPTH * 64)) }
   ptr_add(0, _lv)
}

fn _lget(int level, int field) int { load64(_lvp(), (level * 8 + field) * 8) }

fn _lset(int level, int field, int v) any { store64(_lvp(), v, (level * 8 + field) * 8) }

fn _free_level(int level) any {
   if _lget(level, 2) != 0 { free(ptr_add(0, _lget(level, 2))) }
   if _lget(level, 3) != 0 { free(ptr_add(0, _lget(level, 3))) }
   if _lget(level, 4) != 0 { free(ptr_add(0, _lget(level, 4))) }
   def int snaps = _lget(level, 5)
   if snaps != 0 {
      def int nreg = _lget(level, 6)
      mut int r = 0
      while r < nreg {
         def int b = load64(ptr_add(0, snaps), r * 24 + 16)
         if b != 0 { free(ptr_add(0, b)) }
         r += 1
      }
      free(ptr_add(0, snaps))
   }
   mut int f = 0
   while f < 8 {
      _lset(level, f, 0)
      f += 1
   }
   0
}

fn reset() any {
   "Drops every scan level."
   mut int d = _depth
   while d > 0 {
      d -= 1
      _free_level(d)
   }
   _depth = 0
   0
}

fn previous() bool {
   "Goes back one scan level(the first level stays). Returns false when there is nothing to go back to."
   if _depth <= 1 { return false }
   _depth -= 1
   _free_level(_depth)
   true
}

fn depth() int { "Returns how many scan levels exist(0 before the first scan)." _depth }

fn is_snapshot() bool { "True when the current level is a value-less first scan(a byte snapshot)." _depth > 0 && _lget(_depth - 1, 0) == 2 }

fn truncated() bool { "True when the current level stopped at MAX_RESULTS." _depth > 0 && _lget(_depth - 1, 7) != 0 }

fn _count() int {
   ;; Internal name: `count` is also core's list helper inside this module.
   if _depth == 0 || _lget(_depth - 1, 0) != 1 { return 0 }
   _lget(_depth - 1, 1)
}

fn count() int { "Returns the number of results of the current level(0 for a snapshot)." _count() }

fn snapshot_bytes() int {
   "Returns the bytes held by the current snapshot level."
   if !is_snapshot() { return 0 }
   def int snaps = _lget(_depth - 1, 5)
   mut int total = 0
   mut int r = 0
   while r < _lget(_depth - 1, 6) {
      total += load64(ptr_add(0, snaps), r * 24 + 8)
      r += 1
   }
   total
}

fn addr_at(int i) int {
   "Returns the address of result `i`."
   (i >= 0 && i < _count()) ? load64(ptr_add(0, _lget(_depth - 1, 2)), i * 8) : 0
}

fn type_at(int i) int {
   "Returns the type index of result `i`."
   (i >= 0 && i < _count()) ? load8(ptr_add(0, _lget(_depth - 1, 3)), i) : -1
}

fn _val_at(int i) int { load64(ptr_add(0, _lget(_depth - 1, 4)), i * 8) }

fn read_value_text(int addr, int t) str {
   "Reads the current value at `addr` as type `t` and formats it, or returns \"??\" when unreadable."
   def ptr b = read_bytes(addr, type_size(t))
   if !b { return "??" }
   def int bits = _bits(b, 0, t)
   free(b)
   decode_text(bits, t)
}

fn value_text(int i) str { "Reads and formats the current value of result `i`." read_value_text(addr_at(i), type_at(i)) }

;; Growing output list of the level being built.
mut int _o_addrs = 0
mut int _o_types = 0
mut int _o_vals = 0
mut int _o_n = 0
mut int _o_cap = 0
mut int _o_trunc = 0

fn _out_begin() any {
   _o_cap = 4096
   _o_n = 0
   _o_trunc = 0
   _o_addrs = int(malloc(_o_cap * 8))
   _o_types = int(malloc(_o_cap))
   _o_vals = int(malloc(_o_cap * 8))
   0
}

fn _out_push(int addr, int t, int bits) bool {
   if _o_n >= MAX_RESULTS {
      _o_trunc = 1
      return false
   }
   if _o_n >= _o_cap {
      _o_cap = _o_cap * 2
      _o_addrs = int(realloc(ptr_add(0, _o_addrs), _o_cap * 8))
      _o_types = int(realloc(ptr_add(0, _o_types), _o_cap))
      _o_vals = int(realloc(ptr_add(0, _o_vals), _o_cap * 8))
   }
   store64(ptr_add(0, _o_addrs), addr, _o_n * 8)
   store8(ptr_add(0, _o_types), t, _o_n)
   store64(ptr_add(0, _o_vals), bits, _o_n * 8)
   _o_n += 1
   true
}

fn _push_level_list() any {
   if _depth >= MAX_DEPTH {
      ;; Drop the oldest level after the first so the baseline survives.
      _free_level(1)
      mut int l = 1
      while l < _depth - 1 {
         mut int f = 0
         while f < 8 {
            _lset(l, f, _lget(l + 1, f))
            f += 1
         }
         l += 1
      }
      _depth -= 1
      mut int f2 = 0
      while f2 < 8 { _lset(_depth, f2, 0) f2 += 1 }
   }
   _lset(_depth, 0, 1)
   _lset(_depth, 1, _o_n)
   _lset(_depth, 2, _o_addrs)
   _lset(_depth, 3, _o_types)
   _lset(_depth, 4, _o_vals)
   _lset(_depth, 7, _o_trunc)
   _depth += 1
   0
}

fn _types_for(str type_name) list {
   if type_name == "" || type_name == "Auto" { return _auto_types(_pv_type) }
   def int t = type_index(type_name)
   if t < 0 { return _auto_types(_pv_type) }
   [t]
}

fn scan(list regions, str value, str mode, str type_name="Auto") bool {
   "Runs the first scan over `regions` ([[start, end], ...]) when no level exists, otherwise a next scan over the current results. `value` is required for Exact and the 'by' modes; other first scans keep a byte snapshot. Returns false and sets last_error on failure."
   _err = ""
   if _fd < 0 { _err = "not attached" return false }
   def int m = _mode_index(mode)
   if m < 0 { _err = "unknown mode " + mode return false }
   def bool needs_value = m == 0 || m == 5 || m == 8
   def bool have = parse_value(value)
   if needs_value && !have { _err = "cannot parse value '" + value + "'" return false }
   if !have {
      _pv_type = 2
      _pv_int = 0
      _pv_float = 0.0
      _pv_is_float = 0
   }
   def list types = _types_for(type_name)
   if _depth == 0 { return _first_scan(regions, m, types) }
   _next_scan(m, types)
}

;; Chunked region reader: reads up to 4 MiB at a time with an 8-byte overlap
;; so values straddling a chunk boundary are seen once.
def int CHUNK = 4194304

fn _first_scan(list regions, int m, list types) bool {
   if regions.len == 0 { _err = "no enabled regions" return false }
   if m != 0 { return _snapshot(regions, types) }
   _out_begin()
   def ptr buf = malloc(CHUNK + 16)
   mut int ri = 0
   while ri < regions.len && _o_trunc == 0 {
      def any r = regions.get(ri, 0)
      ri += 1
      def int start = int(r.get(0, 0))
      def int end = int(r.get(1, 0))
      mut int pos = start
      while pos < end && _o_trunc == 0 {
         def int want = min(CHUNK + 8, end - pos)
         def int got = _read_into(buf, pos, want)
         if got <= 0 {
            ;; Skip an unreadable page.
            pos = (pos | 4095) + 1
            continue
         }
         def int limit = got == want && pos + want < end ? got - 8 : got
         _scan_exact_chunk(buf, pos, limit, got, types)
         pos += max(1, limit)
      }
   }
   free(buf)
   _push_level_list()
   true
}

;; Exact matches in buf[0..limit) (values may extend to `got`). Each type
;; steps by its own size from an aligned address, as smem does.
fn _scan_exact_chunk(ptr buf, int base, int limit, int got, list types) any {
   mut int ti = 0
   while ti < types.len && _o_trunc == 0 {
      def int t = int(types.get(ti, -1))
      ti += 1
      if t < 0 { continue }
      def int want = _encode(t)
      if _enc_ok == 0 { continue }
      def int n = type_size(t)
      def int c = _cls(t)
      mut int off = (n - (base % n)) % n
      if c == 2 || c == 3 {
         def f64 wv = _num(want, t)
         while off < limit && off + n <= got {
            def int bits = _bits(buf, off, t)
            if _feq(_num(bits, t), wv, t) { _out_push(base + off, t, bits) }
            off += n
         }
      } elif n == 8 {
         while off < limit && off + 8 <= got {
            if load64(buf, off) == want { _out_push(base + off, t, want) }
            off += 8
         }
      } elif n == 4 {
         while off < limit && off + 4 <= got {
            if (load32(buf, off) & 0xffffffff) == want { _out_push(base + off, t, want) }
            off += 4
         }
      } elif n == 2 {
         while off < limit && off + 2 <= got {
            if (load16(buf, off) & 0xffff) == want { _out_push(base + off, t, want) }
            off += 2
         }
      } else {
         while off < limit {
            if (load8(buf, off) & 255) == want { _out_push(base + off, t, want) }
            off += 1
         }
      }
   }
   0
}

fn _snapshot(list regions, list types) bool {
   mut int total = 0
   def int nreg = regions.len
   def ptr recs = zalloc(max(1, nreg) * 24)
   mut int kept = 0
   mut int ri = 0
   while ri < nreg {
      def any r = regions.get(ri, 0)
      ri += 1
      def int start = int(r.get(0, 0))
      def int size = int(r.get(1, 0)) - start
      if size <= 0 || total + size > MAX_SNAPSHOT { continue }
      def ptr b = malloc(size)
      if !b { continue }
      def int got = _read_into(b, start, size)
      if got <= 0 {
         free(b)
         continue
      }
      store64(recs, start, kept * 24)
      store64(recs, got, kept * 24 + 8)
      store64(recs, int(b), kept * 24 + 16)
      kept += 1
      total += got
   }
   if kept == 0 {
      free(recs)
      _err = "no readable region"
      return false
   }
   _lset(_depth, 0, 2)
   _lset(_depth, 5, int(recs))
   _lset(_depth, 6, kept)
   _lset(_depth, 7, total >= MAX_SNAPSHOT ? 1 : 0)
   _depth += 1
   _snap_types = types
   true
}

mut any _snap_types = nil

fn _next_scan(int m, list types) bool {
   def int prev = _depth - 1
   _out_begin()
   if _lget(prev, 0) == 2 {
      ;; Snapshot: compare every cell of every scanned type with its old bytes.
      mut list st = types
      if is_list(_snap_types) && _snap_types.len > 0 { st = _snap_types }
      def int snaps = _lget(prev, 5)
      def int nreg = _lget(prev, 6)
      def ptr nb = malloc(CHUNK + 16)
      mut int r = 0
      while r < nreg && _o_trunc == 0 {
         def int start = load64(ptr_add(0, snaps), r * 24)
         def int size = load64(ptr_add(0, snaps), r * 24 + 8)
         def ptr old = ptr_add(0, load64(ptr_add(0, snaps), r * 24 + 16))
         r += 1
         mut int pos = 0
         while pos < size && _o_trunc == 0 {
            def int want = min(CHUNK, size - pos)
            def int got = _read_into(nb, start + pos, want)
            if got <= 0 { break }
            mut int ti = 0
            while ti < st.len {
               def int t = int(st.get(ti, 2))
               def int n = type_size(t)
               mut int off = (n - ((start + pos) % n)) % n
               while off + n <= got && _o_trunc == 0 {
                  def int ob = _bits(old, pos + off, t)
                  def int nbits = _bits(nb, off, t)
                  if _match(m, t, ob, nbits) { _out_push(start + pos + off, t, nbits) }
                  off += n
               }
               ti += 1
            }
            pos += got
         }
      }
      free(nb)
   } else {
      ;; Result list: re-read each entry through a 64 KiB page cache.
      def int n = _lget(prev, 1)
      def ptr addrs = ptr_add(0, _lget(prev, 2))
      def ptr tys = ptr_add(0, _lget(prev, 3))
      def ptr vals = ptr_add(0, _lget(prev, 4))
      def ptr page = malloc(65536 + 16)
      mut int page_at = -1
      mut int page_len = 0
      def bool filter = types.len == 1
      def int only = filter ? int(types.get(0, -1)) : -1
      mut int i = 0
      while i < n && _o_trunc == 0 {
         def int a = load64(addrs, i * 8)
         def int t = load8(tys, i)
         def int sz = type_size(t)
         if !filter || t == only {
            if page_at < 0 || a < page_at || a + sz > page_at + page_len {
               ;; The 64 KiB window can start in an unmapped hole before a
               ;; small mapping; then the read fails as a whole. Fall back to
               ;; the 4 KiB page, then to the cell itself.
               page_at = a & (0 - 65536)
               page_len = _read_into(page, page_at, 65536)
               if page_len <= 0 || a + sz > page_at + page_len {
                  page_at = a & (0 - 4096)
                  page_len = _read_into(page, page_at, 4096)
               }
               if page_len <= 0 || a + sz > page_at + page_len {
                  page_at = a
                  page_len = _read_into(page, a, sz)
               }
            }
            if a + sz <= page_at + page_len {
               def int nbits = _bits(page, a - page_at, t)
               if _match(m, t, load64(vals, i * 8), nbits) { _out_push(a, t, nbits) }
            }
         }
         i += 1
      }
      free(page)
   }
   _push_level_list()
   true
}

;; ---- writes and locks -----------------------------------------------------
fn set_one(int i, str value) bool {
   "Writes `value` to result `i`, encoded in that result's type."
   if !parse_value(value) { _err = "cannot parse value '" + value + "'" return false }
   def int t = type_at(i)
   def int bits = _encode(t)
   if _enc_ok == 0 { _err = value + " does not fit " + type_name(t) return false }
   _write_bits(addr_at(i), bits, t)
}

fn write_value(int addr, str value, int t) bool {
   "Writes `value` encoded as type index `t` at `addr`. Returns false and sets last_error when it does not parse, does not fit, or the write fails."
   _err = ""
   if _fd < 0 { _err = "not attached" return false }
   if t < 0 || t >= TYPE_NAMES.len { _err = "unknown type" return false }
   if !parse_value(value) { _err = "cannot parse value '" + value + "'" return false }
   def int bits = _encode(t)
   if _enc_ok == 0 { _err = value + " does not fit " + type_name(t) return false }
   if !_write_bits(addr, bits, t) { _err = "write failed at 0x" + str.to_hex(addr, 0) return false }
   true
}

fn read_value(int addr, int t) str {
   "Alias of read_value_text: the value at `addr` as type index `t`, or \"??\"."
   read_value_text(addr, t)
}

fn region_writable(any r) bool {
   "True when a load_maps region is writable(its perms contain w)."
   str.find(to_str(r.get(2, "")), "w") >= 0
}

fn region_shared(any r) bool {
   "True when a load_maps region is a shared mapping(perms end in s), e.g. memfd or SysV shm."
   def str pm = to_str(r.get(2, ""))
   pm.len >= 4 && str.str_slice(pm, 3, 4) == "s"
}

fn set_all(str value) bool {
   "Writes `value` to every result of the current level, each encoded in its own type. Returns false and sets last_error on the first failure."
   _err = ""
   if !parse_value(value) { _err = "cannot parse value '" + value + "'" return false }
   def int n = _count()
   if n == 0 { _err = "no results" return false }
   mut int i = 0
   while i < n {
      def int t = type_at(i)
      def int bits = _encode(t)
      if _enc_ok == 1 && !_write_bits(addr_at(i), bits, t) {
         _err = "write failed at 0x" + str.to_hex(addr_at(i), 0)
         return false
      }
      i += 1
   }
   true
}

mut int _lock_on = 0
mut str _lock_value = ""
mut int _lock_last = 0

fn lock_start(str value) bool {
   "Keeps writing `value` to the current results about every 100 ms(call lock_tick from the frame loop). Returns false when the value does not parse or there are no results."
   if !parse_value(value) { _err = "cannot parse value '" + value + "'" return false }
   if _count() == 0 { _err = "no results to lock" return false }
   _lock_value = value
   _lock_on = 1
   _lock_last = 0
   set_all(value)
}

fn lock_stop() any { "Stops the value lock." _lock_on = 0 }

fn lock_active() bool { "True while a lock is writing." _lock_on == 1 }

fn lock_tick() bool {
   "Writes the locked value when 100 ms have passed. Returns true when it wrote."
   if _lock_on == 0 || _fd < 0 { return false }
   def int now = ticks()
   if _lock_last != 0 && now - _lock_last < 100000000 { return false }
   _lock_last = now
   set_all(_lock_value)
}
