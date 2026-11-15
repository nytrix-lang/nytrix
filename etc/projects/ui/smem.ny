#!/usr/bin/env ny

;; Keywords: ui memory scanner process proc hex viewer tool
;; Port of smem (x3ric/smem): process memory scanner and visualizer for Linux /proc.

use core
use core.common as common
use core.str as str
use core.math (max, min, clamp, floor, ceil)
use core.os (exit, list_dir, file_read)
use core.os.args as args
use core.os.thread (thread_launch, mutex_new, mutex_lock, mutex_unlock)
use core.os.ui.window as window
use core.os.ui.window.consts as keyc
use core.os.ui.render as gfx
use core.os.ui.render.viewer.gui as gui

;; ---------------------------------------------------------------------------
;; Value types and encoding
;; ---------------------------------------------------------------------------

def VT_INT8    = "Int8"
def VT_INT16   = "Int16"
def VT_INT32   = "Int32"
def VT_INT64   = "Int64"
def VT_UINT8   = "UInt8"
def VT_UINT16  = "UInt16"
def VT_UINT32  = "UInt32"
def VT_UINT64  = "UInt64"
def VT_FLOAT32 = "Float32"
def VT_FLOAT64 = "Float64"
def VT_SIZE    = "Size"
def VT_POINTER = "Pointer"
def VT_BOOL    = "Bool"

def TYPE_NAMES = [
   "Auto", "Int8", "Int16", "Int32", "Int64",
   "UInt8", "UInt16", "UInt32", "UInt64",
   "Float32", "Float64", "Pointer", "Size", "Bool"
]

def SCAN_MODES = [
   "Exact", "Changed", "Unchanged",
   "Increased", "Increased or Greater", "Increased by",
   "Decreased", "Decreased or Less", "Decreased by"
]

fn ptr_size() int { 8 }

fn vt_new(str kind, any val) dict { {"k": kind, "v": val} }

fn vt_size(str kind) int {
   case kind {
      VT_INT8, VT_UINT8, VT_BOOL -> 1
      VT_INT16, VT_UINT16 -> 2
      VT_INT32, VT_UINT32, VT_FLOAT32 -> 4
      VT_INT64, VT_UINT64, VT_FLOAT64 -> 8
      VT_SIZE, VT_POINTER -> ptr_size()
      _ -> 0
   }
}

fn vt_type_string(str kind) str { kind }

fn vt_string_to_type(str s) any {
   case s {
      "Int8" -> vt_new(VT_INT8, 0)
      "Int16" -> vt_new(VT_INT16, 0)
      "Int32" -> vt_new(VT_INT32, 0)
      "Int64" -> vt_new(VT_INT64, 0)
      "UInt8" -> vt_new(VT_UINT8, 0)
      "UInt16" -> vt_new(VT_UINT16, 0)
      "UInt32" -> vt_new(VT_UINT32, 0)
      "UInt64" -> vt_new(VT_UINT64, 0)
      "Float32" -> vt_new(VT_FLOAT32, 0.0)
      "Float64" -> vt_new(VT_FLOAT64, 0.0)
      "Size" -> vt_new(VT_SIZE, 0)
      "Pointer" -> vt_new(VT_POINTER, 0)
      "Bool" -> vt_new(VT_BOOL, false)
      _ -> nil
   }
}

fn mask_u(int v, int bits) int {
   if bits >= 64 { return v }
   def m = (1 << bits) - 1
   v & m
}

fn sign_extend_u(int u, int bits) int {
   if bits >= 64 { return u }
   def m = (1 << bits) - 1
   mut x = u & m
   def sign = 1 << (bits - 1)
   if (x & sign) != 0 { x = x - (m + 1) }
   x
}

fn vt_to_bytes(dict vt) list {
   def k = to_str(vt.get("k", ""))
   def v = vt.get("v", 0)
   mut out = []
   case k {
      VT_INT8, VT_UINT8 -> {
         out = out.append(mask_u(int(v), 8))
      }
      VT_BOOL -> {
         out = out.append((v == true || int(v) != 0) ? 1 : 0)
      }
      VT_INT16, VT_UINT16 -> {
         def u = mask_u(int(v), 16)
         out = out.append(u & 255)
         out = out.append((u >> 8) & 255)
      }
      VT_INT32, VT_UINT32 -> {
         def u = mask_u(int(v), 32)
         mut i = 0
         while i < 4 {
            out = out.append((u >> (i * 8)) & 255)
            i += 1
         }
      }
      VT_FLOAT32 -> {
         def buf = malloc(4)
         if buf {
            store32_f32(buf, float(v), 0)
            mut i = 0
            while i < 4 {
               out = out.append(load8(buf, i))
               i += 1
            }
            free(buf)
         } else {
            mut i = 0
            while i < 4 { out = out.append(0) i += 1 }
         }
      }
      VT_INT64, VT_UINT64, VT_SIZE, VT_POINTER -> {
         def bits = (k == VT_SIZE || k == VT_POINTER) ? (ptr_size() * 8) : 64
         def n = bits / 8
         def buf = malloc(n)
         if buf {
            if n == 8 { store64(buf, int(v), 0) }
            else { store32(buf, mask_u(int(v), 32), 0) }
            mut i = 0
            while i < n {
               out = out.append(load8(buf, i))
               i += 1
            }
            free(buf)
         } else {
            mut i = 0
            while i < n { out = out.append(0) i += 1 }
         }
      }
      VT_FLOAT64 -> {
         def buf = malloc(8)
         if buf {
            store64_f64(buf, float(v), 0)
            mut i = 0
            while i < 8 {
               out = out.append(load8(buf, i))
               i += 1
            }
            free(buf)
         } else {
            mut i = 0
            while i < 8 { out = out.append(0) i += 1 }
         }
      }
      _ -> {}
   }
   out
}

fn read_le_u(any ptr, int off, int nbytes) int {
   if nbytes == 8 { return load64(ptr, off) }
   mut u = 0
   mut i = 0
   while i < nbytes {
      u = u | (int(load8(ptr, off + i)) << (i * 8))
      i += 1
   }
   u
}

fn vt_from_ptr(any ptr, int off, str kind) dict {
   case kind {
      VT_INT8 -> vt_new(kind, sign_extend_u(read_le_u(ptr, off, 1), 8))
      VT_UINT8 -> vt_new(kind, read_le_u(ptr, off, 1))
      VT_BOOL -> vt_new(kind, read_le_u(ptr, off, 1) != 0)
      VT_INT16 -> vt_new(kind, sign_extend_u(read_le_u(ptr, off, 2), 16))
      VT_UINT16 -> vt_new(kind, read_le_u(ptr, off, 2))
      VT_INT32 -> vt_new(kind, sign_extend_u(read_le_u(ptr, off, 4), 32))
      VT_UINT32 -> vt_new(kind, read_le_u(ptr, off, 4))
      VT_FLOAT32 -> vt_new(kind, float(load32_f32(ptr, off)))
      VT_INT64 -> vt_new(kind, load64(ptr, off))
      VT_UINT64 -> vt_new(kind, load64(ptr, off))
      VT_FLOAT64 -> vt_new(kind, float(load64_f64(ptr, off)))
      VT_SIZE, VT_POINTER -> {
         def n = ptr_size()
         vt_new(kind, n >= 8 ? load64(ptr, off) : read_le_u(ptr, off, 4))
      }
      _ -> vt_new(kind, 0)
   }
}

fn vt_from_bytes_list(list bytes, str kind) dict {
   def n = bytes.len
   mut buf = malloc(max(1, n))
   if !buf { return vt_new(kind, 0) }
   mut i = 0
   while i < n {
      store8(buf, int(bytes.get(i, 0)) & 255, i)
      i += 1
   }
   def out = vt_from_ptr(buf, 0, kind)
   free(buf)
   out
}

fn float32_eps() f64 { 0.00000011920928955078125 }
fn float64_eps() f64 { 0.0000000000000002220446049250313 }

fn vt_equals(dict a, dict b) bool {
   def ka = to_str(a.get("k", ""))
   def kb = to_str(b.get("k", ""))
   if ka != kb { return false }
   def va = a.get("v", 0)
   def vb = b.get("v", 0)
   case ka {
      VT_FLOAT32 -> abs(float(va) - float(vb)) < float32_eps()
      VT_FLOAT64 -> abs(float(va) - float(vb)) < float64_eps()
      VT_BOOL -> (va == true || int(va) != 0) == (vb == true || int(vb) != 0)
      _ -> int(va) == int(vb)
   }
}

fn vt_greater(dict a, dict b) bool {
   def ka = to_str(a.get("k", ""))
   def kb = to_str(b.get("k", ""))
   if ka != kb { return false }
   def va = a.get("v", 0)
   def vb = b.get("v", 0)
   case ka {
      VT_FLOAT32 -> !vt_equals(a, b) && float(va) > float(vb)
      VT_FLOAT64 -> !vt_equals(a, b) && float(va) > float(vb)
      VT_BOOL -> (va == true || int(va) != 0) && !(vb == true || int(vb) != 0)
      _ -> int(va) > int(vb)
   }
}

fn vt_less(dict a, dict b) bool {
   def ka = to_str(a.get("k", ""))
   def kb = to_str(b.get("k", ""))
   if ka != kb { return false }
   def va = a.get("v", 0)
   def vb = b.get("v", 0)
   case ka {
      VT_FLOAT32 -> !vt_equals(a, b) && float(va) < float(vb)
      VT_FLOAT64 -> !vt_equals(a, b) && float(va) < float(vb)
      VT_BOOL -> !(va == true || int(va) != 0) && (vb == true || int(vb) != 0)
      _ -> int(va) < int(vb)
   }
}

fn vt_add(dict a, dict b) any {
   def ka = to_str(a.get("k", ""))
   def kb = to_str(b.get("k", ""))
   if ka != kb { return nil }
   def va = a.get("v", 0)
   def vb = b.get("v", 0)
   case ka {
      VT_FLOAT32, VT_FLOAT64 -> vt_new(ka, float(va) + float(vb))
      VT_BOOL -> vt_new(ka, (va == true || int(va) != 0) || (vb == true || int(vb) != 0))
      _ -> vt_new(ka, int(va) + int(vb))
   }
}

fn vt_sub(dict a, dict b) any {
   def ka = to_str(a.get("k", ""))
   def kb = to_str(b.get("k", ""))
   if ka != kb { return nil }
   def va = a.get("v", 0)
   def vb = b.get("v", 0)
   case ka {
      VT_FLOAT32, VT_FLOAT64 -> vt_new(ka, float(va) - float(vb))
      VT_BOOL -> vt_new(ka, (va == true || int(va) != 0) && !(vb == true || int(vb) != 0))
      _ -> vt_new(ka, int(va) - int(vb))
   }
}

fn vt_comparator(str mode, dict oldv, dict newv, dict inp) bool {
   case mode {
      "Exact" -> vt_equals(newv, inp)
      "Changed" -> !vt_equals(newv, oldv)
      "Unchanged" -> vt_equals(newv, oldv)
      "Increased" -> vt_greater(newv, oldv)
      "Increased or Greater" -> vt_greater(newv, oldv) || vt_equals(newv, oldv)
      "Increased by" -> {
         def added = vt_add(oldv, inp)
         is_dict(added) ? vt_equals(newv, added) : false
      }
      "Decreased" -> vt_less(newv, oldv)
      "Decreased or Less" -> vt_less(newv, oldv) || vt_equals(newv, oldv)
      "Decreased by" -> {
         def subd = vt_sub(oldv, inp)
         is_dict(subd) ? vt_equals(newv, subd) : false
      }
      _ -> false
   }
}

fn vt_scan_types(dict v) list {
   def k = to_str(v.get("k", ""))
   case k {
      VT_INT8 -> [vt_new(VT_INT8, 0), vt_new(VT_INT16, 0), vt_new(VT_INT32, 0)]
      VT_INT16 -> [vt_new(VT_INT16, 0), vt_new(VT_INT8, 0), vt_new(VT_INT32, 0)]
      VT_INT32 -> [vt_new(VT_INT32, 0), vt_new(VT_INT64, 0), vt_new(VT_INT16, 0)]
      VT_INT64 -> [vt_new(VT_INT64, 0), vt_new(VT_INT32, 0)]
      VT_UINT8 -> [vt_new(VT_UINT8, 0), vt_new(VT_UINT16, 0), vt_new(VT_UINT32, 0)]
      VT_UINT16 -> [vt_new(VT_UINT16, 0), vt_new(VT_UINT8, 0), vt_new(VT_UINT32, 0)]
      VT_UINT32 -> [vt_new(VT_UINT32, 0), vt_new(VT_UINT64, 0), vt_new(VT_UINT16, 0)]
      VT_UINT64 -> [vt_new(VT_UINT64, 0), vt_new(VT_UINT32, 0)]
      VT_FLOAT32 -> [vt_new(VT_FLOAT32, 0.0), vt_new(VT_FLOAT64, 0.0)]
      VT_FLOAT64 -> [vt_new(VT_FLOAT64, 0.0), vt_new(VT_FLOAT32, 0.0)]
      VT_SIZE -> [vt_new(VT_SIZE, 0), vt_new(VT_UINT64, 0), vt_new(VT_INT64, 0)]
      VT_POINTER -> [vt_new(VT_POINTER, 0), vt_new(VT_UINT64, 0)]
      VT_BOOL -> [vt_new(VT_BOOL, false), vt_new(VT_UINT8, 0)]
      _ -> [vt_new(VT_INT32, 0)]
   }
}

fn parse_i64_checked(str s) any {
   def t = str.strip(s)
   if t.len == 0 { return nil }
   mut i = 0
   mut neg = false
   if str.startswith(t, "-") { neg = true i = 1 }
   elif str.startswith(t, "+") { i = 1 }
   if i >= t.len { return nil }
   mut v = 0
   while i < t.len {
      def c = load8(t, i)
      if c < 48 || c > 57 { return nil }
      v = v * 10 + (c - 48)
      i += 1
   }
   neg ? -v : v
}

fn parse_u64_hex(str s) any {
   def t = str.strip(s)
   if t.len == 0 { return nil }
   mut tt = t
   if str.startswith(tt, "0x") || str.startswith(tt, "0X") { tt = str.str_slice(tt, 2, tt.len) }
   if tt.len == 0 { return nil }
   mut v = 0
   mut i = 0
   while i < tt.len {
      def c = load8(tt, i)
      mut d = -1
      if c >= 48 && c <= 57 { d = c - 48 }
      elif c >= 65 && c <= 70 { d = c - 55 }
      elif c >= 97 && c <= 102 { d = c - 87 }
      if d < 0 { return nil }
      v = v * 16 + d
      i += 1
   }
   v
}

fn parse_bool_str(str s) any {
   def t = str.lower(str.strip(s))
   if t == "true" || t == "1" { return true }
   if t == "false" || t == "0" { return false }
   nil
}

fn parse_float_str(str s) any {
   def t = str.strip(s)
   if t.len == 0 { return nil }
   mut i = 0
   mut seen = false
   if str.startswith(t, "-") || str.startswith(t, "+") { i = 1 }
   mut digits = 0
   while i < t.len {
      def c = load8(t, i)
      if c >= 48 && c <= 57 { digits += 1 seen = true }
      elif c == 46 || c == 101 || c == 69 || c == 43 || c == 45 { seen = true }
      else { return nil }
      i += 1
   }
   if !seen || digits == 0 { return nil }
   str.atof(t)
}

fn vt_parse_user_value(str input) any {
   def t = str.strip(input)
   if t.len == 0 { return nil }
   def cpos = str.find(t, ":")
   if cpos > 0 {
      def pfx = str.lower(str.str_slice(t, 0, cpos))
      def val = str.str_slice(t, cpos + 1, t.len)
      case pfx {
         "bool", "boolean" -> {
            def b = parse_bool_str(val)
            b == nil ? nil : vt_new(VT_BOOL, b)
         }
         "byte", "b" -> {
            def n = parse_i64_checked(val)
            n == nil ? nil : vt_new(VT_UINT8, mask_u(int(n), 8))
         }
         "hex", "h" -> {
            def n = parse_u64_hex(val)
            if n == nil { return nil }
            def u = int(n)
            if u <= 255 { vt_new(VT_UINT8, u) }
            elif u <= 65535 { vt_new(VT_UINT16, u) }
            elif u <= 4294967295 { vt_new(VT_UINT32, u) }
            else { vt_new(VT_UINT64, u) }
         }
         "int8", "i8", "char" -> {
            def n = parse_i64_checked(val)
            n == nil ? nil : vt_new(VT_INT8, sign_extend_u(int(n), 8))
         }
         "int16", "i16", "short" -> {
            def n = parse_i64_checked(val)
            n == nil ? nil : vt_new(VT_INT16, sign_extend_u(int(n), 16))
         }
         "int32", "i32", "int" -> {
            def n = parse_i64_checked(val)
            n == nil ? nil : vt_new(VT_INT32, sign_extend_u(int(n), 32))
         }
         "int64", "i64", "long" -> {
            def n = parse_i64_checked(val)
            n == nil ? nil : vt_new(VT_INT64, int(n))
         }
         "float32", "f32", "float" -> {
            def n = parse_float_str(val)
            n == nil ? nil : vt_new(VT_FLOAT32, float(n))
         }
         "float64", "f64", "double" -> {
            def n = parse_float_str(val)
            n == nil ? nil : vt_new(VT_FLOAT64, float(n))
         }
         "size", "s" -> {
            def n = parse_i64_checked(val)
            n == nil ? nil : vt_new(VT_SIZE, int(n))
         }
         "ptr", "pointer" -> {
            def n = parse_i64_checked(val)
            if n == nil {
               def h = parse_u64_hex(val)
               h == nil ? nil : vt_new(VT_POINTER, int(h))
            } else { vt_new(VT_POINTER, int(n)) }
         }
         _ -> nil
      }
   } else {
      def ni = parse_i64_checked(t)
      if ni != nil {
         def n = int(ni)
         if n >= -128 && n <= 127 { return vt_new(VT_INT8, n) }
         if n >= -32768 && n <= 32767 { return vt_new(VT_INT16, n) }
         if n >= -2147483648 && n <= 2147483647 { return vt_new(VT_INT32, n) }
         return vt_new(VT_INT64, n)
      }
      def nf = parse_float_str(t)
      if nf != nil {
         def fv = float(nf)
         if abs(fv) <= 3.4028234663852886e38 { return vt_new(VT_FLOAT32, fv) }
         return vt_new(VT_FLOAT64, fv)
      }
      nil
   }
}

fn vt_to_display(dict vt) str {
   def k = to_str(vt.get("k", ""))
   def v = vt.get("v", 0)
   case k {
      VT_BOOL -> (v == true || int(v) != 0) ? "true" : "false"
      VT_FLOAT32, VT_FLOAT64 -> to_str(float(v))
      _ -> to_str(int(v))
   }
}


;; ---------------------------------------------------------------------------
;; Process memory I/O and /proc parsing
;; ---------------------------------------------------------------------------

fn mem_open_fd(int pid) int {
   if pid <= 0 { return -1 }
   def path = "/proc/" + to_str(pid) + "/mem"
   def fd = __open_raw(path, 2, 0)
   if fd < 0 { return -1 }
   fd
}

fn mem_close_fd(int fd) any {
   if fd >= 0 { __close(fd) }
   0
}

fn mem_read_buf(int fd, int addr, int len) any {
   if fd < 0 || len <= 0 { return 0 }
   def buf = malloc(len)
   if !buf { return 0 }
   mut off = 0
   mut ok = true
   while off < len {
      def n = __read_off_raw(fd, ptr_add(buf, off), len - off, addr + off)
      if n <= 0 { ok = false break }
      off += n
   }
   if !ok {
      free(buf)
      return 0
   }
   buf
}

fn mem_write_bytes(int fd, int addr, list bytes) bool {
   if fd < 0 { return false }
   def n = bytes.len
   if n <= 0 { return true }
   def buf = malloc(n)
   if !buf { return false }
   mut i = 0
   while i < n {
      store8(buf, int(bytes.get(i, 0)) & 255, i)
      i += 1
   }
   mut off = 0
   mut ok = true
   while off < n {
      def w = __write_off_raw(fd, ptr_add(buf, off), n - off, addr + off)
      if w <= 0 { ok = false break }
      off += w
   }
   free(buf)
   ok
}

fn hex_u64(str s) any { parse_u64_hex(s) }

fn parse_maps_line(str line) any {
   def t = str.strip(line)
   if t.len == 0 { return nil }
   mut parts = []
   mut cur = ""
   mut i = 0
   while i < t.len {
      def c = load8(t, i)
      if c == 32 || c == 9 {
         if cur.len > 0 {
            parts = parts.append(cur)
            cur = ""
         }
      } else {
         cur = cur + str.chr(c)
      }
      i += 1
   }
   if cur.len > 0 { parts = parts.append(cur) }
   if parts.len < 2 { return nil }
   def range = to_str(parts.get(0, ""))
   def perms = to_str(parts.get(1, ""))
   if str.find(perms, "r") < 0 { return nil }
   def dash = str.find(range, "-")
   if dash <= 0 { return nil }
   def s_start = str.str_slice(range, 0, dash)
   def s_end = str.str_slice(range, dash + 1, range.len)
   def a0 = hex_u64(s_start)
   def a1 = hex_u64(s_end)
   if a0 == nil || a1 == nil { return nil }
   def start = int(a0)
   def end = int(a1)
   if end <= start { return nil }
   mut name = "[Anonymous]"
   if parts.len >= 6 {
      mut nstr = ""
      mut j = 5
      while j < parts.len {
         if j > 5 { nstr = nstr + " " }
         nstr = nstr + to_str(parts.get(j, ""))
         j += 1
      }
      if nstr.len > 0 { name = nstr }
   }
   {"start": start, "end": end, "name": name}
}

fn maps_sort_key(str name) list {
   if name == "[Anonymous]" { return [0, name] }
   if name == "[heap]" { return [1, name] }
   if str.startswith(name, "[") { return [2, name] }
   [3, name]
}

fn group_less(dict a, dict b) bool {
   def ka = maps_sort_key(to_str(a.get("name", "")))
   def kb = maps_sort_key(to_str(b.get("name", "")))
   def ra = int(ka.get(0, 0))
   def rb = int(kb.get(0, 0))
   if ra != rb { return ra < rb }
   to_str(ka.get(1, "")) < to_str(kb.get(1, ""))
}

fn load_maps_groups(int pid) list {
   if pid <= 0 { return [] }
   def path = "/proc/" + to_str(pid) + "/maps"
   def res = file_read(path)
   if is_err(res) { return [] }
   def text = unwrap(res)
   mut by_name = dict(64)
   mut names = []
   mut line = ""
   mut i = 0
   def n = str.len(text)
   while i <= n {
      mut ch = 10
      if i < n { ch = load8(text, i) }
      if ch == 10 || i == n {
         def m = parse_maps_line(line)
         if is_dict(m) {
            def name = to_str(m.get("name", "[Anonymous]"))
            def reg = {"start": int(m.get("start", 0)), "end": int(m.get("end", 0))}
            if !by_name.contains(name) {
               by_name[name] = {"name": name, "enabled": true, "regions": []}
               names = names.append(name)
            }
            mut g = by_name.get(name, 0)
            if is_dict(g) {
               mut regs = g.get("regions", [])
               regs = regs.append(reg)
               g["regions"] = regs
               by_name[name] = g
            }
         }
         line = ""
      } else {
         if ch != 13 { line = line + str.chr(ch) }
      }
      i += 1
   }
   mut groups = []
   mut j = 0
   while j < names.len {
      def nm = to_str(names.get(j, ""))
      groups = groups.append(by_name.get(nm, {"name": nm, "enabled": true, "regions": []}))
      j += 1
   }
   ;; simple insertion sort
   mut a = 1
   while a < groups.len {
      def cur = groups.get(a, 0)
      mut b = a - 1
      while b >= 0 {
         def prev = groups.get(b, 0)
         if !is_dict(prev) || !is_dict(cur) || !group_less(cur, prev) { break }
         groups[b + 1] = prev
         b -= 1
      }
      groups[b + 1] = cur
      a += 1
   }
   groups
}

fn refresh_pid_items() list {
   mut items = []
   def entries = list_dir("/proc")
   if !is_list(entries) { return items }
   mut i = 0
   while i < entries.len {
      def name = to_str(entries.get(i, ""))
      mut is_pid = name.len > 0 && name.len <= 10
      mut j = 0
      while j < name.len && is_pid {
         def c = load8(name, j)
         if c < 48 || c > 57 { is_pid = false }
         j += 1
      }
      if is_pid {
         def pid = int(str.atoi(name))
         if pid > 0 {
            def comm_res = file_read("/proc/" + name + "/comm")
            if is_ok(comm_res) {
               def comm = str.strip(unwrap(comm_res))
               if comm.len > 0 {
                  items = items.append({"pid": pid, "name": comm})
               }
            }
         }
      }
      i += 1
   }
   ;; sort by name then pid (insertion sort)
   mut a = 1
   while a < items.len {
      def cur = items.get(a, 0)
      mut b = a - 1
      while b >= 0 {
         def prev = items.get(b, 0)
         def cn = to_str(cur.get("name", ""))
         def pn = to_str(prev.get("name", ""))
         mut swap = false
         if cn < pn { swap = true }
         elif cn == pn && int(cur.get("pid", 0)) < int(prev.get("pid", 0)) { swap = true }
         if !swap { break }
         items[b + 1] = prev
         b -= 1
      }
      items[b + 1] = cur
      a += 1
   }
   items
}


;; ---------------------------------------------------------------------------
;; Scanner state and operations
;; ---------------------------------------------------------------------------

fn new_scan_hist() list { [] }
fn new_scan_types_hist() list { [] }

fn enabled_regions(groups, selected_region) list {
   mut out = []
   mut i = 0
   while i < groups.len {
      def g = groups.get(i, 0)
      if is_dict(g) && (g.get("enabled", true) == true) {
         def gname = to_str(g.get("name", ""))
         if selected_region == nil || to_str(selected_region) == gname {
            def regs = g.get("regions", [])
            if is_list(regs) {
               mut j = 0
               while j < regs.len {
                  out = out.append(regs.get(j, 0))
                  j += 1
               }
            }
         }
      }
      i += 1
   }
   out
}

fn scan_reset(st) any {
   st["scan_history"] = new_scan_hist()
   st["scan_types_history"] = new_scan_types_hist()
   st["scan_results"] = []
   0
}

fn scan_previous(st) any {
   def sh = st.get("scan_history", [])
   def sth = st.get("scan_types_history", [])
   if is_list(sh) && is_list(sth) && sh.len > 1 {
      mut nsh = []
      mut nsth = []
      mut i = 0
      while i < sh.len - 1 { nsh = nsh.append(sh.get(i, 0)) i += 1 }
      i = 0
      while i < sth.len - 1 { nsth = nsth.append(sth.get(i, 0)) i += 1 }
      st["scan_history"] = nsh
      st["scan_types_history"] = nsth
      def prev = nsh.get(nsh.len - 1, dict(0))
      mut keys = []
      if is_dict(prev) {
         def ks = dict_keys(prev)
         if is_list(ks) {
            mut k = 0
            while k < ks.len {
               keys = keys.append(int(str.atoi(to_str(ks.get(k, "0")))))
               k += 1
            }
         }
      }
      st["scan_results"] = keys
   }
   0
}

fn results_from_map(map) list {
   mut keys = []
   if !is_dict(map) { return keys }
   def ks = dict_keys(map)
   if !is_list(ks) { return keys }
   mut i = 0
   while i < ks.len {
      keys = keys.append(int(str.atoi(to_str(ks.get(i, "0")))))
      i += 1
   }
   keys
}

fn first_scan(st, groups, selected_region, str scan_value, str scan_mode, any type_filter) any {
   def pval = vt_parse_user_value(scan_value)
   if !is_dict(pval) { return "Failed to parse value" }
   mut scan_types = []
   if is_list(type_filter) { scan_types = type_filter }
   else { scan_types = vt_scan_types(pval) }
   def regs = enabled_regions(groups, selected_region)
   if regs.len == 0 { return "No enabled regions" }
   def fd = int(st.get("mem_fd", -1))
   if fd < 0 { return "not attached" }
   mut baseline = dict(256)
   mut ri = 0
   while ri < regs.len {
      def reg = regs.get(ri, 0)
      if is_dict(reg) {
         def rstart = int(reg.get("start", 0))
         def rend = int(reg.get("end", 0))
         def size = rend - rstart
         if size > 0 && size <= 64 * 1024 * 1024 {
            def buf = mem_read_buf(fd, rstart, size)
            if buf {
               mut ti = 0
               while ti < scan_types.len {
                  def t = scan_types.get(ti, 0)
                  if is_dict(t) {
                     def kind = to_str(t.get("k", ""))
                     def tsz = vt_size(kind)
                     if tsz > 0 {
                        mut off = 0
                        while off + tsz <= size {
                           def addr = rstart + off
                           def v = vt_from_ptr(buf, off, kind)
                           if scan_mode == "Exact" {
                              if vt_equals(v, pval) {
                                 baseline[to_str(addr)] = {"v": v, "t": kind}
                              }
                           } else {
                              baseline[to_str(addr)] = {"v": v, "t": kind}
                           }
                           off += tsz
                        }
                     }
                  }
                  ti += 1
               }
               free(buf)
            }
         }
      }
      ri += 1
   }
   mut hist_entry = dict(baseline.len > 0 ? baseline.len : 8)
   def bks = dict_keys(baseline)
   if is_list(bks) {
      mut bi = 0
      while bi < bks.len {
         def k = to_str(bks.get(bi, ""))
         def ent = baseline.get(k, 0)
         if is_dict(ent) { hist_entry[k] = ent.get("v", 0) }
         bi += 1
      }
   }
   mut sh = st.get("scan_history", [])
   mut sth = st.get("scan_types_history", [])
   if !is_list(sh) { sh = [] }
   if !is_list(sth) { sth = [] }
   sh = sh.append(hist_entry)
   sth = sth.append(baseline)
   st["scan_history"] = sh
   st["scan_types_history"] = sth
   st["scan_results"] = results_from_map(baseline)
   nil
}

fn next_scan(st, str scan_value, str scan_mode, any type_filter) any {
   mut sth = st.get("scan_types_history", [])
   if !is_list(sth) || sth.len == 0 { return "No baseline" }
   def pval = vt_parse_user_value(scan_value)
   if !is_dict(pval) { return "Failed to parse value" }
   mut scan_types = []
   if is_list(type_filter) { scan_types = type_filter }
   else { scan_types = vt_scan_types(pval) }
   def fd = int(st.get("mem_fd", -1))
   if fd < 0 { return "not attached" }
   def prev = sth.get(sth.len - 1, dict(0))
   if !is_dict(prev) { return "No baseline" }
   mut new_map = dict(128)
   def ks = dict_keys(prev)
   if is_list(ks) {
      mut ki = 0
      while ki < ks.len {
         def kaddr = to_str(ks.get(ki, ""))
         def addr = int(str.atoi(kaddr))
         def ent = prev.get(kaddr, 0)
         mut oldv = 0
         if is_dict(ent) { oldv = ent.get("v", 0) }
         if is_dict(oldv) {
            mut ti = 0
            while ti < scan_types.len {
               def t = scan_types.get(ti, 0)
               if is_dict(t) {
                  def kind = to_str(t.get("k", ""))
                  def tsz = vt_size(kind)
                  if tsz > 0 {
                     def buf = mem_read_buf(fd, addr, tsz)
                     if buf {
                        def nv = vt_from_ptr(buf, 0, kind)
                        if vt_comparator(scan_mode, oldv, nv, pval) {
                           new_map[kaddr] = {"v": nv, "t": kind}
                        }
                        free(buf)
                     }
                  }
               }
               ti += 1
            }
         }
         ki += 1
      }
   }
   mut hist_entry = dict(new_map.len > 0 ? new_map.len : 8)
   def nks = dict_keys(new_map)
   if is_list(nks) {
      mut ni = 0
      while ni < nks.len {
         def k = to_str(nks.get(ni, ""))
         def ent = new_map.get(k, 0)
         if is_dict(ent) { hist_entry[k] = ent.get("v", 0) }
         ni += 1
      }
   }
   mut sh = st.get("scan_history", [])
   if !is_list(sh) { sh = [] }
   sh = sh.append(hist_entry)
   sth = sth.append(new_map)
   st["scan_history"] = sh
   st["scan_types_history"] = sth
   st["scan_results"] = results_from_map(new_map)
   nil
}

fn address_set_all(st, str scan_value) any {
   def pval = vt_parse_user_value(scan_value)
   if !is_dict(pval) { return "Bad value" }
   def fd = int(st.get("mem_fd", -1))
   if fd < 0 { return "not attached" }
   def bytes = vt_to_bytes(pval)
   def results = st.get("scan_results", [])
   if !is_list(results) { return nil }
   mut i = 0
   while i < results.len {
      def addr = int(results.get(i, 0))
      if !mem_write_bytes(fd, addr, bytes) {
         return "write failed 0x" + to_str(addr)
      }
      i += 1
   }
   nil
}

;; Lock thread shared state
mut _lock_mu = 0
mut _lock_active = false
mut _lock_pid = 0
mut _lock_bytes = []
mut _lock_results = []

fn lock_thread_main(any _arg) any {
   while true {
      mut bytes = []
      mut addrs = []
      mut pid = 0
      mut active = false
      if _lock_mu { mutex_lock(_lock_mu) }
      active = _lock_active
      pid = _lock_pid
      bytes = _lock_bytes
      addrs = _lock_results
      if _lock_mu { mutex_unlock(_lock_mu) }
      if active && pid > 0 && is_list(bytes) && bytes.len > 0 && is_list(addrs) && addrs.len > 0 {
         def fd = mem_open_fd(pid)
         if fd >= 0 {
            mut i = 0
            while i < addrs.len {
               mem_write_bytes(fd, int(addrs.get(i, 0)), bytes)
               i += 1
            }
            mem_close_fd(fd)
         }
      }
      ;; ~100ms
      mut spin = 0
      while spin < 8000000 { spin += 1 }
   }
   0
}

fn start_lock(st, str scan_value) any {
   def pval = vt_parse_user_value(scan_value)
   if !is_dict(pval) { return "Bad value" }
   if !_lock_mu { _lock_mu = mutex_new() }
   if _lock_mu { mutex_lock(_lock_mu) }
   _lock_bytes = vt_to_bytes(pval)
   _lock_results = st.get("scan_results", [])
   _lock_pid = int(st.get("pid", 0))
   _lock_active = true
   if _lock_mu { mutex_unlock(_lock_mu) }
   nil
}

fn stop_lock() any {
   if !_lock_mu { return 0 }
   mutex_lock(_lock_mu)
   _lock_active = false
   mutex_unlock(_lock_mu)
   0
}

fn update_lock_results(st) any {
   if !_lock_mu || !_lock_active { return 0 }
   mutex_lock(_lock_mu)
   _lock_results = st.get("scan_results", [])
   _lock_pid = int(st.get("pid", 0))
   mutex_unlock(_lock_mu)
   0
}


;; ---------------------------------------------------------------------------
;; UI helpers and app state
;; ---------------------------------------------------------------------------

fn hex_addr(int a) str { "0x" + str.to_hex(a, 0) }

fn choose_types_for_ui(sel_name) any {
   if sel_name == nil { return nil }
   def s = to_str(sel_name)
   if s == "" || s == "Auto" { return nil }
   def vt = vt_string_to_type(s)
   if !is_dict(vt) { return nil }
   [vt]
}

fn clamp_zoom(f64 z) f64 { clamp(z, 0.5, 16.0) }

fn init_app(int pid) dict {
   mut st = dict(64)
   st["pid"] = 0
   st["mem_fd"] = -1
   st["attached"] = false
   st["groups"] = []
   st["err"] = nil
   st["zoom"] = 1.0
   st["selected_region"] = nil
   st["scan_value"] = "0"
   st["scan_mode"] = "Exact"
   st["sel_type"] = "Auto"
   st["scan_history"] = []
   st["scan_types_history"] = []
   st["scan_results"] = []
   st["pid_query"] = ""
   st["pid_items"] = refresh_pid_items()
   st["pid_selected"] = 0
   st["maps_scroll_y"] = 0.0
   st["results_scroll_y"] = 0.0
   st["lock_started_thread"] = false
   st["view_mode"] = "auto" ;; auto/maps/results
   if pid > 0 {
      st["pid"] = pid
      st["pid_selected"] = pid
      attach_pid(st, pid)
   }
   st
}

fn detach_pid(st) any {
   stop_lock()
   def fd = int(st.get("mem_fd", -1))
   if fd >= 0 { mem_close_fd(fd) }
   st["mem_fd"] = -1
   st["attached"] = false
   st["pid"] = 0
   scan_reset(st)
   0
}

fn attach_pid(st, int pid) any {
   detach_pid(st)
   st["pid"] = pid
   st["groups"] = load_maps_groups(pid)
   def fd = mem_open_fd(pid)
   if fd >= 0 {
      st["mem_fd"] = fd
      st["attached"] = true
      st["err"] = nil
   } else {
      st["mem_fd"] = -1
      st["attached"] = false
      st["err"] = "attach failed (need root/capabilities?)"
   }
   scan_reset(st)
   0
}

fn ensure_lock_thread(st) any {
   if st.get("lock_started_thread", false) == true { return 0 }
   if !_lock_mu { _lock_mu = mutex_new() }
   st["lock_started_thread"] = true
   thread_launch(lock_thread_main, 0)
   0
}

fn run_scan(st) any {
   def tf = choose_types_for_ui(st.get("sel_type", "Auto"))
   mut err = nil
   def sh = st.get("scan_history", [])
   if !is_list(sh) || sh.len == 0 {
      err = first_scan(st,
         st.get("groups", []),
         st.get("selected_region", nil),
         to_str(st.get("scan_value", "0")),
         to_str(st.get("scan_mode", "Exact")),
         tf)
   } else {
      err = next_scan(st,
         to_str(st.get("scan_value", "0")),
         to_str(st.get("scan_mode", "Exact")),
         tf)
   }
   if err != nil { st["err"] = to_str(err) } else { st["err"] = nil update_lock_results(st) }
   0
}

fn do_previous(st) any {
   scan_previous(st)
   update_lock_results(st)
   0
}

fn do_reset(st) any {
   scan_reset(st)
   update_lock_results(st)
   st["err"] = nil
   0
}

fn do_reload_maps(st) any {
   def pid = int(st.get("pid", 0))
   if pid > 0 {
      st["groups"] = load_maps_groups(pid)
   }
   0
}

fn do_attach_selected(st) any {
   def pid = int(st.get("pid_selected", 0))
   if pid > 0 { attach_pid(st, pid) }
   0
}

fn do_quick_attach_query(st) any {
   def q = str.strip(to_str(st.get("pid_query", "")))
   def n = parse_i64_checked(q)
   if n != nil && int(n) > 0 {
      st["pid_selected"] = int(n)
      attach_pid(st, int(n))
   }
   0
}

fn filtered_pid_items(st) list {
   def items = st.get("pid_items", [])
   if !is_list(items) { return [] }
   def q = str.lower(str.strip(to_str(st.get("pid_query", ""))))
   mut out = []
   mut i = 0
   mut shown = 0
   while i < items.len && shown < 200 {
      def it = items.get(i, 0)
      if is_dict(it) {
         def pid = int(it.get("pid", 0))
         def name = to_str(it.get("name", ""))
         def pid_s = to_str(pid)
         if q == "" || str.find(str.lower(name), q) >= 0 || str.find(pid_s, q) >= 0 {
            out = out.append(it)
            shown += 1
         }
      }
      i += 1
   }
   out
}

fn draw_topbar(st, f64 ww) any {
   if !gui.begin_window("smem_top", "smem", 0.0, 0.0, ww, 74.0,
      {"titlebar": false, "resizable": false, "movable": false, "collapsible": false, "closable": false}) {
      gui.end_window()
      return 0
   }
   gui.push_id("topbar")
   gui.set_window_pos("smem_top", 0.0, 0.0)
   gui.set_window_size("smem_top", ww, 74.0)
   def attached = st.get("attached", false) == true
   def pid = int(st.get("pid", 0))
   if attached && pid > 0 {
      gui.layout_row([90.0, 80.0, 110.0, 140.0, 180.0, 160.0, 70.0, 70.0, 70.0, 160.0, 70.0, 70.0, 0.0], 4.0)
      gui.text_colored("PID " + to_str(pid), "ok")
      if gui.button("detach", "Detach", 0.0, 0.0) { detach_pid(st) }
      if gui.button("reload_maps", "Reload Maps", 0.0, 0.0) { do_reload_maps(st) }
      mut sel_type = to_str(st.get("sel_type", "Auto"))
      mut ti = 0
      mut sel_idx = 0
      while ti < TYPE_NAMES.len {
         if to_str(TYPE_NAMES.get(ti, "")) == sel_type { sel_idx = ti }
         ti += 1
      }
      sel_idx = gui.combo_box("type", "Type", TYPE_NAMES, sel_idx, 0.0, 10)
      st["sel_type"] = to_str(TYPE_NAMES.get(sel_idx, "Auto"))
      mut mode = to_str(st.get("scan_mode", "Exact"))
      mut mi = 0
      mut mode_idx = 0
      while mi < SCAN_MODES.len {
         if to_str(SCAN_MODES.get(mi, "")) == mode { mode_idx = mi }
         mi += 1
      }
      mode_idx = gui.combo_box("mode", "Mode", SCAN_MODES, mode_idx, 0.0, 10)
      st["scan_mode"] = to_str(SCAN_MODES.get(mode_idx, "Exact"))
      st["scan_value"] = gui.input_text("scan_value", "Value", to_str(st.get("scan_value", "0")), "value", 0.0)
      if gui.button("scan", "Scan", 0.0, 0.0) { run_scan(st) }
      if gui.button("prev", "Prev", 0.0, 0.0) { do_previous(st) }
      if gui.button("reset", "Reset", 0.0, 0.0) { do_reset(st) }
      st["zoom"] = gui.slider_float("zoom", "Zoom", float(st.get("zoom", 1.0)), 0.5, 16.0, 0.0)
      if gui.button("set", "Set", 0.0, 0.0) {
         def e = address_set_all(st, to_str(st.get("scan_value", "0")))
         if e != nil { st["err"] = to_str(e) } else { st["err"] = nil }
      }
      if gui.button("lock", "Lock", 0.0, 0.0) {
         ensure_lock_thread(st)
         def e = start_lock(st, to_str(st.get("scan_value", "0")))
         if e != nil { st["err"] = to_str(e) } else { st["err"] = nil }
      }
      def err = st.get("err", nil)
      if err != nil { gui.text_colored(to_str(err), "danger") } else { gui.label("") }
   } else {
      gui.layout_row([90.0, 220.0, 100.0, 80.0, 320.0, 80.0, 0.0], 4.0)
      gui.label("Attach")
      st["pid_query"] = gui.input_text("pid_query", "PID/name", to_str(st.get("pid_query", "")), "name or PID", 0.0)
      if gui.button("attach_pid", "Attach PID", 0.0, 0.0) { do_quick_attach_query(st) }
      if gui.button("refresh_pids", "Refresh", 0.0, 0.0) { st["pid_items"] = refresh_pid_items() }
      def fitems = filtered_pid_items(st)
      mut pick_labels = ["select"]
      mut pick_ids = [0]
      mut cur_pick = 0
      mut fi = 0
      while fi < fitems.len {
         def it = fitems.get(fi, 0)
         def p = int(it.get("pid", 0))
         def nm = to_str(it.get("name", ""))
         pick_labels = pick_labels.append(to_str(p) + "  " + nm)
         pick_ids = pick_ids.append(p)
         if p == int(st.get("pid_selected", 0)) { cur_pick = pick_labels.len - 1 }
         fi += 1
      }
      def new_pick = gui.combo_box("pid_pick", "Process", pick_labels, cur_pick, 0.0, 12)
      if new_pick > 0 && new_pick < pick_ids.len { st["pid_selected"] = int(pick_ids.get(new_pick, 0)) }
      if gui.button("attach_sel", "Attach", 0.0, 0.0) { do_attach_selected(st) }
      def err = st.get("err", nil)
      if err != nil { gui.text_colored(to_str(err), "danger") } else { gui.label("") }
   }
   gui.pop_id()
   gui.end_window()
   0
}


fn draw_results_panel(st, f64 x, f64 y, f64 w, f64 h) any {
   if !gui.begin_window("smem_results", "Results", x, y, w, h,
      {"titlebar": false, "resizable": false, "movable": false, "collapsible": false, "closable": false}) {
      gui.end_window()
      return 0
   }
   gui.set_window_pos("smem_results", x, y)
   gui.set_window_size("smem_results", w, h)
   gui.begin_scroll_area("results_scroll", w - 16.0, h - 16.0)
   def sth = st.get("scan_types_history", [])
   mut latest = dict(0)
   if is_list(sth) && sth.len > 0 { latest = sth.get(sth.len - 1, dict(0)) }
   def results = st.get("scan_results", [])
   if is_list(results) && is_dict(latest) {
      ;; sort a copy
      mut addrs = []
      mut i = 0
      while i < results.len { addrs = addrs.append(int(results.get(i, 0))) i += 1 }
      ;; insertion sort ascending
      mut a = 1
      while a < addrs.len {
         def cur = int(addrs.get(a, 0))
         mut b = a - 1
         while b >= 0 && int(addrs.get(b, 0)) > cur {
            addrs[b + 1] = addrs.get(b, 0)
            b -= 1
         }
         addrs[b + 1] = cur
         a += 1
      }
      gui.text_colored(to_str(addrs.len) + " results", "text_dim")
      mut shown = 0
      i = 0
      while i < addrs.len && shown < 4000 {
         def addr = int(addrs.get(i, 0))
         def ent = latest.get(to_str(addr), 0)
         mut kind = "?"
         mut tsize = 1
         mut valtxt = ""
         if is_dict(ent) {
            def v = ent.get("v", 0)
            kind = to_str(ent.get("t", "?"))
            tsize = vt_size(kind)
            if is_dict(v) { valtxt = vt_to_display(v) }
         }
         def label = hex_addr(addr) + "  [" + to_str(tsize) + "]  " + kind + " = " + valtxt
         if gui.button("res_" + to_str(i), label, w - 40.0, 22.0) {
            window.set_clipboard(st.get("win", 0), hex_addr(addr))
         }
         i += 1
         shown += 1
      }
      if addrs.len > shown { gui.text_colored("... truncated display ...", "warn") }
   } else {
      gui.label("No results")
   }
   gui.end_scroll_area()
   gui.end_window()
   0
}

fn draw_group_checkbox(st, int gi, dict g) bool {
   def name = to_str(g.get("name", ""))
   mut en = g.get("enabled", true) == true
   def full = "grp_en_" + to_str(gi)
   en = gui.checkbox(full, name, en)
   g["enabled"] = en
   en
}

fn draw_maps_panel(st, f64 x, f64 y, f64 w, f64 h) any {
   if !gui.begin_window("smem_maps", "Maps", x, y, w, h,
      {"titlebar": false, "resizable": false, "movable": false, "collapsible": false, "closable": false}) {
      gui.end_window()
      return 0
   }
   gui.set_window_pos("smem_maps", x, y)
   gui.set_window_size("smem_maps", w, h)
   gui.begin_scroll_area("maps_scroll", w - 16.0, h - 16.0)
   def groups = st.get("groups", [])
   if !is_list(groups) || groups.len == 0 {
      gui.text_colored("No maps loaded. Attach to a process.", "text_dim")
      gui.end_scroll_area()
      gui.end_window()
      return 0
   }
   mut gi = 0
   while gi < groups.len {
      mut g = groups.get(gi, 0)
      if is_dict(g) {
         gui.push_id("g" + to_str(gi))
         draw_group_checkbox(st, gi, g)
         groups[gi] = g
         if g.get("enabled", true) == true {
            def header = to_str(g.get("name", ""))
            if gui.collapsing_header("hdr_" + to_str(gi), header, false) {
               def regs = g.get("regions", [])
               if is_list(regs) {
                  mut ri = 0
                  while ri < regs.len {
                     def reg = regs.get(ri, 0)
                     if is_dict(reg) {
                        def rs = int(reg.get("start", 0))
                        def re = int(reg.get("end", 0))
                        def size = re - rs
                        if size > 0 {
                           gui.text_colored(hex_addr(rs) + "-" + hex_addr(re) + "  " + to_str(size) + " bytes", "text_dim")
                           ;; Compact byte preview (capped) to keep UI responsive
                           def zoom = clamp(float(st.get("zoom", 1.0)), 0.5, 16.0)
                           def bsz = max(3.0, min(18.0, 6.0 * zoom))
                           def max_preview = 256
                           def preview_n = min(size, max_preview)
                           def pbuf = mem_read_buf(int(st.get("mem_fd", -1)), rs, preview_n)
                           if pbuf {
                              mut px = 0
                              mut row_str = ""
                              while px < preview_n {
                                 def b = int(load8(pbuf, px))
                                 mut h = str.to_hex(b & 255, 2)
                                 if px > 0 && (px % 16) == 0 {
                                    gui.text_colored(row_str, "text")
                                    row_str = ""
                                 }
                                 if row_str.len > 0 { row_str = row_str + " " }
                                 row_str = row_str + h
                                 px += 1
                              }
                              if row_str.len > 0 { gui.text_colored(row_str, "text") }
                              free(pbuf)
                           } else {
                              gui.text_colored("(unreadable)", "text_dim")
                           }
                           if size > max_preview {
                              gui.text_colored("... " + to_str(size - max_preview) + " bytes not previewed ...", "text_faint")
                           }
                           if gui.small_button("cp_" + to_str(gi) + "_" + to_str(ri), "Copy " + hex_addr(rs), 0.0, 0.0) {
                              window.set_clipboard(st.get("win", 0), hex_addr(rs))
                           }
                        }
                     }
                     ri += 1
                  }
               }
            }
         }
         gui.pop_id()
      }
      gi += 1
   }
   st["groups"] = groups
   gui.end_scroll_area()
   gui.end_window()
   0
}

fn draw_main_content(st, f64 ww, f64 wh) any {
   def y0 = 74.0
   def ch = max(40.0, wh - y0)
   def sth = st.get("scan_types_history", [])
   def res = st.get("scan_results", [])
   def has_results = is_list(sth) && sth.len > 0 && is_list(res) && res.len > 0
   if has_results {
      draw_results_panel(st, 0.0, y0, ww, ch)
   } else {
      draw_maps_panel(st, 0.0, y0, ww, ch)
   }
   0
}

fn handle_hotkeys(st, any win) any {
   if window.key_pressed(win, keyc.KEY_F2) { run_scan(st) }
   if window.key_pressed(win, keyc.KEY_F3) { do_previous(st) }
   if window.key_pressed(win, keyc.KEY_F4) { do_reset(st) }
   if window.key_pressed(win, keyc.KEY_F5) { do_reload_maps(st) }
   if window.key_pressed(win, keyc.KEY_F11) { st["zoom"] = clamp_zoom(float(st.get("zoom", 1.0)) - 0.25) }
   if window.key_pressed(win, keyc.KEY_F12) { st["zoom"] = clamp_zoom(float(st.get("zoom", 1.0)) + 0.25) }
   0
}

fn main() int {
   mut pid = 0
   def av = args.args()
   if is_list(av) && av.len > 1 {
      ;; argv may include program/script; pick last numeric argument as PID
      mut i = av.len - 1
      while i >= 0 {
         def a = to_str(av.get(i, ""))
         def n = parse_i64_checked(a)
         if n != nil && int(n) > 0 { pid = int(n) break }
         i -= 1
      }
   }
   if common.env_truthy("NY_UI_HEADLESS") {
      gfx.set_backend_type(gfx.BACKEND_MOCK)
   } else {
      gfx.apply_backend_env()
      gfx.apply_backend_argv()
   }
   def win = gfx.init_window(1100, 720, "smem", keyc.WINDOW_CENTER | keyc.WINDOW_FOCUS_ON_SHOW, false, true, 1)
   if !win { print("smem: window init failed") return 1 }
   window.set_exit_key(win, keyc.KEY_NULL)

   def mock_backend = (is_dict(win) && !!win.get("_mock", false)) || gfx.get_active_backend() == gfx.BACKEND_MOCK
   if mock_backend && !common.env_truthy("NYTRIX_UI_REPLAY") {
      if gfx.begin_frame_clear(gfx.color_hex("#060607")) { gfx.end_frame() }
      gfx.close_window()
      return 0
   }

   def fonts = ["etc/assets/fonts/jetbrains.ttf", "etc/assets/fonts/monocraft.ttf"]
   def font_body = gfx.font_load_first(fonts, 16)
   def font_title = gfx.font_load_first(fonts, 18)
   def font_small = gfx.font_load_first(fonts, 14)
   gui.set_fonts(font_body, font_title, font_small)

   mut st = init_app(pid)
   st["win"] = win
   _lock_mu = mutex_new()

   mut mock_frames = 0
   while !gfx.window_should_close(win) {
      if mock_backend && !common.env_truthy("NYTRIX_UI_REPLAY") {
         if mock_frames >= 2 {
            gfx.close_window()
            break
         }
      }
      handle_hotkeys(st, win)
      def fb = gfx.framebuffer_size_f64(1100, 720)
      def ww = max(320.0, float(fb.get(0, 1100)))
      def wh = max(240.0, float(fb.get(1, 720)))

      if !gfx.begin_frame_clear(gfx.color_hex("#060607")) { continue }
      gfx.set_ortho_2d(0.0, ww, 0.0, wh)
      gui.begin_frame(win, font_body, ww, wh)
      draw_topbar(st, ww)
      draw_main_content(st, ww, wh)
      gui.end_frame()
      gfx.end_frame()
      if mock_backend { mock_frames += 1 }
   }

   detach_pid(st)
   gfx.close_window()
   0
}

main()
