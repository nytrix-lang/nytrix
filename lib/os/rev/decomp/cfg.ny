;; Keywords: cfg blocks edges dominance loops jump-tables
;; Basic-block, jump-table, and control-flow graph recovery.
module std.os.rev.decomp.cfg *
use std.core
use std.core.str as str
use std.os.disasm as dasm
use std.os.rev.decomp.elf
use std.os.rev.decomp.collections (_list_has, _append_unique, _append_all_unique)
use std.os.rev.decomp.cfg_sets (_set_intersection, _same_set, _list_without, _set_difference)
use std.os.rev.decomp.bytes (_u32, _u64)
use std.os.rev.decomp.symbols (_slice_symbol_aliases)

fn _cfg_arch(dict bin) str {
   bin.get("header", dict()).get("machine", "unknown")
}

fn _row_target(str arch_name, list row) int {
   dasm.target_address(row, arch_name)
}

fn _rip_memory_target(list row) int {
   def ops = row[2]
   def p = str.find(ops, "rip")
   if p < 0 { return 0 }
   mut sign = 1
   mut i = p + 3
   while i < ops.len && str.ascii_is_space(load8(ops, i)) { i += 1 }
   if i < ops.len && load8(ops, i) == 45 { sign = -1 i += 1 }
   elif i < ops.len && load8(ops, i) == 43 { i += 1 }
   while i < ops.len && str.ascii_is_space(load8(ops, i)) { i += 1 }
   mut val = 0
   if i + 1 < ops.len && load8(ops, i) == 48 && (load8(ops, i + 1) == 120 || load8(ops, i + 1) == 88) {
      mut end = i + 2
      while end < ops.len && str.ascii_is_hex_digit(load8(ops, end)) { end += 1 }
      val = str.parse_int(slice(ops, i + 2, end, 1), 16)
   } else {
      mut end = i
      while end < ops.len && str.ascii_is_digit(load8(ops, end)) { end += 1 }
      if end > i { val = str.parse_int(slice(ops, i, end, 1), 10) }
   }
   int(row[0]) + int(row[3]) + sign * val
}

fn _label_name(int addr) str { "loc_" + str.to_hex(addr, 0) }

fn _cfg_safe_name(str name, str fallback) str {
   if name.len == 0 { return fallback }
   mut out = ""
   mut i = 0
   while i < name.len {
      def c = load8(name, i)
      out = out + ((str.ascii_is_alnum(c) || c == 95) ? chr(c) : "_")
      i += 1
   }
   out.len > 0 ? out : fallback
}

fn _rename_addr(dict bin, int addr, str fallback) str {
   def renames = bin.get("renames", dict())
   def hex = "0x" + str.to_hex(addr, 0)
   _cfg_safe_name(renames.get(hex, renames.get(str.to_hex(addr, 0), renames.get(to_str(addr), renames.get(fallback, fallback)))), fallback)
}

fn _is_block_end(str a, str m) bool {
   def k = dasm.instruction_kind(a, m)
   k == "branch" || k == "return" || k == "call" || k == "syscall"
}

fn _row_end(any r) int {
   if !is_list(r) { return 0 }
   if r.len < 4 { return r.len > 0 ? int(r[0]) : 0 }
   int(r[0]) + int(r[3])
}

fn _row_addr_index(list rows) dict {
   mut out = dict()
   mut i = 0
   while i < rows.len {
      out = out.set(to_str(int(rows[i][0])), i)
      i += 1
   }
   out
}

fn _block_leaders(list rows, str a) dict {
   mut leaders = dict()
   if rows.len == 0 { return leaders }
   leaders = leaders.set(to_str(int(rows[0][0])), true)
   def idx = _row_addr_index(rows)
   mut i = 0
   while i < rows.len {
      def r = rows[i]
      def k = dasm.instruction_kind(a, r[1])
      if k == "branch" {
         def t = str.find(r[2], "[") >= 0 ? 0 : _row_target(a, r)
         if t != 0 && idx.contains(to_str(t)) { leaders = leaders.set(to_str(t), true) }
         if i + 1 < rows.len { leaders = leaders.set(to_str(int(rows[i + 1][0])), true) }
      } elif k == "call" || k == "syscall" {
         if i + 1 < rows.len { leaders = leaders.set(to_str(int(rows[i + 1][0])), true) }
      }
      i += 1
   }
   leaders
}

fn _basic_blocks_from_rows(list rows, str a) list {
   def leaders = _block_leaders(rows, a)
   mut blocks = []
   mut cur = []
   mut start = 0
   mut i = 0
   while i < rows.len {
      def r = rows[i]
      if cur.len > 0 && leaders.get(to_str(int(r[0])), false) {
         blocks = blocks.append({"addr": start, "end": _row_end(cur[cur.len - 1]), "rows": cur})
         cur = []
      }
      if cur.len == 0 { start = int(r[0]) }
      cur = cur.append(r)
      if _is_block_end(a, r[1]) {
         blocks = blocks.append({"addr": start, "end": _row_end(r), "rows": cur})
         cur = []
      }
      i += 1
   }
   if cur.len > 0 { blocks = blocks.append({"addr": start, "end": _row_end(list(cur[cur.len - 1])), "rows": cur}) }
   blocks
}

fn basic_blocks(any source, any target=0, int max_bytes=1024) list {
   "Split disassembly into compact basic-block records."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   _basic_blocks_from_rows(disassemble(bin, target, max_bytes), _cfg_arch(bin))
}

fn _node_addr_set(list nodes) dict {
   mut out = dict()
   mut i = 0
   while i < nodes.len {
      out = out.set(to_str(int(nodes[i].get("addr", 0))), true)
      i += 1
   }
   out
}

fn _edge_unique(list edges, dict edge) list {
   def key = to_str(edge.get("from", 0)) + ":" + to_str(edge.get("to", 0)) + ":" + edge.get("kind", "")
   mut i = 0
   while i < edges.len {
      def e = edges[i]
      def ek = to_str(e.get("from", 0)) + ":" + to_str(e.get("to", 0)) + ":" + e.get("kind", "")
      if ek == key { return edges }
      i += 1
   }
   edges = edges.append(edge)
   edges
}

fn _addr_in_executable(dict bin, int addr) bool {
   def xs = executable_sections(bin)
   mut i = 0
   while i < xs.len {
      def s = xs[i]
      def start = int(s.get("addr", 0))
      def end = start + int(s.get("size", 0))
      if addr >= start && addr < end { return true }
      i += 1
   }
   false
}

fn _first_hex_in_brackets(str ops) int {
   def lb = str.find(ops, "[")
   if lb < 0 { return 0 }
   mut rb = lb + 1
   while rb < ops.len && load8(ops, rb) != 93 { rb += 1 }
   mut i = lb + 1
   while i + 1 < rb {
      if load8(ops, i) == 48 && (load8(ops, i + 1) == 120 || load8(ops, i + 1) == 88) {
         mut end = i + 2
         while end < rb && str.ascii_is_hex_digit(load8(ops, end)) { end += 1 }
         if end > i + 2 { return str.parse_int(slice(ops, i + 2, end, 1), 16) }
      }
      i += 1
   }
   0
}

fn _jump_table_base_for_row(list row) int {
   def ops = row[2]
   if str.find(ops, "[") < 0 { return 0 }
   def rip = _rip_memory_target(row)
   rip != 0 ? rip : _first_hex_in_brackets(ops)
}

fn _jump_table_index_reg_for_row(list row) str {
   _jump_table_index_reg_for_operand(row[2])
}

fn _jump_table_index_reg_for_operand(str operand) str {
   def ops = str.lower(operand)
   def lb = str.find(ops, "[")
   if lb < 0 { return "" }
   mut rb = lb + 1
   while rb < ops.len && load8(ops, rb) != 93 { rb += 1 }
   mut i = lb + 1
   mut start = i
   while i <= rb {
      if i == rb || load8(ops, i) == 43 || load8(ops, i) == 45 {
         def term = str.strip(slice(ops, start, i, 1))
         def star = str.find(term, "*")
         def reg = star >= 0 ? str.strip(slice(term, 0, star, 1)) : term
         if reg.len > 0 && !str.startswith(reg, "0x") && !str.ascii_is_digit(load8(reg, 0)) && reg != "rip" {
            return reg
         }
         start = i + 1
      }
      i += 1
   }
   ""
}

fn _jump_table_scaled_index_reg_for_operand(str operand, str base_reg="") str {
   def ops = str.lower(operand)
   def base = str.lower(base_reg)
   def lb = str.find(ops, "[")
   if lb < 0 { return "" }
   mut rb = lb + 1
   while rb < ops.len && load8(ops, rb) != 93 { rb += 1 }
   mut i = lb + 1
   mut start = i
   while i <= rb {
      if i == rb || load8(ops, i) == 43 || load8(ops, i) == 45 {
         def term = str.strip(slice(ops, start, i, 1))
         def star = str.find(term, "*")
         if star >= 0 {
            def reg = str.strip(slice(term, 0, star, 1))
            if reg.len > 0 && reg != base { return reg }
         }
         start = i + 1
      }
      i += 1
   }
   def fallback = _jump_table_index_reg_for_operand(operand)
   fallback == base ? "" : fallback
}

fn _u32_signed(int v) int {
   (v & 0x80000000) != 0 ? (v - 0x100000000) : v
}

fn _read_ptr(dict bin, int addr) int {
   def off = _vaddr_to_offset(bin, addr)
   if off < 0 { return 0 }
   def h = bin.get("header", dict())
   def le = h.get("little", true)
   int(h.get("bits", 64)) == 64 ? _u64(bin.get("data", ""), off, le) : _u32(bin.get("data", ""), off, le)
}

fn _jump_table_for_row(dict bin, list row, int max_entries=32) dict {
   if dasm.instruction_kind(_cfg_arch(bin), row[1]) != "branch" { return dict() }
   if dasm.branch_condition(_cfg_arch(bin), row[1]) != "always" { return dict() }
   def base = _jump_table_base_for_row(row)
   if base == 0 { return dict() }
   def h = bin.get("header", dict())
   def ptr_size = int(h.get("bits", 64)) == 64 ? 8 : 4
   mut entries = []
   mut i = 0
   while i < max_entries {
      def target = _read_ptr(bin, base + i * ptr_size)
      if target == 0 || !_addr_in_executable(bin, target) { break }
      entries = entries.append({"index": i, "target": target, "label": _rename_addr(bin, target, _label_name(target))})
      i += 1
   }
   entries.len >= 2 ? {"kind": "jump_table", "from": int(row[0]), "operand": row[2], "base": base, "entry_size": ptr_size, "index_reg": _jump_table_index_reg_for_row(row), "count": entries.len, "entries": entries} : dict()
}

fn _relative_jump_table_entries(dict bin, int base, int max_entries=32) list {
   if base == 0 { return [] }
   def off = _vaddr_to_offset(bin, base)
   if off < 0 { return [] }
   def h = bin.get("header", dict())
   def le = h.get("little", true)
   mut entries = []
   mut i = 0
   while i < max_entries {
      def rel = _u32_signed(_u32(bin.get("data", ""), off + i * 4, le))
      def target = base + rel
      if target == 0 || !_addr_in_executable(bin, target) { break }
      entries = entries.append({"index": i, "target": target, "label": _rename_addr(bin, target, _label_name(target)), "relative": rel})
      i += 1
   }
   entries
}

fn _relative_switch_for_lift_rows(dict bin, list rows, int idx, int max_entries=32) dict {
   if idx < 0 || idx >= rows.len { return dict() }
   def jr = rows[idx]
   if jr.get("op", "") != "branch" || jr.get("condition", "") != "always" || int(jr.get("target", 0)) != 0 { return dict() }
   def jump_reg = str.lower(str.strip(jr.get("operands", "")))
   if jump_reg.len == 0 || str.find(jump_reg, "[") >= 0 { return dict() }
   mut base_reg = ""
   mut index_reg = ""
   mut source_index_reg = ""
   mut table_base = 0
   mut consumes = []
   mut j = idx - 1
   while j >= 0 && idx - j <= 32 {
      def r = rows[j]
      def m = str.lower(r.get("mnemonic", ""))
      def dst = str.lower(r.get("dst", ""))
      def src = str.lower(r.get("src", ""))
      if base_reg.len == 0 && r.get("op", "") == "arith" && r.get("operator", "") == "+" && dst == jump_reg && r.get("src_kind", "") == "reg" {
         base_reg = src
         consumes = _append_unique(consumes, j)
      } elif base_reg.len > 0 && index_reg.len == 0 && r.get("op", "") == "assign" && dst == jump_reg && r.get("src_kind", "") == "mem" && str.startswith(m, "movsx") && str.find(src, base_reg) >= 0 {
         index_reg = _jump_table_scaled_index_reg_for_operand(src, base_reg)
         consumes = _append_unique(consumes, j)
      } elif index_reg.len > 0 && source_index_reg.len == 0 && r.get("op", "") == "assign" && _slice_symbol_aliases(dst).contains(index_reg) && r.get("src_kind", "") == "reg" {
         source_index_reg = src
         consumes = _append_unique(consumes, j)
      } elif base_reg.len > 0 && table_base == 0 && r.get("kind", "") == "lea" && dst == base_reg {
         table_base = int(r.get("ref_target", 0))
      }
      j -= 1
   }
   if table_base == 0 || index_reg.len == 0 { return dict() }
   if source_index_reg.len > 0 { index_reg = source_index_reg }
   def entries = _relative_jump_table_entries(bin, table_base, max_entries)
   if entries.len < 2 { return dict() }
   {"kind": "jump_table", "from": int(jr.get("addr", 0)), "operand": jr.get("operands", ""),
      "base": table_base, "entry_size": 4, "relative": true, "index_reg": index_reg,
      "count": entries.len, "entries": entries, "consumes": consumes}
}

fn _jump_tables_from_rows(dict bin, list rows, int max_entries=32) list {
   mut out = []
   mut seen = dict()
   mut i = 0
   while i < rows.len {
      def jt = _jump_table_for_row(bin, rows[i], max_entries)
      if jt.len > 0 {
         def key = to_str(jt.get("from", 0)) + ":" + to_str(jt.get("base", 0))
         if !seen.get(key, false) {
            seen = seen.set(key, true)
            out = out.append(jt)
         }
      }
      i += 1
   }
   out
}

fn jump_tables(any source, any target=0, int max_bytes=2048, int max_entries=32) list {
   "Recover bounded pointer jump tables used by compiler switch lowering."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   def rows = target == 0 ? disassemble(bin, ".text", max_bytes) : disassemble(bin, target, max_bytes)
   _jump_tables_from_rows(bin, rows, max_entries)
}

fn _cfg_from_blocks(dict bin, list blocks) dict {
   def a = _cfg_arch(bin)
   mut nodes = []
   mut edges = []
   mut i = 0
   while i < blocks.len {
      def b = blocks[i]
      nodes = nodes.append({"addr": b.get("addr", 0), "end": b.get("end", 0), "insns": b.get("rows", []).len})
      i += 1
   }
   i = 0
   while i < blocks.len {
      def b = blocks[i]
      def rows = b.get("rows", [])
      if rows.len > 0 {
         def last = rows[rows.len - 1]
         def m = last[1]
         def k = dasm.instruction_kind(a, m)
         def cond = dasm.branch_condition(a, m)
         def t = str.find(last[2], "[") >= 0 ? 0 : _row_target(a, last)
         if k == "call" && t != 0 {
            edges = _edge_unique(edges, {"from": b.get("addr", 0), "to": t, "kind": "call"})
         } elif k == "branch" && t != 0 {
            edges = _edge_unique(edges, {"from": b.get("addr", 0), "to": t, "kind": cond == "always" ? "jump" : "branch", "condition": cond})
         } elif k == "branch" && t == 0 && cond == "always" {
            def jt = _jump_table_for_row(bin, last, 32)
            def es = jt.get("entries", [])
            mut ei = 0
            while ei < es.len {
               edges = _edge_unique(edges, {"from": b.get("addr", 0), "to": int(es[ei].get("target", 0)), "kind": "switch", "index": int(es[ei].get("index", 0)), "table": int(jt.get("base", 0))})
               ei += 1
            }
         }
         if k != "return" && cond != "always" && i + 1 < blocks.len {
            edges = _edge_unique(edges, {"from": b.get("addr", 0), "to": blocks[i + 1].get("addr", 0), "kind": "fallthrough"})
         }
      }
      i += 1
   }
   {"nodes": nodes, "edges": edges, "entry": nodes.len > 0 ? nodes[0].get("addr", 0) : 0, "node_count": nodes.len, "edge_count": edges.len}
}

fn _cfg_from_rows(dict bin, list rows) dict {
   _cfg_from_blocks(bin, _basic_blocks_from_rows(rows, _cfg_arch(bin)))
}

fn cfg(any source, any target=0, int max_bytes=2048) dict {
   "Recover a local control-flow graph from basic blocks.
   Edges are conservative: direct branches/calls, bounded jump tables, plus simple fallthrough."
   def bin = is_dict(source) && source.contains("header") ? source : load(to_str(source))
   _cfg_from_blocks(bin, basic_blocks(bin, target, max_bytes))
}

fn _cfg_successors(dict graph, int addr) list {
   def edges = graph.get("edges", [])
   mut out = []
   mut i = 0
   while i < edges.len {
      if int(edges[i].get("from", 0)) == addr { out = _append_unique(out, int(edges[i].get("to", 0))) }
      i += 1
   }
   out
}

fn _cfg_predecessors(dict graph, int addr) list {
   def edges = graph.get("edges", [])
   mut out = []
   mut i = 0
   while i < edges.len {
      if int(edges[i].get("to", 0)) == addr { out = _append_unique(out, int(edges[i].get("from", 0))) }
      i += 1
   }
   out
}

fn _cfg_nodes_addrs(dict graph) list {
   def nodes = graph.get("nodes", [])
   mut out = []
   mut i = 0
   while i < nodes.len {
      out = out.append(int(nodes[i].get("addr", 0)))
      i += 1
   }
   out
}

fn _cfg_dominators_for_graph(dict graph) dict {
   def all = _cfg_nodes_addrs(graph)
   if all.len == 0 { return {"entry": 0, "dominators": dict(), "nodes": []} }
   def entry = int(graph.get("entry", all[0]))
   mut dom = dict()
   mut i = 0
   while i < all.len {
      def n = int(all[i])
      dom = dom.set(to_str(n), n == entry ? [entry] : all)
      i += 1
   }
   mut changed = true
   while changed {
      changed = false
      i = 0
      while i < all.len {
         def n = int(all[i])
         if n != entry {
            def preds = _cfg_predecessors(graph, n)
            mut ndom = all
            mut pi = 0
            if preds.len == 0 { ndom = [] }
            while pi < preds.len {
               ndom = _set_intersection(ndom, dom.get(to_str(preds[pi]), []))
               pi += 1
            }
            ndom = _append_unique(ndom, n)
            if !_same_set(ndom, dom.get(to_str(n), [])) {
               dom = dom.set(to_str(n), ndom)
               changed = true
            }
         }
         i += 1
      }
   }
   {"entry": entry, "dominators": dom, "nodes": all}
}

fn _cfg_idominators_from_dominfo(dict dominfo) dict {
   def all = dominfo.get("nodes", [])
   def dom = dominfo.get("dominators", dict())
   def entry = int(dominfo.get("entry", all.len > 0 ? all[0] : 0))
   mut idom = dict()
   mut i = 0
   while i < all.len {
      def n = int(all[i])
      if n != entry {
         def candidates = _list_without(dom.get(to_str(n), []), n)
         mut chosen = 0
         mut ci = 0
         while ci < candidates.len {
            def c = int(candidates[ci])
            mut closer = true
            mut oi = 0
            while oi < candidates.len {
               def other = int(candidates[oi])
               if other != c && _list_has(dom.get(to_str(other), []), c) { closer = false }
               oi += 1
            }
            if closer { chosen = c }
            ci += 1
         }
         if chosen != 0 { idom = idom.set(to_str(n), chosen) }
      }
      i += 1
   }
   idom
}

fn _cfg_dominance_frontiers_for_graph(dict graph, dict dominfo) dict {
   def nodes = dominfo.get("nodes", _cfg_nodes_addrs(graph))
   def idom = _cfg_idominators_from_dominfo(dominfo)
   mut frontier = dict()
   mut i = 0
   while i < nodes.len {
      frontier = frontier.set(to_str(int(nodes[i])), [])
      i += 1
   }
   i = 0
   while i < nodes.len {
      def n = int(nodes[i])
      def preds = _cfg_predecessors(graph, n)
      if preds.len >= 2 {
         mut pi = 0
         while pi < preds.len {
            mut runner = int(preds[pi])
            def stop = int(idom.get(to_str(n), 0))
            mut guard = 0
            while runner != 0 && runner != stop && guard < nodes.len + 4 {
               frontier = frontier.set(to_str(runner), _append_unique(frontier.get(to_str(runner), []), n))
               runner = int(idom.get(to_str(runner), 0))
               guard += 1
            }
            pi += 1
         }
      }
      i += 1
   }
   {"idominators": idom, "frontiers": frontier}
}

fn cfg_dominators(any source, any target=0, int max_bytes=2048) dict {
   "Compute local CFG dominator sets for decompiler structuring passes."
   def graph = cfg(source, target, max_bytes)
   _cfg_dominators_for_graph(graph)
}

fn _cfg_exits(dict graph) list {
   def nodes = graph.get("nodes", [])
   mut out = []
   mut i = 0
   while i < nodes.len {
      def n = int(nodes[i].get("addr", 0))
      if _cfg_successors(graph, n).len == 0 { out = out.append(n) }
      i += 1
   }
   if out.len == 0 && nodes.len > 0 { out = out.append(int(nodes[nodes.len - 1].get("addr", 0))) }
   out
}

fn _cfg_postdominators_for_graph(dict graph) dict {
   def nodes = graph.get("nodes", [])
   if nodes.len == 0 { return {"cfg": graph, "exits": [], "postdominators": dict(), "ipostdominators": dict(), "nodes": []} }
   mut all = []
   mut i = 0
   while i < nodes.len { all = all.append(int(nodes[i].get("addr", 0))) i += 1 }
   def exits = _cfg_exits(graph)
   mut pdom = dict()
   i = 0
   while i < all.len {
      def n = int(all[i])
      pdom = pdom.set(to_str(n), _list_has(exits, n) ? [n] : all)
      i += 1
   }
   mut changed = true
   while changed {
      changed = false
      i = 0
      while i < all.len {
         def n = int(all[i])
         if !_list_has(exits, n) {
            def succs = _cfg_successors(graph, n)
            mut next = all
            mut si = 0
            if succs.len == 0 { next = [] }
            while si < succs.len {
               next = _set_intersection(next, pdom.get(to_str(succs[si]), []))
               si += 1
            }
            next = _append_unique(next, n)
            if !_same_set(next, pdom.get(to_str(n), [])) {
               pdom = pdom.set(to_str(n), next)
               changed = true
            }
         }
         i += 1
      }
   }
   mut ipdom = dict()
   i = 0
   while i < all.len {
      def n = int(all[i])
      def candidates = _list_without(pdom.get(to_str(n), []), n)
      mut chosen = 0
      mut ci = 0
      while ci < candidates.len {
         def c = int(candidates[ci])
         mut closer = true
         mut oi = 0
         while oi < candidates.len {
            def other = int(candidates[oi])
            if other != c && _list_has(pdom.get(to_str(other), []), c) { closer = false }
            oi += 1
         }
         if closer { chosen = c }
         ci += 1
      }
      if chosen != 0 { ipdom = ipdom.set(to_str(n), chosen) }
      i += 1
   }
   {"cfg": graph, "exits": exits, "postdominators": pdom, "ipostdominators": ipdom, "nodes": all}
}

fn cfg_postdominators(any source, any target=0, int max_bytes=2048) dict {
   "Compute local CFG postdominator sets for structured Ny rendering."
   _cfg_postdominators_for_graph(cfg(source, target, max_bytes))
}

fn _cfg_control_dependence_for_graph(dict graph, dict post) dict {
   def pdom = post.get("postdominators", dict())
   def edges = graph.get("edges", [])
   mut regions = []
   mut dependents = dict()
   mut i = 0
   while i < edges.len {
      def e = edges[i]
      def from = int(e.get("from", 0))
      def to = int(e.get("to", 0))
      if !_list_has(pdom.get(to_str(from), []), to) {
         mut controlled = _set_difference(pdom.get(to_str(to), []), pdom.get(to_str(from), []))
         if controlled.len == 0 { controlled = [to] }
         regions = regions.append({"controller": from, "to": to, "edge": e, "condition": e.get("condition", ""), "kind": e.get("kind", ""), "nodes": controlled})
         mut ci = 0
         while ci < controlled.len {
            def n = int(controlled[ci])
            dependents = dependents.set(to_str(n), _append_unique(dependents.get(to_str(n), []), from))
            ci += 1
         }
      }
      i += 1
   }
   {"cfg": graph, "postdominators": post, "regions": regions, "dependents": dependents, "count": regions.len}
}

fn cfg_control_dependence(any source, any target=0, int max_bytes=2048) dict {
   "Return control-dependence regions from CFG postdominators.
   Each region identifies a branch block and the blocks controlled by each outgoing edge."
   def graph = cfg(source, target, max_bytes)
   _cfg_control_dependence_for_graph(graph, _cfg_postdominators_for_graph(graph))
}

fn _cfg_loops_for_graph(dict graph, dict dominfo) dict {
   def dom = dominfo.get("dominators", dict())
   def edges = graph.get("edges", [])
   mut loops = []
   mut back = []
   mut i = 0
   while i < edges.len {
      def e = edges[i]
      def from = int(e.get("from", 0))
      def to = int(e.get("to", 0))
      if _list_has(dom.get(to_str(from), []), to) {
         def e2 = clone(e).set("kind", "back_edge")
         back = back.append(e2)
         loops = loops.append({"header": to, "latch": from, "back_edge": e2, "body_hint": [to, from]})
      }
      i += 1
   }
   {"cfg": graph, "back_edges": back, "loops": loops, "count": loops.len}
}

fn cfg_loops(any source, any target=0, int max_bytes=2048) dict {
   "Return natural loop/back-edge hints from the local CFG."
   def graph = cfg(source, target, max_bytes)
   _cfg_loops_for_graph(graph, _cfg_dominators_for_graph(graph))
}
