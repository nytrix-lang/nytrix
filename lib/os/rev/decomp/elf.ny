;; Keywords: elf sections symbols relocations imports disassembly flirt
;; ELF loading, recovery, symbols, imports, disassembly, and FLIRT signatures.
module std.os.rev.decomp.elf *

use std.core
use std.core.str as str
use std.os as os
use std.os.path as path
use std.os.time as ostime
use std.os.disasm as dasm
use std.os.rev.strings as rev_strings
use std.os.rev.decomp.tools (tool_status, demangle)
use std.os.rev.decomp.elf_types (machine, file_type, symbol_bind, symbol_type, segment_perms)
use std.os.rev.decomp.bytes (_u16, _u32, _u64, _slice, _slice_list, _cstring)
use std.os.rev.decomp.source (_read, _source_data, _looks_path)
use std.os.rev.decomp.collections (_list_has, _append_unique, _append_all_unique, _symbols_intersect)
use std.os.rev.decomp.pe as pe
use std.os.rev.decomp.macho as macho

def SHT_SYMTAB = 2
def SHT_STRTAB = 3
def SHT_RELA = 4
def SHT_REL = 9
def SHT_DYNSYM = 11
def PT_LOAD = 1
def SHF_ALLOC = 2
def SHF_EXECINSTR = 4
mut _smt_archetype_proof_cache = dict()

fn _elf_safe_name(str n, str fallback) str {
   if n.len == 0 { return fallback }
   mut out = ""
   mut i = 0
   while i < n.len {
      def c = load8(n, i)
      out = out + ((str.ascii_is_alnum(c) || c == 95) ? chr(c) : "_")
      i += 1
   }
   out.len == 0 ? fallback : out
}

fn sleep(any seconds) any {
   "Pause the current thread for `seconds`; useful for paced reversing automation."
   ostime.sleep(int(seconds))
}

fn msleep(any ms) any {
   "Pause the current thread for `ms` milliseconds."
   ostime.msleep(int(ms))
}

fn _machine(int id) str {
   machine(id)
}

fn _elf_type(int id) str {
   file_type(id)
}

fn _sym_bind(int info) str {
   symbol_bind(info)
}

fn _sym_type(int info) str {
   symbol_type(info)
}

fn _hex(int v) str { "0x" + str.to_hex(v, 0) }

fn _seg_perms(int flags) str {
   segment_perms(flags)
}

fn _section_call_targets(dict bin, dict section, int max_bytes=512) list {
   "Find direct call destinations while recovering functions from one ELF section."
   def rows = disassemble(bin, section, max_bytes)
   def a = arch(bin)
   mut out = []
   mut i = 0
   while i < rows.len {
      def row = rows[i]
      if dasm.instruction_kind(a, row[1]) == "call" && str.find(row[2], "[") < 0 {
         def target = dasm.target_address(row, a)
         if target != 0 { out = out.append({"target": target, "name": "sub_" + str.to_hex(target, 0), "kind": "direct"}) }
      }
      i += 1
   }
   out
}

fn elf_header(any source) dict {
   "Parse an ELF header from a path, bytes, or an analysis record."
   def b = _source_data(source)
   if b.len < 16 { return {"ok": false, "format": "unknown", "reason": "too_small"} }
   if load8(b, 0) != 0x7f || load8(b, 1) != 69 || load8(b, 2) != 76 || load8(b, 3) != 70 {
      return {"ok": false, "format": "unknown", "reason": "not_elf"}
   }
   def cls = load8(b, 4)
   def le = load8(b, 5) != 2
   def bits = cls == 2 ? 64 : 32
   if bits == 64 {
      return {"ok": true, "format": "elf", "bits": 64, "little": le, "type": _elf_type(_u16(b, 16, le)),
         "machine": _machine(_u16(b, 18, le)), "machine_id": _u16(b, 18, le),
         "entry": _u64(b, 24, le), "phoff": _u64(b, 32, le), "shoff": _u64(b, 40, le),
         "flags": _u32(b, 48, le), "ehsize": _u16(b, 52, le), "phentsize": _u16(b, 54, le),
         "phnum": _u16(b, 56, le), "shentsize": _u16(b, 58, le), "shnum": _u16(b, 60, le),
      "shstrndx": _u16(b, 62, le)}
   }
   return {"ok": true, "format": "elf", "bits": 32, "little": le, "type": _elf_type(_u16(b, 16, le)),
      "machine": _machine(_u16(b, 18, le)), "machine_id": _u16(b, 18, le),
      "entry": _u32(b, 24, le), "phoff": _u32(b, 28, le), "shoff": _u32(b, 32, le),
      "flags": _u32(b, 36, le), "ehsize": _u16(b, 40, le), "phentsize": _u16(b, 42, le),
      "phnum": _u16(b, 44, le), "shentsize": _u16(b, 46, le), "shnum": _u16(b, 48, le),
   "shstrndx": _u16(b, 50, le)}
}

fn _section_table(str b, dict h) list {
   if !h.get("ok", false) { return [] }
   def le = h.get("little", true)
   def bits = h.get("bits", 64)
   def shoff = int(h.get("shoff", 0))
   def entsz = int(h.get("shentsize", 0))
   def n = int(h.get("shnum", 0))
   if shoff <= 0 || entsz <= 0 || n <= 0 { return [] }
   mut raw = []
   mut i = 0
   while i < n {
      def off = shoff + i * entsz
      if off + entsz > b.len { break }
      if bits == 64 {
         raw = raw.append({"index": i, "name_off": _u32(b, off, le), "type": _u32(b, off + 4, le),
               "flags": _u64(b, off + 8, le), "addr": _u64(b, off + 16, le), "offset": _u64(b, off + 24, le),
               "size": _u64(b, off + 32, le), "link": _u32(b, off + 40, le), "info": _u32(b, off + 44, le),
         "align": _u64(b, off + 48, le), "entsize": _u64(b, off + 56, le)})
      } else {
         raw = raw.append({"index": i, "name_off": _u32(b, off, le), "type": _u32(b, off + 4, le),
               "flags": _u32(b, off + 8, le), "addr": _u32(b, off + 12, le), "offset": _u32(b, off + 16, le),
               "size": _u32(b, off + 20, le), "link": _u32(b, off + 24, le), "info": _u32(b, off + 28, le),
         "align": _u32(b, off + 32, le), "entsize": _u32(b, off + 36, le)})
      }
      i += 1
   }
   def shidx = int(h.get("shstrndx", -1))
   mut names_base = -1
   if shidx >= 0 && shidx < raw.len {
      def sh = raw[shidx]
      names_base = int(sh.get("offset", 0))
   }
   mut out = []
   i = 0
   while i < raw.len {
      def s = raw[i]
      def noff = int(s.get("name_off", 0))
      out = out.append(s.set("name", names_base >= 0 ? _cstring(b, names_base + noff) : ""))
      i += 1
   }
   out
}

fn sections(any source) list {
   "Return section records. Loaded records return their cached sections; raw
   sources are parsed as ELF (or the containing format's loader)."
   if is_dict(source) && source.contains("sections") { return source.get("sections", []) }
   def b = _source_data(source)
   _section_table(b, elf_header(b))
}

fn section(any source, str name) dict {
   "Find one ELF section by name."
   def ss = is_dict(source) && source.contains("sections") ? source.get("sections", []) : sections(source)
   mut i = 0
   while i < ss.len {
      if ss[i].get("name", "") == name { return ss[i] }
      i += 1
   }
   dict()
}

fn section_bytes(any source, str name) str {
   "Return raw bytes for a section name."
   def b = _source_data(source)
   def s = section(source, name)
   _slice(b, int(s.get("offset", 0)), int(s.get("size", 0)))
}

fn segments(any source) list {
   "Return segment/load records. Loaded records return their cached segments;
   raw sources are parsed as ELF (or the containing format's loader)."
   if is_dict(source) && source.contains("segments") { return source.get("segments", []) }
   def b = _source_data(source)
   def h = elf_header(b)
   if !h.get("ok", false) { return [] }
   def le = h.get("little", true)
   def bits = h.get("bits", 64)
   def phoff = int(h.get("phoff", 0))
   def entsz = int(h.get("phentsize", 0))
   def n = int(h.get("phnum", 0))
   mut out = []
   mut i = 0
   while i < n {
      def off = phoff + i * entsz
      if off + entsz > b.len { break }
      if bits == 64 {
         out = out.append({"index": i, "type": _u32(b, off, le), "flags": _u32(b, off + 4, le),
               "offset": _u64(b, off + 8, le), "vaddr": _u64(b, off + 16, le),
               "filesz": _u64(b, off + 32, le), "memsz": _u64(b, off + 40, le), "align": _u64(b, off + 48, le),
         "load": _u32(b, off, le) == PT_LOAD})
      } else {
         out = out.append({"index": i, "type": _u32(b, off, le), "offset": _u32(b, off + 4, le),
               "vaddr": _u32(b, off + 8, le), "filesz": _u32(b, off + 16, le), "memsz": _u32(b, off + 20, le),
         "flags": _u32(b, off + 24, le), "align": _u32(b, off + 28, le), "load": _u32(b, off, le) == PT_LOAD})
      }
      i += 1
   }
   out
}

fn _symbols_from_section(str b, dict h, list ss, dict symsec) list {
   def le = h.get("little", true)
   def bits = h.get("bits", 64)
   def ent = int(symsec.get("entsize", bits == 64 ? 24 : 16))
   if ent <= 0 { return [] }
   def stridx = int(symsec.get("link", -1))
   mut tab_base = -1
   if stridx >= 0 && stridx < ss.len {
      def st = ss[stridx]
      tab_base = int(st.get("offset", 0))
   }
   mut out = []
   mut off = int(symsec.get("offset", 0))
   def end = off + int(symsec.get("size", 0))
   mut idx = 0
   while off + ent <= b.len && off + ent <= end {
      def name_off = _u32(b, off, le)
      mut info = 0
      mut shndx = 0
      mut value = 0
      mut size = 0
      if bits == 64 {
         info = load8(b, off + 4)
         shndx = _u16(b, off + 6, le)
         value = _u64(b, off + 8, le)
         size = _u64(b, off + 16, le)
      } else {
         value = _u32(b, off + 4, le)
         size = _u32(b, off + 8, le)
         info = load8(b, off + 12)
         shndx = _u16(b, off + 14, le)
      }
      def nm = tab_base >= 0 ? _cstring(b, tab_base + name_off) : ""
      if nm.len > 0 || value != 0 {
         out = out.append({"index": idx, "name": nm, "value": value, "size": size, "shndx": shndx,
         "bind": _sym_bind(info), "type": _sym_type(info), "source": symsec.get("name", "")})
      }
      off += ent
      idx += 1
   }
   out
}

fn symbols(any source) list {
   "Return symbol records. Loaded records return their cached symbols; raw
   sources are parsed as ELF (or the containing format's loader)."
   if is_dict(source) && source.contains("symbols") { return source.get("symbols", []) }
   def b = _source_data(source)
   def h = elf_header(b)
   def ss = _section_table(b, h)
   mut out = []
   mut i = 0
   while i < ss.len {
      def s = ss[i]
      def t = int(s.get("type", 0))
      if t == SHT_SYMTAB || t == SHT_DYNSYM {
         def part = _symbols_from_section(b, h, ss, s)
         mut j = 0
         while j < part.len { out = out.append(part[j]) j += 1 }
      }
      i += 1
   }
   out
}

fn _display_symbol(str name) str {
   def d = demangle(name)
   if d == name { return name }
   def p = str.find(d, "(")
   mut base = p > 0 ? str.strip(slice(d, 0, p, 1)) : d
   base = str.str_replace(base, "::", "_")
   base = str.str_replace(base, "~", "dtor_")
   base = str.str_replace(base, " ", "_")
   base
}

fn functions(any source) list {
   "Return recovered functions for loaded binaries, or function symbols for raw sources."
   if is_dict(source) && source.contains("functions") { return source.get("functions", []) }
   mut out = []
   def sy = is_dict(source) && source.contains("symbols") ? source.get("symbols", []) : symbols(source)
   mut i = 0
   while i < sy.len {
      def s = sy[i]
      if s.get("type", "") == "function" && int(s.get("value", 0)) > 0 {
         out = out.append(s)
      }
      i += 1
   }
   out
}

fn executable_sections(any source) list {
   "Return executable sections, usually `.init`, `.plt`, `.text`, `.fini`."
   def ss = is_dict(source) && source.contains("sections") ? source.get("sections", []) : sections(source)
   mut out = []
   mut i = 0
   while i < ss.len {
      def s = ss[i]
      def exe = s.contains("exec") ? s.get("exec", false) : (int(s.get("flags", 0)) & SHF_EXECINSTR) != 0
      if exe && int(s.get("size", 0)) > 0 {
         out = out.append(s)
      }
      i += 1
   }
   out
}

fn _section_for_addr(dict bin, int addr) dict {
   def xs = executable_sections(bin)
   mut i = 0
   while i < xs.len {
      def s = xs[i]
      def start = int(s.get("addr", 0))
      def end = start + int(s.get("size", 0))
      if addr >= start && addr < end { return s }
      i += 1
   }
   dict()
}

fn _any_section_for_addr(dict bin, int addr) dict {
   def xs = bin.get("sections", [])
   mut i = 0
   while i < xs.len {
      def s = xs[i]
      def start = int(s.get("addr", 0))
      def end = start + int(s.get("size", 0))
      if addr >= start && addr < end { return s }
      i += 1
   }
   dict()
}

fn _next_function_addr(list fs, int addr) int {
   mut best = 0
   mut i = 0
   while i < fs.len {
      def n = int(fs[i].get("value", fs[i].get("addr", 0)))
      if n > addr { return n }
      i += 1
   }
   best
}

fn _extent_from_functions(dict bin, dict f, list fs, int max_bytes=4096) dict {
   def start = int(f.get("value", f.get("addr", 0)))
   def declared = int(f.get("size", 0))
   def sec = _section_for_addr(bin, start)
   def sec_end = sec.len > 0 ? int(sec.get("addr", 0)) + int(sec.get("size", 0)) : start + max_bytes
   def next = _next_function_addr(fs, start)
   def src = f.get("source", "")
   def natural_end = (src == "exec_section" && next > 0) ? next : (declared > 0 ? start + declared : (next > 0 ? next : sec_end))
   mut end = natural_end
   if end <= start { end = start + max(1, max_bytes) }
   def clipped_by_cap = max_bytes > 0 && end > start + max_bytes
   if clipped_by_cap { end = start + max_bytes }
   if sec_end > start && end > sec_end { end = sec_end }
   {"name": f.get("name", "sub_" + str.to_hex(start, 0)), "start": start, "end": end,
      "size": max(0, end - start), "declared_size": declared, "inferred": declared <= 0,
   "next": next, "section": sec.get("name", ""), "clipped": clipped_by_cap && end < natural_end}
}

fn function_extent(any source, any fn_or_addr, int max_bytes=4096) dict {
   "Return one inferred function extent as `{start,end,size,inferred}`."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   def fs = bin.contains("functions") ? bin.get("functions", []) : recover_functions(bin)
   mut f = dict()
   if is_dict(fn_or_addr) { f = fn_or_addr }
   elif is_str(fn_or_addr) {
      mut i = 0
      while i < fs.len {
         if fs[i].get("name", "") == fn_or_addr { f = fs[i] break }
         i += 1
      }
   } else { f = function_at(bin.set("functions", fs), int(fn_or_addr)) }
   if f.len == 0 { return {"ok": false, "reason": "function_not_found", "start": 0, "end": 0, "size": 0} }
   _extent_from_functions(bin, f, fs, max_bytes).set("ok", true)
}

fn function_ranges(any source) list {
   "Return compact inferred function ranges for UI/reversing scripts."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   def fs = is_dict(source) && source.contains("functions") ? source.get("functions", []) : recover_functions(source)
   mut out = []
   mut i = 0
   while i < fs.len {
      out = out.append(_extent_from_functions(bin, fs[i], fs, 1 << 30))
      i += 1
   }
   out
}

fn _put_func(dict seen, list out, str name, int addr, int size, str source) list {
   if addr <= 0 || seen.get(to_str(addr), false) { return [seen, out] }
   seen = seen.set(to_str(addr), true)
   def rec = {"name": _elf_safe_name(name, "sub_" + str.to_hex(addr, 0)), "value": addr, "size": size, "type": "function", "source": source}
   mut sorted = []
   mut inserted = false
   mut i = 0
   while i < out.len {
      if !inserted && int(out[i].get("value", out[i].get("addr", 0))) > addr {
         sorted = sorted.append(rec)
         inserted = true
      }
      sorted = sorted.append(out[i])
      i += 1
   }
   if !inserted { sorted = sorted.append(rec) }
   out = sorted
   [seen, out]
}

fn _x86_recovery_padding_byte(int c) bool {
   c == 0x00 || c == 0x90 || c == 0x66 || c == 0x2e || c == 0x0f ||
   c == 0x1f || c == 0x44 || c == 0x80 || c == 0x84
}

fn _x86_recent_ret_padding(str data, int off, int pos) bool {
   mut j = pos - 1
   mut seen = 0
   while j >= 0 && seen < 16 {
      def c = load8(data, off + j)
      if c == 0xc3 || c == 0xcb || c == 0xc2 || c == 0xca { return true }
      if !_x86_recovery_padding_byte(c) { return false }
      j -= 1
      seen += 1
   }
   false
}

fn _x86_rip_entry_pattern(str data, int off, int pos, int cap) bool {
   if pos + 6 >= cap { return false }
   if load8(data, off + pos) != 0x48 || load8(data, off + pos + 1) != 0x8d { return false }
   def modrm = load8(data, off + pos + 2)
   modrm == 0x05 || modrm == 0x35 || modrm == 0x3d
}

fn _recover_x86_prologue_functions(dict bin, dict seen0, list out0, dict sec, int scan_bytes) list {
   def fam = dasm.arch_family(arch(bin))
   if fam != "x86" { return [seen0, out0] }
   def data = bin.get("data", "")
   def off = int(sec.get("offset", 0))
   def size = min(int(sec.get("size", 0)), scan_bytes)
   if off < 0 || off >= data.len || size <= 0 { return [seen0, out0] }
   def cap = min(size, data.len - off)
   def base = int(sec.get("addr", 0))
   mut seen = seen0
   mut out = out0
   mut i = 0
   while i + 3 < cap {
      mut pro = false
      mut pstart = i
      if (load8(data, off + i) == 0x55 && load8(data, off + i + 1) == 0x48 &&
         load8(data, off + i + 2) == 0x89 && load8(data, off + i + 3) == 0xe5){
         pro = true
      } elif (load8(data, off + i) == 0x55 && load8(data, off + i + 1) == 0x89 &&
         load8(data, off + i + 2) == 0xe5){
         pro = true
      } elif (i + 7 < cap && load8(data, off + i) == 0xf3 && load8(data, off + i + 1) == 0x0f &&
         load8(data, off + i + 2) == 0x1e && load8(data, off + i + 3) == 0xfa &&
         load8(data, off + i + 4) == 0x55 && load8(data, off + i + 5) == 0x48 &&
         load8(data, off + i + 6) == 0x89 && load8(data, off + i + 7) == 0xe5){
         pro = true
         pstart = i
      } elif (i + 3 < cap && load8(data, off + i) == 0xf3 && load8(data, off + i + 1) == 0x0f &&
         load8(data, off + i + 2) == 0x1e && load8(data, off + i + 3) == 0xfa &&
         _x86_recent_ret_padding(data, off, i)){
         pro = true
         pstart = i
      } elif _x86_recent_ret_padding(data, off, i) && _x86_rip_entry_pattern(data, off, i, cap) {
         pro = true
         pstart = i
      }
      if pro {
         def addr = base + pstart
         def add = _put_func(seen, out, "sub_" + str.to_hex(addr, 0), addr, 0, "prologue")
         seen = add[0]
         out = add[1]
      }
      i += 1
   }
   [seen, out]
}

fn recover_functions(any source, int scan_bytes=65536) list {
   "Recover function candidates from symbols, entry, executable sections, and direct call targets.
   This is intentionally conservative: it creates useful Nytrix decompile entry
   points without inventing fake function bodies."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   mut seen = dict()
   mut out = []
   def fs = bin.get("functions", [])
   mut i = 0
   while i < fs.len {
      def f = fs[i]
      def added = _put_func(seen, out, _display_symbol(f.get("name", "")), int(f.get("value", 0)), int(f.get("size", 0)), "symbol")
      seen = added[0]
      out = added[1]
      i += 1
   }
   def e = entry(bin)
   def add_entry = _put_func(seen, out, "entry_" + str.to_hex(e, 0), e, 0, "entry")
   seen = add_entry[0]
   out = add_entry[1]
   def xs = executable_sections(bin)
   i = 0
   while i < xs.len {
      def s = xs[i]
      def start = int(s.get("addr", 0))
      def sname = s.get("name", "")
      if sname != ".plt" && sname != ".plt.sec" && sname != ".got.plt" {
         def add_sec = _put_func(seen, out, sname + "_start", start, int(s.get("size", 0)), "exec_section")
         seen = add_sec[0]
         out = add_sec[1]
         def pros = _recover_x86_prologue_functions(bin, seen, out, s, scan_bytes)
         seen = pros[0]
         out = pros[1]
      }
      def calls = _section_call_targets(bin, s, min(scan_bytes, int(s.get("size", 0))))
      mut j = 0
      while j < calls.len {
         def ck = calls[j].get("kind", "")
         if ck != "import_plt" && ck != "import_indirect" {
            def ca = int(calls[j].get("target", 0))
            def add_call = _put_func(seen, out, calls[j].get("name", ""), ca, 0, "call_target")
            seen = add_call[0]
            out = add_call[1]
         }
         j += 1
      }
      i += 1
   }
   out
}

fn imports(any source) list {
   "Return imported/undefined symbols."
   mut out = []
   def sy = is_dict(source) && source.contains("symbols") ? source.get("symbols", []) : symbols(source)
   mut i = 0
   while i < sy.len {
      def s = sy[i]
      if int(s.get("shndx", 0)) == 0 && s.get("name", "").len > 0 {
         out = out.append(s.set("raw_name", s.get("name", "")).set("display_name", _display_symbol(s.get("name", ""))))
      }
      i += 1
   }
   out
}

fn _reloc_type_name(int machine, int typ) str {
   if machine == 62 {
      case typ {
         1 -> "x86_64_64"
         2 -> "x86_64_pc32"
         3 -> "x86_64_got32"
         4 -> "x86_64_plt32"
         5 -> "x86_64_copy"
         6 -> "x86_64_glob_dat"
         7 -> "x86_64_jump_slot"
         8 -> "x86_64_relative"
         9 -> "x86_64_gotpcrel"
         10 -> "x86_64_32"
         11 -> "x86_64_32s"
         16 -> "x86_64_pc64"
         18 -> "x86_64_size32"
         19 -> "x86_64_size64"
         24 -> "x86_64_gotpcrelx"
         25 -> "x86_64_rex_gotpcrelx"
         37 -> "x86_64_gotpcrel64"
         41 -> "x86_64_irelative"
         _ -> "x86_64_" + to_str(typ)
      }
   } else if machine == 183 {
      case typ {
         257 -> "aarch64_none"
         276 -> "aarch64_abs64"
         277 -> "aarch64_abs32"
         278 -> "aarch64_abs16"
         279 -> "aarch64_prel64"
         280 -> "aarch64_prel32"
         281 -> "aarch64_prel16"
         282 -> "aarch64_movw_uabs_g0"
         283 -> "aarch64_movw_uabs_g0_nc"
         284 -> "aarch64_movw_uabs_g1"
         285 -> "aarch64_movw_uabs_g1_nc"
         286 -> "aarch64_movw_uabs_g2"
         287 -> "aarch64_movw_uabs_g2_nc"
         288 -> "aarch64_movw_uabs_g3"
         295 -> "aarch64_ld_prel_lo19"
         296 -> "aarch64_adr_prel_lo21"
         297 -> "aarch64_adr_prel_pg_hi21"
         298 -> "aarch64_adr_prel_pg_hi21_nc"
         299 -> "aarch64_add_abs_lo12_nc"
         300 -> "aarch64_ldst8_abs_lo12_nc"
         301 -> "aarch64_tstbr14"
         302 -> "aarch64_condbr19"
         303 -> "aarch64_jump26"
         304 -> "aarch64_call26"
         305 -> "aarch64_ldst16_abs_lo12_nc"
         306 -> "aarch64_ldst32_abs_lo12_nc"
         307 -> "aarch64_ldst64_abs_lo12_nc"
         308 -> "aarch64_ldst128_abs_lo12_nc"
         309 -> "aarch64_adr_got_page"
         310 -> "aarch64_ld64_got_lo12_nc"
         313 -> "aarch64_ld32_got_lo12_nc"
         320 -> "aarch64_adr_gotpage_lo21"
         1024 -> "aarch64_copy"
         1025 -> "aarch64_glob_dat"
         1026 -> "aarch64_jump_slot"
         1027 -> "aarch64_relative"
         1028 -> "aarch64_tls_dtpmod"
         1029 -> "aarch64_tls_dtprel"
         1030 -> "aarch64_tls_tprel"
         1031 -> "aarch64_tlsdesc"
         1032 -> "aarch64_irelative"
         _ -> "aarch64_" + to_str(typ)
      }
   } else if machine == 40 {
      case typ {
         0 -> "arm_none"
         1 -> "arm_pc24"
         2 -> "arm_abs32"
         3 -> "arm_rel32"
         8 -> "arm_abs8"
         17 -> "arm_tls_dtpmod32"
         18 -> "arm_tls_dtpoff32"
         19 -> "arm_tls_tpoff32"
         20 -> "arm_copy"
         21 -> "arm_glob_dat"
         22 -> "arm_jump_slot"
         23 -> "arm_relative"
         24 -> "arm_gotoff32"
         26 -> "arm_got_brel"
         27 -> "arm_plt32"
         28 -> "arm_call"
         29 -> "arm_jump24"
         30 -> "arm_thm_jump24"
         38 -> "arm_target1"
         41 -> "arm_target2"
         42 -> "arm_prel31"
         160 -> "arm_irelative"
         _ -> "arm_" + to_str(typ)
      }
   } else if machine == 243 {
      case typ {
         0 -> "riscv_none"
         1 -> "riscv_32"
         2 -> "riscv_64"
         3 -> "riscv_relative"
         4 -> "riscv_copy"
         5 -> "riscv_jump_slot"
         6 -> "riscv_tls_dtpmod32"
         7 -> "riscv_tls_dtpmod64"
         8 -> "riscv_tls_dtprel32"
         9 -> "riscv_tls_dtprel64"
         10 -> "riscv_tls_tprel32"
         11 -> "riscv_tls_tprel64"
         12 -> "riscv_branch"
         13 -> "riscv_jal"
         14 -> "riscv_call"
         15 -> "riscv_call_plt"
         16 -> "riscv_got_hi20"
         18 -> "riscv_tls_gd_hi20"
         19 -> "riscv_pcrel_hi20"
         20 -> "riscv_pcrel_lo12_i"
         21 -> "riscv_pcrel_lo12_s"
         22 -> "riscv_hi20"
         23 -> "riscv_lo12_i"
         24 -> "riscv_lo12_s"
         25 -> "riscv_tprel_hi20"
         26 -> "riscv_tprel_lo12_i"
         27 -> "riscv_tprel_lo12_s"
         29 -> "riscv_add8"
         30 -> "riscv_add16"
         31 -> "riscv_add32"
         32 -> "riscv_add64"
         33 -> "riscv_sub8"
         34 -> "riscv_sub16"
         35 -> "riscv_sub32"
         36 -> "riscv_sub64"
         41 -> "riscv_gprel_i"
         42 -> "riscv_gprel_s"
         43 -> "riscv_tprel_i"
         44 -> "riscv_tprel_s"
         45 -> "riscv_relax"
         51 -> "riscv_32_pcrel"
         55 -> "riscv_irelative"
         _ -> "riscv_" + to_str(typ)
      }
   } else {
      "reloc_" + to_str(typ)
   }
}

fn _symbols_by_index(list syms) dict {
   mut out = dict()
   mut i = 0
   while i < syms.len {
      def sym = syms[i]
      def key = sym.get("source", "") + ":" + to_str(sym.get("index", -1))
      out[key] = sym
      i += 1
   }
   out
}

fn _symbol_by_index(any syms, int idx, str source_name) dict {
   if is_dict(syms) {
      def exact = syms.get(source_name + ":" + to_str(idx), dict())
      if exact.len > 0 { return exact }
      def suffix = ":" + to_str(idx)
      def keys = syms.keys()
      mut k = 0
      while k < keys.len {
         def key = to_str(keys[k])
         if str.endswith(key, suffix) { return syms.get(key, dict()) }
         k += 1
      }
      return dict()
   }
   mut fallback = dict()
   mut i = 0
   while i < syms.len {
      def s = syms[i]
      if int(s.get("index", -1)) == idx {
         if source_name.len == 0 || s.get("source", "") == source_name { return s }
         if fallback.len == 0 { fallback = s }
      }
      i += 1
   }
   fallback
}

fn _relocations_from_section(str b, dict h, list ss, any syms, dict relsec) list {
   def le = h.get("little", true)
   def bits = h.get("bits", 64)
   def rela = int(relsec.get("type", 0)) == SHT_RELA
   def ent = int(relsec.get("entsize", bits == 64 ? (rela ? 24 : 16) : (rela ? 12 : 8)))
   if ent <= 0 { return [] }
   def symtab_idx = int(relsec.get("link", -1))
   def sym_source = (symtab_idx >= 0 && symtab_idx < ss.len) ? ss[symtab_idx].get("name", "") : ""
   mut out = []
   mut off = int(relsec.get("offset", 0))
   def end = off + int(relsec.get("size", 0))
   mut idx = 0
   while off + ent <= b.len && off + ent <= end {
      mut r_off = 0
      mut r_info = 0
      mut addend = 0
      if bits == 64 {
         r_off = _u64(b, off, le)
         r_info = _u64(b, off + 8, le)
         if rela { addend = _u64(b, off + 16, le) }
      } else {
         r_off = _u32(b, off, le)
         r_info = _u32(b, off + 4, le)
         if rela { addend = _u32(b, off + 8, le) }
      }
      def sym_idx = bits == 64 ? (r_info >> 32) : (r_info >> 8)
      def typ = bits == 64 ? (r_info & 0xffffffff) : (r_info & 255)
      def sym = _symbol_by_index(syms, sym_idx, sym_source)
      out = out.append({"index": idx, "section": relsec.get("name", ""), "offset": r_off,
            "info": r_info, "sym_index": sym_idx, "type": typ,
            "type_name": _reloc_type_name(int(h.get("machine_id", 0)), typ),
            "symbol": sym.get("name", ""), "symbol_record": sym, "addend": addend,
      "rela": rela})
      off += ent
      idx += 1
   }
   out
}

fn relocations(any source) list {
   "Return ELF relocation records with symbol names when available."
   def b = _source_data(source)
   def h = elf_header(b)
   def ss = is_dict(source) && source.contains("sections") ? source.get("sections", []) : _section_table(b, h)
   def sy = is_dict(source) && source.contains("symbols") ? source.get("symbols", []) : symbols({"data": b})
   def sy_index = _symbols_by_index(sy)
   mut out = []
   mut i = 0
   while i < ss.len {
      def s = ss[i]
      def t = int(s.get("type", 0))
      if t == SHT_RELA || t == SHT_REL {
         def part = _relocations_from_section(b, h, ss, sy_index, s)
         mut j = 0
         while j < part.len { out = out.append(part[j]) j += 1 }
      }
      i += 1
   }
   out
}

fn _reloc_by_offset(list rels, int addr) dict {
   mut i = 0
   while i < rels.len {
      if int(rels[i].get("offset", 0)) == addr { return rels[i] }
      i += 1
   }
   dict()
}

fn _plt_import_sites(dict bin) list {
   mut plt = section(bin, ".plt")
   if plt.len == 0 { plt = section(bin, ".plt.sec") }
   if plt.len == 0 { return [] }
   def rels = bin.contains("relocations") ? bin.get("relocations", []) : relocations(bin)
   mut jump = []
   mut i = 0
   while i < rels.len {
      def r = rels[i]
      def rn = r.get("section", "")
      if r.get("symbol", "").len > 0 && (rn == ".rela.plt" || rn == ".rel.plt" || rn == ".rela.plt.sec") {
         jump = jump.append(r)
      }
      i += 1
   }
   mut out = []
   i = 0
   while i < jump.len {
      def addr = int(plt.get("addr", 0)) + 16 * (i + 1)
      def raw = jump[i].get("symbol", "")
      out = out.append({"addr": addr, "name": _elf_safe_name(_display_symbol(raw), "import_" + str.to_hex(addr, 0)),
      "symbol": raw, "display_name": _display_symbol(raw), "relocation": jump[i], "kind": "plt"})
      i += 1
   }
   out
}

fn import_sites(any source) list {
   "Return best-effort import call sites such as x86_64 PLT stubs."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   _plt_import_sites(bin)
}

fn strings(any source, int min_len=4, int limit=512) list {
   "Extract printable ASCII strings from a binary."
   rev_strings.scan(_source_data(source), min_len, limit)
}

fn _detect_format(str data) str {
   if data.len >= 4 {
      if load8(data, 0) == 0x7f && load8(data, 1) == 69 && load8(data, 2) == 76 && load8(data, 3) == 70 {
         return "elf"
      }
      if load8(data, 0) == 77 && load8(data, 1) == 90 {
         return "pe"
      }
      def m = _u32(data, 0, false)
      if m == 0xfeedfacf || m == 0xfeedface || m == 0xcffaedfe || m == 0xcefaedfe ||
         m == 0xcafebabe || m == 0xbebafeca || m == 0xcafebabf || m == 0xbfbafeca {
         return "macho"
      }
   }
   "unknown"
}

fn load(str p, any opts=dict()) dict {
   "Load and analyze a binary file (ELF, PE, or Mach-O). The returned record is
   intentionally compact and stable for scripts, UI panels, and later
   symbolic/decompiler passes."
   def r = _read(p)
   if !r.get("ok", false) { return r }
   def data = r.get("data", "")
   def fmt = _detect_format(data)
   mut bin = dict()
   if fmt == "pe" {
      bin = pe.load(p, opts)
   } elif fmt == "macho" {
      bin = macho.load(p, opts)
   } else {
      def h = elf_header(data)
      def ss = _section_table(data, h)
      def segs = segments(data)
      def sy = symbols({"data": data})
      def rel = relocations({"data": data, "sections": ss, "symbols": sy, "header": h})
      def sym_funcs = functions({"symbols": sy})
      bin = {"ok": h.get("ok", false), "path": p, "name": path.basename(p), "data": data, "header": h,
         "sections": ss, "segments": segs, "symbols": sy, "functions": sym_funcs,
         "imports": imports({"symbols": sy}), "relocations": rel, "import_sites": _plt_import_sites({"sections": ss, "relocations": rel}),
         "strings": strings(data, int(opts.get("min_string", 4)), int(opts.get("string_limit", 256))),
      "tools": tool_status()}
   }
   if bin.get("ok", false) && bool(opts.get("recover_functions", true)) {
      bin = bin.set("functions", recover_functions(bin, int(opts.get("scan_bytes", 65536))))
   }
   bin
}

fn analyze(any source, any opts=dict()) dict {
   "Load a path or return an existing ELF analysis record unchanged."
   if is_dict(source) && source.contains("header") { return source }
   load(to_str(source), opts)
}

fn entry(any source) int {
   "Return the binary entry point."
   def h = is_dict(source) && source.contains("header") ? source.get("header", dict()) : elf_header(source)
   int(h.get("entry", 0))
}

fn arch(any source) str {
   "Return normalized architecture name."
   def h = is_dict(source) && source.contains("header") ? source.get("header", dict()) : elf_header(source)
   h.get("machine", "unknown")
}

fn arch_profile(any source) dict {
   "Return architecture metadata shared by disassembly, lifting, and emulation."
   def h = is_dict(source) && source.contains("header") ? source.get("header", dict()) : elf_header(source)
   def a = dasm.normalize_arch(h.get("machine", "unknown"))
   {"arch": a, "family": dasm.arch_family(a), "bits": int(h.get("bits", a == "aarch64" || a == "x86_64" ? 64 : 32)),
      "machine": h.get("machine", "unknown"), "machine_id": h.get("machine_id", 0),
   "entry": int(h.get("entry", 0)), "disassembler": "capstone"}
}

fn function_at(any source, int addr) dict {
   "Return a function symbol covering or starting at `addr`."
   def bin = is_dict(source) ? source : load(to_str(source))
   def fs = is_dict(source) && source.contains("functions") ? source.get("functions", []) : recover_functions(source)
   mut i = 0
   while i < fs.len {
      def f = fs[i]
      if int(f.get("value", 0)) == addr { return f }
      i += 1
   }
   mut best = dict()
   mut best_start = -1
   mut best_size = 0
   i = 0
   while i < fs.len {
      def f = fs[i]
      def start = int(f.get("value", 0))
      def ext = _extent_from_functions(bin, f, fs, 1 << 30)
      def end = int(ext.get("end", start + int(f.get("size", 0))))
      if start == addr || (end > start && addr >= start && addr < end) {
         def sz = int(ext.get("size", 0))
         if start > best_start || (start == best_start && (best_size == 0 || (sz > 0 && sz < best_size))) {
            best_start = start
            best_size = sz
            best = f
            if int(best.get("size", 0)) <= 0 && sz > 0 {
               best = best.set("size", sz).set("inferred_size", true)
            }
         }
      }
      i += 1
   }
   if best.len > 0 { return best }
   dict()
}

fn _vaddr_to_offset(dict bin, int addr) int {
   def segs = bin.get("segments", [])
   mut i = 0
   while i < segs.len {
      def s = segs[i]
      if s.get("load", false) {
         def va = int(s.get("vaddr", 0))
         def sz = int(s.get("filesz", 0))
         if addr >= va && addr < va + sz { return int(s.get("offset", 0)) + (addr - va) }
      }
      i += 1
   }
   -1
}

fn _offset_to_vaddr(dict bin, int off) int {
   def segs = bin.get("segments", [])
   mut i = 0
   while i < segs.len {
      def s = segs[i]
      if s.get("load", false) {
         def so = int(s.get("offset", 0))
         def sz = int(s.get("filesz", 0))
         if off >= so && off < so + sz { return int(s.get("vaddr", 0)) + (off - so) }
      }
      i += 1
   }
   0
}

fn _target_bytes(any source, any target=0, int max_bytes=512) dict {
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   if !bin.get("ok", false) { return {"ok": false, "bytes": "", "addr": 0, "name": "", "analysis": bin} }
   mut bytes = ""
   mut byte_list = []
   mut addr = 0
   mut name = ""
   mut off = -1
   mut n = 0
   if target == 0 {
      addr = entry(bin)
      off = _vaddr_to_offset(bin, addr)
      n = max_bytes
      bytes = _slice(bin.get("data", ""), off, n)
      byte_list = _slice_list(bin.get("data", ""), off, n)
      name = "entry_" + str.to_hex(addr, 0)
   } elif is_str(target) {
      def sec = section(bin, target)
      if sec.len > 0 {
         addr = int(sec.get("addr", 0))
         off = int(sec.get("offset", 0))
         n = min(max_bytes, int(sec.get("size", 0)))
         bytes = _slice(bin.get("data", ""), off, n)
         byte_list = _slice_list(bin.get("data", ""), off, n)
         name = target
      } else {
         def frows = recover_functions(bin)
         mut i = 0
         while i < frows.len {
            if frows[i].get("name", "") == target { return _target_bytes(bin, frows[i], max_bytes) }
            i += 1
         }
      }
   } elif is_dict(target) {
      addr = int(target.get("value", target.get("addr", entry(bin))))
      off = _vaddr_to_offset(bin, addr)
      def declared = int(target.get("size", 0))
      if declared > 0 { n = min(declared, max_bytes) }
      else {
         def ext = function_extent(bin, target, max_bytes)
         n = min(max(1, int(ext.get("size", max_bytes))), max_bytes)
      }
      bytes = _slice(bin.get("data", ""), off, n)
      byte_list = _slice_list(bin.get("data", ""), off, n)
      name = target.get("name", "sub_" + str.to_hex(addr, 0))
   } else {
      addr = int(target)
      off = _vaddr_to_offset(bin, addr)
      def f = function_at(bin, addr)
      if f.len > 0 {
         def ext = function_extent(bin, f, max_bytes)
         n = min(max(1, int(ext.get("size", max_bytes))), max_bytes)
         name = f.get("name", "sub_" + str.to_hex(addr, 0))
      } else {
         n = max_bytes
         name = "sub_" + str.to_hex(addr, 0)
      }
      bytes = _slice(bin.get("data", ""), off, n)
      byte_list = _slice_list(bin.get("data", ""), off, n)
   }
   {"ok": byte_list.len > 0 || bytes.len > 0, "bytes": bytes, "byte_list": byte_list, "addr": addr, "offset": off, "size": n, "name": name, "analysis": bin}
}

fn disassemble(any source, any target=".text", int max_bytes=512) list {
   "Disassemble a section name, address, or function record. Returns Capstone rows."
   def tb = _target_bytes(source, target, max_bytes)
   if !tb.get("ok", false) { return [] }
   def a = arch(tb.get("analysis", dict()))
   def bytes = tb.get("byte_list", [])
   mut rows = dasm.disasm_lines(bytes, a, int(tb.get("addr", 0)))
   if a == "x86_64" && rows.len == 1 && rows[0][1] == "streq" && bytes.len > 8 {
      rows = dasm.disasm_lines(bytes, a, int(tb.get("addr", 0)))
   }
   rows
}

fn disassemble_function(any source, any fn_name_or_addr, int max_bytes=512) list {
   "Disassemble one known function by name or address."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   mut f = dict()
   if is_dict(fn_name_or_addr) {
      f = fn_name_or_addr
   } elif is_str(fn_name_or_addr) {
      def fs = bin.get("functions", [])
      mut i = 0
      while i < fs.len {
         if fs[i].get("name", "") == fn_name_or_addr { f = fs[i] break }
         i += 1
      }
   } else {
      f = function_at(bin, int(fn_name_or_addr))
   }
   if f.len == 0 { return [] }
   disassemble(bin, f, max_bytes)
}

fn _crc16_ccitt(list bytes, int start, int count) int {
   mut crc = 0xffff
   mut i = 0
   while i < count && start + i < bytes.len {
      crc = crc ^^ ((int(bytes[start + i]) & 255) << 8)
      mut bit = 0
      while bit < 8 {
         if (crc & 0x8000) != 0 { crc = ((crc << 1) ^^ 0x1021) & 0xffff }
         else { crc = (crc << 1) & 0xffff }
         bit += 1
      }
      i += 1
   }
   crc & 0xffff
}

fn _flirt_mask_range(list mask0, int start, int count) list {
   mut mask = mask0
   mut i = 0
   while i < count && start + i < mask.len {
      if start + i >= 0 { mask = mask.set(start + i, true) }
      i += 1
   }
   mask
}

fn _flirt_variant_mask(list rows, int base, int size, str family) list {
   mut mask = []
   mut i = 0
   while i < size { mask = mask.append(false) i += 1 }
   i = 0
   while i < rows.len {
      def r = rows[i]
      def off = int(r[0]) - base
      def m = str.lower(str.strip(r[1]))
      def ops = r[2]
      def sz = int(r[3])
      if off >= 0 && off < size {
         if m == "call" || str.startswith(m, "j") || m == "bl" || m == "b" || m == "cbz" || m == "cbnz" || m == "tbz" || m == "tbnz" || m == "jal" || m == "jalr" {
            if family == "x86" && sz > 1 { mask = _flirt_mask_range(mask, off + 1, sz - 1) }
            else { mask = _flirt_mask_range(mask, off, sz) }
         } elif str.find(ops, "rip") >= 0 && sz >= 4 {
            mask = _flirt_mask_range(mask, off + sz - 4, 4)
         } elif m == "adr" || m == "adrp" {
            mask = _flirt_mask_range(mask, off, sz)
         }
      }
      i += 1
   }
   mask
}

fn _flirt_pattern(list bytes, list mask, int n) str {
   mut out = ""
   mut i = 0
   while i < n && i < bytes.len {
      out = out + (mask[i] ? ".." : str.to_hex(int(bytes[i]) & 255, 2))
      i += 1
   }
   out
}

fn _flirt_tail_len(list mask, int start, int max_len) int {
   mut n = 0
   while start + n < mask.len && n < max_len && !mask[start + n] { n += 1 }
   n
}

fn _flirt_variant_offsets(list mask) list {
   mut out = []
   mut i = 0
   while i < mask.len {
      if mask[i] { out = out.append(i) }
      i += 1
   }
   out
}

fn _flirt_refs_from_rows(list rows) list {
   mut out = []
   mut i = 0
   while i < rows.len {
      def r = rows[i]
      if r.get("op", "") == "call" {
         def name = r.get("target_name", "")
         if name.len > 0 { out = _append_unique(out, name) }
         elif int(r.get("target", 0)) != 0 { out = _append_unique(out, "sub_" + str.to_hex(int(r.get("target", 0)), 0)) }
      }
      def ref_name = r.get("ref_name", "")
      if ref_name.len > 0 { out = _append_unique(out, ref_name) }
      i += 1
   }
   out
}

fn _flirt_refs_from_disassembly(list rows, str arch_name) list {
   "Collect stable direct-call references without depending on high-level lifting."
   mut out = []
   mut i = 0
   while i < rows.len {
      def row = rows[i]
      if dasm.instruction_kind(arch_name, row[1]) == "call" && str.find(row[2], "[") < 0 {
         def target = dasm.target_address(row, arch_name)
         if target != 0 { out = _append_unique(out, "sub_" + str.to_hex(target, 0)) }
      }
      i += 1
   }
   out
}

fn flirt_signature(any source, any target=0, any opts=dict()) dict {
   "Build a conservative FLIRT-style signature for one function.
   The signature stores a masked first-byte pattern, CRC16 over the next stable
   byte run, and named references used to disambiguate otherwise identical code."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   def o = is_dict(opts) ? opts : dict()
   def maxb = int(o.get("max_bytes", 256))
   def pat_cap = int(o.get("pattern_bytes", 32))
   def tail_cap = int(o.get("tail_bytes", 64))
   def min_len = int(o.get("min_len", 4))
   def tb = _target_bytes(bin, target, maxb)
   if !tb.get("ok", false) { return {"ok": false, "reason": "target_unavailable"} }
   def bytes = tb.get("byte_list", [])
   def a = arch(bin)
   def family = dasm.arch_family(a)
   def rows = disassemble(bin, target, maxb)
   def mask = _flirt_variant_mask(rows, int(tb.get("addr", 0)), bytes.len, family)
   def plen = min(pat_cap, bytes.len)
   def tail_len = _flirt_tail_len(mask, plen, tail_cap)
   def crc = _crc16_ccitt(bytes, plen, tail_len)
   def refs = _flirt_refs_from_disassembly(rows, a)
   def accepted = bytes.len >= min_len || refs.len > 0
   def strength = !accepted ? "rejected" : ((refs.len > 0 || tail_len >= 8 || bytes.len >= pat_cap) ? "strong" : "weak")
   {"ok": true, "kind": "flirt_signature", "name": tb.get("name", ""), "addr": int(tb.get("addr", 0)),
      "arch": a, "family": family, "size": bytes.len, "accepted": accepted,
      "pattern": _flirt_pattern(bytes, mask, plen), "pattern_len": plen,
      "pattern_bytes": _slice_list(tb.get("bytes", ""), 0, plen),
      "mask": mask, "tail_start": plen, "tail_len": tail_len, "tail_crc16": crc,
      "variant_offsets": _flirt_variant_offsets(mask), "refs": refs, "row_count": rows.len,
      "strength": strength,
   "reason": accepted ? "ok" : "too_short_without_reference"}
}

fn flirt_signatures(any source, any opts=dict()) list {
   "Build FLIRT-style signatures for recovered functions in a binary."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   def o = is_dict(opts) ? opts : dict()
   def scan = int(o.get("scan_bytes", 65536))
   def include_rejected = o.get("include_rejected", false)
   def fs = bin.contains("functions") && bin.get("functions", []).len > 0 ? bin.get("functions", []) : recover_functions(bin, scan)
   mut out = []
   mut i = 0
   while i < fs.len {
      def sig = flirt_signature(bin, fs[i], o)
      if sig.get("ok", false) && (include_rejected || sig.get("accepted", false)) { out = out.append(sig) }
      i += 1
   }
   out
}

fn _flirt_ref_compatible(list sig_refs, list target_refs) dict {
   if sig_refs.len == 0 { return {"ok": true, "deferred": false, "matched": []} }
   if target_refs.len == 0 { return {"ok": true, "deferred": true, "matched": []} }
   mut matched = []
   mut i = 0
   while i < sig_refs.len {
      if _list_has(target_refs, sig_refs[i]) { matched = matched.append(sig_refs[i]) }
      i += 1
   }
   {"ok": matched.len == sig_refs.len, "deferred": false, "matched": matched}
}

fn _flirt_signature_match(dict sig, dict target, bool strict_refs=false) dict {
   if !sig.get("accepted", false) || !target.get("accepted", false) { return dict() }
   if sig.get("arch", "") != target.get("arch", "") { return dict() }
   def plen = int(sig.get("pattern_len", 0))
   if plen <= 0 || int(target.get("pattern_len", 0)) < plen { return dict() }
   def sbytes = sig.get("pattern_bytes", [])
   def tbytes = target.get("pattern_bytes", [])
   def mask = sig.get("mask", [])
   mut i = 0
   while i < plen {
      if i >= sbytes.len || i >= tbytes.len { return dict() }
      if !(i < mask.len && mask[i]) && (int(sbytes[i]) & 255) != (int(tbytes[i]) & 255) { return dict() }
      i += 1
   }
   def stail = int(sig.get("tail_len", 0))
   if stail != int(target.get("tail_len", 0)) { return dict() }
   if stail > 0 && int(sig.get("tail_crc16", -1)) != int(target.get("tail_crc16", -2)) { return dict() }
   def ref = _flirt_ref_compatible(sig.get("refs", []), target.get("refs", []))
   if !ref.get("ok", false) { return dict() }
   if strict_refs && ref.get("deferred", false) { return dict() }
   def same_size = int(sig.get("size", 0)) == int(target.get("size", -1))
   def score = plen * 2 + stail + sig.get("refs", []).len * 16 + (same_size ? 8 : 0) - (ref.get("deferred", false) ? 12 : 0)
   {"ok": true, "name": sig.get("name", ""), "addr": int(target.get("addr", 0)),
      "target_name": target.get("name", ""), "score": score, "confidence": min(99, score),
      "pattern_len": plen, "tail_len": stail, "tail_crc16": int(sig.get("tail_crc16", 0)),
      "refs": sig.get("refs", []), "matched_refs": ref.get("matched", []),
   "deferred": ref.get("deferred", false), "same_size": same_size}
}

fn flirt_match(any source, list signatures, any opts=dict()) list {
   "Match FLIRT-style signatures against recovered functions.
   Ambiguous collisions are reported but not considered safe for automatic renaming."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   def o = is_dict(opts) ? opts : dict()
   def targets = flirt_signatures(bin, o)
   def strict_refs = o.get("strict_refs", false)
   mut raw = []
   mut ti = 0
   while ti < targets.len {
      mut names = []
      mut best = dict()
      mut si = 0
      while si < signatures.len {
         def m = _flirt_signature_match(signatures[si], targets[ti], strict_refs)
         if m.len > 0 {
            names = _append_unique(names, m.get("name", ""))
            if best.len == 0 || int(m.get("score", 0)) > int(best.get("score", 0)) { best = m }
         }
         si += 1
      }
      if best.len > 0 {
         raw = raw.append(best.set("ambiguous", names.len > 1).set("candidates", names)
         .set("safe", names.len == 1 && !best.get("deferred", false)))
      }
      ti += 1
   }
   raw
}

fn flirt_apply(any source, list signatures, any opts=dict()) dict {
   "Return an analysis record with safe FLIRT-style matches applied as renames."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   def o = is_dict(opts) ? opts : dict()
   def matches = flirt_match(bin, signatures, o)
   mut renames = dict()
   mut i = 0
   while i < matches.len {
      def m = matches[i]
      if m.get("safe", false) {
         renames = renames.set(_hex(int(m.get("addr", 0))), m.get("name", ""))
      }
      i += 1
   }
   bin.set("renames", bin.get("renames", dict()).merge(renames)).set("flirt_matches", matches).set("flirt_renames", renames)
}

#main {
   assert(_reloc_type_name(62, 8) == "x86_64_relative", "x86_64 relocation names")
   assert(_reloc_type_name(62, 41) == "x86_64_irelative", "x86_64 irelative")
   assert(_reloc_type_name(183, 1026) == "aarch64_jump_slot", "aarch64 relocation names")
   assert(_reloc_type_name(183, 304) == "aarch64_call26", "aarch64 call26")
   assert(_reloc_type_name(40, 22) == "arm_jump_slot", "arm relocation names")
   assert(_reloc_type_name(40, 160) == "arm_irelative", "arm irelative")
   assert(_reloc_type_name(243, 5) == "riscv_jump_slot", "riscv relocation names")
   assert(_reloc_type_name(243, 19) == "riscv_pcrel_hi20", "riscv pcrel_hi20")
   assert(_reloc_type_name(8, 3) == "reloc_3", "unknown machine falls back")
   print("✓ std.os.rev.decomp.elf relocation names passed")
}
