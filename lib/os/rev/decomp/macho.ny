;; Keywords: macho darwin mach-o sections symbols imports loader
;; Mach-O loader. Produces the same bin record contract as the ELF loader so
;; every decompiler pass (disassembly, lifting, decompilation) works on Mach-O
;; binaries unchanged. Supports thin 32/64-bit files and universal (FAT) files.
module std.os.rev.decomp.macho(macho_header, sections, segments, symbols, functions, imports, relocations, strings, entry, arch, load)

use std.core
use std.core.str as str
use std.os.path as path
use std.os.disasm as dasm
use std.os.rev.strings as rev_strings
use std.os.rev.decomp.tools (tool_status)
use std.os.rev.decomp.bytes (_u16, _u32, _u64, _slice, _slice_list, _cstring)
use std.os.rev.decomp.source (_read, _source_data)

def _MH_MAGIC_64 = 0xfeedfacf
def _MH_MAGIC = 0xfeedface
def _MH_CIGAM_64 = 0xcffaedfe
def _MH_CIGAM = 0xcefaedfe
def _FAT_MAGIC = 0xcafebabe
def _FAT_CIGAM = 0xbebafeca
def _FAT_MAGIC_64 = 0xcafebabf
def _FAT_CIGAM_64 = 0xbfbafeca

def _CPU_X86 = 7
def _CPU_X86_64 = 0x01000007
def _CPU_ARM = 12
def _CPU_ARM64 = 0x0100000c

def _LC_SEGMENT = 0x1
def _LC_SYMTAB = 0x2
def _LC_SEGMENT_64 = 0x19
def _LC_DYSYMTAB = 0xb
def _LC_LOAD_DYLIB = 0xc
def _LC_MAIN = 0x80000028

def _N_UNDF = 0x0
def _N_ABS = 0x2
def _N_SECT = 0xe
def _N_EXT = 0x1

def _S_ATTR_PURE_INSTRUCTIONS = 0x80000000

fn _hex(int v) str { "0x" + str.to_hex(v, 0) }

fn _machine(int id) str {
   case id {
      _CPU_X86 -> "x86"
      _CPU_X86_64 -> "x86_64"
      _CPU_ARM -> "arm"
      _CPU_ARM64 -> "aarch64"
      _ -> "cpu_" + to_str(id)
   }
}

fn _file_type(int id) str {
   case id {
      0x1 -> "object"
      0x2 -> "executable"
      0x4 -> "core"
      0x6 -> "dylib"
      0x7 -> "dylinker"
      0x8 -> "bundle"
      0x9 -> "dSYM"
      0xa -> "kext"
      _ -> "macho_" + to_str(id)
   }
}

fn _seg_perms(int prot) str {
   ((prot & 1) != 0 ? "r" : "-") + ((prot & 2) != 0 ? "w" : "-") + ((prot & 4) != 0 ? "x" : "-")
}

fn _is_macho_magic(int m) bool {
   m == _MH_MAGIC || m == _MH_MAGIC_64 || m == _MH_CIGAM || m == _MH_CIGAM_64 ||
      m == _FAT_MAGIC || m == _FAT_MAGIC_64 || m == _FAT_CIGAM || m == _FAT_CIGAM_64
}

fn _thin(str b, int fat_magic) dict {
   "Extract one thin slice from a FAT file, picking the first architecture."
   def big = fat_magic == _FAT_MAGIC || fat_magic == _FAT_MAGIC_64
   def is64 = fat_magic == _FAT_MAGIC_64 || fat_magic == _FAT_CIGAM_64
   def ent = is64 ? 32 : 20
   if b.len < 4 + ent { return {"ok": false, "format": "macho", "reason": "fat_too_small"} }
   def off = _u32(b, 8, !big)
   def size = _u32(b, 12, !big)
   if off < 0 || size <= 0 || off + size > b.len { return {"ok": false, "format": "macho", "reason": "fat_bad_slice"} }
   {"ok": true, "data": _slice(b, off, size), "offset": off, "size": size}
}

fn macho_header(any source) dict {
   "Parse a Mach-O header from a path, bytes, or an analysis record."
   if is_dict(source) && source.contains("header") { return source.get("header", dict()) }
   def b0 = _source_data(source)
   if b0.len < 8 { return {"ok": false, "format": "unknown", "reason": "too_small"} }
   mut b = b0
   def magic0 = _u32(b, 0, false)
   if !_is_macho_magic(magic0) {
      return {"ok": false, "format": "unknown", "reason": "not_macho"}
   }
   if magic0 == _FAT_MAGIC || magic0 == _FAT_CIGAM || magic0 == _FAT_MAGIC_64 || magic0 == _FAT_CIGAM_64 {
      def thin = _thin(b, magic0)
      if !thin.get("ok", false) { return {"ok": false, "format": "macho", "reason": thin.get("reason", "fat_error")} }
      b = to_str(thin.get("data", ""))
   }
   def magic = _u32(b, 0, false)
   def big = magic == _MH_MAGIC || magic == _MH_MAGIC_64
   def bits = (magic == _MH_MAGIC_64 || magic == _MH_CIGAM_64) ? 64 : 32
   def cpu = _u32(b, 4, !big)
   def filetype = _u32(b, 12, !big)
   def ncmds = _u32(b, 16, !big)
   def sizeofcmds = _u32(b, 20, !big)
   def flags = _u32(b, 24, !big)
   def reserved = bits == 64 ? _u32(b, 28, !big) : 0
   def cmdoff = bits == 64 ? 32 : 28
   {"ok": true, "format": "macho", "bits": bits, "little": !big, "type": _file_type(filetype),
      "machine": _machine(cpu), "machine_id": cpu, "cputype": cpu,
      "ncmds": ncmds, "sizeofcmds": sizeofcmds, "flags": flags, "reserved": reserved,
      "cmd_off": cmdoff}
}

fn _macho_load_commands(str b, dict h) list {
   def cmdoff = int(h.get("cmd_off", 32))
   def n = int(h.get("ncmds", 0))
   def sz = int(h.get("sizeofcmds", 0))
   def big = !h.get("little", false)
   mut out = []
   mut off = cmdoff
   def end = min(cmdoff + sz, b.len)
   mut i = 0
   while i < n && off + 8 <= end {
      def cmd = _u32(b, off, !big)
      def csize = _u32(b, off + 4, !big)
      if csize < 8 { break }
      out = out.append({"index": i, "cmd": cmd, "cmd_size": csize, "offset": off})
      off += csize
      i += 1
   }
   out
}

fn _load_section_64(str b, int off, bool le) dict {
   def nm = _cstring(b, off, 16)
   def seg = _cstring(b, off + 16, 16)
   {"name": nm, "segment": seg, "addr": _u64(b, off + 32, le), "size": _u64(b, off + 40, le),
      "offset": _u32(b, off + 48, le), "align": _u32(b, off + 52, le),
      "reloff": _u32(b, off + 56, le), "nreloc": _u32(b, off + 60, le),
      "flags": _u32(b, off + 64, le), "reserved1": _u32(b, off + 68, le), "reserved2": _u32(b, off + 72, le)}
}

fn _load_section_32(str b, int off, bool le) dict {
   def nm = _cstring(b, off, 16)
   def seg = _cstring(b, off + 16, 16)
   {"name": nm, "segment": seg, "addr": _u32(b, off + 32, le), "size": _u32(b, off + 36, le),
      "offset": _u32(b, off + 40, le), "align": _u32(b, off + 44, le),
      "reloff": _u32(b, off + 48, le), "nreloc": _u32(b, off + 52, le),
      "flags": _u32(b, off + 56, le), "reserved1": _u32(b, off + 60, le), "reserved2": _u32(b, off + 64, le)}
}

fn _macho_sections(str b, dict h, list cmds) list {
   def big = !h.get("little", false)
   def bits = int(h.get("bits", 64))
   mut out = []
   mut si = 0
   mut i = 0
   while i < cmds.len {
      def c = cmds[i]
      def cmd = int(c.get("cmd", 0))
      if cmd == _LC_SEGMENT_64 || cmd == _LC_SEGMENT {
         def off = int(c.get("offset", 0))
         def nsects = _u32(b, off + (bits == 64 ? 64 : 48), !big)
         def s_off = off + (bits == 64 ? 72 : 56)
         def e = bits == 64 ? 80 : 68
         mut j = 0
         while j < nsects {
            def s = bits == 64 ? _load_section_64(b, s_off + j * e, !big) : _load_section_32(b, s_off + j * e, !big)
            out = out.append(s.set("index", si).set("exec", (int(s.get("flags", 0)) & _S_ATTR_PURE_INSTRUCTIONS) != 0))
            si += 1
            j += 1
         }
      }
      i += 1
   }
   out
}

fn _macho_segments(str b, dict h, list cmds) list {
   def big = !h.get("little", false)
   def bits = int(h.get("bits", 64))
   mut out = []
   mut i = 0
   while i < cmds.len {
      def c = cmds[i]
      def cmd = int(c.get("cmd", 0))
      if cmd == _LC_SEGMENT_64 || cmd == _LC_SEGMENT {
         def off = int(c.get("offset", 0))
         def name = _cstring(b, off + 8, 16)
         def vmaddr = bits == 64 ? _u64(b, off + 24, !big) : _u32(b, off + 24, !big)
         def vmsize = bits == 64 ? _u64(b, off + 32, !big) : _u32(b, off + 28, !big)
         def fileoff = bits == 64 ? _u64(b, off + 40, !big) : _u32(b, off + 32, !big)
         def filesize = bits == 64 ? _u64(b, off + 48, !big) : _u32(b, off + 36, !big)
         def initprot = _u32(b, off + (bits == 64 ? 60 : 44), !big)
         def nsects = _u32(b, off + (bits == 64 ? 64 : 48), !big)
         out = out.append({"index": out.len, "type": "segment", "name": name, "load": true,
            "vaddr": vmaddr, "vsize": vmsize, "offset": fileoff, "filesz": filesize,
            "perms": _seg_perms(initprot), "nsects": nsects})
      }
      i += 1
   }
   out
}

fn sections(any source) list {
   "Return Mach-O section records."
   if is_dict(source) && source.contains("sections") { return source.get("sections", []) }
   def b = _source_data(source)
   def h = macho_header(b)
   _macho_sections(b, h, _macho_load_commands(b, h))
}

fn segments(any source) list {
   "Return Mach-O load-command segment records."
   if is_dict(source) && source.contains("segments") { return source.get("segments", []) }
   def b = _source_data(source)
   def h = macho_header(b)
   _macho_segments(b, h, _macho_load_commands(b, h))
}

fn _symtab_info(str b, dict h, list cmds) dict {
   def big = !h.get("little", false)
   mut i = 0
   while i < cmds.len {
      def c = cmds[i]
      if int(c.get("cmd", 0)) == _LC_SYMTAB {
         def off = int(c.get("offset", 0))
         return {"symoff": _u32(b, off + 8, !big), "nsyms": _u32(b, off + 12, !big),
            "stroff": _u32(b, off + 16, !big), "strsize": _u32(b, off + 20, !big)}
      }
      i += 1
   }
   dict()
}

fn _macho_symbols(str b, dict h, list cmds, list ss) list {
   def big = !h.get("little", false)
   def bits = int(h.get("bits", 64))
   def st = _symtab_info(b, h, cmds)
   def symoff = int(st.get("symoff", 0))
   def nsyms = int(st.get("nsyms", 0))
   def stroff = int(st.get("stroff", 0))
   def strsize = int(st.get("strsize", 0))
   if symoff <= 0 || nsyms <= 0 || stroff <= 0 { return [] }
   def e = bits == 64 ? 16 : 12
   mut out = []
   mut i = 0
   while i < nsyms {
      def off = symoff + i * e
      if off + e > b.len { break }
      def n_strx = _u32(b, off, !big)
      def n_type = load8(b, off + 4)
      def n_sect = load8(b, off + 5)
      def n_desc = _u16(b, off + 6, !big)
      def n_value = bits == 64 ? _u64(b, off + 8, !big) : _u32(b, off + 8, !big)
      if (n_type & 0xe0) != 0 { i += 1 continue }
      def nm = (stroff + n_strx < stroff + strsize) ? _cstring(b, stroff + n_strx) : ""
      def t = n_type & 0x0e
      if nm.len == 0 && t == _N_UNDF { i += 1 continue }
      def bind = (n_type & _N_EXT) != 0 ? "global" : "local"
      mut sec = 0
      mut is_func = false
      if t == _N_SECT {
         sec = n_sect
         if sec > 0 && sec <= ss.len {
            is_func = ss[sec - 1].get("name", "") == "__text"
         }
      } elif t == _N_ABS {
         sec = -1
      }
      out = out.append({"index": i, "name": nm, "value": n_value, "size": 0,
         "shndx": sec, "bind": bind, "type": is_func ? "function" : "object",
         "source": "macho", "desc": n_desc})
      i += 1
   }
   out
}

fn _macho_undefined_imports(list sy) list {
   mut out = []
   mut i = 0
   while i < sy.len {
      def s = sy[i]
      if int(s.get("shndx", 0)) == 0 && s.get("name", "").len > 0 {
         out = out.append(s.set("raw_name", s.get("name", "")).set("display_name", s.get("name", "")))
      }
      i += 1
   }
   out
}

fn symbols(any source) list {
   "Return Mach-O symbol records (nlist entries)."
   if is_dict(source) && source.contains("symbols") { return source.get("symbols", []) }
   def b = _source_data(source)
   def h = macho_header(b)
   def cmds = _macho_load_commands(b, h)
   def ss = _macho_sections(b, h, cmds)
   _macho_symbols(b, h, cmds, ss)
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
      0 -> "macho_vanilla"
      1 -> "macho_pair"
      2 -> "macho_sectdiff"
      4 -> "macho_pbr"
      5 -> "macho_branch"
      7 -> "macho_subtract"
      9 -> "macho_got"
      10 -> "macho_32"
      11 -> "macho_64"
      12 -> "macho_pcrel32"
      _ -> "macho_reloc_" + to_str(typ)
   }
}

fn _section_relocations(str b, dict h, dict sec) list {
   def big = !h.get("little", false)
   def reloff = int(sec.get("reloff", 0))
   def nreloc = int(sec.get("nreloc", 0))
   if reloff <= 0 || nreloc <= 0 { return [] }
   mut out = []
   mut i = 0
   while i < nreloc {
      def off = reloff + i * 8
      if off + 8 > b.len { break }
      def r_address = _u32(b, off, !big)
      def flags = _u32(b, off + 4, !big)
      def r_symbolnum = flags & 0x00ffffff
      def r_pcrel = (flags >> 24) & 1
      def r_length = (flags >> 25) & 3
      def r_extern = (flags >> 27) & 1
      def r_type = (flags >> 28) & 15
      out = out.append({"index": i, "section": sec.get("name", ""), "offset": int(sec.get("addr", 0)) + r_address,
         "info": flags, "sym_index": r_symbolnum, "type": r_type, "type_name": _reloc_type_name(r_type),
         "symbol": "", "symbol_record": dict(), "addend": 0, "rela": false, "pcrel": r_pcrel != 0,
         "length": r_length, "extern": r_extern != 0})
      i += 1
   }
   out
}

fn _resolve_reloc_symbols(list rels, list sy) list {
   mut out = []
   mut i = 0
   while i < rels.len {
      def r = rels[i]
      if r.get("extern", false) {
         def idx = int(r.get("sym_index", 0))
         if idx < sy.len {
            out = out.append(r.set("symbol", sy[idx].get("name", "")))
            i += 1
            continue
         }
      }
      out = out.append(r)
      i += 1
   }
   out
}

fn relocations(any source) list {
   "Return Mach-O relocation records (object external/local relocations)."
   if is_dict(source) && source.contains("relocations") { return source.get("relocations", []) }
   def b = _source_data(source)
   def h = macho_header(b)
   def cmds = _macho_load_commands(b, h)
   def ss = _macho_sections(b, h, cmds)
   def sy = _macho_symbols(b, h, cmds, ss)
   mut out = []
   mut i = 0
   while i < ss.len {
      def part = _section_relocations(b, h, ss[i])
      mut j = 0
      while j < part.len { out = out.append(part[j]) j += 1 }
      i += 1
   }
   _resolve_reloc_symbols(out, sy)
}

fn imports(any source) list {
   "Return imported/undefined symbols."
   if is_dict(source) && source.contains("imports") { return source.get("imports", []) }
   def sy = is_dict(source) && source.contains("symbols") ? source.get("symbols", []) : symbols(source)
   _macho_undefined_imports(sy)
}

fn strings(any source, int min_len=4, int limit=512) list {
   "Extract printable ASCII strings from a Mach-O binary."
   rev_strings.scan(_source_data(source), min_len, limit)
}

fn entry(any source) int {
   "Return the binary entry point."
   def is_bin = is_dict(source) && source.contains("header")
   def h = is_bin ? source.get("header", dict()) : macho_header(source)
   if !h.get("ok", false) { return 0 }
   def b = is_bin ? to_str(source.get("data", "")) : _source_data(source)
   def big = !h.get("little", false)
   def cmds = _macho_load_commands(b, h)
   mut i = 0
   while i < cmds.len {
      def c = cmds[i]
      if int(c.get("cmd", 0)) == _LC_MAIN {
         def off = int(c.get("offset", 0))
         def entryoff = _u64(b, off + 8, !big)
         return _image_base(b, h, cmds) + entryoff
      }
      i += 1
   }
   def sy = _macho_symbols(b, h, cmds, _macho_sections(b, h, cmds))
   i = 0
   while i < sy.len {
      def s = sy[i]
      def nm = s.get("name", "")
      if (nm == "_start" || nm == "_main") && int(s.get("value", 0)) > 0 { return int(s.get("value", 0)) }
      i += 1
   }
   0
}

fn _image_base(str b, dict h, list cmds) int {
   def segs = _macho_segments(b, h, cmds)
   if segs.len == 0 { return 0 }
   def first = segs[0]
   if int(first.get("offset", 0)) == 0 { return int(first.get("vaddr", 0)) }
   int(first.get("vaddr", 0)) - int(first.get("offset", 0))
}

fn arch(any source) str {
   "Return normalized architecture name."
   def h = is_dict(source) && source.contains("header") ? source.get("header", dict()) : macho_header(source)
   h.get("machine", "unknown")
}

fn load(str p, any opts=dict()) dict {
   "Load and analyze a Mach-O file. Returns the same compact, stable record
   shape as the ELF loader so shared decompiler passes work unchanged."
   def r = _read(p)
   if !r.get("ok", false) { return r }
   def data = r.get("data", "")
   def h = macho_header(data)
   def cmds = _macho_load_commands(data, h)
   def ss = _macho_sections(data, h, cmds)
   def segs = _macho_segments(data, h, cmds)
   def sy = _macho_symbols(data, h, cmds, ss)
   def rel = _resolve_reloc_symbols(relocations({"data": data}), sy)
   def imp = _macho_undefined_imports(sy)
   def sym_funcs = functions({"symbols": sy})
   mut bin = {"ok": h.get("ok", false), "path": p, "name": path.basename(p), "data": data, "header": h,
      "sections": ss, "segments": segs, "symbols": sy, "functions": sym_funcs,
      "imports": imp, "relocations": rel, "import_sites": [],
      "strings": strings(data, int(opts.get("min_string", 4)), int(opts.get("string_limit", 256))),
   "tools": tool_status()}
   if bin.get("ok", false) {
      def e = entry(bin)
      if e != 0 {
         mut has_entry = false
         mut fi = 0
         while fi < sym_funcs.len {
            if int(sym_funcs[fi].get("value", 0)) == e { has_entry = true break }
            fi += 1
         }
         if !has_entry {
            bin = bin.set("functions", sym_funcs.append({"name": "entry_" + _hex(e), "value": e,
               "size": 0, "type": "function", "kind": "entry"}))
         }
      }
   }
   bin
}

fn _le16(int v) str { chr(v & 0xff) + chr((v >> 8) & 0xff) }
fn _le32(int v) str { _le16(v) + _le16(v >> 16) }
fn _le64(int v) str { _le32(v) + _le32(v >> 32) }
fn _macho_name(str s, int n) str {
   mut out = s
   while out.len < n { out = out + "\x00" }
   out
}
fn _macho_sample() str {
   def seg = _le32(_LC_SEGMENT_64) + _le32(152) + _macho_name("__TEXT", 16) +
      _le64(0x100000000) + _le64(0x1000) + _le64(0) + _le64(0x1000) +
      _le32(5) + _le32(5) + _le32(1) + _le32(0)
   def sec = _macho_name("__text", 16) + _macho_name("__TEXT", 16) +
      _le64(0x100000000) + _le64(0x40) + _le32(0x1000) + _le32(0) + _le32(0) + _le32(0) +
      _le32(_S_ATTR_PURE_INSTRUCTIONS) + _le32(0) + _le32(0) + _le32(0)
   def main = _le32(_LC_MAIN) + _le32(24) + _le64(0) + _le64(0)
   _le32(_MH_CIGAM_64) + _le32(_CPU_X86_64) + _le32(3) + _le32(2) +
      _le32(2) + _le32(152 + 24) + _le32(0) + _le32(0) + seg + sec + main
}

#main {
   def b = _macho_sample()
   def h = macho_header(b)
   assert(h.get("ok", false), "macho sample header")
   assert(h.get("format", "") == "macho", "macho sample format")
   assert(h.get("machine", "") == "x86_64", "macho sample machine")
   assert(int(h.get("bits", 0)) == 64, "macho sample bits")
   assert(int(h.get("little", false)) == 1, "macho sample little endian")
   def cmds = _macho_load_commands(b, h)
   assert(cmds.len == 2, "macho sample load commands")
   def ss = _macho_sections(b, h, cmds)
   assert(ss.len == 1, "macho sample sections")
   assert(ss[0].get("name", "") == "__text", "macho sample section name")
   assert(int(ss[0].get("addr", 0)) == 0x100000000, "macho sample section addr")
   assert(ss[0].get("exec", false), "macho sample section exec")
   def segs = _macho_segments(b, h, cmds)
   assert(segs.len == 1 && int(segs[0].get("vaddr", 0)) == 0x100000000, "macho sample segment")
   assert(entry(b) == 0x100000000, "macho sample entry via LC_MAIN")
   print("✓ std.os.rev.decomp.macho self-test passed")
}
