;; Keywords: factorization classical helpers dict report
;; References: std.math.crypto.factorization.classical
module std.math.crypto.factorization.classical.misc(_append_used_defaults, _dict_add_int, _dict_add_int_fields, _dict_int, _dict_max_int, _dict_max_int_fields, _dict_with, _elapsed_ms, _fields_extend, _finish_factor_status, _finish_report, _finish_report_with, _is_nontrivial_factor, _list_concat, _report, _report_with, _set_factor_status, _set_fields, _z)
use std.math.nt
use std.os (ticks)

fn _z(any x) any { is_bigint(x) ? x : Z(x) }

fn _is_nontrivial_factor(any g, any n) bool {
   def gz, nz = _z(g), _z(n)
   gz > 1 && gz < nz && nz % gz == 0
}

fn _elapsed_ms(any t0) any { float(ticks() - t0) / 1000000.0 }

fn _report(str method, int cap=8) dict {
   mut out = dict(max(8, cap * 2))
   out["method"] = method
   out
}

fn _finish_report(dict out, any t0) dict {
   out.set("elapsed_ms", _elapsed_ms(t0))
}

fn _set_fields(dict out, list fields) dict {
   mut i = 0
   while i < fields.len {
      def field = fields.get(i)
      def key = field.get(0)
      out = out.set(is_str(key) ? key : to_str(key), field.get(1, nil))
      i += 1
   }
   out
}

fn _finish_report_with(dict out, any t0, list fields) dict {
   _finish_report(_set_fields(out, fields), t0)
}

fn _set_factor_status(dict out, any factor, bool success) dict {
   out.set("factor", factor).set("success", success)
}

fn _finish_factor_status(dict out, any t0, any factor, bool success) dict {
   _finish_report_with(out, t0, [["factor", factor], ["success", success]])
}

fn _report_with(str method, any t0, list fields) dict {
   _finish_report_with(_report(method, fields.len + 1), t0, fields)
}

fn _dict_with(int cap, list fields) dict {
   _set_fields(dict(max(8, cap * 2)), fields)
}

fn _fields_extend(list fields, list more) list {
   mut i = 0
   while i < more.len {
      fields = fields.append(more.get(i))
      i += 1
   }
   fields
}

fn _list_concat(list a, list b) list {
   mut out = []
   mut i = 0
   while i < a.len {
      out = out.append(a.get(i))
      i += 1
   }
   i = 0
   while i < b.len {
      out = out.append(b.get(i))
      i += 1
   }
   out
}

fn _dict_add_int(dict out, str key, any value) dict {
   out.set(key, int(out.get(key, 0)) + int(value))
}

fn _dict_int(dict out, str key, int fallback=0) int {
   int(out.get(key, fallback))
}

fn _dict_max_int(dict out, str key, any value, int fallback=0) dict {
   def v = int(value)
   v > int(out.get(key, fallback)) ? out.set(key, v) : out
}

fn _dict_add_int_fields(dict out, dict source, list specs) dict {
   mut i = 0
   while i < specs.len {
      def spec = specs.get(i)
      out = _dict_add_int(out, to_str(spec.get(0)), source.get(to_str(spec.get(1)), spec.get(2, 0)))
      i += 1
   }
   out
}

fn _dict_max_int_fields(dict out, dict source, list specs) dict {
   mut i = 0
   while i < specs.len {
      def spec = specs.get(i)
      out = _dict_max_int(out, to_str(spec.get(0)), source.get(to_str(spec.get(1)), spec.get(2, 0)), int(spec.get(3, 0)))
      i += 1
   }
   out
}

fn _append_used_defaults(list fields, any used, list specs) list {
   mut i = 0
   while i < specs.len {
      def spec = specs.get(i)
      fields = fields.append([spec.get(0), used.get(to_str(spec.get(0)), spec.get(1, nil))])
      i += 1
   }
   fields
}
