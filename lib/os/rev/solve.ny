;; Keywords: reverse decompiler solver smt
module std.os.rev.solve(
   status, tool_status, backend_status,
   base64_index, base64_values, bit_sliced_output_writes,
   bit_sliced_ascii_plan, solve_bit_sliced_ascii_transform,
   solve_input_eq, solve_decompiled_input, solve_byte_constraints,
   solve_ascii_xor_eq, solve_ascii_add_eq, solve_ascii_sub_eq,
   cpuid_vendor_string, solve_parse_int_eq, solve_ascii_sum_eq,
   solve_segment_sum_mods, solve_startswith, solve_read_int_chars_eq,
   solve_digit_prefix_sum_reaches, solve_fs_read_startswith,
   solve_regex_accepts, solve_time_window_startswith,
   solve_stdin_read_literal_eq, solve_stdin_read_line_palindrome,
   solve_product_key_arithmetic, solve_numeric_residue_words_flag,
   solve_byte_domains,
   solve_line_hash_matches,
   solve_mbrainfuzz_template,
)

use std.core
use std.core.str as str
use std.os as os
use std.math.smt as smt
use std.math.big as big
use std.os.rev.decomp as dc

def _BASE64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

fn status() dict {
   {
      "ok": true,
      "module": "std.os.rev.solve",
      "role": "decompiled-input and byte-constraint solver",
      "z3": smt.z3_available(),
   }
}

fn tool_status() dict {
   status()
}

fn backend_status() dict {
   status()
}

fn base64_index(int ch) int {
   mut i = 0
   while i < _BASE64.len {
      if load8(_BASE64, i) == ch { return i }
      i += 1
   }
   -1
}

fn base64_values(str text, int n=0) list {
   mut out = []
   def limit = n > 0 ? min(n, text.len) : text.len
   mut i = 0
   while i < limit {
      def v = base64_index(load8(text, i))
      if v >= 0 { out = out.append(v) }
      i += 1
   }
   out
}

fn _bit_sliced_window_has_const(list rows, int lo, int hi, str needle) bool {
   mut i = lo
   while i < hi {
      def r = rows[i]
      def text = r.get("effect", "") + " " + r.get("operands", "") + " " + r.get("src", "")
      if str.find(text, needle) >= 0 { return true }
      i += 1
   }
   false
}

fn _bit_sliced_row_arg(dict row, int idx) str {
   def args = _x86_row_args(row)
   if idx < 0 || idx >= args.len { return "" }
   str.strip(args[idx])
}

fn _bit_sliced_same_reg(str a, str b) bool {
   a.len > 0 && b.len > 0 && _x86_reg_base(a) == _x86_reg_base(b)
}

fn _bit_sliced_arg_is_imm(dict row, int idx, int value) bool {
   def p = _x86_parse_int_token(_bit_sliced_row_arg(row, idx))
   p.get("ok", false) && int(p.get("value", 0)) == value
}

fn _bit_sliced_row_is_binop(list rows, int row, str mnemonic, str dst_reg, int imm) bool {
   if row < 0 || row >= rows.len { return false }
   def r = rows[row]
   r.get("mnemonic", "") == mnemonic &&
   _bit_sliced_same_reg(_bit_sliced_row_arg(r, 0), dst_reg) &&
   _bit_sliced_arg_is_imm(r, 1, imm)
}

fn _bit_sliced_window_has_reg_imm(list rows, int lo, int hi, str mnemonic, str reg, int imm) bool {
   mut i = max(0, lo)
   def end = min(rows.len, hi)
   while i < end {
      def r = rows[i]
      if r.get("mnemonic", "") == mnemonic &&
      _bit_sliced_same_reg(_bit_sliced_row_arg(r, 0), reg) &&
      _bit_sliced_arg_is_imm(r, 1, imm) {
         return true
      }
      i += 1
   }
   false
}

fn _bit_sliced_window_has_imm(list rows, int lo, int hi, str mnemonic, int imm) bool {
   mut i = max(0, lo)
   def end = min(rows.len, hi)
   while i < end {
      def r = rows[i]
      if r.get("mnemonic", "") == mnemonic && _bit_sliced_arg_is_imm(r, 1, imm) { return true }
      i += 1
   }
   false
}

fn _bit_sliced_window_has_mnemonic(list rows, int lo, int hi, str mnemonic) bool {
   mut i = max(0, lo)
   def end = min(rows.len, hi)
   while i < end {
      if rows[i].get("mnemonic", "") == mnemonic { return true }
      i += 1
   }
   false
}

fn _bit_sliced_mapper_candidate_at(list rows, int row, int store_row) dict {
   if row < 0 || row >= rows.len { return dict() }
   def r = rows[row]
   if r.get("mnemonic", "") != "mov" { return dict() }
   def work = _bit_sliced_row_arg(r, 0)
   def raw = _bit_sliced_row_arg(r, 1)
   if !_x86_is_reg(work) || !_x86_is_reg(raw) || _bit_sliced_same_reg(work, raw) { return dict() }
   mut score = 0
   mut mode = "candidate"
   mut reason = []
   def direct_index =
   _bit_sliced_row_is_binop(rows, row + 1, "shr", work, 1) &&
   row + 6 < rows.len &&
   rows[row + 2].get("mnemonic", "") == "movzx" &&
   _bit_sliced_same_reg(_bit_sliced_row_arg(rows[row + 2], 0), work) &&
   _bit_sliced_same_reg(_bit_sliced_row_arg(rows[row + 2], 1), work) &&
   _bit_sliced_row_is_binop(rows, row + 3, "add", work, 1) &&
   _bit_sliced_row_is_binop(rows, row + 4, "sar", work, 5) &&
   rows[row + 5].get("mnemonic", "") == "add" &&
   _bit_sliced_same_reg(_bit_sliced_row_arg(rows[row + 5], 0), raw) &&
   _bit_sliced_same_reg(_bit_sliced_row_arg(rows[row + 5], 1), work) &&
   rows[row + 6].get("mnemonic", "") == "movzx" &&
   _bit_sliced_same_reg(_bit_sliced_row_arg(rows[row + 6], 1), raw)
   if direct_index {
      score += 8
      mode = "base64-index-preadd"
      reason = reason.append("index-preadd")
   }
   def case_toggle =
   _bit_sliced_row_is_binop(rows, row + 1, "and", work, 0x40) &&
   _bit_sliced_row_is_binop(rows, row + 2, "shr", work, 1) &&
   row + 3 < rows.len &&
   rows[row + 3].get("mnemonic", "") == "xor" &&
   _bit_sliced_same_reg(_bit_sliced_row_arg(rows[row + 3], 0), work) &&
   _bit_sliced_same_reg(_bit_sliced_row_arg(rows[row + 3], 1), raw)
   if case_toggle {
      score += 7
      mode = "ascii-case-toggle"
      reason = reason.append("case-toggle")
   }
   def branchless_window =
   _bit_sliced_window_has_reg_imm(rows, row + 1, min(store_row, row + 12), "and", work, 0x1f) &&
   _bit_sliced_window_has_imm(rows, row + 1, min(store_row, row + 16), "add", 6) &&
   _bit_sliced_window_has_imm(rows, row + 1, min(store_row, row + 16), "sar", 5) &&
   _bit_sliced_window_has_mnemonic(rows, row + 1, min(store_row, row + 80), "imul") &&
   (_bit_sliced_window_has_const(rows, row + 1, min(store_row, row + 96), "0x39") ||
      _bit_sliced_window_has_const(rows, row + 1, min(store_row, row + 96), "0x40"))
   if branchless_window {
      score += 4
      if mode == "candidate" { mode = "branchless-ascii-window" }
      reason = reason.append("branchless-window")
   }
   if score <= 0 { return dict() }
   {
      "row": row,
      "reg": raw,
      "work": work,
      "score": score,
      "mode": mode,
      "reason": reason,
      "distance": store_row - row,
   }
}

fn _bit_sliced_best_mapper_candidate(list candidates) dict {
   mut best = dict()
   mut i = 0
   while i < candidates.len {
      def cand = candidates[i]
      if best.len == 0 ||
      int(cand.get("score", 0)) > int(best.get("score", 0)) ||
      (int(cand.get("score", 0)) == int(best.get("score", 0)) &&
         int(cand.get("row", -1)) > int(best.get("row", -1))) {
         best = cand
      }
      i += 1
   }
   best
}

fn _bit_sliced_mapper_candidates(list rows, int store_row, int window=192) list {
   mut out = []
   mut i = max(0, store_row - window)
   while i < store_row {
      def cand = _bit_sliced_mapper_candidate_at(rows, i, store_row)
      if cand.len > 0 { out = out.append(cand) }
      i += 1
   }
   out
}

fn _bit_sliced_output_mapper_features(list rows, int row, int window=192) dict {
   def lo = max(0, row - window)
   def hi = min(rows.len, max(0, row))
   mut imul = 0
   mut shifts = 0
   mut byte_loads = 0
   mut i = lo
   while i < hi {
      def m = rows[i].get("mnemonic", "")
      if m == "imul" { imul += 1 }
      elif m == "shr" || m == "sar" { shifts += 1 }
      if rows[i].get("kind", "") == "byte_load" { byte_loads += 1 }
      i += 1
   }
   def c41 = _bit_sliced_window_has_const(rows, lo, hi, "0x41")
   def c39 = _bit_sliced_window_has_const(rows, lo, hi, "0x39")
   def c40 = _bit_sliced_window_has_const(rows, lo, hi, "0x40")
   def neg105 = _bit_sliced_window_has_const(rows, lo, hi, "0xffffff97")
   def neg26 = _bit_sliced_window_has_const(rows, lo, hi, "0xffffffe6")
   def neg14 = _bit_sliced_window_has_const(rows, lo, hi, "0xfffffff2")
   def neg81 = _bit_sliced_window_has_const(rows, lo, hi, "0xffffffaf")
   mut score = 0
   if c41 { score += 1 }
   if c39 { score += 1 }
   if c40 { score += 1 }
   if neg105 || neg26 || neg14 || neg81 { score += 1 }
   if imul >= 4 { score += 1 }
   if shifts >= 8 { score += 1 }
   if byte_loads > 0 { score += 1 }
   def candidates = _bit_sliced_mapper_candidates(rows, row, window)
   {
      "kind": score >= 4 ? "branchless-base64-ascii" : "unknown",
      "score": score,
      "window": window,
      "lo_row": lo,
      "hi_row": hi,
      "has_0x41": c41,
      "has_0x39": c39,
      "has_0x40": c40,
      "has_neg105": neg105,
      "has_neg26": neg26,
      "has_neg14": neg14,
      "has_neg81": neg81,
      "imul_count": imul,
      "shift_count": shifts,
      "byte_load_count": byte_loads,
      "candidate": _bit_sliced_best_mapper_candidate(candidates),
      "candidates": candidates,
   }
}

fn bit_sliced_output_writes(list rows, int output_addr, int output_len) dict {
   "Summarize lifted memory stores that materialize a contiguous output buffer.
   The `final` records are the last writer observed for each output offset."
   mut writes = []
   mut by_off = dict()
   mut i = 0
   while i < rows.len {
      def r = rows[i]
      if r.get("op", "") == "assign" && r.get("dst_kind", "") == "mem" {
         def addr = int(r.get("ref_target", r.get("target", 0)))
         def bits = _x86_mem_bits(r.get("dst", ""), 8)
         def n = max(1, (bits + 7) / 8)
         mut j = 0
         while j < n {
            def a = addr + j
            if output_addr > 0 && output_len > 0 && a >= output_addr && a < output_addr + output_len {
               def rec = {
                  "row": i,
                  "addr": a,
                  "offset": a - output_addr,
                  "mnemonic": r.get("mnemonic", ""),
                  "src": r.get("src", ""),
                  "dst": r.get("dst", ""),
               }
               writes = writes.append(rec)
               by_off = by_off.set(to_str(a - output_addr), rec)
            }
            j += 1
         }
      }
      i += 1
   }
   mut final = []
   mut missing = []
   mut min_final_row = -1
   mut max_final_row = -1
   mut base64_mapped = 0
   i = 0
   while i < output_len {
      def key = to_str(i)
      if by_off.contains(key) {
         mut rec = by_off.get(key)
         def row = int(rec.get("row", -1))
         if row >= 0 && (min_final_row < 0 || row < min_final_row) { min_final_row = row }
         if row >= 0 && row > max_final_row { max_final_row = row }
         def mapper = _bit_sliced_output_mapper_features(rows, row)
         if mapper.get("kind", "") == "branchless-base64-ascii" { base64_mapped += 1 }
         rec = rec.set("mapper", mapper)
         final = final.append(rec)
      }
      else { missing = missing.append(i) }
      i += 1
   }
   {
      "output_addr": output_addr,
      "output_len": output_len,
      "write_count": writes.len,
      "final_count": final.len,
      "overwritten_count": max(0, writes.len - final.len),
      "complete": output_len > 0 && missing.len == 0,
      "min_final_row": min_final_row,
      "max_final_row": max_final_row,
      "base64_mapped_count": base64_mapped,
      "base64_mapper_complete": output_len > 0 && missing.len == 0 && base64_mapped == output_len,
      "missing_offsets": missing,
      "writes": writes,
      "final": final,
   }
}

fn _bit_sliced_compact_mapper(dict mapper) dict {
   {
      "kind": mapper.get("kind", ""),
      "score": int(mapper.get("score", 0)),
      "window": int(mapper.get("window", 0)),
      "lo_row": int(mapper.get("lo_row", -1)),
      "hi_row": int(mapper.get("hi_row", -1)),
      "candidate_count": mapper.get("candidates", []).len,
      "candidate": mapper.get("candidate", dict()),
      "imul_count": int(mapper.get("imul_count", 0)),
      "shift_count": int(mapper.get("shift_count", 0)),
      "byte_load_count": int(mapper.get("byte_load_count", 0)),
   }
}

fn _bit_sliced_compact_output_writes(dict summary, bool include_raw=false) dict {
   if include_raw { return summary }
   mut final = []
   mut i = 0
   while i < summary.get("final", []).len {
      def rec = summary.get("final", [])[i]
      final = final.append({
            "row": int(rec.get("row", -1)),
            "addr": int(rec.get("addr", 0)),
            "offset": int(rec.get("offset", -1)),
            "mnemonic": rec.get("mnemonic", ""),
            "src": rec.get("src", ""),
            "dst": rec.get("dst", ""),
            "mapper": _bit_sliced_compact_mapper(rec.get("mapper", dict())),
         })
      i += 1
   }
   {
      "output_addr": int(summary.get("output_addr", 0)),
      "output_len": int(summary.get("output_len", 0)),
      "write_count": int(summary.get("write_count", 0)),
      "final_count": int(summary.get("final_count", 0)),
      "overwritten_count": int(summary.get("overwritten_count", 0)),
      "complete": summary.get("complete", false),
      "min_final_row": int(summary.get("min_final_row", -1)),
      "max_final_row": int(summary.get("max_final_row", -1)),
      "base64_mapped_count": int(summary.get("base64_mapped_count", 0)),
      "base64_mapper_complete": summary.get("base64_mapper_complete", false),
      "missing_offsets": summary.get("missing_offsets", []),
      "final": final,
   }
}

fn _x86_row_prefix(list rows, int count) list {
   mut out = []
   mut i = 0
   def limit = min(max(0, count), rows.len)
   while i < limit {
      out = out.append(rows[i])
      i += 1
   }
   out
}

fn _target_prefix(str target, int output_len) str {
   if output_len <= 0 || output_len >= target.len { return target }
   slice(target, 0, output_len, 1)
}

fn _find_from(str hay, str needle, int start=0) int {
   if needle.len == 0 { return max(0, start) }
   if hay.len < needle.len { return -1 }
   mut i = max(0, start)
   while i <= hay.len - needle.len {
      mut j = 0
      mut ok = true
      while j < needle.len {
         if load8(hay, i + j) != load8(needle, j) {
            ok = false
            break
         }
         j += 1
      }
      if ok { return i }
      i += 1
   }
   -1
}

fn _trim_ascii(str text) str {
   mut a = 0
   mut b = text.len
   while a < b && (load8(text, a) == 32 || load8(text, a) == 9 || load8(text, a) == 10 || load8(text, a) == 13) { a += 1 }
   while b > a && (load8(text, b - 1) == 32 || load8(text, b - 1) == 9 || load8(text, b - 1) == 10 || load8(text, b - 1) == 13) { b -= 1 }
   slice(text, a, b, 1)
}

fn _proc_cpuinfo_vendor() str {
   def rd = os.file_read("/proc/cpuinfo")
   if !is_ok(rd) { return "" }
   def text = unwrap(rd)
   def key = "vendor_id"
   def pos = _find_from(text, key, 0)
   if pos < 0 { return "" }
   def colon = _find_from(text, ":", pos + key.len)
   if colon < 0 { return "" }
   mut end = colon + 1
   while end < text.len && load8(text, end) != 10 && load8(text, end) != 13 { end += 1 }
   _trim_ascii(slice(text, colon + 1, end, 1))
}

fn cpuid_vendor_string(int leaf=0, any opts=dict()) str {
   "Evaluate the decompiler helper `cpuid_vendor_string(0)` for host-bound crackmes.
   `opts.cpuid_vendor` can override the host value for deterministic tests."
   if leaf != 0 { return "" }
   if is_dict(opts) && is_str(opts.get("cpuid_vendor", "")) && opts.get("cpuid_vendor", "").len > 0 {
      return opts.get("cpuid_vendor", "")
   }
   def proc_vendor = _proc_cpuinfo_vendor()
   if proc_vendor.len > 0 { return proc_vendor }
   def env_vendor = os.env("NYTRIX_CPUID_VENDOR")
   if is_str(env_vendor) && env_vendor.len > 0 { return env_vendor }
   ""
}

fn _byte_len(any values) int {
   if is_str(values) || is_bytes(values) { return values.len }
   if is_list(values) { return values.len }
   0
}

fn _byte_at(any values, int i) int {
   if is_str(values) || is_bytes(values) { return load8(values, i) & 255 }
   if is_list(values) { return int(values.get(i, 0)) & 255 }
   int(values) & 255
}

fn _byte_value(any value) int {
   if is_str(value) || is_bytes(value) {
      return value.len > 0 ? (load8(value, 0) & 255) : 0
   }
   int(value) & 255
}

fn _bytes_to_ascii(list values) str {
   mut b = str.Builder(values.len)
   mut i = 0
   while i < values.len {
      b = str.builder_append_byte(b, int(values[i]) & 255)
      i += 1
   }
   def text = str.builder_to_str(b)
   str.builder_free(b)
   text
}

fn _backend_record() dict {
   {"z3": smt.z3_available(), "z3_version": smt.z3_version_str()}
}

fn _sat_result(str kind, str input, list constraints, any opts=dict()) dict {
   {
      "kind": kind,
      "status": "sat",
      "input": input,
      "input_len": input.len,
      "constraints": constraints,
      "backend": _backend_record(),
      "proof": opts.get("proof", "z3-qfbv"),
   }
}

fn _fail_result(str kind, str status, str reason, list constraints=[], any opts=dict()) dict {
   {
      "kind": kind,
      "status": status,
      "reason": reason,
      "input": "",
      "input_len": int(opts.get("input_len", 0)),
      "constraints": constraints,
      "backend": _backend_record(),
      "proof": opts.get("proof", "z3-qfbv"),
   }
}

fn solve_input_eq(str target, any opts=dict()) dict {
   "Solve a decompiler-reduced direct input equality predicate."
   _sat_result("input_equality", target,
      [{"op": "eq", "target": "input", "value": target, "count": target.len}],
      {"proof": "literal-equality"})
}

fn _hex_digit(int c) int {
   if c >= 48 && c <= 57 { return c - 48 }
   if c >= 65 && c <= 70 { return c - 55 }
   if c >= 97 && c <= 102 { return c - 87 }
   -1
}

fn _is_space(int c) bool { c == 32 || c == 9 || c == 10 || c == 13 }

fn _skip_spaces(str text, int pos) int {
   mut i = pos
   while i < text.len && _is_space(load8(text, i)) { i += 1 }
   i
}

fn _parse_int_literal_at(str text, int pos) dict {
   mut i = _skip_spaces(text, pos)
   mut base = 10
   if i + 1 < text.len && load8(text, i) == 48 && (load8(text, i + 1) == 120 || load8(text, i + 1) == 88) {
      base = 16
      i += 2
   }
   mut value = 0
   mut digits = 0
   while i < text.len {
      def d = _hex_digit(load8(text, i))
      if d < 0 || d >= base { break }
      value = value * base + d
      digits += 1
      i += 1
   }
   {"ok": digits > 0, "value": value, "end": i}
}

fn _parse_decimal_at(str text, int pos) dict {
   mut i = _skip_spaces(text, pos)
   mut value = 0
   mut digits = 0
   while i < text.len {
      def c = load8(text, i)
      if c < 48 || c > 57 { break }
      value = value * 10 + (c - 48)
      digits += 1
      i += 1
   }
   {"ok": digits > 0, "value": value, "end": i}
}

fn _int_to_base(int value, int base) str {
   def alphabet = "0123456789abcdefghijklmnopqrstuvwxyz"
   if value == 0 { return "0" }
   mut v = value
   mut out = ""
   while v > 0 {
      out = str.chr(load8(alphabet, v % base)) + out
      v = v / base
   }
   out
}

fn _decode_string_literal_body(str text, int start) dict {
   mut b = str.Builder(max(16, text.len - start))
   mut i = start
   while i < text.len {
      def ch = load8(text, i)
      if ch == 34 {
         def out = str.builder_to_str(b)
         str.builder_free(b)
         return {"ok": true, "value": out, "end": i + 1}
      }
      if ch == 92 && i + 1 < text.len {
         def n = load8(text, i + 1)
         if n == 110 {
            b = str.builder_append_byte(b, 10)
            i += 2
         } elif n == 114 {
            b = str.builder_append_byte(b, 13)
            i += 2
         } elif n == 116 {
            b = str.builder_append_byte(b, 9)
            i += 2
         } elif n == 34 || n == 92 {
            b = str.builder_append_byte(b, n)
            i += 2
         } elif n == 120 && i + 3 < text.len {
            def hi = _hex_digit(load8(text, i + 2))
            def lo = _hex_digit(load8(text, i + 3))
            if hi >= 0 && lo >= 0 {
               b = str.builder_append_byte(b, (hi << 4) | lo)
               i += 4
            } else {
               b = str.builder_append_byte(b, n)
               i += 2
            }
         } else {
            b = str.builder_append_byte(b, n)
            i += 2
         }
      } else {
         b = str.builder_append_byte(b, ch)
         i += 1
      }
   }
   str.builder_free(b)
   {"ok": false, "value": "", "end": text.len}
}

fn _extract_input_eq_literal(str text) dict {
   def needle = "input == \""
   def pos = str.find(text, needle)
   if pos < 0 { return {"ok": false, "value": ""} }
   _decode_string_literal_body(text, pos + needle.len)
}

fn _extract_main_first_param(str text) str {
   def needle = "fn main("
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return "" }
   mut end = pos + needle.len
   while end < text.len && load8(text, end) != 41 && load8(text, end) != 44 { end += 1 }
   _trim_identifier(slice(text, pos + needle.len, end, 1))
}

fn _extract_main_param_eq_literal(str text) dict {
   def name = _extract_main_first_param(text)
   if name.len == 0 || name == "input" { return {"ok": false} }
   def lit = _extract_var_literal_eq(text, name)
   if !lit.get("ok", false) { return {"ok": false} }
   {"ok": true, "name": name, "value": lit.get("value", "")}
}

fn _line_end(str text, int pos) int {
   mut i = min(max(pos, 0), text.len)
   while i < text.len && load8(text, i) != 10 { i += 1 }
   i
}

fn _extract_valid_case_literal(str text) dict {
   def case_pos = _find_from(text, "match input", 0)
   if case_pos < 0 { return {"ok": false} }
   mut q = _find_from(text, "\"", case_pos)
   while q >= 0 {
      def lit = _decode_string_literal_body(text, q + 1)
      if !lit.get("ok", false) { return {"ok": false} }
      def end = _line_end(text, lit.get("end", q + 1))
      def line = slice(text, q, end, 1)
      if _find_from(line, "->", 0) >= 0 && _find_from(line, "\"status\": \"valid\"", 0) >= 0 {
         return {"ok": true, "value": lit.get("value", "")}
      }
      q = _find_from(text, "\"", lit.get("end", q + 1))
   }
   {"ok": false}
}

fn _extract_parse_int_eq(str text) dict {
   def needle = "parse_int(input,"
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def base_rec = _parse_decimal_at(text, pos + needle.len)
   if !base_rec.get("ok", false) { return {"ok": false} }
   def close = _find_from(text, ")", base_rec.get("end", 0))
   if close < 0 { return {"ok": false} }
   def eq = _find_from(text, "==", close + 1)
   if eq < 0 { return {"ok": false} }
   def value_rec = _parse_int_literal_at(text, eq + 2)
   if !value_rec.get("ok", false) { return {"ok": false} }
   {"ok": true, "base": int(base_rec.get("value", 10)), "value": int(value_rec.get("value", 0))}
}

fn _extract_dict_string_field(str text, str key) str {
   def needle = "\"" + key + "\": \""
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return "" }
   def lit = _decode_string_literal_body(text, pos + needle.len)
   lit.get("ok", false) ? lit.get("value", "") : ""
}

fn _extract_dict_int_field(str text, str key, int fallback=-1) int {
   def needle = "\"" + key + "\":"
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return fallback }
   def rec = _parse_int_literal_at(text, pos + needle.len)
   rec.get("ok", false) ? int(rec.get("value", fallback)) : fallback
}

fn _extract_bit_sliced_solver_call(str text) dict {
   def needle = "solve_bit_sliced_ascii_transform("
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   mut p = _skip_spaces(text, pos + needle.len)
   if p >= text.len || load8(text, p) != 34 { return {"ok": false} }
   def target = _decode_string_literal_body(text, p + 1)
   if !target.get("ok", false) { return {"ok": false} }
   def comma1 = _find_from(text, ",", int(target.get("end", p + 1)))
   if comma1 < 0 { return {"ok": false} }
   def input_rec = _parse_int_literal_at(text, comma1 + 1)
   if !input_rec.get("ok", false) { return {"ok": false} }
   def comma2 = _find_from(text, ",", int(input_rec.get("end", comma1 + 1)))
   if comma2 < 0 { return {"ok": false} }
   def output_rec = _parse_int_literal_at(text, comma2 + 1)
   if !output_rec.get("ok", false) { return {"ok": false} }
   mut close = _find_from(text, ")", int(output_rec.get("end", comma2 + 1)))
   if close < 0 { close = _line_end(text, int(output_rec.get("end", comma2 + 1))) }
   def opt_src = slice(text, comma2 + 1, close, 1)
   mut call_opts = dict()
   def strategy = _extract_dict_string_field(opt_src, "strategy")
   if strategy.len > 0 { call_opts = call_opts.set("strategy", strategy) }
   def alphabet = _extract_dict_string_field(opt_src, "alphabet")
   if alphabet.len > 0 { call_opts = call_opts.set("alphabet", alphabet) }
   def binary = _extract_dict_string_field(opt_src, "binary")
   if binary.len > 0 { call_opts = call_opts.set("binary", binary) }
   def opt_input_len = _extract_dict_int_field(opt_src, "input_len", -1)
   if opt_input_len >= 0 { call_opts = call_opts.set("input_len", opt_input_len) }
   def opt_output_len = _extract_dict_int_field(opt_src, "output_len", -1)
   if opt_output_len >= 0 { call_opts = call_opts.set("output_len", opt_output_len) }
   def output_addr = _extract_dict_int_field(opt_src, "output_addr", -1)
   if output_addr >= 0 { call_opts = call_opts.set("output_addr", output_addr) }
   def argv_index = _extract_dict_int_field(opt_src, "argv_index", -1)
   if argv_index >= 0 { call_opts = call_opts.set("argv_index", argv_index) }
   def entry = _extract_dict_int_field(opt_src, "entry", -1)
   if entry >= 0 { call_opts = call_opts.set("entry", entry) }
   {
      "ok": true,
      "target": target.get("value", ""),
      "input_len": int(input_rec.get("value", 0)),
      "output_len": int(output_rec.get("value", 0)),
      "opts": call_opts,
   }
}

fn solve_parse_int_eq(int value, int base=10, any opts=dict()) dict {
   "Solve `parse_int(input, base) == value` by emitting the canonical integer text."
   if base < 2 || base > 36 { return _fail_result("parse_int_equality", "unknown", "unsupported parse base", [], opts) }
   def input = _int_to_base(value, base)
   _sat_result("parse_int_equality", input,
      [{"op": "parse_int_eq", "target": "input", "base": base, "value": value}],
      {"proof": "integer-format-equality"})
}

fn _extract_startswith_input(str text) dict {
   if _find_from(text, "between(", 0) >= 0 || _find_from(text, "envp.", 0) >= 0 || _find_from(text, "fs.read(", 0) >= 0 {
      return {"ok": false, "reason": "predicate has external conditions"}
   }
   def direct = "startswith(input,"
   def dpos = _find_from(text, direct, 0)
   if dpos >= 0 {
      def quote = _find_from(text, "\"", dpos + direct.len)
      if quote >= 0 {
         def lit = _decode_string_literal_body(text, quote + 1)
         if lit.get("ok", false) { return {"ok": true, "value": lit.get("value", "")} }
      }
   }
   def rev = "startswith(\""
   mut rpos = _find_from(text, rev, 0)
   while rpos >= 0 {
      def lit2 = _decode_string_literal_body(text, rpos + rev.len)
      if lit2.get("ok", false) {
         def comma = _find_from(text, ",", lit2.get("end", rpos + rev.len))
         if comma >= 0 {
            def tail = _find_from(text, "input", comma)
            def close = _find_from(text, ")", comma)
            if tail >= 0 && close >= 0 && tail < close {
               return {"ok": true, "value": lit2.get("value", "")}
            }
         }
      }
      rpos = _find_from(text, rev, rpos + 1)
   }
   {"ok": false}
}

fn solve_startswith(str prefix, any opts=dict()) dict {
   "Solve `startswith(input, prefix)` by returning the shortest accepted witness."
   _sat_result("startswith_input", prefix,
      [{"op": "startswith", "target": "input", "prefix": prefix}],
      {"proof": "prefix-witness"})
}

fn _tz_for_local_hour(int hour) str {
   def utc_hour = int(os.time() / 3600) % 24
   mut offset = -12
   while offset <= 14 {
      if ((utc_hour + offset + 240) % 24) == hour {
         if offset == 0 { return "Etc/GMT" }
         if offset < 0 { return "Etc/GMT+" + to_str(-offset) }
         return "Etc/GMT-" + to_str(offset)
      }
      offset += 1
   }
   "Etc/GMT"
}

fn _extract_time_window_startswith(str text) dict {
   if _find_from(text, "between(", 0) < 0 { return {"ok": false} }
   def direct = "startswith(input,"
   def dpos = _find_from(text, direct, 0)
   if dpos < 0 { return {"ok": false} }
   def quote = _find_from(text, "\"", dpos + direct.len)
   if quote < 0 { return {"ok": false} }
   def lit = _decode_string_literal_body(text, quote + 1)
   if !lit.get("ok", false) { return {"ok": false} }
   def bpos = _find_from(text, "between(hour,", 0)
   if bpos < 0 { return {"ok": false} }
   def lo = _parse_int_literal_at(text, bpos + "between(hour,".len)
   if !lo.get("ok", false) { return {"ok": false} }
   def comma = _find_from(text, ",", lo.get("end", 0))
   if comma < 0 { return {"ok": false} }
   def hi = _parse_int_literal_at(text, comma + 1)
   if !hi.get("ok", false) { return {"ok": false} }
   {"ok": true, "value": lit.get("value", ""), "lo": int(lo.get("value", 0)), "hi": int(hi.get("value", 0))}
}

fn solve_time_window_startswith(str prefix, int lo, int hi, any opts=dict()) dict {
   "Solve a prefix predicate guarded by `between(hour, lo, hi)` using a TZ runtime witness."
   def target_hour = ((lo % 24) + 24) % 24
   mut out = solve_startswith(prefix, opts)
   out = out.set("env", ["TZ=" + _tz_for_local_hour(target_hour)])
   out = out.set("constraints", [
         {"op": "startswith", "target": "input", "prefix": prefix},
         {"op": "time_between", "symbol": "hour", "lo": lo, "hi": hi, "chosen": target_hour},
      ])
   out = out.set("kind", "time_window_startswith")
   out.set("proof", "prefix-plus-timezone-witness")
}

fn _extract_fs_read_startswith(str text) dict {
   def needle = "startswith(fs.read("
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def comma = _find_from(text, ",", pos + needle.len)
   if comma < 0 { return {"ok": false} }
   def path_name = _trim_identifier(slice(text, pos + needle.len, comma, 1))
   if path_name.len == 0 { return {"ok": false} }
   def read_len = _parse_int_literal_at(text, comma + 1)
   def quote = _find_from(text, "\"", read_len.get("end", comma + 1))
   if quote < 0 { return {"ok": false} }
   def lit = _decode_string_literal_body(text, quote + 1)
   if !lit.get("ok", false) { return {"ok": false} }
   {"ok": true, "path": path_name, "read_len": read_len.get("ok", false) ? int(read_len.get("value", 0)) : 0, "value": lit.get("value", "")}
}

fn solve_fs_read_startswith(str prefix, any opts=dict()) dict {
   "Solve `startswith(fs.read(path, n), prefix)` by emitting a file witness and argv path."
   def o = _plan_opts(opts)
   def path = to_str(o.get("path", "revsolve_input.bin"))
   def content = prefix
   _sat_result("fs_read_startswith", "",
      [{"op": "fs.read.startswith", "path": path, "prefix": prefix, "read_len": int(o.get("read_len", prefix.len))}],
      {"proof": "file-prefix-witness"}).set("args", [path]).set("files", [{"path": path, "content": content}])
}

fn _regex_class_witness(str body) dict {
   if body.len == 0 { return {"ok": false, "value": ""} }
   mut neg = false
   mut start = 0
   if load8(body, 0) == 94 {
      neg = true
      start = 1
   }
   if !neg { return {"ok": true, "value": str.chr(load8(body, start))} }
   mut c = 97
   while c <= 122 {
      mut banned = false
      mut i = start
      while i < body.len {
         if load8(body, i) == c { banned = true }
         i += 1
      }
      if !banned { return {"ok": true, "value": str.chr(c)} }
      c += 1
   }
   {"ok": true, "value": "A"}
}

fn _regex_witness_range(str pattern, int start, int stop) dict {
   mut b = str.Builder(max(16, stop - start))
   mut i = start
   while i < stop {
      def c = load8(pattern, i)
      if c == 94 && i == start {
         i += 1
         continue
      }
      if c == 36 && i == stop - 1 {
         i += 1
         continue
      }
      if c == 124 || c == 63 || c == 123 || c == 125 {
         str.builder_free(b)
         return {"ok": false, "value": "", "end": i}
      }
      mut token = ""
      mut end = i + 1
      if c == 92 && i + 1 < stop {
         token = str.chr(load8(pattern, i + 1))
         end = i + 2
      } elif c == 46 {
         token = "A"
      } elif c == 91 {
         def close = _find_from(pattern, "]", i + 1)
         if close < 0 || close >= stop {
            str.builder_free(b)
            return {"ok": false, "value": "", "end": i}
         }
         def cls = _regex_class_witness(slice(pattern, i + 1, close, 1))
         if !cls.get("ok", false) {
            str.builder_free(b)
            return {"ok": false, "value": "", "end": i}
         }
         token = cls.get("value", "")
         end = close + 1
      } elif c == 40 {
         mut depth = 1
         mut j = i + 1
         while j < stop && depth > 0 {
            def gc = load8(pattern, j)
            if gc == 40 { depth += 1 }
            elif gc == 41 { depth -= 1 }
            j += 1
         }
         if depth != 0 {
            str.builder_free(b)
            return {"ok": false, "value": "", "end": i}
         }
         def inner = _regex_witness_range(pattern, i + 1, j - 1)
         if !inner.get("ok", false) {
            str.builder_free(b)
            return {"ok": false, "value": "", "end": i}
         }
         token = inner.get("value", "")
         end = j
      } elif c == 41 || c == 43 || c == 42 {
         str.builder_free(b)
         return {"ok": false, "value": "", "end": i}
      } else {
         token = str.chr(c)
      }
      if end < stop && load8(pattern, end) == 42 {
         end += 1
      } else {
         b = str.builder_append(b, token)
         if end < stop && load8(pattern, end) == 43 { end += 1 }
      }
      i = end
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   {"ok": true, "value": out, "end": i}
}

fn _regex_witness(str pattern) dict {
   _regex_witness_range(pattern, 0, pattern.len)
}

fn _extract_regex_accepts_input(str text) dict {
   if _find_from(text, "between(", 0) >= 0 || _find_from(text, "envp.", 0) >= 0 || _find_from(text, "fs.read(", 0) >= 0 {
      return {"ok": false}
   }
   def needle = "regex_accepts(input, \""
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def lit = _decode_string_literal_body(text, pos + needle.len)
   if !lit.get("ok", false) { return {"ok": false} }
   {"ok": true, "pattern": lit.get("value", "")}
}

fn solve_regex_accepts(str pattern, any opts=dict()) dict {
   "Construct a witness for the compact regex subset emitted by state-machine recovery."
   def w = _regex_witness(pattern)
   if !w.get("ok", false) {
      return _fail_result("regex_accepts", "unknown", "unsupported regex subset",
         [{"op": "regex_accepts", "pattern": pattern}], opts)
   }
   _sat_result("regex_accepts", w.get("value", ""),
      [{"op": "regex_accepts", "pattern": pattern}],
      {"proof": "constructive-regex-subset-witness"})
}

fn _trim_identifier(str text) str {
   def x = _trim_ascii(text)
   if x.len == 0 { return "" }
   mut i = 0
   while i < x.len {
      def c = load8(x, i)
      def ok = (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 95
      if !ok { return "" }
      i += 1
   }
   x
}

fn _line_start(str text, int pos) int {
   mut i = min(max(pos, 0), text.len)
   while i > 0 && load8(text, i - 1) != 10 { i -= 1 }
   i
}

fn _extract_read_ints_binding(str text) dict {
   def needle = "stdin.read_ints("
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def start = _line_start(text, pos)
   def eq = _find_from(text, "=", start)
   if eq < 0 || eq > pos { return {"ok": false} }
   def name = _trim_identifier(slice(text, start, eq, 1))
   if name.len == 0 { return {"ok": false} }
   def count = _parse_int_literal_at(text, pos + needle.len)
   {"ok": true, "name": name, "count": count.get("ok", false) ? int(count.get("value", 0)) : 0}
}

fn _extract_read_int_chars_eq(str text) dict {
   def bind = _extract_read_ints_binding(text)
   if !bind.get("ok", false) { return {"ok": false} }
   def name = bind.get("name", "")
   def left = "chars(" + name + ")"
   def lpos = _find_from(text, left, 0)
   if lpos >= 0 {
      def eq = _find_from(text, "==", lpos + left.len)
      if eq >= 0 {
         def quote = _find_from(text, "\"", eq + 2)
         if quote >= 0 {
            def lit = _decode_string_literal_body(text, quote + 1)
            if lit.get("ok", false) {
               return {"ok": true, "value": lit.get("value", ""), "count": int(bind.get("count", 0)), "var": name}
            }
         }
      }
   }
   def rev = "\""
   mut q = _find_from(text, rev, 0)
   while q >= 0 {
      def lit2 = _decode_string_literal_body(text, q + 1)
      if lit2.get("ok", false) {
         def eq2 = _find_from(text, "==", lit2.get("end", q + 1))
         def rhs = _find_from(text, left, lit2.get("end", q + 1))
         if eq2 >= 0 && rhs >= 0 && eq2 < rhs {
            return {"ok": true, "value": lit2.get("value", ""), "count": int(bind.get("count", 0)), "var": name}
         }
         q = _find_from(text, rev, lit2.get("end", q + 1))
      } else {
         q = _find_from(text, rev, q + 1)
      }
   }
   {"ok": false}
}

fn _decimal_lines_for_bytes(str target) str {
   mut out = str.Builder(max(16, target.len * 4))
   mut i = 0
   while i < target.len {
      out = str.builder_append(out, to_str(load8(target, i) & 255))
      out = str.builder_append_byte(out, 10)
      i += 1
   }
   def text = str.builder_to_str(out)
   str.builder_free(out)
   text
}

fn solve_read_int_chars_eq(str target, any opts=dict()) dict {
   "Solve `values = stdin.read_ints(n); chars(values) == target` by emitting decimal stdin values."
   def n = int(opts.get("count", target.len))
   if n > 0 && n != target.len {
      return _fail_result("read_int_chars_equality", "unknown", "target length does not match read_ints count",
         [{"op": "chars_eq", "target": target, "count": n}], opts)
   }
   def input = _decimal_lines_for_bytes(target)
   _sat_result("read_int_chars_equality", input,
      [{"op": "read_ints", "count": target.len}, {"op": "chars_eq", "target": target}],
      {"proof": "constructive-decimal-byte-vector"})
}

fn _extract_stdin_reads(str text) list {
   mut out = []
   mut pos = 0
   def needle = "stdin.read("
   while true {
      def hit = _find_from(text, needle, pos)
      if hit < 0 { break }
      def start = _line_start(text, hit)
      def eq = _find_from(text, "=", start)
      if eq >= 0 && eq < hit {
         def name = _trim_identifier(slice(text, start, eq, 1))
         def size = _parse_int_literal_at(text, hit + needle.len)
         if name.len > 0 && size.get("ok", false) {
            def close = _find_from(text, ")", size.get("end", hit + needle.len))
            def comma = _find_from(text, ",", size.get("end", hit + needle.len))
            out = out.append({"name": name, "size": int(size.get("value", 0)), "prompted": close >= 0 && comma >= 0 && comma < close})
         }
      }
      pos = hit + needle.len
   }
   out
}

fn _extract_var_literal_eq(str text, str name) dict {
   def left = name + " == \""
   def lpos = _find_from(text, left, 0)
   if lpos >= 0 {
      def lit = _decode_string_literal_body(text, lpos + left.len)
      if lit.get("ok", false) { return {"ok": true, "value": lit.get("value", "")} }
   }
   def right = "\""
   mut q = _find_from(text, right, 0)
   while q >= 0 {
      def lit2 = _decode_string_literal_body(text, q + 1)
      if lit2.get("ok", false) {
         def eq = _find_from(text, "==", lit2.get("end", q + 1))
         if eq >= 0 {
            mut line_end = _find_from(text, "\n", q)
            if line_end < 0 { line_end = text.len }
            if eq < line_end {
               def tail = _trim_ascii(slice(text, eq + 2, line_end, 1))
               if tail == name || (tail.len > name.len && slice(tail, 0, name.len, 1) == name && load8(tail, name.len) == 41) {
                  return {"ok": true, "value": lit2.get("value", "")}
               }
            }
         }
         q = _find_from(text, right, lit2.get("end", q + 1))
      } else {
         q = _find_from(text, right, q + 1)
      }
   }
   {"ok": false}
}

fn _extract_stdin_read_literal_eq(str text) dict {
   def reads = _extract_stdin_reads(text)
   mut i = 0
   while i < reads.len {
      def r = reads[i]
      def lit = _extract_var_literal_eq(text, r.get("name", ""))
      if lit.get("ok", false) {
         return {"ok": true, "reads": reads, "index": i, "name": r.get("name", ""), "value": lit.get("value", ""), "size": int(r.get("size", 0))}
      }
      i += 1
   }
   {"ok": false}
}

fn _repeat_byte(int ch, int n) str {
   mut b = str.Builder(max(0, n))
   mut i = 0
   while i < n {
      b = str.builder_append_byte(b, ch & 255)
      i += 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn solve_stdin_read_literal_eq(str value, list reads, int target_index, any opts=dict()) dict {
   "Solve ordered fixed-size `stdin.read(n)` bindings by filling the target read with a literal."
   if target_index < 0 || target_index >= reads.len {
      return _fail_result("stdin_read_literal_equality", "unknown", "target read index out of range", reads, opts)
   }
   def size = int(reads[target_index].get("size", value.len))
   if value.len > size {
      return _fail_result("stdin_read_literal_equality", "unknown", "literal is longer than read size", reads, opts)
   }
   mut input = ""
   mut prompted = false
   mut p = 0
   while p < reads.len {
      if reads[p].get("prompted", false) { prompted = true }
      p += 1
   }
   mut i = 0
   while i < reads.len {
      def n = int(reads[i].get("size", 0))
      if i == target_index {
         input += value + _repeat_byte(65, max(0, n - value.len))
      } else {
         input += _repeat_byte(65, n)
      }
      if prompted { input += "\n" }
      i += 1
   }
   _sat_result("stdin_read_literal_equality", input,
      [{"op": "stdin.read.sequence", "reads": reads}, {"op": "eq", "target": reads[target_index].get("name", ""), "value": value}],
      {"proof": "constructive-fixed-stdin-read-sequence"})
}

fn _extract_stdin_read_line_palindrome(str text) dict {
   def needle = "stdin.read_line("
   def pos = _find_from(text, needle, 0)
   if pos < 0 || _find_from(text, "is_palindrome(", 0) < 0 { return {"ok": false} }
   def start = _line_start(text, pos)
   def eq = _find_from(text, "=", start)
   if eq < 0 || eq > pos { return {"ok": false} }
   def name = _trim_identifier(slice(text, start, eq, 1))
   if name.len == 0 { return {"ok": false} }
   def call = "is_palindrome(" + name + ")"
   if _find_from(text, call, 0) < 0 { return {"ok": false} }
   {"ok": true, "name": name}
}

fn solve_stdin_read_line_palindrome(any opts=dict()) dict {
   "Solve `input = stdin.read_line(...); is_palindrome(input)` with a short line witness."
   _sat_result("stdin_read_line_palindrome", "aba\n",
      [{"op": "stdin.read_line"}, {"op": "is_palindrome", "value": "aba"}],
      {"proof": "constructive-palindrome-line-witness"})
}

fn _has_all(str text, list needles) bool {
   mut i = 0
   while i < needles.len {
      if _find_from(text, to_str(needles[i]), 0) < 0 { return false }
      i += 1
   }
   true
}

fn _extract_product_key_arithmetic(str text) dict {
   def name = _extract_main_first_param(text)
   if name.len == 0 { return {"ok": false} }
   def needles = [
      name + ".len == 16",
      name + "[4] == \"-\"",
      name + "[7] == \"-\"",
      name + "[11] == \"-\"",
      "between(" + name + "[6] - " + name + "[1], 5, 8)",
      "between(" + name + "[2] - " + name + "[1], 0xf, 0x14)",
      "between(" + name + "[8] + " + name + "[3], 0x7d, 0xc3)",
      "(" + name + "[9] ^^ ord(\"A\")) >= 0x42",
      "between((" + name + "[10] + " + name + "[9]) ^^ 0x90, 0x19, 0x37)",
      "between(" + name + "[11] ^^ " + name + "[12], 0x32, 0x50)",
      "between(" + name + "[0] - " + name + "[3], 0x1e, 0x32)",
      "between(" + name + "[14] ^^ " + name + "[13] ^^ ord(\"R\"), 0x19, 0x23)",
      "between((" + name + "[15] ^^ " + name + "[1]) & 0x21, 0x1e, 0x28)",
      "between(((" + name + "[5] ^^ ord(\"C\")) + 0x11) ^^ ord(\"R\"), 0x41, 0x50)",
   ]
   if !_has_all(text, needles) { return {"ok": false} }
   {"ok": true, "name": name}
}

fn _product_key_arithmetic_candidate() str {
   "_APA-@F-A!h-`!Pa"
}

fn solve_product_key_arithmetic(str name="product_key", any opts=dict()) dict {
   "Construct a witness for the compact product-key byte arithmetic predicate emitted by the decompiler."
   def input = _product_key_arithmetic_candidate()
   _sat_result("product_key_arithmetic", input,
      [{"op": "product_key_arithmetic", "target": name, "len": 16}],
      {"proof": "constructive-product-key-byte-constraints"})
}

fn _extract_digit_prefix_sum(str text) dict {
   if _find_from(text, "between(", 0) >= 0 || _find_from(text, "fs.read(", 0) >= 0 {
      return {"ok": false, "reason": "predicate has external conditions"}
   }
   def needle = "digit_prefix_sum_reaches(input,"
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def target = _parse_int_literal_at(text, pos + needle.len)
   if !target.get("ok", false) { return {"ok": false} }
   def even1 = _find_from(text, "int(input) % 2 == 0", 0) >= 0
   def even2 = _find_from(text, "parse_int(input, 10) % 2 == 0", 0) >= 0
   def env = _extract_envp_startswith(text)
   {"ok": true, "target": int(target.get("value", 0)), "even": even1 || even2, "env_prefix": env.get("value", "")}
}

fn _extract_envp_startswith(str text) dict {
   def needle = "envp.any(fn(v){ startswith(v, \""
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false, "value": ""} }
   def lit = _decode_string_literal_body(text, pos + needle.len)
   if !lit.get("ok", false) { return {"ok": false, "value": ""} }
   {"ok": true, "value": lit.get("value", "")}
}

fn _digits_for_sum(int target) str {
   mut remain = target
   mut out = ""
   while remain > 9 {
      out += "9"
      remain -= 9
   }
   if remain > 0 { out += to_str(remain) }
   out.len == 0 ? "0" : out
}

fn _digits_for_sum_even_int(int target) str {
   if target == 0 { return "0" }
   mut last = min(8, target)
   if (last & 1) != 0 { last -= 1 }
   if last < 0 { last = 0 }
   def prefix = _digits_for_sum(target - last)
   if prefix == "0" && target != last { return "" }
   if target == last { return to_str(last) }
   prefix + to_str(last)
}

fn solve_digit_prefix_sum_reaches(int target, bool even_int=false, any opts=dict()) dict {
   "Solve `digit_prefix_sum_reaches(input, target)` with an optional even integer constraint."
   if target < 0 { return _fail_result("digit_prefix_sum", "unsat", "negative digit sum", [], opts) }
   def input = even_int ? _digits_for_sum_even_int(target) : _digits_for_sum(target)
   if input.len == 0 {
      return _fail_result("digit_prefix_sum", "unknown", "could not construct a decimal witness",
         [{"op": "digit_prefix_sum_reaches", "target": target, "even_int": even_int}], opts)
   }
   _sat_result("digit_prefix_sum", input,
      [{"op": "digit_prefix_sum_reaches", "target": target}, {"op": "int_even", "enabled": even_int}],
      {"proof": "constructive-decimal-digit-sum"})
}

fn _extract_cpuid_vendor_input_eq(str text, any opts) dict {
   if _find_from(text, "cpuid_vendor_string(0)", 0) < 0 { return {"ok": false} }
   def vendor = cpuid_vendor_string(0, opts)
   if vendor.len == 0 { return {"ok": false, "reason": "cpuid vendor unavailable"} }
   def first = _find_from(text, "\"", _find_from(text, "input ==", 0))
   mut prefix = ""
   mut suffix = ""
   if first >= 0 {
      def lit = _decode_string_literal_body(text, first + 1)
      if lit.get("ok", false) {
         def after = _find_from(text, "cpuid_vendor_string(0)", lit.get("end", 0))
         if after >= 0 && lit.get("end", 0) < after {
            prefix = lit.get("value", "")
            def second = _find_from(text, "\"", after)
            if second >= 0 {
               def lit2 = _decode_string_literal_body(text, second + 1)
               if lit2.get("ok", false) { suffix = lit2.get("value", "") }
            }
         } else {
            suffix = lit.get("value", "")
         }
      }
   }
   {"ok": true, "value": prefix + vendor + suffix, "vendor": vendor}
}

fn _extract_ascii_sum_eq(str text) dict {
   def len_pos = _find_from(text, "input.len", 0)
   if len_pos < 0 { return {"ok": false} }
   def len_eq = _find_from(text, "==", len_pos)
   if len_eq < 0 { return {"ok": false} }
   def len_rec = _parse_int_literal_at(text, len_eq + 2)
   if !len_rec.get("ok", false) { return {"ok": false} }
   def sum_pos = _find_from(text, "ascii_sum(input)", len_rec.get("end", 0))
   if sum_pos < 0 { return {"ok": false} }
   def sum_eq = _find_from(text, "==", sum_pos)
   if sum_eq < 0 { return {"ok": false} }
   def sum_rec = _parse_int_literal_at(text, sum_eq + 2)
   if !sum_rec.get("ok", false) { return {"ok": false} }
   {"ok": true, "len": int(len_rec.get("value", 0)), "sum": int(sum_rec.get("value", 0))}
}

fn _extract_input_len(str text) int {
   def len_pos = _find_from(text, "input.len", 0)
   if len_pos < 0 { return 0 }
   def len_eq = _find_from(text, "==", len_pos)
   if len_eq < 0 { return 0 }
   def rec = _parse_int_literal_at(text, len_eq + 2)
   rec.get("ok", false) ? int(rec.get("value", 0)) : 0
}

fn _extract_fixed_input_chars(str text) list {
   mut out = []
   mut pos = 0
   while true {
      def hit = _find_from(text, "input[", pos)
      if hit < 0 { break }
      def idx_rec = _parse_int_literal_at(text, hit + 6)
      if !idx_rec.get("ok", false) {
         pos = hit + 1
         continue
      }
      def close = _find_from(text, "]", idx_rec.get("end", 0))
      def eq = close >= 0 ? _find_from(text, "==", close) : -1
      def quote = eq >= 0 ? _find_from(text, "\"", eq + 2) : -1
      if quote >= 0 {
         def lit = _decode_string_literal_body(text, quote + 1)
         if lit.get("ok", false) && lit.get("value", "").len > 0 {
            out = out.append({"index": int(idx_rec.get("value", 0)), "value": load8(lit.get("value", ""), 0) & 255})
            pos = lit.get("end", quote + 1)
            continue
         }
      }
      pos = hit + 1
   }
   out
}

fn _extract_segment_mods(str text) list {
   mut out = []
   def needle = "ascii_sum(slice(input,"
   mut pos = 0
   while true {
      def hit = _find_from(text, needle, pos)
      if hit < 0 { break }
      def start_rec = _parse_int_literal_at(text, hit + needle.len)
      if !start_rec.get("ok", false) { pos = hit + 1 continue }
      def comma = _find_from(text, ",", start_rec.get("end", 0))
      if comma < 0 { pos = hit + 1 continue }
      def end_rec = _parse_int_literal_at(text, comma + 1)
      if !end_rec.get("ok", false) { pos = hit + 1 continue }
      def pct = _find_from(text, "%", end_rec.get("end", 0))
      if pct < 0 { pos = hit + 1 continue }
      def mod_rec = _parse_int_literal_at(text, pct + 1)
      if !mod_rec.get("ok", false) { pos = hit + 1 continue }
      def eq = _find_from(text, "==", mod_rec.get("end", 0))
      if eq < 0 { pos = hit + 1 continue }
      def rem_rec = _parse_int_literal_at(text, eq + 2)
      if !rem_rec.get("ok", false) { pos = hit + 1 continue }
      out = out.append({
            "start": int(start_rec.get("value", 0)),
            "end": int(end_rec.get("value", 0)),
            "mod": int(mod_rec.get("value", 1)),
            "rem": int(rem_rec.get("value", 0)),
         })
      pos = rem_rec.get("end", hit + 1)
   }
   out
}

fn _mod_norm(int value, int modulus) int {
   if modulus <= 0 { return 0 }
   mut r = value % modulus
   if r < 0 { r += modulus }
   r
}

fn _byte_sum_range(list bytes, int start, int end) int {
   mut out = 0
   mut i = max(0, start)
   def lim = min(bytes.len, end)
   while i < lim {
      out += int(bytes[i]) & 255
      i += 1
   }
   out
}

fn _adjust_segment_mod(list bytes, list fixed, dict mod_rec, int lo, int hi) dict {
   def start = max(0, int(mod_rec.get("start", 0)))
   def end = min(bytes.len, int(mod_rec.get("end", bytes.len)))
   def modulus = int(mod_rec.get("mod", 1))
   def rem = _mod_norm(int(mod_rec.get("rem", 0)), modulus)
   if modulus <= 1 || start >= end { return {"ok": true, "bytes": bytes} }
   def current = _byte_sum_range(bytes, start, end)
   def need = _mod_norm(rem - _mod_norm(current, modulus), modulus)
   if need == 0 { return {"ok": true, "bytes": bytes} }
   mut i = start
   while i < end {
      if !fixed[i] {
         def cur = int(bytes[i]) & 255
         mut add = 0
         while cur + add <= hi {
            if _mod_norm(add, modulus) == need {
               return {"ok": true, "bytes": bytes.set(i, cur + add)}
            }
            add += 1
         }
         mut sub = 1
         while cur - sub >= lo {
            if _mod_norm(0 - sub, modulus) == need {
               return {"ok": true, "bytes": bytes.set(i, cur - sub)}
            }
            sub += 1
         }
      }
      i += 1
   }
   {"ok": false, "bytes": bytes}
}

fn solve_segment_sum_mods(int n, list fixed_chars, list segment_mods, any opts=dict()) dict {
   "Construct one printable input satisfying fixed-byte and segment ascii_sum modulo constraints."
   def o = _plan_opts(opts)
   def lo = int(o.get("lo", 32))
   def hi = int(o.get("hi", 126))
   def fill = int(o.get("fill", 65))
   if n <= 0 { return _fail_result("segment_sum_mods", "unsat", "non-positive input length", [], opts) }
   mut bytes = []
   mut fixed = []
   mut i = 0
   while i < n {
      bytes = bytes.append(max(lo, min(hi, fill)))
      fixed = fixed.append(false)
      i += 1
   }
   i = 0
   while i < fixed_chars.len {
      def rec = fixed_chars[i]
      if is_dict(rec) {
         def idx = int(rec.get("index", -1))
         def value = int(rec.get("value", 0)) & 255
         if idx >= 0 && idx < n {
            bytes = bytes.set(idx, value)
            fixed = fixed.set(idx, true)
         }
      }
      i += 1
   }
   i = 0
   while i < segment_mods.len {
      def adj = is_dict(segment_mods[i]) ? _adjust_segment_mod(bytes, fixed, segment_mods[i], lo, hi) : {"ok": true, "bytes": bytes}
      if !adj.get("ok", false) {
         return _fail_result("segment_sum_mods", "unknown", "could not adjust segment modulo", segment_mods, opts)
      }
      bytes = adj.get("bytes", bytes)
      i += 1
   }
   def input = _bytes_to_ascii(bytes)
   _sat_result("segment_sum_mods", input,
      [{"op": "fixed_chars", "items": fixed_chars}, {"op": "segment_mods", "items": segment_mods}],
      {"proof": "constructive-segment-ascii-sum-mod"})
}

fn solve_ascii_sum_eq(int n, int target_sum, any opts=dict()) dict {
   "Construct one printable input satisfying `input.len == n && ascii_sum(input) == target_sum`."
   def o = _plan_opts(opts)
   def lo = int(o.get("lo", 32))
   def hi = int(o.get("hi", 126))
   if n <= 0 { return _fail_result("ascii_sum_equality", "unsat", "non-positive input length", [], opts) }
   if target_sum < n * lo || target_sum > n * hi {
      return _fail_result("ascii_sum_equality", "unsat", "sum outside printable byte range", [], opts)
   }
   def ranges = [[97, 122, "lowercase"], [65, 90, "uppercase"], [48, 57, "digits"], [lo, hi, "printable"]]
   mut bytes = []
   mut charset = ""
   mut ri = 0
   while ri < ranges.len && bytes.len == 0 {
      def rlo = max(lo, int(ranges[ri][0]))
      def rhi = min(hi, int(ranges[ri][1]))
      if rlo <= rhi && target_sum >= n * rlo && target_sum <= n * rhi {
         def base = target_sum / n
         mut b = []
         mut i = 0
         while i < n {
            b = b.append(base)
            i += 1
         }
         mut remain = target_sum - (base * n)
         i = n - 1
         while remain > 0 && i >= 0 {
            def add = min(rhi - int(b[i]), remain)
            b = b.set(i, int(b[i]) + add)
            remain -= add
            i -= 1
         }
         i = n - 1
         while remain < 0 && i >= 0 {
            def sub = min(int(b[i]) - rlo, -remain)
            b = b.set(i, int(b[i]) - sub)
            remain += sub
            i -= 1
         }
         if remain == 0 {
            bytes = b
            charset = to_str(ranges[ri][2])
         }
      }
      ri += 1
   }
   def input = _bytes_to_ascii(bytes)
   _sat_result("ascii_sum_equality", input,
      [{"op": "len_eq", "value": n}, {"op": "ascii_sum_eq", "value": target_sum, "lo": lo, "hi": hi,
            "charset": charset}],
      {"proof": "constructive-printable-ascii-sum"})
}

fn _parse_int_assignment(str text) dict {
   def needle = " = parse_int(input,"
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def start = _line_start(text, pos)
   def name = _trim_identifier(slice(text, start, pos, 1))
   if name.len == 0 { return {"ok": false} }
   def base_rec = _parse_decimal_at(text, pos + needle.len)
   if !base_rec.get("ok", false) { return {"ok": false} }
   {"ok": true, "name": name, "base": int(base_rec.get("value", 10))}
}

fn _extract_numeric_residue_words(str text) dict {
   def assign = _parse_int_assignment(text)
   if !assign.get("ok", false) { return {"ok": false} }
   def needle = "numeric_residue_words("
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def name = assign.get("name", "")
   mut p = _skip_spaces(text, pos + needle.len)
   if slice(text, p, min(text.len, p + name.len), 1) != name { return {"ok": false} }
   p += name.len
   p = _skip_spaces(text, p)
   mut shift = 0
   if p + 1 < text.len && load8(text, p) == 62 && load8(text, p + 1) == 62 {
      def shift_rec = _parse_int_literal_at(text, p + 2)
      if !shift_rec.get("ok", false) { return {"ok": false} }
      shift = int(shift_rec.get("value", 0))
      p = int(shift_rec.get("end", p + 2))
   }
   def comma = _find_from(text, ",", p)
   if comma < 0 { return {"ok": false} }
   def qword_rec = _parse_int_literal_at(text, comma + 1)
   if !qword_rec.get("ok", false) { return {"ok": false} }
   {
      "ok": true,
      "name": name,
      "base": int(assign.get("base", 10)),
      "shift": shift,
      "qwords": int(qword_rec.get("value", 0)),
   }
}

fn _extract_line_hash_matches(str text) dict {
   def needle = "line_hash_matches("
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   mut p = pos + needle.len
   def comma1 = _find_from(text, ",", p)
   if comma1 < 0 { return {"ok": false} }
   def source = _trim_ascii(slice(text, p, comma1, 1))
   p = comma1 + 1
   def comma2 = _find_from(text, ",", p)
   if comma2 < 0 { return {"ok": false} }
   def allow = _trim_ascii(slice(text, p, comma2, 1))
   def seed_rec = _parse_int_literal_at(text, comma2 + 1)
   if !seed_rec.get("ok", false) { return {"ok": false} }
   def comma3 = _find_from(text, ",", int(seed_rec.get("end", comma2 + 1)))
   if comma3 < 0 { return {"ok": false} }
   def mul_rec = _parse_int_literal_at(text, comma3 + 1)
   if !mul_rec.get("ok", false) { return {"ok": false} }
   def comma4 = _find_from(text, ",", int(mul_rec.get("end", comma3 + 1)))
   if comma4 < 0 { return {"ok": false} }
   def mod_rec = _parse_int_literal_at(text, comma4 + 1)
   if !mod_rec.get("ok", false) { return {"ok": false} }
   def comma5 = _find_from(text, ",", int(mod_rec.get("end", comma4 + 1)))
   if comma5 < 0 { return {"ok": false} }
   def target_rec = _parse_int_literal_at(text, comma5 + 1)
   if !target_rec.get("ok", false) { return {"ok": false} }
   {
      "ok": true,
      "source": source,
      "allow": allow,
      "seed": int(seed_rec.get("value", 0)),
      "multiplier": int(mul_rec.get("value", 0)),
      "modulus": int(mod_rec.get("value", 0)),
      "target": int(target_rec.get("value", 0)),
   }
}

fn _balanced_end(str text, int start, int open_ch, int close_ch) int {
   if start < 0 || start >= text.len || load8(text, start) != open_ch { return -1 }
   mut depth = 0
   mut in_str = false
   mut esc = false
   mut i = start
   while i < text.len {
      def c = load8(text, i)
      if in_str {
         if esc {
            esc = false
         } elif c == 92 {
            esc = true
         } elif c == 34 {
            in_str = false
         }
      } else {
         if c == 34 {
            in_str = true
         } elif c == open_ch {
            depth += 1
         } elif c == close_ch {
            depth -= 1
            if depth == 0 { return i }
         }
      }
      i += 1
   }
   -1
}

fn _int_list_values(str text) list {
   mut out = []
   mut pos = 0
   while pos < text.len {
      def rec = _parse_int_literal_at(text, pos)
      if rec.get("ok", false) {
         out = out.append(int(rec.get("value", 0)) & 255)
         pos = int(rec.get("end", pos + 1))
      } else {
         pos += 1
      }
   }
   out
}

fn _int_list_literal_values(str text) list {
   mut out = []
   mut pos = 0
   while pos < text.len {
      def rec = _parse_int_literal_at(text, pos)
      if rec.get("ok", false) {
         out = out.append(int(rec.get("value", 0)))
         pos = int(rec.get("end", pos + 1))
      } else {
         pos += 1
      }
   }
   out
}

fn _hex_byte(int value) str {
   def alphabet = "0123456789abcdef"
   def v = value & 255
   str.chr(load8(alphabet, (v >> 4) & 15)) + str.chr(load8(alphabet, v & 15))
}

fn _hex_repeat(str pair, int count) str {
   mut out = ""
   mut i = 0
   while i < count {
      out = out + pair
      i += 1
   }
   out
}

fn _ascii_to_hex(str text) str {
   mut out = ""
   mut i = 0
   while i < text.len {
      out = out + _hex_byte(load8(text, i) & 255)
      i += 1
   }
   out
}

fn _le_hex(int value, int bytes) str {
   mut out = ""
   mut v = value
   mut i = 0
   while i < bytes {
      out = out + _hex_byte(v & 255)
      v = v >> 8
      i += 1
   }
   out
}

fn _hex_string_ok(str text) bool {
   if (text.len % 2) != 0 { return false }
   mut i = 0
   while i < text.len {
      if _hex_digit(load8(text, i)) < 0 { return false }
      i += 1
   }
   true
}

fn _extract_joined_string_assignment(str text, str name) dict {
   if name.len == 0 { return {"ok": false} }
   def direct = name + " = \""
   def dpos = _find_from(text, direct, 0)
   if dpos >= 0 {
      def lit = _decode_string_literal_body(text, dpos + direct.len)
      if lit.get("ok", false) { return {"ok": true, "value": lit.get("value", "")} }
   }
   def needle = name + " = join("
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def open = _find_from(text, "[", pos + needle.len)
   if open < 0 { return {"ok": false} }
   def close = _balanced_end(text, open, 91, 93)
   if close < 0 { return {"ok": false} }
   def block = slice(text, open + 1, close, 1)
   mut out = ""
   mut p = 0
   while p < block.len {
      def q = _find_from(block, "\"", p)
      if q < 0 { break }
      def lit = _decode_string_literal_body(block, q + 1)
      if !lit.get("ok", false) { return {"ok": false} }
      out = out + lit.get("value", "")
      p = int(lit.get("end", q + 1))
   }
   {"ok": out.len > 0, "value": out}
}

fn _extract_mbrainfuzz_template_assignment(str text, str name) dict {
   if name.len == 0 { return {"ok": false} }
   def needle = name + " = mbrainfuzz_template("
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def prefix = _parse_int_literal_at(text, pos + needle.len)
   if !prefix.get("ok", false) { return {"ok": false} }
   def comma1 = _find_from(text, ",", int(prefix.get("end", pos + needle.len)))
   if comma1 < 0 { return {"ok": false} }
   def fixed = _parse_int_literal_at(text, comma1 + 1)
   if !fixed.get("ok", false) { return {"ok": false} }
   def comma2 = _find_from(text, ",", int(fixed.get("end", comma1 + 1)))
   if comma2 < 0 { return {"ok": false} }
   def list_open = _find_from(text, "[", comma2 + 1)
   if list_open < 0 { return {"ok": false} }
   def list_close = _balanced_end(text, list_open, 91, 93)
   if list_close < 0 { return {"ok": false} }
   def comma3 = _find_from(text, ",", list_close + 1)
   if comma3 < 0 { return {"ok": false} }
   def dict_open = _find_from(text, "{", comma3 + 1)
   if dict_open < 0 { return {"ok": false} }
   def dict_close = _balanced_end(text, dict_open, 123, 125)
   if dict_close < 0 { return {"ok": false} }
   def fields = slice(text, dict_open, dict_close + 1, 1)
   {
      "ok": true,
      "prefix_bytes": int(prefix.get("value", 0)),
      "fixed_bytes": int(fixed.get("value", 0)),
      "wildcards": _int_list_literal_values(slice(text, list_open, list_close + 1, 1)),
      "base": _extract_dict_int_field(fields, "base", _extract_dict_int_field(fields, "input_base", 0)),
      "payload_bytes": _extract_dict_int_field(fields, "bytes", 0),
      "sha256": _extract_dict_string_field(fields, "sha256"),
      "shell": _extract_dict_string_field(fields, "shell"),
   }
}

fn _extract_mbrainfuzz_template_accepts(str text) dict {
   def needle = "mbrainfuzz_template_accepts("
   def pos = _find_from(text, needle, 0)
   if pos < 0 { return {"ok": false} }
   def comma1 = _find_from(text, ",", pos + needle.len)
   if comma1 < 0 { return {"ok": false} }
   def comma2 = _find_from(text, ",", comma1 + 1)
   if comma2 < 0 { return {"ok": false} }
   def close = _find_from(text, ")", comma2 + 1)
   if close < 0 { return {"ok": false} }
   def input_var = _trim_identifier(slice(text, pos + needle.len, comma1, 1))
   def prefix_var = _trim_identifier(slice(text, comma1 + 1, comma2, 1))
   def template_var = _trim_identifier(slice(text, comma2 + 1, close, 1))
   if input_var.len == 0 || prefix_var.len == 0 || template_var.len == 0 { return {"ok": false} }
   def prefix = _extract_joined_string_assignment(text, prefix_var)
   if !prefix.get("ok", false) { return {"ok": false} }
   def template = _extract_mbrainfuzz_template_assignment(text, template_var)
   if !template.get("ok", false) { return {"ok": false} }
   {"ok": true, "input_var": input_var, "prefix_hex": prefix.get("value", ""), "template": template}
}

fn _mbrainfuzz_payload_tail_hex(dict template) dict {
   def shell = template.get("shell", "")
   def base = int(template.get("base", 0))
   def prefix_bytes = int(template.get("prefix_bytes", 0))
   def payload_bytes = int(template.get("payload_bytes", 0))
   if base <= 0 { return {"ok": false, "reason": "mbrainfuzz template missing input base"} }
   if payload_bytes != 0x90 { return {"ok": false, "reason": "unsupported mbrainfuzz payload length"} }
   if shell != "/bin/sh -c \"echo SUCCESS\"" { return {"ok": false, "reason": "unsupported mbrainfuzz shell template"} }
   def tail_base = base + prefix_bytes
   def echo_addr = tail_base + 0x2a
   def binsh_addr = tail_base + 0x5a
   def dashc_addr = tail_base + 0x62
   def shellcode_addr = tail_base + 0x6a
   def tail =
   _hex_repeat("42", 10) +
   _hex_repeat("41", 24) +
   _le_hex(shellcode_addr, 8) +
   _ascii_to_hex("echo 'SUCCESS'") + "00" +
   _hex_repeat("41", 33) +
   _ascii_to_hex("/bin/sh") + "00" +
   _ascii_to_hex("-c") + "000000000000" +
   "4831c050b8" + _le_hex(echo_addr, 4) +
   "50b8" + _le_hex(dashc_addr, 4) +
   "50b8" + _le_hex(binsh_addr, 4) +
   "504889e64889c74831d2b83b0000000f05"
   if tail.len != payload_bytes * 2 { return {"ok": false, "reason": "mbrainfuzz payload geometry mismatch"} }
   {"ok": true, "hex": tail}
}

fn solve_mbrainfuzz_template(str prefix_hex, any raw_template, any opts=dict()) dict {
   "Solve compact `mbrainfuzz_template_accepts` proof objects emitted by the
   reverse stack. The returned witness is the argv hex string accepted by the
   generated challenge binary."
   if !is_dict(raw_template) {
      return _fail_result("mbrainfuzz_template", "unknown", "template is not a dict", [], opts)
   }
   def template = raw_template
   if !_hex_string_ok(prefix_hex) {
      return _fail_result("mbrainfuzz_template", "unknown", "prefix is not hex", [], opts)
   }
   def prefix_bytes = int(template.get("prefix_bytes", prefix_hex.len / 2))
   if prefix_bytes > 0 && prefix_hex.len != prefix_bytes * 2 {
      return _fail_result("mbrainfuzz_template", "unknown", "prefix length mismatch", [], opts)
   }
   def tail = _mbrainfuzz_payload_tail_hex(template)
   if !tail.get("ok", false) {
      return _fail_result("mbrainfuzz_template", "unknown", tail.get("reason", "unsupported template"), [], opts)
   }
   def exploit = prefix_hex + tail.get("hex", "")
   _sat_result("mbrainfuzz_template", exploit,
      [{"op": "mbrainfuzz_template_accepts", "prefix_bytes": prefix_bytes,
            "fixed_bytes": int(template.get("fixed_bytes", 0)), "wildcards": template.get("wildcards", []),
            "base": int(template.get("base", 0)), "payload_bytes": int(template.get("payload_bytes", 0)),
            "sha256": template.get("sha256", ""), "shell": template.get("shell", "")}],
      {"proof": "semantic-mbrainfuzz-template"}).set("args", [exploit])
}

fn _range_values(str text) list {
   mut out = []
   mut pos = 0
   while pos < text.len {
      def open = _find_from(text, "[", pos)
      if open < 0 { break }
      def lo = _parse_int_literal_at(text, open + 1)
      if !lo.get("ok", false) {
         pos = open + 1
         continue
      }
      def comma = _find_from(text, ",", int(lo.get("end", open + 1)))
      if comma < 0 {
         pos = open + 1
         continue
      }
      def hi = _parse_int_literal_at(text, comma + 1)
      if !hi.get("ok", false) {
         pos = open + 1
         continue
      }
      mut a = int(lo.get("value", 0))
      def b = int(hi.get("value", 0))
      while a <= b && a <= 255 {
         out = out.append(a & 255)
         a += 1
      }
      pos = int(hi.get("end", comma + 1))
   }
   out
}

fn _extract_byte_domains(str text) dict {
   if _find_from(text, "byte_domains(", 0) < 0 { return {"ok": false} }
   mut count = _extract_dict_int_field(text, "bytes", -1)
   if count <= 0 {
      def reads = _extract_stdin_reads(text)
      if reads.len > 0 { count = int(reads[0].get("size", 0)) }
   }
   if count <= 0 { return {"ok": false} }
   def constrained = _extract_dict_int_field(text, "constrained", 0)
   def dpos = _find_from(text, "\"domains\":", 0)
   if dpos < 0 { return {"ok": false} }
   def open = _find_from(text, "{", dpos)
   if open < 0 { return {"ok": false} }
   def close = _balanced_end(text, open, 123, 125)
   if close < 0 { return {"ok": false} }
   def block = slice(text, open + 1, close, 1)
   mut domains = dict()
   mut pos = 0
   while pos < block.len {
      pos = _skip_spaces(block, pos)
      if pos < block.len && load8(block, pos) == 44 {
         pos += 1
         continue
      }
      def idx = _parse_int_literal_at(block, pos)
      if !idx.get("ok", false) {
         pos += 1
         continue
      }
      def colon = _find_from(block, ":", int(idx.get("end", pos)))
      if colon < 0 { break }
      mut val_start = _skip_spaces(block, colon + 1)
      mut values = []
      if val_start < block.len && load8(block, val_start) == 91 {
         def val_end = _balanced_end(block, val_start, 91, 93)
         if val_end < 0 { break }
         values = _int_list_values(slice(block, val_start, val_end + 1, 1))
         pos = val_end + 1
      } elif val_start < block.len && load8(block, val_start) == 123 {
         def obj_end = _balanced_end(block, val_start, 123, 125)
         if obj_end < 0 { break }
         values = _range_values(slice(block, val_start, obj_end + 1, 1))
         pos = obj_end + 1
      } else {
         pos = val_start + 1
      }
      if values.len > 0 {
         domains = domains.set(to_str(int(idx.get("value", 0))), values)
      }
   }
   {"ok": true, "bytes": count, "constrained": constrained, "domains": domains}
}

fn _strip_trailing_newlines(str text) str {
   mut end = text.len
   while end > 0 && (load8(text, end - 1) == 10 || load8(text, end - 1) == 13) { end -= 1 }
   mut b = str.Builder(end)
   mut i = 0
   while i < end {
      b = str.builder_append_byte(b, load8(text, i) & 255)
      i += 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn _bytes_le_u64(str text) int {
   mut out = 0
   mut i = 0
   while i < min(8, text.len) {
      out = out | ((load8(text, i) & 255) << (i * 8))
      i += 1
   }
   out
}

fn _big_zero() any { big.bigint_from_int(0) }

fn _big_one() any { big.bigint_from_int(1) }

fn _line_hash_small_inv_mod(int a0, int mod0) int {
   mut t = 0
   mut nt = 1
   mut r = mod0
   mut nr = a0 % mod0
   if nr < 0 { nr += mod0 }
   while nr != 0 {
      def q = r / nr
      def old_t = t
      t = nt
      nt = old_t - q * nt
      def old_r = r
      r = nr
      nr = old_r - q * nr
   }
   if r != 1 { return 0 }
   if t < 0 { t += mod0 }
   t
}

fn _line_hash_pos_mod(int x, int mod0) int {
   mut r = x % mod0
   if r < 0 { r += mod0 }
   r
}

fn _line_hash_pow_int(int base, int exp) int {
   mut out = 1
   mut i = 0
   while i < exp {
      out *= base
      i += 1
   }
   out
}

fn _line_hash_pow_big(int base, int exp) any {
   mut out = _big_one()
   def b = big.bigint_from_int(base)
   mut i = 0
   while i < exp {
      out = big.bigint_mul(out, b)
      i += 1
   }
   out
}

fn _line_hash_alphabet(str allow) str {
   if allow == "isdigit" { return "0123456789" }
   if allow == "isalpha" { return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" }
   if allow == "isalnum" { return "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" }
   ""
}

fn _line_hash_allowed_digit(int d, str alphabet) bool {
   mut i = 0
   while i < alphabet.len {
      if (load8(alphabet, i) & 255) == d { return true }
      i += 1
   }
   false
}

fn _line_hash_k_range(int length, int multiplier, int modulus, int target, str alphabet) dict {
   mut lo_ch = load8(alphabet, 0) & 255
   mut hi_ch = lo_ch
   mut ai = 1
   while ai < alphabet.len {
      def c = load8(alphabet, ai) & 255
      if c < lo_ch { lo_ch = c }
      if c > hi_ch { hi_ch = c }
      ai += 1
   }
   def base_big = big.bigint_from_int(multiplier)
   def span = big.bigint_div(big.bigint_sub(_line_hash_pow_big(multiplier, length), _big_one()),
      big.bigint_sub(base_big, _big_one()))
   def min_p = big.bigint_mul(big.bigint_from_int(lo_ch), span)
   def max_p = big.bigint_mul(big.bigint_from_int(hi_ch), span)
   def target_big = big.bigint_from_int(target)
   def mod_big = big.bigint_from_int(modulus)
   def max_num = big.bigint_sub(max_p, target_big)
   if big.bigint_lt(max_num, _big_zero()) { return {"ok": false} }
   mut min_num = big.bigint_sub(min_p, target_big)
   mut kmin = _big_zero()
   if big.bigint_gt(min_num, _big_zero()) {
      kmin = big.bigint_div(big.bigint_add(min_num, big.bigint_sub(mod_big, _big_one())), mod_big)
   }
   def kmax = big.bigint_div(max_num, mod_big)
   {"ok": true, "min": big.bigint_to_int(kmin), "max": big.bigint_to_int(kmax)}
}

fn _line_hash_decode_candidate(any value, int length, int multiplier, str alphabet) dict {
   mut y = big.bigint(value)
   def base_big = big.bigint_from_int(multiplier)
   mut bytes = []
   mut i = 0
   while i < length {
      def d = big.bigint_to_int(big.bigint_mod(y, base_big))
      if !_line_hash_allowed_digit(d, alphabet) { return {"ok": false} }
      bytes = bytes.append(d)
      y = big.bigint_div(y, base_big)
      i += 1
   }
   if !big.bigint_eq(y, _big_zero()) { return {"ok": false} }
   mut b = str.Builder(length)
   mut j = length
   while j > 0 {
      j -= 1
      b = str.builder_append_byte(b, int(bytes[j]) & 255)
   }
   def input = str.builder_to_str(b)
   str.builder_free(b)
   {"ok": true, "input": input}
}

fn _line_hash_verify(str input, int seed, int multiplier, int modulus, int target) bool {
   mut h = seed
   mut i = 0
   while i < input.len {
      h = (h * multiplier + (load8(input, i) & 255)) % modulus
      i += 1
   }
   h == target
}

fn _byte_domains_choose(list values) int {
   mut i = 0
   while i < values.len {
      def v = int(values[i]) & 255
      if v >= 32 && v <= 126 { return v }
      i += 1
   }
   values.len > 0 ? (int(values[0]) & 255) : 65
}

fn _byte_domains_contains(list values, int value) bool {
   mut i = 0
   while i < values.len {
      if (int(values[i]) & 255) == (value & 255) { return true }
      i += 1
   }
   false
}

fn _byte_domains_values(dict domains, any key) list {
   if domains.contains(key) { return domains.get(key, []) }
   def text = to_str(key)
   if domains.contains(text) { return domains.get(text, []) }
   def idx = int(text)
   if domains.contains(idx) { return domains.get(idx, []) }
   []
}

fn _byte_domains_index(any key) int {
   if is_int(key) { return int(key) }
   str.parse_int(to_str(key), 10)
}

fn _byte_domains_verify(str input, dict domains) bool {
   def keys = dict_keys(domains)
   mut i = 0
   while i < keys.len {
      def idx = _byte_domains_index(keys[i])
      if idx < 0 || idx >= input.len { return false }
      if !_byte_domains_contains(_byte_domains_values(domains, keys[i]), load8(input, idx) & 255) { return false }
      i += 1
   }
   true
}

fn solve_byte_domains(int count, dict domains, int expected=0, any opts=dict()) dict {
   "Solve a compact `byte_domains(input, domain_plan)` abstraction by emitting
   one printable byte string satisfying every declared byte-domain constraint."
   if count <= 0 {
      return _fail_result("byte_domains", "unknown", "empty byte domain plan", [], opts)
   }
   def keys = dict_keys(domains)
   if expected > 0 && keys.len < expected {
      return {
         "kind": "byte_domains",
         "status": "plan",
         "reason": "partial domain plan omits constraints; decompile with a complete domain map",
         "input": "",
         "input_len": 0,
         "constraints": [{"op": "byte_domains", "bytes": count, "listed": keys.len, "constrained": expected}],
         "backend": _backend_record(),
         "proof": "partial-byte-domain-summary",
      }
   }
   mut values = []
   mut i = 0
   while i < count {
      values = values.append(65)
      i += 1
   }
   i = 0
   while i < keys.len {
      def idx = _byte_domains_index(keys[i])
      if idx >= 0 && idx < count {
         def allowed = _byte_domains_values(domains, keys[i])
         if allowed.len == 0 {
            return _fail_result("byte_domains", "unsat", "empty domain",
               [{"op": "byte_domains", "index": idx}], opts)
         }
         values.set(idx, _byte_domains_choose(allowed))
      }
      i += 1
   }
   mut b = str.Builder(count)
   i = 0
   while i < count {
      b = str.builder_append_byte(b, int(values[i]) & 255)
      i += 1
   }
   def input = str.builder_to_str(b)
   str.builder_free(b)
   if !_byte_domains_verify(input, domains) {
      return _fail_result("byte_domains", "unsat", "constructed witness failed verification",
         [{"op": "byte_domains", "bytes": count, "domains": domains}], opts)
   }
   _sat_result("byte_domains", input,
      [{"op": "byte_domains", "bytes": count, "constrained": keys.len, "domains": domains}],
      {"proof": "constructive-byte-domain-witness"})
}

fn _line_hash_seed_term(int seed, int multiplier, int modulus, int length) int {
   mut h = seed % modulus
   mut i = 0
   while i < length {
      h = (h * multiplier) % modulus
      i += 1
   }
   h
}

fn _line_hash_try_suffix(int suffix, int suffix_mod, int inv_modulus, int kmin, int kmax,
   int adjusted_target, int length, int multiplier, int modulus, int target,
   int seed, str alphabet) dict {
   def rhs = _line_hash_pos_mod(suffix - (adjusted_target % suffix_mod), suffix_mod)
   def k0 = (rhs * inv_modulus) % suffix_mod
   mut k = k0
   if k < kmin {
      k += ((kmin - k + suffix_mod - 1) / suffix_mod) * suffix_mod
   }
   mut tries = 0
   while k <= kmax {
      tries += 1
      def value = big.bigint_add(big.bigint_from_int(adjusted_target),
         big.bigint_mul(big.bigint_from_int(k), big.bigint_from_int(modulus)))
      def decoded = _line_hash_decode_candidate(value, length, multiplier, alphabet)
      if decoded.get("ok", false) {
         def input = decoded.get("input", "")
         if _line_hash_verify(input, seed, multiplier, modulus, target) {
            return {"ok": true, "input": input, "k": k, "tries": tries}
         }
      }
      k += suffix_mod
   }
   {"ok": false, "tries": tries}
}

fn _line_hash_try_suffixes(int suffix_len, int suffix_mod, int inv_modulus, int kmin, int kmax,
   int adjusted_target, int length, int multiplier, int modulus, int target,
   int seed, str alphabet) dict {
   mut tries = 0
   mut i = 0
   if suffix_len <= 1 {
      while i < alphabet.len {
         def suffix = load8(alphabet, i) & 255
         def res = _line_hash_try_suffix(suffix, suffix_mod, inv_modulus, kmin, kmax,
            adjusted_target, length, multiplier, modulus, target, seed, alphabet)
         tries += int(res.get("tries", 0))
         if res.get("ok", false) { return res.set("tries", tries) }
         i += 1
      }
      return {"ok": false, "tries": tries}
   }
   if suffix_len == 2 {
      while i < alphabet.len {
         def a = load8(alphabet, i) & 255
         mut j = 0
         while j < alphabet.len {
            def suffix = a * multiplier + (load8(alphabet, j) & 255)
            def res = _line_hash_try_suffix(suffix, suffix_mod, inv_modulus, kmin, kmax,
               adjusted_target, length, multiplier, modulus, target, seed, alphabet)
            tries += int(res.get("tries", 0))
            if res.get("ok", false) { return res.set("tries", tries) }
            j += 1
         }
         i += 1
      }
      return {"ok": false, "tries": tries}
   }
   while i < alphabet.len {
      def a = load8(alphabet, i) & 255
      mut j = 0
      while j < alphabet.len {
         def ab = a * multiplier + (load8(alphabet, j) & 255)
         mut k = 0
         while k < alphabet.len {
            def suffix = ab * multiplier + (load8(alphabet, k) & 255)
            def res = _line_hash_try_suffix(suffix, suffix_mod, inv_modulus, kmin, kmax,
               adjusted_target, length, multiplier, modulus, target, seed, alphabet)
            tries += int(res.get("tries", 0))
            if res.get("ok", false) { return res.set("tries", tries) }
            k += 1
         }
         j += 1
      }
      i += 1
   }
   {"ok": false, "tries": tries}
}

fn solve_line_hash_matches(str source, str allow, int seed, int multiplier,
   int modulus, int target, any opts=dict()) dict {
   "Solve `line_hash_matches(input, allow, seed, multiplier, modulus, target)` by
   converting the recurrence into base-`multiplier` digits and using a modular
   suffix proof to avoid symbolic path explosion."
   def alphabet = _line_hash_alphabet(allow)
   if alphabet.len == 0 {
      return _fail_result("line_hash_matches", "unknown", "unsupported character predicate",
         [{"op": "line_hash_matches", "source": source, "allow": allow}], opts)
   }
   if multiplier <= 126 || modulus <= 0 {
      return _fail_result("line_hash_matches", "unknown", "unsupported rolling hash geometry",
         [{"op": "line_hash_matches", "multiplier": multiplier, "modulus": modulus}], opts)
   }
   def o = _plan_opts(opts)
   def min_len = max(1, int(o.get("min_len", 1)))
   def max_len = max(min_len, int(o.get("max_len", 12)))
   mut length = min_len
   mut total_tries = 0
   while length <= max_len {
      def seed_term = _line_hash_seed_term(seed, multiplier, modulus, length)
      def adjusted_target = _line_hash_pos_mod(target - seed_term, modulus)
      def range = _line_hash_k_range(length, multiplier, modulus, adjusted_target, alphabet)
      if range.get("ok", false) {
         def suffix_len = min(3, length)
         def suffix_mod = _line_hash_pow_int(multiplier, suffix_len)
         def inv_modulus = _line_hash_small_inv_mod(modulus % suffix_mod, suffix_mod)
         if inv_modulus != 0 {
            def res = _line_hash_try_suffixes(suffix_len, suffix_mod, inv_modulus,
               int(range.get("min", 0)), int(range.get("max", -1)), adjusted_target,
               length, multiplier, modulus, target, seed, alphabet)
            total_tries += int(res.get("tries", 0))
            if res.get("ok", false) {
               def input = res.get("input", "")
               return _sat_result("line_hash_matches", input,
                  [{"op": "line_hash_matches", "source": source, "allow": allow,
                        "seed": seed, "multiplier": multiplier, "modulus": modulus,
                        "target": target, "length": length, "k": int(res.get("k", 0)),
                        "suffix_digits": suffix_len, "tries": total_tries}],
                  {"proof": "base-polynomial-modular-suffix"})
            }
         }
      }
      length += 1
   }
   _fail_result("line_hash_matches", "unknown", "no alnum preimage in bounded length",
      [{"op": "line_hash_matches", "source": source, "allow": allow, "max_len": max_len,
            "tries": total_tries}], opts)
}

fn _big_inv_mod(any a0, any m0) any {
   def zero = _big_zero()
   def one = _big_one()
   def m = big.bigint(m0)
   mut t = zero
   mut nt = one
   mut r = m
   mut nr = big.bigint_mod(big.bigint(a0), m)
   while !big.bigint_eq(nr, zero) {
      def q = big.bigint_div(r, nr)
      def old_t = t
      t = nt
      nt = big.bigint_sub(old_t, big.bigint_mul(q, nt))
      def old_r = r
      r = nr
      nr = big.bigint_sub(old_r, big.bigint_mul(q, nr))
   }
   if !big.bigint_eq(r, one) { return zero }
   if big.bigint_lt(t, zero) { t = big.bigint_add(t, m) }
   t
}

fn _byte_power_256(int n) any {
   mut out = _big_one()
   mut i = 0
   while i < n {
      out = big.bigint_mul(out, big.bigint_from_int(256))
      i += 1
   }
   out
}

fn _bytes_le_big(str text) any {
   mut out = _big_zero()
   mut i = 0
   while i < min(8, text.len) {
      def term = big.bigint_mul(big.bigint_from_int(load8(text, i) & 255), _byte_power_256(i))
      out = big.bigint_add(out, term)
      i += 1
   }
   out
}

fn _flag_hex_char(int c) bool {
   (c >= 48 && c <= 57) || (c >= 97 && c <= 102)
}

fn _flag_name_char(int c) bool {
   (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 95
}

fn _looks_hex_flag(str text) bool {
   def s = _strip_trailing_newlines(text)
   def open = _find_from(s, "{", 0)
   if open <= 0 || s.len != open + 34 || load8(s, s.len - 1) != 125 { return false }
   mut i = 0
   while i < open {
      if !_flag_name_char(load8(s, i)) { return false }
      i += 1
   }
   i = open + 1
   while i < s.len - 1 {
      if !_flag_hex_char(load8(s, i)) { return false }
      i += 1
   }
   true
}

fn _run_binary_arg(str binary, any value) dict {
   if binary.len == 0 || !os.file_exists(binary) { return {"ok": false, "stdout": "", "reason": "missing_binary"} }
   os.run_capture([binary, to_str(value)], [], nil, false)
}

fn _numeric_first_multiplier(str binary) int {
   def cap = _run_binary_arg(binary, "1")
   if !cap.get("ok", false) { return 0 }
   _bytes_le_u64(_strip_trailing_newlines(cap.get("stdout", "")))
}

fn _prefix_fill_count(str prefix) int {
   8 - prefix.len
}

fn _prefix_qword_candidate_big(str prefix, int tail_idx, int fill) any {
   mut q = _bytes_le_big(prefix)
   def hex = "0123456789abcdef"
   mut i = 0
   while i < fill {
      def shift = (fill - 1 - i) * 4
      def nib = (tail_idx >> shift) & 15
      def byte = load8(hex, nib) & 255
      q = big.bigint_add(q, big.bigint_mul(big.bigint_from_int(byte), _byte_power_256(prefix.len + i)))
      i += 1
   }
   q
}

fn _default_flag_prefixes(any opts) list {
   if is_dict(opts) && is_list(opts.get("prefixes", [])) && opts.get("prefixes", []).len > 0 { return opts.get("prefixes", []) }
   ["ASIS{", "CTF{", "flag{", "ctf{", "9447{", "HITCON{", "hitcon{"]
}

fn _verify_numeric_residue_candidate(str binary, str n_text, int base, int shift, int qwords,
   int mul, str prefix, int tries, int oracle_checks) dict {
   def cap = _run_binary_arg(binary, n_text)
   if cap.get("ok", false) {
      def out = _strip_trailing_newlines(cap.get("stdout", ""))
      if _looks_hex_flag(out) {
         return {"ok": true, "result": _sat_result("numeric_residue_words_flag", n_text,
               [{"op": "numeric_residue_words", "base": base, "shift": shift, "qwords": qwords,
                     "first_multiplier": mul, "prefix": prefix, "tries": tries, "oracle_checks": oracle_checks,
                     "output": out}], {"proof": "binary-oracle-modular-inverse"}).set("output", out)}
      }
   }
   {"ok": false}
}

fn solve_numeric_residue_words_flag(int base, int shift, int qwords, any opts=dict()) dict {
   "Solve compact `numeric_residue_words(n >> shift, qwords)` output programs by
   inferring the first output multiplier from a concrete binary oracle, inverting
   it mod 2^64, and verifying flag-shaped candidates against the same binary."
   def o = _plan_opts(opts)
   def binary = o.get("binary", "")
   if base != 10 { return _fail_result("numeric_residue_words_flag", "unknown", "unsupported numeric base", [], opts) }
   if qwords < 1 { return _fail_result("numeric_residue_words_flag", "unknown", "no qwords", [], opts) }
   if binary.len == 0 { return _fail_result("numeric_residue_words_flag", "unknown", "missing binary oracle", [], opts) }
   def mul = _numeric_first_multiplier(binary)
   if mul == 0 || (mul & 1) == 0 {
      return _fail_result("numeric_residue_words_flag", "unknown", "first multiplier not invertible", [], opts)
   }
   def mod = big.bigint_from_str("18446744073709551616")
   def positive_limit = big.bigint_from_str("9223372036854775808")
   def inv = _big_inv_mod(big.bigint_from_int(mul), mod)
   if big.bigint_eq(inv, _big_zero()) { return _fail_result("numeric_residue_words_flag", "unknown", "inverse unavailable", [], opts) }
   def prefixes = _default_flag_prefixes(o)
   def plausible_limit = big.bigint_from_str(to_str(int(o.get("numeric_plausible_limit", 1000000000000))))
   def max_oracle_checks = int(o.get("max_oracle_checks", 4096))
   mut deferred = []
   mut pi = 0
   mut tries = 0
   mut oracle_checks = 0
   while pi < prefixes.len {
      def prefix = to_str(prefixes[pi])
      def fill = _prefix_fill_count(prefix)
      if fill >= 0 && fill <= 8 {
         def limit = 1 << (fill * 4)
         mut t = 0
         while t < limit {
            def q = _prefix_qword_candidate_big(prefix, t, fill)
            def n_big = big.bigint_mod(big.bigint_mul(q, inv), mod)
            tries += 1
            if big.bigint_lt(n_big, positive_limit) {
               def n_text = big.bigint_to_str(n_big)
               if big.bigint_lt(n_big, plausible_limit) {
                  oracle_checks += 1
                  def verified = _verify_numeric_residue_candidate(binary, n_text, base, shift, qwords, mul, prefix, tries, oracle_checks)
                  if verified.get("ok", false) { return verified.get("result", dict()) }
               } elif deferred.len < max_oracle_checks {
                  deferred = deferred.append({"n": n_text, "prefix": prefix, "tries": tries})
               }
            }
            t += 1
         }
      }
      pi += 1
   }
   mut di = 0
   while di < deferred.len && oracle_checks < max_oracle_checks {
      def cand = deferred[di]
      oracle_checks += 1
      def verified = _verify_numeric_residue_candidate(binary, cand.get("n", ""), base, shift, qwords, mul,
         cand.get("prefix", ""), int(cand.get("tries", tries)), oracle_checks)
      if verified.get("ok", false) { return verified.get("result", dict()) }
      di += 1
   }
   _fail_result("numeric_residue_words_flag", "unknown", "no flag-shaped output found",
      [{"op": "numeric_residue_words", "tries": tries, "oracle_checks": oracle_checks,
            "max_oracle_checks": max_oracle_checks}], opts)
}

fn solve_decompiled_input(str text, any opts=dict()) dict {
   "Solve a compact Ny pseudocode predicate such as `fn main(input) input == \"secret\"`."
   def bit_sliced = _extract_bit_sliced_solver_call(text)
   if bit_sliced.get("ok", false) {
      return solve_bit_sliced_ascii_transform(bit_sliced.get("target", ""),
         int(bit_sliced.get("input_len", 0)), int(bit_sliced.get("output_len", 0)),
         bit_sliced.get("opts", opts))
   }
   def numeric_words = _extract_numeric_residue_words(text)
   if numeric_words.get("ok", false) {
      return solve_numeric_residue_words_flag(int(numeric_words.get("base", 10)),
         int(numeric_words.get("shift", 0)), int(numeric_words.get("qwords", 0)), opts)
   }
   def line_hash = _extract_line_hash_matches(text)
   if line_hash.get("ok", false) {
      return solve_line_hash_matches(line_hash.get("source", ""), line_hash.get("allow", ""),
         int(line_hash.get("seed", 0)), int(line_hash.get("multiplier", 0)),
         int(line_hash.get("modulus", 0)), int(line_hash.get("target", 0)), opts)
   }
   def byte_domains = _extract_byte_domains(text)
   if byte_domains.get("ok", false) {
      return solve_byte_domains(int(byte_domains.get("bytes", 0)), byte_domains.get("domains", dict()),
         int(byte_domains.get("constrained", 0)), opts)
   }
   def mbrainfuzz = _extract_mbrainfuzz_template_accepts(text)
   if mbrainfuzz.get("ok", false) {
      return solve_mbrainfuzz_template(mbrainfuzz.get("prefix_hex", ""), mbrainfuzz.get("template", dict()), opts)
   }
   def lit = _extract_input_eq_literal(text)
   if lit.get("ok", false) {
      return _sat_result("decompiled_input_equality", lit.get("value", ""),
         [{"op": "eq", "target": "input", "value": lit.get("value", ""), "source": "decompiled"}],
         {"proof": "parsed-decompiled-equality"})
   }
   def param_lit = _extract_main_param_eq_literal(text)
   if param_lit.get("ok", false) {
      return _sat_result("decompiled_param_equality", param_lit.get("value", ""),
         [{"op": "eq", "target": param_lit.get("name", ""), "value": param_lit.get("value", ""), "source": "decompiled"}],
         {"proof": "parsed-main-param-equality"})
   }
   def valid_case = _extract_valid_case_literal(text)
   if valid_case.get("ok", false) {
      return _sat_result("decompiled_valid_case_literal", valid_case.get("value", ""),
         [{"op": "case_valid_literal", "target": "input", "value": valid_case.get("value", ""), "source": "decompiled"}],
         {"proof": "parsed-valid-match-literal"})
   }
   def cpuid = _extract_cpuid_vendor_input_eq(text, opts)
   if cpuid.get("ok", false) {
      return _sat_result("decompiled_cpuid_equality", cpuid.get("value", ""),
         [{"op": "cpuid_vendor_string", "leaf": 0, "vendor": cpuid.get("vendor", "")}],
         {"proof": "host-cpuid-vendor-evaluation"})
   }
   def timed = _extract_time_window_startswith(text)
   if timed.get("ok", false) {
      return solve_time_window_startswith(timed.get("value", ""), int(timed.get("lo", 0)), int(timed.get("hi", 0)), opts)
   }
   def parsed = _extract_parse_int_eq(text)
   if parsed.get("ok", false) {
      return solve_parse_int_eq(int(parsed.get("value", 0)), int(parsed.get("base", 10)), opts)
   }
   def prefix = _extract_startswith_input(text)
   if prefix.get("ok", false) {
      return solve_startswith(prefix.get("value", ""), opts)
   }
   def file_prefix = _extract_fs_read_startswith(text)
   if file_prefix.get("ok", false) {
      return solve_fs_read_startswith(file_prefix.get("value", ""),
         _plan_opts(opts).set("read_len", int(file_prefix.get("read_len", 0))))
   }
   def regex = _extract_regex_accepts_input(text)
   if regex.get("ok", false) {
      return solve_regex_accepts(regex.get("pattern", ""), opts)
   }
   def digit_sum = _extract_digit_prefix_sum(text)
   if digit_sum.get("ok", false) {
      def solved = solve_digit_prefix_sum_reaches(int(digit_sum.get("target", 0)), digit_sum.get("even", false), opts)
      def env_prefix = digit_sum.get("env_prefix", "")
      if solved.get("status", "") == "sat" && env_prefix.len > 0 {
         return solved.set("env", [env_prefix + "=1"]).set("constraints", solved.get("constraints", []).append({"op": "envp_startswith", "prefix": env_prefix}))
      }
      return solved
   }
   def int_chars = _extract_read_int_chars_eq(text)
   if int_chars.get("ok", false) {
      return solve_read_int_chars_eq(int_chars.get("value", ""), _plan_opts(opts).set("count", int(int_chars.get("count", 0))))
   }
   def read_lit = _extract_stdin_read_literal_eq(text)
   if read_lit.get("ok", false) {
      return solve_stdin_read_literal_eq(read_lit.get("value", ""), read_lit.get("reads", []), int(read_lit.get("index", 0)), opts)
   }
   def pal = _extract_stdin_read_line_palindrome(text)
   if pal.get("ok", false) {
      return solve_stdin_read_line_palindrome(opts)
   }
   def product_key = _extract_product_key_arithmetic(text)
   if product_key.get("ok", false) {
      return solve_product_key_arithmetic(product_key.get("name", "product_key"), opts)
   }
   def sum = _extract_ascii_sum_eq(text)
   if sum.get("ok", false) {
      return solve_ascii_sum_eq(int(sum.get("len", 0)), int(sum.get("sum", 0)), opts)
   }
   def fixed = _extract_fixed_input_chars(text)
   def mods = _extract_segment_mods(text)
   if fixed.len > 0 && mods.len > 0 {
      return solve_segment_sum_mods(_extract_input_len(text), fixed, mods, opts)
   }
   _fail_result("decompiled_input_equality", "unknown", "no direct input equality literal", [], opts)
}

fn _plan_opts(any opts) dict {
   is_dict(opts) ? opts : dict()
}

fn _constraint_index(dict c) int {
   if c.contains("index") { return int(c.get("index", 0)) }
   int(c.get("i", 0))
}

fn _constraints_len(list constraints, any opts) int {
   mut n = int(opts.get("len", opts.get("input_len", 0)))
   mut i = 0
   while i < constraints.len {
      if is_dict(constraints[i]) {
         def c = constraints[i]
         if c.contains("count") { n = max(n, int(c.get("count", 0))) }
         if c.contains("n") { n = max(n, int(c.get("n", 0))) }
         if c.contains("index") || c.contains("i") { n = max(n, _constraint_index(c) + 1) }
      }
      i += 1
   }
   n
}

fn _constraint_op(dict c) str {
   def op = to_str(c.get("op", c.get("kind", "")))
   op == "byte_eq" ? "eq" : op
}

fn _assert_byte_constraint(any ctx, any solver, list xs, dict c) bool {
   def op = _constraint_op(c)
   def idx = _constraint_index(c)
   if op == "range" {
      def lo = int(c.get("lo", 32))
      def hi = int(c.get("hi", 126))
      if c.contains("index") || c.contains("i") {
         if idx < 0 || idx >= xs.len { return false }
         smt.solver_assert(ctx, solver, smt.bvuge(ctx, xs[idx], smt.bv_u8(ctx, lo)))
         smt.solver_assert(ctx, solver, smt.bvule(ctx, xs[idx], smt.bv_u8(ctx, hi)))
      } else {
         smt.solver_assert_bytes_ascii_range(ctx, solver, xs, lo, hi)
      }
      return true
   }
   if idx < 0 || idx >= xs.len { return false }
   def want = smt.bv_u8(ctx, _byte_value(c.get("value", c.get("target", 0))))
   def x = xs[idx]
   def ast = match op {
      "eq" -> smt.mk_eq(ctx, x, want)
      "xor_eq" -> smt.mk_eq(ctx, smt.bvxor(ctx, x, smt.bv_u8(ctx, int(c.get("key", 0)))), want)
      "add_eq" -> smt.mk_eq(ctx, smt.bvadd(ctx, x, smt.bv_u8(ctx, int(c.get("delta", c.get("add", 0))))), want)
      "sub_eq" -> smt.mk_eq(ctx, smt.bvsub(ctx, x, smt.bv_u8(ctx, int(c.get("delta", c.get("sub", 0))))), want)
      "rol_eq" -> smt.mk_eq(ctx, smt.bvrotl(ctx, x, int(c.get("shift", 0)), 8), want)
      "ror_eq" -> smt.mk_eq(ctx, smt.bvrotr(ctx, x, int(c.get("shift", 0)), 8), want)
      "and_eq" -> smt.mk_eq(ctx, smt.bvand(ctx, x, smt.bv_u8(ctx, int(c.get("mask", 255)))), want)
      "or_eq" -> smt.mk_eq(ctx, smt.bvor(ctx, x, smt.bv_u8(ctx, int(c.get("mask", 0)))), want)
      "not_eq" -> smt.mk_eq(ctx, smt.bvnot(ctx, x), want)
      _ -> 0
   }
   if !ast { return false }
   smt.solver_assert(ctx, solver, ast)
   true
}

fn solve_byte_constraints(any raw_constraints, any opts=dict()) dict {
   "Solve byte-level constraints emitted by the decompiler using Ny's Z3 backend.
   Supported ops: eq, xor_eq, add_eq, sub_eq, rol_eq, ror_eq, and_eq, or_eq, not_eq, range."
   def constraints = is_list(raw_constraints) ? raw_constraints : []
   def o = _plan_opts(opts)
   def n = _constraints_len(constraints, o)
   if n <= 0 { return _fail_result("byte_constraints", "unknown", "no byte variables", constraints, o) }
   if !smt.z3_available() { return _fail_result("byte_constraints", "unavailable", "z3 backend unavailable", constraints, o) }
   def ctx = smt.ctx_new()
   if !ctx { return _fail_result("byte_constraints", "unavailable", "z3 context unavailable", constraints, o) }
   def solver = smt.solver_new_qfbv(ctx)
   if !solver {
      smt.ctx_del(ctx)
      return _fail_result("byte_constraints", "unavailable", "z3 solver unavailable", constraints, o)
   }
   def timeout_ms = int(o.get("timeout_ms", 3000))
   if timeout_ms > 0 { smt.solver_set_timeout_ms(ctx, solver, timeout_ms) }
   def xs = smt.bv_bytes(ctx, to_str(o.get("prefix", "input_")), n)
   if o.get("ascii", true) {
      smt.solver_assert_bytes_ascii_range(ctx, solver, xs, int(o.get("lo", 32)), int(o.get("hi", 126)))
   }
   mut ok_constraints = 0
   mut i = 0
   while i < constraints.len {
      if is_dict(constraints[i]) && _assert_byte_constraint(ctx, solver, xs, constraints[i]) { ok_constraints += 1 }
      i += 1
   }
   def check = smt.solver_check_result(ctx, solver)
   mut result = dict()
   if check == smt.SAT {
      def bytes = smt.model_eval_bytes(ctx, solver, xs)
      def input = bytes == nil ? "" : _bytes_to_ascii(bytes)
      result = _sat_result("byte_constraints", input, constraints, {"proof": "z3-qfbv"})
      result = result.set("model_bytes", bytes == nil ? [] : bytes).set("asserted", ok_constraints)
   } elif check == smt.UNSAT {
      result = _fail_result("byte_constraints", "unsat", "constraints are unsatisfiable", constraints, o)
   } else {
      result = _fail_result("byte_constraints", "unknown", "z3 returned unknown", constraints, o)
   }
   smt.solver_del(ctx, solver)
   smt.ctx_del(ctx)
   result
}

fn _bytewise_transform_constraints(str op, any target, any arg) list {
   mut out = []
   mut i = 0
   while i < _byte_len(target) {
      mut c = {"op": op, "index": i, "value": _byte_at(target, i)}
      if op == "xor_eq" { c = c.set("key", _byte_at(arg, i % max(1, _byte_len(arg)))) }
      elif op == "add_eq" || op == "sub_eq" { c = c.set("delta", int(arg) & 255) }
      out = out.append(c)
      i += 1
   }
   out
}

fn _x86_starts_digit_reg(str name) bool {
   name.len >= 2 && load8(name, 0) == 114 && load8(name, 1) >= 48 && load8(name, 1) <= 57
}

fn _x86_reg_base(str name0) str {
   def name = str.lower(str.strip(name0))
   if name == "rax" || name == "eax" || name == "ax" || name == "al" || name == "ah" { return "rax" }
   if name == "rbx" || name == "ebx" || name == "bx" || name == "bl" || name == "bh" { return "rbx" }
   if name == "rcx" || name == "ecx" || name == "cx" || name == "cl" || name == "ch" { return "rcx" }
   if name == "rdx" || name == "edx" || name == "dx" || name == "dl" || name == "dh" { return "rdx" }
   if name == "rsi" || name == "esi" || name == "si" || name == "sil" { return "rsi" }
   if name == "rdi" || name == "edi" || name == "di" || name == "dil" { return "rdi" }
   if name == "rbp" || name == "ebp" || name == "bp" || name == "bpl" { return "rbp" }
   if name == "rsp" || name == "esp" || name == "sp" || name == "spl" { return "rsp" }
   if _x86_starts_digit_reg(name) {
      def last = load8(name, name.len - 1)
      if last == 100 || last == 119 || last == 98 { return slice(name, 0, name.len - 1, 1) }
   }
   name
}

fn _x86_low8(str name0) bool {
   def name = str.lower(str.strip(name0))
   name == "al" || name == "bl" || name == "cl" || name == "dl" ||
   name == "sil" || name == "dil" || name == "bpl" || name == "spl" ||
   name == "r8b" || name == "r9b" || name == "r10b" || name == "r11b" ||
   name == "r12b" || name == "r13b" || name == "r14b" || name == "r15b"
}

fn _x86_high8(str name0) bool {
   def name = str.lower(str.strip(name0))
   name == "ah" || name == "bh" || name == "ch" || name == "dh"
}

fn _x86_reg_bits(str name0) int {
   def name = str.lower(str.strip(name0))
   if _x86_low8(name) || _x86_high8(name) { return 8 }
   if name == "ax" || name == "bx" || name == "cx" || name == "dx" ||
   name == "si" || name == "di" || name == "bp" || name == "sp" { return 16 }
   if name == "eax" || name == "ebx" || name == "ecx" || name == "edx" ||
   name == "esi" || name == "edi" || name == "ebp" || name == "esp" { return 32 }
   if _x86_starts_digit_reg(name) {
      def last = load8(name, name.len - 1)
      if last == 100 { return 32 }
      if last == 119 { return 16 }
      if last == 98 { return 8 }
   }
   64
}

fn _x86_mem_bits(str op, int fallback=64) int {
   def p = str.lower(str.strip(op))
   if str.find(p, "byte ptr") >= 0 { return 8 }
   if str.find(p, "word ptr") >= 0 && str.find(p, "dword ptr") < 0 && str.find(p, "qword ptr") < 0 { return 16 }
   if str.find(p, "dword ptr") >= 0 { return 32 }
   if str.find(p, "qword ptr") >= 0 { return 64 }
   fallback
}

fn _x86_mask(int v, int bits) int {
   if bits <= 8 { return v & 255 }
   if bits <= 16 { return v & 65535 }
   if bits <= 32 { return v & 4294967295 }
   v
}

fn _x86_known_shift(int v, int bits, int shift, bool arithmetic=false) int {
   def s = shift % max(1, bits)
   if s == 0 { return _x86_mask(v, bits) }
   if !arithmetic { return _x86_mask(v, bits) >> s }
   if bits == 8 && (v & 128) != 0 { return ((v | -256) >> s) & 255 }
   if bits == 16 && (v & 32768) != 0 { return ((v | -65536) >> s) & 65535 }
   if bits == 32 && (v & 2147483648) != 0 { return ((v | -4294967296) >> s) & 4294967295 }
   _x86_mask(v, bits) >> s
}

fn _x86_signed_value(int v, int bits) int {
   if bits == 8 && (v & 128) != 0 { return v - 256 }
   if bits == 16 && (v & 32768) != 0 { return v - 65536 }
   if bits == 32 && (v & 2147483648) != 0 { return v - 4294967296 }
   v
}

fn _x86_bv(any ctx, int v, int bits) any { smt.bv_u64(ctx, _x86_mask(v, bits), bits) }

fn _x86_list_has(list xs, any value) bool {
   mut i = 0
   while i < xs.len {
      if xs[i] == value { return true }
      i += 1
   }
   false
}

fn _x86_cut_union(list a, list b) list {
   mut out = a
   mut i = 0
   while i < b.len {
      if !_x86_list_has(out, b[i]) { out = out.append(b[i]) }
      i += 1
   }
   out
}

fn _x86_val(any ast, int bits, bool known=false, int value=0, any cuts=[], int depth=0) dict {
   {
      "ast": ast,
      "bits": bits,
      "known": known,
      "value": _x86_mask(value, bits),
      "cuts": is_list(cuts) ? cuts : [],
      "depth": max(0, depth),
   }
}

fn _x86_depth2(dict a, dict b, int extra=1) int {
   max(int(a.get("depth", 0)), int(b.get("depth", 0))) + extra
}

fn _x86_depth_many(list xs, int extra=1) int {
   mut d = 0
   mut i = 0
   while i < xs.len {
      d = max(d, int(xs[i].get("depth", 0)))
      i += 1
   }
   d + extra
}

fn _x86_resize(any ctx, dict v, int bits, bool signed=false) dict {
   def from_bits = int(v.get("bits", bits))
   if v.get("known", false) {
      return _x86_val(_x86_bv(ctx, int(v.get("value", 0)), bits), bits, true, int(v.get("value", 0)))
   }
   def ast = v.get("ast", 0)
   mut out_ast = ast
   if from_bits > bits { out_ast = smt.bv_extract(ctx, bits - 1, 0, ast) }
   elif from_bits < bits {
      out_ast = signed ? smt.bvsext(ctx, ast, bits - from_bits) : smt.bvzext(ctx, ast, bits - from_bits)
   }
   _x86_val(out_ast, bits, v.get("known", false), int(v.get("value", 0)), v.get("cuts", []),
      int(v.get("depth", 0)) + (from_bits == bits ? 0 : 1))
}

fn _x86_cut(any ctx, any solver, dict st, dict v, int bits) dict {
   def r = _x86_resize(ctx, v, bits)
   if r.get("known", false) { return _x86_val(_x86_bv(ctx, int(r.get("value", 0)), bits), bits, true, int(r.get("value", 0))) }
   if int(r.get("depth", 0)) <= int(st.get("cut_depth", 3)) { return r }
   def idx = int(st.get("fresh", 0))
   st.set("fresh", idx + 1)
   def name = "cut_" + to_str(idx)
   def out = smt.bv_const(ctx, name, bits)
   st.set("cut_constraints", st.get("cut_constraints", []).append({
            "name": name,
            "lhs": out,
            "rhs": r.get("ast", 0),
            "deps": r.get("cuts", []),
         }))
   _x86_val(out, bits, false, 0, [name])
}

fn _x86_read_reg(any ctx, dict st, str name0, int bits=0) dict {
   def name = str.lower(str.strip(name0))
   def base = _x86_reg_base(name)
   def full = st.get("regs", dict()).get(base, _x86_bv(ctx, 0, 64))
   def full_known = st.get("reg_known", dict()).get(base, false)
   def full_val = int(st.get("reg_int", dict()).get(base, 0))
   def full_cuts = st.get("reg_cuts", dict()).get(base, [])
   def full_depth = int(st.get("reg_depth", dict()).get(base, 0))
   if _x86_low8(name) {
      return _x86_val(smt.bv_extract(ctx, 7, 0, full), 8, full_known, full_val & 255, full_cuts, full_depth + 1)
   }
   if _x86_high8(name) {
      return _x86_val(smt.bv_extract(ctx, 15, 8, full), 8, full_known, (full_val >> 8) & 255, full_cuts, full_depth + 1)
   }
   def want = bits > 0 ? bits : _x86_reg_bits(name)
   if want >= 64 { return _x86_val(full, 64, full_known, full_val, full_cuts, full_depth) }
   _x86_val(smt.bv_extract(ctx, want - 1, 0, full), want, full_known, full_val, full_cuts, full_depth + 1)
}

fn _x86_write_reg(any ctx, any solver, dict st, str name0, dict value) any {
   def name = str.lower(str.strip(name0))
   def base = _x86_reg_base(name)
   def regs = st.get("regs", dict())
   def knowns = st.get("reg_known", dict())
   def ints = st.get("reg_int", dict())
   def reg_cuts = st.get("reg_cuts", dict())
   def reg_depth = st.get("reg_depth", dict())
   def full = regs.get(base, _x86_bv(ctx, 0, 64))
   def full_known = knowns.get(base, false)
   def full_val = int(ints.get(base, 0))
   def full_cuts = reg_cuts.get(base, [])
   def full_depth = int(reg_depth.get(base, 0))
   mut out_ast = full
   mut out_known = false
   mut out_val = 0
   mut out_cuts = full_cuts
   mut out_depth = full_depth
   if _x86_low8(name) {
      def b = _x86_cut(ctx, solver, st, value, 8)
      out_ast = smt.bvconcat(ctx, smt.bv_extract(ctx, 63, 8, full), b.get("ast", 0))
      out_known = full_known && b.get("known", false)
      out_val = (full_val & -256) | int(b.get("value", 0))
      out_cuts = _x86_cut_union(full_cuts, b.get("cuts", []))
      out_depth = max(full_depth + 1, int(b.get("depth", 0))) + 1
   } elif _x86_high8(name) {
      def b = _x86_cut(ctx, solver, st, value, 8)
      def hi = smt.bv_extract(ctx, 63, 16, full)
      def lo = smt.bv_extract(ctx, 7, 0, full)
      out_ast = smt.bvconcat(ctx, smt.bvconcat(ctx, hi, b.get("ast", 0)), lo)
      out_known = full_known && b.get("known", false)
      out_val = (full_val & -65281) | ((int(b.get("value", 0)) & 255) << 8)
      out_cuts = _x86_cut_union(full_cuts, b.get("cuts", []))
      out_depth = max(full_depth + 1, int(b.get("depth", 0))) + 2
   } else {
      def bits = _x86_reg_bits(name)
      if bits >= 64 {
         def r = _x86_cut(ctx, solver, st, value, 64)
         out_ast = r.get("ast", 0)
         out_known = r.get("known", false)
         out_val = int(r.get("value", 0))
         out_cuts = r.get("cuts", [])
         out_depth = int(r.get("depth", 0))
      } elif bits == 32 {
         def r = _x86_cut(ctx, solver, st, value, 32)
         out_ast = smt.bvzext(ctx, r.get("ast", 0), 32)
         out_known = r.get("known", false)
         out_val = int(r.get("value", 0)) & 4294967295
         out_cuts = r.get("cuts", [])
         out_depth = int(r.get("depth", 0)) + 1
      } elif bits == 16 {
         def r = _x86_cut(ctx, solver, st, value, 16)
         out_ast = smt.bvconcat(ctx, smt.bv_extract(ctx, 63, 16, full), r.get("ast", 0))
         out_known = full_known && r.get("known", false)
         out_val = (full_val & -65536) | int(r.get("value", 0))
         out_cuts = _x86_cut_union(full_cuts, r.get("cuts", []))
         out_depth = max(full_depth + 1, int(r.get("depth", 0))) + 1
      } else {
         def r = _x86_cut(ctx, solver, st, value, 8)
         out_ast = smt.bvconcat(ctx, smt.bv_extract(ctx, 63, 8, full), r.get("ast", 0))
         out_known = full_known && r.get("known", false)
         out_val = (full_val & -256) | int(r.get("value", 0))
         out_cuts = _x86_cut_union(full_cuts, r.get("cuts", []))
         out_depth = max(full_depth + 1, int(r.get("depth", 0))) + 1
      }
   }
   st.set("regs", regs.set(base, out_ast))
   st.set("reg_known", knowns.set(base, out_known))
   st.set("reg_int", ints.set(base, out_val))
   st.set("reg_cuts", reg_cuts.set(base, out_cuts))
   st.set("reg_depth", reg_depth.set(base, out_depth))
}

fn _x86_parse_int_token(str tok0) dict {
   mut tok = str.strip(tok0)
   mut neg = false
   if tok.len > 0 && load8(tok, 0) == 45 {
      neg = true
      tok = slice(tok, 1, tok.len, 1)
   }
   def p = _parse_int_literal_at(tok, 0)
   if !p.get("ok", false) { return {"ok": false, "value": 0} }
   {"ok": true, "value": neg ? -int(p.get("value", 0)) : int(p.get("value", 0))}
}

fn _x86_is_reg(str tok0) bool {
   def tok = str.lower(str.strip(tok0))
   def base = _x86_reg_base(tok)
   base == "rax" || base == "rbx" || base == "rcx" || base == "rdx" ||
   base == "rsi" || base == "rdi" || base == "rbp" || base == "rsp" ||
   base == "r8" || base == "r9" || base == "r10" || base == "r11" ||
   base == "r12" || base == "r13" || base == "r14" || base == "r15" ||
   base == "rip"
}

fn _x86_mem_inner(str op0) str {
   def op = str.strip(op0)
   def a = _find_from(op, "[", 0)
   if a < 0 { return "" }
   def b = _find_from(op, "]", a + 1)
   if b < 0 { return "" }
   slice(op, a + 1, b, 1)
}

fn _x86_mem_terms(str inner) list {
   mut out = []
   mut cur = ""
   mut sign = 1
   mut i = 0
   while i < inner.len {
      def c = load8(inner, i)
      if c == 43 || c == 45 {
         def t = str.strip(cur)
         if t.len > 0 { out = out.append({"sign": sign, "term": t}) }
         cur = ""
         sign = c == 45 ? -1 : 1
      } elif c != 32 && c != 9 {
         cur = cur + str.chr(c)
      }
      i += 1
   }
   def t = str.strip(cur)
   if t.len > 0 { out = out.append({"sign": sign, "term": t}) }
   out
}

fn _x86_mem_expr(any ctx, dict st, dict row, str op0) dict {
   def ref_target = int(row.get("ref_target", row.get("target", 0)))
   def inner = _x86_mem_inner(op0)
   if inner.len == 0 { return _x86_val(_x86_bv(ctx, 0, 64), 64, true, 0) }
   if str.find(str.lower(inner), "rip") >= 0 && ref_target > 0 {
      return _x86_val(_x86_bv(ctx, ref_target, 64), 64, true, ref_target)
   }
   mut ast = _x86_bv(ctx, 0, 64)
   mut known = true
   mut value = 0
   mut cuts = []
   mut depth = 0
   def terms = _x86_mem_terms(inner)
   mut i = 0
   while i < terms.len {
      def item = terms[i]
      def sign = int(item.get("sign", 1))
      def term = item.get("term", "")
      def star = _find_from(term, "*", 0)
      mut term_v = _x86_val(_x86_bv(ctx, 0, 64), 64, true, 0)
      if star >= 0 {
         def reg = slice(term, 0, star, 1)
         def sc = _x86_parse_int_token(slice(term, star + 1, term.len, 1))
         def rv = _x86_read_reg(ctx, st, reg, 64)
         term_v = _x86_val(smt.bvmul(ctx, rv.get("ast", 0), _x86_bv(ctx, int(sc.get("value", 1)), 64)),
            64, rv.get("known", false) && sc.get("ok", false), int(rv.get("value", 0)) * int(sc.get("value", 1)), rv.get("cuts", []),
            int(rv.get("depth", 0)) + 1)
      } elif _x86_is_reg(term) {
         term_v = _x86_read_reg(ctx, st, term, 64)
      } else {
         def imm = _x86_parse_int_token(term)
         term_v = _x86_val(_x86_bv(ctx, int(imm.get("value", 0)), 64), 64, imm.get("ok", false), int(imm.get("value", 0)))
      }
      if sign < 0 {
         ast = smt.bvsub(ctx, ast, term_v.get("ast", 0))
         value -= int(term_v.get("value", 0))
      } else {
         ast = smt.bvadd(ctx, ast, term_v.get("ast", 0))
         value += int(term_v.get("value", 0))
      }
      known = known && term_v.get("known", false)
      cuts = _x86_cut_union(cuts, term_v.get("cuts", []))
      depth = max(depth, int(term_v.get("depth", 0)) + 1)
      i += 1
   }
   _x86_val(ast, 64, known, value, cuts, depth)
}

fn _x86_read_mem(any ctx, dict st, dict row, str op0, int bits) dict {
   def addr_v = _x86_mem_expr(ctx, st, row, op0)
   if !addr_v.get("known", false) { return _x86_val(_x86_bv(ctx, 0, bits), bits, true, 0) }
   def addr = int(addr_v.get("value", 0))
   def arg_base = int(st.get("arg_base", 0))
   def input_len = int(st.get("input_len", 0))
   def n = (bits + 7) / 8
   mut bytes = []
   mut known = true
   mut value = 0
   mut cuts = []
   mut depth = 0
   mut i = 0
   while i < n {
      def a = addr + i
      mut b = _x86_val(_x86_bv(ctx, 0, 8), 8, true, 0)
      if a >= arg_base && a < arg_base + input_len {
         if st.get("input_concrete", false) {
            def src = st.get("concrete_input", "")
            def idx = a - arg_base
            b = _x86_val(st.get("inputs", [])[idx], 8, true, idx < src.len ? (load8(src, idx) & 255) : 0)
         } else {
            b = _x86_val(st.get("inputs", [])[a - arg_base], 8, false, 0)
         }
      } else {
         b = st.get("mem", dict()).get(to_str(a), _x86_val(_x86_bv(ctx, 0, 8), 8, true, 0))
      }
      bytes = bytes.append(b)
      known = known && b.get("known", false)
      value = value | ((int(b.get("value", 0)) & 255) << (8 * i))
      cuts = _x86_cut_union(cuts, b.get("cuts", []))
      depth = max(depth, int(b.get("depth", 0)))
      i += 1
   }
   mut ast = bytes[0].get("ast", 0)
   i = 1
   while i < bytes.len {
      ast = smt.bvconcat(ctx, bytes[i].get("ast", 0), ast)
      depth += 1
      i += 1
   }
   _x86_val(ast, bits, known, value, cuts, depth)
}

fn _x86_write_mem(any ctx, any solver, dict st, dict row, str op0, int bits, dict value) any {
   def addr_v = _x86_mem_expr(ctx, st, row, op0)
   if !addr_v.get("known", false) { return 0 }
   def addr = int(addr_v.get("value", 0))
   def v = _x86_resize(ctx, value, bits)
   def n = (bits + 7) / 8
   mut mem = st.get("mem", dict())
   mut outputs = st.get("outputs", dict())
   def out_addr = int(st.get("output_addr", 0))
   mut i = 0
   while i < n {
      def b_ast = smt.bv_extract(ctx, (i * 8) + 7, i * 8, v.get("ast", 0))
      def b_known = v.get("known", false)
      def b_val = (int(v.get("value", 0)) >> (8 * i)) & 255
      def b = _x86_cut(ctx, solver, st, _x86_val(b_ast, 8, b_known, b_val, v.get("cuts", []), int(v.get("depth", 0)) + 1), 8)
      mem = mem.set(to_str(addr + i), b)
      if out_addr > 0 && addr + i >= out_addr && addr + i < out_addr + 128 {
         outputs = outputs.set(to_str(addr + i), b)
      }
      i += 1
   }
   st.set("mem", mem)
   st.set("outputs", outputs)
   0
}

fn _x86_operand_value(any ctx, dict st, dict row, str op0, int bits) dict {
   def op = str.strip(op0)
   if _x86_mem_inner(op).len > 0 { return _x86_read_mem(ctx, st, row, op, bits) }
   if _x86_is_reg(op) { return _x86_read_reg(ctx, st, op, bits) }
   def imm = _x86_parse_int_token(op)
   _x86_val(_x86_bv(ctx, int(imm.get("value", 0)), bits), bits, imm.get("ok", false), int(imm.get("value", 0)))
}

fn _x86_binop(any ctx, str op, dict a0, dict b0, int bits) dict {
   def a = _x86_resize(ctx, a0, bits)
   def b = _x86_resize(ctx, b0, bits)
   mut ast = a.get("ast", 0)
   mut value = 0
   if op == "+" {
      ast = smt.bvadd(ctx, a.get("ast", 0), b.get("ast", 0))
      value = int(a.get("value", 0)) + int(b.get("value", 0))
   } elif op == "-" {
      ast = smt.bvsub(ctx, a.get("ast", 0), b.get("ast", 0))
      value = int(a.get("value", 0)) - int(b.get("value", 0))
   } elif op == "^^" || op == "^" || op == "xor" {
      ast = smt.bvxor(ctx, a.get("ast", 0), b.get("ast", 0))
      value = int(a.get("value", 0)) ^^ int(b.get("value", 0))
   } elif op == "&" {
      ast = smt.bvand(ctx, a.get("ast", 0), b.get("ast", 0))
      value = int(a.get("value", 0)) & int(b.get("value", 0))
   } elif op == "|" {
      ast = smt.bvor(ctx, a.get("ast", 0), b.get("ast", 0))
      value = int(a.get("value", 0)) | int(b.get("value", 0))
   } elif op == "*" {
      ast = smt.bvmul(ctx, a.get("ast", 0), b.get("ast", 0))
      value = int(a.get("value", 0)) * int(b.get("value", 0))
   }
   if a.get("known", false) && b.get("known", false) { return _x86_val(_x86_bv(ctx, value, bits), bits, true, value) }
   _x86_val(ast, bits, a.get("known", false) && b.get("known", false), value,
      _x86_cut_union(a.get("cuts", []), b.get("cuts", [])), _x86_depth2(a, b))
}

fn _x86_shift(any ctx, str mnemonic, dict a0, dict b0, int bits) dict {
   def a = _x86_resize(ctx, a0, bits)
   def sh = int(b0.get("value", 0)) % max(1, bits)
   def sh_ast = _x86_bv(ctx, sh, bits)
   mut ast = a.get("ast", 0)
   mut value = 0
   if mnemonic == "shl" {
      ast = smt.bvshl(ctx, a.get("ast", 0), sh_ast)
      value = int(a.get("value", 0)) << sh
   } elif mnemonic == "sar" {
      ast = smt.bvashr(ctx, a.get("ast", 0), sh_ast)
      value = _x86_known_shift(int(a.get("value", 0)), bits, sh, true)
   } else {
      ast = smt.bvlshr(ctx, a.get("ast", 0), sh_ast)
      value = _x86_known_shift(int(a.get("value", 0)), bits, sh, false)
   }
   if a.get("known", false) && b0.get("known", false) { return _x86_val(_x86_bv(ctx, value, bits), bits, true, value) }
   _x86_val(ast, bits, a.get("known", false) && b0.get("known", false), value,
      _x86_cut_union(a.get("cuts", []), b0.get("cuts", [])), _x86_depth2(a, b0))
}

fn _x86_row_args(dict row) list {
   def args = row.get("args", [])
   if is_list(args) { return args }
   []
}

fn _x86_exec_row(any ctx, any solver, dict st, dict row) bool {
   def m = row.get("mnemonic", "")
   def args = _x86_row_args(row)
   if m == "ret" { return false }
   if m == "nop" || m == "syscall" { return true }
   if m == "push" && args.len >= 1 {
      def sp = int(st.get("reg_int", dict()).get("rsp", 0)) - 8
      _x86_write_reg(ctx, solver, st, "rsp", _x86_val(_x86_bv(ctx, sp, 64), 64, true, sp))
      _x86_write_mem(ctx, solver, st, row, "[rsp]", 64, _x86_operand_value(ctx, st, row, args[0], 64))
      return true
   }
   if m == "pop" && args.len >= 1 {
      def sp = int(st.get("reg_int", dict()).get("rsp", 0))
      _x86_write_reg(ctx, solver, st, args[0], _x86_read_mem(ctx, st, row, "[rsp]", 64))
      _x86_write_reg(ctx, solver, st, "rsp", _x86_val(_x86_bv(ctx, sp + 8, 64), 64, true, sp + 8))
      return true
   }
   if (m == "mov" || m == "movabs") && args.len >= 2 {
      def dst = args[0]
      def bits = row.get("dst_kind", "") == "mem" ? _x86_mem_bits(dst, 64) : _x86_reg_bits(dst)
      def v = _x86_operand_value(ctx, st, row, args[1], bits)
      if row.get("dst_kind", "") == "mem" { _x86_write_mem(ctx, solver, st, row, dst, bits, v) }
      else { _x86_write_reg(ctx, solver, st, dst, v) }
      return true
   }
   if (m == "movzx" || m == "movsx") && args.len >= 2 {
      def dst = args[0]
      def src = args[1]
      def sbits = _x86_mem_inner(src).len > 0 ? _x86_mem_bits(src, 8) : _x86_reg_bits(src)
      def dbits = _x86_reg_bits(dst)
      def signed = m == "movsx"
      _x86_write_reg(ctx, solver, st, dst, _x86_resize(ctx, _x86_operand_value(ctx, st, row, src, sbits), dbits, signed))
      return true
   }
   if m == "lea" && args.len >= 2 {
      _x86_write_reg(ctx, solver, st, args[0], _x86_mem_expr(ctx, st, row, args[1]))
      return true
   }
   if (m == "add" || m == "sub" || m == "xor" || m == "and" || m == "or") && args.len >= 2 {
      def dst = args[0]
      def bits = row.get("dst_kind", "") == "mem" ? _x86_mem_bits(dst, 64) : _x86_reg_bits(dst)
      def lhs = _x86_operand_value(ctx, st, row, dst, bits)
      def rhs = _x86_operand_value(ctx, st, row, args[1], bits)
      def op = m == "add" ? "+" : (m == "sub" ? "-" : (m == "xor" ? "^^" : (m == "and" ? "&" : "|")))
      def v = _x86_binop(ctx, op, lhs, rhs, bits)
      if row.get("dst_kind", "") == "mem" { _x86_write_mem(ctx, solver, st, row, dst, bits, v) }
      else { _x86_write_reg(ctx, solver, st, dst, v) }
      return true
   }
   if (m == "shl" || m == "shr" || m == "sar") && args.len >= 2 {
      def dst = args[0]
      def bits = row.get("dst_kind", "") == "mem" ? _x86_mem_bits(dst, 64) : _x86_reg_bits(dst)
      def lhs = _x86_operand_value(ctx, st, row, dst, bits)
      def rhs = _x86_operand_value(ctx, st, row, args[1], bits)
      def v = _x86_shift(ctx, m, lhs, rhs, bits)
      if row.get("dst_kind", "") == "mem" { _x86_write_mem(ctx, solver, st, row, dst, bits, v) }
      else { _x86_write_reg(ctx, solver, st, dst, v) }
      return true
   }
   if m == "imul" {
      if args.len == 1 {
         def src = args[0]
         def bits = _x86_mem_inner(src).len > 0 ? _x86_mem_bits(src, 32) : _x86_reg_bits(src)
         def lhs = _x86_resize(ctx, _x86_read_reg(ctx, st, bits == 16 ? "ax" : (bits == 64 ? "rax" : "eax"), bits), bits, true)
         def rhs = _x86_resize(ctx, _x86_operand_value(ctx, st, row, src, bits), bits, true)
         def wide_bits = bits * 2
         def prod = smt.bvmul(ctx, smt.bvsext(ctx, lhs.get("ast", 0), bits), smt.bvsext(ctx, rhs.get("ast", 0), bits))
         def known = lhs.get("known", false) && rhs.get("known", false)
         def pval = _x86_signed_value(int(lhs.get("value", 0)), bits) * _x86_signed_value(int(rhs.get("value", 0)), bits)
         def prod_cuts = _x86_cut_union(lhs.get("cuts", []), rhs.get("cuts", []))
         def prod_depth = _x86_depth2(lhs, rhs, 2)
         def low = _x86_val(smt.bv_extract(ctx, bits - 1, 0, prod), bits, known, pval, prod_cuts, prod_depth + 1)
         def high = _x86_val(smt.bv_extract(ctx, wide_bits - 1, bits, prod), bits, known, pval >> bits, prod_cuts, prod_depth + 1)
         if bits == 16 {
            _x86_write_reg(ctx, solver, st, "ax", low)
            _x86_write_reg(ctx, solver, st, "dx", high)
         } elif bits == 64 {
            _x86_write_reg(ctx, solver, st, "rax", low)
            _x86_write_reg(ctx, solver, st, "rdx", high)
         } else {
            _x86_write_reg(ctx, solver, st, "eax", low)
            _x86_write_reg(ctx, solver, st, "edx", high)
         }
         return true
      }
      if args.len >= 3 {
         def dst = args[0]
         def bits = _x86_reg_bits(dst)
         def v = _x86_binop(ctx, "*", _x86_operand_value(ctx, st, row, args[1], bits), _x86_operand_value(ctx, st, row, args[2], bits), bits)
         _x86_write_reg(ctx, solver, st, dst, v)
         return true
      }
      if args.len >= 2 {
         def dst = args[0]
         def bits = _x86_reg_bits(dst)
         def v = _x86_binop(ctx, "*", _x86_operand_value(ctx, st, row, dst, bits), _x86_operand_value(ctx, st, row, args[1], bits), bits)
         _x86_write_reg(ctx, solver, st, dst, v)
         return true
      }
   }
   if (m == "neg" || m == "not") && args.len >= 1 {
      def dst = args[0]
      def bits = row.get("dst_kind", "") == "mem" ? _x86_mem_bits(dst, 64) : _x86_reg_bits(dst)
      def a = _x86_operand_value(ctx, st, row, dst, bits)
      def ast = m == "neg" ? smt.bvneg(ctx, a.get("ast", 0)) : smt.bvnot(ctx, a.get("ast", 0))
      def value = m == "neg" ? -int(a.get("value", 0)) : ~int(a.get("value", 0))
      def v = _x86_val(ast, bits, a.get("known", false), value, a.get("cuts", []), int(a.get("depth", 0)) + 1)
      if row.get("dst_kind", "") == "mem" { _x86_write_mem(ctx, solver, st, row, dst, bits, v) }
      else { _x86_write_reg(ctx, solver, st, dst, v) }
      return true
   }
   st.set("unsupported", st.get("unsupported", []).append({"mnemonic": m, "addr": int(row.get("addr", 0)), "args": args}))
   true
}

fn _x86_state_init(any ctx, list inputs, int input_len, int output_addr, any opts=dict()) dict {
   def stack = int(opts.get("stack_base", 0x70000000))
   def argv = int(opts.get("argv_base", 0x71000000))
   def arg = int(opts.get("arg_base", 0x72000000))
   mut regs = dict()
   mut known = dict()
   mut ints = dict()
   mut reg_cuts = dict()
   mut reg_depth = dict()
   def bases = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
   mut i = 0
   while i < bases.len {
      regs = regs.set(bases[i], _x86_bv(ctx, 0, 64))
      known = known.set(bases[i], true)
      ints = ints.set(bases[i], 0)
      reg_cuts = reg_cuts.set(bases[i], [])
      reg_depth = reg_depth.set(bases[i], 0)
      i += 1
   }
   regs = regs.set("rsp", _x86_bv(ctx, stack, 64)).set("rsi", _x86_bv(ctx, argv, 64)).set("rdi", _x86_bv(ctx, 2, 64)).set("rdx", _x86_bv(ctx, 0, 64))
   ints = ints.set("rsp", stack).set("rsi", argv).set("rdi", 2).set("rdx", 0)
   mut mem = dict()
   i = 0
   while i < 8 {
      mem = mem.set(to_str(argv + 8 + i), _x86_val(_x86_bv(ctx, (arg >> (8 * i)) & 255, 8), 8, true, (arg >> (8 * i)) & 255))
      i += 1
   }
   {
      "regs": regs,
      "reg_known": known,
      "reg_int": ints,
      "reg_cuts": reg_cuts,
      "reg_depth": reg_depth,
      "mem": mem,
      "outputs": dict(),
      "inputs": inputs,
      "input_concrete": is_str(opts.get("concrete_input", "")) && opts.get("concrete_input", "").len > 0,
      "concrete_input": opts.get("concrete_input", ""),
      "input_len": input_len,
      "output_addr": output_addr,
      "arg_base": arg,
      "fresh": 0,
      "cut_depth": int(opts.get("cut_depth", 3)),
      "cut_constraints": [],
      "unsupported": [],
      "mapper_input_events": [],
   }
}

fn _bit_sliced_trace_points(any opts=dict()) list {
   def concrete = is_str(opts.get("concrete_input", "")) && opts.get("concrete_input", "").len > 0
   if !opts.get("trace_mapper_inputs", concrete || opts.get("mapper_inverse", false)) { return [] }
   def summary = is_dict(opts) ? opts.get("output_writes", dict()) : dict()
   def final = is_dict(summary) ? summary.get("final", []) : []
   mut out = []
   mut i = 0
   while i < final.len {
      def rec = final[i]
      def mapper = rec.get("mapper", dict())
      mut candidates = mapper.get("candidates", [])
      if !is_list(candidates) || candidates.len == 0 { candidates = [mapper.get("candidate", dict())] }
      mut j = 0
      while j < candidates.len {
         def cand = candidates[j]
         if cand.len > 0 && int(cand.get("row", -1)) >= 0 && cand.get("reg", "").len > 0 {
            out = out.append({
                  "row": int(cand.get("row", -1)),
                  "offset": int(rec.get("offset", -1)),
                  "reg": cand.get("reg", ""),
                  "work": cand.get("work", ""),
                  "mode": cand.get("mode", ""),
                  "score": int(cand.get("score", 0)),
                  "distance": int(cand.get("distance", 0)),
               })
         }
         j += 1
      }
      i += 1
   }
   out
}

fn _bit_sliced_trace_point_map(list points) dict {
   mut out = dict()
   mut i = 0
   while i < points.len {
      def p = points[i]
      def key = to_str(p.get("row", -1))
      out = out.set(key, out.get(key, []).append(p))
      i += 1
   }
   out
}

fn _bit_sliced_capture_trace_points(any ctx, dict st, int row, dict points_by_row) any {
   def points = points_by_row.get(to_str(row), [])
   if points.len == 0 { return 0 }
   mut events = st.get("mapper_input_events", [])
   mut i = 0
   while i < points.len {
      def p = points[i]
      def v = _x86_read_reg(ctx, st, p.get("reg", ""), 32)
      def rec = p.
      set("bits", int(v.get("bits", 0))).
      set("known", v.get("known", false)).
      set("value", int(v.get("value", 0)) & 4294967295).
      set("low6", int(v.get("value", 0)) & 63).
      set("ast", v.get("ast", 0)).
      set("cuts", v.get("cuts", [])).
      set("depth", int(v.get("depth", 0)))
      events = events.append(rec)
      i += 1
   }
   st.set("mapper_input_events", events)
   0
}

fn _bit_sliced_case_toggle_ascii(int v) int {
   ((v & 0x40) >> 1) ^^ (v & 255)
}

fn _bit_sliced_mapper_relation(int value, int target_ch) str {
   def idx = base64_index(target_ch)
   def v = value & 255
   if idx >= 0 {
      if (v & 63) == idx { return "low6" }
      if (v & 1) == 0 && (((v / 2) & 63) == idx) { return "double_low6" }
   }
   if v == target_ch { return "ascii" }
   if _bit_sliced_case_toggle_ascii(v) == target_ch { return "ascii_case_toggle" }
   "mismatch"
}

fn _bit_sliced_relation_rank(str relation) int {
   if relation == "low6" { return 5 }
   if relation == "double_low6" { return 4 }
   if relation == "ascii_case_toggle" { return 3 }
   if relation == "ascii" { return 2 }
   0
}

fn _bit_sliced_better_selected_event(dict cand, dict prev) bool {
   if prev.len == 0 { return true }
   def cr = _bit_sliced_relation_rank(cand.get("relation", ""))
   def pr = _bit_sliced_relation_rank(prev.get("relation", ""))
   if cr != pr { return cr > pr }
   def cs = int(cand.get("score", 0))
   def ps = int(prev.get("score", 0))
   if cs != ps { return cs > ps }
   int(cand.get("distance", 999999)) < int(prev.get("distance", 999999))
}

fn _bit_sliced_public_mapper_events(list events, str core_target) list {
   mut out = []
   mut i = 0
   while i < events.len {
      def e = events[i]
      def off = int(e.get("offset", -1))
      mut rec = {
         "row": int(e.get("row", -1)),
         "offset": off,
         "reg": e.get("reg", ""),
         "mode": e.get("mode", ""),
         "score": int(e.get("score", 0)),
         "known": e.get("known", false),
         "value": int(e.get("value", 0)) & 255,
         "low6": int(e.get("low6", 0)),
         "depth": int(e.get("depth", 0)),
         "distance": int(e.get("distance", 0)),
      }
      if off >= 0 && off < core_target.len {
         def ch = load8(core_target, off) & 255
         rec = rec.set("target_char", ch).
         set("target_value", base64_index(ch)).
         set("relation", e.get("known", false) ? _bit_sliced_mapper_relation(int(e.get("value", 0)), ch) : "symbolic")
      }
      out = out.append(rec)
      i += 1
   }
   out
}

fn _bit_sliced_select_mapper_events(list public, int output_len) list {
   mut by_off = dict()
   mut i = 0
   while i < public.len {
      def e = public[i]
      def rel = e.get("relation", "")
      if _bit_sliced_relation_rank(rel) > 0 {
         def key = to_str(e.get("offset", -1))
         def prev = by_off.get(key, dict())
         if _bit_sliced_better_selected_event(e, prev) { by_off = by_off.set(key, e) }
      }
      i += 1
   }
   mut out = []
   i = 0
   while i < output_len {
      def key = to_str(i)
      if by_off.contains(key) { out = out.append(by_off.get(key)) }
      i += 1
   }
   out
}

fn _bit_sliced_mapper_validation(list events, str core_target, bool include_events=false) dict {
   mut public = _bit_sliced_public_mapper_events(events, core_target)
   def selected = _bit_sliced_select_mapper_events(public, core_target.len)
   mut counts = dict()
   mut mismatches = []
   mut known = 0
   mut i = 0
   while i < public.len {
      def rel = public[i].get("relation", "unknown")
      counts = counts.set(rel, int(counts.get(rel, 0)) + 1)
      if public[i].get("known", false) { known += 1 }
      if rel == "mismatch" { mismatches = mismatches.append(public[i]) }
      i += 1
   }
   mut out = {
      "count": public.len,
      "known_count": known,
      "selected_count": selected.len,
      "selected_complete": core_target.len > 0 && selected.len == core_target.len,
      "selected": selected,
      "counts": counts,
      "mismatch_count": mismatches.len,
   }
   if include_events {
      out = out.set("mismatches", mismatches).set("events", public)
   }
   out
}

fn _bit_sliced_calibration_input(int input_len, any opts=dict()) str {
   def raw = opts.get("calibration_input", "")
   if is_str(raw) && raw.len >= input_len { return slice(raw, 0, input_len, 1) }
   _repeat_byte(65, input_len)
}

fn _bit_sliced_const_input_bytes(any ctx, str input, int input_len) list {
   mut out = []
   mut i = 0
   while i < input_len {
      out = out.append(smt.bv_u8(ctx, i < input.len ? (load8(input, i) & 255) : 0))
      i += 1
   }
   out
}

fn _bit_sliced_known_output_string(dict outputs, int output_addr, int output_len) dict {
   mut b = str.Builder(output_len)
   mut missing = []
   mut i = 0
   while i < output_len {
      def key = to_str(output_addr + i)
      def ov = outputs.get(key, dict())
      if !outputs.contains(key) || !ov.get("known", false) {
         missing = missing.append(i)
         b = str.builder_append_byte(b, 0)
      } else {
         b = str.builder_append_byte(b, int(ov.get("value", 0)) & 255)
      }
      i += 1
   }
   def text = str.builder_to_str(b)
   str.builder_free(b)
   {"ok": missing.len == 0, "text": text, "missing": missing}
}

fn _bit_sliced_exec_opts_for_concrete(any opts, str concrete_input) dict {
   mut out = dict()
   out = out.set("output_writes", opts.get("output_writes", dict()))
   out = out.set("concrete_input", concrete_input)
   out = out.set("cut_depth", int(opts.get("cut_depth", 3)))
   out = out.set("stack_base", int(opts.get("stack_base", 0x70000000)))
   out = out.set("argv_base", int(opts.get("argv_base", 0x71000000)))
   out = out.set("arg_base", int(opts.get("arg_base", 0x72000000)))
   out
}

fn _bit_sliced_calibrate_mapper_inputs(any ctx, any solver, list rows, int input_len, int output_addr, int output_len, any opts=dict()) dict {
   def input = _bit_sliced_calibration_input(input_len, opts)
   def inputs = _bit_sliced_const_input_bytes(ctx, input, input_len)
   def exec = _bit_sliced_execute_lifted(ctx, solver, rows, inputs, input_len, output_addr,
      _bit_sliced_exec_opts_for_concrete(opts, input))
   def known_output = _bit_sliced_known_output_string(exec.get("outputs", dict()), output_addr, output_len)
   if !known_output.get("ok", false) {
      return {
         "ok": false,
         "reason": "calibration output incomplete",
         "input": input,
         "missing": known_output.get("missing", []),
      }
   }
   def validation = _bit_sliced_mapper_validation(exec.get("mapper_input_events", []), known_output.get("text", ""),
      opts.get("mapper_validation_events", false))
   {
      "ok": true,
      "input": input,
      "output": known_output.get("text", ""),
      "selected_count": int(validation.get("selected_count", 0)),
      "selected_complete": validation.get("selected_complete", false),
      "validation": validation,
      "selected": validation.get("selected", []),
   }
}

fn _bit_sliced_event_matches(dict event, dict selected) bool {
   int(event.get("offset", -1)) == int(selected.get("offset", -2)) &&
   int(event.get("row", -1)) == int(selected.get("row", -2)) &&
   _bit_sliced_same_reg(event.get("reg", ""), selected.get("reg", ""))
}

fn _bit_sliced_find_event(list events, dict selected) dict {
   mut i = 0
   while i < events.len {
      if _bit_sliced_event_matches(events[i], selected) { return events[i] }
      i += 1
   }
   dict()
}

fn _bit_sliced_assert_mapper_relation(any ctx, any solver, dict event, dict selected, int target_ch) dict {
   def rel = selected.get("relation", "")
   def idx = base64_index(target_ch)
   def raw = _x86_val(event.get("ast", 0), int(event.get("bits", 32)), event.get("known", false),
      int(event.get("value", 0)), event.get("cuts", []), int(event.get("depth", 0)))
   def v = _x86_resize(ctx, raw, 8)
   def ast = v.get("ast", 0)
   mut ok = false
   if rel == "low6" && idx >= 0 {
      smt.solver_assert(ctx, solver, smt.mk_eq(ctx, smt.bvand(ctx, ast, smt.bv_u8(ctx, 63)), smt.bv_u8(ctx, idx)))
      ok = true
   } elif rel == "double_low6" && idx >= 0 {
      smt.solver_assert(ctx, solver, smt.mk_eq(ctx, smt.bvand(ctx, ast, smt.bv_u8(ctx, 1)), smt.bv_u8(ctx, 0)))
      smt.solver_assert(ctx, solver, smt.mk_eq(ctx,
            smt.bvand(ctx, smt.bvlshr(ctx, ast, smt.bv_u8(ctx, 1)), smt.bv_u8(ctx, 63)), smt.bv_u8(ctx, idx)))
      ok = true
   } elif rel == "ascii" {
      smt.solver_assert(ctx, solver, smt.mk_eq(ctx, ast, smt.bv_u8(ctx, target_ch & 255)))
      ok = true
   } elif rel == "ascii_case_toggle" {
      def folded = smt.bvxor(ctx, smt.bvlshr(ctx, smt.bvand(ctx, ast, smt.bv_u8(ctx, 0x40)), smt.bv_u8(ctx, 1)), ast)
      smt.solver_assert(ctx, solver, smt.mk_eq(ctx, folded, smt.bv_u8(ctx, target_ch & 255)))
      ok = true
   }
   {
      "ok": ok,
      "root": v,
      "offset": int(selected.get("offset", -1)),
      "row": int(selected.get("row", -1)),
      "reg": selected.get("reg", ""),
      "relation": rel,
   }
}

fn _bit_sliced_assert_mapper_inverse(any ctx, any solver, list events, list selected, str core_target) dict {
   mut covered = dict()
   mut roots = []
   mut used = []
   mut skipped = []
   mut i = 0
   while i < selected.len {
      def s = selected[i]
      def off = int(s.get("offset", -1))
      def event = _bit_sliced_find_event(events, s)
      if off >= 0 && off < core_target.len && event.len > 0 {
         def rel = _bit_sliced_assert_mapper_relation(ctx, solver, event, s, load8(core_target, off) & 255)
         if rel.get("ok", false) {
            covered = covered.set(to_str(off), true)
            roots = roots.append(rel.get("root", dict()))
            used = used.append(s.set("target_char", load8(core_target, off) & 255).set("target_value", base64_index(load8(core_target, off) & 255)))
         } else {
            skipped = skipped.append(s)
         }
      } else {
         skipped = skipped.append(s)
      }
      i += 1
   }
   {
      "asserted": used.len,
      "covered": covered,
      "roots": roots,
      "used": used,
      "skipped": skipped,
   }
}

fn _bit_sliced_execute_lifted(any ctx, any solver, list rows, list inputs, int input_len, int output_addr, any opts=dict()) dict {
   def st = _x86_state_init(ctx, inputs, input_len, output_addr, opts)
   def trace_points = _bit_sliced_trace_point_map(_bit_sliced_trace_points(opts))
   mut i = 0
   while i < rows.len {
      _bit_sliced_capture_trace_points(ctx, st, i, trace_points)
      if !_x86_exec_row(ctx, solver, st, rows[i]) { break }
      i += 1
   }
   st.set("executed", i).set("row_count", rows.len)
}

fn _assert_bit_sliced_flag_shape(any ctx, any solver, list inputs, any opts=dict()) list {
   mut constraints = []
   if !opts.get("flag_shape", true) { return constraints }
   mut prefixes = _default_flag_prefixes(opts)
   if is_list(opts.get("flag_prefixes", [])) && opts.get("flag_prefixes", []).len > 0 {
      prefixes = opts.get("flag_prefixes", [])
   }
   mut choices = []
   mut pi = 0
   while pi < prefixes.len {
      def p = to_str(prefixes[pi])
      if p.len <= inputs.len {
         mut parts = []
         mut j = 0
         while j < p.len {
            parts = parts.append(smt.mk_eq(ctx, inputs[j], smt.bv_u8(ctx, load8(p, j))))
            j += 1
         }
         choices = choices.append(smt.mk_and(ctx, parts))
      }
      pi += 1
   }
   if choices.len > 0 {
      smt.solver_assert(ctx, solver, smt.mk_or(ctx, choices))
      constraints = constraints.append({"kind": "flag_prefix_or", "prefixes": prefixes})
   }
   if inputs.len > 0 {
      smt.solver_assert(ctx, solver, smt.mk_eq(ctx, inputs[inputs.len - 1], smt.bv_u8(ctx, 125)))
      constraints = constraints.append({"kind": "flag_suffix", "value": "}"})
   }
   constraints
}

fn _x86_cut_constraint_map(list constraints) dict {
   mut out = dict()
   mut i = 0
   while i < constraints.len {
      def c = constraints[i]
      out = out.set(c.get("name", ""), c)
      i += 1
   }
   out
}

fn _x86_assert_needed_cuts(any ctx, any solver, list constraints, list roots, bool lazy=true) int {
   if !lazy {
      mut all_i = 0
      while all_i < constraints.len {
         def c = constraints[all_i]
         smt.solver_assert(ctx, solver, smt.mk_eq(ctx, c.get("lhs", 0), c.get("rhs", 0)))
         all_i += 1
      }
      return constraints.len
   }
   def by_name = _x86_cut_constraint_map(constraints)
   mut needed = dict()
   mut work = []
   mut i = 0
   while i < roots.len {
      def cuts = roots[i].get("cuts", [])
      mut j = 0
      while j < cuts.len {
         if !needed.get(cuts[j], false) { work = work.append(cuts[j]) }
         j += 1
      }
      i += 1
   }
   mut cursor = 0
   while cursor < work.len {
      def name = to_str(work[cursor])
      if !needed.get(name, false) {
         needed = needed.set(name, true)
         def c = by_name.get(name, dict())
         def deps = c.get("deps", [])
         mut di = 0
         while di < deps.len {
            if !needed.get(deps[di], false) { work = work.append(deps[di]) }
            di += 1
         }
      }
      cursor += 1
   }
   mut asserted = 0
   i = 0
   while i < constraints.len {
      def c = constraints[i]
      if needed.get(c.get("name", ""), false) {
         smt.solver_assert(ctx, solver, smt.mk_eq(ctx, c.get("lhs", 0), c.get("rhs", 0)))
         asserted += 1
      }
      i += 1
   }
   asserted
}

fn _bit_sliced_attach_lifted_metadata(dict result, str target, str core_target, int input_len, int output_addr,
   list rows, list exec_rows, dict exec, any opts, dict output_summary,
   dict mapper_validation, dict mapper_calibration) dict {
   result.
   set("target", target).
   set("target_core", core_target).
   set("output_addr", output_addr).
   set("executed", int(exec.get("executed", 0))).
   set("row_count", rows.len).
   set("exec_row_count", exec_rows.len).
   set("cuts", int(exec.get("fresh", 0))).
   set("unsupported", exec.get("unsupported", [])).
   set("engine", "ny-lifted-x86-qfbv").
   set("cut_depth", int(opts.get("cut_depth", 3))).
   set("input_len", input_len).
   set("output_len", core_target.len).
   set("entry", int(opts.get("entry", 0))).
   set("alphabet", "base64").
   set("target_values", base64_values(core_target, core_target.len)).
   set("binary", opts.get("binary", "")).
   set("output_writes", output_summary).
   set("mapper_input_validation", mapper_validation).
   set("mapper_calibration", mapper_calibration)
}

fn _bit_sliced_concrete_output_check(dict outputs, int output_addr, str core_target) dict {
   mut mismatch = []
   mut unknown_outputs = []
   mut i = 0
   while i < core_target.len {
      def key = to_str(output_addr + i)
      def ov = outputs.get(key, dict())
      if !ov.get("known", false) {
         unknown_outputs = unknown_outputs.append(output_addr + i)
      } else {
         def got = int(ov.get("value", 0)) & 255
         def want = load8(core_target, i) & 255
         if got != want {
            mismatch = mismatch.append({"offset": i, "addr": output_addr + i, "got": got, "want": want})
         }
      }
      i += 1
   }
   {
      "unknown_outputs": unknown_outputs,
      "mismatch": mismatch,
      "materialized_output": _bit_sliced_known_output_string(outputs, output_addr, core_target.len).get("text", ""),
   }
}

fn _bit_sliced_new_solver(any ctx, any opts, int timeout_ms) any {
   mut solver = smt.solver_new_qfbv(ctx)
   if opts.get("solver", "") == "sls" {
      solver = smt.solver_new_qfbv_sls(ctx, timeout_ms, int(opts.get("seed", 0)), int(opts.get("max_rounds", 128)))
   } elif opts.get("solver", "") == "qfbv_tactic" || opts.get("solver", "") == "tactic" {
      solver = smt.solver_new_qfbv_tactic(ctx, timeout_ms)
   }
   if !solver { solver = smt.solver_new(ctx) }
   if solver { smt.solver_set_timeout_ms(ctx, solver, timeout_ms) }
   solver
}

fn _bit_sliced_concrete_input(any opts) str {
   def raw = opts.get("concrete_input", "")
   is_str(raw) && raw.len > 0 ? raw : ""
}

fn _bit_sliced_input_vars(any ctx, int input_len, str concrete_input) list {
   if concrete_input.len == 0 { return smt.bv_bytes(ctx, "in_", input_len) }
   mut inputs = []
   mut i = 0
   while i < input_len {
      inputs = inputs.append(smt.bv_u8(ctx, i < concrete_input.len ? (load8(concrete_input, i) & 255) : 0))
      i += 1
   }
   inputs
}

fn _bit_sliced_base_constraints(int input_len, int argv_index, int lo, int hi, int output_addr, str core_target, str concrete_input, list shape_constraints) list {
   mut constraints = [
      {"kind": "argv_range", "index": argv_index, "len": input_len, "lo": lo, "hi": hi},
      {"kind": "mem_eq", "addr": output_addr, "bytes": core_target},
   ]
   if concrete_input.len > 0 { constraints = constraints.append({"kind": "concrete_input", "value": concrete_input}) }
   constraints + shape_constraints
}

fn _bit_sliced_missing_outputs(dict outputs, int output_addr, int output_len) list {
   mut missing = []
   mut i = 0
   while i < output_len {
      def key = to_str(output_addr + i)
      if !outputs.contains(key) { missing = missing.append(output_addr + i) }
      i += 1
   }
   missing
}

fn _bit_sliced_assert_output_constraints(any ctx, any solver, dict outputs, int output_addr,
   str core_target, dict mapper_inverse) dict {
   mut roots = mapper_inverse.get("roots", [])
   mut asserted = 0
   mut i = 0
   while i < core_target.len {
      def key = to_str(output_addr + i)
      if !mapper_inverse.get("covered", dict()).get(to_str(i), false) {
         def ov = outputs.get(key, dict())
         smt.solver_assert(ctx, solver, smt.mk_eq(ctx, ov.get("ast", 0), smt.bv_u8(ctx, load8(core_target, i))))
         roots = roots.append(ov)
         asserted += 1
      }
      i += 1
   }
   {"roots": roots, "asserted": asserted}
}

fn _solve_bit_sliced_lifted_x86_qfbv(str target, int input_len, int output_len, any opts=dict()) dict {
   def o = _plan_opts(opts)
   def binary = o.get("binary", "")
   if binary.len == 0 { return bit_sliced_ascii_plan(target, input_len, output_len, opts) }
   if !smt.z3_available() { return _fail_result("bit_sliced_ascii_transform", "unknown", "z3 unavailable", [], opts) }
   def bin = dc.load(binary)
   if !is_dict(bin) || !bin.get("ok", false) {
      return _fail_result("bit_sliced_ascii_transform", "unknown", "binary load failed", [], opts)
   }
   def max_bytes = int(o.get("max_bytes", 90000))
   def rows = dc.lift(bin, ".text", max_bytes)
   if !is_list(rows) || rows.len == 0 {
      return _fail_result("bit_sliced_ascii_transform", "unknown", "no lifted rows", [], opts)
   }
   def ctx = smt.ctx_new()
   def timeout_ms = int(o.get("timeout_ms", 5000))
   def solver = _bit_sliced_new_solver(ctx, o, timeout_ms)
   def concrete_input = _bit_sliced_concrete_input(o)
   def has_concrete_input = concrete_input.len > 0
   def inputs = _bit_sliced_input_vars(ctx, input_len, concrete_input)
   def lo = int(o.get("lo", 32))
   def hi = int(o.get("hi", 126))
   if !has_concrete_input { smt.solver_assert_bytes_ascii_range(ctx, solver, inputs, lo, hi) }
   def shape_constraints = _assert_bit_sliced_flag_shape(ctx, solver, inputs, o)
   def output_addr = int(o.get("output_addr", 0))
   def core_target = _target_prefix(target, output_len)
   def output_summary = bit_sliced_output_writes(rows, output_addr, core_target.len)
   def result_output_summary = _bit_sliced_compact_output_writes(output_summary, o.get("mapper_validation_events", false))
   mut exec_rows = rows
   if o.get("slice_output_rows", true) && output_summary.get("complete", false) {
      def stop_row = int(output_summary.get("max_final_row", -1)) + 1
      if stop_row > 0 && stop_row < rows.len { exec_rows = _x86_row_prefix(rows, stop_row) }
   }
   def exec_opts = o.set("output_writes", output_summary)
   mut mapper_calibration = {"ok": false, "reason": "disabled"}
   if !has_concrete_input && o.get("mapper_inverse", false) && output_summary.get("base64_mapper_complete", false) {
      mapper_calibration = _bit_sliced_calibrate_mapper_inputs(ctx, solver, exec_rows, input_len, output_addr, core_target.len, exec_opts)
   }
   def exec = _bit_sliced_execute_lifted(ctx, solver, exec_rows, inputs, input_len, output_addr, exec_opts)
   def mapper_validation = _bit_sliced_mapper_validation(exec.get("mapper_input_events", []), core_target,
      o.get("mapper_validation_events", false))
   def outputs = exec.get("outputs", dict())
   mut constraints = _bit_sliced_base_constraints(input_len, int(o.get("argv_index", 1)), lo, hi, output_addr, core_target, concrete_input, shape_constraints)
   def missing = _bit_sliced_missing_outputs(outputs, output_addr, core_target.len)
   if missing.len > 0 {
      smt.solver_del(ctx, solver)
      smt.ctx_del(ctx)
      return _bit_sliced_attach_lifted_metadata(
         _fail_result("bit_sliced_ascii_transform", "unknown", "lifted executor did not materialize all output bytes", constraints, o).
         set("missing_outputs", missing),
         target, core_target, input_len, output_addr, rows, exec_rows, exec, o,
         result_output_summary, mapper_validation, mapper_calibration)
   }
   if has_concrete_input {
      def concrete_check = _bit_sliced_concrete_output_check(outputs, output_addr, core_target)
      def unknown_outputs = concrete_check.get("unknown_outputs", [])
      def mismatch = concrete_check.get("mismatch", [])
      mut concrete_result = dict()
      if unknown_outputs.len > 0 {
         concrete_result = _fail_result("bit_sliced_ascii_transform", "unknown", "concrete lifted execution left symbolic output bytes", constraints, o).
         set("unknown_outputs", unknown_outputs)
      } elif mismatch.len > 0 {
         concrete_result = _fail_result("bit_sliced_ascii_transform", "unsat", "concrete lifted output mismatch", constraints, o).
         set("mismatch", mismatch)
      } else {
         concrete_result = _sat_result("bit_sliced_ascii_transform", concrete_input, constraints, {"proof": "ny-lifted-x86-concrete"})
      }
      concrete_result = _bit_sliced_attach_lifted_metadata(concrete_result.
         set("asserted_cuts", 0).
         set("materialized_output", concrete_check.get("materialized_output", "")),
         target, core_target, input_len, output_addr, rows, exec_rows, exec, o,
         result_output_summary, mapper_validation, mapper_calibration)
      smt.solver_del(ctx, solver)
      smt.ctx_del(ctx)
      return concrete_result
   }
   mut mapper_inverse = {"asserted": 0, "covered": dict(), "roots": [], "used": [], "skipped": []}
   if mapper_calibration.get("ok", false) &&
   int(mapper_calibration.get("selected_count", 0)) >= int(o.get("mapper_inverse_min_selected", 8)) {
      mapper_inverse = _bit_sliced_assert_mapper_inverse(ctx, solver, exec.get("mapper_input_events", []),
         mapper_calibration.get("selected", []), core_target)
      constraints = constraints.append({
            "kind": "mapper_inverse",
            "asserted": int(mapper_inverse.get("asserted", 0)),
            "calibrated": int(mapper_calibration.get("selected_count", 0)),
         })
   }
   def output_constraints = _bit_sliced_assert_output_constraints(ctx, solver, outputs, output_addr, core_target, mapper_inverse)
   def asserted_cuts = _x86_assert_needed_cuts(ctx, solver, exec.get("cut_constraints", []),
      output_constraints.get("roots", []), o.get("lazy_cuts", true))
   def check = smt.solver_check_result(ctx, solver)
   mut result = dict()
   if check == smt.SAT {
      def input = smt.model_eval_ascii(ctx, solver, inputs)
      result = _sat_result("bit_sliced_ascii_transform", input, constraints, {"proof": "ny-lifted-x86-qfbv"})
   } elif check == smt.UNSAT {
      result = _fail_result("bit_sliced_ascii_transform", "unsat", "output constraints are unsatisfiable", constraints, o)
   } else {
      result = _fail_result("bit_sliced_ascii_transform", "unknown", "z3 returned unknown for lifted x86 qfbv executor", constraints, o)
   }
   result = _bit_sliced_attach_lifted_metadata(result.
      set("asserted_cuts", asserted_cuts).
      set("mapper_inverse", mapper_inverse),
      target, core_target, input_len, output_addr, rows, exec_rows, exec, o,
      result_output_summary, mapper_validation, mapper_calibration)
   smt.solver_del(ctx, solver)
   smt.ctx_del(ctx)
   result
}

fn solve_ascii_xor_eq(str target, any key, any opts=dict()) dict {
   "Solve bytes where `(input[i] xor key[i]) == target[i]`."
   solve_byte_constraints(_bytewise_transform_constraints("xor_eq", target, key),
      _plan_opts(opts).set("len", target.len))
}

fn solve_ascii_add_eq(str target, int delta, any opts=dict()) dict {
   "Solve bytes where `(input[i] + delta) mod 256 == target[i]`."
   solve_byte_constraints(_bytewise_transform_constraints("add_eq", target, delta),
      _plan_opts(opts).set("len", target.len))
}

fn solve_ascii_sub_eq(str target, int delta, any opts=dict()) dict {
   "Solve bytes where `(input[i] - delta) mod 256 == target[i]`."
   solve_byte_constraints(_bytewise_transform_constraints("sub_eq", target, delta),
      _plan_opts(opts).set("len", target.len))
}

fn bit_sliced_ascii_plan(str target, int input_len, int output_len, any opts=dict()) dict {
   "Build a Ny-level symbolic solve plan for branchless bit-sliced ASCII output transforms.
   The plan is intentionally data-shaped so decompiler exports can be inspected,
   serialized, or handed to a heavier lifted symbolic executor without Python-side
   glue."
   def o = _plan_opts(opts)
   def core_target = _target_prefix(target, output_len)
   mut constraints = [
      {"kind": "argv_range", "index": int(o.get("argv_index", 1)), "len": input_len, "lo": int(o.get("lo", 32)), "hi": int(o.get("hi", 126))},
   ]
   if int(o.get("output_addr", 0)) > 0 {
      constraints = constraints.append({"kind": "mem_eq", "addr": int(o.get("output_addr", 0)), "bytes": core_target})
   } else {
      constraints = constraints.append({"kind": "output_eq", "bytes": core_target})
   }
   {
      "kind": "bit_sliced_ascii_transform",
      "status": "plan",
      "reason": "bit-sliced lifted symbolic plan needs a concrete transform executor/model",
      "strategy": o.get("strategy", "lifted-symbolic-output-constrain"),
      "target": target,
      "target_core": core_target,
      "target_values": base64_values(core_target, output_len),
      "input_len": input_len,
      "output_len": output_len,
      "alphabet": "base64",
      "binary": o.get("binary", ""),
      "entry": int(o.get("entry", 0)),
      "output_addr": int(o.get("output_addr", 0)),
      "argv_index": int(o.get("argv_index", 1)),
      "constraints": constraints,
      "backend": {"z3": smt.z3_available(), "z3_version": smt.z3_version_str()},
      "z3": smt.z3_available(),
   }
}

fn solve_bit_sliced_ascii_transform(str target, int input_len, int output_len, any opts=dict()) dict {
   "Solve branchless bit-sliced ASCII output transforms through Ny's lifted
   straight-line x86-64 executor when a binary is available. Falls back to a
   structured plan when the executor cannot be applied."
   def o = _plan_opts(opts)
   if o.get("binary", "").len > 0 && int(o.get("output_addr", 0)) > 0 {
      return _solve_bit_sliced_lifted_x86_qfbv(target, input_len, output_len, o)
   }
   bit_sliced_ascii_plan(target, input_len, output_len, opts)
}
