;; Keywords: decompiler annotations names renames notes
;; Analysis rename overlays, notes, and safe rendering identifiers.
module std.os.rev.decomp.annotations *

use std.core
use std.core.str as str
use std.os.rev.decomp.elf

fn _safe_name(str n, str fallback) str {
   if n.len == 0 { return fallback }
   mut out = ""
   mut i = 0
   while i < n.len {
      def c = load8(n, i)
      if str.ascii_is_alnum(c) || c == 95 { out = out + chr(c) }
      else { out = out + "_" }
      i += 1
   }
   if out.len == 0 { return fallback }
   out
}

fn _rename_map(dict bin) dict {
   bin.get("renames", dict())
}

fn _rename_name(dict bin, str name) str {
   if name.len == 0 { return name }
   def r = _rename_map(bin)
   _safe_name(r.get(name, name), name)
}

fn _rename_addr(dict bin, int addr, str fallback) str {
   def r = _rename_map(bin)
   def hx = _hex(addr)
   def raw = str.to_hex(addr, 0)
   def val = r.get(hx, r.get(raw, r.get(to_str(addr), r.get(fallback, fallback))))
   _safe_name(val, fallback)
}

fn with_renames(any source, dict renames) dict {
   "Return an analysis record with decompiler rename overlays applied.
   Keys may be existing names, decimal addresses, `0x...` addresses, or raw
   hex addresses ; values are sanitized as Ny identifiers when rendered."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   bin.set("renames", bin.get("renames", dict()).merge(renames))
}

fn _note_key(any key) str {
   if is_int(key) { return _hex(int(key)) }
   def s = to_str(key)
   if str.startswith(s, "0x") || str.startswith(s, "0X") { return "0x" + str.lower(slice(s, 2, s.len, 1)) }
   s
}

fn _note_record(any key, any val) dict {
   def k = _note_key(key)
   def addr = str.startswith(k, "0x") ? str.parse_int(slice(k, 2, k.len, 1), 16) : 0
   if is_dict(val) {
      return val.set("key", k).set("addr", int(val.get("addr", addr))).set("text", to_str(val.get("text", val.get("note", ""))))
   }
   {"key": k, "addr": addr, "text": to_str(val), "kind": "note"}
}

fn with_notes(any source, dict items) dict {
   "Return an analysis record with address/name notes attached.
   Keys may be addresses or names ; values may be text or note records."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   mut out = bin.get("notes", dict())
   def ks = items.keys()
   mut i = 0
   while i < ks.len {
      def k = ks[i]
      out = out.set(_note_key(k), _note_record(k, items.get(k, "")))
      i += 1
   }
   bin.set("notes", out)
}

fn notes(any source) list {
   "Return note/bookmark records attached to an analysis."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   def ns = bin.get("notes", dict())
   def ks = ns.keys()
   mut out = []
   mut i = 0
   while i < ks.len {
      out = out.append(ns.get(ks[i], dict()))
      i += 1
   }
   out
}

fn note_at(any source, any key) dict {
   "Return one note by address/name key."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   bin.get("notes", dict()).get(_note_key(key), dict())
}

#main {
   assert(_safe_name("entry-point", "sub") == "entry_point", "safe annotation identifier")
   assert(_note_record(0x401000, "entry").get("key", "") == "0x401000", "address note key")
   print("✓ std.os.rev.decomp.annotations self-test passed")
}
