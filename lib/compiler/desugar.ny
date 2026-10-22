;; Keywords: desugar compound assignment frontend differential ny-desugar
;; The Ny implementation of the compound-assignment desugar pass.
;;
;; The in-parser C desugar (parse/stmt/dispatch.c) is the differential
;; reference.  This
;; library re-implements the same rewrite over the raw parse tree exposed by
;; `__parse_ast_raw`: every `NY_S_AUG` statement is rewritten by span surgery
;; on the source text (`target op= rhs` becomes `target = target op rhs`,
;; `t[i] op= rhs` becomes `set_idx(t, i, t[i] op rhs)`, and `*p op= rhs`
;; becomes `store64_i(p, *p op rhs)`), so the rewritten source keeps the
;; grouping and origins of every untouched token.  The differential harness
;; behind `--ny-desugar=ny|c` proves the two implementations compile to the
;; same program.
module core.compiler.desugar(desugar, desugar_file)
use core
use core.str as str
use core.math.parse.data.json as json
use core.os (file_read, file_write)

;; The JSON decoder returns small integers (notably 0) with a dynamic
;; pointer tag, so numeric dict members must be coerced through their
;; string form instead of a direct int() cast.
fn _int_of(any v) int {
   str.atoi(str.strip(str(v)))
}

fn _desugar_source_max_bytes() int { 1 << 20 }

fn _desugar_output_max_bytes() int { 1 << 20 }

;; Expression kinds that need no parentheses as the right operand of an
;; arithmetic operator.  Everything else (logical, ternary, match, unknown)
;; is parenthesized so the rewritten text keeps the C pass's grouping.
fn _rhs_self_delimiting(str kind) bool {
   kind == "literal" || kind == "ident" || kind == "call" ||
   kind == "memcall" || kind == "index" || kind == "member" ||
   kind == "unary" || kind == "deref" || kind == "list" ||
   kind == "tuple" || kind == "dict" || kind == "fstring" ||
   kind == "quote" || kind == "sizeof" || kind == "comptime" ||
   kind == "exists" || kind == "try_expr" || kind == "splice" ||
   kind == "embed" || kind == "inferred_member"
}

;; `x += a + b` is the one form the C pass reassociates (into `x = x + a + b`),
;; so a right-hand `+` binary under `+=` stays unparenthesized; every other
;; non-self-delimiting operand keeps its grouping via added parentheses.
fn _scan_back_to(str source, int from, str want) int {
   mut int i = from
   while i >= 0 {
      if str.str_slice(source, i, i + 1) == want { return i }
      i -= 1
   }
   -1
}

fn _trim_span(str source, int start, int stop) str {
   if start < 0 || stop > source.len || start >= stop { return "" }
   str.strip(str.str_slice(source, start, stop))
}

fn _rewrite_ident(str source, int target_start, int op_start, str op_char,
   str rhs_text) str {
   def target_text = _trim_span(source, target_start, op_start)
   target_text + " = " + target_text + " " + op_char + " " + rhs_text
}

fn _rewrite_index(str source, int target_start, int op_start, int idx_start,
   str op_char, str rhs_text) str {
   def receiver_close = _scan_back_to(source, op_start, "]")
   mut receiver = ""
   mut index_text = "0"
   if idx_start >= 0 {
      def receiver_open = _scan_back_to(source, idx_start, "[")
      if receiver_open >= 0 {
         receiver = _trim_span(source, target_start, receiver_open)
         index_text = _trim_span(source, idx_start, receiver_close)
      }
   } else if receiver_close >= 0 {
      def open_at = _scan_back_to(source, receiver_close, "[")
      if open_at >= 0 {
         receiver = _trim_span(source, target_start, open_at)
      }
   }
   def full_target = _trim_span(source, target_start, op_start)
   "set_idx(" + receiver + ", " + index_text + ", " + full_target + " " +
   op_char + " " + rhs_text + ")"
}

fn _rewrite_deref(str source, int target_start, int op_start,
   int deref_start, str op_char, str rhs_text) str {
   def pointee = _trim_span(source, deref_start, op_start)
   def full_target = _trim_span(source, target_start, op_start)
   "store64_i(" + pointee + ", " + full_target + " " + op_char + " " +
   rhs_text + ")"
}

fn _rewrite_record(str source, dict rec) Result<str, str> {
   ;; Dynamic dict members must be bound to locals before use: an `any`
   ;; argument evaluated directly at a call site can lose its payload, so
   ;; every offset and label is materialized here as a plain local first.
   def op = str(rec.get("op", ""))
   if op.len < 2 { return err("aug record has no compound operator") }
   def op_char = str.str_slice(op, 0, op.len - 1)
   def offs = rec.get("offs")
   if !is_list(offs) || offs.len < 6 {
      return err("aug record is missing its offset list")
   }
   def span_0 = offs.get(0)
   def span_1 = offs.get(1)
   def span_2 = offs.get(2)
   def span_3 = offs.get(3)
   def span_4 = offs.get(4)
   def span_5 = offs.get(5)
   def target_start = _int_of(span_0)
   def op_start = _int_of(span_1)
   def rhs_start = _int_of(span_2)
   def stmt_end = _int_of(span_3)
   def idx_start = _int_of(span_4)
   def deref_start = _int_of(span_5)
   if !(0 <= target_start && target_start < op_start && op_start <= rhs_start &&
      rhs_start <= stmt_end && stmt_end <= source.len) {
      return err("aug record offsets are out of order")
   }
   def value = rec.get("value")
   def value_kind = is_dict(value) ? str(value.get("type", "")) : ""
   def value_op = is_dict(value) ? str(value.get("op", "")) : ""
   def rhs_text = _trim_span(source, rhs_start, stmt_end)
   if !(_rhs_self_delimiting(value_kind) ||
      (value_kind == "binary" && op_char == "+" && value_op == "+")) {
      rhs_text = "(" + rhs_text + ")"
   }
   def kind = str(rec.get("target_kind", "ident"))
   if kind == "index" {
      return ok(_rewrite_index(source, target_start, op_start, idx_start,
            op_char, rhs_text))
   }
   if kind == "deref" {
      return ok(_rewrite_deref(source, target_start, op_start, deref_start,
            op_char, rhs_text))
   }
   return ok(_rewrite_ident(source, target_start, op_start, op_char,
         rhs_text))
}

fn _collect_aug(any node, dict box, int depth) int {
   if depth > 64 { return -1 }
   mut count = 0
   if is_dict(node) {
      if is_str(node.get("type", nil)) { count = 1 }
      if str(node.get("type", "")) == "aug" {
         ;; Plain `=` records need no rewrite: the ordinary parse already
         ;; handles them, so only compound operators are collected.
         ;; Lists are value-typed: reassign through the reference-typed box
         ;; so appends are visible to the caller (NYAUD1120).
         def aug_op = str(node.get("op", ""))
         if aug_op.len >= 2 {
            box["items"] = get(box, "items", []).append(node)
         }
      }
      for key in node.keys() {
         def child_count = _collect_aug(node.get(key), box, depth + 1)
         if child_count < 0 { return -1 }
         count += child_count
         if count > 65536 { return -1 }
      }
      return count
   } elif is_list(node) {
      for item in node {
         def child_count = _collect_aug(item, box, depth + 1)
         if child_count < 0 { return -1 }
         count += child_count
         if count > 65536 { return -1 }
      }
   }
   count
}

fn desugar(str source) Result<str, str> {
   "Rewrites every compound assignment in `source` into its explicit form;
   returns `ok(rewritten_source)` or `err(reason)`."
   if source.len > _desugar_source_max_bytes() {
      return err("source exceeds the 1 MiB desugar budget")
   }
   def raw = __parse_ast_raw(source)
   if !is_str(raw) || raw.len == 0 {
      return err("raw parse failed")
   }
   def decoded = json.json_try_decode(raw)
   if !is_dict(decoded) || !decoded.get("ok", false) {
      mut reason = "json decode failed"
      if is_dict(decoded) {
         reason = str(decoded.get("error", ""))
      }
      return err(reason)
   }
   mut box = {"items": []}
   if _collect_aug(decoded.get("value"), box, 0) < 0 {
      return err("raw syntax tree exceeds the 65,536-node or 64-container budget")
   }
   mut records = get(box, "items", [])
   if records.len == 0 { return ok(source) }
   mut source_mut = source
   mut i = records.len - 1
   while i >= 0 {
      def rec = records.get(i)
      def start = _int_of(rec.get("target_start", 0))
      def end = _int_of(rec.get("stmt_end", 0))
      match _rewrite_record(source, rec) {
         ok(text) -> {
            def prefix = str._substr(source_mut, 0, start)
            def suffix = str._substr(source_mut, end, source_mut.len)
            def replaced = prefix + text
            source_mut = replaced + suffix
         }
         err(reason) -> { return err(reason) }
      }
      i -= 1
   }
   if source_mut.len > _desugar_output_max_bytes() {
      return err("rewritten source exceeds the output budget")
   }
   ok(source_mut)
}

fn desugar_file(str in_path, str out_path) Result<int, str> {
   "Reads `in_path`, runs `desugar` over its text, and writes the result to
   `out_path` ; returns `ok(bytes_written)` or `err(reason)`."
   match file_read(in_path) {
      ok(source) -> {
         match desugar(source) {
            ok(rewritten) -> {
               match file_write(out_path, rewritten) {
                  ok(count) -> { ok(int(count)) }
                  err(code) -> {
                     err("write failed for " + out_path + ": " + str(code))
                  }
               }
            }
            err(reason) -> { err(reason) }
         }
      }
      err(code) -> { err("read failed: " + str(code)) }
   }
}

fn _freshness_probe() int { return 1 }
