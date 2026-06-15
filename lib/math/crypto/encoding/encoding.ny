;; Keywords: crypto encoding bytes base conversion math
;; Encoding routines for PEM, DER, ASN.1, and public-key encoding.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc7468
;; References:
;; - std.math.crypto.encoding
;; - std.math.crypto
module std.math.crypto.encoding.encoding(asn1_parse, asn1_decode_length, asn1_to_json, asn1_get_integer, asn1_get_sequence, asn1_integers, pem_decode, hex_encode, hex_decode, base64_encode, base64_decode)
use std.core
use std.math.bin as bin
use std.math.nt
use std.core.str

layout shape Asn1Node derive(load) pack(8){
   int tag = 0,
   int len = 0
}

fn base64_encode(list b) str {
   "Encode bytes list to base64 string."
   b.base64
}

fn base64_decode(str s) list {
   "Decode base64 string to bytes list."
   s.base64_decode
}

fn hex_encode(list b) str {
   "Encode bytes list to hex string."
   b.hex
}

fn hex_decode(str s) list {
   "Decode hex string to bytes list."
   s.unhex
}

fn _asn1_byte(any data, int pos) int {
   is_str(data) ? load8(data, pos) : data[pos]
}

fn asn1_decode_length(any data, int offset) list {
   "Decode ASN.1 length field. Returns [length, bytes_read]."
   def b = _asn1_byte(data, offset)
   if b < 128 { return [b, 1] }
   def n = b & 0x7f
   mut len_val = 0
   mut i = 0
   while i < n {
      len_val = (len_val << 8) | _asn1_byte(data, offset + 1 + i)
      i += 1
   }
   [len_val, 1 + n]
}

fn asn1_parse(list data, int offset=0, any limit=nil) list {
   "Recursively parse DER encoded data. Returns list of {tag, len, val} nodes."
   if limit == nil { limit = data.len }
   mut results = []
   mut p = offset
   while p < limit {
      def tag = data.get(p)
      def len_res = asn1_decode_length(data, p + 1)
      def v_len = len_res.get(0)
      def l_len = len_res.get(1)
      def v_start = p + 1 + l_len
      mut value = nil
      def is_constructed = (tag & 0x20) != 0
      if is_constructed { value = asn1_parse(data, v_start, v_start + v_len) } else { value = slice(data, v_start, v_start + v_len) }
      results = results.append({"tag": tag, "len": v_len, "val": value})
      p = v_start + v_len
   }
   results
}

fn _asn1_append_value(list out, any v, str pad, int indent) list {
   if is_list(v) { return builder_append(out, asn1_to_json(v, indent + 1)) }
   if is_str(v) && v.len > 0 {
      out = builder_append(out, pad)
      out = builder_append(out, "  val=")
      out = builder_append(out, to_str(v))
      out = builder_append(out, "\n")
   }
   out
}

fn _asn1_append_node(list out, *Asn1Node child, any value, str pad, int indent) list {
   out = builder_append(out, pad)
   out = builder_append(out, "tag=0x")
   out = builder_append(out, [child.tag].hex)
   out = builder_append(out, " len=")
   out = builder_append(out, to_str(child.len))
   out = builder_append(out, "\n")
   _asn1_append_value(out, value, pad, indent)
}

fn asn1_to_json(any node, int indent=0) str {
   "Pretty-print ASN.1 parse tree as indented text."
   mut out = Builder(128)
   def pad = "  " * indent
   if is_list(node) {
      mut i = 0
      def node_n = node.len
      while i < node_n {
         def child = node.get(i)
         layout guard Asn1Node child_node = child else {
            i += 1
            continue
         }
         def dict child_dict = child
         out = _asn1_append_node(out, child_node, child_dict.get("val"), pad, indent)
         free(child_node)
         i += 1
      }
   }
   def text = builder_to_str(out)
   builder_free(out)
   text
}

fn asn1_get_integer(dict node) any {
   "Extract BigInt from ASN.1 integer node."
   if node.get("tag") != 0x02 { return nil }
   node.get("val").long
}

fn asn1_get_sequence(dict node) any {
   "Extract sequence content from ASN.1 sequence node."
   if node.get("tag") == 0x30 { return node.get("val") }
   nil
}

fn _asn1_bytes_to_bigint(any data, int start, int stop) bigint {
   mut i = start
   while i < stop && _asn1_byte(data, i) == 0 { i += 1 }
   mut out = Z(0)
   while i < stop {
      out = out * Z(256) + Z(_asn1_byte(data, i))
      i += 1
   }
   out
}

fn _asn1_collect_integers(any data, int start, int stop, list acc) list {
   mut out = acc
   mut pos = start
   while pos + 2 <= stop {
      def tag = _asn1_byte(data, pos)
      def len_res = asn1_decode_length(data, pos + 1)
      def body = pos + 1 + len_res[1]
      def next = body + len_res[0]
      if len_res[0] < 0 || len_res[1] <= 0 || body <= pos || next <= pos || next > stop { return out }
      if tag == 0x02 {
         out = out.append(_asn1_bytes_to_bigint(data, body, next))
      } elif tag == 0x30 || tag == 0x31 || (tag & 0x20) != 0 {
         out = _asn1_collect_integers(data, body, next, out)
      } elif tag == 0x03 && len_res[0] > 1 && _asn1_byte(data, body) == 0 && body + 1 < next && (_asn1_byte(data, body + 1) == 0x30 || _asn1_byte(data, body + 1) == 0x02) {
         out = _asn1_collect_integers(data, body + 1, next, out)
      }
      pos = next
   }
   out
}

fn asn1_integers(any der) list {
   "Return every ASN.1 INTEGER in DER order, recursing through SEQUENCE/SET and common public-key BIT STRING wrappers."
   _asn1_collect_integers(der, 0, der.len, [])
}

fn pem_decode(str s) list {
   "Extract and decode Base64 from PEM string."
   def lines = split(s, "\n")
   mut b64 = Builder(s.len + 8)
   mut recording = false
   mut i = 0
   def lines_n = lines.len
   while i < lines_n {
      def line = strip(lines.get(i))
      if startswith(line, "-----BEGIN") { recording = true }
      elif startswith(line, "-----END") { recording = false }
      elif recording { b64 = builder_append(b64, line) }
      i += 1
   }
   def b64_s = builder_to_str(b64)
   builder_free(b64)
   base64_decode(b64_s)
}
