;; Keywords: pe coff windows imports relocations sections loader
;; Portable Executable loader. Produces the same bin record contract as the ELF
;; loader so every decompiler pass (disassembly, lifting, decompilation) works
;; on PE binaries unchanged.
module std.os.rev.decomp.pe(pe_header, sections, segments, symbols, functions, imports, relocations, strings, entry, arch, load)

use std.core
use std.core.str as str
use std.os.path as path
use std.os.disasm as dasm
use std.os.rev.strings as rev_strings
use std.os.rev.decomp.tools (tool_status)
use std.os.rev.decomp.bytes (_u16, _u32, _u64, _slice, _slice_list, _cstring)
use std.os.rev.decomp.source (_read, _source_data)

def _PE_MACHINE_I386 = 0x14c
def _PE_MACHINE_AMD64 = 0x8664
def _PE_MACHINE_ARM = 0x1c0
def _PE_MACHINE_ARM64 = 0xaa64
def _PE_MACHINE_RISCV32 = 0x5032
def _PE_MACHINE_RISCV64 = 0x5064

def _SCN_MEM_EXECUTE = 0x20000000
def _SCN_MEM_READ = 0x40000000
def _SCN_MEM_WRITE = 0x80000000

def _RELOC_HIGHLOW = 3
def _RELOC_DIR32 = 10
def _RELOC_DIR64 = 4

def _DIR_EXPORT = 0
def _DIR_IMPORT = 1
def _DIR_BASERELOC = 5

fn _hex(int v) str { "0x" + str.to_hex(v, 0) }

fn _machine(int id) str {
   case id {
      _PE_MACHINE_I386 -> "x86"
      _PE_MACHINE_AMD64 -> "x86_64"
      _PE_MACHINE_ARM -> "arm"
      _PE_MACHINE_ARM64 -> "aarch64"
      _PE_MACHINE_RISCV32 -> "riscv32"
      _PE_MACHINE_RISCV64 -> "riscv64"
      _ -> "machine_" + to_str(id)
   }
}

fn _file_type(int id) str {
   case id {
      0x1 -> "relocatable"
      0x2 -> "executable"
      0x3 -> "dll"
      0x7 -> "rom"
      _ -> "pe_" + to_str(id)
   }
}

fn _sec_perms(int flags) str {
   ((flags & _SCN_MEM_READ) != 0 ? "r" : "-") + ((flags & _SCN_MEM_WRITE) != 0 ? "w" : "-") +
      ((flags & _SCN_MEM_EXECUTE) != 0 ? "x" : "-")
}

fn pe_header(any source) dict {
   "Parse a PE/COFF header from a path, bytes, or an analysis record."
   if is_dict(source) && source.contains("header") { return source.get("header", dict()) }
   def b = _source_data(source)
   if b.len < 0x40 { return {"ok": false, "format": "unknown", "reason": "too_small"} }
   if load8(b, 0) != 77 || load8(b, 1) != 90 {
      return {"ok": false, "format": "unknown", "reason": "not_pe"}
   }
   def lfanew = _u32(b, 0x3c, true)
   if lfanew + 24 > b.len || load8(b, lfanew) != 80 || load8(b, lfanew + 1) != 69 ||
      load8(b, lfanew + 2) != 0 || load8(b, lfanew + 3) != 0 {
      return {"ok": false, "format": "unknown", "reason": "no_pe_signature"}
   }
   def coff = lfanew + 4
   def machine = _u16(b, coff, true)
   def nsects = _u16(b, coff + 2, true)
   def symptr = _u32(b, coff + 8, true)
   def nsyms = _u32(b, coff + 12, true)
   def opt_size = _u16(b, coff + 16, true)
   def characteristics = _u16(b, coff + 18, true)
   def oh = coff + 20
   if oh + 4 > b.len { return {"ok": false, "format": "pe", "reason": "no_optional_header"} }
   def magic = _u16(b, oh, true)
   def bits = magic == 0x20b ? 64 : (magic == 0x10b ? 32 : 0)
   if bits == 0 { return {"ok": false, "format": "pe", "reason": "unknown_optional_magic"} }
   def entry_rva = _u32(b, oh + 16, true)
   def image_base = bits == 64 ? _u64(b, oh + 24, true) : _u32(b, oh + 28, true)
   def section_alignment = _u32(b, oh + 32, true)
   def file_alignment = _u32(b, oh + 36, true)
   def size_of_image = _u32(b, oh + 56, true)
   def size_of_headers = _u32(b, oh + 60, true)
   def num_rva = _u32(b, oh + 108, true)
   mut dirs = []
   mut di = 0
   while di < num_rva && di < 16 {
      def d_off = oh + 112 + di * 8
      dirs = dirs.append({"index": di, "rva": _u32(b, d_off, true), "size": _u32(b, d_off + 4, true)})
      di += 1
   }
   {
      "ok": true, "format": "pe", "bits": bits, "little": true,
      "type": _file_type(_u16(b, coff + 2, true) + 0),
      "file_type_id": 0,
      "machine": _machine(machine), "machine_id": machine,
      "entry": image_base + entry_rva, "entry_rva": entry_rva, "image_base": image_base,
      "nsections": nsects, "characteristics": characteristics,
      "section_alignment": section_alignment, "file_alignment": file_alignment,
      "size_of_image": size_of_image, "size_of_headers": size_of_headers,
      "coff_off": coff, "optional_off": oh, "opt_size": opt_size,
      "symbol_ptr": symptr, "symbol_count": nsyms,
      "directories": dirs,
   }
}

fn _section_name(str b, int off) str {
   mut out = ""
   mut i = 0
   while i < 8 {
      def c = load8(b, off + i)
      if c == 0 { break }
      out = out + chr(c)
      i += 1
   }
   out
}

fn _pe_section_table(str b, dict h) list {
   if !h.get("ok", false) { return [] }
   def sec_off = int(h.get("coff_off", 0)) + 20 + int(h.get("opt_size", 0))
   def n = int(h.get("nsections", 0))
   mut out = []
   mut i = 0
   while i < n {
      def off = sec_off + i * 40
      if off + 40 > b.len { break }
      def nm = _section_name(b, off)
      def vsize = _u32(b, off + 8, true)
      def vaddr = _u32(b, off + 12, true)
      def rsize = _u32(b, off + 16, true)
      def rawptr = _u32(b, off + 20, true)
      def flags = _u32(b, off + 36, true)
      def base = int(h.get("image_base", 0))
      out = out.append({"index": i + 1, "name": nm, "addr": base + vaddr, "rva": vaddr,
         "offset": rawptr, "size": rsize, "vsize": vsize, "flags": flags,
         "exec": (flags & _SCN_MEM_EXECUTE) != 0, "load": (flags & 0x00000020) != 0,
         "perms": _sec_perms(flags)})
      i += 1
   }
   out
}

fn _rva_to_offset(list ss, int rva) int {
   mut i = 0
   while i < ss.len {
      def s = ss[i]
      def va = int(s.get("rva", 0))
      def vs = int(s.get("vsize", int(s.get("size", 0))))
      if rva >= va && rva < va + vs {
         return int(s.get("offset", 0)) + (rva - va)
      }
      i += 1
   }
   -1
}

fn _vaddr_to_offset(list ss, int addr) int {
   mut i = 0
   while i < ss.len {
      def s = ss[i]
      def va = int(s.get("addr", 0))
      def sz = int(s.get("size", 0))
      if addr >= va && addr < va + sz { return int(s.get("offset", 0)) + (addr - va) }
      i += 1
   }
   -1
}

fn sections(any source) list {
   "Return PE section records."
   if is_dict(source) && source.contains("sections") { return source.get("sections", []) }
   def b = _source_data(source)
   _pe_section_table(b, pe_header(b))
}

fn segments(any source) list {
   "Return loadable segment records derived from PE sections."
   if is_dict(source) && source.contains("segments") { return source.get("segments", []) }
   def b = _source_data(source)
   def ss = _pe_section_table(b, pe_header(b))
   mut out = []
   mut i = 0
   while i < ss.len {
      def s = ss[i]
      if int(s.get("size", 0)) > 0 {
         out = out.append({"index": i, "type": "section", "load": true,
            "vaddr": int(s.get("addr", 0)), "offset": int(s.get("offset", 0)),
            "filesz": int(s.get("size", 0)), "memsz": int(s.get("vsize", int(s.get("size", 0)))),
            "perms": s.get("perms", "r--"), "name": s.get("name", "")})
      }
      i += 1
   }
   out
}

fn _pe_symbol_table(str b, dict h, list ss) list {
   def symptr = int(h.get("symbol_ptr", 0))
   def n = int(h.get("symbol_count", 0))
   if symptr <= 0 || n <= 0 { return [] }
   def strtab = symptr + n * 18
   mut out = []
   mut i = 0
   while i < n {
      def off = symptr + i * 18
      if off + 18 > b.len { break }
      mut nm = _section_name(b, off)
      def zeroes = _u32(b, off, true)
      if zeroes == 0 && _u32(b, off + 4, true) != 0 {
         nm = _cstring(b, strtab + _u32(b, off + 4, true))
      }
      def value = _u32(b, off + 8, true)
      def secnum = _u16(b, off + 12, true)
      def sclass = load8(b, off + 16)
      def aux = load8(b, off + 17)
      if nm.len > 0 && (sclass == 2 || sclass == 0x68) {
         def is_func = sclass == 2 && secnum > 0
         out = out.append({"index": i, "name": nm, "value": value, "size": 0,
            "shndx": is_func ? secnum : 0, "bind": "global", "type": is_func ? "function" : "object",
            "source": "coff", "storage_class": sclass, "section": secnum > 0 && secnum <= ss.len ? ss[secnum - 1].get("name", "") : ""})
      }
      i += 1 + aux
   }
   out
}

fn _imports_from_directories(str b, dict h, list ss) list {
   def dirs = h.get("directories", [])
   if int(dirs.len) <= _DIR_IMPORT { return [] }
   def imp = dirs[_DIR_IMPORT]
   def imp_rva = int(imp.get("rva", 0))
   if imp_rva == 0 { return [] }
   mut out = []
   mut off = _rva_to_offset(ss, imp_rva)
   if off < 0 { return [] }
   def bits = int(h.get("bits", 64))
   def base = int(h.get("image_base", 0))
   mut dli = 0
   while off >= 0 && off + 20 <= b.len {
      def oft = _u32(b, off, true)
      def name_rva = _u32(b, off + 12, true)
      def ft = _u32(b, off + 16, true)
      if oft == 0 && ft == 0 { break }
      def dll = name_rva != 0 ? _cstring(b, _rva_to_offset(ss, name_rva)) : ""
      def thunk_rva = oft != 0 ? oft : ft
      mut th_off = _rva_to_offset(ss, thunk_rva)
      mut ti = 0
      while th_off >= 0 {
         mut entry = 0
         if bits == 64 { entry = _u64(b, th_off, true) } else { entry = _u32(b, th_off, true) }
         if entry == 0 { break }
         if (bits == 64 && (entry & 0x8000000000000000) != 0) || (bits != 64 && (entry & 0x80000000) != 0) {
            def ord = bits == 64 ? (entry & 0xffff) : (entry & 0xffff)
            out = out.append({"name": "ordinal_" + to_str(ord), "raw_name": dll + "#ordinal_" + to_str(ord),
               "dll": dll, "ordinal": ord, "iat_rva": thunk_rva + ti * (bits == 64 ? 8 : 4),
               "iat_addr": base + thunk_rva + ti * (bits == 64 ? 8 : 4), "shndx": 0})
         } else {
            def byname_off = _rva_to_offset(ss, entry)
            def fn_name = byname_off >= 0 && byname_off + 2 <= b.len ? _cstring(b, byname_off + 2) : ""
            if fn_name.len > 0 {
               out = out.append({"name": fn_name, "raw_name": fn_name, "dll": dll,
                  "iat_rva": thunk_rva + ti * (bits == 64 ? 8 : 4),
                  "iat_addr": base + thunk_rva + ti * (bits == 64 ? 8 : 4), "shndx": 0})
            }
         }
         th_off += bits == 64 ? 8 : 4
         ti += 1
      }
      off += 20
      dli += 1
   }
   out
}

fn symbols(any source) list {
   "Return PE symbol records (COFF symbols plus import thunks)."
   if is_dict(source) && source.contains("symbols") { return source.get("symbols", []) }
   def b = _source_data(source)
   def h = pe_header(b)
   def ss = _pe_section_table(b, h)
   def coff = _pe_symbol_table(b, h, ss)
   def imports = _imports_from_directories(b, h, ss)
   mut out = []
   mut i = 0
   while i < coff.len { out = out.append(coff[i]) i += 1 }
   i = 0
   while i < imports.len {
      def im = imports[i]
      out = out.append({"index": 0, "name": im.get("name", ""), "value": int(im.get("iat_addr", 0)),
         "size": 0, "shndx": 0, "bind": "global", "type": "function", "source": "pe_import",
         "dll": im.get("dll", ""), "raw_name": im.get("raw_name", "")})
      i += 1
   }
   out
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

fn _reloc_type_name(int typ) str {
   case typ {
      0 -> "pe_abs"
      _RELOC_HIGHLOW -> "pe_highlow"
      _RELOC_DIR32 -> "pe_dir32"
      _RELOC_DIR64 -> "pe_dir64"
      _ -> "pe_reloc_" + to_str(typ)
   }
}

fn _base_relocations(str b, dict h, list ss) list {
   def dirs = h.get("directories", [])
   if int(dirs.len) <= _DIR_BASERELOC { return [] }
   def rd = dirs[_DIR_BASERELOC]
   def rva = int(rd.get("rva", 0))
   def size = int(rd.get("size", 0))
   if rva == 0 || size <= 0 { return [] }
   mut out = []
   mut off = _rva_to_offset(ss, rva)
   def end = min(off + size, b.len)
   def base = int(h.get("image_base", 0))
   while off >= 0 && off + 8 <= end {
      def page = _u32(b, off, true)
      def blk = _u32(b, off + 4, true)
      if blk < 8 { break }
      mut i = 8
      while i + 2 <= blk && off + i + 2 <= end {
         def ent = _u16(b, off + i, true)
         def typ = ent >> 12
         def rel = ent & 0xfff
         if typ != 0 {
            def addr = base + page + rel
            out = out.append({"index": out.len, "section": ".reloc", "offset": addr,
               "info": ent, "sym_index": 0, "type": typ, "type_name": _reloc_type_name(typ),
               "symbol": "", "symbol_record": dict(), "addend": 0, "rela": false})
         }
         i += 2
      }
      off += blk
   }
   out
}

fn relocations(any source) list {
   "Return PE base-relocation records."
   if is_dict(source) && source.contains("relocations") { return source.get("relocations", []) }
   def b = _source_data(source)
   def h = pe_header(b)
   def ss = _pe_section_table(b, h)
   _base_relocations(b, h, ss)
}

fn imports(any source) list {
   "Return imported/undefined symbols."
   if is_dict(source) && source.contains("imports") { return source.get("imports", []) }
   mut out = []
   def sy = is_dict(source) && source.contains("symbols") ? source.get("symbols", []) : symbols(source)
   mut i = 0
   while i < sy.len {
      def s = sy[i]
      if int(s.get("shndx", 0)) == 0 && s.get("name", "").len > 0 {
         def dll = s.get("dll", "")
         out = out.append(s.set("raw_name", dll.len > 0 ? dll + "!" + s.get("name", "") : s.get("name", "")))
      }
      i += 1
   }
   out
}

fn strings(any source, int min_len=4, int limit=512) list {
   "Extract printable ASCII strings from a PE binary."
   rev_strings.scan(_source_data(source), min_len, limit)
}

fn entry(any source) int {
   "Return the binary entry point."
   def h = is_dict(source) && source.contains("header") ? source.get("header", dict()) : pe_header(source)
   int(h.get("entry", 0))
}

fn arch(any source) str {
   "Return normalized architecture name."
   def h = is_dict(source) && source.contains("header") ? source.get("header", dict()) : pe_header(source)
   h.get("machine", "unknown")
}

fn load(str p, any opts=dict()) dict {
   "Load and analyze a PE/COFF file. Returns the same compact, stable record
   shape as the ELF loader so shared decompiler passes work unchanged."
   def r = _read(p)
   if !r.get("ok", false) { return r }
   def data = r.get("data", "")
   def h = pe_header(data)
   def ss = _pe_section_table(data, h)
   def segs = segments(data)
   def sy = symbols(data)
   def rel = relocations({"data": data})
   def imp = imports({"symbols": sy})
   def sym_funcs = functions({"symbols": sy})
   def funs = sym_funcs
   mut bin = {"ok": h.get("ok", false), "path": p, "name": path.basename(p), "data": data, "header": h,
      "sections": ss, "segments": segs, "symbols": sy, "functions": funs,
      "imports": imp, "relocations": rel, "import_sites": [],
      "strings": strings(data, int(opts.get("min_string", 4)), int(opts.get("string_limit", 256))),
   "tools": tool_status()}
   if bin.get("ok", false) {
      def e = int(h.get("entry", 0))
      if e != 0 && _vaddr_to_offset(ss, e) >= 0 {
         bin = bin.set("functions", funs.append({"name": "entry_" + str.to_hex(e, 0), "value": e,
            "size": 0, "type": "function", "kind": "entry"}))
      }
   }
   bin
}

fn _pe_u16(int v) str { chr(v & 0xff) + chr((v >> 8) & 0xff) }
fn _pe_u32(int v) str { _pe_u16(v) + _pe_u16(v >> 16) }
fn _pe_u64(int v) str { _pe_u32(v) + _pe_u32(v >> 32) }
fn _pe_name(str s, int n) str {
   mut out = s
   while out.len < n { out = out + "\x00" }
   out
}
fn _pe_sample() str {
   def dos = _pe_name("MZ", 0x3c) + _pe_u32(0x80)
   def coff = _pe_name("PE", 4) + _pe_u16(_PE_MACHINE_AMD64) + _pe_u16(1) +
      _pe_u32(0) + _pe_u32(0) + _pe_u32(0) + _pe_u16(0xf0) + _pe_u16(0x22)
   def oh = _pe_u16(0x20b) + _pe_u16(0) +
      _pe_u32(0x40) + _pe_u32(0) + _pe_u32(0) + _pe_u32(0x1000) + _pe_u32(0x1000) +
      _pe_u64(0x140000000) + _pe_u32(0x1000) + _pe_u32(0x200) +
      _pe_name("", 12) + _pe_u32(0) +
      _pe_u32(0x2000) + _pe_u32(0x400) + _pe_u32(0) + _pe_u16(3) + _pe_u16(0) +
      _pe_u64(0x100000) + _pe_u64(0x1000) + _pe_u64(0x100000) + _pe_u64(0x1000) +
      _pe_u32(0) + _pe_u32(16) + _pe_name("", 128)
   def sec = _pe_name(".text", 8) + _pe_u32(0x40) + _pe_u32(0x1000) + _pe_u32(0x40) + _pe_u32(0x400) +
      _pe_u32(0) + _pe_u32(0) + _pe_u16(0) + _pe_u16(0) + _pe_u32(0x60000020)
   dos + coff + oh + sec
}

#main {
   def b = _pe_sample()
   def h = pe_header(b)
   assert(h.get("ok", false), "pe sample header")
   assert(h.get("format", "") == "pe", "pe sample format")
   assert(h.get("machine", "") == "x86_64", "pe sample machine")
   assert(int(h.get("bits", 0)) == 64, "pe sample bits")
   assert(int(h.get("entry", 0)) == 0x140001000, "pe sample entry")
   def ss = sections(b)
   assert(ss.len == 1, "pe sample sections")
   assert(ss[0].get("name", "") == ".text", "pe sample section name")
   assert(int(ss[0].get("addr", 0)) == 0x140001000, "pe sample section addr")
   assert(ss[0].get("exec", false), "pe sample section exec")
   def sg = segments(b)
   assert(sg.len == 1 && sg[0].get("perms", "") == "r-x", "pe sample segment")
   print("✓ std.os.rev.decomp.pe self-test passed")
}
